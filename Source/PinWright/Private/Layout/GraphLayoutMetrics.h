// Copyright (c) 2026 Alexander Penkin. MIT License.

// GraphLayoutMetrics.h - Engine-agnostic node-graph layout-quality metrics + node-size adapter seam.
//
// Pure geometry util: no editor RPC, no asset deps, no node-type coupling. Feed it a flat list of
// node rects ([{nodeId,x,y,w,h}]) plus edges and it returns layout-quality sub-scores, a combined
// 0-1 score and the backward edges. Callable from every layout engine and the bounds-aware engine
// work. Unit-testable in isolation.
//
// An edge with pin anchors (each end's pin centre measured from its node's top) is drawn pin to
// pin: from the source's output side to the target's input side, as the editor draws the wire.
// Straightness, crossings and the per-edge row delta use that segment. An edge without anchors
// falls back to its node centres, and the result says which basis was used. Wires are still
// straight segments, not routed splines.

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

    // A directed connection between two node ids, optionally anchored at its two pins.
    struct PINWRIGHT_API FGraphEdge
    {
        FString From;
        FString To;
        // Pin centres from the From / To node's top edge; used only when bHasPinAnchors.
        bool bHasPinAnchors = false;
        double FromPinY = 0.0;
        double ToPinY = 0.0;

        FGraphEdge() = default;
        FGraphEdge(const FString& InFrom, const FString& InTo) : From(InFrom), To(InTo) {}
        FGraphEdge(const FString& InFrom, double InFromPinY, const FString& InTo, double InToPinY)
            : From(InFrom), To(InTo), bHasPinAnchors(true), FromPinY(InFromPinY), ToPinY(InToPinY) {}
    };

    // Which way wires flow: outputs on the right edge, inputs on the left (every UE graph), or
    // the mirror. An edge whose target lies upstream of its source in this direction is backward.
    enum class EFlowDirection : uint8
    {
        LeftToRight,
        RightToLeft
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

        // Edges whose target's input side lies upstream of the source's output side (left of it,
        // for LeftToRight), so the wire has to run backwards. Self-edges are not counted.
        int32 BackwardEdgeCount = 0;
        TArray<FGraphEdge> BackwardEdges;
        // Parallel to the input edges: |source Y - target Y| on the edge's own basis (pin rows when
        // anchored, else centres); -1 when an endpoint id is unknown.
        TArray<double> PinRowDeltaPx;
        // Geometry basis of straightness / crossings: "pins" (every resolved edge anchored),
        // "centers" (none) or "mixed".
        FString GeometryBasis = TEXT("centers");

        bool HasOverlap() const { return OverlappingPairs.Num() > 0; }
    };

    // Compute layout-quality metrics for a flat node/edge set. Pure — no side effects, no engine
    // state. GridSizePx is the grid the layout snaps to (used for the Grid sub-score); pass the
    // engine's grid (e.g. UBpirLayoutSettings::GridSnapPx) or a sensible default.
    PINWRIGHT_API FGraphLayoutMetricsResult ComputeGraphLayoutMetrics(
        const TArray<FNodeRect>& Nodes,
        const TArray<FGraphEdge>& Edges,
        double GridSizePx = 16.0,
        EFlowDirection Flow = EFlowDirection::LeftToRight);

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
