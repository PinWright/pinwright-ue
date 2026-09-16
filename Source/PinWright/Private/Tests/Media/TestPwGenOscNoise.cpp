// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the `osc` and `noise` generator kernels (AudioGen/PwGenOscNoise.cpp).
//
// These have real spectral ground truth, so the assertions run through PwComputeStft
// (AudioGen/PwStft.h) rather than checking that a call returned true: a 1 kHz sine has a known
// bin, a saw has a known harmonic series AND a known ABSENCE of folded energy, pink has a known
// tilt, a +12 semitone envelope has a known end-to-end ratio. The band-limiting assertion is the
// one that carries the most weight - a naive ramp passes every "did it make noise" check and
// fails only a measurement of where the energy that should not exist ended up.
//
// Per rpc-design.md §12 the failure direction is asserted too, and asserted the hard way: the
// output buffer is pre-filled with a sentinel and the sentinel must survive every rejection, so
// a generator that half-renders before noticing a bad parameter cannot pass.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwSeededRandom.h"
#include "AudioGen/PwStft.h"
#include "AudioGen/PwSynthDsp.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Handlers/ErrorCodes.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true.
namespace PwGenOscNoiseTestHelpers
{
    constexpr int32 TestSampleRate = 48000;

    /** Pre-fill value for the failure-direction tests. Far outside any legal sample value, so a
        surviving sentinel cannot be confused with a rendered sample. */
    constexpr float Sentinel = 12345.678f;

    void SetNumber(FPwSynthParams& Params, const TCHAR* Key, double Value)
    {
        FPwSynthParamValue Entry;
        Entry.Type = EPwSynthParamType::Number;
        Entry.Number = Value;
        Params.Values.Add(FName(Key), Entry);
    }

    void SetToken(FPwSynthParams& Params, const TCHAR* Key, const TCHAR* Value)
    {
        FPwSynthParamValue Entry;
        Entry.Type = EPwSynthParamType::Enum;
        Entry.String = Value;
        Params.Values.Add(FName(Key), Entry);
    }

    FPwSynthParams MakeOscParams(const TCHAR* Waveform, double FrequencyHz)
    {
        FPwSynthParams Params;
        SetToken(Params, TEXT("waveform"), Waveform);
        SetNumber(Params, TEXT("frequencyHz"), FrequencyHz);
        return Params;
    }

    FPwSynthParams MakeNoiseParams(const TCHAR* Color)
    {
        FPwSynthParams Params;
        SetToken(Params, TEXT("color"), Color);
        return Params;
    }

    TArray<float> MakeBuffer(int32 NumFrames, float Fill = 0.f)
    {
        TArray<float> Buffer;
        Buffer.Init(Fill, NumFrames);
        return Buffer;
    }

    /** Renders an osc layer into a fresh buffer. Every optional argument defaults to the
        no-modulation / no-envelope case so a test only spells out what it is actually testing. */
    bool RenderOsc(TArray<float>& OutBuffer, const FPwSynthParams& Params, int32 NumFrames, int32 Seed,
        FString& OutErrorCode, FString& OutError,
        const TArray<FPwSynthPitchPoint>& PitchEnvelope = TArray<FPwSynthPitchPoint>(),
        const FPwSynthModulation& Modulation = FPwSynthModulation())
    {
        OutBuffer = MakeBuffer(NumFrames);
        FPwSeededRandom Rng(Seed);
        return PwGenOsc(Params, PitchEnvelope, Modulation, TestSampleRate, Rng, OutBuffer,
            OutErrorCode, OutError);
    }

    bool RenderNoise(TArray<float>& OutBuffer, const FPwSynthParams& Params, int32 NumFrames, int32 Seed,
        FString& OutErrorCode, FString& OutError,
        const TArray<FPwSynthPitchPoint>& PitchEnvelope = TArray<FPwSynthPitchPoint>(),
        const FPwSynthModulation& Modulation = FPwSynthModulation())
    {
        OutBuffer = MakeBuffer(NumFrames);
        FPwSeededRandom Rng(Seed);
        return PwGenNoise(Params, PitchEnvelope, Modulation, TestSampleRate, Rng, OutBuffer,
            OutErrorCode, OutError);
    }

    bool Analyze(const TArray<float>& Samples, int32 FftSize, int32 HopSize, FPwStftResult& Out,
        FString& OutWhy)
    {
        FPwStftSettings Settings;
        Settings.FftSize = FftSize;
        Settings.HopSize = HopSize;

        FPwStftError Error;
        if (!PwComputeStft(Samples, TestSampleRate, Settings, Out, &Error))
        {
            OutWhy = FString::Printf(TEXT("%s: %s"), *Error.Code, *Error.Message);
            return false;
        }
        return true;
    }

