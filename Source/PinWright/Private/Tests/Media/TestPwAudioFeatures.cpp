// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the three feature extractors in AudioGen/PwAudioFeatures.h.
//
// Every assertion here is against analytic ground truth rather than against "it returned true":
// a sine of known amplitude has a derivable LUFS value, an impulse train has known onset times,
// and a synthesised tone has a known f0. Where a number is asserted, the comment above it derives
// it, and the tolerance is chosen so the plausible implementation bugs fall OUTSIDE it - the
// point of the -20/-26 dBFS pair, for instance, is that an absolute test alone passes a scale
// error that the 6 LU relative test catches.
//
// Per rpc-design.md §12 the failure direction is asserted too, and per §6 both directions of each
// detector are: onsets are checked for false negatives (four impulses must all be found) AND for
// false positives (a smooth tone must yield none), and pitch is checked for a right answer on a
// tone AND for a refusal to name one on noise.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioFeatures.h"
#include "AudioGen/PwStft.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/ErrorCodes.h"

#include "Math/RandomStream.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true.
namespace PwAudioFeaturesTestHelpers
{
    constexpr int32 TestSampleRate = 48000;

    /** Onset analysis settings used by every onset test, kept in one place so the derived
     *  tolerance below stays tied to them. 256/64 at 48 kHz is a 5.33 ms window on a 1.33 ms
     *  grid, small enough that the flux detector's inherent window latency stays under 2 hops. */
    constexpr int32 OnsetFftSize = 256;
    constexpr int32 OnsetHopSize = 64;

    /**
     * Onset time tolerance, in ms. Derived, not guessed: for a tapered analysis window the
     * spectral flux of an impulse peaks when the impulse has reached roughly the three-quarter
     * point of the window, so the reported frame-centre time leads the true transient by about
     * a quarter of a window (64 samples here) plus up to half a hop of frame quantization (32).
     * FftSize/2 = 128 samples = 2.67 ms bounds both with room to spare, and is two hops - the
     * "within a frame or two" the detector claims.
     */
    constexpr double OnsetToleranceMs = 1000.0 * (OnsetFftSize / 2) / static_cast<double>(TestSampleRate);

    TArray<float> MakeSine(double DurationSeconds, double Hz, double PeakAmplitude)
    {
        const int32 NumSamples = FMath::RoundToInt(DurationSeconds * TestSampleRate);
        TArray<float> Samples;
        Samples.SetNumZeroed(NumSamples);
        const double AngularStep = 2.0 * UE_DOUBLE_PI * Hz / static_cast<double>(TestSampleRate);
        for (int32 Index = 0; Index < NumSamples; ++Index)
        {
            Samples[Index] = static_cast<float>(PeakAmplitude * FMath::Sin(AngularStep * Index));
        }
        return Samples;
    }

    /** Linear chirp from StartHz to EndHz across the whole buffer. */
    TArray<float> MakeChirp(double DurationSeconds, double StartHz, double EndHz, double PeakAmplitude)
    {
        const int32 NumSamples = FMath::RoundToInt(DurationSeconds * TestSampleRate);
        TArray<float> Samples;
        Samples.SetNumZeroed(NumSamples);
        const double Sweep = (EndHz - StartHz) / DurationSeconds;
        for (int32 Index = 0; Index < NumSamples; ++Index)
        {
            const double T = static_cast<double>(Index) / static_cast<double>(TestSampleRate);
            const double Phase = 2.0 * UE_DOUBLE_PI * (StartHz * T + 0.5 * Sweep * T * T);
            Samples[Index] = static_cast<float>(PeakAmplitude * FMath::Sin(Phase));
        }
        return Samples;
    }

    /** Deterministic uniform white noise. FRandomStream so the test is reproducible. */
    TArray<float> MakeWhiteNoise(double DurationSeconds, int32 Seed)
    {
        const int32 NumSamples = FMath::RoundToInt(DurationSeconds * TestSampleRate);
        TArray<float> Samples;
        Samples.SetNumUninitialized(NumSamples);
        FRandomStream Stream(Seed);
        for (int32 Index = 0; Index < NumSamples; ++Index)
        {
            Samples[Index] = static_cast<float>(Stream.FRandRange(-1.0, 1.0));
        }
        return Samples;
    }

    /**
     * Zero everywhere except one sample at each requested millisecond. A single-sample impulse
     * has a perfectly flat spectrum, so every bin contributes to the flux and the onset is as
     * unambiguous as the detector will ever see.
     */
    TArray<float> MakeImpulses(double DurationSeconds, const TArray<double>& TimesMs,
        const TArray<double>& Amplitudes)
    {
        const int32 NumSamples = FMath::RoundToInt(DurationSeconds * TestSampleRate);
        TArray<float> Samples;
        Samples.SetNumZeroed(NumSamples);
        for (int32 Impulse = 0; Impulse < TimesMs.Num(); ++Impulse)
        {
            const int32 Index = FMath::RoundToInt(TimesMs[Impulse] * TestSampleRate / 1000.0);
            const double Amplitude = Amplitudes.IsValidIndex(Impulse) ? Amplitudes[Impulse] : 1.0;
            if (Samples.IsValidIndex(Index))
            {
                Samples[Index] = static_cast<float>(Amplitude);
            }
        }
        return Samples;
    }

