// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the CONDITIONAL revolve step floor.
//
// The revolve generators floor their own step count at 2 - Steps = FMath::Max(Steps, 2) - and
// that is right for a PARTIAL sweep and wrong for a full turn. A sweep closes at exactly 360
// degrees (bSweepCurveIsClosed = (TotalRevolutionDegrees == 360), RevolveGenerator.cpp:181 and
// :370) and then wraps its last cross-section onto its first, so 2 steps leave two sections
// joined to each other twice with no interior between them. Measured before the fix, all three
// reporting success with no warning at all:
//
//     torus major_segments=2 minor_segments=8   16 tris, 16 boundary edges, isClosed false
//     torus major_segments=3 minor_segments=8   48 tris,  0 boundary edges, isClosed true
//     revolve steps=2 angle=360                  8 tris, 10 boundary edges, 6 degenerate
//     revolve steps=3 angle=360                 24 tris,  0 boundary edges, isClosed true
//     arch   major_steps=2 angle=180            closed, 44 tris, zero diagnostics
//
// The last row is why the floor cannot be a flat bump to 3: a half-arch at 2 steps is sound
// geometry an author is entitled to ask for, and raising it would both change what existing
// documents build and emit a warning about a change that was not needed.
//
// The assertions below are therefore in three parts, and the third is the one a careless fix
// breaks: the closed cases clamp AND report it, the closed cases come back CLOSED, and the
// partial cases are left alone with no warning.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Primitives.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

UDynamicMesh* ClosedRevolveTest_NewMesh()
{
    return NewObject<UDynamicMesh>(GetTransientPackage());
}

// The exact shape both clamp helpers emit. Asserting the substring rather than the whole line
// keeps the range suffix ClampRangeWarn appends out of the assertion while still pinning the
// three things a caller needs: which parameter, what it passed, what actually ran.
bool ClosedRevolveTest_WarnsClamp(const TArray<FString>& Warnings, const TCHAR* Label,
                                  int32 From, int32 To)
{
    const FString Needle = FString::Printf(TEXT("%s clamped from %d to %d"), Label, From, To);
    for (const FString& Warning : Warnings)
    {
        if (Warning.Contains(Needle))
        {
            return true;
        }
    }
    return false;
}

bool ClosedRevolveTest_MentionsLabel(const TArray<FString>& Warnings, const TCHAR* Label)
{
    const FString Needle = FString::Printf(TEXT("%s clamped from "), Label);
    for (const FString& Warning : Warnings)
    {
        if (Warning.StartsWith(Needle))
        {
            return true;
        }
    }
    return false;
}

FString ClosedRevolveTest_Join(const TArray<FString>& Warnings)
{
    return Warnings.Num() == 0 ? TEXT("<none>") : FString::Join(Warnings, TEXT(" | "));
}

// A four-point profile off the revolve axis, the shape the 8-triangle measurement above was
// taken on. Kept off zero radius so nothing collapses onto the axis for reasons unrelated to
// the step count.
TArray<FVector2D> ClosedRevolveTest_Profile()
{
    return { FVector2D(20.0, 0.0), FVector2D(40.0, 0.0),
             FVector2D(40.0, 60.0), FVector2D(20.0, 60.0) };
}
}

// ============================================================================
// torus: always a full turn, so always floor 3
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryClosedRevolveTorusFloorTest,
    "PinWright.Geometry.Ops.Primitives.TorusMajorSegmentsFloorsAtThreeAndCloses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryClosedRevolveTorusFloorTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(ClosedRevolveTest_NewMesh());

    GeometryOps::FTorusParams Params;
    Params.MajorSteps = 2;
    Params.MinorSteps = 8;

    const GeometryOps::FOpResult Result =
        GeometryOps::GenerateTorus(Mesh.Get(), Params, FTransform::Identity);

    TestTrue(TEXT("a torus at majorSegments=2 still builds"), Result.bSuccess);

    // The parameter is echoed by geometry.create_torus, so the clamp has to write back through
    // the params struct or the echo reports a count the engine never used.
    TestEqual(TEXT("majorSegments is raised to 3 in the params the caller reads back"),
        Params.MajorSteps, 3);

    TestTrue(*FString::Printf(TEXT("and the clamp names itself. warnings: %s"),
        *ClosedRevolveTest_Join(Result.Warnings)),
        ClosedRevolveTest_WarnsClamp(Result.Warnings, TEXT("majorSegments"), 2, 3));

    // The whole point of the floor. At 2 this measured 16 triangles with 16 boundary edges.
    TestTrue(TEXT("the torus comes back closed"), Mesh->GetMeshRef().IsClosed());

    // The minor count keeps its own floor, which is the engine's and is reached for a different
    // reason - a closed profile circle, not a closed sweep. A fix that collapses the two floors
    // into one number fails here.
    TestEqual(TEXT("minorSegments is untouched at 8"), Params.MinorSteps, 8);

    return true;
}

