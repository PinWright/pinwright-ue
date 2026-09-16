// Copyright (c) 2026 Alexander Penkin. MIT License.

// MotionMetricsAnalysis.h
//
// Pure analysis math behind animation.measure_motion. Operates on sampled component-space bone
// transforms and nothing else — no UObject, no world, no component, no rendering — so every
// rule below is unit-testable headlessly and the same math can later be fed from a Sequencer
// transform track instead of an AnimSequence without touching a line of it.
//
// THE CALIBRATION IDEA THAT MAKES THIS WORK
//
// Absolute thresholds on animation are indefensible. "More than 3 cm of foot slide" means one
// thing on a 180 cm human and another on a 12 m crane; "more than 2 degrees of jitter"
// means one thing at 30 fps and another at 120. So almost nothing here is measured in world
// units. Every gate below is a RATIO between two quantities taken from the same animation, and
// the denominator is the animation's own typical per-frame motion:
//
//   seamRatio   = (pose delta across the loop seam) / (median pose delta between adjacent frames)
//   jitterRatio = (second difference at a frame)    / (median first difference)
//
// Both are dimensionless, both are invariant to character scale, frame rate and play rate, and
// both have a value that means something concrete rather than something tuned:
//
//   * seamRatio near 0 means the last key duplicates the first — a cycle authored with an
//     explicit wrap key. seamRatio near 1 means the last key sits one normal step before the
//     wrap — the other common convention. Both are correct. A ratio of 5 means five frames of
//     travel arrive in one frame at the wrap, which is a visible pop.
//   * jitterRatio of 2 is not a taste call: a second difference twice the first difference is
//     exactly the signature of a curve that REVERSES DIRECTION every frame. That is noise by
//     definition, not motion. Below 2 the curve is still going somewhere between samples.
//
// The locomotion block has no threshold at all. It derives a physical quantity — the ground
// speed the animation's own footfalls imply — and a caller compares it with the speed the
// character is actually being translated at. The tolerance in that comparison is the caller's
// statement about their game, not this file's guess about animation.
//
// WHAT IT CANNOT SEE, STATED PLAINLY
//
//  - Whether the motion is any good. Weight, anticipation, follow-through, appeal and timing
//    charm are all legal configurations of numbers that pass every check here.
//  - Whether an animation is BOUND to anything. This reads an asset. An animation that is
//    perfect and simply not attached to the actor in the level scores identically to one that
//    is; that defect is structural and sequencer.list_tracks is what finds it.
//  - Whether the animation suits the character. A human walk on a six-legged insect passes.
//    The locomotion block will however report a stride that does not match the body speed,
//    which is usually how a wrong choice of animation actually shows up.
//  - Anything about the world: ground penetration, collision, other actors.

#pragma once

#include "CoreMinimal.h"

namespace PinWrightMotionMetrics
{
    // Below this a bone is treated as not moving at all. One thousandth of a world unit is far
    // under any authored motion and far over float accumulation error through a component-space
    // transform chain, so the two cases never overlap.
    constexpr double StaticLocationEpsilon = 1.0e-3;

    // Same idea in degrees.
    constexpr double StaticRotationEpsilon = 1.0e-3;

    // Loop-seam gate, in units of "normal frame steps". Both common authoring conventions land
    // at 0 (explicit duplicate wrap key) or 1 (last key one step before the wrap), so the gate
    // has to sit above 1 with room to spare; five frames of travel arriving in one frame is a
    // pop nobody argues about. Overridable, and the raw ratio is always reported so a caller
    // who disagrees can pick their own line from the same number.
    constexpr double DefaultMaxSeamRatio = 3.0;

    // Second difference over median first difference at which a frame counts as jittering. A
    // curve whose second difference reaches twice its first difference reverses direction
    // between every pair of samples, which is the definition of noise rather than motion. This
    // is a per-frame classifier, not a verdict; see FJitterStats::FrameFraction.
    constexpr double JitterFrameRatio = 2.0;

    // Fraction of the foot's total vertical travel, above its lowest point, still counted as
    // planted. A foot is not perfectly still during stance — the ankle rolls and the heel
    // lifts before toe-off — so a zero-width band finds no stance at all on real animation.
    // 15% is wide enough to hold a heel-to-toe roll and narrow enough to exclude the swing.
    constexpr double DefaultPlantBandFraction = 0.15;

    // Vertical travel under which a bone is not doing locomotion at all. A foot that never
    // rises has no stance to separate from a swing, so the locomotion block reports unmeasured
    // rather than inventing a stance window out of noise.
    constexpr double MinFootHeightRange = 0.5;

    // One bone's component-space motion over the sampled frames. Sampling is the caller's
    // business; this file only requires that the samples are evenly spaced in time.
    struct FBoneTrack
    {
        FName BoneName;
        TArray<FVector> Locations;
        TArray<FQuat> Rotations;
    };

    // Angular distance between two orientations, in degrees, always in [0, 180].
    double AngleDegrees(const FQuat& A, const FQuat& B);

    // Median of a copy of Values. Median rather than mean throughout: a single authored pop is
    // exactly what these ratios are trying to detect, and a mean denominator would absorb the
    // very spike the numerator is measuring. Returns 0 for an empty array.
    double Median(TArray<double> Values);

