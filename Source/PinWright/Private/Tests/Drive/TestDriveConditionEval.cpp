// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for FDriveConditionEval: the pure-logic condition evaluator. Covers
// every EDriveConditionType (pass + fail), all six EDriveCompareOp values, the
// severity ordering boundaries, Handle-vs-Label matching, geometry containment,
// and that Actual/Expected are populated.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveConditionEval.h"
#include "Handlers/Drive/DriveTypes.h"

// File-local builders. Drive-prefixed names keep them clear of other
// anonymous-namespace symbols when Unity merges translation units.
namespace
{
    FDriveElement MakeCondTestElement(
        const FString& Handle,
        const FString& Label,
        bool bEnabled = true,
        bool bVisible = true,
        FVector2D Pos = FVector2D::ZeroVector,
        FVector2D Size = FVector2D::ZeroVector)
    {
        FDriveElement Element;
        Element.Handle = Handle;
        Element.Label = Label;
        Element.bEnabled = bEnabled;
        Element.bVisible = bVisible;
        Element.AbsolutePosition = Pos;
        Element.AbsoluteSize = Size;
        return Element;
    }

    FDriveJournalEvent MakeCondTestEvent(const FString& Name, const FString& Severity)
    {
        FDriveJournalEvent Event;
        Event.Id = Name + TEXT("_id");
        Event.Name = Name;
        Event.Severity = Severity;
        return Event;
    }

    FDriveCondition MakeCond(EDriveConditionType Type, const FString& Target)
    {
        FDriveCondition Condition;
        Condition.Type = Type;
        Condition.Target = Target;
        return Condition;
    }
}

// ============================================================================
// WidgetPresent / WidgetAbsent (+ Handle vs Label matching)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveCondWidgetPresenceTest,
    "PinWright.drive.condition.WidgetPresence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveCondWidgetPresenceTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Elements;
    Elements.Add(MakeCondTestElement(TEXT("el_1"), TEXT("Start")));
    Elements.Add(MakeCondTestElement(TEXT("el_2"), TEXT("Quit")));

    // Present: match by Handle.
    {
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(
            MakeCond(EDriveConditionType::WidgetPresent, TEXT("el_1")), Elements, FDriveJournalDelta());
        TestTrue(TEXT("present by handle is met"), R.bMet);
        TestEqual(TEXT("present actual"), R.Actual, TEXT("present"));
        TestEqual(TEXT("present expected"), R.Expected, TEXT("present"));
    }

    // Present: match by Label (case-insensitive).
    {
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(
            MakeCond(EDriveConditionType::WidgetPresent, TEXT("start")), Elements, FDriveJournalDelta());
        TestTrue(TEXT("present by label (case-insensitive) is met"), R.bMet);
    }

    // Not present: no element matches.
    {
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(
            MakeCond(EDriveConditionType::WidgetPresent, TEXT("Nope")), Elements, FDriveJournalDelta());
        TestFalse(TEXT("missing target not present"), R.bMet);
        TestEqual(TEXT("missing actual"), R.Actual, TEXT("absent"));
    }

    // Absent: passes when nothing matches.
    {
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(
            MakeCond(EDriveConditionType::WidgetAbsent, TEXT("Nope")), Elements, FDriveJournalDelta());
        TestTrue(TEXT("absent is met when missing"), R.bMet);
        TestEqual(TEXT("absent actual"), R.Actual, TEXT("absent"));
        TestEqual(TEXT("absent expected"), R.Expected, TEXT("absent"));
    }

    // Absent: fails when present.
    {
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(
            MakeCond(EDriveConditionType::WidgetAbsent, TEXT("el_1")), Elements, FDriveJournalDelta());
        TestFalse(TEXT("absent fails when present"), R.bMet);
        TestEqual(TEXT("absent-but-present actual"), R.Actual, TEXT("present"));
    }

    return true;
}

