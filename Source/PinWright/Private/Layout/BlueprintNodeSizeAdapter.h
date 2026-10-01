// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintNodeSizeAdapter.h - INodeSizeAdapter implementation for UEdGraphNode-based graphs
// (Blueprint / K2, anim graph, state machine).
//
// An estimator, checked against the editor's drawn nodes: the title (every line of it, so a
// subtitle such as "Custom Event" or "Target is Actor" deepens the header), the widest input and
// output pin labels plus the default-value boxes of unlinked inputs, one row per shown pin, and the
// advanced-pin expander and "Development Only" bars. Compact nodes (math operators, conversions)
// have no header and centre their pins. Measured sizes (F-graph-node-size-measured) plug in behind
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

        // Centre of a shown pin, from its node's top edge.
        double PinOffsetY(const UEdGraphPin* Pin) const;

        // True when the pin is drawn: not hidden and not folded away under "advanced".
        static bool IsPinShown(const UEdGraphPin* Pin);

        // True when the pin is drawn in the title bar rather than on a row (an event's delegate
        // output); it takes no row.
        static bool IsPinInTitle(const UEdGraphPin* Pin);

    private:
        // Header depth: the configured header plus one line per extra title line.
        double HeaderDepth(const UEdGraphNode* Node) const;

        const UBpirLayoutSettings& Settings;
    };
}
