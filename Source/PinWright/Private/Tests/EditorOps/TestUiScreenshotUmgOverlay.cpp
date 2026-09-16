// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-screenshot-omits-umg-overlay: the shared game-viewport
// capture path (CaptureGameViewportToPngFile, used by ui.screenshot and
// editor.screenshot's PIE branch) must include the live Slate/UMG viewport overlay
// (HUD widgets added via AddToViewport) — not just the 3D scene render target.
#include "Misc/AutomationTest.h"
#include "Utils/ScreenshotUtils.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Math/Color.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Rendering/SlateRenderer.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Tests/TestSkipReporting.h"

// CaptureGameViewportToPngFile must composite the window-level UMG/Slate overlay into
// the captured frame. With a full-viewport opaque overlay pushed through the same
// mechanism UMG AddToViewport uses (UGameViewportClient::AddViewportWidgetContent ->
// the game layer manager's window overlay), the saved PNG must read as the overlay
// color across the frame.
//
// Counterfactual: the reverted (scene-only) implementation returns
// GEngine->GameViewport->Viewport->ReadPixels, which reads the viewport scene render
// target and silently drops the window-layer overlay — the sampled pixels are the 3D
// scene, never the overlay color, and the dominance assertion below fails.
//
// Guarded to skip when no game/PIE viewport is bound (the headless unit suite, where
// there is no live viewport to composite onto) — the same live-viewport guard the
// sibling editor.screenshot game-viewport regression (FEditorScreenshotGameViewportSyncTest)
// uses, so it never false-negatives. It exercises the overlay-composite path in the
// interactive/PIE runs where this defect surfaces.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiScreenshotIncludesViewportUmgOverlayTest,
    "PinWright.ui.screenshot.IncludesViewportUmgOverlay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUiScreenshotIncludesViewportUmgOverlayTest::RunTest(const FString& Parameters)
{
    if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-game-viewport"),
            TEXT("Skipped: no game/PIE viewport bound; overlay-composite path not exercised."));
        return true;
    }
    if (!FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-slate-renderer"),
            TEXT("Skipped: Slate not initialized; back-buffer capture unavailable."));
        return true;
    }

    UGameViewportClient* GameViewportClient = GEngine->GameViewport;

    // Pure saturated green is chosen because it is extremely unlikely to dominate a 3D
    // scene frame, so a green-dominated capture unambiguously proves the overlay was
    // composited rather than the scene leaking through.
    const FLinearColor OverlayColor(0.0f, 1.0f, 0.0f, 1.0f);
    TSharedRef<SColorBlock> Overlay = SNew(SColorBlock).Color(OverlayColor);
    GameViewportClient->AddViewportWidgetContent(Overlay, /*ZOrder=*/9999);
    ON_SCOPE_EXIT
    {
        GameViewportClient->RemoveViewportWidgetContent(Overlay);
    };

    // Pump Slate so the overlay paints into the window back buffer before the capture.
    FSlateApplication& SlateApp = FSlateApplication::Get();
    for (int32 Pump = 0; Pump < 3; ++Pump)
    {
        SlateApp.PumpMessages();
        SlateApp.Tick(ESlateTickType::All);
    }
    if (FSlateRenderer* Renderer = SlateApp.GetRenderer())
    {
        Renderer->FlushCommands();
    }

    const FString OutPath = FPaths::ProjectSavedDir() / TEXT("Screenshots") / TEXT("test_umg_overlay_capture.png");
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*OutPath, false, true);
    };

    int32 Width = 0;
    int32 Height = 0;
    FString CaptureError;
    TArray<uint8> PngData;
    const bool bCaptured = PinWrightScreenshotUtils::CaptureGameViewportToPngFile(
        OutPath, Width, Height, CaptureError, &PngData);
    TestTrue(FString::Printf(TEXT("capture succeeds (err=%s)"), *CaptureError), bCaptured);
    if (!bCaptured)
    {
        return false;
    }
    TestTrue(TEXT("positive dimensions"), Width > 0 && Height > 0);
    TestTrue(TEXT("png bytes returned"), PngData.Num() > 0);
    if (PngData.Num() == 0)
    {
        return false;
    }

    // Decode the PNG and sample a spread of points across the frame. With the overlay
    // composited every sample is overlay-green; the reverted scene-only path leaves scene
    // pixels there instead.
    IImageWrapperModule& ImageWrapperModule =
        FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
    TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
    const bool bDecoded = ImageWrapper.IsValid()
        && ImageWrapper->SetCompressed(PngData.GetData(), PngData.Num());
    TestTrue(TEXT("png decodable"), bDecoded);
    if (!bDecoded)
    {
        return false;
    }

    TArray<uint8> Raw;
    const bool bRaw = ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, Raw);
    const int32 DecodedWidth = ImageWrapper->GetWidth();
    const int32 DecodedHeight = ImageWrapper->GetHeight();
    TestTrue(TEXT("raw pixels decoded"),
        bRaw && DecodedWidth > 0 && DecodedHeight > 0 && Raw.Num() >= DecodedWidth * DecodedHeight * 4);
    if (!bRaw || DecodedWidth <= 0 || DecodedHeight <= 0 || Raw.Num() < DecodedWidth * DecodedHeight * 4)
    {
        return false;
    }

    // BGRA8: a pixel reads as overlay-green when green dominates and red/blue are low.
    auto IsOverlayGreen = [&Raw, DecodedWidth](int32 X, int32 Y) -> bool
    {
        const int32 Idx = (Y * DecodedWidth + X) * 4;
        const uint8 B = Raw[Idx + 0];
        const uint8 G = Raw[Idx + 1];
        const uint8 R = Raw[Idx + 2];
        return G > 180 && R < 80 && B < 80;
    };

    const TArray<FIntPoint> Samples = {
        FIntPoint(DecodedWidth / 2, DecodedHeight / 2),
        FIntPoint(DecodedWidth / 4, DecodedHeight / 4),
        FIntPoint((DecodedWidth * 3) / 4, (DecodedHeight * 3) / 4),
        FIntPoint(DecodedWidth / 4, (DecodedHeight * 3) / 4),
        FIntPoint((DecodedWidth * 3) / 4, DecodedHeight / 4),
    };
    int32 GreenSamples = 0;
    for (const FIntPoint& Sample : Samples)
    {
        if (IsOverlayGreen(Sample.X, Sample.Y))
        {
            ++GreenSamples;
        }
    }

    TestTrue(
        FString::Printf(TEXT("captured frame shows the UMG overlay (green samples=%d/%d)"),
            GreenSamples, Samples.Num()),
        GreenSamples >= 4);
    return true;
}
