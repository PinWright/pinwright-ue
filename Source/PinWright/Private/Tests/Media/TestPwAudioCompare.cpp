// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the difference report and analysis-by-resynthesis (AudioGen/PwAudioCompare.h).
//
// THE CLOSED LOOP IS THE ONLY GROUND TRUTH HERE, AND IT IS WHERE THE WEIGHT SITS.
// Every other assertion in this subsystem compares an analyzer against another analyzer, or
// against a synthetic fixture the same author wrote. The loop
//
//     known recipe -> PwRenderRecipe -> PwDecomposeBuffer -> PwDecompositionToRecipe -> ?
//
// does not: the mode frequencies, gains and decays at the end are compared against numbers that
// were chosen before any analysis ran, by a path (render, spectrogram, peak picking, track
// continuation, weighted log-amplitude regression, T60 conversion, gain extrapolation) that has
// no way to know what they were. An analyzer that quietly reported its own input, or that
// confused tau with T60, or that dropped the frame-centre gain offset, fails this and cannot
// fake passing it. FPwRoundTripRecoversKnownModalRecipeTest is the single highest-value
// assertion in the file.
//
// TOLERANCES, AND WHY EACH IS WHAT IT IS. Stated here once rather than argued at each call site:
//
//   frequency  2% (about 35 cents). One analysis bin on the module's reference grid (2048 / 48
//              kHz) is 23.4 Hz, which is 5.9% of the lowest fixture mode - so a 2% bound asserts
//              the parabolic sub-bin peak estimator did materially better than bin-centre
//              rounding, which is the property under test, while leaving room for the
//              decomposer's own FFT-size choice and for sidelobe pull from the neighbouring
//              modes.
//   gain       3 dB. Two known systematic terms sit inside it: the Hann window averages a
//              decaying envelope over 42.6 ms and reads slightly high (a fraction of a dB at
//              these decays), and the extrapolation back to t = 0 rests on the fit's reference
//              time being the track's first point. 3 dB is a factor of two in power - loose
//              enough to survive both, tight enough that dropping the extrapolation entirely
//              (which costs 60 * 21.3 / T60 dB, i.e. 4.3 dB at the fixture's shortest mode)
//              fails the test.
//   decay      25%, as a RATIO either side of 1 rather than a millisecond span, because a
//              25% error means the same thing at 300 ms and at 3 s. It is far above the
//              regression's own error on a genuinely exponential envelope and far below the
//              6.9x error that reporting the e-folding time instead of the T60 would produce.
//
// The fixture uses three widely separated, deliberately INHARMONIC modes (400 / 970 / 1830 Hz).
// Inharmonic so no two of them can be confused for octaves of each other by the matcher, and
// separated by tens of bins so the sidelobe leakage the tracker's own documentation warns about
// cannot pull one mode's estimate toward another. Gains stay at or below -6 dB so the summed
// bank cannot reach full scale and clip - a clipped render would be measuring the limiter.
//
// Per rpc-design.md §12 the failure direction carries equal weight, and the ORDER of the
// failure checks is itself asserted: a buffer full of NaN must come back as
// AUDIO_NON_FINITE_SAMPLES and not as AUDIO_EMPTY_BUFFER, because NaN compares false against
// every threshold and therefore measures as digital silence unless it is named first.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioCompare.h"
#include "AudioGen/PwAudioDecompose.h"
#include "AudioGen/PwStft.h"
#include "AudioGen/PwSynthDsp.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Handlers/ErrorCodes.h"

#include "Dom/JsonObject.h"
#include "Math/NumericLimits.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true.
namespace PwAudioCompareTestHelpers
{
    constexpr int32 TestSampleRate = 48000;

    /** The fixture bank. Inharmonic, widely separated, and quiet enough that the sum cannot clip. */
    const TArray<double> FixtureFreqs = { 400.0, 970.0, 1830.0 };
    const TArray<double> FixtureGains = { -6.0, -12.0, -18.0 };
    const TArray<double> FixtureDecays = { 600.0, 450.0, 300.0 };
    constexpr double FixtureDurationMs = 1500.0;

    /** See the tolerance discussion in the file header. */
    constexpr double FreqTolerance = 0.02;
    constexpr double GainToleranceDb = 3.0;
    constexpr double DecayToleranceRatio = 1.25;

    FString JoinNumbers(const TArray<double>& Values)
    {
        TArray<FString> Parts;
        Parts.Reserve(Values.Num());
        for (const double Value : Values)
        {
            Parts.Add(FString::Printf(TEXT("%f"), Value));
        }
        return FString::Join(Parts, TEXT(","));
    }

    /**
     * A bare modal recipe: no master fx, no normalize, no fades. Normalization in particular is
     * left off on purpose - it would rescale the bus and make every recovered gain a measurement
     * of the normalizer instead of of the modes.
     */
    FString ModalRecipeJson(const TArray<double>& Freqs, const TArray<double>& Gains,
        const TArray<double>& Decays, double DurationMs = FixtureDurationMs, int32 Seed = 1)
    {
        return FString::Printf(
            TEXT("{\"version\":1,\"seed\":%d,\"sampleRate\":%d,\"durationMs\":%f,\"layers\":[")
            TEXT("{\"generator\":{\"kind\":\"modal\",\"params\":{\"modeFreqsHz\":[%s],")
            TEXT("\"modeGainsDb\":[%s],\"modeDecaysMs\":[%s],\"exciter\":\"impulse\"}}}")
            TEXT("],\"master\":{}}"),
            Seed, TestSampleRate, DurationMs,
            *JoinNumbers(Freqs), *JoinNumbers(Gains), *JoinNumbers(Decays));
    }

