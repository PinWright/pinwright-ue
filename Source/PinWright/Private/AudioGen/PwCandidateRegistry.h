// Copyright (c) 2026 Alexander Penkin. MIT License.

// In-memory store of rendered synth candidates, owned by FPluginState and shaped after
// FJobRegistry (one FCriticalSection, no call-outs while the lock is held).
//
// Why this is not a plain TMap: a candidate carries its rendered audio, and one second of
// 48 kHz stereo float is ~384 KB. An unbounded map of them is a memory leak with extra
// steps, so the registry is bounded by total bytes AND by count, and reclaims the
// least-recently-TOUCHED candidate first - touched meaning read as well as written, so a
// candidate the agent keeps comparing against is not evicted out from under it.
//
// Why eviction is observable: an agent that asks for a candidate it created three calls ago
// and gets a bare "not found" concludes it passed a bad id and retries the wrong fix. Every
// removal leaves a record, so a lookup answers "evicted"/"discarded" with the reason and the
// live budget instead (rule §1 / §7 in docs/rpc-design.md).
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "Misc/DateTime.h"
#include "Misc/Optional.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwSynthRecipe.h"

// One rendered attempt: the recipe that produced it, its audio, and whatever artifacts have
// been derived from it so far. Analysis/ExportedAssetPath/ImagePaths are filled in after the
// render by the analysis and export verbs, through the registry's setters (which also refresh
// the LRU position, because writing a candidate is using it).
struct FPwCandidate
{
    // Assigned by the registry in Add(); whatever the caller set is overwritten.
    FString                 CandidateId;
    FPwSynthRecipe          Recipe;
    FPwAudioBuffer          Buffer;
    TSharedPtr<FJsonObject> Analysis;      // null until analyzed
    // Empty until the candidate is exported. Audio never lands on disk as a file: export
    // creates a USoundWave asset, so this is an asset path, not a filename.
    FString                 ExportedAssetPath;
    // Analyzer output genuinely is files - waveform / spectrogram PNGs on disk.
    TArray<FString>         ImagePaths;
    // Assigned by the registry in Add().
    FDateTime               CreatedAt;
    // Assigned by the registry in Add(); see FPwCandidateRegistry::ComputeApproxBytes.
    int64                   ApproxBytes = 0;
};

// Why a candidate is no longer resident. Kept distinct because the caller's next move
// differs: an evicted candidate means re-render (and probably raise the budget), a
// discarded one means the caller asked for exactly this.
enum class EPwCandidateRemoval : uint8
{
    ByteBudget,   // reclaimed to stay under MaxBytes
    CountBudget,  // reclaimed to stay under MaxCandidates
    Discarded,    // audio.synth.discard with an explicit id
    DiscardedAll, // audio.synth.discard { all: true }
};

struct FPwCandidateRemovalRecord
{
    FString             CandidateId;
    EPwCandidateRemoval Reason = EPwCandidateRemoval::ByteBudget;
    FDateTime           RemovedAt;
    int64               ApproxBytes = 0;  // what it had been costing while resident
};

// Outcome of resolving a candidate id. Zero is a failure, so a default-constructed result
// can never be mistaken for a hit (rule §2).
enum class EPwCandidateLookup : uint8
{
    NotQueried = 0, // default-constructed; nothing was asked
    RegistryEmpty,  // the registry holds nothing at all - "no candidates exist", not "bad id"
    Evicted,        // this registry issued the id and reclaimed it under budget
    Discarded,      // this registry issued the id and the caller discarded it
    NeverExisted,   // no record of this id, live or removed
    Found,
};

struct FPwCandidateLookupResult
{
    EPwCandidateLookup Status = EPwCandidateLookup::NotQueried;
    // Set only when Status == Found. Shared rather than copied: a candidate is megabytes of
    // samples, and the shared handle also keeps it alive for the caller if another thread
    // evicts it a moment later.
    TSharedPtr<const FPwCandidate> Candidate;
    // Set for Evicted / Discarded - the fact that makes a miss diagnosable.
    TOptional<FPwCandidateRemovalRecord> Removal;

    bool IsHit() const { return Status == EPwCandidateLookup::Found && Candidate.IsValid(); }
};

