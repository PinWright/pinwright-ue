// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "Tests/TestUtils.h"
#include "Tests/Utility/TestAssetDumpDataAssetFixture.h"

#include "Engine/DataTable.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    // Build a transient UDataTable rooted to a unique transient package, bound to
    // FTestAssetDumpDataTableRow. The returned path is the table's object path
    // (suitable as the `assetPath` payload field). Caller is responsible for
    // releasing the root reference via ON_SCOPE_EXIT.
    static UDataTable* MakeTransientDataTable(FString& OutAssetPath)
    {
        const FString PackagePath = FString::Printf(TEXT("/Engine/Transient/DT_AuthoringTest_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPackage* Package = CreatePackage(*PackagePath);
        UDataTable* Table = NewObject<UDataTable>(
            Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transient);
        if (Table)
        {
            Table->RowStruct = FTestAssetDumpDataTableRow::StaticStruct();
            Table->AddToRoot();
            OutAssetPath = Table->GetPathName();
        }
        return Table;
    }

    // Wrap a values object for the standard {Label, Score} row.
    static TSharedPtr<FJsonObject> MakeRowValues(const FString& Label, int32 Score)
    {
        TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
        Values->SetStringField(TEXT("Label"), Label);
        Values->SetNumberField(TEXT("Score"), static_cast<double>(Score));
        return Values;
    }
}

// -----------------------------------------------------------------------------
// add_row -> list_rows round-trip
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableAddRowThenListRowsRoundTripTest,
    "PinWright.data_table.add_row.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableAddRowThenListRowsRoundTripTest::RunTest(const FString& /*Parameters*/)
{
    FString AssetPath;
    UDataTable* Table = MakeTransientDataTable(AssetPath);
    TestNotNull(TEXT("Transient DataTable created"), Table);
    if (!Table) return false;
    ON_SCOPE_EXIT { Table->RemoveFromRoot(); };

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("rowName"),   TEXT("R1"));
        Payload->SetObjectField(TEXT("values"),    MakeRowValues(TEXT("Hello"), 42));

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("data_table.add_row"), Payload, Capture);
        TestTrue(TEXT("add_row handler found"), bFound);
        TestTrue(FString::Printf(TEXT("add_row succeeded (err='%s' msg='%s')"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("data_table.list_rows"), Payload, Capture);
        TestTrue(TEXT("list_rows handler found"), bFound);
        TestTrue(TEXT("list_rows succeeded"), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid()) return false;

        const double RowCount = Capture.Result->GetNumberField(TEXT("rowCount"));
        TestEqual(TEXT("rowCount == 1"), static_cast<int32>(RowCount), 1);

        const TSharedPtr<FJsonObject>* RowsObj = nullptr;
        if (!Capture.Result->TryGetObjectField(TEXT("rows"), RowsObj) || !RowsObj || !RowsObj->IsValid())
        {
            AddError(TEXT("rows field missing or wrong type"));
            return false;
        }
        const TSharedPtr<FJsonObject>* R1 = nullptr;
        if (!(*RowsObj)->TryGetObjectField(TEXT("R1"), R1) || !R1 || !R1->IsValid())
        {
            AddError(TEXT("R1 row missing in rows map"));
            return false;
        }
        TestEqual(TEXT("R1.Label == Hello"),
            (*R1)->GetStringField(TEXT("Label")), FString(TEXT("Hello")));
        TestEqual(TEXT("R1.Score == 42"),
            static_cast<int32>((*R1)->GetNumberField(TEXT("Score"))), 42);
    }

    return true;
}

