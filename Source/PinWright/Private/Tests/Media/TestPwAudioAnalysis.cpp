// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the descriptor metrics (AudioGen/PwAudioAnalysis.h).
//
// This chunk has exact ground truth and the assertions lean on it entirely: a sine's centroid,
// crest factor and LUFS are all closed-form, white noise's flatness converges to 1 by
// construction, and an impulse's attack is zero by definition. "It returned true" is asserted
// nowhere on its own.
//
// Three families of assertion are load-bearing rather than routine:
//   - ABSENCE (rpc-design.md §1). The silence test asserts that the loudness and brightness
//     blocks are MISSING from the serialized report, not that they are small. A test that only
//     checked "digitalSilence == true" would still pass if the analyzer emitted -90 LUFS beside
//     it, which is the exact defect the design exists to prevent.
//   - ORDERING (§7). A buffer of NaN measures as digital silence under any implementation that
//     tests the silence floor first, because NaN compares false against every threshold. The
//     non-finite test asserts the specific error code for exactly that buffer.
//   - BOTH DIRECTIONS (§6). Rising and falling chirps, positive and negative DC, in-phase and
//     anti-phase stereo - each pair is asserted to produce numbers of opposite sign, so a
//     metric that reported an unsigned magnitude would fail rather than look symmetric.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioAnalysis.h"
#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioFeatures.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/ErrorCodes.h"

#include "Dom/JsonObject.h"
#include "Math/RandomStream.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true.
namespace PwAudioAnalysisTestHelpers
{
    constexpr int32 TestSampleRate = 48000;

    /** The response ceiling the summary form is budgeted against, and the gate this suite holds. */
    constexpr int32 WrappedResponseCeiling = 4250;
    constexpr int32 SummaryBudgetGate = 2500;

    FPwAudioBuffer MakeSilentBuffer(double Seconds)
    {
        FPwAudioBuffer Buffer;
        Buffer.SampleRate = TestSampleRate;
        Buffer.SetNumFrames(FMath::RoundToInt32(Seconds * TestSampleRate));
        return Buffer;
    }

    void AddSine(TArray<float>& Channel, double Hz, double Amplitude)
    {
        const double AngularStep = 2.0 * UE_DOUBLE_PI * Hz / TestSampleRate;
        for (int32 Index = 0; Index < Channel.Num(); ++Index)
        {
            Channel[Index] += static_cast<float>(Amplitude * FMath::Sin(AngularStep * Index));
        }
    }

    /** Same tone in both channels: correlation 1, width 0, and a mono downmix of full amplitude. */
    FPwAudioBuffer MakeStereoSine(double Seconds, double Hz, double Amplitude)
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        AddSine(Buffer.Left, Hz, Amplitude);
        AddSine(Buffer.Right, Hz, Amplitude);
        return Buffer;
    }

    /** Identical noise in both channels, so the mono downmix is the noise itself. */
    FPwAudioBuffer MakeWhiteNoise(double Seconds, double Amplitude, int32 Seed)
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        FRandomStream Stream(Seed);
        for (int32 Index = 0; Index < Buffer.NumFrames(); ++Index)
        {
            const float Sample = Stream.FRandRange(static_cast<float>(-Amplitude),
                                                   static_cast<float>(Amplitude));
            Buffer.Left[Index] = Sample;
            Buffer.Right[Index] = Sample;
        }
        return Buffer;
    }

    /**
     * A single non-zero sample at AtMs. Deliberately not at sample 0: an impulse in the very
     * first analysis frame has no previous frame to rise from, so onset detection would have
     * nothing to measure, and placing it late also proves attack is measured from the ONSET
     * rather than from the start of the buffer.
     */
    FPwAudioBuffer MakeImpulse(double Seconds, double AtMs, double Amplitude)
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        const int32 Index = FMath::RoundToInt32(AtMs * TestSampleRate / 1000.0);
        if (Buffer.Left.IsValidIndex(Index))
        {
            Buffer.Left[Index] = static_cast<float>(Amplitude);
            Buffer.Right[Index] = static_cast<float>(Amplitude);
        }
        return Buffer;
    }

    /** Linear chirp: f(t) = StartHz + (EndHz - StartHz) * t / T, phase integrated exactly. */
    FPwAudioBuffer MakeChirp(double Seconds, double StartHz, double EndHz, double Amplitude)
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        const double Rate = (EndHz - StartHz) / Seconds;
        for (int32 Index = 0; Index < Buffer.NumFrames(); ++Index)
        {
            const double Time = static_cast<double>(Index) / TestSampleRate;
            const double Phase = 2.0 * UE_DOUBLE_PI * (StartHz * Time + 0.5 * Rate * Time * Time);
            const float Sample = static_cast<float>(Amplitude * FMath::Sin(Phase));
            Buffer.Left[Index] = Sample;
            Buffer.Right[Index] = Sample;
        }
        return Buffer;
    }

    /**
     * An exponentially decaying tone with slightly different channel gains. Every one of the
     * fourteen EPwSynthMetric values is defined for this signal: it decays (so decayMs exists),
     * it is pitched (so the pitch gate opens) and its two channels each carry variance (so the
     * correlation is defined).
     */
    FPwAudioBuffer MakeDecayingTone(double Seconds, double Hz, double DecayPerSecond)
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        const double AngularStep = 2.0 * UE_DOUBLE_PI * Hz / TestSampleRate;
        for (int32 Index = 0; Index < Buffer.NumFrames(); ++Index)
        {
            const double Time = static_cast<double>(Index) / TestSampleRate;
            const double Wave = FMath::Sin(AngularStep * Index) * FMath::Exp(-DecayPerSecond * Time);
            Buffer.Left[Index] = static_cast<float>(0.6 * Wave);
            Buffer.Right[Index] = static_cast<float>(0.5 * Wave);
        }
        return Buffer;
    }

    /**
     * NaN and infinity from their IEEE-754 bit patterns rather than from sqrt(-1) or an
     * overflowing multiply: a constant-folding compiler is entitled to diagnose or reshape those,
     * and this suite needs the exact bit pattern a real DSP overflow produces.
     */
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

    /**
     * Serializes with the SAME writer the transport uses for a response - JsonRpc::Serialize and
     * HttpResponseSpill both take the default TJsonWriterFactory<>, i.e. the PRETTY policy, whose
     * per-field newline, indent and post-colon space add roughly 20% over a condensed encoding.
     * Measuring the condensed form would understate the budget by exactly that much (§4: measure
     * what the gate measures, not a proxy for it).
     */
    FString ToResponseJson(const TSharedPtr<FJsonObject>& Object)
    {
        FString Text;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
        FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
        return Text;
    }

    bool HasObject(const TSharedPtr<FJsonObject>& Root, const TCHAR* Key)
    {
        return Root.IsValid() && Root->HasTypedField<EJson::Object>(Key);
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

    /** Every family reports unmeasured - the state PwAnalyzeBuffer must leave behind on failure. */
    bool AllFamiliesUnmeasured(const FPwAudioAnalysis& In)
    {
        return !In.Technical.State.bMeasured
            && !In.Envelope.State.bMeasured
            && !In.Loudness.State.bMeasured
            && !In.Spectral.State.bMeasured
            && !In.Pitch.State.bMeasured
            && !In.Stereo.State.bMeasured;
    }

    /** The EPwSynthMetric names a target could actually be scored against for this report. */
    TSet<FString> MeasurableMetrics(const FPwAudioAnalysis& In)
    {
        TSet<FString> Names;
        for (uint8 Index = 0; Index < static_cast<uint8>(EPwSynthMetric::Count); ++Index)
        {
            const EPwSynthMetric Metric = static_cast<EPwSynthMetric>(Index);
            double Value = 0.0;
            if (PwGetAudioAnalysisMetric(In, Metric, Value))
            {
                Names.Add(PwSynthMetricToString(Metric));
            }
        }
        return Names;
    }
}

