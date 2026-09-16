// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Compat/EngineVersionCompat.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraGraphCreateNodePayload.h"
#include "Handlers/Niagara/NiagaraJsonHelpers.h"
#include "Handlers/Niagara/NiagaraOpCatalog.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"

#include "ScopedTransaction.h"
#include "NiagaraEditorCommon.h"
#include "NiagaraSystem.h"
#include "NiagaraParameterStore.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeOp.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeStaticSwitch.h"
#include "NiagaraNodeIf.h"
#include "NiagaraNodeReroute.h"
#include "NiagaraNodeConvert.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_Niagara.h"
#include "UObject/Package.h"

// Shared helper: Resolve target Niagara graph from asset path, emitter name, and script type
static UNiagaraGraph* ResolveNiagaraGraph(
    FHandlerContext& Ctx,
    UNiagaraSystem*& OutSystem,
    FString& OutGraphUsage,
    ENiagaraScriptUsage& OutScriptUsage,
    FGuid& OutScriptUsageId)
{
    OutGraphUsage.Empty();
    OutScriptUsage = ENiagaraScriptUsage::Function;
    OutScriptUsageId.Invalidate();

    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Missing 'assetPath'."));
        return nullptr;
    }

    OutSystem = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
    if (!OutSystem)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, TEXT("Could not load Niagara System."));
        return nullptr;
    }

    FString EmitterName = Ctx.GetString(TEXT("emitterName"));
    FString ScriptType = Ctx.GetString(TEXT("scriptType"));
    ScriptType.TrimStartAndEndInline();
    if (!ScriptType.IsEmpty()
        && !ScriptType.Equals(TEXT("Spawn"), ESearchCase::IgnoreCase)
        && !ScriptType.Equals(TEXT("Update"), ESearchCase::IgnoreCase))
    {
        Ctx.SendError(ErrorCodes::ERR_TARGET_NOT_FOUND,
            FString::Printf(
                TEXT("Could not resolve Niagara graph target script type '%s'. Valid candidates: Spawn, Update."),
                *ScriptType));
        return nullptr;
    }

    const bool bUseUpdateScript = ScriptType.Equals(TEXT("Update"), ESearchCase::IgnoreCase);
    UNiagaraScript* TargetScript = nullptr;

    if (EmitterName.IsEmpty())
    {
        TargetScript = OutSystem->GetSystemSpawnScript();
        if (bUseUpdateScript)
        {
            TargetScript = OutSystem->GetSystemUpdateScript();
        }
    }
    else
    {
        for (const FNiagaraEmitterHandle& Handle : OutSystem->GetEmitterHandles())
        {
            if (Handle.GetName() == FName(*EmitterName))
            {
                UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
                if (Emitter)
                {
                    const auto* EmitterData = Emitter->GetLatestEmitterData();
                    if (!EmitterData)
                    {
                        Ctx.SendError(ErrorCodes::ERR_EMITTER_DATA_MISSING, TEXT("Emitter data not available."));
                        return nullptr;
                    }
                    TargetScript = EmitterData->SpawnScriptProps.Script;
                    if (bUseUpdateScript)
                    {
                        TargetScript = EmitterData->UpdateScriptProps.Script;
                    }
                }
                break;
            }
        }
    }

    UNiagaraGraph* TargetGraph = NiagaraJsonHelpers::GetGraphFromScript(TargetScript);

    if (!TargetGraph)
    {
        Ctx.SendError(ErrorCodes::ERR_GRAPH_NOT_FOUND, TEXT("Could not resolve target Niagara Graph."));
        return nullptr;
    }

    OutGraphUsage = NiagaraEdit::StackScriptUsageToString(TargetScript->GetUsage());
    OutScriptUsage = TargetScript->GetUsage();
    OutScriptUsageId = TargetScript->GetUsageId();
    return TargetGraph;
}

// In-order upstream traversal from the output node that owns a script usage.
//
// UNiagaraGraph::BuildTraversal is unreachable from a plugin before UE 5.6: the class is
// UCLASS(MinimalAPI) and the NIAGARAEDITOR_API on this member (and on FindOutputNode) arrived in
// 5.6, so the call compiles and fails to link. On older engines this reproduces the engine's own
// walk with bEvaluateStaticSwitches=false - locate the output node for the usage, then visit
// every node feeding an input pin depth first, appending each node after its own inputs.
static void BuildNiagaraUsageTraversal(
    UNiagaraGraph* Graph,
    ENiagaraScriptUsage Usage,
    const FGuid& UsageId,
    TArray<UNiagaraNode*>& OutNodesTraversed)
{
    if (!Graph)
    {
        return;
    }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    Graph->BuildTraversal(OutNodesTraversed, Usage, UsageId);
#else
    UNiagaraNodeOutput* OutputNode = nullptr;
    for (UEdGraphNode* GraphNode : Graph->Nodes)
    {
        UNiagaraNodeOutput* Candidate = Cast<UNiagaraNodeOutput>(GraphNode);
        if (Candidate && Candidate->GetUsage() == Usage && Candidate->GetUsageId() == UsageId)
        {
            OutputNode = Candidate;
            break;
        }
    }
    if (!OutputNode)
    {
        return;
    }

    TFunction<void(UNiagaraNode*)> Visit;
    Visit = [&Visit, &OutNodesTraversed](UNiagaraNode* Node)
    {
        if (!Node)
        {
            return;
        }
        for (UEdGraphPin* Pin : Node->GetAllPins())
        {
            if (!Pin || Pin->Direction != EEdGraphPinDirection::EGPD_Input)
            {
                continue;
            }
            for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                UNiagaraNode* LinkedNode = LinkedPin
                    ? Cast<UNiagaraNode>(LinkedPin->GetOwningNode())
                    : nullptr;
                if (LinkedNode && !OutNodesTraversed.Contains(LinkedNode))
                {
                    Visit(LinkedNode);
                }
            }
        }
        OutNodesTraversed.Add(Node);
    };
    Visit(OutputNode);
