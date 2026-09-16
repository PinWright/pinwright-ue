// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for StepSettle -- the pure per-tick settle/wait decision function.
// Each test drives a sequence of ticks through a single FDriveSettleState and
// asserts the per-tick outcome plus the populated FDriveSettleResult fields.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveTypes.h"
#include "Handlers/Drive/DriveSettleDecision.h"

namespace
{
    // Convenience: an int view of the enum for TestEqual (matches the contract
    // test's static_cast style).
    int32 SettleDecisionOutcomeInt(EDriveSettleOutcome Outcome)
    {
        return static_cast<int32>(Outcome);
    }
}

// ============================================================================
// No wait_for: instant change then stable for StableTicks -> SettledChanged,
// with Continue states in between.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSettleSettledChangedTest,
    "PinWright.drive.settle.SettledChanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSettleSettledChangedTest::RunTest(const FString& Parameters)
{
    FDriveSettleConfig Config;
    Config.StableTicks = 2;
    Config.QuietBudgetMs = 500;
    Config.SettleBudgetMs = 1500;
    // No WaitFor.

    FDriveSettleState State;
    FDriveSettleResult R;

    // Tick 1 @0ms: UI changes this tick (not stable) -> in progress.
    EDriveSettleOutcome O1 = StepSettle(State, /*changed*/true, /*stable*/false,
        /*waitForMet*/false, /*elapsed*/0, Config, R);
    TestEqual(TEXT("tick1 Continue"), SettleDecisionOutcomeInt(O1), SettleDecisionOutcomeInt(EDriveSettleOutcome::Continue));
    TestTrue(TEXT("tick1 bChanged latched"), R.bChanged);
    TestFalse(TEXT("tick1 not settled"), R.bSettled);
    TestEqual(TEXT("tick1 Ticks"), R.Ticks, 1);

    // Tick 2 @50ms: stable since last tick -> 1 stable tick, still in progress.
    EDriveSettleOutcome O2 = StepSettle(State, /*changed*/false, /*stable*/true,
        /*waitForMet*/false, /*elapsed*/50, Config, R);
    TestEqual(TEXT("tick2 Continue"), SettleDecisionOutcomeInt(O2), SettleDecisionOutcomeInt(EDriveSettleOutcome::Continue));
    TestTrue(TEXT("tick2 bChanged still true"), R.bChanged);

    // Tick 3 @100ms: second consecutive stable tick reaches StableTicks=2.
    EDriveSettleOutcome O3 = StepSettle(State, /*changed*/false, /*stable*/true,
        /*waitForMet*/false, /*elapsed*/100, Config, R);
    TestEqual(TEXT("tick3 SettledChanged"), SettleDecisionOutcomeInt(O3), SettleDecisionOutcomeInt(EDriveSettleOutcome::SettledChanged));
    TestTrue(TEXT("settled bChanged"), R.bChanged);
    TestTrue(TEXT("settled bSettled"), R.bSettled);
    TestFalse(TEXT("settled bConditionMet false"), R.bConditionMet);
    TestEqual(TEXT("settled ElapsedMs"), R.ElapsedMs, 100);
    TestEqual(TEXT("settled Ticks"), R.Ticks, 3);

    return true;
}

// ============================================================================
// No wait_for: a change that never stabilizes within SettleBudget -> Timeout
// (bChanged=true, bSettled=false).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSettleChangeNeverStabilizesTest,
    "PinWright.drive.settle.ChangeNeverStabilizes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSettleChangeNeverStabilizesTest::RunTest(const FString& Parameters)
{
    FDriveSettleConfig Config;
    Config.StableTicks = 3;
    Config.QuietBudgetMs = 5000; // irrelevant once changed
    Config.SettleBudgetMs = 300;

    FDriveSettleState State;
    FDriveSettleResult R;

    // Every tick: still changing (never stable), elapsed climbing to the budget.
    EDriveSettleOutcome O1 = StepSettle(State, true, false, false, 0, Config, R);
    TestEqual(TEXT("tick1 Continue"), SettleDecisionOutcomeInt(O1), SettleDecisionOutcomeInt(EDriveSettleOutcome::Continue));

    EDriveSettleOutcome O2 = StepSettle(State, true, false, false, 100, Config, R);
    TestEqual(TEXT("tick2 Continue"), SettleDecisionOutcomeInt(O2), SettleDecisionOutcomeInt(EDriveSettleOutcome::Continue));

    EDriveSettleOutcome O3 = StepSettle(State, true, false, false, 200, Config, R);
    TestEqual(TEXT("tick3 Continue"), SettleDecisionOutcomeInt(O3), SettleDecisionOutcomeInt(EDriveSettleOutcome::Continue));

    // @300ms: budget reached, still not stabilized -> Timeout.
    EDriveSettleOutcome O4 = StepSettle(State, true, false, false, 300, Config, R);
    TestEqual(TEXT("tick4 Timeout"), SettleDecisionOutcomeInt(O4), SettleDecisionOutcomeInt(EDriveSettleOutcome::Timeout));
    TestTrue(TEXT("timeout reports bChanged"), R.bChanged);
    TestFalse(TEXT("timeout not settled"), R.bSettled);
    TestFalse(TEXT("timeout bConditionMet false"), R.bConditionMet);
    TestEqual(TEXT("timeout ElapsedMs"), R.ElapsedMs, 300);
    TestEqual(TEXT("timeout Ticks"), R.Ticks, 4);

    return true;
}

