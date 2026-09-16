// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the offline spectral front-end (AudioGen/PwStft.h) and the PNG plotters
// (AudioGen/PwAudioPlot.h).
//
// This chunk has real ground truth, so the assertions are numeric rather than "it returned true":
// a synthesised sine has a known bin, a known level under the documented scaling convention, and a
// known leakage floor, and the frame count has a closed form. Per rpc-design.md §12 the failure
// direction is asserted too - every rejection path is checked for the SPECIFIC error code, because
// the defect this guards against is a silence being reported as a generic FFT failure.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioPlot.h"
#include "AudioGen/PwStft.h"
#include "Handlers/ErrorCodes.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true.
namespace PwStftTestHelpers
{
    constexpr int32 TestSampleRate = 48000;

    /** Adds Amplitude * sin(2*pi*Hz*t) into Out, which must already be sized. */
    void AddSine(TArray<float>& Out, int32 SampleRate, double Hz, double Amplitude)
    {
        const double AngularStep = 2.0 * UE_DOUBLE_PI * Hz / static_cast<double>(SampleRate);
        for (int32 Index = 0; Index < Out.Num(); ++Index)
        {
            Out[Index] += static_cast<float>(Amplitude * FMath::Sin(AngularStep * Index));
        }
    }

    TArray<float> MakeSine(int32 NumSamples, int32 SampleRate, double Hz, double Amplitude)
    {
        TArray<float> Samples;
        Samples.SetNumZeroed(NumSamples);
        AddSine(Samples, SampleRate, Hz, Amplitude);
        return Samples;
    }

    int32 FindPeakBin(const FPwStftResult& Result, int32 FrameIndex)
    {
        int32 BestBin = INDEX_NONE;
        float BestMagnitude = -1.f;
        for (int32 Bin = 0; Bin < Result.NumBins; ++Bin)
        {
            const float Magnitude = PwStftMagnitudeAt(Result, FrameIndex, Bin);
            if (Magnitude > BestMagnitude)
            {
                BestMagnitude = Magnitude;
                BestBin = Bin;
            }
        }
        return BestBin;
    }

    /** Largest magnitude at least ExclusionRadius bins away from every entry of CenterBins. */
    float MaxMagnitudeAwayFrom(const FPwStftResult& Result, int32 FrameIndex,
        const TArray<int32>& CenterBins, int32 ExclusionRadius)
    {
        float Worst = 0.f;
        for (int32 Bin = 0; Bin < Result.NumBins; ++Bin)
        {
            bool bNearACenter = false;
            for (const int32 Center : CenterBins)
            {
                if (FMath::Abs(Bin - Center) < ExclusionRadius)
                {
                    bNearACenter = true;
                    break;
                }
            }
            if (!bNearACenter)
            {
                Worst = FMath::Max(Worst, PwStftMagnitudeAt(Result, FrameIndex, Bin));
            }
        }
        return Worst;
    }

    /**
     * Absolute scratch path under the automation transient dir. ConvertRelativePathToFull is not
     * cosmetic: AutomationTransientDir() is project-relative and the plotters reject a relative
     * path outright rather than resolving it against the editor's working directory.
     */
    FString ScratchPngPath(const TCHAR* Stem)
    {
        const FString FileName = FString::Printf(TEXT("%s_%s.png"), Stem,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        return FPaths::ConvertRelativePathToFull(
            FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("PinWrightAudioPlots"), FileName));
    }

    /**
     * Reads the PNG signature and IHDR. Returns false unless the file starts with the 8-byte PNG
     * signature (89 50 4E 47 0D 0A 1A 0A) followed by an IHDR chunk. Checking the IHDR dimensions
     * as well as the magic is what makes this a real "it is a PNG of the right size" assertion
     * rather than "the first four bytes happen to match".
     */
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
}

