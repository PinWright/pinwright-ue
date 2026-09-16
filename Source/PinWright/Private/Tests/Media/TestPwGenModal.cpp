// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the modal resonator bank (AudioGen/PwGenModal.cpp, declared in AudioGen/PwSynthDsp.h).
//
// This generator has unusually good ground truth, so almost nothing here asserts "it returned
// true". A mode's requested T60 is recoverable from the rendered envelope by solving an
// exponential through two measured points; a mode's requested frequency is recoverable as an
// STFT peak; a mode's requested modeGainsDb is recoverable as the ratio between two STFT peaks.
// Every one of those is asserted against the number that went IN, which is what catches the
// failure this generator is most prone to: an un-normalized bank, where modeGainsDb is a knob
// whose effect silently depends on the mode's frequency and decay (rpc-design §1 - a value
// published under a name it does not measure).
//
// The failure direction (rpc-design §12) is asserted with a pre-filled sentinel buffer: every
// rejection must return false, carry the registered error code, and leave OutMono byte-for-byte
// as it was handed over. Turn any of those rejections into a silent fallback - an unknown
// exciter quietly becoming `impulse`, a mode above Nyquist quietly folding - and these break.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwSeededRandom.h"
#include "AudioGen/PwStft.h"
#include "AudioGen/PwSynthDsp.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Handlers/ErrorCodes.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true.
namespace PwGenModalTestHelpers
{
    constexpr int32 TestSampleRate = 48000;

    FPwSynthParamValue MakeNumberArray(const TArray<double>& Values)
    {
        FPwSynthParamValue Value;
        Value.Type = EPwSynthParamType::NumberArray;
        Value.Numbers = Values;
        return Value;
    }

    FPwSynthParamValue MakeString(const TCHAR* Text)
    {
        FPwSynthParamValue Value;
        Value.Type = EPwSynthParamType::Enum;
        Value.String = Text;
        return Value;
    }

    FPwSynthParamValue MakeNumber(double Number)
    {
        FPwSynthParamValue Value;
        Value.Type = EPwSynthParamType::Number;
        Value.Number = Number;
        return Value;
    }

    /** The bag a successful ParseSynthRecipe would hand the generator: every row materialized. */
    FPwSynthParams MakeModalParams(const TArray<double>& Freqs, const TArray<double>& Decays,
        const TArray<double>& Gains, const TCHAR* Exciter = TEXT("impulse"), double ExciterMs = 2.0)
    {
        FPwSynthParams Params;
        Params.Values.Add(FName(TEXT("modeFreqsHz")), MakeNumberArray(Freqs));
        Params.Values.Add(FName(TEXT("modeDecaysMs")), MakeNumberArray(Decays));
        Params.Values.Add(FName(TEXT("modeGainsDb")), MakeNumberArray(Gains));
        Params.Values.Add(FName(TEXT("exciter")), MakeString(Exciter));
        Params.Values.Add(FName(TEXT("exciterMs")), MakeNumber(ExciterMs));
        return Params;
    }

    /**
     * Renders into a NumSamples buffer. Returns PwGenModal's own verdict; the caller asserts on
     * it. No pitch envelope and no modulation unless the caller supplies them.
     */
    bool Render(TArray<float>& OutBuffer, int32 NumSamples, const FPwSynthParams& Params,
        int32 Seed, FString& OutErrorCode, FString& OutError,
        const TArray<FPwSynthPitchPoint>& PitchEnvelope = TArray<FPwSynthPitchPoint>(),
        const FPwSynthModulation& Modulation = FPwSynthModulation())
    {
        OutBuffer.SetNumZeroed(NumSamples);
        FPwSeededRandom Rng(Seed);
        return PwGenModal(Params, PitchEnvelope, Modulation, TestSampleRate, Rng,
            TArrayView<float>(OutBuffer), OutErrorCode, OutError);
    }

    /** A buffer whose every sample is recognisable, so "untouched on failure" is checkable. */
    void FillSentinel(TArray<float>& Buffer, int32 NumSamples)
    {
        Buffer.SetNumUninitialized(NumSamples);
        for (int32 Index = 0; Index < NumSamples; ++Index)
        {
            Buffer[Index] = 1000.f + static_cast<float>(Index);
        }
    }

    bool IsSentinelIntact(const TArray<float>& Buffer)
    {
        for (int32 Index = 0; Index < Buffer.Num(); ++Index)
        {
            if (Buffer[Index] != 1000.f + static_cast<float>(Index))
            {
                return false;
            }
        }
        return true;
    }

