// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Physics domain handlers
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "PhysicsEngine/PhysicsAsset.h"
#if __has_include("PhysicsEngine/SkeletalBodySetup.h")
#include "PhysicsEngine/SkeletalBodySetup.h"
#endif
#include "UObject/Package.h"
#include "Misc/Guid.h"


// ============================================================================
// physics.setup_physics_simulation — ValidParamsNoCrash: empty payload (all optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetupPhysicsSimulationEmptyPayloadNoCrashTest,
    "PinWright.physics.setup_physics_simulation.EmptyPayloadNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhysicsSetupPhysicsSimulationEmptyPayloadNoCrashTest::RunTest(const FString& Parameters)
{
    // All params are optional; an empty payload must not crash
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

    TestTrue(TEXT("setup_physics_simulation handler found and invoked"),
        InvokeHandler(TEXT("physics.setup_physics_simulation"), Payload));
    return true;
}

// ============================================================================
// physics.setup_physics_simulation — ValidParamsNoCrash: meshPath provided
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetupPhysicsSimulationWithMeshPathNoCrashTest,
    "PinWright.physics.setup_physics_simulation.WithMeshPathNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhysicsSetupPhysicsSimulationWithMeshPathNoCrashTest::RunTest(const FString& Parameters)
{
    // Non-existent mesh path exercises the asset-not-found error branch without crashing
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("meshPath"), TEXT("/Game/DoesNotExist/TestMesh"));
    Payload->SetStringField(TEXT("physicsAssetName"), TEXT("TestMesh_Physics"));

    TestTrue(TEXT("setup_physics_simulation handler found and invoked"),
        InvokeHandler(TEXT("physics.setup_physics_simulation"), Payload));
    return true;
}

// ============================================================================
// physics.setup_physics_simulation — ValidParamsNoCrash: actorName provided
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetupPhysicsSimulationWithActorNameNoCrashTest,
    "PinWright.physics.setup_physics_simulation.WithActorNameNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhysicsSetupPhysicsSimulationWithActorNameNoCrashTest::RunTest(const FString& Parameters)
{
    // actorName path exercises the actor-lookup branch; actor won't exist so it returns an error response
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor_Physics"));

    TestTrue(TEXT("setup_physics_simulation handler found and invoked"),
        InvokeHandler(TEXT("physics.setup_physics_simulation"), Payload));
    return true;
}

// ============================================================================
// physics.setup_ragdoll — ValidParamsNoCrash: actorName supplied (actor absent)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetupRagdollValidParamsNoCrashTest,
    "PinWright.physics.setup_ragdoll.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhysicsSetupRagdollValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // actorName is present but the actor won't exist; handler returns ACTOR_NOT_FOUND without crashing
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentRagdollActor"));

    TestTrue(TEXT("setup_ragdoll handler found and invoked"),
        InvokeHandler(TEXT("physics.setup_ragdoll"), Payload));
    return true;
}

// ============================================================================
// physics.setup_ragdoll — ValidParamsNoCrash: optional blendWeight included
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetupRagdollWithBlendWeightNoCrashTest,
    "PinWright.physics.setup_ragdoll.WithBlendWeightNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhysicsSetupRagdollWithBlendWeightNoCrashTest::RunTest(const FString& Parameters)
{
    // blendWeight is optional; verify the handler parses it without crashing
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentRagdollActor"));
    Payload->SetNumberField(TEXT("blendWeight"), 0.5);

    TestTrue(TEXT("setup_ragdoll handler found and invoked"),
        InvokeHandler(TEXT("physics.setup_ragdoll"), Payload));
    return true;
}

// ============================================================================
// physics.setup_ragdoll — ValidParamsNoCrash: invalid skeletonPath triggers asset-not-found
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetupRagdollInvalidSkeletonPathNoCrashTest,
    "PinWright.physics.setup_ragdoll.InvalidSkeletonPathNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhysicsSetupRagdollInvalidSkeletonPathNoCrashTest::RunTest(const FString& Parameters)
{
    // A non-existent skeletonPath should return ASSET_NOT_FOUND without crashing
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("SomeActor"));
    Payload->SetStringField(TEXT("skeletonPath"), TEXT("/Game/DoesNotExist/MySkeleton"));

    TestTrue(TEXT("setup_ragdoll handler found and invoked"),
        InvokeHandler(TEXT("physics.setup_ragdoll"), Payload));
    return true;
}

