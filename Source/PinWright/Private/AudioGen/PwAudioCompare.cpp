// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwAudioCompare - the difference report and analysis-by-resynthesis.
//
// The contract, the mapping table and every design argument live in PwAudioCompare.h. This file
// carries only the reasoning that belongs to the implementation: the mode matcher, the residual
// band level convention, and the two places where a number is derived rather than republished.
//
// THE MODE MATCHER
// ----------------
// Globally greedy, nearest-first, in CENTS. Every (reference, candidate) pair inside
// PwCompareLimits::ModeMatchToleranceCents is enumerated, sorted by |cents| ascending, and taken
// when neither side is already claimed. That is the same assignment PwTrackPartials uses to
// continue a track from one frame to the next, chosen here for the same reason: it agrees with
// an optimal assignment whenever the modes are resolvable at all, and when they are not, the
// closest pair is the one a listener would call the same mode.
//
// Cents rather than hertz is load-bearing, not tidy. A 40 Hz gap is a major sixth at 60 Hz and
// inaudible at 6 kHz, so a hertz window either loses every low mode or fuses every high one.
//
// Ties are broken by reference index then candidate index, so the report is deterministic and
// two runs diff cleanly.
//
// WHY UNFITTABLE ROWS NEVER ENTER THE MATCHER
// -------------------------------------------
// FPwModalFit's invariant is bMeasured == true <=> DecayMs > 0. A growing partial publishes a
// NEGATIVE DecayMs and a sustained one publishes 0 - both real measurements, neither a mode the
// modal bank can hold (PwGenModal rejects modeDecaysMs <= 0 outright). Matching them would
// produce "you are missing a mode at 4 kHz" for a partial the generator would refuse to render,
// so they are excluded and COUNTED instead.
//
// THE TWO DERIVED NUMBERS IN PwDecompositionToRecipe
// --------------------------------------------------
// 1. modeGainsDb is InitialGainDb extrapolated back to the layer's start along the fitted
//    exponential: GainAtZero = InitialGainDb + 60 * StartMs / DecayMs. The fit's level refers to
//    the track's first measured frame, and PwStft's frame-centre convention puts that half an
//    FFT window into the signal - 21.3 ms on the module's reference grid. Publishing the fitted
//    level unchanged as a t=0 gain would make every mode quiet by 60 * 21.3 / T60 dB, which is
//    5.1 dB for a 250 ms mode and grows as the mode gets shorter. The extrapolation is arithmetic
//    on two measured numbers, and it is what makes a render of the emitted recipe come back at
//    the reference's level rather than systematically under it.
//
//    The reference time comes from Partials[i].StartMs, which is index-aligned with Modes[i] by
//    the decomposition's own contract. FPwModalFit does not publish the time its level refers to
//    (the fit window starts at the track's amplitude PEAK, which for a measured - i.e. decaying -
//    row is at or within a frame or two of its first point), so StartMs is the closest published
//    stand-in. A row whose extrapolated gain leaves the schema's range is DROPPED rather than
//    clamped: a clamped gain is a level the sound does not have.
//
// 2. The noise layer's tilt correction. A residual band's level is the TOTAL POWER in the band,
//    and the bands are LOG-SPACED, so a band one octave up holds twice the bins and twice the
//    power of a white input. White therefore measures as +3.01 dB/octave on this axis, not 0.
//    PwToRecipeLimits::BandwidthTiltDbPerOctave is subtracted before the slope is matched against
//    the schema's five colours; without it every white residual would be authored as blue and
//    every pink one as white.

#include "AudioGen/PwAudioCompare.h"

#include "AudioGen/PwAudioAnalysis.h"
#include "Handlers/ErrorCodes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"

// Named (not anonymous) namespace: this module builds with bUseUnity = true, and anonymous
// namespaces in merged TUs are the ODR-collision source the plugin's Build.cs comment warns
// about. Distinct from the sibling analyzers' PwAudioDecomposeInternal /
// PwAudioDecomposeFitInternal for the same reason.
namespace PwAudioCompareInternal
{
    /** ln(2) and ln(10), for the cents and decibel conversions below. */
    constexpr double Ln2 = 0.69314718055994531;
    constexpr double Ln10 = 2.30258509299404568;

    /**
     * Smallest band-envelope power that is treated as a level rather than as nothing. Matches the
     * silence floor the rest of AudioGen uses, squared, because these are powers not amplitudes.
     */
    constexpr double SilencePower = 1e-18;

    /** Level reported for a band whose integrated power is below SilencePower, dBFS. */
    constexpr double SilentBandDb = -180.0;

    /** Union-of-breakpoints dedup spacing for the residual envelope, ms. */
    constexpr double EnvelopeTimeEpsilonMs = 0.5;

    /** Hard cap on the pre-decimation breakpoint union, so a pathological residual cannot hang. */
    constexpr int32 MaxEnvelopeUnionPoints = 8192;

    bool Fail(FString& OutErrorCode, FString& OutError, const FString& Code, FString&& Message)
    {
        OutErrorCode = Code;
        OutError = MoveTemp(Message);
        return false;
    }

    double ToDbFromPower(double Power)
    {
        return Power > SilencePower ? 10.0 * FMath::Loge(Power) / Ln10 : SilentBandDb;
    }

    double ToPowerFromDb(double Db)
    {
        return FMath::Pow(10.0, Db / 10.0);
    }

    /** 1200 * log2(To / From). Both arguments must be strictly positive. */
    double Cents(double From, double To)
    {
        return 1200.0 * FMath::Loge(To / From) / Ln2;
    }

    // ---- buffer sweeps -----------------------------------------------------------------------
    //
    // Split into single-purpose predicates so PwCompareAudio can run each of them across BOTH
    // buffers before moving to the next: the non-finite-before-silence ordering has to hold
    // across the pair, not merely inside one buffer.

    bool FindNonFinite(const FPwAudioBuffer& In, int32& OutFrame, const TCHAR*& OutChannel)
    {
        const int32 Num = FMath::Min(In.Left.Num(), In.Right.Num());
        for (int32 Index = 0; Index < Num; ++Index)
        {
            if (!FMath::IsFinite(In.Left[Index]))
            {
                OutFrame = Index;
                OutChannel = TEXT("left");
                return true;
            }
            if (!FMath::IsFinite(In.Right[Index]))
            {
                OutFrame = Index;
                OutChannel = TEXT("right");
                return true;
            }
        }
        return false;
    }

    double PeakAbs(const FPwAudioBuffer& In)
    {
        double Peak = 0.0;
        for (const float Sample : In.Left)
        {
            Peak = FMath::Max(Peak, FMath::Abs(static_cast<double>(Sample)));
        }
        for (const float Sample : In.Right)
        {
            Peak = FMath::Max(Peak, FMath::Abs(static_cast<double>(Sample)));
        }
        return Peak;
    }

    // ---- scalar comparison -------------------------------------------------------------------

    void MarkUnmeasured(FPwCompareScalar& Scalar, FString&& Reason)
    {
        Scalar = FPwCompareScalar();
        Scalar.UnmeasuredReason = MoveTemp(Reason);
    }

    void MarkMeasured(FPwCompareScalar& Scalar, double Reference, double Candidate)
    {
        Scalar.bMeasured = true;
        Scalar.UnmeasuredReason.Reset();
        Scalar.Reference = Reference;
        Scalar.Candidate = Candidate;
        Scalar.Delta = Candidate - Reference;
    }

    /**
     * Compare one family-gated scalar. The two family states decide whether the comparison can
     * happen at all, and the losing side is NAMED in the reason - "the candidate's loudness was
     * not measured" is actionable, "unmeasured" is not.
     */
    void CompareFamilyScalar(FPwCompareScalar& Out, const TCHAR* Quantity,
                             const FPwAudioFamilyState& ReferenceState, double ReferenceValue,
                             const FPwAudioFamilyState& CandidateState, double CandidateValue)
    {
        if (!ReferenceState.bMeasured)
        {
            MarkUnmeasured(Out, FString::Printf(TEXT("the reference's %s was not measured: %s"),
                Quantity, *ReferenceState.Reason));
            return;
        }
        if (!CandidateState.bMeasured)
        {
            MarkUnmeasured(Out, FString::Printf(TEXT("the candidate's %s was not measured: %s"),
                Quantity, *CandidateState.Reason));
            return;
        }
        MarkMeasured(Out, ReferenceValue, CandidateValue);
    }

    /** Same, for a TOptional inside a measured family - a sustained tone has no decay time. */
    void CompareOptionalScalar(FPwCompareScalar& Out, const TCHAR* Quantity,
                               const FPwAudioFamilyState& ReferenceState, const TOptional<double>& ReferenceValue,
                               const FPwAudioFamilyState& CandidateState, const TOptional<double>& CandidateValue)
    {
        if (!ReferenceState.bMeasured || !CandidateState.bMeasured)
        {
            const FPwAudioFamilyState& Loser = ReferenceState.bMeasured ? CandidateState : ReferenceState;
            const TCHAR* Side = ReferenceState.bMeasured ? TEXT("candidate") : TEXT("reference");
            MarkUnmeasured(Out, FString::Printf(TEXT("the %s's envelope was not measured, so %s ")
                TEXT("could not be compared: %s"), Side, Quantity, *Loser.Reason));
            return;
        }
        if (!ReferenceValue.IsSet())
        {
            MarkUnmeasured(Out, FString::Printf(TEXT("the reference has no %s - it never drops ")
                TEXT("below the envelope floor, so there is no span to compare against."), Quantity));
            return;
        }
        if (!CandidateValue.IsSet())
        {
            MarkUnmeasured(Out, FString::Printf(TEXT("the candidate has no %s - it never drops ")
                TEXT("below the envelope floor, which is itself the difference to fix."), Quantity));
            return;
        }
        MarkMeasured(Out, ReferenceValue.GetValue(), CandidateValue.GetValue());
    }