// -----------------------------------------------------------------------------
// add_row duplicate -> ROW_EXISTS
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableAddRowDuplicateNameRejectedTest,
    "PinWright.data_table.add_row.DuplicateRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableAddRowDuplicateNameRejectedTest::RunTest(const FString& /*Parameters*/)
{
    FString AssetPath;
    UDataTable* Table = MakeTransientDataTable(AssetPath);
    TestNotNull(TEXT("Transient DataTable created"), Table);
    if (!Table) return false;
    ON_SCOPE_EXIT { Table->RemoveFromRoot(); };

    auto MakePayload = [&]()
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        P->SetStringField(TEXT("rowName"),   TEXT("R1"));
        P->SetObjectField(TEXT("values"),    MakeRowValues(TEXT("X"), 1));
        return P;
    };

    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("data_table.add_row"), MakePayload(), Capture);
        TestTrue(TEXT("first add_row succeeded"), Capture.bSuccess);
    }
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("data_table.add_row"), MakePayload(), Capture);
        TestFalse(TEXT("second add_row failed"), Capture.bSuccess);
        TestEqual(TEXT("error code == ROW_EXISTS"),
            Capture.ErrorCode, FString(TEXT("ROW_EXISTS")));
    }
    return true;
}

// -----------------------------------------------------------------------------
// set_row on existing row updates values
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableSetRowExistingUpdatesBytesTest,
    "PinWright.data_table.set_row.UpdatesExisting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableSetRowExistingUpdatesBytesTest::RunTest(const FString& /*Parameters*/)
{
    FString AssetPath;
    UDataTable* Table = MakeTransientDataTable(AssetPath);
    TestNotNull(TEXT("Transient DataTable created"), Table);
    if (!Table) return false;
    ON_SCOPE_EXIT { Table->RemoveFromRoot(); };

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        P->SetStringField(TEXT("rowName"),   TEXT("R1"));
        P->SetObjectField(TEXT("values"),    MakeRowValues(TEXT("init"), 1));
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.add_row"), P, Cap);
        TestTrue(TEXT("add_row succeeded"), Cap.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        P->SetStringField(TEXT("rowName"),   TEXT("R1"));
        P->SetObjectField(TEXT("values"),    MakeRowValues(TEXT("init"), 99));
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.set_row"), P, Cap);
        TestTrue(TEXT("set_row succeeded"), Cap.bSuccess);
        if (!Cap.bSuccess || !Cap.Result.IsValid()) return false;

        // A genuine in-place overwrite must report an UPDATE: keyed "updated"
        // with created=false, and must NOT key "added". The mirror of the
        // create-path assertion in the CreateIfMissing test.
        FString UpdatedName;
        TestTrue(TEXT("overwrite result keys 'updated'"),
            Cap.Result->TryGetStringField(TEXT("updated"), UpdatedName));
        TestEqual(TEXT("'updated' names the row"), UpdatedName, FString(TEXT("R1")));
        TestFalse(TEXT("overwrite result does NOT key 'added'"),
            Cap.Result->HasField(TEXT("added")));
        bool bCreated = true;
        TestTrue(TEXT("overwrite result carries 'created' flag"),
            Cap.Result->TryGetBoolField(TEXT("created"), bCreated));
        TestFalse(TEXT("created == false for an overwrite"), bCreated);
    }
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.list_rows"), P, Cap);
        TestTrue(TEXT("list_rows succeeded"), Cap.bSuccess);
        if (!Cap.bSuccess || !Cap.Result.IsValid()) return false;

        const TSharedPtr<FJsonObject>* RowsObj = nullptr;
        Cap.Result->TryGetObjectField(TEXT("rows"), RowsObj);
        const TSharedPtr<FJsonObject>* R1 = nullptr;
        if (RowsObj && (*RowsObj)->TryGetObjectField(TEXT("R1"), R1))
        {
            TestEqual(TEXT("R1.Score == 99"),
                static_cast<int32>((*R1)->GetNumberField(TEXT("Score"))), 99);
        }
        else
        {
            AddError(TEXT("R1 row missing"));
            return false;
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// set_row on missing row: createIfMissing=true adds; otherwise ROW_NOT_FOUND.
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableSetRowMissingCreateIfMissingTrueAddsTest,
    "PinWright.data_table.set_row.CreateIfMissing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableSetRowMissingCreateIfMissingTrueAddsTest::RunTest(const FString& /*Parameters*/)
{
    FString AssetPath;
    UDataTable* Table = MakeTransientDataTable(AssetPath);
    TestNotNull(TEXT("Transient DataTable created"), Table);
    if (!Table) return false;
    ON_SCOPE_EXIT { Table->RemoveFromRoot(); };

    // Without createIfMissing → ROW_NOT_FOUND.
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        P->SetStringField(TEXT("rowName"),   TEXT("Ghost"));
        P->SetObjectField(TEXT("values"),    MakeRowValues(TEXT("g"), 1));
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.set_row"), P, Cap);
        TestFalse(TEXT("set_row without createIfMissing failed"), Cap.bSuccess);
        TestEqual(TEXT("error code == ROW_NOT_FOUND"),
            Cap.ErrorCode, FString(TEXT("ROW_NOT_FOUND")));
    }
    // With createIfMissing → success and row appears. Because this branch ran
    // ApplyValuesToNewRow on a row that provably did not exist, the result must
    // report a CREATE, not an update: it keys "added" (matching add_row's create
    // result) with created=true, and must NOT key "updated". Regression guard for
    // E-set-row-create-reports-updated — reverting the fix re-keys this "updated".
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"),       AssetPath);
        P->SetStringField(TEXT("rowName"),         TEXT("Ghost"));
        P->SetObjectField(TEXT("values"),          MakeRowValues(TEXT("g"), 7));
        P->SetBoolField  (TEXT("createIfMissing"), true);
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.set_row"), P, Cap);
        TestTrue(TEXT("set_row with createIfMissing succeeded"), Cap.bSuccess);
        if (!Cap.bSuccess || !Cap.Result.IsValid()) return false;

        FString AddedName;
        TestTrue(TEXT("createIfMissing-create result keys 'added' (not 'updated')"),
            Cap.Result->TryGetStringField(TEXT("added"), AddedName));
        TestEqual(TEXT("'added' names the created row"), AddedName, FString(TEXT("Ghost")));
        TestFalse(TEXT("createIfMissing-create result does NOT key 'updated'"),
            Cap.Result->HasField(TEXT("updated")));

        bool bCreated = false;
        TestTrue(TEXT("createIfMissing-create result carries 'created' flag"),
            Cap.Result->TryGetBoolField(TEXT("created"), bCreated));
        TestTrue(TEXT("created == true for a newly created row"), bCreated);
    }
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.list_rows"), P, Cap);
        TestTrue(TEXT("list_rows succeeded"), Cap.bSuccess);
        if (!Cap.bSuccess || !Cap.Result.IsValid()) return false;
        TestEqual(TEXT("rowCount == 1"),
            static_cast<int32>(Cap.Result->GetNumberField(TEXT("rowCount"))), 1);
    }
    return true;
}

