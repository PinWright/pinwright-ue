// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the formant generator (AudioGen/PwSynthDsp.h -> PwGenFormant).
//
// Ground truth is the PUBLISHED vowel table, not the generator's own copy of it:
// the expected F1/F2 values below are typed out from the CHANT/IRCAM bass-voice
// data (Csound `fof` vowel tables) independently of the implementation, so a test
// still fails if the shipped table is edited to something wrong. Measurement is
// the STFT front-end in AudioGen/PwStft.h.
//
// HOW A FORMANT IS MEASURED HERE, AND WHERE THE TOLERANCE COMES FROM
// A voiced source only puts energy at multiples of f0, so the spectrum samples the
// formant resonance on an f0-spaced grid and the loudest bin can only ever be a
// harmonic. Reading the winning harmonic's frequency straight off would therefore
// carry up to f0/2 of pure quantization error, plus a downward bias from the
// source's -6 dB/octave tilt. Instead the three harmonics around the winner are
// fitted with a parabola in the log-magnitude domain - the standard peak
// interpolator - which recovers the underlying resonance centre to within a few
// tens of Hz. The tolerance is then max(0.6 * f0, 15% of the published centre): the
// first term is the grid spacing the estimate is built from, the second covers the
// bias the finite bandwidth and the source tilt leave behind at high formants, which
// grows with the formant's own frequency. Analysis of the cases used below puts the
// errors at 7-77 Hz against tolerances of 55-243 Hz, i.e. no assertion runs closer
// than about 1.6x to its limit, while a formant that failed to move by its
// formantShift misses by 127-364 Hz and is caught.
//
// Per rpc-design.md §12 every rejection path is asserted for its SPECIFIC code AND
// for leaving a pre-filled output buffer byte-for-byte untouched, because "returned
// false but wrote half a render" is the failure this contract exists to prevent.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwSeededRandom.h"
#include "AudioGen/PwStft.h"
#include "AudioGen/PwSynthDsp.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Handlers/ErrorCodes.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true.
namespace PwGenFormantTestHelpers
{
    constexpr int32 TestSampleRate = 48000;
    constexpr int32 TestFftSize = 4096;         // 11.72 Hz bins at 48 kHz
    constexpr int32 TestHopSize = 1024;
    constexpr int32 WarmupFrames = 2;           // skip the bank's ring-up
    constexpr float SentinelValue = 1234.5f;

    /**
     * Published bass-voice formant centres, retyped from the source cited in
     * PwGenFormant.cpp rather than read back out of it.
     */
    struct FPublishedVowel
    {
        const TCHAR* Name;
        double F1Hz;
        double F2Hz;
    };

    constexpr FPublishedVowel VowelA{ TEXT("a"), 600.0, 1040.0 };
    constexpr FPublishedVowel VowelE{ TEXT("e"), 400.0, 1620.0 };

    void SetNumberParam(FPwSynthParams& Params, const TCHAR* Key, double Value)
    {
        FPwSynthParamValue Entry;
        Entry.Type = EPwSynthParamType::Number;
        Entry.Number = Value;
        Params.Values.Add(FName(Key), Entry);
    }

    void SetEnumParam(FPwSynthParams& Params, const TCHAR* Key, const TCHAR* Value)
    {
        FPwSynthParamValue Entry;
        Entry.Type = EPwSynthParamType::Enum;
        Entry.String = Value;
        Params.Values.Add(FName(Key), Entry);
    }

    FPwSynthParams MakeParams(double F0Hz, const TCHAR* Vowel, double FormantShift = 1.0,
        double Voicing = 1.0, double Breathiness = 0.0)
    {
        FPwSynthParams Params;
        SetNumberParam(Params, TEXT("f0Hz"), F0Hz);
        SetEnumParam(Params, TEXT("vowel"), Vowel);
        SetNumberParam(Params, TEXT("formantShift"), FormantShift);
        SetNumberParam(Params, TEXT("voicing"), Voicing);
        SetNumberParam(Params, TEXT("breathiness"), Breathiness);
        return Params;
    }

    /** Renders one second of mono audio. Returns an empty array when the generator rejects the params. */
    TArray<float> Render(const FPwSynthParams& Params, int32 Seed, FString& OutErrorCode, FString& OutError,
        int32 NumSamples = TestSampleRate)
    {
        TArray<float> Samples;
        Samples.SetNumZeroed(NumSamples);

        FPwSeededRandom Rng(Seed);
        const FPwSynthModulation NoModulation;
        const TArray<FPwSynthPitchPoint> NoPitchEnvelope;

        if (!PwGenFormant(Params, NoPitchEnvelope, NoModulation, TestSampleRate, Rng,
                TArrayView<float>(Samples), OutErrorCode, OutError))
        {
            Samples.Reset();
        }
        return Samples;
    }

