// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "JournalTypes.h"

/**
 * One recent journal event retained in the live tail, stamped with the sequence id (cursor)
 * it was assigned when pushed. Mirrors the event payload the NDJSON writer emits.
 */
struct FLiveTailEvent
{
    /** Monotonic sequence id assigned when this entry was pushed. */
    uint64 Seq = 0;

    /** Wall-clock seconds captured at the producing call site. */
    double Ts = 0.0;

    EJournalDomain Domain = EJournalDomain::None;
    FName Key;
    FName Name;
    EJournalSeverity Severity = EJournalSeverity::Info;
    TArray<TPair<FName, FRecordedValue>> Props;
};

/**
 * The most recent value of one variable series (Key + Tag), stamped with the sequence id of its
 * last change. The tail keeps one entry per series (latest-wins) so a poller sees current values
 * rather than every intermediate sample; this matches the change-point set written to NDJSON.
 */
struct FLiveTailVariable
{
    /** Monotonic sequence id of this series' most recent change. */
    uint64 Seq = 0;

    /** Wall-clock seconds of the most recent change. */
    double Ts = 0.0;

    EJournalDomain Domain = EJournalDomain::None;
    FName Key;
    FName Tag;
    FRecordedValue Value;
};

/**
 * Result of a delta query: every event and changed-variable with Seq > sinceCursor, plus the new
 * high-water cursor to poll from next and a loss signal. Events are returned in ascending Seq
 * (FIFO order); variables are returned in ascending Seq of their last change.
 */
struct FLiveTailDelta
{
    /** Events with Seq > sinceCursor, ascending. */
    TArray<FLiveTailEvent> Events;

    /** Changed variables with Seq > sinceCursor, ascending by last-change Seq. */
    TArray<FLiveTailVariable> Variables;

    /** High-water cursor after this read; pass back as the next sinceCursor. */
    uint64 Cursor = 0;

    /**
     * True when sinceCursor predates an entry that was already evicted, i.e. the caller missed at
     * least one event/variable change between sinceCursor and now. The caller should treat its view
     * as stale and resync rather than trust the partial delta. Never set without genuine loss.
     */
    bool bLostData = false;

    /**
     * Lifetime count of entries this tail has evicted since construction (events + variable series).
     * A coarse health diagnostic; the authoritative per-query loss signal is bLostData.
     */
    int32 DroppedCount = 0;
};

/**
 * Bounded in-memory ring of the most recent journal EVENTS and the last-changed VARIABLES, each
 * stamped with a single monotonically increasing sequence id (the cursor). It is fed in lockstep
 * with the NDJSON writer from the single game-thread consumer (FJournalSession::TryAppendValue /
 * AppendEvent), giving a live "tail" a poller can drain incrementally with QuerySince.
 *
 * Concurrency: game-thread-only, no internal locking — same model as FJournalSession. All
 * mutation flows through the single FJournalRecorder::DrainAndFlush consumer on the game thread;
 * all reads (QuerySince/GetCursor) must also occur on the game thread (RPC handlers are marshaled
 * there). Do not call across threads.
 *
 * Overflow: events use a fixed-capacity FIFO ring (oldest overwritten); variables keep one
 * latest-wins entry per series, evicting the least-recently-changed series when the series cap is
 * exceeded. Updating an existing series is not a loss (intermediate samples are intentionally
 * coalesced); only a genuine eviction raises the watermark behind bLostData.
 */
class PINWRIGHTRECORDER_API FJournalLiveTail
{
public:
    static constexpr int32 DefaultMaxEvents = 256;
    static constexpr int32 DefaultMaxVariables = 256;

    explicit FJournalLiveTail(int32 InMaxEvents = DefaultMaxEvents, int32 InMaxVariables = DefaultMaxVariables);

    // --- Producer side (game thread, via DrainAndFlush -> FJournalSession) ---

    /** Append a recent event, assigning it the next sequence id. Oldest event is overwritten when full. */
    void PushEvent(double Ts, EJournalDomain Domain, FName Key, FName Name, EJournalSeverity Severity, const TArray<TPair<FName, FRecordedValue>>& Props);

    /** Record a variable change (latest-wins per series), assigning it the next sequence id. */
    void PushVariable(double Ts, EJournalDomain Domain, FName Key, FName Tag, const FRecordedValue& Value);

    // --- Query side (game thread) ---

    /** Return all events / changed-variables with Seq > SinceCursor, the new high-water cursor, and the loss signal. */
    FLiveTailDelta QuerySince(uint64 SinceCursor) const;

    /** Current high-water cursor (the largest sequence id assigned so far; 0 before the first push). */
    uint64 GetCursor() const { return Cursor; }

    /** Lifetime count of entries evicted since construction (diagnostic). */
    int32 GetDroppedCount() const { return DroppedCount; }

private:
    /** (Key, Tag) identity for a variable series; mirrors FJournalSession's series key. */
    struct FSeriesKey
    {
        FName Key;
        FName Tag;
        bool operator==(const FSeriesKey& Other) const { return Key == Other.Key && Tag == Other.Tag; }
        friend uint32 GetTypeHash(const FSeriesKey& In) { return HashCombine(GetTypeHash(In.Key), GetTypeHash(In.Tag)); }
    };

    /** Bump the eviction watermark and lifetime drop count for one evicted entry. */
    void RecordEviction(uint64 EvictedSeq);

    /** Drop the least-recently-changed variable series when the series cap is exceeded. */
    void EvictOldestVariable();

    int32 MaxEvents;
    int32 MaxVariables;

    /** Last assigned sequence id (monotonic high-water); first push assigns 1. */
    uint64 Cursor = 0;

    /** Largest Seq evicted from either collection; sinceCursor below this means the caller lost data. */
    uint64 HighestEvictedSeq = 0;

    /** Lifetime eviction count across both collections. */
    int32 DroppedCount = 0;

    /** Fixed-capacity FIFO ring of recent events: EventBuf is sized to MaxEvents, valid range tracked by Head/Count. */
    TArray<FLiveTailEvent> EventBuf;
    int32 EventHead = 0;
    int32 EventCount = 0;

    /** Latest value per variable series (Seq = last-change id). Bounded by MaxVariables. */
    TMap<FSeriesKey, FLiveTailVariable> Variables;
};
