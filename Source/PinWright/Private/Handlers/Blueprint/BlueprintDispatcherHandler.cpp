// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintDispatcherHandler.cpp - Event dispatcher authoring

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Misc/ScopeExit.h"
#include "State/PluginState.h"
#include "Utils/AssetUtils.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

using namespace BlueprintHandlerUtils;

// ---- blueprint.add_dispatcher ----
REGISTER_RPC_HANDLER("blueprint.add_dispatcher", "blueprint", "Add an Event Dispatcher member variable and delegate signature graph to a Blueprint.",
    RPC_PARAMS(
        BlueprintPathParamOpt(
            TEXT("path"),
            TEXT("path"),
            TEXT("Blueprint asset path. Alias for blueprintPath."),
            EBlueprintPathParamAliasSet::ResolveExplicitBlueprintPath),
        BlueprintPathParamOpt(
            TEXT("blueprintPath"),
            TEXT("path"),
            TEXT("Blueprint asset path. Alias for path."),
            EBlueprintPathParamAliasSet::ResolveExplicitBlueprintPath),
        RPC_PARAM_REQ("name", "string", "Dispatcher name."),
        RPC_PARAM_OPT("params", "array", "Array of {name, type} delegate signature parameters."),
        RPC_PARAM_DEF("save", "boolean", "Save the Blueprint asset after compiling; defaults to false.", "false")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const FString Path = ResolveExplicitBlueprintPath(Payload);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.add_dispatcher requires path or blueprintPath."));
        return true;
    }

    FString Name;
    Payload->TryGetStringField(TEXT("name"), Name);
    Name = Name.TrimStartAndEnd();
    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("name required"));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* ParamsField = nullptr;
    Payload->TryGetArrayField(TEXT("params"), ParamsField);
    const TArray<TSharedPtr<FJsonValue>> Params =
        (ParamsField && ParamsField->Num() > 0)
            ? *ParamsField
            : TArray<TSharedPtr<FJsonValue>>();

    TArray<FParsedPinParam> ParsedParams;
    FString ParamError;
    if (!ParseNamedTypePinParams(
            Params,
            ParsedParams,
            EParsedPinParamMode::Strict,
            ParamError,
            TEXT("dispatcher param")))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), *ParamError);
        return true;
    }

    FString Normalized;
    if (!FindBlueprintNormalizedPath(Path, Normalized) || Normalized.TrimStartAndEnd().IsEmpty())
    {
        Normalized = Path;
    }

    if (FPluginState::Get().Blueprints().IsBusy(Normalized))
    {
        Ctx.SendError(TEXT("BLUEPRINT_BUSY"), FString::Printf(TEXT("Blueprint %s is busy"), *Normalized));
        return true;
    }

    FPluginState::Get().Blueprints().MarkBusy(Normalized);
    ON_SCOPE_EXIT
    {
        if (FPluginState::Get().Blueprints().IsBusy(Normalized))
        {
            FPluginState::Get().Blueprints().ClearBusy(Normalized);
        }
    };

    FString LoadNormalized;
    FString LoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(Path, LoadNormalized, LoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LoadError.IsEmpty() ? TEXT("Failed to load blueprint") : *LoadError);
        return true;
    }

    const FString RegistryKey = LoadNormalized.IsEmpty() ? Normalized : LoadNormalized;
    const FName DispatcherName(*Name);

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.add_dispatcher")));
    Blueprint->Modify();

    // Shared recipe: add the multicast-delegate member variable plus its backing delegate
    // signature graph so the property gets a real <Name>__DelegateSignature UFunction. The
    // same machinery interaction.add_interaction_events reuses.
    UEdGraph* SignatureGraph = nullptr;
    FString FailedParamName;
    const EAddDispatcherResult AddResult = AddDispatcherWithSignatureGraph(
        Blueprint, DispatcherName, ParsedParams, &SignatureGraph, &FailedParamName);
    switch (AddResult)
    {
    case EAddDispatcherResult::MemberExists:
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("Failed to add dispatcher '%s'; a member with that name may already exist."), *Name));
        return true;
    case EAddDispatcherResult::GraphUnavailable:
        Ctx.SendError(TEXT("GRAPH_UNAVAILABLE"), TEXT("Failed to create dispatcher signature graph"));
        return true;
    case EAddDispatcherResult::ParamFailed:
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("Failed to add dispatcher param '%s'"), *FailedParamName));
        return true;
    case EAddDispatcherResult::Success:
        break;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    const FBlueprintCompileDiagnostics Diagnostics = CompileBlueprintWithDiagnostics(Blueprint);
    const bool bSaveRequested = GetJsonBoolField(Payload, TEXT("save"), false);
    const bool bSaved = bSaveRequested && Diagnostics.bCompiled
        && WasSavePersisted(SaveLoadedAssetThrottled(Blueprint));

    const FString SignatureFunctionName = Name + TEXT("__DelegateSignature");

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("success"), Diagnostics.bCompiled);
    Response->SetStringField(TEXT("blueprintPath"), RegistryKey);
    Response->SetStringField(TEXT("name"), Name);
    Response->SetStringField(TEXT("signatureGraph"), SignatureGraph->GetName());
    Response->SetStringField(TEXT("signatureFunction"), SignatureFunctionName);
    Response->SetArrayField(TEXT("params"), Params);
    Response->SetBoolField(TEXT("saved"), bSaved);
    AddCompileDiagnosticsToJson(Diagnostics, Response);
    const TSharedPtr<FJsonObject> Snapshot = BuildBlueprintSnapshot(Blueprint, RegistryKey);
    if (Snapshot.IsValid())
    {
        Response->SetObjectField(TEXT("blueprint"), Snapshot);
    }
    AddAssetVerification(Response, Blueprint);
    Ctx.SendSuccess(Response);
    return true;
}
