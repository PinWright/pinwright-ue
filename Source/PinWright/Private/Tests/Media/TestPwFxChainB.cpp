// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for FX chain B (AudioGen/PwFxChainB.cpp): chorus, flanger, phaser, ringmod,
// pitchshift, compressor, eq, gain, width, reverse, convolve.
//
// The assertions here are numeric wherever the effect has ground truth - a pitch shift
// has a measurable output frequency, a ring modulator has known sidebands, an EQ band
// has a known centre - and the measurement is the STFT front-end (AudioGen/PwStft.h),
// whose scaling convention is documented and independently tested by TestPwStft.cpp.
//
// Per rpc-design.md §6, every measured effect is checked in BOTH directions, because a
// one-sided check passes a broken implementation:
//   * pitchshift is asserted up AND down - a shifter stuck on "double" passes an
//     up-only test.
//   * eq is asserted boosted AND cut, and the untouched band is asserted unchanged - a
//     filter that just adds broadband gain passes a boost-only test.
//   * the compressor is asserted to squash a loud signal AND to leave a below-threshold
//     signal alone - the second half is what catches a "compressor" that only attenuates.
//   * width is asserted at 0, 1 and >1 - collapsing, identity, and widening.
//
// Per rpc-design.md §12 the failure direction is asserted with the SPECIFIC error code,
// and every rejection additionally asserts that a PRE-FILLED span came back bit-identical
// sample by sample: a half-processed buffer would silently corrupt the rest of the chain,
// and that corruption is invisible from the return value alone.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwSeededRandom.h"
#include "AudioGen/PwStft.h"
#include "AudioGen/PwSynthDsp.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Handlers/ErrorCodes.h"

#include "Audio.h"
#include "Memory/SharedBuffer.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true and
// Tests/Media/ already carries anonymous-namespace helpers with colliding names.
// See CLAUDE.md > Building.
namespace PwFxChainBTest
{
    constexpr int32 TestSampleRate = 48000;
    constexpr int32 TestSeed = 20260817;

    // ------------------------------------------------------------------
    // Parameter bags
    // ------------------------------------------------------------------

    void SetNumber(FPwSynthParams& Params, const TCHAR* Key, double Value)
    {
        FPwSynthParamValue Entry;
        Entry.Type = EPwSynthParamType::Number;
        Entry.Number = Value;
        Params.Values.Add(FName(Key), Entry);
    }

    void SetBool(FPwSynthParams& Params, const TCHAR* Key, bool bValue)
    {
        FPwSynthParamValue Entry;
        Entry.Type = EPwSynthParamType::Boolean;
        Entry.bBool = bValue;
        Entry.Number = bValue ? 1.0 : 0.0;
        Params.Values.Add(FName(Key), Entry);
    }

    void SetString(FPwSynthParams& Params, const TCHAR* Key, const FString& Value)
    {
        FPwSynthParamValue Entry;
        Entry.Type = EPwSynthParamType::String;
        Entry.String = Value;
        Params.Values.Add(FName(Key), Entry);
    }

    FPwSynthParams ChorusParams(double Mix = 1.0, int32 Voices = 3)
    {
        FPwSynthParams Params;
        SetNumber(Params, TEXT("rateHz"), 1.5);
        SetNumber(Params, TEXT("depthMs"), 8.0);
        SetNumber(Params, TEXT("mix"), Mix);
        SetNumber(Params, TEXT("voices"), Voices);
        return Params;
    }

    FPwSynthParams FlangerParams(double Mix = 1.0, double Feedback = 0.4)
    {
        FPwSynthParams Params;
        SetNumber(Params, TEXT("rateHz"), 0.5);
        SetNumber(Params, TEXT("depthMs"), 3.0);
        SetNumber(Params, TEXT("mix"), Mix);
        SetNumber(Params, TEXT("feedback"), Feedback);
        return Params;
    }

    FPwSynthParams PhaserParams(double Mix = 1.0)
    {
        FPwSynthParams Params;
        SetNumber(Params, TEXT("rateHz"), 0.8);
        SetNumber(Params, TEXT("depth"), 0.7);
        SetNumber(Params, TEXT("mix"), Mix);
        SetNumber(Params, TEXT("stages"), 6);
        SetNumber(Params, TEXT("feedback"), -0.3);
        return Params;
    }

    FPwSynthParams RingmodParams(double Mix = 1.0, double RateHz = 250.0,
        const TCHAR* Waveform = TEXT("sine"))
    {
        FPwSynthParams Params;
        SetNumber(Params, TEXT("rateHz"), RateHz);
        SetNumber(Params, TEXT("mix"), Mix);
        SetString(Params, TEXT("waveform"), Waveform);
        return Params;
    }

    FPwSynthParams PitchshiftParams(double Semitones, double Mix = 1.0, bool bFormant = false)
    {
        FPwSynthParams Params;
        SetNumber(Params, TEXT("semitones"), Semitones);
        SetNumber(Params, TEXT("mix"), Mix);
        SetBool(Params, TEXT("formantPreserve"), bFormant);
        return Params;
    }

    FPwSynthParams CompressorParams(double ThresholdDb = -20.0, double Ratio = 8.0)
    {
        FPwSynthParams Params;
        SetNumber(Params, TEXT("thresholdDb"), ThresholdDb);
        SetNumber(Params, TEXT("ratio"), Ratio);
        SetNumber(Params, TEXT("attackMs"), 1.0);
        SetNumber(Params, TEXT("releaseMs"), 50.0);
        SetNumber(Params, TEXT("kneeDb"), 0.0);
        SetNumber(Params, TEXT("makeupDb"), 0.0);
        return Params;
    }

    FPwSynthParams EqParams(double MidGainDb)
    {
        FPwSynthParams Params;
        SetNumber(Params, TEXT("lowGainDb"), 0.0);
        SetNumber(Params, TEXT("midGainDb"), MidGainDb);
        SetNumber(Params, TEXT("highGainDb"), 0.0);
        SetNumber(Params, TEXT("lowHz"), 200.0);
        SetNumber(Params, TEXT("midHz"), 1000.0);
        SetNumber(Params, TEXT("midQ"), 2.0);
        SetNumber(Params, TEXT("highHz"), 4000.0);
        return Params;
    }

    FPwSynthParams GainParams(double GainDb)
    {
        FPwSynthParams Params;
        SetNumber(Params, TEXT("gainDb"), GainDb);
        return Params;
    }

    FPwSynthParams WidthParams(double Width)
    {
        FPwSynthParams Params;
        SetNumber(Params, TEXT("width"), Width);
        return Params;
    }

    FPwSynthParams ConvolveParams(const FString& ImpulsePath, double Mix = 1.0, bool bNormalize = true)
    {
        FPwSynthParams Params;
        SetString(Params, TEXT("impulsePath"), ImpulsePath);
        SetNumber(Params, TEXT("mix"), Mix);
        SetBool(Params, TEXT("normalize"), bNormalize);
        return Params;
    }

    // ------------------------------------------------------------------
    // Signals and spans
    // ------------------------------------------------------------------

    TArray<float> MakeSine(int32 NumSamples, double Hz, double Amplitude, double PhaseTurns = 0.0)
    {
        TArray<float> Samples;
        Samples.SetNumUninitialized(NumSamples);
        const double Step = 2.0 * UE_DOUBLE_PI * Hz / static_cast<double>(TestSampleRate);
        const double Phase = 2.0 * UE_DOUBLE_PI * PhaseTurns;
        for (int32 Index = 0; Index < NumSamples; ++Index)
        {
            Samples[Index] = static_cast<float>(Amplitude * FMath::Sin(Phase + Step * Index));
        }
        return Samples;
    }

    /** Deterministic non-repeating content: a chord no effect can accidentally reproduce. */
    TArray<float> MakeChord(int32 NumSamples)
    {
        TArray<float> Samples = MakeSine(NumSamples, 220.0, 0.30);
        const TArray<float> Second = MakeSine(NumSamples, 523.0, 0.20, 0.13);
        const TArray<float> Third = MakeSine(NumSamples, 1310.0, 0.12, 0.37);
        for (int32 Index = 0; Index < NumSamples; ++Index)
        {
            Samples[Index] += Second[Index] + Third[Index];
        }
        return Samples;
    }

