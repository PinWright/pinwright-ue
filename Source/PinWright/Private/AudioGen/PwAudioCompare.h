// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"

#include "AudioGen/PwAudioAnalysis.h"    // PwAudioAnalysisLimits - the measurement grid the thresholds alias
#include "AudioGen/PwAudioDecompose.h"   // FPwDecomposition + FPwAudioBuffer, the two inputs
#include "AudioGen/PwSynthRecipe.h"      // FPwSynthRecipe - the shape to_recipe must produce

class FJsonObject;

// PwAudioCompare.h - the two verbs that close the iteration loop.
//
// Everything else in AudioGen produces INFORMATION. These two produce ACTION:
//
//   PwCompareAudio          reference + candidate -> what to change, and in which direction
//   PwDecompositionToRecipe a decomposed reference -> a draft FPwSynthRecipe to start from
//
// PwDecompositionToRecipe is the subsystem's cold start. No preset library ships, so without it
// the calling model has to invent a recipe from nothing; with it, a user's own audio is their
// template. PwCompareAudio is then the loop: render, compare, patch, render again.
//
// -------------------------------------------------------------------------------------------
// EVERY DEVIATION IS SIGNED OR PAIRED (rpc-design.md §6) - THIS IS THE POINT OF THE FILE
// -------------------------------------------------------------------------------------------
// "The attack is 12 ms off" is not a report an agent can act on: it does not say whether to
// lengthen or shorten. So no field here is an unsigned magnitude, and no deviation is scored by
// distance alone:
//
//   * Every scalar is published as FPwCompareScalar - the REFERENCE value, the CANDIDATE value,
//     and the SIGNED difference between them. The reference value travels beside the delta on
//     purpose: with both, the model computes a target ("make the attack 8 ms"); with the delta
//     alone it can only guess a step size and iterate blind.
//   * Frequencies are compared in CENTS, not hertz. The log domain is symmetric - 100 cents up
//     and 100 cents down are the same musical distance at 80 Hz and at 8 kHz, where +50 Hz is
//     an octave in one register and inaudible in the other.
//   * Decays are compared as RATIOS, for the same reason: "half as long" is the actionable
//     statement, "-250 ms" is not.
//   * Levels are compared in dB / LU, where the unit is already logarithmic and already signed.
//   * Each deviation carries a `Diagnosis` drawn from a CLOSED vocabulary
//     (PwCompareDiagnosis below). `attack_too_sharp` and `attack_too_soft` are different tokens
//     rather than one token plus a sign the reader has to interpret, because an open prose
//     string invites the model to parse sentences.
//
// -------------------------------------------------------------------------------------------
// MODE MATCHING IS THE HARD PART, AND BOTH SIDES OF IT ARE REPORTED
// -------------------------------------------------------------------------------------------
// Two decompositions of two different sounds have no shared mode index, so the report has to
// establish the correspondence itself. Reference and candidate modes are paired by frequency
// proximity in cents (see PwCompareLimits::ModeMatchToleranceCents and the algorithm note on
// PwCompareAudio), and then - the half that a naive matcher silently drops -
//
//   a reference mode with no candidate counterpart  -> `missing_mode` ("you are missing a mode")
//   a candidate mode with no reference counterpart  -> `extra_mode`   ("you have one too many")
//
// Those are opposite instructions. A matcher that reported only the first would let every
// spurious candidate mode through unmentioned; one that reported only the second would never
// tell the caller to add anything. Both lists are always populated, and both are always
// serialized, even when empty (an empty `missing` list is the measurement "nothing is missing").
//
// UNFITTABLE ROWS ARE NOT MODES. FPwModalFit carries the sibling invariant bMeasured == true
// <=> DecayMs > 0: a GROWING partial reports a negative DecayMs and a SUSTAINED one reports 0.
// Neither is a mode the modal bank can hold, so neither enters the matcher and neither is
// reported as missing or extra - it would be an instruction to add a mode the generator would
// refuse. They are COUNTED instead (ReferenceUnfittableModes / CandidateUnfittableModes), so
// their exclusion is visible rather than silent (rpc-design.md §1).
//
// -------------------------------------------------------------------------------------------
// ENGINE VERSION
// -------------------------------------------------------------------------------------------
// Nothing in this file or its implementation reaches past CoreMinimal / FMath / TArray /
// TOptional / FJsonObject, so it owes no row in docs/engine-version-support.md. The LUFS figure
// PwCompareAudio consumes is measured by PwAnalyzeBuffer, which owns whatever version note that
// back-end needs; this file only subtracts two numbers it was handed.