    /**
     * Asserts the whole rejection contract in one place: false, the expected registered code,
     * a non-empty message, and a buffer the generator never touched.
     */
    void ExpectRejected(FAutomationTestBase& Test, const TCHAR* Label, const FPwSynthParams& Params,
        const TCHAR* ExpectedCode, int32 NumSamples = 4800)
    {
        TArray<float> Buffer;
        FillSentinel(Buffer, NumSamples);

        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenModal(Params, TArray<FPwSynthPitchPoint>(), FPwSynthModulation(),
            TestSampleRate, Rng, TArrayView<float>(Buffer), ErrorCode, Error);

        Test.TestFalse(FString::Printf(TEXT("%s: PwGenModal returns false"), Label), bRendered);
        Test.TestEqual(FString::Printf(TEXT("%s: error code"), Label), ErrorCode, FString(ExpectedCode));
        Test.TestTrue(FString::Printf(TEXT("%s: error message is not empty"), Label), !Error.IsEmpty());
        Test.TestTrue(FString::Printf(TEXT("%s: OutMono left untouched (was: %s)"), Label, *Error),
            IsSentinelIntact(Buffer));
    }

    double ToDb(double Ratio)
    {
        return 20.0 * FMath::Loge(Ratio) / FMath::Loge(10.0);
    }

    /** Largest |sample| in [StartMs, StartMs + LengthMs). Returns 0 for an out-of-range window. */
    double PeakAbsInWindow(const TArray<float>& Samples, double StartMs, double LengthMs)
    {
        const int32 First = FMath::RoundToInt32(StartMs * TestSampleRate / 1000.0);
        const int32 Last = FMath::Min(Samples.Num(), First + FMath::RoundToInt32(LengthMs * TestSampleRate / 1000.0));
        double Peak = 0.0;
        for (int32 Index = FMath::Max(0, First); Index < Last; ++Index)
        {
            Peak = FMath::Max(Peak, static_cast<double>(FMath::Abs(Samples[Index])));
        }
        return Peak;
    }

    double PeakAbs(const TArray<float>& Samples)
    {
        double Peak = 0.0;
        for (const float Sample : Samples)
        {
            Peak = FMath::Max(Peak, static_cast<double>(FMath::Abs(Sample)));
        }
        return Peak;
    }

    /**
     * Recovers T60 from two envelope samples. The envelope of a mode is A*r^n, so the dB drop
     * between two times is linear in elapsed time and T60 = 60 dB * elapsed / drop. Both windows
     * are measured the same way, so the "peak within a window happens a few samples after the
     * window opens" bias is common to both and divides out of the ratio.
     * Returns a negative number when the signal does not decay between the two points.
     */
    double MeasureT60Ms(const TArray<float>& Samples, double FirstMs, double SecondMs, double WindowMs)
    {
        const double Early = PeakAbsInWindow(Samples, FirstMs, WindowMs);
        const double Late = PeakAbsInWindow(Samples, SecondMs, WindowMs);
        if (Early <= 0.0 || Late <= 0.0 || Late >= Early)
        {
            return -1.0;
        }
        return 60.0 * (SecondMs - FirstMs) / ToDb(Early / Late);
    }

    bool AllSamplesFinite(const TArray<float>& Samples)
    {
        for (const float Sample : Samples)
        {
            if (!FMath::IsFinite(Sample))
            {
                return false;
            }
        }
        return true;
    }

    /** Magnitude spectrum of the first analysis frame. */
    bool AnalyseFrameZero(const TArray<float>& Samples, int32 FftSize, int32 HopSize, FPwStftResult& Out)
    {
        FPwStftSettings Settings;
        Settings.FftSize = FftSize;
        Settings.HopSize = HopSize;
        FPwStftError Error;
        return PwComputeStft(Samples, TestSampleRate, Settings, Out, &Error) && Out.NumFrames > 0;
    }

    /** Argmax bin within RadiusBins of TargetHz. */
    int32 PeakBinNear(const FPwStftResult& Result, int32 Frame, double TargetHz, int32 RadiusBins)
    {
        const int32 Center = FMath::RoundToInt32(TargetHz / static_cast<double>(Result.BinHz));
        const int32 First = FMath::Max(0, Center - RadiusBins);
        const int32 Last = FMath::Min(Result.NumBins - 1, Center + RadiusBins);
        int32 BestBin = First;
        float BestMagnitude = -1.f;
        for (int32 Bin = First; Bin <= Last; ++Bin)
        {
            const float Magnitude = PwStftMagnitudeAt(Result, Frame, Bin);
            if (Magnitude > BestMagnitude)
            {
                BestMagnitude = Magnitude;
                BestBin = Bin;
            }
        }
        return BestBin;
    }
}

