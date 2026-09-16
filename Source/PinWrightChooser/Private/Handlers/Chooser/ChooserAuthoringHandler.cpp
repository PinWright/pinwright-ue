// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/PackagePathCompose.h"
#include "PinWrightHelpers.h"
#include "Utils/PathUtils.h"
#include "Utils/StringUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "BoolColumn.h"
#include "Chooser.h"
#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EnumColumn.h"
#include "FloatRangeColumn.h"
#include "Misc/PackageName.h"
#include "ObjectChooser_Asset.h"
#include "ObjectChooser_Class.h"
#include "ObjectColumn.h"
#include "RandomizeColumn.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "ChooserAuthoringHandler"

namespace PinWrightChooser
{
namespace
{
using PinWright::NormalizeToken;

FString NormalizePackagePath(FString Path)
{
    Path.TrimStartAndEndInline();
    if (Path.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase))
    {
        Path.LeftChopInline(7);
    }
    if (Path.Contains(TEXT(".")))
    {
        Path = FPackageName::ObjectPathToPackageName(Path);
    }
    return SanitizeProjectRelativePath(Path);
}

FString ToObjectPath(const FString& PackagePath)
{
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    return FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
}

bool BuildCreatePaths(FHandlerContext& Ctx, FString& OutPackagePath, FString& OutObjectPath)
{
    const FString Name = Ctx.GetString(TEXT("name"));
    if (!Name.IsEmpty())
    {
        FString Folder = Ctx.GetStringFirstOf({TEXT("folder"), TEXT("savePath"), TEXT("path")}, TEXT("/Game/Choosers"));
        Folder = NormalizePackagePath(Folder);
        while (Folder.EndsWith(TEXT("/")))
        {
            Folder.LeftChopInline(1);
        }
        // `name` is the caller's BARE leaf, and nothing above validated it as one: the folder
        // half goes through SanitizeProjectRelativePath, the name half used to be concatenated
        // raw. A path-shaped name ("Sub/Leaf", "/Game/X") composed a package the caller never
        // named and wrote the chooser there silently; the same argument one character worse
        // ("a//b") is the CreatePackage double-slash Fatal that ends the editor process
        // (UObjectGlobals.cpp:1094-1096; board B-createpackage-unvalidated-paths-plugin-wide).
        // A name of ".." reaches that call's SECOND Fatal instead (:1118, empty after
        // ResolveName2) — INVALID_OBJECTNAME_CHARACTERS covers '.' and ':' as well as '/', so
        // one check turns away both.
        // PinWrightComposeAssetPackagePath runs FName::IsValidXName over the bare name and
        // FPackageName::IsValidLongPackageName over the composed path, surfacing the engine's
        // own reason text. THIS CHECK MUST STAY ABOVE THE CONCATENATION and above HandleCreate's
        // context-class resolution — the regression test's no-crash argument depends on a build
        // without it bailing at CLASS_NOT_FOUND rather than reaching CreatePackage.
        //
        // The helper joins with Printf("%s/%s"), which — unlike FString::operator/ — DOES double
        // a separator when the left side already ends in '/'. That cannot happen here, and both
        // reasons are above: NormalizePackagePath ran SanitizeProjectRelativePath (collapses
        // "//" in a loop) and the while-loop directly above chops every trailing '/'. Keep both
        // if this call moves, or a folder spelled "/Game/X/" starts being refused for a "//" it
        // did not have.
        FString ComposeError;
        if (!PinWrightComposeAssetPackagePath(Folder, Name, OutPackagePath, ComposeError))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), ComposeError);
            return false;
        }
    }
    else
    {
        FString Path;
        if (!Ctx.RequireString(TEXT("path"), Path))
        {
            return false;
        }
        OutPackagePath = NormalizePackagePath(Path);
    }

    if (OutPackagePath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PATH"), TEXT("Invalid chooser package path"));
        return false;
    }

    FText Reason;
    if (!FPackageName::IsValidLongPackageName(OutPackagePath, false, &Reason))
    {
        Ctx.SendError(TEXT("INVALID_PATH"), Reason.ToString());
        return false;
    }

    OutObjectPath = ToObjectPath(OutPackagePath);
    return true;
}

FString GetChooserPath(FHandlerContext& Ctx)
{
    return Ctx.GetStringFirstOf({TEXT("chooserPath"), TEXT("assetPath"), TEXT("tablePath"), TEXT("path")});
}

UChooserTable* LoadChooser(FHandlerContext& Ctx)
{
    const FString RequestedPath = GetChooserPath(Ctx);
    if (RequestedPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("chooserPath, assetPath, tablePath, or path is required"));
        return nullptr;
    }

    const FString PackagePath = NormalizePackagePath(RequestedPath);
    if (PackagePath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PATH"), FString::Printf(TEXT("Invalid chooser package path: %s"), *RequestedPath));
        return nullptr;
    }
    const FString ObjectPath = PackagePath.Contains(TEXT(".")) ? PackagePath : ToObjectPath(PackagePath);

    if (UChooserTable* Existing = FindObject<UChooserTable>(nullptr, *ObjectPath))
    {
        return Existing;
    }
    if (UChooserTable* Loaded = LoadObject<UChooserTable>(nullptr, *ObjectPath))
    {
        return Loaded;
    }
    if (UChooserTable* LoadedByRequest = LoadObject<UChooserTable>(nullptr, *RequestedPath))
    {
        return LoadedByRequest;
    }

    Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Chooser table not found: %s"), *RequestedPath));
    return nullptr;
}

void ApplyBindingChain(const TArray<FString>& Chain, FChooserPropertyBinding& Binding)
{
    Binding.PropertyBindingChain.Reset();
    for (const FString& Segment : Chain)
    {
        if (!Segment.IsEmpty())
        {
            Binding.PropertyBindingChain.Add(FName(*Segment));
        }
    }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    // FChooserPropertyBinding::IsBoundToRoot was added in UE 5.4.
    Binding.IsBoundToRoot = Binding.PropertyBindingChain.IsEmpty();
#endif
}