// ---------------------------------------------------------------------------------------------
// The closed diagnosis vocabulary.
//
// Closed rather than free text so the calling model can BRANCH on the token instead of parsing
// prose, and paired rather than signed so the two directions of one fault are two different
// strings. PwIsKnownCompareDiagnosis is the structural guarantee (rpc-design.md §2): the
// serializer and the tests check membership against the same list the emitters draw from, so a
// hand-typed token cannot reach the wire.
// ---------------------------------------------------------------------------------------------
namespace PwCompareDiagnosis
{
    /** The candidate's first sounding block arrives before the reference's. */
    inline constexpr TCHAR OnsetTooEarly[]      = TEXT("onset_too_early");
    inline constexpr TCHAR OnsetTooLate[]       = TEXT("onset_too_late");

    /** The candidate reaches its peak in less time than the reference - a harder hit. */
    inline constexpr TCHAR AttackTooSharp[]     = TEXT("attack_too_sharp");
    inline constexpr TCHAR AttackTooSoft[]      = TEXT("attack_too_soft");

    /** Whole-sound decay: peak to the analysis floor. */
    inline constexpr TCHAR DecayTooShort[]      = TEXT("decay_too_short");
    inline constexpr TCHAR DecayTooLong[]       = TEXT("decay_too_long");

    /** The quiet ring-out below -20 dB, a subset of the decay (see FPwAudioEnvelope::TailMs). */
    inline constexpr TCHAR TailTooShort[]       = TEXT("tail_too_short");
    inline constexpr TCHAR TailTooLong[]        = TEXT("tail_too_long");

    /** Integrated loudness. */
    inline constexpr TCHAR TooLoud[]            = TEXT("too_loud");
    inline constexpr TCHAR TooQuiet[]           = TEXT("too_quiet");

    /** Spectral centroid - the usual proxy for perceived brightness. */
    inline constexpr TCHAR CandidateTooBright[] = TEXT("candidate_too_bright");
    inline constexpr TCHAR CandidateTooDark[]   = TEXT("candidate_too_dark");

    /** A reference mode with no candidate counterpart / a candidate mode with no reference one. */
    inline constexpr TCHAR MissingMode[]        = TEXT("missing_mode");
    inline constexpr TCHAR ExtraMode[]          = TEXT("extra_mode");

    /** A matched pair whose candidate sits above / below the reference frequency. */
    inline constexpr TCHAR ModeTooSharp[]       = TEXT("mode_too_sharp");
    inline constexpr TCHAR ModeTooFlat[]        = TEXT("mode_too_flat");

    inline constexpr TCHAR ModeTooLoud[]        = TEXT("mode_too_loud");
    inline constexpr TCHAR ModeTooQuiet[]       = TEXT("mode_too_quiet");

    inline constexpr TCHAR ModeDecayTooShort[]  = TEXT("mode_decay_too_short");
    inline constexpr TCHAR ModeDecayTooLong[]   = TEXT("mode_decay_too_long");

    /** One residual band carries more / less noise than the reference's matching band. */
    inline constexpr TCHAR ResidualExcess[]     = TEXT("residual_excess");
    inline constexpr TCHAR ResidualDeficit[]    = TEXT("residual_deficit");
}

