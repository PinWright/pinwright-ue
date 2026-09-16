// Copyright (c) 2026 Alexander Penkin. MIT License.

// The frame-quality precondition a pixel readback has to satisfy: the materials in view are not
// still compiling.
//
// WHAT THIS DEFENDS. A capture taken while shader compilation is in flight photographs the DEFAULT
// material -- grey, or WorldGridMaterial -- where authored materials belong, and returns a
// plausible PNG with every honesty field clean. `warmup` cannot catch it: the frame settles
// perfectly, because a stand-in material is stable. So the gate drains the shader queue, reports
// what it drained, and refuses rather than shipping a frame of stand-ins.
//
// WHAT THIS IS NOT. Not a crash guard. The GPU page fault that prompted the work is a
// resource-lifetime / descriptor-residency fault at the readback, and the D3D12 "late shader
// associations" lines quoted with it are the Aftermath crash-dump DECODER's output AFTER the
// fault, not a pre-fault state. The mitigation aimed at that is the readback flush
// (PinWrightScreenshotUtils::FlushBeforeReadback), asserted at the bottom of this file, and it is
// plausible rather than proven.
//
// WHY IT IS ASSERTED WITHOUT A GPU. The gate is a bounded pumping loop over two compile-queue
// counters plus a pure serializer, and the counters come through one seam (SurveyPendingWork) that
// takes an injected probe under WITH_DEV_AUTOMATION_TESTS. So the blocking decision, the drain
// loop, the published block and the refusal message all run their production code paths against a
// deterministic pending state. A capture test that cannot get a GPU takes a conditional-skip path
// and reports success WITHOUT running its assertions (board B-test-skips-assertions-silently),
// which is exactly what this must not be defended by.
#include "Misc/AutomationTest.h"

#include "Utils/CaptureReadinessGate.h"
#include "Utils/ScreenshotUtils.h"

#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHI.h" // GIsRHIInitialized, the preamble's own precondition

namespace
{
    // Prefixed because anonymous namespaces merge inside one Unity translation unit, so a bare
    // name a sibling test also uses is a latent ODR clash.
    bool PwReadinessReadSource(const FString& RelativePath, FString& OutSource)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return false;
        }
        return FFileHelper::LoadFileToString(
            OutSource, *FPaths::Combine(Plugin->GetBaseDir(), RelativePath));
    }

    // A budget short enough to time out inside a test but long enough to run several pump rounds,
    // so the loop's bookkeeping is exercised rather than skipped.
    constexpr double PwReadinessTestBudgetSeconds = 0.05;
}