// =========================================================================================
// A. A 1 kHz sine lands in the bin nearest 1 kHz, at the level the documented scaling
//    convention predicts, well clear of the leakage floor.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwStftSineLandsInExpectedBinTest,
    "PinWright.audio.stft.SineLandsInExpectedBin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwStftSineLandsInExpectedBinTest::RunTest(const FString& Parameters)
{
    using namespace PwStftTestHelpers;

    const TArray<float> Samples = MakeSine(TestSampleRate, TestSampleRate, 1000.0, 1.0);

    FPwStftSettings Settings;
    Settings.FftSize = 2048;
    Settings.HopSize = 256;

    FPwStftResult Result;
    FPwStftError Error;
    const bool bSucceeded = PwComputeStft(Samples, TestSampleRate, Settings, Result, &Error);
    TestTrue(FString::Printf(TEXT("PwComputeStft succeeded (%s: %s)"), *Error.Code, *Error.Message), bSucceeded);
    if (!bSucceeded)
    {
        return false;
    }

    TestEqual(TEXT("NumBins == FftSize/2 + 1"), Result.NumBins, 1025);
    TestEqual(TEXT("FftSize round-trips"), Result.FftSize, 2048);
    TestEqual(TEXT("HopSize round-trips"), Result.HopSize, 256);
    TestTrue(TEXT("BinHz == SampleRate/FftSize == 23.4375"),
        FMath::IsNearlyEqual(Result.BinHz, 23.4375f, 1e-4f));

    // 1000 / 23.4375 = 42.667, so bin 43 (1007.81 Hz) is nearer than bin 42 (984.38 Hz).
    const int32 ExpectedBin = FMath::RoundToInt(1000.0f / Result.BinHz);
    TestEqual(TEXT("Nearest bin to 1000 Hz is 43"), ExpectedBin, 43);

    // Middle frame: fully inside the signal, no edge effects to argue about.
    const int32 FrameIndex = Result.NumFrames / 2;
    TestEqual(TEXT("Magnitude peak sits in the bin nearest 1000 Hz"),
        FindPeakBin(Result, FrameIndex), ExpectedBin);

    // Level. Under the documented convention a full-scale sine on a bin CENTRE reads 1.0 (0 dBFS);
    // this tone sits 0.333 bins off centre, where the periodic Hann response is
    // |sinc(0.333) / (1 - 0.333^2)| = 0.9304, i.e. -0.627 dBFS of scalloping loss. The window
    // brackets that prediction tightly enough that any of the plausible scaling bugs fails them:
    // a missing single-sided x2 lands at -6.6 dB, a missing sqrt(FftSize) undo at +30 dB, and an
    // un-normalized window at roughly +60 dB.
    const float PeakMagnitude = PwStftMagnitudeAt(Result, FrameIndex, ExpectedBin);
    const float PeakDb = PwMagnitudeToDb(PeakMagnitude);
    TestTrue(FString::Printf(TEXT("Peak is %.4f (%.3f dBFS), expected ~0.9304 (-0.627 dBFS)"),
        PeakMagnitude, PeakDb), PeakDb > -1.5f && PeakDb < 0.2f);

    // Separation from the leakage floor, measured at least 16 bins out where the periodic Hann
    // kernel is already below -80 dB.
    const TArray<int32> Centers = { ExpectedBin };
    const float FloorMagnitude = MaxMagnitudeAwayFrom(Result, FrameIndex, Centers, 16);
    const float FloorDb = PwMagnitudeToDb(FloorMagnitude, -200.f);
    TestTrue(FString::Printf(TEXT("Peak %.3f dBFS is >= 40 dB above the %.3f dBFS floor (>=16 bins out)"),
        PeakDb, FloorDb), (PeakDb - FloorDb) >= 40.f);

    return true;
}