/** Every token PwCompareAudio can emit, in a stable order. The vocabulary IS this list. */
const TArray<FString>& PwCompareDiagnosisVocabulary();

/** Membership test against PwCompareDiagnosisVocabulary. */
bool PwIsKnownCompareDiagnosis(const FString& Diagnosis);

// ---------------------------------------------------------------------------------------------
// Thresholds. Published rather than spelled inline because a report is only comparable across
// runs if the reader can see the floors that decided which deviations were worth naming.
// ---------------------------------------------------------------------------------------------
namespace PwCompareLimits
{
    /**
     * Absolute floor on any timing deviation, ms.
     *
     * ALIASED, not chosen again: it is the RMS envelope block every timing metric in
     * FPwAudioEnvelope is measured on, because a difference smaller than one block is the
     * quantization of the measurement rather than a property of the sound, and a second number
     * here would let this threshold drift away from the grid it is thresholding (rpc-design.md
     * §2 - one writer, one threshold). Combined with TimeToleranceRatio as
     * max(floor, ratio * reference).
     */
    inline constexpr double TimeToleranceMs = PwAudioAnalysisLimits::EnvelopeBlockMs;

    /** Relative part of the timing threshold: a 25% timing difference is the audible floor. */
    inline constexpr double TimeToleranceRatio = 0.25;

    /** Loudness threshold, LU. 1 LU is the delivery tolerance broadcast specs are written to. */
    inline constexpr double LoudnessToleranceLu = 1.0;

    /**
     * Brightness threshold on the centroid, cents. 100 cents is a semitone, about 6% in hertz -
     * below that the centroid moves less than the frame-to-frame scatter of two renders of the
     * same recipe, and a listener does not localise the difference.
     */
    inline constexpr double CentroidToleranceCents = 100.0;

    /**
     * Widest frequency gap at which a reference and a candidate mode are still called the SAME
     * mode, cents. 100 cents (one semitone) is deliberately wider than the tolerance at which a
     * mistuning is reported: an audibly mistuned mode must PAIR - so the report can say "you are
     * 80 cents sharp", which is actionable - rather than come back as a missing_mode plus an
     * extra_mode, which is the same fact stated in a way the caller cannot act on.
     *
     * It cannot reach a neighbouring harmonic for the first 17 harmonics: the k-th and (k+1)-th
     * are 1200*log2((k+1)/k) cents apart, which stays above 100 until k = 17. Past that the
     * nearest-first ordering of the matcher is what keeps the assignment sane.
     */
    inline constexpr double ModeMatchToleranceCents = 100.0;

    /**
     * Mistuning worth reporting on a matched pair, cents. 10 cents is around the pitch JND for a
     * steady tone and roughly five times the parabolic peak estimator's own bias.
     */
    inline constexpr double ModeFreqToleranceCents = 10.0;

    /** Per-mode level threshold, dB. 3 dB is a factor of two in power. */
    inline constexpr double ModeGainToleranceDb = 3.0;

    /**
     * Per-mode decay threshold, as a ratio either side of 1. A T60 within 25% of the reference's
     * is not separable by ear from it on an impact sound.
     */
    inline constexpr double ModeDecayToleranceRatio = 1.25;

    /** Residual band threshold, dB. Same factor-of-two-in-power argument as ModeGainToleranceDb. */
    inline constexpr double ResidualToleranceDb = 3.0;

    /**
     * Relative agreement two residual band edges must reach before the two band sets are treated
     * as the same axis, as a fraction of the edge frequency. The decomposer picks its own FFT
     * size, so two buffers of different lengths can come back on different band grids; comparing
     * band i to band i across two different axes would publish a level difference that is really
     * a frequency difference.
     */
    inline constexpr double ResidualEdgeAgreement = 0.01;
}