    /**
     * Magnitude spectrum averaged over every analysis frame past the warm-up. The
     * source carries per-cycle jitter, so a single frame is a noisy estimate of a
     * stationary spectrum; averaging is what makes the harmonic magnitudes stable
     * enough to interpolate.
     */
    bool AverageSpectrum(const TArray<float>& Samples, TArray<float>& OutMagnitudes, float& OutBinHz)
    {
        FPwStftSettings Settings;
        Settings.FftSize = TestFftSize;
        Settings.HopSize = TestHopSize;

        FPwStftResult Result;
        if (!PwComputeStft(Samples, TestSampleRate, Settings, Result) || Result.NumFrames <= WarmupFrames)
        {
            return false;
        }

        OutBinHz = Result.BinHz;
        OutMagnitudes.Init(0.f, Result.NumBins);

        const int32 UsedFrames = Result.NumFrames - WarmupFrames;
        for (int32 Frame = WarmupFrames; Frame < Result.NumFrames; ++Frame)
        {
            for (int32 Bin = 0; Bin < Result.NumBins; ++Bin)
            {
                OutMagnitudes[Bin] += PwStftMagnitudeAt(Result, Frame, Bin);
            }
        }
        for (float& Magnitude : OutMagnitudes)
        {
            Magnitude /= static_cast<float>(UsedFrames);
        }
        return true;
    }

    /**
     * Largest magnitude within +/-2 bins of a frequency. A Hann main lobe is 4 bins
     * wide, so this is exactly one lobe; the closest harmonic spacing used by these
     * tests (110 Hz, 9.4 bins) keeps neighbouring lobes out of the window.
     */
    float MagnitudeAtHz(const TArray<float>& Magnitudes, float BinHz, double Hz)
    {
        const int32 Center = FMath::RoundToInt32(Hz / BinHz);
        float Best = 0.f;
        for (int32 Bin = Center - 2; Bin <= Center + 2; ++Bin)
        {
            if (Magnitudes.IsValidIndex(Bin))
            {
                Best = FMath::Max(Best, Magnitudes[Bin]);
            }
        }
        return Best;
    }

    /** The magnitude of each harmonic of F0 up to MaxHz. Index 0 is the fundamental. */
    TArray<float> HarmonicMagnitudes(const TArray<float>& Magnitudes, float BinHz, double F0Hz, double MaxHz)
    {
        TArray<float> Harmonics;
        for (int32 Harmonic = 1; Harmonic * F0Hz <= MaxHz; ++Harmonic)
        {
            Harmonics.Add(MagnitudeAtHz(Magnitudes, BinHz, Harmonic * F0Hz));
        }
        return Harmonics;
    }

    /**
     * Formant centre estimate: the loudest harmonic inside [LoHz, HiHz], refined by a
     * log-domain parabolic fit against its two neighbours (which are used even when
     * they sit outside the search band - the band selects the peak, not the fit).
     * Returns a negative value when the band holds no harmonic.
     */
    double EstimateFormantHz(const TArray<float>& Harmonics, double F0Hz, double LoHz, double HiHz)
    {
        int32 BestIndex = INDEX_NONE;
        float BestMagnitude = -1.f;
        for (int32 Index = 0; Index < Harmonics.Num(); ++Index)
        {
            const double Hz = (Index + 1) * F0Hz;
            if (Hz < LoHz || Hz > HiHz)
            {
                continue;
            }
            if (Harmonics[Index] > BestMagnitude)
            {
                BestMagnitude = Harmonics[Index];
                BestIndex = Index;
            }
        }
        if (BestIndex == INDEX_NONE)
        {
            return -1.0;
        }

        const double CoarseHz = (BestIndex + 1) * F0Hz;
        if (BestIndex == 0 || BestIndex == Harmonics.Num() - 1)
        {
            return CoarseHz;
        }

        const double Tiny = 1.e-12;
        const double Y1 = FMath::Loge(FMath::Max(static_cast<double>(Harmonics[BestIndex - 1]), Tiny));
        const double Y2 = FMath::Loge(FMath::Max(static_cast<double>(Harmonics[BestIndex]), Tiny));
        const double Y3 = FMath::Loge(FMath::Max(static_cast<double>(Harmonics[BestIndex + 1]), Tiny));

        const double Denominator = Y1 - 2.0 * Y2 + Y3;
        if (Denominator >= -1.e-6)
        {
            // Not a peak shape (the winner sits on a band edge); no refinement to make.
            return CoarseHz;
        }
        const double Delta = FMath::Clamp(0.5 * (Y1 - Y3) / Denominator, -0.5, 0.5);
        return CoarseHz + Delta * F0Hz;
    }

