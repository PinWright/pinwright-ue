// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwGraphLayoutEdGraph.h - PwGraphLayout adapter for UEdGraph graphs: Blueprint (K2) event,
// function and macro graphs, anim graphs and state-machine graphs.
//
// Exec pins (K2 and state-machine "exec" category) are flow; every other pin is data. Sizes and
// pin rows are measured from the node widgets (EdGraphNodeMeasure) when Slate is running and
// UBpirLayoutSettings::bMeasureNodeSizes is on, else estimated by FBlueprintNodeSizeAdapter.
// Comment nodes are not layout nodes: they become model comments whose members are recorded
// from the current rects, and comments with a moved member are re-fitted.

#pragma once

#include "CoreMinimal.h"
#include "Layout/PwGraphLayout.h"

class UAnimBlueprint;
class UBpirLayoutSettings;
class UEdGraph;
class UEdGraphNode;
class UEdGraphNode_Comment;

namespace PwGraphLayout
{
    struct FEdGraphModel
    {
        FLayoutGraph Layout;
        // Parallel to Layout.Nodes; ordered by node GUID, never by Graph->Nodes order.
        TArray<UEdGraphNode*> Nodes;
        // Parallel to Layout.Comments, ordered the same way.
        TArray<UEdGraphNode_Comment*> Comments;
    };

    PINWRIGHT_API FSpacing SpacingFrom(const UBpirLayoutSettings& Settings);

    // Title-bar depth of a comment box drawn at FontSize (the editor's comment widget pads the
    // title text by 3 + 5 px above and 3 + 3 px below).
    PINWRIGHT_API double CommentTitleHeight(int32 FontSize);

    // Every non-comment node of Graph (nodes outside Movable are fixed) and every comment, with
    // its members recorded.
    PINWRIGHT_API FEdGraphModel BuildEdGraphModel(
        UEdGraph* Graph, const TSet<UEdGraphNode*>& Movable, const UBpirLayoutSettings& Settings);

    // Arranges Movable inside Graph; every other node is a fixed obstacle. Roots are explicit tree
    // roots in priority order and may be fixed anchors. Positions and re-fitted comment rects are
    // written after Modify(), so an enclosing transaction undoes them.
    PINWRIGHT_API FArrangeReport ArrangeEdGraph(
        UEdGraph* Graph,
        const TArray<UEdGraphNode*>& Movable,
        const TArray<UEdGraphNode*>& Roots,
        const UBpirLayoutSettings& Settings);

    // Arranges the nodes still at (0,0) in every anim graph of the blueprint: top-level anim
    // graphs, anim-layer interface graphs, and the state-machine graphs nested in them.
    PINWRIGHT_API FArrangeReport ArrangeAnimBlueprint(UAnimBlueprint* AnimBP);
}
