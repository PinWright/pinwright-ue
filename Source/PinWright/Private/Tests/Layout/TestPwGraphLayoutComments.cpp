// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestPwGraphLayoutComments.cpp - Comment boxes in the PwGraphLayout core (no assets): membership
// recorded from rects, re-fit around moved members (nested comments inner first), non-members
// kept out of every frame, and comments with only fixed members treated as obstacles.

#include "Misc/AutomationTest.h"

#include "Tests/Layout/PwGraphLayoutTestUtils.h"

namespace PwGraphLayoutCommentTest
{
    using namespace PwGraphLayoutTest;

    inline bool Intersects(const FLayoutGraph& G, int32 Node, const FLayoutComment& C)
    {
        return Left(G, Node) < C.Position.X + C.Size.X && C.Position.X < Right(G, Node)
            && Top(G, Node) < C.Position.Y + C.Size.Y && C.Position.Y < Bottom(G, Node);
    }

    inline bool Encloses(const FLayoutComment& C, double L, double T, double R, double B)
    {
        return L >= C.Position.X && T >= C.Position.Y && R <= C.Position.X + C.Size.X && B <= C.Position.Y + C.Size.Y;
    }

    // Every member lies inside its comment and every other node stays clear of it.
    inline void CheckFrames(FAutomationTestBase& Test, const FLayoutGraph& G)
    {
        for (const FLayoutComment& C : G.Comments)
        {
            for (int32 Node = 0; Node < G.Nodes.Num(); ++Node)
            {
                const FString& Key = G.Nodes[Node].Key;
                if (C.Members.Contains(Node))
                {
                    Test.TestTrue(*FString::Printf(TEXT("%s encloses member %s"), *C.Key, *Key),
                        Encloses(C, Left(G, Node), Top(G, Node), Right(G, Node), Bottom(G, Node)));
                }
                else
                {
                    Test.TestFalse(*FString::Printf(TEXT("non-member %s stays out of %s"), *Key, *C.Key), Intersects(G, Node, C));
                }
            }
        }
    }

    inline FLayoutComment& AddComment(FFixture& F, const TCHAR* Key, double X, double Y, double W, double H)
    {
        FLayoutComment& C = F.Graph.Comments.AddDefaulted_GetRef();
        C.Key = Key;
        C.Position = FVector2D(X, Y);
        C.Size = FVector2D(W, H);
        return C;
    }
}

