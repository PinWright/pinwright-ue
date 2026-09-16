// Copyright (c) 2026 Alexander Penkin. MIT License.

// NodeLayoutParameterFormatter.h - Arranges a pure-node subtree feeding an
// impure consumer into a vertical column to the left of the consumer.
// Produces the combined consumer-plus-pures bounding rect that the main
// FormatX pass consults (via FClusterBoundsRegistry) to space later columns
// clear of the parameter column.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Array.h"
#include "Containers/Set.h"
#include "Layout/SlateRect.h"

class UEdGraphNode;
class UBpirLayoutSettings;

namespace BpirLayout
{
    struct FClusterBoundsRegistry;

    // Parameter formatter: repositions pure ancestor nodes of a single consumer
    // into a left-side column. Does not move the consumer itself.
    class PINWRIGHT_API FNodeLayoutParameterFormatter
    {
    public:
        FNodeLayoutParameterFormatter(
            UEdGraphNode* InConsumer,
            const UBpirLayoutSettings& InSettings);

        // Collects pure ancestors reachable from Consumer via input data-pin edges
        // (restricted to NodesInPool and excluding IgnoredNodes), then stamps each
        // one into a vertical column to the left of Consumer. After this returns,
        // GetClusterBounds() is valid.
        void Format(
            const TSet<UEdGraphNode*>& NodesInPool,
            const TSet<UEdGraphNode*>& IgnoredNodes);

        // Union rect covering Consumer + every placed pure node. Valid after Format().
        FSlateRect GetClusterBounds() const { return ClusterBounds; }

        // Pure nodes that this formatter moved — consumers of the formatter use this
        // list to avoid re-placing them in later passes (and to populate IgnoredNodes
        // for sibling consumers so a shared pure isn't claimed twice).
        const TArray<UEdGraphNode*>& GetPlacedPureNodes() const { return PlacedPureNodes; }

        UEdGraphNode* GetConsumer() const { return Consumer; }

    private:
        // BFS over input data-pin edges, gathering pure ancestors in breadth-first
        // order so direct ancestors appear first. Pure = no exec pins on the node.
        void CollectPureAncestors(
            const TSet<UEdGraphNode*>& NodesInPool,
            const TSet<UEdGraphNode*>& IgnoredNodes,
            TArray<UEdGraphNode*>& OutOrdered);

        UEdGraphNode* Consumer;
        const UBpirLayoutSettings& Settings;

        TArray<UEdGraphNode*> PlacedPureNodes;
        FSlateRect ClusterBounds;
    };
}
