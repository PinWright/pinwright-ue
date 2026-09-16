// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for PinWrightBitmapPaint - the shared CPU rasteriser extracted from the two private
// copies in DriveSetOfMarkRenderer.cpp and AnnotatedCaptureHandler.cpp. Pure, so it runs headless
// on a synthetic FColor buffer.
//
// The load-bearing cases:
//   * an opaque paint is an EXACT byte replace, which is what makes the extraction pixel-neutral
//     for both former call sites;
//   * a low-alpha overlay leaves the detail underneath still distinguishable - a full-opacity
//     grid buried exactly the detail it was drawn to measure, which is why alpha exists here;
//   * a thick translucent line does not double-composite itself into a darker smear;
//   * the two label anchor conventions (text corner vs plate corner) describe the SAME painted
//     rectangle, which is the only thing that differed between the two former copies.

#include <initializer_list>

#include "Misc/AutomationTest.h"

#include "Handlers/Render/BitmapPaint.h"

namespace
{
    using namespace PinWrightBitmapPaint;

    constexpr int32 PwPaintW = 96;
    constexpr int32 PwPaintH = 64;

    TArray<FColor> PwPaintMakeBuffer(const FColor& Fill, int32 W = PwPaintW, int32 H = PwPaintH)
    {
        TArray<FColor> Px;
        Px.Init(Fill, W * H);
        return Px;
    }

    FColor PwPaintAt(const TArray<FColor>& Px, int32 X, int32 Y, int32 W = PwPaintW)
    {
        return Px[Y * W + X];
    }

    int32 PwPaintCountNot(const TArray<FColor>& Px, const FColor& Background)
    {
        int32 Count = 0;
        for (const FColor& C : Px)
        {
            if (!(C == Background))
            {
                ++Count;
            }
        }
        return Count;
    }
}

// ============================================================================
// Opaque paint is an exact replace; the surface guard rejects a short buffer
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwBitmapPaintOpaqueReplaceTest,
    "PinWright.render.bitmap_paint.OpaquePaintReplacesExactly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwBitmapPaintOpaqueReplaceTest::RunTest(const FString& Parameters)
{
    const FColor Background(10, 20, 30, 255);
    const FColor Ink(200, 100, 50, 255);
    TArray<FColor> Px = PwPaintMakeBuffer(Background);
    const FSurface Surface(Px, PwPaintW, PwPaintH);

    SetPixel(Surface, 5, 6, Ink);
    TestTrue(TEXT("opaque pixel is the exact colour"), PwPaintAt(Px, 5, 6) == Ink);
    TestTrue(TEXT("neighbour untouched"), PwPaintAt(Px, 6, 6) == Background);

    // Inclusive rect, clamped at the frame edge, and nothing outside it is written.
    FillRect(Surface, -5, -5, 3, 3, Ink);
    TestTrue(TEXT("clamped rect paints (0,0)"), PwPaintAt(Px, 0, 0) == Ink);
    TestTrue(TEXT("clamped rect paints its inclusive far corner"), PwPaintAt(Px, 3, 3) == Ink);
    TestTrue(TEXT("clamped rect stops at its far corner"), PwPaintAt(Px, 4, 3) == Background);

    // A fully off-frame draw writes nothing at all.
    TArray<FColor> Pristine = PwPaintMakeBuffer(Background);
    const FSurface PristineSurface(Pristine, PwPaintW, PwPaintH);
    FillRect(PristineSurface, -100, -100, -50, -50, Ink);
    DrawLine(PristineSurface, -100, -100, -50, -80, 3, Ink);
    TestEqual(TEXT("fully off-frame draws write nothing"), PwPaintCountNot(Pristine, Background), 0);

    // A surface whose buffer is short for its declared size is INVALID: every primitive no-ops,
    // so a truncated readback cannot be half-painted and then shipped as an annotated frame.
    TArray<FColor> Short;
    Short.Init(Background, 10);
    const FSurface Bad(Short, PwPaintW, PwPaintH);
    TestFalse(TEXT("short buffer is an invalid surface"), Bad.IsValid());
    SetPixel(Bad, 1, 1, Ink);
    FillRect(Bad, 0, 0, 5, 5, Ink);
    StrokeBox(Bad, 0, 0, 5, 5, 2, Ink);
    DrawLine(Bad, 0, 0, 5, 5, 2, Ink);
    DrawLabel(Bad, TEXT("A"), 0, 0, FLabelStyle());
    TestEqual(TEXT("an invalid surface is never written"), PwPaintCountNot(Short, Background), 0);

    return true;
}