    int32 FindPeakBin(const FPwStftResult& Result, int32 FrameIndex, int32 FirstBin = 1,
        int32 LastBin = MAX_int32)
    {
        const int32 End = FMath::Min(LastBin, Result.NumBins - 1);
        int32 BestBin = INDEX_NONE;
        float BestMagnitude = -1.f;
        for (int32 Bin = FMath::Max(0, FirstBin); Bin <= End; ++Bin)
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

    /** Largest magnitude in [FirstBin, LastBin], as dBFS under PwStft's documented scaling. */
    float MaxDbInBinRange(const FPwStftResult& Result, int32 FrameIndex, int32 FirstBin, int32 LastBin)
    {
        float Worst = 0.f;
        const int32 End = FMath::Min(LastBin, Result.NumBins - 1);
        for (int32 Bin = FMath::Max(0, FirstBin); Bin <= End; ++Bin)
        {
            Worst = FMath::Max(Worst, PwStftMagnitudeAt(Result, FrameIndex, Bin));
        }
        return PwMagnitudeToDb(Worst, -200.f);
    }

    /**
     * Mean magnitude^2 PER BIN over [LowHz, HighHz), averaged across every analysis frame, in dB.
     *
     * Per bin, not total, and that is the whole point: an octave band holds twice as many bins as
     * the octave below it, so a TOTAL reads flat for pink noise (pink is by definition equal
     * energy per octave) and would hide exactly the tilt this measures.
     */
    float MeanBandPowerDb(const FPwStftResult& Result, double LowHz, double HighHz)
    {
        const int32 FirstBin = FMath::Max(1, FMath::CeilToInt(LowHz / Result.BinHz));
        const int32 LastBin = FMath::Min(Result.NumBins - 1, FMath::FloorToInt(HighHz / Result.BinHz));
        if (LastBin < FirstBin || Result.NumFrames <= 0)
        {
            return -200.f;
        }

        double Sum = 0.0;
        int64 Count = 0;
        for (int32 Frame = 0; Frame < Result.NumFrames; ++Frame)
        {
            for (int32 Bin = FirstBin; Bin <= LastBin; ++Bin)
            {
                const double Magnitude = PwStftMagnitudeAt(Result, Frame, Bin);
                Sum += Magnitude * Magnitude;
                ++Count;
            }
        }

        const double Mean = FMath::Max(Sum / static_cast<double>(Count), 1e-30);
        return static_cast<float>(10.0 * FMath::Loge(Mean) / FMath::Loge(10.0));
    }

    float PeakAbs(const TArray<float>& Samples)
    {
        float Peak = 0.f;
        for (const float Sample : Samples)
        {
            Peak = FMath::Max(Peak, FMath::Abs(Sample));
        }
        return Peak;
    }

    float ToDb(float Linear)
    {
        return PwMagnitudeToDb(Linear, -200.f);
    }

    bool BuffersAreIdentical(const TArray<float>& A, const TArray<float>& B)
    {
        return A.Num() == B.Num()
            && FMemory::Memcmp(A.GetData(), B.GetData(), A.Num() * sizeof(float)) == 0;
    }

    bool AllEqual(const TArray<float>& Samples, float Value)
    {
        for (const float Sample : Samples)
        {
            if (Sample != Value)
            {
                return false;
            }
        }
        return true;
    }
}

// =============================================================================================
// osc A. A 1 kHz sine lands in the bin nearest 1 kHz, well clear of everything else.
//        This also exercises the FSinOsc2DRotation fast path, which is selected for exactly this
//        shape (sine, one voice, no pitch movement, no fm).
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenOscSineLandsInExpectedBinTest,
    "PinWright.audio.gen.osc.SineLandsInExpectedBin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenOscSineLandsInExpectedBinTest::RunTest(const FString& Parameters)
{
    using namespace PwGenOscNoiseTestHelpers;

    TArray<float> Samples;
    FString ErrorCode;
    FString Error;
    const bool bRendered = RenderOsc(Samples, MakeOscParams(TEXT("sine"), 1000.0),
        TestSampleRate, /*Seed*/ 1, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("PwGenOsc succeeded (%s: %s)"), *ErrorCode, *Error), bRendered);
    if (!bRendered)
    {
        return false;
    }

    // A unit sine at 1 kHz / 48 kHz has 48 samples per cycle, so sample 12 sits exactly on the
    // crest: the peak is 1.0, not "close to it".
    TestTrue(FString::Printf(TEXT("Peak is %.6f, expected ~1.0"), PeakAbs(Samples)),
        FMath::IsNearlyEqual(PeakAbs(Samples), 1.f, 1e-3f));

    FPwStftResult Result;
    FString Why;
    const bool bAnalyzed = Analyze(Samples, 2048, 256, Result, Why);
    if (!TestTrue(FString::Printf(TEXT("PwComputeStft succeeded (%s)"), *Why), bAnalyzed))
    {
        return false;
    }

    const int32 FrameIndex = Result.NumFrames / 2;
    const int32 ExpectedBin = FMath::RoundToInt(1000.f / Result.BinHz);      // 1000 / 23.4375 -> 43
    TestEqual(TEXT("Nearest bin to 1000 Hz is 43"), ExpectedBin, 43);
    TestEqual(TEXT("Magnitude peak sits in the bin nearest 1000 Hz"),
        FindPeakBin(Result, FrameIndex), ExpectedBin);

    // Separation from everything at least 16 bins out, where the periodic Hann kernel is already
    // below -80 dB. A generator emitting anything but a clean tone breaks this long before it
    // breaks the peak-bin assertion.
    const float PeakDb = PwMagnitudeToDb(PwStftMagnitudeAt(Result, FrameIndex, ExpectedBin));
    const float LowFloorDb = MaxDbInBinRange(Result, FrameIndex, 1, ExpectedBin - 16);
    const float HighFloorDb = MaxDbInBinRange(Result, FrameIndex, ExpectedBin + 16, Result.NumBins - 1);
    const float FloorDb = FMath::Max(LowFloorDb, HighFloorDb);
    TestTrue(FString::Printf(TEXT("Peak %.2f dBFS is >= 40 dB above the %.2f dBFS floor"), PeakDb, FloorDb),
        (PeakDb - FloorDb) >= 40.f);

    return true;
}