    FPwDspSpan MonoSpan(TArray<float>& Samples)
    {
        FPwDspSpan Span;
        Span.Left = Samples.GetData();
        Span.Right = nullptr;
        Span.NumFrames = Samples.Num();
        return Span;
    }

    FPwDspSpan StereoSpan(TArray<float>& Left, TArray<float>& Right)
    {
        FPwDspSpan Span;
        Span.Left = Left.GetData();
        Span.Right = Right.GetData();
        Span.NumFrames = FMath::Min(Left.Num(), Right.Num());
        return Span;
    }

    bool BitIdentical(const TArray<float>& A, const TArray<float>& B)
    {
        return A.Num() == B.Num()
            && FMemory::Memcmp(A.GetData(), B.GetData(), static_cast<SIZE_T>(A.Num()) * sizeof(float)) == 0;
    }

    /** Index of the first differing sample, or INDEX_NONE. Reported so a failure names where. */
    int32 FirstDifference(const TArray<float>& A, const TArray<float>& B)
    {
        const int32 Count = FMath::Min(A.Num(), B.Num());
        for (int32 Index = 0; Index < Count; ++Index)
        {
            if (A[Index] != B[Index])
            {
                return Index;
            }
        }
        return (A.Num() == B.Num()) ? INDEX_NONE : Count;
    }

    bool AllFinite(const TArray<float>& Samples)
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

    double PeakAbs(const TArray<float>& Samples)
    {
        double Peak = 0.0;
        for (const float Sample : Samples)
        {
            Peak = FMath::Max(Peak, static_cast<double>(FMath::Abs(Sample)));
        }
        return Peak;
    }

    double RmsOverRange(const TArray<float>& Samples, int32 First, int32 Last)
    {
        First = FMath::Max(0, First);
        Last = FMath::Min(Samples.Num() - 1, Last);
        if (Last < First)
        {
            return 0.0;
        }
        double Sum = 0.0;
        for (int32 Index = First; Index <= Last; ++Index)
        {
            Sum += static_cast<double>(Samples[Index]) * Samples[Index];
        }
        return FMath::Sqrt(Sum / static_cast<double>(Last - First + 1));
    }

