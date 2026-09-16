// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit coverage for the actor.duplicate dynamic-mesh integrity check (B2).
//
// Defect being guarded: UDynamicMesh::ImportCustomProperties — the T3D-paste half of
// UEditorActorSubsystem::DuplicateActor — substitutes a 12-triangle 50-unit box when it
// cannot recover the source mesh and still reports SUCCESS (UDynamicMesh.cpp:568 on 5.8,
// :529 on 5.3). Only a transient editor toast and a LogGeometry warning signal it, so an
// RPC caller previously received a "successful" duplicate holding a cube.
//
// actor.duplicate now compares the source's and the copy's dynamic-mesh triangle counts
// via DynamicMeshCountProbe and refuses the mismatch. The end-to-end half of this
// coverage — spawning a real ADynamicMeshActor and dispatching actor.duplicate — lives in
// the geometry sub-module (Source/PinWrightGeometry/Private/Tests/Geometry/
// TestActorDuplicateMeshIntegrity.cpp), because the main module deliberately does not link
// GeometryFramework. What is testable here is the pure classifier truth table and the
// probe's fail-safe behaviour, both of which must hold on every host.
#include "Misc/AutomationTest.h"

#include "Handlers/Actor/DynamicMeshCountProbe.h"
#include "PinWrightHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestWorldUtils.h"

#include "Engine/StaticMeshActor.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

// Verdicts are written out fully qualified rather than pulled in with a file-scope
// `using`: Unity merges these TUs, so a using-declaration here would leak into every
// other test file in the same blob.

// The classifier is the whole decision surface of the fix, so its table is pinned
// exhaustively: "no verdict without both counts" is what keeps every non-dynamic-mesh
// actor on its pre-existing code path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDuplicateMeshClassifierTableTest,
    "PinWright.actor.duplicate.MeshVerdictClassifierTable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDuplicateMeshClassifierTableTest::RunTest(const FString& Parameters)
{
    // Neither side probed, and each one-sided case: a single missing count must never
    // be read as evidence of substitution.
    TestTrue(TEXT("neither probed -> NotProbed"),
        DynamicMeshCountProbe::ClassifyDuplicate(false, false, 0, 0) ==
            DynamicMeshCountProbe::EDuplicateMeshVerdict::NotProbed);
    TestTrue(TEXT("source not probed -> NotProbed"),
        DynamicMeshCountProbe::ClassifyDuplicate(false, true, 400000, 12) ==
            DynamicMeshCountProbe::EDuplicateMeshVerdict::NotProbed);
    TestTrue(TEXT("duplicate not probed -> NotProbed"),
        DynamicMeshCountProbe::ClassifyDuplicate(true, false, 400000, 0) ==
            DynamicMeshCountProbe::EDuplicateMeshVerdict::NotProbed);

    // Equal counts on both sides: the copy is real.
    TestTrue(TEXT("equal counts -> Ok"),
        DynamicMeshCountProbe::ClassifyDuplicate(true, true, 400000, 400000) ==
            DynamicMeshCountProbe::EDuplicateMeshVerdict::Ok);
    TestTrue(TEXT("equal zero counts -> Ok"),
        DynamicMeshCountProbe::ClassifyDuplicate(true, true, 0, 0) ==
            DynamicMeshCountProbe::EDuplicateMeshVerdict::Ok);

    // 12 is the exact signature of FMinimalBoxMeshGenerator's placeholder cube.
    TestTrue(TEXT("400000 -> 12 (placeholder cube) -> Substituted"),
        DynamicMeshCountProbe::ClassifyDuplicate(true, true, 400000, 12) ==
            DynamicMeshCountProbe::EDuplicateMeshVerdict::Substituted);
    // Any inequality counts, including a copy that somehow grew: the check is
    // "identical or nothing", not "smaller".
    TestTrue(TEXT("any inequality -> Substituted"),
        DynamicMeshCountProbe::ClassifyDuplicate(true, true, 12, 400000) ==
            DynamicMeshCountProbe::EDuplicateMeshVerdict::Substituted);

    return true;
}

