// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for segment pairing (list_segments) and the windowed describe_session
// manifest, driven against hand-built in-memory sessions (no file IO): boundary
// pairing with interior anchors + half-open degradation, and window-scoped
// activity ranking / event counting.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

#include "Handlers/Recorder/RecorderSessionModel.h"
#include "Handlers/Recorder/RecorderQueryEngine.h"
#include "Tests/Recorder/RecorderTestHelpers.h"
#include "RecorderSegmentRule.h"

using namespace RecorderModel;
using namespace RecorderTestHelpers;

namespace
{
    // Register the gameplay rules this test exercises (race_round, arena_round) on
    // top of the built-in editor_session default, then restore the default-only
    // set on scope exit so the global registry stays clean for other tests.
    struct FScopedTestSegmentRules
    {
        FScopedTestSegmentRules()
        {
            RecorderSegmentRegistry::ResetToDefault();

            auto Name = [](const TCHAR* N) { return FRecorderEventMatcher{ N, FString(), FString() }; };

            FRecorderSegmentRule Race;
            Race.Type = TEXT("race_round");
            Race.Starts = { Name(TEXT("race:countdown_start")) };
            Race.Anchors = { Name(TEXT("race:run_start")), Name(TEXT("race:reset")) };
            Race.Ends = { Name(TEXT("race:run_finish")), Name(TEXT("race:run_abandon")) };
            RecorderSegmentRegistry::Register(Race);

            FRecorderSegmentRule Arena;
            Arena.Type = TEXT("arena_round");
            Arena.Starts = { Name(TEXT("arena:round_start")) };
            Arena.Ends = { Name(TEXT("arena:round_end")) };
            RecorderSegmentRegistry::Register(Arena);
        }
        ~FScopedTestSegmentRules() { RecorderSegmentRegistry::ResetToDefault(); }
    };
}

