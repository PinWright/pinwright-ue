// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the two derived plot views in AudioGen/PwAudioPlot.h: the constant-Q transform view
// (PwPlotConstantQ) and the annotated spectrogram (PwPlotSpectrogramWithOverlay).
//
// Two of these assertions are load-bearing and the rest are hygiene:
//
//  * OCTAVES ARE EVENLY SPACED. Three tones an octave apart must land at three evenly spaced rows.
//    This is the only assertion that proves Audio::FPseudoConstantQ is really wired in - a linear
//    frequency axis would put the same three tones at 1:2 spacing, and every "the PNG exists"
//    assertion in this file would still pass. The rows are measured by decoding the PNG back and
//    finding the bright runs, not by re-deriving them from the plotter's own arithmetic.
//
//  * AN EMPTY OVERLAY IS BYTE-IDENTICAL to the plain spectrogram. That is what makes the overlay
//    entry point safe to call unconditionally, and it is a structural fact (one shared renderer)
//    rather than a promise, so a regression that forks the two renderers fails here.
//
// Per rpc-design.md §12 the failure direction is asserted too: a relative path, an unwritable path
// and an unusable input each have to return false, explain themselves, and leave nothing on disk.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioFeatures.h"
#include "AudioGen/PwAudioPlot.h"
#include "AudioGen/PwStft.h"

#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true, and an anonymous one
// would collide with the identically-shaped helpers in the sibling audio test files.
namespace PwAudioPlotViewTestHelpers
{
    constexpr int32 TestSampleRate = 48000;

    /** Brightness (R+G+B) above which a pixel counts as a plotted peak rather than page furniture. */
    constexpr int32 PeakBrightnessThreshold = 420;

    void AddSine(TArray<float>& Out, int32 SampleRate, double Hz, double Amplitude)
    {
        const double AngularStep = 2.0 * UE_DOUBLE_PI * Hz / static_cast<double>(SampleRate);
        for (int32 Index = 0; Index < Out.Num(); ++Index)
        {
            Out[Index] += static_cast<float>(Amplitude * FMath::Sin(AngularStep * Index));
        }
    }

    /** Stereo buffer carrying the same sum of tones in both channels. */
    FPwAudioBuffer MakeToneBuffer(int32 NumSamples, TArrayView<const double> ToneHz, double Amplitude)
    {
        TArray<float> Samples;
        Samples.SetNumZeroed(NumSamples);
        for (const double Hz : ToneHz)
        {
            AddSine(Samples, TestSampleRate, Hz, Amplitude);
        }

        FPwAudioBuffer Buffer;
        Buffer.SampleRate = TestSampleRate;
        Buffer.Left = Samples;
        Buffer.Right = Samples;
        return Buffer;
    }

    /**
     * Absolute scratch path under the automation transient dir. ConvertRelativePathToFull is not
     * cosmetic: AutomationTransientDir() is project-relative and every plotter here rejects a
     * relative path outright rather than resolving it against the editor's working directory.
     */
    FString ScratchPngPath(const TCHAR* Stem)
    {
        const FString FileName = FString::Printf(TEXT("%s_%s.png"), Stem,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        return FPaths::ConvertRelativePathToFull(
            FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("PinWrightAudioPlotViews"), FileName));
    }

