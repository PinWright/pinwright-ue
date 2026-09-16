// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintGraphConnectionsHandler.cpp - Blueprint graph pin connection handlers.
// Split from BlueprintGraphHandler.cpp: pin link create/break. Shared graph helpers
// live in BlueprintGraphHelpers.h.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintGraphHelpers.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

using BlueprintGraphHelpers::ResolveBlueprintAndGraph;
using BlueprintGraphHelpers::FindNodeByIdOrName;
using BlueprintGraphHelpers::FindPinByName;
using BlueprintGraphHelpers::BuildPinLookupPayload;

// ---- blueprint.graph.connect_pins ----
REGISTER_RPC_HANDLER("blueprint.graph.connect_pins", "blueprint.graph",
    "Wire an output pin to an input pin between two existing graph nodes. UE applies its standard pin-type compatibility rules; mismatched types fail with an explicit message. Use blueprint.graph.break_pin_links to disconnect.",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path.")),
        RPC_PARAM_REQ("fromNodeId", "string", "Source node id (returned by create_node) or unique node name."),
        RPC_PARAM_REQ("fromPinName", "string", "Output pin name on the source node (e.g. 'Then', 'ReturnValue')."),
        RPC_PARAM_REQ("toNodeId", "string", "Target node id or unique node name."),
        RPC_PARAM_REQ("toPinName", "string", "Input pin name on the target node."),
        RPC_PARAM_OPT("graphName", "string", "Containing UEdGraph name; defaults to 'EventGraph'.")
    ))
{
    const double StartedAt = FPlatformTime::Seconds();
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString FromNodeId, FromPinName, ToNodeId, ToPinName;
    Payload->TryGetStringField(TEXT("fromNodeId"), FromNodeId);
    Payload->TryGetStringField(TEXT("fromPinName"), FromPinName);
    Payload->TryGetStringField(TEXT("toNodeId"), ToNodeId);
    Payload->TryGetStringField(TEXT("toPinName"), ToPinName);

    const FScopedTransaction Transaction(FText::FromString(TEXT("Connect Blueprint Pins")));
    Blueprint->Modify();
    TargetGraph->Modify();

    UEdGraphNode* FromNode = FindNodeByIdOrName(TargetGraph, FromNodeId);
    UEdGraphNode* ToNode = FindNodeByIdOrName(TargetGraph, ToNodeId);

    if (!FromNode || !ToNode)
    {
        Ctx.SendError(TEXT("NODE_NOT_FOUND"), TEXT("Could not find source or target node."));
        return true;
    }

    FString FromPinClean = FromPinName;
    if (FromPinName.Contains(TEXT("."))) FromPinName.Split(TEXT("."), nullptr, &FromPinClean);
    FString ToPinClean = ToPinName;
    if (ToPinName.Contains(TEXT("."))) ToPinName.Split(TEXT("."), nullptr, &ToPinClean);

    UEdGraphPin* FromPin = FindPinByName(FromNode, FromPinClean);
    UEdGraphPin* ToPin = FindPinByName(ToNode, ToPinClean);

    if (!FromPin || !ToPin)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("error"), TEXT("Could not find source or target pin."));
        if (!FromPin)
            Result->SetObjectField(TEXT("sourcePinLookup"),
                BuildPinLookupPayload(FromNode, FromPinClean, TEXT("Source pin not found."), true, EGPD_Output));
        if (!ToPin)
            Result->SetObjectField(TEXT("targetPinLookup"),
                BuildPinLookupPayload(ToNode, ToPinClean, TEXT("Target pin not found."), true, EGPD_Input));
        Ctx.SendError(TEXT("PIN_NOT_FOUND"), TEXT("Could not find source or target pin."));
        return true;
    }

    FromNode->Modify();
    ToNode->Modify();

    if (TargetGraph->GetSchema()->TryCreateConnection(FromPin, ToPin))
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddAssetVerification(Result, Blueprint);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(TEXT("CONNECTION_FAILED"), TEXT("Failed to connect pins (schema rejection)."));
    }
    return true;
}

// ---- blueprint.graph.break_pin_links ----
REGISTER_RPC_HANDLER("blueprint.graph.break_pin_links", "blueprint.graph",
    "Break all links on a pin",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("nodeId", "string", "Node ID or name"),
        RPC_PARAM_REQ("pinName", "string", "Pin name to break links on"),
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

    const FScopedTransaction Transaction(FText::FromString(TEXT("Break Blueprint Pin Links")));
    Blueprint->Modify();
    TargetGraph->Modify();

    UEdGraphNode* TargetNode = FindNodeByIdOrName(TargetGraph, NodeId);
    if (!TargetNode) { Ctx.SendError(TEXT("NODE_NOT_FOUND"), TEXT("Node not found.")); return true; }

    UEdGraphPin* Pin = FindPinByName(TargetNode, PinName);
    if (!Pin)
    {
        Ctx.SendError(TEXT("PIN_NOT_FOUND"), TEXT("Pin not found."));
        return true;
    }

    TargetNode->Modify();
    TargetGraph->GetSchema()->BreakPinLinks(*Pin, true);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}
