// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the extracted modeling / deformer / repair / UV ops (GeometryOps_Modeling.h).
//
// They run against a transient UDynamicMesh with no actor, no editor world and no
// FHandlerContext anywhere - the property the .pwmodel compiler depends on. If a later change
// reintroduces a Ctx read or an actor lookup into one of these ops, this file stops compiling
// rather than failing at runtime.
//
// What each test guards, in the order a regression would appear:
//
//  1. Guards that moved out of the handler. subdivide's iteration clamp and its 4x-per-
//     iteration triangle estimate used to sit in MeshOpsHandler.cpp; only the wrapper ran them,
//     so a compiler front-end calling the op directly would have had no ceiling at all.
//  2. Error codes. The ~146 dispatcher tests observe these verbs only through the response
//     JSON, so the split is behaviour-preserving ONLY if each op returns the exact
//     ErrorCodes.h value the handler emitted at that failure point and the wrapper forwards it.
//     The one exception is the null-mesh pair, which no wrapper can reach: it is
//     GeometryOps::NullMeshFailure's MESH_NOT_FOUND / "DynamicMesh not available" for all five
//     families now, where this one used to answer INVALID_ARGUMENT on its own.
//  2b. A failure must not lose what the op already recorded. FOpResult::Fail builds a FRESH
//     struct, so an op that has warned or captured its before-counts has to fail through
//     FOpResult::FailIn - subdivide is where the two collide on a single request, and the
//     clamp-plus-refusal pair below is unsatisfiable if it does not.
//  3. bChanged. It is count-derived, which is right for the face ops (whose `changed` response
//     field has always been that comparison) and WRONG for the normal/tangent/UV ops, which
//     move no counts at all. Reverting the force-changed flag would make flip_normals report
//     that it did nothing.
//  4. The two mandatory warnings. bevel silently does nothing on a mesh with no polygroups, and
//     extrude with no face filter duplicates a closed solid rather than pushing a face out.
//     Both used to be invisible in the RPC response, because no wrapper read FOpResult::Warnings
//     at all; this file was then the only place they were observable, and that was exactly the
//     defect. The wrappers now emit them as a `warnings` response array
//     (Handlers/Geometry/GeometryOpWarnings.h), asserted over the real dispatcher in
//     TestGeometryOpWarningsOverDispatcher.cpp. These op-level assertions stay because they pin
//     the warning TEXT and the conditions that produce it; they cannot pin the wire contract.
#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOps_Modeling.h"
#include "Handlers/Geometry/GeometryUtils.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "GeometryScript/GeometryScriptSelectionTypes.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace
// helper with a common name would collide with a sibling test TU when Unity merges them.

// A box: 12 triangles, 6 polygroups (the primitive default is PerFace), a populated UV0
// overlay and hard per-face normals. No actor, no component, no world.
UDynamicMesh* GeometryOpsModelingTest_NewBoxMesh()
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform::Identity, 100.0f, 100.0f, 100.0f, 0, 0, 0,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

// An OPEN, fully UV'd mesh: one planar rectangle in XY. The open half is what makes shell
// stitch a boundary loop at all (a closed mesh never constructs FJoinMeshLoops), and the
// primitive generator populates UV channel 0 on every triangle it emits - so this fixture is
// the one shape the shell crash guard must let through.
UDynamicMesh* GeometryOpsModelingTest_NewRectangleMesh()
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendRectangleXY(
        Mesh, Options, FTransform::Identity, 100.0f, 100.0f, 0, 0, nullptr);
    return Mesh;
}

bool GeometryOpsModelingTest_AnyWarningContains(
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

// Reproduces, in one call, the exact state that took the editor down six times: an ALLOCATED
// element of the primary normal overlay that no triangle claims. AppendElement is the ONLY way
// this arises - every site that invalidates a parent also drops the element's last reference
// (DynamicMeshOverlay.cpp:300-302, :344, :599, :648, :1204) - and it is the same call
// FDynamicMesh3::Copy(FMeshShapeGenerator*) makes once per generator normal before assigning only
// the ones the triangle list names (DynamicMesh3.cpp:174-186). Returns the element ID.
int32 GeometryOpsModelingTest_AppendOrphanNormalElement(UDynamicMesh* Mesh)
{
    int32 ElementID = INDEX_NONE;
    Mesh->EditMesh([&ElementID](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        UE::Geometry::FDynamicMeshNormalOverlay* Normals = EditMesh.Attributes()->PrimaryNormals();
        ElementID = Normals->AppendElement(FVector3f(0.0f, 0.0f, 1.0f));
    });
    return ElementID;
}
}

// ============================================================================
// The two guards that used to live in the wrapper
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingSubdivideGuardsTest,
    "PinWright.Geometry.Ops.Modeling.SubdivideClampsIterationsAndGuardsTriangleBudget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingSubdivideGuardsTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewBoxMesh());

    // Two honest iterations: PN tessellation quadruples the triangle count each time.
    GeometryOps::FSubdivideParams Params;
    Params.Iterations = 2;

    GeometryOps::FSubdivideOutcome Outcome;
    GeometryOps::FOpResult Op = GeometryOps::Subdivide(Mesh.Get(), Params, &Outcome);

    TestTrue(TEXT("subdivide(2) succeeds"), Op.bSuccess);
    TestEqual(TEXT("box starts at 12 triangles"), Op.TrianglesBefore, 12);
    TestEqual(TEXT("two PN tessellations quadruple twice"), Op.TrianglesAfter, 192);
    TestTrue(TEXT("subdivide reports a change"), Op.bChanged);
    TestEqual(TEXT("no clamp, so the effective count is the requested one"),
        Outcome.EffectiveIterations, 2);
    TestEqual(TEXT("an unclamped subdivide warns about nothing"), Op.Warnings.Num(), 0);

    // 99 iterations clamp to GEOM_MAX_SUBDIVIDE_ITERATIONS, and 192 * 4^6 is over the
    // half-million-triangle ceiling, so the op must refuse BEFORE touching the mesh.
    Params.Iterations = 99;
    Op = GeometryOps::Subdivide(Mesh.Get(), Params, &Outcome);

    TestEqual(TEXT("iterations clamp to the module ceiling"),
        Outcome.EffectiveIterations, GEOM_MAX_SUBDIVIDE_ITERATIONS);
    // These two assertions on the SAME result are the point: the clamp and the budget refusal
    // fire together on this one request, so a failure path that returns a fresh FOpResult
    // (FOpResult::Fail) makes the pair unsatisfiable - it deletes the warning recorded moments
    // earlier and the clamp becomes unobservable exactly when it happened. Subdivide's refusal
    // must therefore be FOpResult::FailIn.
    TestTrue(TEXT("the clamp is reported rather than applied silently"),
        GeometryOpsModelingTest_AnyWarningContains(Op, TEXT("clamped")));
    TestFalse(TEXT("subdivide refuses a request that would blow the triangle budget"), Op.bSuccess);
    TestEqual(TEXT("and refuses it with the handler's own code"),
        Op.ErrorCode, FString(ErrorCodes::ERR_POLYGON_LIMIT_EXCEEDED));
    // The before-counts BeginOp captured survive the failure for the same reason.
    TestEqual(TEXT("a refused subdivide still reports what it measured going in"),
        Op.TrianglesBefore, 192);
    TestEqual(TEXT("a refused subdivide leaves the mesh untouched"),
        Mesh->GetTriangleCount(), 192);

    return true;
}

