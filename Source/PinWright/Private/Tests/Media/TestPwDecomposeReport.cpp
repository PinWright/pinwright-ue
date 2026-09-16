// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the decomposition orchestrator and its report (AudioGen/PwAudioDecompose.h:
// PwDecomposeBuffer and SerializeDecomposition).
//
// The report is the product: a model that cannot hear learns what a sound is made of from this
// JSON and nothing else. So these tests assert what the report SAYS, not that the pipeline
// returned true, and four families of assertion are load-bearing rather than routine:
//
//   - THREE-WAY CLASSIFICATION. Harmonic, inharmonic and unpitched are asserted together in one
//     test. A two-way test (harmonic vs not) is passed by a classifier that never says
//     "inharmonic" at all, which is precisely the distinction between a string and a bell.
//
//   - BOTH DIRECTIONS (rpc-design.md §6). The hint scores are asserted as ORDERED PAIRS across a
//     click, a tone and a noise bed, so a score that moved the wrong way fails rather than merely
//     looking plausible; the stereo test asserts a widening tail AND a narrowing one, so a
//     one-sided measure cannot pass.
//
//   - ERROR PROPAGATION (§7). A stage that refuses must reach the caller with ITS OWN code and
//     message, because the sentence naming what to change is the whole value of the error.
//
//   - FAILURE DIRECTION AND ORDER (§12, §7). Empty, non-finite and zero-rate inputs each fail
//     with the code that names them, every one of them leaves bMeasured false, and an ALL-NaN
//     buffer is asserted to come back non-finite rather than empty - FMath::Max(0.0, NaN) is 0,
//     so a NaN buffer measures as digital silence under any implementation that tests silence
//     first.
//
// The size gate is the fifth: the summary form is measured with the same pretty writer the
// transport uses and held well under the ~4,250-character wrapped ceiling, so growth trips CI
// rather than a user's response.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioDecompose.h"
#include "Handlers/ErrorCodes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Math/RandomStream.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true.
namespace PwDecomposeReportTestHelpers
{
    constexpr int32 TestSampleRate = 48000;

    /**
     * The response ceiling the summary form is budgeted against, and the gate this suite holds.
     *
     * The wrapped MCP ToolResult carries the payload twice - escaped in content[0].text and
     * verbatim in structuredContent - for roughly 2.35x amplification, so the real ceiling for a
     * bare result is ~4,250 characters rather than the 10,000 the spill threshold suggests
     * (board ticket E-spill-threshold-measured-post-wrap). The gate sits well inside it, matching
     * the precedent TestPwAudioAnalysis.cpp set for SerializeAudioAnalysis.
     */
    constexpr int32 WrappedResponseCeiling = 4250;
    constexpr int32 SummaryBudgetGate = 2500;

    /** ln(1000): the amplitude ratio a -60 dB decay spans, i.e. PwGenModal's T60 convention. */
    constexpr double Ln1000 = 6.907755278982137;

    FPwAudioBuffer MakeSilentBuffer(double Seconds)
    {
        FPwAudioBuffer Buffer;
        Buffer.SampleRate = TestSampleRate;
        Buffer.SetNumFrames(FMath::RoundToInt32(Seconds * TestSampleRate));
        return Buffer;
    }

    /** A quiet NaN from its bit pattern, matching the helper in the sibling decomposition tests. */
    float MakeNaNFloat()
    {
        constexpr uint32 QuietNaNBits = 0x7FC00000u;
        float Value = 0.f;
        FMemory::Memcpy(&Value, &QuietNaNBits, sizeof(Value));
        return Value;
    }

    /** Steady sine with a 20 ms raised-cosine fade-in, so the tone itself contributes no click. */
    void AddSine(TArray<float>& Channel, double Hz, double Amplitude)
    {
        const double AngularStep = 2.0 * UE_DOUBLE_PI * Hz / TestSampleRate;
        const int32 FadeSamples = FMath::Min(Channel.Num(), TestSampleRate / 50);
        for (int32 Index = 0; Index < Channel.Num(); ++Index)
        {
            double Gain = Amplitude;
            if (Index < FadeSamples && FadeSamples > 1)
            {
                const double Phase = UE_DOUBLE_PI * static_cast<double>(Index)
                    / static_cast<double>(FadeSamples);
                Gain *= 0.5 * (1.0 - FMath::Cos(Phase));
            }
            Channel[Index] += static_cast<float>(Gain * FMath::Sin(AngularStep * Index));
        }
    }

    /** A(t) = A0 * exp(-ln(1000) * t / T60) from FirstSample - PwGenModal's own decay law. */
    void AddDecayingSine(TArray<float>& Channel, double Hz, double Amplitude, double T60Ms,
                         int32 FirstSample = 0)
    {
        const double AngularStep = 2.0 * UE_DOUBLE_PI * Hz / TestSampleRate;
        const double MsPerSample = 1000.0 / TestSampleRate;
        for (int32 Index = FMath::Max(0, FirstSample); Index < Channel.Num(); ++Index)
        {
            const int32 Offset = Index - FirstSample;
            const double TimeMs = static_cast<double>(Offset) * MsPerSample;
            const double Envelope = FMath::Exp(-Ln1000 * TimeMs / T60Ms);
            Channel[Index] += static_cast<float>(Amplitude * Envelope * FMath::Sin(AngularStep * Offset));
        }
    }