    double MaxAbsDifference(const TArray<float>& A, const TArray<float>& B)
    {
        double Worst = 0.0;
        const int32 Count = FMath::Min(A.Num(), B.Num());
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Worst = FMath::Max(Worst, static_cast<double>(FMath::Abs(A[Index] - B[Index])));
        }
        return Worst;
    }

    double MeanAbsDifference(const TArray<float>& A, const TArray<float>& B)
    {
        const int32 Count = FMath::Min(A.Num(), B.Num());
        if (Count == 0)
        {
            return 0.0;
        }
        double Sum = 0.0;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Sum += FMath::Abs(static_cast<double>(A[Index]) - B[Index]);
        }
        return Sum / static_cast<double>(Count);
    }

    // ------------------------------------------------------------------
    // Spectral ground truth (AudioGen/PwStft.h)
    // ------------------------------------------------------------------

    bool Analyze(const TArray<float>& Mono, FPwStftResult& Out)
    {
        FPwStftSettings Settings;
        Settings.FftSize = 4096;    // 11.7 Hz bins at 48 kHz
        Settings.HopSize = 2048;
        return PwComputeStft(Mono, TestSampleRate, Settings, Out);
    }

    /** Frequency of the loudest bin, summed over every analysis frame. -1 when unanalyzable. */
    double DominantHz(const TArray<float>& Mono)
    {
        FPwStftResult Result;
        if (!Analyze(Mono, Result))
        {
            return -1.0;
        }
        TArray<double> BinTotals;
        BinTotals.SetNumZeroed(Result.NumBins);
        for (int32 Frame = 0; Frame < Result.NumFrames; ++Frame)
        {
            for (int32 Bin = 0; Bin < Result.NumBins; ++Bin)
            {
                BinTotals[Bin] += PwStftMagnitudeAt(Result, Frame, Bin);
            }
        }
        int32 BestBin = 0;
        for (int32 Bin = 1; Bin < BinTotals.Num(); ++Bin)
        {
            if (BinTotals[Bin] > BinTotals[BestBin])
            {
                BestBin = Bin;
            }
        }
        return BestBin * Result.BinHz;
    }

    /** Mean per-frame magnitude summed across the bins inside CenterHz +/- HalfWidthHz. */
    double BandMagnitude(const TArray<float>& Mono, double CenterHz, double HalfWidthHz)
    {
        FPwStftResult Result;
        if (!Analyze(Mono, Result) || Result.BinHz <= 0.f)
        {
            return -1.0;
        }
        // The *ToInt32 spellings, not FloorToInt / CeilToInt: their double overloads return
        // int64, which would make the surrounding FMath::Min / Max templates fail to deduce.
        const int32 LowBin = FMath::Max(0, FMath::FloorToInt32((CenterHz - HalfWidthHz) / Result.BinHz));
        const int32 HighBin = FMath::Min(Result.NumBins - 1,
            FMath::CeilToInt32((CenterHz + HalfWidthHz) / Result.BinHz));
        double Total = 0.0;
        for (int32 Frame = 0; Frame < Result.NumFrames; ++Frame)
        {
            for (int32 Bin = LowBin; Bin <= HighBin; ++Bin)
            {
                Total += PwStftMagnitudeAt(Result, Frame, Bin);
            }
        }
        return Total / FMath::Max(1, Result.NumFrames);
    }

    // ------------------------------------------------------------------
    // Convolution impulse-response fixture
    //
    // A transient USoundWave carrying a real imported PCM payload, referenced by its
    // Wave->GetPathName() (/Engine/Transient.<name>), so the effect resolves it through
    // the production PwResolveSourceBuffer path rather than a test-only seam. Fixture
    // shape follows TestPwGenSampleGranular.cpp, which owns that resolver.
    // Caller owns RemoveFromRoot.
    // ------------------------------------------------------------------

    USoundWave* MakeImpulseFixtureWave(const TArray<int16>& MonoPcm, int32 WaveSampleRate)
    {
        const FString Name = FString::Printf(TEXT("PwFxImpulseFixture_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        USoundWave* Wave = NewObject<USoundWave>(GetTransientPackage(), FName(*Name), RF_Transient);
        if (!Wave)
        {
            return nullptr;
        }
        Wave->AddToRoot();
        // GetImportedSoundWaveData asserts check(NumChannels > 0) against this UPROPERTY.
        Wave->NumChannels = 1;

        TArray<uint8> WavBytes;
        SerializeWaveFile(WavBytes,
            reinterpret_cast<const uint8*>(MonoPcm.GetData()),
            MonoPcm.Num() * static_cast<int32>(sizeof(int16)),
            /*NumChannels=*/1, WaveSampleRate);
        Wave->RawData.UpdatePayload(FSharedBuffer::Clone(WavBytes.GetData(), WavBytes.Num()));
        return Wave;
    }

    TArray<int16> UnitImpulsePcm()
    {
        TArray<int16> Pcm;
        Pcm.SetNumZeroed(64);
        Pcm[0] = 32767;
        return Pcm;
    }

    /** A flat rectangular impulse response, so its tail has an unambiguous end. */
    TArray<int16> RectangularImpulsePcm(int32 NumFrames)
    {
        TArray<int16> Pcm;
        Pcm.SetNumUninitialized(NumFrames);
        for (int32 Index = 0; Index < NumFrames; ++Index)
        {
            Pcm[Index] = 16384;     // exactly 0.5 once decoded against the -32768 rail
        }
        return Pcm;
    }

    /** Index of the last sample whose magnitude exceeds Threshold, or INDEX_NONE. */
    int32 LastSampleAbove(const TArray<float>& Samples, double Threshold)
    {
        for (int32 Index = Samples.Num() - 1; Index >= 0; --Index)
        {
            if (FMath::Abs(Samples[Index]) > Threshold)
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

    // ------------------------------------------------------------------
    // Table used by the cross-effect mono/stereo/determinism sweep
    // ------------------------------------------------------------------

    using FFxEntryPoint = bool (*)(const FPwSynthParams&, int32, FPwSeededRandom&,
        const FPwDspSpan&, FString&, FString&);

    struct FFxCase
    {
        const TCHAR* Name;
        FFxEntryPoint Function;
        FPwSynthParams Params;
    };

    /** Every effect in chain B that accepts a mono span (i.e. everything but width). */
    TArray<FFxCase> MonoCapableCases()
    {
        return {
            { TEXT("chorus"),     &PwFxChorus,     ChorusParams() },
            { TEXT("flanger"),    &PwFxFlanger,    FlangerParams() },
            { TEXT("phaser"),     &PwFxPhaser,     PhaserParams() },
            { TEXT("ringmod"),    &PwFxRingmod,    RingmodParams() },
            { TEXT("pitchshift"), &PwFxPitchshift, PitchshiftParams(7.0) },
            { TEXT("compressor"), &PwFxCompressor, CompressorParams() },
            { TEXT("eq"),         &PwFxEq,         EqParams(6.0) },
            { TEXT("gain"),       &PwFxGain,       GainParams(-3.0) },
            { TEXT("reverse"),    &PwFxReverse,    FPwSynthParams() }
        };
    }
}

// =========================================================================
// A. Cross-effect sweep: every effect that takes a mono span runs on mono AND
//    stereo, produces finite samples, and is byte-identical across two runs
//    with the same params and seed.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBMonoStereoDeterminismTest,
    "PinWright.audio.fx.chain.MonoAndStereoAreDeterministic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBMonoStereoDeterminismTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    constexpr int32 NumFrames = TestSampleRate / 4;     // 250 ms
    const TArray<float> SourceLeft = MakeChord(NumFrames);
    const TArray<float> SourceRight = MakeSine(NumFrames, 330.0, 0.35, 0.21);

    for (const FFxCase& Case : MonoCapableCases())
    {
        // --- mono, run twice ---
        TArray<float> MonoA = SourceLeft;
        TArray<float> MonoB = SourceLeft;
        FPwSeededRandom RngA(TestSeed);
        FPwSeededRandom RngB(TestSeed);
        FString CodeA, ErrorA, CodeB, ErrorB;

        FPwDspSpan SpanA = MonoSpan(MonoA);
        FPwDspSpan SpanB = MonoSpan(MonoB);
        const bool bOkA = Case.Function(Case.Params, TestSampleRate, RngA, SpanA, CodeA, ErrorA);
        const bool bOkB = Case.Function(Case.Params, TestSampleRate, RngB, SpanB, CodeB, ErrorB);

        TestTrue(FString::Printf(TEXT("%s succeeds on a mono span (%s %s)"), Case.Name, *CodeA, *ErrorA), bOkA);
        TestTrue(FString::Printf(TEXT("%s succeeds on a mono span, second run"), Case.Name), bOkB);
        TestTrue(FString::Printf(TEXT("%s mono output is finite"), Case.Name), AllFinite(MonoA));
        TestTrue(FString::Printf(TEXT("%s mono output is byte-identical across two identically seeded runs"),
            Case.Name), BitIdentical(MonoA, MonoB));

        // --- stereo, run twice ---
        TArray<float> LeftA = SourceLeft;
        TArray<float> RightA = SourceRight;
        TArray<float> LeftB = SourceLeft;
        TArray<float> RightB = SourceRight;
        FPwSeededRandom RngC(TestSeed);
        FPwSeededRandom RngD(TestSeed);
        FString CodeC, ErrorC, CodeD, ErrorD;

        FPwDspSpan SpanC = StereoSpan(LeftA, RightA);
        FPwDspSpan SpanD = StereoSpan(LeftB, RightB);
        const bool bOkC = Case.Function(Case.Params, TestSampleRate, RngC, SpanC, CodeC, ErrorC);
        const bool bOkD = Case.Function(Case.Params, TestSampleRate, RngD, SpanD, CodeD, ErrorD);

        TestTrue(FString::Printf(TEXT("%s succeeds on a stereo span (%s %s)"), Case.Name, *CodeC, *ErrorC), bOkC);
        TestTrue(FString::Printf(TEXT("%s succeeds on a stereo span, second run"), Case.Name), bOkD);
        TestTrue(FString::Printf(TEXT("%s stereo left is finite"), Case.Name), AllFinite(LeftA));
        TestTrue(FString::Printf(TEXT("%s stereo right is finite"), Case.Name), AllFinite(RightA));
        TestTrue(FString::Printf(TEXT("%s stereo left is byte-identical across two runs"), Case.Name),
            BitIdentical(LeftA, LeftB));
        TestTrue(FString::Printf(TEXT("%s stereo right is byte-identical across two runs"), Case.Name),
            BitIdentical(RightA, RightB));
    }

    return true;
}

// =========================================================================
// B. mix == 0 is bit-identical for every effect that carries a mix row.
//    Asserted sample by sample, not "close enough": the contract is that a
//    fully dry effect never touches the span at all.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBMixZeroIsBitIdenticalTest,
    "PinWright.audio.fx.chain.MixZeroIsBitIdentical",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBMixZeroIsBitIdenticalTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    constexpr int32 NumFrames = TestSampleRate / 8;
    const TArray<float> Source = MakeChord(NumFrames);

    struct FDryCase
    {
        const TCHAR* Name;
        FFxEntryPoint Function;
        FPwSynthParams Params;
    };

    const TArray<FDryCase> Cases = {
        { TEXT("chorus"),     &PwFxChorus,     ChorusParams(/*Mix=*/0.0) },
        { TEXT("flanger"),    &PwFxFlanger,    FlangerParams(/*Mix=*/0.0) },
        { TEXT("phaser"),     &PwFxPhaser,     PhaserParams(/*Mix=*/0.0) },
        { TEXT("ringmod"),    &PwFxRingmod,    RingmodParams(/*Mix=*/0.0) },
        { TEXT("pitchshift"), &PwFxPitchshift, PitchshiftParams(12.0, /*Mix=*/0.0) }
    };

    for (const FDryCase& Case : Cases)
    {
        TArray<float> Samples = Source;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = MonoSpan(Samples);

        TestTrue(FString::Printf(TEXT("%s at mix 0 succeeds"), Case.Name),
            Case.Function(Case.Params, TestSampleRate, Rng, Span, Code, Error));

        const int32 Difference = FirstDifference(Source, Samples);
        TestEqual(FString::Printf(TEXT("%s at mix 0 leaves the span bit-identical (first differing sample)"),
            Case.Name), Difference, INDEX_NONE);
    }

    return true;
}

// =========================================================================
// C. chorus / flanger / phaser: the output differs from the input and stays finite.
//    "Differs" is the load-bearing half - a modulation effect that silently
//    no-oped would pass every finiteness and determinism check above.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBModulationEffectsAlterSignalTest,
    "PinWright.audio.fx.chain.ModulationEffectsAlterTheSignal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBModulationEffectsAlterSignalTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    constexpr int32 NumFrames = TestSampleRate / 2;
    const TArray<float> Source = MakeChord(NumFrames);
    const double SourcePeak = PeakAbs(Source);

    struct FModCase
    {
        const TCHAR* Name;
        FFxEntryPoint Function;
        FPwSynthParams Params;
    };

    const TArray<FModCase> Cases = {
        { TEXT("chorus"),  &PwFxChorus,  ChorusParams(1.0, /*Voices=*/4) },
        { TEXT("flanger"), &PwFxFlanger, FlangerParams(1.0, /*Feedback=*/0.6) },
        { TEXT("phaser"),  &PwFxPhaser,  PhaserParams() }
    };

    for (const FModCase& Case : Cases)
    {
        TArray<float> Samples = Source;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = MonoSpan(Samples);

        TestTrue(FString::Printf(TEXT("%s succeeds (%s %s)"), Case.Name, *Code, *Error),
            Case.Function(Case.Params, TestSampleRate, Rng, Span, Code, Error));
        TestTrue(FString::Printf(TEXT("%s output is finite"), Case.Name), AllFinite(Samples));
        TestTrue(FString::Printf(TEXT("%s changed the signal"), Case.Name),
            FirstDifference(Source, Samples) != INDEX_NONE);

        // A change that is only in the last few samples would satisfy FirstDifference; the
        // mean difference proves the modulation ran across the whole span.
        const double MeanDelta = MeanAbsDifference(Source, Samples);
        TestTrue(FString::Printf(TEXT("%s changed the signal audibly (mean |delta| %g against source peak %g)"),
            Case.Name, MeanDelta, SourcePeak), MeanDelta > 0.001);
    }

    return true;
}