    FPwCompareDeviation MakeDeviation(const TCHAR* Diagnosis, FString&& Quantity, const TCHAR* Unit,
                                      double Reference, double Candidate, double Delta)
    {
        FPwCompareDeviation Deviation;
        Deviation.Diagnosis = Diagnosis;
        Deviation.Quantity = MoveTemp(Quantity);
        Deviation.Unit = Unit;
        Deviation.Reference = Reference;
        Deviation.Candidate = Candidate;
        Deviation.Delta = Delta;
        return Deviation;
    }

    /**
     * Emit a paired diagnosis for a signed scalar, or nothing when the difference is inside the
     * threshold. Both tokens are passed in so the caller states the two directions explicitly at
     * the call site, which is where a wrong pairing would be visible.
     */
    void EmitSignedDeviation(TArray<FPwCompareDeviation>& Out, const FPwCompareScalar& Scalar,
                             const TCHAR* Quantity, const TCHAR* Unit, double Tolerance,
                             const TCHAR* NegativeDiagnosis, const TCHAR* PositiveDiagnosis)
    {
        if (!Scalar.bMeasured || FMath::Abs(Scalar.Delta) <= Tolerance)
        {
            return;
        }
        Out.Add(MakeDeviation(Scalar.Delta < 0.0 ? NegativeDiagnosis : PositiveDiagnosis,
            Quantity, Unit, Scalar.Reference, Scalar.Candidate, Scalar.Delta));
    }

    /** max(absolute floor, relative share of the reference) - the timing threshold everywhere. */
    double TimeTolerance(double ReferenceMs)
    {
        return FMath::Max(PwCompareLimits::TimeToleranceMs,
            PwCompareLimits::TimeToleranceRatio * FMath::Abs(ReferenceMs));
    }

    // ---- residual band levels ------------------------------------------------------------------

    /**
     * Time-integrated mean power of a piecewise-linear dB envelope, expressed in dB.
     *
     * Mean rather than peak so the level is invariant to the clip's length - which is exactly the
     * property a comparison of two buffers of different durations needs. The integral trapezoids
     * the LINEAR power at the breakpoints; the envelope is piecewise-linear in dB, so this is an
     * approximation of the exact integral, and it is the same approximation on both sides of the
     * comparison, which is what the difference depends on.
     */
    bool BandMeanLevelDb(const TArray<FPwDbPoint>& Envelope, double& OutDb)
    {
        if (Envelope.Num() == 0)
        {
            return false;
        }
        if (Envelope.Num() == 1)
        {
            OutDb = Envelope[0].ValueDb;
            return true;
        }

        double PowerTime = 0.0;
        double TotalTime = 0.0;
        for (int32 Index = 1; Index < Envelope.Num(); ++Index)
        {
            const double Span = Envelope[Index].TimeMs - Envelope[Index - 1].TimeMs;
            if (!(Span > 0.0))
            {
                continue;
            }
            const double MeanPower = 0.5 * (ToPowerFromDb(Envelope[Index - 1].ValueDb)
                + ToPowerFromDb(Envelope[Index].ValueDb));
            PowerTime += MeanPower * Span;
            TotalTime += Span;
        }

        if (!(TotalTime > 0.0))
        {
            // Every breakpoint sits at the same instant. The arithmetic mean of the powers is the
            // only defensible answer, and it is still a measurement of what was there.
            double Sum = 0.0;
            for (const FPwDbPoint& Point : Envelope)
            {
                Sum += ToPowerFromDb(Point.ValueDb);
            }
            OutDb = ToDbFromPower(Sum / static_cast<double>(Envelope.Num()));
            return true;
        }

        OutDb = ToDbFromPower(PowerTime / TotalTime);
        return true;
    }

    /** Piecewise-linear dB envelope evaluated at TimeMs, held flat outside its own span. */
    double EvalEnvelopeDb(const TArray<FPwDbPoint>& Envelope, double TimeMs)
    {
        const int32 Num = Envelope.Num();
        if (Num == 0)
        {
            return SilentBandDb;
        }
        if (TimeMs <= Envelope[0].TimeMs)
        {
            return Envelope[0].ValueDb;
        }
        if (TimeMs >= Envelope[Num - 1].TimeMs)
        {
            return Envelope[Num - 1].ValueDb;
        }
        for (int32 Index = 1; Index < Num; ++Index)
        {
            if (TimeMs <= Envelope[Index].TimeMs)
            {
                const double Span = Envelope[Index].TimeMs - Envelope[Index - 1].TimeMs;
                if (!(Span > 0.0))
                {
                    return Envelope[Index].ValueDb;
                }
                const double Alpha = (TimeMs - Envelope[Index - 1].TimeMs) / Span;
                return FMath::Lerp(Envelope[Index - 1].ValueDb, Envelope[Index].ValueDb, Alpha);
            }
        }
        return Envelope[Num - 1].ValueDb;
    }

    // ---- mode matching -------------------------------------------------------------------------

    /** One eligible mode: a fitted row the modal bank could actually hold. */
    struct FEligibleMode
    {
        int32 Index = INDEX_NONE;
        double FreqHz = 0.0;
        double GainDb = 0.0;
        double DecayMs = 0.0;
    };

    /**
     * Split a decomposition's fits into "modes the bank can hold" and a count of everything else.
     * The invariant bMeasured == true <=> DecayMs > 0 is re-checked rather than trusted: this
     * function is reachable from a hand-built decomposition, and a row with bMeasured true and a
     * zero decay would pair against a reference mode and then be reported as a decay ratio of
     * infinity.
     */
    void CollectEligibleModes(const FPwDecomposition& In, TArray<FEligibleMode>& Out, int32& OutUnfittable)
    {
        Out.Reset();
        OutUnfittable = 0;
        for (int32 Index = 0; Index < In.Modes.Num(); ++Index)
        {
            const FPwModalFit& Fit = In.Modes[Index];
            if (!Fit.bMeasured || !(Fit.DecayMs > 0.0) || !(Fit.FreqHz > 0.0)
                || !FMath::IsFinite(Fit.FreqHz) || !FMath::IsFinite(Fit.DecayMs)
                || !FMath::IsFinite(Fit.InitialGainDb))
            {
                ++OutUnfittable;
                continue;
            }
            FEligibleMode& Mode = Out.AddDefaulted_GetRef();
            Mode.Index = Index;
            Mode.FreqHz = Fit.FreqHz;
            Mode.GainDb = Fit.InitialGainDb;
            Mode.DecayMs = Fit.DecayMs;
        }
    }

    /** One candidate assignment, sorted by |cents| so the closest pair is taken first. */
    struct FModeCandidatePair
    {
        int32 ReferenceSlot = INDEX_NONE;   // index into the eligible reference array
        int32 CandidateSlot = INDEX_NONE;
        double AbsCents = 0.0;
        double SignedCents = 0.0;
    };
}

const TArray<FString>& PwCompareDiagnosisVocabulary()
{
    static const TArray<FString> Vocabulary = {
        PwCompareDiagnosis::OnsetTooEarly,      PwCompareDiagnosis::OnsetTooLate,
        PwCompareDiagnosis::AttackTooSharp,     PwCompareDiagnosis::AttackTooSoft,
        PwCompareDiagnosis::DecayTooShort,      PwCompareDiagnosis::DecayTooLong,
        PwCompareDiagnosis::TailTooShort,       PwCompareDiagnosis::TailTooLong,
        PwCompareDiagnosis::TooLoud,            PwCompareDiagnosis::TooQuiet,
        PwCompareDiagnosis::CandidateTooBright, PwCompareDiagnosis::CandidateTooDark,
        PwCompareDiagnosis::MissingMode,        PwCompareDiagnosis::ExtraMode,
        PwCompareDiagnosis::ModeTooSharp,       PwCompareDiagnosis::ModeTooFlat,
        PwCompareDiagnosis::ModeTooLoud,        PwCompareDiagnosis::ModeTooQuiet,
        PwCompareDiagnosis::ModeDecayTooShort,  PwCompareDiagnosis::ModeDecayTooLong,
        PwCompareDiagnosis::ResidualExcess,     PwCompareDiagnosis::ResidualDeficit
    };
    return Vocabulary;
}

bool PwIsKnownCompareDiagnosis(const FString& Diagnosis)
{
    return PwCompareDiagnosisVocabulary().Contains(Diagnosis);
}

