// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

// THE CPU rasteriser for every PinWright overlay painted onto a raw FColor readback.
//
// WHY THIS EXISTS. This code shipped TWICE, file-local and unexported: PinWrightDriveSomRender in
// Handlers/Drive/DriveSetOfMarkRenderer.cpp (pixel/rect/box + a digit font) and
// PinWrightAnnotatedCapture in Handlers/Render/AnnotatedCaptureHandler.cpp (the same, plus a
// Bresenham line, an A-Z font and a badged label). The second copy exists precisely because the
// first was unreachable - its own header comment says so. Both call sites now come through here;
// a third copy is the thing to refuse, not to add.
//
// Pure: no UObject, no editor, no RHI, no Slate, no engine I/O. Every write is clamped to the
// surface, so an empty draw list leaves the buffer byte-for-byte untouched and the whole file is
// unit-testable on a synthetic bitmap.
//
// ---- ALPHA COMPOSITING IS A REQUIREMENT, NOT A GARNISH ----
//
// A prototype grid overlay drawn at full opacity BURIED the detail it was drawn to measure: the
// reader could no longer see the thing the grid was supposed to let them locate. Overlays that
// span the frame (grids, guides, seam markers) must composite at low alpha, and labels belong on
// the frame edges rather than in the field. FPaint therefore carries an Opacity alongside the
// colour, and every primitive honours it.
//
// The full-opacity path is an exact byte replace, identical to what both former private copies
// did, so folding them onto this header moved no pixels. Opaque is `Opacity >= 1` AND `Color.A ==
// 255` - a colour whose own alpha is below 255 composites even at Opacity 1.
//
// KNOWN LIMIT: a primitive that covers a pixel twice in one call composites it twice, which reads
// as a darker spot. DrawLine avoids this for thick pens by only painting the newly covered strip
// at each Bresenham step (the union of painted pixels is unchanged, so opaque output is
// identical). FillRect / StrokeBox / the label glyph pass each touch a pixel once. What is NOT
// handled is one translucent call overlapping another - two crossing translucent grid lines are
// darker at the intersection. That is the honest visual, not a defect to hide.
//
// ---- PIXEL CONVENTION ----
// Row-major, TOP-LEFT origin: Pixels[Y * Width + X]. Rect arguments are INCLUSIVE on both ends
// (that is the convention both former copies used and what the existing Drive tests assert).
namespace PinWrightBitmapPaint
{
    // A mutable view over somebody else's FColor buffer. Holds no ownership: the referenced
    // storage must outlive the surface (in practice both live on the same stack frame as the
    // capture readback).
    struct FSurface
    {
        TArrayView<FColor> Pixels;
        int32 Width = 0;
        int32 Height = 0;

        FSurface() = default;
        FSurface(TArrayView<FColor> InPixels, int32 InWidth, int32 InHeight)
            : Pixels(InPixels), Width(InWidth), Height(InHeight)
        {
        }

        // A surface with a short buffer is INVALID, not silently clipped: every primitive
        // no-ops on it, so a truncated readback cannot be half-painted and shipped.
        bool IsValid() const
        {
            return Width > 0 && Height > 0 &&
                Pixels.Num() >= static_cast<int64>(Width) * static_cast<int64>(Height);
        }
    };

    // Colour plus coverage. The implicit FColor constructor keeps opaque call sites reading
    // exactly as they did before the two private copies were folded in here.
    struct FPaint
    {
        FColor Color = FColor::White;
        // 0 = invisible, 1 = fully covering. Multiplies Color.A.
        float Opacity = 1.0f;

        FPaint() = default;
        FPaint(const FColor& InColor) : Color(InColor) {}
        FPaint(const FColor& InColor, float InOpacity) : Color(InColor), Opacity(InOpacity) {}

        // Effective source coverage in [0,1].
        float EffectiveAlpha() const
        {
            return FMath::Clamp(Opacity, 0.0f, 1.0f) * (static_cast<float>(Color.A) / 255.0f);
        }
        // Takes the exact-replace path (byte-identical to the pre-unification behaviour).
        bool IsOpaque() const { return Opacity >= 1.0f && Color.A >= 255; }
    };

    // ---- primitives ----

