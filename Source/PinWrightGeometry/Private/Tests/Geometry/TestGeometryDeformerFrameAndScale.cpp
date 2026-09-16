// Copyright (c) 2026 Alexander Penkin. MIT License.

// Two deformer parameters that were hardcoded, and the compatibility promise each one owes.
//
//  1. FWarpFrameSpec on bend / twist / taper. The three ops passed FTransform::Identity as the
//     engine's gizmo frame, so the extent could only ever be measured along part-local +Z about
//     the part-local origin. The tests below pin three things: the default frame is BIT-IDENTICAL
//     to that hardcoded identity, `axis` really moves which axis the extent is measured along,
//     and `center` is exactly a translation of the deform (a mesh moved by T and deformed about
//     Center + T lands on the un-moved result moved by T).
//
//  2. ENoiseMagnitudeMode on noise_deform. Absolute must stay the engine call, byte for byte,
//     because every existing recipe took it. Relative is a local loop, so the loop's formula is
//     asserted EXACTLY rather than statistically: relative displacement equals absolute
//     displacement at magnitude 1, scaled per vertex by Magnitude * the vertex's mean one-ring
//     edge length. That identity holds vertex for vertex, which is what makes it a real pin -
//     comparing average displacements between two mesh regions would pass on any monotone
//     mistake.
//
// Everything runs against transient UDynamicMesh objects: no actor, no editor world, no
// FHandlerContext. Fixtures are generated primitives, never content.
#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOps_Modeling.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "Compat/EngineVersionCompat.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "GeometryScript/GeometryScriptSelectionTypes.h"
#include "GeometryScript/MeshDeformFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so a plainly-named
// anonymous-namespace helper collides with a sibling test TU once Unity merges them.

TArray<FVector> DeformerFrameTest_Positions(UDynamicMesh* Mesh)
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

// Largest per-vertex distance between two runs, or a negative sentinel when the two runs did not
// produce the same vertex set - which is a different defect from a different deform and must not
// read as one.
double DeformerFrameTest_MaxDelta(const TArray<FVector>& A, const TArray<FVector>& B)
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

// Same, after translating B back by Offset. The `center` test's whole assertion.
double DeformerFrameTest_MaxDeltaAfterUntranslating(
    const TArray<FVector>& A, const TArray<FVector>& B, const FVector& Offset)
{
    if (A.Num() != B.Num() || A.Num() == 0)
    {
        return -1.0;
    }
    double MaxDelta = 0.0;
    for (int32 Index = 0; Index < A.Num(); ++Index)
    {
        MaxDelta = FMath::Max(MaxDelta, FVector::Dist(A[Index], B[Index] - Offset));
    }
    return MaxDelta;
}

UDynamicMesh* DeformerFrameTest_NewBox(
    double SizeX, double SizeY, double SizeZ,
    int32 StepsX, int32 StepsY, int32 StepsZ,
    const FVector& Center = FVector::ZeroVector)
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform(Center),
        static_cast<float>(SizeX), static_cast<float>(SizeY), static_cast<float>(SizeZ),
        StepsX, StepsY, StepsZ,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

FVector DeformerFrameTest_BoundsExtent(const TArray<FVector>& Positions)
{
    if (Positions.Num() == 0)
    {
        return FVector::ZeroVector;
    }
    FVector Min = Positions[0];
    FVector Max = Positions[0];
    for (const FVector& Position : Positions)
    {
        Min = Min.ComponentMin(Position);
        Max = Max.ComponentMax(Position);
    }
    return Max - Min;
}

// Mean length of the edges INCIDENT TO each vertex, measured on the mesh as given. Written out
// here rather than shared with the op, deliberately: a test that imports the implementation's own
// helper cannot tell a correct formula from a formula that agrees with itself.
TArray<double> DeformerFrameTest_MeanOneRingEdgeLengths(UDynamicMesh* Mesh)
{
    TArray<double> Means;
    Mesh->ProcessMesh([&Means](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        Means.Reserve(ReadMesh.VertexCount());
        for (const int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            const FVector3d Position = ReadMesh.GetVertex(VertexID);
            double Sum = 0.0;
            int32 Count = 0;
            for (const int32 EdgeID : ReadMesh.VtxEdgesItr(VertexID))
            {
                const UE::Geometry::FIndex2i EdgeVertices = ReadMesh.GetEdgeV(EdgeID);
                const int32 Far = (EdgeVertices.A == VertexID) ? EdgeVertices.B : EdgeVertices.A;
                Sum += FVector3d::Distance(Position, ReadMesh.GetVertex(Far));
                ++Count;
            }
            Means.Add(Count > 0 ? Sum / static_cast<double>(Count) : 0.0);
        }
    });
    return Means;
}
}

