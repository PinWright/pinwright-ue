// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
// TConstArrayView, for the pixel buffer below. CoreMinimal.h pulls in Containers/Array.h but not
// this one.
#include "Containers/ArrayView.h"

class FJsonObject;

// Is a large CONTIGUOUS part of this frame locally near-uniform?
//
// WHY THIS EXISTS. Every frame statistic the plugin publishes today is a WHOLE-FRAME aggregate:
// `meanLuminance`, `luminanceVariance`, `litPixelFraction`, `toneLevelsUsed`, and the `blank` /
// `crushed` / `blownOut` verdicts built from them. Board ticket
// B-mrq-run-jobs-succeeds-on-unrenderable-frames is the failure none of them can see: four
// 3840x2160 frames came back in which everything below ~53 % of frame height was a low-contrast
// pale-grey gradient -- no ground, no shadow, the yard the camera was pointed at simply absent --
// while the sky, the perimeter wall and the buildings above that line rendered correctly. Every
// aggregate stays healthy on such a frame: the top half carries a normal mean, a normal variance
// and dozens of tone levels, so `blank` is false and correct, `crushed` and `blownOut` are false
// and correct, and the frames are still unusable.
//
// A whole-frame aggregate cannot answer this by construction. It has no notion of WHERE the
// content is, and half a good picture averages to a plausible picture. The quantity that
// separates the two is SPATIAL: the largest connected area of the frame with near-zero local
// variation.
//
// WHAT IS MEASURED. The frame is partitioned into a fixed number of blocks PER AXIS (not a fixed
// block size -- see below), each block is flat when its 8-bit luminance standard deviation is at
// most FlatBlockStdDevLevels, and flat blocks are grown into 4-connected regions. The largest such
// region is reported as a fraction of the frame, with the luminance level it sits at and its
// bounding box in normalised frame coordinates, so a caller can say "the lower 47 % is a void"
// without opening the file.
//
// SEED-ANCHORED MERGING, AND WHY A GRADIENT DOES NOT MERGE. A neighbour joins a region only when
// its level is within the tolerance OF THE SEED BLOCK, never of the running frontier. Chaining
// against the frontier would walk a smooth sky gradient one level at a time and report the whole
// sky as one flat region -- the loudest possible false positive, on the most ordinary content
// there is. Anchoring to the seed bounds every region to a bounded luminance band end to end, so a
// long gradient is reported as the many small regions it actually is while the ticket's shallow
// few-level void remains connected.
//
// RESOLUTION-INVARIANT BY CONSTRUCTION, the same property PinWrightRenderCapture's blank and
// tone-range criteria were rewritten to have (see the measurement table in
// PreviewViewportCaptureUtils.h). A fixed block COUNT per axis means every block covers the same
// SHARE of the frame at every capture size, so the same content scores the same fraction at
// 1024 and at 3840 and raising `width` cannot move a verdict.
//
// WHAT IT DELIBERATELY DOES NOT CLAIM. Not the cause: a streaming hole, a missing ground mesh, an
// unconverged first frame and a deliberately flat backdrop produce the same pixels, and no
// readback separates them. Not an error, either -- a large flat region is a WARNING and nothing
// is refused on it, because legitimate content lands here (a flat overcast sky dome, a matte
// backdrop, a title card). The verdict names the observation and its extent; the caller judges.
namespace PinWrightFlatRegion
{
    // Blocks per axis. 64 is fine enough that a quarter-frame void spans ~1000 blocks (so the
    // fraction is accurate to well under a percent) and coarse enough that the whole partition is
    // 4096 entries at any resolution.
    constexpr int32 MaxBlocksPerAxis = 64;
    // A block narrower than this carries too few pixels for "its luminance variance is small" to mean
    // anything -- at one pixel per block every block is trivially flat and the whole frame reads
    // as a void. Small frames therefore get FEWER blocks rather than degenerate ones.
    constexpr int32 MinBlockEdgePixels = 4;
    // A local block whose standard deviation is at most three 8-bit luminance levels is visually
    // flat even when a few noisy pixels widen its min/max range. Connected blocks remain anchored
    // to an eight-level band around the seed, so a gradual sky cannot chain across the image.
    constexpr double FlatBlockStdDevLevels = 3.0;
    constexpr int32 FlatRegionToleranceLevels = 8;
    // The share of the frame at which one flat region stops being scenery and starts being a
    // hole. A quarter of the frame is the figure the board ticket asked for, and it is well above
    // what a matte foreground element or a letterbox bar contributes.
    constexpr double LargeFlatRegionFraction = 0.25;