bool PwCompareAudio(const FPwAudioBuffer& Reference, const FPwAudioBuffer& Candidate,
                    FPwCompareResult& Out, FString& OutErrorCode, FString& OutError)
{
    using namespace PwAudioCompareInternal;

    // Failure is the default: a caller that ignores the returned bool reads an unmeasured report,
    // never a stale or half-filled one (rpc-design.md §1).
    Out = FPwCompareResult();
    OutErrorCode.Reset();
    OutError.Reset();

    // ---- degenerate cases, ANSWERED in order (rpc-design.md §7) -------------------------------
    // 1. existence. Zero frames is not a quiet sound, it is an analysis with no input.
    if (Reference.NumFrames() <= 0 || Candidate.NumFrames() <= 0)
    {
        const TCHAR* Side = Reference.NumFrames() <= 0
            ? (Candidate.NumFrames() <= 0 ? TEXT("both buffers have") : TEXT("the reference has"))
            : TEXT("the candidate has");
        Out.UnmeasuredReason = FString::Printf(TEXT("%s no frames"), Side);
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            FString::Printf(TEXT("Compare: %s no frames (reference %d, candidate %d). An empty ")
                TEXT("buffer is not a silent one - there is nothing to compare."),
                Side, Reference.NumFrames(), Candidate.NumFrames()));
    }

    // 2. non-finite, BEFORE silence and deliberately so. NaN compares false against every
    //    threshold, so FMath::Max(0.0, NaN) returns 0 and a NaN-filled buffer scanned for its
    //    peak first measures as digital silence - which would send the caller to look at their
    //    gain staging instead of at the generator that produced the NaN.
    for (int32 Side = 0; Side < 2; ++Side)
    {
        const FPwAudioBuffer& Buffer = (Side == 0) ? Reference : Candidate;
        const TCHAR* Name = (Side == 0) ? TEXT("reference") : TEXT("candidate");
        int32 Frame = INDEX_NONE;
        const TCHAR* Channel = TEXT("");
        if (FindNonFinite(Buffer, Frame, Channel))
        {
            Out.UnmeasuredReason = FString::Printf(TEXT("the %s holds non-finite samples"), Name);
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES,
                FString::Printf(TEXT("Compare: the %s buffer holds a non-finite sample at frame ")
                    TEXT("%d of the %s channel. One NaN poisons every sum, every FFT bin and ")
                    TEXT("every threshold downstream, and it compares false against all of them, ")
                    TEXT("so it is named here rather than measured as silence."),
                    Name, Frame, Channel));
        }
    }

    // 3. digital silence. An ERROR here, unlike in PwAnalyzeBuffer: a comparison against silence
    //    produces no deviations at all, which reads as "these match" - the one false report this
    //    verb must never produce.
    for (int32 Side = 0; Side < 2; ++Side)
    {
        const FPwAudioBuffer& Buffer = (Side == 0) ? Reference : Candidate;
        const TCHAR* Name = (Side == 0) ? TEXT("reference") : TEXT("candidate");
        const double Peak = PeakAbs(Buffer);
        if (!(Peak > PwAudioAnalysisLimits::SilencePeak))
        {
            Out.UnmeasuredReason = FString::Printf(TEXT("the %s is digitally silent"), Name);
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
                FString::Printf(TEXT("Compare: the %s buffer is digitally silent - its peak ")
                    TEXT("sample is %.3g, below the %.3g silence floor. A comparison against ")
                    TEXT("silence finds no deviations, which would read as a match."),
                    Name, Peak, PwAudioAnalysisLimits::SilencePeak));
        }
    }

    // 4. malformed.
    for (int32 Side = 0; Side < 2; ++Side)
    {
        const FPwAudioBuffer& Buffer = (Side == 0) ? Reference : Candidate;
        const TCHAR* Name = (Side == 0) ? TEXT("reference") : TEXT("candidate");
        if (!Buffer.IsValid())
        {
            Out.UnmeasuredReason = FString::Printf(TEXT("the %s buffer is malformed"), Name);
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("Compare: the %s buffer is malformed - %d left samples, %d ")
                    TEXT("right, sample rate %d. Both channels must agree in length and the rate ")
                    TEXT("must be positive."),
                    Name, Buffer.Left.Num(), Buffer.Right.Num(), Buffer.SampleRate));
        }
    }

    // 5. sample-rate mismatch. Refused, never resampled: resampling behind the caller's back
    //    would compare a signal they never passed, which is the stance FPwAudioBuffer::MixInto
    //    already takes at the same seam.
    if (Reference.SampleRate != Candidate.SampleRate)
    {
        Out.UnmeasuredReason = TEXT("the two buffers have different sample rates");
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Compare: the reference is at %d Hz and the candidate at %d Hz. ")
                TEXT("Resample one of them to the other's rate before comparing - resampling here ")
                TEXT("would report a comparison of a signal you did not pass."),
                Reference.SampleRate, Candidate.SampleRate));
    }

    // ---- measure both sides -------------------------------------------------------------------
    FPwAudioAnalysis ReferenceAnalysis;
    FPwAudioAnalysis CandidateAnalysis;
    for (int32 Side = 0; Side < 2; ++Side)
    {
        const FPwAudioBuffer& Buffer = (Side == 0) ? Reference : Candidate;
        FPwAudioAnalysis& Analysis = (Side == 0) ? ReferenceAnalysis : CandidateAnalysis;
        const TCHAR* Name = (Side == 0) ? TEXT("reference") : TEXT("candidate");

        FString StageCode;
        FString StageError;
        if (!PwAnalyzeBuffer(Buffer, Analysis, StageCode, StageError))
        {
            Out.UnmeasuredReason = FString::Printf(TEXT("the %s could not be analysed"), Name);
            return Fail(OutErrorCode, OutError, StageCode,
                FString::Printf(TEXT("Compare: the %s buffer could not be analysed. %s"),
                    Name, *StageError));
        }
    }

    const FPwDecomposeSettings Settings;
    FPwDecomposition ReferenceDecomposition;
    FPwDecomposition CandidateDecomposition;
    for (int32 Side = 0; Side < 2; ++Side)
    {
        const FPwAudioBuffer& Buffer = (Side == 0) ? Reference : Candidate;
        FPwDecomposition& Decomposition = (Side == 0) ? ReferenceDecomposition : CandidateDecomposition;
        const TCHAR* Name = (Side == 0) ? TEXT("reference") : TEXT("candidate");

        FString StageCode;
        FString StageError;
        if (!PwDecomposeBuffer(Buffer, Settings, Decomposition, StageCode, StageError)
            || !Decomposition.bMeasured)
        {
            // A partial comparison is not a cheaper comparison: the mode diff is the reason this
            // verb exists, and a report missing it would still look complete (rpc-design.md §1).
            Out.UnmeasuredReason = FString::Printf(TEXT("the %s could not be decomposed"), Name);
            return Fail(OutErrorCode, OutError,
                StageCode.IsEmpty() ? FString(ErrorCodes::ERR_INVALID_PARAMS) : StageCode,
                FString::Printf(TEXT("Compare: the %s buffer could not be decomposed, so the ")
                    TEXT("per-mode difference - the reason to compare at all - cannot be ")
                    TEXT("reported. %s"), Name,
                    StageError.IsEmpty() ? *Decomposition.UnmeasuredReason : *StageError));
        }
    }

    Out.SampleRate = Reference.SampleRate;
    Out.ReferenceDurationMs = ReferenceAnalysis.Technical.DurationMs;
    Out.CandidateDurationMs = CandidateAnalysis.Technical.DurationMs;

    // ---- scalar comparisons --------------------------------------------------------------------
    CompareFamilyScalar(Out.OnsetMs, TEXT("envelope"),
        ReferenceAnalysis.Envelope.State, ReferenceAnalysis.Envelope.OnsetMs,
        CandidateAnalysis.Envelope.State, CandidateAnalysis.Envelope.OnsetMs);
    CompareFamilyScalar(Out.AttackMs, TEXT("envelope"),
        ReferenceAnalysis.Envelope.State, ReferenceAnalysis.Envelope.AttackMs,
        CandidateAnalysis.Envelope.State, CandidateAnalysis.Envelope.AttackMs);
    CompareOptionalScalar(Out.DecayMs, TEXT("decayMs"),
        ReferenceAnalysis.Envelope.State, ReferenceAnalysis.Envelope.DecayMs,
        CandidateAnalysis.Envelope.State, CandidateAnalysis.Envelope.DecayMs);
    CompareOptionalScalar(Out.TailMs, TEXT("tailMs"),
        ReferenceAnalysis.Envelope.State, ReferenceAnalysis.Envelope.TailMs,
        CandidateAnalysis.Envelope.State, CandidateAnalysis.Envelope.TailMs);
    CompareFamilyScalar(Out.LoudnessLufs, TEXT("loudness"),
        ReferenceAnalysis.Loudness.State, ReferenceAnalysis.Loudness.IntegratedLufs,
        CandidateAnalysis.Loudness.State, CandidateAnalysis.Loudness.IntegratedLufs);
    CompareFamilyScalar(Out.CentroidHz, TEXT("spectrum"),
        ReferenceAnalysis.Spectral.State, ReferenceAnalysis.Spectral.CentroidHz,
        CandidateAnalysis.Spectral.State, CandidateAnalysis.Spectral.CentroidHz);

    if (Out.CentroidHz.bMeasured && Out.CentroidHz.Reference > 0.0 && Out.CentroidHz.Candidate > 0.0)
    {
        Out.CentroidDeltaCents = Cents(Out.CentroidHz.Reference, Out.CentroidHz.Candidate);
    }

    // ---- mode matching ---------------------------------------------------------------------------
    TArray<FEligibleMode> ReferenceModes;
    TArray<FEligibleMode> CandidateModes;
    CollectEligibleModes(ReferenceDecomposition, ReferenceModes, Out.ReferenceUnfittableModes);
    CollectEligibleModes(CandidateDecomposition, CandidateModes, Out.CandidateUnfittableModes);

    TArray<FModeCandidatePair> Pairs;
    Pairs.Reserve(ReferenceModes.Num() * CandidateModes.Num());
    for (int32 R = 0; R < ReferenceModes.Num(); ++R)
    {
        for (int32 C = 0; C < CandidateModes.Num(); ++C)
        {
            const double SignedCents = Cents(ReferenceModes[R].FreqHz, CandidateModes[C].FreqHz);
            if (!FMath::IsFinite(SignedCents)
                || FMath::Abs(SignedCents) > PwCompareLimits::ModeMatchToleranceCents)
            {
                continue;
            }
            FModeCandidatePair& Pair = Pairs.AddDefaulted_GetRef();
            Pair.ReferenceSlot = R;
            Pair.CandidateSlot = C;
            Pair.AbsCents = FMath::Abs(SignedCents);
            Pair.SignedCents = SignedCents;
        }
    }

    // Nearest first, with a deterministic tie-break so two runs of the same comparison produce
    // byte-identical reports.
    Pairs.Sort([](const FModeCandidatePair& A, const FModeCandidatePair& B)
    {
        if (A.AbsCents != B.AbsCents)
        {
            return A.AbsCents < B.AbsCents;
        }
        if (A.ReferenceSlot != B.ReferenceSlot)
        {
            return A.ReferenceSlot < B.ReferenceSlot;
        }
        return A.CandidateSlot < B.CandidateSlot;
    });

    TArray<bool> ReferenceClaimed;
    ReferenceClaimed.Init(false, ReferenceModes.Num());
    TArray<bool> CandidateClaimed;
    CandidateClaimed.Init(false, CandidateModes.Num());

    for (const FModeCandidatePair& Pair : Pairs)
    {
        if (ReferenceClaimed[Pair.ReferenceSlot] || CandidateClaimed[Pair.CandidateSlot])
        {
            continue;
        }
        ReferenceClaimed[Pair.ReferenceSlot] = true;
        CandidateClaimed[Pair.CandidateSlot] = true;

        const FEligibleMode& Ref = ReferenceModes[Pair.ReferenceSlot];
        const FEligibleMode& Cand = CandidateModes[Pair.CandidateSlot];

        FPwCompareModePair& Row = Out.ModePairs.AddDefaulted_GetRef();
        Row.ReferenceIndex = Ref.Index;
        Row.CandidateIndex = Cand.Index;
        Row.ReferenceFreqHz = Ref.FreqHz;
        Row.CandidateFreqHz = Cand.FreqHz;
        Row.FreqDeltaCents = Pair.SignedCents;
        Row.ReferenceGainDb = Ref.GainDb;
        Row.CandidateGainDb = Cand.GainDb;
        Row.GainDeltaDb = Cand.GainDb - Ref.GainDb;
        Row.ReferenceDecayMs = Ref.DecayMs;
        Row.CandidateDecayMs = Cand.DecayMs;
        Row.DecayRatio = Cand.DecayMs / Ref.DecayMs;
    }

    Out.ModePairs.Sort([](const FPwCompareModePair& A, const FPwCompareModePair& B)
    {
        return A.ReferenceFreqHz < B.ReferenceFreqHz;
    });

    // BOTH sides of the leftover set. A reference mode with no counterpart says "add a mode";
    // a candidate mode with no counterpart says "remove one". Reporting only one of the two
    // would answer half the question and look complete doing it.
    for (int32 R = 0; R < ReferenceModes.Num(); ++R)
    {
        if (ReferenceClaimed[R])
        {
            continue;
        }
        FPwCompareUnmatchedMode& Row = Out.MissingModes.AddDefaulted_GetRef();
        Row.Index = ReferenceModes[R].Index;
        Row.FreqHz = ReferenceModes[R].FreqHz;
        Row.GainDb = ReferenceModes[R].GainDb;
        Row.DecayMs = ReferenceModes[R].DecayMs;
        Row.Diagnosis = PwCompareDiagnosis::MissingMode;
    }
    for (int32 C = 0; C < CandidateModes.Num(); ++C)
    {
        if (CandidateClaimed[C])
        {
            continue;
        }
        FPwCompareUnmatchedMode& Row = Out.ExtraModes.AddDefaulted_GetRef();
        Row.Index = CandidateModes[C].Index;
        Row.FreqHz = CandidateModes[C].FreqHz;
        Row.GainDb = CandidateModes[C].GainDb;
        Row.DecayMs = CandidateModes[C].DecayMs;
        Row.Diagnosis = PwCompareDiagnosis::ExtraMode;
    }

    Out.MissingModes.Sort([](const FPwCompareUnmatchedMode& A, const FPwCompareUnmatchedMode& B)
    {
        return A.FreqHz < B.FreqHz;
    });
    Out.ExtraModes.Sort([](const FPwCompareUnmatchedMode& A, const FPwCompareUnmatchedMode& B)
    {
        return A.FreqHz < B.FreqHz;
    });

    // ---- residual bands ---------------------------------------------------------------------------
    {
        const TArray<FPwResidualBand>& Ref = ReferenceDecomposition.Residual;
        const TArray<FPwResidualBand>& Cand = CandidateDecomposition.Residual;
        if (Ref.Num() == 0 || Cand.Num() == 0)
        {
            Out.ResidualUnmeasuredReason = FString::Printf(
                TEXT("one side has no residual bands (reference %d, candidate %d), so there is no ")
                TEXT("shared frequency axis to report excess or deficit on."), Ref.Num(), Cand.Num());
        }
        else if (Ref.Num() != Cand.Num())
        {
            Out.ResidualUnmeasuredReason = FString::Printf(
                TEXT("the two decompositions landed on different band grids (%d bands against ")
                TEXT("%d). Comparing band to band across two axes would publish a frequency ")
                TEXT("difference as a level difference."), Ref.Num(), Cand.Num());
        }
        else
        {
            int32 Mismatch = INDEX_NONE;
            for (int32 Index = 0; Index < Ref.Num(); ++Index)
            {
                const double LowScale = FMath::Max(FMath::Abs(Ref[Index].LowHz), 1.0);
                const double HighScale = FMath::Max(FMath::Abs(Ref[Index].HighHz), 1.0);
                if (FMath::Abs(Ref[Index].LowHz - Cand[Index].LowHz)
                        > PwCompareLimits::ResidualEdgeAgreement * LowScale
                    || FMath::Abs(Ref[Index].HighHz - Cand[Index].HighHz)
                        > PwCompareLimits::ResidualEdgeAgreement * HighScale)
                {
                    Mismatch = Index;
                    break;
                }
            }

            if (Mismatch != INDEX_NONE)
            {
                Out.ResidualUnmeasuredReason = FString::Printf(
                    TEXT("band %d covers %.1f-%.1f Hz in the reference but %.1f-%.1f Hz in the ")
                    TEXT("candidate; the two band grids do not describe the same frequencies."),
                    Mismatch, Ref[Mismatch].LowHz, Ref[Mismatch].HighHz,
                    Cand[Mismatch].LowHz, Cand[Mismatch].HighHz);
            }
            else
            {
                bool bAllLevelsMeasured = true;
                for (int32 Index = 0; Index < Ref.Num() && bAllLevelsMeasured; ++Index)
                {
                    double ReferenceDb = 0.0;
                    double CandidateDb = 0.0;
                    if (!BandMeanLevelDb(Ref[Index].EnvelopeDb, ReferenceDb)
                        || !BandMeanLevelDb(Cand[Index].EnvelopeDb, CandidateDb))
                    {
                        bAllLevelsMeasured = false;
                        Out.ResidualBands.Reset();
                        Out.ResidualUnmeasuredReason = FString::Printf(
                            TEXT("band %d (%.1f-%.1f Hz) carries an empty level envelope on at ")
                            TEXT("least one side, so it has no level to compare."),
                            Index, Ref[Index].LowHz, Ref[Index].HighHz);
                        break;
                    }
                    FPwCompareResidualBand& Row = Out.ResidualBands.AddDefaulted_GetRef();
                    Row.LowHz = Ref[Index].LowHz;
                    Row.HighHz = Ref[Index].HighHz;
                    Row.ReferenceLevelDb = ReferenceDb;
                    Row.CandidateLevelDb = CandidateDb;
                    Row.DeltaDb = CandidateDb - ReferenceDb;
                }
                Out.bResidualMeasured = bAllLevelsMeasured;
            }
        }
    }

    // ---- the deviation list, in a fixed order ------------------------------------------------------
    EmitSignedDeviation(Out.Deviations, Out.OnsetMs, TEXT("envelope.onsetMs"), TEXT("ms"),
        TimeTolerance(Out.OnsetMs.Reference),
        PwCompareDiagnosis::OnsetTooEarly, PwCompareDiagnosis::OnsetTooLate);
    EmitSignedDeviation(Out.Deviations, Out.AttackMs, TEXT("envelope.attackMs"), TEXT("ms"),
        TimeTolerance(Out.AttackMs.Reference),
        PwCompareDiagnosis::AttackTooSharp, PwCompareDiagnosis::AttackTooSoft);
    EmitSignedDeviation(Out.Deviations, Out.DecayMs, TEXT("envelope.decayMs"), TEXT("ms"),
        TimeTolerance(Out.DecayMs.Reference),
        PwCompareDiagnosis::DecayTooShort, PwCompareDiagnosis::DecayTooLong);
    EmitSignedDeviation(Out.Deviations, Out.TailMs, TEXT("envelope.tailMs"), TEXT("ms"),
        TimeTolerance(Out.TailMs.Reference),
        PwCompareDiagnosis::TailTooShort, PwCompareDiagnosis::TailTooLong);
    EmitSignedDeviation(Out.Deviations, Out.LoudnessLufs, TEXT("loudness.integratedLufs"), TEXT("lu"),
        PwCompareLimits::LoudnessToleranceLu,
        PwCompareDiagnosis::TooQuiet, PwCompareDiagnosis::TooLoud);

    // Brightness is thresholded in the LOG domain and reported there too: +200 Hz is a different
    // perceptual distance at 300 Hz than at 6 kHz, so a hertz threshold would flag one and miss
    // the other. The pair itself stays in hertz, which is the unit the caller sets things in.
    if (Out.CentroidHz.bMeasured && Out.CentroidDeltaCents.IsSet())
    {
        const double DeltaCents = Out.CentroidDeltaCents.GetValue();
        if (FMath::Abs(DeltaCents) > PwCompareLimits::CentroidToleranceCents)
        {
            Out.Deviations.Add(MakeDeviation(
                DeltaCents < 0.0 ? PwCompareDiagnosis::CandidateTooDark
                                 : PwCompareDiagnosis::CandidateTooBright,
                TEXT("spectral.centroidHz"), TEXT("cents"),
                Out.CentroidHz.Reference, Out.CentroidHz.Candidate, DeltaCents));
        }
    }

    for (int32 Index = 0; Index < Out.ModePairs.Num(); ++Index)
    {
        const FPwCompareModePair& Row = Out.ModePairs[Index];
        if (FMath::Abs(Row.FreqDeltaCents) > PwCompareLimits::ModeFreqToleranceCents)
        {
            Out.Deviations.Add(MakeDeviation(
                Row.FreqDeltaCents < 0.0 ? PwCompareDiagnosis::ModeTooFlat
                                         : PwCompareDiagnosis::ModeTooSharp,
                FString::Printf(TEXT("modes[%d].freqHz"), Index), TEXT("cents"),
                Row.ReferenceFreqHz, Row.CandidateFreqHz, Row.FreqDeltaCents));
        }
        if (FMath::Abs(Row.GainDeltaDb) > PwCompareLimits::ModeGainToleranceDb)
        {
            Out.Deviations.Add(MakeDeviation(
                Row.GainDeltaDb < 0.0 ? PwCompareDiagnosis::ModeTooQuiet
                                      : PwCompareDiagnosis::ModeTooLoud,
                FString::Printf(TEXT("modes[%d].gainDb"), Index), TEXT("db"),
                Row.ReferenceGainDb, Row.CandidateGainDb, Row.GainDeltaDb));
        }
        // A ratio, not a difference: "half as long" is the actionable statement about a decay,
        // and -250 ms means something different at a 300 ms T60 than at a 3 s one.
        if (Row.DecayRatio > PwCompareLimits::ModeDecayToleranceRatio
            || Row.DecayRatio < 1.0 / PwCompareLimits::ModeDecayToleranceRatio)
        {
            Out.Deviations.Add(MakeDeviation(
                Row.DecayRatio < 1.0 ? PwCompareDiagnosis::ModeDecayTooShort
                                     : PwCompareDiagnosis::ModeDecayTooLong,
                FString::Printf(TEXT("modes[%d].decayMs"), Index), TEXT("ratio"),
                Row.ReferenceDecayMs, Row.CandidateDecayMs, Row.DecayRatio));
        }
    }

    for (const FPwCompareUnmatchedMode& Row : Out.MissingModes)
    {
        // Candidate and Delta stay UNSET: there is no candidate mode, and emitting 0 Hz for the
        // side that does not exist would read as a mode at DC (rpc-design.md §1).
        FPwCompareDeviation Deviation;
        Deviation.Diagnosis = PwCompareDiagnosis::MissingMode;
        Deviation.Quantity = FString::Printf(TEXT("referenceModes[%d].freqHz"), Row.Index);
        Deviation.Unit = TEXT("hz");
        Deviation.Reference = Row.FreqHz;
        Out.Deviations.Add(MoveTemp(Deviation));
    }
    for (const FPwCompareUnmatchedMode& Row : Out.ExtraModes)
    {
        FPwCompareDeviation Deviation;
        Deviation.Diagnosis = PwCompareDiagnosis::ExtraMode;
        Deviation.Quantity = FString::Printf(TEXT("candidateModes[%d].freqHz"), Row.Index);
        Deviation.Unit = TEXT("hz");
        Deviation.Candidate = Row.FreqHz;
        Out.Deviations.Add(MoveTemp(Deviation));
    }

    for (int32 Index = 0; Index < Out.ResidualBands.Num(); ++Index)
    {
        const FPwCompareResidualBand& Row = Out.ResidualBands[Index];
        if (FMath::Abs(Row.DeltaDb) <= PwCompareLimits::ResidualToleranceDb)
        {
            continue;
        }
        Out.Deviations.Add(MakeDeviation(
            Row.DeltaDb < 0.0 ? PwCompareDiagnosis::ResidualDeficit
                              : PwCompareDiagnosis::ResidualExcess,
            FString::Printf(TEXT("residual.bands[%d].levelDb"), Index), TEXT("db"),
            Row.ReferenceLevelDb, Row.CandidateLevelDb, Row.DeltaDb));
    }

    Out.bMeasured = true;
    Out.UnmeasuredReason.Reset();
    return true;
}

