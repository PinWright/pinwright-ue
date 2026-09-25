// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveSetOfMarkRenderer.h"

#include "Handlers/Drive/DriveEditorChrome.h"
#include "Handlers/Render/BitmapPaint.h"
#include "Utils/ScreenshotUtils.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "UnrealClient.h"
#include "Widgets/SViewport.h"

// Uniquely-named namespace (not anonymous) so Unity-build TU merges can't ODR-clash
// these constants with same-named symbols elsewhere. The pixel/rect/box/font primitives
// that used to live here are now PinWrightBitmapPaint (Handlers/Render/BitmapPaint.h) -
// this file was one of the two private copies that header was extracted from, and the
// other (PinWrightAnnotatedCapture) existed only because these were unreachable.
namespace PinWrightDriveSomRender
{
    // Overlay colors. A red badge/outline with white digit ink reads against most
    // game content without a per-pixel contrast pass.
    static const FColor GMarkColor(220, 32, 32, 255);
    static const FColor GLabelColor(255, 255, 255, 255);

    // Box outline stroke thickness (px) and the padding (px) around the digit block
    // inside the number badge.
    static constexpr int32 GBorderThickness = 2;
    static constexpr int32 GBadgePadding = 2;

    // Per-font-pixel block scale; the font cell is 3x5, so a glyph is 6x10 at scale 2.
    static constexpr int32 GDigitScale = 2;
}

void FDriveSetOfMarkRenderer::DrawMarks(TArray<FColor>& Pixels, int32 Width, int32 Height,
    const FDriveMarkLayout& Layout)
{
    using namespace PinWrightDriveSomRender;

    // FSurface::IsValid() is the same short-buffer rejection this function used to spell out.
    const PinWrightBitmapPaint::FSurface Surface(Pixels, Width, Height);
    if (!Surface.IsValid())
    {
        return;
    }

    // FDriveMark::LabelAnchor is the BADGE corner, not the text corner - hence PlateTopLeft.
    PinWrightBitmapPaint::FLabelStyle BadgeStyle;
    BadgeStyle.Scale = GDigitScale;
    BadgeStyle.PlatePaddingPx = GBadgePadding;
    BadgeStyle.Anchor = PinWrightBitmapPaint::ELabelAnchor::PlateTopLeft;
    BadgeStyle.Plate = GMarkColor;
    BadgeStyle.Ink = GLabelColor;
    BadgeStyle.bDrawPlate = true;

    for (const FDriveMark& Mark : Layout.Marks)
    {
        const int32 MinX = FMath::RoundToInt(Mark.Box.Min.X);
        const int32 MinY = FMath::RoundToInt(Mark.Box.Min.Y);
        const int32 MaxX = FMath::RoundToInt(Mark.Box.Max.X);
        const int32 MaxY = FMath::RoundToInt(Mark.Box.Max.Y);
        PinWrightBitmapPaint::StrokeBox(Surface, MinX, MinY, MaxX, MaxY, GBorderThickness, GMarkColor);

        const int32 AnchorX = FMath::RoundToInt(Mark.LabelAnchor.X);
        const int32 AnchorY = FMath::RoundToInt(Mark.LabelAnchor.Y);
        // Mark numbers are 1-based and non-negative (FDriveSetOfMarkLayout), so the shared font's
        // extra glyph coverage over the old digit-only table cannot change a painted badge.
        PinWrightBitmapPaint::DrawLabel(Surface, FString::FromInt(Mark.Number), AnchorX, AnchorY, BadgeStyle);
    }
}

FColor FDriveSetOfMarkRenderer::MarkColor()
{
    return PinWrightDriveSomRender::GMarkColor;
}

FColor FDriveSetOfMarkRenderer::LabelColor()
{
    return PinWrightDriveSomRender::GLabelColor;
}