// =========================================================================================
// A. A single 1 kHz mode with a 200 ms T60 rings at 1 kHz and decays at exactly the rate the
//    caller asked for. Two independent measurements of the same claim: the two-point solve,
//    and the direct "-60 dB one T60 later" check.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenModalSingleModeT60Test,
    "PinWright.audio.gen.modal.SingleModeT60MatchesRequest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenModalSingleModeT60Test::RunTest(const FString& Parameters)
{
    using namespace PwGenModalTestHelpers;

    // -6 dB rather than 0 dB: a normalization bug that happens to produce unity would pass a
    // 0 dB test by coincidence.
    const FPwSynthParams Params = MakeModalParams({ 1000.0 }, { 200.0 }, { -6.0 });

    TArray<float> Buffer;
    FString ErrorCode;
    FString Error;
    const bool bRendered = Render(Buffer, TestSampleRate * 2 / 5, Params, 7, ErrorCode, Error);   // 400 ms
    TestTrue(FString::Printf(TEXT("PwGenModal succeeded (%s: %s)"), *ErrorCode, *Error), bRendered);
    if (!bRendered)
    {
        return false;
    }
    TestTrue(TEXT("error code cleared on success"), ErrorCode.IsEmpty());
    TestTrue(TEXT("every sample is finite"), AllSamplesFinite(Buffer));

    // The bank is normalized so the impulse response peaks at the requested gain, so -6 dB
    // must arrive as 0.5012 - not as "whatever 1/sin(theta) happens to be at 1 kHz" (7.6).
    const double ExpectedPeak = FMath::Pow(10.0, -6.0 / 20.0);
    const double MeasuredPeak = PeakAbs(Buffer);
    TestTrue(FString::Printf(TEXT("peak %.5f is the requested -6 dB (%.5f)"), MeasuredPeak, ExpectedPeak),
        FMath::Abs(MeasuredPeak - ExpectedPeak) < 0.01 * ExpectedPeak);

    // Two-point solve. 50 ms and 150 ms are both whole numbers of 1 kHz periods at 48 kHz, so
    // the two windows sample the waveform at identical phase and the ratio is exactly r^4800.
    const double SolvedT60Ms = MeasureT60Ms(Buffer, 50.0, 150.0, 10.0);
    TestTrue(FString::Printf(TEXT("solved T60 %.2f ms is within 3%% of the requested 200 ms"), SolvedT60Ms),
        SolvedT60Ms > 0.0 && FMath::Abs(SolvedT60Ms - 200.0) < 0.03 * 200.0);

    // Direct statement of what a T60 means: 60 dB down, one T60 later.
    const double EarlyPeak = PeakAbsInWindow(Buffer, 0.0, 10.0);
    const double PeakOneT60Later = PeakAbsInWindow(Buffer, 200.0, 10.0);
    TestTrue(TEXT("the mode is still measurable one T60 later"), PeakOneT60Later > 0.0);
    if (PeakOneT60Later > 0.0)
    {
        const double DropDb = ToDb(EarlyPeak / PeakOneT60Later);
        TestTrue(FString::Printf(TEXT("drop over one T60 is %.2f dB, expected 60 dB"), DropDb),
            FMath::Abs(DropDb - 60.0) < 1.0);
    }

    // The frequency half of the claim.
    FPwStftResult Spectrum;
    if (TestTrue(TEXT("STFT of the render succeeded"), AnalyseFrameZero(Buffer, 4096, 1024, Spectrum)))
    {
        const int32 PeakBin = PeakBinNear(Spectrum, 0, 1000.0, 40);
        const double PeakHz = PeakBin * Spectrum.BinHz;
        TestTrue(FString::Printf(TEXT("spectral peak at %.1f Hz is within one bin of 1000 Hz"), PeakHz),
            FMath::Abs(PeakHz - 1000.0) <= Spectrum.BinHz);
    }
    return true;
}

// =========================================================================================
// B. Three modes produce three distinct spectral peaks at the three requested frequencies,
//    with a floor between them - i.e. it is a mode bank, not one smeared resonance.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenModalThreeModePeaksTest,
    "PinWright.audio.gen.modal.ThreeModesProduceThreePeaks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenModalThreeModePeaksTest::RunTest(const FString& Parameters)
{
    using namespace PwGenModalTestHelpers;

    const TArray<double> ModeHz = { 500.0, 1300.0, 2900.0 };
    const FPwSynthParams Params = MakeModalParams(ModeHz, { 400.0, 400.0, 400.0 }, { 0.0, 0.0, 0.0 });

    TArray<float> Buffer;
    FString ErrorCode;
    FString Error;
    const bool bRendered = Render(Buffer, 16384, Params, 11, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("PwGenModal succeeded (%s: %s)"), *ErrorCode, *Error), bRendered);
    if (!bRendered)
    {
        return false;
    }

    FPwStftResult Spectrum;
    if (!TestTrue(TEXT("STFT of the render succeeded"), AnalyseFrameZero(Buffer, 4096, 1024, Spectrum)))
    {
        return false;
    }

    double WeakestPeak = TNumericLimits<double>::Max();
    for (const double TargetHz : ModeHz)
    {
        const int32 PeakBin = PeakBinNear(Spectrum, 0, TargetHz, 8);
        const double PeakHz = PeakBin * Spectrum.BinHz;
        TestTrue(FString::Printf(TEXT("a peak sits within one bin of %.0f Hz (found %.1f Hz)"), TargetHz, PeakHz),
            FMath::Abs(PeakHz - TargetHz) <= Spectrum.BinHz);
        WeakestPeak = FMath::Min(WeakestPeak, static_cast<double>(PwStftMagnitudeAt(Spectrum, 0, PeakBin)));
    }
    TestTrue(FString::Printf(TEXT("the weakest of the three peaks (%.4f) is well above the noise floor"), WeakestPeak),
        WeakestPeak > 0.05);

    // Midway between adjacent modes there must be nothing. Without this the test would pass on
    // a single broadband hiss whose argmax happens to land near each target.
    const double TroughHz[2] = { 0.5 * (ModeHz[0] + ModeHz[1]), 0.5 * (ModeHz[1] + ModeHz[2]) };
    for (const double Trough : TroughHz)
    {
        const int32 TroughBin = FMath::RoundToInt32(Trough / static_cast<double>(Spectrum.BinHz));
        const double TroughMagnitude = PwStftMagnitudeAt(Spectrum, 0, TroughBin);
        TestTrue(FString::Printf(TEXT("the gap at %.0f Hz (%.5f) is at least 10x below the weakest peak (%.4f)"),
            Trough, TroughMagnitude, WeakestPeak), TroughMagnitude * 10.0 < WeakestPeak);
    }
    return true;
}

