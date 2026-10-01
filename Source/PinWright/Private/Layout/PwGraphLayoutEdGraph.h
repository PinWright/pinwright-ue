// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwGraphLayoutEdGraph.h - PwGraphLayout adapter for UEdGraph graphs: Blueprint (K2) event,
// function and macro graphs, anim graphs and state-machine graphs.
//
// Exec pins (K2 and state-machine "exec" category) are flow; every other pin is data. Sizes come
// from FBlueprintNodeSizeAdapter (estimated). Comment nodes are left out of the model.

#pragma once

#include "CoreMinimal.h"
#include "Layout/PwGraphLayout.h"

class UAnimBlueprint;
class UBpirLayoutSettings;
class UEdGraph;
class UEdGraphNode;

namespace PwGraphLayout
{
    struct FEdGraphModel
    {
        FLayoutGraph Layout;
        // Parallel to Layout.Nodes; ordered by node GUID, never by Graph->Nodes order.
        TArray<UEdGraphNode*> Nodes;
    };

    PINWRIGHT_API FSpacing SpacingFrom(const UBpirLayoutSettings& Settings);

    // Every non-comment node of Graph; nodes outside Movable are fixed.
    PINWRIGHT_API FEdGraphModel BuildEdGraphModel(
        UEdGraph* Graph, const TSet<UEdGraphNode*>& Movable, const UBpirLayoutSettings& Settings);

    // Arranges Movable inside Graph; every other node is a fixed obstacle. Roots are explicit tree
    // roots in priority order and may be fixed anchors. Positions are written after Modify(), so
    // an enclosing transaction undoes them.
    PINWRIGHT_API FArrangeReport ArrangeEdGraph(
        UEdGraph* Graph,
        const TArray<UEdGraphNode*>& Movable,
        const TArray<UEdGraphNode*>& Roots,
        const UBpirLayoutSettings& Settings);

    // Arranges the nodes still at (0,0) in every anim graph of the blueprint: top-level anim
    // graphs, anim-layer interface graphs, and the state-machine graphs nested in them.
    PINWRIGHT_API FArrangeReport ArrangeAnimBlueprint(UAnimBlueprint* AnimBP);
}
