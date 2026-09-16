// Copyright (c) 2026 Alexander Penkin. MIT License.

// NodeLayoutEngine.h - Node size estimation and graph layout for BPIR compiler

#pragma once
#include "CoreMinimal.h"
#include "Math/Vector2D.h"
#include "Layout/SlateRect.h"
#include "EdGraph/EdGraphPin.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

class UEdGraphNode;
class UBpirLayoutSettings;

namespace BpirLayout { class FNodeLayoutParameterFormatter; }

namespace BpirLayout
{
    // Estimate the pixel bounds of an EdGraph node for layout purposes,
    // using live Slate font measurement. Works headless (no rendered widget).
    PINWRIGHT_API FVector2D EstimateNodeSize(const UEdGraphNode* Node, const UBpirLayoutSettings& Settings);

    // True when a node has no exec-category pins in either direction.
    PINWRIGHT_API bool IsPureNode(const UEdGraphNode* Node);

    // Grid-rounding helpers (negative-safe integer arithmetic).
    PINWRIGHT_API int32 FloorToGrid(int32 Value, int32 Grid);
    PINWRIGHT_API int32 CeilToGrid(int32 Value, int32 Grid);
    PINWRIGHT_API int32 RoundToGrid(int32 Value, int32 Grid);

    // ------------------------------------------------------------------------
    // Cluster bounds registry — populated by FormatParameterNodes,
    // queried by GetNodeBounds when bUseClusterBounds is true.
    // ------------------------------------------------------------------------
    struct PINWRIGHT_API FClusterBoundsRegistry
    {
        TMap<const UEdGraphNode*, FSlateRect> ClusterRectByConsumer;

        void Register(const UEdGraphNode* Consumer, const FSlateRect& Rect);
        bool Has(const UEdGraphNode* Consumer) const;
        FSlateRect Get(const UEdGraphNode* Consumer) const;
    };

    // Returns the node's bounding rect. If bUseClusterBounds and the node has a
    // registered cluster overlay, extends the rect to cover consumer + parameter subtree.
    PINWRIGHT_API FSlateRect GetNodeBounds(
        const UEdGraphNode* Node,
        const UBpirLayoutSettings& Settings,
        bool bUseClusterBounds = false,
        const FClusterBoundsRegistry* ClusterRegistry = nullptr);

    // ------------------------------------------------------------------------
    // FormatX adjacency walk (builds the parent/child tree used by later passes).
    // ------------------------------------------------------------------------

    // Directed connection between two pins. Direction follows FromPin's direction:
    // EGPD_Output means we are walking the "child of an output" branch, EGPD_Input the reverse.
    struct PINWRIGHT_API FPinLink
    {
        UEdGraphPin* FromPin = nullptr;
        UEdGraphPin* ToPin = nullptr;

        EEdGraphPinDirection GetDirection() const;
        UEdGraphNode* GetFromNode() const;
        UEdGraphNode* GetToNode() const;

        bool IsValid() const { return FromPin != nullptr && ToPin != nullptr; }

        bool operator==(const FPinLink& Other) const;
        friend PINWRIGHT_API uint32 GetTypeHash(const FPinLink& Link);
    };

    struct FFormatXInfo;
    using FFormatXInfoPtr = TSharedPtr<FFormatXInfo>;

    // Parent/child relationship in the formatted tree built during FormatX traversal.
    struct FFormatXInfo
    {
        UEdGraphNode* Node = nullptr;
        FPinLink LinkFromParent;                  // invalid for root
        TWeakPtr<FFormatXInfo> Parent;
        TArray<FFormatXInfoPtr> Children;
        bool bSameRowAsParent = false;
        bool bIsRoot = false;
    };

    // Result of the adjacency walk: maps every reachable node in the pool to its FormatXInfo.
    struct PINWRIGHT_API FFormatXInfoMap
    {
        TMap<UEdGraphNode*, FFormatXInfoPtr> Infos;
        FFormatXInfoPtr RootInfo;

        FFormatXInfoPtr Find(UEdGraphNode* Node) const;
    };

    // Walks outward from AnchorNode through exec pin connections using the dual-stack
    // output/input alternating BFS. Restricts traversal to NodesInPool (ignores external nodes).
    // Respects the iteration cap from settings to prevent runaway.
    PINWRIGHT_API FFormatXInfoMap BuildFormatXInfoMap(
        UEdGraphNode* AnchorNode,
        const TSet<UEdGraphNode*>& NodesInPool,
        const UBpirLayoutSettings& Settings);

