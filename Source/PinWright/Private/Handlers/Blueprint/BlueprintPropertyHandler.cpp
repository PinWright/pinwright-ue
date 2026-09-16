// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintPropertyHandler.cpp - Migrated from PinWright_BlueprintHandlers.cpp
// Blueprint variable management: add, remove, rename, set_default, set_variable_metadata

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintReinstancingGuard.h"
#include "Compiler/BpirTypeSpec.h"
#include "Compiler/BpirTypeSpecParser.h"
#include "Compiler/CodePinResolver.h"
#include "State/PluginState.h"
#include "Utils/PropertyUtils.h"
#include "Compat/JsonKeyCompat.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "EditorAssetLibrary.h"
#include "JsonObjectConverter.h"
#include "ScopedTransaction.h"

using namespace BlueprintHandlerUtils;

static FName GetVariablePrivateMetadataKey()
{
    return FBlueprintMetadata::MD_Private;
}

static FName GetExposeOnSpawnMetadataKey()
{
    return FBlueprintMetadata::MD_ExposeOnSpawn;
}

// ---- blueprint.add_variable ----
REGISTER_RPC_HANDLER("blueprint.add_variable", "blueprint", "Add a member variable to a Blueprint class. The Blueprint must be recompiled (blueprint.compile) for the variable to be usable in graphs. For per-variable replication/exposure flags use blueprint.set_variable_settings.",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path.")),
        RPC_PARAM_REQ("variableName", "string", "Identifier for the new variable."),
        RPC_PARAM_OPT("variableType", "string", "Type token: built-in primitive (float, int, bool, string, name, text, vector, rotator, transform), or 'class:/Script/X.Y' for object/class refs, 'struct:/Game/...' for struct refs. Container wrappers: array<T>, set<T>, map<K,V> (e.g. 'set<name>', 'map<string,int>')."),
        RPC_PARAM_OPT("defaultValue", "string", "Default value as JSON-compatible string; type-coerced to the variable's type."),
        RPC_PARAM_OPT("category", "string", "Display category in the Blueprint editor's Variables panel."),
        RPC_PARAM_DEF("isReplicated", "boolean", "Marks variable as replicated; defaults to false. For full replication settings (RepCondition, RepNotify) use blueprint.set_variable_settings.", "false"),
        RPC_PARAM_DEF("isPublic", "boolean", "Whether the variable is public (exposed) on the Blueprint's instance details panel; defaults to false.", "false")
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.add_variable requires a blueprint path."));
        return true;
    }

    const TSharedPtr<FJsonObject>& LocalPayload = Ctx.GetRawPayload();
    FString VarName;
    LocalPayload->TryGetStringField(TEXT("variableName"), VarName);
    if (VarName.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("variableName required"));
        return true;
    }

    FString VarType;
    LocalPayload->TryGetStringField(TEXT("variableType"), VarType);
    FString Category;
    LocalPayload->TryGetStringField(TEXT("category"), Category);
    FString DefaultValue;
    const bool bHasDefaultValue = LocalPayload->TryGetStringField(TEXT("defaultValue"), DefaultValue);
    const bool bReplicated = LocalPayload->HasField(TEXT("isReplicated"))
        ? GetJsonBoolField(LocalPayload, TEXT("isReplicated")) : false;
    const bool bPublic = LocalPayload->HasField(TEXT("isPublic"))
        ? GetJsonBoolField(LocalPayload, TEXT("isPublic")) : false;

    FEdGraphPinType PinType;
    const FString CleanVarType = VarType.TrimStartAndEnd();
    if (CleanVarType.IsEmpty())
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
    }
    else
    {
        // Wildcard fallback drives the error check that follows.
        BlueprintHandlerUtils::MakePinTypeFromBpirText(CleanVarType, PinType);
        if (PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
        {
            Ctx.SendError(TEXT("TYPE_NOT_FOUND"),
                FString::Printf(
                    TEXT("Could not resolve variableType '%s'. %s"),
                    *VarType, GetAcceptedPinTypeFormsText()));
            return true;
        }
    }

    auto* Subsystem = Ctx.GetSubsystem();
    FString RegKey = Path;
    FString NormPath;
    if (FindBlueprintNormalizedPath(Path, NormPath) && !NormPath.TrimStartAndEnd().IsEmpty())
        RegKey = NormPath;

    if (FPluginState::Get().Blueprints().IsBusy(RegKey))
    {
        Ctx.SendError(TEXT("BLUEPRINT_BUSY"), FString::Printf(TEXT("Blueprint %s is busy"), *RegKey));
        return true;
    }

    FPluginState::Get().Blueprints().MarkBusy(RegKey);
    ON_SCOPE_EXIT {
        if (FPluginState::Get().Blueprints().IsBusy(RegKey)) FPluginState::Get().Blueprints().ClearBusy(RegKey);
    };

    FString LocalNormalized, LocalLoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(Path, LocalNormalized, LocalLoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LocalLoadError.IsEmpty() ? TEXT("Failed to load blueprint") : *LocalLoadError);
        return true;
    }

    const FString RegistryKey = !LocalNormalized.IsEmpty() ? LocalNormalized : Path;

    // Check if already exists
    bool bAlreadyExists = false;
    for (const FBPVariableDescription& Existing : Blueprint->NewVariables)
    {
        if (Existing.VarName == FName(*VarName))
        {
            bAlreadyExists = true;
            break;
        }
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("blueprintPath"), RegistryKey);
    Response->SetStringField(TEXT("variableName"), VarName);

    if (bAlreadyExists)
    {
        const TSharedPtr<FJsonObject> Snapshot = BuildBlueprintSnapshot(Blueprint, RegistryKey);
        if (Snapshot.IsValid()) Response->SetObjectField(TEXT("blueprint"), Snapshot);
        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("note"), TEXT("Variable already exists; no changes applied."));
        Ctx.SendSuccess(Response);
        return true;
    }

    // A variable category is an editor-only organizational label, not persisted
    // localizable game/UI text — see MakeBlueprintCategoryText for the rationale.
    const FText CategoryText = BlueprintHandlerUtils::MakeBlueprintCategoryText(Category);

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.add_variable")));
    Blueprint->Modify();
    FBPVariableDescription NewVar;
    NewVar.VarName = FName(*VarName);
    NewVar.VarGuid = FGuid::NewGuid();
    NewVar.FriendlyName = VarName;
    NewVar.Category = CategoryText;
    NewVar.VarType = PinType;
    NewVar.PropertyFlags |= CPF_Edit;
    NewVar.PropertyFlags |= CPF_BlueprintVisible;
    NewVar.PropertyFlags &= ~CPF_BlueprintReadOnly;
    if (bReplicated) NewVar.PropertyFlags |= CPF_Net;
    // FBPVariableDescription.DefaultValue is the export-text string the Blueprint
    // editor stores; the compiler lands it on the CDO. This mirrors the engine's
    // own FBlueprintEditorUtils::AddMemberVariable(Blueprint, Name, Type, DefaultValue),
    // which the manual add path below otherwise leaves unset (CDO reads back zero).
    if (bHasDefaultValue) NewVar.DefaultValue = DefaultValue;

    Blueprint->NewVariables.Add(NewVar);
    if (bPublic)
    {
        FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(
            Blueprint, NewVar.VarName, nullptr, GetVariablePrivateMetadataKey());
    }
    else
    {
        FBlueprintEditorUtils::SetBlueprintVariableMetaData(
            Blueprint, NewVar.VarName, nullptr, GetVariablePrivateMetadataKey(), TEXT("true"));
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const FBlueprintCompileDiagnostics CompileDiagnostics =
        CompileBlueprintWithDiagnostics(Blueprint);

    // After compile the CDO exists; apply the requested default authoritatively through
    // the same shared coercion path blueprint.set_default uses (the compiler also lands
    // NewVar.DefaultValue via export-text, but routing it here makes "did it land?" a
    // fact the shared ladder reports, with a real ConversionError on failure, and keeps
    // coercibility identical across the two handlers for every property type). Done
    // before the save so the applied value persists.
    bool bDefaultApplied = false;
    FString DefaultConversionError;
    FProperty* DefaultAppliedProp = nullptr;
    if (bHasDefaultValue && Blueprint->GeneratedClass)
    {
        if (UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject())
        {
            DefaultAppliedProp = FindFProperty<FProperty>(CDO->GetClass(), FName(*VarName));
            if (DefaultAppliedProp)
            {
                const TSharedPtr<FJsonValue> CoercedDefault =
                    CoerceStringToJsonValueByProperty(DefaultValue, DefaultAppliedProp);
                bDefaultApplied = ApplyJsonValueToProperty(
                    CDO, DefaultAppliedProp, CoercedDefault, DefaultConversionError);
            }
        }
    }

    // WasSavePersisted, not the raw call: the throttle skip and the transient-package
    // early-out used to return true, so a second edit inside the 0.5s window reported
    // saved:true with the change still only in memory.
    const bool bSaved = WasSavePersisted(SaveLoadedAssetThrottled(Blueprint));

    // Verify
    bool bVerified = false;
    if (Blueprint->GeneratedClass)
    {
        if (FindFProperty<FProperty>(Blueprint->GeneratedClass, FName(*VarName)))
            bVerified = true;
    }
    if (!bVerified)
    {
        for (const FBPVariableDescription& Var : Blueprint->NewVariables)
        {
            if (Var.VarName == FName(*VarName)) { bVerified = true; break; }
        }
    }
    if (!bVerified)
    {
        Ctx.SendError(TEXT("VERIFICATION_FAILED"), TEXT("Verification failed: variable not found after add"));
        return true;
    }

    // Echo the default that landed on the CDO so a dropped/failed coercion is visible
    // in-band (the variable readback object never carries a default field), and surface
    // the shared coercion path's real error when the value could not be applied — rather
    // than reporting a clean success the agent can't tell apart from a real apply.
    if (DefaultAppliedProp && Blueprint->GeneratedClass)
    {
        if (UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject())
        {
            const TSharedPtr<FJsonValue> AppliedDefault =
                ExportPropertyToJsonValue(CDO, DefaultAppliedProp);
            if (AppliedDefault.IsValid())
                Response->SetField(TEXT("defaultValue"), AppliedDefault);

            if (!bDefaultApplied)
            {
                Response->SetStringField(TEXT("warning"),
                    FString::Printf(
                        TEXT("defaultValue '%s' could not be applied to type '%s': %s. The variable was created with the type's zero value."),
                        *DefaultValue, *VarType, *DefaultConversionError));
            }
        }
    }

    Response->SetBoolField(TEXT("success"), true);
    Response->SetBoolField(TEXT("saved"), bSaved);
    if (!VarType.IsEmpty()) Response->SetStringField(TEXT("variableType"), VarType);
    if (!Category.IsEmpty()) Response->SetStringField(TEXT("category"), Category);
    Response->SetBoolField(TEXT("replicated"), bReplicated);
    Response->SetBoolField(TEXT("public"), bPublic);
    const TSharedPtr<FJsonObject> Snapshot = BuildBlueprintSnapshot(Blueprint, RegistryKey);
    if (Snapshot.IsValid())
    {
        Response->SetObjectField(TEXT("blueprint"), Snapshot);
        if (Snapshot->HasField(TEXT("variables")))
        {
            const TArray<TSharedPtr<FJsonValue>> Vars = Snapshot->GetArrayField(TEXT("variables"));
            if (const TSharedPtr<FJsonObject> VarJson = FindNamedEntry(Vars, TEXT("name"), VarName))
                Response->SetObjectField(TEXT("variable"), VarJson);
        }
    }
    AddAssetVerification(Response, Blueprint);
    AddCompileDiagnosticsToJson(CompileDiagnostics, Response);
    Ctx.SendSuccess(Response);
    return true;
}