TArray<FString> ParseBindingChain(const TSharedPtr<FJsonValue>& Value)
{
    TArray<FString> Chain;
    if (!Value.IsValid() || Value->IsNull())
    {
        return Chain;
    }

    if (Value->Type == EJson::String)
    {
        Value->AsString().ParseIntoArray(Chain, TEXT("."), true);
        return Chain;
    }

    const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
    if (Value->TryGetArray(Array) && Array)
    {
        for (const TSharedPtr<FJsonValue>& Segment : *Array)
        {
            Chain.Add(Segment->AsString());
        }
        return Chain;
    }

    const TSharedPtr<FJsonObject>* Object = nullptr;
    if (Value->TryGetObject(Object) && Object && Object->IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* ObjectChain = nullptr;
        if ((*Object)->TryGetArrayField(TEXT("chain"), ObjectChain) && ObjectChain)
        {
            for (const TSharedPtr<FJsonValue>& Segment : *ObjectChain)
            {
                Chain.Add(Segment->AsString());
            }
        }
        else
        {
            FString Path;
            if ((*Object)->TryGetStringField(TEXT("path"), Path) || (*Object)->TryGetStringField(TEXT("property"), Path))
            {
                Path.ParseIntoArray(Chain, TEXT("."), true);
            }
        }
    }

    return Chain;
}

void ApplyCommonBinding(const TSharedPtr<FJsonObject>& Payload, FChooserPropertyBinding& Binding)
{
    if (!Payload.IsValid())
    {
        return;
    }

    if (TSharedPtr<FJsonValue> BindingValue = Payload->TryGetField(TEXT("propertyBinding")))
    {
        ApplyBindingChain(ParseBindingChain(BindingValue), Binding);
    }

    double ContextIndex = 0.0;
    if (Payload->TryGetNumberField(TEXT("contextIndex"), ContextIndex))
    {
        Binding.ContextIndex = static_cast<int32>(ContextIndex);
    }

    const TSharedPtr<FJsonObject>* BindingObject = nullptr;
    if (Payload->TryGetObjectField(TEXT("propertyBinding"), BindingObject) && BindingObject && BindingObject->IsValid())
    {
        double BindingContextIndex = 0.0;
        if ((*BindingObject)->TryGetNumberField(TEXT("contextIndex"), BindingContextIndex))
        {
            Binding.ContextIndex = static_cast<int32>(BindingContextIndex);
        }
    }
}

void ApplyColumnBinding(const TSharedPtr<FJsonObject>& Payload, FBoolColumn& Column)
{
    if (FBoolContextProperty* Input = Column.InputValue.GetMutablePtr<FBoolContextProperty>())
    {
        ApplyCommonBinding(Payload, Input->Binding);
    }
}

void ApplyColumnBinding(const TSharedPtr<FJsonObject>& Payload, FFloatRangeColumn& Column)
{
    if (FFloatContextProperty* Input = Column.InputValue.GetMutablePtr<FFloatContextProperty>())
    {
        ApplyCommonBinding(Payload, Input->Binding);
    }
}

void ApplyColumnBinding(const TSharedPtr<FJsonObject>& Payload, FEnumColumn& Column)
{
    if (FEnumContextProperty* Input = Column.InputValue.GetMutablePtr<FEnumContextProperty>())
    {
        ApplyCommonBinding(Payload, Input->Binding);
        FString EnumType;
        if (Payload.IsValid())
        {
            Payload->TryGetStringField(TEXT("enumType"), EnumType);
        }
        if (!EnumType.IsEmpty())
        {
            Input->Binding.Enum = ResolveUEnum(EnumType);
        }
    }
}

void ApplyColumnBinding(const TSharedPtr<FJsonObject>& Payload, FObjectColumn& Column)
{
    if (FObjectContextProperty* Input = Column.InputValue.GetMutablePtr<FObjectContextProperty>())
    {
        ApplyCommonBinding(Payload, Input->Binding);
        FString AllowedClass;
        if (Payload.IsValid())
        {
            Payload->TryGetStringField(TEXT("allowedClass"), AllowedClass);
        }
        if (!AllowedClass.IsEmpty())
        {
            Input->Binding.AllowedClass = ResolveUClass(AllowedClass);
        }
    }
}

void ApplyColumnBinding(const TSharedPtr<FJsonObject>& Payload, FRandomizeColumn& Column)
{
    if (FRandomizeContextProperty* Input = Column.InputValue.GetMutablePtr<FRandomizeContextProperty>())
    {
        ApplyCommonBinding(Payload, Input->Binding);
    }
}

bool InitializeColumn(const FString& Kind, const TSharedPtr<FJsonObject>& Payload, FInstancedStruct& OutColumn, FString& OutError)
{
    const FString NormalizedKind = NormalizeToken(Kind);
    if (NormalizedKind == TEXT("bool"))
    {
        OutColumn.InitializeAs(FBoolColumn::StaticStruct());
        ApplyColumnBinding(Payload, OutColumn.GetMutable<FBoolColumn>());
        return true;
    }
    if (NormalizedKind == TEXT("float"))
    {
        OutColumn.InitializeAs(FFloatRangeColumn::StaticStruct());
        ApplyColumnBinding(Payload, OutColumn.GetMutable<FFloatRangeColumn>());
        return true;
    }
    if (NormalizedKind == TEXT("enum"))
    {
        FString EnumType;
        if (Payload.IsValid())
        {
            Payload->TryGetStringField(TEXT("enumType"), EnumType);
        }
        if (!EnumType.IsEmpty() && !ResolveUEnum(EnumType))
        {
            OutError = FString::Printf(TEXT("Enum type not found: %s"), *EnumType);
            return false;
        }

        OutColumn.InitializeAs(FEnumColumn::StaticStruct());
        ApplyColumnBinding(Payload, OutColumn.GetMutable<FEnumColumn>());
        return true;
    }
    if (NormalizedKind == TEXT("object"))
    {
        FString AllowedClass;
        if (Payload.IsValid())
        {
            Payload->TryGetStringField(TEXT("allowedClass"), AllowedClass);
        }
        if (!AllowedClass.IsEmpty() && !ResolveUClass(AllowedClass))
        {
            OutError = FString::Printf(TEXT("Allowed class not found: %s"), *AllowedClass);
            return false;
        }

        OutColumn.InitializeAs(FObjectColumn::StaticStruct());
        ApplyColumnBinding(Payload, OutColumn.GetMutable<FObjectColumn>());
        return true;
    }
    if (NormalizedKind == TEXT("randomize"))
    {
        OutColumn.InitializeAs(FRandomizeColumn::StaticStruct());
        ApplyColumnBinding(Payload, OutColumn.GetMutable<FRandomizeColumn>());
        return true;
    }

    OutError = FString::Printf(TEXT("Unsupported chooser column kind '%s'. Supported kinds: bool, float, enum, object, randomize."), *Kind);
    return false;
}

