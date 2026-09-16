// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintGraphInspectionHandler.cpp - Blueprint graph inspection / search handlers.
// Split from BlueprintGraphHandler.cpp: read-only node/pin/graph queries, search, and
// execution-flow walking. Shared graph helpers live in BlueprintGraphHelpers.h.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintGraphHelpers.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_Tunnel.h"

using BlueprintGraphHelpers::ResolveBlueprintAndGraph;
using BlueprintGraphHelpers::FindNodeByIdOrName;
using BlueprintGraphHelpers::FindPinByName;
using BlueprintGraphHelpers::BuildPinJson;
using BlueprintGraphHelpers::BuildPinLookupPayload;

// ---------------------------------------------------------------------------
// Inspection-only node-state / node-details helpers
// ---------------------------------------------------------------------------

static bool TryGetNodeEnabledStateValue(UEdGraphNode* Node, int64& OutValue)
{
    if (!Node)
    {
        return false;
    }

    OutValue = static_cast<int64>(Node->GetDesiredEnabledState());
    return true;
}

static bool TryGetNodeCanDisable(UEdGraphNode* Node, bool& OutCanDisable)
{
    if (!Node)
    {
        return false;
    }

    // UE 5.6 does not expose a stable generic API for "can disable this node".
    // Use a practical signal: nodes with exec pins are disable-capable in BP UX.
    bool bHasExecPin = false;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            bHasExecPin = true;
            break;
        }
    }

    OutCanDisable = bHasExecPin;
    return true;
}

static FString DescribeNodeEnabledState(int64 StateValue)
{
    switch (StateValue)
    {
        case 0: return TEXT("enabled");
        case 1: return TEXT("disabled");
        case 2: return TEXT("development_only");
        default: return TEXT("unknown");
    }
}

static TSharedPtr<FJsonObject> BuildNodeStateJson(UEdGraphNode* Node)
{
    TSharedPtr<FJsonObject> NodeState = MakeShared<FJsonObject>();
    if (!Node)
    {
        NodeState->SetBoolField(TEXT("hasEnabledState"), false);
        NodeState->SetBoolField(TEXT("hasCanDisable"), false);
        return NodeState;
    }

    int64 EnabledStateValue = 0;
    const bool bHasEnabledState = TryGetNodeEnabledStateValue(Node, EnabledStateValue);
    NodeState->SetBoolField(TEXT("hasEnabledState"), bHasEnabledState);

    if (bHasEnabledState)
    {
        NodeState->SetNumberField(TEXT("enabledStateValue"), static_cast<double>(EnabledStateValue));
        NodeState->SetStringField(TEXT("enabledState"), DescribeNodeEnabledState(EnabledStateValue));
        NodeState->SetStringField(TEXT("desiredEnabledState"), DescribeNodeEnabledState(EnabledStateValue));
        NodeState->SetBoolField(TEXT("isEnabled"), Node->IsNodeEnabled());
        NodeState->SetBoolField(TEXT("hasUserSetEnabledState"), Node->HasUserSetTheEnabledState());
        NodeState->SetBoolField(TEXT("isDisabledByUser"),
            Node->HasUserSetTheEnabledState() && EnabledStateValue == 1);
    }

    bool bCanDisable = false;
    const bool bHasCanDisable = TryGetNodeCanDisable(Node, bCanDisable);
    NodeState->SetBoolField(TEXT("hasCanDisable"), bHasCanDisable);
    if (bHasCanDisable)
    {
        NodeState->SetBoolField(TEXT("canDisable"), bCanDisable);
    }

    return NodeState;
}

// Build node details JSON
static TSharedPtr<FJsonObject> BuildNodeDetailsJson(
    UEdGraphNode* Node,
    bool bIncludePinDefaults = true,
    bool bIncludeNodeState = true,
    const TSet<FString>* FieldFilter = nullptr)
{
    // Optional per-key allow-list. Null (the default, and every pre-existing call
    // site) emits every key, so the unprojected shape is byte-identical to before.
    // Non-null holds an already-lowercased key set (ReadFieldProjection lowercases
    // it) and only those keys are emitted; the probe literals below are lowercase to
    // match — no per-key ToLower, mirroring the get_nodes / ConsoleSearchHandler
    // Wants convention. The get_node_details_batch call site owns the why (the
    // inline-vs-disk narrowing rationale).
    const auto Wants = [FieldFilter](const TCHAR* Key)
    {
        return !FieldFilter || FieldFilter->Contains(FString(Key));
    };

    TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
    if (Wants(TEXT("nodeid")))
        NodeObj->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
    if (Wants(TEXT("nodename")))
        NodeObj->SetStringField(TEXT("nodeName"), Node->GetName());
    if (Wants(TEXT("nodetype")))
        NodeObj->SetStringField(TEXT("nodeType"), Node->GetClass()->GetName());
    if (Wants(TEXT("nodetitle")))
        NodeObj->SetStringField(TEXT("nodeTitle"),
            Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
    if (Wants(TEXT("nodecomment")))
        NodeObj->SetStringField(TEXT("nodeComment"), Node->NodeComment);
    if (Wants(TEXT("x")))
        NodeObj->SetNumberField(TEXT("x"), Node->NodePosX);
    if (Wants(TEXT("y")))
        NodeObj->SetNumberField(TEXT("y"), Node->NodePosY);

    if (Wants(TEXT("pins")))
    {
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin) continue;
            Pins.Add(MakeShared<FJsonValueObject>(BuildPinJson(Pin, false, bIncludePinDefaults)));
        }
        NodeObj->SetArrayField(TEXT("pins"), Pins);
    }
    if (bIncludeNodeState && Wants(TEXT("nodestate")))
    {
        NodeObj->SetObjectField(TEXT("nodeState"), BuildNodeStateJson(Node));
    }
    return NodeObj;
}

namespace
{
    enum class EEdgeTypeFilter : uint8
    {
        Any,
        Exec,
        Data
    };
}

static void BuildGraphConnectionsJson(
	UEdGraph* Graph,
	TArray<TSharedPtr<FJsonValue>>& OutConnections,
	const TSet<FString>* NodeIdFilter = nullptr,
	EEdgeTypeFilter EdgeTypeFilter = EEdgeTypeFilter::Any,
	int32 MaxEdges = 0,
	bool* bOutTruncated = nullptr,
	int32* OutTotalMatched = nullptr)
{
	OutConnections.Reset();
	if (bOutTruncated) *bOutTruncated = false;
	if (OutTotalMatched) *OutTotalMatched = 0;
	if (!Graph)
	{
		return;
	}

	TSet<FString> SeenEdges;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output) continue;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;

				const FString FromNodeId = Node->NodeGuid.ToString();
				const FString ToNodeId = LinkedPin->GetOwningNode()->NodeGuid.ToString();
				const FString FromPinName = Pin->PinName.ToString();
				const FString ToPinName = LinkedPin->PinName.ToString();
				const FString EdgeKey = FString::Printf(
					TEXT("%s|%s|%s|%s"),
					*FromNodeId,
					*FromPinName,
					*ToNodeId,
					*ToPinName);
				if (SeenEdges.Contains(EdgeKey)) continue;
				SeenEdges.Add(EdgeKey);

				const bool bIsExecEdge =
					Pin->PinType.PinCategory == TEXT("exec") ||
					LinkedPin->PinType.PinCategory == TEXT("exec");

				// Apply node ID filter
				if (NodeIdFilter && !NodeIdFilter->Contains(FromNodeId) && !NodeIdFilter->Contains(ToNodeId))
					continue;

				// Apply edge type filter
				if (EdgeTypeFilter == EEdgeTypeFilter::Exec && !bIsExecEdge) continue;
				if (EdgeTypeFilter == EEdgeTypeFilter::Data && bIsExecEdge) continue;

				if (OutTotalMatched) ++(*OutTotalMatched);

				// Apply max edges cap — keep counting totalMatched but skip push
				if (MaxEdges > 0 && OutConnections.Num() >= MaxEdges)
				{
					if (bOutTruncated) *bOutTruncated = true;
					continue;
				}

				TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
				EdgeObj->SetStringField(TEXT("fromNodeId"), FromNodeId);
				EdgeObj->SetStringField(TEXT("fromPin"), FromPinName);
				EdgeObj->SetStringField(TEXT("toNodeId"), ToNodeId);
				EdgeObj->SetStringField(TEXT("toPin"), ToPinName);
				EdgeObj->SetStringField(TEXT("edgeType"), bIsExecEdge ? TEXT("exec") : TEXT("data"));
				OutConnections.Add(MakeShared<FJsonValueObject>(EdgeObj));
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Inspection-only search-mode machinery (find_nodes)
// ---------------------------------------------------------------------------

enum class EMcpSearchMatchMode : uint8
{
    Contains,
    Prefix,
    Exact,
    Word
};

static EMcpSearchMatchMode ParseSearchMatchMode(const FString& MatchModeRaw)
{
    const FString MatchMode = MatchModeRaw.TrimStartAndEnd().ToLower();
    if (MatchMode == TEXT("exact"))
    {
        return EMcpSearchMatchMode::Exact;
    }
    if (MatchMode == TEXT("prefix") || MatchMode == TEXT("starts_with"))
    {
        return EMcpSearchMatchMode::Prefix;
    }
    if (MatchMode == TEXT("word") || MatchMode == TEXT("whole_word"))
    {
        return EMcpSearchMatchMode::Word;
    }
    return EMcpSearchMatchMode::Contains;
}