// ============================================================================
// WidgetEnabled / WidgetVisible
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveCondWidgetStateTest,
    "PinWright.drive.condition.WidgetEnabledVisible",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveCondWidgetStateTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Elements;
    Elements.Add(MakeCondTestElement(TEXT("on"), TEXT("Play"), /*bEnabled*/ true, /*bVisible*/ true));
    Elements.Add(MakeCondTestElement(TEXT("off"), TEXT("Locked"), /*bEnabled*/ false, /*bVisible*/ false));

    // Enabled pass.
    {
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(
            MakeCond(EDriveConditionType::WidgetEnabled, TEXT("on")), Elements, FDriveJournalDelta());
        TestTrue(TEXT("enabled met"), R.bMet);
        TestEqual(TEXT("enabled actual"), R.Actual, TEXT("enabled"));
        TestEqual(TEXT("enabled expected"), R.Expected, TEXT("enabled"));
    }

    // Enabled fail (disabled element).
    {
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(
            MakeCond(EDriveConditionType::WidgetEnabled, TEXT("off")), Elements, FDriveJournalDelta());
        TestFalse(TEXT("disabled not met"), R.bMet);
        TestEqual(TEXT("disabled actual"), R.Actual, TEXT("disabled"));
    }

    // Enabled, but absent.
    {
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(
            MakeCond(EDriveConditionType::WidgetEnabled, TEXT("ghost")), Elements, FDriveJournalDelta());
        TestFalse(TEXT("absent not enabled"), R.bMet);
        TestEqual(TEXT("absent actual"), R.Actual, TEXT("absent"));
    }

    // Visible pass.
    {
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(
            MakeCond(EDriveConditionType::WidgetVisible, TEXT("on")), Elements, FDriveJournalDelta());
        TestTrue(TEXT("visible met"), R.bMet);
        TestEqual(TEXT("visible actual"), R.Actual, TEXT("visible"));
    }

    // Visible fail (hidden element).
    {
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(
            MakeCond(EDriveConditionType::WidgetVisible, TEXT("off")), Elements, FDriveJournalDelta());
        TestFalse(TEXT("hidden not met"), R.bMet);
        TestEqual(TEXT("hidden actual"), R.Actual, TEXT("hidden"));
    }

    return true;
}

// ============================================================================
// TextEquals / TextContains
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveCondTextTest,
    "PinWright.drive.condition.Text",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveCondTextTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Elements;
    Elements.Add(MakeCondTestElement(TEXT("score"), TEXT("Score: 100")));

    // TextEquals pass (against the element's label).
    {
        FDriveCondition C = MakeCond(EDriveConditionType::TextEquals, TEXT("score"));
        C.ExpectedText = TEXT("Score: 100");
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(C, Elements, FDriveJournalDelta());
        TestTrue(TEXT("text equals met"), R.bMet);
        TestEqual(TEXT("text equals actual"), R.Actual, TEXT("label=\"Score: 100\""));
        TestEqual(TEXT("text equals expected"), R.Expected, TEXT("label==\"Score: 100\""));
    }

    // TextEquals fail (different text + case sensitivity).
    {
        FDriveCondition C = MakeCond(EDriveConditionType::TextEquals, TEXT("score"));
        C.ExpectedText = TEXT("score: 100"); // lowercase 's' -> not equal (case-sensitive)
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(C, Elements, FDriveJournalDelta());
        TestFalse(TEXT("text equals is case-sensitive"), R.bMet);
    }

    // TextEquals against an absent widget.
    {
        FDriveCondition C = MakeCond(EDriveConditionType::TextEquals, TEXT("missing"));
        C.ExpectedText = TEXT("anything");
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(C, Elements, FDriveJournalDelta());
        TestFalse(TEXT("text equals absent not met"), R.bMet);
        TestEqual(TEXT("text equals absent actual"), R.Actual, TEXT("absent"));
    }

    // TextContains pass.
    {
        FDriveCondition C = MakeCond(EDriveConditionType::TextContains, TEXT("score"));
        C.ExpectedText = TEXT("100");
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(C, Elements, FDriveJournalDelta());
        TestTrue(TEXT("text contains met"), R.bMet);
    }

    // TextContains fail.
    {
        FDriveCondition C = MakeCond(EDriveConditionType::TextContains, TEXT("score"));
        C.ExpectedText = TEXT("999");
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(C, Elements, FDriveJournalDelta());
        TestFalse(TEXT("text contains miss not met"), R.bMet);
    }

    return true;
}