    /** NumSamples defaults to "the rest of the channel"; the end is computed in 64 bits so that
     *  default cannot overflow FirstSample + NumSamples. */
    void AddNoise(TArray<float>& Channel, double Amplitude, int32 Seed,
                  int32 FirstSample = 0, int32 NumSamples = MAX_int32)
    {
        FRandomStream Stream(Seed);
        const int64 End = static_cast<int64>(FirstSample) + static_cast<int64>(NumSamples);
        const int32 Last = static_cast<int32>(FMath::Min<int64>(Channel.Num(), End));
        for (int32 Index = FMath::Max(0, FirstSample); Index < Last; ++Index)
        {
            Channel[Index] += Stream.FRandRange(static_cast<float>(-Amplitude),
                                                static_cast<float>(Amplitude));
        }
    }

    /**
     * A noise burst exciting three decaying modes - the canonical impact this subsystem exists to
     * take apart and hand back as a modal recipe. The three frequencies are deliberately not
     * harmonically related (ratios 2.81 and 6.28), which is what a struck body sounds like and
     * what makes the pitch classification of this signal a real answer rather than a default.
     *
     * The strike is 50 ms in, not at sample 0, and that is load-bearing: onset detection is
     * spectral FLUX, so a signal that opens at its own maximum never rises and produces no onset
     * at all. A fixture that starts on the transient would be testing nothing.
     */
    FPwAudioBuffer MakeImpact(double Seconds)
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        const int32 Strike = TestSampleRate / 20;
        AddNoise(Buffer.Left, 1.0e-4, 7);
        AddNoise(Buffer.Left, 0.3, 1234, Strike, TestSampleRate / 100);
        AddDecayingSine(Buffer.Left, 400.0, 0.5, 250.0, Strike);
        AddDecayingSine(Buffer.Left, 1123.0, 0.3, 180.0, Strike);
        AddDecayingSine(Buffer.Left, 2510.0, 0.2, 120.0, Strike);
        Buffer.Right = Buffer.Left;
        return Buffer;
    }

    FPwAudioBuffer MakeSteadyTone(double Seconds, double Hz, double Amplitude)
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        AddSine(Buffer.Left, Hz, Amplitude);
        Buffer.Right = Buffer.Left;
        return Buffer;
    }

    FPwAudioBuffer MakeWhiteNoise(double Seconds, double Amplitude, int32 Seed)
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        AddNoise(Buffer.Left, Amplitude, Seed);
        Buffer.Right = Buffer.Left;
        return Buffer;
    }

    /**
     * A 30 ms burst on a quiet noise floor, 50 ms in.
     *
     * Two constants here are load-bearing rather than arbitrary. The floor stops the buffer being
     * exactly zero outside the burst, which would give the peak picker frames with no maximum to
     * threshold against - a numerical corner rather than the temporal concentration this signal
     * exists to exercise. And the burst is 30 ms rather than 3 because PwAnalyzeResidual median-
     * smooths each band envelope over 20 ms: a burst shorter than that smoother is an outlier to
     * it and is removed from the envelope the transient hint is measured on.
     */
    FPwAudioBuffer MakeClick(double Seconds)
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        AddNoise(Buffer.Left, 1.0e-4, 99);
        AddNoise(Buffer.Left, 0.7, 5150, TestSampleRate / 20, TestSampleRate * 3 / 100);
        Buffer.Right = Buffer.Left;
        return Buffer;
    }

    /** Pretty writer, because it is the one the transport actually uses - condensed understates. */
    FString ToResponseJson(const TSharedPtr<FJsonObject>& Object)
    {
        FString Text;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
        FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
        return Text;
    }

    TSharedPtr<FJsonObject> GetObject(const TSharedPtr<FJsonObject>& Root, const TCHAR* Key)
    {
        const TSharedPtr<FJsonObject>* Found = nullptr;
        if (Root.IsValid() && Root->TryGetObjectField(Key, Found) && Found)
        {
            return *Found;
        }
        return nullptr;
    }

    const TArray<TSharedPtr<FJsonValue>>* GetArray(const TSharedPtr<FJsonObject>& Root, const TCHAR* Key)
    {
        const TArray<TSharedPtr<FJsonValue>>* Found = nullptr;
        if (Root.IsValid() && Root->TryGetArrayField(Key, Found))
        {
            return Found;
        }
        return nullptr;
    }

    FString GetString(const TSharedPtr<FJsonObject>& Root, const TCHAR* Key)
    {
        FString Value;
        if (Root.IsValid())
        {
            Root->TryGetStringField(Key, Value);
        }
        return Value;
    }

    double GetNumber(const TSharedPtr<FJsonObject>& Root, const TCHAR* Key, double Fallback)
    {
        double Value = Fallback;
        if (Root.IsValid() && Root->TryGetNumberField(Key, Value))
        {
            return Value;
        }
        return Fallback;
    }

    /** The pitch verdict the report published, or an empty string if it published none. */
    FString PitchInterpretationOf(const FPwDecomposition& Decomposition)
    {
        const TSharedPtr<FJsonObject> Root = SerializeDecomposition(Decomposition, /*bFullDetail*/ false);
        return GetString(GetObject(Root, TEXT("interpretationHints")), TEXT("pitchInterpretation"));
    }

    /** The three hint scores, read back off the serialized report rather than recomputed. */
    struct FHintScores
    {
        double Tonal = -1.0;
        double Noisy = -1.0;
        double Transient = -1.0;
    };

    FHintScores HintScoresOf(const FPwDecomposition& Decomposition)
    {
        const TSharedPtr<FJsonObject> Root = SerializeDecomposition(Decomposition, /*bFullDetail*/ false);
        const TSharedPtr<FJsonObject> Hints = GetObject(Root, TEXT("interpretationHints"));

        FHintScores Scores;
        Scores.Tonal = GetNumber(Hints, TEXT("tonal"), -1.0);
        Scores.Noisy = GetNumber(Hints, TEXT("noisy"), -1.0);
        Scores.Transient = GetNumber(Hints, TEXT("transient"), -1.0);
        return Scores;
    }

    /** True when some mode sits within Tolerance of Expected. Order-independent by design. */
    bool HasModeNear(const TArray<FPwModalFit>& Modes, double ExpectedHz, double ToleranceHz)
    {
        for (const FPwModalFit& Fit : Modes)
        {
            if (Fit.FreqHz > 0.0 && FMath::Abs(Fit.FreqHz - ExpectedHz) <= ToleranceHz)
            {
                return true;
            }
        }
        return false;
    }

    int32 CountFittedModes(const TArray<FPwModalFit>& Modes)
    {
        int32 Count = 0;
        for (const FPwModalFit& Fit : Modes)
        {
            if (Fit.bMeasured && Fit.DecayMs > 0.0)
            {
                ++Count;
            }
        }
        return Count;
    }
}

