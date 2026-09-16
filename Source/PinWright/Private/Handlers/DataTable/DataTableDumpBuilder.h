// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UDataTable;

namespace DataTableDumpBuilder
{
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildDataTableJson(UDataTable* Table);

    // Build a JSON object for a single row by name. Returns null if Table or
    // RowStruct is null, or if the row is not present in the table.
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildRowJson(const UDataTable* Table, FName RowName);

    // Build a JSON object for a row given its struct + memory pointer directly.
    // Lets BuildDataTableJson skip a redundant RowMap lookup per row.
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildRowJsonFromPtr(const UScriptStruct* RowStruct, const uint8* RowMem);
}