// =============================================================================================
// A. 1 kHz sine: every closed-form number at once.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioAnalysisSineGroundTruthTest,
    "PinWright.audio.analysis.SineGroundTruth",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioAnalysisSineGroundTruthTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisTestHelpers;

    // Amplitude 0.5, not 1.0: at 48 kHz a 1 kHz sine samples exactly on sin(pi/2), so a
    // full-scale tone genuinely lands 4,000 samples at +/-1.0 and would report as clipped. That
    // is correct behaviour, but it belongs in the clipping test rather than here.
    const FPwAudioBuffer Buffer = MakeStereoSine(1.0, 1000.0, 0.5);

    FPwAudioAnalysis Analysis;
    FString Code;
    FString Message;
    const bool bAnalyzed = PwAnalyzeBuffer(Buffer, Analysis, Code, Message);
    TestTrue(FString::Printf(TEXT("PwAnalyzeBuffer succeeded (%s: %s)"), *Code, *Message), bAnalyzed);
    if (!bAnalyzed)
    {
        return false;
    }

    // ---- technical --------------------------------------------------------------------------
    TestTrue(TEXT("technical measured"), Analysis.Technical.State.bMeasured);
    TestFalse(TEXT("a 0.5 sine is not digital silence"), Analysis.Technical.bDigitalSilence);
    TestEqual(TEXT("nonFiniteSamples is 0 and was checked"), Analysis.Technical.NonFiniteSamples, 0);
    TestTrue(FString::Printf(TEXT("durationMs is 1000, got %.4f"), Analysis.Technical.DurationMs),
        FMath::IsNearlyEqual(Analysis.Technical.DurationMs, 1000.0, 1e-6));
    TestTrue(FString::Printf(TEXT("peak is 0.5, got %.6f"), Analysis.Technical.PeakLinear),
        FMath::IsNearlyEqual(Analysis.Technical.PeakLinear, 0.5, 1e-4));

    TestTrue(TEXT("peakDb is set for a non-silent buffer"), Analysis.Technical.PeakDb.IsSet());
    TestTrue(TEXT("rmsDb is set for a non-silent buffer"), Analysis.Technical.RmsDb.IsSet());
    if (Analysis.Technical.PeakDb.IsSet() && Analysis.Technical.RmsDb.IsSet())
    {
        // 20*log10(0.5) = -6.0206 ; 20*log10(0.5/sqrt(2)) = -9.0309.
        const double PeakDb = Analysis.Technical.PeakDb.GetValue();
        const double RmsDb = Analysis.Technical.RmsDb.GetValue();
        TestTrue(FString::Printf(TEXT("peakDb is -6.021, got %.4f"), PeakDb),
            FMath::IsNearlyEqual(PeakDb, -6.0206, 0.01));
        TestTrue(FString::Printf(TEXT("rmsDb is -9.031, got %.4f"), RmsDb),
            FMath::IsNearlyEqual(RmsDb, -9.0309, 0.02));
    }

    TestEqual(TEXT("a 0.5 sine clips nothing"), Analysis.Technical.ClippedSamples, 0);
    TestTrue(FString::Printf(TEXT("dcOffset is 0 for whole cycles, got %.3e"), Analysis.Technical.DcOffset),
        FMath::Abs(Analysis.Technical.DcOffset) < 1e-5);
    // 1000 Hz means 2000 sign changes per second.
    TestTrue(FString::Printf(TEXT("zeroCrossingRate is ~2000, got %.2f"), Analysis.Technical.ZeroCrossingRate),
        FMath::Abs(Analysis.Technical.ZeroCrossingRate - 2000.0) < 20.0);

    // ---- envelope ---------------------------------------------------------------------------
    TestTrue(TEXT("envelope measured"), Analysis.Envelope.State.bMeasured);
    // Crest factor of ANY sine is 20*log10(sqrt(2)) = 3.0103 dB regardless of its amplitude.
    TestTrue(FString::Printf(TEXT("crestDb is 3.010 for a sine, got %.4f"), Analysis.Envelope.CrestDb),
        FMath::IsNearlyEqual(Analysis.Envelope.CrestDb, 3.0103, 0.05));
    TestTrue(FString::Printf(TEXT("onsetMs is 0 for a tone that starts immediately, got %.2f"),
        Analysis.Envelope.OnsetMs), Analysis.Envelope.OnsetMs < 15.0);
    // A tone that never stops has no decay time, and reporting one would be a fabrication (§1).
    TestFalse(TEXT("decayMs is unset for a sustained tone"), Analysis.Envelope.DecayMs.IsSet());

    // ---- spectral ---------------------------------------------------------------------------
    TestTrue(TEXT("spectral measured"), Analysis.Spectral.State.bMeasured);
    // Window leakage and the float noise floor both pull the magnitude-weighted centroid upward
    // and never downward, so the tolerance is deliberately asymmetric about the tone.
    TestTrue(FString::Printf(TEXT("centroidHz is ~1000, got %.1f"), Analysis.Spectral.CentroidHz),
        Analysis.Spectral.CentroidHz > 950.0 && Analysis.Spectral.CentroidHz < 1150.0);
    TestTrue(FString::Printf(TEXT("flatness is ~0 for a pure tone, got %.6f"), Analysis.Spectral.Flatness),
        Analysis.Spectral.Flatness < 0.01);
    TestTrue(FString::Printf(TEXT("rolloffHz sits at the tone, got %.1f"), Analysis.Spectral.RolloffHz),
        Analysis.Spectral.RolloffHz > 900.0 && Analysis.Spectral.RolloffHz < 1500.0);
    TestTrue(FString::Printf(TEXT("one dominant peak survives the separation gate, got %d"),
        Analysis.Spectral.Peaks.Num()), Analysis.Spectral.Peaks.Num() == 1);
    if (Analysis.Spectral.Peaks.Num() >= 1)
    {
        TestTrue(FString::Printf(TEXT("the dominant peak is the tone, got %.1f Hz"),
            Analysis.Spectral.Peaks[0].Hz),
            FMath::Abs(Analysis.Spectral.Peaks[0].Hz - 1000.0) < 30.0);
    }

    // The 500-1500 Hz band must hold essentially all of the energy.
    double MidBandRatio = -1.0;
    for (const FPwAudioBandRatio& Band : Analysis.Spectral.Bands)
    {
        if (FMath::IsNearlyEqual(Band.LowHz, 500.0) && FMath::IsNearlyEqual(Band.HighHz, 1500.0))
        {
            MidBandRatio = Band.Ratio;
        }
    }
    TestEqual(TEXT("all six bands exist at 48 kHz"), Analysis.Spectral.Bands.Num(), 6);
    TestTrue(FString::Printf(TEXT("the 500-1500 Hz band holds the tone, ratio %.5f"), MidBandRatio),
        MidBandRatio > 0.99);

    // ---- pitch ------------------------------------------------------------------------------
    TestTrue(TEXT("pitch measured for a clean tone"), Analysis.Pitch.State.bMeasured);
    if (Analysis.Pitch.State.bMeasured)
    {
        TestTrue(FString::Printf(TEXT("f0 is 1000 Hz, got %.2f"), Analysis.Pitch.F0Hz),
            FMath::Abs(Analysis.Pitch.F0Hz - 1000.0) < 10.0);
        TestTrue(FString::Printf(TEXT("confidence clears the %.2f floor, got %.3f"),
            PwAudioAnalysisLimits::MinPitchConfidence, Analysis.Pitch.Confidence),
            Analysis.Pitch.Confidence >= PwAudioAnalysisLimits::MinPitchConfidence);
    }

    // ---- stereo -----------------------------------------------------------------------------
    TestTrue(TEXT("stereo measured"), Analysis.Stereo.State.bMeasured);
    TestTrue(TEXT("correlation is defined when both channels carry signal"),
        Analysis.Stereo.Correlation.IsSet());
    if (Analysis.Stereo.Correlation.IsSet())
    {
        TestTrue(FString::Printf(TEXT("identical channels correlate at 1, got %.6f"),
            Analysis.Stereo.Correlation.GetValue()),
            FMath::IsNearlyEqual(Analysis.Stereo.Correlation.GetValue(), 1.0, 1e-4));
    }
    TestTrue(FString::Printf(TEXT("identical channels have zero width, got %.6f"), Analysis.Stereo.Width),
        Analysis.Stereo.Width < 1e-5);
    TestTrue(FString::Printf(TEXT("mono fold-down loses nothing, got %.6f"),
        Analysis.Stereo.MonoCompatibility),
        FMath::IsNearlyEqual(Analysis.Stereo.MonoCompatibility, 1.0, 1e-4));

    return true;
}

