// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "Tests/TestUtils.h"
#include "Tests/Utility/TestAssetDumpDataAssetFixture.h"

#include "Engine/DataTable.h"
#include "EditorAssetLibrary.h"
#include "UObject/UObjectGlobals.h"

// Regression coverage for F-data-table-create-asset: data_table.create authors a
// fresh UDataTable bound to a row struct. Before this handler existed, no
// data_table.* verb created the asset — every one resolved an EXISTING table and
// bailed ASSET_NOT_FOUND — so the create -> add_row loop had no first step.
// Reverting the handler makes InvokeHandlerWithCapture return false (handler
// absent) and the first assertion of each test fails.

namespace
{
    // Row struct used by the whole DataTable test family — an in-code
    // FTableRowBase fixture ({Label, Score}), never example/Lyra content.
    static FString FixtureRowStructPath()
    {
        return FTestAssetDumpDataTableRow::StaticStruct()->GetPathName();
    }

    static FString UniqueTablePackagePath(const FString& NamePrefix, FString& OutName)
    {
        OutName = FString::Printf(TEXT("%s_%s"), *NamePrefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        return FString::Printf(TEXT("/Game/PinWrightTests/%s"), *OutName);
    }
}

// -----------------------------------------------------------------------------
// create -> the table exists, is bound to the requested struct, and add_row now
// works on it (the create->populate loop the ticket says was impossible).
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableCreateBindsStructAndPopulatesTest,
    "PinWright.data_table.create.BindsStructAndPopulates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableCreateBindsStructAndPopulatesTest::RunTest(const FString& /*Parameters*/)
{
    FString AssetName;
    const FString PackagePath = UniqueTablePackagePath(TEXT("DT_Create"), AssetName);
    const FString Folder = FPackageName::GetLongPackagePath(PackagePath);
    const FString StructPath = FixtureRowStructPath();

    // Always clean up the (unsaved, in-memory) asset this test creates.
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    FString CreatedAssetPath;

    // 1. Create the table bound to the in-code fixture row struct.
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"),       AssetName);
        P->SetStringField(TEXT("path"),       Folder);
        P->SetStringField(TEXT("structPath"), StructPath);

        FTestResponseCapture Cap;
        const bool bFound = InvokeHandlerWithCapture(TEXT("data_table.create"), P, Cap);
        TestTrue(TEXT("data_table.create handler is registered"), bFound);
        TestTrue(FString::Printf(TEXT("create succeeded (err='%s' msg='%s')"),
            *Cap.ErrorCode, *Cap.Message), Cap.bSuccess);
        if (!Cap.bSuccess || !Cap.Result.IsValid()) return false;

        // Response echoes the bound struct + a create signal and carries a
        // resolvable assetPath.
        FString EchoedStruct;
        TestTrue(TEXT("response carries rowStruct"),
            Cap.Result->TryGetStringField(TEXT("rowStruct"), EchoedStruct));
        TestEqual(TEXT("rowStruct == requested struct"), EchoedStruct, StructPath);

        bool bCreated = false;
        TestTrue(TEXT("response carries created flag"),
            Cap.Result->TryGetBoolField(TEXT("created"), bCreated));
        TestTrue(TEXT("created == true"), bCreated);

        TestTrue(TEXT("response carries assetPath"),
            Cap.Result->TryGetStringField(TEXT("assetPath"), CreatedAssetPath));
        TestFalse(TEXT("assetPath is non-empty"), CreatedAssetPath.IsEmpty());
    }

    // 2. The asset really exists and its RowStruct is the requested struct.
    {
        UDataTable* Table = LoadObject<UDataTable>(nullptr, *CreatedAssetPath);
        TestNotNull(TEXT("created DataTable resolves from the returned assetPath"), Table);
        if (!Table) return false;
        TestTrue(TEXT("RowStruct is the requested struct"),
            Table->RowStruct == FTestAssetDumpDataTableRow::StaticStruct());
    }

