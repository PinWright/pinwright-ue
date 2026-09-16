// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "TestDataTableAtomicityFixture.h"

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"

#include "Tests/TestUtils.h"

#include "Engine/DataTable.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    static TStrongObjectPtr<UDataTable> MakeAtomicityDataTable(FString& OutAssetPath)
    {
        const FString PackagePath = FString::Printf(TEXT("/Engine/Transient/DT_SetRowAtomic_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPackage* Package = CreatePackage(*PackagePath);
        UDataTable* Table = NewObject<UDataTable>(
            Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transient);
        if (Table)
        {
            Table->RowStruct = FTestDataTableAtomicRow::StaticStruct();
            OutAssetPath = Table->GetPathName();
        }
        return TStrongObjectPtr<UDataTable>(Table);
    }

    static TSharedPtr<FJsonObject> MakeAtomicityValues(
        const FString& Label, int32 Score, const FString& Mode)
    {
        TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
        Values->SetStringField(TEXT("Label"), Label);
        Values->SetNumberField(TEXT("Score"), static_cast<double>(Score));
        Values->SetStringField(TEXT("Mode"), Mode);
        return Values;
    }
}

// The converter writes properties in struct order and returns false as soon as a
// later property cannot be converted. A failed set_row must leave the existing row
// byte-identical even though the earlier Label and Score values are valid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableSetRowLateConversionFailureIsAtomicTest,
    "PinWright.data_table.set_row.AtomicOnLateConversionFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableSetRowLateConversionFailureIsAtomicTest::RunTest(const FString& /*Parameters*/)
{
    FString AssetPath;
    // The fixture is RF_Standalone, so releasing TableOwner alone leaves it alive through the
    // periodic suite GC and visible to a later /Engine/Transient asset-registry rescan. Declared
    // before TableOwner so it runs after that release.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    TStrongObjectPtr<UDataTable> TableOwner = MakeAtomicityDataTable(AssetPath);
    UDataTable* Table = TableOwner.Get();
    TestNotNull(TEXT("Transient atomicity DataTable created"), Table);
    if (!Table)
    {
        return false;
    }
    const FName RowName(TEXT("Room"));
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("rowName"), RowName.ToString());
        Payload->SetObjectField(TEXT("values"),
            MakeAtomicityValues(TEXT("original"), 17, TEXT("Stable")));

        FTestResponseCapture Capture;
        TestTrue(TEXT("seed add_row handler found"),
            InvokeHandlerWithCapture(TEXT("data_table.add_row"), Payload, Capture));
        TestTrue(FString::Printf(TEXT("seed add_row succeeded (err='%s' msg='%s')"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
    }

    uint8* const* BeforePtr = Table->GetRowMap().Find(RowName);
    TestNotNull(TEXT("seed row exists"), BeforePtr);
    if (!BeforePtr || !*BeforePtr)
    {
        return false;
    }

    const UScriptStruct* RowStruct = Table->RowStruct;
    const int32 RowSize = RowStruct->GetStructureSize();
    TArray<uint8> BeforeBytes;
    BeforeBytes.SetNumUninitialized(RowSize);
    FMemory::Memcpy(BeforeBytes.GetData(), *BeforePtr, static_cast<SIZE_T>(RowSize));

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("rowName"), RowName.ToString());
        // Label and Score are valid and would be written before the invalid enum
        // Mode. This is the late-failure sequence that used to mutate the live row.
        Payload->SetObjectField(TEXT("values"),
            MakeAtomicityValues(TEXT("should-not-land"), 99, TEXT("Bogus")));

        FTestResponseCapture Capture;
        TestTrue(TEXT("failing set_row handler found"),
            InvokeHandlerWithCapture(TEXT("data_table.set_row"), Payload, Capture));
        TestFalse(TEXT("set_row with a late invalid enum failed"), Capture.bSuccess);
        TestEqual(TEXT("error code == INVALID_ROW_VALUES"),
            Capture.ErrorCode, FString(TEXT("INVALID_ROW_VALUES")));
        TestTrue(TEXT("error identifies the late failing Mode field"),
            Capture.Message.Contains(TEXT("Mode")));
    }

    uint8* const* AfterPtr = Table->GetRowMap().Find(RowName);
    TestNotNull(TEXT("row still exists after failed set_row"), AfterPtr);
    if (!AfterPtr || !*AfterPtr)
    {
        return false;
    }

    TestTrue(TEXT("failed set_row leaves the target row byte-identical"),
        FMemory::Memcmp(BeforeBytes.GetData(), *AfterPtr, static_cast<SIZE_T>(RowSize)) == 0);

    const FTestDataTableAtomicRow* Row = reinterpret_cast<const FTestDataTableAtomicRow*>(*AfterPtr);
    TestEqual(TEXT("Label remains original"), Row->Label, FString(TEXT("original")));
    TestEqual(TEXT("Score remains original"), Row->Score, 17);
    TestEqual(TEXT("Mode remains Stable"), Row->Mode, ETestDataTableAtomicMode::Stable);
    return true;
}
