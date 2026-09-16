// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "JournalLiveTail.h"

FJournalLiveTail::FJournalLiveTail(int32 InMaxEvents, int32 InMaxVariables)
    : MaxEvents(FMath::Max(1, InMaxEvents))
    , MaxVariables(FMath::Max(1, InMaxVariables))
{
    // Pre-size the event ring so PushEvent only ever overwrites slots, never reallocates.
    EventBuf.SetNum(MaxEvents);
}

void FJournalLiveTail::RecordEviction(uint64 EvictedSeq)
{
    HighestEvictedSeq = FMath::Max(HighestEvictedSeq, EvictedSeq);
    ++DroppedCount;
}

void FJournalLiveTail::PushEvent(double Ts, EJournalDomain Domain, FName Key, FName Name, EJournalSeverity Severity, const TArray<TPair<FName, FRecordedValue>>& Props)
{
    FLiveTailEvent Entry;
    Entry.Seq = ++Cursor;
    Entry.Ts = Ts;
    Entry.Domain = Domain;
    Entry.Key = Key;
    Entry.Name = Name;
    Entry.Severity = Severity;
    Entry.Props = Props;

    if (EventCount < MaxEvents)
    {
        EventBuf[(EventHead + EventCount) % MaxEvents] = MoveTemp(Entry);
        ++EventCount;
    }
    else
    {
        // Ring full: the slot at Head is the oldest entry — evict it, write the new one, advance Head.
        RecordEviction(EventBuf[EventHead].Seq);
        EventBuf[EventHead] = MoveTemp(Entry);
        EventHead = (EventHead + 1) % MaxEvents;
    }
}

void FJournalLiveTail::PushVariable(double Ts, EJournalDomain Domain, FName Key, FName Tag, const FRecordedValue& Value)
{
    const uint64 Seq = ++Cursor;
    const FSeriesKey SeriesKey{ Key, Tag };

    if (FLiveTailVariable* Existing = Variables.Find(SeriesKey))
    {
        // Latest-wins update: same series, no new slot, so this is a coalesce — not an eviction.
        Existing->Seq = Seq;
        Existing->Ts = Ts;
        Existing->Domain = Domain;
        Existing->Value = Value;
        return;
    }

    if (Variables.Num() >= MaxVariables)
    {
        EvictOldestVariable();
    }

    FLiveTailVariable Entry;
    Entry.Seq = Seq;
    Entry.Ts = Ts;
    Entry.Domain = Domain;
    Entry.Key = Key;
    Entry.Tag = Tag;
    Entry.Value = Value;
    Variables.Add(SeriesKey, MoveTemp(Entry));
}

void FJournalLiveTail::EvictOldestVariable()
{
    // Least-recently-changed series == smallest current Seq. Bounded linear scan (MaxVariables).
    const FSeriesKey* VictimKey = nullptr;
    uint64 MinSeq = MAX_uint64;
    for (const TPair<FSeriesKey, FLiveTailVariable>& Pair : Variables)
    {
        if (Pair.Value.Seq < MinSeq)
        {
            MinSeq = Pair.Value.Seq;
            VictimKey = &Pair.Key;
        }
    }

    if (VictimKey)
    {
        const FSeriesKey Victim = *VictimKey;
        RecordEviction(MinSeq);
        Variables.Remove(Victim);
    }
}

FLiveTailDelta FJournalLiveTail::QuerySince(uint64 SinceCursor) const
{
    FLiveTailDelta Delta;
    Delta.Cursor = Cursor;
    Delta.DroppedCount = DroppedCount;

    // Loss signal: the caller missed entries only if its cursor predates an entry we evicted.
    // (A superseded variable sample is not loss — that seq was coalesced, never evicted.)
    Delta.bLostData = SinceCursor < HighestEvictedSeq;

    // Events: walk the ring from oldest to newest (FIFO == ascending Seq), collect Seq > SinceCursor.
    Delta.Events.Reserve(EventCount);
    for (int32 Index = 0; Index < EventCount; ++Index)
    {
        const FLiveTailEvent& Entry = EventBuf[(EventHead + Index) % MaxEvents];
        if (Entry.Seq > SinceCursor)
        {
            Delta.Events.Add(Entry);
        }
    }

    // Variables: map is unordered, so collect then sort by last-change Seq ascending.
    for (const TPair<FSeriesKey, FLiveTailVariable>& Pair : Variables)
    {
        if (Pair.Value.Seq > SinceCursor)
        {
            Delta.Variables.Add(Pair.Value);
        }
    }
    Delta.Variables.Sort([](const FLiveTailVariable& A, const FLiveTailVariable& B) { return A.Seq < B.Seq; });

    return Delta;
}