// =============================================================================================
// B. White noise: flatness converges on 1 and pitch is SUPPRESSED, not reported as 0.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioAnalysisWhiteNoiseTest,
    "PinWright.audio.analysis.WhiteNoiseIsFlatAndPitchless",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioAnalysisWhiteNoiseTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisTestHelpers;

    const FPwAudioBuffer Buffer = MakeWhiteNoise(2.0, 0.25, /*Seed*/ 20260816);

    FPwAudioAnalysis Analysis;
    FString Code;
    FString Message;
    const bool bAnalyzed = PwAnalyzeBuffer(Buffer, Analysis, Code, Message);
    TestTrue(FString::Printf(TEXT("PwAnalyzeBuffer succeeded (%s: %s)"), *Code, *Message), bAnalyzed);
    if (!bAnalyzed)
    {
        return false;
    }

    TestTrue(TEXT("spectral measured"), Analysis.Spectral.State.bMeasured);

    // Averaging ~180 frames shrinks each bin's relative spread to ~5%, which puts the flatness of
    // a definitionally flat spectrum above 0.99. The gate is set well below that so a genuine
    // regression fails while estimator noise does not.
    TestTrue(FString::Printf(TEXT("flatness approaches 1 for white noise, got %.4f"),
        Analysis.Spectral.Flatness), Analysis.Spectral.Flatness > 0.85);

    // A flat spectrum over 0..24 kHz puts the magnitude-weighted centroid near the middle.
    TestTrue(FString::Printf(TEXT("centroidHz sits mid-band, got %.1f"), Analysis.Spectral.CentroidHz),
        Analysis.Spectral.CentroidHz > 9000.0 && Analysis.Spectral.CentroidHz < 15000.0);
    TestTrue(FString::Printf(TEXT("rolloffHz sits near 0.85 of Nyquist, got %.1f"),
        Analysis.Spectral.RolloffHz),
        Analysis.Spectral.RolloffHz > 16000.0 && Analysis.Spectral.RolloffHz < 23000.0);

    // Widest band by far in a flat spectrum: 8k-20k is 12 kHz of the 20 kHz analysed.
    double TopBandRatio = -1.0;
    for (const FPwAudioBandRatio& Band : Analysis.Spectral.Bands)
    {
        if (FMath::IsNearlyEqual(Band.LowHz, 8000.0))
        {
            TopBandRatio = Band.Ratio;
        }
    }
    TestTrue(FString::Printf(TEXT("the 8k-20k band dominates a flat spectrum, ratio %.4f"), TopBandRatio),
        TopBandRatio > 0.4);

    // THE assertion of this test. Noise has no f0; a reported one is worse than none, because the
    // agent will act on it. PwEstimatePitch's own measured reference is ~0.13 confidence for
    // uniform white noise against a 0.50 floor, so this has real margin - if it fails, the
    // estimator has become over-confident on noise, which is a defect there rather than a reason
    // to widen the gate here.
    TestFalse(FString::Printf(TEXT("pitch is suppressed for noise (reason: '%s')"),
        *Analysis.Pitch.State.Reason), Analysis.Pitch.State.bMeasured);
    TestTrue(TEXT("the suppression carries a reason"), !Analysis.Pitch.State.Reason.IsEmpty());
    // The suppressed fields are CLEARED, not left holding the rejected estimate.
    TestTrue(TEXT("no f0 is left behind"), FMath::IsNearlyZero(Analysis.Pitch.F0Hz));

    // And the absence must survive into the wire form.
    const TSharedPtr<FJsonObject> Summary = SerializeAudioAnalysis(Analysis, /*bFullDetail*/ false);
    TestFalse(TEXT("the serialized report carries no pitch block"), HasObject(Summary, TEXT("pitch")));
    const TSharedPtr<FJsonObject> Unmeasured = GetObject(Summary, TEXT("unmeasured"));
    TestTrue(TEXT("unmeasured names pitch"),
        Unmeasured.IsValid() && Unmeasured->HasField(TEXT("pitch")));

    return true;
}

// =============================================================================================
// C. Unit impulse: zero attack, exactly one transient, an enormous crest factor.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioAnalysisImpulseTest,
    "PinWright.audio.analysis.ImpulseIsOneTransientWithZeroAttack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioAnalysisImpulseTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisTestHelpers;

    const FPwAudioBuffer Buffer = MakeImpulse(0.5, /*AtMs*/ 100.0, /*Amplitude*/ 0.9);

    FPwAudioAnalysis Analysis;
    FString Code;
    FString Message;
    const bool bAnalyzed = PwAnalyzeBuffer(Buffer, Analysis, Code, Message);
    TestTrue(FString::Printf(TEXT("PwAnalyzeBuffer succeeded (%s: %s)"), *Code, *Message), bAnalyzed);
    if (!bAnalyzed)
    {
        return false;
    }

    TestTrue(TEXT("envelope measured"), Analysis.Envelope.State.bMeasured);

    // The energy is confined to one 10 ms envelope block, so the loudest block IS the first
    // active block: attack is zero to within the envelope resolution.
    TestTrue(FString::Printf(TEXT("attackMs is ~0 for an impulse, got %.2f"), Analysis.Envelope.AttackMs),
        Analysis.Envelope.AttackMs < 15.0);
    // ...and the 100 ms of leading silence is reported separately rather than folded into attack.
    TestTrue(FString::Printf(TEXT("onsetMs reports the 100 ms lead-in, got %.2f"), Analysis.Envelope.OnsetMs),
        FMath::Abs(Analysis.Envelope.OnsetMs - 100.0) < 15.0);
    TestTrue(TEXT("an impulse decays, so decayMs is measured"), Analysis.Envelope.DecayMs.IsSet());

    // One sample of 0.9 in 24,000 frames of two channels: rms = 0.9 * sqrt(2/48000) = 0.005809,
    // so crest = 20*log10(0.9/0.005809) = 43.8 dB.
    TestTrue(FString::Printf(TEXT("crestDb is ~43.8 for a lone impulse, got %.2f"), Analysis.Envelope.CrestDb),
        Analysis.Envelope.CrestDb > 40.0);
    TestEqual(TEXT("0.9 is below full scale, so nothing clips"), Analysis.Technical.ClippedSamples, 0);

    // Active duration is one block of a half-second buffer.
    TestTrue(TEXT("activeDurationMs is measured"), Analysis.Technical.ActiveDurationMs.IsSet());
    if (Analysis.Technical.ActiveDurationMs.IsSet())
    {
        TestTrue(FString::Printf(TEXT("activeDurationMs is one block, got %.2f of 500 ms"),
            Analysis.Technical.ActiveDurationMs.GetValue()),
            Analysis.Technical.ActiveDurationMs.GetValue() <= 20.0);
    }

    TestTrue(TEXT("transientCount was measured, not merely defaulted"),
        Analysis.Envelope.TransientCount.IsSet());
    if (Analysis.Envelope.TransientCount.IsSet())
    {
        TestEqual(TEXT("one impulse is one transient"), Analysis.Envelope.TransientCount.GetValue(), 1);
    }

    return true;
}