    /** PNG signature + IHDR width/height. Checking the IHDR is what makes this a real size assertion. */
    bool ReadPngHeader(const FString& Path, int32& OutWidth, int32& OutHeight, int64& OutFileSize)
    {
        OutWidth = 0;
        OutHeight = 0;
        OutFileSize = 0;

        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *Path))
        {
            return false;
        }
        OutFileSize = Bytes.Num();
        if (Bytes.Num() < 24)
        {
            return false;
        }

        static const uint8 Signature[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
        for (int32 Index = 0; Index < 8; ++Index)
        {
            if (Bytes[Index] != Signature[Index])
            {
                return false;
            }
        }
        if (Bytes[12] != 'I' || Bytes[13] != 'H' || Bytes[14] != 'D' || Bytes[15] != 'R')
        {
            return false;
        }

        // IHDR width/height are big-endian uint32 at offsets 16 and 20.
        OutWidth = (Bytes[16] << 24) | (Bytes[17] << 16) | (Bytes[18] << 8) | Bytes[19];
        OutHeight = (Bytes[20] << 24) | (Bytes[21] << 16) | (Bytes[22] << 8) | Bytes[23];
        return true;
    }

    bool LoadBytes(const FString& Path, TArray<uint8>& OutBytes)
    {
        OutBytes.Reset();
        return FFileHelper::LoadFileToArray(OutBytes, *Path);
    }

    /** Decode the written PNG back to pixels, so the geometry assertions read the IMAGE, not the code. */
    bool DecodePng(const FString& Path, FImage& OutImage, FString& OutWhy)
    {
        TArray<uint8> Bytes;
        if (!LoadBytes(Path, Bytes))
        {
            OutWhy = FString::Printf(TEXT("could not read '%s'"), *Path);
            return false;
        }
        if (!FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), OutImage))
        {
            OutWhy = FString::Printf(TEXT("FImageUtils::DecompressImage rejected %d bytes"), Bytes.Num());
            return false;
        }
        if (OutImage.Format != ERawImageFormat::BGRA8)
        {
            OutWhy = FString::Printf(TEXT("decoded format is %d, expected BGRA8 (%d)"),
                static_cast<int32>(OutImage.Format), static_cast<int32>(ERawImageFormat::BGRA8));
            return false;
        }
        if (OutImage.RawData.Num() < static_cast<int64>(OutImage.SizeX) * OutImage.SizeY * 4)
        {
            OutWhy = FString::Printf(TEXT("decoded %lld bytes for a %dx%d BGRA8 image"),
                static_cast<int64>(OutImage.RawData.Num()), OutImage.SizeX, OutImage.SizeY);
            return false;
        }
        return true;
    }

    /** A contiguous band of bright rows - one plotted tone, smeared over the rows its band covers. */
    struct FRowRun
    {
        int32 FirstY = 0;
        int32 LastY = 0;
        int32 PeakBrightness = 0;

        double CenterY() const { return 0.5 * (static_cast<double>(FirstY) + static_cast<double>(LastY)); }
    };

    /**
     * Rows of the X0..X1, Y0..Y1 window whose brightest pixel clears the threshold, grouped into runs.
     *
     * The centre of a run, not its brightest row, is the measurement: max-pooling spreads one
     * constant-Q band across several rows, and the argmax inside that plateau jitters while the
     * centre does not.
     *
     * The row window is as load-bearing as the column window: a plot's axis LABELS are drawn in
     * ColorText across the full width of the image, so any scan that walks rows outside the data
     * area reports page furniture as a plotted tone.
     */
    TArray<FRowRun> FindBrightRowRuns(const FImage& Image, int32 X0, int32 X1, int32 Y0, int32 Y1, int32 Threshold)
    {
        TArray<FRowRun> Runs;
        const uint8* Pixels = Image.RawData.GetData();
        const int32 ClampedX0 = FMath::Clamp(X0, 0, Image.SizeX - 1);
        const int32 ClampedX1 = FMath::Clamp(X1, ClampedX0, Image.SizeX - 1);
        const int32 ClampedY0 = FMath::Clamp(Y0, 0, Image.SizeY - 1);
        const int32 ClampedY1 = FMath::Clamp(Y1, ClampedY0, Image.SizeY - 1);

        bool bInRun = false;
        for (int32 Y = ClampedY0; Y <= ClampedY1; ++Y)
        {
            int32 Brightest = 0;
            for (int32 X = ClampedX0; X <= ClampedX1; ++X)
            {
                const int64 Offset = (static_cast<int64>(Y) * Image.SizeX + X) * 4;
                const int32 Brightness =
                    static_cast<int32>(Pixels[Offset]) +        // B
                    static_cast<int32>(Pixels[Offset + 1]) +    // G
                    static_cast<int32>(Pixels[Offset + 2]);     // R
                Brightest = FMath::Max(Brightest, Brightness);
            }

            if (Brightest >= Threshold)
            {
                if (!bInRun)
                {
                    Runs.Add(FRowRun{ Y, Y, Brightest });
                    bInRun = true;
                }
                else
                {
                    Runs.Last().LastY = Y;
                    Runs.Last().PeakBrightness = FMath::Max(Runs.Last().PeakBrightness, Brightest);
                }
            }
            else
            {
                bInRun = false;
            }
        }
        return Runs;
    }

    /** Type-agnostic construction: the sibling chunk owns FPwPitchPointResult's field types. */
    FPwPitchPointResult MakePitchPoint(double TimeMs, double F0Hz, double Confidence)
    {
        FPwPitchPointResult Point;
        Point.TimeMs = static_cast<decltype(Point.TimeMs)>(TimeMs);
        Point.F0Hz = static_cast<decltype(Point.F0Hz)>(F0Hz);
        Point.Confidence = static_cast<decltype(Point.Confidence)>(Confidence);
        return Point;
    }

    /** A 0.5 s two-tone STFT, the base image every overlay test annotates. */
    bool MakeOverlayStft(FPwStftResult& OutResult, FString& OutWhy)
    {
        TArray<float> Samples;
        Samples.SetNumZeroed(TestSampleRate / 2);
        AddSine(Samples, TestSampleRate, 1000.0, 0.5);
        AddSine(Samples, TestSampleRate, 5000.0, 0.25);

        FPwStftSettings Settings;
        Settings.FftSize = 2048;
        Settings.HopSize = 256;

        FPwStftError Error;
        if (!PwComputeStft(Samples, TestSampleRate, Settings, OutResult, &Error))
        {
            OutWhy = FString::Printf(TEXT("%s: %s"), *Error.Code, *Error.Message);
            return false;
        }
        return true;
    }
}

