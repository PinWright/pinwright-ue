// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"

struct FPwAudioAnalysis;
struct FPwAudioBuffer;
class FJsonObject;

// PwAudioContext.h - the two questions the descriptor report structurally cannot answer.
//
// Every metric in PwAudioAnalysis.h describes a sound heard ONCE, IN ISOLATION, ON A FULL-RANGE
// MONITOR. Two whole classes of shipped failure live outside that frame, and neither is visible
// in a single-shot analysis no matter how many scalars it carries:
//
//   1. RETRIGGER. A UI blip that is pleasant once is unbearable at 10 Hz. Menu navigation, weapon
//      fire, footsteps and impact spam all retrigger fast; overlapping tails accumulate, the low
//      end builds, and the result stops being an event and becomes a texture - or clips. A
//      one-shot analysis measures the first repetition and nothing else.
//   2. PLAYBACK CONDITION. A sound mixed on a full-range monitor can vanish on a laptop. If the
//      transient and the identifying spectral content live below ~200 Hz, the sound is simply not
//      there for a large share of players, and the full-range report says it is fine.
//
// Both families SYNTHESIZE a new signal and re-measure it, so both are far more expensive than
// the descriptor pass. They are therefore OPT-IN (FPwAudioContextRequest), default off, and
// omitted from the serialized report entirely when they were not run - not reported as zeros and
// not even listed as unmeasured, because "you did not ask for this" is a different fact from
// "this could not be measured" (rpc-design.md §1).
//
// The three rules PwAudioAnalysis.h is built on apply here unchanged:
//
//   §1  A sound whose tail is shorter than the retrigger interval genuinely does not accumulate.
//       That MUST read as a measured "no buildup" (a 0.0 dB delta with tailOverlapRatio 0), never
//       as an unmeasured gap - the whole point of asking is to learn that the sound is safe.
//   §6  Every accumulation figure is a SIGNED delta against a stated reference, so "quieter when
//       retriggered" and "louder when retriggered" are different numbers rather than the same
//       magnitude with different prose. PeakDbDelta and CentroidHzDelta are signed for the same
//       reason. RetainedEnergyRatio is the one quantity that cannot be signed - a ratio of two
//       energies has no direction - so it is PAIRED instead: the FullRange row sits in the same
//       array with ratio 1.0, and FPwPlaybackConditionSet publishes the absolute reference the
//       deltas were taken against.
//   §7  Degenerate cases are answered first and IN ORDER, with non-finite tested before silence.
//       FMath::Max(0.0, NaN) returns 0, so a NaN-filled buffer checked for silence first
//       measures as digital silence and is reported as a valid, quiet render.
//
// ENGINE VERSION: Audio::FBiquadFilter / Audio::EBiquadFilter (DSP/Filter.h) are present with the
// same signature on UE 5.3 through 5.8, and only the single-channel
// ProcessAudio(const float*, int32, float*) overload is used - the planar
// ProcessAudio(const float* const*, ...) overload is 5.4+ and is deliberately avoided, exactly as
// PwFxChainA.cpp documents. PwAnalyzePlaybackConditions re-measures through PwAnalyzeBuffer,
// which already owes the Audio::FLKFSAnalyzer row in docs/engine-version-support.md; this file
// adds no new 5.8-only symbol and therefore owes no new row.

// ---------------------------------------------------------------------------------------------
// Documented constants. Every threshold either aliases one PwAudioAnalysisLimits already defines
// (static_asserted against it in the .cpp, so the two cannot drift apart) or states the basis it
// was chosen on.
// ---------------------------------------------------------------------------------------------
namespace PwAudioContextLimits
{
    /**
     * Corner of the "low band" the retrigger family reports buildup in. ALIASED, not chosen
     * again: it is PwAudioAnalysisLimits::BandEdgesHz[1], so `lowBandDbDelta` here covers exactly
     * the region the descriptor report publishes as `hz20_150`. PwAudioContext.cpp static_asserts
     * the two agree.
     */
    inline constexpr double LowBandCornerHz = 150.0;

    /**
     * Depth, in dB below the loudest moment of one retrigger period, at which a dip counts as a
     * GAP between events rather than as ripple inside one continuous texture. ALIASED to
     * -PwAudioAnalysisLimits::EnvelopeTailDb (20 dB, a tenth of the amplitude): the descriptor
     * report already uses that level as the boundary between the body of a sound and its quiet
     * ring-out, and "the level never returns to the ring-out region between repetitions" is
     * exactly what "the gaps have disappeared" means. Static_asserted in the .cpp.
     */
    inline constexpr double ContinuousGapDb = 20.0;