// =========================================================================================
// C. modeGainsDb is honoured. THIS is the assertion that catches an un-normalized bank: a raw
//    two-pole resonator's gain is ~1/sin(theta), so 750 Hz would come out ~9.4 dB hotter than
//    2250 Hz all on its own and the requested 12 dB separation would measure as ~21 dB.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenModalGainsHonouredTest,
    "PinWright.audio.gen.modal.ModeGainsAreHonoured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenModalGainsHonouredTest::RunTest(const FString& Parameters)
{
    using namespace PwGenModalTestHelpers;

    // 750 and 2250 Hz are exact bin centres at FftSize 4096 / 48 kHz (bins 64 and 192), so
    // scalloping loss cannot contaminate the measured ratio.
    const FPwSynthParams Params = MakeModalParams({ 750.0, 2250.0 }, { 500.0, 500.0 }, { 0.0, -12.0 });

    TArray<float> Buffer;
    FString ErrorCode;
    FString Error;
    const bool bRendered = Render(Buffer, 8192, Params, 13, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("PwGenModal succeeded (%s: %s)"), *ErrorCode, *Error), bRendered);
    if (!bRendered)
    {
        return false;
    }

    FPwStftResult Spectrum;
    if (!TestTrue(TEXT("STFT of the render succeeded"), AnalyseFrameZero(Buffer, 4096, 1024, Spectrum)))
    {
        return false;
    }

    const double LoudMagnitude = PwStftMagnitudeAt(Spectrum, 0, PeakBinNear(Spectrum, 0, 750.0, 4));
    const double QuietMagnitude = PwStftMagnitudeAt(Spectrum, 0, PeakBinNear(Spectrum, 0, 2250.0, 4));
    TestTrue(TEXT("both modes are present"), LoudMagnitude > 0.0 && QuietMagnitude > 0.0);
    if (LoudMagnitude <= 0.0 || QuietMagnitude <= 0.0)
    {
        return false;
    }

    // Both modes share a T60, so the window weighting is identical and the magnitude ratio is
    // the amplitude ratio the caller requested.
    const double SeparationDb = ToDb(LoudMagnitude / QuietMagnitude);
    TestTrue(FString::Printf(TEXT("measured separation %.2f dB matches the requested 12 dB"), SeparationDb),
        FMath::Abs(SeparationDb - 12.0) < 1.0);
    return true;
}

// =========================================================================================
// C2. modeGainsDb is honoured ABOVE SampleRate/4 too. Separate from C because it fails for a
//     different reason: past SR/4 the sampled sine steps over whole lobes of its own envelope,
//     so a normalization that solves the impulse peak in theta directly finds the peak of a
//     curve the samples never visit. A 20 kHz mode on a 48 kHz bus really peaks at 1.99x the
//     naive answer, so this catches a ~6 dB error in exactly the register where a bank most
//     needs its top modes not to be hot.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenModalNearNyquistGainTest,
    "PinWright.audio.gen.modal.NearNyquistModeGainIsHonoured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenModalNearNyquistGainTest::RunTest(const FString& Parameters)
{
    using namespace PwGenModalTestHelpers;

    const FPwSynthParams Params = MakeModalParams({ 20000.0 }, { 50.0 }, { -6.0 });

    TArray<float> Buffer;
    FString ErrorCode;
    FString Error;
    const bool bRendered = Render(Buffer, TestSampleRate / 10, Params, 23, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("PwGenModal succeeded (%s: %s)"), *ErrorCode, *Error), bRendered);
    if (!bRendered)
    {
        return false;
    }

    const double ExpectedPeak = FMath::Pow(10.0, -6.0 / 20.0);
    const double MeasuredPeak = PeakAbs(Buffer);
    TestTrue(FString::Printf(TEXT("20 kHz mode peaks at %.5f, the requested -6 dB (%.5f)"),
        MeasuredPeak, ExpectedPeak), FMath::Abs(MeasuredPeak - ExpectedPeak) < 0.01 * ExpectedPeak);
    return true;
}

