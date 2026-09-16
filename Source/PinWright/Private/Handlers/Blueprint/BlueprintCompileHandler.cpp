// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintCompileHandler.cpp - Migrated from PinWright_BlueprintHandlers.cpp
// Blueprint compilation: compile, add_construction_script

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintReinstancingGuard.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

using namespace BlueprintHandlerUtils;

// ---- blueprint.compile ----
REGISTER_RPC_HANDLER("blueprint.compile", "blueprint", "Run UE's Blueprint compiler on the named asset and return any compile errors/warnings. Required after structural changes (new variables, graph edits, reparenting) before the BP is functional. Compile is in-memory only — to persist the result to disk, call asset.save afterward (it runs the same Blueprint integrity gate at the save choke point). blueprint.compile_bpir compiles + then runs this implicitly. Refused with LIVE_INSTANCES_WOULD_BE_REINSTANCED when loaded worlds hold live instances of the class; see allowReinstancing.",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path to compile.")),
        BlueprintReinstancingGuard::AllowReinstancingParam()
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.compile requires a blueprint path."));
        return true;
    }

    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(Path, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), LoadErr.IsEmpty() ? TEXT("Failed to load blueprint for compilation") : *LoadErr);
        return true;
    }

    // Live-instance precondition. A compile flushes the reinstancing queue, which
    // destroys and re-creates every live instance of the class in every loaded world —
    // including placed actors in a map another agent has open. Refuse unless the caller
    // said allowReinstancing=true (BlueprintReinstancingGuard.h has the engine chain).
    if (BlueprintReinstancingGuard::RefuseIfLiveInstancesWouldBeReinstanced(
            Ctx, BP, TEXT("blueprint.compile")))
    {
        return true;
    }

    // Compile-only: this verb checks compile status and never persists. Persistence
    // (and its Blueprint integrity gate) lives on the save path — call asset.save to
    // write to disk, where ValidateBlueprintGraphIntegrity runs at the universal
    // SaveLoadedAssetThrottled choke point for every save source.
    const FBlueprintCompileDiagnostics Diagnostics = CompileBlueprintWithDiagnostics(BP);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("blueprintPath"), Path);
    AddCompileDiagnosticsToJson(Diagnostics, Out);

    Ctx.SendSuccess(Out);
    return true;
}

// ---- blueprint.add_construction_script ----
REGISTER_RPC_HANDLER("blueprint.add_construction_script", "blueprint", "Ensure a construction script graph exists on a blueprint",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path"))
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.add_construction_script requires a blueprint path."));
        return true;
    }

    auto* Subsystem = Ctx.GetSubsystem();
    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(Path, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"), LoadErr.IsEmpty() ? TEXT("Failed to load blueprint") : *LoadErr);
        return true;
    }

    UEdGraph* ConstructionGraph = nullptr;
    for (UEdGraph* Graph : BP->FunctionGraphs)
    {
        if (Graph && Graph->GetFName() == UEdGraphSchema_K2::FN_UserConstructionScript)
        {
            ConstructionGraph = Graph;
            break;
        }
    }

    if (!ConstructionGraph)
    {
        ConstructionGraph = FBlueprintEditorUtils::CreateNewGraph(
            BP, UEdGraphSchema_K2::FN_UserConstructionScript,
            UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddFunctionGraph<UClass>(
            BP, ConstructionGraph, /*bIsUserCreated=*/false, nullptr);
    }

    if (ConstructionGraph)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        Result->SetStringField(TEXT("blueprintPath"), Path);
        Result->SetStringField(TEXT("graphName"), ConstructionGraph->GetName());
        Result->SetStringField(TEXT("note"),
            TEXT("Construction script graph ensured. Use blueprint.graph.create_node with graphName='UserConstructionScript' to add nodes."));
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(TEXT("GRAPH_ERROR"), TEXT("Failed to create construction script graph"));
    }
    return true;
}