// =========================================================================
// D. pitchshift: BOTH directions. A shifter stuck on "double" passes an up-only test.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBPitchShiftUpAndDownTest,
    "PinWright.audio.fx.pitchshift.ShiftsUpAndDown",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBPitchShiftUpAndDownTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    constexpr int32 NumFrames = TestSampleRate;         // 1 s
    constexpr double SourceHz = 500.0;
    constexpr double ToleranceHz = 30.0;                // ~2.5 bins at FftSize 4096

    const TArray<float> Source = MakeSine(NumFrames, SourceHz, 0.5);

    // Control: the measurement itself must find the unshifted tone, or the two
    // assertions below prove nothing.
    TestTrue(TEXT("the source tone measures at 500 Hz before any shift"),
        FMath::Abs(DominantHz(Source) - SourceHz) < ToleranceHz);

    TArray<float> Up = Source;
    FPwSeededRandom RngUp(TestSeed);
    FString UpCode, UpError;
    FPwDspSpan UpSpan = MonoSpan(Up);
    TestTrue(FString::Printf(TEXT("+12 semitone shift succeeds (%s %s)"), *UpCode, *UpError),
        PwFxPitchshift(PitchshiftParams(12.0), TestSampleRate, RngUp, UpSpan, UpCode, UpError));

    TArray<float> Down = Source;
    FPwSeededRandom RngDown(TestSeed);
    FString DownCode, DownError;
    FPwDspSpan DownSpan = MonoSpan(Down);
    TestTrue(FString::Printf(TEXT("-12 semitone shift succeeds (%s %s)"), *DownCode, *DownError),
        PwFxPitchshift(PitchshiftParams(-12.0), TestSampleRate, RngDown, DownSpan, DownCode, DownError));

    const double UpHz = DominantHz(Up);
    const double DownHz = DominantHz(Down);

    TestTrue(FString::Printf(TEXT("+12 semitones on 500 Hz measures near 1000 Hz (measured %g Hz)"), UpHz),
        FMath::Abs(UpHz - 1000.0) < ToleranceHz);
    TestTrue(FString::Printf(TEXT("-12 semitones on 500 Hz measures near 250 Hz (measured %g Hz)"), DownHz),
        FMath::Abs(DownHz - 250.0) < ToleranceHz);
    TestTrue(TEXT("the two shifts moved the tone in opposite directions"), UpHz > SourceHz && DownHz < SourceHz);
    TestTrue(TEXT("shifted output is finite"), AllFinite(Up) && AllFinite(Down));

    return true;
}

// =========================================================================
// E. pitchshift: formantPreserve is refused, not silently dropped, and the
//    pre-filled span comes back bit-identical.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBPitchShiftRejectsFormantPreserveTest,
    "PinWright.audio.fx.pitchshift.RejectsFormantPreserve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBPitchShiftRejectsFormantPreserveTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    const TArray<float> Source = MakeChord(TestSampleRate / 16);
    TArray<float> Samples = Source;
    FPwSeededRandom Rng(TestSeed);
    FString Code, Error;
    FPwDspSpan Span = MonoSpan(Samples);

    const bool bResult = PwFxPitchshift(PitchshiftParams(5.0, 1.0, /*bFormant=*/true),
        TestSampleRate, Rng, Span, Code, Error);

    TestFalse(TEXT("formantPreserve is refused rather than silently ignored"), bResult);
    TestEqual(TEXT("the code is UNSUPPORTED_OPTION"), Code, FString(ErrorCodes::ERR_UNSUPPORTED_OPTION));
    TestTrue(TEXT("the message names the parameter"), Error.Contains(TEXT("formantPreserve")));
    TestEqual(TEXT("a refused pitchshift leaves the span bit-identical (first differing sample)"),
        FirstDifference(Source, Samples), INDEX_NONE);

    return true;
}

// =========================================================================
// F. ringmod: sum and difference sidebands appear and the carrier tone does not.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBRingmodSidebandsTest,
    "PinWright.audio.fx.ringmod.ProducesSumAndDifference",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBRingmodSidebandsTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    constexpr int32 NumFrames = TestSampleRate;
    constexpr double SignalHz = 1000.0;
    constexpr double CarrierHz = 250.0;
    constexpr double HalfWidthHz = 30.0;

    const TArray<float> Source = MakeSine(NumFrames, SignalHz, 0.5);
    TArray<float> Samples = Source;
    FPwSeededRandom Rng(TestSeed);
    FString Code, Error;
    FPwDspSpan Span = MonoSpan(Samples);

    TestTrue(FString::Printf(TEXT("ringmod succeeds (%s %s)"), *Code, *Error),
        PwFxRingmod(RingmodParams(1.0, CarrierHz), TestSampleRate, Rng, Span, Code, Error));

    const double Difference = BandMagnitude(Samples, SignalHz - CarrierHz, HalfWidthHz);
    const double Sum = BandMagnitude(Samples, SignalHz + CarrierHz, HalfWidthHz);
    const double Carrier = BandMagnitude(Samples, SignalHz, HalfWidthHz);
    const double SourceCarrier = BandMagnitude(Source, SignalHz, HalfWidthHz);

    TestTrue(FString::Printf(TEXT("the 750 Hz difference tone is present (%g)"), Difference), Difference > 0.0);
    TestTrue(FString::Printf(TEXT("the 1250 Hz sum tone is present (%g)"), Sum), Sum > 0.0);
    // Both sidebands must dominate what is left at the original frequency. A modulator
    // that produced only one sideband, or that passed the input through, fails here.
    TestTrue(FString::Printf(TEXT("the difference tone dominates the original 1000 Hz (%g vs %g)"),
        Difference, Carrier), Difference > Carrier * 5.0);
    TestTrue(FString::Printf(TEXT("the sum tone dominates the original 1000 Hz (%g vs %g)"),
        Sum, Carrier), Sum > Carrier * 5.0);
    TestTrue(FString::Printf(TEXT("the original 1000 Hz tone was removed (%g down from %g)"),
        Carrier, SourceCarrier), Carrier < SourceCarrier * 0.2);
    TestTrue(TEXT("ringmod output is finite"), AllFinite(Samples));

    return true;
}

