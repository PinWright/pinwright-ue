// Copyright (c) 2026 Alexander Penkin. MIT License.

// The modeling / warp / repair half of the op widening: the ops whose published parameter set
// was a fraction of the engine options struct behind them, and now is not.
//
// Sibling file, DIFFERENT ops: Tests/Geometry/TestGeometryWidenedOpOptions.cpp covers
// simplify_mesh, remesh_uniform, the two `uv` modes, the symmetric booleans and trim. Nothing is
// tested twice; the two files share only the shape of the argument below.
//
// Two questions per op, and they are different questions:
//
//  1. DOES THE DEFAULT PATH STILL DO WHAT IT DID? Answered structurally rather than by geometry,
//     in FGeometryOpsModelingOptionsDefaultsMatchEngineTest: every NEWLY PUBLISHED field of every
//     widened params struct is compared against the DEFAULT-CONSTRUCTED engine options struct it
//     maps onto. Omitting a parameter therefore provably reproduces the call the op made before
//     it had one. This is stronger than a before/after geometry comparison, which can only show
//     that two runs agree - this shows WHY they agree and names the field when they stop.
//
//     Enums are compared by ORDINAL, which is not a shortcut: every enum in GeometryOps_Modeling.h
//     is documented as mirroring its Geometry Script counterpart "one-for-one and in the same
//     order", and the .cpp translates with a defaultless switch. The ordinal comparison is the
//     only place that documented claim is checked, so a value inserted upstream is caught here as
//     well as by the switch.
//
//     The DELIBERATE divergences - the ones that must NOT be moved onto the engine value - are
//     asserted separately and by literal in FGeometryOpsModelingOptionsDocumentedOverridesTest.
//     Each is a shipped default that predates the widening, so matching the engine there would
//     silently change every existing document.
//
//  2. DOES A NON-DEFAULT VALUE REACH THE ENGINE? Answered per op by running the op twice and
//     showing the geometry differs. Each test picks the field whose effect is STRUCTURAL - a
//     triangle or vertex count, an axis of the bounding box, an overlay element count - rather
//     than metric, because "these two float digests disagree" is a coin flip on a simple fixture
//     and a flaky test is worse than no test. Where an op's own field has no structural effect on
//     any cheap fixture, the test says so rather than asserting something fragile.
//
// NOT COVERED HERE, and deliberately:
//   - smooth, relax, noise_deform. Their one remaining engine field is
//     EGeometryScriptEmptySelectionBehavior, which none of the three publishes on purpose (see
//     the comment above FSmoothParams): its only non-default value turns the op into an
//     unconditional no-op. There is no new parameter to prove reaches anything.
//   - mirror's weld tolerance, owned and tested elsewhere.
//   - weld_vertices' bOnlyUniquePairs. Its effect needs a mesh with AMBIGUOUS duplicate-edge
//     matches, which no primitive generator produces and a hand-built one would be a test of the
//     fixture. The defaults test pins the field; nothing else here can honestly pin it.
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/ErrorCodes.h"

#include "Handlers/Geometry/GeometryOps_Boolean.h"
#include "Handlers/Geometry/GeometryOps_Modeling.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "GeometryScript/MeshBooleanFunctions.h"
#include "GeometryScript/MeshDeformFunctions.h"
#include "GeometryScript/MeshModelingFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshRepairFunctions.h"
#include "GeometryScript/MeshSubdivideFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so a plainly-named
// anonymous-namespace helper would collide with a sibling test TU once Unity merges them.

UDynamicMesh* ModelingOptionsTest_NewMesh()
{
    return NewObject<UDynamicMesh>(GetTransientPackage());
}