    // One pixel, dropping any coordinate outside the surface.
    void SetPixel(const FSurface& Surface, int32 X, int32 Y, const FPaint& Paint);

    // Inclusive rect [X0,X1] x [Y0,Y1], clamped to the surface. Each covered pixel is written
    // exactly once, so a translucent fill is uniform.
    void FillRect(const FSurface& Surface, int32 X0, int32 Y0, int32 X1, int32 Y1, const FPaint& Paint);

    // A `Thickness`-px border just INSIDE the inclusive rect edges (the border eats into the
    // rect; it does not grow it). Each border pixel is written exactly once, corners included.
    // Thickness < 1 is treated as 1 (both former copies drew nothing, by accident of their
    // comparison; no call site passes < 1).
    void StrokeBox(const FSurface& Surface, int32 MinX, int32 MinY, int32 MaxX, int32 MaxY,
        int32 Thickness, const FPaint& Paint);

    // Bresenham line with a Thickness x Thickness pen block centred on each plotted point
    // (Thickness < 1 is treated as 1). Endpoints may be off-surface; the run is bounded so two
    // far-off-frame endpoints cannot spin. No pixel is composited twice within one call
    // (see the header note).
    void DrawLine(const FSurface& Surface, int32 X0, int32 Y0, int32 X1, int32 Y1,
        int32 Thickness, const FPaint& Paint);

    // ---- text ----

    // The shared 3x5 bitmap font: digits, A-Z (case-folded) and the handful of punctuation
    // glyphs the overlays need. Unknown characters render BLANK but still advance the pen, so a
    // stray character never corrupts the rest of a label.
    constexpr int32 GlyphCellWidth = 3;
    constexpr int32 GlyphCellHeight = 5;
    // Per-font-pixel block scale used by every existing overlay: a glyph is 6x10 px at scale 2.
    constexpr int32 DefaultLabelScale = 2;

    // Row bitmask per glyph row: bit 2 is the leftmost column, row 0 is the top.
    void GetGlyphRows(TCHAR Ch, uint8 OutRows[GlyphCellHeight]);

    // What (AnchorX, AnchorY) means for DrawLabel. The two former copies disagreed on this and
    // nothing else, so it is the one thing the shared entry point has to be told.
    enum class ELabelAnchor : uint8
    {
        // Anchor is the top-left of the TEXT; the plate extends Padding px further out on every
        // side. (AnnotatedCaptureHandler's convention.)
        TextTopLeft,
        // Anchor is the top-left of the PLATE; the text starts Padding px inside it.
        // (DriveSetOfMarkRenderer's convention - its FDriveMark::LabelAnchor is a badge corner.)
        PlateTopLeft
    };

    struct FLabelStyle
    {
        int32 Scale = DefaultLabelScale;
        FPaint Ink = FPaint(FColor(255, 255, 255, 255));
        // Dark backing plate, drawn first so ink stays legible over arbitrary scene content.
        FPaint Plate = FPaint(FColor(16, 16, 16, 255));
        bool bDrawPlate = true;
        // Plate margin around the text, in PIXELS (not font-pixels). Both former copies used 2.
        int32 PlatePaddingPx = 2;
        ELabelAnchor Anchor = ELabelAnchor::TextTopLeft;
    };

    // Painted width / height of the glyph run itself (no plate padding), in pixels.
    // Glyphs are separated by one font-pixel (Scale px) of gap.
    int32 MeasureLabelTextWidth(const FString& Text, int32 Scale);
    int32 MeasureLabelTextHeight(int32 Scale);

    // The plate rect DrawLabel would paint, so a caller can keep labels off the field (edge
    // placement) or de-overlap them without re-deriving the metrics. FIntRect's own convention:
    // Min inclusive, Max EXCLUSIVE, so Width()/Height() are the painted extent. An empty Text
    // yields a zero-area rect and DrawLabel draws nothing.
    FIntRect MeasureLabelPlate(const FString& Text, int32 AnchorX, int32 AnchorY, const FLabelStyle& Style);

    // Plate (optional) then glyphs. Empty Text draws nothing at all.
    void DrawLabel(const FSurface& Surface, const FString& Text, int32 AnchorX, int32 AnchorY,
        const FLabelStyle& Style);
}
