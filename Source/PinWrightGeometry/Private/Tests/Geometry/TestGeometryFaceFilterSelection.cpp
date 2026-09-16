// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the two face-selection defects of extrude / inset / outset / offset_faces.
//
// DEFECT 1 - a face_direction that matched ZERO faces silently ran on the WHOLE MESH.
// GeometryOpsModeling_BuildSelection left the engine selection empty in that case, and every
// selection-consuming Geometry Script entry point reads an empty selection as "every triangle"
// (MeshModelingFunctions.cpp:613, :722, :815 - an explicit `Selection.GetNumSelected() == 0`
// branch that iterates TriangleIndicesItr()). That convention is right for "no filter given" and
// catastrophic for "filter given, satisfied by nothing": the author named a subset and got the
// entire mesh operated on, with no warning, no diagnostic, and a result plausible enough to ship.
// It DID ship - two extrudes in an example model carried it. The fix makes that case a NO-OP with
// a loud warning, which is exactly what the engine's own
// EGeometryScriptEmptySelectionBehavior::EmptySelection means (the four ops here have no field to
// pass it to, so it has to be implemented on PinWright's side of the call).
//
// DEFECT 2 - on an OPEN mesh a MATCHING face_direction produced strictly worse geometry than no
// filter at all: 24 triangles (a closed shell) unfiltered vs 14 with one wing's skin missing.
// Traced to FOffsetMeshRegion::Apply (OffsetMeshRegion.cpp:40-45), where
// `bIsSolid = GrowToConnectedTriangles(region).Num() == region.Num()` is true ONLY for a COMPLETE
// connected component, and bIsSolid is the sole gate on the duplicate-and-cap that closes an open
// sheet (:598-602, flipped outward at :734-745). A filter either drops whole components (never
// extruded, no side walls) or leaves a partial one (moved and skirted, never closed). No engine
// option repairs it - bSolidsToShells is already the engine default and bIsSolid is derived, not
// settable - so the op says so instead.
//
// These run against transient UDynamicMesh objects with no actor, no editor world and no
// FHandlerContext, the same property TestGeometryOpsModeling.cpp depends on.
//
// Counterfactual for every case below: revert the MatchedNothing early-return and the first three
// tests fail on `TrianglesAfter == TrianglesBefore` / `bChanged == false` / the missing warning;
// revert the open-mesh warning and the last test fails on the missing warning while its geometry
// assertions stay green (which is the point - the geometry is the engine's contract, the warning
// is the fix).
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Modeling.h"

#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "GeometryScript/GeometryScriptSelectionTypes.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshQueryFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so a plainly-named
// anonymous-namespace helper would collide with a sibling test TU once Unity merges them.

// One planar rectangle in XY: 2 triangles, every normal +Z, 4 open boundary edges. The simplest
// mesh on which a +Z filter matches EVERYTHING and a -Z filter matches NOTHING, which is what
// separates the two zero-count cases the fix exists to distinguish.
UDynamicMesh* GeomFaceFilterTest_NewSheet()
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendRectangleXY(
        Mesh, Options, FTransform::Identity, 100.0f, 100.0f, 0, 0, nullptr);
    return Mesh;
}

// The open TWO-SIDED sheet the 24-vs-14 measurement was taken on: two rectangles that share no
// vertex, the second rolled 180 degrees about X so its normals point -Z. 4 triangles, 8 boundary
// edges, and - the load-bearing property - TWO connected components, so a +Z filter selects one
// of them WHOLE. Roll rather than pitch/yaw because it is the rotation that inverts Z without
// moving the rectangle off its own plane.
UDynamicMesh* GeomFaceFilterTest_NewTwoSidedSheet()
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendRectangleXY(
        Mesh, Options, FTransform::Identity, 100.0f, 100.0f, 0, 0, nullptr);

    const FTransform Flipped(FRotator(0.0, 0.0, 180.0), FVector(0.0, 0.0, 200.0));
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendRectangleXY(
        Mesh, Options, Flipped, 100.0f, 100.0f, 0, 0, nullptr);
    return Mesh;
}

bool GeomFaceFilterTest_AnyWarningContains(
    const GeometryOps::FOpResult& Op, const TCHAR* Fragment)
{
    for (const FString& Warning : Op.Warnings)
    {
        if (Warning.Contains(Fragment))
        {
            return true;
        }
    }
    return false;
}