    /** Fixtures go through the real parser, so every test renders the bag production produces. */
    bool BuildRecipe(const FString& Json, FPwSynthRecipe& Out, FString& OutError)
    {
        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            OutError = TEXT("fixture is not valid JSON");
            return false;
        }
        return ParseSynthRecipe(Root, Out, OutError);
    }

    bool RenderJson(const FString& Json, FPwAudioBuffer& Out, FString& OutError)
    {
        FPwSynthRecipe Recipe;
        if (!BuildRecipe(Json, Recipe, OutError))
        {
            return false;
        }
        FPwRenderReport Report;
        FString Code;
        return PwRenderRecipe(Recipe, Out, Report, Code, OutError);
    }

    bool RenderRecipe(const FPwSynthRecipe& Recipe, FPwAudioBuffer& Out, FString& OutError)
    {
        FPwRenderReport Report;
        FString Code;
        return PwRenderRecipe(Recipe, Out, Report, Code, OutError);
    }

    const FPwSynthLayer* FindLayer(const FPwSynthRecipe& Recipe, EPwSynthGeneratorKind Kind)
    {
        for (const FPwSynthLayer& Layer : Recipe.Layers)
        {
            if (Layer.Generator.Kind == Kind)
            {
                return &Layer;
            }
        }
        return nullptr;
    }

    /**
     * A quiet NaN built from its IEEE-754 bit pattern rather than from sqrt(-1) or <limits>,
     * matching the helper TestPwAudioAnalysis.cpp and TestPwDecomposeFit.cpp already use: a
     * compiler free to fold sqrt(-1) at compile time under fast-math is free to fold it to
     * something that is not a NaN at all, which would silently turn this into a test of nothing.
     */
    float MakeNaN()
    {
        constexpr uint32 QuietNaNBits = 0x7FC00000u;
        float Value = 0.f;
        FMemory::Memcpy(&Value, &QuietNaNBits, sizeof(Value));
        return Value;
    }

    /** A buffer of a constant sample value in both channels - fixtures for the degenerate cases. */
    FPwAudioBuffer ConstantBuffer(int32 NumFrames, float Value, int32 SampleRate = TestSampleRate)
    {
        FPwAudioBuffer Buffer;
        Buffer.SampleRate = SampleRate;
        Buffer.SetNumFrames(NumFrames);
        for (int32 Index = 0; Index < NumFrames; ++Index)
        {
            Buffer.Left[Index] = Value;
            Buffer.Right[Index] = Value;
        }
        return Buffer;
    }

    /**
     * RMS difference of the two mean magnitude spectra, in dB - a single number for "how far
     * apart do these two sounds sit spectrally".
     *
     * The MEAN spectrum rather than the frame-by-frame one, so two renders of different lengths
     * are directly comparable: NumBins depends only on FftSize, never on the signal length.
     * Returns a negative value when either spectrogram could not be computed, so a caller cannot
     * read a failure as a perfect match.
     */
    double SpectralDistanceDb(const FPwAudioBuffer& A, const FPwAudioBuffer& B)
    {
        const FPwStftSettings Settings;

        TArray<float> MeanA;
        TArray<float> MeanB;
        for (int32 Side = 0; Side < 2; ++Side)
        {
            const FPwAudioBuffer& Buffer = (Side == 0) ? A : B;
            TArray<float>& Mean = (Side == 0) ? MeanA : MeanB;

            TArray<float> Mono;
            Mono.SetNumUninitialized(Buffer.NumFrames());
            for (int32 Index = 0; Index < Buffer.NumFrames(); ++Index)
            {
                Mono[Index] = 0.5f * (Buffer.Left[Index] + Buffer.Right[Index]);
            }

            FPwStftResult Stft;
            if (!PwComputeStft(Mono, Buffer.SampleRate, Settings, Stft))
            {
                return -1.0;
            }
            Mean.SetNumZeroed(Stft.NumBins);
            for (int32 Frame = 0; Frame < Stft.NumFrames; ++Frame)
            {
                for (int32 Bin = 0; Bin < Stft.NumBins; ++Bin)
                {
                    Mean[Bin] += Stft.Magnitudes[Frame * Stft.NumBins + Bin];
                }
            }
            for (float& Value : Mean)
            {
                Value /= static_cast<float>(FMath::Max(Stft.NumFrames, 1));
            }
        }

        if (MeanA.Num() != MeanB.Num() || MeanA.Num() == 0)
        {
            return -1.0;
        }

        double SumSquares = 0.0;
        for (int32 Bin = 0; Bin < MeanA.Num(); ++Bin)
        {
            const double Difference = static_cast<double>(PwMagnitudeToDb(MeanA[Bin]))
                - static_cast<double>(PwMagnitudeToDb(MeanB[Bin]));
            SumSquares += Difference * Difference;
        }
        return FMath::Sqrt(SumSquares / static_cast<double>(MeanA.Num()));
    }

    /**
     * A hand-built decomposition carrying exactly the modes given, index-aligned with a matching
     * partial set whose tracks start at 0 ms - so no gain extrapolation applies and the recipe's
     * modeGainsDb must come back byte-for-byte as the fits' InitialGainDb.
     */
    FPwDecomposition MakeModalDecomposition(const TArray<double>& Freqs, const TArray<double>& Gains,
        const TArray<double>& Decays, double DurationMs = FixtureDurationMs)
    {
        FPwDecomposition Out;
        Out.bMeasured = true;
        Out.SampleRate = TestSampleRate;
        Out.DurationMs = DurationMs;
        for (int32 Index = 0; Index < Freqs.Num(); ++Index)
        {
            FPwPartialTrack& Track = Out.Partials.AddDefaulted_GetRef();
            Track.StartMs = 0.0;
            Track.EndMs = DurationMs;
            Track.MeanFreqHz = Freqs[Index];
            Track.PeakAmpLinear = FMath::Pow(10.0, Gains[Index] / 20.0);
            FPwPartialPoint& Point = Track.Points.AddDefaulted_GetRef();
            Point.TimeMs = 0.0;
            Point.FreqHz = Freqs[Index];
            Point.AmpLinear = Track.PeakAmpLinear;

            FPwModalFit& Fit = Out.Modes.AddDefaulted_GetRef();
            Fit.FreqHz = Freqs[Index];
            Fit.InitialGainDb = Gains[Index];
            Fit.DecayMs = Decays[Index];
            Fit.bMeasured = true;
        }
        return Out;
    }

    /** Appends one residual band holding a flat level for the whole span. */
    void AddResidualBand(FPwDecomposition& Decomposition, double LowHz, double HighHz, double LevelDb)
    {
        FPwResidualBand& Band = Decomposition.Residual.AddDefaulted_GetRef();
        Band.LowHz = LowHz;
        Band.HighHz = HighHz;
        FPwDbPoint& First = Band.EnvelopeDb.AddDefaulted_GetRef();
        First.TimeMs = 0.0;
        First.ValueDb = LevelDb;
        FPwDbPoint& Last = Band.EnvelopeDb.AddDefaulted_GetRef();
        Last.TimeMs = Decomposition.DurationMs;
        Last.ValueDb = LevelDb;
    }

    /** Index of the emitted mode nearest InHz, or INDEX_NONE for an empty bank. */
    int32 NearestModeIndex(const TArray<double>& Freqs, double InHz)
    {
        int32 Best = INDEX_NONE;
        double BestDistance = TNumericLimits<double>::Max();
        for (int32 Index = 0; Index < Freqs.Num(); ++Index)
        {
            const double Distance = FMath::Abs(Freqs[Index] - InHz);
            if (Distance < BestDistance)
            {
                BestDistance = Distance;
                Best = Index;
            }
        }
        return Best;
    }

    bool HasDiagnosis(const FPwCompareResult& Result, const TCHAR* Diagnosis)
    {
        for (const FPwCompareDeviation& Deviation : Result.Deviations)
        {
            if (Deviation.Diagnosis == Diagnosis)
            {
                return true;
            }
        }
        return false;
    }
}