namespace PwAudioCompareInternal
{
    double RoundTo(double Value, int32 Decimals)
    {
        if (!FMath::IsFinite(Value))
        {
            // Nothing this file computes can reach here - every quotient is guarded at its source -
            // so this exists only so a future caller cannot put a NaN on the wire, where it would
            // be invalid JSON rather than a wrong number.
            return 0.0;
        }
        const double Scale = FMath::Pow(10.0, static_cast<double>(Decimals));
        return FMath::RoundToDouble(Value * Scale) / Scale;
    }

    TSharedPtr<FJsonObject> SerializeScalar(const FPwCompareScalar& In, int32 Decimals)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("reference"), RoundTo(In.Reference, Decimals));
        Obj->SetNumberField(TEXT("candidate"), RoundTo(In.Candidate, Decimals));
        Obj->SetNumberField(TEXT("delta"), RoundTo(In.Delta, Decimals));
        return Obj;
    }

    /** Adds a measured scalar under `Scalars`, or its reason under `Unmeasured`. Never both. */
    void EmitScalar(const TSharedPtr<FJsonObject>& Scalars, const TSharedPtr<FJsonObject>& Unmeasured,
                    const TCHAR* Name, const FPwCompareScalar& In, int32 Decimals)
    {
        if (In.bMeasured)
        {
            Scalars->SetObjectField(Name, SerializeScalar(In, Decimals));
        }
        else
        {
            Unmeasured->SetStringField(Name, In.UnmeasuredReason);
        }
    }
}

