// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestPwGraphLayoutAdapters.cpp - PwGraphLayout adapter tests on real graphs: a Blueprint event
// graph (including the BPIR compile pass), a material, an anim graph and a Control Rig graph.
// Geometry is read back through each adapter's Build*Model after the positions were written.

#include "Misc/AutomationTest.h"

#include "AGIR/AGIRCompiler.h"
#include "Algo/Reverse.h"
#include "AnimGraphNode_Root.h"
#include "Animation/AnimBlueprint.h"
#include "BpirLayoutSettings.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Layout/BlueprintNodeSizeAdapter.h"
#include "Layout/PwGraphLayoutEdGraph.h"
#include "Layout/PwGraphLayoutMaterial.h"
#include "Layout/PwGraphLayoutRigVM.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Misc/ScopeExit.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "ScopedTransaction.h"
#include "Tests/Assets/CRIRTestHelpers.h"
#include "Tests/Assets/TestAGIRFixtures.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/Gameplay/TestAnimationFixtures.h"
#include "Tests/Layout/PwGraphLayoutTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Units/RigUnit.h"

namespace PwGraphLayoutAdapterTest
{
    using namespace PwGraphLayoutTest;

    // Index of the wire between two model nodes, INDEX_NONE when there is none.
    inline int32 FindWire(const FLayoutGraph& G, int32 From, int32 To)
    {
        return G.Wires.IndexOfByPredicate([From, To](const FLayoutWire& W) { return W.FromNode == From && W.ToNode == To; });
    }

    inline UK2Node_CallFunction* SpawnCall(UEdGraph* Graph, FName Function, UClass* Owner)
    {
        UK2Node_CallFunction* Node = CompilerTestUtils::SpawnNode<UK2Node_CallFunction>(Graph, 0, 0);
        Node->FunctionReference.SetExternalMember(Function, Owner);
        Node->ReconstructNode();
        return Node;
    }

    inline void Link(UEdGraphNode* From, const TCHAR* FromPin, UEdGraphNode* To, const TCHAR* ToPin)
    {
        From->FindPin(FromPin, EGPD_Output)->MakeLinkTo(To->FindPin(ToPin, EGPD_Input));
    }

    inline TMap<UEdGraphNode*, FIntPoint> Positions(const TArray<UEdGraphNode*>& Nodes)
    {
        TMap<UEdGraphNode*, FIntPoint> Out;
        for (UEdGraphNode* Node : Nodes)
        {
            Out.Add(Node, FIntPoint(Node->NodePosX, Node->NodePosY));
        }
        return Out;
    }
}

