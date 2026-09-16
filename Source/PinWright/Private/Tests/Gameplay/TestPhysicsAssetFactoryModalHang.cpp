// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-physics-asset-factory-modal-hang.
//
// Both physics.setup_physics_simulation (mesh-backed) and skeleton.create_physics_asset
// (mesh-backed) used to hand a real USkeletalMesh to UPhysicsAssetFactory on the game
// thread. With TargetSkeletalMesh set, the engine factory's FactoryCreateNew ->
// CreatePhysicsAssetFromMesh -> OpenNewBodyDlg unconditionally opens the interactive
// "New Physics Asset" body-generation modal via GEditor->EditorAddModalWindow
// (PhysicsAssetFactory.cpp:85/122, PhysicsAssetEditorSharedData.cpp:3558). On a normal
// interactive MCP editor that modal blocks the game thread forever — no asset, no error,
// every later RPC times out, OS-kill required.
//
// The fix bypasses the factory entirely and drives the non-interactive core the factory
// itself runs after the modal returns Ok: NewObject<UPhysicsAsset> +
// FPhysicsAssetUtils::CreateFromSkeletalMesh.
//
// These tests exercise the production handlers through InvokeHandlerWithCapture against
// the engine Mannequin mesh (the exact asset shape the repro hung on) and assert a
// physics asset with auto-generated bodies is produced headlessly. Counterfactual: under
// the automation harness's -unattended run, GIsRunningUnattendedScript suppresses the
// modal, so the reverted UPhysicsAssetFactory path leaves NewBodyResponse != Ok and
// FactoryCreateNew returns nullptr -> ASSET_CREATION_FAILED / CREATE_FAILED -> bSuccess
// would be false and these assertions would fail.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "UObject/UObjectGlobals.h"
#include "Tests/TestUtils.h"

namespace
{
    // The mesh the ticket repro hung on. Package-path form (StaticLoadObject /
    // UEditorAssetLibrary resolve the trailing asset name).
    const TCHAR* const GMannyMeshPath = TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny");

    // Loads the Manny skeletal mesh fixture and confirms it carries the render resources
    // FPhysicsAssetUtils::CreateFromSkeletalMesh requires (it check()s GetResourceForRendering
    // internally). Callers gate host absence with PINWRIGHT_SKIP_IF_FIXTURE_MISSING before
    // calling this, so by the time we load here the package exists — a load failure or a mesh
    // stripped of render data is a hard FAILURE (a broken content baseline can't mask a
    // reverted fix). Under the mandated -RenderOffScreen run a real RHI builds these resources.
    static USkeletalMesh* PhysAssetModalHang_LoadRenderableManny(FAutomationTestBase& Test)
    {
        USkeletalMesh* Mesh = Cast<USkeletalMesh>(
            StaticLoadObject(USkeletalMesh::StaticClass(), nullptr, GMannyMeshPath));
        Test.TestNotNull(TEXT("the engine Mannequin fixture loads"), Mesh);
        if (Mesh)
        {
            Test.TestNotNull(TEXT("SKM_Manny has render resources for physics generation"),
                Mesh->GetResourceForRendering());
        }
        return Mesh;
    }
}

