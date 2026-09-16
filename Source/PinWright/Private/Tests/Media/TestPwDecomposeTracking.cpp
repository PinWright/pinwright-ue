// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the front half of the decomposition analyzer in AudioGen/PwAudioDecompose.h:
// harmonic/percussive separation, spectral peak refinement, partial tracking and transient
// characterisation.
//
// Every assertion is against analytic ground truth rather than against "it returned true". Three
// steady sines have exactly three known frequencies; a linear chirp has a derivable signed drift;
// a tone placed between two bin centres has a known distance to the nearer of them. Where a
// tolerance appears, the comment above it derives it from the bin spacing and the estimator's
// documented bias, and it is chosen so the plausible implementation bugs fall OUTSIDE it - the
// point of the interpolation test, for instance, is that it fails a peak-picker that reports bin
// centres, which a loose tolerance would pass.
//
// Per rpc-design.md §6 both directions of each measurement are asserted: the chirp is run forwards
// AND backwards so a magnitude-only drift cannot pass, and HPSS is fed a sustained tone AND an
// impulse train in the same test so a separator that dumps everything into one bucket fails.
// Per §12 the failure direction is asserted too: empty, NaN-carrying, silent and over-limit inputs
// each have to fail with the right code and leave the out-parameter cleared.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioDecompose.h"
#include "AudioGen/PwAudioFeatures.h"
#include "AudioGen/PwStft.h"
#include "Handlers/ErrorCodes.h"

#include "Math/RandomStream.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true, and anonymous
// namespaces in merged TUs are the ODR-collision source the plugin's Build.cs comment warns about.
namespace PwDecomposeTrackingTestHelpers
{
    constexpr int32 TestSampleRate = 48000;

    // ---- analysis grids ----------------------------------------------------------------------
    //
    // Two grids, each chosen for what it has to resolve, and every derived tolerance below is
    // stated against the one it belongs to.
    //
    // COARSE (partial tracking): 2048 / 256 at 48 kHz -> 23.4375 Hz per bin, 5.333 ms per hop,
    // 42.7 ms window. The fine frequency resolution is what the sub-bin peak assertions need.
    constexpr int32 CoarseFft = 2048;
    constexpr int32 CoarseHop = 256;
    constexpr double CoarseBinHz = static_cast<double>(TestSampleRate) / CoarseFft;   // 23.4375

    // FINE (impulses): 512 / 128 -> 93.75 Hz per bin, 2.667 ms per hop, 10.7 ms window. A short
    // window is what keeps an impulse's footprint (4 frames) far under the 31-frame HPSS time
    // kernel, which is the condition under which the time median rejects it.
    constexpr int32 FineFft = 512;
    constexpr int32 FineHop = 128;

    // ONSET (transients): 256 / 64, the grid PwDetectOnsets' own tests are calibrated on.
    constexpr int32 OnsetFft = 256;
    constexpr int32 OnsetHop = 64;

    double FrameCentreMs(int32 Frame, int32 HopSize, int32 FftSize)
    {
        return 1000.0 * (Frame * static_cast<double>(HopSize) + 0.5 * FftSize) / TestSampleRate;
    }

    float MakeNaN()
    {
        constexpr uint32 QuietNaNBits = 0x7FC00000u;
        float Value = 0.f;
        FMemory::Memcpy(&Value, &QuietNaNBits, sizeof(Value));
        return Value;
    }

    float MakeInfinity()
    {
        constexpr uint32 PositiveInfinityBits = 0x7F800000u;
        float Value = 0.f;
        FMemory::Memcpy(&Value, &PositiveInfinityBits, sizeof(Value));
        return Value;
    }

    TArray<float> MakeSilence(double DurationSeconds)
    {
        TArray<float> Samples;
        Samples.SetNumZeroed(FMath::RoundToInt32(DurationSeconds * TestSampleRate));
        return Samples;
    }

    /** Sum of sinusoids, each with its own frequency and peak amplitude. */
    TArray<float> MakeSines(double DurationSeconds, const TArray<double>& Hz,
        const TArray<double>& PeakAmplitudes)
    {
        TArray<float> Samples = MakeSilence(DurationSeconds);
        for (int32 Tone = 0; Tone < Hz.Num(); ++Tone)
        {
            const double Amplitude = PeakAmplitudes.IsValidIndex(Tone) ? PeakAmplitudes[Tone] : 1.0;
            const double AngularStep = 2.0 * UE_DOUBLE_PI * Hz[Tone] / TestSampleRate;
            for (int32 Index = 0; Index < Samples.Num(); ++Index)
            {
                Samples[Index] = static_cast<float>(Samples[Index] + Amplitude * FMath::Sin(AngularStep * Index));
            }
        }
        return Samples;
    }

    /** Linear chirp from StartHz to EndHz across the whole buffer. */
    TArray<float> MakeChirp(double DurationSeconds, double StartHz, double EndHz, double PeakAmplitude)
    {
        TArray<float> Samples = MakeSilence(DurationSeconds);
        const double Sweep = (EndHz - StartHz) / DurationSeconds;
        for (int32 Index = 0; Index < Samples.Num(); ++Index)
        {
            const double T = static_cast<double>(Index) / TestSampleRate;
            const double Phase = 2.0 * UE_DOUBLE_PI * (StartHz * T + 0.5 * Sweep * T * T);
            Samples[Index] = static_cast<float>(PeakAmplitude * FMath::Sin(Phase));
        }
        return Samples;
    }

    /** Deterministic uniform white noise added on top of an existing signal. */
    void AddWhiteNoise(TArray<float>& Samples, double PeakAmplitude, int32 Seed)
    {
        FRandomStream Stream(Seed);
        for (int32 Index = 0; Index < Samples.Num(); ++Index)
        {
            Samples[Index] = static_cast<float>(Samples[Index] + Stream.FRandRange(-PeakAmplitude, PeakAmplitude));
        }
    }