// =============================================================================================
// D. Chirps: the two directions must produce numbers of opposite sign (§6).
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioAnalysisChirpDirectionTest,
    "PinWright.audio.analysis.ChirpMotionIsSignedInBothDirections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioAnalysisChirpDirectionTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisTestHelpers;

    // 200 -> 1600 Hz, deliberately inside PwEstimatePitch's 50-2000 Hz search range at both ends:
    // a sweep that touches the boundary drops its outermost windows and would make the measured
    // travel depend on where the estimator gives up rather than on the signal.
    const FPwAudioBuffer Rising = MakeChirp(2.0, 200.0, 1600.0, 0.5);
    const FPwAudioBuffer Falling = MakeChirp(2.0, 1600.0, 200.0, 0.5);

    FPwAudioAnalysis RisingAnalysis;
    FPwAudioAnalysis FallingAnalysis;
    FString Code;
    FString Message;
    const bool bRisingOk = PwAnalyzeBuffer(Rising, RisingAnalysis, Code, Message);
    TestTrue(FString::Printf(TEXT("rising chirp analysed (%s: %s)"), *Code, *Message), bRisingOk);
    const bool bFallingOk = PwAnalyzeBuffer(Falling, FallingAnalysis, Code, Message);
    TestTrue(FString::Printf(TEXT("falling chirp analysed (%s: %s)"), *Code, *Message), bFallingOk);
    if (!bRisingOk || !bFallingOk)
    {
        return false;
    }

    TestTrue(TEXT("pitch measured on the rising chirp"), RisingAnalysis.Pitch.State.bMeasured);
    TestTrue(TEXT("pitch measured on the falling chirp"), FallingAnalysis.Pitch.State.bMeasured);
    if (!RisingAnalysis.Pitch.State.bMeasured || !FallingAnalysis.Pitch.State.bMeasured)
    {
        return false;
    }

    // The signed travel is the half of the classification this module owns, so it is the hard
    // assertion. Quartile medians of a 200 -> 1600 Hz sweep sit near 375 Hz and 1425 Hz, i.e.
    // +23.1 semitones; the gate at +/-6 leaves room for a different track density.
    TestTrue(TEXT("the rising chirp reports a semitone delta"), RisingAnalysis.Pitch.SemitoneDelta.IsSet());
    TestTrue(TEXT("the falling chirp reports a semitone delta"), FallingAnalysis.Pitch.SemitoneDelta.IsSet());
    if (RisingAnalysis.Pitch.SemitoneDelta.IsSet() && FallingAnalysis.Pitch.SemitoneDelta.IsSet())
    {
        const double Up = RisingAnalysis.Pitch.SemitoneDelta.GetValue();
        const double Down = FallingAnalysis.Pitch.SemitoneDelta.GetValue();
        TestTrue(FString::Printf(TEXT("rising is positive, got %+.2f semitones"), Up), Up > 6.0);
        TestTrue(FString::Printf(TEXT("falling is negative, got %+.2f semitones"), Down), Down < -6.0);
    }

    // The estimator's own motion class is passed through verbatim, so this asserts the property
    // that matters without hard-coding its vocabulary: the two directions must not read alike.
    TestTrue(TEXT("the rising chirp carries a motion class"), !RisingAnalysis.Pitch.Motion.IsEmpty());
    TestTrue(TEXT("the falling chirp carries a motion class"), !FallingAnalysis.Pitch.Motion.IsEmpty());
    TestTrue(FString::Printf(TEXT("the two directions classify differently ('%s' vs '%s')"),
        *RisingAnalysis.Pitch.Motion, *FallingAnalysis.Pitch.Motion),
        !RisingAnalysis.Pitch.Motion.Equals(FallingAnalysis.Pitch.Motion, ESearchCase::IgnoreCase));

    return true;
}