// ============================================================================
// 1. The default warp frame IS the identity that was hardcoded
// ============================================================================
//
// The one assertion that has to hold for this change to be safe to land on existing content: an
// op run with default FWarpFrameSpec must reproduce, vertex for vertex, what the raw engine call
// at FTransform::Identity produces with default engine options. Not "close" - equal. The
// arithmetic reason is on FWarpFrameSpec (a cyclic basis at Axis=Z is the identity basis, and a
// zero Center is a zero translation), and this is that reason turned into a measurement.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryWarpFrameDefaultIsIdentityTest,
    "PinWright.Geometry.Ops.WarpFrame.DefaultFrameReproducesTheHardcodedIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryWarpFrameDefaultIsIdentityTest::RunTest(const FString& Parameters)
{
    // bend
    {
        TStrongObjectPtr<UDynamicMesh> ViaOp(DeformerFrameTest_NewBox(100, 100, 200, 3, 3, 6));
        GeometryOps::FBendParams Params;
        const GeometryOps::FOpResult Op = GeometryOps::Bend(ViaOp.Get(), Params);
        TestTrue(TEXT("bend succeeded"), Op.bSuccess);

        TStrongObjectPtr<UDynamicMesh> ViaEngine(DeformerFrameTest_NewBox(100, 100, 200, 3, 3, 6));
        FGeometryScriptBendWarpOptions EngineOptions;
        UGeometryScriptLibrary_MeshDeformFunctions::ApplyBendWarpToMesh(
            ViaEngine.Get(), EngineOptions, FTransform::Identity,
            static_cast<float>(Params.Angle), static_cast<float>(Params.Extent), nullptr);

        TestEqual(TEXT("bend at the default frame is the engine call at Identity"),
            DeformerFrameTest_MaxDelta(
                DeformerFrameTest_Positions(ViaOp.Get()),
                DeformerFrameTest_Positions(ViaEngine.Get())),
            0.0);
    }

    // twist
    {
        TStrongObjectPtr<UDynamicMesh> ViaOp(DeformerFrameTest_NewBox(100, 100, 200, 3, 3, 6));
        GeometryOps::FTwistParams Params;
        const GeometryOps::FOpResult Op = GeometryOps::Twist(ViaOp.Get(), Params);
        TestTrue(TEXT("twist succeeded"), Op.bSuccess);

        TStrongObjectPtr<UDynamicMesh> ViaEngine(DeformerFrameTest_NewBox(100, 100, 200, 3, 3, 6));
        FGeometryScriptTwistWarpOptions EngineOptions;
        UGeometryScriptLibrary_MeshDeformFunctions::ApplyTwistWarpToMesh(
            ViaEngine.Get(), EngineOptions, FTransform::Identity,
            static_cast<float>(Params.Angle), static_cast<float>(Params.Extent), nullptr);

        TestEqual(TEXT("twist at the default frame is the engine call at Identity"),
            DeformerFrameTest_MaxDelta(
                DeformerFrameTest_Positions(ViaOp.Get()),
                DeformerFrameTest_Positions(ViaEngine.Get())),
            0.0);
    }

    // taper
    {
        TStrongObjectPtr<UDynamicMesh> ViaOp(DeformerFrameTest_NewBox(100, 100, 200, 3, 3, 6));
        GeometryOps::FTaperParams Params;
        const GeometryOps::FOpResult Op = GeometryOps::Taper(ViaOp.Get(), Params);
        TestTrue(TEXT("taper succeeded"), Op.bSuccess);

        TStrongObjectPtr<UDynamicMesh> ViaEngine(DeformerFrameTest_NewBox(100, 100, 200, 3, 3, 6));
        FGeometryScriptFlareWarpOptions EngineOptions;
        UGeometryScriptLibrary_MeshDeformFunctions::ApplyFlareWarpToMesh(
            ViaEngine.Get(), EngineOptions, FTransform::Identity,
            static_cast<float>(Params.FlareX), static_cast<float>(Params.FlareY),
            static_cast<float>(Params.Extent), nullptr);

        TestEqual(TEXT("taper at the default frame is the engine call at Identity"),
            DeformerFrameTest_MaxDelta(
                DeformerFrameTest_Positions(ViaOp.Get()),
                DeformerFrameTest_Positions(ViaEngine.Get())),
            0.0);
    }

    return true;
}

