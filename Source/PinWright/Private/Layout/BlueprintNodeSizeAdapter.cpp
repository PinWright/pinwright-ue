// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintNodeSizeAdapter.cpp - Bridges the node-size adapter seam to BpirLayout::EstimateNodeSize.

#include "Layout/BlueprintNodeSizeAdapter.h"
#include "Compiler/NodeLayoutEngine.h"
#include "EdGraph/EdGraphNode.h"

namespace GraphLayout
{
    FVector2D FBlueprintNodeSizeAdapter::EstimateNodeSize(const UObject* Node) const
    {
        // Checked downcast: a null or non-UEdGraphNode pointer yields nullptr here, and
        // EstimateNodeSize tolerates a null node (returns its minimum bounds), so a wrong-type or
        // missing node flows through to a sane minimum rather than reinterpreting raw memory.
        const UEdGraphNode* GraphNode = Cast<UEdGraphNode>(Node);
        return BpirLayout::EstimateNodeSize(GraphNode, Settings);
    }
}
