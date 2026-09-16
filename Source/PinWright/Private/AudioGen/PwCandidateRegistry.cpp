// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AudioGen/PwCandidateRegistry.h"

#include "Compat/EngineVersionCompat.h"
#include "Handlers/ErrorCodes.h"
#include "Misc/Guid.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    // Reads the first present numeric field among the camelCase / snake_case spellings.
    // Used only by the digest, which reads back the serializer's own output rather than the
    // recipe struct, so it survives either naming convention.
    bool TryReadRecipeNumber(const TSharedPtr<FJsonObject>& Json, const TCHAR* CamelKey,
                             const TCHAR* SnakeKey, double& Out)
    {
        return Json->TryGetNumberField(CamelKey, Out) || Json->TryGetNumberField(SnakeKey, Out);
    }
}

FPwCandidateRegistry::FPwCandidateRegistry(int64 InMaxBytes, int32 InMaxCandidates)
    // Clamped rather than trusted: a zero or negative budget would make the eviction loop's
    // exit condition meaningless and would evict every candidate the instant it was added.
    : MaxBytes(FMath::Max<int64>(1, InMaxBytes))
    , MaxCandidates(FMath::Max(1, InMaxCandidates))
{
}

int64 FPwCandidateRegistry::ComputeApproxBytes(const FPwAudioBuffer& Buffer)
{
    return static_cast<int64>(Buffer.Left.Num() + Buffer.Right.Num())
         * static_cast<int64>(sizeof(float));
}

FString FPwCandidateRegistry::RecipeDigest(const FPwSynthRecipe& Recipe)
{
    const TSharedPtr<FJsonObject> Json = SerializeSynthRecipe(Recipe);
    if (!Json.IsValid())
    {
        // Not a fabricated digest: a recipe that would not serialize has no fingerprint, and
        // an invented one would compare equal to other unserializable recipes.
        return TEXT("unserializable");
    }

    FString Compact;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Compact);
    FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);

    // Only the scalars the serializer actually published are emitted. An absent field is
    // omitted rather than printed as 0, which would read as a measurement (rule §4).
    TArray<FString> Parts;
    const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
    if (Json->TryGetArrayField(TEXT("layers"), Layers) && Layers != nullptr)
    {
        Parts.Add(FString::Printf(TEXT("%dL"), Layers->Num()));
    }
    double Scalar = 0.0;
    if (TryReadRecipeNumber(Json, TEXT("durationMs"), TEXT("duration_ms"), Scalar))
    {
        Parts.Add(FString::Printf(TEXT("%dms"), static_cast<int32>(Scalar)));
    }
    if (TryReadRecipeNumber(Json, TEXT("sampleRate"), TEXT("sample_rate"), Scalar))
    {
        Parts.Add(FString::Printf(TEXT("%dHz"), static_cast<int32>(Scalar)));
    }
    if (TryReadRecipeNumber(Json, TEXT("seed"), TEXT("randomSeed"), Scalar))
    {
        Parts.Add(FString::Printf(TEXT("seed%lld"), static_cast<int64>(Scalar)));
    }
    // The hash is the half that is always present, so the digest is a usable fingerprint
    // even when none of the scalars above were published.
    Parts.Add(FString::Printf(TEXT("#%08x"), FCrc::StrCrc32(*Compact)));
    return FString::Join(Parts, TEXT(" "));
}

FString FPwCandidateRegistry::AllocateId_Locked(int32& OutIndex)
{
    // Short and readable on purpose: these ids travel into LLM prompts over and over, so a
    // 36-char GUID is a recurring token cost. The counter carries uniqueness; the 4-hex
    // suffix keeps a recreated registry (whose counter restarts at 1) from re-issuing an id
    // the agent is still holding. FGuid::NewGuid draws from the platform GUID source, so it
    // perturbs neither FMath::Rand's nor FMath::SRand's global stream and therefore cannot
    // shift a seeded render - the id is metadata, never an input to the audio.
    for (;;)
    {
        OutIndex = NextIdIndex++;
        const FString Id = FString::Printf(TEXT("c%d_%s"), OutIndex,
            *FGuid::NewGuid().ToString(EGuidFormats::DigitsLower).Left(4));
        // Re-issuing an id that still carries a removal record would make a later lookup
        // report a live candidate as evicted. The counter already rules this out; the check
        // makes it structural rather than an assumption.
        if (!Candidates.Contains(Id) && !RemovalLog.Contains(Id))
        {
            return Id;
        }
    }
}

