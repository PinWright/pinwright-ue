// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Render/CaptureDefaults.h"

// Pure analysis math behind render.detect_z_fighting. No UObject, no RHI, no viewport, so
// every rule below is unit-testable headlessly; the handler supplies the pixels.
//
// WHAT THE DETECTOR ACTUALLY TESTS
//
// Two renders of ONE instant, identical in every respect except the near clipping plane.
// Under the infinite-far reversed-Z projection a scene capture builds, clip-space Z is the
// near plane itself and clip-space W is view depth, so the depth written per pixel is
// Near/W. Rescaling Near therefore perturbs the depth VALUES without moving a single pixel
// in X or Y: screen position, perspective-correct attribute interpolation, texture mip
// selection and every shading input are bit-for-bit unchanged.
//
// The consequence is the whole design. A pixel whose depth comparison is not close to a tie
// renders the same surface in both captures, and because nothing else changed, its G-buffer
// values come back BIT-IDENTICAL. A pixel whose depth comparison sits within the depth
// buffer's resolution can be re-decided by the perturbation, and then the pixel shows a
// DIFFERENT SURFACE — a discrete jump in albedo or normal. Signal is a surface swap; noise
// is zero by construction.
//
// WHY THE COMPARISON IS ON G-BUFFER IDENTITY AND NOT ON DEPTH ITSELF
//
// Differencing the two captures' linear DEPTH looks like the obvious test and does not work.
// Z-fighting is by definition the case where two surfaces' depths agree to within the depth
// buffer's resolution, so the depth difference between the two contenders is at most one
// quantum. But rescaling the near plane also moves the quantisation grid, so EVERY pixel's
// reconstructed depth moves by up to one quantum, fighting or not. Signal and noise are the
// same physical quantity and no threshold separates them. Worse, a power-of-two ratio makes
// the perturbation vanish entirely: scaling Near by an exact power of two scales every
// depth by that power of two with no change of rounding at all, so the comparison outcome is
// provably identical everywhere and the detector returns a confident zero on a broken scene.
// IsPowerOfTwoRatio exists to make that input impossible rather than silently useless.
//
// WHAT IT CANNOT SEE, STATED PLAINLY
//
//  - Two surfaces that fight but look IDENTICAL (same albedo, same normal) produce no
//    difference. They also produce no visible artefact, so this is a deliberate exclusion,
//    not an accident — but it does mean a clean result is "nothing visibly fights from this
//    camera", not "no geometry is coincident".
//  - Two surfaces at EXACTLY the same depth with exactly the same vertex positions never
//    fight at all: the depth test resolves them by draw order, identically every frame and
//    under any projection. They are invisible to this test because there is nothing to see.
//  - Anything outside the frame. This is a per-view test and coverage is the caller's
//    problem.
//  - Translucent surfaces, which are not depth-tested against each other and write no
//    G-buffer.
namespace PinWrightZFighting
{
    // Measured 2026-08-24 on the matched coplanar-slab fixture in TestZFightingDetect: 1920 found
    // 461 affected pixels in 8 regions; 768 found 38 pixels in the same 8 regions and preserved
    // the failing verdict. The separated control found 0 pixels / 0 regions at both resolutions.
    // Therefore this analysis shares the 768 capture default; ResolutionComparison guards both
    // the true-positive and true-negative directions against future detector or renderer drift.
    constexpr int32 DefaultAnalysisLongEdge = PinWrightRenderCapture::DefaultCaptureEdge;

    // The MASK is a picture and does take the multi-image review budget: it is the only
    // output a human or agent looks at. Downscaling is max-pooled, never averaged, so a
    // one-pixel seam at analysis resolution survives as a visible pixel here instead of
    // being diluted below visibility.
    constexpr int32 DefaultMaskLongEdge = PinWrightRenderCapture::DefaultCaptureEdge;

    // Per-channel absolute difference that counts as a surface swap. Non-swapping pixels are
    // expected to be bit-identical, so this is not a signal/noise trade: it is slack against
    // renderer non-determinism that is not supposed to exist. It doubles as the "these two
    // surfaces look the same" cutoff, below which a swap is not a visible defect.
    constexpr double DefaultChannelThreshold = 0.02;

    // Second render's near plane as a multiple of the first. Must not be 1 (no perturbation)
    // and must not be a power of two (provably no perturbation, see the header comment).
    constexpr double DefaultNearPlaneRatio = 3.0;

    // Pass/fail gate, as a FRACTION of analysed pixels rather than an absolute count, so the
    // same threshold means the same thing at every resolution. An absolute floor would silently
    // change strictness when a caller supplies another size.
    //
    // 2e-5 is about 12 pixels at 768x768 — roughly a 3x4 block.
    // block at the default size. Below that a result is isolated pixels; above it there is a
    // seam. Because non-swapping pixels are bit-identical the floor exists to absorb
    // speckle at genuine geometry intersections, not measurement noise.
    constexpr double DefaultMinFraction = 0.00002;