// Returns a non-fatal hint string if a column that was just added would fail compile because its
// input value is unbound. The "will fail" verdict and the message are sourced from the engine's own
// per-column diagnostic (the same FChooserParameterBase::HasCompileErrors call AddCompileDiagnostics
// makes), so the add_column hint and the later chooser.compile diagnostic stay consistent by
// construction — there is no second, approximate copy of the kind taxonomy or the failure wording.
// Empty string means nothing to warn about. This is the discoverability fix for
// E-chooser-column-binding-undocumented — surface the column->context->propertyBinding contract at
// add_column time instead of only as a "No Property Bound" diagnostic at compile.
FString BuildColumnBindingHint(UChooserTable* Chooser, int32 ColumnIndex)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // FChooserParameterBase::HasCompileErrors was added in UE 5.6. randomize columns have a null
    // input value, so they are skipped here and never produce a hint.
    if (!Chooser->ColumnsStructs.IsValidIndex(ColumnIndex))
    {
        return FString();
    }

    // HasCompileErrors reports the per-binding FPropertyBindingChain::CompileMessage, which is empty
    // until the binding is compiled. add_column does not run a compile (unlike chooser.compile, where
    // FinishMutation(..., bCompile=true) populates it before AddCompileDiagnostics reads it), so force
    // a compile here first — otherwise the unbound column reports no error and no hint is emitted.
    Chooser->Compile(true);

    FChooserParameterBase* Input = Chooser->ColumnsStructs[ColumnIndex].GetMutable<FChooserColumnBase>().GetInputValue();
    FText Message;
    if (!Input || !Input->HasCompileErrors(Message))
    {
        return FString();
    }

    // The engine message states the failure; append the add_column-specific remediation so the agent
    // knows which verbs/params close the gap rather than only learning it at compile time.
    return FString::Printf(
        TEXT("%s Set contextObjectType on chooser.create and pass a resolvable propertyBinding to chooser.add_column."),
        *Message.ToString());
#else
    // UE 5.4: HasCompileErrors not available — no authoritative per-column verdict to surface.
    (void)Chooser;
    (void)ColumnIndex;
    return FString();
#endif
}

void InsertColumn(UChooserTable* Chooser, const FInstancedStruct& NewColumn, int32& OutColumnIndex)
{
    const FChooserColumnBase& Column = NewColumn.Get<FChooserColumnBase>();
    if (Column.IsRandomizeColumn())
    {
        OutColumnIndex = Chooser->ColumnsStructs.Num();
        if (OutColumnIndex == 0 || !Chooser->ColumnsStructs[OutColumnIndex - 1].Get<FChooserColumnBase>().IsRandomizeColumn())
        {
            Chooser->ColumnsStructs.Add(NewColumn);
        }
        else
        {
            OutColumnIndex = Chooser->ColumnsStructs.Num() - 1;
        }
        return;
    }

    OutColumnIndex = 0;
    while (OutColumnIndex < Chooser->ColumnsStructs.Num())
    {
        const FChooserColumnBase& ExistingColumn = Chooser->ColumnsStructs[OutColumnIndex].Get<FChooserColumnBase>();
        if (ExistingColumn.HasOutputs() || ExistingColumn.IsRandomizeColumn())
        {
            break;
        }
        ++OutColumnIndex;
    }
    Chooser->ColumnsStructs.Insert(NewColumn, OutColumnIndex);
}

// Finalize a chooser mutation. When bSave, persist the .uasset to disk for real via
// SaveAssetToDiskReportingPresence — NOT the mark-dirty-only McpSafeAssetSave, which
// writes nothing. That was this file's version of the defect fixed one namespace over
// in PoseSearchHandler.cpp:156-167 (B-pose-search-create-save-no-disk-write), and it
// was worse here: the write path never wrote, the verify path (AddAssetVerification)
// asserted existsAfter unconditionally, and `saved` was the caller's own request flag
// read a second time — three fields agreeing with each other and all three disagreeing
// with the disk, so create -> add_column -> add_row -> set_cell -> compile all reported
// success for a table that no longer existed after an editor restart.
// A UChooserTable is not a Blueprint, so the bulkdata-corruption vector that pins the
// deferred mark-dirty on Blueprint edits does not apply here.
// Returns whether the .uasset actually landed on disk (false when bSave is false).
bool FinishMutation(UChooserTable* Chooser, bool bSave, bool bCompile = false)
{
    if (bCompile)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        // UChooserTable::Compile() was added in UE 5.4; on 5.3 the chooser is evaluated directly
        // from its data with no separate compile/materialize step.
        Chooser->Compile(true);
#endif
    }
    Chooser->PostEditChange();
    Chooser->MarkPackageDirty();
    if (bSave)
    {
        return SaveAssetToDiskReportingPresence(Chooser, /*bForce=*/true);
    }
    return false;
}

// bSaveRequested / bSavedToDisk are threaded through so every chooser.* verb emits the
// shared {saveRequested, saved, pendingFlush} contract. Five of the six verbs used to
// report nothing about persistence at all, leaving AddAssetVerification's existsAfter
// as the only signal — which was itself a constant.
void AddChooserFields(TSharedPtr<FJsonObject> Result, UChooserTable* Chooser,
                      bool bSaveRequested, bool bSavedToDisk)
{
    AddAssetVerification(Result, Chooser);
    AddAssetSaveReport(Result, bSaveRequested, bSavedToDisk);
    Result->SetStringField(TEXT("chooserPath"), Chooser->GetPathName());
    Result->SetNumberField(TEXT("columnCount"), Chooser->ColumnsStructs.Num());
    Result->SetNumberField(TEXT("rowCount"), Chooser->ResultsStructs.Num());
}

FInstancedStruct MakeDefaultResult(UChooserTable* Chooser)
{
    FInstancedStruct Result;
    if (Chooser->ResultType == EObjectChooserResultType::ClassResult)
    {
        Result.InitializeAs(FClassChooser::StaticStruct());
        Result.GetMutable<FClassChooser>().Class = UClass::StaticClass();
    }
    else
    {
        Result.InitializeAs(FAssetChooser::StaticStruct());
    }
    return Result;
}

bool TryGetCellValue(FHandlerContext& Ctx, TSharedPtr<FJsonValue>& OutValue)
{
    OutValue = Ctx.GetJsonValueFirstOf({TEXT("value"), TEXT("cellValue")});
    if (!OutValue.IsValid() || OutValue->IsNull())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("value is required"));
        return false;
    }
    return true;
}