// =========================================================================
// G. ringmod failure directions: an unknown waveform names the valid set, and a
//    carrier outside the engine's window is refused instead of clamped.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBRingmodRejectionsTest,
    "PinWright.audio.fx.ringmod.RejectsUnknownWaveformAndUnsupportedCarrier",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBRingmodRejectionsTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    const TArray<float> Source = MakeChord(TestSampleRate / 16);

    {
        TArray<float> Samples = Source;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = MonoSpan(Samples);

        const bool bResult = PwFxRingmod(RingmodParams(1.0, 250.0, TEXT("supersaw")),
            TestSampleRate, Rng, Span, Code, Error);

        TestFalse(TEXT("an unrecognised waveform is an error, not a fallback to sine"), bResult);
        TestEqual(TEXT("the code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("the message names the offending value"), Error.Contains(TEXT("supersaw")));
        TestTrue(TEXT("the message names the whole valid set"),
            Error.Contains(TEXT("sine")) && Error.Contains(TEXT("triangle"))
            && Error.Contains(TEXT("saw")) && Error.Contains(TEXT("square")));
        TestEqual(TEXT("a refused ringmod leaves the span bit-identical (first differing sample)"),
            FirstDifference(Source, Samples), INDEX_NONE);
    }

    {
        // 2 Hz is inside the schema's rateHz row but below the carrier window
        // Audio::FRingModulation supports; it must be refused, never clamped to 10 Hz.
        TArray<float> Samples = Source;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = MonoSpan(Samples);

        const bool bResult = PwFxRingmod(RingmodParams(1.0, 2.0), TestSampleRate, Rng, Span, Code, Error);

        TestFalse(TEXT("a carrier below the engine's supported window is refused"), bResult);
        TestEqual(TEXT("the code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("the message names the supported carrier range"),
            Error.Contains(TEXT("10")) && Error.Contains(TEXT("10000")));
        TestEqual(TEXT("a refused ringmod leaves the span bit-identical (first differing sample)"),
            FirstDifference(Source, Samples), INDEX_NONE);
    }

    return true;
}

// =========================================================================
// H. compressor: a loud signal loses dynamic range AND a below-threshold signal
//    comes back essentially untouched. The second assertion is what catches a
//    "compressor" that simply attenuates everything.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBCompressorBothDirectionsTest,
    "PinWright.audio.fx.compressor.SquashesLoudAndSparesQuiet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBCompressorBothDirectionsTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    constexpr int32 HalfFrames = TestSampleRate / 2;
    constexpr int32 NumFrames = HalfFrames * 2;

    // A 200 Hz tone that is 0.9 for the first half and 0.07 for the second: the loud half
    // sits 19 dB above the -20 dB threshold and the quiet half sits 3 dB below it, far
    // enough that an envelope follower's overshoot cannot carry it over.
    TArray<float> Source = MakeSine(NumFrames, 200.0, 1.0);
    for (int32 Index = 0; Index < NumFrames; ++Index)
    {
        Source[Index] *= (Index < HalfFrames) ? 0.9f : 0.07f;
    }

    // Measured over the settled tail of each half, clear of the attack and release ramps.
    const int32 LoudFirst = HalfFrames * 3 / 5;
    const int32 LoudLast = HalfFrames - 1;
    const int32 QuietFirst = HalfFrames + HalfFrames * 3 / 5;
    const int32 QuietLast = NumFrames - 1;

    const double BeforeRatio = RmsOverRange(Source, LoudFirst, LoudLast)
        / FMath::Max(RmsOverRange(Source, QuietFirst, QuietLast), UE_DOUBLE_SMALL_NUMBER);

    TArray<float> Samples = Source;
    FPwSeededRandom Rng(TestSeed);
    FString Code, Error;
    FPwDspSpan Span = MonoSpan(Samples);
    TestTrue(FString::Printf(TEXT("compressor succeeds (%s %s)"), *Code, *Error),
        PwFxCompressor(CompressorParams(), TestSampleRate, Rng, Span, Code, Error));

    const double AfterLoudRms = RmsOverRange(Samples, LoudFirst, LoudLast);
    const double AfterQuietRms = RmsOverRange(Samples, QuietFirst, QuietLast);
    const double AfterRatio = AfterLoudRms / FMath::Max(AfterQuietRms, UE_DOUBLE_SMALL_NUMBER);

    TestTrue(FString::Printf(TEXT("the loud/quiet ratio shrank (%g -> %g)"), BeforeRatio, AfterRatio),
        AfterRatio < BeforeRatio * 0.5);
    TestTrue(FString::Printf(TEXT("the loud half was actually attenuated (%g -> %g rms)"),
        RmsOverRange(Source, LoudFirst, LoudLast), AfterLoudRms),
        AfterLoudRms < RmsOverRange(Source, LoudFirst, LoudLast) * 0.5);
    TestTrue(TEXT("compressor output is finite"), AllFinite(Samples));

    // The other direction: a signal entirely below the threshold must come back unchanged.
    // Its own control is that the threshold is the only thing that differs from the case above.
    const TArray<float> QuietSource = MakeSine(TestSampleRate / 2, 200.0, 0.05);
    TArray<float> QuietSamples = QuietSource;
    FPwSeededRandom QuietRng(TestSeed);
    FString QuietCode, QuietError;
    FPwDspSpan QuietSpan = MonoSpan(QuietSamples);
    TestTrue(FString::Printf(TEXT("compressor succeeds on a quiet signal (%s %s)"), *QuietCode, *QuietError),
        PwFxCompressor(CompressorParams(), TestSampleRate, QuietRng, QuietSpan, QuietCode, QuietError));

    const double QuietDelta = MaxAbsDifference(QuietSource, QuietSamples);
    TestTrue(FString::Printf(TEXT("a below-threshold signal is left essentially untouched (max |delta| %g)"),
        QuietDelta), QuietDelta < 1.e-6);

    return true;
}

// =========================================================================
// I. eq: a signed peak. +12 dB at 1 kHz raises 1 kHz and leaves 100 Hz alone;
//    -12 dB lowers it. Boost-only would pass for a plain output gain.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBEqSignedPeakTest,
    "PinWright.audio.fx.eq.PeakIsSignedAndBandLimited",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBEqSignedPeakTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    constexpr int32 NumFrames = TestSampleRate;
    constexpr double HalfWidthHz = 40.0;

    TArray<float> Source = MakeSine(NumFrames, 1000.0, 0.3);
    const TArray<float> Low = MakeSine(NumFrames, 100.0, 0.3);
    for (int32 Index = 0; Index < NumFrames; ++Index)
    {
        Source[Index] += Low[Index];
    }

    const double SourceMid = BandMagnitude(Source, 1000.0, HalfWidthHz);
    const double SourceLow = BandMagnitude(Source, 100.0, HalfWidthHz);

    TArray<float> Boosted = Source;
    FPwSeededRandom BoostRng(TestSeed);
    FString BoostCode, BoostError;
    FPwDspSpan BoostSpan = MonoSpan(Boosted);
    TestTrue(FString::Printf(TEXT("eq boost succeeds (%s %s)"), *BoostCode, *BoostError),
        PwFxEq(EqParams(+12.0), TestSampleRate, BoostRng, BoostSpan, BoostCode, BoostError));

    TArray<float> Cut = Source;
    FPwSeededRandom CutRng(TestSeed);
    FString CutCode, CutError;
    FPwDspSpan CutSpan = MonoSpan(Cut);
    TestTrue(FString::Printf(TEXT("eq cut succeeds (%s %s)"), *CutCode, *CutError),
        PwFxEq(EqParams(-12.0), TestSampleRate, CutRng, CutSpan, CutCode, CutError));

    const double BoostedMid = BandMagnitude(Boosted, 1000.0, HalfWidthHz);
    const double CutMid = BandMagnitude(Cut, 1000.0, HalfWidthHz);
    const double BoostedLow = BandMagnitude(Boosted, 100.0, HalfWidthHz);
    const double CutLow = BandMagnitude(Cut, 100.0, HalfWidthHz);

    // +12 dB is a factor of ~4; assert well over 2 so a stray 0 dB pass cannot slip through.
    TestTrue(FString::Printf(TEXT("+12 dB at 1 kHz raised the 1 kHz band (%g -> %g)"), SourceMid, BoostedMid),
        BoostedMid > SourceMid * 2.0);
    // -12 dB is a factor of ~0.25; assert well under 0.5.
    TestTrue(FString::Printf(TEXT("-12 dB at 1 kHz lowered the 1 kHz band (%g -> %g)"), SourceMid, CutMid),
        CutMid < SourceMid * 0.5);
    TestTrue(TEXT("the boost and the cut moved 1 kHz in opposite directions"), BoostedMid > CutMid);

    // The 100 Hz tone sits far outside the Q = 2 peak, and both shelves are at 0 dB, which
    // Audio::FBiquadFilter renders as an exact pass-through. It must not have moved.
    TestTrue(FString::Printf(TEXT("the boost left 100 Hz alone (%g -> %g)"), SourceLow, BoostedLow),
        FMath::Abs(BoostedLow - SourceLow) < SourceLow * 0.1);
    TestTrue(FString::Printf(TEXT("the cut left 100 Hz alone (%g -> %g)"), SourceLow, CutLow),
        FMath::Abs(CutLow - SourceLow) < SourceLow * 0.1);

    return true;
}

