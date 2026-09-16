// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for state management: FBlueprintTracker, FSaveThrottler, FPluginState
#include "Misc/AutomationTest.h"
#include "State/BlueprintTracker.h"
#include "State/SaveThrottler.h"
#include "State/PluginState.h"
#include "HAL/PlatformProcess.h"

// ============================================================================
// FBlueprintTracker - MarkInflight / IsInflight / ClearInflight
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintTrackerInflightCycleTest,
    "PinWright.infra.state.BlueprintTracker.InflightCycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintTrackerInflightCycleTest::RunTest(const FString& Parameters)
{
    FBlueprintTracker Tracker;

    TestFalse(TEXT("Initially not inflight"), Tracker.IsInflight(TEXT("BP_Test")));

    Tracker.MarkInflight(TEXT("BP_Test"), {TEXT("AssetA"), TEXT("AssetB")});
    TestTrue(TEXT("Now inflight after mark"), Tracker.IsInflight(TEXT("BP_Test")));
    TestFalse(TEXT("Other key not inflight"), Tracker.IsInflight(TEXT("BP_Other")));

    Tracker.ClearInflight(TEXT("BP_Test"));
    TestFalse(TEXT("No longer inflight after clear"), Tracker.IsInflight(TEXT("BP_Test")));

    return true;
}

// ============================================================================
// FBlueprintTracker - MarkBusy / IsBusy / ClearBusy
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintTrackerBusyCycleTest,
    "PinWright.infra.state.BlueprintTracker.BusyCycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintTrackerBusyCycleTest::RunTest(const FString& Parameters)
{
    FBlueprintTracker Tracker;

    TestFalse(TEXT("Initially not busy"), Tracker.IsBusy(TEXT("BP_Test")));

    Tracker.MarkBusy(TEXT("BP_Test"));
    TestTrue(TEXT("Busy after mark"), Tracker.IsBusy(TEXT("BP_Test")));
    TestFalse(TEXT("Other key not busy"), Tracker.IsBusy(TEXT("BP_Other")));

    Tracker.ClearBusy(TEXT("BP_Test"));
    TestFalse(TEXT("Not busy after clear"), Tracker.IsBusy(TEXT("BP_Test")));

    return true;
}

// ============================================================================
// FBlueprintTracker - IsCreateStale returns true after timeout
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintTrackerCreateStaleTest,
    "PinWright.infra.state.BlueprintTracker.IsCreateStale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintTrackerCreateStaleTest::RunTest(const FString& Parameters)
{
    FBlueprintTracker Tracker;
    // Set a very short timeout for testing (0.05 seconds)
    Tracker.StaleTimeoutRef() = 0.05;

    Tracker.MarkCreateInflight(TEXT("BP_Test"));
    TestTrue(TEXT("CreateInflight is marked"), Tracker.IsCreateInflight(TEXT("BP_Test")));

    // Immediately after marking, should NOT be stale yet
    TestFalse(TEXT("Not stale immediately"), Tracker.IsCreateStale(TEXT("BP_Test")));

    // Wait a bit longer than the timeout
    FPlatformProcess::Sleep(0.1f);

    // Now it should be stale
    TestTrue(TEXT("Stale after timeout"), Tracker.IsCreateStale(TEXT("BP_Test")));

    // Clean up
    Tracker.ClearCreateInflight(TEXT("BP_Test"));
    TestFalse(TEXT("Not inflight after clear"), Tracker.IsCreateInflight(TEXT("BP_Test")));
    TestFalse(TEXT("Not stale after clear"), Tracker.IsCreateStale(TEXT("BP_Test")));

    return true;
}

// ============================================================================
// FBlueprintTracker - RegisterBlueprint / FindBlueprintEntry
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintTrackerRegistryTest,
    "PinWright.infra.state.BlueprintTracker.RegistryLookup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintTrackerRegistryTest::RunTest(const FString& Parameters)
{
    FBlueprintTracker Tracker;

    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("path"), TEXT("/Game/BP_Test"));

    Tracker.RegisterBlueprint(TEXT("BP_Test"), Entry);

    TSharedPtr<FJsonObject> Found = Tracker.FindBlueprintEntry(TEXT("BP_Test"));
    TestTrue(TEXT("Entry found"), Found.IsValid());
    if (Found.IsValid())
    {
        TestEqual(TEXT("Entry path matches"),
            Found->GetStringField(TEXT("path")), TEXT("/Game/BP_Test"));
    }

    // Non-existent key
    TSharedPtr<FJsonObject> Missing = Tracker.FindBlueprintEntry(TEXT("BP_Other"));
    TestFalse(TEXT("Missing entry returns null"), Missing.IsValid());

    return true;
}