// -----------------------------------------------------------------------------
// remove_row present then absent
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableRemoveRowPresentAbsentTest,
    "PinWright.data_table.remove_row.PresentThenAbsent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableRemoveRowPresentAbsentTest::RunTest(const FString& /*Parameters*/)
{
    FString AssetPath;
    UDataTable* Table = MakeTransientDataTable(AssetPath);
    TestNotNull(TEXT("Transient DataTable created"), Table);
    if (!Table) return false;
    ON_SCOPE_EXIT { Table->RemoveFromRoot(); };

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        P->SetStringField(TEXT("rowName"),   TEXT("R1"));
        P->SetObjectField(TEXT("values"),    MakeRowValues(TEXT("x"), 1));
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.add_row"), P, Cap);
        TestTrue(TEXT("add_row succeeded"), Cap.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        P->SetStringField(TEXT("rowName"),   TEXT("R1"));
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.remove_row"), P, Cap);
        TestTrue(TEXT("remove_row succeeded"), Cap.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.list_rows"), P, Cap);
        TestTrue(TEXT("list_rows succeeded"), Cap.bSuccess);
        if (!Cap.bSuccess || !Cap.Result.IsValid()) return false;
        TestEqual(TEXT("rowCount == 0 after remove"),
            static_cast<int32>(Cap.Result->GetNumberField(TEXT("rowCount"))), 0);
    }
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        P->SetStringField(TEXT("rowName"),   TEXT("R1"));
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.remove_row"), P, Cap);
        TestFalse(TEXT("second remove_row failed"), Cap.bSuccess);
        TestEqual(TEXT("error code == ROW_NOT_FOUND"),
            Cap.ErrorCode, FString(TEXT("ROW_NOT_FOUND")));
    }
    return true;
}