TSharedPtr<FJsonObject> SerializeCompareResult(const FPwCompareResult& In)
{
    using namespace PwAudioCompareInternal;

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetBoolField(TEXT("measured"), In.bMeasured);
    if (!In.bMeasured)
    {
        // Nothing else may be read off an unmeasured report, so nothing else is emitted - a wall
        // of plausible zeros beside `measured: false` is exactly what §1 forbids.
        Root->SetStringField(TEXT("unmeasuredReason"), In.UnmeasuredReason);
        return Root;
    }

    Root->SetBoolField(TEXT("match"), In.IsMatch());
    Root->SetNumberField(TEXT("sampleRate"), In.SampleRate);
    Root->SetNumberField(TEXT("referenceDurationMs"), RoundTo(In.ReferenceDurationMs, 2));
    Root->SetNumberField(TEXT("candidateDurationMs"), RoundTo(In.CandidateDurationMs, 2));

    TSharedPtr<FJsonObject> Scalars = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> Unmeasured = MakeShared<FJsonObject>();
    EmitScalar(Scalars, Unmeasured, TEXT("onsetMs"), In.OnsetMs, 2);
    EmitScalar(Scalars, Unmeasured, TEXT("attackMs"), In.AttackMs, 2);
    EmitScalar(Scalars, Unmeasured, TEXT("decayMs"), In.DecayMs, 2);
    EmitScalar(Scalars, Unmeasured, TEXT("tailMs"), In.TailMs, 2);
    EmitScalar(Scalars, Unmeasured, TEXT("loudnessLufs"), In.LoudnessLufs, 2);
    if (In.CentroidHz.bMeasured)
    {
        // Built here rather than through EmitScalar because the centroid carries one extra field:
        // the log-domain form of its delta, which is what the brightness diagnosis is thresholded
        // on. It is omitted when either centroid is not strictly positive, since there is then no
        // ratio to take (rpc-design.md §1).
        TSharedPtr<FJsonObject> Centroid = SerializeScalar(In.CentroidHz, 1);
        if (In.CentroidDeltaCents.IsSet())
        {
            Centroid->SetNumberField(TEXT("deltaCents"), RoundTo(In.CentroidDeltaCents.GetValue(), 1));
        }
        Scalars->SetObjectField(TEXT("centroidHz"), Centroid);
    }
    else
    {
        Unmeasured->SetStringField(TEXT("centroidHz"), In.CentroidHz.UnmeasuredReason);
    }
    Root->SetObjectField(TEXT("scalars"), Scalars);

    TSharedPtr<FJsonObject> Modes = MakeShared<FJsonObject>();
    {
        TArray<TSharedPtr<FJsonValue>> Matched;
        Matched.Reserve(In.ModePairs.Num());
        for (const FPwCompareModePair& Row : In.ModePairs)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetNumberField(TEXT("referenceIndex"), Row.ReferenceIndex);
            Obj->SetNumberField(TEXT("candidateIndex"), Row.CandidateIndex);
            Obj->SetNumberField(TEXT("referenceFreqHz"), RoundTo(Row.ReferenceFreqHz, 2));
            Obj->SetNumberField(TEXT("candidateFreqHz"), RoundTo(Row.CandidateFreqHz, 2));
            Obj->SetNumberField(TEXT("freqDeltaCents"), RoundTo(Row.FreqDeltaCents, 1));
            Obj->SetNumberField(TEXT("referenceGainDb"), RoundTo(Row.ReferenceGainDb, 2));
            Obj->SetNumberField(TEXT("candidateGainDb"), RoundTo(Row.CandidateGainDb, 2));
            Obj->SetNumberField(TEXT("gainDeltaDb"), RoundTo(Row.GainDeltaDb, 2));
            Obj->SetNumberField(TEXT("referenceDecayMs"), RoundTo(Row.ReferenceDecayMs, 1));
            Obj->SetNumberField(TEXT("candidateDecayMs"), RoundTo(Row.CandidateDecayMs, 1));
            Obj->SetNumberField(TEXT("decayRatio"), RoundTo(Row.DecayRatio, 3));
            Matched.Add(MakeShared<FJsonValueObject>(Obj));
        }
        Modes->SetArrayField(TEXT("matched"), Matched);
    }

    // `missing` and `extra` are emitted even when empty. "Nothing is missing" is a measurement,
    // and an omitted array is indistinguishable from a matcher that only looked one way.
    for (int32 Side = 0; Side < 2; ++Side)
    {
        const TArray<FPwCompareUnmatchedMode>& Source = (Side == 0) ? In.MissingModes : In.ExtraModes;
        TArray<TSharedPtr<FJsonValue>> Entries;
        Entries.Reserve(Source.Num());
        for (const FPwCompareUnmatchedMode& Row : Source)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetNumberField(TEXT("index"), Row.Index);
            Obj->SetNumberField(TEXT("freqHz"), RoundTo(Row.FreqHz, 2));
            Obj->SetNumberField(TEXT("gainDb"), RoundTo(Row.GainDb, 2));
            Obj->SetNumberField(TEXT("decayMs"), RoundTo(Row.DecayMs, 1));
            Obj->SetStringField(TEXT("diagnosis"), Row.Diagnosis);
            Entries.Add(MakeShared<FJsonValueObject>(Obj));
        }
        Modes->SetArrayField((Side == 0) ? TEXT("missing") : TEXT("extra"), Entries);
    }
    Modes->SetNumberField(TEXT("referenceUnfittable"), In.ReferenceUnfittableModes);
    Modes->SetNumberField(TEXT("candidateUnfittable"), In.CandidateUnfittableModes);
    Root->SetObjectField(TEXT("modes"), Modes);

    if (In.bResidualMeasured)
    {
        TArray<TSharedPtr<FJsonValue>> Bands;
        Bands.Reserve(In.ResidualBands.Num());
        for (const FPwCompareResidualBand& Row : In.ResidualBands)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetNumberField(TEXT("lowHz"), RoundTo(Row.LowHz, 1));
            Obj->SetNumberField(TEXT("highHz"), RoundTo(Row.HighHz, 1));
            Obj->SetNumberField(TEXT("referenceLevelDb"), RoundTo(Row.ReferenceLevelDb, 2));
            Obj->SetNumberField(TEXT("candidateLevelDb"), RoundTo(Row.CandidateLevelDb, 2));
            Obj->SetNumberField(TEXT("deltaDb"), RoundTo(Row.DeltaDb, 2));
            Bands.Add(MakeShared<FJsonValueObject>(Obj));
        }
        TSharedPtr<FJsonObject> Residual = MakeShared<FJsonObject>();
        Residual->SetArrayField(TEXT("bands"), Bands);
        Root->SetObjectField(TEXT("residual"), Residual);
    }
    else
    {
        Unmeasured->SetStringField(TEXT("residual"), In.ResidualUnmeasuredReason);
    }

    {
        TArray<TSharedPtr<FJsonValue>> Deviations;
        Deviations.Reserve(In.Deviations.Num());
        for (const FPwCompareDeviation& Row : In.Deviations)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("diagnosis"), Row.Diagnosis);
            Obj->SetStringField(TEXT("quantity"), Row.Quantity);
            Obj->SetStringField(TEXT("unit"), Row.Unit);
            if (Row.Reference.IsSet())
            {
                Obj->SetNumberField(TEXT("reference"), RoundTo(Row.Reference.GetValue(), 2));
            }
            if (Row.Candidate.IsSet())
            {
                Obj->SetNumberField(TEXT("candidate"), RoundTo(Row.Candidate.GetValue(), 2));
            }
            if (Row.Delta.IsSet())
            {
                Obj->SetNumberField(TEXT("delta"), RoundTo(Row.Delta.GetValue(), 2));
            }
            Deviations.Add(MakeShared<FJsonValueObject>(Obj));
        }
        Root->SetArrayField(TEXT("deviations"), Deviations);
    }

    if (Unmeasured->Values.Num() > 0)
    {
        Root->SetObjectField(TEXT("unmeasured"), Unmeasured);
    }
    return Root;
}

