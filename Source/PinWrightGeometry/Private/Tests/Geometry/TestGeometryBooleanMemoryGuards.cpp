// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the memory-pressure pre-flight on the boolean family.
//
// `trim` and `self_union` shipped without one while `union` / `subtract` / `intersection` had
// it, despite dispatching the same class of allocation: trim IS ApplyMeshBoolean on the same
// two meshes, and self_union is a boolean resolve of a mesh against itself. The gap was
// invisible to the suite because the guard fires only near OOM, which no automation test can
// arrange - so GeometryOps::FScopedBooleanMemoryPressure exists to force the answer. Without
// that seam these three assertions could not be written at all, and a fourth guard could be
// dropped tomorrow with every test still green.
//
// What is asserted, and why each half matters:
//
//  - The guard runs BEFORE the mesh is touched. A caller that trips it keeps exactly the
//    geometry it had, and the before-counts on the failed result are what prove it. FailIn
//    exists for this; a fresh FOpResult::Fail would report 0 -> 0 and lose the evidence.
//  - Under no pressure the same call succeeds. A guard that refuses unconditionally passes the
//    first half and is useless, which is the other way this regresses.
#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOps_Boolean.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "GeometryScript/MeshPrimitiveFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

UDynamicMesh* BooleanMemoryTest_NewMesh()
{
    return NewObject<UDynamicMesh>(GetTransientPackage());
}

// Two overlapping boxes, so every op under test has real work to decline.
UDynamicMesh* BooleanMemoryTest_Box(UDynamicMesh* Mesh, const FVector& Center, double Dimension)
{
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform(Center),
        static_cast<float>(Dimension), static_cast<float>(Dimension), static_cast<float>(Dimension),
        0, 0, 0,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryTrimMemoryGuardTest,
    "PinWright.Geometry.Ops.Boolean.TrimRefusesUnderMemoryPressure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryTrimMemoryGuardTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Target(BooleanMemoryTest_Box(
        BooleanMemoryTest_NewMesh(), FVector::ZeroVector, 100.0));
    TStrongObjectPtr<UDynamicMesh> Tool(BooleanMemoryTest_Box(
        BooleanMemoryTest_NewMesh(), FVector(40, 0, 0), 100.0));

    const int32 TrianglesBefore = Target->GetTriangleCount();

    GeometryOps::FTrimParams Params;

    {
        GeometryOps::FScopedBooleanMemoryPressure Pressure;

        const GeometryOps::FOpResult Result = GeometryOps::Trim(
            Target.Get(), FTransform::Identity, Tool.Get(), FTransform::Identity, Params);

        TestFalse(TEXT("trim refuses under memory pressure"), Result.bSuccess);
        TestEqual(TEXT("and answers the family's code"), Result.ErrorCode,
            FString(ErrorCodes::ERR_MEMORY_PRESSURE));

        // Untouched, and the failed result still carries the before-count that says so.
        TestEqual(TEXT("the target keeps its geometry"), Target->GetTriangleCount(),
            TrianglesBefore);
        TestEqual(TEXT("and the refusal reports the count it protected"), Result.TrianglesBefore,
            TrianglesBefore);
    }

    // The control. Same meshes, same params, no override.
    const GeometryOps::FOpResult Allowed = GeometryOps::Trim(
        Target.Get(), FTransform::Identity, Tool.Get(), FTransform::Identity, Params);

    TestTrue(*FString::Printf(TEXT("the same trim runs with no pressure. %s: %s"),
        *Allowed.ErrorCode, *Allowed.ErrorMessage), Allowed.bSuccess);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySelfUnionMemoryGuardTest,
    "PinWright.Geometry.Ops.Boolean.SelfUnionRefusesUnderMemoryPressure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySelfUnionMemoryGuardTest::RunTest(const FString& Parameters)
{
    // One mesh carrying two intersecting boxes - the shape self_union exists to resolve.
    TStrongObjectPtr<UDynamicMesh> Mesh(BooleanMemoryTest_NewMesh());
    BooleanMemoryTest_Box(Mesh.Get(), FVector::ZeroVector, 100.0);
    BooleanMemoryTest_Box(Mesh.Get(), FVector(40, 0, 0), 100.0);

    const int32 TrianglesBefore = Mesh->GetTriangleCount();

    GeometryOps::FSelfUnionParams Params;

    {
        GeometryOps::FScopedBooleanMemoryPressure Pressure;

        const GeometryOps::FOpResult Result = GeometryOps::SelfUnion(Mesh.Get(), Params);

        TestFalse(TEXT("self_union refuses under memory pressure"), Result.bSuccess);
        TestEqual(TEXT("and answers the family's code"), Result.ErrorCode,
            FString(ErrorCodes::ERR_MEMORY_PRESSURE));
        TestEqual(TEXT("the mesh keeps its geometry"), Mesh->GetTriangleCount(), TrianglesBefore);
        TestEqual(TEXT("and the refusal reports the count it protected"), Result.TrianglesBefore,
            TrianglesBefore);
    }

    const GeometryOps::FOpResult Allowed = GeometryOps::SelfUnion(Mesh.Get(), Params);

    TestTrue(*FString::Printf(TEXT("the same self_union runs with no pressure. %s: %s"),
        *Allowed.ErrorCode, *Allowed.ErrorMessage), Allowed.bSuccess);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBooleanMemorySeamTest,
    "PinWright.Geometry.Ops.Boolean.MemoryPressureSeamCoversTheWholeFamily",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryBooleanMemorySeamTest::RunTest(const FString& Parameters)
{
    // The seam has to reach the three guards that were already correct, or the two tests above
    // are proving something about a helper only they call.
    TestTrue(TEXT("no pressure by default on a build machine"),
        GeometryOps::BooleanMemoryPressureSafe());

    {
        GeometryOps::FScopedBooleanMemoryPressure Outer;
        TestFalse(TEXT("the override takes effect"), GeometryOps::BooleanMemoryPressureSafe());

        {
            // Nesting counts: an inner scope closing must not lift the outer one's override.
            GeometryOps::FScopedBooleanMemoryPressure Inner;
            TestFalse(TEXT("nested overrides hold"), GeometryOps::BooleanMemoryPressureSafe());
        }

        TestFalse(TEXT("and the outer override survives the inner one"),
            GeometryOps::BooleanMemoryPressureSafe());

        TStrongObjectPtr<UDynamicMesh> Target(BooleanMemoryTest_Box(
            BooleanMemoryTest_NewMesh(), FVector::ZeroVector, 100.0));
        TStrongObjectPtr<UDynamicMesh> Tool(BooleanMemoryTest_Box(
            BooleanMemoryTest_NewMesh(), FVector(40, 0, 0), 100.0));

        const GeometryOps::FOpResult Result = GeometryOps::Boolean(
            Target.Get(), FTransform::Identity, Tool.Get(), FTransform::Identity,
            EGeometryScriptBooleanOperation::Union, GeometryOps::FBooleanParams());

        TestFalse(TEXT("the symmetric booleans read the same seam"), Result.bSuccess);
        TestEqual(TEXT("with the same code"), Result.ErrorCode,
            FString(ErrorCodes::ERR_MEMORY_PRESSURE));
    }

    TestTrue(TEXT("and the override is gone when the scope ends"),
        GeometryOps::BooleanMemoryPressureSafe());

    return true;
}