    /**
     * Search bands, derived from the formant positions the render is being tested
     * FOR (published centre * formantShift). F1's band is generous; F2's starts 65%
     * of the way from F1 to F2 so the far stronger F1 peak cannot win it, and stops
     * below F3. A formant that failed to move lands outside its band, the estimate
     * comes back at the wrong place, and the assertion fails - which is the point.
     */
    void FormantBands(double F1Hz, double F2Hz, double& OutF1Lo, double& OutF1Hi, double& OutF2Lo, double& OutF2Hi)
    {
        OutF1Lo = 0.55 * F1Hz;
        OutF1Hi = 1.90 * F1Hz;
        OutF2Lo = F1Hz + 0.65 * (F2Hz - F1Hz);
        OutF2Hi = 1.35 * F2Hz;
    }

    /** max(0.6 * f0, 15% of the expected centre) - see the derivation in the file header. */
    double FormantTolerance(double F0Hz, double ExpectedHz)
    {
        return FMath::Max(0.6 * F0Hz, 0.15 * ExpectedHz);
    }

    /** Measures F1 and F2 of a rendered buffer against the formant positions it should have. */
    bool MeasureFormants(const TArray<float>& Samples, double F0Hz, double ExpectedF1, double ExpectedF2,
        double& OutF1, double& OutF2)
    {
        TArray<float> Magnitudes;
        float BinHz = 0.f;
        if (!AverageSpectrum(Samples, Magnitudes, BinHz))
        {
            return false;
        }

        double F1Lo, F1Hi, F2Lo, F2Hi;
        FormantBands(ExpectedF1, ExpectedF2, F1Lo, F1Hi, F2Lo, F2Hi);

        const TArray<float> Harmonics = HarmonicMagnitudes(Magnitudes, BinHz, F0Hz, F2Hi + 2.0 * F0Hz);
        OutF1 = EstimateFormantHz(Harmonics, F0Hz, F1Lo, F1Hi);
        OutF2 = EstimateFormantHz(Harmonics, F0Hz, F2Lo, F2Hi);
        return OutF1 > 0.0 && OutF2 > 0.0;
    }

    /** Bin index of the loudest bin within +/-20% of F0 - the fundamental, isolated. */
    int32 FundamentalBin(const TArray<float>& Magnitudes, float BinHz, double F0Hz)
    {
        const int32 Lo = FMath::Max(1, FMath::FloorToInt32(0.8 * F0Hz / BinHz));
        const int32 Hi = FMath::CeilToInt32(1.2 * F0Hz / BinHz);
        int32 BestBin = INDEX_NONE;
        float BestMagnitude = -1.f;
        for (int32 Bin = Lo; Bin <= Hi && Magnitudes.IsValidIndex(Bin); ++Bin)
        {
            if (Magnitudes[Bin] > BestMagnitude)
            {
                BestMagnitude = Magnitudes[Bin];
                BestBin = Bin;
            }
        }
        return BestBin;
    }

    /**
     * Mean level at the harmonics of F0 minus the mean level exactly between them,
     * in dB, over the first HarmonicCount harmonics. A pitched source has energy
     * only on the grid, so this is large; a noise source has a continuous spectrum,
     * so both sets sample the same envelope and it collapses toward zero.
     */
    float HarmonicCombDb(const TArray<float>& Magnitudes, float BinHz, double F0Hz, int32 HarmonicCount)
    {
        float OnGridDb = 0.f;
        float OffGridDb = 0.f;
        for (int32 Harmonic = 1; Harmonic <= HarmonicCount; ++Harmonic)
        {
            OnGridDb += PwMagnitudeToDb(MagnitudeAtHz(Magnitudes, BinHz, Harmonic * F0Hz), -140.f);
            OffGridDb += PwMagnitudeToDb(MagnitudeAtHz(Magnitudes, BinHz, (Harmonic + 0.5) * F0Hz), -140.f);
        }
        return (OnGridDb - OffGridDb) / static_cast<float>(HarmonicCount);
    }

    /** Fills a buffer with the sentinel so an untouched-on-failure claim is checkable. */
    TArray<float> MakeSentinelBuffer(int32 NumSamples = 512)
    {
        TArray<float> Buffer;
        Buffer.Init(SentinelValue, NumSamples);
        return Buffer;
    }

