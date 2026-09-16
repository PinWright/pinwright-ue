// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression for the render-thread access violation that killed a shared editor mid-session.
//
// THE DEFECT. FSlateApplication::TakeScreenshot parks a RAW POINTER to the caller's
// TArray<FColor> in FSlateRHIRenderer::ScreenshotState and then draws the widget's window. The
// renderer clears that state only when the target window's FSlateViewportInfo actually turns up in
// the pass that draw produced (DrawWindows_Private clears under `if (bScreenshotProcessed)`), and
// FSlateApplication::DrawWindowAndChildren silently skips a window that is hidden, minimized or
// pre-empted by an active modal window. Every screenshot handler passed a STACK-LOCAL array, so a
// window that was not drawn left the renderer armed with a pointer into a frame that died the
// moment the handler returned - and the readback then ran on whatever later Slate frame finally
// drew that window, into freed memory, faulting inside RHIReadSurfaceData's
// TArray::SetNumUninitialized on a frame with no call of ours anywhere near it.
//
// WHAT THIS TEST ASSERTS, and why it is the contract rather than the symptom. The crash itself is
// a use-after-free on the render thread: it is timing-dependent, it does not reproduce on demand,
// and reproducing it would take the suite host down with it. The OBSERVABLE property that makes it
// impossible is the one PinWrightScreenshotUtils::TakeSlateScreenshot guarantees - when it returns,
// the renderer holds no pending screenshot request aimed at plugin memory, on every exit path,
// whether the capture succeeded or not.
//
// HOW THE FIXTURE REACHES THE FAILING BRANCH. The window is added and SHOWN first (SWindow only
// creates its renderer viewport on first show, and without a viewport the screenshot state is
// unmatchable and the bug cannot arm at all), then hidden natively. SWindow::HideWindow touches
// only the native window, not the widget's Visibility attribute, so the screenshot path can still
// resolve a widget path to it - it arms - while DrawWindowAndChildren, which tests
// SWindow::IsVisible(), skips it - so nothing consumes the arming. Showing it again afterwards and
// ticking Slate is precisely the "later Slate frame" on which the stale readback used to fire.
//
// Under -RenderOffScreen the draw ignores window visibility, so the capture completes in-call and
// that branch is not exercised; the postcondition assertions still run and the test says so through
// the shared skip marker rather than reporting a quiet green.

#include "Misc/AutomationTest.h"

#include "Framework/Application/SlateApplication.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Utils/ScreenshotUtils.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SWindow.h"

#include "Tests/TestSkipReporting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSlateScreenshotLeavesNoPendingRendererStateTest,
    "PinWright.render.slate_screenshot.LeavesNoPendingRendererState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSlateScreenshotLeavesNoPendingRendererStateTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("slate-application-not-initialized"),
            TEXT("FSlateApplication is not initialized on this host, so no Slate screenshot "
                 "request can be armed and the pending-state contract cannot be measured."));
        return true;
    }
    FSlateApplication& SlateApp = FSlateApplication::Get();

    TestFalse(TEXT("no screenshot request is pending before the call"),
        PinWrightScreenshotUtils::HasPendingSlateScreenshotRequest());
    TestEqual(TEXT("the shared destination buffer starts empty"),
        PinWrightScreenshotUtils::SlateScreenshotDestinationPixelCount(), 0);

    // Uniquely titled and never addressed by any verb: this test only ever touches its own
    // fixture, so it cannot hide or destroy the host's main editor frame.
    const FString Title = FString::Printf(TEXT("PW_SlateScreenshotPending_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(FText::FromString(Title))
        .ScreenPosition(FVector2D(160.0f, 160.0f))
        .ClientSize(FVector2D(320.0f, 240.0f))
        .FocusWhenFirstShown(false)
        .SupportsMaximize(false)
        .SupportsMinimize(false);

    SlateApp.AddWindow(Window, /*bShowImmediately=*/true);
    ON_SCOPE_EXIT
    {
        // Re-show before teardown on every path, including a failing one, so no hidden fixture
        // window can outlive the test.
        Window->ShowWindow();
        SlateApp.RequestDestroyWindow(Window);
    };

    for (int32 Tick = 0; Tick < 4; ++Tick)
    {
        SlateApp.Tick(ESlateTickType::All);
    }

    Window->HideWindow();
    for (int32 Tick = 0; Tick < 2; ++Tick)
    {
        SlateApp.Tick(ESlateTickType::All);
    }

    TArray<FColor> Pixels;
    FIntVector Size(0, 0, 0);
    const bool bCaptured = PinWrightScreenshotUtils::TakeSlateScreenshot(
        StaticCastSharedRef<SWidget>(Window), Pixels, Size);

    // THE assertion the fix exists for. Succeeded or failed, the renderer must not still be
    // holding a request aimed at plugin memory.
    TestFalse(TEXT("no screenshot request is pending once TakeSlateScreenshot has returned"),
        PinWrightScreenshotUtils::HasPendingSlateScreenshotRequest());
    TestEqual(TEXT("the shared destination buffer is drained on return"),
        PinWrightScreenshotUtils::SlateScreenshotDestinationPixelCount(), 0);

    if (bCaptured)
    {
        TestTrue(TEXT("a capture reported complete carries at least a full rect of pixels"),
            Size.X > 0 && Size.Y > 0
                && Pixels.Num() >= static_cast<int64>(Size.X) * static_cast<int64>(Size.Y));
    }
    else
    {
        TestEqual(TEXT("a capture that did not complete in-call hands back no pixels"),
            Pixels.Num(), 0);
    }

    // The later Slate frame. Pre-fix this is where the stale readback executed - into the
    // handler's dead stack frame on the render thread.
    Window->ShowWindow();
    for (int32 Tick = 0; Tick < 8; ++Tick)
    {
        SlateApp.Tick(ESlateTickType::All);
    }

    TestEqual(TEXT("no readback landed in the shared buffer on a later Slate frame"),
        PinWrightScreenshotUtils::SlateScreenshotDestinationPixelCount(), 0);
    TestFalse(TEXT("no screenshot request is pending after later Slate frames have been drawn"),
        PinWrightScreenshotUtils::HasPendingSlateScreenshotRequest());

    if (bCaptured)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("hidden-window-was-still-drawn-on-host"),
            FString::Printf(
                TEXT("The capture of hidden fixture window '%s' completed in-call (%dx%d, %d "
                     "pixels), which is what -RenderOffScreen does: the draw ignores window "
                     "visibility, so the renderer consumed and cleared its own state and the "
                     "not-drawn branch that the disarm exists for was never entered. The "
                     "pending-state and drained-buffer assertions above DID run."),
                *Title, Size.X, Size.Y, Pixels.Num()));
    }

    return true;
}
