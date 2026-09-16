// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintGraphHelpers.cpp - Implementation of shared graph-aware Blueprint helpers.
#include "Handlers/Blueprint/BlueprintGraphHelpers.h"

#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Utils/PathUtils.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "UObject/Object.h"

namespace
{
    FString ToDirectionString(EEdGraphPinDirection Direction)
    {
        return Direction == EGPD_Input ? TEXT("Input") : TEXT("Output");
    }
}

namespace BlueprintGraphHelpers
{

FString BuildGraphNotFoundMessage(
    const FString& RequestedGraphName,
    const TArray<FString>& AvailableGraphNames)
{
    FString Message = FString::Printf(
        TEXT("Could not find graph '%s' in blueprint."), *RequestedGraphName);

    if (AvailableGraphNames.Num() > 0)
    {
        Message += FString::Printf(TEXT(" Available graphs: %s."),
            *FString::Join(AvailableGraphNames, TEXT(", ")));
    }

    // blueprint.decompile renders each event body as `entry override <Event>(...)` /
    // `entry custom_event <Event>(...)`; a caller naturally copies that name as graphName,
    // but events are nodes inside the shared EventGraph ubergraph, not graphs of their own.
    Message += FString::Printf(
        TEXT(" If '%s' is an event, it lives inside EventGraph"
             " — pass graphName:\"EventGraph\" (or omit graphName to search all graphs)."),
        *RequestedGraphName);

    return Message;
}

FString BuildGraphNotFoundMessage(
    const FString& RequestedGraphName,
    const TArray<UEdGraph*>& AvailableGraphs)
{
    TArray<FString> AvailableGraphNames;
    AvailableGraphNames.Reserve(AvailableGraphs.Num());
    for (UEdGraph* Graph : AvailableGraphs)
    {
        if (Graph) { AvailableGraphNames.Add(Graph->GetName()); }
    }
    return BuildGraphNotFoundMessage(RequestedGraphName, AvailableGraphNames);
}

bool ResolveBlueprintAndGraph(
    FHandlerContext& Ctx,
    UBlueprint*& OutBlueprint,
    UEdGraph*& OutGraph,
    bool bGraphRequired)
{
    FString AssetPath = BlueprintHandlerUtils::ResolveBlueprintPath(Ctx);

    // THE "//" HALF OF THIS CONDITION GUARDS THE LoadObject DIRECTLY BELOW, and it has to sit
    // here rather than at the 24 blueprint.graph.* call sites: LoadObject reaches
    // CreatePackage's Fatal (StaticLoadObjectInternal -> ResolveName2(..., Create=true) ->
    // CreatePackage on the partial name), and Fatal is not compiled out in any configuration -
    // it ends the PROCESS, so the `if (!OutBlueprint)` below can never fire on such a string.
    //
    // NOTHING ABOVE STOPS IT. ResolveBlueprintPath does not load and does not sanitize: for each
    // alias field it calls FindBlueprintNormalizedPath, which prepends "/Game" to an input
    // IsValidMountPoint rejects and then asks the shared asset resolver about the result. That
    // probe can answer about a DIFFERENT, clean path - and on a miss ResolveBlueprintPath assigns
    // `ResolvedPath = Req`, the caller's RAW string. So "/A//B" arrives here verbatim, and a
    // probe that "found nothing" is what let it through rather than what caught it.
    //
    // The code is the one this function already emits for a malformed path argument; only the
    // message differs, and it names "//" so support can grep this refusal apart from a
    // not-found. Sanitizing instead of refusing is wrong: a caller that wrote "//" asked for a
    // package that does not exist, and quietly repairing it answers about another asset.
    if (AssetPath.IsEmpty() || CanReachCreatePackageFatal(AssetPath))
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"),
            AssetPath.IsEmpty()
                ? FString(TEXT("blueprint.graph.*: a blueprint path is required (use 'path' or 'assetPath')."))
                : FString::Printf(
                    TEXT("blueprint.graph.*: blueprint path '%s' contains '//', which no package "
                         "name may; pass the path without the duplicated separator."),
                    *AssetPath));
        return false;
    }

    OutBlueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (!OutBlueprint)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load blueprint at path: %s"), *AssetPath));
        return false;
    }

    if (!bGraphRequired)
    {
        OutGraph = nullptr;
        return true;
    }

    FString GraphName = Ctx.GetString(TEXT("graphName"));

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
            if (Graph->GetName() == GraphName) { OutGraph = Graph; break; }
        }
        if (!OutGraph)
        {
            for (UEdGraph* Graph : OutBlueprint->UbergraphPages)
            {
                if (Graph->GetName() == GraphName) { OutGraph = Graph; break; }
            }
        }
    }

    if (!OutGraph)
    {
        // Only the not-yet-found fallback scan and the GRAPH_NOT_FOUND error need the full
        // graph list, so the allocate-and-walk cost is scoped here off the resolved hot path.
        TArray<UEdGraph*> AllGraphs;
        OutBlueprint->GetAllGraphs(AllGraphs);

        for (UEdGraph* Graph : AllGraphs)
        {
            if (Graph->GetName() == GraphName) { OutGraph = Graph; break; }
        }

        if (!OutGraph)
        {
            Ctx.SendError(TEXT("GRAPH_NOT_FOUND"),
                BuildGraphNotFoundMessage(GraphName, AllGraphs));
            return false;
        }
    }

    return true;
}

