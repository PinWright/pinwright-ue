// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Tests/TestUtils.h"
#include "Utils/AssetDumpWriter.h"

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


#include "AssetDumpFixtureHelpers.h"
#include "TestAssetDumpDataAssetFixture.h"

namespace
{
    using AssetDumpFixtureHelpers::LoadJsonFile;

    template <typename AssetType>
    AssetType* NewTestDataTableAsset(const FString& PackagePath)
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpDataTableRowsTest,
    "PinWright.asset.dump.DataTableRows",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpDataTableRowsTest::RunTest(const FString& Parameters)
{
    const FString Suffix     = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetPath  = FString::Printf(TEXT("/Engine/Transient/AssetDumpDataTable_%s"), *Suffix);
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpDataTable") / Suffix;
    // The fixture is RF_Standalone, so the periodic suite GC keeps it alive; detach it so it
    // cannot answer a later /Engine/Transient asset-registry rescan.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    UDataTable* Table = NewTestDataTableAsset<UDataTable>(AssetPath);
    if (!TestNotNull(TEXT("DataTable created"), Table))
    {
        return true;
    }

    Table->RowStruct = FTestAssetDumpDataTableRow::StaticStruct();

    FTestAssetDumpDataTableRow RowAlpha;
    RowAlpha.Label = TEXT("Alpha");
    RowAlpha.Score = 7;
    Table->AddRow(FName("RowAlpha"), RowAlpha);

    FTestAssetDumpDataTableRow RowBeta;
    RowBeta.Label = TEXT("Beta");
    RowBeta.Score = 99;
    Table->AddRow(FName("RowBeta"), RowBeta);

    const FString ObjectPath = FString::Printf(
        TEXT("%s.%s"),
        *AssetPath,
        *FPackageName::GetLongPackageAssetName(AssetPath));

    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot);

    TestTrue(TEXT("Dump succeeds"), Result.ErrorCode.IsEmpty());

    const FString DataTablePath = Result.DumpDir / DumpFileNames::DataTable;
    TestTrue(TEXT("data_table.json exists"), IFileManager::Get().FileExists(*DataTablePath));

    TSharedPtr<FJsonObject> DataTableJson = LoadJsonFile(DataTablePath);
    if (!TestTrue(TEXT("data_table.json parsed"), DataTableJson.IsValid()))
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    // rowCount
    double RowCount = 0.0;
    TestTrue(TEXT("rowCount field present"),
        DataTableJson->TryGetNumberField(TEXT("rowCount"), RowCount));
    TestEqual(TEXT("rowCount == 2"), static_cast<int32>(RowCount), 2);

    // rowStructName
    FString RowStructName;
    TestTrue(TEXT("rowStructName field present"),
        DataTableJson->TryGetStringField(TEXT("rowStructName"), RowStructName));
    TestTrue(TEXT("rowStructName contains FTestAssetDumpDataTableRow"),
        RowStructName.Contains(TEXT("TestAssetDumpDataTableRow")));

    // rows
    const TSharedPtr<FJsonObject>* RowsPtr = nullptr;
    if (!TestTrue(TEXT("rows field present"), DataTableJson->TryGetObjectField(TEXT("rows"), RowsPtr)))
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }
    const TSharedPtr<FJsonObject>& Rows = *RowsPtr;

    // RowAlpha
    const TSharedPtr<FJsonObject>* AlphaPtr = nullptr;
    if (TestTrue(TEXT("rows.RowAlpha present"), Rows->TryGetObjectField(TEXT("RowAlpha"), AlphaPtr)))
    {
        FString AlphaLabel;
        TestTrue(TEXT("RowAlpha.Label present"),
            (*AlphaPtr)->TryGetStringField(TEXT("Label"), AlphaLabel));
        TestEqual(TEXT("RowAlpha.Label == Alpha"), AlphaLabel, FString(TEXT("Alpha")));

        double AlphaScore = 0.0;
        TestTrue(TEXT("RowAlpha.Score present"),
            (*AlphaPtr)->TryGetNumberField(TEXT("Score"), AlphaScore));
        TestEqual(TEXT("RowAlpha.Score == 7"), static_cast<int32>(AlphaScore), 7);
    }

    // RowBeta
    const TSharedPtr<FJsonObject>* BetaPtr = nullptr;
    if (TestTrue(TEXT("rows.RowBeta present"), Rows->TryGetObjectField(TEXT("RowBeta"), BetaPtr)))
    {
        double BetaScore = 0.0;
        TestTrue(TEXT("RowBeta.Score present"),
            (*BetaPtr)->TryGetNumberField(TEXT("Score"), BetaScore));
        TestEqual(TEXT("RowBeta.Score == 99"), static_cast<int32>(BetaScore), 99);
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}