    /** Zero everywhere except one full-scale sample at each requested millisecond. */
    TArray<float> MakeImpulses(double DurationSeconds, const TArray<double>& TimesMs)
    {
        TArray<float> Samples = MakeSilence(DurationSeconds);
        for (const double TimeMs : TimesMs)
        {
            const int32 Index = FMath::RoundToInt32(TimeMs * TestSampleRate / 1000.0);
            if (Samples.IsValidIndex(Index))
            {
                Samples[Index] = 1.f;
            }
        }
        return Samples;
    }

    bool BuildStft(const TArray<float>& Samples, int32 FftSize, int32 HopSize, FPwStftResult& Out)
    {
        FPwStftSettings Settings;
        Settings.FftSize = FftSize;
        Settings.HopSize = HopSize;
        FPwStftError Error;
        return PwComputeStft(Samples, TestSampleRate, Settings, Out, &Error);
    }

    /**
     * Tracking settings for the tone tests.
     *
     * PartialMinAmpDb is raised from the -60 default to -20, and the reason is the documented
     * KNOWN LIMIT in PwAudioDecompose.h: a Hann window's first sidelobe sits about 31 dB below the
     * mainlobe and IS a genuine local maximum, so at -60 dB every tone contributes a ring of
     * spurious sidelobe tracks and "exactly three tones give exactly three tracks" would be
     * testing the sidelobe count rather than the tracker. -20 dB is above the worst-case sidelobe
     * (about -29 dB for a half-bin-offset tone) and below the quietest tone used here (-14 dB
     * relative to the loudest), so it admits every real partial and no window artefact.
     */
    FPwDecomposeSettings ToneTrackingSettings()
    {
        FPwDecomposeSettings Settings;
        Settings.PartialMinAmpDb = -20.0;
        return Settings;
    }

    const FPwPartialTrack* FindTrackNear(const TArray<FPwPartialTrack>& Tracks, double Hz, double ToleranceHz)
    {
        for (const FPwPartialTrack& Track : Tracks)
        {
            if (FMath::Abs(Track.MeanFreqHz - Hz) <= ToleranceHz)
            {
                return &Track;
            }
        }
        return nullptr;
    }

    double SumSquares(const TArray<float>& Values)
    {
        double Total = 0.0;
        for (const float Value : Values)
        {
            Total += static_cast<double>(Value) * static_cast<double>(Value);
        }
        return Total;
    }

    /** Fraction of the separated energy that landed in the harmonic component, 0..1. */
    double HarmonicEnergyFraction(const TArray<float>& Harmonic, const TArray<float>& Percussive)
    {
        const double H = SumSquares(Harmonic);
        const double P = SumSquares(Percussive);
        return (H + P > 0.0) ? (H / (H + P)) : 0.0;
    }
}

// =============================================================================================
// A. PARTIAL TRACKING - three steady sines give exactly three tracks at the right frequencies.
//
// Frequency tolerance: 3.5 Hz, which is 0.15 bins on the coarse grid. Derived from two facts and
// deliberately tighter than either failure it has to catch. The three-point parabolic fit on log
// magnitudes leaves under 0.02 bins (0.5 Hz) of bias for a Hann window, and the three tones are
// 34+ bins apart, so their mutual leakage at the peaks is below -100 dB and contributes nothing.
// A peak-picker that reported bin centres instead would be wrong by 7.8, 10.9 and 6.3 Hz for
// these three tones - every one of them outside 3.5 Hz, so this test fails such an implementation
// rather than absorbing it.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeThreeSinesTest,
    "PinWright.audio.decompose.ThreeSteadySinesProduceThreePartialTracks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeThreeSinesTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeTrackingTestHelpers;

    // 500 / 1300 / 2900 Hz land on bins 21.33, 55.47 and 123.73 - none of them on a bin centre,
    // so the sub-bin refinement is exercised by all three.
    const TArray<double> Hz = { 500.0, 1300.0, 2900.0 };
    const TArray<double> Amplitudes = { 0.3, 0.2, 0.1 };
    const TArray<float> Samples = MakeSines(0.5, Hz, Amplitudes);

    FPwStftResult Stft;
    if (!TestTrue(TEXT("STFT computed"), BuildStft(Samples, CoarseFft, CoarseHop, Stft)))
    {
        return false;
    }

    TArray<FPwPartialTrack> Tracks;
    FString Code;
    FString Message;
    const bool bTracked = PwTrackPartials(Stft, TestSampleRate, ToneTrackingSettings(), Tracks, Code, Message);

    TestTrue(FString::Printf(TEXT("PwTrackPartials succeeded (%s: %s)"), *Code, *Message), bTracked);
    if (!bTracked)
    {
        return false;
    }

    TestEqual(TEXT("three tones give exactly three tracks"), Tracks.Num(), 3);
    TestTrue(TEXT("no error code on success"), Code.IsEmpty());

    // Nothing was dropped, so the truncation note must be ABSENT. A note emitted unconditionally
    // would make the honest note indistinguishable from noise.
    TestTrue(FString::Printf(TEXT("no truncation note when nothing was dropped (got '%s')"), *Message),
        Message.IsEmpty());

    constexpr double FreqToleranceHz = 3.5;
    const double FirstCentreMs = FrameCentreMs(0, CoarseHop, CoarseFft);
    const double LastCentreMs = FrameCentreMs(Stft.NumFrames - 1, CoarseHop, CoarseFft);
    const double HopMs = 1000.0 * CoarseHop / static_cast<double>(TestSampleRate);

    for (int32 Tone = 0; Tone < Hz.Num(); ++Tone)
    {
        const FPwPartialTrack* Track = FindTrackNear(Tracks, Hz[Tone], FreqToleranceHz);
        if (!TestNotNull(*FString::Printf(TEXT("a track within %.1f Hz of %.0f Hz exists"),
            FreqToleranceHz, Hz[Tone]), Track))
        {
            continue;
        }

        TestTrue(FString::Printf(TEXT("%.0f Hz track starts at the first frame (%.2f ms vs %.2f ms)"),
            Hz[Tone], Track->StartMs, FirstCentreMs),
            Track->StartMs <= FirstCentreMs + 2.0 * HopMs);
        TestTrue(FString::Printf(TEXT("%.0f Hz track runs to the last frame (%.2f ms vs %.2f ms)"),
            Hz[Tone], Track->EndMs, LastCentreMs),
            Track->EndMs >= LastCentreMs - 2.0 * HopMs);

        // A steady tone does not drift. One bin of slack absorbs the peak jitter at the two ends.
        TestTrue(FString::Printf(TEXT("%.0f Hz track has no drift (%.3f Hz)"), Hz[Tone], Track->FreqDriftHz),
            FMath::Abs(Track->FreqDriftHz) <= CoarseBinHz);

        // The STFT convention makes a bin-centred 0 dBFS sine read 1.0, and log-parabolic
        // interpolation recovers the off-centre peak to well under 0.1 dB, so the measured
        // amplitude must land within 1 dB of the tone's own amplitude. Without the interpolation
        // Hann scalloping alone costs up to 1.42 dB, which this would catch.
        const double AmplitudeDb = 20.0 * FMath::LogX(10.0, Track->PeakAmpLinear / Amplitudes[Tone]);
        TestTrue(FString::Printf(TEXT("%.0f Hz track peak amplitude %.4f is within 1 dB of %.2f (%.3f dB)"),
            Hz[Tone], Track->PeakAmpLinear, Amplitudes[Tone], AmplitudeDb),
            FMath::Abs(AmplitudeDb) <= 1.0);
    }

    // The cap is applied on loudness, so the output order has to be loudness order.
    if (Tracks.Num() == 3)
    {
        TestTrue(TEXT("tracks are returned loudest first"),
            Tracks[0].PeakAmpLinear >= Tracks[1].PeakAmpLinear
            && Tracks[1].PeakAmpLinear >= Tracks[2].PeakAmpLinear);
    }

    return true;
}