#endif
}

static bool ValidateNiagaraGraphNodeUsageScope(
    UNiagaraGraph* TargetGraph,
    UEdGraphNode* TargetNode,
    ENiagaraScriptUsage RequestedUsage,
    const FGuid& RequestedUsageId,
    const FString& RequestedCanonicalUsage,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(TargetNode);
    if (!TargetGraph || !NiagaraNode)
    {
        return true;
    }

    TArray<UNiagaraNode*> RequestedTraversal;
    BuildNiagaraUsageTraversal(TargetGraph, RequestedUsage, RequestedUsageId, RequestedTraversal);
    if (RequestedTraversal.Contains(NiagaraNode))
    {
        return true;
    }

    TArray<FString> ReachableUsages;
    for (UEdGraphNode* GraphNode : TargetGraph->Nodes)
    {
        UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(GraphNode);
        if (!OutputNode)
        {
            continue;
        }

        TArray<UNiagaraNode*> Traversal;
        BuildNiagaraUsageTraversal(
            TargetGraph, OutputNode->GetUsage(), OutputNode->GetUsageId(), Traversal);
        if (Traversal.Contains(NiagaraNode))
        {
            ReachableUsages.AddUnique(NiagaraEdit::StackScriptUsageToString(OutputNode->GetUsage()));
        }
    }

    // A disconnected node belongs to no stage yet and remains available for graph authoring.
    if (ReachableUsages.IsEmpty())
    {
        return true;
    }

    ReachableUsages.Sort();
    OutErrorCode = ErrorCodes::ERR_TARGET_NOT_FOUND;
    OutErrorMessage = FString::Printf(
        TEXT("Niagara node '%s' is outside requested script usage '%s'. Reachable usages: %s."),
        *TargetNode->NodeGuid.ToString(),
        *RequestedCanonicalUsage,
        *FString::Join(ReachableUsages, TEXT(", ")));
    return false;
}

static bool ResolveNiagaraGraphNode(
    UNiagaraGraph* TargetGraph,
    const FString& RequestedNodeId,
    ENiagaraScriptUsage RequestedUsage,
    const FGuid& RequestedUsageId,
    const FString& RequestedCanonicalUsage,
    UEdGraphNode*& OutNode,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    OutNode = nullptr;
    OutErrorCode.Empty();
    OutErrorMessage.Empty();

    FGuid RequestedGuid;
    if (FGuid::Parse(RequestedNodeId, RequestedGuid))
    {
        for (UEdGraphNode* Node : TargetGraph->Nodes)
        {
            if (Node && Node->NodeGuid == RequestedGuid)
            {
                OutNode = Node;
                return ValidateNiagaraGraphNodeUsageScope(
                    TargetGraph, OutNode, RequestedUsage, RequestedUsageId,
                    RequestedCanonicalUsage, OutErrorCode, OutErrorMessage);
            }
        }

        // UE also accepts 22-character Base64 short GUID strings. If no node has
        // the parsed GUID, continue below so a GUID-looking name/title alias can
        // still resolve without weakening real GUID precedence.
    }

    TArray<UEdGraphNode*> AliasMatches;
    for (UEdGraphNode* Node : TargetGraph->Nodes)
    {
        if (Node
            && (Node->GetName() == RequestedNodeId
                || Node->GetNodeTitle(ENodeTitleType::ListView).ToString() == RequestedNodeId))
        {
            AliasMatches.AddUnique(Node);
        }
    }

    if (AliasMatches.Num() == 0)
    {
        OutErrorCode = ErrorCodes::ERR_NODE_NOT_FOUND;
        OutErrorMessage = FString::Printf(TEXT("Could not find Niagara node '%s'."), *RequestedNodeId);
        return false;
    }

    if (AliasMatches.Num() == 1)
    {
        OutNode = AliasMatches[0];
        return ValidateNiagaraGraphNodeUsageScope(
            TargetGraph, OutNode, RequestedUsage, RequestedUsageId,
            RequestedCanonicalUsage, OutErrorCode, OutErrorMessage);
    }

    FString CandidateGuids;
    for (int32 Index = 0; Index < AliasMatches.Num(); ++Index)
    {
        if (Index > 0)
        {
            CandidateGuids += TEXT(", ");
        }
        CandidateGuids += AliasMatches[Index]->NodeGuid.ToString();
    }

    OutErrorCode = ErrorCodes::ERR_AMBIGUOUS_NODE;
    OutErrorMessage = FString::Printf(
        TEXT("Node alias '%s' matches multiple Niagara nodes. Candidate GUIDs: %s."),
        *RequestedNodeId,
        *CandidateGuids);
    return false;
}

static FString NormalizeNiagaraScriptUsageFilter(const FString& ScriptUsage)
{
    FString Normalized = ScriptUsage;
    Normalized.RemoveFromEnd(TEXT("Script"), ESearchCase::IgnoreCase);
    return Normalized;
}

static bool MatchesNiagaraScriptUsage(const FString& ActualUsage, const FString& RequestedUsage)
{
    if (RequestedUsage.IsEmpty())
    {
        return true;
    }

    const FString ActualNormalized = NormalizeNiagaraScriptUsageFilter(ActualUsage);
    const FString RequestedNormalized = NormalizeNiagaraScriptUsageFilter(RequestedUsage);
    return ActualNormalized.Equals(RequestedNormalized, ESearchCase::IgnoreCase)
        || ActualNormalized.EndsWith(RequestedNormalized, ESearchCase::IgnoreCase);
}

