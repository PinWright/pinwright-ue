// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirExpressionHandler.cpp - Create and update BPIR expression nodes in Blueprint graphs

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "K2Node_BpirExpression.h"
#include "Utils/AssetUtils.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"

using namespace BlueprintHandlerUtils;

// ---------------------------------------------------------------------------
// Helper: Build pin info JSON array from a BPIR expression node
// ---------------------------------------------------------------------------
static TArray<TSharedPtr<FJsonValue>> BuildPinInfoArray(const UK2Node_BpirExpression* Node)
{
    TArray<TSharedPtr<FJsonValue>> PinsArray;
    if (!Node)
    {
        return PinsArray;
    }

    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->bHidden)
        {
            continue;
        }

        TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
        PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
        PinObj->SetStringField(TEXT("direction"),
            Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
        PinObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
        PinsArray.Add(MakeShared<FJsonValueObject>(PinObj));
    }

    return PinsArray;
}

// ---------------------------------------------------------------------------
// Helper: Find graph by name or return first event graph
// ---------------------------------------------------------------------------
static UEdGraph* FindTargetGraph(UBlueprint* BP, const FString& GraphName)
{
    if (!GraphName.IsEmpty())
    {
        TArray<UEdGraph*> AllGraphs;
        BP->GetAllGraphs(AllGraphs);
        for (UEdGraph* Graph : AllGraphs)
        {
            if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
            {
                return Graph;
            }
        }
        return nullptr;
    }

    // Default: first UbergraphPage (event graph)
    if (BP->UbergraphPages.Num() > 0)
    {
        return BP->UbergraphPages[0];
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Helper: Find UK2Node_BpirExpression by GUID across all graphs
// ---------------------------------------------------------------------------
static UK2Node_BpirExpression* FindBpirExpressionByGuid(UBlueprint* BP, const FGuid& NodeGuid)
{
    TArray<UEdGraph*> AllGraphs;
    BP->GetAllGraphs(AllGraphs);
    for (UEdGraph* Graph : AllGraphs)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;
            if (Node->NodeGuid == NodeGuid)
            {
                return Cast<UK2Node_BpirExpression>(Node);
            }
        }
    }
    return nullptr;
}

// ---- blueprint.create_bpir_expression ----
REGISTER_RPC_HANDLER("blueprint.create_bpir_expression", "blueprint",
    "Create a BPIR expression node in a blueprint graph",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("bpirText", "string", "BPIR text (header + body)"),
        RPC_PARAM_OPT("graphName", "string", "Target graph name (default: first event graph)"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    FString AssetPath = ResolveBlueprintPath(Ctx);
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"),
            TEXT("blueprint.create_bpir_expression: a blueprint path is required (use 'path' or 'assetPath')."));
        return true;
    }
    FString BpirText;
    if (!Ctx.RequireString(TEXT("bpirText"), BpirText)) return true;

    FString GraphName = Ctx.GetString(TEXT("graphName"));
    double PosXD = 0.0, PosYD = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), PosXD)) return true;
    if (!Ctx.RequireNumber(TEXT("y"), PosYD)) return true;
    int32 PosX = static_cast<int32>(PosXD);
    int32 PosY = static_cast<int32>(PosYD);

    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(AssetPath, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            LoadErr.IsEmpty() ? TEXT("Failed to load Blueprint asset") : *LoadErr);
        return true;
    }

    UEdGraph* Graph = FindTargetGraph(BP, GraphName);
    if (!Graph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"),
            GraphName.IsEmpty()
                ? TEXT("Blueprint has no event graphs")
                : *FString::Printf(TEXT("Graph '%s' not found"), *GraphName));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.create_bpir_expression")));
    FGraphNodeCreator<UK2Node_BpirExpression> NodeCreator(*Graph);
    UK2Node_BpirExpression* NewNode = NodeCreator.CreateNode();
    NewNode->BpirText = BpirText;
    NewNode->NodePosX = PosX;
    NewNode->NodePosY = PosY;
    NodeCreator.Finalize();

    NewNode->RebuildFromBpir();

    // Collect errors from the node
    TArray<TSharedPtr<FJsonValue>> ErrorsArray;
    // Access CachedErrors via ValidateNodeDuringCompilation is not direct;
    // check for compiler message flag instead
    bool bHasErrors = NewNode->bHasCompilerMessage &&
                      NewNode->ErrorType == EMessageSeverity::Error;
    if (bHasErrors && !NewNode->ErrorMsg.IsEmpty())
    {
        ErrorsArray.Add(MakeShared<FJsonValueString>(NewNode->ErrorMsg));
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("nodeId"), NewNode->NodeGuid.ToString());
    Out->SetBoolField(TEXT("success"), !bHasErrors);
    Out->SetArrayField(TEXT("pins"), BuildPinInfoArray(NewNode));
    Out->SetArrayField(TEXT("errors"), ErrorsArray);

    Ctx.SendSuccess(Out);
    return true;
}

// ---- blueprint.update_bpir_expression ----
REGISTER_RPC_HANDLER("blueprint.update_bpir_expression", "blueprint",
    "Update the BPIR text of an existing expression node",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("nodeId", "string", "GUID of existing UK2Node_BpirExpression"),
        RPC_PARAM_REQ("bpirText", "string", "New BPIR text")
    ))
{
    FString AssetPath = ResolveBlueprintPath(Ctx);
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"),
            TEXT("blueprint.update_bpir_expression: a blueprint path is required (use 'path' or 'assetPath')."));
        return true;
    }
    FString NodeId, BpirText;
    if (!Ctx.RequireString(TEXT("nodeId"), NodeId)) return true;
    if (!Ctx.RequireString(TEXT("bpirText"), BpirText)) return true;

    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(AssetPath, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            LoadErr.IsEmpty() ? TEXT("Failed to load Blueprint asset") : *LoadErr);
        return true;
    }

    FGuid NodeGuid;
    if (!FGuid::Parse(NodeId, NodeGuid))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("'%s' is not a valid GUID"), *NodeId));
        return true;
    }

    UK2Node_BpirExpression* Node = FindBpirExpressionByGuid(BP, NodeGuid);
    if (!Node)
    {
        // Check if a node with that GUID exists but is the wrong type
        UEdGraphNode* GenericNode = FBlueprintEditorUtils::GetNodeByGUID(BP, NodeGuid);
        if (GenericNode)
        {
            Ctx.SendError(TEXT("INVALID_NODE_TYPE"),
                FString::Printf(TEXT("Node '%s' exists but is not a UK2Node_BpirExpression"), *NodeId));
        }
        else
        {
            Ctx.SendError(TEXT("NODE_NOT_FOUND"),
                FString::Printf(TEXT("No node found with GUID '%s'"), *NodeId));
        }
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.update_bpir_expression")));
    Node->BpirText = BpirText;
    Node->RebuildFromBpir();

    // Collect errors
    TArray<TSharedPtr<FJsonValue>> ErrorsArray;
    bool bHasErrors = Node->bHasCompilerMessage &&
                      Node->ErrorType == EMessageSeverity::Error;
    if (bHasErrors && !Node->ErrorMsg.IsEmpty())
    {
        ErrorsArray.Add(MakeShared<FJsonValueString>(Node->ErrorMsg));
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
    Out->SetBoolField(TEXT("success"), !bHasErrors);
    Out->SetArrayField(TEXT("pins"), BuildPinInfoArray(Node));
    Out->SetArrayField(TEXT("errors"), ErrorsArray);

    Ctx.SendSuccess(Out);
    return true;
}