// =========================================================================
// J. eq failure direction: a gain outside the schema row is refused against the
//    spec table, and the span comes back bit-identical.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBEqRejectsOutOfRangeGainTest,
    "PinWright.audio.fx.eq.RejectsOutOfRangeGain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBEqRejectsOutOfRangeGainTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    const TArray<float> Source = MakeChord(TestSampleRate / 16);
    TArray<float> Samples = Source;
    FPwSeededRandom Rng(TestSeed);
    FString Code, Error;
    FPwDspSpan Span = MonoSpan(Samples);

    // The midGainDb row is -24..24; 99 is outside it.
    const bool bResult = PwFxEq(EqParams(99.0), TestSampleRate, Rng, Span, Code, Error);

    TestFalse(TEXT("a gain outside the documented range is refused, not clamped"), bResult);
    TestEqual(TEXT("the code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestTrue(TEXT("the message names the parameter"), Error.Contains(TEXT("midGainDb")));
    TestEqual(TEXT("a refused eq leaves the span bit-identical (first differing sample)"),
        FirstDifference(Source, Samples), INDEX_NONE);

    return true;
}

// =========================================================================
// K. gain: dB to linear, both directions, plus the 0 dB identity.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBGainScalesByDecibelsTest,
    "PinWright.audio.fx.gain.ScalesByDecibels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBGainScalesByDecibelsTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    const TArray<float> Source = MakeChord(TestSampleRate / 16);

    // -6.0206 dB is exactly a factor of 0.5.
    {
        TArray<float> Samples = Source;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = MonoSpan(Samples);
        TestTrue(FString::Printf(TEXT("gain succeeds (%s %s)"), *Code, *Error),
            PwFxGain(GainParams(-6.0205999), TestSampleRate, Rng, Span, Code, Error));

        double Worst = 0.0;
        for (int32 Index = 0; Index < Source.Num(); ++Index)
        {
            Worst = FMath::Max(Worst, FMath::Abs(static_cast<double>(Samples[Index]) - Source[Index] * 0.5));
        }
        TestTrue(FString::Printf(TEXT("-6.02 dB halves every sample (max |delta| %g)"), Worst), Worst < 1.e-5);
    }

    // +6.0206 dB is the opposite direction: a doubling. Asserted so a gain stuck on
    // "attenuate" cannot pass.
    {
        TArray<float> Samples = Source;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = MonoSpan(Samples);
        TestTrue(TEXT("gain succeeds"), PwFxGain(GainParams(+6.0205999), TestSampleRate, Rng, Span, Code, Error));

        double Worst = 0.0;
        for (int32 Index = 0; Index < Source.Num(); ++Index)
        {
            Worst = FMath::Max(Worst, FMath::Abs(static_cast<double>(Samples[Index]) - Source[Index] * 2.0));
        }
        TestTrue(FString::Printf(TEXT("+6.02 dB doubles every sample (max |delta| %g)"), Worst), Worst < 1.e-5);
    }

    // 0 dB is exactly 1.0 linear, so the span must come back bit-identical.
    {
        TArray<float> Samples = Source;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = MonoSpan(Samples);
        TestTrue(TEXT("gain succeeds"), PwFxGain(GainParams(0.0), TestSampleRate, Rng, Span, Code, Error));
        TestEqual(TEXT("0 dB leaves the span bit-identical (first differing sample)"),
            FirstDifference(Source, Samples), INDEX_NONE);
    }

    return true;
}

// =========================================================================
// L. width: 0 collapses to mono, 1 is bit-identical, >1 widens.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBWidthCollapsesAndWidensTest,
    "PinWright.audio.fx.width.CollapsesPassesThroughAndWidens",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBWidthCollapsesAndWidensTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    constexpr int32 NumFrames = TestSampleRate / 8;
    const TArray<float> SourceLeft = MakeSine(NumFrames, 440.0, 0.4);
    const TArray<float> SourceRight = MakeSine(NumFrames, 660.0, 0.35, 0.19);
    const double SourceSpread = MeanAbsDifference(SourceLeft, SourceRight);
    TestTrue(TEXT("the fixture actually has a stereo difference to work on"), SourceSpread > 0.05);

    // width 0 -> both channels become the mid signal.
    {
        TArray<float> Left = SourceLeft;
        TArray<float> Right = SourceRight;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = StereoSpan(Left, Right);
        TestTrue(FString::Printf(TEXT("width 0 succeeds (%s %s)"), *Code, *Error),
            PwFxWidth(WidthParams(0.0), TestSampleRate, Rng, Span, Code, Error));
        TestEqual(TEXT("width 0 makes left and right identical (first differing sample)"),
            FirstDifference(Left, Right), INDEX_NONE);
    }

    // width 1 -> identity, bit for bit.
    {
        TArray<float> Left = SourceLeft;
        TArray<float> Right = SourceRight;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = StereoSpan(Left, Right);
        TestTrue(TEXT("width 1 succeeds"), PwFxWidth(WidthParams(1.0), TestSampleRate, Rng, Span, Code, Error));
        TestEqual(TEXT("width 1 leaves left bit-identical (first differing sample)"),
            FirstDifference(SourceLeft, Left), INDEX_NONE);
        TestEqual(TEXT("width 1 leaves right bit-identical (first differing sample)"),
            FirstDifference(SourceRight, Right), INDEX_NONE);
    }

    // width 2 -> the channels move further apart.
    {
        TArray<float> Left = SourceLeft;
        TArray<float> Right = SourceRight;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = StereoSpan(Left, Right);
        TestTrue(TEXT("width 2 succeeds"), PwFxWidth(WidthParams(2.0), TestSampleRate, Rng, Span, Code, Error));
        const double WidenedSpread = MeanAbsDifference(Left, Right);
        TestTrue(FString::Printf(TEXT("width 2 increases the channel difference (%g -> %g)"),
            SourceSpread, WidenedSpread), WidenedSpread > SourceSpread * 1.5);
        TestTrue(TEXT("widened output is finite"), AllFinite(Left) && AllFinite(Right));
    }

    return true;
}

// =========================================================================
// M. width failure direction: a mono span is refused with the master.fx remedy,
//    not silently no-oped.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBWidthRejectsMonoSpanTest,
    "PinWright.audio.fx.width.RejectsMonoSpan",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBWidthRejectsMonoSpanTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    const TArray<float> Source = MakeChord(TestSampleRate / 16);
    TArray<float> Samples = Source;
    FPwSeededRandom Rng(TestSeed);
    FString Code, Error;
    FPwDspSpan Span = MonoSpan(Samples);

    const bool bResult = PwFxWidth(WidthParams(1.6), TestSampleRate, Rng, Span, Code, Error);

    TestFalse(TEXT("width on a mono span is an error, not a silent no-op"), bResult);
    TestEqual(TEXT("the code is UNSUPPORTED_OPERATION"), Code,
        FString(ErrorCodes::ERR_UNSUPPORTED_OPERATION));
    TestTrue(TEXT("the message names master.fx as the remedy"), Error.Contains(TEXT("master.fx")));
    TestEqual(TEXT("a refused width leaves the span bit-identical (first differing sample)"),
        FirstDifference(Source, Samples), INDEX_NONE);

    return true;
}

