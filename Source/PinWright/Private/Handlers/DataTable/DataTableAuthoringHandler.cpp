// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/DataTable/DataTableDumpBuilder.h"
#include "Handlers/DataTable/DataTableAuthoringInternal.h"
#include "Handlers/Blueprint/BlueprintEnumHelpers.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/AssetUtils.h"
#include "Utils/JsonUtils.h"

#include "Engine/DataTable.h"
#include "DataTableEditorUtils.h"
#include "JsonObjectConverter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"

// data_table.create dependencies (author a fresh UDataTable asset from scratch).
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorAssetLibrary.h"
#include "Factories/DataTableFactory.h"
#include "Modules/ModuleManager.h"

namespace
{
    // Resolve a UDataTable from `assetPath` payload param. On error, sends the
    // response on Ctx and returns false. On success, fills OutPath/OutTable.
    static bool ResolveDataTable(FHandlerContext& Ctx, FString& OutPath, UDataTable*& OutTable)
    {
        OutTable = nullptr;
        if (!Ctx.RequireAssetPath(TEXT("assetPath"), OutPath))
        {
            return false;
        }
        UDataTable* Table = LoadObject<UDataTable>(nullptr, *OutPath);
        if (!Table)
        {
            Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
                FString::Printf(TEXT("Could not load DataTable: %s"), *OutPath));
            return false;
        }
        OutTable = Table;
        return true;
    }

    // Resolve + validate a UScriptStruct as a DataTable row struct from a path.
    // On error, sends STRUCT_NOT_FOUND / INVALID_ROW_STRUCT on Ctx and returns false.
    // On success, fills OutStruct. Mirrors ResolveDataTable's send-and-return-false
    // contract so data_table.create and data_table.set_row_struct share one gate.
    static bool ResolveRowStruct(FHandlerContext& Ctx, const FString& StructPath, UScriptStruct*& OutStruct)
    {
        OutStruct = nullptr;
        UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
        if (!Struct)
        {
            Ctx.SendError(TEXT("STRUCT_NOT_FOUND"),
                FString::Printf(TEXT("Could not load UScriptStruct: %s"), *StructPath));
            return false;
        }
        if (!FDataTableEditorUtils::IsValidTableStruct(Struct))
        {
            Ctx.SendError(TEXT("INVALID_ROW_STRUCT"),
                FString::Printf(TEXT("Struct '%s' is not a valid DataTable row struct"), *StructPath));
            return false;
        }
        OutStruct = Struct;
        return true;
    }

    // Human-readable label for a JSON value type, used in validation error messages.
    static const TCHAR* JsonTypeName(EJson Type)
    {
        switch (Type)
        {
            case EJson::None:    return TEXT("none");
            case EJson::Null:    return TEXT("null");
            case EJson::String:  return TEXT("string");
            case EJson::Number:  return TEXT("number");
            case EJson::Boolean: return TEXT("boolean");
            case EJson::Array:   return TEXT("array");
            case EJson::Object:  return TEXT("object");
            default:             return TEXT("unknown");
        }
    }

    // The JSON-type label we expect for a given UE property kind. Returns nullptr
    // for property kinds we don't pre-validate (e.g. enums, soft refs) — in that
    // case the converter's own behaviour stands.
    static const TCHAR* ExpectedJsonTypeName(const FProperty* Prop)
    {
        if (Prop->IsA<FStrProperty>() || Prop->IsA<FNameProperty>() || Prop->IsA<FTextProperty>())
        {
            return TEXT("string");
        }
        if (Prop->IsA<FNumericProperty>())
        {
            return TEXT("number");
        }
        if (Prop->IsA<FBoolProperty>())
        {
            return TEXT("boolean");
        }
        if (Prop->IsA<FStructProperty>() || Prop->IsA<FMapProperty>())
        {
            return TEXT("object");
        }
        if (Prop->IsA<FArrayProperty>() || Prop->IsA<FSetProperty>())
        {
            return TEXT("array");
        }
        return nullptr;
    }

    // Returns true if the JSON value's type is acceptable for the given UE property,
    // mirroring the table in ExpectedJsonTypeName. Null is allowed (treated as
    // "use default"). FNumericProperty-with-enum is treated as numeric.
    static bool IsJsonValueTypeCompatible(const FProperty* Prop, const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid() || Value->Type == EJson::Null)
        {
            return true;
        }
        const TCHAR* Expected = ExpectedJsonTypeName(Prop);
        if (!Expected)
        {
            // Unhandled property kind — defer to the converter.
            return true;
        }
        const EJson Actual = Value->Type;
        if (FCString::Stricmp(Expected, TEXT("string")) == 0)   return Actual == EJson::String;
        if (FCString::Stricmp(Expected, TEXT("number")) == 0)   return Actual == EJson::Number;
        if (FCString::Stricmp(Expected, TEXT("boolean")) == 0)  return Actual == EJson::Boolean;
        if (FCString::Stricmp(Expected, TEXT("object")) == 0)   return Actual == EJson::Object;
        if (FCString::Stricmp(Expected, TEXT("array")) == 0)    return Actual == EJson::Array;
        return true;
    }

    // Pre-validate that each JSON field whose name matches a row-struct property
    // carries a JSON type compatible with that property. The built-in converter
    // is permissive (silent number->string coercion, etc.); this check rejects
    // mismatches up front so callers get a clear error and no orphan row is left.
    // Returns true on pass; on failure, fills OutErrorMsg and returns false.
    static bool ValidateValuesAgainstStruct(const UScriptStruct* Struct,
        const TSharedPtr<FJsonObject>& Values, FString& OutErrorMsg)
    {
        if (!Struct || !Values.IsValid())
        {
            return true;
        }
        for (TFieldIterator<FProperty> It(Struct); It; ++It)
        {
            const FProperty* Prop = *It;
            // FJsonObjectConverter::JsonObjectToUStruct matches on the struct's
            // *authored* name (GetAuthoredNameForField) — for a UserDefinedStruct
            // this is the display name with its first letter lowercased and spaces
            // preserved (e.g. "roof Drop"), which differs from FProperty::GetName().
            // We must use the same resolver so the keys we validate exactly match
            // the keys the converter would consume (no false positives/negatives).
            const FString FieldName = Struct->GetAuthoredNameForField(Prop);
            const TSharedPtr<FJsonValue> Value = Values->TryGetField(FieldName);
            if (!Value.IsValid())
            {
                continue; // Missing field is allowed; converter leaves default.
            }
            if (!IsJsonValueTypeCompatible(Prop, Value))
            {
                const TCHAR* Expected = ExpectedJsonTypeName(Prop);
                OutErrorMsg = FString::Printf(
                    TEXT("Field '%s' expects JSON %s but got %s"),
                    *FieldName,
                    Expected ? Expected : TEXT("?"),
                    JsonTypeName(Value->Type));
                return false;
            }
        }
        return true;
    }

    // Detect `values` keys that resolve to no field on the row struct. The
    // converter (JsonObjectToUStruct) silently skips such keys — no error, no
    // signal — so a typo / wrong key form / flat-vs-nested mismatch lands as a
    // success-shaped no-op (every key unmatched => an all-defaults row reported
    // as a successful add/update). We reject the whole call up front instead.
    // On failure, fills a structured error Result carrying `droppedFields`
    // (the unmatched keys, in payload order) and `validFields` (the struct's
    // authored field names) and returns false; on pass returns true.
    static bool ValidateNoUnmatchedKeys(const UStruct* Struct,
        const TSharedPtr<FJsonObject>& Values,
        FString& OutErrorMsg, TSharedPtr<FJsonObject>& OutErrorResult)
    {
        if (!Struct || !Values.IsValid())
        {
            return true;
        }

        // Authored field names the converter would accept, in declaration order.
        // The set drives the membership test; the array preserves order for the
        // human-readable message and the structured `validFields` payload.
        TArray<FString> ValidFieldNames;
        TSet<FString> ValidFieldSet;
        for (TFieldIterator<FProperty> It(Struct); It; ++It)
        {
            const FString FieldName = Struct->GetAuthoredNameForField(*It);
            ValidFieldNames.Add(FieldName);
            ValidFieldSet.Add(FieldName);
        }

        // Any supplied key not in the field set is dropped by the converter.
        TArray<FString> DroppedNames;
        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : Values->Values)
        {
            if (!ValidFieldSet.Contains(Pair.Key))
            {
                DroppedNames.Add(Pair.Key);
            }
        }

        if (DroppedNames.Num() == 0)
        {
            return true;
        }

        OutErrorMsg = FString::Printf(
            TEXT("values contains %d key(s) that match no field on row struct '%s' and would be silently dropped: %s. Valid field keys: %s"),
            DroppedNames.Num(),
            *Struct->GetName(),
            *FString::Join(DroppedNames, TEXT(", ")),
            *FString::Join(ValidFieldNames, TEXT(", ")));

        OutErrorResult = MakeShared<FJsonObject>();
        OutErrorResult->SetArrayField(TEXT("droppedFields"), EmitStringArray(DroppedNames));
        OutErrorResult->SetArrayField(TEXT("validFields"), EmitStringArray(ValidFieldNames));
        return false;
    }

    // Run ValidateNoUnmatchedKeys and, on failure, send the structured INVALID_PARAMS
    // error on Ctx. Returns true when the call should be rejected (caller returns
    // immediately), false when the keys all match a field and the call may proceed.
    static bool RejectUnmatchedKeys(FHandlerContext& Ctx, const UStruct* Struct,
        const TSharedPtr<FJsonObject>& Values)
    {
        FString UnmatchedMsg;
        TSharedPtr<FJsonObject> UnmatchedResult;
        if (!ValidateNoUnmatchedKeys(Struct, Values, UnmatchedMsg, UnmatchedResult))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"), UnmatchedMsg, UnmatchedResult);
            return true;
        }
        return false;
    }

    // Append the offending field + value (+ valid enum literals) to a base
    // INVALID_ROW_VALUES message so the caller can self-correct without an
    // out-of-band asset.dump. Single source of truth for the enriched message
    // shape shared by add_row and set_row's converter-failure paths.
    static FString WithRowValueDetail(const FString& Base, const UStruct* RowStruct,
        const TSharedPtr<FJsonObject>& Values)
    {
        const FString Detail = DataTableAuthoringInternal::DescribeRowValueFailure(RowStruct, Values);
        return Detail.IsEmpty() ? Base : Base + FString::Printf(TEXT(". %s"), *Detail);
    }

    static void AddDataTableSaveReport(const TSharedPtr<FJsonObject>& Result,
        UDataTable* Table, bool bSaveRequested)
    {
        EAssetSaveState SaveState = bSaveRequested
            ? EAssetSaveState::Failed
            : EAssetSaveState::NotRequested;
        const bool bSaved = bSaveRequested
            && SaveAssetToDiskReportingPresence(
                Table, /*bForce=*/true, nullptr, nullptr, &SaveState);
        AddAssetSaveReport(Result, bSaveRequested, bSaved, SaveState);
    }

    // Shared "Modify + AddRow + JsonObjectToUStruct + rollback" sequence for
    // add_row and set_row's createIfMissing branch. Returns the newly-added row
    // memory on success; on failure, fills OutErrorCode/OutErrorMsg, rolls back
    // the just-added row (if any), and returns null.
    static uint8* ApplyValuesToNewRow(UDataTable* Table, FName RowName,
        const TSharedPtr<FJsonObject>& Values,
        FString& OutErrorCode, FString& OutErrorMsg)
    {
        // Pre-validate JSON types against the row struct before mutating the
        // table, so a bad payload never produces an orphan row.
        FString ValidationMsg;
        if (!ValidateValuesAgainstStruct(Table->RowStruct, Values, ValidationMsg))
        {
            OutErrorCode = TEXT("INVALID_ARGUMENT");
            OutErrorMsg = ValidationMsg;
            return nullptr;
        }

        Table->Modify();
        uint8* RowMem = FDataTableEditorUtils::AddRow(Table, RowName);
        if (!RowMem)
        {
            OutErrorCode = TEXT("ADD_ROW_FAILED");
            OutErrorMsg = FString::Printf(TEXT("FDataTableEditorUtils::AddRow returned null for '%s'"), *RowName.ToString());
            return nullptr;
        }
        if (!FJsonObjectConverter::JsonObjectToUStruct(Values.ToSharedRef(), Table->RowStruct, RowMem, 0, 0))
        {
            // Roll back the just-added row so we never leave an orphan.
            FDataTableEditorUtils::RemoveRow(Table, RowName);
            OutErrorCode = TEXT("INVALID_ROW_VALUES");
            OutErrorMsg = WithRowValueDetail(
                FString::Printf(TEXT("Failed to apply values to row '%s' of struct '%s'"),
                    *RowName.ToString(), *Table->RowStruct->GetName()),
                Table->RowStruct, Values);
            return nullptr;
        }
        Table->MarkPackageDirty();
        return RowMem;
    }

    // Resolve the UEnum backing an enum-typed property (FByteProperty with an
    // Enum, or FEnumProperty). Returns null for non-enum properties.
    static const UEnum* GetPropertyEnum(const FProperty* Prop)
    {
        if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
        {
            return ByteProp->Enum;
        }
        if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
        {
            return EnumProp->GetEnum();
        }
        return nullptr;
    }

    // Collect an enum's user-facing literals (short names), skipping the
    // auto-generated `_MAX` sentinel UE appends to every UENUM and any
    // author-hidden (UMETA(Hidden)) entries.
    static TArray<FString> GatherEnumLiterals(const UEnum* Enum)
    {
        TArray<FString> Out;
        if (!Enum)
        {
            return Out;
        }
        const int32 Num = Enum->NumEnums();
        for (int32 Index = 0; Index < Num; ++Index)
        {
            // Skip UMETA(Hidden) entries — not author-selectable.
            if (Enum->HasMetaData(TEXT("Hidden"), Index))
            {
                continue;
            }
            const FString Name = Enum->GetNameStringByIndex(Index);
            // The trailing _MAX entry is synthetic and not a selectable value.
            if (Name.IsEmpty() || BlueprintEnumHelpers::IsEnumMaxEntryName(Name))
            {
                continue;
            }
            Out.Add(Name);
        }
        return Out;
    }

    // True when `InStr` resolves to a value of `Enum` (by short name or fully
    // qualified name). Mirrors the enum string resolution in PropertyImport.cpp
    // so this pre-check matches what the converter would accept.
    static bool EnumStringResolves(const UEnum* Enum, const FString& InStr)
    {
        if (!Enum)
        {
            return true;
        }
        if (Enum->GetValueByNameString(InStr) != INDEX_NONE)
        {
            return true;
        }
        const FString FullName = Enum->GenerateFullEnumName(*InStr);
        return Enum->GetValueByName(FName(*FullName)) != INDEX_NONE;
    }
}

