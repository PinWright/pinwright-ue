// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for FX chain A (AudioGen/PwFxChainA.cpp): filter, distort, delay, reverb.
//
// These effects have real ground truth, so nothing here asserts "it returned true". A lowpass has
// a measurable stopband AND a measurable passband, distortion puts energy in harmonic bins that
// were empty before, a delay puts a copy of the input at a countable sample offset, and a reverb
// leaves energy where the dry signal has none. Spectral measurements go through AudioGen/PwStft.h,
// the same front-end the analysis verbs use.
//
// Both directions are measured (rpc-design.md §6): a filter that killed the whole spectrum would
// pass a stopband-only check, so every filter assertion pairs "the stopband dropped" with "the
// passband did not". And per §12 the failure direction is asserted for real - an unknown filter or
// distortion type must return false with the registered code AND leave a pre-filled buffer
// bit-identical, checked sample by sample rather than by a summary statistic.
//
// One note on coverage: the spec table gives distort, delay and reverb a `mix` row and gives the
// filter none, so "mix 0 is bit-identical" is asserted for the three effects that have the
// parameter. A filter is always fully wet by schema, not by omission in the implementation.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwSeededRandom.h"
#include "AudioGen/PwStft.h"
#include "AudioGen/PwSynthDsp.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Handlers/ErrorCodes.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true.
namespace PwFxChainATestHelpers
{
    constexpr int32 TestSampleRate = 48000;

    /** Signature shared by all four effects, so the table-driven tests can hold one pointer type. */
    using FEffectFn = bool (*)(const FPwSynthParams&, int32, FPwSeededRandom&, const FPwDspSpan&,
        FString&, FString&);

    // FFT size 2048 at 48 kHz puts bin 43 on 1007.8125 Hz and bin 129 on exactly three times that,
    // so a fundamental and its third harmonic both sit on bin centres and neither measurement pays
    // scalloping loss.
    constexpr int32 HarmonicFftSize = 2048;
    constexpr int32 FundamentalBin = 43;
    constexpr int32 ThirdHarmonicBin = 129;

    /** INDEX_NONE is an untyped enumerator; TestNotEqual deduces one ValueType from both arguments. */
    constexpr int32 NoIndex = INDEX_NONE;

    // -----------------------------------------------------------------------------------------
    // Signal construction
    // -----------------------------------------------------------------------------------------

    TArray<float> MakeSine(int32 NumSamples, int32 SampleRate, double Hz, double Amplitude)
    {
        TArray<float> Samples;
        Samples.SetNumZeroed(NumSamples);
        const double AngularStep = 2.0 * UE_DOUBLE_PI * Hz / static_cast<double>(SampleRate);
        for (int32 Index = 0; Index < NumSamples; ++Index)
        {
            Samples[Index] = static_cast<float>(Amplitude * FMath::Sin(AngularStep * Index));
        }
        return Samples;
    }

    /** White noise from the seeded stream, so a failing run reproduces exactly. */
    TArray<float> MakeNoise(int32 NumSamples, int32 Seed, float Amplitude)
    {
        FPwSeededRandom Rng(Seed);
        TArray<float> Samples;
        Samples.SetNumUninitialized(NumSamples);
        for (int32 Index = 0; Index < NumSamples; ++Index)
        {
            Samples[Index] = Rng.FloatInRange(-Amplitude, Amplitude);
        }
        return Samples;
    }

    /** A 1 kHz burst at the head of an otherwise silent buffer. */
    TArray<float> MakeBurst(int32 NumSamples, int32 SampleRate, int32 BurstSamples)
    {
        TArray<float> Samples;
        Samples.SetNumZeroed(NumSamples);
        const double AngularStep = 2.0 * UE_DOUBLE_PI * 1000.0 / static_cast<double>(SampleRate);
        for (int32 Index = 0; Index < FMath::Min(BurstSamples, NumSamples); ++Index)
        {
            Samples[Index] = static_cast<float>(FMath::Sin(AngularStep * Index));
        }
        return Samples;
    }

