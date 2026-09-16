// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for GeometryOps::Generate* - the 16 primitive generators extracted out of
// PrimitiveHandler.cpp so they can be called with no actor, no editor world, and no
// FHandlerContext in scope.
//
// These test the ops DIRECTLY, on transient meshes. The dispatcher-level geometry.create_*
// tests are untouched and remain the regression net for the RPC contract; this file covers the
// three things only the pure layer can express, each of which reverting would silently break:
//
//   1. LocalTransform bakes into the vertices. The RPC wrappers all pass FTransform::Identity
//      and keep the placement on the actor, so no dispatcher test can observe this parameter at
//      all - yet it is the whole reason the .pwmodel compiler can place a primitive without
//      spawning an actor. Wire it to Identity inside the op and BoxHonorsLocalTransform fails.
//   2. Clamping reports itself. GeometryUtils::ClampDimension / ClampSegments used to alter a
//      caller's value in silence; the ops now append an FOpResult warning and write the
//      effective value back into the params struct, which is what lets geometry.create_box keep
//      echoing the clamped dimension without a second copy of the clamp beside the echo.
//   3. Generators APPEND. A .pwmodel part is a sequence of generators merging into one mesh, so
//      an op that replaced the mesh instead of adding to it would compile every multi-generator
//      part down to its last shape.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Primitives.h"
#include "Handlers/Geometry/GeometryUtils.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"

