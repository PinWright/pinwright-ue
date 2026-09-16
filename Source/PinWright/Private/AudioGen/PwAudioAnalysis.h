// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"

#include "AudioGen/PwAudioContext.h"     // FPwRetriggerResult / FPwPlaybackConditionSet - the opt-in families
#include "AudioGen/PwAudioFeatures.h"    // FPwOnset / FPwPitchPointResult - loudness, onset and pitch back-ends
#include "AudioGen/PwSynthRecipe.h"      // EPwSynthMetric - the vocabulary `targets` is scored against

struct FPwAudioBuffer;
class FJsonObject;

// PwAudioAnalysis.h - the numeric description of a rendered buffer.
//
// This report is the substitute for the calling LLM's missing ears. It is the only thing an
// agent iterating on a synth recipe can compare against, so every number here is either a
// measurement or absent. There is no third state: a family that was not measured does not
// appear in the serialized output at all, and the reason it did not appear is published beside
// the families that did.
//
// Three rules from docs/rpc-design.md shape the whole file:
//
//   §1  An unmeasured value must be ABSENT, not zero. Each family carries an
//       FPwAudioFamilyState whose bMeasured defaults to false, and SerializeAudioAnalysis omits
//       a family whose state is unmeasured. Within a measured family, a scalar that a
//       particular signal cannot define is TOptional and is omitted the same way - a sustained
//       tone has no decay time, and reporting 0 ms (or the buffer length) would be a fabricated
//       measurement either way.
//
//   §7  Order the checks so zero cannot be reported as a small number. PwAnalyzeBuffer runs its
//       degenerate-case checks first and ANSWERS there rather than falling through:
//         no frames        -> AUDIO_EMPTY_BUFFER
//         non-finite       -> AUDIO_NON_FINITE_SAMPLES   (before silence: NaN fails every
//                                                         comparison, so a NaN-filled buffer
//                                                         would otherwise measure as silence)
//         digital silence  -> SUCCESS, technical-only, every level/brightness family absent
//       A silent buffer reported as "-90 LUFS, centroid 0 Hz" tells the agent to raise the gain
//       on a signal that does not exist; naming the silence and withholding the rest is the
//       whole point of the ordering.
//
//   §6  Every deviation is signed or paired. DcOffset is signed, not |dc|. StereoCorrelation
//       spans -1..+1 so an anti-phase image cannot score like an in-phase one. Pitch motion
//       carries a signed SemitoneDelta so "rising" and "falling" are different numbers rather
//       than the same magnitude with different prose. Band ratios are a six-way split rather
//       than a single "brightness error", so too-dark and too-bright move different entries.
//       (Metrics whose ideal is a hard zero and whose only direction is "more" - the click
//       magnitudes, the clipped-sample count, the non-finite count - are deliberately unsigned:
//       they have one failure direction, not two.)
//
// Metric NAMES are not free: `FPwSynthRecipe::Targets` constrains EPwSynthMetric values, and a
// target can only be scored against a name this report emits. PwGetAudioAnalysisMetric is the
// single bridge between the two vocabularies, so the scorer and the reporter cannot disagree
// about what `centroidHz` means (rpc-design.md §2 - one writer per fact).
//
// Every engine symbol used here (FMath, TOptional, FJsonObject, and the Audio::* symbols reached
// indirectly through PwStft.h) is present unchanged on UE 5.3 through 5.8, so this file owes no
// row in docs/engine-version-support.md.

// ---------------------------------------------------------------------------------------------
// Documented constants. Every threshold the report depends on is named here rather than spelled
// inline, because the report is only comparable across runs if the reader can see the floors.
// ---------------------------------------------------------------------------------------------
namespace PwAudioAnalysisLimits
{
    /**
     * Peak absolute sample below which the buffer is DIGITAL SILENCE rather than quiet audio.
     * Same 1e-9 (about -180 dBFS, ~36 dB below a 24-bit LSB) as PwStft's floor, deliberately:
     * the two must agree or a buffer could be analysed here and rejected there.
     */
    inline constexpr double SilencePeak = 1e-9;

    /**
     * At-or-beyond-full-scale threshold for the clipped-sample count. Float PCM's full scale is
     * +/-1.0; the epsilon catches a sample a float round-trip left one ULP short of it.
     * Counted per channel-sample, so a stereo frame clipped on both sides counts twice.
     */
    inline constexpr double ClipThreshold = 0.999999;