    FPwDspSpan MonoSpan(TArray<float>& Buffer)
    {
        FPwDspSpan Span;
        Span.Left = Buffer.GetData();
        Span.Right = nullptr;
        Span.NumFrames = Buffer.Num();
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

    // -----------------------------------------------------------------------------------------
    // Params bags. Built directly rather than through ParseSynthRecipe: these tests exercise the
    // DSP contract, and a bag that never met the parser is exactly the input the DSP's own
    // validation exists for.
    // -----------------------------------------------------------------------------------------

    void SetNumber(FPwSynthParams& Params, const TCHAR* Name, double Number)
    {
        FPwSynthParamValue Entry;
        Entry.Type = EPwSynthParamType::Number;
        Entry.Number = Number;
        Params.Values.Add(FName(Name), Entry);
    }

    void SetEnum(FPwSynthParams& Params, const TCHAR* Name, const TCHAR* Token)
    {
        FPwSynthParamValue Entry;
        Entry.Type = EPwSynthParamType::Enum;
        Entry.String = Token;
        Params.Values.Add(FName(Name), Entry);
    }

    FPwSynthParams FilterBag(const TCHAR* Type, double CutoffHz, double Resonance = 0.707, double GainDb = 0.0)
    {
        FPwSynthParams Params;
        SetEnum(Params, TEXT("type"), Type);
        SetNumber(Params, TEXT("cutoffHz"), CutoffHz);
        SetNumber(Params, TEXT("resonance"), Resonance);
        SetNumber(Params, TEXT("gainDb"), GainDb);
        return Params;
    }

    FPwSynthParams DistortBag(const TCHAR* Type, double Drive, double Mix = 1.0, double OutGainDb = 0.0)
    {
        FPwSynthParams Params;
        SetEnum(Params, TEXT("type"), Type);
        SetNumber(Params, TEXT("drive"), Drive);
        SetNumber(Params, TEXT("mix"), Mix);
        SetNumber(Params, TEXT("outGainDb"), OutGainDb);
        return Params;
    }

    FPwSynthParams DelayBag(double TimeMs, double Mix, double Feedback = 0.0, double DampingHz = 20000.0)
    {
        FPwSynthParams Params;
        SetNumber(Params, TEXT("timeMs"), TimeMs);
        SetNumber(Params, TEXT("mix"), Mix);
        SetNumber(Params, TEXT("feedback"), Feedback);
        SetNumber(Params, TEXT("dampingHz"), DampingHz);
        return Params;
    }

    FPwSynthParams ReverbBag(double DecayMs, double Mix, double PreDelayMs = 0.0, double DampingHz = 8000.0)
    {
        FPwSynthParams Params;
        SetNumber(Params, TEXT("decayMs"), DecayMs);
        SetNumber(Params, TEXT("mix"), Mix);
        SetNumber(Params, TEXT("preDelayMs"), PreDelayMs);
        SetNumber(Params, TEXT("dampingHz"), DampingHz);
        return Params;
    }

    // -----------------------------------------------------------------------------------------
    // Measurement
    // -----------------------------------------------------------------------------------------

    /** Total magnitude-squared energy in [LoHz, HiHz], in dB. Returns a floor for a silent input. */
    double BandEnergyDb(TArrayView<const float> Mono, int32 SampleRate, double LoHz, double HiHz)
    {
        FPwStftSettings Settings;
        Settings.FftSize = 2048;
        Settings.HopSize = 1024;

        FPwStftResult Result;
        if (!PwComputeStft(Mono, SampleRate, Settings, Result, nullptr))
        {
            return -300.0;
        }

        const int32 LoBin = FMath::Max(1, FMath::CeilToInt(LoHz / Result.BinHz));
        const int32 HiBin = FMath::Min(Result.NumBins - 1, FMath::FloorToInt(HiHz / Result.BinHz));

        double Sum = 0.0;
        for (int32 Frame = 0; Frame < Result.NumFrames; ++Frame)
        {
            for (int32 Bin = LoBin; Bin <= HiBin; ++Bin)
            {
                const double Magnitude = PwStftMagnitudeAt(Result, Frame, Bin);
                Sum += Magnitude * Magnitude;
            }
        }
        return 10.0 * FMath::LogX(10.0, FMath::Max(Sum, 1.e-30));
    }

    /** Level of one bin in the middle analysis frame, in dB, with no floor clipping. */
    double BinDb(TArrayView<const float> Mono, int32 SampleRate, int32 BinIndex)
    {
        FPwStftSettings Settings;
        Settings.FftSize = HarmonicFftSize;
        Settings.HopSize = HarmonicFftSize / 2;

        FPwStftResult Result;
        if (!PwComputeStft(Mono, SampleRate, Settings, Result, nullptr))
        {
            return -300.0;
        }
        const float Magnitude = PwStftMagnitudeAt(Result, Result.NumFrames / 2, BinIndex);
        return 20.0 * FMath::LogX(10.0, FMath::Max(static_cast<double>(Magnitude), 1.e-15));
    }

    double Rms(TArrayView<const float> Samples, int32 First, int32 Last)
    {
        const int32 Begin = FMath::Max(0, First);
        const int32 End = FMath::Min(Samples.Num(), Last);
        if (End <= Begin)
        {
            return 0.0;
        }
        double Sum = 0.0;
        for (int32 Index = Begin; Index < End; ++Index)
        {
            Sum += static_cast<double>(Samples[Index]) * static_cast<double>(Samples[Index]);
        }
        return FMath::Sqrt(Sum / static_cast<double>(End - Begin));
    }

    float MaxAbs(TArrayView<const float> Samples, int32 First, int32 Last)
    {
        const int32 Begin = FMath::Max(0, First);
        const int32 End = FMath::Min(Samples.Num(), Last);
        float Worst = 0.f;
        for (int32 Index = Begin; Index < End; ++Index)
        {
            Worst = FMath::Max(Worst, FMath::Abs(Samples[Index]));
        }
        return Worst;
    }

    /** Peak magnitude in a window, plus the index it sat at. */
    float PeakInWindow(TArrayView<const float> Samples, int32 First, int32 Last, int32& OutIndex)
    {
        OutIndex = INDEX_NONE;
        float Worst = -1.f;
        const int32 Begin = FMath::Max(0, First);
        const int32 End = FMath::Min(Samples.Num(), Last);
        for (int32 Index = Begin; Index < End; ++Index)
        {
            const float Magnitude = FMath::Abs(Samples[Index]);
            if (Magnitude > Worst)
            {
                Worst = Magnitude;
                OutIndex = Index;
            }
        }
        return FMath::Max(Worst, 0.f);
    }

    bool AllFinite(TArrayView<const float> Samples, int32& OutFirstBadIndex)
    {
        for (int32 Index = 0; Index < Samples.Num(); ++Index)
        {
            if (!FMath::IsFinite(Samples[Index]))
            {
                OutFirstBadIndex = Index;
                return false;
            }
        }
        OutFirstBadIndex = INDEX_NONE;
        return true;
    }

    /** Index of the first sample whose bit pattern differs, or INDEX_NONE for a byte-for-byte match. */
    int32 FirstDifference(const TArray<float>& A, const TArray<float>& B)
    {
        if (A.Num() != B.Num())
        {
            return 0;
        }
        for (int32 Index = 0; Index < A.Num(); ++Index)
        {
            if (FMemory::Memcmp(&A[Index], &B[Index], sizeof(float)) != 0)
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }
}

// =========================================================================================
// A. filter - a lowpass has to do BOTH halves of its job: drop the stopband and keep the
//    passband. A filter that silenced everything would pass a stopband-only check.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxFilterLowpassTest,
    "PinWright.audio.fx.filter.LowpassCutsHighsKeepsLows",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxFilterLowpassTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainATestHelpers;

    const TArray<float> Reference = MakeNoise(TestSampleRate, /*Seed=*/20261, 0.5f);
    TArray<float> Filtered = Reference;

    const double LowBefore = BandEnergyDb(Reference, TestSampleRate, 100.0, 400.0);
    const double HighBefore = BandEnergyDb(Reference, TestSampleRate, 4000.0, 16000.0);

    FPwSeededRandom Rng(7);
    FPwDspSpan Span = MonoSpan(Filtered);
    FString Code;
    FString Error;
    const bool bOk = PwFxFilter(FilterBag(TEXT("lowpass"), 1000.0), TestSampleRate, Rng, Span, Code, Error);
    TestTrue(FString::Printf(TEXT("PwFxFilter succeeded (%s: %s)"), *Code, *Error), bOk);
    if (!bOk)
    {
        return false;
    }

    const double LowAfter = BandEnergyDb(Filtered, TestSampleRate, 100.0, 400.0);
    const double HighAfter = BandEnergyDb(Filtered, TestSampleRate, 4000.0, 16000.0);

    // A 12 dB/octave lowpass at 1 kHz integrates to about -34 dB over 4-16 kHz and to about
    // -0.02 dB over 100-400 Hz. The thresholds sit well inside both predictions, so what they
    // catch is a wrong response type or a botched Q -> bandwidth conversion, not measurement noise.
    TestTrue(FString::Printf(TEXT("4-16 kHz dropped from %.2f to %.2f dB (expected >= 15 dB)"),
        HighBefore, HighAfter), (HighBefore - HighAfter) >= 15.0);
    TestTrue(FString::Printf(TEXT("100-400 Hz survived: %.2f -> %.2f dB (expected within 1.5 dB)"),
        LowBefore, LowAfter), FMath::Abs(LowBefore - LowAfter) <= 1.5);

    int32 BadIndex = INDEX_NONE;
    const bool bFinite = AllFinite(Filtered, BadIndex);
    TestTrue(FString::Printf(TEXT("no NaN/Inf (first bad index %d)"), BadIndex), bFinite);

    return true;
}

// =========================================================================================
// B. filter - the mirror image. Together with A this proves the type really selects the
//    response, instead of one shape being applied under every name.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxFilterHighpassTest,
    "PinWright.audio.fx.filter.HighpassCutsLowsKeepsHighs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxFilterHighpassTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainATestHelpers;

