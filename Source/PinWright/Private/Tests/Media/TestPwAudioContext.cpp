// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the two context families (AudioGen/PwAudioContext.h).
//
// Ground truth is constructible here, so it is used rather than approximated. For a tone whose
// envelope decays exponentially and whose period divides the retrigger interval, the steady-state
// signal is the single copy scaled by exactly 1/(1 - r) with r = exp(-interval/tau) - so the
// expected accumulation is a closed-form number, not a plausible range. For a noise burst the
// copies are mutually uncorrelated, so their energies add and the same accumulation is only
// 1/sqrt(1 - r^2). That gap between coherent and incoherent addition is the physical reason low
// end builds faster than bright content, and it is what the low-band test measures.
//
// Four families of assertion are load-bearing rather than routine:
//   - BOTH DIRECTIONS (rpc-design.md §6). A short click and a long tail are asserted against each
//     other at the SAME rate. A one-sided test passes an analysis that always reports buildup.
//   - MEASURED ABSENCE OF BUILDUP (§1). The click asserts bMeasured TRUE with a ~0 dB delta. A
//     sound that genuinely does not accumulate must read as a measurement, not as a gap.
//   - ORDERING (§7). A buffer of NaN measures as digital silence under any implementation that
//     tests the silence floor first, because FMath::Max(0.0, NaN) is 0 and NaN fails every
//     comparison. The degenerate tests assert the specific non-finite code for exactly that buffer.
//   - OPT-IN ABSENCE. The default report is asserted to carry neither family under any key,
//     including `unmeasured` - "you did not ask" and "this could not be measured" are different
//     facts and the JSON must not collapse them.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioAnalysis.h"
#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioContext.h"
#include "Handlers/ErrorCodes.h"

#include "Dom/JsonObject.h"
#include "Math/RandomStream.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true. Deliberately distinct
// from the sibling suites' helper namespaces.
namespace PwAudioContextTestHelpers
{
    constexpr int32 TestSampleRate = 48000;

    /** Nepers per -60 dB. exp(-t/tau) reaches -60 dB at t = 6.9078 * tau. */
    constexpr double DecayNepersTo60Db = 6.907755278982137;

    FPwAudioBuffer MakeSilentBuffer(double Seconds)
    {
        FPwAudioBuffer Buffer;
        Buffer.SampleRate = TestSampleRate;
        Buffer.SetNumFrames(FMath::RoundToInt32(Seconds * TestSampleRate));
        return Buffer;
    }

    /**
     * A tone with an exponential decay that reaches the -60 dB envelope floor at TailMs, and an
     * optional raised-cosine attack.
     *
     * The attack ramp matters: an instantaneous edge is broadband by construction and survives any
     * highpass as a click, which is a true fact about hard-edged bass but not what the playback
     * tests are asking. AttackMs 0 is still click-free because the sine starts at phase 0.
     */
    FPwAudioBuffer MakeDecayingTone(double Seconds, double Hz, double TailMs, double Amplitude,
        double AttackMs = 0.0)
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        const double TauMs = TailMs / DecayNepersTo60Db;
        const double AngularStep = 2.0 * UE_DOUBLE_PI * Hz / TestSampleRate;
        const int32 AttackFrames = FMath::RoundToInt32(AttackMs * TestSampleRate / 1000.0);

