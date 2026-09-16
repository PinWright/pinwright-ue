// Copyright (c) 2026 Alexander Penkin. MIT License.

// Implementation of the pure skinning-audit math. See SkinAuditAnalysis.h for what each check
// measures, why its threshold is derived rather than chosen, and what it cannot see.

#include "Handlers/Animation/SkinAuditAnalysis.h"

#include "Algo/Sort.h"
#include "Math/UnrealMathUtility.h"

namespace PinWrightSkinAudit
{
namespace
{
    // Disjoint-set over vertex indices, used to merge coincident vertices that chain through
    // intermediate neighbours (a -> b within tolerance, b -> c within tolerance, a -> c not).
    // A plain "bucket by quantized cell" grouping would split such a chain across cells and
    // report two clean pairs where there is one four-way seam.
    struct FUnionFind
    {
        TArray<int32> Parent;

        explicit FUnionFind(int32 Count)
        {
            Parent.SetNumUninitialized(Count);
            for (int32 Index = 0; Index < Count; ++Index)
            {
                Parent[Index] = Index;
            }
        }

        int32 Find(int32 Index)
        {
            while (Parent[Index] != Index)
            {
                Parent[Index] = Parent[Parent[Index]];   // path halving
                Index = Parent[Index];
            }
            return Index;
        }

        void Union(int32 A, int32 B)
        {
            const int32 RootA = Find(A);
            const int32 RootB = Find(B);
            if (RootA != RootB)
            {
                Parent[RootB] = RootA;
            }
        }
    };