// ============================================================================
// 2. `axis` moves which axis the extent is measured along
// ============================================================================
//
// The defect this fixes, expressed as geometry rather than as a parameter: a form elongated along
// X could not be tapered along its own length. On an X-elongated box the two axes are
// distinguishable by the bounding box alone - a taper about X flares the Y/Z cross-section and
// leaves the X span alone, while the default Z taper flares the X/Y cross-section and so GROWS
// the X span. Both readings are asserted, so a change that made `axis` a no-op fails on the pair
// rather than on one arm that could be satisfied by accident.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryWarpFrameAxisTest,
    "PinWright.Geometry.Ops.WarpFrame.AxisChoosesWhichAxisTheExtentSpans",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryWarpFrameAxisTest::RunTest(const FString& Parameters)
{
    // Long in X, thin in Y and Z: the shape the identity frame could not warp along its length.
    const FVector SourceExtent = DeformerFrameTest_BoundsExtent(
        DeformerFrameTest_Positions(
            TStrongObjectPtr<UDynamicMesh>(DeformerFrameTest_NewBox(400, 40, 40, 8, 2, 2)).Get()));

    GeometryOps::FTaperParams AlongX;
    AlongX.FlareX = 100.0;
    AlongX.FlareY = 100.0;
    AlongX.Extent = 200.0;
    AlongX.Frame.Axis = GeometryOps::EMeshAxis::X;

    TStrongObjectPtr<UDynamicMesh> XMesh(DeformerFrameTest_NewBox(400, 40, 40, 8, 2, 2));
    TestTrue(TEXT("taper about X succeeded"), GeometryOps::Taper(XMesh.Get(), AlongX).bSuccess);
    const FVector XExtent = DeformerFrameTest_BoundsExtent(DeformerFrameTest_Positions(XMesh.Get()));

    GeometryOps::FTaperParams AlongZ = AlongX;
    AlongZ.Frame.Axis = GeometryOps::EMeshAxis::Z;

    TStrongObjectPtr<UDynamicMesh> ZMesh(DeformerFrameTest_NewBox(400, 40, 40, 8, 2, 2));
    TestTrue(TEXT("taper about Z succeeded"), GeometryOps::Taper(ZMesh.Get(), AlongZ).bSuccess);
    const FVector ZExtent = DeformerFrameTest_BoundsExtent(DeformerFrameTest_Positions(ZMesh.Get()));

    TestTrue(TEXT("the fixture is long in X"), SourceExtent.X > 300.0);

    // About X, the flare acts on the two PERPENDICULAR axes, so the long axis is left alone and
    // the thin cross-section opens up.
    TestTrue(TEXT("a taper about X leaves the X span alone"),
        FMath::Abs(XExtent.X - SourceExtent.X) < 1.0);
    TestTrue(TEXT("a taper about X flares the Y cross-section"), XExtent.Y > SourceExtent.Y * 1.2);
    TestTrue(TEXT("a taper about X flares the Z cross-section"), XExtent.Z > SourceExtent.Z * 1.2);

    // About Z - the axis that was hardcoded - the long axis IS one of the perpendiculars, so the
    // same request grows the X span instead. This is the arm that shows the parameter is load
    // bearing rather than cosmetic.
    TestTrue(TEXT("a taper about Z grows the X span instead"), ZExtent.X > SourceExtent.X * 1.2);
    TestTrue(TEXT("a taper about Z leaves the Z span alone"),
        FMath::Abs(ZExtent.Z - SourceExtent.Z) < 1.0);

    return true;
}

// ============================================================================
// 3. `center` is exactly a translation of the deform
// ============================================================================
//
// Stated as an equivariance so it cannot be satisfied by "something changed": deform a mesh about
// the origin, then deform the SAME mesh translated by T about Center = T, and the second result
// must be the first result translated by T - vertex for vertex, to floating-point tolerance.
//
// The failure it guards is the one a limb placed away from the origin used to hit: with Center
// pinned at the origin the extent lands somewhere off in space, and every vertex saturates at one
// end of the deform instead of sweeping through it.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryWarpFrameCenterTest,
    "PinWright.Geometry.Ops.WarpFrame.CenterTranslatesTheDeformWithTheMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryWarpFrameCenterTest::RunTest(const FString& Parameters)
{
    const FVector Offset(0.0, 0.0, 500.0);

    GeometryOps::FTwistParams AtOrigin;
    AtOrigin.Angle = 90.0;
    AtOrigin.Extent = 100.0;

    TStrongObjectPtr<UDynamicMesh> Origin(DeformerFrameTest_NewBox(100, 100, 200, 3, 3, 6));
    TestTrue(TEXT("twist at the origin succeeded"),
        GeometryOps::Twist(Origin.Get(), AtOrigin).bSuccess);

    GeometryOps::FTwistParams AtOffset = AtOrigin;
    AtOffset.Frame.Center = Offset;

    TStrongObjectPtr<UDynamicMesh> Moved(
        DeformerFrameTest_NewBox(100, 100, 200, 3, 3, 6, Offset));
    TestTrue(TEXT("twist about the moved centre succeeded"),
        GeometryOps::Twist(Moved.Get(), AtOffset).bSuccess);

    const TArray<FVector> OriginPositions = DeformerFrameTest_Positions(Origin.Get());
    const TArray<FVector> MovedPositions = DeformerFrameTest_Positions(Moved.Get());

    TestTrue(TEXT("moving the mesh and its centre together reproduces the origin result"),
        DeformerFrameTest_MaxDeltaAfterUntranslating(OriginPositions, MovedPositions, Offset) < 1e-3);

    // The other half: with the centre LEFT at the origin, the same moved mesh gets a different
    // deform. Without this the equivariance above would also pass if `center` were ignored and
    // the deform happened to be translation-invariant.
    TStrongObjectPtr<UDynamicMesh> MovedNoCentre(
        DeformerFrameTest_NewBox(100, 100, 200, 3, 3, 6, Offset));
    TestTrue(TEXT("twist on the moved mesh without a centre succeeded"),
        GeometryOps::Twist(MovedNoCentre.Get(), AtOrigin).bSuccess);

    TestTrue(TEXT("leaving the centre at the origin gives a different deform"),
        DeformerFrameTest_MaxDeltaAfterUntranslating(
            OriginPositions, DeformerFrameTest_Positions(MovedNoCentre.Get()), Offset) > 1.0);

    return true;
}

