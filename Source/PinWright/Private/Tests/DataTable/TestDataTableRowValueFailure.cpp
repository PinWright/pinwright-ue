// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "TestDataTableRowValueFailureFixture.h"

#include "Handlers/DataTable/DataTableAuthoringInternal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"

// Regression test for E-data-table-invalid-row-values-no-field-hint.
//
// Exercises the production symbol DataTableAuthoringInternal::DescribeRowValueFailure
// (the helper that enriches the flat [INVALID_ROW_VALUES] error). If the
// enrichment were reverted (the handler back to its bare "Failed to apply values
// to row ..." message with no per-field detail), this helper would return an
// empty string and the field/value/literal assertions below would fail.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataTableRowValueFailureEnumHintTest,
    "PinWright.data_table.row_value_failure.EnumHint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDataTableRowValueFailureEnumHintTest::RunTest(const FString& Parameters)
{
    const UScriptStruct* RowStruct = FTestRoomSettingsRow::StaticStruct();
    TestNotNull(TEXT("Fixture row struct resolved"), RowStruct);
    if (!RowStruct)
    {
        return false;
    }

    // Invalid enum literal on the FEnumProperty field — the exact ticket repro
    // ("None" is not a member of the enum). Other fields are valid.
    {
        TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
        Values->SetStringField(TEXT("WallDrop"), TEXT("None"));
        Values->SetNumberField(TEXT("Area"), 42.0);

        const FString Detail = DataTableAuthoringInternal::DescribeRowValueFailure(RowStruct, Values);

        TestFalse(TEXT("Detail is non-empty for an invalid enum literal"), Detail.IsEmpty());
        // Names the offending field.
        TestTrue(TEXT("Detail names the failing field"), Detail.Contains(TEXT("WallDrop")));
        // Names the rejected value.
        TestTrue(TEXT("Detail names the rejected value"), Detail.Contains(TEXT("None")));
        // Names the enum type.
        TestTrue(TEXT("Detail names the enum type"), Detail.Contains(TEXT("ETestEnclosureLevel")));
        // Enumerates the valid literals so the caller can self-correct in-band.
        TestTrue(TEXT("Detail lists valid literal Closed"), Detail.Contains(TEXT("Closed")));
        TestTrue(TEXT("Detail lists valid literal Partial"), Detail.Contains(TEXT("Partial")));
        TestTrue(TEXT("Detail lists valid literal Open"), Detail.Contains(TEXT("Open")));
        // The synthetic _MAX sentinel must not leak into the valid set.
        TestFalse(TEXT("Detail omits the synthetic _MAX entry"), Detail.Contains(TEXT("_MAX")));
    }

    // Invalid literal on the TEnumAsByte (FByteProperty) field — the older enum
    // property kind must also be pinpointed.
    {
        TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
        Values->SetStringField(TEXT("RoofDrop"), TEXT("Bogus"));

        const FString Detail = DataTableAuthoringInternal::DescribeRowValueFailure(RowStruct, Values);

        TestFalse(TEXT("Detail is non-empty for an invalid byte-enum literal"), Detail.IsEmpty());
        TestTrue(TEXT("Detail names the byte-enum field"), Detail.Contains(TEXT("RoofDrop")));
        TestTrue(TEXT("Detail names the rejected byte-enum value"), Detail.Contains(TEXT("Bogus")));
    }

    // A valid enum literal must NOT be flagged — the helper returns empty so the
    // caller keeps the (here unreached) original message and reports no spurious
    // field failure.
    {
        TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
        Values->SetStringField(TEXT("WallDrop"), TEXT("Open"));
        Values->SetStringField(TEXT("RoofDrop"), TEXT("TestEnclosureByteOpen"));

        const FString Detail = DataTableAuthoringInternal::DescribeRowValueFailure(RowStruct, Values);
        TestTrue(TEXT("Detail is empty when all enum literals are valid"), Detail.IsEmpty());
    }

    return true;
}
