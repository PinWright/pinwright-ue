// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// PwAudioFeatures.h for FPwPitchPointResult (the overlay draws a pitch track by value, so a
// forward declaration will not do). ConstantQ.h for Audio::EPseudoConstantQNormalization, which is
// a real analysis choice the constant-Q view exposes - same shape as PwStft.h pulling
// DSP/AudioFFT.h in for Audio::EWindowType.
#include "AudioGen/PwAudioFeatures.h"
#include "DSP/ConstantQ.h"

struct FPwAudioBuffer;
struct FPwStftResult;

// PNG plotting for the audio-analysis verbs. These images are the LLM's only perceptual channel
// onto the audio, so three properties are non-negotiable and are enforced here rather than left
// to the caller:
//
//  1. FIXED SCALES. The dB range, the amplitude range and the frequency range are parameters with
//     fixed defaults, never fitted to the data. An auto-scaled pair of images is unreadable as a
//     comparison: a quieter take that got rescaled looks identical to the loud one, so a model
//     reports "no change" for a change of 40 dB. Two images produced with the same settings are
//     directly comparable pixel for pixel.
//  2. LABELLED AXES. Without tick labels a model can say "there is a bright horizontal line" but
//     not "there is a tone at 1 kHz starting at 250 ms", which is the entire point of the image.
//     Rendered with the built-in 5x7 bitmap glyph set below - no font asset, no Slate, no RHI.
//  3. A PERCEPTUALLY MONOTONIC COLORMAP (viridis). A rainbow ramp is non-monotonic in luminance,
//     so it invents banding a model reads as structure and hides real level ordering.
//
// Encoding goes through FImageUtils::PNGCompressImageArray, never the UE_DEPRECATED(5.1)
// FImageUtils::CompressImageArray - that one forwards to ThumbnailCompressImageArray, which picks
// png or jpg at its own discretion and would silently emit a lossy JPEG spectrogram.

/** Waveform plot options. Every field is fixed by default; nothing is fitted to the signal. */
struct FPwWaveformPlotSettings
{
    int32 Width = 1024;
    int32 Height = 256;

    /**
     * Fixed vertical half-range in sample units: the lane spans -AmplitudeRange..+AmplitudeRange.
     * 1.0 = full scale. Samples outside the range are clamped to the lane edge rather than
     * rescaling the plot.
     */
    float AmplitudeRange = 1.0f;
};

/** Spectrogram plot options. Every field is fixed by default; nothing is fitted to the spectrum. */
struct FPwSpectrogramPlotSettings
{
    int32 Width = 1024;
    int32 Height = 512;

    /** Fixed dB window. MinDb maps to the dark end of the colormap, MaxDb to the bright end. */
    float MinDb = -90.f;
    float MaxDb = 0.f;

    /** Bottom of a LOGARITHMIC frequency axis, in Hz. Ignored on a linear axis, which always starts at 0. */
    float MinHz = 20.f;

    /** Top of the frequency axis, in Hz. 0 means Nyquist (SampleRate / 2). */
    float MaxHz = 0.f;
};

/**
 * Constant-Q plot options. Same fixed-scale discipline as the spectrogram: the dB window and the
 * band layout are defaults, never fitted to the signal, so two constant-Q images are comparable.
 */
struct FPwConstantQPlotSettings
{
    int32 Width = 1024;
    int32 Height = 512;

    /** Fixed dB window, deliberately the same one the spectrogram uses so both read on one ramp. */
    float MinDb = -90.f;
    float MaxDb = 0.f;

    /**
     * Band layout. 85 bands at 12 per octave starting from C1 = 32.703 Hz covers C1..C8 inclusive -
     * seven octaves, which is where musical and most mechanical / UI sound lives. Bands whose
     * centre would land above Nyquist are DROPPED, not plotted: an unmeasurable band rendered at
     * the floor of the colormap reads as "measured, and silent", which is a different claim.
     */
    int32 NumBands = 85;
    float NumBandsPerOctave = 12.f;
    float LowestBandCenterHz = 32.703196f;

    /** Gaussian band-width multiplier for Audio::FPseudoConstantQ. 1.0 = adjacent bands just meet. */
    float BandWidthStretch = 1.f;

    /**
     * How each band's Gaussian weights are normalized.
     *
     * EqualAmplitude is the default, which is a deliberate departure from the engine header's
     * "EqualEuclideanNorm is good when using magnitude spectrum" note. Under EqualAmplitude the
     * weights peak at 1.0, so a band's value tracks the AMPLITUDE of a tone inside it instead of
     * being divided by the band's width, and a level-matched tone reads at the same brightness in
     * every octave. Under EqualEuclideanNorm the same sweep sags by roughly 7 dB from C1 to C8,
     * and a vision model reads that tilt as a high-harmonic rolloff that is not in the signal -
     * which defeats the one job this view has.
     *
     * The cost, stated because it is real: broadband noise reads BRIGHTER in the upper octaves,
     * where bands are wider and collect more FFT bins. Switch to EqualEnergy / EqualEuclideanNorm
     * when the subject is noise rather than tones.
     */
    Audio::EPseudoConstantQNormalization Normalization = Audio::EPseudoConstantQNormalization::EqualAmplitude;