// =========================================================================================
// A. The constant-Q PNG is written at the size that was ASKED FOR, not at the defaults.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwPlotConstantQPngTest,
    "PinWright.audio.plot.ConstantQPngIsWritten",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwPlotConstantQPngTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioPlotViewTestHelpers;

    const double Tones[] = { 440.0 };
    const FPwAudioBuffer Buffer = MakeToneBuffer(TestSampleRate / 2, Tones, 0.5);

    // Non-default dimensions on purpose: the defaults would pass even if Width/Height were ignored.
    FPwConstantQPlotSettings Settings;
    Settings.Width = 900;
    Settings.Height = 480;

    const FString PngPath = ScratchPngPath(TEXT("ConstantQ"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*PngPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
    };

    FString PlotError;
    const bool bPlotted = PwPlotConstantQ(Buffer, PngPath, PlotError, &Settings);
    TestTrue(FString::Printf(TEXT("PwPlotConstantQ succeeded (%s)"), *PlotError), bPlotted);
    if (!bPlotted)
    {
        return false;
    }

    int32 PngWidth = 0;
    int32 PngHeight = 0;
    int64 FileSize = 0;
    TestTrue(TEXT("File is a PNG (signature + IHDR)"), ReadPngHeader(PngPath, PngWidth, PngHeight, FileSize));
    TestEqual(TEXT("IHDR width matches the requested 900"), PngWidth, 900);
    TestEqual(TEXT("IHDR height matches the requested 480"), PngHeight, 480);
    TestTrue(FString::Printf(TEXT("PNG is non-trivial (%lld bytes)"), FileSize), FileSize > 2048);

    return true;
}