// ============================================================================
// FSaveThrottler - ShouldSave returns true after throttle period
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveThrottlerAllowsAfterPeriodTest,
    "PinWright.infra.state.SaveThrottler.AllowsAfterPeriod",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSaveThrottlerAllowsAfterPeriodTest::RunTest(const FString& Parameters)
{
    FSaveThrottler Throttler;
    // Set a very short throttle period for testing
    Throttler.ThrottleSecondsRef() = 0.05;

    // Never saved before -- should allow
    TestTrue(TEXT("ShouldSave true for new asset"), Throttler.ShouldSave(TEXT("/Game/Asset")));

    // Record a save
    Throttler.RecordSave(TEXT("/Game/Asset"));

    // Immediately after -- should be throttled
    TestFalse(TEXT("ShouldSave false immediately after save"),
        Throttler.ShouldSave(TEXT("/Game/Asset")));

    // Wait past the throttle period
    FPlatformProcess::Sleep(0.1f);

    // Now should allow again
    TestTrue(TEXT("ShouldSave true after throttle expires"),
        Throttler.ShouldSave(TEXT("/Game/Asset")));

    return true;
}

// ============================================================================
// FSaveThrottler - ShouldSave returns false within throttle period
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveThrottlerBlocksWithinPeriodTest,
    "PinWright.infra.state.SaveThrottler.BlocksWithinPeriod",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSaveThrottlerBlocksWithinPeriodTest::RunTest(const FString& Parameters)
{
    FSaveThrottler Throttler;
    Throttler.ThrottleSecondsRef() = 5.0; // Long period

    Throttler.RecordSave(TEXT("/Game/Heavy"));

    // Should be blocked within the throttle period
    TestFalse(TEXT("Blocked within throttle window"),
        Throttler.ShouldSave(TEXT("/Game/Heavy")));

    // Different asset should not be affected
    TestTrue(TEXT("Different asset not throttled"),
        Throttler.ShouldSave(TEXT("/Game/Other")));

    return true;
}

// ============================================================================
// FSaveThrottler - TrySave
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveThrottlerTrySaveTest,
    "PinWright.infra.state.SaveThrottler.TrySave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSaveThrottlerTrySaveTest::RunTest(const FString& Parameters)
{
    FSaveThrottler Throttler;
    Throttler.ThrottleSecondsRef() = 5.0;

    int32 SaveCount = 0;

    // First TrySave should invoke the lambda
    bool bFirst = Throttler.TrySave(TEXT("/Game/A"), [&SaveCount]() -> bool
    {
        SaveCount++;
        return true;
    });
    TestTrue(TEXT("First TrySave succeeds"), bFirst);
    TestEqual(TEXT("Lambda called once"), SaveCount, 1);

    // Second immediate TrySave should be throttled (no lambda call)
    bool bSecond = Throttler.TrySave(TEXT("/Game/A"), [&SaveCount]() -> bool
    {
        SaveCount++;
        return true;
    });
    TestTrue(TEXT("Throttled TrySave returns true (throttled = success)"), bSecond);
    TestEqual(TEXT("Lambda not called second time"), SaveCount, 1);

    return true;
}

// ============================================================================
// FPluginState::Get() returns same instance (singleton)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPluginStateSingletonTest,
    "PinWright.infra.state.PluginState.Singleton",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPluginStateSingletonTest::RunTest(const FString& Parameters)
{
    FPluginState& A = FPluginState::Get();
    FPluginState& B = FPluginState::Get();
    TestTrue(TEXT("Singleton returns same address"), &A == &B);
    return true;
}

// ============================================================================
// FPluginState provides access to sub-state objects
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPluginStateSubStateAccessTest,
    "PinWright.infra.state.PluginState.SubStateAccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPluginStateSubStateAccessTest::RunTest(const FString& Parameters)
{
    FPluginState& State = FPluginState::Get();

    // Verify sub-state references are valid (non-crashing)
    FBlueprintTracker& BP = State.Blueprints();
    FSaveThrottler& ST = State.SaveThrottle();

    // Use the references to avoid unused-variable warnings
    (void)BP.IsBusy(TEXT("_test_noop"));
    (void)ST.ShouldSave(TEXT("_test_noop"));

    TestTrue(TEXT("Blueprint tracker accessible"), true);
    TestTrue(TEXT("Save throttler accessible"), true);

    return true;
}

// ============================================================================
// FSaveThrottler - TrySave with failed save lambda
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveThrottlerTrySaveFailedTest,
    "PinWright.infra.state.SaveThrottler.TrySaveFailedSave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSaveThrottlerTrySaveFailedTest::RunTest(const FString& Parameters)
{
    FSaveThrottler Throttler;
    Throttler.ThrottleSecondsRef() = 5.0;

    bool bResult = Throttler.TrySave(TEXT("/Game/Fail"), []() -> bool
    {
        return false; // Save failed
    });

    TestFalse(TEXT("TrySave returns false when save fails"), bResult);

    // Because save failed, timestamp should NOT be recorded — next TrySave should invoke lambda
    int32 CallCount = 0;
    Throttler.TrySave(TEXT("/Game/Fail"), [&CallCount]() -> bool
    {
        CallCount++;
        return true;
    });
    TestEqual(TEXT("Lambda called again (not throttled)"), CallCount, 1);

    return true;
}