// ---- blueprint.remove_variable ----
REGISTER_RPC_HANDLER("blueprint.remove_variable", "blueprint", "Remove a variable from a blueprint",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("variableName", "string", "Name of variable to remove")
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.remove_variable requires a blueprint path."));
        return true;
    }

    FString VarName = Ctx.GetString(TEXT("variableName"));
    if (VarName.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("variableName required"));
        return true;
    }

    FString LocalNormalized, LocalLoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(Path, LocalNormalized, LocalLoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LocalLoadError.IsEmpty() ? TEXT("Failed to load blueprint") : *LocalLoadError);
        return true;
    }

    const FName TargetVarName(*VarName);
    bool bFound = false;
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        if (Var.VarName == TargetVarName) { bFound = true; break; }
    }
    if (!bFound)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Variable '%s' not found in blueprint."), *VarName));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.remove_variable")));
    FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, TargetVarName);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const FBlueprintCompileDiagnostics CompileDiagnostics =
        CompileBlueprintWithDiagnostics(Blueprint);
    // WasSavePersisted, not the raw call: the throttle skip and the transient-package
    // early-out used to return true, so a second edit inside the 0.5s window reported
    // saved:true with the change still only in memory.
    const bool bSaved = WasSavePersisted(SaveLoadedAssetThrottled(Blueprint));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("variableName"), VarName);
    Result->SetStringField(TEXT("blueprintPath"), LocalNormalized);
    // bSaved was computed and then dropped, so the response said nothing about
    // persistence at all and left AddAssetVerification's existsAfter as the only
    // (formerly constant) signal.
    Result->SetBoolField(TEXT("saved"), bSaved);
    AddAssetVerification(Result, Blueprint);
    AddCompileDiagnosticsToJson(CompileDiagnostics, Result);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.rename_variable ----