// ============================================================================
// Alpha compositing: the reason this header exists at all
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwBitmapPaintAlphaTest,
    "PinWright.render.bitmap_paint.AlphaCompositePreservesDetail",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwBitmapPaintAlphaTest::RunTest(const FString& Parameters)
{
    // Half coverage of white over black is the midpoint, alpha left opaque.
    {
        TArray<FColor> Px = PwPaintMakeBuffer(FColor(0, 0, 0, 255));
        const FSurface Surface(Px, PwPaintW, PwPaintH);
        SetPixel(Surface, 1, 1, FPaint(FColor(255, 255, 255, 255), 0.5f));
        const FColor Blended = PwPaintAt(Px, 1, 1);
        TestEqual(TEXT("50% white over black is 128"), static_cast<int32>(Blended.R), 128);
        TestEqual(TEXT("blend keeps the destination opaque"), static_cast<int32>(Blended.A), 255);
    }

    // Opacity 0 is a no-op, and a colour whose own alpha is < 255 composites even at opacity 1.
    {
        const FColor Background(40, 40, 40, 255);
        TArray<FColor> Px = PwPaintMakeBuffer(Background);
        const FSurface Surface(Px, PwPaintW, PwPaintH);
        SetPixel(Surface, 2, 2, FPaint(FColor(255, 255, 255, 255), 0.0f));
        TestTrue(TEXT("zero opacity writes nothing"), PwPaintAt(Px, 2, 2) == Background);
        SetPixel(Surface, 3, 3, FPaint(FColor(255, 255, 255, 128), 1.0f));
        TestTrue(TEXT("a translucent colour composites at full opacity"),
            !(PwPaintAt(Px, 3, 3) == FColor(255, 255, 255, 128)) && PwPaintAt(Px, 3, 3).R > Background.R);
    }

    // THE regression: a full-opacity grid line buried the detail it was drawn to measure. Two
    // distinguishable background values must stay distinguishable under a low-alpha overlay, and
    // must NOT under an opaque one.
    {
        const FColor Dark(40, 40, 40, 255);
        const FColor Light(200, 200, 200, 255);
        TArray<FColor> Px = PwPaintMakeBuffer(Dark);
        const FSurface Surface(Px, PwPaintW, PwPaintH);
        // Row 10 carries the "detail": alternating dark/light pixels.
        for (int32 X = 0; X < PwPaintW; X += 2)
        {
            SetPixel(Surface, X, 10, Light);
        }

        DrawLine(Surface, 0, 10, PwPaintW - 1, 10, 1, FPaint(FColor(255, 255, 255, 255), 0.15f));
        const FColor OverDark = PwPaintAt(Px, 1, 10);
        const FColor OverLight = PwPaintAt(Px, 0, 10);
        TestTrue(TEXT("a 15% grid line leaves the detail underneath distinguishable"),
            OverLight.R - OverDark.R > 100);

        DrawLine(Surface, 0, 10, PwPaintW - 1, 10, 1, FPaint(FColor(255, 255, 255, 255), 1.0f));
        TestTrue(TEXT("an opaque grid line buries it"),
            PwPaintAt(Px, 0, 10) == PwPaintAt(Px, 1, 10));
    }

    // A thick translucent line must composite each pixel ONCE; overlapping pen blocks would show
    // as darker spots along the run.
    {
        const FColor Background(0, 0, 0, 255);
        TArray<FColor> Px = PwPaintMakeBuffer(Background);
        const FSurface Surface(Px, PwPaintW, PwPaintH);
        const FPaint Translucent(FColor(255, 255, 255, 255), 0.5f);
        DrawLine(Surface, 5, 5, 80, 50, 3, Translucent);

        int32 Painted = 0;
        int32 DoubleComposited = 0;
        for (const FColor& C : Px)
        {
            if (C == Background)
            {
                continue;
            }
            ++Painted;
            if (C.R != 128)
            {
                ++DoubleComposited;
            }
        }
        TestTrue(TEXT("the thick line painted something"), Painted > 100);
        TestEqual(TEXT("no pixel is composited twice in one line"), DoubleComposited, 0);
    }

    // The strip optimisation changes WHEN a pixel is written, never WHICH pixels are covered.
    // Checked against an independent reference that paints the FULL Thickness x Thickness pen
    // block at every Bresenham step - i.e. exactly what both former private copies did.
    {
        const FColor Background(0, 0, 0, 255);
        const FColor Ink(255, 255, 255, 255);

        auto ReferenceLine = [](TArray<FColor>& Px, int32 X0, int32 Y0, int32 X1, int32 Y1,
            int32 Thickness, const FColor& C)
        {
            const int32 Half = FMath::Max(Thickness, 1) / 2;
            const int32 DX = FMath::Abs(X1 - X0);
            const int32 DY = -FMath::Abs(Y1 - Y0);
            const int32 SX = X0 < X1 ? 1 : -1;
            const int32 SY = Y0 < Y1 ? 1 : -1;
            int32 Err = DX + DY;
            int32 X = X0;
            int32 Y = Y0;
            const int32 MaxSteps = (PwPaintW + PwPaintH) * 4 + 8;
            for (int32 Step = 0; Step < MaxSteps; ++Step)
            {
                for (int32 BY = Y - Half; BY <= Y - Half + Thickness - 1; ++BY)
                {
                    for (int32 BX = X - Half; BX <= X - Half + Thickness - 1; ++BX)
                    {
                        if (BX >= 0 && BY >= 0 && BX < PwPaintW && BY < PwPaintH)
                        {
                            Px[BY * PwPaintW + BX] = C;
                        }
                    }
                }
                if (X == X1 && Y == Y1) { break; }
                const int32 Err2 = 2 * Err;
                if (Err2 >= DY) { Err += DY; X += SX; }
                if (Err2 <= DX) { Err += DX; Y += SY; }
            }
        };

        const int32 Cases[][5] =
        {
            { 5, 5, 80, 50, 3 },   // shallow, down-right
            { 80, 50, 5, 5, 3 },   // the same run reversed
            { 10, 55, 70, 4, 4 },  // up-right, even thickness
            { 40, 2, 40, 60, 2 },  // vertical
            { 2, 30, 90, 30, 1 },  // horizontal, single pixel pen
            { 3, 3, 60, 60, 5 },   // exact diagonal, thick pen
        };
        for (const int32* Case : Cases)
        {
            TArray<FColor> Actual = PwPaintMakeBuffer(Background);
            TArray<FColor> Expected = PwPaintMakeBuffer(Background);
            DrawLine(FSurface(Actual, PwPaintW, PwPaintH), Case[0], Case[1], Case[2], Case[3], Case[4],
                FPaint(Ink, 1.0f));
            ReferenceLine(Expected, Case[0], Case[1], Case[2], Case[3], Case[4], Ink);

            int32 Mismatched = 0;
            for (int32 I = 0; I < Actual.Num(); ++I)
            {
                if (!(Actual[I] == Expected[I]))
                {
                    ++Mismatched;
                }
            }
            TestEqual(*FString::Printf(TEXT("line (%d,%d)->(%d,%d) t=%d matches the full-block reference"),
                Case[0], Case[1], Case[2], Case[3], Case[4]), Mismatched, 0);
            TestTrue(TEXT("the reference line painted something"),
                PwPaintCountNot(Expected, Background) > 0);
        }
    }

    return true;
}