// =========================================================================================
// B. Two well-separated tones resolve as two peaks, at the right bins AND the right relative
//    level. The 6 dB amplitude split is the part a caller-visible scaling regression breaks.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwStftTwoTonesResolveTest,
    "PinWright.audio.stft.TwoTonesResolveTwoPeaks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwStftTwoTonesResolveTest::RunTest(const FString& Parameters)
{
    using namespace PwStftTestHelpers;

    TArray<float> Samples;
    Samples.SetNumZeroed(TestSampleRate / 2);
    AddSine(Samples, TestSampleRate, 1000.0, 0.5);
    AddSine(Samples, TestSampleRate, 5000.0, 0.25);

    FPwStftSettings Settings;
    Settings.FftSize = 2048;
    Settings.HopSize = 512;

    FPwStftResult Result;
    FPwStftError Error;
    const bool bSucceeded = PwComputeStft(Samples, TestSampleRate, Settings, Result, &Error);
    TestTrue(FString::Printf(TEXT("PwComputeStft succeeded (%s: %s)"), *Error.Code, *Error.Message), bSucceeded);
    if (!bSucceeded)
    {
        return false;
    }

    const int32 FrameIndex = Result.NumFrames / 2;
    const int32 LowBin = FMath::RoundToInt(1000.0f / Result.BinHz);    // 43
    const int32 HighBin = FMath::RoundToInt(5000.0f / Result.BinHz);   // 213
    TestEqual(TEXT("1 kHz maps to bin 43"), LowBin, 43);
    TestEqual(TEXT("5 kHz maps to bin 213"), HighBin, 213);

    const float LowDb = PwMagnitudeToDb(PwStftMagnitudeAt(Result, FrameIndex, LowBin));
    const float HighDb = PwMagnitudeToDb(PwStftMagnitudeAt(Result, FrameIndex, HighBin));

    // Each tone must be the local maximum of its own neighbourhood, i.e. two distinct peaks and
    // not one smeared blob.
    for (int32 Offset = -3; Offset <= 3; ++Offset)
    {
        if (Offset == 0)
        {
            continue;
        }
        TestTrue(FString::Printf(TEXT("Bin %d is a local max (offset %d)"), LowBin, Offset),
            PwStftMagnitudeAt(Result, FrameIndex, LowBin) >= PwStftMagnitudeAt(Result, FrameIndex, LowBin + Offset));
        TestTrue(FString::Printf(TEXT("Bin %d is a local max (offset %d)"), HighBin, Offset),
            PwStftMagnitudeAt(Result, FrameIndex, HighBin) >= PwStftMagnitudeAt(Result, FrameIndex, HighBin + Offset));
    }

    // Amplitudes 0.5 and 0.25 are exactly 6.02 dB apart, and the same 0.333-bin scalloping loss
    // applies to both, so the difference survives it unchanged.
    TestTrue(FString::Printf(TEXT("1 kHz (%.3f dBFS) sits 6.02 dB above 5 kHz (%.3f dBFS)"), LowDb, HighDb),
        FMath::Abs((LowDb - HighDb) - 6.02f) < 0.5f);

    // Both peaks clear the valley between them by a wide margin.
    const TArray<int32> Centers = { LowBin, HighBin };
    const float FloorDb = PwMagnitudeToDb(MaxMagnitudeAwayFrom(Result, FrameIndex, Centers, 16), -200.f);
    TestTrue(FString::Printf(TEXT("Quieter peak %.3f dBFS is >= 40 dB above the %.3f dBFS floor"), HighDb, FloorDb),
        (HighDb - FloorDb) >= 40.f);

    return true;
}

