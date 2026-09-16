// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"

// Seeded RNG for the AudioGen render path, plus the substream derivation that makes a render
// byte-identical regardless of the order layers happen to be evaluated in.
//
// Determinism note for every caller: Audio::FWhiteNoise and Audio::FPinkNoise (DSP/Noise.h)
// have a default constructor that picks its seed from the CPU cycle counter. Constructing one
// without a seed silently makes the render unreproducible and nothing downstream can detect
// it, so always pass a seed drawn from here.
struct FPwSeededRandom
{
    explicit FPwSeededRandom(int32 InSeed)
        : Stream(InSeed)
    {
    }

    // A per-layer substream. Order-independent by construction: the child seed is a pure hash
    // of the parent's INITIAL seed and StreamIndex, so stream 3 is the same stream whether or
    // not 0-2 were derived first, whether or not the parent has been drawn from since, and
    // deriving it advances nothing. Seeding from the parent's live position instead would tie
    // every layer's noise to the evaluation order, which is the defect this type exists to
    // make unrepresentable.
    FPwSeededRandom Derive(int32 StreamIndex) const
    {
        const uint32 ChildSeed = HashCombine(
            GetTypeHash(Stream.GetInitialSeed()), GetTypeHash(StreamIndex));
        return FPwSeededRandom(static_cast<int32>(ChildSeed));
    }

    float FloatInRange(float Min, float Max)
    {
        return static_cast<float>(Stream.FRandRange(Min, Max));
    }

    // Inclusive of both bounds, matching FRandomStream::RandRange.
    int32 IntInRange(int32 Min, int32 Max)
    {
        return Stream.RandRange(Min, Max);
    }

    FRandomStream Stream;
};
