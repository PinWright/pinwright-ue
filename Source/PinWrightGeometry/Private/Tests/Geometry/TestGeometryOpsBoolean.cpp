// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for GeometryOps::Boolean / Trim / SelfUnion / Mirror / ValidateArrayCount /
// ArrayLinear / ArrayRadial - the 9 boolean and transform verbs extracted out of
// BooleanHandler.cpp and GeometryTransformHandler.cpp so they run with no actor, no editor
// world and no FHandlerContext in scope.
//
// The dispatcher-level geometry.boolean_* / geometry.mirror / geometry.array_* tests are
// untouched and remain the regression net for the RPC contract. This file covers what only the
// pure layer can express, each of which reverting would silently break:
//
//   1. bChanged separates a real cut from a no-effect pass. ApplyMeshBoolean returns the target
//      UNCHANGED when the two meshes do not overlap in their resolved common space, so bSuccess
//      alone reports a disjoint subtract as a clean success. That is the most confusing failure
//      on this surface, because every downstream op then runs happily on unmodified geometry.
//      Wire bChanged to a constant true and DisjointSubtractReportsNoChange fails.
//   2. The two transforms are load-bearing parameters, not decoration. The RPC wrappers pass the
//      two actors' transforms; the .pwmodel compiler passes part-local ones with no actor in
//      sight. Drop either transform on the floor inside the op and the same pair of meshes stops
//      overlapping - which is exactly the bug the spawn-transform convention was fixed for.
//   3. The MEMORY_PRESSURE / POLYGON_LIMIT_EXCEEDED guards run BEFORE either mesh is touched, so
//      a caller that trips one is left with exactly the geometry it had.
//   4. mirror is a mirror-AND-MERGE, and an unrecognized axis appends an UNMIRRORED clone rather
//      than being rejected. Neither RPC verb validates its `axis` string, so that fallback is
//      shipped behaviour; GeometryOps::EMeshAxis::None is how the op reproduces it.
//   5. A null mesh comes back as MESH_NOT_FOUND instead of a dereference. Unreachable from the
//      RPC front-ends - GeometryTarget::ResolveOrSendError never returns true with a null Mesh,
//      and boolean_trim checks both handles itself - so nothing here can change a response; it
//      is the .pwmodel compiler that needs a code back rather than a crash inside an engine
//      call. The order in the array ops is part of the contract: the count check runs first, so
//      a bad count still reports INVALID_ARGUMENT even with no mesh at all.
#include "Misc/AutomationTest.h"

#include "Compat/EngineVersionCompat.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOps_Boolean.h"
#include "Handlers/Geometry/GeometryUtils.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"

#include "GeometryScript/MeshPrimitiveFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

UDynamicMesh* GeometryOpsBooleanTest_NewMesh()
{
    return NewObject<UDynamicMesh>(GetTransientPackage());
}

// Steps subdivides every face, which is the only cheap way to build a mesh dense enough to trip
// the array polygon-limit guard.
UDynamicMesh* GeometryOpsBooleanTest_AppendBox(
    UDynamicMesh* Mesh, const FVector& Center, double Dimension, int32 Steps = 0)
{
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform(Center),
        static_cast<float>(Dimension), static_cast<float>(Dimension), static_cast<float>(Dimension),
        Steps, Steps, Steps,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

UDynamicMesh* GeometryOpsBooleanTest_NewBox(const FVector& Center, double Dimension, int32 Steps = 0)
{
    return GeometryOpsBooleanTest_AppendBox(GeometryOpsBooleanTest_NewMesh(), Center, Dimension, Steps);
}

UE::Geometry::FAxisAlignedBox3d GeometryOpsBooleanTest_Bounds(UDynamicMesh* Mesh)
{
    return Mesh->GetMeshRef().GetBounds();
}
}