static void AddMatchingNiagaraGraphs(
    const TSharedPtr<FJsonObject>& GraphsJson,
    const FString& EmitterFilter,
    const FString& ScriptUsageFilter,
    TArray<TSharedPtr<FJsonValue>>& OutGraphs)
{
    const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
    if (!GraphsJson.IsValid() || !GraphsJson->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
    {
        return;
    }

    for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
    {
        TSharedPtr<FJsonObject> Graph = GraphValue.IsValid() ? GraphValue->AsObject() : nullptr;
        if (!Graph.IsValid())
        {
            continue;
        }

        FString OwnerKind;
        FString OwnerName;
        FString ActualScriptUsage;
        Graph->TryGetStringField(TEXT("ownerKind"), OwnerKind);
        Graph->TryGetStringField(TEXT("ownerName"), OwnerName);
        Graph->TryGetStringField(TEXT("scriptUsage"), ActualScriptUsage);

        if (!EmitterFilter.IsEmpty())
        {
            const bool bEmitterGraph = OwnerKind.Equals(TEXT("emitter"), ESearchCase::IgnoreCase)
                || OwnerKind.Equals(TEXT("emitterAsset"), ESearchCase::IgnoreCase);
            if (!bEmitterGraph || !OwnerName.Equals(EmitterFilter, ESearchCase::IgnoreCase))
            {
                continue;
            }
        }

        if (!MatchesNiagaraScriptUsage(ActualScriptUsage, ScriptUsageFilter))
        {
            continue;
        }

        OutGraphs.Add(GraphValue);
    }
}

// ---- niagara.graph.get ----
REGISTER_RPC_HANDLER("niagara.graph.get", "niagara.graph", "Read Niagara graph metadata, nodes, pins, links, and function-call script references for a system, emitter, or script asset.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System, Niagara Emitter, or Niagara Script asset"),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("emitter"), TEXT("string"),
            TEXT("Emitter name when reading a graph from a Niagara System. The emitterName spelling the sibling niagara.graph.* verbs declare (and niagara.add_emitter returns) is also accepted."),
            /*bRequired=*/false, TArray<FString>({TEXT("emitter"), TEXT("emitterName")})),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("scriptUsage"), TEXT("string"),
            TEXT("Script usage such as SystemSpawn, ParticleUpdate, or Update. The scriptType spelling the sibling niagara.graph.* verbs declare is also accepted."),
            /*bRequired=*/false, TArray<FString>({TEXT("scriptUsage"), TEXT("scriptType")}))
    ))
{
    const FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Missing 'assetPath'."));
        return true;
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, TEXT("Could not load Niagara asset."));
        return true;
    }

    FString EmitterFilter = Ctx.GetString(TEXT("emitter"));
    if (EmitterFilter.IsEmpty())
    {
        EmitterFilter = Ctx.GetString(TEXT("emitterName"));
    }

    FString ScriptUsageFilter = Ctx.GetString(TEXT("scriptUsage"));
    if (ScriptUsageFilter.IsEmpty())
    {
        ScriptUsageFilter = Ctx.GetString(TEXT("scriptType"));
    }

    TSharedPtr<FJsonObject> SourceGraphs;
    FString AssetKind;
    if (const UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset))
    {
        SourceGraphs = NiagaraDumpBuilder::BuildGraphsJson(System);
        AssetKind = TEXT("NiagaraSystem");
    }
    else if (const UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset))
    {
        SourceGraphs = NiagaraDumpBuilder::BuildEmitterGraphsJson(Emitter);
        AssetKind = TEXT("NiagaraEmitter");
    }
    else if (const UNiagaraScript* Script = Cast<UNiagaraScript>(Asset))
    {
        SourceGraphs = NiagaraDumpBuilder::BuildScriptGraphsJson(Script);
        AssetKind = TEXT("NiagaraScript");
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET, TEXT("Asset is not a Niagara System, Niagara Emitter, or Niagara Script."));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Graphs;
    AddMatchingNiagaraGraphs(SourceGraphs, EmitterFilter, ScriptUsageFilter, Graphs);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("assetKind"), AssetKind);
    Result->SetStringField(TEXT("emitter"), EmitterFilter);
    Result->SetStringField(TEXT("scriptUsage"), ScriptUsageFilter);
    Result->SetNumberField(TEXT("count"), Graphs.Num());
    Result->SetArrayField(TEXT("graphs"), Graphs);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- niagara.graph.connect_pins ----
