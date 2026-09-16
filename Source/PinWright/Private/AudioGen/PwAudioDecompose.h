// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioFeatures.h"   // FPwOnset - the seeds PwDetectTransients characterises
#include "AudioGen/PwStft.h"

class FJsonObject;

// PwAudioDecompose.h - taking a recorded sound apart into the three layers the synthesizer can
// put back together.
//
// The descriptor metrics in PwAudioAnalysis.h answer "is this too bright?". They cannot answer
// "make it resemble THIS sound", because a centroid and a decay time do not tell an agent which
// knobs to turn. Decomposition answers that, and the model is chosen to be SYMMETRIC WITH THE
// SYNTHESIZER rather than to be the most faithful analysis available:
//
//   Transients (FPwTransient)      -> the noise / impulse generator (`noise`, modal `exciter`)
//   Partials  (FPwPartialTrack)    -> the modal resonator bank, one track per mode: MeanFreqHz
//                                     feeds `modeFreqsHz`, the fitted FPwModalFit's InitialGainDb
//                                     and DecayMs feed `modeGainsDb` and `modeDecaysMs`
//   Residual  (FPwResidualBand)    -> the filtered-noise layer, one log-spaced band per row
//
// That symmetry is the whole point: a decomposition of a reference sound is one mapping step away
// from a draft FPwSynthRecipe. A decomposition that produced, say, per-instrument stems would be
// a better analysis and a useless one, because nothing downstream could consume it.
//
// HONESTY RULES THIS FILE IS BUILT AROUND (docs/rpc-design.md):
//
//   §1  bMeasured defaults false everywhere and is set only on the path that completed a real
//       measurement. A layer that could not be computed is ABSENT, not empty-and-confident:
//       an empty Partials array beside bMeasured=true is the claim "this sound contains no
//       sinusoidal partials", which is a very different statement from "partial tracking did
//       not run". Every entry point clears its out-parameter before the first check, so a
//       caller that ignores the returned bool cannot read a stale or half-filled result.
//
//   §7  Degenerate cases are ordered so zero cannot be reported as a small number, and each is
//       ANSWERED where it is detected rather than left to fall through into a threshold test.
//       The order is fixed in every entry point here:
//           empty            -> AUDIO_EMPTY_BUFFER
//           non-finite       -> AUDIO_NON_FINITE_SAMPLES
//           digital silence  -> AUDIO_EMPTY_BUFFER (named as silence in the message)
//           malformed        -> INVALID_PARAMS
//           over the cost cap-> INVALID_PARAMS naming the limit
//       NON-FINITE BEFORE SILENCE IS LOAD-BEARING, and it is the trap a sibling already hit:
//       FMath::Max(0.0, NaN) returns 0 because NaN >= A is false for every A, so a NaN-filled
//       spectrogram scanned for its peak magnitude first measures as digital silence and gets
//       reported as "there was no signal". The scan for non-finite values therefore runs before
//       anything that compares a magnitude against a threshold.
//
//   Cost is refused, never silently trimmed. MaxDurationMs is an error naming the limit and the
//   measured duration; cropping a 40 s input to 30 s and reporting the result as a decomposition
//   of the input would be a measurement of a signal the caller never passed.
//
// ENGINE VERSION: nothing in this file or its implementation reaches past CoreMinimal / FMath /
// TArray, so it owes no row in docs/engine-version-support.md. The STFT it consumes is likewise
// portable (see PwStft.h); only the LUFS numbers elsewhere in AudioGen are 5.8-only.

/** One point on a piecewise-linear decibel envelope. */
struct FPwDbPoint
{
    /** Position of the point, milliseconds from the start of the analysed signal. */
    double TimeMs = 0.0;

    /** Level at that point, dBFS under the STFT magnitude convention (see PwStft.h). */
    double ValueDb = 0.0;
};

/**
 * One characterised transient: an onset from PwDetectOnsets plus the shape of the burst around it.
 *
 * The window is bounded on both sides by a level test relative to the burst's own peak, so
 * StartMs/EndMs describe the transient rather than the note it started - a struck string's
 * transient is its attack, not its whole ring-out.
 */