namespace DataTableAuthoringInternal
{
    FString DescribeRowValueFailure(const UStruct* Struct, const TSharedPtr<FJsonObject>& Values)
    {
        if (!Struct || !Values.IsValid())
        {
            return FString();
        }

        // Walk authored fields (the names the converter matches on) and return a
        // rich detail for the first supplied value that cannot be converted. The
        // enum-literal case is the one that escapes ValidateValuesAgainstStruct
        // and lands as the bare converter failure, so it is the one we enrich.
        for (TFieldIterator<FProperty> It(Struct); It; ++It)
        {
            const FProperty* Prop = *It;
            const FString FieldName = Struct->GetAuthoredNameForField(Prop);
            const TSharedPtr<FJsonValue> Value = Values->TryGetField(FieldName);
            if (!Value.IsValid() || Value->Type == EJson::Null)
            {
                continue; // Missing/null: converter leaves the default — not a failure.
            }

            const UEnum* Enum = GetPropertyEnum(Prop);
            if (!Enum)
            {
                continue; // Only enum literals are pinpointed here.
            }

            // A string that does not resolve to any literal is the rejected case.
            if (Value->Type == EJson::String)
            {
                const FString InStr = Value->AsString();
                if (EnumStringResolves(Enum, InStr))
                {
                    continue;
                }

                const TArray<FString> Literals = GatherEnumLiterals(Enum);
                FString Detail = FString::Printf(
                    TEXT("Field '%s' of struct '%s': value \"%s\" is not a valid %s"),
                    *FieldName, *Struct->GetName(), *InStr, *Enum->GetName());
                if (Literals.Num() > 0)
                {
                    Detail += FString::Printf(TEXT(". Valid values: %s"),
                        *FString::Join(Literals, TEXT(", ")));
                }
                return Detail;
            }
        }

        return FString();
    }
}

