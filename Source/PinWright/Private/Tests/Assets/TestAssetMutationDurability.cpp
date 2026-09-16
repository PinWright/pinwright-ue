// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Compat/EngineVersionCompat.h"
#include "Dispatch/SafePoint.h"
#include "Dom/JsonObject.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Guid.h"
#include "Sound/SoundClass.h"
#include "Tests/TestUtils.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "Utils/AssetUtils.h"

namespace PinWrightAssetMutationDurabilityTests
{
    static UObject* ReloadFromDisk(
        FAutomationTestBase& Test, const FString& ObjectPath, const FString& Label)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);

        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        Test.TestTrue(*FString::Printf(TEXT("%s reload handler found"), *Label),
            InvokeHandlerWithSharedCapture(TEXT("asset.reload"), Payload, Capture));
        PumpUntilCaptured(*Capture, 20.0);
        Test.TestTrue(*FString::Printf(TEXT("%s reload responded"), *Label), Capture->bWasCalled);
        Test.TestTrue(*FString::Printf(TEXT("%s reload succeeded"), *Label), Capture->bSuccess);
        if (!Capture->bSuccess)
        {
            Test.AddError(*FString::Printf(TEXT("%s reload failed: %s - %s"),
                *Label, *Capture->ErrorCode, *Capture->Message));
            return nullptr;
        }
        return LoadObject<UObject>(nullptr, *ObjectPath);
    }

    static void AssertSaveReport(
        FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Result,
        const FString& Label,
        bool bExpectedRequested,
        bool bExpectedSaved,
        const FString& ExpectedState)
    {
        if (!Test.TestTrue(*FString::Printf(TEXT("%s returned a result"), *Label), Result.IsValid()))
        {
            return;
        }

        bool bSaveRequested = !bExpectedRequested;
        bool bSaved = !bExpectedSaved;
        FString SaveState;
        Test.TestTrue(*FString::Printf(TEXT("%s reports saveRequested"), *Label),
            Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested));
        Test.TestTrue(*FString::Printf(TEXT("%s reports saved"), *Label),
            Result->TryGetBoolField(TEXT("saved"), bSaved));
        Test.TestTrue(*FString::Printf(TEXT("%s reports saveState"), *Label),
            Result->TryGetStringField(TEXT("saveState"), SaveState));
        Test.TestEqual(*FString::Printf(TEXT("%s saveRequested value"), *Label),
            bSaveRequested, bExpectedRequested);
        Test.TestEqual(*FString::Printf(TEXT("%s saved value"), *Label),
            bSaved, bExpectedSaved);
        Test.TestEqual(*FString::Printf(TEXT("%s saveState value"), *Label),
            SaveState, ExpectedState);
    }

    static FString ReadMetadata(UObject* Asset, const FString& Key)
    {
        if (!Asset || !Asset->GetOutermost())
        {
            return FString();
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        return Asset->GetOutermost()->GetMetaData().GetValue(Asset, *Key);
#else
        return Asset->GetOutermost()->GetMetaData()->GetValue(Asset, *Key);
#endif
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAssetSetMetadataDurableSaveRoundTripTest,
    "PinWright.asset.set_metadata.DurableSaveRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetSetMetadataDurableSaveRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAssetMutationDurabilityTests;

    TestTrue(TEXT("asset.set_metadata is routed outside UWorld::Tick"),
        PinWrightSafePoint::IsTickUnsafeMethod(TEXT("asset.set_metadata")));

    const FString AssetName = FString::Printf(
        TEXT("SC_MetadataDurability_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/__PW_GatewayTests/%s"), *AssetName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    const FString MetadataKey = TEXT("PinWrightDurableMetadata");

    UPackage* Package = CreatePackage(*PackagePath);
    USoundClass* Asset = Package
        ? NewObject<USoundClass>(Package, *AssetName, RF_Public | RF_Standalone)
        : nullptr;
    if (!TestNotNull(TEXT("metadata fixture package created"), Package) ||
        !TestNotNull(TEXT("metadata fixture asset created"), Asset))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    FAssetRegistryModule::AssetCreated(Asset);
    Package->MarkPackageDirty();
    if (!TestTrue(TEXT("metadata fixture baseline reached disk"),
            SaveAssetToDiskReportingPresence(Asset, /*bForce=*/true)))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> UnsavedMetadata = MakeShared<FJsonObject>();
    UnsavedMetadata->SetStringField(MetadataKey, TEXT("memory-only"));
    TSharedPtr<FJsonObject> UnsavedPayload = MakeShared<FJsonObject>();
    UnsavedPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    UnsavedPayload->SetObjectField(TEXT("metadata"), UnsavedMetadata);
    UnsavedPayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture UnsavedCapture;
    TestTrue(TEXT("asset.set_metadata handler found for save:false"),
        InvokeHandlerWithCapture(TEXT("asset.set_metadata"), UnsavedPayload, UnsavedCapture));
    TestTrue(TEXT("asset.set_metadata save:false succeeds"), UnsavedCapture.bSuccess);
    AssertSaveReport(*this, UnsavedCapture.Result, TEXT("asset.set_metadata save:false"),
        /*bExpectedRequested=*/false, /*bExpectedSaved=*/false, TEXT("notRequested"));
    TestTrue(TEXT("asset.set_metadata save:false leaves the package dirty"), Package->IsDirty());

    Package->SetDirtyFlag(false);
    Asset = Cast<USoundClass>(ReloadFromDisk(*this, ObjectPath, TEXT("metadata save:false")));
    if (!TestNotNull(TEXT("metadata fixture reloads after save:false"), Asset))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }
    TestTrue(TEXT("save:false metadata is absent after disk reload"),
        ReadMetadata(Asset, MetadataKey).IsEmpty());

    TSharedPtr<FJsonObject> DurableMetadata = MakeShared<FJsonObject>();
    DurableMetadata->SetStringField(MetadataKey, TEXT("durable"));
    TSharedPtr<FJsonObject> DurablePayload = MakeShared<FJsonObject>();
    DurablePayload->SetStringField(TEXT("assetPath"), ObjectPath);
    DurablePayload->SetObjectField(TEXT("metadata"), DurableMetadata);

    FTestResponseCapture DurableCapture;
    TestTrue(TEXT("asset.set_metadata handler found for default save"),
        InvokeHandlerWithCapture(TEXT("asset.set_metadata"), DurablePayload, DurableCapture));
    TestTrue(TEXT("asset.set_metadata default save succeeds"), DurableCapture.bSuccess);
    AssertSaveReport(*this, DurableCapture.Result, TEXT("asset.set_metadata default save"),
        /*bExpectedRequested=*/true, /*bExpectedSaved=*/true, TEXT("written"));
    if (DurableCapture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* Readback = nullptr;
        TestTrue(TEXT("asset.set_metadata returns read-back metadata"),
            DurableCapture.Result->TryGetObjectField(TEXT("metadata"), Readback) && Readback);
        FString ReadbackValue;
        if (Readback)
        {
            TestTrue(TEXT("asset.set_metadata readback contains the written key"),
                (*Readback)->TryGetStringField(MetadataKey, ReadbackValue));
            TestEqual(TEXT("asset.set_metadata readback matches the written value"),
                ReadbackValue, FString(TEXT("durable")));
        }
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    Asset->GetOutermost()->GetMetaData().SetValue(Asset, *MetadataKey, TEXT("resident-only"));
#else
    Asset->GetOutermost()->GetMetaData()->SetValue(Asset, *MetadataKey, TEXT("resident-only"));
#endif
    Asset = Cast<USoundClass>(ReloadFromDisk(*this, ObjectPath, TEXT("metadata default save")));
    if (TestNotNull(TEXT("metadata fixture reloads after default save"), Asset))
    {
        TestEqual(TEXT("disk reload preserves the metadata written by the handler"),
            ReadMetadata(Asset, MetadataKey), FString(TEXT("durable")));
    }

    CleanupTestAsset(PackagePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAssetResetInstanceParametersDurableSaveRoundTripTest,
    "PinWright.asset.reset_instance_parameters.DurableSaveRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetResetInstanceParametersDurableSaveRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAssetMutationDurabilityTests;

    TestTrue(TEXT("asset.reset_instance_parameters is routed outside UWorld::Tick"),
        PinWrightSafePoint::IsTickUnsafeMethod(TEXT("asset.reset_instance_parameters")));

    const FString AssetName = FString::Printf(
        TEXT("MI_ResetDurability_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/__PW_GatewayTests/%s"), *AssetName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);

    UPackage* Package = CreatePackage(*PackagePath);
    UMaterialInstanceConstant* Instance = Package
        ? NewObject<UMaterialInstanceConstant>(Package, *AssetName, RF_Public | RF_Standalone)
        : nullptr;
    if (!TestNotNull(TEXT("material-instance fixture package created"), Package) ||
        !TestNotNull(TEXT("material-instance fixture asset created"), Instance))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    FScalarParameterValue Override;
    Override.ParameterInfo = FMaterialParameterInfo(FName(TEXT("PinWrightDurableScalar")));
    Override.ParameterValue = 0.75f;
    Instance->ScalarParameterValues.Add(Override);
    FAssetRegistryModule::AssetCreated(Instance);
    Package->MarkPackageDirty();
    if (!TestTrue(TEXT("material-instance fixture baseline reached disk"),
            SaveAssetToDiskReportingPresence(Instance, /*bForce=*/true)))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> UnsavedPayload = MakeShared<FJsonObject>();
    UnsavedPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    UnsavedPayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture UnsavedCapture;
    TestTrue(TEXT("asset.reset_instance_parameters handler found for save:false"),
        InvokeHandlerWithCapture(
            TEXT("asset.reset_instance_parameters"), UnsavedPayload, UnsavedCapture));
    TestTrue(TEXT("asset.reset_instance_parameters save:false succeeds"), UnsavedCapture.bSuccess);
    AssertSaveReport(*this, UnsavedCapture.Result,
        TEXT("asset.reset_instance_parameters save:false"),
        /*bExpectedRequested=*/false, /*bExpectedSaved=*/false, TEXT("notRequested"));
    TestEqual(TEXT("save:false clears the resident scalar override"),
        Instance->ScalarParameterValues.Num(), 0);
    TestTrue(TEXT("asset.reset_instance_parameters save:false leaves the package dirty"),
        Package->IsDirty());

    Package->SetDirtyFlag(false);
    Instance = Cast<UMaterialInstanceConstant>(
        ReloadFromDisk(*this, ObjectPath, TEXT("reset save:false")));
    if (!TestNotNull(TEXT("material instance reloads after save:false"), Instance))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }
    TestEqual(TEXT("disk reload restores the override after save:false"),
        Instance->ScalarParameterValues.Num(), 1);

    TSharedPtr<FJsonObject> DurablePayload = MakeShared<FJsonObject>();
    DurablePayload->SetStringField(TEXT("assetPath"), ObjectPath);

    FTestResponseCapture DurableCapture;
    TestTrue(TEXT("asset.reset_instance_parameters handler found for default save"),
        InvokeHandlerWithCapture(
            TEXT("asset.reset_instance_parameters"), DurablePayload, DurableCapture));
    TestTrue(TEXT("asset.reset_instance_parameters default save succeeds"), DurableCapture.bSuccess);
    AssertSaveReport(*this, DurableCapture.Result,
        TEXT("asset.reset_instance_parameters default save"),
        /*bExpectedRequested=*/true, /*bExpectedSaved=*/true, TEXT("written"));
    if (DurableCapture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* Counts = nullptr;
        TestTrue(TEXT("reset response carries remainingOverrideCounts"),
            DurableCapture.Result->TryGetObjectField(TEXT("remainingOverrideCounts"), Counts) && Counts);
        double Total = -1.0;
        if (Counts)
        {
            TestTrue(TEXT("remainingOverrideCounts carries total"),
                (*Counts)->TryGetNumberField(TEXT("total"), Total));
            TestEqual(TEXT("reset reports zero remaining overrides"), Total, 0.0);
        }
    }

    Instance->ScalarParameterValues.Add(Override);
    Instance = Cast<UMaterialInstanceConstant>(
        ReloadFromDisk(*this, ObjectPath, TEXT("reset default save")));
    if (TestNotNull(TEXT("material instance reloads after default save"), Instance))
    {
        TestEqual(TEXT("disk reload preserves the handler's cleared overrides"),
            Instance->ScalarParameterValues.Num(), 0);
    }

    CleanupTestAsset(PackagePath);
    return true;
}