    constexpr int32 DefaultMaxRegions = 8;
    constexpr int32 MaxAllowedRegions = 64;

    // Per-pixel state. Bit flags rather than an enum because a pixel can be both flagged by
    // one channel and excluded by the near-clip rule, and the two must stay separable: an
    // excluded pixel is "not measured", which is a different statement from "measured clean"
    // and is reported separately.
    enum EMaskFlags : uint8
    {
        MASK_None = 0,
        MASK_Flagged = 1 << 0,
        MASK_ClipExcluded = 1 << 1,
    };

    inline bool IsAffected(uint8 Flags)
    {
        return (Flags & MASK_Flagged) != 0 && (Flags & MASK_ClipExcluded) == 0;
    }

    // True when Ratio is (within float slop) an exact power of two, including 1. Scaling the
    // near plane by such a ratio scales every stored depth by the same power of two with no
    // change of rounding, so no depth comparison anywhere in the frame can change its
    // outcome. Callers reject these inputs instead of running a guaranteed-empty analysis.
    bool IsPowerOfTwoRatio(double Ratio);

    struct FRegion
    {
        int32 MinX = 0;
        int32 MinY = 0;
        int32 MaxX = 0;
        int32 MaxY = 0;
        int32 Pixels = 0;
        double CentroidX = 0.0;
        double CentroidY = 0.0;
    };

    // OR the pixels where |A - B| exceeds Threshold on any of R/G/B into InOutMask's
    // MASK_Flagged bit. Returns the number of pixels this channel flagged (including ones a
    // previous channel had already flagged, so per-channel counts are independently
    // meaningful and do not sum to the total).
    //
    // Alpha is ignored: the capture shader writes a constant there for every G-buffer source,
    // so it carries no surface identity and comparing it would only add a way to be wrong.
    int32 AccumulateChannelDifference(TConstArrayView<FLinearColor> CaptureA,
        TConstArrayView<FLinearColor> CaptureB, double Threshold, TArray<uint8>& InOutMask);

    // Mark pixels the perturbation itself invalidated, so they are never counted as defects.
    //
    // Moving a clipping plane does not only change the depth encoding: it also changes what
    // is clipped. Raising the near plane clips away geometry closer than the new plane, and
    // introducing a finite far plane clips away geometry beyond it. Either way the perturbed
    // capture legitimately shows whatever was behind the clipped surface — a full surface
    // swap that is an artefact of the measurement, not a defect in the level. Every such
    // pixel is identifiable from the UNPERTURBED capture's depth alone.
    //
    // Depth is the SCS_SceneDepth readback (linear centimetres in R). MinDepth and MaxDepth
    // are the inclusive bounds of the trustworthy range; pass MaxDepth <= 0 for no upper
    // bound. A margin is applied on both sides because a surface sitting exactly on a
    // clipping plane is clipped or not depending on rounding, and a pixel whose status is
    // decided by rounding must not be reported as a defect. Returns the number of pixels
    // excluded.
    int32 ApplyDepthRangeExclusion(TConstArrayView<FLinearColor> Depth, double MinDepth,
        double MaxDepth, TArray<uint8>& InOutMask);

    int32 CountAffected(const TArray<uint8>& Mask);

    // Cluster affected pixels into 8-connected regions, largest first. OutTotalRegions
    // receives the number found before the MaxRegions cap, so a caller can tell "three
    // seams" from "three of ninety".
    void FindRegions(const TArray<uint8>& Mask, int32 Width, int32 Height, int32 MaxRegions,
        TArray<FRegion>& OutRegions, int32& OutTotalRegions);

    // Mask output size: cap the long edge at LongEdge, preserve aspect, never upscale.
    FIntPoint ResolveMaskSize(int32 Width, int32 Height, int32 LongEdge);

    // Render the mask as an opaque bitmap at MaskWidth x MaskHeight. Downscaling is
    // MAX-POOLED: an output pixel is affected if ANY source pixel in its block is. Averaging
    // would fade a one-pixel seam to invisibility, which is the same false-negative failure
    // the analysis resolution exists to prevent.
    //
    // Affected pixels are red, clip-excluded pixels dark blue, clean pixels black, and
    // every pixel is written with alpha 255 — this bitmap is built rather than read back, so
    // it needs no ForceOpaqueAlpha pass, but it must still be opaque to encode as a viewable
    // PNG.
    void BuildMaskBitmap(const TArray<uint8>& Mask, int32 Width, int32 Height,
        int32 MaskWidth, int32 MaskHeight, TArray<FColor>& OutBitmap);
}