FString FPwCandidateRegistry::Add(FPwCandidate&& InCandidate)
{
    TSharedPtr<FPwCandidate> Owned = MakeShared<FPwCandidate>(MoveTemp(InCandidate));
    Owned->CreatedAt = FDateTime::UtcNow();
    Owned->ApproxBytes = ComputeApproxBytes(Owned->Buffer);

    FScopeLock Lock(&Mutex);

    FSlot Slot;
    Owned->CandidateId = AllocateId_Locked(Slot.CreationIndex);
    Slot.Candidate = Owned;
    // Highest touch sequence: the candidate that was just rendered is by definition the most
    // recently used one, so this call's own eviction pass can never reclaim it.
    Slot.TouchSeq = NextTouchSeq++;

    const FString Id = Owned->CandidateId;
    TotalBytes += Owned->ApproxBytes;
    ++NumAddedTotal;
    Candidates.Add(Id, MoveTemp(Slot));

    EvictToBudget_Locked();
    return Id;
}

FPwCandidateRegistry::FSlot* FPwCandidateRegistry::FindAndTouch_Locked(const FString& CandidateId)
{
    FSlot* Slot = Candidates.Find(CandidateId);
    if (Slot != nullptr)
    {
        Slot->TouchSeq = NextTouchSeq++;
    }
    return Slot;
}

void FPwCandidateRegistry::EvictToBudget_Locked()
{
    // Never reclaim the last resident candidate: doing so would delete the render the caller
    // just asked for and leave them with nothing to iterate on. When that single candidate is
    // itself larger than the whole budget, the over-budget fact is reported through
    // FPwCandidateUsage::bOverBudget instead of being silently repaired.
    while (Candidates.Num() > 1 && (TotalBytes > MaxBytes || Candidates.Num() > MaxCandidates))
    {
        FString VictimId;
        uint64 OldestSeq = MAX_uint64;
        for (const TPair<FString, FSlot>& Pair : Candidates)
        {
            if (Pair.Value.TouchSeq < OldestSeq)
            {
                OldestSeq = Pair.Value.TouchSeq;
                VictimId = Pair.Key;
            }
        }
        FSlot* Victim = Candidates.Find(VictimId);
        if (Victim == nullptr)
        {
            // Unreachable while Num() > 1; the bail-out keeps the loop terminating rather
            // than spinning on a state that cannot be reduced.
            break;
        }

        // The reason is read off the constraint that is actually violated, so a count-capped
        // eviction is never reported as a memory-pressure one.
        const EPwCandidateRemoval Reason = (TotalBytes > MaxBytes)
            ? EPwCandidateRemoval::ByteBudget
            : EPwCandidateRemoval::CountBudget;
        RecordRemoval_Locked(*Victim, Reason);
        TotalBytes -= Victim->Candidate->ApproxBytes;
        ++NumEvictedTotal;
        Candidates.Remove(VictimId);
    }
}

void FPwCandidateRegistry::RecordRemoval_Locked(const FSlot& Slot, EPwCandidateRemoval Reason)
{
    if (!Slot.Candidate.IsValid())
    {
        return;
    }

    FPwCandidateRemovalRecord Record;
    Record.CandidateId = Slot.Candidate->CandidateId;
    Record.Reason = Reason;
    Record.RemovedAt = FDateTime::UtcNow();
    Record.ApproxBytes = Slot.Candidate->ApproxBytes;

    RemovalLog.Add(Record.CandidateId, Record);
    RemovalOrder.Add(Record.CandidateId);
    while (RemovalOrder.Num() > MaxRemovalRecords)
    {
        const FString Oldest = RemovalOrder[0];
        RemovalOrder.RemoveAt(0, 1, EAllowShrinking::No);
        RemovalLog.Remove(Oldest);
    }
}

FPwCandidateLookupResult FPwCandidateRegistry::ResolveMiss_Locked(const FString& CandidateId) const
{
    FPwCandidateLookupResult Out;

    // Check order is the contract (rule §7). The precise fact wins first: an id we removed
    // ourselves is answered with why and when, never as an unknown id. Only then does the
    // empty-registry case answer, because "nothing exists yet" and "that id is wrong" send
    // the caller to different fixes, and answering the second for the first is how zero gets
    // reported as a small number.
    if (const FPwCandidateRemovalRecord* Record = RemovalLog.Find(CandidateId))
    {
        Out.Status = (Record->Reason == EPwCandidateRemoval::Discarded ||
                      Record->Reason == EPwCandidateRemoval::DiscardedAll)
            ? EPwCandidateLookup::Discarded
            : EPwCandidateLookup::Evicted;
        Out.Removal = *Record;
        return Out;
    }

    Out.Status = (Candidates.Num() == 0)
        ? EPwCandidateLookup::RegistryEmpty
        : EPwCandidateLookup::NeverExisted;
    return Out;
}

