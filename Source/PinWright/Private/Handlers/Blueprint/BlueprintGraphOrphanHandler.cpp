// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintGraphOrphanHandler.cpp - Find (read-only) and delete (mutating) orphaned blueprint graph nodes

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintGraphHelpers.h"
#include "Dom/JsonObject.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "K2Node_Composite.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

using namespace BlueprintHandlerUtils;

// ---------------------------------------------------------------------------
// Static helpers (editor-only)
// ---------------------------------------------------------------------------


static bool LoadOrphanBlueprint(FHandlerContext& Ctx, UBlueprint*& OutBlueprint)
{
    FString AssetPath = ResolveBlueprintPath(Ctx);
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"),
            TEXT("a blueprint path is required (use 'path' or 'assetPath')."));
        return false;
    }

    OutBlueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (!OutBlueprint)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load blueprint at path: %s"), *AssetPath));
        return false;
    }

    return true;
}

static bool ResolveOrphanBlueprintAndGraph(
    FHandlerContext& Ctx,
    UBlueprint*& OutBlueprint,
    UEdGraph*& OutGraph)
{
    if (!LoadOrphanBlueprint(Ctx, OutBlueprint))
    {
        return false;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString GraphName;
    Payload->TryGetStringField(TEXT("graphName"), GraphName);

    OutGraph = nullptr;

    if (GraphName.IsEmpty() || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
    {
        if (OutBlueprint->UbergraphPages.Num() > 0)
        {
            OutGraph = OutBlueprint->UbergraphPages[0];
        }
    }
    else
    {
        for (UEdGraph* Graph : OutBlueprint->FunctionGraphs)
        {
            if (Graph && Graph->GetName() == GraphName) { OutGraph = Graph; break; }
        }
        if (!OutGraph)
        {
            for (UEdGraph* Graph : OutBlueprint->UbergraphPages)
            {
                if (Graph && Graph->GetName() == GraphName) { OutGraph = Graph; break; }
            }
        }
    }

    if (!OutGraph)
    {
        TArray<UEdGraph*> AllGraphs;
        OutBlueprint->GetAllGraphs(AllGraphs);
        for (UEdGraph* Graph : AllGraphs)
        {
            if (Graph && Graph->GetName() == GraphName) { OutGraph = Graph; break; }
        }

        if (!OutGraph)
        {
            Ctx.SendError(TEXT("GRAPH_NOT_FOUND"),
                BlueprintGraphHelpers::BuildGraphNotFoundMessage(
                    GraphName.IsEmpty() ? TEXT("EventGraph") : GraphName, AllGraphs));
            return false;
        }
    }

    return true;
}

// Name the first composite left with a null entry/exit tunnel back-pointer, or return an
// empty string when every composite is intact. UK2Node_Composite::GetEntryNode()/GetExitNode()
// are check()-guarded, so a null back-pointer is not a recoverable error: it is a fatal assert
// the next time anything reconstructs the node — including regenerate-on-load, which makes the
// package unopenable. Compile and save both accept the broken state, so this has to be checked
// before the save, not inferred from its result.
static FString FindBrokenCompositeBoundary(UBlueprint* Blueprint)
{
    for (UEdGraph* Graph : CollectAllBlueprintGraphsRecursive(Blueprint))
    {
        if (!Graph) { continue; }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node);
            if (Composite && (!Composite->InputSinkNode || !Composite->OutputSourceNode))
            {
                return FString::Printf(TEXT("%s in graph '%s'"),
                    *Composite->GetName(), *Graph->GetName());
            }
        }
    }

    return FString();
}

