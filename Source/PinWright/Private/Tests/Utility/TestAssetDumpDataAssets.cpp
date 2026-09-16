// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "McpGenericDataAsset.h"
#include "Utils/AssetDumpWriter.h"
#include "Utils/PropertyUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"


#include "TestAssetDumpDataAssetFixture.h"

namespace
{
    template <typename AssetType>
    AssetType* NewTestAsset(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }

        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        return NewObject<AssetType>(
            Package,
            FName(*AssetName),
            RF_Public | RF_Standalone | RF_Transient);
    }

    TSharedPtr<FJsonObject> LoadJsonObject(const FString& Path)
    {
        FString Content;
        if (!FFileHelper::LoadFileToString(Content, *Path))
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> Object;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
        if (!FJsonSerializer::Deserialize(Reader, Object))
        {
            return nullptr;
        }
        return Object;
    }

    TSharedPtr<FJsonObject> FindPropertyObject(
        const TSharedPtr<FJsonObject>& Properties,
        const TCHAR* Name)
    {
        if (!Properties.IsValid())
        {
            return nullptr;
        }
        TSharedPtr<FJsonValue>* Value = Properties->Values.Find(Name);
        return Value && Value->IsValid() ? (*Value)->AsObject() : nullptr;
    }

    bool TryGetPropertyString(
        const TSharedPtr<FJsonObject>& Properties,
        const TCHAR* Name,
        FString& OutValue)
    {
        TSharedPtr<FJsonObject> PropertyObject = FindPropertyObject(Properties, Name);
        return PropertyObject.IsValid()
            && PropertyObject->TryGetStringField(TEXT("value"), OutValue);
    }

    bool TryGetPropertyBool(
        const TSharedPtr<FJsonObject>& Properties,
        const TCHAR* Name,
        const TCHAR* FieldName,
        bool& OutValue)
    {
        TSharedPtr<FJsonObject> PropertyObject = FindPropertyObject(Properties, Name);
        return PropertyObject.IsValid()
            && PropertyObject->TryGetBoolField(FieldName, OutValue);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpNativeDataAssetInstanceValuesTest,
    "PinWright.asset.dump.NativeDataAssetInstanceValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpNativeDataAssetInstanceValuesTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetPath = FString::Printf(TEXT("/Engine/Transient/AssetDumpGeneric_%s"), *Suffix);
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpDataAssets") / Suffix;

    UMcpGenericDataAsset* Asset = NewTestAsset<UMcpGenericDataAsset>(AssetPath);
    if (!TestNotNull(TEXT("Native data asset created"), Asset))
    {
        return true;
    }

    Asset->ItemName = TEXT("Dumped item sentinel");
    Asset->Description = TEXT("Dumped description sentinel");
    Asset->Properties.Add(TEXT("key"), TEXT("value"));

    const FString ObjectPath = FString::Printf(
        TEXT("%s.%s"),
        *AssetPath,
        *FPackageName::GetLongPackageAssetName(AssetPath));
    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot);

    TestTrue(TEXT("Dump succeeds"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("properties.json exists"),
        IFileManager::Get().FileExists(*(Result.DumpDir / DumpFileNames::Properties)));

    TSharedPtr<FJsonObject> Properties =
        LoadJsonObject(Result.DumpDir / DumpFileNames::Properties);
    TestTrue(TEXT("properties.json parsed"), Properties.IsValid());
    if (!Properties.IsValid())
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    FString ItemName;
    TestTrue(TEXT("ItemName value is present"),
        TryGetPropertyString(Properties, TEXT("ItemName"), ItemName));
    TestEqual(TEXT("ItemName value comes from asset instance"),
        ItemName,
        FString(TEXT("Dumped item sentinel")));

    FString Description;
    TestTrue(TEXT("Description value is present"),
        TryGetPropertyString(Properties, TEXT("Description"), Description));
    TestEqual(TEXT("Description value comes from asset instance"),
        Description,
        FString(TEXT("Dumped description sentinel")));

    bool bOverridden = false;
    TestTrue(TEXT("ItemName override state is present"),
        TryGetPropertyBool(Properties, TEXT("ItemName"), TEXT("is_overridden_locally"), bOverridden));
    TestTrue(TEXT("ItemName is overridden against class default"), bOverridden);

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpNativePrimaryDataAssetInstanceValuesTest,
    "PinWright.asset.dump.NativePrimaryDataAssetInstanceValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpNativePrimaryDataAssetInstanceValuesTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetPath = FString::Printf(TEXT("/Engine/Transient/AssetDumpPrimary_%s"), *Suffix);
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpDataAssets") / Suffix;

    UTestAssetDumpPrimaryDataAsset* Asset =
        NewTestAsset<UTestAssetDumpPrimaryDataAsset>(AssetPath);
    if (!TestNotNull(TEXT("Native primary data asset created"), Asset))
    {
        return true;
    }

    Asset->SentinelText = TEXT("Primary asset sentinel");

    const FString ObjectPath = FString::Printf(
        TEXT("%s.%s"),
        *AssetPath,
        *FPackageName::GetLongPackageAssetName(AssetPath));
    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot);

    TestTrue(TEXT("Dump succeeds"), Result.ErrorCode.IsEmpty());

    TSharedPtr<FJsonObject> Properties =
        LoadJsonObject(Result.DumpDir / DumpFileNames::Properties);
    TestTrue(TEXT("properties.json parsed"), Properties.IsValid());
    if (!Properties.IsValid())
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    FString SentinelText;
    TestTrue(TEXT("SentinelText value is present"),
        TryGetPropertyString(Properties, TEXT("SentinelText"), SentinelText));
    TestEqual(TEXT("SentinelText value comes from primary asset instance"),
        SentinelText,
        FString(TEXT("Primary asset sentinel")));

    bool bChangedOverridden = false;
    TestTrue(TEXT("SentinelText override state is present"),
        TryGetPropertyBool(Properties, TEXT("SentinelText"), TEXT("is_overridden_locally"), bChangedOverridden));
    TestTrue(TEXT("SentinelText is overridden against class default"), bChangedOverridden);

    // Pin WHY the assertion below passes. TryGetPropertyBool returns false both when the
    // flag is absent and when the DefaultNumber entry is missing altogether, so on its own
    // it also passes if the property were dropped for an unrelated reason. The verified
    // contract is the second one: ExportPropertyToJsonValueWithInheritance sets
    // is_overridden_locally only when the value differs from the parent CDO, and
    // BuildClassPropertyJson skips any property whose object lacks that field — so a
    // property equal to the class default is dropped outright, not emitted flagless.
    bool bDefaultOverridden = true;
    TestFalse(TEXT("DefaultNumber entry is dropped entirely when it matches the class default"),
        FindPropertyObject(Properties, TEXT("DefaultNumber")).IsValid());
    TestFalse(TEXT("DefaultNumber omits override state when same as class default"),
        TryGetPropertyBool(Properties, TEXT("DefaultNumber"), TEXT("is_overridden_locally"), bDefaultOverridden));

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpDataAssetDefaultComparisonTest,
    "PinWright.utils.asset_dump_data_asset.DefaultComparison",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpDataAssetDefaultComparisonTest::RunTest(const FString& Parameters)
{
    UTestAssetDumpPrimaryDataAsset* Asset =
        NewObject<UTestAssetDumpPrimaryDataAsset>(GetTransientPackage());
    if (!TestNotNull(TEXT("Native primary data asset created"), Asset))
    {
        return true;
    }

    Asset->SentinelText = TEXT("Direct serializer sentinel");
    Asset->DefaultNumber = 7;

    TSharedPtr<FJsonObject> Properties = BuildClassPropertyJson(
        Asset,
        UTestAssetDumpPrimaryDataAsset::StaticClass()->GetDefaultObject());
    TestTrue(TEXT("Properties object is valid"), Properties.IsValid());
    if (!Properties.IsValid())
    {
        return true;
    }

    FString SentinelText;
    TestTrue(TEXT("SentinelText value is present"),
        TryGetPropertyString(Properties, TEXT("SentinelText"), SentinelText));
    TestEqual(TEXT("SentinelText value comes from asset instance"),
        SentinelText,
        FString(TEXT("Direct serializer sentinel")));

    bool bChangedOverridden = false;
    TestTrue(TEXT("SentinelText override state is present"),
        TryGetPropertyBool(Properties, TEXT("SentinelText"), TEXT("is_overridden_locally"), bChangedOverridden));
    TestTrue(TEXT("SentinelText differs from class default"), bChangedOverridden);

    // Pin WHY the assertion below passes. TryGetPropertyBool returns false both when the
    // flag is absent and when the DefaultNumber entry is missing altogether, so on its own
    // it also passes if the property were dropped for an unrelated reason. The verified
    // contract is the second one: ExportPropertyToJsonValueWithInheritance sets
    // is_overridden_locally only when the value differs from the parent CDO, and
    // BuildClassPropertyJson skips any property whose object lacks that field — so a
    // property equal to the class default is dropped outright, not emitted flagless.
    bool bDefaultOverridden = true;
    TestFalse(TEXT("DefaultNumber entry is dropped entirely when it matches the class default"),
        FindPropertyObject(Properties, TEXT("DefaultNumber")).IsValid());
    TestFalse(TEXT("DefaultNumber omits override state when same as class default"),
        TryGetPropertyBool(Properties, TEXT("DefaultNumber"), TEXT("is_overridden_locally"), bDefaultOverridden));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpSparsePropertyDiffTest,
    "PinWright.utils.asset_dump_sparse_properties.OnlyChangedProperties",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpSparsePropertyDiffTest::RunTest(const FString& Parameters)
{
    UTestAssetDumpPrimaryDataAsset* Asset =
        NewObject<UTestAssetDumpPrimaryDataAsset>(GetTransientPackage());
    if (!TestNotNull(TEXT("Native primary data asset created"), Asset))
    {
        return true;
    }

    Asset->SentinelText = TEXT("Sparse diff sentinel");
    Asset->DefaultNumber = 7;

    TSharedPtr<FJsonObject> Diff = BuildSparsePropertyDiffJson(
        Asset,
        UTestAssetDumpPrimaryDataAsset::StaticClass()->GetDefaultObject());
    TestTrue(TEXT("Sparse diff object is valid"), Diff.IsValid());
    if (!Diff.IsValid())
    {
        return true;
    }

    TestTrue(TEXT("Changed property is included"), Diff->HasField(TEXT("SentinelText")));
    TestFalse(TEXT("Unchanged property is omitted"), Diff->HasField(TEXT("DefaultNumber")));

    TSharedPtr<FJsonObject> IncompatibleDiff = BuildSparsePropertyDiffJson(
        Asset,
        UMcpGenericDataAsset::StaticClass()->GetDefaultObject());
    TestTrue(TEXT("Incompatible baseline falls back safely"), IncompatibleDiff.IsValid());
    TestTrue(TEXT("Changed property still included after fallback"),
        IncompatibleDiff.IsValid() && IncompatibleDiff->HasField(TEXT("SentinelText")));

    return true;
}