// =============================================================================================
// E. Digital silence: named, and every level/brightness family ABSENT rather than zero.
//    This is the §7 ordering test, and the assertions are deliberately about absence.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioAnalysisSilenceTest,
    "PinWright.audio.analysis.DigitalSilenceSuppressesLoudnessAndBrightness",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioAnalysisSilenceTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisTestHelpers;

    const FPwAudioBuffer Buffer = MakeSilentBuffer(0.5);

    FPwAudioAnalysis Analysis;
    FString Code;
    FString Message;
    // Silence is a SUCCESS: "this rendered nothing" is a measurement, and a different fact from
    // "this could not be analysed".
    const bool bAnalyzed = PwAnalyzeBuffer(Buffer, Analysis, Code, Message);
    TestTrue(FString::Printf(TEXT("silence analyses successfully (%s: %s)"), *Code, *Message), bAnalyzed);
    TestTrue(TEXT("no error code on the silence path"), Code.IsEmpty());
    if (!bAnalyzed)
    {
        return false;
    }

    TestTrue(TEXT("technical measured"), Analysis.Technical.State.bMeasured);
    TestTrue(TEXT("silence is NAMED"), Analysis.Technical.bDigitalSilence);
    TestTrue(FString::Printf(TEXT("durationMs is still measured, got %.2f"), Analysis.Technical.DurationMs),
        FMath::IsNearlyEqual(Analysis.Technical.DurationMs, 500.0, 1e-6));

    // The whole point: these are ABSENT, not small.
    TestFalse(TEXT("peakDb is absent - log(0) is not -90 dB"), Analysis.Technical.PeakDb.IsSet());
    TestFalse(TEXT("rmsDb is absent"), Analysis.Technical.RmsDb.IsSet());
    TestFalse(TEXT("envelope is absent"), Analysis.Envelope.State.bMeasured);
    TestFalse(TEXT("loudness is absent"), Analysis.Loudness.State.bMeasured);
    TestFalse(TEXT("spectral is absent"), Analysis.Spectral.State.bMeasured);
    TestFalse(TEXT("pitch is absent"), Analysis.Pitch.State.bMeasured);
    TestFalse(TEXT("stereo is absent"), Analysis.Stereo.State.bMeasured);

    // And absent on the wire, with a reason for each.
    const TSharedPtr<FJsonObject> Summary = SerializeAudioAnalysis(Analysis, /*bFullDetail*/ false);
    TestTrue(TEXT("technical is serialized"), HasObject(Summary, TEXT("technical")));
    TestFalse(TEXT("no envelope block"), HasObject(Summary, TEXT("envelope")));
    TestFalse(TEXT("no loudness block"), HasObject(Summary, TEXT("loudness")));
    TestFalse(TEXT("no spectral block"), HasObject(Summary, TEXT("spectral")));
    TestFalse(TEXT("no pitch block"), HasObject(Summary, TEXT("pitch")));
    TestFalse(TEXT("no stereo block"), HasObject(Summary, TEXT("stereo")));

    const TSharedPtr<FJsonObject> Technical = GetObject(Summary, TEXT("technical"));
    TestTrue(TEXT("technical block exists"), Technical.IsValid());
    if (Technical.IsValid())
    {
        TestFalse(TEXT("no peakDb key at all"), Technical->HasField(TEXT("peakDb")));
        TestFalse(TEXT("no rmsDb key at all"), Technical->HasField(TEXT("rmsDb")));
        // The LINEAR peak is a true measurement of zero, and it sits beside digitalSilence.
        TestTrue(TEXT("linear peak is reported as an exact 0"),
            Technical->HasField(TEXT("peak")) && FMath::IsNearlyZero(Technical->GetNumberField(TEXT("peak"))));
        TestTrue(TEXT("digitalSilence is on the wire"),
            Technical->HasField(TEXT("digitalSilence")) && Technical->GetBoolField(TEXT("digitalSilence")));
    }

    const TSharedPtr<FJsonObject> Unmeasured = GetObject(Summary, TEXT("unmeasured"));
    TestTrue(TEXT("unmeasured block exists"), Unmeasured.IsValid());
    if (Unmeasured.IsValid())
    {
        for (const TCHAR* Family : { TEXT("envelope"), TEXT("loudness"), TEXT("spectral"),
                                     TEXT("pitch"), TEXT("stereo") })
        {
            FString Reason;
            const bool bNamed = Unmeasured->TryGetStringField(Family, Reason) && !Reason.IsEmpty();
            TestTrue(FString::Printf(TEXT("unmeasured names '%s' with a reason"), Family), bNamed);
        }
        TestFalse(TEXT("technical is not listed as unmeasured"), Unmeasured->HasField(TEXT("technical")));
    }

    // A target on a level or brightness metric is UNSCORED here, not failed against zero.
    const TSet<FString> Measurable = MeasurableMetrics(Analysis);
    for (const TCHAR* Name : { TEXT("durationMs"), TEXT("dcOffset"), TEXT("clippedSamples"),
                               TEXT("zeroCrossingRate") })
    {
        TestTrue(FString::Printf(TEXT("'%s' is still scoreable on silence"), Name),
            Measurable.Contains(Name));
    }
    for (const TCHAR* Name : { TEXT("peakDb"), TEXT("rmsDb"), TEXT("lufs"), TEXT("crestDb"),
                               TEXT("centroidHz"), TEXT("rolloffHz"), TEXT("flatness"),
                               TEXT("attackMs"), TEXT("decayMs"), TEXT("stereoCorrelation") })
    {
        TestFalse(FString::Printf(TEXT("'%s' is NOT scoreable on silence"), Name),
            Measurable.Contains(Name));
    }

    return true;
}

// =============================================================================================
// F. Clipped-sample count is exact, per channel-sample.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioAnalysisClippingCountTest,
    "PinWright.audio.analysis.ClippedSampleCountIsExact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioAnalysisClippingCountTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisTestHelpers;

    // A 0.5 sine peaks at exactly 0.5, so every full-scale sample below is one this test planted.
    FPwAudioBuffer Buffer = MakeStereoSine(1.0, 440.0, 0.5);

    const int32 LeftIndices[] = { 100, 2000, 2001, 2002, 30000, 30001, 47000 };
    const int32 RightIndices[] = { 500, 501, 25000, 25001, 25002 };
    for (const int32 Index : LeftIndices)
    {
        Buffer.Left[Index] = 1.0f;
    }
    for (const int32 Index : RightIndices)
    {
        Buffer.Right[Index] = -1.0f;
    }
    const int32 ExpectedClipped =
        static_cast<int32>(UE_ARRAY_COUNT(LeftIndices) + UE_ARRAY_COUNT(RightIndices));

    FPwAudioAnalysis Analysis;
    FString Code;
    FString Message;
    const bool bAnalyzed = PwAnalyzeBuffer(Buffer, Analysis, Code, Message);
    TestTrue(FString::Printf(TEXT("PwAnalyzeBuffer succeeded (%s: %s)"), *Code, *Message), bAnalyzed);
    if (!bAnalyzed)
    {
        return false;
    }

    // Counted per channel-sample, both polarities, and exactly - not "at least".
    TestEqual(TEXT("clippedSamples matches the planted count exactly"),
        Analysis.Technical.ClippedSamples, ExpectedClipped);
    TestTrue(FString::Printf(TEXT("peak reaches full scale, got %.6f"), Analysis.Technical.PeakLinear),
        FMath::IsNearlyEqual(Analysis.Technical.PeakLinear, 1.0, 1e-6));

    double MetricValue = 0.0;
    TestTrue(TEXT("clippedSamples is reachable through the target vocabulary"),
        PwGetAudioAnalysisMetric(Analysis, EPwSynthMetric::ClippedSamples, MetricValue));
    TestEqual(TEXT("and it agrees with the struct"),
        static_cast<int32>(MetricValue), ExpectedClipped);

    return true;
}

// =============================================================================================
// G. DC offset is measured, and SIGNED (§6).
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioAnalysisDcOffsetTest,
    "PinWright.audio.analysis.DcOffsetIsSignedAndExact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioAnalysisDcOffsetTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisTestHelpers;

    auto MeasureOffset = [this](double Injected, double& OutMeasured) -> bool
    {
        FPwAudioBuffer Buffer = MakeStereoSine(1.0, 500.0, 0.3);
        for (int32 Index = 0; Index < Buffer.NumFrames(); ++Index)
        {
            Buffer.Left[Index] += static_cast<float>(Injected);
            Buffer.Right[Index] += static_cast<float>(Injected);
        }

        FPwAudioAnalysis Analysis;
        FString Code;
        FString Message;
        if (!PwAnalyzeBuffer(Buffer, Analysis, Code, Message))
        {
            AddError(FString::Printf(TEXT("PwAnalyzeBuffer failed (%s: %s)"), *Code, *Message));
            return false;
        }
        OutMeasured = Analysis.Technical.DcOffset;
        return true;
    };

    double Positive = 0.0;
    double Negative = 0.0;
    if (!MeasureOffset(0.25, Positive) || !MeasureOffset(-0.25, Negative))
    {
        return false;
    }

    TestTrue(FString::Printf(TEXT("+0.25 injection measures +0.25, got %+.6f"), Positive),
        FMath::IsNearlyEqual(Positive, 0.25, 1e-4));
    TestTrue(FString::Printf(TEXT("-0.25 injection measures -0.25, got %+.6f"), Negative),
        FMath::IsNearlyEqual(Negative, -0.25, 1e-4));
    // An unsigned magnitude would make these two identical, which is exactly the failure §6 names.
    TestTrue(TEXT("the two directions do not score alike"), Positive > 0.0 && Negative < 0.0);

    return true;
}