    /**
     * Blocks per retrigger period in the gap envelope. Deliberately NOT the report's fixed 10 ms
     * block: at 20 Hz a period is 50 ms, so a 10 ms block is a fifth of the whole period and
     * would smear away the gap it is trying to find. 16 blocks per period keeps the resolution
     * proportional to the question at every rate.
     */
    inline constexpr int32 GapBlocksPerPeriod = 16;

    /**
     * Extra repetitions rendered past the number needed for steady state. Two, which buys two
     * complete steady periods of lead-in before the measurement window opens - 1 s at 2 Hz and
     * 100 ms at 20 Hz, the latter being ~15 cycles of the 150 Hz low-band corner, so the low-band
     * biquads are settled before anything is measured.
     */
    inline constexpr int32 SteadyMarginRepeats = 2;

    /**
     * Cap on the work one rate may cost, in per-channel sample writes (repetitions x active
     * source length). The synthetic buffer itself is only about twice the source length, but
     * building it costs one write per copied sample, and a 30 s source at 20 Hz would be 600
     * overlapping copies. Past the cap the rate is REJECTED with a message naming both levers
     * (shorter source, lower rate) rather than silently measured on a truncated build that never
     * reached steady state.
     */
    inline constexpr int64 MaxSyntheticSampleWrites = 32000000;

    /** Retrigger rates outside this window are rejected. Below 0.05 Hz a "repeat" is not a
        repeat; above 200 Hz the repetition rate is itself an audio frequency and the question
        the family answers no longer applies. */
    inline constexpr double MinRetriggerRateHz = 0.05;
    inline constexpr double MaxRetriggerRateHz = 200.0;

    /**
     * Playback-condition filter corners. These are CHEAP DETERMINISTIC APPROXIMATIONS, not device
     * models - no measured impulse response is involved, and the report says so at the call site
     * rather than presenting them as a laptop or a phone.
     *
     * Laptop: 200 Hz, two cascaded Butterworth-Q highpass biquads (-24 dB/octave). A laptop's
     * driver-plus-box resonance typically sits at 180-250 Hz with a steep acoustic rolloff below
     * it; 200 Hz is the middle of that window and the slope is the order that reaches it.
     *
     * Phone: 400 Hz highpass on the same -24 dB/octave slope, plus a single Butterworth-Q lowpass
     * at 10 kHz. Micro-speakers resonate higher still (their usable response commonly starts
     * between 300 and 800 Hz) and roll off in the top octave; 400 Hz and 10 kHz are the middle of
     * the stated windows.
     */
    inline constexpr double LaptopHighpassHz = 200.0;
    inline constexpr double PhoneHighpassHz = 400.0;
    inline constexpr double PhoneLowpassHz = 10000.0;

    /** Butterworth Q. Used for every stage so the cascade's shape is one documented choice. */
    inline constexpr double FilterQ = 0.70710678118654752;

    /**
     * The realisable cutoff window of Audio::FBiquadFilter. Mirrors
     * FBiquadFilter::ClampCutoffFrequency (Filter.cpp:64-67) exactly - it clamps with no error, so
     * a corner outside this window would run a filter nobody asked for. Same two numbers
     * PwFxChainA.cpp rejects against, for the same reason.
     */
    inline constexpr double MinFilterCutoffHz = 5.0;
    inline constexpr double NyquistFraction = 0.45;

    /**
     * The Quiet condition: the sound played 30 dB below the level it was mixed at, which is what
     * a player who turned the game down does. A pure gain, no filtering - the point of this
     * condition is the audibility floor, not a spectrum.
     */
    inline constexpr double QuietGainDb = -30.0;

    /**
     * Absolute level below which a rendered condition counts as inaudible, dBFS. On a chain
     * calibrated so 0 dBFS is about 85 dB SPL, -60 dBFS is about 25 dB SPL - roughly the noise
     * floor of a quiet real room. Stated as the approximation it is; what matters is that
     * `bAudible` is a comparison of a measured number against this stated one rather than a guess.
     */
    inline constexpr double AudibilityFloorDb = -60.0;

    /**
     * How much of the full-range attack prominence a condition must retain for
     * `bTransientSurvives`. 0.5 is a halving in amplitude (-6 dB) of the attack step measured
     * RELATIVE to the sound's own body, so a uniform level reduction (the Quiet condition) does
     * not read as a destroyed transient while a filter that removes the attack's frequency
     * content does.
     */
    inline constexpr double TransientRetentionFraction = 0.5;