// =============================================================================================
// osc B. A saw's harmonics fall as 1/n.
//
// A bipolar saw of amplitude 1 has Fourier coefficients 2 / (pi * n), so harmonic n sits
// 20*log10(n) dB below the fundamental: -6.02, -9.54, -12.04 dB at 2, 3 and 4 kHz. Scalloping
// moves each by up to 0.63 dB (1 / 2 / 4 kHz land a third of a bin off centre at this FFT size,
// 3 kHz lands exactly on bin 128), which the 2.5 dB tolerance absorbs while still failing any
// generator that is not producing a saw.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenOscSawHarmonicsTest,
    "PinWright.audio.gen.osc.SawHarmonicsFollowOneOverN",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenOscSawHarmonicsTest::RunTest(const FString& Parameters)
{
    using namespace PwGenOscNoiseTestHelpers;

    TArray<float> Samples;
    FString ErrorCode;
    FString Error;
    const bool bRendered = RenderOsc(Samples, MakeOscParams(TEXT("saw"), 1000.0),
        TestSampleRate, /*Seed*/ 1, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("PwGenOsc succeeded (%s: %s)"), *ErrorCode, *Error), bRendered);
    if (!bRendered)
    {
        return false;
    }

    FPwStftResult Result;
    FString Why;
    const bool bAnalyzed = Analyze(Samples, 2048, 256, Result, Why);
    if (!TestTrue(FString::Printf(TEXT("PwComputeStft succeeded (%s)"), *Why), bAnalyzed))
    {
        return false;
    }

    const int32 FrameIndex = Result.NumFrames / 2;
    const int32 FundamentalBin = FMath::RoundToInt(1000.f / Result.BinHz);
    TestEqual(TEXT("The fundamental is the global peak"), FindPeakBin(Result, FrameIndex), FundamentalBin);

    const float FundamentalDb = PwMagnitudeToDb(PwStftMagnitudeAt(Result, FrameIndex, FundamentalBin));

    for (int32 Harmonic = 2; Harmonic <= 4; ++Harmonic)
    {
        const int32 Bin = FMath::RoundToInt(Harmonic * 1000.f / Result.BinHz);
        const float HarmonicDb = PwMagnitudeToDb(PwStftMagnitudeAt(Result, FrameIndex, Bin));
        const float RelativeDb = HarmonicDb - FundamentalDb;
        const float IdealDb = -20.f * FMath::Loge(static_cast<float>(Harmonic)) / FMath::Loge(10.f);

        TestTrue(FString::Printf(
            TEXT("Harmonic %d (bin %d) is %.2f dB below the fundamental, expected %.2f dB (1/n)"),
            Harmonic, Bin, RelativeDb, IdealDb),
            FMath::Abs(RelativeDb - IdealDb) <= 2.5f);
    }

    return true;
}

// =============================================================================================
// osc C. The saw is BAND-LIMITED: nothing folds back down over Nyquist.
//
// This is the assertion the PolyBLEP correction exists for, and it is deliberately measured
// below the fundamental, where a band-limited saw has no business putting any energy at all.
//
// 2500.7 Hz is chosen so the fold lands somewhere visible: a NAIVE saw at this pitch puts its
// 19th harmonic at 47513 Hz, which folds to 487 Hz at an amplitude of 2/(19*pi) = -29.5 dBFS,
// i.e. only ~25.6 dB below the fundamental's -3.9 dBFS. The 35 dB threshold below therefore
// fails a naive ramp outright while leaving the band-limited generator a wide margin. The
// fundamental is deliberately NOT an exact submultiple of the sample rate - at 1000 Hz / 48 kHz
// every alias would fold exactly onto a real harmonic and the test would be blind.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenOscSawIsBandLimitedTest,
    "PinWright.audio.gen.osc.SawIsBandLimited",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenOscSawIsBandLimitedTest::RunTest(const FString& Parameters)
{
    using namespace PwGenOscNoiseTestHelpers;

    constexpr double FundamentalHz = 2500.7;

    TArray<float> Samples;
    FString ErrorCode;
    FString Error;
    const bool bRendered = RenderOsc(Samples, MakeOscParams(TEXT("saw"), FundamentalHz),
        TestSampleRate, /*Seed*/ 1, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("PwGenOsc succeeded (%s: %s)"), *ErrorCode, *Error), bRendered);
    if (!bRendered)
    {
        return false;
    }

    FPwStftResult Result;
    FString Why;
    const bool bAnalyzed = Analyze(Samples, 2048, 256, Result, Why);
    if (!TestTrue(FString::Printf(TEXT("PwComputeStft succeeded (%s)"), *Why), bAnalyzed))
    {
        return false;
    }

    const int32 FrameIndex = Result.NumFrames / 2;
    const int32 FundamentalBin = FMath::RoundToInt(FundamentalHz / Result.BinHz);   // 107
    const float FundamentalDb = PwMagnitudeToDb(PwStftMagnitudeAt(Result, FrameIndex, FundamentalBin));
    TestEqual(TEXT("The fundamental is the global peak"), FindPeakBin(Result, FrameIndex), FundamentalBin);

    // Bins 3 .. FundamentalBin-8: everything below the fundamental, skipping DC and its immediate
    // leakage and holding back 8 bins so the measurement is not reading the fundamental's own
    // Hann skirt.
    const float SubFundamentalDb = MaxDbInBinRange(Result, FrameIndex, 3, FundamentalBin - 8);
    TestTrue(FString::Printf(
        TEXT("Loudest bin below the fundamental is %.2f dBFS, %.2f dB under the %.2f dBFS fundamental ")
        TEXT("(a naive ramp lands at ~25.6 dB here); require >= 35 dB"),
        SubFundamentalDb, FundamentalDb - SubFundamentalDb, FundamentalDb),
        (FundamentalDb - SubFundamentalDb) >= 35.f);

    return true;
}