// ============================================================================
// A pending-compile predicate holds the gate, and ONLY shader work does.
//
// The asset-queue case is the important half of this test: a non-zero FAssetCompilingManager queue
// is the normal steady state after any map load, so a gate that waited on it burned the entire
// budget on the game thread on EVERY capture and then took the frame anyway.
//
// The counterfactual: revert ShouldRefuseReadback in Utils/CaptureReadinessGate.h (or drop the
// drain loop's timeout bookkeeping) and the shader case below reports ShouldRefuseReadback false,
// so the verb would photograph default materials exactly as it did before; widen the drain loop
// back to "any pending work" and the asset-only case below stops reporting PumpRounds 0.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureReadinessPendingWorkTest,
    "PinWright.render.capture_readiness.PendingShaderWorkRefusesTheReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureReadinessPendingWorkTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureReadiness;

    // Shader work that never drains: the crash mechanism.
    {
        const FScopedPendingWorkProbe Probe([](FPendingWork& Work)
        {
            Work.bShaderCompilerAvailable = true;
            Work.ShaderJobs = 3;
            Work.AssetCompilations = 0;
            return true;
        });

        const FReadinessResult Result = DrainBeforeReadback(PwReadinessTestBudgetSeconds);
        TestTrue(TEXT("the gate ran"), Result.bMeasured);
        TestTrue(TEXT("pending shader work is seen at entry"), Result.AtEntry.HasShaderWork());
        TestTrue(TEXT("the bounded drain gave up rather than waiting forever"), Result.bTimedOut);
        TestFalse(TEXT("a timed-out drain is not reported ready"), Result.bReady);
        TestEqual(TEXT("the shader count that held the gate is carried out"),
            Result.AtExit.ShaderJobs, 3);
        TestTrue(TEXT("shader work still in flight refuses the readback"),
            Result.ShouldRefuseReadback());
        // The message is the whole value of the refusal to a caller who now has to decide what to
        // do, so it names the mechanism and the numbers rather than saying "not ready".
        const FString Refusal = MakeRefusalMessage(Result);
        TestTrue(TEXT("the refusal names shader compilation"),
            Refusal.Contains(TEXT("shader compilation")));
        TestTrue(TEXT("the refusal names the default material as the consequence"),
            Refusal.Contains(TEXT("DEFAULT material")));
    }

    // Asset compilation that never drains: NOT waited on and NOT a refusal. It is the normal
    // steady state after a map load; waiting on it spends the caller's whole budget for nothing.
    {
        const FScopedPendingWorkProbe Probe([](FPendingWork& Work)
        {
            Work.bShaderCompilerAvailable = true;
            Work.ShaderJobs = 0;
            Work.AssetCompilations = 2;
            return true;
        });

        const FReadinessResult Result = DrainBeforeReadback(PwReadinessTestBudgetSeconds);
        TestFalse(TEXT("a pending asset queue does not time the gate out"), Result.bTimedOut);
        TestTrue(TEXT("a pending asset queue still counts as ready"), Result.bReady);
        // The regression this pins: any wall clock here is 20 s burned on the game thread on every
        // screenshot in a normal editor.
        TestEqual(TEXT("a pending asset queue costs no pump rounds"), Result.PumpRounds, 0);
        TestFalse(TEXT("asset compilation alone does not refuse the readback"),
            Result.ShouldRefuseReadback());

        const TSharedPtr<FJsonObject> Block = MakeReadinessInfoObject(Result);
        if (TestTrue(TEXT("a block is published for the shot frame"), Block.IsValid()))
        {
            TestTrue(TEXT("the unwaited asset queue is disclosed as a caveat"),
                Block->HasTypedField<EJson::String>(TEXT("assetCompilationWarning")));
            TestFalse(TEXT("an asset queue is not reported as a shader-readiness failure"),
                Block->HasField(TEXT("readinessWarning")));
            TestEqual(TEXT("the queued asset count is published"),
                static_cast<int32>(Block->GetNumberField(TEXT("assetCompilationsRemaining"))), 2);
        }
    }

    // The ready case: nothing pending, no wait, no refusal.
    {
        const FScopedPendingWorkProbe Probe([](FPendingWork& Work)
        {
            Work.bShaderCompilerAvailable = true;
            Work.ShaderJobs = 0;
            Work.AssetCompilations = 0;
            return true;
        });

        const FReadinessResult Result = DrainBeforeReadback(PwReadinessTestBudgetSeconds);
        TestTrue(TEXT("a quiet editor is ready immediately"), Result.bReady);
        TestFalse(TEXT("a quiet editor does not time out"), Result.bTimedOut);
        TestEqual(TEXT("a quiet editor costs no pump rounds"), Result.PumpRounds, 0);
        TestFalse(TEXT("a quiet editor does not refuse"), Result.ShouldRefuseReadback());
    }

    return true;
}

// ============================================================================
// The block is emitted with what was pending, how long it drained and whether it timed out -- and
// is ABSENT on a path that took no reading.
//
// The counterfactual: delete the AddReadinessFields calls in RenderHandler.cpp /
// ViewportHandler.cpp / UiHandler.cpp and the `viewport.shadersCompiling` block disappears from
// all three verbs, which is the state the ticket was filed in -- a capture that reported nothing
// at all about what was still compiling when the frame was taken.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureReadinessBlockTest,
    "PinWright.render.capture_readiness.BlockIsPublishedAndAbsentWhenUnmeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureReadinessBlockTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureReadiness;

    FReadinessResult Measured;
    Measured.bMeasured = true;
    Measured.AtEntry.bShaderCompilerAvailable = true;
    Measured.AtEntry.ShaderJobs = 41;
    Measured.AtEntry.AssetCompilations = 7;
    Measured.AtExit.bShaderCompilerAvailable = true;
    Measured.AtExit.ShaderJobs = 0;
    Measured.AtExit.AssetCompilations = 0;
    Measured.bReady = true;
    Measured.PumpRounds = 12;
    Measured.DrainMs = 133.5;
    Measured.BudgetMs = 20000.0;

    const TSharedPtr<FJsonObject> Block = MakeReadinessInfoObject(Measured);
    if (!TestTrue(TEXT("a measured gate publishes a block"), Block.IsValid()))
    {
        return false;
    }
    TestTrue(TEXT("measured says the reading was taken"), Block->GetBoolField(TEXT("measured")));
    TestTrue(TEXT("pendingAtEntry separates a wait from a no-op"),
        Block->GetBoolField(TEXT("pendingAtEntry")));
    TestTrue(TEXT("ready is published"), Block->GetBoolField(TEXT("ready")));
    TestFalse(TEXT("timedOut is published"), Block->GetBoolField(TEXT("timedOut")));
    TestEqual(TEXT("what was pending at entry is published"),
        static_cast<int32>(Block->GetNumberField(TEXT("shaderJobsAtEntry"))), 41);
    TestEqual(TEXT("what remained is published beside it"),
        static_cast<int32>(Block->GetNumberField(TEXT("shaderJobsRemaining"))), 0);
    TestEqual(TEXT("the asset queue is published too"),
        static_cast<int32>(Block->GetNumberField(TEXT("assetCompilationsAtEntry"))), 7);
    TestEqual(TEXT("how long it drained is published"),
        Block->GetNumberField(TEXT("drainMs")), 133.5);
    TestEqual(TEXT("the budget it drained against is published"),
        Block->GetNumberField(TEXT("budgetMs")), 20000.0);
    TestFalse(TEXT("a drained gate carries no warning"),
        Block->HasField(TEXT("readinessWarning")));

    // A path that never gated must not publish a clean reading nobody took.
    const FReadinessResult Unmeasured;
    TestFalse(TEXT("an unmeasured gate publishes no block"),
        MakeReadinessInfoObject(Unmeasured).IsValid());
    const TSharedPtr<FJsonObject> Viewport = MakeShared<FJsonObject>();
    AddReadinessFields(Unmeasured, Viewport);
    TestFalse(TEXT("an unmeasured gate adds no field to the viewport block"),
        Viewport->HasField(TEXT("shadersCompiling")));
    TestFalse(TEXT("an unmeasured gate claims no readback flush"),
        Viewport->HasField(TEXT("readbackFlushed")));

    Measured.bReadbackFlushed = true;
    AddReadinessFields(Measured, Viewport);
    TestTrue(TEXT("a measured gate lands under viewport.shadersCompiling"),
        Viewport->HasTypedField<EJson::Object>(TEXT("shadersCompiling")));
    // The other half of the readback preamble travels on the same publish, so a caller never has
    // to infer from one whether the other ran.
    TestTrue(TEXT("the readback flush verdict is published beside it"),
        Viewport->GetBoolField(TEXT("readbackFlushed")));

    return true;
}

