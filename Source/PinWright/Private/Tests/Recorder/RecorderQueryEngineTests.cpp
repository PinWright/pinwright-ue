// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for FRecorderQueryEngine driven against hand-built in-memory sessions
// (no file IO): summarize argMax on a known peak, as-of carry-forward, downsample
// endpoint + peak preservation, and find_events filtering.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

#include "Handlers/Recorder/RecorderSessionModel.h"
#include "Handlers/Recorder/RecorderQueryEngine.h"
#include "Tests/Recorder/RecorderTestHelpers.h"

using namespace RecorderModel;
using namespace RecorderTestHelpers;

namespace
{

FChangePoint FloatPoint(double Ts, double Value)
{
    FChangePoint P;
    P.Ts = Ts;
    P.Value.Kind = EValueKind::Float;
    P.Value.F0 = Value;
    return P;
}

// Build a session with one Float series under (key, tag) from the given samples.
TSharedRef<FSessionModel> MakeFloatSession(const FString& Key, const FString& Tag,
                                           const TArray<TPair<double, double>>& Samples,
                                           const FString& Unit = FString())
{
    TSharedRef<FSessionModel> Session = MakeShared<FSessionModel>();

    FObjectRecord Obj;
    Obj.Key = Key;
    Obj.Label = Key;
    Obj.Activity = Samples.Num();
    Session->Objects.Add(Key, Obj);

    FVariableManifest Var;
    Var.Tag = Tag;
    Var.Kind = EValueKind::Float;
    Var.Unit = Unit;
    Session->Variables.Add(Tag, Var);

    TArray<FChangePoint>& Series = Session->Series.FindOrAdd(TPair<FString, FString>(Key, Tag));
    for (const TPair<double, double>& S : Samples)
    {
        Series.Add(FloatPoint(S.Key, S.Value));
        if (S.Key < Session->MinTs) Session->MinTs = S.Key;
        if (S.Key > Session->MaxTs) Session->MaxTs = S.Key;
    }
    Series.Sort([](const FChangePoint& A, const FChangePoint& B) { return A.Ts < B.Ts; });
    return Session;
}

} // namespace

// ============================================================================
// summarize_change: argMax / argMin / net delta on a known peak
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecorderSummarizeArgMaxTest,
    "PinWright.recorder.query.summarize.ArgMaxOnKnownPeak",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRecorderSummarizeArgMaxTest::RunTest(const FString& Parameters)
{
    // Peak (5.0) at t=2.0; trough (1.0) at t=0.0; ends at 3.0.
    TSharedRef<FSessionModel> Session = MakeFloatSession(TEXT("d0"), TEXT("alt"),
    {
        { 0.0, 1.0 }, { 1.0, 2.0 }, { 2.0, 5.0 }, { 3.0, 3.0 }
    }, TEXT("m"));

    FRecorderQueryEngine Engine(Session);
    TSharedRef<FJsonObject> R = Engine.SummarizeChange(TEXT("d0"), TEXT("alt"), 0.0, 3.0);

    TestEqual(TEXT("startValue"), GetNum(R, TEXT("startValue")), 1.0);
    TestEqual(TEXT("endValue"), GetNum(R, TEXT("endValue")), 3.0);
    TestEqual(TEXT("netDelta"), GetNum(R, TEXT("netDelta")), 2.0);
    TestEqual(TEXT("max"), GetNum(R, TEXT("max")), 5.0);
    TestEqual(TEXT("min"), GetNum(R, TEXT("min")), 1.0);
    TestEqual(TEXT("argMaxT"), GetNum(R, TEXT("argMaxT")), 2.0);
    TestEqual(TEXT("argMinT"), GetNum(R, TEXT("argMinT")), 0.0);
    TestEqual(TEXT("changeCount"), GetNum(R, TEXT("changeCount")), 4.0);

    const TSharedPtr<FJsonObject>* Meta = nullptr;
    TestTrue(TEXT("has meta"), R->TryGetObjectField(TEXT("meta"), Meta));
    if (Meta)
    {
        FString Unit;
        (*Meta)->TryGetStringField(TEXT("units"), Unit);
        TestEqual(TEXT("units echoed"), Unit, FString(TEXT("m")));
    }
    return true;
}

