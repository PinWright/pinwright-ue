// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintNodeSizeAdapter.h - INodeSizeAdapter implementation for Blueprint (UEdGraphNode) nodes.
//
// Wraps the existing, real Slate-measurement estimator BpirLayout::EstimateNodeSize so every
// engine / report can size a Blueprint node through the shared node-size adapter seam without
// reaching into the BPIR compiler layer. The material / anim / rig adapters land with their own
// bounds-aware engine tickets; this is the reference adapter the seam was built for.

#pragma once

#include "CoreMinimal.h"
#include "Layout/GraphLayoutMetrics.h"

class UEdGraphNode;
class UBpirLayoutSettings;

namespace GraphLayout
{
    // Sizes a UEdGraphNode by delegating to BpirLayout::EstimateNodeSize. The UObject passed to
    // EstimateNodeSize is checked-cast to UEdGraphNode; a null or non-Blueprint node falls through
    // to the estimator's minimum bounds.
    class PINWRIGHT_API FBlueprintNodeSizeAdapter : public INodeSizeAdapter
    {
    public:
        // Settings drive padding / min sizes; the adapter holds a reference for the call duration.
        explicit FBlueprintNodeSizeAdapter(const UBpirLayoutSettings& InSettings)
            : Settings(InSettings) {}

        virtual FVector2D EstimateNodeSize(const UObject* Node) const override;

    private:
        const UBpirLayoutSettings& Settings;
    };
}
