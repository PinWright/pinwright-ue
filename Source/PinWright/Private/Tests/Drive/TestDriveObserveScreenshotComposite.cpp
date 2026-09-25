// Copyright (c) 2026 Alexander Penkin. MIT License.

// Live regression test for drive.observe's game-surface Set-of-Mark screenshot:
//  - B-drive-observe-screenshot-omits-umg: the frame must be the composited back buffer, so the
//    Slate/UMG game layers the element list describes are in the image.
//  - B-set-of-mark-layout-ignores-surface-origin: a mark must land on its element's pixels, i.e.
//    at desktop rect minus the viewport widget's desktop position.
#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveSetOfMarkRenderer.h"
#include "Handlers/Drive/DriveTypes.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Base64.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/SViewport.h"
#include "Tests/AutomationEditorCommon.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/PieState.h"

namespace DriveObserveCompositeTestHelpers
{
    struct FState
    {
        TSharedPtr<SColorBlock> Overlay;
        double Deadline = 0.0;
        double CaptureNotBefore = 0.0;
        uint64 CaptureNotBeforeFrame = 0;
        double CleanupDeadline = 0.0;
    };

    void RemoveOverlay(FState& State)
    {
        if (State.Overlay.IsValid() && GEngine && GEngine->GameViewport)
        {
            GEngine->GameViewport->RemoveViewportWidgetContent(State.Overlay.ToSharedRef());
        }
        State.Overlay.Reset();
    }

    // The capture + assertions, run once the owned PIE viewport has painted the overlay.
    void CaptureAndAssert(FAutomationTestBase& Test)
    {
        UGameViewportClient* GameViewportClient = GEngine->GameViewport;
        const FVector2D ViewportOrigin =
            GameViewportClient->GetGameViewportWidget()->GetCachedGeometry().GetAbsolutePosition();
        Test.AddInfo(FString::Printf(TEXT("viewport desktop origin = (%.1f, %.1f)"), ViewportOrigin.X, ViewportOrigin.Y));

        constexpr int32 LocalX = 100;
        constexpr int32 LocalY = 100;
        constexpr int32 BoxW = 120;
        constexpr int32 BoxH = 120;
        FDriveElement Element;
        Element.Handle = TEXT("test/mark");
        Element.bInteractable = true;
        Element.AbsolutePosition = ViewportOrigin + FVector2D(LocalX, LocalY);
        Element.AbsoluteSize = FVector2D(BoxW, BoxH);

        FDriveScreenshot Shot;
        FString Error;
        const bool bCaptured = FDriveSetOfMarkRenderer::CaptureAnnotated(
            { Element }, 0, Shot, Error, EDriveSurface::Game);
        Test.TestTrue(FString::Printf(TEXT("capture succeeds (err=%s)"), *Error), bCaptured);
        if (!bCaptured)
        {
            return;
        }
        Test.TestEqual(TEXT("the on-screen element is marked"), Shot.MarksDrawn.Num(), 1);
        Test.TestEqual(TEXT("nothing omitted"), Shot.MarksOmitted.Num(), 0);

        TArray<uint8> Encoded;
        Test.TestTrue(TEXT("base64 decodes"), FBase64::Decode(Shot.Base64, Encoded) && Encoded.Num() > 0);
        IImageWrapperModule& ImageWrapperModule =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
        const EImageFormat Format = ImageWrapperModule.DetectImageFormat(Encoded.GetData(), Encoded.Num());
        TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(Format);
        TArray<uint8> Raw;
        const bool bDecoded = ImageWrapper.IsValid()
            && ImageWrapper->SetCompressed(Encoded.GetData(), Encoded.Num())
            && ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, Raw);
        const int32 W = bDecoded ? static_cast<int32>(ImageWrapper->GetWidth()) : 0;
        const int32 H = bDecoded ? static_cast<int32>(ImageWrapper->GetHeight()) : 0;
        const bool bUsable = bDecoded && W > LocalX + BoxW && H > LocalY + BoxH && Raw.Num() >= W * H * 4;
        Test.TestTrue(TEXT("image decodes"), bUsable);
        if (!bUsable)
        {
            return;
        }

        // Channel thresholds, not exact values: the encoder may be lossy.
        auto Pixel = [&Raw, W](int32 X, int32 Y) -> FColor
        {
            const int32 Idx = (Y * W + X) * 4;
            return FColor(Raw[Idx + 2], Raw[Idx + 1], Raw[Idx + 0], 255);
        };
        auto IsGreen = [&Pixel](int32 X, int32 Y) { const FColor C = Pixel(X, Y); return C.G > 180 && C.R < 80 && C.B < 80; };
        // Over the green overlay only mark ink can be red-dominant; the Set-of-Mark encode is lossy
        // with chroma subsampling, so a 2 px red line on green is not saturated red any more.
        auto IsRed = [&Pixel](int32 X, int32 Y) { const FColor C = Pixel(X, Y); return C.R > 100 && C.R > C.G; };

        // (a) UMG in the frame: sample well away from the mark.
        const TArray<FIntPoint> Samples = {
            FIntPoint(W / 2, H / 2), FIntPoint((W * 3) / 4, H / 4), FIntPoint((W * 3) / 4, (H * 3) / 4),
            FIntPoint(W / 4, (H * 3) / 4), FIntPoint(W / 2, H - 20) };
        int32 GreenSamples = 0;
        for (const FIntPoint& Sample : Samples)
        {
            GreenSamples += IsGreen(Sample.X, Sample.Y) ? 1 : 0;
        }
        Test.TestTrue(FString::Printf(TEXT("frame shows the UMG overlay (green samples=%d/%d, center=%s)"),
            GreenSamples, Samples.Num(), *Pixel(W / 2, H / 2).ToString()), GreenSamples >= 4);