    bool IsSentinelIntact(const TArray<float>& Buffer)
    {
        for (const float Sample : Buffer)
        {
            if (Sample != SentinelValue)
            {
                return false;
            }
        }
        return true;
    }
}

// =========================================================================================
// A. /a/ at 220 Hz puts its two lowest spectral maxima at the published F1 and F2.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenFormantVowelAMatchesPublishedTest,
    "PinWright.audio.gen.formant.VowelAMatchesPublishedF1F2",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenFormantVowelAMatchesPublishedTest::RunTest(const FString& Parameters)
{
    using namespace PwGenFormantTestHelpers;

    constexpr double F0Hz = 220.0;

    FString ErrorCode;
    FString Error;
    const TArray<float> Samples = Render(MakeParams(F0Hz, VowelA.Name), 7331, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("PwGenFormant succeeded (%s: %s)"), *ErrorCode, *Error), Samples.Num() > 0);
    if (Samples.Num() == 0)
    {
        return false;
    }

    double MeasuredF1 = 0.0;
    double MeasuredF2 = 0.0;
    TestTrue(TEXT("F1 and F2 were both measurable"),
        MeasureFormants(Samples, F0Hz, VowelA.F1Hz, VowelA.F2Hz, MeasuredF1, MeasuredF2));

    const double F1Tolerance = FormantTolerance(F0Hz, VowelA.F1Hz);
    const double F2Tolerance = FormantTolerance(F0Hz, VowelA.F2Hz);

    TestTrue(FString::Printf(TEXT("/a/ F1 measured %.1f Hz, published %.1f Hz (tolerance %.1f)"),
        MeasuredF1, VowelA.F1Hz, F1Tolerance), FMath::Abs(MeasuredF1 - VowelA.F1Hz) <= F1Tolerance);
    TestTrue(FString::Printf(TEXT("/a/ F2 measured %.1f Hz, published %.1f Hz (tolerance %.1f)"),
        MeasuredF2, VowelA.F2Hz, F2Tolerance), FMath::Abs(MeasuredF2 - VowelA.F2Hz) <= F2Tolerance);

    // The buffer must be filled, not partly filled: an all-zero tail would still pass
    // a spectral test taken from the early frames.
    float Peak = 0.f;
    for (int32 Index = Samples.Num() - 1024; Index < Samples.Num(); ++Index)
    {
        Peak = FMath::Max(Peak, FMath::Abs(Samples[Index]));
    }
    TestTrue(FString::Printf(TEXT("The last 1024 samples carry signal (peak %.4f)"), Peak), Peak > 1.e-4f);

    return true;
}

// =========================================================================================
// B. Two vowels at the SAME f0 land their formants in measurably different places, each at
//    its own published values. This is the assertion that proves the vowel table is wired
//    in rather than ignored - a hardcoded single vowel passes test A and fails this one.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenFormantVowelsDifferTest,
    "PinWright.audio.gen.formant.TwoVowelsProduceDifferentFormants",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenFormantVowelsDifferTest::RunTest(const FString& Parameters)
{
    using namespace PwGenFormantTestHelpers;

    constexpr double F0Hz = 220.0;

    FString ErrorCode;
    FString Error;
    const TArray<float> SamplesA = Render(MakeParams(F0Hz, VowelA.Name), 4242, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("/a/ rendered (%s: %s)"), *ErrorCode, *Error), SamplesA.Num() > 0);
    const TArray<float> SamplesE = Render(MakeParams(F0Hz, VowelE.Name), 4242, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("/e/ rendered (%s: %s)"), *ErrorCode, *Error), SamplesE.Num() > 0);
    if (SamplesA.Num() == 0 || SamplesE.Num() == 0)
    {
        return false;
    }

    double AF1 = 0.0, AF2 = 0.0, EF1 = 0.0, EF2 = 0.0;
    TestTrue(TEXT("/a/ formants measurable"), MeasureFormants(SamplesA, F0Hz, VowelA.F1Hz, VowelA.F2Hz, AF1, AF2));
    TestTrue(TEXT("/e/ formants measurable"), MeasureFormants(SamplesE, F0Hz, VowelE.F1Hz, VowelE.F2Hz, EF1, EF2));

    TestTrue(FString::Printf(TEXT("/e/ F1 measured %.1f Hz, published %.1f Hz"), EF1, VowelE.F1Hz),
        FMath::Abs(EF1 - VowelE.F1Hz) <= FormantTolerance(F0Hz, VowelE.F1Hz));
    TestTrue(FString::Printf(TEXT("/e/ F2 measured %.1f Hz, published %.1f Hz"), EF2, VowelE.F2Hz),
        FMath::Abs(EF2 - VowelE.F2Hz) <= FormantTolerance(F0Hz, VowelE.F2Hz));

    // Separation, asserted in the same direction the table predicts: /a/ is the more
    // open vowel, so its F1 is higher and its F2 lower than /e/'s.
    TestTrue(FString::Printf(TEXT("F1 differs by %.1f Hz (/a/ %.1f above /e/ %.1f)"), AF1 - EF1, AF1, EF1),
        (AF1 - EF1) > 150.0);
    TestTrue(FString::Printf(TEXT("F2 differs by %.1f Hz (/e/ %.1f above /a/ %.1f)"), EF2 - AF2, EF2, AF2),
        (EF2 - AF2) > 400.0);

    return true;
}