bool SetBoolCell(FHandlerContext& Ctx, FBoolColumn& Column, int32 Row, int32 NumRows, const TSharedPtr<FJsonValue>& Value)
{
    EBoolColumnCellValue CellValue = EBoolColumnCellValue::MatchAny;
    if (Value->Type == EJson::Boolean)
    {
        CellValue = Value->AsBool() ? EBoolColumnCellValue::MatchTrue : EBoolColumnCellValue::MatchFalse;
    }
    else
    {
        const FString Text = NormalizeToken(Value->AsString());
        if (Text == TEXT("true") || Text == TEXT("match_true"))
        {
            CellValue = EBoolColumnCellValue::MatchTrue;
        }
        else if (Text == TEXT("false") || Text == TEXT("match_false"))
        {
            CellValue = EBoolColumnCellValue::MatchFalse;
        }
        else if (Text == TEXT("any") || Text == TEXT("match_any"))
        {
            CellValue = EBoolColumnCellValue::MatchAny;
        }
        else
        {
            Ctx.SendError(TEXT("INVALID_VALUE"), FString::Printf(TEXT("Unsupported bool cell value: %s"), *Value->AsString()));
            return false;
        }
    }

    Column.SetNumRows(NumRows);
    Column.RowValuesWithAny[Row] = CellValue;
    return true;
}

bool SetFloatCell(FFloatRangeColumn& Column, int32 Row, int32 NumRows, const TSharedPtr<FJsonValue>& Value)
{
    FChooserFloatRangeRowData RowData;
    const TSharedPtr<FJsonObject>* Object = nullptr;
    if (Value->TryGetObject(Object) && Object && Object->IsValid())
    {
        double Number = 0.0;
        if ((*Object)->TryGetNumberField(TEXT("min"), Number))
        {
            RowData.Min = static_cast<float>(Number);
        }
        else
        {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
            // FChooserFloatRangeRowData::bNoMin/bNoMax were added in UE 5.6
            RowData.bNoMin = true;
#else
            // UE 5.4: no bNoMin field; use the most negative finite float as sentinel
            RowData.Min = -3.402823466e+38f;
#endif
        }
        if ((*Object)->TryGetNumberField(TEXT("max"), Number))
        {
            RowData.Max = static_cast<float>(Number);
        }
        else
        {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
            RowData.bNoMax = true;
#else
            // UE 5.4: no bNoMax field; use the most positive finite float as sentinel
            RowData.Max = 3.402823466e+38f;
#endif
        }
    }
    else
    {
        const float Number = static_cast<float>(Value->AsNumber());
        RowData.Min = Number;
        RowData.Max = Number;
    }

    Column.SetNumRows(NumRows);
    Column.RowValues[Row] = RowData;
    return true;
}

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
// EEnumColumnCellValueComparison (the 3-state enum-column comparison) was added in UE 5.4. On 5.3
// FChooserEnumRowData carries a plain CompareNotEqual bool instead (handled in SetEnumCell below).
bool ParseComparison(const FString& Text, EEnumColumnCellValueComparison& Out)
{
    const FString Normalized = NormalizeToken(Text);
    if (Normalized.IsEmpty() || Normalized == TEXT("equal") || Normalized == TEXT("match_equal"))
    {
        Out = EEnumColumnCellValueComparison::MatchEqual;
        return true;
    }
    if (Normalized == TEXT("not_equal") || Normalized == TEXT("match_not_equal"))
    {
        Out = EEnumColumnCellValueComparison::MatchNotEqual;
        return true;
    }
    if (Normalized == TEXT("any") || Normalized == TEXT("match_any"))
    {
        Out = EEnumColumnCellValueComparison::MatchAny;
        return true;
    }
    return false;
}
#else
// UE 5.3: enum rows compare equal/not-equal only via the CompareNotEqual bool; "any" is unsupported.
bool ParseEnumCompareNotEqual(const FString& Text, bool& bOutCompareNotEqual)
{
    const FString Normalized = NormalizeToken(Text);
    if (Normalized.IsEmpty() || Normalized == TEXT("equal") || Normalized == TEXT("match_equal"))
    {
        bOutCompareNotEqual = false;
        return true;
    }
    if (Normalized == TEXT("not_equal") || Normalized == TEXT("match_not_equal"))
    {
        bOutCompareNotEqual = true;
        return true;
    }
    return false;
}
#endif

bool ParseComparison(const FString& Text, EObjectColumnCellValueComparison& Out)
{
    const FString Normalized = NormalizeToken(Text);
    if (Normalized.IsEmpty() || Normalized == TEXT("equal") || Normalized == TEXT("match_equal"))
    {
        Out = EObjectColumnCellValueComparison::MatchEqual;
        return true;
    }
    if (Normalized == TEXT("not_equal") || Normalized == TEXT("match_not_equal"))
    {
        Out = EObjectColumnCellValueComparison::MatchNotEqual;
        return true;
    }
    if (Normalized == TEXT("any") || Normalized == TEXT("match_any"))
    {
        Out = EObjectColumnCellValueComparison::MatchAny;
        return true;
    }
    return false;
}

bool SetEnumCell(FHandlerContext& Ctx, FEnumColumn& Column, int32 Row, int32 NumRows, const TSharedPtr<FJsonValue>& Value)
{
    FChooserEnumRowData RowData;
    const TSharedPtr<FJsonObject>* Object = nullptr;
    TSharedPtr<FJsonValue> ActualValue = Value;
    FString Comparison;
    if (Value->TryGetObject(Object) && Object && Object->IsValid())
    {
        ActualValue = (*Object)->TryGetField(TEXT("value"));
        (*Object)->TryGetStringField(TEXT("comparison"), Comparison);
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    if (!ParseComparison(Comparison, RowData.Comparison))
    {
        Ctx.SendError(TEXT("INVALID_VALUE"), FString::Printf(TEXT("Unsupported enum comparison: %s"), *Comparison));
        return false;
    }
#else
    // UE 5.3: only equal / not_equal are representable (CompareNotEqual bool); "any" is unsupported.
    if (!ParseEnumCompareNotEqual(Comparison, RowData.CompareNotEqual))
    {
        Ctx.SendError(TEXT("INVALID_VALUE"), FString::Printf(TEXT("Unsupported enum comparison on UE 5.3: %s"), *Comparison));
        return false;
    }
#endif
    if (!ActualValue.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("enum cell value is required"));
        return false;
    }

    if (ActualValue->Type == EJson::Number)
    {
        RowData.Value = static_cast<uint8>(ActualValue->AsNumber());
    }
    else
    {
        const FString EnumValueName = ActualValue->AsString();
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // FEnumColumn::GetEnum() is a direct helper added in UE 5.6
        const UEnum* Enum = Column.GetEnum();
#else
        // UE 5.4: access the enum through the InputValue FChooserParameterEnumBase interface
        const UEnum* Enum = nullptr;
        if (const FChooserParameterEnumBase* EnumParam = Column.InputValue.GetPtr<FChooserParameterEnumBase>())
        {
            Enum = EnumParam->GetEnum();
        }
#endif
        if (!Enum)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("String enum cells require add_column enumType."));
            return false;
        }
        const int64 EnumValue = Enum->GetValueByNameString(EnumValueName);
        if (EnumValue == INDEX_NONE)
        {
            Ctx.SendError(TEXT("INVALID_VALUE"), FString::Printf(TEXT("Enum value not found: %s"), *EnumValueName));
            return false;
        }
        RowData.Value = static_cast<uint8>(EnumValue);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // FChooserEnumRowData::ValueName was added in UE 5.6
        RowData.ValueName = FName(*EnumValueName);
