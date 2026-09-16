// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "Dom/JsonObject.h"

// Height-buffer readback statistics for the landscape.get_heights verb — the read
// counterpart to the height-writing verbs (landscape.sculpt / landscape.edit).
//
// The landscape.* namespace could WRITE the heightmap but had no way to sample it,
// so a sculpt/edit round-trip could not be verified per region (a whole-actor
// bounding box is one aggregate Z extent and cannot confirm "the central region
// rose above baseline while the pad flattened to a constant Z") — see
// F-landscape-height-readback. This builds cheap content-revealing aggregates from
// a region of raw uint16 height samples (the same samples the write path reads via
// FLandscapeEditDataInterface::GetHeightData), so no new access mechanism is needed.
//
// The math here is pure (no engine landscape dependency), so the round-trip
// verification aggregates and the world-space Z conversion are unit-testable
// without spinning up a real ALandscape.
namespace LandscapeHeightStats
{
    // World-space Z (centimeters) that a heightmap sample value maps to — the exact inverse of
    // the height math landscape.sculpt / landscape.edit use: 32768 is the zero plane, one height
    // unit is 1/128 of a local unit, scaled by the actor's Z scale and offset by the actor's Z
    // location. Takes a double so the single formula serves both raw uint16 samples (min/max) and
    // fractional aggregates (the region mean). (Valid for the axis-aligned landscapes
    // landscape.create produces; a rotated landscape would mix X/Y into world Z, not modeled here.)
    PINWRIGHT_API double HeightToWorldZ(double Height, double ScaleZ, double ActorLocationZ);

    // A heightmap-pixel region resolved against a landscape's full extent, ready to sample.
    // bValid is false when the region cannot be sampled at all — it is empty after clamping
    // (MinX>MaxX or MinY>MaxY), or the requested rectangle never touched the extent — in which
    // case the coordinate fields are unspecified and the caller must reject the request.
    struct FResolvedHeightRegion
    {
        int32 MinX = 0;
        int32 MinY = 0;
        int32 MaxX = 0;
        int32 MaxY = 0;
        bool bValid = false;
        // Whether the REQUESTED rectangle intersects the extent at all, decided BEFORE the clamp
        // because the clamp destroys the evidence: a request lying wholly past an edge collapses
        // onto the nearest edge pixel and is then indistinguishable from a deliberate 1x1 read of
        // real terrain. See B-landscape-get-heights-fabricates-out-of-extent-region.
        bool bOverlapsExtent = false;
        // Whether the clamp moved a coordinate the caller actually supplied. An absent field
        // defaults to the extent and is never a clamp; only a supplied value can be overridden,
        // and a caller who asked for a window it did not get has to be told which one it got.
        bool bClamped = false;
    };

    // Resolves a requested heightmap-pixel region against the landscape's [Full*, Full*] extent,
    // shared by the read verb (landscape.get_heights) and the write verb (landscape.edit) so the
    // two never diverge. Each coordinate is an explicit TOptional so an ABSENT field (unset ->
    // defaults to the corresponding full extent) is distinguished from a legitimately-NEGATIVE
    // value: on a multi-component landscape whose GetLandscapeExtent min is below zero (not
    // anchored at section 0) a caller must be able to address the negative part of the coordinate
    // space, so a provided negative coordinate is honored rather than treated as "unspecified".
    // (The old int32 -1 sentinel silently replaced any negative coordinate with the full extent —
    // see F-landscape-height-readback.) All four resolved coordinates are then clamped into the
    // full extent; bValid is false when the region is empty after that clamp OR when the
    // requested rectangle does not overlap the extent at all (bOverlapsExtent, tested pre-clamp).
    PINWRIGHT_API FResolvedHeightRegion ResolveHeightRegion(
        const TOptional<int32>& ReqMinX, const TOptional<int32>& ReqMinY,
        const TOptional<int32>& ReqMaxX, const TOptional<int32>& ReqMaxY,
        int32 FullMinX, int32 FullMinY, int32 FullMaxX, int32 FullMaxY);

    // What FLandscapeEditDataInterface::GetHeightData actually filled with terrain, read off the
    // bounds it writes BACK through its four int32& parameters. The engine seeds those references
    // with INT_MAX / INT_MIN and narrows them only for components it found
    // (LandscapeEditInterface.cpp:863, :914-917); every sample in the buffer outside the result is
    // CalcMissingValues interpolation (:1283) — invented terrain, not a measurement. When no
    // component was found the sentinels survive the final clamp untouched (:1291-1294), so echoing
    // those bounds publishes 2147483647 as a coordinate and the uint16 midpoint as world Z 0.
    // Intersecting the returned bounds with the rectangle that was asked for handles both cases
    // without naming the sentinels: an empty intersection IS "nothing here was measured".
    struct FMeasuredHeightRegion
    {
        bool bAnyMeasured = false;
        bool bFullyMeasured = false;
        // The measured sub-rectangle, in the same coordinate space as the asked-for rectangle.
        // Meaningful only when bAnyMeasured.
        int32 MinX = 0;
        int32 MinY = 0;
        int32 MaxX = 0;
        int32 MaxY = 0;
        int64 MeasuredSampleCount = 0;
        // Samples inside the asked-for rectangle the engine filled by interpolation rather than
        // by reading terrain. They are legal-looking uint16 values and must never be published
        // as heights, folded into an aggregate, or counted in sampleCount.
        int64 FabricatedSampleCount = 0;
    };

    PINWRIGHT_API FMeasuredHeightRegion ResolveMeasuredRegion(
        int32 AskedMinX, int32 AskedMinY, int32 AskedMaxX, int32 AskedMaxY,
        int32 ReturnedMinX, int32 ReturnedMinY, int32 ReturnedMaxX, int32 ReturnedMaxY);

    // Copies the [SubMinX..SubMaxX] x [SubMinY..SubMaxY] window out of a row-major buffer whose
    // first sample is (BufMinX, BufMinY) and whose row stride is BufSizeX. Returns false and
    // leaves Out empty when the buffer size disagrees with BufSizeX*BufSizeY or the window is not
    // wholly inside it — used to drop a partially-measured read's fabricated fill on the floor
    // rather than reporting it.
    PINWRIGHT_API bool ExtractSubRegion(
        const TArray<uint16>& In, int32 BufMinX, int32 BufMinY, int32 BufSizeX, int32 BufSizeY,
        int32 SubMinX, int32 SubMinY, int32 SubMaxX, int32 SubMaxY, TArray<uint16>& Out);

    // Builds the readback JSON for a row-major Heights buffer covering SizeX*SizeY samples.
    // Returns per-region raw min/max/mean heights and their world-space Z (minZ/maxZ/meanZ),
    // plus sizeX/sizeY/sampleCount; when bIncludeSamples is set it also emits up to MaxSamples
    // raw samples in the "heights" array with a "samplesTruncated" flag. Returns nullptr with
    // OutError populated when the region is empty or Heights.Num() != SizeX*SizeY.
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildHeightStatsJson(
        const TArray<uint16>& Heights, int32 SizeX, int32 SizeY,
        double ScaleZ, double ActorLocationZ,
        bool bIncludeSamples, int32 MaxSamples, FString& OutError);
}