// =========================================================================
// N. reverse: exact and self-inverse, mono and stereo.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBReverseRoundTripsTest,
    "PinWright.audio.fx.reverse.RoundTripsBitIdentically",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBReverseRoundTripsTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    // An odd length, so the middle sample has to be handled correctly, and asymmetric
    // content, so a reversal is observable at all.
    constexpr int32 NumFrames = 4097;
    const TArray<float> SourceLeft = MakeChord(NumFrames);
    TArray<float> SourceRight = MakeSine(NumFrames, 137.0, 0.4);
    for (int32 Index = 0; Index < NumFrames; ++Index)
    {
        SourceRight[Index] *= static_cast<float>(Index) / static_cast<float>(NumFrames);
    }

    const FPwSynthParams NoParams;

    // --- mono ---
    {
        TArray<float> Samples = SourceLeft;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = MonoSpan(Samples);

        TestTrue(FString::Printf(TEXT("reverse succeeds (%s %s)"), *Code, *Error),
            PwFxReverse(NoParams, TestSampleRate, Rng, Span, Code, Error));
        TestTrue(TEXT("one reversal changed the buffer"),
            FirstDifference(SourceLeft, Samples) != INDEX_NONE);
        // Exact, not near-equal: a reversal moves samples, it does not compute with them.
        TestTrue(TEXT("the first output sample is exactly the last input sample"),
            Samples[0] == SourceLeft[NumFrames - 1]);

        TestTrue(TEXT("reverse succeeds a second time"),
            PwFxReverse(NoParams, TestSampleRate, Rng, Span, Code, Error));
        TestEqual(TEXT("reversing twice restores the buffer bit-identically (first differing sample)"),
            FirstDifference(SourceLeft, Samples), INDEX_NONE);
    }

    // --- stereo: both channels reverse ---
    {
        TArray<float> Left = SourceLeft;
        TArray<float> Right = SourceRight;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = StereoSpan(Left, Right);

        TestTrue(TEXT("stereo reverse succeeds"), PwFxReverse(NoParams, TestSampleRate, Rng, Span, Code, Error));
        TestTrue(TEXT("the right channel was reversed too"),
            Right[0] == SourceRight[NumFrames - 1]);

        TestTrue(TEXT("stereo reverse succeeds a second time"),
            PwFxReverse(NoParams, TestSampleRate, Rng, Span, Code, Error));
        TestEqual(TEXT("stereo left round-trips bit-identically (first differing sample)"),
            FirstDifference(SourceLeft, Left), INDEX_NONE);
        TestEqual(TEXT("stereo right round-trips bit-identically (first differing sample)"),
            FirstDifference(SourceRight, Right), INDEX_NONE);
    }

    return true;
}

// =========================================================================
// O. convolve: a unit-impulse impulse response is the identity, on mono and stereo.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBConvolveUnitImpulseIsIdentityTest,
    "PinWright.audio.fx.convolve.UnitImpulseIsIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBConvolveUnitImpulseIsIdentityTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    USoundWave* Wave = MakeImpulseFixtureWave(UnitImpulsePcm(), TestSampleRate);
    TestNotNull(TEXT("the impulse-response fixture wave was created"), Wave);
    if (!Wave)
    {
        return false;
    }
    ON_SCOPE_EXIT{ Wave->RemoveFromRoot(); };
    const FString ImpulsePath = Wave->GetPathName();

    // Resolved through the production path first, so a failure below points at the
    // convolution and not at the fixture.
    FPwAudioBuffer Resolved;
    FString ResolveCode, ResolveError;
    TestTrue(FString::Printf(TEXT("the fixture resolves through PwResolveSourceBuffer (%s %s)"),
        *ResolveCode, *ResolveError),
        PwResolveSourceBuffer(ImpulsePath, Resolved, ResolveCode, ResolveError));
    TestEqual(TEXT("the fixture decoded at the render sample rate"), Resolved.SampleRate, TestSampleRate);

    constexpr int32 NumFrames = TestSampleRate / 4;
    const TArray<float> SourceLeft = MakeChord(NumFrames);
    const TArray<float> SourceRight = MakeSine(NumFrames, 330.0, 0.3, 0.11);

    // --- mono ---
    {
        TArray<float> Samples = SourceLeft;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = MonoSpan(Samples);
        TestTrue(FString::Printf(TEXT("convolve succeeds (%s %s)"), *Code, *Error),
            PwFxConvolve(ConvolveParams(ImpulsePath), TestSampleRate, Rng, Span, Code, Error));

        const double Worst = MaxAbsDifference(SourceLeft, Samples);
        TestTrue(FString::Printf(TEXT("a unit impulse returns the mono input unchanged (max |delta| %g)"),
            Worst), Worst < 1.e-4);
        TestTrue(TEXT("convolved output is finite"), AllFinite(Samples));
    }

    // --- stereo: both channels convolve against their own copy of the impulse ---
    {
        TArray<float> Left = SourceLeft;
        TArray<float> Right = SourceRight;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = StereoSpan(Left, Right);
        TestTrue(TEXT("stereo convolve succeeds"),
            PwFxConvolve(ConvolveParams(ImpulsePath), TestSampleRate, Rng, Span, Code, Error));

        TestTrue(FString::Printf(TEXT("a unit impulse returns the left channel unchanged (max |delta| %g)"),
            MaxAbsDifference(SourceLeft, Left)), MaxAbsDifference(SourceLeft, Left) < 1.e-4);
        TestTrue(FString::Printf(TEXT("a unit impulse returns the right channel unchanged (max |delta| %g)"),
            MaxAbsDifference(SourceRight, Right)), MaxAbsDifference(SourceRight, Right) < 1.e-4);
    }

    // --- mix 0 is bit-identical even though the impulse resolved fine ---
    {
        TArray<float> Samples = SourceLeft;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = MonoSpan(Samples);
        TestTrue(TEXT("convolve at mix 0 succeeds"),
            PwFxConvolve(ConvolveParams(ImpulsePath, /*Mix=*/0.0), TestSampleRate, Rng, Span, Code, Error));
        TestEqual(TEXT("convolve at mix 0 leaves the span bit-identical (first differing sample)"),
            FirstDifference(SourceLeft, Samples), INDEX_NONE);
    }

    return true;
}