// ============================================================================
// 4. noise_deform absolute mode is still the engine call
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryNoiseScaleAbsoluteIsEngineTest,
    "PinWright.Geometry.Ops.NoiseScale.AbsoluteReproducesTheEngineCallExactly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryNoiseScaleAbsoluteIsEngineTest::RunTest(const FString& Parameters)
{
    GeometryOps::FNoiseDeformParams Params;
    Params.Magnitude = 7.0;
    Params.Frequency = 0.05;
    Params.Seed = 12345;

    TStrongObjectPtr<UDynamicMesh> ViaOp(DeformerFrameTest_NewBox(100, 100, 100, 6, 6, 6));
    const GeometryOps::FOpResult Op = GeometryOps::NoiseDeform(ViaOp.Get(), Params);
    TestTrue(TEXT("absolute noise succeeded"), Op.bSuccess);

    TStrongObjectPtr<UDynamicMesh> ViaEngine(DeformerFrameTest_NewBox(100, 100, 100, 6, 6, 6));
    FGeometryScriptPerlinNoiseOptions EngineOptions;
    EngineOptions.BaseLayer.Magnitude = static_cast<float>(Params.Magnitude);
    EngineOptions.BaseLayer.Frequency = static_cast<float>(Params.Frequency);
    EngineOptions.BaseLayer.RandomSeed = Params.Seed;
    // Mirrors the op's own fork exactly (GeometryOps_Modeling.cpp, NoiseDeform): 5.7 renamed the
    // call because the old one squared Frequency, so "the engine call" is a different symbol
    // either side of that line and this test has to follow it or stop comparing like with like.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    UGeometryScriptLibrary_MeshDeformFunctions::ApplyPerlinNoiseToMesh2(
        ViaEngine.Get(), FGeometryScriptMeshSelection(), EngineOptions, nullptr);
#else
    UGeometryScriptLibrary_MeshDeformFunctions::ApplyPerlinNoiseToMesh(
        ViaEngine.Get(), FGeometryScriptMeshSelection(), EngineOptions, nullptr);
#endif

    TestEqual(TEXT("absolute mode is the engine call, vertex for vertex"),
        DeformerFrameTest_MaxDelta(
            DeformerFrameTest_Positions(ViaOp.Get()),
            DeformerFrameTest_Positions(ViaEngine.Get())),
        0.0);

    // The default is what an existing recipe gets, so it has to BE absolute rather than merely
    // resemble it.
    GeometryOps::FNoiseDeformParams Defaulted = Params;
    TestTrue(TEXT("the struct default is absolute"),
        Defaulted.MagnitudeMode == GeometryOps::ENoiseMagnitudeMode::Absolute);

    return true;
}

// ============================================================================
// 5. relative mode's formula, asserted exactly
// ============================================================================
//
// Absolute at magnitude 1 displaces each vertex by Noise(v) along its normal. Relative at
// magnitude M displaces it by M * MeanEdge(v) * Noise(v) along the same normal. So for every
// vertex the two displacement VECTORS must satisfy
//
//     relative(v) == M * MeanEdge(v) * absolute_at_1(v)
//
// with MeanEdge computed independently in this file from the undeformed fixture. That is a
// per-vertex identity, so a formula that scaled by something merely correlated with edge length -
// a valence, a triangle area, a distance from the centroid - fails it, where a test comparing
// average displacement between a coarse and a fine region would not.
//
// The fixture is deliberately two DISJOINT boxes an order of magnitude apart in size, which is
// the mixed-scale case the mode exists for.

