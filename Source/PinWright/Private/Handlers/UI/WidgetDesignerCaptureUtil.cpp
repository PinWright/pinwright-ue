// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/UI/WidgetDesignerCaptureUtil.h"

#include "Handlers/UI/WidgetDesignerCaptureInternal.h"
#include "Handlers/UI/WidgetGeometryResolver.h"
#include "Blueprint/UserWidget.h"
#include "ImageUtils.h"
#include "Math/UnrealMathUtility.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Utils/ScreenshotUtils.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"

namespace WidgetDesignerCaptureUtil
{

// Counterfactual for Tests/Widget/TestWidgetDesignerCaptureAlpha.cpp: delete the
// PinWrightScreenshotUtils::ForceOpaqueAlpha call below and the uncovered region of a
// preview comes back alpha 0 in the decoded PNG —
// PreviewPathStampsOpaque fails. Move the fraction measurement to AFTER the stamp and it
// reads 0 on every input — AlphaZeroFractionIsMeasuredBeforeTheStamp fails. The test never
// touches alpha itself and never calls ForceOpaqueAlpha directly, which is the lesson
// recorded at Tests/Assets/TestGenerateThumbnail.cpp:18-24: a test that supplies the fix it
// is meant to verify proves nothing.
bool StampOpaqueAndEncodePng(
    int32 Width,
    int32 Height,
    TArray<FColor>& ColorData,
    TArray<uint8>& OutPng,
    double& OutAlphaZeroFractionBeforeStamp,
    FString& OutError)
{
    OutPng.Reset();
    OutError.Reset();
    OutAlphaZeroFractionBeforeStamp = 0.0;

    if (Width <= 0 || Height <= 0 || int64(ColorData.Num()) < int64(Width) * int64(Height))
    {
        OutError = TEXT("ENCODE_FAILED");
        return false;
    }

    // BEFORE the stamp, and it has to be: ForceOpaqueAlpha rewrites A to 0xFF on every
    // pixel, so the same loop run afterwards returns 0 for every possible input. Same
    // ordering rule PinWrightScreenshotUtils::IsBlankReadback carries in ScreenshotUtils.h,
    // and the one TestScreenshotBlankCapture.cpp:239-244 pins.
    int64 AlphaZeroPixels = 0;
    for (const FColor& Pixel : ColorData)
    {
        if (Pixel.A == 0)
        {
            ++AlphaZeroPixels;
        }
    }
    OutAlphaZeroFractionBeforeStamp = double(AlphaZeroPixels) / double(ColorData.Num());

    // FWidgetRenderer draws into a render target whose ClearColor is
    // FLinearColor::Transparent, so an unfilled region of the Designer preview reads back at
    // alpha 0 and encoding it raw yields a PNG that reads as blank in any alpha-compositing
    // viewer while its RGB is intact (B-horizontal-orthographic-views-render-no-geometry is
    // the same failure on the viewport paths). This was the plugin's only capture path with
    // no stamp; the sibling window branch in WidgetDesignerScreenshotHandler.cpp:310 already
    // had one, which is how the gap read as fixed. The transparency that is destroyed here
    // is published as OutAlphaZeroFractionBeforeStamp rather than silently dropped.
    PinWrightScreenshotUtils::ForceOpaqueAlpha(ColorData);

    TArray64<uint8> PngData;
    FImageUtils::PNGCompressImageArray(Width, Height,
        TArrayView64<const FColor>(ColorData.GetData(), ColorData.Num()), PngData);
    if (PngData.Num() == 0)
    {
        OutError = TEXT("ENCODE_FAILED");
        return false;
    }

    // PNG byte count fits in int32: callers clamp MaxSize to 16384, so raw RGBA pixels stay
    // well below INT32_MAX and PNG-compressed output is strictly smaller. Skip Append's
    // per-element loop in favor of a single Memcpy.
    OutPng.SetNumUninitialized(PngData.Num());
    FMemory::Memcpy(OutPng.GetData(), PngData.GetData(), PngData.Num());
    return true;
}

bool RenderSlateWidgetToSrgbColors(
    const TSharedRef<SWidget>& Widget,
    FIntPoint DrawSize,
    TArray<FColor>& OutColorData,
    FString& OutError)
{
    // EXACTLY ONE sRGB ENCODE MUST REACH THE PIXELS, AND THE RENDER TARGET IS THE ONE THAT
    // PERFORMS IT. CreateTargetFor(..., /*bUseGammaCorrection=*/true) leaves bForceLinearGamma
    // false, so UTextureRenderTarget2D::IsSRGB() is true, the RHI texture is created with
    // TexCreate_SRGB, and every write is encoded by the ROP on the way out. The Slate shader
    // must therefore stay in LINEAR space: FWidgetRenderer(true) sets
    // FSlate3DRenderer::bGammaCorrection, which reaches the pixel shader as a DisplayGamma !=
    // 1 and runs LinearToSrgb a second time. That was B-screenshot-designer-double-srgb — a
    // linear tint of (0.0185, 0.0742, 0.1357) read back (106,149,170) where (37,77,103) is
    // correct, every colour wrong by exactly one transfer function while the image still
    // looked plausible.
    //
    // This is the pairing UMG's own widget-to-texture consumer uses (UWidgetComponent's
    // bApplyGammaCorrection=false over a hardware-sRGB target) and the one
    // OrthoTileCaptureUtils relies on for the same reason. It also puts
    // ESlateBatchDrawFlag::NoGamma batches — which skip the shader encode unconditionally — in
    // the same output space as everything else in the frame, instead of one encode behind it.
    //
    // Do NOT correct a wrong colour here by flipping BOTH flags: that yields raw linear bytes,
    // which is a different wrong answer, and never by applying an inverse curve.
    return PinWrightScreenshotUtils::RenderSlateWidgetToSrgbColors(
        Widget, DrawSize, /*DrawScale=*/1.0f, OutColorData, OutError);
}

bool CapturePreviewToPng(
    UWidgetBlueprint* WidgetBlueprint,
    int32 MaxSize,
    TArray<uint8>& OutPng,
    FString& OutError,
    FCaptureInfo* OutInfo,
    bool bDesignerAlreadyOpen)
{
    OutPng.Reset();
    OutError.Reset();
    if (OutInfo)
    {
        *OutInfo = FCaptureInfo{};
    }

    if (!WidgetBlueprint)
    {
        OutError = TEXT("WIDGET_BLUEPRINT_NULL");
        return false;
    }

    if (MaxSize <= 0 || MaxSize > 16384)
    {
        OutError = TEXT("INVALID_MAX_SIZE");
        return false;
    }

    if (!GEditor)
    {
        OutError = TEXT("EDITOR_NOT_AVAILABLE");
        return false;
    }

    FString ReadinessError;
    if (!WidgetDesignerCaptureInternal::QueryDesignerCaptureReadiness(ReadinessError))
    {
        OutError = MoveTemp(ReadinessError);
        return false;
    }

    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!AssetEditorSubsystem)
    {
        OutError = TEXT("SUBSYSTEM_MISSING");
        return false;
    }