    /** dB floor for a zero linear value, matching FPwLoudnessResult's documented -200 floor. */
    inline constexpr double DbFloor = -200.0;
}

// ---------------------------------------------------------------------------------------------
// Retrigger
// ---------------------------------------------------------------------------------------------

/**
 * One retrigger rate, measured on the STEADY STATE of the sound repeating at that rate.
 *
 * The three dB fields are SIGNED DELTAS, not absolute levels: each is the measurement taken over
 * one steady-state period minus the same measurement taken over the identical window with only
 * ONE copy in it. That reference is what makes 0.0 dB mean "retriggering this changes nothing",
 * which is the answer a caller most needs to be able to trust (§1). The absolute levels they are
 * deltas against are already published beside them in the report - `technical.peakDb` and
 * `technical.rmsDb` for the broadband pair, `spectral.hz20_150` for the low band - so nothing is
 * lost by reporting the difference.
 */
struct FPwRetriggerPoint
{
    /** Repetitions per second this point was measured at. */
    double RateHz = 0.0;

    /** Steady-state peak minus single-copy peak, dB. Positive = the repeats stack higher. */
    double PeakDb = 0.0;

    /** Steady-state RMS minus single-copy RMS over the same period, dB. */
    double RmsDb = 0.0;

    /** The same delta measured through the 150 Hz low-band cascade. Mud accumulates fastest. */
    double LowBandDb = 0.0;

    /**
     * Samples at or beyond PwAudioAnalysisLimits::ClipThreshold within ONE steady-state period,
     * counted per channel-sample. Per repetition rather than per buffer so the number means the
     * same thing at every rate. Unsigned deliberately: clipping has one failure direction.
     */
    int32 ClippedSamples = 0;

    /**
     * Fraction of the sound's energy that arrives more than one retrigger interval after its own
     * onset - i.e. the share that is still sounding when the next repetition begins. 0.0 for a
     * sound that finishes inside its own interval (which is the measured statement that it cannot
     * accumulate), approaching 1.0 for a tail far longer than the interval.
     */
    double TailOverlapRatio = 0.0;

    /**
     * The perceptual verdict: within one steady-state period the level never falls
     * PwAudioContextLimits::ContinuousGapDb below that period's own peak, so the gaps between
     * repetitions have closed and the sound has stopped being an event and become a texture.
     * Measured on a 16-blocks-per-period envelope of the synthesized signal, not asserted from
     * the rate.
     */
    bool bEffectivelyContinuous = false;
};

/** The retrigger family. Points come back in ASCENDING rate order whatever order they were asked in. */
struct FPwRetriggerResult
{
    bool bMeasured = false;

    /** Why the family is absent. Meaningful only while bMeasured is false. */
    FString UnmeasuredReason;

    TArray<FPwRetriggerPoint> Points;

    /**
     * Rate above which the sound reads as continuous, DERIVED from the measured points rather
     * than asserted as a constant: the gap depth falls monotonically with rate, so this is the
     * log-frequency interpolation of the rate at which it crosses ContinuousGapDb, taken over the
     * lowest bracketing pair of measured points.
     */
    double ContinuousAboveRateHz = 0.0;

    /**
     * False when the measured points do not BRACKET the crossing - every rate already continuous
     * (the true threshold is below the lowest rate asked for) or none of them continuous (it is
     * above the highest). Extrapolating past the measured range would be a fabricated number, so
     * the field is omitted from the JSON instead; the per-point bEffectivelyContinuous flags show
     * which side the caller ran off.
     */
    bool bContinuousRateMeasured = false;
};

/** The default rate set: 2, 5, 10 and 20 Hz - menu spam, footsteps, weapon fire, impact spam. */
TArray<double> PwGetDefaultRetriggerRates();

