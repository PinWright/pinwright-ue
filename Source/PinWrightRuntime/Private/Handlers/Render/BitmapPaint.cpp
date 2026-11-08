// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/BitmapPaint.h"

#include "Math/UnrealMathUtility.h"

namespace PinWrightBitmapPaint
{
    namespace
    {
        // Self-contained 3x5 bitmap font for digits 0-9. Each row is a 3-bit mask: bit 2 is the
        // leftmost column, bit 0 the rightmost; row 0 is the top of the glyph. These are the
        // exact tables the two former private copies carried, so glyph pixels did not move.
        const uint8 GDigitFont[10][GlyphCellHeight] =
        {
            { 0b111, 0b101, 0b101, 0b101, 0b111 }, // 0
            { 0b010, 0b110, 0b010, 0b010, 0b111 }, // 1
            { 0b111, 0b001, 0b111, 0b100, 0b111 }, // 2
            { 0b111, 0b001, 0b111, 0b001, 0b111 }, // 3
            { 0b101, 0b101, 0b111, 0b001, 0b001 }, // 4
            { 0b111, 0b100, 0b111, 0b001, 0b111 }, // 5
            { 0b111, 0b100, 0b111, 0b101, 0b111 }, // 6
            { 0b111, 0b001, 0b010, 0b010, 0b010 }, // 7
            { 0b111, 0b101, 0b111, 0b101, 0b111 }, // 8
            { 0b111, 0b101, 0b111, 0b001, 0b111 }, // 9
        };

        // 3x5 uppercase A-Z. Legible enough at scale 2 for axis letters, actor names and unit
        // suffixes; the digit-only Drive font could not spell these.
        const uint8 GLetterFont[26][GlyphCellHeight] =
        {
            { 0b111, 0b101, 0b111, 0b101, 0b101 }, // A
            { 0b110, 0b101, 0b110, 0b101, 0b110 }, // B
            { 0b111, 0b100, 0b100, 0b100, 0b111 }, // C
            { 0b110, 0b101, 0b101, 0b101, 0b110 }, // D
            { 0b111, 0b100, 0b111, 0b100, 0b111 }, // E
            { 0b111, 0b100, 0b111, 0b100, 0b100 }, // F
            { 0b111, 0b100, 0b101, 0b101, 0b111 }, // G
            { 0b101, 0b101, 0b111, 0b101, 0b101 }, // H
            { 0b111, 0b010, 0b010, 0b010, 0b111 }, // I
            { 0b001, 0b001, 0b001, 0b101, 0b111 }, // J
            { 0b101, 0b110, 0b100, 0b110, 0b101 }, // K
            { 0b100, 0b100, 0b100, 0b100, 0b111 }, // L
            { 0b101, 0b111, 0b111, 0b101, 0b101 }, // M
            { 0b110, 0b101, 0b101, 0b101, 0b011 }, // N
            { 0b111, 0b101, 0b101, 0b101, 0b111 }, // O
            { 0b111, 0b101, 0b111, 0b100, 0b100 }, // P
            { 0b111, 0b101, 0b101, 0b111, 0b001 }, // Q
            { 0b111, 0b101, 0b111, 0b110, 0b101 }, // R
            { 0b111, 0b100, 0b111, 0b001, 0b111 }, // S
            { 0b111, 0b010, 0b010, 0b010, 0b010 }, // T
            { 0b101, 0b101, 0b101, 0b101, 0b111 }, // U
            { 0b101, 0b101, 0b101, 0b101, 0b010 }, // V
            { 0b101, 0b101, 0b111, 0b111, 0b101 }, // W
            { 0b101, 0b101, 0b010, 0b101, 0b101 }, // X
            { 0b101, 0b101, 0b010, 0b010, 0b010 }, // Y
            { 0b111, 0b001, 0b010, 0b100, 0b111 }, // Z
        };