// ============================================================================
// No wait_for: no change within QuietBudget -> NoChangeWithinBudget
// (bChanged=false).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSettleNoChangeWithinBudgetTest,
    "PinWright.drive.settle.NoChangeWithinBudget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSettleNoChangeWithinBudgetTest::RunTest(const FString& Parameters)
{
    FDriveSettleConfig Config;
    Config.StableTicks = 2;
    Config.QuietBudgetMs = 400;
    Config.SettleBudgetMs = 1500;

    FDriveSettleState State;
    FDriveSettleResult R;

    // Nothing ever changes; stable every tick.
    EDriveSettleOutcome O1 = StepSettle(State, false, true, false, 0, Config, R);
    TestEqual(TEXT("tick1 Continue"), SettleDecisionOutcomeInt(O1), SettleDecisionOutcomeInt(EDriveSettleOutcome::Continue));
    TestFalse(TEXT("tick1 bChanged false"), R.bChanged);

    EDriveSettleOutcome O2 = StepSettle(State, false, true, false, 200, Config, R);
    TestEqual(TEXT("tick2 Continue"), SettleDecisionOutcomeInt(O2), SettleDecisionOutcomeInt(EDriveSettleOutcome::Continue));

    // @400ms: quiet budget reached with no change -> clean no-change.
    EDriveSettleOutcome O3 = StepSettle(State, false, true, false, 400, Config, R);
    TestEqual(TEXT("tick3 NoChangeWithinBudget"),
        SettleDecisionOutcomeInt(O3), SettleDecisionOutcomeInt(EDriveSettleOutcome::NoChangeWithinBudget));
    TestFalse(TEXT("no-change bChanged false"), R.bChanged);
    TestFalse(TEXT("no-change bSettled false"), R.bSettled);
    TestFalse(TEXT("no-change bConditionMet false"), R.bConditionMet);
    TestEqual(TEXT("no-change ElapsedMs"), R.ElapsedMs, 400);
    TestEqual(TEXT("no-change Ticks"), R.Ticks, 3);

    return true;
}

// ============================================================================
// wait_for: condition met before timeout -> WaitForMet (bConditionMet=true).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSettleWaitForMetTest,
    "PinWright.drive.settle.WaitForMet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSettleWaitForMetTest::RunTest(const FString& Parameters)
{
    FDriveSettleConfig Config;
    Config.WaitForTimeoutMs = 5000;
    FDriveCondition Cond;
    Cond.Type = EDriveConditionType::WidgetVisible;
    Cond.Target = TEXT("PlayButton");
    Config.WaitFor = Cond;

    FDriveSettleState State;
    FDriveSettleResult R;

    // Not met yet -> in progress (even though the UI happens to be changing,
    // change tracking does not drive the wait_for path).
    EDriveSettleOutcome O1 = StepSettle(State, true, false, /*waitForMet*/false, 0, Config, R);
    TestEqual(TEXT("tick1 Continue"), SettleDecisionOutcomeInt(O1), SettleDecisionOutcomeInt(EDriveSettleOutcome::Continue));
    TestFalse(TEXT("tick1 bConditionMet false"), R.bConditionMet);

    EDriveSettleOutcome O2 = StepSettle(State, false, true, false, 100, Config, R);
    TestEqual(TEXT("tick2 Continue"), SettleDecisionOutcomeInt(O2), SettleDecisionOutcomeInt(EDriveSettleOutcome::Continue));

    // @200ms: condition satisfied -> WaitForMet.
    EDriveSettleOutcome O3 = StepSettle(State, false, true, /*waitForMet*/true, 200, Config, R);
    TestEqual(TEXT("tick3 WaitForMet"), SettleDecisionOutcomeInt(O3), SettleDecisionOutcomeInt(EDriveSettleOutcome::WaitForMet));
    TestTrue(TEXT("waitfor bConditionMet true"), R.bConditionMet);
    TestFalse(TEXT("waitfor bSettled false"), R.bSettled);
    TestEqual(TEXT("waitfor ElapsedMs"), R.ElapsedMs, 200);
    TestEqual(TEXT("waitfor Ticks"), R.Ticks, 3);

    return true;
}

