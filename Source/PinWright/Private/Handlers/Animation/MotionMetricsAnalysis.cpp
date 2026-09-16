// Copyright (c) 2026 Alexander Penkin. MIT License.

// Implementation of the pure motion-metrics math. See MotionMetricsAnalysis.h for why every
// gate here is a ratio against the animation's own frame step rather than an absolute number.

#include "Handlers/Animation/MotionMetricsAnalysis.h"

#include "Algo/Sort.h"

namespace PinWrightMotionMetrics
{
namespace
{
    // Median first difference of a location track, and the count of usable steps.
    double MedianLocationStep(const FBoneTrack& Track)
    {
        TArray<double> Steps;
        Steps.Reserve(FMath::Max(0, Track.Locations.Num() - 1));
        for (int32 Index = 1; Index < Track.Locations.Num(); ++Index)
        {
            Steps.Add(FVector::Dist(Track.Locations[Index - 1], Track.Locations[Index]));
        }
        return Median(MoveTemp(Steps));
    }

    double MedianRotationStep(const FBoneTrack& Track)
    {
        TArray<double> Steps;
        Steps.Reserve(FMath::Max(0, Track.Rotations.Num() - 1));
        for (int32 Index = 1; Index < Track.Rotations.Num(); ++Index)
        {
            Steps.Add(AngleDegrees(Track.Rotations[Index - 1], Track.Rotations[Index]));
        }
        return Median(MoveTemp(Steps));
    }
}

double AngleDegrees(const FQuat& A, const FQuat& B)
{
    // FQuat::Error-style dot, clamped: a dot marginally outside [-1,1] from accumulated float
    // error makes Acos return NaN, which would then poison every median and ratio downstream.
    const double Dot = FMath::Clamp(FMath::Abs(A | B), -1.0, 1.0);
    return FMath::RadiansToDegrees(2.0 * FMath::Acos(Dot));
}

double Median(TArray<double> Values)
{
    if (Values.Num() == 0)
    {
        return 0.0;
    }
    Algo::Sort(Values);
    return Values[Values.Num() / 2];
}

void AccumulateMotionExtent(TConstArrayView<FBoneTrack> Tracks, int32 MaxReported,
    FFrozenStats& OutStats)
{
    OutStats = FFrozenStats();
    OutStats.TrackCount = Tracks.Num();

    TArray<FMotionExtent> Extents;
    for (const FBoneTrack& Track : Tracks)
    {
        OutStats.SampleCount = FMath::Max(OutStats.SampleCount, Track.Locations.Num());

        FMotionExtent Extent;
        Extent.BoneName = Track.BoneName;
        for (int32 Index = 1; Index < Track.Locations.Num(); ++Index)
        {
            Extent.MaxLocationDelta = FMath::Max(Extent.MaxLocationDelta,
                FVector::Dist(Track.Locations[0], Track.Locations[Index]));
        }
        for (int32 Index = 1; Index < Track.Rotations.Num(); ++Index)
        {
            Extent.MaxRotationDegrees = FMath::Max(Extent.MaxRotationDegrees,
                AngleDegrees(Track.Rotations[0], Track.Rotations[Index]));
        }

        if (Extent.MaxLocationDelta > StaticLocationEpsilon
            || Extent.MaxRotationDegrees > StaticRotationEpsilon)
        {
            ++OutStats.MovingBoneCount;
        }
        if (Extent.MaxLocationDelta > OutStats.MaxLocationDelta
            || Extent.MaxRotationDegrees > OutStats.MaxRotationDegrees)
        {
            if (Extent.MaxLocationDelta > OutStats.MaxLocationDelta)
            {
                OutStats.MaxLocationDelta = Extent.MaxLocationDelta;
                OutStats.MostMovedBone = Track.BoneName;
            }
            OutStats.MaxRotationDegrees = FMath::Max(OutStats.MaxRotationDegrees, Extent.MaxRotationDegrees);
        }
        Extents.Add(Extent);
    }

    Algo::Sort(Extents, [](const FMotionExtent& Lhs, const FMotionExtent& Rhs)
    {
        if (Lhs.MaxLocationDelta != Rhs.MaxLocationDelta)
        {
            return Lhs.MaxLocationDelta > Rhs.MaxLocationDelta;
        }
        return Lhs.MaxRotationDegrees > Rhs.MaxRotationDegrees;
    });
    const int32 Keep = FMath::Min(Extents.Num(), FMath::Max(0, MaxReported));
    OutStats.Extents.Append(Extents.GetData(), Keep);
}

void MeasureLoopSeam(TConstArrayView<FBoneTrack> Tracks, FSeamStats& OutStats)
{
    OutStats = FSeamStats();

    for (const FBoneTrack& Track : Tracks)
    {
        if (Track.Locations.Num() < 2)
        {
            continue;
        }
        const double StepLocation = MedianLocationStep(Track);
        const double StepRotation = MedianRotationStep(Track);
        // A bone that never moves has a zero seam gap over a zero step. Including it would
        // contribute a meaningless 0/0 that, read as 0, looks like the cleanest seam in the
        // animation and hides the bone that actually pops.
        if (StepLocation <= StaticLocationEpsilon && StepRotation <= StaticRotationEpsilon)
        {
            continue;
        }
        ++OutStats.ComparedBoneCount;

        const double SeamLocation = FVector::Dist(Track.Locations[0], Track.Locations.Last());
        const double SeamRotation = AngleDegrees(Track.Rotations.Num() > 0 ? Track.Rotations[0] : FQuat::Identity,
            Track.Rotations.Num() > 0 ? Track.Rotations.Last() : FQuat::Identity);

        const double LocationRatio = StepLocation > StaticLocationEpsilon ? SeamLocation / StepLocation : 0.0;
        const double RotationRatio = StepRotation > StaticRotationEpsilon ? SeamRotation / StepRotation : 0.0;
        const double WorstRatio = FMath::Max(LocationRatio, RotationRatio);

        if (!OutStats.bMeasurable || WorstRatio > FMath::Max(OutStats.LocationRatio, OutStats.RotationRatio))
        {
            OutStats.LocationDelta = SeamLocation;
            OutStats.MedianStepLocation = StepLocation;
            OutStats.LocationRatio = LocationRatio;
            OutStats.RotationDelta = SeamRotation;
            OutStats.MedianStepRotation = StepRotation;
            OutStats.RotationRatio = RotationRatio;
            OutStats.WorstBone = Track.BoneName;
        }
        OutStats.bMeasurable = true;
    }
}

void MeasureJitter(TConstArrayView<FBoneTrack> Tracks, FJitterStats& OutStats)
{
    OutStats = FJitterStats();

    for (const FBoneTrack& Track : Tracks)
    {
        if (Track.Locations.Num() < 3)
        {
            continue;
        }
        const double StepLocation = MedianLocationStep(Track);
        if (StepLocation <= StaticLocationEpsilon)
        {
            // No denominator. A perfectly static bone has a zero second difference too, so
            // including it would dilute FrameFraction with samples that were never at risk.
            continue;
        }
        ++OutStats.ComparedBoneCount;

        for (int32 Index = 1; Index + 1 < Track.Locations.Num(); ++Index)
        {
            const FVector SecondDifference =
                Track.Locations[Index - 1] - 2.0 * Track.Locations[Index] + Track.Locations[Index + 1];
            const double Ratio = SecondDifference.Size() / StepLocation;
            ++OutStats.EvaluatedSamples;
            if (Ratio > JitterFrameRatio)
            {
                ++OutStats.JitteringSamples;
            }
            if (Ratio > OutStats.MaxRatio)
            {
                OutStats.MaxRatio = Ratio;
                OutStats.WorstBone = Track.BoneName;
                OutStats.WorstSample = Index;
            }
        }
    }

    if (OutStats.EvaluatedSamples > 0)
    {
        OutStats.bMeasurable = true;
        OutStats.FrameFraction =
            static_cast<double>(OutStats.JitteringSamples) / static_cast<double>(OutStats.EvaluatedSamples);
    }
}

void MeasureLocomotion(const FBoneTrack& FootTrack, double SampleSeconds, bool bLooping,
    double PlantBandFraction, FLocomotionStats& OutStats)
{
    OutStats = FLocomotionStats();
    OutStats.BoneName = FootTrack.BoneName;

    int32 SampleCount = FootTrack.Locations.Num();
    if (SampleCount < 3 || !(SampleSeconds > 0.0))
    {
        return;
    }

    // A cycle is normally authored with an explicit wrap key: UE stores NumberOfFrames + 1
    // keys, and the last duplicates the first so the loop is seamless. That duplicate is a
    // POSITION, not an extra interval of time. Counting it would stretch a 30-frame cycle to
    // 31 samples and deflate every derived speed by 30/31 — a silent 3% error that looks like
    // foot slide and is not. Dropped here, and reported, so the arithmetic below runs on
    // distinct samples only.
    if (bLooping && FVector::Dist(FootTrack.Locations[0], FootTrack.Locations[SampleCount - 1])
        <= StaticLocationEpsilon)
    {
        OutStats.bDroppedWrapSample = true;
        --SampleCount;
        if (SampleCount < 3)
        {
            return;
        }
    }

    OutStats.MinHeight = FootTrack.Locations[0].Z;
    OutStats.MaxHeight = FootTrack.Locations[0].Z;
    for (int32 Index = 0; Index < SampleCount; ++Index)
    {
        const FVector& Location = FootTrack.Locations[Index];
        OutStats.MinHeight = FMath::Min(OutStats.MinHeight, Location.Z);
        OutStats.MaxHeight = FMath::Max(OutStats.MaxHeight, Location.Z);
    }
    OutStats.HeightRange = OutStats.MaxHeight - OutStats.MinHeight;
    if (OutStats.HeightRange < MinFootHeightRange)
    {
        // The foot never lifts. There is no stance to separate from a swing, so anything this
        // function could report about stride would be an artefact of sampling noise.
        return;
    }

    OutStats.PlantBandHeight = OutStats.MinHeight + OutStats.HeightRange * PlantBandFraction;

    TArray<bool> Planted;
    Planted.SetNumUninitialized(SampleCount);
    for (int32 Index = 0; Index < SampleCount; ++Index)
    {
        Planted[Index] = FootTrack.Locations[Index].Z <= OutStats.PlantBandHeight;
    }

    // Longest contiguous planted run. On a looping clip the run may straddle the array
    // boundary, and refusing to wrap there would cut a real stance in two and halve the
    // measured stride. On a one-shot clip wrapping would instead join two unrelated stances,
    // which is why bLooping is the caller's declaration rather than something inferred.
    int32 BestStart = INDEX_NONE;
    int32 BestLength = 0;
    bool bBestWrapped = false;

    const int32 Limit = bLooping ? SampleCount * 2 : SampleCount;
    int32 RunStart = INDEX_NONE;
    int32 RunLength = 0;
    for (int32 Step = 0; Step < Limit; ++Step)
    {
        const int32 Index = Step % SampleCount;
        if (Planted[Index])
        {
            if (RunLength == 0)
            {
                RunStart = Step;
            }
            ++RunLength;
            // A wrapped scan can otherwise report a run longer than the clip when every sample
            // is planted, which would produce an infinite implied speed.
            if (RunLength > SampleCount)
            {
                RunLength = SampleCount;
            }
            if (RunLength > BestLength)
            {
                BestLength = RunLength;
                BestStart = RunStart;
                bBestWrapped = (RunStart + RunLength) > SampleCount;
            }
        }
        else
        {
            RunLength = 0;
        }
    }

    if (BestLength < 2 || BestStart == INDEX_NONE)
    {
        return;
    }

    const int32 StartIndex = BestStart % SampleCount;
    const int32 EndIndex = (BestStart + BestLength - 1) % SampleCount;

    OutStats.StanceStartSample = StartIndex;
    OutStats.StanceEndSample = EndIndex;
    OutStats.StanceSampleCount = BestLength;
    OutStats.bStanceWrapped = bBestWrapped;
    // BestLength samples span BestLength - 1 intervals.
    OutStats.StanceSeconds = static_cast<double>(BestLength - 1) * SampleSeconds;

    const FVector Start = FootTrack.Locations[StartIndex];
    const FVector End = FootTrack.Locations[EndIndex];
    OutStats.StanceDisplacement = FVector::Dist2D(Start, End);

    if (OutStats.StanceSeconds > 0.0)
    {
        OutStats.ImpliedGroundSpeed = OutStats.StanceDisplacement / OutStats.StanceSeconds;
        // With the wrap key dropped the cycle spans SampleCount intervals (the last sample
        // wraps to the first); without it, the samples span SampleCount - 1 intervals and
        // nothing is claimed about what happens past the end.
        const int32 CycleIntervals = OutStats.bDroppedWrapSample ? SampleCount : SampleCount - 1;
        OutStats.CycleSeconds = static_cast<double>(CycleIntervals) * SampleSeconds;
        OutStats.StridePerCycle = OutStats.ImpliedGroundSpeed * OutStats.CycleSeconds;
        OutStats.bMeasurable = true;
    }
}
}
