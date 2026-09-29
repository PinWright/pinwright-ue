// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "TestDataTableAtomicityFixture.h"

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
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
        // Label, Score and Mode are valid and are written before the last field, When,
        // whose malformed date only the engine converter's parser rejects. This is the
        // late-failure sequence that used to mutate the live row.
        TSharedPtr<FJsonObject> Values =
            MakeAtomicityValues(TEXT("should-not-land"), 99, TEXT("Alternate"));
        Values->SetStringField(TEXT("When"), TEXT("not-a-date"));
        Payload->SetObjectField(TEXT("values"), Values);

        // The converter's refusal of the date, logged once by the field importer and once by
        // the struct walk that called it.
        AddExpectedErrorPlain(
            TEXT("JsonValueToUProperty - Unable to import JSON string into DateTime property When"),
            EAutomationExpectedErrorFlags::Contains, 1);
        AddExpectedErrorPlain(
            TEXT("JsonObjectToUStruct - Unable to import JSON value into property When"),
            EAutomationExpectedErrorFlags::Contains, 1);

        FTestResponseCapture Capture;
        TestTrue(TEXT("failing set_row handler found"),
            InvokeHandlerWithCapture(TEXT("data_table.set_row"), Payload, Capture));
        TestFalse(TEXT("set_row with a late malformed date failed"), Capture.bSuccess);
        TestEqual(TEXT("error code == INVALID_ROW_VALUES"),
            Capture.ErrorCode, FString(TEXT("INVALID_ROW_VALUES")));
        TestTrue(FString::Printf(TEXT("error identifies the late failing When field: %s"), *Capture.Message),
            Capture.Message.Contains(TEXT("When")));
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

// An unresolvable enum literal inside an array is refused before the engine converter runs,
// naming the element. The converter would log two LogJson errors for it; this test declares
// none, so any engine error here means the element walk regressed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableSetRowEnumArrayElementRejectedTest,
    "PinWright.data_table.set_row.EnumArrayElementRejectedBeforeConverter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableSetRowEnumArrayElementRejectedTest::RunTest(const FString& /*Parameters*/)
{
    FString AssetPath;
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    TStrongObjectPtr<UDataTable> TableOwner = MakeAtomicityDataTable(AssetPath);
    UDataTable* Table = TableOwner.Get();
    if (!TestNotNull(TEXT("Transient atomicity DataTable created"), Table))
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
        InvokeHandlerWithCapture(TEXT("data_table.add_row"), Payload, Capture);
        TestTrue(TEXT("seed add_row succeeded"), Capture.bSuccess);
    }

    {
        TArray<TSharedPtr<FJsonValue>> Modes;
        Modes.Add(MakeShared<FJsonValueString>(TEXT("Alternate")));
        Modes.Add(MakeShared<FJsonValueString>(TEXT("Bogus")));
        TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
        Values->SetArrayField(TEXT("Modes"), Modes);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("rowName"), RowName.ToString());
        Payload->SetObjectField(TEXT("values"), Values);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("data_table.set_row"), Payload, Capture);
        TestFalse(TEXT("set_row with an invalid array element failed"), Capture.bSuccess);
        TestEqual(TEXT("error code == INVALID_ROW_VALUES"),
            Capture.ErrorCode, FString(TEXT("INVALID_ROW_VALUES")));
        TestTrue(FString::Printf(TEXT("error names the element path Modes[1]: %s"), *Capture.Message),
            Capture.Message.Contains(TEXT("'Modes[1]'")));
        TestTrue(TEXT("error names the rejected literal"), Capture.Message.Contains(TEXT("\"Bogus\"")));
        TestTrue(TEXT("error lists the valid literals"), Capture.Message.Contains(TEXT("Alternate")));
    }

    uint8* const* RowPtr = Table->GetRowMap().Find(RowName);
    if (!TestTrue(TEXT("row still exists"), RowPtr && *RowPtr))
    {
        return false;
    }
    const FTestDataTableAtomicRow* Row = reinterpret_cast<const FTestDataTableAtomicRow*>(*RowPtr);
    TestEqual(TEXT("Modes stays empty"), Row->Modes.Num(), 0);
    return true;
}

// An unresolvable enum literal inside a nested struct is refused before the engine converter
// runs, naming the dotted field path, and add_row leaves no orphan row behind.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableAddRowNestedStructEnumRejectedTest,
    "PinWright.data_table.add_row.NestedStructEnumRejectedBeforeConverter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableAddRowNestedStructEnumRejectedTest::RunTest(const FString& /*Parameters*/)
{
    FString AssetPath;
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    TStrongObjectPtr<UDataTable> TableOwner = MakeAtomicityDataTable(AssetPath);
    UDataTable* Table = TableOwner.Get();
    if (!TestNotNull(TEXT("Transient atomicity DataTable created"), Table))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
    Inner->SetStringField(TEXT("Mode"), TEXT("Bogus"));
    TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
    Values->SetStringField(TEXT("Label"), TEXT("nested"));
    Values->SetObjectField(TEXT("Inner"), Inner);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("rowName"), TEXT("Nested"));
    Payload->SetObjectField(TEXT("values"), Values);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("data_table.add_row"), Payload, Capture);
    TestFalse(TEXT("add_row with an invalid nested enum failed"), Capture.bSuccess);
    TestEqual(TEXT("error code == INVALID_ROW_VALUES"),
        Capture.ErrorCode, FString(TEXT("INVALID_ROW_VALUES")));
    TestTrue(FString::Printf(TEXT("error names the nested path Inner.Mode: %s"), *Capture.Message),
        Capture.Message.Contains(TEXT("'Inner.Mode'")));
    TestEqual(TEXT("no orphan row was added"), Table->GetRowMap().Num(), 0);
    return true;
}
