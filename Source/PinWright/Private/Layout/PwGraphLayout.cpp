// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwGraphLayout.cpp - See PwGraphLayout.h for the algorithm summary.

#include "Layout/PwGraphLayout.h"

#include "Algo/StableSort.h"

DEFINE_LOG_CATEGORY_STATIC(LogPwGraphLayout, Log, All);

namespace PwGraphLayoutCore
{
    using namespace PwGraphLayout;

    // One wire as seen from one of its two nodes.
    struct FAdjacent
    {
        int32 Wire = INDEX_NONE;
        int32 Other = INDEX_NONE;
        int32 LocalPin = INDEX_NONE;
        int32 OtherPin = INDEX_NONE;
    };

    // An occupied rect: a placed node's (Node) or a comment's frame or reservation (Comment).
    struct FRect
    {
        double L = 0.0;
        double T = 0.0;
        double R = 0.0;
        double B = 0.0;
        int32 Node = INDEX_NONE;
        int32 Comment = INDEX_NONE;
    };

    // A rect being fitted: a node, or the reservation of a comment anchored on a node. Top and
    // Bottom are relative to the candidate offset.
    struct FShape
    {
        double L = 0.0;
        double R = 0.0;
        double Top = 0.0;
        double Bottom = 0.0;
        int32 Node = INDEX_NONE;
        int32 Comment = INDEX_NONE;
    };

    struct FSpan
    {
        double Lo = 0.0;
        double Hi = 0.0;
    };

    enum class EVisit : uint8
    {
        Fresh,
        Open,
        Closed
    };

    class FArranger
    {
    public:
        FArranger(FLayoutGraph& InGraph, const FSpacing& InSpacing)
            : Graph(InGraph)
            , Spacing(InSpacing)
            , Num(InGraph.Nodes.Num())
        {
        }

        FArrangeReport Run()
        {
            Allocate();
            IndexWires();
            PlantFlowTrees();
            ClaimDataNodes();
            for (int32 Owner = 0; Owner < Num; ++Owner)
            {
                if (TreeOf[Owner] != INDEX_NONE)
                {
                    ShapeBlock(Owner);
                }
            }
            AssignX();
            PrepareComments();
            for (int32 Pass = 1; ; ++Pass)
            {
                AssignY();
                if (Pass == MaxCommentPasses || !GrowReservations())
                {
                    break;
                }
                ResetPlacement();
            }
            return Commit();
        }

    private:
        FLayoutGraph& Graph;
        const FSpacing& Spacing;
        const int32 Num;

        TArray<TArray<FAdjacent>> Outs;
        TArray<TArray<FAdjacent>> Ins;
        TArray<bool> bFlowWire;
        TArray<bool> bSkippedWire;
        TArray<bool> bHasFlowPin;

        // Forest (flow trees plus single-node data roots).
        TArray<EVisit> Visit;
        TArray<int32> TreeOf;
        TArray<int32> Rank;
        TArray<int32> Preorder;
        TArray<int32> ParentWire;
        TArray<TArray<int32>> Kids;
        TArray<int32> Finished;
        TArray<int32> TreeRoots;
        int32 PreorderCounter = 0;

        // Data blocks.
        TArray<int32> BlockOwner;
        TArray<TArray<int32>> Block;
        TArray<int32> Level;
        TArray<int32> Slot;
        TArray<EVisit> BlockVisit;
        TArray<double> RelX;
        TArray<double> RelY;
        TArray<double> BlockReach;

        // Geometry.
        TArray<double> PosX;
        TArray<double> PosY;
        TArray<bool> bPlaced;
        TArray<FRect> Occupied;

        // Comments. Live = has a movable member (re-fitted); the others are fixed obstacles.
        static constexpr int32 MaxCommentPasses = 4;
        TArray<bool> bLiveComment;
        TArray<TArray<bool>> bInComment;
        TArray<TArray<int32>> CommentsOf;
        TArray<TArray<int32>> ChildComments;
        TArray<int32> CommentAnchor;
        TArray<double> FrameL;
        TArray<double> FrameR;
        TArray<double> ReserveTop;
        TArray<double> ReserveBottom;

        const FLayoutNode& Node(int32 Index) const { return Graph.Nodes[Index]; }
        double Width(int32 Index) const { return Node(Index).Size.X; }
        double Height(int32 Index) const { return Node(Index).Size.Y; }
        double PinY(int32 Index, int32 Pin) const { return Node(Index).Pins[Pin].OffsetY; }

        double SnapDown(double Value) const
        {
            return Spacing.Grid > 0.0 ? FMath::FloorToDouble(Value / Spacing.Grid + 1e-6) * Spacing.Grid : Value;
        }

        double SnapUp(double Value) const
        {
            return Spacing.Grid > 0.0 ? FMath::CeilToDouble(Value / Spacing.Grid - 1e-6) * Spacing.Grid : Value;
        }

        double SnapNear(double Value) const
        {
            return Spacing.Grid > 0.0 ? FMath::RoundToDouble(Value / Spacing.Grid) * Spacing.Grid : Value;
        }

        // Reading order on the canvas, then identity: the order in which nodes of equal standing
        // are taken, independent of the caller's node order.
        bool PlacedBefore(int32 A, int32 B) const
        {
            const FVector2D& PA = Node(A).Position;
            const FVector2D& PB = Node(B).Position;
            if (PA.Y != PB.Y) { return PA.Y < PB.Y; }
            if (PA.X != PB.X) { return PA.X < PB.X; }
            return Node(A).Key < Node(B).Key;
        }