        // (b) The outline's left edge crosses the middle of the box at local x=100..101, below the
        // badge, so only the outline is red there. +-3 px for rounding and encoder bleed.
        const int32 EdgeY = LocalY + BoxH / 2;
        bool bRedAtLocalEdge = false;
        for (int32 X = LocalX - 3; X <= LocalX + 4; ++X)
        {
            bRedAtLocalEdge |= IsRed(X, EdgeY);
        }
        Test.TestTrue(TEXT("mark outline drawn at the element's frame-local x"), bRedAtLocalEdge);
        Test.TestTrue(TEXT("box interior shows the overlay, not a mark"), IsGreen(LocalX + BoxW / 2, EdgeY));
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FRunOwnedPieDriveObserveComposite,
    FAutomationTestBase*, Test, TSharedRef<DriveObserveCompositeTestHelpers::FState>, State);

bool FRunOwnedPieDriveObserveComposite::Update()
{
    using namespace DriveObserveCompositeTestHelpers;
    const bool bViewportReady = GEditor && GEditor->PlayWorld && GEngine && GEngine->GameViewport
        && GEngine->GameViewport->Viewport && GEngine->GameViewport->GetGameViewportWidget().IsValid();
    if (!bViewportReady)
    {
        if (FPlatformTime::Seconds() < State->Deadline)
        {
            return false;
        }
        PinWrightTestSkip::SkipAssertions(*Test, TEXT("owned_pie_viewport_unavailable"),
            TEXT("The owned PIE session did not bind a game viewport."));
        return true;
    }

    if (!State->Overlay.IsValid())
    {
        // Same mechanism UMG AddToViewport uses. The ZOrder sits above any game overlay (a
        // loading screen is commonly added at 10000 and is still up this early in PIE). Let the
        // viewport present a few frames with it.
        State->Overlay = SNew(SColorBlock).Color(FLinearColor(0.0f, 1.0f, 0.0f, 1.0f));
        GEngine->GameViewport->AddViewportWidgetContent(State->Overlay.ToSharedRef(), /*ZOrder=*/1000000);
        State->CaptureNotBefore = FPlatformTime::Seconds() + 1.0;
        State->CaptureNotBeforeFrame = GFrameCounter + 10;
        return false;
    }
    if (FPlatformTime::Seconds() < State->CaptureNotBefore || GFrameCounter < State->CaptureNotBeforeFrame)
    {
        return false;
    }

    CaptureAndAssert(*Test);
    RemoveOverlay(*State);
    return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FCleanupOwnedPieDriveObserveComposite,
    FAutomationTestBase*, Test, TSharedRef<DriveObserveCompositeTestHelpers::FState>, State);

bool FCleanupOwnedPieDriveObserveComposite::Update()
{
    if (!PinWrightPieState::IsPlayInEditorActive())
    {
        return true;
    }
    if (FPlatformTime::Seconds() >= State->CleanupDeadline)
    {
        Test->AddError(TEXT("Timed out waiting for the owned PIE session to stop."));
        return true;
    }
    return false;
}

// Owns a PIE session (the automation run ends any ambient one), pushes a full-viewport
// opaque-green overlay through the mechanism UMG AddToViewport uses, and places one interactable
// element 100 px into the viewport in desktop space, as the element walk reports it. The annotated
// capture must (a) read green away from the mark and (b) draw the mark's red outline at frame-local
// x=100.
//
// Counterfactuals: the old FViewport::ReadPixels capture is scene-only, so (a) samples scene
// pixels; the old origin-less layout drew the outline at the element's DESKTOP x (100 + the
// viewport's desktop x), so (b) finds no red at local x=100 whenever the PIE viewport is not at the
// desktop origin (the level-editor viewport never is).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveObserveScreenshotCompositeTest,
    "PinWright.drive.observe.ScreenshotIncludesUmgAndMarksAtSurfaceLocalCoords",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveObserveScreenshotCompositeTest::RunTest(const FString& Parameters)
{
    using namespace DriveObserveCompositeTestHelpers;
    if (!GEditor || !FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor_or_slate_unavailable"),
            TEXT("Owned PIE requires GEditor and Slate."));
        return true;
    }
    if (GEditor->PlayWorld || GEditor->IsPlaySessionInProgress())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("preexisting_pie_session"),
            TEXT("The test never borrows or stops ambient PIE."));
        return true;
    }
    if (!GEditor->GetActiveViewport())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level_viewport_unavailable"),
            TEXT("PIE in the level viewport requires an active level viewport."));
        return true;
    }

    // Starting PIE boots the host's game, whose startup may log errors (network, streaming)
    // unrelated to the capture; the assertions below carry the verdict.
    bSuppressLogErrors = true;
    const TSharedRef<FState> State = MakeShared<FState>();
    State->Deadline = FPlatformTime::Seconds() + 20.0;
    State->CleanupDeadline = State->Deadline + 15.0;
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FRunOwnedPieDriveObserveComposite(this, State));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FCleanupOwnedPieDriveObserveComposite(this, State));
    return true;
}