bool NodeGuidMatchesId(const FGuid& NodeGuid, const FString& Id)
{
    if (Id.IsEmpty()) return false;
    // FGuid::Parse length-dispatches to the matching format (undashed 32-char
    // EGuidFormats::Digits, dashed 36-char DigitsWithHyphens, and brace/paren
    // variants) and compares hex case-insensitively, so a dashed nodeId copied
    // verbatim from a blueprint.decompile orphan/cast warning resolves against the
    // undashed-stored GUID. It length-checks before ParseExact, so it is bounds-safe
    // on every supported engine (and returns false for empty/short/non-hex ids).
    FGuid Parsed;
    return FGuid::Parse(Id, Parsed) && NodeGuid == Parsed;
}

UEdGraphNode* FindNodeByIdOrName(UEdGraph* Graph, const FString& Id)
{
    if (Id.IsEmpty()) return nullptr;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node) continue;
        if (NodeGuidMatchesId(Node->NodeGuid, Id) ||
            Node->GetName().Equals(Id, ESearchCase::IgnoreCase))
        {
            return Node;
        }
    }
    return nullptr;
}

UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& RequestedName)
{
    if (!Node || RequestedName.IsEmpty()) return nullptr;
    if (UEdGraphPin* ExactPin = Node->FindPin(*RequestedName))
        return ExactPin;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->PinName.ToString().Equals(RequestedName, ESearchCase::IgnoreCase))
            return Pin;
    }
    return nullptr;
}

TSharedPtr<FJsonObject> BuildPinJson(
    UEdGraphPin* Pin,
    bool bIncludeLinks,
    bool bIncludeDefaults)
{
    TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
    PinObj->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
    PinObj->SetStringField(TEXT("direction"), ToDirectionString(Pin->Direction));
    PinObj->SetStringField(TEXT("pinType"), Pin->PinType.PinCategory.ToString());
    if (Pin->PinType.PinSubCategoryObject.IsValid())
    {
        PinObj->SetStringField(TEXT("pinSubType"), Pin->PinType.PinSubCategoryObject->GetName());
    }

    if (bIncludeLinks && Pin->LinkedTo.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> LinkedArray;
        for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            if (!LinkedPin) continue;
            FString LinkedNodeId = LinkedPin->GetOwningNode()
                ? LinkedPin->GetOwningNode()->NodeGuid.ToString() : FString();
            const FString LinkedLabel = LinkedNodeId.IsEmpty()
                ? LinkedPin->PinName.ToString()
                : FString::Printf(TEXT("%s:%s"), *LinkedNodeId, *LinkedPin->PinName.ToString());
            LinkedArray.Add(MakeShared<FJsonValueString>(LinkedLabel));
        }
        PinObj->SetArrayField(TEXT("linkedTo"), LinkedArray);
    }

    if (bIncludeDefaults)
    {
        if (!Pin->DefaultValue.IsEmpty())
        {
            PinObj->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
        }
        else if (!Pin->DefaultTextValue.IsEmptyOrWhitespace())
        {
            PinObj->SetStringField(TEXT("defaultTextValue"), Pin->DefaultTextValue.ToString());
        }
        else if (Pin->DefaultObject)
        {
            PinObj->SetStringField(TEXT("defaultObjectPath"), Pin->DefaultObject->GetPathName());
        }
    }

    return PinObj;
}

TSharedPtr<FJsonObject> BuildPinLookupPayload(
    UEdGraphNode* Node,
    const FString& RequestedPinName,
    const FString& ErrorMessage,
    bool bFilterDirection,
    EEdGraphPinDirection DesiredDirection)
{
    TSharedPtr<FJsonObject> ErrPayload = MakeShared<FJsonObject>();
    ErrPayload->SetStringField(TEXT("error"), ErrorMessage);
    if (!RequestedPinName.IsEmpty())
        ErrPayload->SetStringField(TEXT("requestedPinName"), RequestedPinName);
    if (!Node) return ErrPayload;

    ErrPayload->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
    ErrPayload->SetStringField(TEXT("nodeName"), Node->GetName());

    TArray<TSharedPtr<FJsonValue>> AvailablePins, InputPins, OutputPins, ClosestMatches;
    TSet<FString> ClosestMatchNames;
    const FString RequestedLower = RequestedPinName.ToLower();

    for (UEdGraphPin* NodePin : Node->Pins)
    {
        if (!NodePin) continue;
        if (bFilterDirection && NodePin->Direction != DesiredDirection) continue;

        const FString PinNameValue = NodePin->PinName.ToString();
        AvailablePins.Add(MakeShared<FJsonValueString>(PinNameValue));
        if (NodePin->Direction == EGPD_Input)
            InputPins.Add(MakeShared<FJsonValueString>(PinNameValue));
        else if (NodePin->Direction == EGPD_Output)
            OutputPins.Add(MakeShared<FJsonValueString>(PinNameValue));

        if (!RequestedLower.IsEmpty())
        {
            const FString PinLower = PinNameValue.ToLower();
            const bool bLikelyMatch = PinLower.Equals(RequestedLower) ||
                PinLower.StartsWith(RequestedLower) ||
                PinLower.Contains(RequestedLower) ||
                RequestedLower.Contains(PinLower);
            if (bLikelyMatch && !ClosestMatchNames.Contains(PinNameValue))
            {
                ClosestMatchNames.Add(PinNameValue);
                ClosestMatches.Add(MakeShared<FJsonValueString>(PinNameValue));
            }
        }
    }

    ErrPayload->SetArrayField(TEXT("availablePins"), AvailablePins);
    ErrPayload->SetArrayField(TEXT("inputPins"), InputPins);
    ErrPayload->SetArrayField(TEXT("outputPins"), OutputPins);
    ErrPayload->SetArrayField(TEXT("closestMatches"), ClosestMatches);
    return ErrPayload;
}

} // namespace BlueprintGraphHelpers