        bool IsMember(int32 Index) const { return TreeOf[Index] != INDEX_NONE; }

        bool IsClaimable(int32 Index) const
        {
            return Node(Index).bMovable && !bHasFlowPin[Index] && !IsMember(Index)
                && BlockOwner[Index] == INDEX_NONE;
        }

        void Allocate()
        {
            Outs.SetNum(Num);
            Ins.SetNum(Num);
            bHasFlowPin.Init(false, Num);
            Visit.Init(EVisit::Fresh, Num);
            TreeOf.Init(INDEX_NONE, Num);
            Rank.Init(0, Num);
            Preorder.Init(0, Num);
            ParentWire.Init(INDEX_NONE, Num);
            Kids.SetNum(Num);
            BlockOwner.Init(INDEX_NONE, Num);
            Block.SetNum(Num);
            Level.Init(0, Num);
            Slot.Init(0, Num);
            BlockVisit.Init(EVisit::Fresh, Num);
            RelX.Init(0.0, Num);
            RelY.Init(0.0, Num);
            BlockReach.Init(0.0, Num);
            PosX.Init(0.0, Num);
            PosY.Init(0.0, Num);
            bPlaced.Init(false, Num);

            for (int32 Index = 0; Index < Num; ++Index)
            {
                PosX[Index] = Node(Index).Position.X;
                PosY[Index] = Node(Index).Position.Y;
                for (const FPinSlot& Pin : Node(Index).Pins)
                {
                    bHasFlowPin[Index] |= Pin.Kind == EWireKind::Flow;
                }
            }
        }

        void IndexWires()
        {
            const int32 WireCount = Graph.Wires.Num();
            bFlowWire.Init(false, WireCount);
            bSkippedWire.Init(true, WireCount);
            for (int32 WireIndex = 0; WireIndex < WireCount; ++WireIndex)
            {
                const FLayoutWire& W = Graph.Wires[WireIndex];
                const bool bNodesValid = Graph.Nodes.IsValidIndex(W.FromNode) && Graph.Nodes.IsValidIndex(W.ToNode)
                    && W.FromNode != W.ToNode;
                if (!bNodesValid
                    || !Node(W.FromNode).Pins.IsValidIndex(W.FromPin)
                    || !Node(W.ToNode).Pins.IsValidIndex(W.ToPin))
                {
                    continue;
                }
                const FPinSlot& From = Node(W.FromNode).Pins[W.FromPin];
                const FPinSlot& To = Node(W.ToNode).Pins[W.ToPin];
                if (From.Side != EPinSide::Output || To.Side != EPinSide::Input)
                {
                    continue;
                }
                bSkippedWire[WireIndex] = false;
                bFlowWire[WireIndex] = From.Kind == EWireKind::Flow && To.Kind == EWireKind::Flow;
                Outs[W.FromNode].Add({ WireIndex, W.ToNode, W.FromPin, W.ToPin });
                Ins[W.ToNode].Add({ WireIndex, W.FromNode, W.ToPin, W.FromPin });
            }

            auto ByPinThenIdentity = [this](const FAdjacent& A, const FAdjacent& B)
            {
                if (A.LocalPin != B.LocalPin) { return A.LocalPin < B.LocalPin; }
                if (A.Other != B.Other)
                {
                    const FString& KA = Node(A.Other).Key;
                    const FString& KB = Node(B.Other).Key;
                    if (KA != KB) { return KA < KB; }
                }
                if (A.OtherPin != B.OtherPin) { return A.OtherPin < B.OtherPin; }
                return A.Wire < B.Wire;
            };
            for (int32 Index = 0; Index < Num; ++Index)
            {
                Outs[Index].Sort(ByPinThenIdentity);
                Ins[Index].Sort(ByPinThenIdentity);
            }
        }

        void Enter(int32 Index, int32 FromWire, int32 Tree)
        {
            Visit[Index] = EVisit::Open;
            TreeOf[Index] = Tree;
            ParentWire[Index] = FromWire;
            Preorder[Index] = PreorderCounter++;
        }

        // Depth-first growth along flow output pins, in pin order. The first wire that reaches a
        // node adopts it; a wire into a node still open on the stack closes a cycle and is skipped
        // for layering.
        void GrowTree(int32 Root)
        {
            TreeRoots.Add(Root);
            const int32 Tree = TreeRoots.Num() - 1;
            Enter(Root, INDEX_NONE, Tree);

            TArray<TPair<int32, int32>> Stack;
            Stack.Emplace(Root, 0);
            while (Stack.Num() > 0)
            {
                const int32 Current = Stack.Last().Key;
                const int32 Cursor = Stack.Last().Value;
                if (Cursor >= Outs[Current].Num())
                {
                    Visit[Current] = EVisit::Closed;
                    Finished.Add(Current);
                    Stack.Pop();
                    continue;
                }
                Stack.Last().Value = Cursor + 1;

                const FAdjacent& Next = Outs[Current][Cursor];
                if (!bFlowWire[Next.Wire] || bSkippedWire[Next.Wire])
                {
                    continue;
                }
                if (Visit[Next.Other] == EVisit::Open)
                {
                    bSkippedWire[Next.Wire] = true;
                    continue;
                }
                if (Visit[Next.Other] == EVisit::Closed || !Node(Next.Other).bMovable)
                {
                    continue;
                }
                const int32 Child = Next.Other;
                Enter(Child, Next.Wire, Tree);
                Kids[Current].Add(Child);
                Stack.Emplace(Child, 0);
            }
        }