    /** Block length of the RMS envelope every timing metric is measured on. */
    inline constexpr double EnvelopeBlockMs = 10.0;

    /**
     * Envelope floor, in dB RELATIVE to the loudest envelope block. -60 dB is the level below
     * which the sound has stopped for any practical purpose, and it is the same floor
     * EPwSynthMetric::DecayMs is documented with, so `decayMs` here means what the recipe schema
     * says it means. Relative rather than absolute so the same signal measures the same whether
     * it was rendered at -6 or -40 dBFS.
     */
    inline constexpr double EnvelopeFloorDb = -60.0;

    /**
     * Second envelope threshold, again relative to the loudest block. The point where the sound
     * has dropped to a tenth of its peak amplitude: the boundary between the body of the decay
     * and its quiet ring-out. See FPwAudioEnvelope::TailMs.
     */
    inline constexpr double EnvelopeTailDb = -20.0;

    /**
     * Confidence below which pitch is SUPPRESSED ENTIRELY rather than reported.
     *
     * ALIASED, not chosen again: PwEstimatePitch already uses PwPitchConfidenceFloor to pick the
     * windows that feed MedianF0Hz, and a second number here would let the report's gate drift
     * away from the estimator's. One writer, one threshold (rpc-design.md §2).
     *
     * The value is 0.5 and the measured reference points behind it are on
     * PwAudioFeatures.h: a clean sine scores above 0.95, a moderate chirp above 0.9, and uniform
     * white noise about 0.13. The gate sits in the empty middle of that bimodal distribution, so
     * it is insensitive to the exact estimator.
     *
     * Suppression is not cosmetic, and it is applied here as well as in the back-end because the
     * report is what the agent reads: noise has no f0, and a reported 0 Hz - or a
     * confident-looking 137 Hz read off a click - is worse than no number at all, because the
     * agent will chase it.
     */
    inline constexpr double MinPitchConfidence = PwPitchConfidenceFloor;

    /** Preferred spectral analysis window. Halved down to MinFftSize for a short buffer. */
    inline constexpr int32 PreferredFftSize = 2048;
    inline constexpr int32 MinFftSize = 64;

    /** Fraction of cumulative spectral magnitude that defines the rolloff frequency. */
    inline constexpr double RolloffFraction = 0.85;

    /**
     * Dominant-peak selection. Greedy, strongest first: a candidate must be at least
     * MinPeakSeparationBins from every already-selected peak and within PeakDynamicRangeDb of
     * the strongest. The separation is load-bearing rather than tidy - a periodic Hann main lobe
     * has sidelobes that are genuine local maxima, and without it a single pure tone reports as
     * five "dominant peaks" all within 200 Hz of each other.
     */
    inline constexpr int32 MinPeakSeparationBins = 8;
    inline constexpr double PeakDynamicRangeDb = 40.0;
    inline constexpr int32 MaxSummaryPeaks = 3;
    inline constexpr int32 MaxDetailPeaks = 8;

    /**
     * Cap on any per-frame series in the full-detail form. A 60-second render at hop 512 is
     * 5,600 frames; emitting them raw would be ~70 KB of one field. Over the cap the series is
     * decimated and SAYS SO (`decimated`, `stride`, `totalPoints`), so a reader never mistakes a
     * thinned series for the whole one.
     */
    inline constexpr int32 MaxSeriesPoints = 256;

    /**
     * The six reported energy bands, in Hz. Ratios are normalized across these six, so content
     * below 20 Hz (inaudible rumble and DC) and above 20 kHz is excluded from the split rather
     * than silently folded into an edge band.
     */
    inline constexpr int32 NumBands = 6;
    inline constexpr double BandEdgesHz[NumBands + 1] = { 20.0, 150.0, 500.0, 1500.0, 4000.0, 8000.0, 20000.0 };
}

// ---------------------------------------------------------------------------------------------
// Presence of one metric family.
//
// The two setters are the only writers, which is what makes "absent" and "absent for a stated
// reason" the same state (rpc-design.md §2 - prefer a structural guarantee to a rule someone has
// to remember). A default-constructed state is unmeasured, so a family a code path forgot to
// fill cannot be serialized as a measurement.
// ---------------------------------------------------------------------------------------------
struct FPwAudioFamilyState
{
    bool bMeasured = false;