// ============================================================================
// Boolean
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanSubtractCutsTest,
    "PinWright.Geometry.Ops.Boolean.SubtractCutsOverlappingTool",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanSubtractCutsTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Target = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 200.0);
    UDynamicMesh* Tool = GeometryOpsBooleanTest_NewBox(FVector(100.0, 100.0, 100.0), 100.0);

    const int32 TargetTrisBefore = Target->GetTriangleCount();

    const GeometryOps::FOpResult Op = GeometryOps::Boolean(
        Target, FTransform::Identity, Tool, FTransform::Identity,
        EGeometryScriptBooleanOperation::Subtract, GeometryOps::FBooleanParams());

    TestTrue(TEXT("overlapping subtract succeeds"), Op.bSuccess);
    TestEqual(TEXT("error code is empty on success"), Op.ErrorCode, FString());
    TestEqual(TEXT("trianglesBefore is the pre-op target count"), Op.TrianglesBefore, TargetTrisBefore);
    TestTrue(TEXT("trianglesAfter is populated"), Op.TrianglesAfter > 0);
    TestTrue(TEXT("verticesBefore is populated"), Op.VerticesBefore > 0);
    TestTrue(TEXT("verticesAfter is populated"), Op.VerticesAfter > 0);
    TestTrue(TEXT("overlapping subtract reports a change"), Op.bChanged);

    // The tool is read only. A boolean that mutated it would corrupt any caller reusing it as a
    // cutter, which the keepTool=true default makes the normal case.
    TestEqual(TEXT("tool mesh is untouched"), Tool->GetTriangleCount(), 12);

    // WHICH operation ran. Success plus a moved triangle count plus bChanged is equally true of
    // a UNION or an INTERSECTION of this same pair, so hard-wiring the Operation argument of
    // ApplyMeshBoolean is invisible to every assertion above. The bounds separate all three:
    // the target spans -100..100 on each axis and the tool spans 50..150, so a union reaches the
    // tool's far corner at +150 and an intersection keeps only the shared 50..100 octant.
    const UE::Geometry::FAxisAlignedBox3d Bounds = GeometryOpsBooleanTest_Bounds(Target);
    TestTrue(TEXT("subtract adds no material outside the target (a union would reach +150)"),
        Bounds.Max.X < 105.0 && Bounds.Max.Y < 105.0 && Bounds.Max.Z < 105.0);
    TestTrue(TEXT("subtract keeps the target's far side (an intersection would lose it)"),
        Bounds.Min.X < -95.0 && Bounds.Min.Y < -95.0 && Bounds.Min.Z < -95.0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanDisjointTest,
    "PinWright.Geometry.Ops.Boolean.DisjointSubtractReportsNoChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanDisjointTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Target = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 100.0);
    UDynamicMesh* Tool = GeometryOpsBooleanTest_NewBox(FVector(1000.0, 0.0, 0.0), 100.0);

    const GeometryOps::FOpResult Op = GeometryOps::Boolean(
        Target, FTransform::Identity, Tool, FTransform::Identity,
        EGeometryScriptBooleanOperation::Subtract, GeometryOps::FBooleanParams());

    // The whole point: the engine reports success on a boolean that did nothing at all.
    TestTrue(TEXT("disjoint subtract still succeeds"), Op.bSuccess);
    TestFalse(TEXT("disjoint subtract reports bChanged == false"), Op.bChanged);
    TestEqual(TEXT("disjoint subtract leaves the triangle count alone"),
        Op.TrianglesAfter, Op.TrianglesBefore);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanToolTransformTest,
    "PinWright.Geometry.Ops.Boolean.ToolTransformResolvesOverlap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanToolTransformTest::RunTest(const FString& Parameters)
{
    // Both meshes are built at their own local origin, so overlap is decided entirely by the two
    // transforms - the arrangement the RPC wrappers produce from actor transforms and the
    // .pwmodel compiler produces from part-local ones.
    UDynamicMesh* Cut = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 200.0);
    UDynamicMesh* CutTool = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 100.0);

    const GeometryOps::FOpResult CutOp = GeometryOps::Boolean(
        Cut, FTransform::Identity,
        CutTool, FTransform(FVector(100.0, 100.0, 100.0)),
        EGeometryScriptBooleanOperation::Subtract, GeometryOps::FBooleanParams());

    TestTrue(TEXT("transform-resolved overlap succeeds"), CutOp.bSuccess);
    TestTrue(TEXT("tool transform moves the tool onto the target"), CutOp.bChanged);

    UDynamicMesh* Miss = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 200.0);
    UDynamicMesh* MissTool = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 100.0);

    const GeometryOps::FOpResult MissOp = GeometryOps::Boolean(
        Miss, FTransform::Identity,
        MissTool, FTransform(FVector(1000.0, 0.0, 0.0)),
        EGeometryScriptBooleanOperation::Subtract, GeometryOps::FBooleanParams());

    TestTrue(TEXT("transform-separated pair still succeeds"), MissOp.bSuccess);
    TestFalse(TEXT("tool transform moves the tool off the target"), MissOp.bChanged);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanOperationNameTest,
    "PinWright.Geometry.Ops.Boolean.OperationNameMatchesTheEchoedField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanOperationNameTest::RunTest(const FString& Parameters)
{
    // These strings are the `operation` field of the four boolean verbs' response JSON, which
    // dispatcher tests assert on. geometry.difference dispatches Subtract and has always
    // reported "Subtract", not "Difference".
    TestEqual(TEXT("Union"), FString(GeometryOps::BooleanOperationName(
        EGeometryScriptBooleanOperation::Union)), FString(TEXT("Union")));
    TestEqual(TEXT("Intersection"), FString(GeometryOps::BooleanOperationName(
        EGeometryScriptBooleanOperation::Intersection)), FString(TEXT("Intersection")));
    TestEqual(TEXT("Subtract"), FString(GeometryOps::BooleanOperationName(
        EGeometryScriptBooleanOperation::Subtract)), FString(TEXT("Subtract")));
    // The four below joined EGeometryScriptBooleanOperation in UE 5.6; on older engines the
    // enumerators do not exist to name, so the three above are the whole vocabulary.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    TestEqual(TEXT("TrimInside"), FString(GeometryOps::BooleanOperationName(
        EGeometryScriptBooleanOperation::TrimInside)), FString(TEXT("TrimInside")));
    TestEqual(TEXT("TrimOutside"), FString(GeometryOps::BooleanOperationName(
        EGeometryScriptBooleanOperation::TrimOutside)), FString(TEXT("TrimOutside")));
    TestEqual(TEXT("NewPolyGroupInside"), FString(GeometryOps::BooleanOperationName(
        EGeometryScriptBooleanOperation::NewPolyGroupInside)), FString(TEXT("NewPolyGroupInside")));
    TestEqual(TEXT("NewPolyGroupOutside"), FString(GeometryOps::BooleanOperationName(
        EGeometryScriptBooleanOperation::NewPolyGroupOutside)), FString(TEXT("NewPolyGroupOutside")));