// ---------------------------------------------------------------------------------------------
// One paired scalar. Reference, candidate, and the SIGNED difference between them.
//
// bMeasured false means the two sides could not be compared at all - one of them did not
// measure the quantity - and the three numbers below it are then meaningless, not zero. The
// serializer omits an unmeasured scalar and publishes its reason under `unmeasured` instead.
// ---------------------------------------------------------------------------------------------
struct FPwCompareScalar
{
    bool bMeasured = false;

    /** Why the comparison is absent, naming the side that could not measure it. */
    FString UnmeasuredReason;

    double Reference = 0.0;
    double Candidate = 0.0;

    /** Candidate - Reference, in the quantity's own unit. Always signed (rpc-design.md §6). */
    double Delta = 0.0;
};

/**
 * One matched (reference mode, candidate mode) pair.
 *
 * Both sides' raw values travel with every deviation, so the caller can compute a target rather
 * than apply a step: knowing the candidate is 40 cents sharp is less useful than knowing the
 * reference mode is at 970.2 Hz.
 */
struct FPwCompareModePair
{
    /** Indices into the two decompositions' Modes arrays, so a caller can find the row again. */
    int32 ReferenceIndex = INDEX_NONE;
    int32 CandidateIndex = INDEX_NONE;

    double ReferenceFreqHz = 0.0;
    double CandidateFreqHz = 0.0;

    /** 1200 * log2(candidate / reference). Positive means the candidate mode is sharp. */
    double FreqDeltaCents = 0.0;

    double ReferenceGainDb = 0.0;
    double CandidateGainDb = 0.0;

    /** Candidate - reference, dB. Positive means the candidate mode is louder. */
    double GainDeltaDb = 0.0;

    double ReferenceDecayMs = 0.0;
    double CandidateDecayMs = 0.0;

    /** candidate / reference. Above 1 means the candidate mode rings longer. Paired, not signed. */
    double DecayRatio = 1.0;
};

/** A mode present on exactly one side. Diagnosis is missing_mode or extra_mode - never both. */
struct FPwCompareUnmatchedMode
{
    /** Index into the side it came from. */
    int32 Index = INDEX_NONE;

    double FreqHz = 0.0;
    double GainDb = 0.0;
    double DecayMs = 0.0;

    /** PwCompareDiagnosis::MissingMode (reference side) or ::ExtraMode (candidate side). */
    FString Diagnosis;
};

/** One residual band compared against the reference band covering the same frequencies. */
struct FPwCompareResidualBand
{
    double LowHz = 0.0;
    double HighHz = 0.0;

    /** Time-integrated mean level of the band's envelope, dBFS. See PwCompareAudio's note. */
    double ReferenceLevelDb = 0.0;
    double CandidateLevelDb = 0.0;

    /** Candidate - reference, dB. Positive is excess noise in this band. */
    double DeltaDb = 0.0;
};

/**
 * One named, actionable difference. This array is what the calling model reads first.
 *
 * Reference / Candidate / Delta are TOptional because an unmatched mode genuinely has no
 * counterpart: emitting 0.0 for the side that does not exist would read as "the candidate has a
 * mode at 0 Hz" (rpc-design.md §1).
 *
 * `Unit` names the unit of DELTA, which is not always the unit of the pair: a frequency
 * deviation is reported in cents beside a pair in hertz, and a decay deviation is reported as a
 * ratio beside a pair in milliseconds. That asymmetry is deliberate - see the log-domain note at
 * the top of this file.
 */
struct FPwCompareDeviation
{
    /** A token from PwCompareDiagnosis. Closed vocabulary; never free prose. */
    FString Diagnosis;

    /** Dotted path of what was measured, e.g. "envelope.attackMs" or "modes[3].decayMs". */
    FString Quantity;

    /** "ms" | "db" | "lu" | "hz" | "cents" | "ratio". */
    FString Unit;

    TOptional<double> Reference;
    TOptional<double> Candidate;
    TOptional<double> Delta;
};

