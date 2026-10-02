// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwGraphLayoutTestUtils.h - Fixture builder and geometry checks shared by the PwGraphLayout core
// and adapter tests.

#pragma once

#include "CoreMinimal.h"
#include "Layout/GraphLayoutMetrics.h"
#include "Layout/PwGraphLayout.h"
#include "Misc/AutomationTest.h"

namespace PwGraphLayoutTest
{
    using namespace PwGraphLayout;

    // Synthetic node shape: header, then one row per pin on each side.
    constexpr double Header = 32.0;
    constexpr double Row = 24.0;

    inline double RowCentre(int32 RowIndex) { return Header + (RowIndex + 0.5) * Row; }

    struct FShape
    {
        int32 Node = INDEX_NONE;
        int32 FlowIn = INDEX_NONE;
        TArray<int32> FlowOuts;
        TArray<int32> DataIns;
        TArray<int32> DataOuts;
    };

    struct FFixture
    {
        FLayoutGraph Graph;

        // Flow pins take the top rows on each side, data pins follow.
        FShape Add(const TCHAR* Key, bool bFlowIn, int32 FlowOuts, int32 DataIns, int32 DataOuts,
            double Width = 160.0, bool bMovable = true, FVector2D Position = FVector2D::ZeroVector)
        {
            FShape Shape;
            Shape.Node = Graph.Nodes.Num();
            FLayoutNode& Node = Graph.Nodes.AddDefaulted_GetRef();
            Node.Key = Key;
            Node.bMovable = bMovable;
            Node.Position = Position;
            int32 InRow = 0;
            int32 OutRow = 0;
            if (bFlowIn) { Shape.FlowIn = Node.Pins.Add({ EPinSide::Input, EWireKind::Flow, RowCentre(InRow++) }); }
            for (int32 I = 0; I < FlowOuts; ++I) { Shape.FlowOuts.Add(Node.Pins.Add({ EPinSide::Output, EWireKind::Flow, RowCentre(OutRow++) })); }
            for (int32 I = 0; I < DataIns; ++I) { Shape.DataIns.Add(Node.Pins.Add({ EPinSide::Input, EWireKind::Data, RowCentre(InRow++) })); }
            for (int32 I = 0; I < DataOuts; ++I) { Shape.DataOuts.Add(Node.Pins.Add({ EPinSide::Output, EWireKind::Data, RowCentre(OutRow++) })); }
            Node.Size = FVector2D(Width, Header + (FMath::Max(FMath::Max(InRow, OutRow), 1) + 0.5) * Row);
            return Shape;
        }

        int32 Wire(int32 FromNode, int32 FromPin, int32 ToNode, int32 ToPin)
        {
            return Graph.Wires.Add({ FromNode, FromPin, ToNode, ToPin });
        }

        int32 Flow(const FShape& From, int32 Out, const FShape& To)
        {
            return Wire(From.Node, From.FlowOuts[Out], To.Node, To.FlowIn);
        }

        int32 Data(const FShape& From, int32 Out, const FShape& To, int32 In)
        {
            return Wire(From.Node, From.DataOuts[Out], To.Node, To.DataIns[In]);
        }
    };

    inline double Left(const FLayoutGraph& G, int32 N) { return G.Nodes[N].Position.X; }
    inline double Right(const FLayoutGraph& G, int32 N) { return G.Nodes[N].Position.X + G.Nodes[N].Size.X; }
    inline double Top(const FLayoutGraph& G, int32 N) { return G.Nodes[N].Position.Y; }
    inline double Bottom(const FLayoutGraph& G, int32 N) { return G.Nodes[N].Position.Y + G.Nodes[N].Size.Y; }

    // Pairs of node rects that overlap with positive area. Pairs of two fixed nodes are not the
    // layout's doing and are skipped.
    inline int32 CountOverlaps(const FLayoutGraph& G)
    {
        int32 Count = 0;
        for (int32 A = 0; A < G.Nodes.Num(); ++A)
        {
            for (int32 B = A + 1; B < G.Nodes.Num(); ++B)
            {
                if (!G.Nodes[A].bMovable && !G.Nodes[B].bMovable)
                {
                    continue;
                }
                if (Left(G, A) < Right(G, B) - 0.5 && Left(G, B) < Right(G, A) - 0.5
                    && Top(G, A) < Bottom(G, B) - 0.5 && Top(G, B) < Bottom(G, A) - 0.5)
                {
                    ++Count;
                }
            }
        }
        return Count;
    }

    // Wires whose source's right edge lies right of the target's left edge.
    inline int32 CountBackward(const FLayoutGraph& G, const TArray<int32>& Excused = {})
    {
        int32 Count = 0;
        for (int32 W = 0; W < G.Wires.Num(); ++W)
        {
            const FLayoutWire& Wire = G.Wires[W];
            if (!Excused.Contains(W) && Right(G, Wire.FromNode) > Left(G, Wire.ToNode) + 0.5)
            {
                ++Count;
            }
        }
        return Count;
    }

    inline double PinScreenY(const FLayoutGraph& G, int32 N, int32 Pin)
    {
        return G.Nodes[N].Position.Y + G.Nodes[N].Pins[Pin].OffsetY;
    }

    inline bool IsHorizontal(const FLayoutGraph& G, int32 WireIndex)
    {
        const FLayoutWire& W = G.Wires[WireIndex];
        return FMath::Abs(PinScreenY(G, W.FromNode, W.FromPin) - PinScreenY(G, W.ToNode, W.ToPin)) <= 1.0;
    }

