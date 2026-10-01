// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestPwGraphLayoutCore.cpp - Synthetic-fixture tests for the PwGraphLayout core (no assets).
//
// Every fixture goes through ArrangeAndCheck: zero overlaps, zero backward wires (cycle back
// edges excused), identical positions from a shuffled node order, and a second pass that moves
// nothing. Each test then asserts its fixture's own shape: columns left to right in flow order,
// pin-aligned wires, siblings stacked below, data growing leftwards from its consumer.

#include "Misc/AutomationTest.h"

#include "Tests/Layout/PwGraphLayoutTestUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutLinearChainTest,
    "PinWright.layout.core.LinearChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutLinearChainTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutTest;

    FFixture F;
    const FShape Event = F.Add(TEXT("event"), false, 1, 0, 0);
    const FShape A = F.Add(TEXT("a"), true, 1, 0, 0, 240.0);
    const FShape B = F.Add(TEXT("b"), true, 1, 0, 0);
    const FShape C = F.Add(TEXT("c"), true, 0, 0, 0);
    const TArray<int32> Wires = { F.Flow(Event, 0, A), F.Flow(A, 0, B), F.Flow(B, 0, C) };
    F.Graph.Roots = { Event.Node };

    const FLayoutGraph G = ArrangeAndCheck(*this, F.Graph, TestSpacing());
    TestTrue(TEXT("the root keeps its position"), G.Nodes[Event.Node].Position.IsZero());
    for (int32 Wire : Wires)
    {
        TestTrue(*FString::Printf(TEXT("exec wire %d is horizontal"), Wire), IsHorizontal(G, Wire));
        TestTrue(*FString::Printf(TEXT("exec wire %d leaves a column gap"), Wire),
            Left(G, G.Wires[Wire].ToNode) >= Right(G, G.Wires[Wire].FromNode) + TestSpacing().ColumnGap);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutBranchTest,
    "PinWright.layout.core.Branch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutBranchTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutTest;

    FFixture F;
    const FShape Event = F.Add(TEXT("event"), false, 1, 0, 0);
    const FShape Branch = F.Add(TEXT("branch"), true, 2, 0, 0);
    const FShape OnTrue = F.Add(TEXT("on_true"), true, 0, 0, 0);
    const FShape OnFalse = F.Add(TEXT("on_false"), true, 0, 0, 0);
    const int32 ToBranch = F.Flow(Event, 0, Branch);
    const int32 TrueWire = F.Flow(Branch, 0, OnTrue);
    F.Flow(Branch, 1, OnFalse);
    F.Graph.Roots = { Event.Node };

    const FLayoutGraph G = ArrangeAndCheck(*this, F.Graph, TestSpacing());
    TestTrue(TEXT("event -> branch is horizontal"), IsHorizontal(G, ToBranch));
    TestTrue(TEXT("the first branch output is horizontal"), IsHorizontal(G, TrueWire));
    TestTrue(TEXT("the second branch output stacks below the first"),
        Top(G, OnFalse.Node) >= Bottom(G, OnTrue.Node) + TestSpacing().RowGap);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutSequenceTest,
    "PinWright.layout.core.Sequence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutSequenceTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutTest;

    FFixture F;
    const FShape Event = F.Add(TEXT("event"), false, 1, 0, 0);
    const FShape Sequence = F.Add(TEXT("sequence"), true, 3, 0, 0);
    const FShape Then0 = F.Add(TEXT("then0"), true, 1, 0, 0);
    const FShape Then0Next = F.Add(TEXT("then0_next"), true, 0, 0, 0, 320.0);
    const FShape Then1 = F.Add(TEXT("then1"), true, 0, 0, 0);
    const FShape Then2 = F.Add(TEXT("then2"), true, 0, 0, 0);
    F.Flow(Event, 0, Sequence);
    const int32 First = F.Flow(Sequence, 0, Then0);
    F.Flow(Then0, 0, Then0Next);
    F.Flow(Sequence, 1, Then1);
    F.Flow(Sequence, 2, Then2);
    F.Graph.Roots = { Event.Node };
    // A tangled input placement, for the crossing comparison.
    F.Graph.Nodes[Then2.Node].Position = FVector2D(400.0, -300.0);
    F.Graph.Nodes[Then0.Node].Position = FVector2D(400.0, 300.0);
    F.Graph.Nodes[Then1.Node].Position = FVector2D(900.0, 0.0);
    F.Graph.Nodes[Then0Next.Node].Position = FVector2D(200.0, -100.0);
    const int32 CrossingsBefore = CountCrossings(F.Graph);

    const FLayoutGraph G = ArrangeAndCheck(*this, F.Graph, TestSpacing());
    TestTrue(TEXT("then_0 is horizontal"), IsHorizontal(G, First));
    TestTrue(TEXT("outputs keep pin order top to bottom"),
        Top(G, Then0.Node) < Top(G, Then1.Node) && Top(G, Then1.Node) < Top(G, Then2.Node));
    TestTrue(TEXT("crossings are not worse than the input placement"), CountCrossings(G) <= CrossingsBefore);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutDiamondTest,
    "PinWright.layout.core.DiamondReconvergence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutDiamondTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutTest;

    FFixture F;
    const FShape Event = F.Add(TEXT("event"), false, 1, 0, 0);
    const FShape Branch = F.Add(TEXT("branch"), true, 2, 0, 0);
    const FShape OnTrue = F.Add(TEXT("on_true"), true, 1, 0, 0);
    const FShape OnFalse = F.Add(TEXT("on_false"), true, 1, 0, 0, 400.0);
    const FShape Merge = F.Add(TEXT("merge"), true, 0, 0, 0);
    F.Flow(Event, 0, Branch);
    const int32 TrueWire = F.Flow(Branch, 0, OnTrue);
    F.Flow(Branch, 1, OnFalse);
    const int32 TrueToMerge = F.Flow(OnTrue, 0, Merge);
    F.Flow(OnFalse, 0, Merge);
    F.Graph.Roots = { Event.Node };

    const FLayoutGraph G = ArrangeAndCheck(*this, F.Graph, TestSpacing());
    TestTrue(TEXT("the merge lands right of its right-most predecessor"),
        Left(G, Merge.Node) >= FMath::Max(Right(G, OnTrue.Node), Right(G, OnFalse.Node)) + TestSpacing().ColumnGap);
    TestTrue(TEXT("branch -> true is horizontal"), IsHorizontal(G, TrueWire));
    TestTrue(TEXT("true -> merge is horizontal"), IsHorizontal(G, TrueToMerge));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutExecLoopTest,
    "PinWright.layout.core.ExecLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutExecLoopTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutTest;

    FFixture F;
    const FShape Event = F.Add(TEXT("event"), false, 1, 0, 0);
    const FShape A = F.Add(TEXT("a"), true, 1, 0, 0);
    const FShape B = F.Add(TEXT("b"), true, 1, 0, 0);
    const FShape C = F.Add(TEXT("c"), true, 1, 0, 0);
    F.Flow(Event, 0, A);
    F.Flow(A, 0, B);
    F.Flow(B, 0, C);
    const int32 BackEdge = F.Flow(C, 0, A);
    F.Graph.Roots = { Event.Node };

    const FLayoutGraph G = ArrangeAndCheck(*this, F.Graph, TestSpacing(), { BackEdge });
    TestTrue(TEXT("the loop body still runs left to right"),
        Left(G, A.Node) < Left(G, B.Node) && Left(G, B.Node) < Left(G, C.Node));
    TestEqual(TEXT("only the back edge runs backwards"), CountBackward(G), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutThreeEventsTest,
    "PinWright.layout.core.ThreeEvents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutThreeEventsTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutTest;

    FFixture F;
    TArray<FShape> Events;
    TArray<int32> Bodies;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        // Overlapping start positions: every tree has to be laid out and stacked.
        Events.Add(F.Add(*FString::Printf(TEXT("event%d"), Index), false, 1, 0, 0, 160.0, true, FVector2D(0.0, Index * 10.0)));
        const FShape Body = F.Add(*FString::Printf(TEXT("body%d"), Index), true, 0, 0, 0);
        Bodies.Add(F.Flow(Events.Last(), 0, Body));
    }

    const FLayoutGraph G = ArrangeAndCheck(*this, F.Graph, TestSpacing());
    TestTrue(TEXT("the first event keeps its position"), G.Nodes[Events[0].Node].Position.IsZero());
    for (int32 Index = 0; Index < 3; ++Index)
    {
        TestEqual(TEXT("each event keeps its X"), G.Nodes[Events[Index].Node].Position.X, 0.0);
        TestTrue(*FString::Printf(TEXT("chain %d is laid out (horizontal exec wire)"), Index), IsHorizontal(G, Bodies[Index]));
    }
    TestTrue(TEXT("trees stack in root order"),
        Top(G, Events[0].Node) < Top(G, Events[1].Node) && Top(G, Events[1].Node) < Top(G, Events[2].Node));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutSharedDataTest,
    "PinWright.layout.core.SharedDataNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutSharedDataTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutTest;

    FFixture F;
    const FShape Event = F.Add(TEXT("event"), false, 1, 0, 0);
    const FShape First = F.Add(TEXT("first"), true, 1, 1, 0);
    const FShape Second = F.Add(TEXT("second"), true, 0, 1, 0);
    const FShape Shared = F.Add(TEXT("shared"), false, 0, 0, 1);
    F.Flow(Event, 0, First);
    F.Flow(First, 0, Second);
    const int32 ToFirst = F.Data(Shared, 0, First, 0);
    F.Data(Shared, 0, Second, 0);
    F.Graph.Roots = { Event.Node };

    const FLayoutGraph G = ArrangeAndCheck(*this, F.Graph, TestSpacing());
    TestTrue(TEXT("the shared node sits left of its first consumer"), Right(G, Shared.Node) <= Left(G, First.Node));
    TestTrue(TEXT("the shared node sits right of the event"), Left(G, Shared.Node) >= Right(G, Event.Node));
    // The event -> first exec wire crosses the shared node's column at First's exec row; the data
    // node stays below it rather than straddling it (so its own wire bends slightly).
    TestTrue(TEXT("the shared node stays below the incoming exec wire"),
        Top(G, Shared.Node) > PinScreenY(G, First.Node, First.FlowIn));
    TestTrue(TEXT("the wire to the first consumer still runs forwards"), Right(G, Shared.Node) <= Left(G, G.Wires[ToFirst].ToNode));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutDataChainTest,
    "PinWright.layout.core.DataChainThreeDeep",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutDataChainTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutTest;

    FFixture F;
    const FShape Event = F.Add(TEXT("event"), false, 1, 0, 0);
    const FShape Consumer = F.Add(TEXT("consumer"), true, 0, 1, 0);
    const FShape D1 = F.Add(TEXT("d1"), false, 0, 1, 1);
    const FShape D2 = F.Add(TEXT("d2"), false, 0, 1, 1, 220.0);
    const FShape D3 = F.Add(TEXT("d3"), false, 0, 0, 1);
    F.Flow(Event, 0, Consumer);
    F.Data(D1, 0, Consumer, 0);
    const TArray<int32> Wires = { F.Data(D2, 0, D1, 0), F.Data(D3, 0, D2, 0) };
    F.Graph.Roots = { Event.Node };

    const FLayoutGraph G = ArrangeAndCheck(*this, F.Graph, TestSpacing());
    TestTrue(TEXT("one column per dependency level, leftwards"),
        Right(G, D3.Node) <= Left(G, D2.Node) && Right(G, D2.Node) <= Left(G, D1.Node) && Right(G, D1.Node) <= Left(G, Consumer.Node));
    TestTrue(TEXT("the chain fits between the event and its consumer"), Left(G, D3.Node) >= Right(G, Event.Node));
    for (int32 Wire : Wires)
    {
        TestTrue(*FString::Printf(TEXT("data wire %d inside the chain is horizontal"), Wire), IsHorizontal(G, Wire));
    }
    // The event -> consumer exec wire crosses all three columns at the consumer's exec row; the
    // chain sits below it instead of having that wire run through its nodes.
    for (const FShape& Data : { D1, D2, D3 })
    {
        TestTrue(*FString::Printf(TEXT("%s stays below the incoming exec wire"), *G.Nodes[Data.Node].Key),
            Top(G, Data.Node) > PinScreenY(G, Consumer.Node, Consumer.FlowIn));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutMaterialChainTest,
    "PinWright.layout.core.MaterialMathChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutMaterialChainTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutTest;

    FFixture F;
    const FShape Output = F.Add(TEXT("output"), false, 0, 2, 0, 240.0, false, FVector2D(0.0, 0.0));
    const FShape Texture = F.Add(TEXT("texture"), false, 0, 1, 5, 200.0);
    const FShape Tint = F.Add(TEXT("tint"), false, 0, 0, 1);
    const FShape Multiply = F.Add(TEXT("multiply"), false, 0, 2, 1);
    const FShape Add = F.Add(TEXT("add"), false, 0, 2, 1);
    const FShape Bias = F.Add(TEXT("bias"), false, 0, 0, 1);
    const FShape Roughness = F.Add(TEXT("roughness"), false, 0, 0, 1);
    const FShape Uv = F.Add(TEXT("uv"), false, 0, 0, 1);
    F.Data(Uv, 0, Texture, 0);
    const int32 TextureWire = F.Data(Texture, 0, Multiply, 0);
    F.Data(Tint, 0, Multiply, 1);
    const int32 MultiplyWire = F.Data(Multiply, 0, Add, 0);
    F.Data(Bias, 0, Add, 1);
    const int32 AddWire = F.Data(Add, 0, Output, 0);
    F.Data(Roughness, 0, Output, 1);
    F.Graph.Roots = { Output.Node };
    // The old fixed-grid placement: depth columns growing right, lanes by name.
    F.Graph.Nodes[Uv.Node].Position = FVector2D(320.0, 360.0);
    F.Graph.Nodes[Bias.Node].Position = FVector2D(320.0, 0.0);
    F.Graph.Nodes[Roughness.Node].Position = FVector2D(320.0, 180.0);
    F.Graph.Nodes[Tint.Node].Position = FVector2D(320.0, 540.0);
    F.Graph.Nodes[Texture.Node].Position = FVector2D(640.0, 0.0);
    F.Graph.Nodes[Multiply.Node].Position = FVector2D(960.0, 0.0);
    F.Graph.Nodes[Add.Node].Position = FVector2D(1280.0, 0.0);
    const int32 CrossingsBefore = CountCrossings(F.Graph);

    const FLayoutGraph G = ArrangeAndCheck(*this, F.Graph, TestSpacing());
    TestTrue(TEXT("the output node stays put"), G.Nodes[Output.Node].Position.IsZero());
    for (int32 Node = 0; Node < G.Nodes.Num(); ++Node)
    {
        if (Node != Output.Node)
        {
            TestTrue(*FString::Printf(TEXT("%s grows leftwards from the output"), *G.Nodes[Node].Key),
                Right(G, Node) <= Left(G, Output.Node));
        }
    }
    TestTrue(TEXT("add -> output is horizontal"), IsHorizontal(G, AddWire));
    TestTrue(TEXT("multiply -> add is horizontal"), IsHorizontal(G, MultiplyWire));
    TestTrue(TEXT("texture -> multiply is horizontal"), IsHorizontal(G, TextureWire));
    TestTrue(TEXT("crossings are not worse than the input placement"), CountCrossings(G) <= CrossingsBefore);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutAnimPoseChainTest,
    "PinWright.layout.core.AnimPoseChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutAnimPoseChainTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutTest;

    FFixture F;
    // The pose sink is a fixed node that no caller names as a root.
    const FShape Result = F.Add(TEXT("result"), false, 0, 1, 0, 160.0, false, FVector2D(800.0, 96.0));
    const FShape Slot = F.Add(TEXT("slot"), false, 0, 1, 1);
    const FShape Blend = F.Add(TEXT("blend"), false, 0, 3, 1, 200.0);
    const FShape PoseA = F.Add(TEXT("pose_a"), false, 0, 0, 1, 240.0);
    const FShape PoseB = F.Add(TEXT("pose_b"), false, 0, 0, 1, 240.0);
    const FShape Alpha = F.Add(TEXT("alpha"), false, 0, 0, 1);
    const int32 SlotWire = F.Data(Slot, 0, Result, 0);
    const int32 BlendWire = F.Data(Blend, 0, Slot, 0);
    const int32 PoseAWire = F.Data(PoseA, 0, Blend, 0);
    F.Data(PoseB, 0, Blend, 1);
    F.Data(Alpha, 0, Blend, 2);

    const FLayoutGraph G = ArrangeAndCheck(*this, F.Graph, TestSpacing());
    TestTrue(TEXT("the pose chain grows leftwards from the sink"),
        Right(G, Slot.Node) <= Left(G, Result.Node) && Right(G, Blend.Node) <= Left(G, Slot.Node)
        && Right(G, PoseA.Node) <= Left(G, Blend.Node) && Right(G, PoseB.Node) <= Left(G, Blend.Node));
    TestTrue(TEXT("slot -> result is horizontal"), IsHorizontal(G, SlotWire));
    TestTrue(TEXT("blend -> slot is horizontal"), IsHorizontal(G, BlendWire));
    TestTrue(TEXT("first pose -> blend is horizontal"), IsHorizontal(G, PoseAWire));
    TestTrue(TEXT("inputs keep pin order top to bottom"),
        Top(G, PoseA.Node) < Top(G, PoseB.Node) && Top(G, PoseB.Node) < Top(G, Alpha.Node));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutObstacleTest,
    "PinWright.layout.core.FixedObstacleAvoided",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutObstacleTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutTest;

    FFixture F;
    const FShape Event = F.Add(TEXT("event"), false, 1, 0, 0);
    const FShape A = F.Add(TEXT("a"), true, 1, 0, 0);
    const FShape B = F.Add(TEXT("b"), true, 0, 0, 0);
    // Exactly where A would land beside the event.
    const FShape Obstacle = F.Add(TEXT("obstacle"), true, 1, 0, 0, 160.0, false, FVector2D(240.0, 0.0));
    F.Flow(Event, 0, A);
    const int32 AToB = F.Flow(A, 0, B);
    F.Graph.Roots = { Event.Node };

    const FLayoutGraph G = ArrangeAndCheck(*this, F.Graph, TestSpacing());
    TestTrue(TEXT("the fixed node is not moved"), G.Nodes[Obstacle.Node].Position.Equals(FVector2D(240.0, 0.0)));
    TestTrue(TEXT("the root, clear of the obstacle's columns, keeps its position"), G.Nodes[Event.Node].Position.IsZero());
    TestTrue(TEXT("the chain settles below the obstacle"),
        Top(G, A.Node) >= Bottom(G, Obstacle.Node) + TestSpacing().RowGap);
    TestTrue(TEXT("the chain past the obstacle stays horizontal"), IsHorizontal(G, AToB));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutMetricsDirectionTest,
    "PinWright.layout.core.MetricsScoreBrokenLayoutsWorse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutMetricsDirectionTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutTest;

    FFixture F;
    const FShape Event = F.Add(TEXT("event"), false, 1, 0, 0);
    const FShape Branch = F.Add(TEXT("branch"), true, 2, 0, 0);
    const FShape OnTrue = F.Add(TEXT("on_true"), true, 0, 1, 0);
    const FShape OnFalse = F.Add(TEXT("on_false"), true, 0, 0, 0);
    const FShape Value = F.Add(TEXT("value"), false, 0, 0, 1);
    F.Flow(Event, 0, Branch);
    F.Flow(Branch, 0, OnTrue);
    F.Flow(Branch, 1, OnFalse);
    F.Data(Value, 0, OnTrue, 0);
    F.Graph.Roots = { Event.Node };

    FLayoutGraph Arranged = F.Graph;
    Arrange(Arranged, TestSpacing());
    const GraphLayout::FGraphLayoutMetricsResult Good =
        GraphLayout::ComputeGraphLayoutMetrics(ToRects(Arranged), ToEdges(Arranged), TestSpacing().Grid);
    TestFalse(TEXT("the arranged fixture has no overlap"), Good.HasOverlap());

    FLayoutGraph Overlapping = Arranged;
    Overlapping.Nodes[OnFalse.Node].Position = Overlapping.Nodes[OnTrue.Node].Position + FVector2D(16.0, 16.0);
    const GraphLayout::FGraphLayoutMetricsResult Bad =
        GraphLayout::ComputeGraphLayoutMetrics(ToRects(Overlapping), ToEdges(Overlapping), TestSpacing().Grid);
    TestTrue(TEXT("the overlapping copy is flagged"), Bad.HasOverlap());
    TestTrue(TEXT("the overlapping copy scores worse"), Bad.CombinedScore < Good.CombinedScore);

    FLayoutGraph Backward = Arranged;
    Swap(Backward.Nodes[Event.Node].Position, Backward.Nodes[Branch.Node].Position);
    TestEqual(TEXT("the arranged fixture has no backward wire"), CountBackward(Arranged), 0);
    TestTrue(TEXT("the swapped copy has a backward wire"), CountBackward(Backward) > 0);
    return true;
}

// The adapters' "only nodes still at (0,0) move" rule: after one pass the arranged pose chain is
// fixed and only the sink at the origin is movable again. Fixed nodes block only the columns they
// stand in, so the second pass must leave the sink where it is.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutOriginRuleRepassTest,
    "PinWright.layout.core.OriginOnlyRepassMovesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutOriginRuleRepassTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutTest;

    FFixture F;
    const FShape Result = F.Add(TEXT("result"), false, 0, 1, 0);
    const FShape Blend = F.Add(TEXT("blend"), false, 0, 3, 1, 200.0);
    const FShape PoseA = F.Add(TEXT("pose_a"), false, 0, 0, 1, 240.0);
    const FShape PoseB = F.Add(TEXT("pose_b"), false, 0, 0, 1, 240.0);
    F.Data(Blend, 0, Result, 0);
    F.Data(PoseA, 0, Blend, 0);
    F.Data(PoseB, 0, Blend, 1);

    FLayoutGraph G = F.Graph;
    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        for (FLayoutNode& Node : G.Nodes)
        {
            Node.bMovable = Node.Position.IsZero();
        }
        const FArrangeReport Report = Arrange(G, TestSpacing());
        if (Pass == 1)
        {
            TestEqual(TEXT("the second pass moves nothing"), Report.Moved, 0);
        }
    }
    TestTrue(TEXT("the sink stays at the origin"), G.Nodes[Result.Node].Position.IsZero());
    TestEqual(TEXT("no node rects overlap"), CountOverlaps(G), 0);
    return true;
}

// Trees stack below one another; a short root must not leave its taller first child to collide
// with the tree above. The root is placed together with its pin-aligned exec spine, so every
// stacked chain stays horizontal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutStackedSpinesTest,
    "PinWright.layout.core.StackedTreesKeepSpinesStraight",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutStackedSpinesTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutTest;

    FFixture F;
    TArray<int32> Spines;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        // A one-row event feeding a three-row call: the call hangs below its event.
        const FShape Event = F.Add(*FString::Printf(TEXT("event%d"), Index), false, 1, 0, 0);
        const FShape Call = F.Add(*FString::Printf(TEXT("call%d"), Index), true, 1, 2, 0);
        const FShape Next = F.Add(*FString::Printf(TEXT("next%d"), Index), true, 0, 4, 0);
        Spines.Add(F.Flow(Event, 0, Call));
        Spines.Add(F.Flow(Call, 0, Next));
        F.Graph.Roots.Add(Event.Node);
    }

    const FLayoutGraph G = ArrangeAndCheck(*this, F.Graph, TestSpacing());
    for (int32 Wire : Spines)
    {
        TestTrue(*FString::Printf(TEXT("stacked exec wire %d is horizontal"), Wire), IsHorizontal(G, Wire));
    }
    return true;
}