    /** Steady tone with raised-cosine fades - level changes everywhere, transients nowhere. */
    TArray<float> MakeFadedTone(double DurationSeconds, double Hz, double PeakAmplitude, double FadeSeconds)
    {
        TArray<float> Samples = MakeSine(DurationSeconds, Hz, PeakAmplitude);
        const int32 FadeSampleCount = FMath::Min(FMath::RoundToInt(FadeSeconds * TestSampleRate), Samples.Num() / 2);
        for (int32 Index = 0; Index < FadeSampleCount; ++Index)
        {
            const double Gain = 0.5 * (1.0 - FMath::Cos(UE_DOUBLE_PI * Index / static_cast<double>(FadeSampleCount)));
            Samples[Index] = static_cast<float>(Samples[Index] * Gain);
            Samples[Samples.Num() - 1 - Index] = static_cast<float>(Samples[Samples.Num() - 1 - Index] * Gain);
        }
        return Samples;
    }

    /** Dual-mono buffer: the same samples in both channels, which is what a mono source decodes to. */
    FPwAudioBuffer MakeDualMono(const TArray<float>& Samples)
    {
        FPwAudioBuffer Buffer;
        Buffer.SampleRate = TestSampleRate;
        Buffer.SetNumFrames(Samples.Num());
        for (int32 Index = 0; Index < Samples.Num(); ++Index)
        {
            Buffer.Left[Index] = Samples[Index];
            Buffer.Right[Index] = Samples[Index];
        }
        return Buffer;
    }
}

// LOUDNESS MEASUREMENT AND THE ENGINE. Sections A and B measure LUFS, which PwComputeLoudness
// obtains from Audio::FLKFSAnalyzer - the engine's ITU-R BS.1770 meter, which ships from UE 5.8
// only. On 5.3-5.7 the function refuses the request outright rather than publishing a peak or a
// perceptual-loudness figure under the LUFS name, so there is no measurement for these two
// sections to check. The refusal itself is asserted in section C, unguarded, which is what keeps
// the absence measured rather than merely skipped.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)

// =========================================================================================
// A. LOUDNESS - absolute value against a derived LUFS figure.
//
// Derivation for a dual-mono 1 kHz sine of peak amplitude A, from ITU-R BS.1770 as the engine
// implements it (FLoudnessAnalyzer with ELoudnessCurveType::K and the Corrected scaling, summed
// over channels by FMultichannelLoudnessAnalyzer):
//
//   per-channel K-weighted mean square  z = K(1kHz)^2 * A^2 / 2
//   channel weights                     G_L = G_R = 1.0  (BS.1770 for front L/R)
//   summed energy                       E = G_L*z + G_R*z = K(1kHz)^2 * A^2
//   loudness                            LKFS = -0.691 + 10*log10(E)
//
// K(1000 Hz) is the magnitude of the BS.1770 pre-filter times the RLB filter at 1 kHz, which the
// engine evaluates from the same biquad coefficients the standard prints:
//   K(1000) = 1.083640, so 10*log10(K^2) = +0.6977 dB.
//
// For A = 0.1 (-20 dBFS peak):
//   10*log10(0.1^2) = -20.000 dB;  -20.000 + 0.698 - 0.691 = -19.993 LUFS.
//
// A numerical run of the exact analyzer pipeline (4096-point rectangular window, bins 2..2048,
// the engine's own per-bin K^2 table, HalfSpectrumScaling 2, window-energy normalization) lands
// at -19.990 LUFS across five different window phases, i.e. the leakage and the K-curve
// integration cost 0.004 LU.
//
// Tolerance 0.5 LU. It is 100x the modelling residue, and every mistake worth catching is far
// outside it: forgetting to interleave, or feeding one channel, or dropping the BS.1770 channel
// sum, all move the answer by 3 dB or more.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwLoudnessSineMatchesDerivedLufsTest,
    "PinWright.audio.features.LoudnessOfMinus20dBFSSineMatchesDerivedLufs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwLoudnessSineMatchesDerivedLufsTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioFeaturesTestHelpers;

    // 2 s: FLKFSAnalyzer needs ~0.44 s before it emits its first result at 48 kHz, and this
    // leaves room for the 1 s integrated-loudness update period to fire more than once.
    const FPwAudioBuffer Buffer = MakeDualMono(MakeSine(2.0, 1000.0, 0.1));

    FPwLoudnessResult Result;
    FString Code;
    FString Message;
    const bool bMeasured = PwComputeLoudness(Buffer, Result, Code, Message);

    TestTrue(FString::Printf(TEXT("PwComputeLoudness succeeded (%s: %s)"), *Code, *Message), bMeasured);
    if (!bMeasured)
    {
        return false;
    }
    TestTrue(TEXT("bMeasured is set only after a real measurement"), Result.bMeasured);

    constexpr double ExpectedLufs = -19.99;
    constexpr double LufsTolerance = 0.5;
    TestTrue(FString::Printf(TEXT("IntegratedLufs is %.3f, expected %.2f +/- %.2f"),
        Result.IntegratedLufs, ExpectedLufs, LufsTolerance),
        FMath::Abs(Result.IntegratedLufs - ExpectedLufs) <= LufsTolerance);
    TestTrue(FString::Printf(TEXT("MomentaryMaxLufs is %.3f, expected %.2f +/- %.2f"),
        Result.MomentaryMaxLufs, ExpectedLufs, LufsTolerance),
        FMath::Abs(Result.MomentaryMaxLufs - ExpectedLufs) <= LufsTolerance);
    TestTrue(FString::Printf(TEXT("ShortTermMaxLufs is %.3f, expected %.2f +/- %.2f"),
        Result.ShortTermMaxLufs, ExpectedLufs, LufsTolerance),
        FMath::Abs(Result.ShortTermMaxLufs - ExpectedLufs) <= LufsTolerance);

    // Peak and RMS are exact for this signal: 1000 Hz at 48 kHz puts a sample on every quarter
    // cycle, so the sine reaches exactly +/-A, and 2 s is exactly 2000 whole cycles so the mean
    // square is exactly A^2/2 - i.e. RMS sits 3.01 dB below peak, not at it.
    TestTrue(FString::Printf(TEXT("PeakDb is %.4f, expected -20.000"), Result.PeakDb),
        FMath::Abs(Result.PeakDb - (-20.0)) <= 0.05);
    TestTrue(FString::Printf(TEXT("RmsDb is %.4f, expected -23.010"), Result.RmsDb),
        FMath::Abs(Result.RmsDb - (-23.0103)) <= 0.05);

    // A steady tone has no loudness range worth the name. The flag is asserted FIRST and
    // separately: without it the ~0 below passes identically on a range the analyzer produced and
    // on one it never produced, which would make bLoudnessRangeMeasured decorative.
    TestTrue(TEXT("The loudness range was actually measured, not left at the 0 default"),
        Result.bLoudnessRangeMeasured);
    TestTrue(FString::Printf(TEXT("LoudnessRangeLu is %.3f, expected ~0 for a steady tone"),
        Result.LoudnessRangeLu), Result.LoudnessRangeLu >= 0.0 && Result.LoudnessRangeLu < 1.0);

    return true;
}