// =========================================================================================
// B. THE assertion for this chunk: three tones one octave apart plot at three EVENLY SPACED
//    rows. On a linear frequency axis the same three tones sit at 1:2 spacing, so this is the
//    single test that distinguishes a real constant-Q transform from a relabelled spectrogram.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwPlotConstantQOctaveSpacingTest,
    "PinWright.audio.plot.ConstantQOctavesAreEvenlySpaced",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwPlotConstantQOctaveSpacingTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioPlotViewTestHelpers;

    // A3, A4, A5 - two octave steps, and every one of them is exactly on a band centre for the
    // default 12-bands-per-octave layout anchored at C1, because both are equal temperament.
    const double Tones[] = { 220.0, 440.0, 880.0 };
    const FPwAudioBuffer Buffer = MakeToneBuffer(TestSampleRate / 2, Tones, 0.5);

    const FString PngPath = ScratchPngPath(TEXT("ConstantQOctaves"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*PngPath, false, true, true);
    };

    FString PlotError;
    const bool bPlotted = PwPlotConstantQ(Buffer, PngPath, PlotError);
    TestTrue(FString::Printf(TEXT("PwPlotConstantQ succeeded (%s)"), *PlotError), bPlotted);
    if (!bPlotted)
    {
        return false;
    }

    FImage Image;
    FString DecodeWhy;
    const bool bDecoded = DecodePng(PngPath, Image, DecodeWhy);
    TestTrue(FString::Printf(TEXT("Decoded the written PNG (%s)"), *DecodeWhy), bDecoded);
    if (!bDecoded)
    {
        return false;
    }
    TestEqual(TEXT("Decoded width is the default 1024"), Image.SizeX, 1024);
    TestEqual(TEXT("Decoded height is the default 512"), Image.SizeY, 512);

    // Columns 300..800 and rows 12..467 are the data area of a 1024x512 constant-Q plot (margins
    // left 128, right 108, top 12, bottom 44), so no axis label, plot border or colour-bar pixel is
    // inside the sampled window.
    //
    // BOUNDING THE ROWS IS NOT COSMETIC. The bottom time axis draws its labels at PlotY1 + 10, i.e.
    // rows 477..490 (7-pixel glyphs at TextScale 2), centred under ticks that live inside the plot's
    // own column range - so they land squarely inside columns 300..800. They are ColorText
    // (226,226,236), brightness 688, which is above BOTH the 420 threshold and the 521 of viridis's
    // brightest possible pixel: a row scan over the whole image counts them as a fourth, impossibly
    // bright "tone". Only rows the plotter filled with colormapped data can carry a tone.
    const TArray<FRowRun> Runs = FindBrightRowRuns(Image, 300, 800, 12, 467, PeakBrightnessThreshold);
    TestEqual(FString::Printf(TEXT("Exactly three tones are visible (found %d bright runs)"), Runs.Num()),
        Runs.Num(), 3);
    if (Runs.Num() != 3)
    {
        for (const FRowRun& Run : Runs)
        {
            AddInfo(FString::Printf(TEXT("run rows %d..%d peak %d"), Run.FirstY, Run.LastY, Run.PeakBrightness));
        }
        return false;
    }

    // Runs come out top-down, i.e. highest frequency first, because the frequency axis increases
    // upward. Asserting that ordering is itself the orientation check.
    const double TopCenter = Runs[0].CenterY();
    const double MiddleCenter = Runs[1].CenterY();
    const double BottomCenter = Runs[2].CenterY();
    TestTrue(TEXT("880 Hz is drawn above 440 Hz, which is above 220 Hz"),
        TopCenter < MiddleCenter && MiddleCenter < BottomCenter);

    const double UpperGap = MiddleCenter - TopCenter;     // 880 -> 440
    const double LowerGap = BottomCenter - MiddleCenter;  // 440 -> 220
    AddInfo(FString::Printf(TEXT("octave gaps: %.2f px (880->440) and %.2f px (440->220)"), UpperGap, LowerGap));

    TestTrue(FString::Printf(TEXT("Both octaves are a real distance apart (%.2f px, %.2f px)"), UpperGap, LowerGap),
        UpperGap > 30.0 && LowerGap > 30.0);

    // The property that makes this constant-Q. A linear axis would put the 880->440 gap at roughly
    // twice the 440->220 gap; here they must agree to within the row quantisation of the max-pool.
    TestTrue(FString::Printf(
        TEXT("Equal octaves occupy equal pixels: %.2f px vs %.2f px (difference %.2f, tolerance 5)"),
        UpperGap, LowerGap, FMath::Abs(UpperGap - LowerGap)),
        FMath::Abs(UpperGap - LowerGap) <= 5.0);

    // And the two-octave span is twice one octave, which a 1:2 linear layout could not satisfy at
    // the same time as the equality above.
    const double TwoOctaveSpan = BottomCenter - TopCenter;
    TestTrue(FString::Printf(TEXT("Two octaves span %.2f px, twice the %.2f px single octave"),
        TwoOctaveSpan, UpperGap),
        FMath::Abs(TwoOctaveSpan - 2.0 * UpperGap) <= 6.0);

    return true;
}

// =========================================================================================
// C. An EMPTY overlay is byte-identical to the plain spectrogram. This is what lets a caller
//    always take the overlay entry point, and it only holds because both share one renderer.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwPlotEmptyOverlayIsIdenticalTest,
    "PinWright.audio.plot.EmptyOverlayMatchesPlainSpectrogram",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwPlotEmptyOverlayIsIdenticalTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioPlotViewTestHelpers;

    FPwStftResult Stft;
    FString StftWhy;
    const bool bStft = MakeOverlayStft(Stft, StftWhy);
    TestTrue(FString::Printf(TEXT("PwComputeStft succeeded (%s)"), *StftWhy), bStft);
    if (!bStft)
    {
        return false;
    }

    const FString PlainPath = ScratchPngPath(TEXT("OverlayBasePlain"));
    const FString OverlaidPath = ScratchPngPath(TEXT("OverlayBaseEmpty"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*PlainPath, false, true, true);
        IFileManager::Get().Delete(*OverlaidPath, false, true, true);
    };

    const FPwPlotOverlay EmptyOverlay;
    TestTrue(TEXT("A default-constructed overlay reports itself empty"), EmptyOverlay.IsEmpty());

    FString PlainError;
    FString OverlaidError;
    const bool bPlain = PwPlotSpectrogram(Stft, TestSampleRate, /*bLogFrequency*/ true, PlainPath, PlainError);
    const bool bOverlaid = PwPlotSpectrogramWithOverlay(Stft, TestSampleRate, /*bLogFrequency*/ true,
        EmptyOverlay, OverlaidPath, OverlaidError);
    TestTrue(FString::Printf(TEXT("Plain spectrogram succeeded (%s)"), *PlainError), bPlain);
    TestTrue(FString::Printf(TEXT("Empty-overlay spectrogram succeeded (%s)"), *OverlaidError), bOverlaid);
    if (!bPlain || !bOverlaid)
    {
        return false;
    }

    int32 Width = 0;
    int32 Height = 0;
    int64 FileSize = 0;
    TestTrue(TEXT("Overlay output is a PNG (signature + IHDR)"),
        ReadPngHeader(OverlaidPath, Width, Height, FileSize));
    TestEqual(TEXT("IHDR width matches the default 1024"), Width, 1024);
    TestEqual(TEXT("IHDR height matches the default 512"), Height, 512);
    TestTrue(FString::Printf(TEXT("Overlay PNG is non-trivial (%lld bytes)"), FileSize), FileSize > 2048);

    TArray<uint8> PlainBytes;
    TArray<uint8> OverlaidBytes;
    const bool bReadPlain = LoadBytes(PlainPath, PlainBytes);
    const bool bReadOverlaid = LoadBytes(OverlaidPath, OverlaidBytes);
    TestTrue(TEXT("Read both PNGs back"), bReadPlain && bReadOverlaid);
    if (!bReadPlain || !bReadOverlaid)
    {
        return false;
    }
    TestTrue(FString::Printf(TEXT("An empty overlay is byte-identical to the plain plot (%d vs %d bytes)"),
        PlainBytes.Num(), OverlaidBytes.Num()), PlainBytes == OverlaidBytes);

    return true;
}