        for (int32 Index = 0; Index < Buffer.NumFrames(); ++Index)
        {
            const double TimeMs = 1000.0 * Index / TestSampleRate;
            double Envelope = FMath::Exp(-TimeMs / TauMs);
            if (Index < AttackFrames)
            {
                Envelope *= 0.5 * (1.0 - FMath::Cos(UE_DOUBLE_PI * Index / AttackFrames));
            }
            const float Sample = static_cast<float>(
                Amplitude * Envelope * FMath::Sin(AngularStep * Index));
            Buffer.Left[Index] = Sample;
            Buffer.Right[Index] = Sample;
        }
        return Buffer;
    }

    /**
     * The incoherent counterpart of MakeDecayingTone: the same envelope over white noise. Copies
     * of it separated by a retrigger interval are mutually uncorrelated, so their energies add
     * rather than their amplitudes - which is exactly why a bright, noise-like tail does not
     * accumulate the way a low tonal tail does.
     */
    FPwAudioBuffer MakeDecayingNoise(double Seconds, double TailMs, double Amplitude, int32 Seed)
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        const double TauMs = TailMs / DecayNepersTo60Db;
        FRandomStream Stream(Seed);

        for (int32 Index = 0; Index < Buffer.NumFrames(); ++Index)
        {
            const double TimeMs = 1000.0 * Index / TestSampleRate;
            const double Envelope = FMath::Exp(-TimeMs / TauMs);
            const float Sample = static_cast<float>(
                Amplitude * Envelope * Stream.FRandRange(-1.f, 1.f));
            Buffer.Left[Index] = Sample;
            Buffer.Right[Index] = Sample;
        }
        return Buffer;
    }

    /** A short raised-cosine-windowed burst - a UI click. Placed away from frame 0 so the leading
        silence the analysis trims off is real. */
    FPwAudioBuffer MakeClick(double Seconds, double AtMs, double LengthMs, double Hz,
        double Amplitude)
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        const int32 Start = FMath::RoundToInt32(AtMs * TestSampleRate / 1000.0);
        const int32 Length = FMath::Max(1, FMath::RoundToInt32(LengthMs * TestSampleRate / 1000.0));
        const double AngularStep = 2.0 * UE_DOUBLE_PI * Hz / TestSampleRate;

        for (int32 Offset = 0; Offset < Length; ++Offset)
        {
            const int32 Index = Start + Offset;
            if (!Buffer.Left.IsValidIndex(Index))
            {
                break;
            }
            const double Window = 0.5 * (1.0 - FMath::Cos(2.0 * UE_DOUBLE_PI * Offset / Length));
            const float Sample = static_cast<float>(
                Amplitude * Window * FMath::Sin(AngularStep * Offset));
            Buffer.Left[Index] = Sample;
            Buffer.Right[Index] = Sample;
        }
        return Buffer;
    }

    /** Right is the exact negation of Left, so 0.5 * (L + R) is bitwise zero rather than nearly so. */
    FPwAudioBuffer MakeAntiPhaseTone(double Seconds, double Hz, double Amplitude)
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        const double AngularStep = 2.0 * UE_DOUBLE_PI * Hz / TestSampleRate;
        for (int32 Index = 0; Index < Buffer.NumFrames(); ++Index)
        {
            const float Sample = static_cast<float>(Amplitude * FMath::Sin(AngularStep * Index));
            Buffer.Left[Index] = Sample;
            Buffer.Right[Index] = -Sample;
        }
        return Buffer;
    }

    /**
     * NaN from its IEEE-754 bit pattern rather than from sqrt(-1): a constant-folding compiler is
     * entitled to diagnose or reshape that, and these tests need the exact bit pattern a real DSP
     * overflow produces. Same idiom TestPwAudioAnalysis.cpp uses, for the same reason.
     */
    float MakeNaN()
    {
        constexpr uint32 QuietNaNBits = 0x7FC00000u;
        float Value = 0.f;
        FMemory::Memcpy(&Value, &QuietNaNBits, sizeof(Value));
        return Value;
    }

    double MakeNaNDouble()
    {
        constexpr uint64 QuietNaNBits = 0x7FF8000000000000ull;
        double Value = 0.0;
        FMemory::Memcpy(&Value, &QuietNaNBits, sizeof(Value));
        return Value;
    }

    FPwAudioBuffer MakeNanBuffer(double Seconds)
    {
        FPwAudioBuffer Buffer = MakeSilentBuffer(Seconds);
        const float NaNSample = MakeNaN();
        for (int32 Index = 0; Index < Buffer.NumFrames(); ++Index)
        {
            Buffer.Left[Index] = NaNSample;
            Buffer.Right[Index] = NaNSample;
        }
        return Buffer;
    }

    /** The measured point for a rate, or nullptr. */
    const FPwRetriggerPoint* FindPoint(const FPwRetriggerResult& In, double RateHz)
    {
        for (const FPwRetriggerPoint& Point : In.Points)
        {
            if (FMath::IsNearlyEqual(Point.RateHz, RateHz, 1e-6))
            {
                return &Point;
            }
        }
        return nullptr;
    }

    const FPwConditionResult* FindCondition(const TArray<FPwConditionResult>& In,
        EPwPlaybackCondition Condition)
    {
        for (const FPwConditionResult& Result : In)
        {
            if (Result.Condition == Condition)
            {
                return &Result;
            }
        }
        return nullptr;
    }
}

