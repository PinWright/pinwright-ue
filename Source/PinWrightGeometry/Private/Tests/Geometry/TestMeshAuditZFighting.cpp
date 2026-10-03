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

    // 257 full-span triangles: each spans all 64 coarse cells, each holding all 257, so the
    // fallback would inspect 257 * 64 * 257 = 4,227,136 references - above the whole-mesh
    // budget max(2^20, 257 * 1024). The per-triangle 256-reference cap this used to pin was
    // replaced by that budget (B-mesh-audit-zfight-coarse-grid-unrunnable-on-tiny-meshes); the
    // property kept is the same: a dense scan is refused, never reported clean.
    constexpr int32 DenseFallbackCount = 257;
    TArray<MeshAudit::FZFightTriangle> DenseFallback;
    DenseFallback.Reserve(DenseFallbackCount);
    for (int32 Index = 0; Index < DenseFallbackCount; ++Index)
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
    TestEqual(TEXT("dense fallback names the whole-mesh work budget as the bound"),
        DenseFallbackResult.UnrunnableBound, FString(TEXT("broad_phase_work")));
    TestEqual(TEXT("dense fallback counts every coarse reference it would inspect"),
        DenseFallbackResult.LargeReferenceInspectCount,
        DenseFallbackCount * 64 * DenseFallbackCount);
    TestEqual(TEXT("dense fallback records the per-triangle maximum"),
        DenseFallbackResult.MaxLargeReferenceInspectCount, 64 * DenseFallbackCount);
    TestTrue(TEXT("dense fallback reports work above its budget"),
        DenseFallbackResult.BroadPhaseWork > DenseFallbackResult.BroadPhaseWorkBudget);
    TestEqual(TEXT("dense fallback budget is the floor for a small mesh"),
        DenseFallbackResult.BroadPhaseWorkBudget, MeshAudit::ZFightMinBroadPhaseWork);
    TestEqual(TEXT("dense fallback refuses before building any candidate"),
        DenseFallbackResult.CandidatePairCount, 0);

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