    const TArray<float> Reference = MakeNoise(TestSampleRate, /*Seed=*/20261, 0.5f);
    TArray<float> Filtered = Reference;

    const double LowBefore = BandEnergyDb(Reference, TestSampleRate, 100.0, 400.0);
    const double HighBefore = BandEnergyDb(Reference, TestSampleRate, 4000.0, 16000.0);

    FPwSeededRandom Rng(7);
    FPwDspSpan Span = MonoSpan(Filtered);
    FString Code;
    FString Error;
    const bool bOk = PwFxFilter(FilterBag(TEXT("highpass"), 1000.0), TestSampleRate, Rng, Span, Code, Error);
    TestTrue(FString::Printf(TEXT("PwFxFilter succeeded (%s: %s)"), *Code, *Error), bOk);
    if (!bOk)
    {
        return false;
    }

    const double LowAfter = BandEnergyDb(Filtered, TestSampleRate, 100.0, 400.0);
    const double HighAfter = BandEnergyDb(Filtered, TestSampleRate, 4000.0, 16000.0);

    // Predicted: about -22 dB over 100-400 Hz, about -0.01 dB over 4-16 kHz.
    TestTrue(FString::Printf(TEXT("100-400 Hz dropped from %.2f to %.2f dB (expected >= 15 dB)"),
        LowBefore, LowAfter), (LowBefore - LowAfter) >= 15.0);
    TestTrue(FString::Printf(TEXT("4-16 kHz survived: %.2f -> %.2f dB (expected within 1.5 dB)"),
        HighBefore, HighAfter), FMath::Abs(HighBefore - HighAfter) <= 1.5);

    return true;
}

