// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AudioGen/PwAudioPlot.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwStft.h"

// Audio::FPseudoConstantQ / NewPseudoConstantQKernelTransform / FContiguousSparse2DKernelTransform
// (and the FPseudoConstantQKernelSettings fields and EqualAmplitude normalization used below) are
// present unchanged in SignalProcessing on UE 5.3 through 5.8 - verified against the six engine
// trees on the dev host - so the constant-Q view owes no row in docs/engine-version-support.md.
#include "DSP/ConstantQ.h"

#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// Named (not anonymous) namespace: this module builds with bUseUnity = true.
namespace PwAudioPlotInternal
{
    // ---------------------------------------------------------------------------------------
    // Palette. Dark ground on purpose: viridis is designed against a dark background, and a dark
    // page keeps the plotted signal the brightest thing in the image.
    // ---------------------------------------------------------------------------------------
    const FColor ColorPage       (16, 16, 20, 255);
    const FColor ColorPlotGround (26, 26, 32, 255);
    const FColor ColorAxis       (96, 96, 110, 255);
    const FColor ColorGrid       (52, 52, 62, 255);
    const FColor ColorText       (226, 226, 236, 255);
    const FColor ColorZeroLine   (128, 128, 144, 255);
    const FColor ColorChannelL   (86, 172, 255, 255);
    const FColor ColorChannelR   (255, 148, 86, 255);
    const FColor ColorRms        (255, 232, 116, 255);

    // Overlay marks. Picked so the viridis ramp cannot produce them - see FPwPlotOverlay's comment
    // for the channel-ratio argument. Red is impossible in viridis (nothing in the ramp has red
    // dominant over green), magenta is impossible twice over (high red AND high blue), and white
    // is brighter than viridis's brightest.
    const FColor ColorOnset      (255,  48,  48, 255);
    const FColor ColorPitch      (255,   0, 200, 255);
    const FColor ColorEvent      (255, 255, 255, 255);

    /** Pitch-curve stroke width. 2 px, so the curve survives the downscale into a vision model. */
    constexpr int32 PitchStrokePx = 2;

    // Text is always rendered at 2x the 5x7 cell, i.e. 10x14 px glyphs. A 1x label on a 1024-wide
    // plot survives a full-resolution read but disappears the moment the image is downscaled,
    // which is exactly what happens on the way into a vision model.
    constexpr int32 TextScale = 2;

    // Upper bound on either image dimension. 8192x8192 FColor is already 256 MB before the PNG
    // encoder runs, and both dimensions come from the caller.
    constexpr int32 MaxImageDimension = 8192;

    // ---------------------------------------------------------------------------------------
    // 5x7 bitmap glyphs, one uint8 per row, bit 4 = leftmost column.
    //
    // Minimal set, and every entry is here because an axis label needs it:
    //   0-9 . -   numbers and negative dB / negative amplitude
    //   m s       "ms", "s"                (time axis)
    //   k H z     "Hz", "kHz"              (frequency axis)
    //   d B       "dB"                     (colour bar)
    //   L R       channel lane identity    (waveform)
    //   C         note names "C1".."C9"    (constant-Q octave axis)
    // Anything outside the set renders as blank space; the label formatters below only ever emit
    // characters from it. That includes the space in a constant-Q label ("C4 262Hz") - it has no
    // glyph on purpose and simply advances the pen one cell.
    // ---------------------------------------------------------------------------------------
    constexpr int32 GlyphW = 5;
    constexpr int32 GlyphH = 7;

    struct FGlyphDef
    {
        TCHAR Ch;
        uint8 Rows[GlyphH];
    };

    const FGlyphDef GGlyphTable[] =
    {
        { TEXT('0'), { 0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110 } },
        { TEXT('1'), { 0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 } },
        { TEXT('2'), { 0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111 } },
        { TEXT('3'), { 0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110 } },
        { TEXT('4'), { 0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010 } },
        { TEXT('5'), { 0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110 } },
        { TEXT('6'), { 0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110 } },
        { TEXT('7'), { 0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000 } },
        { TEXT('8'), { 0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110 } },
        { TEXT('9'), { 0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100 } },
        { TEXT('.'), { 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100 } },
        { TEXT('-'), { 0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000 } },
        { TEXT('k'), { 0b01000, 0b01000, 0b01001, 0b01010, 0b01100, 0b01010, 0b01001 } },
        { TEXT('s'), { 0b00000, 0b00000, 0b01111, 0b10000, 0b01110, 0b00001, 0b11110 } },
        { TEXT('m'), { 0b00000, 0b00000, 0b11010, 0b10101, 0b10101, 0b10101, 0b10101 } },
        { TEXT('z'), { 0b00000, 0b00000, 0b11111, 0b00010, 0b00100, 0b01000, 0b11111 } },
        { TEXT('d'), { 0b00001, 0b00001, 0b01111, 0b10001, 0b10001, 0b10001, 0b01111 } },
        { TEXT('H'), { 0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001 } },
        { TEXT('B'), { 0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110 } },
        { TEXT('C'), { 0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110 } },
        { TEXT('L'), { 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111 } },
        { TEXT('R'), { 0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001 } },
    };

    const uint8* FindGlyphRows(TCHAR Ch)
    {
        for (const FGlyphDef& Glyph : GGlyphTable)
        {
            if (Glyph.Ch == Ch)
            {
                return Glyph.Rows;
            }
        }
        return nullptr;
    }

    /** Rendered width of NumChars glyphs at Scale, including the 1-cell inter-glyph gap. */
    int32 TextWidth(int32 NumChars, int32 Scale)
    {
        return NumChars > 0 ? (NumChars * (GlyphW + 1) - 1) * Scale : 0;
    }

    int32 TextHeight(int32 Scale)
    {
        return GlyphH * Scale;
    }

    // ---------------------------------------------------------------------------------------
    // Canvas
    // ---------------------------------------------------------------------------------------
    struct FPlotCanvas
    {
        int32 Width = 0;
        int32 Height = 0;
        TArray<FColor> Pixels;

        void Init(int32 InWidth, int32 InHeight, const FColor& Background)
        {
            Width = InWidth;
            Height = InHeight;
            Pixels.Init(Background, InWidth * InHeight);
        }

        FORCEINLINE void Set(int32 X, int32 Y, const FColor& Color)
        {
            if (X < 0 || Y < 0 || X >= Width || Y >= Height)
            {
                return;
            }
            Pixels[Y * Width + X] = Color;
        }

        /** Inclusive on both corners. */
        void FillRect(int32 X0, int32 Y0, int32 X1, int32 Y1, const FColor& Color)
        {
            for (int32 Y = FMath::Max(Y0, 0); Y <= FMath::Min(Y1, Height - 1); ++Y)
            {
                for (int32 X = FMath::Max(X0, 0); X <= FMath::Min(X1, Width - 1); ++X)
                {
                    Pixels[Y * Width + X] = Color;
                }
            }
        }

        void VLine(int32 X, int32 Y0, int32 Y1, const FColor& Color)
        {
            FillRect(X, FMath::Min(Y0, Y1), X, FMath::Max(Y0, Y1), Color);
        }

        void HLine(int32 Y, int32 X0, int32 X1, const FColor& Color)
        {
            FillRect(FMath::Min(X0, X1), Y, FMath::Max(X0, X1), Y, Color);
        }

        void Rect(int32 X0, int32 Y0, int32 X1, int32 Y1, const FColor& Color)
        {
            HLine(Y0, X0, X1, Color);
            HLine(Y1, X0, X1, Color);
            VLine(X0, Y0, Y1, Color);
            VLine(X1, Y0, Y1, Color);
        }

        /** Filled square of side 2*Radius+1 centred on (X, Y). Used for isolated overlay points. */
        void Dot(int32 X, int32 Y, int32 Radius, const FColor& Color)
        {
            FillRect(X - Radius, Y - Radius, X + Radius, Y + Radius, Color);
        }

        /**
         * Bresenham segment stamped with a Thickness x Thickness brush. Overlay curves are drawn
         * thicker than one pixel deliberately: a 1 px trace vanishes the moment the image is
         * downscaled on the way into a vision model, which is the only way this image is ever read.
         */
        void Line(int32 X0, int32 Y0, int32 X1, int32 Y1, const FColor& Color, int32 Thickness)
        {
            const int32 Brush = FMath::Max(Thickness, 1);
            const int32 Half = Brush / 2;
            const int32 Dx = FMath::Abs(X1 - X0);
            const int32 Dy = -FMath::Abs(Y1 - Y0);
            const int32 StepX = X0 < X1 ? 1 : -1;
            const int32 StepY = Y0 < Y1 ? 1 : -1;
            int32 Error = Dx + Dy;
            for (;;)
            {
                FillRect(X0 - Half, Y0 - Half, X0 - Half + Brush - 1, Y0 - Half + Brush - 1, Color);
                if (X0 == X1 && Y0 == Y1)
                {
                    break;
                }
                const int32 DoubledError = 2 * Error;
                if (DoubledError >= Dy)
                {
                    Error += Dy;
                    X0 += StepX;
                }
                if (DoubledError <= Dx)
                {
                    Error += Dx;
                    Y0 += StepY;
                }
            }
        }

