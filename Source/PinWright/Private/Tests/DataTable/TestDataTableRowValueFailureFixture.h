// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "TestDataTableRowValueFailureFixture.generated.h"

// Enum fixture for the [INVALID_ROW_VALUES] enrichment test. Models the
// real-world repro from E-data-table-invalid-row-values-no-field-hint where an
// enum field was set to an invalid literal ("None"). The valid literals here
// stand in for the ticket's Closed/Partial/Open.
UENUM()
enum class ETestEnclosureLevel : uint8
{
    Closed,
    Partial,
    Open
};

// Byte-enum variant (TEnumAsByte → FByteProperty) so the test also covers the
// older enum property kind the enrichment helper handles.
UENUM()
enum ETestEnclosureByteLevel
{
    TestEnclosureByteClosed,
    TestEnclosureBytePartial,
    TestEnclosureByteOpen
};

// Row struct with an enum field plus a plain field. The enum field exercises the
// branch that escapes ValidateValuesAgainstStruct and lands as the bare
// converter failure the ticket is about.
USTRUCT()
struct FTestRoomSettingsRow : public FTableRowBase
{
    GENERATED_BODY()

    UPROPERTY()
    ETestEnclosureLevel WallDrop = ETestEnclosureLevel::Closed;

    UPROPERTY()
    TEnumAsByte<ETestEnclosureByteLevel> RoofDrop = TestEnclosureByteClosed;

    UPROPERTY()
    int32 Area = 0;
};