    /**
     * The STFT the constant-Q kernel is applied to. 4096 at 48 kHz gives 11.7 Hz bins; a band
     * narrower than that (everything below roughly 200 Hz at 12 bands/octave) is FFT-resolution
     * limited, and Audio::FPseudoConstantQ falls back to interpolating the two nearest bins for it.
     * That limit is inherent to a PSEUDO constant-Q - it windows a DFT rather than running a filter
     * bank - and raising FftSize trades time resolution for it.
     */
    int32 FftSize = 4096;
    int32 HopSize = 512;
};

/**
 * Analysis marks drawn over a spectrogram. Every array is optional, and an EMPTY overlay produces
 * a byte-identical PNG to PwPlotSpectrogram - asserted in the tests, and what makes the overlay
 * entry point safe to call unconditionally.
 *
 * THE COLOURS ARE CHOSEN AGAINST THE VIRIDIS RAMP, not for looks. Viridis runs dark purple -> blue
 * -> teal -> green -> yellow. It never emits a red-dominant pixel (its warmest point, yellow
 * 253,231,37, has green as high as red), and never a pixel with high red AND high blue at once
 * (the purple end is 68,1,84, the yellow end has blue 37). So:
 *   - onsets  pure red      (255,  48,  48) - a channel ratio viridis cannot produce
 *   - pitch   magenta       (255,   0, 200) - high R and high B together, also clearly not the red
 *   - events  white         (255, 255, 255) - brighter than viridis's brightest
 * Another blue-green would be invisible as a mark: it lies inside the ramp, so a reader parses it
 * as a level, not as an annotation.
 *
 * Event bounds are BRACKETED, never shaded. An alpha wash over the spectrogram would change the
 * apparent dB inside the region, and the entire point of the fixed scale is that brightness means
 * level and nothing else.
 */
struct FPwPlotOverlay
{
    /** Vertical lines, in milliseconds from the start of the analysed signal. */
    TArray<double> OnsetTimesMs;

    /** Fundamental-frequency track drawn as a curve over the spectrogram. See MinPitchConfidence. */
    TArray<FPwPitchPointResult> PitchTrack;

    /** (StartMs, EndMs) pairs, drawn as brackets around the region. */
    TArray<FVector2D> EventBoundsMs;

    /**
     * Pitch points below this confidence are NOT DRAWN, and the curve BREAKS at the gap rather
     * than interpolating across it. A curve drawn through a guess is worse than no curve at all,
     * because the reader cannot tell the two apart and will believe it.
     *
     * The default is PwPitchConfidenceFloor, the SAME constant PwEstimatePitch uses to decide
     * which windows feed its median - taken rather than re-chosen, so the curve cannot show a
     * point the aggregate discarded (or hide one it kept). Its calibration is documented there:
     * a clean sine scores >0.95, white noise ~0.13.
     *
     * The exclusion is silent by design. "No confident pitch anywhere" is itself a finding worth
     * showing, so an all-low-confidence track plots the bare spectrogram instead of failing the
     * call; the image never asserts a pitch it did not measure, which is the property that matters.
     */
    double MinPitchConfidence = PwPitchConfidenceFloor;

    /**
     * Two kept points are joined by a line only when they were ADJACENT in PitchTrack and no more
     * than this far apart in time. Adjacency is the real rule - it is what breaks the curve at a
     * point the confidence floor dropped - and the millisecond cap covers the other gap the track
     * can contain: PwEstimatePitch OMITS windows too quiet to analyse rather than zeroing them, so
     * two array neighbours can straddle a silence with no confidence signal to give it away.
     * 50 ms against that estimator's ~10.7 ms hop bridges up to four skipped windows - a short
     * unvoiced dip inside one note - and breaks across anything longer.
     */
    double MaxPitchGapMs = 50.0;

    bool IsEmpty() const
    {
        return OnsetTimesMs.Num() == 0 && PitchTrack.Num() == 0 && EventBoundsMs.Num() == 0;
    }
};

/**
 * Render both channels of In as stacked peak-envelope lanes (left over right; a mono buffer gets
 * one lane) with an RMS-envelope band drawn over each, and write the result as a PNG.
 *
 * Failure is the default: on any failure the function returns false with OutError naming the
 * measured reason, and no file is left behind unless the write itself succeeded and the readback
 * failed (in which case OutError says so).
 *
 * @param AbsolutePngPath  Absolute path. A relative path is rejected rather than resolved against
 *                         the editor's working directory. Parent directories are created.
 * @param Settings         Optional override; nullptr uses the fixed defaults above.
 */
