// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwSynthDsp.h - the wave-2 DSP contract. One entry point per generator kind, one per effect
// kind, and the renderer that walks an FPwSynthRecipe (AudioGen/PwSynthRecipe.h) and turns it
// into samples. PwSynthRecipe.h owns the SCHEMA; this header owns the EXECUTION of it.
//
// -----------------------------------------------------------------------------------------
// SPAN CONVENTION - FPwDspSpan is the only shape an effect ever sees
// -----------------------------------------------------------------------------------------
// An effect runs in exactly two places, and the span tells it which:
//
//   layer chain   Left = the layer's mono scratch, Right = nullptr, IsStereo() == false.
//   master chain  Left / Right = the stereo mix bus, both non-null, IsStereo() == true.
//
// `Right == nullptr` is therefore load-bearing, not an optional field: an effect that
// dereferences Right without checking IsStereo() crashes in a layer chain. NumFrames is the
// length of BOTH channels; there is no separate per-channel count. Effects work in place -
// they read and write the same memory - so an effect that needs a dry copy makes one itself.
//
// The one effect kind that genuinely cannot run mono (`width`, mid/side) is marked
// bMasterOnly in its FPwSynthKindSpec, rejected by the parser, and rejected AGAIN by the
// renderer before dispatch. A no-op on a mono span would be a silent nothing, which is the
// failure mode rpc-design.md §3 exists to prevent.
//
// -----------------------------------------------------------------------------------------
// WHY GENERATORS ARE MONO
// -----------------------------------------------------------------------------------------
// Every generator fills a mono buffer, exactly NumFrames of it, and no generator carries a
// stereo field. Stereo placement belongs to ONE owner - the mix bus, via the layer's `pan`
// and an equal-power law applied in PwRenderRecipe. A generator that also produced width
// would make "how wide is this layer" a question with two answers that can disagree, and the
// recipe schema (which has a per-layer `pan` and no per-generator stereo knobs) could not
// round-trip the second one. Mono generators make that state unrepresentable (§2).
//
// -----------------------------------------------------------------------------------------
// OUTPUT LEVEL - what a generator hands the layer, and therefore what `gainDb` is relative to
// -----------------------------------------------------------------------------------------
// The generators do NOT share one output-level convention, so the layer's `gainDb` is a
// relative trim against six different references. Each choice is justified where it is made
// and none of them is going to change; they are collected here so a recipe author does not
// have to read six files to find out what "0 dB" means for the kind they picked.
//
//   osc       Waveform amplitude, peak 1.0 by construction (a unison stack divides by voice
//             count, so N voices do not raise the level). PREDICTABLE.
//   noise     The colour's own convention, NOT a common peak. white/pink/blue/violet come out
//             at the engine noise source's level; brown carries a closed-form make-up gain
//             that restores white's RMS, so it matches in RMS and overshoots 1.0 in peak
//             because brown noise is peaky. RMS-referenced, not peak-referenced.
//   modal     Per-mode: each resonator's input is divided by the closed-form peak of its own
//             impulse response, so with the `impulse` exciter a mode's peak sample amplitude
//             IS its modeGainsDb. The bank's SUM has no bound - many loud modes add.
//   formant   Peak-normalized to exactly 1.0 as the last step, because the bank's absolute
//             output swings tens of dB with vowel, f0 and formantShift. The only generator
//             whose output level is fixed regardless of its parameters.
//   sample    The source asset's own level, mono-collapsed as 0.5*(L+R). Whatever was recorded.
//   granular  The source's level divided by the mean overlap (densityHz * grainMs / 1000,
//             floored at 1), so density is not a second volume knob. Isolated grains keep the
//             source's level; a dense cloud sits at roughly it too.
//
// The consequence worth stating plainly: the same `gainDb` on a `formant` layer and on a
// `sample` layer does not produce the same loudness, and nothing downstream repairs that.
// Absolute level is master `normalize`'s job.
//
// -----------------------------------------------------------------------------------------
// DETERMINISM - the headline property of this subsystem
// -----------------------------------------------------------------------------------------
// The same recipe and seed must render a byte-identical buffer, in the same session and
// across an editor restart. That is a hard contract on every function in this header:
//
//   - No wall-clock, no FDateTime, no cycle counter, no FMath::Rand / FRandomStream default
//     construction. Randomness comes only from the FPwSeededRandom passed in. Audio::FWhiteNoise
//     and Audio::FPinkNoise seed themselves from the CPU cycle counter when default-constructed
//     (see the note on FPwSeededRandom) - always hand them a seed drawn from the argument.
//   - No dependence on layer evaluation order. Each layer receives Root.Derive(LayerIndex),
//     a pure hash of the root seed and the index, so layer 3's stream is identical whether or
//     not layers 0-2 were rendered first.
//   - No thread-scheduling-dependent float accumulation. PwRenderRecipe is single-threaded by
//     construction; a parallel-for over layers would reorder the mix-bus adds and change the
//     low bits. Do not add one.
//   - No engine global state (audio device settings, quality cvars) read at render time.
//
// -----------------------------------------------------------------------------------------
// FAILURE DIRECTION
// -----------------------------------------------------------------------------------------
// Every function here returns bool and takes OutErrorCode / OutError. OutErrorCode is an
// ErrorCodes::ERR_* literal (Handlers/ErrorCodes.h) so a handler forwards it unmodified;
// OutError is the human sentence naming the measured quantity that failed. On false, the
// output buffer is not to be trusted and the caller must abort rather than continue with
// whatever partial samples are there - PwRenderRecipe does exactly that.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwSeededRandom.h"
#include "AudioGen/PwSynthRecipe.h"