// The no-regression guarantee for every actor that has no dynamic mesh component: the
// probe must decline to answer, which is what keeps actor.duplicate's response shape and
// behaviour byte-identical for them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDuplicateMeshProbeIgnoresNonDynamicMeshActorTest,
    "PinWright.actor.duplicate.MeshProbeIgnoresNonDynamicMeshActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDuplicateMeshProbeIgnoresNonDynamicMeshActorTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    // A null actor must not crash the probe: actor.duplicate calls it before the engine
    // has produced anything.
    int64 NullActorTriangles = -1;
    TestFalse(TEXT("null actor does not probe"),
        DynamicMeshCountProbe::TryGetTotalTriangleCount(nullptr, NullActorTriangles));
    TestTrue(TEXT("null actor leaves the out param untouched"), NullActorTriangles == -1);

    const FString Label = FString::Printf(TEXT("PW_DupMeshProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = SpawnActorInActiveWorld<AStaticMeshActor>(
        AStaticMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Label);
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("No editor world available; skipping static mesh actor probe case"));
        return true;
    }
    int64 Triangles = -1;
    TestFalse(TEXT("plain AStaticMeshActor does not probe"),
        DynamicMeshCountProbe::TryGetTotalTriangleCount(Actor, Triangles));
    TestTrue(TEXT("static mesh actor leaves the out param untouched"), Triangles == -1);

    return true;
}

// Drift canary for the reflection chain. The probe resolves everything by name because
// the main module does not link GeometryFramework; if Epic renames MeshObject or drops
// the GetTriangleCount UFUNCTION, the probe silently degrades to "never probed" and
// actor.duplicate quietly stops catching the substitution. This test makes that
// degradation loud on a host that HAS the plugin, and asserts the fail-safe on a host
// that does not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDuplicateMeshProbeReflectionChainTest,
    "PinWright.actor.duplicate.MeshProbeReflectionChainIntact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDuplicateMeshProbeReflectionChainTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    // Exactly the lookup the probe performs — FindObject, so a host with the
    // GeometryScripting plugin disabled is not forced to load GeometryFramework.
    UClass* DMCClass =
        FindObject<UClass>(nullptr, TEXT("/Script/GeometryFramework.DynamicMeshComponent"));

    if (!DMCClass)
    {
        // GeometryFramework absent: every probe must be a no-op so this host behaves
        // exactly as it did before the integrity check existed.
        const FString Label = FString::Printf(TEXT("PW_DupMeshNoGeo_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        AActor* Actor = SpawnActorInActiveWorld<AActor>(
            AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Label);
        if (Actor)
        {
            int64 Triangles = -1;
            TestFalse(TEXT("probe declines when GeometryFramework is not loaded"),
                DynamicMeshCountProbe::TryGetTotalTriangleCount(Actor, Triangles));
        }
        PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
            TEXT("GeometryFramework not loaded on this host; asserted the probe's "
                         "fail-safe path only"));
        return true;
    }

    // MeshObject is UPROPERTY(Instanced) TObjectPtr<UDynamicMesh> on 5.3-5.8
    // (DynamicMeshComponent.h:259 on 5.8, :129 on 5.3). GetDynamicMesh() is deliberately
    // NOT used: its UFUNCTION specifier is commented out, so it is not callable here.
    TestNotNull(TEXT("UDynamicMeshComponent still exposes a MeshObject FObjectProperty"),
        CastField<FObjectProperty>(DMCClass->FindPropertyByName(TEXT("MeshObject"))));

    UClass* DynamicMeshClass =
        FindObject<UClass>(nullptr, TEXT("/Script/GeometryFramework.DynamicMesh"));
    if (TestNotNull(TEXT("UDynamicMesh class resolves by path"), DynamicMeshClass))
    {
        UFunction* Fn = DynamicMeshClass->FindFunctionByName(TEXT("GetTriangleCount"));
        if (TestNotNull(TEXT("UDynamicMesh::GetTriangleCount is still a UFUNCTION"), Fn))
        {
            // The probe allocates Fn->ParmsSize and reads the return parameter back; an
            // added input parameter would make it bail rather than guess a value.
            TestTrue(TEXT("GetTriangleCount still returns a value"),
                Fn->GetReturnProperty() != nullptr);
            TestNotNull(TEXT("GetTriangleCount's return parameter is still an int"),
                CastField<FIntProperty>(Fn->GetReturnProperty()));
        }
    }

    return true;
}