// A face filter aimed at -Z on a sheet whose every normal is +Z: 180 degrees away, so nothing is
// within the 45-degree tolerance.
GeometryOps::FFaceSelectionSpec GeomFaceFilterTest_MatchesNothing()
{
    GeometryOps::FFaceSelectionSpec Spec;
    Spec.bHasDirection = true;
    Spec.Direction = FVector(0, 0, -1);
    Spec.AngleTolerance = 45.0;
    return Spec;
}

GeometryOps::FFaceSelectionSpec GeomFaceFilterTest_MatchesTop()
{
    GeometryOps::FFaceSelectionSpec Spec;
    Spec.bHasDirection = true;
    Spec.Direction = FVector(0, 0, 1);
    Spec.AngleTolerance = 45.0;
    return Spec;
}
}

// ============================================================================
// Defect 1: a filter that matches nothing is a no-op, not the whole mesh
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryFaceFilterExtrudeMatchedNothingTest,
    "PinWright.Geometry.Ops.Modeling.ExtrudeWithAFilterThatMatchesNothingDoesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryFaceFilterExtrudeMatchedNothingTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeomFaceFilterTest_NewSheet());
    const int32 TrianglesBefore = Mesh->GetTriangleCount();
    TestEqual(TEXT("the fixture sheet is 2 triangles"), TrianglesBefore, 2);

    GeometryOps::FExtrudeParams Params;
    Params.Distance = 10.0;
    Params.Faces = GeomFaceFilterTest_MatchesNothing();

    GeometryOps::FFaceOpOutcome Outcome;
    const GeometryOps::FOpResult Op = GeometryOps::Extrude(Mesh.Get(), Params, &Outcome);

    // Success, not failure: an FOpResult failure aborts the whole part in the .pwmodel compiler,
    // which would turn one variant's legitimately-empty filter into a failed build.
    TestTrue(TEXT("a filter that matches nothing still SUCCEEDS"), Op.bSuccess);

    // The geometry assertion the whole fix exists for. Before it, this call extruded all 2
    // triangles and came back at 12.
    TestEqual(TEXT("the mesh is untouched - the triangle count did not move"),
        Op.TrianglesAfter, TrianglesBefore);
    TestEqual(TEXT("and neither did the vertex count"), Op.VerticesAfter, Op.VerticesBefore);
    TestFalse(TEXT("so the op reports that it changed nothing"), Op.bChanged);
    TestEqual(TEXT("no face was selected"), Outcome.FacesSelected, 0);
    TestTrue(TEXT("and that 0 is flagged as 'the filter matched nothing', not 'the whole mesh'"),
        Outcome.bFilterMatchedNothing);

    // The warning has to be actionable on its own: the direction that matched nothing, the
    // tolerance used, the count it found and the mesh's triangle count.
    TestTrue(TEXT("extrude warns, naming itself"),
        GeomFaceFilterTest_AnyWarningContains(Op, TEXT("extrude")));
    TestTrue(TEXT("the warning names the direction that matched nothing"),
        GeomFaceFilterTest_AnyWarningContains(Op, TEXT("(0.000, 0.000, -1.000)")));
    TestTrue(TEXT("the warning names the angle tolerance it used"),
        GeomFaceFilterTest_AnyWarningContains(Op, TEXT("45.0 deg")));
    TestTrue(TEXT("the warning names the 0 faces it found and the mesh's triangle count"),
        GeomFaceFilterTest_AnyWarningContains(Op, TEXT("matched 0 of the mesh's 2 triangles")));
    TestTrue(TEXT("the warning says the op did nothing"),
        GeomFaceFilterTest_AnyWarningContains(Op, TEXT("did NOTHING")));

    // The whole-mesh warning is a DIFFERENT case and must not be borrowed for this one.
    TestFalse(TEXT("and it is not the no-face-direction whole-mesh warning"),
        GeomFaceFilterTest_AnyWarningContains(Op, TEXT("No face direction given")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryFaceFilterSharedAcrossOpsTest,
    "PinWright.Geometry.Ops.Modeling.EveryFaceOpNoOpsOnAFilterThatMatchesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryFaceFilterSharedAcrossOpsTest::RunTest(const FString& Parameters)
{
    // The defect is in the shared selection helper, so the fix is shared. inset, outset and
    // offset_faces are asserted here for the same reason extrude is asserted above: three of the
    // four never warned about anything at all before this change, so a fix applied only to
    // extrude would have looked complete and left three verbs silently operating on whole meshes.
    const auto CheckNoOp = [this](const TCHAR* Label, const GeometryOps::FOpResult& Op,
                                  const GeometryOps::FFaceOpOutcome& Outcome,
                                  const TCHAR* OpNameInWarning)
    {
        TestTrue(FString::Printf(TEXT("%s succeeds"), Label), Op.bSuccess);
        TestEqual(FString::Printf(TEXT("%s left the triangle count alone"), Label),
            Op.TrianglesAfter, Op.TrianglesBefore);
        TestEqual(FString::Printf(TEXT("%s left the vertex count alone"), Label),
            Op.VerticesAfter, Op.VerticesBefore);
        TestFalse(FString::Printf(TEXT("%s reports no change"), Label), Op.bChanged);
        TestEqual(FString::Printf(TEXT("%s selected no face"), Label), Outcome.FacesSelected, 0);
        TestTrue(FString::Printf(TEXT("%s flags the zero-match case"), Label),
            Outcome.bFilterMatchedNothing);
        TestTrue(FString::Printf(TEXT("%s warns and names itself"), Label),
            GeomFaceFilterTest_AnyWarningContains(Op, OpNameInWarning));
        TestTrue(FString::Printf(TEXT("%s names the direction that matched nothing"), Label),
            GeomFaceFilterTest_AnyWarningContains(Op, TEXT("(0.000, 0.000, -1.000)")));
    };

    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeomFaceFilterTest_NewSheet());
        GeometryOps::FInsetParams Params;
        Params.Distance = 5.0;
        Params.Faces = GeomFaceFilterTest_MatchesNothing();
        GeometryOps::FFaceOpOutcome Outcome;
        const GeometryOps::FOpResult Op = GeometryOps::Inset(Mesh.Get(), Params, &Outcome);
        CheckNoOp(TEXT("inset"), Op, Outcome, TEXT("inset"));
    }

    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeomFaceFilterTest_NewSheet());
        GeometryOps::FOutsetParams Params;
        Params.Distance = 5.0;
        Params.Faces = GeomFaceFilterTest_MatchesNothing();
        GeometryOps::FFaceOpOutcome Outcome;
        const GeometryOps::FOpResult Op = GeometryOps::Outset(Mesh.Get(), Params, &Outcome);
        CheckNoOp(TEXT("outset"), Op, Outcome, TEXT("outset"));
    }

    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeomFaceFilterTest_NewSheet());
        GeometryOps::FOffsetFacesParams Params;
        Params.Distance = 5.0;
        Params.Faces = GeomFaceFilterTest_MatchesNothing();
        GeometryOps::FFaceOpOutcome Outcome;
        const GeometryOps::FOpResult Op = GeometryOps::OffsetFaces(Mesh.Get(), Params, &Outcome);
        CheckNoOp(TEXT("offset_faces"), Op, Outcome, TEXT("offset_faces"));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryFaceFilterControlsTest,
    "PinWright.Geometry.Ops.Modeling.AMatchingFilterAndNoFilterBothStillRun",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryFaceFilterControlsTest::RunTest(const FString& Parameters)
{
    // Control 1: the fix must not be "never run anything". A +Z filter on a +Z sheet matches, so
    // the op has to run and move counts.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeomFaceFilterTest_NewSheet());
        GeometryOps::FExtrudeParams Params;
        Params.Distance = 10.0;
        Params.Faces = GeomFaceFilterTest_MatchesTop();

        GeometryOps::FFaceOpOutcome Outcome;
        const GeometryOps::FOpResult Op = GeometryOps::Extrude(Mesh.Get(), Params, &Outcome);

        TestTrue(TEXT("a matching filter succeeds"), Op.bSuccess);
        TestTrue(TEXT("a matching filter selects faces"), Outcome.FacesSelected > 0);
        TestFalse(TEXT("and is NOT flagged as a zero match"), Outcome.bFilterMatchedNothing);
        TestTrue(TEXT("a matching filter actually extrudes"),
            Op.TrianglesAfter > Op.TrianglesBefore);
        TestTrue(TEXT("and reports the change"), Op.bChanged);
        TestFalse(TEXT("a matching filter carries no zero-match warning"),
            GeomFaceFilterTest_AnyWarningContains(Op, TEXT("did NOTHING")));
    }

    // Control 2 - the compatibility guard. No direction at all must keep the whole-mesh behaviour
    // byte for byte: the selection stays empty, the engine expands it to every triangle, and the
    // only warning is the pre-existing whole-mesh one.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeomFaceFilterTest_NewSheet());
        GeometryOps::FExtrudeParams Params;
        Params.Distance = 10.0;

        GeometryOps::FFaceOpOutcome Outcome;
        const GeometryOps::FOpResult Op = GeometryOps::Extrude(Mesh.Get(), Params, &Outcome);

        TestTrue(TEXT("an unfiltered extrude succeeds"), Op.bSuccess);
        TestEqual(TEXT("an empty selection still reports 0 faces"), Outcome.FacesSelected, 0);
        TestFalse(TEXT("and that 0 is NOT the zero-match case"), Outcome.bFilterMatchedNothing);
        TestTrue(TEXT("the whole mesh is still extruded"), Op.TrianglesAfter > Op.TrianglesBefore);
        TestTrue(TEXT("and the change is still reported"), Op.bChanged);
        TestTrue(TEXT("the pre-existing whole-mesh warning is unchanged"),
            GeomFaceFilterTest_AnyWarningContains(Op, TEXT("No face direction given")));
        TestFalse(TEXT("and it does not pick up the zero-match warning"),
            GeomFaceFilterTest_AnyWarningContains(Op, TEXT("did NOTHING")));
    }

    // Control 3: a zero direction is an author error, not a filter. It is still read as +Z (the
    // engine would otherwise normalize a zero vector), but it says so now.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeomFaceFilterTest_NewSheet());
        GeometryOps::FExtrudeParams Params;
        Params.Distance = 10.0;
        Params.Faces.bHasDirection = true;
        Params.Faces.Direction = FVector::ZeroVector;

        GeometryOps::FFaceOpOutcome Outcome;
        const GeometryOps::FOpResult Op = GeometryOps::Extrude(Mesh.Get(), Params, &Outcome);

        TestTrue(TEXT("a zero face direction succeeds"), Op.bSuccess);
        TestTrue(TEXT("and warns that it was read as +Z"),
            GeomFaceFilterTest_AnyWarningContains(Op, TEXT("(0, 0, 0)")));
    }

    return true;
}

