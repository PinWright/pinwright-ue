// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintGraphConnectionsHandler.cpp - Blueprint graph pin connection handlers.
// Split from BlueprintGraphHandler.cpp: pin link create/break. Shared graph helpers
// live in BlueprintGraphHelpers.h.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintGraphHelpers.h"
#include "Handlers/Blueprint/BlueprintReinstancingGuard.h"
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

// Named (not anonymous) namespace: Unity builds merge this TU with its blueprint.graph siblings.
namespace BlueprintGraphConnectionsLocal
{
    // Callers may qualify a pin with its node ("PrintString.execute"); only the pin part resolves.
    FString StripNodeQualifier(const FString& PinName)
    {
        FString Clean = PinName;
        if (PinName.Contains(TEXT("."))) PinName.Split(TEXT("."), nullptr, &Clean);
        return Clean;
    }

    const TCHAR* ConnectionKindName(ECanCreateConnectionResponse Response)
    {
        switch (Response)
        {
        case CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE: return TEXT("conversionNode");
        case CONNECT_RESPONSE_MAKE_WITH_PROMOTION:       return TEXT("promotion");
        default:                                         return TEXT("direct");
        }
    }

    // Re-resolves both pins by the ids the caller sent, because a promotion earlier in the
    // batch may have reconstructed a node and trashed the pin pointers resolved at link time.
    bool IsLinkPresent(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Item)
    {
        UEdGraphPin* FromPin = FindPinByName(
            FindNodeByIdOrName(Graph, Item->GetStringField(TEXT("fromNodeId"))),
            StripNodeQualifier(Item->GetStringField(TEXT("fromPinName"))));
        UEdGraphPin* ToPin = FindPinByName(
            FindNodeByIdOrName(Graph, Item->GetStringField(TEXT("toNodeId"))),
            StripNodeQualifier(Item->GetStringField(TEXT("toPinName"))));
        return FromPin && ToPin && FromPin->LinkedTo.Contains(ToPin);
    }

    // One entry of connect_pins_batch. Never sends a response: the outcome is the returned item.
    TSharedPtr<FJsonObject> ConnectOneLink(UEdGraph* Graph, const TSharedPtr<FJsonValue>& Value, int32 Index)
    {
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetNumberField(TEXT("index"), Index);
        Item->SetBoolField(TEXT("success"), false);
        auto Fail = [&Item](const TCHAR* Code, const FString& Message)
        {
            Item->SetStringField(TEXT("error"), Code);
            Item->SetStringField(TEXT("message"), Message);
            return Item;
        };

        const TSharedPtr<FJsonObject>* LinkObj = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(LinkObj) || !LinkObj || !LinkObj->IsValid())
        {
            return Fail(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("Each link must be an object with fromNodeId, fromPinName, toNodeId, toPinName."));
        }

        FString FromNodeId, FromPinName, ToNodeId, ToPinName;
        (*LinkObj)->TryGetStringField(TEXT("fromNodeId"), FromNodeId);
        (*LinkObj)->TryGetStringField(TEXT("fromPinName"), FromPinName);
        (*LinkObj)->TryGetStringField(TEXT("toNodeId"), ToNodeId);
        (*LinkObj)->TryGetStringField(TEXT("toPinName"), ToPinName);
        Item->SetStringField(TEXT("fromNodeId"), FromNodeId);
        Item->SetStringField(TEXT("fromPinName"), FromPinName);
        Item->SetStringField(TEXT("toNodeId"), ToNodeId);
        Item->SetStringField(TEXT("toPinName"), ToPinName);
        if (FromNodeId.IsEmpty() || FromPinName.IsEmpty() || ToNodeId.IsEmpty() || ToPinName.IsEmpty())
        {
            return Fail(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("fromNodeId, fromPinName, toNodeId and toPinName are all required."));
        }

        UEdGraphNode* FromNode = FindNodeByIdOrName(Graph, FromNodeId);
        UEdGraphNode* ToNode = FindNodeByIdOrName(Graph, ToNodeId);
        if (!FromNode || !ToNode)
        {
            return Fail(ErrorCodes::ERR_NODE_NOT_FOUND, FString::Printf(
                TEXT("Node '%s' not found in graph '%s'."),
                !FromNode ? *FromNodeId : *ToNodeId, *Graph->GetName()));
        }

