// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "ScopedTransaction.h"
#include "Selection.h"
#include "Tests/AutomationSuiteMaintenance.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#if WITH_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAutomationSuiteMaintenancePeriodicResetTest,
    "PinWright.infra.contract.SuiteMaintenance.PeriodicResetRunsOnSchedule",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAutomationSuiteMaintenancePeriodicResetTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    TestTrue(TEXT("Editor transaction buffer is available"), GEditor != nullptr && GEditor->Trans != nullptr);
    if (!GEditor || !GEditor->Trans)
    {
        return false;
    }

    PinWrightSuiteMaintenance::FStats& Stats = PinWrightSuiteMaintenance::GetStats();
    const PinWrightSuiteMaintenance::FStats Snapshot = Stats;
    ON_SCOPE_EXIT
    {
        Stats = Snapshot;
    };

    Stats.TestsEnded = 0;
    Stats.Resets = 0;
    Stats.LastResetAtTest = 0;
    // Scheduling reads TestsSinceReset, not TestsEnded, so a carried-over value from the real
    // hook makes the first Advance below fire immediately.
    Stats.TestsSinceReset = 0;

    PinWrightSuiteMaintenance::FScopedInterval ScopedInterval(3);
    // Pin the memory trigger out of the way: this contract is about the COUNT schedule, and the
    // host's real working set must not be able to decide it.
    PinWrightSuiteMaintenance::FScopedWatermark ScopedWatermark(0.0f, 0.0f);
    const int32 ResetsBefore = Stats.Resets;

    TestFalse(TEXT("First test in interval does not reset"),
        PinWrightSuiteMaintenance::AdvanceAndShouldReset());
    TestFalse(TEXT("Second test in interval does not reset"),
        PinWrightSuiteMaintenance::AdvanceAndShouldReset());
    TestTrue(TEXT("Third test in interval resets"),
        PinWrightSuiteMaintenance::AdvanceAndShouldReset());

    UCurveFloat* Fixture = NewObject<UCurveFloat>(
        GetTransientPackage(), NAME_None, RF_Transient | RF_Transactional);
    TestNotNull(TEXT("Transient transactional fixture created"), Fixture);
    if (Fixture)
    {
        {
            FScopedTransaction Transaction(NSLOCTEXT(
                "PinWright", "SuiteMaintenanceTestTransaction", "PinWright suite maintenance test"));
            Fixture->Modify();
            Fixture->FloatCurve.AddKey(0.0f, 1.0f);
        }

        TestTrue(TEXT("Transaction was recorded before reset"), GEditor->Trans->GetQueueLength() > 0);
    }

    PinWrightSuiteMaintenance::RunResetNow();
    TestEqual(TEXT("Reset count increments"), Stats.Resets, ResetsBefore + 1);
    TestEqual(TEXT("Reset records the current test count"), Stats.LastResetAtTest, Stats.TestsEnded);
    TestEqual(TEXT("Reset clears the editor transaction queue"), GEditor->Trans->GetQueueLength(), 0);

    const bool bMaintenanceRegistered = PinWrightSuiteMaintenance::IsRegistered();
    TestTrue(TEXT("PinWright maintenance hook is registered"), bMaintenanceRegistered);

    const bool bHookBound = FAutomationTestFramework::Get().OnTestEndEvent.IsBound();
    TestTrue(TEXT("Automation test-end event is bound"), bHookBound);
    TestTrue(
        TEXT("Maintenance hook ran for an earlier test or remains bound"),
        Snapshot.TestsEnded > 0 || bHookBound);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAutomationSuiteMaintenanceWatermarkTriggerTest,
    "PinWright.infra.contract.SuiteMaintenance.ResetTriggersOnCountOrWatermark",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAutomationSuiteMaintenanceWatermarkTriggerTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    PinWrightSuiteMaintenance::FStats& Stats = PinWrightSuiteMaintenance::GetStats();
    const PinWrightSuiteMaintenance::FStats Snapshot = Stats;
    ON_SCOPE_EXIT
    {
        Stats = Snapshot;
    };

    // 64 GiB of "physical RAM", so the watermark fractions below name round working sets.
    const uint64 Total = 64ull * 1024ull * 1024ull * 1024ull;
    const uint64 Quiet = static_cast<uint64>(Total * 0.20);
    const uint64 Pressured = static_cast<uint64>(Total * 0.60);

    auto Advance = [](int32 Times)
    {
        bool bLast = false;
        for (int32 Index = 0; Index < Times; ++Index)
        {
            bLast = PinWrightSuiteMaintenance::AdvanceAndShouldReset();
        }
        return bLast;
    };

    // Count trigger: the watermark is nowhere near, so only the interval can fire.
    {
        PinWrightSuiteMaintenance::FScopedInterval ScopedInterval(3);
        PinWrightSuiteMaintenance::FScopedWatermark ScopedWatermark(0.55f, 0.75f);
        PinWrightSuiteMaintenance::FScopedMemorySample ScopedMemory(Quiet, Total);
        Stats.TestsSinceReset = 0;

        PinWrightSuiteMaintenance::EResetTrigger Trigger =
            PinWrightSuiteMaintenance::EResetTrigger::Watermark;
        TestFalse(TEXT("Below the interval and below the watermark, no reset is due"),
            Advance(2));
        TestTrue(TEXT("Reaching the interval is due"),
            PinWrightSuiteMaintenance::AdvanceAndShouldReset(&Trigger));
        TestTrue(TEXT("The interval reports the count trigger"),
            Trigger == PinWrightSuiteMaintenance::EResetTrigger::Count);
    }

    // Watermark trigger: the interval is far away, so a reset can only come from memory. The
    // min-gap floor the shared predicate applies is what the 24-vs-25 pair pins.
    {
        PinWrightSuiteMaintenance::FScopedInterval ScopedInterval(10000);
        PinWrightSuiteMaintenance::FScopedWatermark ScopedWatermark(0.55f, 0.75f);
        PinWrightSuiteMaintenance::FScopedMemorySample ScopedMemory(Pressured, Total);
        Stats.TestsSinceReset = 0;

        TestFalse(TEXT("Over the watermark but under the min-gap floor, no reset is due"),
            Advance(AssetDumpHandler::DumpReleaseMinAssetsBetweenSteps - 1));

        PinWrightSuiteMaintenance::EResetTrigger Trigger =
            PinWrightSuiteMaintenance::EResetTrigger::Count;
        TestTrue(TEXT("Over the watermark and past the floor, a reset is due"),
            PinWrightSuiteMaintenance::AdvanceAndShouldReset(&Trigger));
        TestTrue(TEXT("Memory pressure reports the watermark trigger"),
            Trigger == PinWrightSuiteMaintenance::EResetTrigger::Watermark);
    }

    // Watermark disabled: the same pressure must produce nothing when the fraction is zero.
    {
        PinWrightSuiteMaintenance::FScopedInterval ScopedInterval(10000);
        PinWrightSuiteMaintenance::FScopedWatermark ScopedWatermark(0.0f, 0.0f);
        PinWrightSuiteMaintenance::FScopedMemorySample ScopedMemory(Pressured, Total);
        Stats.TestsSinceReset = 0;

        TestFalse(TEXT("A zero watermark disables the memory trigger"),
            Advance(AssetDumpHandler::DumpReleaseMinAssetsBetweenSteps + 5));
    }

    // The escalation rule itself, with no reset and no editor state involved.
    TestTrue(TEXT("At the hard fraction the reset is reported as having failed"),
        PinWrightSuiteMaintenance::ShouldEscalateAfterReset(
            static_cast<uint64>(Total * 0.75), Total, 0.75f));
    TestFalse(TEXT("Below the hard fraction nothing escalates"),
        PinWrightSuiteMaintenance::ShouldEscalateAfterReset(
            static_cast<uint64>(Total * 0.74), Total, 0.75f));
    TestFalse(TEXT("A zero hard fraction disables the escalation"),
        PinWrightSuiteMaintenance::ShouldEscalateAfterReset(Total, Total, 0.0f));
    TestFalse(TEXT("An unreadable total physical size cannot escalate"),
        PinWrightSuiteMaintenance::ShouldEscalateAfterReset(Total, 0, 0.75f));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAutomationSuiteMaintenanceResetBookkeepingTest,
    "PinWright.infra.contract.SuiteMaintenance.ResetRecordsItsTriggerAndRestartsTheCount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAutomationSuiteMaintenanceResetBookkeepingTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    PinWrightSuiteMaintenance::FStats& Stats = PinWrightSuiteMaintenance::GetStats();
    const PinWrightSuiteMaintenance::FStats Snapshot = Stats;
    ON_SCOPE_EXIT
    {
        Stats = Snapshot;
    };

    const uint64 Total = 64ull * 1024ull * 1024ull * 1024ull;
    const uint64 Stuck = static_cast<uint64>(Total * 0.90);

    const int32 EscalationsBefore = Stats.HardFractionEscalations;
    const int32 WatermarkResetsBefore = Stats.WatermarkResets;

    {
        // Hard fraction pinned to 0 so this reset CANNOT escalate. That is a requirement, not a
        // convenience: RunResetNow's escalation writes the real PINWRIGHT_MEMORY_WATERMARK_EXCEEDED
        // token, check_suite_log.py greps the whole suite log for it, and a test that emitted one
        // would classify every run COMPLETED_WITH_MEMORY_PRESSURE. The escalation branch is
        // therefore covered by the pure predicate above and by the structural contract below.
        PinWrightSuiteMaintenance::FScopedWatermark ScopedWatermark(0.55f, 0.0f);
        PinWrightSuiteMaintenance::FScopedMemorySample ScopedMemory(Stuck, Total);
        Stats.TestsSinceReset = 7;

        PinWrightSuiteMaintenance::RunResetNow(
            PinWrightSuiteMaintenance::EResetTrigger::Watermark);
    }

    TestEqual(TEXT("A watermark-triggered reset is counted as one"),
        Stats.WatermarkResets, WatermarkResetsBefore + 1);
    TestEqual(TEXT("A reset restarts the per-interval test count"), Stats.TestsSinceReset, 0);
    TestEqual(TEXT("A zero hard fraction escalates nothing however full the host is"),
        Stats.HardFractionEscalations, EscalationsBefore);

    return true;
}