// ---------------------------------------------------------------------------------------------
// The whole difference report.
//
// Default-constructed = nothing compared, which is exactly the state PwCompareAudio leaves
// behind on every failure path.
// ---------------------------------------------------------------------------------------------
struct FPwCompareResult
{
    /** True only after both sides were analysed, decomposed and diffed. */
    bool bMeasured = false;

    /** Why the comparison is absent. Meaningful only while bMeasured is false. */
    FString UnmeasuredReason;

    /** The rate both buffers were measured at - they must agree, so there is one field. */
    int32 SampleRate = 0;

    double ReferenceDurationMs = 0.0;
    double CandidateDurationMs = 0.0;

    /** Start of the first sounding block, ms. Positive delta means the candidate starts late. */
    FPwCompareScalar OnsetMs;

    /** Onset to the loudest block, ms. Negative delta means the candidate hits harder. */
    FPwCompareScalar AttackMs;

    /** Loudest block to the -60 dB floor, ms - the whole decay. */
    FPwCompareScalar DecayMs;

    /** The part of the decay spent below -20 dB - the ring-out, a SUBSET of DecayMs. */
    FPwCompareScalar TailMs;

    /** Gated BS.1770 integrated loudness, LUFS. Delta is in LU. */
    FPwCompareScalar LoudnessLufs;

    /** Magnitude-weighted mean frequency, Hz. Delta is in Hz; see CentroidDeltaCents. */
    FPwCompareScalar CentroidHz;

    /**
     * The log-domain form of CentroidHz.Delta: 1200 * log2(candidate / reference). This is what
     * the brightness diagnosis is thresholded on, because +200 Hz is a different perceptual
     * distance at 300 Hz than at 6 kHz. Unset when either centroid is not strictly positive.
     */
    TOptional<double> CentroidDeltaCents;

    /** Modes paired between the two sides, in ascending reference-frequency order. */
    TArray<FPwCompareModePair> ModePairs;

    /** Reference modes with no candidate counterpart. Every entry is a missing_mode. */
    TArray<FPwCompareUnmatchedMode> MissingModes;

    /** Candidate modes with no reference counterpart. Every entry is an extra_mode. */
    TArray<FPwCompareUnmatchedMode> ExtraModes;

    /**
     * Fitted rows excluded from matching because they are not modes: a growing partial (negative
     * DecayMs) or a sustained one (0). Counted rather than dropped silently - see the header.
     */
    int32 ReferenceUnfittableModes = 0;
    int32 CandidateUnfittableModes = 0;

    /** True only when the two band grids agreed and the bands below could be compared. */
    bool bResidualMeasured = false;

    /** Why the residual comparison is absent. Meaningful only while bResidualMeasured is false. */
    FString ResidualUnmeasuredReason;

    /** Band-by-band excess / deficit, ascending LowHz. Empty whenever bResidualMeasured is false. */
    TArray<FPwCompareResidualBand> ResidualBands;

    /**
     * Every deviation that cleared its threshold, in a fixed order: onset, attack, decay, tail,
     * loudness, centroid, matched modes (reference order, frequency then gain then decay),
     * missing modes, extra modes, residual bands. Deterministic, so two runs diff cleanly.
     */
    TArray<FPwCompareDeviation> Deviations;

    /**
     * Derived, not stored: the two sounds agree everywhere the report looked. Derived so the
     * verdict cannot disagree with the list it summarises (rpc-design.md §2).
     */
    bool IsMatch() const { return bMeasured && Deviations.Num() == 0; }
};