// =========================================================================================
// C. filter failure direction (§12) - an unrecognised type errors with the registered code,
//    names the valid set, and leaves the buffer bit-identical sample by sample.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxFilterUnknownTypeTest,
    "PinWright.audio.fx.filter.UnknownTypeIsAnErrorAndLeavesBufferUntouched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxFilterUnknownTypeTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainATestHelpers;

    const TArray<float> Reference = MakeNoise(4096, /*Seed=*/99, 0.75f);
    TArray<float> Left = Reference;
    TArray<float> Right = Reference;

    FPwSeededRandom Rng(3);
    FPwDspSpan Span = StereoSpan(Left, Right);
    FString Code;
    FString Error;
    const bool bOk = PwFxFilter(FilterBag(TEXT("bandreject"), 1000.0), TestSampleRate, Rng, Span, Code, Error);

    TestFalse(TEXT("an unrecognised filter type is rejected"), bOk);
    TestEqual(TEXT("code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestTrue(FString::Printf(TEXT("message names the offending value: %s"), *Error),
        Error.Contains(TEXT("bandreject")));
    TestTrue(FString::Printf(TEXT("message names the valid set: %s"), *Error),
        Error.Contains(TEXT("lowpass")) && Error.Contains(TEXT("peaking")));

    TestEqual(TEXT("left channel is bit-identical (index of first difference)"),
        FirstDifference(Left, Reference), NoIndex);
    TestEqual(TEXT("right channel is bit-identical (index of first difference)"),
        FirstDifference(Right, Reference), NoIndex);

    // A missing required value fails the same way.
    FPwSynthParams NoCutoff;
    SetEnum(NoCutoff, TEXT("type"), TEXT("lowpass"));
    TArray<float> Mono = Reference;
    FPwDspSpan MonoBus = MonoSpan(Mono);
    const bool bSecondOk = PwFxFilter(NoCutoff, TestSampleRate, Rng, MonoBus, Code, Error);
    TestFalse(TEXT("a missing required 'cutoffHz' is rejected"), bSecondOk);
    TestEqual(TEXT("code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestEqual(TEXT("buffer is bit-identical (index of first difference)"),
        FirstDifference(Mono, Reference), NoIndex);

    return true;
}

// =========================================================================================
// C2. filter failure direction, the cross-field case - a cutoff the biquad cannot realise at
//     this render rate is rejected rather than silently clamped. The pairing is what makes this
//     meaningful: 5000 Hz is a legal schema value and IS accepted at 48 kHz, so what the
//     rejection reports is the interaction with sampleRate, not the value in isolation.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxFilterCutoffWindowTest,
    "PinWright.audio.fx.filter.CutoffPastNyquistIsAnErrorNotASilentClamp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxFilterCutoffWindowTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainATestHelpers;

    // 5 kHz is above 0.45 * 8000 = 3600, the window Audio::FBiquadFilter would clamp into.
    const int32 LowRate = 8000;
    const TArray<float> Reference = MakeNoise(4096, /*Seed=*/515, 0.5f);
    TArray<float> Buffer = Reference;

    FPwSeededRandom Rng(3);
    FPwDspSpan Span = MonoSpan(Buffer);
    FString Code;
    FString Error;
    const bool bOk = PwFxFilter(FilterBag(TEXT("lowpass"), 5000.0), LowRate, Rng, Span, Code, Error);

    TestFalse(TEXT("a cutoff past the biquad's window is rejected"), bOk);
    TestEqual(TEXT("code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestTrue(FString::Printf(TEXT("message names the render rate: %s"), *Error),
        Error.Contains(TEXT("8000")));
    TestEqual(TEXT("buffer is bit-identical (index of first difference)"),
        FirstDifference(Buffer, Reference), NoIndex);

    // The same cutoff at a rate that can carry it is accepted, which is what proves the rejection
    // is about the pair and not about the number.
    TArray<float> Accepted = Reference;
    FPwDspSpan AcceptedSpan = MonoSpan(Accepted);
    const bool bAcceptedOk = PwFxFilter(FilterBag(TEXT("lowpass"), 5000.0), TestSampleRate, Rng,
        AcceptedSpan, Code, Error);
    TestTrue(FString::Printf(TEXT("5 kHz at 48 kHz is accepted (%s: %s)"), *Code, *Error), bAcceptedOk);
    TestNotEqual(TEXT("and it actually filtered"), FirstDifference(Accepted, Reference), NoIndex);

    return true;
}

// =========================================================================================
// D. distort - harmonics that were not there before, and more drive puts more of them there.
//    The dry sine's third-harmonic bin is empty, so the wet reading cannot come from the source.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxDistortHarmonicsTest,
    "PinWright.audio.fx.distort.AddsHarmonicsAndMoreDriveAddsMore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxDistortHarmonicsTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainATestHelpers;

    // Bin 43 of a 2048-point transform at 48 kHz: 1007.8125 Hz. Its third harmonic is bin 129.
    const double FundamentalHz = static_cast<double>(FundamentalBin) * TestSampleRate / HarmonicFftSize;
    const TArray<float> Dry = MakeSine(TestSampleRate / 2, TestSampleRate, FundamentalHz, 0.5);

    const double DryRatio = BinDb(Dry, TestSampleRate, ThirdHarmonicBin)
        - BinDb(Dry, TestSampleRate, FundamentalBin);
    TestTrue(FString::Printf(TEXT("the dry sine has no third harmonic (%.1f dB relative)"), DryRatio),
        DryRatio < -60.0);

    auto MeasureRatio = [this, &Dry](double Drive, double& OutRatio) -> bool
    {
        TArray<float> Wet = Dry;
        FPwSeededRandom Rng(11);
        FPwDspSpan Span = MonoSpan(Wet);
        FString Code;
        FString Error;
        if (!PwFxDistort(DistortBag(TEXT("soft"), Drive), TestSampleRate, Rng, Span, Code, Error))
        {
            AddError(FString::Printf(TEXT("PwFxDistort(drive=%g) failed: %s: %s"), Drive, *Code, *Error));
            return false;
        }
        OutRatio = BinDb(Wet, TestSampleRate, ThirdHarmonicBin) - BinDb(Wet, TestSampleRate, FundamentalBin);
        return true;
    };

    double GentleRatio = 0.0;
    double HardRatio = 0.0;
    if (!MeasureRatio(0.5, GentleRatio) || !MeasureRatio(4.0, HardRatio))
    {
        return false;
    }

    // Normalized tanh at amount 1.5 on a 0.5-amplitude sine predicts a third harmonic near -25 dB
    // relative to the fundamental; at amount 5 the curve is deep into saturation and the
    // prediction is nearer -13 dB.
    TestTrue(FString::Printf(TEXT("drive 0.5 creates a third harmonic (%.1f dB relative)"), GentleRatio),
        GentleRatio > -40.0);
    TestTrue(FString::Printf(TEXT("drive 4 adds more than drive 0.5 (%.1f dB vs %.1f dB)"),
        HardRatio, GentleRatio), (HardRatio - GentleRatio) >= 4.0);

    return true;
}

// =========================================================================================
// E. distort failure direction (§12).
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxDistortUnknownTypeTest,
    "PinWright.audio.fx.distort.UnknownTypeIsAnErrorAndLeavesBufferUntouched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxDistortUnknownTypeTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainATestHelpers;

    const TArray<float> Reference = MakeNoise(4096, /*Seed=*/1234, 0.6f);
    TArray<float> Buffer = Reference;

    FPwSeededRandom Rng(3);
    FPwDspSpan Span = MonoSpan(Buffer);
    FString Code;
    FString Error;
    const bool bOk = PwFxDistort(DistortBag(TEXT("fuzz"), 10.0), TestSampleRate, Rng, Span, Code, Error);

    TestFalse(TEXT("an unrecognised distortion type is rejected"), bOk);
    TestEqual(TEXT("code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestTrue(FString::Printf(TEXT("message names the offending value: %s"), *Error),
        Error.Contains(TEXT("fuzz")));
    TestTrue(FString::Printf(TEXT("message names the valid set: %s"), *Error),
        Error.Contains(TEXT("soft")) && Error.Contains(TEXT("bitcrush")));
    TestEqual(TEXT("buffer is bit-identical (index of first difference)"),
        FirstDifference(Buffer, Reference), NoIndex);

    // A missing required value is the same kind of failure and must behave the same way.
    FPwSynthParams NoDrive;
    SetEnum(NoDrive, TEXT("type"), TEXT("soft"));
    TArray<float> SecondBuffer = Reference;
    FPwDspSpan SecondSpan = MonoSpan(SecondBuffer);
    const bool bSecondOk = PwFxDistort(NoDrive, TestSampleRate, Rng, SecondSpan, Code, Error);
    TestFalse(TEXT("a missing required 'drive' is rejected"), bSecondOk);
    TestEqual(TEXT("code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestEqual(TEXT("buffer is bit-identical (index of first difference)"),
        FirstDifference(SecondBuffer, Reference), NoIndex);

    return true;
}

// =========================================================================================
// F. distort - every shape in the closed set runs on both span shapes and produces finite
//    audio. This is the test that catches FFoldbackDistortion's uninitialised InputGain and
//    FBitCrusher's uninitialised ReciprocalBitDelta if either setter is ever dropped: both would
//    show up as garbage or as an unchanged buffer.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxDistortEveryTypeTest,
    "PinWright.audio.fx.distort.EveryTypeRunsOnMonoAndStereo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxDistortEveryTypeTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainATestHelpers;

    static const TCHAR* Types[] = { TEXT("soft"), TEXT("hard"), TEXT("foldback"), TEXT("bitcrush"), TEXT("tube") };
    const TArray<float> Reference = MakeSine(4096, TestSampleRate, 440.0, 0.8);

    for (const TCHAR* Type : Types)
    {
        TArray<float> Mono = Reference;
        FPwSeededRandom Rng(5);
        FPwDspSpan Span = MonoSpan(Mono);
        FString Code;
        FString Error;
        const bool bOk = PwFxDistort(DistortBag(Type, 50.0), TestSampleRate, Rng, Span, Code, Error);
        TestTrue(FString::Printf(TEXT("distort '%s' mono succeeded (%s: %s)"), Type, *Code, *Error), bOk);

        int32 BadIndex = INDEX_NONE;
        const bool bFinite = AllFinite(Mono, BadIndex);
        TestTrue(FString::Printf(TEXT("distort '%s' mono is finite (first bad index %d)"), Type, BadIndex),
            bFinite);
        TestNotEqual(FString::Printf(TEXT("distort '%s' mono actually changed the buffer"), Type),
            FirstDifference(Mono, Reference), NoIndex);

        TArray<float> Left = Reference;
        TArray<float> Right = Reference;
        FPwDspSpan StereoBus = StereoSpan(Left, Right);
        const bool bStereoOk = PwFxDistort(DistortBag(Type, 50.0), TestSampleRate, Rng, StereoBus, Code, Error);
        TestTrue(FString::Printf(TEXT("distort '%s' stereo succeeded (%s: %s)"), Type, *Code, *Error),
            bStereoOk);
        TestEqual(FString::Printf(TEXT("distort '%s' treats L and R identically"), Type),
            FirstDifference(Left, Right), NoIndex);
        TestEqual(FString::Printf(TEXT("distort '%s' stereo matches the mono result"), Type),
            FirstDifference(Left, Mono), NoIndex);

        // Silence in, silence out. This is the assertion the tube shape's DC correction exists
        // for: its 0.2 bias, left uncorrected, offsets a silent passage by about 0.34.
        TArray<float> Silence;
        Silence.SetNumZeroed(1024);
        FPwDspSpan SilentSpan = MonoSpan(Silence);
        const bool bSilentOk = PwFxDistort(DistortBag(Type, 50.0), TestSampleRate, Rng, SilentSpan, Code, Error);
        TestTrue(FString::Printf(TEXT("distort '%s' on silence succeeded (%s: %s)"), Type, *Code, *Error),
            bSilentOk);
        const float SilentPeak = MaxAbs(Silence, 0, Silence.Num());
        TestTrue(FString::Printf(TEXT("distort '%s' leaves no DC step on silence (peak %.6f)"),
            Type, SilentPeak), SilentPeak < 1.e-6f);
    }

    return true;
}

