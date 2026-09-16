// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "RecorderQueryEngine.h"
#include "RecorderAsOf.h"
#include "RecorderReductions.h"
#include "RecorderSegments.h"
#include "RecorderSegmentRule.h"
#include "RecorderEnvelope.h"

using namespace RecorderModel;
using RecorderEnvelope::FEnvelopeMeta;

namespace
{

// Severity ordinal table shared by parse + filter. Order matches the writer's enum.
const TArray<FString>& SeverityNames()
{
    static const TArray<FString> Names = {
        TEXT("Trace"), TEXT("Debug"), TEXT("Info"), TEXT("Warning"), TEXT("Error"), TEXT("Fatal")
    };
    return Names;
}

// Set the kind-appropriate value fields ("value" scalar, "vector" array, or "text")
// on Target for a recorded value. Shared by series points and tag states.
void FillValueFields(const TSharedRef<FJsonObject>& Target, const FValuePoint& V)
{
    switch (V.Kind)
    {
    case EValueKind::String:
        Target->SetStringField(TEXT("text"), V.S);
        break;
    case EValueKind::Enum:
        Target->SetNumberField(TEXT("value"), V.F0);
        Target->SetStringField(TEXT("text"), V.S);
        break;
    case EValueKind::Vec2:
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(V.F0));
        Arr.Add(MakeShared<FJsonValueNumber>(V.F1));
        Target->SetArrayField(TEXT("vector"), Arr);
        break;
    }
    case EValueKind::Vec3:
    case EValueKind::Rotator:
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(V.F0));
        Arr.Add(MakeShared<FJsonValueNumber>(V.F1));
        Arr.Add(MakeShared<FJsonValueNumber>(V.F2));
        Target->SetArrayField(TEXT("vector"), Arr);
        break;
    }
    case EValueKind::Vec4:
    case EValueKind::Quat:
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(V.F0));
        Arr.Add(MakeShared<FJsonValueNumber>(V.F1));
        Arr.Add(MakeShared<FJsonValueNumber>(V.F2));
        Arr.Add(MakeShared<FJsonValueNumber>(V.F3));
        Target->SetArrayField(TEXT("vector"), Arr);
        break;
    }
    default:
        Target->SetNumberField(TEXT("value"), V.F0);
        break;
    }
}

TSharedRef<FJsonObject> SeriesPointJson(const FChangePoint& P)
{
    TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("t"), P.Ts);
    if (P.DomainFrame >= 0)
    {
        Obj->SetNumberField(TEXT("df"), (double)P.DomainFrame);
    }
    FillValueFields(Obj, P.Value);
    return Obj;
}

FEnvelopeMeta BaseEnvelope(const FSessionModel& Session, int32 TotalCount)
{
    FEnvelopeMeta Meta;
    Meta.TotalCount = TotalCount;
    if (!Session.TimeAxis.IsEmpty())
    {
        Meta.bHasTimeDomain = true;
        Meta.TimeDomain = Session.TimeAxis;
    }
    if (Session.HasTimeRange())
    {
        Meta.bHasTimeRange = true;
        Meta.TimeRangeFrom = Session.MinTs;
        Meta.TimeRangeTo = Session.MaxTs;
    }
    return Meta;
}

int32 ParseCursor(const FString& Cursor)
{
    if (Cursor.IsEmpty())
    {
        return 0;
    }
    const int32 V = FCString::Atoi(*Cursor);
    return V > 0 ? V : 0;
}

} // namespace

int32 RecorderQuery::SeverityLevelFromName(const FString& Name)
{
    if (Name.IsEmpty())
    {
        return INDEX_NONE;
    }
    // Numeric severities round-trip through the ordinal directly.
    if (Name.IsNumeric())
    {
        return FCString::Atoi(*Name);
    }
    for (int32 i = 0; i < SeverityNames().Num(); ++i)
    {
        if (SeverityNames()[i].Equals(Name, ESearchCase::IgnoreCase))
        {
            return i;
        }
    }
    return INDEX_NONE;
}

