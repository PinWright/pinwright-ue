// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UStruct;

// Test-visible helpers for data_table.add_row / set_row error enrichment.
// Exported (PINWRIGHT_API) so unit tests can link the real
// production symbols without pulling in the handler registration translation
// unit.
namespace DataTableAuthoringInternal
{
    // When FJsonObjectConverter::JsonObjectToUStruct rejects a `values` payload,
    // it only returns a bool — no field, no value, no valid-set. This walks the
    // row struct's authored fields against the supplied `Values` and returns a
    // human-readable detail string for the FIRST field whose value cannot be
    // converted, so the flat [INVALID_ROW_VALUES] error can name the offender.
    //
    // The common, otherwise-undiscoverable case is an enum field set to an
    // invalid literal: ValidateValuesAgainstStruct deliberately skips enum/byte
    // properties (their JSON type is "string", which the type pre-walk can't
    // judge), so the literal failure falls through to the bare converter error.
    // For an enum-typed property (FByteProperty-with-enum or FEnumProperty) given
    // an unresolvable string, the returned detail names the field, the rejected
    // value, the enum, and enumerates the valid literals — e.g.:
    //   Field 'wall Drop' of struct 'S_RoomSettings': value "None" is not a valid
    //   E_EnclosureLevel. Valid values: Closed, Partial, Open
    //
    // Returns an empty string when no specific failing field can be pinpointed
    // (the caller then keeps its original whole-row message). Pure inspection —
    // mutates nothing.
    PINWRIGHT_API FString DescribeRowValueFailure(
        const UStruct* Struct, const TSharedPtr<FJsonObject>& Values);
}