    /** Why the family is absent. Meaningful only while bMeasured is false; cleared when measured. */
    FString Reason;

    void MarkMeasured()
    {
        bMeasured = true;
        Reason.Reset();
    }

    void MarkUnmeasured(FString&& InReason)
    {
        bMeasured = false;
        Reason = MoveTemp(InReason);
    }
};

// ---------------------------------------------------------------------------------------------
// Technical: the facts that hold for any buffer with samples in it, including a silent one.
// This is the family that NAMES a degenerate buffer, so it is measured whenever PwAnalyzeBuffer
// returns true - and on a silent buffer it is the ONLY measured family.
// ---------------------------------------------------------------------------------------------
struct FPwAudioTechnical
{
    FPwAudioFamilyState State;

    /**
     * Always 0 on a successful analysis: a buffer carrying any non-finite sample is rejected
     * outright, because one NaN poisons every sum downstream of it. The field exists so the
     * report positively states that the check ran and found none, rather than leaving its
     * absence to be inferred.
     */
    int32 NonFiniteSamples = 0;

    /** Peak absolute sample is below PwAudioAnalysisLimits::SilencePeak - there is no signal. */
    bool bDigitalSilence = false;

    double DurationMs = 0.0;
    int32 SampleRate = 0;
    int32 NumFrames = 0;

    /** Linear peak across both channels. 0.0 here is a true measurement (see bDigitalSilence). */
    double PeakLinear = 0.0;

    /** dBFS forms. Unset exactly when the corresponding linear value is zero, i.e. on silence. */
    TOptional<double> PeakDb;
    TOptional<double> RmsDb;

    /** Samples at or beyond PwAudioAnalysisLimits::ClipThreshold, counted per channel-sample. */
    int32 ClippedSamples = 0;

    /** Mean sample value across both channels. SIGNED (§6): +0.2 and -0.2 are different faults. */
    double DcOffset = 0.0;

    /** Sign changes per second in the mono downmix. */
    double ZeroCrossingRate = 0.0;

    /**
     * Time spent above the envelope floor (EnvelopeFloorDb below the loudest 10 ms block).
     * Unset when the buffer is shorter than one whole envelope block, which is the only case
     * where it cannot be measured at all.
     */
    TOptional<double> ActiveDurationMs;

    /**
     * Absolute value of the first and last mono sample, linear 0..1. A buffer that starts or
     * ends away from zero clicks on playback and on loop. Reported linear rather than in dB so
     * "starts exactly at zero" is the honest 0.0 instead of a floored -120 dB.
     */
    double StartDiscontinuity = 0.0;
    double EndDiscontinuity = 0.0;
};

// ---------------------------------------------------------------------------------------------
// Envelope: the shape of the sound in time, measured on the 10 ms RMS block envelope.
//
// The three timing spans are defined against ONE floor so they compose:
//   OnsetMs   - start of the buffer to the first block above the floor (the leading silence).
//   AttackMs  - that first block to the loudest block. Measured from the onset, NOT from sample
//               0, so a layer with 200 ms of lead-in does not report a 200 ms attack; the
//               lead-in is OnsetMs, and OnsetMs + AttackMs is "start to peak".
//   DecayMs   - loudest block to the first block back below the floor. Unset when the sound
//               never gets that quiet, which is the honest answer for a sustained tone.
//   TailMs    - a SUBSET of DecayMs, not an addition to it: the part of the decay spent below
//               EnvelopeTailDb. It separates a plucked string (short body, long ring-out) from
//               a uniformly decaying pad, which share a DecayMs.
// ---------------------------------------------------------------------------------------------
struct FPwAudioEnvelope
{
    FPwAudioFamilyState State;

    double OnsetMs = 0.0;
    double AttackMs = 0.0;
    TOptional<double> DecayMs;
    TOptional<double> TailMs;

    /** Energy-weighted mean time. Low = front-loaded hit, ~half the duration = steady, high = swell. */
    double TemporalCentroidMs = 0.0;

    /** Peak minus RMS, in dB. 3.01 for a sine, ~0 for a square, very high for an impulse. */
    double CrestDb = 0.0;