TSharedRef<FJsonObject> FRecorderQueryEngine::Describe(int32 ObjectCatalogCap, bool bHasFrom, double From,
                                                       bool bHasTo, double To) const
{
    const FSessionModel& S = *Session;

    const bool bWindowed = bHasFrom || bHasTo;
    const double WinFrom = bHasFrom ? From : (S.HasTimeRange() ? S.MinTs : 0.0);
    const double WinTo = bHasTo ? To : (S.HasTimeRange() ? S.MaxTs : 0.0);

    // Windowed activity: change points whose Ts falls inside the window, summed
    // per object key over every (key, tag) series.
    TMap<FString, int64> WindowedActivity;
    if (bWindowed)
    {
        for (const TPair<TPair<FString, FString>, TArray<FChangePoint>>& Entry : S.Series)
        {
            int32 First = 0;
            int32 Last = -1;
            RecorderAsOf::SliceRange(Entry.Value, WinFrom, WinTo, First, Last);
            if (First <= Last)
            {
                WindowedActivity.FindOrAdd(Entry.Key.Key) += Last - First + 1;
            }
        }
    }
    auto ActivityOf = [&](const FObjectRecord& O) -> int64
    {
        if (!bWindowed)
        {
            return O.Activity;
        }
        const int64* Found = WindowedActivity.Find(O.Key);
        return Found ? *Found : 0;
    };

    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("sessionId"), S.SessionId);
    Result->SetStringField(TEXT("label"), S.Label);
    Result->SetStringField(TEXT("engine"), S.Engine);
    Result->SetStringField(TEXT("startUtc"), S.StartUtc);

    // Object catalog, ranked by activity (change-point count) so a cap keeps the
    // busiest objects. Elide the tail rather than truncating arbitrarily.
    TArray<const FObjectRecord*> Ordered;
    Ordered.Reserve(S.Objects.Num());
    for (const TPair<FString, FObjectRecord>& Entry : S.Objects)
    {
        Ordered.Add(&Entry.Value);
    }
    Ordered.Sort([&ActivityOf](const FObjectRecord& A, const FObjectRecord& B)
    {
        return ActivityOf(A) > ActivityOf(B);
    });

    const int32 Cap = ObjectCatalogCap > 0 ? ObjectCatalogCap : Ordered.Num();
    const int32 Shown = FMath::Min(Cap, Ordered.Num());
    const int32 ObjectsElided = Ordered.Num() - Shown;

    TArray<TSharedPtr<FJsonValue>> ObjectsJson;
    for (int32 i = 0; i < Shown; ++i)
    {
        const FObjectRecord& O = *Ordered[i];
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("key"), O.Key);
        Obj->SetStringField(TEXT("label"), O.Label);
        Obj->SetStringField(TEXT("path"), O.Path);
        Obj->SetStringField(TEXT("type"), O.Type);
        Obj->SetStringField(TEXT("parent"), O.ParentKey);
        Obj->SetNumberField(TEXT("tFirst"), O.TsFirst);
        Obj->SetNumberField(TEXT("activity"), (double)ActivityOf(O));
        ObjectsJson.Add(MakeShared<FJsonValueObject>(Obj));
    }
    Result->SetArrayField(TEXT("objects"), ObjectsJson);

    // Variable manifest is the load-bearing first-call payload; never elided.
    TArray<TSharedPtr<FJsonValue>> VarsJson;
    for (const TPair<FString, FVariableManifest>& Entry : S.Variables)
    {
        const FVariableManifest& V = Entry.Value;
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("tag"), V.Tag);
        Obj->SetStringField(TEXT("kind"), KindToString(V.Kind));
        Obj->SetStringField(TEXT("unit"), V.Unit);
        Obj->SetStringField(TEXT("domain"), V.Domain);
        Obj->SetNumberField(TEXT("tFirst"), V.TsFirst);
        if (V.bHasEpsilon)
        {
            Obj->SetNumberField(TEXT("eps"), V.Epsilon);
        }
        Obj->SetBoolField(TEXT("mixed"), V.bMixed);
        VarsJson.Add(MakeShared<FJsonValueObject>(Obj));
    }
    Result->SetArrayField(TEXT("variables"), VarsJson);

    int32 EventCount = S.Events.Num();
    if (bWindowed)
    {
        EventCount = 0;
        for (const FEventRecord& E : S.Events)
        {
            if (E.Ts >= WinFrom && E.Ts <= WinTo)
            {
                ++EventCount;
            }
        }
    }
    Result->SetNumberField(TEXT("eventCount"), EventCount);

    FEnvelopeMeta Meta = BaseEnvelope(S, S.Objects.Num() + S.Variables.Num());
    Meta.Elided = ObjectsElided;
    Meta.bTruncated = ObjectsElided > 0;
    if (bWindowed)
    {
        Meta.bHasTimeRange = true;
        Meta.TimeRangeFrom = WinFrom;
        Meta.TimeRangeTo = WinTo;
    }
    Result->SetObjectField(TEXT("meta"), RecorderEnvelope::Build(Meta));
    return Result;
}