// =========================================================================================
// C. formantShift 0.7 drags the formants down without moving the fundamental. The single
//    most important test in this file: it is the one that proves the source and the filter
//    are genuinely independent rather than a pitch shifter in disguise.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenFormantShiftMovesFormantsOnlyTest,
    "PinWright.audio.gen.formant.FormantShiftMovesFormantsNotF0",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenFormantShiftMovesFormantsOnlyTest::RunTest(const FString& Parameters)
{
    using namespace PwGenFormantTestHelpers;

    // 110 Hz, not 220: the harmonic grid is what samples the resonance, and /e/'s
    // shifted formants need a finer grid than a 220 Hz voice provides.
    constexpr double F0Hz = 110.0;
    constexpr double Shift = 0.7;

    FString ErrorCode;
    FString Error;
    const TArray<float> Unshifted = Render(MakeParams(F0Hz, VowelE.Name, 1.0), 909, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("Unshifted render succeeded (%s: %s)"), *ErrorCode, *Error), Unshifted.Num() > 0);
    const TArray<float> Shifted = Render(MakeParams(F0Hz, VowelE.Name, Shift), 909, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("Shifted render succeeded (%s: %s)"), *ErrorCode, *Error), Shifted.Num() > 0);
    if (Unshifted.Num() == 0 || Shifted.Num() == 0)
    {
        return false;
    }

    double PlainF1 = 0.0, PlainF2 = 0.0, ShiftedF1 = 0.0, ShiftedF2 = 0.0;
    TestTrue(TEXT("Unshifted formants measurable"),
        MeasureFormants(Unshifted, F0Hz, VowelE.F1Hz, VowelE.F2Hz, PlainF1, PlainF2));
    TestTrue(TEXT("Shifted formants measurable"),
        MeasureFormants(Shifted, F0Hz, VowelE.F1Hz * Shift, VowelE.F2Hz * Shift, ShiftedF1, ShiftedF2));

    const double TargetF1 = VowelE.F1Hz * Shift;
    const double TargetF2 = VowelE.F2Hz * Shift;

    TestTrue(FString::Printf(TEXT("Shifted F1 measured %.1f Hz, expected %.1f Hz (0.7 * %.1f)"),
        ShiftedF1, TargetF1, VowelE.F1Hz),
        FMath::Abs(ShiftedF1 - TargetF1) <= FormantTolerance(F0Hz, TargetF1));
    TestTrue(FString::Printf(TEXT("Shifted F2 measured %.1f Hz, expected %.1f Hz (0.7 * %.1f)"),
        ShiftedF2, TargetF2, VowelE.F2Hz),
        FMath::Abs(ShiftedF2 - TargetF2) <= FormantTolerance(F0Hz, TargetF2));

    // Direction, not just position: both formants moved DOWN by clearly more than the
    // grid could account for. A no-op shift scores 0 here.
    TestTrue(FString::Printf(TEXT("F1 moved down: %.1f -> %.1f Hz"), PlainF1, ShiftedF1),
        (PlainF1 - ShiftedF1) > 0.5 * F0Hz);
    TestTrue(FString::Printf(TEXT("F2 moved down: %.1f -> %.1f Hz"), PlainF2, ShiftedF2),
        (PlainF2 - ShiftedF2) > 0.5 * F0Hz);

    // ... and the fundamental did not move at all. Same bin, both renders.
    TArray<float> PlainMagnitudes;
    TArray<float> ShiftedMagnitudes;
    float PlainBinHz = 0.f;
    float ShiftedBinHz = 0.f;
    TestTrue(TEXT("Unshifted spectrum computed"), AverageSpectrum(Unshifted, PlainMagnitudes, PlainBinHz));
    TestTrue(TEXT("Shifted spectrum computed"), AverageSpectrum(Shifted, ShiftedMagnitudes, ShiftedBinHz));

    const int32 PlainF0Bin = FundamentalBin(PlainMagnitudes, PlainBinHz, F0Hz);
    const int32 ShiftedF0Bin = FundamentalBin(ShiftedMagnitudes, ShiftedBinHz, F0Hz);

    // 110 Hz falls at bin 9.4 of an 11.72 Hz grid, so "the right bin" is 9 or 10 - the
    // claim under test is that the shift did not move it, measured against formants
    // that moved by 10 to 47 bins.
    const int32 ExpectedF0Bin = FMath::RoundToInt32(F0Hz / PlainBinHz);
    TestTrue(FString::Printf(TEXT("Unshifted fundamental is at bin %d (expected %d +/- 1)"),
        PlainF0Bin, ExpectedF0Bin), FMath::Abs(PlainF0Bin - ExpectedF0Bin) <= 1);
    TestEqual(TEXT("formantShift left the fundamental on the same bin"), ShiftedF0Bin, PlainF0Bin);

    return true;
}

