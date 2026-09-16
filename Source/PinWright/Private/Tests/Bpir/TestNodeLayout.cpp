// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestNodeLayout.cpp - Unit tests for NodeLayoutEngine::EstimateNodeSize

#include "Misc/AutomationTest.h"

#include "CompilerTestUtils.h"
#include "BpirGraphTestHelpers.h"
#include "Compiler/NodeLayoutEngine.h"
#include "BpirLayoutSettings.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "Compiler/NodeLayoutParameterFormatter.h"
#include "Kismet/KismetMathLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Layout/SlateRect.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetStringLibrary.h"

// ============================================================================
// EstimateSize — returns sensible, finite bounds for a node with default pins.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutEstimateSizeTest,
    "PinWright.bpir.node_layout.EstimateSize",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutEstimateSizeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestNodeLayoutBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("Transient Blueprint has no ubergraph page")); return false; }

    // CustomEvent is self-contained: no external function lookup required, and
    // AllocateDefaultPins gives it an exec-out pin, which is enough to exercise
    // the row-measurement loop.
    UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(EventGraph);
    Node->CreateNewGuid();
    Node->CustomFunctionName = TEXT("TestEventForLayout");
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();
    EventGraph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    if (!Settings) { AddError(TEXT("Failed to instantiate UBpirLayoutSettings")); return false; }

    const FVector2D Size = BpirLayout::EstimateNodeSize(Node, *Settings);

    TestTrue(TEXT("Width is finite"), FMath::IsFinite(Size.X));
    TestTrue(TEXT("Height is finite"), FMath::IsFinite(Size.Y));
    TestTrue(TEXT("Width respects 160px minimum"), Size.X >= 160.0);

    const double MinExpectedHeight = static_cast<double>(Settings->HeaderHeightPx + Settings->PinRowHeightPx);
    TestTrue(TEXT("Height accounts for header + at least one pin row"),
        Size.Y >= MinExpectedHeight);

    return true;
}

// ============================================================================
// Graph-construction helpers — local to this test file.
// ============================================================================

namespace
{
    using CompilerTestUtils::SpawnNode;
    using CompilerTestUtils::SpawnPrintStringCall;
    using CompilerTestUtils::WireThenToExec;

    // Wires a specific named output exec pin on Src to Dst's execute input.
    // Falls back to the first input exec pin for macro nodes (e.g. ForEachLoop uses "Exec" not "execute").
    bool WireNamedOutputToExec(UEdGraphNode* Src, const FName& PinName, UEdGraphNode* Dst)
    {
        UEdGraphPin* Out = Src->FindPin(PinName, EGPD_Output);
        UEdGraphPin* In = Dst->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        if (!In)
        {
            for (UEdGraphPin* Pin : Dst->Pins)
            {
                if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    In = Pin;
                    break;
                }
            }
        }
        if (Out && In)
        {
            Out->MakeLinkTo(In);
            return true;
        }
        return false;
    }

    using BpirGraphTestHelpers::SpawnForEachLoopMacro;
} // namespace

// ============================================================================
// GetNodeBounds — bare rect + cluster-extended rect.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutBoundsTest,
    "PinWright.bpir.node_layout.Bounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutBoundsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestNodeLayoutBoundsBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("Transient Blueprint has no ubergraph page")); return false; }

    UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(EventGraph);
    Node->CreateNewGuid();
    Node->CustomFunctionName = TEXT("TestEventForBounds");
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();
    Node->NodePosX = 500;
    Node->NodePosY = 300;
    EventGraph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    if (!Settings) { AddError(TEXT("Failed to instantiate UBpirLayoutSettings")); return false; }

    // Bare rect: cluster-mode off, no registry.
    const FSlateRect Bare = BpirLayout::GetNodeBounds(Node, *Settings, /*bUseClusterBounds=*/false, nullptr);
    TestEqual(TEXT("Bare rect Left matches NodePosX"), Bare.Left, static_cast<float>(Node->NodePosX));
    TestEqual(TEXT("Bare rect Top matches NodePosY"), Bare.Top, static_cast<float>(Node->NodePosY));
    TestTrue(TEXT("Bare rect has positive width"), (Bare.Right - Bare.Left) > 0.0f);
    TestTrue(TEXT("Bare rect has positive height"), (Bare.Bottom - Bare.Top) > 0.0f);

    // Register a cluster that extends 200px to the left of the node.
    BpirLayout::FClusterBoundsRegistry Registry;
    const FSlateRect ClusterRect(
        static_cast<float>(Node->NodePosX) - 200.0f,
        static_cast<float>(Node->NodePosY),
        static_cast<float>(Node->NodePosX),
        static_cast<float>(Node->NodePosY) + 50.0f);
    Registry.Register(Node, ClusterRect);

    TestTrue(TEXT("Registry reports Has() true"), Registry.Has(Node));

    const FSlateRect Extended = BpirLayout::GetNodeBounds(Node, *Settings, /*bUseClusterBounds=*/true, &Registry);
    TestTrue(TEXT("Cluster-mode Left extends past NodePosX"),
        Extended.Left < static_cast<float>(Node->NodePosX));
    TestTrue(TEXT("Cluster-mode Right covers node's right edge"),
        Extended.Right >= (static_cast<float>(Node->NodePosX) + (Bare.Right - Bare.Left)));

    // The registered cluster rect must not be mutated by GetNodeBounds.
    const FSlateRect StoredAfter = Registry.Get(Node);
    TestEqual(TEXT("Registry cluster rect Left not mutated"), StoredAfter.Left, ClusterRect.Left);
    TestEqual(TEXT("Registry cluster rect Right not mutated"), StoredAfter.Right, ClusterRect.Right);

    return true;
}