// =============================================================================================
// H. Stereo image: three constructions with exact expected geometry.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioAnalysisStereoImageTest,
    "PinWright.audio.analysis.StereoImageMatchesConstruction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioAnalysisStereoImageTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisTestHelpers;

    auto Analyze = [this](const FPwAudioBuffer& Buffer, FPwAudioAnalysis& Out) -> bool
    {
        FString Code;
        FString Message;
        if (!PwAnalyzeBuffer(Buffer, Out, Code, Message))
        {
            AddError(FString::Printf(TEXT("PwAnalyzeBuffer failed (%s: %s)"), *Code, *Message));
            return false;
        }
        return true;
    };

    // ---- identical channels: correlation +1, width 0 -----------------------------------------
    {
        const FPwAudioBuffer Buffer = MakeStereoSine(0.5, 700.0, 0.4);
        FPwAudioAnalysis Analysis;
        if (!Analyze(Buffer, Analysis))
        {
            return false;
        }
        TestTrue(TEXT("identical: correlation is defined"), Analysis.Stereo.Correlation.IsSet());
        if (Analysis.Stereo.Correlation.IsSet())
        {
            TestTrue(FString::Printf(TEXT("identical: correlation is +1, got %+.6f"),
                Analysis.Stereo.Correlation.GetValue()),
                FMath::IsNearlyEqual(Analysis.Stereo.Correlation.GetValue(), 1.0, 1e-4));
        }
        TestTrue(FString::Printf(TEXT("identical: width is 0, got %.6f"), Analysis.Stereo.Width),
            Analysis.Stereo.Width < 1e-5);
        TestFalse(TEXT("identical: neither channel is silent"), Analysis.Stereo.bOneChannelSilent);
    }

    // ---- anti-phase: correlation -1, width 1, mono fold-down cancels --------------------------
    {
        FPwAudioBuffer Buffer = MakeStereoSine(0.5, 700.0, 0.4);
        for (int32 Index = 0; Index < Buffer.NumFrames(); ++Index)
        {
            Buffer.Right[Index] = -Buffer.Left[Index];
        }
        FPwAudioAnalysis Analysis;
        if (!Analyze(Buffer, Analysis))
        {
            return false;
        }
        TestTrue(TEXT("anti-phase: correlation is defined"), Analysis.Stereo.Correlation.IsSet());
        if (Analysis.Stereo.Correlation.IsSet())
        {
            // The opposite end of the same scale. An unsigned "how decorrelated" measure would
            // score this identically to the in-phase case above (§6).
            TestTrue(FString::Printf(TEXT("anti-phase: correlation is -1, got %+.6f"),
                Analysis.Stereo.Correlation.GetValue()),
                FMath::IsNearlyEqual(Analysis.Stereo.Correlation.GetValue(), -1.0, 1e-4));
        }
        TestTrue(FString::Printf(TEXT("anti-phase: width is 1, got %.6f"), Analysis.Stereo.Width),
            FMath::IsNearlyEqual(Analysis.Stereo.Width, 1.0, 1e-4));
        TestTrue(FString::Printf(TEXT("anti-phase: mono fold-down cancels to 0, got %.6f"),
            Analysis.Stereo.MonoCompatibility), Analysis.Stereo.MonoCompatibility < 1e-4);
    }

    // ---- hard-panned left: correlation UNDEFINED, width 0.5, fold-down 1/sqrt(2) --------------
    {
        FPwAudioBuffer Buffer = MakeStereoSine(0.5, 700.0, 0.4);
        FMemory::Memzero(Buffer.Right.GetData(), Buffer.Right.Num() * sizeof(float));

        FPwAudioAnalysis Analysis;
        if (!Analyze(Buffer, Analysis))
        {
            return false;
        }
        // 0/0 is not 0. A correlation of 0.0 here would read as "wide decorrelated image" for a
        // signal that is entirely on one side.
        TestFalse(TEXT("hard pan: correlation is omitted, not reported as 0"),
            Analysis.Stereo.Correlation.IsSet());
        TestTrue(TEXT("hard pan: the silent side is named"), Analysis.Stereo.bOneChannelSilent);
        TestEqual(TEXT("hard pan: the right channel is the silent one"),
            Analysis.Stereo.SilentChannel, FString(TEXT("right")));
        // mid = side = L/2, so width = 0.5 exactly.
        TestTrue(FString::Printf(TEXT("hard pan: width is 0.5, got %.6f"), Analysis.Stereo.Width),
            FMath::IsNearlyEqual(Analysis.Stereo.Width, 0.5, 1e-4));
        // mid rms = RmsL/2 against a reference of RmsL/sqrt(2) gives 1/sqrt(2) = 0.7071.
        TestTrue(FString::Printf(TEXT("hard pan: mono fold-down keeps 0.7071, got %.6f"),
            Analysis.Stereo.MonoCompatibility),
            FMath::IsNearlyEqual(Analysis.Stereo.MonoCompatibility, 0.70711, 1e-3));

        const TSharedPtr<FJsonObject> Summary = SerializeAudioAnalysis(Analysis, /*bFullDetail*/ false);
        const TSharedPtr<FJsonObject> Stereo = GetObject(Summary, TEXT("stereo"));
        TestTrue(TEXT("hard pan: the stereo block is serialized"), Stereo.IsValid());
        if (Stereo.IsValid())
        {
            TestFalse(TEXT("hard pan: no correlation key on the wire"),
                Stereo->HasField(TEXT("correlation")));
            TestTrue(TEXT("hard pan: silentChannel explains its absence"),
                Stereo->HasField(TEXT("silentChannel")));
        }
    }

    return true;
}

// =============================================================================================
// I. A -20 dBFS 1 kHz stereo sine has a closed-form BS.1770 loudness.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioAnalysisKnownLufsTest,
    "PinWright.audio.analysis.KnownLufsReferenceTone",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioAnalysisKnownLufsTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisTestHelpers;

    // DERIVATION (BS.1770-4, 48 kHz coefficients, so the numbers below are checkable by hand):
    //
    //   L_K = -0.691 + 10*log10( sum_i G_i * z_i ),  G_left = G_right = 1.0
    //
    // For a sine of amplitude A present identically in both channels, z_i = |H(f)|^2 * A^2 / 2,
    // so the sum over two channels is |H(f)|^2 * A^2 and
    //
    //   L_K = -0.691 + 20*log10( |H(1 kHz)| * A ).
    //
    // |H(1 kHz)| is the K-weighting gain, the product of the two stages evaluated at
    // w = 2*pi*1000/48000:
    //   stage 1 (shelving, b = [1.53512486, -2.69169619, 1.19839281],
    //                      a = [1, -1.69065929, 0.73248077])  ->  1.08021  (+0.670 dB)
    //   stage 2 (RLB highpass, b = [1, -2, 1],
    //                          a = [1, -1.99004745, 0.99007225]) -> 1.01489  (+0.128 dB)
    //   product                                                   -> 1.09629  (+0.799 dB)
    //
    // With A = 0.1 (-20 dBFS peak):
    //   L_K = -0.691 + 20*log10(0.109629) = -0.691 - 19.2015 = -19.89 LUFS.
    //
    // Sanity check on the same formula: a FULL-scale stereo 1 kHz sine gives -0.691 + 0.799 =
    // +0.11 LUFS, which is the well-known "a 0 dBFS stereo sine reads about 0 LUFS" figure.
    // Four seconds: long enough for the 400 ms momentary and 3 s short-term windows both to have
    // complete instances, so a back-end that gates on window completeness still measures.
    const FPwAudioBuffer Buffer = MakeStereoSine(4.0, 1000.0, 0.1);

    FPwAudioAnalysis Analysis;
    FString Code;
    FString Message;
    const bool bAnalyzed = PwAnalyzeBuffer(Buffer, Analysis, Code, Message);
    TestTrue(FString::Printf(TEXT("PwAnalyzeBuffer succeeded (%s: %s)"), *Code, *Message), bAnalyzed);
    if (!bAnalyzed)
    {
        return false;
    }

