// Copyright (c) 2026 Alexander Penkin. MIT License.

// MetaSoundNodeInputDefaultHandler.cpp
// Implements:
//   audio.authoring.set_metasound_node_input_default
//
// The graph-input family (set_metasound_default) writes a default onto a GRAPH input — the
// vertex exposed to whoever plays the MetaSound. This verb writes a default onto a NODE's
// input pin inside the graph, which is a different piece of state and the only way to bind a
// value to a pin that has no graph input wired to it. The decisive case is a Wave Player's
// "Wave Asset" pin: without this verb the wave can only be bound by adding a WaveAsset graph
// input and wiring it, which is not what a fixed per-stem player wants.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Audio/MetaSound/MetaSoundLiteralFromTypeName.h"
#include "Handlers/Audio/MetaSound/MetaSoundLiteralParams.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "PinWrightHelpers.h"
#include "Misc/EngineVersionComparison.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

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

// =========================================================================
// audio.authoring.set_metasound_node_input_default
// =========================================================================
REGISTER_RPC_HANDLER("audio.authoring.set_metasound_node_input_default", "audio.authoring",
    "Set the literal on a node's input pin inside a MetaSound graph (not a graph input). This is how a Wave Player's \"Wave Asset\" pin is bound to a USoundWave: pass the node's GUID from add_metasound_node, the pin name, and objectValue. Exactly one value param is required; it is validated against the pin's declared data type. An array-typed pin (e.g. an Array.Random Get \"In Array\" or its Float:Array \"Weights\") takes arrayValue instead of the scalar params.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath",   "path",    "Asset path of the MetaSound"),
        RPC_PARAM_REQ("nodeId",      "string",  "GUID of the node, as returned by add_metasound_node or reported by describe_metasound (nodes[].id)"),
        RPC_PARAM_REQ("inputName",   "string",  "Input pin name on that node, exactly as describe_metasound reports it (e.g. \"Wave Asset\", \"Loop Start\")"),
        RPC_PARAM_OPT("floatValue",  "number",  "Float pin value"),
        RPC_PARAM_OPT("intValue",    "integer", "Integer pin value"),
        RPC_PARAM_OPT("boolValue",   "boolean", "Boolean pin value"),
        RPC_PARAM_OPT("stringValue", "string",  "String pin value"),
        RPC_PARAM_OPT("objectValue", "path",    "Asset path of the object to bind, e.g. a USoundWave for a WaveAsset pin (alias: objectPath). NOT assetPath, which names the MetaSound being edited."),
        RPC_PARAM_OPT("arrayValue",  "array",   "Values for an ARRAY-typed target, as a JSON array whose entries match the element type: numbers for Float:Array / Int32:Array, booleans for Bool:Array, strings for String:Array, asset paths for object arrays such as WaveAsset:Array. This is the ONLY param an array-typed target accepts, and a scalar-typed target refuses it. An empty array clears the value. Required to reach the Array.* node family (Array.Random Get, Array.Shuffle, Array.Get/Set/Concat), whose pins and the Weights pin of Array.Random Get are all array-typed."),
        RPC_PARAM_DEF("save",        "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND && MCP_HAS_METASOUND_LITERAL_HELPER
    const FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString NodeId;
    if (!Ctx.RequireString(TEXT("nodeId"), NodeId)) return true;
    FString InputName;
    if (!Ctx.RequireString(TEXT("inputName"), InputName)) return true;

    const bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PATH, TEXT("Asset path is required"));
        return true;
    }

    FGuid NodeGuid;
    if (!FGuid::Parse(NodeId, NodeGuid))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_GUID,
            FString::Printf(TEXT("nodeId '%s' is not a GUID. Use the nodeId add_metasound_node returned, or a nodes[].id from describe_metasound."),
                *NodeId));
        return true;
    }

    // Load gate accepts Source or Patch — see LoadMetaSoundDocumentObject.
    UObject* MetaSound = PinWright::MetaSound::LoadMetaSoundDocumentObject(AssetPath);
    if (!MetaSound)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("Could not load MetaSound: %s"), *AssetPath));
        return true;
    }

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    // Distinguish the two opposite failures with distinct codes (rpc-design §7): a caller who
    // passed the wrong node acts differently from one who passed the wrong pin name.
    if (!Builder.FindNode(NodeGuid))
    {
        Ctx.SendError(ErrorCodes::ERR_NODE_NOT_FOUND,
            FString::Printf(TEXT("MetaSound '%s' has no node with id %s. List node ids with audio.authoring.describe_metasound (nodes[].id)."),
                *AssetPath, *NodeId));
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }

    const FMetasoundFrontendVertex* TargetVertex = Builder.FindNodeInput(NodeGuid, FName(*InputName));
    if (!TargetVertex)
    {
        // Name the pins that DO exist — the usual cause is a display-name guess
        // ("WaveAsset") for a pin the registry spells with a space ("Wave Asset").
        TSharedPtr<FJsonObject> PinErrorPayload = MakeShared<FJsonObject>();
        PinErrorPayload->SetStringField(TEXT("nodeId"), NodeId);
        PinErrorPayload->SetStringField(TEXT("inputName"), InputName);
        TArray<TSharedPtr<FJsonValue>> AvailableJson;
        for (const FMetasoundFrontendVertex* Vertex : Builder.FindNodeInputs(NodeGuid))
        {
            if (Vertex)
            {
                AvailableJson.Add(MakeShared<FJsonValueString>(Vertex->Name.ToString()));
            }
        }
        PinErrorPayload->SetArrayField(TEXT("availableInputs"), AvailableJson);
        Ctx.SendError(ErrorCodes::ERR_INPUT_NOT_FOUND,
            FString::Printf(TEXT("Node %s has no input pin named '%s'; nothing was written. See availableInputs for its %d pin name(s) — they carry spaces exactly as the registry declares them."),
                *NodeId, *InputName, AvailableJson.Num()),
            PinErrorPayload);
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }

    const FGuid VertexId = TargetVertex->VertexID;
    const FString PinTypeName = TargetVertex->TypeName.ToString();

    FMetasoundFrontendLiteral Literal;
    // No type default: a set verb called with no value is a caller mistake, not a request to
    // reset the pin. Nothing is written on either rejection path.
    const PinWright::MetaSound::EMetaSoundLiteralParamOutcome LiteralOutcome =
        PinWright::MetaSound::BuildMetaSoundLiteralFromParams(Ctx, PinTypeName,
            /*bAllowTypeDefault*/ false, Literal);
    if (LiteralOutcome == PinWright::MetaSound::EMetaSoundLiteralParamOutcome::ErrorSent)
    {
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }
    if (LiteralOutcome != PinWright::MetaSound::EMetaSoundLiteralParamOutcome::Ok)
    {
        PinWright::MetaSound::SendMetaSoundMissingValueError(Ctx,
            FString::Printf(TEXT("pin '%s' on node %s"), *InputName, *NodeId), PinTypeName);
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }

    MetaSound->Modify();
    const bool bSetAccepted = Builder.SetNodeInputDefault(NodeGuid, VertexId, Literal);

    // Commit the builder's caches before anything reads the document or saves it — see
    // audio.authoring.metasound_gotchas ("FinishBuilding() must be called before save").
    PW_METASOUND_FINISH_BUILDING(Builder);

    // Read the stored literal back off the DOCUMENT's node record rather than through the
    // builder that wrote it, so the reported value cannot be an echo of the setter's own
    // argument (rpc-design §4). This is the same array describe_metasound reads for
    // nodes[].inputs[].defaultLiteral, so a caller can re-verify through a second surface.
    FMetasoundFrontendLiteral StoredLiteral;
    bool bReadBack = false;
    if (const IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(MetaSound))
    {
        const FMetasoundFrontendDocument& Doc = PW_METASOUND_GET_CONST_DOCUMENT(DocInterface);
        const FMetasoundFrontendGraph& Graph = PinWright::MetaSound::GetGraphClassConstGraph(Doc.RootGraph);
        for (const FMetasoundFrontendNode& DocNode : Graph.Nodes)
        {
            if (DocNode.GetID() != NodeGuid)
            {
                continue;
            }
            for (const FMetasoundFrontendVertexLiteral& VertexLiteral : DocNode.InputLiterals)
            {
                if (VertexLiteral.VertexID == VertexId)
                {
                    StoredLiteral = VertexLiteral.Value;
                    bReadBack = true;
                    break;
                }
            }
            break;
        }
    }

    const bool bStoredMatches = bReadBack && StoredLiteral.IsEqual(Literal);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), NodeId);
    Result->SetStringField(TEXT("inputName"), InputName);
    Result->SetStringField(TEXT("inputType"), PinTypeName);
    Result->SetStringField(TEXT("vertexId"), VertexId.ToString());
    Result->SetBoolField(TEXT("readBack"), bReadBack);
    if (bReadBack)
    {
        TSharedPtr<FJsonObject> StoredJson = MakeShared<FJsonObject>();
        PinWright::MetaSound::DescribeMetaSoundLiteral(StoredLiteral, StoredJson);
        Result->SetObjectField(TEXT("storedDefault"), StoredJson);
    }

    if (!bSetAccepted || !bStoredMatches)
    {
        Result->SetBoolField(TEXT("setAccepted"), bSetAccepted);
        Ctx.SendError(ErrorCodes::ERR_SET_DEFAULT_FAILED,
            FString::Printf(TEXT("Literal for pin '%s' (type '%s') on node %s did not land: builder %s and the document read back %s. Check the value's type against the pin's."),
                *InputName, *PinTypeName, *NodeId,
                bSetAccepted ? TEXT("accepted the write") : TEXT("rejected the write"),
                bReadBack ? TEXT("a different literal") : TEXT("no literal at all")),
            Result);
        return true;
    }

    Result->SetStringField(TEXT("message"),
        FString::Printf(TEXT("Node %s input '%s' set"), *NodeId, *InputName));
    PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
    AddAssetVerification(Result, MetaSound);
    Ctx.SendSuccess(Result);
    return true;
#elif MCP_HAS_METASOUND
    Ctx.SendError(ErrorCodes::ERR_METASOUND_FRONTEND_NOT_SUPPORTED,
        TEXT("Cannot set a MetaSound node input default - Frontend Builder not available. Requires UE 5.3+"));
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_METASOUND_NOT_AVAILABLE, TEXT("MetaSound support not available"));
    return true;
#endif
}
