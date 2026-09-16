// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-open-asset-world-map-load-crash / the level.load half of it.
//
// The defect: level.load called FEditorFileUtils::LoadMap on whatever game-thread
// stack the RPC happened to land on. Requests are marshalled with
// AsyncTask(ENamedThreads::GameThread, ...) (RpcDispatcher.cpp:377-385), and that
// queue is drained from INSIDE UWorld::Tick while the engine waits on tick groups
// (TickTaskManager.cpp:1040/1045/1064). LoadMap tears the outgoing world down and
// GCs its ULevel, which mid-frame trips
// `check(!LevelList.Contains(TickTaskLevel))` (TickTaskManager.cpp:1992) and kills
// the editor. Full engine evidence is in Dispatch/SafePoint.h.
//
// The fix: level.load gates the swap on PinWrightSafePoint::IsSafeNow() - running it
// inline (and responding through Ctx, exactly as before) when no world is ticking,
// and otherwise handing it to PinWrightSafePoint::DeferToSafePoint, which parks it on
// the next core-ticker pass (pumped by FEngineLoop::Tick AFTER the world tick ends).
//
// Counterfactual for each test is stated at its own header.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Containers/Ticker.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

#include "Dispatch/SafePoint.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"

namespace LevelLoadSafePointTests
{
    // RAII for the forced-unsafe override so a failing check macro that aborts the
    // test body can never leave the latch set for the rest of the suite (it is a
    // process-global, exactly like PinWrightAutomationMode's depth).
    struct FScopedForcedUnsafeMapSwap
    {
        FScopedForcedUnsafeMapSwap() { PinWrightSafePoint::SetForcedUnsafeForTests(true); }
        ~FScopedForcedUnsafeMapSwap() { PinWrightSafePoint::SetForcedUnsafeForTests(false); }
    };
}

// ============================================================================
// 1. The primitive: inline on a safe stack, one core-ticker hop otherwise.
// ============================================================================

// Automation tests run outside UWorld::Tick, so IsSafeNow() must report safe -
// which is what keeps the common case (an RPC pumped outside the world tick) on
// the unchanged inline path in level.load below.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMapSwapSafePointReportsSafeOffTickTest,
    "PinWright.level.load.SafePointReportsSafeOffTick",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMapSwapSafePointReportsSafeOffTickTest::RunTest(const FString& Parameters)
{
    TestFalse(TEXT("no world is ticking on an automation stack"),
        PinWrightSafePoint::IsAnyWorldTicking());
    TestTrue(TEXT("a map swap is safe on an automation stack"),
        PinWrightSafePoint::IsSafeNow());
    return true;
}

// Counterfactual: make DeferToSafePoint call Work() directly and "work has not run
// yet" fails; drop the `return false` one-shot and "runs exactly once" fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMapSwapSafePointDefersOneCoreTickerPassTest,
    "PinWright.level.load.SafePointDefersOneCoreTickerPass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMapSwapSafePointDefersOneCoreTickerPassTest::RunTest(const FString& Parameters)
{
    using namespace LevelLoadSafePointTests;

    // TSharedRef so the ticker lambda cannot outlive the counter if the pump below
    // somehow fails to drain it before RunTest returns.
    TSharedRef<int32> RunCount = MakeShared<int32>(0);

    {
        FScopedForcedUnsafeMapSwap ForcedUnsafe;

        TestFalse(TEXT("the override makes IsSafeNow() report unsafe"),
            PinWrightSafePoint::IsSafeNow());

        PinWrightSafePoint::DeferToSafePoint([RunCount]() { ++(*RunCount); });

        TestEqual(TEXT("work has not run yet on the caller's stack"), *RunCount, 0);
    }

    // One core-ticker pass is all the deferral costs. The deferred branch does not
    // re-check IsSafeNow(), so clearing the override above is irrelevant to whether
    // this fires - it fires on the first pump either way.
    FTSTicker::GetCoreTicker().Tick(0.0f);

    TestEqual(TEXT("work ran after one core-ticker pass"), *RunCount, 1);

    // One-shot: the ticker delegate removed itself.
    FTSTicker::GetCoreTicker().Tick(0.0f);
    TestEqual(TEXT("deferred work runs exactly once"), *RunCount, 1);
    return true;
}