#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    // The BS.1770 meter PwComputeLoudness measures with (Audio::FLKFSAnalyzer) ships from UE 5.8
    // only, so on this engine the derivation above has nothing to check. What must still hold is
    // the reporting direction: the family comes back UNMEASURED carrying a reason that names the
    // engine, rather than measured with a substituted number - a recipe targeting -16 LUFS has to
    // be able to tell the two apart.
    TestFalse(TEXT("loudness is reported as unmeasured, not measured from a substitute"),
        Analysis.Loudness.State.bMeasured);
    TestTrue(FString::Printf(TEXT("and the reason names the engine that carries the meter (%s)"),
        *Analysis.Loudness.State.Reason),
        Analysis.Loudness.State.Reason.Contains(TEXT("5.8")));
    return true;
#else
    TestTrue(FString::Printf(TEXT("loudness measured (%s)"), *Analysis.Loudness.State.Reason),
        Analysis.Loudness.State.bMeasured);
    if (!Analysis.Loudness.State.bMeasured)
    {
        return false;
    }

    constexpr double ExpectedLufs = -19.89;
    TestTrue(FString::Printf(TEXT("integrated loudness is %.2f LUFS, got %.2f"),
        ExpectedLufs, Analysis.Loudness.IntegratedLufs),
        FMath::Abs(Analysis.Loudness.IntegratedLufs - ExpectedLufs) < 1.0);

    // A steady tone has essentially no loudness range; a large one would mean the gating is wrong.
    TestTrue(FString::Printf(TEXT("a steady tone has a small loudness range, got %.2f LU"),
        Analysis.Loudness.LoudnessRangeLu), Analysis.Loudness.LoudnessRangeLu < 2.0);

    double MetricValue = 0.0;
    TestTrue(TEXT("lufs is reachable through the target vocabulary"),
        PwGetAudioAnalysisMetric(Analysis, EPwSynthMetric::Lufs, MetricValue));
    TestTrue(TEXT("and it is the integrated value"),
        FMath::IsNearlyEqual(MetricValue, Analysis.Loudness.IntegratedLufs, 1e-9));

    return true;
#endif
}

// =============================================================================================
// J. Failure direction: an empty buffer (§12).
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioAnalysisEmptyBufferTest,
    "PinWright.audio.analysis.EmptyBufferFailsWithEmptyCode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioAnalysisEmptyBufferTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisTestHelpers;

    FPwAudioBuffer Buffer;
    Buffer.SampleRate = TestSampleRate;

    FPwAudioAnalysis Analysis;
    FString Code;
    FString Message;
    const bool bAnalyzed = PwAnalyzeBuffer(Buffer, Analysis, Code, Message);

    TestFalse(TEXT("a zero-frame buffer is a failure, not a zero-length success"), bAnalyzed);
    TestEqual(TEXT("code is AUDIO_EMPTY_BUFFER"), Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
    TestTrue(TEXT("the message explains the outcome"), !Message.IsEmpty());
    TestTrue(TEXT("every family is left unmeasured"), AllFamiliesUnmeasured(Analysis));

    // The serialized failure must carry no measurement-shaped object at all.
    const TSharedPtr<FJsonObject> Summary = SerializeAudioAnalysis(Analysis, /*bFullDetail*/ false);
    for (const TCHAR* Family : { TEXT("technical"), TEXT("envelope"), TEXT("loudness"),
                                 TEXT("spectral"), TEXT("pitch"), TEXT("stereo") })
    {
        TestFalse(FString::Printf(TEXT("no '%s' block"), Family), HasObject(Summary, Family));
    }
    const TSharedPtr<FJsonObject> Unmeasured = GetObject(Summary, TEXT("unmeasured"));
    TestTrue(TEXT("all six families are listed as unmeasured"),
        Unmeasured.IsValid() && Unmeasured->Values.Num() == 6);

    // A buffer whose channels disagree is a different failure with a different remedy.
    FPwAudioBuffer Ragged;
    Ragged.SampleRate = TestSampleRate;
    Ragged.Left.SetNumZeroed(1000);
    Ragged.Right.SetNumZeroed(999);
    FString RaggedCode;
    FString RaggedMessage;
    FPwAudioAnalysis RaggedAnalysis;
    TestFalse(TEXT("a ragged buffer fails"),
        PwAnalyzeBuffer(Ragged, RaggedAnalysis, RaggedCode, RaggedMessage));
    TestEqual(TEXT("ragged channels report INVALID_PARAMS, not EMPTY_BUFFER"),
        RaggedCode, FString(ErrorCodes::ERR_INVALID_PARAMS));

    return true;
}

// =============================================================================================
// K. Failure direction and check ORDER: non-finite samples must not measure as silence (§7/§12).
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioAnalysisNonFiniteTest,
    "PinWright.audio.analysis.NonFiniteFailsBeforeSilence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioAnalysisNonFiniteTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisTestHelpers;

    auto ExpectNonFiniteRejection = [this](const TCHAR* Label, const FPwAudioBuffer& Buffer)
    {
        FPwAudioAnalysis Analysis;
        FString Code;
        FString Message;
        const bool bAnalyzed = PwAnalyzeBuffer(Buffer, Analysis, Code, Message);
        TestFalse(FString::Printf(TEXT("%s: analysis fails"), Label), bAnalyzed);
        TestEqual(FString::Printf(TEXT("%s: code is AUDIO_NON_FINITE_SAMPLES"), Label),
            Code, FString(ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES));
        TestTrue(FString::Printf(TEXT("%s: every family is left unmeasured"), Label),
            AllFamiliesUnmeasured(Analysis));
        TestFalse(FString::Printf(TEXT("%s: it is NOT reported as digital silence"), Label),
            Analysis.Technical.bDigitalSilence);
    };

    // THE ordering case. NaN compares false against every threshold, so FMath::Max leaves the
    // measured peak at 0 and an implementation that tested the silence floor first would report
    // this buffer as a clean silent render.
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(0.25);
        for (int32 Index = 0; Index < Buffer.NumFrames(); ++Index)
        {
            Buffer.Left[Index] = MakeNaN();
            Buffer.Right[Index] = MakeNaN();
        }
        ExpectNonFiniteRejection(TEXT("all-NaN"), Buffer);
    }

    // A single NaN in an otherwise real signal - the realistic case, a filter that went unstable
    // for one sample. It must not be diluted away by 48,000 finite neighbours.
    {
        FPwAudioBuffer Buffer = MakeStereoSine(0.5, 440.0, 0.5);
        Buffer.Left[12345] = MakeNaN();
        ExpectNonFiniteRejection(TEXT("one NaN in real audio"), Buffer);
    }

    // Infinity is the mirror case: it survives a silence test and then poisons every sum.
    {
        FPwAudioBuffer Buffer = MakeStereoSine(0.5, 440.0, 0.5);
        Buffer.Right[999] = MakeInfinity();
        ExpectNonFiniteRejection(TEXT("one infinity in real audio"), Buffer);
    }

    return true;
}