// ============================================================================
// arch: the floor follows the angle
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryClosedRevolveArchFloorTest,
    "PinWright.Geometry.Ops.Primitives.ArchMajorStepsFloorFollowsTheSweepAngle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryClosedRevolveArchFloorTest::RunTest(const FString& Parameters)
{
    // A half-arch at 2 steps is SOUND and must be left exactly as it is - no clamp, no warning.
    // This is the assertion a flat bump to 3 breaks.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ClosedRevolveTest_NewMesh());

        GeometryOps::FArchParams Params;
        Params.Angle = 180.0;
        Params.MajorSteps = 2;
        Params.MinorSteps = 8;

        const GeometryOps::FOpResult Result =
            GeometryOps::GenerateArch(Mesh.Get(), Params, FTransform::Identity);

        TestTrue(TEXT("a 180-degree arch at majorSteps=2 succeeds"), Result.bSuccess);
        TestEqual(TEXT("and its majorSteps is left at 2"), Params.MajorSteps, 2);
        TestFalse(*FString::Printf(TEXT("with nothing clamped. warnings: %s"),
            *ClosedRevolveTest_Join(Result.Warnings)),
            ClosedRevolveTest_MentionsLabel(Result.Warnings, TEXT("majorSteps")));
        TestTrue(TEXT("and it still builds geometry"), Result.TrianglesAfter > 0);
    }

    // The same verb at 360 is a torus and takes the torus's floor.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ClosedRevolveTest_NewMesh());

        GeometryOps::FArchParams Params;
        Params.Angle = 360.0;
        Params.MajorSteps = 2;
        Params.MinorSteps = 8;

        const GeometryOps::FOpResult Result =
            GeometryOps::GenerateArch(Mesh.Get(), Params, FTransform::Identity);

        TestTrue(TEXT("a 360-degree arch at majorSteps=2 succeeds"), Result.bSuccess);
        TestEqual(TEXT("its majorSteps is raised to 3"), Params.MajorSteps, 3);
        TestTrue(*FString::Printf(TEXT("and the clamp names itself. warnings: %s"),
            *ClosedRevolveTest_Join(Result.Warnings)),
            ClosedRevolveTest_WarnsClamp(Result.Warnings, TEXT("majorSteps"), 2, 3));
        TestTrue(TEXT("and the full turn comes back closed"), Mesh->GetMeshRef().IsClosed());
    }

    return true;
}

// ============================================================================
// revolve: same conditional floor, and its default angle is 360
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryClosedRevolveRevolveFloorTest,
    "PinWright.Geometry.Ops.Primitives.RevolveStepsFloorFollowsTheSweepAngle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryClosedRevolveRevolveFloorTest::RunTest(const FString& Parameters)
{
    // A full turn: 2 measured 8 triangles, 10 boundary edges and 6 degenerate triangles.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ClosedRevolveTest_NewMesh());

        GeometryOps::FRevolveParams Params;
        Params.Profile = ClosedRevolveTest_Profile();
        Params.Angle = 360.0;
        Params.Steps = 2;

        const GeometryOps::FOpResult Result =
            GeometryOps::GenerateRevolve(Mesh.Get(), Params, FTransform::Identity);

        TestTrue(TEXT("a 360-degree revolve at steps=2 succeeds"), Result.bSuccess);
        TestEqual(TEXT("its steps is raised to 3"), Params.Steps, 3);
        TestTrue(*FString::Printf(TEXT("and the clamp names itself. warnings: %s"),
            *ClosedRevolveTest_Join(Result.Warnings)),
            ClosedRevolveTest_WarnsClamp(Result.Warnings, TEXT("steps"), 2, 3));
        TestTrue(TEXT("and the revolve comes back closed"), Mesh->GetMeshRef().IsClosed());
    }

    // A partial revolve keeps the engine's own floor. `bCapped` stays at its default true so
    // the shape is the one the verb ships; the assertion is about the count, not the caps.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(ClosedRevolveTest_NewMesh());

        GeometryOps::FRevolveParams Params;
        Params.Profile = ClosedRevolveTest_Profile();
        Params.Angle = 180.0;
        Params.Steps = 2;

        const GeometryOps::FOpResult Result =
            GeometryOps::GenerateRevolve(Mesh.Get(), Params, FTransform::Identity);

        TestTrue(TEXT("a 180-degree revolve at steps=2 succeeds"), Result.bSuccess);
        TestEqual(TEXT("and its steps is left at 2"), Params.Steps, 2);
        TestFalse(*FString::Printf(TEXT("with nothing clamped. warnings: %s"),
            *ClosedRevolveTest_Join(Result.Warnings)),
            ClosedRevolveTest_MentionsLabel(Result.Warnings, TEXT("steps")));
        TestTrue(TEXT("and it still builds geometry"), Result.TrianglesAfter > 0);
    }

    return true;
}