// =========================================================================================
// G. delay - with no feedback an impulse comes back once, at exactly the requested offset and
//    at exactly the mix level. Both halves of the blend are checked: the dry impulse is still at
//    index 0 scaled by (1 - mix), and nothing else in the buffer is non-zero.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxDelayEchoTest,
    "PinWright.audio.fx.delay.EchoLandsAtTheRightTimeAndLevel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxDelayEchoTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainATestHelpers;

    TArray<float> Buffer;
    Buffer.SetNumZeroed(TestSampleRate);
    Buffer[0] = 1.0f;

    FPwSeededRandom Rng(2);
    FPwDspSpan Span = MonoSpan(Buffer);
    FString Code;
    FString Error;
    const bool bOk = PwFxDelay(DelayBag(100.0, /*Mix=*/0.5), TestSampleRate, Rng, Span, Code, Error);
    TestTrue(FString::Printf(TEXT("PwFxDelay succeeded (%s: %s)"), *Code, *Error), bOk);
    if (!bOk)
    {
        return false;
    }

    // 100 ms at 48 kHz is 4800 samples exactly, so the delay line's fractional read degenerates to
    // an exact tap and the echo is a bit-for-bit copy scaled by the mix.
    const int32 EchoIndex = 4800;
    TestTrue(FString::Printf(TEXT("dry impulse survives at (1 - mix): %.6f"), Buffer[0]),
        FMath::IsNearlyEqual(Buffer[0], 0.5f, 1.e-6f));
    TestTrue(FString::Printf(TEXT("echo at sample %d is at mix level: %.6f"), EchoIndex, Buffer[EchoIndex]),
        FMath::IsNearlyEqual(Buffer[EchoIndex], 0.5f, 1.e-6f));

    // Nothing anywhere else: with feedback 0 there is exactly one repeat.
    float Elsewhere = 0.f;
    for (int32 Index = 0; Index < Buffer.Num(); ++Index)
    {
        if (Index != 0 && Index != EchoIndex)
        {
            Elsewhere = FMath::Max(Elsewhere, FMath::Abs(Buffer[Index]));
        }
    }
    TestTrue(FString::Printf(TEXT("feedback 0 produces no second repeat (worst stray %.3e)"), Elsewhere),
        Elsewhere < 1.e-6f);

    return true;
}

// =========================================================================================
// H. delay - with feedback the repeats keep coming, evenly spaced and decaying.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxDelayFeedbackTest,
    "PinWright.audio.fx.delay.FeedbackProducesDecayingRepeats",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxDelayFeedbackTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainATestHelpers;

    TArray<float> Buffer;
    Buffer.SetNumZeroed(TestSampleRate);
    Buffer[0] = 1.0f;

    FPwSeededRandom Rng(2);
    FPwDspSpan Span = MonoSpan(Buffer);
    FString Code;
    FString Error;
    const bool bOk = PwFxDelay(DelayBag(100.0, /*Mix=*/1.0, /*Feedback=*/0.5), TestSampleRate, Rng, Span, Code, Error);
    TestTrue(FString::Printf(TEXT("PwFxDelay succeeded (%s: %s)"), *Code, *Error), bOk);
    if (!bOk)
    {
        return false;
    }

    // The damping filter sits in the feedback path, so each regeneration re-enters the line one
    // sample after the repeat that produced it and the peaks drift forward by one sample per
    // repeat. That is why the repeats are located by peak-in-window and the assertion is on the
    // SPACING rather than on an absolute index.
    float PreviousPeak = 0.f;
    int32 PreviousIndex = INDEX_NONE;
    for (int32 Repeat = 1; Repeat <= 3; ++Repeat)
    {
        const int32 Center = 4800 * Repeat;
        int32 PeakIndex = INDEX_NONE;
        const float Peak = PeakInWindow(Buffer, Center - 50, Center + 50, PeakIndex);

        TestTrue(FString::Printf(TEXT("repeat %d exists near sample %d (peak %.5f at %d)"),
            Repeat, Center, Peak, PeakIndex), Peak > 1.e-4f);

        if (Repeat > 1)
        {
            const int32 Spacing = PeakIndex - PreviousIndex;
            TestTrue(FString::Printf(TEXT("repeat %d is 4800 samples after the last one (%d)"),
                Repeat, Spacing), FMath::Abs(Spacing - 4800) <= 4);

            const float Ratio = Peak / PreviousPeak;
            // Feedback 0.5 plus one more pass of the 20 kHz damping filter predicts about 0.46.
            TestTrue(FString::Printf(TEXT("repeat %d decayed by a plausible factor (%.3f)"), Repeat, Ratio),
                Ratio > 0.2f && Ratio < 0.8f);
        }
        PreviousPeak = Peak;
        PreviousIndex = PeakIndex;
    }

    return true;
}