static int32 DeleteOrphans(
    UBlueprint* Blueprint,
    const TArray<UEdGraph*>& GraphsToModify,
    const TArray<FBlueprintOrphanNodeInfo>& Orphans,
    FString& OutIntegrityError,
    FBlueprintCompileDiagnostics& OutDiagnostics)
{
    OutIntegrityError.Empty();

    if (!Blueprint || Orphans.Num() == 0)
    {
        return 0;
    }

    FScopedTransaction Transaction(NSLOCTEXT("PinWright", "DeleteOrphanedNodes", "Delete Orphaned Nodes"));
    Blueprint->Modify();
    for (UEdGraph* G : GraphsToModify)
    {
        if (G) { G->Modify(); }
    }

    int32 DeletedCount = 0;
    for (const FBlueprintOrphanNodeInfo& Info : Orphans)
    {
        if (Info.WeakNodePtr.IsValid())
        {
            FBlueprintEditorUtils::RemoveNode(Blueprint, Info.WeakNodePtr.Get(), true);
            DeletedCount++;
        }
    }

    const FString BrokenComposite = FindBrokenCompositeBoundary(Blueprint);
    if (!BrokenComposite.IsEmpty())
    {
        // Roll the deletions back and, above all, do not save: a saved half-torn composite
        // is unrecoverable without a git revert of the uasset.
        Transaction.Cancel();
        OutIntegrityError = FString::Printf(
            TEXT("Orphan sweep would leave composite %s with a null entry/exit tunnel, which asserts fatally on the next load of this package. Deletions rolled back and nothing was saved."),
            *BrokenComposite);
        return 0;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    OutDiagnostics = CompileBlueprintWithDiagnostics(Blueprint);
    SaveLoadedAssetThrottled(Blueprint);

    return DeletedCount;
}


// ---------------------------------------------------------------------------
// Handler: blueprint.graph.find_orphaned_nodes (read-only)
// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("blueprint.graph.find_orphaned_nodes", "blueprint.graph",
    "Find blueprint graph nodes unreachable from any entry point (read-only)",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_OPT("graphName", "string", "Graph name (omit to scan all graphs)"),
        RPC_PARAM_DEF("includeDataOnly", "boolean", "Also report transitive data-only orphan subgraphs", "true")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString GraphName;
    Payload->TryGetStringField(TEXT("graphName"), GraphName);
    const bool bScanAllGraphs = GraphName.IsEmpty();

    if (bScanAllGraphs)
    {
        UBlueprint* Blueprint = nullptr;
        if (!LoadOrphanBlueprint(Ctx, Blueprint))
        {
            return true;
        }

        const bool bIncludeDataOnly = GetJsonBoolField(Payload, TEXT("includeDataOnly"), true);

        const TArray<UEdGraph*> GraphsToScan = BlueprintHandlerUtils::CollectAllBlueprintGraphsRecursive(Blueprint);

        TArray<FString> GraphsScanned;
        int32 TotalNodes = 0;
        for (UEdGraph* Graph : GraphsToScan)
        {
            GraphsScanned.Add(Graph->GetName());
            TotalNodes += Graph->Nodes.Num();
        }

        const TArray<FBlueprintOrphanNodeInfo> Orphans =
            BlueprintHandlerUtils::FindBlueprintOrphanNodes(Blueprint, bIncludeDataOnly);

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("blueprintPath"), Blueprint->GetPathName());

        TArray<TSharedPtr<FJsonValue>> GraphsScannedArray;
        for (const FString& GName : GraphsScanned)
        {
            GraphsScannedArray.Add(MakeShared<FJsonValueString>(GName));
        }
        Resp->SetArrayField(TEXT("graphsScanned"), GraphsScannedArray);
        Resp->SetNumberField(TEXT("totalNodes"), TotalNodes);

        Resp->SetArrayField(TEXT("orphanedNodes"),
            BlueprintHandlerUtils::BuildOrphanNodeInfoJsonArray(Orphans));
        Resp->SetNumberField(TEXT("orphanedCount"), Orphans.Num());

        AddAssetVerification(Resp, Blueprint);
        Ctx.SendSuccess(Resp);
        return true;
    }

    // -----------------------------------------------------------------------
    // Single-graph path
    // -----------------------------------------------------------------------

    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveOrphanBlueprintAndGraph(Ctx, Blueprint, TargetGraph))
    {
        return true;
    }

    const bool bIncludeDataOnly = GetJsonBoolField(Payload, TEXT("includeDataOnly"), true);

    TArray<UEdGraph*> GraphsToScan;
    BlueprintHandlerUtils::CollectBlueprintOrphanGraphFamily(TargetGraph, GraphsToScan);
    int32 TotalNodes = 0;
    for (UEdGraph* Graph : GraphsToScan)
    {
        if (Graph)
        {
            TotalNodes += Graph->Nodes.Num();
        }
    }
    const TArray<FBlueprintOrphanNodeInfo> Orphans =
        BlueprintHandlerUtils::FindBlueprintOrphanNodes(Blueprint, bIncludeDataOnly, TargetGraph);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("blueprintPath"), Blueprint->GetPathName());
    Resp->SetStringField(TEXT("graphName"), TargetGraph->GetName());
    Resp->SetNumberField(TEXT("totalNodes"), TotalNodes);

    Resp->SetArrayField(TEXT("orphanedNodes"),
        BlueprintHandlerUtils::BuildOrphanNodeInfoJsonArray(Orphans));
    Resp->SetNumberField(TEXT("orphanedCount"), Orphans.Num());

    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}