struct FPwTransient
{
    /** First frame of the transient window, ms from the start of the signal. */
    double StartMs = 0.0;

    /** Frame of maximum broadband energy inside the window, ms. */
    double PeakMs = 0.0;

    /** Last frame of the transient window, ms. EndMs >= StartMs always. */
    double EndMs = 0.0;

    /** Power-weighted mean frequency over the whole window, Hz. */
    double CentroidHz = 0.0;

    /** Power-weighted standard deviation about CentroidHz over the window, Hz. */
    double BandwidthHz = 0.0;

    /** Frequency below which 5% of the window's energy lies, Hz. A percentile, not a support edge. */
    double LowHz = 0.0;

    /** Frequency below which 95% of the window's energy lies, Hz. A percentile, not a support edge. */
    double HighHz = 0.0;

    /** Loudest single bin anywhere in the window, dBFS under the STFT magnitude convention. */
    double PeakDb = 0.0;
};

/** One measurement of one partial in one analysis frame. */
struct FPwPartialPoint
{
    /** Centre of the analysis frame the peak was found in, ms from the start of the signal. */
    double TimeMs = 0.0;

    /** Parabolically interpolated peak frequency, Hz. Sub-bin: not quantized to a bin centre. */
    double FreqHz = 0.0;

    /**
     * Interpolated peak amplitude, linear and referenced to full scale under the STFT convention
     * in PwStft.h - a 0 dBFS sinusoid on a bin centre reads 1.0, so 20*log10(AmpLinear) is dBFS.
     */
    double AmpLinear = 0.0;
};

/**
 * One sinusoidal partial followed across frames.
 *
 * Points may be non-contiguous in frame index: a partial briefly masked by a transient survives
 * a short gap rather than being split into two tracks, and no point is invented to fill the gap.
 * Read the timeline off Points[i].TimeMs, never off the point index.
 */
struct FPwPartialTrack
{
    /** Per-frame measurements in ascending time order. Never empty on a returned track. */
    TArray<FPwPartialPoint> Points;

    /** Points[0].TimeMs, ms from the start of the signal. */
    double StartMs = 0.0;

    /** Points.Last().TimeMs, ms. EndMs - StartMs is the track's duration. */
    double EndMs = 0.0;

    /** Unweighted arithmetic mean of the point frequencies, Hz. The modal bank's mode frequency. */
    double MeanFreqHz = 0.0;

    /** Largest AmpLinear over the points, linear full-scale-referenced amplitude. */
    double PeakAmpLinear = 0.0;

    /**
     * Total frequency change across the track, Hz.
     *
     * SIGNED, and signed deliberately (rpc-design.md §6): a magnitude cannot tell a rising sweep
     * from a falling one, and the two ask the synthesizer for opposite pitch envelopes.
     */
    double FreqDriftHz = 0.0;         // signed: end minus start
};

/**
 * An exponentially decaying mode fitted to one partial track - the modal resonator bank's row.
 *
 * bMeasured is false for a track whose amplitude trajectory does not support a fit (too few
 * points, no measurable decay, a rising envelope), because a decay time invented for a partial
 * that never decayed would be played back as an audible wrong answer.
 */
struct FPwModalFit
{
    /** Mode frequency, Hz. Maps to one entry of the modal generator's `modeFreqsHz`. */
    double FreqHz = 0.0;

    /** Fitted amplitude at the track's start, dBFS. Maps to `modeGainsDb`. */
    double InitialGainDb = 0.0;

    /** Fitted -60 dB decay time, ms. Maps to `modeDecaysMs`. */
    double DecayMs = 0.0;

    /** RMS residual of the exponential fit, dB. Small means the mode really is exponential. */
    double FitErrorDb = 0.0;          // RMS residual of the exponential fit

    /** False means no fit was possible; the three numbers above are then meaningless, not zero. */
    bool   bMeasured = false;
};

/** One log-spaced band of the residual (everything the partials did not explain). */
struct FPwResidualBand
{
    /** Lower edge of the band, Hz (inclusive). */
    double LowHz = 0.0;

    /** Upper edge of the band, Hz (exclusive, except for the top band which includes Nyquist). */
    double HighHz = 0.0;

