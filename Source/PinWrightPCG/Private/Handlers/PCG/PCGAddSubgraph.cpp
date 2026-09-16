// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "Dom/JsonObject.h"
#include "UObject/UObjectGlobals.h"

#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSubgraph.h"
#include "Handlers/PCG/PCGHandlerHelpers.h"

REGISTER_RPC_HANDLER("pcg.add_subgraph", "pcg",
    "Append a UPCGSubgraphSettings node referencing another UPCGGraph(Interface) asset. "
    "Rejects self-reference (RECURSIVE_SUBGRAPH) and non-graph assets (INVALID_SUBGRAPH_ASSET).",
    RPC_PARAMS(
        RPC_PARAM_REQ("graphPath", "path", "Outer PCG graph asset path (the graph to mutate)."),
        RPC_PARAM_REQ("x", "integer", "Node X position."),
        RPC_PARAM_REQ("y", "integer", "Node Y position."),
        RPC_PARAM_REQ("subgraphAsset", "path", "Path to the UPCGGraph(Interface) asset to embed.")
    ))
{
    FString GraphPath;
    if (!Ctx.RequireString(TEXT("graphPath"), GraphPath)) return true;

    int32 X = 0;
    if (!Ctx.RequireInt(TEXT("x"), X)) return true;
    int32 Y = 0;
    if (!Ctx.RequireInt(TEXT("y"), Y)) return true;

    FString SubgraphPath;
    if (!Ctx.RequireString(TEXT("subgraphAsset"), SubgraphPath)) return true;

    UPCGGraph* Graph = PinWrightPCG::LoadGraphOrError(Ctx, GraphPath);
    if (!Graph) return true;

    UObject* Loaded = LoadObject<UObject>(nullptr, *SubgraphPath);
    UPCGGraphInterface* SubgraphIface = Cast<UPCGGraphInterface>(Loaded);
    if (!SubgraphIface)
    {
        Ctx.SendError(TEXT("INVALID_SUBGRAPH_ASSET"),
            FString::Printf(TEXT("Asset is not a UPCGGraphInterface: %s"), *SubgraphPath));
        return true;
    }

    // Self-reference check: refuse to embed a graph into itself.
    if (Cast<UPCGGraph>(Loaded) == Graph)
    {
        Ctx.SendError(TEXT("RECURSIVE_SUBGRAPH"),
            FString::Printf(TEXT("Refusing to embed graph as its own subgraph: %s"), *SubgraphPath));
        return true;
    }

    UPCGSettings* DefaultSettings = nullptr;
    UPCGNode* Node = Graph->AddNodeOfType(
        TSubclassOf<UPCGSettings>(UPCGSubgraphSettings::StaticClass()),
        DefaultSettings);
    if (!Node)
    {
        Ctx.SendError(TEXT("ADD_NODE_FAILED"),
            TEXT("AddNodeOfType returned null for UPCGSubgraphSettings"));
        return true;
    }

    if (UPCGSubgraphSettings* Settings = Cast<UPCGSubgraphSettings>(DefaultSettings))
    {
        // SetSubgraph is the public method that wires the editor callbacks.
        Settings->SetSubgraph(SubgraphIface);
    }

    Node->SetNodePosition(X, Y);
    Graph->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), Node->GetName());
    Result->SetStringField(TEXT("subgraphPath"), SubgraphPath);
    Ctx.SendSuccess(Result);
    return true;
}

#endif // __has_include("PCGGraph.h")
