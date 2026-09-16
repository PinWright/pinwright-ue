// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Deinterleaved stereo float PCM - the currency of the AudioGen subsystem. Generators,
// effects, decode, STFT and export all read and write this one shape, so no stage has to
// know whether the samples came from a mono .wav, a USoundWave, or a procedural recipe.
//
// Invariant: Left.Num() == Right.Num(). SetNumFrames is the only resize that enforces it;
// code that fills the channel arrays directly owns keeping them equal, and IsValid() is the
// check for that. MixInto re-clamps per channel anyway, so a broken invariant degrades to a
// short mix rather than an out-of-bounds write.
struct FPwAudioBuffer
{
    TArray<float> Left;

    // Always allocated, including for mono sources, which duplicate their single channel into
    // both sides so downstream stages never branch on channel count.
    TArray<float> Right;

    int32 SampleRate = 48000;

    int32 NumFrames() const { return Left.Num(); }

    // Both channels agree on length and the sample rate is usable.
    bool IsValid() const;

    // Resizes both channels. Growing zero-fills the new frames when bZeroed; with
    // bZeroed=false they hold uninitialized memory and the caller must write every one.
    // A negative count clamps to empty.
    void SetNumFrames(int32 InNumFrames, bool bZeroed = true);

    // Seconds of audio at the current sample rate; zero when the rate is unset.
    float DurationSeconds() const;

    // Adds this buffer into Target at Target frame StartFrame, scaled by GainLinear, and
    // returns the number of frames actually mixed.
    //
    // The return is the measurement of this call, not a detail to skip: 0 means nothing was
    // mixed - a sample-rate mismatch, an empty source, or a layer that falls entirely outside
    // the target - and a caller summing layers is expected to report that a layer contributed
    // nothing rather than let it look identical to one that contributed.
    //
    // The window is clipped to Target: a negative StartFrame drops the leading source frames
    // instead of shifting the layer forward, a StartFrame past the end writes nothing, and
    // Target is never grown - a layer longer than the render is truncated, not silently
    // extended. The return is that clipped count, so it is the frames written, never the
    // frames requested.
    //
    // A sample-rate mismatch is a no-op. Mixing 44.1k frames into a 48k timeline pitch-shifts
    // the layer, and resampling behind the caller's back would hide the mistake; the skip is
    // logged as a warning on LogPinWrightSubsystem rather than asserted, so a bad recipe
    // surfaces in the log as well as in the returned 0 instead of taking down the editor.
    int32 MixInto(FPwAudioBuffer& Target, float GainLinear, int32 StartFrame) const;
};