// =============================================================================================
// An impact: a transient, three modes, and a structure hint that says "excited resonator"
// =============================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeReportImpactTest,
    "PinWright.audio.decompose.report.ImpactYieldsTransientModesAndResonatorStructure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeReportImpactTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeReportTestHelpers;

    const FPwAudioBuffer Buffer = MakeImpact(0.6);

    FPwDecomposition Decomposition;
    FString ErrorCode;
    FString Error;
    const bool bDecomposed = PwDecomposeBuffer(Buffer, FPwDecomposeSettings(), Decomposition,
                                               ErrorCode, Error);
    if (!TestTrue(FString::Printf(TEXT("impact decomposes (%s: %s)"), *ErrorCode, *Error), bDecomposed))
    {
        return false;
    }
    TestTrue(TEXT("the decomposition is measured"), Decomposition.bMeasured);

    // The excitation was a 5 ms noise burst, so at least one transient must be characterised.
    // Zero here would be the false negative that costs a caller most: it is what tells them the
    // recipe needs an exciter at all.
    TestTrue(FString::Printf(TEXT("at least one transient was characterised (%d found)"),
        Decomposition.Transients.Num()), Decomposition.Transients.Num() >= 1);

    // One bin of the reference grid is 23.4 Hz; the parabolic refinement is sub-bin, so a mode
    // more than a bin away from the excited frequency is a tracking failure, not estimator noise.
    TestTrue(TEXT("a mode was recovered near 400 Hz"), HasModeNear(Decomposition.Modes, 400.0, 25.0));
    TestTrue(TEXT("a mode was recovered near 1123 Hz"), HasModeNear(Decomposition.Modes, 1123.0, 25.0));
    TestTrue(TEXT("a mode was recovered near 2510 Hz"), HasModeNear(Decomposition.Modes, 2510.0, 25.0));

    const int32 Fitted = CountFittedModes(Decomposition.Modes);
    TestTrue(FString::Printf(TEXT("at least three modes carry a fitted decay (%d do)"), Fitted),
        Fitted >= 3);

    const TSharedPtr<FJsonObject> Root = SerializeDecomposition(Decomposition, /*bFullDetail*/ false);
    const TSharedPtr<FJsonObject> Hints = GetObject(Root, TEXT("interpretationHints"));
    const FString Structure = GetString(Hints, TEXT("likelyStructure"));
    AddInfo(FString::Printf(TEXT("likelyStructure: %s"), *Structure));

    // The hint must describe an excitation reaching a resonance. It must NOT name a material or
    // an object - "glass", "a dropped mug" - because no spectrogram contains that.
    TestTrue(FString::Printf(TEXT("likelyStructure names an excitation ('%s')"), *Structure),
        Structure.Contains(TEXT("impact")));
    TestTrue(FString::Printf(TEXT("likelyStructure names a resonator ('%s')"), *Structure),
        Structure.Contains(TEXT("resonator")));

    // Both energy shares are measured off the same separation, so they account for all of it.
    const TSharedPtr<FJsonObject> Energy = GetObject(Root, TEXT("energy"));
    const double Harmonic = GetNumber(Energy, TEXT("harmonic"), -1.0);
    const double Percussive = GetNumber(Energy, TEXT("percussive"), -1.0);
    TestTrue(TEXT("harmonic share is a fraction"), Harmonic >= 0.0 && Harmonic <= 1.0);
    TestTrue(TEXT("percussive share is a fraction"), Percussive >= 0.0 && Percussive <= 1.0);
    TestTrue(FString::Printf(TEXT("the two shares account for the separated energy (%.3f + %.3f)"),
        Harmonic, Percussive), FMath::Abs(Harmonic + Percussive - 1.0) < 0.01);

    return true;
}