        bool FeedsFreshMovableFlowNode(int32 Index) const
        {
            for (const FAdjacent& Out : Outs[Index])
            {
                if (bFlowWire[Out.Wire] && !bSkippedWire[Out.Wire]
                    && Node(Out.Other).bMovable && Visit[Out.Other] == EVisit::Fresh)
                {
                    return true;
                }
            }
            return false;
        }

        bool HasMovableFlowPredecessor(int32 Index) const
        {
            for (const FAdjacent& In : Ins[Index])
            {
                if (bFlowWire[In.Wire] && !bSkippedWire[In.Wire] && Node(In.Other).bMovable)
                {
                    return true;
                }
            }
            return false;
        }

        void PlantFlowTrees()
        {
            for (int32 Root : Graph.Roots)
            {
                if (Graph.Nodes.IsValidIndex(Root) && Visit[Root] == EVisit::Fresh)
                {
                    GrowTree(Root);
                }
            }

            // Fixed nodes driving movable flow nodes anchor those nodes in place.
            TArray<int32> Anchors;
            for (int32 Index = 0; Index < Num; ++Index)
            {
                if (!Node(Index).bMovable && Visit[Index] == EVisit::Fresh && FeedsFreshMovableFlowNode(Index))
                {
                    Anchors.Add(Index);
                }
            }
            Anchors.Sort([this](int32 A, int32 B) { return PlacedBefore(A, B); });
            for (int32 Anchor : Anchors)
            {
                if (FeedsFreshMovableFlowNode(Anchor))
                {
                    GrowTree(Anchor);
                }
            }

            // Remaining movable flow nodes: sources first; whatever only a cycle reaches comes last.
            TArray<int32> Sources;
            TArray<bool> bHasPredecessor;
            bHasPredecessor.Init(false, Num);
            for (int32 Index = 0; Index < Num; ++Index)
            {
                if (Node(Index).bMovable && bHasFlowPin[Index] && Visit[Index] == EVisit::Fresh)
                {
                    Sources.Add(Index);
                    bHasPredecessor[Index] = HasMovableFlowPredecessor(Index);
                }
            }
            Sources.Sort([this, &bHasPredecessor](int32 A, int32 B)
            {
                if (bHasPredecessor[A] != bHasPredecessor[B]) { return !bHasPredecessor[A]; }
                return PlacedBefore(A, B);
            });
            for (int32 Source : Sources)
            {
                if (Visit[Source] == EVisit::Fresh)
                {
                    GrowTree(Source);
                }
            }

            // Flow depth, in topological order (reverse DFS finish order of the forest).
            for (int32 Cursor = Finished.Num() - 1; Cursor >= 0; --Cursor)
            {
                const int32 Index = Finished[Cursor];
                for (const FAdjacent& In : Ins[Index])
                {
                    if (bFlowWire[In.Wire] && !bSkippedWire[In.Wire] && IsMember(In.Other))
                    {
                        Rank[Index] = FMath::Max(Rank[Index], Rank[In.Other] + 1);
                    }
                }
            }
        }

        void AddDataRoot(int32 Index)
        {
            TreeRoots.Add(Index);
            Enter(Index, INDEX_NONE, TreeRoots.Num() - 1);
            Visit[Index] = EVisit::Closed;
            Finished.Add(Index);
        }

        // Breadth-first over input wires: every claimable data node upstream of Owner joins its
        // block, in pin order.
        void Claim(int32 Owner)
        {
            TArray<int32> Queue;
            Queue.Add(Owner);
            for (int32 Head = 0; Head < Queue.Num(); ++Head)
            {
                for (const FAdjacent& In : Ins[Queue[Head]])
                {
                    if (!bSkippedWire[In.Wire] && IsClaimable(In.Other))
                    {
                        BlockOwner[In.Other] = Owner;
                        Block[Owner].Add(In.Other);
                        Queue.Add(In.Other);
                    }
                }
            }
        }

        bool ConsumesClaimable(int32 Index) const
        {
            for (const FAdjacent& In : Ins[Index])
            {
                if (!bSkippedWire[In.Wire] && IsClaimable(In.Other))
                {
                    return true;
                }
            }
            return false;
        }