        void DrawText(int32 X, int32 Y, const FString& Text, const FColor& Color, int32 Scale)
        {
            int32 PenX = X;
            for (int32 Index = 0; Index < Text.Len(); ++Index)
            {
                if (const uint8* Rows = FindGlyphRows(Text[Index]))
                {
                    for (int32 Row = 0; Row < GlyphH; ++Row)
                    {
                        for (int32 Col = 0; Col < GlyphW; ++Col)
                        {
                            if ((Rows[Row] & (1 << (GlyphW - 1 - Col))) != 0)
                            {
                                FillRect(PenX + Col * Scale, Y + Row * Scale,
                                    PenX + Col * Scale + Scale - 1, Y + Row * Scale + Scale - 1, Color);
                            }
                        }
                    }
                }
                PenX += (GlyphW + 1) * Scale;
            }
        }

        void DrawTextRightAligned(int32 RightX, int32 Y, const FString& Text, const FColor& Color, int32 Scale)
        {
            DrawText(RightX - TextWidth(Text.Len(), Scale), Y, Text, Color, Scale);
        }

        void DrawTextCentered(int32 CenterX, int32 Y, const FString& Text, const FColor& Color, int32 Scale)
        {
            DrawText(CenterX - TextWidth(Text.Len(), Scale) / 2, Y, Text, Color, Scale);
        }
    };

    // ---------------------------------------------------------------------------------------
    // Axis helpers
    // ---------------------------------------------------------------------------------------
    struct FAxisTick
    {
        int32 Pixel = 0;
        FString Label;
        bool bDrawLabel = true;
    };

    double Log10d(double Value)
    {
        return FMath::Loge(Value) / FMath::Loge(10.0);
    }

    /** Octaves. The constant-Q band axis is linear in this, which is the whole point of the view. */
    double Log2d(double Value)
    {
        return FMath::Loge(Value) / FMath::Loge(2.0);
    }

    /** "Nice" 1/2/5 x 10^k step that yields roughly TargetTicks divisions across Range. */
    double NiceStep(double Range, int32 TargetTicks)
    {
        if (!(Range > 0.0) || TargetTicks <= 0)
        {
            return 0.0;
        }
        const double Raw = Range / static_cast<double>(TargetTicks);
        const double Magnitude = FMath::Pow(10.0, FMath::FloorToDouble(Log10d(Raw)));
        const double Normalized = Raw / Magnitude;
        double Multiplier = 10.0;
        if (Normalized <= 1.0)      { Multiplier = 1.0; }
        else if (Normalized <= 2.0) { Multiplier = 2.0; }
        else if (Normalized <= 5.0) { Multiplier = 5.0; }
        return Multiplier * Magnitude;
    }

    /** Drop labels (never tick marks) that would collide with the previous kept one. */
    void ThinCenteredLabels(TArray<FAxisTick>& Ticks, int32 Scale, int32 GapPx)
    {
        int32 LastRight = MIN_int32 / 2;
        for (FAxisTick& Tick : Ticks)
        {
            const int32 LabelWidth = TextWidth(Tick.Label.Len(), Scale);
            const int32 Left = Tick.Pixel - LabelWidth / 2;
            if (Left - LastRight < GapPx)
            {
                Tick.bDrawLabel = false;
                continue;
            }
            LastRight = Left + LabelWidth;
        }
    }

    void ThinStackedLabels(TArray<FAxisTick>& Ticks, int32 Scale, int32 GapPx)
    {
        Ticks.Sort([](const FAxisTick& A, const FAxisTick& B) { return A.Pixel < B.Pixel; });
        int32 LastBottom = MIN_int32 / 2;
        for (FAxisTick& Tick : Ticks)
        {
            const int32 Top = Tick.Pixel - TextHeight(Scale) / 2;
            if (Top - LastBottom < GapPx)
            {
                Tick.bDrawLabel = false;
                continue;
            }
            LastBottom = Top + TextHeight(Scale);
        }
    }

    FString FormatFrequencyLabel(double Hz)
    {
        if (Hz >= 1000.0)
        {
            const double Kilohertz = Hz / 1000.0;
            const double Rounded = FMath::RoundToDouble(Kilohertz);
            return FMath::IsNearlyEqual(Kilohertz, Rounded, 0.005)
                ? FString::Printf(TEXT("%dkHz"), static_cast<int32>(Rounded))
                : FString::Printf(TEXT("%.1fkHz"), Kilohertz);
        }
        return FString::Printf(TEXT("%dHz"), FMath::RoundToInt32(Hz));
    }

    FString FormatTimeLabel(double Seconds, bool bUseMilliseconds)
    {
        if (bUseMilliseconds)
        {
            return FString::Printf(TEXT("%dms"), FMath::RoundToInt32(Seconds * 1000.0));
        }
        const double Rounded = FMath::RoundToDouble(Seconds);
        return FMath::IsNearlyEqual(Seconds, Rounded, 0.005)
            ? FString::Printf(TEXT("%ds"), static_cast<int32>(Rounded))
            : FString::Printf(TEXT("%.1fs"), Seconds);
    }

    /** Time ticks over [0, DurationSeconds], mapped into [PixelStart, PixelEnd]. */
    void BuildTimeTicks(double DurationSeconds, int32 PixelStart, int32 PixelEnd, TArray<FAxisTick>& OutTicks)
    {
        const double Step = NiceStep(DurationSeconds, 6);
        if (!(Step > 0.0))
        {
            return;
        }
        // Milliseconds below 2 s, seconds above: "1500ms" is harder to read at a glance than "1.5s",
        // and "0.002s" is harder to read than "2ms".
        const bool bUseMilliseconds = DurationSeconds < 2.0;
        const int32 PixelSpan = PixelEnd - PixelStart;
        for (int32 Index = 0; ; ++Index)
        {
            const double Time = Index * Step;
            if (Time > DurationSeconds + Step * 0.001)
            {
                break;
            }
            FAxisTick Tick;
            Tick.Pixel = PixelStart + FMath::RoundToInt32(Time / DurationSeconds * PixelSpan);
            Tick.Label = FormatTimeLabel(Time, bUseMilliseconds);
            OutTicks.Add(MoveTemp(Tick));
        }
    }

    /**
     * Where the spectrogram data area ended up, and how to map a (time, frequency) pair onto it.
     *
     * This exists so the overlay lands on the SAME pixels the axes are labelled with: TimeToX
     * reproduces BuildTimeTicks' mapping exactly and FrequencyToY reproduces the frequency-tick
     * mapping exactly, so an onset drawn at 250 ms sits on the column the "250ms" tick points at.
     * Two independent mappings would drift by a pixel or two and quietly make the labels wrong.
     */
    struct FSpectrogramLayout
    {
        int32 PlotX0 = 0;
        int32 PlotX1 = 0;
        int32 PlotY0 = 0;
        int32 PlotY1 = 0;
        int32 PlotW = 0;
        int32 PlotH = 0;
        bool bLogFrequency = false;
        double MinHz = 0.0;
        double MaxHz = 0.0;
        double DurationSeconds = 0.0;

        bool ContainsTime(double Seconds) const
        {
            return DurationSeconds > 0.0 && Seconds >= 0.0 && Seconds <= DurationSeconds;
        }

        int32 TimeToX(double Seconds) const
        {
            return PlotX0 + FMath::RoundToInt32(Seconds / DurationSeconds * (PlotX1 - PlotX0));
        }

        /** A frequency the axis can actually show. DC is off a log axis; anything above MaxHz is off both. */
        bool ContainsFrequency(double Hz) const
        {
            return Hz > 0.0 && Hz <= MaxHz && (!bLogFrequency || Hz >= MinHz);
        }

        int32 FrequencyToY(double Hz) const
        {
            const double Fraction = bLogFrequency
                ? Log10d(MaxHz / Hz) / Log10d(MaxHz / MinHz)
                : (MaxHz - Hz) / MaxHz;
            return PlotY0 + FMath::RoundToInt32(FMath::Clamp(Fraction, 0.0, 1.0) * (PlotH - 1));
        }
    };