#endif

    return true;
}

// ============================================================================
// Trim
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanTrimTest,
    "PinWright.Geometry.Ops.Boolean.TrimKeepsInsideOrOutside",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanTrimTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Inside = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 200.0);
    UDynamicMesh* InsideTool = GeometryOpsBooleanTest_NewBox(FVector(100.0, 0.0, 0.0), 200.0);

    GeometryOps::FTrimParams KeepInside;
    KeepInside.bKeepInside = true;

    const GeometryOps::FOpResult InsideOp = GeometryOps::Trim(
        Inside, FTransform::Identity, InsideTool, FTransform::Identity, KeepInside);

    TestTrue(TEXT("keepInside trim succeeds"), InsideOp.bSuccess);
    TestTrue(TEXT("keepInside trim changes the mesh"), InsideOp.bChanged);
    // Intersection of two 200-boxes offset by 100 on X keeps the shared 100-wide slab.
    const UE::Geometry::FAxisAlignedBox3d InsideBounds = GeometryOpsBooleanTest_Bounds(Inside);
    TestTrue(TEXT("keepInside trim keeps only the overlap"), InsideBounds.Width() < 150.0);

    // WHICH slab, not just how wide. Subtracting the same tool also leaves a 100-wide result -
    // the target's own half, on the far side of X=0 - so a width check passes with
    // `bKeepInside ? Intersection : Subtract` inverted. The target spans X -100..100 and the
    // tool spans 0..200, so keeping the inside lands the result at X 0..100 and keeping the
    // outside lands it at -100..0. The side is the whole behaviour this verb selects.
    TestTrue(TEXT("keepInside keeps the half INSIDE the tool"), InsideBounds.Min.X > -25.0);

    UDynamicMesh* Outside = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 200.0);
    UDynamicMesh* OutsideTool = GeometryOpsBooleanTest_NewBox(FVector(100.0, 0.0, 0.0), 200.0);

    const GeometryOps::FOpResult OutsideOp = GeometryOps::Trim(
        Outside, FTransform::Identity, OutsideTool, FTransform::Identity, GeometryOps::FTrimParams());

    TestTrue(TEXT("default trim succeeds"), OutsideOp.bSuccess);
    TestTrue(TEXT("default trim changes the mesh"), OutsideOp.bChanged);

    // The other half of the same discrimination: the default keeps what the tool does NOT cover.
    const UE::Geometry::FAxisAlignedBox3d OutsideBounds = GeometryOpsBooleanTest_Bounds(Outside);
    TestTrue(TEXT("default trim keeps only the non-overlap"), OutsideBounds.Width() < 150.0);
    TestTrue(TEXT("default trim keeps the half OUTSIDE the tool"), OutsideBounds.Max.X < 25.0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanTrimDisjointTest,
    "PinWright.Geometry.Ops.Boolean.DisjointTrimSucceedsWithoutChanging",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanTrimDisjointTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Target = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 100.0);
    UDynamicMesh* Tool = GeometryOpsBooleanTest_NewBox(FVector(1000.0, 0.0, 0.0), 100.0);

    const GeometryOps::FOpResult Op = GeometryOps::Trim(
        Target, FTransform::Identity, Tool, FTransform::Identity, GeometryOps::FTrimParams());

    // A disjoint trim is not an engine failure: ApplyMeshBoolean returns the target untouched
    // rather than null, so the op succeeds and bChanged is the only honest signal it has. Trim
    // now captures the engine's return and can report BOOLEAN_FAILED, which this case must not
    // trip - the RPC verb still reports success unconditionally either way.
    TestTrue(TEXT("disjoint trim reports success"), Op.bSuccess);
    TestEqual(TEXT("disjoint trim is not an engine failure"), Op.ErrorCode, FString());
    TestFalse(TEXT("disjoint trim reports bChanged == false"), Op.bChanged);

    return true;
}