/**
 * Measure what this sound becomes when it is retriggered.
 *
 * For each rate a synthetic buffer is built by summing copies of the input at 1/rate spacing, and
 * the measurement is taken over ONE PERIOD STARTING AT THE LAST ONSET. That window is exact
 * steady state rather than an approximation of it: within it the signal is the sum over every
 * copy that has already started, copies that start later contribute nothing to it, and the number
 * of repetitions rendered (ceil(activeLength / interval) + 2) guarantees every predecessor that
 * can still be sounding is present. The +2 margin also gives the low-band biquads two complete
 * steady periods to settle in before the window opens.
 *
 * Leading and trailing silence are trimmed off the input first. A designer retriggering a cue
 * triggers the SOUND, not the asset's padding; leaving 500 ms of leading silence in place would
 * compare a window of silence against a window of audio and report a fabricated infinite buildup.
 *
 * Degenerate cases are answered first, in this order (§7):
 *   1. zero frames                -> false, AUDIO_EMPTY_BUFFER
 *   2. broken buffer / bad rate   -> false, INVALID_PARAMS
 *   3. any non-finite sample      -> false, AUDIO_NON_FINITE_SAMPLES   (before silence)
 *   4. digital silence            -> false, AUDIO_EMPTY_BUFFER - the same spelling
 *                                   PwComputeLoudness uses for the same condition
 *   5. empty / non-finite / out-of-window / too-expensive rate -> false, INVALID_PARAMS naming
 *                                   the offending value and the way out
 * Step 3 must precede step 4 (NaN fails every comparison, so it would otherwise pass for silence).
 *
 * @return true only when Out.bMeasured is true. Out is reset on entry, so a caller that ignored
 *         the bool cannot read a stale point.
 */
bool PwAnalyzeRetrigger(const FPwAudioBuffer& In, const TArray<double>& RatesHz,
                        FPwRetriggerResult& Out, FString& OutErrorCode, FString& OutError);

// ---------------------------------------------------------------------------------------------
// Playback conditions
// ---------------------------------------------------------------------------------------------

enum class EPwPlaybackCondition : uint8
{
    /** The unfiltered input. The reference every other row's deltas are taken against. */
    FullRange,
    LaptopSpeaker,
    PhoneSpeaker,
    Quiet,

    /** Channel fold-down. Also the only measurement that catches phase cancellation. */
    Mono
};

/** Stable JSON spelling of a condition. One writer, so the enum and the report cannot disagree. */
const TCHAR* PwPlaybackConditionName(EPwPlaybackCondition Condition);

/** One condition, re-measured after the input was rendered through it. */
struct FPwConditionResult
{
    EPwPlaybackCondition Condition = EPwPlaybackCondition::FullRange;

    /**
     * True on every row of a measured set. The set fails as a whole rather than per row: a
     * condition that could not be rendered means the reference itself is in doubt, and a partial
     * set whose reference row is missing would publish deltas against nothing.
     */
    bool bMeasured = false;

    /**
     * Mean square of the rendered signal over the mean square of the full-range input - the share
     * of the ENERGY that survives this condition. 1.0 for FullRange by construction. A ratio has
     * no sign, so this is the one figure §6 asks to be paired rather than signed: the absolutes it
     * was taken against are on FPwPlaybackConditionSet.
     */
    double RetainedEnergyRatio = 0.0;

    /** Rendered peak minus full-range peak, dB, each floored at PwAudioContextLimits::DbFloor. */
    double PeakDbDelta = 0.0;

    /**
     * Rendered spectral centroid minus full-range centroid, Hz. Positive for a highpass. Read
     * bCentroidMeasured first - a condition that rendered to digital silence has no centroid, and
     * a 0.0 delta there would read as "brightness unchanged" for a signal that is gone.
     */
    double CentroidHzDelta = 0.0;
    bool bCentroidMeasured = false;

    /**
     * The transient is still there. Two stated comparisons, both on measured numbers, both
     * required:
     *   1. the attack step (the largest rise between consecutive 10 ms envelope blocks) is above
     *      PwAudioContextLimits::AudibilityFloorDb in absolute dBFS, and
     *   2. the attack step RELATIVE to the rendering's own overall RMS is at least
     *      PwAudioContextLimits::TransientRetentionFraction of the same relative figure measured
     *      on the full-range input.
     * Leg 2 is relative on purpose: a uniform level cut (Quiet) keeps a transient that is merely
     * quieter, while a highpass that removes the attack's frequency content does not, and only a
     * ratio against the sound's own body separates those two.
     */
    bool bTransientSurvives = false;

    /**
     * The loudest 10 ms block of the rendering, in dBFS on the two-channel energy, is above
     * PwAudioContextLimits::AudibilityFloorDb. A block RMS rather than a sample peak, so a single
     * sample of a click cannot certify a sound that carries no loudness.
     */
    bool bAudible = false;
};

/**
 * The condition family, with the reference the deltas are against (§6 - a delta a caller cannot
 * un-difference is not actionable).
 */
struct FPwPlaybackConditionSet
{
    bool bMeasured = false;
    FString UnmeasuredReason;