// =========================================================================================
// C. Frame count is exactly 1 + (NumSamples - FftSize) / HopSize, and the magnitude matrix is
//    sized to match it.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwStftFrameCountTest,
    "PinWright.audio.stft.FrameCountMatchesFormula",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwStftFrameCountTest::RunTest(const FString& Parameters)
{
    using namespace PwStftTestHelpers;

    struct FCase
    {
        int32 NumSamples;
        int32 FftSize;
        int32 HopSize;
    };
    const FCase Cases[] =
    {
        { 48000, 2048, 256 },   // 1 + 45952/256 = 180 (the division truncates: 179.5 -> 179)
        {  4096, 1024, 512 },   // 1 + 3072/512   = 7
        {  2048, 2048, 256 },   // exactly one window fits
        {  8192,   64,  64 },   // hop == window, no overlap
    };

    for (const FCase& Case : Cases)
    {
        const TArray<float> Samples = MakeSine(Case.NumSamples, TestSampleRate, 440.0, 0.5);

        FPwStftSettings Settings;
        Settings.FftSize = Case.FftSize;
        Settings.HopSize = Case.HopSize;

        FPwStftResult Result;
        FPwStftError Error;
        const bool bSucceeded = PwComputeStft(Samples, TestSampleRate, Settings, Result, &Error);
        const FString Label = FString::Printf(TEXT("n=%d fft=%d hop=%d"),
            Case.NumSamples, Case.FftSize, Case.HopSize);
        TestTrue(FString::Printf(TEXT("%s succeeded (%s: %s)"), *Label, *Error.Code, *Error.Message), bSucceeded);
        if (!bSucceeded)
        {
            continue;
        }

        const int32 Expected = 1 + (Case.NumSamples - Case.FftSize) / Case.HopSize;
        TestEqual(FString::Printf(TEXT("%s: NumFrames"), *Label), Result.NumFrames, Expected);
        TestEqual(FString::Printf(TEXT("%s: PwStftFrameCount agrees"), *Label),
            PwStftFrameCount(Case.NumSamples, Case.FftSize, Case.HopSize), Expected);
        TestEqual(FString::Printf(TEXT("%s: NumBins"), *Label), Result.NumBins, Case.FftSize / 2 + 1);
        TestEqual(FString::Printf(TEXT("%s: magnitude matrix is frame-major NumFrames*NumBins"), *Label),
            Result.Magnitudes.Num(), Result.NumFrames * Result.NumBins);
    }

    // A signal shorter than one window yields no whole frame at all.
    TestEqual(TEXT("PwStftFrameCount(2047, 2048, 256) == 0"), PwStftFrameCount(2047, 2048, 256), 0);

    return true;
}

// =========================================================================================
// D. Failure directions. Each rejection is checked for its SPECIFIC code, and for leaving Out
//    empty - a caller that ignores the bool must not find a half-filled result.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwStftNonPowerOfTwoRejectedTest,
    "PinWright.audio.stft.NonPowerOfTwoFftSizeIsRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwStftNonPowerOfTwoRejectedTest::RunTest(const FString& Parameters)
{
    using namespace PwStftTestHelpers;

    const TArray<float> Samples = MakeSine(8192, TestSampleRate, 1000.0, 0.8);

    FPwStftSettings Settings;
    Settings.FftSize = 1000;   // valid-looking, and NOT a power of two
    Settings.HopSize = 256;

    FPwStftResult Result;
    FPwStftError Error;
    const bool bSucceeded = PwComputeStft(Samples, TestSampleRate, Settings, Result, &Error);

    TestFalse(TEXT("A non-power-of-two FftSize is rejected, not rounded"), bSucceeded);
    TestEqual(TEXT("Code is STFT_FAILED"), Error.Code, FString(ErrorCodes::ERR_STFT_FAILED));
    TestFalse(TEXT("Failure carries a message"), Error.Message.IsEmpty());
    TestEqual(TEXT("Out.NumFrames stays 0"), Result.NumFrames, 0);
    TestEqual(TEXT("Out.Magnitudes stays empty"), Result.Magnitudes.Num(), 0);
    TestFalse(TEXT("Out.IsValid() is false"), Result.IsValid());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwStftEmptyBufferRejectedTest,
    "PinWright.audio.stft.EmptyBufferReportsEmptyBufferCode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwStftEmptyBufferRejectedTest::RunTest(const FString& Parameters)
{
    FPwStftSettings Settings;
    FPwStftResult Result;
    FPwStftError Error;

    const bool bSucceeded = PwComputeStft(TArrayView<const float>(), 48000, Settings, Result, &Error);

    TestFalse(TEXT("An empty buffer is rejected"), bSucceeded);
    TestEqual(TEXT("Code is AUDIO_EMPTY_BUFFER, not a generic failure"),
        Error.Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
    TestNotEqual(TEXT("Code is specifically NOT STFT_FAILED"),
        Error.Code, FString(ErrorCodes::ERR_STFT_FAILED));
    TestEqual(TEXT("Out.NumFrames stays 0"), Result.NumFrames, 0);

    return true;
}

// The rpc-design.md §7 ordering test: this buffer is silent AND too short AND asked for a
// non-power-of-two FFT. All three checks match, and the answer must be the existence one - a
// caller told "the FFT failed" would go and change the FFT size on audio that has no signal in it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwStftSilenceOutranksSizeErrorsTest,
    "PinWright.audio.stft.SilenceIsNamedBeforeSizeErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwStftSilenceOutranksSizeErrorsTest::RunTest(const FString& Parameters)
{
    TArray<float> Silence;
    Silence.SetNumZeroed(100);   // shorter than any window

    FPwStftSettings Settings;
    Settings.FftSize = 1000;     // and not a power of two
    Settings.HopSize = 256;

    FPwStftResult Result;
    FPwStftError Error;
    const bool bSucceeded = PwComputeStft(Silence, 48000, Settings, Result, &Error);

    TestFalse(TEXT("All-silent input is rejected"), bSucceeded);
    TestEqual(TEXT("Silence is named first, ahead of the size errors that also match"),
        Error.Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));

    // And a long, correctly-sized silent buffer is still silence rather than a floor-level result.
    TArray<float> LongSilence;
    LongSilence.SetNumZeroed(8192);
    FPwStftSettings ValidSettings;
    FPwStftResult SecondResult;
    FPwStftError SecondError;
    TestFalse(TEXT("A well-formed but silent buffer is still rejected"),
        PwComputeStft(LongSilence, 48000, ValidSettings, SecondResult, &SecondError));
    TestEqual(TEXT("...with the empty-buffer code"),
        SecondError.Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));

    return true;
}