bool FDriveSetOfMarkRenderer::CaptureAnnotated(const TArray<FDriveElement>& Elements,
    int32 MarkCap, FDriveScreenshot& OutScreenshot, FString& OutErrorCode,
    EDriveSurface Surface, const FDriveWindowSelector& WindowSelector, bool bWriteToFile)
{
    // 1) Live capture into a raw FColor bitmap, plus the desktop position of its (0,0) pixel
    // (element geometry is desktop space; see FDriveSetOfMarkLayout::BuildLayout). The source
    // depends on the surface: the editor-chrome path captures the selected top-level window; the
    // game AND web paths capture the game/PIE viewport widget's rect of its window back buffer -
    // the composited frame (scene, post-process, and the Slate/UMG game layers, including the
    // CEF/WebUI HUD), the same pixels editor.screenshot reports as captureMode:nativeBackBuffer.
    // Both kept in-memory (no disk write) so we can paint marks before encoding.
    TArray<FColor> Bitmap;
    int32 Width = 0;
    int32 Height = 0;
    FVector2D FrameOrigin = FVector2D::ZeroVector;

    if (Surface == EDriveSurface::EditorChrome)
    {
        // CaptureWindow sets OutErrorCode (window-resolution codes / CAPTURE_FAILED) on failure.
        if (!FDriveEditorChrome::CaptureWindow(WindowSelector, Bitmap, Width, Height, FrameOrigin, OutErrorCode))
        {
            return false;
        }
    }
    else
    {
        // Game and Web: the game/PIE viewport.
        if (!GEngine || !GEngine->GameViewport)
        {
            OutErrorCode = TEXT("NO_VIEWPORT");
            return false;
        }
        FViewport* Viewport = GEngine->GameViewport->Viewport;
        if (!Viewport)
        {
            OutErrorCode = TEXT("NO_VIEWPORT");
            return false;
        }

        const TSharedPtr<SViewport> ViewportWidget = GEngine->GameViewport->GetGameViewportWidget();
        if (ViewportWidget.IsValid())
        {
            // Both the back-buffer rect and the scene render target start at the viewport
            // widget's desktop position - read from the same cached geometry the element walk uses.
            FrameOrigin = ViewportWidget->GetCachedGeometry().GetAbsolutePosition();
            if (FSlateApplication::IsInitialized())
            {
                // Never FSlateApplication::TakeScreenshot directly; contract in ScreenshotUtils.h.
                FIntVector ImageSize(0, 0, 0);
                if (PinWrightScreenshotUtils::TakeSlateScreenshot(
                        StaticCastSharedRef<SWidget>(ViewportWidget.ToSharedRef()), Bitmap, ImageSize)
                    && ImageSize.X > 0 && ImageSize.Y > 0)
                {
                    Width = ImageSize.X;
                    Height = ImageSize.Y;
                }
                else
                {
                    Bitmap.Reset();
                }
            }
        }

        // Fallback: scene-only render target (no UMG, no post-process UI blur), for when the back
        // buffer is unavailable (headless / -RenderOffScreen) - the same fallback
        // CaptureGameViewportToPngFile takes.
        if (Bitmap.Num() == 0)
        {
            if (!Viewport->ReadPixels(Bitmap) || Bitmap.Num() == 0)
            {
                OutErrorCode = TEXT("CAPTURE_FAILED");
                return false;
            }
            const FIntPoint Size = Viewport->GetSizeXY();
            Width = Size.X;
            Height = Size.Y;
        }
    }

    if (Width <= 0 || Height <= 0 || Bitmap.Num() < Width * Height)
    {
        OutErrorCode = TEXT("CAPTURE_FAILED");
        return false;
    }

    // Same never-drawn-surface rejection the shared game-viewport->PNG path performs, and for
    // the same reason: it must run BEFORE ForceOpaqueAlpha, which would otherwise turn an
    // all-zero readback into opaque black. Painting marks over that black frame made it worse
    // than a plain empty capture — the marks look like a successful observation of a scene
    // that was never rendered. Contract in ScreenshotUtils.h.
    if (PinWrightScreenshotUtils::IsBlankReadback(Bitmap))
    {
        OutErrorCode = TEXT("BLANK_CAPTURE");
        return false;
    }

    // Force alpha opaque before the marks are painted; contract in ScreenshotUtils.h. The
    // FViewport::ReadPixels branch above carries alpha 0 over every scene pixel, and while the
    // primary ThumbnailCompressImageArray encode below happens to drop alpha (it emits JPEG for
    // any image >= 8x8), the IImageWrapper PNG fallback right after it encodes BGRA verbatim and
    // would emit a near-fully-transparent frame (B-horizontal-orthographic-views-render-no-geometry).
    // Stamping here keeps both encoders honest rather than relying on the first one's format.
    PinWrightScreenshotUtils::ForceOpaqueAlpha(Bitmap);

    // 2) Filter to interactables, then lay out and paint the marks.
    TArray<FDriveElement> Interactables;
    Interactables.Reserve(Elements.Num());
    for (const FDriveElement& Element : Elements)
    {
        if (Element.bInteractable)
        {
            Interactables.Add(Element);
        }
    }

    const int32 EffectiveCap = MarkCap > 0 ? MarkCap : DefaultMarkCap;
    const FDriveMarkLayout Layout =
        FDriveSetOfMarkLayout::BuildLayout(Interactables, FrameOrigin, Width, Height, EffectiveCap);

    DrawMarks(Bitmap, Width, Height, Layout);

    // 3) PNG-encode the annotated bitmap to a byte buffer (no disk write). Mirrors
    // ScreenshotUtils: try the thumbnail compressor first, then the ImageWrapper
    // BGRA path (FColor is laid out B,G,R,A so the raw buffer feeds in directly).
    TArray<uint8> PngData;
    FImageUtils::ThumbnailCompressImageArray(Width, Height, Bitmap, PngData);
    if (PngData.Num() == 0)
    {
        IImageWrapperModule& ImageWrapperModule =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
        TSharedPtr<IImageWrapper> ImageWrapper =
            ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        if (ImageWrapper.IsValid() &&
            ImageWrapper->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor),
                Width, Height, ERGBFormat::BGRA, 8))
        {
            PngData = ImageWrapper->GetCompressed(100);
        }
    }
    if (PngData.Num() == 0)
    {
        OutErrorCode = TEXT("ENCODE_FAILED");
        return false;
    }

    // 4) Deliver the encoded bytes either inline (base64) or to a file (path), then
    // populate the rest of the contract. DrawMarks paints every mark the layout produced, so
    // MarksDrawn is exactly the set of FDriveMark::Number values (gaps preserved);
    // MarksOmitted mirrors the layout's omissions.
    if (!DeliverScreenshotBytes(PngData, bWriteToFile, OutScreenshot, OutErrorCode))
    {
        return false;
    }
    OutScreenshot.Mime = TEXT("image/png");
    OutScreenshot.Width = Width;
    OutScreenshot.Height = Height;

    OutScreenshot.MarksDrawn.Reset();
    OutScreenshot.MarksDrawn.Reserve(Layout.Marks.Num());
    for (const FDriveMark& Mark : Layout.Marks)
    {
        OutScreenshot.MarksDrawn.Add(Mark.Number);
    }
    OutScreenshot.MarksOmitted = Layout.Omitted;

    OutErrorCode.Reset();
    return true;
}

bool FDriveSetOfMarkRenderer::DeliverScreenshotBytes(const TArray<uint8>& PngData,
    bool bWriteToFile, FDriveScreenshot& OutScreenshot, FString& OutErrorCode)
{
    if (bWriteToFile)
    {
        // Route the encoded PNG to disk (as editor.screenshot / ui.screenshot already do)
        // instead of inlining ~1.33x its bytes as base64, so the observation payload stays
        // small and the caller can Read the marked image directly. MakeScreenshotOutputPath
        // creates the Saved/Screenshots/Drive directory and returns a timestamped path.
        FString OutFilename;
        const FString OutPath = PinWrightScreenshotUtils::MakeScreenshotOutputPath(
            FString(), TEXT("DriveObserve"), TEXT("Drive"), OutFilename);
        if (!FFileHelper::SaveArrayToFile(PngData, *OutPath))
        {
            OutErrorCode = TEXT("WRITE_FAILED");
            return false;
        }
        OutScreenshot.Path = OutPath;
        OutScreenshot.Base64.Reset();
    }
    else
    {
        OutScreenshot.Base64 = FBase64::Encode(PngData);
        OutScreenshot.Path.Reset();
    }
    return true;
}