// =============================================================================================
// Three-way pitch classification. Two-way would let a classifier that never says "inharmonic" pass
// =============================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeReportPitchClassificationTest,
    "PinWright.audio.decompose.report.ClassifiesHarmonicInharmonicAndUnpitched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeReportPitchClassificationTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeReportTestHelpers;

    const FPwDecomposeSettings Settings;
    FString ErrorCode;
    FString Error;

    // ---- a harmonic series: 220 / 440 / 660 -------------------------------------------------
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(0.6);
        AddSine(Buffer.Left, 220.0, 0.40);
        AddSine(Buffer.Left, 440.0, 0.28);
        AddSine(Buffer.Left, 660.0, 0.20);
        Buffer.Right = Buffer.Left;

        FPwDecomposition Decomposition;
        if (!TestTrue(TEXT("the harmonic tone decomposes"),
                PwDecomposeBuffer(Buffer, Settings, Decomposition, ErrorCode, Error)))
        {
            AddError(FString::Printf(TEXT("%s: %s"), *ErrorCode, *Error));
            return false;
        }

        const FString Verdict = PitchInterpretationOf(Decomposition);
        AddInfo(FString::Printf(TEXT("220/440/660 classified as %s"), *Verdict));
        TestEqual(TEXT("an integer-multiple mode set is harmonic"), Verdict, FString(TEXT("harmonic")));
    }

    // ---- a bell-like set: 400 / 1000 / 1800 (ratios 2.5 and 4.5) -----------------------------
    // Every ratio sits exactly half way between two integers, so the set stays inharmonic under
    // every subset of modes the tracker might recover - the classification cannot pass by luck of
    // which partials survived.
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(0.6);
        AddSine(Buffer.Left, 400.0, 0.40);
        AddSine(Buffer.Left, 1000.0, 0.32);
        AddSine(Buffer.Left, 1800.0, 0.24);
        Buffer.Right = Buffer.Left;

        FPwDecomposition Decomposition;
        if (!TestTrue(TEXT("the inharmonic set decomposes"),
                PwDecomposeBuffer(Buffer, Settings, Decomposition, ErrorCode, Error)))
        {
            AddError(FString::Printf(TEXT("%s: %s"), *ErrorCode, *Error));
            return false;
        }

        const FString Verdict = PitchInterpretationOf(Decomposition);
        AddInfo(FString::Printf(TEXT("400/1000/1800 classified as %s"), *Verdict));
        TestEqual(TEXT("a half-integer mode set is inharmonic"), Verdict, FString(TEXT("inharmonic")));

        const TSharedPtr<FJsonObject> Root = SerializeDecomposition(Decomposition, /*bFullDetail*/ false);
        const TSharedPtr<FJsonObject> Hints = GetObject(Root, TEXT("interpretationHints"));
        const double Harmonicity = GetNumber(Hints, TEXT("harmonicity"), -1.0);
        TestTrue(FString::Printf(TEXT("harmonicity is low for an inharmonic set (%.2f)"), Harmonicity),
            Harmonicity >= 0.0 && Harmonicity < 0.6);
    }

    // ---- white noise ------------------------------------------------------------------------
    // Every local maximum of a noise spectrum is a real local maximum, so noise yields a full
    // mode table. Reporting that table as a pitch structure would send an agent hunting for a
    // fundamental that is not there.
    {
        const FPwAudioBuffer Buffer = MakeWhiteNoise(0.6, 0.5, 4242);

        FPwDecomposition Decomposition;
        if (!TestTrue(TEXT("white noise decomposes"),
                PwDecomposeBuffer(Buffer, Settings, Decomposition, ErrorCode, Error)))
        {
            AddError(FString::Printf(TEXT("%s: %s"), *ErrorCode, *Error));
            return false;
        }

        const FString Verdict = PitchInterpretationOf(Decomposition);
        AddInfo(FString::Printf(TEXT("white noise classified as %s"), *Verdict));
        TestEqual(TEXT("white noise is unpitched"), Verdict, FString(TEXT("unpitched")));
    }

    return true;
}

// =============================================================================================
// Hint scores, asserted as ordered pairs across three signals (rpc-design.md §6)
// =============================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeReportHintDirectionTest,
    "PinWright.audio.decompose.report.HintScoresMoveWithSignalCharacter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeReportHintDirectionTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeReportTestHelpers;

    const FPwDecomposeSettings Settings;
    FString ErrorCode;
    FString Error;

    FPwDecomposition ClickDecomposition;
    FPwDecomposition ToneDecomposition;
    FPwDecomposition NoiseDecomposition;

    const FPwAudioBuffer Click = MakeClick(0.6);
    const FPwAudioBuffer Tone = MakeSteadyTone(0.6, 440.0, 0.5);
    const FPwAudioBuffer Noise = MakeWhiteNoise(0.6, 0.5, 777);

    if (!TestTrue(TEXT("the click decomposes"),
            PwDecomposeBuffer(Click, Settings, ClickDecomposition, ErrorCode, Error))
        || !TestTrue(TEXT("the tone decomposes"),
            PwDecomposeBuffer(Tone, Settings, ToneDecomposition, ErrorCode, Error))
        || !TestTrue(TEXT("the noise decomposes"),
            PwDecomposeBuffer(Noise, Settings, NoiseDecomposition, ErrorCode, Error)))
    {
        AddError(FString::Printf(TEXT("%s: %s"), *ErrorCode, *Error));
        return false;
    }

    const FHintScores ClickScores = HintScoresOf(ClickDecomposition);
    const FHintScores ToneScores = HintScoresOf(ToneDecomposition);
    const FHintScores NoiseScores = HintScoresOf(NoiseDecomposition);

    AddInfo(FString::Printf(TEXT("click  tonal=%.2f noisy=%.2f transient=%.2f"),
        ClickScores.Tonal, ClickScores.Noisy, ClickScores.Transient));
    AddInfo(FString::Printf(TEXT("tone   tonal=%.2f noisy=%.2f transient=%.2f"),
        ToneScores.Tonal, ToneScores.Noisy, ToneScores.Transient));
    AddInfo(FString::Printf(TEXT("noise  tonal=%.2f noisy=%.2f transient=%.2f"),
        NoiseScores.Tonal, NoiseScores.Noisy, NoiseScores.Transient));

    // Every score must have been published at all - a missing field reads back as -1 here.
    TestTrue(TEXT("every hint score was published"),
        ClickScores.Tonal >= 0.0 && ToneScores.Tonal >= 0.0 && NoiseScores.Tonal >= 0.0);

    // Paired, in both directions, so a score that cannot tell two of the three apart fails.
    TestTrue(FString::Printf(TEXT("tonal: tone (%.2f) over noise (%.2f)"),
        ToneScores.Tonal, NoiseScores.Tonal), ToneScores.Tonal > NoiseScores.Tonal);
    TestTrue(FString::Printf(TEXT("tonal: tone (%.2f) over click (%.2f)"),
        ToneScores.Tonal, ClickScores.Tonal), ToneScores.Tonal > ClickScores.Tonal);

    TestTrue(FString::Printf(TEXT("noisy: noise (%.2f) over tone (%.2f)"),
        NoiseScores.Noisy, ToneScores.Noisy), NoiseScores.Noisy > ToneScores.Noisy);
    TestTrue(FString::Printf(TEXT("noisy: noise (%.2f) over click (%.2f)"),
        NoiseScores.Noisy, ClickScores.Noisy), NoiseScores.Noisy > ClickScores.Noisy);

    TestTrue(FString::Printf(TEXT("transient: click (%.2f) over tone (%.2f)"),
        ClickScores.Transient, ToneScores.Transient), ClickScores.Transient > ToneScores.Transient);
    TestTrue(FString::Printf(TEXT("transient: click (%.2f) over noise (%.2f)"),
        ClickScores.Transient, NoiseScores.Transient), ClickScores.Transient > NoiseScores.Transient);

    // The hints must be fenced and labelled, not mixed in with the measurements.
    const TSharedPtr<FJsonObject> Root = SerializeDecomposition(NoiseDecomposition, /*bFullDetail*/ false);
    const TSharedPtr<FJsonObject> Hints = GetObject(Root, TEXT("interpretationHints"));
    TestTrue(TEXT("the hints live in their own block"), Hints.IsValid());
    TestFalse(TEXT("the hint block carries its own disclaimer"),
        GetString(Hints, TEXT("note")).IsEmpty());
    TestFalse(TEXT("tonal is not published beside the measurements"),
        Root->HasField(TEXT("tonal")));

    return true;
}