// =========================================================================================
// E. Plotting. The PNG must exist, carry the PNG signature + an IHDR of the requested size, and
//    be non-trivial in length.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwPlotWaveformPngTest,
    "PinWright.audio.plot.WaveformPngIsWritten",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwPlotWaveformPngTest::RunTest(const FString& Parameters)
{
    using namespace PwStftTestHelpers;

    FPwAudioBuffer Buffer;
    Buffer.SampleRate = TestSampleRate;
    Buffer.Left = MakeSine(TestSampleRate / 2, TestSampleRate, 220.0, 0.8);
    Buffer.Right = MakeSine(TestSampleRate / 2, TestSampleRate, 330.0, 0.4);

    const FString PngPath = ScratchPngPath(TEXT("Waveform"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*PngPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
    };

    FString PlotError;
    const bool bPlotted = PwPlotWaveform(Buffer, PngPath, PlotError);
    TestTrue(FString::Printf(TEXT("PwPlotWaveform succeeded (%s)"), *PlotError), bPlotted);
    if (!bPlotted)
    {
        return false;
    }

    int32 PngWidth = 0;
    int32 PngHeight = 0;
    int64 FileSize = 0;
    TestTrue(TEXT("File is a PNG (signature + IHDR)"), ReadPngHeader(PngPath, PngWidth, PngHeight, FileSize));
    TestEqual(TEXT("IHDR width matches the default 1024"), PngWidth, 1024);
    TestEqual(TEXT("IHDR height matches the default 256"), PngHeight, 256);
    TestTrue(FString::Printf(TEXT("PNG is non-trivial (%lld bytes)"), FileSize), FileSize > 1024);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwPlotSpectrogramPngTest,
    "PinWright.audio.plot.SpectrogramPngIsWritten",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwPlotSpectrogramPngTest::RunTest(const FString& Parameters)
{
    using namespace PwStftTestHelpers;

    TArray<float> Samples;
    Samples.SetNumZeroed(TestSampleRate / 2);
    AddSine(Samples, TestSampleRate, 1000.0, 0.5);
    AddSine(Samples, TestSampleRate, 5000.0, 0.25);

    FPwStftSettings Settings;
    Settings.FftSize = 2048;
    Settings.HopSize = 256;

    FPwStftResult Result;
    FPwStftError Error;
    const bool bSucceeded = PwComputeStft(Samples, TestSampleRate, Settings, Result, &Error);
    TestTrue(FString::Printf(TEXT("PwComputeStft succeeded (%s: %s)"), *Error.Code, *Error.Message), bSucceeded);
    if (!bSucceeded)
    {
        return false;
    }

    const FString LinearPath = ScratchPngPath(TEXT("SpectrogramLinear"));
    const FString LogAxisPngPath = ScratchPngPath(TEXT("SpectrogramLog"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*LinearPath, false, true, true);
        IFileManager::Get().Delete(*LogAxisPngPath, false, true, true);
    };

    FString LinearError;
    const bool bLinear = PwPlotSpectrogram(Result, TestSampleRate, /*bLogFrequency*/ false, LinearPath, LinearError);
    TestTrue(FString::Printf(TEXT("Linear-axis plot succeeded (%s)"), *LinearError), bLinear);

    FString LogError;
    const bool bLog = PwPlotSpectrogram(Result, TestSampleRate, /*bLogFrequency*/ true, LogAxisPngPath, LogError);
    TestTrue(FString::Printf(TEXT("Log-axis plot succeeded (%s)"), *LogError), bLog);

    if (!bLinear || !bLog)
    {
        return false;
    }

    int32 LinearWidth = 0;
    int32 LinearHeight = 0;
    int64 LinearSize = 0;
    TestTrue(TEXT("Linear plot is a PNG (signature + IHDR)"),
        ReadPngHeader(LinearPath, LinearWidth, LinearHeight, LinearSize));
    TestEqual(TEXT("IHDR width matches the default 1024"), LinearWidth, 1024);
    TestEqual(TEXT("IHDR height matches the default 512"), LinearHeight, 512);
    TestTrue(FString::Printf(TEXT("Linear plot is non-trivial (%lld bytes)"), LinearSize), LinearSize > 2048);

    int32 LogWidth = 0;
    int32 LogHeight = 0;
    int64 LogSize = 0;
    TestTrue(TEXT("Log plot is a PNG (signature + IHDR)"), ReadPngHeader(LogAxisPngPath, LogWidth, LogHeight, LogSize));
    TestEqual(TEXT("Log IHDR width"), LogWidth, 1024);
    TestEqual(TEXT("Log IHDR height"), LogHeight, 512);
    TestTrue(FString::Printf(TEXT("Log plot is non-trivial (%lld bytes)"), LogSize), LogSize > 2048);

    // bLogFrequency must actually change the image. Without this the two calls could share one
    // code path and both "file exists" assertions would still pass.
    TArray<uint8> LinearBytes;
    TArray<uint8> LogBytes;
    if (FFileHelper::LoadFileToArray(LinearBytes, *LinearPath)
        && FFileHelper::LoadFileToArray(LogBytes, *LogAxisPngPath))
    {
        TestTrue(TEXT("The linear and log spectrograms are different images"), LinearBytes != LogBytes);
    }

    return true;
}

// §12: the plotters must fail on bad input rather than emit a plausible-looking image of nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwPlotRejectsBadInputTest,
    "PinWright.audio.plot.RejectsUnplottableInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwPlotRejectsBadInputTest::RunTest(const FString& Parameters)
{
    using namespace PwStftTestHelpers;

    const FString PngPath = ScratchPngPath(TEXT("ShouldNotExist"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*PngPath, false, true, true);
    };

    // An empty STFT result carries no time axis and no data.
    const FPwStftResult EmptyResult;
    FString SpectrogramError;
    TestFalse(TEXT("An empty STFT result is not plotted"),
        PwPlotSpectrogram(EmptyResult, 48000, false, PngPath, SpectrogramError));
    TestFalse(TEXT("...and the failure is explained"), SpectrogramError.IsEmpty());
    TestFalse(TEXT("...and no file was left behind"),
        IFileManager::Get().FileExists(*PngPath));

    // An empty audio buffer likewise.
    FPwAudioBuffer EmptyBuffer;
    EmptyBuffer.SampleRate = 48000;
    FString WaveformError;
    TestFalse(TEXT("An empty audio buffer is not plotted"),
        PwPlotWaveform(EmptyBuffer, PngPath, WaveformError));
    TestFalse(TEXT("...and the failure is explained"), WaveformError.IsEmpty());

    // A relative path is refused rather than resolved against the editor's working directory.
    FPwAudioBuffer Buffer;
    Buffer.SampleRate = TestSampleRate;
    Buffer.Left = MakeSine(4096, TestSampleRate, 440.0, 0.5);
    FString RelativeError;
    TestFalse(TEXT("A relative output path is refused"),
        PwPlotWaveform(Buffer, TEXT("PwPlotRelative.png"), RelativeError));
    TestFalse(TEXT("...and the failure is explained"), RelativeError.IsEmpty());

    return true;
}
