// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/MeshAuditUtils.h"

namespace
{
    MeshAudit::FZFightTriangle MeshAuditZFightingTriangle(
        int32 Id, int32 Component, const FVector3d& A, const FVector3d& B,
        const FVector3d& C)
    {
        MeshAudit::FZFightTriangle Triangle;
        Triangle.TriangleID = Id;
        Triangle.ComponentIndex = Component;
        Triangle.V0 = A;
        Triangle.V1 = B;
        Triangle.V2 = C;
        const FVector3d Cross = FVector3d::CrossProduct(B - A, C - A);
        Triangle.Area = 0.5 * Cross.Length();
        Triangle.Normal = Cross.GetSafeNormal();
        Triangle.BoundsMin = FVector3d(
            FMath::Min3(A.X, B.X, C.X), FMath::Min3(A.Y, B.Y, C.Y),
            FMath::Min3(A.Z, B.Z, C.Z));
        Triangle.BoundsMax = FVector3d(
            FMath::Max3(A.X, B.X, C.X), FMath::Max3(A.Y, B.Y, C.Y),
            FMath::Max3(A.Z, B.Z, C.Z));
        return Triangle;
    }

    MeshAudit::FZFightTriangle MeshAuditZFightingRightTriangle(
        int32 Id, int32 Component, double Z, bool bReverse)
    {
        const FVector3d A(0.0, 0.0, Z);
        const FVector3d B(2.0, 0.0, Z);
        const FVector3d C(0.0, 2.0, Z);
        return bReverse
            ? MeshAuditZFightingTriangle(Id, Component, A, C, B)
            : MeshAuditZFightingTriangle(Id, Component, A, B, C);
    }

    TArray<MeshAudit::FZFightTriangle> MeshAuditZFightingQuad(
        int32 FirstId, int32 Component, const FVector3d& A, const FVector3d& B,
        const FVector3d& C, const FVector3d& D, bool bReverse)
    {
        TArray<MeshAudit::FZFightTriangle> Quad;
        Quad.Reserve(2);
        if (bReverse)
        {
            Quad.Add(MeshAuditZFightingTriangle(FirstId, Component, A, C, B));
            Quad.Add(MeshAuditZFightingTriangle(FirstId + 1, Component, A, D, C));
        }
        else
        {
            Quad.Add(MeshAuditZFightingTriangle(FirstId, Component, A, B, C));
            Quad.Add(MeshAuditZFightingTriangle(FirstId + 1, Component, A, C, D));
        }
        return Quad;
    }

    TArray<MeshAudit::FZFightTriangle> MeshAuditZFightingReverseOrder(
        const TArray<MeshAudit::FZFightTriangle>& Triangles)
    {
        TArray<MeshAudit::FZFightTriangle> Reversed;
        Reversed.Reserve(Triangles.Num());
        for (int32 Index = Triangles.Num() - 1; Index >= 0; --Index)
        {
            Reversed.Add(Triangles[Index]);
        }
        return Reversed;
    }