REGISTER_RPC_HANDLER("blueprint.rename_variable", "blueprint", "Rename a variable in a blueprint",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("oldName", "string", "Current variable name"),
        RPC_PARAM_REQ("newName", "string", "New variable name")
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.rename_variable requires a blueprint path."));
        return true;
    }

    FString OldName = Ctx.GetString(TEXT("oldName"));
    FString NewName = Ctx.GetString(TEXT("newName"));
    if (OldName.IsEmpty() || NewName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'oldName' or 'newName' in payload."));
        return true;
    }

    auto* Subsystem = Ctx.GetSubsystem();
    FString LocalNormalized, LocalLoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(Path, LocalNormalized, LocalLoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LocalLoadError.IsEmpty() ? TEXT("Failed to load blueprint") : *LocalLoadError);
        return true;
    }

    const FName OldVarName(*OldName);
    bool bFound = false;
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        if (Var.VarName == OldVarName) { bFound = true; break; }
    }
    if (!bFound)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Variable '%s' not found in blueprint."), *OldName));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.rename_variable")));
    FBlueprintEditorUtils::RenameMemberVariable(Blueprint, OldVarName, FName(*NewName));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const FBlueprintCompileDiagnostics CompileDiagnostics =
        CompileBlueprintWithDiagnostics(Blueprint);
    // WasSavePersisted, not the raw call: the throttle skip and the transient-package
    // early-out used to return true, so a second edit inside the 0.5s window reported
    // saved:true with the change still only in memory.
    const bool bSaved = WasSavePersisted(SaveLoadedAssetThrottled(Blueprint));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("oldName"), OldName);
    Result->SetStringField(TEXT("newName"), NewName);
    Result->SetStringField(TEXT("blueprintPath"), LocalNormalized);
    // bSaved was computed and then dropped; report it rather than leaving the
    // (formerly constant) existsAfter as the only persistence signal.
    Result->SetBoolField(TEXT("saved"), bSaved);
    AddAssetVerification(Result, Blueprint);
    AddCompileDiagnosticsToJson(CompileDiagnostics, Result);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.set_default ----