// Event -> Branch -> B -> C -> D, Branch's second output -> E. An outer comment holds the Branch,
// B and C (a 3-node sub-chain), a nested one holds B. A data node feeds B from outside both, and
// a fixed node sits where only the full outer frame (two title bars deep) would reach it. Before
// the layout the members are scattered inside their comments, so the layout moves them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutCommentRefitNestedTest,
    "PinWright.layout.core.CommentRefitNested",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutCommentRefitNestedTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutCommentTest;

    FFixture F;
    const FShape Event = F.Add(TEXT("event"), false, 1, 0, 0, 160.0, true, FVector2D(-480.0, 0.0));
    const FShape Branch = F.Add(TEXT("branch"), true, 2, 0, 0, 160.0, true, FVector2D(-80.0, 360.0));
    const FShape B = F.Add(TEXT("b"), true, 1, 1, 0, 160.0, true, FVector2D(160.0, 560.0));
    const FShape C = F.Add(TEXT("c"), true, 1, 0, 0, 160.0, true, FVector2D(-40.0, 720.0));
    const FShape D = F.Add(TEXT("d"), true, 0, 0, 0, 160.0, true, FVector2D(900.0, 0.0));
    const FShape E = F.Add(TEXT("e"), true, 0, 0, 0, 160.0, true, FVector2D(900.0, 300.0));
    const FShape Data = F.Add(TEXT("data"), false, 0, 0, 1, 160.0, true, FVector2D(-600.0, 900.0));
    // A fixed node just above the branch's own title bar but inside the outer frame's reach once
    // the nested title bar is added: the first reservation misses it, the grown one must not.
    const FShape Above = F.Add(TEXT("above"), false, 0, 0, 0, 160.0, false, FVector2D(-200.0, -160.0));
    F.Flow(Event, 0, Branch);
    const TArray<int32> Chain = { F.Flow(Branch, 0, B), F.Flow(B, 0, C) };
    F.Flow(C, 0, D);
    F.Flow(Branch, 1, E);
    F.Data(Data, 0, B, 0);
    F.Graph.Roots = { Event.Node };
    AddComment(F, TEXT("outer"), -160.0, 280.0, 560.0, 640.0);
    AddComment(F, TEXT("inner"), 120.0, 500.0, 240.0, 200.0);
    RecordCommentMembers(F.Graph);

    TestEqual(TEXT("the outer comment records the 3-node sub-chain"), F.Graph.Comments[0].Members.Num(), 3);
    TestTrue(TEXT("the outer comment records the branch, b and c"),
        F.Graph.Comments[0].Members.Contains(Branch.Node) && F.Graph.Comments[0].Members.Contains(B.Node)
        && F.Graph.Comments[0].Members.Contains(C.Node));
    TestTrue(TEXT("the inner comment records b only"),
        F.Graph.Comments[1].Members.Num() == 1 && F.Graph.Comments[1].Members.Contains(B.Node));
    TestEqual(TEXT("the inner comment nests in the outer one"), F.Graph.Comments[1].Parent, 0);
    TestEqual(TEXT("the outer comment is top level"), F.Graph.Comments[0].Parent, static_cast<int32>(INDEX_NONE));

    FLayoutGraph Probe = F.Graph;
    const FArrangeReport Report = Arrange(Probe, TestSpacing());
    TestEqual(TEXT("both comments are re-fitted"), Report.CommentsRefit, 2);

    const FLayoutGraph G = ArrangeAndCheck(*this, F.Graph, TestSpacing());
    CheckFrames(*this, G);
    TestFalse(TEXT("the fixed node above stays out of the outer frame"), Intersects(G, Above.Node, G.Comments[0]));
    const FLayoutComment& Outer = G.Comments[0];
    const FLayoutComment& Inner = G.Comments[1];
    TestTrue(TEXT("the inner comment lies inside the outer one"),
        Encloses(Outer, Inner.Position.X, Inner.Position.Y, Inner.Position.X + Inner.Size.X, Inner.Position.Y + Inner.Size.Y));
    TestTrue(TEXT("the inner title bar sits above b"),
        Inner.Position.Y <= Top(G, B.Node) - Inner.TitleHeight - TestSpacing().CommentPad + 0.01);
    TestTrue(TEXT("the outer title bar sits above the inner comment"),
        Outer.Position.Y <= Inner.Position.Y - Outer.TitleHeight - TestSpacing().CommentPad + 0.01);
    for (int32 Wire : Chain)
    {
        TestTrue(*FString::Printf(TEXT("chain wire %d inside the comment stays horizontal"), Wire), IsHorizontal(G, Wire));
    }

    // Membership read back from the arranged rects is exactly what was recorded, and a second
    // pass from that reading moves nothing and re-fits nothing.
    FLayoutGraph Again = G;
    RecordCommentMembers(Again);
    for (int32 Index = 0; Index < G.Comments.Num(); ++Index)
    {
        TestTrue(*FString::Printf(TEXT("%s still encloses exactly its members"), *G.Comments[Index].Key),
            Again.Comments[Index].Members.Num() == G.Comments[Index].Members.Num()
            && Again.Comments[Index].Parent == G.Comments[Index].Parent);
    }
    const FArrangeReport Second = Arrange(Again, TestSpacing());
    TestEqual(TEXT("a second pass moves no node"), Second.Moved, 0);
    TestEqual(TEXT("a second pass re-fits no comment"), Second.CommentsRefit, 0);
    return true;
}

// A comment around a fixed node is left alone, and the movable chain keeps out of it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutCommentFixedObstacleTest,
    "PinWright.layout.core.CommentWithFixedMembersIsObstacle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutCommentFixedObstacleTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutCommentTest;

    FFixture F;
    const FShape Event = F.Add(TEXT("event"), false, 1, 0, 0);
    const FShape A = F.Add(TEXT("a"), true, 1, 0, 0);
    const FShape B = F.Add(TEXT("b"), true, 0, 0, 0);
    // The fixed node sits clear of B's row, but its comment reaches across it.
    F.Add(TEXT("fixed"), false, 0, 0, 0, 160.0, false, FVector2D(560.0, 160.0));
    F.Flow(Event, 0, A);
    F.Flow(A, 0, B);
    F.Graph.Roots = { Event.Node };
    AddComment(F, TEXT("fixed_comment"), 480.0, -64.0, 400.0, 360.0);
    RecordCommentMembers(F.Graph);
    const FLayoutComment Before = F.Graph.Comments[0];
    TestEqual(TEXT("the comment holds only the fixed node"), Before.Members.Num(), 1);

    FLayoutGraph Plain = F.Graph;
    Plain.Comments.Reset();
    Arrange(Plain, TestSpacing());
    TestTrue(TEXT("without the comment, b lands inside its rect (the fixture exercises the obstacle)"),
        Intersects(Plain, B.Node, Before));

    const FLayoutGraph G = ArrangeAndCheck(*this, F.Graph, TestSpacing());
    TestTrue(TEXT("the comment keeps its rect"),
        G.Comments[0].Position.Equals(Before.Position, 0.01) && G.Comments[0].Size.Equals(Before.Size, 0.01) && !G.Comments[0].bRefit);
    CheckFrames(*this, G);
    return true;
}