// =============================================================================================
// osc D. A pitch envelope of 0 -> +12 semitones doubles the measured fundamental.
//
// 468.75 Hz is exactly bin 20 at this FFT size and 937.5 Hz is exactly bin 40, so the ratio the
// test measures is exact rather than a quantised approximation. The envelope holds flat for the
// first and last 100 ms so the first and last analysis windows each see a steady pitch instead
// of a smear.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenOscPitchEnvelopeTest,
    "PinWright.audio.gen.osc.PitchEnvelopeDoublesFundamental",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenOscPitchEnvelopeTest::RunTest(const FString& Parameters)
{
    using namespace PwGenOscNoiseTestHelpers;

    TArray<FPwSynthPitchPoint> PitchEnvelope;
    PitchEnvelope.Add({ 0.0, 0.0 });
    PitchEnvelope.Add({ 100.0, 0.0 });
    PitchEnvelope.Add({ 900.0, 12.0 });
    PitchEnvelope.Add({ 1000.0, 12.0 });

    TArray<float> Samples;
    FString ErrorCode;
    FString Error;
    const bool bRendered = RenderOsc(Samples, MakeOscParams(TEXT("sine"), 468.75),
        TestSampleRate, /*Seed*/ 1, ErrorCode, Error, PitchEnvelope);
    TestTrue(FString::Printf(TEXT("PwGenOsc succeeded (%s: %s)"), *ErrorCode, *Error), bRendered);
    if (!bRendered)
    {
        return false;
    }

    FPwStftResult Result;
    FString Why;
    const bool bAnalyzed = Analyze(Samples, 2048, 256, Result, Why);
    if (!TestTrue(FString::Printf(TEXT("PwComputeStft succeeded (%s)"), *Why), bAnalyzed))
    {
        return false;
    }
    TestTrue(TEXT("The analysis has at least two frames"), Result.NumFrames >= 2);

    const int32 FirstPeakBin = FindPeakBin(Result, 0);
    const int32 LastPeakBin = FindPeakBin(Result, Result.NumFrames - 1);

    TestEqual(TEXT("The first window peaks at 468.75 Hz (bin 20)"), FirstPeakBin, 20);
    TestEqual(TEXT("The last window peaks at 937.5 Hz (bin 40), one octave up"), LastPeakBin, 40);

    const float Ratio = (FirstPeakBin > 0) ? (static_cast<float>(LastPeakBin) / static_cast<float>(FirstPeakBin)) : 0.f;
    TestTrue(FString::Printf(TEXT("Measured f0 ratio is %.3f, expected 2.0"), Ratio),
        FMath::IsNearlyEqual(Ratio, 2.f, 0.1f));

    return true;
}

// =============================================================================================
// osc E. Unison does not change level.
//
// The 1/N sum is exactly level-preserving for a coherent stack, which is what a zero-spread
// unison is by design (scattering the phases of N identical waveforms would build a comb filter
// instead of a thicker tone). The zero-spread case is therefore the tight assertion; the detuned
// case asserts the weaker property that still has to hold - a detuned stack sits AT or BELOW the
// single voice, never above it.
//
// The unison-1 render also crosses the FSinOsc2DRotation fast path while unison-4 goes through
// the general per-voice path, so this doubles as a check that the two paths agree on level.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenOscUnisonPreservesLevelTest,
    "PinWright.audio.gen.osc.UnisonPreservesLevel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenOscUnisonPreservesLevelTest::RunTest(const FString& Parameters)
{
    using namespace PwGenOscNoiseTestHelpers;

    FString ErrorCode;
    FString Error;

    TArray<float> Single;
    const bool bSingle = RenderOsc(Single, MakeOscParams(TEXT("sine"), 1000.0),
        TestSampleRate, /*Seed*/ 7, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("unison 1 rendered (%s: %s)"), *ErrorCode, *Error), bSingle);

    FPwSynthParams CoherentParams = MakeOscParams(TEXT("sine"), 1000.0);
    SetNumber(CoherentParams, TEXT("unison"), 4.0);
    SetNumber(CoherentParams, TEXT("unisonSpreadCents"), 0.0);

    TArray<float> Coherent;
    const bool bCoherent = RenderOsc(Coherent, CoherentParams, TestSampleRate, /*Seed*/ 7, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("unison 4 / spread 0 rendered (%s: %s)"), *ErrorCode, *Error), bCoherent);

    if (!bSingle || !bCoherent)
    {
        return false;
    }

    const float SingleDb = ToDb(PeakAbs(Single));
    const float CoherentDb = ToDb(PeakAbs(Coherent));
    TestTrue(FString::Printf(TEXT("unison 4 peaks at %.4f dB vs %.4f dB for unison 1 (delta %.4f dB)"),
        CoherentDb, SingleDb, CoherentDb - SingleDb),
        FMath::Abs(CoherentDb - SingleDb) <= 0.1f);

    // Detuned: the stack can only lose peak level to phase drift, never gain it.
    FPwSynthParams DetunedParams = MakeOscParams(TEXT("sine"), 1000.0);
    SetNumber(DetunedParams, TEXT("unison"), 4.0);
    SetNumber(DetunedParams, TEXT("unisonSpreadCents"), 40.0);

    TArray<float> Detuned;
    const bool bDetuned = RenderOsc(Detuned, DetunedParams, TestSampleRate, /*Seed*/ 7, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("unison 4 / spread 40 rendered (%s: %s)"), *ErrorCode, *Error), bDetuned);
    if (bDetuned)
    {
        const float DetunedDb = ToDb(PeakAbs(Detuned));
        TestTrue(FString::Printf(TEXT("detuned unison peaks at %.3f dB, at or below the %.3f dB single voice"),
            DetunedDb, SingleDb), DetunedDb <= SingleDb + 0.1f);
        TestTrue(FString::Printf(TEXT("detuned unison peaks at %.3f dB, not collapsed to silence"), DetunedDb),
            DetunedDb >= SingleDb - 12.f);
    }

    return true;
}

// =============================================================================================
// osc F. Determinism: same params + same seed is byte-identical; a different seed is not.
//
// unison + spread is the osc's only stochastic input (per-voice phase scatter), so the render is
// made to use it. The "different seed differs" half matters as much as the identical half: a
// generator that ignored the seed entirely would pass the first assertion trivially.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenOscDeterminismTest,
    "PinWright.audio.gen.osc.SameSeedIsByteIdentical",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenOscDeterminismTest::RunTest(const FString& Parameters)
{
    using namespace PwGenOscNoiseTestHelpers;

    FPwSynthParams Params = MakeOscParams(TEXT("saw"), 220.0);
    SetNumber(Params, TEXT("unison"), 4.0);
    SetNumber(Params, TEXT("unisonSpreadCents"), 50.0);

    constexpr int32 NumFrames = 8192;
    FString ErrorCode;
    FString Error;

    TArray<float> First;
    TArray<float> Second;
    TArray<float> Other;
    const bool bFirst = RenderOsc(First, Params, NumFrames, /*Seed*/ 4242, ErrorCode, Error);
    const bool bSecond = RenderOsc(Second, Params, NumFrames, /*Seed*/ 4242, ErrorCode, Error);
    const bool bOther = RenderOsc(Other, Params, NumFrames, /*Seed*/ 4243, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("all three renders succeeded (%s: %s)"), *ErrorCode, *Error),
        bFirst && bSecond && bOther);
    if (!bFirst || !bSecond || !bOther)
    {
        return false;
    }

    TestTrue(TEXT("Seed 4242 twice is byte-identical"), BuffersAreIdentical(First, Second));
    TestFalse(TEXT("Seed 4243 differs from seed 4242"), BuffersAreIdentical(First, Other));

    return true;
}