// The periodic reset only reclaims what the per-test teardown detached, so its safety depends on
// the teardown leaving nothing behind that a later GC can turn into a dangling raw pointer. The two
// tests below pin the one such hazard the wave-11 discard path introduced: a fixture left selected
// keeps an external FTypedElementHandle whose FObjectElementData::Object is a raw UObject* GC never
// nulls, and the end-of-frame drain that destroys that element dereferences it for the leak warning
// long after the object was freed. Named-namespace helper so the Unity build cannot merge it with
// the identically-shaped extractors in the neighbouring teardown ratchets.
namespace SuiteMaintenanceElementHygieneHelpers
{
    bool ExtractHelperBody(
        const FString& Source, const FString& SignatureText, FString& OutBody)
    {
        const FString Neutralized = NeutralizeSourceText(Source);
        const int32 Signature = Neutralized.Find(SignatureText);
        const int32 OpenBrace = Signature == INDEX_NONE
            ? INDEX_NONE
            : Neutralized.Find(TEXT("{"), ESearchCase::CaseSensitive,
                ESearchDir::FromStart, Signature);
        if (OpenBrace == INDEX_NONE)
        {
            return false;
        }

        int32 Depth = 0;
        for (int32 Index = OpenBrace; Index < Neutralized.Len(); ++Index)
        {
            if (Neutralized[Index] == TEXT('{'))
            {
                ++Depth;
            }
            else if (Neutralized[Index] == TEXT('}') && --Depth == 0)
            {
                OutBody = Neutralized.Mid(OpenBrace + 1, Index - OpenBrace - 1);
                return true;
            }
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAutomationSuiteMaintenanceDiscardDeselectsFirstTest,
    "PinWright.infra.contract.SuiteMaintenance.DiscardDeselectsBeforeDetachingFixture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAutomationSuiteMaintenanceDiscardDeselectsFirstTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    if (!TestTrue(TEXT("PinWright plugin resolved for the discard ordering contract"),
            Plugin.IsValid()))
    {
        return false;
    }