// =========================================================================================
// I. delay failure direction (§12) - feedback past the documented ceiling is rejected rather
//    than clamped, and the buffer survives untouched.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxDelayFeedbackCeilingTest,
    "PinWright.audio.fx.delay.FeedbackAboveCeilingIsAnErrorAndLeavesBufferUntouched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxDelayFeedbackCeilingTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainATestHelpers;

    const TArray<float> Reference = MakeNoise(4096, /*Seed=*/77, 0.4f);
    TArray<float> Buffer = Reference;

    FPwSeededRandom Rng(2);
    FPwDspSpan Span = MonoSpan(Buffer);
    FString Code;
    FString Error;
    const bool bOk = PwFxDelay(DelayBag(50.0, 1.0, /*Feedback=*/1.0), TestSampleRate, Rng, Span, Code, Error);

    TestFalse(TEXT("feedback of 1.0 is rejected"), bOk);
    TestEqual(TEXT("code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestTrue(FString::Printf(TEXT("message names the ceiling: %s"), *Error), Error.Contains(TEXT("0.99")));
    TestEqual(TEXT("buffer is bit-identical (index of first difference)"),
        FirstDifference(Buffer, Reference), NoIndex);

    // A zero delay time is the other way the delay line stops meaning anything.
    TArray<float> SecondBuffer = Reference;
    FPwDspSpan SecondSpan = MonoSpan(SecondBuffer);
    const bool bSecondOk = PwFxDelay(DelayBag(0.0, 1.0), TestSampleRate, Rng, SecondSpan, Code, Error);
    TestFalse(TEXT("a zero timeMs is rejected"), bSecondOk);
    TestEqual(TEXT("code is INVALID_PARAMS"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestEqual(TEXT("buffer is bit-identical (index of first difference)"),
        FirstDifference(SecondBuffer, Reference), NoIndex);

    return true;
}

// =========================================================================================
// J. reverb - the tail keeps going long after the dry burst has stopped, and a longer decayMs
//    is measurably longer. The dry buffer is exactly silent in the measurement window, so the
//    energy found there cannot have come from anywhere but the reverb.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxReverbTailTest,
    "PinWright.audio.fx.reverb.TailOutlastsDryAndLongerDecayIsLonger",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxReverbTailTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainATestHelpers;

    // 3 s buffer, 50 ms of tone at the head, silence for the remaining 2.95 s.
    const int32 NumFrames = TestSampleRate * 3;
    const int32 BurstSamples = TestSampleRate / 20;
    const TArray<float> Dry = MakeBurst(NumFrames, TestSampleRate, BurstSamples);

    const int32 WindowFirst = TestSampleRate;               // 1.0 s
    const int32 WindowLast = TestSampleRate * 3 / 2;        // 1.5 s
    TestTrue(TEXT("the dry signal is exactly silent in the measurement window"),
        Rms(Dry, WindowFirst, WindowLast) == 0.0);

    auto RenderTail = [this, &Dry, WindowFirst, WindowLast](double DecayMs, double& OutTailRms) -> bool
    {
        TArray<float> Wet = Dry;
        FPwSeededRandom Rng(13);
        FPwDspSpan Span = MonoSpan(Wet);
        FString Code;
        FString Error;
        if (!PwFxReverb(ReverbBag(DecayMs, /*Mix=*/1.0), TestSampleRate, Rng, Span, Code, Error))
        {
            AddError(FString::Printf(TEXT("PwFxReverb(decayMs=%g) failed: %s: %s"), DecayMs, *Code, *Error));
            return false;
        }
        int32 BadIndex = INDEX_NONE;
        if (!AllFinite(Wet, BadIndex))
        {
            AddError(FString::Printf(TEXT("PwFxReverb(decayMs=%g) produced a non-finite sample at %d"),
                DecayMs, BadIndex));
            return false;
        }
        OutTailRms = Rms(Wet, WindowFirst, WindowLast);
        return true;
    };

    double ShortTail = 0.0;
    double LongTail = 0.0;
    if (!RenderTail(300.0, ShortTail) || !RenderTail(3000.0, LongTail))
    {
        return false;
    }

    // The mapping puts a 3 s RT60 at roughly -20 dB/s of tail, so 1.0-1.5 s after a 50 ms burst is
    // still well above the noise floor; a 300 ms RT60 is past -200 dB by then.
    TestTrue(FString::Printf(TEXT("a 3000 ms decay still has tail at 1.0-1.5 s (RMS %.3e)"), LongTail),
        LongTail > 1.e-5);
    TestTrue(FString::Printf(TEXT("3000 ms outlasts 300 ms by 10x or more (%.3e vs %.3e)"),
        LongTail, ShortTail), LongTail > FMath::Max(ShortTail * 10.0, 1.e-5));

    return true;
}

// =========================================================================================
// K. mix - a mix of 0 is bit-identical to the input, for every effect whose spec table has the
//    parameter. Asserted on both span shapes, because the blend runs per channel.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxMixZeroTest,
    "PinWright.audio.fx.mix.ZeroIsBitIdentical",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxMixZeroTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainATestHelpers;

    const TArray<float> Reference = MakeNoise(8192, /*Seed=*/424242, 0.9f);

    struct FCase
    {
        const TCHAR* Name;
        FPwSynthParams Params;
        FEffectFn Effect;
    };

    // outGainDb, feedback and preDelayMs are all non-neutral here on purpose: none of them may
    // leak into the output once mix is 0.
    const FCase Cases[] = {
        { TEXT("distort"), DistortBag(TEXT("hard"), 60.0, /*Mix=*/0.0, /*OutGainDb=*/12.0), &PwFxDistort },
        { TEXT("delay"),   DelayBag(37.0, /*Mix=*/0.0, /*Feedback=*/0.8),                   &PwFxDelay },
        { TEXT("reverb"),  ReverbBag(4000.0, /*Mix=*/0.0, /*PreDelayMs=*/120.0),            &PwFxReverb },
    };

    for (const FCase& Case : Cases)
    {
        TArray<float> Mono = Reference;
        FPwSeededRandom Rng(17);
        FPwDspSpan Span = MonoSpan(Mono);
        FString Code;
        FString Error;
        const bool bOk = Case.Effect(Case.Params, TestSampleRate, Rng, Span, Code, Error);
        TestTrue(FString::Printf(TEXT("%s at mix 0 succeeded (%s: %s)"), Case.Name, *Code, *Error), bOk);
        TestEqual(FString::Printf(TEXT("%s at mix 0 is bit-identical on a mono span"), Case.Name),
            FirstDifference(Mono, Reference), NoIndex);

        TArray<float> Left = Reference;
        TArray<float> Right = Reference;
        FPwDspSpan StereoBus = StereoSpan(Left, Right);
        const bool bStereoOk = Case.Effect(Case.Params, TestSampleRate, Rng, StereoBus, Code, Error);
        TestTrue(FString::Printf(TEXT("%s at mix 0 succeeded on a stereo span (%s: %s)"),
            Case.Name, *Code, *Error), bStereoOk);
        TestEqual(FString::Printf(TEXT("%s at mix 0 is bit-identical on the left channel"), Case.Name),
            FirstDifference(Left, Reference), NoIndex);
        TestEqual(FString::Printf(TEXT("%s at mix 0 is bit-identical on the right channel"), Case.Name),
            FirstDifference(Right, Reference), NoIndex);
    }

    return true;
}

// =========================================================================================
// L. stability - the schema's most extreme legal settings over a long buffer must stay finite
//    and must not grow without bound.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxStabilityTest,
    "PinWright.audio.fx.stability.ExtremeSettingsStayFinite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxStabilityTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainATestHelpers;

    // Ten seconds is long enough for a 10 ms delay at the schema's maximum feedback to converge:
    // 0.99^1000 is 4.3e-5, so the loop has settled well before the end of the buffer and a growing
    // envelope over the last five seconds means a genuinely divergent recursion rather than
    // build-up.
    const int32 NumFrames = TestSampleRate * 10;
    const TArray<float> Source = MakeNoise(NumFrames, /*Seed=*/31337, 0.5f);

    {
        TArray<float> Buffer = Source;
        FPwSeededRandom Rng(1);
        FPwDspSpan Span = MonoSpan(Buffer);
        FString Code;
        FString Error;
        const bool bOk = PwFxDelay(DelayBag(10.0, 1.0, /*Feedback=*/0.99, /*DampingHz=*/20000.0),
            TestSampleRate, Rng, Span, Code, Error);
        TestTrue(FString::Printf(TEXT("delay at maximum feedback succeeded (%s: %s)"), *Code, *Error), bOk);

        int32 BadIndex = INDEX_NONE;
        const bool bFinite = AllFinite(Buffer, BadIndex);
        TestTrue(FString::Printf(TEXT("delay at maximum feedback has no NaN/Inf (index %d)"), BadIndex),
            bFinite);

        const float Peak = MaxAbs(Buffer, 0, Buffer.Num());
        TestTrue(FString::Printf(TEXT("delay at maximum feedback stays bounded (peak %.3f)"), Peak),
            Peak < 200.f);

        const double Settled = Rms(Buffer, TestSampleRate * 4, TestSampleRate * 5);
        const double Final = Rms(Buffer, TestSampleRate * 9, TestSampleRate * 10);
        TestTrue(FString::Printf(TEXT("delay at maximum feedback is not still growing (%.4f -> %.4f)"),
            Settled, Final), Final <= Settled * 3.0 + UE_DOUBLE_SMALL_NUMBER);
    }

    {
        // Maximum Q at the lowest legal cutoff is where a bad Q -> bandwidth conversion blows up.
        TArray<float> Buffer = Source;
        FPwSeededRandom Rng(1);
        FPwDspSpan Span = MonoSpan(Buffer);
        FString Code;
        FString Error;
        const bool bOk = PwFxFilter(FilterBag(TEXT("bandpass"), 20.0, /*Resonance=*/20.0),
            TestSampleRate, Rng, Span, Code, Error);
        TestTrue(FString::Printf(TEXT("filter at maximum resonance succeeded (%s: %s)"), *Code, *Error), bOk);

        int32 BadIndex = INDEX_NONE;
        const bool bFinite = AllFinite(Buffer, BadIndex);
        TestTrue(FString::Printf(TEXT("filter at maximum resonance has no NaN/Inf (index %d)"), BadIndex),
            bFinite);
        TestTrue(TEXT("filter at maximum resonance stays bounded"),
            MaxAbs(Buffer, 0, Buffer.Num()) < 200.f);
    }

    {
        TArray<float> Buffer = Source;
        FPwSeededRandom Rng(1);
        FPwDspSpan Span = MonoSpan(Buffer);
        FString Code;
        FString Error;
        const bool bOk = PwFxDistort(DistortBag(TEXT("foldback"), /*Drive=*/100.0),
            TestSampleRate, Rng, Span, Code, Error);
        TestTrue(FString::Printf(TEXT("distort at maximum drive succeeded (%s: %s)"), *Code, *Error), bOk);

        int32 BadIndex = INDEX_NONE;
        const bool bFinite = AllFinite(Buffer, BadIndex);
        TestTrue(FString::Printf(TEXT("distort at maximum drive has no NaN/Inf (index %d)"), BadIndex),
            bFinite);
        TestTrue(TEXT("distort at maximum drive stays bounded"),
            MaxAbs(Buffer, 0, Buffer.Num()) < 10.f);
    }

    {
        TArray<float> Buffer = Source;
        FPwSeededRandom Rng(1);
        FPwDspSpan Span = MonoSpan(Buffer);
        FString Code;
        FString Error;
        const bool bOk = PwFxReverb(ReverbBag(/*DecayMs=*/20000.0, 1.0, /*PreDelayMs=*/500.0,
            /*DampingHz=*/20000.0), TestSampleRate, Rng, Span, Code, Error);
        TestTrue(FString::Printf(TEXT("reverb at maximum decay succeeded (%s: %s)"), *Code, *Error), bOk);

        int32 BadIndex = INDEX_NONE;
        const bool bFinite = AllFinite(Buffer, BadIndex);
        TestTrue(FString::Printf(TEXT("reverb at maximum decay has no NaN/Inf (index %d)"), BadIndex),
            bFinite);

        const double Settled = Rms(Buffer, TestSampleRate * 4, TestSampleRate * 5);
        const double Final = Rms(Buffer, TestSampleRate * 9, TestSampleRate * 10);
        TestTrue(FString::Printf(TEXT("reverb at maximum decay is not still growing (%.4f -> %.4f)"),
            Settled, Final), Final <= Settled * 3.0 + UE_DOUBLE_SMALL_NUMBER);
    }

    return true;
}