struct FPwDspSpan
{
    float* Left = nullptr;
    float* Right = nullptr;          // null for a mono layer chain
    int32  NumFrames = 0;
    bool IsStereo() const { return Right != nullptr; }
};

// =============================================================================================
// BAND-LIMITED OSCILLATOR CORE + THE ONE LAYER-MODULATION IMPLEMENTATION
// =============================================================================================
// Both of these were file-local to PwGenOscNoise.cpp, which is exactly why `am` and `ring` ended
// up written three more times in PwGenModal.cpp, PwGenFormant.cpp and PwGenSampleGranular.cpp.
// The four copies had drifted in four independent ways - two different `am` formulas, two
// different triangle phase conventions, band-limited saw/square in one file and naive saw/square
// in the other three, and a `noise` source that was sample-and-hold in one generator and
// full-rate white in the rest. A recipe's `modulation` block therefore meant something slightly
// different depending on which generator the layer happened to name, which is not a difference
// the schema can express. One implementation lives here so a fifth copy is never needed and a
// seventh generator inherits the settled answers.
// ---------------------------------------------------------------------------------------------

enum class EPwOscWave : uint8
{
    Sine,
    Triangle,
    Saw,
    Square,
    Pulse
};

/**
 * PolyBLEP residual for the -1 -> +1 step of a bipolar wave, spread over the two samples
 * straddling the discontinuity. T is the phase in turns, Dt the phase increment per sample.
 */
inline float PwPolyBlep(float T, float Dt)
{
    if (Dt <= 0.f)
    {
        return 0.f;
    }
    if (T < Dt)
    {
        const float X = T / Dt;
        return X + X - X * X - 1.f;
    }
    if (T > 1.f - Dt)
    {
        const float X = (T - 1.f) / Dt;
        return X * X + X + X + 1.f;
    }
    return 0.f;
}

/** Wraps a phase in turns into [0, 1). Correct for negative input, which an FM sweep past
    zero Hz produces. */
inline float PwWrapTurns(float Phase)
{
    return Phase - FMath::FloorToFloat(Phase);
}

