// Copyright (c) 2026 Alexander Penkin. MIT License.

// NodeLayoutParameterFormatter.cpp - See header for API contract.

#include "Compiler/NodeLayoutParameterFormatter.h"

#include "BpirLayoutSettings.h"
#include "Compiler/NodeLayoutEngine.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Math/UnrealMathUtility.h"
#include "Math/Vector2D.h"

namespace BpirLayout
{

    FNodeLayoutParameterFormatter::FNodeLayoutParameterFormatter(
        UEdGraphNode* InConsumer,
        const UBpirLayoutSettings& InSettings)
        : Consumer(InConsumer)
        , Settings(InSettings)
        , ClusterBounds(FSlateRect(0, 0, 0, 0))
    {
    }

    void FNodeLayoutParameterFormatter::CollectPureAncestors(
        const TSet<UEdGraphNode*>& NodesInPool,
        const TSet<UEdGraphNode*>& IgnoredNodes,
        TArray<UEdGraphNode*>& OutOrdered)
    {
        OutOrdered.Reset();
        if (!Consumer)
        {
            return;
        }

        TSet<UEdGraphNode*> Visited;
        TArray<UEdGraphNode*> Queue;
        Queue.Add(Consumer);

        // BFS so that direct ancestors of the consumer come first in OutOrdered,
        // which matches the desired top-to-bottom visual stacking order.
        int32 Head = 0;
        while (Head < Queue.Num())
        {
            UEdGraphNode* Current = Queue[Head++];

            if (!Current)
            {
                continue;
            }

            for (UEdGraphPin* Pin : Current->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Input)
                {
                    continue;
                }
                if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    continue;
                }
                if (Pin->bHidden)
                {
                    continue;
                }

                for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    if (!LinkedPin)
                    {
                        continue;
                    }
                    UEdGraphNode* Upstream = LinkedPin->GetOwningNode();
                    if (!Upstream || Upstream == Consumer)
                    {
                        continue;
                    }
                    if (Visited.Contains(Upstream))
                    {
                        continue;
                    }
                    if (IgnoredNodes.Contains(Upstream))
                    {
                        continue;
                    }
                    if (!NodesInPool.Contains(Upstream))
                    {
                        continue;
                    }
                    if (!IsPureNode(Upstream))
                    {
                        continue;
                    }

                    Visited.Add(Upstream);
                    OutOrdered.Add(Upstream);
                    Queue.Add(Upstream);
                }
            }
        }
    }

    void FNodeLayoutParameterFormatter::Format(
        const TSet<UEdGraphNode*>& NodesInPool,
        const TSet<UEdGraphNode*>& IgnoredNodes)
    {
        PlacedPureNodes.Reset();
        ClusterBounds = FSlateRect(0, 0, 0, 0);

        if (!Consumer)
        {
            return;
        }

        CollectPureAncestors(NodesInPool, IgnoredNodes, PlacedPureNodes);

        // Consumer-only case: cluster bounds == consumer's bare bounds.
        const FSlateRect ConsumerBounds = GetNodeBounds(Consumer, Settings, /*bUseClusterBounds=*/false, nullptr);
        if (PlacedPureNodes.Num() == 0)
        {
            ClusterBounds = ConsumerBounds;
            return;
        }

        const int32 Grid = FMath::Max(1, Settings.InternalGridPx);
        const int32 ColumnRightEdge = Consumer->NodePosX - Settings.PinPadX;

        // Top-align the first pure node with the consumer; subsequent pures stack
        // below with IntraParameterPadY between successive row bottoms and tops.
        int32 PreviousBottom = Consumer->NodePosY;
        bool bFirst = true;

        FSlateRect Accumulated = ConsumerBounds;

        for (UEdGraphNode* Pure : PlacedPureNodes)
        {
            if (!Pure)
            {
                continue;
            }

            const FVector2D Size = EstimateNodeSize(Pure, Settings);
            const int32 Width = FMath::CeilToInt(Size.X);
            const int32 Height = FMath::CeilToInt(Size.Y);

            // Align right edge with (Consumer.Left - PinPadX). Floor so the node's
            // right edge never crosses over ColumnRightEdge after grid snapping.
            const int32 RawX = ColumnRightEdge - Width;
            Pure->NodePosX = FloorToGrid(RawX, Grid);

            const int32 RawY = bFirst
                ? Consumer->NodePosY
                : (PreviousBottom + Settings.IntraParameterPadY);
            Pure->NodePosY = CeilToGrid(RawY, Grid);

            PreviousBottom = Pure->NodePosY + Height;
            bFirst = false;

            const FSlateRect PureRect(
                static_cast<float>(Pure->NodePosX),
                static_cast<float>(Pure->NodePosY),
                static_cast<float>(Pure->NodePosX + Width),
                static_cast<float>(Pure->NodePosY + Height));

            Accumulated = Accumulated.Expand(PureRect);
        }

        ClusterBounds = Accumulated;
    }
}