// physics.setup_physics_simulation: a mesh-backed call must create a real UPhysicsAsset
// with auto-generated bodies headlessly (assignToMesh:false leaves the shared mesh
// untouched), instead of wedging on the factory's body-generation modal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetupSimulationMeshBackedCreatesBodiesHeadlessTest,
    "PinWright.physics.setup_physics_simulation.MeshBackedCreatesBodiesHeadless",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhysicsSetupSimulationMeshBackedCreatesBodiesHeadlessTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(GMannyMeshPath);

    USkeletalMesh* Mesh = PhysAssetModalHang_LoadRenderableManny(*this);
    if (!Mesh || !Mesh->GetResourceForRendering())
    {
        return false; // fixture / RHI failure already recorded; do not proceed into a check()
    }

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SavePath = FString::Printf(TEXT("/Game/__PW_GatewayTests/PhysHang_%s"), *Guid);
    const FString AssetName = FString::Printf(TEXT("PA_SetupSim_%s"), *Guid);
    const FString PackagePath = FString::Printf(TEXT("%s/%s"), *SavePath, *AssetName);
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("meshPath"), GMannyMeshPath);
    Payload->SetStringField(TEXT("physicsAssetName"), AssetName);
    Payload->SetStringField(TEXT("savePath"), SavePath);
    Payload->SetBoolField(TEXT("assignToMesh"), false);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("setup_physics_simulation handler registered"),
        InvokeHandlerWithCapture(TEXT("physics.setup_physics_simulation"), Payload, Capture));

    // Core guarantee: the mesh-backed call completes headlessly (no modal, no hang) and
    // reports success. The reverted factory path returns nullptr under -unattended, so the
    // handler would send ASSET_CREATION_FAILED and bSuccess would be false here.
    TestTrue(TEXT("mesh-backed setup_physics_simulation succeeds headlessly"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    FString PhysicsAssetPath;
    TestTrue(TEXT("response carries physicsAssetPath"),
        Capture.Result->TryGetStringField(TEXT("physicsAssetPath"), PhysicsAssetPath));

    // Prove auto-generation actually produced bodies (the work the modal used to gate),
    // not just an empty asset shell.
    if (!PhysicsAssetPath.IsEmpty())
    {
        UPhysicsAsset* Created = Cast<UPhysicsAsset>(
            StaticLoadObject(UPhysicsAsset::StaticClass(), nullptr, *PhysicsAssetPath));
        TestNotNull(TEXT("created physics asset resolves"), Created);
        if (Created)
        {
            TestTrue(TEXT("generated physics asset has at least one body"),
                Created->SkeletalBodySetups.Num() > 0);
        }
    }

    return true;
}

// skeleton.create_physics_asset (mesh-backed): same guarantee via the sibling verb, which
// reports bodyCount directly. McpSafeAssetSave only marks the mesh dirty (no disk write),
// so the shared SKM_Manny content is not mutated on disk.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreatePhysicsAssetMeshBackedCreatesBodiesHeadlessTest,
    "PinWright.skeleton.create_physics_asset.MeshBackedCreatesBodiesHeadless",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCreatePhysicsAssetMeshBackedCreatesBodiesHeadlessTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(GMannyMeshPath);

    USkeletalMesh* Mesh = PhysAssetModalHang_LoadRenderableManny(*this);
    if (!Mesh || !Mesh->GetResourceForRendering())
    {
        return false;
    }

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString OutputPath = FString::Printf(TEXT("/Game/__PW_GatewayTests/PA_CreatePA_%s"), *Guid);
    ON_SCOPE_EXIT { CleanupTestAsset(OutputPath); };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("skeletalMeshPath"), GMannyMeshPath);
    Payload->SetStringField(TEXT("outputPath"), OutputPath);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("create_physics_asset handler registered"),
        InvokeHandlerWithCapture(TEXT("skeleton.create_physics_asset"), Payload, Capture));

    // The reverted Factory->FactoryCreateNew returns nullptr under -unattended -> CREATE_FAILED.
    TestTrue(TEXT("mesh-backed create_physics_asset succeeds headlessly"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    // The mesh-backed branch reports bodyCount directly; auto-generation must yield bodies.
    double BodyCount = 0.0;
    TestTrue(TEXT("response carries bodyCount"),
        Capture.Result->TryGetNumberField(TEXT("bodyCount"), BodyCount));
    TestTrue(TEXT("generated physics asset has at least one body"), BodyCount > 0.0);

    // skeletalMeshPath is emitted only by the mesh-backed branch (the bare-skeleton branch
    // emits skeletonPath + fromSkeleton), confirming we exercised the fixed factory-free
    // mesh path, not the pre-existing skeleton path.
    FString EchoedMeshPath;
    TestTrue(TEXT("mesh-backed branch echoes skeletalMeshPath"),
        Capture.Result->TryGetStringField(TEXT("skeletalMeshPath"), EchoedMeshPath));

    return true;
}