/**
 * Band-limited waveform core, shared by the `osc` generator and by every generator's modulator.
 *
 * Audio::FOsc is deliberately NOT used, for two reasons measured against UE 5.8 source:
 *
 *   1. It cannot express this schema. `phase` (start phase in turns) and per-voice unison phase
 *      both need a settable initial phase, and IOscBase exposes only ResetPhase() -> 0.
 *   2. FOsc::Generate's EOsc::Square case returns before the shared UpdatePhase() at the bottom
 *      of the function (Private/Osc.cpp:318), so a square oscillator's phase never advances and
 *      it emits constant zero. Building `square` / `pulse` on it would ship silence under a
 *      waveform name.
 *
 * What FOsc's PolySmooth does IS kept: the discontinuities of saw / square / pulse carry a
 * two-sample PolyBLEP residual, which is what stops their 1/n harmonic series folding back down
 * over Nyquist. Sine and triangle need no correction (see the triangle comment).
 */
struct FPwBlepOsc
{
    void Init(EPwOscWave InWave, float InPulseWidth, float StartPhaseTurns)
    {
        Wave = InWave;
        Phase = PwWrapTurns(StartPhaseTurns);

        PulseWidth = FMath::Clamp(InPulseWidth, 0.01f, 0.99f);

        // A duty cycle other than 50% gives the naive pulse a DC offset of (2*pw - 1), which
        // would waste headroom, show up as bin-0 energy in every analysis, and click at the
        // layer's edges. Removing it lifts one of the two levels to 1 + |2*pw - 1|, so the
        // scale below puts the peak back at unity.
        PulseDc = 2.f * PulseWidth - 1.f;
        PulseScale = 1.f / (1.f + FMath::Abs(PulseDc));
    }

    /** One sample at the supplied phase increment (turns per sample); advances the phase. */
    float Generate(float PhaseInc)
    {
        // A large FM index can drive the instantaneous frequency negative, i.e. run the phase
        // backwards. The PolyBLEP window is symmetric about the discontinuity, so the residual
        // is taken against the magnitude of the increment.
        const float Dt = FMath::Abs(PhaseInc);

        float Out = 0.f;
        switch (Wave)
        {
        case EPwOscWave::Sine:
            Out = FMath::Sin(2.f * UE_PI * Phase);
            break;

        case EPwOscWave::Triangle:
            // Naive, deliberately. A triangle's harmonics fall as 1/n^2, so its folded images
            // land roughly 40 dB below a naive saw's at the same fundamental - already under
            // the noise floor of a 16-bit render. Every band-limited alternative buys that
            // back with a worse defect at the edges of this schema's range: integrating a
            // band-limited square needs a DC-blocking leak that would eat the sub-20 Hz
            // fundamentals `frequencyHz` allows down to 0.01 Hz, and the engine's DPW triangle
            // (Private/Osc.cpp:321-341) divides by the fundamental, which is numerically
            // hopeless there. The correction effort goes where the aliasing actually is: saw,
            // square and pulse, whose 1/n series folds audibly.
            Out = (Phase < 0.5f) ? (4.f * Phase - 1.f) : (3.f - 4.f * Phase);
            break;

        case EPwOscWave::Saw:
            Out = 2.f * Phase - 1.f;
            Out -= PwPolyBlep(Phase, Dt);
            break;

        case EPwOscWave::Square:
        case EPwOscWave::Pulse:
            Out = (Phase < PulseWidth) ? 1.f : -1.f;
            Out += PwPolyBlep(Phase, Dt);                            // rising edge, at phase 0
            Out -= PwPolyBlep(PwWrapTurns(Phase - PulseWidth), Dt);  // falling edge, at phase PulseWidth
            Out = (Out - PulseDc) * PulseScale;
            break;
        }

        Phase = PwWrapTurns(Phase + PhaseInc);
        return Out;
    }

    EPwOscWave Wave = EPwOscWave::Sine;
    float PulseWidth = 0.5f;
    float PulseDc = 0.f;
    float PulseScale = 1.f;
    float Phase = 0.f;      // turns, [0, 1)
};