    /** One row per EPwPlaybackCondition, in enum order, FullRange first. */
    TArray<FPwConditionResult> Conditions;

    /** Full-range peak, dBFS, floored at DbFloor. The absolute PeakDbDelta is measured from. */
    double ReferencePeakDb = 0.0;

    /** Full-range loudest 10 ms block, dBFS. The absolute bAudible is decided against. */
    double ReferenceBlockPeakDb = 0.0;

    /**
     * Full-range spectral centroid, Hz, over both channels. Absent only when neither channel has
     * a measurable spectrum - an anti-phase pair cancels in the downmix but not on two speakers,
     * so its reference brightness is still published.
     */
    double ReferenceCentroidHz = 0.0;
    bool bReferenceCentroidMeasured = false;
};

/**
 * Render the input through each cheap deterministic playback approximation and re-measure it.
 *
 * The re-measurement goes through PwAnalyzeBuffer rather than a private copy of the spectral
 * code, so `centroidHzDelta` here is a difference of two numbers that mean exactly what
 * `spectral.centroidHz` means in the report (rpc-design.md §2 - one writer per fact). It is
 * taken per CHANNEL and combined by channel energy rather than on the 0.5 * (L + R) downmix that
 * family analyses on its own: that downmix is exactly zero for an anti-phase pair, and reading
 * the reference off it would withhold the brightness of a sound that is perfectly healthy on two
 * speakers. For correlated material the two are the same number, because there each channel IS
 * the downmix. That re-measurement is also most of the cost, and most of why this family is
 * opt-in.
 *
 * Same degenerate ladder as PwAnalyzeRetrigger, non-finite before silence.
 *
 * @return true only when Out.bMeasured is true.
 */
bool PwAnalyzePlaybackConditionSet(const FPwAudioBuffer& In, FPwPlaybackConditionSet& Out,
                                   FString& OutErrorCode, FString& OutError);

/** Rows only. Thin wrapper over PwAnalyzePlaybackConditionSet; Out is emptied on every failure. */
bool PwAnalyzePlaybackConditions(const FPwAudioBuffer& In, TArray<FPwConditionResult>& Out,
                                 FString& OutErrorCode, FString& OutError);

// ---------------------------------------------------------------------------------------------
// Attaching the two families to a descriptor report
// ---------------------------------------------------------------------------------------------

/**
 * What the caller asked for. Both flags default OFF - these families synthesize and re-analyze
 * whole buffers and cost multiples of the descriptor pass, so they are never run because a code
 * path forgot to say no.
 */
struct FPwAudioContextRequest
{
    bool bRetrigger = false;

    /** Empty means PwGetDefaultRetriggerRates(). A rate of 0 is an error, not a default. */
    TArray<double> RetriggerRatesHz;

    bool bPlaybackConditions = false;

    bool IsAnyRequested() const { return bRetrigger || bPlaybackConditions; }
};

/**
 * Run the requested families and attach them to an existing report.
 *
 * A family that was NOT requested is left unset on FPwAudioAnalysis, which is a different state
 * from a family that was requested and failed: the first is omitted from the JSON entirely, the
 * second appears under `unmeasured` with its reason.
 *
 * @return true when every requested family was measured. On a failure the families are still
 *         attached carrying their honest unmeasured state, and OutErrorCode / OutError carry the
 *         FIRST failure so a handler can forward it unmodified.
 */
bool PwAnalyzeAudioContext(const FPwAudioBuffer& In, const FPwAudioContextRequest& Request,
                           FPwAudioAnalysis& InOut, FString& OutErrorCode, FString& OutError);

/**
 * Serialize whatever context families the report carries into an existing JSON root, following
 * the same omit-when-unmeasured convention SerializeAudioAnalysis uses for the six descriptor
 * families: measured goes under its own key on Root, requested-but-unmeasured goes under
 * Unmeasured, and not-requested writes nothing to either.
 *
 * SIZE. Both families are off by default, so the default summary form is byte-for-byte what it
 * was before this file existed and the suite's 2,500-character summary gate is untouched. Opting
 * in adds roughly 0.9 KB (four retrigger rates) and 1.1 KB (five conditions) as the transport
 * encodes it, which can push a bare handler result past the ~4,250-character ceiling - the same
 * deliberate trade bFullDetail already makes for a caller that asked for more.
 */
void PwSerializeAudioContext(const FPwAudioAnalysis& In, bool bFullDetail,
                             const TSharedPtr<FJsonObject>& Root,
                             const TSharedPtr<FJsonObject>& Unmeasured);