        FORCEINLINE uint8 BlendChannel(uint8 Src, uint8 Dst, float Alpha)
        {
            const float Blended = static_cast<float>(Src) * Alpha + static_cast<float>(Dst) * (1.0f - Alpha);
            return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Blended), 0, 255));
        }

        // Source-over composite of Paint onto Dst. The opaque case is an exact byte replace so
        // the unification moved no pixels at either former call site.
        FORCEINLINE void CompositeInto(FColor& Dst, const FPaint& Paint)
        {
            if (Paint.IsOpaque())
            {
                Dst = Paint.Color;
                return;
            }
            const float Alpha = Paint.EffectiveAlpha();
            if (Alpha <= 0.0f)
            {
                return;
            }
            Dst.R = BlendChannel(Paint.Color.R, Dst.R, Alpha);
            Dst.G = BlendChannel(Paint.Color.G, Dst.G, Alpha);
            Dst.B = BlendChannel(Paint.Color.B, Dst.B, Alpha);
            Dst.A = BlendChannel(255, Dst.A, Alpha);
        }
    }

    void GetGlyphRows(TCHAR Ch, uint8 OutRows[GlyphCellHeight])
    {
        if (Ch >= TEXT('0') && Ch <= TEXT('9'))
        {
            FMemory::Memcpy(OutRows, GDigitFont[Ch - TEXT('0')], GlyphCellHeight);
            return;
        }
        const TCHAR Upper = FChar::ToUpper(Ch);
        if (Upper >= TEXT('A') && Upper <= TEXT('Z'))
        {
            FMemory::Memcpy(OutRows, GLetterFont[Upper - TEXT('A')], GlyphCellHeight);
            return;
        }
        switch (Ch)
        {
            case TEXT('.'): { const uint8 R[GlyphCellHeight] = { 0, 0, 0, 0, 0b010 }; FMemory::Memcpy(OutRows, R, GlyphCellHeight); return; }
            case TEXT('-'): { const uint8 R[GlyphCellHeight] = { 0, 0, 0b111, 0, 0 };  FMemory::Memcpy(OutRows, R, GlyphCellHeight); return; }
            case TEXT(':'): { const uint8 R[GlyphCellHeight] = { 0, 0b010, 0, 0b010, 0 }; FMemory::Memcpy(OutRows, R, GlyphCellHeight); return; }
            case TEXT('/'): { const uint8 R[GlyphCellHeight] = { 0b001, 0b001, 0b010, 0b100, 0b100 }; FMemory::Memcpy(OutRows, R, GlyphCellHeight); return; }
            default: break;
        }
        FMemory::Memset(OutRows, 0, GlyphCellHeight); // space / unknown
    }

    void SetPixel(const FSurface& Surface, int32 X, int32 Y, const FPaint& Paint)
    {
        if (!Surface.IsValid() || X < 0 || Y < 0 || X >= Surface.Width || Y >= Surface.Height)
        {
            return;
        }
        CompositeInto(Surface.Pixels[Y * Surface.Width + X], Paint);
    }

    void FillRect(const FSurface& Surface, int32 X0, int32 Y0, int32 X1, int32 Y1, const FPaint& Paint)
    {
        if (!Surface.IsValid())
        {
            return;
        }
        // Clamp once instead of per-pixel, so a rect anchored far off-frame does not iterate
        // millions of rejected coordinates.
        const int32 ClampedX0 = FMath::Max(X0, 0);
        const int32 ClampedY0 = FMath::Max(Y0, 0);
        const int32 ClampedX1 = FMath::Min(X1, Surface.Width - 1);
        const int32 ClampedY1 = FMath::Min(Y1, Surface.Height - 1);
        for (int32 Y = ClampedY0; Y <= ClampedY1; ++Y)
        {
            FColor* Row = Surface.Pixels.GetData() + Y * Surface.Width;
            for (int32 X = ClampedX0; X <= ClampedX1; ++X)
            {
                CompositeInto(Row[X], Paint);
            }
        }
    }

    void StrokeBox(const FSurface& Surface, int32 MinX, int32 MinY, int32 MaxX, int32 MaxY,
        int32 Thickness, const FPaint& Paint)
    {
        if (!Surface.IsValid())
        {
            return;
        }
        const int32 T = FMath::Max(Thickness, 1);
        // Iterate only the visible span; the border test stays in ABSOLUTE coordinates, so a box
        // straddling the frame edge strokes exactly the pixels it did before this clamp existed.
        const int32 ScanY0 = FMath::Max(MinY, 0);
        const int32 ScanY1 = FMath::Min(MaxY, Surface.Height - 1);
        const int32 ScanX0 = FMath::Max(MinX, 0);
        const int32 ScanX1 = FMath::Min(MaxX, Surface.Width - 1);
        for (int32 Y = ScanY0; Y <= ScanY1; ++Y)
        {
            for (int32 X = ScanX0; X <= ScanX1; ++X)
            {
                const bool bOnBorder =
                    X < MinX + T || X > MaxX - T ||
                    Y < MinY + T || Y > MaxY - T;
                if (bOnBorder)
                {
                    SetPixel(Surface, X, Y, Paint);
                }
            }
        }
    }

    void DrawLine(const FSurface& Surface, int32 X0, int32 Y0, int32 X1, int32 Y1,
        int32 Thickness, const FPaint& Paint)
    {
        if (!Surface.IsValid())
        {
            return;
        }
        const int32 T = FMath::Max(Thickness, 1);
        const int32 Half = T / 2;

        const int32 DX = FMath::Abs(X1 - X0);
        const int32 DY = -FMath::Abs(Y1 - Y0);
        const int32 SX = X0 < X1 ? 1 : -1;
        const int32 SY = Y0 < Y1 ? 1 : -1;
        int32 Err = DX + DY;
        int32 X = X0;
        int32 Y = Y0;

        // Guard against a run-away when both endpoints are far off-frame.
        const int32 MaxSteps = (Surface.Width + Surface.Height) * 4 + 8;

        bool bHasPrev = false;
        int32 PrevX0 = 0, PrevY0 = 0, PrevX1 = 0, PrevY1 = 0;

        for (int32 Step = 0; Step < MaxSteps; ++Step)
        {
            const int32 BX0 = X - Half;
            const int32 BY0 = Y - Half;
            const int32 BX1 = BX0 + T - 1;
            const int32 BY1 = BY0 + T - 1;

            if (!bHasPrev)
            {
                FillRect(Surface, BX0, BY0, BX1, BY1, Paint);
            }
            else
            {
                // Paint only the strip this step newly covers. The pen advances by at most one
                // pixel per axis and moves monotonically, so the previously painted block is the
                // ONLY earlier block that can overlap this one - which makes "block minus
                // previous block" exactly "block minus everything painted so far". The union of
                // painted pixels is therefore unchanged (opaque output is byte-identical to the
                // former full-block-per-step code) while a translucent line stops double-
                // compositing itself into a darker smear.
                const int32 StepDX = BX0 - PrevX0;
                const int32 StepDY = BY0 - PrevY0;
                if (StepDX > 0)
                {
                    FillRect(Surface, PrevX1 + 1, BY0, BX1, BY1, Paint);
                }
                else if (StepDX < 0)
                {
                    FillRect(Surface, BX0, BY0, PrevX0 - 1, BY1, Paint);
                }
                if (StepDY != 0)
                {
                    // Restrict to the columns shared with the previous block; the rest were just
                    // painted by the vertical strip above.
                    const int32 SharedX0 = FMath::Max(BX0, PrevX0);
                    const int32 SharedX1 = FMath::Min(BX1, PrevX1);
                    if (SharedX0 <= SharedX1)
                    {
                        if (StepDY > 0)
                        {
                            FillRect(Surface, SharedX0, PrevY1 + 1, SharedX1, BY1, Paint);
                        }
                        else
                        {
                            FillRect(Surface, SharedX0, BY0, SharedX1, PrevY0 - 1, Paint);
                        }
                    }
                }
            }

            PrevX0 = BX0; PrevY0 = BY0; PrevX1 = BX1; PrevY1 = BY1;
            bHasPrev = true;

            if (X == X1 && Y == Y1)
            {
                break;
            }
            const int32 Err2 = 2 * Err;
            if (Err2 >= DY) { Err += DY; X += SX; }
            if (Err2 <= DX) { Err += DX; Y += SY; }
        }
    }

    int32 MeasureLabelTextWidth(const FString& Text, int32 Scale)
    {
        const int32 Count = Text.Len();
        if (Count <= 0)
        {
            return 0;
        }
        const int32 SafeScale = FMath::Max(Scale, 1);
        const int32 GlyphW = GlyphCellWidth * SafeScale;
        const int32 GapPx = SafeScale; // one font-pixel between glyphs
        return Count * GlyphW + (Count - 1) * GapPx;
    }

    int32 MeasureLabelTextHeight(int32 Scale)
    {
        return GlyphCellHeight * FMath::Max(Scale, 1);
    }

    FIntRect MeasureLabelPlate(const FString& Text, int32 AnchorX, int32 AnchorY, const FLabelStyle& Style)
    {
        if (Text.Len() <= 0)
        {
            return FIntRect(AnchorX, AnchorY, AnchorX, AnchorY);
        }
        const int32 Scale = FMath::Max(Style.Scale, 1);
        const int32 Pad = FMath::Max(Style.PlatePaddingPx, 0);
        const int32 TextW = MeasureLabelTextWidth(Text, Scale);
        const int32 TextH = MeasureLabelTextHeight(Scale);

        int32 TextX = AnchorX;
        int32 TextY = AnchorY;
        if (Style.Anchor == ELabelAnchor::PlateTopLeft)
        {
            TextX += Pad;
            TextY += Pad;
        }
        // FIntRect Max is exclusive; the painted rect is inclusive of (Max - 1).
        return FIntRect(TextX - Pad, TextY - Pad, TextX + TextW + Pad, TextY + TextH + Pad);
    }

    void DrawLabel(const FSurface& Surface, const FString& Text, int32 AnchorX, int32 AnchorY,
        const FLabelStyle& Style)
    {
        const int32 Count = Text.Len();
        if (!Surface.IsValid() || Count <= 0)
        {
            return;
        }
        const int32 Scale = FMath::Max(Style.Scale, 1);
        const int32 Pad = FMath::Max(Style.PlatePaddingPx, 0);
        const int32 GlyphW = GlyphCellWidth * Scale;
        const int32 GapPx = Scale;

        int32 TextX = AnchorX;
        int32 TextY = AnchorY;
        if (Style.Anchor == ELabelAnchor::PlateTopLeft)
        {
            TextX += Pad;
            TextY += Pad;
        }

        if (Style.bDrawPlate)
        {
            const FIntRect Plate = MeasureLabelPlate(Text, AnchorX, AnchorY, Style);
            FillRect(Surface, Plate.Min.X, Plate.Min.Y, Plate.Max.X - 1, Plate.Max.Y - 1, Style.Plate);
        }

        int32 PenX = TextX;
        for (int32 I = 0; I < Count; ++I)
        {
            uint8 Rows[GlyphCellHeight];
            GetGlyphRows(Text[I], Rows);
            for (int32 Row = 0; Row < GlyphCellHeight; ++Row)
            {
                const uint8 Bits = Rows[Row];
                for (int32 Col = 0; Col < GlyphCellWidth; ++Col)
                {
                    if ((Bits >> (GlyphCellWidth - 1 - Col)) & 1)
                    {
                        const int32 BX = PenX + Col * Scale;
                        const int32 BY = TextY + Row * Scale;
                        FillRect(Surface, BX, BY, BX + Scale - 1, BY + Scale - 1, Style.Ink);
                    }
                }
            }
            PenX += GlyphW + GapPx;
        }
    }
}