FPwCandidateLookupResult FPwCandidateRegistry::Get(const FString& CandidateId)
{
    FScopeLock Lock(&Mutex);
    if (const FSlot* Slot = FindAndTouch_Locked(CandidateId))
    {
        FPwCandidateLookupResult Out;
        Out.Status = EPwCandidateLookup::Found;
        Out.Candidate = Slot->Candidate;
        return Out;
    }
    return ResolveMiss_Locked(CandidateId);
}

EPwCandidateLookup FPwCandidateRegistry::SetAnalysis(const FString& CandidateId,
                                                     const TSharedPtr<FJsonObject>& Analysis)
{
    FScopeLock Lock(&Mutex);
    FSlot* Slot = FindAndTouch_Locked(CandidateId);
    if (Slot == nullptr)
    {
        return ResolveMiss_Locked(CandidateId).Status;
    }
    Slot->Candidate->Analysis = Analysis;
    return EPwCandidateLookup::Found;
}

EPwCandidateLookup FPwCandidateRegistry::SetExportedAssetPath(const FString& CandidateId,
                                                              const FString& AssetPath)
{
    FScopeLock Lock(&Mutex);
    FSlot* Slot = FindAndTouch_Locked(CandidateId);
    if (Slot == nullptr)
    {
        return ResolveMiss_Locked(CandidateId).Status;
    }
    Slot->Candidate->ExportedAssetPath = AssetPath;
    return EPwCandidateLookup::Found;
}

EPwCandidateLookup FPwCandidateRegistry::AddImagePath(const FString& CandidateId,
                                                      const FString& ImagePath)
{
    FScopeLock Lock(&Mutex);
    FSlot* Slot = FindAndTouch_Locked(CandidateId);
    if (Slot == nullptr)
    {
        return ResolveMiss_Locked(CandidateId).Status;
    }
    Slot->Candidate->ImagePaths.AddUnique(ImagePath);
    return EPwCandidateLookup::Found;
}

EPwCandidateLookup FPwCandidateRegistry::Discard(const FString& CandidateId)
{
    FScopeLock Lock(&Mutex);
    // Deliberately not FindAndTouch_Locked: refreshing the LRU position of a candidate that
    // is about to be removed would be bookkeeping about nothing.
    FSlot* Slot = Candidates.Find(CandidateId);
    if (Slot == nullptr)
    {
        return ResolveMiss_Locked(CandidateId).Status;
    }

    RecordRemoval_Locked(*Slot, EPwCandidateRemoval::Discarded);
    TotalBytes -= Slot->Candidate->ApproxBytes;
    ++NumDiscardedTotal;
    Candidates.Remove(CandidateId);
    return EPwCandidateLookup::Found;
}

int32 FPwCandidateRegistry::DiscardAll()
{
    FScopeLock Lock(&Mutex);
    const int32 NumResident = Candidates.Num();
    for (const TPair<FString, FSlot>& Pair : Candidates)
    {
        RecordRemoval_Locked(Pair.Value, EPwCandidateRemoval::DiscardedAll);
    }
    Candidates.Empty();
    TotalBytes = 0;
    NumDiscardedTotal += NumResident;
    return NumResident;
}

TArray<FPwCandidateSummary> FPwCandidateRegistry::List(int32 Offset, int32 Limit,
                                                       int32& OutTotal) const
{
    // Snapshot handles under the lock, then build rows outside it: RecipeDigest serializes a
    // recipe, and no call-out runs while the mutex is held.
    TArray<TPair<int32, TSharedPtr<const FPwCandidate>>> Snapshot;
    {
        FScopeLock Lock(&Mutex);
        Snapshot.Reserve(Candidates.Num());
        for (const TPair<FString, FSlot>& Pair : Candidates)
        {
            Snapshot.Emplace(Pair.Value.CreationIndex, Pair.Value.Candidate);
        }
    }

    // Newest first, ordered on the creation counter. Listing does not touch, so a caller
    // paging through the registry cannot reshuffle it by reading it.
    Snapshot.Sort([](const TPair<int32, TSharedPtr<const FPwCandidate>>& A,
                     const TPair<int32, TSharedPtr<const FPwCandidate>>& B)
    {
        return A.Key > B.Key;
    });

    OutTotal = Snapshot.Num();

    const int32 Start = FMath::Clamp(Offset, 0, Snapshot.Num());
    const int32 End = (Limit > 0) ? FMath::Min(Start + Limit, Snapshot.Num()) : Snapshot.Num();

    TArray<FPwCandidateSummary> Out;
    Out.Reserve(FMath::Max(0, End - Start));
    for (int32 Index = Start; Index < End; ++Index)
    {
        const TSharedPtr<const FPwCandidate>& Candidate = Snapshot[Index].Value;
        if (!Candidate.IsValid())
        {
            continue;
        }
        FPwCandidateSummary Row;
        Row.CandidateId = Candidate->CandidateId;
        Row.CreatedAt = Candidate->CreatedAt;
        // Duration and sample rate come off the rendered buffer, not the recipe: the row
        // describes the artifact that exists, not the one that was requested.
        Row.DurationSeconds = Candidate->Buffer.DurationSeconds();
        Row.SampleRate = Candidate->Buffer.SampleRate;
        Row.ApproxBytes = Candidate->ApproxBytes;
        Row.bHasAnalysis = Candidate->Analysis.IsValid();
        Row.bExported = !Candidate->ExportedAssetPath.IsEmpty();
        Row.ExportedAssetPath = Candidate->ExportedAssetPath;
        Row.NumImages = Candidate->ImagePaths.Num();
        Row.RecipeDigest = RecipeDigest(Candidate->Recipe);
        Out.Add(MoveTemp(Row));
    }
    return Out;
}

