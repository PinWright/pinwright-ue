// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintGraphHelpers.h - Shared graph-aware helpers used by the Blueprint graph
// handler clusters (CRUD / Connections / Inspection-Search). Extracted from
// BlueprintGraphHandler.cpp so the three cluster translation units can link against a
// single definition without ODR collisions when unity merges them.
#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "Misc/Guid.h"

class FHandlerContext;
class FJsonObject;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

namespace BlueprintGraphHelpers
{
    // Resolve assetPath or blueprintPath from context, load the blueprint, and find the
    // target graph. Returns false (and sends error) on failure.
    bool ResolveBlueprintAndGraph(
        FHandlerContext& Ctx,
        UBlueprint*& OutBlueprint,
        UEdGraph*& OutGraph,
        bool bGraphRequired = true);

    // Build the GRAPH_NOT_FOUND error message for an unresolved graphName. Enumerates the
    // blueprint's available graph names and, because blueprint.decompile presents events as
    // `entry override <Event>(...)`, hints that the name may be an event living inside
    // EventGraph (events are not their own graphs). Pure (no UObject access) so it is unit
    // testable: callers pass the already-collected GetAllGraphs() names.
    FString BuildGraphNotFoundMessage(
        const FString& RequestedGraphName,
        const TArray<FString>& AvailableGraphNames);

    // Convenience overload that does the null-guarded GetName() projection once so callers
    // who already hold the blueprint's UEdGraph* list pass it directly instead of hand-rolling
    // the name-collection loop. Forwards to the pure TArray<FString> overload above.
    FString BuildGraphNotFoundMessage(
        const FString& RequestedGraphName,
        const TArray<UEdGraph*>& AvailableGraphs);

    // Case-insensitively match a node's GUID against an id string in either the
    // undashed 32-char (FGuid default `Digits`) or dashed 36-char
    // (`DigitsWithHyphens`) form. blueprint.decompile orphan/cast warnings print
    // nodeId dashed while most surfaces emit the undashed default, so a nodeId
    // pasted verbatim from a warning must resolve either way. Pure (no UObject
    // access) so it is unit-testable in isolation.
    bool NodeGuidMatchesId(const FGuid& NodeGuid, const FString& Id);

    // Find a node by GUID string (dashed or undashed, via NodeGuidMatchesId) or
    // UObject name.
    UEdGraphNode* FindNodeByIdOrName(UEdGraph* Graph, const FString& Id);

    // Find a pin by name (exact then case-insensitive).
    UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& RequestedName);

    // Build a pin JSON object.
    TSharedPtr<FJsonObject> BuildPinJson(
        UEdGraphPin* Pin,
        bool bIncludeLinks,
        bool bIncludeDefaults = true);

    // Build a pin-lookup error payload with available pins listed.
    TSharedPtr<FJsonObject> BuildPinLookupPayload(
        UEdGraphNode* Node,
        const FString& RequestedPinName,
        const FString& ErrorMessage,
        bool bFilterDirection = false,
        EEdGraphPinDirection DesiredDirection = EGPD_Input);
}