// ============================================================================
// StrokeBox: border in, interior untouched (the Drive overlay's contract)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwBitmapPaintStrokeBoxTest,
    "PinWright.render.bitmap_paint.StrokeBoxDrawsBorderOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwBitmapPaintStrokeBoxTest::RunTest(const FString& Parameters)
{
    const FColor Background(10, 10, 10, 255);
    const FColor Ink(220, 32, 32, 255);
    TArray<FColor> Px = PwPaintMakeBuffer(Background);
    const FSurface Surface(Px, PwPaintW, PwPaintH);

    StrokeBox(Surface, 10, 10, 60, 40, 2, Ink);

    TestTrue(TEXT("top edge stroked"), PwPaintAt(Px, 35, 10) == Ink);
    TestTrue(TEXT("second row of the 2px border stroked"), PwPaintAt(Px, 35, 11) == Ink);
    TestTrue(TEXT("bottom edge stroked"), PwPaintAt(Px, 35, 40) == Ink);
    TestTrue(TEXT("left edge stroked"), PwPaintAt(Px, 10, 25) == Ink);
    TestTrue(TEXT("right edge stroked"), PwPaintAt(Px, 60, 25) == Ink);
    TestTrue(TEXT("corner stroked"), PwPaintAt(Px, 10, 10) == Ink);

    // The border eats INTO the rect; it does not grow it.
    TestTrue(TEXT("interior untouched"), PwPaintAt(Px, 35, 25) == Background);
    TestTrue(TEXT("just inside the 2px border is untouched"), PwPaintAt(Px, 35, 12) == Background);
    TestTrue(TEXT("just outside the box is untouched"), PwPaintAt(Px, 35, 9) == Background);

    // A box straddling the frame edge strokes only what is visible, without wrapping.
    TArray<FColor> Edge = PwPaintMakeBuffer(Background);
    const FSurface EdgeSurface(Edge, PwPaintW, PwPaintH);
    StrokeBox(EdgeSurface, -10, -10, 5, 5, 2, Ink);
    TestTrue(TEXT("visible part of an off-frame box is stroked"), PwPaintAt(Edge, 5, 3) == Ink);
    TestTrue(TEXT("the clipped-away side does not wrap"), PwPaintAt(Edge, PwPaintW - 1, 3) == Background);

    return true;
}