namespace
{
    // Prefixed because Unity merges this TU with the sibling geometry test files.
    UDynamicMesh* PrimOpsTestMesh()
    {
        return GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());
    }

    // Both clamp helpers open with "<label> clamped from <passed> to <effective>" (ClampRangeWarn
    // appends the range after it), so this one substring is the assertion that matters: a warning
    // that names only the value that ran leaves the author unable to find the typo that produced
    // it, which is the failure the whole clamp-reports-itself rule exists to prevent.
    bool PrimOpsWarnsClamp(const TArray<FString>& Warnings, const TCHAR* Label, int32 From, int32 To)
    {
        const FString Needle = FString::Printf(TEXT("%s clamped from %d to %d"), Label, From, To);
        for (const FString& Warning : Warnings)
        {
            if (Warning.Contains(Needle)) return true;
        }
        return false;
    }

    // Label-only variant for ClampDimensionWarn, whose two numbers go through
    // FString::SanitizeFloat and so are not worth spelling out in an assertion. The question it
    // answers is the one the labels were wrong about: does some warning name THIS parameter.
    bool PrimOpsWarnsClampFloat(const TArray<FString>& Warnings, const TCHAR* Label)
    {
        const FString Needle = FString::Printf(TEXT("%s clamped from "), Label);
        for (const FString& Warning : Warnings)
        {
            if (Warning.StartsWith(Needle)) return true;
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesBoxTransformTest,
    "PinWright.Geometry.Ops.Primitives.BoxHonorsLocalTransform",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesBoxTransformTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* AtOrigin = PrimOpsTestMesh();
    GeometryOps::FBoxParams OriginParams;
    OriginParams.Size = FVector(80.0, 50.0, 40.0);
    const GeometryOps::FOpResult OriginOp =
        GeometryOps::GenerateBox(AtOrigin, OriginParams, FTransform::Identity);

    TestTrue(TEXT("identity box succeeds"), OriginOp.bSuccess);
    TestTrue(TEXT("identity box adds triangles"), OriginOp.TrianglesAfter > 0);
    TestTrue(TEXT("identity box reports a change"), OriginOp.bChanged);

    const FVector OriginCenter = AtOrigin->GetMeshRef().GetBounds().Center();
    TestTrue(TEXT("identity box is centered on the origin"), OriginCenter.Size() < 1.0);

    // Position is not size. An AppendBox call that dropped Params.Size and built the engine's
    // own cube centres on the origin just as well, and the triangle count does not move either -
    // so nothing else in this file would notice. Dimension is the FULL extent (the engine builds
    // its FOrientedBox from Dimension * 0.5 extents, MeshPrimitiveFunctions.cpp), which is the
    // same reading PipeLeavesExistingGeometryIntact relies on when it finds a Size-200 box's
    // corners at +/-100.
    TestTrue(TEXT("the box measures the requested size on every axis"),
        FVector(AtOrigin->GetMeshRef().GetBounds().Diagonal())
            .Equals(FVector(80.0, 50.0, 40.0), 0.001));

    UDynamicMesh* Offset = PrimOpsTestMesh();
    GeometryOps::FBoxParams OffsetParams;
    OffsetParams.Size = FVector(80.0, 50.0, 40.0);
    const FVector RequestedLocation(1000.0, -250.0, 75.0);
    const GeometryOps::FOpResult OffsetOp = GeometryOps::GenerateBox(
        Offset, OffsetParams, FTransform(FQuat::Identity, RequestedLocation, FVector::OneVector));

    TestTrue(TEXT("transformed box succeeds"), OffsetOp.bSuccess);

    const FVector OffsetCenter = Offset->GetMeshRef().GetBounds().Center();
    TestTrue(TEXT("transformed box centers on the requested location"),
        FVector::Dist(OffsetCenter, RequestedLocation) < 1.0);

    // Same shape, different place: the transform must move the vertices, not resize them. The
    // triangle count alone cannot say that - a scaled box has exactly as many triangles - so the
    // extent is measured too.
    TestEqual(TEXT("transform does not change the triangle count"),
        OffsetOp.TrianglesAfter, OriginOp.TrianglesAfter);
    TestTrue(TEXT("transform does not change the box's extent either"),
        FVector(Offset->GetMeshRef().GetBounds().Diagonal())
            .Equals(FVector(80.0, 50.0, 40.0), 0.001));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesBoxClampTest,
    "PinWright.Geometry.Ops.Primitives.BoxClampsAndWarns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesBoxClampTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = PrimOpsTestMesh();

    GeometryOps::FBoxParams Params;
    Params.Size = FVector(0.0, GEOM_MAX_DIMENSION * 10.0, 100.0);
    Params.Steps = FIntVector(0, GEOM_MAX_SEGMENTS * 4, 1);

    const GeometryOps::FOpResult Op = GeometryOps::GenerateBox(Mesh, Params, FTransform::Identity);
    TestTrue(TEXT("clamped box still succeeds"), Op.bSuccess);

    // Written back into the params so the RPC wrapper's width/height/depth echo is the value the
    // engine actually saw.
    TestEqual(TEXT("zero extent falls back to the default"), Params.Size.X, 100.0);
    TestEqual(TEXT("oversized extent clamps to the module limit"), Params.Size.Y, GEOM_MAX_DIMENSION);
    TestEqual(TEXT("in-range extent is untouched"), Params.Size.Z, 100.0);

    // 0 is HONOURED, not read as unset. AppendBox floors these at 0 itself, exactly as
    // AppendRectangleXY does for the plane, so the "<= 0 means unset" substitution that belongs
    // to the radial counts does not belong here - it turned a deliberate "no subdivision" into 1
    // and warned about a change it had not made (FGridBoxMeshGenerator raises the count to 2
    // regardless, so 0 and 1 build the identical 12-triangle box).
    TestEqual(TEXT("a deliberate zero segment count is honoured"), Params.Steps.X, 0);
    TestEqual(TEXT("oversized segment count clamps to the module limit"), Params.Steps.Y, GEOM_MAX_SEGMENTS);
    TestEqual(TEXT("in-range segment count is untouched"), Params.Steps.Z, 1);

    // One per altered value: width, height, heightSegments. Steps.X did not move, so nothing is
    // reported about it - a warning for an unchanged value is the noise this clamp used to make.
    TestEqual(TEXT("every clamp reports itself"), Op.Warnings.Num(), 3);

    // Labels are the RPC's published names. Grepping for the struct's field names - the old
    // `size.x` / `steps.x` - is what a caller could not do, because those strings appear nowhere
    // on the wire.
    TestTrue(TEXT("the extent clamp names the published width parameter"),
        PrimOpsWarnsClampFloat(Op.Warnings, TEXT("width")));
    TestTrue(TEXT("the segment clamp names the published heightSegments parameter"),
        PrimOpsWarnsClamp(Op.Warnings, TEXT("heightSegments"), GEOM_MAX_SEGMENTS * 4, GEOM_MAX_SEGMENTS));

    // A negative count is a typo, not a request for no subdivision, and it is the one value the
    // bare range clamp still has to move.
    {
        UDynamicMesh* NegativeMesh = PrimOpsTestMesh();
        GeometryOps::FBoxParams Negative;
        Negative.Steps = FIntVector(-3, 1, 1);
        const GeometryOps::FOpResult NegOp =
            GeometryOps::GenerateBox(NegativeMesh, Negative, FTransform::Identity);

        TestTrue(TEXT("a negative segment count still succeeds"), NegOp.bSuccess);
        TestEqual(TEXT("a negative segment count clamps up to the engine floor of 0"), Negative.Steps.X, 0);
        TestTrue(TEXT("and the clamp reports itself under the published name"),
            PrimOpsWarnsClamp(NegOp.Warnings, TEXT("widthSegments"), -3, 0));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesSphereClampTest,
    "PinWright.Geometry.Ops.Primitives.SphereClampsSubdivisions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesSphereClampTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = PrimOpsTestMesh();

    // Zero, not an oversized count: the upper clamp is covered on the box, where 256 segments
    // cost a few thousand triangles - on a box-topology sphere the same limit is 256 steps on
    // all three axes, which is ~786k triangles for one assertion.
    GeometryOps::FSphereParams Params;
    Params.Radius = 25.0;
    Params.Subdivisions = 0;

    const GeometryOps::FOpResult Op = GeometryOps::GenerateSphere(Mesh, Params, FTransform::Identity);

    TestTrue(TEXT("clamped sphere succeeds"), Op.bSuccess);
    TestEqual(TEXT("a zero subdivision count falls back to the verb's default"), Params.Subdivisions, 16);
    TestEqual(TEXT("the clamp reports itself"), Op.Warnings.Num(), 1);
    // Radius is deliberately NOT clamped here - geometry.create_sphere never clamped it, and the
    // extraction must not add a limit the RPC contract does not have.
    TestEqual(TEXT("radius is left alone"), Params.Radius, 25.0);

    return true;
}

// ============================================================================
// The round primitives' segment floors
// ============================================================================
//
// Every Append* generator floors its own counts and reports nothing, so `cylinder segments=-5`
// used to return 18 triangles, success, and zero diagnostics - the response echoed a value the
// geometry never saw. The floors are read out of the engine, NOT guessed: MeshPrimitiveFunctions
// .cpp:658-659 (cylinder), :695-696 (cone), :418-419 (capsule), :720 and :757 (torus),
// :1295 (disc/ring). They are not all 3, which is why each op is pinned separately below.
//
// Each test drives one value under the floor and one in-range value: a clamp with no silent
// counterpart proves the warning is emitted, and the in-range case proves it is not emitted for
// ordinary documents - a clamp that fires on the defaults would bury every real warning.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesCylinderClampTest,
    "PinWright.Geometry.Ops.Primitives.CylinderClampsSegments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesCylinderClampTest::RunTest(const FString& Parameters)
{
    // 1, not 0. Zero was already handled by the default substitution every segment count has;
    // 1 is the value that slipped through it - above zero, so ClampSegments passed it along
    // unchanged, and AppendCylinder turned it into 3 without a word.
    GeometryOps::FCylinderParams Low;
    Low.RadialSteps = 1;
    const GeometryOps::FOpResult LowOp =
        GeometryOps::GenerateCylinder(PrimOpsTestMesh(), Low, FTransform::Identity);
    TestTrue(TEXT("a below-floor cylinder still succeeds"), LowOp.bSuccess);
    TestEqual(TEXT("segments is raised to the engine floor of 3"), Low.RadialSteps, 3);
    TestTrue(TEXT("the warning names both the passed value and the clamped one"),
        PrimOpsWarnsClamp(LowOp.Warnings, TEXT("segments"), 1, 3));

    GeometryOps::FCylinderParams Negative;
    Negative.RadialSteps = -5;
    Negative.HeightSteps = -5;
    const GeometryOps::FOpResult NegOp =
        GeometryOps::GenerateCylinder(PrimOpsTestMesh(), Negative, FTransform::Identity);
    TestEqual(TEXT("a negative segment count falls back to the verb's default"), Negative.RadialSteps, 16);
    TestEqual(TEXT("a negative height step count falls back to the engine floor of 0"), Negative.HeightSteps, 0);
    TestTrue(TEXT("the negative segments clamp is reported"),
        PrimOpsWarnsClamp(NegOp.Warnings, TEXT("segments"), -5, 16));
    TestTrue(TEXT("the negative heightSteps clamp is reported"),
        PrimOpsWarnsClamp(NegOp.Warnings, TEXT("heightSteps"), -5, 0));

    GeometryOps::FCylinderParams InRange;
    const GeometryOps::FOpResult InOp =
        GeometryOps::GenerateCylinder(PrimOpsTestMesh(), InRange, FTransform::Identity);
    TestEqual(TEXT("the defaults are in range and warn about nothing"), InOp.Warnings.Num(), 0);
    TestEqual(TEXT("and are left exactly as they were"), InRange.RadialSteps, 16);
    TestEqual(TEXT("heightSteps too"), InRange.HeightSteps, 1);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesConeClampTest,
    "PinWright.Geometry.Ops.Primitives.ConeClampsSegments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesConeClampTest::RunTest(const FString& Parameters)
{
    GeometryOps::FConeParams Low;
    Low.RadialSteps = 2;
    Low.HeightSteps = -1;
    const GeometryOps::FOpResult LowOp =
        GeometryOps::GenerateCone(PrimOpsTestMesh(), Low, FTransform::Identity);
    TestTrue(TEXT("a below-floor cone still succeeds"), LowOp.bSuccess);
    TestEqual(TEXT("segments is raised to the engine floor of 3"), Low.RadialSteps, 3);
    TestEqual(TEXT("heightSteps is raised to the engine floor of 0"), Low.HeightSteps, 0);
    TestTrue(TEXT("the segments warning names both values"),
        PrimOpsWarnsClamp(LowOp.Warnings, TEXT("segments"), 2, 3));
    TestTrue(TEXT("the heightSteps warning names both values"),
        PrimOpsWarnsClamp(LowOp.Warnings, TEXT("heightSteps"), -1, 0));

    GeometryOps::FConeParams InRange;
    const GeometryOps::FOpResult InOp =
        GeometryOps::GenerateCone(PrimOpsTestMesh(), InRange, FTransform::Identity);
    TestEqual(TEXT("the defaults warn about nothing"), InOp.Warnings.Num(), 0);
    TestEqual(TEXT("and are left alone"), InRange.RadialSteps, 16);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesCapsuleClampTest,
    "PinWright.Geometry.Ops.Primitives.CapsuleClampsBothStepCounts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesCapsuleClampTest::RunTest(const FString& Parameters)
{
    // The two floors differ - 2 for the hemisphere arc, 3 for the radial ring - so a single
    // shared floor would be wrong in one direction for one of them.
    GeometryOps::FCapsuleParams Low;
    Low.HemisphereSteps = 1;
    Low.RadialSteps = 1;
    const GeometryOps::FOpResult LowOp =
        GeometryOps::GenerateCapsule(PrimOpsTestMesh(), Low, FTransform::Identity);
    TestTrue(TEXT("a below-floor capsule still succeeds"), LowOp.bSuccess);
    TestEqual(TEXT("hemisphereSteps floors at 2, not 3"), Low.HemisphereSteps, 2);
    TestEqual(TEXT("segments floors at 3"), Low.RadialSteps, 3);
    TestTrue(TEXT("the hemisphereSteps warning names both values"),
        PrimOpsWarnsClamp(LowOp.Warnings, TEXT("hemisphereSteps"), 1, 2));
    TestTrue(TEXT("the segments warning names both values"),
        PrimOpsWarnsClamp(LowOp.Warnings, TEXT("segments"), 1, 3));

    GeometryOps::FCapsuleParams InRange;
    const GeometryOps::FOpResult InOp =
        GeometryOps::GenerateCapsule(PrimOpsTestMesh(), InRange, FTransform::Identity);
    TestEqual(TEXT("the defaults warn about nothing"), InOp.Warnings.Num(), 0);
    TestEqual(TEXT("hemisphereSteps is left alone"), InRange.HemisphereSteps, 4);
    TestEqual(TEXT("segments is left alone"), InRange.RadialSteps, 16);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesTorusClampTest,
    "PinWright.Geometry.Ops.Primitives.TorusClampsItsTwoDifferentFloors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesTorusClampTest::RunTest(const FString& Parameters)
{
    // Both counts floor at 3, and NEITHER reaches it the same way - which is what this test
    // exists to catch, in either direction. minorSegments builds the profile circle, whose floor
    // of 3 is the engine's own. majorSegments drives FRevolvePlanarPolygonGenerator, whose floor
    // IS 2, raised to 3 here because a torus revolves a full 360 and a closed sweep at 2 steps
    // cannot close (measured: 16 triangles, 16 boundary edges, isClosed false). Collapsing the
    // two into one rule is still the mistake - `arch` below 360 takes the engine's 2, which
    // TestGeometryClosedRevolveFloor.cpp pins.
    GeometryOps::FTorusParams Low;
    Low.MajorSteps = 1;
    Low.MinorSteps = 1;
    const GeometryOps::FOpResult LowOp =
        GeometryOps::GenerateTorus(PrimOpsTestMesh(), Low, FTransform::Identity);
    TestTrue(TEXT("a below-floor torus still succeeds"), LowOp.bSuccess);
    TestEqual(TEXT("majorSegments floors at 3 on a closed revolution"), Low.MajorSteps, 3);
    TestEqual(TEXT("minorSegments floors at 3"), Low.MinorSteps, 3);
    TestTrue(TEXT("the majorSegments warning names both values"),
        PrimOpsWarnsClamp(LowOp.Warnings, TEXT("majorSegments"), 1, 3));
    TestTrue(TEXT("the minorSegments warning names both values"),
        PrimOpsWarnsClamp(LowOp.Warnings, TEXT("minorSegments"), 1, 3));

    GeometryOps::FTorusParams InRange;
    const GeometryOps::FOpResult InOp =
        GeometryOps::GenerateTorus(PrimOpsTestMesh(), InRange, FTransform::Identity);
    TestEqual(TEXT("the defaults warn about nothing"), InOp.Warnings.Num(), 0);
    TestEqual(TEXT("majorSegments is left alone"), InRange.MajorSteps, 16);
    TestEqual(TEXT("minorSegments is left alone"), InRange.MinorSteps, 8);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesDiscClampTest,
    "PinWright.Geometry.Ops.Primitives.DiscClampsSegments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesDiscClampTest::RunTest(const FString& Parameters)
{
    GeometryOps::FDiscParams Low;
    Low.AngleSteps = 2;
    const GeometryOps::FOpResult LowOp =
        GeometryOps::GenerateDisc(PrimOpsTestMesh(), Low, FTransform::Identity);
    TestTrue(TEXT("a below-floor disc still succeeds"), LowOp.bSuccess);
    TestEqual(TEXT("segments floors at 3"), Low.AngleSteps, 3);
    TestTrue(TEXT("the warning names both values"),
        PrimOpsWarnsClamp(LowOp.Warnings, TEXT("segments"), 2, 3));

    GeometryOps::FDiscParams InRange;
    const GeometryOps::FOpResult InOp =
        GeometryOps::GenerateDisc(PrimOpsTestMesh(), InRange, FTransform::Identity);
    TestEqual(TEXT("the default warns about nothing"), InOp.Warnings.Num(), 0);
    TestEqual(TEXT("and is left alone"), InRange.AngleSteps, 16);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesRingClampTest,
    "PinWright.Geometry.Ops.Primitives.RingClampsSegments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesRingClampTest::RunTest(const FString& Parameters)
{
    GeometryOps::FRingParams Low;
    Low.AngleSteps = 1;
    const GeometryOps::FOpResult LowOp =
        GeometryOps::GenerateRing(PrimOpsTestMesh(), Low, FTransform::Identity);
    TestTrue(TEXT("a below-floor ring still succeeds"), LowOp.bSuccess);
    TestEqual(TEXT("segments floors at 3"), Low.AngleSteps, 3);
    TestTrue(TEXT("the warning names both values"),
        PrimOpsWarnsClamp(LowOp.Warnings, TEXT("segments"), 1, 3));

    // The ring's default is 32, not the disc's 16, and the substitution must use the verb's own.
    GeometryOps::FRingParams Zero;
    Zero.AngleSteps = 0;
    const GeometryOps::FOpResult ZeroOp =
        GeometryOps::GenerateRing(PrimOpsTestMesh(), Zero, FTransform::Identity);
    TestEqual(TEXT("zero falls back to the ring's own default of 32"), Zero.AngleSteps, 32);
    TestTrue(TEXT("that substitution is reported too"),
        PrimOpsWarnsClamp(ZeroOp.Warnings, TEXT("segments"), 0, 32));

    GeometryOps::FRingParams InRange;
    const GeometryOps::FOpResult InOp =
        GeometryOps::GenerateRing(PrimOpsTestMesh(), InRange, FTransform::Identity);
    TestEqual(TEXT("the default warns about nothing"), InOp.Warnings.Num(), 0);
    TestEqual(TEXT("and is left alone"), InRange.AngleSteps, 32);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesArchClampTest,
    "PinWright.Geometry.Ops.Primitives.ArchClampsTheTorusFloors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesArchClampTest::RunTest(const FString& Parameters)
{
    // Arch reaches AppendTorus by a second door. Clamping the torus and leaving the arch open
    // would fix the report and not the defect.
    //
    // Angle stays at its 180 default here, so this is the PARTIAL sweep and majorSteps keeps the
    // engine's floor of 2 - a half-arch at 2 steps has a real span between its two sections and
    // is left as written. The same verb at angle 360 is a torus and floors at 3;
    // TestGeometryClosedRevolveFloor.cpp pins both halves of that fork.
    GeometryOps::FArchParams Low;
    Low.MajorSteps = 1;
    Low.MinorSteps = 2;
    const GeometryOps::FOpResult LowOp =
        GeometryOps::GenerateArch(PrimOpsTestMesh(), Low, FTransform::Identity);
    TestTrue(TEXT("a below-floor arch still succeeds"), LowOp.bSuccess);
    TestEqual(TEXT("majorSteps floors at 2"), Low.MajorSteps, 2);
    TestEqual(TEXT("minorSteps floors at 3"), Low.MinorSteps, 3);
    TestTrue(TEXT("the majorSteps warning names both values"),
        PrimOpsWarnsClamp(LowOp.Warnings, TEXT("majorSteps"), 1, 2));
    TestTrue(TEXT("the minorSteps warning names both values"),
        PrimOpsWarnsClamp(LowOp.Warnings, TEXT("minorSteps"), 2, 3));

    GeometryOps::FArchParams InRange;
    const GeometryOps::FOpResult InOp =
        GeometryOps::GenerateArch(PrimOpsTestMesh(), InRange, FTransform::Identity);
    TestEqual(TEXT("the defaults warn about nothing"), InOp.Warnings.Num(), 0);
    TestEqual(TEXT("majorSteps is left alone"), InRange.MajorSteps, 16);
    TestEqual(TEXT("minorSteps is left alone"), InRange.MinorSteps, 8);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesPipeClampTest,
    "PinWright.Geometry.Ops.Primitives.PipeClampsRadialSteps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesPipeClampTest::RunTest(const FString& Parameters)
{
    // Pipe is the second door onto AppendCylinder, and it opens that door TWICE - shell and bore
    // cutter. Clamping once, up front, is what keeps the pair coincident.
    GeometryOps::FPipeParams Low;
    Low.RadialSteps = 2;
    Low.HeightSteps = -3;
    const GeometryOps::FOpResult LowOp =
        GeometryOps::GeneratePipe(PrimOpsTestMesh(), Low, FTransform::Identity);
    TestTrue(TEXT("a below-floor pipe still succeeds"), LowOp.bSuccess);
    TestEqual(TEXT("radialSteps floors at 3"), Low.RadialSteps, 3);
    TestEqual(TEXT("heightSteps floors at 0"), Low.HeightSteps, 0);
    TestTrue(TEXT("the radialSteps warning names both values"),
        PrimOpsWarnsClamp(LowOp.Warnings, TEXT("radialSteps"), 2, 3));
    TestTrue(TEXT("the heightSteps warning names both values"),
        PrimOpsWarnsClamp(LowOp.Warnings, TEXT("heightSteps"), -3, 0));

    GeometryOps::FPipeParams InRange;
    const GeometryOps::FOpResult InOp =
        GeometryOps::GeneratePipe(PrimOpsTestMesh(), InRange, FTransform::Identity);
    TestEqual(TEXT("the defaults warn about nothing"), InOp.Warnings.Num(), 0);
    TestEqual(TEXT("radialSteps is left alone"), InRange.RadialSteps, 24);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesAppendTest,
    "PinWright.Geometry.Ops.Primitives.GeneratorsAppendToTheSameMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesAppendTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = PrimOpsTestMesh();

    GeometryOps::FBoxParams BoxParams;
    const GeometryOps::FOpResult First = GeometryOps::GenerateBox(Mesh, BoxParams, FTransform::Identity);

    GeometryOps::FCylinderParams CylinderParams;
    const GeometryOps::FOpResult Second = GeometryOps::GenerateCylinder(
        Mesh, CylinderParams, FTransform(FQuat::Identity, FVector(300.0, 0.0, 0.0), FVector::OneVector));

    TestEqual(TEXT("first op starts from an empty mesh"), First.TrianglesBefore, 0);
    TestTrue(TEXT("first op adds geometry"), First.TrianglesAfter > 0);
    TestEqual(TEXT("second op sees the first op's result"), Second.TrianglesBefore, First.TrianglesAfter);
    TestTrue(TEXT("second op adds on top rather than replacing"), Second.TrianglesAfter > Second.TrianglesBefore);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesRevolveProfileTest,
    "PinWright.Geometry.Ops.Primitives.RevolveSubstitutesDefaultProfile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesRevolveProfileTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = PrimOpsTestMesh();

    GeometryOps::FRevolveParams Params;
    Params.Profile.Add(FVector2D(10.0, 0.0));  // one point is not a path

    const GeometryOps::FOpResult Op = GeometryOps::GenerateRevolve(Mesh, Params, FTransform::Identity);

    TestTrue(TEXT("revolve succeeds on the substituted profile"), Op.bSuccess);
    TestEqual(TEXT("the default profile replaces the too-short one"), Params.Profile.Num(), 6);
    TestTrue(TEXT("the substitution reports itself"), Op.Warnings.Num() > 0);
    TestTrue(TEXT("revolve produces geometry"), Op.TrianglesAfter > 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesPipeHollowTest,
    "PinWright.Geometry.Ops.Primitives.PipeSubtractsItsBore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesPipeHollowTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = PrimOpsTestMesh();

    GeometryOps::FPipeParams Params;
    Params.OuterRadius = 50.0;
    Params.InnerRadius = 30.0;
    Params.Height = 100.0;

    const GeometryOps::FOpResult Op = GeometryOps::GeneratePipe(Mesh, Params, FTransform::Identity);
    TestTrue(TEXT("pipe succeeds"), Op.bSuccess);
    TestTrue(TEXT("pipe produces geometry"), Op.TrianglesAfter > 0);

    // The bore is what distinguishes a pipe from a cylinder: without it every vertex sits on the
    // outer radius and none on the inner one.
    int32 OnBore = 0;
    const UE::Geometry::FDynamicMesh3& Ref = Mesh->GetMeshRef();
    for (int32 VertexID : Ref.VertexIndicesItr())
    {
        const FVector3d P = Ref.GetVertex(VertexID);
        const double Radial = FMath::Sqrt(P.X * P.X + P.Y * P.Y);
        if (FMath::Abs(Radial - Params.InnerRadius) < 1.0)
        {
            ++OnBore;
        }
    }
    TestTrue(TEXT("vertices land on the inner bore"), OnBore >= 3);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesPipeClosedTest,
    "PinWright.Geometry.Ops.Primitives.PipeIsAClosedSolid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesPipeClosedTest::RunTest(const FString& Parameters)
{
    // The defect this pins: `pipe` used to be an UNCAPPED outer cylinder minus an inner one, so
    // nothing ever formed the annular end caps. It was two open walls - 4 triangles per radial
    // step where a closed annulus needs 8 - and it rendered see-through with no diagnostic.
    // PipeSubtractsItsBore cannot catch that: the bore existed the whole time, it was the CAPS
    // that were missing, and a count-and-radius assertion is blind to an open boundary.
    //
    // Two assertions, because either alone passes on a broken mesh. A closedness check alone
    // passes on a mesh that happens to close by welding the wrong things; a triangle count alone
    // passes on any mesh with the right number of triangles in the wrong topology.
    UDynamicMesh* Mesh = PrimOpsTestMesh();

    GeometryOps::FPipeParams Params;
    Params.OuterRadius = 95.0;
    Params.InnerRadius = 50.0;
    Params.Height = 60.0;
    Params.RadialSteps = 32;
    Params.HeightSteps = 0;

    const GeometryOps::FOpResult Op = GeometryOps::GeneratePipe(Mesh, Params, FTransform::Identity);
    TestTrue(TEXT("pipe succeeds"), Op.bSuccess);

    // Eight per radial step at heightSteps 0: outer wall 2, bore 2, top rim 2, bottom rim 2.
    // The measured figure on the broken op was exactly half this.
    TestEqual(TEXT("a heightSteps=0 pipe is 8 triangles per radial step"),
        Op.TrianglesAfter, 8 * Params.RadialSteps);

    TestTrue(TEXT("the pipe is a closed manifold - no boundary edges"),
        Mesh->GetMeshRef().IsClosed());

    // The general form. heightSteps subdivides BOTH walls and neither rim, so each extra loop
    // adds 4 triangles per radial step, not 2 - the reading that would be right for a cylinder
    // with one wall.
    UDynamicMesh* Stepped = PrimOpsTestMesh();
    GeometryOps::FPipeParams SteppedParams = Params;
    SteppedParams.HeightSteps = 3;
    const GeometryOps::FOpResult SteppedOp =
        GeometryOps::GeneratePipe(Stepped, SteppedParams, FTransform::Identity);
    TestTrue(TEXT("a subdivided pipe succeeds"), SteppedOp.bSuccess);
    TestEqual(TEXT("triangles are 2 * radialSteps * (2 * heightSteps + 4)"),
        SteppedOp.TrianglesAfter,
        2 * SteppedParams.RadialSteps * (2 * SteppedParams.HeightSteps + 4));
    TestTrue(TEXT("subdividing the walls does not open the solid"),
        Stepped->GetMeshRef().IsClosed());

    // FOUR polygroups, at every step count. The revolve generator this is built on ignores
    // FGeometryScriptPrimitiveOptions::PolygroupMode and groups PER QUAD, which is the layout
    // that makes `bevel` chamfer every interior quad boundary and return a grid of notches at ~3x
    // the triangle cost - the standing trap on torus and arch. The op regroups after the sweep so
    // the pipe is not in it; without that pass this count would be radialSteps * (heightSteps + 2).
    const UE::Geometry::FDynamicMesh3& Ref = Mesh->GetMeshRef();
    TSet<int32> Groups;
    for (const int32 TriangleID : Ref.TriangleIndicesItr())
    {
        Groups.Add(Ref.GetTriangleGroup(TriangleID));
    }
    TestEqual(TEXT("the pipe carries one polygroup per face: outer wall, bore, and both rims"),
        Groups.Num(), 4);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesPipeOriginTest,
    "PinWright.Geometry.Ops.Primitives.PipeIsCentredLikeEveryOtherPrimitive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesPipeOriginTest::RunTest(const FString& Parameters)
{
    // `pipe` was the family's ONE base-placed generator: height 400 spanned z 0..400 while
    // cylinder height 400 spanned -200..200, so every pipe sat half its height off from the
    // primitive it is otherwise interchangeable with. Nothing documented it and nothing tested
    // it, which is why it survived - so this asserts the span directly rather than trusting a
    // count, and asserts it AGAINST the cylinder so the two cannot drift apart again.
    UDynamicMesh* PipeMesh = PrimOpsTestMesh();
    GeometryOps::FPipeParams PipeParams;
    PipeParams.OuterRadius = 100.0;
    PipeParams.InnerRadius = 60.0;
    PipeParams.Height = 400.0;
    const GeometryOps::FOpResult PipeOp =
        GeometryOps::GeneratePipe(PipeMesh, PipeParams, FTransform::Identity);
    TestTrue(TEXT("pipe succeeds"), PipeOp.bSuccess);

    const UE::Geometry::FAxisAlignedBox3d PipeBounds = PipeMesh->GetMeshRef().GetBounds();
    TestTrue(TEXT("a height-400 pipe spans z -200..200"),
        FMath::IsNearlyEqual(PipeBounds.Min.Z, -200.0, 0.001)
        && FMath::IsNearlyEqual(PipeBounds.Max.Z, 200.0, 0.001));

    UDynamicMesh* CylinderMesh = PrimOpsTestMesh();
    GeometryOps::FCylinderParams CylinderParams;
    CylinderParams.Radius = 100.0;
    CylinderParams.Height = 400.0;
    const GeometryOps::FOpResult CylinderOp =
        GeometryOps::GenerateCylinder(CylinderMesh, CylinderParams, FTransform::Identity);
    TestTrue(TEXT("cylinder succeeds"), CylinderOp.bSuccess);

    const UE::Geometry::FAxisAlignedBox3d CylinderBounds = CylinderMesh->GetMeshRef().GetBounds();
    TestTrue(TEXT("a pipe and a cylinder of the same height occupy the same z span"),
        FMath::IsNearlyEqual(PipeBounds.Min.Z, CylinderBounds.Min.Z, 0.001)
        && FMath::IsNearlyEqual(PipeBounds.Max.Z, CylinderBounds.Max.Z, 0.001));

    // The transform still bakes in, same as every other generator - a centred pipe placed at
    // z=1000 spans 800..1200, and reading the centre back is what proves the origin moved with it.
    UDynamicMesh* Placed = PrimOpsTestMesh();
    GeometryOps::FPipeParams PlacedParams = PipeParams;
    const GeometryOps::FOpResult PlacedOp = GeometryOps::GeneratePipe(
        Placed, PlacedParams, FTransform(FQuat::Identity, FVector(0.0, 0.0, 1000.0), FVector::OneVector));
    TestTrue(TEXT("a placed pipe succeeds"), PlacedOp.bSuccess);
    TestTrue(TEXT("LocalTransform bakes into the vertices"),
        FMath::IsNearlyEqual(Placed->GetMeshRef().GetBounds().Center().Z, 1000.0, 0.001));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesPipeBoreRangeTest,
    "PinWright.Geometry.Ops.Primitives.PipeRefusesADegenerateBore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesPipeBoreRangeTest::RunTest(const FString& Parameters)
{
    // Refused, not clamped, and this is the one place in the family where that is right: every
    // other out-of-range value here has a defensible substitute (the engine's floor, the verb's
    // default), and a radius pair does not. The old boolean form accepted both of these and
    // returned an empty or single-walled mesh in silence.
    GeometryOps::FPipeParams Inverted;
    Inverted.OuterRadius = 40.0;
    Inverted.InnerRadius = 50.0;
    const GeometryOps::FOpResult InvertedOp =
        GeometryOps::GeneratePipe(PrimOpsTestMesh(), Inverted, FTransform::Identity);
    TestFalse(TEXT("a bore at or outside the wall fails"), InvertedOp.bSuccess);
    TestEqual(TEXT("it fails with INVALID_PARAMS"), InvertedOp.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    TestTrue(TEXT("the message names both radii"),
        InvertedOp.ErrorMessage.Contains(TEXT("innerRadius"))
        && InvertedOp.ErrorMessage.Contains(TEXT("outerRadius")));

    GeometryOps::FPipeParams NoBore;
    NoBore.OuterRadius = 50.0;
    NoBore.InnerRadius = 0.0;
    const GeometryOps::FOpResult NoBoreOp =
        GeometryOps::GeneratePipe(PrimOpsTestMesh(), NoBore, FTransform::Identity);
    TestFalse(TEXT("a zero bore fails rather than sweeping zero-area triangles onto the axis"),
        NoBoreOp.bSuccess);

    // The failure must not eat the mesh it was handed. A pipe that fails after a box has run into
    // the same mesh has to leave the box alone.
    UDynamicMesh* Mesh = PrimOpsTestMesh();
    GeometryOps::FBoxParams BoxParams;
    const GeometryOps::FOpResult Box = GeometryOps::GenerateBox(Mesh, BoxParams, FTransform::Identity);
    GeometryOps::FPipeParams Bad = Inverted;
    const GeometryOps::FOpResult BadOp = GeometryOps::GeneratePipe(Mesh, Bad, FTransform::Identity);
    TestFalse(TEXT("the failing pipe still fails on a non-empty mesh"), BadOp.bSuccess);
    TestEqual(TEXT("and leaves every triangle it was handed"),
        Mesh->GetTriangleCount(), Box.TrianglesAfter);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesPipeAppendsTest,
    "PinWright.Geometry.Ops.Primitives.PipeLeavesExistingGeometryIntact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesPipeAppendsTest::RunTest(const FString& Parameters)
{
    // Pipe is the one generator here that runs a BOOLEAN, and a boolean is not an append. It used
    // to bore its own target mesh, so anything already in that mesh was cut and rewelded by an op
    // whose whole family contract is "append" - exactly what a .pwmodel part reading
    // `box ... / pipe ...` produces. GeneratorsAppendToTheSameMesh cannot catch it: that test only
    // asserts the count grew, and a bored box still grows the count.

    // The pipe alone, so the append below has an exact expected size to compare against rather
    // than an inequality that a partially-destroyed box would still satisfy.
    UDynamicMesh* PipeOnly = PrimOpsTestMesh();
    GeometryOps::FPipeParams SoloParams;
    SoloParams.OuterRadius = 50.0;
    SoloParams.InnerRadius = 30.0;
    SoloParams.Height = 100.0;
    const GeometryOps::FOpResult Solo =
        GeometryOps::GeneratePipe(PipeOnly, SoloParams, FTransform::Identity);
    TestTrue(TEXT("the control pipe succeeds"), Solo.bSuccess);
    TestTrue(TEXT("the control pipe produces geometry"), Solo.TrianglesAfter > 0);

    // 200 uu on a side, centred on the origin, so the pipe (outer radius 50, spanning Z -50..50)
    // sits wholly inside it: anything that cut or rewelded the target rather than appending to it
    // lands on neither exact sum below.
    UDynamicMesh* Mesh = PrimOpsTestMesh();
    GeometryOps::FBoxParams BoxParams;
    BoxParams.Size = FVector(200.0, 200.0, 200.0);
    const GeometryOps::FOpResult Box = GeometryOps::GenerateBox(Mesh, BoxParams, FTransform::Identity);
    TestTrue(TEXT("the box succeeds"), Box.bSuccess);

    const int32 BoxTriangles = Box.TrianglesAfter;
    const int32 BoxVertices = Box.VerticesAfter;

    GeometryOps::FPipeParams Params = SoloParams;
    const GeometryOps::FOpResult Op = GeometryOps::GeneratePipe(Mesh, Params, FTransform::Identity);
    TestTrue(TEXT("pipe succeeds on a non-empty mesh"), Op.bSuccess);
    TestEqual(TEXT("pipe sees the box it is appended to"), Op.TrianglesBefore, BoxTriangles);

    // Exact sums, not ">": an append adds the control pipe's elements and touches nothing else.
    // Boring the target instead retriangulates the box around the cut, which lands on neither sum.
    TestEqual(TEXT("the box's triangles all survive and the pipe's are added on top"),
        Op.TrianglesAfter, BoxTriangles + Solo.TrianglesAfter);
    TestEqual(TEXT("the box's vertices all survive and the pipe's are added on top"),
        Op.VerticesAfter, BoxVertices + Solo.VerticesAfter);

    // The corners are the cheapest positional proof the box is still a box: the cut would have
    // moved or removed geometry, and a weld would have merged coincident corners away.
    int32 Corners = 0;
    const UE::Geometry::FDynamicMesh3& Ref = Mesh->GetMeshRef();
    for (int32 VertexID : Ref.VertexIndicesItr())
    {
        const FVector3d P = Ref.GetVertex(VertexID);
        if (FMath::IsNearlyEqual(FMath::Abs(P.X), 100.0, 0.001)
            && FMath::IsNearlyEqual(FMath::Abs(P.Y), 100.0, 0.001)
            && FMath::IsNearlyEqual(FMath::Abs(P.Z), 100.0, 0.001))
        {
            ++Corners;
        }
    }
    TestEqual(TEXT("all eight box corners are still where the box put them"), Corners, 8);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesEmptyTest,
    "PinWright.Geometry.Ops.Primitives.EmptyGeneratorAppendsNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesEmptyTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = PrimOpsTestMesh();

    GeometryOps::FEmptyMeshParams Params;
    const GeometryOps::FOpResult Op = GeometryOps::GenerateEmpty(Mesh, Params, FTransform::Identity);

    TestTrue(TEXT("the empty generator succeeds"), Op.bSuccess);
    TestEqual(TEXT("it appends no triangles"), Op.TrianglesAfter, 0);
    TestFalse(TEXT("it reports no change"), Op.bChanged);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsPrimitivesNullMeshTest,
    "PinWright.Geometry.Ops.Primitives.NullMeshFailsWithACode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOpsPrimitivesNullMeshTest::RunTest(const FString& Parameters)
{
    GeometryOps::FBoxParams Params;
    const GeometryOps::FOpResult Op = GeometryOps::GenerateBox(nullptr, Params, FTransform::Identity);

    TestFalse(TEXT("a null target mesh fails"), Op.bSuccess);
    TestEqual(TEXT("it fails with the handler's own mesh code"), Op.ErrorCode, FString(TEXT("MESH_NOT_FOUND")));
    TestFalse(TEXT("the failure carries a message"), Op.ErrorMessage.IsEmpty());

    return true;
}