// =========================================================================================
// D. The mirror image of C: raising f0 moves the fundamental and leaves the formants where
//    the vowel table put them.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenFormantF0MovesFundamentalOnlyTest,
    "PinWright.audio.gen.formant.RaisingF0LeavesFormantsInPlace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenFormantF0MovesFundamentalOnlyTest::RunTest(const FString& Parameters)
{
    using namespace PwGenFormantTestHelpers;

    constexpr double LowF0 = 110.0;
    constexpr double HighF0 = 165.0;    // a perfect fifth up

    FString ErrorCode;
    FString Error;
    const TArray<float> Low = Render(MakeParams(LowF0, VowelA.Name), 515, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("Low render succeeded (%s: %s)"), *ErrorCode, *Error), Low.Num() > 0);
    const TArray<float> High = Render(MakeParams(HighF0, VowelA.Name), 515, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("High render succeeded (%s: %s)"), *ErrorCode, *Error), High.Num() > 0);
    if (Low.Num() == 0 || High.Num() == 0)
    {
        return false;
    }

    double LowF1 = 0.0, LowF2 = 0.0, HighF1 = 0.0, HighF2 = 0.0;
    TestTrue(TEXT("Low-pitch formants measurable"),
        MeasureFormants(Low, LowF0, VowelA.F1Hz, VowelA.F2Hz, LowF1, LowF2));
    TestTrue(TEXT("High-pitch formants measurable"),
        MeasureFormants(High, HighF0, VowelA.F1Hz, VowelA.F2Hz, HighF1, HighF2));

    // Both renders must agree with the SAME published centres. The tolerance follows
    // f0 because the harmonic grid the estimate is sampled on does.
    TestTrue(FString::Printf(TEXT("F1 at f0=%.0f measured %.1f Hz, published %.1f Hz"), LowF0, LowF1, VowelA.F1Hz),
        FMath::Abs(LowF1 - VowelA.F1Hz) <= FormantTolerance(LowF0, VowelA.F1Hz));
    TestTrue(FString::Printf(TEXT("F1 at f0=%.0f measured %.1f Hz, published %.1f Hz"), HighF0, HighF1, VowelA.F1Hz),
        FMath::Abs(HighF1 - VowelA.F1Hz) <= FormantTolerance(HighF0, VowelA.F1Hz));
    TestTrue(FString::Printf(TEXT("F2 at f0=%.0f measured %.1f Hz, published %.1f Hz"), LowF0, LowF2, VowelA.F2Hz),
        FMath::Abs(LowF2 - VowelA.F2Hz) <= FormantTolerance(LowF0, VowelA.F2Hz));
    TestTrue(FString::Printf(TEXT("F2 at f0=%.0f measured %.1f Hz, published %.1f Hz"), HighF0, HighF2, VowelA.F2Hz),
        FMath::Abs(HighF2 - VowelA.F2Hz) <= FormantTolerance(HighF0, VowelA.F2Hz));

    // The fundamental, meanwhile, moved by the ratio it was asked to.
    TArray<float> LowMagnitudes;
    TArray<float> HighMagnitudes;
    float LowBinHz = 0.f;
    float HighBinHz = 0.f;
    TestTrue(TEXT("Low spectrum computed"), AverageSpectrum(Low, LowMagnitudes, LowBinHz));
    TestTrue(TEXT("High spectrum computed"), AverageSpectrum(High, HighMagnitudes, HighBinHz));

    const double LowFundamentalHz = FundamentalBin(LowMagnitudes, LowBinHz, LowF0) * LowBinHz;
    const double HighFundamentalHz = FundamentalBin(HighMagnitudes, HighBinHz, HighF0) * HighBinHz;

    TestTrue(FString::Printf(TEXT("Fundamental at %.1f Hz for f0=%.0f"), LowFundamentalHz, LowF0),
        FMath::Abs(LowFundamentalHz - LowF0) <= LowBinHz);
    TestTrue(FString::Printf(TEXT("Fundamental at %.1f Hz for f0=%.0f"), HighFundamentalHz, HighF0),
        FMath::Abs(HighFundamentalHz - HighF0) <= HighBinHz);

    return true;
}