// -----------------------------------------------------------------------------
// set_row_struct without force on non-empty table -> ROWS_PRESENT_FORCE_REQUIRED.
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableSetRowStructNonEmptyWithoutForceRejectedTest,
    "PinWright.data_table.set_row_struct.RejectedWithoutForce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableSetRowStructNonEmptyWithoutForceRejectedTest::RunTest(const FString& /*Parameters*/)
{
    FString AssetPath;
    UDataTable* Table = MakeTransientDataTable(AssetPath);
    TestNotNull(TEXT("Transient DataTable created"), Table);
    if (!Table) return false;
    ON_SCOPE_EXIT { Table->RemoveFromRoot(); };

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        P->SetStringField(TEXT("rowName"),   TEXT("R1"));
        P->SetObjectField(TEXT("values"),    MakeRowValues(TEXT("x"), 1));
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.add_row"), P, Cap);
        TestTrue(TEXT("add_row succeeded"), Cap.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"),  AssetPath);
        P->SetStringField(TEXT("structPath"), FTestAssetDumpDataTableRow::StaticStruct()->GetPathName());
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.set_row_struct"), P, Cap);
        TestFalse(TEXT("set_row_struct without force failed"), Cap.bSuccess);
        TestEqual(TEXT("error code == ROWS_PRESENT_FORCE_REQUIRED"),
            Cap.ErrorCode, FString(TEXT("ROWS_PRESENT_FORCE_REQUIRED")));
    }
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.list_rows"), P, Cap);
        TestTrue(TEXT("list_rows succeeded"), Cap.bSuccess);
        if (!Cap.bSuccess || !Cap.Result.IsValid()) return false;
        TestEqual(TEXT("rowCount stays 1"),
            static_cast<int32>(Cap.Result->GetNumberField(TEXT("rowCount"))), 1);
    }
    return true;
}

// -----------------------------------------------------------------------------
// set_row_struct with force=true clears rows and rebinds.
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableSetRowStructWithForceClearsTest,
    "PinWright.data_table.set_row_struct.ForceClearsRows",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableSetRowStructWithForceClearsTest::RunTest(const FString& /*Parameters*/)
{
    FString AssetPath;
    UDataTable* Table = MakeTransientDataTable(AssetPath);
    TestNotNull(TEXT("Transient DataTable created"), Table);
    if (!Table) return false;
    ON_SCOPE_EXIT { Table->RemoveFromRoot(); };

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        P->SetStringField(TEXT("rowName"),   TEXT("R1"));
        P->SetObjectField(TEXT("values"),    MakeRowValues(TEXT("x"), 1));
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.add_row"), P, Cap);
        TestTrue(TEXT("add_row succeeded"), Cap.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"),  AssetPath);
        P->SetStringField(TEXT("structPath"), FTestAssetDumpDataTableRow::StaticStruct()->GetPathName());
        P->SetBoolField  (TEXT("force"),      true);
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.set_row_struct"), P, Cap);
        TestTrue(FString::Printf(TEXT("set_row_struct with force succeeded (err='%s')"), *Cap.ErrorCode),
            Cap.bSuccess);
        if (Cap.bSuccess && Cap.Result.IsValid())
        {
            TestEqual(TEXT("clearedRows == 1"),
                static_cast<int32>(Cap.Result->GetNumberField(TEXT("clearedRows"))), 1);
        }
    }
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.list_rows"), P, Cap);
        TestTrue(TEXT("list_rows succeeded"), Cap.bSuccess);
        if (!Cap.bSuccess || !Cap.Result.IsValid()) return false;
        TestEqual(TEXT("rowCount == 0 after force-rebind"),
            static_cast<int32>(Cap.Result->GetNumberField(TEXT("rowCount"))), 0);
    }
    return true;
}