    /** Band level over time, dBFS, after piecewise-linear simplification to SimplifyToleranceDb. */
    TArray<FPwDbPoint> EnvelopeDb;    // piecewise-linear simplified
};

/**
 * How the stereo image behaves over the sound's lifetime.
 *
 * bMeasured is false for a mono or dual-mono source: correlation 1.0 reported for a signal with
 * no stereo information at all would read as a deliberate narrow image rather than as an absence.
 */
struct FPwStereoBehaviour
{
    /** True only when both channels carried independent signal and both correlations were computed. */
    bool   bMeasured = false;

    /** L/R Pearson correlation over the opening window, -1..+1 (1 = mono, 0 = uncorrelated). */
    double InitialCorrelation = 0.0;

    /** L/R Pearson correlation over the closing window, -1..+1. */
    double TailCorrelation = 0.0;

    /** True when TailCorrelation is meaningfully below InitialCorrelation, i.e. the image opens up. */
    bool   bWidthIncreasesOverTime = false;
};

/** Cost and detail limits for one decomposition. Every field is a hard limit, never a hint. */
struct FPwDecomposeSettings
{
    /**
     * Longest input this verb will accept, ms. Exceeding it is an ERROR naming the limit and the
     * measured duration - not a silent crop, which would report a decomposition of a signal the
     * caller never passed. The default bounds the median-filter pass (see
     * PwSeparateHarmonicPercussive's cost note) at a few seconds of CPU.
     */
    int32  MaxDurationMs = 30000;

    /** Most partial tracks kept, count. Excess tracks are dropped LOUDEST-FIRST-KEPT and reported. */
    int32  MaxPartials = 64;

    /** Number of log-spaced bands the residual is summarised into, count. */
    int32  ResidualBands = 24;

    /** Tracks shorter than this are discarded as peak-picking noise, ms. */
    double PartialMinDurationMs = 20.0;

    /** Peak rejection threshold RELATIVE TO ITS OWN FRAME's maximum, dB (negative). */
    double PartialMinAmpDb = -60.0;

    /** Douglas-Peucker tolerance for residual envelope simplification, dB. */
    double SimplifyToleranceDb = 1.5;
};

/**
 * The whole decomposition.
 *
 * bMeasured false means nothing below it may be read: the arrays are cleared, not partially
 * filled, and UnmeasuredReason carries the human-readable why.
 */
struct FPwDecomposition
{
    /** True only after every stage the input supported completed. */
    bool bMeasured = false;

    /** Why the decomposition is absent. Meaningful only while bMeasured is false. */
    FString UnmeasuredReason;

    /** Analysed span of the source, ms. */
    double DurationMs = 0.0;

    /** Rate the source and the spectrogram were measured at, Hz. */
    int32 SampleRate = 0;

    /** Characterised transients in ascending StartMs order. Empty is a legal measurement. */
    TArray<FPwTransient>     Transients;

    /** Partial tracks, LOUDEST FIRST (see PwTrackPartials - the cap is applied on that order). */
    TArray<FPwPartialTrack>  Partials;

    /** One fit per kept partial track, index-aligned with Partials. */
    TArray<FPwModalFit>      Modes;

    /** Residual bands in ascending LowHz order. */
    TArray<FPwResidualBand>  Residual;

    /** Stereo image behaviour; its own bMeasured governs whether its numbers may be read. */
    FPwStereoBehaviour       Stereo;

    /** Fraction of total spectrogram energy in the harmonic component, 0..1. */
    double HarmonicEnergyRatio = 0.0;

    /** Fraction of total spectrogram energy in the percussive component, 0..1. */
    double PercussiveEnergyRatio = 0.0;

    /** Fraction of total energy left after the partials were subtracted, 0..1. */
    double ResidualEnergyRatio = 0.0;
};

// Yours (this chunk):