// Relative magnitude mode is built on UGeometryScriptLibrary_MeshDeformFunctions::
// ComputePerlinNoise, which the engine only ships from UE 5.8, so the op refuses the mode
// outright on older engines (see NoiseDeform). The three tests below assert the formula that
// mode computes; the refusal itself is asserted unguarded in the refusals test at the end.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryNoiseScaleRelativeFormulaTest,
    "PinWright.Geometry.Ops.NoiseScale.RelativeScalesByMeanOneRingEdgeLength",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryNoiseScaleRelativeFormulaTest::RunTest(const FString& Parameters)
{
    auto BuildMixedScaleFixture = []() -> UDynamicMesh*
    {
        UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
        FGeometryScriptPrimitiveOptions Options;
        // A coarse body and a fine one, far enough apart that neither's noise sample is the
        // other's. Both are subdivided so the mean edge length is a real average rather than one
        // number repeated.
        UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
            Mesh, Options, FTransform::Identity, 200.0f, 200.0f, 200.0f, 4, 4, 4,
            EGeometryScriptPrimitiveOriginMode::Center, nullptr);
        UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
            Mesh, Options, FTransform(FVector(600.0, 0.0, 0.0)), 20.0f, 20.0f, 20.0f, 4, 4, 4,
            EGeometryScriptPrimitiveOriginMode::Center, nullptr);
        return Mesh;
    };

    const double Magnitude = 0.4;

    TStrongObjectPtr<UDynamicMesh> Source(BuildMixedScaleFixture());
    const TArray<FVector> Undeformed = DeformerFrameTest_Positions(Source.Get());
    const TArray<double> MeanEdges = DeformerFrameTest_MeanOneRingEdgeLengths(Source.Get());

    GeometryOps::FNoiseDeformParams UnitAbsolute;
    UnitAbsolute.Magnitude = 1.0;
    UnitAbsolute.Frequency = 0.02;
    UnitAbsolute.Seed = 7;

    TStrongObjectPtr<UDynamicMesh> AbsoluteMesh(BuildMixedScaleFixture());
    TestTrue(TEXT("unit absolute noise succeeded"),
        GeometryOps::NoiseDeform(AbsoluteMesh.Get(), UnitAbsolute).bSuccess);
    const TArray<FVector> AbsolutePositions = DeformerFrameTest_Positions(AbsoluteMesh.Get());

    GeometryOps::FNoiseDeformParams Relative = UnitAbsolute;
    Relative.Magnitude = Magnitude;
    Relative.MagnitudeMode = GeometryOps::ENoiseMagnitudeMode::Relative;

    TStrongObjectPtr<UDynamicMesh> RelativeMesh(BuildMixedScaleFixture());
    const GeometryOps::FOpResult RelativeOp = GeometryOps::NoiseDeform(RelativeMesh.Get(), Relative);
    TestTrue(TEXT("relative noise succeeded"), RelativeOp.bSuccess);
    TestTrue(TEXT("relative noise reports the change"), RelativeOp.bChanged);
    const TArray<FVector> RelativePositions = DeformerFrameTest_Positions(RelativeMesh.Get());

    if (!TestEqual(TEXT("all three runs see the same vertex set"),
            RelativePositions.Num(), Undeformed.Num())
        || AbsolutePositions.Num() != Undeformed.Num()
        || MeanEdges.Num() != Undeformed.Num())
    {
        return false;
    }

    double WorstError = 0.0;
    double LargestRelativeDisplacement = 0.0;
    double SmallestMeanEdge = TNumericLimits<double>::Max();
    double LargestMeanEdge = 0.0;

    for (int32 Index = 0; Index < Undeformed.Num(); ++Index)
    {
        const FVector AbsoluteDisplacement = AbsolutePositions[Index] - Undeformed[Index];
        const FVector RelativeDisplacement = RelativePositions[Index] - Undeformed[Index];
        const FVector Expected = AbsoluteDisplacement * (Magnitude * MeanEdges[Index]);

        WorstError = FMath::Max(WorstError, FVector::Dist(Expected, RelativeDisplacement));
        LargestRelativeDisplacement =
            FMath::Max(LargestRelativeDisplacement, RelativeDisplacement.Size());
        SmallestMeanEdge = FMath::Min(SmallestMeanEdge, MeanEdges[Index]);
        LargestMeanEdge = FMath::Max(LargestMeanEdge, MeanEdges[Index]);
    }

    // The fixture has to actually span two scales, or the identity above is trivially satisfiable.
    TestTrue(TEXT("the fixture spans an order of magnitude in edge length"),
        LargestMeanEdge > SmallestMeanEdge * 5.0);
    TestTrue(TEXT("relative mode displaced the fixture"), LargestRelativeDisplacement > 1.0);

    // Tolerance is float-scale, not double-scale: the engine computes its noise value in float on
    // both paths, so the two agree to float precision and no further.
    TestTrue(FString::Printf(TEXT(
        "relative displacement is magnitude * mean one-ring edge length * the absolute unit "
        "displacement, vertex for vertex (worst error %.6g uu over a largest displacement of "
        "%.6g uu)"), WorstError, LargestRelativeDisplacement),
        WorstError < 1e-3);

    return true;
}