// =================================================================================================
// The closed loop.
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwRoundTripRecoversKnownModalRecipeTest,
    "PinWright.audio.to_recipe.RoundTripRecoversKnownModalRecipe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwRoundTripRecoversKnownModalRecipeTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioCompareTestHelpers;

    // The call runs BEFORE the message is formatted, deliberately: TestTrue's two arguments are
    // evaluated in an unspecified order, so building the message inline would print the error
    // string as it was before the call - i.e. empty on exactly the run that failed.
    FString Error;
    FPwAudioBuffer Audio;
    const bool bRendered =
        RenderJson(ModalRecipeJson(FixtureFreqs, FixtureGains, FixtureDecays), Audio, Error);
    if (!TestTrue(FString::Printf(TEXT("the fixture recipe renders: %s"), *Error), bRendered))
    {
        return false;
    }

    const FPwDecomposeSettings Settings;
    FPwDecomposition Decomposition;
    FString Code;
    const bool bDecomposed = PwDecomposeBuffer(Audio, Settings, Decomposition, Code, Error);
    if (!TestTrue(FString::Printf(TEXT("the render decomposes: %s %s"), *Code, *Error), bDecomposed))
    {
        return false;
    }
    TestTrue(TEXT("the decomposition is measured"), Decomposition.bMeasured);

    FPwSynthRecipe Recovered;
    Code.Reset();
    Error.Reset();
    const bool bMapped = PwDecompositionToRecipe(Decomposition, Recovered, Code, Error);
    if (!TestTrue(FString::Printf(TEXT("the decomposition maps to a recipe: %s %s"), *Code, *Error),
            bMapped))
    {
        return false;
    }
    TestTrue(TEXT("a successful mapping reports no error code (any message is a note)"),
        Code.IsEmpty());

    const FPwSynthLayer* Modal = FindLayer(Recovered, EPwSynthGeneratorKind::Modal);
    if (!TestNotNull(TEXT("the recovered recipe carries a modal layer"), Modal))
    {
        return false;
    }
    TestTrue(TEXT("the modal layer carries no amplitude envelope - the modes are the envelope"),
        Modal->AmpEnvelope.Num() == 0);

    const TArray<double>* Freqs = Modal->Generator.Params.GetNumbers(FName(TEXT("modeFreqsHz")));
    const TArray<double>* Gains = Modal->Generator.Params.GetNumbers(FName(TEXT("modeGainsDb")));
    const TArray<double>* Decays = Modal->Generator.Params.GetNumbers(FName(TEXT("modeDecaysMs")));
    if (!TestNotNull(TEXT("modeFreqsHz survived the round trip"), Freqs)
        || !TestNotNull(TEXT("modeGainsDb survived the round trip"), Gains)
        || !TestNotNull(TEXT("modeDecaysMs survived the round trip"), Decays))
    {
        return false;
    }
    TestTrue(TEXT("the three mode arrays agree in length"),
        Freqs->Num() == Gains->Num() && Freqs->Num() == Decays->Num());

    // Each KNOWN mode must be recovered by SOME emitted mode. Not "exactly three modes": the
    // partial tracker's own documentation records that a Hann window's -31 dB sidelobes are real
    // local maxima and are picked as peaks whenever they clear the relative threshold, so a
    // three-mode render legitimately yields a handful of quiet extra tracks flanking the real
    // ones. Asserting a count would be asserting a known limitation of a different stage.
    for (int32 Index = 0; Index < FixtureFreqs.Num(); ++Index)
    {
        const double ExpectedFreq = FixtureFreqs[Index];
        const int32 Match = NearestModeIndex(*Freqs, ExpectedFreq);
        if (!TestTrue(FString::Printf(TEXT("a mode was recovered near %.0f Hz"), ExpectedFreq),
                Match != INDEX_NONE))
        {
            continue;
        }

        const double RecoveredFreq = (*Freqs)[Match];
        const double RecoveredGain = (*Gains)[Match];
        const double RecoveredDecay = (*Decays)[Match];

        TestTrue(FString::Printf(
            TEXT("mode %d frequency: expected %.1f Hz, recovered %.1f Hz (%.2f%% off, bound %.0f%%)"),
            Index, ExpectedFreq, RecoveredFreq,
            100.0 * FMath::Abs(RecoveredFreq - ExpectedFreq) / ExpectedFreq, 100.0 * FreqTolerance),
            FMath::Abs(RecoveredFreq - ExpectedFreq) <= FreqTolerance * ExpectedFreq);

        TestTrue(FString::Printf(
            TEXT("mode %d gain: expected %.1f dB, recovered %.1f dB (%.2f dB off, bound %.1f dB)"),
            Index, FixtureGains[Index], RecoveredGain,
            FMath::Abs(RecoveredGain - FixtureGains[Index]), GainToleranceDb),
            FMath::Abs(RecoveredGain - FixtureGains[Index]) <= GainToleranceDb);

        const double DecayRatio = RecoveredDecay / FixtureDecays[Index];
        TestTrue(FString::Printf(
            TEXT("mode %d decay: expected %.0f ms, recovered %.0f ms (ratio %.3f, bound %.2f)"),
            Index, FixtureDecays[Index], RecoveredDecay, DecayRatio, DecayToleranceRatio),
            DecayRatio <= DecayToleranceRatio && DecayRatio >= 1.0 / DecayToleranceRatio);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwRoundTripConvergesRatherThanDriftsTest,
    "PinWright.audio.to_recipe.SecondRoundTripConvergesRatherThanDrifts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwRoundTripConvergesRatherThanDriftsTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioCompareTestHelpers;

    // One pass through the loop can be close by luck. The property that matters for an iterating
    // agent is that the SECOND pass does not walk further away - a mapping that drifts turns
    // every iteration into a new sound rather than a nearer one.
    FString Error;
    FPwAudioBuffer First;
    const bool bRendered =
        RenderJson(ModalRecipeJson(FixtureFreqs, FixtureGains, FixtureDecays), First, Error);
    if (!TestTrue(FString::Printf(TEXT("the fixture renders: %s"), *Error), bRendered))
    {
        return false;
    }

    const FPwDecomposeSettings Settings;
    FPwAudioBuffer Second;
    FPwAudioBuffer Third;
    FPwAudioBuffer* Sources[] = { &First, &Second };
    FPwAudioBuffer* Targets[] = { &Second, &Third };
    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        FPwDecomposition Decomposition;
        FString Code;
        const bool bDecomposed =
            PwDecomposeBuffer(*Sources[Pass], Settings, Decomposition, Code, Error);
        if (!TestTrue(FString::Printf(TEXT("pass %d decomposes: %s %s"), Pass, *Code, *Error),
                bDecomposed))
        {
            return false;
        }
        FPwSynthRecipe Recipe;
        Code.Reset();
        Error.Reset();
        const bool bMapped = PwDecompositionToRecipe(Decomposition, Recipe, Code, Error);
        if (!TestTrue(FString::Printf(TEXT("pass %d maps to a recipe: %s %s"), Pass, *Code, *Error),
                bMapped))
        {
            return false;
        }
        const bool bRerendered = RenderRecipe(Recipe, *Targets[Pass], Error);
        if (!TestTrue(FString::Printf(TEXT("pass %d re-renders: %s"), Pass, *Error), bRerendered))
        {
            return false;
        }
    }

    // Deliberately unrelated: a low, slow, single-mode body shares neither register nor tail with
    // the fixture, so any mapping that is doing something rather than nothing beats it.
    FPwAudioBuffer Unrelated;
    const bool bUnrelatedRendered =
        RenderJson(ModalRecipeJson({ 82.0 }, { -6.0 }, { 4000.0 }), Unrelated, Error);
    if (!TestTrue(FString::Printf(TEXT("the unrelated fixture renders: %s"), *Error),
            bUnrelatedRendered))
    {
        return false;
    }

    const double Converged = SpectralDistanceDb(Second, Third);
    const double SecondToUnrelated = SpectralDistanceDb(Second, Unrelated);
    const double ThirdToUnrelated = SpectralDistanceDb(Third, Unrelated);

    // A negative distance is the metric's own failure signal; asserting it away would let a
    // broken measurement read as a perfect match.
    if (!TestTrue(TEXT("every spectral distance was measurable"),
            Converged >= 0.0 && SecondToUnrelated >= 0.0 && ThirdToUnrelated >= 0.0))
    {
        return false;
    }

    TestTrue(FString::Printf(
        TEXT("two successive round trips converge: d(2,3) = %.2f dB, d(2,unrelated) = %.2f dB"),
        Converged, SecondToUnrelated), Converged < SecondToUnrelated);
    TestTrue(FString::Printf(
        TEXT("two successive round trips converge: d(2,3) = %.2f dB, d(3,unrelated) = %.2f dB"),
        Converged, ThirdToUnrelated), Converged < ThirdToUnrelated);

    return true;
}