// =================================================================================================
// Analysis by resynthesis.
// =================================================================================================
namespace PwAudioCompareInternal
{
    void SetNumberParam(FPwSynthParams& Params, const TCHAR* Key, double Value)
    {
        FPwSynthParamValue Entry;
        Entry.Type = EPwSynthParamType::Number;
        Entry.Number = Value;
        Params.Values.Add(FName(Key), MoveTemp(Entry));
    }

    void SetEnumParam(FPwSynthParams& Params, const TCHAR* Key, const TCHAR* Value)
    {
        FPwSynthParamValue Entry;
        Entry.Type = EPwSynthParamType::Enum;
        Entry.String = Value;
        Params.Values.Add(FName(Key), MoveTemp(Entry));
    }

    void SetNumberArrayParam(FPwSynthParams& Params, const TCHAR* Key, TArray<double>&& Value)
    {
        FPwSynthParamValue Entry;
        Entry.Type = EPwSynthParamType::NumberArray;
        Entry.Numbers = MoveTemp(Value);
        Params.Values.Add(FName(Key), MoveTemp(Entry));
    }

    /** The modal schema's own bounds, restated so a drop decision can name the limit it hit. */
    constexpr double ModalMinFreqHz = 0.01;
    constexpr double ModalMaxFreqHz = 20000.0;
    constexpr double ModalMinDecayMs = 1.0;
    constexpr double ModalMaxDecayMs = 20000.0;
    constexpr int32 ModalMaxModes = 32;

    /** The noise schema's own band-limit bounds. */
    constexpr double NoiseMinCutHz = 20.0;
    constexpr double NoiseMaxCutHz = 20000.0;

    /** The exciter schema's own length bounds. */
    constexpr double ExciterMinMs = 0.1;
    constexpr double ExciterMaxMs = 100.0;

    /**
     * The five colours the noise generator speaks, as the POWER-SPECTRAL-DENSITY slope each one
     * produces, dB per octave. A measured band slope is corrected by BandwidthTiltDbPerOctave
     * before it is matched against this table - see the file header.
     */
    struct FNoiseColorSpec
    {
        const TCHAR* Name;
        double PsdSlopeDbPerOctave;
    };