// =========================================================================================
// D. A shorter T60 decays faster - measured, not assumed - while the peak stays put, which is
//    the other half of the normalization claim: level and decay are independent knobs.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenModalShorterDecayFallsFasterTest,
    "PinWright.audio.gen.modal.ShorterDecayFallsFaster",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenModalShorterDecayFallsFasterTest::RunTest(const FString& Parameters)
{
    using namespace PwGenModalTestHelpers;

    TArray<float> Short;
    TArray<float> Long;
    FString ErrorCode;
    FString Error;
    const bool bShortRendered = Render(Short, TestSampleRate / 2, MakeModalParams({ 1000.0 }, { 100.0 }, { 0.0 }),
        3, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("short-decay render succeeded (%s: %s)"), *ErrorCode, *Error), bShortRendered);
    const bool bLongRendered = Render(Long, TestSampleRate / 2, MakeModalParams({ 1000.0 }, { 400.0 }, { 0.0 }),
        3, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("long-decay render succeeded (%s: %s)"), *ErrorCode, *Error), bLongRendered);
    if (!bShortRendered || !bLongRendered)
    {
        return false;
    }

    const double ShortPeak = PeakAbs(Short);
    const double LongPeak = PeakAbs(Long);
    TestTrue(FString::Printf(TEXT("both peak at the requested 0 dB (short %.5f, long %.5f)"), ShortPeak, LongPeak),
        FMath::Abs(ShortPeak - 1.0) < 0.01 && FMath::Abs(LongPeak - 1.0) < 0.01);

    // 200 ms in: the 100 ms mode is two T60s down (-120 dB), the 400 ms mode half a T60 (-30 dB).
    const double ShortLate = PeakAbsInWindow(Short, 200.0, 10.0);
    const double LongLate = PeakAbsInWindow(Long, 200.0, 10.0);
    TestTrue(FString::Printf(TEXT("at 200 ms the 100 ms mode (%.3e) is far below the 400 ms mode (%.3e)"),
        ShortLate, LongLate), ShortLate < 0.01 * LongLate);

    // And each one's measured T60 is its own requested T60, so "faster" is quantitatively right
    // rather than merely ordered.
    const double ShortT60 = MeasureT60Ms(Short, 20.0, 60.0, 10.0);
    const double LongT60 = MeasureT60Ms(Long, 50.0, 150.0, 10.0);
    TestTrue(FString::Printf(TEXT("short mode measures %.2f ms against a requested 100 ms"), ShortT60),
        ShortT60 > 0.0 && FMath::Abs(ShortT60 - 100.0) < 0.05 * 100.0);
    TestTrue(FString::Printf(TEXT("long mode measures %.2f ms against a requested 400 ms"), LongT60),
        LongT60 > 0.0 && FMath::Abs(LongT60 - 400.0) < 0.05 * 400.0);
    return true;
}

// =========================================================================================
// E. Stability. A pole at the radius a multi-second T60 needs sits within 1e-5 of the unit
//    circle; the failure mode this guards is a bank that grows, NaNs, or rings forever.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenModalHighQStaysStableTest,
    "PinWright.audio.gen.modal.HighQModeStaysStable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenModalHighQStaysStableTest::RunTest(const FString& Parameters)
{
    using namespace PwGenModalTestHelpers;

    // The schema's longest legal decay (20 s) across a full-width bank, over two seconds of audio.
    TArray<double> Freqs;
    TArray<double> Decays;
    TArray<double> Gains;
    for (int32 Index = 0; Index < 8; ++Index)
    {
        Freqs.Add(180.0 + 437.0 * Index);
        Decays.Add(20000.0);
        Gains.Add(0.0);
    }
    const FPwSynthParams Params = MakeModalParams(Freqs, Decays, Gains);

    const int32 NumSamples = TestSampleRate * 2;
    TArray<float> Buffer;
    // A sentinel far outside any legal render, so "every sample is bounded" also proves every
    // sample was WRITTEN - the generator must fill OutMono exactly, never partially.
    Buffer.SetNumUninitialized(NumSamples);
    for (int32 Index = 0; Index < NumSamples; ++Index)
    {
        Buffer[Index] = -1.0e30f;
    }

    FPwSeededRandom Rng(29);
    FString ErrorCode;
    FString Error;
    const bool bRendered = PwGenModal(Params, TArray<FPwSynthPitchPoint>(), FPwSynthModulation(),
        TestSampleRate, Rng, TArrayView<float>(Buffer), ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("PwGenModal succeeded (%s: %s)"), *ErrorCode, *Error), bRendered);
    if (!bRendered)
    {
        return false;
    }

    TestTrue(TEXT("every sample is finite"), AllSamplesFinite(Buffer));

    // Eight modes each normalized to a peak of 1.0, and their peaks do not coincide, so the sum
    // cannot reach 8. The sentinel is -1e30, so this also fails on any unwritten sample.
    const double Peak = PeakAbs(Buffer);
    TestTrue(FString::Printf(TEXT("peak %.4f is bounded by the summed mode gains"), Peak),
        Peak > 0.0 && Peak <= 8.0);

    // Non-growing: a 20 s T60 loses ~12 dB over two seconds, so the tail must be clearly
    // quieter than the head while still ringing.
    const double Head = PeakAbsInWindow(Buffer, 10.0, 20.0);
    const double Tail = PeakAbsInWindow(Buffer, 1970.0, 20.0);
    TestTrue(FString::Printf(TEXT("the tail (%.5f) is quieter than the head (%.5f)"), Tail, Head), Tail < Head);
    TestTrue(FString::Printf(TEXT("the bank is still ringing at 2 s (%.5f)"), Tail), Tail > 1.0e-4);
    return true;
}

