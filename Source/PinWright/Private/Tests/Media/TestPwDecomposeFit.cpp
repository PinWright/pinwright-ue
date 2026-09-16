// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the back half of the decomposition analyzer: PwFitModes and PwAnalyzeResidual.
//
// The modal-fit tests are against ANALYTIC ground truth, not against "it returned true". Every
// track is synthesised from A(t) = A0 * exp(-ln(1000) * t / T60), which is PwGenModal's own decay
// law, so the expected DecayMs and InitialGainDb are exact numbers rather than estimates - and
// the convention test builds its track from the generator's literal pole-radius line, so a
// tau-versus-T60 confusion (a factor of 6.9) cannot pass.
//
// Per rpc-design.md §6 both directions of every decision are asserted: a decaying track must be
// measured AND a sustained one must not, AND a growing one must come back with the sign kept
// rather than as a large positive decay; the residual must be near-silent for a signal the
// partials fully explain AND must retain noise the partials do not. Per §12 the failure direction
// is asserted for both entry points, including the ordering trap the sibling analysers hit - a
// track of NaN must be named non-finite, never measured as silence (FMath::Max(0.0, NaN) is 0).

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioDecompose.h"
#include "AudioGen/PwStft.h"
#include "Handlers/ErrorCodes.h"

#include "Math/RandomStream.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true.
namespace PwDecomposeFitTestHelpers
{
    constexpr int32 TestSampleRate = 48000;

    /** ln(1000): the amplitude ratio PwGenModal's modeDecaysMs spans. */
    constexpr double Ln1000 = 6.907755278982137;

    /** STFT geometry for the residual tests. 1024/256 at 48 kHz is 46.875 Hz bins on a 5.33 ms grid. */
    constexpr int32 ResidualFftSize = 1024;
    constexpr int32 ResidualHopSize = 256;

    /**
     * A quiet NaN built from its bit pattern rather than from <limits>, matching the MakeInfinity
     * helper in TestPwAudioAnalysis.cpp: exponent all ones with a non-zero mantissa.
     */
    double MakeNaNDouble()
    {
        constexpr uint64 QuietNaNBits = 0x7FF8000000000000ull;
        double Value = 0.0;
        FMemory::Memcpy(&Value, &QuietNaNBits, sizeof(Value));
        return Value;
    }

    float MakeNaNFloat()
    {
        constexpr uint32 QuietNaNBits = 0x7FC00000u;
        float Value = 0.f;
        FMemory::Memcpy(&Value, &QuietNaNBits, sizeof(Value));
        return Value;
    }

    /** Fills the summary fields a real tracker would have derived from the points. */
    void FinishTrack(FPwPartialTrack& Track)
    {
        if (Track.Points.Num() == 0)
        {
            return;
        }
        Track.StartMs = Track.Points[0].TimeMs;
        Track.EndMs = Track.Points.Last().TimeMs;

        double SumFreq = 0.0;
        double PeakAmp = 0.0;
        for (const FPwPartialPoint& Point : Track.Points)
        {
            SumFreq += Point.FreqHz;
            PeakAmp = FMath::Max(PeakAmp, Point.AmpLinear);
        }
        Track.MeanFreqHz = SumFreq / static_cast<double>(Track.Points.Num());
        Track.PeakAmpLinear = PeakAmp;
        // Signed end-minus-start, per FPwPartialTrack::FreqDriftHz - not a magnitude.
        Track.FreqDriftHz = Track.Points.Last().FreqHz - Track.Points[0].FreqHz;
    }

    /**
     * A track whose amplitude is exactly A0 * exp(-ln(1000) * t / T60Ms).
     *
     * A NEGATIVE T60Ms makes it grow at the same rate, which is how the growing-partial test is
     * built: the fit's signed answer for such a track is exactly -T60Ms, so one helper covers both
     * directions with the same arithmetic.
     */
    FPwPartialTrack MakeExponentialTrack(double FreqHz, double A0, double T60Ms,
                                        double StepMs, double DurationMs)
    {
        FPwPartialTrack Track;
        const int32 NumPoints = FMath::FloorToInt32(DurationMs / StepMs) + 1;
        Track.Points.Reserve(NumPoints);
        for (int32 Index = 0; Index < NumPoints; ++Index)
        {
            const double TimeMs = static_cast<double>(Index) * StepMs;
            FPwPartialPoint& Point = Track.Points.AddDefaulted_GetRef();
            Point.TimeMs = TimeMs;
            Point.FreqHz = FreqHz;
            Point.AmpLinear = A0 * FMath::Exp(-Ln1000 * TimeMs / T60Ms);
        }
        FinishTrack(Track);
        return Track;
    }

    /** Same shape, plus additive uniform noise - the noise model the A^2 weighting assumes. */
    FPwPartialTrack MakeNoisyExponentialTrack(double FreqHz, double A0, double T60Ms, double StepMs,
                                             double DurationMs, double NoiseAmp, int32 Seed)
    {
        FPwPartialTrack Track = MakeExponentialTrack(FreqHz, A0, T60Ms, StepMs, DurationMs);
        FRandomStream Rng(Seed);
        for (FPwPartialPoint& Point : Track.Points)
        {
            const double Noise = NoiseAmp * (2.0 * static_cast<double>(Rng.GetFraction()) - 1.0);
            Point.AmpLinear = FMath::Max(1.0e-12, Point.AmpLinear + Noise);
        }
        FinishTrack(Track);
        return Track;
    }

    FPwPartialTrack MakeConstantTrack(double FreqHz, double AmpLinear, double StepMs, double DurationMs)
    {
        FPwPartialTrack Track;
        const int32 NumPoints = FMath::FloorToInt32(DurationMs / StepMs) + 1;
        Track.Points.Reserve(NumPoints);
        for (int32 Index = 0; Index < NumPoints; ++Index)
        {
            FPwPartialPoint& Point = Track.Points.AddDefaulted_GetRef();
            Point.TimeMs = static_cast<double>(Index) * StepMs;
            Point.FreqHz = FreqHz;
            Point.AmpLinear = AmpLinear;
        }
        FinishTrack(Track);
        return Track;
    }

    /**
     * The structural guarantee PwFitModes documents: bMeasured is true exactly when DecayMs is
     * positive, so a caller that ignores the flag still cannot feed an unmeasured row into
     * PwGenModal (which rejects modeDecaysMs <= 0). Asserted in every fit test rather than once.
     */
    void CheckDecaySignInvariant(FAutomationTestBase& Test, const TArray<FPwModalFit>& Fits)
    {
        for (int32 Index = 0; Index < Fits.Num(); ++Index)
        {
            Test.TestTrue(FString::Printf(
                TEXT("Fit %d: bMeasured (%d) agrees with DecayMs > 0 (%.6f)"),
                Index, Fits[Index].bMeasured ? 1 : 0, Fits[Index].DecayMs),
                Fits[Index].bMeasured == (Fits[Index].DecayMs > 0.0));
        }
    }