    // Computes the X coordinate at which Child should be placed relative to Parent,
    // following the BA formula (see .cpp for derivation). LargerBounds = cluster-extended
    // bounds of the Child when bUseClusterBounds is true (covers the child's own parameter
    // subtree); ChildBounds = always bare; ParentBounds = cluster-extended when
    // bUseClusterBounds is true. Result is directionally grid-aligned to Settings.InternalGridPx
    // (floor for EGPD_Input, ceil for EGPD_Output). Returns integer X for Child->NodePosX.
    PINWRIGHT_API int32 GetChildX(
        const UEdGraphNode* Parent,
        const UEdGraphNode* Child,
        EEdGraphPinDirection Direction,
        const UBpirLayoutSettings& Settings,
        bool bUseClusterBounds,
        const FClusterBoundsRegistry* ClusterRegistry);

    // Walks the FormatXInfoMap and marks the FIRST exec-output child of each parent
    // as same-row (sets FFormatXInfo::bSameRowAsParent=true). "First" is determined
    // by iterating the parent's pins in Node->Pins order, then LinkedTo order; the
    // first linked child whose LinkFromParent matches (i.e. the child was actually
    // adopted by *this* parent through *this* pin) wins. One same-row child per parent.
    //
    // The first exec child continues the execution thread at the parent's Y level;
    // later siblings stack below. Must run AFTER BuildFormatXInfoMap.
    PINWRIGHT_API void GetPinsOfSameHeight(FFormatXInfoMap& InfoMap);

    // Applies GetChildX across every non-root entry in the FormatXInfoMap, in topological
    // order (parent before child). Root NodePosX is left untouched so the layout stays anchored.
    // Pass 1 uses bUseClusterBounds=false / ClusterRegistry=nullptr; pass 2 (run after the
    // parameter formatter has populated cluster rects) uses bUseClusterBounds=true.
    PINWRIGHT_API void FormatX(
        const FFormatXInfoMap& InfoMap,
        const UBpirLayoutSettings& Settings,
        bool bUseClusterBounds,
        const FClusterBoundsRegistry* ClusterRegistry);

    // ------------------------------------------------------------------------
    // FormatParameterNodes — glue pass between FormatX pass 1 and pass 2.
    //
    // Iterates the impure nodes in InfoMap (BFS from RootInfo through Children),
    // instantiating an FNodeLayoutParameterFormatter per consumer. The first
    // consumer to encounter a given pure node claims it; later consumers see it
    // in their IgnoredNodes set and skip it (first-wins rule driven by BFS order).
    //
    // Outputs:
    //   OutRegistry        — cluster rect per consumer that actually placed pures
    //   OutPlacedPureNodes — union of every pure that got claimed (used by grid-snap
    //                        pass to skip already-positioned nodes)
    //   OutFormatters      — owned formatters retained so callers can inspect placed
    //                        pures / cluster bounds later; formatters that found no
    //                        pures are discarded and not added here.
    // ------------------------------------------------------------------------
    PINWRIGHT_API void FormatParameterNodes(
        const FFormatXInfoMap& InfoMap,
        const TSet<UEdGraphNode*>& NodesInPool,
        const UBpirLayoutSettings& Settings,
        FClusterBoundsRegistry& OutRegistry,
        TSet<UEdGraphNode*>& OutPlacedPureNodes,
        TArray<TUniquePtr<FNodeLayoutParameterFormatter>>& OutFormatters);

    // Re-register each consumer's current cluster bounds into Registry. Call after any
    // pass that translates clusters (e.g. ResolveConsumerClusterOverlaps) so the
    // registry reflects post-translation positions rather than stale FormatX-era bounds.
    // Bounds are recomputed live from each consumer's and its pures' current NodePosX/Y
    // rather than the Formatter's cached ClusterBounds (which is frozen at Format() time).
    PINWRIGHT_API void SyncRegistryFromFormatters(
        const TMap<UEdGraphNode*, FNodeLayoutParameterFormatter*>& FormatterByConsumer,
        FClusterBoundsRegistry& Registry,
        const UBpirLayoutSettings& Settings);