bool PwPlotWaveform(const FPwAudioBuffer& In, const FString& AbsolutePngPath, FString& OutError,
                    const FPwWaveformPlotSettings* Settings = nullptr);

/**
 * Render a magnitude spectrogram as a PNG: time across, frequency up, level as viridis colour on
 * the fixed dB scale, plus a labelled colour bar.
 *
 * The time axis is labelled by ANALYSIS-WINDOW START time (frame f starts at
 * f * HopSize / SampleRate); each column's window additionally extends FftSize / SampleRate
 * seconds to the right of its label. In must therefore come from PwComputeStft - a hand-built
 * FPwStftResult with no FftSize/HopSize is rejected rather than plotted against a made-up axis.
 *
 * Down-sampling to pixels uses MAX-pooling in both axes, not averaging: a single-bin tone in a
 * 1025-bin spectrum drawn into 456 rows survives max-pooling and is averaged into the floor by
 * the alternative, which would delete exactly the feature the image exists to show.
 *
 * @param bLogFrequency  true for a logarithmic frequency axis from Settings->MinHz, false for a
 *                       linear axis from 0 Hz. Both are provided; neither is assumed better.
 */
bool PwPlotSpectrogram(const FPwStftResult& In, int32 SampleRate, bool bLogFrequency,
                       const FString& AbsolutePngPath, FString& OutError,
                       const FPwSpectrogramPlotSettings* Settings = nullptr);

/**
 * Render a CONSTANT-Q magnitude plot of In as a PNG: time across, log-spaced pitch up, level as
 * viridis colour on the same fixed dB window the spectrogram uses.
 *
 * Why this exists beside PwPlotSpectrogram: on a linear frequency axis every octave above the
 * first lives in the top half of the image and the bottom four octaves are crushed into a handful
 * of rows, so harmonic structure - the thing that makes a sound read as a note, a chord, a motor
 * or a gear whine - is unreadable. Here each octave occupies the same number of pixels BY
 * CONSTRUCTION, so a harmonic stack is an evenly spaced ladder and a pitch change is a rigid
 * vertical translation. The frequency axis is therefore labelled in NOTE NAMES at octave
 * boundaries ("C4 262Hz"), never in linear ticks: evenly spaced ticks carrying names one octave
 * apart are the visual proof that the axis is what it claims to be.
 *
 * Both channels are averaged to mono first. The view is about pitch content, and two side-by-side
 * channel panels would halve exactly the vertical resolution the octave spacing needs.
 *
 * SCALE CONVENTION, stated because it is NOT the spectrogram's: a band's value is the
 * Gaussian-weighted sum of its magnitude bins, so for a band wide enough to contain a tone's whole
 * main lobe it comes out near 2x the tone's amplitude (+6 dB), and a full-scale tone saturates the
 * top of the fixed window. This is a BAND-LEVEL scale, not the per-bin dBFS of PwPlotSpectrogram.
 * It is fixed, so two constant-Q images stay directly comparable, which is the property the images
 * exist for; do not read an absolute dBFS off the colour bar of this one.
 *
 * Failure is the default: a bad setting, an unusable buffer, an STFT that does not run, or a
 * kernel that does not match the spectrum all return false with OutError naming the measured
 * reason, and no file is written.
 */
bool PwPlotConstantQ(const FPwAudioBuffer& In, const FString& AbsolutePngPath, FString& OutError,
                     const FPwConstantQPlotSettings* Settings = nullptr);

/**
 * PwPlotSpectrogram plus analysis marks. The base image comes from the SAME renderer as the plain
 * spectrogram - not a second copy of it - so the two entry points cannot drift, and an empty
 * Overlay yields a byte-identical PNG.
 *
 * Marks that cannot be placed honestly are DROPPED, not clamped: an onset past the end of the
 * analysed signal, a pitch point outside the plotted frequency range, an event that does not
 * intersect the time axis. A clamped mark would sit on the edge of the plot asserting an event at
 * a time or a pitch that was never measured, which is the failure mode this whole file exists to
 * avoid.
 */
bool PwPlotSpectrogramWithOverlay(const FPwStftResult& In, int32 SampleRate, bool bLogFrequency,
                                  const FPwPlotOverlay& Overlay, const FString& AbsolutePngPath,
                                  FString& OutError,
                                  const FPwSpectrogramPlotSettings* Settings = nullptr);

/**
 * Viridis colormap sample. T is clamped to [0, 1]. Perceptually monotonic in luminance, so a
 * brighter pixel is always a louder pixel; interpolated from a 9-entry anchor table.
 */
FColor PwViridis(float T);