#endif
    }

    Column.SetNumRows(NumRows);
    Column.RowValues[Row] = RowData;
    return true;
}

bool SetObjectCell(FHandlerContext& Ctx, FObjectColumn& Column, int32 Row, int32 NumRows, const TSharedPtr<FJsonValue>& Value)
{
    FChooserObjectRowData RowData;
    const TSharedPtr<FJsonObject>* Object = nullptr;
    FString ObjectPath;
    FString Comparison;
    if (Value->TryGetObject(Object) && Object && Object->IsValid())
    {
        (*Object)->TryGetStringField(TEXT("value"), ObjectPath);
        if (ObjectPath.IsEmpty())
        {
            (*Object)->TryGetStringField(TEXT("assetPath"), ObjectPath);
        }
        (*Object)->TryGetStringField(TEXT("comparison"), Comparison);
    }
    else
    {
        ObjectPath = Value->AsString();
    }

    if (!ParseComparison(Comparison, RowData.Comparison))
    {
        Ctx.SendError(TEXT("INVALID_VALUE"), FString::Printf(TEXT("Unsupported object comparison: %s"), *Comparison));
        return false;
    }

    RowData.Value = TSoftObjectPtr<UObject>(FSoftObjectPath(ObjectPath));
    Column.SetNumRows(NumRows);
    Column.RowValues[Row] = RowData;
    return true;
}

bool SetRandomizeCell(FRandomizeColumn& Column, int32 Row, int32 NumRows, const TSharedPtr<FJsonValue>& Value)
{
    Column.SetNumRows(NumRows);
    Column.RowValues[Row] = static_cast<float>(Value->AsNumber());
    return true;
}

bool SetCellValue(FHandlerContext& Ctx, UChooserTable* Chooser, int32 Row, int32 ColumnIndex, const TSharedPtr<FJsonValue>& Value)
{
    if (!Chooser->ResultsStructs.IsValidIndex(Row))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Invalid row index: %d"), Row));
        return false;
    }
    if (!Chooser->ColumnsStructs.IsValidIndex(ColumnIndex))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Invalid column index: %d"), ColumnIndex));
        return false;
    }

    FInstancedStruct& ColumnData = Chooser->ColumnsStructs[ColumnIndex];
    const int32 NumRows = Chooser->ResultsStructs.Num();

    if (FBoolColumn* Column = ColumnData.GetMutablePtr<FBoolColumn>())
    {
        return SetBoolCell(Ctx, *Column, Row, NumRows, Value);
    }
    if (FFloatRangeColumn* Column = ColumnData.GetMutablePtr<FFloatRangeColumn>())
    {
        return SetFloatCell(*Column, Row, NumRows, Value);
    }
    if (FEnumColumn* Column = ColumnData.GetMutablePtr<FEnumColumn>())
    {
        return SetEnumCell(Ctx, *Column, Row, NumRows, Value);
    }
    if (FObjectColumn* Column = ColumnData.GetMutablePtr<FObjectColumn>())
    {
        return SetObjectCell(Ctx, *Column, Row, NumRows, Value);
    }
    if (FRandomizeColumn* Column = ColumnData.GetMutablePtr<FRandomizeColumn>())
    {
        return SetRandomizeCell(*Column, Row, NumRows, Value);
    }

    Ctx.SendError(TEXT("UNSUPPORTED_COLUMN"), TEXT("Column type is not supported by chooser.set_cell."));
    return false;
}

UScriptStruct* ResolveResultStruct(const FString& ResultKind)
{
    const FString Normalized = NormalizeToken(ResultKind);
    if (Normalized == TEXT("asset"))
    {
        return FAssetChooser::StaticStruct();
    }
    if (Normalized == TEXT("class"))
    {
        return FClassChooser::StaticStruct();
    }
    if (Normalized == TEXT("evaluate_chooser"))
    {
        return FEvaluateChooser::StaticStruct();
    }
    return nullptr;
}

bool SetResultValue(FHandlerContext& Ctx, UChooserTable* Chooser, int32 Row, const FString& ResultKind, const FString& Value)
{
    if (!Chooser->ResultsStructs.IsValidIndex(Row))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Invalid row index: %d"), Row));
        return false;
    }

    UScriptStruct* ResultStruct = ResolveResultStruct(ResultKind);
    if (!ResultStruct)
    {
        Ctx.SendError(TEXT("UNSUPPORTED_RESULT"), FString::Printf(TEXT("Unsupported chooser result kind '%s'. Supported kinds: asset, class, evaluate_chooser."), *ResultKind));
        return false;
    }

    const FString NormalizedKind = NormalizeToken(ResultKind);
    if (NormalizedKind == TEXT("asset"))
    {
        UObject* Asset = LoadObject<UObject>(nullptr, *Value);
        if (!Asset)
        {
            Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Asset result not found: %s"), *Value));
            return false;
        }
        FInstancedStruct& Result = Chooser->ResultsStructs[Row];
        Result.InitializeAs(ResultStruct);
        Result.GetMutable<FAssetChooser>().Asset = Asset;
        Chooser->ResultType = EObjectChooserResultType::ObjectResult;
        Chooser->OutputObjectType = Asset->GetClass();
        return true;
    }
    if (NormalizedKind == TEXT("class"))
    {
        UClass* Class = ResolveUClass(Value);
        if (!Class)
        {
            Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Class result not found: %s"), *Value));
            return false;
        }
        FInstancedStruct& Result = Chooser->ResultsStructs[Row];
        Result.InitializeAs(ResultStruct);
        Result.GetMutable<FClassChooser>().Class = Class;
        Chooser->ResultType = EObjectChooserResultType::ClassResult;
        Chooser->OutputObjectType = Class;
        return true;
    }

    UChooserTable* NestedChooser = LoadObject<UChooserTable>(nullptr, *Value);
    if (!NestedChooser)
    {
        NestedChooser = FindObject<UChooserTable>(nullptr, *Value);
    }
    if (!NestedChooser)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Chooser result not found: %s"), *Value));
        return false;
    }
    FInstancedStruct& Result = Chooser->ResultsStructs[Row];
    Result.InitializeAs(ResultStruct);
    Result.GetMutable<FEvaluateChooser>().Chooser = NestedChooser;
    Chooser->ResultType = NestedChooser->ResultType;
    Chooser->OutputObjectType = NestedChooser->OutputObjectType;
    return true;
}

