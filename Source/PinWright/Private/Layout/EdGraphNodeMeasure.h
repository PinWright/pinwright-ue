// Copyright (c) 2026 Alexander Penkin. MIT License.

// EdGraphNodeMeasure.h - Measured sizes and pin rows of UEdGraph nodes for graph layout.
//
// Builds the node's editor widget offscreen through the graph editor's node factory and runs one
// Slate prepass on it: the same widget and desired size a graph panel uses for the node's bounds
// (SGraphPanel prepasses a new node widget the same way, and node widgets take no outer layout
// into account), with no window and no open editor. Pin centres come from arranging the widget
// tree once at scale 1. Without a Slate renderer (commandlets) nothing is measured and the caller
// falls back to FBlueprintNodeSizeAdapter's estimate.

#pragma once

#include "CoreMinimal.h"

class UEdGraphNode;
class UEdGraphPin;

namespace GraphLayout
{
    struct FMeasuredNode
    {
        FVector2D Size = FVector2D::ZeroVector;
        // Centre of every pin the widget draws, from the node's top edge.
        TMap<const UEdGraphPin*, double> PinOffsetY;
    };

    // True when node widgets can be measured here: game thread, Slate initialized with a renderer
    // (also under -NullRHI); false in commandlets.
    PINWRIGHT_API bool CanMeasureEdGraphNodes();

    // False, with Out untouched, when Slate cannot measure here or the widget has no size.
    PINWRIGHT_API bool MeasureEdGraphNode(UEdGraphNode* Node, FMeasuredNode& Out);
}