// ============================================================================
// Every capture verb runs the preamble in the order gate -> flush -> readback.
//
// Ordering carries the whole value. A gate placed at handler entry measures the compile queue
// BEFORE the new camera pose has queued anything, which is the wrong queue; a flush placed after
// the readback mitigates nothing. On the level path both halves therefore ride the
// BeforeFinalFrame hook, which the shared capture calls after the pose apply and the warm-up
// settle loop and immediately before the final draw + readback.
//
// Asserted on source text because the ordering lives in handler bodies that need a live viewport.
// The search is on the call's NAME, not `DrainBeforeReadback()` with empty parens, so passing a
// budget argument does not silently turn the assertion into a no-op.
//
// The counterfactual: move any DrainBeforeReadback below its capture call, or hoist either level
// path's gate out of the BeforeFinalFrame hook back to handler entry, and this test fails.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureReadinessWiringTest,
    "PinWright.render.capture_readiness.EveryVerbGatesThenFlushesThenReadsBack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureReadinessWiringTest::RunTest(const FString& Parameters)
{
    const TCHAR* GateCall = TEXT("PinWrightCaptureReadiness::DrainBeforeReadback(");
    const TCHAR* FlushCall = TEXT("PinWrightScreenshotUtils::FlushBeforeReadback(");
    const TCHAR* RefusalCheck = TEXT("ShouldRefuseReadback()");

    FString RenderSource;
    if (TestTrue(TEXT("RenderHandler.cpp is readable"), PwReadinessReadSource(
            TEXT("Source/PinWright/Private/Handlers/Render/RenderHandler.cpp"), RenderSource)))
    {
        const int32 GateAt = RenderSource.Find(GateCall);
        const int32 FlushAt = RenderSource.Find(FlushCall);
        const int32 ReadbackAt = RenderSource.Find(
            TEXT("PinWrightRenderCapture::CaptureEditorViewportToPng("));
        TestTrue(TEXT("render.capture_open_level gates, then flushes, then captures"),
            GateAt != INDEX_NONE && FlushAt != INDEX_NONE && ReadbackAt != INDEX_NONE
                && GateAt < FlushAt && FlushAt < ReadbackAt);
        // Both halves must sit inside the hook, or they run before the pose instead of after it.
        TestTrue(TEXT("render.capture_open_level runs the preamble from the BeforeFinalFrame hook"),
            RenderSource.Contains(TEXT("GatedHooks.BeforeFinalFrame")));
        TestTrue(TEXT("render.capture_open_level refuses on the shader case"),
            RenderSource.Contains(RefusalCheck));
    }

    FString ViewportSource;
    if (TestTrue(TEXT("ViewportHandler.cpp is readable"), PwReadinessReadSource(
            TEXT("Source/PinWright/Private/Handlers/Editor/ViewportHandler.cpp"), ViewportSource)))
    {
        const int32 GateAt = ViewportSource.Find(GateCall);
        const int32 ReadbackAt = ViewportSource.Find(
            TEXT("PinWrightScreenshotUtils::CaptureGameViewportToPngFile("));
        TestTrue(TEXT("editor.screenshot gates before the game/PIE readback"),
            GateAt != INDEX_NONE && ReadbackAt != INDEX_NONE && GateAt < ReadbackAt);
        TestTrue(TEXT("editor.screenshot's level branch gates from the BeforeFinalFrame hook"),
            ViewportSource.Contains(TEXT("GatedHooks.BeforeFinalFrame")));
        TestTrue(TEXT("editor.screenshot refuses on the shader case"),
            ViewportSource.Contains(RefusalCheck));
    }

    FString UiSource;
    if (TestTrue(TEXT("UiHandler.cpp is readable"), PwReadinessReadSource(
            TEXT("Source/PinWright/Private/Handlers/UI/UiHandler.cpp"), UiSource)))
    {
        const int32 GateAt = UiSource.Find(GateCall);
        const int32 ReadbackAt = UiSource.Find(
            TEXT("PinWrightScreenshotUtils::CaptureGameViewportToPngFile("));
        TestTrue(TEXT("ui.screenshot gates before the readback rather than being ungated"),
            GateAt != INDEX_NONE && ReadbackAt != INDEX_NONE && GateAt < ReadbackAt);
        TestTrue(TEXT("ui.screenshot refuses on the shader case"),
            UiSource.Contains(RefusalCheck));
    }

    return true;
}