// =========================================================================================
// D. Onsets that land on the time axis change the image; onsets that do not are dropped, and
//    dropping them has to leave the image untouched rather than nudging it.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwPlotOverlayOnsetsTest,
    "PinWright.audio.plot.OverlayOnsetsChangeTheImage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwPlotOverlayOnsetsTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioPlotViewTestHelpers;

    FPwStftResult Stft;
    FString StftWhy;
    const bool bStft = MakeOverlayStft(Stft, StftWhy);
    TestTrue(FString::Printf(TEXT("PwComputeStft succeeded (%s)"), *StftWhy), bStft);
    if (!bStft)
    {
        return false;
    }

    const FString PlainPath = ScratchPngPath(TEXT("OnsetsNone"));
    const FString OnsetPath = ScratchPngPath(TEXT("OnsetsDrawn"));
    const FString OffAxisPath = ScratchPngPath(TEXT("OnsetsOffAxis"));
    const FString EventPath = ScratchPngPath(TEXT("EventBrackets"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*PlainPath, false, true, true);
        IFileManager::Get().Delete(*OnsetPath, false, true, true);
        IFileManager::Get().Delete(*OffAxisPath, false, true, true);
        IFileManager::Get().Delete(*EventPath, false, true, true);
    };

    FPwPlotOverlay WithOnsets;
    WithOnsets.OnsetTimesMs = { 100.0, 200.0, 300.0 };

    // Both outside 0..~459 ms, so both have to be dropped rather than clamped onto the edge.
    FPwPlotOverlay OffAxisOnsets;
    OffAxisOnsets.OnsetTimesMs = { -50.0, 5000.0 };

    FPwPlotOverlay WithEvent;
    WithEvent.EventBoundsMs = { FVector2D(120.0, 260.0) };

    FString PlainError;
    FString OnsetError;
    FString OffAxisError;
    FString EventError;
    const bool bPlain = PwPlotSpectrogram(Stft, TestSampleRate, true, PlainPath, PlainError);
    const bool bOnsets = PwPlotSpectrogramWithOverlay(Stft, TestSampleRate, true, WithOnsets, OnsetPath, OnsetError);
    const bool bOffAxis = PwPlotSpectrogramWithOverlay(Stft, TestSampleRate, true, OffAxisOnsets, OffAxisPath, OffAxisError);
    const bool bEvent = PwPlotSpectrogramWithOverlay(Stft, TestSampleRate, true, WithEvent, EventPath, EventError);
    TestTrue(FString::Printf(TEXT("Plain plot succeeded (%s)"), *PlainError), bPlain);
    TestTrue(FString::Printf(TEXT("Onset overlay succeeded (%s)"), *OnsetError), bOnsets);
    TestTrue(FString::Printf(TEXT("Off-axis onset overlay succeeded (%s)"), *OffAxisError), bOffAxis);
    TestTrue(FString::Printf(TEXT("Event overlay succeeded (%s)"), *EventError), bEvent);
    if (!bPlain || !bOnsets || !bOffAxis || !bEvent)
    {
        return false;
    }

    TArray<uint8> PlainBytes;
    TArray<uint8> OnsetBytes;
    TArray<uint8> OffAxisBytes;
    TArray<uint8> EventBytes;
    const bool bReadPlain = LoadBytes(PlainPath, PlainBytes);
    const bool bReadOnset = LoadBytes(OnsetPath, OnsetBytes);
    const bool bReadOffAxis = LoadBytes(OffAxisPath, OffAxisBytes);
    const bool bReadEvent = LoadBytes(EventPath, EventBytes);
    const bool bRead = bReadPlain && bReadOnset && bReadOffAxis && bReadEvent;
    TestTrue(TEXT("Read all four PNGs back"), bRead);
    if (!bRead)
    {
        return false;
    }

    TestTrue(TEXT("Drawn onsets change the image"), PlainBytes != OnsetBytes);
    TestTrue(TEXT("Event brackets change the image"), PlainBytes != EventBytes);
    TestTrue(TEXT("Onsets and event brackets are different marks"), OnsetBytes != EventBytes);

    // The complement of the first assertion: if out-of-range onsets were clamped to the plot edge
    // instead of dropped, this image would differ too, and the difference above would prove nothing
    // about WHERE the lines went.
    TestTrue(TEXT("Onsets outside the time axis are dropped, leaving the plain image"),
        PlainBytes == OffAxisBytes);

    // The onset colour must be one viridis cannot make. Decoding is the only way to assert that
    // about the actual output rather than about the constant in the source.
    FImage Image;
    FString DecodeWhy;
    const bool bDecodedOnsetPng = DecodePng(OnsetPath, Image, DecodeWhy);
    TestTrue(FString::Printf(TEXT("Decoded the onset PNG (%s)"), *DecodeWhy), bDecodedOnsetPng);
    if (bDecodedOnsetPng)
    {
        int32 RedDominantPixels = 0;
        const uint8* Pixels = Image.RawData.GetData();
        for (int64 Index = 0; Index < static_cast<int64>(Image.SizeX) * Image.SizeY; ++Index)
        {
            const int32 Blue = Pixels[Index * 4];
            const int32 Green = Pixels[Index * 4 + 1];
            const int32 Red = Pixels[Index * 4 + 2];
            // Viridis never produces red this far ahead of green; the ramp's warmest point has
            // green as high as red.
            if (Red > 200 && Green < 100 && Blue < 100)
            {
                ++RedDominantPixels;
            }
        }
        TestTrue(FString::Printf(TEXT("Onset lines are drawn in a colour viridis cannot produce (%d pixels)"),
            RedDominantPixels), RedDominantPixels > 100);
    }

    return true;
}