// =============================================================================================
// Retrigger - both directions at one rate
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioContextRetriggerBothDirectionsTest,
    "PinWright.audio.context.RetriggerBuildupBothDirections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioContextRetriggerBothDirectionsTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioContextTestHelpers;

    const TArray<double> Rates({ 10.0 });

    // ---- direction 1: a click far shorter than the 100 ms interval cannot accumulate ----------
    // The steady-state window and the single-copy reference window contain the identical waveform
    // here, so the deltas are exactly zero. §1: that must be a MEASURED "no buildup" - bMeasured
    // true with a 0 dB delta - not an unmeasured gap, because "this sound is safe to spam" is the
    // answer the caller came for.
    const FPwAudioBuffer Click = MakeClick(1.0, /*AtMs*/ 100.0, /*LengthMs*/ 2.0, 2000.0, 0.5);

    FPwRetriggerResult ClickResult;
    FString Code;
    FString Message;
    const bool bClickMeasured = PwAnalyzeRetrigger(Click, Rates, ClickResult, Code, Message);
    TestTrue(FString::Printf(TEXT("click retrigger measured (%s: %s)"), *Code, *Message),
        bClickMeasured);
    TestTrue(TEXT("click family is MEASURED, not an unmeasured gap"), ClickResult.bMeasured);

    const FPwRetriggerPoint* ClickPoint = FindPoint(ClickResult, 10.0);
    if (!ClickPoint)
    {
        AddError(TEXT("no 10 Hz point for the click"));
        return false;
    }
    TestTrue(FString::Printf(TEXT("click peak delta is 0 dB, got %.4f"), ClickPoint->PeakDb),
        FMath::Abs(ClickPoint->PeakDb) < 0.05);
    TestTrue(FString::Printf(TEXT("click rms delta is 0 dB, got %.4f"), ClickPoint->RmsDb),
        FMath::Abs(ClickPoint->RmsDb) < 0.05);
    TestTrue(FString::Printf(TEXT("click low-band delta is 0 dB, got %.4f"), ClickPoint->LowBandDb),
        FMath::Abs(ClickPoint->LowBandDb) < 1.0);
    TestTrue(FString::Printf(TEXT("no energy past the interval, got %.6f"),
            ClickPoint->TailOverlapRatio),
        ClickPoint->TailOverlapRatio < 1e-6);
    TestFalse(TEXT("a 2 ms click at 10 Hz is not continuous"), ClickPoint->bEffectivelyContinuous);

    // ---- direction 2: a 1.5 s reverberant tail at the same rate stacks 15 copies deep ---------
    // tau = 1500 / 6.9078 = 217.2 ms, r = exp(-100 / 217.2) = 0.6310, and because 1 kHz has an
    // integer number of cycles in the 100 ms interval every copy lands in phase, so the closed
    // form is 1 / (1 - r) = 2.71 -> +8.66 dB.
    const FPwAudioBuffer Tail = MakeDecayingTone(2.0, 1000.0, /*TailMs*/ 1500.0, 0.3);

    FPwRetriggerResult TailResult;
    const bool bTailMeasured = PwAnalyzeRetrigger(Tail, Rates, TailResult, Code, Message);
    TestTrue(FString::Printf(TEXT("tail retrigger measured (%s: %s)"), *Code, *Message),
        bTailMeasured);

    const FPwRetriggerPoint* TailPoint = FindPoint(TailResult, 10.0);
    if (!TailPoint)
    {
        AddError(TEXT("no 10 Hz point for the tail"));
        return false;
    }
    TestTrue(FString::Printf(TEXT("tail peak delta is +8.66 dB, got %.3f"), TailPoint->PeakDb),
        FMath::IsNearlyEqual(TailPoint->PeakDb, 8.66, 1.0));
    TestTrue(FString::Printf(TEXT("tail rms delta is large and positive, got %.3f"),
            TailPoint->RmsDb),
        TailPoint->RmsDb > 4.0);
    TestTrue(FString::Printf(TEXT("most of the tail outlives its interval, got %.4f"),
            TailPoint->TailOverlapRatio),
        TailPoint->TailOverlapRatio > 0.3);

    // The contrast is the point. A metric that always reports buildup passes each half alone.
    TestTrue(FString::Printf(TEXT("tail accumulates far more than the click (%.3f vs %.3f dB)"),
            TailPoint->PeakDb, ClickPoint->PeakDb),
        TailPoint->PeakDb > ClickPoint->PeakDb + 5.0);

    return true;
}

// =============================================================================================
// Retrigger - monotonicity in rate
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioContextRetriggerMonotonicTest,
    "PinWright.audio.context.RetriggerPeakRisesWithRate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioContextRetriggerMonotonicTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioContextTestHelpers;

    // 600 ms tail -> tau 86.9 ms. Closed-form peak gain 1 / (1 - exp(-interval / tau)):
    // 2 Hz +0.03 dB, 5 Hz +0.92 dB, 10 Hz +3.31 dB, 20 Hz +7.17 dB. Amplitude 0.3 keeps even the
    // 20 Hz sum under full scale, so nothing is measured through a clipped peak.
    const FPwAudioBuffer Buffer = MakeDecayingTone(1.0, 1000.0, /*TailMs*/ 600.0, 0.3);

    FPwRetriggerResult Result;
    FString Code;
    FString Message;
    // Deliberately out of order: the points must come back ascending whatever order was asked.
    const bool bMeasured = PwAnalyzeRetrigger(Buffer, TArray<double>({ 10.0, 2.0, 20.0, 5.0 }),
        Result, Code, Message);
    TestTrue(FString::Printf(TEXT("retrigger measured (%s: %s)"), *Code, *Message), bMeasured);
    if (!bMeasured)
    {
        return false;
    }

    TestEqual(TEXT("one point per requested rate"), Result.Points.Num(), 4);
    if (Result.Points.Num() != 4)
    {
        return false;
    }

    for (int32 Index = 1; Index < Result.Points.Num(); ++Index)
    {
        TestTrue(FString::Printf(TEXT("points ascend in rate: %.3f then %.3f Hz"),
                Result.Points[Index - 1].RateHz, Result.Points[Index].RateHz),
            Result.Points[Index].RateHz > Result.Points[Index - 1].RateHz);

        // Non-strict with a small margin: what is being asserted is that the measure never goes
        // BACKWARDS as the sound is fired faster, which is the property a designer reasons with.
        TestTrue(FString::Printf(TEXT("peak buildup does not fall from %.3f Hz (%.3f dB) to ")
                TEXT("%.3f Hz (%.3f dB)"),
                Result.Points[Index - 1].RateHz, Result.Points[Index - 1].PeakDb,
                Result.Points[Index].RateHz, Result.Points[Index].PeakDb),
            Result.Points[Index].PeakDb >= Result.Points[Index - 1].PeakDb - 0.05);
    }

    // And the span is real, not four numbers within noise of each other.
    TestTrue(FString::Printf(TEXT("20 Hz builds far past 2 Hz (%.3f vs %.3f dB)"),
            Result.Points.Last().PeakDb, Result.Points[0].PeakDb),
        Result.Points.Last().PeakDb > Result.Points[0].PeakDb + 3.0);

    return true;
}

