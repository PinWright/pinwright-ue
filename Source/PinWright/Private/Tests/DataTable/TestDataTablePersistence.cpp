// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "TestDataTableAtomicityFixture.h"

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dispatch/SafePoint.h"
#include "Dom/JsonObject.h"
#include "Engine/DataTable.h"
#include "Tests/TestUtils.h"
#include "Tests/Utility/TestAssetDumpDataAssetFixture.h"
#include "Utils/AssetUtils.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace DataTablePersistenceTest
{
    static TSharedPtr<FJsonObject> MakeRowValues(const FString& Label, int32 Score)
    {
        TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
        Values->SetStringField(TEXT("Label"), Label);
        Values->SetNumberField(TEXT("Score"), static_cast<double>(Score));
        return Values;
    }

    static bool AssertSaveReport(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Result, bool bExpectedRequested)
    {
        if (!Test.TestTrue(TEXT("mutation response carries a result"), Result.IsValid()))
        {
            return false;
        }

        bool bSaveRequested = !bExpectedRequested;
        bool bSaved = bExpectedRequested;
        FString SaveState;
        Test.TestTrue(TEXT("response carries saveRequested"),
            Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested));
        Test.TestEqual(TEXT("saveRequested matches the request"),
            bSaveRequested, bExpectedRequested);
        Test.TestTrue(TEXT("response carries saved"),
            Result->TryGetBoolField(TEXT("saved"), bSaved));
        Test.TestTrue(TEXT("response carries saveState"),
            Result->TryGetStringField(TEXT("saveState"), SaveState));
        Test.TestTrue(TEXT("response carries saveDetail"),
            Result->HasTypedField<EJson::String>(TEXT("saveDetail")));

        if (bExpectedRequested)
        {
            Test.TestTrue(TEXT("requested DataTable mutation is durable"), bSaved);
            Test.TestEqual(TEXT("requested mutation reports a disk write"),
                SaveState, FString(TEXT("written")));
            Test.TestFalse(TEXT("durable mutation has no pending flush"),
                Result->HasField(TEXT("pendingFlush")));
        }
        else
        {
            Test.TestFalse(TEXT("save:false does not claim durability"), bSaved);
            Test.TestEqual(TEXT("save:false reports notRequested"),
                SaveState, FString(TEXT("notRequested")));
            Test.TestFalse(TEXT("save:false has no requested-save pending flush"),
                Result->HasField(TEXT("pendingFlush")));
        }
        return true;
    }

    static bool InvokeMutation(FAutomationTestBase& Test, const TCHAR* Method,
        const TSharedPtr<FJsonObject>& Payload, bool bExpectedSaveRequested)
    {
        FTestResponseCapture Capture;
        Test.TestTrue(FString::Printf(TEXT("%s handler found"), Method),
            InvokeHandlerWithCapture(Method, Payload, Capture));
        Test.TestTrue(FString::Printf(TEXT("%s succeeded (err='%s' msg='%s')"),
            Method, *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
        return Capture.bSuccess
            && AssertSaveReport(Test, Capture.Result, bExpectedSaveRequested);
    }

    static UDataTable* ReloadFromDisk(FAutomationTestBase& Test, const FString& ObjectPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("asset.reload handler found"),
            InvokeHandlerWithCapture(TEXT("asset.reload"), Payload, Capture));
        Test.TestTrue(FString::Printf(TEXT("asset.reload succeeded (err='%s' msg='%s')"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
        if (!Capture.bSuccess)
        {
            return nullptr;
        }
        return LoadObject<UDataTable>(nullptr, *ObjectPath);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableMutatorPersistenceTest,
    "PinWright.data_table.mutators.PersistenceAndColdReload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableMutatorPersistenceTest::RunTest(const FString& /*Parameters*/)
{
    using namespace DataTablePersistenceTest;

    const TArray<FString> Mutators = {
        TEXT("data_table.add_row"),
        TEXT("data_table.set_row"),
        TEXT("data_table.remove_row"),
        TEXT("data_table.set_row_struct")
    };
    for (const FString& Method : Mutators)
    {
        TestTrue(FString::Printf(TEXT("%s is routed outside UWorld::Tick"), *Method),
            PinWrightSafePoint::IsTickUnsafeMethod(Method));
    }

    const FString AssetName = FString::Printf(TEXT("DT_MutatorPersistence_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);

    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("DataTable test package created"), Package))
    {
        return false;
    }
    UDataTable* Table = NewObject<UDataTable>(
        Package, FName(*AssetName), RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("DataTable test asset created"), Table))
    {
        return false;
    }

    TStrongObjectPtr<UDataTable> TableOwner(Table);
    ON_SCOPE_EXIT
    {
        TableOwner.Reset();
        CleanupTestAsset(PackagePath);
    };

    Table->RowStruct = FTestAssetDumpDataTableRow::StaticStruct();
    FAssetRegistryModule::AssetCreated(Table);
    Table->MarkPackageDirty();
    EAssetSaveState BaselineSaveState = EAssetSaveState::NotRequested;
    if (!TestTrue(TEXT("empty DataTable baseline saved to disk"),
            SaveAssetToDiskReportingPresence(
                Table, /*bForce=*/true, nullptr, nullptr, &BaselineSaveState)))
    {
        return false;
    }

    auto ReloadTable = [&]() -> UDataTable*
    {
        TableOwner.Reset();
        UDataTable* Reloaded = ReloadFromDisk(*this, ObjectPath);
        TableOwner = TStrongObjectPtr<UDataTable>(Reloaded);
        return Reloaded;
    };

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetStringField(TEXT("rowName"), TEXT("Persisted"));
        Payload->SetObjectField(TEXT("values"), DataTablePersistenceTest::MakeRowValues(TEXT("added"), 10));
        if (!InvokeMutation(*this, TEXT("data_table.add_row"), Payload, true))
        {
            return false;
        }
    }

    Table = ReloadTable();
    if (!TestNotNull(TEXT("DataTable reloads after add_row"), Table))
    {
        return false;
    }
    const FTestAssetDumpDataTableRow* Row = Table->FindRow<FTestAssetDumpDataTableRow>(
        FName(TEXT("Persisted")), TEXT("DataTable persistence test"), false);
    if (!TestNotNull(TEXT("add_row survives a cold reload"), Row))
    {
        return false;
    }
    TestEqual(TEXT("cold-loaded add_row Label"), Row->Label, FString(TEXT("added")));
    TestEqual(TEXT("cold-loaded add_row Score"), Row->Score, 10);

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetStringField(TEXT("rowName"), TEXT("Persisted"));
        Payload->SetObjectField(TEXT("values"), DataTablePersistenceTest::MakeRowValues(TEXT("memory update"), 15));
        Payload->SetBoolField(TEXT("save"), false);
        if (!InvokeMutation(*this, TEXT("data_table.set_row"), Payload, false))
        {
            return false;
        }
        Row = Table->FindRow<FTestAssetDumpDataTableRow>(
            FName(TEXT("Persisted")), TEXT("DataTable persistence test"), false);
        if (!TestNotNull(TEXT("save:false set_row updates memory"), Row))
        {
            return false;
        }
        TestEqual(TEXT("save:false set_row value is visible in memory"), Row->Score, 15);
    }

    Table = ReloadTable();
    if (!TestNotNull(TEXT("DataTable reloads after save:false set_row"), Table))
    {
        return false;
    }
    Row = Table->FindRow<FTestAssetDumpDataTableRow>(
        FName(TEXT("Persisted")), TEXT("DataTable persistence test"), false);
    if (!TestNotNull(TEXT("persisted row remains after save:false set_row reload"), Row))
    {
        return false;
    }
    TestEqual(TEXT("save:false set_row is discarded by a cold reload"), Row->Score, 10);

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetStringField(TEXT("rowName"), TEXT("Persisted"));
        Payload->SetObjectField(TEXT("values"), DataTablePersistenceTest::MakeRowValues(TEXT("updated"), 20));
        if (!InvokeMutation(*this, TEXT("data_table.set_row"), Payload, true))
        {
            return false;
        }
    }

    Table = ReloadTable();
    if (!TestNotNull(TEXT("DataTable reloads after set_row"), Table))
    {
        return false;
    }
    Row = Table->FindRow<FTestAssetDumpDataTableRow>(
        FName(TEXT("Persisted")), TEXT("DataTable persistence test"), false);
    if (!TestNotNull(TEXT("set_row survives a cold reload"), Row))
    {
        return false;
    }
    TestEqual(TEXT("cold-loaded set_row Label"), Row->Label, FString(TEXT("updated")));
    TestEqual(TEXT("cold-loaded set_row Score"), Row->Score, 20);

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetStringField(TEXT("rowName"), TEXT("MemoryOnly"));
        Payload->SetObjectField(TEXT("values"), DataTablePersistenceTest::MakeRowValues(TEXT("unsaved"), 99));
        Payload->SetBoolField(TEXT("save"), false);
        if (!InvokeMutation(*this, TEXT("data_table.add_row"), Payload, false))
        {
            return false;
        }
        TestTrue(TEXT("save:false mutation exists in memory before reload"),
            Table->GetRowMap().Contains(FName(TEXT("MemoryOnly"))));
    }

    Table = ReloadTable();
    if (!TestNotNull(TEXT("DataTable reloads after save:false add_row"), Table))
    {
        return false;
    }
    TestFalse(TEXT("save:false row is absent after a cold reload"),
        Table->GetRowMap().Contains(FName(TEXT("MemoryOnly"))));

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetStringField(TEXT("rowName"), TEXT("Persisted"));
        Payload->SetBoolField(TEXT("save"), false);
        if (!InvokeMutation(*this, TEXT("data_table.remove_row"), Payload, false))
        {
            return false;
        }
        TestFalse(TEXT("save:false remove_row changes memory"),
            Table->GetRowMap().Contains(FName(TEXT("Persisted"))));
    }

    Table = ReloadTable();
    if (!TestNotNull(TEXT("DataTable reloads after save:false remove_row"), Table))
    {
        return false;
    }
    TestTrue(TEXT("save:false remove_row is discarded by a cold reload"),
        Table->GetRowMap().Contains(FName(TEXT("Persisted"))));

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetStringField(TEXT("rowName"), TEXT("Persisted"));
        if (!InvokeMutation(*this, TEXT("data_table.remove_row"), Payload, true))
        {
            return false;
        }
    }

    Table = ReloadTable();
    if (!TestNotNull(TEXT("DataTable reloads after remove_row"), Table))
    {
        return false;
    }
    TestFalse(TEXT("remove_row survives a cold reload"),
        Table->GetRowMap().Contains(FName(TEXT("Persisted"))));

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetStringField(TEXT("rowName"), TEXT("BeforeRebind"));
        Payload->SetObjectField(TEXT("values"), DataTablePersistenceTest::MakeRowValues(TEXT("clear me"), 30));
        if (!InvokeMutation(*this, TEXT("data_table.add_row"), Payload, true))
        {
            return false;
        }
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetStringField(
            TEXT("structPath"), FTestDataTableAtomicRow::StaticStruct()->GetPathName());
        Payload->SetBoolField(TEXT("force"), true);
        Payload->SetBoolField(TEXT("save"), false);
        if (!InvokeMutation(*this, TEXT("data_table.set_row_struct"), Payload, false))
        {
            return false;
        }
        TestTrue(TEXT("save:false row-struct rebind changes memory"),
            Table->RowStruct == FTestDataTableAtomicRow::StaticStruct());
        TestEqual(TEXT("save:false destructive rebind clears rows in memory"),
            Table->GetRowMap().Num(), 0);
    }

    Table = ReloadTable();
    if (!TestNotNull(TEXT("DataTable reloads after save:false set_row_struct"), Table))
    {
        return false;
    }
    TestTrue(TEXT("save:false row-struct rebind is discarded by a cold reload"),
        Table->RowStruct == FTestAssetDumpDataTableRow::StaticStruct());
    TestTrue(TEXT("save:false destructive clear is discarded by a cold reload"),
        Table->GetRowMap().Contains(FName(TEXT("BeforeRebind"))));

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetStringField(
            TEXT("structPath"), FTestDataTableAtomicRow::StaticStruct()->GetPathName());
        Payload->SetBoolField(TEXT("force"), true);
        if (!InvokeMutation(*this, TEXT("data_table.set_row_struct"), Payload, true))
        {
            return false;
        }
    }

    Table = ReloadTable();
    if (!TestNotNull(TEXT("DataTable reloads after set_row_struct"), Table))
    {
        return false;
    }
    TestTrue(TEXT("row struct rebind survives a cold reload"),
        Table->RowStruct == FTestDataTableAtomicRow::StaticStruct());
    TestEqual(TEXT("destructive row clear survives a cold reload"),
        Table->GetRowMap().Num(), 0);
    return true;
}
