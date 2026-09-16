// Copyright (c) 2026 Alexander Penkin. MIT License.

// NodeLayoutEngine.cpp - Node size estimation using live Slate font measurement

#include "Compiler/NodeLayoutEngine.h"
#include "Compat/EngineVersionCompat.h"
#include "Compiler/NodeLayoutParameterFormatter.h"

#include "CoreMinimal.h"
#include "Styling/AppStyle.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_MacroInstance.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "BpirLayoutSettings.h"

DEFINE_LOG_CATEGORY_STATIC(LogBpirLayout, Log, All);

namespace BpirLayout
{
    namespace
    {
        // Constants tuned for typical Blueprint node rendering in the graph editor.
        constexpr float MinWidthPx = 160.0f;
        constexpr float MinHeightPx = 64.0f;
        constexpr float ColumnGapPx = 40.0f;
        constexpr float HeaderBiasPx = 16.0f;
        constexpr float FallbackTitleCharPx = 8.0f;
        constexpr float FallbackPinCharPx = 7.0f;

        // Fallback path when Slate is not initialized (headless / commandlet without renderer).
        // Warns once so repeated calls don't spam the log.
        float FallbackMeasureWidth(const FString& Text, float PxPerChar)
        {
            static bool bWarnedOnce = false;
            if (!bWarnedOnce)
            {
                UE_LOG(LogBpirLayout, Verbose,
                    TEXT("Slate font measurement unavailable; using char-count fallback for node size estimation."));
                bWarnedOnce = true;
            }
            return static_cast<float>(Text.Len()) * PxPerChar;
        }

        // Returns true if Slate application + renderer are available for measurement.
        bool IsFontMeasureAvailable()
        {
            if (!FSlateApplication::IsInitialized())
            {
                return false;
            }
            return FSlateApplication::Get().GetRenderer() != nullptr;
        }

        // Measures text width via Slate if available, else falls back to char count.
        float MeasureTextWidth(const FString& Text, const FSlateFontInfo& Font, float FallbackPxPerChar, bool bSlateAvailable)
        {
            if (Text.IsEmpty())
            {
                return 0.0f;
            }
            if (!bSlateAvailable)
            {
                return FallbackMeasureWidth(Text, FallbackPxPerChar);
            }
            const TSharedRef<FSlateFontMeasure> FontMeasure =
                FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
            const FVector2D Size = FontMeasure->Measure(Text, Font);
            return static_cast<float>(Size.X);
        }

        // Returns a pin's display label, falling back to the raw pin name when unset.
        FString GetPinLabel(const UEdGraphPin* Pin)
        {
            if (!Pin)
            {
                return FString();
            }
            const FString Display = Pin->GetDisplayName().ToString();
            if (!Display.IsEmpty())
            {
                return Display;
            }
            return Pin->PinName.ToString();
        }

        // True for node classes that traditionally render a taller header (event roots, comments).
        bool NodeHasLargeHeader(const UEdGraphNode* Node)
        {
            if (!Node) return false;
            return Node->IsA<UK2Node_Event>()
                || Node->IsA<UK2Node_CustomEvent>()
                || Node->IsA<UK2Node_FunctionEntry>()
                || Node->IsA<UK2Node_MacroInstance>()
                || Node->IsA<UEdGraphNode_Comment>();
        }

        FSlateRect GetCurrentClusterBounds(
            UEdGraphNode* Consumer,
            const TMap<UEdGraphNode*, FNodeLayoutParameterFormatter*>& FormatterByConsumer,
            const UBpirLayoutSettings& Settings)
        {
            FSlateRect Bounds = GetNodeBounds(Consumer, Settings, /*bUseClusterBounds=*/false, nullptr);
            if (const FNodeLayoutParameterFormatter* const* Formatter = FormatterByConsumer.Find(Consumer))
            {
                for (UEdGraphNode* PureNode : (*Formatter)->GetPlacedPureNodes())
                {
                    if (!PureNode)
                    {
                        continue;
                    }
                    Bounds = Bounds.Expand(GetNodeBounds(PureNode, Settings, /*bUseClusterBounds=*/false, nullptr));
                }
            }
            return Bounds;
        }

        void TranslateCluster(
            UEdGraphNode* Consumer,
            const TMap<UEdGraphNode*, FNodeLayoutParameterFormatter*>& FormatterByConsumer,
            int32 DeltaY)
        {
            if (!Consumer || DeltaY == 0)
            {
                return;
            }

            Consumer->NodePosY += DeltaY;
            if (const FNodeLayoutParameterFormatter* const* Formatter = FormatterByConsumer.Find(Consumer))
            {
                for (UEdGraphNode* PureNode : (*Formatter)->GetPlacedPureNodes())
                {
                    if (PureNode)
                    {
                        PureNode->NodePosY += DeltaY;
                    }
                }
            }
        }

        // BFS over the exec tree, pushing each consumer node down until its
        // cluster no longer overlaps any previously-placed cluster.
        // Same-row nodes are skipped — their Y was set intentionally by FormatY
        // and must not be displaced.
        void ResolveConsumerClusterOverlaps(
            const FFormatXInfoMap& InfoMap,
            const TMap<UEdGraphNode*, FNodeLayoutParameterFormatter*>& FormatterByConsumer,
            const UBpirLayoutSettings& Settings)
        {
            if (!InfoMap.RootInfo.IsValid())
            {
                return;
            }

            TArray<FFormatXInfoPtr> Queue;
            Queue.Add(InfoMap.RootInfo);

            // Placed consumers and their cached bounds are kept in parallel arrays.
            // Bounds of already-placed consumers never change (only the *current*
            // consumer is translated), so each placed cluster's bounds are computed
            // at most twice: once when translated into its final position, once when
            // pushed onto the obstacle list. GetCurrentClusterBounds iterates pins
            // and measures Slate text — caching avoids an O(N^2) Slate-measure cost.
            TArray<UEdGraphNode*> PlacedConsumers;
            TArray<FSlateRect> PlacedBounds;
            int32 Head = 0;
            while (Head < Queue.Num())
            {
                const FFormatXInfoPtr Info = Queue[Head++];
                if (!Info.IsValid() || !Info->Node)
                {
                    continue;
                }

                for (const FFormatXInfoPtr& Child : Info->Children)
                {
                    if (Child.IsValid())
                    {
                        Queue.Add(Child);
                    }
                }

                UEdGraphNode* Consumer = Info->Node;
                if (!Info->bIsRoot && !Info->bSameRowAsParent)
                {
                    const int32 IterationCap = FMath::Max(1, Settings.CollisionIterationCap);
                    for (int32 Iteration = 0; Iteration < IterationCap; ++Iteration)
                    {
                        const FSlateRect CurrentBounds = GetCurrentClusterBounds(Consumer, FormatterByConsumer, Settings);
                        float LowestBlockingBottom = -FLT_MAX;

                        for (const FSlateRect& Blocker : PlacedBounds)
                        {
                            if (FSlateRect::DoRectanglesIntersect(CurrentBounds, Blocker))
                            {
                                LowestBlockingBottom = FMath::Max(LowestBlockingBottom, Blocker.Bottom);
                            }
                        }

                        if (LowestBlockingBottom == -FLT_MAX)
                        {
                            break;
                        }

                        const int32 DeltaY = FMath::RoundToInt(
                            LowestBlockingBottom + static_cast<float>(Settings.NodePadY) - CurrentBounds.Top);
                        TranslateCluster(Consumer, FormatterByConsumer, DeltaY);
                    }
                }

                // Same-row nodes skip self-resolution (FormatY already placed them),
                // but still register as obstacles for their BFS-later descendants.
                PlacedConsumers.Add(Consumer);
                PlacedBounds.Add(GetCurrentClusterBounds(Consumer, FormatterByConsumer, Settings));
            }
        }

    }