// ============================================================================
// 2. The production call path: level.load must route its swap through the safe
//    point, not call LoadMap on the caller's stack.
// ============================================================================

// Drives the REAL level.load handler (the same FHandlerRegistration::Func the
// dispatcher invokes) through a genuine map swap with the safe point forced
// unsafe, and asserts the handler returned WITHOUT having responded - i.e. the
// LoadMap did not run on the caller's stack. Then pumps one core-ticker pass and
// asserts the load completed and reported the deferral.
//
// Counterfactual: revert LevelHandler.cpp's PinWrightSafePoint::RunAtSafePoint call
// back to a bare `FEditorFileUtils::LoadMap(FileToLoad);` and "level.load has not
// responded on the caller's stack" fails immediately (the handler responds
// synchronously), as does the deferredToSafePoint assertion.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelLoadDefersMapSwapToSafePointTest,
    "PinWright.level.load.DefersMapSwapToSafePoint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelLoadDefersMapSwapToSafePointTest::RunTest(const FString& Parameters)
{
    using namespace LevelLoadSafePointTests;

    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No GEditor (commandlet/unit context); skipping the level.load safe-point "
                "dispatch assertions."));
        return true;
    }

    UWorld* const StartWorld = GEditor->GetEditorWorldContext().World();
    const FString OriginalMapPath = (StartWorld && StartWorld->GetOutermost())
        ? StartWorld->GetOutermost()->GetName()
        : FString();
    // The map the deferred swap will load. The open world is untitled since
    // PinWright.aa_suite_start.OpenBlankTransientWorld, so it is no longer a map that can
    // be loaded back and the fixture is discovered from the asset registry instead; the
    // ambient world is still preferred when it does have a package on disk (a scoped run
    // that filters the suite-start step out). Only a host with no map asset at all skips.
    const FString TargetMapPath =
        (!OriginalMapPath.IsEmpty() && FPackageName::DoesPackageExist(OriginalMapPath))
            ? OriginalMapPath
            : FindAlternateOnDiskMap(OriginalMapPath);
    if (TargetMapPath.IsEmpty())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("FIXTURE-SKIP: this host has no World asset with a .umap on disk, so there "
                "is no map to swap to; skipping."));
        return true;
    }

    // Safety net for every exit path, including a check macro that aborts mid-body.
    FScopedEditorWorldMapGuard MapGuard;

    // Leave the original map so level.load has a genuine swap to perform (it
    // early-outs with alreadyLoaded when the requested map is already active, which
    // never reaches the safe point). NewMap builds a transient in-memory world, so
    // nothing is written to disk and there is no probe asset to discard.
    if (!GEditor->NewMap(false))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("probe-world-unavailable"),
            TEXT("GEditor->NewMap(false) returned no world; skipping."));
        return true;
    }

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelPath"), TargetMapPath);

    {
        FScopedForcedUnsafeMapSwap ForcedUnsafe;

        TestTrue(TEXT("level.load handler is registered"),
            InvokeHandlerWithSharedCapture(TEXT("level.load"), Payload, Capture));

        // THE COUNTERFACTUAL. Pre-fix this is already true: LoadMap ran inline and
        // the response was sent before the handler returned.
        TestFalse(TEXT("level.load has not responded on the caller's stack (the swap deferred)"),
            Capture->bWasCalled);
    }

    // Let the deferred swap run. PumpUntilCaptured ticks the core ticker, which is
    // where DeferToSafePoint parked the work.
    PumpUntilCaptured(*Capture, 120.0);

    TestTrue(TEXT("level.load responded after the safe-point hop"), Capture->bWasCalled);
    TestTrue(TEXT("level.load succeeded"), Capture->bSuccess);

    if (Capture->Result.IsValid())
    {
        bool bDeferred = false;
        TestTrue(TEXT("response carries deferredToSafePoint"),
            Capture->Result->TryGetBoolField(TEXT("deferredToSafePoint"), bDeferred));
        TestTrue(TEXT("the swap reports it was deferred to a safe point"), bDeferred);

        bool bLoaded = false;
        Capture->Result->TryGetBoolField(TEXT("loaded"), bLoaded);
        TestTrue(TEXT("the requested map is the active world after the deferred swap"), bLoaded);
    }
    else
    {
        AddError(TEXT("level.load produced no result object after the safe-point hop."));
    }

    return true;
}
