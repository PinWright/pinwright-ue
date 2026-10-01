// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintNodeSizeAdapter.h - INodeSizeAdapter implementation for UEdGraphNode-based graphs
// (Blueprint / K2, anim graph, state machine).
//
// An estimator: the title and the widest input and output pin labels are measured with the
// editor's Slate fonts when a renderer is up (character counts otherwise), and the height is the
// header plus one row per visible pin. Measured sizes (F-graph-node-size-measured) plug in behind
// the same seam later.

#pragma once

#include "CoreMinimal.h"
#include "Layout/GraphLayoutMetrics.h"

class UEdGraphNode;
class UEdGraphPin;
class UBpirLayoutSettings;

namespace GraphLayout
{
    class PINWRIGHT_API FBlueprintNodeSizeAdapter : public INodeSizeAdapter
    {
    public:
        // Settings supply the row height, header height and side padding; the adapter holds a
        // reference for the call duration.
        explicit FBlueprintNodeSizeAdapter(const UBpirLayoutSettings& InSettings)
            : Settings(InSettings) {}

        // A null or non-UEdGraphNode object gets the minimum node size.
        virtual FVector2D EstimateNodeSize(const UObject* Node) const override;

        // Centre of the Row-th visible pin on one side of the node, from the node's top edge.
        double PinOffsetY(int32 Row) const;

        // True when the pin is drawn: not hidden and not folded away under "advanced".
        static bool IsPinShown(const UEdGraphPin* Pin);

        // True when the pin is drawn in the title bar rather than on a row (an event's delegate
        // output); it takes no row and sits at TitlePinOffsetY().
        static bool IsPinInTitle(const UEdGraphPin* Pin);
        double TitlePinOffsetY() const;

    private:
        const UBpirLayoutSettings& Settings;
    };
}
