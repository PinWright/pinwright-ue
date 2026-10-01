// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwGraphLayout.h - PinWright's engine-agnostic layered node-graph formatter.
//
// The core works on an abstract graph (nodes with sizes and pin rows, wires from an output pin to
// an input pin), so it is testable without assets. Per-graph-type adapters (PwGraphLayoutEdGraph,
// PwGraphLayoutMaterial, PwGraphLayoutRigVM) build the model and write positions back.
//
// Algorithm, in order (standard layered-drawing techniques; see docs/bpir-compiler-internals.md):
//   1. Trees. Every root gets its own tree. Roots are the caller's explicit roots, then fixed nodes
//      that feed movable flow nodes, then movable flow sources, then any flow node left over (cycles).
//      A flow tree is grown depth-first along flow (exec) output pins in pin order; a wire that
//      reaches a node still on the DFS stack is a back edge and is ignored for layering.
//   2. Data blocks. Data-only nodes (no flow pins) are claimed by the first consumer in traversal
//      order (tree, then flow rank, then DFS preorder) and form that consumer's block: columns to
//      its left, one per dependency level (longest path), right-aligned, ordered by barycenter
//      sweeps. Unclaimed data nodes go to fixed consumers, then become roots of their own (sinks).
//   3. X. Flow nodes take longest-path X with real widths: the maximum over their flow predecessors
//      of (right edge + column gap), plus room for their own data block.
//   4. Y. Each node is first placed where its incoming wire is horizontal (pin aligned), then moved
//      down to the first slot where it and its exec spine (the chain of first flow children) are
//      free of everything already placed and every fixed node. The free slot is found in one
//      sorted sweep over the blocked intervals, so nothing can overlap and no push-down loop runs.
//      Trees are placed in root order; the first root moves only if it overlaps something itself.
//   5. Grid. X is snapped; Y is snapped wherever the node is not pin-aligned to its parent.
//
// Every tie breaks on pin index, then node Key, so the result never depends on input node order.

#pragma once

#include "CoreMinimal.h"
#include "Math/Vector2D.h"

namespace PwGraphLayout
{
    enum class EPinSide : uint8
    {
        Input,
        Output
    };

    // Flow = control flow (K2 exec, state-machine transition, RigVM execute context). Everything
    // else is data.
    enum class EWireKind : uint8
    {
        Flow,
        Data
    };

    struct PINWRIGHT_API FPinSlot
    {
        EPinSide Side = EPinSide::Input;
        EWireKind Kind = EWireKind::Data;
        // Pin centre, measured from the node's top edge.
        double OffsetY = 0.0;
    };

    struct PINWRIGHT_API FLayoutNode
    {
        // Stable identity (GUID, object path). Every ordering tie breaks on it.
        FString Key;
        // In: the node's current position. Out: the arranged position (movable nodes only).
        FVector2D Position = FVector2D::ZeroVector;
        FVector2D Size = FVector2D(160.0, 64.0);
        // Fixed nodes are never moved; they are obstacles, or anchors when listed as roots.
        bool bMovable = true;
        // True when Size came from a measured widget rather than an estimator.
        bool bMeasuredSize = false;
        // In the node's own pin order.
        TArray<FPinSlot> Pins;
    };

    // A wire runs from an output pin to an input pin (indices into Nodes and each node's Pins).
    struct PINWRIGHT_API FLayoutWire
    {
        int32 FromNode = INDEX_NONE;
        int32 FromPin = INDEX_NONE;
        int32 ToNode = INDEX_NONE;
        int32 ToPin = INDEX_NONE;
    };

    struct PINWRIGHT_API FLayoutGraph
    {
        TArray<FLayoutNode> Nodes;
        TArray<FLayoutWire> Wires;
        // Explicit roots in priority order (auto roots follow). A movable root keeps its X and its
        // Y, snapped to the grid. The first root moves down only if it overlaps a fixed node; later
        // roots move down until they and their exec spine clear fixed nodes and earlier trees.
        TArray<int32> Roots;
    };

    struct PINWRIGHT_API FSpacing
    {
        // Between a flow node's right edge and the next flow node (or its data block).
        double ColumnGap = 80.0;
        // Vertical clearance between any two nodes that share an X range.
        double RowGap = 32.0;
        // Between data columns, and between a data column and its consumer.
        double DataColumnGap = 48.0;
        double Grid = 16.0;
    };

    struct PINWRIGHT_API FArrangeReport
    {
        int32 Moved = 0;
        int32 Trees = 0;
        int32 MeasuredSizes = 0;
        int32 EstimatedSizes = 0;
    };

    // Arranges every movable node in place (Graph.Nodes[i].Position). Deterministic and
    // idempotent: a second call with the result moves nothing.
    PINWRIGHT_API FArrangeReport Arrange(FLayoutGraph& Graph, const FSpacing& Spacing);
}
