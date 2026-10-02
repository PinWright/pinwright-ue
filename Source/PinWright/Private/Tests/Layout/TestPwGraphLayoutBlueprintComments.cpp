// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestPwGraphLayoutBlueprintComments.cpp - Comment boxes through the K2 adapter (ArrangeEdGraph):
// a comment around a 3-node sub-chain with a nested comment inside it. After a layout that moves
// those nodes, each comment encloses exactly the members recorded before the layout, no other
// node touches a comment rect, and one undo restores both comment rects and every position.

#include "Misc/AutomationTest.h"

#include "BpirLayoutSettings.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "Editor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "Layout/PwGraphLayoutEdGraph.h"
#include "ScopedTransaction.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/Layout/PwGraphLayoutTestUtils.h"

namespace PwGraphLayoutBlueprintCommentTest
{
    using namespace PwGraphLayoutTest;

    // Members of the comment as the adapter records them from the current rects.
    inline TSet<UEdGraphNode*> MembersOf(const FEdGraphModel& Model, const UEdGraphNode_Comment* Comment)
    {
        TSet<UEdGraphNode*> Members;
        const int32 Index = Model.Comments.IndexOfByKey(Comment);
        if (Index != INDEX_NONE)
        {
            for (int32 Member : Model.Layout.Comments[Index].Members)
            {
                Members.Add(Model.Nodes[Member]);
            }
        }
        return Members;
    }

    inline FIntRect RectOf(const UEdGraphNode_Comment* Comment)
    {
        return FIntRect(Comment->NodePosX, Comment->NodePosY,
            Comment->NodePosX + Comment->NodeWidth, Comment->NodePosY + Comment->NodeHeight);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutBlueprintCommentsTest,
    "PinWright.layout.blueprint.CommentsRefitAroundMovedMembers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutBlueprintCommentsTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutBlueprintCommentTest;

    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("PwLayoutCommentsBP"));
    UEdGraph* Graph = (BP && BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!TestNotNull(TEXT("transient Blueprint with an event graph"), Graph))
    {
        return false;
    }

    // Event -> First -> Branch -> Second -> Last. The outer comment holds First, Branch and Second,
    // the inner one the Branch; the members start scattered inside their comments. Everything sits
    // at an offset (Ox, Oy) clear of the new Actor Blueprint's default event nodes near the origin,
    // which are fixed nodes and must not fall inside the comments.
    constexpr int32 Ox = 4000;
    constexpr int32 Oy = 4000;
    UK2Node_CustomEvent* Event = CompilerTestUtils::SpawnNode<UK2Node_CustomEvent>(Graph, Ox - 1200, Oy);
    Event->CustomFunctionName = TEXT("PwLayoutCommentEvent");
    Event->ReconstructNode();
    UK2Node_CallFunction* First = CompilerTestUtils::SpawnPrintStringCall(Graph, Ox - 160, Oy + 300);
    UK2Node_IfThenElse* Branch = CompilerTestUtils::SpawnNode<UK2Node_IfThenElse>(Graph, Ox + 420, Oy + 560);
    UK2Node_CallFunction* Second = CompilerTestUtils::SpawnPrintStringCall(Graph, Ox - 160, Oy + 900);
    UK2Node_CallFunction* Last = CompilerTestUtils::SpawnPrintStringCall(Graph, Ox + 1800, Oy);
    for (UK2Node_CallFunction* Call : { First, Second, Last })
    {
        Call->PostPlacedNewNode();
    }
    CompilerTestUtils::WireThenToExec(Event, First);
    CompilerTestUtils::WireThenToExec(First, Branch);
    CompilerTestUtils::WireThenToExec(Branch, Second);
    CompilerTestUtils::WireThenToExec(Second, Last);

    UEdGraphNode_Comment* Outer = CompilerTestUtils::SpawnNode<UEdGraphNode_Comment>(Graph, Ox - 240, Oy + 200);
    Outer->NodeWidth = 1500;
    Outer->NodeHeight = 1100;
    UEdGraphNode_Comment* Inner = CompilerTestUtils::SpawnNode<UEdGraphNode_Comment>(Graph, Ox + 360, Oy + 460);
    Inner->NodeWidth = 460;
    Inner->NodeHeight = 340;

