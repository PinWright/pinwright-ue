// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the blank-capture contract: a capture verb must NEVER write a
// valid-looking but entirely empty image file and report success.
//
// The defect: the shared game-viewport capture path reads the game viewport's render
// target through FViewport::ReadPixels. That read is GPU-coherent (the engine enqueues
// and flushes internally), but it returns whatever the surface last held — all zeros when
// nothing has ever been presented into it (headless / -RenderOffScreen, a minimized
// viewport, PIE before its first frame). PinWrightScreenshotUtils::ForceOpaqueAlpha then
// rewrote every alpha to 0xFF, converting that detectably-empty all-zero buffer into a
// plausible opaque-black frame, which encoded and wrote cleanly. The caller got
// success:true, a correctly sized .png on disk, and — for ui.screenshot — the same black
// frame echoed back as base64, with nothing anywhere indicating the frame was never
// rendered. The fix rejects an all-zero readback with a typed BLANK_CAPTURE before the
// alpha stamp destroys the evidence.
//
// The primary test below drives the REGISTERED ui.screenshot verb, not
// CaptureGameViewportToPngFile. That distinction is load-bearing here: the sibling
// Tests/Render/TestCaptureOpaqueAlpha.cpp and Tests/EditorOps/TestUiScreenshotUmgOverlay.cpp
// both call the helper directly, so either would keep passing if the handler stopped
// calling it at all. Only a test wired to the dispatched verb proves the shipped path.
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/ScreenshotUtils.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

// Named (not anonymous) namespace: the plugin's tests share one module under Unity builds,
// so same-named anonymous-namespace helpers across .cpp files collide at ODR. Same
// convention as Tests/Infra/DispatcherTestHelpers.h.
namespace ScreenshotBlankCaptureTestHelpers
{
    // Every typed failure ui.screenshot is allowed to return. A capture verb that cannot
    // produce a frame must land on one of these — never on success, and never on an
    // untyped/empty code.
    inline bool IsTypedCaptureFailure(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("NO_VIEWPORT")
            || ErrorCode == TEXT("CAPTURE_FAILED")
            || ErrorCode == TEXT("BLANK_CAPTURE")
            || ErrorCode == TEXT("WRITE_FAILED");
    }

    // Loads a PNG and reports whether any pixel differs from pure black. This is the
    // decisive assertion for the shipped defect: the pre-fix all-zero readback reached disk
    // as uniform opaque black, so "at least one non-black pixel" is exactly the property
    // that separates a real capture from the empty one. Returns false (OutNonBlack = 0)
    // when the file cannot be loaded/decoded so the caller can tell "not checked" from
    // "checked and empty".
    inline bool PngHasAnyNonBlackPixel(const FString& PngPath, int64& OutNonBlack, int64& OutTotal)
    {
        OutNonBlack = 0;
        OutTotal = 0;
        FImage Loaded;
        if (!FImageUtils::LoadImage(*PngPath, Loaded))
        {
            return false;
        }
        Loaded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
        TArrayView64<FColor> Pixels = Loaded.AsBGRA8();
        if (Pixels.Num() == 0)
        {
            return false;
        }
        OutTotal = Pixels.Num();
        for (const FColor& C : Pixels)
        {
            if (C.R != 0 || C.G != 0 || C.B != 0)
            {
                ++OutNonBlack;
            }
        }
        return true;
    }
}