// ============================================================================
// list_segments: boundary pairing, interior anchors, half-open degradation
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecorderListSegmentsPairingTest,
    "PinWright.recorder.query.list_segments.PairsAndDegradesHalfOpen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRecorderListSegmentsPairingTest::RunTest(const FString& Parameters)
{
    FScopedTestSegmentRules SegmentRules;

    TSharedRef<FSessionModel> Session = MakeShared<FSessionModel>();

    auto AddEvent = [&](int64 Id, double Ts, const FString& Name, TSharedPtr<FJsonObject> Props = nullptr)
    {
        FEventRecord E;
        E.EventId = Id;
        E.Ts = Ts;
        E.Name = Name;
        E.Severity = TEXT("Info");
        E.Props = Props;
        Session->Events.Add(E);
        if (Ts < Session->MinTs) Session->MinTs = Ts;
        if (Ts > Session->MaxTs) Session->MaxTs = Ts;
    };
    auto EditorProps = [](const TCHAR* Action)
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("action"), Action);
        P->SetStringField(TEXT("editor_session_id"), TEXT("GUID-A"));
        return P;
    };
    auto RoundProps = [](int32 Round)
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetNumberField(TEXT("round"), Round);
        return P;
    };

    // Race round 1: countdown -> run_start + mid-run soft reset (anchors) -> finish.
    AddEvent(1, 1.0, TEXT("race:countdown_start"));
    AddEvent(2, 2.0, TEXT("race:run_start"));
    AddEvent(3, 5.0, TEXT("race:reset"));
    AddEvent(4, 10.0, TEXT("race:run_finish"));
    // Race round 2: never finished -> closes half-open at session end.
    AddEvent(5, 12.0, TEXT("race:countdown_start"));
    // Arena round, cleanly paired.
    AddEvent(6, 20.0, TEXT("arena:round_start"), RoundProps(1));
    AddEvent(7, 25.0, TEXT("arena:round_end"), RoundProps(1));
    // Editor session: same event name, paired via the action discriminator.
    AddEvent(8, 30.0, TEXT("editor:session"), EditorProps(TEXT("open")));
    AddEvent(9, 40.0, TEXT("editor:session"), EditorProps(TEXT("exit")));

    FRecorderQueryEngine Engine(Session);
    TSharedRef<FJsonObject> R = Engine.ListSegments();

    const TArray<TSharedPtr<FJsonValue>>* Segments = nullptr;
    TestTrue(TEXT("has segments"), R->TryGetArrayField(TEXT("segments"), Segments));
    if (!Segments) return false;
    TestEqual(TEXT("segment count"), Segments->Num(), 4);
    if (Segments->Num() != 4) return false;

    // Output is sorted by tMin: round 1, round 2, arena, editor.
    const TSharedPtr<FJsonObject> Round1 = (*Segments)[0]->AsObject();
    TestEqual(TEXT("round1 type"), GetStr(Round1, TEXT("type")), FString(TEXT("race_round")));
    TestEqual(TEXT("round1 index"), GetNum(Round1, TEXT("index")), 1.0);
    TestEqual(TEXT("round1 tMin"), GetNum(Round1, TEXT("tMin")), 1.0);
    // race:reset@5 must NOT have split round 1 — it runs through to the finish.
    TestEqual(TEXT("round1 tMax is the finish, not the reset"), GetNum(Round1, TEXT("tMax")), 10.0);
    TestFalse(TEXT("round1 closed"), GetBool(Round1, TEXT("endOpen")));
    {
        const TArray<TSharedPtr<FJsonValue>>* Anchors = nullptr;
        TestTrue(TEXT("round1 has anchors"), Round1->TryGetArrayField(TEXT("anchorEvents"), Anchors));
        if (Anchors && Anchors->Num() == 2)
        {
            TestEqual(TEXT("round1 anchor run_start"), GetStr((*Anchors)[0]->AsObject(), TEXT("name")), FString(TEXT("race:run_start")));
            TestEqual(TEXT("round1 anchor reset"), GetStr((*Anchors)[1]->AsObject(), TEXT("name")), FString(TEXT("race:reset")));
        }
        else
        {
            AddError(TEXT("round1 expected 2 anchor events (run_start + reset)"));
        }
    }

    const TSharedPtr<FJsonObject> Round2 = (*Segments)[1]->AsObject();
    TestEqual(TEXT("round2 type"), GetStr(Round2, TEXT("type")), FString(TEXT("race_round")));
    TestEqual(TEXT("round2 index"), GetNum(Round2, TEXT("index")), 2.0);
    TestEqual(TEXT("round2 tMin"), GetNum(Round2, TEXT("tMin")), 12.0);
    TestEqual(TEXT("round2 closes at session end"), GetNum(Round2, TEXT("tMax")), 40.0);
    TestTrue(TEXT("round2 endOpen"), GetBool(Round2, TEXT("endOpen")));

    const TSharedPtr<FJsonObject> Arena = (*Segments)[2]->AsObject();
    TestEqual(TEXT("arena type"), GetStr(Arena, TEXT("type")), FString(TEXT("arena_round")));
    TestEqual(TEXT("arena tMin"), GetNum(Arena, TEXT("tMin")), 20.0);
    TestEqual(TEXT("arena tMax"), GetNum(Arena, TEXT("tMax")), 25.0);
    {
        const TSharedPtr<FJsonObject>* Props = nullptr;
        TestTrue(TEXT("arena has props"), Arena->TryGetObjectField(TEXT("props"), Props));
        if (Props)
        {
            TestEqual(TEXT("arena props echo round"), GetNum(*Props, TEXT("round")), 1.0);
        }
    }

    const TSharedPtr<FJsonObject> Editor = (*Segments)[3]->AsObject();
    TestEqual(TEXT("editor type"), GetStr(Editor, TEXT("type")), FString(TEXT("editor_session")));
    TestEqual(TEXT("editor tMin"), GetNum(Editor, TEXT("tMin")), 30.0);
    TestEqual(TEXT("editor tMax"), GetNum(Editor, TEXT("tMax")), 40.0);
    TestFalse(TEXT("editor closed"), GetBool(Editor, TEXT("endOpen")));
    return true;
}