    // 3. The loop closes: add_row now works on the fresh table. Before the create
    //    handler existed there was no MCP path to reach this state at all.
    {
        TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
        Values->SetStringField(TEXT("Label"), TEXT("Gold"));
        Values->SetNumberField(TEXT("Score"), 100.0);

        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), CreatedAssetPath);
        P->SetStringField(TEXT("rowName"),   TEXT("Drop1"));
        P->SetObjectField(TEXT("values"),    Values);

        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.add_row"), P, Cap);
        TestTrue(FString::Printf(TEXT("add_row on the created table succeeded (err='%s')"),
            *Cap.ErrorCode), Cap.bSuccess);
    }

    // 4. Creating again at the same path is idempotent (AssetCreatePolicy /
    //    B-asset-create-modal-deadlock): the existing table comes back untouched with
    //    existing:true, mode:"updated_in_place" and created:false. The property this
    //    step has always guarded — never a silent overwrite of the table just
    //    populated — is what update-in-place gives by construction; the row added in
    //    step 3 is still there afterwards.
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"),       AssetName);
        P->SetStringField(TEXT("path"),       Folder);
        P->SetStringField(TEXT("structPath"), StructPath);

        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("data_table.create"), P, Cap);
        TestTrue(FString::Printf(TEXT("second create at same path succeeded (err='%s')"),
            *Cap.ErrorCode), Cap.bSuccess);
        if (Cap.bSuccess && Cap.Result.IsValid())
        {
            bool bExisting = false;
            Cap.Result->TryGetBoolField(TEXT("existing"), bExisting);
            TestTrue(TEXT("second create reports existing:true"), bExisting);

            FString Mode;
            Cap.Result->TryGetStringField(TEXT("mode"), Mode);
            TestEqual(TEXT("second create reports mode:updated_in_place"),
                Mode, FString(TEXT("updated_in_place")));

            bool bCreated = true;
            Cap.Result->TryGetBoolField(TEXT("created"), bCreated);
            TestFalse(TEXT("second create reports created:false"), bCreated);
        }

        // The table was not rebuilt: the row added in step 3 survives.
        UDataTable* Table = LoadObject<UDataTable>(nullptr, *CreatedAssetPath);
        TestNotNull(TEXT("table still resolves after the idempotent re-create"), Table);
        if (Table)
        {
            TestTrue(TEXT("the row added before the re-create is still present"),
                Table->GetRowNames().Contains(FName(TEXT("Drop1"))));
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// create with an unresolvable struct path is rejected STRUCT_NOT_FOUND and
// creates nothing (validation happens before the asset is made).
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableCreateBadStructRejectedTest,
    "PinWright.data_table.create.BadStructRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableCreateBadStructRejectedTest::RunTest(const FString& /*Parameters*/)
{
    FString AssetName;
    const FString PackagePath = UniqueTablePackagePath(TEXT("DT_CreateBadStruct"), AssetName);
    const FString Folder = FPackageName::GetLongPackagePath(PackagePath);
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("name"),       AssetName);
    P->SetStringField(TEXT("path"),       Folder);
    P->SetStringField(TEXT("structPath"), TEXT("/Game/Nonexistent/Struct_Bogus.Struct_Bogus"));

    FTestResponseCapture Cap;
    const bool bFound = InvokeHandlerWithCapture(TEXT("data_table.create"), P, Cap);
    TestTrue(TEXT("data_table.create handler is registered"), bFound);
    TestFalse(TEXT("create with a bogus struct failed"), Cap.bSuccess);
    TestEqual(TEXT("error code == STRUCT_NOT_FOUND"),
        Cap.ErrorCode, FString(TEXT("STRUCT_NOT_FOUND")));

    // No asset was created on the validation failure.
    TestFalse(TEXT("no asset exists after a rejected create"),
        UEditorAssetLibrary::DoesAssetExist(PackagePath));
    return true;
}