/**
 * Measure what a candidate render differs from a reference recording BY, and in which direction.
 *
 * Both buffers are run through PwAnalyzeBuffer and PwDecomposeBuffer and the two reports are
 * diffed. Every difference is published as a paired, signed quantity plus a closed-vocabulary
 * diagnosis; see the §6 discussion at the top of this file for why none of them is a magnitude.
 *
 * MODE MATCHING. Only rows with FPwModalFit::bMeasured enter the matcher. Every (reference,
 * candidate) pair whose separation is within PwCompareLimits::ModeMatchToleranceCents is
 * considered in ascending |cents| order and taken when neither side is already claimed - the
 * same globally-greedy nearest-first assignment PwTrackPartials uses to continue tracks across
 * frames, so the two stages agree about what "the same partial" means. Leftovers on the
 * reference side are missing_mode, leftovers on the candidate side are extra_mode, and BOTH
 * lists are always produced: a one-sided matcher answers half the question.
 *
 * RESIDUAL BANDS. A band's level is the TIME-INTEGRATED MEAN POWER of its dB envelope, not its
 * peak. Mean rather than peak because it is invariant to how long the sound runs - a stationary
 * hiss measures the same in a 1 s clip and a 4 s one - which matters here precisely because the
 * two buffers may have different durations. Bands are compared index-wise only after their edges
 * are confirmed to agree to PwCompareLimits::ResidualEdgeAgreement; when the two decompositions
 * landed on different band grids the whole residual comparison is marked unmeasured with a
 * reason, rather than published against the wrong frequency axis.
 *
 * FAILURE DIRECTION (rpc-design.md §7, §12). Out is cleared on entry, so a caller that ignored
 * the return value cannot read a stale report. Degenerate inputs are detected and ANSWERED in
 * this fixed order, each naming the offending side:
 *   1. either buffer has no frames           -> AUDIO_EMPTY_BUFFER
 *   2. either buffer holds a non-finite sample-> AUDIO_NON_FINITE_SAMPLES
 *   3. either buffer is digitally silent      -> AUDIO_EMPTY_BUFFER (named as silence)
 *   4. either buffer is malformed             -> INVALID_PARAMS
 *   5. the two sample rates disagree          -> INVALID_PARAMS naming both
 * Step 2 MUST precede step 3: NaN compares false against every threshold, so a NaN-filled buffer
 * scanned for its peak first measures as digital silence and would be reported as "there was no
 * signal" - the wrong remedy, pointed at the wrong stage.
 *
 * Silence is an ERROR here even though PwAnalyzeBuffer treats it as a success. Comparing a sound
 * against silence produces no deviations at all, which reads as "these match" - the one false
 * report this verb must never produce.
 *
 * A sample-rate mismatch is refused rather than resampled: resampling behind the caller's back
 * would compare a signal they never passed, which is the stance FPwAudioBuffer::MixInto already
 * takes at the same seam.
 *
 * A failure of either analysis or either decomposition fails the whole comparison and forwards
 * that stage's code. Degrading to "loudness and brightness only" would hand back a report that
 * looks complete while the mode diff - the reason this verb exists - silently did not run.
 *
 * @return true when Out carries a usable report. A true return with an empty Deviations array is
 *         the measurement "these two agree everywhere I looked" (see IsMatch).
 */
bool PwCompareAudio(const FPwAudioBuffer& Reference, const FPwAudioBuffer& Candidate,
                    FPwCompareResult& Out, FString& OutErrorCode, FString& OutError);

/**
 * JSON for an RPC response.
 *
 * An unmeasured scalar is OMITTED and its reason listed under the top-level `unmeasured` object,
 * so an absence is always explicit and always explained (rpc-design.md §1). An unmeasured report
 * serializes to `{"measured":false,"unmeasuredReason":"..."}` and nothing else.
 *
 * The `missing` and `extra` mode arrays are emitted even when empty: "nothing is missing" is a
 * measurement the caller needs, and an omitted array would be indistinguishable from a matcher
 * that did not run.
 *
 * Numbers are rounded to a per-quantity precision. The report must not publish sixteen digits of
 * a centroid estimate that is good to three, and UE's JSON writer prints doubles with "%.17g".
 */
TSharedPtr<FJsonObject> SerializeCompareResult(const FPwCompareResult& In);