    // ---- Check 1: is anything moving at all ----

    struct FMotionExtent
    {
        FName BoneName;
        double MaxLocationDelta = 0.0;    // largest distance from the first sample
        double MaxRotationDegrees = 0.0;
    };

    struct FFrozenStats
    {
        int32 TrackCount = 0;
        int32 SampleCount = 0;
        int32 MovingBoneCount = 0;
        double MaxLocationDelta = 0.0;
        double MaxRotationDegrees = 0.0;
        FName MostMovedBone;
        TArray<FMotionExtent> Extents;   // most-moved first, capped by the caller
    };

    // Largest departure from the first sample, per bone and overall. Measured against the FIRST
    // SAMPLE rather than between adjacent frames on purpose: an animation that oscillates by a
    // hundredth of a unit has adjacent-frame motion but is still, for review purposes, frozen.
    void AccumulateMotionExtent(TConstArrayView<FBoneTrack> Tracks, int32 MaxReported,
        FFrozenStats& OutStats);

    // ---- Check 2: loop seam ----

    struct FSeamStats
    {
        // False when no bone has a measurable per-frame step, so there is no denominator and
        // every ratio below would be a fabrication.
        bool bMeasurable = false;
        int32 ComparedBoneCount = 0;
        double LocationDelta = 0.0;        // worst bone's first-to-last location gap
        double MedianStepLocation = 0.0;   // that bone's median adjacent-frame step
        double LocationRatio = 0.0;
        double RotationDelta = 0.0;        // degrees
        double MedianStepRotation = 0.0;
        double RotationRatio = 0.0;
        FName WorstBone;
    };

    // Compares the first and last sample against the animation's own typical frame step. Only
    // bones whose median step exceeds the static epsilon take part: a bone that never moves has
    // a zero seam gap over a zero step, which is 0/0 and must not be reported as a clean pass
    // for the seam as a whole.
    void MeasureLoopSeam(TConstArrayView<FBoneTrack> Tracks, FSeamStats& OutStats);

    // ---- Check 3: jitter ----

    struct FJitterStats
    {
        bool bMeasurable = false;
        int32 ComparedBoneCount = 0;
        int32 EvaluatedSamples = 0;
        int32 JitteringSamples = 0;
        // Fraction of interior samples whose second difference exceeded JitterFrameRatio times
        // the bone's median first difference. Noise drives this toward 1; a clean cycle with
        // genuine velocity discontinuities at footfall sits well under 0.1.
        double FrameFraction = 0.0;
        double MaxRatio = 0.0;
        FName WorstBone;
        int32 WorstSample = INDEX_NONE;
    };

    // Second difference of location per interior sample, normalized by that bone's median first
    // difference. Needs at least three samples; fewer leaves bMeasurable false.
    void MeasureJitter(TConstArrayView<FBoneTrack> Tracks, FJitterStats& OutStats);

    // ---- Check 4: locomotion ----

    struct FLocomotionStats
    {
        bool bMeasurable = false;
        FName BoneName;
        double MinHeight = 0.0;
        double MaxHeight = 0.0;
        double HeightRange = 0.0;
        double PlantBandHeight = 0.0;
        int32 StanceStartSample = INDEX_NONE;
        int32 StanceEndSample = INDEX_NONE;
        int32 StanceSampleCount = 0;
        // Whether the stance run wrapped around the end of the sample array. Only attempted
        // when the caller says the animation loops; on a non-looping clip a wrap would join two
        // unrelated stances.
        bool bStanceWrapped = false;
        // Whether the final sample was discarded as a duplicate wrap key. UE stores
        // NumberOfFrames + 1 keys and a seamless cycle repeats the first pose in the last key;
        // that duplicate is a position, not an extra interval of time, and counting it deflates
        // every derived speed by N/(N+1).
        bool bDroppedWrapSample = false;
        double StanceSeconds = 0.0;
        // Duration of one full cycle over the samples actually used.
        double CycleSeconds = 0.0;
        // Horizontal distance the planted foot travels in COMPONENT space during stance. On an
        // in-place cycle this is the ground sliding past under a stationary root, so it equals
        // the distance the character should cover in that time.
        double StanceDisplacement = 0.0;
        // StanceDisplacement / StanceSeconds. The speed at which this animation, played at rate
        // 1.0, wants the character to travel. Compare against the actual translation speed:
        // the difference IS the foot slide.
        double ImpliedGroundSpeed = 0.0;
        // ImpliedGroundSpeed x the full sampled duration: how far one cycle carries the body.
        double StridePerCycle = 0.0;
    };

    // Derives the stance window from the foot bone's own height minima and measures how far it
    // travels while planted. SampleSeconds is the wall-clock interval between adjacent samples.
    // bLooping allows the stance run to wrap the array boundary, which is correct for a cycle
    // and wrong for a one-shot, so it is the caller's declaration rather than a guess.
    //
    // Leaves bMeasurable false, with nothing else populated, when the foot's vertical travel is
    // below MinFootHeightRange — there is then no stance to find and any number produced would
    // be an artefact of noise.
    void MeasureLocomotion(const FBoneTrack& FootTrack, double SampleSeconds, bool bLooping,
        double PlantBandFraction, FLocomotionStats& OutStats);
}