TSharedRef<FJsonObject> FRecorderQueryEngine::GetState(const FString& Key, const TArray<FString>& Tags, double AtTs) const
{
    const FSessionModel& S = *Session;

    TArray<FString> Wanted = Tags.Num() > 0 ? Tags : S.TagsForKey(Key);

    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("key"), Key);
    Result->SetNumberField(TEXT("t"), AtTs);

    TArray<TSharedPtr<FJsonValue>> StatesJson;
    for (const FString& Tag : Wanted)
    {
        TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
        State->SetStringField(TEXT("tag"), Tag);
        State->SetStringField(TEXT("kind"), KindToString(S.KindOf(Tag)));

        const TArray<FChangePoint>* Series = S.FindSeries(Key, Tag);
        FChangePoint Point;
        if (Series && RecorderAsOf::ValueAt(*Series, AtTs, Point))
        {
            FillValueFields(State, Point.Value);
            State->SetNumberField(TEXT("atT"), Point.Ts);
        }
        else
        {
            State->SetBoolField(TEXT("noData"), true);
        }
        StatesJson.Add(MakeShared<FJsonValueObject>(State));
    }
    Result->SetArrayField(TEXT("tags"), StatesJson);

    FEnvelopeMeta Meta = BaseEnvelope(S, StatesJson.Num());
    Result->SetObjectField(TEXT("meta"), RecorderEnvelope::Build(Meta));
    return Result;
}

TSharedRef<FJsonObject> FRecorderQueryEngine::SummarizeChange(const FString& Key, const FString& Tag,
                                                              double From, double To) const
{
    const FSessionModel& S = *Session;

    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("key"), Key);
    Result->SetStringField(TEXT("tag"), Tag);
    Result->SetStringField(TEXT("kind"), KindToString(S.KindOf(Tag)));
    Result->SetNumberField(TEXT("from"), From);
    Result->SetNumberField(TEXT("to"), To);

    FEnvelopeMeta Meta = BaseEnvelope(S, 0);
    Meta.bHasReduction = true;
    Meta.Reduction = TEXT("summary");
    const FString Unit = S.UnitOf(Tag);
    if (!Unit.IsEmpty())
    {
        Meta.bHasUnits = true;
        Meta.Units = Unit;
    }
    Meta.bHasThreshold = true;
    Meta.ThresholdApplied = S.EpsilonOf(Tag, DefaultEpsilon);

    const TArray<FChangePoint>* Series = S.FindSeries(Key, Tag);
    if (!Series || Series->Num() == 0)
    {
        Result->SetObjectField(TEXT("meta"), RecorderEnvelope::Build(Meta));
        return Result;
    }

    const RecorderReductions::FSummary Sum = RecorderReductions::Summarize(*Series, From, To);
    if (Sum.bStartSet)    Result->SetNumberField(TEXT("startValue"), Sum.Start);
    if (Sum.bEndSet)      Result->SetNumberField(TEXT("endValue"), Sum.End);
    if (Sum.bNetDeltaSet) Result->SetNumberField(TEXT("netDelta"), Sum.NetDelta);
    if (Sum.bMinSet)      Result->SetNumberField(TEXT("min"), Sum.Min);
    if (Sum.bMaxSet)      Result->SetNumberField(TEXT("max"), Sum.Max);
    if (Sum.bMeanSet)     Result->SetNumberField(TEXT("mean"), Sum.Mean);
    if (Sum.bArgMaxSet)   Result->SetNumberField(TEXT("argMaxT"), Sum.ArgMaxTs);
    if (Sum.bArgMinSet)   Result->SetNumberField(TEXT("argMinT"), Sum.ArgMinTs);
    Result->SetNumberField(TEXT("changeCount"), Sum.ChangeCount);

    Meta.TotalCount = Sum.ChangeCount;
    Meta.bHasTimeRange = true;
    Meta.TimeRangeFrom = From;
    Meta.TimeRangeTo = To;
    Result->SetObjectField(TEXT("meta"), RecorderEnvelope::Build(Meta));
    return Result;
}