/**
 * Analysis by resynthesis: turn a decomposed reference into a draft FPwSynthRecipe.
 *
 * THE MAPPING, element by element:
 *
 *   modes -> a `modal` layer. FPwModalFit's FreqHz / InitialGainDb / DecayMs become one row each
 *      of modeFreqsHz / modeGainsDb / modeDecaysMs. DecayMs is PwGenModal's T60 (the time for the
 *      amplitude to fall by a factor of 1000) with no reinterpretation, which is what makes the
 *      round trip reproduce the reference's tail rather than a scaled version of it.
 *      InitialGainDb is EXTRAPOLATED BACK TO THE LAYER'S START along the fitted exponential:
 *      the fit's level refers to the track's first measured frame, and the analysis grid's first
 *      frame centre sits half an FFT window into the signal, so publishing it unchanged would
 *      make every mode quiet by 60 * StartMs / DecayMs dB - about 5 dB for a 250 ms mode on the
 *      reference grid. The extrapolation is two measured numbers, not an invented one.
 *      The layer carries NO amplitude envelope: the modes' own decays are the envelope, and an
 *      envelope on top would apply the decay twice.
 *
 *   transients -> the exciter and its length. A burst no longer than the schema's own default
 *      exciter is a click and selects `impulse` (which has no length at all). A longer burst
 *      whose 5-95% spectral span is at least as wide as its centroid is broadband and selects
 *      `noise`; a longer narrow one selects `strike`, the deterministic mallet pulse. No
 *      transients at all leaves `impulse`, the flat unshaped drive.
 *
 *   residual bands -> a `noise` layer with the residual's own band limits, colour and contour.
 *      lowCutHz / highCutHz are the outer edges of the bands within
 *      PwToRecipeLimits::NoiseSupportRangeDb of the loudest one; `color` is the nearest of the
 *      schema's five tilts to the measured slope; the amplitude envelope is the band-summed
 *      residual level over time, normalized to its own peak.
 *
 *   envelopes -> the noise layer's ampEnvelope, from the measured residual contour. The modal
 *      layer deliberately has none, per above.
 *
 *   stereo -> NOT emitted, and this is the one mapping that declines. `width` is a master-only
 *      mid/side effect, and mid/side has nothing to work on when the bus is mono: every
 *      generator here is mono and this mapping places every layer at pan 0, so a `width` effect
 *      on the resulting bus is an exact no-op - the silent-nothing rpc-design.md §3 exists to
 *      prevent. When the decomposition measured a genuinely non-mono image, that fact is
 *      returned as a NOTE naming what the caller must do instead (pan the layers apart, or add a
 *      second differently-seeded noise layer) rather than emitted as an effect that does nothing.
 *
 * OMIT RATHER THAN INVENT. A layer whose inputs are unmeasured or below the confidence floor is
 * left out entirely. A two-layer recipe that is honest beats a five-layer one that is guessed,
 * and the model can add what is missing once it can hear the draft.
 *
 * THE ONE ESTIMATE, FLAGGED AS ONE. The noise layer's `gainDb` is the residual's measured
 * broadband level in dBFS taken at face value against PwGenNoise's output, and PwGenNoise
 * publishes no dBFS calibration (its own header states that absolute level is the master
 * normalize block's job). So the noise layer's SHAPE - band limits, tilt, contour - is measured
 * and its ABSOLUTE LEVEL is a first estimate. It is named in the returned note, and
 * PwCompareAudio's loudness delta closes it in one iteration.
 *
 * THE OUTPUT PARSES, AND THAT IS CHECKED HERE (rpc-design.md §4). The recipe is serialized and
 * run back through ParseSynthRecipe before it is handed over, and `Out` receives the PARSED
 * form - not the hand-built one - so what the caller gets is the canonical, fully-materialized
 * recipe the schema itself accepted. A recipe that does not validate is refused rather than
 * returned, because a draft that errors on first use costs the caller more than no draft. This
 * is exactly the check the write path cannot fake: it runs through the schema's own parser, not
 * through a readback of the values this function just set.
 *
 * NOTES ARE NOT FAILURES. Following PwTrackPartials' convention, a successful call that had to
 * drop, cap or estimate something returns true with an EMPTY OutErrorCode and a note in
 * OutError. A non-empty OutError beside an empty OutErrorCode and a true return is a note; the
 * caller distinguishes the two by the return value together with OutErrorCode.
 *
 * FAILURE DIRECTION. `Out` is cleared on entry, so a failure leaves a default-constructed recipe
 * - zero layers and zero duration, which ParseSynthRecipe and PwRenderRecipe both reject - and
 * never a half-built one. Rejects, in order: an unmeasured decomposition (INVALID_PARAMS,
 * quoting UnmeasuredReason), a sample rate or duration outside the schema's range
 * (INVALID_PARAMS naming the limit), and a decomposition that yielded no usable layer at all
 * (INVALID_PARAMS - a recipe needs at least one layer, and an empty one is not a draft).
 */