// ---------------------------------------------------------------------------
// Handler: blueprint.graph.delete_orphaned_nodes (mutating)
// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("blueprint.graph.delete_orphaned_nodes", "blueprint.graph",
    "Delete orphaned nodes (unconnected, side-effect-free) from a blueprint graph.",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_OPT("graphName", "string", "Graph name (omit to scan all graphs)"),
        RPC_PARAM_DEF("includeDataOnly", "boolean", "Also delete transitive data-only orphan subgraphs", "true"),
        RPC_PARAM_DEF("includeNestedGraphs", "boolean", "Also delete orphans in nested child graphs for a named graph; defaults to false", "false")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString GraphName;
    Payload->TryGetStringField(TEXT("graphName"), GraphName);
    const bool bScanAllGraphs = GraphName.IsEmpty();

    if (bScanAllGraphs)
    {
        UBlueprint* Blueprint = nullptr;
        if (!LoadOrphanBlueprint(Ctx, Blueprint))
        {
            return true;
        }

        const bool bIncludeDataOnly = GetJsonBoolField(Payload, TEXT("includeDataOnly"), true);

        const TArray<UEdGraph*> GraphsToScan = BlueprintHandlerUtils::CollectAllBlueprintGraphsRecursive(Blueprint);

        TArray<FString> GraphsScanned;
        int32 TotalNodes = 0;
        for (UEdGraph* Graph : GraphsToScan)
        {
            GraphsScanned.Add(Graph->GetName());
            TotalNodes += Graph->Nodes.Num();
        }

        const TArray<FBlueprintOrphanNodeInfo> Orphans =
            BlueprintHandlerUtils::FindBlueprintOrphanNodes(Blueprint, bIncludeDataOnly);

        // Snapshot orphan node descriptors before delete invalidates the weak pointers.
        TArray<TSharedPtr<FJsonValue>> DeletedNodesJson =
            BlueprintHandlerUtils::BuildOrphanNodeInfoJsonArray(Orphans);

        FString IntegrityError;
        FBlueprintCompileDiagnostics Diagnostics;
        const int32 DeletedCount = DeleteOrphans(
            Blueprint, GraphsToScan, Orphans, IntegrityError, Diagnostics);
        if (!IntegrityError.IsEmpty())
        {
            Ctx.SendError(TEXT("COMPOSITE_BOUNDARY_BROKEN"), IntegrityError);
            return true;
        }

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("blueprintPath"), Blueprint->GetPathName());

        TArray<TSharedPtr<FJsonValue>> GraphsScannedArray;
        for (const FString& GName : GraphsScanned)
        {
            GraphsScannedArray.Add(MakeShared<FJsonValueString>(GName));
        }
        Resp->SetArrayField(TEXT("graphsScanned"), GraphsScannedArray);
        Resp->SetNumberField(TEXT("totalNodes"), TotalNodes);

        Resp->SetNumberField(TEXT("orphanedCount"), Orphans.Num());
        Resp->SetNumberField(TEXT("deletedCount"), DeletedCount);
        Resp->SetNumberField(TEXT("nestedGraphsSkipped"), 0);
        Resp->SetArrayField(TEXT("deletedNodes"), DeletedNodesJson);
        if (DeletedCount > 0)
        {
            AddCompileDiagnosticsToJson(Diagnostics, Resp);
        }

        AddAssetVerification(Resp, Blueprint);
        Ctx.SendSuccess(Resp);
        return true;
    }

    // -----------------------------------------------------------------------
    // Single-graph path
    // -----------------------------------------------------------------------

    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveOrphanBlueprintAndGraph(Ctx, Blueprint, TargetGraph))
    {
        return true;
    }

    const bool bIncludeDataOnly = GetJsonBoolField(Payload, TEXT("includeDataOnly"), true);
    const bool bIncludeNestedGraphs = GetJsonBoolField(Payload, TEXT("includeNestedGraphs"), false);

    TArray<UEdGraph*> GraphFamily;
    BlueprintHandlerUtils::CollectBlueprintOrphanGraphFamily(TargetGraph, GraphFamily);
    TArray<UEdGraph*> GraphsToModify;
    if (bIncludeNestedGraphs)
    {
        GraphsToModify = GraphFamily;
    }
    else
    {
        GraphsToModify.Add(TargetGraph);
    }
    const int32 NestedGraphsSkipped = bIncludeNestedGraphs
        ? 0
        : (GraphFamily.Num() > 0 ? GraphFamily.Num() - 1 : 0);
    int32 TotalNodes = 0;
    for (UEdGraph* Graph : GraphsToModify)
    {
        if (Graph)
        {
            TotalNodes += Graph->Nodes.Num();
        }
    }
    const TArray<FBlueprintOrphanNodeInfo> Orphans =
        BlueprintHandlerUtils::FindBlueprintOrphanNodes(
            Blueprint, bIncludeDataOnly, TargetGraph, bIncludeNestedGraphs);

    // Snapshot orphan node descriptors before delete invalidates the weak pointers.
    TArray<TSharedPtr<FJsonValue>> DeletedNodesJson =
        BlueprintHandlerUtils::BuildOrphanNodeInfoJsonArray(Orphans);

    FString IntegrityError;
    FBlueprintCompileDiagnostics Diagnostics;
    const int32 DeletedCount = DeleteOrphans(
        Blueprint, GraphsToModify, Orphans, IntegrityError, Diagnostics);
    if (!IntegrityError.IsEmpty())
    {
        Ctx.SendError(TEXT("COMPOSITE_BOUNDARY_BROKEN"), IntegrityError);
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("blueprintPath"), Blueprint->GetPathName());
    Resp->SetStringField(TEXT("graphName"), TargetGraph->GetName());
    Resp->SetNumberField(TEXT("totalNodes"), TotalNodes);
    Resp->SetNumberField(TEXT("nestedGraphsSkipped"), NestedGraphsSkipped);

    Resp->SetNumberField(TEXT("orphanedCount"), Orphans.Num());
    Resp->SetNumberField(TEXT("deletedCount"), DeletedCount);
    Resp->SetArrayField(TEXT("deletedNodes"), DeletedNodesJson);
    if (DeletedCount > 0)
    {
        AddCompileDiagnosticsToJson(Diagnostics, Resp);
    }

    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}
