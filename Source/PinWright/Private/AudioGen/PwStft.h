// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "DSP/AudioFFT.h"

// Offline short-time Fourier transform for the audio-analysis verbs.
//
// Deliberately NOT built on Audio::FSpectrumAnalyzer: that class is a realtime
// producer/consumer ring buffer with a double-buffered locked output, an enum-capped FFT
// size, and only point queries / extractor bands on the read side. An offline spectrogram
// wants the opposite shape - the whole magnitude matrix, computed in one pass, with no
// locking and no size enum. So this drives Audio::FFFTFactory (DSP/FFTAlgorithm.h) and
// Audio::FWindow (DSP/AudioFFT.h) directly. The factory self-registers from
// SignalProcessingModule::StartupModule (FVectorFFTFactory), so no plugin has to be enabled.
//
// Every symbol used here (FFFTSettings, FFFTFactory, IFFTAlgorithm, FWindow, CeilLog2,
// GetPowerSpectrumScaling, EFFTScaling) is present unchanged on UE 5.3 through 5.8, so this
// file owes no row in docs/engine-version-support.md.

/** Analysis parameters. FftSize must be a power of two; a non-power-of-two is an error, never rounded. */
struct FPwStftSettings
{
    /** Window length AND transform length, in samples. Power of two, 32..131072 (FVectorFFT's supported Log2 range). */
    int32 FftSize = 2048;

    /** Advance between consecutive analysis frames, in samples. */
    int32 HopSize = 256;

    /** Analysis window. Hann is the default: -31 dB first sidelobe and well-behaved for COLA. */
    Audio::EWindowType Window = Audio::EWindowType::Hann;
};

/**
 * Magnitude spectrogram.
 *
 * SCALING CONVENTION (fixed, documented, not caller-selectable): magnitudes are single-sided
 * window-normalized amplitudes referenced to full scale. A unit-amplitude (0 dBFS) sinusoid
 * sitting exactly on a bin centre produces magnitude 1.0 in that bin, so 20*log10(magnitude)
 * reads directly as dBFS and is comparable between two different FFT sizes, window types and
 * hop sizes. The three factors that produce it are applied in PwComputeStft:
 *   1. Normalize the implementation's output to the UNSCALED DFT, via
 *      Audio::GetPowerSpectrumScaling(FftSize, Alg->ForwardScaling(),
 *      EFFTScaling::MultipliedBySqrtFFTSize), square-rooted to turn that power factor into an
 *      amplitude factor. EFFTScaling is measured against an ENERGY-PRESERVING transform, and by
 *      Parseval the unscaled DFT carries a factor-N energy gain over that - so the unscaled DFT
 *      is itself "MultipliedBySqrtFFTSize", which is what FVectorFFT reports and why its factor
 *      works out to 1. Targeting None instead would divide a second time by sqrt(N).
 *   2. Divide by the window's coherent gain (the sum of its samples), which removes the
 *      window's amplitude loss so Hann and rectangular agree on a tone's level.
 *   3. Fold the negative frequencies in: x2 for bins 1..NumBins-2, x1 for DC and Nyquist,
 *      which have no mirror partner.
 */
struct FPwStftResult
{
    /** Frame-major, NumFrames * NumBins floats. Frame f, bin b is Magnitudes[f * NumBins + b]. */
    TArray<float> Magnitudes;

    int32 NumFrames = 0;

    /** FftSize / 2 + 1 (DC through Nyquist inclusive). */
    int32 NumBins = 0;

    /** SampleRate / FftSize. Bin b is centred on b * BinHz. */
    float BinHz = 0.f;

    // FftSize and HopSize are carried through because a plot cannot label a time axis without
    // them: NumFrames alone gives no seconds-per-column. FftSize is recoverable as
    // (NumBins - 1) * 2, HopSize is not recoverable at all.
    int32 FftSize = 0;
    int32 HopSize = 0;

    /** True only for a fully populated result. A default-constructed result is invalid by construction. */
    bool IsValid() const
    {
        return NumFrames > 0 && NumBins > 0 && FftSize > 0 && HopSize > 0
            && Magnitudes.Num() == NumFrames * NumBins;
    }
};

/** Structured failure. Code is an ErrorCodes::ERR_* literal so a handler can forward it unmodified. */
struct FPwStftError
{
    FString Code;
    FString Message;
};

/**
 * Compute the magnitude spectrogram of a mono signal.
 *
 * Failure is the default (rpc-design.md §1): Out is cleared on entry and is populated only on
 * the single full-success path. The checks run existence-first (rpc-design.md §7) - an empty
 * buffer and an all-silent buffer are named as such by ERR_AUDIO_EMPTY_BUFFER BEFORE any
 * size-validation or FFT-construction failure, so "there was no signal" can never be reported
 * as the smaller, more specific-sounding "the FFT failed".
 *
 * @param OutError  Optional. When supplied, receives the ErrorCodes code + a message naming the
 *                  measured quantity that failed. Untouched contents on success are cleared.
 * @return true only when Out.IsValid().
 */
bool PwComputeStft(TArrayView<const float> Mono, int32 SampleRate,
                   const FPwStftSettings& Settings, FPwStftResult& Out,
                   FPwStftError* OutError = nullptr);

/**
 * Number of analysis frames for a signal of NumSamples: 1 + (NumSamples - FftSize) / HopSize,
 * integer division, i.e. only whole windows that fit inside the signal. Returns 0 when the
 * signal is shorter than one window or the parameters are non-positive.
 */
int32 PwStftFrameCount(int32 NumSamples, int32 FftSize, int32 HopSize);

/**
 * dBFS for a magnitude under the convention above. Returns FloorDb for zero, negative and NaN
 * inputs rather than -inf, so a silent bin plots at the bottom of the scale instead of poisoning
 * the image.
 */
float PwMagnitudeToDb(float Magnitude, float FloorDb = -90.f);

/** Bounds-checked accessor; returns 0 for an out-of-range coordinate. */
inline float PwStftMagnitudeAt(const FPwStftResult& In, int32 FrameIndex, int32 BinIndex)
{
    if (FrameIndex < 0 || FrameIndex >= In.NumFrames || BinIndex < 0 || BinIndex >= In.NumBins)
    {
        return 0.f;
    }
    return In.Magnitudes[FrameIndex * In.NumBins + BinIndex];
}
