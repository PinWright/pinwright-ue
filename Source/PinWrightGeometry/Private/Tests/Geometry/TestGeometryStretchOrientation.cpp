// Copyright (c) 2026 Alexander Penkin. MIT License.

// Orientation regression net for GeometryOps::Stretch. Separate from TestGeometryOpsModeling.cpp,
// which covers the verb CONTRACT (null mesh, error codes) and whose only stretch assertion passed
// while a negative factor produced inside-out geometry.
//
// THE DEFECT. Stretch built a per-axis scale vector and applied it under
// `#if (UE_VERSION >= 5.4 && UE_VERSION < 5.5)` via the engine ScaleMesh, falling back to a
// hand-rolled per-vertex multiply on every other version. 5.8 is the only version this plugin
// builds, so the fallback was the shipped path - and it moved positions only. A negative factor
// is a reflection (determinant < 0), which turns every triangle inside out; nothing clamps
// Factor, so `stretch axis=z factor=-1` was reachable from both front-ends. This is the same
// dead-guard defect that made `mirror` emit inside-out geometry, covered separately in
// TestGeometryMirrorOrientation.cpp.
//
// WHY SIGNED VOLUME AND NOT NORMALS. A normal-vs-winding consistency check PASSES on the broken
// mesh: inverting every triangle leaves the mesh perfectly self-consistent, just inside out. A
// recalculate_normals afterwards makes such a check pass BY CONSTRUCTION. Signed volume reads the
// winding itself:
//
//     V = sum over triangles of dot(A, cross(B, C)) / 6
//
// positive for one winding and negative for the other. For a CLOSED shell that sum is the
// enclosed volume and is independent of where the shell sits relative to the origin, so these
// tests do not depend on the box origin mode.
//
// No test below asserts a particular SIGN. The sign is a convention of the generator; what is
// asserted is that stretch PRESERVES whatever that convention produced, so these stay true if a
// generator winding convention ever flips wholesale.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Modeling.h"
#include "Handlers/Geometry/GeometryOps_Primitives.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

// 40 on a side, so the enclosed volume is exactly 64000 - the figure the sibling mirror defect
// was measured against.
constexpr double StretchOrientTest_Side = 40.0;
constexpr double StretchOrientTest_Volume = 64000.0;

UDynamicMesh* StretchOrientTest_NewBox()
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());

    GeometryOps::FBoxParams Params;
    Params.Size = FVector(StretchOrientTest_Side, StretchOrientTest_Side, StretchOrientTest_Side);

    GeometryOps::GenerateBox(Mesh, Params, FTransform::Identity);
    return Mesh;
}

// The signed volume of the tetrahedron each triangle forms with the origin, summed. Positive for
// one winding and negative for the other, so the SIGN is the orientation.
double StretchOrientTest_SignedVolume(const UE::Geometry::FDynamicMesh3& Mesh)
{
    double Volume = 0.0;
    for (int32 TID : Mesh.TriangleIndicesItr())
    {
        FVector3d A, B, C;
        Mesh.GetTriVertices(TID, A, B, C);
        Volume += A.Dot(B.Cross(C));
    }
    return Volume / 6.0;
}

// Stretch one axis by Factor and report the signed volume before and after.
bool StretchOrientTest_Run(
    FAutomationTestBase& Test, GeometryOps::EMeshAxis Axis, double Factor,
    double& OutBefore, double& OutAfter)
{
    UDynamicMesh* Mesh = StretchOrientTest_NewBox();

    OutBefore = StretchOrientTest_SignedVolume(Mesh->GetMeshRef());
    Test.TestEqual(TEXT("the bare box encloses 40^3"),
        FMath::Abs(OutBefore), StretchOrientTest_Volume, StretchOrientTest_Volume * 1e-6);

    GeometryOps::FStretchParams Params;
    Params.Axis = Axis;
    Params.Factor = Factor;

    const GeometryOps::FOpResult Op = GeometryOps::Stretch(Mesh, Params);
    Test.TestTrue(TEXT("stretch succeeds"), Op.bSuccess);

    OutAfter = StretchOrientTest_SignedVolume(Mesh->GetMeshRef());
    return Op.bSuccess;
}
}

// ============================================================================
// A negative factor must not turn the mesh inside out
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryStretchNegativeFactorPreservesOrientationTest,
    "PinWright.Geometry.Ops.Modeling.StretchNegativeFactorPreservesOrientation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryStretchNegativeFactorPreservesOrientationTest::RunTest(const FString& Parameters)
{
    double Before = 0.0;
    double After = 0.0;
    if (!StretchOrientTest_Run(*this, GeometryOps::EMeshAxis::Z, -1.0, Before, After))
    {
        return false;
    }

    // THE ASSERTION. Scaling z by -1 maps a box onto the same point set, so the magnitude cannot
    // move. The reflection alone negates the signed volume (determinant < 0) and the winding
    // reversal negates it back, so a correct stretch returns the SAME signed value - not merely
    // the same magnitude. The broken path returns the exact negative, and every other observable
    // property of the mesh is identical between the two cases: same vertex count, same triangle
    // count, same bounds, same normal-vs-winding consistency.
    const double Tolerance = StretchOrientTest_Volume * 1e-6;
    TestEqual(TEXT("a factor of -1 preserves the signed volume"), After, Before, Tolerance);

    // Stated a second way, so a change that collapsed the mesh cannot pass the tolerance check
    // above by driving both sides to zero.
    TestTrue(TEXT("a factor of -1 does not invert the shell"), After * Before > 0.0);

    return true;
}

// ============================================================================
// A negative factor that also changes the magnitude
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryStretchNegativeFactorScalesVolumeTest,
    "PinWright.Geometry.Ops.Modeling.StretchNegativeFactorScalesVolume",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryStretchNegativeFactorScalesVolumeTest::RunTest(const FString& Parameters)
{
    double Before = 0.0;
    double After = 0.0;
    if (!StretchOrientTest_Run(*this, GeometryOps::EMeshAxis::Z, -2.0, Before, After))
    {
        return false;
    }

    // Magnitude doubles with the axis, sign is preserved. Separate from the -1 case because -1 is
    // the one factor whose magnitude is unchanged, so a fix that got the magnitude wrong and the
    // sign right would pass the test above on its own.
    TestEqual(TEXT("a factor of -2 doubles the signed volume without inverting it"),
        After, 2.0 * Before, FMath::Abs(Before) * 1e-6);
    TestTrue(TEXT("a factor of -2 does not invert the shell"), After * Before > 0.0);

    return true;
}

// ============================================================================
// Control - a positive factor must NOT be reversed
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryStretchPositiveFactorTest,
    "PinWright.Geometry.Ops.Modeling.StretchPositiveFactorPreservesOrientation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryStretchPositiveFactorTest::RunTest(const FString& Parameters)
{
    double Before = 0.0;
    double After = 0.0;
    if (!StretchOrientTest_Run(*this, GeometryOps::EMeshAxis::Z, 2.0, Before, After))
    {
        return false;
    }

    // The other half of the net: a "fix" that reverses the winding unconditionally, rather than
    // only when the scale determinant is negative, breaks HERE rather than shipping. The engine
    // MeshTransforms::Scale gates the reversal on Scale.X * Scale.Y * Scale.Z < 0
    // (MeshTransforms.cpp:179), so a positive factor must leave the orientation alone.
    TestEqual(TEXT("a factor of 2 doubles the signed volume"),
        After, 2.0 * Before, FMath::Abs(Before) * 1e-6);
    TestTrue(TEXT("a factor of 2 does not invert the shell"), After * Before > 0.0);

    return true;
}