    // ------------------------------------------------------------------------
    // FormatY — vertical placement pass with iterative collision resolution.
    //
    // Walks InfoMap as a tree rooted at RootInfo in DFS pre-order. For each
    // non-root node:
    //   * same-row child: inherits parent's NodePosY exactly.
    //   * non-same-row child: stacked below the previous sibling, with
    //     Settings.NodePadY between their bounding-box bottoms and tops; falls
    //     back to parent's Y when no placed previous sibling exists.
    //
    // After tentative placement, a collision loop scans every already-placed
    // node (plus ExternalObstacles) and — on AABB overlap — jumps the current
    // node's NodePosY just past the blocker's bottom. Cluster-extended bounds
    // are used so parameter columns count as part of their consumer. Loop caps
    // at Settings.CollisionIterationCap; log + move on if exceeded.
    //
    // ExternalObstacles are nodes outside InfoMap that must be respected (e.g.
    // a sibling chain that wasn't part of this traversal). They contribute to
    // collision checks but are never repositioned.
    //
    // The root's Y is preserved (it acts as the layout anchor).
    // ------------------------------------------------------------------------
    // OutRegistry (optional): if provided, FormatY refreshes each consumer's
    // cluster rect in the registry to match its post-translation position, so
    // callers observing bounds via GetNodeBounds(..., bUseClusterBounds=true,
    // Registry) see live positions rather than stale FormatX-era bounds.
    PINWRIGHT_API void FormatY(
        FFormatXInfoMap& InfoMap,
        const TSet<UEdGraphNode*>& ExternalObstacles,
        const UBpirLayoutSettings& Settings,
        const TMap<UEdGraphNode*, FNodeLayoutParameterFormatter*>& FormatterByConsumer,
        FClusterBoundsRegistry* OutRegistry = nullptr);

    // ------------------------------------------------------------------------
    // Finalization passes.
    // ------------------------------------------------------------------------

    // Translates every node in the pool by (SavedAnchorPos - CurrentAnchorPos) so
    // AnchorNode lands back at the saved position. Pools that don't contain
    // AnchorNode (or have AnchorNode at the saved position already) are no-ops.
    PINWRIGHT_API void ResetRelativeToAnchor(
        const TSet<UEdGraphNode*>& PoolNodes,
        UEdGraphNode* AnchorNode,
        FIntPoint SavedAnchorPos);

    // Applies directional grid rounding to every node's X/Y in the pool:
    //   - X: Floor for nodes reached via EGPD_Input direction, Ceil for EGPD_Output
    //        direction, Round otherwise (root or unknown-direction).
    //   - Y: Round.
    // Uses Settings.InternalGridPx. Also snaps the anchor itself (Round both axes)
    // so the anchor lands exactly on the grid after ResetRelativeToAnchor ran.
    //
    // InfoMap tells us each non-root node's incoming link direction via
    // LinkFromParent.GetDirection(); pass it in for lookup. Nodes not in InfoMap
    // (e.g. frozen obstacles — but they aren't in the pool either) are skipped.
    PINWRIGHT_API void SnapToGrid(
        const TSet<UEdGraphNode*>& PoolNodes,
        UEdGraphNode* AnchorNode,
        const FFormatXInfoMap& InfoMap,
        const UBpirLayoutSettings& Settings);

    // ------------------------------------------------------------------------
    // FNodeLayoutEngine — top-level driver that runs the full layout pipeline.
    //
    // Pipeline order (matches the plan's "Algorithm" section):
    //   1. Save AnchorNode's original position.
    //   2. BuildFormatXInfoMap.
    //   3. FormatX pass 1 (no cluster bounds).
    //   4. FormatParameterNodes — builds cluster registry, repositions pures.
    //   5. FormatX pass 2 (with cluster bounds).
    //   6. GetPinsOfSameHeight — populates SameRowMapping + bSameRowAsParent.
    //   7. FormatY — recursive placement with collision resolution.
    //   8. ResetRelativeToAnchor — translate whole layout so anchor stays put.
    //   9. SnapToGrid — directional rounding to InternalGridPx.
    //
    // Honors Settings.bEnableBpirLayoutPass — when false, Format() is a no-op.
    // Plain C++ class (not a UObject) — no reflection, no UCLASS needed.
    // ------------------------------------------------------------------------
    class PINWRIGHT_API FNodeLayoutEngine
    {
    public:
        FNodeLayoutEngine(
            UEdGraph* InGraph,
            TArray<UEdGraphNode*> InPool,
            UEdGraphNode* InAnchor,
            TArray<UEdGraphNode*> InExternalObstacles = {});

        // Runs the full pipeline. Reads settings from GetDefault<UBpirLayoutSettings>().
        // Safe to call on empty or anchor-less pools (no-op with verbose log).
        void Format();

    private:
        UEdGraph* Graph;
        TSet<UEdGraphNode*> Pool;
        UEdGraphNode* Anchor;
        TSet<UEdGraphNode*> ExternalObstacles;
    };
}