// =========================================================================================
// F. Determinism. Same params and seed twice, byte-identical; a different seed changes the
//    render, which is what proves the RNG is actually consulted rather than decorative.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenModalDeterminismTest,
    "PinWright.audio.gen.modal.SameSeedIsByteIdentical",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenModalDeterminismTest::RunTest(const FString& Parameters)
{
    using namespace PwGenModalTestHelpers;

    // The noise exciter is the only stochastic path in this generator, so it is the one that
    // can be non-deterministic (Audio::FWhiteNoise's default constructor seeds from the CPU
    // cycle counter - see the warning on FPwSeededRandom).
    const FPwSynthParams Params = MakeModalParams({ 420.0, 980.0, 2310.0 },
        { 300.0, 220.0, 140.0 }, { 0.0, -3.0, -9.0 }, TEXT("noise"), 6.0);

    const int32 NumSamples = TestSampleRate / 5;
    TArray<float> First;
    TArray<float> Second;
    TArray<float> Different;
    FString ErrorCode;
    FString Error;
    const bool bAll =
        Render(First, NumSamples, Params, 20260816, ErrorCode, Error) &&
        Render(Second, NumSamples, Params, 20260816, ErrorCode, Error) &&
        Render(Different, NumSamples, Params, 90210, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("all three renders succeeded (%s: %s)"), *ErrorCode, *Error), bAll);
    if (!bAll)
    {
        return false;
    }

    TestEqual(TEXT("the two same-seed renders are the same length"), First.Num(), Second.Num());
    TestTrue(TEXT("same seed renders byte-identically"),
        First.Num() == Second.Num() &&
        FMemory::Memcmp(First.GetData(), Second.GetData(), First.Num() * sizeof(float)) == 0);
    TestTrue(TEXT("a different seed produces a different render"),
        First.Num() == Different.Num() &&
        FMemory::Memcmp(First.GetData(), Different.GetData(), First.Num() * sizeof(float)) != 0);
    return true;
}

// =========================================================================================
// G. All three schema exciters render, are audibly different from each other, and none of
//    them is secretly the impulse path.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenModalExcitersRenderTest,
    "PinWright.audio.gen.modal.AllExcitersRender",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenModalExcitersRenderTest::RunTest(const FString& Parameters)
{
    using namespace PwGenModalTestHelpers;

    // 250 / 600 Hz with a 2 ms burst: both sit inside the raised-cosine window's mainlobe
    // (its first null is at 2 / exciterMs = 1000 Hz), so the strike exciter cannot land on a
    // spectral null and read as a broken exciter.
    const TArray<double> Freqs = { 250.0, 600.0 };
    const TArray<double> Decays = { 300.0, 300.0 };
    const TArray<double> Gains = { 0.0, 0.0 };
    const int32 NumSamples = TestSampleRate / 5;

    TArray<TArray<float>> Renders;
    const TCHAR* Names[] = { TEXT("impulse"), TEXT("noise"), TEXT("strike") };
    for (const TCHAR* Name : Names)
    {
        TArray<float> Buffer;
        FString ErrorCode;
        FString Error;
        const bool bRendered = Render(Buffer, NumSamples, MakeModalParams(Freqs, Decays, Gains, Name, 2.0),
            5, ErrorCode, Error);
        TestTrue(FString::Printf(TEXT("exciter '%s' rendered (%s: %s)"), Name, *ErrorCode, *Error), bRendered);
        if (!bRendered)
        {
            return false;
        }
        TestTrue(FString::Printf(TEXT("exciter '%s' produced finite samples"), Name), AllSamplesFinite(Buffer));
        TestTrue(FString::Printf(TEXT("exciter '%s' produced audible output (peak %.5f)"), Name, PeakAbs(Buffer)),
            PeakAbs(Buffer) > 1.0e-3);
        Renders.Add(MoveTemp(Buffer));
    }

    TestTrue(TEXT("noise differs from impulse"),
        FMemory::Memcmp(Renders[0].GetData(), Renders[1].GetData(), NumSamples * sizeof(float)) != 0);
    TestTrue(TEXT("strike differs from impulse"),
        FMemory::Memcmp(Renders[0].GetData(), Renders[2].GetData(), NumSamples * sizeof(float)) != 0);
    TestTrue(TEXT("strike differs from noise"),
        FMemory::Memcmp(Renders[1].GetData(), Renders[2].GetData(), NumSamples * sizeof(float)) != 0);
    return true;
}