// =============================================================================================
// B. PARTIAL TRACKING - a chirp gives ONE track whose drift is signed (§6, both directions).
//
// Derivation. The sweep is 1000 -> 1500 Hz over 2.0 s, i.e. 250 Hz/s. The track cannot span the
// whole buffer, only the whole FRAME GRID: the first frame is centred at FftSize/2 = 21.333 ms and
// the last at frame 367, i.e. 1978.667 ms, so the measured drift is 250 Hz/s * 1.957333 s =
// 489.33 Hz, not the full 500 Hz. Tolerance 20 Hz (0.85 bins) covers the peak jitter at both ends
// of a mildly smeared lobe - the sweep moves 10.7 Hz (0.46 bins) within one analysis window, so
// the lobe is barely wider than a stationary tone's.
//
// The reverse sweep must give the SAME magnitude with the opposite sign. That pairing is the whole
// test: an implementation reporting |drift|, or reporting max-minus-min, passes the forward case
// and fails here.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeChirpDriftTest,
    "PinWright.audio.decompose.ChirpDriftIsSignedAndMirrorsUnderReversal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeChirpDriftTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeTrackingTestHelpers;

    auto TrackChirp = [this](double StartHz, double EndHz, double& OutDriftHz, int32& OutTrackCount)
    {
        const TArray<float> Samples = MakeChirp(2.0, StartHz, EndHz, 0.5);

        FPwStftResult Stft;
        if (!BuildStft(Samples, CoarseFft, CoarseHop, Stft))
        {
            AddError(FString::Printf(TEXT("STFT failed for the %.0f -> %.0f Hz chirp"), StartHz, EndHz));
            return false;
        }

        TArray<FPwPartialTrack> Tracks;
        FString Code;
        FString Message;
        if (!PwTrackPartials(Stft, TestSampleRate, ToneTrackingSettings(), Tracks, Code, Message))
        {
            AddError(FString::Printf(TEXT("PwTrackPartials failed for the %.0f -> %.0f Hz chirp (%s: %s)"),
                StartHz, EndHz, *Code, *Message));
            return false;
        }

        OutTrackCount = Tracks.Num();
        OutDriftHz = (Tracks.Num() > 0) ? Tracks[0].FreqDriftHz : 0.0;
        return Tracks.Num() > 0;
    };

    // 250 Hz/s over the 1.957333 s spanned by the frame centres.
    constexpr double ExpectedDriftHz = 489.33;
    constexpr double DriftToleranceHz = 20.0;

    double RisingDrift = 0.0;
    double FallingDrift = 0.0;
    int32 RisingTracks = 0;
    int32 FallingTracks = 0;

    if (!TrackChirp(1000.0, 1500.0, RisingDrift, RisingTracks)
        || !TrackChirp(1500.0, 1000.0, FallingDrift, FallingTracks))
    {
        return false;
    }

    TestEqual(TEXT("the rising chirp is one partial, not several"), RisingTracks, 1);
    TestEqual(TEXT("the falling chirp is one partial, not several"), FallingTracks, 1);

    TestTrue(FString::Printf(TEXT("rising drift %.2f Hz is positive and matches the %.2f Hz sweep"),
        RisingDrift, ExpectedDriftHz),
        RisingDrift > 0.0 && FMath::Abs(RisingDrift - ExpectedDriftHz) <= DriftToleranceHz);

    TestTrue(FString::Printf(TEXT("falling drift %.2f Hz is negative and matches -%.2f Hz"),
        FallingDrift, ExpectedDriftHz),
        FallingDrift < 0.0 && FMath::Abs(FallingDrift + ExpectedDriftHz) <= DriftToleranceHz);

    TestTrue(FString::Printf(TEXT("the two sweeps have equal magnitude (%.2f vs %.2f Hz)"),
        FMath::Abs(RisingDrift), FMath::Abs(FallingDrift)),
        FMath::Abs(FMath::Abs(RisingDrift) - FMath::Abs(FallingDrift)) <= 10.0);

    return true;
}