// A centred 100^3 box: 12 triangles, 6 polygroups (the primitive default is PerFace), a
// populated UV0 overlay and hard per-face normals. Centred rather than base-origin because the
// warp deformers measure their extents about the ORIGIN, so a base-origin box would sit entirely
// on one side of every extent this file sets.
//
// Steps is AppendBox's EdgeVertices, i.e. the number of VERTICES along each edge
// (MeshPrimitiveFunctions.cpp:236) - not a subdivision count. 0 and 2 both mean the plain
// 12-triangle box every caller here wants; 5 means a 5x5 vertex grid per face, which is what
// gives a per-polygroup face region INTERIOR vertices. Exactly one test needs that; see
// OutsetAreaScaleReachesTheEngine.
UDynamicMesh* ModelingOptionsTest_NewBoxMesh(int32 Steps = 0)
{
    UDynamicMesh* Mesh = ModelingOptionsTest_NewMesh();
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform::Identity, 100.0f, 100.0f, 100.0f, Steps, Steps, Steps,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

// A centred 50 x 50 x 200 column with 8 steps along Z. The steps are load-bearing: a warp
// deformer moves VERTICES, and a box with no Z subdivision has vertices only at +-100, both
// outside every extent used here - the warp would then be a rigid transform of two corner rings
// and several of its parameters would provably change nothing on it.
UDynamicMesh* ModelingOptionsTest_NewColumnMesh()
{
    UDynamicMesh* Mesh = ModelingOptionsTest_NewMesh();
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform::Identity, 50.0f, 50.0f, 200.0f, 0, 0, 8,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

// An OPEN, fully UV'd, interior-bearing mesh: one planar rectangle in XY at 4x4 steps. Open is
// what makes shell stitch a boundary loop at all; the steps give the offset solve interior
// vertices to move, so pinning the boundary is distinguishable from not pinning it. The
// generator populates UV channel 0 on every triangle, which is what the shell crash guard
// demands of an open mesh.
UDynamicMesh* ModelingOptionsTest_NewRectangleMesh()
{
    UDynamicMesh* Mesh = ModelingOptionsTest_NewMesh();
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendRectangleXY(
        Mesh, Options, FTransform::Identity, 100.0f, 100.0f, 4, 4, nullptr);
    return Mesh;
}

// A SMOOTH-SHADED, CURVED, UV'd sphere - and every one of those words is why two tests below use
// it instead of the box.
//
// The box is flat-shaded: its normal overlay carries one hard normal per face. PN tessellation
// derives its control points from those element normals (PNTriangles.cpp ComputeControlPoints),
// and on a face whose three corner normals are all the face normal every control point lands
// exactly on the flat triangle - so subdividing a box produces a flat tessellation whose
// recomputed normals are bit-identical to the interpolated ones, and `recompute_normals` is a
// provable no-op there rather than an untested one. The same flatness kills recompute_tangents:
// a box face has one tangent frame across both of its triangles, so averaging (FastMikkT) and
// not averaging (PerTriangle) cannot disagree.
//
// A lat-long sphere has curvature, smooth shared normals, and a real UV unwrap, which is what
// gives both parameters something to change. Kept small (8 x 12) because subdivide runs it twice
// and quadruples the triangle count per iteration.
UDynamicMesh* ModelingOptionsTest_NewSphereMesh()
{
    UDynamicMesh* Mesh = ModelingOptionsTest_NewMesh();
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
        Mesh, Options, FTransform::Identity, 50.0f, 8, 12,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

// Two overlapping boxes in ONE mesh, so the mesh self-intersects and self_union has something to
// resolve. Appending twice into the same UDynamicMesh is what makes it a SELF union rather than a
// two-operand boolean.
//
// BOTH BOXES ARE SUBDIVIDED, and that is what makes simplify_output measurable on this fixture
// rather than a coin flip. FMeshSelfUnion::SimplifyAlongNewEdges (MeshSelfUnion.cpp:497) walks
// only the cut-boundary edges and collapses a vertex only when FLocalPlanarSimplify::IsFlat finds
// its whole one-ring coplanar. Two PLAIN axis-aligned boxes offer none: the intersection curve on
// each cut face runs through that face's own diagonal vertex, so the resolve already produces the
// minimal triangulation and every cut-boundary vertex sits on a box edge. Measured on the plain
// pair: 36 triangles with the flag on and 36 with it off.
//
// The third AppendBox argument triple is EdgeVertices, i.e. the number of VERTICES along an edge
// (MeshPrimitiveFunctions.cpp:236). 4 puts grid lines at -50/-16.7/+16.7/+50 on the first box
// while the second box's faces cut at -10 and +50, so the cut crosses grid edges at points
// interior to a flat face - a coplanar one-ring on a straight cut, which is what the collapse
// needs.
UDynamicMesh* ModelingOptionsTest_NewSelfIntersectingMesh()
{
    UDynamicMesh* Mesh = ModelingOptionsTest_NewMesh();
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform::Identity, 100.0f, 100.0f, 100.0f, 4, 4, 4,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform(FVector(40.0, 40.0, 40.0)), 100.0f, 100.0f, 100.0f, 4, 4, 4,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

// A box with two adjacent triangles removed, leaving a FOUR-EDGE hole.
//
// Four edges rather than three, deliberately: FillAllMeshHoles sets bQuickFillSmallHoles
// unconditionally (MeshRepairFunctions.cpp), and that path fills SINGLE-TRIANGLE holes without
// consulting FillMethod at all. A three-edge hole would therefore come back identical under every
// method and the fill_holes test would pass for the wrong reason - it would prove nothing.
UDynamicMesh* ModelingOptionsTest_NewHoledBoxMesh()
{
    UDynamicMesh* Mesh = ModelingOptionsTest_NewBoxMesh();
    Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        // The two triangles of one polygroup are one quad face of the box, so removing a group
        // removes exactly one quad and leaves a four-edge boundary loop.
        const int32 TargetGroup = EditMesh.GetTriangleGroup(0);
        TArray<int32> Doomed;
        for (int32 TriangleID : EditMesh.TriangleIndicesItr())
        {
            if (EditMesh.GetTriangleGroup(TriangleID) == TargetGroup)
            {
                Doomed.Add(TriangleID);
            }
        }
        for (int32 TriangleID : Doomed)
        {
            EditMesh.RemoveTriangle(TriangleID);
        }
    });
    return Mesh;
}

int32 ModelingOptionsTest_TriangleCount(UDynamicMesh* Mesh)
{
    int32 Count = 0;
    Mesh->ProcessMesh([&Count](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        Count = ReadMesh.TriangleCount();
    });
    return Count;
}

int32 ModelingOptionsTest_VertexCount(UDynamicMesh* Mesh)
{
    int32 Count = 0;
    Mesh->ProcessMesh([&Count](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        Count = ReadMesh.VertexCount();
    });
    return Count;
}

// ORDER-INDEPENDENT position digest. Deliberately not a per-index comparison: several of these
// ops renumber vertices, and an indexed comparison would report a difference that is only a
// renumbering. Summing a coordinate-mixed term per vertex is invariant to order and still moves
// when any vertex moves.
double ModelingOptionsTest_PositionDigest(UDynamicMesh* Mesh)
{
    double Digest = 0.0;
    Mesh->ProcessMesh([&Digest](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        for (int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            const FVector3d P = ReadMesh.GetVertex(VertexID);
            Digest += P.X * 1.0 + P.Y * 3.0 + P.Z * 7.0 + P.SquaredLength();
        }
    });
    return Digest;
}

// THE DIGEST FOR A ROTATION ABOUT Z, and the one thing ModelingOptionsTest_PositionDigest above
// structurally cannot see.
//
// twist is a pure rotation about the warp frame's Z (TwistMeshOp.cpp: it rotates X and Y by
// Theta(z) and leaves Z alone). Every term of the position digest survives that untouched:
// P.SquaredLength() is rotation-invariant, P.Z is unchanged, and the column fixture is 4-fold
// symmetric about Z, so each of its rings contributes Sum(X) = Sum(Y) = 0 whatever angle the ring
// was turned through. The digest is therefore EXACTLY the undeformed mesh's value for every
// possible twist - which is what it reported: 177142.857143 under three different parameter sets,
// the same number an untwisted column gives.
//
// Absolute values break that invariance, because Sum|X| over a rotated ring of four corners is
// 2r(|cos a| + |sin a|) and does move with the angle. The |X*Z| term weights the outermost rings,
// which is where the extent and bidirectional settings differ most.
double ModelingOptionsTest_SpinDigest(UDynamicMesh* Mesh)
{
    double Digest = 0.0;
    Mesh->ProcessMesh([&Digest](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        for (int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            const FVector3d P = ReadMesh.GetVertex(VertexID);
            Digest += FMath::Abs(P.X) * 1.0 + FMath::Abs(P.Y) * 3.0 + FMath::Abs(P.X * P.Z) * 7.0;
        }
    });
    return Digest;
}

// The same idea over the primary normal overlay, for the ops that move no vertex at all.
//
// ABSOLUTE VALUES, and that is not cosmetic. A signed component sum is ZERO for any mesh whose
// normal set is closed under negation - which is every fixture in this file: a box's six face
// normals cancel exactly, and so does a sphere's. The signed form reported 0.000000 against
// 0.000000 for two genuinely different normal sets and could never have reported anything else.
// The absolute form is still order-independent (the only property the digest actually needs) and
// is bounded below by the element count, so "zero" now means "no overlay" and nothing else.
double ModelingOptionsTest_NormalDigest(UDynamicMesh* Mesh)
{
    double Digest = 0.0;
    Mesh->ProcessMesh([&Digest](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        if (!ReadMesh.HasAttributes() || ReadMesh.Attributes()->PrimaryNormals() == nullptr)
        {
            return;
        }
        const UE::Geometry::FDynamicMeshNormalOverlay* Normals = ReadMesh.Attributes()->PrimaryNormals();
        for (int32 ElementID : Normals->ElementIndicesItr())
        {
            const FVector3f N = Normals->GetElement(ElementID);
            Digest += FMath::Abs(N.X) * 1.0 + FMath::Abs(N.Y) * 3.0 + FMath::Abs(N.Z) * 7.0;
        }
    });
    return Digest;
}

int32 ModelingOptionsTest_NormalElementCount(UDynamicMesh* Mesh)
{
    int32 Count = 0;
    Mesh->ProcessMesh([&Count](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        if (!ReadMesh.HasAttributes() || ReadMesh.Attributes()->PrimaryNormals() == nullptr)
        {
            return;
        }
        Count = ReadMesh.Attributes()->PrimaryNormals()->ElementCount();
    });
    return Count;
}

// Absolute values for the same reason as the normal digest above - an axis-aligned tangent frame
// set cancels to exactly zero under a signed sum.
double ModelingOptionsTest_TangentDigest(UDynamicMesh* Mesh)
{
    double Digest = 0.0;
    Mesh->ProcessMesh([&Digest](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        if (!ReadMesh.HasAttributes() || !ReadMesh.Attributes()->HasTangentSpace())
        {
            return;
        }
        const UE::Geometry::FDynamicMeshNormalOverlay* Tangents = ReadMesh.Attributes()->PrimaryTangents();
        for (int32 ElementID : Tangents->ElementIndicesItr())
        {
            const FVector3f T = Tangents->GetElement(ElementID);
            Digest += FMath::Abs(T.X) * 1.0 + FMath::Abs(T.Y) * 3.0 + FMath::Abs(T.Z) * 7.0;
        }
    });
    return Digest;
}

// "Did the op write a tangent layer at all" is a COUNT question and has to be asked as one: a
// digest can be zero for two unrelated reasons - no overlay, or an overlay that cancels - and
// asking it with a digest is how "recompute_tangents produced a tangent layer at all" came to
// fail on a mesh that had one.
int32 ModelingOptionsTest_TangentElementCount(UDynamicMesh* Mesh)
{
    int32 Count = 0;
    Mesh->ProcessMesh([&Count](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        if (!ReadMesh.HasAttributes() || !ReadMesh.Attributes()->HasTangentSpace())
        {
            return;
        }
        Count = ReadMesh.Attributes()->PrimaryTangents()->ElementCount();
    });
    return Count;
}

FBox ModelingOptionsTest_Bounds(UDynamicMesh* Mesh)
{
    FBox Bounds(ForceInit);
    Mesh->ProcessMesh([&Bounds](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        for (int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            Bounds += (FVector)ReadMesh.GetVertex(VertexID);
        }
    });
    return Bounds;
}

// Scalar comparison against an engine default that is often a float where the op's field is a
// double. An exact == is wrong for exactly that reason: 0.1f widens to 0.10000000149011612 and
// would never equal the literal 0.1 the op carries. A RELATIVE tolerance is what makes the
// comparison mean "the same number, spelled in two widths" without letting a genuinely different
// default (1e-06 against 0.0001) slip through - those differ by two orders of magnitude and fail
// this at any relative tolerance.
void ModelingOptionsTest_ExpectSameScalar(
    FAutomationTestBase& Test, const TCHAR* What, double OpDefault, double EngineDefault)
{
    const double Tolerance = FMath::Max(1e-9, FMath::Abs(EngineDefault) * 1e-6);
    Test.TestTrue(
        *FString::Printf(TEXT("%s: op default %.10g, engine default %.10g"),
            What, OpDefault, EngineDefault),
        FMath::IsNearlyEqual(OpDefault, EngineDefault, Tolerance));
}

template <typename OpEnumType, typename EngineEnumType>
void ModelingOptionsTest_ExpectSameEnum(
    FAutomationTestBase& Test, const TCHAR* What, OpEnumType OpDefault, EngineEnumType EngineDefault)
{
    Test.TestTrue(
        *FString::Printf(TEXT("%s: op default ordinal %d, engine default ordinal %d"),
            What, (int32)OpDefault, (int32)EngineDefault),
        (int32)OpDefault == (int32)EngineDefault);
}
}

// ============================================================================
// 1. The default path
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsDefaultsMatchEngineTest,
    "PinWright.Geometry.Ops.ModelingOptions.DefaultsMatchTheEngineStructDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsDefaultsMatchEngineTest::RunTest(const FString& Parameters)
{
    // The three face ops share FFaceOpCommonSpec, and all three engine structs spell those three
    // fields identically - so one comparison against any of them covers the shared half.
    {
        const GeometryOps::FFaceOpCommonSpec Common;
        const FGeometryScriptMeshOffsetFacesOptions Engine;
        ModelingOptionsTest_ExpectSameEnum(*this, TEXT("FFaceOpCommonSpec::AreaMode"),
            Common.AreaMode, Engine.AreaMode);
        ModelingOptionsTest_ExpectSameEnum(*this, TEXT("FPolygroupEditSpec::GroupMode"),
            Common.Groups.GroupMode, Engine.GroupOptions.GroupMode);
        TestEqual(TEXT("FPolygroupEditSpec::ConstantGroup"),
            Common.Groups.ConstantGroup, Engine.GroupOptions.ConstantGroup);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("FFaceOpCommonSpec::UVScale"),
            Common.UVScale, (double)Engine.UVScale);
    }

    // inset / outset - FGeometryScriptMeshInsetOutsetFacesOptions ---------------
    {
        const GeometryOps::FInsetParams Inset;
        const GeometryOps::FOutsetParams Outset;
        const FGeometryScriptMeshInsetOutsetFacesOptions Engine;

        // The one field this pass was told to leave forced. It is forced to the ENGINE value, so
        // "forced" and "matches the engine" are the same assertion here - and this line is what
        // fails if anyone re-pins it to false.
        TestEqual(TEXT("inset bReproject is the engine default"), Inset.bReproject, Engine.bReproject);

        TestEqual(TEXT("inset bBoundaryOnly"), Inset.bBoundaryOnly, Engine.bBoundaryOnly);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("inset Softness"),
            Inset.Softness, (double)Engine.Softness);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("inset AreaScale"),
            Inset.AreaScale, (double)Engine.AreaScale);

        TestEqual(TEXT("outset bBoundaryOnly"), Outset.bBoundaryOnly, Engine.bBoundaryOnly);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("outset Softness"),
            Outset.Softness, (double)Engine.Softness);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("outset AreaScale"),
            Outset.AreaScale, (double)Engine.AreaScale);
    }

    // offset_faces / poke - FGeometryScriptMeshOffsetFacesOptions --------------
    {
        const GeometryOps::FOffsetFacesParams Offset;
        const GeometryOps::FPokeParams Poke;
        const FGeometryScriptMeshOffsetFacesOptions Engine;
        const FGeometryScriptPNTessellateOptions EngineTessellate;

        ModelingOptionsTest_ExpectSameEnum(*this, TEXT("offset_faces OffsetType"),
            Offset.OffsetType, Engine.OffsetType);
        TestEqual(TEXT("offset_faces bSolidsToShells"), Offset.bSolidsToShells, Engine.bSolidsToShells);

        ModelingOptionsTest_ExpectSameEnum(*this, TEXT("poke OffsetType"),
            Poke.OffsetType, Engine.OffsetType);
        TestEqual(TEXT("poke bSolidsToShells"), Poke.bSolidsToShells, Engine.bSolidsToShells);
        TestEqual(TEXT("poke bRecomputeNormals"), Poke.bRecomputeNormals, EngineTessellate.bRecomputeNormals);
    }

    // subdivide - FGeometryScriptPNTessellateOptions ---------------------------
    {
        const GeometryOps::FSubdivideParams Subdivide;
        const FGeometryScriptPNTessellateOptions Engine;
        TestEqual(TEXT("subdivide bRecomputeNormals"), Subdivide.bRecomputeNormals, Engine.bRecomputeNormals);
    }

    // extrude - FGeometryScriptMeshLinearExtrudeOptions -------------------------
    {
        const GeometryOps::FExtrudeParams Extrude;
        const FGeometryScriptMeshLinearExtrudeOptions Engine;

        // The second deliberately-forced field, and like inset's it is forced to the ENGINE
        // value: FixedDirection is both what the op hardcoded and what the struct defaults to, so
        // publishing DirectionMode was a pure widening.
        ModelingOptionsTest_ExpectSameEnum(*this, TEXT("extrude DirectionMode is the engine default"),
            Extrude.DirectionMode, Engine.DirectionMode);

        TestEqual(TEXT("extrude Direction"), Extrude.Direction, Engine.Direction);
        TestEqual(TEXT("extrude bSolidsToShells"), Extrude.bSolidsToShells, Engine.bSolidsToShells);
    }

    // bevel - FGeometryScriptMeshBevelOptions ----------------------------------
    {
        const GeometryOps::FBevelParams Bevel;
        const FGeometryScriptMeshBevelOptions Engine;
        // Subdivisions / RoundWeight exist on the engine struct only from UE 5.4. On 5.3 the op
        // rejects a non-zero subdivision count outright (GeometryOps_Modeling.cpp), so there is no
        // engine default to compare the op's default against.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        TestEqual(TEXT("bevel Subdivisions"), Bevel.Subdivisions, Engine.Subdivisions);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("bevel RoundWeight"),
            Bevel.RoundWeight, (double)Engine.RoundWeight);