// ============================================================================
// SelfUnion
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanSelfUnionTest,
    "PinWright.Geometry.Ops.Boolean.SelfUnionResolvesOverlappingShells",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanSelfUnionTest::RunTest(const FString& Parameters)
{
    // Two boxes appended into one mesh interpenetrate: 24 triangles of two separate closed
    // shells with no shared topology, which is exactly the self-intersection the verb exists for.
    UDynamicMesh* Mesh = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 200.0);
    GeometryOpsBooleanTest_AppendBox(Mesh, FVector(100.0, 0.0, 0.0), 200.0);

    const int32 TrisBefore = Mesh->GetTriangleCount();
    TestEqual(TEXT("fixture is two un-unioned boxes"), TrisBefore, 24);

    const GeometryOps::FOpResult Op = GeometryOps::SelfUnion(Mesh, GeometryOps::FSelfUnionParams());

    TestTrue(TEXT("self union succeeds"), Op.bSuccess);
    TestEqual(TEXT("trianglesBefore is the pre-op count"), Op.TrianglesBefore, TrisBefore);
    TestTrue(TEXT("self union reports a change"), Op.bChanged);
    TestTrue(TEXT("self union retriangulates the overlap"), Op.TrianglesAfter != Op.TrianglesBefore);

    return true;
}

// ============================================================================
// Mirror
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanMirrorTest,
    "PinWright.Geometry.Ops.Boolean.MirrorAppendsTheReflectedHalf",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanMirrorTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = GeometryOpsBooleanTest_NewBox(FVector(200.0, 0.0, 0.0), 100.0);
    const int32 TrisBefore = Mesh->GetTriangleCount();

    GeometryOps::FMirrorParams Params;
    Params.Axis = GeometryOps::EMeshAxis::X;
    Params.bWeld = false;

    const GeometryOps::FOpResult Op = GeometryOps::Mirror(Mesh, Params);

    TestTrue(TEXT("mirror succeeds"), Op.bSuccess);
    TestTrue(TEXT("mirror reports a change"), Op.bChanged);
    // Mirror-AND-MERGE: the source half is never removed, so the output carries both.
    TestEqual(TEXT("mirror doubles the triangle count"), Op.TrianglesAfter, TrisBefore * 2);

    const UE::Geometry::FAxisAlignedBox3d Bounds = GeometryOpsBooleanTest_Bounds(Mesh);
    TestTrue(TEXT("mirrored half lands on the far side of X=0"), Bounds.Min.X < -240.0);
    TestTrue(TEXT("source half is retained"), Bounds.Max.X > 240.0);

    // A second axis, because X alone exercises one arm of the switch in
    // GeometryOpsBoolean_MirrorScaleForAxis: swap its Y and Z cases, or let either fall through
    // to the unit-scale default, and every assertion above still passes. Same 100-box, offset on
    // Y this time, so the reflected half has to land at negative Y.
    UDynamicMesh* YMesh = GeometryOpsBooleanTest_NewBox(FVector(0.0, 200.0, 0.0), 100.0);

    GeometryOps::FMirrorParams YParams;
    YParams.Axis = GeometryOps::EMeshAxis::Y;
    YParams.bWeld = false;

    const GeometryOps::FOpResult YOp = GeometryOps::Mirror(YMesh, YParams);
    TestTrue(TEXT("mirror on Y succeeds"), YOp.bSuccess);

    const UE::Geometry::FAxisAlignedBox3d YBounds = GeometryOpsBooleanTest_Bounds(YMesh);
    TestTrue(TEXT("the Y clone lands on the far side of Y=0"), YBounds.Min.Y < -240.0);
    TestTrue(TEXT("the Y source half is retained"), YBounds.Max.Y > 240.0);
    // X is the axis that must NOT have moved. The box is symmetric about X, so an arm that
    // negated X instead would leave the mesh sitting entirely at positive Y - which the two
    // assertions above already catch - while this one pins the axis that stayed put.
    TestTrue(TEXT("mirror on Y leaves the X extent alone"),
        YBounds.Min.X > -55.0 && YBounds.Max.X < 55.0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanMirrorUnknownAxisTest,
    "PinWright.Geometry.Ops.Boolean.MirrorUnknownAxisDoublesInPlace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanMirrorUnknownAxisTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = GeometryOpsBooleanTest_NewBox(FVector(200.0, 0.0, 0.0), 100.0);
    const int32 TrisBefore = Mesh->GetTriangleCount();
    const UE::Geometry::FAxisAlignedBox3d BoundsBefore = GeometryOpsBooleanTest_Bounds(Mesh);

    GeometryOps::FMirrorParams Params;
    Params.Axis = GeometryOps::EMeshAxis::None;
    Params.bWeld = false;

    const GeometryOps::FOpResult Op = GeometryOps::Mirror(Mesh, Params);

    // geometry.mirror does not validate its `axis` string. An unrecognized value has always
    // fallen through to a unit scale, so the clone is appended UNMIRRORED and the verb silently
    // doubles the mesh where it stands. None reproduces that rather than rejecting the input.
    TestTrue(TEXT("unknown axis still succeeds"), Op.bSuccess);
    TestEqual(TEXT("unknown axis doubles the triangle count"), Op.TrianglesAfter, TrisBefore * 2);

    const UE::Geometry::FAxisAlignedBox3d BoundsAfter = GeometryOpsBooleanTest_Bounds(Mesh);
    TestTrue(TEXT("unknown axis leaves the bounds where they were"),
        BoundsAfter.Min.Equals(BoundsBefore.Min, 0.001) && BoundsAfter.Max.Equals(BoundsBefore.Max, 0.001));

    return true;
}