// =============================================================================================
// Size discipline: the summary fits the wrapped ceiling, the full form carries strictly more
// =============================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeReportBudgetTest,
    "PinWright.audio.decompose.report.SummaryFitsTheBudgetAndFullDetailCarriesMore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeReportBudgetTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeReportTestHelpers;

    // A deliberately rich signal: an excitation, eight decaying modes, and a decorrelated tail in
    // the right channel, so every capped array in the report is over its cap.
    FPwAudioBuffer Buffer = MakeSilentBuffer(0.7);
    const int32 Strike = TestSampleRate / 20;
    AddNoise(Buffer.Left, 1.0e-4, 31336);
    AddNoise(Buffer.Left, 0.4, 31337, Strike, TestSampleRate / 100);
    AddNoise(Buffer.Left, 0.3, 31338, TestSampleRate / 4, TestSampleRate / 100);
    const double Frequencies[] = { 190.0, 431.0, 733.0, 1187.0, 1699.0, 2311.0, 3067.0, 4211.0 };
    const double Decays[] = { 400.0, 350.0, 300.0, 260.0, 220.0, 190.0, 160.0, 130.0 };
    for (int32 Index = 0; Index < 8; ++Index)
    {
        AddDecayingSine(Buffer.Left, Frequencies[Index], 0.25 - 0.02 * Index, Decays[Index], Strike);
    }
    Buffer.Right = Buffer.Left;
    AddNoise(Buffer.Right, 0.05, 4711, Buffer.NumFrames() / 2);

    FPwDecomposition Decomposition;
    FString ErrorCode;
    FString Error;
    if (!TestTrue(TEXT("the rich signal decomposes"),
            PwDecomposeBuffer(Buffer, FPwDecomposeSettings(), Decomposition, ErrorCode, Error)))
    {
        AddError(FString::Printf(TEXT("%s: %s"), *ErrorCode, *Error));
        return false;
    }

    const TSharedPtr<FJsonObject> SummaryRoot = SerializeDecomposition(Decomposition, /*bFullDetail*/ false);
    const TSharedPtr<FJsonObject> FullRoot = SerializeDecomposition(Decomposition, /*bFullDetail*/ true);
    const FString Summary = ToResponseJson(SummaryRoot);
    const FString Full = ToResponseJson(FullRoot);

    AddInfo(FString::Printf(TEXT("summary form is %d chars (gate %d, wrapped ceiling ~%d); ")
                            TEXT("full form is %d chars"),
        Summary.Len(), SummaryBudgetGate, WrappedResponseCeiling, Full.Len()));

    TestTrue(FString::Printf(TEXT("summary form is %d chars, under the %d gate"),
        Summary.Len(), SummaryBudgetGate), Summary.Len() < SummaryBudgetGate);
    TestTrue(FString::Printf(TEXT("summary form is %d chars, under the ~%d wrapped ceiling"),
        Summary.Len(), WrappedResponseCeiling), Summary.Len() < WrappedResponseCeiling);
    TestTrue(FString::Printf(TEXT("full form (%d) carries strictly more than the summary (%d)"),
        Full.Len(), Summary.Len()), Full.Len() > Summary.Len());

    // Caps bite, and every one that bit is reported: a capped list published silently reads as
    // the whole list (rpc-design.md §1).
    const TArray<TSharedPtr<FJsonValue>>* SummaryModes = GetArray(SummaryRoot, TEXT("modes"));
    const TArray<TSharedPtr<FJsonValue>>* FullModes = GetArray(FullRoot, TEXT("modes"));
    const TArray<TSharedPtr<FJsonValue>>* SummaryBands = GetArray(SummaryRoot, TEXT("residual"));
    const TArray<TSharedPtr<FJsonValue>>* FullBands = GetArray(FullRoot, TEXT("residual"));

    if (TestTrue(TEXT("both forms carry a modes table and residual bands"),
            SummaryModes && FullModes && SummaryBands && FullBands))
    {
        TestTrue(FString::Printf(TEXT("summary modes are capped (%d of %d)"),
            SummaryModes->Num(), FullModes->Num()), SummaryModes->Num() <= 5);
        TestTrue(FString::Printf(TEXT("full detail keeps every mode (%d)"), FullModes->Num()),
            FullModes->Num() > SummaryModes->Num());
        TestTrue(FString::Printf(TEXT("summary bands are capped (%d of %d)"),
            SummaryBands->Num(), FullBands->Num()), SummaryBands->Num() <= 3);
        TestTrue(TEXT("full detail keeps every residual band"),
            FullBands->Num() > SummaryBands->Num());
    }

    const TSharedPtr<FJsonObject> Truncated = GetObject(SummaryRoot, TEXT("truncated"));
    if (TestTrue(TEXT("the summary reports what it capped"), Truncated.IsValid()))
    {
        TestFalse(TEXT("the mode cap is named with its counts"),
            GetString(Truncated, TEXT("modes")).IsEmpty());
        TestFalse(TEXT("the residual band cap is named with its counts"),
            GetString(Truncated, TEXT("residualBands")).IsEmpty());
    }
    TestFalse(TEXT("the full form reports no truncation"),
        GetObject(FullRoot, TEXT("truncated")).IsValid());

    // The stereo tail was decorrelated on purpose, so the behaviour block must be there and the
    // widening must be the measured one - not a default.
    const TSharedPtr<FJsonObject> Stereo = GetObject(SummaryRoot, TEXT("stereo"));
    if (TestTrue(TEXT("stereo behaviour was measured"), Stereo.IsValid()))
    {
        bool bWidens = false;
        Stereo->TryGetBoolField(TEXT("widthIncreasesOverTime"), bWidens);
        TestTrue(TEXT("the decorrelated tail is reported as widening"), bWidens);
    }

    return true;
}