        const FString FromPinClean = StripNodeQualifier(FromPinName);
        const FString ToPinClean = StripNodeQualifier(ToPinName);
        UEdGraphPin* FromPin = FindPinByName(FromNode, FromPinClean);
        UEdGraphPin* ToPin = FindPinByName(ToNode, ToPinClean);
        if (!FromPin || !ToPin)
        {
            if (!FromPin)
                Item->SetObjectField(TEXT("sourcePinLookup"),
                    BuildPinLookupPayload(FromNode, FromPinClean, TEXT("Source pin not found."), true, EGPD_Output));
            if (!ToPin)
                Item->SetObjectField(TEXT("targetPinLookup"),
                    BuildPinLookupPayload(ToNode, ToPinClean, TEXT("Target pin not found."), true, EGPD_Input));
            return Fail(ErrorCodes::ERR_PIN_NOT_FOUND,
                TEXT("Could not find source or target pin; sourcePinLookup/targetPinLookup list the live pins."));
        }

        // Idempotent retry: an existing link is reported, not re-made (re-making it would break
        // and relink a single-link pin and dirty the graph for nothing).
        if (FromPin->LinkedTo.Contains(ToPin))
        {
            Item->SetBoolField(TEXT("success"), true);
            Item->SetStringField(TEXT("connection"), TEXT("alreadyConnected"));
            return Item;
        }

        const UEdGraphSchema* Schema = Graph->GetSchema();
        const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
        if (Response.Response == CONNECT_RESPONSE_DISALLOW)
        {
            return Fail(ErrorCodes::ERR_CONNECTION_FAILED, FString::Printf(
                TEXT("Schema rejected the connection: %s"), *Response.Message.ToString()));
        }

        FromNode->Modify();
        ToNode->Modify();
        if (!Schema->TryCreateConnection(FromPin, ToPin))
        {
            return Fail(ErrorCodes::ERR_CONNECTION_FAILED, FString::Printf(
                TEXT("Schema accepted the connection (%s) but could not create it."),
                ConnectionKindName(Response.Response)));
        }

        Item->SetBoolField(TEXT("success"), true);
        Item->SetStringField(TEXT("connection"), ConnectionKindName(Response.Response));
        if (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A
            || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B
            || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB)
        {
            Item->SetBoolField(TEXT("brokeExistingLinks"), true);
        }
        return Item;
    }
}

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
        Ctx.SendError(ErrorCodes::ERR_NODE_NOT_FOUND, TEXT("Could not find source or target node."));
        return true;
    }

    const FString FromPinClean = BlueprintGraphConnectionsLocal::StripNodeQualifier(FromPinName);
    const FString ToPinClean = BlueprintGraphConnectionsLocal::StripNodeQualifier(ToPinName);

    UEdGraphPin* FromPin = FindPinByName(FromNode, FromPinClean);
    UEdGraphPin* ToPin = FindPinByName(ToNode, ToPinClean);

    if (!FromPin || !ToPin)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        if (!FromPin)
            Result->SetObjectField(TEXT("sourcePinLookup"),
                BuildPinLookupPayload(FromNode, FromPinClean, TEXT("Source pin not found."), true, EGPD_Output));
        if (!ToPin)
            Result->SetObjectField(TEXT("targetPinLookup"),
                BuildPinLookupPayload(ToNode, ToPinClean, TEXT("Target pin not found."), true, EGPD_Input));
        Ctx.SendError(ErrorCodes::ERR_PIN_NOT_FOUND,
            TEXT("Could not find source or target pin; sourcePinLookup/targetPinLookup list the live pins."), Result);
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
        Ctx.SendError(ErrorCodes::ERR_CONNECTION_FAILED, TEXT("Failed to connect pins (schema rejection)."));
    }
    return true;
}