    /**
     * The fixed-dB colour bar, drawn to the right of a plot area spanning PlotY0..PlotY1.
     *
     * Shared by the spectrogram and the constant-Q view rather than copied: the bar is the only
     * thing in either image that states the absolute scale, so two copies that drift would make
     * one of the two images silently unreadable.
     */
    void DrawDbColorBar(FPlotCanvas& Canvas, int32 BarX0, int32 BarX1, int32 PlotY0, int32 PlotY1,
                        double MinDb, double MaxDb)
    {
        const int32 PlotH = PlotY1 - PlotY0 + 1;
        const double DbSpan = MaxDb - MinDb;
        if (PlotH < 2 || !(DbSpan > 0.0))
        {
            return;
        }

        for (int32 Row = 0; Row < PlotH; ++Row)
        {
            const float Normalized = 1.f - static_cast<float>(Row) / static_cast<float>(PlotH - 1);
            Canvas.FillRect(BarX0, PlotY0 + Row, BarX1, PlotY0 + Row, PwViridis(Normalized));
        }
        Canvas.Rect(BarX0, PlotY0, BarX1, PlotY1, ColorAxis);

        TArray<FAxisTick> DbTicks;
        auto AddDbTick = [&](double Db)
        {
            const double Fraction = (MaxDb - Db) / DbSpan;
            const int32 Y = PlotY0 + FMath::RoundToInt32(FMath::Clamp(Fraction, 0.0, 1.0) * (PlotH - 1));
            DbTicks.Add(FAxisTick{ Y, FString::Printf(TEXT("%ddB"), FMath::RoundToInt32(Db)), true });
        };
        const double DbStep = NiceStep(DbSpan, 6);
        double LowestLabelledDb = MaxDb;
        if (DbStep > 0.0)
        {
            for (double Db = MaxDb; Db >= MinDb - DbStep * 0.001; Db -= DbStep)
            {
                AddDbTick(Db);
                LowestLabelledDb = Db;
            }
        }
        // Always label the floor even when the nice step misses it: MinDb is the number that makes
        // this image comparable with the next one, so leaving it implicit defeats the fixed scale.
        if (FMath::Abs(LowestLabelledDb - MinDb) > 0.5)
        {
            AddDbTick(MinDb);
        }
        ThinStackedLabels(DbTicks, TextScale, 6);
        for (const FAxisTick& Tick : DbTicks)
        {
            Canvas.HLine(Tick.Pixel, BarX1 + 1, BarX1 + 5, ColorAxis);
            if (Tick.bDrawLabel)
            {
                Canvas.DrawText(BarX1 + 9, Tick.Pixel - TextHeight(TextScale) / 2, Tick.Label, ColorText, TextScale);
            }
        }
    }

