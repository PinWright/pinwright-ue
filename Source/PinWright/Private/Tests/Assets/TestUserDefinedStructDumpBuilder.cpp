// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Asset/UserDefinedStructDumpBuilder.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "AssetDumpTestHelpers.h"
#include "Tests/TestUtils.h"


#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/StructureEditorUtils.h"
#if __has_include("StructUtils/UserDefinedStruct.h")
#include "StructUtils/UserDefinedStruct.h"
#elif __has_include("Engine/UserDefinedStruct.h")
#include "Engine/UserDefinedStruct.h"
#endif
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    using AssetDumpTestHelpers::HasDumpFile;
    using AssetDumpTestHelpers::FindDumpFile;
    using AssetDumpTestHelpers::LoadJsonFile;

    FString MakeUniqueUDSTestAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UUserDefinedStruct* NewTransientUserDefinedStruct(FString& OutObjectPath)
    {
        const FString AssetName = MakeUniqueUDSTestAssetName(TEXT("UDS_UserDefinedStructDump"));
        // Use /Engine/Transient/ — an in-memory-only mount — so DumpSingleAsset's
        // FPackageName::DoesPackageExist gate (which guards against orphan-stub baker
        // residue) does not short-circuit on a /Game/ package that was never saved to
        // disk. TestAssetDumpHandler.cpp's NewPackageBackedTestAsset uses the same
        // convention.
        const FString PackageName = FString::Printf(TEXT("/Engine/Transient/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(
            Package, *AssetName, RF_Public | RF_Standalone | RF_Transient);
        if (!Struct)
        {
            return nullptr;
        }

        // CreateUserDefinedStruct seeds one default Boolean variable (StructureEditorUtils.cpp ~L60);
        // strip it so the fixture owns the full field set deterministically.
        {
            const TArray<FStructVariableDescription> SeedDescs = FStructureEditorUtils::GetVarDesc(Struct);
            for (const FStructVariableDescription& SeedDesc : SeedDescs)
            {
                FStructureEditorUtils::RemoveVariable(Struct, SeedDesc.VarGuid);
            }
        }

        const FEdGraphPinType IntType(UEdGraphSchema_K2::PC_Int, NAME_None, nullptr,
            EPinContainerType::None, false, FEdGraphTerminalType());
        FStructureEditorUtils::AddVariable(Struct, IntType);

        const FEdGraphPinType BoolType(UEdGraphSchema_K2::PC_Boolean, NAME_None, nullptr,
            EPinContainerType::None, false, FEdGraphTerminalType());
        FStructureEditorUtils::AddVariable(Struct, BoolType);

        const TArray<FStructVariableDescription>& VarDescs = FStructureEditorUtils::GetVarDesc(Struct);
        if (VarDescs.Num() >= 1)
        {
            FStructureEditorUtils::RenameVariable(Struct, VarDescs[0].VarGuid, TEXT("RenamedIntField"));
        }

        Struct->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return Struct;
    }

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUserDefinedStructDumpBuilderShapeTest,
    "PinWright.Assets.UserDefinedStruct.DumpBuilder.Shape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUserDefinedStructDumpBuilderShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UUserDefinedStruct* Struct = NewTransientUserDefinedStruct(ObjectPath);
    TestNotNull(TEXT("Transient UUserDefinedStruct created"), Struct);
    if (!Struct)
    {
        return false;
    }

    TSharedPtr<FJsonObject> StructJson = UserDefinedStructDumpBuilder::BuildUserDefinedStructJson(Struct);
    TestTrue(TEXT("BuildUserDefinedStructJson returns non-null"), StructJson.IsValid());
    if (!StructJson.IsValid())
    {
        Struct->RemoveFromRoot();
        return false;
    }

    TestEqual(TEXT("assetKind"), StructJson->GetStringField(TEXT("assetKind")), FString(TEXT("UserDefinedStruct")));
    TestEqual(TEXT("path"), StructJson->GetStringField(TEXT("path")), Struct->GetPathName());

    const TArray<FStructVariableDescription>& VarDescs = FStructureEditorUtils::GetVarDesc(Struct);
    const int32 ExpectedCount = VarDescs.Num();

    const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
    TestTrue(TEXT("fields array exists"), StructJson->TryGetArrayField(TEXT("fields"), Fields));
    TestTrue(TEXT("fields array length matches GetVarDesc"), Fields && Fields->Num() == ExpectedCount);
    if (!Fields || Fields->Num() != ExpectedCount)
    {
        Struct->RemoveFromRoot();
        return false;
    }

    TSet<FString> SeenGuids;
    TSet<FString> ExpectedGuids;
    for (const FStructVariableDescription& VarDesc : VarDescs)
    {
        ExpectedGuids.Add(VarDesc.VarGuid.ToString());
    }

    for (const TSharedPtr<FJsonValue>& Value : *Fields)
    {
        TSharedPtr<FJsonObject> Field = Value.IsValid() ? Value->AsObject() : nullptr;
        TestTrue(TEXT("field entry is object"), Field.IsValid());
        if (!Field.IsValid())
        {
            continue;
        }
        const FString Name = Field->GetStringField(TEXT("name"));
        const FString DisplayName = Field->GetStringField(TEXT("displayName"));
        const FString Guid = Field->GetStringField(TEXT("guid"));
        const FString Type = Field->GetStringField(TEXT("type"));
        TestTrue(TEXT("field name non-empty"), !Name.IsEmpty());
        TestTrue(TEXT("field displayName non-empty"), !DisplayName.IsEmpty());
        TestTrue(TEXT("field guid non-empty"), !Guid.IsEmpty());
        TestTrue(TEXT("field type non-empty"), !Type.IsEmpty());
        TestFalse(TEXT("field guid is unique"), SeenGuids.Contains(Guid));
        SeenGuids.Add(Guid);
        TestTrue(TEXT("field guid matches FStructureEditorUtils::GetVarDesc"), ExpectedGuids.Contains(Guid));
    }

    Struct->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUserDefinedStructAssetDumpWritesUDSAspectFileTest,
    "PinWright.Assets.UserDefinedStruct.AssetDump.WritesUDSAspectFile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUserDefinedStructAssetDumpWritesUDSAspectFileTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UUserDefinedStruct* Struct = NewTransientUserDefinedStruct(ObjectPath);
    TestNotNull(TEXT("Transient UUserDefinedStruct created"), Struct);
    if (!Struct)
    {
        return false;
    }

    const FString ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("UserDefinedStructDumpBuilderTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for transient UDS"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("properties.json remains present"), HasDumpFile(Result.WrittenPaths, DumpFileNames::Properties));
    TestTrue(TEXT("user_defined_struct.json is written"), HasDumpFile(Result.WrittenPaths, DumpFileNames::UserDefinedStruct));

    const FString UDSPath = FindDumpFile(Result.WrittenPaths, DumpFileNames::UserDefinedStruct);
    TSharedPtr<FJsonObject> UDSJson = LoadJsonFile(UDSPath);
    TestTrue(TEXT("user_defined_struct.json parses"), UDSJson.IsValid());
    if (UDSJson.IsValid())
    {
        TestEqual(TEXT("user_defined_struct.json assetKind"),
            UDSJson->GetStringField(TEXT("assetKind")), FString(TEXT("UserDefinedStruct")));

        const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
        TestTrue(TEXT("user_defined_struct.json fields array exists"), UDSJson->TryGetArrayField(TEXT("fields"), Fields));
        const int32 ExpectedCount = FStructureEditorUtils::GetVarDesc(Struct).Num();
        TestTrue(TEXT("user_defined_struct.json fields length matches GetVarDesc"),
            Fields && Fields->Num() == ExpectedCount);
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    Struct->RemoveFromRoot();
    return true;
}