static FString SearchMatchModeToString(const EMcpSearchMatchMode MatchMode)
{
    switch (MatchMode)
    {
        case EMcpSearchMatchMode::Exact: return TEXT("exact");
        case EMcpSearchMatchMode::Prefix: return TEXT("prefix");
        case EMcpSearchMatchMode::Word: return TEXT("word");
        case EMcpSearchMatchMode::Contains:
        default:
            return TEXT("contains");
    }
}

static bool IsWordBoundaryChar(const TCHAR Ch)
{
    return !FChar::IsAlnum(Ch) && Ch != TEXT('_');
}

static bool ContainsWholeWord(const FString& FieldValue, const FString& Term)
{
    if (Term.IsEmpty())
    {
        return true;
    }

    int32 SearchFrom = 0;
    while (SearchFrom < FieldValue.Len())
    {
        const int32 FoundAt = FieldValue.Find(Term, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
        if (FoundAt == INDEX_NONE)
        {
            return false;
        }

        const int32 EndExclusive = FoundAt + Term.Len();
        const bool bLeftBoundary = (FoundAt == 0) || IsWordBoundaryChar(FieldValue[FoundAt - 1]);
        const bool bRightBoundary = (EndExclusive >= FieldValue.Len()) ||
            IsWordBoundaryChar(FieldValue[EndExclusive]);

        if (bLeftBoundary && bRightBoundary)
        {
            return true;
        }

        SearchFrom = FoundAt + 1;
    }

    return false;
}

static bool SearchFieldMatchesTerm(
    const FString& FieldValue,
    const FString& Term,
    const EMcpSearchMatchMode MatchMode)
{
    if (Term.IsEmpty())
    {
        return true;
    }

    switch (MatchMode)
    {
        case EMcpSearchMatchMode::Exact:
            return FieldValue.Equals(Term, ESearchCase::CaseSensitive);
        case EMcpSearchMatchMode::Prefix:
            return FieldValue.StartsWith(Term, ESearchCase::CaseSensitive);
        case EMcpSearchMatchMode::Word:
            return ContainsWholeWord(FieldValue, Term);
        case EMcpSearchMatchMode::Contains:
        default:
            return FieldValue.Contains(Term, ESearchCase::CaseSensitive);
    }
}

static bool IsNodeSearchField(const FString& FieldName)
{
    return FieldName == TEXT("nodeName") ||
        FieldName == TEXT("nodeType") ||
        FieldName == TEXT("nodeTitle") ||
        FieldName == TEXT("nodeFunction") ||
        FieldName == TEXT("nodeComment");
}

static bool IsKnownSearchFieldName(const FString& FieldName)
{
    return IsNodeSearchField(FieldName) ||
        FieldName == TEXT("pinName") ||
        FieldName == TEXT("pinType") ||
        FieldName == TEXT("pinSubTypeObjectPath") ||
        FieldName == TEXT("pinSubTypeObjectName") ||
        FieldName == TEXT("pinDefaultValue") ||
        FieldName == TEXT("pinDefaultTextValue") ||
        FieldName == TEXT("pinDefaultObjectPath");
}

static bool NormalizeKnownSearchField(const FString& InFieldName, FString& OutCanonicalField)
{
    const FString InLower = InFieldName.ToLower();
    if (InLower == TEXT("nodename")) { OutCanonicalField = TEXT("nodeName"); return true; }
    if (InLower == TEXT("nodetype")) { OutCanonicalField = TEXT("nodeType"); return true; }
    if (InLower == TEXT("nodetitle")) { OutCanonicalField = TEXT("nodeTitle"); return true; }
    if (InLower == TEXT("nodefunction")) { OutCanonicalField = TEXT("nodeFunction"); return true; }
    if (InLower == TEXT("nodecomment")) { OutCanonicalField = TEXT("nodeComment"); return true; }
    if (InLower == TEXT("pinname")) { OutCanonicalField = TEXT("pinName"); return true; }
    if (InLower == TEXT("pintype")) { OutCanonicalField = TEXT("pinType"); return true; }
    if (InLower == TEXT("pinsubtypeobjectpath")) { OutCanonicalField = TEXT("pinSubTypeObjectPath"); return true; }
    if (InLower == TEXT("pinsubtypeobjectname")) { OutCanonicalField = TEXT("pinSubTypeObjectName"); return true; }
    if (InLower == TEXT("pindefaultvalue")) { OutCanonicalField = TEXT("pinDefaultValue"); return true; }
    if (InLower == TEXT("pindefaulttextvalue")) { OutCanonicalField = TEXT("pinDefaultTextValue"); return true; }
    if (InLower == TEXT("pindefaultobjectpath")) { OutCanonicalField = TEXT("pinDefaultObjectPath"); return true; }
    OutCanonicalField.Reset();
    return false;
}

// ---- blueprint.graph.get_nodes ----
REGISTER_RPC_HANDLER("blueprint.graph.get_nodes", "blueprint.graph",
    "Get all nodes in a blueprint graph",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_OPT("graphName", "string", "Graph name (defaults to EventGraph)"),
        RPC_PARAM_OPT("includePinDefaults", "boolean", "Include pin default/default object values"),
        RPC_PARAM_OPT("includeNodeState", "boolean", "Include structured node enabled/disabled state"),
        RPC_PARAM_OPT_ALIAS("fields", "array|string", "Case-insensitive allow-list of per-node keys to return (valid keys: nodeId, nodeName, nodeType, nodeTitle, comment, x, y, pins). The heavy 'pins' adjacency is the dominant payload; drop it to keep an enumerate-to-pick call inline. e.g. [\"nodeId\",\"nodeTitle\"]. Omit for the full shape. A single string is also accepted. Full pin adjacency stays available via get_graph_connections / get_node_details(_batch).", "field"),
        RPC_PARAM_OPT_ALIAS("namesOnly", "boolean", "When true, returns only nodeId/nodeName/nodeType/nodeTitle/x/y per node and omits the heavy pins/linkedTo arrays — shorthand for the common 'just list the nodes so I can pick one' read. Snake_case names_only accepted. Ignored when fields is supplied.", "names_only"),
        RPC_PARAM_DEF("limit", "number", "Max nodes to return. 0 (default) = all. nodeCount reports the rows returned; totalNodeCount always reports the full untruncated node count and truncated flags whether the list was capped.", "0")
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    const bool bIncludePinDefaults = Ctx.GetBool(TEXT("includePinDefaults"), false);
    const bool bIncludeNodeState = Ctx.GetBool(TEXT("includeNodeState"), false);

    // Per-node field projection: an explicit `fields` allow-list (array or bare
    // string) wins; otherwise namesOnly expands to the light identification set
    // (everything except the heavy pins/linkedTo adjacency). An empty set means
    // "no projection" — every key is emitted, so the unprojected output is
    // byte-identical to the prior shape. ReadFieldProjection returns lowercased
    // keys, so the probe keys below are lowercase too.
    const TSet<FString> Fields = Ctx.ReadFieldProjection(
        {TEXT("nodeId"), TEXT("nodeName"), TEXT("nodeType"), TEXT("nodeTitle"), TEXT("x"), TEXT("y")});
    const bool bProject = Fields.Num() > 0;
    const auto Wants = [&Fields, bProject](const TCHAR* Key)
    {
        return !bProject || Fields.Contains(FString(Key));
    };
    const bool bWantNodeId = Wants(TEXT("nodeid"));
    const bool bWantNodeName = Wants(TEXT("nodename"));
    const bool bWantNodeType = Wants(TEXT("nodetype"));
    const bool bWantNodeTitle = Wants(TEXT("nodetitle"));
    const bool bWantComment = Wants(TEXT("comment"));
    const bool bWantX = Wants(TEXT("x"));
    const bool bWantY = Wants(TEXT("y"));
    const bool bWantPins = Wants(TEXT("pins"));

    // limit caps the returned array; 0 = all. totalNodeCount below always reports the
    // full node count regardless of the cap so elision is detectable.
    const int32 Limit = Ctx.GetInt(TEXT("limit"), 0);

    TArray<TSharedPtr<FJsonValue>> NodesArray;
    int32 TotalCount = 0;
    for (UEdGraphNode* Node : TargetGraph->Nodes)
    {
        if (!Node) continue;
        ++TotalCount;
        if (Limit > 0 && NodesArray.Num() >= Limit) continue; // keep counting, stop appending

        TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
        if (bWantNodeId) NodeObj->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
        if (bWantNodeName) NodeObj->SetStringField(TEXT("nodeName"), Node->GetName());
        if (bWantNodeType) NodeObj->SetStringField(TEXT("nodeType"), Node->GetClass()->GetName());
        if (bWantNodeTitle) NodeObj->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        if (bWantComment) NodeObj->SetStringField(TEXT("comment"), Node->NodeComment);
        if (bWantX) NodeObj->SetNumberField(TEXT("x"), Node->NodePosX);
        if (bWantY) NodeObj->SetNumberField(TEXT("y"), Node->NodePosY);

        if (bWantPins)
        {
            TArray<TSharedPtr<FJsonValue>> PinsArray;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin) continue;
                TSharedPtr<FJsonObject> PinObj = BuildPinJson(Pin, false, bIncludePinDefaults);
                TArray<TSharedPtr<FJsonValue>> LinkedToArray;
                for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    if (LinkedPin && LinkedPin->GetOwningNode())
                    {
                        TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
                        LinkObj->SetStringField(TEXT("nodeId"), LinkedPin->GetOwningNode()->NodeGuid.ToString());
                        LinkObj->SetStringField(TEXT("pinName"), LinkedPin->PinName.ToString());
                        LinkedToArray.Add(MakeShared<FJsonValueObject>(LinkObj));
                    }
                }
                PinObj->SetArrayField(TEXT("linkedTo"), LinkedToArray);
                PinsArray.Add(MakeShared<FJsonValueObject>(PinObj));
            }
            NodeObj->SetArrayField(TEXT("pins"), PinsArray);
        }
        if (bIncludeNodeState)
        {
            NodeObj->SetObjectField(TEXT("nodeState"), BuildNodeStateJson(Node));
        }
        NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("nodes"), NodesArray);
    Result->SetStringField(TEXT("graphName"), TargetGraph->GetName());
    // nodeCount = rows actually returned; totalNodeCount = full node count before any
    // limit cap; truncated flags whether the returned array was shortened by limit. This
    // mirrors the get_execution_flow count triplet so the blueprint.graph family stays
    // uniform. Additive top-level fields — the per-node shape and prior fields are
    // unchanged when no projection/limit is supplied.
    Result->SetNumberField(TEXT("nodeCount"), NodesArray.Num());
    Result->SetNumberField(TEXT("totalNodeCount"), TotalCount);
    Result->SetBoolField(TEXT("truncated"), NodesArray.Num() < TotalCount);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.graph.get_node_details ----