// One row of audio.synth.list_candidates. Deliberately not the recipe: a full recipe is
// hundreds of characters and a dozen of them spill the 10,000-char response threshold, so
// the row carries a digest and the caller fetches the recipe for the one it picked.
struct FPwCandidateSummary
{
    FString   CandidateId;
    FDateTime CreatedAt;
    double    DurationSeconds = 0.0;
    int32     SampleRate = 0;
    int64     ApproxBytes = 0;
    bool      bHasAnalysis = false;
    bool      bExported = false;
    FString   ExportedAssetPath;   // empty unless bExported
    int32     NumImages = 0;
    FString   RecipeDigest;
};

// Live budget state. Reported beside every listing and every miss so the agent can see
// eviction coming instead of discovering it from a vanished id.
struct FPwCandidateUsage
{
    int32 NumCandidates = 0;
    int64 ApproxBytes = 0;
    int64 MaxBytes = 0;
    int32 MaxCandidates = 0;
    int64 BytesRemaining = 0;    // clamped at 0
    // True when the resident set is still over budget after eviction - i.e. the single
    // surviving candidate is bigger than the whole budget. Reported rather than repaired:
    // dropping the render the caller just asked for would leave them with nothing.
    bool  bOverBudget = false;
    int64 NumAdded = 0;          // session totals, so a caller can see churn
    int64 NumEvicted = 0;
    int64 NumDiscarded = 0;
};

class PINWRIGHT_API FPwCandidateRegistry
{
public:
    // 256 MB is ~11 minutes of 48 kHz stereo float - far more iteration history than a
    // session needs, and well inside an editor's headroom. 64 keeps a listing page bounded.
    // Both are constructor arguments so a test can drive eviction with a two-buffer budget
    // instead of allocating 256 MB.
    static constexpr int64 DefaultMaxBytes = 256LL * 1024LL * 1024LL;
    static constexpr int32 DefaultMaxCandidates = 64;
    // Removal records outlive their candidate so a later lookup can still answer "evicted"
    // rather than "never existed". 512 records is a few tens of KB and covers a session
    // eight times longer than the live cap; past that the oldest record is dropped and its
    // id degrades to NeverExisted, which is then the only honest answer left.
    static constexpr int32 MaxRemovalRecords = 512;

    explicit FPwCandidateRegistry(int64 InMaxBytes = DefaultMaxBytes,
                                  int32 InMaxCandidates = DefaultMaxCandidates);

    // Takes ownership of the candidate, assigns its id / CreatedAt / ApproxBytes, and
    // evicts under budget. Returns the assigned id. The freshly added candidate is the
    // most-recently-touched one, so it is never the thing this call evicts.
    FString Add(FPwCandidate&& InCandidate);

    // Resolves an id and marks it most-recently-used. NOT const: a read is a touch, which
    // is exactly what keeps a candidate the agent keeps comparing against alive.
    FPwCandidateLookupResult Get(const FString& CandidateId);

    // Artifact attachment. Each returns the pre-call status (Found == the write landed) and
    // touches the candidate, because writing to one is using it.
    EPwCandidateLookup SetAnalysis(const FString& CandidateId, const TSharedPtr<FJsonObject>& Analysis);
    EPwCandidateLookup SetExportedAssetPath(const FString& CandidateId, const FString& AssetPath);
    EPwCandidateLookup AddImagePath(const FString& CandidateId, const FString& ImagePath);

    // Removes one candidate. The return value is the status the id had BEFORE the call, so
    // Found means "was resident, now discarded" and Discarded means "a previous discard
    // already removed it" - the retry converged rather than failed (rule §8). Map it for the
    // wire with LexDiscardOutcome.
    EPwCandidateLookup Discard(const FString& CandidateId);

    // Removes everything; returns how many candidates were actually resident. Zero is a
    // truthful answer, not a failure - discard-all has to be idempotent.
    int32 DiscardAll();

    // Newest-first page of summaries. Listing does NOT touch: an agent enumerating the
    // registry must not thereby reset every candidate's LRU position. Limit <= 0 means
    // "all"; OutTotal is the unpaged count.
    TArray<FPwCandidateSummary> List(int32 Offset, int32 Limit, int32& OutTotal) const;

    FPwCandidateUsage Usage() const;