FPwCandidateUsage FPwCandidateRegistry::Usage() const
{
    FScopeLock Lock(&Mutex);
    FPwCandidateUsage Out;
    Out.NumCandidates = Candidates.Num();
    Out.ApproxBytes = TotalBytes;
    Out.MaxBytes = MaxBytes;
    Out.MaxCandidates = MaxCandidates;
    Out.BytesRemaining = FMath::Max<int64>(0, MaxBytes - TotalBytes);
    Out.bOverBudget = TotalBytes > MaxBytes;
    Out.NumAdded = NumAddedTotal;
    Out.NumEvicted = NumEvictedTotal;
    Out.NumDiscarded = NumDiscardedTotal;
    return Out;
}

TSharedPtr<FJsonObject> FPwCandidateRegistry::UsageToJson(const FPwCandidateUsage& In)
{
    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetNumberField(TEXT("count"), In.NumCandidates);
    Out->SetNumberField(TEXT("approxBytes"), static_cast<double>(In.ApproxBytes));
    Out->SetNumberField(TEXT("maxBytes"), static_cast<double>(In.MaxBytes));
    Out->SetNumberField(TEXT("maxCandidates"), In.MaxCandidates);
    Out->SetNumberField(TEXT("bytesRemaining"), static_cast<double>(In.BytesRemaining));
    Out->SetBoolField(TEXT("overBudget"), In.bOverBudget);
    Out->SetNumberField(TEXT("addedThisSession"), static_cast<double>(In.NumAdded));
    Out->SetNumberField(TEXT("evictedThisSession"), static_cast<double>(In.NumEvicted));
    Out->SetNumberField(TEXT("discardedThisSession"), static_cast<double>(In.NumDiscarded));
    return Out;
}

const TCHAR* FPwCandidateRegistry::MissErrorCode(EPwCandidateLookup Status)
{
    switch (Status)
    {
    case EPwCandidateLookup::Evicted:       return ErrorCodes::ERR_CANDIDATE_EVICTED;
    case EPwCandidateLookup::RegistryEmpty: return ErrorCodes::ERR_NO_CANDIDATES;
    // Discarded lands here with NeverExisted on purpose: the caller removed it themselves,
    // so the remedy matches a wrong id, and the payload's `status` still separates them.
    default:                                return ErrorCodes::ERR_CANDIDATE_NOT_FOUND;
    }
}

const TCHAR* FPwCandidateRegistry::LexLookupStatus(EPwCandidateLookup Status)
{
    switch (Status)
    {
    case EPwCandidateLookup::Found:         return TEXT("found");
    case EPwCandidateLookup::Evicted:       return TEXT("evicted");
    case EPwCandidateLookup::Discarded:     return TEXT("discarded");
    case EPwCandidateLookup::RegistryEmpty: return TEXT("registry_empty");
    case EPwCandidateLookup::NeverExisted:  return TEXT("never_existed");
    default:                                return TEXT("not_queried");
    }
}

const TCHAR* FPwCandidateRegistry::LexRemovalReason(EPwCandidateRemoval Reason)
{
    switch (Reason)
    {
    case EPwCandidateRemoval::ByteBudget:   return TEXT("byte_budget");
    case EPwCandidateRemoval::CountBudget:  return TEXT("count_budget");
    case EPwCandidateRemoval::Discarded:    return TEXT("discarded");
    case EPwCandidateRemoval::DiscardedAll: return TEXT("discarded_all");
    default:                                return TEXT("unknown");
    }
}