void AddCompileDiagnostics(TSharedPtr<FJsonObject> Result, UChooserTable* Chooser)
{
    TArray<TSharedPtr<FJsonValue>> Diagnostics;
    for (int32 ColumnIndex = 0; ColumnIndex < Chooser->ColumnsStructs.Num(); ++ColumnIndex)
    {
        FChooserColumnBase& Column = Chooser->ColumnsStructs[ColumnIndex].GetMutable<FChooserColumnBase>();
        FChooserParameterBase* Input = Column.GetInputValue();
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // FChooserParameterBase::HasCompileErrors was added in UE 5.6
        FText Message;
        if (Input && Input->HasCompileErrors(Message))
        {
            TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
            Diagnostic->SetStringField(TEXT("kind"), TEXT("column"));
            Diagnostic->SetNumberField(TEXT("columnIndex"), ColumnIndex);
            Diagnostic->SetStringField(TEXT("message"), Message.ToString());
            Diagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
        }
#else
        // UE 5.4: HasCompileErrors not available — skip per-column diagnostics
        (void)Input;
#endif
    }

    for (int32 Row = 0; Row < Chooser->ResultsStructs.Num(); ++Row)
    {
        FObjectChooserBase* ResultChooser = Chooser->ResultsStructs[Row].GetMutablePtr<FObjectChooserBase>();
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // FObjectChooserBase::HasCompileErrors was added in UE 5.6
        FText Message;
        if (ResultChooser && ResultChooser->HasCompileErrors(Message))
        {
            TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
            Diagnostic->SetStringField(TEXT("kind"), TEXT("result"));
            Diagnostic->SetNumberField(TEXT("row"), Row);
            Diagnostic->SetStringField(TEXT("message"), Message.ToString());
            Diagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
        }
#else
        // UE 5.4: HasCompileErrors not available — skip per-result diagnostics
        (void)ResultChooser;
#endif
    }

    Result->SetArrayField(TEXT("diagnostics"), Diagnostics);
    Result->SetBoolField(TEXT("hasDiagnostics"), Diagnostics.Num() > 0);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    Result->SetBoolField(TEXT("diagnosticsSupported"), true);
#else
    // FChooserParameterBase / FObjectChooserBase::HasCompileErrors arrived in UE 5.6, so
    // on 5.3-5.5 both loops above are compiled out entirely. diagnostics is then empty
    // because nothing was probed, not because the table is clean — publishing
    // hasDiagnostics:false without this flag is a clean bill of health nobody issued.
    Result->SetBoolField(TEXT("diagnosticsSupported"), false);
#endif
}
}

bool HandleCreate(FHandlerContext& Ctx)
{
    FString PackagePath;
    FString ObjectPath;
    if (!BuildCreatePaths(Ctx, PackagePath, ObjectPath))
    {
        return true;
    }

    if (FindObject<UChooserTable>(nullptr, *ObjectPath) || LoadObject<UChooserTable>(nullptr, *ObjectPath))
    {
        Ctx.SendError(TEXT("ALREADY_EXISTS"), FString::Printf(TEXT("Chooser table already exists: %s"), *ObjectPath));
        return true;
    }

    const FString ContextObjectType = Ctx.GetString(TEXT("contextObjectType"));
    UClass* ContextClass = nullptr;
    if (!ContextObjectType.IsEmpty())
    {
        ContextClass = ResolveUClass(ContextObjectType);
        if (!ContextClass)
        {
            Ctx.SendError(TEXT("CLASS_NOT_FOUND"), FString::Printf(TEXT("Context class not found: %s"), *ContextObjectType));
            return true;
        }
    }

    const FString OutputObjectType = Ctx.GetString(TEXT("outputObjectType"));
    UClass* OutputClass = OutputObjectType.IsEmpty() ? UObject::StaticClass() : ResolveUClass(OutputObjectType);
    if (!OutputObjectType.IsEmpty() && !OutputClass)
    {
        Ctx.SendError(TEXT("CLASS_NOT_FOUND"), FString::Printf(TEXT("Output class not found: %s"), *OutputObjectType));
        return true;
    }

    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), FString::Printf(TEXT("Failed to create package: %s"), *PackagePath));
        return true;
    }

    const FScopedTransaction Transaction(LOCTEXT("CreateChooser", "Create Chooser Table"));
    UChooserTable* Chooser = NewObject<UChooserTable>(
        Package,
        UChooserTable::StaticClass(),
        FName(*AssetName),
        RF_Public | RF_Standalone | RF_Transactional | RF_LoadCompleted);
    if (!Chooser)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create Chooser table"));
        return true;
    }

    if (ContextClass)
    {
        Chooser->ContextData.SetNum(1);
        Chooser->ContextData[0].InitializeAs(FContextObjectTypeClass::StaticStruct());
        FContextObjectTypeClass& ContextData = Chooser->ContextData[0].GetMutable<FContextObjectTypeClass>();
        ContextData.Class = ContextClass;
        ContextData.Direction = EContextObjectDirection::ReadWrite;
    }

    const FString ResultType = NormalizeToken(Ctx.GetString(TEXT("resultType"), TEXT("object")));
    if (ResultType == TEXT("class"))
    {
        Chooser->ResultType = EObjectChooserResultType::ClassResult;
    }
    else if (ResultType == TEXT("none") || ResultType == TEXT("no_primary_result"))
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // EObjectChooserResultType::NoPrimaryResult was added in UE 5.6 when the Chooser
        // plugin moved out of Experimental
        Chooser->ResultType = EObjectChooserResultType::NoPrimaryResult;