// -----------------------------------------------------------------------------
// add_row with bad-typed field values is rejected; no orphan row remains.
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableAddRowInvalidFieldRejectsTest,
    "PinWright.data_table.add_row.InvalidFieldRejects",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableAddRowInvalidFieldRejectsTest::RunTest(const FString& /*Parameters*/)
{
    FString AssetPath;
    UDataTable* Table = MakeTransientDataTable(AssetPath);
    TestNotNull(TEXT("Transient DataTable created"), Table);
    if (!Table) return false;
    ON_SCOPE_EXIT { Table->RemoveFromRoot(); };

    {
        // Label expects FString, Score expects int32 — swap the JSON types.
        TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
        Values->SetNumberField(TEXT("Label"), 123.0);
        Values->SetStringField(TEXT("Score"), TEXT("oops"));

        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        P->SetStringField(TEXT("rowName"),   TEXT("R1"));
        P->SetObjectField(TEXT("values"),    Values);

        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.add_row"), P, Cap);
        TestFalse(TEXT("add_row with invalid values failed"), Cap.bSuccess);
        // Pin the code as every sibling here does. Both keys DO match a field, so
        // RejectUnmatchedKeys' INVALID_PARAMS path is not the one under test; the
        // rejection must come from ValidateValuesAgainstStruct, which reports
        // INVALID_ARGUMENT (DataTableAuthoringHandler.cpp, ApplyValuesToNewRow).
        // A bare TestFalse(bSuccess) was satisfied by any failure at all, including
        // the row never being reached.
        TestEqual(TEXT("error code == INVALID_ARGUMENT"),
            Cap.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.list_rows"), P, Cap);
        TestTrue(TEXT("list_rows succeeded"), Cap.bSuccess);
        if (!Cap.bSuccess || !Cap.Result.IsValid()) return false;
        TestEqual(TEXT("rowCount == 0 (no orphan)"),
            static_cast<int32>(Cap.Result->GetNumberField(TEXT("rowCount"))), 0);
    }
    return true;
}

// -----------------------------------------------------------------------------
// add_row with `values` keys that match no struct field is rejected with
// INVALID_PARAMS + droppedFields; no orphan row is created. Guards the
// silent-drop / success-with-no-effect bug (B-data-table-row-values-silent-drop):
// the converter (JsonObjectToUStruct) silently skips unmatched keys, so before
// the fix this all-keys-unmatched call reported {"added":...} with an
// all-defaults row.
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableAddRowUnmatchedKeysRejectedTest,
    "PinWright.data_table.add_row.UnmatchedKeysRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableAddRowUnmatchedKeysRejectedTest::RunTest(const FString& /*Parameters*/)
{
    FString AssetPath;
    UDataTable* Table = MakeTransientDataTable(AssetPath);
    TestNotNull(TEXT("Transient DataTable created"), Table);
    if (!Table) return false;
    ON_SCOPE_EXIT { Table->RemoveFromRoot(); };

    {
        // Both keys are typos / made-up names that resolve to no field on the
        // {Label, Score} row struct — under the old code both are silently
        // dropped and add_row reports a successful create with an all-defaults row.
        // NOTE keys must mismatch even case-INSENSITIVELY: the converter looks up
        // each field via TMap<FString>::Find (JsonObjectConverter.cpp:1174), whose
        // FString key compare is case-insensitive, so "score" would actually MATCH
        // "Score" and be consumed. We use "scoreValue" — a true non-match the
        // converter drops — to exercise the unmatched-key rejection.
        TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
        Values->SetNumberField(TEXT("completelyMadeUpField"), 42.0);
        Values->SetStringField(TEXT("scoreValue"), TEXT("near-miss-typo")); // != "Score" even case-insensitively

        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        P->SetStringField(TEXT("rowName"),   TEXT("BogusOnly"));
        P->SetObjectField(TEXT("values"),    Values);

        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.add_row"), P, Cap);
        TestFalse(TEXT("add_row with all-unmatched keys failed"), Cap.bSuccess);
        TestEqual(TEXT("error code == INVALID_PARAMS"),
            Cap.ErrorCode, FString(TEXT("INVALID_PARAMS")));

        // droppedFields should name both unmatched keys.
        if (Cap.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Dropped = nullptr;
            if (Cap.Result->TryGetArrayField(TEXT("droppedFields"), Dropped) && Dropped)
            {
                TSet<FString> DroppedSet;
                for (const TSharedPtr<FJsonValue>& V : *Dropped)
                {
                    if (V.IsValid()) DroppedSet.Add(V->AsString());
                }
                TestTrue(TEXT("droppedFields contains completelyMadeUpField"),
                    DroppedSet.Contains(TEXT("completelyMadeUpField")));
                TestTrue(TEXT("droppedFields contains scoreValue"),
                    DroppedSet.Contains(TEXT("scoreValue")));
            }
            else
            {
                AddError(TEXT("droppedFields array missing from error result"));
            }
        }
        else
        {
            AddError(TEXT("error result object missing"));
        }
    }
    // No orphan row was created.
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.list_rows"), P, Cap);
        TestTrue(TEXT("list_rows succeeded"), Cap.bSuccess);
        if (!Cap.bSuccess || !Cap.Result.IsValid()) return false;
        TestEqual(TEXT("rowCount == 0 (no orphan)"),
            static_cast<int32>(Cap.Result->GetNumberField(TEXT("rowCount"))), 0);
    }
    return true;
}