// ============================================================================
// FBlueprintTracker - MarkCreateInflight idempotency
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintTrackerCreateInflightIdempotencyTest,
    "PinWright.infra.state.BlueprintTracker.CreateInflightIdempotency",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintTrackerCreateInflightIdempotencyTest::RunTest(const FString& Parameters)
{
    FBlueprintTracker Tracker;
    Tracker.StaleTimeoutRef() = 10.0;

    // First call creates inflight entry, returns false (not already subscribed)
    const double Now = FPlatformTime::Seconds();
    const bool bSubscribed1 = Tracker.SubscribeOrCreateInflight(TEXT("BP_Test"), TEXT("req1"), Now);
    TestFalse(TEXT("First call creates new inflight"), bSubscribed1);

    FPlatformProcess::Sleep(0.05f);

    // Second call should subscribe to existing entry, returns true
    const double Later = FPlatformTime::Seconds();
    const bool bSubscribed2 = Tracker.SubscribeOrCreateInflight(TEXT("BP_Test"), TEXT("req2"), Later);
    TestTrue(TEXT("Second call subscribes to existing"), bSubscribed2);

    // Verify IsCreateInflight is still true
    TestTrue(TEXT("Key still inflight"), Tracker.IsCreateInflight(TEXT("BP_Test")));

    // Verify it's NOT stale (timestamp was set at 'Now', not 'Later')
    TestFalse(TEXT("Not stale yet"), Tracker.IsCreateStale(TEXT("BP_Test")));

    // Drain subscribers and verify we get both request IDs
    TArray<FString> Subscribers = Tracker.DrainCreateInflightSubscribers(TEXT("BP_Test"));
    TestEqual(TEXT("Two subscribers drained"), Subscribers.Num(), 2);
    TestTrue(TEXT("Contains req1"), Subscribers.Contains(TEXT("req1")));
    TestTrue(TEXT("Contains req2"), Subscribers.Contains(TEXT("req2")));

    // After drain, key should no longer be inflight
    TestFalse(TEXT("Key cleared after drain"), Tracker.IsCreateInflight(TEXT("BP_Test")));

    return true;
}

// ============================================================================
// FPluginState - SequenceRegistry store/retrieve
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPluginStateSequenceRegistryTest,
    "PinWright.infra.state.PluginState.SequenceRegistry.StoreRetrieve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPluginStateSequenceRegistryTest::RunTest(const FString& Parameters)
{
    FPluginState& State = FPluginState::Get();
    TMap<FString, TSharedPtr<FJsonObject>>& Reg = State.SequenceRegistry();

    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("path"), TEXT("/Game/Seq_Test"));
    Reg.Add(TEXT("_test_seq_key"), Entry);

    TSharedPtr<FJsonObject>* Found = Reg.Find(TEXT("_test_seq_key"));
    TestTrue(TEXT("Entry found in SequenceRegistry"), Found != nullptr && Found->IsValid());
    if (Found && Found->IsValid())
    {
        TestEqual(TEXT("Path matches"),
            (*Found)->GetStringField(TEXT("path")), TEXT("/Game/Seq_Test"));
    }

    // Cleanup
    Reg.Remove(TEXT("_test_seq_key"));
    return true;
}

// ============================================================================
// FPluginState - NiagaraRegistry store/retrieve
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPluginStateNiagaraRegistryTest,
    "PinWright.infra.state.PluginState.NiagaraRegistry.StoreRetrieve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPluginStateNiagaraRegistryTest::RunTest(const FString& Parameters)
{
    FPluginState& State = FPluginState::Get();
    TMap<FString, TSharedPtr<FJsonObject>>& Reg = State.NiagaraRegistry();

    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("system"), TEXT("NS_TestSystem"));
    Reg.Add(TEXT("_test_niagara_key"), Entry);

    TSharedPtr<FJsonObject>* Found = Reg.Find(TEXT("_test_niagara_key"));
    TestTrue(TEXT("Entry found in NiagaraRegistry"), Found != nullptr && Found->IsValid());
    if (Found && Found->IsValid())
    {
        TestEqual(TEXT("System matches"),
            (*Found)->GetStringField(TEXT("system")), TEXT("NS_TestSystem"));
    }

    // Cleanup
    Reg.Remove(TEXT("_test_niagara_key"));
    return true;
}

// ============================================================================
// FPluginState - CurrentSequencePath set/get
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPluginStateCurrentSequencePathTest,
    "PinWright.infra.state.PluginState.CurrentSequencePath.SetGet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPluginStateCurrentSequencePathTest::RunTest(const FString& Parameters)
{
    FPluginState& State = FPluginState::Get();
    FString OldPath = State.CurrentSequencePath();

    State.CurrentSequencePath() = TEXT("/Game/TestSequence");
    TestEqual(TEXT("Path updated"), State.CurrentSequencePath(), TEXT("/Game/TestSequence"));

    // Restore
    State.CurrentSequencePath() = OldPath;
    return true;
}
