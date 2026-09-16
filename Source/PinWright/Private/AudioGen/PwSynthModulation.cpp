// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwSynthModulation.cpp - validation and construction for the one layer-modulation
// implementation. The type, the routing formulas and the reasoning behind each of them live in
// AudioGen/PwSynthDsp.h; this file holds only the two things that do not belong inline in a
// per-sample hot path: the modulator's configuring constructor and the validator that is the
// sole route to it.
//
// Everything here used to be PwGenOscNoiseInternal::ValidateModulation plus a second copy in
// PwGenSampleGranular.cpp, with PwGenModal.cpp and PwGenFormant.cpp carrying no validation at
// all - so a `modal` layer accepted an `am` depth of 50 (legal against the shared 0..100 row,
// which exists for the fm index) and rendered a gain sweeping -49 to +1, and a `formant` layer
// silently clamped the same recipe to 1.0. Both are the class of defect rpc-design.md §2 says
// to make unrepresentable rather than to remember: the modulator's configuring constructor is
// private and PwPrepareModulation is its only friend, so no generator can render a modulation
// block it has not validated.

#include "AudioGen/PwSynthDsp.h"

#include "Handlers/ErrorCodes.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true.
namespace PwSynthModulationInternal
{
    bool Fail(const TCHAR* Code, FString&& Message, FString& OutErrorCode, FString& OutError)
    {
        OutErrorCode = Code;
        OutError = MoveTemp(Message);
        return false;
    }

    /** The modulator never uses `pulse`, and `noise` never reaches the oscillator at all. */
    EPwOscWave ModSourceToWave(EPwSynthModSource Source)
    {
        switch (Source)
        {
        case EPwSynthModSource::Triangle: return EPwOscWave::Triangle;
        case EPwSynthModSource::Saw:      return EPwOscWave::Saw;
        case EPwSynthModSource::Square:   return EPwOscWave::Square;
        default:                          return EPwOscWave::Sine;      // Sine, and Noise (unused)
        }
    }
}

FPwSynthModulator::FPwSynthModulator(const FPwSynthModulation& Modulation, int32 SampleRate,
                                     const FPwSeededRandom& InNoiseRng)
    : Routing(Modulation.Routing)
    , Source(Modulation.Source)
    , Depth(Modulation.Depth)
    , RateHz(Modulation.RateHz)
    , PhaseInc((SampleRate > 0) ? Modulation.RateHz / static_cast<double>(SampleRate) : 0.0)
    , NoiseRng(InNoiseRng)
{
    const EPwOscWave Wave = PwSynthModulationInternal::ModSourceToWave(Source);

    // A quarter turn of head start for the triangle ONLY. FPwBlepOsc's triangle is the `osc`
    // generator's triangle, which starts at -1; a modulator has to start at its neutral value or
    // the layer's first sample carries a step. A quarter turn in, the shared triangle reads 0 at
    // modulator phase 0 and +1 at a quarter turn, i.e. phase-aligned with the sine. Offsetting
    // the shared shape is deliberate - writing a second, differently-phased triangle here is
    // what produced the drift this file exists to end. 50% duty: `pulse` is unreachable.
    const float StartPhaseTurns = (Wave == EPwOscWave::Triangle) ? 0.25f : 0.f;
    Osc.Init(Wave, 0.5f, StartPhaseTurns);

    // First sample-and-hold value drawn here rather than at the first wrap, so frame 0 already
    // carries a drawn value instead of spending the modulator's first whole period at zero.
    Held = static_cast<double>(NoiseRng.FloatInRange(-1.f, 1.f));
}

bool PwPrepareModulation(const FPwSynthModulation& Modulation, bool bAcceptsFm,
                         const TCHAR* GeneratorName, int32 SampleRate,
                         const FPwSeededRandom& NoiseRng, FPwSynthModulator& OutModulator,
                         FString& OutErrorCode, FString& OutError)
{
    using namespace PwSynthModulationInternal;

    // Checked before IsSet(), because an out-of-range routing is not "no modulation" - reading
    // it as absent would render a silent layer for a recipe that asked for one that moves.
    if (static_cast<uint8>(Modulation.Routing) >= static_cast<uint8>(EPwSynthModulationRouting::Count))
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("modulation routing %d is not a routing (fm|am|ring)."),
            static_cast<int32>(Modulation.Routing)), OutErrorCode, OutError);
    }

    if (!Modulation.IsSet())
    {
        // No block at all. OutModulator keeps whatever the caller default-constructed, which is
        // the inert modulator - AmplitudeScale() 1, FrequencyDeviationHz() 0.
        return true;
    }

    if (static_cast<uint8>(Modulation.Source) >= static_cast<uint8>(EPwSynthModSource::Count))
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("modulation source %d is not a modulator shape (sine|triangle|saw|square|noise)."),
            static_cast<int32>(Modulation.Source)), OutErrorCode, OutError);
    }

    if (Modulation.Routing == EPwSynthModulationRouting::Fm && !bAcceptsFm)
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("'fm' modulation has no instantaneous frequency to act on in the '%s' generator; ")
            TEXT("use 'am' or 'ring', or the layer's pitchEnvelope to move the pitch."),
            GeneratorName), OutErrorCode, OutError);
    }

    if (!(Modulation.RateHz > 0.0))
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("modulation rateHz is %g; a modulator with no rate produces no modulation."),
            Modulation.RateHz), OutErrorCode, OutError);
    }

    if (Modulation.Depth < 0.0)
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("modulation depth is %g; depth is a magnitude and cannot be negative."),
            Modulation.Depth), OutErrorCode, OutError);
    }

    // The shared `depth` row spans 0..100 because it doubles as the fm index; its own Meaning
    // text scopes am and ring to 0..1. Enforced rather than clamped: a clamped recipe renders a
    // depth its author did not write and does not round-trip through describe_schema.
    const bool bAmplitudeRouting = Modulation.Routing == EPwSynthModulationRouting::Am
        || Modulation.Routing == EPwSynthModulationRouting::Ring;
    if (bAmplitudeRouting && Modulation.Depth > 1.0)
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("modulation depth is %g; for 'am' and 'ring' depth is the 0..1 wet amount (the 0..100 range on the row is the fm index)."),
            Modulation.Depth), OutErrorCode, OutError);
    }

    OutModulator = FPwSynthModulator(Modulation, SampleRate, NoiseRng);
    return true;
}