// Event -> Print -> Branch -> (Print, Print), with a pure Add -> ToString chain feeding the first
// Print. Covers the K2 adapter end to end: overlaps, backward wires, pin alignment, undo, a
// reordered Graph->Nodes and a second pass.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutBlueprintAdapterTest,
    "PinWright.layout.blueprint.EventGraphFixture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutBlueprintAdapterTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutAdapterTest;

    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("PwLayoutAdapterBP"));
    UEdGraph* Graph = (BP && BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!TestNotNull(TEXT("transient Blueprint with an event graph"), Graph))
    {
        return false;
    }

    UK2Node_CustomEvent* Event = CompilerTestUtils::SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    Event->CustomFunctionName = TEXT("PwLayoutEvent");
    Event->ReconstructNode();
    UK2Node_CallFunction* Print = CompilerTestUtils::SpawnPrintStringCall(Graph, 0, 0);
    UK2Node_IfThenElse* Branch = CompilerTestUtils::SpawnNode<UK2Node_IfThenElse>(Graph, 0, 0);
    UK2Node_CallFunction* PrintTrue = CompilerTestUtils::SpawnPrintStringCall(Graph, 0, 0);
    UK2Node_CallFunction* PrintFalse = CompilerTestUtils::SpawnPrintStringCall(Graph, 0, 0);
    UK2Node_CallFunction* ToText = SpawnCall(Graph, GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary, Conv_IntToString), UKismetStringLibrary::StaticClass());
    UK2Node_CallFunction* Sum = SpawnCall(Graph, GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Add_IntInt), UKismetMathLibrary::StaticClass());

    CompilerTestUtils::WireThenToExec(Event, Print);
    CompilerTestUtils::WireThenToExec(Print, Branch);
    Link(Branch, TEXT("then"), PrintTrue, TEXT("execute"));
    Link(Branch, TEXT("else"), PrintFalse, TEXT("execute"));
    Link(Sum, TEXT("ReturnValue"), ToText, TEXT("InInt"));
    Link(ToText, TEXT("ReturnValue"), Print, TEXT("InString"));

    // The spawn helpers run PostPlacedNewNode before the function is set, so a Print String misses
    // the Development Only state the editor gives it on placement, and the undo below would add it
    // (UK2Node_CallFunction::Serialize fixes the state up on load), changing the node's height mid
    // test. Re-run placement now that the function is known, as the editor's spawn order does.
    for (UK2Node_CallFunction* Call : { Print, PrintTrue, PrintFalse, ToText, Sum })
    {
        Call->PostPlacedNewNode();
    }
    TestEqual(TEXT("Print String is Development Only, as the editor places it"),
        Print->GetDesiredEnabledState(), ENodeEnabledState::DevelopmentOnly);

    const TArray<UEdGraphNode*> Movable = { Event, Print, Branch, PrintTrue, PrintFalse, ToText, Sum };
    for (UEdGraphNode* Node : Movable)
    {
        Node->SetFlags(RF_Transactional);
    }
    const TMap<UEdGraphNode*, FIntPoint> Before = Positions(Movable);
    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();

    {
        FScopedTransaction Transaction(NSLOCTEXT("PinWrightTests", "PwLayoutBlueprint", "Arrange test graph"));
        const FArrangeReport Report = ArrangeEdGraph(Graph, Movable, { Event }, *Settings);
        TestTrue(TEXT("the arrange reports moved nodes"), Report.Moved > 0);
        TestTrue(TEXT("sizes are reported as estimated"), Report.EstimatedSizes > 0 && Report.MeasuredSizes == 0);
    }
    const TMap<UEdGraphNode*, FIntPoint> Arranged = Positions(Movable);

    FEdGraphModel Model = BuildEdGraphModel(Graph, TSet<UEdGraphNode*>(Movable), *Settings);
    const FLayoutGraph& G = Model.Layout;
    auto At = [&Model](UEdGraphNode* Node) { return Model.Nodes.IndexOfByKey(Node); };
    TestEqual(TEXT("no arranged node overlaps any node"), CountOverlaps(G), 0);
    TestEqual(TEXT("no wire runs backwards"), CountBackward(G), 0);
    const TPair<UEdGraphNode*, UEdGraphNode*> Straight[] = {
        { Event, Print }, { Print, Branch }, { Branch, PrintTrue }, { ToText, Print } };
    for (const TPair<UEdGraphNode*, UEdGraphNode*>& Pair : Straight)
    {
        const int32 Wire = FindWire(G, At(Pair.Key), At(Pair.Value));
        TestTrue(*FString::Printf(TEXT("%s -> %s is horizontal"), *Pair.Key->GetName(), *Pair.Value->GetName()),
            Wire != INDEX_NONE && IsHorizontal(G, Wire));
    }
    // The event's exec output and the call's exec input must be level on screen. The event's
    // delegate pin sits in its title bar (no row), but its "Custom Event" subtitle deepens the
    // header, so level pins mean the call's NodePosY is one title line below the event's.
    const GraphLayout::FBlueprintNodeSizeAdapter Sizer(*Settings);
    TestEqual(TEXT("the event's exec output and its first call's exec input are level"),
        Event->NodePosY + Sizer.PinOffsetY(Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)),
        Print->NodePosY + Sizer.PinOffsetY(Print->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)));
    // The event -> print exec wire crosses the pure chain's columns; the chain stays below it.
    const double ExecWireY = Print->NodePosY + Sizer.PinOffsetY(Print->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
    TestTrue(TEXT("the pure chain stays below the incoming exec wire"),
        Top(G, At(Sum)) > ExecWireY && Top(G, At(ToText)) > ExecWireY);
    TestTrue(TEXT("the false branch stacks below the true branch"), PrintFalse->NodePosY > PrintTrue->NodePosY);
    TestTrue(TEXT("the pure chain sits between the event and its consumer"),
        Sum->NodePosX >= Right(G, At(Event)) && Right(G, At(ToText)) <= Print->NodePosX);
    TestEqual(TEXT("X is on the grid"), Print->NodePosX % Settings->GridSnapPx, 0);

    TestTrue(TEXT("undo succeeds"), GEditor && GEditor->UndoTransaction(/*bCanRedo=*/false));
    TestTrue(TEXT("undo restores every original position"), Positions(Movable).OrderIndependentCompareEqual(Before));

    // Same graph, Graph->Nodes reversed: identical positions.
    Algo::Reverse(Graph->Nodes);
    ArrangeEdGraph(Graph, Movable, { Event }, *Settings);
    TestTrue(TEXT("a reordered Graph->Nodes gives identical positions"), Positions(Movable).OrderIndependentCompareEqual(Arranged));

    const FArrangeReport Second = ArrangeEdGraph(Graph, Movable, { Event }, *Settings);
    TestEqual(TEXT("a second pass moves nothing"), Second.Moved, 0);
    return true;
}

