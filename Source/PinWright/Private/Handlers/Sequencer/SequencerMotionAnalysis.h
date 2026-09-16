// Copyright (c) 2026 Alexander Penkin. MIT License.

// SequencerMotionAnalysis.h
//
// Pure analysis math behind sequencer.measure_motion. Operates on the keys and tangents of a
// transform section's double channels and nothing else — no UObject, no world, no playhead, no
// interrogation pipeline — so every rule below is unit-testable headlessly.
//
// WHY ANALYTIC RATHER THAN A DENSE BAKE
//
// Differencing a bake gives clean first derivatives and useless second ones: channel values are
// stored at float32 precision, so the noise floor is divided by dt^2 and then by dt^3. The
// Hermite/Bezier basis the engine itself evaluates gives all three derivatives in closed form from
// numbers already in hand, and the third is CONSTANT within a cubic segment rather than sampled.
//
// The evaluator mirrors UE 5.8's own segment construction verbatim
// (MovieSceneCurveChannelImpl.cpp CacheInterpolationForRange -> FCubicBezierInterpolation):
//
//     DX = Key2.Tick - Key1.Tick                 (ticks, NOT frames and NOT seconds)
//     P0 = Key1.Value
//     P1 = P0 + Key1.LeaveTangent  * DX / 3
//     P2 = P3 - Key2.ArriveTangent * DX / 3
//     P3 = Key2.Value
//
// so a tangent is a slope in CURVE VALUE PER TICK, the same unit the write side takes
// (SequencerKeyInterp.h). A linear segment is (V2-V1)/DX with zero higher derivatives, a constant
// segment is flat, and a linear key followed by a cubic key is promoted to cubic exactly as the
// engine's Sequencer.LinearCubicInterpolation cvar dictates — the caller passes that flag in.
//
// UNITS, STATED ONCE
//
// EvaluateCurve/EvaluateVector return derivatives PER TICK, because that is what the stored data
// is. Everything above them converts once, by multiplying the n-th derivative by
// TicksPerSecond^n, and every struct below is therefore already in per-SECOND units:
//
//     speed                uu/s        acceleration   uu/s^2      jerk    uu/s^3
//     angular rate         deg/s       angular accel  deg/s^2     radius  uu
//
// The tick divide is the single place this verb could be silently wrong by a factor of 24000, so
// it happens in exactly one function (BuildPathSamples / MeasureKeys / MeasureAngular) and nowhere
// else.
//
// WHAT IT CANNOT SEE, STATED PLAINLY
//
//  - The composited world pose. This reads ONE transform section's own channels. Attachment to a
//    moving parent, a second overlapping section blending on top, a camera-rig rail driving the
//    binding — none of it is here. sequencer.get_binding_transform evaluates the composite; it
//    cannot produce a derivative.
//  - Whether the motion is any good. Deliberate deceleration into a subject and a deliberate hard
//    cut are legal configurations of these numbers. This reports the profile and marks outliers;
//    the author decides which are motivated.
//  - Weighted tangents (RCTWM_Weighted*). Those reparameterize the segment and the caller is told
//    the checks are unmeasured rather than handed numbers from the unweighted basis.

#pragma once

#include "CoreMinimal.h"
#include "Curves/RichCurve.h"

namespace PinWrightSequencerMotion
{
    // Fraction of the path's own mean speed under which a loop seam counts as a full stop. A seam
    // is judged by a RATIO against the move it belongs to, not an absolute number, for the same
    // reason the animation gates are: 2 uu/s means one thing on a 6000 uu/s flythrough and
    // another on a 40 uu/s dolly. 2% of the mean is a stop by any reading.
    constexpr double SeamStopSpeedFraction = 0.02;

    // Default relative seam-velocity gate: |v_out - v_in| over the mean of the two seam speeds.
    // 5% of the seam's own speed is a mismatch a viewer reads as a hitch at the one frame a loop
    // shows most often. Overridable; the raw numbers are always reported.
    constexpr double DefaultMaxSeamVelocityMismatch = 0.05;

    // A turn radius this many times the path's own arc length bends the path by under a
    // milliradian across its whole extent, which is a straight line. Dimensionless on purpose:
    // an absolute radius threshold would mean different things at different world scales.
    constexpr double StraightRadiusPathLengthMultiple = 1000.0;

    // Speed floor for a curvature sample, as a fraction of the path's mean speed. Below it the
    // |v|^3 denominator turns float noise into a fabricated hairpin.
    constexpr double CurvatureMinSpeedFraction = 1.0e-3;