    FString TeardownSource;
    const FString TeardownHeader = Plugin->GetBaseDir()
        / TEXT("Source/PinWright/Private/Tests/TestAssetTeardown.h");
    if (!TestTrue(TEXT("Read TestAssetTeardown.h for the discard ordering contract"),
            FFileHelper::LoadFileToString(TeardownSource, *TeardownHeader)))
    {
        return false;
    }

    FString CoreBody;
    if (!TestTrue(TEXT("Located DiscardLoadedAssetNoGc implementation"),
            SuiteMaintenanceElementHygieneHelpers::ExtractHelperBody(
                TeardownSource, TEXT("inline UPackage* DiscardLoadedAssetNoGc(UObject* Asset)"),
                CoreBody)))
    {
        return false;
    }

    const int32 SelectionAccess = CoreBody.Find(TEXT("GEditor->GetSelectedObjects()"));
    const int32 Deselect = CoreBody.Find(TEXT("Deselect(Asset)"));
    const int32 ClearFlags = CoreBody.Find(TEXT("Asset->ClearFlags(RF_Public | RF_Standalone)"));
    const int32 RemoveFromRoot = CoreBody.Find(TEXT("Asset->RemoveFromRoot()"));
    const int32 AssetRename = CoreBody.Find(TEXT("const bool bAssetRenamed = Asset->Rename"));

