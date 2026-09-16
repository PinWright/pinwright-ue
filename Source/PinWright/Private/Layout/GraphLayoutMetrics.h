// Copyright (c) 2026 Alexander Penkin. MIT License.

// GraphLayoutMetrics.h - Engine-agnostic node-graph layout-quality metrics + node-size adapter seam.
//
// Pure geometry util: no editor RPC, no asset deps, no node-type coupling. Feed it a flat list of
// node rects ([{nodeId,x,y,w,h}]) plus center-to-center edges and it returns layout-quality
// sub-scores and a combined 0-1 score. Callable from every layout engine, the layout_report RPCs,
// and the bounds-aware engine work. Unit-testable in isolation.
//
// LIMITATION (documented intentionally): straightness and edge-crossings are approximated from
// node *centers*, not pin coordinates or actual wire routing — pin positions and routed-wire
// geometry are not exposed at this layer. The scores are therefore a layout-quality proxy, not a
// pixel-exact wire measurement.

#pragma once

#include "CoreMinimal.h"
#include "Math/Vector2D.h"

namespace GraphLayout
{
    // A single node's axis-aligned bounding box in graph space. NodeId is an opaque caller-chosen
    // identity (e.g. a node GUID string or array index as text) used only to report problem nodes
    // back; the metrics never interpret it.
    struct PINWRIGHT_API FNodeRect
    {
        FString NodeId;
        double X = 0.0;
        double Y = 0.0;
        double W = 0.0;
        double H = 0.0;

        FNodeRect() = default;
        FNodeRect(const FString& InId, double InX, double InY, double InW, double InH)
            : NodeId(InId), X(InX), Y(InY), W(InW), H(InH) {}

        double Right() const { return X + W; }
        double Bottom() const { return Y + H; }
        FVector2D Center() const { return FVector2D(X + W * 0.5, Y + H * 0.5); }
    };

    // A directed connection between two node ids. The metrics treat edges as undirected
    // center-to-center segments for crossing/straightness; From/To are kept for reporting.
    struct PINWRIGHT_API FGraphEdge
    {
        FString From;
        FString To;

        FGraphEdge() = default;
        FGraphEdge(const FString& InFrom, const FString& InTo) : From(InFrom), To(InTo) {}
    };

    // A pair of node ids whose bounding boxes intersect (positive-area overlap).
    struct PINWRIGHT_API FOverlapPair
    {
        FString A;
        FString B;
        double Area = 0.0;
    };

    // Layout-quality result. Each sub-score is in [0,1] (1 = best). CombinedScore is a weighted
    // blend, also [0,1]. Overlap is an absolute fail: any positive-area bbox intersection drives
    // Overlap to 0 and populates OverlappingPairs.
    struct PINWRIGHT_API FGraphLayoutMetricsResult
    {
        double Overlap = 1.0;       // 1.0 = no overlap; 0.0 = at least one overlapping pair.
        double Spacing = 1.0;       // gap-distribution regularity between nodes.
        double Straightness = 1.0;  // edge alignment (axis-aligned center-to-center runs).
        double EdgeCrossings = 1.0; // 1.0 = no crossings; falls toward 0 as crossings grow.
        double Grid = 1.0;          // snap/alignment regularity of node origins.
        double CombinedScore = 1.0;

        TArray<FOverlapPair> OverlappingPairs;
        int32 EdgeCrossingCount = 0;

        bool HasOverlap() const { return OverlappingPairs.Num() > 0; }
    };

    // Compute layout-quality metrics for a flat node/edge set. Pure — no side effects, no engine
    // state. GridSizePx is the grid the layout snaps to (used for the Grid sub-score); pass the
    // engine's grid (e.g. UBpirLayoutSettings::InternalGridPx) or a sensible default.
    PINWRIGHT_API FGraphLayoutMetricsResult ComputeGraphLayoutMetrics(
        const TArray<FNodeRect>& Nodes,
        const TArray<FGraphEdge>& Edges,
        double GridSizePx = 16.0);

    // ------------------------------------------------------------------------
    // Node-size adapter seam.
    //
    // Engine-agnostic interface so every layout engine / report can ask "how big is this node"
    // through one path. The concrete per-engine adapters (Blueprint here; material / anim / rig
    // adapter bodies land with their bounds-aware engine tickets) wrap their engine's real size
    // estimator. Node is the engine's node pointer as a UObject (every supported node type --
    // UEdGraphNode, UMaterialExpression, URigVMNode -- is UObject-derived); each adapter does a
    // checked Cast<> to its own concrete type, so a wrong-type pointer is detected, not blindly
    // reinterpreted.
    // ------------------------------------------------------------------------
    class PINWRIGHT_API INodeSizeAdapter
    {
    public:
        virtual ~INodeSizeAdapter() = default;

        // Estimated pixel size (width, height) of one node. Implementations must tolerate a null
        // or wrong-type pointer by returning a sane minimum rather than crashing (a checked Cast<>
        // on the UObject makes that recovery real).
        virtual FVector2D EstimateNodeSize(const UObject* Node) const = 0;
    };
}