// ============================================================================
// FormatX — linear chain: Event -> Call1 -> Call2
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatXLinearChainTest,
    "PinWright.bpir.node_layout.format_x.LinearChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatXLinearChainTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatXLinearBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("LinearEvent");
    EventNode->ReconstructNode();

    UK2Node_CallFunction* Call1 = SpawnPrintStringCall(Graph, 300, 0);
    UK2Node_CallFunction* Call2 = SpawnPrintStringCall(Graph, 600, 0);

    // Wire Event.then -> Call1.exec, Call1.then -> Call2.exec.
    if (!WireThenToExec(EventNode, Call1)) { AddError(TEXT("Wire Event->Call1 failed")); return false; }
    if (!WireThenToExec(Call1, Call2))     { AddError(TEXT("Wire Call1->Call2 failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(Call1);
    Pool.Add(Call2);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    const BpirLayout::FFormatXInfoMap Map = BpirLayout::BuildFormatXInfoMap(EventNode, Pool, *Settings);

    TestEqual(TEXT("Map size = 3"), Map.Infos.Num(), 3);

    BpirLayout::FFormatXInfoPtr RootInfo = Map.RootInfo;
    TestTrue(TEXT("Root info valid"), RootInfo.IsValid());
    if (!RootInfo.IsValid()) return false;
    TestTrue(TEXT("Root is Event"), RootInfo->Node == EventNode);
    TestTrue(TEXT("Root bIsRoot=true"), RootInfo->bIsRoot);
    TestEqual(TEXT("Root has 1 child"), RootInfo->Children.Num(), 1);
    if (RootInfo->Children.Num() != 1) return false;

    BpirLayout::FFormatXInfoPtr FirstChild = RootInfo->Children[0];
    TestTrue(TEXT("First child is Call1"), FirstChild.IsValid() && FirstChild->Node == Call1);
    TestFalse(TEXT("Call1 bIsRoot=false"), FirstChild->bIsRoot);
    TestEqual(TEXT("Call1 has 1 child"), FirstChild->Children.Num(), 1);
    if (FirstChild->Children.Num() != 1) return false;

    BpirLayout::FFormatXInfoPtr SecondChild = FirstChild->Children[0];
    TestTrue(TEXT("Second child is Call2"), SecondChild.IsValid() && SecondChild->Node == Call2);
    TestEqual(TEXT("Call2 has 0 children"), SecondChild->Children.Num(), 0);
    TestFalse(TEXT("Call2 bIsRoot=false"), SecondChild->bIsRoot);

    return true;
}

// ============================================================================
// FormatX — branch: Event -> Branch, Branch.True -> A, Branch.False -> B
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatXBranchTest,
    "PinWright.bpir.node_layout.format_x.Branch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatXBranchTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatXBranchBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("BranchEvent");
    EventNode->ReconstructNode();

    UK2Node_IfThenElse* Branch = SpawnNode<UK2Node_IfThenElse>(Graph, 300, 0);
    UK2Node_CallFunction* CallA = SpawnPrintStringCall(Graph, 600, -100);
    UK2Node_CallFunction* CallB = SpawnPrintStringCall(Graph, 600, 100);

    if (!WireThenToExec(EventNode, Branch)) { AddError(TEXT("Wire Event->Branch failed")); return false; }
    // Branch's true output is PN_Then; false output is PN_Else.
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Then, CallA))
        { AddError(TEXT("Wire Branch.True->CallA failed")); return false; }
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Else, CallB))
        { AddError(TEXT("Wire Branch.False->CallB failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(Branch);
    Pool.Add(CallA);
    Pool.Add(CallB);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    const BpirLayout::FFormatXInfoMap Map = BpirLayout::BuildFormatXInfoMap(EventNode, Pool, *Settings);

    TestEqual(TEXT("Map size = 4"), Map.Infos.Num(), 4);

    BpirLayout::FFormatXInfoPtr RootInfo = Map.RootInfo;
    if (!RootInfo.IsValid()) { AddError(TEXT("Root info null")); return false; }
    TestEqual(TEXT("Root has 1 child (Branch)"), RootInfo->Children.Num(), 1);
    if (RootInfo->Children.Num() != 1) return false;

    BpirLayout::FFormatXInfoPtr BranchInfo = RootInfo->Children[0];
    TestTrue(TEXT("First child is Branch"), BranchInfo.IsValid() && BranchInfo->Node == Branch);
    TestEqual(TEXT("Branch has 2 children"), BranchInfo->Children.Num(), 2);
    if (BranchInfo->Children.Num() != 2) return false;

    // Both CallA and CallB should appear as Branch's children (order may vary with pin
    // iteration; we don't over-specify, just that both are present).
    TSet<UEdGraphNode*> BranchChildNodes;
    for (const BpirLayout::FFormatXInfoPtr& Child : BranchInfo->Children)
    {
        if (Child.IsValid()) BranchChildNodes.Add(Child->Node);
    }
    TestTrue(TEXT("Branch children contain CallA"), BranchChildNodes.Contains(CallA));
    TestTrue(TEXT("Branch children contain CallB"), BranchChildNodes.Contains(CallB));

    return true;
}

// ============================================================================
// FormatX — diamond: Event -> Branch -> {A, B}, A -> C, B -> C. C has two
// exec-input connections; the walk must choose a single parent deterministically.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatXDiamondTest,
    "PinWright.bpir.node_layout.format_x.Diamond",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatXDiamondTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatXDiamondBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("DiamondEvent");
    EventNode->ReconstructNode();

    UK2Node_IfThenElse* Branch = SpawnNode<UK2Node_IfThenElse>(Graph, 300, 0);
    // Position A/B distinctly on the X-axis so the BetterParent tie-break rule has
    // a deterministic answer (for EGPD_Output direction, larger NodePosX wins).
    UK2Node_CallFunction* CallA = SpawnPrintStringCall(Graph, 600, -100);
    UK2Node_CallFunction* CallB = SpawnPrintStringCall(Graph, 800, 100);
    UK2Node_CallFunction* CallC = SpawnPrintStringCall(Graph, 1100, 0);

    if (!WireThenToExec(EventNode, Branch)) { AddError(TEXT("Wire Event->Branch failed")); return false; }
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Then, CallA))
        { AddError(TEXT("Wire Branch.True->A failed")); return false; }
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Else, CallB))
        { AddError(TEXT("Wire Branch.False->B failed")); return false; }
    if (!WireThenToExec(CallA, CallC)) { AddError(TEXT("Wire A->C failed")); return false; }
    if (!WireThenToExec(CallB, CallC)) { AddError(TEXT("Wire B->C failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(Branch);
    Pool.Add(CallA);
    Pool.Add(CallB);
    Pool.Add(CallC);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    const BpirLayout::FFormatXInfoMap Map = BpirLayout::BuildFormatXInfoMap(EventNode, Pool, *Settings);

    TestEqual(TEXT("Map size = 5"), Map.Infos.Num(), 5);

    BpirLayout::FFormatXInfoPtr CInfo = Map.Find(CallC);
    TestTrue(TEXT("C info exists exactly once"), CInfo.IsValid());
    if (!CInfo.IsValid()) return false;

    UEdGraphNode* CParent = CInfo->LinkFromParent.GetFromNode();
    TestTrue(TEXT("C's parent is either A or B"),
        CParent == CallA || CParent == CallB);

    // With CallB at NodePosX=800 and CallA at NodePosX=600, and EGPD_Output walk
    // direction preferring larger NodePosX, CallB should win on the tie-break.
    // Note: this is deterministic given the priority rule; if stack order admits
    // A-first and B-second, A gets picked first, then B replaces it. If B-first
    // then A-second, B is set first and A does not replace it. Either way, CallB wins.
    TestTrue(TEXT("C's parent is B (larger NodePosX wins for output-direction walk)"),
        CParent == CallB);

    // C must appear exactly once in the parent's Children list (no duplicates after
    // a re-parent swap).
    BpirLayout::FFormatXInfoPtr ParentInfo = Map.Find(CParent);
    if (ParentInfo.IsValid())
    {
        int32 CCount = 0;
        for (const BpirLayout::FFormatXInfoPtr& Child : ParentInfo->Children)
        {
            if (Child.IsValid() && Child->Node == CallC) ++CCount;
        }
        TestEqual(TEXT("C appears exactly once in its parent's Children"), CCount, 1);
    }

    // C's other potential parent must not also list C as a child.
    UEdGraphNode* Other = (CParent == CallA) ? CallB : CallA;
    BpirLayout::FFormatXInfoPtr OtherInfo = Map.Find(Other);
    if (OtherInfo.IsValid())
    {
        bool bOtherListsC = false;
        for (const BpirLayout::FFormatXInfoPtr& Child : OtherInfo->Children)
        {
            if (Child.IsValid() && Child->Node == CallC) { bOtherListsC = true; break; }
        }
        TestFalse(TEXT("Loser does not list C as a child"), bOtherListsC);
    }

    return true;
}

// ============================================================================
// FormatX — linear spacing: after FormatX, child X = parent.right + pad (grid-rounded).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatXLinearSpacingTest,
    "PinWright.bpir.node_layout.format_x.LinearSpacing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatXLinearSpacingTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatXSpacingBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    // Event at X=100 (non-zero so we can verify it's untouched).
    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 100, 0);
    EventNode->CustomFunctionName = TEXT("SpacingEvent");
    EventNode->ReconstructNode();

    // Initial positions are arbitrary — FormatX should overwrite them.
    UK2Node_CallFunction* Call1 = SpawnPrintStringCall(Graph, 9999, 0);
    UK2Node_CallFunction* Call2 = SpawnPrintStringCall(Graph, -9999, 0);

    if (!WireThenToExec(EventNode, Call1)) { AddError(TEXT("Wire Event->Call1 failed")); return false; }
    if (!WireThenToExec(Call1, Call2))     { AddError(TEXT("Wire Call1->Call2 failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(Call1);
    Pool.Add(Call2);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    const BpirLayout::FFormatXInfoMap Map = BpirLayout::BuildFormatXInfoMap(EventNode, Pool, *Settings);

    const int32 RootXBefore = EventNode->NodePosX;
    BpirLayout::FormatX(Map, *Settings, /*bUseClusterBounds=*/false, /*Registry=*/nullptr);

    TestEqual(TEXT("Root NodePosX is unchanged"), EventNode->NodePosX, RootXBefore);
    TestTrue(TEXT("Call1 placed right of Event"), Call1->NodePosX > EventNode->NodePosX);
    TestTrue(TEXT("Call2 placed right of Call1"), Call2->NodePosX > Call1->NodePosX);

    // Compute expected Call1 X: output direction, so Delta = 0 (bare==larger), then
    // NewX = EventRight + 0 + NodePadX, grid-ceil'd.
    const FVector2D EventSize = BpirLayout::EstimateNodeSize(EventNode, *Settings);
    const float EventRight = static_cast<float>(EventNode->NodePosX) + static_cast<float>(EventSize.X);
    const int32 Grid = FMath::Max(1, Settings->InternalGridPx);
    const float Raw = EventRight + static_cast<float>(Settings->NodePadX);
    const int32 ExpectedCall1 = FMath::CeilToInt(Raw / static_cast<float>(Grid)) * Grid;

    // The tolerance is a single grid step. It's meaningful because BA's formula rounds
    // via FMath::RoundToInt after the directional ceil, which could shift by <1px in
    // edge cases where font measurement differs between platforms.
    TestTrue(TEXT("Call1.NodePosX within +/- InternalGridPx of expected"),
        FMath::Abs(Call1->NodePosX - ExpectedCall1) <= Grid);

    // Sanity: Call2 spacing relative to Call1 follows the same rule.
    const FVector2D Call1Size = BpirLayout::EstimateNodeSize(Call1, *Settings);
    const float Call1Right = static_cast<float>(Call1->NodePosX) + static_cast<float>(Call1Size.X);
    const float Raw2 = Call1Right + static_cast<float>(Settings->NodePadX);
    const int32 ExpectedCall2 = FMath::CeilToInt(Raw2 / static_cast<float>(Grid)) * Grid;
    TestTrue(TEXT("Call2.NodePosX within +/- InternalGridPx of expected"),
        FMath::Abs(Call2->NodePosX - ExpectedCall2) <= Grid);

    return true;
}

// ============================================================================
// FormatX — cluster bounds: pass 2 with registered cluster rect widens spacing.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatXClusterBoundsTest,
    "PinWright.bpir.node_layout.format_x.ClusterBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatXClusterBoundsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatXClusterBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("ClusterEvent");
    EventNode->ReconstructNode();

    UK2Node_CallFunction* Call1 = SpawnPrintStringCall(Graph, 0, 0);
    UK2Node_CallFunction* Call2 = SpawnPrintStringCall(Graph, 0, 0);

    if (!WireThenToExec(EventNode, Call1)) { AddError(TEXT("Wire Event->Call1 failed")); return false; }
    if (!WireThenToExec(Call1, Call2))     { AddError(TEXT("Wire Call1->Call2 failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(Call1);
    Pool.Add(Call2);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    const BpirLayout::FFormatXInfoMap Map = BpirLayout::BuildFormatXInfoMap(EventNode, Pool, *Settings);

    // Pass 1: no cluster bounds.
    BpirLayout::FormatX(Map, *Settings, /*bUseClusterBounds=*/false, /*Registry=*/nullptr);
    const int32 X1 = Call2->NodePosX;

    // Register a cluster rect on Call1 extending 150px to the left of Call1, simulating
    // a parameter subtree. The Delta term for Call2 (EGPD_Output) is ChildBounds.Left -
    // LargerBounds.Left; widening LargerBounds' left edge increases Delta, pushing Call2 right.
    const FVector2D Call1Size = BpirLayout::EstimateNodeSize(Call1, *Settings);
    BpirLayout::FClusterBoundsRegistry Registry;
    const FSlateRect ClusterRect(
        static_cast<float>(Call1->NodePosX) - 150.0f,
        static_cast<float>(Call1->NodePosY),
        static_cast<float>(Call1->NodePosX) + static_cast<float>(Call1Size.X),
        static_cast<float>(Call1->NodePosY) + static_cast<float>(Call1Size.Y));
    Registry.Register(Call1, ClusterRect);

    // Pass 2: with cluster bounds. Re-seed Call1 to its post-pass-1 X before running pass 2
    // (FormatX only rewrites Call1 based on Event, which doesn't change, so this is a no-op
    // in practice — but being explicit avoids depending on that invariant).
    BpirLayout::FormatX(Map, *Settings, /*bUseClusterBounds=*/true, &Registry);
    const int32 X2 = Call2->NodePosX;

    TestTrue(TEXT("Call2 shifted right after cluster bounds applied"), X2 > X1);

    // The delta should be at least the cluster extension minus one grid cell of rounding slack.
    const int32 Grid = FMath::Max(1, Settings->InternalGridPx);
    const int32 MinExpectedDelta = 150 - Grid;
    TestTrue(TEXT("Call2 shift >= cluster extension minus one grid cell"),
        (X2 - X1) >= MinExpectedDelta);

    return true;
}

// ============================================================================
// FormatX — child NodePosX is divisible by InternalGridPx.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatXGridAlignmentTest,
    "PinWright.bpir.node_layout.format_x.GridAlignment",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatXGridAlignmentTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatXGridBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    // Deliberately place the event off-grid (NodePosX=7, grid default=8) to confirm the
    // alignment applies to the *child* regardless of where the parent sits.
    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 7, 0);
    EventNode->CustomFunctionName = TEXT("GridEvent");
    EventNode->ReconstructNode();

    UK2Node_CallFunction* Call = SpawnPrintStringCall(Graph, 0, 0);
    if (!WireThenToExec(EventNode, Call)) { AddError(TEXT("Wire Event->Call failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(Call);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    const BpirLayout::FFormatXInfoMap Map = BpirLayout::BuildFormatXInfoMap(EventNode, Pool, *Settings);

    BpirLayout::FormatX(Map, *Settings, /*bUseClusterBounds=*/false, /*Registry=*/nullptr);

    const int32 Grid = FMath::Max(1, Settings->InternalGridPx);
    TestEqual(TEXT("Child NodePosX is divisible by InternalGridPx"),
        Call->NodePosX % Grid, 0);

    return true;
}

// ============================================================================
// FNodeLayoutParameterFormatter — two pure Conv_BoolToInt feeding Add_IntInt.
// Verifies the formatter places pures to the left, stacks them vertically, and
// produces a cluster rect that covers consumer + pures.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutParameterFormatterBasicTest,
    "PinWright.bpir.node_layout.parameter_formatter.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutParameterFormatterBasicTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestParamFormatterBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    // Consumer: UKismetMathLibrary::Add_IntInt.
    // Conv_BoolToInt is a pure node producing an int result in UE 5.6.
    UK2Node_CallFunction* Add = SpawnNode<UK2Node_CallFunction>(Graph, 800, 400);
    Add->FunctionReference.SetExternalMember(
        TEXT("Add_IntInt"), UKismetMathLibrary::StaticClass());
    Add->ReconstructNode();

    UK2Node_CallFunction* Conv1 = SpawnNode<UK2Node_CallFunction>(Graph, 0, 0);
    Conv1->FunctionReference.SetExternalMember(
        TEXT("Conv_BoolToInt"), UKismetMathLibrary::StaticClass());
    Conv1->ReconstructNode();

    UK2Node_CallFunction* Conv2 = SpawnNode<UK2Node_CallFunction>(Graph, 50, 50);
    Conv2->FunctionReference.SetExternalMember(
        TEXT("Conv_BoolToInt"), UKismetMathLibrary::StaticClass());
    Conv2->ReconstructNode();

    // Wire Conv1.ReturnValue -> Add.A and Conv2.ReturnValue -> Add.B
    UEdGraphPin* Conv1Ret = Conv1->FindPin(TEXT("ReturnValue"), EGPD_Output);
    UEdGraphPin* Conv2Ret = Conv2->FindPin(TEXT("ReturnValue"), EGPD_Output);
    UEdGraphPin* AddA = Add->FindPin(TEXT("A"), EGPD_Input);
    UEdGraphPin* AddB = Add->FindPin(TEXT("B"), EGPD_Input);
    if (!Conv1Ret || !Conv2Ret || !AddA || !AddB)
    {
        AddError(TEXT("Failed to locate expected pins on Add/Conv nodes"));
        return false;
    }
    Conv1Ret->MakeLinkTo(AddA);
    Conv2Ret->MakeLinkTo(AddB);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    if (!Settings) { AddError(TEXT("Failed to instantiate UBpirLayoutSettings")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(Add);
    Pool.Add(Conv1);
    Pool.Add(Conv2);

    TSet<UEdGraphNode*> Ignored;

    const int32 AddXBefore = Add->NodePosX;
    const int32 AddYBefore = Add->NodePosY;

    BpirLayout::FNodeLayoutParameterFormatter Formatter(Add, *Settings);
    Formatter.Format(Pool, Ignored);

    // Consumer itself must not have moved.
    TestEqual(TEXT("Consumer NodePosX unchanged"), Add->NodePosX, AddXBefore);
    TestEqual(TEXT("Consumer NodePosY unchanged"), Add->NodePosY, AddYBefore);

    const TArray<UEdGraphNode*>& Placed = Formatter.GetPlacedPureNodes();
    TestEqual(TEXT("Formatter placed 2 pure nodes"), Placed.Num(), 2);

    // Both Conv nodes ended up to the left of Add, with right edges clear of the
    // consumer by at least PinPadX (minus at most one grid cell from floor snap).
    const int32 Grid = FMath::Max(1, Settings->InternalGridPx);
    for (UEdGraphNode* Pure : Placed)
    {
        TestTrue(TEXT("Pure left of consumer"), Pure->NodePosX < Add->NodePosX);
        const FVector2D Size = BpirLayout::EstimateNodeSize(Pure, *Settings);
        const int32 PureRight = Pure->NodePosX + FMath::CeilToInt(Size.X);
        const int32 Boundary = Add->NodePosX - Settings->PinPadX;
        TestTrue(TEXT("Pure right edge at or before ColumnRightEdge (within a grid cell)"),
            PureRight <= Boundary + Grid);
    }

    // Stacking: the two Convs do not vertically overlap.
    if (Placed.Num() == 2)
    {
        UEdGraphNode* A = Placed[0];
        UEdGraphNode* B = Placed[1];
        const FVector2D ASize = BpirLayout::EstimateNodeSize(A, *Settings);
        const FVector2D BSize = BpirLayout::EstimateNodeSize(B, *Settings);
        const int32 ATop = A->NodePosY;
        const int32 ABot = A->NodePosY + FMath::CeilToInt(ASize.Y);
        const int32 BTop = B->NodePosY;
        const int32 BBot = B->NodePosY + FMath::CeilToInt(BSize.Y);
        const bool bDisjoint = (ABot <= BTop) || (BBot <= ATop);
        TestTrue(TEXT("Stacked Convs do not vertically overlap"), bDisjoint);
    }

    // Cluster bounds must cover the consumer horizontally and extend to its left.
    const FSlateRect Cluster = Formatter.GetClusterBounds();
    const FVector2D AddSize = BpirLayout::EstimateNodeSize(Add, *Settings);
    TestTrue(TEXT("Cluster extends left of consumer"),
        Cluster.Left < static_cast<float>(Add->NodePosX));
    TestTrue(TEXT("Cluster covers consumer's right edge"),
        Cluster.Right >= static_cast<float>(Add->NodePosX) + static_cast<float>(AddSize.X) - 0.5f);

    return true;
}

// Consumer-only graph: formatter must not crash and must produce cluster bounds
// equal to the consumer's bare bounds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutParameterFormatterConsumerOnlyTest,
    "PinWright.bpir.node_layout.parameter_formatter.ConsumerOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutParameterFormatterConsumerOnlyTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestParamFormatterEmptyBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    UK2Node_CallFunction* Add = SpawnNode<UK2Node_CallFunction>(Graph, 500, 250);
    Add->FunctionReference.SetExternalMember(
        TEXT("Add_IntInt"), UKismetMathLibrary::StaticClass());
    Add->ReconstructNode();

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();

    TSet<UEdGraphNode*> Pool;
    Pool.Add(Add);
    TSet<UEdGraphNode*> Ignored;

    BpirLayout::FNodeLayoutParameterFormatter Formatter(Add, *Settings);
    Formatter.Format(Pool, Ignored);

    TestEqual(TEXT("No pures placed when consumer has no wired inputs"),
        Formatter.GetPlacedPureNodes().Num(), 0);

    const FSlateRect Bare = BpirLayout::GetNodeBounds(Add, *Settings, false, nullptr);
    const FSlateRect Cluster = Formatter.GetClusterBounds();
    TestEqual(TEXT("Cluster Left equals bare Left"), Cluster.Left, Bare.Left);
    TestEqual(TEXT("Cluster Top equals bare Top"), Cluster.Top, Bare.Top);
    TestEqual(TEXT("Cluster Right equals bare Right"), Cluster.Right, Bare.Right);
    TestEqual(TEXT("Cluster Bottom equals bare Bottom"), Cluster.Bottom, Bare.Bottom);

    return true;
}

// ============================================================================
// FormatParameterNodes — integration of the parameter formatter with an
// FFormatXInfoMap. Covers three scenarios:
//   1. Empty/no-candidate graph — function is a no-op, does not crash.
//   2. Single impure consumer with two pure inputs — exactly one cluster
//      registered, two pures placed, formatter retained.
//   3. Two impure consumers sharing a pure input — the BFS-first consumer wins
//      the shared pure; the sibling's formatter still runs but sees one fewer
//      claim candidate.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatParameterNodesEmptyTest,
    "PinWright.bpir.node_layout.format_parameter_nodes.Empty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatParameterNodesEmptyTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatParamNodesEmptyBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    // Lone CustomEvent with no incoming pure data — should produce no clusters.
    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("LoneEvent");
    EventNode->ReconstructNode();

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    const BpirLayout::FFormatXInfoMap Map =
        BpirLayout::BuildFormatXInfoMap(EventNode, Pool, *Settings);

    BpirLayout::FClusterBoundsRegistry Registry;
    TSet<UEdGraphNode*> Placed;
    TArray<TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>> Formatters;

    BpirLayout::FormatParameterNodes(Map, Pool, *Settings, Registry, Placed, Formatters);

    TestEqual(TEXT("No formatters retained for pure-free graph"), Formatters.Num(), 0);
    TestEqual(TEXT("No pures placed"), Placed.Num(), 0);
    TestFalse(TEXT("Registry has no entry for event"), Registry.Has(EventNode));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatParameterNodesSingleConsumerTest,
    "PinWright.bpir.node_layout.format_parameter_nodes.SingleConsumer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatParameterNodesSingleConsumerTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatParamNodesSingleBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    // Event -> PrintString, with PrintString.InString fed by Conv_DoubleToString
    // (plus a second pure Conv_DoubleToString left dangling to confirm only
    // connected pures are claimed).
    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("SingleConsumerEvent");
    EventNode->ReconstructNode();

    UK2Node_CallFunction* PrintCall = SpawnPrintStringCall(Graph, 400, 0);

    UK2Node_CallFunction* Conv1 = SpawnNode<UK2Node_CallFunction>(Graph, 0, 0);
    Conv1->FunctionReference.SetExternalMember(
        TEXT("Conv_DoubleToString"), UKismetStringLibrary::StaticClass());
    Conv1->ReconstructNode();

    UK2Node_CallFunction* Conv2 = SpawnNode<UK2Node_CallFunction>(Graph, 0, 0);
    Conv2->FunctionReference.SetExternalMember(
        TEXT("Conv_DoubleToString"), UKismetStringLibrary::StaticClass());
    Conv2->ReconstructNode();

    if (!WireThenToExec(EventNode, PrintCall))
    {
        AddError(TEXT("Wire Event->PrintCall failed"));
        return false;
    }

    UEdGraphPin* ConvRet1 = Conv1->FindPin(TEXT("ReturnValue"), EGPD_Output);
    UEdGraphPin* InStringPin = PrintCall->FindPin(TEXT("InString"), EGPD_Input);
    if (!ConvRet1 || !InStringPin)
    {
        AddError(TEXT("Failed to find Conv1.ReturnValue / PrintCall.InString pins"));
        return false;
    }
    ConvRet1->MakeLinkTo(InStringPin);
    // Conv2 left dangling on purpose.

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(PrintCall);
    Pool.Add(Conv1);
    Pool.Add(Conv2);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    const BpirLayout::FFormatXInfoMap Map =
        BpirLayout::BuildFormatXInfoMap(EventNode, Pool, *Settings);

    BpirLayout::FClusterBoundsRegistry Registry;
    TSet<UEdGraphNode*> Placed;
    TArray<TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>> Formatters;

    BpirLayout::FormatParameterNodes(Map, Pool, *Settings, Registry, Placed, Formatters);

    TestEqual(TEXT("One formatter retained for PrintCall consumer"), Formatters.Num(), 1);
    TestTrue(TEXT("Registry has entry for PrintCall"), Registry.Has(PrintCall));
    TestFalse(TEXT("Registry has no entry for Event (no pures wired in)"), Registry.Has(EventNode));
    TestEqual(TEXT("Exactly one pure claimed (wired Conv1)"), Placed.Num(), 1);
    TestTrue(TEXT("Placed set contains Conv1"), Placed.Contains(Conv1));
    TestFalse(TEXT("Placed set excludes dangling Conv2"), Placed.Contains(Conv2));

    // Cluster rect must extend left of the consumer.
    const FSlateRect Cluster = Registry.Get(PrintCall);
    TestTrue(TEXT("Cluster extends left of PrintCall"),
        Cluster.Left < static_cast<float>(PrintCall->NodePosX));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatParameterNodesSharedPureTest,
    "PinWright.bpir.node_layout.format_parameter_nodes.SharedPure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatParameterNodesSharedPureTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatParamNodesSharedBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    // Event -> Print1 -> Print2, where a single pure Conv_DoubleToString feeds
    // both PrintString consumers' InString pin. The BFS-first consumer (Print1,
    // closer to the root) should claim the shared pure; Print2's formatter
    // sees it in IgnoredNodes and finds nothing to claim.
    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("SharedPureEvent");
    EventNode->ReconstructNode();

    UK2Node_CallFunction* Print1 = SpawnPrintStringCall(Graph, 400, 0);
    UK2Node_CallFunction* Print2 = SpawnPrintStringCall(Graph, 800, 0);

    UK2Node_CallFunction* SharedConv = SpawnNode<UK2Node_CallFunction>(Graph, 0, 0);
    SharedConv->FunctionReference.SetExternalMember(
        TEXT("Conv_DoubleToString"), UKismetStringLibrary::StaticClass());
    SharedConv->ReconstructNode();

    if (!WireThenToExec(EventNode, Print1)) { AddError(TEXT("Wire Event->Print1 failed")); return false; }
    if (!WireThenToExec(Print1, Print2))    { AddError(TEXT("Wire Print1->Print2 failed")); return false; }

    UEdGraphPin* SharedRet = SharedConv->FindPin(TEXT("ReturnValue"), EGPD_Output);
    UEdGraphPin* InStr1 = Print1->FindPin(TEXT("InString"), EGPD_Input);
    UEdGraphPin* InStr2 = Print2->FindPin(TEXT("InString"), EGPD_Input);
    if (!SharedRet || !InStr1 || !InStr2)
    {
        AddError(TEXT("Failed to find shared conv / print pins"));
        return false;
    }
    SharedRet->MakeLinkTo(InStr1);
    SharedRet->MakeLinkTo(InStr2);

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(Print1);
    Pool.Add(Print2);
    Pool.Add(SharedConv);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    const BpirLayout::FFormatXInfoMap Map =
        BpirLayout::BuildFormatXInfoMap(EventNode, Pool, *Settings);

    BpirLayout::FClusterBoundsRegistry Registry;
    TSet<UEdGraphNode*> Placed;
    TArray<TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>> Formatters;

    BpirLayout::FormatParameterNodes(Map, Pool, *Settings, Registry, Placed, Formatters);

    // Only the first-claiming consumer (Print1) should appear in the registry
    // and in the retained formatters list. Print2's formatter finds nothing
    // because SharedConv is already in IgnoredNodes by the time it runs.
    TestTrue(TEXT("Registry has entry for Print1 (first-claim winner)"),
        Registry.Has(Print1));
    TestFalse(TEXT("Registry has no entry for Print2 (shared pure already claimed)"),
        Registry.Has(Print2));
    TestEqual(TEXT("Exactly one formatter retained"), Formatters.Num(), 1);
    // The shared pure is distinct count 1, not the wiring count 2.
    TestEqual(TEXT("Placed pure count == distinct pure count, not wiring count"),
        Placed.Num(), 1);
    TestTrue(TEXT("Placed set contains SharedConv"), Placed.Contains(SharedConv));

    return true;
}

// ============================================================================
// GetPinsOfSameHeight — linear chain: every child is the sole exec child of its
// parent, so every non-root should end up marked same-row.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutSameRowLinearTest,
    "PinWright.bpir.node_layout.same_row.Linear",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutSameRowLinearTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestSameRowLinearBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("SameRowLinearEvent");
    EventNode->ReconstructNode();

    UK2Node_CallFunction* Call1 = SpawnPrintStringCall(Graph, 300, 0);
    UK2Node_CallFunction* Call2 = SpawnPrintStringCall(Graph, 600, 0);

    if (!WireThenToExec(EventNode, Call1)) { AddError(TEXT("Wire Event->Call1 failed")); return false; }
    if (!WireThenToExec(Call1, Call2))     { AddError(TEXT("Wire Call1->Call2 failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(Call1);
    Pool.Add(Call2);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    BpirLayout::FFormatXInfoMap Map = BpirLayout::BuildFormatXInfoMap(EventNode, Pool, *Settings);

    BpirLayout::GetPinsOfSameHeight(Map);

    BpirLayout::FFormatXInfoPtr Call1Info = Map.Find(Call1);
    BpirLayout::FFormatXInfoPtr Call2Info = Map.Find(Call2);
    TestTrue(TEXT("Call1 info exists"), Call1Info.IsValid());
    TestTrue(TEXT("Call2 info exists"), Call2Info.IsValid());
    if (!Call1Info.IsValid() || !Call2Info.IsValid()) return false;

    TestTrue(TEXT("Call1 marked same-row (sole exec child of Event)"),
        Call1Info->bSameRowAsParent);
    TestTrue(TEXT("Call2 marked same-row (sole exec child of Call1)"),
        Call2Info->bSameRowAsParent);

    return true;
}

// ============================================================================
// GetPinsOfSameHeight — branch: exactly one of the two exec children of Branch
// gets same-row. Which one depends on pin iteration order on UK2Node_IfThenElse
// (typically PN_Then first, so CallA wins), but the test only asserts "exactly
// one" to avoid over-specifying an engine detail.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutSameRowBranchTest,
    "PinWright.bpir.node_layout.same_row.Branch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutSameRowBranchTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestSameRowBranchBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("SameRowBranchEvent");
    EventNode->ReconstructNode();

    UK2Node_IfThenElse* Branch = SpawnNode<UK2Node_IfThenElse>(Graph, 300, 0);
    UK2Node_CallFunction* CallA = SpawnPrintStringCall(Graph, 600, -100);
    UK2Node_CallFunction* CallB = SpawnPrintStringCall(Graph, 600, 100);

    if (!WireThenToExec(EventNode, Branch)) { AddError(TEXT("Wire Event->Branch failed")); return false; }
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Then, CallA))
        { AddError(TEXT("Wire Branch.True->CallA failed")); return false; }
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Else, CallB))
        { AddError(TEXT("Wire Branch.False->CallB failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(Branch);
    Pool.Add(CallA);
    Pool.Add(CallB);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    BpirLayout::FFormatXInfoMap Map = BpirLayout::BuildFormatXInfoMap(EventNode, Pool, *Settings);

    BpirLayout::GetPinsOfSameHeight(Map);

    BpirLayout::FFormatXInfoPtr BranchInfo = Map.Find(Branch);
    BpirLayout::FFormatXInfoPtr AInfo = Map.Find(CallA);
    BpirLayout::FFormatXInfoPtr BInfo = Map.Find(CallB);
    TestTrue(TEXT("Branch info exists"), BranchInfo.IsValid());
    TestTrue(TEXT("CallA info exists"), AInfo.IsValid());
    TestTrue(TEXT("CallB info exists"), BInfo.IsValid());
    if (!BranchInfo.IsValid() || !AInfo.IsValid() || !BInfo.IsValid()) return false;

    // Branch itself must be same-row as Event (sole exec child of Event).
    TestTrue(TEXT("Branch marked same-row (sole exec child of Event)"),
        BranchInfo->bSameRowAsParent);

    // Exactly one of CallA / CallB gets same-row — don't assume which, since pin
    // iteration order on UK2Node_IfThenElse is an engine detail.
    const int32 BranchSameRowChildCount =
        (AInfo->bSameRowAsParent ? 1 : 0) + (BInfo->bSameRowAsParent ? 1 : 0);
    TestEqual(TEXT("Exactly one of A/B is same-row as Branch"),
        BranchSameRowChildCount, 1);

    return true;
}

// ============================================================================
// GetPinsOfSameHeight — empty map: no crash, empty mapping.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutSameRowEmptyMapTest,
    "PinWright.bpir.node_layout.same_row.EmptyMap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutSameRowEmptyMapTest::RunTest(const FString& Parameters)
{
    BpirLayout::FFormatXInfoMap EmptyMap;
    // No RootInfo, no Infos — simulates the pre-BuildFormatXInfoMap state or a
    // degenerate call with a null anchor node.

    // Should not crash on an empty map.
    BpirLayout::GetPinsOfSameHeight(EmptyMap);

    TestEqual(TEXT("Empty map has no infos"), EmptyMap.Infos.Num(), 0);

    return true;
}

// ============================================================================
// FormatY helpers — run the full X-then-Y pipeline so tests share one code path.
// ============================================================================

namespace
{
    // Runs the standard FormatX (pass 1 + parameter formatting + pass 2) and then
    // GetPinsOfSameHeight, returning the populated FFormatXInfoMap + registry so
    // callers can invoke FormatY on top.
    void RunXThroughSameRow(
        UEdGraphNode* Anchor,
        const TSet<UEdGraphNode*>& Pool,
        const UBpirLayoutSettings& Settings,
        BpirLayout::FFormatXInfoMap& OutMap,
        BpirLayout::FClusterBoundsRegistry& OutRegistry,
        TArray<TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>>& OutFormatters,
        TMap<UEdGraphNode*, BpirLayout::FNodeLayoutParameterFormatter*>& OutFormatterByConsumer)
    {
        OutMap = BpirLayout::BuildFormatXInfoMap(Anchor, Pool, Settings);

        // Pass 1: X without cluster bounds (registry empty).
        BpirLayout::FormatX(OutMap, Settings, /*bUseClusterBounds=*/false, nullptr);

        TSet<UEdGraphNode*> Placed;
        BpirLayout::FormatParameterNodes(OutMap, Pool, Settings, OutRegistry, Placed, OutFormatters);

        // Snapshot consumer X before pass 2 so pures can be translated with
        // their consumer (mirrors FNodeLayoutEngine::Format step 5b).
        TMap<UEdGraphNode*, int32> ConsumerXBeforePass2;
        for (const TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>& F : OutFormatters)
        {
            if (F && F->GetConsumer())
            {
                ConsumerXBeforePass2.Add(F->GetConsumer(), F->GetConsumer()->NodePosX);
            }
        }

        // Pass 2: X with cluster bounds populated.
        BpirLayout::FormatX(OutMap, Settings, /*bUseClusterBounds=*/true, &OutRegistry);

        // Shift each pure cluster's X by its consumer's pass-2 delta so pures
        // stay attached to their consumer's left side after pass 2. Also
        // re-register the shifted cluster rect so the registry is not stale.
        for (const TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>& F : OutFormatters)
        {
            if (!F || !F->GetConsumer()) continue;
            UEdGraphNode* Consumer = F->GetConsumer();
            const int32* OldX = ConsumerXBeforePass2.Find(Consumer);
            if (!OldX) continue;
            const int32 DeltaX = Consumer->NodePosX - *OldX;
            if (DeltaX == 0) continue;
            for (UEdGraphNode* Pure : F->GetPlacedPureNodes())
            {
                if (Pure)
                {
                    Pure->NodePosX += DeltaX;
                }
            }
            if (OutRegistry.Has(Consumer))
            {
                const FSlateRect OldRect = OutRegistry.Get(Consumer);
                const FSlateRect ShiftedRect(
                    OldRect.Left + static_cast<float>(DeltaX),
                    OldRect.Top,
                    OldRect.Right + static_cast<float>(DeltaX),
                    OldRect.Bottom);
                OutRegistry.Register(Consumer, ShiftedRect);
            }
        }

        BpirLayout::GetPinsOfSameHeight(OutMap);

        for (const TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>& F : OutFormatters)
        {
            if (F && F->GetConsumer())
            {
                OutFormatterByConsumer.Add(F->GetConsumer(), F.Get());
            }
        }

        // Mirror step-7.5-post from FNodeLayoutEngine::Format so tests see the same
        // Registry==current-positions invariant as the full engine pipeline.
        BpirLayout::SyncRegistryFromFormatters(OutFormatterByConsumer, OutRegistry, Settings);
    }
}

// ============================================================================
// FormatY — linear same-row chain: Y stays constant along Event->Call1->Call2.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatYLinearSameRowTest,
    "PinWright.bpir.node_layout.format_y.LinearSameRow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatYLinearSameRowTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatYLinearBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("LinearYEvent");
    EventNode->ReconstructNode();

    UK2Node_CallFunction* Call1 = SpawnPrintStringCall(Graph, 400, 0);
    UK2Node_CallFunction* Call2 = SpawnPrintStringCall(Graph, 800, 0);

    if (!WireThenToExec(EventNode, Call1)) { AddError(TEXT("Wire Event->Call1 failed")); return false; }
    if (!WireThenToExec(Call1, Call2))     { AddError(TEXT("Wire Call1->Call2 failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(Call1);
    Pool.Add(Call2);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    BpirLayout::FFormatXInfoMap Map;
    BpirLayout::FClusterBoundsRegistry Registry;
    TArray<TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>> Formatters;
    TMap<UEdGraphNode*, BpirLayout::FNodeLayoutParameterFormatter*> FormatterByConsumer;
    RunXThroughSameRow(EventNode, Pool, *Settings, Map, Registry, Formatters, FormatterByConsumer);

    const TSet<UEdGraphNode*> NoExternals;
    BpirLayout::FormatY(Map, NoExternals, *Settings, FormatterByConsumer);

    TestEqual(TEXT("Call1 Y == Event Y (same-row)"),
        Call1->NodePosY, EventNode->NodePosY);
    TestEqual(TEXT("Call2 Y == Event Y (same-row chain)"),
        Call2->NodePosY, EventNode->NodePosY);

    return true;
}

// ============================================================================
// FormatY — linear-with-pures: Event -> Call1 -> Call2 -> Call3, each Call has a
// pure MakeLiteralString feeding InString. All three Call nodes must remain at
// the anchor's Y after FormatY.
// ============================================================================

namespace
{
    // Spawn a UK2Node_CallFunction targeting UKismetSystemLibrary::MakeLiteralString.
    // This is a pure node whose ReturnValue output is a string — ideal for wiring
    // into PrintString's InString input to simulate "Call with one pure input".
    UK2Node_CallFunction* SpawnMakeLiteralStringCall(UEdGraph* Graph, int32 PosX, int32 PosY)
    {
        UK2Node_CallFunction* Node = SpawnNode<UK2Node_CallFunction>(Graph, PosX, PosY);
        Node->FunctionReference.SetExternalMember(
            GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralString),
            UKismetSystemLibrary::StaticClass());
        Node->ReconstructNode();
        return Node;
    }

    // Connect a named output data pin on Src to a named input data pin on Dst via
    // the K2 schema so the link is validated like a real editor connection.
    bool WireDataPin(UEdGraphNode* Src, const FName& SrcPinName, UEdGraphNode* Dst, const FName& DstPinName)
    {
        UEdGraphPin* OutPin = Src->FindPin(SrcPinName, EGPD_Output);
        UEdGraphPin* InPin = Dst->FindPin(DstPinName, EGPD_Input);
        if (!OutPin || !InPin) { return false; }
        return GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(OutPin, InPin);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatYLinearWithPuresTest,
    "PinWright.bpir.node_layout.format_y.LinearWithPures",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatYLinearWithPuresTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatYLinearPuresBP"));
    if (!BP) { AddError(TEXT("BP create failed")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("no ubergraph")); return false; }

    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("LinearPuresEvent");
    EventNode->ReconstructNode();

    UK2Node_CallFunction* Call1 = SpawnPrintStringCall(Graph, 400, 0);
    UK2Node_CallFunction* Call2 = SpawnPrintStringCall(Graph, 800, 0);
    UK2Node_CallFunction* Call3 = SpawnPrintStringCall(Graph, 1200, 0);

    // Pure inputs — one MakeLiteralString per Call wired to InString. Positions
    // are (0,0) to force FormatParameterNodes to place them, exercising the pass
    // that later interacts with FormatY's collision-avoidance loop.
    UK2Node_CallFunction* Str1 = SpawnMakeLiteralStringCall(Graph, 0, 0);
    UK2Node_CallFunction* Str2 = SpawnMakeLiteralStringCall(Graph, 0, 0);
    UK2Node_CallFunction* Str3 = SpawnMakeLiteralStringCall(Graph, 0, 0);

    if (!WireThenToExec(EventNode, Call1)) { AddError(TEXT("wire 1 failed")); return false; }
    if (!WireThenToExec(Call1, Call2))     { AddError(TEXT("wire 2 failed")); return false; }
    if (!WireThenToExec(Call2, Call3))     { AddError(TEXT("wire 3 failed")); return false; }

    // Wire each pure's ReturnValue output to the Call's InString input.
    static const FName ReturnValuePin(TEXT("ReturnValue"));
    static const FName InStringPin(TEXT("InString"));
    if (!WireDataPin(Str1, ReturnValuePin, Call1, InStringPin)) { AddError(TEXT("wire Str1->Call1 failed")); return false; }
    if (!WireDataPin(Str2, ReturnValuePin, Call2, InStringPin)) { AddError(TEXT("wire Str2->Call2 failed")); return false; }
    if (!WireDataPin(Str3, ReturnValuePin, Call3, InStringPin)) { AddError(TEXT("wire Str3->Call3 failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode); Pool.Add(Call1); Pool.Add(Call2); Pool.Add(Call3);
    Pool.Add(Str1); Pool.Add(Str2); Pool.Add(Str3);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    BpirLayout::FFormatXInfoMap Map;
    BpirLayout::FClusterBoundsRegistry Registry;
    TArray<TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>> Formatters;
    TMap<UEdGraphNode*, BpirLayout::FNodeLayoutParameterFormatter*> FormatterByConsumer;
    RunXThroughSameRow(EventNode, Pool, *Settings, Map, Registry, Formatters, FormatterByConsumer);

    const TSet<UEdGraphNode*> NoExternals;
    BpirLayout::FormatY(Map, NoExternals, *Settings, FormatterByConsumer);

    TestEqual(TEXT("Call1 Y == Event Y"), Call1->NodePosY, EventNode->NodePosY);
    TestEqual(TEXT("Call2 Y == Event Y"), Call2->NodePosY, EventNode->NodePosY);
    TestEqual(TEXT("Call3 Y == Event Y"), Call3->NodePosY, EventNode->NodePosY);
    return true;
}

// ============================================================================
// FormatY — ForEachLoop macro: LoopBody is the same-row primary child, while
// the Completed subtree must land strictly below the primary subtree's cluster
// bottom.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatYForEachLoopTest,
    "PinWright.bpir.node_layout.format_y.ForEachLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatYForEachLoopTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatYForEachBP"));
    if (!BP) { AddError(TEXT("BP create failed")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("no ubergraph")); return false; }

    UK2Node_CustomEvent* Event = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    Event->CustomFunctionName = TEXT("Evt"); Event->ReconstructNode();

    UK2Node_MacroInstance* ForEach = SpawnForEachLoopMacro(Graph, 300, 0);
    if (!ForEach) { AddError(TEXT("ForEach spawn failed")); return false; }

    UK2Node_CallFunction* Body1 = SpawnPrintStringCall(Graph, 700, 0);
    UK2Node_CallFunction* Body2 = SpawnPrintStringCall(Graph, 1100, 0);
    UK2Node_CallFunction* AfterLoop = SpawnPrintStringCall(Graph, 700, 0);

    if (!WireThenToExec(Event, ForEach)) { AddError(TEXT("wire Event->ForEach failed")); return false; }
    if (!WireNamedOutputToExec(ForEach, TEXT("LoopBody"), Body1)) { AddError(TEXT("wire LoopBody failed")); return false; }
    if (!WireThenToExec(Body1, Body2)) { AddError(TEXT("wire Body1->Body2 failed")); return false; }
    if (!WireNamedOutputToExec(ForEach, TEXT("Completed"), AfterLoop)) { AddError(TEXT("wire Completed failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(Event); Pool.Add(ForEach); Pool.Add(Body1); Pool.Add(Body2); Pool.Add(AfterLoop);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    BpirLayout::FFormatXInfoMap Map;
    BpirLayout::FClusterBoundsRegistry Registry;
    TArray<TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>> Formatters;
    TMap<UEdGraphNode*, BpirLayout::FNodeLayoutParameterFormatter*> FormatterByConsumer;
    RunXThroughSameRow(Event, Pool, *Settings, Map, Registry, Formatters, FormatterByConsumer);

    const TSet<UEdGraphNode*> NoExternals;
    BpirLayout::FormatY(Map, NoExternals, *Settings, FormatterByConsumer);

    TestEqual(TEXT("ForEach Y == Event Y"), ForEach->NodePosY, Event->NodePosY);
    TestEqual(TEXT("Body1 Y == Event Y (LoopBody primary)"), Body1->NodePosY, Event->NodePosY);
    TestEqual(TEXT("Body2 Y == Event Y"), Body2->NodePosY, Event->NodePosY);

    const FSlateRect Body1Cluster = BpirLayout::GetNodeBounds(Body1, *Settings, true, &Registry);
    const FSlateRect Body2Cluster = BpirLayout::GetNodeBounds(Body2, *Settings, true, &Registry);
    const float PrimarySubtreeBottom = FMath::Max(Body1Cluster.Bottom, Body2Cluster.Bottom);
    TestTrue(TEXT("Completed subtree (AfterLoop) Y strictly below primary subtree"),
        static_cast<float>(AfterLoop->NodePosY) > PrimarySubtreeBottom);
    return true;
}

// ============================================================================
// FormatY — branch: one child shares parent's row, the other stacks below with
// at least NodePadY separation.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatYBranchStackingTest,
    "PinWright.bpir.node_layout.format_y.BranchStacking",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatYBranchStackingTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatYBranchBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("BranchYEvent");
    EventNode->ReconstructNode();

    UK2Node_IfThenElse* Branch = SpawnNode<UK2Node_IfThenElse>(Graph, 300, 0);
    UK2Node_CallFunction* CallA = SpawnPrintStringCall(Graph, 600, 0);
    UK2Node_CallFunction* CallB = SpawnPrintStringCall(Graph, 600, 0);

    if (!WireThenToExec(EventNode, Branch)) { AddError(TEXT("Wire Event->Branch failed")); return false; }
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Then, CallA))
        { AddError(TEXT("Wire Branch.True->CallA failed")); return false; }
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Else, CallB))
        { AddError(TEXT("Wire Branch.False->CallB failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(Branch);
    Pool.Add(CallA);
    Pool.Add(CallB);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    BpirLayout::FFormatXInfoMap Map;
    BpirLayout::FClusterBoundsRegistry Registry;
    TArray<TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>> Formatters;
    TMap<UEdGraphNode*, BpirLayout::FNodeLayoutParameterFormatter*> FormatterByConsumer;
    RunXThroughSameRow(EventNode, Pool, *Settings, Map, Registry, Formatters, FormatterByConsumer);

    const TSet<UEdGraphNode*> NoExternals;
    BpirLayout::FormatY(Map, NoExternals, *Settings, FormatterByConsumer);

    // Branch inherits Event's Y (sole exec child of Event -> same row).
    TestEqual(TEXT("Branch Y == Event Y"), Branch->NodePosY, EventNode->NodePosY);

    // Exactly one of CallA/CallB shares Branch's Y; the other is strictly below.
    const bool bAOnRow = (CallA->NodePosY == Branch->NodePosY);
    const bool bBOnRow = (CallB->NodePosY == Branch->NodePosY);
    TestTrue(TEXT("Exactly one of CallA/CallB shares Branch row"),
        (bAOnRow && !bBOnRow) || (!bAOnRow && bBOnRow));

    UK2Node_CallFunction* Stacked = bAOnRow ? CallB : CallA;
    const int32 MinGap = Settings->NodePadY; // NodePadY between stacked bounds.
    TestTrue(TEXT("Stacked child is placed strictly below Branch row by at least NodePadY"),
        Stacked->NodePosY >= Branch->NodePosY + MinGap);

    return true;
}

// ============================================================================
// FormatY — collision nudge: before FormatY, seed CallA and CallB at identical
// Y; after FormatY the two bounding boxes must not overlap.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatYCollisionNudgeTest,
    "PinWright.bpir.node_layout.format_y.CollisionNudge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatYCollisionNudgeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatYCollisionBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("CollisionYEvent");
    EventNode->ReconstructNode();

    UK2Node_IfThenElse* Branch = SpawnNode<UK2Node_IfThenElse>(Graph, 300, 0);
    UK2Node_CallFunction* CallA = SpawnPrintStringCall(Graph, 600, 0);
    UK2Node_CallFunction* CallB = SpawnPrintStringCall(Graph, 600, 0);

    if (!WireThenToExec(EventNode, Branch)) { AddError(TEXT("Wire Event->Branch failed")); return false; }
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Then, CallA))
        { AddError(TEXT("Wire Branch.True->CallA failed")); return false; }
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Else, CallB))
        { AddError(TEXT("Wire Branch.False->CallB failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(Branch);
    Pool.Add(CallA);
    Pool.Add(CallB);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    BpirLayout::FFormatXInfoMap Map;
    BpirLayout::FClusterBoundsRegistry Registry;
    TArray<TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>> Formatters;
    TMap<UEdGraphNode*, BpirLayout::FNodeLayoutParameterFormatter*> FormatterByConsumer;
    RunXThroughSameRow(EventNode, Pool, *Settings, Map, Registry, Formatters, FormatterByConsumer);

    // Seed pathological state: force CallA and CallB to identical Y so tentative
    // placement produces guaranteed overlap before the collision loop runs.
    CallA->NodePosY = 0;
    CallB->NodePosY = 0;

    const TSet<UEdGraphNode*> NoExternals;
    BpirLayout::FormatY(Map, NoExternals, *Settings, FormatterByConsumer);

    const FSlateRect RectA = BpirLayout::GetNodeBounds(
        CallA, *Settings, /*bUseClusterBounds=*/true, &Registry);
    const FSlateRect RectB = BpirLayout::GetNodeBounds(
        CallB, *Settings, /*bUseClusterBounds=*/true, &Registry);

    TestFalse(TEXT("CallA and CallB bounds must not overlap after FormatY"),
        FSlateRect::DoRectanglesIntersect(RectA, RectB));

    return true;
}

// ============================================================================
// FormatY — external obstacle: a node outside the pool acts as a hard blocker.
// The traversed chain must route around it.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatYExternalObstacleTest,
    "PinWright.bpir.node_layout.format_y.ExternalObstacle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatYExternalObstacleTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatYExternalBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    // Chain A: EventA -> CallA. This chain is in the pool and gets laid out.
    UK2Node_CustomEvent* EventA = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventA->CustomFunctionName = TEXT("EventA");
    EventA->ReconstructNode();
    UK2Node_CallFunction* CallA = SpawnPrintStringCall(Graph, 400, 0);
    if (!WireThenToExec(EventA, CallA)) { AddError(TEXT("Wire EventA->CallA failed")); return false; }

    // Chain B: EventB -> CallB. Treated as frozen obstacles. Position the
    // obstacle directly in CallA's tentative Y slot (the event-row) so only
    // collision-vs-external can move CallA off that row.
    UK2Node_CustomEvent* EventB = SpawnNode<UK2Node_CustomEvent>(Graph, 400, 0);
    EventB->CustomFunctionName = TEXT("EventB");
    EventB->ReconstructNode();
    UK2Node_CallFunction* CallB = SpawnPrintStringCall(Graph, 400, 0);
    if (!WireThenToExec(EventB, CallB)) { AddError(TEXT("Wire EventB->CallB failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventA);
    Pool.Add(CallA);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    BpirLayout::FFormatXInfoMap Map;
    BpirLayout::FClusterBoundsRegistry Registry;
    TArray<TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>> Formatters;
    TMap<UEdGraphNode*, BpirLayout::FNodeLayoutParameterFormatter*> FormatterByConsumer;
    RunXThroughSameRow(EventA, Pool, *Settings, Map, Registry, Formatters, FormatterByConsumer);

    // Pin CallB directly on top of CallA's planned position (same X after X-pass
    // and same Y as EventA) to force a collision.
    CallB->NodePosX = CallA->NodePosX;
    CallB->NodePosY = EventA->NodePosY;

    TSet<UEdGraphNode*> ExternalObstacles;
    ExternalObstacles.Add(EventB);
    ExternalObstacles.Add(CallB);

    BpirLayout::FormatY(Map, ExternalObstacles, *Settings, FormatterByConsumer);

    const FSlateRect RectCallA = BpirLayout::GetNodeBounds(
        CallA, *Settings, /*bUseClusterBounds=*/true, &Registry);
    const FSlateRect RectCallB = BpirLayout::GetNodeBounds(
        CallB, *Settings, /*bUseClusterBounds=*/true, &Registry);

    TestFalse(TEXT("CallA must not overlap external obstacle CallB"),
        FSlateRect::DoRectanglesIntersect(RectCallA, RectCallB));

    return true;
}

// ============================================================================
// ResetRelativeToAnchor — translating the pool restores the anchor's saved
// position and shifts every other node by the same offset.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutAnchorResetTest,
    "PinWright.bpir.node_layout.AnchorReset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutAnchorResetTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestAnchorResetBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 200, 300);
    EventNode->CustomFunctionName = TEXT("AnchorResetEvent");
    EventNode->ReconstructNode();

    UK2Node_CallFunction* Call1 = SpawnPrintStringCall(Graph, 600, 300);
    if (!WireThenToExec(EventNode, Call1)) { AddError(TEXT("Wire Event->Call1 failed")); return false; }

    // Capture the authoritative pre-layout anchor position BEFORE running any pass.
    const FIntPoint Saved(EventNode->NodePosX, EventNode->NodePosY);

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(Call1);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    BpirLayout::FFormatXInfoMap Map;
    BpirLayout::FClusterBoundsRegistry Registry;
    TArray<TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>> Formatters;
    TMap<UEdGraphNode*, BpirLayout::FNodeLayoutParameterFormatter*> FormatterByConsumer;
    RunXThroughSameRow(EventNode, Pool, *Settings, Map, Registry, Formatters, FormatterByConsumer);

    const TSet<UEdGraphNode*> NoExternals;
    BpirLayout::FormatY(Map, NoExternals, *Settings, FormatterByConsumer);

    // Simulate that some pass shifted the anchor: perturb it by (+50, +50) and
    // shift children by the same delta so we can independently verify the reset
    // translation lands Call1 back at its correct relative offset.
    EventNode->NodePosX += 50;
    EventNode->NodePosY += 50;
    Call1->NodePosX    += 50;
    Call1->NodePosY    += 50;

    // Capture Call1's pre-reset position so we can verify the translation offset
    // was applied to it as well.
    const int32 Call1PreX = Call1->NodePosX;
    const int32 Call1PreY = Call1->NodePosY;

    const FIntPoint ExpectedOffset(
        Saved.X - EventNode->NodePosX,
        Saved.Y - EventNode->NodePosY);

    BpirLayout::ResetRelativeToAnchor(Pool, EventNode, Saved);

    TestEqual(TEXT("Event X restored to saved"), EventNode->NodePosX, Saved.X);
    TestEqual(TEXT("Event Y restored to saved"), EventNode->NodePosY, Saved.Y);

    TestEqual(TEXT("Call1 X translated by same offset"),
        Call1->NodePosX, Call1PreX + ExpectedOffset.X);
    TestEqual(TEXT("Call1 Y translated by same offset"),
        Call1->NodePosY, Call1PreY + ExpectedOffset.Y);

    return true;
}

// ============================================================================
// SnapToGrid — every node's X/Y becomes a multiple of Settings.InternalGridPx
// (default 8), including the anchor.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutGridSnapTest,
    "PinWright.bpir.node_layout.GridSnap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutGridSnapTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestGridSnapBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    // Use explicit non-grid-aligned positions so the Round test has signal.
    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 5, 11);
    EventNode->CustomFunctionName = TEXT("GridSnapEvent");
    EventNode->ReconstructNode();

    UK2Node_CallFunction* Call1 = SpawnPrintStringCall(Graph, 317, 103);
    UK2Node_CallFunction* Call2 = SpawnPrintStringCall(Graph, 643, 259);

    if (!WireThenToExec(EventNode, Call1)) { AddError(TEXT("Wire Event->Call1 failed")); return false; }
    if (!WireThenToExec(Call1, Call2))     { AddError(TEXT("Wire Call1->Call2 failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(Call1);
    Pool.Add(Call2);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();

    // Build the FormatXInfoMap so SnapToGrid can look up per-node directions. We
    // DON'T run FormatX here because that would snap coordinates already; we
    // want non-grid inputs to verify SnapToGrid actually snaps them.
    BpirLayout::FFormatXInfoMap Map =
        BpirLayout::BuildFormatXInfoMap(EventNode, Pool, *Settings);

    // Force the positions back to the non-grid values since BuildFormatXInfoMap
    // only populates Infos/RootInfo and does not move nodes. (Defensive: in case
    // any earlier pass did move them.)
    EventNode->NodePosX = 5;   EventNode->NodePosY = 11;
    Call1->NodePosX    = 317;  Call1->NodePosY    = 103;
    Call2->NodePosX    = 643;  Call2->NodePosY    = 259;

    BpirLayout::SnapToGrid(Pool, EventNode, Map, *Settings);

    const int32 Grid = Settings->InternalGridPx;
    TestTrue(TEXT("Grid is 8 by default"), Grid == 8);

    TestEqual(TEXT("Event X divisible by grid"), EventNode->NodePosX % Grid, 0);
    TestEqual(TEXT("Event Y divisible by grid"), EventNode->NodePosY % Grid, 0);
    TestEqual(TEXT("Call1 X divisible by grid"), Call1->NodePosX % Grid, 0);
    TestEqual(TEXT("Call1 Y divisible by grid"), Call1->NodePosY % Grid, 0);
    TestEqual(TEXT("Call2 X divisible by grid"), Call2->NodePosX % Grid, 0);
    TestEqual(TEXT("Call2 Y divisible by grid"), Call2->NodePosY % Grid, 0);

    // Anchor must be snapped (5 is not on the grid; nearest round is 8 or 0).
    TestTrue(TEXT("Anchor X actually snapped (not left at 5)"),
        EventNode->NodePosX != 5);

    return true;
}

// ============================================================================
// SnapToGrid — directional: a node reached via EGPD_Output whose X is just past
// a grid line should Ceil (round up), not Round to the nearest.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutDirectionalSnapTest,
    "PinWright.bpir.node_layout.DirectionalSnap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutDirectionalSnapTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestDirectionalSnapBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("DirSnapEvent");
    EventNode->ReconstructNode();

    UK2Node_CallFunction* Call1 = SpawnPrintStringCall(Graph, 321, 0);
    if (!WireThenToExec(EventNode, Call1)) { AddError(TEXT("Wire Event->Call1 failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(EventNode);
    Pool.Add(Call1);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    BpirLayout::FFormatXInfoMap Map =
        BpirLayout::BuildFormatXInfoMap(EventNode, Pool, *Settings);

    // Reassert non-grid X after BuildFormatXInfoMap (defensive).
    Call1->NodePosX = 321;
    Call1->NodePosY = 0;

    BpirLayout::SnapToGrid(Pool, EventNode, Map, *Settings);

    // Call1's LinkFromParent goes Event.then(out) -> Call1.exec(in); FromPin is
    // the output pin, so Direction == EGPD_Output => Ceil(321/8)*8 == 328.
    TestEqual(TEXT("Call1 X uses Ceil rounding under EGPD_Output"),
        Call1->NodePosX, 328);

    return true;
}

// ============================================================================
// FNodeLayoutEngine — end-to-end: runs the full pipeline on a representative
// graph (event -> branch -> two prints, each print fed by a pure conversion).
// Asserts no bare bounds overlap, anchor preserved, all coords grid-aligned.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutEngineEndToEndTest,
    "PinWright.bpir.node_layout.engine.EndToEnd",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutEngineEndToEndTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestLayoutEngineE2EBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    // Build the graph at arbitrary non-grid positions; Format() is expected to
    // lay it out from scratch. Anchor starts at (100, 200).
    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 100, 200);
    EventNode->CustomFunctionName = TEXT("E2EEvent");
    EventNode->ReconstructNode();
    // ReconstructNode may reset position; reassert anchor.
    EventNode->NodePosX = 100;
    EventNode->NodePosY = 200;

    UK2Node_IfThenElse* Branch = SpawnNode<UK2Node_IfThenElse>(Graph, 0, 0);
    UK2Node_CallFunction* PrintA = SpawnPrintStringCall(Graph, 0, 0);
    UK2Node_CallFunction* PrintB = SpawnPrintStringCall(Graph, 0, 0);

    UK2Node_CallFunction* ConvA = SpawnNode<UK2Node_CallFunction>(Graph, 0, 0);
    ConvA->FunctionReference.SetExternalMember(
        TEXT("Conv_DoubleToString"), UKismetStringLibrary::StaticClass());
    ConvA->ReconstructNode();

    UK2Node_CallFunction* ConvB = SpawnNode<UK2Node_CallFunction>(Graph, 0, 0);
    ConvB->FunctionReference.SetExternalMember(
        TEXT("Conv_DoubleToString"), UKismetStringLibrary::StaticClass());
    ConvB->ReconstructNode();

    if (!WireThenToExec(EventNode, Branch)) { AddError(TEXT("Wire Event->Branch failed")); return false; }
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Then, PrintA))
        { AddError(TEXT("Wire Branch.True->PrintA failed")); return false; }
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Else, PrintB))
        { AddError(TEXT("Wire Branch.False->PrintB failed")); return false; }

    // Wire each Conv.ReturnValue -> Print.InString.
    UEdGraphPin* ConvARet = ConvA->FindPin(TEXT("ReturnValue"), EGPD_Output);
    UEdGraphPin* ConvBRet = ConvB->FindPin(TEXT("ReturnValue"), EGPD_Output);
    UEdGraphPin* PrintAIn = PrintA->FindPin(TEXT("InString"), EGPD_Input);
    UEdGraphPin* PrintBIn = PrintB->FindPin(TEXT("InString"), EGPD_Input);
    if (!ConvARet || !ConvBRet || !PrintAIn || !PrintBIn)
    {
        AddError(TEXT("Failed to find Conv/Print pins for wiring"));
        return false;
    }
    ConvARet->MakeLinkTo(PrintAIn);
    ConvBRet->MakeLinkTo(PrintBIn);

    TArray<UEdGraphNode*> Pool = { EventNode, Branch, PrintA, PrintB, ConvA, ConvB };
    const int32 PoolCount = Pool.Num();

    BpirLayout::FNodeLayoutEngine Engine(Graph, Pool, EventNode);
    Engine.Format();

    // 1. Anchor is restored relative to the saved location, then snapped to grid.
    TestEqual(TEXT("Anchor X snapped from saved location"), EventNode->NodePosX, 104);
    TestEqual(TEXT("Anchor Y preserved"), EventNode->NodePosY, 200);

    // 2. Every node grid-aligned (default InternalGridPx == 8).
    const UBpirLayoutSettings* Settings = GetDefault<UBpirLayoutSettings>();
    const int32 Grid = (Settings && Settings->InternalGridPx > 0) ? Settings->InternalGridPx : 8;
    for (UEdGraphNode* N : Pool)
    {
        if (!N) continue;
        TestTrue(*FString::Printf(TEXT("Node %s NodePosX divisible by %d (got %d)"),
                *N->GetName(), Grid, N->NodePosX),
            (N->NodePosX % Grid) == 0);
        TestTrue(*FString::Printf(TEXT("Node %s NodePosY divisible by %d (got %d)"),
                *N->GetName(), Grid, N->NodePosY),
            (N->NodePosY % Grid) == 0);
    }

    // 3. Pairwise non-overlap on bare rects — the pipeline's fundamental
    //    guarantee is that placed nodes don't visually collide.
    for (int32 i = 0; i < PoolCount; ++i)
    {
        for (int32 j = i + 1; j < PoolCount; ++j)
        {
            UEdGraphNode* A = Pool[i];
            UEdGraphNode* B = Pool[j];
            if (!A || !B) continue;
            const FSlateRect RectA = BpirLayout::GetNodeBounds(A, *Settings, /*bUseClusterBounds=*/false, nullptr);
            const FSlateRect RectB = BpirLayout::GetNodeBounds(B, *Settings, /*bUseClusterBounds=*/false, nullptr);
            if (FSlateRect::DoRectanglesIntersect(RectA, RectB))
            {
                AddError(FString::Printf(
                    TEXT("Nodes %s and %s overlap: A=(%.1f,%.1f,%.1f,%.1f) B=(%.1f,%.1f,%.1f,%.1f)"),
                    *A->GetName(), *B->GetName(),
                    RectA.Left, RectA.Top, RectA.Right, RectA.Bottom,
                    RectB.Left, RectB.Top, RectB.Right, RectB.Bottom));
            }
        }
    }

    return true;
}

// ============================================================================
// FNodeLayoutEngine — kill switch: when bEnableBpirLayoutPass is false,
// Format() must be a no-op (positions untouched).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutEngineKillSwitchTest,
    "PinWright.bpir.node_layout.engine.KillSwitch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutEngineKillSwitchTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestLayoutEngineKillSwitchBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    // Known non-grid positions that should survive the pipeline untouched.
    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 13, 17);
    EventNode->CustomFunctionName = TEXT("KillSwitchEvent");
    EventNode->ReconstructNode();
    EventNode->NodePosX = 13;
    EventNode->NodePosY = 17;

    UK2Node_CallFunction* Call1 = SpawnPrintStringCall(Graph, 91, 73);
    Call1->NodePosX = 91;
    Call1->NodePosY = 73;

    if (!WireThenToExec(EventNode, Call1)) { AddError(TEXT("Wire Event->Call1 failed")); return false; }

    TArray<UEdGraphNode*> Pool = { EventNode, Call1 };

    // Mutate the CDO's kill switch — this persists across tests, so we MUST
    // revert before returning to avoid poisoning subsequent test runs that
    // assume the default (true).
    UBpirLayoutSettings* MutableSettings = GetMutableDefault<UBpirLayoutSettings>();
    if (!MutableSettings) { AddError(TEXT("No mutable default UBpirLayoutSettings")); return false; }
    const bool bOriginalEnabled = MutableSettings->bEnableBpirLayoutPass;
    MutableSettings->bEnableBpirLayoutPass = false;

    BpirLayout::FNodeLayoutEngine Engine(Graph, Pool, EventNode);
    Engine.Format();

    const int32 EventX = EventNode->NodePosX;
    const int32 EventY = EventNode->NodePosY;
    const int32 Call1X = Call1->NodePosX;
    const int32 Call1Y = Call1->NodePosY;

    // Restore BEFORE asserting so a failure doesn't leak state into other tests.
    MutableSettings->bEnableBpirLayoutPass = bOriginalEnabled;

    TestEqual(TEXT("Event X untouched by disabled pipeline"), EventX, 13);
    TestEqual(TEXT("Event Y untouched by disabled pipeline"), EventY, 17);
    TestEqual(TEXT("Call1 X untouched by disabled pipeline"), Call1X, 91);
    TestEqual(TEXT("Call1 Y untouched by disabled pipeline"), Call1Y, 73);

    return true;
}

// ============================================================================
// FormatY — cluster pures come along: in a linear same-row chain where two
// consecutive consumers each have pure feeders, FormatY must move pures along
// with their consumer so cluster bounds stay coherent. Before the fix, stale
// cluster bounds caused cascading displacement of thousands of pixels.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatYClusterPuresComeAlongTest,
    "PinWright.bpir.node_layout.format_y.ClusterPuresComeAlong",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatYClusterPuresComeAlongTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatYClusterPuresBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    // Linear chain: Event -> Print1 -> Print2 -> Print3
    // Print1 and Print2 each have a pure feeder (Conv_DoubleToString -> InString).
    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("ClusterPuresEvent");
    EventNode->ReconstructNode();

    UK2Node_CallFunction* Print1 = SpawnPrintStringCall(Graph, 0, 0);
    UK2Node_CallFunction* Print2 = SpawnPrintStringCall(Graph, 0, 0);
    UK2Node_CallFunction* Print3 = SpawnPrintStringCall(Graph, 0, 0);

    UK2Node_CallFunction* Conv1 = SpawnNode<UK2Node_CallFunction>(Graph, 0, 0);
    Conv1->FunctionReference.SetExternalMember(
        TEXT("Conv_DoubleToString"), UKismetStringLibrary::StaticClass());
    Conv1->ReconstructNode();

    UK2Node_CallFunction* Conv2 = SpawnNode<UK2Node_CallFunction>(Graph, 0, 0);
    Conv2->FunctionReference.SetExternalMember(
        TEXT("Conv_DoubleToString"), UKismetStringLibrary::StaticClass());
    Conv2->ReconstructNode();

    if (!WireThenToExec(EventNode, Print1)) { AddError(TEXT("Wire Event->Print1 failed")); return false; }
    if (!WireThenToExec(Print1, Print2))    { AddError(TEXT("Wire Print1->Print2 failed")); return false; }
    if (!WireThenToExec(Print2, Print3))    { AddError(TEXT("Wire Print2->Print3 failed")); return false; }

    // Wire Conv1.ReturnValue -> Print1.InString
    UEdGraphPin* Conv1Ret = Conv1->FindPin(TEXT("ReturnValue"), EGPD_Output);
    UEdGraphPin* Print1In = Print1->FindPin(TEXT("InString"), EGPD_Input);
    if (!Conv1Ret || !Print1In) { AddError(TEXT("Failed to find Conv1/Print1 pins")); return false; }
    Conv1Ret->MakeLinkTo(Print1In);

    // Wire Conv2.ReturnValue -> Print2.InString
    UEdGraphPin* Conv2Ret = Conv2->FindPin(TEXT("ReturnValue"), EGPD_Output);
    UEdGraphPin* Print2In = Print2->FindPin(TEXT("InString"), EGPD_Input);
    if (!Conv2Ret || !Print2In) { AddError(TEXT("Failed to find Conv2/Print2 pins")); return false; }
    Conv2Ret->MakeLinkTo(Print2In);

    TArray<UEdGraphNode*> Pool = { EventNode, Print1, Print2, Print3, Conv1, Conv2 };

    BpirLayout::FNodeLayoutEngine Engine(Graph, Pool, EventNode);
    Engine.Format();

    // All 4 exec nodes should share the same Y (linear same-row chain).
    const int32 AnchorY = EventNode->NodePosY;
    TestEqual(TEXT("Print1 Y == Event Y (same-row chain)"), Print1->NodePosY, AnchorY);
    TestEqual(TEXT("Print2 Y == Event Y (same-row chain)"), Print2->NodePosY, AnchorY);
    TestEqual(TEXT("Print3 Y == Event Y (same-row chain)"), Print3->NodePosY, AnchorY);

    // Each pure feeder should be near its consumer, not thousands of px away.
    // "Near" = within 500px, which is generous (a typical cluster is <300px tall).
    const int32 MaxPureDistance = 500;
    TestTrue(TEXT("Conv1 Y near Print1 Y"),
        FMath::Abs(Conv1->NodePosY - Print1->NodePosY) < MaxPureDistance);
    TestTrue(TEXT("Conv2 Y near Print2 Y"),
        FMath::Abs(Conv2->NodePosY - Print2->NodePosY) < MaxPureDistance);

    // No pairwise bare-rect overlap across all 6 nodes.
    const UBpirLayoutSettings* Settings = GetDefault<UBpirLayoutSettings>();
    for (int32 i = 0; i < Pool.Num(); ++i)
    {
        for (int32 j = i + 1; j < Pool.Num(); ++j)
        {
            if (!Pool[i] || !Pool[j]) continue;
            const FSlateRect A = BpirLayout::GetNodeBounds(Pool[i], *Settings, false, nullptr);
            const FSlateRect B = BpirLayout::GetNodeBounds(Pool[j], *Settings, false, nullptr);
            if (FSlateRect::DoRectanglesIntersect(A, B))
            {
                AddError(FString::Printf(
                    TEXT("Nodes %s and %s overlap"), *Pool[i]->GetName(), *Pool[j]->GetName()));
            }
        }
    }

    return true;
}

// ============================================================================
// FormatY — branch with pures: Event -> Branch, each arm has a consumer with a
// pure feeder. The stacked arm must not be displaced thousands of pixels below.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatYClusterBranchWithPuresTest,
    "PinWright.bpir.node_layout.format_y.ClusterBranchWithPures",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatYClusterBranchWithPuresTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatYBranchPuresBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("No ubergraph page")); return false; }

    UK2Node_CustomEvent* EventNode = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    EventNode->CustomFunctionName = TEXT("BranchPuresEvent");
    EventNode->ReconstructNode();

    UK2Node_IfThenElse* Branch = SpawnNode<UK2Node_IfThenElse>(Graph, 0, 0);
    UK2Node_CallFunction* PrintA = SpawnPrintStringCall(Graph, 0, 0);
    UK2Node_CallFunction* PrintB = SpawnPrintStringCall(Graph, 0, 0);

    UK2Node_CallFunction* ConvA = SpawnNode<UK2Node_CallFunction>(Graph, 0, 0);
    ConvA->FunctionReference.SetExternalMember(
        TEXT("Conv_DoubleToString"), UKismetStringLibrary::StaticClass());
    ConvA->ReconstructNode();

    UK2Node_CallFunction* ConvB = SpawnNode<UK2Node_CallFunction>(Graph, 0, 0);
    ConvB->FunctionReference.SetExternalMember(
        TEXT("Conv_DoubleToString"), UKismetStringLibrary::StaticClass());
    ConvB->ReconstructNode();

    if (!WireThenToExec(EventNode, Branch)) { AddError(TEXT("Wire Event->Branch failed")); return false; }
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Then, PrintA))
        { AddError(TEXT("Wire Branch.True->PrintA failed")); return false; }
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Else, PrintB))
        { AddError(TEXT("Wire Branch.False->PrintB failed")); return false; }

    // Wire ConvA.ReturnValue -> PrintA.InString
    UEdGraphPin* ConvARet = ConvA->FindPin(TEXT("ReturnValue"), EGPD_Output);
    UEdGraphPin* PrintAIn = PrintA->FindPin(TEXT("InString"), EGPD_Input);
    if (!ConvARet || !PrintAIn) { AddError(TEXT("Failed to find ConvA/PrintA pins")); return false; }
    ConvARet->MakeLinkTo(PrintAIn);

    // Wire ConvB.ReturnValue -> PrintB.InString
    UEdGraphPin* ConvBRet = ConvB->FindPin(TEXT("ReturnValue"), EGPD_Output);
    UEdGraphPin* PrintBIn = PrintB->FindPin(TEXT("InString"), EGPD_Input);
    if (!ConvBRet || !PrintBIn) { AddError(TEXT("Failed to find ConvB/PrintB pins")); return false; }
    ConvBRet->MakeLinkTo(PrintBIn);

    TArray<UEdGraphNode*> Pool = { EventNode, Branch, PrintA, PrintB, ConvA, ConvB };

    BpirLayout::FNodeLayoutEngine Engine(Graph, Pool, EventNode);
    Engine.Format();

    const int32 AnchorY = EventNode->NodePosY;

    // Branch should be same-row as Event.
    TestEqual(TEXT("Branch Y == Event Y"), Branch->NodePosY, AnchorY);

    // One of PrintA/PrintB shares Branch Y, the other is stacked below.
    const bool bAOnRow = (PrintA->NodePosY == Branch->NodePosY);
    const bool bBOnRow = (PrintB->NodePosY == Branch->NodePosY);
    TestTrue(TEXT("Exactly one Print on Branch row"),
        (bAOnRow && !bBOnRow) || (!bAOnRow && bBOnRow));

    // The stacked print should be below but NOT thousands of px away.
    // 1000px is very generous — a typical stacking offset is ~200-400px.
    UK2Node_CallFunction* StackedPrint = bAOnRow ? PrintB : PrintA;
    const int32 MaxStackDistance = 1000;
    TestTrue(TEXT("Stacked print below anchor"),
        StackedPrint->NodePosY > AnchorY);
    TestTrue(TEXT("Stacked print not displaced thousands of px"),
        (StackedPrint->NodePosY - AnchorY) < MaxStackDistance);

    // Each pure feeder should be near its consumer.
    const int32 MaxPureDistance = 500;
    TestTrue(TEXT("ConvA Y near PrintA Y"),
        FMath::Abs(ConvA->NodePosY - PrintA->NodePosY) < MaxPureDistance);
    TestTrue(TEXT("ConvB Y near PrintB Y"),
        FMath::Abs(ConvB->NodePosY - PrintB->NodePosY) < MaxPureDistance);

    // No pairwise bare-rect overlap.
    const UBpirLayoutSettings* Settings = GetDefault<UBpirLayoutSettings>();
    for (int32 i = 0; i < Pool.Num(); ++i)
    {
        for (int32 j = i + 1; j < Pool.Num(); ++j)
        {
            if (!Pool[i] || !Pool[j]) continue;
            const FSlateRect A = BpirLayout::GetNodeBounds(Pool[i], *Settings, false, nullptr);
            const FSlateRect B = BpirLayout::GetNodeBounds(Pool[j], *Settings, false, nullptr);
            if (FSlateRect::DoRectanglesIntersect(A, B))
            {
                AddError(FString::Printf(
                    TEXT("Nodes %s and %s overlap"), *Pool[i]->GetName(), *Pool[j]->GetName()));
            }
        }
    }

    return true;
}