    const FNoiseColorSpec& NearestNoiseColor(double PsdSlopeDbPerOctave)
    {
        static const FNoiseColorSpec Colors[] = {
            { TEXT("brown"),  -6.0 },
            { TEXT("pink"),   -3.0 },
            { TEXT("white"),   0.0 },
            { TEXT("blue"),   +3.0 },
            { TEXT("violet"), +6.0 }
        };

        int32 Best = 0;
        double BestDistance = TNumericLimits<double>::Max();
        for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(Colors)); ++Index)
        {
            const double Distance = FMath::Abs(PsdSlopeDbPerOctave - Colors[Index].PsdSlopeDbPerOctave);
            if (Distance < BestDistance)
            {
                BestDistance = Distance;
                Best = Index;
            }
        }
        return Colors[Best];
    }

    /** The exciter the transients call for, plus its length when it has one. */
    struct FExciterChoice
    {
        const TCHAR* Name = TEXT("impulse");
        bool bHasLength = false;
        double LengthMs = 0.0;
        bool bLengthClamped = false;
    };

    FExciterChoice ChooseExciter(const TArray<FPwTransient>& Transients)
    {
        FExciterChoice Choice;
        if (Transients.Num() == 0)
        {
            // No transient was characterised, so there is nothing to shape the drive from.
            // `impulse` is the flat, unshaped exciter and the schema's documented default - it is
            // the absence of a choice, not a guess at one.
            return Choice;
        }

        // The FIRST transient, not the loudest: it is the attack that started the sound, and the
        // exciter fires at the layer's start.
        const FPwTransient& First = Transients[0];
        const double SpanMs = FMath::Max(0.0, First.EndMs - First.StartMs);
        if (SpanMs <= PwToRecipeLimits::ImpulseMaxTransientMs)
        {
            return Choice;
        }

        const double SpectralSpanHz = FMath::Max(0.0, First.HighHz - First.LowHz);
        const bool bBroadband = First.CentroidHz > 0.0
            && SpectralSpanHz >= PwToRecipeLimits::BroadbandSpanRatio * First.CentroidHz;

        Choice.Name = bBroadband ? TEXT("noise") : TEXT("strike");
        Choice.bHasLength = true;
        Choice.LengthMs = FMath::Clamp(SpanMs, ExciterMinMs, ExciterMaxMs);
        Choice.bLengthClamped = (Choice.LengthMs != SpanMs);
        return Choice;
    }

    /**
     * Sample the band-summed residual level on the union of the bands' own breakpoint times, then
     * reduce it to at most MaxEnvelopePoints by uniform resampling of the INDEX axis.
     *
     * The union rather than a uniform time grid because the breakpoints are already where the
     * signal moves - PwAnalyzeResidual simplified each band to its own tolerance - so uniform
     * sampling of the union is automatically denser in time where the contour is busy. First and
     * last points are always preserved.
     */
    void BuildResidualEnvelope(const TArray<FPwResidualBand>& Bands, double DurationMs,
                               TArray<FPwSynthEnvelopePoint>& Out)
    {
        Out.Reset();

        TArray<double> Times;
        for (const FPwResidualBand& Band : Bands)
        {
            for (const FPwDbPoint& Point : Band.EnvelopeDb)
            {
                if (FMath::IsFinite(Point.TimeMs))
                {
                    Times.Add(FMath::Clamp(Point.TimeMs, 0.0, DurationMs));
                }
            }
            if (Times.Num() >= MaxEnvelopeUnionPoints)
            {
                break;
            }
        }
        if (Times.Num() == 0)
        {
            return;
        }

        Times.Add(0.0);
        Times.Add(DurationMs);
        Times.Sort();

        TArray<double> Unique;
        Unique.Reserve(Times.Num());
        for (const double Time : Times)
        {
            if (Unique.Num() == 0 || Time - Unique.Last() > EnvelopeTimeEpsilonMs)
            {
                Unique.Add(Time);
            }
        }
        if (Unique.Num() < 2)
        {
            return;
        }

        TArray<double> Power;
        Power.Reserve(Unique.Num());
        double PeakPower = 0.0;
        for (const double Time : Unique)
        {
            double Sum = 0.0;
            for (const FPwResidualBand& Band : Bands)
            {
                Sum += ToPowerFromDb(EvalEnvelopeDb(Band.EnvelopeDb, Time));
            }
            Power.Add(Sum);
            PeakPower = FMath::Max(PeakPower, Sum);
        }
        if (!(PeakPower > SilencePower))
        {
            return;
        }

        TArray<FPwSynthEnvelopePoint> Dense;
        Dense.Reserve(Unique.Num());
        for (int32 Index = 0; Index < Unique.Num(); ++Index)
        {
            FPwSynthEnvelopePoint& Point = Dense.AddDefaulted_GetRef();
            Point.TimeMs = Unique[Index];
            // Amplitude, not power: the envelope multiplies a signal, so the square root of the
            // power ratio is the factor that reproduces the measured level.
            Point.Value = FMath::Clamp(FMath::Sqrt(Power[Index] / PeakPower), 0.0, 1.0);
            Point.Curve = EPwSynthCurve::Linear;
        }

        const int32 MaxPoints = PwSynthLimits::MaxEnvelopePoints;
        if (Dense.Num() <= MaxPoints)
        {
            Out = MoveTemp(Dense);
            return;
        }

        Out.Reserve(MaxPoints);
        for (int32 Step = 0; Step < MaxPoints; ++Step)
        {
            const int32 Index = FMath::Min(Dense.Num() - 1, FMath::FloorToInt32(
                static_cast<double>(Step) * static_cast<double>(Dense.Num() - 1)
                    / static_cast<double>(MaxPoints - 1)));
            if (Out.Num() == 0 || Out.Last().TimeMs < Dense[Index].TimeMs)
            {
                Out.Add(Dense[Index]);
            }
        }
    }
}

