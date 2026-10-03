// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-pwmodel-remove-degenerates-deletes-instead-of-repairing-and-opens-mesh.
//
// remove_degenerates handed every triangle straight to the engine's RepairMeshDegenerateGeometry,
// whose only "repair" is a QEM simplify to min_edge_length with boundaries and seams pinned. A
// zero-area triangle with no short edge - a CAP, one corner lying on the opposite edge - is never
// touched by that, so repair_or_delete deleted it and opened the mesh, and repair_or_skip was a
// no-op that left it. The pointed-cone apex (32 coincident ring vertices) fared no better: the
// op deleted 64 triangles and left 64 boundary edges.
//
// Each test asserts the CLOSED mesh and the zero degenerate count together. Either alone was
// already reachable before the fix: deletion zeroes the count, and skipping keeps the mesh
// closed. Only the repair gives both.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Modeling.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Model/PwModelCompiler.h"
#include "Model/PwModelDiagnostic.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "GeometryScript/MeshPrimitiveFunctions.h"

namespace TestGeometryRemoveDegeneratesRepairsHelpers
{
// A closed 100^3 box carrying exactly one zero-area CAP. One face's diagonal a-b is split at its
// midpoint c, then the new edge c-y is flipped back to a-b: the face becomes (a, b, c) - c on a-b,
// area zero, every edge >= 70 - plus the real half-face (b, a, y). 14 triangles, still closed.
// That is the shape a boolean or bevel leaves behind, and the one the engine pass cannot see:
// its shortest edge is 70, far above any min_edge_length.
UDynamicMesh* NewBoxWithCap()
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform::Identity, 100.0f, 100.0f, 100.0f, 0, 0, 0,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);

    Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        for (const int32 EdgeID : EditMesh.EdgeIndicesItr())
        {
            const UE::Geometry::FIndex2i EdgeT = EditMesh.GetEdgeT(EdgeID);
            // A face diagonal: both triangles in one polygroup (the primitive groups per face).
            if (EdgeT.B == UE::Geometry::FDynamicMesh3::InvalidID
                || EditMesh.GetTriangleGroup(EdgeT.A) != EditMesh.GetTriangleGroup(EdgeT.B))
            {
                continue;
            }
            const UE::Geometry::FIndex2i Opposing = EditMesh.GetEdgeOpposingV(EdgeID);
            UE::Geometry::FDynamicMesh3::FEdgeSplitInfo SplitInfo;
            if (EditMesh.SplitEdge(EdgeID, SplitInfo, 0.5) != UE::Geometry::EMeshResult::Ok)
            {
                return;
            }
            UE::Geometry::FDynamicMesh3::FEdgeFlipInfo FlipInfo;
            EditMesh.FlipEdge(SplitInfo.NewVertex, Opposing.B, FlipInfo);
            return;
        }
    });
    return Mesh;
}

bool AnyWarningContains(const GeometryOps::FOpResult& Result, const TCHAR* Needle)
{
    for (const FString& Warning : Result.Warnings)
    {
        if (Warning.Contains(Needle))
        {
            return true;
        }
    }
    return false;
}