    TArray<float> MakeSine(double DurationSeconds, double Hz, double PeakAmplitude)
    {
        const int32 NumSamples = FMath::RoundToInt32(DurationSeconds * TestSampleRate);
        TArray<float> Samples;
        Samples.SetNumZeroed(NumSamples);
        const double AngularStep = 2.0 * UE_DOUBLE_PI * Hz / static_cast<double>(TestSampleRate);
        for (int32 Index = 0; Index < NumSamples; ++Index)
        {
            Samples[Index] = static_cast<float>(PeakAmplitude * FMath::Sin(AngularStep * Index));
        }
        return Samples;
    }

    TArray<float> MakeSilentBuffer(double DurationSeconds)
    {
        TArray<float> Samples;
        Samples.SetNumZeroed(FMath::RoundToInt32(DurationSeconds * TestSampleRate));
        return Samples;
    }

    void AddWhiteNoise(TArray<float>& Samples, double PeakAmplitude, int32 Seed)
    {
        FRandomStream Rng(Seed);
        for (float& Sample : Samples)
        {
            Sample += static_cast<float>(PeakAmplitude * (2.0 * static_cast<double>(Rng.GetFraction()) - 1.0));
        }
    }

    /** Multiplies the buffer by exp(-ln(1000) * t / T60Ms), so every band envelope is a dB ramp. */
    void ApplyExponentialGain(TArray<float>& Samples, double T60Ms)
    {
        for (int32 Index = 0; Index < Samples.Num(); ++Index)
        {
            const double TimeMs = 1000.0 * static_cast<double>(Index) / static_cast<double>(TestSampleRate);
            Samples[Index] = static_cast<float>(static_cast<double>(Samples[Index])
                * FMath::Exp(-Ln1000 * TimeMs / T60Ms));
        }
    }

    bool ComputeResidualStft(const TArray<float>& Samples, FPwStftResult& Out, FString& OutMessage)
    {
        FPwStftSettings Settings;
        Settings.FftSize = ResidualFftSize;
        Settings.HopSize = ResidualHopSize;
        FPwStftError Error;
        const bool bOk = PwComputeStft(Samples, TestSampleRate, Settings, Out, &Error);
        OutMessage = FString::Printf(TEXT("%s: %s"), *Error.Code, *Error.Message);
        return bOk;
    }

    /**
     * A steady partial sampled on the spectrogram's own frame-centre grid, extended one frame past
     * each end so no frame falls outside the track's span through floating-point equality.
     */
    FPwPartialTrack MakeFrameAlignedTrack(const FPwStftResult& Stft, double FreqHz, double AmpLinear)
    {
        const double MsPerFrame = 1000.0 * static_cast<double>(Stft.HopSize) / static_cast<double>(TestSampleRate);
        const double OffsetMs = 1000.0 * 0.5 * static_cast<double>(Stft.FftSize) / static_cast<double>(TestSampleRate);

        FPwPartialTrack Track;
        Track.Points.Reserve(Stft.NumFrames + 2);
        for (int32 Frame = -1; Frame <= Stft.NumFrames; ++Frame)
        {
            FPwPartialPoint& Point = Track.Points.AddDefaulted_GetRef();
            Point.TimeMs = static_cast<double>(Frame) * MsPerFrame + OffsetMs;
            Point.FreqHz = FreqHz;
            Point.AmpLinear = AmpLinear;
        }
        FinishTrack(Track);
        return Track;
    }

    FPwDecomposeSettings MakeResidualSettings(int32 NumBands, double SimplifyToleranceDb)
    {
        FPwDecomposeSettings Settings;
        Settings.ResidualBands = NumBands;
        Settings.SimplifyToleranceDb = SimplifyToleranceDb;
        return Settings;
    }

    /** Loudest envelope point in a band, in dB. Returns the floor sentinel for an empty band. */
    double BandMaxDb(const FPwResidualBand& Band)
    {
        double Max = -1000.0;
        for (const FPwDbPoint& Point : Band.EnvelopeDb)
        {
            Max = FMath::Max(Max, Point.ValueDb);
        }
        return Max;
    }

    /** Loudest envelope point across every band. */
    double AllBandsMaxDb(const TArray<FPwResidualBand>& Bands)
    {
        double Max = -1000.0;
        for (const FPwResidualBand& Band : Bands)
        {
            Max = FMath::Max(Max, BandMaxDb(Band));
        }
        return Max;
    }