    MeshAudit::FZFightAnalysis MeshAuditZFightingAnalyze(
        const TArray<MeshAudit::FZFightTriangle>& Triangles)
    {
        MeshAudit::FZFightAnalysis Result;
        MeshAudit::AnalyzeZFighting(Triangles, MeshAudit::FThresholds(), Result);
        return Result;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditZFightingTest,
    "PinWright.Geometry.MeshAudit.ZFightingSyntheticSurfaces",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshAuditZFightingTest::RunTest(const FString& Parameters)
{
    // Each surface fixture is an explicit quad represented by two triangles. The two quads
    // use separate components so the detector's component filter cannot make a case vacuous.
    TArray<MeshAudit::FZFightTriangle> Coincident;
    Coincident.Append(MeshAuditZFightingQuad(0, 0,
        FVector3d(0.0, 0.0, 0.0), FVector3d(2.0, 0.0, 0.0),
        FVector3d(2.0, 2.0, 0.0), FVector3d(0.0, 2.0, 0.0), false));
    Coincident.Append(MeshAuditZFightingQuad(2, 1,
        FVector3d(0.0, 0.0, 0.0), FVector3d(2.0, 0.0, 0.0),
        FVector3d(2.0, 2.0, 0.0), FVector3d(0.0, 2.0, 0.0), false));
    const MeshAudit::FZFightAnalysis CoincidentResult =
        MeshAuditZFightingAnalyze(Coincident);
    const MeshAudit::FZFightAnalysis CoincidentReverseResult =
        MeshAuditZFightingAnalyze(MeshAuditZFightingReverseOrder(Coincident));
    TestFalse(TEXT("coincident triangles are measurable"), CoincidentResult.bUnrunnable);
    TestFalse(TEXT("coincident quads are measurable in reverse order"),
        CoincidentReverseResult.bUnrunnable);
    TestTrue(TEXT("coincident quads reach the broad phase"),
        CoincidentResult.CandidatePairCount > 0);
    TestTrue(TEXT("coincident quads reach the exact phase"),
        CoincidentResult.ExactOverlapTestCount > 0);
    TestEqual(TEXT("coincident quads fight in forward order"),
        CoincidentResult.FightingPairCount, 2);
    TestEqual(TEXT("coincident quads fight in reverse order"),
        CoincidentReverseResult.FightingPairCount, 2);
    TestTrue(TEXT("coincident quad overlap area is the true area 4"),
        FMath::IsNearlyEqual(CoincidentResult.TotalOverlapArea, 4.0));
    TestTrue(TEXT("coincident quad overlap area is true in reverse order"),
        FMath::IsNearlyEqual(CoincidentReverseResult.TotalOverlapArea, 4.0));
    TestEqual(TEXT("the two triangles of one coincident quad pair form one region"),
        CoincidentResult.Regions.Num(), 1);
    TestEqual(TEXT("the two triangles of one coincident quad pair form one reverse region"),
        CoincidentReverseResult.Regions.Num(), 1);
    TestEqual(TEXT("fighting triangle count is unique"), CoincidentResult.FightingTriangleCount, 4);
    if (CoincidentResult.Regions.Num() == 1)
    {
        const MeshAudit::FZFightRegion& Region = CoincidentResult.Regions[0];
        TestTrue(TEXT("coincident region area is the true area 4"),
            FMath::IsNearlyEqual(Region.OverlapArea, 4.0));
        TestEqual(TEXT("coincident region has both overlapping triangle pairs"),
            Region.PairCount, 2);
        TestEqual(TEXT("coincident region has four unique triangles"),
            Region.TriangleIds.Num(), 4);
        TestEqual(TEXT("coincident region has two unique components"),
            Region.ComponentIds.Num(), 2);
        TestEqual(TEXT("coincident region rank starts at one"), Region.Rank, 1);
        TestEqual(TEXT("all evidence fits below the bounded top-pair cap"),
            Region.TopPairs.Num(), 2);
    }
    // Pair-summing is wrong when more than two shells cover the same surface: six pair findings
    // describe one physical square, not twelve square units of fighting area.
    TArray<MeshAudit::FZFightTriangle> TripleCoincident;
    TripleCoincident.Append(MeshAuditZFightingQuad(60, 0,
        FVector3d(0.0, 0.0, 0.0), FVector3d(2.0, 0.0, 0.0),
        FVector3d(2.0, 2.0, 0.0), FVector3d(0.0, 2.0, 0.0), false));
    TripleCoincident.Append(MeshAuditZFightingQuad(62, 1,
        FVector3d(0.0, 0.0, 0.0), FVector3d(2.0, 0.0, 0.0),
        FVector3d(2.0, 2.0, 0.0), FVector3d(0.0, 2.0, 0.0), false));
    TripleCoincident.Append(MeshAuditZFightingQuad(64, 2,
        FVector3d(0.0, 0.0, 0.0), FVector3d(2.0, 0.0, 0.0),
        FVector3d(2.0, 2.0, 0.0), FVector3d(0.0, 2.0, 0.0), false));
    const MeshAudit::FZFightAnalysis TripleResult =
        MeshAuditZFightingAnalyze(TripleCoincident);
    TestEqual(TEXT("three coincident quads expose all six surface pairs"),
        TripleResult.FightingPairCount, 6);
    TestTrue(TEXT("three coincident quads report unique union area 4"),
        FMath::IsNearlyEqual(TripleResult.TotalOverlapArea, 4.0));
    TestEqual(TEXT("three coincident quads form one fighting region"),
        TripleResult.Regions.Num(), 1);
    if (TripleResult.Regions.Num() == 1)
    {
        TestTrue(TEXT("three coincident quads region area is unique 4"),
            FMath::IsNearlyEqual(TripleResult.Regions[0].OverlapArea, 4.0));
        TestEqual(TEXT("three coincident quads retain all pair evidence"),
            TripleResult.Regions[0].PairCount, 6);
    }

    TestTrue(TEXT("the broad phase records real grid references"),
        CoincidentResult.GridReferenceCount > 0);
    TestTrue(TEXT("grid references remain bounded per small fixture"),
        CoincidentResult.GridReferenceCount
            <= CoincidentResult.ValidTriangleCount
                * (MeshAudit::ZFightMaxFineCellsPerTriangle
                   + MeshAudit::ZFightLargeGridResolution
                     * MeshAudit::ZFightLargeGridResolution
                     * MeshAudit::ZFightLargeGridResolution));
    TestTrue(TEXT("the full-span fixture exercises the large-triangle fallback"),
        CoincidentResult.LargeTriangleCount > 0);
    TestTrue(TEXT("large fallback reports inspected references"),
        CoincidentResult.LargeReferenceInspectCount > 0);

    TArray<MeshAudit::FZFightTriangle> DenseFallback;
    DenseFallback.Reserve(MeshAudit::ZFightMaxCoarseReferencesPerLargeTriangle + 1);
    for (int32 Index = 0;
         Index <= MeshAudit::ZFightMaxCoarseReferencesPerLargeTriangle; ++Index)
    {
        // These triangles are deliberately in one component and span the model so they all
        // use the coarse fallback. Same-component filtering must not allow an unbounded bucket
        // scan to masquerade as a clean result.
        DenseFallback.Add(MeshAuditZFightingTriangle(1000 + Index, 7,
            FVector3d(0.0, 0.0, 0.0), FVector3d(2.0, 0.0, 0.0),
            FVector3d(0.0, 2.0, 2.0)));
    }
    const MeshAudit::FZFightAnalysis DenseFallbackResult =
        MeshAuditZFightingAnalyze(DenseFallback);
    TestTrue(TEXT("dense large-triangle fallback fails closed"),
        DenseFallbackResult.bUnrunnable);
    TestTrue(TEXT("dense fallback uses the registered unrunnable code"),
        DenseFallbackResult.UnrunnableCode
            == ErrorCodes::ERR_MESH_AUDIT_Z_FIGHTING_UNRUNNABLE);
    TestEqual(TEXT("dense fallback stops after one reference beyond the cap"),
        DenseFallbackResult.LargeReferenceInspectCount,
        MeshAudit::ZFightMaxCoarseReferencesPerLargeTriangle + 1);
    TestEqual(TEXT("dense fallback records the bounded per-triangle maximum"),
        DenseFallbackResult.MaxLargeReferenceInspectCount,
        MeshAudit::ZFightMaxCoarseReferencesPerLargeTriangle + 1);

    TArray<MeshAudit::FZFightTriangle> Separated;
    Separated.Append(MeshAuditZFightingQuad(10, 0,
        FVector3d(0.0, 0.0, 0.0), FVector3d(2.0, 0.0, 0.0),
        FVector3d(2.0, 2.0, 0.0), FVector3d(0.0, 2.0, 0.0), false));
    Separated.Append(MeshAuditZFightingQuad(12, 1,
        FVector3d(0.0, 0.0, 1.0), FVector3d(2.0, 0.0, 1.0),
        FVector3d(2.0, 2.0, 1.0), FVector3d(0.0, 2.0, 1.0), false));
    const MeshAudit::FZFightAnalysis SeparatedResult =
        MeshAuditZFightingAnalyze(Separated);
    const MeshAudit::FZFightAnalysis SeparatedReverseResult =
        MeshAuditZFightingAnalyze(MeshAuditZFightingReverseOrder(Separated));
    TestFalse(TEXT("normal-separated quads are measurable"), SeparatedResult.bUnrunnable);
    TestFalse(TEXT("normal-separated quads are measurable in reverse order"),
        SeparatedReverseResult.bUnrunnable);
    TestEqual(TEXT("normal-separated quads are clean in forward order"),
        SeparatedResult.FightingPairCount, 0);
    TestEqual(TEXT("normal-separated quads are clean in reverse order"),
        SeparatedReverseResult.FightingPairCount, 0);
    TestTrue(TEXT("normal-separated quad area is zero"),
        FMath::IsNearlyEqual(SeparatedResult.TotalOverlapArea, 0.0));
    TestTrue(TEXT("normal-separated quad area is zero in reverse order"),
        FMath::IsNearlyEqual(SeparatedReverseResult.TotalOverlapArea, 0.0));

    TArray<MeshAudit::FZFightTriangle> SideBySide;
    SideBySide.Append(MeshAuditZFightingQuad(20, 0,
        FVector3d(0.0, 0.0, 0.0), FVector3d(2.0, 0.0, 0.0),
        FVector3d(2.0, 2.0, 0.0), FVector3d(0.0, 2.0, 0.0), false));
    SideBySide.Append(MeshAuditZFightingQuad(22, 1,
        FVector3d(3.0, 0.0, 0.0), FVector3d(5.0, 0.0, 0.0),
        FVector3d(5.0, 2.0, 0.0), FVector3d(3.0, 2.0, 0.0), false));
    const MeshAudit::FZFightAnalysis SideBySideResult =
        MeshAuditZFightingAnalyze(SideBySide);
    const MeshAudit::FZFightAnalysis SideBySideReverseResult =
        MeshAuditZFightingAnalyze(MeshAuditZFightingReverseOrder(SideBySide));
    TestEqual(TEXT("coplanar side-by-side quads are clean in forward order"),
        SideBySideResult.FightingPairCount, 0);
    TestEqual(TEXT("coplanar side-by-side quads are clean in reverse order"),
        SideBySideReverseResult.FightingPairCount, 0);
    TestTrue(TEXT("coplanar side-by-side quad area is zero"),
        FMath::IsNearlyEqual(SideBySideResult.TotalOverlapArea, 0.0));
    TestTrue(TEXT("coplanar side-by-side quad area is zero in reverse order"),
        FMath::IsNearlyEqual(SideBySideReverseResult.TotalOverlapArea, 0.0));

    // The vertical quad intersects the horizontal quad at x=1. Distinct components are
    // intentional: the detector must run the exact normal test, then reject transverse
    // geometry, rather than skip the pair as same-component tessellation.
    TArray<MeshAudit::FZFightTriangle> Transverse;
    Transverse.Append(MeshAuditZFightingQuad(30, 0,
        FVector3d(0.0, 0.0, 0.0), FVector3d(2.0, 0.0, 0.0),
        FVector3d(2.0, 2.0, 0.0), FVector3d(0.0, 2.0, 0.0), false));
    Transverse.Append(MeshAuditZFightingQuad(32, 1,
        FVector3d(1.0, -1.0, -1.0), FVector3d(1.0, 3.0, -1.0),
        FVector3d(1.0, 3.0, 1.0), FVector3d(1.0, -1.0, 1.0), false));
    const MeshAudit::FZFightAnalysis TransverseResult =
        MeshAuditZFightingAnalyze(Transverse);
    const MeshAudit::FZFightAnalysis TransverseReverseResult =
        MeshAuditZFightingAnalyze(MeshAuditZFightingReverseOrder(Transverse));
    TestTrue(TEXT("transverse quads reach the broad phase"),
        TransverseResult.CandidatePairCount > 0);
    TestTrue(TEXT("transverse quads reach the exact phase"),
        TransverseResult.ExactOverlapTestCount > 0);
    TestTrue(TEXT("transverse quads reach the broad phase in reverse order"),
        TransverseReverseResult.CandidatePairCount > 0);
    TestTrue(TEXT("transverse quads reach the exact phase in reverse order"),
        TransverseReverseResult.ExactOverlapTestCount > 0);
    TestEqual(TEXT("transverse quads fail the normal-alignment condition"),
        TransverseResult.FightingPairCount, 0);
    TestEqual(TEXT("transverse quads fail the normal-alignment condition in reverse order"),
        TransverseReverseResult.FightingPairCount, 0);
    TestTrue(TEXT("transverse quad area is zero"),
        FMath::IsNearlyEqual(TransverseResult.TotalOverlapArea, 0.0));
    TestTrue(TEXT("transverse quad area is zero in reverse order"),
        FMath::IsNearlyEqual(TransverseReverseResult.TotalOverlapArea, 0.0));

    TArray<MeshAudit::FZFightTriangle> AntiParallel;
    AntiParallel.Append(MeshAuditZFightingQuad(40, 0,
        FVector3d(0.0, 0.0, 0.0), FVector3d(2.0, 0.0, 0.0),
        FVector3d(2.0, 2.0, 0.0), FVector3d(0.0, 2.0, 0.0), false));
    AntiParallel.Append(MeshAuditZFightingQuad(42, 1,
        FVector3d(0.0, 0.0, 0.0), FVector3d(2.0, 0.0, 0.0),
        FVector3d(2.0, 2.0, 0.0), FVector3d(0.0, 2.0, 0.0), true));
    const MeshAudit::FZFightAnalysis AntiParallelResult =
        MeshAuditZFightingAnalyze(AntiParallel);
    const MeshAudit::FZFightAnalysis AntiParallelReverseResult =
        MeshAuditZFightingAnalyze(MeshAuditZFightingReverseOrder(AntiParallel));
    TestTrue(TEXT("anti-parallel fixture normals are opposite"),
        FMath::IsNearlyEqual(
            FVector3d::DotProduct(AntiParallel[0].Normal, AntiParallel[2].Normal), -1.0));
    TestEqual(TEXT("anti-parallel quads flag in forward order"),
        AntiParallelResult.FightingPairCount, 2);
    TestEqual(TEXT("anti-parallel quads flag in reverse order"),
        AntiParallelReverseResult.FightingPairCount, 2);
    TestTrue(TEXT("anti-parallel quad overlap area is the true area 4"),
        FMath::IsNearlyEqual(AntiParallelResult.TotalOverlapArea, 4.0));
    TestTrue(TEXT("anti-parallel quad overlap area is true in reverse order"),
        FMath::IsNearlyEqual(AntiParallelReverseResult.TotalOverlapArea, 4.0));
    TestEqual(TEXT("anti-parallel quad triangles form one region"),
        AntiParallelResult.Regions.Num(), 1);

    // Normals are within the angular threshold and the triangles touch at one edge, but the
    // tilted triangle's far vertex is outside the extent-derived plane epsilon. A one-anchor
    // plane test accepted this; the symmetric max-distance test must reject it.
    const TArray<MeshAudit::FZFightTriangle> Tilted{
        MeshAuditZFightingRightTriangle(50, 0, 0.0, false),
        MeshAuditZFightingTriangle(51, 1, FVector3d(0.0, 0.0, 0.0),
            FVector3d(2.0, 0.0, 0.0), FVector3d(0.0, 2.0, 0.04))};
    const MeshAudit::FZFightAnalysis TiltedResult = MeshAuditZFightingAnalyze(Tilted);
    TestTrue(TEXT("tilted near-parallel triangles reach the exact phase"),
        TiltedResult.ExactOverlapTestCount > 0);
    TestEqual(TEXT("symmetric plane distance rejects the tilted surface"),
        TiltedResult.FightingPairCount, 0);

    MeshAudit::FConfig Config;
    Config.SelectedChecks = MeshAudit::CheckBit(MeshAudit::ECheck::ZFighting);
    MeshAudit::FAssetMeasurement FightingMeasurement;
    FightingMeasurement.bMeasured = true;
    FightingMeasurement.Health.TriangleCount = Coincident.Num();
    FightingMeasurement.Health.VertexCount = Coincident.Num() * 3;
    FightingMeasurement.ZFightTriangles = Coincident;
    MeshAudit::FReport FightingReport;
    MeshAudit::EvaluateAsset(TEXT("/Game/Test/ZFight"), TEXT("ZFight"),
        FightingMeasurement, Config, FightingReport);
    TestEqual(TEXT("z-fighting is one warning finding"), FightingReport.WarningCount, 1);
    TestEqual(TEXT("z-fighting is not an error finding"), FightingReport.ErrorCount, 0);
    TestTrue(TEXT("warning-only z-fighting passes failOn error"),
        FightingReport.DerivePass(TEXT("error")));
    TestFalse(TEXT("warning-only z-fighting fails failOn any"),
        FightingReport.DerivePass(TEXT("any")));

    MeshAudit::FAssetMeasurement EmptyMeasurement;
    EmptyMeasurement.bMeasured = true;
    MeshAudit::FReport EmptyReport;
    MeshAudit::EvaluateAsset(TEXT("/Game/Test/Empty"), TEXT("Empty"),
        EmptyMeasurement, Config, EmptyReport);
    const MeshAudit::FCheckTally& EmptyTally =
        EmptyReport.Tallies[static_cast<int32>(MeshAudit::ECheck::ZFighting)];
    TestEqual(TEXT("empty z-fighting check remains applicable"), EmptyTally.Applicable, 1);
    TestEqual(TEXT("empty z-fighting check is not not-applicable"),
        EmptyTally.NotApplicable, 0);
    TestEqual(TEXT("empty z-fighting check is unrunnable"), EmptyTally.Unrunnable, 1);
    TestEqual(TEXT("empty z-fighting check is never clean"), EmptyTally.Clean, 0);
    TestFalse(TEXT("empty z-fighting evidence fails even failOn none"),
        EmptyReport.DerivePass(TEXT("none")));
    TestTrue(TEXT("empty finding uses the registered z-fighting unrunnable code"),
        EmptyReport.Findings.Num() == 1
        && EmptyReport.Findings[0].Code
            == ErrorCodes::ERR_MESH_AUDIT_Z_FIGHTING_UNRUNNABLE);

    return true;
}