// =========================================================================================
// G2. Choosing an exciter buys spectral content, not loudness. Low enough in the band that the
//     burst window is still flat, a strike must arrive at the impulse's level. This is the
//     assertion that pins the exciter normalization: scale the strike to unit ENERGY instead of
//     unit AREA and a 2 ms mallet arrives ~18 dB hot, growing with exciterMs - "longer contact,
//     louder hit", which is backwards for a struck body.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenModalExciterLevelsAgreeTest,
    "PinWright.audio.gen.modal.ExciterLevelsAgreeAtLowFrequency",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenModalExciterLevelsAgreeTest::RunTest(const FString& Parameters)
{
    using namespace PwGenModalTestHelpers;

    // 200 Hz against a 2 ms burst is fT = 0.4, where the raised cosine is still at 0.90 of its
    // DC response - so the exciters differ here by their normalization and by nothing else.
    const TArray<double> Freqs = { 200.0 };
    const TArray<double> Decays = { 300.0 };
    const TArray<double> Gains = { 0.0 };
    const int32 NumSamples = TestSampleRate / 5;

    TArray<float> Impulse;
    TArray<float> Strike;
    TArray<float> Noise;
    FString ErrorCode;
    FString Error;
    const bool bAll =
        Render(Impulse, NumSamples, MakeModalParams(Freqs, Decays, Gains, TEXT("impulse"), 2.0), 31, ErrorCode, Error) &&
        Render(Strike, NumSamples, MakeModalParams(Freqs, Decays, Gains, TEXT("strike"), 2.0), 31, ErrorCode, Error) &&
        Render(Noise, NumSamples, MakeModalParams(Freqs, Decays, Gains, TEXT("noise"), 2.0), 31, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("all three renders succeeded (%s: %s)"), *ErrorCode, *Error), bAll);
    if (!bAll)
    {
        return false;
    }

    const double ImpulsePeak = PeakAbs(Impulse);
    TestTrue(TEXT("the impulse render is audible"), ImpulsePeak > 0.0);
    if (ImpulsePeak <= 0.0)
    {
        return false;
    }

    // A narrowband mode filters the exciter down to |W(f_mode)|, and a unit-area raised cosine
    // is at 0.90 of its DC response at fT = 0.4, so the strike must land ~0.9 dB below impulse.
    const double StrikeDb = ToDb(PeakAbs(Strike) / ImpulsePeak);
    TestTrue(FString::Printf(TEXT("strike sits %.2f dB from impulse at 200 Hz"), StrikeDb),
        FMath::Abs(StrikeDb) < 3.0);

    // The noise burst gets no level assertion on purpose. Its level is a draw, not a constant -
    // the mode integrates ~96 random samples, so its peak is a chi-like variable around the
    // impulse's - and a tight gate here would be a coin flip dressed up as a measurement, while
    // a gate loose enough to be safe would measure nothing. Its render is covered by
    // AllExcitersRender (finite, audible, distinct) instead.
    TestTrue(TEXT("the noise render is audible"), PeakAbs(Noise) > 1.0e-3);
    return true;
}

// =========================================================================================
// H. The pitch envelope shifts every mode together as a multiplier: +12 semitones doubles the
//    ringing frequency.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenModalPitchEnvelopeTest,
    "PinWright.audio.gen.modal.PitchEnvelopeShiftsModes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenModalPitchEnvelopeTest::RunTest(const FString& Parameters)
{
    using namespace PwGenModalTestHelpers;

    const FPwSynthParams Params = MakeModalParams({ 600.0 }, { 400.0 }, { 0.0 });

    TArray<FPwSynthPitchPoint> Envelope;
    Envelope.Add({ 0.0, 12.0 });
    Envelope.Add({ 1000.0, 12.0 });

    TArray<float> Buffer;
    FString ErrorCode;
    FString Error;
    const bool bRendered = Render(Buffer, 16384, Params, 17, ErrorCode, Error, Envelope);
    TestTrue(FString::Printf(TEXT("PwGenModal succeeded (%s: %s)"), *ErrorCode, *Error), bRendered);
    if (!bRendered)
    {
        return false;
    }

    FPwStftResult Spectrum;
    if (!TestTrue(TEXT("STFT of the render succeeded"), AnalyseFrameZero(Buffer, 4096, 1024, Spectrum)))
    {
        return false;
    }

    const int32 PeakBin = PeakBinNear(Spectrum, 0, 1200.0, 40);
    const double PeakHz = PeakBin * Spectrum.BinHz;
    TestTrue(FString::Printf(TEXT("+12 semitones moved the 600 Hz mode to %.1f Hz (expected 1200)"), PeakHz),
        FMath::Abs(PeakHz - 1200.0) <= Spectrum.BinHz);
    return true;
}