// ============================================================================
// physics.activate_ragdoll — ValidParamsNoCrash: actorName supplied, activate true
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsActivateRagdollActivateTrueNoCrashTest,
    "PinWright.physics.activate_ragdoll.ActivateTrueNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhysicsActivateRagdollActivateTrueNoCrashTest::RunTest(const FString& Parameters)
{
    // activate=true is the default path; actor won't exist so handler returns ACTOR_NOT_FOUND
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentRagdollTarget"));
    Payload->SetBoolField(TEXT("activate"), true);

    TestTrue(TEXT("activate_ragdoll handler found and invoked"),
        InvokeHandler(TEXT("physics.activate_ragdoll"), Payload));
    return true;
}

// ============================================================================
// physics.activate_ragdoll — ValidParamsNoCrash: activate false (deactivate branch)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsActivateRagdollActivateFalseNoCrashTest,
    "PinWright.physics.activate_ragdoll.ActivateFalseNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhysicsActivateRagdollActivateFalseNoCrashTest::RunTest(const FString& Parameters)
{
    // activate=false exercises the deactivation code path; actor absent returns ACTOR_NOT_FOUND
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentRagdollTarget"));
    Payload->SetBoolField(TEXT("activate"), false);

    TestTrue(TEXT("activate_ragdoll handler found and invoked"),
        InvokeHandler(TEXT("physics.activate_ragdoll"), Payload));
    return true;
}