REGISTER_RPC_HANDLER("niagara.graph.connect_pins", "niagara.graph", "Wire two pins on Niagara script-graph nodes (UNiagaraNode). Pin types must be compatible; otherwise the connection is rejected.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System asset"),
        RPC_PARAM_OPT("emitterName", "string", "Name of emitter"),
        RPC_PARAM_OPT("scriptType", "string", "Script type: Spawn or Update"),
        RPC_PARAM_REQ("fromNode", "string", "Source node ID or name"),
        RPC_PARAM_REQ("fromPin", "string", "Source pin name"),
        RPC_PARAM_REQ("toNode", "string", "Destination node ID or name"),
        RPC_PARAM_REQ("toPin", "string", "Destination pin name")
    ))
{
    UNiagaraSystem* System = nullptr;
    FString GraphUsage;
    ENiagaraScriptUsage ScriptUsage;
    FGuid ScriptUsageId;
    UNiagaraGraph* TargetGraph = ResolveNiagaraGraph(
        Ctx, System, GraphUsage, ScriptUsage, ScriptUsageId);
    if (!TargetGraph) return true;

    FString FromNodeId = Ctx.GetString(TEXT("fromNode"));
    FString FromPinName = Ctx.GetString(TEXT("fromPin"));
    FString ToNodeId = Ctx.GetString(TEXT("toNode"));
    FString ToPinName = Ctx.GetString(TEXT("toPin"));

    if (FromNodeId.IsEmpty() || FromPinName.IsEmpty() || ToNodeId.IsEmpty() || ToPinName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("connect_pins requires fromNode, fromPin, toNode, toPin"));
        return true;
    }

    UEdGraphNode* FromNode = nullptr;
    UEdGraphNode* ToNode = nullptr;
    FString NodeErrorCode;
    FString NodeErrorMessage;
    if (!ResolveNiagaraGraphNode(
        TargetGraph, FromNodeId, ScriptUsage, ScriptUsageId, GraphUsage,
        FromNode, NodeErrorCode, NodeErrorMessage))
    {
        Ctx.SendError(NodeErrorCode, NodeErrorMessage);
        return true;
    }
    if (!ResolveNiagaraGraphNode(
        TargetGraph, ToNodeId, ScriptUsage, ScriptUsageId, GraphUsage,
        ToNode, NodeErrorCode, NodeErrorMessage))
    {
        Ctx.SendError(NodeErrorCode, NodeErrorMessage);
        return true;
    }

    UEdGraphPin* FromPin = FromNode->FindPin(FName(*FromPinName));
    UEdGraphPin* ToPin = ToNode->FindPin(FName(*ToPinName));

    if (!FromPin)
    {
        for (UEdGraphPin* Pin : FromNode->Pins)
        {
            if (Pin->PinName.ToString() == FromPinName || Pin->GetDisplayName().ToString() == FromPinName) { FromPin = Pin; break; }
        }
    }
    if (!ToPin)
    {
        for (UEdGraphPin* Pin : ToNode->Pins)
        {
            if (Pin->PinName.ToString() == ToPinName || Pin->GetDisplayName().ToString() == ToPinName) { ToPin = Pin; break; }
        }
    }

    if (!FromPin || !ToPin)
    {
        Ctx.SendError(ErrorCodes::ERR_PIN_NOT_FOUND, TEXT("Could not find source or destination pin."));
        return true;
    }

    const UEdGraphSchema* GraphSchema = TargetGraph->GetSchema();

    // Attempt the wire first. On the common success path this is the ONLY schema verdict
    // paid: UEdGraphSchema_Niagara::TryCreateConnection computes CanCreateConnection
    // internally, so pre-checking it here would just duplicate that traversal. Only when
    // the link fails to form do we re-query CanCreateConnection (below) to surface the
    // schema's OWN reason + named pins/types and to pick the error code.
    if (GraphSchema->TryCreateConnection(FromPin, ToPin))
    {
        // TryCreateConnection only reaches UNiagaraNode::PinConnectionListChanged, whose
        // NotifyGraphNeedsRecompile broadcasts OnGraphNeedsRecompile and returns WITHOUT
        // reaching UEdGraph::OnGraphChanged. An open editor therefore keeps
        // UNiagaraStackFunctionInput::OverridePinCache and SGraphPanel's connection splines
        // on the pre-edit wiring. The bare notification fires OnGraphChanged, which drives
        // SGraphPanel::PurgeVisualRepresentation and clears the cache.
        TargetGraph->NotifyGraphChanged();

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddAssetVerification(Result, System);
        Result->SetStringField(TEXT("scriptUsage"), GraphUsage);
        Result->SetStringField(TEXT("fromNode"), FromNode->NodeGuid.ToString());
        Result->SetStringField(TEXT("fromPin"), FromPinName);
        Result->SetStringField(TEXT("toNode"), ToNode->NodeGuid.ToString());
        Result->SetStringField(TEXT("toPin"), ToPinName);
        Result->SetBoolField(TEXT("connected"), true);
        Ctx.SendSuccess(Result);
        return true;
    }

    // The link did not form. A failed TryCreateConnection leaves the pins unmodified, so
    // this post-hoc verdict matches what the wire attempt saw. Mirrors the singular
    // niagara.connect_pin, which reports CanCreateConnection().Message on a rejected wire.
    const FPinConnectionResponse ConnectionResponse = GraphSchema->CanCreateConnection(FromPin, ToPin);
    if (ConnectionResponse.Response == CONNECT_RESPONSE_DISALLOW)
    {
        const FString FromType = UEdGraphSchema_Niagara::PinToTypeDefinition(FromPin).GetName();
        const FString ToType = UEdGraphSchema_Niagara::PinToTypeDefinition(ToPin).GetName();
        FString SchemaReason = ConnectionResponse.Message.ToString();
        if (SchemaReason.IsEmpty())
        {
            SchemaReason = TEXT("the Niagara graph schema blocked the connection");
        }
        FString Message = FString::Printf(
            TEXT("Schema rejected wiring '%s'.'%s' (%s) -> '%s'.'%s' (%s): %s"),
            *FromNodeId, *FromPinName, *FromType,
            *ToNodeId, *ToPinName, *ToType, *SchemaReason);
        if (NiagaraJsonHelpers::IsDynamicAddPin(ToPin) || NiagaraJsonHelpers::IsDynamicAddPin(FromPin))
        {
            Message += TEXT(" One of these pins is a Map Set '+' add pin (DynamicAddPin), which accepts a"
                " direct wire only from a concretely-typed output (the engine then materializes a"
                " named parameter entry and links the value into it); a generic-numeric source"
                " (e.g. an unresolved Multiply 'Result' whose type has not been inferred) is"
                " rejected until its type resolves. Give the source pin a concrete type, or wire"
                " through a node whose output type is known.");
        }
        Ctx.SendError(ErrorCodes::ERR_CONNECTION_DISALLOWED, Message);
        return true;
    }

    // CanCreateConnection permitted a MAKE-family response but the link still did not
    // form (rare internal failure). Name the pins so the caller isn't left guessing.
    Ctx.SendError(ErrorCodes::ERR_CONNECTION_FAILED,
        FString::Printf(
            TEXT("Failed to connect '%s'.'%s' -> '%s'.'%s' (the schema permitted the connection but it did not form)."),
            *FromNodeId, *FromPinName, *ToNodeId, *ToPinName));
    return true;
}