// =========================================================================================
// B. LOUDNESS - the relative check. A 6 dB quieter sine must read exactly 6 LU quieter. This is
//    the assertion that catches a scale error (a stray factor of two, a power-vs-amplitude mixup,
//    a channel counted twice) which an absolute test with a 0.5 LU window can absorb.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwLoudnessTracksLevelChangeTest,
    "PinWright.audio.features.LoudnessDropsSixLuForSixDbQuieterSine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwLoudnessTracksLevelChangeTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioFeaturesTestHelpers;

    // 10^(-20/20) = 0.1 and 10^(-26/20) = 0.0501187: exactly 6 dB apart.
    const FPwAudioBuffer Loud = MakeDualMono(MakeSine(2.0, 1000.0, 0.1));
    const FPwAudioBuffer Quiet = MakeDualMono(MakeSine(2.0, 1000.0, 0.05011872336));

    FPwLoudnessResult LoudResult;
    FPwLoudnessResult QuietResult;
    FString Code;
    FString Message;
    const bool bLoudOk = PwComputeLoudness(Loud, LoudResult, Code, Message);
    TestTrue(FString::Printf(TEXT("Loud measurement succeeded (%s: %s)"), *Code, *Message), bLoudOk);
    const bool bQuietOk = PwComputeLoudness(Quiet, QuietResult, Code, Message);
    TestTrue(FString::Printf(TEXT("Quiet measurement succeeded (%s: %s)"), *Code, *Message), bQuietOk);
    if (!bLoudOk || !bQuietOk)
    {
        return false;
    }

    // The K weighting, the leakage and the channel sum are identical for the two signals, so the
    // difference is the level difference and nothing else. Tolerance 0.15 LU is 100x the float32
    // noise on the measurement and 40x smaller than the smallest interesting error (3 LU).
    const double IntegratedDelta = LoudResult.IntegratedLufs - QuietResult.IntegratedLufs;
    TestTrue(FString::Printf(TEXT("IntegratedLufs delta is %.4f LU, expected 6.000"), IntegratedDelta),
        FMath::Abs(IntegratedDelta - 6.0) <= 0.15);

    const double MomentaryDelta = LoudResult.MomentaryMaxLufs - QuietResult.MomentaryMaxLufs;
    TestTrue(FString::Printf(TEXT("MomentaryMaxLufs delta is %.4f LU, expected 6.000"), MomentaryDelta),
        FMath::Abs(MomentaryDelta - 6.0) <= 0.15);

    const double PeakDelta = LoudResult.PeakDb - QuietResult.PeakDb;
    TestTrue(FString::Printf(TEXT("PeakDb delta is %.4f dB, expected 6.000"), PeakDelta),
        FMath::Abs(PeakDelta - 6.0) <= 0.05);

    return true;
}

#endif // LUFS measurement (UE 5.8+)