// ============================================================================
// Count (all six compare ops)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveCondCountTest,
    "PinWright.drive.condition.Count",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveCondCountTest::RunTest(const FString& Parameters)
{
    // Three elements with label "Item" (case-insensitive match) -> count == 3.
    TArray<FDriveElement> Elements;
    Elements.Add(MakeCondTestElement(TEXT("a"), TEXT("Item")));
    Elements.Add(MakeCondTestElement(TEXT("b"), TEXT("item")));
    Elements.Add(MakeCondTestElement(TEXT("c"), TEXT("ITEM")));
    Elements.Add(MakeCondTestElement(TEXT("d"), TEXT("Other")));

    auto Eval = [&Elements](EDriveCompareOp Op, int32 Expected) -> FDriveConditionResult
    {
        FDriveCondition C = MakeCond(EDriveConditionType::Count, TEXT("Item"));
        C.CountOp = Op;
        C.ExpectedCount = Expected;
        return FDriveConditionEval::Evaluate(C, Elements, FDriveJournalDelta());
    };

    // Sanity: Actual/Expected strings.
    {
        const FDriveConditionResult R = Eval(EDriveCompareOp::GreaterOrEqual, 1);
        TestEqual(TEXT("count actual"), R.Actual, TEXT("count=3"));
        TestEqual(TEXT("count expected"), R.Expected, TEXT(">= 1"));
    }

    // Equal
    TestTrue(TEXT("== 3 met"), Eval(EDriveCompareOp::Equal, 3).bMet);
    TestFalse(TEXT("== 2 not met"), Eval(EDriveCompareOp::Equal, 2).bMet);

    // NotEqual
    TestTrue(TEXT("!= 2 met"), Eval(EDriveCompareOp::NotEqual, 2).bMet);
    TestFalse(TEXT("!= 3 not met"), Eval(EDriveCompareOp::NotEqual, 3).bMet);

    // Less
    TestTrue(TEXT("< 4 met"), Eval(EDriveCompareOp::Less, 4).bMet);
    TestFalse(TEXT("< 3 not met"), Eval(EDriveCompareOp::Less, 3).bMet);

    // LessOrEqual
    TestTrue(TEXT("<= 3 met"), Eval(EDriveCompareOp::LessOrEqual, 3).bMet);
    TestFalse(TEXT("<= 2 not met"), Eval(EDriveCompareOp::LessOrEqual, 2).bMet);

    // Greater
    TestTrue(TEXT("> 2 met"), Eval(EDriveCompareOp::Greater, 2).bMet);
    TestFalse(TEXT("> 3 not met"), Eval(EDriveCompareOp::Greater, 3).bMet);

    // GreaterOrEqual
    TestTrue(TEXT(">= 3 met"), Eval(EDriveCompareOp::GreaterOrEqual, 3).bMet);
    TestFalse(TEXT(">= 4 not met"), Eval(EDriveCompareOp::GreaterOrEqual, 4).bMet);

    // Zero-match count.
    {
        FDriveCondition C = MakeCond(EDriveConditionType::Count, TEXT("Nope"));
        C.CountOp = EDriveCompareOp::Equal;
        C.ExpectedCount = 0;
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(C, Elements, FDriveJournalDelta());
        TestTrue(TEXT("count==0 for no matches"), R.bMet);
        TestEqual(TEXT("zero count actual"), R.Actual, TEXT("count=0"));
    }

    return true;
}