REGISTER_RPC_HANDLER("blueprint.set_default", "blueprint", "Mutate the Blueprint's class default object (CDO) so every newly spawned instance starts with this value. For per-instance edits on placed actors use actor.set_blueprint_variables instead. A CDO write only persists through a compile, so this verb compiles — and is therefore refused with LIVE_INSTANCES_WOULD_BE_REINSTANCED when loaded worlds hold live instances of the class; see allowReinstancing.",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path.")),
        RPC_PARAM_REQ("propertyName", "string", "UPROPERTY name on the Blueprint's class (case-sensitive FName lookup)."),
        RPC_PARAM_REQ("value", "string", "JSON-compatible string value; type-coerced to the property's type via reflection."),
        BlueprintReinstancingGuard::AllowReinstancingParam()
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.set_default requires a blueprint path."));
        return true;
    }

    const TSharedPtr<FJsonObject>& LocalPayload = Ctx.GetRawPayload();
    FString PropertyName;
    LocalPayload->TryGetStringField(TEXT("propertyName"), PropertyName);
    if (PropertyName.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("propertyName required"));
        return true;
    }

    const TSharedPtr<FJsonValue> ValueField = LocalPayload->TryGetField(TEXT("value"));
    if (!ValueField.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("value field required"));
        return true;
    }

    auto* Subsystem = Ctx.GetSubsystem();
    FString LocalNormalized, LocalLoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(Path, LocalNormalized, LocalLoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LocalLoadError.IsEmpty() ? TEXT("Failed to load blueprint") : *LocalLoadError);
        return true;
    }

    if (!Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT"), TEXT("Blueprint has no generated class"));
        return true;
    }

    // Before the CDO is touched, not after: this verb finalizes with a full compile
    // (see the persistence note below), so it reinstances exactly like blueprint.compile.
    // Refusing here leaves the Blueprint untouched; refusing after the write would leave
    // a CDO edit stranded in memory with no compile to persist it.
    if (BlueprintReinstancingGuard::RefuseIfLiveInstancesWouldBeReinstanced(
            Ctx, Blueprint, TEXT("blueprint.set_default")))
    {
        return true;
    }

    UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
    if (!CDO)
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT"), TEXT("Could not get CDO"));
        return true;
    }

    void* TargetContainer = nullptr;
    FProperty* Property = nullptr;
    FString ResolveError;

    if (PropertyName.Contains(TEXT(".")))
    {
        Property = ResolveNestedPropertyPath(CDO, PropertyName, TargetContainer, ResolveError);
    }
    else
    {
        TargetContainer = CDO;
        Property = CDO->GetClass()->FindPropertyByName(*PropertyName);
        if (!Property)
            ResolveError = FString::Printf(TEXT("Property '%s' not found"), *PropertyName);
    }

    if (!Property || !TargetContainer)
    {
        Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"),
            ResolveError.IsEmpty() ? TEXT("Property not found") : *ResolveError);
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.set_default")));
    Blueprint->Modify();
    CDO->Modify();

    FString ConversionError;
    if (!ApplyJsonValueToProperty(TargetContainer, Property, ValueField, ConversionError))
    {
        Ctx.SendError(TEXT("CONVERSION_FAILED"), ConversionError);
        return true;
    }

    // A raw CDO write is in-memory only: without a compile the new default never
    // reaches the serialized Blueprint and silently reverts on editor restart
    // (B-blueprint-set-default-not-persisted). Mirror the SCS finalize sequence
    // (FinalizeBlueprintSCSChange): structural mark + compile, then re-apply to
    // the rebuilt CDO and mark the package for save.
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    // Surveyed before the compile because the flush destroys these objects; reported on
    // the response below so an opted-in caller learns which worlds were rebuilt.
    const BlueprintReinstancingGuard::FLiveInstanceSurvey ReinstancedSurvey =
        BlueprintReinstancingGuard::SurveyLiveInstances(Blueprint);
    const FBlueprintCompileDiagnostics CompileDiagnostics =
        CompileBlueprintWithDiagnostics(Blueprint);

    // The compile regenerated the class and rebuilt the CDO, invalidating the
    // pre-compile Property/TargetContainer pointers — re-resolve and re-apply.
    CDO = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
    if (!CDO)
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT"), TEXT("Could not get CDO after compile"));
        return true;
    }
    CDO->Modify();

    TargetContainer = nullptr;
    Property = nullptr;
    ResolveError.Empty();
    FPropertyNotifyTarget ExportTarget;
    ExportTarget.Object = CDO;
    ExportTarget.RelativePath = PropertyName;
    if (PropertyName.Contains(TEXT(".")))
    {
        Property = ResolveNestedPropertyPath(
            CDO, PropertyName, TargetContainer, ResolveError, &ExportTarget);
    }
    else
    {
        TargetContainer = CDO;
        Property = CDO->GetClass()->FindPropertyByName(*PropertyName);
        if (!Property)
            ResolveError = FString::Printf(TEXT("Property '%s' not found after compile"), *PropertyName);
    }

    if (!Property || !TargetContainer)
    {
        Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"),
            ResolveError.IsEmpty() ? TEXT("Property not found after compile") : *ResolveError);
        return true;
    }

    if (!ApplyJsonValueToProperty(TargetContainer, Property, ValueField, ConversionError))
    {
        Ctx.SendError(TEXT("CONVERSION_FAILED"), ConversionError);
        return true;
    }

    // Mark-for-save via the same helper the SCS path uses; an immediate
    // SaveLoadedAsset here risks the recursive-FlushRenderingCommands crash
    // documented in FinalizeBlueprintSCSChange. That means nothing here writes,
    // so `saved` is reported below from a measurement rather than from this call
    // (whose bool return was the constant true for any non-null Blueprint —
    // B-blueprint-set-default-not-persisted was exactly this).
    McpSafeAssetSave(Blueprint);

    // Read back from the post-compile CDO so the echo reports what the compiled
    // class actually holds.
    const FPropertyExportSource ExportSource = FPropertyExportSource::FromResolvedContainer(
        TargetContainer, ExportTarget.Object);
    TSharedPtr<FJsonValue> CurrentValue = ExportPropertyToJsonValue(ExportSource, Property);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("propertyName"), PropertyName);
    Result->SetStringField(TEXT("blueprintPath"), LocalNormalized);
    AddMarkDirtySaveReport(Result, Blueprint, /*bSaveRequested=*/true);
    if (CurrentValue.IsValid())
        Result->SetField(TEXT("value"), CurrentValue);
    AddCompileDiagnosticsToJson(CompileDiagnostics, Result);
    BlueprintReinstancingGuard::AddSurveyToJson(ReinstancedSurvey, Result);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.set_variable_metadata ----