// =============================================================================================
// C. PEAK REFINEMENT - a tone placed BETWEEN bins is recovered closer to truth than the bin centre.
//
// The tone sits at bin 42.3 of the coarse grid, i.e. 991.40625 Hz. The nearest bin centre is bin
// 42 at 984.375 Hz, 7.03 Hz away - that 7.03 Hz IS the error a peak-picker without interpolation
// would report, and it is computed here from the grid rather than hardcoded. 0.3 bins is close to
// the worst case for the log-parabolic estimator's own bias (which vanishes at 0 and 0.5 by
// symmetry), so this is the unfavourable offset, not a convenient one.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeParabolicInterpolationTest,
    "PinWright.audio.decompose.ParabolicInterpolationBeatsNearestBinCentre",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeParabolicInterpolationTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeTrackingTestHelpers;

    const double TrueHz = 42.3 * CoarseBinHz;                      // 991.40625 Hz
    const TArray<float> Samples = MakeSines(0.5, { TrueHz }, { 0.5 });

    FPwStftResult Stft;
    if (!TestTrue(TEXT("STFT computed"), BuildStft(Samples, CoarseFft, CoarseHop, Stft)))
    {
        return false;
    }

    TArray<FPwPartialTrack> Tracks;
    FString Code;
    FString Message;
    const bool bTracked = PwTrackPartials(Stft, TestSampleRate, ToneTrackingSettings(), Tracks, Code, Message);
    TestTrue(FString::Printf(TEXT("PwTrackPartials succeeded (%s: %s)"), *Code, *Message), bTracked);
    if (!bTracked || !TestEqual(TEXT("one tone gives one track"), Tracks.Num(), 1))
    {
        return false;
    }

    const double MeasuredHz = Tracks[0].MeanFreqHz;
    const double InterpolatedError = FMath::Abs(MeasuredHz - TrueHz);

    // The error the same peak would carry if it were reported at its bin centre.
    const double NearestBinHz = FMath::RoundToDouble(TrueHz / CoarseBinHz) * CoarseBinHz;
    const double BinCentreError = FMath::Abs(NearestBinHz - TrueHz);

    TestTrue(FString::Printf(TEXT("the reference bin-centre error is the expected %.3f Hz"), BinCentreError),
        FMath::Abs(BinCentreError - 0.3 * CoarseBinHz) < 0.01);

    TestTrue(FString::Printf(TEXT("interpolated %.4f Hz is within 3 Hz of the true %.4f Hz (error %.4f Hz)"),
        MeasuredHz, TrueHz, InterpolatedError), InterpolatedError <= 3.0);

    TestTrue(FString::Printf(
        TEXT("interpolation beats bin-picking: %.4f Hz error vs %.4f Hz at the bin centre"),
        InterpolatedError, BinCentreError),
        InterpolatedError < 0.4 * BinCentreError);

    return true;
}

// =============================================================================================
// D. PARTIAL TRACKING - the MaxPartials cap keeps the loudest AND reports the drop.
//
// Five tones, cap of three. Both halves are asserted: the right three survive, and the note names
// what went. A silently truncated list of three reads as "this sound has three partials", which is
// the exact defect rpc-design.md §1 is about - so the same input is also run with the cap out of
// the way, to prove the note is absent when nothing was dropped rather than always present.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeTruncationReportedTest,
    "PinWright.audio.decompose.MaxPartialsKeepsLoudestAndReportsTheDrop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeTruncationReportedTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeTrackingTestHelpers;

    // The quietest tone is 14 dB below the loudest, inside the -20 dB relative gate, so all five
    // are pickable and the cap is the only thing that can remove any of them.
    const TArray<double> Hz = { 400.0, 900.0, 1500.0, 2200.0, 3100.0 };
    const TArray<double> Amplitudes = { 0.5, 0.4, 0.3, 0.2, 0.1 };
    const TArray<float> Samples = MakeSines(0.5, Hz, Amplitudes);

    FPwStftResult Stft;
    if (!TestTrue(TEXT("STFT computed"), BuildStft(Samples, CoarseFft, CoarseHop, Stft)))
    {
        return false;
    }

    // ---- uncapped: five tracks, no note ------------------------------------------------------
    {
        TArray<FPwPartialTrack> Tracks;
        FString Code;
        FString Message;
        const bool bTracked = PwTrackPartials(Stft, TestSampleRate, ToneTrackingSettings(), Tracks, Code, Message);
        TestTrue(FString::Printf(TEXT("uncapped run succeeded (%s: %s)"), *Code, *Message), bTracked);
        TestEqual(TEXT("uncapped, all five tones are tracked"), Tracks.Num(), 5);
        TestTrue(FString::Printf(TEXT("uncapped, no truncation note (got '%s')"), *Message), Message.IsEmpty());
    }

    // ---- capped at three: the loudest three, and a note that says so -------------------------
    FPwDecomposeSettings Settings = ToneTrackingSettings();
    Settings.MaxPartials = 3;

    TArray<FPwPartialTrack> Tracks;
    FString Code;
    FString Message;
    const bool bTracked = PwTrackPartials(Stft, TestSampleRate, Settings, Tracks, Code, Message);

    TestTrue(FString::Printf(TEXT("capped run succeeded (%s: %s)"), *Code, *Message), bTracked);
    if (!bTracked)
    {
        return false;
    }

    TestEqual(TEXT("the cap is honoured exactly"), Tracks.Num(), 3);
    TestTrue(TEXT("truncation is a success, not an error code"), Code.IsEmpty());

    // The kept three must be the three LOUDEST, not the three lowest or the first three found.
    for (int32 Tone = 0; Tone < 3; ++Tone)
    {
        TestNotNull(*FString::Printf(TEXT("the %.0f Hz tone (amplitude %.1f) survived the cap"),
            Hz[Tone], Amplitudes[Tone]), FindTrackNear(Tracks, Hz[Tone], 3.5));
    }
    for (int32 Tone = 3; Tone < Hz.Num(); ++Tone)
    {
        TestNull(*FString::Printf(TEXT("the %.0f Hz tone (amplitude %.1f) was dropped by the cap"),
            Hz[Tone], Amplitudes[Tone]), FindTrackNear(Tracks, Hz[Tone], 3.5));
    }

    // THE assertion this test exists for: the caller is told the list is a truncation.
    TestTrue(FString::Printf(TEXT("the drop is reported with its counts (got '%s')"), *Message),
        Message.Contains(TEXT("kept 3 of 5")));

    return true;
}