// ============================================================================
// bChanged
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingAttributeOpsChangedTest,
    "PinWright.Geometry.Ops.Modeling.AttributeOpsReportChangedWithoutMovingCounts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingAttributeOpsChangedTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewBoxMesh());

    // Flipping normals moves no vertex and no triangle, so a count-derived bChanged would
    // report this real edit as a no-op.
    const GeometryOps::FOpResult Flip =
        GeometryOps::FlipNormals(Mesh.Get(), GeometryOps::FFlipNormalsParams());
    TestTrue(TEXT("flip_normals succeeds"), Flip.bSuccess);
    TestEqual(TEXT("flip_normals moves no triangles"), Flip.TrianglesAfter, Flip.TrianglesBefore);
    TestEqual(TEXT("flip_normals moves no vertices"), Flip.VerticesAfter, Flip.VerticesBefore);
    TestTrue(TEXT("flip_normals still reports a change"), Flip.bChanged);

    const GeometryOps::FOpResult Recalc =
        GeometryOps::RecalculateNormals(Mesh.Get(), GeometryOps::FRecalculateNormalsParams());
    TestTrue(TEXT("recalculate_normals succeeds"), Recalc.bSuccess);
    TestTrue(TEXT("recalculate_normals reports a change"), Recalc.bChanged);

    const GeometryOps::FOpResult Tangents =
        GeometryOps::RecomputeTangents(Mesh.Get(), GeometryOps::FRecomputeTangentsParams());
    TestTrue(TEXT("recompute_tangents succeeds"), Tangents.bSuccess);
    TestTrue(TEXT("recompute_tangents reports a change"), Tangents.bChanged);

    GeometryOps::FSplitNormalsParams SplitParams;
    const GeometryOps::FOpResult Split = GeometryOps::SplitNormals(Mesh.Get(), SplitParams);
    TestTrue(TEXT("split_normals succeeds"), Split.bSuccess);
    TestTrue(TEXT("split_normals reports a change"), Split.bChanged);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingTransformUVsChangedTest,
    "PinWright.Geometry.Ops.Modeling.TransformUVsReportsChangeOnlyWhenItRanSomething",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingTransformUVsChangedTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewBoxMesh());

    // Every one of the three sub-transforms is skipped at its identity, so the default params
    // touch nothing at all - and must say so rather than claim an edit.
    const GeometryOps::FOpResult Identity =
        GeometryOps::TransformUVs(Mesh.Get(), GeometryOps::FTransformUVsParams());
    TestTrue(TEXT("an identity transform_uvs succeeds"), Identity.bSuccess);
    TestFalse(TEXT("an identity transform_uvs reports no change"), Identity.bChanged);

    GeometryOps::FTransformUVsParams Rotated;
    Rotated.Rotation = 45.0;
    const GeometryOps::FOpResult Op = GeometryOps::TransformUVs(Mesh.Get(), Rotated);
    TestTrue(TEXT("a rotating transform_uvs succeeds"), Op.bSuccess);
    TestTrue(TEXT("a rotating transform_uvs reports a change"), Op.bChanged);

    return true;
}