// ============================================================================
// FormatY - ExecutionSequence with 3 outputs (then_0/then_1/then_2).
// then_0's child is the primary same-row child; then_1 and then_2's children
// stack strictly below. All three children share X (fan-out shares X, differs
// only in Y).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutFormatYSequenceTest,
    "PinWright.bpir.node_layout.format_y.Sequence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutFormatYSequenceTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestFormatYSequenceBP"));
    if (!BP) { AddError(TEXT("BP create failed")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("no ubergraph")); return false; }

    UK2Node_CustomEvent* Event = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    Event->CustomFunctionName = TEXT("Evt"); Event->ReconstructNode();

    // SpawnNode() already calls AllocateDefaultPins(), which creates then_0 and
    // then_1. AddInputPin() appends a third output pin (then_2).
    UK2Node_ExecutionSequence* Seq = SpawnNode<UK2Node_ExecutionSequence>(Graph, 300, 0);
    Seq->AddInputPin();

    UK2Node_CallFunction* Call0 = SpawnPrintStringCall(Graph, 700, 0);
    UK2Node_CallFunction* Call1 = SpawnPrintStringCall(Graph, 700, 0);
    UK2Node_CallFunction* Call2 = SpawnPrintStringCall(Graph, 700, 0);

    if (!WireThenToExec(Event, Seq)) { AddError(TEXT("wire Event->Seq failed")); return false; }
    // Pin names are lowercase with underscore (then_0 / then_1 / then_2) per
    // UK2Node_ExecutionSequence::GetPinNameGivenIndex, which formats as
    // "<PN_Then>_<Index>" where PN_Then == "then".
    if (!WireNamedOutputToExec(Seq, TEXT("then_0"), Call0)) { AddError(TEXT("wire then_0 failed")); return false; }
    if (!WireNamedOutputToExec(Seq, TEXT("then_1"), Call1)) { AddError(TEXT("wire then_1 failed")); return false; }
    if (!WireNamedOutputToExec(Seq, TEXT("then_2"), Call2)) { AddError(TEXT("wire then_2 failed")); return false; }

    TSet<UEdGraphNode*> Pool;
    Pool.Add(Event); Pool.Add(Seq); Pool.Add(Call0); Pool.Add(Call1); Pool.Add(Call2);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    BpirLayout::FFormatXInfoMap Map;
    BpirLayout::FClusterBoundsRegistry Registry;
    TArray<TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>> Formatters;
    TMap<UEdGraphNode*, BpirLayout::FNodeLayoutParameterFormatter*> FormatterByConsumer;
    RunXThroughSameRow(Event, Pool, *Settings, Map, Registry, Formatters, FormatterByConsumer);

    const TSet<UEdGraphNode*> NoExternals;
    BpirLayout::FormatY(Map, NoExternals, *Settings, FormatterByConsumer);

    TestEqual(TEXT("Call0 shares Seq row (then_0 primary)"), Call0->NodePosY, Seq->NodePosY);
    TestTrue(TEXT("Call1 Y strictly below Call0"), Call1->NodePosY > Call0->NodePosY);
    TestTrue(TEXT("Call2 Y strictly below Call1"), Call2->NodePosY > Call1->NodePosY);
    TestEqual(TEXT("Call0.X == Call1.X (fan-out shares X)"), Call0->NodePosX, Call1->NodePosX);
    TestEqual(TEXT("Call1.X == Call2.X"), Call1->NodePosX, Call2->NodePosX);
    return true;
}

