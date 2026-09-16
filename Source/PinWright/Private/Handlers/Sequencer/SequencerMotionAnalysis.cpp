// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Sequencer/SequencerMotionAnalysis.h"

// Median, reused rather than re-typed. MotionMetricsAnalysis.h's own header comment states that
// its math is meant to be fed from a Sequencer transform track as well as an AnimSequence, and the
// argument for a median over a mean is identical here: a single authored spike is exactly what
// these summaries are trying to surface, and a mean denominator absorbs it.
#include "Handlers/Animation/MotionMetricsAnalysis.h"

namespace PinWrightSequencerMotion
{
namespace
{
    // Index of the START key of the segment that owns InTick. Binary search rather than a scan:
    // a dense grid over a densely keyed channel is O(samples * keys) otherwise.
    int32 FindSegmentIndex(const TArray<FCurveKey>& Keys, double InTick, ESide Side)
    {
        const int32 Num = Keys.Num();
        if (Num < 2)
        {
            return INDEX_NONE;
        }
        if (InTick <= static_cast<double>(Keys[0].Tick))
        {
            return 0;
        }
        if (InTick >= static_cast<double>(Keys[Num - 1].Tick))
        {
            return Num - 2;
        }

        int32 Lo = 0;
        int32 Hi = Num - 1;
        while (Lo < Hi)
        {
            const int32 Mid = (Lo + Hi + 1) / 2;
            if (static_cast<double>(Keys[Mid].Tick) <= InTick)
            {
                Lo = Mid;
            }
            else
            {
                Hi = Mid - 1;
            }
        }
        // Lo is the last key at or before InTick. Landing exactly on a key, the left-hand limit
        // belongs to the segment that ENDS there.
        if (Side == ESide::Left && Lo > 0 && static_cast<double>(Keys[Lo].Tick) == InTick)
        {
            --Lo;
        }
        return FMath::Min(Lo, Num - 2);
    }
}

void CollectKeyTicks(const FVectorChannel& Channels, TArray<int32>& OutTicks)
{
    OutTicks.Reset();
    for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
    {
        for (const FCurveKey& Key : Channels.Axis[AxisIndex].Keys)
        {
            OutTicks.AddUnique(Key.Tick);
        }
    }
    OutTicks.Sort();
}

FScalarSample EvaluateCurve(const FCurveChannel& Channel, double InTick, ESide Side)
{
    FScalarSample Out;
    const TArray<FCurveKey>& Keys = Channel.Keys;
    if (Keys.Num() == 0)
    {
        return Out;
    }
    if (Keys.Num() == 1)
    {
        Out.Value = Keys[0].Value;
        return Out;
    }
    // Outside the keyed range a MovieScene curve channel holds constant (PreInfinityExtrap and
    // PostInfinityExtrap both default to RCCE_Constant), so every derivative there is a true zero.
    // Clamping into the nearest segment instead would report the endpoint's slope continuing
    // forever, which on a track whose rotation is keyed past its location invents motion that the
    // viewport does not play. Strict comparisons: a time landing exactly ON an endpoint key is
    // inside the range and belongs to the adjacent segment.
    if (InTick < static_cast<double>(Keys[0].Tick))
    {
        Out.Value = Keys[0].Value;
        return Out;
    }
    if (InTick > static_cast<double>(Keys.Last().Tick))
    {
        Out.Value = Keys.Last().Value;
        return Out;
    }

    const int32 Index = FindSegmentIndex(Keys, InTick, Side);
    if (Index == INDEX_NONE)
    {
        Out.Value = Keys[0].Value;
        return Out;
    }

    const FCurveKey& Key1 = Keys[Index];
    const FCurveKey& Key2 = Keys[Index + 1];
    const double DX = static_cast<double>(Key2.Tick) - static_cast<double>(Key1.Tick);
    if (!(DX > 0.0))
    {
        // Two keys on the same tick: no segment, so no slope exists to report.
        Out.Value = Key2.Value;
        return Out;
    }
    const double U = FMath::Clamp((InTick - static_cast<double>(Key1.Tick)) / DX, 0.0, 1.0);

    ERichCurveInterpMode Mode = Key1.Interp;
    if (Mode == RCIM_Linear && Channel.bLinearActsAsCubic && Key2.Interp == RCIM_Cubic)
    {
        Mode = RCIM_Cubic;
    }

    if (Mode == RCIM_Constant)
    {
        Out.Value = Key1.Value;
        return Out;
    }
    if (Mode == RCIM_Linear)
    {
        const double DY = Key2.Value - Key1.Value;
        Out.Value = Key1.Value + DY * U;
        Out.D1 = DY / DX;
        return Out;
    }

    // Cubic: the same four control points FCubicBezierInterpolation builds.
    constexpr double OneThird = 1.0 / 3.0;
    const double P0 = Key1.Value;
    const double P1 = P0 + Key1.LeaveTangent * DX * OneThird;
    const double P3 = Key2.Value;
    const double P2 = P3 - Key2.ArriveTangent * DX * OneThird;

    const double OneMinusU = 1.0 - U;
    Out.Value = OneMinusU * OneMinusU * OneMinusU * P0
        + 3.0 * OneMinusU * OneMinusU * U * P1
        + 3.0 * OneMinusU * U * U * P2
        + U * U * U * P3;

    // Bezier derivatives with respect to the normalized parameter, then divided by DX^n to land
    // back in per-tick units. The third is independent of U, which is what makes segment jerk
    // exact rather than sampled.
    const double B1 = 3.0 * (OneMinusU * OneMinusU * (P1 - P0)
        + 2.0 * OneMinusU * U * (P2 - P1)
        + U * U * (P3 - P2));
    const double B2 = 6.0 * (OneMinusU * (P2 - 2.0 * P1 + P0) + U * (P3 - 2.0 * P2 + P1));
    const double B3 = 6.0 * (P3 - 3.0 * P2 + 3.0 * P1 - P0);

    Out.D1 = B1 / DX;
    Out.D2 = B2 / (DX * DX);
    Out.D3 = B3 / (DX * DX * DX);
    return Out;
}

FVectorSample EvaluateVector(const FVectorChannel& Channels, double InTick, ESide Side)
{
    FVectorSample Out;
    for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
    {
        const FScalarSample Sample = EvaluateCurve(Channels.Axis[AxisIndex], InTick, Side);
        Out.Value[AxisIndex] = Sample.Value;
        Out.D1[AxisIndex] = Sample.D1;
        Out.D2[AxisIndex] = Sample.D2;
        Out.D3[AxisIndex] = Sample.D3;
    }
    return Out;
}

void BuildPathSamples(const FVectorChannel& Location, TConstArrayView<int32> Ticks,
    double TicksPerSecond, TArray<FPathSample>& OutSamples)
{
    OutSamples.Reset();
    OutSamples.Reserve(Ticks.Num());
    const double TicksPerSecondSquared = TicksPerSecond * TicksPerSecond;
    for (const int32 Tick : Ticks)
    {
        const FVectorSample Raw = EvaluateVector(Location, static_cast<double>(Tick), ESide::Right);
        FPathSample& Sample = OutSamples.AddDefaulted_GetRef();
        Sample.Tick = Tick;
        Sample.Position = Raw.Value;
        Sample.Velocity = Raw.D1 * TicksPerSecond;
        Sample.Acceleration = Raw.D2 * TicksPerSecondSquared;
        Sample.Speed = Sample.Velocity.Size();
    }
}

void MeasureSpeed(TConstArrayView<FPathSample> Samples, FSpeedStats& OutStats)
{
    OutStats = FSpeedStats();
    if (Samples.Num() < 2)
    {
        return;
    }

    OutStats.bMeasurable = true;
    OutStats.Min = Samples[0].Speed;
    OutStats.Max = Samples[0].Speed;
    OutStats.MinTick = Samples[0].Tick;
    OutStats.MaxTick = Samples[0].Tick;
    double Total = 0.0;
    for (int32 Index = 0; Index < Samples.Num(); ++Index)
    {
        const FPathSample& Sample = Samples[Index];
        Total += Sample.Speed;
        if (Sample.Speed < OutStats.Min)
        {
            OutStats.Min = Sample.Speed;
            OutStats.MinTick = Sample.Tick;
        }
        if (Sample.Speed > OutStats.Max)
        {
            OutStats.Max = Sample.Speed;
            OutStats.MaxTick = Sample.Tick;
        }
        if (Index > 0)
        {
            OutStats.PathLength += (Sample.Position - Samples[Index - 1].Position).Size();
        }
    }
    OutStats.Mean = Total / static_cast<double>(Samples.Num());
    // A Min of zero has no ratio; reporting the Min itself is the honest signal there.
    OutStats.Ratio = OutStats.Min > 0.0 ? OutStats.Max / OutStats.Min : 0.0;
}

void MeasureKeys(const FVectorChannel& Location, TConstArrayView<int32> KeyTicks,
    double TicksPerSecond, TArray<FKeyMotion>& OutKeys)
{
    OutKeys.Reset();
    OutKeys.Reserve(KeyTicks.Num());
    const double TicksPerSecondSquared = TicksPerSecond * TicksPerSecond;
    for (int32 Index = 0; Index < KeyTicks.Num(); ++Index)
    {
        const double Tick = static_cast<double>(KeyTicks[Index]);
        const FVectorSample Arrive = EvaluateVector(Location, Tick, ESide::Left);
        const FVectorSample Leave = EvaluateVector(Location, Tick, ESide::Right);

        FKeyMotion& Key = OutKeys.AddDefaulted_GetRef();
        Key.Tick = KeyTicks[Index];
        // At the first and last key the evaluator clamps to the only adjacent segment, so both
        // "sides" are the same limit and the step is a true zero rather than a missing number.
        Key.bInterior = Index > 0 && Index + 1 < KeyTicks.Num();
        Key.ArriveSpeed = (Arrive.D1 * TicksPerSecond).Size();
        Key.LeaveSpeed = (Leave.D1 * TicksPerSecond).Size();
        Key.ArriveAcceleration = Arrive.D2 * TicksPerSecondSquared;
        Key.LeaveAcceleration = Leave.D2 * TicksPerSecondSquared;
        Key.AccelerationStep = (Key.LeaveAcceleration - Key.ArriveAcceleration).Size();
        const double Local = FMath::Max(Key.ArriveAcceleration.Size(), Key.LeaveAcceleration.Size());
        Key.RelativeAccelerationStep = Local > 0.0 ? Key.AccelerationStep / Local : 0.0;
    }
}

void SummarizeAccelerationSteps(TConstArrayView<FKeyMotion> Keys, FAccelerationStats& OutStats)
{
    OutStats = FAccelerationStats();
    TArray<double> Steps;
    for (const FKeyMotion& Key : Keys)
    {
        // Endpoints contribute a structural zero that would drag the median toward nothing.
        if (!Key.bInterior)
        {
            continue;
        }
        ++OutStats.InteriorKeyCount;
        if (OutStats.InteriorKeyCount == 1)
        {
            // Seeded from the first interior key rather than left at zero: a curve whose steps are
            // all exactly zero would otherwise report its worst step at tick 0, a frame that may
            // not carry a key at all.
            OutStats.WorstTick = Key.Tick;
        }
        Steps.Add(Key.AccelerationStep);
        if (Key.AccelerationStep > OutStats.MaxStep)
        {
            OutStats.MaxStep = Key.AccelerationStep;
            OutStats.WorstTick = Key.Tick;
        }
        OutStats.MaxRelativeStep = FMath::Max(OutStats.MaxRelativeStep, Key.RelativeAccelerationStep);
    }
    if (Steps.Num() == 0)
    {
        return;
    }
    OutStats.bMeasurable = true;
    OutStats.MedianStep = PinWrightMotionMetrics::Median(MoveTemp(Steps));
}

void MeasureJerk(const FVectorChannel& Location, TConstArrayView<int32> KeyTicks,
    double TicksPerSecond, TArray<FSegmentJerk>& OutSegments)
{
    OutSegments.Reset();
    if (KeyTicks.Num() < 2)
    {
        return;
    }
    const double TicksPerSecondCubed = TicksPerSecond * TicksPerSecond * TicksPerSecond;
    for (int32 Index = 0; Index + 1 < KeyTicks.Num(); ++Index)
    {
        const int32 StartTick = KeyTicks[Index];
        const int32 EndTick = KeyTicks[Index + 1];
        if (EndTick <= StartTick)
        {
            continue;
        }
        // Constant across the segment, so any interior point gives the exact value; the start
        // read from the right is the segment's own.
        const FVectorSample Sample = EvaluateVector(Location, static_cast<double>(StartTick), ESide::Right);
        FSegmentJerk& Segment = OutSegments.AddDefaulted_GetRef();
        Segment.StartTick = StartTick;
        Segment.EndTick = EndTick;
        Segment.DurationSeconds = static_cast<double>(EndTick - StartTick) / TicksPerSecond;
        Segment.Magnitude = (Sample.D3 * TicksPerSecondCubed).Size();
    }
}

void SummarizeJerk(TConstArrayView<FSegmentJerk> Segments, FJerkStats& OutStats)
{
    OutStats = FJerkStats();
    if (Segments.Num() == 0)
    {
        return;
    }
    OutStats.bMeasurable = true;
    OutStats.MaxStartTick = Segments[0].StartTick;
    OutStats.MaxDurationSeconds = Segments[0].DurationSeconds;
    TArray<double> Magnitudes;
    Magnitudes.Reserve(Segments.Num());
    for (const FSegmentJerk& Segment : Segments)
    {
        Magnitudes.Add(Segment.Magnitude);
        if (Segment.Magnitude > OutStats.Max)
        {
            OutStats.Max = Segment.Magnitude;
            OutStats.MaxStartTick = Segment.StartTick;
            OutStats.MaxDurationSeconds = Segment.DurationSeconds;
        }
    }
    OutStats.Median = PinWrightMotionMetrics::Median(MoveTemp(Magnitudes));
}

void MeasureCurvature(TConstArrayView<FPathSample> Samples, double ReferenceSpeed,
    double PathLength, FCurvatureStats& OutStats)
{
    OutStats = FCurvatureStats();
    const double SpeedFloor = ReferenceSpeed * CurvatureMinSpeedFraction;
    if (!(SpeedFloor > 0.0))
    {
        return;
    }

    double BestRadius = 0.0;
    for (const FPathSample& Sample : Samples)
    {
        if (Sample.Speed <= SpeedFloor)
        {
            continue;
        }
        OutStats.bMeasurable = true;
        // kappa = |v x a| / |v|^3, so radius = |v|^3 / |v x a|. Invariant to how the curve is
        // parameterized, which is why per-second versus per-tick cannot change this number.
        const double CrossSize = FVector::CrossProduct(Sample.Velocity, Sample.Acceleration).Size();
        if (!(CrossSize > 0.0))
        {
            continue;
        }
        const double Radius = (Sample.Speed * Sample.Speed * Sample.Speed) / CrossSize;
        if (BestRadius <= 0.0 || Radius < BestRadius)
        {
            BestRadius = Radius;
            OutStats.MinRadius = Radius;
            OutStats.MinRadiusTick = Sample.Tick;
            OutStats.SpeedAtMinRadius = Sample.Speed;
            const FVector Direction = Sample.Velocity / Sample.Speed;
            OutStats.TangentialAccelerationAtMinRadius =
                FVector::DotProduct(Sample.Acceleration, Direction);
            OutStats.NormalAccelerationAtMinRadius = CrossSize / Sample.Speed;
        }
    }

    if (!OutStats.bMeasurable)
    {
        return;
    }
    if (BestRadius <= 0.0 ||
        (PathLength > 0.0 && BestRadius > StraightRadiusPathLengthMultiple * PathLength))
    {
        OutStats.bStraight = true;
    }
}

void MeasureAngular(const FVectorChannel& Rotation, TConstArrayView<int32> Ticks,
    double TicksPerSecond, FAngularStats& OutStats)
{
    OutStats = FAngularStats();
    if (Ticks.Num() < 2)
    {
        return;
    }
    const double TicksPerSecondSquared = TicksPerSecond * TicksPerSecond;
    for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
    {
        if (Rotation.Axis[AxisIndex].Keys.Num() < 2)
        {
            continue;
        }
        OutStats.bMeasurable = true;
        FAngularAxisStats& Axis = OutStats.Axis[AxisIndex];
        for (const int32 Tick : Ticks)
        {
            const FScalarSample Sample =
                EvaluateCurve(Rotation.Axis[AxisIndex], static_cast<double>(Tick), ESide::Right);
            const double Rate = FMath::Abs(Sample.D1 * TicksPerSecond);
            const double Acceleration = FMath::Abs(Sample.D2 * TicksPerSecondSquared);
            if (Rate > Axis.MaxRate)
            {
                Axis.MaxRate = Rate;
                Axis.MaxRateTick = Tick;
            }
            if (Acceleration > Axis.MaxAcceleration)
            {
                Axis.MaxAcceleration = Acceleration;
                Axis.MaxAccelerationTick = Tick;
            }
        }
    }
}

void MeasureLoopSeam(const FVectorChannel& Location, const FVectorChannel& Rotation,
    int32 StartTick, int32 EndTick, double TicksPerSecond, double ReferenceSpeed,
    FSeamStats& OutStats)
{
    OutStats = FSeamStats();
    if (EndTick <= StartTick)
    {
        return;
    }
    TArray<int32> LocationTicks;
    CollectKeyTicks(Location, LocationTicks);
    if (LocationTicks.Num() < 2)
    {
        return;
    }

    const FVectorSample Arrive = EvaluateVector(Location, static_cast<double>(EndTick), ESide::Left);
    const FVectorSample Leave = EvaluateVector(Location, static_cast<double>(StartTick), ESide::Right);

    OutStats.bMeasurable = true;
    OutStats.ArriveVelocity = Arrive.D1 * TicksPerSecond;
    OutStats.LeaveVelocity = Leave.D1 * TicksPerSecond;
    OutStats.ArriveSpeed = OutStats.ArriveVelocity.Size();
    OutStats.LeaveSpeed = OutStats.LeaveVelocity.Size();
    OutStats.Mismatch = (OutStats.LeaveVelocity - OutStats.ArriveVelocity).Size();
    OutStats.PositionGap = (Leave.Value - Arrive.Value).Size();

    const double MeanSeamSpeed = 0.5 * (OutStats.ArriveSpeed + OutStats.LeaveSpeed);
    OutStats.RelativeMismatch = MeanSeamSpeed > 0.0 ? OutStats.Mismatch / MeanSeamSpeed : 0.0;

    for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
    {
        if (Rotation.Axis[AxisIndex].Keys.Num() < 2)
        {
            continue;
        }
        const double ArriveRate = EvaluateCurve(Rotation.Axis[AxisIndex],
            static_cast<double>(EndTick), ESide::Left).D1 * TicksPerSecond;
        const double LeaveRate = EvaluateCurve(Rotation.Axis[AxisIndex],
            static_cast<double>(StartTick), ESide::Right).D1 * TicksPerSecond;
        OutStats.AngularMismatch =
            FMath::Max(OutStats.AngularMismatch, FMath::Abs(LeaveRate - ArriveRate));
    }

    // The reason a mismatch alone proves nothing: a camera stopped on both sides matches
    // perfectly. Judged against the whole move's mean speed, not against the seam's own.
    if (ReferenceSpeed > 0.0)
    {
        const double StopThreshold = ReferenceSpeed * SeamStopSpeedFraction;
        OutStats.bStopsDead = OutStats.ArriveSpeed < StopThreshold && OutStats.LeaveSpeed < StopThreshold;
    }
}
}