REGISTER_RPC_HANDLER("blueprint.set_variable_metadata", "blueprint", "Apply metadata to a Blueprint variable",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("variableName", "string", "Variable name"),
        RPC_PARAM_REQ("metadata", "object", "Key-value metadata to set")
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.set_variable_metadata requires a blueprint path."));
        return true;
    }

    const TSharedPtr<FJsonObject>& LocalPayload = Ctx.GetRawPayload();
    FString VarName;
    LocalPayload->TryGetStringField(TEXT("variableName"), VarName);
    if (VarName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("variableName required"));
        return true;
    }

    const TSharedPtr<FJsonValue> MetaVal = LocalPayload->TryGetField(TEXT("metadata"));
    const TSharedPtr<FJsonObject> MetaObjPtr = MetaVal.IsValid() && MetaVal->Type == EJson::Object
        ? MetaVal->AsObject() : nullptr;
    if (!MetaObjPtr.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("metadata object required"));
        return true;
    }

    if (FPluginState::Get().Blueprints().IsBusy(Path))
    {
        Ctx.SendError(TEXT("BLUEPRINT_BUSY"), TEXT("Blueprint is busy"));
        return true;
    }

    FPluginState::Get().Blueprints().MarkBusy(Path);
    ON_SCOPE_EXIT {
        if (FPluginState::Get().Blueprints().IsBusy(Path)) FPluginState::Get().Blueprints().ClearBusy(Path);
    };

    auto* Subsystem = Ctx.GetSubsystem();
    FString Normalized, LoadErr;
    UBlueprint* Blueprint = LoadBlueprintAsset(Path, Normalized, LoadErr);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LoadErr.IsEmpty() ? TEXT("Failed to load blueprint") : *LoadErr);
        return true;
    }

    const FString RegistryKey = Normalized.IsEmpty() ? Path : Normalized;

    // Find variable (case-insensitive)
    FBPVariableDescription* VariableDesc = nullptr;
    for (FBPVariableDescription& Desc : Blueprint->NewVariables)
    {
        if (Desc.VarName == FName(*VarName))
        {
            VariableDesc = &Desc; break;
        }
        if (Desc.VarName.ToString().Equals(VarName, ESearchCase::IgnoreCase))
        {
            VariableDesc = &Desc;
            VarName = Desc.VarName.ToString();
            break;
        }
    }
    if (!VariableDesc)
    {
        Ctx.SendError(TEXT("VARIABLE_NOT_FOUND"), TEXT("Variable not found"));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.set_variable_metadata")));
    Blueprint->Modify();
    TArray<FString> AppliedKeys;
    for (const auto& Pair : MetaObjPtr->Values)
    {
        if (!Pair.Value.IsValid()) continue;
        const FString ValueStr = JsonValueToString(Pair.Value);
        const FName MetaKey = ResolveMetadataKey(EARGCompat::JsonKeyToString(Pair.Key));

        if (ValueStr.IsEmpty())
            FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Blueprint, VariableDesc->VarName, nullptr, MetaKey);
        else
            FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VariableDesc->VarName, nullptr, MetaKey, ValueStr);

        AppliedKeys.Add(MetaKey.ToString());
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const FBlueprintCompileDiagnostics CompileDiagnostics =
        CompileBlueprintWithDiagnostics(Blueprint);
    // WasSavePersisted, not the raw call: the throttle skip and the transient-package
    // early-out used to return true, so a second edit inside the 0.5s window reported
    // saved:true with the change still only in memory.
    const bool bSaved = WasSavePersisted(SaveLoadedAssetThrottled(Blueprint));

    const TSharedPtr<FJsonObject> Snapshot = BuildBlueprintSnapshot(Blueprint, RegistryKey);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("blueprintPath"), RegistryKey);
    Resp->SetStringField(TEXT("variableName"), VarName);
    Resp->SetBoolField(TEXT("saved"), bSaved);
    TArray<TSharedPtr<FJsonValue>> AppliedKeysJson;
    for (const FString& Key : AppliedKeys)
        AppliedKeysJson.Add(MakeShared<FJsonValueString>(Key));
    Resp->SetArrayField(TEXT("appliedKeys"), AppliedKeysJson);
    if (Snapshot.IsValid())
    {
        if (Snapshot->HasField(TEXT("metadata")))
            Resp->SetObjectField(TEXT("metadata"), Snapshot->GetObjectField(TEXT("metadata")));
        Resp->SetObjectField(TEXT("blueprint"), Snapshot);
    }
    AddCompileDiagnosticsToJson(CompileDiagnostics, Resp);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- blueprint.set_variable_settings ----