// =========================================================================
// P. convolve failure direction: an impulse path that resolves to nothing is an
//    error - including when the effect is fully dry, where a silent success would
//    hide a typo until someone raised the mix.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBConvolveRejectsUnresolvableImpulseTest,
    "PinWright.audio.fx.convolve.RejectsUnresolvableImpulse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBConvolveRejectsUnresolvableImpulseTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    // Full object path (Package.Object), the shape TestPwGenSampleGranular.cpp uses for the
    // same resolver's missing-asset cases.
    const FString MissingName = FString::Printf(TEXT("SW_FxImpulseAbsent_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString MissingPath = FString::Printf(TEXT("/Game/PinWrightTests/%s.%s"),
        *MissingName, *MissingName);
    const TArray<float> Source = MakeChord(TestSampleRate / 16);

    {
        TArray<float> Samples = Source;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = MonoSpan(Samples);

        const bool bResult = PwFxConvolve(ConvolveParams(MissingPath), TestSampleRate, Rng, Span, Code, Error);

        TestFalse(TEXT("an unresolvable impulse path is an error"), bResult);
        TestFalse(TEXT("the failure carries a registered error code"), Code.IsEmpty());
        TestEqual(TEXT("a refused convolve leaves the span bit-identical (first differing sample)"),
            FirstDifference(Source, Samples), INDEX_NONE);
    }

    {
        // Fully dry, but still resolved: a typo must not be masked by mix == 0.
        TArray<float> Samples = Source;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = MonoSpan(Samples);

        const bool bResult = PwFxConvolve(ConvolveParams(MissingPath, /*Mix=*/0.0),
            TestSampleRate, Rng, Span, Code, Error);

        TestFalse(TEXT("an unresolvable impulse path is still an error at mix 0"), bResult);
        TestEqual(TEXT("the dry refusal also leaves the span bit-identical (first differing sample)"),
            FirstDifference(Source, Samples), INDEX_NONE);
    }

    {
        // An empty impulsePath is a caller mistake, not a reason to pass audio through.
        TArray<float> Samples = Source;
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;
        FPwDspSpan Span = MonoSpan(Samples);

        const bool bResult = PwFxConvolve(ConvolveParams(FString()), TestSampleRate, Rng, Span, Code, Error);

        TestFalse(TEXT("an empty impulsePath is an error"), bResult);
        TestEqual(TEXT("the code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("the message names the parameter"), Error.Contains(TEXT("impulsePath")));
        TestEqual(TEXT("the refusal leaves the span bit-identical (first differing sample)"),
            FirstDifference(Source, Samples), INDEX_NONE);
    }

    return true;
}

// =========================================================================
// P2. convolve: an impulse response at a DIFFERENT sample rate is resampled to the
//     render rate, so its tail lasts its real duration in SECONDS rather than its raw
//     frame count. This is the assertion that catches the stretch: a 100 ms impulse
//     stored at 24 kHz is 2400 frames, and using those frames unresampled at 48 kHz
//     would produce a 50 ms tail - half as long, with every resonance an octave off.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBConvolveResamplesImpulseTest,
    "PinWright.audio.fx.convolve.ResamplesImpulseToRenderRate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBConvolveResamplesImpulseTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    constexpr int32 ImpulseRate = 24000;                    // half the render rate
    constexpr int32 ImpulseFrames = 2400;                   // 100 ms at 24 kHz
    constexpr int32 ExpectedTailFrames = 4800;              // 100 ms at the 48 kHz render rate
    constexpr int32 RawFrameCountTail = ImpulseFrames;      // what an unresampled impulse would give

    USoundWave* Wave = MakeImpulseFixtureWave(RectangularImpulsePcm(ImpulseFrames), ImpulseRate);
    TestNotNull(TEXT("the 24 kHz impulse fixture was created"), Wave);
    if (!Wave)
    {
        return false;
    }
    ON_SCOPE_EXIT{ Wave->RemoveFromRoot(); };
    const FString ImpulsePath = Wave->GetPathName();

    // The premise of the whole test: the loader hands the impulse back at its NATIVE rate.
    FPwAudioBuffer Resolved;
    FString ResolveCode, ResolveError;
    TestTrue(FString::Printf(TEXT("the fixture resolves (%s %s)"), *ResolveCode, *ResolveError),
        PwResolveSourceBuffer(ImpulsePath, Resolved, ResolveCode, ResolveError));
    TestEqual(TEXT("the loader returns the impulse at its native 24 kHz, not the render rate"),
        Resolved.SampleRate, ImpulseRate);
    TestEqual(TEXT("the impulse is 2400 frames as stored"), Resolved.NumFrames(), ImpulseFrames);

    // A single unit sample in: the convolution output IS the impulse response, so the
    // output's tail length is the impulse response's length, measured directly.
    constexpr int32 NumFrames = TestSampleRate / 2;         // 500 ms, five times the tail
    TArray<float> Samples;
    Samples.SetNumZeroed(NumFrames);
    Samples[0] = 1.f;

    FPwSeededRandom Rng(TestSeed);
    FString Code, Error;
    FPwDspSpan Span = MonoSpan(Samples);
    TestTrue(FString::Printf(TEXT("convolve succeeds against a 24 kHz impulse (%s %s)"), *Code, *Error),
        PwFxConvolve(ConvolveParams(ImpulsePath, /*Mix=*/1.0, /*bNormalize=*/false),
            TestSampleRate, Rng, Span, Code, Error));

    const int32 MeasuredTail = LastSampleAbove(Samples, 0.05) + 1;

    // Both directions of the same measurement. The first assertion alone would pass an
    // implementation that stretched the impulse too far; the second alone would pass one
    // that did not stretch it at all.
    TestTrue(FString::Printf(TEXT("the tail lasts the impulse's real duration (%d frames, expected ~%d)"),
        MeasuredTail, ExpectedTailFrames),
        FMath::Abs(MeasuredTail - ExpectedTailFrames) < 120);
    TestTrue(FString::Printf(TEXT("the tail is NOT the impulse's raw frame count (%d frames, unresampled would be %d)"),
        MeasuredTail, RawFrameCountTail),
        MeasuredTail > RawFrameCountTail * 3 / 2);

    // Spot checks either side of where the unresampled tail would have ended.
    TestTrue(FString::Printf(TEXT("the tail is still at full level well past the raw frame count (out[4000] = %g)"),
        Samples[4000]), FMath::Abs(Samples[4000] - 0.5f) < 0.02f);
    TestTrue(FString::Printf(TEXT("the tail has ended past the real duration (out[6000] = %g)"),
        Samples[6000]), FMath::Abs(Samples[6000]) < 0.01f);
    TestTrue(TEXT("resampled convolution output is finite"), AllFinite(Samples));

    return true;
}

// =========================================================================
// P3. eq: a band frequency the biquad could only realise by clamping is refused.
//     Audio::FBiquadFilter pins every cutoff into [5 Hz, 0.9 * Nyquist] silently, so
//     the default 4 kHz high shelf is unrealisable at an 8 kHz render rate.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBEqRejectsUnrealisableFrequencyTest,
    "PinWright.audio.fx.eq.RejectsUnrealisableBandFrequency",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBEqRejectsUnrealisableFrequencyTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    constexpr int32 LowRenderRate = 8000;   // 0.9 * Nyquist = 3600 Hz, below the 4 kHz shelf
    const TArray<float> Source = MakeChord(2048);
    TArray<float> Samples = Source;
    FPwSeededRandom Rng(TestSeed);
    FString Code, Error;
    FPwDspSpan Span = MonoSpan(Samples);

    const bool bResult = PwFxEq(EqParams(3.0), LowRenderRate, Rng, Span, Code, Error);

    TestFalse(TEXT("an unrealisable band frequency is refused, not silently clamped"), bResult);
    TestEqual(TEXT("the code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestTrue(TEXT("the message names the offending band"), Error.Contains(TEXT("highHz")));
    TestTrue(TEXT("the message names the render rate"), Error.Contains(TEXT("8000")));
    TestEqual(TEXT("a refused eq leaves the span bit-identical (first differing sample)"),
        FirstDifference(Source, Samples), INDEX_NONE);

    // Control: the same parameters at 48 kHz are realisable and succeed, so the rejection
    // above is about the rate and not about the parameters.
    TArray<float> Fine = Source;
    FPwSeededRandom FineRng(TestSeed);
    FString FineCode, FineError;
    FPwDspSpan FineSpan = MonoSpan(Fine);
    TestTrue(FString::Printf(TEXT("the same bands are accepted at 48 kHz (%s %s)"), *FineCode, *FineError),
        PwFxEq(EqParams(3.0), TestSampleRate, FineRng, FineSpan, FineCode, FineError));

    return true;
}

// =========================================================================
// Q. Empty-span rejection, across every effect. Existence is checked before
//    anything else (rpc-design.md §7): "there were no samples" must not surface
//    as a narrower-sounding parameter or effect failure.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxChainBRejectsEmptySpanTest,
    "PinWright.audio.fx.chain.RejectsEmptySpan",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxChainBRejectsEmptySpanTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainBTest;

    TArray<FFxCase> Cases = MonoCapableCases();
    Cases.Add(FFxCase{ TEXT("width"), &PwFxWidth, WidthParams(1.5) });

    for (const FFxCase& Case : Cases)
    {
        FPwDspSpan Span;      // Left == nullptr, NumFrames == 0
        FPwSeededRandom Rng(TestSeed);
        FString Code, Error;

        TestFalse(FString::Printf(TEXT("%s refuses an empty span"), Case.Name),
            Case.Function(Case.Params, TestSampleRate, Rng, Span, Code, Error));

        const FString Description = FString::Printf(
            TEXT("%s reports AUDIO_EMPTY_BUFFER for an empty span"), Case.Name);
        TestEqual(*Description, Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
    }

    return true;
}
