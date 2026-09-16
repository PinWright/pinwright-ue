// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behavioural coverage for B-physics-asset-create-helper-no-disk-write.
// The setup handler must exercise the shared headless creator, write the new package when
// save:true is requested, and leave the result discoverable through the asset registry.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Tests/TestUtils.h"
#include "Utils/AssetUtils.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    const TCHAR* const GPhysicsPersistenceMannyMeshPath = TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny");

    static TStrongObjectPtr<USkeletalMesh> MakeSavedMeshCopy(
        FAutomationTestBase& Test,
        USkeletalMesh* SourceMesh,
        FString& OutPackagePath,
        FString& OutObjectPath)
    {
        OutPackagePath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/PhysMesh_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString AssetName = FPackageName::GetLongPackageAssetName(OutPackagePath);

        UPackage* Package = CreatePackage(*OutPackagePath);
        Test.TestNotNull(TEXT("temporary skeletal mesh package is created"), Package);
        if (!Package)
        {
            return TStrongObjectPtr<USkeletalMesh>();
        }

        USkeletalMesh* Copy = DuplicateObject<USkeletalMesh>(
            SourceMesh, Package, FName(*AssetName));
        Test.TestNotNull(TEXT("temporary skeletal mesh copy is created"), Copy);
        if (!Copy)
        {
            return TStrongObjectPtr<USkeletalMesh>();
        }

        FAssetRegistryModule::AssetCreated(Copy);
        McpSafeAssetSave(Copy);
        FString PackageName;
        int64 SizeBytes = 0;
        EAssetSaveState SaveState = EAssetSaveState::NotRequested;
        Test.TestTrue(TEXT("temporary skeletal mesh copy is saved"),
            SaveAssetToDiskReportingPresence(
                Copy, /*bForce=*/true, &PackageName, &SizeBytes, &SaveState));

        OutObjectPath = Copy->GetPathName();
        return TStrongObjectPtr<USkeletalMesh>(Copy);
    }

    static bool AssertPhysicsAssetPersistence(
        FAutomationTestBase& Test,
        const FString& Method,
        const TSharedPtr<FJsonObject>& Payload,
        const FString& PackagePath)
    {
        ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

        FTestResponseCapture Capture;
        Test.TestTrue(*FString::Printf(TEXT("%s handler is registered"), *Method),
            InvokeHandlerWithCapture(Method, Payload, Capture));
        Test.TestTrue(*FString::Printf(TEXT("%s responded"), *Method), Capture.bWasCalled);
        Test.TestTrue(*FString::Printf(TEXT("%s succeeds"), *Method), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }

        FString PhysicsAssetPath;
        Test.TestTrue(TEXT("response carries physicsAssetPath"),
            Capture.Result->TryGetStringField(TEXT("physicsAssetPath"), PhysicsAssetPath));
        if (PhysicsAssetPath.IsEmpty())
        {
            return false;
        }

        bool bSaveRequested = false;
        bool bSaved = false;
        bool bSavedToDisk = false;
        bool bPendingFlush = true;
        Test.TestTrue(TEXT("response carries saveRequested"),
            Capture.Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested));
        Test.TestTrue(TEXT("response carries saved"),
            Capture.Result->TryGetBoolField(TEXT("saved"), bSaved));
        Test.TestTrue(TEXT("response carries savedToDisk"),
            Capture.Result->TryGetBoolField(TEXT("savedToDisk"), bSavedToDisk));
        Test.TestTrue(TEXT("response carries pendingFlush"),
            Capture.Result->TryGetBoolField(TEXT("pendingFlush"), bPendingFlush));
        Test.TestTrue(TEXT("save:true is reported as requested"), bSaveRequested);
        Test.TestTrue(TEXT("save:true reports saved"), bSaved);
        Test.TestTrue(TEXT("save:true reports savedToDisk"), bSavedToDisk);
        Test.TestFalse(TEXT("a durable create has no pending flush"), bPendingFlush);

        const FString PackageName = FPackageName::ObjectPathToPackageName(PhysicsAssetPath);
        FString PackageFilename;
        Test.TestTrue(TEXT("physics asset package resolves to a filename"),
            FPackageName::TryConvertLongPackageNameToFilename(
                PackageName, PackageFilename, FPackageName::GetAssetPackageExtension()));
        Test.TestTrue(TEXT("created physics asset .uasset is on disk"),
            !PackageFilename.IsEmpty() && IFileManager::Get().FileSize(*PackageFilename) > 0);

        const FResolvedAsset Resolved = ResolveAsset(PhysicsAssetPath, /*bLoadObject=*/false);
        Test.TestTrue(TEXT("created physics asset is discoverable through the resolver"),
            Resolved.bRegistryOrMemoryExists);
        Test.TestTrue(TEXT("resolver sees the created physics asset registry data"),
            Resolved.AssetData.IsValid());

        const FAssetData RegistryData =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
                .Get().GetAssetByObjectPath(
                    FSoftObjectPath(PhysicsAssetPath),
                    /*bIncludeOnlyOnDiskAssets=*/false);
        Test.TestTrue(TEXT("asset registry resolves the created physics asset"),
            RegistryData.IsValid());

        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetupSimulationPersistsPhysicsAssetTest,
    "PinWright.physics.setup_physics_simulation.PersistenceWritesDiskAndRegistry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhysicsSetupSimulationPersistsPhysicsAssetTest::RunTest(const FString& /*Parameters*/)
{
    FAutomationTestBase& Test = *this;
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(GPhysicsPersistenceMannyMeshPath);

    USkeletalMesh* Mesh = Cast<USkeletalMesh>(
        StaticLoadObject(USkeletalMesh::StaticClass(), nullptr, GPhysicsPersistenceMannyMeshPath));
    Test.TestNotNull(TEXT("the engine Mannequin fixture loads"), Mesh);
    if (!Mesh)
    {
        return false;
    }
    Test.TestNotNull(TEXT("SKM_Manny has render resources for physics generation"),
        Mesh->GetResourceForRendering());
    if (!Mesh->GetResourceForRendering())
    {
        return false;
    }

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SavePath = FString::Printf(TEXT("/Game/__PW_GatewayTests/PhysPersist_%s"), *Guid);
    const FString AssetName = FString::Printf(TEXT("PA_SetupPersist_%s"), *Guid);
    const FString PackagePath = FString::Printf(TEXT("%s/%s"), *SavePath, *AssetName);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("meshPath"), GPhysicsPersistenceMannyMeshPath);
    Payload->SetStringField(TEXT("physicsAssetName"), AssetName);
    Payload->SetStringField(TEXT("savePath"), SavePath);
    Payload->SetBoolField(TEXT("assignToMesh"), false);
    Payload->SetBoolField(TEXT("save"), true);

    return AssertPhysicsAssetPersistence(
        *this, TEXT("physics.setup_physics_simulation"), Payload, PackagePath);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonCreatePhysicsAssetPersistsPhysicsAssetTest,
    "PinWright.skeleton.create_physics_asset.PersistenceWritesDiskAndRegistry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonCreatePhysicsAssetPersistsPhysicsAssetTest::RunTest(const FString& /*Parameters*/)
{
    FAutomationTestBase& Test = *this;
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(GPhysicsPersistenceMannyMeshPath);

    USkeletalMesh* Mesh = Cast<USkeletalMesh>(
        StaticLoadObject(USkeletalMesh::StaticClass(), nullptr, GPhysicsPersistenceMannyMeshPath));
    Test.TestNotNull(TEXT("the engine Mannequin fixture loads"), Mesh);
    if (!Mesh)
    {
        return false;
    }
    Test.TestNotNull(TEXT("SKM_Manny has render resources for physics generation"),
        Mesh->GetResourceForRendering());
    if (!Mesh->GetResourceForRendering())
    {
        return false;
    }

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString OutputPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/PhysSkeletonPersist_%s"), *Guid);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("skeletalMeshPath"), GPhysicsPersistenceMannyMeshPath);
    Payload->SetStringField(TEXT("outputPath"), OutputPath);
    Payload->SetBoolField(TEXT("save"), true);

    return AssertPhysicsAssetPersistence(
        *this, TEXT("skeleton.create_physics_asset"), Payload, OutputPath);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetupSimulationAssignedAssetSurvivesReloadTest,
    "PinWright.physics.setup_physics_simulation.AssignedAssetSurvivesReload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhysicsSetupSimulationAssignedAssetSurvivesReloadTest::RunTest(
    const FString& /*Parameters*/)
{
    FAutomationTestBase& Test = *this;
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(GPhysicsPersistenceMannyMeshPath);

    USkeletalMesh* SourceMesh = Cast<USkeletalMesh>(
        StaticLoadObject(USkeletalMesh::StaticClass(), nullptr, GPhysicsPersistenceMannyMeshPath));
    Test.TestNotNull(TEXT("the engine Mannequin fixture loads"), SourceMesh);
    if (!SourceMesh || !Test.TestNotNull(
        TEXT("SKM_Manny has render resources for physics generation"),
        SourceMesh->GetResourceForRendering()))
    {
        return false;
    }

    FString MeshPackagePath;
    FString MeshObjectPath;
    TStrongObjectPtr<USkeletalMesh> MeshCopyOwner = MakeSavedMeshCopy(
        *this, SourceMesh, MeshPackagePath, MeshObjectPath);
    USkeletalMesh* MeshCopy = MeshCopyOwner.Get();
    if (!MeshCopy)
    {
        CleanupTestAsset(MeshPackagePath);
        return false;
    }
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SavePath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/PhysAssign_%s"), *Guid);
    const FString AssetName = FString::Printf(TEXT("PA_AssignPersist_%s"), *Guid);
    const FString PhysicsPackagePath = FString::Printf(TEXT("%s/%s"), *SavePath, *AssetName);

    ON_SCOPE_EXIT
    {
        MeshCopyOwner.Reset();
        CleanupTestAsset(PhysicsPackagePath);
        CleanupTestAsset(MeshPackagePath);
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("meshPath"), MeshObjectPath);
    Payload->SetStringField(TEXT("physicsAssetName"), AssetName);
    Payload->SetStringField(TEXT("savePath"), SavePath);
    Payload->SetBoolField(TEXT("assignToMesh"), true);
    Payload->SetBoolField(TEXT("save"), true);

    FTestResponseCapture Capture;
    Test.TestTrue(TEXT("setup_physics_simulation handler is registered"),
        InvokeHandlerWithCapture(TEXT("physics.setup_physics_simulation"), Payload, Capture));
    Test.TestTrue(TEXT("assigned setup responds successfully"),
        Capture.bWasCalled && Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    FString PhysicsAssetPath;
    Test.TestTrue(TEXT("assigned response carries physicsAssetPath"),
        Capture.Result->TryGetStringField(TEXT("physicsAssetPath"), PhysicsAssetPath));
    FString EchoedMeshPath;
    Test.TestTrue(TEXT("assigned response carries the copied mesh path"),
        Capture.Result->TryGetStringField(TEXT("meshPath"), EchoedMeshPath));
    if (!EchoedMeshPath.IsEmpty())
    {
        Test.TestEqual(TEXT("assigned response echoes the copied mesh path"),
            EchoedMeshPath, MeshObjectPath);
    }
    bool bSaved = false;
    Test.TestTrue(TEXT("assigned response reports a durable physics asset"),
        Capture.Result->TryGetBoolField(TEXT("saved"), bSaved) && bSaved);

    const TSharedPtr<FJsonObject>* MeshSave = nullptr;
    Test.TestTrue(TEXT("assigned response carries a skeletal mesh save report"),
        Capture.Result->TryGetObjectField(TEXT("skeletalMeshSave"), MeshSave) &&
            MeshSave && MeshSave->IsValid());
    bool bMeshSaved = false;
    if (MeshSave && MeshSave->IsValid())
    {
        Test.TestTrue(TEXT("skeletal mesh save report is durable"),
            (*MeshSave)->TryGetBoolField(TEXT("saved"), bMeshSaved) && bMeshSaved);
    }

    Test.TestTrue(TEXT("physics asset package was written"),
        !PhysicsAssetPath.IsEmpty() && [&]()
        {
            FString Filename;
            const FString PackageName = FPackageName::ObjectPathToPackageName(PhysicsAssetPath);
            return FPackageName::TryConvertLongPackageNameToFilename(
                       PackageName, Filename, FPackageName::GetAssetPackageExtension()) &&
                IFileManager::Get().FileSize(*Filename) > 0;
        }());

    MeshCopyOwner.Reset();
    MeshCopy = nullptr;
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

    USkeletalMesh* ReloadedMesh = LoadObject<USkeletalMesh>(nullptr, *MeshObjectPath);
    Test.TestNotNull(TEXT("saved skeletal mesh reloads"), ReloadedMesh);
    UPhysicsAsset* ReloadedPhysicsAsset = PhysicsAssetPath.IsEmpty()
        ? nullptr
        : LoadObject<UPhysicsAsset>(nullptr, *PhysicsAssetPath);
    Test.TestNotNull(TEXT("saved physics asset reloads"), ReloadedPhysicsAsset);
    UPhysicsAsset* ReloadedAssignedAsset = ReloadedMesh
        ? ReloadedMesh->GetPhysicsAsset()
        : nullptr;
    Test.TestNotNull(TEXT("reloaded mesh carries a physics asset"), ReloadedAssignedAsset);
    if (ReloadedAssignedAsset && ReloadedPhysicsAsset)
    {
        Test.TestEqual(TEXT("reloaded mesh points to the durably saved physics asset"),
            ReloadedAssignedAsset->GetPathName(), ReloadedPhysicsAsset->GetPathName());
    }

    return true;
}