#endif
        TestEqual(TEXT("bevel bInferMaterialID"), Bevel.bInferMaterialID, Engine.bInferMaterialID);
        TestEqual(TEXT("bevel SetMaterialID"), Bevel.SetMaterialID, Engine.SetMaterialID);
        TestEqual(TEXT("bevel bApplyFilterBox"), Bevel.bApplyFilterBox, Engine.bApplyFilterBox);
        TestEqual(TEXT("bevel bFullyContained"), Bevel.bFullyContained, Engine.bFullyContained);
        // The engine carries one FBox where the op carries two corners; the engine's default is
        // ForceInit, whose Min and Max are both zero - which is what the op's two corners are.
        TestEqual(TEXT("bevel FilterBoxMin"), Bevel.FilterBoxMin, Engine.FilterBox.Min);
        TestEqual(TEXT("bevel FilterBoxMax"), Bevel.FilterBoxMax, Engine.FilterBox.Max);
    }

    // shell - FGeometryScriptMeshOffsetOptions ---------------------------------
    {
        const GeometryOps::FShellParams Shell;
        const FGeometryScriptMeshOffsetOptions Engine;
        TestEqual(TEXT("shell bFixedBoundary"), Shell.bFixedBoundary, Engine.bFixedBoundary);
        TestEqual(TEXT("shell SolveSteps"), Shell.SolveSteps, Engine.SolveSteps);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("shell SmoothAlpha"),
            Shell.SmoothAlpha, (double)Engine.SmoothAlpha);
        TestEqual(TEXT("shell bReprojectDuringSmoothing"),
            Shell.bReprojectDuringSmoothing, Engine.bReprojectDuringSmoothing);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("shell BoundaryAlpha"),
            Shell.BoundaryAlpha, (double)Engine.BoundaryAlpha);
    }

    // bend / twist / taper - the three warp option structs ---------------------
    {
        const GeometryOps::FBendParams Bend;
        const GeometryOps::FTwistParams Twist;
        const GeometryOps::FTaperParams Taper;
        const FGeometryScriptBendWarpOptions EngineBend;
        const FGeometryScriptTwistWarpOptions EngineTwist;
        const FGeometryScriptFlareWarpOptions EngineFlare;

        TestEqual(TEXT("bend bSymmetricExtents"),
            Bend.Extents.bSymmetricExtents, EngineBend.bSymmetricExtents);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("bend LowerExtent"),
            Bend.Extents.LowerExtent, (double)EngineBend.LowerExtent);
        TestEqual(TEXT("bend bBidirectional"), Bend.bBidirectional, EngineBend.bBidirectional);

        TestEqual(TEXT("twist bSymmetricExtents"),
            Twist.Extents.bSymmetricExtents, EngineTwist.bSymmetricExtents);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("twist LowerExtent"),
            Twist.Extents.LowerExtent, (double)EngineTwist.LowerExtent);
        TestEqual(TEXT("twist bBidirectional"), Twist.bBidirectional, EngineTwist.bBidirectional);

        TestEqual(TEXT("taper bSymmetricExtents"),
            Taper.Extents.bSymmetricExtents, EngineFlare.bSymmetricExtents);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("taper LowerExtent"),
            Taper.Extents.LowerExtent, (double)EngineFlare.LowerExtent);
        ModelingOptionsTest_ExpectSameEnum(*this, TEXT("taper FlareType"),
            Taper.FlareType, EngineFlare.FlareType);
    }

    // noise_deform - FGeometryScriptPerlinNoiseOptions -------------------------
    {
        const GeometryOps::FNoiseDeformParams Noise;
        const FGeometryScriptPerlinNoiseOptions Engine;
        TestEqual(TEXT("noise_deform bApplyAlongNormal"), Noise.bApplyAlongNormal, Engine.bApplyAlongNormal);
        // FGeometryScriptPerlinNoiseOptions::NormalSource arrived in UE 5.8. Older engines behave
        // as Computed unconditionally, so there is no engine field to compare against - the local
        // default is asserted directly against the behaviour those engines actually have.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        ModelingOptionsTest_ExpectSameEnum(*this, TEXT("noise_deform NormalSource"),
            Noise.NormalSource, Engine.NormalSource);
#else
        TestTrue(TEXT("noise_deform NormalSource defaults to Computed, which is what this engine does"),
            Noise.NormalSource == GeometryOps::ENoiseNormalSource::Computed);
#endif
    }

    // split_normals / recompute_tangents - MeshNormalsFunctions.h --------------
    {
        const GeometryOps::FSplitNormalsParams Split;
        const FGeometryScriptSplitNormalsOptions EngineSplit;
        TestEqual(TEXT("split_normals bSplitByOpeningAngle"),
            Split.bSplitByOpeningAngle, EngineSplit.bSplitByOpeningAngle);
        TestEqual(TEXT("split_normals bSplitByFaceGroup"),
            Split.bSplitByFaceGroup, EngineSplit.bSplitByFaceGroup);
        TestEqual(TEXT("split_normals bUseDefaultGroupLayer"),
            Split.bUseDefaultGroupLayer, EngineSplit.GroupLayer.bDefaultLayer);
        TestEqual(TEXT("split_normals ExtendedGroupLayerIndex"),
            Split.ExtendedGroupLayerIndex, EngineSplit.GroupLayer.ExtendedLayerIndex);

        const GeometryOps::FRecomputeTangentsParams Tangents;
        const FGeometryScriptTangentsOptions EngineTangents;
        ModelingOptionsTest_ExpectSameEnum(*this, TEXT("recompute_tangents Type"),
            Tangents.Type, EngineTangents.Type);
        TestEqual(TEXT("recompute_tangents UVLayer"), Tangents.UVLayer, EngineTangents.UVLayer);
    }

    // fill_holes / remove_degenerates / weld_vertices - MeshRepairFunctions.h --
    {
        const GeometryOps::FFillHolesParams Fill;
        const FGeometryScriptFillHolesOptions EngineFill;
        ModelingOptionsTest_ExpectSameEnum(*this, TEXT("fill_holes FillMethod"),
            Fill.FillMethod, EngineFill.FillMethod);
        TestEqual(TEXT("fill_holes bDeleteIsolatedTriangles"),
            Fill.bDeleteIsolatedTriangles, EngineFill.bDeleteIsolatedTriangles);

        const GeometryOps::FRemoveDegeneratesParams Degenerates;
        const FGeometryScriptDegenerateTriangleOptions EngineDegenerates;
        ModelingOptionsTest_ExpectSameEnum(*this, TEXT("remove_degenerates Mode"),
            Degenerates.Mode, EngineDegenerates.Mode);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("remove_degenerates MinTriangleArea"),
            Degenerates.MinTriangleArea, EngineDegenerates.MinTriangleArea);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("remove_degenerates MinEdgeLength"),
            Degenerates.MinEdgeLength, EngineDegenerates.MinEdgeLength);
        TestEqual(TEXT("remove_degenerates bCompactOnCompletion"),
            Degenerates.bCompactOnCompletion, EngineDegenerates.bCompactOnCompletion);

        const GeometryOps::FWeldVerticesParams Weld;
        const FGeometryScriptWeldEdgesOptions EngineWeld;
        TestEqual(TEXT("weld_vertices bOnlyUniquePairs"),
            Weld.bOnlyUniquePairs, EngineWeld.bOnlyUniquePairs);
        // Weld.Tolerance is deliberately NOT compared here - see the overrides test.
    }

    // self_union - FGeometryScriptMeshSelfUnionOptions --------------------------
    {
        const GeometryOps::FSelfUnionParams Self;
        const FGeometryScriptMeshSelfUnionOptions Engine;
        TestEqual(TEXT("self_union bFillHoles"), Self.bFillHoles, Engine.bFillHoles);
        TestEqual(TEXT("self_union bTrimFlaps"), Self.bTrimFlaps, Engine.bTrimFlaps);
        // NOT the four boolean verbs' bSimplifyOutput=false override: self_union has always run
        // on the engine struct defaults and this widening must reproduce that exactly.
        TestEqual(TEXT("self_union bSimplifyOutput"), Self.bSimplifyOutput, Engine.bSimplifyOutput);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("self_union SimplifyPlanarTolerance"),
            Self.SimplifyPlanarTolerance, (double)Engine.SimplifyPlanarTolerance);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("self_union WindingThreshold"),
            Self.WindingThreshold, (double)Engine.WindingThreshold);
    }

    return true;
}