REGISTER_RPC_HANDLER("blueprint.graph.get_node_details", "blueprint.graph",
    "Get detailed information about a specific node",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("nodeId", "string", "Node ID or name"),
        RPC_PARAM_OPT("graphName", "string", "Graph name"),
        RPC_PARAM_OPT("includePinDefaults", "boolean", "Include pin default/default object values (default true)")
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    FString NodeId = Ctx.GetString(TEXT("nodeId"));
    UEdGraphNode* TargetNode = FindNodeByIdOrName(TargetGraph, NodeId);
    if (!TargetNode) { Ctx.SendError(TEXT("NODE_NOT_FOUND"), TEXT("Node not found.")); return true; }

    const bool bIncludePinDefaults = Ctx.GetBool(TEXT("includePinDefaults"), true);
    TSharedPtr<FJsonObject> Result = BuildNodeDetailsJson(TargetNode, bIncludePinDefaults, true);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.graph.get_graph_details ----
REGISTER_RPC_HANDLER("blueprint.graph.get_graph_details", "blueprint.graph",
    "Get details about a specific graph including its nodes",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_OPT("graphName", "string", "Graph name"),
        RPC_PARAM_OPT("includeNodeDetails", "boolean", "When false (the default) each node is the light {nodeId, nodeName, nodeTitle} inventory next to the top-level nodeCount — the right call for 'count nodes / list entry nodes'. When true each node switches to the full pins+adjacency payload (BuildNodeDetailsJson), the heavy shape that can exceed the inline budget and spill to disk."),
        RPC_PARAM_OPT("includePinDefaults", "boolean", "Include pin default/default object values when includeNodeDetails is true"),
        RPC_PARAM_OPT("includeNodeState", "boolean", "Include structured node enabled/disabled state"),
		RPC_PARAM_OPT("includeConnections", "boolean", "Include graph connection/edge payload"),
        RPC_PARAM_OPT_ALIAS("fields", "array|string", "Case-insensitive allow-list of per-node keys to return (valid keys: nodeId, nodeName, nodeType, nodeTitle, nodeComment, x, y, pins, nodeState). Projects each node to just these keys and takes precedence over includeNodeDetails — drop the heavy 'pins' to keep a node inventory inline. e.g. [\"nodeId\",\"nodeTitle\"]. A single string is also accepted. Omit for the default light inventory (or the full shape when includeNodeDetails is true).", "field"),
        RPC_PARAM_OPT_ALIAS("namesOnly", "boolean", "When true, returns only nodeId/nodeName/nodeType/nodeTitle/x/y per node and omits the heavy pins/nodeState — shorthand for 'just list the nodes'. Snake_case names_only accepted. Takes precedence over includeNodeDetails; ignored when fields is supplied.", "names_only")
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    const bool bIncludeNodeDetails = Ctx.GetBool(TEXT("includeNodeDetails"), false);
    const bool bIncludePinDefaults = Ctx.GetBool(TEXT("includePinDefaults"), false);
    const bool bIncludeNodeState = Ctx.GetBool(TEXT("includeNodeState"), false);
    const bool bIncludeConnections = Ctx.GetBool(TEXT("includeConnections"), false);

    // Per-node field projection, mirroring blueprint.graph.get_nodes / get_node_details_batch:
    // an explicit `fields` allow-list (array or bare string) wins; otherwise `namesOnly` expands
    // to the light identification set (nodeId/nodeName/nodeType/nodeTitle/x/y, dropping the heavy
    // pins/nodeState). A non-empty projection is applied through the shared BuildNodeDetailsJson
    // field filter and TAKES PRECEDENCE over includeNodeDetails, so a caller who overflowed with
    // includeNodeDetails:true can add namesOnly to the same args to dial back to an inline shape.
    // An empty set means "no projection", so the default light {nodeId,nodeName,nodeTitle}
    // inventory and the includeNodeDetails:true full shape are both byte-identical to before.
    const TSet<FString> Fields = Ctx.ReadFieldProjection(
        {TEXT("nodeId"), TEXT("nodeName"), TEXT("nodeType"), TEXT("nodeTitle"), TEXT("x"), TEXT("y")});
    const TSet<FString>* FieldFilter = Fields.Num() > 0 ? &Fields : nullptr;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("graphName"), TargetGraph->GetName());
    Result->SetNumberField(TEXT("nodeCount"), TargetGraph->Nodes.Num());

    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (UEdGraphNode* Node : TargetGraph->Nodes)
    {
        if (!Node) continue;
        TSharedPtr<FJsonObject> NodeObj;
        if (FieldFilter || bIncludeNodeDetails)
        {
            // A supplied projection (FieldFilter) applies regardless of includeNodeDetails and
            // wins; with no projection FieldFilter is null, so BuildNodeDetailsJson emits every
            // key — the full includeNodeDetails:true shape, byte-identical to before. An
            // explicitly requested `nodeState` field is honored even though includeNodeState
            // defaults false, matching get_node_details_batch whose fields:["nodeState"] returns it.
            const bool bWantNodeState = bIncludeNodeState
                || (FieldFilter && FieldFilter->Contains(TEXT("nodestate")));
            NodeObj = BuildNodeDetailsJson(Node, bIncludePinDefaults, bWantNodeState, FieldFilter);
        }
        else
        {
            NodeObj = MakeShared<FJsonObject>();
            NodeObj->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
            NodeObj->SetStringField(TEXT("nodeName"), Node->GetName());
            NodeObj->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
            if (bIncludeNodeState)
            {
                NodeObj->SetObjectField(TEXT("nodeState"), BuildNodeStateJson(Node));
            }
        }
        Nodes.Add(MakeShared<FJsonValueObject>(NodeObj));
    }
    Result->SetArrayField(TEXT("nodes"), Nodes);

    if (bIncludeConnections)
    {
		TArray<TSharedPtr<FJsonValue>> Connections;
		BuildGraphConnectionsJson(TargetGraph, Connections);
		Result->SetArrayField(TEXT("connections"), Connections);
		Result->SetNumberField(TEXT("connectionCount"), Connections.Num());
    }

    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.graph.get_graph_connections ----
REGISTER_RPC_HANDLER("blueprint.graph.get_graph_connections", "blueprint.graph",
    "Get all connections in a blueprint graph",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_OPT("graphName", "string", "Graph name"),
        RPC_PARAM_OPT("nodeIds", "array", "Only include edges where fromNodeId or toNodeId is in this set (strings)"),
        RPC_PARAM_OPT("edgeType", "string", "Filter to 'exec' or 'data' edges only; omit for both"),
        RPC_PARAM_OPT("maxEdges", "number", "Cap on returned edges; sets truncated=true when reached")
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    // Parse nodeIds filter
    TSet<FString> NodeIdSet;
    const TArray<TSharedPtr<FJsonValue>>* NodeIdsArray = Ctx.GetArray(TEXT("nodeIds"));
    if (NodeIdsArray)
    {
        for (const TSharedPtr<FJsonValue>& Val : *NodeIdsArray)
        {
            if (Val.IsValid() && Val->Type == EJson::String)
                NodeIdSet.Add(Val->AsString());
        }
    }
    const TSet<FString>* NodeIdFilter = NodeIdsArray ? &NodeIdSet : nullptr;

    // Parse edgeType filter
    const FString EdgeTypeRaw = Ctx.GetString(TEXT("edgeType")).ToLower();
    EEdgeTypeFilter EdgeTypeFilter = EEdgeTypeFilter::Any;
    if (EdgeTypeRaw == TEXT("exec")) EdgeTypeFilter = EEdgeTypeFilter::Exec;
    else if (EdgeTypeRaw == TEXT("data")) EdgeTypeFilter = EEdgeTypeFilter::Data;

    // Parse maxEdges
    const double MaxEdgesNumber = Ctx.GetNumber(TEXT("maxEdges"));
    const int32 MaxEdges = MaxEdgesNumber > 0.0 ? FMath::RoundToInt(MaxEdgesNumber) : 0;

    bool bTruncated = false;
    int32 TotalMatched = 0;
    TArray<TSharedPtr<FJsonValue>> Connections;
    BuildGraphConnectionsJson(TargetGraph, Connections, NodeIdFilter, EdgeTypeFilter, MaxEdges, &bTruncated, &TotalMatched);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("graphName"), TargetGraph->GetName());
    Result->SetArrayField(TEXT("connections"), Connections);
    Result->SetNumberField(TEXT("connectionCount"), Connections.Num());
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    Result->SetNumberField(TEXT("totalMatched"), TotalMatched);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.graph.get_pin_details ----
REGISTER_RPC_HANDLER("blueprint.graph.get_pin_details", "blueprint.graph",
    "Get details about a node's pins",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("nodeId", "string", "Node ID or name"),
        RPC_PARAM_OPT("pinName", "string", "Specific pin name (omit for all pins)"),
        RPC_PARAM_OPT("graphName", "string", "Graph name")
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString NodeId, PinName;
    Payload->TryGetStringField(TEXT("nodeId"), NodeId);
    Payload->TryGetStringField(TEXT("pinName"), PinName);

    UEdGraphNode* TargetNode = FindNodeByIdOrName(TargetGraph, NodeId);
    if (!TargetNode) { Ctx.SendError(TEXT("NODE_NOT_FOUND"), TEXT("Node not found.")); return true; }

    TArray<UEdGraphPin*> PinsToReport;
    if (!PinName.IsEmpty())
    {
        UEdGraphPin* Pin = FindPinByName(TargetNode, PinName);
        if (!Pin)
        {
            Ctx.SendError(TEXT("PIN_NOT_FOUND"), TEXT("Pin not found."));
            return true;
        }
        PinsToReport.Add(Pin);
    }
    else
    {
        PinsToReport = TargetNode->Pins;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), NodeId);
    TArray<TSharedPtr<FJsonValue>> PinsJson;
    for (UEdGraphPin* Pin : PinsToReport)
    {
        if (!Pin) continue;
        TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
        PinObj->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
        PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
        PinObj->SetStringField(TEXT("pinType"), Pin->PinType.PinCategory.ToString());
        if (Pin->LinkedTo.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> LinkedArray;
            for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                if (!LinkedPin) continue;
                FString LinkedNodeId = LinkedPin->GetOwningNode() ? LinkedPin->GetOwningNode()->NodeGuid.ToString() : FString();
                const FString LinkedLabel = LinkedNodeId.IsEmpty()
                    ? LinkedPin->PinName.ToString()
                    : FString::Printf(TEXT("%s:%s"), *LinkedNodeId, *LinkedPin->PinName.ToString());
                LinkedArray.Add(MakeShared<FJsonValueString>(LinkedLabel));
            }
            PinObj->SetArrayField(TEXT("linkedTo"), LinkedArray);
        }
        if (!Pin->DefaultValue.IsEmpty()) PinObj->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
        else if (!Pin->DefaultTextValue.IsEmptyOrWhitespace()) PinObj->SetStringField(TEXT("defaultTextValue"), Pin->DefaultTextValue.ToString());
        else if (Pin->DefaultObject) PinObj->SetStringField(TEXT("defaultObjectPath"), Pin->DefaultObject->GetPathName());
        PinsJson.Add(MakeShared<FJsonValueObject>(PinObj));
    }
    Result->SetArrayField(TEXT("pins"), PinsJson);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.graph.get_node_details_batch ----
