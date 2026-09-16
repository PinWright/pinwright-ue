// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintGraphHandler.cpp - Blueprint graph node/pin reference extraction.
// The node CRUD, pin connection, and inspection/search handlers that used to live here
// were split into BlueprintGraphCrudHandler.cpp, BlueprintGraphConnectionsHandler.cpp,
// and BlueprintGraphInspectionHandler.cpp (shared helpers in BlueprintGraphHelpers.h).
// This file retains only the blueprint.references handler, which sits in the bare
// blueprint.* namespace rather than blueprint.graph.* — relocating it to a dedicated
// BlueprintReferencesHandler.cpp is deferred to a follow-up.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintGraphHelpers.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/NameMatchFilter.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"

using BlueprintGraphHelpers::ResolveBlueprintAndGraph;

// ---- blueprint.references ----
REGISTER_RPC_HANDLER("blueprint.references", "blueprint",
    "Find asset/class references used by blueprint graph nodes and pins",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_OPT("graphName", "string", "Optional graph name filter"),
        RPC_PARAM_OPT("targetPath", "path", "Optional reference path/class filter"),
        RPC_PARAM_OPT("exactTarget", "boolean", "Require exact targetPath match (default false)"),
        RPC_PARAM_OPT("nodeType", "string", "Optional node class/name filter"),
        RPC_PARAM_OPT("caseSensitive", "boolean", "When true, BOTH 'targetPath' and 'nodeType' are matched case-sensitively; default false, i.e. case-INSENSITIVE. Applies to the substring form as well as to exactTarget. Rejected with INVALID_ARGUMENT when neither filter is supplied. The resolved value is echoed back whenever a filter is active."),
        RPC_PARAM_OPT("offset", "number", "Result offset for pagination"),
        RPC_PARAM_OPT("limit", "number", "Result page size (0 = unbounded)"),
        RPC_PARAM_OPT("includePinSubTypes", "boolean", "Include pin subtype object references (default true)"),
        RPC_PARAM_OPT("includePinDefaults", "boolean", "Include pin default object/text references (default true)"),
        RPC_PARAM_OPT("includeNodeTypePath", "boolean", "Include node class paths as references"),
        RPC_PARAM_OPT("maxResults", "number", "Legacy alias for limit")
    ))
{
    const double StartedAt = FPlatformTime::Seconds();
    UBlueprint* Blueprint = nullptr;
    UEdGraph* UnusedGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, UnusedGraph, false)) return true;

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const FString GraphNameFilter = Ctx.GetString(TEXT("graphName"));
    const FString TargetPath = Ctx.GetString(TEXT("targetPath"));
    const FString NodeTypeFilter = Ctx.GetString(TEXT("nodeType"));
    const bool bExactTarget = Ctx.GetBool(TEXT("exactTarget"), false);
    const bool bCaseSensitive = Ctx.GetBool(TEXT("caseSensitive"), false);
    const bool bIncludePinSubTypes = Ctx.GetBool(TEXT("includePinSubTypes"), true);
    const bool bIncludePinDefaults = Ctx.GetBool(TEXT("includePinDefaults"), true);
    const bool bIncludeNodeTypePath = Ctx.GetBool(TEXT("includeNodeTypePath"), false);

    double OffsetNumber = 0.0;
    Payload->TryGetNumberField(TEXT("offset"), OffsetNumber);
    const int32 Offset = FMath::Max(0, FMath::RoundToInt(OffsetNumber));

    double LimitNumber = 0.0;
    Payload->TryGetNumberField(TEXT("limit"), LimitNumber);
    int32 Limit = LimitNumber > 0.0 ? FMath::RoundToInt(LimitNumber) : 0;

    double MaxResultsNumber = 0.0;
    Payload->TryGetNumberField(TEXT("maxResults"), MaxResultsNumber);
    if (Limit <= 0 && MaxResultsNumber > 0.0)
    {
        Limit = FMath::RoundToInt(MaxResultsNumber);
    }
    Limit = FMath::Clamp(Limit, 0, 100000);

    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);

    TArray<UEdGraph*> GraphsToSearch;
    GraphsToSearch.Reserve(AllGraphs.Num());
    if (!GraphNameFilter.IsEmpty())
    {
        for (UEdGraph* Graph : AllGraphs)
        {
            if (Graph && Graph->GetName().Equals(GraphNameFilter, ESearchCase::IgnoreCase))
            {
                GraphsToSearch.Add(Graph);
            }
        }

        if (GraphsToSearch.Num() == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_GRAPH_NOT_FOUND,
                BlueprintGraphHelpers::BuildGraphNotFoundMessage(GraphNameFilter, AllGraphs));
            return true;
        }
    }
    else
    {
        for (UEdGraph* Graph : AllGraphs)
        {
            if (Graph)
            {
                GraphsToSearch.Add(Graph);
            }
        }
    }

    // `caseSensitive` used to decide only whether both operands were lowercased first.
    // That is sufficient for the exactTarget branch, which pinned
    // Equals(ESearchCase::CaseSensitive) - but not for the DEFAULT substring branch:
    // FString::Contains defaults to ESearchCase::IgnoreCase, so lowercasing nothing and
    // comparing case-insensitively is the same match either way. The flag was dead on the
    // path callers actually use, and dead in a way the response could not reveal, so a
    // caller who passed caseSensitive:true got positive confirmation of a case-exact
    // result set that was never case-exact.
    //
    // NameMatch::FFilter is the shared matcher whose whole point is that every comparison
    // passes ESearchCase explicitly. It is built here rather than through NameMatch::Parse
    // because Parse is single-pattern and this verb carries TWO independent patterns
    // (targetPath and nodeType) under one shared case modifier, plus its own exactTarget
    // switch. Reusing the struct keeps the semantics identical to actor.list and
    // system.inspect.* without pretending the wire shape is the same.
    const bool bTargetFilterActive = !TargetPath.IsEmpty();
    const bool bNodeTypeFilterActive = !NodeTypeFilter.IsEmpty();

    // A case modifier with nothing to match would silently return every reference and look
    // like a filtered answer - the same false-confidence failure, one step earlier. Refuse
    // it rather than accept-and-ignore. exactTarget is in the same guard: it is equally
    // meaningless without targetPath.
    {
        const TSharedPtr<FJsonObject>& RawPayload = Ctx.GetRawPayload();
        const bool bCaseSupplied = RawPayload.IsValid() && RawPayload->HasField(TEXT("caseSensitive"));
        const bool bExactSupplied = RawPayload.IsValid() && RawPayload->HasField(TEXT("exactTarget"));
        if (bCaseSupplied && !bTargetFilterActive && !bNodeTypeFilterActive)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("'caseSensitive' only applies to 'targetPath' or 'nodeType'. Supplied with "
                     "neither, every reference matches and the result would look like a filtered "
                     "list. Pass a filter, or drop the modifier."));
            return true;
        }
        if (bExactSupplied && !bTargetFilterActive)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("'exactTarget' only applies to 'targetPath'. Supplied without one, every "
                     "reference matches and the result would look like a filtered list. Pass "
                     "'targetPath', or drop the modifier."));
            return true;
        }
    }

    NameMatch::FFilter TargetFilter;
    TargetFilter.Pattern = TargetPath;
    TargetFilter.Mode = bExactTarget ? NameMatch::EMode::Exact : NameMatch::EMode::Contains;
    TargetFilter.bCaseSensitive = bCaseSensitive;

    NameMatch::FFilter NodeTypeFilterMatcher;
    NodeTypeFilterMatcher.Pattern = NodeTypeFilter;
    NodeTypeFilterMatcher.Mode = NameMatch::EMode::Contains;
    NodeTypeFilterMatcher.bCaseSensitive = bCaseSensitive;

    auto MatchesTargetFilter = [&TargetFilter](const FString& Candidate) -> bool
    {
        return TargetFilter.Matches(Candidate);
    };

    auto MatchesNodeTypeFilter = [&NodeTypeFilterMatcher, bNodeTypeFilterActive](UEdGraphNode* Node) -> bool
    {
        if (!bNodeTypeFilterActive)
        {
            return true;
        }
        if (!Node)
        {
            return true;
        }

        // GetPathName allocates, so keep the short-circuit rather than calling
        // MatchesEither, which would materialize it unconditionally.
        return NodeTypeFilterMatcher.Matches(Node->GetClass()->GetName())
            || NodeTypeFilterMatcher.Matches(Node->GetClass()->GetPathName());
    };

    auto LooksLikeAssetReference = [](const FString& Candidate) -> bool
    {
        return Candidate.Contains(TEXT("/Game/")) ||
            Candidate.Contains(TEXT("/Engine/")) ||
            Candidate.Contains(TEXT("/Script/")) ||
            Candidate.Contains(TEXT("'/")) ||
            Candidate.StartsWith(TEXT("/"));
    };

    TArray<TSharedPtr<FJsonValue>> References;
    TSet<FString> DedupKeys;
    int32 TotalMatchedCount = 0;
    int32 GraphsScanned = 0;
    int32 NodesScanned = 0;
    bool bHasMore = false;

    auto AddReference = [&](UEdGraph* Graph, UEdGraphNode* Node, const FString& SourceField,
        const FString& ReferencePath, const FString& PinName = FString())
    {
        if (!Graph || !Node || ReferencePath.IsEmpty())
        {
            return;
        }
        if (!MatchesTargetFilter(ReferencePath))
        {
            return;
        }

        const FString DedupKey = FString::Printf(TEXT("%s|%s|%s|%s|%s"),
            *Graph->GetName(), *Node->NodeGuid.ToString(), *SourceField, *PinName, *ReferencePath);
        if (DedupKeys.Contains(DedupKey))
        {
            return;
        }
        DedupKeys.Add(DedupKey);

        ++TotalMatchedCount;
        if (TotalMatchedCount <= Offset)
        {
            return;
        }

        if (Limit > 0 && References.Num() >= Limit)
        {
            bHasMore = true;
            return;
        }

        TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
        RefObj->SetStringField(TEXT("graphName"), Graph->GetName());
        RefObj->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
        RefObj->SetStringField(TEXT("nodeName"), Node->GetName());
        RefObj->SetStringField(TEXT("nodeType"), Node->GetClass()->GetName());
        RefObj->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        RefObj->SetStringField(TEXT("sourceField"), SourceField);
        RefObj->SetStringField(TEXT("referencePath"), ReferencePath);
        if (!PinName.IsEmpty())
        {
            RefObj->SetStringField(TEXT("pinName"), PinName);
        }
        References.Add(MakeShared<FJsonValueObject>(RefObj));
    };

    for (UEdGraph* Graph : GraphsToSearch)
    {
        if (!Graph)
        {
            continue;
        }
        ++GraphsScanned;

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            ++NodesScanned;

            if (!MatchesNodeTypeFilter(Node))
            {
                continue;
            }

            if (bIncludeNodeTypePath)
            {
                AddReference(Graph, Node, TEXT("nodeTypePath"), Node->GetClass()->GetPathName());
            }

            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin)
                {
                    continue;
                }

                if (bIncludePinSubTypes && Pin->PinType.PinSubCategoryObject.IsValid())
                {
                    AddReference(Graph, Node, TEXT("pinSubTypeObjectPath"),
                        Pin->PinType.PinSubCategoryObject->GetPathName(), Pin->PinName.ToString());
                }

                if (!bIncludePinDefaults)
                {
                    continue;
                }

                if (Pin->DefaultObject)
                {
                    AddReference(Graph, Node, TEXT("pinDefaultObjectPath"),
                        Pin->DefaultObject->GetPathName(), Pin->PinName.ToString());
                }

                if (!Pin->DefaultValue.IsEmpty() && LooksLikeAssetReference(Pin->DefaultValue))
                {
                    AddReference(Graph, Node, TEXT("pinDefaultValue"),
                        Pin->DefaultValue, Pin->PinName.ToString());
                }

                const FString DefaultTextValue = Pin->DefaultTextValue.ToString();
                if (!DefaultTextValue.IsEmpty() && LooksLikeAssetReference(DefaultTextValue))
                {
                    AddReference(Graph, Node, TEXT("pinDefaultTextValue"),
                        DefaultTextValue, Pin->PinName.ToString());
                }
            }

            if (Limit > 0 && TotalMatchedCount > (Offset + Limit))
            {
                break;
            }
        }

        if (Limit > 0 && TotalMatchedCount > (Offset + Limit))
        {
            break;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    Result->SetArrayField(TEXT("references"), References);
    Result->SetNumberField(TEXT("referenceCount"), References.Num());
    Result->SetNumberField(TEXT("returnedCount"), References.Num());
    Result->SetNumberField(TEXT("totalMatchedCount"), TotalMatchedCount);
    Result->SetNumberField(TEXT("offset"), Offset);
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetBoolField(TEXT("hasMore"), bHasMore || (Limit > 0 && TotalMatchedCount > (Offset + References.Num())));
    Result->SetNumberField(TEXT("graphsScanned"), GraphsScanned);
    Result->SetNumberField(TEXT("nodesScanned"), NodesScanned);
    Result->SetNumberField(TEXT("durationMs"), (FPlatformTime::Seconds() - StartedAt) * 1000.0);
    if (!GraphNameFilter.IsEmpty())
    {
        Result->SetStringField(TEXT("graphName"), GraphNameFilter);
    }
    if (!TargetPath.IsEmpty())
    {
        Result->SetStringField(TEXT("targetPath"), TargetPath);
        Result->SetBoolField(TEXT("exactTarget"), bExactTarget);
    }
    if (!NodeTypeFilter.IsEmpty())
    {
        Result->SetStringField(TEXT("nodeType"), NodeTypeFilter);
    }
    // Echo the case mode whenever it could have changed the row set. Without it the
    // original no-op was invisible from the response: a caller could not tell which
    // semantics produced the references it is about to act on.
    if (bTargetFilterActive || bNodeTypeFilterActive)
    {
        Result->SetBoolField(TEXT("caseSensitive"), bCaseSensitive);
    }
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}