// =============================================================================================
// noise A. Pink falls ~3 dB per octave; white does not.
//
// Measured as mean power PER BIN so the tilt is visible (see MeanBandPowerDb). Five octave bands
// spanning 250 Hz to 8 kHz - a decade and a half - give four independent slope readings each.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenNoiseSpectralTiltTest,
    "PinWright.audio.gen.noise.PinkFallsThreeDbPerOctave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenNoiseSpectralTiltTest::RunTest(const FString& Parameters)
{
    using namespace PwGenOscNoiseTestHelpers;

    constexpr int32 NumFrames = TestSampleRate * 2;     // 2 s: ~46 independent blocks per band
    static const double BandEdgesHz[] = { 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0 };
    constexpr int32 NumBands = UE_ARRAY_COUNT(BandEdgesHz) - 1;

    FString ErrorCode;
    FString Error;

    TArray<float> Pink;
    TArray<float> White;
    const bool bPink = RenderNoise(Pink, MakeNoiseParams(TEXT("pink")), NumFrames, /*Seed*/ 11, ErrorCode, Error);
    const bool bWhite = RenderNoise(White, MakeNoiseParams(TEXT("white")), NumFrames, /*Seed*/ 11, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("both noise renders succeeded (%s: %s)"), *ErrorCode, *Error), bPink && bWhite);
    if (!bPink || !bWhite)
    {
        return false;
    }

    FPwStftResult PinkResult;
    FPwStftResult WhiteResult;
    FString PinkWhy;
    FString WhiteWhy;
    const bool bPinkAnalyzed = Analyze(Pink, 2048, 256, PinkResult, PinkWhy);
    const bool bWhiteAnalyzed = Analyze(White, 2048, 256, WhiteResult, WhiteWhy);
    if (!TestTrue(FString::Printf(TEXT("pink STFT succeeded (%s)"), *PinkWhy), bPinkAnalyzed)
        || !TestTrue(FString::Printf(TEXT("white STFT succeeded (%s)"), *WhiteWhy), bWhiteAnalyzed))
    {
        return false;
    }

    float PinkBandDb[NumBands];
    float WhiteBandDb[NumBands];
    for (int32 Band = 0; Band < NumBands; ++Band)
    {
        PinkBandDb[Band] = MeanBandPowerDb(PinkResult, BandEdgesHz[Band], BandEdgesHz[Band + 1]);
        WhiteBandDb[Band] = MeanBandPowerDb(WhiteResult, BandEdgesHz[Band], BandEdgesHz[Band + 1]);
    }

    for (int32 Step = 1; Step < NumBands; ++Step)
    {
        const float PinkSlope = PinkBandDb[Step] - PinkBandDb[Step - 1];
        const float WhiteSlope = WhiteBandDb[Step] - WhiteBandDb[Step - 1];

        TestTrue(FString::Printf(
            TEXT("Pink %.0f-%.0f Hz -> %.0f-%.0f Hz slope is %.2f dB/octave, expected ~-3"),
            BandEdgesHz[Step - 1], BandEdgesHz[Step], BandEdgesHz[Step], BandEdgesHz[Step + 1], PinkSlope),
            PinkSlope <= -1.5f && PinkSlope >= -4.5f);

        TestTrue(FString::Printf(
            TEXT("White %.0f-%.0f Hz -> %.0f-%.0f Hz slope is %.2f dB/octave, expected ~0"),
            BandEdgesHz[Step - 1], BandEdgesHz[Step], BandEdgesHz[Step], BandEdgesHz[Step + 1], WhiteSlope),
            FMath::Abs(WhiteSlope) <= 1.2f);
    }

    // The end-to-end drop is the part that cannot be produced by a mislabelled white generator.
    const float PinkTotal = PinkBandDb[NumBands - 1] - PinkBandDb[0];
    const float WhiteTotal = WhiteBandDb[NumBands - 1] - WhiteBandDb[0];
    TestTrue(FString::Printf(TEXT("Pink drops %.2f dB across 250 Hz -> 8 kHz, expected ~-12"), PinkTotal),
        PinkTotal <= -8.f && PinkTotal >= -16.f);
    TestTrue(FString::Printf(TEXT("White drops %.2f dB across the same span, expected ~0"), WhiteTotal),
        FMath::Abs(WhiteTotal) <= 2.f);

    return true;
}