// =============================================================================================
// A stage that refuses reaches the caller with its own code and its own sentence
// =============================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeReportStagePropagationTest,
    "PinWright.audio.decompose.report.StageFailurePropagatesItsOwnErrorCode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeReportStagePropagationTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeReportTestHelpers;

    // ---- a buffer that is above the sample-domain silence floor and silent in the spectrogram --
    // One sample at 1e-8 clears the 1e-9 buffer floor, but a single impulse spread over a
    // 2048-sample window normalizes to about 2e-11 per bin, which is under the same floor the
    // spectrogram stages test. The refusal therefore comes from a stage, and it must arrive with
    // that stage's own words rather than as a generic decomposition failure.
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(0.5);
        Buffer.Left[1000] = 1.0e-8f;
        Buffer.Right[1000] = 1.0e-8f;

        FPwDecomposition Decomposition;
        FString ErrorCode;
        FString Error;
        const bool bDecomposed = PwDecomposeBuffer(Buffer, FPwDecomposeSettings(), Decomposition,
                                                   ErrorCode, Error);

        TestFalse(TEXT("a spectrogram-silent buffer is refused"), bDecomposed);
        TestFalse(TEXT("nothing is measured after a stage refusal"), Decomposition.bMeasured);
        TestEqual(TEXT("the stage's own code is propagated, not a generic one"),
            ErrorCode, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        AddInfo(FString::Printf(TEXT("propagated: %s / %s"), *ErrorCode, *Error));
        TestTrue(FString::Printf(TEXT("the failing stage is named ('%s')"), *Error),
            Error.Contains(TEXT("Decompose(")));
        TestTrue(FString::Printf(TEXT("the stage's own message survives ('%s')"), *Error),
            Error.Contains(TEXT("spectrogram")));
        TestEqual(TEXT("the decomposition carries the same reason"),
            Decomposition.UnmeasuredReason, Error);
    }

    // ---- a settings value only the residual stage validates ---------------------------------
    {
        const FPwAudioBuffer Buffer = MakeSteadyTone(0.5, 440.0, 0.5);
        FPwDecomposeSettings Settings;
        Settings.ResidualBands = 0;

        FPwDecomposition Decomposition;
        FString ErrorCode;
        FString Error;
        const bool bDecomposed = PwDecomposeBuffer(Buffer, Settings, Decomposition, ErrorCode, Error);

        TestFalse(TEXT("zero residual bands is refused"), bDecomposed);
        TestFalse(TEXT("nothing is measured after the residual stage refuses"), Decomposition.bMeasured);
        TestEqual(TEXT("the residual stage's code is propagated"),
            ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(FString::Printf(TEXT("the residual stage is named ('%s')"), *Error),
            Error.Contains(TEXT("Decompose(residual)")));
        TestTrue(FString::Printf(TEXT("the offending setting is named ('%s')"), *Error),
            Error.Contains(TEXT("residualBands")));
    }

    return true;
}

// =============================================================================================
// Cost is refused, and the refusal names the limit
// =============================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeReportDurationCapTest,
    "PinWright.audio.decompose.report.MaxDurationMsIsRefusedAndNamesTheLimit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeReportDurationCapTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeReportTestHelpers;

    const FPwAudioBuffer Buffer = MakeSteadyTone(0.5, 440.0, 0.5);

    FPwDecomposeSettings Settings;
    Settings.MaxDurationMs = 100;

    FPwDecomposition Decomposition;
    FString ErrorCode;
    FString Error;
    const bool bDecomposed = PwDecomposeBuffer(Buffer, Settings, Decomposition, ErrorCode, Error);

    TestFalse(TEXT("an over-length buffer is refused, not cropped"), bDecomposed);
    TestFalse(TEXT("no decomposition is published for a refused buffer"), Decomposition.bMeasured);
    TestEqual(TEXT("the refusal is a parameter error"), ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));

    AddInfo(FString::Printf(TEXT("refusal: %s"), *Error));
    TestTrue(FString::Printf(TEXT("the limit is named ('%s')"), *Error),
        Error.Contains(TEXT("100 ms")));
    TestTrue(FString::Printf(TEXT("the measured duration is named ('%s')"), *Error),
        Error.Contains(TEXT("500.0 ms")));
    TestTrue(FString::Printf(TEXT("the setting is named ('%s')"), *Error),
        Error.Contains(TEXT("maxDurationMs")));

    // The whole point of refusing: nothing that looks like an analysis of the first 100 ms.
    TestEqual(TEXT("no transients were published"), Decomposition.Transients.Num(), 0);
    TestEqual(TEXT("no modes were published"), Decomposition.Modes.Num(), 0);
    TestEqual(TEXT("no residual bands were published"), Decomposition.Residual.Num(), 0);

    // The refusal is on the decomposition too, not only in the out-parameters: that is what
    // SerializeDecomposition publishes and what PwCompareBuffers reads back.
    TestFalse(TEXT("the decomposition carries the reason it is absent"),
        Decomposition.UnmeasuredReason.IsEmpty());
    TestEqual(TEXT("the serialized report names the same reason"),
        GetString(GetObject(SerializeDecomposition(Decomposition, /*bFullDetail*/ false),
            TEXT("unmeasured")), TEXT("decomposition")),
        Decomposition.UnmeasuredReason);

    return true;
}