REGISTER_RPC_HANDLER("blueprint.graph.get_node_details_batch", "blueprint.graph",
    "Get details for multiple nodes in one call",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("nodeIds", "array", "Array of node IDs"),
        RPC_PARAM_OPT("graphName", "string", "Graph name"),
        RPC_PARAM_OPT("includePinDefaults", "boolean", "Include pin default/default object values in each node's pins (default true). The per-pin default payload is the dominant per-node bloat; set false to shed it while keeping the pins adjacency."),
        RPC_PARAM_OPT_ALIAS("fields", "array|string", "Case-insensitive allow-list of per-node detail keys to return (valid keys: nodeId, nodeName, nodeType, nodeTitle, nodeComment, x, y, pins, nodeState). The heavy 'pins' array (with pin defaults) and 'nodeState' dominate the payload; drop them to keep a targeted positions/wiring cross-check inline instead of spilling to disk. e.g. [\"nodeId\",\"x\",\"y\"]. Omit for the full per-node shape. A single string is also accepted. The per-item nodeId echo stays regardless so results are always correlatable.", "field"),
        RPC_PARAM_OPT_ALIAS("namesOnly", "boolean", "When true, each node's details carry only nodeId/nodeName/nodeType/nodeTitle/x/y and omit the heavy pins/nodeState — shorthand for a positions/identity cross-check. Snake_case names_only accepted. Ignored when fields is supplied.", "names_only")
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    const TArray<TSharedPtr<FJsonValue>>* NodeIds = Ctx.GetArray(TEXT("nodeIds"));
    if (!NodeIds)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("get_node_details_batch requires 'nodeIds' array."));
        return true;
    }

    // Per-node field projection, mirroring blueprint.graph.get_nodes: an explicit
    // `fields` allow-list (array or bare string) wins; otherwise `namesOnly` expands
    // to the light identification set (dropping the heavy pins/nodeState). An empty
    // set means "no projection", so the default full per-node shape stays byte-identical
    // to before. ReadFieldProjection lowercases the keys; BuildNodeDetailsJson matches
    // case-insensitively. This is the narrowing lever that lets a positions/wiring
    // cross-check of a handful of nodes stay inline instead of overflowing to disk.
    const bool bIncludePinDefaults = Ctx.GetBool(TEXT("includePinDefaults"), true);
    const TSet<FString> Fields = Ctx.ReadFieldProjection(
        {TEXT("nodeId"), TEXT("nodeName"), TEXT("nodeType"), TEXT("nodeTitle"), TEXT("x"), TEXT("y")});
    const TSet<FString>* FieldFilter = Fields.Num() > 0 ? &Fields : nullptr;

    TArray<TSharedPtr<FJsonValue>> Items;
    int32 SuccessCount = 0, ErrorCount = 0;

    for (const TSharedPtr<FJsonValue>& NodeIdValue : *NodeIds)
    {
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        FString NodeId;
        if (NodeIdValue.IsValid())
        {
            if (NodeIdValue->Type == EJson::String) NodeId = NodeIdValue->AsString();
            else if (NodeIdValue->Type == EJson::Object && NodeIdValue->AsObject().IsValid())
                NodeIdValue->AsObject()->TryGetStringField(TEXT("nodeId"), NodeId);
        }
        Item->SetStringField(TEXT("nodeId"), NodeId);

        if (NodeId.IsEmpty())
        {
            Item->SetBoolField(TEXT("success"), false);
            Item->SetStringField(TEXT("error"), TEXT("INVALID_NODE_ID"));
            Item->SetStringField(TEXT("message"), TEXT("nodeId must be non-empty."));
            ++ErrorCount;
        }
        else
        {
            UEdGraphNode* TargetNode = FindNodeByIdOrName(TargetGraph, NodeId);
            if (!TargetNode)
            {
                Item->SetBoolField(TEXT("success"), false);
                Item->SetStringField(TEXT("error"), TEXT("NODE_NOT_FOUND"));
                Item->SetStringField(TEXT("message"), TEXT("Node not found."));
                ++ErrorCount;
            }
            else
            {
                Item->SetBoolField(TEXT("success"), true);
                Item->SetObjectField(TEXT("details"),
                    BuildNodeDetailsJson(TargetNode, bIncludePinDefaults, /*bIncludeNodeState=*/true, FieldFilter));
                ++SuccessCount;
            }
        }
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("items"), Items);
    Result->SetNumberField(TEXT("successCount"), SuccessCount);
    Result->SetNumberField(TEXT("errorCount"), ErrorCount);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.graph.get_pin_details_batch ----