    // Shared helpers declared in NodeLayoutEngine.h

    bool IsPureNode(const UEdGraphNode* Node)
    {
        if (!Node) return false;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                return false;
            }
        }
        return true;
    }

    int32 FloorToGrid(int32 Value, int32 Grid)
    {
        const int32 G = FMath::Max(1, Grid);
        const int32 Q = (Value >= 0) ? (Value / G) : -(((-Value) + G - 1) / G);
        return Q * G;
    }

    int32 CeilToGrid(int32 Value, int32 Grid)
    {
        const int32 G = FMath::Max(1, Grid);
        const int32 Q = (Value >= 0) ? ((Value + G - 1) / G) : -((-Value) / G);
        return Q * G;
    }

    int32 RoundToGrid(int32 Value, int32 Grid)
    {
        const int32 G = FMath::Max(1, Grid);
        return FMath::RoundToInt(static_cast<float>(Value) / static_cast<float>(G)) * G;
    }

    FVector2D EstimateNodeSize(const UEdGraphNode* Node, const UBpirLayoutSettings& Settings)
    {
        if (!Node)
        {
            return FVector2D(MinWidthPx, MinHeightPx);
        }

        // Cache font info and Slate availability to avoid repeated global lookups.
        static const FSlateFontInfo TitleFont = FAppStyle::Get().GetFontStyle(TEXT("Graph.Node.NodeTitle"));
        static const FSlateFontInfo PinFont = FAppStyle::Get().GetFontStyle(TEXT("Graph.Node.PinName"));
        const bool bSlateAvailable = IsFontMeasureAvailable();

        // Title width
        const FString TitleText = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
        const float TitleWidth = MeasureTextWidth(TitleText, TitleFont, FallbackTitleCharPx, bSlateAvailable);

        // Split visible pins into input / output columns, preserving order.
        TArray<const UEdGraphPin*> InputPins;
        TArray<const UEdGraphPin*> OutputPins;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->bHidden)
            {
                continue;
            }
            if (Pin->Direction == EGPD_Input)
            {
                InputPins.Add(Pin);
            }
            else if (Pin->Direction == EGPD_Output)
            {
                OutputPins.Add(Pin);
            }
        }

        const int32 RowCount = FMath::Max(InputPins.Num(), OutputPins.Num());

        // Largest input+output label pair across all rows.
        float MaxRowWidth = 0.0f;
        for (int32 Row = 0; Row < RowCount; ++Row)
        {
            const FString InLabel = (Row < InputPins.Num()) ? GetPinLabel(InputPins[Row]) : FString();
            const FString OutLabel = (Row < OutputPins.Num()) ? GetPinLabel(OutputPins[Row]) : FString();

            const float InWidth = MeasureTextWidth(InLabel, PinFont, FallbackPinCharPx, bSlateAvailable);
            const float OutWidth = MeasureTextWidth(OutLabel, PinFont, FallbackPinCharPx, bSlateAvailable);
            const float RowWidth = InWidth + OutWidth + ColumnGapPx;
            MaxRowWidth = FMath::Max(MaxRowWidth, RowWidth);
        }

        const float HorizontalPadding = 2.0f * static_cast<float>(Settings.HorizontalPaddingPx);
        float Width = FMath::Max(TitleWidth, MaxRowWidth) + HorizontalPadding;
        Width = FMath::Max(Width, MinWidthPx);

        // Height: header + pin rows + trailing footer pad for breathing room.
        float Header = static_cast<float>(Settings.HeaderHeightPx);
        if (NodeHasLargeHeader(Node))
        {
            Header += HeaderBiasPx;
        }
        const float RowsHeight = static_cast<float>(RowCount) * static_cast<float>(Settings.PinRowHeightPx);
        const float Footer = static_cast<float>(Settings.PinRowHeightPx);
        float Height = Header + RowsHeight + Footer;
        Height = FMath::Max(Height, MinHeightPx);

        return FVector2D(Width, Height);
    }

    // ------------------------------------------------------------------------
    // FClusterBoundsRegistry
    // ------------------------------------------------------------------------

    void FClusterBoundsRegistry::Register(const UEdGraphNode* Consumer, const FSlateRect& Rect)
    {
        if (!Consumer)
        {
            return;
        }
        ClusterRectByConsumer.Add(Consumer, Rect);
    }

    bool FClusterBoundsRegistry::Has(const UEdGraphNode* Consumer) const
    {
        return Consumer != nullptr && ClusterRectByConsumer.Contains(Consumer);
    }

    FSlateRect FClusterBoundsRegistry::Get(const UEdGraphNode* Consumer) const
    {
        if (!Consumer)
        {
            return FSlateRect();
        }
        if (const FSlateRect* Found = ClusterRectByConsumer.Find(Consumer))
        {
            return *Found;
        }
        return FSlateRect();
    }

    void SyncRegistryFromFormatters(
        const TMap<UEdGraphNode*, FNodeLayoutParameterFormatter*>& FormatterByConsumer,
        FClusterBoundsRegistry& Registry,
        const UBpirLayoutSettings& Settings)
    {
        // Recompute cluster bounds from the consumer's and its pures' CURRENT
        // node positions. The cached ClusterBounds inside FNodeLayoutParameterFormatter
        // is frozen at Format() time and does not track subsequent TranslateCluster
        // calls (FormatY / ResolveConsumerClusterOverlaps), so using it here would
        // re-stamp stale pre-translation Y coordinates into the registry and leave
        // GetNodeBounds(..., bUseClusterBounds=true, ...) returning a vertically
        // over-extended rect that unions current and pre-translation positions.
        for (const TPair<UEdGraphNode*, FNodeLayoutParameterFormatter*>& Pair : FormatterByConsumer)
        {
            UEdGraphNode* Consumer = Pair.Key;
            FNodeLayoutParameterFormatter* Formatter = Pair.Value;
            if (!Consumer || !Formatter)
            {
                continue;
            }
            Registry.Register(Consumer, GetCurrentClusterBounds(Consumer, FormatterByConsumer, Settings));
        }
    }

    // ------------------------------------------------------------------------
    // GetNodeBounds
    // ------------------------------------------------------------------------

    FSlateRect GetNodeBounds(
        const UEdGraphNode* Node,
        const UBpirLayoutSettings& Settings,
        bool bUseClusterBounds,
        const FClusterBoundsRegistry* ClusterRegistry)
    {
        if (!Node)
        {
            return FSlateRect(0, 0, 0, 0);
        }

        const FVector2D Size = EstimateNodeSize(Node, Settings);
        const float Left = static_cast<float>(Node->NodePosX);
        const float Top = static_cast<float>(Node->NodePosY);
        const float Right = Left + static_cast<float>(Size.X);
        const float Bottom = Top + static_cast<float>(Size.Y);

        FSlateRect BareRect(Left, Top, Right, Bottom);

        if (bUseClusterBounds && ClusterRegistry && ClusterRegistry->Has(Node))
        {
            // Union with the registered cluster rect. Don't mutate the registry.
            const FSlateRect ClusterRect = ClusterRegistry->Get(Node);
            return BareRect.Expand(ClusterRect);
        }

        return BareRect;
    }

    // ------------------------------------------------------------------------
    // FPinLink
    // ------------------------------------------------------------------------

    EEdGraphPinDirection FPinLink::GetDirection() const
    {
        // Fall back to EGPD_Output for invalid links; callers should check IsValid() first.
        return FromPin ? FromPin->Direction.GetValue() : EGPD_Output;
    }

    UEdGraphNode* FPinLink::GetFromNode() const
    {
        return FromPin ? FromPin->GetOwningNodeUnchecked() : nullptr;
    }

    UEdGraphNode* FPinLink::GetToNode() const
    {
        return ToPin ? ToPin->GetOwningNodeUnchecked() : nullptr;
    }

    bool FPinLink::operator==(const FPinLink& Other) const
    {
        // Directional equality: (A->B) is NOT equal to (B->A).
        return FromPin == Other.FromPin && ToPin == Other.ToPin;
    }

    uint32 GetTypeHash(const FPinLink& Link)
    {
        // Hash both pin pointers; order matters so (A,B) hashes differently from (B,A).
        return HashCombine(GetTypeHash(Link.FromPin), GetTypeHash(Link.ToPin));
    }

    // ------------------------------------------------------------------------
    // FFormatXInfoMap
    // ------------------------------------------------------------------------

    FFormatXInfoPtr FFormatXInfoMap::Find(UEdGraphNode* Node) const
    {
        if (!Node)
        {
            return nullptr;
        }
        if (const FFormatXInfoPtr* Found = Infos.Find(Node))
        {
            return *Found;
        }
        return nullptr;
    }

    // ------------------------------------------------------------------------
    // BuildFormatXInfoMap — adjacency walk (dual-stack output/input alternating BFS)
    // ------------------------------------------------------------------------

    namespace
    {
        // True when a pin is a visible exec-category pin.
        bool IsExecPin(const UEdGraphPin* Pin)
        {
            return Pin != nullptr
                && !Pin->bHidden
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
        }

        // Seed-and-expand helper: push every exec-pin link originating from Node onto
        // the appropriate stack (output links -> OutputStack, input links -> InputStack),
        // restricted to nodes in the pool.
        void ExpandExecLinksFromNode(
            UEdGraphNode* Node,
            const TSet<UEdGraphNode*>& NodesInPool,
            TArray<FPinLink>& OutputStack,
            TArray<FPinLink>& InputStack)
        {
            if (!Node)
            {
                return;
            }
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!IsExecPin(Pin))
                {
                    continue;
                }
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    if (!Linked)
                    {
                        continue;
                    }
                    UEdGraphNode* OtherNode = Linked->GetOwningNodeUnchecked();
                    if (!OtherNode || !NodesInPool.Contains(OtherNode))
                    {
                        continue;
                    }
                    FPinLink Link;
                    Link.FromPin = Pin;
                    Link.ToPin = Linked;
                    if (Pin->Direction == EGPD_Output)
                    {
                        OutputStack.Push(Link);
                    }
                    else
                    {
                        InputStack.Push(Link);
                    }
                }
            }
        }

        // True if Candidate wins over Existing for the given walk direction.
        // EGPD_Output: prefer the parent with the LARGER NodePosX (rightmost upstream).
        // EGPD_Input:  prefer the parent with the SMALLER NodePosX (leftmost upstream).
        // Ties keep Existing.
        bool BetterParent(UEdGraphNode* Candidate, UEdGraphNode* Existing, EEdGraphPinDirection Direction)
        {
            if (!Candidate)
            {
                return false;
            }
            if (!Existing)
            {
                return true;
            }
            if (Direction == EGPD_Output)
            {
                return Candidate->NodePosX > Existing->NodePosX;
            }
            return Candidate->NodePosX < Existing->NodePosX;
        }

        // Walk Candidate's parent chain; return true if PotentialAncestor appears.
        // Guards null weak-ptr parents and a hard iteration cap in case of corrupted trees.
        bool IsDescendantOf(
            UEdGraphNode* Candidate,
            UEdGraphNode* PotentialAncestor,
            const TMap<UEdGraphNode*, FFormatXInfoPtr>& Infos)
        {
            if (!Candidate || !PotentialAncestor)
            {
                return false;
            }
            const FFormatXInfoPtr* StartInfo = Infos.Find(Candidate);
            if (!StartInfo || !StartInfo->IsValid())
            {
                return false;
            }
            // Walk upward, guarding against circular parent chains.
            FFormatXInfoPtr Current = *StartInfo;
            int32 Guard = 0;
            constexpr int32 MaxChainDepth = 10000;
            while (Current.IsValid() && Guard++ < MaxChainDepth)
            {
                if (Current->Node == PotentialAncestor)
                {
                    return true;
                }
                Current = Current->Parent.Pin();
            }
            return false;
        }
    }

    FFormatXInfoMap BuildFormatXInfoMap(
        UEdGraphNode* AnchorNode,
        const TSet<UEdGraphNode*>& NodesInPool,
        const UBpirLayoutSettings& Settings)
    {
        FFormatXInfoMap Result;
        if (!AnchorNode)
        {
            return Result;
        }

        // Seed: root info for the anchor.
        FFormatXInfoPtr RootInfo = MakeShared<FFormatXInfo>();
        RootInfo->Node = AnchorNode;
        RootInfo->bIsRoot = true;
        Result.RootInfo = RootInfo;
        Result.Infos.Add(AnchorNode, RootInfo);

        TArray<FPinLink> OutputStack;
        TArray<FPinLink> InputStack;
        TSet<FPinLink> VisitedLinks;

        // Seed both stacks with exec links emanating from the anchor.
        ExpandExecLinksFromNode(AnchorNode, NodesInPool, OutputStack, InputStack);

        int32 Iterations = 0;
        const int32 IterationCap = FMath::Max(1, Settings.TraversalIterationCap);

        while ((OutputStack.Num() > 0 || InputStack.Num() > 0) && Iterations < IterationCap)
        {
            ++Iterations;

            // Output stack takes priority so we finish the "forward" flow before reversing.
            FPinLink Link = (OutputStack.Num() > 0) ? OutputStack.Pop(EAllowShrinking::No)
                                                   : InputStack.Pop(EAllowShrinking::No);
            if (VisitedLinks.Contains(Link))
            {
                continue;
            }
            VisitedLinks.Add(Link);

            UEdGraphNode* ChildNode = Link.GetToNode();
            UEdGraphNode* ParentNode = Link.GetFromNode();
            if (!ChildNode || !ParentNode || !NodesInPool.Contains(ChildNode))
            {
                continue;
            }

            FFormatXInfoPtr* ParentInfoPtr = Result.Infos.Find(ParentNode);
            if (!ParentInfoPtr || !ParentInfoPtr->IsValid())
            {
                // Shouldn't happen — parent must already be recorded since we expand from it.
                continue;
            }
            FFormatXInfoPtr ParentInfo = *ParentInfoPtr;

            FFormatXInfoPtr* ChildInfoPtr = Result.Infos.Find(ChildNode);
            if (!ChildInfoPtr || !ChildInfoPtr->IsValid())
            {
                // First visit: assign this parent and record the link.
                FFormatXInfoPtr ChildInfo = MakeShared<FFormatXInfo>();
                ChildInfo->Node = ChildNode;
                ChildInfo->LinkFromParent = Link;
                ChildInfo->Parent = ParentInfo;
                Result.Infos.Add(ChildNode, ChildInfo);
                ParentInfo->Children.Add(ChildInfo);
            }
            else
            {
                FFormatXInfoPtr ChildInfo = *ChildInfoPtr;

                // Cycle check: don't re-parent a child under one of its own descendants.
                if (IsDescendantOf(ParentNode, ChildNode, Result.Infos))
                {
                    continue;
                }

                UEdGraphNode* ExistingParentNode = ChildInfo->LinkFromParent.GetFromNode();
                if (BetterParent(ParentNode, ExistingParentNode, Link.GetDirection()))
                {
                    // Detach from old parent.
                    if (FFormatXInfoPtr* OldParentInfoPtr = Result.Infos.Find(ExistingParentNode))
                    {
                        if (OldParentInfoPtr->IsValid())
                        {
                            (*OldParentInfoPtr)->Children.RemoveAll([&ChildInfo](const FFormatXInfoPtr& Existing)
                            {
                                return Existing == ChildInfo;
                            });
                        }
                    }
                    // Reattach under the new parent.
                    ChildInfo->LinkFromParent = Link;
                    ChildInfo->Parent = ParentInfo;
                    ParentInfo->Children.Add(ChildInfo);
                }
            }

            // Expand outward from the child regardless of re-parent outcome so we
            // see all reachable nodes exactly once (VisitedLinks suppresses dupes).
            ExpandExecLinksFromNode(ChildNode, NodesInPool, OutputStack, InputStack);
        }

        if (Iterations >= IterationCap && (OutputStack.Num() > 0 || InputStack.Num() > 0))
        {
            UE_LOG(LogBpirLayout, Verbose,
                TEXT("BPIR layout traversal hit iteration cap at %d; graph may be unusually deep or contain a cycle BPIR shouldn't produce"),
                Iterations);
        }

        return Result;
    }

    // ------------------------------------------------------------------------
    // GetPinsOfSameHeight — set bSameRowAsParent on the first exec child.
    //
    // For each parent in BFS order from the root, find the first exec-output pin
    // (in Node->Pins iteration order) whose linked child was actually adopted by
    // this parent through this pin during BuildFormatXInfoMap. Mark that single
    // child as same-row. A parent's remaining exec children remain stacked.
    // ------------------------------------------------------------------------

    void GetPinsOfSameHeight(FFormatXInfoMap& InfoMap)
    {
        if (!InfoMap.RootInfo.IsValid())
        {
            return;
        }

        TArray<FFormatXInfoPtr> Queue;
        Queue.Add(InfoMap.RootInfo);

        // Guard loosely by info count; each info is visited at most once but we give
        // slack in case of accidental duplicate enqueues from malformed child arrays.
        const int32 IterationCap = FMath::Max(1, InfoMap.Infos.Num() * 2);
        int32 Iterations = 0;

        // Parents already claimed by a same-row child — guards against a child that
        // is connected to the parent through two different pins getting marked twice
        // (same child node appearing in multiple pins' LinkedTo lists).
        TSet<UEdGraphNode*> ParentsWithSameRow;

        int32 Head = 0;
        while (Head < Queue.Num() && Iterations < IterationCap)
        {
            ++Iterations;
            FFormatXInfoPtr ParentInfo = Queue[Head++];

            if (!ParentInfo.IsValid() || !ParentInfo->Node)
            {
                continue;
            }
            UEdGraphNode* ParentNode = ParentInfo->Node;

            if (!ParentsWithSameRow.Contains(ParentNode))
            {
                // Iterate exec-output pins in Node->Pins order; first matching child wins.
                bool bClaimed = false;
                for (UEdGraphPin* Pin : ParentNode->Pins)
                {
                    if (bClaimed)
                    {
                        break;
                    }
                    if (!Pin || Pin->bHidden)
                    {
                        continue;
                    }
                    if (Pin->Direction != EGPD_Output)
                    {
                        continue;
                    }
                    if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                    {
                        continue;
                    }

                    for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                    {
                        if (!LinkedPin)
                        {
                            continue;
                        }
                        UEdGraphNode* ChildNode = LinkedPin->GetOwningNodeUnchecked();
                        if (!ChildNode)
                        {
                            continue;
                        }
                        FFormatXInfoPtr ChildInfo = InfoMap.Find(ChildNode);
                        if (!ChildInfo.IsValid())
                        {
                            continue;
                        }

                        // Confirm the child was adopted by THIS parent through THIS pin
                        // during BuildFormatXInfoMap. If re-parented to a different
                        // parent (via BetterParent rule), LinkFromParent reflects the
                        // winning parent and this check fails — correctly skipping it.
                        const FPinLink& LinkFromParent = ChildInfo->LinkFromParent;
                        if (LinkFromParent.GetFromNode() != ParentNode
                            || LinkFromParent.FromPin != Pin
                            || LinkFromParent.ToPin != LinkedPin)
                        {
                            continue;
                        }

                        // Mark same-row. One per parent even if the same child node
                        // appears in multiple LinkedTo entries (ParentsWithSameRow
                        // guards against that path via the outer gate).
                        ChildInfo->bSameRowAsParent = true;
                        ParentsWithSameRow.Add(ParentNode);
                        bClaimed = true;
                        break;
                    }
                }
            }

            // Enqueue all children so the whole reachable tree gets visited.
            for (const FFormatXInfoPtr& Child : ParentInfo->Children)
            {
                if (Child.IsValid())
                {
                    Queue.Add(Child);
                }
            }
        }

        if (Iterations >= IterationCap && Head < Queue.Num())
        {
            UE_LOG(LogBpirLayout, Verbose,
                TEXT("BPIR GetPinsOfSameHeight hit iteration cap at %d with %d items unprocessed"),
                Iterations, Queue.Num() - Head);
        }
    }

    // ------------------------------------------------------------------------
    // GetChildX / FormatX
    //
    // Port of BlueprintAssist's FEdGraphFormatter::GetChildX formula. For an
    // EGPD_Output walk (child right of parent):
    //     Delta = ChildBounds.Left - LargerBounds.Left
    //     NewX  = ParentBounds.Right + Delta + NodePadX
    // For an EGPD_Input walk (child left of parent):
    //     Delta = LargerBounds.Right - ChildBounds.Left
    //     NewX  = ParentBounds.Left - Delta - NodePadX
    //
    // ChildBounds is always the child's bare rect; LargerBounds is the child's
    // cluster-extended rect when bUseClusterBounds is true (so the child's
    // parameter subtree stays clear of the parent). ParentBounds uses the same
    // cluster mode as LargerBounds.
    //
    // The Delta term lets a child whose cluster rect extends further left than
    // its own node (Delta > 0 for EGPD_Output) shift right enough to clear the
    // parent by exactly NodePadX, rather than overlapping the parameter column.
    // ------------------------------------------------------------------------

    int32 GetChildX(
        const UEdGraphNode* Parent,
        const UEdGraphNode* Child,
        EEdGraphPinDirection Direction,
        const UBpirLayoutSettings& Settings,
        bool bUseClusterBounds,
        const FClusterBoundsRegistry* ClusterRegistry)
    {
        if (!Parent || !Child)
        {
            return Child ? Child->NodePosX : 0;
        }

        const FSlateRect ParentBounds = GetNodeBounds(Parent, Settings, bUseClusterBounds, ClusterRegistry);
        const FSlateRect LargerBounds = GetNodeBounds(Child, Settings, bUseClusterBounds, ClusterRegistry);
        // Child's bare rect — intentionally ignores cluster overlay so Delta isolates the
        // cluster-vs-bare offset rather than canceling out.
        const FSlateRect ChildBounds = GetNodeBounds(Child, Settings, /*bUseClusterBounds=*/false, nullptr);

        const float NodePadX = static_cast<float>(Settings.NodePadX);
        float NewX;
        if (Direction == EGPD_Input)
        {
            const float Delta = LargerBounds.Right - ChildBounds.Left;
            NewX = ParentBounds.Left - Delta - NodePadX;
        }
        else
        {
            const float Delta = ChildBounds.Left - LargerBounds.Left;
            NewX = ParentBounds.Right + Delta + NodePadX;
        }

        // Directional grid alignment: floor (more-negative) for input walks, ceil
        // (more-positive) for output walks, so the child's bounds never shrink toward the parent.
        const int32 Grid = FMath::Max(1, Settings.InternalGridPx);
        int32 ResultX;
        if (Direction == EGPD_Input)
        {
            ResultX = FloorToGrid(FMath::RoundToInt(NewX), Grid);
        }
        else
        {
            ResultX = CeilToGrid(FMath::RoundToInt(NewX), Grid);
        }
        return ResultX;
    }

    void FormatX(
        const FFormatXInfoMap& InfoMap,
        const UBpirLayoutSettings& Settings,
        bool bUseClusterBounds,
        const FClusterBoundsRegistry* ClusterRegistry)
    {
        if (!InfoMap.RootInfo.IsValid())
        {
            return;
        }

        // BFS from RootInfo: parent always visited before any of its children, which
        // matters because GetChildX reads ParentBounds (which depends on NodePosX).
        TArray<FFormatXInfoPtr> Queue;
        for (const FFormatXInfoPtr& Child : InfoMap.RootInfo->Children)
        {
            if (Child.IsValid())
            {
                Queue.Add(Child);
            }
        }

        int32 Iterations = 0;
        const int32 IterationCap = FMath::Max(1, Settings.TraversalIterationCap);
        int32 Head = 0;

        while (Head < Queue.Num() && Iterations < IterationCap)
        {
            ++Iterations;
            FFormatXInfoPtr Info = Queue[Head++];

            if (!Info.IsValid() || !Info->Node || Info->bIsRoot)
            {
                continue;
            }

            UEdGraphNode* ParentNode = Info->LinkFromParent.GetFromNode();
            if (ParentNode)
            {
                const EEdGraphPinDirection Dir = Info->LinkFromParent.GetDirection();
                const int32 NewX = GetChildX(
                    ParentNode, Info->Node, Dir, Settings, bUseClusterBounds, ClusterRegistry);
                Info->Node->NodePosX = NewX;
            }

            for (const FFormatXInfoPtr& Child : Info->Children)
            {
                if (Child.IsValid())
                {
                    Queue.Add(Child);
                }
            }
        }

        if (Iterations >= IterationCap && Head < Queue.Num())
        {
            UE_LOG(LogBpirLayout, Verbose,
                TEXT("BPIR FormatX hit iteration cap at %d with %d items unprocessed"),
                Iterations, Queue.Num() - Head);
        }
    }

    // ------------------------------------------------------------------------
    // FormatParameterNodes — see header for contract.
    // ------------------------------------------------------------------------

    void FormatParameterNodes(
        const FFormatXInfoMap& InfoMap,
        const TSet<UEdGraphNode*>& NodesInPool,
        const UBpirLayoutSettings& Settings,
        FClusterBoundsRegistry& OutRegistry,
        TSet<UEdGraphNode*>& OutPlacedPureNodes,
        TArray<TUniquePtr<FNodeLayoutParameterFormatter>>& OutFormatters)
    {
        if (!InfoMap.RootInfo.IsValid())
        {
            return;
        }

        // BFS from root (including the root itself — a root like CustomEvent is
        // impure and may legitimately have pure data inputs).
        TArray<FFormatXInfoPtr> Queue;
        Queue.Add(InfoMap.RootInfo);

        int32 Iterations = 0;
        const int32 IterationCap = FMath::Max(1, Settings.TraversalIterationCap);
        int32 Head = 0;

        while (Head < Queue.Num() && Iterations < IterationCap)
        {
            ++Iterations;
            FFormatXInfoPtr Info = Queue[Head++];

            if (!Info.IsValid() || !Info->Node)
            {
                continue;
            }

            UEdGraphNode* ConsumerNode = Info->Node;

            // Enqueue children up-front so BFS order is preserved regardless of
            // whether we skip the current node as a non-consumer.
            for (const FFormatXInfoPtr& Child : Info->Children)
            {
                if (Child.IsValid())
                {
                    Queue.Add(Child);
                }
            }

            if (IsPureNode(ConsumerNode))
            {
                continue;
            }

            TUniquePtr<FNodeLayoutParameterFormatter> Formatter =
                MakeUnique<FNodeLayoutParameterFormatter>(ConsumerNode, Settings);

            // IgnoredNodes is the accumulated claim set — each successive consumer
            // sees every pure claimed by an earlier (BFS-higher) consumer and skips it.
            Formatter->Format(NodesInPool, OutPlacedPureNodes);

            const TArray<UEdGraphNode*>& JustPlaced = Formatter->GetPlacedPureNodes();
            if (JustPlaced.Num() == 0)
            {
                // No pures to claim — discard the formatter entirely; registering a
                // cluster rect that merely mirrors the consumer's bare rect would be
                // a no-op later (GetNodeBounds short-circuits to bare when cluster
                // equals bare), and clutters the registry.
                continue;
            }

            OutRegistry.Register(ConsumerNode, Formatter->GetClusterBounds());
            for (UEdGraphNode* Pure : JustPlaced)
            {
                if (Pure)
                {
                    OutPlacedPureNodes.Add(Pure);
                }
            }
            OutFormatters.Add(MoveTemp(Formatter));
        }

        if (Iterations >= IterationCap && Head < Queue.Num())
        {
            UE_LOG(LogBpirLayout, Verbose,
                TEXT("BPIR FormatParameterNodes hit iteration cap at %d with %d items unprocessed"),
                Iterations, Queue.Num() - Head);
        }
    }

    // ------------------------------------------------------------------------
    // FormatY — see header for contract.
    // ------------------------------------------------------------------------

    namespace
    {
        // Recursive DFS pre-order placement. AlreadyPlaced accumulates every node
        // that has received a Y assignment (root included), forming the obstacle
        // set for subsequent collision checks.
        void FormatY_Recursive(
            const FFormatXInfoPtr& Info,
            TArray<UEdGraphNode*>& AlreadyPlaced,
            const TSet<UEdGraphNode*>& ExternalObstacles,
            const UBpirLayoutSettings& Settings,
            const TMap<UEdGraphNode*, FNodeLayoutParameterFormatter*>& FormatterByConsumer)
        {
            if (!Info.IsValid() || !Info->Node)
            {
                return;
            }

            UEdGraphNode* Node = Info->Node;

            if (Info->bIsRoot)
            {
                // Root keeps its anchor Y. Still treat it as an obstacle for
                // downstream placements so children can't overlap it.
                AlreadyPlaced.Add(Node);
            }
            else
            {
                FFormatXInfoPtr Parent = Info->Parent.Pin();
                if (!Parent.IsValid() || !Parent->Node)
                {
                    // Orphan info — shouldn't happen for a well-formed map, but
                    // don't touch the Y and still participate as an obstacle.
                    AlreadyPlaced.Add(Node);
                }
                else
                {
                    UEdGraphNode* ParentNode = Parent->Node;

                    // Tentative Y assignment.
                    if (Info->bSameRowAsParent)
                    {
                        const int32 DeltaY = ParentNode->NodePosY - Node->NodePosY;
                        TranslateCluster(Node, FormatterByConsumer, DeltaY);
                    }
                    else
                    {
                        // Stack below the lowest sibling already placed for this
                        // parent. This stays correct even if the child ordering
                        // changed during traversal or a sibling was revisited
                        // through another exec path.
                        float LowestPlacedSiblingBottom = -FLT_MAX;
                        bool bFoundPlacedSibling = false;
                        for (const FFormatXInfoPtr& SiblingInfo : Parent->Children)
                        {
                            if (!SiblingInfo.IsValid() || !SiblingInfo->Node || SiblingInfo.Get() == Info.Get())
                            {
                                continue;
                            }
                            if (!AlreadyPlaced.Contains(SiblingInfo->Node))
                            {
                                continue;
                            }

                            const FSlateRect SiblingBounds = GetCurrentClusterBounds(
                                SiblingInfo->Node, FormatterByConsumer, Settings);
                            LowestPlacedSiblingBottom = FMath::Max(LowestPlacedSiblingBottom, SiblingBounds.Bottom);
                            bFoundPlacedSibling = true;
                        }

                        if (bFoundPlacedSibling)
                        {
                            const int32 NewY = FMath::RoundToInt(
                                LowestPlacedSiblingBottom + static_cast<float>(Settings.NodePadY));
                            const int32 DeltaY = NewY - Node->NodePosY;
                            TranslateCluster(Node, FormatterByConsumer, DeltaY);
                        }
                        else
                        {
                            // First sibling (or no earlier sibling has been placed yet).
                            // Subsequent collision checks still keep it clear of the
                            // parent cluster and external obstacles.
                            const int32 DeltaY = ParentNode->NodePosY - Node->NodePosY;
                            TranslateCluster(Node, FormatterByConsumer, DeltaY);
                        }
                    }

                    // Collision resolution loop. Jump-to-clear on overlap: push
                    // Node's top just below the overlapping obstacle's bottom.
                    // This matches BA's actual behavior (set NodePosY =
                    // OtherBounds.Bottom + 1) rather than literally nudging one
                    // pixel per iteration — the latter would need orders of
                    // magnitude more iterations on tall obstacles and is what
                    // caused BA's cap to sometimes trip. Cap is still honored as
                    // a safety valve against pathological cascades.

                    // Build the set of same-row ancestors once before the loop.
                    // Any ancestor that placed this node on its row must not be
                    // treated as a blocker — doing so would push the node off the
                    // row that FormatY intentionally assigned it to.
                    TSet<UEdGraphNode*> SameRowAncestors;
                    {
                        FFormatXInfoPtr Cursor = Info;
                        while (Cursor.IsValid() && Cursor->bSameRowAsParent)
                        {
                            FFormatXInfoPtr ParentCursor = Cursor->Parent.Pin();
                            if (!ParentCursor.IsValid() || !ParentCursor->Node)
                            {
                                break;
                            }
                            SameRowAncestors.Add(ParentCursor->Node);
                            Cursor = ParentCursor;
                        }
                    }

                    const int32 Cap = FMath::Max(1, Settings.CollisionIterationCap);
                    int32 Iter = 0;
                    bool bOverlap = true;
                    for (; Iter < Cap && bOverlap; ++Iter)
                    {
                        bOverlap = false;
                        const FSlateRect MyBounds = GetCurrentClusterBounds(
                            Node, FormatterByConsumer, Settings);

                        auto TryBlock = [&](UEdGraphNode* Other) -> bool
                        {
                            if (!Other || Other == Node)
                            {
                                return false;
                            }
                            // Same-row ancestors must not block this node — their pure
                            // clusters may extend downward past the row Y but that is
                            // intentional layout, not a collision to resolve.
                            if (SameRowAncestors.Contains(Other))
                            {
                                return false;
                            }
                            const FSlateRect OtherBounds = GetCurrentClusterBounds(
                                Other, FormatterByConsumer, Settings);
                            if (FSlateRect::DoRectanglesIntersect(MyBounds, OtherBounds))
                            {
                                // Jump past the obstacle by placing Node's top at
                                // OtherBounds.Bottom + 1. Guarantee forward motion
                                // of at least 1 px even if bounds coincide exactly
                                // at the top edge (i.e. MyBounds.Top == OtherBounds.Bottom).
                                const float NewTop = OtherBounds.Bottom + 1.0f;
                                const int32 NewY = FMath::Max(
                                    Node->NodePosY + 1,
                                    FMath::RoundToInt(NewTop));
                                const int32 CollisionDeltaY = NewY - Node->NodePosY;
                                TranslateCluster(Node, FormatterByConsumer, CollisionDeltaY);
                                return true;
                            }
                            return false;
                        };

                        for (UEdGraphNode* Other : AlreadyPlaced)
                        {
                            if (TryBlock(Other))
                            {
                                bOverlap = true;
                                break;
                            }
                        }
                        if (!bOverlap)
                        {
                            for (UEdGraphNode* Other : ExternalObstacles)
                            {
                                if (TryBlock(Other))
                                {
                                    bOverlap = true;
                                    break;
                                }
                            }
                        }
                    }

                    if (bOverlap)
                    {
                        UE_LOG(LogBpirLayout, Verbose,
                            TEXT("BPIR FormatY collision cap (%d) hit for node %s; leaving last position"),
                            Cap, *Node->GetName());
                    }

                    AlreadyPlaced.Add(Node);
                }
            }

            // Recurse into children in pin-order: same-row child (first exec
            // output pin's adoptee) naturally comes first, then stacked
            // siblings in the order their pins appear on the parent. Info->
            // Children is built during BuildFormatXInfoMap from a LIFO stack
            // and therefore can end up reversed relative to pin order — which
            // mis-stacks both the same-row child (it overlaps its siblings
            // placed before it since they are not same-row ancestors) and the
            // stacked siblings themselves (then_2 ends up above then_1). We
            // reorder here by iterating the parent's exec-output pins in
            // Node->Pins order and matching each pin to the child whose
            // LinkFromParent recorded that pin as the edge of adoption.
            TArray<FFormatXInfoPtr> OrderedChildren;
            OrderedChildren.Reserve(Info->Children.Num());
            if (Info->Node)
            {
                for (UEdGraphPin* Pin : Info->Node->Pins)
                {
                    if (!Pin || Pin->bHidden) continue;
                    if (Pin->Direction != EGPD_Output) continue;
                    if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
                    for (const FFormatXInfoPtr& Child : Info->Children)
                    {
                        if (!Child.IsValid()) continue;
                        if (Child->LinkFromParent.FromPin == Pin
                            && !OrderedChildren.Contains(Child))
                        {
                            OrderedChildren.Add(Child);
                        }
                    }
                }
            }
            // Append any children not matched above (e.g. adopted via an
            // EGPD_Input walk, or a non-exec link) in their original order.
            for (const FFormatXInfoPtr& Child : Info->Children)
            {
                if (Child.IsValid() && !OrderedChildren.Contains(Child))
                {
                    OrderedChildren.Add(Child);
                }
            }
            for (const FFormatXInfoPtr& Child : OrderedChildren)
            {
                FormatY_Recursive(Child, AlreadyPlaced, ExternalObstacles, Settings, FormatterByConsumer);
            }
        }
    }

    void FormatY(
        FFormatXInfoMap& InfoMap,
        const TSet<UEdGraphNode*>& ExternalObstacles,
        const UBpirLayoutSettings& Settings,
        const TMap<UEdGraphNode*, FNodeLayoutParameterFormatter*>& FormatterByConsumer,
        FClusterBoundsRegistry* OutRegistry)
    {
        if (!InfoMap.RootInfo.IsValid())
        {
            return;
        }

        TArray<UEdGraphNode*> AlreadyPlaced;
        AlreadyPlaced.Reserve(InfoMap.Infos.Num());
        FormatY_Recursive(InfoMap.RootInfo, AlreadyPlaced, ExternalObstacles, Settings, FormatterByConsumer);

        // Refresh the registry so post-FormatY callers reading cluster bounds via
        // GetNodeBounds(..., bUseClusterBounds=true, Registry) observe current
        // NodePosX/Y rather than the pre-FormatY bounds that the X-pass left behind.
        if (OutRegistry)
        {
            SyncRegistryFromFormatters(FormatterByConsumer, *OutRegistry, Settings);
        }
    }

    // ------------------------------------------------------------------------
    // ResetRelativeToAnchor — translate the pool so AnchorNode lands at SavedAnchorPos.
    // ------------------------------------------------------------------------
    void ResetRelativeToAnchor(
        const TSet<UEdGraphNode*>& PoolNodes,
        UEdGraphNode* AnchorNode,
        FIntPoint SavedAnchorPos)
    {
        // Can't translate without a concrete reference node in the pool.
        if (AnchorNode == nullptr || !PoolNodes.Contains(AnchorNode))
        {
            return;
        }

        const FIntPoint Offset(
            SavedAnchorPos.X - AnchorNode->NodePosX,
            SavedAnchorPos.Y - AnchorNode->NodePosY);

        // No-op if the anchor already sits at the saved position.
        if (Offset.X == 0 && Offset.Y == 0)
        {
            return;
        }

        for (UEdGraphNode* Node : PoolNodes)
        {
            if (Node)
            {
                Node->NodePosX += Offset.X;
                Node->NodePosY += Offset.Y;
            }
        }
    }

    // ------------------------------------------------------------------------
    // SnapToGrid — directional rounding on X, standard Round on Y. Uses
    // Settings.InternalGridPx; uses InfoMap.LinkFromParent to pick direction.
    // Anchor always rounds on both axes so it lands squarely on the grid.
    // ------------------------------------------------------------------------
    void SnapToGrid(
        const TSet<UEdGraphNode*>& PoolNodes,
        UEdGraphNode* AnchorNode,
        const FFormatXInfoMap& InfoMap,
        const UBpirLayoutSettings& Settings)
    {
        const int32 Grid = FMath::Max(1, Settings.InternalGridPx);

        for (UEdGraphNode* Node : PoolNodes)
        {
            if (!Node)
            {
                continue;
            }

            // Determine the X-axis rounding direction.
            // Default (root, anchor, or missing info) is standard Round.
            bool bUseFloor = false;
            bool bUseCeil = false;

            if (Node != AnchorNode)
            {
                const FFormatXInfoPtr Info = InfoMap.Find(Node);
                if (Info.IsValid() && Info->LinkFromParent.IsValid())
                {
                    const EEdGraphPinDirection Dir = Info->LinkFromParent.GetDirection();
                    if (Dir == EGPD_Input)
                    {
                        bUseFloor = true;
                    }
                    else if (Dir == EGPD_Output)
                    {
                        bUseCeil = true;
                    }
                }
            }

            if (bUseFloor)
            {
                Node->NodePosX = FloorToGrid(Node->NodePosX, Grid);
            }
            else if (bUseCeil)
            {
                Node->NodePosX = CeilToGrid(Node->NodePosX, Grid);
            }
            else
            {
                Node->NodePosX = RoundToGrid(Node->NodePosX, Grid);
            }

            // Y always uses standard Round.
            Node->NodePosY = RoundToGrid(Node->NodePosY, Grid);
        }
    }

    // ------------------------------------------------------------------------
    // FNodeLayoutEngine — top-level driver (Task 10).
    // ------------------------------------------------------------------------

    FNodeLayoutEngine::FNodeLayoutEngine(
        UEdGraph* InGraph,
        TArray<UEdGraphNode*> InPool,
        UEdGraphNode* InAnchor,
        TArray<UEdGraphNode*> InExternalObstacles)
        : Graph(InGraph)
        , Anchor(InAnchor)
    {
        // Copy caller-friendly arrays into internal sets (de-dup + fast Contains).
        Pool.Reserve(InPool.Num());
        for (UEdGraphNode* Node : InPool)
        {
            if (Node)
            {
                Pool.Add(Node);
            }
        }

        ExternalObstacles.Reserve(InExternalObstacles.Num());
        for (UEdGraphNode* Node : InExternalObstacles)
        {
            if (Node)
            {
                ExternalObstacles.Add(Node);
            }
        }
    }

    void FNodeLayoutEngine::Format()
    {
        // Project-level CDO — reflects the user's saved UBpirLayoutSettings values.
        const UBpirLayoutSettings* Settings = GetDefault<UBpirLayoutSettings>();
        if (!Settings)
        {
            UE_LOG(LogBpirLayout, Verbose,
                TEXT("FNodeLayoutEngine::Format — UBpirLayoutSettings CDO unavailable; skipping layout"));
            return;
        }

        // Kill switch — lets users disable the entire pass from Project Settings.
        if (!Settings->bEnableBpirLayoutPass)
        {
            UE_LOG(LogBpirLayout, Verbose,
                TEXT("FNodeLayoutEngine::Format — layout pass disabled by settings"));
            return;
        }

        // Guard against degenerate inputs.
        if (Graph == nullptr || Anchor == nullptr || Pool.IsEmpty())
        {
            UE_LOG(LogBpirLayout, Verbose,
                TEXT("FNodeLayoutEngine::Format — nothing to do (Graph=%p, Anchor=%p, PoolNum=%d)"),
                Graph, Anchor, Pool.Num());
            return;
        }

        // 1. Save anchor's original position so step 8 can translate the whole
        //    subgraph back to where the user expects it.
        const FIntPoint SavedAnchorPos(Anchor->NodePosX, Anchor->NodePosY);

        // 2. Build adjacency/tree map — only reaches exec-wired nodes in Pool.
        FFormatXInfoMap InfoMap = BuildFormatXInfoMap(Anchor, Pool, *Settings);

        // 3. FormatX pass 1 — places X without cluster awareness so the
        //    parameter formatter has a stable coordinate system to work from.
        FormatX(InfoMap, *Settings, /*bUseClusterBounds=*/false, /*ClusterRegistry=*/nullptr);

        // 4. FormatParameterNodes — instantiates parameter formatters per
        //    consumer, placing pure nodes and registering cluster bounds.
        FClusterBoundsRegistry Registry;
        TSet<UEdGraphNode*> ClaimedPures;
        TArray<TUniquePtr<FNodeLayoutParameterFormatter>> ParamFormatters;
        FormatParameterNodes(InfoMap, Pool, *Settings, Registry, ClaimedPures, ParamFormatters);

        // Snapshot each consumer's X before pass 2 so we can shift their pure
        // clusters by the same delta after pass 2. The parameter formatter
        // placed each pure relative to the consumer's pass-1 X, but pass 2
        // moves consumers rightward to accommodate cluster bounds — leaving
        // pures orphaned at stale X coordinates that can overlap upstream
        // neighbors (e.g. the root Event node).
        TMap<UEdGraphNode*, int32> ConsumerXBeforePass2;
        ConsumerXBeforePass2.Reserve(ParamFormatters.Num());
        for (const TUniquePtr<FNodeLayoutParameterFormatter>& Formatter : ParamFormatters)
        {
            if (Formatter && Formatter->GetConsumer())
            {
                ConsumerXBeforePass2.Add(Formatter->GetConsumer(), Formatter->GetConsumer()->NodePosX);
            }
        }

        // 5. FormatX pass 2 — repositions impure nodes so clusters no longer
        //    overlap their upstream neighbors.
        FormatX(InfoMap, *Settings, /*bUseClusterBounds=*/true, &Registry);

        // 5b. Translate each pure cluster in X by (new Consumer.X - old Consumer.X)
        // so pures stay attached to the left of their consumer after pass 2.
        // Also re-register the translated cluster rect so the registry reflects
        // current positions — stale registry entries would otherwise make
        // GetNodeBounds (cluster-aware) report false overlaps downstream.
        for (const TUniquePtr<FNodeLayoutParameterFormatter>& Formatter : ParamFormatters)
        {
            if (!Formatter || !Formatter->GetConsumer()) continue;
            UEdGraphNode* Consumer = Formatter->GetConsumer();
            const int32* OldX = ConsumerXBeforePass2.Find(Consumer);
            if (!OldX) continue;
            const int32 DeltaX = Consumer->NodePosX - *OldX;
            if (DeltaX == 0) continue;
            for (UEdGraphNode* Pure : Formatter->GetPlacedPureNodes())
            {
                if (Pure)
                {
                    Pure->NodePosX += DeltaX;
                }
            }
            // Shift the registered cluster rect by the same DeltaX.
            if (Registry.Has(Consumer))
            {
                const FSlateRect OldRect = Registry.Get(Consumer);
                const FSlateRect ShiftedRect(
                    OldRect.Left + static_cast<float>(DeltaX),
                    OldRect.Top,
                    OldRect.Right + static_cast<float>(DeltaX),
                    OldRect.Bottom);
                Registry.Register(Consumer, ShiftedRect);
            }
        }

        // 6. Mark first exec-output child of each parent as same-row.
        GetPinsOfSameHeight(InfoMap);

        // 7. FormatY — recursive DFS placement with AABB collision resolution.
        //    Build a live formatter lookup so FormatY computes cluster bounds
        //    from current node positions rather than stale registry entries,
        //    and moves pure nodes along with their consumers.
        TMap<UEdGraphNode*, FNodeLayoutParameterFormatter*> FormatterByConsumer;
        for (const TUniquePtr<FNodeLayoutParameterFormatter>& Formatter : ParamFormatters)
        {
            if (Formatter && Formatter->GetConsumer())
            {
                FormatterByConsumer.Add(Formatter->GetConsumer(), Formatter.Get());
            }
        }
        FormatY(InfoMap, ExternalObstacles, *Settings, FormatterByConsumer, &Registry);

        // 7.5. Resolve any remaining cluster overlaps after the main placement pass.
        ResolveConsumerClusterOverlaps(InfoMap, FormatterByConsumer, *Settings);

        // 7.5-post. ResolveConsumerClusterOverlaps may translate clusters via
        // TranslateCluster; restore the Registry==current-positions invariant.
        SyncRegistryFromFormatters(FormatterByConsumer, Registry, *Settings);

        // 8. Translate everything so anchor lands back at SavedAnchorPos.
        //    Pure nodes are in Pool and travel with the rest of the subgraph.
        ResetRelativeToAnchor(Pool, Anchor, SavedAnchorPos);

        // 9. Snap to grid — directional for impure nodes (via InfoMap lookup),
        //    Round for pure nodes (not in InfoMap, falls through to Round/Round).
        SnapToGrid(Pool, Anchor, InfoMap, *Settings);
    }
}