// =============================================================================================
// Failure direction and failure ORDER (rpc-design.md §7, §12)
// =============================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeReportRejectsBadInputTest,
    "PinWright.audio.decompose.report.RejectsEmptyNonFiniteSilentAndZeroRateInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeReportRejectsBadInputTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeReportTestHelpers;

    const FPwDecomposeSettings Settings;
    FString ErrorCode;
    FString Error;

    // ---- empty ------------------------------------------------------------------------------
    {
        FPwAudioBuffer Buffer;
        Buffer.SampleRate = TestSampleRate;

        FPwDecomposition Decomposition;
        TestFalse(TEXT("an empty buffer is refused"),
            PwDecomposeBuffer(Buffer, Settings, Decomposition, ErrorCode, Error));
        TestFalse(TEXT("an empty buffer measures nothing"), Decomposition.bMeasured);
        TestEqual(TEXT("an empty buffer is named as empty"),
            ErrorCode, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
    }

    // ---- ALL non-finite: the ordering trap ---------------------------------------------------
    // FMath::Max(0.0, NaN) is 0, so a buffer of NaN measures as digital silence under any
    // implementation that tests the silence floor before scanning for non-finite values. This
    // assertion is what makes that ordering a contract rather than an accident.
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(0.2);
        const float NaNValue = MakeNaNFloat();
        for (int32 Index = 0; Index < Buffer.NumFrames(); ++Index)
        {
            Buffer.Left[Index] = NaNValue;
            Buffer.Right[Index] = NaNValue;
        }

        FPwDecomposition Decomposition;
        TestFalse(TEXT("an all-NaN buffer is refused"),
            PwDecomposeBuffer(Buffer, Settings, Decomposition, ErrorCode, Error));
        TestFalse(TEXT("an all-NaN buffer measures nothing"), Decomposition.bMeasured);
        TestEqual(TEXT("an all-NaN buffer is named non-finite, NOT silent"),
            ErrorCode, FString(ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES));
    }

    // ---- one non-finite sample in real audio -------------------------------------------------
    {
        FPwAudioBuffer Buffer = MakeSteadyTone(0.3, 440.0, 0.5);
        Buffer.Left[5000] = MakeNaNFloat();

        FPwDecomposition Decomposition;
        TestFalse(TEXT("a single NaN is enough to refuse"),
            PwDecomposeBuffer(Buffer, Settings, Decomposition, ErrorCode, Error));
        TestEqual(TEXT("a single NaN is named non-finite"),
            ErrorCode, FString(ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES));
        TestTrue(FString::Printf(TEXT("the offending frame is named ('%s')"), *Error),
            Error.Contains(TEXT("5000")));
    }

    // ---- digital silence ---------------------------------------------------------------------
    {
        const FPwAudioBuffer Buffer = MakeSilentBuffer(0.3);

        FPwDecomposition Decomposition;
        TestFalse(TEXT("a silent buffer is refused"),
            PwDecomposeBuffer(Buffer, Settings, Decomposition, ErrorCode, Error));
        TestFalse(TEXT("a silent buffer measures nothing"), Decomposition.bMeasured);
        TestEqual(TEXT("a silent buffer is named as empty of signal"),
            ErrorCode, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        TestTrue(FString::Printf(TEXT("the message says silence, not emptiness ('%s')"), *Error),
            Error.Contains(TEXT("silence")));
    }

    // ---- zero sample rate ---------------------------------------------------------------------
    {
        FPwAudioBuffer Buffer = MakeSteadyTone(0.3, 440.0, 0.5);
        Buffer.SampleRate = 0;

        FPwDecomposition Decomposition;
        TestFalse(TEXT("a zero sample rate is refused"),
            PwDecomposeBuffer(Buffer, Settings, Decomposition, ErrorCode, Error));
        TestFalse(TEXT("a zero sample rate measures nothing"), Decomposition.bMeasured);
        TestEqual(TEXT("a zero sample rate is a parameter error"),
            ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
    }

    return true;
}

// =============================================================================================
// Stereo behaviour, measured in both directions and absent when there is nothing to measure
// =============================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeReportStereoBehaviourTest,
    "PinWright.audio.decompose.report.StereoTailWideningIsMeasuredInBothDirections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeReportStereoBehaviourTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeReportTestHelpers;

    const FPwDecomposeSettings Settings;
    FString ErrorCode;
    FString Error;
    const double Seconds = 0.4;

    // ---- correlated head, decorrelated tail: the image opens up ------------------------------
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        const int32 Half = Buffer.NumFrames() / 2;
        AddNoise(Buffer.Left, 0.5, 21, 0, Half);
        Buffer.Right = Buffer.Left;
        AddNoise(Buffer.Left, 0.5, 22, Half);
        AddNoise(Buffer.Right, 0.5, 23, Half);

        FPwDecomposition Decomposition;
        if (!TestTrue(TEXT("the widening signal decomposes"),
                PwDecomposeBuffer(Buffer, Settings, Decomposition, ErrorCode, Error)))
        {
            AddError(FString::Printf(TEXT("%s: %s"), *ErrorCode, *Error));
            return false;
        }

        TestTrue(TEXT("stereo behaviour was measured"), Decomposition.Stereo.bMeasured);
        AddInfo(FString::Printf(TEXT("widening: initial=%.3f tail=%.3f"),
            Decomposition.Stereo.InitialCorrelation, Decomposition.Stereo.TailCorrelation));
        TestTrue(TEXT("the head is correlated"), Decomposition.Stereo.InitialCorrelation > 0.9);
        TestTrue(TEXT("the tail is not"), Decomposition.Stereo.TailCorrelation < 0.5);
        TestTrue(TEXT("the image is reported as widening"),
            Decomposition.Stereo.bWidthIncreasesOverTime);
    }

    // ---- the mirror case: decorrelated head, correlated tail ---------------------------------
    // The same two numbers must be able to say the opposite thing, or the measure is one-sided.
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        const int32 Half = Buffer.NumFrames() / 2;
        AddNoise(Buffer.Left, 0.5, 31, 0, Half);
        AddNoise(Buffer.Right, 0.5, 32, 0, Half);
        FPwAudioBuffer Shared = MakeSilentBuffer(Seconds);
        AddNoise(Shared.Left, 0.5, 33, Half);
        for (int32 Index = Half; Index < Buffer.NumFrames(); ++Index)
        {
            Buffer.Left[Index] = Shared.Left[Index];
            Buffer.Right[Index] = Shared.Left[Index];
        }

        FPwDecomposition Decomposition;
        if (!TestTrue(TEXT("the narrowing signal decomposes"),
                PwDecomposeBuffer(Buffer, Settings, Decomposition, ErrorCode, Error)))
        {
            AddError(FString::Printf(TEXT("%s: %s"), *ErrorCode, *Error));
            return false;
        }

        TestTrue(TEXT("stereo behaviour was measured"), Decomposition.Stereo.bMeasured);
        AddInfo(FString::Printf(TEXT("narrowing: initial=%.3f tail=%.3f"),
            Decomposition.Stereo.InitialCorrelation, Decomposition.Stereo.TailCorrelation));
        TestTrue(TEXT("the tail is more correlated than the head"),
            Decomposition.Stereo.TailCorrelation > Decomposition.Stereo.InitialCorrelation);
        TestFalse(TEXT("a narrowing image is not reported as widening"),
            Decomposition.Stereo.bWidthIncreasesOverTime);
    }

    // ---- dual mono: absent, not "perfectly correlated" ---------------------------------------
    {
        const FPwAudioBuffer Buffer = MakeWhiteNoise(Seconds, 0.5, 41);

        FPwDecomposition Decomposition;
        if (!TestTrue(TEXT("the dual-mono signal decomposes"),
                PwDecomposeBuffer(Buffer, Settings, Decomposition, ErrorCode, Error)))
        {
            AddError(FString::Printf(TEXT("%s: %s"), *ErrorCode, *Error));
            return false;
        }

        TestFalse(TEXT("dual mono measures no stereo behaviour"), Decomposition.Stereo.bMeasured);

        const TSharedPtr<FJsonObject> Root = SerializeDecomposition(Decomposition, /*bFullDetail*/ false);
        TestFalse(TEXT("no stereo block is published for dual mono"),
            GetObject(Root, TEXT("stereo")).IsValid());
        TestFalse(TEXT("no channel count is claimed for dual mono"),
            Root->HasField(TEXT("channels")));
        TestFalse(TEXT("the absence is named"),
            GetString(GetObject(Root, TEXT("unmeasured")), TEXT("stereo")).IsEmpty());
    }

    return true;
}