const TCHAR* FPwCandidateRegistry::LexDiscardOutcome(EPwCandidateLookup PriorStatus)
{
    switch (PriorStatus)
    {
    // Found is the pre-call status: the candidate was resident, so this call is what
    // removed it.
    case EPwCandidateLookup::Found:         return TEXT("discarded");
    case EPwCandidateLookup::Discarded:     return TEXT("already_discarded");
    case EPwCandidateLookup::Evicted:       return TEXT("already_evicted");
    case EPwCandidateLookup::RegistryEmpty: return TEXT("registry_empty");
    case EPwCandidateLookup::NeverExisted:  return TEXT("not_found");
    default:                                return TEXT("not_queried");
    }
}

FString FPwCandidateRegistry::MakeMissMessage(const FString& CandidateId,
                                              const FPwCandidateLookupResult& Miss)
{
    switch (Miss.Status)
    {
    case EPwCandidateLookup::Evicted:
        return FString::Printf(
            TEXT("Candidate '%s' existed and was evicted at %s to stay inside the registry ")
            TEXT("budget (%s). The id was valid; the audio is gone."),
            *CandidateId,
            Miss.Removal.IsSet() ? *Miss.Removal->RemovedAt.ToIso8601() : TEXT("an earlier point"),
            Miss.Removal.IsSet() ? LexRemovalReason(Miss.Removal->Reason) : TEXT("unknown reason"));
    case EPwCandidateLookup::Discarded:
        return FString::Printf(
            TEXT("Candidate '%s' existed and was discarded at %s by an earlier ")
            TEXT("audio.synth.discard call. The id was valid; the audio is gone."),
            *CandidateId,
            Miss.Removal.IsSet() ? *Miss.Removal->RemovedAt.ToIso8601() : TEXT("an earlier point"));
    case EPwCandidateLookup::RegistryEmpty:
        return FString::Printf(
            TEXT("No candidates exist in this session, so '%s' resolves to nothing. This is ")
            TEXT("an empty registry, not a bad id."),
            *CandidateId);
    case EPwCandidateLookup::NeverExisted:
        return FString::Printf(
            TEXT("No candidate with id '%s' has been issued in this session."), *CandidateId);
    case EPwCandidateLookup::Found:
        return FString::Printf(TEXT("Candidate '%s' is resident."), *CandidateId);
    default:
        return FString::Printf(TEXT("Candidate '%s' was not looked up."), *CandidateId);
    }
}

TSharedPtr<FJsonObject> FPwCandidateRegistry::BuildMissPayload(
    const FString& CandidateId, const FPwCandidateLookupResult& Miss) const
{
    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("candidateId"), CandidateId);
    // The discriminator an agent should branch on. Without it every miss looks like a typo,
    // and the agent retries the wrong fix.
    Out->SetStringField(TEXT("status"), LexLookupStatus(Miss.Status));

    if (Miss.Removal.IsSet())
    {
        TSharedPtr<FJsonObject> Removal = MakeShared<FJsonObject>();
        Removal->SetStringField(TEXT("reason"), LexRemovalReason(Miss.Removal->Reason));
        Removal->SetStringField(TEXT("removedAt"), Miss.Removal->RemovedAt.ToIso8601());
        Removal->SetNumberField(TEXT("approxBytes"),
            static_cast<double>(Miss.Removal->ApproxBytes));
        Out->SetObjectField(TEXT("removal"), Removal);
    }

    // The budget travels with the miss so the caller can act on it in the same round trip
    // instead of calling list_candidates to find out why its candidate disappeared.
    Out->SetObjectField(TEXT("registry"), UsageToJson(Usage()));

    const TCHAR* Recovery = TEXT("");
    switch (Miss.Status)
    {
    case EPwCandidateLookup::Evicted:
        Recovery = TEXT("Re-render the recipe. To keep more candidates resident, discard the ")
                   TEXT("ones you are done with, or raise the registry budget.");
        break;
    case EPwCandidateLookup::Discarded:
        Recovery = TEXT("A previous audio.synth.discard removed this candidate. Re-render it ")
                   TEXT("if you still need it.");
        break;
    case EPwCandidateLookup::RegistryEmpty:
        Recovery = TEXT("Render a candidate first; nothing has been generated in this session.");
        break;
    case EPwCandidateLookup::NeverExisted:
        Recovery = TEXT("Check the id against audio.synth.list_candidates.");
        break;
    default:
        break;
    }
    if (*Recovery != TEXT('\0'))
    {
        Out->SetStringField(TEXT("recovery"), Recovery);
    }
    return Out;
}