    /**
     * Onsets found by PwDetectOnsets. Unset (rather than 0) when onset detection could not run,
     * which happens exactly when the spectral family could not be measured - they share an STFT.
     */
    TOptional<int32> TransientCount;

    /** Full-detail only. Empty whenever TransientCount is unset. */
    TArray<FPwOnset> Onsets;

    /** Why TransientCount is unset, when the rest of the family was measured. */
    FString OnsetReason;
};

// ---------------------------------------------------------------------------------------------
// Loudness: the four gated BS.1770 values, straight from PwComputeLoudness.
//
// PeakDb and RmsDb are deliberately NOT republished from FPwLoudnessResult even though it
// carries them. They are measured directly in the Technical family instead, so that one fact has
// one writer (rpc-design.md §2) and so crest factor survives a loudness-back-end failure. A
// report that carried two independently computed RMS figures could show them disagreeing.
// ---------------------------------------------------------------------------------------------
struct FPwAudioLoudness
{
    FPwAudioFamilyState State;

    double IntegratedLufs = 0.0;
    double ShortTermMaxLufs = 0.0;
    double MomentaryMaxLufs = 0.0;

    /** Mirrors FPwLoudnessResult::bLoudnessRangeMeasured - the range can be absent while the
        rest of the family is measured, and the JSON omits the field rather than emitting 0. */
    bool bLoudnessRangeMeasured = false;
    double LoudnessRangeLu = 0.0;
};

/** One of the six reported energy bands. Edges travel with the ratio so a reader never has to guess. */
struct FPwAudioBandRatio
{
    double LowHz = 0.0;
    double HighHz = 0.0;

    /** Share of the 20 Hz - 20 kHz power in this band, 0..1. The reported bands sum to 1. */
    double Ratio = 0.0;
};

struct FPwAudioSpectralPeak
{
    double Hz = 0.0;
    double Db = 0.0;
};

// ---------------------------------------------------------------------------------------------
// Spectral: everything computed from the STFT, time-averaged.
//
// The scalars are measured on the MEAN magnitude spectrum across frames rather than as the mean
// of per-frame scalars. Averaging first is what makes the noise case converge - a single-frame
// periodogram of white noise has exponentially distributed bins and a flatness near 0.56, which
// would read as "half tonal" for a signal that is definitionally flat.
//
// DC and Nyquist are excluded from EVERY spectral metric. They have no folding partner, so their
// magnitudes sit on a different scale than the interior bins, and DC would additionally let a
// plain DC offset dominate the centroid.
// ---------------------------------------------------------------------------------------------
struct FPwAudioSpectral
{
    FPwAudioFamilyState State;

    /** Magnitude-weighted mean frequency. The usual proxy for perceived brightness. */
    double CentroidHz = 0.0;

    /** Frequency below which RolloffFraction of the cumulative magnitude sits. */
    double RolloffHz = 0.0;

    /** Geometric mean / arithmetic mean of the POWER spectrum. 0 = pure tone, 1 = white noise. */
    double Flatness = 0.0;

    /** Magnitude-weighted standard deviation about the centroid: how spread out the energy is. */
    double BandwidthHz = 0.0;

    /**
     * Six-band power split. A band whose LOW edge is at or above Nyquist is OMITTED rather than
     * reported as 0.0 - at 8 kHz there is no 8k-20k band to be empty, and a zero would read as
     * "you have no top end" instead of "this rate cannot carry one". Empty altogether when no
     * energy at all lands inside 20 Hz - 20 kHz, for the same reason: six zeros would describe a
     * spectrum, and there is none to describe.
     */
    TArray<FPwAudioBandRatio> Bands;

    /** Nyquist of the analysed signal, so a reader can see which bands were truncated. */
    double NyquistHz = 0.0;

    /**
     * Spectral flux per frame: the mean per-bin magnitude INCREASE from the previous frame,
     * half-wave rectified. Index f of the array is the flux from frame f to frame f+1, so it has
     * NumFrames-1 entries. Unset when the analysis produced a single frame.
     */
    TOptional<double> FluxMean;
    TOptional<double> FluxMax;

    /** Full-detail only, decimated to MaxSeriesPoints by taking the MAX of each bucket. */
    TArray<float> Flux;

    /** Strongest peaks of the mean spectrum, strongest first. See MinPeakSeparationBins. */
    TArray<FPwAudioSpectralPeak> Peaks;