// ---------------------------------------------------------------------------------------------
// Layer modulation - fm / am / ring, one implementation for all six generators.
// ---------------------------------------------------------------------------------------------
//
// SHAPE AND PHASE. Every shape is read at the CURRENT phase and then advanced by exactly one
// frame, and every shape starts at its NEUTRAL value where that notion applies: sine and
// triangle are both 0 at phase 0 and peak a quarter turn later. The triangle is the FPwBlepOsc
// triangle above started a quarter turn in rather than a second triangle written out again, so
// the two cannot drift apart; starting it at its own phase 0 would put the modulator at -1 on
// the layer's first sample, which is a step rather than a modulation. saw and square keep their
// conventional -1 / +1 starts, having no neutral value to start from.
//
// BAND-LIMITED. saw and square go through the same PolyBLEP core the `osc` generator uses. At
// LFO rates the correction is nil; at ring-modulation rates it is the difference between a clean
// sideband pair and a spray of folded harmonics.
//
// `noise` IS SAMPLE-AND-HOLD, redrawn once per rateHz period. Full-rate white noise would make
// `rateHz` mean nothing at all on that source, and a parameter that silently does nothing is
// indistinguishable from one that is broken (rpc-design.md §1). The held value comes from
// FPwSeededRandom, so the choice costs no determinism.
//
// DEPTH IS TWO DIFFERENT QUANTITIES, which is why the schema's `depth` row spans 0..100:
//
//   fm    the textbook FM index I. Peak deviation is I * rateHz, in Hz, so the whole 0..100
//         range is a range of indices.
//   am    a 0..1 WET amount:  scale = lerp(1, 0.5 + 0.5*m, depth)
//   ring  a 0..1 WET amount:  scale = lerp(1, m,           depth)
//
// Both amplitude routings are a dry/wet blend of the same modulator, differing only in whether
// the wet signal is unipolar (am) or bipolar (ring). Depth above 1 on either is an ERROR, not a
// clamp: the row's upper bound belongs to fm, and a recipe silently rendered at a depth its
// author did not write does not round-trip.
//
// `am` DELIBERATELY CANNOT BOOST. The obvious alternative, `1 + depth*m`, reaches 2.0 at full
// depth - an amplitude modulator that quietly adds 6 dB before the master chain has measured
// anything, in a pipeline that then normalizes and clamps. The blend form also degrades sanely:
// depth 0 is exactly unity, depth 1 modulates all the way to silence, and nothing in between
// surprises.

/**
 * The layer's modulator, advanced one output frame at a time.
 *
 * A default-constructed modulator is INERT: no routing, so Generate() returns 0,
 * FrequencyDeviationHz() returns 0 and AmplitudeScale() returns exactly 1. Callers may therefore
 * multiply by AmplitudeScale unconditionally without first asking whether modulation is present.
 *
 * The CONFIGURING constructor is private and PwPrepareModulation is its only friend, so a
 * modulator that carries a routing cannot exist without having been validated (rpc-design.md §2).
 */
struct FPwSynthModulator
{
    /** Inert. NoiseRng is seeded only so the member has a value; no routing ever reads it. */
    FPwSynthModulator()
        : NoiseRng(0)
    {
    }

    bool IsSet() const { return Routing != EPwSynthModulationRouting::None; }

    bool IsAmplitudeRouting() const
    {
        return Routing == EPwSynthModulationRouting::Am || Routing == EPwSynthModulationRouting::Ring;
    }

    /** Advances one frame and returns the bipolar modulator value in [-1, 1]. */
    double Generate()
    {
        if (Routing == EPwSynthModulationRouting::None)
        {
            return 0.0;
        }

        if (Source == EPwSynthModSource::Noise)
        {
            // Sample-and-hold: the drawn value is held for a whole rateHz period, so rateHz is
            // the redraw rate rather than a knob with no observable effect.
            const double Value = Held;
            NoisePhase += PhaseInc;
            if (NoisePhase >= 1.0)
            {
                NoisePhase -= FMath::FloorToDouble(NoisePhase);
                Held = static_cast<double>(NoiseRng.FloatInRange(-1.f, 1.f));
            }
            return Value;
        }

        return static_cast<double>(Osc.Generate(static_cast<float>(PhaseInc)));
    }