// =============================================================================================
// noise B. The band limits actually run.
//
// The generator skips a filter stage that is parked at the extreme of its documented range. That
// optimisation's failure mode is a stage that never runs at all, which is invisible in every
// "did it render" check - so this asserts the filtered render really has lost its low end.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenNoiseBandLimitTest,
    "PinWright.audio.gen.noise.BandLimitsNarrowTheSpectrum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenNoiseBandLimitTest::RunTest(const FString& Parameters)
{
    using namespace PwGenOscNoiseTestHelpers;

    constexpr int32 NumFrames = TestSampleRate;
    FString ErrorCode;
    FString Error;

    FPwSynthParams FilteredParams = MakeNoiseParams(TEXT("white"));
    SetNumber(FilteredParams, TEXT("lowCutHz"), 2000.0);
    SetNumber(FilteredParams, TEXT("highCutHz"), 4000.0);

    TArray<float> Wide;
    TArray<float> Narrow;
    const bool bWide = RenderNoise(Wide, MakeNoiseParams(TEXT("white")), NumFrames, /*Seed*/ 5, ErrorCode, Error);
    const bool bNarrow = RenderNoise(Narrow, FilteredParams, NumFrames, /*Seed*/ 5, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("both noise renders succeeded (%s: %s)"), *ErrorCode, *Error), bWide && bNarrow);
    if (!bWide || !bNarrow)
    {
        return false;
    }

    FPwStftResult WideResult;
    FPwStftResult NarrowResult;
    FString WideWhy;
    FString NarrowWhy;
    const bool bWideAnalyzed = Analyze(Wide, 2048, 256, WideResult, WideWhy);
    const bool bNarrowAnalyzed = Analyze(Narrow, 2048, 256, NarrowResult, NarrowWhy);
    if (!TestTrue(FString::Printf(TEXT("wide STFT succeeded (%s)"), *WideWhy), bWideAnalyzed)
        || !TestTrue(FString::Printf(TEXT("narrow STFT succeeded (%s)"), *NarrowWhy), bNarrowAnalyzed))
    {
        return false;
    }

    // Two to three octaves under the 2 kHz highpass corner, where a 2-pole stage is 24-36 dB down.
    const float WideLowDb = MeanBandPowerDb(WideResult, 250.0, 500.0);
    const float NarrowLowDb = MeanBandPowerDb(NarrowResult, 250.0, 500.0);
    TestTrue(FString::Printf(TEXT("250-500 Hz drops %.2f dB under a 2 kHz lowCut, require >= 20"),
        WideLowDb - NarrowLowDb), (WideLowDb - NarrowLowDb) >= 20.f);

    // Two octaves and up over the 4 kHz lowpass corner.
    const float WideHighDb = MeanBandPowerDb(WideResult, 16000.0, 20000.0);
    const float NarrowHighDb = MeanBandPowerDb(NarrowResult, 16000.0, 20000.0);
    TestTrue(FString::Printf(TEXT("16-20 kHz drops %.2f dB under a 4 kHz highCut, require >= 20"),
        WideHighDb - NarrowHighDb), (WideHighDb - NarrowHighDb) >= 20.f);

    // The passband survives: a filter pair that killed everything would pass both checks above.
    const float WidePassDb = MeanBandPowerDb(WideResult, 2500.0, 3500.0);
    const float NarrowPassDb = MeanBandPowerDb(NarrowResult, 2500.0, 3500.0);
    TestTrue(FString::Printf(TEXT("2.5-3.5 kHz passband is within 6 dB of unfiltered (%.2f dB)"),
        NarrowPassDb - WidePassDb), FMath::Abs(NarrowPassDb - WidePassDb) <= 6.f);

    return true;
}

// =============================================================================================
// noise C. Determinism, and the seeded-constructor rule specifically.
//
// Audio::FWhiteNoise / FPinkNoise default-construct from FPlatformTime::Cycles(). If a colour
// path ever regresses to the default constructor, two renders with the same seed stop matching -
// which is the ONLY externally visible symptom that failure has.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenNoiseDeterminismTest,
    "PinWright.audio.gen.noise.SameSeedIsByteIdentical",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenNoiseDeterminismTest::RunTest(const FString& Parameters)
{
    using namespace PwGenOscNoiseTestHelpers;

    static const TCHAR* Colors[] = { TEXT("white"), TEXT("pink"), TEXT("brown"), TEXT("blue"), TEXT("violet") };
    constexpr int32 NumFrames = 8192;

    for (const TCHAR* Color : Colors)
    {
        const FPwSynthParams Params = MakeNoiseParams(Color);

        FString ErrorCode;
        FString Error;
        TArray<float> First;
        TArray<float> Second;
        TArray<float> Other;
        const bool bFirst = RenderNoise(First, Params, NumFrames, /*Seed*/ 909, ErrorCode, Error);
        const bool bSecond = RenderNoise(Second, Params, NumFrames, /*Seed*/ 909, ErrorCode, Error);
        const bool bOther = RenderNoise(Other, Params, NumFrames, /*Seed*/ 910, ErrorCode, Error);
        if (!TestTrue(FString::Printf(TEXT("'%s' rendered three times (%s: %s)"), Color, *ErrorCode, *Error),
            bFirst && bSecond && bOther))
        {
            continue;
        }

        TestTrue(FString::Printf(TEXT("'%s' seed 909 twice is byte-identical"), Color),
            BuffersAreIdentical(First, Second));
        TestFalse(FString::Printf(TEXT("'%s' seed 910 differs from seed 909"), Color),
            BuffersAreIdentical(First, Other));
        TestTrue(FString::Printf(TEXT("'%s' produced signal, not silence"), Color), PeakAbs(First) > 1e-4f);
    }

    return true;
}