// ============================================================================
// get_state: as-of carry-forward between change points
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecorderGetStateCarryForwardTest,
    "PinWright.recorder.query.get_state.CarryForward",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRecorderGetStateCarryForwardTest::RunTest(const FString& Parameters)
{
    // Changes at t=0 (=>10), t=2 (=>20), t=5 (=>30).
    TSharedRef<FSessionModel> Session = MakeFloatSession(TEXT("d0"), TEXT("throttle"),
    {
        { 0.0, 10.0 }, { 2.0, 20.0 }, { 5.0, 30.0 }
    });

    FRecorderQueryEngine Engine(Session);

    auto StateValueAt = [&](double Ts, double& OutValue, bool& OutNoData, double& OutAtT)
    {
        TSharedRef<FJsonObject> R = Engine.GetState(TEXT("d0"), {}, Ts);
        const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
        OutNoData = true;
        OutValue = -999.0;
        OutAtT = -999.0;
        if (R->TryGetArrayField(TEXT("tags"), Tags) && Tags && Tags->Num() == 1)
        {
            const TSharedPtr<FJsonObject> Tag = (*Tags)[0]->AsObject();
            bool bNoData = false;
            Tag->TryGetBoolField(TEXT("noData"), bNoData);
            OutNoData = bNoData;
            Tag->TryGetNumberField(TEXT("value"), OutValue);
            Tag->TryGetNumberField(TEXT("atT"), OutAtT);
        }
    };

    double Value, AtT; bool NoData;

    // Before the first change: no data.
    StateValueAt(-1.0, Value, NoData, AtT);
    TestTrue(TEXT("noData before first change"), NoData);

    // Between t=2 and t=5: carries forward the t=2 value.
    StateValueAt(3.5, Value, NoData, AtT);
    TestFalse(TEXT("has data at 3.5"), NoData);
    TestEqual(TEXT("carries 20.0 forward"), Value, 20.0);
    TestEqual(TEXT("atT is 2.0"), AtT, 2.0);

    // Exactly at a change point: that point's value.
    StateValueAt(5.0, Value, NoData, AtT);
    TestEqual(TEXT("value at exact change"), Value, 30.0);
    TestEqual(TEXT("atT is 5.0"), AtT, 5.0);

    // After the last change: still carries the last value.
    StateValueAt(100.0, Value, NoData, AtT);
    TestEqual(TEXT("carries last value past end"), Value, 30.0);
    return true;
}

// ============================================================================
// get_series downsample: endpoints + interior peak preserved under budget
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecorderDownsamplePeakTest,
    "PinWright.recorder.query.get_series.DownsamplePreservesEndpointsAndPeak",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRecorderDownsamplePeakTest::RunTest(const FString& Parameters)
{
    // 9 samples; a sharp interior peak at t=4 (=100). Budget = 3 must keep
    // first (t=0), last (t=8), and the peak (t=4).
    TArray<TPair<double, double>> Samples;
    for (int32 i = 0; i < 9; ++i)
    {
        Samples.Add(TPair<double, double>((double)i, i == 4 ? 100.0 : 1.0));
    }
    TSharedRef<FSessionModel> Session = MakeFloatSession(TEXT("d0"), TEXT("g"), Samples);

    FRecorderQueryEngine Engine(Session);
    TSharedRef<FJsonObject> R = Engine.GetSeries(TEXT("d0"), TEXT("g"), 0.0, 8.0, TEXT("downsample"), 3, FString());

    const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
    TestTrue(TEXT("has points"), R->TryGetArrayField(TEXT("points"), Points));
    if (!Points) return false;

    TestEqual(TEXT("point count == budget"), Points->Num(), 3);

    TArray<double> Ts;
    for (const TSharedPtr<FJsonValue>& V : *Points)
    {
        Ts.Add(GetNum(V->AsObject(), TEXT("t")));
    }
    TestEqual(TEXT("first endpoint t=0"), Ts[0], 0.0);
    TestEqual(TEXT("peak t=4 preserved"), Ts[1], 4.0);
    TestEqual(TEXT("last endpoint t=8"), Ts[2], 8.0);

    const TSharedPtr<FJsonObject>* Meta = nullptr;
    R->TryGetObjectField(TEXT("meta"), Meta);
    if (Meta)
    {
        bool bTruncated = false;
        (*Meta)->TryGetBoolField(TEXT("truncated"), bTruncated);
        TestTrue(TEXT("truncated when elided"), bTruncated);
        TestEqual(TEXT("totalCount is full window"), GetNum(*Meta, TEXT("totalCount")), 9.0);
        TestEqual(TEXT("elided count"), GetNum(*Meta, TEXT("elided")), 6.0);
    }
    return true;
}

