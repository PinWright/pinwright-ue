// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the two silent failure modes of the sweep family.
//
// 1. A PATH THAT LIES IN ITS OWN SECTION PLANE. AppendSweepPolygon lands the cross-section in
//    each frame's (Rotation.AxisY(), Rotation.AxisZ()) plane and takes Rotation.AxisX() as that
//    plane's normal (MeshPrimitiveFunctions.cpp:1136-1138), so frames left at rotation (0,0,0)
//    on a path that runs along Z slide the section along INSIDE its own plane and sweep no
//    volume. Nothing in the engine reports it. Measured before the fix, on the broken form and
//    a corrected control: both success, both isClosed, both 0 boundary edges, both 26
//    triangles - the only tells were a degenerate-triangle count of 2 against 0 and a bounding
//    box with zero extent on one axis, neither of which names a parameter. The shipped example
//    in docs/pwmodel-format.md was one of the broken ones.
//
// 2. `cap` ON extrude_along_spline. The op hardcoded bLoop=true, and
//    FGeneralizedCylinderGenerator builds caps under `if (bCapped && !bLoop)`
//    (SweepGenerator.cpp:608, :707), so `cap` was unreachable at every path length: `cap`
//    omitted and `cap=true` produced byte-identical meshes. A 2-frame path came back with 8
//    boundary edges and not closed, where `sweep` over the same profile and path came back
//    closed with caps.
//
// Both are asserted the way they were measured - against a control that differs only in the
// parameter under test - because an absolute triangle count says nothing here: the broken and
// the correct form had the same one.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Advanced.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

UDynamicMesh* SweepGuardTest_NewMesh()
{
    return NewObject<UDynamicMesh>(GetTransientPackage());
}

// The four-point section the pwmodel-format measurements were taken on: 20 wide in the frame's
// local Y, 4 deep in its local Z.
GeometryOps::FSweepProfile SweepGuardTest_Profile()
{
    GeometryOps::FSweepProfile Profile;
    Profile.Vertices = { FVector2D(10.0, 0.0), FVector2D(10.0, 4.0),
                         FVector2D(-10.0, 4.0), FVector2D(-10.0, 0.0) };
    return Profile;
}

FTransform SweepGuardTest_Frame(const FVector& Location, const FRotator& Rotation)
{
    return FTransform(Rotation.Quaternion(), Location, FVector::OneVector);
}

bool SweepGuardTest_HasWarningStarting(const TArray<FString>& Warnings, const TCHAR* Prefix)
{
    for (const FString& Warning : Warnings)
    {
        if (Warning.StartsWith(Prefix))
        {
            return true;
        }
    }
    return false;
}

bool SweepGuardTest_HasWarningContaining(const TArray<FString>& Warnings, const TCHAR* Needle)
{
    for (const FString& Warning : Warnings)
    {
        if (Warning.Contains(Needle))
        {
            return true;
        }
    }
    return false;
}

FString SweepGuardTest_Join(const TArray<FString>& Warnings)
{
    return Warnings.Num() == 0 ? TEXT("<none>") : FString::Join(Warnings, TEXT(" | "));
}
}