/**
 * Harmonic/percussive source separation by median filtering of the magnitude spectrogram
 * (Fitzgerald 2010), producing two magnitude spectrograms rather than two signals.
 *
 * WHAT IT SEPARATES, AND WHAT IT DOES NOT (rpc-design.md §1 - this bounds what the report may
 * claim): it separates ENERGY THAT IS HORIZONTAL IN THE SPECTROGRAM from ENERGY THAT IS VERTICAL.
 * It does NOT separate instruments, sources, or "the tonal part" from "the noise part". A cymbal
 * ringing steadily lands in the harmonic component; a bowed note's bow noise and its attack land
 * in the percussive one. Any wording downstream that says "harmonic instruments" or "the drum
 * track" is a claim this function did not measure.
 *
 * METHOD. For each bin, the median of the magnitudes across a window of FRAMES estimates the
 * harmonic (horizontal) component: a sustained partial occupies every frame in the window, so the
 * median keeps it, while a transient occupies a handful and is rejected as an outlier. For each
 * frame, the median across a window of BINS estimates the percussive (vertical) component by the
 * mirror argument. Windows are truncated at the array edges rather than zero-padded - padding
 * with zeros pulls the edge medians down and manufactures a percussive-looking onset at frame 0.
 *
 * KERNEL SIZES. 31 frames (time / harmonic) and 31 bins (frequency / percussive), both odd, both
 * fixed rather than caller-tunable, and both justified against the module's reference analysis
 * grid (FftSize 2048, HopSize 256, 48 kHz -> 5.33 ms per frame, 23.4 Hz per bin):
 *   - 31 frames = 165 ms. A transient's footprint in one bin lasts about one analysis window
 *     (42.7 ms, 8 frames), comfortably under half the kernel, so it cannot drag the median.
 *   - 31 bins = 727 Hz. A Hann mainlobe is 4 bins wide, so a sinusoid's footprint is far under
 *     half the kernel and is medianed away, while a transient's flat spectrum survives intact.
 * A spectrogram shorter or narrower than a kernel is handled by the same truncation as the edges.
 *
 * MASK TYPE. Soft Wiener masks with power 2 and no margin: Mh = H^2/(H^2+P^2), Mp = 1 - Mh, and
 * the outputs are S*Mh and S*Mp. Soft rather than binary because a binary mask assigns every
 * mixed bin wholly to one side and produces the classic musical-noise artefacts; power 2 rather
 * than 1 because the squared form is the Wiener solution for two independent sources. The masks
 * sum to exactly 1 per bin, so OutHarmonic[i] + OutPercussive[i] == In.Magnitudes[i] to within
 * float rounding and no energy is created or lost - a caller may therefore compute the harmonic
 * and percussive energy ratios directly off these arrays.
 *
 * COST: O(NumFrames * NumBins * K log K) with K = 31, run twice. That is the reason
 * FPwDecomposeSettings::MaxDurationMs exists.
 *
 * @param OutHarmonic    Cleared on entry. On success, NumFrames * NumBins magnitudes in the same
 *                       frame-major layout as In.Magnitudes.
 * @param OutPercussive  Cleared on entry, same layout.
 * @return true only when both outputs are fully populated. Rejects, in this order: an empty
 *         spectrogram (AUDIO_EMPTY_BUFFER), one holding non-finite magnitudes
 *         (AUDIO_NON_FINITE_SAMPLES), a digitally silent one (AUDIO_EMPTY_BUFFER), a malformed
 *         one, and one with fewer than 3 frames (both INVALID_PARAMS - with fewer than three
 *         frames there is no time axis to call anything horizontal, so "all of it is harmonic"
 *         would be an artefact of the window, not a measurement).
 */
bool PwSeparateHarmonicPercussive(const FPwStftResult& In, TArray<float>& OutHarmonic,
                                  TArray<float>& OutPercussive, FString& OutErrorCode, FString& OutError);