// ---- niagara.graph.remove_node ----
REGISTER_RPC_HANDLER("niagara.graph.remove_node", "niagara.graph", "Delete a node from a Niagara script graph by id, breaking any incident connections.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System asset"),
        RPC_PARAM_OPT("emitterName", "string", "Name of emitter"),
        RPC_PARAM_OPT("scriptType", "string", "Script type: Spawn or Update"),
        RPC_PARAM_REQ("nodeId", "string", "Node GUID to remove")
    ))
{
    UNiagaraSystem* System = nullptr;
    FString GraphUsage;
    ENiagaraScriptUsage ScriptUsage;
    FGuid ScriptUsageId;
    UNiagaraGraph* TargetGraph = ResolveNiagaraGraph(
        Ctx, System, GraphUsage, ScriptUsage, ScriptUsageId);
    if (!TargetGraph) return true;

    FString NodeId = Ctx.GetString(TEXT("nodeId"));
    FGuid RequestedNodeGuid;
    if (!FGuid::Parse(NodeId, RequestedNodeGuid))
    {
        Ctx.SendError(ErrorCodes::ERR_NODE_NOT_FOUND,
            FString::Printf(TEXT("Node id '%s' is not a valid GUID."), *NodeId));
        return true;
    }

    UEdGraphNode* TargetNode = nullptr;
    for (UEdGraphNode* Node : TargetGraph->Nodes)
    {
        if (Node && Node->NodeGuid == RequestedNodeGuid)
        {
            TargetNode = Node;
            break;
        }
    }

    if (TargetNode)
    {
        FString ScopeErrorCode;
        FString ScopeErrorMessage;
        if (!ValidateNiagaraGraphNodeUsageScope(
            TargetGraph, TargetNode, ScriptUsage, ScriptUsageId, GraphUsage,
            ScopeErrorCode, ScopeErrorMessage))
        {
            Ctx.SendError(ScopeErrorCode, ScopeErrorMessage);
            return true;
        }

        TargetGraph->RemoveNode(TargetNode);
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddAssetVerification(Result, System);
        Result->SetStringField(TEXT("scriptUsage"), GraphUsage);
        Result->SetStringField(TEXT("nodeId"), TargetNode->NodeGuid.ToString());
        Result->SetBoolField(TEXT("removed"), true);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_NODE_NOT_FOUND, TEXT("Node not found."));
    }
    return true;
}

// ---- NiagaraGraphCreate::ValidateCreateNodeClass / ApplyCreateNodePayload implementation ----