    int32 FftSize = 0;
    int32 HopSize = 0;
    int32 NumFrames = 0;
    double HopMs = 0.0;
};

// ---------------------------------------------------------------------------------------------
// Pitch. State.bMeasured is false whenever the estimator's confidence is below
// PwAudioAnalysisLimits::MinPitchConfidence, and the fields are cleared in that case - a
// suppressed pitch leaves no zeros behind for a careless reader to pick up. The confidence that
// caused the suppression is quoted in State.Reason, which is where it belongs: it explains an
// absence, it is not a measurement of the signal's pitch.
// ---------------------------------------------------------------------------------------------
struct FPwAudioPitch
{
    FPwAudioFamilyState State;

    double F0Hz = 0.0;
    double Confidence = 0.0;

    /** Motion class as PwEstimatePitch classified it. Passed through verbatim - it owns the vocabulary. */
    FString Motion;

    /**
     * Signed semitone difference between the end and the start of the confident part of the
     * track (§6). Positive is rising. This is what makes the two directions score differently as
     * NUMBERS rather than only as prose, and it says how far, which the class cannot. Unset when
     * the track carries too few confident points to compare two ends.
     */
    TOptional<double> SemitoneDelta;

    /** Full-detail only, decimated to MaxSeriesPoints by stride. */
    TArray<FPwPitchPointResult> Track;
};

// ---------------------------------------------------------------------------------------------
// Stereo image.
// ---------------------------------------------------------------------------------------------
struct FPwAudioStereo
{
    FPwAudioFamilyState State;

    /**
     * Zero-mean Pearson correlation of L and R, -1..+1. Unset when one channel is digitally
     * silent while the other is not: the correlation of anything with nothing is 0/0, and
     * emitting 0.0 there would read as "decorrelated wide image" for a signal that is in fact
     * hard-panned. bOneChannelSilent / SilentChannel name that case instead.
     */
    TOptional<double> Correlation;

    /** SideRms / (MidRms + SideRms). 0 = mono, 0.5 = hard-panned, 1 = fully out of phase. */
    double Width = 0.0;

    /**
     * Rms((L+R)/2) / sqrt((RmsL^2 + RmsR^2)/2) - the share of the level that survives a mono
     * fold-down. 1.0 = nothing lost, 0.707 = a hard pan, 0.0 = total cancellation. Linear rather
     * than dB precisely so full cancellation is an exact 0.0 instead of a floored -60.
     */
    double MonoCompatibility = 0.0;

    bool bOneChannelSilent = false;

    /** "left" or "right" - which side is silent. Empty unless bOneChannelSilent. */
    FString SilentChannel;
};

// ---------------------------------------------------------------------------------------------
// The whole report. Default-constructed = nothing measured, which is exactly the state
// PwAnalyzeBuffer leaves behind on every failure path.
// ---------------------------------------------------------------------------------------------
struct FPwAudioAnalysis
{
    FPwAudioTechnical Technical;
    FPwAudioEnvelope Envelope;
    FPwAudioLoudness Loudness;
    FPwAudioSpectral Spectral;
    FPwAudioPitch Pitch;
    FPwAudioStereo Stereo;

    // -----------------------------------------------------------------------------------------
    // OPT-IN context families (AudioGen/PwAudioContext.h). PwAnalyzeBuffer never fills these -
    // PwAnalyzeAudioContext does, and only for the families the caller asked for. Both of them
    // synthesize a new signal and re-measure it, so they cost multiples of the descriptor pass
    // and are default-off rather than merely cheap-to-skip.
    //
    // TOptional is the presence mechanism because the state space has THREE values, not two:
    //   unset            nobody asked - omitted from the JSON entirely, listed nowhere
    //   set, unmeasured  asked and failed - listed under `unmeasured` with its reason
    //   set, measured    published under its own key
    // A bare bMeasured flag would collapse the first two, and "you did not ask for this" is a
    // different fact from "this could not be measured" (rpc-design.md §1).
    // -----------------------------------------------------------------------------------------
    TOptional<FPwRetriggerResult> Retrigger;
    TOptional<FPwPlaybackConditionSet> PlaybackConditions;
};