// =============================================================================================
// Retrigger - the low band builds fastest
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioContextRetriggerLowBandTest,
    "PinWright.audio.context.RetriggerLowBandBuildsFasterThanBright",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioContextRetriggerLowBandTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioContextTestHelpers;

    const TArray<double> Rates({ 10.0 });

    // Both sounds carry the same 800 ms envelope and are fired at the same rate, so the only
    // difference measured is HOW their overlapping tails add.
    //
    // Bass: 60 Hz has exactly 6 cycles in the 100 ms interval, so the copies land in phase and
    // their AMPLITUDES add: 1 / (1 - exp(-100/115.8)) = 1.727 -> +4.75 dB, and 60 Hz sits inside
    // the 150 Hz low band.
    // Bright: copies of a noise burst are mutually uncorrelated, so only their ENERGIES add:
    // 1 / sqrt(1 - exp(-200/115.8)) = 1.102 -> +0.85 dB, in the low band as everywhere else.
    // That factor-of-five gap in dB is the physical reason mud accumulates first.
    const FPwAudioBuffer Bass = MakeDecayingTone(1.5, 60.0, /*TailMs*/ 800.0, 0.4);
    const FPwAudioBuffer Bright = MakeDecayingNoise(1.5, /*TailMs*/ 800.0, 0.4, /*Seed*/ 20260818);

    FPwRetriggerResult BassResult;
    FPwRetriggerResult BrightResult;
    FString Code;
    FString Message;
    const bool bBass = PwAnalyzeRetrigger(Bass, Rates, BassResult, Code, Message);
    TestTrue(FString::Printf(TEXT("bass retrigger measured (%s: %s)"), *Code, *Message), bBass);
    const bool bBright = PwAnalyzeRetrigger(Bright, Rates, BrightResult, Code, Message);
    TestTrue(FString::Printf(TEXT("bright retrigger measured (%s: %s)"), *Code, *Message), bBright);
    if (!bBass || !bBright)
    {
        return false;
    }

    const FPwRetriggerPoint* BassPoint = FindPoint(BassResult, 10.0);
    const FPwRetriggerPoint* BrightPoint = FindPoint(BrightResult, 10.0);
    if (!BassPoint || !BrightPoint)
    {
        AddError(TEXT("missing 10 Hz point"));
        return false;
    }

    TestTrue(FString::Printf(TEXT("bass low band builds about +4.75 dB, got %.3f"),
            BassPoint->LowBandDb),
        FMath::IsNearlyEqual(BassPoint->LowBandDb, 4.75, 1.5));
    TestTrue(FString::Printf(TEXT("bright low band barely builds, got %.3f dB"),
            BrightPoint->LowBandDb),
        BrightPoint->LowBandDb < 2.0);
    TestTrue(FString::Printf(TEXT("bass low band builds far more than bright (%.3f vs %.3f dB)"),
            BassPoint->LowBandDb, BrightPoint->LowBandDb),
        BassPoint->LowBandDb > BrightPoint->LowBandDb + 2.0);

    return true;
}