    /** Additive Hz deviation for `fm`, 0 for every other routing. depth is the FM index I,
        whose textbook definition is I = peak deviation / modulator frequency. */
    double FrequencyDeviationHz(double ModValue) const
    {
        return (Routing == EPwSynthModulationRouting::Fm) ? Depth * RateHz * ModValue : 0.0;
    }

    /** Amplitude scale for `am` / `ring`; exactly 1 for every other routing. Never exceeds 1
        for `am` - see the depth note above. */
    double AmplitudeScale(double ModValue) const
    {
        switch (Routing)
        {
        case EPwSynthModulationRouting::Am:
            // Unipolar wet signal: 0.5 + 0.5*m rides 0..1, blended against dry unity.
            return FMath::Lerp(1.0, 0.5 + 0.5 * ModValue, Depth);

        case EPwSynthModulationRouting::Ring:
            // Bipolar wet signal: depth 1 is a true ring modulator, depth 0 is the dry signal.
            return FMath::Lerp(1.0, ModValue, Depth);

        default:
            return 1.0;
        }
    }

private:
    friend bool PwPrepareModulation(const FPwSynthModulation&, bool, const TCHAR*, int32,
                                    const FPwSeededRandom&, FPwSynthModulator&, FString&, FString&);

    FPwSynthModulator(const FPwSynthModulation& Modulation, int32 SampleRate,
                      const FPwSeededRandom& InNoiseRng);

    EPwSynthModulationRouting Routing = EPwSynthModulationRouting::None;
    EPwSynthModSource Source = EPwSynthModSource::Sine;
    double Depth = 0.0;
    double RateHz = 0.0;
    double PhaseInc = 0.0;      // turns per output frame
    double NoisePhase = 0.0;    // `noise` source only; the shaped sources keep their phase in Osc
    double Held = 0.0;          // sample-and-hold value, `noise` source only
    FPwSeededRandom NoiseRng;
    FPwBlepOsc Osc;
};

/**
 * Validates a modulation block and, on success, hands back a modulator configured from it.
 * On failure OutModulator is left untouched and the caller must abort before writing a sample.
 *
 * THIS IS THE ONLY WAY TO GET A CONFIGURED MODULATOR, and that is the point: the validation
 * below cannot be forgotten by a new generator, because skipping it leaves the caller holding an
 * inert modulator that renders NO modulation - which any modulation test catches - rather than
 * unvalidated modulation, which nothing catches (rpc-design.md §2).
 *
 * What is enforced: routing and source inside their closed vocabularies, rateHz > 0, depth >= 0,
 * and depth <= 1 for `am` / `ring`. Every one of them is an error; none of them clamps.
 *
 * bAcceptsFm is false for a generator with no instantaneous frequency for `fm` to act on -
 * `noise` has none, and a recorded sample has a read RATE rather than a carrier frequency, so
 * there is no single number to add Hz to. Those generators REJECT `fm` rather than ignoring it,
 * and the rejection names `am` / `ring` and the layer's pitchEnvelope as the recoveries.
 */
bool PwPrepareModulation(const FPwSynthModulation& Modulation, bool bAcceptsFm,
                         const TCHAR* GeneratorName, int32 SampleRate,
                         const FPwSeededRandom& NoiseRng, FPwSynthModulator& OutModulator,
                         FString& OutErrorCode, FString& OutError);

// Generators - mono, fill OutMono exactly. Pitch envelope is layer-relative and may be empty.
bool PwGenOsc     (const FPwSynthParams& Params, const TArray<FPwSynthPitchPoint>& PitchEnvelope,
                   const FPwSynthModulation& Modulation, int32 SampleRate, FPwSeededRandom& Rng,
                   TArrayView<float> OutMono, FString& OutErrorCode, FString& OutError);
