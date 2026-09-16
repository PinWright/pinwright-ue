// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for FDriveSettleDriver -- the async settle driver that wires the
// pure StepSettle decision to a sampled UI fingerprint. These tests drive the
// testable step (Tick) directly with a scripted GetElements sequence and injected
// times via CreateForStep(), so there is no real FTSTicker and no engine clock.
// The end-to-end FTSTicker + MakeAsyncToken path is covered by later integration.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveTypes.h"
#include "Handlers/Drive/DriveSettleDriver.h"

namespace
{
    int32 SettleDriverOutcomeInt(EDriveSettleOutcome Outcome)
    {
        return static_cast<int32>(Outcome);
    }

    // One visible element at a fixed rect. Distinct visible counts produce
    // distinct fingerprints, which is all the change detector needs here.
    FDriveElement MakeElement(const FString& Handle, float X)
    {
        FDriveElement E;
        E.Handle = Handle;
        E.Type = TEXT("Button");
        E.bVisible = true;
        E.AbsolutePosition = FVector2D(X, 0.0f);
        E.AbsoluteSize = FVector2D(40.0f, 20.0f);
        return E;
    }

    // Baseline shape: a single element.
    TArray<FDriveElement> MakeSetA()
    {
        return { MakeElement(TEXT("A"), 0.0f) };
    }

    // Changed shape: two elements (different visible count -> different fingerprint).
    TArray<FDriveElement> MakeSetB()
    {
        return { MakeElement(TEXT("A"), 0.0f), MakeElement(TEXT("B"), 60.0f) };
    }
}

// ============================================================================
// Instant change then stable -> OnComplete fires SettledChanged with the final
// (changed) elements, exactly once, and the driver reports complete (the ticker
// would stop).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSettleDriverSettledChangedTest,
    "PinWright.drive.settledriver.SettledChanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSettleDriverSettledChangedTest::RunTest(const FString& Parameters)
{
    FDriveSettleConfig Config;
    Config.StableTicks = 2;
    Config.QuietBudgetMs = 500;
    Config.SettleBudgetMs = 1500;
    // No WaitFor.

    // Call 0 (baseline) -> set A; every later call -> set B (changed then stable).
    int32 CallCount = 0;
    auto GetElements = [&CallCount]() -> TArray<FDriveElement>
    {
        const int32 N = CallCount++;
        return N == 0 ? MakeSetA() : MakeSetB();
    };

    int32 CompleteCount = 0;
    FDriveSettleResult CapturedResult;
    TArray<FDriveElement> CapturedFinal;
    auto OnComplete = [&](const FDriveSettleResult& Res, const TArray<FDriveElement>& Final)
    {
        ++CompleteCount;
        CapturedResult = Res;
        CapturedFinal = Final;
    };

    TSharedRef<FDriveSettleDriver> Driver = FDriveSettleDriver::CreateForStep(
        Config, GetElements, /*IsWaitForMet*/nullptr, OnComplete, /*StartSeconds*/0.0);

    // Tick 1 @0ms: B vs baseline A -> changed, not stable. In progress.
    TestTrue(TEXT("tick1 keep ticking"), Driver->Tick(0.0));
    TestFalse(TEXT("tick1 not complete"), Driver->IsComplete());
    TestEqual(TEXT("tick1 no completion yet"), CompleteCount, 0);

    // Tick 2 @50ms: B == B -> 1 stable tick. In progress.
    TestTrue(TEXT("tick2 keep ticking"), Driver->Tick(0.05));
    TestEqual(TEXT("tick2 no completion yet"), CompleteCount, 0);

    // Tick 3 @100ms: 2nd consecutive stable tick reaches StableTicks=2 -> settled.
    TestFalse(TEXT("tick3 ticker stops"), Driver->Tick(0.10));
    TestTrue(TEXT("tick3 complete"), Driver->IsComplete());

    TestEqual(TEXT("fired exactly once"), CompleteCount, 1);
    TestEqual(TEXT("outcome SettledChanged"),
        SettleDriverOutcomeInt(CapturedResult.Outcome),
        SettleDriverOutcomeInt(EDriveSettleOutcome::SettledChanged));
    TestTrue(TEXT("result bChanged"), CapturedResult.bChanged);
    TestTrue(TEXT("result bSettled"), CapturedResult.bSettled);
    TestFalse(TEXT("result bConditionMet false"), CapturedResult.bConditionMet);
    TestEqual(TEXT("result ElapsedMs"), CapturedResult.ElapsedMs, 100);
    TestEqual(TEXT("result Ticks"), CapturedResult.Ticks, 3);

    // Final elements are the latest sample (the changed set B), not the baseline.
    TestEqual(TEXT("final element count"), CapturedFinal.Num(), 2);

    // A stray tick after the terminal outcome must not re-fire OnComplete and must
    // keep the ticker stopped.
    TestFalse(TEXT("post-terminal tick stays stopped"), Driver->Tick(0.20));
    TestEqual(TEXT("still fired exactly once"), CompleteCount, 1);

    return true;
}

