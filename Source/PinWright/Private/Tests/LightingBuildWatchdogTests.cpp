// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Level/LevelBuildBinds.h"

// Regression coverage for B-level-build-level-lighting-no-completion-signal.
//
// level.build_lighting (and its siblings lighting.build_lighting /
// level.build_all) bind completion via
// BindLightingBuildCompletion(). The three FEditorDelegates::OnLightingBuild*
// delegates are one-shot and in-memory: if their broadcast is ever missed
// (editor stall, GC/serialization deferral, kill+relaunch) the job hangs
// "running" forever. The fix adds a poll-based watchdog whose per-tick decision
// is StepBuildWatchdog(). These tests pin that decision logic — the engine's
// real FEditorBuildUtils::IsBuildCurrentlyRunning() flag is the only production
// input, modeled here as the bBuildRunning argument so the reconcile path is
// exercised deterministically without driving a real Lightmass bake.
//
// If the watchdog were reverted to a delegate-only bind, StepBuildWatchdog
// would not exist and these would fail to compile/link — the regression guard.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingWatchdogReconcilesOnMissedDelegateTest,
    "PinWright.Level.LightingWatchdog.ReconcilesOnMissedDelegate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLightingWatchdogReconcilesOnMissedDelegateTest::RunTest(const FString& Parameters)
{
    // Model the build lifecycle: not-yet-started -> running -> stopped, with the
    // completion delegate NEVER firing (the missed-broadcast case the ticket
    // reproduced). The watchdog must reach ResolveAndStop so the job terminates.
    bool bHasSeenBuildStart = false;
    int32 WarmupTicks = 0;

    // Tick 1: build hasn't started yet (warmup).
    TestTrue(TEXT("Pre-start tick keeps polling"),
        StepBuildWatchdog(/*bDelegateFired=*/false, /*bBuildRunning=*/false,
            bHasSeenBuildStart, WarmupTicks) == EBuildWatchdogAction::Continue);
    TestFalse(TEXT("Not seen build start yet"), bHasSeenBuildStart);

    // Tick 2: build is now running.
    TestTrue(TEXT("Running tick keeps polling"),
        StepBuildWatchdog(false, /*bBuildRunning=*/true,
            bHasSeenBuildStart, WarmupTicks) == EBuildWatchdogAction::Continue);
    TestTrue(TEXT("Build start observed"), bHasSeenBuildStart);

    // Tick 3: build has stopped and no delegate fired -> reconcile to terminal.
    TestTrue(TEXT("Build stopped with no delegate reconciles the job"),
        StepBuildWatchdog(false, /*bBuildRunning=*/false,
            bHasSeenBuildStart, WarmupTicks) == EBuildWatchdogAction::ResolveAndStop);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingWatchdogYieldsToDelegateTest,
    "PinWright.Level.LightingWatchdog.YieldsToDelegate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLightingWatchdogYieldsToDelegateTest::RunTest(const FString& Parameters)
{
    // Happy path: a completion delegate already fired. The watchdog must NOT
    // double-resolve — it stops without reconciling, leaving the delegate's
    // outcome authoritative (first-fires-wins).
    bool bHasSeenBuildStart = true; // build had been running
    int32 WarmupTicks = 0;

    TestTrue(TEXT("Delegate already fired stops the watchdog without resolving"),
        StepBuildWatchdog(/*bDelegateFired=*/true, /*bBuildRunning=*/false,
            bHasSeenBuildStart, WarmupTicks) == EBuildWatchdogAction::Stop);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingWatchdogGivesUpIfBuildNeverStartsTest,
    "PinWright.Level.LightingWatchdog.GivesUpIfBuildNeverStarts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLightingWatchdogGivesUpIfBuildNeverStartsTest::RunTest(const FString& Parameters)
{
    // If the build never starts, the watchdog must NOT reconcile (it would be a
    // false completion) and must eventually stop polling rather than spin
    // forever — the delegate binding remains to catch an instant completion.
    bool bHasSeenBuildStart = false;
    int32 WarmupTicks = 0;
    const int32 MaxWarmupTicks = 12;

    for (int32 i = 0; i < MaxWarmupTicks - 1; ++i)
    {
        const EBuildWatchdogAction A = StepBuildWatchdog(
            false, /*bBuildRunning=*/false, bHasSeenBuildStart, WarmupTicks, MaxWarmupTicks);
        TestTrue(FString::Printf(TEXT("Warmup tick %d keeps waiting"), i),
            A == EBuildWatchdogAction::Continue);
    }

    // The MaxWarmupTicks-th tick gives up (no false ResolveAndStop).
    TestTrue(TEXT("Warmup exhaustion stops without false reconcile"),
        StepBuildWatchdog(false, false, bHasSeenBuildStart, WarmupTicks, MaxWarmupTicks)
            == EBuildWatchdogAction::Stop);
    TestFalse(TEXT("Never observed a build start"), bHasSeenBuildStart);

    return true;
}
