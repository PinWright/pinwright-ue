// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "RecorderSegments.h"

namespace RecorderSegments
{

namespace
{

// Matches an event by exact name, plus an optional string-prop discriminator for
// single-name boundary pairs (editor:session action=open/exit).
bool MatchesEvent(const FRecorderEventMatcher& M, const FEventRecord& E)
{
    if (!E.Name.Equals(M.Name, ESearchCase::CaseSensitive))
    {
        return false;
    }
    if (M.PropName.IsEmpty())
    {
        return true;
    }
    FString Value;
    return E.Props.IsValid() && E.Props->TryGetStringField(M.PropName, Value) && Value == M.PropValue;
}

bool MatchesAny(const TArray<FRecorderEventMatcher>& Matchers, const FEventRecord& E)
{
    for (const FRecorderEventMatcher& M : Matchers)
    {
        if (MatchesEvent(M, E))
        {
            return true;
        }
    }
    return false;
}

void AddAnchor(FSegment& Seg, const FEventRecord& E)
{
    FSegmentAnchor A;
    A.EventId = E.EventId;
    A.Ts = E.Ts;
    A.Name = E.Name;
    A.Key = E.Key;
    Seg.AnchorEvents.Add(A);
}

} // namespace

TArray<FSegment> PairSegments(const FSessionModel& Session, const TArray<FRecorderSegmentRule>& Rules)
{
    // Sort event pointers by timestamp once; every rule scans the same view.
    TArray<const FEventRecord*> Ordered;
    Ordered.Reserve(Session.Events.Num());
    for (const FEventRecord& E : Session.Events)
    {
        Ordered.Add(&E);
    }
    // StableSort: equal-Ts events keep file (emission) order, so same-frame
    // start/end pairs are not swapped into bogus half-open + degenerate pairs.
    Ordered.StableSort([](const FEventRecord& A, const FEventRecord& B)
    {
        return A.Ts < B.Ts;
    });

    const double SessionMin = Session.HasTimeRange() ? Session.MinTs : 0.0;
    const double SessionMax = Session.HasTimeRange() ? Session.MaxTs : 0.0;

    TArray<FSegment> Out;
    for (const FRecorderSegmentRule& Rule : Rules)
    {
        // Open segments keyed by correlation value; rules without a correlation
        // prop key everything under "" (one open slot).
        TMap<FString, FSegment> Open;
        int32 NextIndex = 1;

        auto CorrelationOf = [&Rule](const FEventRecord& E) -> FString
        {
            FString Value;
            if (!Rule.CorrelationProp.IsEmpty() && E.Props.IsValid())
            {
                E.Props->TryGetStringField(Rule.CorrelationProp, Value);
            }
            return Value;
        };

        for (const FEventRecord* EventPtr : Ordered)
        {
            const FEventRecord& E = *EventPtr;
            const bool bIsStart = MatchesAny(Rule.Starts, E);
            const bool bIsEnd = !bIsStart && MatchesAny(Rule.Ends, E);
            const bool bIsAnchor = !bIsStart && !bIsEnd && MatchesAny(Rule.Anchors, E);
            if (!bIsStart && !bIsEnd && !bIsAnchor)
            {
                continue;
            }

            const FString Corr = CorrelationOf(E);
            FSegment* OpenSeg = Open.Find(Corr);

            if (bIsStart)
            {
                if (OpenSeg)
                {
                    if (Rule.bIgnoreStartWhileOpen)
                    {
                        AddAnchor(*OpenSeg, E);
                        continue;
                    }
                    // Restart: the prior segment never saw its end; close it
                    // half-open at the new start.
                    OpenSeg->TMax = E.Ts;
                    OpenSeg->bEndOpen = true;
                    Out.Add(MoveTemp(*OpenSeg));
                    Open.Remove(Corr);
                }
                FSegment Seg;
                Seg.Type = Rule.Type;
                Seg.Index = NextIndex++;
                Seg.TMin = E.Ts;
                Seg.TMax = E.Ts;
                Seg.Props = E.Props;
                Open.Add(Corr, MoveTemp(Seg));
            }
            else if (bIsEnd)
            {
                if (OpenSeg)
                {
                    OpenSeg->TMax = E.Ts;
                    Out.Add(MoveTemp(*OpenSeg));
                    Open.Remove(Corr);
                }
                else
                {
                    // End without a start: degrade to a half-open segment
                    // anchored at the session's start.
                    FSegment Seg;
                    Seg.Type = Rule.Type;
                    Seg.Index = NextIndex++;
                    Seg.TMin = SessionMin;
                    Seg.TMax = E.Ts;
                    Seg.bStartOpen = true;
                    Seg.Props = E.Props;
                    Out.Add(MoveTemp(Seg));
                }
            }
            else if (OpenSeg)
            {
                AddAnchor(*OpenSeg, E);
            }
        }

        // EOF: still-open segments close at the session's end.
        for (TPair<FString, FSegment>& Entry : Open)
        {
            Entry.Value.TMax = SessionMax;
            Entry.Value.bEndOpen = true;
            Out.Add(MoveTemp(Entry.Value));
        }
    }

    Out.StableSort([](const FSegment& A, const FSegment& B)
    {
        return A.TMin < B.TMin;
    });
    return Out;
}

} // namespace RecorderSegments