// ============================================================================
// No change -> OnComplete fires NoChangeWithinBudget once the quiet budget
// elapses, with the (unchanged) final elements.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSettleDriverNoChangeTest,
    "PinWright.drive.settledriver.NoChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSettleDriverNoChangeTest::RunTest(const FString& Parameters)
{
    FDriveSettleConfig Config;
    Config.StableTicks = 2;
    Config.QuietBudgetMs = 400;
    Config.SettleBudgetMs = 1500;
    // No WaitFor.

    // The UI never moves: every sample is the baseline shape.
    auto GetElements = []() -> TArray<FDriveElement> { return MakeSetA(); };

    int32 CompleteCount = 0;
    FDriveSettleResult CapturedResult;
    TArray<FDriveElement> CapturedFinal;
    auto OnComplete = [&](const FDriveSettleResult& Res, const TArray<FDriveElement>& Final)
    {
        ++CompleteCount;
        CapturedResult = Res;
        CapturedFinal = Final;
    };

    TSharedRef<FDriveSettleDriver> Driver = FDriveSettleDriver::CreateForStep(
        Config, GetElements, /*IsWaitForMet*/nullptr, OnComplete, /*StartSeconds*/0.0);

    // Below the quiet budget: keep ticking, no completion.
    TestTrue(TEXT("tick1 keep ticking"), Driver->Tick(0.0));
    TestTrue(TEXT("tick2 keep ticking"), Driver->Tick(0.2));
    TestEqual(TEXT("no completion before budget"), CompleteCount, 0);

    // @400ms: quiet budget reached with no change -> NoChangeWithinBudget.
    TestFalse(TEXT("tick3 ticker stops"), Driver->Tick(0.4));
    TestEqual(TEXT("fired exactly once"), CompleteCount, 1);
    TestEqual(TEXT("outcome NoChangeWithinBudget"),
        SettleDriverOutcomeInt(CapturedResult.Outcome),
        SettleDriverOutcomeInt(EDriveSettleOutcome::NoChangeWithinBudget));
    TestFalse(TEXT("result bChanged false"), CapturedResult.bChanged);
    TestFalse(TEXT("result bSettled false"), CapturedResult.bSettled);
    TestEqual(TEXT("result ElapsedMs"), CapturedResult.ElapsedMs, 400);
    TestEqual(TEXT("final element count"), CapturedFinal.Num(), 1);

    return true;
}

// ============================================================================
// wait_for met -> OnComplete fires WaitForMet (bConditionMet=true) on the tick
// the injected condition flips true.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSettleDriverWaitForMetTest,
    "PinWright.drive.settledriver.WaitForMet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSettleDriverWaitForMetTest::RunTest(const FString& Parameters)
{
    FDriveSettleConfig Config;
    Config.WaitForTimeoutMs = 5000;
    FDriveCondition Cond;
    Cond.Type = EDriveConditionType::WidgetVisible;
    Cond.Target = TEXT("PlayButton");
    Config.WaitFor = Cond;

    // UI is irrelevant to the wait_for path; keep it constant.
    auto GetElements = []() -> TArray<FDriveElement> { return MakeSetA(); };

    // Condition flips true after the first tick.
    bool bMet = false;
    auto IsWaitForMet = [&bMet]() -> bool { return bMet; };

    int32 CompleteCount = 0;
    FDriveSettleResult CapturedResult;
    auto OnComplete = [&](const FDriveSettleResult& Res, const TArray<FDriveElement>& /*Final*/)
    {
        ++CompleteCount;
        CapturedResult = Res;
    };

    TSharedRef<FDriveSettleDriver> Driver = FDriveSettleDriver::CreateForStep(
        Config, GetElements, IsWaitForMet, OnComplete, /*StartSeconds*/0.0);

    // @0ms: condition not met -> keep ticking.
    TestTrue(TEXT("tick1 keep ticking"), Driver->Tick(0.0));
    TestEqual(TEXT("no completion yet"), CompleteCount, 0);

    bMet = true;

    // @200ms: condition met -> WaitForMet.
    TestFalse(TEXT("tick2 ticker stops"), Driver->Tick(0.2));
    TestEqual(TEXT("fired exactly once"), CompleteCount, 1);
    TestEqual(TEXT("outcome WaitForMet"),
        SettleDriverOutcomeInt(CapturedResult.Outcome),
        SettleDriverOutcomeInt(EDriveSettleOutcome::WaitForMet));
    TestTrue(TEXT("result bConditionMet true"), CapturedResult.bConditionMet);
    TestFalse(TEXT("result bSettled false"), CapturedResult.bSettled);
    TestEqual(TEXT("result ElapsedMs"), CapturedResult.ElapsedMs, 200);

    return true;
}