    int32 FindBandContaining(const TArray<FPwResidualBand>& Bands, double FreqHz)
    {
        for (int32 Index = 0; Index < Bands.Num(); ++Index)
        {
            if (FreqHz >= Bands[Index].LowHz && FreqHz < Bands[Index].HighHz)
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

    /** The simplified polyline evaluated at an arbitrary time - the curve a caller reconstructs. */
    double EvalEnvelopeAt(const TArray<FPwDbPoint>& Envelope, double TimeMs)
    {
        if (Envelope.Num() == 0)
        {
            return 0.0;
        }
        if (TimeMs <= Envelope[0].TimeMs)
        {
            return Envelope[0].ValueDb;
        }
        if (TimeMs >= Envelope.Last().TimeMs)
        {
            return Envelope.Last().ValueDb;
        }
        for (int32 Index = 1; Index < Envelope.Num(); ++Index)
        {
            if (Envelope[Index].TimeMs >= TimeMs)
            {
                const double Span = Envelope[Index].TimeMs - Envelope[Index - 1].TimeMs;
                const double Alpha = Span > 0.0 ? (TimeMs - Envelope[Index - 1].TimeMs) / Span : 0.0;
                return Envelope[Index - 1].ValueDb
                    + (Envelope[Index].ValueDb - Envelope[Index - 1].ValueDb) * Alpha;
            }
        }
        return Envelope.Last().ValueDb;
    }
}

// ---- modal fitting ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFitModesRecoversKnownDecayTest,
    "PinWright.audio.decompose.fit.RecoversKnownDecayFromExponentialTrack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFitModesRecoversKnownDecayTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeFitTestHelpers;

    // A0 = 0.5 -> 20*log10(0.5) = -6.0206 dBFS. T60 = 500 ms, so the -40 dB fit floor is reached
    // at 500 * 40/60 = 333 ms and 67 of the 121 points enter the regression.
    constexpr double A0 = 0.5;
    constexpr double T60Ms = 500.0;
    constexpr double FreqHz = 1000.0;
    const double ExpectedGainDb = 20.0 * FMath::LogX(10.0, A0);

    TArray<FPwPartialTrack> Partials;
    Partials.Add(MakeExponentialTrack(FreqHz, A0, T60Ms, 5.0, 600.0));

    TArray<FPwModalFit> Fits;
    FString Code;
    FString Message;
    const bool bOk = PwFitModes(Partials, TestSampleRate, Fits, Code, Message);

    TestTrue(FString::Printf(TEXT("PwFitModes succeeded (%s: %s)"), *Code, *Message), bOk);
    TestEqual(TEXT("One fit row per input track"), Fits.Num(), 1);
    if (Fits.Num() != 1)
    {
        return false;
    }
    CheckDecaySignInvariant(*this, Fits);

    const FPwModalFit& Fit = Fits[0];
    TestTrue(TEXT("An exponential decay is a measured mode"), Fit.bMeasured);

    // 1% of 500 ms. The regression is exact on an analytic exponential, so the real error is at
    // double-rounding level; 1% is chosen so that only a genuine defect fails - the two defects
    // this catches (reporting tau instead of T60, or letting the tail flatten the slope) are 590%
    // and tens of percent out respectively.
    const double DecayErrorPct = 100.0 * FMath::Abs(Fit.DecayMs - T60Ms) / T60Ms;
    TestTrue(FString::Printf(TEXT("DecayMs is %.4f ms, expected %.1f ms (%.4f%% off, tolerance 1%%)"),
        Fit.DecayMs, T60Ms, DecayErrorPct), DecayErrorPct <= 1.0);

    TestTrue(FString::Printf(TEXT("InitialGainDb is %.4f dB, expected %.4f dB +/- 0.2"),
        Fit.InitialGainDb, ExpectedGainDb),
        FMath::Abs(Fit.InitialGainDb - ExpectedGainDb) <= 0.2);

    TestTrue(FString::Printf(TEXT("FreqHz is %.4f Hz, expected %.1f Hz"), Fit.FreqHz, FreqHz),
        FMath::Abs(Fit.FreqHz - FreqHz) <= 0.01);

    // A noiseless exponential lies exactly on the fitted line, so the residual is numerical only.
    TestTrue(FString::Printf(TEXT("FitErrorDb is %.6g dB for a noiseless decay (expected ~0)"),
        Fit.FitErrorDb), Fit.FitErrorDb >= 0.0 && Fit.FitErrorDb < 0.01);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFitModesAcrossRatesTest,
    "PinWright.audio.decompose.fit.RecoversDecayAcrossRatesAndFrequencies",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFitModesAcrossRatesTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeFitTestHelpers;

    // The failure modes differ by decay rate and register, so one rate proves little: a very short
    // decay can starve the regression of above-floor points, a very long one can lose the slope to
    // cancellation if the times are not centred, and a near-Nyquist mode is the one the generator
    // refuses. Each row's time step is chosen so the -40 dB window still holds tens of points.
    struct FCase
    {
        double T60Ms;
        double FreqHz;
        double StepMs;
        double DurationMs;
        const TCHAR* Label;
    };
    const FCase Cases[] = {
        { 30.0,   220.0,  0.5, 40.0,   TEXT("very short / low") },
        { 250.0,  1000.0, 2.0, 300.0,  TEXT("medium / mid") },
        { 800.0,  12000.0, 5.0, 1000.0, TEXT("medium / high") },
        { 3000.0, 4500.0, 10.0, 3500.0, TEXT("long / mid") },
    };

    TArray<FPwPartialTrack> Partials;
    for (const FCase& Case : Cases)
    {
        Partials.Add(MakeExponentialTrack(Case.FreqHz, 0.8, Case.T60Ms, Case.StepMs, Case.DurationMs));
    }

    TArray<FPwModalFit> Fits;
    FString Code;
    FString Message;
    const bool bOk = PwFitModes(Partials, TestSampleRate, Fits, Code, Message);

    TestTrue(FString::Printf(TEXT("PwFitModes succeeded (%s: %s)"), *Code, *Message), bOk);
    TestEqual(TEXT("One fit row per input track"), Fits.Num(), static_cast<int32>(UE_ARRAY_COUNT(Cases)));
    if (Fits.Num() != static_cast<int32>(UE_ARRAY_COUNT(Cases)))
    {
        return false;
    }
    CheckDecaySignInvariant(*this, Fits);

    for (int32 Index = 0; Index < Fits.Num(); ++Index)
    {
        const FCase& Case = Cases[Index];
        const FPwModalFit& Fit = Fits[Index];

        TestTrue(FString::Printf(TEXT("%s: measured"), Case.Label), Fit.bMeasured);

        const double ErrorPct = 100.0 * FMath::Abs(Fit.DecayMs - Case.T60Ms) / Case.T60Ms;
        TestTrue(FString::Printf(TEXT("%s: DecayMs is %.4f ms, expected %.1f ms (%.4f%% off)"),
            Case.Label, Fit.DecayMs, Case.T60Ms, ErrorPct), ErrorPct <= 1.0);

        TestTrue(FString::Printf(TEXT("%s: FreqHz is %.3f Hz, expected %.1f Hz"),
            Case.Label, Fit.FreqHz, Case.FreqHz), FMath::Abs(Fit.FreqHz - Case.FreqHz) <= 0.01);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFitModesMatchesGenModalConventionTest,
    "PinWright.audio.decompose.fit.MatchesPwGenModalT60Convention",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFitModesMatchesGenModalConventionTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeFitTestHelpers;

    // This test exists to pin ONE number: the fit must invert exactly what PwGenModal synthesises.
    // The track below is built from the generator's own line,
    //     r = exp(-ln(1000) / (T60Seconds * SampleRate))
    // sampled as A0 * r^n, with no reference to how the fit is implemented. If the fit reported the
    // e-folding time tau instead of the T60, it would come back at T60/ln(1000) = 36.2 ms, and a
    // recipe built from it would ring for a seventh of the reference's tail.
    constexpr double T60Ms = 250.0;
    constexpr double A0 = 0.8;
    constexpr int32 HopSamples = 96;                    // 2 ms at 48 kHz
    const double R = FMath::Exp(-Ln1000 / (T60Ms * 0.001 * static_cast<double>(TestSampleRate)));

    FPwPartialTrack Track;
    for (int32 Frame = 0; Frame * HopSamples <= 19200; ++Frame)    // 400 ms
    {
        const int32 Sample = Frame * HopSamples;
        FPwPartialPoint& Point = Track.Points.AddDefaulted_GetRef();
        Point.TimeMs = 1000.0 * static_cast<double>(Sample) / static_cast<double>(TestSampleRate);
        Point.FreqHz = 640.0;
        Point.AmpLinear = A0 * FMath::Pow(R, static_cast<double>(Sample));
    }
    FinishTrack(Track);

    TArray<FPwPartialTrack> Partials;
    Partials.Add(Track);

    TArray<FPwModalFit> Fits;
    FString Code;
    FString Message;
    const bool bOk = PwFitModes(Partials, TestSampleRate, Fits, Code, Message);

    TestTrue(FString::Printf(TEXT("PwFitModes succeeded (%s: %s)"), *Code, *Message), bOk);
    if (Fits.Num() != 1)
    {
        TestEqual(TEXT("One fit row per input track"), Fits.Num(), 1);
        return false;
    }
    CheckDecaySignInvariant(*this, Fits);

    const FPwModalFit& Fit = Fits[0];
    TestTrue(TEXT("A generator-shaped decay is a measured mode"), Fit.bMeasured);

    const double ErrorPct = 100.0 * FMath::Abs(Fit.DecayMs - T60Ms) / T60Ms;
    TestTrue(FString::Printf(TEXT("DecayMs is %.4f ms, expected the generator's T60 of %.1f ms ")
        TEXT("(%.4f%% off, tolerance 1%%)"), Fit.DecayMs, T60Ms, ErrorPct), ErrorPct <= 1.0);

    // The explicit anti-assertion, so the failure this test exists for names itself in the log
    // rather than arriving as an out-of-tolerance number.
    const double TauMs = T60Ms / Ln1000;
    TestTrue(FString::Printf(TEXT("DecayMs (%.4f ms) is a T60, not the e-folding time tau ")
        TEXT("(%.4f ms)"), Fit.DecayMs, TauMs), FMath::Abs(Fit.DecayMs - TauMs) > 0.2 * TauMs);

    const double ExpectedGainDb = 20.0 * FMath::LogX(10.0, A0);
    TestTrue(FString::Printf(TEXT("InitialGainDb is %.4f dB, expected %.4f dB"),
        Fit.InitialGainDb, ExpectedGainDb),
        FMath::Abs(Fit.InitialGainDb - ExpectedGainDb) <= 0.2);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFitModesSustainedIsNotAModeTest,
    "PinWright.audio.decompose.fit.SustainedTrackIsNotMeasuredAsAMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFitModesSustainedIsNotAModeTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeFitTestHelpers;

    // A held tone is not a mode. The defect this guards against is a fit that divides by a
    // near-zero slope and publishes an enormous but plausible-looking DecayMs, which a caller
    // would happily paste into modeDecaysMs.
    constexpr double Amp = 0.3;
    const double ExpectedGainDb = 20.0 * FMath::LogX(10.0, Amp);

    TArray<FPwPartialTrack> Partials;
    Partials.Add(MakeConstantTrack(440.0, Amp, 4.0, 800.0));

    TArray<FPwModalFit> Fits;
    FString Code;
    FString Message;
    const bool bOk = PwFitModes(Partials, TestSampleRate, Fits, Code, Message);

    TestTrue(FString::Printf(TEXT("PwFitModes succeeded (%s: %s)"), *Code, *Message), bOk);
    if (Fits.Num() != 1)
    {
        TestEqual(TEXT("One fit row per input track"), Fits.Num(), 1);
        return false;
    }
    CheckDecaySignInvariant(*this, Fits);

    const FPwModalFit& Fit = Fits[0];
    TestFalse(TEXT("A sustained track is not measured as a mode"), Fit.bMeasured);
    TestTrue(FString::Printf(TEXT("DecayMs is %.6f - a sustained track publishes no rate at all, ")
        TEXT("not a very long one"), Fit.DecayMs), Fit.DecayMs == 0.0);

    // The level IS measured even though the decay is not: an unmeasured decay does not make the
    // partial's frequency and gain unknown, and zeroing them would throw away facts.
    TestTrue(FString::Printf(TEXT("InitialGainDb is %.4f dB, expected %.4f dB - the level of a ")
        TEXT("sustained partial is still a measurement"), Fit.InitialGainDb, ExpectedGainDb),
        FMath::Abs(Fit.InitialGainDb - ExpectedGainDb) <= 0.2);
    TestTrue(FString::Printf(TEXT("FreqHz is %.3f Hz, expected 440.0"), Fit.FreqHz),
        FMath::Abs(Fit.FreqHz - 440.0) <= 0.01);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFitModesGrowingKeepsSignTest,
    "PinWright.audio.decompose.fit.GrowingTrackReportsNegativeDecayAndIsNotMeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFitModesGrowingKeepsSignTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeFitTestHelpers;

    // rpc-design.md §6: a growing partial and a decaying one must not score alike. A magnitude-only
    // fit would report this track as a healthy 400 ms decay. The sign is kept instead, and a
    // negative decay is structurally unconsumable - PwGenModal rejects modeDecaysMs <= 0 - so it
    // cannot leak into a recipe even if the caller ignores bMeasured.
    constexpr double GrowthMs = 400.0;

    TArray<FPwPartialTrack> Partials;
    Partials.Add(MakeExponentialTrack(900.0, 0.001, -GrowthMs, 3.0, 300.0));

    TArray<FPwModalFit> Fits;
    FString Code;
    FString Message;
    const bool bOk = PwFitModes(Partials, TestSampleRate, Fits, Code, Message);

    TestTrue(FString::Printf(TEXT("PwFitModes succeeded (%s: %s)"), *Code, *Message), bOk);
    if (Fits.Num() != 1)
    {
        TestEqual(TEXT("One fit row per input track"), Fits.Num(), 1);
        return false;
    }
    CheckDecaySignInvariant(*this, Fits);

    const FPwModalFit& Fit = Fits[0];
    TestFalse(TEXT("A growing track is not measured as a mode"), Fit.bMeasured);
    TestTrue(FString::Printf(TEXT("DecayMs is %.4f ms - negative, so the growth is visible and ")
        TEXT("the value cannot be consumed as a decay"), Fit.DecayMs), Fit.DecayMs < 0.0);

    // The magnitude is the time it would take to GAIN 60 dB at the fitted rate, which for this
    // track is exactly GrowthMs.
    const double ErrorPct = 100.0 * FMath::Abs(Fit.DecayMs + GrowthMs) / GrowthMs;
    TestTrue(FString::Printf(TEXT("DecayMs is %.4f ms, expected %.1f ms (%.3f%% off)"),
        Fit.DecayMs, -GrowthMs, ErrorPct), ErrorPct <= 2.0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFitModesNoisyDecayTest,
    "PinWright.audio.decompose.fit.NoisyDecayRecoversT60WithLargerFitError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFitModesNoisyDecayTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeFitTestHelpers;

    // Both tracks go through one call so the comparison cannot drift on anything but the noise.
    // The noise is ADDITIVE (0.01 peak against a 1.0 peak signal, i.e. -40 dBFS relative), which is
    // the model the A^2 weighting is derived from: broadband noise adds to a spectrogram bin, it
    // does not scale with the partial sitting in it.
    constexpr double T60Ms = 400.0;
    constexpr double NoiseAmp = 0.01;

    TArray<FPwPartialTrack> Partials;
    Partials.Add(MakeExponentialTrack(800.0, 1.0, T60Ms, 2.0, 500.0));
    Partials.Add(MakeNoisyExponentialTrack(800.0, 1.0, T60Ms, 2.0, 500.0, NoiseAmp, 20260817));

    TArray<FPwModalFit> Fits;
    FString Code;
    FString Message;
    const bool bOk = PwFitModes(Partials, TestSampleRate, Fits, Code, Message);

    TestTrue(FString::Printf(TEXT("PwFitModes succeeded (%s: %s)"), *Code, *Message), bOk);
    if (Fits.Num() != 2)
    {
        TestEqual(TEXT("One fit row per input track"), Fits.Num(), 2);
        return false;
    }
    CheckDecaySignInvariant(*this, Fits);

    const FPwModalFit& Clean = Fits[0];
    const FPwModalFit& Noisy = Fits[1];

    TestTrue(TEXT("The clean decay is measured"), Clean.bMeasured);
    TestTrue(TEXT("The noisy decay is still measured"), Noisy.bMeasured);

    const double CleanErrorPct = 100.0 * FMath::Abs(Clean.DecayMs - T60Ms) / T60Ms;
    TestTrue(FString::Printf(TEXT("Clean DecayMs is %.4f ms, expected %.1f ms (%.4f%% off)"),
        Clean.DecayMs, T60Ms, CleanErrorPct), CleanErrorPct <= 1.0);

    // The slope's 1-sigma uncertainty for this track works out near 0.7% (residual sigma ~0.018 in
    // ln-amplitude over an effective 29 points spanning ~29 ms of A^2-weighted time), so 8% is over
    // ten sigma: this fails only if the weighting or the floor cut is actually broken, not on the
    // particular seed.
    const double NoisyErrorPct = 100.0 * FMath::Abs(Noisy.DecayMs - T60Ms) / T60Ms;
    TestTrue(FString::Printf(TEXT("Noisy DecayMs is %.4f ms, expected %.1f ms (%.4f%% off, ")
        TEXT("tolerance 8%%)"), Noisy.DecayMs, T60Ms, NoisyErrorPct), NoisyErrorPct <= 8.0);

    // This pair is what makes FitErrorDb worth reading: it must move with the data's actual
    // disagreement with the fitted line, not be a constant dressed as a measurement.
    TestTrue(FString::Printf(TEXT("Clean FitErrorDb is %.6g dB (expected ~0 for an analytic decay)"),
        Clean.FitErrorDb), Clean.FitErrorDb < 0.01);
    TestTrue(FString::Printf(TEXT("Noisy FitErrorDb is %.6g dB, materially above the clean %.6g dB"),
        Noisy.FitErrorDb, Clean.FitErrorDb),
        Noisy.FitErrorDb > 0.02 && Noisy.FitErrorDb > 20.0 * Clean.FitErrorDb);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFitModesRejectsBadInputTest,
    "PinWright.audio.decompose.fit.RejectsEmptyPartialsNonFiniteAndZeroSampleRate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFitModesRejectsBadInputTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeFitTestHelpers;

    // rpc-design.md §12: asserting that a verb succeeds proves almost nothing. Each case below
    // must FAIL, with the code that names what was actually wrong, and must leave Out empty rather
    // than half-populated.
    {
        TArray<FPwPartialTrack> Partials;
        TArray<FPwModalFit> Fits;
        FString Code;
        FString Message;
        TestFalse(TEXT("No partials at all is a failure, not a bank of zero modes"),
            PwFitModes(Partials, TestSampleRate, Fits, Code, Message));
        TestEqual(TEXT("Code is AUDIO_EMPTY_BUFFER"), Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        TestEqual(TEXT("Out is left empty"), Fits.Num(), 0);
    }
    {
        // Tracks exist but carry no points: a different situation from "no tracks", and the one an
        // over-eager index loop would read as success.
        TArray<FPwPartialTrack> Partials;
        Partials.AddDefaulted(3);
        TArray<FPwModalFit> Fits;
        FString Code;
        FString Message;
        TestFalse(TEXT("Tracks with no points are a failure"),
            PwFitModes(Partials, TestSampleRate, Fits, Code, Message));
        TestEqual(TEXT("Code is AUDIO_EMPTY_BUFFER"), Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        TestEqual(TEXT("Out is left empty"), Fits.Num(), 0);
    }
    {
        TArray<FPwPartialTrack> Partials;
        Partials.Add(MakeExponentialTrack(1000.0, 0.5, 400.0, 5.0, 300.0));
        Partials[0].Points[7].AmpLinear = MakeNaNDouble();
        TArray<FPwModalFit> Fits;
        FString Code;
        FString Message;
        TestFalse(TEXT("A non-finite amplitude is a failure"),
            PwFitModes(Partials, TestSampleRate, Fits, Code, Message));
        TestEqual(TEXT("Code is AUDIO_NON_FINITE_SAMPLES"), Code,
            FString(ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES));
        TestEqual(TEXT("Out is left empty"), Fits.Num(), 0);
    }
    {
        // The ordering trap: FMath::Max(0.0, NaN) returns 0, so an all-NaN track measures as
        // digital silence unless non-finite is checked BEFORE any amplitude comparison. A silence
        // code here would send the caller to re-render a track that is actually poisoned.
        TArray<FPwPartialTrack> Partials;
        Partials.Add(MakeExponentialTrack(1000.0, 0.5, 400.0, 5.0, 300.0));
        for (FPwPartialPoint& Point : Partials[0].Points)
        {
            Point.AmpLinear = MakeNaNDouble();
        }
        TArray<FPwModalFit> Fits;
        FString Code;
        FString Message;
        TestFalse(TEXT("An all-NaN track is a failure"),
            PwFitModes(Partials, TestSampleRate, Fits, Code, Message));
        TestEqual(TEXT("An all-NaN track is named non-finite, not measured as silence"), Code,
            FString(ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES));
        TestEqual(TEXT("Out is left empty"), Fits.Num(), 0);
    }
    {
        TArray<FPwPartialTrack> Partials;
        Partials.Add(MakeConstantTrack(1000.0, 0.0, 5.0, 300.0));
        TArray<FPwModalFit> Fits;
        FString Code;
        FString Message;
        TestFalse(TEXT("An all-silent partial set is a failure"),
            PwFitModes(Partials, TestSampleRate, Fits, Code, Message));
        TestEqual(TEXT("Code is AUDIO_EMPTY_BUFFER"), Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        TestEqual(TEXT("Out is left empty"), Fits.Num(), 0);
    }
    {
        TArray<FPwPartialTrack> Partials;
        Partials.Add(MakeExponentialTrack(1000.0, 0.5, 400.0, 5.0, 300.0));
        TArray<FPwModalFit> Fits;
        FString Code;
        FString Message;
        TestFalse(TEXT("A zero sample rate is a failure"),
            PwFitModes(Partials, 0, Fits, Code, Message));
        TestEqual(TEXT("Code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestEqual(TEXT("Out is left empty"), Fits.Num(), 0);
    }
    {
        // Time order is a structural guarantee, not a convention: an out-of-order track would
        // still produce a slope, just a meaningless one, with no symptom for the caller.
        TArray<FPwPartialTrack> Partials;
        Partials.Add(MakeExponentialTrack(1000.0, 0.5, 400.0, 5.0, 300.0));
        Partials[0].Points[10].TimeMs = 1.0;
        TArray<FPwModalFit> Fits;
        FString Code;
        FString Message;
        TestFalse(TEXT("A track that goes backwards in time is a failure"),
            PwFitModes(Partials, TestSampleRate, Fits, Code, Message));
        TestEqual(TEXT("Code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestEqual(TEXT("Out is left empty"), Fits.Num(), 0);
    }

    return true;
}

// ---- noise residual ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwResidualPureSineIsSilentTest,
    "PinWright.audio.decompose.residual.PureSineLeavesNearSilentBands",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwResidualPureSineIsSilentTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeFitTestHelpers;

    // A 1 kHz sine at -6 dBFS is entirely explained by one partial, so its residual must be
    // near-silent EVERYWHERE - including the band the sine sat in, which is the band a subtractor
    // that only masked its neighbours would leave hot.
    const TArray<float> Samples = MakeSine(0.5, 1000.0, 0.5);

    FPwStftResult Stft;
    FString StftMessage;
    const bool bStftOk = ComputeResidualStft(Samples, Stft, StftMessage);
    TestTrue(FString::Printf(TEXT("STFT succeeded (%s)"), *StftMessage), bStftOk);
    if (!bStftOk)
    {
        return false;
    }

    const FPwDecomposeSettings Settings = MakeResidualSettings(24, 1.5);

    // Direction one: with the partial supplied, everything must be gone.
    TArray<FPwPartialTrack> Partials;
    Partials.Add(MakeFrameAlignedTrack(Stft, 1000.0, 0.5));

    TArray<FPwResidualBand> Bands;
    FString Code;
    FString Message;
    const bool bOk = PwAnalyzeResidual(Stft, Partials, TestSampleRate, Settings, Bands, Code, Message);
    TestTrue(FString::Printf(TEXT("PwAnalyzeResidual succeeded (%s: %s)"), *Code, *Message), bOk);
    if (!bOk)
    {
        return false;
    }

    // 24 log bands from one bin width (46.875 Hz) to Nyquist put bins 1, 2 and 3 into bands 0, 2
    // and 4, so bands 1 and 3 receive no bin at all and are OMITTED rather than reported at the
    // floor (rpc-design.md §1: a band you could not measure is absent, not zero).
    TestTrue(FString::Printf(TEXT("%d of 24 bands were emitted - the binless low bands are absent, ")
        TEXT("not reported as silence"), Bands.Num()), Bands.Num() >= 20 && Bands.Num() < 24);

    for (int32 Index = 1; Index < Bands.Num(); ++Index)
    {
        TestTrue(FString::Printf(TEXT("Band %d starts (%.2f Hz) at or above band %d's start ")
            TEXT("(%.2f Hz)"), Index, Bands[Index].LowHz, Index - 1, Bands[Index - 1].LowHz),
            Bands[Index].LowHz > Bands[Index - 1].LowHz);
        TestTrue(FString::Printf(TEXT("Band %d has a positive width (%.2f..%.2f Hz)"),
            Index, Bands[Index].LowHz, Bands[Index].HighHz),
            Bands[Index].HighHz > Bands[Index].LowHz);
        TestTrue(FString::Printf(TEXT("Band %d carries an envelope"), Index),
            Bands[Index].EnvelopeDb.Num() > 0);
    }

    const double ResidualMaxDb = AllBandsMaxDb(Bands);
    TestTrue(FString::Printf(TEXT("The loudest residual point over every band is %.2f dBFS, ")
        TEXT("expected below -30 for a fully explained sine"), ResidualMaxDb),
        ResidualMaxDb < -30.0);

    const int32 SineBand = FindBandContaining(Bands, 1000.0);
    TestTrue(TEXT("A band covering 1000 Hz was emitted"), SineBand != INDEX_NONE);
    if (SineBand != INDEX_NONE)
    {
        TestTrue(FString::Printf(TEXT("The sine's own band (%.1f..%.1f Hz) peaks at %.2f dBFS, ")
            TEXT("expected below -30 - the partial was removed, not merely bracketed"),
            Bands[SineBand].LowHz, Bands[SineBand].HighHz, BandMaxDb(Bands[SineBand])),
            BandMaxDb(Bands[SineBand]) < -30.0);
    }

    // Direction two (rpc-design.md §6): with NO partials the same spectrogram must come back loud.
    // Without this, a subtractor that returned silence unconditionally would pass the test above.
    TArray<FPwResidualBand> Unsubtracted;
    const bool bUnsubtractedOk = PwAnalyzeResidual(Stft, TArray<FPwPartialTrack>(), TestSampleRate,
                                                  Settings, Unsubtracted, Code, Message);
    TestTrue(FString::Printf(TEXT("PwAnalyzeResidual with no partials succeeded (%s: %s)"),
        *Code, *Message), bUnsubtractedOk);
    if (bUnsubtractedOk)
    {
        const double UnsubtractedMaxDb = AllBandsMaxDb(Unsubtracted);
        TestTrue(FString::Printf(TEXT("With no partials supplied the residual peaks at %.2f dBFS ")
            TEXT("(the sine is still there), against %.2f dBFS once it is subtracted"),
            UnsubtractedMaxDb, ResidualMaxDb),
            UnsubtractedMaxDb > -12.0 && UnsubtractedMaxDb - ResidualMaxDb > 20.0);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwResidualRetainsNoiseTest,
    "PinWright.audio.decompose.residual.SineWithNoiseRetainsTheNoise",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwResidualRetainsNoiseTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeFitTestHelpers;

    // The other direction of the same measurement. Removing the tracked sine must not remove the
    // broadband noise underneath it - a subtractor that zeroed everything, or one that mistook the
    // noise floor for leakage, would report the same near-silence as the pure-sine case.
    const TArray<float> CleanSamples = MakeSine(0.5, 1000.0, 0.5);
    TArray<float> NoisySamples = CleanSamples;
    AddWhiteNoise(NoisySamples, 0.1, 90210);

    FPwStftResult CleanStft;
    FPwStftResult NoisyStft;
    FString StftMessage;
    const bool bCleanOk = ComputeResidualStft(CleanSamples, CleanStft, StftMessage);
    TestTrue(FString::Printf(TEXT("Clean STFT succeeded (%s)"), *StftMessage), bCleanOk);
    const bool bNoisyOk = ComputeResidualStft(NoisySamples, NoisyStft, StftMessage);
    TestTrue(FString::Printf(TEXT("Noisy STFT succeeded (%s)"), *StftMessage), bNoisyOk);
    if (!bCleanOk || !bNoisyOk)
    {
        return false;
    }

    const FPwDecomposeSettings Settings = MakeResidualSettings(24, 1.5);
    TArray<FPwPartialTrack> Partials;
    Partials.Add(MakeFrameAlignedTrack(CleanStft, 1000.0, 0.5));

    TArray<FPwResidualBand> CleanBands;
    TArray<FPwResidualBand> NoisyBands;
    FString Code;
    FString Message;
    TestTrue(TEXT("Clean residual succeeded"),
        PwAnalyzeResidual(CleanStft, Partials, TestSampleRate, Settings, CleanBands, Code, Message));
    TestTrue(TEXT("Noisy residual succeeded"),
        PwAnalyzeResidual(NoisyStft, Partials, TestSampleRate, Settings, NoisyBands, Code, Message));
    TestEqual(TEXT("Both inputs produce the same band layout"), NoisyBands.Num(), CleanBands.Num());
    if (NoisyBands.Num() != CleanBands.Num() || CleanBands.Num() == 0)
    {
        return false;
    }

    // 5 kHz is 85 bins away from the sine, so the clean case has nothing there but the numerical
    // floor while the noisy case must carry the noise.
    const int32 FarBand = FindBandContaining(CleanBands, 5000.0);
    TestTrue(TEXT("A band covering 5000 Hz was emitted"), FarBand != INDEX_NONE);
    if (FarBand == INDEX_NONE)
    {
        return false;
    }

    const double CleanFarDb = BandMaxDb(CleanBands[FarBand]);
    const double NoisyFarDb = BandMaxDb(NoisyBands[FarBand]);
    TestTrue(FString::Printf(TEXT("The 5 kHz band reads %.2f dBFS with noise against %.2f dBFS ")
        TEXT("without it (delta %.2f dB, expected at least 20)"),
        NoisyFarDb, CleanFarDb, NoisyFarDb - CleanFarDb), NoisyFarDb - CleanFarDb >= 20.0);

    // Retained at a plausible level, not merely nonzero: uniform noise of 0.1 peak spreads roughly
    // -49 dBFS per bin over 513 bins, and this band holds 24 of them, so about -35 dBFS. A window
    // this wide rules out both a floor-level answer and a full-scale one without pinning the exact
    // spectral convention.
    TestTrue(FString::Printf(TEXT("The retained noise reads %.2f dBFS, inside the plausible ")
        TEXT("-60..-10 dBFS window"), NoisyFarDb), NoisyFarDb > -60.0 && NoisyFarDb < -10.0);

    // And the tracked sine is still removed in the noisy case - subtraction and retention are not
    // in tension, so failing to show both would mean the mask width is doing the work. This is the
    // ONLY assertion in this test that requires the subtraction to have run at all (the two above
    // are satisfied by an analyser that subtracts nothing), so the band's existence is asserted
    // rather than used as a silent guard: without it, a layout regression that dropped the 1 kHz
    // band would skip the assertion and the test would pass green.
    const int32 SineBand = FindBandContaining(NoisyBands, 1000.0);
    TestTrue(TEXT("A band covering 1000 Hz was emitted in the noisy case"), SineBand != INDEX_NONE);
    if (SineBand != INDEX_NONE)
    {
        TestTrue(FString::Printf(TEXT("The sine's band reads %.2f dBFS in the noisy case, well ")
            TEXT("below the sine's own -6.02 dBFS"), BandMaxDb(NoisyBands[SineBand])),
            BandMaxDb(NoisyBands[SineBand]) < -20.0);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwResidualSimplificationTest,
    "PinWright.audio.decompose.residual.SimplificationPreservesShapeAndReducesPoints",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwResidualSimplificationTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeFitTestHelpers;

    // Simplification is mandatory rather than an optimization: a bare handler result has room for
    // roughly 4,250 characters, and a per-frame envelope for every band is an order of magnitude
    // past that. So both halves are asserted - the shape survives within the stated tolerance AND
    // the point count actually falls.
    //
    // The signal is white noise under a gentle 1500 ms exponential gain, so every band envelope is
    // a straight dB ramp with the estimator ripple a real noise layer has, and 12 bands (rather
    // than 24) are used so each band averages enough bins for its ripple to be smaller than the
    // tolerance being tested.
    TArray<float> Samples = MakeSilentBuffer(0.5);
    AddWhiteNoise(Samples, 0.5, 4242);
    ApplyExponentialGain(Samples, 1500.0);

    FPwStftResult Stft;
    FString StftMessage;
    const bool bStftOk = ComputeResidualStft(Samples, Stft, StftMessage);
    TestTrue(FString::Printf(TEXT("STFT succeeded (%s)"), *StftMessage), bStftOk);
    if (!bStftOk)
    {
        return false;
    }

    constexpr double ToleranceDb = 1.5;
    const TArray<FPwPartialTrack> NoPartials;

    // A tolerance of 0.01 dB is below the smoother's own output resolution, so this run is the
    // dense curve the simplified one has to stay near.
    TArray<FPwResidualBand> Dense;
    TArray<FPwResidualBand> Sparse;
    FString Code;
    FString Message;
    TestTrue(TEXT("Dense residual succeeded"),
        PwAnalyzeResidual(Stft, NoPartials, TestSampleRate, MakeResidualSettings(12, 0.01),
                          Dense, Code, Message));
    TestTrue(TEXT("Simplified residual succeeded"),
        PwAnalyzeResidual(Stft, NoPartials, TestSampleRate, MakeResidualSettings(12, ToleranceDb),
                          Sparse, Code, Message));
    TestEqual(TEXT("Both runs describe the same bands"), Sparse.Num(), Dense.Num());
    if (Sparse.Num() != Dense.Num() || Dense.Num() == 0)
    {
        return false;
    }

    int32 TotalDense = 0;
    int32 TotalSparse = 0;
    double WorstDeviationDb = 0.0;
    int32 WorstBand = INDEX_NONE;
    for (int32 Band = 0; Band < Dense.Num(); ++Band)
    {
        TotalDense += Dense[Band].EnvelopeDb.Num();
        TotalSparse += Sparse[Band].EnvelopeDb.Num();

        TestTrue(FString::Printf(TEXT("Band %d: simplified to %d points from %d"),
            Band, Sparse[Band].EnvelopeDb.Num(), Dense[Band].EnvelopeDb.Num()),
            Sparse[Band].EnvelopeDb.Num() <= Dense[Band].EnvelopeDb.Num()
            && Sparse[Band].EnvelopeDb.Num() >= 2);

        for (const FPwDbPoint& DensePoint : Dense[Band].EnvelopeDb)
        {
            const double Reconstructed = EvalEnvelopeAt(Sparse[Band].EnvelopeDb, DensePoint.TimeMs);
            const double Deviation = FMath::Abs(Reconstructed - DensePoint.ValueDb);
            if (Deviation > WorstDeviationDb)
            {
                WorstDeviationDb = Deviation;
                WorstBand = Band;
            }
        }
    }

    // The Douglas-Peucker guarantee, stated as the caller sees it: reconstructing the simplified
    // polyline at any dropped point's time lands within the tolerance the caller asked for.
    TestTrue(FString::Printf(TEXT("Worst reconstruction error is %.4f dB (band %d), tolerance %.2f dB"),
        WorstDeviationDb, WorstBand, ToleranceDb), WorstDeviationDb <= ToleranceDb + 1.0e-6);

    TestTrue(FString::Printf(TEXT("The dense run carries %d points across %d bands - enough for the ")
        TEXT("reduction below to mean something"), TotalDense, Dense.Num()),
        TotalDense >= 12 * 60);
    TestTrue(FString::Printf(TEXT("Simplification cut %d points to %d, at least halving them"),
        TotalDense, TotalSparse), TotalSparse * 2 <= TotalDense);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwResidualRejectsBadInputTest,
    "PinWright.audio.decompose.residual.RejectsEmptySpectrogramNonFiniteAndZeroSampleRate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwResidualRejectsBadInputTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeFitTestHelpers;

    const TArray<float> Samples = MakeSine(0.2, 1000.0, 0.5);
    FPwStftResult GoodStft;
    FString StftMessage;
    const bool bStftOk = ComputeResidualStft(Samples, GoodStft, StftMessage);
    TestTrue(FString::Printf(TEXT("STFT succeeded (%s)"), *StftMessage), bStftOk);
    if (!bStftOk)
    {
        return false;
    }

    const FPwDecomposeSettings Settings = MakeResidualSettings(24, 1.5);
    const TArray<FPwPartialTrack> NoPartials;

    {
        const FPwStftResult Empty;
        TArray<FPwResidualBand> Bands;
        FString Code;
        FString Message;
        TestFalse(TEXT("An empty spectrogram is a failure, not a residual of zero bands"),
            PwAnalyzeResidual(Empty, NoPartials, TestSampleRate, Settings, Bands, Code, Message));
        TestEqual(TEXT("Code is AUDIO_EMPTY_BUFFER"), Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        TestEqual(TEXT("Out is left empty"), Bands.Num(), 0);
    }
    {
        FPwStftResult Poisoned = GoodStft;
        Poisoned.Magnitudes[Poisoned.Magnitudes.Num() / 2] = MakeNaNFloat();
        TArray<FPwResidualBand> Bands;
        FString Code;
        FString Message;
        TestFalse(TEXT("A single non-finite bin is a failure"),
            PwAnalyzeResidual(Poisoned, NoPartials, TestSampleRate, Settings, Bands, Code, Message));
        TestEqual(TEXT("Code is AUDIO_NON_FINITE_SAMPLES"), Code,
            FString(ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES));
        TestEqual(TEXT("Out is left empty"), Bands.Num(), 0);
    }
    {
        // The ordering trap again, on the spectrogram side: an all-NaN matrix has a "peak" of 0
        // under FMath::Max, so it measures as digital silence unless non-finite is named first.
        FPwStftResult AllNaN = GoodStft;
        for (float& Magnitude : AllNaN.Magnitudes)
        {
            Magnitude = MakeNaNFloat();
        }
        TArray<FPwResidualBand> Bands;
        FString Code;
        FString Message;
        TestFalse(TEXT("An all-NaN spectrogram is a failure"),
            PwAnalyzeResidual(AllNaN, NoPartials, TestSampleRate, Settings, Bands, Code, Message));
        TestEqual(TEXT("An all-NaN spectrogram is named non-finite, not measured as silence"),
            Code, FString(ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES));
        TestEqual(TEXT("Out is left empty"), Bands.Num(), 0);
    }
    {
        FPwStftResult Silent = GoodStft;
        for (float& Magnitude : Silent.Magnitudes)
        {
            Magnitude = 0.f;
        }
        TArray<FPwResidualBand> Bands;
        FString Code;
        FString Message;
        TestFalse(TEXT("A silent spectrogram is a failure"),
            PwAnalyzeResidual(Silent, NoPartials, TestSampleRate, Settings, Bands, Code, Message));
        TestEqual(TEXT("Code is AUDIO_EMPTY_BUFFER"), Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        TestEqual(TEXT("Out is left empty"), Bands.Num(), 0);
    }
    {
        TArray<FPwResidualBand> Bands;
        FString Code;
        FString Message;
        TestFalse(TEXT("A zero sample rate is a failure"),
            PwAnalyzeResidual(GoodStft, NoPartials, 0, Settings, Bands, Code, Message));
        TestEqual(TEXT("Code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestEqual(TEXT("Out is left empty"), Bands.Num(), 0);
    }
    {
        TArray<FPwResidualBand> Bands;
        FString Code;
        FString Message;
        TestFalse(TEXT("Zero bands is a failure - no description, not an empty one"),
            PwAnalyzeResidual(GoodStft, NoPartials, TestSampleRate, MakeResidualSettings(0, 1.5),
                              Bands, Code, Message));
        TestEqual(TEXT("Code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestEqual(TEXT("Out is left empty"), Bands.Num(), 0);
    }
    {
        // A sample rate that disagrees with the spectrogram it was computed from would still
        // produce bands, just on the wrong frequency axis, with nothing in the output to show it.
        TArray<FPwResidualBand> Bands;
        FString Code;
        FString Message;
        TestFalse(TEXT("A sample rate that contradicts the spectrogram is a failure"),
            PwAnalyzeResidual(GoodStft, NoPartials, 96000, Settings, Bands, Code, Message));
        TestEqual(TEXT("Code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestEqual(TEXT("Out is left empty"), Bands.Num(), 0);
    }
    {
        TArray<FPwPartialTrack> Poisoned;
        Poisoned.Add(MakeFrameAlignedTrack(GoodStft, 1000.0, 0.5));
        Poisoned[0].Points[2].FreqHz = MakeNaNDouble();
        TArray<FPwResidualBand> Bands;
        FString Code;
        FString Message;
        TestFalse(TEXT("A non-finite partial point is a failure"),
            PwAnalyzeResidual(GoodStft, Poisoned, TestSampleRate, Settings, Bands, Code, Message));
        TestEqual(TEXT("Code is AUDIO_NON_FINITE_SAMPLES"), Code,
            FString(ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES));
        TestEqual(TEXT("Out is left empty"), Bands.Num(), 0);
    }

    return true;
}