        void ClaimDataNodes()
        {
            TArray<int32> Members;
            for (int32 Index = 0; Index < Num; ++Index)
            {
                if (IsMember(Index))
                {
                    Members.Add(Index);
                }
            }
            Members.Sort([this](int32 A, int32 B)
            {
                if (TreeOf[A] != TreeOf[B]) { return TreeOf[A] < TreeOf[B]; }
                if (Rank[A] != Rank[B]) { return Rank[A] < Rank[B]; }
                return Preorder[A] < Preorder[B];
            });
            for (int32 Member : Members)
            {
                Claim(Member);
            }

            // Fixed consumers keep the data nodes that feed them.
            TArray<int32> Consumers;
            for (int32 Index = 0; Index < Num; ++Index)
            {
                if (!Node(Index).bMovable && !IsMember(Index) && ConsumesClaimable(Index))
                {
                    Consumers.Add(Index);
                }
            }
            Consumers.Sort([this](int32 A, int32 B) { return PlacedBefore(A, B); });
            for (int32 Consumer : Consumers)
            {
                if (ConsumesClaimable(Consumer))
                {
                    AddDataRoot(Consumer);
                    Claim(Consumer);
                }
            }

            // Whatever is left roots its own tree: sinks first.
            TArray<int32> Leftover;
            TArray<bool> bFeedsLeftover;
            bFeedsLeftover.Init(false, Num);
            for (int32 Index = 0; Index < Num; ++Index)
            {
                if (IsClaimable(Index))
                {
                    Leftover.Add(Index);
                }
            }
            for (int32 Index : Leftover)
            {
                for (const FAdjacent& Out : Outs[Index])
                {
                    bFeedsLeftover[Index] |= !bSkippedWire[Out.Wire] && IsClaimable(Out.Other);
                }
            }
            Leftover.Sort([this, &bFeedsLeftover](int32 A, int32 B)
            {
                if (bFeedsLeftover[A] != bFeedsLeftover[B]) { return !bFeedsLeftover[A]; }
                return PlacedBefore(A, B);
            });
            for (int32 Index : Leftover)
            {
                if (IsClaimable(Index))
                {
                    AddDataRoot(Index);
                    Claim(Index);
                }
            }
        }

        bool InBlockOf(int32 Index, int32 Owner) const
        {
            return Index == Owner || BlockOwner[Index] == Owner;
        }

        double PinFraction(int32 Index, int32 Pin) const
        {
            return PinY(Index, Pin) / FMath::Max(Height(Index), 1.0);
        }

        // Reorders one column by the mean slot of its neighbours in the adjacent column
        // (barycenter heuristic). Nodes with no such neighbour keep their slot as their weight.
        void SweepColumn(TArray<int32>& Column, int32 Owner, int32 NeighbourLevel, bool bTowardConsumers)
        {
            TMap<int32, double> WeightOf;
            for (int32 Index : Column)
            {
                double Sum = 0.0;
                int32 Count = 0;
                const TArray<FAdjacent>& Links = bTowardConsumers ? Outs[Index] : Ins[Index];
                for (const FAdjacent& Link : Links)
                {
                    if (!bSkippedWire[Link.Wire] && InBlockOf(Link.Other, Owner) && Level[Link.Other] == NeighbourLevel)
                    {
                        Sum += Slot[Link.Other] + PinFraction(Link.Other, Link.OtherPin);
                        ++Count;
                    }
                }
                WeightOf.Add(Index, Count > 0 ? Sum / Count : static_cast<double>(Slot[Index]));
            }
            Algo::StableSort(Column, [&WeightOf](int32 A, int32 B) { return WeightOf[A] < WeightOf[B]; });
            for (int32 Position = 0; Position < Column.Num(); ++Position)
            {
                Slot[Column[Position]] = Position;
            }
        }