// The BPIR compile pass: every created event chain in one graph is laid out (not only the first),
// and the kill switch leaves positions alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutBpirPassTest,
    "PinWright.layout.blueprint.CompilePassArrangesEveryEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutBpirPassTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutAdapterTest;

    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("PwLayoutPassBP"));
    UEdGraph* Graph = (BP && BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!TestNotNull(TEXT("transient Blueprint with an event graph"), Graph))
    {
        return false;
    }

    TArray<UEdGraphNode*> Created;
    TArray<TPair<UEdGraphNode*, UEdGraphNode*>> Chains;
    for (int32 Index = 0; Index < 2; ++Index)
    {
        UK2Node_CustomEvent* Event = CompilerTestUtils::SpawnNode<UK2Node_CustomEvent>(Graph, 0, 2000);
        Event->CustomFunctionName = *FString::Printf(TEXT("PwLayoutEntry%d"), Index);
        Event->ReconstructNode();
        UK2Node_CallFunction* Body = CompilerTestUtils::SpawnPrintStringCall(Graph, 0, 2000);
        CompilerTestUtils::WireThenToExec(Event, Body);
        Created.Append({ Event, Body });
        Chains.Emplace(Event, Body);
    }
    TArray<FGuid> Guids;
    for (UEdGraphNode* Node : Created)
    {
        Guids.Add(Node->NodeGuid);
    }

    UBpirLayoutSettings* Settings = GetMutableDefault<UBpirLayoutSettings>();
    const bool bWasEnabled = Settings->bEnableBpirLayoutPass;
    ON_SCOPE_EXIT { Settings->bEnableBpirLayoutPass = bWasEnabled; };

    Settings->bEnableBpirLayoutPass = false;
    FBpirCompiler::RunLayoutPassForTest(BP, Guids, Graph, TArray<UEdGraph*>(), nullptr);
    for (UEdGraphNode* Node : Created)
    {
        TestTrue(TEXT("the kill switch leaves positions alone"), Node->NodePosX == 0 && Node->NodePosY == 2000);
    }

    Settings->bEnableBpirLayoutPass = true;
    FBpirCompiler::RunLayoutPassForTest(BP, Guids, Graph, TArray<UEdGraph*>(), nullptr);
    FEdGraphModel Model = BuildEdGraphModel(Graph, TSet<UEdGraphNode*>(Created), *Settings);
    auto At = [&Model](UEdGraphNode* Node) { return Model.Nodes.IndexOfByKey(Node); };
    TestEqual(TEXT("no created node overlaps any node"), CountOverlaps(Model.Layout), 0);
    for (const TPair<UEdGraphNode*, UEdGraphNode*>& Chain : Chains)
    {
        const int32 Wire = FindWire(Model.Layout, At(Chain.Key), At(Chain.Value));
        TestTrue(*FString::Printf(TEXT("%s's body is laid out beside it"), *Chain.Key->GetName()),
            Wire != INDEX_NONE && IsHorizontal(Model.Layout, Wire)
            && Chain.Value->NodePosX >= Right(Model.Layout, At(Chain.Key)));
    }
    return true;
}