// =========================================================================================
// E. Low-confidence pitch points are excluded. Three images make this airtight: adding the
//    low-confidence points must change NOTHING, and the same points at high confidence must
//    change something - otherwise "excluded" could just mean "the curve is never drawn".
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwPlotOverlayPitchConfidenceTest,
    "PinWright.audio.plot.OverlayExcludesLowConfidencePitch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwPlotOverlayPitchConfidenceTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioPlotViewTestHelpers;

    FPwStftResult Stft;
    FString StftWhy;
    const bool bStft = MakeOverlayStft(Stft, StftWhy);
    TestTrue(FString::Printf(TEXT("PwComputeStft succeeded (%s)"), *StftWhy), bStft);
    if (!bStft)
    {
        return false;
    }

    // Confident run first, then a tail. Keeping the tail at the END of the array means dropping it
    // cannot change which of the confident points are adjacent, so the only difference between the
    // first two images can be the tail itself.
    TArray<FPwPitchPointResult> ConfidentRun;
    for (int32 Step = 0; Step < 5; ++Step)
    {
        ConfidentRun.Add(MakePitchPoint(50.0 + 10.0 * Step, 1000.0, 0.9));
    }

    FPwPlotOverlay ConfidentOnly;
    ConfidentOnly.PitchTrack = ConfidentRun;

    FPwPlotOverlay WithLowConfidenceTail;
    WithLowConfidenceTail.PitchTrack = ConfidentRun;
    FPwPlotOverlay WithConfidentTail;
    WithConfidentTail.PitchTrack = ConfidentRun;
    for (int32 Step = 0; Step < 3; ++Step)
    {
        const double TimeMs = 200.0 + 10.0 * Step;
        // Same times, same frequencies - only the confidence differs between the two tails, so a
        // difference in the images can only come from the confidence floor.
        // 1500 Hz keeps the fabricated track inside the [50, 2000] band PwEstimatePitch can report,
        // so this exercises the plotter on data the producer could actually hand it.
        WithLowConfidenceTail.PitchTrack.Add(MakePitchPoint(TimeMs, 1500.0, 0.2));
        WithConfidentTail.PitchTrack.Add(MakePitchPoint(TimeMs, 1500.0, 0.9));
    }

    // The plot must not pick its own floor: a curve showing points PwEstimatePitch's own aggregate
    // discarded would put two different definitions of "confident" in front of the same reader.
    TestEqual(TEXT("The overlay floor is PwEstimatePitch's own PwPitchConfidenceFloor"),
        ConfidentOnly.MinPitchConfidence, PwPitchConfidenceFloor);

    const FString ConfidentPath = ScratchPngPath(TEXT("PitchConfident"));
    const FString LowTailPath = ScratchPngPath(TEXT("PitchLowTail"));
    const FString HighTailPath = ScratchPngPath(TEXT("PitchHighTail"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*ConfidentPath, false, true, true);
        IFileManager::Get().Delete(*LowTailPath, false, true, true);
        IFileManager::Get().Delete(*HighTailPath, false, true, true);
    };

    FString ConfidentError;
    FString LowTailError;
    FString HighTailError;
    const bool bConfident = PwPlotSpectrogramWithOverlay(Stft, TestSampleRate, true, ConfidentOnly,
        ConfidentPath, ConfidentError);
    const bool bLowTail = PwPlotSpectrogramWithOverlay(Stft, TestSampleRate, true, WithLowConfidenceTail,
        LowTailPath, LowTailError);
    const bool bHighTail = PwPlotSpectrogramWithOverlay(Stft, TestSampleRate, true, WithConfidentTail,
        HighTailPath, HighTailError);
    TestTrue(FString::Printf(TEXT("Confident-only overlay succeeded (%s)"), *ConfidentError), bConfident);
    TestTrue(FString::Printf(TEXT("Low-confidence-tail overlay succeeded (%s)"), *LowTailError), bLowTail);
    TestTrue(FString::Printf(TEXT("Confident-tail overlay succeeded (%s)"), *HighTailError), bHighTail);
    if (!bConfident || !bLowTail || !bHighTail)
    {
        return false;
    }

    TArray<uint8> ConfidentBytes;
    TArray<uint8> LowTailBytes;
    TArray<uint8> HighTailBytes;
    const bool bReadConfident = LoadBytes(ConfidentPath, ConfidentBytes);
    const bool bReadLowTail = LoadBytes(LowTailPath, LowTailBytes);
    const bool bReadHighTail = LoadBytes(HighTailPath, HighTailBytes);
    const bool bRead = bReadConfident && bReadLowTail && bReadHighTail;
    TestTrue(TEXT("Read all three PNGs back"), bRead);
    if (!bRead)
    {
        return false;
    }

    TestTrue(TEXT("Points below the confidence floor are not drawn at all"),
        ConfidentBytes == LowTailBytes);
    TestTrue(TEXT("...and the very same points above the floor ARE drawn"),
        HighTailBytes != LowTailBytes);

    return true;
}