REGISTER_RPC_HANDLER("data_table.list_rows", "data_table",
    "Return the DataTable's rowStruct, rowCount, and rows map (same shape as data_table.json asset dumps).",
    RPC_PARAMS(RPC_PARAM_REQ("assetPath", "path", "DataTable asset path")))
{
    FString AssetPath;
    UDataTable* Table = nullptr;
    if (!ResolveDataTable(Ctx, AssetPath, Table))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Json = DataTableDumpBuilder::BuildDataTableJson(Table);
    if (!Json.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_DATATABLE"),
            FString::Printf(TEXT("DataTable has no RowStruct: %s"), *AssetPath));
        return true;
    }

    Ctx.SendSuccess(Json);
    return true;
}

REGISTER_RPC_HANDLER("data_table.add_row", "data_table",
    "Add a new row to a DataTable. Errors if the row name already exists. Routes through FDataTableEditorUtils::AddRow and applies `values` via JSON -> struct conversion. `save` defaults to true; the result carries the standard persistence report. Returns {\"added\":<rowName>,\"created\":true,...} (the same create signal data_table.set_row's createIfMissing create branch emits).",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path",   "DataTable asset path"),
        RPC_PARAM_REQ("rowName",   "string", "Name of the new row"),
        RPC_PARAM_REQ("values",    "object", "JSON object with the row's field values"),
        RPC_PARAM_DEF("save",      "bool",   "Persist the mutation to disk", "true")))
{
    FString AssetPath;
    UDataTable* Table = nullptr;
    if (!ResolveDataTable(Ctx, AssetPath, Table))
    {
        return true;
    }

    FString RowNameStr;
    if (!Ctx.RequireString(TEXT("rowName"), RowNameStr))
    {
        return true;
    }
    TSharedPtr<FJsonObject> Values;
    if (!Ctx.RequireObject(TEXT("values"), Values))
    {
        return true;
    }
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), true);

    if (!Table->RowStruct)
    {
        Ctx.SendError(TEXT("MISSING_ROW_STRUCT"),
            FString::Printf(TEXT("DataTable has no RowStruct: %s"), *AssetPath));
        return true;
    }

    const FName RowName(*RowNameStr);
    if (Table->GetRowMap().Contains(RowName))
    {
        Ctx.SendError(TEXT("ROW_EXISTS"),
            FString::Printf(TEXT("Row '%s' already exists in DataTable %s"), *RowNameStr, *AssetPath));
        return true;
    }

    // Reject keys that match no field before mutating — the converter would
    // otherwise silently drop them and report a success-shaped no-op.
    if (RejectUnmatchedKeys(Ctx, Table->RowStruct, Values))
    {
        return true;
    }

    FString ErrCode, ErrMsg;
    if (!ApplyValuesToNewRow(Table, RowName, Values, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("added"), RowNameStr);
    // Carry the same created=true that set_row's createIfMissing-create branch
    // emits, so both add-paths surface an identical create signal.
    Result->SetBoolField(TEXT("created"), true);
    Result->SetObjectField(TEXT("row"), DataTableDumpBuilder::BuildRowJson(Table, RowName));
    AddDataTableSaveReport(Result, Table, bSaveRequested);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("data_table.set_row", "data_table",
    "Overwrite an existing row's values from JSON. When createIfMissing=true, falls back to add-row semantics if the row does not exist. `save` defaults to true; the result carries the standard persistence report. The result keys the operation performed: an in-place overwrite returns {\"updated\":<rowName>,\"created\":false,...}, while a createIfMissing create returns {\"added\":<rowName>,\"created\":true,...} (matching add_row) so callers can distinguish create from update.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath",       "path",   "DataTable asset path"),
        RPC_PARAM_REQ("rowName",         "string", "Name of the row to update"),
        RPC_PARAM_REQ("values",          "object", "JSON object with the row's field values"),
        RPC_PARAM_DEF("createIfMissing", "bool",   "If true, create the row when it does not exist", "false"),
        RPC_PARAM_DEF("save",            "bool",   "Persist the mutation to disk", "true")))
{
    FString AssetPath;
    UDataTable* Table = nullptr;
    if (!ResolveDataTable(Ctx, AssetPath, Table))
    {
        return true;
    }

    FString RowNameStr;
    if (!Ctx.RequireString(TEXT("rowName"), RowNameStr))
    {
        return true;
    }
    TSharedPtr<FJsonObject> Values;
    if (!Ctx.RequireObject(TEXT("values"), Values))
    {
        return true;
    }
    const bool bCreateIfMissing = Ctx.GetBool(TEXT("createIfMissing"), false);
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), true);

    if (!Table->RowStruct)
    {
        Ctx.SendError(TEXT("MISSING_ROW_STRUCT"),
            FString::Printf(TEXT("DataTable has no RowStruct: %s"), *AssetPath));
        return true;
    }

    // Reject keys that match no field before mutating (covers both the in-place
    // update and the createIfMissing branch) — the converter would otherwise
    // silently drop them and report a success-shaped no-op update.
    if (RejectUnmatchedKeys(Ctx, Table->RowStruct, Values))
    {
        return true;
    }

    const FName RowName(*RowNameStr);
    uint8* const* ExistingPtr = Table->GetRowMap().Find(RowName);
    const bool bRowPresent = ExistingPtr && *ExistingPtr;

    if (!bRowPresent)
    {
        if (!bCreateIfMissing)
        {
            Ctx.SendError(TEXT("ROW_NOT_FOUND"),
                FString::Printf(TEXT("Row '%s' not found in DataTable %s"), *RowNameStr, *AssetPath));
            return true;
        }

        FString ErrCode, ErrMsg;
        if (!ApplyValuesToNewRow(Table, RowName, Values, ErrCode, ErrMsg))
        {
            Ctx.SendError(ErrCode, ErrMsg);
            return true;
        }

        // The row provably did not exist (handled above) and was just created
        // via the same ApplyValuesToNewRow helper add_row uses — so report this
        // as a create: key the result "added" (matching add_row and the handler's
        // documented "add-row semantics") and surface an explicit created=true so
        // upsert callers get an unambiguous create-vs-update signal.
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("added"), RowNameStr);
        Result->SetBoolField(TEXT("created"), true);
        Result->SetObjectField(TEXT("row"), DataTableDumpBuilder::BuildRowJson(Table, RowName));
        AddDataTableSaveReport(Result, Table, bSaveRequested);
        Ctx.SendSuccess(Result);
        return true;
    }

    // Convert against an initialized copy first. The engine converter writes each
    // property as it walks the struct and can fail after earlier properties have
    // already been assigned; using the live allocation here would leave a failed
    // request with a partial update.
    uint8* Existing = *ExistingPtr;
    const UScriptStruct* RowStruct = Table->RowStruct;
    FStructOnScope Scratch(RowStruct);
    uint8* ScratchMemory = Scratch.GetStructMemory();
    if (!ScratchMemory)
    {
        Ctx.SendError(TEXT("INVALID_ROW_VALUES"),
            FString::Printf(TEXT("Failed to allocate scratch storage for row '%s' of struct '%s'"),
                *RowNameStr, *RowStruct->GetName()));
        return true;
    }
    RowStruct->CopyScriptStruct(ScratchMemory, Existing);

    if (!FJsonObjectConverter::JsonObjectToUStruct(Values.ToSharedRef(), RowStruct, ScratchMemory, 0, 0))
    {
        const FString ErrMsg = WithRowValueDetail(
            FString::Printf(TEXT("Failed to apply values to row '%s' of struct '%s'"),
                *RowNameStr, *RowStruct->GetName()),
            RowStruct, Values);
        Ctx.SendError(TEXT("INVALID_ROW_VALUES"), ErrMsg);
        return true;
    }

    Table->Modify();
    FDataTableEditorUtils::BroadcastPreChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
    RowStruct->CopyScriptStruct(Existing, ScratchMemory);
    FDataTableEditorUtils::BroadcastPostChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
    Table->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("updated"), RowNameStr);
    Result->SetBoolField(TEXT("created"), false);
    Result->SetObjectField(TEXT("row"), DataTableDumpBuilder::BuildRowJson(Table, RowName));
    AddDataTableSaveReport(Result, Table, bSaveRequested);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("data_table.remove_row", "data_table",
    "Remove a row from a DataTable. Errors if the row does not exist. `save` defaults to true; the result carries the standard persistence report.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path",   "DataTable asset path"),
        RPC_PARAM_REQ("rowName",   "string", "Name of the row to remove"),
        RPC_PARAM_DEF("save",      "bool",   "Persist the mutation to disk", "true")))
{
    FString AssetPath;
    UDataTable* Table = nullptr;
    if (!ResolveDataTable(Ctx, AssetPath, Table))
    {
        return true;
    }

    FString RowNameStr;
    if (!Ctx.RequireString(TEXT("rowName"), RowNameStr))
    {
        return true;
    }
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), true);

    const FName RowName(*RowNameStr);
    {
        uint8* const* ExistingPtr = Table->GetRowMap().Find(RowName);
        if (!ExistingPtr || !*ExistingPtr)
        {
            Ctx.SendError(TEXT("ROW_NOT_FOUND"),
                FString::Printf(TEXT("Row '%s' not found in DataTable %s"), *RowNameStr, *AssetPath));
            return true;
        }
    }

    Table->Modify();
    FDataTableEditorUtils::RemoveRow(Table, RowName);
    Table->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("removed"), RowNameStr);
    AddDataTableSaveReport(Result, Table, bSaveRequested);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("data_table.set_row_struct", "data_table",
    "Rebind a DataTable to a different row struct. Destructive (drops all existing rows). Requires force=true when rowCount>0. `save` defaults to true; the result carries the standard persistence report.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath",  "path",   "DataTable asset path"),
        RPC_PARAM_REQ("structPath", "path",   "Path of the UScriptStruct to bind as RowStruct"),
        RPC_PARAM_DEF("force",      "bool",   "Required when the table is non-empty (drops all rows)", "false"),
        RPC_PARAM_DEF("save",       "bool",   "Persist the mutation to disk", "true")))
{
    FString AssetPath;
    UDataTable* Table = nullptr;
    if (!ResolveDataTable(Ctx, AssetPath, Table))
    {
        return true;
    }

    FString StructPath;
    if (!Ctx.RequireString(TEXT("structPath"), StructPath))
    {
        return true;
    }
    const bool bForce = Ctx.GetBool(TEXT("force"), false);
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), true);

    UScriptStruct* NewStruct = nullptr;
    if (!ResolveRowStruct(Ctx, StructPath, NewStruct))
    {
        return true;
    }

    const int32 Cleared = Table->GetRowMap().Num();
    if (Cleared > 0 && !bForce)
    {
        Ctx.SendError(TEXT("ROWS_PRESENT_FORCE_REQUIRED"),
            FString::Printf(TEXT("DataTable %s has %d row(s); pass force=true to clear and rebind"),
                *AssetPath, Cleared));
        return true;
    }

    Table->Modify();
    FDataTableEditorUtils::BroadcastPreChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowList);
    // Skip CleanBeforeStructChange/RestoreAfterStructChange on the force branch:
    // that pair serializes existing rows into RowsSerializedWithTags and re-hydrates
    // them after the rebind, which undoes EmptyTable() when the new struct overlaps
    // the old. The force=true contract here is "drop all rows", so we just empty
    // and rebind directly.
    Table->EmptyTable();
    Table->RowStruct = NewStruct;
    FDataTableEditorUtils::BroadcastPostChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowList);
    Table->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("rowStruct"), StructPath);
    Result->SetNumberField(TEXT("clearedRows"), static_cast<double>(Cleared));
    AddDataTableSaveReport(Result, Table, bSaveRequested);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("data_table.create", "data_table",
    "Create a new UDataTable asset bound to a row struct, so the row-CRUD verbs have a table to author. Every other data_table.* verb resolves an EXISTING table and bails ASSET_NOT_FOUND — this is the create-side first step that bootstraps the create -> add_row -> point interaction.configure_chest_properties lootTablePath at it loop. Leaves the new package dirty (no auto-save; follow with asset.save). Idempotent: re-running against an existing UDataTable returns it with its rows intact (existing:true, mode:\"updated_in_place\") and never rebinds its RowStruct — use data_table.set_row_struct for that. Pass overwrite:true to discard and recreate it (ASSET_IN_USE when other packages reference it). Errors STRUCT_NOT_FOUND if structPath does not resolve, INVALID_ROW_STRUCT if it is not a valid DataTable row struct, and ASSET_ALREADY_EXISTS if a non-DataTable asset occupies <path>/<name>.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name",       "string", "Asset filename (no extension) for the new DataTable, e.g. 'DT_Loot'."),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("path"), TEXT("path"),
            TEXT("Content-browser folder for the new asset, e.g. /Game/Data. Alias: savePath."),
            /*bRequired=*/true, TArray<FString>({TEXT("path"), TEXT("savePath")})),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("structPath"), TEXT("path"),
            TEXT("Path of the UScriptStruct to bind as the table's RowStruct. Alias: rowStruct."),
            /*bRequired=*/true, TArray<FString>({TEXT("structPath"), TEXT("rowStruct")})),
        RPC_PARAM_DEF("overwrite",  "boolean", "Delete and recreate an existing table (discarding every row) instead of returning it. Rejected with ASSET_IN_USE when any package still references it.", "false")))
{
    const FString Name = Ctx.GetString(TEXT("name"));
    // Accept `path` (matching input.create_input_action) or the
    // ticket's `savePath` alias; `structPath` (matching data_table.set_row_struct) or `rowStruct`.
    const FString Path = Ctx.GetStringFirstOf({TEXT("path"), TEXT("savePath")});
    const FString StructPath = Ctx.GetStringFirstOf({TEXT("structPath"), TEXT("rowStruct")});

    if (Name.IsEmpty() || Path.IsEmpty() || StructPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("name, path, and structPath are required"));
        return true;
    }

    // Gate on the same validity check set_row_struct and the engine's own factory UI use,
    // so we never create a table bound to a struct the DataTable subsystem would reject.
    UScriptStruct* RowStruct = nullptr;
    if (!ResolveRowStruct(Ctx, StructPath, RowStruct))
    {
        return true;
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *Path, *Name);

    // Replaces the registry-only DoesAssetExist / ASSET_EXISTS pre-check, which missed
    // a same-session in-memory table and let it reach CanCreateAsset's modal chain.
    const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
        FullPath, Name, UDataTable::StaticClass(), Ctx.GetBool(TEXT("overwrite"), false));
    if (Resolution.IsRejected())
    {
        return AssetCreatePolicy::SendRejection(Ctx, Resolution);
    }
    if (Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace)
    {
        UDataTable* ExistingTable = CastChecked<UDataTable>(Resolution.Existing);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("assetPath"), ExistingTable->GetPathName());
        Result->SetStringField(TEXT("assetName"), ExistingTable->GetName());
        Result->SetStringField(TEXT("assetClass"), ExistingTable->GetClass()->GetName());
        // The table's ACTUAL binding, which may differ from the requested structPath:
        // rebinding here would run FDataTableEditorUtils' struct-change path and drop
        // every existing row, so the existing binding is reported rather than changed.
        const UScriptStruct* BoundStruct = ExistingTable->RowStruct;
        Result->SetStringField(TEXT("rowStruct"), BoundStruct ? BoundStruct->GetPathName() : FString());
        Result->SetBoolField(TEXT("created"), false);
        Result->SetBoolField(TEXT("existsAfter"), true);
        if (BoundStruct != RowStruct)
        {
            Result->SetStringField(TEXT("warning"),
                FString::Printf(TEXT("Existing table is bound to '%s', not the requested '%s'. ")
                                TEXT("Call data_table.set_row_struct to rebind, or pass overwrite:true to recreate."),
                    BoundStruct ? *BoundStruct->GetPathName() : TEXT("<none>"), *StructPath));
        }
        AssetCreatePolicy::AddCreateReport(Result, Resolution);
        Ctx.SendSuccess(Result);
        return true;
    }

    IAssetTools& AssetTools =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

    // UDataTableFactory::FactoryCreateNew binds DataTable->RowStruct = Struct and
    // returns null unless Struct is set, so seed it before CreateAsset (which calls
    // FactoryCreateNew directly, never the modal ConfigureProperties struct picker).
    UDataTableFactory* Factory = NewObject<UDataTableFactory>();
    Factory->Struct = RowStruct;

    UObject* NewAsset = AssetTools.CreateAsset(Name, Path, UDataTable::StaticClass(), Factory);
    UDataTable* NewTable = Cast<UDataTable>(NewAsset);
    if (!NewTable)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"),
            FString::Printf(TEXT("Failed to create DataTable at %s"), *FullPath));
        return true;
    }

    // CreateAsset already marked the new package dirty; no extra dirtying needed.
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    // Return the OBJECT path (.../DT.DT) — the resolvable form the sibling data_table.*
    // verbs (LoadObject) and interaction.configure_chest_properties' lootTablePath
    // expect, so the create -> add_row loop works directly off this response.
    Result->SetStringField(TEXT("assetPath"), NewTable->GetPathName());
    Result->SetStringField(TEXT("assetName"), NewTable->GetName());
    Result->SetStringField(TEXT("assetClass"), NewTable->GetClass()->GetName());
    Result->SetStringField(TEXT("rowStruct"), RowStruct->GetPathName());
    Result->SetBoolField(TEXT("created"), true);
    Result->SetBoolField(TEXT("existsAfter"), true);
    AssetCreatePolicy::AddCreateReport(Result, Resolution);
    Ctx.SendSuccess(Result);
    return true;
}