// Texture * Tint + Bias -> BaseColor, every expression at (0,0): the graph grows leftwards from the
// output node, which stays at Material->EditorX/Y.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutMaterialAdapterTest,
    "PinWright.layout.material.GrowsLeftFromOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutMaterialAdapterTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutAdapterTest;

    UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transactional);
    if (!TestNotNull(TEXT("transient material"), Material))
    {
        return false;
    }
    auto Make = [Material](UClass* Class)
    {
        UMaterialExpression* Expression = NewObject<UMaterialExpression>(Material, Class, NAME_None, RF_Transactional);
        Expression->MaterialExpressionGuid = FGuid::NewGuid();
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Expression);
        return Expression;
    };
    UMaterialExpression* Texture = Make(UMaterialExpressionTextureSample::StaticClass());
    UMaterialExpression* Tint = Make(UMaterialExpressionConstant3Vector::StaticClass());
    UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Make(UMaterialExpressionMultiply::StaticClass()));
    UMaterialExpression* Bias = Make(UMaterialExpressionConstant::StaticClass());
    UMaterialExpressionAdd* Add = Cast<UMaterialExpressionAdd>(Make(UMaterialExpressionAdd::StaticClass()));
    Multiply->A.Expression = Texture;
    Multiply->B.Expression = Tint;
    Add->A.Expression = Multiply;
    Add->B.Expression = Bias;
    Material->GetEditorOnlyData()->BaseColor.Expression = Add;
    const FIntPoint Output(Material->EditorX, Material->EditorY);

    {
        FScopedTransaction Transaction(NSLOCTEXT("PinWrightTests", "PwLayoutMaterial", "Arrange test material"));
        const FArrangeReport Report = ArrangeMaterial(Material);
        TestEqual(TEXT("every expression moved off the origin"), Report.Moved, 5);
    }
    FMaterialModel Model = BuildMaterialModel(Material, nullptr);
    TArray<FIntPoint> Arranged;
    for (int32 Index = 0; Index < Model.Expressions.Num(); ++Index)
    {
        Model.Layout.Nodes[Index].bMovable = true; // count overlaps among the arranged expressions
        Arranged.Add(FIntPoint(Model.Expressions[Index]->MaterialExpressionEditorX, Model.Expressions[Index]->MaterialExpressionEditorY));
    }
    const FLayoutGraph& G = Model.Layout;
    const int32 Root = Model.Expressions.Num();
    TestTrue(TEXT("the output node did not move"), FIntPoint(Material->EditorX, Material->EditorY) == Output);
    TestEqual(TEXT("no expressions overlap"), CountOverlaps(G), 0);
    TestEqual(TEXT("no wire runs backwards"), CountBackward(G), 0);
    for (int32 Index = 0; Index < Root; ++Index)
    {
        TestTrue(TEXT("every expression sits left of the output"), Right(G, Index) <= Left(G, Root));
    }
    const int32 ToOutput = FindWire(G, Model.Expressions.IndexOfByKey(Add), Root);
    TestTrue(TEXT("add -> base color is horizontal"), ToOutput != INDEX_NONE && IsHorizontal(G, ToOutput));

    TestTrue(TEXT("undo succeeds"), GEditor && GEditor->UndoTransaction(/*bCanRedo=*/false));
    for (UMaterialExpression* Expression : Model.Expressions)
    {
        TestTrue(TEXT("undo puts every expression back at the origin"),
            Expression->MaterialExpressionEditorX == 0 && Expression->MaterialExpressionEditorY == 0);
    }
    ArrangeMaterial(Material);
    for (int32 Index = 0; Index < Model.Expressions.Num(); ++Index)
    {
        TestTrue(TEXT("re-arranging reproduces the positions"),
            FIntPoint(Model.Expressions[Index]->MaterialExpressionEditorX, Model.Expressions[Index]->MaterialExpressionEditorY)
                == Arranged[Index]);
    }
    return true;
}