// =========================================================================================
// M. spans - all four effects run on a mono layer chain and on the stereo master bus, and the
//    stereo case really writes both channels rather than only the left.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxSpanShapesTest,
    "PinWright.audio.fx.spans.MonoAndStereoBothProcess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxSpanShapesTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainATestHelpers;

    const TArray<float> Reference = MakeNoise(TestSampleRate / 2, /*Seed=*/606, 0.5f);

    struct FCase
    {
        const TCHAR* Name;
        FPwSynthParams Params;
        FEffectFn Effect;
    };

    const FCase Cases[] = {
        { TEXT("filter"),  FilterBag(TEXT("lowpass"), 800.0), &PwFxFilter },
        { TEXT("distort"), DistortBag(TEXT("soft"), 8.0),     &PwFxDistort },
        { TEXT("delay"),   DelayBag(80.0, 0.5, 0.3),          &PwFxDelay },
        { TEXT("reverb"),  ReverbBag(1500.0, 0.6),            &PwFxReverb },
    };

    for (const FCase& Case : Cases)
    {
        TArray<float> Mono = Reference;
        FPwSeededRandom Rng(23);
        FPwDspSpan MonoBus = MonoSpan(Mono);
        FString Code;
        FString Error;
        const bool bMonoOk = Case.Effect(Case.Params, TestSampleRate, Rng, MonoBus, Code, Error);
        TestTrue(FString::Printf(TEXT("%s runs on a mono span (%s: %s)"), Case.Name, *Code, *Error), bMonoOk);
        TestNotEqual(FString::Printf(TEXT("%s changed the mono span"), Case.Name),
            FirstDifference(Mono, Reference), NoIndex);

        int32 MonoBadIndex = INDEX_NONE;
        const bool bMonoFinite = AllFinite(Mono, MonoBadIndex);
        TestTrue(FString::Printf(TEXT("%s mono output is finite (index %d)"), Case.Name, MonoBadIndex),
            bMonoFinite);

        TArray<float> Left = Reference;
        TArray<float> Right = Reference;
        FPwDspSpan StereoBus = StereoSpan(Left, Right);
        const bool bStereoOk = Case.Effect(Case.Params, TestSampleRate, Rng, StereoBus, Code, Error);
        TestTrue(FString::Printf(TEXT("%s runs on a stereo span (%s: %s)"), Case.Name, *Code, *Error),
            bStereoOk);
        TestNotEqual(FString::Printf(TEXT("%s changed the left channel"), Case.Name),
            FirstDifference(Left, Reference), NoIndex);
        TestNotEqual(FString::Printf(TEXT("%s changed the right channel"), Case.Name),
            FirstDifference(Right, Reference), NoIndex);

        int32 LeftBadIndex = INDEX_NONE;
        const bool bLeftFinite = AllFinite(Left, LeftBadIndex);
        TestTrue(FString::Printf(TEXT("%s left output is finite (index %d)"), Case.Name, LeftBadIndex),
            bLeftFinite);

        int32 RightBadIndex = INDEX_NONE;
        const bool bRightFinite = AllFinite(Right, RightBadIndex);
        TestTrue(FString::Printf(TEXT("%s right output is finite (index %d)"), Case.Name, RightBadIndex),
            bRightFinite);
    }

    return true;
}