// ---- blueprint.graph.connect_pins_batch ----
REGISTER_RPC_HANDLER("blueprint.graph.connect_pins_batch", "blueprint.graph",
    "Wire many output->input pin pairs in one Blueprint graph in one call and one undo transaction. Links apply in array order and each is reported in results[]; a failed link never rolls back the others. An existing link is reported as alreadyConnected, so a retry converges. compile=true runs the Blueprint compiler once after all links (refused with LIVE_INSTANCES_WOULD_BE_REINSTANCED when loaded worlds hold live instances; see allowReinstancing); without it nothing compiles, same as connect_pins.",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path.")),
        RPC_PARAM_REQ("links", "array", "Non-empty array of {fromNodeId, fromPinName, toNodeId, toPinName} objects, each with the meaning it has on blueprint.graph.connect_pins. Applied in order."),
        RPC_PARAM_OPT("graphName", "string", "Containing UEdGraph name; defaults to 'EventGraph'."),
        RPC_PARAM_DEF("compile", "boolean", "Compile the Blueprint once after all links and report compile diagnostics.", "false"),
        BlueprintReinstancingGuard::AllowReinstancingParam()
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    const TArray<TSharedPtr<FJsonValue>>* Links = Ctx.GetArray(TEXT("links"));
    if (!Links || Links->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("'links' array is required and must not be empty."));
        return true;
    }

    // Refused before any link is made, so a refused compile leaves the graph untouched.
    const bool bCompile = Ctx.GetBool(TEXT("compile"), false);
    if (bCompile && BlueprintReinstancingGuard::RefuseIfLiveInstancesWouldBeReinstanced(
            Ctx, Blueprint, TEXT("blueprint.graph.connect_pins_batch")))
    {
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Results;
    int32 SuccessCount = 0;
    bool bGraphChanged = false;
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("Batch Connect Blueprint Pins")));
        Blueprint->Modify();
        TargetGraph->Modify();

        TArray<TSharedPtr<FJsonObject>> Items;
        for (int32 Index = 0; Index < Links->Num(); ++Index)
        {
            Items.Add(BlueprintGraphConnectionsLocal::ConnectOneLink(TargetGraph, (*Links)[Index], Index));
        }

        // A later entry can break an earlier one: a single-link pin (an exec output, a data
        // input) keeps only its newest link. Success is therefore measured on the final graph,
        // not taken from the moment each link was made. Conversion and promotion links are not
        // direct pin-to-pin links, so they have no direct link to measure.
        for (const TSharedPtr<FJsonObject>& Item : Items)
        {
            FString Connection;
            Item->TryGetStringField(TEXT("connection"), Connection);
            bGraphChanged |= !Connection.IsEmpty() && Connection != TEXT("alreadyConnected");
            if ((Connection == TEXT("direct") || Connection == TEXT("alreadyConnected"))
                && !BlueprintGraphConnectionsLocal::IsLinkPresent(TargetGraph, Item))
            {
                Item->SetBoolField(TEXT("success"), false);
                Item->SetStringField(TEXT("error"), ErrorCodes::ERR_LINK_SUPERSEDED);
                Item->SetStringField(TEXT("message"),
                    TEXT("The link was made, then broken by a later entry in this batch: one of its pins keeps a single link. Wire each single-link pin once."));
            }

            bool bItemSuccess = false;
            Item->TryGetBoolField(TEXT("success"), bItemSuccess);
            SuccessCount += bItemSuccess ? 1 : 0;
            Results.Add(MakeShared<FJsonValueObject>(Item));
        }
    }

    if (bGraphChanged)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("results"), Results);
    Result->SetNumberField(TEXT("totalLinks"), Links->Num());
    Result->SetNumberField(TEXT("successCount"), SuccessCount);
    Result->SetNumberField(TEXT("failureCount"), Links->Num() - SuccessCount);
    Result->SetBoolField(TEXT("compiled"), false);
    if (bCompile)
    {
        const BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
            BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);
        BlueprintHandlerUtils::AddCompileDiagnosticsToJson(
            Diagnostics, Result, TEXT("compileErrors"), TEXT("compileWarnings"));
    }
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
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
    if (!TargetNode) { Ctx.SendError(ErrorCodes::ERR_NODE_NOT_FOUND, TEXT("Node not found.")); return true; }

    UEdGraphPin* Pin = FindPinByName(TargetNode, PinName);
    if (!Pin)
    {
        Ctx.SendError(ErrorCodes::ERR_PIN_NOT_FOUND, TEXT("Pin not found."));
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