        // Lays out Owner's data block relative to Owner: one right-aligned column per dependency
        // level, each node aligned to the pin of its first consumer where the column has room.
        void ShapeBlock(int32 Owner)
        {
            const TArray<int32>& Members = Block[Owner];
            if (Members.Num() == 0)
            {
                return;
            }

            // Topological order, consumers before their inputs; wires closing a data cycle are skipped.
            TArray<int32> FinishOrder;
            TArray<TPair<int32, int32>> Stack;
            BlockVisit[Owner] = EVisit::Open;
            Stack.Emplace(Owner, 0);
            while (Stack.Num() > 0)
            {
                const int32 Current = Stack.Last().Key;
                const int32 Cursor = Stack.Last().Value;
                if (Cursor >= Ins[Current].Num())
                {
                    BlockVisit[Current] = EVisit::Closed;
                    FinishOrder.Add(Current);
                    Stack.Pop();
                    continue;
                }
                Stack.Last().Value = Cursor + 1;
                const FAdjacent& In = Ins[Current][Cursor];
                if (bSkippedWire[In.Wire] || BlockOwner[In.Other] != Owner)
                {
                    continue;
                }
                if (BlockVisit[In.Other] == EVisit::Open)
                {
                    bSkippedWire[In.Wire] = true;
                }
                else if (BlockVisit[In.Other] == EVisit::Fresh)
                {
                    BlockVisit[In.Other] = EVisit::Open;
                    Stack.Emplace(In.Other, 0);
                }
            }

            int32 Depth = 0;
            Level[Owner] = 0;
            for (int32 Cursor = FinishOrder.Num() - 1; Cursor >= 0; --Cursor)
            {
                const int32 Current = FinishOrder[Cursor];
                for (const FAdjacent& In : Ins[Current])
                {
                    if (!bSkippedWire[In.Wire] && BlockOwner[In.Other] == Owner)
                    {
                        Level[In.Other] = FMath::Max(Level[In.Other], Level[Current] + 1);
                        Depth = FMath::Max(Depth, Level[In.Other]);
                    }
                }
            }

            TArray<TArray<int32>> Columns;
            Columns.SetNum(Depth + 1);
            Columns[0].Add(Owner);
            Slot[Owner] = 0;
            for (int32 Member : Members)
            {
                Slot[Member] = Columns[Level[Member]].Num();
                Columns[Level[Member]].Add(Member);
            }

            // Barycenter sweeps: toward the inputs, back toward the consumer, toward the inputs.
            for (int32 Column = 1; Column <= Depth; ++Column) { SweepColumn(Columns[Column], Owner, Column - 1, true); }
            for (int32 Column = Depth - 1; Column >= 1; --Column) { SweepColumn(Columns[Column], Owner, Column + 1, false); }
            for (int32 Column = 1; Column <= Depth; ++Column) { SweepColumn(Columns[Column], Owner, Column - 1, true); }

            double ColumnRight = -Spacing.DataColumnGap;
            double Reach = 0.0;
            for (int32 Column = 1; Column <= Depth; ++Column)
            {
                double ColumnLeft = ColumnRight;
                for (int32 Member : Columns[Column])
                {
                    RelX[Member] = SnapDown(ColumnRight - Width(Member));
                    ColumnLeft = FMath::Min(ColumnLeft, RelX[Member]);
                }
                Reach = -ColumnLeft;
                ColumnRight = ColumnLeft - Spacing.DataColumnGap;
            }
            BlockReach[Owner] = Reach;

            // A flow owner's incoming exec wire crosses the whole block at the owner's flow-input
            // row; every block node stays below it, so that wire never runs through a data node.
            double ExecClearance = TNumericLimits<double>::Lowest();
            for (const FPinSlot& Pin : Node(Owner).Pins)
            {
                if (Pin.Side == EPinSide::Input && Pin.Kind == EWireKind::Flow)
                {
                    ExecClearance = FMath::Max(ExecClearance, Pin.OffsetY + 0.25 * Spacing.RowGap);
                }
            }

            RelY[Owner] = 0.0;
            for (int32 Column = 1; Column <= Depth; ++Column)
            {
                bool bHasAbove = false;
                double Floor = 0.0;
                for (int32 Member : Columns[Column])
                {
                    const FAdjacent* Primary = nullptr;
                    for (const FAdjacent& Out : Outs[Member])
                    {
                        if (!bSkippedWire[Out.Wire] && InBlockOf(Out.Other, Owner) && Level[Out.Other] == Column - 1
                            && (!Primary || Slot[Out.Other] < Slot[Primary->Other]))
                        {
                            Primary = &Out;
                        }
                    }
                    double Top = Primary
                        ? RelY[Primary->Other] + PinY(Primary->Other, Primary->OtherPin) - PinY(Member, Primary->LocalPin)
                        : (bHasAbove ? Floor + Spacing.RowGap : 0.0);
                    if (bHasAbove && Top < Floor + Spacing.RowGap)
                    {
                        Top = SnapUp(Floor + Spacing.RowGap);
                    }
                    if (Top < ExecClearance)
                    {
                        Top = SnapUp(ExecClearance);
                    }
                    RelY[Member] = Top;
                    Floor = Top + Height(Member);
                    bHasAbove = true;
                }
            }
        }

        void AssignX()
        {
            for (int32 Cursor = Finished.Num() - 1; Cursor >= 0; --Cursor)
            {
                const int32 Index = Finished[Cursor];
                if (ParentWire[Index] == INDEX_NONE)
                {
                    PosX[Index] = Node(Index).bMovable ? SnapNear(Node(Index).Position.X) : Node(Index).Position.X;
                }
                else
                {
                    double Edge = TNumericLimits<double>::Lowest();
                    for (const FAdjacent& In : Ins[Index])
                    {
                        if (bFlowWire[In.Wire] && !bSkippedWire[In.Wire] && IsMember(In.Other))
                        {
                            Edge = FMath::Max(Edge, PosX[In.Other] + Width(In.Other) + Spacing.ColumnGap);
                        }
                    }
                    PosX[Index] = SnapUp(Edge + BlockReach[Index]);
                }
                for (int32 Member : Block[Index])
                {
                    PosX[Member] = PosX[Index] + RelX[Member];
                }
            }
        }

        // Marks Index placed; the live comments it is the first placed member of reserve their
        // frame around it from now on.
        void Occupy(int32 Index)
        {
            bPlaced[Index] = true;
            Occupied.Add({ PosX[Index], PosY[Index], PosX[Index] + Width(Index), PosY[Index] + Height(Index), Index, INDEX_NONE });
            for (int32 Comment : CommentsOf[Index])
            {
                if (bLiveComment[Comment] && CommentAnchor[Comment] == INDEX_NONE)
                {
                    CommentAnchor[Comment] = Index;
                    const FSpan Reach = ReservationReach(Comment, Index);
                    Occupied.Add({ FrameL[Comment], PosY[Index] - Reach.Lo, FrameR[Comment], PosY[Index] + Reach.Hi, INDEX_NONE, Comment });
                }
            }
        }

        FShape NodeShape(int32 Index, double Offset) const
        {
            return { PosX[Index], PosX[Index] + Width(Index), Offset, Offset + Height(Index), Index, INDEX_NONE };
        }

        // True when Rect does not constrain Shape: a node inside a comment ignores that comment's
        // frame, a reservation ignores its own members and comments nested with it.
        bool Ignores(const FShape& Shape, const FRect& Rect) const
        {
            if (Rect.Comment != INDEX_NONE)
            {
                return Shape.Comment == INDEX_NONE
                    ? bInComment[Rect.Comment][Shape.Node]
                    : IsWithin(Rect.Comment, Shape.Comment) || IsWithin(Shape.Comment, Rect.Comment);
            }
            return Shape.Comment != INDEX_NONE && bInComment[Shape.Comment][Rect.Node];
        }