    // Restores the editor's prior open/closed state on every exit path, including the error
    // returns below. Without it the asset.dump widget aspect left one Designer open per widget it
    // dumped, and each was an editor holding an asset a later compile would rebuild underneath it
    // (board B-screenshot-designer-leaves-designer-open-compile-crash). When
    // widget.screenshot_designer is the caller its own outer scope already opened the editor, so
    // this one sees bWasAlreadyOpen and owns nothing - the close stays with the scope that opened
    // it.
    WidgetDesignerCaptureInternal::FScopedDesignerAssetEditor ScopedDesigner(WidgetBlueprint);
    if (!ScopedDesigner.IsOpen())
    {
        OutError = TEXT("OPEN_FAILED");
        return false;
    }

    FWidgetBlueprintEditor* WidgetEditor = FWidgetGeometryResolver::FindWidgetBlueprintEditor(
        WidgetBlueprint, true);
    if (!WidgetEditor)
    {
        OutError = TEXT("EDITOR_NOT_FOUND");
        return false;
    }

    FWidgetDesignerPreviewTarget DesignerTarget;
    FString ResolveError;
    if (!WidgetDesignerCaptureInternal::ResolveDesignerPreviewTargetWithRetry(
        WidgetBlueprint, WidgetEditor, DesignerTarget, ResolveError,
        bDesignerAlreadyOpen))
    {
        OutError = ResolveError.IsEmpty() ? TEXT("PREVIEW_NOT_FOUND") : ResolveError;
        return false;
    }
    if (!DesignerTarget.DesignerViewSlate.IsValid() || !DesignerTarget.bHasPreviewCropRect)
    {
        OutError = TEXT("PREVIEW_BOUNDS_NOT_FOUND");
        return false;
    }

    UUserWidget* PreviewWidget = WidgetEditor->GetPreview();
    if (!PreviewWidget)
    {
        OutError = TEXT("PREVIEW_NOT_FOUND");
        return false;
    }

    const FVector2D Natural = DesignerTarget.PreviewGeometry.GetAbsoluteSize();
    const double LongAxis = FMath::Max(Natural.X, Natural.Y);
    if (LongAxis <= 0.0)
    {
        OutError = TEXT("PREVIEW_ZERO_SIZE");
        return false;
    }
    const double Factor = double(MaxSize) / LongAxis;
    const FIntPoint PreviewDrawSize(
        FMath::Max(1, FMath::RoundToInt(Natural.X * Factor)),
        FMath::Max(1, FMath::RoundToInt(Natural.Y * Factor)));

    // Renderer and render target live in RenderSlateWidgetToSrgbColors so the sRGB pairing
    // has one definition; its contract (encoded exactly once, error codes) is on the header.
    TSharedRef<SWidget> PreviewSlate = PreviewWidget->TakeWidget();
    TArray<FColor> ColorData;
    FString RenderError;
    if (!RenderSlateWidgetToSrgbColors(PreviewSlate, PreviewDrawSize, ColorData, RenderError))
    {
        OutError = MoveTemp(RenderError);
        return false;
    }

    // The stamp and the encode are one call so this function cannot reach PNG bytes without
    // stamping. Contract and the byte-level consequence for existing callers are on
    // StampOpaqueAndEncodePng / CapturePreviewToPng in WidgetDesignerCaptureUtil.h.
    double AlphaZeroFraction = 0.0;
    FString EncodeError;
    if (!StampOpaqueAndEncodePng(PreviewDrawSize.X, PreviewDrawSize.Y, ColorData, OutPng,
        AlphaZeroFraction, EncodeError))
    {
        OutError = MoveTemp(EncodeError);
        return false;
    }

    if (OutInfo)
    {
        OutInfo->Width = PreviewDrawSize.X;
        OutInfo->Height = PreviewDrawSize.Y;
        OutInfo->DpiScale = DesignerTarget.DpiScale;
        OutInfo->bOpaqueStamped = true;
        OutInfo->AlphaZeroFraction = AlphaZeroFraction;
        if (DesignerTarget.Preview)
        {
            OutInfo->PreviewName = DesignerTarget.Preview->GetName();
        }
    }
    return true;
}

} // namespace WidgetDesignerCaptureUtil