#else
        // UE 5.4: NoPrimaryResult does not exist. Falling back to ObjectResult would silently
        // produce a fundamentally different chooser (one that returns a primary object) than the
        // caller requested, so reject rather than fake the primary result.
        Ctx.SendUnsupportedEngineVersion(TEXT("5.6"), TEXT("Creating a no-primary-result chooser"));
        return true;
#endif
    }
    else
    {
        Chooser->ResultType = EObjectChooserResultType::ObjectResult;
    }

    Chooser->OutputObjectType = OutputClass;

    FAssetRegistryModule::AssetCreated(Chooser);
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), false);
    const bool bSavedToDisk = FinishMutation(Chooser, bSaveRequested);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    // The `saved` field here used to be `Ctx.GetBool("save")` — the caller's own request
    // flag read a second time, downstream of no save at all. AddChooserFields now emits
    // it from FinishMutation's on-disk verdict.
    AddChooserFields(Result, Chooser, bSaveRequested, bSavedToDisk);
    Ctx.SendSuccess(Result);
    return true;
}

bool HandleAddColumn(FHandlerContext& Ctx)
{
    FString Kind;
    if (!Ctx.RequireString(TEXT("kind"), Kind))
    {
        return true;
    }
    UChooserTable* Chooser = LoadChooser(Ctx);
    if (!Chooser)
    {
        return true;
    }

    FInstancedStruct NewColumn;
    FString Error;
    if (!InitializeColumn(Kind, Ctx.GetRawPayload(), NewColumn, Error))
    {
        Ctx.SendError(TEXT("UNSUPPORTED_COLUMN"), Error);
        return true;
    }

    const FScopedTransaction Transaction(LOCTEXT("AddChooserColumn", "Add Chooser Column"));
    Chooser->Modify(true);
    int32 ColumnIndex = INDEX_NONE;
    InsertColumn(Chooser, NewColumn, ColumnIndex);
    if (Chooser->ColumnsStructs.IsValidIndex(ColumnIndex))
    {
        Chooser->ColumnsStructs[ColumnIndex].GetMutable<FChooserColumnBase>().SetNumRows(Chooser->ResultsStructs.Num());
    }
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), false);
    const bool bSavedToDisk = FinishMutation(Chooser, bSaveRequested);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddChooserFields(Result, Chooser, bSaveRequested, bSavedToDisk);
    Result->SetNumberField(TEXT("columnIndex"), ColumnIndex);
    const FString NormalizedKind = NormalizeToken(Kind);
    Result->SetStringField(TEXT("kind"), NormalizedKind);

    const FString BindingHint = BuildColumnBindingHint(Chooser, ColumnIndex);
    if (!BindingHint.IsEmpty())
    {
        TArray<TSharedPtr<FJsonValue>> Warnings;
        Warnings.Add(MakeShared<FJsonValueString>(BindingHint));
        Result->SetArrayField(TEXT("warnings"), Warnings);
    }

    Ctx.SendSuccess(Result);
    return true;
}

bool HandleAddRow(FHandlerContext& Ctx)
{
    UChooserTable* Chooser = LoadChooser(Ctx);
    if (!Chooser)
    {
        return true;
    }

    const FScopedTransaction Transaction(LOCTEXT("AddChooserRow", "Add Chooser Row"));
    Chooser->Modify(true);
    const int32 Row = Chooser->ResultsStructs.Add(MakeDefaultResult(Chooser));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // UChooserTable::DisabledRows was added in UE 5.6
    Chooser->DisabledRows.Add(false);
#endif
    for (FInstancedStruct& ColumnData : Chooser->ColumnsStructs)
    {
        ColumnData.GetMutable<FChooserColumnBase>().SetNumRows(Chooser->ResultsStructs.Num());
    }
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), false);
    const bool bSavedToDisk = FinishMutation(Chooser, bSaveRequested);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddChooserFields(Result, Chooser, bSaveRequested, bSavedToDisk);
    Result->SetNumberField(TEXT("row"), Row);
    Ctx.SendSuccess(Result);
    return true;
}

bool HandleSetCell(FHandlerContext& Ctx)
{
    int32 Row = INDEX_NONE;
    if (!Ctx.RequireInt(TEXT("row"), Row))
    {
        return true;
    }
    int32 Column = Ctx.GetInt(TEXT("column"), INDEX_NONE);
    if (Column == INDEX_NONE)
    {
        Column = Ctx.GetInt(TEXT("columnIndex"), INDEX_NONE);
    }
    if (Column == INDEX_NONE)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("column or columnIndex is required"));
        return true;
    }

    TSharedPtr<FJsonValue> Value;
    if (!TryGetCellValue(Ctx, Value))
    {
        return true;
    }
    UChooserTable* Chooser = LoadChooser(Ctx);
    if (!Chooser)
    {
        return true;
    }

    const FScopedTransaction Transaction(LOCTEXT("SetChooserCell", "Set Chooser Cell"));
    Chooser->Modify(true);
    if (!SetCellValue(Ctx, Chooser, Row, Column, Value))
    {
        return true;
    }
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), false);
    const bool bSavedToDisk = FinishMutation(Chooser, bSaveRequested);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddChooserFields(Result, Chooser, bSaveRequested, bSavedToDisk);
    Result->SetNumberField(TEXT("row"), Row);
    Result->SetNumberField(TEXT("columnIndex"), Column);
    Ctx.SendSuccess(Result);
    return true;
}

bool HandleSetResult(FHandlerContext& Ctx)
{
    int32 Row = INDEX_NONE;
    if (!Ctx.RequireInt(TEXT("row"), Row))
    {
        return true;
    }
    FString ResultKind;
    if (!Ctx.RequireString(TEXT("resultKind"), ResultKind))
    {
        return true;
    }
    FString Value;
    if (!Ctx.RequireString(TEXT("value"), Value))
    {
        return true;
    }

    UChooserTable* Chooser = LoadChooser(Ctx);
    if (!Chooser)
    {
        return true;
    }

    const FScopedTransaction Transaction(LOCTEXT("SetChooserResult", "Set Chooser Result"));
    Chooser->Modify(true);
    if (!SetResultValue(Ctx, Chooser, Row, ResultKind, Value))
    {
        return true;
    }
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), false);
    const bool bSavedToDisk = FinishMutation(Chooser, bSaveRequested);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddChooserFields(Result, Chooser, bSaveRequested, bSavedToDisk);
    Result->SetNumberField(TEXT("row"), Row);
    Result->SetStringField(TEXT("resultKind"), NormalizeToken(ResultKind));
    Ctx.SendSuccess(Result);
    return true;
}