// =============================================================================================
// Retrigger - the continuity verdict and the rate it is derived at
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioContextRetriggerContinuityTest,
    "PinWright.audio.context.RetriggerContinuityThresholdIsDerived",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioContextRetriggerContinuityTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioContextTestHelpers;

    // 200 ms tail -> tau 29.0 ms. Per-period envelope ripple is exp(-interval / tau): 58 dB at
    // 5 Hz, 29 dB at 10 Hz, 15 dB at 20 Hz. The verdict flips when the ripple stops reaching the
    // 20 dB ring-out boundary, which happens between 10 and 20 Hz.
    const FPwAudioBuffer Buffer = MakeDecayingTone(0.5, 1000.0, /*TailMs*/ 200.0, 0.3);

    FPwRetriggerResult Result;
    FString Code;
    FString Message;
    const bool bMeasured = PwAnalyzeRetrigger(Buffer, PwGetDefaultRetriggerRates(), Result,
        Code, Message);
    TestTrue(FString::Printf(TEXT("retrigger measured (%s: %s)"), *Code, *Message), bMeasured);
    if (!bMeasured)
    {
        return false;
    }

    const FPwRetriggerPoint* Slow = FindPoint(Result, 2.0);
    const FPwRetriggerPoint* Fast = FindPoint(Result, 20.0);
    if (!Slow || !Fast)
    {
        AddError(TEXT("missing 2 Hz or 20 Hz point"));
        return false;
    }

    TestFalse(TEXT("at 2 Hz a 200 ms tail leaves 300 ms of silence between hits"),
        Slow->bEffectivelyContinuous);
    TestTrue(TEXT("at 20 Hz the gaps have closed and it is a texture"),
        Fast->bEffectivelyContinuous);

    // Derived from the bracketing pair, not asserted as a constant.
    TestTrue(TEXT("the crossing was bracketed by the measured points"),
        Result.bContinuousRateMeasured);
    if (Result.bContinuousRateMeasured)
    {
        TestTrue(FString::Printf(TEXT("continuousAboveRateHz %.3f lies between 2 and 20 Hz"),
                Result.ContinuousAboveRateHz),
            Result.ContinuousAboveRateHz > 2.0 && Result.ContinuousAboveRateHz < 20.0);
    }

    // A rate ladder that never crosses cannot report a crossing. A steady tone with no decay is
    // continuous at every rate, so the true threshold sits below the lowest rate asked for and
    // extrapolating to it would be a fabricated number.
    const FPwAudioBuffer Steady = MakeDecayingTone(0.5, 1000.0, /*TailMs*/ 1.0e6, 0.3);
    FPwRetriggerResult SteadyResult;
    if (PwAnalyzeRetrigger(Steady, PwGetDefaultRetriggerRates(), SteadyResult, Code, Message))
    {
        TestTrue(TEXT("a sustained tone is continuous at the lowest rate too"),
            SteadyResult.Points.Num() > 0 && SteadyResult.Points[0].bEffectivelyContinuous);
        TestFalse(TEXT("an unbracketed crossing is not reported"),
            SteadyResult.bContinuousRateMeasured);
    }
    else
    {
        AddError(FString::Printf(TEXT("sustained-tone retrigger failed (%s: %s)"), *Code, *Message));
    }

    return true;
}

// =============================================================================================
// Playback conditions - low frequency versus mid band, both directions
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioContextPlaybackBandTest,
    "PinWright.audio.context.PlaybackConditionsLowFrequencyVanishesOnPhone",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioContextPlaybackBandTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioContextTestHelpers;

    // 50 ms raised-cosine attack on both, so neither carries a broadband edge that would survive
    // a highpass as a click - that would be a true fact about hard-edged bass, but it is not what
    // this test is asking. A ramp that long puts the attack's own spectrum three decades below the
    // 400 Hz phone corner, so what reaches the phone rendering is the attenuated carrier and
    // nothing else.
    const FPwAudioBuffer LowOnly = MakeDecayingTone(0.5, 60.0, /*TailMs*/ 300.0, 0.6,
        /*AttackMs*/ 50.0);
    const FPwAudioBuffer MidBand = MakeDecayingTone(0.5, 1200.0, /*TailMs*/ 300.0, 0.6,
        /*AttackMs*/ 50.0);

    TArray<FPwConditionResult> LowRows;
    TArray<FPwConditionResult> MidRows;
    FString Code;
    FString Message;
    const bool bLow = PwAnalyzePlaybackConditions(LowOnly, LowRows, Code, Message);
    TestTrue(FString::Printf(TEXT("low-frequency conditions measured (%s: %s)"), *Code, *Message),
        bLow);
    const bool bMid = PwAnalyzePlaybackConditions(MidBand, MidRows, Code, Message);
    TestTrue(FString::Printf(TEXT("mid-band conditions measured (%s: %s)"), *Code, *Message), bMid);
    if (!bLow || !bMid)
    {
        return false;
    }

    TestEqual(TEXT("one row per condition"), LowRows.Num(), 5);

    const FPwConditionResult* LowFull = FindCondition(LowRows, EPwPlaybackCondition::FullRange);
    const FPwConditionResult* LowPhone = FindCondition(LowRows, EPwPlaybackCondition::PhoneSpeaker);
    const FPwConditionResult* LowLaptop = FindCondition(LowRows, EPwPlaybackCondition::LaptopSpeaker);
    const FPwConditionResult* MidPhone = FindCondition(MidRows, EPwPlaybackCondition::PhoneSpeaker);
    const FPwConditionResult* MidLaptop = FindCondition(MidRows, EPwPlaybackCondition::LaptopSpeaker);
    if (!LowFull || !LowPhone || !LowLaptop || !MidPhone || !MidLaptop)
    {
        AddError(TEXT("a condition row is missing"));
        return false;
    }

    // The reference row is the identity, and it says so numerically rather than by convention.
    TestTrue(FString::Printf(TEXT("full range retains everything, got %.6f"),
            LowFull->RetainedEnergyRatio),
        FMath::IsNearlyEqual(LowFull->RetainedEnergyRatio, 1.0, 1e-6));
    TestTrue(TEXT("full range is audible"), LowFull->bAudible);
    TestTrue(TEXT("full range keeps its own transient"), LowFull->bTransientSurvives);

    // ---- direction 1: 60 Hz through a 400 Hz fourth-order highpass is -66 dB ------------------
    TestTrue(FString::Printf(TEXT("a 60 Hz sound barely survives a phone speaker, got %.8f"),
            LowPhone->RetainedEnergyRatio),
        LowPhone->RetainedEnergyRatio < 1e-3);
    TestFalse(TEXT("its transient does not survive the phone"), LowPhone->bTransientSurvives);
    TestFalse(TEXT("and it is not audible there at all"), LowPhone->bAudible);
    TestTrue(FString::Printf(TEXT("its peak drops hard on the phone, got %.2f dB"),
            LowPhone->PeakDbDelta),
        LowPhone->PeakDbDelta < -30.0);
    TestTrue(FString::Printf(TEXT("the laptop keeps more of it than the phone (%.8f vs %.8f)"),
            LowLaptop->RetainedEnergyRatio, LowPhone->RetainedEnergyRatio),
        LowLaptop->RetainedEnergyRatio > LowPhone->RetainedEnergyRatio);

    // ---- direction 2: 1200 Hz passes both approximations nearly untouched --------------------
    // Without this half, an analysis that reported every sound as destroyed would still pass.
    TestTrue(FString::Printf(TEXT("a 1200 Hz sound survives the phone, got %.4f"),
            MidPhone->RetainedEnergyRatio),
        MidPhone->RetainedEnergyRatio > 0.8);
    TestTrue(TEXT("its transient survives the phone"), MidPhone->bTransientSurvives);
    TestTrue(TEXT("it is audible on the phone"), MidPhone->bAudible);
    TestTrue(FString::Printf(TEXT("a 1200 Hz sound survives the laptop, got %.4f"),
            MidLaptop->RetainedEnergyRatio),
        MidLaptop->RetainedEnergyRatio > 0.9);
    TestTrue(TEXT("its transient survives the laptop"), MidLaptop->bTransientSurvives);

    // A rendering that still has a spectrum reports a centroid delta; one that does not is
    // asserted in the mono-cancellation test, which is the case where the difference matters.
    TestTrue(TEXT("the surviving mid-band rendering reports a centroid delta"),
        MidPhone->bCentroidMeasured);

    return true;
}