// ============================================================================
// The shared readback preamble flushes, once, at the last point before the pixels are asked for.
//
// This is the half aimed at the ACTUAL reported failure -- a GPU page fault at the readback with
// Aftermath reporting AddressTranslationError / Read, which is a resource-lifetime or
// descriptor-residency shape. FlushRenderingCommands drains the render queue and issues
// ImmediateFlush(FlushRHIThreadFlushResources), so resources the preceding draws created, resized
// or released are settled before the copy is set up. It is a PLAUSIBLE mitigation, not a proven
// one: the fault was seen once, on one scene, and nothing here reproduces it.
//
// What IS asserted: the flush exists as one shared function rather than four copies, it is the
// last thing the shared game-viewport capture does before any of its three readback branches, and
// its verdict is reported rather than assumed.
//
// The counterfactual: delete the FlushBeforeReadback call from CaptureGameViewportToPngFile and
// the ordering assertion below fails; drop bReadbackFlushed from the metadata and callers go back
// to being unable to tell a flushed readback from an unflushed one.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureReadbackFlushPreambleTest,
    "PinWright.render.capture_readiness.SharedReadbackPreambleFlushesBeforeThePixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureReadbackFlushPreambleTest::RunTest(const FString& Parameters)
{
    // Production call, on the game thread the automation framework runs on. It returns false only
    // when it could not run, which is itself the honest report -- so both outcomes are legal and
    // the assertion is on the reporting, not on the host.
    const bool bFlushed = PinWrightScreenshotUtils::FlushBeforeReadback();
    TestTrue(TEXT("the preamble reports true on a game thread with an initialised RHI, "
                  "false otherwise -- never claims a flush it did not perform"),
        bFlushed == (IsInGameThread() && GIsRHIInitialized));

    FString CaptureSource;
    if (TestTrue(TEXT("ScreenshotUtils.cpp is readable"), PwReadinessReadSource(
            TEXT("Source/PinWright/Private/Utils/ScreenshotUtils.cpp"), CaptureSource)))
    {
        const int32 FlushAt = CaptureSource.Find(TEXT("FlushBeforeReadback();"));
        const int32 FixedSizeReadAt = CaptureSource.Find(TEXT("if (!Viewport->ReadPixels(Bitmap)"));
        const int32 SlateReadAt = CaptureSource.Find(TEXT("if (TakeSlateScreenshot("));
        TestTrue(TEXT("the shared capture flushes before its fixed-size readback"),
            FlushAt != INDEX_NONE && FixedSizeReadAt != INDEX_NONE && FlushAt < FixedSizeReadAt);
        TestTrue(TEXT("the shared capture flushes before its native back-buffer readback"),
            FlushAt != INDEX_NONE && SlateReadAt != INDEX_NONE && FlushAt < SlateReadAt);
        TestTrue(TEXT("the flush verdict reaches the caller through the capture metadata"),
            CaptureSource.Contains(TEXT("Metadata.bReadbackFlushed = bReadbackFlushed;")));
    }

    return true;
}