// ============================================================================
// skeleton.list_physics_bodies — limit / namesOnly / boneName narrowing
// (board E-list-physics-bodies-no-limit-spills)
//
// Builds a real on-disk UPhysicsAsset with three named bodies so the handler's
// path-based StaticLoadObject resolves it, then drives the production handler and
// asserts: (1) the default un-narrowed call returns all three rows with the full
// per-body shape and totalCount==count / truncated==false; (2) limit=2 caps the
// returned rows at 2 while totalCount stays 3 and truncated flips true;
// (3) namesOnly drops the primitive-count fields, leaving only boneName;
// (4) a boneName substring filter returns only the matching body. Reverting the
// limit/projection/filter plumbing fails this test.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonListPhysicsBodiesNarrowingTest,
    "PinWright.skeleton.list_physics_bodies.Narrowing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonListPhysicsBodiesNarrowingTest::RunTest(const FString& Parameters)
{
    // A real package on disk: the handler resolves the asset by path via
    // StaticLoadObject, which a transient NewObject would not satisfy. GUID-suffixed
    // so parallel/repeat runs never collide.
    const FString PkgPath = FString::Printf(TEXT("/Game/PinWrightTest_PA_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString AssetName = FPackageName::GetLongPackageAssetName(PkgPath);

    UPackage* Package = CreatePackage(*PkgPath);
    TestNotNull(TEXT("test package created"), Package);
    if (!Package) return false;
    ON_SCOPE_EXIT { CleanupTestAsset(PkgPath); };

    UPhysicsAsset* PhysicsAsset = NewObject<UPhysicsAsset>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    TestNotNull(TEXT("physics asset created"), PhysicsAsset);
    if (!PhysicsAsset) return false;

    // Three bodies with distinct bone names; one ("L_Foot") is the filter target.
    const TCHAR* BoneNames[] = {TEXT("Pelvis"), TEXT("Spine"), TEXT("L_Foot")};
    for (const TCHAR* Bone : BoneNames)
    {
        USkeletalBodySetup* Body = NewObject<USkeletalBodySetup>(PhysicsAsset);
        Body->BoneName = FName(Bone);
        PhysicsAsset->SkeletalBodySetups.Add(Body);
    }
    const FString PhysicsAssetPath = PhysicsAsset->GetPathName();

    // (1) Default: all three rows, full per-body shape, no truncation.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("physicsAssetPath"), PhysicsAssetPath);

        FTestResponseCapture Capture;
        TestTrue(TEXT("list_physics_bodies handler found (default)"),
            InvokeHandlerWithCapture(TEXT("skeleton.list_physics_bodies"), Payload, Capture));
        TestTrue(TEXT("default call succeeds"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Bodies = nullptr;
            TestTrue(TEXT("physicsBodies array present"),
                Capture.Result->TryGetArrayField(TEXT("physicsBodies"), Bodies));
            if (Bodies)
            {
                TestEqual(TEXT("default returns all 3 bodies"), Bodies->Num(), 3);
            }
            double Count = 0, TotalCount = 0;
            Capture.Result->TryGetNumberField(TEXT("count"), Count);
            Capture.Result->TryGetNumberField(TEXT("totalCount"), TotalCount);
            TestEqual(TEXT("count==3"), (int32)Count, 3);
            TestEqual(TEXT("totalCount==3"), (int32)TotalCount, 3);
            bool bTruncated = true;
            Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
            TestFalse(TEXT("default not truncated"), bTruncated);

            // Full per-body shape is preserved by default (primitive counts present).
            if (Bodies && Bodies->Num() > 0)
            {
                const TSharedPtr<FJsonObject>* First = nullptr;
                if ((*Bodies)[0]->TryGetObject(First) && First)
                {
                    double Sphere = -1;
                    TestTrue(TEXT("default row carries sphereCount"),
                        (*First)->TryGetNumberField(TEXT("sphereCount"), Sphere));
                }
            }
        }
    }

    // (2) limit=2 caps returned rows but totalCount/truncated expose the elision.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("physicsAssetPath"), PhysicsAssetPath);
        Payload->SetNumberField(TEXT("limit"), 2);

        FTestResponseCapture Capture;
        TestTrue(TEXT("list_physics_bodies handler found (limit)"),
            InvokeHandlerWithCapture(TEXT("skeleton.list_physics_bodies"), Payload, Capture));
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Bodies = nullptr;
            Capture.Result->TryGetArrayField(TEXT("physicsBodies"), Bodies);
            if (Bodies)
            {
                TestEqual(TEXT("limit=2 returns 2 rows"), Bodies->Num(), 2);
            }
            double Count = 0, TotalCount = 0;
            Capture.Result->TryGetNumberField(TEXT("count"), Count);
            Capture.Result->TryGetNumberField(TEXT("totalCount"), TotalCount);
            TestEqual(TEXT("limited count==2"), (int32)Count, 2);
            TestEqual(TEXT("limited totalCount stays 3"), (int32)TotalCount, 3);
            bool bTruncated = false;
            Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
            TestTrue(TEXT("limited result is truncated"), bTruncated);
        }
    }

    // (3) namesOnly drops the primitive-count fields, leaving boneName.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("physicsAssetPath"), PhysicsAssetPath);
        Payload->SetBoolField(TEXT("namesOnly"), true);

        FTestResponseCapture Capture;
        TestTrue(TEXT("list_physics_bodies handler found (namesOnly)"),
            InvokeHandlerWithCapture(TEXT("skeleton.list_physics_bodies"), Payload, Capture));
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Bodies = nullptr;
            Capture.Result->TryGetArrayField(TEXT("physicsBodies"), Bodies);
            if (Bodies && Bodies->Num() > 0)
            {
                const TSharedPtr<FJsonObject>* First = nullptr;
                if ((*Bodies)[0]->TryGetObject(First) && First)
                {
                    FString BoneName;
                    TestTrue(TEXT("namesOnly row keeps boneName"),
                        (*First)->TryGetStringField(TEXT("boneName"), BoneName));
                    double Sphere = 0;
                    TestFalse(TEXT("namesOnly row drops sphereCount"),
                        (*First)->TryGetNumberField(TEXT("sphereCount"), Sphere));
                    FString Collision;
                    TestFalse(TEXT("namesOnly row drops collisionType"),
                        (*First)->TryGetStringField(TEXT("collisionType"), Collision));
                }
            }
        }
    }

    // (4) boneName substring filter returns only the matching body.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("physicsAssetPath"), PhysicsAssetPath);
        Payload->SetStringField(TEXT("boneName"), TEXT("Foot"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("list_physics_bodies handler found (filter)"),
            InvokeHandlerWithCapture(TEXT("skeleton.list_physics_bodies"), Payload, Capture));
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Bodies = nullptr;
            Capture.Result->TryGetArrayField(TEXT("physicsBodies"), Bodies);
            if (Bodies)
            {
                TestEqual(TEXT("filter returns 1 body"), Bodies->Num(), 1);
            }
            double TotalCount = 0;
            Capture.Result->TryGetNumberField(TEXT("totalCount"), TotalCount);
            TestEqual(TEXT("filtered totalCount==1"), (int32)TotalCount, 1);
            if (Bodies && Bodies->Num() == 1)
            {
                const TSharedPtr<FJsonObject>* First = nullptr;
                if ((*Bodies)[0]->TryGetObject(First) && First)
                {
                    FString BoneName;
                    (*First)->TryGetStringField(TEXT("boneName"), BoneName);
                    TestEqual(TEXT("filtered body is L_Foot"), BoneName, FString(TEXT("L_Foot")));
                }
            }
        }
    }

    return true;
}