// ============================================================================
// The deliberate divergences
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsDocumentedOverridesTest,
    "PinWright.Geometry.Ops.ModelingOptions.DocumentedDivergencesFromTheEngineDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsDocumentedOverridesTest::RunTest(const FString& Parameters)
{
    // Each of these is a SHIPPED default that predates the widening. Moving it onto the engine's
    // value would silently change every existing .pwmodel document and every existing RPC call,
    // so each is asserted by literal AND asserted to still differ from the engine - the second
    // half is what turns this from a restatement into a record of a decision.

    // split_normals: 60 degrees, not the engine's 15. Lowering it to the engine value would put
    // a hard edge on every mesh that currently comes out smooth.
    {
        const GeometryOps::FSplitNormalsParams Split;
        const FGeometryScriptSplitNormalsOptions Engine;
        TestEqual(TEXT("split_normals SplitAngle is the shipped 60"), Split.SplitAngle, 60.0);
        TestTrue(TEXT("split_normals SplitAngle deliberately differs from the engine's 15"),
            !FMath::IsNearlyEqual(Split.SplitAngle, (double)Engine.OpeningAngleDeg, 1e-6));
    }

    // weld_vertices: 1e-04, not the engine's 1e-06. The same "weld coincident edges" operation is
    // reachable at three tolerances - this one, the engine's, and mirror's 1e-03 - and this is the
    // one weld_vertices has always used.
    {
        const GeometryOps::FWeldVerticesParams Weld;
        const FGeometryScriptWeldEdgesOptions Engine;
        TestEqual(TEXT("weld_vertices Tolerance is the shipped 0.0001"), Weld.Tolerance, 0.0001);
        TestTrue(TEXT("weld_vertices Tolerance deliberately differs from the engine's 1e-06"),
            !FMath::IsNearlyEqual(Weld.Tolerance, (double)Engine.Tolerance, 1e-9));
    }

    // The distances. Every face op's engine struct defaults its distance to 1.0, which is a
    // sub-millimetre operation on a mesh authored in centimetres and reads as a no-op. The
    // shipped defaults are the visible ones.
    {
        TestEqual(TEXT("inset Distance"), GeometryOps::FInsetParams().Distance, 5.0);
        TestEqual(TEXT("outset Distance"), GeometryOps::FOutsetParams().Distance, 5.0);
        TestEqual(TEXT("offset_faces Distance"), GeometryOps::FOffsetFacesParams().Distance, 5.0);
        TestEqual(TEXT("bevel Distance"), GeometryOps::FBevelParams().Distance, 5.0);
        TestEqual(TEXT("extrude Distance"), GeometryOps::FExtrudeParams().Distance, 10.0);
        TestEqual(TEXT("shell Thickness"), GeometryOps::FShellParams().Thickness, 5.0);
    }

    // relax is FGeometryScriptIterativeMeshSmoothingOptions with its own shipped pair, and
    // smooth is the same struct with the engine's. They must not be reconciled onto each other:
    // relax is documented as the light pass and smooth as the heavy one.
    {
        const GeometryOps::FSmoothParams Smooth;
        const GeometryOps::FRelaxParams Relax;
        const FGeometryScriptIterativeMeshSmoothingOptions Engine;
        TestEqual(TEXT("smooth Iterations is the engine's"), Smooth.Iterations, Engine.NumIterations);
        ModelingOptionsTest_ExpectSameScalar(*this, TEXT("smooth Alpha"),
            Smooth.Alpha, (double)Engine.Alpha);
        TestEqual(TEXT("relax Iterations is the shipped 3"), Relax.Iterations, 3);
        TestEqual(TEXT("relax Strength is the shipped 0.5"), Relax.Strength, 0.5);
    }

    return true;
}