bool PwDecompositionToRecipe(const FPwDecomposition& In, FPwSynthRecipe& Out,
                             FString& OutErrorCode, FString& OutError)
{
    using namespace PwAudioCompareInternal;

    // Failure is the default. A default-constructed recipe has zero layers and zero duration,
    // which both ParseSynthRecipe and PwRenderRecipe reject outright - so a caller that ignored
    // the returned bool holds something structurally unusable rather than something plausible
    // (rpc-design.md §1, §2).
    Out = FPwSynthRecipe();
    OutErrorCode.Reset();
    OutError.Reset();

    if (!In.bMeasured)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("ToRecipe: the decomposition is unmeasured, so there is nothing ")
                TEXT("to map onto a recipe.%s%s"),
                In.UnmeasuredReason.IsEmpty() ? TEXT("") : TEXT(" "), *In.UnmeasuredReason));
    }
    if (In.SampleRate < PwSynthLimits::MinSampleRate || In.SampleRate > PwSynthLimits::MaxSampleRate)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("ToRecipe: the decomposition was measured at %d Hz; the recipe ")
                TEXT("schema accepts %d..%d Hz."), In.SampleRate,
                PwSynthLimits::MinSampleRate, PwSynthLimits::MaxSampleRate));
    }
    if (!FMath::IsFinite(In.DurationMs) || In.DurationMs < PwSynthLimits::MinDurationMs
        || In.DurationMs > PwSynthLimits::MaxDurationMs)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("ToRecipe: the decomposition spans %g ms; the recipe schema ")
                TEXT("accepts %g..%g ms."), In.DurationMs,
                PwSynthLimits::MinDurationMs, PwSynthLimits::MaxDurationMs));
    }

    TArray<FString> Notes;

    FPwSynthRecipe Work;
    Work.Version = PwSynthLimits::RecipeVersion;
    Work.Seed = PwSynthLimits::DefaultSeed;
    Work.SampleRate = In.SampleRate;
    Work.DurationMs = In.DurationMs;

    // ---- the modal layer ------------------------------------------------------------------------
    {
        const double Nyquist = 0.5 * static_cast<double>(In.SampleRate);
        const double MaxFreqHz = FMath::Min(ModalMaxFreqHz, Nyquist);

        TArray<double> Freqs;
        TArray<double> Gains;
        TArray<double> Decays;
        int32 Unfittable = 0;
        int32 DroppedOutOfRange = 0;
        int32 DroppedByCap = 0;

        // Modes is index-aligned with Partials, and Partials is LOUDEST FIRST, so taking the
        // first ModalMaxModes acceptable rows keeps the loudest ones - the cap is visible in the
        // ordering rather than applied to an arbitrary subset.
        for (int32 Index = 0; Index < In.Modes.Num(); ++Index)
        {
            const FPwModalFit& Fit = In.Modes[Index];
            if (!Fit.bMeasured || !(Fit.DecayMs > 0.0))
            {
                // A growing partial (negative DecayMs) or a sustained one (0) is not a mode the
                // bank can hold; the generator rejects modeDecaysMs <= 0 outright.
                ++Unfittable;
                continue;
            }
            if (Freqs.Num() >= ModalMaxModes)
            {
                ++DroppedByCap;
                continue;
            }

            // The fit's level refers to the track's first measured frame, which the analysis grid
            // places half an FFT window into the signal. modeGainsDb is the level at the LAYER's
            // start, so the fitted level is walked back along its own exponential. See the file
            // header for why leaving it out costs ~5 dB on a 250 ms mode.
            double GainDb = Fit.InitialGainDb;
            if (In.Partials.IsValidIndex(Index) && FMath::IsFinite(In.Partials[Index].StartMs)
                && In.Partials[Index].StartMs > 0.0)
            {
                GainDb += 60.0 * In.Partials[Index].StartMs / Fit.DecayMs;
            }

            const bool bAcceptable = FMath::IsFinite(Fit.FreqHz) && Fit.FreqHz >= ModalMinFreqHz
                && Fit.FreqHz < MaxFreqHz
                && Fit.DecayMs >= ModalMinDecayMs && Fit.DecayMs <= ModalMaxDecayMs
                && FMath::IsFinite(GainDb)
                && GainDb >= PwSynthLimits::MinGainDb && GainDb <= PwSynthLimits::MaxGainDb;
            if (!bAcceptable)
            {
                // Dropped, never clamped: a clamped frequency, decay or level is a number the
                // reference sound does not have, published under a name that says it does.
                ++DroppedOutOfRange;
                continue;
            }

            Freqs.Add(Fit.FreqHz);
            Gains.Add(GainDb);
            Decays.Add(Fit.DecayMs);
        }

        if (Freqs.Num() > 0)
        {
            const FExciterChoice Exciter = ChooseExciter(In.Transients);

            FPwSynthLayer Layer;
            Layer.StartMs = PwSynthLimits::DefaultLayerStartMs;
            Layer.GainDb = PwSynthLimits::DefaultLayerGainDb;
            Layer.Pan = PwSynthLimits::DefaultLayerPan;
            Layer.Generator.Kind = EPwSynthGeneratorKind::Modal;
            SetNumberArrayParam(Layer.Generator.Params, TEXT("modeFreqsHz"), MoveTemp(Freqs));
            SetNumberArrayParam(Layer.Generator.Params, TEXT("modeGainsDb"), MoveTemp(Gains));
            SetNumberArrayParam(Layer.Generator.Params, TEXT("modeDecaysMs"), MoveTemp(Decays));
            SetEnumParam(Layer.Generator.Params, TEXT("exciter"), Exciter.Name);
            if (Exciter.bHasLength)
            {
                SetNumberParam(Layer.Generator.Params, TEXT("exciterMs"), Exciter.LengthMs);
            }
            // No ampEnvelope, deliberately: the modes carry their own decays, and an envelope on
            // top of them would apply the decay a second time.
            Work.Layers.Add(MoveTemp(Layer));

            if (Exciter.bLengthClamped)
            {
                Notes.Add(FString::Printf(TEXT("the first transient is longer than the schema's ")
                    TEXT("%g ms exciter cap, so exciterMs was capped at %g ms"),
                    ExciterMaxMs, Exciter.LengthMs));
            }
            if (DroppedByCap > 0)
            {
                Notes.Add(FString::Printf(TEXT("%d fitted modes past the bank's cap of %d were ")
                    TEXT("dropped; the kept ones are the loudest, since the decomposition orders ")
                    TEXT("partials loudest-first"), DroppedByCap, ModalMaxModes));
            }
            if (DroppedOutOfRange > 0)
            {
                Notes.Add(FString::Printf(TEXT("%d fitted modes fell outside the schema's ")
                    TEXT("frequency, decay or level range and were dropped rather than clamped"),
                    DroppedOutOfRange));
            }
        }
        else if (Unfittable > 0 || In.Modes.Num() > 0)
        {
            Notes.Add(FString::Printf(TEXT("no modal layer: of %d fitted rows, %d carry no ")
                TEXT("measurable decay and %d fell outside the schema's range"),
                In.Modes.Num(), Unfittable, DroppedOutOfRange));
        }
    }

    // ---- the noise layer ------------------------------------------------------------------------
    if (In.Residual.Num() > 0)
    {
        TArray<double> BandLevelDb;
        BandLevelDb.Reserve(In.Residual.Num());
        double PeakBandDb = -TNumericLimits<double>::Max();
        bool bAllMeasured = true;
        for (const FPwResidualBand& Band : In.Residual)
        {
            double LevelDb = 0.0;
            if (!BandMeanLevelDb(Band.EnvelopeDb, LevelDb))
            {
                bAllMeasured = false;
                break;
            }
            BandLevelDb.Add(LevelDb);
            PeakBandDb = FMath::Max(PeakBandDb, LevelDb);
        }

        if (!bAllMeasured)
        {
            Notes.Add(TEXT("no noise layer: at least one residual band carries an empty level ")
                TEXT("envelope, so its contour could not be measured"));
        }
        else if (PeakBandDb < PwToRecipeLimits::MinResidualPeakBandDb)
        {
            Notes.Add(FString::Printf(TEXT("no noise layer: the loudest residual band sits at ")
                TEXT("%.1f dBFS, below the %.1f dBFS confidence floor - a layer built from it ")
                TEXT("would mostly reproduce the analyzer's own leakage"),
                PeakBandDb, PwToRecipeLimits::MinResidualPeakBandDb));
        }
        else
        {
            // The band limits are the outer edges of the bands that carry the residual, not the
            // outer edges of the analysis - a band 40 dB down is not part of this noise.
            int32 FirstSupport = INDEX_NONE;
            int32 LastSupport = INDEX_NONE;
            for (int32 Index = 0; Index < BandLevelDb.Num(); ++Index)
            {
                if (BandLevelDb[Index] >= PeakBandDb - PwToRecipeLimits::NoiseSupportRangeDb)
                {
                    FirstSupport = (FirstSupport == INDEX_NONE) ? Index : FirstSupport;
                    LastSupport = Index;
                }
            }

            const double LowCutHz = FMath::Clamp(In.Residual[FirstSupport].LowHz,
                NoiseMinCutHz, NoiseMaxCutHz);
            const double HighCutHz = FMath::Clamp(In.Residual[LastSupport].HighHz,
                NoiseMinCutHz, NoiseMaxCutHz);

            if (!(HighCutHz > LowCutHz))
            {
                Notes.Add(FString::Printf(TEXT("no noise layer: the residual's support collapses ")
                    TEXT("to %.1f Hz once clamped into the schema's %g-%g Hz range, which is not ")
                    TEXT("a band"), LowCutHz, NoiseMinCutHz, NoiseMaxCutHz));
            }
            else
            {
                // Least squares of band level against log2(band centre). The measured slope
                // includes the +3 dB/octave a log-spaced band grid adds to a flat spectrum, so it
                // is corrected before the colour is chosen - see the file header.
                double SumX = 0.0;
                double SumY = 0.0;
                double SumXX = 0.0;
                double SumXY = 0.0;
                int32 Count = 0;
                for (int32 Index = FirstSupport; Index <= LastSupport; ++Index)
                {
                    const double CentreHz = FMath::Sqrt(
                        In.Residual[Index].LowHz * In.Residual[Index].HighHz);
                    if (!(CentreHz > 0.0))
                    {
                        continue;
                    }
                    const double X = FMath::Loge(CentreHz) / Ln2;
                    const double Y = BandLevelDb[Index];
                    SumX += X;
                    SumY += Y;
                    SumXX += X * X;
                    SumXY += X * Y;
                    ++Count;
                }

                double MeasuredSlope = PwToRecipeLimits::BandwidthTiltDbPerOctave;   // -> white
                if (Count >= 2)
                {
                    const double Denominator = static_cast<double>(Count) * SumXX - SumX * SumX;
                    if (FMath::Abs(Denominator) > UE_DOUBLE_KINDA_SMALL_NUMBER)
                    {
                        MeasuredSlope = (static_cast<double>(Count) * SumXY - SumX * SumY) / Denominator;
                    }
                }
                else
                {
                    // A single support band has no slope to measure. White is the neutral tilt
                    // through a band limit, and it is named as an assumption in the note.
                    Notes.Add(TEXT("the residual occupies one band, which has no measurable ")
                        TEXT("spectral tilt, so the noise layer was authored as white inside its ")
                        TEXT("band limits"));
                }

                const FNoiseColorSpec& Color = NearestNoiseColor(
                    MeasuredSlope - PwToRecipeLimits::BandwidthTiltDbPerOctave);

                // The residual's whole broadband level, as the sum of the bands' mean powers.
                double TotalPower = 0.0;
                for (const double LevelDb : BandLevelDb)
                {
                    TotalPower += ToPowerFromDb(LevelDb);
                }
                const double TotalDb = ToDbFromPower(TotalPower);
                const double GainDb = FMath::Clamp(TotalDb,
                    PwSynthLimits::MinGainDb, PwSynthLimits::MaxGainDb);

                FPwSynthLayer Layer;
                Layer.StartMs = PwSynthLimits::DefaultLayerStartMs;
                Layer.GainDb = GainDb;
                Layer.Pan = PwSynthLimits::DefaultLayerPan;
                Layer.Generator.Kind = EPwSynthGeneratorKind::Noise;
                SetEnumParam(Layer.Generator.Params, TEXT("color"), Color.Name);
                SetNumberParam(Layer.Generator.Params, TEXT("lowCutHz"), LowCutHz);
                SetNumberParam(Layer.Generator.Params, TEXT("highCutHz"), HighCutHz);
                BuildResidualEnvelope(In.Residual, In.DurationMs, Layer.AmpEnvelope);
                Work.Layers.Add(MoveTemp(Layer));

                Notes.Add(FString::Printf(TEXT("the noise layer's gainDb of %.1f dB is an ")
                    TEXT("ESTIMATE, not a measurement: its shape (%s, %.0f-%.0f Hz) is measured, ")
                    TEXT("but PwGenNoise publishes no dBFS calibration, so the absolute level ")
                    TEXT("assumes its output sits near 0 dBFS RMS. Close it with one compare pass"),
                    GainDb, Color.Name, LowCutHz, HighCutHz));
            }
        }
    }

    if (Work.Layers.Num() == 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("ToRecipe: the decomposition yielded no layer the synthesizer ")
                TEXT("can hold - %d fitted rows, none of them a usable mode, and %d residual ")
                TEXT("bands. A recipe needs at least one layer, and an empty one is not a draft. ")
                TEXT("%s"), In.Modes.Num(), In.Residual.Num(), *FString::Join(Notes, TEXT("; "))));
    }

    // The stereo image is measured but NOT emitted, and the reason is returned rather than
    // silently swallowed. `width` is a master-only mid/side effect and mid/side has nothing to
    // work on when the bus is mono: every generator here is mono and every layer above sits at
    // pan 0, so a width effect would be an exact no-op - the silent nothing rpc-design.md §3
    // exists to prevent. Naming what the caller must do instead is the honest half of this.
    if (In.Stereo.bMeasured && In.Stereo.InitialCorrelation < PwToRecipeLimits::MonoCorrelationFloor)
    {
        Notes.Add(FString::Printf(TEXT("the reference has a stereo image (L/R correlation %.2f ")
            TEXT("at the start, %.2f in the tail) that this recipe does not carry: generators are ")
            TEXT("mono and both layers sit at pan 0, so a master `width` effect would be an exact ")
            TEXT("no-op and was not emitted. Pan the layers apart, or add a second noise layer ")
            TEXT("with the same params at the opposite pan - layers draw independent random ")
            TEXT("substreams, so two of them decorrelate"),
            In.Stereo.InitialCorrelation, In.Stereo.TailCorrelation));
    }

    // ---- the check the write path cannot fake (rpc-design.md §4) ----------------------------------
    // Run the emitted recipe through the SCHEMA'S OWN parser, not through a readback of the values
    // just set, and hand back the PARSED form: the caller receives the canonical, fully-
    // materialized recipe the schema accepted, and a recipe that does not validate is refused
    // instead of shipped to fail on first use.
    const TSharedPtr<FJsonObject> Json = SerializeSynthRecipe(Work);
    FPwSynthRecipeError ParseError;
    FPwSynthRecipe Validated;
    if (!ParseSynthRecipe(Json, Validated, ParseError))
    {
        return Fail(OutErrorCode, OutError,
            ParseError.Code.IsEmpty() ? FString(ErrorCodes::ERR_INVALID_RECIPE) : ParseError.Code,
            FString::Printf(TEXT("ToRecipe: the recipe built from this decomposition does not ")
                TEXT("validate, so it is refused rather than returned - a draft that errors on ")
                TEXT("first use costs more than no draft. %s"), *ParseError.ToString()));
    }

    Out = MoveTemp(Validated);
    if (Notes.Num() > 0)
    {
        // A note, not a failure: OutErrorCode stays empty, following PwTrackPartials' convention.
        OutError = FString::Join(Notes, TEXT("; "));
    }
    return true;
}