// =============================================================================================
// L. The summary form fits the measured post-wrap response budget.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioAnalysisResponseBudgetTest,
    "PinWright.audio.analysis.SummaryFormFitsResponseBudget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioAnalysisResponseBudgetTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisTestHelpers;

    // Worst realistic case for size: every family measured, all six bands present, several
    // dominant peaks surviving the separation gate, and a long enough buffer for a large flux
    // series in the full form.
    FPwAudioBuffer Buffer = MakeWhiteNoise(3.0, 0.05, /*Seed*/ 4242);
    AddSine(Buffer.Left, 220.0, 0.3);
    AddSine(Buffer.Right, 220.0, 0.28);
    AddSine(Buffer.Left, 1750.0, 0.2);
    AddSine(Buffer.Right, 1750.0, 0.2);
    AddSine(Buffer.Left, 6300.0, 0.12);
    AddSine(Buffer.Right, 6300.0, 0.10);

    FPwAudioAnalysis Analysis;
    FString Code;
    FString Message;
    const bool bAnalyzed = PwAnalyzeBuffer(Buffer, Analysis, Code, Message);
    TestTrue(FString::Printf(TEXT("PwAnalyzeBuffer succeeded (%s: %s)"), *Code, *Message), bAnalyzed);
    if (!bAnalyzed)
    {
        return false;
    }

    const FString Summary = ToResponseJson(SerializeAudioAnalysis(Analysis, /*bFullDetail*/ false));
    const FString Full = ToResponseJson(SerializeAudioAnalysis(Analysis, /*bFullDetail*/ true));

    // The real ceiling is ~4,250 characters, not the 10,000 the spill threshold suggests: the
    // wrapped MCP ToolResult carries the payload twice (escaped in content[0].text, verbatim in
    // structuredContent) for roughly 2.35x amplification. The gate below sits well inside that so
    // an addition that doubles the summary trips this test rather than a user's response.
    AddInfo(FString::Printf(TEXT("summary form is %d chars (gate %d, wrapped ceiling ~%d); ")
                            TEXT("full form is %d chars"),
        Summary.Len(), SummaryBudgetGate, WrappedResponseCeiling, Full.Len()));
    TestTrue(FString::Printf(TEXT("summary form is %d chars, under the %d gate"),
        Summary.Len(), SummaryBudgetGate), Summary.Len() < SummaryBudgetGate);
    TestTrue(FString::Printf(TEXT("summary form is %d chars, under the ~%d wrapped ceiling"),
        Summary.Len(), WrappedResponseCeiling), Summary.Len() < WrappedResponseCeiling);

    // No per-frame array may reach the summary form.
    TestFalse(TEXT("summary carries no flux series"), Summary.Contains(TEXT("\"flux\"")));
    TestFalse(TEXT("summary carries no pitch track"), Summary.Contains(TEXT("\"track\"")));
    TestFalse(TEXT("summary carries no onset list"), Summary.Contains(TEXT("\"onsets\"")));

    // The full form is where the series live, and it is allowed to spill.
    TestTrue(TEXT("full form is larger than the summary"), Full.Len() > Summary.Len());
    const TSharedPtr<FJsonObject> FullRoot = SerializeAudioAnalysis(Analysis, /*bFullDetail*/ true);
    const TSharedPtr<FJsonObject> Spectral = GetObject(FullRoot, TEXT("spectral"));
    TestTrue(TEXT("full form carries the spectral block"), Spectral.IsValid());
    if (Spectral.IsValid())
    {
        const TSharedPtr<FJsonObject> Flux = GetObject(Spectral, TEXT("flux"));
        TestTrue(TEXT("full form carries the flux series"), Flux.IsValid());
        if (Flux.IsValid())
        {
            // A decimated series must say so, or a reader cannot tell a thinned curve from a
            // short one.
            const int32 Total = static_cast<int32>(Flux->GetNumberField(TEXT("totalPoints")));
            const int32 Stride = static_cast<int32>(Flux->GetNumberField(TEXT("stride")));
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            TestTrue(TEXT("flux series carries its values"), Flux->TryGetArrayField(TEXT("values"), Values));
            TestTrue(FString::Printf(TEXT("totalPoints (%d) reports the undecimated length"), Total),
                Total > PwAudioAnalysisLimits::MaxSeriesPoints);
            TestTrue(FString::Printf(TEXT("stride (%d) is greater than 1 when decimating"), Stride),
                Stride > 1);
            if (Values)
            {
                TestTrue(FString::Printf(TEXT("the emitted series is capped at %d, got %d"),
                    PwAudioAnalysisLimits::MaxSeriesPoints, Values->Num()),
                    Values->Num() <= PwAudioAnalysisLimits::MaxSeriesPoints);
            }
        }
    }

    return true;
}

// =============================================================================================
// M. Every EPwSynthMetric a `targets` block can name is answerable for a well-formed signal.
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioAnalysisMetricVocabularyTest,
    "PinWright.audio.analysis.MetricVocabularyIsComplete",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioAnalysisMetricVocabularyTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisTestHelpers;

    // Decaying so decayMs exists, pitched so the confidence gate opens, and with unequal channel
    // gains so the correlation is defined - i.e. a signal for which all fourteen are meaningful.
    const FPwAudioBuffer Buffer = MakeDecayingTone(2.0, 440.0, /*DecayPerSecond*/ 8.0);

    FPwAudioAnalysis Analysis;
    FString Code;
    FString Message;
    const bool bAnalyzed = PwAnalyzeBuffer(Buffer, Analysis, Code, Message);
    TestTrue(FString::Printf(TEXT("PwAnalyzeBuffer succeeded (%s: %s)"), *Code, *Message), bAnalyzed);
    if (!bAnalyzed)
    {
        return false;
    }

    // A target the report cannot answer is a target that can never be scored, so the coverage
    // itself is the assertion: every name in the recipe vocabulary must resolve here.
    for (uint8 Index = 0; Index < static_cast<uint8>(EPwSynthMetric::Count); ++Index)
    {
        const EPwSynthMetric Metric = static_cast<EPwSynthMetric>(Index);
        double Value = 0.0;
#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        if (Metric == EPwSynthMetric::Lufs)
        {
            // The one name in the vocabulary this engine cannot answer: it reads the BS.1770
            // meter the engine ships from 5.8. Asserted in the negative rather than skipped, so
            // a build that silently starts answering it with something else fails here.
            TestFalse(TEXT("metric 'lufs' does not resolve without the engine's BS.1770 meter"),
                PwGetAudioAnalysisMetric(Analysis, Metric, Value));
            continue;
        }
#endif
        TestTrue(FString::Printf(TEXT("metric '%s' resolves for a decaying pitched tone"),
            PwSynthMetricToString(Metric)), PwGetAudioAnalysisMetric(Analysis, Metric, Value));
    }

    // The accessor and the serializer must be reading the same facts, not two parallel copies.
    const TSharedPtr<FJsonObject> Summary = SerializeAudioAnalysis(Analysis, /*bFullDetail*/ false);
    const TSharedPtr<FJsonObject> Technical = GetObject(Summary, TEXT("technical"));
    const TSharedPtr<FJsonObject> Spectral = GetObject(Summary, TEXT("spectral"));
    TestTrue(TEXT("technical and spectral both serialized"), Technical.IsValid() && Spectral.IsValid());
    if (Technical.IsValid() && Spectral.IsValid())
    {
        double Value = 0.0;
        if (PwGetAudioAnalysisMetric(Analysis, EPwSynthMetric::PeakDb, Value))
        {
            // Rounded to two decimals on the wire, so the comparison carries that tolerance.
            TestTrue(TEXT("peakDb agrees between the accessor and the wire"),
                FMath::IsNearlyEqual(Value, Technical->GetNumberField(TEXT("peakDb")), 0.005));
        }
        if (PwGetAudioAnalysisMetric(Analysis, EPwSynthMetric::CentroidHz, Value))
        {
            TestTrue(TEXT("centroidHz agrees between the accessor and the wire"),
                FMath::IsNearlyEqual(Value, Spectral->GetNumberField(TEXT("centroidHz")), 0.05));
        }
    }

    return true;
}