REGISTER_RPC_HANDLER("blueprint.graph.get_pin_details_batch", "blueprint.graph",
    "Get pin details for multiple nodes in one call",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("requests", "array", "Array of {nodeId, pinName} objects"),
        RPC_PARAM_OPT("graphName", "string", "Graph name")
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    const TArray<TSharedPtr<FJsonValue>>* Requests = Ctx.GetArray(TEXT("requests"));
    if (!Requests)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("get_pin_details_batch requires 'requests' array."));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Items;
    int32 SuccessCount = 0, ErrorCount = 0;

    for (const TSharedPtr<FJsonValue>& RequestValue : *Requests)
    {
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        if (!RequestValue.IsValid() || RequestValue->Type != EJson::Object || !RequestValue->AsObject().IsValid())
        {
            Item->SetBoolField(TEXT("success"), false);
            Item->SetStringField(TEXT("error"), TEXT("INVALID_REQUEST"));
            Item->SetStringField(TEXT("message"), TEXT("Each request must be an object."));
            ++ErrorCount;
            Items.Add(MakeShared<FJsonValueObject>(Item));
            continue;
        }

        const TSharedPtr<FJsonObject> RequestObj = RequestValue->AsObject();
        FString NodeId, PinName;
        RequestObj->TryGetStringField(TEXT("nodeId"), NodeId);
        RequestObj->TryGetStringField(TEXT("pinName"), PinName);
        Item->SetStringField(TEXT("nodeId"), NodeId);
        if (!PinName.IsEmpty()) Item->SetStringField(TEXT("pinName"), PinName);

        if (NodeId.IsEmpty())
        {
            Item->SetBoolField(TEXT("success"), false);
            Item->SetStringField(TEXT("error"), TEXT("INVALID_NODE_ID"));
            ++ErrorCount;
            Items.Add(MakeShared<FJsonValueObject>(Item));
            continue;
        }

        UEdGraphNode* TargetNode = FindNodeByIdOrName(TargetGraph, NodeId);
        if (!TargetNode)
        {
            Item->SetBoolField(TEXT("success"), false);
            Item->SetStringField(TEXT("error"), TEXT("NODE_NOT_FOUND"));
            ++ErrorCount;
            Items.Add(MakeShared<FJsonValueObject>(Item));
            continue;
        }

        TArray<UEdGraphPin*> PinsToReport;
        if (!PinName.IsEmpty())
        {
            UEdGraphPin* Pin = FindPinByName(TargetNode, PinName);
            if (!Pin)
            {
                Item->SetBoolField(TEXT("success"), false);
                Item->SetStringField(TEXT("error"), TEXT("PIN_NOT_FOUND"));
                Item->SetObjectField(TEXT("lookup"), BuildPinLookupPayload(TargetNode, PinName, TEXT("Pin not found.")));
                ++ErrorCount;
                Items.Add(MakeShared<FJsonValueObject>(Item));
                continue;
            }
            PinsToReport.Add(Pin);
        }
        else
        {
            PinsToReport = TargetNode->Pins;
        }

        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("nodeId"), NodeId);
        TArray<TSharedPtr<FJsonValue>> PinsJson;
        for (UEdGraphPin* Pin : PinsToReport)
        {
            if (!Pin) continue;
            PinsJson.Add(MakeShared<FJsonValueObject>(BuildPinJson(Pin, true)));
        }
        Details->SetArrayField(TEXT("pins"), PinsJson);

        Item->SetBoolField(TEXT("success"), true);
        Item->SetObjectField(TEXT("details"), Details);
        ++SuccessCount;
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("items"), Items);
    Result->SetNumberField(TEXT("successCount"), SuccessCount);
    Result->SetNumberField(TEXT("errorCount"), ErrorCount);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.graph.find_nodes ----
