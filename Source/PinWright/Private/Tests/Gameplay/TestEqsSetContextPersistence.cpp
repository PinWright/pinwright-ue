// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behavioural coverage for eqs.set_context_class {save:true}. The fixture starts as a
// real saved UEnvQuery, then drives the handler and verifies the changed context through
// the response, disk, registry, and a forced package reload.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dispatch/SafePoint.h"
#include "Dom/JsonObject.h"
#include "EnvironmentQuery/Contexts/EnvQueryContext_Item.h"
#include "EnvironmentQuery/Contexts/EnvQueryContext_Querier.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_OnCircle.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Tests/TestUtils.h"
#include "Utils/AssetUtils.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEqsSetContextPersistenceTest,
    "PinWright.eqs.set_context_class.PersistenceWritesDiskRegistryAndColdReload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEqsSetContextPersistenceTest::RunTest(const FString& /*Parameters*/)
{
    TestTrue(TEXT("eqs.set_context_class is routed outside UWorld::Tick"),
        PinWrightSafePoint::IsTickUnsafeMethod(TEXT("eqs.set_context_class")));

    const FString AssetName = FString::Printf(TEXT("EQS_ContextPersistence_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);

    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("EQS package is created"), Package))
    {
        return false;
    }
    UEnvQuery* Query = NewObject<UEnvQuery>(
        Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
    if (!TestNotNull(TEXT("EQS asset is created"), Query))
    {
        return false;
    }
    TStrongObjectPtr<UEnvQuery> QueryOwner(Query);
    ON_SCOPE_EXIT
    {
        QueryOwner.Reset();
        CleanupTestAsset(PackagePath);
    };

    UEnvQueryOption* Option = NewObject<UEnvQueryOption>(
        Query, UEnvQueryOption::StaticClass(), NAME_None, RF_Transactional);
    UEnvQueryGenerator_OnCircle* Generator = NewObject<UEnvQueryGenerator_OnCircle>(
        Option, UEnvQueryGenerator_OnCircle::StaticClass(), NAME_None, RF_Transactional);
    if (!TestNotNull(TEXT("EQS option is created"), Option)
        || !TestNotNull(TEXT("EQS generator is created"), Generator))
    {
        return false;
    }
    Generator->CircleCenter = UEnvQueryContext_Item::StaticClass();
    Option->Generator = Generator;
    Query->GetOptionsMutable().Add(Option);
    FAssetRegistryModule::AssetCreated(Query);
    Query->MarkPackageDirty();

    EAssetSaveState BaselineState = EAssetSaveState::NotRequested;
    if (!TestTrue(TEXT("baseline EQS fixture is saved"),
            SaveAssetToDiskReportingPresence(
                Query, /*bForce=*/true, nullptr, nullptr, &BaselineState)))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("queryPath"), ObjectPath);
    Payload->SetNumberField(TEXT("generatorIndex"), 0);
    Payload->SetStringField(TEXT("propertyName"), TEXT("CircleCenter"));
    Payload->SetStringField(TEXT("contextClass"), TEXT("querier"));
    Payload->SetBoolField(TEXT("save"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("eqs.set_context_class handler is registered"),
        InvokeHandlerWithCapture(TEXT("eqs.set_context_class"), Payload, Capture));
    TestTrue(TEXT("eqs.set_context_class responds"), Capture.bWasCalled);
    TestTrue(FString::Printf(TEXT("eqs.set_context_class succeeds (err='%s' msg='%s')"),
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
    TestTrue(TEXT("save:true is reported as requested"), bSaveRequested);
    TestTrue(TEXT("response carries saved"),
        Capture.Result->TryGetBoolField(TEXT("saved"), bSaved));
    TestTrue(TEXT("set_context_class reports measured durability"), bSaved);
    TestTrue(TEXT("response carries saveState"),
        Capture.Result->TryGetStringField(TEXT("saveState"), SaveState));
    TestEqual(TEXT("context mutation reports a disk write"),
        SaveState, FString(TEXT("written")));
    TestTrue(TEXT("response carries existsOnDisk"),
        Capture.Result->TryGetBoolField(TEXT("existsOnDisk"), bExistsOnDisk));
    TestTrue(TEXT("response confirms the query exists on disk"), bExistsOnDisk);
    TestFalse(TEXT("durable context mutation has no pendingFlush"),
        Capture.Result->HasField(TEXT("pendingFlush")));
    TestFalse(TEXT("durable context mutation has no pendingSave"),
        Capture.Result->HasField(TEXT("pendingSave")));
    TestEqual(TEXT("context changes in memory"),
        Generator->CircleCenter.Get(), UEnvQueryContext_Querier::StaticClass());

    FString PackageFilename;
    TestTrue(TEXT("EQS package resolves to a filename"),
        FPackageName::TryConvertLongPackageNameToFilename(
            PackagePath, PackageFilename, FPackageName::GetAssetPackageExtension()));
    TestTrue(TEXT("set_context_class leaves a non-empty .uasset"),
        !PackageFilename.IsEmpty() && IFileManager::Get().FileSize(*PackageFilename) > 0);

    const FAssetData RegistryData =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
            .Get().GetAssetByObjectPath(
                FSoftObjectPath(ObjectPath), /*bIncludeOnlyOnDiskAssets=*/false);
    TestTrue(TEXT("saved EQS query remains present in the asset registry"),
        RegistryData.IsValid());

    QueryOwner.Reset();
    Query = nullptr;
    Option = nullptr;
    Generator = nullptr;
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

    UEnvQuery* ReloadedQuery = LoadObject<UEnvQuery>(nullptr, *ObjectPath);
    if (!TestNotNull(TEXT("EQS query cold-reloads from disk"), ReloadedQuery))
    {
        return false;
    }
    const auto& ReloadedOptions = ReloadedQuery->GetOptions();
    UEnvQueryOption* ReloadedOption = ReloadedOptions.IsValidIndex(0)
        ? ReloadedOptions[0]
        : nullptr;
    UEnvQueryGenerator_OnCircle* ReloadedGenerator = ReloadedOption
        ? Cast<UEnvQueryGenerator_OnCircle>(ReloadedOption->Generator)
        : nullptr;
    if (!TestNotNull(TEXT("cold-reloaded EQS generator exists"), ReloadedGenerator))
    {
        return false;
    }
    TestEqual(TEXT("cold-reloaded query keeps the new context class"),
        ReloadedGenerator->CircleCenter.Get(), UEnvQueryContext_Querier::StaticClass());
    return true;
}