// ============================================================================
// sweep: a path inside the section plane is named, a correct one is silent
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySweepInPlanePathTest,
    "PinWright.Geometry.Ops.Advanced.SweepNamesAPathThatLiesInItsSectionPlane",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySweepInPlanePathTest::RunTest(const FString& Parameters)
{
    // BROKEN FORM: frames left unrotated while the path runs +Z. The section plane's normal is
    // world +X and the path direction is world +Z, so the two are perpendicular.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(SweepGuardTest_NewMesh());

        GeometryOps::FSweepParams Params;
        Params.Profile = SweepGuardTest_Profile();

        GeometryOps::FSweepPath Path;
        Path.Samples = {
            SweepGuardTest_Frame(FVector(0, 0, 0), FRotator::ZeroRotator),
            SweepGuardTest_Frame(FVector(0, 0, 100), FRotator::ZeroRotator) };

        GeometryOps::FSweepOutputs Outputs;
        const GeometryOps::FOpResult Result =
            GeometryOps::Sweep(Mesh.Get(), Params, Path, Outputs);

        // It still BUILDS - the op does not refuse and does not substitute a path of its own.
        // The defect was never that it failed; it was that it succeeded without saying anything.
        TestTrue(TEXT("a degenerate sweep still succeeds"), Result.bSuccess);

        TestTrue(*FString::Printf(TEXT("the first offending frame is named. warnings: %s"),
            *SweepGuardTest_Join(Result.Warnings)),
            SweepGuardTest_HasWarningStarting(Result.Warnings, TEXT("path frame 0")));

        // The result-side half of the diagnosis: the sweep had the mesh to itself, so the
        // bounding box is the swept geometry's and it has no thickness on X.
        TestTrue(*FString::Printf(TEXT("and the flat result is reported too. warnings: %s"),
            *SweepGuardTest_Join(Result.Warnings)),
            SweepGuardTest_HasWarningContaining(Result.Warnings, TEXT("zero extent on X")));
    }

    // CORRECTED CONTROL: the same path with each frame carrying the tangent's pitch. Identical
    // in every other respect, so a check that fires on this one is firing on the path's shape
    // rather than on the parameter under test.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(SweepGuardTest_NewMesh());

        GeometryOps::FSweepParams Params;
        Params.Profile = SweepGuardTest_Profile();

        GeometryOps::FSweepPath Path;
        Path.Samples = {
            SweepGuardTest_Frame(FVector(0, 0, 0), FRotator(90.0, 0.0, 0.0)),
            SweepGuardTest_Frame(FVector(0, 0, 100), FRotator(90.0, 0.0, 0.0)) };

        GeometryOps::FSweepOutputs Outputs;
        const GeometryOps::FOpResult Result =
            GeometryOps::Sweep(Mesh.Get(), Params, Path, Outputs);

        TestTrue(TEXT("the corrected sweep succeeds"), Result.bSuccess);
        TestFalse(*FString::Printf(TEXT("and says nothing about its path. warnings: %s"),
            *SweepGuardTest_Join(Result.Warnings)),
            SweepGuardTest_HasWarningStarting(Result.Warnings, TEXT("path")));

        // And it encloses a volume, which the broken form did not.
        const FVector Size = Mesh->GetMeshRef().GetBounds().Diagonal();
        TestTrue(*FString::Printf(TEXT("the corrected sweep has thickness on every axis (%s)"),
            *Size.ToString()),
            Size.X > 1.0 && Size.Y > 1.0 && Size.Z > 1.0);
    }

    return true;
}

// ============================================================================
// sweep: the op's OWN fallback path was the same defect
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySweepVerticalFallbackTest,
    "PinWright.Geometry.Ops.Advanced.SweepVerticalFallbackEnclosesVolume",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySweepVerticalFallbackTest::RunTest(const FString& Parameters)
{
    // No samples selects the documented "sweep vertically through the mesh's own bounding box"
    // fallback - the branch geometry.sweep runs when its spline actor resolves to nothing. Its
    // frames used to be rotations about world Z, which left the section plane's normal
    // perpendicular to the path and made the fallback sweep a flat ribbon at exactly the
    // triangle count a solid tube has. No caller supplied a path, so no `path` warning could
    // name it; the only assertion that can is the enclosed volume.
    TStrongObjectPtr<UDynamicMesh> Mesh(SweepGuardTest_NewMesh());

    GeometryOps::FSweepParams Params;
    Params.Steps = 12;

    GeometryOps::FSweepOutputs Outputs;
    const GeometryOps::FOpResult Result =
        GeometryOps::Sweep(Mesh.Get(), Params, GeometryOps::FSweepPath(), Outputs);

    TestTrue(TEXT("the vertical fallback succeeds"), Result.bSuccess);
    TestTrue(TEXT("and reports itself as the fallback"),
        Outputs.Status.StartsWith(TEXT("Linear sweep")));
    TestTrue(TEXT("and sweeps geometry"), Result.TrianglesAfter > 0);

    const FVector Size = Mesh->GetMeshRef().GetBounds().Diagonal();
    TestTrue(*FString::Printf(TEXT("the fallback tube has thickness on every axis (%s)"),
        *Size.ToString()), Size.X > 1.0 && Size.Y > 1.0 && Size.Z > 1.0);

    // And nothing complains about a `path` the caller never supplied.
    TestFalse(*FString::Printf(TEXT("the fallback names no path. warnings: %s"),
        *SweepGuardTest_Join(Result.Warnings)),
        SweepGuardTest_HasWarningStarting(Result.Warnings, TEXT("path")));

    return true;
}