        // Lowest offset >= Want at which every shape (X already fixed, Y relative to the offset)
        // clears every occupied rect it does not ignore by RowGap. Each such rect sharing an X range
        // with a shape blocks one open interval of offsets; one sweep over the intervals sorted by
        // their low end finds the first free offset. A pushed offset lands on the grid.
        // ponytail: O(group x occupied) per placement, so O(n^2) per graph; index the occupied
        // rects by X if graphs of thousands of nodes need arranging.
        double FirstFreeOffset(const TArray<FShape>& Shapes, double Want) const
        {
            TArray<FSpan> Blocked;
            for (const FShape& Shape : Shapes)
            {
                for (const FRect& Rect : Occupied)
                {
                    if (Rect.L < Shape.R && Shape.L < Rect.R && !Ignores(Shape, Rect))
                    {
                        Blocked.Add({ Rect.T - Spacing.RowGap - Shape.Bottom, Rect.B + Spacing.RowGap - Shape.Top });
                    }
                }
            }
            Blocked.Sort([](const FSpan& A, const FSpan& B) { return A.Lo < B.Lo; });
            double Offset = Want;
            for (const FSpan& Span : Blocked)
            {
                if (Span.Lo < Offset && Offset < Span.Hi)
                {
                    Offset = SnapUp(Span.Hi);
                }
            }
            return Offset;
        }

        // Lowest Y >= Want at which Index fits together with its spine: the chain of first flow
        // children, each pin-aligned to its parent. A node is never placed where its first child
        // could not follow on the same row, so a short node stacked under a taller neighbour
        // keeps its exec chain straight.
        // ponytail: the spine is re-collected per placement, O(depth) extra per node; cache the
        // spine offsets if long exec chains show up in profiles.
        double FirstFreeY(int32 Index, double Want) const
        {
            TArray<FShape> Shapes = { NodeShape(Index, 0.0) };
            AddReservations(Shapes, Index, 0.0);
            int32 Last = Index;
            double Offset = 0.0;
            while (Kids[Last].Num() > 0)
            {
                const int32 Child = Kids[Last][0];
                const FLayoutWire& In = Graph.Wires[ParentWire[Child]];
                Offset += PinY(In.FromNode, In.FromPin) - PinY(Child, In.ToPin);
                Shapes.Add(NodeShape(Child, Offset));
                Last = Child;
            }
            return FirstFreeOffset(Shapes, Want);
        }

        void PlaceBlock(int32 Owner)
        {
            const TArray<int32>& Members = Block[Owner];
            if (Members.Num() == 0)
            {
                return;
            }
            TArray<FShape> Shapes;
            for (int32 Member : Members)
            {
                Shapes.Add(NodeShape(Member, RelY[Member]));
                AddReservations(Shapes, Member, RelY[Member]);
            }
            const double Base = FirstFreeOffset(Shapes, PosY[Owner]);
            for (int32 Member : Members)
            {
                PosY[Member] = Base + RelY[Member];
                Occupy(Member);
            }
        }

        void AssignY()
        {
            TArray<int32> Fixed;
            for (int32 Index = 0; Index < Num; ++Index)
            {
                if (!Node(Index).bMovable)
                {
                    PosX[Index] = Node(Index).Position.X;
                    PosY[Index] = Node(Index).Position.Y;
                    Fixed.Add(Index);
                }
            }
            Fixed.Sort([this](int32 A, int32 B) { return PlacedBefore(A, B); });
            for (int32 Index : Fixed)
            {
                Occupy(Index);
            }
            for (int32 Comment = 0; Comment < Graph.Comments.Num(); ++Comment)
            {
                if (!bLiveComment[Comment])
                {
                    const FLayoutComment& C = Graph.Comments[Comment];
                    Occupied.Add({ C.Position.X, C.Position.Y, C.Position.X + C.Size.X, C.Position.Y + C.Size.Y, INDEX_NONE, Comment });
                }
            }

            for (int32 Tree = 0; Tree < TreeRoots.Num(); ++Tree)
            {
                const int32 Root = TreeRoots[Tree];
                if (Node(Root).bMovable)
                {
                    const double Want = SnapNear(Node(Root).Position.Y);
                    // The first tree's root stays put unless it would overlap something itself;
                    // later roots also make room for their exec spine below earlier trees.
                    TArray<FShape> Shapes = { NodeShape(Root, 0.0) };
                    AddReservations(Shapes, Root, 0.0);
                    PosY[Root] = Tree == 0 ? FirstFreeOffset(Shapes, Want) : FirstFreeY(Root, Want);
                    Occupy(Root);
                }
                PlaceBlock(Root);

                // Preorder: a node's whole subtree is placed before its next sibling, so each later
                // sibling settles below the subtrees it shares X range with.
                TArray<int32> Pending;
                for (int32 Kid = Kids[Root].Num() - 1; Kid >= 0; --Kid) { Pending.Add(Kids[Root][Kid]); }
                while (Pending.Num() > 0)
                {
                    const int32 Index = Pending.Pop();
                    const FLayoutWire& In = Graph.Wires[ParentWire[Index]];
                    const double Want = PosY[In.FromNode] + PinY(In.FromNode, In.FromPin) - PinY(Index, In.ToPin);
                    PosY[Index] = FirstFreeY(Index, Want);
                    Occupy(Index);
                    PlaceBlock(Index);
                    for (int32 Kid = Kids[Index].Num() - 1; Kid >= 0; --Kid) { Pending.Add(Kids[Index][Kid]); }
                }
            }
        }