    const TArray<UEdGraphNode*> Movable = { Event, First, Branch, Second, Last };
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        Node->SetFlags(RF_Transactional);
    }
    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();

    const FEdGraphModel Before = BuildEdGraphModel(Graph, TSet<UEdGraphNode*>(Movable), *Settings);
    const TSet<UEdGraphNode*> OuterMembers = { First, Branch, Second };
    const TSet<UEdGraphNode*> InnerMembers = { Branch };
    const bool bFixtureHolds = TestTrue(TEXT("the outer comment records the 3-node sub-chain"),
            MembersOf(Before, Outer).Num() == 3 && MembersOf(Before, Outer).Includes(OuterMembers))
        & TestTrue(TEXT("the inner comment records the branch only"),
            MembersOf(Before, Inner).Num() == 1 && MembersOf(Before, Inner).Includes(InnerMembers));
    if (!bFixtureHolds)
    {
        for (UEdGraphNode_Comment* Comment : { Outer, Inner })
        {
            for (const UEdGraphNode* Member : MembersOf(Before, Comment))
            {
                AddInfo(FString::Printf(TEXT("%s records %s at (%d, %d)"), *Comment->GetName(), *Member->GetName(), Member->NodePosX, Member->NodePosY));
            }
        }
        return false;
    }
    TMap<UEdGraphNode*, FIntPoint> PositionsBefore;
    for (UEdGraphNode* Node : Movable)
    {
        PositionsBefore.Add(Node, FIntPoint(Node->NodePosX, Node->NodePosY));
    }
    const FIntRect OuterBefore = RectOf(Outer);
    const FIntRect InnerBefore = RectOf(Inner);

    {
        FScopedTransaction Transaction(NSLOCTEXT("PinWrightTests", "PwLayoutComments", "Arrange commented graph"));
        const FArrangeReport Report = ArrangeEdGraph(Graph, Movable, { Event }, *Settings);
        TestTrue(TEXT("the layout moves the commented nodes"), Report.Moved > 0
            && FIntPoint(Branch->NodePosX, Branch->NodePosY) != PositionsBefore[Branch]);
        TestEqual(TEXT("both comments are re-fitted"), Report.CommentsRefit, 2);
    }

    const FEdGraphModel After = BuildEdGraphModel(Graph, TSet<UEdGraphNode*>(Movable), *Settings);
    TestTrue(TEXT("the outer comment encloses exactly its recorded members"), MembersOf(After, Outer).Num() == 3
        && MembersOf(After, Outer).Includes(OuterMembers));
    TestTrue(TEXT("the inner comment encloses exactly its recorded member"), MembersOf(After, Inner).Num() == 1
        && MembersOf(After, Inner).Includes(InnerMembers));
    for (UEdGraphNode_Comment* Comment : { Outer, Inner })
    {
        const FIntRect Rect = RectOf(Comment);
        const TSet<UEdGraphNode*> Members = MembersOf(After, Comment);
        for (int32 Index = 0; Index < After.Nodes.Num(); ++Index)
        {
            if (Members.Contains(After.Nodes[Index]))
            {
                continue;
            }
            const FLayoutGraph& G = After.Layout;
            const bool bTouches = Left(G, Index) < Rect.Max.X && Rect.Min.X < Right(G, Index)
                && Top(G, Index) < Rect.Max.Y && Rect.Min.Y < Bottom(G, Index);
            TestFalse(*FString::Printf(TEXT("non-member %s stays out of %s"), *After.Nodes[Index]->GetName(), *Comment->GetName()), bTouches);
        }
    }
    TestTrue(TEXT("the inner comment stays inside the outer one"), RectOf(Outer).Contains(RectOf(Inner).Min)
        && RectOf(Inner).Max.X <= RectOf(Outer).Max.X && RectOf(Inner).Max.Y <= RectOf(Outer).Max.Y);
    TestEqual(TEXT("a second pass re-fits nothing"), ArrangeEdGraph(Graph, Movable, { Event }, *Settings).CommentsRefit, 0);

    // The second pass wrote nothing, so the latest transaction is still the arrange.
    TestTrue(TEXT("undo succeeds"), GEditor && GEditor->UndoTransaction(/*bCanRedo=*/false));
    TestTrue(TEXT("undo restores the outer comment rect"), RectOf(Outer) == OuterBefore);
    TestTrue(TEXT("undo restores the inner comment rect"), RectOf(Inner) == InnerBefore);
    for (UEdGraphNode* Node : Movable)
    {
        TestTrue(*FString::Printf(TEXT("undo restores %s"), *Node->GetName()),
            FIntPoint(Node->NodePosX, Node->NodePosY) == PositionsBefore[Node]);
    }
    return true;
}