REGISTER_RPC_HANDLER("blueprint.set_variable_settings", "blueprint", "Set Blueprint variable settings (visibility, editable/read-only, expose-on-spawn, replication, and category)",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("variableName", "string", "Variable name"),
        RPC_PARAM_OPT("isPublic", "boolean", "Whether variable is public"),
        RPC_PARAM_OPT("isPrivate", "boolean", "Whether variable is private"),
        RPC_PARAM_OPT("isInstanceEditable", "boolean", "Whether variable is instance-editable"),
        RPC_PARAM_OPT("isReadOnly", "boolean", "Whether variable is read-only in Blueprints"),
        RPC_PARAM_OPT("exposeOnSpawn", "boolean", "Whether variable is exposed on spawn"),
        RPC_PARAM_OPT("isReplicated", "boolean", "Whether variable is replicated"),
        RPC_PARAM_OPT("isTransient", "boolean", "Whether variable is transient"),
        RPC_PARAM_OPT("isSaveGame", "boolean", "Whether variable has SaveGame flag"),
        RPC_PARAM_OPT("isAdvancedDisplay", "boolean", "Whether variable is advanced display"),
        RPC_PARAM_OPT("category", "string", "Variable category")
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.set_variable_settings requires a blueprint path."));
        return true;
    }

    const TSharedPtr<FJsonObject>& LocalPayload = Ctx.GetRawPayload();
    FString VarName;
    LocalPayload->TryGetStringField(TEXT("variableName"), VarName);
    if (VarName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("variableName required"));
        return true;
    }

    auto ReadOptionalBool = [&LocalPayload](std::initializer_list<const TCHAR*> Keys) -> TOptional<bool>
    {
        for (const TCHAR* Key : Keys)
        {
            if (LocalPayload->HasField(Key))
            {
                return GetJsonBoolField(LocalPayload, Key, false);
            }
        }
        return TOptional<bool>();
    };

    const TOptional<bool> PublicSetting = ReadOptionalBool({ TEXT("isPublic"), TEXT("public") });
    const TOptional<bool> PrivateSetting = ReadOptionalBool({ TEXT("isPrivate"), TEXT("private") });
    const TOptional<bool> InstanceEditableSetting = ReadOptionalBool(
        { TEXT("isInstanceEditable"), TEXT("instanceEditable"), TEXT("editable") });
    const TOptional<bool> ReadOnlySetting = ReadOptionalBool(
        { TEXT("isReadOnly"), TEXT("readOnly"), TEXT("isConst"), TEXT("const") });
    const TOptional<bool> ExposeOnSpawnSetting = ReadOptionalBool({ TEXT("exposeOnSpawn") });
    const TOptional<bool> ReplicatedSetting = ReadOptionalBool({ TEXT("isReplicated"), TEXT("replicated") });
    const TOptional<bool> TransientSetting = ReadOptionalBool({ TEXT("isTransient"), TEXT("transient") });
    const TOptional<bool> SaveGameSetting = ReadOptionalBool({ TEXT("isSaveGame"), TEXT("saveGame") });
    const TOptional<bool> AdvancedDisplaySetting = ReadOptionalBool(
        { TEXT("isAdvancedDisplay"), TEXT("advancedDisplay") });

    FString Category;
    const bool bHasCategory = LocalPayload->TryGetStringField(TEXT("category"), Category);

    const bool bHasAnySetting =
        PublicSetting.IsSet() || PrivateSetting.IsSet() || InstanceEditableSetting.IsSet() ||
        ReadOnlySetting.IsSet() || ExposeOnSpawnSetting.IsSet() || ReplicatedSetting.IsSet() ||
        TransientSetting.IsSet() || SaveGameSetting.IsSet() || AdvancedDisplaySetting.IsSet() || bHasCategory;

    if (!bHasAnySetting)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("No variable settings were provided."));
        return true;
    }

    if (PublicSetting.IsSet() && PrivateSetting.IsSet() &&
        (PublicSetting.GetValue() == PrivateSetting.GetValue()))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("Conflicting visibility settings: isPublic and isPrivate cannot both be true or both be false."));
        return true;
    }

    FString Normalized, LoadErr;
    UBlueprint* Blueprint = LoadBlueprintAsset(Path, Normalized, LoadErr);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LoadErr.IsEmpty() ? TEXT("Failed to load blueprint") : *LoadErr);
        return true;
    }

    const FString RegistryKey = Normalized.IsEmpty() ? Path : Normalized;

    FBPVariableDescription* VariableDesc = nullptr;
    for (FBPVariableDescription& Desc : Blueprint->NewVariables)
    {
        if (Desc.VarName == FName(*VarName))
        {
            VariableDesc = &Desc;
            break;
        }
        if (Desc.VarName.ToString().Equals(VarName, ESearchCase::IgnoreCase))
        {
            VariableDesc = &Desc;
            VarName = Desc.VarName.ToString();
            break;
        }
    }
    if (!VariableDesc)
    {
        Ctx.SendError(TEXT("VARIABLE_NOT_FOUND"), TEXT("Variable not found"));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.set_variable_settings")));
    Blueprint->Modify();

    TArray<FString> AppliedSettings;
    TOptional<bool> DesiredPrivate;
    if (PrivateSetting.IsSet())
    {
        DesiredPrivate = PrivateSetting.GetValue();
    }
    else if (PublicSetting.IsSet())
    {
        DesiredPrivate = !PublicSetting.GetValue();
    }

    if (DesiredPrivate.IsSet())
    {
        if (DesiredPrivate.GetValue())
        {
            FBlueprintEditorUtils::SetBlueprintVariableMetaData(
                Blueprint, VariableDesc->VarName, nullptr, GetVariablePrivateMetadataKey(), TEXT("true"));
        }
        else
        {
            FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(
                Blueprint, VariableDesc->VarName, nullptr, GetVariablePrivateMetadataKey());
        }
        AppliedSettings.Add(TEXT("private"));
    }

    if (InstanceEditableSetting.IsSet())
    {
        FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(
            Blueprint, VariableDesc->VarName, !InstanceEditableSetting.GetValue());
        AppliedSettings.Add(TEXT("instanceEditable"));
    }

    if (ReadOnlySetting.IsSet())
    {
        FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(
            Blueprint, VariableDesc->VarName, ReadOnlySetting.GetValue());
        AppliedSettings.Add(TEXT("readOnly"));
    }

    if (ExposeOnSpawnSetting.IsSet())
    {
        if (ExposeOnSpawnSetting.GetValue())
        {
            FBlueprintEditorUtils::SetBlueprintVariableMetaData(
                Blueprint, VariableDesc->VarName, nullptr, GetExposeOnSpawnMetadataKey(), TEXT("true"));
        }
        else
        {
            FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(
                Blueprint, VariableDesc->VarName, nullptr, GetExposeOnSpawnMetadataKey());
        }
        AppliedSettings.Add(TEXT("exposeOnSpawn"));
    }

    if (ReplicatedSetting.IsSet())
    {
        if (uint64* PropertyFlags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, VariableDesc->VarName))
        {
            if (ReplicatedSetting.GetValue())
            {
                *PropertyFlags |= CPF_Net;
            }
            else
            {
                *PropertyFlags &= ~CPF_Net;
            }
            AppliedSettings.Add(TEXT("replicated"));
        }
    }

    if (TransientSetting.IsSet())
    {
        FBlueprintEditorUtils::SetVariableTransientFlag(
            Blueprint, VariableDesc->VarName, TransientSetting.GetValue());
        AppliedSettings.Add(TEXT("transient"));
    }

    if (SaveGameSetting.IsSet())
    {
        FBlueprintEditorUtils::SetVariableSaveGameFlag(
            Blueprint, VariableDesc->VarName, SaveGameSetting.GetValue());
        AppliedSettings.Add(TEXT("saveGame"));
    }

    if (AdvancedDisplaySetting.IsSet())
    {
        FBlueprintEditorUtils::SetVariableAdvancedDisplayFlag(
            Blueprint, VariableDesc->VarName, AdvancedDisplaySetting.GetValue());
        AppliedSettings.Add(TEXT("advancedDisplay"));
    }

    if (bHasCategory)
    {
        // Editor-only organizational label, not persisted localizable text —
        // see MakeBlueprintCategoryText for the rationale.
        const FText CategoryText = BlueprintHandlerUtils::MakeBlueprintCategoryText(Category);
        FBlueprintEditorUtils::SetBlueprintVariableCategory(
            Blueprint, VariableDesc->VarName, nullptr, CategoryText, true);
        AppliedSettings.Add(TEXT("category"));
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const FBlueprintCompileDiagnostics CompileDiagnostics =
        CompileBlueprintWithDiagnostics(Blueprint);
    // WasSavePersisted, not the raw call: the throttle skip and the transient-package
    // early-out used to return true, so a second edit inside the 0.5s window reported
    // saved:true with the change still only in memory.
    const bool bSaved = WasSavePersisted(SaveLoadedAssetThrottled(Blueprint));

    const TSharedPtr<FJsonObject> Snapshot = BuildBlueprintSnapshot(Blueprint, RegistryKey);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("blueprintPath"), RegistryKey);
    Resp->SetStringField(TEXT("variableName"), VarName);
    Resp->SetBoolField(TEXT("saved"), bSaved);
    TArray<TSharedPtr<FJsonValue>> AppliedArray;
    for (const FString& Setting : AppliedSettings)
    {
        AppliedArray.Add(MakeShared<FJsonValueString>(Setting));
    }
    Resp->SetArrayField(TEXT("appliedSettings"), AppliedArray);
    if (Snapshot.IsValid())
    {
        Resp->SetObjectField(TEXT("blueprint"), Snapshot);
        if (Snapshot->HasField(TEXT("variables")))
        {
            if (const TSharedPtr<FJsonObject> VarJson = FindNamedEntry(
                    Snapshot->GetArrayField(TEXT("variables")), TEXT("name"), VarName))
            {
                Resp->SetObjectField(TEXT("variable"), VarJson);
            }
        }
    }
    AddAssetVerification(Resp, Blueprint);
    AddCompileDiagnosticsToJson(CompileDiagnostics, Resp);
    Ctx.SendSuccess(Resp);
    return true;
}
