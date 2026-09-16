// Copyright (c) 2026 Alexander Penkin. MIT License.

// MetaSoundDestructiveHandler.cpp — inverse ops for the forward-only MetaSound authoring surface.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "PinWrightHelpers.h"

// MetaSound support (UE 5.0+)
#if __has_include("MetasoundSource.h")
#if __has_include("MetasoundDocumentInterface.h")
#include "MetasoundDocumentInterface.h"
#define MCP_HAS_METASOUND_DOCUMENT_INTERFACE 1
#else
#define MCP_HAS_METASOUND_DOCUMENT_INTERFACE 0
#endif
#include "MetasoundSource.h"
#define MCP_HAS_METASOUND 1
#else
#define MCP_HAS_METASOUND_DOCUMENT_INTERFACE 0
#define MCP_HAS_METASOUND 0
#endif

// MetaSound Frontend Document Builder (UE 5.3+)
#if __has_include("MetasoundFrontendDocumentBuilder.h")
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundFrontendDocument.h"
#define MCP_HAS_METASOUND_FRONTEND 1
#else
#define MCP_HAS_METASOUND_FRONTEND 0
#endif


// ---- audio.authoring.remove_metasound_node ----
REGISTER_RPC_HANDLER("audio.authoring.remove_metasound_node", "audio.authoring",
    "Remove a node from a MetaSound graph by GUID. Edges referencing the node are also removed. "
    "Use this to undo mistakes from add_metasound_node since MetaSound has no text-IR replacement path.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath",  "path",    "Asset path of the MetaSound"),
        RPC_PARAM_REQ("nodeId",     "string",  "GUID of the node to remove (from add_metasound_node or describe_metasound)"),
        RPC_PARAM_DEF("save",       "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_DOCUMENT_INTERFACE && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString NodeIdStr = Ctx.GetString(TEXT("nodeId"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PATH"), TEXT("Asset path is required"));
        return true;
    }

    FGuid NodeGuid;
    if (!FGuid::Parse(NodeIdStr, NodeGuid))
    {
        Ctx.SendError(TEXT("INVALID_GUID"), TEXT("Invalid nodeId format — must be a valid GUID"));
        return true;
    }

    // Accept both UMetaSoundSource and the sibling UMetaSoundPatch — the prior
    // Cast<UMetaSoundSource>(LoadedAsset) re-gate rejected a Patch with a false
    // ASSET_NOT_FOUND even though it loads. LoadMetaSoundDocumentObject is the
    // document-interface gate: a non-null return already implements the interface.
    UObject* MetaSound = PinWright::MetaSound::LoadMetaSoundDocumentObject(AssetPath);
    if (!MetaSound)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load MetaSound: %s"), *AssetPath));
        return true;
    }

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    MetaSound->Modify();
    bool bRemoved = Builder.RemoveNode(NodeGuid);
    if (!bRemoved)
    {
        PW_METASOUND_FINISH_BUILDING(Builder);
        Ctx.SendError(TEXT("NODE_NOT_FOUND"),
            FString::Printf(TEXT("No node with ID '%s' found in MetaSound graph"), *NodeIdStr));
        return true;
    }

    PW_METASOUND_FINISH_BUILDING(Builder);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"),
        FString::Printf(TEXT("Node '%s' removed from MetaSound graph"), *NodeIdStr));
    Result->SetNumberField(TEXT("edgesRemoved"), 0);
    PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
    AddAssetVerification(Result, MetaSound);
    Ctx.SendSuccess(Result);
    return true;
#elif MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    Ctx.SendError(TEXT("METASOUND_DOCUMENT_INTERFACE_NOT_SUPPORTED"),
        TEXT("Cannot remove MetaSound node — document interface not available"));
    return true;
#elif MCP_HAS_METASOUND
    Ctx.SendError(TEXT("METASOUND_FRONTEND_NOT_SUPPORTED"),
        TEXT("Cannot remove MetaSound node — Frontend Builder not available. Requires UE 5.3+"));
    return true;
#else
    Ctx.SendError(TEXT("METASOUND_NOT_AVAILABLE"), TEXT("MetaSound support not available"));
    return true;
#endif
}