bool PwDecompositionToRecipe(const FPwDecomposition& In, FPwSynthRecipe& Out,
                             FString& OutErrorCode, FString& OutError);

// ---------------------------------------------------------------------------------------------
// Confidence floors for PwDecompositionToRecipe. Published for the same reason the compare
// thresholds are: a caller cannot tell "omitted because unmeasured" from "omitted because below
// the floor" without seeing the floor.
// ---------------------------------------------------------------------------------------------
namespace PwToRecipeLimits
{
    /**
     * A residual whose loudest band never reaches this level is not authored as a noise layer,
     * dBFS. -60 dB is the same floor the analysis already calls the end of a sound
     * (PwAudioAnalysisLimits::EnvelopeFloorDb, relative there and absolute here), and it sits at
     * or under the leakage the spectral subtraction leaves behind - so a layer built from
     * anything quieter would mostly reproduce the analyzer's own artefacts.
     */
    inline constexpr double MinResidualPeakBandDb = -60.0;

    /**
     * Bands within this much of the loudest residual band define the noise layer's band limits,
     * dB. 20 dB is the same "no longer audible over what follows" span PwDetectTransients uses
     * to bound a transient window.
     */
    inline constexpr double NoiseSupportRangeDb = 20.0;

    /**
     * A transient no longer than this selects the `impulse` exciter, ms. It is the schema's own
     * default exciterMs: a burst shorter than the shortest burst the schema describes is a
     * click, and a click is what `impulse` is.
     */
    inline constexpr double ImpulseMaxTransientMs = 2.0;

    /**
     * A transient whose 5-95% spectral span reaches this multiple of its centroid is broadband
     * and selects `noise` rather than `strike`. 1.0 - the span is at least as wide as the
     * centroid frequency - separates a near-flat burst from a tonal one without a second
     * threshold to tune.
     */
    inline constexpr double BroadbandSpanRatio = 1.0;

    /**
     * Level-per-octave a white spectrum reads at when measured as TOTAL POWER PER LOG-SPACED
     * BAND, dB. Not zero: a log-spaced band holds a bandwidth proportional to its centre
     * frequency, so it holds twice the bins - and twice the power - one octave up. Measured
     * slopes are corrected by this before being matched against the schema's colours, and
     * without the correction every white residual would be reported as blue.
     */
    inline constexpr double BandwidthTiltDbPerOctave = 3.0102999566398120;

    /**
     * L/R correlation at or above which the reference's image is close enough to mono that this
     * mapping's all-centred layers reproduce it, and no stereo note is returned. Below it, the
     * measured image is reported as a note naming what the caller must author by hand - see the
     * stereo paragraph on PwDecompositionToRecipe. 0.95 is roughly where a mid/side split leaves
     * under 5% of the level on the side channel, i.e. an image a listener calls centred.
     */
    inline constexpr double MonoCorrelationFloor = 0.95;
}