// AGIR compile with layout on: a two-pose additive chain into the output pose.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutAnimAdapterTest,
    "PinWright.layout.anim.PoseChainGrowsLeftFromResult",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutAnimAdapterTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutAdapterTest;

    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Animation;
    if (!TestTrue(TEXT("skeleton fixture created"), Animation.Create(TEXT("PwLayout"))))
    {
        return false;
    }
    const FString Path = FString::Printf(TEXT("/Game/PinWrightTests/ABP_PwLayout_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT { CleanupTestAsset(Path); };
    UAnimBlueprint* AnimBP = AGIRTestFixtures::CreateFreshAnimBlueprint(Path, Animation.Skeleton);
    if (!TestNotNull(TEXT("anim blueprint created"), AnimBP))
    {
        return false;
    }

    FAGIRCompileOptions Options;
    Options.Context = AnimBP->GetPathName();
    Options.Mode = EAGIRCompileMode::Replace;
    Options.bRunLayout = true;
    Options.bSave = false;
    const FAGIRCompileResult Result = FAGIRCompiler::Compile(TEXT(
        "entry anim_graph \"AnimGraph\" {\n"
        "    %base = call `/Script/AnimGraph.AnimGraphNode_SequencePlayer`\n"
        "    %extra = call `/Script/AnimGraph.AnimGraphNode_SequencePlayer`\n"
        "    %additive = call `/Script/AnimGraph.AnimGraphNode_ApplyAdditive`(Base: %base, Additive: %extra)\n"
        "    output %additive\n"
        "}\n"), Options);
    if (!TestTrue(*FString::Printf(TEXT("AGIR compile (%s: %s)"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess))
    {
        return false;
    }

    UEdGraph* Graph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!TestNotNull(TEXT("anim graph"), Graph))
    {
        return false;
    }
    TSet<UEdGraphNode*> All;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        All.Add(Node);
    }
    FEdGraphModel Model = BuildEdGraphModel(Graph, All, *GetDefault<UBpirLayoutSettings>());
    const FLayoutGraph& G = Model.Layout;
    const int32 Root = Model.Nodes.IndexOfByPredicate([](const UEdGraphNode* Node) { return Node->IsA<UAnimGraphNode_Root>(); });
    if (!TestTrue(TEXT("output pose node present"), Root != INDEX_NONE))
    {
        return false;
    }
    TestEqual(TEXT("no anim nodes overlap"), CountOverlaps(G), 0);
    TestEqual(TEXT("no pose wire runs backwards"), CountBackward(G), 0);
    int32 IntoRoot = 0;
    for (int32 Wire = 0; Wire < G.Wires.Num(); ++Wire)
    {
        if (G.Wires[Wire].ToNode == Root)
        {
            ++IntoRoot;
            TestTrue(TEXT("the pose into the output is horizontal"), IsHorizontal(G, Wire));
        }
    }
    TestEqual(TEXT("one pose wire reaches the output"), IntoRoot, 1);
    for (int32 Index = 0; Index < G.Nodes.Num(); ++Index)
    {
        if (Index != Root)
        {
            TestTrue(*FString::Printf(TEXT("%s sits left of the output"), *Model.Nodes[Index]->GetName()),
                Right(G, Index) <= Left(G, Root));
        }
    }
    TestEqual(TEXT("a second pass moves nothing"), ArrangeAnimBlueprint(AnimBP).Moved, 0);

    // Undo: back at the origin, arranged inside a transaction, then undone.
    TArray<UEdGraphNode*> PoseNodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node && !Node->IsA<UAnimGraphNode_Root>())
        {
            Node->SetFlags(RF_Transactional);
            Node->NodePosX = 0;
            Node->NodePosY = 0;
            PoseNodes.Add(Node);
        }
    }
    {
        FScopedTransaction Transaction(NSLOCTEXT("PinWrightTests", "PwLayoutAnim", "Arrange test anim graph"));
        TestTrue(TEXT("the pose nodes are arranged again"), ArrangeAnimBlueprint(AnimBP).Moved > 0);
    }
    TestTrue(TEXT("undo succeeds"), GEditor && GEditor->UndoTransaction(/*bCanRedo=*/false));
    for (const UEdGraphNode* Node : PoseNodes)
    {
        TestTrue(*FString::Printf(TEXT("undo puts %s back at the origin"), *Node->GetName()),
            Node->NodePosX == 0 && Node->NodePosY == 0);
    }
    return true;
}