// ============================================================================
// PRIMARY: the shipped ui.screenshot verb never emits an empty file.
//
// Runs in every environment. Interactive/PIE runs reach the success branch and assert the
// written PNG carries real pixels. Headless runs (no game viewport) reach the failure
// branch and assert the verb failed TYPED and wrote nothing — which is the same contract
// stated from the other side, and is precisely the case the pre-fix code got wrong by
// shipping an opaque-black file with success:true.
//
// Counterfactual: delete the IsBlankReadback guard in
// PinWrightScreenshotUtils::CaptureGameViewportToPngFile. On any host whose viewport reads
// back all zeros the verb then returns success with a black PNG on disk, so the
// non-black-pixel assertion below fails. Reverting the guard cannot make this test pass by
// routing around it, because the test never touches the helper — it goes through the
// dispatcher's registered handler.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiScreenshotNeverWritesBlankFrameTest,
    "PinWright.ui.screenshot.NeverWritesBlankFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUiScreenshotNeverWritesBlankFrameTest::RunTest(const FString& Parameters)
{
    using namespace ScreenshotBlankCaptureTestHelpers;

    // A GUID-unique name so a stale file from an earlier run can never be mistaken for
    // this call's output — the test asserts on file PRESENCE, so a collision would
    // silently invert the meaning of the headless branch.
    const FString OutDir = FPaths::ProjectSavedDir() / TEXT("Screenshots") / TEXT("PinWrightTests");
    const FString OutName = FString::Printf(TEXT("blank_capture_%s.png"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString OutPath = FPaths::Combine(OutDir, OutName);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), OutDir);
    Payload->SetStringField(TEXT("filename"), OutName);
    // Skip the base64 echo: this test asserts on the file, and a full-frame base64 string
    // would bloat the automation log for no added coverage.
    Payload->SetBoolField(TEXT("returnBase64"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("ui.screenshot handler is registered"),
        InvokeHandlerWithCapture(TEXT("ui.screenshot"), Payload, Capture));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*OutPath, false, true);
    };

    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    if (!Capture.bSuccess)
    {
        // No frame could be produced. The contract is that the verb says so with a typed
        // code AND leaves no file behind — a valid-looking empty .png is the exact
        // forbidden outcome, whether or not the response also reports failure.
        TestTrue(FString::Printf(TEXT("failure is typed (got '%s')"), *Capture.ErrorCode),
            IsTypedCaptureFailure(Capture.ErrorCode));
        TestFalse(TEXT("a failed capture leaves no file on disk"),
            IFileManager::Get().FileExists(*OutPath));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-game-viewport"),
            FString::Printf(
                TEXT("Skipped pixel check: no capturable game viewport in this run (%s)."),
                *Capture.ErrorCode));
        return true;
    }

    TestTrue(TEXT("success result exists"), Capture.Result.IsValid());
    if (!Capture.Result.IsValid())
    {
        return false;
    }

    // A success must be backed by a real file with real dimensions.
    FString ReportedPath;
    TestTrue(TEXT("success result carries screenshotPath"),
        Capture.Result->TryGetStringField(TEXT("screenshotPath"), ReportedPath));
    TestTrue(TEXT("reported screenshot exists on disk"),
        !ReportedPath.IsEmpty() && IFileManager::Get().FileExists(*ReportedPath));
    if (ReportedPath.IsEmpty() || !IFileManager::Get().FileExists(*ReportedPath))
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*ReportedPath, false, true);
    };

    double ReportedWidth = 0.0;
    double ReportedHeight = 0.0;
    Capture.Result->TryGetNumberField(TEXT("width"), ReportedWidth);
    Capture.Result->TryGetNumberField(TEXT("height"), ReportedHeight);
    TestTrue(TEXT("reported dimensions are positive"), ReportedWidth > 0.0 && ReportedHeight > 0.0);
    TestTrue(TEXT("reported file is non-trivial in size"),
        IFileManager::Get().FileSize(*ReportedPath) > 0);

    // The core regression assertion: a successful capture must contain actual picture data.
    // The pre-fix empty readback produced a uniform opaque-black frame, so zero non-black
    // pixels is precisely the shipped defect.
    int64 NonBlack = 0;
    int64 Total = 0;
    const bool bDecoded = PngHasAnyNonBlackPixel(ReportedPath, NonBlack, Total);
    TestTrue(TEXT("written screenshot decodes as an image"), bDecoded);
    if (bDecoded)
    {
        TestTrue(
            FString::Printf(TEXT("a successful screenshot is not an all-black frame (%lld of %lld pixels non-black)"),
                NonBlack, Total),
            NonBlack > 0);
    }
    return true;
}

// ============================================================================
// COMPANION: pins IsBlankReadback's semantics headlessly.
//
// This is deliberately a helper-level test and does NOT substitute for the verb-level one
// above — it exists so the false-positive boundary (a dark but genuine frame must NOT be
// called blank) is asserted on hosts that can never bring up a viewport.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScreenshotIsBlankReadbackContractTest,
    "PinWright.render.capture.IsBlankReadbackContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScreenshotIsBlankReadbackContractTest::RunTest(const FString& Parameters)
{
    const int32 PixelCount = 64;

    // A never-drawn surface: every channel zero.
    TArray<FColor> AllZero;
    AllZero.Init(FColor(0, 0, 0, 0), PixelCount);
    TestTrue(TEXT("an all-zero readback is blank"),
        PinWrightScreenshotUtils::IsBlankReadback(AllZero));

    // The false-positive boundary that matters most: the scene-only readback path carries
    // alpha 0 over every scene pixel (the B-horizontal-orthographic-views-render-no-geometry
    // layout), so a genuine dark frame differs from an empty one ONLY in the colour planes.
    // A single near-black but non-zero pixel must keep the frame out of the blank bucket.
    TArray<FColor> NearlyBlackScene;
    NearlyBlackScene.Init(FColor(0, 0, 0, 0), PixelCount);
    NearlyBlackScene[PixelCount / 2] = FColor(1, 0, 0, 0);
    TestFalse(TEXT("a dark frame with any non-zero colour is not blank"),
        PinWrightScreenshotUtils::IsBlankReadback(NearlyBlackScene));

    // Opaque black (alpha already stamped) is a real frame, not an empty one.
    TArray<FColor> OpaqueBlack;
    OpaqueBlack.Init(FColor(0, 0, 0, 255), PixelCount);
    TestFalse(TEXT("opaque black is not blank"),
        PinWrightScreenshotUtils::IsBlankReadback(OpaqueBlack));

    // "Nothing was read" is a size failure the call sites report as CAPTURE_FAILED;
    // labelling it blank would mislabel the failure.
    TArray<FColor> Empty;
    TestFalse(TEXT("an empty bitmap is not reported blank"),
        PinWrightScreenshotUtils::IsBlankReadback(Empty));

    // Ordering guard: ForceOpaqueAlpha must never run before the blank check, because it
    // erases the very signal the check reads. This asserts the hazard directly, so the
    // ordering requirement is covered by a test and not only by a comment.
    TArray<FColor> StampedAfterwards = AllZero;
    PinWrightScreenshotUtils::ForceOpaqueAlpha(StampedAfterwards);
    TestFalse(TEXT("ForceOpaqueAlpha destroys blankness — it must run after the check"),
        PinWrightScreenshotUtils::IsBlankReadback(StampedAfterwards));

    return true;
}