        // True when Inner is Outer or nested (at any depth) inside it.
        bool IsWithin(int32 Inner, int32 Outer) const
        {
            for (int32 Comment = Inner; Comment != INDEX_NONE; Comment = Graph.Comments[Comment].Parent)
            {
                if (Comment == Outer)
                {
                    return true;
                }
            }
            return false;
        }

        // The comment's frame around its members' and nested comments' current rects (Y is only
        // meaningful once they are placed), inner comments fitted first. A fixed comment keeps
        // its own rect.
        FRect FitFrame(int32 Comment) const
        {
            const FLayoutComment& C = Graph.Comments[Comment];
            if (!bLiveComment[Comment])
            {
                return { C.Position.X, C.Position.Y, C.Position.X + C.Size.X, C.Position.Y + C.Size.Y };
            }
            FRect Box = { TNumericLimits<double>::Max(), TNumericLimits<double>::Max(),
                TNumericLimits<double>::Lowest(), TNumericLimits<double>::Lowest() };
            auto Extend = [&Box](const FRect& Rect)
            {
                Box.L = FMath::Min(Box.L, Rect.L);
                Box.T = FMath::Min(Box.T, Rect.T);
                Box.R = FMath::Max(Box.R, Rect.R);
                Box.B = FMath::Max(Box.B, Rect.B);
            };
            for (int32 Member : C.Members)
            {
                Extend({ PosX[Member], PosY[Member], PosX[Member] + Width(Member), PosY[Member] + Height(Member) });
            }
            for (int32 Child : ChildComments[Comment])
            {
                Extend(FitFrame(Child));
            }
            const double Pad = Spacing.CommentPad;
            return { Box.L - Pad, Box.T - Pad - C.TitleHeight, Box.R + Pad, Box.B + Pad };
        }

        // How far a comment's frame reaches above (Lo) and below (Hi) the top of its anchor
        // member: at least the nested title bars and pads around the anchor itself, at least
        // what the previous pass measured.
        FSpan ReservationReach(int32 Comment, int32 Index) const
        {
            double Above = 0.0;
            double Below = Height(Index);
            for (int32 Enclosing : CommentsOf[Index])
            {
                if (IsWithin(Enclosing, Comment))
                {
                    Above += Graph.Comments[Enclosing].TitleHeight + Spacing.CommentPad;
                    Below += Spacing.CommentPad;
                }
            }
            return { FMath::Max(Above, ReserveTop[Comment]), FMath::Max(Below, ReserveBottom[Comment]) };
        }

        // The reservations Index would anchor if it were placed at Offset now.
        void AddReservations(TArray<FShape>& Shapes, int32 Index, double Offset) const
        {
            for (int32 Comment : CommentsOf[Index])
            {
                const bool bListed = Shapes.ContainsByPredicate([Comment](const FShape& S) { return S.Comment == Comment; });
                if (bLiveComment[Comment] && CommentAnchor[Comment] == INDEX_NONE && !bListed)
                {
                    const FSpan Reach = ReservationReach(Comment, Index);
                    Shapes.Add({ FrameL[Comment], FrameR[Comment], Offset - Reach.Lo, Offset + Reach.Hi, INDEX_NONE, Comment });
                }
            }
        }

        // Membership tables and the frames' X ranges (X is final before any Y is assigned).
        void PrepareComments()
        {
            const int32 CommentNum = Graph.Comments.Num();
            bLiveComment.Init(false, CommentNum);
            bInComment.SetNum(CommentNum);
            CommentsOf.SetNum(Num);
            ChildComments.SetNum(CommentNum);
            CommentAnchor.Init(INDEX_NONE, CommentNum);
            FrameL.Init(0.0, CommentNum);
            FrameR.Init(0.0, CommentNum);
            ReserveTop.Init(0.0, CommentNum);
            ReserveBottom.Init(0.0, CommentNum);
            for (int32 Comment = 0; Comment < CommentNum; ++Comment)
            {
                FLayoutComment& C = Graph.Comments[Comment];
                if (!Graph.Comments.IsValidIndex(C.Parent) || C.Parent == Comment)
                {
                    C.Parent = INDEX_NONE;
                }
                else
                {
                    ChildComments[C.Parent].Add(Comment);
                }
                bInComment[Comment].Init(false, Num);
                TArray<int32> Members;
                for (int32 Member : C.Members)
                {
                    if (Graph.Nodes.IsValidIndex(Member) && !bInComment[Comment][Member])
                    {
                        bInComment[Comment][Member] = true;
                        Members.Add(Member);
                        CommentsOf[Member].Add(Comment);
                        bLiveComment[Comment] |= Node(Member).bMovable;
                    }
                }
                C.Members = Members;
            }
            // A comment whose nested comment is live is live too.
            for (int32 Comment = 0; Comment < CommentNum; ++Comment)
            {
                if (bLiveComment[Comment])
                {
                    for (int32 Outer = Graph.Comments[Comment].Parent; Outer != INDEX_NONE; Outer = Graph.Comments[Outer].Parent)
                    {
                        bLiveComment[Outer] = true;
                    }
                }
            }
            for (int32 Comment = 0; Comment < CommentNum; ++Comment)
            {
                if (bLiveComment[Comment])
                {
                    const FRect Frame = FitFrame(Comment);
                    FrameL[Comment] = Frame.L;
                    FrameR[Comment] = Frame.R;
                }
            }
        }