TSharedRef<FJsonObject> FRecorderQueryEngine::GetSeries(const FString& Key, const FString& Tag,
                                                        double From, double To, const FString& Reduction,
                                                        int32 MaxPoints, const FString& Cursor) const
{
    const FSessionModel& S = *Session;

    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("key"), Key);
    Result->SetStringField(TEXT("tag"), Tag);
    Result->SetStringField(TEXT("kind"), KindToString(S.KindOf(Tag)));

    FEnvelopeMeta Meta = BaseEnvelope(S, 0);
    Meta.bHasReduction = true;
    Meta.Reduction = Reduction.IsEmpty() ? TEXT("downsample") : Reduction;
    const FString Unit = S.UnitOf(Tag);
    if (!Unit.IsEmpty())
    {
        Meta.bHasUnits = true;
        Meta.Units = Unit;
    }
    Meta.bHasThreshold = true;
    Meta.ThresholdApplied = S.EpsilonOf(Tag, DefaultEpsilon);
    Meta.bHasTimeRange = true;
    Meta.TimeRangeFrom = From;
    Meta.TimeRangeTo = To;

    const TArray<FChangePoint>* Series = S.FindSeries(Key, Tag);
    if (!Series || Series->Num() == 0)
    {
        Result->SetArrayField(TEXT("points"), TArray<TSharedPtr<FJsonValue>>());
        Result->SetObjectField(TEXT("meta"), RecorderEnvelope::Build(Meta));
        return Result;
    }

    const int32 RequestedCap = MaxPoints <= 0 ? DefaultMaxPoints : MaxPoints;
    const int32 Cap = FMath::Min(RequestedCap, HardRowCap);

    TArray<FChangePoint> Points;
    int32 Total = 0;
    int32 Elided = 0;
    int32 NextCursor = INDEX_NONE;

    if (Reduction == TEXT("diff"))
    {
        const int32 CursorIndex = ParseCursor(Cursor);
        RecorderReductions::Diff(*Series, From, To, Cap, CursorIndex, Points, Total, Elided, NextCursor);
    }
    else
    {
        // "downsample" and "summary" both fall to the peak-preserving downsample.
        RecorderReductions::Downsample(*Series, From, To, Cap, Points, Total, Elided);
    }

    TArray<TSharedPtr<FJsonValue>> PointsJson;
    PointsJson.Reserve(Points.Num());
    for (const FChangePoint& P : Points)
    {
        PointsJson.Add(MakeShared<FJsonValueObject>(SeriesPointJson(P)));
    }
    Result->SetArrayField(TEXT("points"), PointsJson);

    Meta.TotalCount = Total;
    Meta.Elided = FMath::Max(0, Elided);
    Meta.bTruncated = Elided > 0;
    if (NextCursor != INDEX_NONE)
    {
        Meta.bHasNextCursor = true;
        Meta.NextCursor = FString::FromInt(NextCursor);
    }
    Result->SetObjectField(TEXT("meta"), RecorderEnvelope::Build(Meta));
    return Result;
}