// Three chained integer adds at (0,0): data flows left to right, aligned, around the graph's
// default nodes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGraphLayoutRigAdapterTest,
    "PinWright.layout.controlrig.DataChainFlowsRight",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGraphLayoutRigAdapterTest::RunTest(const FString& Parameters)
{
    using namespace PwGraphLayoutAdapterTest;

    const FString Name = FString::Printf(TEXT("CR_PwLayout_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT { CleanupTestAsset(TEXT("/Game/PinWrightTests/") + Name); };
    FString CreateError;
    UControlRigBlueprint* RigBP = Cast<UControlRigBlueprint>(
        McpCreateControlRigBlueprint(Name, TEXT("/Game/PinWrightTests"), nullptr, CreateError));
    if (!RigBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("Control Rig blueprint unavailable (%s) - skipped."), *CreateError));
        return true;
    }
    URigVMGraph* Graph = nullptr;
    URigVMController* Controller = GetFirstModelController(RigBP, Graph);
    if (!TestNotNull(TEXT("rig model controller"), Controller))
    {
        return false;
    }

    TArray<URigVMNode*> Adds;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        Adds.Add(Controller->AddUnitNodeFromStructPath(
            TEXT("/Script/RigVM.RigVMFunction_MathIntAdd"), FRigUnit::GetMethodName(), FVector2D::ZeroVector,
            FString::Printf(TEXT("PwLayoutAdd%d"), Index), /*bSetupUndoRedo*/ false, /*bPrintPythonCommand*/ false));
        if (!TestNotNull(TEXT("add node created"), Adds.Last()))
        {
            return false;
        }
    }
    for (int32 Index = 0; Index + 1 < Adds.Num(); ++Index)
    {
        TestTrue(TEXT("adds linked"), Controller->AddLink(
            Adds[Index]->GetName() + TEXT(".Result"), Adds[Index + 1]->GetName() + TEXT(".A"),
            /*bSetupUndoRedo*/ false, /*bPrintPythonCommand*/ false));
    }

    const TSet<URigVMNode*> Movable(Adds);
    ArrangeRigVMGraph(Graph, Movable, Controller);
    TArray<FVector2D> Arranged;
    for (URigVMNode* Node : Adds)
    {
        Arranged.Add(Node->GetPosition());
    }

    FRigVMModel Model = BuildRigVMModel(Graph, Movable);
    const FLayoutGraph& G = Model.Layout;
    {
        // The Control Rig editor draws output pins above input pins (execute, outputs, IO, inputs).
        const FLayoutNode& AddNode = G.Nodes[Model.Nodes.IndexOfByKey(Adds[0])];
        const FPinSlot* FirstOut = AddNode.Pins.FindByPredicate([](const FPinSlot& Pin) { return Pin.Side == EPinSide::Output; });
        const FPinSlot* FirstIn = AddNode.Pins.FindByPredicate([](const FPinSlot& Pin) { return Pin.Side == EPinSide::Input; });
        TestTrue(TEXT("the Result output row sits above the A / B input rows"),
            FirstOut && FirstIn && FirstOut->OffsetY < FirstIn->OffsetY);
    }
    TestEqual(TEXT("no arranged node overlaps any node"), CountOverlaps(G), 0);
    TestEqual(TEXT("no wire runs backwards"), CountBackward(G), 0);
    for (int32 Index = 0; Index + 1 < Adds.Num(); ++Index)
    {
        const int32 From = Model.Nodes.IndexOfByKey(Adds[Index]);
        const int32 To = Model.Nodes.IndexOfByKey(Adds[Index + 1]);
        const int32 Wire = FindWire(G, From, To);
        TestTrue(TEXT("result -> A is horizontal"), Wire != INDEX_NONE && IsHorizontal(G, Wire));
        TestTrue(TEXT("data flows left to right"), Right(G, From) <= Left(G, To));
    }

    for (URigVMNode* Node : Adds)
    {
        Controller->SetNodePosition(Node, FVector2D::ZeroVector, false, false, false);
    }
    ArrangeRigVMGraph(Graph, Movable, Controller);
    for (int32 Index = 0; Index < Adds.Num(); ++Index)
    {
        TestTrue(TEXT("re-arranging reproduces the positions"), Adds[Index]->GetPosition().Equals(Arranged[Index]));
    }
    TestEqual(TEXT("a second pass moves nothing"), ArrangeRigVMGraph(Graph, Movable, Controller).Moved, 0);
    return true;
}