    TestTrue(TEXT("Discard reads the editor object selection"), SelectionAccess != INDEX_NONE);
    TestTrue(TEXT("Discard deselects the asset it is about to detach"), Deselect != INDEX_NONE);
    TestTrue(
        TEXT("Discard deselects BEFORE it detaches and renames the asset"),
        Deselect != INDEX_NONE && SelectionAccess < Deselect
            && ClearFlags > Deselect && RemoveFromRoot > Deselect && AssetRename > Deselect);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAutomationSuiteMaintenanceDiscardDropsSelectionHandleTest,
    "PinWright.infra.contract.SuiteMaintenance.DiscardLeavesNoSelectionHandle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAutomationSuiteMaintenanceDiscardDropsSelectionHandleTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    USelection* SelectedObjects = GEditor ? GEditor->GetSelectedObjects() : nullptr;
    if (SelectedObjects == nullptr || SelectedObjects->GetElementSelectionSet() == nullptr)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-object-selection-unavailable"),
            TEXT("GEditor->GetSelectedObjects() has no element selection set on this host."));
        return true;
    }

    const FString AssetName = FString::Printf(
        TEXT("Curve_SuiteMaintenanceSelection_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = TEXT("/Game/__PW_SuiteMaintenanceTests/") + AssetName;

    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("fixture package created"), Package))
    {
        return false;
    }

    TStrongObjectPtr<UCurveFloat> Fixture(NewObject<UCurveFloat>(
        Package, FName(*AssetName), RF_Public | RF_Standalone));
    if (!TestNotNull(TEXT("never-saved curve fixture created"), Fixture.Get()))
    {
        return false;
    }
    Fixture->FloatCurve.AddKey(0.0f, 1.0f);
    FAssetRegistryModule::AssetCreated(Fixture.Get());

    // Unconditional: a regression must fail this test's assertion, not leave a live element handle
    // pointing at a fixture that a later frame's GC will free.
    ON_SCOPE_EXIT
    {
        if (Fixture.IsValid())
        {
            SelectedObjects->Deselect(Fixture.Get());
        }
    };

    SelectedObjects->Select(Fixture.Get());
    if (!SelectedObjects->IsSelected(Fixture.Get()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("object-not-selectable-on-host"),
            TEXT("USelection::Select refused the curve fixture, so no element handle was taken."));
        return true;
    }

    PwTestAssetTeardown::DiscardLoadedAssetNoGc(Fixture.Get());

    TestFalse(
        TEXT("Discard releases the selection's typed-element handle on the detached fixture"),
        SelectedObjects->IsSelected(Fixture.Get()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAutomationSuiteMaintenanceMarkerContractTest,
    "PinWright.infra.contract.SuiteMaintenance.EscalationMarkerMatchesTheCheckerLiteral",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAutomationSuiteMaintenanceMarkerContractTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // The escalation marker is a cross-language contract: C++ writes it, check_suite_log.py greps
    // it, and nothing links the two spellings. Asserting it by RUNNING the escalation is not
    // available -- that would put the token in this run's own log and classify the suite
    // COMPLETED_WITH_MEMORY_PRESSURE -- so it is asserted structurally, the way the discard
    // ordering above is.
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    if (!TestTrue(TEXT("PinWright plugin resolved for the marker contract"), Plugin.IsValid()))
    {
        return false;
    }

    const FString Marker = TEXT("PINWRIGHT_MEMORY_WATERMARK_EXCEEDED");

    FString MaintenanceSource;
    const FString MaintenanceCpp = Plugin->GetBaseDir()
        / TEXT("Source/PinWright/Private/Tests/AutomationSuiteMaintenance.cpp");
    if (!TestTrue(TEXT("Read AutomationSuiteMaintenance.cpp"),
            FFileHelper::LoadFileToString(MaintenanceSource, *MaintenanceCpp)))
    {
        return false;
    }

    FString ResetBody;
    if (!TestTrue(TEXT("Located RunResetNow implementation"),
            SuiteMaintenanceElementHygieneHelpers::ExtractHelperBody(
                MaintenanceSource, TEXT("void RunResetNow(EResetTrigger Trigger)"), ResetBody)))
    {
        return false;
    }

    const int32 Guard = ResetBody.Find(TEXT("ShouldEscalateAfterReset("));
    const int32 Counter = ResetBody.Find(TEXT("++Stats.HardFractionEscalations"));
    const int32 Token = ResetBody.Find(*Marker);

    TestTrue(TEXT("The escalation is guarded by the pure predicate"), Guard != INDEX_NONE);
    TestTrue(TEXT("The escalation increments its counter"),
        Counter != INDEX_NONE && Counter > Guard);
    TestTrue(TEXT("The escalation writes the marker after the guard"),
        Token != INDEX_NONE && Token > Guard);
    TestTrue(TEXT("The escalation logs at Error, so it cannot be lost at default verbosity"),
        ResetBody.Find(TEXT("Error,"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Guard)
            != INDEX_NONE);
    // Every reset is on the record at Display; a reset that reclaimed nothing and a reset that
    // never happened are indistinguishable otherwise.
    TestTrue(TEXT("Every reset logs at Display with its trigger"),
        ResetBody.Contains(TEXT("Display,")) && ResetBody.Contains(TEXT("(trigger=%s)")));

    FString CheckerSource;
    const FString CheckerPy = Plugin->GetBaseDir() / TEXT("Content/Python/mcp_proxy.py");
    if (!TestTrue(TEXT("Read mcp_proxy.py"),
            FFileHelper::LoadFileToString(CheckerSource, *CheckerPy)))
    {
        return false;
    }
    TestTrue(TEXT("The log checker greps the same marker literal"),
        CheckerSource.Contains(Marker));
    // The other half of the same contract: the engine's OOM strings the MEMORY_EXHAUSTED verdict
    // keys on. A reworded engine string is a silent regression to DID_NOT_COMPLETE.
    TestTrue(TEXT("The log checker greps the allocation-failure string"),
        CheckerSource.Contains(TEXT("Ran out of memory allocating")));

    return true;
}

#endif
