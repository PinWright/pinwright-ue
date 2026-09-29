// Copyright (c) 2026 Alexander Penkin. MIT License.

// Failure-direction tests for the NullRHI hardening (Utils/RenderingAvailability.h).
//
// The guard's refusal branch only runs on a host with no renderer, which a suite taken in mode
// `offscreen` or `visible` never is. PinWrightRendering::FScopedForceUnavailableForTesting forces
// that branch in any editor, so both tests measure the same thing in every mode, headless included.
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Dom/JsonObject.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Utils/RenderingAvailability.h"

#include "Compat/EngineVersionCompat.h"

namespace
{
    // Every verb that calls PinWrightRendering::RequireRenderer as its first statement. Adding
    // the guard to a verb means adding it here, so the refusal is asserted for it too.
    const TCHAR* const PWRenderGuardedVerbs[] = {
        TEXT("render.capture_mesh"),
        TEXT("render.capture_asset_preview"),
        TEXT("render.capture_open_level"),
        TEXT("render.capture_animation_preview"),
        TEXT("render.capture_annotated"),
        TEXT("render.capture_ortho_tiles"),
        TEXT("render.detect_z_fighting"),
        TEXT("render.lumen_update_scene"),
        TEXT("camera.frame_actor"),
        TEXT("camera.orbit_shots"),
        TEXT("camera.animation_shots"),
        TEXT("editor.screenshot"),
        TEXT("editor.screenshot_window"),
        TEXT("ui.screenshot"),
        TEXT("widget.screenshot_designer"),
        TEXT("asset.generate_thumbnail"),
        TEXT("effect.step_and_capture"),
        TEXT("performance.run_benchmark"),
        TEXT("mrq.run_jobs"),
        TEXT("landscape.sculpt"),
        TEXT("landscape.edit"),
    };

    // A test that exists only to be emitted into, so the skip helper's marker is read back here
    // and never reaches a suite log (same technique as Tests/Infra/TestSkipMarkerEmission.cpp).
    // No dot in the name, so it cannot become a branch node of a real id.
    const TCHAR* const PWRenderGuardProbeName = TEXT("PinWrightRenderGuardSkipProbe");

    class FPWRenderGuardSkipProbe final : public FAutomationTestBase
    {
    public:
        FPWRenderGuardSkipProbe() : FAutomationTestBase(PWRenderGuardProbeName, /*bInComplexTask=*/false) {}

        virtual MCP_AUTOMATION_TEST_FLAGS GetTestFlags() const override
        {
            return EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
        }
        virtual uint32 GetRequiredDeviceNum() const override { return 1; }
        virtual FString GetBeautifiedTestName() const override { return PWRenderGuardProbeName; }
        virtual void GetTests(TArray<FString>& OutBeautifiedNames,
            TArray<FString>& OutTestCommands) const override {}
        virtual bool RunTest(const FString& Parameters) override { return true; }
    };
}

// Every guarded verb refuses with RENDERING_UNAVAILABLE, names the modes that render, and does so
// before touching its payload (an empty payload would otherwise be a parameter error).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPWRenderGuardVerbsRefuseTest,
    "PinWright.infra.rendering_guard.EveryGuardedVerbRefusesWithoutRenderer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPWRenderGuardVerbsRefuseTest::RunTest(const FString& Parameters)
{
    {
        PinWrightRendering::FScopedForceUnavailableForTesting ForceNoRenderer;
        TestFalse(TEXT("the seam makes IsAvailable() false"), PinWrightRendering::IsAvailable());

        for (const TCHAR* Verb : PWRenderGuardedVerbs)
        {
            FTestResponseCapture Capture;
            if (!TestTrue(FString::Printf(TEXT("%s is registered"), Verb),
                    InvokeHandlerWithCapture(Verb, MakeShared<FJsonObject>(), Capture)))
            {
                continue;
            }
            TestFalse(FString::Printf(TEXT("%s does not succeed"), Verb), Capture.bSuccess);
            TestEqual(FString::Printf(TEXT("%s error code"), Verb),
                Capture.ErrorCode, FString(ErrorCodes::ERR_RENDERING_UNAVAILABLE));
            TestTrue(FString::Printf(TEXT("%s message names mode 'offscreen'"), Verb),
                Capture.Message.Contains(TEXT("'offscreen'")));
            TestTrue(FString::Printf(TEXT("%s message names mode 'visible'"), Verb),
                Capture.Message.Contains(TEXT("'visible'")));
            FString DataMethod;
            TestTrue(FString::Printf(TEXT("%s error data carries the method"), Verb),
                Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("method"), DataMethod)
                && DataMethod == Verb);
        }
    }

    TestEqual(TEXT("leaving the seam restores the host's real availability"),
        PinWrightRendering::IsAvailable(), FApp::CanEverRender());
    return true;
}

// The test-side skip emits reason=null-rhi exactly when the handler guard would refuse, and
// nothing otherwise.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPWRenderGuardSkipHelperTest,
    "PinWright.infra.rendering_guard.SkipHelperEmitsNullRhiReasonWithoutRenderer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPWRenderGuardSkipHelperTest::RunTest(const FString& Parameters)
{
    {
        PinWrightRendering::FScopedForceUnavailableForTesting ForceNoRenderer;
        FPWRenderGuardSkipProbe Probe;
        TestTrue(TEXT("the helper reports a skip without a renderer"),
            PinWrightTestSkip::SkipIfRenderingUnavailable(Probe));

        FAutomationTestExecutionInfo Info;
        Probe.GetExecutionInfo(Info);
        TestEqual(TEXT("exactly one warning"), Info.GetWarningTotal(), 1);
        TestEqual(TEXT("no error"), Info.GetErrorTotal(), 0);
        FString Emitted;
        for (const FAutomationExecutionEntry& Entry : Info.GetEntries())
        {
            if (Entry.Event.Type == EAutomationEventType::Warning)
            {
                Emitted = Entry.Event.Message;
                break;
            }
        }
        TestTrue(TEXT("the warning leads with the skip marker"),
            Emitted.StartsWith(PinWrightTestSkip::Marker()));
        TestTrue(TEXT("the warning carries reason=null-rhi"),
            Emitted.Contains(TEXT(" reason=null-rhi ")));
    }

    // The pass-through direction is only observable on a host that renders; under a real NullRHI
    // the helper correctly skips, which the block above already covers.
    if (FApp::CanEverRender())
    {
        FPWRenderGuardSkipProbe Probe;
        TestFalse(TEXT("the helper does not skip on a rendering host"),
            PinWrightTestSkip::SkipIfRenderingUnavailable(Probe));
        FAutomationTestExecutionInfo Info;
        Probe.GetExecutionInfo(Info);
        TestEqual(TEXT("and emits nothing"), Info.GetWarningTotal(), 0);
    }
    return true;
}