// ============================================================================
// The two mandatory warnings
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingBevelPolygroupWarningTest,
    "PinWright.Geometry.Ops.Modeling.BevelWarnsWhenTheMeshHasNoPolygroups",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingBevelPolygroupWarningTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewBoxMesh());

    GeometryOps::FBevelParams Params;
    Params.Distance = 5.0;

    // With the primitive's PerFace polygroups intact there are group edges to bevel.
    const GeometryOps::FOpResult Grouped = GeometryOps::Bevel(Mesh.Get(), Params);
    TestTrue(TEXT("bevel succeeds on a polygrouped box"), Grouped.bSuccess);
    TestFalse(TEXT("a polygrouped box gets no missing-polygroup warning"),
        GeometryOpsModelingTest_AnyWarningContains(Grouped, TEXT("polygroup")));
    TestTrue(TEXT("bevel actually beveled something"), Grouped.bChanged);

    // Strip the groups and the same call becomes a silent no-op - which is the single most
    // confusing outcome on this verb, so the op must say so.
    TStrongObjectPtr<UDynamicMesh> Ungrouped(GeometryOpsModelingTest_NewBoxMesh());
    Ungrouped->GetMeshRef().DiscardTriangleGroups();

    const GeometryOps::FOpResult Op = GeometryOps::Bevel(Ungrouped.Get(), Params);
    TestTrue(TEXT("bevel still succeeds without polygroups"), Op.bSuccess);
    TestTrue(TEXT("bevel warns that a mesh without polygroups has no edges to bevel"),
        GeometryOpsModelingTest_AnyWarningContains(Op, TEXT("polygroup")));
    TestFalse(TEXT("and reports that nothing changed"), Op.bChanged);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingBevelPerQuadWarningTest,
    "PinWright.Geometry.Ops.Modeling.BevelWarnsWhenEveryQuadIsItsOwnPolygroup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingBevelPerQuadWarningTest::RunTest(const FString& Parameters)
{
    // The OPPOSITE of BevelWarnsWhenTheMeshHasNoPolygroups, and the expensive one. bevel chamfers
    // EVERY polygroup edge, so a mesh whose every quad is its own group gets every interior quad
    // boundary notched - a grid that reads as surface damage rather than as a chamfer, at roughly
    // 3x the triangle cost. One example model spent 30,000 triangles producing an artifact.
    //
    // A TORUS is used because it is not a synthetic case: AppendTorus routes through
    // FBaseRevolveGenerator, which never reads FGeometryScriptPrimitiveOptions::PolygroupMode and
    // defaults PolygonGroupingMode to EProfileSweepPolygonGrouping::PerFace - and for a sweep
    // "PerFace" means one group per QUAD (PolygonId = SweepIndex * NumProfileSegments +
    // ProfileIndex, SweepGenerator.cpp). Nothing on the PinWright side can turn that off, so
    // torus, arch and revolve all arrive in this shape. Regrouping the mesh here instead would
    // test a mesh nobody has.
    TStrongObjectPtr<UDynamicMesh> Torus(NewObject<UDynamicMesh>(GetTransientPackage()));
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendTorus(
        Torus.Get(), Options, FTransform::Identity, FGeometryScriptRevolveOptions(),
        100.0f, 25.0f, 16, 8, EGeometryScriptPrimitiveOriginMode::Center, nullptr);

    // The premise, asserted rather than assumed: if a future engine starts coarsening these
    // groups the warning becomes wrong, and this is the line that says so first.
    const UE::Geometry::FDynamicMesh3& Read = Torus->GetMeshRef();
    TSet<int32> Groups;
    for (const int32 TriangleID : Read.TriangleIndicesItr())
    {
        Groups.Add(Read.GetTriangleGroup(TriangleID));
    }
    TestEqual(TEXT("a 16x8 torus arrives with one polygroup per quad"), Groups.Num(), 128);
    TestEqual(TEXT("over twice as many triangles"), Read.TriangleCount(), 256);

    GeometryOps::FBevelParams Params;
    Params.Distance = 1.0;
    const GeometryOps::FOpResult Op = GeometryOps::Bevel(Torus.Get(), Params);

    TestTrue(TEXT("bevel succeeds on a per-quad-grouped mesh"), Op.bSuccess);
    TestTrue(TEXT("bevel warns that the polygroups are one per quad"),
        GeometryOpsModelingTest_AnyWarningContains(Op, TEXT("group per quad")));
    TestTrue(TEXT("and the warning names the counts it decided on"),
        GeometryOpsModelingTest_AnyWarningContains(Op, TEXT("128 polygroups over 256 triangles")));

    // The cost the warning exists to predict. Asserted as a floor rather than an exact figure:
    // the multiplier is the engine's, and pinning it would fail on an unrelated bevel change.
    TestTrue(*FString::Printf(TEXT("bevelling every quad boundary multiplies the triangle count (%d -> %d)"),
            Op.TrianglesBefore, Op.TrianglesAfter),
        Op.TrianglesAfter > Op.TrianglesBefore * 2);

    // A COARSELY grouped mesh must stay quiet, or the warning is noise on the meshes bevel is
    // for. A default box is 6 groups over 12 triangles - the SAME 1:2 ratio as per-quad grouping -
    // so the ratio alone cannot separate them and the op's absolute floor is what does.
    TStrongObjectPtr<UDynamicMesh> Box(GeometryOpsModelingTest_NewBoxMesh());
    const GeometryOps::FOpResult BoxOp = GeometryOps::Bevel(Box.Get(), Params);
    TestTrue(TEXT("bevel succeeds on a box"), BoxOp.bSuccess);
    TestFalse(TEXT("a 6-group box does not trip the per-quad warning"),
        GeometryOpsModelingTest_AnyWarningContains(BoxOp, TEXT("group per quad")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingExtrudeSelectionTest,
    "PinWright.Geometry.Ops.Modeling.ExtrudeWarnsOnWholeMeshAndHonoursAFaceDirection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingExtrudeSelectionTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> WholeMesh(GeometryOpsModelingTest_NewBoxMesh());

    GeometryOps::FExtrudeParams Params;
    Params.Distance = 10.0;

    GeometryOps::FFaceOpOutcome Outcome;
    GeometryOps::FOpResult Op = GeometryOps::Extrude(WholeMesh.Get(), Params, &Outcome);

    TestTrue(TEXT("extrude with no face filter succeeds"), Op.bSuccess);
    TestEqual(TEXT("an empty selection reports 0 faces, meaning the whole mesh"),
        Outcome.FacesSelected, 0);
    TestTrue(TEXT("extrude warns that it is extruding the whole closed solid"),
        GeometryOpsModelingTest_AnyWarningContains(Op, TEXT("whole mesh")));
    TestTrue(TEXT("extruding every face of a closed box adds geometry"),
        Op.TrianglesAfter > Op.TrianglesBefore);
    TestTrue(TEXT("and reports the change"), Op.bChanged);

    // A face direction narrows the op to a subset - the whole reason the selection exists.
    TStrongObjectPtr<UDynamicMesh> TopOnly(GeometryOpsModelingTest_NewBoxMesh());
    Params.Faces.bHasDirection = true;
    Params.Faces.Direction = FVector(0, 0, 1);
    Params.Faces.AngleTolerance = 45.0;

    Op = GeometryOps::Extrude(TopOnly.Get(), Params, &Outcome);

    TestTrue(TEXT("a directional extrude succeeds"), Op.bSuccess);
    TestTrue(TEXT("a +Z face direction selects at least one face"), Outcome.FacesSelected > 0);
    TestTrue(TEXT("a +Z face direction selects fewer than all 12 of the box's faces"),
        Outcome.FacesSelected < 12);
    TestFalse(TEXT("a filtered extrude does not carry the whole-mesh warning"),
        GeometryOpsModelingTest_AnyWarningContains(Op, TEXT("whole mesh")));

    return true;
}

// ============================================================================
// Error codes forwarded to the wrappers
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingUVChannelRangeTest,
    "PinWright.Geometry.Ops.Modeling.UVOpsRejectAnOutOfRangeChannel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingUVChannelRangeTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewBoxMesh());

    // A dynamic mesh carries at most 8 UV sets, so channel 8 cannot be created. Reporting the
    // resulting engine no-op as success is the defect these codes exist to prevent.
    const GeometryOps::FOpResult Unwrap = GeometryOps::UnwrapUVXAtlas(Mesh.Get(), 8);
    TestFalse(TEXT("XAtlas unwrap refuses uvChannel 8"), Unwrap.bSuccess);
    TestEqual(TEXT("XAtlas unwrap refuses it as INVALID_ARGUMENT"),
        Unwrap.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    TestTrue(TEXT("and the message names the range, as the RPC response has always done"),
        Unwrap.ErrorMessage.Contains(TEXT("out of range")));

    GeometryOps::FProjectUVParams ProjectParams;
    ProjectParams.UVChannel = 8;
    const GeometryOps::FOpResult Project = GeometryOps::ProjectUV(Mesh.Get(), ProjectParams);
    TestFalse(TEXT("project_uv refuses uvChannel 8"), Project.bSuccess);
    TestEqual(TEXT("project_uv refuses it as INVALID_ARGUMENT"),
        Project.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    // Channel 0 is the ordinary path and must still work on the same mesh.
    ProjectParams.UVChannel = 0;
    const GeometryOps::FOpResult Ok = GeometryOps::ProjectUV(Mesh.Get(), ProjectParams);
    TestTrue(TEXT("project_uv still projects into channel 0"), Ok.bSuccess);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingNullMeshTest,
    "PinWright.Geometry.Ops.Modeling.OpsRefuseANullMeshInsteadOfCrashing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingNullMeshTest::RunTest(const FString& Parameters)
{
    // The RPC wrappers resolve a non-null mesh before calling, but the ops are a public entry
    // point now: the .pwmodel compiler reaches them directly, and several of them dereference
    // the mesh (GetMeshRef, GetTriangleCount) before any engine call would null-check it.
    const GeometryOps::FOpResult Flip =
        GeometryOps::FlipNormals(nullptr, GeometryOps::FFlipNormalsParams());
    TestFalse(TEXT("flip_normals refuses a null mesh"), Flip.bSuccess);
    // GeometryOps::NullMeshFailure's pair, now shared by all five families. This family used to
    // answer INVALID_ARGUMENT / "No dynamic mesh to operate on" here while the other four
    // answered this; no RPC wrapper can reach the divergence (every one resolves its mesh
    // through GeometryTarget::ResolveOrSendError or GetOrCreateDynamicMesh, neither of which
    // returns a null mesh), so the change is invisible on the wire and observable only here.
    TestEqual(TEXT("flip_normals refuses it as MESH_NOT_FOUND"),
        Flip.ErrorCode, FString(ErrorCodes::ERR_MESH_NOT_FOUND));
    TestEqual(TEXT("with the shared null-mesh message"),
        Flip.ErrorMessage, FString(TEXT("DynamicMesh not available")));

    const GeometryOps::FOpResult Merge =
        GeometryOps::MergeVertices(nullptr, GeometryOps::FMergeVerticesParams());
    TestFalse(TEXT("merge_vertices refuses a null mesh"), Merge.bSuccess);
    TestEqual(TEXT("merge_vertices refuses it with the same code"),
        Merge.ErrorCode, FString(ErrorCodes::ERR_MESH_NOT_FOUND));

    GeometryOps::FStretchParams StretchParams;
    const GeometryOps::FOpResult Stretch = GeometryOps::Stretch(nullptr, StretchParams);
    TestFalse(TEXT("stretch refuses a null mesh"), Stretch.bSuccess);
    TestEqual(TEXT("stretch refuses it with the same code"),
        Stretch.ErrorCode, FString(ErrorCodes::ERR_MESH_NOT_FOUND));

    return true;
}

// ============================================================================
// Clamping, outcome structs, and the counts the wrappers now echo
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingFactorClampTest,
    "PinWright.Geometry.Ops.Modeling.SpherifyAndCylindrifyClampTheirFactorAndWarn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingFactorClampTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewBoxMesh());

    GeometryOps::FSpherifyParams SpherifyParams;
    SpherifyParams.Factor = 5.0;
    const GeometryOps::FOpResult Spherify = GeometryOps::Spherify(Mesh.Get(), SpherifyParams);
    TestTrue(TEXT("spherify succeeds with an out-of-range factor"), Spherify.bSuccess);
    TestTrue(TEXT("spherify reports the clamp instead of applying it silently"),
        GeometryOpsModelingTest_AnyWarningContains(Spherify, TEXT("clamped")));

    TStrongObjectPtr<UDynamicMesh> Other(GeometryOpsModelingTest_NewBoxMesh());
    GeometryOps::FCylindrifyParams CylindrifyParams;
    CylindrifyParams.Factor = -1.0;
    GeometryOps::FCylindrifyOutcome Outcome;
    const GeometryOps::FOpResult Cylindrify =
        GeometryOps::Cylindrify(Other.Get(), CylindrifyParams, &Outcome);
    TestTrue(TEXT("cylindrify succeeds with an out-of-range factor"), Cylindrify.bSuccess);
    TestTrue(TEXT("cylindrify reports the clamp"),
        GeometryOpsModelingTest_AnyWarningContains(Cylindrify, TEXT("clamped")));
    // avgRadius is echoed straight into the RPC response, so it must be a real measurement.
    TestTrue(TEXT("cylindrify measures a positive average radius"), Outcome.AverageRadius > 0.0);
    TestTrue(TEXT("cylindrify visits the box's vertices"), Outcome.VerticesModified > 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingFillHolesTest,
    "PinWright.Geometry.Ops.Modeling.FillHolesReportsFilledAndFailedCounts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingFillHolesTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewBoxMesh());

    // Punch one triangle out of the closed box, leaving exactly one hole.
    const UE::Geometry::EMeshResult Removed = Mesh->GetMeshRef().RemoveTriangle(0);
    TestEqual(TEXT("the fixture removed a triangle"),
        static_cast<int32>(Removed), static_cast<int32>(UE::Geometry::EMeshResult::Ok));

    GeometryOps::FFillHolesOutcome Outcome;
    const GeometryOps::FOpResult Op =
        GeometryOps::FillHoles(Mesh.Get(), GeometryOps::FFillHolesParams(), &Outcome);

    TestTrue(TEXT("fill_holes succeeds"), Op.bSuccess);
    // These two feed the verb's filledHoles/failedHoles response fields directly.
    TestEqual(TEXT("fill_holes fills the one hole"), Outcome.FilledHoles, 1);
    TestEqual(TEXT("fill_holes fails none"), Outcome.FailedHoles, 0);
    TestTrue(TEXT("fill_holes adds geometry back"), Op.TrianglesAfter > Op.TrianglesBefore);
    TestTrue(TEXT("fill_holes reports the change"), Op.bChanged);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingFillHolesMissingOverlaysTest,
    "PinWright.Geometry.Ops.Modeling.FillHolesCreatesRequiredAttributeLayers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingFillHolesMissingOverlaysTest::RunTest(const FString& Parameters)
{
    // UV0 is the reachable append_buffers state from the ticket. Before the production preflight,
    // the engine filled the hole, wrote its normals, then asserted while projecting into UV layer 0.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewBoxMesh());
        Mesh->GetMeshRef().RemoveTriangle(0);
        Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
        {
            EditMesh.Attributes()->SetNumUVLayers(0);
        });

        TestTrue(TEXT("the UV-less fixture keeps its attribute set"),
            Mesh->GetMeshRef().HasAttributes());
        TestEqual(TEXT("the UV-less fixture has zero UV layers"),
            Mesh->GetMeshRef().Attributes()->NumUVLayers(), 0);

        GeometryOps::FFillHolesOutcome Outcome;
        const GeometryOps::FOpResult Op =
            GeometryOps::FillHoles(Mesh.Get(), GeometryOps::FFillHolesParams(), &Outcome);

        TestTrue(TEXT("fill_holes succeeds on an attributed mesh with no UV layer"), Op.bSuccess);
        TestEqual(TEXT("fill_holes creates only the required UV0 layer"),
            Mesh->GetMeshRef().Attributes()->NumUVLayers(), 1);
        TestNotNull(TEXT("the engine has a UV overlay to project the filled triangles into"),
            Mesh->GetMeshRef().Attributes()->PrimaryUV());
        TestEqual(TEXT("the hole is still filled after UV-layer preparation"),
            Outcome.FilledHoles, 1);
    }

    // SetTriangleNormals has the sibling unchecked dereference: HasAttributes() is true but
    // PrimaryNormals() is null. The shared preflight must create that overlay too.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewBoxMesh());
        Mesh->GetMeshRef().RemoveTriangle(0);
        Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
        {
            EditMesh.Attributes()->SetNumNormalLayers(0);
        });

        TestNull(TEXT("the normal-less fixture has no primary normal overlay"),
            Mesh->GetMeshRef().Attributes()->PrimaryNormals());

        GeometryOps::FFillHolesOutcome Outcome;
        const GeometryOps::FOpResult Op =
            GeometryOps::FillHoles(Mesh.Get(), GeometryOps::FFillHolesParams(), &Outcome);

        TestTrue(TEXT("fill_holes succeeds on an attributed mesh with no normal layer"),
            Op.bSuccess);
        TestNotNull(TEXT("the engine has a normal overlay for the filled triangles"),
            Mesh->GetMeshRef().Attributes()->PrimaryNormals());
        TestEqual(TEXT("the hole is still filled after normal-layer preparation"),
            Outcome.FilledHoles, 1);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingSimplifyCountsTest,
    "PinWright.Geometry.Ops.Modeling.SimplifyReportsBeforeAndAfterTriangleCounts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingSimplifyCountsTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewBoxMesh());

    GeometryOps::FSubdivideParams SubdivideParams;
    SubdivideParams.Iterations = 2;
    GeometryOps::Subdivide(Mesh.Get(), SubdivideParams);

    GeometryOps::FSimplifyMeshParams Params;
    Params.TargetPercentage = 25.0;
    const GeometryOps::FOpResult Op = GeometryOps::SimplifyMesh(Mesh.Get(), Params);

    // simplify_mesh's originalTriangles / simplifiedTriangles / reductionPercent response
    // fields are all computed from these two numbers, so a wrong one changes the published
    // response without changing any geometry.
    TestTrue(TEXT("simplify_mesh succeeds"), Op.bSuccess);
    TestEqual(TEXT("simplify_mesh reports the pre-op triangle count"), Op.TrianglesBefore, 192);
    TestTrue(TEXT("simplify_mesh reduces the triangle count"),
        Op.TrianglesAfter < Op.TrianglesBefore);
    TestTrue(TEXT("simplify_mesh keeps at least one triangle"), Op.TrianglesAfter > 0);
    TestTrue(TEXT("simplify_mesh reports the change"), Op.bChanged);

    return true;
}

// ============================================================================
// noise_deform: the seed selects the field, and omitting it changes nothing
//
// Perlin noise is a SPATIAL field, so `seed` is not a per-call random draw and cannot be
// checked by "the result differs from last time". What it has to satisfy is a pair of
// properties that pull in opposite directions, and only the pair is meaningful:
//
//   - Two seeds must disagree. Before FNoiseDeformParams carried a Seed, every noise_deform
//     in every model ran at RandomSeed 0, so a family of variants generated from one recipe
//     came out byte-identical no matter how it was seeded. That is invisible in every count
//     the op reports - the triangle and vertex counts are unchanged by noise - so nothing
//     short of comparing positions can fail.
//   - One seed must agree with itself, run to run. That is the half that makes .pwmodel a
//     durable source; a seed that varied per call would make every recompile a new asset.
//
// The default-versus-explicit-0 test is the one that pins the compatibility promise: a
// document or request that names no seed must reach the engine with exactly the field it
// reached before the parameter existed.
// ============================================================================

namespace
{
TArray<FVector> GeometryOpsModelingTest_VertexPositions(UDynamicMesh* Mesh)
{
    TArray<FVector> Positions;
    Mesh->ProcessMesh([&Positions](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        Positions.Reserve(ReadMesh.VertexCount());
        for (const int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            Positions.Add(ReadMesh.GetVertex(VertexID));
        }
    });
    return Positions;
}

// Largest per-vertex displacement between two runs, or a negative sentinel when the two runs
// did not even produce the same vertex set - which would be a different defect from a
// different noise field and must not read as one.
double GeometryOpsModelingTest_MaxVertexDelta(const TArray<FVector>& A, const TArray<FVector>& B)
{
    if (A.Num() != B.Num() || A.Num() == 0)
    {
        return -1.0;
    }
    double MaxDelta = 0.0;
    for (int32 Index = 0; Index < A.Num(); ++Index)
    {
        MaxDelta = FMath::Max(MaxDelta, FVector::Dist(A[Index], B[Index]));
    }
    return MaxDelta;
}

// A subdivided box: enough distinct sample positions that two noise fields cannot agree at
// every one of them by chance. A bare 8-vertex box would make "the seeds disagree" a coin
// flip on eight corners.
UDynamicMesh* GeometryOpsModelingTest_NewNoiseFixtureMesh()
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform::Identity, 100.0f, 100.0f, 100.0f, 6, 6, 6,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

TArray<FVector> GeometryOpsModelingTest_NoisedPositions(const GeometryOps::FNoiseDeformParams& Params)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewNoiseFixtureMesh());
    GeometryOps::NoiseDeform(Mesh.Get(), Params);
    return GeometryOpsModelingTest_VertexPositions(Mesh.Get());
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingNoiseSeedTest,
    "PinWright.Geometry.Ops.Modeling.NoiseDeformSeedSelectsADistinctReproducibleField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingNoiseSeedTest::RunTest(const FString& Parameters)
{
    GeometryOps::FNoiseDeformParams Base;
    Base.Magnitude = 12.0;
    Base.Frequency = 0.05;

    // The undeformed fixture, so a magnitude that turned out to be a no-op cannot make the
    // "same seed agrees" assertion pass for the wrong reason.
    TStrongObjectPtr<UDynamicMesh> Plain(GeometryOpsModelingTest_NewNoiseFixtureMesh());
    const TArray<FVector> Undeformed = GeometryOpsModelingTest_VertexPositions(Plain.Get());

    GeometryOps::FNoiseDeformParams SeedZero = Base;
    SeedZero.Seed = 0;
    const TArray<FVector> ZeroA = GeometryOpsModelingTest_NoisedPositions(SeedZero);
    const TArray<FVector> ZeroB = GeometryOpsModelingTest_NoisedPositions(SeedZero);

    GeometryOps::FNoiseDeformParams SeedOak = Base;
    SeedOak.Seed = 101;
    const TArray<FVector> Oak = GeometryOpsModelingTest_NoisedPositions(SeedOak);

    // Defaults untouched: this is what a caller who names no seed gets.
    const TArray<FVector> Defaulted = GeometryOpsModelingTest_NoisedPositions(Base);

    TestTrue(TEXT("the fixture produced vertices"), ZeroA.Num() > 0);
    TestTrue(TEXT("noise_deform actually displaced the fixture"),
        GeometryOpsModelingTest_MaxVertexDelta(Undeformed, ZeroA) > 1.0);

    TestTrue(TEXT("one seed reproduces itself bit-exactly"),
        GeometryOpsModelingTest_MaxVertexDelta(ZeroA, ZeroB) == 0.0);

    TestTrue(TEXT("a different seed selects a different field"),
        GeometryOpsModelingTest_MaxVertexDelta(ZeroA, Oak) > 0.1);

    // The compatibility promise, stated as an assertion: the struct default IS seed 0.
    TestTrue(TEXT("omitting the seed is bit-exactly seed 0"),
        GeometryOpsModelingTest_MaxVertexDelta(Defaulted, ZeroA) == 0.0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingNoiseFrequencyShiftTest,
    "PinWright.Geometry.Ops.Modeling.NoiseDeformFrequencyShiftSlidesTheField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingNoiseFrequencyShiftTest::RunTest(const FString& Parameters)
{
    GeometryOps::FNoiseDeformParams Base;
    Base.Magnitude = 12.0;
    Base.Frequency = 0.05;

    const TArray<FVector> Unshifted = GeometryOpsModelingTest_NoisedPositions(Base);

    GeometryOps::FNoiseDeformParams Shifted = Base;
    Shifted.FrequencyShift = FVector(37.0, -19.0, 5.0);
    const TArray<FVector> Moved = GeometryOpsModelingTest_NoisedPositions(Shifted);

    TestTrue(TEXT("a frequency shift moves the sampling window"),
        GeometryOpsModelingTest_MaxVertexDelta(Unshifted, Moved) > 0.1);

    // `Base` leaves FrequencyShift at its struct default, so `Unshifted` is simultaneously the
    // "shift omitted" case; re-running it is the reproducibility half of the same property.
    const TArray<FVector> MovedAgain = GeometryOpsModelingTest_NoisedPositions(Shifted);
    TestTrue(TEXT("a shifted field reproduces itself bit-exactly"),
        GeometryOpsModelingTest_MaxVertexDelta(Moved, MovedAgain) == 0.0);

    return true;
}

// ============================================================================
// shell's crash guard, and the two shapes it must NOT confuse with each other
// ============================================================================
//
// The guard fails NO_UV_ELEMENTS on an open mesh whose UV channel 0 does not cover every
// triangle, because the engine's boundary stitcher reads a specific triangle's UV elements with
// no null check on the overlay and no IsSetTriangle check on the triangle
// (JoinMeshLoops.cpp:81 and :54). Both tests below are about the guard's PRECISION rather than
// its existence: one shape it must reject that it used to accept, and one it must accept that
// it briefly rejected.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingShellEmptyMeshTest,
    "PinWright.Geometry.Ops.Modeling.ShellOnAnEmptyMeshWarnsInsteadOfDemandingUVs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingShellEmptyMeshTest::RunTest(const FString& Parameters)
{
    // A bare UDynamicMesh is what `procedural_mesh` produces: attributes enabled, one UV layer,
    // zero UV elements, zero triangles.
    TStrongObjectPtr<UDynamicMesh> Mesh(NewObject<UDynamicMesh>(GetTransientPackage()));

    // The two engine facts that made this a false positive, asserted rather than assumed.
    // FDynamicMesh3::IsClosed() counts boundary edges and a mesh with no edges has none, so it
    // answers FALSE on an empty mesh (DynamicMesh3_Queries.cpp:752-757) - i.e. "not closed",
    // which paired with "no usable UVs" is exactly the guard's rejection condition.
    TestEqual(TEXT("the fixture really is empty"), Mesh->GetTriangleCount(), 0);
    TestFalse(TEXT("IsClosed() is FALSE on an empty mesh - the term that made the guard misfire"),
        Mesh->GetMeshRef().IsClosed());
    TestFalse(TEXT("and an empty mesh has no usable UVs either"),
        GeometryUtils::MeshHasUsableUVs(Mesh.Get()));

    GeometryOps::FShellParams Params;
    Params.Thickness = 5.0;
    const GeometryOps::FOpResult Op = GeometryOps::Shell(Mesh.Get(), Params);

    // The regression, stated as the assertion that fails if the triangle-count term is removed.
    TestNotEqual(TEXT("shell must NOT answer an empty mesh with a UV complaint"),
        Op.ErrorCode, FString(ErrorCodes::ERR_NO_UV_ELEMENTS));
    TestTrue(*FString::Printf(TEXT("shell on an empty mesh succeeds. code='%s' msg='%s'"),
            *Op.ErrorCode, *Op.ErrorMessage), Op.bSuccess);
    TestFalse(TEXT("and reports that nothing changed"), Op.bChanged);
    TestEqual(TEXT("and left the mesh empty"), Op.TrianglesAfter, 0);

    // Deliberate: success, but not SILENT success. A modifier that moves no counts and says
    // nothing is this surface's most confusing outcome - the same reason bevel warns on a mesh
    // with no polygroups.
    TestTrue(TEXT("shell says why an empty mesh changed nothing"),
        GeometryOpsModelingTest_AnyWarningContains(Op, TEXT("no triangles")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingShellPartialUVsTest,
    "PinWright.Geometry.Ops.Modeling.ShellRefusesAnOpenMeshWhoseUVsMissSomeTriangles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingShellPartialUVsTest::RunTest(const FString& Parameters)
{
    // Control first: a fully UV'd open mesh still shells. Without this the test below would pass
    // just as well if the guard rejected every open mesh.
    {
        TStrongObjectPtr<UDynamicMesh> Rect(GeometryOpsModelingTest_NewRectangleMesh());
        TestTrue(TEXT("a primitive rectangle carries UVs on every triangle"),
            GeometryUtils::MeshHasUVsOnEveryTriangle(Rect.Get()));

        GeometryOps::FShellParams Params;
        Params.Thickness = 5.0;
        const GeometryOps::FOpResult Op = GeometryOps::Shell(Rect.Get(), Params);
        TestTrue(*FString::Printf(TEXT("shell still works on a fully UV'd open mesh. code='%s'"),
                *Op.ErrorCode), Op.bSuccess);
        TestTrue(TEXT("and it built the shell"), Op.TrianglesAfter > Op.TrianglesBefore);
    }

    // Now the residual gap. UnsetTriangle with bAllowElementFreeing=false leaves the elements
    // allocated and only clears the triangle's references, which is precisely the shape
    // MeshHasUsableUVs cannot see: ElementCount() is still positive while a triangle's entry in
    // ElementTriangles is -1. Feeding that triangle to GetTriElements indexes Elements[-2].
    TStrongObjectPtr<UDynamicMesh> Partial(GeometryOpsModelingTest_NewRectangleMesh());
    int32 UnsetTriangleID = INDEX_NONE;
    Partial->EditMesh([&UnsetTriangleID](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = EditMesh.Attributes()->PrimaryUV();
        for (const int32 TriangleID : EditMesh.TriangleIndicesItr())
        {
            UnsetTriangleID = TriangleID;
            UVOverlay->UnsetTriangle(TriangleID, /*bAllowElementFreeing=*/false);
            break;
        }
    });

    TestTrue(TEXT("the fixture unset a triangle"), UnsetTriangleID != INDEX_NONE);

    // The gap itself, asserted on the two predicates rather than described in a comment: the
    // old guard's question still answers "fine", the new one does not.
    TestTrue(TEXT("MeshHasUsableUVs STILL says yes - ElementCount() > 0 cannot see an unset triangle"),
        GeometryUtils::MeshHasUsableUVs(Partial.Get()));
    TestFalse(TEXT("MeshHasUVsOnEveryTriangle says no, which is the question shell has"),
        GeometryUtils::MeshHasUVsOnEveryTriangle(Partial.Get()));
    TestFalse(TEXT("and the mesh is open, so the boundary stitcher would run"),
        Partial->GetMeshRef().IsClosed());

    GeometryOps::FShellParams Params;
    Params.Thickness = 5.0;
    const GeometryOps::FOpResult Op = GeometryOps::Shell(Partial.Get(), Params);

    TestFalse(TEXT("shell refuses a partially-UV'd open mesh"), Op.bSuccess);
    TestEqual(TEXT("and refuses it as NO_UV_ELEMENTS"),
        Op.ErrorCode, FString(ErrorCodes::ERR_NO_UV_ELEMENTS));
    TestTrue(*FString::Printf(TEXT("the message names UVs. '%s'"), *Op.ErrorMessage),
        Op.ErrorMessage.Contains(TEXT("UV")));
    TestEqual(TEXT("a refused shell leaves the mesh untouched"),
        Partial->GetTriangleCount(), Op.TrianglesBefore);

    return true;
}

// The predicate's CHANNEL parameter, which the .pwmodel merge stage depends on.
//
// FCompiler::FillMissingUVChannels loops over every channel any part populated and asks this
// question per channel. It used to ask an element-count question instead, which treats a channel
// with SOME elements as complete - so the compiler's own box-projection rescue skipped exactly
// the mesh it existed to repair. Widening the guard is only a fix if the widened form actually
// answers about the channel it was handed, and a defaulted parameter is the easiest thing in the
// file to get silently wrong: an implementation that ignored UVChannel and answered about
// channel 0 would pass every other test here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingUVsOnEveryTrianglePerChannelTest,
    "PinWright.Geometry.Ops.Modeling.UVsOnEveryTriangleAnswersAboutTheChannelItIsGiven",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingUVsOnEveryTrianglePerChannelTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewRectangleMesh());

    // Baseline: channel 0 is complete, channel 1 does not exist at all.
    TestTrue(TEXT("channel 0 is complete on a primitive rectangle"),
        GeometryUtils::MeshHasUVsOnEveryTriangle(Mesh.Get(), 0));
    TestTrue(TEXT("the default argument is channel 0"),
        GeometryUtils::MeshHasUVsOnEveryTriangle(Mesh.Get()));
    TestFalse(TEXT("an absent channel is not complete"),
        GeometryUtils::MeshHasUVsOnEveryTriangle(Mesh.Get(), 1));
    TestFalse(TEXT("a negative channel is not complete"),
        GeometryUtils::MeshHasUVsOnEveryTriangle(Mesh.Get(), -1));
    TestFalse(TEXT("a null mesh is not complete"),
        GeometryUtils::MeshHasUVsOnEveryTriangle(nullptr, 0));

    // A layer that EXISTS but is element-less is the case the element-presence half rejects, and
    // it is what EnsureMeshHasUVChannel leaves behind before a projection runs.
    TestTrue(TEXT("channel 1 can be grown"),
        GeometryUtils::EnsureMeshHasUVChannel(Mesh.Get(), 1));
    TestFalse(TEXT("an element-less channel 1 is still not complete"),
        GeometryUtils::MeshHasUVsOnEveryTriangle(Mesh.Get(), 1));
    TestTrue(TEXT("and growing a layer did not disturb channel 0"),
        GeometryUtils::MeshHasUVsOnEveryTriangle(Mesh.Get(), 0));

    // Now make channel 1 complete and channel 0 PARTIAL, so the two channels disagree and a
    // predicate that ignored its argument would have to answer one of them wrongly.
    UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromBoxProjection(
        Mesh.Get(), /*UVSetIndex=*/1, FTransform(FQuat::Identity, FVector::ZeroVector, FVector(100.0)),
        FGeometryScriptMeshSelection(), /*MinIslandTriCount=*/2, nullptr);

    Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = EditMesh.Attributes()->GetUVLayer(0);
        for (const int32 TriangleID : EditMesh.TriangleIndicesItr())
        {
            // bAllowElementFreeing=false keeps ElementCount() positive, which is the whole point:
            // the channel stays "usable" by the old question and incomplete by this one.
            UVOverlay->UnsetTriangle(TriangleID, /*bAllowElementFreeing=*/false);
            break;
        }
    });

    TestFalse(TEXT("channel 0 is now incomplete"),
        GeometryUtils::MeshHasUVsOnEveryTriangle(Mesh.Get(), 0));
    TestTrue(TEXT("while channel 0 still has elements - the distinction the fix turns on"),
        GeometryUtils::MeshHasUVElementsInChannel(Mesh.Get(), 0));
    TestTrue(TEXT("and channel 1 is complete, so the two channels answer differently"),
        GeometryUtils::MeshHasUVsOnEveryTriangle(Mesh.Get(), 1));

    return true;
}

// ============================================================================
// The warp deformers' normal-overlay crash guard
//
// bend, twist and taper each drive an FMeshSpaceDeformerOp subclass, and all three subclasses
// open their normal pass with the same two unchecked lines (FlareMeshOp.cpp:71-78,
// BendMeshOp.cpp:106-113, TwistMeshOp.cpp:51-58): IsElement is the only guard, then the element's
// PARENT VERTEX goes straight into FDynamicMesh3::GetVertex. IsElement does not imply a parent -
// AppendElement allocates and writes InvalidID - and GetVertex's bounds test is a checkSlow,
// compiled out here, over a TDynamicVector::operator[] that takes a uint32. So -1 reads out of
// bounds at the constant offset 511 * sizeof(FVector3d) = 0x2FE8, which is the address every
// EXCEPTION_ACCESS_VIOLATION dump in Saved/Crashes carries.
//
// Four tests, because the guard makes three different decisions and each has to be pinned:
// REPAIR the orphan (it is legal, engine-produced, information-free input), REFUSE a mesh with no
// normal layer at all (a different unchecked read, one PinWright cannot produce), and LEAVE A
// CLEAN MESH ALONE at any extent. The clean-mesh test is what fails if the guard is tightened;
// the orphan tests are what fail if it is loosened.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingTaperOrphanNormalTest,
    "PinWright.Geometry.Ops.Modeling.TaperRepairsAnUnparentedNormalElementInsteadOfCrashing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingTaperOrphanNormalTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewBoxMesh());

    TestTrue(TEXT("a primitive box starts with every normal element parented"),
        GeometryUtils::MeshNormalParentsAreValid(Mesh.Get()));

    const int32 OrphanID = GeometryOpsModelingTest_AppendOrphanNormalElement(Mesh.Get());
    TestTrue(TEXT("the fixture appended a normal element"), OrphanID != INDEX_NONE);

    // The precondition stated as assertions on the mesh, not as a comment: the engine's own guard
    // waves this element through, and the value it then feeds to GetVertex is -1.
    {
        const UE::Geometry::FDynamicMeshNormalOverlay* Normals =
            Mesh->GetMeshRef().Attributes()->PrimaryNormals();
        TestTrue(TEXT("IsElement - the ONLY check the deformers make - says the element is valid"),
            Normals->IsElement(OrphanID));
        TestEqual(TEXT("and its parent vertex is InvalidID, which is what indexes out of bounds"),
            Normals->GetParentVertex(OrphanID), (int32)UE::Geometry::FDynamicMesh3::InvalidID);
    }
    TestFalse(TEXT("so the mesh fails the deformers' precondition"),
        GeometryUtils::MeshNormalParentsAreValid(Mesh.Get()));

    // driftwood's own numbers, and deliberately so: the crash was first blamed on an extent far
    // larger than the mesh's half-height. It is not - ZMin/ZMax reach the faulting lambda only
    // through T = Clamp((z - ZMin) / (ZMax - ZMin), 0, 1) - and this test passes identically at
    // extent=1. The clean-mesh test below is the other half of that claim.
    GeometryOps::FTaperParams Params;
    Params.FlareX = 34.0;
    Params.FlareY = 12.0;
    Params.Extent = 268.0;

    const GeometryOps::FOpResult Op = GeometryOps::Taper(Mesh.Get(), Params);

    TestTrue(*FString::Printf(
            TEXT("taper survives the shape that crashed the editor. code='%s' msg='%s'"),
            *Op.ErrorCode, *Op.ErrorMessage), Op.bSuccess);
    TestTrue(TEXT("and it repaired rather than refused - an orphan is legal, information-free input"),
        GeometryUtils::MeshNormalParentsAreValid(Mesh.Get()));
    TestFalse(TEXT("the orphan is freed, not merely reparented onto some vertex"),
        Mesh->GetMeshRef().Attributes()->PrimaryNormals()->IsElement(OrphanID));
    TestTrue(TEXT("and the repair is announced, not silent"),
        GeometryOpsModelingTest_AnyWarningContains(Op, TEXT("unparented normal element")));
    TestEqual(TEXT("freeing an element no triangle used moves no geometry"),
        Op.TrianglesAfter, Op.TrianglesBefore);
    TestEqual(TEXT("and no vertices either"), Op.VerticesAfter, Op.VerticesBefore);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingBendTwistOrphanNormalTest,
    "PinWright.Geometry.Ops.Modeling.BendAndTwistShareTaperUnparentedNormalGuard",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingBendTwistOrphanNormalTest::RunTest(const FString& Parameters)
{
    // The siblings, because the defect is copy-pasted across the three engine files and a guard on
    // taper alone would leave two ops that still take the editor down on the same document.
    // driftwood runs all three back to back, so a taper-only fix would have moved the crash one
    // line rather than removing it.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewBoxMesh());
        GeometryOpsModelingTest_AppendOrphanNormalElement(Mesh.Get());
        TestFalse(TEXT("the bend fixture is the crashing shape"),
            GeometryUtils::MeshNormalParentsAreValid(Mesh.Get()));

        GeometryOps::FBendParams Params;
        Params.Angle = 30.0;
        Params.Extent = 170.0;
        const GeometryOps::FOpResult Op = GeometryOps::Bend(Mesh.Get(), Params);

        TestTrue(*FString::Printf(TEXT("bend survives it too. code='%s'"), *Op.ErrorCode), Op.bSuccess);
        TestTrue(TEXT("bend repaired the overlay"),
            GeometryUtils::MeshNormalParentsAreValid(Mesh.Get()));
        TestTrue(TEXT("bend announced the repair"),
            GeometryOpsModelingTest_AnyWarningContains(Op, TEXT("unparented normal element")));
    }

    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewBoxMesh());
        GeometryOpsModelingTest_AppendOrphanNormalElement(Mesh.Get());
        TestFalse(TEXT("the twist fixture is the crashing shape"),
            GeometryUtils::MeshNormalParentsAreValid(Mesh.Get()));

        GeometryOps::FTwistParams Params;
        Params.Angle = 70.0;
        Params.Extent = 170.0;
        const GeometryOps::FOpResult Op = GeometryOps::Twist(Mesh.Get(), Params);

        TestTrue(*FString::Printf(TEXT("twist survives it too. code='%s'"), *Op.ErrorCode), Op.bSuccess);
        TestTrue(TEXT("twist repaired the overlay"),
            GeometryUtils::MeshNormalParentsAreValid(Mesh.Get()));
        TestTrue(TEXT("twist announced the repair"),
            GeometryOpsModelingTest_AnyWarningContains(Op, TEXT("unparented normal element")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingTaperCleanMeshTest,
    "PinWright.Geometry.Ops.Modeling.TaperOnACleanMeshIsUntouchedAtEveryExtent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingTaperCleanMeshTest::RunTest(const FString& Parameters)
{
    // The control, and the assertion that fails if the guard is ever tightened into a refusal.
    // Two extents on the SAME clean mesh: amphora's, which is smaller than the mesh, and
    // driftwood's, which is many times larger. Both must behave identically, which is what pins
    // the diagnosis - if extent magnitude were the trigger, these two would have to differ.
    const double Extents[] = { 11.2, 268.0 };

    for (const double Extent : Extents)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewBoxMesh());
        TestTrue(TEXT("the fixture is clean to begin with"),
            GeometryUtils::MeshNormalParentsAreValid(Mesh.Get()));

        GeometryOps::FTaperParams Params;
        Params.FlareX = -20.0;
        Params.FlareY = 0.0;
        Params.Extent = Extent;

        const GeometryOps::FOpResult Op = GeometryOps::Taper(Mesh.Get(), Params);

        TestTrue(*FString::Printf(TEXT("taper still runs at extent=%f. code='%s' msg='%s'"),
                Extent, *Op.ErrorCode, *Op.ErrorMessage), Op.bSuccess);
        TestTrue(*FString::Printf(TEXT("and still reports that it deformed the mesh at extent=%f"),
                Extent), Op.bChanged);
        TestEqual(*FString::Printf(TEXT("a warp moves no triangles at extent=%f"), Extent),
            Op.TrianglesAfter, Op.TrianglesBefore);

        // The guard has to be INVISIBLE on a clean mesh: no repair, so nothing to say. A warning
        // here would mean the sweep is finding orphans that are not there.
        TestFalse(*FString::Printf(TEXT("and says nothing about normals at extent=%f"), Extent),
            GeometryOpsModelingTest_AnyWarningContains(Op, TEXT("unparented normal element")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsModelingTaperNoNormalLayerTest,
    "PinWright.Geometry.Ops.Modeling.TaperRefusesAMeshWithNoNormalLayer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsModelingTaperNoNormalLayerTest::RunTest(const FString& Parameters)
{
    // The other arm, and the one shape the guard REFUSES. An attribute set with zero normal layers
    // makes PrimaryNormals() return nullptr (DynamicMeshAttributeSet.h:250), and the deformers
    // call MaxElementID() on it before they reach any element at all - a null dereference one step
    // earlier than the orphan read. Nothing repairs that without inventing normals the document
    // never asked for, so it fails with a code instead. bevel already refuses the same shape.
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsModelingTest_NewBoxMesh());
    Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        EditMesh.Attributes()->SetNumNormalLayers(0);
    });

    TestNull(TEXT("the fixture really has no primary normal overlay"),
        Mesh->GetMeshRef().Attributes()->PrimaryNormals());
    TestTrue(TEXT("and the parent predicate answers TRUE on it - there is no element to be "
                  "unparented, which is why the null case has to be asked separately"),
        GeometryUtils::MeshNormalParentsAreValid(Mesh.Get()));

    GeometryOps::FTaperParams Params;
    Params.FlareX = -20.0;
    Params.FlareY = 0.0;
    Params.Extent = 11.2;

    const GeometryOps::FOpResult Op = GeometryOps::Taper(Mesh.Get(), Params);

    TestFalse(TEXT("taper refuses a mesh with no normal layer"), Op.bSuccess);
    TestEqual(TEXT("and refuses it as INVALID_NORMAL_OVERLAY"),
        Op.ErrorCode, FString(ErrorCodes::ERR_INVALID_NORMAL_OVERLAY));
    TestTrue(*FString::Printf(TEXT("the message names the normal layer. '%s'"), *Op.ErrorMessage),
        Op.ErrorMessage.Contains(TEXT("normal layer")));
    TestEqual(TEXT("a refused taper leaves the mesh untouched"),
        Mesh->GetTriangleCount(), Op.TrianglesBefore);

    return true;
}