/**
 * Measure a buffer.
 *
 * Failure is the default: `Out` is reset on entry, so every early return leaves a report with
 * every family unmeasured, and a caller that ignored the bool cannot read a stale number.
 *
 * Degenerate cases are detected and ANSWERED first, in this order (rpc-design.md §7):
 *   1. zero frames                  -> false, AUDIO_EMPTY_BUFFER
 *   2. broken buffer / bad rate     -> false, INVALID_PARAMS (the spelling PwComputeLoudness
 *                                     already uses for the same two conditions, so an agent sees
 *                                     one code for one fault whichever entry point rejected it)
 *   3. any non-finite sample        -> false, AUDIO_NON_FINITE_SAMPLES
 *   4. digital silence              -> TRUE, Technical only, every other family absent
 * Step 3 must precede step 4: NaN compares false against every threshold, so a NaN-filled buffer
 * checked for silence first would be measured as silence and reported as valid audio.
 *
 * Silence is a SUCCESS, not an error: "this rendered, and what it rendered is nothing" is a
 * measurement the agent needs, and it is a different fact from "this could not be analysed".
 *
 * Below the degenerate cases, each family is measured independently and a family that fails only
 * marks itself unmeasured with a reason - a loudness back-end that cannot run does not take the
 * spectral numbers down with it.
 *
 * @param OutErrorCode  An ErrorCodes::ERR_* value, so a handler can forward it unmodified.
 *                      Cleared on success.
 * @param OutError      Human-readable message naming the measured quantity that failed.
 * @return true when Out carries a usable report (including the silence report).
 */
bool PwAnalyzeBuffer(const FPwAudioBuffer& In, FPwAudioAnalysis& Out,
                     FString& OutErrorCode, FString& OutError);

/**
 * JSON for an RPC response.
 *
 * A family whose state is unmeasured is OMITTED, and its reason is listed under the top-level
 * `unmeasured` object, so absence is always explicit and always explained. A report with nothing
 * measured therefore serializes to `{"unmeasured":{...six entries...}}` rather than to a
 * plausible-looking wall of zeros.
 *
 * SIZE. The wrapped MCP ToolResult carries the payload twice - escaped in `content[0].text` and
 * verbatim in `structuredContent` - for roughly 2.35x amplification against the spill gate, so
 * the real ceiling for a bare result is about 4,250 characters rather than the 10,000 the
 * threshold constant suggests (board ticket E-spill-threshold-measured-post-wrap).
 *   bFullDetail == false  the summary form. Scalars only, no per-frame arrays, at most
 *                         MaxSummaryPeaks peaks. Roughly 1.6 KB on a rich signal as the transport
 *                         encodes it - JsonRpc::Serialize takes the default TJsonWriterFactory<>,
 *                         i.e. the PRETTY policy, which costs about 20% over a condensed encoding
 *                         in newlines, indents and post-colon spaces. That leaves the budget room
 *                         for whatever the handler wraps around this object.
 *   bFullDetail == true   adds the onset list, the pitch track, the flux series (each decimated
 *                         to MaxSeriesPoints and flagged when decimated), MaxDetailPeaks peaks,
 *                         and the analysis parameters. This form may exceed the ceiling and
 *                         spill; that is the deliberate trade for a caller that asked for it.
 *
 * Numbers are rounded to a per-quantity precision before emission. Two reasons, both real: the
 * report must not publish sixteen digits of a centroid estimate that is good to three, and UE's
 * JSON writer prints doubles with "%.17g", so an unrounded value can cost twenty characters of a
 * response budget measured in thousands.
 */
TSharedPtr<FJsonObject> SerializeAudioAnalysis(const FPwAudioAnalysis& In, bool bFullDetail);

/**
 * The bridge to the recipe's `targets` vocabulary: resolves one EPwSynthMetric against the
 * report. Returns false and leaves OutValue untouched when the metric's family was not measured
 * or the metric itself is undefined for this signal - so a target on a metric this buffer cannot
 * express is scored as UNSCORED rather than as a failure against 0 (rpc-design.md §1).
 *
 * Existing as a function rather than as a convention is the point: the scorer and the serializer
 * read the same accessor, so `centroidHz` in a target can never mean something different from
 * `centroidHz` in the report.
 */
bool PwGetAudioAnalysisMetric(const FPwAudioAnalysis& In, EPwSynthMetric Metric, double& OutValue);