// =============================================================================================
// Playback conditions - the phase-cancellation catch
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioContextMonoCancellationTest,
    "PinWright.audio.context.PlaybackConditionsMonoCatchesPhaseCancellation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioContextMonoCancellationTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioContextTestHelpers;

    // Full scale on both channels and completely gone the moment they are summed. Nothing in the
    // single-shot report says so: peak, RMS, LUFS, centroid and flatness are all healthy.
    const FPwAudioBuffer AntiPhase = MakeAntiPhaseTone(0.5, 1000.0, 0.5);

    FPwPlaybackConditionSet Set;
    FString Code;
    FString Message;
    const bool bMeasured = PwAnalyzePlaybackConditionSet(AntiPhase, Set, Code, Message);
    TestTrue(FString::Printf(TEXT("conditions measured (%s: %s)"), *Code, *Message), bMeasured);
    if (!bMeasured)
    {
        return false;
    }

    const FPwConditionResult* Full = FindCondition(Set.Conditions, EPwPlaybackCondition::FullRange);
    const FPwConditionResult* Mono = FindCondition(Set.Conditions, EPwPlaybackCondition::Mono);
    if (!Full || !Mono)
    {
        AddError(TEXT("full-range or mono row missing"));
        return false;
    }

    TestTrue(TEXT("the sound is perfectly healthy at full range"), Full->bAudible);
    TestTrue(FString::Printf(TEXT("full range retains 1.0, got %.6f"), Full->RetainedEnergyRatio),
        FMath::IsNearlyEqual(Full->RetainedEnergyRatio, 1.0, 1e-6));

    TestTrue(FString::Printf(TEXT("mono fold-down leaves nothing, got %.10f"),
            Mono->RetainedEnergyRatio),
        Mono->RetainedEnergyRatio < 1e-6);
    TestFalse(TEXT("nothing is audible after the fold-down"), Mono->bAudible);
    TestFalse(TEXT("and no transient survives it"), Mono->bTransientSurvives);
    TestTrue(FString::Printf(TEXT("the peak collapses to the dB floor, got %.1f dB"),
            Mono->PeakDbDelta),
        Mono->PeakDbDelta < -100.0);

    // A signal that cancelled to nothing has no spectrum, so the centroid delta is ABSENT rather
    // than 0.0 - a 0 Hz shift would read as "brightness unchanged" for a sound that is gone.
    TestFalse(TEXT("a cancelled signal reports no centroid delta"), Mono->bCentroidMeasured);
    TestTrue(TEXT("the full-range reference centroid is published for the deltas"),
        Set.bReferenceCentroidMeasured);

    return true;
}