// =========================================================================================
// N. determinism - the same params and the same seed twice give byte-identical output. This is
//    what keeps a candidate render reproducible from its recipe alone.
// =========================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFxDeterminismTest,
    "PinWright.audio.fx.determinism.SameParamsAndSeedProduceIdenticalOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFxDeterminismTest::RunTest(const FString& Parameters)
{
    using namespace PwFxChainATestHelpers;

    const TArray<float> Reference = MakeNoise(TestSampleRate / 2, /*Seed=*/808, 0.5f);

    struct FCase
    {
        const TCHAR* Name;
        FPwSynthParams Params;
        FEffectFn Effect;
    };

    const FCase Cases[] = {
        { TEXT("filter"),  FilterBag(TEXT("peaking"), 2500.0, 4.0, 9.0),  &PwFxFilter },
        { TEXT("distort"), DistortBag(TEXT("bitcrush"), 65.0, 0.7, -3.0), &PwFxDistort },
        { TEXT("delay"),   DelayBag(123.0, 0.45, 0.6, 4000.0),            &PwFxDelay },
        { TEXT("reverb"),  ReverbBag(2200.0, 0.8, 40.0, 6000.0),          &PwFxReverb },
    };

    for (const FCase& Case : Cases)
    {
        TArray<float> First = Reference;
        TArray<float> Second = Reference;
        FString Code;
        FString Error;

        FPwSeededRandom FirstRng(4242);
        FPwDspSpan FirstSpan = MonoSpan(First);
        const bool bFirstOk = Case.Effect(Case.Params, TestSampleRate, FirstRng, FirstSpan, Code, Error);
        TestTrue(FString::Printf(TEXT("%s first pass succeeded (%s: %s)"), Case.Name, *Code, *Error), bFirstOk);

        FPwSeededRandom SecondRng(4242);
        FPwDspSpan SecondSpan = MonoSpan(Second);
        const bool bSecondOk = Case.Effect(Case.Params, TestSampleRate, SecondRng, SecondSpan, Code, Error);
        TestTrue(FString::Printf(TEXT("%s second pass succeeded (%s: %s)"), Case.Name, *Code, *Error), bSecondOk);

        TestEqual(FString::Printf(TEXT("%s is byte-identical across runs"), Case.Name),
            FirstDifference(First, Second), NoIndex);
    }

    return true;
}