    // Proper crossings between wire segments drawn pin to pin; wires sharing a node never count.
    inline int32 CountCrossings(const FLayoutGraph& G)
    {
        auto Segment = [&G](const FLayoutWire& W, FVector2D& A, FVector2D& B)
        {
            A = FVector2D(Right(G, W.FromNode), PinScreenY(G, W.FromNode, W.FromPin));
            B = FVector2D(Left(G, W.ToNode), PinScreenY(G, W.ToNode, W.ToPin));
        };
        auto Side = [](const FVector2D& P, const FVector2D& Q, const FVector2D& R)
        {
            const double Cross = (Q.X - P.X) * (R.Y - P.Y) - (Q.Y - P.Y) * (R.X - P.X);
            return Cross > 1e-6 ? 1 : (Cross < -1e-6 ? -1 : 0);
        };
        int32 Count = 0;
        for (int32 I = 0; I < G.Wires.Num(); ++I)
        {
            for (int32 J = I + 1; J < G.Wires.Num(); ++J)
            {
                const FLayoutWire& WI = G.Wires[I];
                const FLayoutWire& WJ = G.Wires[J];
                if (WI.FromNode == WJ.FromNode || WI.FromNode == WJ.ToNode || WI.ToNode == WJ.FromNode || WI.ToNode == WJ.ToNode)
                {
                    continue;
                }
                FVector2D A, B, C, D;
                Segment(WI, A, B);
                Segment(WJ, C, D);
                if (Side(A, B, C) * Side(A, B, D) < 0 && Side(C, D, A) * Side(C, D, B) < 0)
                {
                    ++Count;
                }
            }
        }
        return Count;
    }

    // Same graph with the node order reversed and rotated; wires and roots follow their nodes.
    inline FLayoutGraph Shuffled(const FLayoutGraph& G)
    {
        const int32 Num = G.Nodes.Num();
        TArray<int32> NewIndexOf;
        NewIndexOf.SetNum(Num);
        for (int32 Old = 0; Old < Num; ++Old)
        {
            NewIndexOf[Old] = (Num - 1 - Old + Num / 2) % FMath::Max(Num, 1);
        }
        FLayoutGraph Out;
        Out.Nodes.SetNum(Num);
        for (int32 Old = 0; Old < Num; ++Old)
        {
            Out.Nodes[NewIndexOf[Old]] = G.Nodes[Old];
        }
        for (int32 W = G.Wires.Num() - 1; W >= 0; --W)
        {
            const FLayoutWire& Wire = G.Wires[W];
            Out.Wires.Add({ NewIndexOf[Wire.FromNode], Wire.FromPin, NewIndexOf[Wire.ToNode], Wire.ToPin });
        }
        for (int32 Root : G.Roots)
        {
            Out.Roots.Add(NewIndexOf[Root]);
        }
        for (const FLayoutComment& Comment : G.Comments)
        {
            FLayoutComment& Copy = Out.Comments.Add_GetRef(Comment);
            for (int32& Member : Copy.Members)
            {
                Member = NewIndexOf[Member];
            }
        }
        return Out;
    }

    inline TArray<GraphLayout::FNodeRect> ToRects(const FLayoutGraph& G)
    {
        TArray<GraphLayout::FNodeRect> Rects;
        for (const FLayoutNode& Node : G.Nodes)
        {
            Rects.Emplace(Node.Key, Node.Position.X, Node.Position.Y, Node.Size.X, Node.Size.Y);
        }
        return Rects;
    }

    inline TArray<GraphLayout::FGraphEdge> ToEdges(const FLayoutGraph& G)
    {
        TArray<GraphLayout::FGraphEdge> Edges;
        for (const FLayoutWire& W : G.Wires)
        {
            Edges.Emplace(G.Nodes[W.FromNode].Key, G.Nodes[W.ToNode].Key);
        }
        return Edges;
    }

    // Arranges Fixture and checks the properties every fixture must have: no overlaps, no backward
    // wires except Excused, the same positions from a shuffled node order, and nothing moving on a
    // second pass. Returns the arranged graph.
    inline FLayoutGraph ArrangeAndCheck(FAutomationTestBase& Test, const FLayoutGraph& Fixture,
        const FSpacing& Spacing, const TArray<int32>& Excused = {})
    {
        FLayoutGraph Arranged = Fixture;
        const FArrangeReport Report = Arrange(Arranged, Spacing);
        Test.TestEqual(TEXT("every size is reported as estimated"), Report.EstimatedSizes, Fixture.Nodes.Num());
        Test.TestEqual(TEXT("no node rects overlap"), CountOverlaps(Arranged), 0);
        Test.TestEqual(TEXT("no wire runs backwards"), CountBackward(Arranged, Excused), 0);

        FLayoutGraph Reordered = Shuffled(Fixture);
        Arrange(Reordered, Spacing);
        for (const FLayoutNode& Node : Reordered.Nodes)
        {
            const FLayoutNode* Twin = Arranged.Nodes.FindByPredicate([&Node](const FLayoutNode& Other) { return Other.Key == Node.Key; });
            Test.TestTrue(*FString::Printf(TEXT("shuffled node order places %s identically"), *Node.Key),
                Twin && Twin->Position.Equals(Node.Position, 0.01));
        }

        FLayoutGraph Again = Arranged;
        const FArrangeReport Second = Arrange(Again, Spacing);
        Test.TestEqual(TEXT("a second pass moves nothing"), Second.Moved, 0);
        return Arranged;
    }

    // Spacing whose grid divides the synthetic row height, so pin-aligned rows stay on the grid.
    inline FSpacing TestSpacing()
    {
        FSpacing Spacing;
        Spacing.ColumnGap = 80.0;
        Spacing.RowGap = 32.0;
        Spacing.DataColumnGap = 48.0;
        Spacing.Grid = 8.0;
        return Spacing;
    }
}