// ============================================================================
// get_series diff: raw change list, cursor pagination
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecorderDiffPaginationTest,
    "PinWright.recorder.query.get_series.DiffPaginates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRecorderDiffPaginationTest::RunTest(const FString& Parameters)
{
    TArray<TPair<double, double>> Samples;
    for (int32 i = 0; i < 5; ++i)
    {
        Samples.Add(TPair<double, double>((double)i, (double)i));
    }
    TSharedRef<FSessionModel> Session = MakeFloatSession(TEXT("d0"), TEXT("g"), Samples);
    FRecorderQueryEngine Engine(Session);

    // First page: 2 of 5, nextCursor = 2.
    TSharedRef<FJsonObject> P1 = Engine.GetSeries(TEXT("d0"), TEXT("g"), 0.0, 4.0, TEXT("diff"), 2, FString());
    const TArray<TSharedPtr<FJsonValue>>* Pts1 = nullptr;
    P1->TryGetArrayField(TEXT("points"), Pts1);
    TestEqual(TEXT("page1 size"), Pts1 ? Pts1->Num() : -1, 2);

    const TSharedPtr<FJsonObject>* Meta1 = nullptr;
    P1->TryGetObjectField(TEXT("meta"), Meta1);
    FString NextCursor;
    if (Meta1) (*Meta1)->TryGetStringField(TEXT("nextCursor"), NextCursor);
    TestEqual(TEXT("page1 nextCursor"), NextCursor, FString(TEXT("2")));

    // Second page from the cursor: next 2.
    TSharedRef<FJsonObject> P2 = Engine.GetSeries(TEXT("d0"), TEXT("g"), 0.0, 4.0, TEXT("diff"), 2, NextCursor);
    const TArray<TSharedPtr<FJsonValue>>* Pts2 = nullptr;
    P2->TryGetArrayField(TEXT("points"), Pts2);
    TestEqual(TEXT("page2 size"), Pts2 ? Pts2->Num() : -1, 2);
    if (Pts2 && Pts2->Num() == 2)
    {
        TestEqual(TEXT("page2 starts at t=2"), GetNum((*Pts2)[0]->AsObject(), TEXT("t")), 2.0);
    }
    return true;
}