// ============================================================================
// Defect 2: a MATCHING filter on an open mesh returns less closed geometry
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryFaceFilterOpenSheetTest,
    "PinWright.Geometry.Ops.Modeling.ExtrudeWarnsThatAFilteredOpenSheetIsNotClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryFaceFilterOpenSheetTest::RunTest(const FString& Parameters)
{
    // Unfiltered: every component is fully selected, so every component takes
    // FOffsetMeshRegion's bIsSolid path (OffsetMeshRegion.cpp:40-45) and is duplicated, flipped
    // and skirted into a closed box. The result has NO open boundary edges.
    int32 UnfilteredTriangles = 0;
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeomFaceFilterTest_NewTwoSidedSheet());
        TestEqual(TEXT("the two-sided sheet is 4 triangles"), Mesh->GetTriangleCount(), 4);
        TestTrue(TEXT("and it is OPEN before the extrude"),
            UGeometryScriptLibrary_MeshQueryFunctions::GetNumOpenBorderEdges(Mesh.Get()) > 0);

        GeometryOps::FExtrudeParams Params;
        Params.Distance = 10.0;

        GeometryOps::FFaceOpOutcome Outcome;
        const GeometryOps::FOpResult Op = GeometryOps::Extrude(Mesh.Get(), Params, &Outcome);
        UnfilteredTriangles = Op.TrianglesAfter;

        AddInfo(FString::Printf(TEXT("unfiltered extrude: %d -> %d triangles"),
            Op.TrianglesBefore, Op.TrianglesAfter));

        TestTrue(TEXT("an unfiltered extrude of an open sheet succeeds"), Op.bSuccess);
        TestEqual(TEXT("an unfiltered extrude of an open sheet leaves NO open boundary edges"),
            UGeometryScriptLibrary_MeshQueryFunctions::GetNumOpenBorderEdges(Mesh.Get()), 0);
        TestFalse(TEXT("and carries no open-mesh partial-selection warning"),
            GeomFaceFilterTest_AnyWarningContains(Op, TEXT("boundary edges)")));
    }

    // Filtered on the +Z wing: that wing is a whole component so it still solidifies, but the -Z
    // wing is never touched and stays a bare sheet. The result is a closed box PLUS an
    // un-extruded, zero-thickness wing - the "one wing's skin missing" of the report.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeomFaceFilterTest_NewTwoSidedSheet());

        GeometryOps::FExtrudeParams Params;
        Params.Distance = 10.0;
        Params.Faces = GeomFaceFilterTest_MatchesTop();

        GeometryOps::FFaceOpOutcome Outcome;
        const GeometryOps::FOpResult Op = GeometryOps::Extrude(Mesh.Get(), Params, &Outcome);

        AddInfo(FString::Printf(TEXT("filtered extrude: %d -> %d triangles, %d faces selected"),
            Op.TrianglesBefore, Op.TrianglesAfter, Outcome.FacesSelected));

        TestTrue(TEXT("a filtered extrude of an open sheet succeeds"), Op.bSuccess);
        TestTrue(TEXT("the +Z filter matched a strict subset of the two-sided sheet"),
            Outcome.FacesSelected > 0 && Outcome.FacesSelected < Op.TrianglesBefore);

        // The geometry claim: the filtered call is measurably LESS closed than the unfiltered
        // one, on the same input. This is the engine's contract for a sub-region, not something
        // PinWright can repair - which is why the fix is the warning below.
        TestTrue(TEXT("a filtered extrude leaves the mesh OPEN where the unfiltered one closed it"),
            UGeometryScriptLibrary_MeshQueryFunctions::GetNumOpenBorderEdges(Mesh.Get()) > 0);
        TestTrue(*FString::Printf(
                TEXT("and produces less geometry than the unfiltered call (%d vs %d)"),
                Op.TrianglesAfter, UnfilteredTriangles),
            Op.TrianglesAfter < UnfilteredTriangles);

        // THE FIX. Without it the caller gets the degraded shell with no diagnostic at all.
        TestTrue(TEXT("extrude warns that a filtered open mesh is not closed"),
            GeomFaceFilterTest_AnyWarningContains(
                Op, TEXT("returns LESS closed geometry than the same call with no face direction")));
        TestTrue(TEXT("the warning names the mesh as open, with its boundary edge count"),
            GeomFaceFilterTest_AnyWarningContains(Op, TEXT("boundary edges)")));
        TestTrue(TEXT("the warning names the unselected triangles left flat"),
            GeomFaceFilterTest_AnyWarningContains(Op, TEXT("unselected triangles are left flat")));
    }

    // A CLOSED solid must stay quiet, or the warning is noise on the meshes a face filter is
    // actually for: extruding the +Z face of a box is the intended use and stays closed.
    {
        TStrongObjectPtr<UDynamicMesh> Box(NewObject<UDynamicMesh>(GetTransientPackage()));
        FGeometryScriptPrimitiveOptions PrimitiveOptions;
        UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
            Box.Get(), PrimitiveOptions, FTransform::Identity, 100.0f, 100.0f, 100.0f, 0, 0, 0,
            EGeometryScriptPrimitiveOriginMode::Center, nullptr);

        GeometryOps::FExtrudeParams Params;
        Params.Distance = 10.0;
        Params.Faces = GeomFaceFilterTest_MatchesTop();

        GeometryOps::FFaceOpOutcome Outcome;
        const GeometryOps::FOpResult Op = GeometryOps::Extrude(Box.Get(), Params, &Outcome);

        TestTrue(TEXT("a filtered extrude of a closed box succeeds"), Op.bSuccess);
        TestTrue(TEXT("and selects a strict subset of its 12 triangles"),
            Outcome.FacesSelected > 0 && Outcome.FacesSelected < 12);
        TestFalse(TEXT("a closed solid does NOT trip the open-mesh warning"),
            GeomFaceFilterTest_AnyWarningContains(Op, TEXT("boundary edges)")));
    }

    return true;
}