// =============================================================================================
// An unmeasured decomposition is absent from the report, not a report full of zeros
// =============================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwDecomposeReportUnmeasuredTest,
    "PinWright.audio.decompose.report.UnmeasuredDecompositionSerializesAsAbsence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwDecomposeReportUnmeasuredTest::RunTest(const FString& Parameters)
{
    using namespace PwDecomposeReportTestHelpers;

    FPwDecomposition Decomposition;
    Decomposition.UnmeasuredReason = TEXT("the buffer was refused as digital silence");

    const TSharedPtr<FJsonObject> Root = SerializeDecomposition(Decomposition, /*bFullDetail*/ false);
    if (!TestTrue(TEXT("serialization produced a report"), Root.IsValid()))
    {
        return false;
    }

    TestEqual(TEXT("the reason is published"),
        GetString(GetObject(Root, TEXT("unmeasured")), TEXT("decomposition")),
        Decomposition.UnmeasuredReason);

    // Absence, not zeros: an empty modes array beside a duration of 0 would read as "this sound
    // has no modes and lasts no time", which is a measurement nobody made.
    TestFalse(TEXT("no duration is published"), Root->HasField(TEXT("durationMs")));
    TestFalse(TEXT("no energy split is published"), Root->HasField(TEXT("energy")));
    TestFalse(TEXT("no modes table is published"), Root->HasField(TEXT("modes")));
    TestFalse(TEXT("no events array is published"), Root->HasField(TEXT("events")));
    TestFalse(TEXT("no residual bands are published"), Root->HasField(TEXT("residual")));
    TestFalse(TEXT("no interpretation hints are published"),
        Root->HasField(TEXT("interpretationHints")));

    return true;
}