    struct FFlatRegionStats
    {
        // FALSE IS THE DEFAULT AND IT MATTERS, for the same reason FCaptureImageStats gates its
        // tone fields: a zeroed struct reads as "no flat region at all", which is the reassuring
        // answer, reached by nobody.
        bool bMeasured = false;
        // Why not. Published, because "the buffer was short" and "the frame is too small to
        // partition" are different facts.
        FString NotMeasuredReason;

        int32 BlocksX = 0;
        int32 BlocksY = 0;
        int64 FlatBlockCount = 0;
        int64 BlockCount = 0;
        // Flat blocks anywhere in the frame, connected or not. The context number for the one
        // below: a frame that is flat in many scattered places is a different picture from one
        // that is flat in a single slab.
        double FlatBlockFraction = 0.0;

        // The largest 4-connected flat region, as a share of the frame's pixels. THE number.
        double LargestRegionFraction = 0.0;
        // The 8-bit luminance the region sits at, so "a pale grey void" and "a black void" are
        // distinguishable in the response.
        int32 LargestRegionLevel = 0;
        // The region's bounding box in normalised frame coordinates, TOP-LEFT origin -- the same
        // convention PinWrightSubjectRegion publishes. This is what lets a caller read "everything
        // below 53 % of frame height" straight off the response.
        double RegionMinX = 0.0;
        double RegionMinY = 0.0;
        double RegionMaxX = 0.0;
        double RegionMaxY = 0.0;

        // LargestRegionFraction >= LargeFlatRegionFraction.
        bool bLargeFlatRegion = false;
    };

    // A direct RGB comparison between two captures of the same frame. Values stay in the source
    // BGRA8 domain: MeanAbsDelta is the mean of all R/G/B channel deltas, MaxDelta is the largest
    // per-channel delta, and ChangedPixelFraction counts pixels where any channel exceeds the
    // caller's threshold. Alpha is ignored because capture readback forces it opaque.
    struct FFrameDifferenceStats
    {
        bool bMeasured = false;
        double MeanAbsDelta = 0.0;
        int32 MaxDelta = 0;
        double ChangedPixelFraction = 0.0;
    };

    FFrameDifferenceStats MeasureFrameDifference(TConstArrayView<FColor> Reference,
        TConstArrayView<FColor> Subject, int32 ChannelThreshold);

    // Measure the frame. ColorData is Width*Height BGRA8 entries -- the same buffer
    // PinWrightRenderCapture::CalculateCaptureImageStats runs on, so a measurement taken here and
    // the published imageStats describe the same pixels. A short buffer or a frame too small to
    // partition into at least 2x2 blocks yields bMeasured false rather than a partial reading.
    FFlatRegionStats MeasureLargestFlatRegion(TConstArrayView<FColor> ColorData,
        int32 Width, int32 Height);

    // Write `flatRegionFraction`, `flatRegionLevel`, `flatRegionBounds` and `flatBlockFraction`
    // into a response's `imageStats` object. Writes NOTHING when the region was never measured,
    // the same rule PinWrightRenderCapture::AddToneRangeStatsFields follows -- a
    // `flatRegionFraction: 0` on an unmeasured frame is the reassuring answer nobody reached.
    void AddFlatRegionStatsFields(const FFlatRegionStats& Stats,
        const TSharedPtr<FJsonObject>& ImageStats);

    // One human-readable phrase naming the region's size and where it sits, e.g.
    // "47.1% of the frame at luminance level 233, spanning 0-100% horizontally and 53-100%
    // vertically (top-left origin)". Empty when nothing was measured. Kept beside the numbers
    // rather than assembled at each call site so every warning that quotes a region says it the
    // same way.
    FString DescribeFlatRegion(const FFlatRegionStats& Stats);
}
