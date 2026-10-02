// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Math/Color.h"

class SWidget;
class UMaterialInterface;
class UTexture;
class UWidgetBlueprint;

// Safe Designer-tier preview capture, extracted from WidgetDesignerScreenshotHandler so
// the asset.dump aspect path can reuse it without duplicating the screenshot pipeline.
//
// CapturePreviewToPng MUST stay on the Designer/SlatePreview path — never the offscreen
// fallback — because extension-point widgets crash on the offscreen renderer
// (B-widget-describe-offscreen-crash-extension-point).
namespace WidgetDesignerCaptureUtil
{
    // Optional out-info populated alongside OutPng on success. Mirrors the metadata the
    // widget.screenshot_designer RPC reports back to the caller (width/height/dpiScale/
    // previewName) so the handler can keep its existing response shape after delegating,
    // plus the two alpha facts StampOpaqueAndEncodePng produces.
    struct FCaptureInfo
    {
        int32 Width = 0;
        int32 Height = 0;
        float DpiScale = 1.0f;
        FString PreviewName;

        // True once the read-back pixels went through StampOpaqueAndEncodePng's
        // PinWrightScreenshotUtils::ForceOpaqueAlpha pass. Always true on a successful
        // preview capture; it is a field rather than a constant so a caller reading the
        // response can tell "stamped" from "this build predates the stamp".
        bool bOpaqueStamped = false;

        // Fraction (0..1) of read-back pixels whose alpha was exactly 0 BEFORE the stamp.
        // This is the information the stamp destroys, published so a caller that wanted the
        // designer preview's real transparency knows exactly how much there was. Unlike a
        // scene capture, a widget's alpha 0 is legitimate content: FWidgetRenderer draws
        // onto a render target cleared to FLinearColor::Transparent, so an unfilled region
        // of the preview is genuinely transparent rather than a back-buffer artefact.
        // Meaningless after the stamp — measured pre-stamp is the only way it is ever
        // non-zero.
        double AlphaZeroFraction = 0.0;

        // READINESS (B-screenshot-designer-cold-capture-missing-images-and-text). A cold preview
        // draws textures that are still compiling as the engine's placeholder (or nothing) and
        // text whose font material has no game-thread shader map as nothing at all, while
        // reporting the same success shape as a finished frame. Before drawing, the capture
        // collects every UTexture and UMaterialInterface the preview tree references and waits
        // for them, bounded by WidgetDesignerCaptureInternal::ReadinessBudgetSeconds().
        // These are measured off those objects as the frame is drawn, never assumed.
        int32 ReadinessTextures = 0;
        int32 ReadinessMaterials = 0;
        double ReadinessWaitSeconds = 0.0;
        // Object paths still compiling when the budget ran out. Empty <=> bPreviewComplete.
        TArray<FString> NotReadyAssets;
        bool bPreviewComplete = false;

        // Designer dashed-outline wrappers (UWidget::CreateDesignerOutline) collapsed for the
        // render and restored afterwards (B-screenshot-designer-preview-draws-dashed-outlines).
        int32 DesignerOutlinesHidden = 0;
    };

    // Measures the pre-stamp alpha-zero fraction, stamps every pixel opaque, and encodes the
    // result as PNG bytes. This is the ONLY path from read-back pixels to PNG bytes in this
    // util, which is what makes "the preview path is stamped" a structural property of the
    // translation unit rather than a call-site convention that a later edit can quietly drop.
    //
    // ColorData is stamped in place. OutAlphaZeroFractionBeforeStamp is written before the
    // stamp; reading it afterwards off the buffer would always yield 0.
    //
    // Returns false with OutError = "ENCODE_FAILED" on a degenerate size, a short buffer, or
    // an encoder that produced no bytes. OutPng is emptied on failure.
    PINWRIGHT_API bool StampOpaqueAndEncodePng(
        int32 Width,
        int32 Height,
        TArray<FColor>& ColorData,
        TArray<uint8>& OutPng,
        double& OutAlphaZeroFractionBeforeStamp,
        FString& OutError);

    // Draws a Slate widget into an off-screen render target and hands back the read-back
    // pixels. This is the ONLY route from a widget to pixels in this util, and the single
    // place the sRGB pairing is decided, which is what makes "the preview is encoded exactly
    // once" a property of the translation unit rather than of one call site.
    //
    // The returned FColor bytes are sRGB-ENCODED, once: the render target carries
    // TexCreate_SRGB so its ROP performs the encode, and the Slate shader is therefore run in
    // linear space. Encoding again anywhere downstream is the defect this pairing exists to
    // prevent (B-screenshot-designer-double-srgb) — a PNG whose every colour reads roughly
    // sRGB_encode(correct) and looks merely "washed out" rather than broken.
    //
    // OutColorData is Width*Height in row-major order and is emptied on failure. Returns
    // false with OutError = "PREVIEW_ZERO_SIZE" on a degenerate size, "RT_CREATE_FAILED" when
    // no render target could be allocated (this is also what a host that cannot render
    // reports), or "READ_PIXELS_FAILED" when the read-back itself failed.
    PINWRIGHT_API bool RenderSlateWidgetToSrgbColors(
        const TSharedRef<SWidget>& Widget,
        FIntPoint DrawSize,
        TArray<FColor>& OutColorData,
        FString& OutError);

    // Every UTexture and UMaterialInterface a draw of Root's object tree samples: brushes
    // (UImage, UBorder, button/slider styles), font materials (FSlateFontInfo::FontMaterial),
    // atlas sprites, and the textures those materials sample. Reflection-only, one reference
    // level from Root and every object nested under it, so child user widgets of a preview are
    // covered. A resource reached only through a style CLASS's CDO, or through a soft pointer
    // loaded later, is not collected. CapturePreviewToPng waits on this set before drawing.
    PINWRIGHT_API void CollectRenderAssets(
        UObject* Root,
        TArray<UTexture*>& OutTextures,
        TArray<UMaterialInterface*>& OutMaterials);

    // Renders the Widget Blueprint's Designer preview into a PNG byte buffer. Caller
    // chooses the destination path; util never writes to disk.
    //
    // The encoded PNG is OPAQUE: every pixel's alpha is 0xFF. This changed the output bytes
    // for existing callers — widget.screenshot_designer with target:"preview" and
    // asset.dump's widget aspect (preview.png) both used to emit the render target's raw
    // alpha, so a designer preview with a transparent background reached disk as a
    // near-fully-transparent PNG that reads as blank in any alpha-compositing viewer while
    // its RGB was intact. FCaptureInfo::AlphaZeroFraction reports how much transparency was
    // stamped over.
    //
    // Returns false on any failure with OutError set to a stable diagnostic string.
    // OutPng is left empty on failure. OutInfo is optional.
    PINWRIGHT_API bool CapturePreviewToPng(
        UWidgetBlueprint* WidgetBlueprint,
        int32 MaxSize,
        TArray<uint8>& OutPng,
        FString& OutError,
        FCaptureInfo* OutInfo = nullptr,
        bool bDesignerAlreadyOpen = false);
}