// ============================================================================
// Labels: the two anchor conventions describe the same rectangle
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwBitmapPaintLabelAnchorTest,
    "PinWright.render.bitmap_paint.LabelAnchorConventionsAgree",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwBitmapPaintLabelAnchorTest::RunTest(const FString& Parameters)
{
    const FColor Background(10, 10, 10, 255);
    const FString Text = TEXT("AB12");

    FLabelStyle TextAnchored;
    TextAnchored.Scale = 2;
    TextAnchored.PlatePaddingPx = 2;
    TextAnchored.Anchor = ELabelAnchor::TextTopLeft;

    FLabelStyle PlateAnchored = TextAnchored;
    PlateAnchored.Anchor = ELabelAnchor::PlateTopLeft;

    // The plate corner is exactly Padding up-left of the text corner, so the two calls must paint
    // byte-identical buffers. That equivalence is what let the Drive badge (plate-anchored) and
    // the annotated overlay (text-anchored) collapse onto one implementation.
    TArray<FColor> ByText = PwPaintMakeBuffer(Background);
    TArray<FColor> ByPlate = PwPaintMakeBuffer(Background);
    DrawLabel(FSurface(ByText, PwPaintW, PwPaintH), Text, 20, 20, TextAnchored);
    DrawLabel(FSurface(ByPlate, PwPaintW, PwPaintH), Text, 18, 18, PlateAnchored);

    int32 Differences = 0;
    for (int32 I = 0; I < ByText.Num(); ++I)
    {
        if (!(ByText[I] == ByPlate[I]))
        {
            ++Differences;
        }
    }
    TestEqual(TEXT("text-anchored and plate-anchored labels agree byte for byte"), Differences, 0);

    // Measured metrics match what was painted: 4 glyphs at scale 2 are 4*6 + 3*2 = 30 px of text,
    // plus 2 px of plate padding on each side.
    TestEqual(TEXT("measured text width"), MeasureLabelTextWidth(Text, 2), 30);
    TestEqual(TEXT("measured text height"), MeasureLabelTextHeight(2), 10);
    const FIntRect Plate = MeasureLabelPlate(Text, 20, 20, TextAnchored);
    TestEqual(TEXT("plate rect left"), Plate.Min.X, 18);
    TestEqual(TEXT("plate rect top"), Plate.Min.Y, 18);
    TestEqual(TEXT("plate rect width"), Plate.Width(), 34);
    TestEqual(TEXT("plate rect height"), Plate.Height(), 14);

    // Nothing is painted outside the measured plate.
    int32 OutsidePlate = 0;
    for (int32 Y = 0; Y < PwPaintH; ++Y)
    {
        for (int32 X = 0; X < PwPaintW; ++X)
        {
            const bool bInside = X >= Plate.Min.X && X < Plate.Max.X && Y >= Plate.Min.Y && Y < Plate.Max.Y;
            if (!bInside && !(PwPaintAt(ByText, X, Y) == Background))
            {
                ++OutsidePlate;
            }
        }
    }
    TestEqual(TEXT("nothing painted outside the measured plate"), OutsidePlate, 0);

    // Both the plate and the glyph ink are present: a plate with no ink is a solid rectangle
    // pretending to be a label.
    int32 PlatePixels = 0;
    int32 InkPixels = 0;
    for (int32 Y = Plate.Min.Y; Y < Plate.Max.Y; ++Y)
    {
        for (int32 X = Plate.Min.X; X < Plate.Max.X; ++X)
        {
            const FColor C = PwPaintAt(ByText, X, Y);
            if (C == TextAnchored.Plate.Color) { ++PlatePixels; }
            else if (C == TextAnchored.Ink.Color) { ++InkPixels; }
        }
    }
    TestTrue(TEXT("plate painted"), PlatePixels > 0);
    TestTrue(TEXT("glyph ink painted"), InkPixels > 0);

    // Empty text is a no-op, and the measured plate is empty rather than a stray 4-px square.
    TArray<FColor> Empty = PwPaintMakeBuffer(Background);
    DrawLabel(FSurface(Empty, PwPaintW, PwPaintH), FString(), 20, 20, TextAnchored);
    TestEqual(TEXT("empty text paints nothing"), PwPaintCountNot(Empty, Background), 0);
    TestEqual(TEXT("empty text measures zero-area"), MeasureLabelPlate(FString(), 20, 20, TextAnchored).Area(), 0);

    // bDrawPlate false leaves the field visible under the glyphs.
    FLabelStyle NoPlate = TextAnchored;
    NoPlate.bDrawPlate = false;
    TArray<FColor> Bare = PwPaintMakeBuffer(Background);
    DrawLabel(FSurface(Bare, PwPaintW, PwPaintH), Text, 20, 20, NoPlate);
    TestTrue(TEXT("no-plate label leaves the background between glyph rows"),
        PwPaintAt(Bare, 18, 18) == Background);
    TestTrue(TEXT("no-plate label still paints ink"), PwPaintCountNot(Bare, Background) > 0);

    return true;
}