// B-mesh-audit-zfight-coarse-grid-unrunnable-on-tiny-meshes. Three fixtures that the previous
// local 256 caps refused outright, each small enough that an all-pairs scan is trivially cheap:
// one wall-sized quad over a tiled panel (coarse references per large triangle), 300 triangles
// packed into one fine cell (triangles per fine cell), and a duplicated surface bent through
// 90 degrees (one projection axis for a non-planar region). Each must now be ANSWERED, with the
// right answer - a refusal or a wrong area fails the test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditZFightingSmallMeshBoundsTest,
    "PinWright.Geometry.MeshAudit.ZFightingAnswersSmallMeshesTheOldCapsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshAuditZFightingSmallMeshBoundsTest::RunTest(const FString& Parameters)
{
    // ---- one large quad under a 12 x 12 tiled panel of another component ----
    constexpr int32 Tiles = 12;
    constexpr double Span = 10.0;
    constexpr double Tile = Span / Tiles;
    TArray<MeshAudit::FZFightTriangle> Panel;
    Panel.Append(MeshAuditZFightingQuad(0, 0,
        FVector3d(0.0, 0.0, 0.0), FVector3d(Span, 0.0, 0.0),
        FVector3d(Span, Span, 0.0), FVector3d(0.0, Span, 0.0), false));
    for (int32 X = 0; X < Tiles; ++X)
    {
        for (int32 Y = 0; Y < Tiles; ++Y)
        {
            const double X0 = X * Tile, Y0 = Y * Tile;
            Panel.Append(MeshAuditZFightingQuad(Panel.Num(), 1,
                FVector3d(X0, Y0, 0.0), FVector3d(X0 + Tile, Y0, 0.0),
                FVector3d(X0 + Tile, Y0 + Tile, 0.0), FVector3d(X0, Y0 + Tile, 0.0), false));
        }
    }
    TestEqual(TEXT("panel fixture is 290 triangles"), Panel.Num(), 2 + Tiles * Tiles * 2);
    const MeshAudit::FZFightAnalysis PanelResult = MeshAuditZFightingAnalyze(Panel);
    TestTrue(TEXT("panel fixture routes the big quad through the coarse fallback"),
        PanelResult.LargeTriangleCount >= 2);
    TestTrue(TEXT("panel's large triangle inspects more coarse references than the old cap of 256"),
        PanelResult.MaxLargeReferenceInspectCount > 256);
    TestFalse(FString::Printf(TEXT("a 290-triangle panel is measurable (refused: %s)"),
        *PanelResult.UnrunnableReason), PanelResult.bUnrunnable);
    TestTrue(TEXT("the tiles fighting the big quad are found"), PanelResult.FightingPairCount > 0);
    TestTrue(FString::Printf(TEXT("the fought area is the panel's true area 100 (got %.6g)"),
        PanelResult.TotalOverlapArea), FMath::IsNearlyEqual(PanelResult.TotalOverlapArea, 100.0, 1.0e-6));
    TestTrue(TEXT("panel work stays inside its budget"),
        PanelResult.BroadPhaseWork > 0
        && PanelResult.BroadPhaseWork <= PanelResult.BroadPhaseWorkBudget);

    // ---- 300 small triangles of 300 components packed into one fine cell ----
    // A far triangle sets the model extent to ~64, so a fine cell is ~2 units wide. The 300
    // triangles fan about one shared edge inside that cell, each turned 0.3 degrees from the
    // last: every pair overlaps in AABB (so all reach the exact phase), and none is within the
    // plane epsilon of another, so nothing fights.
    TArray<MeshAudit::FZFightTriangle> Dense;
    Dense.Add(MeshAuditZFightingTriangle(0, 0,
        FVector3d(64.0, 64.0, 64.0), FVector3d(63.9, 64.0, 64.0), FVector3d(64.0, 63.9, 64.0)));
    for (int32 Index = 0; Index < 300; ++Index)
    {
        const double Angle = Index * (UE_DOUBLE_HALF_PI / 300.0);
        Dense.Add(MeshAuditZFightingTriangle(Index + 1, Index + 1,
            FVector3d(0.5, 0.5, 0.5), FVector3d(1.0, 0.5, 0.5),
            FVector3d(0.5, 0.5 + 0.5 * FMath::Cos(Angle), 0.5 + 0.5 * FMath::Sin(Angle))));
    }
    const MeshAudit::FZFightAnalysis DenseResult = MeshAuditZFightingAnalyze(Dense);
    TestTrue(TEXT("dense fixture packs more than the old per-cell cap of 256 into one cell"),
        DenseResult.DensestFineCellTriangleCount > 256);
    TestFalse(FString::Printf(TEXT("a 301-triangle mesh with one dense cell is measurable "
        "(refused: %s)"), *DenseResult.UnrunnableReason), DenseResult.bUnrunnable);
    TestTrue(TEXT("the dense cell's pairs all reached the exact phase"),
        DenseResult.ExactOverlapTestCount >= 300 * 299 / 2);
    TestEqual(TEXT("the fanned triangles do not fight"), DenseResult.FightingPairCount, 0);

    // ---- a duplicated L: two faces at 90 degrees, copied into a second component ----
    TArray<MeshAudit::FZFightTriangle> Bent;
    for (int32 Component = 0; Component < 2; ++Component)
    {
        Bent.Append(MeshAuditZFightingQuad(Bent.Num(), Component,
            FVector3d(0.0, 0.0, 0.0), FVector3d(2.0, 0.0, 0.0),
            FVector3d(2.0, 2.0, 0.0), FVector3d(0.0, 2.0, 0.0), false));
        Bent.Append(MeshAuditZFightingQuad(Bent.Num(), Component,
            FVector3d(0.0, 0.0, 0.0), FVector3d(0.0, 2.0, 0.0),
            FVector3d(0.0, 2.0, 2.0), FVector3d(0.0, 0.0, 2.0), false));
    }
    const MeshAudit::FZFightAnalysis BentResult = MeshAuditZFightingAnalyze(Bent);
    TestFalse(FString::Printf(TEXT("a duplicated bent surface is measurable (refused: %s)"),
        *BentResult.UnrunnableReason), BentResult.bUnrunnable);
    TestEqual(TEXT("both faces of the L fight"), BentResult.FightingPairCount, 4);
    TestEqual(TEXT("the bend joins both faces into one region"), BentResult.Regions.Num(), 1);
    TestTrue(FString::Printf(TEXT("the bent region's area is both faces, 8 (got %.6g)"),
        BentResult.TotalOverlapArea), FMath::IsNearlyEqual(BentResult.TotalOverlapArea, 8.0, 1.0e-6));
    return true;
}