// =========================================================================================
// E. voicing/breathiness select between a harmonic comb and a whisper.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenFormantVoicingControlsCombTest,
    "PinWright.audio.gen.formant.VoicingControlsHarmonicComb",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenFormantVoicingControlsCombTest::RunTest(const FString& Parameters)
{
    using namespace PwGenFormantTestHelpers;

    constexpr double F0Hz = 220.0;
    constexpr int32 CombHarmonics = 8;

    FString ErrorCode;
    FString Error;
    const TArray<float> Voiced = Render(MakeParams(F0Hz, VowelA.Name, 1.0, /*Voicing=*/1.0, /*Breathiness=*/0.0),
        60606, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("Voiced render succeeded (%s: %s)"), *ErrorCode, *Error), Voiced.Num() > 0);
    const TArray<float> Whispered = Render(MakeParams(F0Hz, VowelA.Name, 1.0, /*Voicing=*/0.0, /*Breathiness=*/1.0),
        60606, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("Whispered render succeeded (%s: %s)"), *ErrorCode, *Error), Whispered.Num() > 0);
    if (Voiced.Num() == 0 || Whispered.Num() == 0)
    {
        return false;
    }

    TArray<float> VoicedMagnitudes;
    TArray<float> WhisperedMagnitudes;
    float VoicedBinHz = 0.f;
    float WhisperedBinHz = 0.f;
    TestTrue(TEXT("Voiced spectrum computed"), AverageSpectrum(Voiced, VoicedMagnitudes, VoicedBinHz));
    TestTrue(TEXT("Whispered spectrum computed"), AverageSpectrum(Whispered, WhisperedMagnitudes, WhisperedBinHz));

    const float VoicedCombDb = HarmonicCombDb(VoicedMagnitudes, VoicedBinHz, F0Hz, CombHarmonics);
    const float WhisperedCombDb = HarmonicCombDb(WhisperedMagnitudes, WhisperedBinHz, F0Hz, CombHarmonics);

    // A whisper's spectrum is continuous, so the on-grid and off-grid probes sample the
    // same formant envelope: the residual is the envelope's own asymmetry between the
    // two probe sets (about -1 dB for /a/ at 220 Hz), not a comb.
    TestTrue(FString::Printf(TEXT("voicing=1 produces a comb: harmonics %.1f dB above the midpoints"), VoicedCombDb),
        VoicedCombDb > 12.f);
    TestTrue(FString::Printf(TEXT("voicing=0/breathiness=1 produces no comb: %.1f dB"), WhisperedCombDb),
        WhisperedCombDb < 8.f);
    TestTrue(FString::Printf(TEXT("Comb contrast is %.1f dB"), VoicedCombDb - WhisperedCombDb),
        (VoicedCombDb - WhisperedCombDb) > 20.f);

    return true;
}