    // One authored key of one scalar channel, in the units the channel stores. Tangents are
    // value per TICK (see the file header).
    struct FCurveKey
    {
        int32 Tick = 0;
        double Value = 0.0;
        double ArriveTangent = 0.0;
        double LeaveTangent = 0.0;
        ERichCurveInterpMode Interp = RCIM_Cubic;
    };

    struct FCurveChannel
    {
        TArray<FCurveKey> Keys;
        // Mirrors the engine's Sequencer.LinearCubicInterpolation cvar: with it set, a linear key
        // whose successor is cubic evaluates as cubic. Reading it rather than assuming it is what
        // keeps this evaluator agreeing with what the viewport plays.
        bool bLinearActsAsCubic = true;
    };

    // Three channels addressed as one vector quantity — Location XYZ or Rotation Roll/Pitch/Yaw.
    struct FVectorChannel
    {
        FCurveChannel Axis[3];
    };

    // Which segment a time that lands exactly ON a key belongs to. Cubic Hermite curves are C1
    // and not C2, so the second derivative genuinely differs on the two sides of every key and
    // both one-sided values are real answers rather than one being an artefact.
    enum class ESide : uint8
    {
        Left,
        Right,
    };

    // Value and its first three derivatives with respect to TICKS.
    struct FScalarSample
    {
        double Value = 0.0;
        double D1 = 0.0;
        double D2 = 0.0;
        double D3 = 0.0;
    };

    struct FVectorSample
    {
        FVector Value = FVector::ZeroVector;
        FVector D1 = FVector::ZeroVector;
        FVector D2 = FVector::ZeroVector;
        FVector D3 = FVector::ZeroVector;
    };

    FScalarSample EvaluateCurve(const FCurveChannel& Channel, double Tick, ESide Side);
    FVectorSample EvaluateVector(const FVectorChannel& Channels, double Tick, ESide Side);

    // Sorted union of every axis's key ticks. These are the segment boundaries of the VECTOR
    // quantity: a tick keyed on X but not on Y still starts a new cubic segment for the path as a
    // whole, so "jerk is constant within a segment" is only true against this union.
    void CollectKeyTicks(const FVectorChannel& Channels, TArray<int32>& OutTicks);

    // One grid sample of the location path, already converted to per-second units.
    struct FPathSample
    {
        int32 Tick = 0;
        FVector Position = FVector::ZeroVector;
        FVector Velocity = FVector::ZeroVector;
        FVector Acceleration = FVector::ZeroVector;
        double Speed = 0.0;
    };

    // Sample the location channels on the supplied tick grid. THE tick-to-second conversion:
    // velocity scales by TicksPerSecond, acceleration by its square.
    void BuildPathSamples(const FVectorChannel& Location, TConstArrayView<int32> Ticks,
        double TicksPerSecond, TArray<FPathSample>& OutSamples);

    struct FSpeedStats
    {
        bool bMeasurable = false;
        double Min = 0.0;
        double Max = 0.0;
        double Mean = 0.0;
        // Max / Min. The "stops dead at every key" defect shows here as a ratio that runs away,
        // and as a Min at or near zero.
        double Ratio = 0.0;
        int32 MinTick = 0;
        int32 MaxTick = 0;
        // Arc length of the sampled path, in world units. Also the scale the straightness test
        // below measures a turn radius against.
        double PathLength = 0.0;
    };

    void MeasureSpeed(TConstArrayView<FPathSample> Samples, FSpeedStats& OutStats);

    // One key, measured from BOTH sides. The acceleration step across a key is the C1 seam every
    // cubic curve has by construction, so this is a distribution to read, not a fault to fix.
    struct FKeyMotion
    {
        int32 Tick = 0;
        // False at the first and last key, where the evaluator clamps to the only adjacent
        // segment so both one-sided values are the same limit. Their structural zero step must
        // not enter the distribution below.
        bool bInterior = false;
        double ArriveSpeed = 0.0;
        double LeaveSpeed = 0.0;
        FVector ArriveAcceleration = FVector::ZeroVector;
        FVector LeaveAcceleration = FVector::ZeroVector;
        double AccelerationStep = 0.0;
        // AccelerationStep over the larger of the two one-sided magnitudes. Dimensionless, so a
        // 13000 uu/s^2 step on a 40000 uu/s^2 move reads differently from the same step on a
        // 500 uu/s^2 one.
        double RelativeAccelerationStep = 0.0;
    };

    void MeasureKeys(const FVectorChannel& Location, TConstArrayView<int32> KeyTicks,
        double TicksPerSecond, TArray<FKeyMotion>& OutKeys);