/**
 * Find spectral peaks per frame and connect them across frames into partial tracks
 * (McAulay-Quatieri sinusoidal-model continuation).
 *
 * PEAK PICKING. A bin is a candidate when it exceeds its lower neighbour and is at least its
 * upper neighbour (the tie-break makes a plateau report once, matching PwDetectOnsets). DC and
 * Nyquist can never be peaks - they have no neighbour on one side, so no parabola exists there.
 * Each candidate is refined by fitting a parabola through the LOG magnitudes (dB) of the three
 * bins around it and taking the vertex; the log domain is what buys the sub-bin precision,
 * because a windowed sinusoid's mainlobe is close to parabolic in dB and distinctly not parabolic
 * in linear magnitude. For a Hann window the residual bias of that estimator is under 0.02 bins,
 * against the 0.5-bin worst case of taking the bin centre - a 25x improvement that matters here
 * because the frequency is going straight into a modal resonator.
 * Candidates below Settings.PartialMinAmpDb relative to THEIR OWN FRAME's maximum are rejected,
 * so a decaying sound keeps yielding peaks after it has dropped below the loudest moment.
 *
 * CONTINUATION. Each frame's peaks are matched to the currently active tracks by nearest
 * frequency, greedily and globally: every (track, peak) pair inside tolerance is considered in
 * ascending |delta f| order and taken if neither side is already claimed. This is a
 * simplification of MQ's own two-pass conflict resolution and agrees with it whenever the peaks
 * are resolvable at all. Tolerance is max(2 bins, 3% of the track's current frequency):
 *   - The 2-bin floor covers estimator jitter plus a real glide of up to 2 bins per hop (8.8 kHz
 *     per second on the reference grid) while staying well inside the spacing of adjacent
 *     harmonics of anything above ~50 Hz.
 *   - The 3% relative term takes over above ~1.5 kHz, where a fixed 2-bin window is far tighter
 *     than real vibrato. 3% is about half a semitone per frame; a partial moving faster than that
 *     is a slide, not a steady mode. It also cannot reach a neighbouring harmonic for the first
 *     33 harmonics, since the k-th and (k+1)-th are 1/k apart in relative terms.
 * An unmatched peak births a track. A track with no match for longer than 20 ms (about four hops
 * on the reference grid, under half an analysis window) dies at its last matched frame; the gap
 * itself contributes no points, so a track's Points array can skip frames.
 *
 * FILTERING AND THE CAP. Tracks shorter than Settings.PartialMinDurationMs are discarded as
 * peak-picking noise. The survivors are sorted by PeakAmpLinear descending and the first
 * Settings.MaxPartials are kept, so Out is loudest-first and the cap is visible in the ordering.
 *
 * TRUNCATION IS REPORTED, NEVER SILENT (rpc-design.md §1). A list of 64 tracks returned for a
 * sound that produced 300 reads as "this sound has 64 partials". On a successful call that
 * dropped anything, this function returns true with OutErrorCode EMPTY and OutError carrying a
 * note naming the counts (short tracks dropped, tracks kept out of how many, the peak amplitude
 * of the loudest dropped track, and any per-frame peak-list clipping); it also logs the same note
 * as a warning on LogPinWrightSubsystem. A caller distinguishes the two states by the return
 * value together with OutErrorCode - a non-empty OutError beside an EMPTY OutErrorCode and a
 * true return is a note, not a failure. When nothing was dropped, OutError stays empty.
 *
 * To bound cost on noisy material, each frame keeps only its loudest peaks (4 * MaxPartials,
 * clamped to 32..512). A peak outside a frame's own loudest 4*MaxPartials cannot be part of a
 * top-MaxPartials track of the whole sound; the clipping is counted and included in the note.
 *
 * KNOWN LIMIT: analysis-window sidelobes are real local maxima and are picked as peaks whenever
 * they clear the relative threshold. A Hann window's first sidelobe is about 31 dB below the
 * mainlobe, so with the default PartialMinAmpDb of -60 a single loud tone contributes a handful
 * of spurious sidelobe tracks flanking the real one. They are quiet, so the loudest-first cap
 * usually buries them, and the fix is a threshold above the window's sidelobe level (-25 dB or
 * higher for Hann) rather than a heuristic here - suppressing "peaks near a louder peak" would
 * also delete the closely spaced real partials of an inharmonic body.
 *
 * @return true when tracking ran; Out may legitimately be empty (a pure noise burst has no
 *         partials that survive the duration filter). Rejects, in this order: an empty
 *         spectrogram, non-finite magnitudes, digital silence, a malformed spectrogram, a
 *         non-positive or spectrogram-contradicting SampleRate, an input longer than
 *         Settings.MaxDurationMs (message naming both the limit and the measurement), and
 *         nonsensical settings.
 */