// ============================================================================
// find_events: name / severity / key / time-window filtering + ordering
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecorderFindEventsFilterTest,
    "PinWright.recorder.query.find_events.Filters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRecorderFindEventsFilterTest::RunTest(const FString& Parameters)
{
    TSharedRef<FSessionModel> Session = MakeShared<FSessionModel>();

    auto AddEvent = [&](int64 Id, double Ts, const FString& Key, const FString& Name, const FString& Sev)
    {
        FEventRecord E;
        E.EventId = Id;
        E.Ts = Ts;
        E.Key = Key;
        E.Name = Name;
        E.Severity = Sev;
        Session->Events.Add(E);
        if (Ts < Session->MinTs) Session->MinTs = Ts;
        if (Ts > Session->MaxTs) Session->MaxTs = Ts;
    };

    // Intentionally out of timestamp order to exercise the sort.
    AddEvent(1, 5.0, TEXT("d0"), TEXT("crash"), TEXT("Error"));
    AddEvent(2, 1.0, TEXT("d0"), TEXT("gate"), TEXT("Info"));
    AddEvent(3, 3.0, TEXT("d1"), TEXT("crash"), TEXT("Warning"));
    AddEvent(4, 9.0, TEXT("d0"), TEXT("gate"), TEXT("Info"));
    // Boundary probes sitting EXACTLY on the [2, 6] window edges. Without an event on
    // an edge, flipping FindEvents' `E.Ts < From` / `E.Ts > To` to `<=` / `>=` changes
    // no result and the window's inclusivity is untested. They carry a name/key/severity
    // that no other filter in this test selects (edge / d2 / Info), so only the window
    // counts below move.
    AddEvent(5, 2.0, TEXT("d2"), TEXT("edge"), TEXT("Info"));
    AddEvent(6, 6.0, TEXT("d2"), TEXT("edge"), TEXT("Info"));

    FRecorderQueryEngine Engine(Session);

    auto EventCount = [](const TSharedRef<FJsonObject>& R) -> int32
    {
        const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
        return R->TryGetArrayField(TEXT("events"), Events) && Events ? Events->Num() : -1;
    };

    // Name filter: two "crash" events.
    TSharedRef<FJsonObject> ByName = Engine.FindEvents(TEXT("crash"), false, 0, FString(), false, 0, false, 0, 0, FString());
    TestEqual(TEXT("crash count"), EventCount(ByName), 2);
    {
        const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
        ByName->TryGetArrayField(TEXT("events"), Events);
        // Ordered by timestamp: d1@3.0 before d0@5.0.
        TestEqual(TEXT("crash ordered by t"), GetNum((*Events)[0]->AsObject(), TEXT("t")), 3.0);
    }

    // Severity floor Warning excludes Info; keeps Warning + Error.
    TSharedRef<FJsonObject> BySev = Engine.FindEvents(FString(), true,
        RecorderQuery::SeverityLevelFromName(TEXT("Warning")), FString(), false, 0, false, 0, 0, FString());
    TestEqual(TEXT("severity>=Warning count"), EventCount(BySev), 2);

    // Key filter: three events on d0.
    TSharedRef<FJsonObject> ByKey = Engine.FindEvents(FString(), false, 0, TEXT("d0"), false, 0, false, 0, 0, FString());
    TestEqual(TEXT("d0 count"), EventCount(ByKey), 3);

    // Time window [2, 6] is INCLUSIVE on both ends (RecorderQueryEngine.cpp skips only
    // `E.Ts < From` and `E.Ts > To`): d2@2.0, d1@3.0, d0@5.0 and d2@6.0.
    TSharedRef<FJsonObject> ByWindow = Engine.FindEvents(FString(), false, 0, FString(), true, 2.0, true, 6.0, 0, FString());
    TestEqual(TEXT("window count"), EventCount(ByWindow), 4);

    // The same window pulled just inside both edges drops exactly the two boundary
    // events — so the 4 above is the boundary events being counted, not a wider match.
    TSharedRef<FJsonObject> ByInnerWindow = Engine.FindEvents(FString(), false, 0, FString(), true, 2.5, true, 5.5, 0, FString());
    TestEqual(TEXT("window count strictly inside the edges"), EventCount(ByInnerWindow), 2);

    // Each bound pinned on its own: a degenerate window collapsed onto one edge keeps
    // the event sitting on it. `<` -> `<=` (or `>` -> `>=`) empties these.
    TSharedRef<FJsonObject> AtFrom = Engine.FindEvents(FString(), false, 0, FString(), true, 2.0, true, 2.0, 0, FString());
    TestEqual(TEXT("event exactly at From is inside the window"), EventCount(AtFrom), 1);
    TSharedRef<FJsonObject> AtTo = Engine.FindEvents(FString(), false, 0, FString(), true, 6.0, true, 6.0, 0, FString());
    TestEqual(TEXT("event exactly at To is inside the window"), EventCount(AtTo), 1);

    // Half-open forms, so each bound is also exercised with the other absent.
    TSharedRef<FJsonObject> FromOnly = Engine.FindEvents(FString(), false, 0, FString(), true, 2.0, false, 0, 0, FString());
    TestEqual(TEXT("from-only window keeps t == From"), EventCount(FromOnly), 5);
    TSharedRef<FJsonObject> ToOnly = Engine.FindEvents(FString(), false, 0, FString(), false, 0, true, 6.0, 0, FString());
    TestEqual(TEXT("to-only window keeps t == To"), EventCount(ToOnly), 5);

    // Limit caps results and reports truncation.
    TSharedRef<FJsonObject> Limited = Engine.FindEvents(FString(), false, 0, FString(), false, 0, false, 0, 1, FString());
    TestEqual(TEXT("limited to 1"), EventCount(Limited), 1);
    const TSharedPtr<FJsonObject>* Meta = nullptr;
    Limited->TryGetObjectField(TEXT("meta"), Meta);
    if (Meta)
    {
        bool bTruncated = false;
        (*Meta)->TryGetBoolField(TEXT("truncated"), bTruncated);
        TestTrue(TEXT("truncated when limited"), bTruncated);
        TestEqual(TEXT("totalCount before cap"), GetNum(*Meta, TEXT("totalCount")), 6.0);
    }
    return true;
}