// ============================================================================
// GeometryInBounds (inside / outside / edge / unset bounds / absent)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveCondGeometryTest,
    "PinWright.drive.condition.Geometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveCondGeometryTest::RunTest(const FString& Parameters)
{
    const FBox2D Bounds(FVector2D(0.0, 0.0), FVector2D(100.0, 100.0));

    TArray<FDriveElement> Elements;
    // Fully inside: rect (10,10)..(30,30).
    Elements.Add(MakeCondTestElement(TEXT("inside"), TEXT("In"), true, true, FVector2D(10, 10), FVector2D(20, 20)));
    // Spills out: rect (90,90)..(140,140).
    Elements.Add(MakeCondTestElement(TEXT("outside"), TEXT("Out"), true, true, FVector2D(90, 90), FVector2D(50, 50)));
    // Exactly on the bounds: rect (0,0)..(100,100).
    Elements.Add(MakeCondTestElement(TEXT("edge"), TEXT("Edge"), true, true, FVector2D(0, 0), FVector2D(100, 100)));

    auto Eval = [&Elements, &Bounds](const FString& Target) -> FDriveConditionResult
    {
        FDriveCondition C = MakeCond(EDriveConditionType::GeometryInBounds, Target);
        C.ExpectedBounds = Bounds;
        return FDriveConditionEval::Evaluate(C, Elements, FDriveJournalDelta());
    };

    // Inside passes.
    {
        const FDriveConditionResult R = Eval(TEXT("inside"));
        TestTrue(TEXT("inside met"), R.bMet);
        TestTrue(TEXT("inside actual populated"), R.Actual.Contains(TEXT("(10,10)")));
        TestTrue(TEXT("inside expected populated"), R.Expected.Contains(TEXT("within")));
    }

    // Outside fails.
    {
        const FDriveConditionResult R = Eval(TEXT("outside"));
        TestFalse(TEXT("outside not met"), R.bMet);
    }

    // Edge is inclusive -> passes.
    {
        const FDriveConditionResult R = Eval(TEXT("edge"));
        TestTrue(TEXT("edge inclusive met"), R.bMet);
    }

    // Unset bounds -> not met with a clear detail.
    {
        FDriveCondition C = MakeCond(EDriveConditionType::GeometryInBounds, TEXT("inside"));
        // ExpectedBounds left default (invalid).
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(C, Elements, FDriveJournalDelta());
        TestFalse(TEXT("unset bounds not met"), R.bMet);
        TestTrue(TEXT("unset bounds detail"), R.Detail.Contains(TEXT("not set")));
    }

    // Absent target.
    {
        const FDriveConditionResult R = Eval(TEXT("ghost"));
        TestFalse(TEXT("geometry absent not met"), R.bMet);
        TestEqual(TEXT("geometry absent actual"), R.Actual, TEXT("absent"));
    }

    return true;
}

// ============================================================================
// JournalEvent
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveCondJournalEventTest,
    "PinWright.drive.condition.JournalEvent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveCondJournalEventTest::RunTest(const FString& Parameters)
{
    FDriveJournalDelta Delta;
    Delta.Events.Add(MakeCondTestEvent(TEXT("run_start"), TEXT("info")));
    Delta.Events.Add(MakeCondTestEvent(TEXT("checkpoint"), TEXT("debug")));

    const TArray<FDriveElement> NoElements;

    // Present.
    {
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(
            MakeCond(EDriveConditionType::JournalEvent, TEXT("run_start")), NoElements, Delta);
        TestTrue(TEXT("event present met"), R.bMet);
        TestEqual(TEXT("event present actual"), R.Actual, TEXT("present"));
    }

    // Absent.
    {
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(
            MakeCond(EDriveConditionType::JournalEvent, TEXT("run_end")), NoElements, Delta);
        TestFalse(TEXT("event absent not met"), R.bMet);
        TestEqual(TEXT("event absent actual"), R.Actual, TEXT("absent"));
    }

    return true;
}