// =========================================================================================
// F. §12, the failure direction. Every rejection returns false, explains itself, and leaves no
//    file - not even a partial one - behind.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwPlotViewsRejectBadInputTest,
    "PinWright.audio.plot.ViewsRejectUnplottableInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwPlotViewsRejectBadInputTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioPlotViewTestHelpers;

    const double Tones[] = { 440.0 };
    const FPwAudioBuffer Buffer = MakeToneBuffer(TestSampleRate / 2, Tones, 0.5);

    FPwStftResult Stft;
    FString StftWhy;
    const bool bStft = MakeOverlayStft(Stft, StftWhy);
    TestTrue(FString::Printf(TEXT("PwComputeStft succeeded (%s)"), *StftWhy), bStft);

    const FString PngPath = ScratchPngPath(TEXT("ShouldNotExist"));
    // A real file standing where a directory would have to be. Both the MakeDirectory and the
    // write then fail, which is the closest thing to a reliably unwritable absolute path.
    const FString BlockerPath = ScratchPngPath(TEXT("Blocker"));
    const FString BlockedPath = FPaths::Combine(BlockerPath, TEXT("Inner.png"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*PngPath, false, true, true);
        IFileManager::Get().Delete(*BlockedPath, false, true, true);
        IFileManager::Get().Delete(*BlockerPath, false, true, true);
    };
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(BlockerPath), /*Tree*/ true);
    TestTrue(TEXT("Wrote the blocker file that stands in for a directory"),
        FFileHelper::SaveStringToFile(TEXT("not a directory"), *BlockerPath));

    const FPwPlotOverlay EmptyOverlay;

    // ---- Constant-Q ----
    {
        FString Error;
        TestFalse(TEXT("Constant-Q refuses a relative output path"),
            PwPlotConstantQ(Buffer, TEXT("PwPlotConstantQRelative.png"), Error));
        TestFalse(TEXT("...and the refusal is explained"), Error.IsEmpty());
        TestFalse(TEXT("...and nothing was written next to the working directory"),
            IFileManager::Get().FileExists(TEXT("PwPlotConstantQRelative.png")));
    }
    {
        FString Error;
        TestFalse(TEXT("Constant-Q refuses an unwritable path"),
            PwPlotConstantQ(Buffer, BlockedPath, Error));
        TestFalse(TEXT("...and the refusal is explained"), Error.IsEmpty());
        TestFalse(TEXT("...and no partial file is left behind"), IFileManager::Get().FileExists(*BlockedPath));
    }
    {
        FPwAudioBuffer EmptyBuffer;
        EmptyBuffer.SampleRate = TestSampleRate;
        FString Error;
        TestFalse(TEXT("Constant-Q refuses an empty buffer"), PwPlotConstantQ(EmptyBuffer, PngPath, Error));
        TestFalse(TEXT("...and the refusal is explained"), Error.IsEmpty());
        TestFalse(TEXT("...and no file was created"), IFileManager::Get().FileExists(*PngPath));
    }
    {
        // A band layout that cannot describe an axis. Rejected here rather than passed to
        // Audio::FPseudoConstantQ, which check()s its arguments and would assert.
        FPwConstantQPlotSettings Settings;
        Settings.NumBands = 0;
        FString Error;
        TestFalse(TEXT("Constant-Q refuses a zero-band layout"), PwPlotConstantQ(Buffer, PngPath, Error, &Settings));
        TestFalse(TEXT("...and the refusal is explained"), Error.IsEmpty());
        TestFalse(TEXT("...and no file was created"), IFileManager::Get().FileExists(*PngPath));

        Settings = FPwConstantQPlotSettings();
        Settings.NumBandsPerOctave = 0.f;
        Error.Reset();
        TestFalse(TEXT("Constant-Q refuses zero bands per octave"), PwPlotConstantQ(Buffer, PngPath, Error, &Settings));
        TestFalse(TEXT("...and the refusal is explained"), Error.IsEmpty());

        Settings = FPwConstantQPlotSettings();
        Settings.LowestBandCenterHz = 0.f;
        Error.Reset();
        TestFalse(TEXT("Constant-Q refuses a zero base frequency"), PwPlotConstantQ(Buffer, PngPath, Error, &Settings));
        TestFalse(TEXT("...and the refusal is explained"), Error.IsEmpty());
        TestFalse(TEXT("...and no file was created by any of them"), IFileManager::Get().FileExists(*PngPath));
    }

    // ---- Overlay ----
    {
        const FPwStftResult InvalidStft;
        FString Error;
        TestFalse(TEXT("The overlay plot refuses an empty STFT result"),
            PwPlotSpectrogramWithOverlay(InvalidStft, TestSampleRate, true, EmptyOverlay, PngPath, Error));
        TestFalse(TEXT("...and the refusal is explained"), Error.IsEmpty());
        TestFalse(TEXT("...and no file was created"), IFileManager::Get().FileExists(*PngPath));
    }
    if (bStft)
    {
        FString Error;
        TestFalse(TEXT("The overlay plot refuses a relative output path"),
            PwPlotSpectrogramWithOverlay(Stft, TestSampleRate, true, EmptyOverlay,
                TEXT("PwPlotOverlayRelative.png"), Error));
        TestFalse(TEXT("...and the refusal is explained"), Error.IsEmpty());
        TestFalse(TEXT("...and nothing was written next to the working directory"),
            IFileManager::Get().FileExists(TEXT("PwPlotOverlayRelative.png")));

        Error.Reset();
        TestFalse(TEXT("The overlay plot refuses an unwritable path"),
            PwPlotSpectrogramWithOverlay(Stft, TestSampleRate, true, EmptyOverlay, BlockedPath, Error));
        TestFalse(TEXT("...and the refusal is explained"), Error.IsEmpty());
        TestFalse(TEXT("...and no partial file is left behind"), IFileManager::Get().FileExists(*BlockedPath));

        // A non-populated overlay is not an excuse to skip the input checks: a zero sample rate has
        // to fail here exactly as it does on the plain spectrogram.
        Error.Reset();
        TestFalse(TEXT("The overlay plot refuses a non-positive sample rate"),
            PwPlotSpectrogramWithOverlay(Stft, 0, true, EmptyOverlay, PngPath, Error));
        TestFalse(TEXT("...and the refusal is explained"), Error.IsEmpty());
        TestFalse(TEXT("...and no file was created"), IFileManager::Get().FileExists(*PngPath));
    }

    return true;
}
