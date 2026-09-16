// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwStft.h"

// The three feature extractors the descriptor metrics are built on: BS.1770 loudness, spectral
// -flux onset detection, and YIN pitch estimation. Each one is an offline, side-effect-free
// function over a buffer the caller already owns; none of them touch the editor, the audio device
// or any global state, so they are callable from a test as directly as from a handler.
//
// Failure is the default everywhere (rpc-design.md §1): every result struct's bMeasured starts
// false and is set only on the one path that completed a real measurement, and every entry point
// clears its out-parameters before the first check so a caller that ignores the bool cannot read
// a stale or half-filled struct. The checks are ordered existence-first (rpc-design.md §7): an
// empty buffer and a digitally-silent buffer are named as such BEFORE any length, consistency or
// engine-primitive failure, because "there was no signal" reported as "the analyzer failed" - or,
// worse, as a loudness of -inf or a pitch read off the noise floor - points the caller at the
// wrong repair.
//
// ENGINE VERSION: PwComputeLoudness uses Audio::FLKFSAnalyzer (DSP/LKFSAnalyzer.h), which is
// 5.8-only - see the row in docs/engine-version-support.md. PwDetectOnsets and PwEstimatePitch
// deliberately are not built on the engine's FOnsetStrengthAnalyzer / FYINPitchDetector: those
// live in the AudioSynesthesia plugin, which is off by default, so depending on them would make
// two of the three extractors unavailable on a default host. FBlockCorrelator, which
// PwEstimatePitch does use, is core SignalProcessing and is present unchanged on 5.3-5.8.

/**
 * ITU-R BS.1770 loudness of a whole buffer, plus the two plain level measures.
 *
 * All six numbers are decibel-domain and only meaningful when bMeasured is true; a
 * default-constructed result is "not measured", never "measured as zero". Zero is a legal
 * loudness (0 LUFS is a very loud clip) so the flag, not the value, is the presence test.
 */
struct FPwLoudnessResult
{
    /** True only after FLKFSAnalyzer returned at least one result AND peak/RMS were computed. */
    bool bMeasured = false;

    /**
     * Gated integrated loudness over the whole buffer, LUFS. This is the gated figure (the
     * -70 LU absolute gate followed by the -10 LU relative gate), which is what "Integrated
     * LUFS" means in BS.1770 and in every delivery spec; the analyzer's ungated running average
     * is used only as a fallback when gating leaves nothing above the threshold.
     */
    double IntegratedLufs = 0;

    /** Loudest 3-second short-term window, LUFS. */
    double ShortTermMaxLufs = 0;

    /** Loudest 400 ms momentary window, LUFS. */
    double MomentaryMaxLufs = 0;

    /**
     * True only when FLKFSAnalyzer actually produced a gated short-term distribution to take a
     * range from. Read it before LoudnessRangeLu: a steady-level clip genuinely measures ~0 LU
     * and a clip too short to accumulate the distribution would also leave the field at 0, so
     * the value alone cannot tell "no spread" apart from "not measured" (rpc-design.md §1).
     * The rest of the struct can still be measured when this is false - a clip long enough for
     * an integrated loudness is not necessarily long enough for a range.
     */
    bool bLoudnessRangeMeasured = false;

    /**
     * Loudness range (EBU Tech 3342), LU: the 95th minus the 10th percentile of the gated
     * short-term distribution. Only meaningful when bLoudnessRangeMeasured is true.
     */
    double LoudnessRangeLu = 0;

    /** Largest absolute sample across both channels, dBFS. -200 floor rather than -inf. */
    double PeakDb = 0;

    /**
     * RMS across both channels over the whole buffer, dBFS. Reference: a full-scale sine in
     * both channels reads -3.01 dBFS, not 0 - this is an RMS, not a peak.
     */
    double RmsDb = 0;
};

/** One detected transient. */
struct FPwOnset
{
    /** Onset position in milliseconds from the start of the analysed signal. */
    double TimeMs = 0;

    /**
     * Spectral flux at the peak, normalized so the strongest onset in the buffer is exactly 1.0.
     * Relative within one call only - it says which of these onsets is the biggest, not how loud
     * any of them is.
     */
    double Strength = 0;
};

/** One pitch-track sample: the estimate from a single analysis window. */
struct FPwPitchPointResult
{
    /** Centre of the analysis window, milliseconds from the start of the signal. */
    double TimeMs = 0;

    /** Fundamental in Hz, parabolically interpolated between lags. Always within [50, 2000]. */
    double F0Hz = 0;

    /** 1 - the YIN cumulative-mean-normalized difference at the chosen lag, clamped to [0, 1]. */
    double Confidence = 0;
};

/**
 * Pitch of a whole buffer: a per-window track, an aggregate, and a direction.
 *
 * The aggregate is deliberately conservative. MedianF0Hz is left at 0 - "measured, not pitched" -
 * whenever Confidence lands below PwPitchConfidenceFloor, because a plausible-looking frequency
 * printed for a noise burst is worse than no frequency at all. Track still carries the raw
 * per-window estimates in that case, so a caller that wants to see what was rejected can.
 */