    // Structured payload for a lookup that did not hit: the reason, when the candidate went
    // away, and the live budget. One builder so every verb that resolves a candidate id
    // reports the same recovery information (rule §7), and so the recovery survives the
    // oversize-spill rewrite by living in the structured result rather than in prose.
    TSharedPtr<FJsonObject> BuildMissPayload(const FString& CandidateId,
                                             const FPwCandidateLookupResult& Miss) const;
    // Human-readable half of the same miss. Never says "not found" for something that
    // existed.
    static FString MakeMissMessage(const FString& CandidateId,
                                   const FPwCandidateLookupResult& Miss);
    // The error code a verb emits for a candidate-id miss, paired with BuildMissPayload here
    // so the code and its recovery information cannot drift apart across the verbs that
    // resolve candidate ids. The code is the FIRST thing error handling branches on, so the
    // three outcomes whose remedies diverge get three codes:
    //   Evicted       -> CANDIDATE_EVICTED  (the id was real; re-render, mind the budget)
    //   RegistryEmpty -> NO_CANDIDATES      (nothing has been generated yet; generate first)
    //   NeverExisted  -> CANDIDATE_NOT_FOUND(the id is wrong; check where it came from)
    // Discarded also maps to CANDIDATE_NOT_FOUND: "you discarded it yourself" is a
    // caller-side fact whose remedy matches a wrong id, and the payload's `status` field
    // ("discarded", with the removal record) is what separates the two for a caller that
    // wants the detail.
    static const TCHAR* MissErrorCode(EPwCandidateLookup Status);

    static TSharedPtr<FJsonObject> UsageToJson(const FPwCandidateUsage& In);

    // Wire spellings. Single writer per vocabulary so a handler and a test cannot disagree.
    static const TCHAR* LexLookupStatus(EPwCandidateLookup Status);
    static const TCHAR* LexRemovalReason(EPwCandidateRemoval Reason);
    // Same enum, discard's point of view: Found -> "discarded", Discarded ->
    // "already_discarded", Evicted -> "already_evicted".
    static const TCHAR* LexDiscardOutcome(EPwCandidateLookup PriorStatus);

    // Short fingerprint of a recipe for listings. Derived from SerializeSynthRecipe's
    // output rather than the struct's fields, so it depends only on the serializer's
    // published contract; scalars that the serializer did not publish are omitted rather
    // than emitted as zeros that would read as measurements (rule §4).
    static FString RecipeDigest(const FPwSynthRecipe& Recipe);

    // Sample storage only - the term that matters. One second of 48 kHz stereo float is
    // ~384 KB, while the recipe, the analysis JSON and the paths together are a few KB.
    // Counted from Num() rather than GetAllocatedSize() so the figure is reproducible.
    static int64 ComputeApproxBytes(const FPwAudioBuffer& Buffer);

private:
    struct FSlot
    {
        TSharedPtr<FPwCandidate> Candidate;
        // Monotonic touch counter, not a timestamp: LRU order must be drivable explicitly
        // (and testable) without depending on clock resolution or wall-clock sleeps.
        uint64 TouchSeq = 0;
        // The id's counter value. Listing orders on this rather than on CreatedAt, whose
        // ticks can tie between two renders issued in the same instant.
        int32 CreationIndex = 0;
    };

    // All *_Locked helpers assume Mutex is already held.
    FSlot* FindAndTouch_Locked(const FString& CandidateId);
    void   EvictToBudget_Locked();
    void   RecordRemoval_Locked(const FSlot& Slot, EPwCandidateRemoval Reason);
    FPwCandidateLookupResult ResolveMiss_Locked(const FString& CandidateId) const;
    FString AllocateId_Locked(int32& OutIndex);

    const int64 MaxBytes;
    const int32 MaxCandidates;

    TMap<FString, FSlot> Candidates;
    int64  TotalBytes = 0;
    uint64 NextTouchSeq = 1;
    int32  NextIdIndex = 1;

    // Bounded tombstone log: id -> why it went away. RemovalOrder is the FIFO used to drop
    // the oldest record once the log is full.
    TMap<FString, FPwCandidateRemovalRecord> RemovalLog;
    TArray<FString> RemovalOrder;

    int64 NumAddedTotal = 0;
    int64 NumEvictedTotal = 0;
    int64 NumDiscardedTotal = 0;

    mutable FCriticalSection Mutex;
};