// ============================================================================
// describe_session: from/to window scopes activity ranking + event count
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecorderDescribeWindowedTest,
    "PinWright.recorder.query.describe.WindowScopesActivityAndEventCount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRecorderDescribeWindowedTest::RunTest(const FString& Parameters)
{
    TSharedRef<FSessionModel> Session = MakeShared<FSessionModel>();

    auto AddObjectSeries = [&](const FString& Key, const TArray<double>& Times)
    {
        FObjectRecord O;
        O.Key = Key;
        O.Label = Key;
        O.Activity = Times.Num();
        Session->Objects.Add(Key, O);

        TArray<FChangePoint>& Series = Session->Series.FindOrAdd(TPair<FString, FString>(Key, TEXT("v")));
        for (double T : Times)
        {
            FChangePoint P;
            P.Ts = T;
            P.Value.Kind = EValueKind::Float;
            P.Value.F0 = T;
            Series.Add(P);
            if (T < Session->MinTs) Session->MinTs = T;
            if (T > Session->MaxTs) Session->MaxTs = T;
        }
    };
    auto AddEvent = [&](int64 Id, double Ts)
    {
        FEventRecord E;
        E.EventId = Id;
        E.Ts = Ts;
        E.Name = TEXT("tick");
        E.Severity = TEXT("Info");
        Session->Events.Add(E);
        if (Ts < Session->MinTs) Session->MinTs = Ts;
        if (Ts > Session->MaxTs) Session->MaxTs = Ts;
    };

    FVariableManifest Var;
    Var.Tag = TEXT("v");
    Var.Kind = EValueKind::Float;
    Session->Variables.Add(TEXT("v"), Var);

    // A is busiest over the whole session (10 changes in [0, 4.5]); B has only
    // 3 changes, but all inside [6, 10].
    TArray<double> ATimes;
    for (int32 i = 0; i < 10; ++i)
    {
        ATimes.Add(i * 0.5);
    }
    AddObjectSeries(TEXT("A"), ATimes);
    AddObjectSeries(TEXT("B"), { 6.0, 8.0, 10.0 });

    // Six events split across the halves: two before t=6, four inside [6, 10] — of
    // which two sit EXACTLY on the window edges. The edge pair is what makes the
    // window's inclusivity testable: Describe's event filter is
    // `E.Ts >= WinFrom && E.Ts <= WinTo` (RecorderQueryEngine.cpp), and with no event
    // on an edge, weakening it to `>` / `<` changes no count.
    AddEvent(1, 1.0);
    AddEvent(2, 2.0);
    AddEvent(3, 7.0);
    AddEvent(4, 9.0);
    AddEvent(5, 6.0);
    AddEvent(6, 10.0);

    FRecorderQueryEngine Engine(Session);

    auto FirstObjectKey = [](const TSharedRef<FJsonObject>& R) -> FString
    {
        const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
        if (R->TryGetArrayField(TEXT("objects"), Objects) && Objects && Objects->Num() > 0)
        {
            return GetStr((*Objects)[0]->AsObject(), TEXT("key"));
        }
        return FString();
    };

    // Whole session: A ranks first; the event count covers everything.
    TSharedRef<FJsonObject> Whole = Engine.Describe();
    TestEqual(TEXT("whole-session busiest is A"), FirstObjectKey(Whole), FString(TEXT("A")));
    TestEqual(TEXT("whole-session eventCount"), GetNum(Whole, TEXT("eventCount")), 6.0);

    // Window [6, 10] with cap 1: B's 3 in-window changes beat A's 0, and the four
    // in-window events are counted — the two interior ones (7, 9) plus the two sitting
    // exactly on the edges (6, 10), because the window is inclusive at both ends.
    TSharedRef<FJsonObject> Windowed = Engine.Describe(1, true, 6.0, true, 10.0);
    TestEqual(TEXT("windowed busiest is B"), FirstObjectKey(Windowed), FString(TEXT("B")));
    TestEqual(TEXT("windowed eventCount"), GetNum(Windowed, TEXT("eventCount")), 4.0);

    // Pulling the window just inside both edges drops exactly the two edge events, so
    // the 4 above is those edges being counted rather than a looser match.
    TSharedRef<FJsonObject> Inner = Engine.Describe(1, true, 6.5, true, 9.5);
    TestEqual(TEXT("eventCount strictly inside the edges"), GetNum(Inner, TEXT("eventCount")), 2.0);

    // Each bound pinned on its own: a window collapsed onto one edge keeps the event
    // that sits on it. `>=`/`<=` weakened to `>`/`<` empties both of these.
    TSharedRef<FJsonObject> AtFrom = Engine.Describe(1, true, 6.0, true, 6.0);
    TestEqual(TEXT("event exactly at the window start is counted"), GetNum(AtFrom, TEXT("eventCount")), 1.0);
    TSharedRef<FJsonObject> AtTo = Engine.Describe(1, true, 10.0, true, 10.0);
    TestEqual(TEXT("event exactly at the window end is counted"), GetNum(AtTo, TEXT("eventCount")), 1.0);
    {
        const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
        Windowed->TryGetArrayField(TEXT("objects"), Objects);
        if (Objects && Objects->Num() == 1)
        {
            TestEqual(TEXT("windowed activity is in-window count"), GetNum((*Objects)[0]->AsObject(), TEXT("activity")), 3.0);
        }
        else
        {
            AddError(TEXT("windowed catalog expected exactly 1 object (cap 1)"));
        }
    }

    // The envelope echoes the effective window as the covered time range.
    const TSharedPtr<FJsonObject>* Meta = nullptr;
    TestTrue(TEXT("windowed has meta"), Windowed->TryGetObjectField(TEXT("meta"), Meta));
    if (Meta)
    {
        const TArray<TSharedPtr<FJsonValue>>* Range = nullptr;
        TestTrue(TEXT("meta has timeRangeCovered"), (*Meta)->TryGetArrayField(TEXT("timeRangeCovered"), Range));
        if (Range && Range->Num() == 2)
        {
            TestEqual(TEXT("timeRange from"), (*Range)[0]->AsNumber(), 6.0);
            TestEqual(TEXT("timeRange to"), (*Range)[1]->AsNumber(), 10.0);
        }
    }
    return true;
}