// ============================================================================
// find_events: cursor pagination through the sorted match list
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecorderFindEventsPaginationTest,
    "PinWright.recorder.query.find_events.Paginates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRecorderFindEventsPaginationTest::RunTest(const FString& Parameters)
{
    TSharedRef<FSessionModel> Session = MakeShared<FSessionModel>();

    auto AddEvent = [&](int64 Id, double Ts)
    {
        FEventRecord E;
        E.EventId = Id;
        E.Ts = Ts;
        E.Key = TEXT("d0");
        E.Name = TEXT("tick");
        E.Severity = TEXT("Info");
        Session->Events.Add(E);
        if (Ts < Session->MinTs) Session->MinTs = Ts;
        if (Ts > Session->MaxTs) Session->MaxTs = Ts;
    };

    for (int32 i = 0; i < 5; ++i)
    {
        AddEvent(i + 1, (double)i);
    }

    FRecorderQueryEngine Engine(Session);

    auto FirstEventT = [](const TSharedRef<FJsonObject>& R) -> double
    {
        const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
        if (R->TryGetArrayField(TEXT("events"), Events) && Events && Events->Num() > 0)
        {
            return GetNum((*Events)[0]->AsObject(), TEXT("t"));
        }
        return -999.0;
    };
    auto EventCount = [](const TSharedRef<FJsonObject>& R) -> int32
    {
        const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
        return R->TryGetArrayField(TEXT("events"), Events) && Events ? Events->Num() : -1;
    };

    // Page 1: 2 of 5, nextCursor = "2".
    TSharedRef<FJsonObject> P1 = Engine.FindEvents(FString(), false, 0, FString(), false, 0, false, 0, 2, FString());
    TestEqual(TEXT("page1 size"), EventCount(P1), 2);
    TestEqual(TEXT("page1 starts at t=0"), FirstEventT(P1), 0.0);
    const TSharedPtr<FJsonObject>* Meta1 = nullptr;
    P1->TryGetObjectField(TEXT("meta"), Meta1);
    FString NextCursor1;
    if (Meta1)
    {
        TestEqual(TEXT("page1 totalCount"), GetNum(*Meta1, TEXT("totalCount")), 5.0);
        TestEqual(TEXT("page1 elided"), GetNum(*Meta1, TEXT("elided")), 3.0);
        bool bTruncated = false;
        (*Meta1)->TryGetBoolField(TEXT("truncated"), bTruncated);
        TestTrue(TEXT("page1 truncated"), bTruncated);
        (*Meta1)->TryGetStringField(TEXT("nextCursor"), NextCursor1);
    }
    TestEqual(TEXT("page1 nextCursor"), NextCursor1, FString(TEXT("2")));

    // Page 2 from the cursor: next 2 starting at t=2.
    TSharedRef<FJsonObject> P2 = Engine.FindEvents(FString(), false, 0, FString(), false, 0, false, 0, 2, NextCursor1);
    TestEqual(TEXT("page2 size"), EventCount(P2), 2);
    TestEqual(TEXT("page2 starts at t=2"), FirstEventT(P2), 2.0);
    const TSharedPtr<FJsonObject>* Meta2 = nullptr;
    P2->TryGetObjectField(TEXT("meta"), Meta2);
    FString NextCursor2;
    if (Meta2) (*Meta2)->TryGetStringField(TEXT("nextCursor"), NextCursor2);
    TestEqual(TEXT("page2 nextCursor"), NextCursor2, FString(TEXT("4")));

    // Page 3: the final event; no truncation, nextCursor rendered null.
    TSharedRef<FJsonObject> P3 = Engine.FindEvents(FString(), false, 0, FString(), false, 0, false, 0, 2, NextCursor2);
    TestEqual(TEXT("page3 size"), EventCount(P3), 1);
    TestEqual(TEXT("page3 starts at t=4"), FirstEventT(P3), 4.0);
    const TSharedPtr<FJsonObject>* Meta3 = nullptr;
    P3->TryGetObjectField(TEXT("meta"), Meta3);
    if (Meta3)
    {
        bool bTruncated = true;
        (*Meta3)->TryGetBoolField(TEXT("truncated"), bTruncated);
        TestFalse(TEXT("page3 not truncated"), bTruncated);
        TestEqual(TEXT("page3 elided"), GetNum(*Meta3, TEXT("elided")), 0.0);
        FString NextCursor3;
        TestFalse(TEXT("page3 nextCursor is null"), (*Meta3)->TryGetStringField(TEXT("nextCursor"), NextCursor3));
    }
    return true;
}