// ============================================================================
// JournalSeverity (ordering boundaries + unknown threshold)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveCondJournalSeverityTest,
    "PinWright.drive.condition.JournalSeverity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveCondJournalSeverityTest::RunTest(const FString& Parameters)
{
    const TArray<FDriveElement> NoElements;

    auto EvalSeverity = [&NoElements](const TArray<FString>& Severities, const FString& Threshold) -> FDriveConditionResult
    {
        FDriveJournalDelta Delta;
        for (const FString& S : Severities)
        {
            Delta.Events.Add(MakeCondTestEvent(TEXT("evt"), S));
        }
        FDriveCondition C = MakeCond(EDriveConditionType::JournalSeverity, FString());
        C.SeverityThreshold = Threshold;
        return FDriveConditionEval::Evaluate(C, NoElements, Delta);
    };

    // Above threshold: error >= warning.
    {
        const FDriveConditionResult R = EvalSeverity({ TEXT("info"), TEXT("error") }, TEXT("warning"));
        TestTrue(TEXT("error >= warning met"), R.bMet);
        TestEqual(TEXT("severity actual is max"), R.Actual, TEXT("max=error"));
        TestEqual(TEXT("severity expected"), R.Expected, TEXT(">= warning"));
    }

    // Below threshold: info < warning.
    {
        const FDriveConditionResult R = EvalSeverity({ TEXT("info"), TEXT("debug") }, TEXT("warning"));
        TestFalse(TEXT("info < warning not met"), R.bMet);
        TestEqual(TEXT("max info"), R.Actual, TEXT("max=info"));
    }

    // Exact boundary: warning >= warning.
    {
        const FDriveConditionResult R = EvalSeverity({ TEXT("warning") }, TEXT("warning"));
        TestTrue(TEXT("warning >= warning met"), R.bMet);
    }

    // Lowest boundary: trace >= trace.
    {
        const FDriveConditionResult R = EvalSeverity({ TEXT("trace") }, TEXT("trace"));
        TestTrue(TEXT("trace >= trace met"), R.bMet);
    }

    // Highest: fatal >= error.
    {
        const FDriveConditionResult R = EvalSeverity({ TEXT("fatal") }, TEXT("error"));
        TestTrue(TEXT("fatal >= error met"), R.bMet);
    }

    // Case-insensitive severities and threshold.
    {
        const FDriveConditionResult R = EvalSeverity({ TEXT("ERROR") }, TEXT("Warning"));
        TestTrue(TEXT("case-insensitive severity comparison"), R.bMet);
    }

    // No events -> not met.
    {
        const FDriveConditionResult R = EvalSeverity({}, TEXT("info"));
        TestFalse(TEXT("no events not met"), R.bMet);
        TestEqual(TEXT("no events actual"), R.Actual, TEXT("max=<none>"));
    }

    // Unrecognized threshold -> not met.
    {
        const FDriveConditionResult R = EvalSeverity({ TEXT("error") }, TEXT("verbose"));
        TestFalse(TEXT("unknown threshold not met"), R.bMet);
        TestTrue(TEXT("unknown threshold detail"), R.Detail.Contains(TEXT("unrecognized")));
    }

    return true;
}

// ============================================================================
// SeverityRank helper (ordering)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveCondSeverityRankTest,
    "PinWright.drive.condition.SeverityRank",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveCondSeverityRankTest::RunTest(const FString& Parameters)
{
    const int32 Trace = FDriveConditionEval::SeverityRank(TEXT("trace"));
    const int32 Debug = FDriveConditionEval::SeverityRank(TEXT("debug"));
    const int32 Info = FDriveConditionEval::SeverityRank(TEXT("info"));
    const int32 Warning = FDriveConditionEval::SeverityRank(TEXT("warning"));
    const int32 Error = FDriveConditionEval::SeverityRank(TEXT("error"));
    const int32 Fatal = FDriveConditionEval::SeverityRank(TEXT("fatal"));

    TestTrue(TEXT("trace < debug"), Trace < Debug);
    TestTrue(TEXT("debug < info"), Debug < Info);
    TestTrue(TEXT("info < warning"), Info < Warning);
    TestTrue(TEXT("warning < error"), Warning < Error);
    TestTrue(TEXT("error < fatal"), Error < Fatal);

    TestEqual(TEXT("trace is 0"), Trace, 0);
    TestEqual(TEXT("fatal is 5"), Fatal, 5);

    // Case-insensitive.
    TestEqual(TEXT("ERROR rank case-insensitive"), FDriveConditionEval::SeverityRank(TEXT("ERROR")), Error);

    // Unknown token.
    TestEqual(TEXT("unknown token is INDEX_NONE"), FDriveConditionEval::SeverityRank(TEXT("verbose")), (int32)INDEX_NONE);

    return true;
}