bool PwGenNoise   (const FPwSynthParams& Params, const TArray<FPwSynthPitchPoint>& PitchEnvelope,
                   const FPwSynthModulation& Modulation, int32 SampleRate, FPwSeededRandom& Rng,
                   TArrayView<float> OutMono, FString& OutErrorCode, FString& OutError);
bool PwGenModal   (const FPwSynthParams& Params, const TArray<FPwSynthPitchPoint>& PitchEnvelope,
                   const FPwSynthModulation& Modulation, int32 SampleRate, FPwSeededRandom& Rng,
                   TArrayView<float> OutMono, FString& OutErrorCode, FString& OutError);
bool PwGenFormant (const FPwSynthParams& Params, const TArray<FPwSynthPitchPoint>& PitchEnvelope,
                   const FPwSynthModulation& Modulation, int32 SampleRate, FPwSeededRandom& Rng,
                   TArrayView<float> OutMono, FString& OutErrorCode, FString& OutError);
bool PwGenGranular(const FPwSynthParams& Params, const TArray<FPwSynthPitchPoint>& PitchEnvelope,
                   const FPwSynthModulation& Modulation, int32 SampleRate, FPwSeededRandom& Rng,
                   TArrayView<float> OutMono, FString& OutErrorCode, FString& OutError);
bool PwGenSample  (const FPwSynthParams& Params, const TArray<FPwSynthPitchPoint>& PitchEnvelope,
                   const FPwSynthModulation& Modulation, int32 SampleRate, FPwSeededRandom& Rng,
                   TArrayView<float> OutMono, FString& OutErrorCode, FString& OutError);

// Effects - in place. Right == nullptr means a mono layer chain.
bool PwFxFilter    (const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
                    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError);
bool PwFxDistort   (const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
                    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError);
bool PwFxDelay     (const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
                    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError);
bool PwFxReverb    (const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
                    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError);
bool PwFxChorus    (const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
                    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError);
bool PwFxFlanger   (const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
                    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError);
bool PwFxPhaser    (const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
                    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError);
bool PwFxRingmod   (const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
                    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError);
bool PwFxPitchshift(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
                    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError);
bool PwFxCompressor(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
                    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError);
bool PwFxEq        (const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
                    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError);
bool PwFxGain      (const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
                    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError);
bool PwFxWidth     (const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
                    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError);
bool PwFxReverse   (const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
                    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError);
bool PwFxConvolve  (const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
                    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError);

// Shared source loader - implemented by the sample/granular chunk, used by convolve too.
//
// SAMPLE-RATE SEAM, and the rule is on the CONSUMER: `Out` always comes back at the ASSET'S
// NATIVE rate, with FPwAudioBuffer::SampleRate set to it. The loader is handed a path and
// nothing else - it cannot know the render's rate - so resampling to the render rate is every
// caller's job, not this function's.
//
// Getting this wrong is silent, which is why it is written down here at the seam: a 44.1 kHz
// asset used unresampled in a 48 kHz render makes FPwAudioBuffer::MixInto refuse the mix
// outright (it no-ops on a rate mismatch and returns 0), so a `sample` layer becomes inaudible
// zero while every stage reports success. Compare Out.SampleRate against the render rate and
// resample before use.
bool PwResolveSourceBuffer(const FString& AssetPath, FPwAudioBuffer& Out,
                           FString& OutErrorCode, FString& OutError);

// Renderer.

// Per-layer measurement. Every field is observed, never assumed: `bMeasured` stays false on
// any layer the renderer did not get as far as, so a report row cannot claim a clean zero it
// never looked at.
struct FPwLayerReport
{
    int32  LayerIndex = 0;
    bool   bMeasured = false;
    int32  FramesMixed = 0;          // from FPwAudioBuffer::MixInto - 0 means contributed nothing
    // Peak absolute sample of the layer's own mono signal, measured after the generator, the
    // amp envelope and the layer fx chain, but BEFORE gainDb and the pan law. This deliberate
    // generator-stage diagnostic is retained as the legacy `peakLinear` response field.
    double PeakLinear = 0.0;
    // Peak absolute sample after gainDb and the equal-power pan law, before the layer is mixed
    // into the stereo bus. This is the layer's actual contribution level for balance guidance.
    double PeakLinearPostGain = 0.0;
};

