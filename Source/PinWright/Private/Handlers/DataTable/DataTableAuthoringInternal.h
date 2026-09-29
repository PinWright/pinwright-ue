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
    // Walks `Values` the way FJsonObjectConverter::JsonObjectToUStruct imports them into the
    // row struct (authored field names, array and set elements, map keys and values, nested
    // struct fields) and returns a human-readable detail for the FIRST enum string the
    // converter would refuse, or empty when every supplied enum literal resolves.
    //
    // add_row and set_row call it BEFORE the converter, so a caller's enum typo is refused as a
    // typed [INVALID_ROW_VALUES] error and never reaches the converter, which would log two
    // LogJson Error lines for it. ValidateValuesAgainstStruct cannot catch these: an enum's
    // JSON type is "string", which a type check cannot judge. The detail names the property
    // path, the rejected value, the enum, and the valid literals - e.g.:
    //   Field 'Modes[2]' of struct 'S_RoomSettings': value "None" is not a valid
    //   E_EnclosureLevel. Valid values: Closed, Partial, Open
    // Map keys use `Field["Key"]` paths and read "key" instead of "value"; nested struct fields
    // use dotted paths (`Inner.Mode`). Pure inspection - mutates nothing.
    PINWRIGHT_API FString DescribeRowValueFailure(
        const UStruct* Struct, const TSharedPtr<FJsonObject>& Values);
}