// =========================================================================================
// F. Determinism, including the jitter and shimmer. The differing-seed half is what proves
//    the RNG is actually consumed: a generator that ignored the seed would pass the
//    byte-identical half trivially.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenFormantDeterminismTest,
    "PinWright.audio.gen.formant.SameSeedRendersIdentically",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenFormantDeterminismTest::RunTest(const FString& Parameters)
{
    using namespace PwGenFormantTestHelpers;

    // breathiness 0, so the only randomness left in the render is jitter and shimmer.
    const FPwSynthParams Params = MakeParams(180.0, VowelA.Name, 1.0, /*Voicing=*/1.0, /*Breathiness=*/0.0);

    FString ErrorCode;
    FString Error;
    const TArray<float> First = Render(Params, 20260817, ErrorCode, Error, /*NumSamples=*/16384);
    const TArray<float> Second = Render(Params, 20260817, ErrorCode, Error, /*NumSamples=*/16384);
    const TArray<float> Other = Render(Params, 20260818, ErrorCode, Error, /*NumSamples=*/16384);

    TestTrue(TEXT("All three renders succeeded"), First.Num() > 0 && Second.Num() > 0 && Other.Num() > 0);
    if (First.Num() == 0 || Second.Num() == 0 || Other.Num() == 0)
    {
        return false;
    }

    TestEqual(TEXT("Both renders are the same length"), Second.Num(), First.Num());

    int32 FirstDifferingSample = INDEX_NONE;
    for (int32 Index = 0; Index < First.Num(); ++Index)
    {
        if (First[Index] != Second[Index])
        {
            FirstDifferingSample = Index;
            break;
        }
    }
    TestEqual(TEXT("Same seed renders byte-identically"), FirstDifferingSample, INDEX_NONE);

    int32 SeedSensitiveSamples = 0;
    for (int32 Index = 0; Index < First.Num(); ++Index)
    {
        if (First[Index] != Other[Index])
        {
            ++SeedSensitiveSamples;
        }
    }
    TestTrue(FString::Printf(TEXT("A different seed changes the jitter/shimmer (%d of %d samples differ)"),
        SeedSensitiveSamples, First.Num()), SeedSensitiveSamples > First.Num() / 4);

    return true;
}

// =========================================================================================
// G. Failure direction (rpc-design.md §12). Each rejection is checked for its code AND for
//    leaving the caller's buffer exactly as it found it.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenFormantRejectsBadParamsTest,
    "PinWright.audio.gen.formant.RejectsInvalidParamsWithoutWriting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenFormantRejectsBadParamsTest::RunTest(const FString& Parameters)
{
    using namespace PwGenFormantTestHelpers;

    const FPwSynthModulation NoModulation;
    const TArray<FPwSynthPitchPoint> NoPitchEnvelope;

    auto Reject = [&](const TCHAR* What, const FPwSynthParams& Params, const TCHAR* ExpectedInMessage)
    {
        TArray<float> Buffer = MakeSentinelBuffer();
        FPwSeededRandom Rng(11);
        FString ErrorCode;
        FString Error;

        const bool bSucceeded = PwGenFormant(Params, NoPitchEnvelope, NoModulation, TestSampleRate, Rng,
            TArrayView<float>(Buffer), ErrorCode, Error);

        TestFalse(FString::Printf(TEXT("%s is rejected"), What), bSucceeded);
        TestEqual(FString::Printf(TEXT("%s reports INVALID_PARAMS"), What), ErrorCode,
            FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(FString::Printf(TEXT("%s message names '%s' (got: %s)"), What, ExpectedInMessage, *Error),
            Error.Contains(ExpectedInMessage));
        TestTrue(FString::Printf(TEXT("%s left the output buffer untouched"), What), IsSentinelIntact(Buffer));
    };

    // An unrecognised vowel names the valid set instead of quietly becoming /a/.
    Reject(TEXT("An unknown vowel"), MakeParams(220.0, TEXT("q")), TEXT("a, e, i, o, u"));

    // Non-positive f0. Zero is not a small number - it must not read as "very low pitch".
    Reject(TEXT("f0Hz of zero"), MakeParams(0.0, VowelA.Name), TEXT("f0Hz"));
    Reject(TEXT("Negative f0Hz"), MakeParams(-220.0, VowelA.Name), TEXT("f0Hz"));

    // formantShift outside the schema's documented range, in both directions.
    Reject(TEXT("formantShift above range"), MakeParams(220.0, VowelA.Name, 3.0), TEXT("formantShift"));
    Reject(TEXT("formantShift below range"), MakeParams(220.0, VowelA.Name, 0.1), TEXT("formantShift"));

    // A source with neither pulse nor noise renders silence; that is an error, not a
    // successful empty layer.
    Reject(TEXT("voicing and breathiness both zero"),
        MakeParams(220.0, VowelA.Name, 1.0, /*Voicing=*/0.0, /*Breathiness=*/0.0), TEXT("silent"));

    // A missing required parameter is named as missing, before any range talk.
    FPwSynthParams NoF0;
    SetEnumParam(NoF0, TEXT("vowel"), VowelA.Name);
    Reject(TEXT("Missing f0Hz"), NoF0, TEXT("f0Hz"));

    FPwSynthParams NoVowel;
    SetNumberParam(NoVowel, TEXT("f0Hz"), 220.0);
    Reject(TEXT("Missing vowel"), NoVowel, TEXT("vowel"));

    return true;
}