REGISTER_RPC_HANDLER("blueprint.graph.find_nodes", "blueprint.graph",
    "Search for nodes matching a query string",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("query", "string", "Search query (matches name, type, title, comments, pin metadata/defaults)"),
        RPC_PARAM_OPT("graphName", "string", "Graph name; if omitted, all graphs in the blueprint are searched and each match is tagged with graphName/graphKind."),
        RPC_PARAM_OPT("caseSensitive", "boolean", "Case-sensitive search"),
        RPC_PARAM_OPT("matchMode", "string", "Text match mode: contains|prefix|exact|word (default contains)"),
        RPC_PARAM_OPT("searchFields", "array", "Optional allowlist of fields to search"),
        RPC_PARAM_OPT("includePinNames", "boolean", "Include pin name fields in matching (default true)"),
        RPC_PARAM_OPT("includePinTypes", "boolean", "Include pin type/subtype fields in matching (default true)"),
        RPC_PARAM_OPT("includePinDefaults", "boolean", "Include pin default value/object fields in matching (default true)"),
        RPC_PARAM_OPT("requireNodeFieldMatch", "boolean", "Require at least one node-level field hit (name/type/title/comment)"),
        RPC_PARAM_OPT("maxResults", "number", "Maximum results to return"),
        RPC_PARAM_OPT("includeNodeState", "boolean", "Include structured node enabled/disabled state in matches")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    // Detect whether graphName was explicitly provided
    FString GraphNameParam;
    const bool bGraphNameProvided = Payload->TryGetStringField(TEXT("graphName"), GraphNameParam) && !GraphNameParam.IsEmpty();

    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;

    if (bGraphNameProvided)
    {
        // Single-graph path: existing behaviour via ResolveBlueprintAndGraph
        if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;
    }
    else
    {
        // All-graphs path: only resolve the Blueprint, not a specific graph
        if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph, false)) return true;
    }

    FString Query;
    Payload->TryGetStringField(TEXT("query"), Query);
    if (Query.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("find_nodes requires non-empty 'query'."));
        return true;
    }

    bool bCaseSensitive = false;
    Payload->TryGetBoolField(TEXT("caseSensitive"), bCaseSensitive);
    const bool bIncludeNodeState = Ctx.GetBool(TEXT("includeNodeState"), false);
    const bool bIncludePinNames = Ctx.GetBool(TEXT("includePinNames"), true);
    const bool bIncludePinTypes = Ctx.GetBool(TEXT("includePinTypes"), true);
    const bool bIncludePinDefaults = Ctx.GetBool(TEXT("includePinDefaults"), true);
    const bool bRequireNodeFieldMatch = Ctx.GetBool(TEXT("requireNodeFieldMatch"), false);

    FString MatchModeRaw = Ctx.GetString(TEXT("matchMode"));
    const EMcpSearchMatchMode MatchMode = ParseSearchMatchMode(MatchModeRaw);

    TSet<FString> SearchFieldAllowList;
    SearchFieldAllowList.Add(TEXT("nodeName"));
    SearchFieldAllowList.Add(TEXT("nodeType"));
    SearchFieldAllowList.Add(TEXT("nodeTitle"));
    SearchFieldAllowList.Add(TEXT("nodeFunction"));
    SearchFieldAllowList.Add(TEXT("nodeComment"));
    if (bIncludePinNames)
    {
        SearchFieldAllowList.Add(TEXT("pinName"));
    }
    if (bIncludePinTypes)
    {
        SearchFieldAllowList.Add(TEXT("pinType"));
        SearchFieldAllowList.Add(TEXT("pinSubTypeObjectPath"));
        SearchFieldAllowList.Add(TEXT("pinSubTypeObjectName"));
    }
    if (bIncludePinDefaults)
    {
        SearchFieldAllowList.Add(TEXT("pinDefaultValue"));
        SearchFieldAllowList.Add(TEXT("pinDefaultTextValue"));
        SearchFieldAllowList.Add(TEXT("pinDefaultObjectPath"));
    }

    const TArray<TSharedPtr<FJsonValue>>* SearchFieldsArray = nullptr;
    if (Payload->TryGetArrayField(TEXT("searchFields"), SearchFieldsArray) && SearchFieldsArray)
    {
        TSet<FString> ExplicitFields;
        for (const TSharedPtr<FJsonValue>& FieldValue : *SearchFieldsArray)
        {
            if (!FieldValue.IsValid() || FieldValue->Type != EJson::String)
            {
                continue;
            }
            FString CanonicalField;
            if (NormalizeKnownSearchField(FieldValue->AsString(), CanonicalField) && IsKnownSearchFieldName(CanonicalField))
            {
                ExplicitFields.Add(CanonicalField);
            }
        }

        if (ExplicitFields.Num() == 0)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                TEXT("searchFields provided but no known field names were supplied."));
            return true;
        }

        SearchFieldAllowList = MoveTemp(ExplicitFields);
    }

    double MaxResultsNumber = 0.0;
    Payload->TryGetNumberField(TEXT("maxResults"), MaxResultsNumber);
    // In all-graphs mode default to 500; in single-graph mode default to unlimited (0)
    const int32 MaxResults = MaxResultsNumber > 0.0
        ? FMath::RoundToInt(MaxResultsNumber)
        : (bGraphNameProvided ? 0 : 500);

    const FString QueryNormalized = bCaseSensitive ? Query : Query.ToLower();
    TArray<FString> QueryTerms;
    QueryNormalized.ParseIntoArrayWS(QueryTerms);
    if (QueryTerms.Num() == 0) QueryTerms.Add(QueryNormalized);

    // Build the list of graphs to search
    TArray<TPair<UEdGraph*, FString>> GraphsToSearch; // (Graph, Kind)
    if (bGraphNameProvided)
    {
        // Determine kind of the resolved graph for consistent tagging
        FString SingleKind = TEXT("ubergraph");
        for (UEdGraph* G : Blueprint->FunctionGraphs)        { if (G == TargetGraph) { SingleKind = TEXT("function"); break; } }
        for (UEdGraph* G : Blueprint->MacroGraphs)           { if (G == TargetGraph) { SingleKind = TEXT("macro");    break; } }
        for (UEdGraph* G : Blueprint->DelegateSignatureGraphs) { if (G == TargetGraph) { SingleKind = TEXT("event");  break; } }
        GraphsToSearch.Emplace(TargetGraph, SingleKind);
    }
    else
    {
        TSet<FString> SeenGraphNames;
        auto AppendSearchGraph = [&](UEdGraph* Graph, const FString& Kind)
        {
            if (!Graph) return;
            const FString GName = Graph->GetName();
            if (SeenGraphNames.Contains(GName)) return;
            SeenGraphNames.Add(GName);
            GraphsToSearch.Emplace(Graph, Kind);
        };
        for (UEdGraph* Graph : Blueprint->UbergraphPages)          AppendSearchGraph(Graph, TEXT("ubergraph"));
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)          AppendSearchGraph(Graph, TEXT("function"));
        for (UEdGraph* Graph : Blueprint->MacroGraphs)             AppendSearchGraph(Graph, TEXT("macro"));
        for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs) AppendSearchGraph(Graph, TEXT("event"));
    }

    // Per-graph search lambda — returns false when the global MaxResults cap is hit
    TArray<TSharedPtr<FJsonValue>> Matches;
    bool bTruncated = false;

    auto SearchGraph = [&](UEdGraph* Graph, const FString& GraphKind) -> bool
    {
        const FString GraphNameStr = Graph->GetName();
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;

            const FString NodeName = Node->GetName();
            const FString NodeType = Node->GetClass()->GetName();
            const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
            const FString NodeComment = Node->NodeComment;

            TArray<TPair<FString, FString>> SearchFields;
            auto AddSearchField = [&](const FString& FieldName, const FString& Value)
            {
                if (Value.IsEmpty() || !SearchFieldAllowList.Contains(FieldName)) return;
                SearchFields.Emplace(FieldName, bCaseSensitive ? Value : Value.ToLower());
            };

            AddSearchField(TEXT("nodeName"), NodeName);
            AddSearchField(TEXT("nodeType"), NodeType);
            AddSearchField(TEXT("nodeTitle"), NodeTitle);
            AddSearchField(TEXT("nodeComment"), NodeComment);

            // Raw underlying function name for call nodes. The ListView title is
            // display-formatted (e.g. "Print String"), and UE 5.7's
            // ObjectTools::GetUserFacingFunctionName may return the localized
            // friendly name even for ListView, so a query on the actual API name
            // ("PrintString") would no longer hit nodeTitle. Match the unformatted
            // member name here so API-name queries remain reliable.
            if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
            {
                AddSearchField(TEXT("nodeFunction"), CallNode->FunctionReference.GetMemberName().ToString());
            }

            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin) continue;
                AddSearchField(TEXT("pinName"), Pin->PinName.ToString());
                AddSearchField(TEXT("pinType"), Pin->PinType.PinCategory.ToString());

                if (Pin->PinType.PinSubCategoryObject.IsValid())
                {
                    AddSearchField(TEXT("pinSubTypeObjectPath"), Pin->PinType.PinSubCategoryObject->GetPathName());
                    AddSearchField(TEXT("pinSubTypeObjectName"), Pin->PinType.PinSubCategoryObject->GetName());
                }

                if (!Pin->DefaultValue.IsEmpty())
                {
                    AddSearchField(TEXT("pinDefaultValue"), Pin->DefaultValue);
                }
                if (!Pin->DefaultTextValue.IsEmptyOrWhitespace())
                {
                    AddSearchField(TEXT("pinDefaultTextValue"), Pin->DefaultTextValue.ToString());
                }
                if (Pin->DefaultObject)
                {
                    AddSearchField(TEXT("pinDefaultObjectPath"), Pin->DefaultObject->GetPathName());
                }
            }

            bool bMatches = true;
            TMap<FString, TSet<FString>> TermToFields;
            TSet<FString> MatchedFieldsSet;
            for (const FString& Term : QueryTerms)
            {
                bool bTermMatched = false;
                for (const TPair<FString, FString>& Field : SearchFields)
                {
                    if (SearchFieldMatchesTerm(Field.Value, Term, MatchMode))
                    {
                        bTermMatched = true;
                        MatchedFieldsSet.Add(Field.Key);
                        TermToFields.FindOrAdd(Term).Add(Field.Key);
                    }
                }
                if (!bTermMatched)
                {
                    bMatches = false;
                    break;
                }
            }
            if (!bMatches) continue;

            if (bRequireNodeFieldMatch)
            {
                bool bHasNodeFieldMatch = false;
                for (const FString& MatchedField : MatchedFieldsSet)
                {
                    if (IsNodeSearchField(MatchedField))
                    {
                        bHasNodeFieldMatch = true;
                        break;
                    }
                }
                if (!bHasNodeFieldMatch)
                {
                    continue;
                }
            }

            TSharedPtr<FJsonObject> Match = MakeShared<FJsonObject>();
            Match->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
            Match->SetStringField(TEXT("nodeName"), NodeName);
            Match->SetStringField(TEXT("nodeType"), NodeType);
            Match->SetStringField(TEXT("nodeTitle"), NodeTitle);
            Match->SetStringField(TEXT("nodeComment"), NodeComment);
            Match->SetStringField(TEXT("graphName"), GraphNameStr);
            Match->SetStringField(TEXT("graphKind"), GraphKind);
            Match->SetNumberField(TEXT("x"), Node->NodePosX);
            Match->SetNumberField(TEXT("y"), Node->NodePosY);

            TArray<FString> MatchedFields = MatchedFieldsSet.Array();
            MatchedFields.Sort();
            TArray<TSharedPtr<FJsonValue>> MatchedFieldsJson;
            MatchedFieldsJson.Reserve(MatchedFields.Num());
            for (const FString& FieldName : MatchedFields)
            {
                MatchedFieldsJson.Add(MakeShared<FJsonValueString>(FieldName));
            }
            Match->SetArrayField(TEXT("matchedFields"), MatchedFieldsJson);

            TArray<TSharedPtr<FJsonValue>> TermHitsJson;
            for (const FString& Term : QueryTerms)
            {
                const TSet<FString>* FieldsForTerm = TermToFields.Find(Term);
                if (!FieldsForTerm) continue;

                TArray<FString> FieldNames = FieldsForTerm->Array();
                FieldNames.Sort();
                TArray<TSharedPtr<FJsonValue>> FieldNamesJson;
                FieldNamesJson.Reserve(FieldNames.Num());
                for (const FString& FieldName : FieldNames)
                {
                    FieldNamesJson.Add(MakeShared<FJsonValueString>(FieldName));
                }

                TSharedPtr<FJsonObject> TermHitObj = MakeShared<FJsonObject>();
                TermHitObj->SetStringField(TEXT("term"), Term);
                TermHitObj->SetArrayField(TEXT("fields"), FieldNamesJson);
                TermHitsJson.Add(MakeShared<FJsonValueObject>(TermHitObj));
            }
            Match->SetArrayField(TEXT("termHits"), TermHitsJson);

            if (bIncludeNodeState)
            {
                Match->SetObjectField(TEXT("nodeState"), BuildNodeStateJson(Node));
            }
            Matches.Add(MakeShared<FJsonValueObject>(Match));

            // Global cap across all graphs
            if (MaxResults > 0 && Matches.Num() >= MaxResults)
            {
                return false; // cap reached
            }
        }
        return true; // continue to next graph
    };

    for (const TPair<UEdGraph*, FString>& GraphEntry : GraphsToSearch)
    {
        if (!SearchGraph(GraphEntry.Key, GraphEntry.Value))
        {
            bTruncated = true;
            break;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("query"), Query);
    Result->SetArrayField(TEXT("matches"), Matches);
    Result->SetNumberField(TEXT("matchCount"), Matches.Num());
    Result->SetStringField(TEXT("matchMode"), SearchMatchModeToString(MatchMode));
    Result->SetBoolField(TEXT("requireNodeFieldMatch"), bRequireNodeFieldMatch);
    Result->SetNumberField(TEXT("graphsSearched"), GraphsToSearch.Num());
    Result->SetStringField(TEXT("scope"), bGraphNameProvided ? TEXT("single") : TEXT("all"));
    if (bTruncated)
    {
        Result->SetBoolField(TEXT("truncated"), true);
    }

    TArray<FString> AppliedFields = SearchFieldAllowList.Array();
    AppliedFields.Sort();
    TArray<TSharedPtr<FJsonValue>> AppliedFieldsJson;
    AppliedFieldsJson.Reserve(AppliedFields.Num());
    for (const FString& FieldName : AppliedFields)
    {
        AppliedFieldsJson.Add(MakeShared<FJsonValueString>(FieldName));
    }
    Result->SetArrayField(TEXT("searchFields"), AppliedFieldsJson);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.graph.list_node_types ----
REGISTER_RPC_HANDLER("blueprint.graph.list_node_types", "blueprint.graph",
    "List all available K2Node types for node creation",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path"))
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* Unused = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, Unused, false)) return true;

    TArray<TSharedPtr<FJsonValue>> NodeTypes;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        if (!It->IsChildOf(UK2Node::StaticClass())) continue;
        if (It->HasAnyClassFlags(CLASS_Abstract)) continue;
        TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
        TypeObj->SetStringField(TEXT("className"), It->GetName());
        TypeObj->SetStringField(TEXT("displayName"), It->GetDisplayNameText().ToString());
        NodeTypes.Add(MakeShared<FJsonValueObject>(TypeObj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("nodeTypes"), NodeTypes);
    Result->SetNumberField(TEXT("count"), NodeTypes.Num());
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.graph.get_execution_flow ----
REGISTER_RPC_HANDLER("blueprint.graph.get_execution_flow", "blueprint.graph",
    "Walk execution pin connections from a starting node (BFS)",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_OPT("graphName", "string", "Graph name"),
        RPC_PARAM_OPT("startNodeId", "string", "Starting node (defaults to first event/entry)"),
        RPC_PARAM_OPT("maxDepth", "number", "Maximum nodes to walk (default 100)"),
        RPC_PARAM_OPT("includeAllEntryPoints", "boolean", "Include flow chains for all event/function entry nodes"),
        RPC_PARAM_OPT("includeNodeState", "boolean", "Include structured node enabled/disabled state"),
        RPC_PARAM_OPT("preferEnabledStart", "boolean", "Prefer an enabled entry node when selecting default start"),
        RPC_PARAM_OPT("includeExecutionChain", "boolean", "Include executionChain payload (default true)"),
        RPC_PARAM_OPT("includeDataInputs", "boolean", "Include dataInputs payload (default true)"),
        RPC_PARAM_OPT("includeExecOutputs", "boolean", "Include execOutputs payload (default true)"),
        RPC_PARAM_OPT("entryPointsOnly", "boolean", "Return only entry points without walking chains"),
        RPC_PARAM_OPT("offset", "number", "Execution chain offset for pagination"),
        RPC_PARAM_OPT("pageSize", "number", "Execution chain page size (0 = full chain)"),
        RPC_PARAM_OPT("compact", "boolean", "Use compact chain objects to reduce payload size"),
        RPC_PARAM_OPT("allExecutionChainsSummaryOnly", "boolean", "When includeAllEntryPoints=true, omit per-chain execution arrays")
    ))
{
    const double StartedAt = FPlatformTime::Seconds();
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    int32 MaxDepth = 100;
    if (Payload->HasField(TEXT("maxDepth")))
    {
        MaxDepth = FMath::Clamp(static_cast<int32>(Payload->GetNumberField(TEXT("maxDepth"))), 1, 1000);
    }

    const bool bIncludeAllEntryPoints = Ctx.GetBool(TEXT("includeAllEntryPoints"), false);
    const bool bIncludeNodeState = Ctx.GetBool(TEXT("includeNodeState"), false);
    const bool bPreferEnabledStart = Ctx.GetBool(TEXT("preferEnabledStart"), true);
    const bool bIncludeExecutionChain = Ctx.GetBool(TEXT("includeExecutionChain"), true);
    const bool bIncludeDataInputs = Ctx.GetBool(TEXT("includeDataInputs"), true);
    const bool bIncludeExecOutputs = Ctx.GetBool(TEXT("includeExecOutputs"), true);
    const bool bEntryPointsOnly = Ctx.GetBool(TEXT("entryPointsOnly"), false);
    const bool bCompact = Ctx.GetBool(TEXT("compact"), false);
    const bool bAllExecutionChainsSummaryOnly = Ctx.GetBool(TEXT("allExecutionChainsSummaryOnly"), false);

    int32 Offset = 0;
    if (Payload->HasField(TEXT("offset")))
    {
        Offset = FMath::Max(0, static_cast<int32>(Payload->GetNumberField(TEXT("offset"))));
    }
    int32 PageSize = 0;
    if (Payload->HasField(TEXT("pageSize")))
    {
        PageSize = FMath::Clamp(static_cast<int32>(Payload->GetNumberField(TEXT("pageSize"))), 0, 1000);
    }
    const bool bUsePagination = (Offset > 0) || (PageSize > 0);

    FString StartNodeId;
    Payload->TryGetStringField(TEXT("startNodeId"), StartNodeId);

    TArray<UEdGraphNode*> EntryNodes;
    BlueprintHandlerUtils::CollectEntryNodesRecursive(TargetGraph, EntryNodes);

    // Fallback for graphs driven by a latent exec-output-only root (e.g. an auto-play
    // K2Node_Timeline) with no K2Node_Event / K2Node_FunctionEntry: discover those roots
    // so default-start, includeAllEntryPoints, and entryPointsOnly all behave instead of
    // returning a bare NODE_NOT_FOUND. Only kicks in when there is no real entry node, so
    // ordinary graphs (and timelines wired from an upstream caller) are unaffected.
    bool bEntryPointsAreLatentRoots = false;
    if (EntryNodes.Num() == 0)
    {
        BlueprintHandlerUtils::CollectLatentExecRootNodes(TargetGraph, EntryNodes);
        bEntryPointsAreLatentRoots = EntryNodes.Num() > 0;
    }

    UEdGraphNode* StartNode = FindNodeByIdOrName(TargetGraph, StartNodeId);
    if (!StartNode && EntryNodes.Num() > 0)
    {
        if (bPreferEnabledStart)
        {
            for (UEdGraphNode* Candidate : EntryNodes)
            {
                int64 EnabledStateValue = 0;
                if (!TryGetNodeEnabledStateValue(Candidate, EnabledStateValue) ||
                    EnabledStateValue != 1)
                {
                    StartNode = Candidate;
                    break;
                }
            }
        }
        if (!StartNode)
        {
            StartNode = EntryNodes[0];
        }
    }

    if (!StartNode)
    {
        Ctx.SendError(TEXT("NODE_NOT_FOUND"),
            TEXT("Could not find a starting node (event, function entry, or latent exec root). ")
            TEXT("Pass a 'startNodeId' to root the walk at a specific node; ")
            TEXT("use find_nodes / get_graph_details to locate one."));
        return true;
    }

    auto BuildExecutionChain = [&](UEdGraphNode* ChainStart, bool& bOutTruncated)
        -> TArray<TSharedPtr<FJsonValue>>
    {
        TArray<TSharedPtr<FJsonValue>> Chain;
        TSet<FGuid> Visited;
        TArray<UEdGraphNode*> WorkQueue;
        WorkQueue.Add(ChainStart);

        while (WorkQueue.Num() > 0 && Chain.Num() < MaxDepth)
        {
            UEdGraphNode* CurrentNode = WorkQueue[0];
            WorkQueue.RemoveAt(0);
            if (!CurrentNode || Visited.Contains(CurrentNode->NodeGuid)) continue;
            Visited.Add(CurrentNode->NodeGuid);

            TSharedPtr<FJsonObject> NodeEntry = MakeShared<FJsonObject>();
            NodeEntry->SetNumberField(TEXT("index"), Chain.Num());
            NodeEntry->SetStringField(TEXT("nodeId"), CurrentNode->NodeGuid.ToString());
            NodeEntry->SetStringField(TEXT("nodeType"), CurrentNode->GetClass()->GetName());
            NodeEntry->SetStringField(TEXT("nodeTitle"), CurrentNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
            if (bIncludeNodeState)
            {
                NodeEntry->SetObjectField(TEXT("nodeState"), BuildNodeStateJson(CurrentNode));
            }

            // Data inputs
            if (bIncludeDataInputs)
            {
                TArray<TSharedPtr<FJsonValue>> DataInputs;
                for (UEdGraphPin* Pin : CurrentNode->Pins)
                {
                    if (!Pin || Pin->Direction != EGPD_Input) continue;
                    if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
                    if (Pin->LinkedTo.Num() == 0) continue;
                    for (UEdGraphPin* SourcePin : Pin->LinkedTo)
                    {
                        if (!SourcePin || !SourcePin->GetOwningNode()) continue;
                        TSharedPtr<FJsonObject> DataInput = MakeShared<FJsonObject>();
                        DataInput->SetStringField(TEXT("pin"), Pin->GetName());
                        DataInput->SetStringField(TEXT("source"), SourcePin->GetOwningNode()->NodeGuid.ToString());
                        if (!bCompact)
                        {
                            DataInput->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
                            DataInput->SetStringField(TEXT("sourcePin"), SourcePin->GetName());
                        }
                        DataInputs.Add(MakeShared<FJsonValueObject>(DataInput));
                    }
                }
                NodeEntry->SetArrayField(TEXT("dataInputs"), DataInputs);
            }

            // Exec outputs
            TArray<TSharedPtr<FJsonValue>> ExecOutputs;
            for (UEdGraphPin* Pin : CurrentNode->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output) continue;
                if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
                for (UEdGraphPin* TargetPin : Pin->LinkedTo)
                {
                    if (!TargetPin || !TargetPin->GetOwningNode()) continue;
                    UEdGraphNode* TargetNode = TargetPin->GetOwningNode();
                    if (bIncludeExecOutputs)
                    {
                        TSharedPtr<FJsonObject> ExecOutput = MakeShared<FJsonObject>();
                        ExecOutput->SetStringField(TEXT("pin"), Pin->GetName());
                        ExecOutput->SetStringField(TEXT("targetNodeId"), TargetNode->NodeGuid.ToString());
                        if (!bCompact)
                        {
                            ExecOutput->SetStringField(TEXT("targetNodeTitle"),
                                TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
                        }
                        ExecOutputs.Add(MakeShared<FJsonValueObject>(ExecOutput));
                    }
                    if (!Visited.Contains(TargetNode->NodeGuid)) WorkQueue.Add(TargetNode);
                }
            }
            if (bIncludeExecOutputs)
            {
                NodeEntry->SetArrayField(TEXT("execOutputs"), ExecOutputs);
            }

            // WorkQueue bridge: cross composite boundaries so the BFS doesn't
            // terminate at the composite node edge.
            //
            // Outer-into-composite: when we dequeue a UK2Node_Composite, its normal
            // exec-output pins mirror the inner exit tunnel — but the body starts at
            // InputSinkNode's output exec pins (the inner entry tunnel).  Push every
            // node connected to those inner output exec pins onto the queue.
            if (UK2Node_Composite* Composite = Cast<UK2Node_Composite>(CurrentNode))
            {
                if (Composite->InputSinkNode)
                {
                    for (UEdGraphPin* InnerPin : Composite->InputSinkNode->Pins)
                    {
                        if (!InnerPin || InnerPin->Direction != EGPD_Output) continue;
                        if (InnerPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
                        for (UEdGraphPin* LinkedPin : InnerPin->LinkedTo)
                        {
                            if (!LinkedPin) continue;
                            UEdGraphNode* InnerNode = LinkedPin->GetOwningNode();
                            if (InnerNode && !Visited.Contains(InnerNode->NodeGuid))
                            {
                                WorkQueue.Add(InnerNode);
                            }
                        }
                    }
                }
            }

            // Inner-out-of-composite: when we dequeue the body's exit tunnel
            // (bCanHaveInputs=true, bCanHaveOutputs=false), find the owning
            // UK2Node_Composite in the outer graph and push nodes connected to
            // its outer output exec pins.
            if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(CurrentNode))
            {
                // Exit tunnel: inputs flow in, no outputs (the composite handles the outer side)
                if (Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs)
                {
                    UK2Node_Composite* OwnerComposite = nullptr;

                    // BoundGraph->Outer is the UK2Node_Composite (production layout, and test fixture).
                    if (UEdGraph* BoundGraph = Cast<UEdGraph>(Tunnel->GetOuter()))
                    {
                        OwnerComposite = Cast<UK2Node_Composite>(BoundGraph->GetOuter());
                    }

                    if (OwnerComposite)
                    {
                        // The composite's outer output exec pins drive downstream nodes.
                        // Match by pin name: the exit tunnel pin name corresponds to the
                        // composite's outer output pin of the same name (UE canonical convention).
                        for (UEdGraphPin* TunnelPin : Tunnel->Pins)
                        {
                            if (!TunnelPin || TunnelPin->Direction != EGPD_Input) continue;
                            if (TunnelPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

                            // Find the matching outer composite pin by name
                            for (UEdGraphPin* CompositePin : OwnerComposite->Pins)
                            {
                                if (!CompositePin || CompositePin->Direction != EGPD_Output) continue;
                                if (CompositePin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
                                if (CompositePin->PinName != TunnelPin->PinName) continue;

                                for (UEdGraphPin* LinkedPin : CompositePin->LinkedTo)
                                {
                                    if (!LinkedPin) continue;
                                    UEdGraphNode* OuterNode = LinkedPin->GetOwningNode();
                                    if (OuterNode && !Visited.Contains(OuterNode->NodeGuid))
                                    {
                                        WorkQueue.Add(OuterNode);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Chain.Add(MakeShared<FJsonValueObject>(NodeEntry));
        }

        bOutTruncated = WorkQueue.Num() > 0;
        return Chain;
    };

    auto ApplyChainPagination = [&](const TArray<TSharedPtr<FJsonValue>>& InChain)
        -> TArray<TSharedPtr<FJsonValue>>
    {
        if (!bUsePagination || InChain.Num() == 0)
        {
            return InChain;
        }

        const int32 SafeOffset = FMath::Clamp(Offset, 0, InChain.Num());
        const int32 EndExclusive = (PageSize > 0)
            ? FMath::Min(InChain.Num(), SafeOffset + PageSize)
            : InChain.Num();

        TArray<TSharedPtr<FJsonValue>> Slice;
        Slice.Reserve(FMath::Max(0, EndExclusive - SafeOffset));
        for (int32 Index = SafeOffset; Index < EndExclusive; ++Index)
        {
            Slice.Add(InChain[Index]);
        }
        return Slice;
    };

    TArray<TSharedPtr<FJsonValue>> EntryPoints;
    EntryPoints.Reserve(EntryNodes.Num());
    for (UEdGraphNode* EntryNode : EntryNodes)
    {
        TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
        EntryObj->SetStringField(TEXT("nodeId"), EntryNode->NodeGuid.ToString());
        EntryObj->SetStringField(TEXT("nodeType"), EntryNode->GetClass()->GetName());
        EntryObj->SetStringField(TEXT("nodeTitle"), EntryNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
        if (bIncludeNodeState)
        {
            EntryObj->SetObjectField(TEXT("nodeState"), BuildNodeStateJson(EntryNode));
        }
        EntryPoints.Add(MakeShared<FJsonValueObject>(EntryObj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("graphName"), TargetGraph->GetName());
    Result->SetStringField(TEXT("startNodeId"), StartNode->NodeGuid.ToString());
    Result->SetArrayField(TEXT("entryPoints"), EntryPoints);
    // True when entryPoints were discovered as latent exec-output-only roots (e.g. an
    // auto-play Timeline) because the graph has no event/function entry node — lets a
    // caller know these roots are inferred, not declared event entries.
    Result->SetBoolField(TEXT("entryPointsAreLatentRoots"), bEntryPointsAreLatentRoots);
    Result->SetBoolField(TEXT("entryPointsOnly"), bEntryPointsOnly);
    Result->SetBoolField(TEXT("compact"), bCompact);
    Result->SetBoolField(TEXT("includeExecutionChain"), bIncludeExecutionChain);
    Result->SetBoolField(TEXT("includeDataInputs"), bIncludeDataInputs);
    Result->SetBoolField(TEXT("includeExecOutputs"), bIncludeExecOutputs);
    Result->SetNumberField(TEXT("offset"), Offset);
    Result->SetNumberField(TEXT("pageSize"), PageSize);

    if (bEntryPointsOnly)
    {
        Result->SetNumberField(TEXT("nodeCount"), 0);
        Result->SetNumberField(TEXT("totalNodeCount"), 0);
        Result->SetNumberField(TEXT("returnedNodeCount"), 0);
    }
    else
    {
        bool bSelectedTruncated = false;
        TArray<TSharedPtr<FJsonValue>> FullExecutionChain = BuildExecutionChain(StartNode, bSelectedTruncated);
        const int32 TotalNodeCount = FullExecutionChain.Num();

        if (bIncludeExecutionChain)
        {
            TArray<TSharedPtr<FJsonValue>> ReturnedExecutionChain = ApplyChainPagination(FullExecutionChain);
            Result->SetArrayField(TEXT("executionChain"), ReturnedExecutionChain);
            Result->SetNumberField(TEXT("nodeCount"), ReturnedExecutionChain.Num());
            Result->SetNumberField(TEXT("returnedNodeCount"), ReturnedExecutionChain.Num());
        }
        else
        {
            Result->SetNumberField(TEXT("nodeCount"), TotalNodeCount);
            Result->SetNumberField(TEXT("returnedNodeCount"), 0);
        }
        Result->SetNumberField(TEXT("totalNodeCount"), TotalNodeCount);
        if (bSelectedTruncated)
        {
            Result->SetBoolField(TEXT("truncated"), true);
        }
    }

    if (bIncludeAllEntryPoints && !bEntryPointsOnly)
    {
        TArray<TSharedPtr<FJsonValue>> AllChains;
        AllChains.Reserve(EntryNodes.Num());
        for (UEdGraphNode* EntryNode : EntryNodes)
        {
            bool bChainTruncated = false;
            TArray<TSharedPtr<FJsonValue>> EntryChain = BuildExecutionChain(EntryNode, bChainTruncated);
            const int32 EntryTotalNodeCount = EntryChain.Num();
            TArray<TSharedPtr<FJsonValue>> ReturnedEntryChain = bUsePagination
                ? ApplyChainPagination(EntryChain)
                : EntryChain;

            TSharedPtr<FJsonObject> ChainObj = MakeShared<FJsonObject>();
            ChainObj->SetStringField(TEXT("startNodeId"), EntryNode->NodeGuid.ToString());
            ChainObj->SetNumberField(TEXT("totalNodeCount"), EntryTotalNodeCount);

            if (bIncludeExecutionChain && !bAllExecutionChainsSummaryOnly)
            {
                ChainObj->SetArrayField(TEXT("executionChain"), ReturnedEntryChain);
                ChainObj->SetNumberField(TEXT("nodeCount"), ReturnedEntryChain.Num());
                ChainObj->SetNumberField(TEXT("returnedNodeCount"), ReturnedEntryChain.Num());
            }
            else
            {
                ChainObj->SetNumberField(TEXT("nodeCount"), EntryTotalNodeCount);
                ChainObj->SetNumberField(TEXT("returnedNodeCount"), 0);
            }

            if (bChainTruncated)
            {
                ChainObj->SetBoolField(TEXT("truncated"), true);
            }
            AllChains.Add(MakeShared<FJsonValueObject>(ChainObj));
        }
        Result->SetArrayField(TEXT("allExecutionChains"), AllChains);
        Result->SetNumberField(TEXT("allExecutionChainCount"), AllChains.Num());
    }

    Result->SetNumberField(TEXT("durationMs"), (FPlatformTime::Seconds() - StartedAt) * 1000.0);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}