// =================================================================================================
// to_recipe: the mapping, the schema check, and the failure direction.
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwToRecipeAlwaysParsesTest,
    "PinWright.audio.to_recipe.EveryEmittedRecipeParses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwToRecipeAlwaysParsesTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioCompareTestHelpers;

    // Several input shapes, including a degenerate one. The check is not "it returned true" -
    // the emitted recipe is serialized and re-parsed here, through the schema's own parser,
    // which is what the write path cannot fake (rpc-design.md §4).
    TArray<TPair<FString, FPwDecomposition>> Cases;

    Cases.Emplace(TEXT("modes only"),
        MakeModalDecomposition(FixtureFreqs, FixtureGains, FixtureDecays));

    {
        // Degenerate: one mode, the shortest duration the schema accepts, nothing else at all.
        FPwDecomposition Degenerate = MakeModalDecomposition({ 1000.0 }, { -20.0 }, { 50.0 },
            PwSynthLimits::MinDurationMs);
        Cases.Emplace(TEXT("one mode at the minimum duration"), MoveTemp(Degenerate));
    }

    {
        FPwDecomposition WithTransient =
            MakeModalDecomposition(FixtureFreqs, FixtureGains, FixtureDecays);
        FPwTransient& Transient = WithTransient.Transients.AddDefaulted_GetRef();
        Transient.StartMs = 0.0;
        Transient.PeakMs = 4.0;
        Transient.EndMs = 18.0;              // longer than the impulse threshold
        Transient.CentroidHz = 2000.0;
        Transient.BandwidthHz = 3000.0;
        Transient.LowHz = 300.0;
        Transient.HighHz = 9000.0;           // span 8700 Hz > centroid -> broadband -> noise
        Transient.PeakDb = -8.0;
        Cases.Emplace(TEXT("modes plus a broadband transient"), MoveTemp(WithTransient));
    }

    {
        FPwDecomposition WithResidual =
            MakeModalDecomposition(FixtureFreqs, FixtureGains, FixtureDecays);
        AddResidualBand(WithResidual, 100.0, 200.0, -30.0);
        AddResidualBand(WithResidual, 200.0, 400.0, -30.0);
        AddResidualBand(WithResidual, 400.0, 800.0, -30.0);
        AddResidualBand(WithResidual, 800.0, 1600.0, -30.0);
        Cases.Emplace(TEXT("modes plus a residual"), MoveTemp(WithResidual));
    }

    {
        // No modes at all - a purely noisy source. The mapping must still produce something the
        // schema accepts, from the residual alone.
        FPwDecomposition ResidualOnly;
        ResidualOnly.bMeasured = true;
        ResidualOnly.SampleRate = TestSampleRate;
        ResidualOnly.DurationMs = 800.0;
        AddResidualBand(ResidualOnly, 500.0, 1000.0, -24.0);
        AddResidualBand(ResidualOnly, 1000.0, 2000.0, -24.0);
        Cases.Emplace(TEXT("residual only"), MoveTemp(ResidualOnly));
    }

    for (const TPair<FString, FPwDecomposition>& Case : Cases)
    {
        FPwSynthRecipe Recipe;
        FString Code;
        FString Error;
        const bool bMapped = PwDecompositionToRecipe(Case.Value, Recipe, Code, Error);
        if (!TestTrue(FString::Printf(TEXT("%s maps to a recipe: %s %s"), *Case.Key, *Code, *Error),
                bMapped))
        {
            continue;
        }
        TestTrue(FString::Printf(TEXT("%s: a note is not a failure, so the code stays empty"),
            *Case.Key), Code.IsEmpty());
        TestTrue(FString::Printf(TEXT("%s: the recipe carries at least one layer"), *Case.Key),
            Recipe.Layers.Num() > 0);

        // Re-parse from the canonical serialization. PwDecompositionToRecipe already does this
        // internally; doing it again here is what makes the assertion independent of it.
        FString ParseError;
        FPwSynthRecipe Reparsed;
        const bool bParsed = ParseSynthRecipe(SerializeSynthRecipe(Recipe), Reparsed, ParseError);
        TestTrue(FString::Printf(TEXT("%s: the emitted recipe parses - %s"), *Case.Key, *ParseError),
            bParsed);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwToRecipeExciterFollowsTransientTest,
    "PinWright.audio.to_recipe.ExciterFollowsTheTransientShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwToRecipeExciterFollowsTransientTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioCompareTestHelpers;

    struct FCase
    {
        const TCHAR* Name;
        bool bHasTransient;
        double SpanMs;
        double CentroidHz;
        double LowHz;
        double HighHz;
        const TCHAR* ExpectedExciter;
    };

    const FCase Cases[] = {
        // No transient at all: `impulse` is the absence of a choice, not a guess at one.
        { TEXT("no transient"),      false, 0.0,  0.0,    0.0,   0.0,    TEXT("impulse") },
        // A 1 ms burst is a click, whatever its spectrum.
        { TEXT("short burst"),       true,  1.0,  2000.0, 300.0, 9000.0, TEXT("impulse") },
        // Long and broadband: the 5-95% span (8700 Hz) exceeds the centroid (2000 Hz).
        { TEXT("long broadband"),    true,  20.0, 2000.0, 300.0, 9000.0, TEXT("noise") },
        // Long and narrow: the span (400 Hz) is a fifth of the centroid - a soft mallet.
        { TEXT("long narrow"),       true,  20.0, 2000.0, 1800.0, 2200.0, TEXT("strike") }
    };

    for (const FCase& Case : Cases)
    {
        FPwDecomposition Decomposition =
            MakeModalDecomposition(FixtureFreqs, FixtureGains, FixtureDecays);
        if (Case.bHasTransient)
        {
            FPwTransient& Transient = Decomposition.Transients.AddDefaulted_GetRef();
            Transient.StartMs = 0.0;
            Transient.PeakMs = 0.5 * Case.SpanMs;
            Transient.EndMs = Case.SpanMs;
            Transient.CentroidHz = Case.CentroidHz;
            Transient.LowHz = Case.LowHz;
            Transient.HighHz = Case.HighHz;
            Transient.PeakDb = -8.0;
        }

        FPwSynthRecipe Recipe;
        FString Code;
        FString Error;
        const bool bMapped = PwDecompositionToRecipe(Decomposition, Recipe, Code, Error);
        if (!TestTrue(FString::Printf(TEXT("%s maps to a recipe: %s %s"), Case.Name, *Code, *Error),
                bMapped))
        {
            continue;
        }

        const FPwSynthLayer* Modal = FindLayer(Recipe, EPwSynthGeneratorKind::Modal);
        if (!TestNotNull(*FString::Printf(TEXT("%s: a modal layer was emitted"), Case.Name), Modal))
        {
            continue;
        }
        const FString Exciter = Modal->Generator.Params.GetString(FName(TEXT("exciter")));
        TestEqual(FString::Printf(TEXT("%s selects the right exciter"), Case.Name),
            Exciter, FString(Case.ExpectedExciter));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwToRecipeNoiseColorCorrectsBandwidthTiltTest,
    "PinWright.audio.to_recipe.NoiseColorCorrectsForLogBandBandwidth",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwToRecipeNoiseColorCorrectsBandwidthTiltTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioCompareTestHelpers;

    // A residual band's level is the TOTAL power in the band and the bands are log-spaced, so a
    // band one octave up holds twice the bins - and twice the power - of a flat input. White
    // therefore measures as +3 dB/octave on this axis, not 0. Without the correction every white
    // residual would be authored as blue and every pink one as white, so both rows below fail if
    // PwToRecipeLimits::BandwidthTiltDbPerOctave is dropped.
    struct FCase
    {
        const TCHAR* Name;
        double SlopeDbPerOctave;
        const TCHAR* ExpectedColor;
    };

    const FCase Cases[] = {
        { TEXT("flat band levels"),       0.0, TEXT("pink") },
        { TEXT("+3 dB/oct band levels"),  3.0, TEXT("white") },
        { TEXT("-3 dB/oct band levels"), -3.0, TEXT("brown") },
        { TEXT("+6 dB/oct band levels"),  6.0, TEXT("blue") }
    };

    for (const FCase& Case : Cases)
    {
        FPwDecomposition Decomposition;
        Decomposition.bMeasured = true;
        Decomposition.SampleRate = TestSampleRate;
        Decomposition.DurationMs = 800.0;

        // Five octave-wide bands from 250 Hz, so log2(centre) advances by exactly 1 per band and
        // the fitted slope is the row's SlopeDbPerOctave by construction. Levels stay within the
        // 20 dB support window so every band contributes to the fit.
        double LowHz = 250.0;
        double LevelDb = -30.0;
        for (int32 Band = 0; Band < 5; ++Band)
        {
            AddResidualBand(Decomposition, LowHz, LowHz * 2.0, LevelDb);
            LowHz *= 2.0;
            LevelDb += Case.SlopeDbPerOctave;
        }

        FPwSynthRecipe Recipe;
        FString Code;
        FString Error;
        const bool bMapped = PwDecompositionToRecipe(Decomposition, Recipe, Code, Error);
        if (!TestTrue(FString::Printf(TEXT("%s maps to a recipe: %s %s"), Case.Name, *Code, *Error),
                bMapped))
        {
            continue;
        }

        const FPwSynthLayer* Noise = FindLayer(Recipe, EPwSynthGeneratorKind::Noise);
        if (!TestNotNull(*FString::Printf(TEXT("%s: a noise layer was emitted"), Case.Name), Noise))
        {
            continue;
        }
        TestEqual(FString::Printf(TEXT("%s picks the right colour"), Case.Name),
            Noise->Generator.Params.GetString(FName(TEXT("color"))), FString(Case.ExpectedColor));
        TestTrue(FString::Printf(TEXT("%s: the noise layer carries a measured contour"), Case.Name),
            Noise->AmpEnvelope.Num() > 0
                && Noise->AmpEnvelope.Num() <= PwSynthLimits::MaxEnvelopePoints);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwToRecipeRejectsUnusableInputTest,
    "PinWright.audio.to_recipe.RejectsUnusableInputLeavingOutUnmeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwToRecipeRejectsUnusableInputTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioCompareTestHelpers;

    // A recipe carrying layers, so a failure that leaves Out alone is visibly distinguishable
    // from one that clears it. The contract is that Out is CLEARED - a default-constructed
    // recipe has no layers and no duration, which both the parser and the renderer reject, so a
    // caller who ignored the returned bool cannot get a plausible-looking result.
    FPwSynthRecipe Sentinel;
    {
        FString Ignored;
        TestTrue(TEXT("the sentinel recipe builds"),
            BuildRecipe(ModalRecipeJson(FixtureFreqs, FixtureGains, FixtureDecays), Sentinel, Ignored));
    }

    TArray<TPair<FString, FPwDecomposition>> Cases;

    {
        // Unmeasured: the analysis never ran. Everything else in the struct is irrelevant.
        FPwDecomposition Unmeasured;
        Unmeasured.UnmeasuredReason = TEXT("the spectrogram was empty");
        Unmeasured.SampleRate = TestSampleRate;
        Unmeasured.DurationMs = FixtureDurationMs;
        Cases.Emplace(TEXT("unmeasured decomposition"), MoveTemp(Unmeasured));
    }
    {
        // Measured, but nothing in it the synthesizer can hold: every fit is a sustained partial
        // (DecayMs == 0, bMeasured false), and there is no residual either.
        FPwDecomposition NoLayers;
        NoLayers.bMeasured = true;
        NoLayers.SampleRate = TestSampleRate;
        NoLayers.DurationMs = FixtureDurationMs;
        for (int32 Index = 0; Index < 3; ++Index)
        {
            FPwModalFit& Fit = NoLayers.Modes.AddDefaulted_GetRef();
            Fit.FreqHz = 500.0 * (Index + 1);
            Fit.InitialGainDb = -10.0;
            Fit.DecayMs = 0.0;
            Fit.bMeasured = false;
        }
        Cases.Emplace(TEXT("no usable layer"), MoveTemp(NoLayers));
    }
    {
        // Measured, but at a rate the schema cannot express.
        FPwDecomposition BadRate =
            MakeModalDecomposition(FixtureFreqs, FixtureGains, FixtureDecays);
        BadRate.SampleRate = 100;
        Cases.Emplace(TEXT("sample rate below the schema floor"), MoveTemp(BadRate));
    }
    {
        // Measured, but spanning a duration the schema cannot express.
        FPwDecomposition BadDuration =
            MakeModalDecomposition(FixtureFreqs, FixtureGains, FixtureDecays);
        BadDuration.DurationMs = 0.0;
        Cases.Emplace(TEXT("zero duration"), MoveTemp(BadDuration));
    }

    for (const TPair<FString, FPwDecomposition>& Case : Cases)
    {
        FPwSynthRecipe Out = Sentinel;
        FString Code;
        FString Error;
        const bool bResult = PwDecompositionToRecipe(Case.Value, Out, Code, Error);

        TestFalse(FString::Printf(TEXT("%s is refused"), *Case.Key), bResult);
        TestEqual(FString::Printf(TEXT("%s reports INVALID_PARAMS"), *Case.Key),
            Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(FString::Printf(TEXT("%s explains itself"), *Case.Key), !Error.IsEmpty());
        TestTrue(FString::Printf(TEXT("%s leaves Out with no layers"), *Case.Key),
            Out.Layers.Num() == 0);
        TestTrue(FString::Printf(TEXT("%s leaves Out with no duration"), *Case.Key),
            Out.DurationMs == 0.0);
    }

    return true;
}

// =================================================================================================
// compare: the metric, both directions, and both sides of the mode matcher.
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCompareSelfIsAMatchTest,
    "PinWright.audio.compare.SelfComparisonReportsNoDeviation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCompareSelfIsAMatchTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioCompareTestHelpers;

    // A buffer against itself is the metric's own calibration: the analysis is deterministic, so
    // every delta must be EXACTLY zero. Any non-trivial deviation here is a bug in the metric
    // rather than a property of any sound, which is why the bound is 0 rather than a tolerance.
    FString Error;
    FPwAudioBuffer Audio;
    const bool bRendered =
        RenderJson(ModalRecipeJson(FixtureFreqs, FixtureGains, FixtureDecays), Audio, Error);
    if (!TestTrue(FString::Printf(TEXT("the fixture renders: %s"), *Error), bRendered))
    {
        return false;
    }

    FPwCompareResult Result;
    FString Code;
    Error.Reset();
    const bool bCompared = PwCompareAudio(Audio, Audio, Result, Code, Error);
    if (!TestTrue(FString::Printf(TEXT("a buffer compares against itself: %s %s"), *Code, *Error),
            bCompared))
    {
        return false;
    }

    TestTrue(TEXT("the report is measured"), Result.bMeasured);
    TestTrue(FString::Printf(TEXT("a buffer matches itself (%d deviations reported)"),
        Result.Deviations.Num()), Result.IsMatch());

    const TPair<const TCHAR*, const FPwCompareScalar*> Scalars[] = {
        { TEXT("onsetMs"),      &Result.OnsetMs },
        { TEXT("attackMs"),     &Result.AttackMs },
        { TEXT("decayMs"),      &Result.DecayMs },
        { TEXT("tailMs"),       &Result.TailMs },
        { TEXT("loudnessLufs"), &Result.LoudnessLufs },
        { TEXT("centroidHz"),   &Result.CentroidHz }
    };
    for (const TPair<const TCHAR*, const FPwCompareScalar*>& Scalar : Scalars)
    {
        if (!Scalar.Value->bMeasured)
        {
            // An unmeasured scalar is legal - a sustained tone has no decay - but it must carry
            // its reason, never sit silently at zero.
            TestTrue(FString::Printf(TEXT("%s explains why it is absent"), Scalar.Key),
                !Scalar.Value->UnmeasuredReason.IsEmpty());
            continue;
        }
        TestTrue(FString::Printf(TEXT("%s deviates by exactly zero against itself (got %g)"),
            Scalar.Key, Scalar.Value->Delta), Scalar.Value->Delta == 0.0);
    }

    TestTrue(TEXT("a buffer is missing none of its own modes"), Result.MissingModes.Num() == 0);
    TestTrue(TEXT("a buffer has none of its own modes to spare"), Result.ExtraModes.Num() == 0);
    TestTrue(TEXT("every mode paired with itself"),
        Result.ModePairs.Num() > 0 || Result.ReferenceUnfittableModes > 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCompareBrightnessIsSignedTest,
    "PinWright.audio.compare.BrightnessDeviationIsSignedInBothDirections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCompareBrightnessIsSignedTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioCompareTestHelpers;

    // The same three modes, with the top one 12 dB louder in the bright version - so the
    // difference is brightness alone and nothing else about the sound has moved.
    FString Error;
    FPwAudioBuffer Dark;
    FPwAudioBuffer Bright;
    const bool bDarkRendered = RenderJson(
        ModalRecipeJson(FixtureFreqs, { -6.0, -12.0, -18.0 }, FixtureDecays), Dark, Error);
    const bool bBrightRendered = RenderJson(
        ModalRecipeJson(FixtureFreqs, { -6.0, -12.0, -6.0 }, FixtureDecays), Bright, Error);
    if (!TestTrue(FString::Printf(TEXT("both brightness fixtures render: %s"), *Error),
            bDarkRendered && bBrightRendered))
    {
        return false;
    }

    // Both directions, because a one-sided measure hides half the failures (rpc-design.md §6):
    // a metric that scored "too bright" and "too dark" alike would pass the first half of this.
    FPwCompareResult Brighter;
    FPwCompareResult Darker;
    FString Code;
    Error.Reset();
    const bool bBrighterCompared = PwCompareAudio(Dark, Bright, Brighter, Code, Error);
    const bool bDarkerCompared = PwCompareAudio(Bright, Dark, Darker, Code, Error);
    if (!TestTrue(FString::Printf(TEXT("both directions compare: %s %s"), *Code, *Error),
            bBrighterCompared && bDarkerCompared))
    {
        return false;
    }

    if (!TestTrue(TEXT("both centroids were measured"),
            Brighter.CentroidHz.bMeasured && Darker.CentroidHz.bMeasured))
    {
        return false;
    }

    TestTrue(FString::Printf(TEXT("a brighter candidate reports a POSITIVE centroid delta (got %.1f Hz)"),
        Brighter.CentroidHz.Delta), Brighter.CentroidHz.Delta > 0.0);
    TestTrue(FString::Printf(TEXT("a darker candidate reports a NEGATIVE centroid delta (got %.1f Hz)"),
        Darker.CentroidHz.Delta), Darker.CentroidHz.Delta < 0.0);

    TestTrue(TEXT("the brighter comparison publishes a log-domain delta"),
        Brighter.CentroidDeltaCents.IsSet() && Brighter.CentroidDeltaCents.GetValue() > 0.0);
    TestTrue(TEXT("the darker comparison publishes a log-domain delta"),
        Darker.CentroidDeltaCents.IsSet() && Darker.CentroidDeltaCents.GetValue() < 0.0);

    TestTrue(TEXT("the brighter comparison diagnoses candidate_too_bright"),
        HasDiagnosis(Brighter, PwCompareDiagnosis::CandidateTooBright));
    TestTrue(TEXT("the darker comparison diagnoses candidate_too_dark"),
        HasDiagnosis(Darker, PwCompareDiagnosis::CandidateTooDark));

    // The reference value travels with every deviation so the caller can compute a target rather
    // than guess a step size.
    for (const FPwCompareDeviation& Deviation : Brighter.Deviations)
    {
        TestTrue(FString::Printf(TEXT("deviation '%s' names a quantity"), *Deviation.Diagnosis),
            !Deviation.Quantity.IsEmpty());
        TestTrue(FString::Printf(TEXT("deviation '%s' is drawn from the closed vocabulary"),
            *Deviation.Diagnosis), PwIsKnownCompareDiagnosis(Deviation.Diagnosis));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCompareReportsBothSidesOfTheMatcherTest,
    "PinWright.audio.compare.MissingAndExtraModesAreBothReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCompareReportsBothSidesOfTheMatcherTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioCompareTestHelpers;

    // A one-sided matcher passes half of this, which is exactly why both halves are asserted in
    // one test: the same pair of buffers, compared in both orders.
    constexpr double DroppedHz = 970.0;

    FString Error;
    FPwAudioBuffer Full;
    FPwAudioBuffer Reduced;
    const bool bFullRendered =
        RenderJson(ModalRecipeJson(FixtureFreqs, FixtureGains, FixtureDecays), Full, Error);
    const bool bReducedRendered = RenderJson(
        ModalRecipeJson({ 400.0, 1830.0 }, { -6.0, -18.0 }, { 600.0, 300.0 }), Reduced, Error);
    if (!TestTrue(FString::Printf(TEXT("both mode-set fixtures render: %s"), *Error),
            bFullRendered && bReducedRendered))
    {
        return false;
    }

    FPwCompareResult Missing;
    FPwCompareResult Extra;
    FString Code;
    Error.Reset();
    const bool bMissingCompared = PwCompareAudio(Full, Reduced, Missing, Code, Error);
    const bool bExtraCompared = PwCompareAudio(Reduced, Full, Extra, Code, Error);
    if (!TestTrue(FString::Printf(TEXT("both orders compare: %s %s"), *Code, *Error),
            bMissingCompared && bExtraCompared))
    {
        return false;
    }

    // "Near" rather than "exactly": the reported frequency is the tracker's estimate of the
    // dropped mode, and the match tolerance is what defines whether it would have paired.
    const double NearHz = DroppedHz * 0.06;   // the matcher's own ~100 cent window in hertz

    bool bFoundMissing = false;
    for (const FPwCompareUnmatchedMode& Mode : Missing.MissingModes)
    {
        bFoundMissing |= FMath::Abs(Mode.FreqHz - DroppedHz) <= NearHz;
        TestEqual(TEXT("every entry on the missing list is diagnosed missing_mode"),
            Mode.Diagnosis, FString(PwCompareDiagnosis::MissingMode));
    }
    TestTrue(FString::Printf(
        TEXT("a reference mode absent from the candidate is reported missing near %.0f Hz ")
        TEXT("(%d missing, %d extra)"),
        DroppedHz, Missing.MissingModes.Num(), Missing.ExtraModes.Num()),
        bFoundMissing);
    TestTrue(TEXT("the missing case emits a missing_mode deviation"),
        HasDiagnosis(Missing, PwCompareDiagnosis::MissingMode));

    bool bFoundExtra = false;
    for (const FPwCompareUnmatchedMode& Mode : Extra.ExtraModes)
    {
        bFoundExtra |= FMath::Abs(Mode.FreqHz - DroppedHz) <= NearHz;
        TestEqual(TEXT("every entry on the extra list is diagnosed extra_mode"),
            Mode.Diagnosis, FString(PwCompareDiagnosis::ExtraMode));
    }
    TestTrue(FString::Printf(
        TEXT("a candidate mode absent from the reference is reported extra near %.0f Hz ")
        TEXT("(%d missing, %d extra)"),
        DroppedHz, Extra.MissingModes.Num(), Extra.ExtraModes.Num()),
        bFoundExtra);
    TestTrue(TEXT("the extra case emits an extra_mode deviation"),
        HasDiagnosis(Extra, PwCompareDiagnosis::ExtraMode));

    // The mode that IS shared must pair rather than appear on both leftover lists - otherwise
    // "missing plus extra" would be how this matcher reports every mistuning.
    bool bPairedShared = false;
    for (const FPwCompareModePair& Pair : Missing.ModePairs)
    {
        bPairedShared |= FMath::Abs(Pair.ReferenceFreqHz - 400.0) <= 400.0 * 0.06;
    }
    TestTrue(TEXT("the shared 400 Hz mode paired instead of landing on both leftover lists"),
        bPairedShared);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCompareRejectsDegenerateInputTest,
    "PinWright.audio.compare.RejectsDegenerateInputInOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCompareRejectsDegenerateInputTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioCompareTestHelpers;

    FString Error;
    FPwAudioBuffer Good;
    const bool bRendered =
        RenderJson(ModalRecipeJson(FixtureFreqs, FixtureGains, FixtureDecays), Good, Error);
    if (!TestTrue(FString::Printf(TEXT("the fixture renders: %s"), *Error), bRendered))
    {
        return false;
    }

    const int32 Frames = Good.NumFrames();
    const FPwAudioBuffer Empty;
    const FPwAudioBuffer Silent = ConstantBuffer(Frames, 0.f);
    const FPwAudioBuffer NotFinite = ConstantBuffer(Frames, MakeNaN());
    const FPwAudioBuffer WrongRate = ConstantBuffer(Frames, 0.25f, 44100);

    struct FCase
    {
        const TCHAR* Name;
        const FPwAudioBuffer* Reference;
        const FPwAudioBuffer* Candidate;
        const TCHAR* ExpectedCode;
    };

    const FCase Cases[] = {
        { TEXT("empty reference"),  &Empty,     &Good,      ErrorCodes::ERR_AUDIO_EMPTY_BUFFER },
        { TEXT("empty candidate"),  &Good,      &Empty,     ErrorCodes::ERR_AUDIO_EMPTY_BUFFER },
        // THE ORDERING ASSERTION. A NaN-filled buffer scanned for its peak first measures as
        // digital silence, because NaN compares false against every threshold - so this row
        // comes back AUDIO_EMPTY_BUFFER the moment the non-finite check stops running first,
        // and the caller is sent to look at gain staging instead of at the generator.
        { TEXT("non-finite reference"), &NotFinite, &Good,  ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES },
        { TEXT("non-finite candidate"), &Good, &NotFinite,  ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES },
        { TEXT("silent candidate"), &Good,      &Silent,    ErrorCodes::ERR_AUDIO_EMPTY_BUFFER },
        { TEXT("silent reference"), &Silent,    &Good,      ErrorCodes::ERR_AUDIO_EMPTY_BUFFER },
        { TEXT("sample-rate mismatch"), &Good,  &WrongRate, ErrorCodes::ERR_INVALID_PARAMS }
    };

    for (const FCase& Case : Cases)
    {
        FPwCompareResult Result;
        // Pre-populate, so a path that forgets to clear Out is caught rather than hidden.
        Result.bMeasured = true;
        Result.SampleRate = 12345;

        FString Code;
        FString Message;
        const bool bResult = PwCompareAudio(*Case.Reference, *Case.Candidate, Result, Code, Message);

        TestFalse(FString::Printf(TEXT("%s is refused"), Case.Name), bResult);
        TestEqual(FString::Printf(TEXT("%s reports the right code"), Case.Name),
            Code, FString(Case.ExpectedCode));
        TestTrue(FString::Printf(TEXT("%s explains itself"), Case.Name), !Message.IsEmpty());
        TestFalse(FString::Printf(TEXT("%s leaves the report unmeasured"), Case.Name),
            Result.bMeasured);
        TestFalse(FString::Printf(TEXT("%s reports a match"), Case.Name), Result.IsMatch());
        TestTrue(FString::Printf(TEXT("%s says why the report is absent"), Case.Name),
            !Result.UnmeasuredReason.IsEmpty());
        TestTrue(FString::Printf(TEXT("%s emits no deviations"), Case.Name),
            Result.Deviations.Num() == 0);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCompareSerializesAbsenceExplicitlyTest,
    "PinWright.audio.compare.SerializationExplainsEveryAbsence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCompareSerializesAbsenceExplicitlyTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioCompareTestHelpers;

    // An unmeasured report must serialize to its reason and NOTHING else - a wall of plausible
    // zeros beside `measured: false` is the §1 defect this check exists to catch.
    {
        FPwCompareResult Unmeasured;
        Unmeasured.UnmeasuredReason = TEXT("the candidate is digitally silent");
        const TSharedPtr<FJsonObject> Json = SerializeCompareResult(Unmeasured);
        if (TestNotNull(TEXT("an unmeasured report serializes"), Json.Get()))
        {
            bool bMeasured = true;
            TestTrue(TEXT("it carries `measured`"), Json->TryGetBoolField(TEXT("measured"), bMeasured));
            TestFalse(TEXT("`measured` is false"), bMeasured);
            TestTrue(TEXT("it carries the reason"), Json->HasField(TEXT("unmeasuredReason")));
            TestFalse(TEXT("it publishes no scalars"), Json->HasField(TEXT("scalars")));
            TestFalse(TEXT("it publishes no modes"), Json->HasField(TEXT("modes")));
            TestFalse(TEXT("it publishes no deviations"), Json->HasField(TEXT("deviations")));
        }
    }

    FString Error;
    FPwAudioBuffer Audio;
    const bool bRendered =
        RenderJson(ModalRecipeJson(FixtureFreqs, FixtureGains, FixtureDecays), Audio, Error);
    if (!TestTrue(FString::Printf(TEXT("the fixture renders: %s"), *Error), bRendered))
    {
        return false;
    }

    FPwCompareResult Result;
    FString Code;
    Error.Reset();
    const bool bCompared = PwCompareAudio(Audio, Audio, Result, Code, Error);
    if (!TestTrue(FString::Printf(TEXT("the comparison runs: %s %s"), *Code, *Error), bCompared))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Json = SerializeCompareResult(Result);
    if (!TestNotNull(TEXT("a measured report serializes"), Json.Get()))
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* Modes = nullptr;
    if (TestTrue(TEXT("the report carries a modes block"),
            Json->TryGetObjectField(TEXT("modes"), Modes)))
    {
        // Emitted even when empty: "nothing is missing" is a measurement, and an omitted array is
        // indistinguishable from a matcher that only looked one way.
        TestTrue(TEXT("`missing` is emitted even when empty"), (*Modes)->HasField(TEXT("missing")));
        TestTrue(TEXT("`extra` is emitted even when empty"), (*Modes)->HasField(TEXT("extra")));
    }

    TestTrue(TEXT("the report carries a deviations array"), Json->HasField(TEXT("deviations")));
    TestTrue(TEXT("the report carries the derived verdict"), Json->HasField(TEXT("match")));

    // Every scalar is either published under `scalars` or explained under `unmeasured`. Never
    // both, and never neither.
    const TSharedPtr<FJsonObject>* Scalars = nullptr;
    const TSharedPtr<FJsonObject>* Unmeasured = nullptr;
    Json->TryGetObjectField(TEXT("scalars"), Scalars);
    Json->TryGetObjectField(TEXT("unmeasured"), Unmeasured);
    const TCHAR* Names[] = { TEXT("onsetMs"), TEXT("attackMs"), TEXT("decayMs"), TEXT("tailMs"),
                             TEXT("loudnessLufs"), TEXT("centroidHz") };
    for (const TCHAR* Name : Names)
    {
        const bool bPublished = Scalars != nullptr && (*Scalars)->HasField(Name);
        const bool bExplained = Unmeasured != nullptr && (*Unmeasured)->HasField(Name);
        TestTrue(FString::Printf(TEXT("%s is either published or explained, never both"), Name),
            bPublished != bExplained);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCompareVocabularyIsClosedTest,
    "PinWright.audio.compare.DiagnosisVocabularyIsClosedAndPaired",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCompareVocabularyIsClosedTest::RunTest(const FString& Parameters)
{
    const TArray<FString>& Vocabulary = PwCompareDiagnosisVocabulary();

    TestTrue(TEXT("the vocabulary is not empty"), Vocabulary.Num() > 0);
    TestFalse(TEXT("a token outside the vocabulary is rejected"),
        PwIsKnownCompareDiagnosis(TEXT("the attack is a bit off")));
    TestFalse(TEXT("an empty diagnosis is rejected"), PwIsKnownCompareDiagnosis(FString()));

    TSet<FString> Seen;
    for (const FString& Token : Vocabulary)
    {
        TestTrue(FString::Printf(TEXT("'%s' is unique in the vocabulary"), *Token),
            !Seen.Contains(Token));
        Seen.Add(Token);
        TestTrue(FString::Printf(TEXT("'%s' passes its own membership test"), *Token),
            PwIsKnownCompareDiagnosis(Token));
    }

    // Every deviation the report can name has an opposite. A vocabulary with an odd number of
    // tokens would mean some fault is reported as a magnitude with no direction (rpc-design.md
    // §6) - which is the whole thing this file exists to avoid.
    TestTrue(FString::Printf(TEXT("the vocabulary is paired (%d tokens)"), Vocabulary.Num()),
        (Vocabulary.Num() % 2) == 0);

    return true;
}