struct FPwPitchResult
{
    /** True once the estimator ran to completion, even if it found nothing pitched. */
    bool bMeasured = false;

    /** Median f0 over the confident windows, Hz. 0 means "measured, no pitch" (see above). */
    double MedianF0Hz = 0;

    /**
     * Median of the per-window confidences over the WHOLE track, including the unpitched
     * windows. It answers "how consistently is this clip pitched", not "how good was its best
     * moment" - a clip that is 90% noise scores low even if one window locked perfectly.
     */
    double Confidence = 0;

    /** "rising" | "falling" | "stable" | "unknown". See PwEstimatePitch for the thresholds. */
    FString Motion;

    /** Per-window estimates in time order. Windows too quiet to analyse are omitted, not zeroed. */
    TArray<FPwPitchPointResult> Track;
};

/**
 * Confidence at or above which a pitch estimate is worth reporting.
 *
 * Exported rather than duplicated: the analysis layer suppresses pitch below a floor, and a
 * floor chosen independently there would drift from the one the estimator uses to pick the
 * windows that feed MedianF0Hz. 0.5 means the YIN difference function at the chosen lag came
 * down to half of its running mean. Measured reference points: a clean sine scores >0.95, a
 * moderate linear chirp >0.9, and uniform white noise scores ~0.13 (the best of ~940 candidate
 * lags in a 2048-sample block is only about three standard deviations below the flat expectation,
 * so noise cannot climb near 0.5 by chance).
 */
constexpr double PwPitchConfidenceFloor = 0.5;

/**
 * Measure BS.1770 loudness plus peak and RMS of a deinterleaved stereo buffer.
 *
 * The buffer is interleaved internally before it reaches Audio::FLKFSAnalyzer, which consumes
 * interleaved frames and deinterleaves them itself - feeding it the two planar channels back to
 * back would measure the left channel for the first half of a doubled timeline and the right for
 * the second, which reads as a plausible number rather than as an error.
 *
 * Channel weighting is BS.1770's: L and R both weight 1.0, so a dual-mono signal measures 3 LU
 * above the same signal as a single channel. That is the standard's behaviour, not a bug.
 *
 * Rejects, in this order: an empty buffer, a digitally-silent buffer (both ERR_AUDIO_EMPTY_BUFFER),
 * mismatched channel lengths, a non-positive sample rate, and a buffer too short for the analyzer
 * to produce even one result (all ERR_INVALID_PARAMS, message naming the measured value and the
 * requirement). ERR_INTERNAL_ERROR is reserved for the analyzer returning nothing from a buffer
 * that was long enough.
 *
 * @return true only when Out.bMeasured is true.
 */
bool PwComputeLoudness(const FPwAudioBuffer& In, FPwLoudnessResult& Out,
                       FString& OutErrorCode, FString& OutError);

/**
 * Detect transients in a magnitude spectrogram by half-wave-rectified spectral flux with
 * adaptive-median peak picking.
 *
 * SampleRate must be the rate the spectrogram was computed at; it is cross-checked against
 * Stft.BinHz * Stft.FftSize and a mismatch is rejected rather than silently producing times on
 * the wrong clock.
 *
 * Zero onsets is a success, not a failure: a sustained tone genuinely contains none, and the
 * detector is tuned so that it reports none rather than manufacturing one (rpc-design.md §6 -
 * the false-positive direction is the one that costs the caller). A spectrogram with fewer than
 * two frames is rejected instead, because "no inter-frame difference exists" is not the same
 * claim as "there were no onsets".
 *
 * @return true when the detection ran; Out may legitimately be empty.
 */
bool PwDetectOnsets(const FPwStftResult& Stft, int32 SampleRate, TArray<FPwOnset>& Out,
                    FString& OutErrorCode, FString& OutError);

/**
 * Estimate the fundamental frequency track of a mono signal with YIN over Audio::FBlockCorrelator.
 *
 * Search range is 50-2000 Hz. Below 50 Hz the lag exceeds half the analysis block and the
 * difference function is estimated from too few overlapping samples to trust; above 2000 Hz the
 * lag is under 24 samples at 48 kHz and the parabolic interpolation between integer lags is
 * coarser than the harmonic structure it is trying to resolve. A caller that needs a whistle
 * above 2 kHz should read the spectral peak instead.
 *
 * Motion is fitted over the confident windows only, on log2(f0) against time: a total change of
 * less than one semitone (1/12 octave) across the confident span is "stable", otherwise "rising"
 * or "falling" by the sign of the fitted slope. Fitting the signed slope rather than a magnitude
 * is what makes a chirp and its reverse distinguishable (rpc-design.md §6). Fewer than four
 * confident windows, or an overall confidence below PwPitchConfidenceFloor, gives "unknown".
 *
 * @return true when the estimator ran; a completed run that found nothing pitched still returns
 *         true with bMeasured set, MedianF0Hz 0 and Motion "unknown".
 */
bool PwEstimatePitch(TArrayView<const float> Mono, int32 SampleRate, FPwPitchResult& Out,
                     FString& OutErrorCode, FString& OutError);