// =========================================================================================
// I. Failure direction (rpc-design §12). Every rejection returns false with the registered
//    code and leaves the caller's buffer untouched. A silent fallback breaks all of these.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenModalRejectionsTest,
    "PinWright.audio.gen.modal.RejectsInvalidBanks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenModalRejectionsTest::RunTest(const FString& Parameters)
{
    using namespace PwGenModalTestHelpers;

    // Mismatched array lengths. The schema's Validator hook catches this too, but the generator
    // is callable without the parser and an unchecked index here is a crash, not an error.
    ExpectRejected(*this, TEXT("decays shorter than freqs"),
        MakeModalParams({ 400.0, 900.0 }, { 200.0 }, { 0.0, 0.0 }), ErrorCodes::ERR_INVALID_PARAMS);
    ExpectRejected(*this, TEXT("gains longer than freqs"),
        MakeModalParams({ 400.0 }, { 200.0 }, { 0.0, -6.0 }), ErrorCodes::ERR_INVALID_PARAMS);

    // At and above Nyquist. Both fold onto a mirrored frequency instead of ringing where asked.
    ExpectRejected(*this, TEXT("mode exactly at Nyquist"),
        MakeModalParams({ 24000.0 }, { 200.0 }, { 0.0 }), ErrorCodes::ERR_INVALID_PARAMS);
    ExpectRejected(*this, TEXT("mode above Nyquist"),
        MakeModalParams({ 500.0, 31000.0 }, { 200.0, 200.0 }, { 0.0, 0.0 }), ErrorCodes::ERR_INVALID_PARAMS);

    // Non-positive decay: r >= 1, an oscillator that never stops.
    ExpectRejected(*this, TEXT("zero decay"),
        MakeModalParams({ 500.0 }, { 0.0 }, { 0.0 }), ErrorCodes::ERR_INVALID_PARAMS);
    ExpectRejected(*this, TEXT("negative decay"),
        MakeModalParams({ 500.0, 900.0 }, { 200.0, -50.0 }, { 0.0, 0.0 }), ErrorCodes::ERR_INVALID_PARAMS);

    // Unknown exciter - never a silent fallback to impulse (rpc-design §3).
    ExpectRejected(*this, TEXT("unknown exciter"),
        MakeModalParams({ 500.0 }, { 200.0 }, { 0.0 }, TEXT("hammer"), 2.0), ErrorCodes::ERR_INVALID_PARAMS);
    ExpectRejected(*this, TEXT("empty exciter"),
        MakeModalParams({ 500.0 }, { 200.0 }, { 0.0 }, TEXT(""), 2.0), ErrorCodes::ERR_INVALID_PARAMS);

    // Missing arrays and an empty bank.
    {
        FPwSynthParams Missing = MakeModalParams({ 500.0 }, { 200.0 }, { 0.0 });
        Missing.Values.Remove(FName(TEXT("modeFreqsHz")));
        ExpectRejected(*this, TEXT("modeFreqsHz absent"), Missing, ErrorCodes::ERR_INVALID_PARAMS);
    }
    ExpectRejected(*this, TEXT("empty mode arrays"),
        MakeModalParams({}, {}, {}), ErrorCodes::ERR_INVALID_PARAMS);

    // Past the mode cap - an error, not a truncated bank.
    {
        TArray<double> Freqs;
        TArray<double> Decays;
        TArray<double> Gains;
        for (int32 Index = 0; Index < 33; ++Index)
        {
            Freqs.Add(100.0 + 50.0 * Index);
            Decays.Add(120.0);
            Gains.Add(-6.0);
        }
        ExpectRejected(*this, TEXT("33 modes"), MakeModalParams(Freqs, Decays, Gains),
            ErrorCodes::ERR_INVALID_PARAMS);
    }

    // A zero-length output buffer is an empty result, not a silent one.
    {
        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenModal(MakeModalParams({ 500.0 }, { 200.0 }, { 0.0 }),
            TArray<FPwSynthPitchPoint>(), FPwSynthModulation(), TestSampleRate, Rng,
            TArrayView<float>(), ErrorCode, Error);
        TestFalse(TEXT("zero-length OutMono is rejected"), bRendered);
        TestEqual(TEXT("zero-length OutMono reports AUDIO_EMPTY_BUFFER"), ErrorCode,
            FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
    }

    // A non-positive sample rate cannot produce coefficients.
    {
        TArray<float> Buffer;
        FillSentinel(Buffer, 480);
        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenModal(MakeModalParams({ 500.0 }, { 200.0 }, { 0.0 }),
            TArray<FPwSynthPitchPoint>(), FPwSynthModulation(), 0, Rng,
            TArrayView<float>(Buffer), ErrorCode, Error);
        TestFalse(TEXT("sampleRate 0 is rejected"), bRendered);
        TestEqual(TEXT("sampleRate 0 reports INVALID_PARAMS"), ErrorCode,
            FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("sampleRate 0 leaves OutMono untouched"), IsSentinelIntact(Buffer));
    }
    return true;
}