// ============================================================================
// describe_session: object catalog capping by activity
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecorderDescribeCapTest,
    "PinWright.recorder.query.describe.CapsByActivity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRecorderDescribeCapTest::RunTest(const FString& Parameters)
{
    TSharedRef<FSessionModel> Session = MakeShared<FSessionModel>();
    for (int32 i = 0; i < 5; ++i)
    {
        FObjectRecord O;
        O.Key = FString::Printf(TEXT("d%d"), i);
        O.Activity = i; // d4 busiest, d0 idlest.
        Session->Objects.Add(O.Key, O);
    }

    FRecorderQueryEngine Engine(Session);
    TSharedRef<FJsonObject> R = Engine.Describe(2);

    const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
    TestTrue(TEXT("has objects"), R->TryGetArrayField(TEXT("objects"), Objects));
    if (!Objects) return false;
    TestEqual(TEXT("catalog capped to 2"), Objects->Num(), 2);

    // Highest activity kept first.
    FString FirstKey;
    (*Objects)[0]->AsObject()->TryGetStringField(TEXT("key"), FirstKey);
    TestEqual(TEXT("busiest object first"), FirstKey, FString(TEXT("d4")));

    const TSharedPtr<FJsonObject>* Meta = nullptr;
    R->TryGetObjectField(TEXT("meta"), Meta);
    if (Meta)
    {
        TestEqual(TEXT("elided tail"), GetNum(*Meta, TEXT("elided")), 3.0);
        bool bTruncated = false;
        (*Meta)->TryGetBoolField(TEXT("truncated"), bTruncated);
        TestTrue(TEXT("truncated flagged"), bTruncated);
    }
    return true;
}