// ============================================================================
// wait_for: condition not met by the timeout -> Timeout (bConditionMet=false).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSettleWaitForTimeoutTest,
    "PinWright.drive.settle.WaitForTimeout",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSettleWaitForTimeoutTest::RunTest(const FString& Parameters)
{
    FDriveSettleConfig Config;
    Config.WaitForTimeoutMs = 1000;
    FDriveCondition Cond;
    Cond.Type = EDriveConditionType::WidgetPresent;
    Cond.Target = TEXT("NeverAppears");
    Config.WaitFor = Cond;

    FDriveSettleState State;
    FDriveSettleResult R;

    EDriveSettleOutcome O1 = StepSettle(State, false, true, false, 0, Config, R);
    TestEqual(TEXT("tick1 Continue"), SettleDecisionOutcomeInt(O1), SettleDecisionOutcomeInt(EDriveSettleOutcome::Continue));

    EDriveSettleOutcome O2 = StepSettle(State, false, true, false, 500, Config, R);
    TestEqual(TEXT("tick2 Continue"), SettleDecisionOutcomeInt(O2), SettleDecisionOutcomeInt(EDriveSettleOutcome::Continue));

    // @1000ms: timeout reached, condition still unmet -> Timeout.
    EDriveSettleOutcome O3 = StepSettle(State, false, true, false, 1000, Config, R);
    TestEqual(TEXT("tick3 Timeout"), SettleDecisionOutcomeInt(O3), SettleDecisionOutcomeInt(EDriveSettleOutcome::Timeout));
    TestFalse(TEXT("timeout bConditionMet false"), R.bConditionMet);
    TestFalse(TEXT("timeout bSettled false"), R.bSettled);
    TestEqual(TEXT("timeout ElapsedMs"), R.ElapsedMs, 1000);
    TestEqual(TEXT("timeout Ticks"), R.Ticks, 3);

    return true;
}

// ============================================================================
// No wait_for: a fresh change mid-settle resets the consecutive-stable counter,
// so settling requires StableTicks consecutive stable ticks after the LAST
// change.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSettleStableCountResetsOnChangeTest,
    "PinWright.drive.settle.StableCountResetsOnChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSettleStableCountResetsOnChangeTest::RunTest(const FString& Parameters)
{
    FDriveSettleConfig Config;
    Config.StableTicks = 2;
    Config.QuietBudgetMs = 5000;
    Config.SettleBudgetMs = 10000; // large so the timeout never fires here

    FDriveSettleState State;
    FDriveSettleResult R;

    // Change, then one stable tick (count = 1).
    StepSettle(State, true, false, false, 0, Config, R);
    EDriveSettleOutcome O2 = StepSettle(State, false, true, false, 50, Config, R);
    TestEqual(TEXT("tick2 Continue (1 stable)"), SettleDecisionOutcomeInt(O2), SettleDecisionOutcomeInt(EDriveSettleOutcome::Continue));

    // Fresh change resets the counter -> still in progress, NOT settled.
    EDriveSettleOutcome O3 = StepSettle(State, true, false, false, 100, Config, R);
    TestEqual(TEXT("tick3 Continue (reset)"), SettleDecisionOutcomeInt(O3), SettleDecisionOutcomeInt(EDriveSettleOutcome::Continue));
    TestFalse(TEXT("tick3 not settled after reset"), R.bSettled);

    // Now two consecutive stable ticks settle it.
    EDriveSettleOutcome O4 = StepSettle(State, false, true, false, 150, Config, R);
    TestEqual(TEXT("tick4 Continue (1 stable again)"), SettleDecisionOutcomeInt(O4), SettleDecisionOutcomeInt(EDriveSettleOutcome::Continue));

    EDriveSettleOutcome O5 = StepSettle(State, false, true, false, 200, Config, R);
    TestEqual(TEXT("tick5 SettledChanged"), SettleDecisionOutcomeInt(O5), SettleDecisionOutcomeInt(EDriveSettleOutcome::SettledChanged));
    TestTrue(TEXT("final bSettled"), R.bSettled);
    TestTrue(TEXT("final bChanged"), R.bChanged);
    TestEqual(TEXT("final Ticks"), R.Ticks, 5);

    return true;
}