// ============================================================================
// Stress regression — 15-node mixed graph combining Branch + ForEachLoop +
// linear chain + pure MakeLiteralString clusters. After FormatY runs, the
// cluster-expanded group rect for every node must be pairwise non-intersecting.
// Exercises collision resolution across nested macro, fan-out, and pure-input
// scenarios simultaneously.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeLayoutGroupRectNoIntersectTest,
    "PinWright.bpir.node_layout.GroupRectNoIntersect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNodeLayoutGroupRectNoIntersectTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestGroupRectBP"));
    if (!BP) { AddError(TEXT("BP create failed")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("no ubergraph")); return false; }

    UK2Node_CustomEvent* Event = SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    Event->CustomFunctionName = TEXT("Evt"); Event->ReconstructNode();

    UK2Node_IfThenElse* Branch = SpawnNode<UK2Node_IfThenElse>(Graph, 300, 0);
    UK2Node_MacroInstance* ForEach = SpawnForEachLoopMacro(Graph, 600, 0);
    UK2Node_CallFunction* Body1 = SpawnPrintStringCall(Graph, 900, 0);
    UK2Node_CallFunction* Body2 = SpawnPrintStringCall(Graph, 1200, 0);
    UK2Node_CallFunction* CallA = SpawnPrintStringCall(Graph, 600, 0);
    UK2Node_CallFunction* Converge = SpawnPrintStringCall(Graph, 1500, 0);
    UK2Node_CallFunction* Str1 = SpawnMakeLiteralStringCall(Graph, 0, 0);
    UK2Node_CallFunction* Str2 = SpawnMakeLiteralStringCall(Graph, 0, 0);
    UK2Node_CallFunction* StrA = SpawnMakeLiteralStringCall(Graph, 0, 0);
    UK2Node_CallFunction* StrConv = SpawnMakeLiteralStringCall(Graph, 0, 0);
    UK2Node_CallFunction* Filler1 = SpawnPrintStringCall(Graph, 1800, 0);
    UK2Node_CallFunction* Filler2 = SpawnPrintStringCall(Graph, 2100, 0);
    UK2Node_CallFunction* FillerStr1 = SpawnMakeLiteralStringCall(Graph, 0, 0);
    UK2Node_CallFunction* FillerStr2 = SpawnMakeLiteralStringCall(Graph, 0, 0);

    if (!WireThenToExec(Event, Branch)) { AddError(TEXT("wire Event->Branch failed")); return false; }
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Then, ForEach)) { AddError(TEXT("wire Then failed")); return false; }
    if (!WireNamedOutputToExec(Branch, UEdGraphSchema_K2::PN_Else, CallA)) { AddError(TEXT("wire Else failed")); return false; }
    if (!WireNamedOutputToExec(ForEach, TEXT("LoopBody"), Body1)) { AddError(TEXT("wire LoopBody failed")); return false; }
    if (!WireThenToExec(Body1, Body2)) { AddError(TEXT("wire Body1->Body2 failed")); return false; }
    if (!WireNamedOutputToExec(ForEach, TEXT("Completed"), Converge)) { AddError(TEXT("wire Completed failed")); return false; }
    if (!WireThenToExec(Converge, Filler1)) { AddError(TEXT("wire Converge->Filler1 failed")); return false; }
    if (!WireThenToExec(Filler1, Filler2)) { AddError(TEXT("wire Filler1->Filler2 failed")); return false; }

    // Data wires — one pure MakeLiteralString ReturnValue -> InString per Call.
    static const FName ReturnValuePin(TEXT("ReturnValue"));
    static const FName InStringPin(TEXT("InString"));
    WireDataPin(Str1, ReturnValuePin, Body1, InStringPin);
    WireDataPin(Str2, ReturnValuePin, Body2, InStringPin);
    WireDataPin(StrA, ReturnValuePin, CallA, InStringPin);
    WireDataPin(StrConv, ReturnValuePin, Converge, InStringPin);
    WireDataPin(FillerStr1, ReturnValuePin, Filler1, InStringPin);
    WireDataPin(FillerStr2, ReturnValuePin, Filler2, InStringPin);

    TSet<UEdGraphNode*> Pool;
    for (UEdGraphNode* N : TArray<UEdGraphNode*>{ Event, Branch, ForEach, Body1, Body2, CallA, Converge,
            Str1, Str2, StrA, StrConv, Filler1, Filler2, FillerStr1, FillerStr2 })
    { Pool.Add(N); }

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    BpirLayout::FFormatXInfoMap Map;
    BpirLayout::FClusterBoundsRegistry Registry;
    TArray<TUniquePtr<BpirLayout::FNodeLayoutParameterFormatter>> Formatters;
    TMap<UEdGraphNode*, BpirLayout::FNodeLayoutParameterFormatter*> FormatterByConsumer;
    RunXThroughSameRow(Event, Pool, *Settings, Map, Registry, Formatters, FormatterByConsumer);

    const TSet<UEdGraphNode*> NoExternals;
    BpirLayout::FormatY(Map, NoExternals, *Settings, FormatterByConsumer, &Registry);

    // Build group-rect list from cluster-expanded bounds. A pure node that was placed
    // by some consumer's parameter formatter is, by construction, contained inside that
    // consumer's cluster rect (cluster = union of consumer + its pures). Iterating every
    // pool node would therefore compare a pure's bare rect against its own cluster rect
    // and trivially "intersect". To assert disjointness across distinct groups we skip
    // any node already represented by its consumer's cluster — the consumer's cluster
    // rect already covers it.
    TSet<UEdGraphNode*> ClusterMembers;
    for (const TPair<UEdGraphNode*, BpirLayout::FNodeLayoutParameterFormatter*>& Pair : FormatterByConsumer)
    {
        if (Pair.Value)
        {
            for (UEdGraphNode* Pure : Pair.Value->GetPlacedPureNodes())
            {
                ClusterMembers.Add(Pure);
            }
        }
    }

    TArray<FSlateRect> GroupRects;
    for (UEdGraphNode* N : Pool)
    {
        if (ClusterMembers.Contains(N))
        {
            // Already covered by its consumer's cluster rect; skip to avoid the
            // self-intersection (pure-rect ⊂ own-cluster-rect) noted above.
            continue;
        }
        GroupRects.Add(BpirLayout::GetNodeBounds(N, *Settings, /*bUseClusterBounds=*/true, &Registry));
    }

    for (int32 i = 0; i < GroupRects.Num(); ++i)
    {
        for (int32 j = i + 1; j < GroupRects.Num(); ++j)
        {
            TestFalse(*FString::Printf(TEXT("group[%d] and group[%d] must not intersect"), i, j),
                FSlateRect::DoRectanglesIntersect(GroupRects[i], GroupRects[j]));
        }
    }
    return true;
}