// ---- audio.authoring.disconnect_metasound_nodes ----
REGISTER_RPC_HANDLER("audio.authoring.disconnect_metasound_nodes", "audio.authoring",
    "Disconnect an edge between two MetaSound nodes by source-output / target-input names.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath",        "path",    "Asset path of the MetaSound"),
        RPC_PARAM_REQ("sourceNodeId",     "string",  "GUID of source node"),
        RPC_PARAM_REQ("sourceOutputName", "string",  "Output pin name on source"),
        RPC_PARAM_REQ("targetNodeId",     "string",  "GUID of target node"),
        RPC_PARAM_REQ("targetInputName",  "string",  "Input pin name on target"),
        RPC_PARAM_DEF("save",             "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_DOCUMENT_INTERFACE && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString SourceNodeId, SourceOutputName, TargetNodeId, TargetInputName;
    if (!Ctx.RequireString(TEXT("sourceNodeId"),     SourceNodeId))     return true;
    if (!Ctx.RequireString(TEXT("sourceOutputName"), SourceOutputName)) return true;
    if (!Ctx.RequireString(TEXT("targetNodeId"),     TargetNodeId))     return true;
    if (!Ctx.RequireString(TEXT("targetInputName"),  TargetInputName))  return true;

    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PATH"), TEXT("Asset path is required"));
        return true;
    }

    FGuid SourceGuid, TargetGuid;
    if (!FGuid::Parse(SourceNodeId, SourceGuid) || !FGuid::Parse(TargetNodeId, TargetGuid))
    {
        Ctx.SendError(TEXT("INVALID_GUID"), TEXT("Invalid node ID format — must be valid GUIDs"));
        return true;
    }

    // Accept both UMetaSoundSource and the sibling UMetaSoundPatch — the prior
    // Cast<UMetaSoundSource>(LoadedAsset) re-gate rejected a Patch with a false
    // ASSET_NOT_FOUND even though it loads. LoadMetaSoundDocumentObject is the
    // document-interface gate: a non-null return already implements the interface.
    UObject* MetaSound = PinWright::MetaSound::LoadMetaSoundDocumentObject(AssetPath);
    if (!MetaSound)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load MetaSound: %s"), *AssetPath));
        return true;
    }

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    MetaSound->Modify();
    Metasound::Frontend::FNamedEdge NamedEdge{
        SourceGuid, FName(*SourceOutputName),
        TargetGuid, FName(*TargetInputName)
    };

    TSet<Metasound::Frontend::FNamedEdge> Edges;
    Edges.Add(NamedEdge);

    TArray<FMetasoundFrontendEdge> RemovedEdges;
    bool bDisconnected = Builder.RemoveNamedEdges(Edges, &RemovedEdges);

    if (!bDisconnected || RemovedEdges.Num() == 0)
    {
        PW_METASOUND_FINISH_BUILDING(Builder);
        Ctx.SendError(TEXT("EDGE_NOT_FOUND"),
            FString::Printf(
                TEXT("No MetaSound edge found for source node '%s' output '%s' to target node '%s' input '%s'"),
                *SourceNodeId,
                *SourceOutputName,
                *TargetNodeId,
                *TargetInputName));
        return true;
    }

    PW_METASOUND_FINISH_BUILDING(Builder);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"),
        TEXT("MetaSound edge disconnected"));
    Result->SetNumberField(TEXT("edgesRemoved"), RemovedEdges.Num());
    PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
    AddAssetVerification(Result, MetaSound);
    Ctx.SendSuccess(Result);
    return true;
#elif MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    Ctx.SendError(TEXT("METASOUND_DOCUMENT_INTERFACE_NOT_SUPPORTED"),
        TEXT("Cannot disconnect MetaSound nodes — document interface not available"));
    return true;
#elif MCP_HAS_METASOUND
    Ctx.SendError(TEXT("METASOUND_FRONTEND_NOT_SUPPORTED"),
        TEXT("Cannot disconnect MetaSound nodes — Frontend Builder not available. Requires UE 5.3+"));
    return true;
#else
    Ctx.SendError(TEXT("METASOUND_NOT_AVAILABLE"), TEXT("MetaSound support not available"));
    return true;
#endif
}