namespace NiagaraGraphCreate
{
    FNiagaraEditError ValidateCreateNodeClass(UClass* NodeClass)
    {
        if (!NodeClass)
        {
            return FNiagaraEditError::Make(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Node class is null."));
        }

        // UEdGraph::CreateNode routes through NewObject, which fatals on an abstract or deprecated
        // class instead of returning null. It has to be caught here: once FGraphNodeCreator exists
        // there is no recoverable answer, because ~FGraphNodeCreator demands a Finalize() that
        // dereferences the node CreateNode did not hand back.
        if (NodeClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
        {
            return FNiagaraEditError::Make(ErrorCodes::ERR_NODE_CREATE_FAILED,
                FString::Printf(TEXT("Node class '%s' cannot be instantiated."), *NodeClass->GetName()));
        }

        // Single source of truth for the v1 supported-class list; ApplyCreateNodePayload defers to
        // it rather than repeating the list.
        // UNiagaraNodeConvert has no NIAGARAEDITOR_API; resolve its UClass via reflection at
        // runtime to avoid an unresolved GetPrivateStaticClass link error.
        UClass* NiagaraNodeConvertClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeConvert"));
        const bool bSupported =
               NodeClass->IsChildOf(UNiagaraNodeOp::StaticClass())
            || NodeClass->IsChildOf(UNiagaraNodeInput::StaticClass())
            || NodeClass->IsChildOf(UNiagaraNodeOutput::StaticClass())
            || NodeClass->IsChildOf(UNiagaraNodeCustomHlsl::StaticClass())
            || NodeClass->IsChildOf(UNiagaraNodeStaticSwitch::StaticClass())
            || NodeClass->IsChildOf(UNiagaraNodeIf::StaticClass())
            || NodeClass->IsChildOf(UNiagaraNodeReroute::StaticClass())
            || (NiagaraNodeConvertClass && NodeClass->IsChildOf(NiagaraNodeConvertClass));

        if (!bSupported)
        {
            return FNiagaraEditError::Make(ErrorCodes::ERR_UNSUPPORTED_NODE_CLASS,
                FString::Printf(TEXT("Node class '%s' is not supported by niagara.graph.create_node v1."),
                    *NodeClass->GetName()));
        }

        return FNiagaraEditError{};
    }

    FNiagaraEditError ApplyCreateNodePayload(
        UNiagaraNode* Node,
        const TSharedPtr<FJsonObject>& Payload)
    {
        if (!Node)
        {
            return FNiagaraEditError::Make(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Node is null."));
        }

        UClass* NodeClass = Node->GetClass();

        // Op node
        if (UNiagaraNodeOp* OpNode = Cast<UNiagaraNodeOp>(Node))
        {
            FString PayloadOpName;
            if (Payload.IsValid() && Payload->TryGetStringField(TEXT("opName"), PayloadOpName) && !PayloadOpName.IsEmpty())
            {
                // FNiagaraOpInfo::GetOpInfo / GetOpInfoArray are not NIAGARAEDITOR_API
                // exported, and UNiagaraNodeOp::AllocateDefaultPins relies on the
                // un-exported registry too — calling it on a transient node yields zero
                // pins even for valid ops, so a pin-count heuristic is unreliable both
                // in tests (no owning graph) and in production for bare leaf names.
                //
                // The engine op registry is keyed on "Category::Leaf" (built by
                // FNiagaraOpInfo::BuildName in NiagaraEditorCommon.cpp), NOT on the bare
                // "Leaf". Validate the payload against the shared NiagaraOpCatalog
                // snapshot (the same source of truth search_ops reports from), accepting
                // either a bare leaf ("Mul") or a full key ("Numeric::Mul"), then
                // canonicalize to the full "Category::Leaf" key before storing. Storing
                // the bare leaf would resolve to nothing at pin allocation, producing a
                // pinless "Unknown" node (B-niagara-create-op-bare-leaf-pinless).
                FString CanonicalOpName;
                if (!NiagaraOpCatalog::CanonicalizeOpName(PayloadOpName, CanonicalOpName))
                {
                    return FNiagaraEditError::Make(ErrorCodes::ERR_INVALID_OP,
                        FString::Printf(TEXT("Unknown Niagara op '%s'."), *PayloadOpName));
                }

                OpNode->OpName = FName(*CanonicalOpName);
            }
            return FNiagaraEditError{};
        }

        // Input node
        if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node))
        {
            if (Payload.IsValid())
            {
                FString InputName;
                if (Payload->TryGetStringField(TEXT("inputName"), InputName) && !InputName.IsEmpty())
                {
                    InputNode->Input.SetName(FName(*InputName));
                }

                FString InputType;
                if (Payload->TryGetStringField(TEXT("inputType"), InputType) && !InputType.IsEmpty())
                {
                    // Leave type as default — full NiagaraTypeDefinition resolution is complex
                    // and not required for v1 create_node (type is set via pin wiring).
                }

                FString UsageStr;
                if (Payload->TryGetStringField(TEXT("usage"), UsageStr) && !UsageStr.IsEmpty())
                {
                    if (UsageStr.Equals(TEXT("Parameter"), ESearchCase::IgnoreCase))
                    {
                        InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
                    }
                    else if (UsageStr.Equals(TEXT("Attribute"), ESearchCase::IgnoreCase))
                    {
                        InputNode->Usage = ENiagaraInputNodeUsage::Attribute;
                    }
                    else if (UsageStr.Equals(TEXT("RapidIterationParameter"), ESearchCase::IgnoreCase))
                    {
                        InputNode->Usage = ENiagaraInputNodeUsage::RapidIterationParameter;
                    }
                }

                double SortPriority = 0.0;
                if (Payload->TryGetNumberField(TEXT("callSortPriority"), SortPriority))
                {
                    InputNode->CallSortPriority = static_cast<int32>(SortPriority);
                }
            }
            return FNiagaraEditError{};
        }

        // Output node — no payload fields in v1
        if (NodeClass->IsChildOf(UNiagaraNodeOutput::StaticClass()))
        {
            return FNiagaraEditError{};
        }

        // CustomHlsl — customHlsl text is applied post-AllocateDefaultPins by the caller
        if (NodeClass->IsChildOf(UNiagaraNodeCustomHlsl::StaticClass()))
        {
            return FNiagaraEditError{};
        }

        // StaticSwitch
        if (UNiagaraNodeStaticSwitch* SwitchNode = Cast<UNiagaraNodeStaticSwitch>(Node))
        {
            if (Payload.IsValid())
            {
                FString ParamName;
                if (Payload->TryGetStringField(TEXT("inputParameterName"), ParamName) && !ParamName.IsEmpty())
                {
                    SwitchNode->InputParameterName = FName(*ParamName);
                }

                FString SwitchTypeStr;
                if (Payload->TryGetStringField(TEXT("staticSwitchType"), SwitchTypeStr) && !SwitchTypeStr.IsEmpty())
                {
                    if (SwitchTypeStr.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
                    {
                        SwitchNode->SwitchTypeData.SwitchType = ENiagaraStaticSwitchType::Bool;
                    }
                    else if (SwitchTypeStr.Equals(TEXT("Integer"), ESearchCase::IgnoreCase))
                    {
                        SwitchNode->SwitchTypeData.SwitchType = ENiagaraStaticSwitchType::Integer;
                    }
                    else if (SwitchTypeStr.Equals(TEXT("Enum"), ESearchCase::IgnoreCase))
                    {
                        SwitchNode->SwitchTypeData.SwitchType = ENiagaraStaticSwitchType::Enum;

                        FString EnumPath;
                        if (Payload->TryGetStringField(TEXT("enumPath"), EnumPath) && !EnumPath.IsEmpty())
                        {
                            if (UEnum* Enum = LoadObject<UEnum>(nullptr, *EnumPath))
                            {
                                SwitchNode->SwitchTypeData.Enum = Enum;
                            }
                        }
                    }
                }
            }
            return FNiagaraEditError{};
        }

        // If, Reroute, Convert — no payload fields in v1, so the remaining question is only
        // whether the class is on the v1 list at all. ValidateCreateNodeClass owns that list and
        // the handler runs it before the node is constructed.
        return ValidateCreateNodeClass(NodeClass);
    }
}

// ---- niagara.graph.create_node ----
REGISTER_RPC_HANDLER("niagara.graph.create_node", "niagara.graph",
    "Create a new node in a Niagara script graph. Resolves the target graph via the target spec (kind=graph), "
    "constructs a node of the specified class, applies class-specific payload fields, and returns the new node id and its pins. "
    "v1 supported classes: NiagaraNodeOp, NiagaraNodeInput, NiagaraNodeOutput, NiagaraNodeCustomHlsl, "
    "NiagaraNodeStaticSwitch, NiagaraNodeIf, NiagaraNodeReroute, NiagaraNodeConvert.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath",  "path", "Path to the Niagara System or Emitter asset"),
        RPC_PARAM_OPT("target",     "object", "Target spec: {kind, emitter, scriptUsage}. Defaults to kind=graph."),
        RPC_PARAM_REQ("nodeClass",  "classref", "Short class name (e.g. NiagaraNodeOp) or full class path"),
        RPC_PARAM_REQ("x",          "number", "Horizontal position in the graph canvas"),
        RPC_PARAM_REQ("y",          "number", "Vertical position in the graph canvas"),
        RPC_PARAM_OPT("payload",    "object", "Node-class-specific fields (opName, customHlsl, inputParameterName, …)"),
        RPC_PARAM_OPT("compile",    "boolean", "Compile the system/emitter after creating the node"),
        RPC_PARAM_OPT("save",       "boolean", "Save the asset after creating the node")
    ))
{
    TSharedPtr<FJsonObject> RawPayload = Ctx.GetRawPayload();

    // --- Validate x / y ---
    double X = 0.0;
    double Y = 0.0;
    if (!RawPayload->TryGetNumberField(TEXT("x"), X) || !RawPayload->TryGetNumberField(TEXT("y"), Y))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("niagara.graph.create_node requires 'x' and 'y' coordinates."));
        return true;
    }

    // --- Resolve asset path ---
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Missing 'assetPath'."));
        return true;
    }

    // --- Build target spec (kind=graph) ---
    FNiagaraEditTargetSpec TargetSpec;
    TargetSpec.Kind = ENiagaraEditTargetKind::Graph;

    const TSharedPtr<FJsonObject>* TargetObjPtr = nullptr;
    if (RawPayload->TryGetObjectField(TEXT("target"), TargetObjPtr) && TargetObjPtr)
    {
        const TSharedPtr<FJsonObject>& TargetObj = *TargetObjPtr;
        FString KindStr;
        if (TargetObj->TryGetStringField(TEXT("kind"), KindStr) && !KindStr.IsEmpty())
        {
            // Only "graph" is supported by this handler; leave default.
        }
        TargetObj->TryGetStringField(TEXT("emitter"), TargetSpec.EmitterName);
        if (TargetSpec.EmitterName.IsEmpty())
        {
            TargetObj->TryGetStringField(TEXT("emitterName"), TargetSpec.EmitterName);
        }
        FString ScriptUsageStr;
        if (TargetObj->TryGetStringField(TEXT("scriptUsage"), ScriptUsageStr))
        {
            TargetSpec.ScriptUsage = ScriptUsageStr;
        }
    }

    // --- Resolve graph ---
    FNiagaraResolvedTarget ResolvedTarget;
    FNiagaraEditError ResolveErr = NiagaraEdit::ResolveTarget(AssetPath, TargetSpec, ResolvedTarget);
    if (ResolveErr.HasError())
    {
        Ctx.SendError(*ResolveErr.Code, ResolveErr.Message);
        return true;
    }

    UNiagaraGraph* Graph = ResolvedTarget.Graph;
    if (!Graph)
    {
        Ctx.SendError(ErrorCodes::ERR_GRAPH_NOT_FOUND, TEXT("Could not resolve a UNiagaraGraph for the given target."));
        return true;
    }

    // This verb mutates the graph directly instead of going through ModifyResolvedTarget or
    // BeginEmitterMutationScope, so it is the one path that would reach FinalizeNiagaraEdit with no
    // before-verdict - and without one, the live-system data-interface repair there cannot tell an
    // inherited mismatch from one this call created and so never runs.
    NiagaraEdit::RecordDataInterfaceVerdictBefore(ResolvedTarget);

    // --- Resolve node class ---
    FString NodeClassStr = Ctx.GetString(TEXT("nodeClass"));
    if (NodeClassStr.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Missing 'nodeClass'."));
        return true;
    }

    UClass* NodeClass = NiagaraEdit::ResolveNiagaraSubclassByPath(
        UNiagaraNode::StaticClass(), NodeClassStr, TEXT("NiagaraEditor"));
    if (!NodeClass)
    {
        Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND,
            FString::Printf(TEXT("Could not find a UNiagaraNode subclass for '%s'."), *NodeClassStr));
        return true;
    }

    // --- Reject an unsupported class before touching the graph ---
    // This gate used to be the last branch of ApplyCreateNodePayload, i.e. it ran after the node
    // had already been built by FGraphNodeCreator, and the rejection returned before
    // NodeCreator.Finalize(). ~FGraphNodeCreator's checkf(bPlaced) then fired an appError and
    // terminated the editor while the caller saw only a well-formed typed error.
    if (FNiagaraEditError ClassError = NiagaraGraphCreate::ValidateCreateNodeClass(NodeClass); ClassError.HasError())
    {
        Ctx.SendError(*ClassError.Code, ClassError.Message);
        return true;
    }

    // --- Obtain class-specific payload object ---
    TSharedPtr<FJsonObject> NodePayload;
    const TSharedPtr<FJsonObject>* NodePayloadPtr = nullptr;
    if (RawPayload->TryGetObjectField(TEXT("payload"), NodePayloadPtr) && NodePayloadPtr)
    {
        NodePayload = *NodePayloadPtr;
    }

    // --- Create the node ---
    // Dirty baseline for the refusal path below. Sampled after the graph was resolved — so dirt
    // a cold load's PostLoad produced belongs to the baseline and survives the restore — and
    // before Graph->Modify() dirties the package itself. Only a package that was already clean
    // is restored, so dirt from a concurrent editor edit is never cleared.
    UPackage* const GraphPackage = Graph->GetOutermost();
    const bool bGraphPackageWasDirty = GraphPackage && GraphPackage->IsDirty();

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.graph.create_node")));
    Graph->Modify();

    // UEdGraph::CreateNode is protected in UE 5.6; FGraphNodeCreator is the friended public entry point.
    //
    // The creator is scoped so that no statement between CreateNode() and Finalize() can leave the
    // block early: ~FGraphNodeCreator is an unconditional checkf(bPlaced), so a `return` in that
    // window is an editor-terminating appError rather than an error response. Payload application
    // has to stay inside the window — Finalize() allocates the default pins and those depend on the
    // applied fields — so its error is carried out in PayloadError and handled below, after the
    // node has been finalized.
    UEdGraphNode* NewEdNode = nullptr;
    UNiagaraNode* NiagaraNode = nullptr;
    FNiagaraEditError PayloadError;
    {
        FGraphNodeCreator<UEdGraphNode> NodeCreator(*Graph);
        NewEdNode = NodeCreator.CreateNode(/*bSelectNewNode=*/false, NodeClass);
        // UEdGraph::CreateNode routes through NewObject, which cannot return null for the concrete
        // UNiagaraNode subclasses ValidateCreateNodeClass admits, and a null could not be reported
        // from here anyway — Finalize() dereferences the node and skipping it aborts the process.
        NiagaraNode = CastChecked<UNiagaraNode>(NewEdNode);

        PayloadError = NiagaraGraphCreate::ApplyCreateNodePayload(NiagaraNode, NodePayload);

        NewEdNode->NodePosX = static_cast<int32>(X);
        NewEdNode->NodePosY = static_cast<int32>(Y);
        // Finalize assigns a new GUID, calls PostPlacedNewNode, and allocates default pins if the node didn't create any.
        NodeCreator.Finalize();
    }

    if (PayloadError.HasError())
    {
        // The creator is satisfied, so the node can be dropped normally: a rejected payload must
        // leave the graph exactly as it found it.
        Graph->RemoveNode(NewEdNode);
        // ...and the package too. Graph->Modify() and the node construction dirtied it before the
        // payload could be validated, so without this a refused create_node queues an asset the
        // caller never changed for the next save prompt (B-niagara-refused-edit-dirties-package).
        if (GraphPackage && !bGraphPackageWasDirty)
        {
            GraphPackage->SetDirtyFlag(false);
        }
        Ctx.SendError(*PayloadError.Code, PayloadError.Message);
        return true;
    }

    if (UNiagaraNodeCustomHlsl* HlslNode = Cast<UNiagaraNodeCustomHlsl>(NiagaraNode))
    {
        FString CustomHlsl;
        if (NodePayload.IsValid() && NodePayload->TryGetStringField(TEXT("customHlsl"), CustomHlsl))
        {
            // UNiagaraNodeCustomHlsl::SetCustomHlsl is declared public but is not
            // NIAGARAEDITOR_API in UE 5.6. Replicate it by writing the CustomHlsl
            // UPROPERTY (visible via UE reflection) and then reallocating pins so
            // the node's signature is rebuilt from the new HLSL source — same
            // observable effect as the private setter. ReallocatePins is
            // NIAGARAEDITOR_API exported but `protected` on UNiagaraNode, so we
            // access it through a `using`-shim derived class.
            struct FNiagaraNodeReallocatePinsAccessor : public UNiagaraNode
            {
                using UNiagaraNode::ReallocatePins;
            };
            static FStrProperty* CustomHlslProp = FindFProperty<FStrProperty>(
                UNiagaraNodeCustomHlsl::StaticClass(), TEXT("CustomHlsl"));
            if (CustomHlslProp)
            {
                CustomHlslProp->SetPropertyValue_InContainer(HlslNode, CustomHlsl);
                static_cast<FNiagaraNodeReallocatePinsAccessor*>(static_cast<UNiagaraNode*>(HlslNode))->ReallocatePins();
            }
        }
    }

    Graph->NotifyGraphChanged();

    // --- Build pin list (mirror add_module style) ---
    TArray<TSharedPtr<FJsonValue>> PinArray;
    for (UEdGraphPin* Pin : NewEdNode->Pins)
    {
        if (!Pin) continue;
        TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
        PinObj->SetStringField(TEXT("name"),      Pin->PinName.ToString());
        PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
        PinObj->SetStringField(TEXT("type"),      Pin->PinType.PinCategory.ToString());
        PinArray.Add(MakeShared<FJsonValueObject>(PinObj));
    }

    // --- Compile / save ---
    FNiagaraEditOptions Options;
    RawPayload->TryGetBoolField(TEXT("compile"), Options.bCompile);
    RawPayload->TryGetBoolField(TEXT("save"),    Options.bSave);

    bool bCompiled = false;
    bool bSaved    = false;
    NiagaraEdit::FinalizeNiagaraEdit(ResolvedTarget, Options, bCompiled, bSaved);

    // --- Response ---
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"),    NewEdNode->NodeGuid.ToString());
    Result->SetStringField(TEXT("nodeClass"), NodeClass->GetPathName());
    Result->SetArrayField(TEXT("pins"),       PinArray);
    Result->SetBoolField(TEXT("compiled"),    bCompiled);
    Result->SetBoolField(TEXT("saved"),       bSaved);
    Ctx.SendSuccess(Result);
    return true;
}