// =============================================================================================
// E. HPSS - both directions in one test.
//
// A sustained tone is horizontal in the spectrogram and must land in the harmonic component; an
// impulse train is vertical and must land in the percussive one. Asserting only one of those
// passes a separator that dumps everything into a single bucket, which is why both signals run
// here and the two fractions are also compared against each other.
//
// The masks are Wiener masks summing to 1 per cell, so the split is also checked for energy
// conservation: harmonic + percussive must reconstruct the input magnitude bin for bin.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeHpssBothDirectionsTest,
    "PinWright.audio.decompose.HpssSplitsToneAndImpulsesInOppositeDirections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeHpssBothDirectionsTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeTrackingTestHelpers;

    auto SeparateFraction = [this](const TCHAR* Label, const TArray<float>& Samples, double& OutHarmonicFraction)
    {
        FPwStftResult Stft;
        if (!BuildStft(Samples, FineFft, FineHop, Stft))
        {
            AddError(FString::Printf(TEXT("%s: STFT failed"), Label));
            return false;
        }

        TArray<float> Harmonic;
        TArray<float> Percussive;
        FString Code;
        FString Message;
        if (!PwSeparateHarmonicPercussive(Stft, Harmonic, Percussive, Code, Message))
        {
            AddError(FString::Printf(TEXT("%s: HPSS failed (%s: %s)"), Label, *Code, *Message));
            return false;
        }

        TestEqual(FString::Printf(TEXT("%s: harmonic component has one value per cell"), Label),
            Harmonic.Num(), Stft.Magnitudes.Num());
        TestEqual(FString::Printf(TEXT("%s: percussive component has one value per cell"), Label),
            Percussive.Num(), Stft.Magnitudes.Num());

        // Energy conservation: the two masks sum to 1, so the parts must rebuild the whole.
        double WorstReconstructionError = 0.0;
        double PeakMagnitude = 0.0;
        for (int32 Cell = 0; Cell < Stft.Magnitudes.Num(); ++Cell)
        {
            const double Sum = static_cast<double>(Harmonic[Cell]) + static_cast<double>(Percussive[Cell]);
            WorstReconstructionError = FMath::Max(WorstReconstructionError,
                FMath::Abs(Sum - static_cast<double>(Stft.Magnitudes[Cell])));
            PeakMagnitude = FMath::Max(PeakMagnitude, static_cast<double>(Stft.Magnitudes[Cell]));
        }
        TestTrue(FString::Printf(
            TEXT("%s: harmonic + percussive reconstructs the input (worst cell off by %.3g against a %.3g peak)"),
            Label, WorstReconstructionError, PeakMagnitude),
            WorstReconstructionError <= 1e-5 * PeakMagnitude + 1e-7);

        OutHarmonicFraction = HarmonicEnergyFraction(Harmonic, Percussive);
        return true;
    };

    // A loud 1 kHz tone with a modest noise bed. The noise is neither horizontal nor vertical, so
    // it splits roughly evenly and cannot carry the result on its own; the tone decides it.
    TArray<float> ToneSamples = MakeSines(0.5, { 1000.0 }, { 0.5 });
    AddWhiteNoise(ToneSamples, 0.05, 20260817);

    // Impulses 100 ms apart. At the fine grid an impulse occupies about 4 frames, far under half
    // the 31-frame time kernel, which is the condition under which the time median rejects it.
    const TArray<float> ImpulseSamples = MakeImpulses(0.5, { 100.0, 200.0, 300.0, 400.0 });

    double ToneHarmonicFraction = 0.0;
    double ImpulseHarmonicFraction = 0.0;
    if (!SeparateFraction(TEXT("tone+noise"), ToneSamples, ToneHarmonicFraction)
        || !SeparateFraction(TEXT("impulse train"), ImpulseSamples, ImpulseHarmonicFraction))
    {
        return false;
    }

    TestTrue(FString::Printf(TEXT("a sustained tone is mostly harmonic (%.3f of the energy)"),
        ToneHarmonicFraction), ToneHarmonicFraction > 0.75);

    TestTrue(FString::Printf(TEXT("an impulse train is mostly percussive (%.3f of the energy is harmonic)"),
        ImpulseHarmonicFraction), (1.0 - ImpulseHarmonicFraction) > 0.70);

    // The cross-check that kills a one-bucket separator: the two signals must be separated in
    // opposite directions, by a wide margin, not merely land on opposite sides of 0.5.
    TestTrue(FString::Printf(
        TEXT("the two signals separate in opposite directions (tone %.3f harmonic vs impulses %.3f)"),
        ToneHarmonicFraction, ImpulseHarmonicFraction),
        ToneHarmonicFraction - ImpulseHarmonicFraction > 0.5);

    return true;
}