        // Widens every reservation its fitted frame outgrew. True when one grew.
        bool GrowReservations()
        {
            bool bGrew = false;
            for (int32 Comment = 0; Comment < Graph.Comments.Num(); ++Comment)
            {
                const int32 At = CommentAnchor[Comment];
                if (!bLiveComment[Comment] || At == INDEX_NONE)
                {
                    continue;
                }
                const FRect Frame = FitFrame(Comment);
                const FSpan Reach = ReservationReach(Comment, At);
                const double Above = PosY[At] - Frame.T;
                const double Below = Frame.B - PosY[At];
                if (Above > Reach.Lo + 0.01 || Below > Reach.Hi + 0.01)
                {
                    ReserveTop[Comment] = FMath::Max(Reach.Lo, Above);
                    ReserveBottom[Comment] = FMath::Max(Reach.Hi, Below);
                    bGrew = true;
                }
            }
            return bGrew;
        }

        void ResetPlacement()
        {
            Occupied.Reset();
            bPlaced.Init(false, Num);
            CommentAnchor.Init(INDEX_NONE, Graph.Comments.Num());
        }

        FArrangeReport Commit()
        {
            FArrangeReport Report;
            Report.Trees = TreeRoots.Num();
            for (int32 Index = 0; Index < Num; ++Index)
            {
                FLayoutNode& Target = Graph.Nodes[Index];
                (Target.bMeasuredSize ? Report.MeasuredSizes : Report.EstimatedSizes)++;
                if (!Target.bMovable || !ensure(bPlaced[Index]))
                {
                    continue;
                }
                const FVector2D Arranged(PosX[Index], PosY[Index]);
                if (!Arranged.Equals(Target.Position, 0.01))
                {
                    ++Report.Moved;
                    Target.Position = Arranged;
                }
            }
            for (int32 Comment = 0; Comment < Graph.Comments.Num(); ++Comment)
            {
                if (!bLiveComment[Comment])
                {
                    continue;
                }
                FLayoutComment& Target = Graph.Comments[Comment];
                const FRect Frame = FitFrame(Comment);
                const FVector2D Position(Frame.L, Frame.T);
                const FVector2D Size(Frame.R - Frame.L, Frame.B - Frame.T);
                Target.bRefit = !Position.Equals(Target.Position, 0.01) || !Size.Equals(Target.Size, 0.01);
                Target.Position = Position;
                Target.Size = Size;
                Report.CommentsRefit += Target.bRefit ? 1 : 0;
            }
            UE_LOG(LogPwGraphLayout, Verbose,
                TEXT("Arranged %d nodes in %d trees: moved=%d, comments refit=%d, sizes measured=%d estimated=%d"),
                Num, Report.Trees, Report.Moved, Report.CommentsRefit, Report.MeasuredSizes, Report.EstimatedSizes);
            return Report;
        }
    };
}

namespace PwGraphLayout
{
    FArrangeReport Arrange(FLayoutGraph& Graph, const FSpacing& Spacing)
    {
        return PwGraphLayoutCore::FArranger(Graph, Spacing).Run();
    }

    void RecordCommentMembers(FLayoutGraph& Graph)
    {
        auto Contains = [](const FLayoutComment& Outer, const FVector2D& Position, const FVector2D& Size)
        {
            return Position.X >= Outer.Position.X && Position.Y >= Outer.Position.Y
                && Position.X + Size.X <= Outer.Position.X + Outer.Size.X
                && Position.Y + Size.Y <= Outer.Position.Y + Outer.Size.Y;
        };
        auto Area = [](const FLayoutComment& C) { return C.Size.X * C.Size.Y; };
        // Outer encloses Inner: contains its rect and is larger, or equal and first by Key.
        auto Encloses = [&](const FLayoutComment& Outer, const FLayoutComment& Inner)
        {
            return Contains(Outer, Inner.Position, Inner.Size)
                && (Area(Outer) > Area(Inner) || (Area(Outer) == Area(Inner) && Outer.Key < Inner.Key));
        };

        for (int32 Comment = 0; Comment < Graph.Comments.Num(); ++Comment)
        {
            FLayoutComment& C = Graph.Comments[Comment];
            C.Members.Reset();
            C.Parent = INDEX_NONE;
            for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index)
            {
                const FLayoutNode& Node = Graph.Nodes[Index];
                const bool bUnplaced = Node.bMovable && Node.Position.IsZero();
                if (!bUnplaced && Contains(C, Node.Position, Node.Size))
                {
                    C.Members.Add(Index);
                }
            }
            for (int32 Other = 0; Other < Graph.Comments.Num(); ++Other)
            {
                const FLayoutComment& O = Graph.Comments[Other];
                if (Other == Comment || !Encloses(O, C))
                {
                    continue;
                }
                if (C.Parent == INDEX_NONE || Encloses(Graph.Comments[C.Parent], O))
                {
                    C.Parent = Other;
                }
            }
        }
    }
}