// ============================================================================
// Array count validation
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanArrayCountTest,
    "PinWright.Geometry.Ops.Boolean.ValidateArrayCountBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanArrayCountTest::RunTest(const FString& Parameters)
{
    const GeometryOps::FOpResult TooLow = GeometryOps::ValidateArrayCount(0);
    TestFalse(TEXT("count 0 is rejected"), TooLow.bSuccess);
    TestEqual(TEXT("count 0 error code"), TooLow.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    TestEqual(TEXT("count 0 error message"), TooLow.ErrorMessage,
        FString(TEXT("count must be between 1 and 100")));

    const GeometryOps::FOpResult TooHigh = GeometryOps::ValidateArrayCount(101);
    TestFalse(TEXT("count 101 is rejected"), TooHigh.bSuccess);
    TestEqual(TEXT("count 101 error code"), TooHigh.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    TestTrue(TEXT("count 1 is accepted"), GeometryOps::ValidateArrayCount(1).bSuccess);
    TestTrue(TEXT("count 100 is accepted"), GeometryOps::ValidateArrayCount(100).bSuccess);

    return true;
}

// ============================================================================
// Arrays
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanArrayLinearTest,
    "PinWright.Geometry.Ops.Boolean.ArrayLinearMergesCopiesInPlace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanArrayLinearTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 100.0);
    const int32 TrisBefore = Mesh->GetTriangleCount();

    GeometryOps::FArrayLinearParams Params;
    Params.Count = 3;
    Params.Offset = FVector(300.0, 0.0, 0.0);

    const GeometryOps::FOpResult Op = GeometryOps::ArrayLinear(Mesh, Params);

    TestTrue(TEXT("array_linear succeeds"), Op.bSuccess);
    TestTrue(TEXT("array_linear reports a change"), Op.bChanged);
    // Count includes the original, so Count-1 copies are appended.
    TestEqual(TEXT("array_linear multiplies the triangle count by count"),
        Op.TrianglesAfter, TrisBefore * 3);

    // The row runs offset..(count-1)*offset with the original left at zero. A first copy placed
    // at the identity would double the original and leave the row one spacing short.
    const UE::Geometry::FAxisAlignedBox3d Bounds = GeometryOpsBooleanTest_Bounds(Mesh);
    TestTrue(TEXT("original stays at the origin"), Bounds.Min.X < -49.0 && Bounds.Min.X > -51.0);
    TestTrue(TEXT("last copy sits at (count-1) * offset"), Bounds.Max.X > 649.0 && Bounds.Max.X < 651.0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanArrayRadialTest,
    "PinWright.Geometry.Ops.Boolean.ArrayRadialMergesCopiesInPlace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanArrayRadialTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = GeometryOpsBooleanTest_NewBox(FVector(300.0, 0.0, 0.0), 100.0);
    const int32 TrisBefore = Mesh->GetTriangleCount();

    GeometryOps::FArrayRadialParams Params;
    Params.Count = 4;
    Params.Center = FVector::ZeroVector;
    Params.Axis = GeometryOps::EMeshAxis::Z;
    Params.TotalAngleDegrees = 360.0;

    const GeometryOps::FOpResult Op = GeometryOps::ArrayRadial(Mesh, Params);

    TestTrue(TEXT("array_radial succeeds"), Op.bSuccess);
    TestTrue(TEXT("array_radial reports a change"), Op.bChanged);
    TestEqual(TEXT("array_radial multiplies the triangle count by count"),
        Op.TrianglesAfter, TrisBefore * 4);

    // 360 swept across all 4 copies is a 90-degree step, so the ring is symmetric about the
    // origin on both X and Y. A step of TotalAngle/(Count-1) would put a duplicate at 360.
    const UE::Geometry::FAxisAlignedBox3d Bounds = GeometryOpsBooleanTest_Bounds(Mesh);
    TestTrue(TEXT("ring spans both X directions"), Bounds.Min.X < -340.0 && Bounds.Max.X > 340.0);
    TestTrue(TEXT("ring spans both Y directions"), Bounds.Min.Y < -340.0 && Bounds.Max.Y > 340.0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanArrayGuardTest,
    "PinWright.Geometry.Ops.Boolean.ArrayPolygonGuardLeavesTheMeshUntouched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanArrayGuardTest::RunTest(const FString& Parameters)
{
    // 24 steps per axis subdivides each face into a 25x25 grid: 12 * 25^2 = 7500 triangles, so
    // 100 copies estimate at 750k against the 500k ceiling.
    UDynamicMesh* Mesh = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 100.0, 24);
    const int32 TrisBefore = Mesh->GetTriangleCount();

    GeometryOps::FArrayLinearParams Params;
    Params.Count = 100;
    Params.Offset = FVector(300.0, 0.0, 0.0);

    TestTrue(TEXT("fixture is dense enough to trip the guard"),
        static_cast<int64>(TrisBefore) * Params.Count > GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH);

    const GeometryOps::FOpResult Op = GeometryOps::ArrayLinear(Mesh, Params);

    TestFalse(TEXT("over-budget array is rejected"), Op.bSuccess);
    TestEqual(TEXT("over-budget array error code"), Op.ErrorCode,
        FString(ErrorCodes::ERR_POLYGON_LIMIT_EXCEEDED));
    TestTrue(TEXT("the message names the estimate"), Op.ErrorMessage.Contains(TEXT("Estimated")));

    // The guard runs before either mesh is touched, so a caller that trips it keeps exactly the
    // geometry it had.
    TestEqual(TEXT("rejected array leaves the mesh alone"), Mesh->GetTriangleCount(), TrisBefore);
    TestEqual(TEXT("rejected array still reports the pre-op count"), Op.TrianglesBefore, TrisBefore);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanArrayCountGuardOrderTest,
    "PinWright.Geometry.Ops.Boolean.ArrayOpsValidateCountThemselves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanArrayCountGuardOrderTest::RunTest(const FString& Parameters)
{
    // The RPC wrappers call ValidateArrayCount before resolving the actor, so a bad count and a
    // missing actor together keep reporting INVALID_ARGUMENT. A front-end that resolves nothing
    // is covered because the ops re-run the same check themselves.
    UDynamicMesh* Linear = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 100.0);
    const int32 LinearTrisBefore = Linear->GetTriangleCount();

    GeometryOps::FArrayLinearParams LinearParams;
    LinearParams.Count = 0;

    const GeometryOps::FOpResult LinearOp = GeometryOps::ArrayLinear(Linear, LinearParams);
    TestFalse(TEXT("array_linear rejects count 0"), LinearOp.bSuccess);
    TestEqual(TEXT("array_linear count error code"), LinearOp.ErrorCode,
        FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    TestEqual(TEXT("array_linear leaves the mesh alone"), Linear->GetTriangleCount(), LinearTrisBefore);

    UDynamicMesh* Radial = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 100.0);
    const int32 RadialTrisBefore = Radial->GetTriangleCount();

    GeometryOps::FArrayRadialParams RadialParams;
    RadialParams.Count = 101;

    const GeometryOps::FOpResult RadialOp = GeometryOps::ArrayRadial(Radial, RadialParams);
    TestFalse(TEXT("array_radial rejects count 101"), RadialOp.bSuccess);
    TestEqual(TEXT("array_radial count error code"), RadialOp.ErrorCode,
        FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    TestEqual(TEXT("array_radial leaves the mesh alone"), Radial->GetTriangleCount(), RadialTrisBefore);

    return true;
}

// ============================================================================
// Null-mesh contract
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanNullMeshTest,
    "PinWright.Geometry.Ops.Boolean.NullMeshIsRejectedNotDereferenced",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanNullMeshTest::RunTest(const FString& Parameters)
{
    // MESH_NOT_FOUND / "DynamicMesh not available" is the family-wide contract from
    // GeometryOps::NullMeshFailure, the same pair GeometryTarget sends for a resolved actor
    // carrying no mesh. No RPC wrapper can reach it, so these assertions guard the compiler
    // front-end rather than the wire: before the guards existed, every one of these calls
    // dereferenced the null handle inside an engine call.
    const FString ExpectedCode(ErrorCodes::ERR_MESH_NOT_FOUND);
    const FString ExpectedMessage(TEXT("DynamicMesh not available"));

    UDynamicMesh* Real = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 100.0);

    const GeometryOps::FOpResult NullTarget = GeometryOps::Boolean(
        nullptr, FTransform::Identity, Real, FTransform::Identity,
        EGeometryScriptBooleanOperation::Subtract, GeometryOps::FBooleanParams());
    TestFalse(TEXT("boolean with a null target fails"), NullTarget.bSuccess);
    TestEqual(TEXT("boolean null-target code"), NullTarget.ErrorCode, ExpectedCode);
    TestEqual(TEXT("boolean null-target message"), NullTarget.ErrorMessage, ExpectedMessage);

    // The tool handle is guarded by the same check: the boolean reads its triangle count before
    // either guard runs, so a null tool used to crash one line into the op.
    const GeometryOps::FOpResult NullTool = GeometryOps::Boolean(
        Real, FTransform::Identity, nullptr, FTransform::Identity,
        EGeometryScriptBooleanOperation::Subtract, GeometryOps::FBooleanParams());
    TestFalse(TEXT("boolean with a null tool fails"), NullTool.bSuccess);
    TestEqual(TEXT("boolean null-tool code"), NullTool.ErrorCode, ExpectedCode);

    const GeometryOps::FOpResult NullTrim = GeometryOps::Trim(
        nullptr, FTransform::Identity, Real, FTransform::Identity, GeometryOps::FTrimParams());
    TestFalse(TEXT("trim with a null target fails"), NullTrim.bSuccess);
    TestEqual(TEXT("trim null-mesh code"), NullTrim.ErrorCode, ExpectedCode);

    const GeometryOps::FOpResult NullSelfUnion =
        GeometryOps::SelfUnion(nullptr, GeometryOps::FSelfUnionParams());
    TestFalse(TEXT("self_union with a null mesh fails"), NullSelfUnion.bSuccess);
    TestEqual(TEXT("self_union null-mesh code"), NullSelfUnion.ErrorCode, ExpectedCode);

    const GeometryOps::FOpResult NullMirror =
        GeometryOps::Mirror(nullptr, GeometryOps::FMirrorParams());
    TestFalse(TEXT("mirror with a null mesh fails"), NullMirror.bSuccess);
    TestEqual(TEXT("mirror null-mesh code"), NullMirror.ErrorCode, ExpectedCode);

    const GeometryOps::FOpResult NullLinear =
        GeometryOps::ArrayLinear(nullptr, GeometryOps::FArrayLinearParams());
    TestFalse(TEXT("array_linear with a null mesh fails"), NullLinear.bSuccess);
    TestEqual(TEXT("array_linear null-mesh code"), NullLinear.ErrorCode, ExpectedCode);

    const GeometryOps::FOpResult NullRadial =
        GeometryOps::ArrayRadial(nullptr, GeometryOps::FArrayRadialParams());
    TestFalse(TEXT("array_radial with a null mesh fails"), NullRadial.bSuccess);
    TestEqual(TEXT("array_radial null-mesh code"), NullRadial.ErrorCode, ExpectedCode);

    // Order, not just presence: the count check runs before the mesh guard in both array ops,
    // because the RPC wrappers run the same check before they resolve the actor. Swap the two
    // and a bad count starts reporting MESH_NOT_FOUND on a front-end that resolves nothing.
    GeometryOps::FArrayLinearParams BadCount;
    BadCount.Count = 0;
    const GeometryOps::FOpResult CountBeatsMesh = GeometryOps::ArrayLinear(nullptr, BadCount);
    TestFalse(TEXT("null mesh plus bad count fails"), CountBeatsMesh.bSuccess);
    TestEqual(TEXT("count is checked before the mesh handle"), CountBeatsMesh.ErrorCode,
        FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    return true;
}

// ============================================================================
// bSimplifyOutput default, and the mesh-health walk it is judged by
// ============================================================================
//
// FBooleanParams::bSimplifyOutput was pinned FALSE and no front-end published it, so the
// engine's own simplification pass was unreachable from every boolean this plugin exposes.
// Measured cost: union { cylinder r=25.5 h=4 segments=32; array_linear count=16 } onto a
// 32-segment 300-tall cylinder produced 135,324 triangles against 2,608 appended - 52x.
//
// Two assertions, because the default and the knob fail independently. Pin the default back to
// false and the first fails; drop Params.bSimplifyOutput on the floor inside Boolean() and the
// second fails while the first still passes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsBooleanSimplifyDefaultTest,
    "PinWright.Geometry.Ops.Boolean.SimplifyOutputDefaultsOnAndIsReachable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsBooleanSimplifyDefaultTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("the boolean default is the engine's true, not the old false pin"),
        GeometryOps::FBooleanParams().bSimplifyOutput);
    TestTrue(TEXT("trim already ran the engine default and still does"),
        GeometryOps::FTrimParams().bSimplifyOutput);

    // Two overlapping boxes, and the TARGET IS SUBDIVIDED - the grid is the whole fixture.
    //
    // bSimplifyOutput becomes FMeshBoolean::bSimplifyAlongNewEdges, and SimplifyAlongNewEdges
    // (MeshBoolean.cpp:796) walks ONLY the cut-boundary edges, collapsing a vertex only when
    // FLocalPlanarSimplify::IsFlat finds its entire one-ring coplanar. Two PLAIN axis-aligned
    // boxes give it no such vertex: the intersection curve on each cut face runs through that
    // face's own diagonal vertex, so the retriangulation is ALREADY the minimal four triangles
    // per L-hexagon, and every cut-boundary vertex sits on a box edge where the one-ring spans
    // two faces. Measured: 36 triangles with the flag on and 36 with it off - the fixture was
    // reporting the box, not the flag.
    //
    // Steps is AppendBox's EdgeVertices, i.e. the number of VERTICES along an edge
    // (MeshPrimitiveFunctions.cpp:236), so 4 puts grid lines at -100/-33.3/+33.3/+100 on a
    // 200 box while the cut planes sit at 0. The cut therefore crosses grid edges at points
    // INTERIOR to a flat face: coplanar one-ring, straight cut boundary, per-face affine UVs and
    // constant per-face normals, which is every condition CollapseWouldChangeShapeOrUVs asks for.
    auto UnionTriangles = [](bool bSimplify) -> int32
    {
        UDynamicMesh* Target = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 200.0, 4);
        UDynamicMesh* Tool = GeometryOpsBooleanTest_NewBox(FVector(100.0, 100.0, 100.0), 200.0);

        GeometryOps::FBooleanParams Params;
        Params.bSimplifyOutput = bSimplify;
        const GeometryOps::FOpResult Op = GeometryOps::Boolean(
            Target, FTransform::Identity, Tool, FTransform::Identity,
            EGeometryScriptBooleanOperation::Union, Params);
        return Op.bSuccess ? Op.TrianglesAfter : -1;
    };

    const int32 Simplified = UnionTriangles(true);
    const int32 Unsimplified = UnionTriangles(false);

    TestTrue(TEXT("both unions succeed"), Simplified > 0 && Unsimplified > 0);

    // <=, not <. The direction is the contract - simplification can never ADD triangles - and it
    // is what a dropped Params.bSimplifyOutput breaks, since both calls would then return the
    // same number and the strict form below reports which way it went.
    TestTrue(*FString::Printf(
            TEXT("simplify_output=true never yields more triangles (%d simplified vs %d not)"),
            Simplified, Unsimplified),
        Simplified <= Unsimplified);
    TestTrue(*FString::Printf(
            TEXT("and on a subdivided coplanar cut it yields fewer (%d simplified vs %d not) - ")
            TEXT("equal here means the flag stopped reaching ApplyMeshBoolean"),
            Simplified, Unsimplified),
        Simplified < Unsimplified);

    return true;
}

// GeometryUtils::MeasureMeshHealth is the walk geometry.check_health reports and the .pwmodel
// compiler's final gate now runs. It was inline in MeshMeasureHandler.cpp, so the compiler had
// no way to ask and shipped open shells as successful assets; a second copy would let the verb
// and the compiler answer differently about the same mesh.
//
// The two fixtures are the two answers that must not collapse into each other: a box is a closed
// solid, and a single appended triangle is an open shell with three boundary edges.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsMeshHealthTest,
    "PinWright.Geometry.Ops.Boolean.MeshHealthSeparatesClosedFromOpen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsMeshHealthTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Box = GeometryOpsBooleanTest_NewBox(FVector::ZeroVector, 100.0);
    const GeometryUtils::FMeshHealth Closed = GeometryUtils::MeasureMeshHealth(Box);

    TestEqual(TEXT("a box has no boundary edges"), Closed.BoundaryEdges, 0);
    TestTrue(TEXT("and reads as closed"), Closed.IsClosed());
    TestTrue(TEXT("and as healthy"), Closed.IsHealthy());
    TestEqual(TEXT("and is one connected component"), Closed.ComponentCount, 1);
    TestEqual(TEXT("and its triangle count matches the handle's"),
        Closed.TriangleCount, Box->GetTriangleCount());

    UDynamicMesh* Open = GeometryOpsBooleanTest_NewMesh();
    Open->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        EditMesh.AppendVertex(FVector3d(0, 0, 0));
        EditMesh.AppendVertex(FVector3d(100, 0, 0));
        EditMesh.AppendVertex(FVector3d(0, 100, 0));
        EditMesh.AppendTriangle(0, 1, 2);
    }, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

    const GeometryUtils::FMeshHealth Shell = GeometryUtils::MeasureMeshHealth(Open);
    TestEqual(TEXT("one loose triangle has three boundary edges"), Shell.BoundaryEdges, 3);
    TestFalse(TEXT("so it is not closed"), Shell.IsClosed());
    TestFalse(TEXT("and not healthy"), Shell.IsHealthy());

    // Null-safe, and all-zero rather than a crash: the compiler calls this on a merged mesh it
    // has already proven non-empty, but the RPC verb's mesh comes from an actor resolve.
    const GeometryUtils::FMeshHealth None = GeometryUtils::MeasureMeshHealth(nullptr);
    TestEqual(TEXT("a null mesh measures zero triangles"), None.TriangleCount, 0);

    return true;
}