// =============================================================================================
// F. TRANSIENTS - one characterised burst per impulse, at the right time and with a sane shape.
//
// The onsets come from PwDetectOnsets rather than being hand-written, because that is the seed
// path the production caller uses. Time tolerance 6 ms: the frame centres are on a 1.333 ms grid,
// the loudest frame is the one whose window centre is nearest the impulse (so within half a hop of
// it in principle), and the remaining slack covers the tie between two equidistant frames.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeTransientsTest,
    "PinWright.audio.decompose.TransientsCharacteriseEveryImpulse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeTransientsTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeTrackingTestHelpers;

    // The same four-impulse signal PwDetectOnsets' own test is calibrated on, so a failure here
    // is a transient-characterisation failure and not a re-litigation of onset detection. Every
    // one of these times lands exactly on an analysis frame centre on the 256/64 grid
    // ((T*48 - 128) / 64 is a whole number for all four), so the expected peak frame is exact.
    const TArray<double> ImpulseMs = { 100.0, 300.0, 500.0, 700.0 };
    const TArray<float> Samples = MakeImpulses(1.0, ImpulseMs);

    FPwStftResult Stft;
    if (!TestTrue(TEXT("STFT computed"), BuildStft(Samples, OnsetFft, OnsetHop, Stft)))
    {
        return false;
    }

    TArray<FPwOnset> Onsets;
    FString Code;
    FString Message;
    if (!TestTrue(TEXT("onset detection ran"),
        PwDetectOnsets(Stft, TestSampleRate, Onsets, Code, Message)))
    {
        return false;
    }
    TestEqual(TEXT("four impulses give four onset seeds"), Onsets.Num(), 4);

    TArray<FPwTransient> Transients;
    const bool bDetected = PwDetectTransients(Stft, TestSampleRate, Onsets, Transients, Code, Message);
    TestTrue(FString::Printf(TEXT("PwDetectTransients succeeded (%s: %s)"), *Code, *Message), bDetected);
    if (!bDetected)
    {
        return false;
    }

    TestEqual(TEXT("one transient per seed"), Transients.Num(), ImpulseMs.Num());
    if (Transients.Num() != ImpulseMs.Num())
    {
        return false;
    }

    for (int32 Index = 0; Index < Transients.Num(); ++Index)
    {
        const FPwTransient& Transient = Transients[Index];
        const double ExpectedMs = ImpulseMs[Index];

        TestTrue(FString::Printf(TEXT("transient %d peaks at %.2f ms, expected %.0f ms +/- 6"),
            Index, Transient.PeakMs, ExpectedMs),
            FMath::Abs(Transient.PeakMs - ExpectedMs) <= 6.0);

        TestTrue(FString::Printf(TEXT("transient %d is bracketed: %.2f <= %.2f <= %.2f ms"),
            Index, Transient.StartMs, Transient.PeakMs, Transient.EndMs),
            Transient.StartMs <= Transient.PeakMs && Transient.PeakMs <= Transient.EndMs);

        // A single-sample impulse has a flat spectrum, so its power centroid sits near half of
        // Nyquist (12 kHz here) and the 5/95 percentiles must straddle it.
        TestTrue(FString::Printf(TEXT("transient %d centroid %.0f Hz is broadband"),
            Index, Transient.CentroidHz),
            Transient.CentroidHz > 7000.0 && Transient.CentroidHz < 17000.0);
        TestTrue(FString::Printf(TEXT("transient %d percentiles straddle the centroid (%.0f < %.0f < %.0f Hz)"),
            Index, Transient.LowHz, Transient.CentroidHz, Transient.HighHz),
            Transient.LowHz < Transient.CentroidHz && Transient.CentroidHz < Transient.HighHz);
        TestTrue(FString::Printf(TEXT("transient %d bandwidth %.0f Hz is positive"),
            Index, Transient.BandwidthHz), Transient.BandwidthHz > 0.0);

        // A full-scale impulse spreads its energy over every bin, so no single bin is anywhere
        // near 0 dBFS - but it is far above a silence floor. Both bounds matter: the upper one
        // catches a level computed off the summed frame power and mislabelled as a bin level.
        TestTrue(FString::Printf(TEXT("transient %d peak level %.1f dBFS is a real measurement"),
            Index, Transient.PeakDb),
            Transient.PeakDb > -80.0 && Transient.PeakDb < 0.0);

        if (Index > 0)
        {
            TestTrue(FString::Printf(TEXT("transient %d starts after transient %d ends"), Index, Index - 1),
                Transient.StartMs > Transients[Index - 1].EndMs);
        }
    }

    return true;
}

// =============================================================================================
// G. TRANSIENTS - no seeds is an empty SUCCESS, not a failure and not an invented transient.
//
// The false-positive direction is the one that costs the caller (§6): a fabricated transient sends
// the agent to add an attack layer to a sound that has none.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeNoOnsetsTest,
    "PinWright.audio.decompose.NoOnsetsIsAnEmptySuccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeNoOnsetsTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeTrackingTestHelpers;

    const TArray<float> Samples = MakeSines(0.5, { 1000.0 }, { 0.5 });

    FPwStftResult Stft;
    if (!TestTrue(TEXT("STFT computed"), BuildStft(Samples, OnsetFft, OnsetHop, Stft)))
    {
        return false;
    }

    TArray<FPwTransient> Transients;
    Transients.AddDefaulted();          // pre-filled, so "cleared on entry" is actually tested
    FString Code;
    FString Message;
    const bool bDetected = PwDetectTransients(Stft, TestSampleRate, TArray<FPwOnset>(), Transients, Code, Message);

    TestTrue(TEXT("an empty seed list is a legal call"), bDetected);
    TestEqual(TEXT("no seeds means no transients"), Transients.Num(), 0);
    TestTrue(TEXT("no error code"), Code.IsEmpty());

    return true;
}