// =============================================================================================
// osc / noise failure direction (rpc-design.md §12).
//
// Every rejection is checked for the SPECIFIC error code AND for an untouched output buffer. The
// sentinel is the load-bearing half: a generator that validated lazily would still return false
// with the right code after having already written part of the layer, and the caller would be
// left holding a buffer that is neither the previous contents nor a finished render.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenOscRejectsBadParamsTest,
    "PinWright.audio.gen.osc.RejectsUnknownWaveform",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenOscRejectsBadParamsTest::RunTest(const FString& Parameters)
{
    using namespace PwGenOscNoiseTestHelpers;

    constexpr int32 NumFrames = 512;
    const TArray<FPwSynthPitchPoint> NoPitch;
    const FPwSynthModulation NoModulation;

    // Unknown waveform.
    {
        TArray<float> Buffer = MakeBuffer(NumFrames, Sentinel);
        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenOsc(MakeOscParams(TEXT("sawtooth"), 1000.0), NoPitch, NoModulation,
            TestSampleRate, Rng, Buffer, ErrorCode, Error);

        TestFalse(TEXT("An unknown waveform is rejected"), bRendered);
        TestEqual(TEXT("Unknown waveform reports INVALID_PARAMS"), ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("Unknown waveform left the buffer untouched"), AllEqual(Buffer, Sentinel));
        TestTrue(TEXT("Unknown waveform names the offending token"), Error.Contains(TEXT("sawtooth")));
    }

    // Missing required frequencyHz. There is no safe default for a pitch (rpc-design §3).
    {
        FPwSynthParams Params;
        SetToken(Params, TEXT("waveform"), TEXT("sine"));

        TArray<float> Buffer = MakeBuffer(NumFrames, Sentinel);
        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenOsc(Params, NoPitch, NoModulation, TestSampleRate, Rng, Buffer,
            ErrorCode, Error);

        TestFalse(TEXT("A missing frequencyHz is rejected"), bRendered);
        TestEqual(TEXT("Missing frequencyHz reports INVALID_PARAMS"), ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("Missing frequencyHz left the buffer untouched"), AllEqual(Buffer, Sentinel));
    }

    // Out-of-range value: a clamp here would render a pitch the caller never asked for.
    {
        FPwSynthParams Params = MakeOscParams(TEXT("sine"), 1000.0);
        SetNumber(Params, TEXT("unison"), 99.0);

        TArray<float> Buffer = MakeBuffer(NumFrames, Sentinel);
        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenOsc(Params, NoPitch, NoModulation, TestSampleRate, Rng, Buffer,
            ErrorCode, Error);

        TestFalse(TEXT("An out-of-range unison is rejected rather than clamped"), bRendered);
        TestEqual(TEXT("Out-of-range unison reports INVALID_PARAMS"), ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("Out-of-range unison left the buffer untouched"), AllEqual(Buffer, Sentinel));
    }

    // A zero-length render is an error, not a zero-length success.
    {
        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenOsc(MakeOscParams(TEXT("sine"), 1000.0), NoPitch, NoModulation,
            TestSampleRate, Rng, TArrayView<float>(), ErrorCode, Error);

        TestFalse(TEXT("A zero-length buffer is rejected"), bRendered);
        TestEqual(TEXT("Zero-length reports AUDIO_EMPTY_BUFFER"), ErrorCode,
            FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenOscRejectsBadModulationTest,
    "PinWright.audio.gen.osc.RejectsUnknownModulationSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenOscRejectsBadModulationTest::RunTest(const FString& Parameters)
{
    using namespace PwGenOscNoiseTestHelpers;

    constexpr int32 NumFrames = 512;
    const TArray<FPwSynthPitchPoint> NoPitch;

    // Unknown modulator shape. The enum is closed at the type level, so the only caller that can
    // present one is a caller that built the struct by hand - exactly the one a silent fallback
    // to sine would mislead.
    {
        FPwSynthModulation Modulation;
        Modulation.Routing = EPwSynthModulationRouting::Am;
        Modulation.Depth = 0.5;
        Modulation.RateHz = 5.0;
        Modulation.Source = static_cast<EPwSynthModSource>(99);

        TArray<float> Buffer = MakeBuffer(NumFrames, Sentinel);
        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenOsc(MakeOscParams(TEXT("sine"), 1000.0), NoPitch, Modulation,
            TestSampleRate, Rng, Buffer, ErrorCode, Error);

        TestFalse(TEXT("An unknown modulator source is rejected"), bRendered);
        TestEqual(TEXT("Unknown modulator source reports INVALID_PARAMS"), ErrorCode,
            FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("Unknown modulator source left the buffer untouched"), AllEqual(Buffer, Sentinel));
    }

    // Unknown routing.
    {
        FPwSynthModulation Modulation;
        Modulation.Routing = static_cast<EPwSynthModulationRouting>(77);
        Modulation.Depth = 0.5;
        Modulation.RateHz = 5.0;

        TArray<float> Buffer = MakeBuffer(NumFrames, Sentinel);
        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenOsc(MakeOscParams(TEXT("sine"), 1000.0), NoPitch, Modulation,
            TestSampleRate, Rng, Buffer, ErrorCode, Error);

        TestFalse(TEXT("An unknown modulation routing is rejected"), bRendered);
        TestEqual(TEXT("Unknown routing reports INVALID_PARAMS"), ErrorCode,
            FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("Unknown routing left the buffer untouched"), AllEqual(Buffer, Sentinel));
    }

    // A valid am block still renders, and audibly moves the amplitude around unity.
    {
        FPwSynthModulation Modulation;
        Modulation.Routing = EPwSynthModulationRouting::Am;
        Modulation.Depth = 1.0;
        Modulation.RateHz = 4.0;
        Modulation.Source = EPwSynthModSource::Sine;

        TArray<float> Buffer = MakeBuffer(TestSampleRate);
        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenOsc(MakeOscParams(TEXT("sine"), 1000.0), NoPitch, Modulation,
            TestSampleRate, Rng, Buffer, ErrorCode, Error);
        TestTrue(FString::Printf(TEXT("A valid am block renders (%s: %s)"), *ErrorCode, *Error), bRendered);

        if (bRendered)
        {
            // `am` is a dry/wet blend, scale = lerp(1, 0.5 + 0.5*m, depth), so depth 1 swings the
            // amplitude across 0..1 and NEVER above unity. The upper bound is the load-bearing
            // half: the rejected alternative (1 + depth*m) would peak at 2.0 here, i.e. an
            // amplitude modulator quietly adding 6 dB ahead of the master chain.
            TestTrue(FString::Printf(TEXT("am depth 1 reaches unity but does not boost past it (peak %.3f)"),
                PeakAbs(Buffer)),
                PeakAbs(Buffer) > 0.9f && PeakAbs(Buffer) <= 1.0f);

            float QuietestWindowPeak = TNumericLimits<float>::Max();
            constexpr int32 WindowFrames = 1024;
            for (int32 Start = 0; Start + WindowFrames <= Buffer.Num(); Start += WindowFrames)
            {
                float WindowPeak = 0.f;
                for (int32 Index = Start; Index < Start + WindowFrames; ++Index)
                {
                    WindowPeak = FMath::Max(WindowPeak, FMath::Abs(Buffer[Index]));
                }
                QuietestWindowPeak = FMath::Min(QuietestWindowPeak, WindowPeak);
            }
            TestTrue(FString::Printf(TEXT("am depth 1 reaches near silence (quietest window peak %.4f)"),
                QuietestWindowPeak), QuietestWindowPeak < 0.35f);
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenNoiseRejectsBadParamsTest,
    "PinWright.audio.gen.noise.RejectsUnknownColor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenNoiseRejectsBadParamsTest::RunTest(const FString& Parameters)
{
    using namespace PwGenOscNoiseTestHelpers;

    constexpr int32 NumFrames = 512;
    const TArray<FPwSynthPitchPoint> NoPitch;
    const FPwSynthModulation NoModulation;

    // Unknown colour.
    {
        TArray<float> Buffer = MakeBuffer(NumFrames, Sentinel);
        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenNoise(MakeNoiseParams(TEXT("chartreuse")), NoPitch, NoModulation,
            TestSampleRate, Rng, Buffer, ErrorCode, Error);

        TestFalse(TEXT("An unknown noise colour is rejected"), bRendered);
        TestEqual(TEXT("Unknown colour reports INVALID_PARAMS"), ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("Unknown colour left the buffer untouched"), AllEqual(Buffer, Sentinel));
        TestTrue(TEXT("Unknown colour names the offending token"), Error.Contains(TEXT("chartreuse")));
    }

    // Missing required colour.
    {
        FPwSynthParams Params;
        TArray<float> Buffer = MakeBuffer(NumFrames, Sentinel);
        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenNoise(Params, NoPitch, NoModulation, TestSampleRate, Rng, Buffer,
            ErrorCode, Error);

        TestFalse(TEXT("A missing colour is rejected"), bRendered);
        TestEqual(TEXT("Missing colour reports INVALID_PARAMS"), ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("Missing colour left the buffer untouched"), AllEqual(Buffer, Sentinel));
    }

    // An inverted band leaves no passband; rendering it would report success over silence.
    {
        FPwSynthParams Params = MakeNoiseParams(TEXT("white"));
        SetNumber(Params, TEXT("lowCutHz"), 9000.0);
        SetNumber(Params, TEXT("highCutHz"), 300.0);

        TArray<float> Buffer = MakeBuffer(NumFrames, Sentinel);
        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenNoise(Params, NoPitch, NoModulation, TestSampleRate, Rng, Buffer,
            ErrorCode, Error);

        TestFalse(TEXT("An inverted lowCut/highCut pair is rejected"), bRendered);
        TestEqual(TEXT("Inverted band reports INVALID_PARAMS"), ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("Inverted band left the buffer untouched"), AllEqual(Buffer, Sentinel));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenNoiseRejectsPitchRoutingsTest,
    "PinWright.audio.gen.noise.RejectsPitchAndFmRoutings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenNoiseRejectsPitchRoutingsTest::RunTest(const FString& Parameters)
{
    using namespace PwGenOscNoiseTestHelpers;

    constexpr int32 NumFrames = 512;
    const TArray<FPwSynthPitchPoint> NoPitch;

    // fm has no instantaneous frequency to act on. Accepting it would render an unmodulated layer
    // while reporting a modulated one.
    {
        FPwSynthModulation Modulation;
        Modulation.Routing = EPwSynthModulationRouting::Fm;
        Modulation.Depth = 2.0;
        Modulation.RateHz = 100.0;

        TArray<float> Buffer = MakeBuffer(NumFrames, Sentinel);
        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenNoise(MakeNoiseParams(TEXT("white")), NoPitch, Modulation,
            TestSampleRate, Rng, Buffer, ErrorCode, Error);

        TestFalse(TEXT("fm on a noise generator is rejected"), bRendered);
        TestEqual(TEXT("fm on noise reports INVALID_PARAMS"), ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("fm on noise left the buffer untouched"), AllEqual(Buffer, Sentinel));
    }

    // Same reasoning for a pitch envelope that actually moves.
    {
        TArray<FPwSynthPitchPoint> PitchEnvelope;
        PitchEnvelope.Add({ 0.0, 0.0 });
        PitchEnvelope.Add({ 100.0, 12.0 });

        TArray<float> Buffer = MakeBuffer(NumFrames, Sentinel);
        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenNoise(MakeNoiseParams(TEXT("white")), PitchEnvelope,
            FPwSynthModulation(), TestSampleRate, Rng, Buffer, ErrorCode, Error);

        TestFalse(TEXT("A moving pitch envelope on a noise generator is rejected"), bRendered);
        TestEqual(TEXT("Pitch envelope on noise reports INVALID_PARAMS"), ErrorCode,
            FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("Pitch envelope on noise left the buffer untouched"), AllEqual(Buffer, Sentinel));
    }

    // A flat-zero envelope changes nothing, so it is accepted rather than rejected pedantically.
    {
        TArray<FPwSynthPitchPoint> FlatEnvelope;
        FlatEnvelope.Add({ 0.0, 0.0 });
        FlatEnvelope.Add({ 100.0, 0.0 });

        TArray<float> Buffer = MakeBuffer(NumFrames);
        FPwSeededRandom Rng(1);
        FString ErrorCode;
        FString Error;
        const bool bRendered = PwGenNoise(MakeNoiseParams(TEXT("white")), FlatEnvelope,
            FPwSynthModulation(), TestSampleRate, Rng, Buffer, ErrorCode, Error);

        TestTrue(FString::Printf(TEXT("A flat-zero pitch envelope is accepted (%s: %s)"), *ErrorCode, *Error),
            bRendered);
    }

    return true;
}