// ============================================================================
// extrude_along_spline: `cap` reaches the engine on an open path
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryExtrudeAlongSplineCapTest,
    "PinWright.Geometry.Ops.Advanced.ExtrudeAlongSplineHonoursCapOnAnOpenPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryExtrudeAlongSplineCapTest::RunTest(const FString& Parameters)
{
    // A two-frame path running along +X with unrotated frames: the frame's local +X IS the path
    // direction, so this path is sound and only `cap` is under test.
    const TArray<FTransform> Path = {
        SweepGuardTest_Frame(FVector(0, 0, 0), FRotator::ZeroRotator),
        SweepGuardTest_Frame(FVector(200, 0, 0), FRotator::ZeroRotator) };

    auto RunWithCap = [&Path](bool bCap, UDynamicMesh* Mesh)
    {
        GeometryOps::FExtrudeAlongSplineParams Params;
        Params.Profile = SweepGuardTest_Profile();
        Params.bCap = bCap;
        return GeometryOps::ExtrudeAlongSpline(Mesh, Params, Path);
    };

    TStrongObjectPtr<UDynamicMesh> Capped(SweepGuardTest_NewMesh());
    TStrongObjectPtr<UDynamicMesh> Uncapped(SweepGuardTest_NewMesh());

    const GeometryOps::FOpResult CappedResult = RunWithCap(true, Capped.Get());
    const GeometryOps::FOpResult UncappedResult = RunWithCap(false, Uncapped.Get());

    TestTrue(TEXT("both extrudes succeed"), CappedResult.bSuccess && UncappedResult.bSuccess);

    // The whole failure signature: these two used to be byte-identical.
    TestTrue(*FString::Printf(TEXT("cap=true adds end caps (capped %d tris, uncapped %d)"),
        CappedResult.TrianglesAfter, UncappedResult.TrianglesAfter),
        CappedResult.TrianglesAfter > UncappedResult.TrianglesAfter);

    // And what the caps are FOR. Before the fix this measured 8 boundary edges and not closed.
    TestTrue(TEXT("a capped extrude over an open path is closed"),
        Capped->GetMeshRef().IsClosed());
    TestFalse(TEXT("and an uncapped one is not"), Uncapped->GetMeshRef().IsClosed());

    // The path is sound, so neither run may complain about it.
    TestFalse(*FString::Printf(TEXT("no path warning on a sound path. warnings: %s"),
        *SweepGuardTest_Join(CappedResult.Warnings)),
        SweepGuardTest_HasWarningStarting(CappedResult.Warnings, TEXT("path")));

    // `cap` was honoured, so nothing may claim it was ignored.
    TestFalse(*FString::Printf(TEXT("no cap warning on an open path. warnings: %s"),
        *SweepGuardTest_Join(CappedResult.Warnings)),
        SweepGuardTest_HasWarningStarting(CappedResult.Warnings, TEXT("cap")));

    return true;
}