// =============================================================================================
// Failure direction (rpc-design.md §12)
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioContextDegenerateInputTest,
    "PinWright.audio.context.DegenerateInputsFailUnmeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioContextDegenerateInputTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioContextTestHelpers;

    const TArray<double> Rates = PwGetDefaultRetriggerRates();
    FString Code;
    FString Message;

    // ---- empty ------------------------------------------------------------------------------
    {
        FPwAudioBuffer Empty;
        Empty.SampleRate = TestSampleRate;

        FPwRetriggerResult Result;
        TestFalse(TEXT("an empty buffer cannot be retriggered"),
            PwAnalyzeRetrigger(Empty, Rates, Result, Code, Message));
        TestEqual(TEXT("empty buffer reports AUDIO_EMPTY_BUFFER"), Code,
            FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        TestFalse(TEXT("and leaves the family unmeasured"), Result.bMeasured);
        TestEqual(TEXT("with no points left behind"), Result.Points.Num(), 0);

        TArray<FPwConditionResult> Rows;
        TestFalse(TEXT("an empty buffer has no playback conditions"),
            PwAnalyzePlaybackConditions(Empty, Rows, Code, Message));
        TestEqual(TEXT("empty buffer reports AUDIO_EMPTY_BUFFER for conditions"), Code,
            FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        TestEqual(TEXT("and returns no rows"), Rows.Num(), 0);
    }

    // ---- non-finite, which must be caught BEFORE silence -------------------------------------
    // FMath::Max(0.0, NaN) returns 0, so an implementation that tested the silence floor first
    // would measure this buffer as digital silence and answer AUDIO_EMPTY_BUFFER. Asserting the
    // specific code for exactly this buffer is what pins the ordering.
    {
        const FPwAudioBuffer Nan = MakeNanBuffer(0.2);

        FPwRetriggerResult Result;
        TestFalse(TEXT("a NaN buffer cannot be retriggered"),
            PwAnalyzeRetrigger(Nan, Rates, Result, Code, Message));
        TestEqual(TEXT("NaN is named as NaN, not as silence"), Code,
            FString(ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES));
        TestFalse(TEXT("and leaves the family unmeasured"), Result.bMeasured);

        TArray<FPwConditionResult> Rows;
        TestFalse(TEXT("a NaN buffer has no playback conditions"),
            PwAnalyzePlaybackConditions(Nan, Rows, Code, Message));
        TestEqual(TEXT("NaN is named as NaN for conditions too"), Code,
            FString(ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES));
        TestEqual(TEXT("and returns no rows"), Rows.Num(), 0);
    }

    // ---- digital silence ---------------------------------------------------------------------
    {
        const FPwAudioBuffer Silence = MakeSilentBuffer(0.2);

        FPwRetriggerResult Result;
        TestFalse(TEXT("silence cannot accumulate"),
            PwAnalyzeRetrigger(Silence, Rates, Result, Code, Message));
        TestEqual(TEXT("silence reports AUDIO_EMPTY_BUFFER, the spelling loudness already uses"),
            Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        TestFalse(TEXT("and leaves the family unmeasured"), Result.bMeasured);
        TestTrue(TEXT("with a reason attached"), !Result.UnmeasuredReason.IsEmpty());
    }

    // ---- rate validation ---------------------------------------------------------------------
    {
        const FPwAudioBuffer Tone = MakeDecayingTone(0.3, 1000.0, 200.0, 0.4);

        FPwRetriggerResult Result;
        TestFalse(TEXT("a zero rate is not a rate"),
            PwAnalyzeRetrigger(Tone, TArray<double>({ 0.0 }), Result, Code, Message));
        TestEqual(TEXT("a zero rate reports INVALID_PARAMS"), Code,
            FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestFalse(TEXT("and leaves the family unmeasured"), Result.bMeasured);
        TestEqual(TEXT("with no points left behind"), Result.Points.Num(), 0);

        TestFalse(TEXT("a negative rate is not a rate"),
            PwAnalyzeRetrigger(Tone, TArray<double>({ -10.0 }), Result, Code, Message));
        TestEqual(TEXT("a negative rate reports INVALID_PARAMS"), Code,
            FString(ErrorCodes::ERR_INVALID_PARAMS));

        // A NaN rate must be rejected as non-finite rather than slipping through the range test,
        // which it would because every comparison against NaN is false.
        TestFalse(TEXT("a NaN rate is not a rate"),
            PwAnalyzeRetrigger(Tone, TArray<double>({ MakeNaNDouble() }), Result, Code, Message));
        TestEqual(TEXT("a NaN rate reports INVALID_PARAMS"), Code,
            FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestFalse(TEXT("and leaves the family unmeasured"), Result.bMeasured);

        TestFalse(TEXT("an empty rate set is rejected rather than defaulted silently"),
            PwAnalyzeRetrigger(Tone, TArray<double>(), Result, Code, Message));
        TestEqual(TEXT("an empty rate set reports INVALID_PARAMS"), Code,
            FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("and the message names the standard set as the way out"),
            Message.Contains(TEXT("10")) && Message.Contains(TEXT("20")));
    }

    return true;
}

// =============================================================================================
// Opt-in and omission
// =============================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioContextOptInTest,
    "PinWright.audio.context.ContextFamiliesAreOptIn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioContextOptInTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioContextTestHelpers;

    const FPwAudioBuffer Tone = MakeDecayingTone(0.4, 1000.0, 200.0, 0.4);

    FPwAudioAnalysis Analysis;
    FString Code;
    FString Message;
    const bool bAnalyzed = PwAnalyzeBuffer(Tone, Analysis, Code, Message);
    TestTrue(FString::Printf(TEXT("descriptor analysis succeeded (%s: %s)"), *Code, *Message),
        bAnalyzed);
    if (!bAnalyzed)
    {
        return false;
    }

    // ---- default: not requested, so absent from BOTH halves of the report --------------------
    TestFalse(TEXT("PwAnalyzeBuffer does not run the retrigger family"),
        Analysis.Retrigger.IsSet());
    TestFalse(TEXT("PwAnalyzeBuffer does not run the playback-condition family"),
        Analysis.PlaybackConditions.IsSet());
    {
        const TSharedPtr<FJsonObject> Summary = SerializeAudioAnalysis(Analysis,
            /*bFullDetail*/ false);
        TestFalse(TEXT("no retrigger key"), Summary->HasField(TEXT("retrigger")));
        TestFalse(TEXT("no playbackConditions key"), Summary->HasField(TEXT("playbackConditions")));

        // The stronger claim: an unrequested family is not listed as unmeasured either. "You did
        // not ask" and "this could not be measured" are different facts.
        if (Summary->HasTypedField<EJson::Object>(TEXT("unmeasured")))
        {
            const TSharedPtr<FJsonObject> Unmeasured = Summary->GetObjectField(TEXT("unmeasured"));
            TestFalse(TEXT("retrigger is not listed as unmeasured either"),
                Unmeasured->HasField(TEXT("retrigger")));
            TestFalse(TEXT("playbackConditions is not listed as unmeasured either"),
                Unmeasured->HasField(TEXT("playbackConditions")));
        }
    }

    // ---- opted in: both families published under their own keys ------------------------------
    {
        FPwAudioContextRequest Request;
        Request.bRetrigger = true;
        Request.bPlaybackConditions = true;

        FPwAudioAnalysis WithContext = Analysis;
        const bool bContext = PwAnalyzeAudioContext(Tone, Request, WithContext, Code, Message);
        TestTrue(FString::Printf(TEXT("context analysis succeeded (%s: %s)"), *Code, *Message),
            bContext);
        TestTrue(TEXT("the retrigger family is attached"), WithContext.Retrigger.IsSet());
        TestTrue(TEXT("the playback-condition family is attached"),
            WithContext.PlaybackConditions.IsSet());
        if (WithContext.Retrigger.IsSet())
        {
            TestEqual(TEXT("an empty request list uses the four standard rates"),
                WithContext.Retrigger.GetValue().Points.Num(), 4);
        }

        const TSharedPtr<FJsonObject> Summary = SerializeAudioAnalysis(WithContext,
            /*bFullDetail*/ false);
        TestTrue(TEXT("retrigger is published"),
            Summary->HasTypedField<EJson::Object>(TEXT("retrigger")));
        TestTrue(TEXT("playbackConditions is published"),
            Summary->HasTypedField<EJson::Object>(TEXT("playbackConditions")));

        const TSharedPtr<FJsonObject> Conditions =
            Summary->GetObjectField(TEXT("playbackConditions"));
        TestTrue(TEXT("the deltas travel with the reference they were taken against"),
            Conditions.IsValid() && Conditions->HasTypedField<EJson::Object>(TEXT("reference")));
    }

    // ---- requested but unmeasurable: listed under `unmeasured` with a reason ------------------
    // A silent buffer analyses fine as a descriptor and cannot be retriggered, which is exactly
    // the case that must not serialize as a wall of zero deltas.
    {
        const FPwAudioBuffer Silence = MakeSilentBuffer(0.2);

        FPwAudioAnalysis SilentAnalysis;
        TestTrue(TEXT("silence still analyses as a descriptor"),
            PwAnalyzeBuffer(Silence, SilentAnalysis, Code, Message));

        FPwAudioContextRequest Request;
        Request.bRetrigger = true;
        TestFalse(TEXT("but its retrigger family cannot be measured"),
            PwAnalyzeAudioContext(Silence, Request, SilentAnalysis, Code, Message));
        TestEqual(TEXT("and the failure code is forwarded"), Code,
            FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));

        TestTrue(TEXT("the family is attached, carrying its unmeasured state"),
            SilentAnalysis.Retrigger.IsSet());
        if (SilentAnalysis.Retrigger.IsSet())
        {
            TestFalse(TEXT("marked unmeasured"), SilentAnalysis.Retrigger.GetValue().bMeasured);
        }

        const TSharedPtr<FJsonObject> Summary = SerializeAudioAnalysis(SilentAnalysis,
            /*bFullDetail*/ false);
        TestFalse(TEXT("no retrigger block is published"), Summary->HasField(TEXT("retrigger")));
        TestTrue(TEXT("an unmeasured block exists"),
            Summary->HasTypedField<EJson::Object>(TEXT("unmeasured")));
        if (Summary->HasTypedField<EJson::Object>(TEXT("unmeasured")))
        {
            const TSharedPtr<FJsonObject> Unmeasured = Summary->GetObjectField(TEXT("unmeasured"));
            TestTrue(TEXT("retrigger is listed there with its reason"),
                Unmeasured->HasField(TEXT("retrigger"))
                && !Unmeasured->GetStringField(TEXT("retrigger")).IsEmpty());
        }
    }

    return true;
}