// ============================================================================
// 2. Non-default values reaching the engine
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsExtrudeDirectionModeTest,
    "PinWright.Geometry.Ops.ModelingOptions.ExtrudeDirectionModeReachesTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsExtrudeDirectionModeTest::RunTest(const FString& Parameters)
{
    // The cleanest possible separation of the two modes: select the +Z face only, then extrude it
    // along +X. FixedDirection must move it in X; AverageFaceNormal ignores Direction entirely and
    // must move it in Z (MeshModelingFunctions.cpp computes the area-weighted normal of the
    // selection and only falls back to Direction if that normalizes to zero, which one flat face
    // never does). The two answers are on different AXES, so no tolerance question arises.
    auto RunExtrude = [](GeometryOps::ELinearExtrudeDirection Mode)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewBoxMesh());
        GeometryOps::FExtrudeParams Params;
        Params.Distance = 40.0;
        Params.Direction = FVector(1, 0, 0);
        Params.DirectionMode = Mode;
        Params.Faces.bHasDirection = true;
        Params.Faces.Direction = FVector(0, 0, 1);
        Params.Faces.AngleTolerance = 10.0;
        const GeometryOps::FOpResult Result = GeometryOps::Extrude(Mesh.Get(), Params);
        return TTuple<GeometryOps::FOpResult, FBox>(Result, ModelingOptionsTest_Bounds(Mesh.Get()));
    };

    const TTuple<GeometryOps::FOpResult, FBox> Fixed =
        RunExtrude(GeometryOps::ELinearExtrudeDirection::FixedDirection);
    const TTuple<GeometryOps::FOpResult, FBox> Averaged =
        RunExtrude(GeometryOps::ELinearExtrudeDirection::AverageFaceNormal);

    TestTrue(TEXT("extrude fixed_direction succeeds"), Fixed.Get<0>().bSuccess);
    TestTrue(TEXT("extrude average_face_normal succeeds"), Averaged.Get<0>().bSuccess);

    // The box is centred and 100 across, so it starts at +-50 on every axis.
    TestTrue(
        *FString::Printf(TEXT("fixed_direction extrudes the selected face along X (max X %.2f, expected > 50)"),
            Fixed.Get<1>().Max.X),
        Fixed.Get<1>().Max.X > 60.0);
    TestTrue(
        *FString::Printf(TEXT("average_face_normal ignores `direction` and extrudes along the face normal "
                              "(max X %.2f, expected ~50; max Z %.2f, expected > 50)"),
            Averaged.Get<1>().Max.X, Averaged.Get<1>().Max.Z),
        Averaged.Get<1>().Max.X < 60.0 && Averaged.Get<1>().Max.Z > 60.0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsOffsetFacesTypeTest,
    "PinWright.Geometry.Ops.ModelingOptions.OffsetFacesOffsetTypeReachesTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsOffsetFacesTypeTest::RunTest(const FString& Parameters)
{
    // ParallelFaceOffset (the default) moves every face along its own normal by exactly Distance,
    // so a box grows into a bigger box and its corners land at Distance*sqrt(3) from where they
    // started. VertexNormal moves each VERTEX along its averaged normal by Distance, so the same
    // corners land at Distance. Different corner positions is a structural difference, not a
    // metric one - the two modes cannot agree on a box for any non-zero distance.
    auto RunOffset = [](GeometryOps::EOffsetFacesType Type)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewBoxMesh());
        GeometryOps::FOffsetFacesParams Params;
        Params.Distance = 25.0;
        Params.OffsetType = Type;
        const GeometryOps::FOpResult Result = GeometryOps::OffsetFaces(Mesh.Get(), Params);
        return TTuple<GeometryOps::FOpResult, FBox>(Result, ModelingOptionsTest_Bounds(Mesh.Get()));
    };

    const TTuple<GeometryOps::FOpResult, FBox> Parallel =
        RunOffset(GeometryOps::EOffsetFacesType::ParallelFaceOffset);
    const TTuple<GeometryOps::FOpResult, FBox> VertexNormal =
        RunOffset(GeometryOps::EOffsetFacesType::VertexNormal);

    TestTrue(TEXT("offset_faces parallel_face_offset succeeds"), Parallel.Get<0>().bSuccess);
    TestTrue(TEXT("offset_faces vertex_normal succeeds"), VertexNormal.Get<0>().bSuccess);

    // Corner distance along one axis: parallel offset puts a face at 50 + 25 = 75, vertex-normal
    // puts the corner at 50 + 25/sqrt(3) ~= 64.4. Comparing the bound rather than a digest so the
    // failure message says which shape came out.
    TestTrue(
        *FString::Printf(TEXT("offset_type changes the offset direction (parallel max X %.3f, "
                              "vertex_normal max X %.3f)"),
            Parallel.Get<1>().Max.X, VertexNormal.Get<1>().Max.X),
        !FMath::IsNearlyEqual(Parallel.Get<1>().Max.X, VertexNormal.Get<1>().Max.X, 0.5));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsPokeOptionsTest,
    "PinWright.Geometry.Ops.ModelingOptions.PokeOffsetTypeReachesTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsPokeOptionsTest::RunTest(const FString& Parameters)
{
    // poke is offset_faces followed by one PN tessellation, and it carries BOTH option structs.
    // The offset half is tested the same way offset_faces' is; the tessellation half is tested by
    // subdivide, which is the same FGeometryScriptPNTessellateOptions.
    auto RunPoke = [](GeometryOps::EOffsetFacesType Type)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewBoxMesh());
        GeometryOps::FPokeParams Params;
        Params.Offset = 25.0;
        Params.OffsetType = Type;
        const GeometryOps::FOpResult Result = GeometryOps::Poke(Mesh.Get(), Params);
        return TTuple<GeometryOps::FOpResult, FBox>(Result, ModelingOptionsTest_Bounds(Mesh.Get()));
    };

    const TTuple<GeometryOps::FOpResult, FBox> Parallel =
        RunPoke(GeometryOps::EOffsetFacesType::ParallelFaceOffset);
    const TTuple<GeometryOps::FOpResult, FBox> VertexNormal =
        RunPoke(GeometryOps::EOffsetFacesType::VertexNormal);

    TestTrue(TEXT("poke parallel_face_offset succeeds"), Parallel.Get<0>().bSuccess);
    TestTrue(TEXT("poke vertex_normal succeeds"), VertexNormal.Get<0>().bSuccess);
    TestTrue(
        *FString::Printf(TEXT("poke offset_type reaches the engine (parallel max X %.3f, "
                              "vertex_normal max X %.3f)"),
            Parallel.Get<1>().Max.X, VertexNormal.Get<1>().Max.X),
        !FMath::IsNearlyEqual(Parallel.Get<1>().Max.X, VertexNormal.Get<1>().Max.X, 0.5));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsInsetAreaModeTest,
    "PinWright.Geometry.Ops.ModelingOptions.InsetAreaModeReachesTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsInsetAreaModeTest::RunTest(const FString& Parameters)
{
    // area_mode is the field with the largest structural effect of the six inset gained, and on a
    // closed box it is also the starkest: EntireSelection makes all 12 triangles ONE region, and a
    // closed region has no boundary loop for FInsetMeshRegion to inset, so the mesh comes back
    // untouched. PerPolygroup splits it into the box's six faces, each of which is a quad with a
    // real boundary, and each grows an inset ring. Triangle counts cannot coincide.
    auto RunInset = [](GeometryOps::EPolyOperationArea AreaMode)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewBoxMesh());
        GeometryOps::FInsetParams Params;
        Params.Distance = 10.0;
        Params.Common.AreaMode = AreaMode;
        const GeometryOps::FOpResult Result = GeometryOps::Inset(Mesh.Get(), Params);
        return TTuple<GeometryOps::FOpResult, int32>(Result, ModelingOptionsTest_TriangleCount(Mesh.Get()));
    };

    const TTuple<GeometryOps::FOpResult, int32> Entire =
        RunInset(GeometryOps::EPolyOperationArea::EntireSelection);
    const TTuple<GeometryOps::FOpResult, int32> PerGroup =
        RunInset(GeometryOps::EPolyOperationArea::PerPolygroup);

    TestTrue(TEXT("inset entire_selection succeeds"), Entire.Get<0>().bSuccess);
    TestTrue(TEXT("inset per_polygroup succeeds"), PerGroup.Get<0>().bSuccess);
    TestTrue(
        *FString::Printf(TEXT("inset area_mode reaches the engine (entire_selection left %d triangles, "
                              "per_polygroup left %d)"),
            Entire.Get<1>(), PerGroup.Get<1>()),
        PerGroup.Get<1>() > Entire.Get<1>());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsOutsetAreaScaleTest,
    "PinWright.Geometry.Ops.ModelingOptions.OutsetAreaScaleReachesTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsOutsetAreaScaleTest::RunTest(const FString& Parameters)
{
    // area_scale is FInsetMeshRegion::AreaCorrection, documented there as a "linear attenuation of
    // area correction factor, valid range [0,1], 0 means ignore area correction entirely".
    //
    // THE REGION MUST HAVE INTERIOR VERTICES OR THE FIELD IS UNREACHABLE, which is the whole of
    // what was wrong here. AreaCorrection is read in exactly one place (InsetMeshRegion.cpp:278),
    // inside a block gated on `(bHaveInteriorVerts || Softness > 0) && bSolveRegionInteriors`
    // (line 226). It scales the Laplacian target of a soft deformation solve whose boundary loop
    // is post-fixed, so the only vertices it can move are the region's INTERIOR ones. On a PLAIN
    // box at per_polygroup each region is a single quad - four vertices, every one of them on the
    // boundary - so bHaveInteriorVerts is false, Softness is at its default 0, the block never
    // runs and both settings produce byte-identical geometry. The old fixture was not measuring
    // the parameter weakly; the parameter was not executing at all.
    //
    // per_polygroup and the box are both KEPT, because they are right for the other reason the
    // original comment gives: at entire_selection a closed box has no boundary and outset does
    // nothing. The one change is the 5x5 grid per face, which gives each face region nine
    // interior vertices while leaving the region boundary and the triangles outside it exactly as
    // they were.
    auto RunOutset = [](double AreaScale)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewBoxMesh(5));
        GeometryOps::FOutsetParams Params;
        Params.Distance = 10.0;
        Params.AreaScale = AreaScale;
        Params.Common.AreaMode = GeometryOps::EPolyOperationArea::PerPolygroup;
        const GeometryOps::FOpResult Result = GeometryOps::Outset(Mesh.Get(), Params);
        return TTuple<GeometryOps::FOpResult, double>(
            Result, ModelingOptionsTest_PositionDigest(Mesh.Get()));
    };

    const TTuple<GeometryOps::FOpResult, double> Corrected = RunOutset(1.0);
    const TTuple<GeometryOps::FOpResult, double> Uncorrected = RunOutset(0.0);

    TestTrue(TEXT("outset area_scale=1 succeeds"), Corrected.Get<0>().bSuccess);
    TestTrue(TEXT("outset area_scale=0 succeeds"), Uncorrected.Get<0>().bSuccess);
    TestTrue(TEXT("outset actually moved something at area_scale=1"), Corrected.Get<0>().bChanged);
    TestTrue(
        *FString::Printf(TEXT("outset area_scale reaches the engine (digest %.6f against %.6f)"),
            Corrected.Get<1>(), Uncorrected.Get<1>()),
        !FMath::IsNearlyEqual(Corrected.Get<1>(), Uncorrected.Get<1>(), 1e-4));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsBevelOptionsTest,
    "PinWright.Geometry.Ops.ModelingOptions.BevelSubdivisionsAndFilterBoxReachTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsBevelOptionsTest::RunTest(const FString& Parameters)
{
    auto RunBevel = [](int32 Subdivisions, bool bApplyFilterBox, bool bFullyContained)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewBoxMesh());
        GeometryOps::FBevelParams Params;
        Params.Distance = 5.0;
        Params.Subdivisions = Subdivisions;
        Params.bApplyFilterBox = bApplyFilterBox;
        // The lower half of the box. Its four bottom edges lie entirely inside; the four vertical
        // edges only have one endpoint in it, which is exactly the distinction bFullyContained
        // draws - so the same box gives two different edge sets depending on that flag.
        Params.FilterBoxMin = FVector(-60, -60, -60);
        Params.FilterBoxMax = FVector(60, 60, 0);
        Params.bFullyContained = bFullyContained;
        const GeometryOps::FOpResult Result = GeometryOps::Bevel(Mesh.Get(), Params);
        return TTuple<GeometryOps::FOpResult, int32>(Result, ModelingOptionsTest_TriangleCount(Mesh.Get()));
    };

    const TTuple<GeometryOps::FOpResult, int32> Plain = RunBevel(0, false, true);
    const TTuple<GeometryOps::FOpResult, int32> Subdivided = RunBevel(2, false, true);
    const TTuple<GeometryOps::FOpResult, int32> Filtered = RunBevel(0, true, true);
    const TTuple<GeometryOps::FOpResult, int32> FilteredLoose = RunBevel(0, true, false);

    TestTrue(TEXT("plain bevel succeeds"), Plain.Get<0>().bSuccess);
    TestTrue(TEXT("filtered bevel succeeds"), Filtered.Get<0>().bSuccess);
    TestTrue(TEXT("loosely-filtered bevel succeeds"), FilteredLoose.Get<0>().bSuccess);

    // FGeometryScriptMeshBevelOptions::Subdivisions arrived in UE 5.4. GeometryOps::Bevel rejects
    // a non-zero subdivision count before it touches the mesh on older engines, so 5.3 asserts the
    // refusal rather than a triangle-count increase the engine cannot produce.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    TestTrue(TEXT("subdivided bevel succeeds"), Subdivided.Get<0>().bSuccess);

    // segments adds edge loops ACROSS each bevel face, so it can only add triangles.
    TestTrue(
        *FString::Printf(TEXT("bevel segments reaches the engine (%d triangles at 0, %d at 2)"),
            Plain.Get<1>(), Subdivided.Get<1>()),
        Subdivided.Get<1>() > Plain.Get<1>());
#else
    TestFalse(TEXT("subdivided bevel is refused on an engine without Subdivisions"),
        Subdivided.Get<0>().bSuccess);
    TestEqual(TEXT("with UNSUPPORTED_ENGINE_VERSION"),
        Subdivided.Get<0>().ErrorCode, FString(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION));