// -----------------------------------------------------------------------------
// set_row with an unmatched key is rejected with INVALID_PARAMS and leaves the
// existing row untouched (no success-with-no-effect update).
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableSetRowUnmatchedKeysRejectedTest,
    "PinWright.data_table.set_row.UnmatchedKeysRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableSetRowUnmatchedKeysRejectedTest::RunTest(const FString& /*Parameters*/)
{
    FString AssetPath;
    UDataTable* Table = MakeTransientDataTable(AssetPath);
    TestNotNull(TEXT("Transient DataTable created"), Table);
    if (!Table) return false;
    ON_SCOPE_EXIT { Table->RemoveFromRoot(); };

    // Seed an existing row with Score == 14.
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        P->SetStringField(TEXT("rowName"),   TEXT("Studio"));
        P->SetObjectField(TEXT("values"),    MakeRowValues(TEXT("seed"), 14));
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.add_row"), P, Cap);
        TestTrue(TEXT("seed add_row succeeded"), Cap.bSuccess);
    }
    // set_row with a typo'd key ("heightX" -> no field) must be rejected, not a no-op success.
    {
        TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
        Values->SetNumberField(TEXT("heightX"), 777.0);
        Values->SetNumberField(TEXT("bogusKey"), 1.0);

        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        P->SetStringField(TEXT("rowName"),   TEXT("Studio"));
        P->SetObjectField(TEXT("values"),    Values);
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.set_row"), P, Cap);
        TestFalse(TEXT("set_row with unmatched keys failed"), Cap.bSuccess);
        TestEqual(TEXT("error code == INVALID_PARAMS"),
            Cap.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    }
    // The existing row is unchanged (Score still 14).
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), AssetPath);
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.list_rows"), P, Cap);
        TestTrue(TEXT("list_rows succeeded"), Cap.bSuccess);
        if (!Cap.bSuccess || !Cap.Result.IsValid()) return false;

        const TSharedPtr<FJsonObject>* RowsObj = nullptr;
        Cap.Result->TryGetObjectField(TEXT("rows"), RowsObj);
        const TSharedPtr<FJsonObject>* Studio = nullptr;
        if (RowsObj && (*RowsObj)->TryGetObjectField(TEXT("Studio"), Studio))
        {
            TestEqual(TEXT("Studio.Score unchanged == 14"),
                static_cast<int32>((*Studio)->GetNumberField(TEXT("Score"))), 14);
        }
        else
        {
            AddError(TEXT("Studio row missing"));
            return false;
        }
    }
    return true;
}
