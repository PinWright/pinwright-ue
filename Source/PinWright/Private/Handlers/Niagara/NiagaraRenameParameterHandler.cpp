// Copyright (c) 2026 Alexander Penkin. MIT License.

// Handler for niagara.rename_parameter.
// UE 5.6 does not export FNiagaraSystemViewModel::RenameParameter to this plugin,
// so the implementation mirrors the reachable parts of that path with exported APIs.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraJsonHelpers.h"
#include "Handlers/Niagara/NiagaraParameterRenameUtils.h"

#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"

REGISTER_RPC_HANDLER("niagara.rename_parameter", "niagara",
    "Rename a Niagara user parameter through exported UE APIs: exposed store, user metadata, graphs, assignment nodes, and system rename hooks. "
    "Cross-namespace renames and standalone emitter assets are rejected in v1.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System asset"),
        RPC_PARAM_REQ("scope",     "string", "Parameter scope (e.g. 'user')"),
        RPC_PARAM_REQ("oldName",   "string", "Existing full parameter name including namespace (e.g. 'User.Speed')"),
        RPC_PARAM_REQ("newName",   "string", "New full parameter name including namespace (e.g. 'User.Velocity')"),
        RPC_PARAM_OPT("emitter",   "string", "Unused in v1; reserved for future emitter-scope support"),
        RPC_PARAM_OPT("compile",   "boolean", "Compile the system after renaming"),
        RPC_PARAM_OPT("save",      "boolean", "Save the asset after renaming")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;

    FString Scope;
    if (!Ctx.RequireString(TEXT("scope"), Scope)) return true;

    FString OldName;
    if (!Ctx.RequireString(TEXT("oldName"), OldName)) return true;

    FString NewName;
    if (!Ctx.RequireString(TEXT("newName"), NewName)) return true;

    bool bCompile = false;
    bool bSave = false;
    Ctx.GetRawPayload()->TryGetBoolField(TEXT("compile"), bCompile);
    Ctx.GetRawPayload()->TryGetBoolField(TEXT("save"), bSave);

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load asset at '%s'."), *AssetPath));
        return true;
    }
    if (Cast<UNiagaraEmitter>(Asset))
    {
        Ctx.SendError(TEXT("EMITTER_ONLY_UNSUPPORTED"),
            TEXT("niagara.rename_parameter does not support standalone UNiagaraEmitter assets in v1. Use a UNiagaraSystem asset path instead."));
        return true;
    }

    UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
    if (!System)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Asset at '%s' is not a Niagara System."), *AssetPath));
        return true;
    }

    PinWrightNiagara::FNiagaraParameterRenameResult RenameResult;
    FString ErrorCode;
    FString ErrorMessage;
    if (!PinWrightNiagara::RenameNiagaraParameterWithExportedApis(
        *System,
        OldName,
        NewName,
        RenameResult,
        ErrorCode,
        ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    FNiagaraResolvedTarget Target;
    Target.Asset     = System;
    Target.System    = System;
    Target.AssetPath = AssetPath;
    Target.AssetKind = TEXT("NiagaraSystem");

    FNiagaraEditOptions Options;
    Options.bCompile = bCompile;
    Options.bSave    = bSave;

    bool bCompiled = false;
    bool bSaved    = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> RefsUpdated = MakeShared<FJsonObject>();
    RefsUpdated->SetBoolField(TEXT("userStore"), RenameResult.bUserStoreRenamed);
    RefsUpdated->SetBoolField(TEXT("userScriptMetadata"), RenameResult.bUserScriptMetadataRenamed);
    RefsUpdated->SetNumberField(TEXT("graphs"), static_cast<double>(RenameResult.GraphsRenamed));
    RefsUpdated->SetNumberField(TEXT("assignmentTargets"), static_cast<double>(RenameResult.AssignmentTargetsRenamed));
    RefsUpdated->SetBoolField(TEXT("systemRenameHook"), RenameResult.bSystemRenameHookInvoked);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("renamed"), true);
    Result->SetStringField(TEXT("scope"), Scope);
    Result->SetStringField(TEXT("oldName"), OldName);
    Result->SetStringField(TEXT("newName"), NewName);
    Result->SetObjectField(TEXT("referencesUpdated"), RefsUpdated);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetBoolField(TEXT("saved"), bSaved);

    Ctx.SendSuccess(Result);
    return true;
}