bool HandleCompile(FHandlerContext& Ctx)
{
    UChooserTable* Chooser = LoadChooser(Ctx);
    if (!Chooser)
    {
        return true;
    }

    const FScopedTransaction Transaction(LOCTEXT("CompileChooser", "Compile Chooser"));
    Chooser->Modify(true);
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), false);
    const bool bSavedToDisk = FinishMutation(Chooser, bSaveRequested, /*bCompile=*/true);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddChooserFields(Result, Chooser, bSaveRequested, bSavedToDisk);
    AddCompileDiagnostics(Result, Chooser);
    // `compiled` was the literal true on every engine, including UE 5.3 where
    // FinishMutation's Compile block is #if'd out entirely and no compile step exists.
    // It now states exactly one fact — the compile step ran — and the separate
    // compileClean carries the error verdict, so neither field has to mean two things.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    Result->SetBoolField(TEXT("compiled"), true);
    // compileClean is the honest "no errors were found" claim, and it is false whenever
    // that could not be established: on UE 5.3-5.5 the per-column / per-result
    // HasCompileErrors probes in AddCompileDiagnostics are compiled out, so an empty
    // diagnostics array proves nothing. Gate on this, not on `compiled`.
    Result->SetBoolField(TEXT("compileClean"),
        Result->GetBoolField(TEXT("diagnosticsSupported"))
            && !Result->GetBoolField(TEXT("hasDiagnostics")));
#else
    // UE 5.3 has no UChooserTable::Compile — the table is evaluated straight from its
    // data. compiled:true here asserted a step that never ran.
    Result->SetBoolField(TEXT("compiled"), false);
    Result->SetBoolField(TEXT("compileClean"), false);
    Result->SetStringField(TEXT("compileSkippedReason"),
        TEXT("UChooserTable::Compile requires UE 5.4+; this engine evaluates the chooser directly from its data, so there is no compile step to run"));
#endif
    Ctx.SendSuccess(Result);
    return true;
}
}

REGISTER_RPC_HANDLER("chooser.create", "chooser",
    "Create a UChooserTable asset.",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Full package path for the chooser, or folder path when name is provided."),
        RPC_PARAM_OPT("name", "string", "Optional asset name when path is a folder."),
        RPC_PARAM_OPT("contextObjectType", "classref", "Context UObject class path. Required for any chooser with property-driven (bool/float/enum/object) columns; without it those columns fail compile with 'No Property Bound'."),
        RPC_PARAM_DEF("resultType", "string", "object, class, or none.", "object"),
        RPC_PARAM_OPT("outputObjectType", "classref", "Optional output UObject class path."),
        RPC_PARAM_DEF("save", "boolean", "Write the new .uasset to disk. Without it the chooser lives only in memory and is lost on editor restart; the response reports saved/pendingFlush accordingly.", "false")
    ))
{
    return PinWrightChooser::HandleCreate(Ctx);
}

REGISTER_RPC_HANDLER("chooser.add_column", "chooser",
    "Append a first-slice chooser column. Supported kinds: bool, float, enum, object, randomize.",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Chooser table asset path."),
        RPC_PARAM_REQ("kind", "string", "bool, float, enum, object, or randomize."),
        RPC_PARAM_OPT("propertyBinding", "string|array|object", "Property binding chain (a property path on the context class). Required for bool/float/enum/object columns to compile clean; omitting it returns a non-fatal warning and yields a 'No Property Bound' diagnostic at compile."),
        RPC_PARAM_OPT("enumType", "string", "Enum path for enum columns."),
        RPC_PARAM_OPT("allowedClass", "classref", "Allowed UObject class for object columns."),
        RPC_PARAM_DEF("save", "boolean", "Write the .uasset to disk. Without it the change lives only in memory and is lost on editor restart; the response reports saved/pendingFlush accordingly.", "false")
    ))
{
    return PinWrightChooser::HandleAddColumn(Ctx);
}

REGISTER_RPC_HANDLER("chooser.add_row", "chooser",
    "Append a chooser row and initialize column cell storage.",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Chooser table asset path."),
        RPC_PARAM_DEF("save", "boolean", "Write the .uasset to disk. Without it the change lives only in memory and is lost on editor restart; the response reports saved/pendingFlush accordingly.", "false")
    ))
{
    return PinWrightChooser::HandleAddRow(Ctx);
}

REGISTER_RPC_HANDLER("chooser.set_cell", "chooser",
    "Set a chooser cell value using the stored column kind.",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Chooser table asset path."),
        RPC_PARAM_REQ("row", "number", "Row index."),
        RPC_PARAM_REQ("column", "number", "Column index."),
        RPC_PARAM_REQ("value", "any", "Typed cell value; shape depends on the column kind: float={min,max} or a number, bool=true/false/any, object/enum={value,comparison}. See the chooser.set_cell wiki page."),
        RPC_PARAM_DEF("save", "boolean", "Write the .uasset to disk. Without it the change lives only in memory and is lost on editor restart; the response reports saved/pendingFlush accordingly.", "false")
    ))
{
    return PinWrightChooser::HandleSetCell(Ctx);
}

REGISTER_RPC_HANDLER("chooser.set_result", "chooser",
    "Set a chooser row result. Supported result kinds: asset, class, evaluate_chooser.",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Chooser table asset path."),
        RPC_PARAM_REQ("row", "number", "Row index."),
        RPC_PARAM_REQ("resultKind", "string", "asset, class, or evaluate_chooser."),
        RPC_PARAM_REQ("value", "path", "Asset, class, or chooser path."),
        RPC_PARAM_DEF("save", "boolean", "Write the .uasset to disk. Without it the change lives only in memory and is lost on editor restart; the response reports saved/pendingFlush accordingly.", "false")
    ))
{
    return PinWrightChooser::HandleSetResult(Ctx);
}

REGISTER_RPC_HANDLER("chooser.compile", "chooser",
    "Compile a chooser table and return diagnostics.",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Chooser table asset path."),
        RPC_PARAM_DEF("save", "boolean", "Write the .uasset to disk. Without it the change lives only in memory and is lost on editor restart; the response reports saved/pendingFlush accordingly.", "false")
    ))
{
    return PinWrightChooser::HandleCompile(Ctx);
}

#undef LOCTEXT_NAMESPACE