// ============================================================================
// 6. the two vertex shapes the documentation has to be right about
// ============================================================================
//
// BOUNDARY: an open mesh has vertices whose one-ring is an open fan. They are not skipped and
// their mean is not extrapolated across the hole - it is the mean of the edges that are actually
// there, which is a smaller sample of the same quantity. Asserted on a plane, where every corner
// and edge vertex is a boundary vertex.
//
// SPLIT / DUPLICATED: two vertices at the SAME position with disjoint one-rings each get their own
// mean, so they displace by different amounts and a seam between them opens. That is inherent to a
// per-vertex local measure and is the reason absolute mode is worth keeping; the test pins the
// contrast rather than the defect, by showing the same pair displaces IDENTICALLY under absolute.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryNoiseScaleBoundaryTest,
    "PinWright.Geometry.Ops.NoiseScale.BoundaryVerticesUseTheirOwnOpenFan",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryNoiseScaleBoundaryTest::RunTest(const FString& Parameters)
{
    auto BuildPlane = []() -> UDynamicMesh*
    {
        UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
        FGeometryScriptPrimitiveOptions Options;
        UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendRectangleXY(
            Mesh, Options, FTransform::Identity, 200.0f, 200.0f, 5, 5, nullptr);
        return Mesh;
    };

    const double Magnitude = 0.5;

    TStrongObjectPtr<UDynamicMesh> Source(BuildPlane());
    const TArray<FVector> Undeformed = DeformerFrameTest_Positions(Source.Get());
    const TArray<double> MeanEdges = DeformerFrameTest_MeanOneRingEdgeLengths(Source.Get());

    // The plane must actually have boundary vertices with a smaller fan than the interior ones,
    // or "boundary vertices are handled" would be untested.
    int32 BoundaryVertices = 0;
    Source->ProcessMesh([&BoundaryVertices](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        for (const int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            if (ReadMesh.IsBoundaryVertex(VertexID))
            {
                ++BoundaryVertices;
            }
        }
    });
    TestTrue(TEXT("the plane fixture has boundary vertices"), BoundaryVertices > 0);

    GeometryOps::FNoiseDeformParams UnitAbsolute;
    UnitAbsolute.Magnitude = 1.0;
    UnitAbsolute.Frequency = 0.03;
    UnitAbsolute.Seed = 3;

    TStrongObjectPtr<UDynamicMesh> AbsoluteMesh(BuildPlane());
    TestTrue(TEXT("unit absolute noise succeeded on the open mesh"),
        GeometryOps::NoiseDeform(AbsoluteMesh.Get(), UnitAbsolute).bSuccess);
    const TArray<FVector> AbsolutePositions = DeformerFrameTest_Positions(AbsoluteMesh.Get());

    GeometryOps::FNoiseDeformParams Relative = UnitAbsolute;
    Relative.Magnitude = Magnitude;
    Relative.MagnitudeMode = GeometryOps::ENoiseMagnitudeMode::Relative;

    TStrongObjectPtr<UDynamicMesh> RelativeMesh(BuildPlane());
    TestTrue(TEXT("relative noise succeeded on the open mesh"),
        GeometryOps::NoiseDeform(RelativeMesh.Get(), Relative).bSuccess);
    const TArray<FVector> RelativePositions = DeformerFrameTest_Positions(RelativeMesh.Get());

    double WorstBoundaryError = 0.0;
    double LargestBoundaryDisplacement = 0.0;
    Source->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        int32 Index = 0;
        for (const int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            if (ReadMesh.IsBoundaryVertex(VertexID))
            {
                const FVector AbsoluteDisplacement = AbsolutePositions[Index] - Undeformed[Index];
                const FVector RelativeDisplacement = RelativePositions[Index] - Undeformed[Index];
                const FVector Expected = AbsoluteDisplacement * (Magnitude * MeanEdges[Index]);
                WorstBoundaryError =
                    FMath::Max(WorstBoundaryError, FVector::Dist(Expected, RelativeDisplacement));
                LargestBoundaryDisplacement =
                    FMath::Max(LargestBoundaryDisplacement, RelativeDisplacement.Size());
            }
            ++Index;
        }
    });

    TestTrue(TEXT("boundary vertices are displaced rather than skipped"),
        LargestBoundaryDisplacement > 0.01);
    TestTrue(TEXT("a boundary vertex scales by the mean of the edges it actually has"),
        WorstBoundaryError < 1e-3);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryNoiseScaleSplitVertexTest,
    "PinWright.Geometry.Ops.NoiseScale.SplitVerticesEachGetTheirOwnMean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryNoiseScaleSplitVertexTest::RunTest(const FString& Parameters)
{
    // Two open rectangles meeting at a corner but never welded: one vertex per rectangle sits at
    // exactly (0,0,0), with disjoint one-rings ten times apart in edge length and the same +Z
    // normal. Closed boxes would give the two coincident corners opposite outward normals, which
    // would make the absolute engine call move them apart and invalidate the seam contract this
    // test is meant to contrast.
    auto BuildTouchingPair = []() -> UDynamicMesh*
    {
        UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
        FGeometryScriptPrimitiveOptions Options;
        UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendRectangleXY(
            Mesh, Options, FTransform(FVector(50.0, 50.0, 0.0)),
            100.0f, 100.0f, 0, 0, nullptr);
        UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendRectangleXY(
            Mesh, Options, FTransform(FVector(5.0, 5.0, 0.0)),
            10.0f, 10.0f, 0, 0, nullptr);
        return Mesh;
    };

    TStrongObjectPtr<UDynamicMesh> Source(BuildTouchingPair());
    const TArray<FVector> Undeformed = DeformerFrameTest_Positions(Source.Get());
    const TArray<double> MeanEdges = DeformerFrameTest_MeanOneRingEdgeLengths(Source.Get());

    TArray<int32> CoincidentIndices;
    for (int32 Index = 0; Index < Undeformed.Num(); ++Index)
    {
        if (Undeformed[Index].IsNearlyZero(1e-4))
        {
            CoincidentIndices.Add(Index);
        }
    }

    if (!TestEqual(TEXT("the fixture has exactly one duplicated vertex position"),
            CoincidentIndices.Num(), 2))
    {
        return false;
    }

    const int32 First = CoincidentIndices[0];
    const int32 Second = CoincidentIndices[1];
    TestTrue(TEXT("the two coincident vertices have different one-ring scales"),
        FMath::Max(MeanEdges[First], MeanEdges[Second])
            > FMath::Min(MeanEdges[First], MeanEdges[Second]) * 3.0);

    GeometryOps::FNoiseDeformParams Absolute;
    Absolute.Magnitude = 4.0;
    Absolute.Frequency = 0.02;
    Absolute.Seed = 11;

    TStrongObjectPtr<UDynamicMesh> AbsoluteMesh(BuildTouchingPair());
    TestTrue(TEXT("absolute noise succeeded"),
        GeometryOps::NoiseDeform(AbsoluteMesh.Get(), Absolute).bSuccess);
    const TArray<FVector> AbsolutePositions = DeformerFrameTest_Positions(AbsoluteMesh.Get());

    // Same position, same normal direction, one magnitude for the whole mesh: absolute keeps the
    // seam closed. This is the contrast that makes the relative behaviour a documented trade
    // rather than a bug.
    TestTrue(TEXT("absolute mode moves both halves of the seam together"),
        FVector::Dist(AbsolutePositions[First], AbsolutePositions[Second]) < 1e-3);

    GeometryOps::FNoiseDeformParams Relative = Absolute;
    Relative.Magnitude = 0.3;
    Relative.MagnitudeMode = GeometryOps::ENoiseMagnitudeMode::Relative;

    TStrongObjectPtr<UDynamicMesh> RelativeMesh(BuildTouchingPair());
    TestTrue(TEXT("relative noise succeeded"),
        GeometryOps::NoiseDeform(RelativeMesh.Get(), Relative).bSuccess);
    const TArray<FVector> RelativePositions = DeformerFrameTest_Positions(RelativeMesh.Get());

    // And relative opens it, by exactly the ratio of the two means. Documented on
    // ENoiseMagnitudeMode::Relative; asserted here so the documentation cannot quietly become
    // wrong.
    const double FirstDisplacement = FVector::Dist(RelativePositions[First], Undeformed[First]);
    const double SecondDisplacement = FVector::Dist(RelativePositions[Second], Undeformed[Second]);

    TestTrue(TEXT("relative mode displaced the seam vertices"),
        FMath::Min(FirstDisplacement, SecondDisplacement) > 1e-4);

    const double DisplacementRatio = FirstDisplacement / SecondDisplacement;
    const double MeanRatio = MeanEdges[First] / MeanEdges[Second];
    TestTrue(FString::Printf(TEXT(
        "the two halves of the seam displace in the ratio of their own mean one-ring edge "
        "lengths (displacement ratio %.6g, mean ratio %.6g)"), DisplacementRatio, MeanRatio),
        FMath::Abs(DisplacementRatio - MeanRatio) < 1e-3 * FMath::Max(1.0, MeanRatio));

    return true;
}
#endif // relative magnitude mode (UE 5.8+)