// =============================================================================================
// H. FAILURE DIRECTION (§12) - empty, non-finite, silent and over-limit inputs.
//
// The ordering case is the one that matters most: FMath::Max(0.0, NaN) returns 0 because every
// comparison against NaN is false, so an all-zero spectrogram carrying a single NaN measures as
// digital silence unless the non-finite scan runs FIRST. The third block below is exactly that
// input, and it must come back AUDIO_NON_FINITE_SAMPLES rather than AUDIO_EMPTY_BUFFER.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeFailureDirectionTest,
    "PinWright.audio.decompose.DegenerateInputsFailUnmeasuredWithTheRightCode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeFailureDirectionTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeTrackingTestHelpers;

    // Runs the same input through all three entry points and asserts the same code from each, so
    // the shared degenerate-case gate cannot drift apart between them.
    auto ExpectRejection = [this](const TCHAR* Label, const FPwStftResult& Stft, const TCHAR* ExpectedCode)
    {
        {
            TArray<float> Harmonic;
            TArray<float> Percussive;
            Harmonic.AddZeroed(3);
            Percussive.AddZeroed(3);
            FString Code;
            FString Message;
            const bool bSeparated = PwSeparateHarmonicPercussive(Stft, Harmonic, Percussive, Code, Message);
            TestFalse(FString::Printf(TEXT("%s: HPSS fails"), Label), bSeparated);
            TestEqual(FString::Printf(TEXT("%s: HPSS code"), Label), Code, FString(ExpectedCode));
            TestEqual(FString::Printf(TEXT("%s: HPSS leaves the harmonic output cleared"), Label),
                Harmonic.Num(), 0);
            TestEqual(FString::Printf(TEXT("%s: HPSS leaves the percussive output cleared"), Label),
                Percussive.Num(), 0);
            TestFalse(FString::Printf(TEXT("%s: HPSS explains itself"), Label), Message.IsEmpty());
        }
        {
            TArray<FPwPartialTrack> Tracks;
            Tracks.AddDefaulted();
            FString Code;
            FString Message;
            const bool bTracked = PwTrackPartials(Stft, TestSampleRate, FPwDecomposeSettings(),
                Tracks, Code, Message);
            TestFalse(FString::Printf(TEXT("%s: tracking fails"), Label), bTracked);
            TestEqual(FString::Printf(TEXT("%s: tracking code"), Label), Code, FString(ExpectedCode));
            TestEqual(FString::Printf(TEXT("%s: tracking leaves the track list cleared"), Label),
                Tracks.Num(), 0);
        }
        {
            TArray<FPwTransient> Transients;
            Transients.AddDefaulted();
            TArray<FPwOnset> Onsets;
            FPwOnset& Onset = Onsets.AddDefaulted_GetRef();
            Onset.TimeMs = 10.0;
            FString Code;
            FString Message;
            const bool bDetected = PwDetectTransients(Stft, TestSampleRate, Onsets, Transients, Code, Message);
            TestFalse(FString::Printf(TEXT("%s: transients fail"), Label), bDetected);
            TestEqual(FString::Printf(TEXT("%s: transient code"), Label), Code, FString(ExpectedCode));
            TestEqual(FString::Printf(TEXT("%s: transients leave the list cleared"), Label),
                Transients.Num(), 0);
        }
    };

    // 1. Nothing at all.
    ExpectRejection(TEXT("empty spectrogram"), FPwStftResult(), ErrorCodes::ERR_AUDIO_EMPTY_BUFFER);

    // 2. Well-formed but digitally silent. Named as silence, and NOT confused with case 1.
    //    PwComputeStft refuses a silent input itself, so the silent spectrogram is built by
    //    zeroing a real one - which is the point, since this has to exercise the decomposition
    //    analyzer's own gate rather than the STFT's.
    {
        FPwStftResult Real;
        if (TestTrue(TEXT("reference STFT computed"),
            BuildStft(MakeSines(0.2, { 1000.0 }, { 0.5 }), CoarseFft, CoarseHop, Real)))
        {
            FPwStftResult Zeroed = Real;
            for (float& Magnitude : Zeroed.Magnitudes)
            {
                Magnitude = 0.f;
            }
            ExpectRejection(TEXT("digitally silent spectrogram"), Zeroed, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER);

            // 3. THE ORDERING CASE. All zeros except one NaN: the peak scan alone reads this as
            //    silence, so only a non-finite check that runs first can name it correctly.
            FPwStftResult NaNInSilence = Zeroed;
            NaNInSilence.Magnitudes[NaNInSilence.Magnitudes.Num() / 2] = MakeNaN();
            ExpectRejection(TEXT("one NaN in an otherwise silent spectrogram"), NaNInSilence,
                ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES);

            // 4. One NaN in real signal - the realistic case, an unstable filter upstream.
            FPwStftResult NaNInSignal = Real;
            NaNInSignal.Magnitudes[17] = MakeNaN();
            ExpectRejection(TEXT("one NaN in real audio"), NaNInSignal,
                ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES);

            // 5. Infinity is the mirror case: it survives a silence test and poisons every sum.
            FPwStftResult InfiniteInSignal = Real;
            InfiniteInSignal.Magnitudes[23] = MakeInfinity();
            ExpectRejection(TEXT("one infinity in real audio"), InfiniteInSignal,
                ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES);

            // 6. Over the cost cap. An ERROR naming the limit, never a silent crop.
            FPwDecomposeSettings Settings;
            Settings.MaxDurationMs = 10;

            TArray<FPwPartialTrack> Tracks;
            Tracks.AddDefaulted();
            FString Code;
            FString OverLimitMessage;
            const bool bTracked = PwTrackPartials(Real, TestSampleRate, Settings, Tracks, Code, OverLimitMessage);

            TestFalse(TEXT("an input over maxDurationMs is refused"), bTracked);
            TestEqual(TEXT("over-limit code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
            TestEqual(TEXT("over-limit leaves the track list cleared"), Tracks.Num(), 0);
            TestTrue(FString::Printf(TEXT("the message names the limit (got '%s')"), *OverLimitMessage),
                OverLimitMessage.Contains(TEXT("10 ms")));
            TestTrue(FString::Printf(TEXT("the message names the measurement (got '%s')"), *OverLimitMessage),
                OverLimitMessage.Contains(TEXT("197.3")));
        }
    }

    return true;
}

// =============================================================================================
// I. HPSS - a spectrogram with no usable time axis is refused, not reported as all-harmonic.
//
// With one or two frames the time median is the cell itself, so the harmonic mask would be 1
// everywhere and the answer "this sound is entirely harmonic" would be an artefact of the window.
// Refusing is the honest outcome (§1).
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeHpssTooFewFramesTest,
    "PinWright.audio.decompose.HpssRefusesASpectrogramWithNoTimeAxis",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeHpssTooFewFramesTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeTrackingTestHelpers;

    FPwStftResult Real;
    if (!TestTrue(TEXT("STFT computed"),
        BuildStft(MakeSines(0.2, { 1000.0 }, { 0.5 }), CoarseFft, CoarseHop, Real)))
    {
        return false;
    }

    // Trim to two frames, keeping the magnitude array consistent so IsValid() still holds - the
    // rejection must come from the frame-count rule, not from a malformed-input fallback.
    FPwStftResult TwoFrames = Real;
    TwoFrames.NumFrames = 2;
    TwoFrames.Magnitudes.SetNum(2 * TwoFrames.NumBins);

    TArray<float> Harmonic;
    TArray<float> Percussive;
    FString Code;
    FString Message;
    const bool bSeparated = PwSeparateHarmonicPercussive(TwoFrames, Harmonic, Percussive, Code, Message);

    TestFalse(TEXT("two frames are refused"), bSeparated);
    TestEqual(TEXT("code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestEqual(TEXT("harmonic output is cleared"), Harmonic.Num(), 0);
    TestEqual(TEXT("percussive output is cleared"), Percussive.Num(), 0);
    TestTrue(FString::Printf(TEXT("the message names the requirement (got '%s')"), *Message),
        Message.Contains(TEXT("at least 3")));

    // Three frames is the boundary and must be accepted, so the rule is a floor rather than a
    // blanket refusal of short inputs.
    FPwStftResult ThreeFrames = Real;
    ThreeFrames.NumFrames = 3;
    ThreeFrames.Magnitudes.SetNum(3 * ThreeFrames.NumBins);

    Harmonic.Reset();
    Percussive.Reset();
    const bool bBoundary = PwSeparateHarmonicPercussive(ThreeFrames, Harmonic, Percussive, Code, Message);
    TestTrue(FString::Printf(TEXT("three frames are accepted (%s: %s)"), *Code, *Message), bBoundary);
    TestEqual(TEXT("three frames produce a full harmonic component"),
        Harmonic.Num(), 3 * ThreeFrames.NumBins);

    return true;
}

// =============================================================================================
// J. PARTIAL TRACKING - nonsensical settings are named rather than absorbed.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeSettingsValidationTest,
    "PinWright.audio.decompose.NonsensicalSettingsAreRejectedByName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeSettingsValidationTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeTrackingTestHelpers;

    FPwStftResult Stft;
    if (!TestTrue(TEXT("STFT computed"),
        BuildStft(MakeSines(0.2, { 1000.0 }, { 0.5 }), CoarseFft, CoarseHop, Stft)))
    {
        return false;
    }

    auto ExpectSettingsRejection = [this, &Stft](const TCHAR* Label, const FPwDecomposeSettings& Settings,
        const TCHAR* ExpectedFragment)
    {
        TArray<FPwPartialTrack> Tracks;
        Tracks.AddDefaulted();
        FString Code;
        FString Message;
        const bool bTracked = PwTrackPartials(Stft, TestSampleRate, Settings, Tracks, Code, Message);
        TestFalse(FString::Printf(TEXT("%s is refused"), Label), bTracked);
        TestEqual(FString::Printf(TEXT("%s code"), Label), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestEqual(FString::Printf(TEXT("%s leaves the track list cleared"), Label), Tracks.Num(), 0);
        TestTrue(FString::Printf(TEXT("%s names the offending field (got '%s')"), Label, *Message),
            Message.Contains(ExpectedFragment));
    };

    {
        FPwDecomposeSettings Settings;
        Settings.MaxPartials = 0;
        ExpectSettingsRejection(TEXT("maxPartials of 0"), Settings, TEXT("maxPartials"));
    }
    {
        FPwDecomposeSettings Settings;
        Settings.PartialMinDurationMs = -1.0;
        ExpectSettingsRejection(TEXT("a negative partialMinDurationMs"), Settings, TEXT("partialMinDurationMs"));
    }
    {
        // A threshold relative to the frame maximum can only ever be negative; at 0 or above it
        // admits nothing, which would silently produce an empty and confident track list.
        FPwDecomposeSettings Settings;
        Settings.PartialMinAmpDb = 6.0;
        ExpectSettingsRejection(TEXT("a positive partialMinAmpDb"), Settings, TEXT("partialMinAmpDb"));
    }
    {
        FPwDecomposeSettings Settings;
        Settings.MaxDurationMs = 0;
        ExpectSettingsRejection(TEXT("maxDurationMs of 0"), Settings, TEXT("maxDurationMs"));
    }

    return true;
}