// Whole-render measurement. `bMeasured` is set on the single full-success path at the very end
// of PwRenderRecipe; a render that aborted reports false and carries no layer rows at all,
// so a caller cannot read half a report as a whole one.
struct FPwRenderReport
{
    bool   bMeasured = false;
    TArray<FPwLayerReport> Layers;
    // Samples the final clamp actually moved, counted across both channels. Not a prediction.
    int32  ClampedSamples = 0;
    // Peak of the stereo bus after the master fx chain and before normalization, in dBFS.
    // A fully silent bus reports PwSynthRender::SilenceFloorDb rather than -inf.
    double PeakDbBeforeNormalize = 0.0;
    // The value the normalizer KEYED ON, in the unit its mode measures: dBFS peak in Peak mode,
    // LUFS in Lufs mode. It is the analyzer's own output, not Target - NormalizeGainDb, which
    // would only restate the gain. In Lufs mode it is the one number in this report that says
    // what the loudness actually was; nothing else here can express it.
    double NormalizeInputDb = 0.0;
    // False whenever no normalization measurement was taken - no normalize block at all, or a
    // silent bus in Peak mode. Read it before NormalizeInputDb: the default 0.0 is not a level.
    bool   bNormalizeMeasured = false;
    // The gain normalization ACTUALLY applied, in dB. 0.0 when no normalize block was present,
    // and also 0.0 when the bus was silent and no finite gain could reach the target -
    // bNormalizeMeasured is what distinguishes those two cases.
    double NormalizeGainDb = 0.0;
};

namespace PwSynthRender
{
    // dBFS reported for a zero-amplitude measurement. Below 24-bit LSB (-144.5 dBFS), so it
    // cannot be confused with a real level, and finite, so it survives a JSON round trip.
    inline constexpr double SilenceFloorDb = -144.0;

    // Shortest render LUFS normalization is accepted for. Audio::FLKFSAnalyzer emits its first
    // result only once NumAnalysisWindows sliding windows have gone through it - the 0.4 s
    // AnalysisWindowDuration plus a 4096-sample FFT window of ramp-in - so 400 ms is not enough
    // and 500 ms clears it at 48 kHz (0.4 + 4096/48000 = 485 ms).
    //
    // This is a fast, clearly-worded rejection, NOT the authority, and it is deliberately a
    // FLOOR rather than the exact requirement: the true minimum is rate-dependent (0.4 s +
    // 4096/sampleRate), so at the low end of PwSynthLimits::MinSampleRate a render can clear
    // 500 ms and still be too short. PwComputeLoudness (AudioGen/PwAudioFeatures.h) computes
    // that per-rate figure and is the real gate; the renderer calls it and reports its sentence.
    // Both paths error rather than degrading to peak normalization, because -16 LUFS and
    // -16 dBFS are different requests.
    inline constexpr double MinLufsDurationMs = 500.0;

    // Exponent of the exp / log envelope curves. Published because a curve shape that is not
    // written down is not reproducible from the recipe alone.
    inline constexpr double CurveExponent = 2.0;
}

// Renders a recipe into `Out`.
//
// Failure is the default: `Out` is emptied on entry and written only on the single full-success
// path, and `OutReport` is reset on every failure exit, so neither a half-mixed buffer nor a
// partial report is observable. On false, OutReport.bMeasured == false and OutReport.Layers is
// empty.
//
// Single-threaded by contract - see the determinism note at the top of this header.
bool PwRenderRecipe(const FPwSynthRecipe& Recipe, FPwAudioBuffer& Out, FPwRenderReport& OutReport,
                    FString& OutErrorCode, FString& OutError);