bool PwTrackPartials(const FPwStftResult& In, int32 SampleRate, const FPwDecomposeSettings& Settings,
                     TArray<FPwPartialTrack>& Out, FString& OutErrorCode, FString& OutError);

/**
 * Characterise each onset from PwDetectOnsets as a transient: its extent, its spectral shape and
 * its level.
 *
 * The onsets are SEEDS, not the answer - this function does not detect anything, and it reports
 * exactly one transient per seed it could characterise, in ascending StartMs order. Passing an
 * empty onset list is a legal call that succeeds with an empty Out: "this sound has no
 * transients" is the correct answer for a sustained tone, and manufacturing one would be a false
 * positive in the direction that costs the caller most (rpc-design.md §6).
 *
 * EXTENT. Frame energy is the sum of squared magnitudes over the bins. The peak frame is the
 * loudest frame from the onset's own frame up to one analysis window later - the flux detector
 * reports a time that LEADS the true transient by about a quarter of a window (see
 * PwDetectOnsets), so searching forward by a whole window covers that lead with margin. Start and
 * end are then the first frames either side of the peak that fall 20 dB below it (1% of peak
 * power, the point past which the burst is no longer audible over what follows), bounded by the
 * neighbouring onsets so two transients cannot overlap, and by 250 ms so a note that sustains
 * after its attack does not turn its "transient" into the whole clip.
 *
 * SHAPE. Centroid and bandwidth are power-weighted over the bin spectrum accumulated across the
 * window. LowHz/HighHz are the 5% and 95% cumulative-power points of that same spectrum - they
 * are percentiles, so a transient with a little energy everywhere still reports a narrow range,
 * and they are NOT the edges of the transient's support. PeakDb is the loudest single bin in the
 * window in dBFS.
 *
 * @return true when characterisation ran; Out may legitimately be empty. Rejects, in this order:
 *         an empty spectrogram, non-finite magnitudes, digital silence, a malformed spectrogram,
 *         and a non-positive or spectrogram-contradicting SampleRate. An onset whose time falls
 *         outside the spectrogram is skipped rather than clamped onto the last frame, because a
 *         transient reported at the end of a clip that was actually past its end is a fabricated
 *         measurement; the skip count is logged.
 */
bool PwDetectTransients(const FPwStftResult& In, int32 SampleRate, const TArray<FPwOnset>& Onsets,
                        TArray<FPwTransient>& Out, FString& OutErrorCode, FString& OutError);

// Siblings implement these against the same header - declare them, do not define them:

/**
 * Fit one exponentially decaying mode per partial track.
 *
 * Out is index-aligned with Partials: a track that could not be fitted still produces an entry,
 * with bMeasured false, so the two arrays never drift apart.
 */
bool PwFitModes(const TArray<FPwPartialTrack>& Partials, int32 SampleRate,
                TArray<FPwModalFit>& Out, FString& OutErrorCode, FString& OutError);

/**
 * Summarise what the partials did not explain as Settings.ResidualBands log-spaced band
 * envelopes, each simplified to Settings.SimplifyToleranceDb.
 */
bool PwAnalyzeResidual(const FPwStftResult& Original, const TArray<FPwPartialTrack>& Partials,
                       int32 SampleRate, const FPwDecomposeSettings& Settings,
                       TArray<FPwResidualBand>& Out, FString& OutErrorCode, FString& OutError);

/**
 * Serialize a decomposition for an RPC response. An unmeasured decomposition, and an unmeasured
 * sub-struct inside a measured one, are OMITTED rather than emitted as zeros (rpc-design.md §1).
 * bFullDetail false emits the summaries only, without the per-frame point arrays.
 */
TSharedPtr<FJsonObject> SerializeDecomposition(const FPwDecomposition& In, bool bFullDetail);

/**
 * Run the whole decomposition over a buffer: STFT, HPSS, partial tracking, modal fitting,
 * residual bands, stereo behaviour.
 */
bool PwDecomposeBuffer(const FPwAudioBuffer& In, const FPwDecomposeSettings& Settings,
                       FPwDecomposition& Out, FString& OutErrorCode, FString& OutError);