    struct FAccelerationStats
    {
        // False when no key has a segment on each side. That is a measurement about a one-segment
        // move, not a failure to look; the caller decides how to present it.
        bool bMeasurable = false;
        int32 InteriorKeyCount = 0;
        double MedianStep = 0.0;
        double MaxStep = 0.0;
        double MaxRelativeStep = 0.0;
        int32 WorstTick = 0;
    };

    void SummarizeAccelerationSteps(TConstArrayView<FKeyMotion> Keys, FAccelerationStats& OutStats);

    // Jerk is constant within a cubic segment, so this is exact rather than sampled. The duration
    // rides along because jerk scales as 1/h^3: a segment half its neighbours' length carries 8x
    // their jerk for the same authored shape, and that is a re-timing problem, not a re-shaping
    // one. Without the duration column a caller cannot tell the two apart.
    struct FSegmentJerk
    {
        int32 StartTick = 0;
        int32 EndTick = 0;
        double DurationSeconds = 0.0;
        double Magnitude = 0.0;
    };

    void MeasureJerk(const FVectorChannel& Location, TConstArrayView<int32> KeyTicks,
        double TicksPerSecond, TArray<FSegmentJerk>& OutSegments);

    struct FJerkStats
    {
        bool bMeasurable = false;
        double Median = 0.0;
        double Max = 0.0;
        int32 MaxStartTick = 0;
        // The worst segment's own duration, carried beside its magnitude precisely because the
        // two are only meaningful together (see FSegmentJerk).
        double MaxDurationSeconds = 0.0;
    };

    void SummarizeJerk(TConstArrayView<FSegmentJerk> Segments, FJerkStats& OutStats);

    struct FCurvatureStats
    {
        // False when no sample cleared the speed floor, so every radius would be |v|^3 noise.
        bool bMeasurable = false;
        // True when the tightest radius found still exceeds StraightRadiusPathLengthMultiple
        // times the path's arc length. A straight path has no turn to report and passes any
        // minimum-radius gate correctly rather than being called unmeasured.
        bool bStraight = false;
        double MinRadius = 0.0;
        int32 MinRadiusTick = 0;
        double SpeedAtMinRadius = 0.0;
        // The acceleration at the tightest corner, split along and across the direction of
        // travel. Tangential is the speed changing; normal is the path bending. Only the second
        // is what a hairpin costs.
        double TangentialAccelerationAtMinRadius = 0.0;
        double NormalAccelerationAtMinRadius = 0.0;
    };

    void MeasureCurvature(TConstArrayView<FPathSample> Samples, double ReferenceSpeed,
        double PathLength, FCurvatureStats& OutStats);

    struct FAngularAxisStats
    {
        double MaxRate = 0.0;
        int32 MaxRateTick = 0;
        double MaxAcceleration = 0.0;
        int32 MaxAccelerationTick = 0;
    };

    // Roll / Pitch / Yaw, matching the transform section's channel order 3-5.
    struct FAngularStats
    {
        bool bMeasurable = false;
        FAngularAxisStats Axis[3];
    };

    void MeasureAngular(const FVectorChannel& Rotation, TConstArrayView<int32> Ticks,
        double TicksPerSecond, FAngularStats& OutStats);

    struct FSeamStats
    {
        bool bMeasurable = false;
        // One-sided velocities: the left limit at the last tick, the right limit at the first.
        // Exact, because the basis is analytic — there is no sampling step to converge.
        FVector ArriveVelocity = FVector::ZeroVector;
        FVector LeaveVelocity = FVector::ZeroVector;
        double ArriveSpeed = 0.0;
        double LeaveSpeed = 0.0;
        double Mismatch = 0.0;
        double RelativeMismatch = 0.0;
        // |P(end) - P(start)|. Reported, never gated: a flythrough that deliberately ends
        // elsewhere is not a defect, but a caller calling the move a loop needs to see it.
        double PositionGap = 0.0;
        // Largest per-axis rotation-rate mismatch across the seam, in deg/s.
        double AngularMismatch = 0.0;
        // Both one-sided speeds under SeamStopSpeedFraction of the path's mean. THE reason a bare
        // mismatch number is not enough: a camera that halts on both sides of the seam has a
        // mismatch of zero and is the exact defect a seam check exists to catch.
        bool bStopsDead = false;
    };

    void MeasureLoopSeam(const FVectorChannel& Location, const FVectorChannel& Rotation,
        int32 StartTick, int32 EndTick, double TicksPerSecond, double ReferenceSpeed,
        FSeamStats& OutStats);
}