TSharedRef<FJsonObject> FRecorderQueryEngine::FindEvents(const FString& Name, bool bHasSeverityMin, int32 SeverityMin,
                                                         const FString& Key, bool bHasFrom, double From,
                                                         bool bHasTo, double To, int32 Limit,
                                                         const FString& Cursor) const
{
    const FSessionModel& S = *Session;

    const int32 Cap = FMath::Min(Limit <= 0 ? DefaultEventLimit : Limit, HardRowCap);

    TArray<const FEventRecord*> Matches;
    for (const FEventRecord& E : S.Events)
    {
        if (!Name.IsEmpty() && !E.Name.Equals(Name, ESearchCase::CaseSensitive))
            continue;
        if (!Key.IsEmpty() && !E.Key.Equals(Key, ESearchCase::CaseSensitive))
            continue;
        if (bHasFrom && E.Ts < From)
            continue;
        if (bHasTo && E.Ts > To)
            continue;
        if (bHasSeverityMin)
        {
            const int32 Level = RecorderQuery::SeverityLevelFromName(E.Severity);
            if (Level == INDEX_NONE || Level < SeverityMin)
                continue;
        }
        Matches.Add(&E);
    }

    Matches.Sort([](const FEventRecord& A, const FEventRecord& B)
    {
        return A.Ts < B.Ts;
    });

    const int32 Total = Matches.Num();
    const int32 StartOffset = ParseCursor(Cursor);
    const int32 Shown = FMath::Max(0, FMath::Min(Cap, Total - StartOffset));

    TArray<TSharedPtr<FJsonValue>> EventsJson;
    EventsJson.Reserve(Shown);
    for (int32 i = 0; i < Shown; ++i)
    {
        const FEventRecord& E = *Matches[StartOffset + i];
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("id"), (double)E.EventId);
        Obj->SetNumberField(TEXT("t"), E.Ts);
        Obj->SetStringField(TEXT("dom"), E.Domain);
        Obj->SetStringField(TEXT("key"), E.Key);
        Obj->SetStringField(TEXT("name"), E.Name);
        Obj->SetStringField(TEXT("sev"), E.Severity);
        Obj->SetObjectField(TEXT("props"), E.Props.IsValid() ? E.Props.ToSharedRef() : MakeShared<FJsonObject>());
        EventsJson.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("events"), EventsJson);

    FEnvelopeMeta Meta = BaseEnvelope(S, Total);
    const int32 Consumed = StartOffset + Shown;
    Meta.Elided = FMath::Max(0, Total - Consumed);
    Meta.bTruncated = Meta.Elided > 0;
    if (Consumed < Total)
    {
        Meta.bHasNextCursor = true;
        Meta.NextCursor = FString::FromInt(Consumed);
    }
    if (bHasFrom && bHasTo)
    {
        Meta.bHasTimeRange = true;
        Meta.TimeRangeFrom = From;
        Meta.TimeRangeTo = To;
    }
    Result->SetObjectField(TEXT("meta"), RecorderEnvelope::Build(Meta));
    return Result;
}

TSharedRef<FJsonObject> FRecorderQueryEngine::ListSegments() const
{
    const FSessionModel& S = *Session;

    const TArray<RecorderSegments::FSegment> Segments =
        RecorderSegments::PairSegments(S, RecorderSegmentRegistry::Get());

    TArray<TSharedPtr<FJsonValue>> SegmentsJson;
    SegmentsJson.Reserve(Segments.Num());
    for (const RecorderSegments::FSegment& Seg : Segments)
    {
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("type"), Seg.Type);
        Obj->SetNumberField(TEXT("index"), (double)Seg.Index);
        Obj->SetNumberField(TEXT("tMin"), Seg.TMin);
        Obj->SetNumberField(TEXT("tMax"), Seg.TMax);
        if (Seg.bStartOpen)
        {
            Obj->SetBoolField(TEXT("startOpen"), true);
        }
        if (Seg.bEndOpen)
        {
            Obj->SetBoolField(TEXT("endOpen"), true);
        }
        Obj->SetObjectField(TEXT("props"), Seg.Props.IsValid() ? Seg.Props.ToSharedRef() : MakeShared<FJsonObject>());

        TArray<TSharedPtr<FJsonValue>> AnchorsJson;
        AnchorsJson.Reserve(Seg.AnchorEvents.Num());
        for (const RecorderSegments::FSegmentAnchor& A : Seg.AnchorEvents)
        {
            TSharedRef<FJsonObject> Anchor = MakeShared<FJsonObject>();
            Anchor->SetNumberField(TEXT("id"), (double)A.EventId);
            Anchor->SetNumberField(TEXT("t"), A.Ts);
            Anchor->SetStringField(TEXT("name"), A.Name);
            Anchor->SetStringField(TEXT("key"), A.Key);
            AnchorsJson.Add(MakeShared<FJsonValueObject>(Anchor));
        }
        Obj->SetArrayField(TEXT("anchorEvents"), AnchorsJson);

        SegmentsJson.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("segments"), SegmentsJson);

    FEnvelopeMeta Meta = BaseEnvelope(S, Segments.Num());
    Result->SetObjectField(TEXT("meta"), RecorderEnvelope::Build(Meta));
    return Result;
}