    /** Bottom time axis: tick marks plus thinned labels, shared by every time-domain image here. */
    void DrawTimeAxis(FPlotCanvas& Canvas, const TArray<FAxisTick>& TimeTicks, int32 PlotY1)
    {
        for (const FAxisTick& Tick : TimeTicks)
        {
            Canvas.VLine(Tick.Pixel, PlotY1 + 1, PlotY1 + 6, ColorAxis);
            if (Tick.bDrawLabel)
            {
                Canvas.DrawTextCentered(Tick.Pixel, PlotY1 + 10, Tick.Label, ColorText, TextScale);
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    // PNG encode + write
    // ---------------------------------------------------------------------------------------
    bool EncodeAndWrite(const FPlotCanvas& Canvas, const FString& AbsolutePngPath, FString& OutError)
    {
        if (AbsolutePngPath.IsEmpty())
        {
            OutError = TEXT("Output PNG path is empty.");
            return false;
        }
        if (FPaths::IsRelative(AbsolutePngPath))
        {
            // Refused rather than resolved: the editor's working directory is not a place the
            // caller chose, and a file written there is a file the caller cannot find again.
            OutError = FString::Printf(
                TEXT("Output PNG path '%s' is relative; an absolute path is required."), *AbsolutePngPath);
            return false;
        }

        const FString Directory = FPaths::GetPath(AbsolutePngPath);
        if (!Directory.IsEmpty() && !IFileManager::Get().DirectoryExists(*Directory))
        {
            IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
        }

        // PNGCompressImageArray, never the UE_DEPRECATED(5.1) CompressImageArray: that one
        // forwards to ThumbnailCompressImageArray, which chooses png or jpg on its own and would
        // hand back a lossy JPEG spectrogram under a .png filename.
        TArray64<uint8> PngBytes;
        FImageUtils::PNGCompressImageArray(Canvas.Width, Canvas.Height,
            TArrayView64<const FColor>(Canvas.Pixels.GetData(), Canvas.Pixels.Num()), PngBytes);
        if (PngBytes.Num() == 0)
        {
            OutError = FString::Printf(
                TEXT("FImageUtils::PNGCompressImageArray produced 0 bytes for a %dx%d image."),
                Canvas.Width, Canvas.Height);
            return false;
        }

        if (!FFileHelper::SaveArrayToFile(PngBytes, *AbsolutePngPath))
        {
            OutError = FString::Printf(TEXT("Failed to write %lld PNG bytes to '%s'."),
                static_cast<int64>(PngBytes.Num()), *AbsolutePngPath);
            return false;
        }

        // Re-probe the filesystem rather than trusting the writer's own bool (rpc-design.md §4).
        const int64 SizeOnDisk = IFileManager::Get().FileSize(*AbsolutePngPath);
        if (SizeOnDisk != PngBytes.Num())
        {
            OutError = FString::Printf(
                TEXT("Wrote '%s' but the file is %lld bytes on disk, not the %lld encoded."),
                *AbsolutePngPath, SizeOnDisk, static_cast<int64>(PngBytes.Num()));
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------------------------------------
    // Spectrogram renderer
    //
    // The whole spectrogram lives here, one step short of the PNG encode, so that the plain and
    // the overlaid entry points draw the SAME image from the SAME code. A second copy for the
    // overlay would be a second thing to keep in sync, and the byte-identity guarantee for an
    // empty overlay would be a promise rather than a structural fact.
    // ---------------------------------------------------------------------------------------
    bool RenderSpectrogramCanvas(const FPwStftResult& In, int32 SampleRate, bool bLogFrequency,
                                 const FPwSpectrogramPlotSettings& Settings,
                                 FPlotCanvas& OutCanvas, FSpectrogramLayout& OutLayout, FString& OutError)
    {
        constexpr int32 MarginLeft = 88;
        constexpr int32 MarginRight = 108;
        constexpr int32 MarginTop = 12;
        constexpr int32 MarginBottom = 44;
        constexpr int32 MinWidth = 320;
        constexpr int32 MinHeight = 200;
        constexpr int32 ColorBarWidth = 16;

        if (!In.IsValid())
        {
            // Named precisely, because the two ways to get here need different fixes: an empty result
            // means the STFT never ran, a missing FftSize/HopSize means the result was assembled by
            // hand and carries no time axis.
            OutError = FString::Printf(
                TEXT("STFT result is not plottable: numFrames=%d numBins=%d fftSize=%d hopSize=%d magnitudes=%d ")
                TEXT("(expected numFrames*numBins). Produce it with PwComputeStft."),
                In.NumFrames, In.NumBins, In.FftSize, In.HopSize, In.Magnitudes.Num());
            return false;
        }
        if (SampleRate <= 0)
        {
            OutError = FString::Printf(TEXT("SampleRate is %d; a non-positive rate leaves both axes unlabellable."),
                SampleRate);
            return false;
        }
        if (!(In.BinHz > 0.f))
        {
            OutError = FString::Printf(TEXT("STFT result reports BinHz=%.6f; cannot map bins to frequencies."), In.BinHz);
            return false;
        }
        if (!(Settings.MaxDb > Settings.MinDb))
        {
            OutError = FString::Printf(TEXT("dB range is empty: MinDb=%.2f MaxDb=%.2f."), Settings.MinDb, Settings.MaxDb);
            return false;
        }
        if (Settings.Width < MinWidth || Settings.Height < MinHeight
            || Settings.Width > MaxImageDimension || Settings.Height > MaxImageDimension)
        {
            OutError = FString::Printf(
                TEXT("Requested %dx%d image is outside the supported %dx%d..%dx%d range ")
                TEXT("(the minimum is what labelled axes and a colour bar need; the maximum bounds the allocation)."),
                Settings.Width, Settings.Height, MinWidth, MinHeight, MaxImageDimension, MaxImageDimension);
            return false;
        }

        const double Nyquist = static_cast<double>(In.NumBins - 1) * static_cast<double>(In.BinHz);
        const double MaxHz = Settings.MaxHz > 0.f
            ? FMath::Min(static_cast<double>(Settings.MaxHz), Nyquist)
            : Nyquist;
        if (!(MaxHz > 0.0))
        {
            OutError = FString::Printf(TEXT("Frequency axis top resolved to %.3f Hz."), MaxHz);
            return false;
        }
        // The log axis needs a positive floor; DC cannot be placed on it at all.
        const double MinHz = bLogFrequency
            ? FMath::Clamp(static_cast<double>(Settings.MinHz), 1.0, MaxHz * 0.5)
            : 0.0;

        FPlotCanvas& Canvas = OutCanvas;
        Canvas.Init(Settings.Width, Settings.Height, ColorPage);

        const int32 PlotX0 = MarginLeft;
        const int32 PlotX1 = Settings.Width - MarginRight - 1;
        const int32 PlotY0 = MarginTop;
        const int32 PlotY1 = Settings.Height - MarginBottom - 1;
        const int32 PlotW = PlotX1 - PlotX0 + 1;
        const int32 PlotH = PlotY1 - PlotY0 + 1;
        if (PlotW < 32 || PlotH < 32)
        {
            OutError = FString::Printf(TEXT("Plot area collapsed to %dx%d after axis margins."), PlotW, PlotH);
            return false;
        }

        // Row -> [FirstBin, LastBin) max-pool span. Precomputed once; the pool is what keeps a
        // single-bin tone visible when 1025 bins are squeezed into a few hundred rows.
        TArray<int32> RowBinFirst;
        TArray<int32> RowBinLast;
        RowBinFirst.SetNumUninitialized(PlotH);
        RowBinLast.SetNumUninitialized(PlotH);
        auto FrequencyAtRowFraction = [&](double Fraction) -> double
        {
            return bLogFrequency
                ? MaxHz * FMath::Pow(MinHz / MaxHz, Fraction)
                : MaxHz * (1.0 - Fraction);
        };
        for (int32 Row = 0; Row < PlotH; ++Row)
        {
            const double HighHz = FrequencyAtRowFraction(static_cast<double>(Row) / PlotH);
            const double LowHz = FrequencyAtRowFraction(static_cast<double>(Row + 1) / PlotH);
            const int32 First = FMath::Clamp(FMath::FloorToInt32(LowHz / In.BinHz), 0, In.NumBins - 1);
            const int32 Last = FMath::Clamp(FMath::CeilToInt32(HighHz / In.BinHz) + 1, First + 1, In.NumBins);
            RowBinFirst[Row] = First;
            RowBinLast[Row] = Last;
        }

        // Column -> per-bin max over that column's frames. Collapsing time first makes the whole
        // render O(NumFrames * NumBins + PlotW * sum(row spans)) instead of a nested re-scan.
        TArray<float> ColumnBinMax;
        ColumnBinMax.SetNumUninitialized(In.NumBins);

        const double DbSpan = static_cast<double>(Settings.MaxDb) - static_cast<double>(Settings.MinDb);

        for (int32 Column = 0; Column < PlotW; ++Column)
        {
            const int32 FirstFrame = static_cast<int32>(static_cast<int64>(Column) * In.NumFrames / PlotW);
            int32 LastFrame = static_cast<int32>(static_cast<int64>(Column + 1) * In.NumFrames / PlotW);
            LastFrame = FMath::Clamp(FMath::Max(LastFrame, FirstFrame + 1), 0, In.NumFrames);

            const float* RESTRICT Row0 = In.Magnitudes.GetData() + static_cast<int64>(FirstFrame) * In.NumBins;
            FMemory::Memcpy(ColumnBinMax.GetData(), Row0, In.NumBins * sizeof(float));
            for (int32 Frame = FirstFrame + 1; Frame < LastFrame; ++Frame)
            {
                const float* RESTRICT Source = In.Magnitudes.GetData() + static_cast<int64>(Frame) * In.NumBins;
                for (int32 Bin = 0; Bin < In.NumBins; ++Bin)
                {
                    ColumnBinMax[Bin] = FMath::Max(ColumnBinMax[Bin], Source[Bin]);
                }
            }

            const int32 X = PlotX0 + Column;
            for (int32 Row = 0; Row < PlotH; ++Row)
            {
                float Peak = 0.f;
                for (int32 Bin = RowBinFirst[Row]; Bin < RowBinLast[Row]; ++Bin)
                {
                    Peak = FMath::Max(Peak, ColumnBinMax[Bin]);
                }
                const float Db = PwMagnitudeToDb(Peak, Settings.MinDb);
                const float Normalized = static_cast<float>((Db - Settings.MinDb) / DbSpan);
                Canvas.Set(X, PlotY0 + Row, PwViridis(Normalized));
            }
        }

        Canvas.Rect(PlotX0, PlotY0, PlotX1, PlotY1, ColorAxis);

        OutLayout.PlotX0 = PlotX0;
        OutLayout.PlotX1 = PlotX1;
        OutLayout.PlotY0 = PlotY0;
        OutLayout.PlotY1 = PlotY1;
        OutLayout.PlotW = PlotW;
        OutLayout.PlotH = PlotH;
        OutLayout.bLogFrequency = bLogFrequency;
        OutLayout.MinHz = MinHz;
        OutLayout.MaxHz = MaxHz;

        // ---- Frequency axis (left). Ticks only, no grid over the data. ----
        TArray<FAxisTick> FrequencyTicks;
        if (bLogFrequency)
        {
            // 1-2-5 per decade; ThinStackedLabels drops whichever of those collide.
            static const double Mantissas[] = { 1.0, 2.0, 5.0 };
            for (int32 Decade = 0; Decade <= 5; ++Decade)
            {
                const double DecadeScale = FMath::Pow(10.0, static_cast<double>(Decade));
                for (const double Mantissa : Mantissas)
                {
                    const double Hz = Mantissa * DecadeScale;
                    if (Hz >= MinHz && Hz <= MaxHz)
                    {
                        FrequencyTicks.Add(FAxisTick{ OutLayout.FrequencyToY(Hz), FormatFrequencyLabel(Hz), true });
                    }
                }
            }
        }
        else
        {
            const double Step = NiceStep(MaxHz, 6);
            if (Step > 0.0)
            {
                for (int32 Index = 0; ; ++Index)
                {
                    const double Hz = Index * Step;
                    if (Hz > MaxHz + Step * 0.001)
                    {
                        break;
                    }
                    FrequencyTicks.Add(FAxisTick{ OutLayout.FrequencyToY(Hz), FormatFrequencyLabel(Hz), true });
                }
            }
        }
        ThinStackedLabels(FrequencyTicks, TextScale, 6);
        for (const FAxisTick& Tick : FrequencyTicks)
        {
            Canvas.HLine(Tick.Pixel, PlotX0 - 6, PlotX0 - 1, ColorAxis);
            if (Tick.bDrawLabel)
            {
                Canvas.DrawTextRightAligned(PlotX0 - 10, Tick.Pixel - TextHeight(TextScale) / 2,
                    Tick.Label, ColorText, TextScale);
            }
        }

        // ---- Time axis (bottom). Labelled by analysis-window START time. ----
        OutLayout.DurationSeconds =
            static_cast<double>(In.NumFrames) * static_cast<double>(In.HopSize) / static_cast<double>(SampleRate);
        TArray<FAxisTick> TimeTicks;
        BuildTimeTicks(OutLayout.DurationSeconds, PlotX0, PlotX1, TimeTicks);
        ThinCenteredLabels(TimeTicks, TextScale, 10);
        DrawTimeAxis(Canvas, TimeTicks, PlotY1);

        // ---- Colour bar (right), carrying the FIXED dB scale so a reader can read absolute level
        //      off the image instead of guessing from relative brightness. ----
        const int32 BarX0 = PlotX1 + 14;
        DrawDbColorBar(Canvas, BarX0, BarX0 + ColorBarWidth - 1, PlotY0, PlotY1,
            Settings.MinDb, Settings.MaxDb);

        return true;
    }

    // ---------------------------------------------------------------------------------------
    // Overlay marks
    // ---------------------------------------------------------------------------------------
    void DrawOverlay(FPlotCanvas& Canvas, const FSpectrogramLayout& Layout, const FPwPlotOverlay& Overlay)
    {
        // Stated rather than left emergent: an overlay with nothing in it touches no pixel, which
        // is what makes the overlaid PNG byte-identical to the plain one.
        if (Overlay.IsEmpty())
        {
            return;
        }

        // Draw order is deliberate: event brackets frame regions and are the least precise mark,
        // onsets are single instants, the pitch curve is the finest detail. Coarse to fine means
        // the finer mark is never buried under the coarser one.

        // ---- Event bounds: brackets, never a shaded box. A wash over the spectrogram would change
        //      the apparent dB inside the region, and brightness on this image means level. ----
        for (const FVector2D& Bounds : Overlay.EventBoundsMs)
        {
            const double StartSeconds = Bounds.X / 1000.0;
            const double EndSeconds = Bounds.Y / 1000.0;
            if (!(EndSeconds > StartSeconds))
            {
                // A zero-length or reversed pair is not an event; drawing it would invent one.
                continue;
            }
            if (EndSeconds <= 0.0 || StartSeconds >= Layout.DurationSeconds)
            {
                continue;
            }

            const bool bStartVisible = StartSeconds >= 0.0;
            const bool bEndVisible = EndSeconds <= Layout.DurationSeconds;
            const int32 X0 = Layout.TimeToX(FMath::Max(StartSeconds, 0.0));
            const int32 X1 = Layout.TimeToX(FMath::Min(EndSeconds, Layout.DurationSeconds));
            const int32 CapHeight = FMath::Clamp(Layout.PlotH / 10, 4, 16);

            Canvas.HLine(Layout.PlotY0 + 1, X0, X1, ColorEvent);
            Canvas.HLine(Layout.PlotY1 - 1, X0, X1, ColorEvent);
            // End caps only where the boundary is really inside the image. A cap drawn at the edge
            // of a clipped event would assert a start or an end that was never measured there.
            if (bStartVisible)
            {
                Canvas.VLine(X0, Layout.PlotY0 + 1, Layout.PlotY0 + 1 + CapHeight, ColorEvent);
                Canvas.VLine(X0, Layout.PlotY1 - 1 - CapHeight, Layout.PlotY1 - 1, ColorEvent);
            }
            if (bEndVisible)
            {
                Canvas.VLine(X1, Layout.PlotY0 + 1, Layout.PlotY0 + 1 + CapHeight, ColorEvent);
                Canvas.VLine(X1, Layout.PlotY1 - 1 - CapHeight, Layout.PlotY1 - 1, ColorEvent);
            }
        }

        // ---- Onsets: full-height red rules. Dropped, not clamped, when outside the time axis. ----
        for (const double OnsetMs : Overlay.OnsetTimesMs)
        {
            const double Seconds = OnsetMs / 1000.0;
            if (!Layout.ContainsTime(Seconds))
            {
                continue;
            }
            Canvas.VLine(Layout.TimeToX(Seconds), Layout.PlotY0, Layout.PlotY1, ColorOnset);
        }

        // ---- Pitch track: only confident points, and the curve BREAKS rather than bridging a gap.
        //      PreviousIndex is the mechanism - it is reset by every dropped point, so a segment can
        //      only ever span two array neighbours that both survived the confidence floor. ----
        int32 PreviousIndex = INDEX_NONE;
        int32 PreviousX = 0;
        int32 PreviousY = 0;
        double PreviousTimeMs = 0.0;
        for (int32 Index = 0; Index < Overlay.PitchTrack.Num(); ++Index)
        {
            const FPwPitchPointResult& Point = Overlay.PitchTrack[Index];
            const double TimeMs = static_cast<double>(Point.TimeMs);
            const double F0Hz = static_cast<double>(Point.F0Hz);
            const bool bKeep =
                static_cast<double>(Point.Confidence) >= Overlay.MinPitchConfidence
                && FMath::IsFinite(F0Hz) && F0Hz > 0.0
                && Layout.ContainsTime(TimeMs / 1000.0)
                && Layout.ContainsFrequency(F0Hz);
            if (!bKeep)
            {
                PreviousIndex = INDEX_NONE;
                continue;
            }

            const int32 X = Layout.TimeToX(TimeMs / 1000.0);
            const int32 Y = Layout.FrequencyToY(F0Hz);
            const bool bJoinToPrevious = PreviousIndex == Index - 1
                && TimeMs > PreviousTimeMs
                && (TimeMs - PreviousTimeMs) <= Overlay.MaxPitchGapMs;
            if (bJoinToPrevious)
            {
                Canvas.Line(PreviousX, PreviousY, X, Y, ColorPitch, PitchStrokePx);
            }
            else
            {
                // Segment start, or a lone confident point in a sea of noise. Either way it is a
                // real measurement and gets a mark of its own instead of disappearing.
                Canvas.Dot(X, Y, PitchStrokePx / 2 + 1, ColorPitch);
            }

            PreviousIndex = Index;
            PreviousX = X;
            PreviousY = Y;
            PreviousTimeMs = TimeMs;
        }
    }
}

// -------------------------------------------------------------------------------------------
// Colormap
// -------------------------------------------------------------------------------------------
FColor PwViridis(float T)
{
    // Nine evenly spaced viridis anchors, linearly interpolated. Viridis is perceptually uniform
    // and monotonic in luminance, so "brighter" always means "louder" - a rainbow ramp is neither,
    // and its luminance reversals read to a vision model as edges that are not in the data.
    static const uint8 Anchors[9][3] =
    {
        {  68,   1,  84 },
        {  72,  40, 120 },
        {  62,  74, 137 },
        {  49, 104, 142 },
        {  38, 130, 142 },
        {  31, 158, 137 },
        {  53, 183, 121 },
        { 109, 205,  89 },
        { 253, 231,  37 },
    };
    constexpr int32 NumAnchors = UE_ARRAY_COUNT(Anchors);

    const float Clamped = FMath::Clamp(T, 0.f, 1.f);
    const float Scaled = Clamped * (NumAnchors - 1);
    const int32 Low = FMath::Clamp(FMath::FloorToInt(Scaled), 0, NumAnchors - 2);
    const float Alpha = Scaled - static_cast<float>(Low);

    return FColor(
        static_cast<uint8>(FMath::RoundToInt(FMath::Lerp<float>(Anchors[Low][0], Anchors[Low + 1][0], Alpha))),
        static_cast<uint8>(FMath::RoundToInt(FMath::Lerp<float>(Anchors[Low][1], Anchors[Low + 1][1], Alpha))),
        static_cast<uint8>(FMath::RoundToInt(FMath::Lerp<float>(Anchors[Low][2], Anchors[Low + 1][2], Alpha))),
        255);
}

// -------------------------------------------------------------------------------------------
// Waveform
// -------------------------------------------------------------------------------------------
bool PwPlotWaveform(const FPwAudioBuffer& In, const FString& AbsolutePngPath, FString& OutError,
                    const FPwWaveformPlotSettings* SettingsOrNull)
{
    using namespace PwAudioPlotInternal;

    OutError.Reset();
    const FPwWaveformPlotSettings Settings = SettingsOrNull ? *SettingsOrNull : FPwWaveformPlotSettings();

    constexpr int32 MarginLeft = 88;
    constexpr int32 MarginRight = 16;
    constexpr int32 MarginTop = 10;
    constexpr int32 MarginBottom = 44;
    constexpr int32 MinWidth = 256;
    constexpr int32 MinHeight = 128;

    if (Settings.Width < MinWidth || Settings.Height < MinHeight
        || Settings.Width > MaxImageDimension || Settings.Height > MaxImageDimension)
    {
        OutError = FString::Printf(
            TEXT("Requested %dx%d image is outside the supported %dx%d..%dx%d range ")
            TEXT("(the minimum is what labelled axes need; the maximum bounds the allocation)."),
            Settings.Width, Settings.Height, MinWidth, MinHeight, MaxImageDimension, MaxImageDimension);
        return false;
    }
    if (!(Settings.AmplitudeRange > 0.f))
    {
        OutError = FString::Printf(TEXT("AmplitudeRange must be positive; got %.6f."), Settings.AmplitudeRange);
        return false;
    }
    if (In.SampleRate <= 0)
    {
        OutError = FString::Printf(
            TEXT("Buffer SampleRate is %d; a non-positive rate leaves the time axis unlabellable."),
            In.SampleRate);
        return false;
    }

    // Lanes: left over right, one lane per non-empty channel array. FPwAudioBuffer duplicates a
    // mono source into both channels, so such a buffer draws two identical lanes - which is what
    // it actually contains, and reads at a glance as "this is mono". An empty buffer is an error;
    // DIGITAL SILENCE is not, because a flat line on the fixed +/-AmplitudeRange scale says
    // "exactly zero" unambiguously (the spectrogram side has no equivalent and rejects silence).
    struct FLane
    {
        const float* Samples = nullptr;
        int32 Count = 0;
        FColor Color = FColor::White;
        FString Identity;
    };
    TArray<FLane, TInlineAllocator<2>> Lanes;
    if (In.Left.Num() > 0)
    {
        Lanes.Add(FLane{ In.Left.GetData(), In.Left.Num(), ColorChannelL, TEXT("L") });
    }
    if (In.Right.Num() > 0)
    {
        Lanes.Add(FLane{ In.Right.GetData(), In.Right.Num(), ColorChannelR, TEXT("R") });
    }
    if (Lanes.Num() == 0)
    {
        OutError = TEXT("Audio buffer has 0 samples in both channels; there is nothing to plot.");
        return false;
    }

    int32 LongestChannel = 0;
    for (const FLane& Lane : Lanes)
    {
        LongestChannel = FMath::Max(LongestChannel, Lane.Count);
    }
    const double DurationSeconds = static_cast<double>(LongestChannel) / static_cast<double>(In.SampleRate);

    FPlotCanvas Canvas;
    Canvas.Init(Settings.Width, Settings.Height, ColorPage);

    const int32 PlotX0 = MarginLeft;
    const int32 PlotX1 = Settings.Width - MarginRight - 1;
    const int32 PlotY0 = MarginTop;
    const int32 PlotY1 = Settings.Height - MarginBottom - 1;
    const int32 PlotW = PlotX1 - PlotX0 + 1;
    const int32 PlotH = PlotY1 - PlotY0 + 1;
    if (PlotW < 32 || PlotH < 32)
    {
        OutError = FString::Printf(TEXT("Plot area collapsed to %dx%d after axis margins."), PlotW, PlotH);
        return false;
    }

    constexpr int32 LaneGap = 6;
    const int32 LaneHeight = (PlotH - LaneGap * (Lanes.Num() - 1)) / Lanes.Num();

    // Time ticks first: the grid is drawn under the signal.
    TArray<FAxisTick> TimeTicks;
    BuildTimeTicks(DurationSeconds, PlotX0, PlotX1, TimeTicks);
    ThinCenteredLabels(TimeTicks, TextScale, 10);

    for (int32 LaneIndex = 0; LaneIndex < Lanes.Num(); ++LaneIndex)
    {
        const FLane& Lane = Lanes[LaneIndex];
        const int32 LaneTop = PlotY0 + LaneIndex * (LaneHeight + LaneGap);
        const int32 LaneBottom = LaneTop + LaneHeight - 1;
        const int32 LaneZeroY = LaneTop + (LaneHeight - 1) / 2;

        Canvas.FillRect(PlotX0, LaneTop, PlotX1, LaneBottom, ColorPlotGround);
        for (const FAxisTick& Tick : TimeTicks)
        {
            Canvas.VLine(Tick.Pixel, LaneTop, LaneBottom, ColorGrid);
        }
        Canvas.HLine(LaneZeroY, PlotX0, PlotX1, ColorZeroLine);

        auto ValueToY = [&](double Value) -> int32
        {
            const double Clamped = FMath::Clamp(Value,
                -static_cast<double>(Settings.AmplitudeRange), static_cast<double>(Settings.AmplitudeRange));
            const double Normalized = (Clamped + Settings.AmplitudeRange) / (2.0 * Settings.AmplitudeRange);
            return LaneTop + FMath::RoundToInt32((1.0 - Normalized) * (LaneHeight - 1));
        };

        for (int32 Column = 0; Column < PlotW; ++Column)
        {
            const int64 First = static_cast<int64>(Column) * Lane.Count / PlotW;
            int64 Last = static_cast<int64>(Column + 1) * Lane.Count / PlotW;
            if (Last <= First)
            {
                Last = First + 1;
            }
            Last = FMath::Min<int64>(Last, Lane.Count);
            if (First >= Last)
            {
                continue;
            }

            float MinSample = Lane.Samples[First];
            float MaxSample = Lane.Samples[First];
            double SumOfSquares = 0.0;
            for (int64 Index = First; Index < Last; ++Index)
            {
                const float Sample = Lane.Samples[Index];
                MinSample = FMath::Min(MinSample, Sample);
                MaxSample = FMath::Max(MaxSample, Sample);
                SumOfSquares += static_cast<double>(Sample) * static_cast<double>(Sample);
            }
            const double Rms = FMath::Sqrt(SumOfSquares / static_cast<double>(Last - First));

            const int32 X = PlotX0 + Column;
            // Peak envelope underneath, RMS band on top: the outer extent reads as "how loud did
            // it ever get", the solid inner band as "how loud is it on average" - the two numbers
            // a listener would ask for, and they cannot be recovered from either alone.
            Canvas.VLine(X, ValueToY(MaxSample), ValueToY(MinSample), Lane.Color);
            Canvas.VLine(X, ValueToY(Rms), ValueToY(-Rms), ColorRms);
        }

        Canvas.Rect(PlotX0, LaneTop, PlotX1, LaneBottom, ColorAxis);

        // Fixed amplitude labels. Never derived from the data, so two plots are comparable.
        const int32 LabelRight = PlotX0 - 8;
        const int32 HalfText = TextHeight(TextScale) / 2;
        Canvas.DrawTextRightAligned(LabelRight, LaneTop - HalfText + 1,
            FString::Printf(TEXT("%.1f"), Settings.AmplitudeRange), ColorText, TextScale);
        Canvas.DrawTextRightAligned(LabelRight, LaneZeroY - HalfText,
            TEXT("0.0"), ColorText, TextScale);
        Canvas.DrawTextRightAligned(LabelRight, LaneBottom - HalfText - 1,
            FString::Printf(TEXT("%.1f"), -Settings.AmplitudeRange), ColorText, TextScale);

        // Channel identity inside the lane so a downscaled read still knows which lane is which.
        Canvas.DrawText(PlotX0 + 6, LaneTop + 4, Lane.Identity, Lane.Color, TextScale);
    }

    // Time axis marks + labels below the plot.
    for (const FAxisTick& Tick : TimeTicks)
    {
        Canvas.VLine(Tick.Pixel, PlotY1 + 1, PlotY1 + 6, ColorAxis);
        if (Tick.bDrawLabel)
        {
            Canvas.DrawTextCentered(Tick.Pixel, PlotY1 + 10, Tick.Label, ColorText, TextScale);
        }
    }

    return EncodeAndWrite(Canvas, AbsolutePngPath, OutError);
}

// -------------------------------------------------------------------------------------------
// Spectrogram
// -------------------------------------------------------------------------------------------
bool PwPlotSpectrogram(const FPwStftResult& In, int32 SampleRate, bool bLogFrequency,
                       const FString& AbsolutePngPath, FString& OutError,
                       const FPwSpectrogramPlotSettings* SettingsOrNull)
{
    using namespace PwAudioPlotInternal;

    OutError.Reset();
    const FPwSpectrogramPlotSettings Settings = SettingsOrNull ? *SettingsOrNull : FPwSpectrogramPlotSettings();

    FPlotCanvas Canvas;
    FSpectrogramLayout Layout;
    if (!RenderSpectrogramCanvas(In, SampleRate, bLogFrequency, Settings, Canvas, Layout, OutError))
    {
        return false;
    }
    return EncodeAndWrite(Canvas, AbsolutePngPath, OutError);
}

bool PwPlotSpectrogramWithOverlay(const FPwStftResult& In, int32 SampleRate, bool bLogFrequency,
                                  const FPwPlotOverlay& Overlay, const FString& AbsolutePngPath,
                                  FString& OutError,
                                  const FPwSpectrogramPlotSettings* SettingsOrNull)
{
    using namespace PwAudioPlotInternal;

    OutError.Reset();
    const FPwSpectrogramPlotSettings Settings = SettingsOrNull ? *SettingsOrNull : FPwSpectrogramPlotSettings();

    // Same renderer, same settings, same canvas as PwPlotSpectrogram - so an empty overlay is not
    // "close to" the plain image, it IS the plain image, byte for byte. DrawOverlay touches no
    // pixel when every array is empty, and the PNG encoder is deterministic.
    FPlotCanvas Canvas;
    FSpectrogramLayout Layout;
    if (!RenderSpectrogramCanvas(In, SampleRate, bLogFrequency, Settings, Canvas, Layout, OutError))
    {
        return false;
    }

    DrawOverlay(Canvas, Layout, Overlay);

    return EncodeAndWrite(Canvas, AbsolutePngPath, OutError);
}

// -------------------------------------------------------------------------------------------
// Constant-Q
// -------------------------------------------------------------------------------------------
bool PwPlotConstantQ(const FPwAudioBuffer& In, const FString& AbsolutePngPath, FString& OutError,
                     const FPwConstantQPlotSettings* SettingsOrNull)
{
    using namespace PwAudioPlotInternal;

    OutError.Reset();
    const FPwConstantQPlotSettings Settings = SettingsOrNull ? *SettingsOrNull : FPwConstantQPlotSettings();

    // Wider left margin than the spectrogram: "C10 17kHz" is 106 px at TextScale 2, and the note
    // name is the label that makes this view worth rendering at all - clipping it would leave an
    // axis of bare numbers, which is the linear-spectrogram experience this view exists to replace.
    constexpr int32 MarginLeft = 128;
    constexpr int32 MarginRight = 108;
    constexpr int32 MarginTop = 12;
    constexpr int32 MarginBottom = 44;
    constexpr int32 MinWidth = 320;
    constexpr int32 MinHeight = 200;
    constexpr int32 ColorBarWidth = 16;

    // ---- Settings, checked before Audio::FPseudoConstantQ is reached at all. That code check()s
    //      its bands-per-octave, centre frequency, band width, FFT size and sample rate, so a zero
    //      or negative one would take the editor down instead of failing this call.
    if (Settings.Width < MinWidth || Settings.Height < MinHeight
        || Settings.Width > MaxImageDimension || Settings.Height > MaxImageDimension)
    {
        OutError = FString::Printf(
            TEXT("Requested %dx%d image is outside the supported %dx%d..%dx%d range ")
            TEXT("(the minimum is what labelled axes and a colour bar need; the maximum bounds the allocation)."),
            Settings.Width, Settings.Height, MinWidth, MinHeight, MaxImageDimension, MaxImageDimension);
        return false;
    }
    if (!(Settings.MaxDb > Settings.MinDb))
    {
        OutError = FString::Printf(TEXT("dB range is empty: MinDb=%.2f MaxDb=%.2f."), Settings.MinDb, Settings.MaxDb);
        return false;
    }
    if (Settings.NumBands < 2)
    {
        OutError = FString::Printf(TEXT("NumBands is %d; a constant-Q axis needs at least 2 bands."), Settings.NumBands);
        return false;
    }
    if (!(Settings.NumBandsPerOctave > 0.f))
    {
        OutError = FString::Printf(TEXT("NumBandsPerOctave is %.4f; it must be positive."), Settings.NumBandsPerOctave);
        return false;
    }
    if (!(Settings.LowestBandCenterHz > 0.f))
    {
        OutError = FString::Printf(
            TEXT("LowestBandCenterHz is %.4f; a log-spaced axis has no zero and no negative frequency."),
            Settings.LowestBandCenterHz);
        return false;
    }
    if (!(Settings.BandWidthStretch > 0.f))
    {
        OutError = FString::Printf(TEXT("BandWidthStretch is %.4f; it must be positive."), Settings.BandWidthStretch);
        return false;
    }
    if (Settings.FftSize <= 0 || Settings.HopSize <= 0)
    {
        OutError = FString::Printf(TEXT("FftSize=%d HopSize=%d; both must be positive."),
            Settings.FftSize, Settings.HopSize);
        return false;
    }

    // ---- Buffer.
    const int32 NumSamples = FMath::Max(In.Left.Num(), In.Right.Num());
    if (NumSamples == 0)
    {
        OutError = TEXT("Audio buffer has 0 samples in both channels; there is nothing to transform.");
        return false;
    }
    if (In.SampleRate <= 0)
    {
        OutError = FString::Printf(
            TEXT("Buffer SampleRate is %d; a non-positive rate leaves the band layout undefined."), In.SampleRate);
        return false;
    }

    const double Nyquist = static_cast<double>(In.SampleRate) * 0.5;
    if (!(static_cast<double>(Settings.LowestBandCenterHz) < Nyquist))
    {
        OutError = FString::Printf(
            TEXT("LowestBandCenterHz %.3f is at or above Nyquist %.3f for a %d Hz buffer; no band is measurable."),
            Settings.LowestBandCenterHz, Nyquist, In.SampleRate);
        return false;
    }

    // Bands whose centre would sit above Nyquist are DROPPED, not plotted. The engine kernel leaves
    // such a row empty, which renders at the floor of the colormap - indistinguishable from a
    // measured silence, i.e. a claim the data does not support (rpc-design.md §1).
    const int32 MeasurableBands = FMath::FloorToInt32(
        static_cast<double>(Settings.NumBandsPerOctave)
            * Log2d(Nyquist / static_cast<double>(Settings.LowestBandCenterHz))) + 1;
    const int32 NumBands = FMath::Min(Settings.NumBands, MeasurableBands);
    if (NumBands < 2)
    {
        OutError = FString::Printf(
            TEXT("Only %d of the %d requested bands fall below Nyquist %.3f Hz at %.4f bands/octave from %.3f Hz."),
            NumBands, Settings.NumBands, Nyquist, Settings.NumBandsPerOctave, Settings.LowestBandCenterHz);
        return false;
    }

    // Mono downmix. The constant-Q view is about pitch content, and two side-by-side channel panels
    // would halve exactly the vertical resolution the octave spacing needs. A buffer with one side
    // unfilled uses the side that is there rather than halving it, so a caller who filled only Left
    // is not silently plotted 6 dB down; FPwAudioBuffer duplicates a mono source into both channels,
    // so the common case is the average of two identical channels, which is that channel exactly.
    const bool bHasLeft = In.Left.Num() > 0;
    const bool bHasRight = In.Right.Num() > 0;
    const float DownmixScale = (bHasLeft && bHasRight) ? 0.5f : 1.f;
    TArray<float> Mono;
    Mono.SetNumZeroed(NumSamples);
    for (int32 Index = 0; Index < NumSamples; ++Index)
    {
        const float LeftSample = Index < In.Left.Num() ? In.Left[Index] : 0.f;
        const float RightSample = Index < In.Right.Num() ? In.Right[Index] : 0.f;
        Mono[Index] = (LeftSample + RightSample) * DownmixScale;
    }

    // ---- The spectrum the kernel is applied to. Pseudo constant-Q windows an existing DFT rather
    //      than running a filter bank, so the STFT is not an implementation detail here - its bin
    //      spacing is the floor on how narrow a low band can really be.
    FPwStftSettings StftSettings;
    StftSettings.FftSize = Settings.FftSize;
    StftSettings.HopSize = Settings.HopSize;

    FPwStftResult Stft;
    FPwStftError StftError;
    if (!PwComputeStft(Mono, In.SampleRate, StftSettings, Stft, &StftError))
    {
        OutError = FString::Printf(
            TEXT("The STFT the constant-Q kernel is applied to did not run (%s): %s"),
            *StftError.Code, *StftError.Message);
        return false;
    }

    // ---- Kernel: a (NumBands x NumBins) sparse matrix of truncated Gaussians, one row per band,
    //      centred on GetConstantQCenterFrequency(b) with GetConstantQBandWidth(...) width.
    Audio::FPseudoConstantQKernelSettings KernelSettings;
    KernelSettings.NumBands = NumBands;
    KernelSettings.NumBandsPerOctave = Settings.NumBandsPerOctave;
    KernelSettings.KernelLowestCenterFreq = Settings.LowestBandCenterHz;
    KernelSettings.BandWidthStretch = Settings.BandWidthStretch;
    KernelSettings.Normalization = Settings.Normalization;

    const TUniquePtr<Audio::FContiguousSparse2DKernelTransform> Kernel =
        Audio::NewPseudoConstantQKernelTransform(KernelSettings, Stft.FftSize, static_cast<float>(In.SampleRate));
    if (!Kernel.IsValid())
    {
        OutError = FString::Printf(
            TEXT("Audio::NewPseudoConstantQKernelTransform returned no kernel for %d bands, fftSize=%d, %d Hz."),
            NumBands, Stft.FftSize, In.SampleRate);
        return false;
    }
    if (Kernel->GetNumInElements() != Stft.NumBins || Kernel->GetNumOutElements() != NumBands)
    {
        // TransformArray check()s the input length, so a shape mismatch has to be caught here or it
        // becomes an assert instead of a returned failure.
        OutError = FString::Printf(
            TEXT("Constant-Q kernel is %dx%d but the spectrum is %d bins and %d bands were requested."),
            Kernel->GetNumOutElements(), Kernel->GetNumInElements(), Stft.NumBins, NumBands);
        return false;
    }

    const int64 NumBandValues = static_cast<int64>(Stft.NumFrames) * static_cast<int64>(NumBands);
    if (NumBandValues > static_cast<int64>(MAX_int32))
    {
        OutError = FString::Printf(
            TEXT("%d frames x %d bands exceeds the %d-element array limit; raise HopSize or shorten the buffer."),
            Stft.NumFrames, NumBands, MAX_int32);
        return false;
    }

    TArray<float> BandMagnitudes;
    BandMagnitudes.SetNumUninitialized(static_cast<int32>(NumBandValues));
    for (int32 Frame = 0; Frame < Stft.NumFrames; ++Frame)
    {
        Kernel->TransformArray(
            Stft.Magnitudes.GetData() + static_cast<int64>(Frame) * Stft.NumBins,
            BandMagnitudes.GetData() + static_cast<int64>(Frame) * NumBands);
    }

    // ---- Render.
    FPlotCanvas Canvas;
    Canvas.Init(Settings.Width, Settings.Height, ColorPage);

    const int32 PlotX0 = MarginLeft;
    const int32 PlotX1 = Settings.Width - MarginRight - 1;
    const int32 PlotY0 = MarginTop;
    const int32 PlotY1 = Settings.Height - MarginBottom - 1;
    const int32 PlotW = PlotX1 - PlotX0 + 1;
    const int32 PlotH = PlotY1 - PlotY0 + 1;
    if (PlotW < 32 || PlotH < 32)
    {
        OutError = FString::Printf(TEXT("Plot area collapsed to %dx%d after axis margins."), PlotW, PlotH);
        return false;
    }

    // Row -> [FirstBand, LastBand) max-pool span. The band axis is LINEAR in band index - that is
    // what makes every octave the same height - so the mapping is a plain division. Max-pooling
    // rather than averaging, for the same reason the spectrogram uses it: one narrow band is often
    // the entire content of the image, and averaging deletes it.
    TArray<int32> RowBandFirst;
    TArray<int32> RowBandLast;
    RowBandFirst.SetNumUninitialized(PlotH);
    RowBandLast.SetNumUninitialized(PlotH);
    auto BandAtRowFraction = [&](double Fraction) -> double
    {
        return (1.0 - Fraction) * static_cast<double>(NumBands - 1);
    };
    for (int32 Row = 0; Row < PlotH; ++Row)
    {
        const double HighBand = BandAtRowFraction(static_cast<double>(Row) / PlotH);
        const double LowBand = BandAtRowFraction(static_cast<double>(Row + 1) / PlotH);
        const int32 First = FMath::Clamp(FMath::FloorToInt32(LowBand), 0, NumBands - 1);
        const int32 Last = FMath::Clamp(FMath::CeilToInt32(HighBand) + 1, First + 1, NumBands);
        RowBandFirst[Row] = First;
        RowBandLast[Row] = Last;
    }

    TArray<float> ColumnBandMax;
    ColumnBandMax.SetNumUninitialized(NumBands);

    const double DbSpan = static_cast<double>(Settings.MaxDb) - static_cast<double>(Settings.MinDb);

    for (int32 Column = 0; Column < PlotW; ++Column)
    {
        const int32 FirstFrame = static_cast<int32>(static_cast<int64>(Column) * Stft.NumFrames / PlotW);
        int32 LastFrame = static_cast<int32>(static_cast<int64>(Column + 1) * Stft.NumFrames / PlotW);
        LastFrame = FMath::Clamp(FMath::Max(LastFrame, FirstFrame + 1), 0, Stft.NumFrames);

        const float* RESTRICT Row0 = BandMagnitudes.GetData() + static_cast<int64>(FirstFrame) * NumBands;
        FMemory::Memcpy(ColumnBandMax.GetData(), Row0, NumBands * sizeof(float));
        for (int32 Frame = FirstFrame + 1; Frame < LastFrame; ++Frame)
        {
            const float* RESTRICT Source = BandMagnitudes.GetData() + static_cast<int64>(Frame) * NumBands;
            for (int32 Band = 0; Band < NumBands; ++Band)
            {
                ColumnBandMax[Band] = FMath::Max(ColumnBandMax[Band], Source[Band]);
            }
        }

        const int32 X = PlotX0 + Column;
        for (int32 Row = 0; Row < PlotH; ++Row)
        {
            float Peak = 0.f;
            for (int32 Band = RowBandFirst[Row]; Band < RowBandLast[Row]; ++Band)
            {
                Peak = FMath::Max(Peak, ColumnBandMax[Band]);
            }
            const float Db = PwMagnitudeToDb(Peak, Settings.MinDb);
            const float Normalized = static_cast<float>((Db - Settings.MinDb) / DbSpan);
            Canvas.Set(X, PlotY0 + Row, PwViridis(Normalized));
        }
    }

    Canvas.Rect(PlotX0, PlotY0, PlotX1, PlotY1, ColorAxis);

    // ---- Pitch axis (left), labelled in note names at octave boundaries.
    const double LowestHz = static_cast<double>(Settings.LowestBandCenterHz);
    const double HighestHz = LowestHz * FMath::Pow(2.0,
        static_cast<double>(NumBands - 1) / static_cast<double>(Settings.NumBandsPerOctave));

    auto FrequencyToY = [&](double Hz) -> int32
    {
        const double BandIndex = static_cast<double>(Settings.NumBandsPerOctave) * Log2d(Hz / LowestHz);
        const double Fraction = 1.0 - BandIndex / static_cast<double>(NumBands - 1);
        return PlotY0 + FMath::RoundToInt32(FMath::Clamp(Fraction, 0.0, 1.0) * (PlotH - 1));
    };

    // The C frequencies come from A4 = 440 Hz equal temperament (C of octave n is MIDI note
    // 12*(n+1)), NOT from the band centres: LowestBandCenterHz is a caller setting and may sit
    // anywhere, and a tick placed at a true C is right either way. Any frequency maps exactly
    // because the axis is linear in log2(f) - which is also why ticks one octave apart come out at
    // even pixel spacing, and that even spacing is the visible proof the axis is constant-Q.
    TArray<FAxisTick> NoteTicks;
    for (int32 Octave = 0; Octave <= 10; ++Octave)
    {
        const double Hz = 440.0 * FMath::Pow(2.0, (12.0 * static_cast<double>(Octave + 1) - 69.0) / 12.0);
        if (Hz < LowestHz || Hz > HighestHz)
        {
            continue;
        }
        NoteTicks.Add(FAxisTick{ FrequencyToY(Hz),
            FString::Printf(TEXT("C%d %s"), Octave, *FormatFrequencyLabel(Hz)), true });
    }
    if (NoteTicks.Num() < 2)
    {
        // A caller-narrowed range holding fewer than two Cs. Fall back to octave-spaced Hz from the
        // lowest band: still octave spacing, which is the property this view exists for, just
        // without note names to hang it on. Uses only glyphs the frequency formatter already emits.
        NoteTicks.Reset();
        for (double Hz = LowestHz; Hz <= HighestHz * 1.0001; Hz *= 2.0)
        {
            NoteTicks.Add(FAxisTick{ FrequencyToY(Hz), FormatFrequencyLabel(Hz), true });
        }
    }
    ThinStackedLabels(NoteTicks, TextScale, 6);
    for (const FAxisTick& Tick : NoteTicks)
    {
        Canvas.HLine(Tick.Pixel, PlotX0 - 6, PlotX0 - 1, ColorAxis);
        if (Tick.bDrawLabel)
        {
            Canvas.DrawTextRightAligned(PlotX0 - 10, Tick.Pixel - TextHeight(TextScale) / 2,
                Tick.Label, ColorText, TextScale);
        }
    }

    // ---- Time axis (bottom), labelled by analysis-window START time, exactly as the spectrogram.
    const double DurationSeconds =
        static_cast<double>(Stft.NumFrames) * static_cast<double>(Stft.HopSize) / static_cast<double>(In.SampleRate);
    TArray<FAxisTick> TimeTicks;
    BuildTimeTicks(DurationSeconds, PlotX0, PlotX1, TimeTicks);
    ThinCenteredLabels(TimeTicks, TextScale, 10);
    DrawTimeAxis(Canvas, TimeTicks, PlotY1);

    // ---- Colour bar (right), same fixed window as the spectrogram so the two images read on one
    //      ramp. See the header: this is a BAND-LEVEL dB scale, not per-bin dBFS.
    const int32 BarX0 = PlotX1 + 14;
    DrawDbColorBar(Canvas, BarX0, BarX0 + ColorBarWidth - 1, PlotY0, PlotY1, Settings.MinDb, Settings.MaxDb);

    return EncodeAndWrite(Canvas, AbsolutePngPath, OutError);
}