// =========================================================================================
// C. LOUDNESS - failure direction (rpc-design.md §12). An empty buffer and a silent buffer must
//    fail with the existence code, not come back with a very small number that reads as
//    "extremely quiet audio".
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwLoudnessRejectsEmptyAndSilentTest,
    "PinWright.audio.features.LoudnessRejectsEmptyAndSilentBuffers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwLoudnessRejectsEmptyAndSilentTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioFeaturesTestHelpers;

    {
        FPwAudioBuffer Empty;
        Empty.SampleRate = TestSampleRate;
        FPwLoudnessResult Result;
        FString Code;
        FString Message;

        TestFalse(TEXT("An empty buffer is rejected"), PwComputeLoudness(Empty, Result, Code, Message));
        TestEqual(TEXT("Code is AUDIO_EMPTY_BUFFER"), Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        TestFalse(TEXT("Failure carries a message"), Message.IsEmpty());
        TestFalse(TEXT("bMeasured stays false"), Result.bMeasured);
        TestEqual(TEXT("IntegratedLufs stays 0, not -inf"), Result.IntegratedLufs, 0.0);
        TestEqual(TEXT("PeakDb stays 0, not -inf"), Result.PeakDb, 0.0);
    }

    {
        // Two seconds of digital silence: long enough that every other check would pass.
        FPwAudioBuffer Silence;
        Silence.SampleRate = TestSampleRate;
        Silence.SetNumFrames(2 * TestSampleRate);
        FPwLoudnessResult Result;
        FString Code;
        FString Message;

        TestFalse(TEXT("A silent buffer is rejected"), PwComputeLoudness(Silence, Result, Code, Message));
        TestEqual(TEXT("Silence is named AUDIO_EMPTY_BUFFER, not measured as -inf LUFS"),
            Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        TestNotEqual(TEXT("Silence is specifically NOT reported as a parameter problem"),
            Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestFalse(TEXT("bMeasured stays false"), Result.bMeasured);
    }

    {
        // Too short to measure. Named as a parameter problem with the requirement in the message,
        // rather than reported as a loudness derived from no analysis window at all.
        const FPwAudioBuffer TooShort = MakeDualMono(MakeSine(0.1, 1000.0, 0.5));
        FPwLoudnessResult Result;
        FString Code;
        FString Message;

        TestFalse(TEXT("A 0.1 s buffer is rejected"), PwComputeLoudness(TooShort, Result, Code, Message));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        TestEqual(TEXT("Code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("The message states the measured duration"), Message.Contains(TEXT("0.100")));
#else
        // On an engine with no BS.1770 meter the too-short check is never reached, because there
        // is no analysis window to be too short for. The refusal is the engine one, and it has to
        // name the engine and the version that carries the meter - a caller reading it must be
        // able to tell "this editor cannot" from "this buffer cannot".
        TestEqual(TEXT("Code is UNSUPPORTED_ENGINE_VERSION"), Code,
            FString(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION));
        TestTrue(TEXT("The message names the engine that carries the meter"),
            Message.Contains(TEXT("5.8")));
#endif
        TestFalse(TEXT("bMeasured stays false"), Result.bMeasured);
        TestEqual(TEXT("IntegratedLufs stays 0"), Result.IntegratedLufs, 0.0);
    }

    {
        // Mismatched channels: a structural problem, and distinguishable from emptiness.
        FPwAudioBuffer Ragged;
        Ragged.SampleRate = TestSampleRate;
        Ragged.Left = MakeSine(1.0, 1000.0, 0.5);
        Ragged.Right = MakeSine(0.5, 1000.0, 0.5);
        FPwLoudnessResult Result;
        FString Code;
        FString Message;

        TestFalse(TEXT("Mismatched channel lengths are rejected"),
            PwComputeLoudness(Ragged, Result, Code, Message));
        TestEqual(TEXT("Code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
    }

    return true;
}

// =========================================================================================
// D. ONSETS - four impulses at known times are found, all four, at those times.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwOnsetsFindImpulsesTest,
    "PinWright.audio.features.OnsetsFindFourImpulsesAtKnownTimes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwOnsetsFindImpulsesTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioFeaturesTestHelpers;

    const TArray<double> ExpectedMs = { 100.0, 300.0, 500.0, 700.0 };
    const TArray<double> Amplitudes = { 1.0, 1.0, 1.0, 1.0 };
    const TArray<float> Samples = MakeImpulses(1.0, ExpectedMs, Amplitudes);

    FPwStftSettings StftSettings;
    StftSettings.FftSize = OnsetFftSize;
    StftSettings.HopSize = OnsetHopSize;
    FPwStftResult Stft;
    FPwStftError StftError;
    const bool bStftOk = PwComputeStft(Samples, TestSampleRate, StftSettings, Stft, &StftError);
    TestTrue(FString::Printf(TEXT("STFT succeeded (%s: %s)"), *StftError.Code, *StftError.Message), bStftOk);
    if (!bStftOk)
    {
        return false;
    }

    TArray<FPwOnset> Onsets;
    FString Code;
    FString Message;
    const bool bOk = PwDetectOnsets(Stft, TestSampleRate, Onsets, Code, Message);
    TestTrue(FString::Printf(TEXT("PwDetectOnsets succeeded (%s: %s)"), *Code, *Message), bOk);

    TestEqual(TEXT("Exactly four onsets - no misses and no doubles"), Onsets.Num(), ExpectedMs.Num());
    if (Onsets.Num() != ExpectedMs.Num())
    {
        for (const FPwOnset& Onset : Onsets)
        {
            AddInfo(FString::Printf(TEXT("  onset at %.2f ms, strength %.3f"), Onset.TimeMs, Onset.Strength));
        }
        return false;
    }

    for (int32 Index = 0; Index < ExpectedMs.Num(); ++Index)
    {
        const double Delta = Onsets[Index].TimeMs - ExpectedMs[Index];
        TestTrue(FString::Printf(TEXT("Onset %d at %.3f ms, expected %.1f ms (delta %.3f ms, tolerance %.3f)"),
            Index, Onsets[Index].TimeMs, ExpectedMs[Index], Delta, OnsetToleranceMs),
            FMath::Abs(Delta) <= OnsetToleranceMs);
        TestTrue(FString::Printf(TEXT("Onset %d strength %.3f is normalized into (0, 1]"),
            Index, Onsets[Index].Strength),
            Onsets[Index].Strength > 0.0 && Onsets[Index].Strength <= 1.0 + UE_DOUBLE_KINDA_SMALL_NUMBER);
    }

    // Onsets come out in time order, which the minimum-interval filter depends on.
    for (int32 Index = 1; Index < Onsets.Num(); ++Index)
    {
        TestTrue(TEXT("Onsets are in ascending time order"), Onsets[Index].TimeMs > Onsets[Index - 1].TimeMs);
    }

    return true;
}

// =========================================================================================
// E. ONSETS - the false-positive direction (rpc-design.md §6). A tone that changes level
//    continuously but contains no transient must produce ZERO onsets. This is the case the
//    max-normalized flux curve gets wrong on its own: with nothing but ripple in the buffer, the
//    largest ripple frame still normalizes to 1.0 and clears a median-plus-margin test. The
//    absolute gate is what makes this test pass, so this test is what guards the gate.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwOnsetsSustainedToneHasNoneTest,
    "PinWright.audio.features.OnsetsSustainedToneReportsNone",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwOnsetsSustainedToneHasNoneTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioFeaturesTestHelpers;

    // 1.5 s of 1 kHz with 200 ms raised-cosine fades at each end. The fades give the detector a
    // continuously rising spectrum to be wrong about: the steepest hop gains ~1.3% of the clip's
    // mean frame magnitude, against a 5% gate.
    const TArray<float> Samples = MakeFadedTone(1.5, 1000.0, 0.5, 0.2);

    FPwStftSettings StftSettings;
    StftSettings.FftSize = OnsetFftSize;
    StftSettings.HopSize = OnsetHopSize;
    FPwStftResult Stft;
    FPwStftError StftError;
    const bool bStftOk = PwComputeStft(Samples, TestSampleRate, StftSettings, Stft, &StftError);
    TestTrue(FString::Printf(TEXT("STFT succeeded (%s: %s)"), *StftError.Code, *StftError.Message), bStftOk);
    if (!bStftOk)
    {
        return false;
    }

    TArray<FPwOnset> Onsets;
    FString Code;
    FString Message;
    const bool bOk = PwDetectOnsets(Stft, TestSampleRate, Onsets, Code, Message);

    // Success with an empty array: the detection ran and found nothing. Not a failure.
    TestTrue(FString::Printf(TEXT("PwDetectOnsets succeeded (%s: %s)"), *Code, *Message), bOk);
    TestEqual(TEXT("A smoothly faded tone contains no onsets"), Onsets.Num(), 0);
    if (Onsets.Num() != 0)
    {
        for (const FPwOnset& Onset : Onsets)
        {
            AddInfo(FString::Printf(TEXT("  false onset at %.2f ms, strength %.3f"),
                Onset.TimeMs, Onset.Strength));
        }
    }

    return true;
}

// =========================================================================================
// F. ONSETS - one transient must not be reported more than once. Four impulses with the last
//    two 20 ms apart (inside the 30 ms minimum interval) must yield three onsets, never five.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwOnsetsMergeCloseTransientsTest,
    "PinWright.audio.features.OnsetsMergeTransientsInsideMinimumInterval",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwOnsetsMergeCloseTransientsTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioFeaturesTestHelpers;

    const TArray<double> TimesMs = { 100.0, 300.0, 500.0, 520.0 };
    const TArray<double> Amplitudes = { 1.0, 1.0, 1.0, 0.9 };
    const TArray<float> Samples = MakeImpulses(1.0, TimesMs, Amplitudes);

    FPwStftSettings StftSettings;
    StftSettings.FftSize = OnsetFftSize;
    StftSettings.HopSize = OnsetHopSize;
    FPwStftResult Stft;
    FPwStftError StftError;
    const bool bStftOk = PwComputeStft(Samples, TestSampleRate, StftSettings, Stft, &StftError);
    TestTrue(FString::Printf(TEXT("STFT succeeded (%s: %s)"), *StftError.Code, *StftError.Message), bStftOk);
    if (!bStftOk)
    {
        return false;
    }

    TArray<FPwOnset> Onsets;
    FString Code;
    FString Message;
    TestTrue(TEXT("PwDetectOnsets succeeded"), PwDetectOnsets(Stft, TestSampleRate, Onsets, Code, Message));

    // Never more than the four transients present - the failure this guards is one attack
    // reported two or three times as it smears across hops.
    TestTrue(FString::Printf(TEXT("At most four onsets for four impulses; got %d"), Onsets.Num()),
        Onsets.Num() <= 4);
    // And the 20 ms pair collapses, because 20 ms < the 30 ms minimum inter-onset interval.
    TestEqual(TEXT("The 20 ms pair reports as one onset, so three in total"), Onsets.Num(), 3);

    for (int32 Index = 1; Index < Onsets.Num(); ++Index)
    {
        TestTrue(FString::Printf(TEXT("Onsets %d and %d are at least 30 ms apart (%.2f ms)"),
            Index - 1, Index, Onsets[Index].TimeMs - Onsets[Index - 1].TimeMs),
            (Onsets[Index].TimeMs - Onsets[Index - 1].TimeMs) >= 30.0);
    }

    return true;
}

// =========================================================================================
// G. ONSETS - failure direction. Empty, silent, and mismatched-rate inputs.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwOnsetsRejectEmptyAndSilentTest,
    "PinWright.audio.features.OnsetsRejectEmptySilentAndMismatchedInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwOnsetsRejectEmptyAndSilentTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioFeaturesTestHelpers;

    {
        const FPwStftResult Empty;
        TArray<FPwOnset> Onsets;
        FString Code;
        FString Message;

        TestFalse(TEXT("An empty spectrogram is rejected"),
            PwDetectOnsets(Empty, TestSampleRate, Onsets, Code, Message));
        TestEqual(TEXT("Code is AUDIO_EMPTY_BUFFER"), Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        TestNotEqual(TEXT("Emptiness is specifically NOT reported as invalid params"),
            Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestEqual(TEXT("No onsets are invented"), Onsets.Num(), 0);
    }

    {
        // A well-formed spectrogram of nothing. Every structural check passes, so this is the one
        // that proves silence is named ahead of them rather than falling through to zero onsets -
        // "there was no signal" and "there were no onsets" are different answers.
        FPwStftResult Silent;
        Silent.NumFrames = 64;
        Silent.NumBins = OnsetFftSize / 2 + 1;
        Silent.FftSize = OnsetFftSize;
        Silent.HopSize = OnsetHopSize;
        Silent.BinHz = static_cast<float>(TestSampleRate) / static_cast<float>(OnsetFftSize);
        Silent.Magnitudes.SetNumZeroed(Silent.NumFrames * Silent.NumBins);
        TestTrue(TEXT("The silent spectrogram is otherwise well formed"), Silent.IsValid());

        TArray<FPwOnset> Onsets;
        FString Code;
        FString Message;
        TestFalse(TEXT("A silent spectrogram is rejected"),
            PwDetectOnsets(Silent, TestSampleRate, Onsets, Code, Message));
        TestEqual(TEXT("Silence is named AUDIO_EMPTY_BUFFER, not answered with zero onsets"),
            Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
    }

    {
        // Wrong sample rate for this spectrogram. Detected structurally rather than producing
        // onsets on the wrong clock, which has no symptom the caller could see.
        const TArray<double> TimesMs = { 100.0, 300.0 };
        const TArray<double> Amplitudes = { 1.0, 1.0 };
        const TArray<float> Samples = MakeImpulses(0.5, TimesMs, Amplitudes);
        FPwStftSettings StftSettings;
        StftSettings.FftSize = OnsetFftSize;
        StftSettings.HopSize = OnsetHopSize;
        FPwStftResult Stft;
        FPwStftError StftError;
        if (!PwComputeStft(Samples, TestSampleRate, StftSettings, Stft, &StftError))
        {
            AddError(FString::Printf(TEXT("STFT setup failed (%s: %s)"), *StftError.Code, *StftError.Message));
            return false;
        }

        TArray<FPwOnset> Onsets;
        FString Code;
        FString Message;
        TestFalse(TEXT("A sample rate that disagrees with the spectrogram is rejected"),
            PwDetectOnsets(Stft, 44100, Onsets, Code, Message));
        TestEqual(TEXT("Code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("The message names both rates"),
            Message.Contains(TEXT("44100")) && Message.Contains(TEXT("48000")));

        // And a non-positive rate is refused too.
        TestFalse(TEXT("A zero sample rate is rejected"),
            PwDetectOnsets(Stft, 0, Onsets, Code, Message));
        TestEqual(TEXT("Code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
    }

    return true;
}

// =========================================================================================
// H. PITCH - a 440 Hz sine reads 440, confidently, and classifies as stable.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwPitchSine440Test,
    "PinWright.audio.features.PitchSineReads440WithHighConfidence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwPitchSine440Test::RunTest(const FString& Parameters)
{
    using namespace PwAudioFeaturesTestHelpers;

    const TArray<float> Samples = MakeSine(0.5, 440.0, 0.5);

    FPwPitchResult Result;
    FString Code;
    FString Message;
    const bool bOk = PwEstimatePitch(Samples, TestSampleRate, Result, Code, Message);
    TestTrue(FString::Printf(TEXT("PwEstimatePitch succeeded (%s: %s)"), *Code, *Message), bOk);
    if (!bOk)
    {
        return false;
    }

    TestTrue(TEXT("bMeasured is set"), Result.bMeasured);
    TestTrue(FString::Printf(TEXT("Track has %d points"), Result.Track.Num()), Result.Track.Num() >= 10);

    // 440 Hz at 48 kHz is lag 109.09, so the answer depends on the parabolic interpolation between
    // integer lags: without it the nearest lags give 440.4 Hz (109) or 436.4 Hz (110). 1% is a
    // loose band around a numerically simulated error of 0.004%, chosen to survive float32
    // rounding in the correlator rather than to be tight.
    TestTrue(FString::Printf(TEXT("MedianF0Hz is %.3f, expected 440 +/- 1%%"), Result.MedianF0Hz),
        FMath::Abs(Result.MedianF0Hz - 440.0) <= 4.4);
    TestTrue(FString::Printf(TEXT("Confidence is %.4f, expected > 0.9"), Result.Confidence),
        Result.Confidence > 0.9);
    TestTrue(TEXT("Confidence clears the exported floor"), Result.Confidence >= PwPitchConfidenceFloor);
    TestEqual(TEXT("A steady tone classifies as stable"), Result.Motion, FString(TEXT("stable")));

    for (const FPwPitchPointResult& Point : Result.Track)
    {
        TestTrue(FString::Printf(TEXT("Track point at %.1f ms has f0 %.2f inside [50, 2000]"),
            Point.TimeMs, Point.F0Hz), Point.F0Hz >= 50.0 && Point.F0Hz <= 2000.0);
        TestTrue(TEXT("Track confidences are inside [0, 1]"),
            Point.Confidence >= 0.0 && Point.Confidence <= 1.0);
    }

    return true;
}

// =========================================================================================
// I. PITCH - 220 Hz, tested separately because the classic YIN failure is an octave error and a
//    single-frequency test cannot see one. 110 and 440 are asserted against explicitly.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwPitchSine220NoOctaveErrorTest,
    "PinWright.audio.features.PitchSineReads220WithoutOctaveError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwPitchSine220NoOctaveErrorTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioFeaturesTestHelpers;

    const TArray<float> Samples = MakeSine(0.5, 220.0, 0.5);

    FPwPitchResult Result;
    FString Code;
    FString Message;
    const bool bOk = PwEstimatePitch(Samples, TestSampleRate, Result, Code, Message);
    TestTrue(FString::Printf(TEXT("PwEstimatePitch succeeded (%s: %s)"), *Code, *Message), bOk);
    if (!bOk)
    {
        return false;
    }

    TestTrue(FString::Printf(TEXT("MedianF0Hz is %.3f, expected 220 +/- 1%%"), Result.MedianF0Hz),
        FMath::Abs(Result.MedianF0Hz - 220.0) <= 2.2);
    TestTrue(FString::Printf(TEXT("Not the octave below (110 Hz); read %.3f"), Result.MedianF0Hz),
        FMath::Abs(Result.MedianF0Hz - 110.0) > 20.0);
    TestTrue(FString::Printf(TEXT("Not the octave above (440 Hz); read %.3f"), Result.MedianF0Hz),
        FMath::Abs(Result.MedianF0Hz - 440.0) > 20.0);
    TestTrue(FString::Printf(TEXT("Confidence is %.4f, expected > 0.9"), Result.Confidence),
        Result.Confidence > 0.9);

    return true;
}

// =========================================================================================
// J. PITCH - noise must not be reported as pitched. The estimator still runs and still fills the
//    track (so a caller can see what was rejected), but the aggregate refuses to name a
//    frequency: reporting a plausible number for noise is worse than reporting none.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwPitchNoiseIsUnpitchedTest,
    "PinWright.audio.features.PitchWhiteNoiseIsNotReportedAsPitched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwPitchNoiseIsUnpitchedTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioFeaturesTestHelpers;

    const TArray<float> Samples = MakeWhiteNoise(0.5, 20260817);

    FPwPitchResult Result;
    FString Code;
    FString Message;
    const bool bOk = PwEstimatePitch(Samples, TestSampleRate, Result, Code, Message);

    // Noise is a successful measurement of something unpitched, not an error.
    TestTrue(FString::Printf(TEXT("PwEstimatePitch succeeded (%s: %s)"), *Code, *Message), bOk);
    TestTrue(TEXT("bMeasured is set - the estimator ran"), Result.bMeasured);
    TestTrue(FString::Printf(TEXT("The track was populated (%d points)"), Result.Track.Num()),
        Result.Track.Num() > 0);

    // Simulated over three seeds of the same FRandomStream generator, the median per-window
    // confidence lands at 0.085 and the single best window at 0.13 - a factor of four below the
    // 0.5 floor, which is what makes this a margin rather than a coin flip.
    TestTrue(FString::Printf(TEXT("Confidence is %.4f, below the %.2f floor"),
        Result.Confidence, PwPitchConfidenceFloor), Result.Confidence < PwPitchConfidenceFloor);
    TestEqual(TEXT("No frequency is named for noise"), Result.MedianF0Hz, 0.0);
    TestEqual(TEXT("Motion is unknown for noise"), Result.Motion, FString(TEXT("unknown")));

    return true;
}

// =========================================================================================
// K. PITCH - a rising chirp classifies rising and a falling chirp falling. Two tests in one
//    body because the pair IS the assertion (rpc-design.md §6): either one alone passes for an
//    implementation that measures the magnitude of the frequency change and not its sign.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwPitchChirpDirectionTest,
    "PinWright.audio.features.PitchChirpsClassifyRisingAndFalling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwPitchChirpDirectionTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioFeaturesTestHelpers;

    // 300 -> 600 Hz over 1.5 s: exactly one octave, twelve times the one-semitone stability
    // threshold, and slow enough (200 Hz/s) that the frequency barely moves inside one 42.7 ms
    // analysis block, so the confidence stays high.
    FPwPitchResult Rising;
    FPwPitchResult Falling;
    FString Code;
    FString Message;

    const bool bRisingOk = PwEstimatePitch(MakeChirp(1.5, 300.0, 600.0, 0.5), TestSampleRate,
        Rising, Code, Message);
    TestTrue(FString::Printf(TEXT("Rising chirp measured (%s: %s)"), *Code, *Message), bRisingOk);
    const bool bFallingOk = PwEstimatePitch(MakeChirp(1.5, 600.0, 300.0, 0.5), TestSampleRate,
        Falling, Code, Message);
    TestTrue(FString::Printf(TEXT("Falling chirp measured (%s: %s)"), *Code, *Message), bFallingOk);
    if (!bRisingOk || !bFallingOk)
    {
        return false;
    }

    TestEqual(TEXT("300 -> 600 Hz classifies as rising"), Rising.Motion, FString(TEXT("rising")));
    TestEqual(TEXT("600 -> 300 Hz classifies as falling"), Falling.Motion, FString(TEXT("falling")));
    TestNotEqual(TEXT("A chirp and its reverse are distinguishable"), Rising.Motion, Falling.Motion);

    TestTrue(FString::Printf(TEXT("Rising chirp confidence %.4f > 0.8"), Rising.Confidence),
        Rising.Confidence > 0.8);
    TestTrue(FString::Printf(TEXT("Falling chirp confidence %.4f > 0.8"), Falling.Confidence),
        Falling.Confidence > 0.8);

    // The two sweeps cover the same frequencies, so their medians must agree; a direction bug
    // that reversed the track would still satisfy that, which is why Motion is asserted above.
    TestTrue(FString::Printf(TEXT("Medians agree (%.2f vs %.2f Hz), both inside [300, 600]"),
        Rising.MedianF0Hz, Falling.MedianF0Hz),
        Rising.MedianF0Hz > 300.0 && Rising.MedianF0Hz < 600.0
        && Falling.MedianF0Hz > 300.0 && Falling.MedianF0Hz < 600.0);

    // And the tracks really do move in opposite directions, measured rather than inferred.
    if (Rising.Track.Num() > 4 && Falling.Track.Num() > 4)
    {
        TestTrue(TEXT("The rising track ends above where it started"),
            Rising.Track.Last().F0Hz > Rising.Track[0].F0Hz);
        TestTrue(TEXT("The falling track ends below where it started"),
            Falling.Track.Last().F0Hz < Falling.Track[0].F0Hz);
    }

    return true;
}

// =========================================================================================
// L. PITCH - failure direction. Silence must not read as a pitch: an all-zero signal correlates
//    perfectly with itself at every lag, so a YIN implementation that does not check for it
//    first returns the shortest searched lag at full confidence - a fabricated 2 kHz tone.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwPitchRejectsEmptyAndSilentTest,
    "PinWright.audio.features.PitchRejectsEmptyAndSilentSignals",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwPitchRejectsEmptyAndSilentTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioFeaturesTestHelpers;

    {
        FPwPitchResult Result;
        FString Code;
        FString Message;

        TestFalse(TEXT("An empty signal is rejected"),
            PwEstimatePitch(TArrayView<const float>(), TestSampleRate, Result, Code, Message));
        TestEqual(TEXT("Code is AUDIO_EMPTY_BUFFER"), Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        TestFalse(TEXT("bMeasured stays false"), Result.bMeasured);
        TestEqual(TEXT("MedianF0Hz stays 0"), Result.MedianF0Hz, 0.0);
        TestEqual(TEXT("No track is invented"), Result.Track.Num(), 0);
    }

    {
        TArray<float> Silence;
        Silence.SetNumZeroed(TestSampleRate / 2);   // long enough for many analysis blocks
        FPwPitchResult Result;
        FString Code;
        FString Message;

        TestFalse(TEXT("A silent signal is rejected"),
            PwEstimatePitch(Silence, TestSampleRate, Result, Code, Message));
        TestEqual(TEXT("Silence is named AUDIO_EMPTY_BUFFER"),
            Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        TestFalse(TEXT("bMeasured stays false"), Result.bMeasured);
        TestEqual(TEXT("Silence produces no fabricated f0"), Result.MedianF0Hz, 0.0);
        TestEqual(TEXT("Silence produces no confidence"), Result.Confidence, 0.0);
    }

    {
        // Shorter than one analysis block: named as a parameter problem with the requirement in
        // the message, rather than answered with a pitch derived from a partial window.
        const TArray<float> TooShort = MakeSine(0.01, 440.0, 0.5);   // 480 samples, block is 2048
        FPwPitchResult Result;
        FString Code;
        FString Message;

        TestFalse(TEXT("A signal shorter than one block is rejected"),
            PwEstimatePitch(TooShort, TestSampleRate, Result, Code, Message));
        TestEqual(TEXT("Code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("The message names the block size"), Message.Contains(TEXT("2048")));
        TestFalse(TEXT("bMeasured stays false"), Result.bMeasured);
    }

    {
        const TArray<float> Samples = MakeSine(0.5, 440.0, 0.5);
        FPwPitchResult Result;
        FString Code;
        FString Message;

        TestFalse(TEXT("A zero sample rate is rejected"),
            PwEstimatePitch(Samples, 0, Result, Code, Message));
        TestEqual(TEXT("Code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
    }

    return true;
}