// ============================================================================
// extrude_along_spline: a path that returns to its start is still a closed loop
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryExtrudeAlongSplineClosedPathTest,
    "PinWright.Geometry.Ops.Advanced.ExtrudeAlongSplineLoopsOnlyWhenThePathReturns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryExtrudeAlongSplineClosedPathTest::RunTest(const FString& Parameters)
{
    // An octagonal ring of radius 100 with the ninth frame repeating the first, each frame
    // carrying yaw = theta + 90 so its local +X is the ring's tangent. This is the authoring
    // shape a closed sweep has to keep working: neither front-end publishes a `loop` parameter,
    // so returning to the start is the only signal a document can carry.
    TArray<FTransform> Ring;
    for (int32 Index = 0; Index <= 8; ++Index)
    {
        const double Theta = 45.0 * Index;
        const double Radians = FMath::DegreesToRadians(Theta);
        Ring.Add(SweepGuardTest_Frame(
            FVector(100.0 * FMath::Cos(Radians), 100.0 * FMath::Sin(Radians), 0.0),
            FRotator(0.0, Theta + 90.0, 0.0)));
    }

    TStrongObjectPtr<UDynamicMesh> Mesh(SweepGuardTest_NewMesh());

    GeometryOps::FExtrudeAlongSplineParams Params;
    Params.Profile = SweepGuardTest_Profile();
    Params.bCap = true;

    const GeometryOps::FOpResult Result =
        GeometryOps::ExtrudeAlongSpline(Mesh.Get(), Params, Ring);

    TestTrue(TEXT("a closing path succeeds"), Result.bSuccess);

    // A loop has no ends, so `cap` had nothing to do - and the op says so rather than leaving
    // the caller to discover it. This is the "reject or warn" half of the fix.
    TestTrue(*FString::Printf(TEXT("cap is reported as inapplicable. warnings: %s"),
        *SweepGuardTest_Join(Result.Warnings)),
        SweepGuardTest_HasWarningStarting(Result.Warnings, TEXT("cap has no effect")));

    // Watertight without caps, which is the reason a closing path is still swept as a loop
    // rather than as an open span with two coincident caps buried in each other.
    TestTrue(TEXT("the ring comes back closed"), Mesh->GetMeshRef().IsClosed());

    // The repeated final frame is DROPPED before the engine wraps the ring itself
    // (NumPathSegs = bLoop ? Path.Num() : Path.Num() - 1, SweepGenerator.cpp:96), so the tube
    // must carry exactly one more segment than the same eight frames swept open - and not two,
    // which is what leaving the duplicate in place would produce.
    {
        TStrongObjectPtr<UDynamicMesh> OpenMesh(SweepGuardTest_NewMesh());

        TArray<FTransform> OpenFrames = Ring;
        OpenFrames.RemoveAt(OpenFrames.Num() - 1);

        GeometryOps::FExtrudeAlongSplineParams OpenParams;
        OpenParams.Profile = SweepGuardTest_Profile();
        OpenParams.bCap = false;

        const GeometryOps::FOpResult OpenResult =
            GeometryOps::ExtrudeAlongSpline(OpenMesh.Get(), OpenParams, OpenFrames);

        TestTrue(*FString::Printf(TEXT("the ring adds the wrap segment (open %d, ring %d)"),
            OpenResult.TrianglesAfter, Result.TrianglesAfter),
            Result.TrianglesAfter > OpenResult.TrianglesAfter);
        TestTrue(*FString::Printf(TEXT("and adds it once, not twice (open %d, ring %d)"),
            OpenResult.TrianglesAfter, Result.TrianglesAfter),
            Result.TrianglesAfter < OpenResult.TrianglesAfter * 2);
        TestFalse(TEXT("the same frames swept open are not closed"),
            OpenMesh->GetMeshRef().IsClosed());
    }

    // cap=false on the same ring is the control for the warning: nothing was ignored, so
    // nothing is reported.
    {
        TStrongObjectPtr<UDynamicMesh> Uncapped(SweepGuardTest_NewMesh());

        GeometryOps::FExtrudeAlongSplineParams UncappedParams;
        UncappedParams.Profile = SweepGuardTest_Profile();
        UncappedParams.bCap = false;

        const GeometryOps::FOpResult UncappedResult =
            GeometryOps::ExtrudeAlongSpline(Uncapped.Get(), UncappedParams, Ring);

        TestFalse(*FString::Printf(TEXT("cap=false on a ring says nothing. warnings: %s"),
            *SweepGuardTest_Join(UncappedResult.Warnings)),
            SweepGuardTest_HasWarningStarting(UncappedResult.Warnings, TEXT("cap")));
        TestEqual(TEXT("and builds the same tube"), UncappedResult.TrianglesAfter,
            Result.TrianglesAfter);
    }

    return true;
}