    // Percentile by nearest-rank on an already-sorted array. Returns 0 for an empty array;
    // every caller reports the sample count alongside, so an empty percentile block is never
    // mistaken for a measured zero.
    double SortedPercentile(const TArray<double>& Sorted, double Fraction)
    {
        if (Sorted.Num() == 0)
        {
            return 0.0;
        }
        const int32 Index = FMath::Clamp(
            FMath::FloorToInt(Fraction * static_cast<double>(Sorted.Num() - 1) + 0.5),
            0, Sorted.Num() - 1);
        return Sorted[Index];
    }
}

double DistanceToSegment(const FVector3f& P, const FBoneSegment& Segment)
{
    const FVector3f Delta = Segment.End - Segment.Start;
    const double LengthSq = static_cast<double>(Delta.SizeSquared());
    if (LengthSq <= UE_DOUBLE_SMALL_NUMBER)
    {
        // Degenerate segment: a leaf bone, or a joint coincident with its child. Distance to
        // the point, which is the correct answer rather than a special case to reject.
        return static_cast<double>((P - Segment.Start).Size());
    }
    const double T = FMath::Clamp(
        static_cast<double>(FVector3f::DotProduct(P - Segment.Start, Delta)) / LengthSq, 0.0, 1.0);
    const FVector3f Closest = Segment.Start + Delta * static_cast<float>(T);
    return static_cast<double>((P - Closest).Size());
}

double MedianSegmentLength(TConstArrayView<FBoneSegment> Segments)
{
    TArray<double> Lengths;
    Lengths.Reserve(Segments.Num());
    for (const FBoneSegment& Segment : Segments)
    {
        const double Length = static_cast<double>((Segment.End - Segment.Start).Size());
        if (Length > UE_DOUBLE_SMALL_NUMBER)
        {
            Lengths.Add(Length);
        }
    }
    if (Lengths.Num() == 0)
    {
        return 0.0;
    }
    Algo::Sort(Lengths);
    return Lengths[Lengths.Num() / 2];
}

void AccumulateWeightSums(TConstArrayView<FAuditVertex> Vertices, double Tolerance,
    int32 MaxOffenders, FWeightSumStats& OutStats)
{
    OutStats = FWeightSumStats();
    OutStats.VertexCount = Vertices.Num();

    bool bSeenInfluenced = false;
    TArray<FWeightSumOffender> Candidates;

    for (int32 VertexIndex = 0; VertexIndex < Vertices.Num(); ++VertexIndex)
    {
        const FAuditVertex& Vertex = Vertices[VertexIndex];
        if (Vertex.InfluenceCount <= 0)
        {
            ++OutStats.ZeroInfluenceCount;
            if (OutStats.ZeroInfluenceVertices.Num() < FMath::Max(0, MaxOffenders))
            {
                OutStats.ZeroInfluenceVertices.Add(VertexIndex);
            }
            continue;
        }

        double Sum = 0.0;
        for (int32 Influence = 0; Influence < Vertex.InfluenceCount; ++Influence)
        {
            Sum += Vertex.Weights[Influence];
        }

        if (!bSeenInfluenced)
        {
            OutStats.MinWeightSum = Sum;
            OutStats.MaxWeightSum = Sum;
            bSeenInfluenced = true;
        }
        else
        {
            OutStats.MinWeightSum = FMath::Min(OutStats.MinWeightSum, Sum);
            OutStats.MaxWeightSum = FMath::Max(OutStats.MaxWeightSum, Sum);
        }

        if (FMath::Abs(Sum - 1.0) <= Tolerance)
        {
            ++OutStats.NormalizedCount;
        }
        else
        {
            ++OutStats.UnnormalizedCount;
            Candidates.Add({ VertexIndex, Sum, Vertex.InfluenceCount });
        }
    }

    Algo::Sort(Candidates, [](const FWeightSumOffender& Lhs, const FWeightSumOffender& Rhs)
    {
        return FMath::Abs(Lhs.WeightSum - 1.0) > FMath::Abs(Rhs.WeightSum - 1.0);
    });
    const int32 Keep = FMath::Min(Candidates.Num(), FMath::Max(0, MaxOffenders));
    OutStats.Offenders.Append(Candidates.GetData(), Keep);
}

void AccumulateInfluenceCounts(TConstArrayView<FAuditVertex> Vertices, int32 InfluenceLimit,
    int32 MaxOffenders, FInfluenceCountStats& OutStats)
{
    OutStats = FInfluenceCountStats();
    OutStats.VertexCount = Vertices.Num();

    for (int32 VertexIndex = 0; VertexIndex < Vertices.Num(); ++VertexIndex)
    {
        const int32 Count = FMath::Clamp(Vertices[VertexIndex].InfluenceCount, 0, MAX_TOTAL_INFLUENCES);
        ++OutStats.Histogram[Count];
        OutStats.MaxInfluences = FMath::Max(OutStats.MaxInfluences, Count);

        if (InfluenceLimit > 0 && Count > InfluenceLimit)
        {
            ++OutStats.OverLimitCount;
            if (OutStats.OverLimitVertices.Num() < FMath::Max(0, MaxOffenders))
            {
                OutStats.OverLimitVertices.Add(VertexIndex);
            }
        }
    }
}

void BeginBoneCoverage(int32 BoneCount, double WeightEpsilon, FBoneCoverage& OutCoverage)
{
    OutCoverage = FBoneCoverage();
    OutCoverage.BoneCount = FMath::Max(0, BoneCount);
    OutCoverage.WeightEpsilon = WeightEpsilon;
    OutCoverage.InfluencedVertexCountPerBone.Init(0, OutCoverage.BoneCount);
    OutCoverage.RigidVertexCountPerBone.Init(0, OutCoverage.BoneCount);
}

void AddBoneCoverageVertex(FBoneCoverage& InOutCoverage, TConstArrayView<int32> BoneIndices,
    TConstArrayView<double> Weights)
{
    ++InOutCoverage.VertexCount;

    // Two passes over at most MAX_TOTAL_INFLUENCES slots. The first decides whether the vertex
    // is rigid at all, because "rigid" is a property of the VERTEX (exactly one meaningful
    // influence) and cannot be decided while walking a single influence.
    const int32 Num = FMath::Min(BoneIndices.Num(), Weights.Num());
    int32 MeaningfulCount = 0;
    int32 SoleBoneIndex = INDEX_NONE;
    for (int32 Slot = 0; Slot < Num; ++Slot)
    {
        if (Weights[Slot] < InOutCoverage.WeightEpsilon)
        {
            continue;
        }
        ++MeaningfulCount;
        SoleBoneIndex = BoneIndices[Slot];
    }

    for (int32 Slot = 0; Slot < Num; ++Slot)
    {
        if (Weights[Slot] < InOutCoverage.WeightEpsilon)
        {
            continue;
        }
        const int32 BoneIndex = BoneIndices[Slot];
        if (!InOutCoverage.InfluencedVertexCountPerBone.IsValidIndex(BoneIndex))
        {
            // Out of the reference skeleton's range. Skipped rather than clamped: attributing
            // it to bone 0 would invent coverage for the root and hide a real gap.
            continue;
        }
        ++InOutCoverage.InfluencedVertexCountPerBone[BoneIndex];
    }

    if (MeaningfulCount == 1 && InOutCoverage.RigidVertexCountPerBone.IsValidIndex(SoleBoneIndex))
    {
        ++InOutCoverage.RigidVertexCount;
        ++InOutCoverage.RigidVertexCountPerBone[SoleBoneIndex];
    }
}

void EndBoneCoverage(FBoneCoverage& InOutCoverage)
{
    InOutCoverage.InfluencedBoneCount = 0;
    InOutCoverage.BonesWithNoInfluence.Reset();
    for (int32 BoneIndex = 0; BoneIndex < InOutCoverage.BoneCount; ++BoneIndex)
    {
        if (InOutCoverage.InfluencedVertexCountPerBone[BoneIndex] > 0)
        {
            ++InOutCoverage.InfluencedBoneCount;
        }
        else
        {
            InOutCoverage.BonesWithNoInfluence.Add(BoneIndex);
        }
    }
}

void AccumulateBoneCoverage(TConstArrayView<FAuditVertex> Vertices, int32 BoneCount,
    double WeightEpsilon, FBoneCoverage& OutCoverage)
{
    BeginBoneCoverage(BoneCount, WeightEpsilon, OutCoverage);
    for (const FAuditVertex& Vertex : Vertices)
    {
        const int32 Count = FMath::Clamp(Vertex.InfluenceCount, 0, MAX_TOTAL_INFLUENCES);
        AddBoneCoverageVertex(OutCoverage,
            TConstArrayView<int32>(Vertex.BoneIndices, Count),
            TConstArrayView<double>(Vertex.Weights, Count));
    }
    EndBoneCoverage(OutCoverage);
}

void FindCoincidentSplits(TConstArrayView<FAuditVertex> Vertices, double PositionTolerance,
    double SplitTolerance, int32 MaxGroups, FCoincidentStats& OutStats)
{
    OutStats = FCoincidentStats();
    if (Vertices.Num() == 0 || !(PositionTolerance > 0.0))
    {
        return;
    }

    const double InvCell = 1.0 / PositionTolerance;
    const double ToleranceSq = PositionTolerance * PositionTolerance;

    // Hash grid at exactly the tolerance, so two vertices within tolerance are always in the
    // same cell or in one of its 26 neighbours. A coarser grid would need a wider neighbour
    // sweep; a finer one would miss pairs.
    TMap<FIntVector, TArray<int32>> Cells;
    Cells.Reserve(Vertices.Num());

    TArray<FIntVector> VertexCell;
    VertexCell.SetNumUninitialized(Vertices.Num());

    for (int32 VertexIndex = 0; VertexIndex < Vertices.Num(); ++VertexIndex)
    {
        const FVector3f& P = Vertices[VertexIndex].Position;
        const FIntVector Cell(
            static_cast<int32>(FMath::FloorToDouble(static_cast<double>(P.X) * InvCell)),
            static_cast<int32>(FMath::FloorToDouble(static_cast<double>(P.Y) * InvCell)),
            static_cast<int32>(FMath::FloorToDouble(static_cast<double>(P.Z) * InvCell)));
        VertexCell[VertexIndex] = Cell;
        Cells.FindOrAdd(Cell).Add(VertexIndex);
    }

    FUnionFind Sets(Vertices.Num());
    for (int32 VertexIndex = 0; VertexIndex < Vertices.Num(); ++VertexIndex)
    {
        const FIntVector& Cell = VertexCell[VertexIndex];
        for (int32 DX = -1; DX <= 1; ++DX)
        {
            for (int32 DY = -1; DY <= 1; ++DY)
            {
                for (int32 DZ = -1; DZ <= 1; ++DZ)
                {
                    const TArray<int32>* Bucket = Cells.Find(Cell + FIntVector(DX, DY, DZ));
                    if (!Bucket)
                    {
                        continue;
                    }
                    for (int32 Other : *Bucket)
                    {
                        if (Other <= VertexIndex)
                        {
                            continue;
                        }
                        if (static_cast<double>(FVector3f::DistSquared(
                                Vertices[VertexIndex].Position, Vertices[Other].Position)) <= ToleranceSq)
                        {
                            Sets.Union(VertexIndex, Other);
                        }
                    }
                }
            }
        }
    }

    TMap<int32, TArray<int32>> Groups;
    for (int32 VertexIndex = 0; VertexIndex < Vertices.Num(); ++VertexIndex)
    {
        Groups.FindOrAdd(Sets.Find(VertexIndex)).Add(VertexIndex);
    }

    TArray<FCoincidentGroup> Splitting;
    for (const TPair<int32, TArray<int32>>& Pair : Groups)
    {
        const TArray<int32>& Members = Pair.Value;
        if (Members.Num() < 2)
        {
            continue;
        }
        ++OutStats.GroupCount;
        OutStats.CoincidentVertexCount += Members.Num();

        // Per-bone min and max weight across the group. The split coefficient is the sum of
        // (max - min) over every bone any member references. For a two-member group this is
        // exactly the pairwise L1 disagreement; for a larger group it is the tight upper
        // bound over all member pairs, which is the quantity a caller wants because any pair
        // splitting is a visible seam.
        TMap<int32, TPair<double, double>> BoneRange;
        for (int32 Member : Members)
        {
            const FAuditVertex& Vertex = Vertices[Member];
            for (int32 Influence = 0; Influence < Vertex.InfluenceCount; ++Influence)
            {
                TPair<double, double>& Range = BoneRange.FindOrAdd(Vertex.BoneIndices[Influence],
                    TPair<double, double>(TNumericLimits<double>::Max(), 0.0));
                Range.Key = FMath::Min(Range.Key, Vertex.Weights[Influence]);
                Range.Value = FMath::Max(Range.Value, Vertex.Weights[Influence]);
            }
        }

        // A bone absent from a member contributes weight 0 for that member, so its minimum is
        // 0 rather than the smallest weight seen. Detect absence by counting how many members
        // reference the bone.
        TMap<int32, int32> BoneMemberCount;
        for (int32 Member : Members)
        {
            const FAuditVertex& Vertex = Vertices[Member];
            for (int32 Influence = 0; Influence < Vertex.InfluenceCount; ++Influence)
            {
                ++BoneMemberCount.FindOrAdd(Vertex.BoneIndices[Influence], 0);
            }
        }

        FCoincidentGroup Group;
        Group.Position = Vertices[Members[0]].Position;
        Group.VertexIndices = Members;

        TArray<TPair<int32, double>> Deltas;
        for (const TPair<int32, TPair<double, double>>& BonePair : BoneRange)
        {
            const int32 BoneIndex = BonePair.Key;
            const int32* MemberCount = BoneMemberCount.Find(BoneIndex);
            const double MinWeight = (MemberCount && *MemberCount == Members.Num()) ? BonePair.Value.Key : 0.0;
            const double Delta = BonePair.Value.Value - MinWeight;
            if (Delta > 0.0)
            {
                Deltas.Add(TPair<int32, double>(BoneIndex, Delta));
                Group.SplitCoefficient += Delta;
            }
        }

        if (Group.SplitCoefficient <= SplitTolerance)
        {
            continue;
        }

        Algo::Sort(Deltas, [](const TPair<int32, double>& Lhs, const TPair<int32, double>& Rhs)
        {
            return Lhs.Value > Rhs.Value;
        });
        for (const TPair<int32, double>& Delta : Deltas)
        {
            Group.DisagreeingBones.Add(Delta.Key);
        }

        ++OutStats.SplittingGroupCount;
        OutStats.MaxSplitCoefficient = FMath::Max(OutStats.MaxSplitCoefficient, Group.SplitCoefficient);
        Splitting.Add(MoveTemp(Group));
    }

    Algo::Sort(Splitting, [](const FCoincidentGroup& Lhs, const FCoincidentGroup& Rhs)
    {
        return Lhs.SplitCoefficient > Rhs.SplitCoefficient;
    });
    const int32 Keep = FMath::Min(Splitting.Num(), FMath::Max(0, MaxGroups));
    OutStats.Groups.Append(Splitting.GetData(), Keep);
}

void ComputeReach(TConstArrayView<FAuditVertex> Vertices, TConstArrayView<FBoneSegment> Segments,
    double MinWeight, int32 VertexBudget, int32 MaxOffenders, FReachStats& OutStats)
{
    OutStats = FReachStats();
    OutStats.TotalVertexCount = Vertices.Num();

    const double Median = MedianSegmentLength(Segments);
    OutStats.MedianSegmentLength = Median;
    if (!(Median > 0.0) || Vertices.Num() == 0 || Segments.Num() == 0)
    {
        // No usable scale: a one-bone skeleton, or every joint coincident. Leaving
        // bMeasurable false is the whole point — a reach block full of zeros must never read
        // as "measured, nothing wrong".
        return;
    }
    OutStats.bMeasurable = true;

    int32 MaxBoneIndex = INDEX_NONE;
    for (const FBoneSegment& Segment : Segments)
    {
        MaxBoneIndex = FMath::Max(MaxBoneIndex, Segment.BoneIndex);
    }
    if (MaxBoneIndex < 0)
    {
        OutStats.bMeasurable = false;
        return;
    }

    const int32 NumBones = MaxBoneIndex + 1;
    const int32 Budget = VertexBudget > 0 ? VertexBudget : Vertices.Num();
    const int32 Stride = FMath::Max(1, FMath::DivideAndRoundUp(Vertices.Num(), Budget));

    TArray<double> PerBoneDistance;
    PerBoneDistance.SetNumUninitialized(NumBones);

    TArray<double> GatedNormalized;
    TArray<double> RigidNormalized;
    TArray<FReachOffender> Candidates;
    TArray<FReachOffender> RigidCandidates;

    for (int32 VertexIndex = 0; VertexIndex < Vertices.Num(); VertexIndex += Stride)
    {
        const FAuditVertex& Vertex = Vertices[VertexIndex];
        ++OutStats.SampledVertexCount;

        for (int32 Bone = 0; Bone < NumBones; ++Bone)
        {
            PerBoneDistance[Bone] = TNumericLimits<double>::Max();
        }
        for (const FBoneSegment& Segment : Segments)
        {
            if (Segment.BoneIndex < 0 || Segment.BoneIndex >= NumBones)
            {
                continue;
            }
            const double Distance = DistanceToSegment(Vertex.Position, Segment);
            PerBoneDistance[Segment.BoneIndex] = FMath::Min(PerBoneDistance[Segment.BoneIndex], Distance);
        }

        // Rigidity is a property of the VERTEX, so it is decided before any influence is
        // classified. The same MinWeight epsilon that decides an influence is worth measuring
        // decides whether it counts toward rigidity - one epsilon, so the two answers cannot
        // disagree about what "this influence matters" means.
        int32 MeaningfulInfluences = 0;
        for (int32 Influence = 0; Influence < Vertex.InfluenceCount; ++Influence)
        {
            if (Vertex.Weights[Influence] >= MinWeight)
            {
                ++MeaningfulInfluences;
            }
        }
        const bool bRigidVertex = MeaningfulInfluences == 1;
        bool bCountedRigidVertex = false;

        for (int32 Influence = 0; Influence < Vertex.InfluenceCount; ++Influence)
        {
            const double Weight = Vertex.Weights[Influence];
            if (Weight < MinWeight)
            {
                continue;
            }
            const int32 BoneIndex = Vertex.BoneIndices[Influence];
            if (BoneIndex < 0 || BoneIndex >= NumBones)
            {
                continue;
            }
            const double Distance = PerBoneDistance[BoneIndex];
            if (Distance >= TNumericLimits<double>::Max())
            {
                // The influencing bone contributed no segment. Not measurable for this
                // influence; skipping it keeps it out of the percentiles rather than
                // recording a fabricated distance.
                continue;
            }

            int32 CloserBoneCount = 0;
            for (int32 Bone = 0; Bone < NumBones; ++Bone)
            {
                if (Bone != BoneIndex && PerBoneDistance[Bone] < Distance)
                {
                    ++CloserBoneCount;
                }
            }

            const double Normalized = Distance / Median;
            ++OutStats.InfluencesConsidered;
            const FReachOffender Measurement{ VertexIndex, BoneIndex, Weight, Distance,
                Normalized, CloserBoneCount };

            // The intended-authoring signature, stated as a conjunction: this vertex has one
            // meaningful influence, and no bone is strictly nearer than the one it names. A
            // rigid bind to a bone something else is closer to still lands in the gated
            // bucket, which is what keeps the wrong-bone defect catchable.
            if (bRigidVertex && CloserBoneCount == 0)
            {
                ++OutStats.RigidBindInfluences;
                if (!bCountedRigidVertex)
                {
                    ++OutStats.RigidBindVertices;
                    bCountedRigidVertex = true;
                }
                RigidNormalized.Add(Normalized);
                RigidCandidates.Add(Measurement);
                continue;
            }

            ++OutStats.GatedInfluences;
            GatedNormalized.Add(Normalized);
            Candidates.Add(Measurement);
        }
    }

    if (OutStats.InfluencesConsidered == 0)
    {
        // Every influence fell below MinWeight or referenced a segment-less bone. Nothing was
        // measured AT ALL - neither bucket - so say so rather than emitting zeroed
        // percentiles. An empty GATED bucket alongside a populated rigid one is a different
        // state entirely: that one WAS measured, and every measurement was not-applicable to
        // a reach verdict.
        OutStats.bMeasurable = false;
        return;
    }

    Algo::Sort(GatedNormalized);
    if (GatedNormalized.Num() > 0)
    {
        OutStats.NormalizedDistancePercentiles[0] = SortedPercentile(GatedNormalized, 0.5);
        OutStats.NormalizedDistancePercentiles[1] = SortedPercentile(GatedNormalized, 0.9);
        OutStats.NormalizedDistancePercentiles[2] = SortedPercentile(GatedNormalized, 0.99);
        OutStats.NormalizedDistancePercentiles[3] = SortedPercentile(GatedNormalized, 0.999);
        OutStats.NormalizedDistancePercentiles[4] = GatedNormalized.Last();
    }

    Algo::Sort(RigidNormalized);
    if (RigidNormalized.Num() > 0)
    {
        OutStats.RigidBindNormalizedDistancePercentiles[0] = SortedPercentile(RigidNormalized, 0.5);
        OutStats.RigidBindNormalizedDistancePercentiles[1] = SortedPercentile(RigidNormalized, 0.9);
        OutStats.RigidBindNormalizedDistancePercentiles[2] = SortedPercentile(RigidNormalized, 0.99);
        OutStats.RigidBindNormalizedDistancePercentiles[3] = SortedPercentile(RigidNormalized, 0.999);
        OutStats.RigidBindNormalizedDistancePercentiles[4] = RigidNormalized.Last();
    }

    const auto ByNormalizedDistanceDescending = [](const FReachOffender& Lhs, const FReachOffender& Rhs)
    {
        return Lhs.NormalizedDistance > Rhs.NormalizedDistance;
    };
    Algo::Sort(Candidates, ByNormalizedDistanceDescending);
    Algo::Sort(RigidCandidates, ByNormalizedDistanceDescending);
    const int32 Cap = FMath::Max(0, MaxOffenders);
    OutStats.Offenders.Append(Candidates.GetData(), FMath::Min(Candidates.Num(), Cap));
    OutStats.RigidBindOffenders.Append(RigidCandidates.GetData(), FMath::Min(RigidCandidates.Num(), Cap));
}
}