// ============================================================================
// Element matching rule (Handle exact/case-sensitive, Label case-insensitive,
// empty target matches nothing)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveCondMatchingTest,
    "PinWright.drive.condition.Matching",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveCondMatchingTest::RunTest(const FString& Parameters)
{
    const FDriveElement Element = MakeCondTestElement(TEXT("el_1"), TEXT("Start"));

    // Handle: exact, case-sensitive.
    TestTrue(TEXT("handle exact match"), FDriveConditionEval::ElementMatchesTarget(Element, TEXT("el_1")));
    TestFalse(TEXT("handle is case-sensitive"), FDriveConditionEval::ElementMatchesTarget(Element, TEXT("EL_1")));

    // Label: case-insensitive.
    TestTrue(TEXT("label exact match"), FDriveConditionEval::ElementMatchesTarget(Element, TEXT("Start")));
    TestTrue(TEXT("label case-insensitive match"), FDriveConditionEval::ElementMatchesTarget(Element, TEXT("start")));

    // No match.
    TestFalse(TEXT("no match"), FDriveConditionEval::ElementMatchesTarget(Element, TEXT("Stop")));

    // Empty target matches nothing.
    TestFalse(TEXT("empty target matches nothing"), FDriveConditionEval::ElementMatchesTarget(Element, FString()));

    // An element with an empty handle still matches via its label, and an empty
    // target does not spuriously match the empty handle.
    const FDriveElement LabelOnly = MakeCondTestElement(FString(), TEXT("Play"));
    TestTrue(TEXT("label-only element matches via label"), FDriveConditionEval::ElementMatchesTarget(LabelOnly, TEXT("PLAY")));
    TestFalse(TEXT("empty target does not match empty handle"), FDriveConditionEval::ElementMatchesTarget(LabelOnly, FString()));

    return true;
}

// ============================================================================
// Actual/Expected always populated across condition types
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveCondActualExpectedTest,
    "PinWright.drive.condition.ActualExpectedPopulated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveCondActualExpectedTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Elements;
    Elements.Add(MakeCondTestElement(TEXT("btn"), TEXT("Go"), true, true, FVector2D(5, 5), FVector2D(10, 10)));

    FDriveJournalDelta Delta;
    Delta.Events.Add(MakeCondTestEvent(TEXT("ping"), TEXT("info")));

    const EDriveConditionType Types[] = {
        EDriveConditionType::WidgetPresent,
        EDriveConditionType::WidgetAbsent,
        EDriveConditionType::WidgetEnabled,
        EDriveConditionType::WidgetVisible,
        EDriveConditionType::TextEquals,
        EDriveConditionType::TextContains,
        EDriveConditionType::Count,
        EDriveConditionType::GeometryInBounds,
        EDriveConditionType::JournalEvent,
        EDriveConditionType::JournalSeverity
    };

    for (EDriveConditionType Type : Types)
    {
        FDriveCondition C = MakeCond(Type, TEXT("btn"));
        C.ExpectedText = TEXT("Go");
        C.ExpectedCount = 1;
        C.CountOp = EDriveCompareOp::GreaterOrEqual;
        C.ExpectedBounds = FBox2D(FVector2D(0, 0), FVector2D(100, 100));
        C.SeverityThreshold = TEXT("info");
        if (Type == EDriveConditionType::JournalEvent)
        {
            C.Target = TEXT("ping");
        }

        const FDriveConditionResult R = FDriveConditionEval::Evaluate(C, Elements, Delta);
        TestFalse(TEXT("Actual is non-empty"), R.Actual.IsEmpty());
        TestFalse(TEXT("Expected is non-empty"), R.Expected.IsEmpty());
    }

    return true;
}