FString JoinDiagnostics(const TArray<FPwDiagnostic>& Diagnostics)
{
    TArray<FString> Lines;
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        Lines.Add(Diagnostic.ToString());
    }
    return Lines.Num() > 0 ? FString::Join(Lines, TEXT(" | ")) : FString(TEXT("<none>"));
}
} // namespace TestGeometryRemoveDegeneratesRepairsHelpers

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryRemoveDegeneratesRepairsCapTest,
    "PinWright.Geometry.Ops.RemoveDegeneratesRepair.CapIsRepairedAndTheMeshStaysClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryRemoveDegeneratesRepairsCapTest::RunTest(const FString& Parameters)
{
    using namespace TestGeometryRemoveDegeneratesRepairsHelpers;

    struct FCase
    {
        const TCHAR* Name;
        GeometryOps::ERepairMeshMode Mode;
    };
    const FCase Cases[] = {
        { TEXT("repair_or_delete"), GeometryOps::ERepairMeshMode::RepairOrDelete },
        { TEXT("repair_or_skip"), GeometryOps::ERepairMeshMode::RepairOrSkip },
    };

    for (const FCase& Case : Cases)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(NewBoxWithCap());

        // Preconditions: a broken fixture must not read as a verb defect.
        const GeometryUtils::FMeshHealth Before = GeometryUtils::MeasureMeshHealth(Mesh.Get());
        if (!TestEqual(*FString::Printf(TEXT("[%s] fixture has 14 triangles"), Case.Name), Before.TriangleCount, 14)
            || !TestEqual(*FString::Printf(TEXT("[%s] fixture carries one degenerate"), Case.Name), Before.DegenerateTriangles, 1)
            || !TestEqual(*FString::Printf(TEXT("[%s] fixture is closed"), Case.Name), Before.BoundaryEdges, 0))
        {
            continue;
        }

        GeometryOps::FRemoveDegeneratesParams Params; // default thresholds
        Params.Mode = Case.Mode;
        const GeometryOps::FOpResult Op = GeometryOps::RemoveDegenerates(Mesh.Get(), Params);
        TestTrue(*FString::Printf(TEXT("[%s] the op succeeds"), Case.Name), Op.bSuccess);

        const GeometryUtils::FMeshHealth After = GeometryUtils::MeasureMeshHealth(Mesh.Get());
        // Before the fix repair_or_delete returned 13 / 0 / 3 and repair_or_skip 14 / 1 / 0.
        TestEqual(*FString::Printf(TEXT("[%s] the cap is gone"), Case.Name), After.DegenerateTriangles, 0);
        TestEqual(*FString::Printf(TEXT("[%s] and the mesh is still closed"), Case.Name), After.BoundaryEdges, 0);
        TestEqual(*FString::Printf(TEXT("[%s] repaired, not deleted: the count is unchanged"), Case.Name),
            After.TriangleCount, 14);
        TestTrue(*FString::Printf(TEXT("[%s] and still encloses positive volume (%g)"), Case.Name, After.SignedVolume),
            After.SignedVolume > 0.0);
        TestTrue(*FString::Printf(TEXT("[%s] a repair is reported as a change"), Case.Name), Op.bChanged);
        TestFalse(*FString::Printf(TEXT("[%s] no opened-mesh warning"), Case.Name),
            AnyWarningContains(Op, TEXT("boundary edge")));
    }

    // delete_only still deletes - that is its contract, and the .pwmodel boolean cleanup relies
    // on it - and is not warned about: the caller asked for exactly that.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(NewBoxWithCap());
        GeometryOps::FRemoveDegeneratesParams Params;
        Params.Mode = GeometryOps::ERepairMeshMode::DeleteOnly;
        const GeometryOps::FOpResult Op = GeometryOps::RemoveDegenerates(Mesh.Get(), Params);
        const GeometryUtils::FMeshHealth After = GeometryUtils::MeasureMeshHealth(Mesh.Get());
        TestEqual(TEXT("[delete_only] deletes the cap"), After.TriangleCount, 13);
        TestEqual(TEXT("[delete_only] which opens three edges"), After.BoundaryEdges, 3);
        TestFalse(TEXT("[delete_only] without an opened-mesh warning"), AnyWarningContains(Op, TEXT("boundary edge")));
    }

    // A cap whose long edge is already a boundary has no neighbour to split into, so
    // repair_or_delete falls back to deleting it - and must say the mesh opened further.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(NewBoxWithCap());
        Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
        {
            for (const int32 TriangleID : EditMesh.TriangleIndicesItr())
            {
                FVector3d A, B, C;
                EditMesh.GetTriVertices(TriangleID, A, B, C);
                if (FVector3d::CrossProduct(B - A, C - A).Length() < 1e-6)
                {
                    // Remove the real half-face across the cap's long edge (the diagonal).
                    const UE::Geometry::FIndex3i TriE = EditMesh.GetTriEdges(TriangleID);
                    auto EdgeLength = [&EditMesh](int32 EdgeID)
                    {
                        const UE::Geometry::FIndex2i EdgeV = EditMesh.GetEdgeV(EdgeID);
                        return FVector3d::Distance(EditMesh.GetVertex(EdgeV.A), EditMesh.GetVertex(EdgeV.B));
                    };
                    int32 LongEdge = TriE[0];
                    for (int32 Edge = 1; Edge < 3; ++Edge)
                    {
                        if (EdgeLength(TriE[Edge]) > EdgeLength(LongEdge))
                        {
                            LongEdge = TriE[Edge];
                        }
                    }
                    const UE::Geometry::FIndex2i EdgeT = EditMesh.GetEdgeT(LongEdge);
                    EditMesh.RemoveTriangle((EdgeT.A == TriangleID) ? EdgeT.B : EdgeT.A);
                    return;
                }
            }
        });
        const GeometryUtils::FMeshHealth Before = GeometryUtils::MeasureMeshHealth(Mesh.Get());
        if (TestEqual(TEXT("[boundary cap] fixture: the long edge's neighbour is gone"), Before.BoundaryEdges, 3)
            && TestEqual(TEXT("[boundary cap] fixture: the cap is still there"), Before.DegenerateTriangles, 1))
        {
            const GeometryOps::FOpResult Op =
                GeometryOps::RemoveDegenerates(Mesh.Get(), GeometryOps::FRemoveDegeneratesParams());
            const GeometryUtils::FMeshHealth After = GeometryUtils::MeasureMeshHealth(Mesh.Get());
            TestEqual(TEXT("[boundary cap] the unrepairable cap is deleted"), After.DegenerateTriangles, 0);
            TestEqual(TEXT("[boundary cap] which opens one more edge"), After.BoundaryEdges, 4);
            TestTrue(*FString::Printf(TEXT("[boundary cap] and the response names it. warnings: %s"),
                *FString::Join(Op.Warnings, TEXT(" | "))),
                AnyWarningContains(Op, TEXT("opened 1 boundary edge(s)")));
        }
    }

    // A cap FOLDED back over its neighbour must be left alone. Two triangles in z=0 sharing A-B:
    // the cap (A, B, Apex) with Apex on the same side as Far, and the neighbour (B, A, Far). At
    // min_triangle_area=10 the cap (area 5) is degenerate, its edges (10, 5.1, 5.1) are all long,
    // and both replacement triangles carry enough area (22.5, 72.5) - but (A, Apex, Far) would
    // face opposite to (A, B, Far). An unsigned area test let that through as a flipped triangle.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(NewObject<UDynamicMesh>(GetTransientPackage()));
        Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
        {
            EditMesh.DiscardAttributes();
            const int32 A = EditMesh.AppendVertex(FVector3d(0, 0, 0));
            const int32 B = EditMesh.AppendVertex(FVector3d(10, 0, 0));
            const int32 Apex = EditMesh.AppendVertex(FVector3d(5, -1, 0));
            const int32 Far = EditMesh.AppendVertex(FVector3d(100, -11, 0));
            EditMesh.AppendTriangle(A, B, Apex);
            EditMesh.AppendTriangle(B, A, Far);
        });
        auto CountUnder = [](UDynamicMesh* InMesh, double Threshold)
        {
            int32 Count = 0;
            InMesh->ProcessMesh([&Count, Threshold](const UE::Geometry::FDynamicMesh3& ReadMesh)
            {
                for (const int32 TriangleID : ReadMesh.TriangleIndicesItr())
                {
                    Count += ReadMesh.GetTriArea(TriangleID) < Threshold ? 1 : 0;
                }
            });
            return Count;
        };
        if (TestEqual(TEXT("[folded cap] fixture: two triangles"), Mesh->GetTriangleCount(), 2)
            && TestEqual(TEXT("[folded cap] fixture: the cap is under the threshold"), CountUnder(Mesh.Get(), 10.0), 1))
        {
            GeometryOps::FRemoveDegeneratesParams Params;
            Params.Mode = GeometryOps::ERepairMeshMode::RepairOrSkip;
            Params.MinTriangleArea = 10.0;
            const GeometryOps::FOpResult Op = GeometryOps::RemoveDegenerates(Mesh.Get(), Params);
            TestTrue(TEXT("[folded cap] the op succeeds"), Op.bSuccess);
            TestEqual(TEXT("[folded cap] the repair is refused, so the cap is still there"),
                CountUnder(Mesh.Get(), 10.0), 1);
            TestEqual(TEXT("[folded cap] and nothing was added or removed"), Mesh->GetTriangleCount(), 2);
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryRemoveDegeneratesDefaultThresholdTest,
    "PinWright.Geometry.Ops.RemoveDegeneratesRepair.DefaultLeavesSmallRealTrianglesAlone",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryRemoveDegeneratesDefaultThresholdTest::RunTest(const FString& Parameters)
{
    // The engine default min_triangle_area of 0.001 deleted 5,284 real triangles from a bevelled
    // centimetre-scale part. A 0.04-unit box has 0.0008-area triangles: real, under 0.001, and
    // ~800x the health threshold - the old default deleted all twelve.
    TStrongObjectPtr<UDynamicMesh> Mesh(NewObject<UDynamicMesh>(GetTransientPackage()));
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh.Get(), Options, FTransform::Identity, 0.04f, 0.04f, 0.04f, 0, 0, 0,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    const GeometryUtils::FMeshHealth Before = GeometryUtils::MeasureMeshHealth(Mesh.Get());
    if (!TestEqual(TEXT("fixture: 12 triangles, none degenerate by the health threshold"), Before.DegenerateTriangles, 0)
        || !TestEqual(TEXT("fixture: 12 triangles"), Before.TriangleCount, 12))
    {
        return true;
    }

    const GeometryOps::FRemoveDegeneratesParams Params;
    TestEqual(TEXT("the default threshold is the health report's"),
        Params.MinTriangleArea, GeometryUtils::DegenerateAreaEpsilon, 1e-12);
    GeometryOps::RemoveDegenerates(Mesh.Get(), Params);

    const GeometryUtils::FMeshHealth After = GeometryUtils::MeasureMeshHealth(Mesh.Get());
    TestEqual(TEXT("at the defaults a small real box keeps every triangle"), After.TriangleCount, 12);
    TestEqual(TEXT("and stays closed"), After.BoundaryEdges, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelRemoveDegeneratesConeApexTest,
    "PinWright.Model.RemoveDegeneratesRepair.PointedConeApexIsWeldedClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelRemoveDegeneratesConeApexTest::RunTest(const FString& Parameters)
{
    using namespace TestGeometryRemoveDegeneratesRepairsHelpers;

    // The case model.authoring documented as "the wrong fix": `cone top_radius=0` builds its apex
    // as a ring of coincident vertices, and the op deleted the zero-area triangles there and came
    // back with 64 boundary edges. Repaired, the ring collapses to one apex vertex.
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;

    const TCHAR* Cone = TEXT("    cone base_radius=148 top_radius=0 height=220 segments=32\n");

    const FPwModelCompileResult Bare = FPwModelCompiler::Compile(
        FString(TEXT("pwmodel 0\npart spire {\n")) + Cone + TEXT("}\n"), Options);
    if (!TestTrue(*FString::Printf(TEXT("fixture: the bare cone compiles. %s"), *JoinDiagnostics(Bare.Diagnostics)),
            Bare.bSuccess)
        || !TestTrue(*FString::Printf(TEXT("fixture: the bare cone carries degenerates (%d)"), Bare.MeshDegenerateTriangles),
            Bare.MeshDegenerateTriangles > 0)
        || !TestEqual(TEXT("fixture: the bare cone is closed"), Bare.MeshBoundaryEdges, 0))
    {
        return true;
    }

    const FPwModelCompileResult Repaired = FPwModelCompiler::Compile(
        FString(TEXT("pwmodel 0\npart spire {\n")) + Cone + TEXT("    remove_degenerates\n}\n"), Options);
    TestTrue(*FString::Printf(TEXT("the repaired cone compiles. %s"), *JoinDiagnostics(Repaired.Diagnostics)),
        Repaired.bSuccess);
    TestEqual(TEXT("no degenerate triangles remain"), Repaired.MeshDegenerateTriangles, 0);
    TestEqual(TEXT("and the cone is still closed (64 boundary edges before the fix)"), Repaired.MeshBoundaryEdges, 0);
    TestTrue(*FString::Printf(TEXT("and encloses positive volume (%g)"), Repaired.MeshSignedVolume),
        Repaired.MeshSignedVolume > 0.0);
    return true;
}
