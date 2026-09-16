// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/DataTable/DataTableDumpBuilder.h"

#include "Engine/DataTable.h"
#include "JsonObjectConverter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

TSharedPtr<FJsonObject> DataTableDumpBuilder::BuildRowJsonFromPtr(const UScriptStruct* RowStruct, const uint8* RowMem)
{
    if (!RowStruct || !RowMem)
    {
        return nullptr;
    }
    TSharedPtr<FJsonObject> RowJson = MakeShared<FJsonObject>();
    FJsonObjectConverter::UStructToJsonObject(RowStruct, RowMem, RowJson.ToSharedRef(), 0, 0);
    return RowJson;
}

TSharedPtr<FJsonObject> DataTableDumpBuilder::BuildRowJson(const UDataTable* Table, FName RowName)
{
    if (!Table || !Table->RowStruct)
    {
        return nullptr;
    }
    const TMap<FName, uint8*>& RowMap = Table->GetRowMap();
    const uint8* const* RowPtrPtr = RowMap.Find(RowName);
    if (!RowPtrPtr || !*RowPtrPtr)
    {
        return nullptr;
    }
    return BuildRowJsonFromPtr(Table->RowStruct, *RowPtrPtr);
}

TSharedPtr<FJsonObject> DataTableDumpBuilder::BuildDataTableJson(UDataTable* Table)
{
    if (!Table || !Table->RowStruct)
    {
        return nullptr;
    }

    const TMap<FName, uint8*>& RowMap = Table->GetRowMap();

    // Collect (name, ptr) pairs so we can sort by name without re-looking-up the map per row.
    TArray<TPair<FName, const uint8*>> Entries;
    Entries.Reserve(RowMap.Num());
    for (const TPair<FName, uint8*>& Pair : RowMap)
    {
        Entries.Emplace(Pair.Key, Pair.Value);
    }
    Entries.Sort([](const TPair<FName, const uint8*>& A, const TPair<FName, const uint8*>& B)
    {
        return A.Key.ToString() < B.Key.ToString();
    });

    TSharedPtr<FJsonObject> RowsObj = MakeShared<FJsonObject>();
    for (const TPair<FName, const uint8*>& Entry : Entries)
    {
        TSharedPtr<FJsonObject> RowJson = BuildRowJsonFromPtr(Table->RowStruct, Entry.Value);
        if (RowJson.IsValid())
        {
            RowsObj->SetObjectField(Entry.Key.ToString(), RowJson);
        }
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("rowStruct"),     Table->RowStruct->GetPathName());
    Root->SetStringField(TEXT("rowStructName"), Table->RowStruct->GetName());
    Root->SetNumberField(TEXT("rowCount"),      static_cast<double>(RowMap.Num()));
    Root->SetObjectField(TEXT("rows"),          RowsObj);
    return Root;
}

namespace
{
    UClass* GetDataTableSidecarClass()
    {
        return UDataTable::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildDataTableSidecar(UObject* Asset)
    {
        return DataTableDumpBuilder::BuildDataTableJson(Cast<UDataTable>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("data_table"), DumpFileNames::DataTable,
    &GetDataTableSidecarClass, &BuildDataTableSidecar,
    nullptr, nullptr, TEXT("DataTable has no RowStruct."), 100);
