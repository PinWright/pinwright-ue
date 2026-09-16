// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behavioural coverage for blueprint.create's durable-save contract. The handler must
// write before responding, expose the measured save state, register the asset, and leave
// a Blueprint that survives asset.reload rather than only existing in resident memory.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dispatch/SafePoint.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Tests/TestUtils.h"
#include "UObject/SoftObjectPath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCreatePersistenceTest,
    "PinWright.blueprint.create.PersistenceWritesDiskRegistryAndColdReload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintCreatePersistenceTest::RunTest(const FString& /*Parameters*/)
{
    TestTrue(TEXT("blueprint.create is routed outside UWorld::Tick"),
        PinWrightSafePoint::IsTickUnsafeMethod(TEXT("blueprint.create")));

    const FString AssetName = FString::Printf(TEXT("BP_CreatePersistence_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString FolderPath = TEXT("/Game/PinWrightTests");
    const FString PackagePath = FString::Printf(TEXT("%s/%s"), *FolderPath, *AssetName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("savePath"), FolderPath);
    Payload->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Pawn"));
    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.create handler is registered"),
        InvokeHandlerWithCapture(TEXT("blueprint.create"), Payload, Capture));
    TestTrue(TEXT("blueprint.create responds"), Capture.bWasCalled);
    TestTrue(FString::Printf(TEXT("blueprint.create succeeds (err='%s' msg='%s')"),
        *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    bool bSaveRequested = false;
    bool bSaved = false;
    bool bExistsOnDisk = false;
    FString SaveState;
    TestTrue(TEXT("response carries saveRequested"),
        Capture.Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested));
    TestTrue(TEXT("blueprint.create requests persistence"), bSaveRequested);
    TestTrue(TEXT("response carries saved"),
        Capture.Result->TryGetBoolField(TEXT("saved"), bSaved));
    TestTrue(TEXT("blueprint.create reports measured durability"), bSaved);
    TestTrue(TEXT("response carries saveState"),
        Capture.Result->TryGetStringField(TEXT("saveState"), SaveState));
    TestEqual(TEXT("new Blueprint reports a disk write"), SaveState, FString(TEXT("written")));
    TestTrue(TEXT("response carries existsOnDisk"),
        Capture.Result->TryGetBoolField(TEXT("existsOnDisk"), bExistsOnDisk));
    TestTrue(TEXT("response confirms the package exists on disk"), bExistsOnDisk);
    TestFalse(TEXT("durable create has no pendingFlush"),
        Capture.Result->HasField(TEXT("pendingFlush")));
    TestFalse(TEXT("durable create has no pendingSave"),
        Capture.Result->HasField(TEXT("pendingSave")));

    FString PackageFilename;
    TestTrue(TEXT("Blueprint package resolves to a filename"),
        FPackageName::TryConvertLongPackageNameToFilename(
            PackagePath, PackageFilename, FPackageName::GetAssetPackageExtension()));
    TestTrue(TEXT("blueprint.create wrote a non-empty .uasset"),
        !PackageFilename.IsEmpty() && IFileManager::Get().FileSize(*PackageFilename) > 0);

    const FAssetData RegistryData =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
            .Get().GetAssetByObjectPath(
                FSoftObjectPath(ObjectPath), /*bIncludeOnlyOnDiskAssets=*/false);
    TestTrue(TEXT("created Blueprint is present in the asset registry"),
        RegistryData.IsValid());

    TSharedPtr<FJsonObject> ReloadPayload = MakeShared<FJsonObject>();
    ReloadPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    FTestResponseCapture ReloadCapture;
    TestTrue(TEXT("asset.reload handler is registered"),
        InvokeHandlerWithCapture(TEXT("asset.reload"), ReloadPayload, ReloadCapture));
    TestTrue(FString::Printf(TEXT("asset.reload succeeds (err='%s' msg='%s')"),
        *ReloadCapture.ErrorCode, *ReloadCapture.Message), ReloadCapture.bSuccess);
    if (!ReloadCapture.bSuccess)
    {
        return false;
    }

    UBlueprint* Reloaded = LoadObject<UBlueprint>(nullptr, *ObjectPath);
    if (!TestNotNull(TEXT("created Blueprint cold-reloads from disk"), Reloaded))
    {
        return false;
    }
    TestEqual(TEXT("cold-reloaded Blueprint keeps its requested parent"),
        Reloaded->ParentClass.Get(), APawn::StaticClass());
    return true;
}