#endif

    // The filter box removes edges from the set, so it can only remove triangles.
    TestTrue(
        *FString::Printf(TEXT("bevel filter box reaches the engine (%d triangles unfiltered, %d filtered)"),
            Plain.Get<1>(), Filtered.Get<1>()),
        Filtered.Get<1>() < Plain.Get<1>());

    // fully_contained=false admits every edge with a vertex in the box, which is a superset of
    // the edges fully inside it.
    TestTrue(
        *FString::Printf(TEXT("bevel fully_contained reaches the engine (%d triangles contained, %d loose)"),
            Filtered.Get<1>(), FilteredLoose.Get<1>()),
        FilteredLoose.Get<1>() > Filtered.Get<1>());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsShellFixedBoundaryTest,
    "PinWright.Geometry.Ops.ModelingOptions.ShellFixedBoundaryReachesTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsShellFixedBoundaryTest::RunTest(const FString& Parameters)
{
    // fixed_boundary pins the boundary loops instead of offsetting them, so on an open rectangle
    // the rim stays where it started while the interior moves. Chosen over solve_steps /
    // smooth_alpha deliberately: those three are knobs on an iterative solve that a planar mesh
    // converges out of in one step, so on this fixture they could legitimately agree.
    auto RunShell = [](bool bFixedBoundary)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewRectangleMesh());
        GeometryOps::FShellParams Params;
        Params.Thickness = 10.0;
        Params.bFixedBoundary = bFixedBoundary;
        const GeometryOps::FOpResult Result = GeometryOps::Shell(Mesh.Get(), Params);
        return TTuple<GeometryOps::FOpResult, double>(
            Result, ModelingOptionsTest_PositionDigest(Mesh.Get()));
    };

    const TTuple<GeometryOps::FOpResult, double> Free = RunShell(false);
    const TTuple<GeometryOps::FOpResult, double> Pinned = RunShell(true);

    TestTrue(*FString::Printf(TEXT("shell with a free boundary succeeds (%s)"),
        *Free.Get<0>().ErrorCode), Free.Get<0>().bSuccess);
    TestTrue(*FString::Printf(TEXT("shell with a fixed boundary succeeds (%s)"),
        *Pinned.Get<0>().ErrorCode), Pinned.Get<0>().bSuccess);
    TestTrue(
        *FString::Printf(TEXT("shell fixed_boundary reaches the engine (digest %.6f against %.6f)"),
            Free.Get<1>(), Pinned.Get<1>()),
        !FMath::IsNearlyEqual(Free.Get<1>(), Pinned.Get<1>(), 1e-4));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsWarpExtentsTest,
    "PinWright.Geometry.Ops.ModelingOptions.WarpExtentsAndBidirectionalReachTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsWarpExtentsTest::RunTest(const FString& Parameters)
{
    // bend and twist share FWarpExtentSpec and both publish bidirectional, so they are tested
    // together against the same column: the two fields are the whole of what each op gained.
    auto RunBend = [](bool bSymmetric, double LowerExtent, bool bBidirectional)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewColumnMesh());
        GeometryOps::FBendParams Params;
        Params.Angle = 60.0;
        Params.Extent = 50.0;
        Params.Extents.bSymmetricExtents = bSymmetric;
        Params.Extents.LowerExtent = LowerExtent;
        Params.bBidirectional = bBidirectional;
        const GeometryOps::FOpResult Result = GeometryOps::Bend(Mesh.Get(), Params);
        return TTuple<GeometryOps::FOpResult, double>(
            Result, ModelingOptionsTest_PositionDigest(Mesh.Get()));
    };

    // TWIST IS MEASURED WITH A DIFFERENT DIGEST, and the difference is forced rather than a
    // preference: bend displaces the column off its axis, which the position digest sees, while
    // twist is a pure rotation about Z that the position digest is mathematically blind to. See
    // ModelingOptionsTest_SpinDigest for the derivation - the short version is that the digest's
    // three terms are each rotation-invariant on a 4-fold-symmetric column, so it returned the
    // UNDEFORMED column's value for all three parameter sets and would have done so for any twist
    // whatsoever.
    auto RunTwist = [](bool bSymmetric, double LowerExtent, bool bBidirectional)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewColumnMesh());
        GeometryOps::FTwistParams Params;
        Params.Angle = 60.0;
        Params.Extent = 50.0;
        Params.Extents.bSymmetricExtents = bSymmetric;
        Params.Extents.LowerExtent = LowerExtent;
        Params.bBidirectional = bBidirectional;
        const GeometryOps::FOpResult Result = GeometryOps::Twist(Mesh.Get(), Params);
        return TTuple<GeometryOps::FOpResult, double>(
            Result, ModelingOptionsTest_SpinDigest(Mesh.Get()));
    };

    // The reference run is the default one: symmetric extents, bidirectional.
    const TTuple<GeometryOps::FOpResult, double> BendDefault = RunBend(true, 10.0, true);
    const TTuple<GeometryOps::FOpResult, double> BendAsymmetric = RunBend(false, 5.0, true);
    const TTuple<GeometryOps::FOpResult, double> BendOneSided = RunBend(true, 10.0, false);

    TestTrue(TEXT("bend succeeds at its defaults"), BendDefault.Get<0>().bSuccess);
    TestTrue(TEXT("bend succeeds with asymmetric extents"), BendAsymmetric.Get<0>().bSuccess);
    TestTrue(TEXT("bend succeeds one-sided"), BendOneSided.Get<0>().bSuccess);

    // symmetric_extents=false narrows the warped band from [-50,+50] to [-5,+50], so every vertex
    // below -5 is transformed differently.
    TestTrue(
        *FString::Printf(TEXT("bend symmetric_extents/lower_extent reach the engine (%.6f against %.6f)"),
            BendDefault.Get<1>(), BendAsymmetric.Get<1>()),
        !FMath::IsNearlyEqual(BendDefault.Get<1>(), BendAsymmetric.Get<1>(), 1e-4));

    TestTrue(
        *FString::Printf(TEXT("bend bidirectional reaches the engine (%.6f against %.6f)"),
            BendDefault.Get<1>(), BendOneSided.Get<1>()),
        !FMath::IsNearlyEqual(BendDefault.Get<1>(), BendOneSided.Get<1>(), 1e-4));

    const TTuple<GeometryOps::FOpResult, double> TwistDefault = RunTwist(true, 10.0, true);
    const TTuple<GeometryOps::FOpResult, double> TwistAsymmetric = RunTwist(false, 5.0, true);
    const TTuple<GeometryOps::FOpResult, double> TwistOneSided = RunTwist(true, 10.0, false);

    TestTrue(TEXT("twist succeeds at its defaults"), TwistDefault.Get<0>().bSuccess);
    TestTrue(TEXT("twist succeeds with asymmetric extents"), TwistAsymmetric.Get<0>().bSuccess);
    TestTrue(TEXT("twist succeeds one-sided"), TwistOneSided.Get<0>().bSuccess);

    // The baseline the three runs are all measured against, and the assertion that separates
    // "the parameters do not reach the engine" from "twist does not reach the engine at all".
    // The two failure modes look identical from a three-way comparison and need different fixes.
    double Untwisted = 0.0;
    {
        TStrongObjectPtr<UDynamicMesh> Reference(ModelingOptionsTest_NewColumnMesh());
        Untwisted = ModelingOptionsTest_SpinDigest(Reference.Get());
    }
    TestTrue(
        *FString::Printf(TEXT("twist deforms the column at all (%.6f against the untwisted %.6f)"),
            TwistDefault.Get<1>(), Untwisted),
        !FMath::IsNearlyEqual(TwistDefault.Get<1>(), Untwisted, 1e-4));

    TestTrue(
        *FString::Printf(TEXT("twist symmetric_extents/lower_extent reach the engine (%.6f against %.6f)"),
            TwistDefault.Get<1>(), TwistAsymmetric.Get<1>()),
        !FMath::IsNearlyEqual(TwistDefault.Get<1>(), TwistAsymmetric.Get<1>(), 1e-4));

    TestTrue(
        *FString::Printf(TEXT("twist bidirectional reaches the engine (%.6f against %.6f)"),
            TwistDefault.Get<1>(), TwistOneSided.Get<1>()),
        !FMath::IsNearlyEqual(TwistDefault.Get<1>(), TwistOneSided.Get<1>(), 1e-4));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsTaperFlareTypeTest,
    "PinWright.Geometry.Ops.ModelingOptions.TaperFlareTypeAndExtentsReachTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsTaperFlareTypeTest::RunTest(const FString& Parameters)
{
    // flare_type selects the displacement PROFILE swept over the extent - sin, sin-squared or a
    // piecewise-linear triangle. Three different functions of the same parameter cannot agree
    // anywhere except at the endpoints, and the column has 7 interior rings between them.
    auto RunTaper = [](GeometryOps::EFlareType FlareType, bool bSymmetric)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewColumnMesh());
        GeometryOps::FTaperParams Params;
        Params.FlareX = 80.0;
        Params.FlareY = 80.0;
        Params.Extent = 50.0;
        Params.FlareType = FlareType;
        Params.Extents.bSymmetricExtents = bSymmetric;
        Params.Extents.LowerExtent = 5.0;
        const GeometryOps::FOpResult Result = GeometryOps::Taper(Mesh.Get(), Params);
        return TTuple<GeometryOps::FOpResult, double>(
            Result, ModelingOptionsTest_PositionDigest(Mesh.Get()));
    };

    const TTuple<GeometryOps::FOpResult, double> Sin =
        RunTaper(GeometryOps::EFlareType::SinMode, true);
    const TTuple<GeometryOps::FOpResult, double> SinSquared =
        RunTaper(GeometryOps::EFlareType::SinSquaredMode, true);
    const TTuple<GeometryOps::FOpResult, double> Triangle =
        RunTaper(GeometryOps::EFlareType::TriangleMode, true);
    const TTuple<GeometryOps::FOpResult, double> Asymmetric =
        RunTaper(GeometryOps::EFlareType::SinMode, false);

    TestTrue(TEXT("taper sin_mode succeeds"), Sin.Get<0>().bSuccess);
    TestTrue(TEXT("taper sin_squared_mode succeeds"), SinSquared.Get<0>().bSuccess);
    TestTrue(TEXT("taper triangle_mode succeeds"), Triangle.Get<0>().bSuccess);
    TestTrue(TEXT("taper with asymmetric extents succeeds"), Asymmetric.Get<0>().bSuccess);

    TestTrue(
        *FString::Printf(TEXT("taper flare_type reaches the engine (sin %.6f, sin_squared %.6f, triangle %.6f)"),
            Sin.Get<1>(), SinSquared.Get<1>(), Triangle.Get<1>()),
        !FMath::IsNearlyEqual(Sin.Get<1>(), SinSquared.Get<1>(), 1e-4)
            && !FMath::IsNearlyEqual(Sin.Get<1>(), Triangle.Get<1>(), 1e-4));

    TestTrue(
        *FString::Printf(TEXT("taper symmetric_extents/lower_extent reach the engine (%.6f against %.6f)"),
            Sin.Get<1>(), Asymmetric.Get<1>()),
        !FMath::IsNearlyEqual(Sin.Get<1>(), Asymmetric.Get<1>(), 1e-4));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsSubdivideRecomputeNormalsTest,
    "PinWright.Geometry.Ops.ModelingOptions.SubdivideRecomputeNormalsReachesTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsSubdivideRecomputeNormalsTest::RunTest(const FString& Parameters)
{
    // The one field of FGeometryScriptPNTessellateOptions, and WHERE IT ACTS is the whole test.
    // FPNTriangles::Compute (PNTriangles.cpp) runs a fixed order: control points off the INPUT
    // mesh's normals, tessellate, DisplaceAndSetQuadraticNormals to place every new vertex, and
    // only THEN, guarded by bRecalculateNormals, RecomputeOverlayNormals + CopyToOverlay. Every
    // position is already written by the time the flag is read.
    //
    // So the relationship is TWO-SIDED, and each side is a different assertion:
    //
    //   - WITHIN one call the flag cannot move a vertex. Both runs share an input mesh, hence the
    //     same control points, the same tessellation and the same displacement pass; the flag
    //     rewrites the normal overlay afterwards and touches nothing else.
    //
    //   - ACROSS calls it must. Subdivide() runs ApplyPNTessellation ONCE PER ITERATION
    //     (GeometryOps_Modeling.cpp), so a second iteration computes its control points from the
    //     overlay the first one left behind - and ComputeControlPoint's Weight12 = Edge12 . Normal
    //     feeds those normals straight into the new vertex POSITIONS. Recomputed normals therefore
    //     bend the second iteration's patch away from the kept ones. Measured: 2877562.19 against
    //     2876447.79, a 0.04% shift.
    //
    // "recompute_normals does not move vertices" is true of ONE iteration and false of two, so it
    // has to be asserted on a one-iteration run or not at all. Both sides are run here because
    // each proves the parameter reached the engine through a different half of the mechanism.
    //
    // ON A SPHERE, NOT A BOX, and the box was not merely a weak choice - it made the field a
    // provable no-op. FPNTriangles reads the control-point normals out of the normal OVERLAY
    // (PNTriangles.cpp ComputeControlPoints), and a box is flat-shaded: all three corner normals
    // of a face are that face's normal, so every control point lands on the flat triangle, the
    // "curved" patch is the original plane, and RecomputeOverlayNormals hands back exactly the
    // face normal it started from. Both settings then produce identical overlays and no digest,
    // however constructed, can tell them apart. A smooth-shaded sphere has corner normals that
    // differ from the face normal, so the patch really curves and the recomputed normals really
    // differ from the interpolated ones.
    auto RunSubdivide = [](bool bRecomputeNormals, int32 Iterations)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewSphereMesh());
        GeometryOps::FSubdivideParams Params;
        Params.Iterations = Iterations;
        Params.bRecomputeNormals = bRecomputeNormals;
        const GeometryOps::FOpResult Result = GeometryOps::Subdivide(Mesh.Get(), Params);
        return TTuple<GeometryOps::FOpResult, double, double, int32>(
            Result,
            ModelingOptionsTest_PositionDigest(Mesh.Get()),
            ModelingOptionsTest_NormalDigest(Mesh.Get()),
            ModelingOptionsTest_TriangleCount(Mesh.Get()));
    };

    // Read off the fixture rather than written as a literal, so the count assertions below stay
    // true if the sphere's step counts are ever retuned.
    int32 FixtureTriangles = 0;
    {
        TStrongObjectPtr<UDynamicMesh> Fixture(ModelingOptionsTest_NewSphereMesh());
        FixtureTriangles = ModelingOptionsTest_TriangleCount(Fixture.Get());
    }

    using FSubdivideRun = TTuple<GeometryOps::FOpResult, double, double, int32>;
    const FSubdivideRun OneRecomputed = RunSubdivide(true, 1);
    const FSubdivideRun OneKept = RunSubdivide(false, 1);
    const FSubdivideRun TwoRecomputed = RunSubdivide(true, 2);
    const FSubdivideRun TwoKept = RunSubdivide(false, 2);

    TestTrue(TEXT("one iteration with recompute_normals=true succeeds"),
        OneRecomputed.Get<0>().bSuccess);
    TestTrue(TEXT("one iteration with recompute_normals=false succeeds"),
        OneKept.Get<0>().bSuccess);
    TestTrue(TEXT("two iterations with recompute_normals=true succeed"),
        TwoRecomputed.Get<0>().bSuccess);
    TestTrue(TEXT("two iterations with recompute_normals=false succeed"),
        TwoKept.Get<0>().bSuccess);

    // SUBDIVIDE ACTUALLY RAN, and the flag changed no TOPOLOGY. PN tessellation quadruples the
    // triangle count per iteration whatever the flag says, so these are counts and cannot drift.
    // They are what separates "the parameter reached the engine" from "the op did nothing at
    // all" - an op that did nothing produces identical positions too, which would satisfy the
    // one-iteration equality below for the one reason that proves nothing.
    TestEqual(TEXT("one iteration quadruples the triangle count (recompute_normals=true)"),
        OneRecomputed.Get<3>(), FixtureTriangles * 4);
    TestEqual(TEXT("one iteration quadruples the triangle count (recompute_normals=false)"),
        OneKept.Get<3>(), FixtureTriangles * 4);
    TestEqual(TEXT("two iterations quadruple it twice (recompute_normals=true)"),
        TwoRecomputed.Get<3>(), FixtureTriangles * 16);
    TestEqual(TEXT("two iterations quadruple it twice (recompute_normals=false)"),
        TwoKept.Get<3>(), FixtureTriangles * 16);

    // Non-zero, so that "the digests disagree" below cannot be satisfied by one of them being
    // zero for the uninteresting reason. The normal digest is an absolute-value sum bounded below
    // by the element count, so zero here means the overlay is GONE - which is a different bug
    // from the parameter not reaching the engine and deserves its own sentence.
    TestTrue(TEXT("every run still carries a normal overlay"),
        OneRecomputed.Get<2>() > 0.0 && OneKept.Get<2>() > 0.0
            && TwoRecomputed.Get<2>() > 0.0 && TwoKept.Get<2>() > 0.0);

    // Side one. The engine guarantees this bit-for-bit - same input, same control points, same
    // displacement pass - so the 1e-3 tolerance is a float-noise guard many orders below the
    // digest's own magnitude, not a slackened comparison.
    TestTrue(
        *FString::Printf(TEXT("one iteration of recompute_normals moves no vertex (%.6f against %.6f)"),
            OneRecomputed.Get<1>(), OneKept.Get<1>()),
        FMath::IsNearlyEqual(OneRecomputed.Get<1>(), OneKept.Get<1>(), 1e-3));

    // ... and with the positions pinned identical, ANY normal difference at one iteration is the
    // flag and can be nothing else. This is the assertion the test is named for.
    TestTrue(
        *FString::Printf(TEXT("subdivide recompute_normals reaches the engine (normal digest %.6f "
                              "against %.6f at one iteration)"),
            OneRecomputed.Get<2>(), OneKept.Get<2>()),
        !FMath::IsNearlyEqual(OneRecomputed.Get<2>(), OneKept.Get<2>(), 1e-4));

    // Side two: the overlay the flag wrote is an INPUT to the next iteration's control points, so
    // the vertices one iteration could not move, two must. This is the half that would still fail
    // if the recomputed normals were written somewhere the tessellator never reads back.
    TestTrue(
        *FString::Printf(TEXT("the recomputed normals feed the next iteration's control points "
                              "(%.6f against %.6f at two iterations)"),
            TwoRecomputed.Get<1>(), TwoKept.Get<1>()),
        !FMath::IsNearlyEqual(TwoRecomputed.Get<1>(), TwoKept.Get<1>(), 1e-4));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsSplitNormalsAngleTest,
    "PinWright.Geometry.Ops.ModelingOptions.SplitNormalsOptionsReachTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsSplitNormalsAngleTest::RunTest(const FString& Parameters)
{
    // A box's every edge is a 90-degree dihedral. Below 90 the angle predicate splits all of them
    // and each of the 8 corners ends up with three normal elements; above 90 it splits none and
    // each corner keeps one. Element COUNT, not element values - a count cannot drift.
    auto RunSplit = [](double SplitAngle, bool bByAngle, bool bByGroup)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewBoxMesh());
        GeometryOps::FSplitNormalsParams Params;
        Params.SplitAngle = SplitAngle;
        Params.bSplitByOpeningAngle = bByAngle;
        Params.bSplitByFaceGroup = bByGroup;
        const GeometryOps::FOpResult Result = GeometryOps::SplitNormals(Mesh.Get(), Params);
        return TTuple<GeometryOps::FOpResult, int32>(
            Result, ModelingOptionsTest_NormalElementCount(Mesh.Get()));
    };

    const TTuple<GeometryOps::FOpResult, int32> Hard = RunSplit(60.0, true, false);
    const TTuple<GeometryOps::FOpResult, int32> Smooth = RunSplit(120.0, true, false);
    // Both predicates off: the engine still re-averages every normal, which is the surprising
    // outcome the op warns about. The count must match the fully-smoothed run.
    const TTuple<GeometryOps::FOpResult, int32> NoPredicate = RunSplit(60.0, false, false);

    TestTrue(TEXT("split_normals at 60 degrees succeeds"), Hard.Get<0>().bSuccess);
    TestTrue(TEXT("split_normals at 120 degrees succeeds"), Smooth.Get<0>().bSuccess);
    TestTrue(TEXT("split_normals with no predicate succeeds"), NoPredicate.Get<0>().bSuccess);

    TestTrue(
        *FString::Printf(TEXT("split_normals split_angle reaches the engine (%d elements at 60 degrees, "
                              "%d at 120)"),
            Hard.Get<1>(), Smooth.Get<1>()),
        Hard.Get<1>() > Smooth.Get<1>());

    TestTrue(
        *FString::Printf(TEXT("split_normals split_by_opening_angle reaches the engine (%d elements on, "
                              "%d off)"),
            Hard.Get<1>(), NoPredicate.Get<1>()),
        Hard.Get<1>() > NoPredicate.Get<1>());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsRecomputeTangentsTypeTest,
    "PinWright.Geometry.Ops.ModelingOptions.RecomputeTangentsTypeReachesTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsRecomputeTangentsTypeTest::RunTest(const FString& Parameters)
{
    // recompute_tangents published NOTHING before this pass - both of its engine fields were
    // unreachable. PerTriangle gives every triangle corner its own frame; FastMikkT averages the
    // frames that meet at a vertex (FComputeTangentsOptions::bAveraged, set from the type in
    // MeshNormalsFunctions.cpp).
    //
    // ON A SPHERE, NOT A BOX, because averaging can only differ from not-averaging where the
    // frames being averaged DIFFER. A box face is planar with an affine UV island, so both of its
    // triangles carry the identical frame, and each face is its own UV and normal island so
    // nothing is averaged across faces either: the two settings produce identical tangents there,
    // and the old digest of 0.000000 against 0.000000 was two identical (and, being axis-aligned
    // and sign-symmetric, exactly cancelling) tangent sets. A sphere's neighbouring triangles
    // carry genuinely different frames.
    auto RunTangents = [](GeometryOps::ETangentType Type)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewSphereMesh());
        GeometryOps::FRecomputeTangentsParams Params;
        Params.Type = Type;
        const GeometryOps::FOpResult Result = GeometryOps::RecomputeTangents(Mesh.Get(), Params);
        return TTuple<GeometryOps::FOpResult, double, int32>(
            Result,
            ModelingOptionsTest_TangentDigest(Mesh.Get()),
            ModelingOptionsTest_TangentElementCount(Mesh.Get()));
    };

    const TTuple<GeometryOps::FOpResult, double, int32> Fast =
        RunTangents(GeometryOps::ETangentType::FastMikkT);
    const TTuple<GeometryOps::FOpResult, double, int32> PerTriangle =
        RunTangents(GeometryOps::ETangentType::PerTriangle);

    TestTrue(TEXT("recompute_tangents fast_mikkt succeeds"), Fast.Get<0>().bSuccess);
    TestTrue(TEXT("recompute_tangents per_triangle succeeds"), PerTriangle.Get<0>().bSuccess);
    // A COUNT, not a digest. "Did the op write a tangent layer" and "are the tangents non-zero"
    // are different questions, and asking the first one with a digest is how this assertion came
    // to fail on a mesh whose tangent layer was present and correct.
    TestTrue(*FString::Printf(TEXT("recompute_tangents produced a tangent layer at all (%d elements)"),
            Fast.Get<2>()),
        Fast.Get<2>() > 0 && PerTriangle.Get<2>() > 0);
    TestTrue(
        *FString::Printf(TEXT("recompute_tangents type reaches the engine (digest %.6f against %.6f)"),
            Fast.Get<1>(), PerTriangle.Get<1>()),
        !FMath::IsNearlyEqual(Fast.Get<1>(), PerTriangle.Get<1>(), 1e-4));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsRemoveDegeneratesTest,
    "PinWright.Geometry.Ops.ModelingOptions.RemoveDegeneratesThresholdsReachTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsRemoveDegeneratesTest::RunTest(const FString& Parameters)
{
    // remove_degenerates published NOTHING before this pass - all four of its engine fields were
    // unreachable, which made the op a button with no settings.
    //
    // The thresholds are ABSOLUTE world units, which is what makes them testable without a
    // hand-built degenerate mesh: at the engine defaults nothing on a 100-unit box qualifies and
    // the op is a no-op, while at min_edge_length=120 the box's 100-unit face edges all do. That
    // is also exactly the trap the header documents - a mesh authored in metres has defaults that
    // are effectively zero.
    auto RunRepair = [](double MinEdgeLength, GeometryOps::ERepairMeshMode Mode)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewBoxMesh());
        GeometryOps::FRemoveDegeneratesParams Params;
        Params.MinEdgeLength = MinEdgeLength;
        Params.Mode = Mode;
        const GeometryOps::FOpResult Result = GeometryOps::RemoveDegenerates(Mesh.Get(), Params);
        return TTuple<GeometryOps::FOpResult, int32>(
            Result, ModelingOptionsTest_TriangleCount(Mesh.Get()));
    };

    const TTuple<GeometryOps::FOpResult, int32> Default =
        RunRepair(0.0001, GeometryOps::ERepairMeshMode::RepairOrDelete);
    const TTuple<GeometryOps::FOpResult, int32> Aggressive =
        RunRepair(120.0, GeometryOps::ERepairMeshMode::RepairOrDelete);

    TestTrue(TEXT("remove_degenerates at the engine defaults succeeds"), Default.Get<0>().bSuccess);
    TestTrue(TEXT("remove_degenerates at a large threshold succeeds"), Aggressive.Get<0>().bSuccess);

    TestEqual(TEXT("remove_degenerates at the engine defaults leaves a clean box alone"),
        Default.Get<1>(), 12);
    TestTrue(
        *FString::Printf(TEXT("remove_degenerates min_edge_length reaches the engine (%d triangles at "
                              "the default, %d at 120)"),
            Default.Get<1>(), Aggressive.Get<1>()),
        Aggressive.Get<1>() < Default.Get<1>());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsFillHolesMethodTest,
    "PinWright.Geometry.Ops.ModelingOptions.FillHolesMethodReachesTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsFillHolesMethodTest::RunTest(const FString& Parameters)
{
    // fill_holes also published nothing at all before this pass.
    //
    // The two methods compared are the two whose OUTPUT SHAPE differs by construction rather than
    // by heuristic: TriangleFan introduces a centre vertex and fans to it, polygon triangulation
    // ear-clips the existing boundary and introduces none. A vertex count is therefore a
    // structural assertion, not a metric one.
    auto RunFill = [](GeometryOps::EFillHolesMethod Method)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewHoledBoxMesh());
        GeometryOps::FFillHolesParams Params;
        Params.FillMethod = Method;
        GeometryOps::FFillHolesOutcome Outcome;
        const GeometryOps::FOpResult Result = GeometryOps::FillHoles(Mesh.Get(), Params, &Outcome);
        return TTuple<GeometryOps::FOpResult, int32, int32>(
            Result, ModelingOptionsTest_VertexCount(Mesh.Get()), Outcome.FilledHoles);
    };

    const TTuple<GeometryOps::FOpResult, int32, int32> Fan =
        RunFill(GeometryOps::EFillHolesMethod::TriangleFan);
    const TTuple<GeometryOps::FOpResult, int32, int32> Polygon =
        RunFill(GeometryOps::EFillHolesMethod::PolygonTriangulation);

    TestTrue(TEXT("fill_holes triangle_fan succeeds"), Fan.Get<0>().bSuccess);
    TestTrue(TEXT("fill_holes polygon_triangulation succeeds"), Polygon.Get<0>().bSuccess);
    TestEqual(TEXT("triangle_fan filled the hole"), Fan.Get<2>(), 1);
    TestEqual(TEXT("polygon_triangulation filled the hole"), Polygon.Get<2>(), 1);

    TestTrue(
        *FString::Printf(TEXT("fill_holes method reaches the engine (triangle_fan left %d vertices, "
                              "polygon_triangulation %d)"),
            Fan.Get<1>(), Polygon.Get<1>()),
        Fan.Get<1>() > Polygon.Get<1>());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingOptionsSelfUnionSimplifyTest,
    "PinWright.Geometry.Ops.ModelingOptions.SelfUnionSimplifyOutputReachesTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingOptionsSelfUnionSimplifyTest::RunTest(const FString& Parameters)
{
    // simplify_output collapses the coplanar fans the boolean resolve leaves along every cut, so
    // it can only ever reduce the triangle count - which makes the comparison directional and not
    // a question of degree.
    auto RunSelfUnion = [](bool bSimplifyOutput)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ModelingOptionsTest_NewSelfIntersectingMesh());
        GeometryOps::FSelfUnionParams Params;
        Params.bSimplifyOutput = bSimplifyOutput;
        const GeometryOps::FOpResult Result = GeometryOps::SelfUnion(Mesh.Get(), Params);
        return TTuple<GeometryOps::FOpResult, int32>(
            Result, ModelingOptionsTest_TriangleCount(Mesh.Get()));
    };

    const TTuple<GeometryOps::FOpResult, int32> Simplified = RunSelfUnion(true);
    const TTuple<GeometryOps::FOpResult, int32> Raw = RunSelfUnion(false);

    TestTrue(TEXT("self_union with simplify_output succeeds"), Simplified.Get<0>().bSuccess);
    TestTrue(TEXT("self_union without simplify_output succeeds"), Raw.Get<0>().bSuccess);
    TestTrue(
        *FString::Printf(TEXT("self_union simplify_output reaches the engine (%d triangles simplified, "
                              "%d raw)"),
            Simplified.Get<1>(), Raw.Get<1>()),
        Raw.Get<1>() > Simplified.Get<1>());

    return true;
}
