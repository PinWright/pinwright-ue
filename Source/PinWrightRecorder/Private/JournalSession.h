// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "JournalTypes.h"
#include "NdjsonSessionWriter.h"
#include "JournalLiveTail.h"

/**
 * Game-thread-owned in-memory store for one recording session plus its NDJSON writer. The
 * single consumer (FJournalRecorder::DrainAndFlush) feeds drained producer messages through
 * the mutators here, so all change-point compression is single-threaded and lock-free. Ported
 * from the Unity recorder's RecorderSession.
 */
class FJournalSession
{
public:
    /**
     * Create the session file under Saved/<RecordingsSubdir>, prune to the newest RetentionCap,
     * write the header, and return the session. Returns nullptr if the file could not be opened.
     */
    static TUniquePtr<FJournalSession> Create(const FString& Label, int32 RetentionCap, const FString& RecordingsSubdir);

    /** Catalog upsert: writes the `obj` line once, on first sight of the key. */
    void UpsertObject(FName Key, const FString& Label, const FString& Path, const FString& Type, FName ParentKey, double Ts);

    /** Per-tag manifest upsert: writes the `var` line once, on first sight of the tag. */
    void UpsertVariable(FName Tag, EJournalKind Kind, double Epsilon, EJournalDomain Domain, double Ts);

    /** Append a change point when the value differs from the last by > epsilon (first always emits). */
    void TryAppendValue(double Ts, EJournalDomain Domain, double DomainTime, int64 DomainFrame, FName Key, FName Tag, const FRecordedValue& Value);

    /** Append a free-form event. */
    void AppendEvent(double Ts, EJournalDomain Domain, FName Key, FName Name, EJournalSeverity Severity, const TArray<TPair<FName, FRecordedValue>>& Props);

    void Flush() { if (Writer) Writer->Flush(); }

    /** Live in-memory tail fed in lockstep with the NDJSON writes; query target for the live-tail RPC. */
    FJournalLiveTail& GetLiveTail() { return LiveTail; }

private:
    /** Per-kind default change threshold: floats/vectors use a small epsilon; int/bool/enum/string compare exactly. */
    static double DefaultEpsilon(EJournalKind Kind);

    TUniquePtr<FNdjsonSessionWriter> Writer;

    TSet<FName> KnownObjects;
    TMap<FName, FVariableManifest> Variables;

    struct FSeriesKey
    {
        FName Key;
        FName Tag;
        bool operator==(const FSeriesKey& Other) const { return Key == Other.Key && Tag == Other.Tag; }
        friend uint32 GetTypeHash(const FSeriesKey& In) { return HashCombine(GetTypeHash(In.Key), GetTypeHash(In.Tag)); }
    };
    TMap<FSeriesKey, FRecordedValue> LastBySeries;

    int32 NextEventId = 0;

    FJournalLiveTail LiveTail;
};