// ============================================================================
// 7. the refusals and the isolated-vertex report
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryNoiseScaleRefusalsTest,
    "PinWright.Geometry.Ops.NoiseScale.RelativeRefusesTheVectorFormAndReportsLoneVertices",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryNoiseScaleRefusalsTest::RunTest(const FString& Parameters)
{
    // The vector form has three decorrelated fields whose offsets have no public entry point, so
    // relative refuses rather than guessing at them.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(DeformerFrameTest_NewBox(100, 100, 100, 3, 3, 3));
        GeometryOps::FNoiseDeformParams Params;
        Params.MagnitudeMode = GeometryOps::ENoiseMagnitudeMode::Relative;
        Params.bApplyAlongNormal = false;

        const GeometryOps::FOpResult Op = GeometryOps::NoiseDeform(Mesh.Get(), Params);
        TestFalse(TEXT("relative with applyAlongNormal false is refused"), Op.bSuccess);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        TestEqual(TEXT("the refusal is INVALID_ARGUMENT"),
            Op.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        TestTrue(TEXT("the refusal names both parameters"),
            Op.ErrorMessage.Contains(TEXT("magnitudeMode"))
                && Op.ErrorMessage.Contains(TEXT("applyAlongNormal")));
#else
        // Below 5.8 the whole relative mode is refused first, for want of ComputePerlinNoise, so
        // the request never reaches the vector-form check. That refusal is the one asserted here -
        // the mode is still refused rather than silently degraded to absolute.
        TestEqual(TEXT("the refusal is UNSUPPORTED_ENGINE_VERSION"),
            Op.ErrorCode, FString(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION));
        TestTrue(TEXT("the refusal names the mode and the engine it needs"),
            Op.ErrorMessage.Contains(TEXT("magnitudeMode"))
                && Op.ErrorMessage.Contains(TEXT("5.8")));
#endif
    }

    // Absolute takes the same request without complaint - the refusal is about the relative
    // formula, not about the vector form itself.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(DeformerFrameTest_NewBox(100, 100, 100, 3, 3, 3));
        GeometryOps::FNoiseDeformParams Params;
        Params.bApplyAlongNormal = false;

        TestTrue(TEXT("absolute still accepts the vector form"),
            GeometryOps::NoiseDeform(Mesh.Get(), Params).bSuccess);
    }

    // A vertex with no incident edges has no one-ring, so it has no local feature size to be a
    // fraction of. It stays where it is and is reported rather than displaced by an invented
    // number. Relative mode only runs from 5.8 (see the guard above the formula tests).
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(DeformerFrameTest_NewBox(100, 100, 100, 3, 3, 3));
        FVector LonePosition(400.0, 0.0, 0.0);
        Mesh->EditMesh([&LonePosition](UE::Geometry::FDynamicMesh3& EditMesh)
        {
            EditMesh.AppendVertex((FVector3d)LonePosition);
        });

        GeometryOps::FNoiseDeformParams Params;
        Params.Magnitude = 0.5;
        Params.Frequency = 0.02;
        Params.MagnitudeMode = GeometryOps::ENoiseMagnitudeMode::Relative;

        const GeometryOps::FOpResult Op = GeometryOps::NoiseDeform(Mesh.Get(), Params);
        TestTrue(TEXT("a lone vertex does not fail the op"), Op.bSuccess);

        bool bReported = false;
        for (const FString& Warning : Op.Warnings)
        {
            bReported = bReported || Warning.Contains(TEXT("no incident edges"));
        }
        TestTrue(TEXT("the lone vertex is reported"), bReported);

        bool bStayedPut = false;
        Mesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& ReadMesh)
        {
            for (const int32 VertexID : ReadMesh.VertexIndicesItr())
            {
                if (ReadMesh.GetVtxEdgeCount(VertexID) == 0)
                {
                    bStayedPut = FVector::Dist(
                        (FVector)ReadMesh.GetVertex(VertexID), LonePosition) < 1e-6;
                }
            }
        });
        TestTrue(TEXT("the lone vertex was left where it was"), bStayedPut);
    }
#endif // relative magnitude mode (UE 5.8+)

    return true;
}