// ============================================================================
// Font coverage: digits, letters, punctuation, and the blank-but-advancing unknown
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwBitmapPaintFontTest,
    "PinWright.render.bitmap_paint.FontCoversDigitsAndLetters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwBitmapPaintFontTest::RunTest(const FString& Parameters)
{
    uint8 Rows[GlyphCellHeight];

    auto RowsAreBlank = [&Rows]()
    {
        for (int32 I = 0; I < GlyphCellHeight; ++I)
        {
            if (Rows[I] != 0)
            {
                return false;
            }
        }
        return true;
    };

    for (TCHAR Ch = TEXT('0'); Ch <= TEXT('9'); ++Ch)
    {
        GetGlyphRows(Ch, Rows);
        TestFalse(*FString::Printf(TEXT("digit '%c' has ink"), Ch), RowsAreBlank());
    }
    for (TCHAR Ch = TEXT('A'); Ch <= TEXT('Z'); ++Ch)
    {
        GetGlyphRows(Ch, Rows);
        TestFalse(*FString::Printf(TEXT("letter '%c' has ink"), Ch), RowsAreBlank());
    }

    // Lowercase is case-folded onto the uppercase cell, not dropped.
    uint8 Upper[GlyphCellHeight];
    GetGlyphRows(TEXT('Q'), Upper);
    GetGlyphRows(TEXT('q'), Rows);
    TestEqual(TEXT("lowercase folds to uppercase"),
        FMemory::Memcmp(Upper, Rows, GlyphCellHeight), 0);

    for (const TCHAR Punct : { TEXT('.'), TEXT('-'), TEXT(':'), TEXT('/') })
    {
        GetGlyphRows(Punct, Rows);
        TestFalse(*FString::Printf(TEXT("punctuation '%c' has ink"), Punct), RowsAreBlank());
    }

    // An unknown character renders BLANK but still advances the pen, so a stray character cannot
    // corrupt the rest of a label.
    GetGlyphRows(TEXT('~'), Rows);
    TestTrue(TEXT("unknown character is blank"), RowsAreBlank());
    TestEqual(TEXT("an unknown character still occupies a cell"),
        MeasureLabelTextWidth(TEXT("A~B"), 2), MeasureLabelTextWidth(TEXT("ABC"), 2));

    return true;
}
