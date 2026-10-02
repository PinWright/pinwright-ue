// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestEdGraphNodeMeasure.cpp - Measured UEdGraph node sizes (GraphLayout::MeasureEdGraphNode).
//
// MeasuredSizesMatchGraphPanel: on a Print String (advanced pins folded), a Branch, a variable
// getter and an integer Add, the offscreen measurement is within 2 px of the node widget a graph
// panel builds for the same node, and the estimator is within 15 % of the measurement. Every size
// is also logged, as the evidence of the run mode (offscreen / -NullRHI) the suite ran in.
// EstimatedSizesNeverReportedMeasured: with measurement off, every size is estimated and reported
// so; with it on, every size is measured exactly when Slate can measure.

#include "Misc/AutomationTest.h"

#include "BpirLayoutSettings.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "GraphEditor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Layout/BlueprintNodeSizeAdapter.h"
#include "Layout/EdGraphNodeMeasure.h"
#include "Layout/PwGraphLayoutEdGraph.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/TestSkipReporting.h"

namespace EdGraphNodeMeasureTest
{
    // Print String -> Branch, plus an int variable getter and an int Add left unlinked, so the
    // Add keeps its default-value boxes.
    inline TArray<UEdGraphNode*> SpawnFixture(UBlueprint* BP, UEdGraph* Graph)
    {
        UK2Node_CallFunction* Print = CompilerTestUtils::SpawnPrintStringCall(Graph, 0, 0);
        Print->PostPlacedNewNode();
        UK2Node_IfThenElse* Branch = CompilerTestUtils::SpawnNode<UK2Node_IfThenElse>(Graph, 400, 0);
        CompilerTestUtils::WireThenToExec(Print, Branch);

        FEdGraphPinType IntType;
        IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
        FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("PwMeasuredCount"), IntType);
        UK2Node_VariableGet* Getter = CompilerTestUtils::SpawnNode<UK2Node_VariableGet>(Graph, 0, 300);
        Getter->VariableReference.SetSelfMember(TEXT("PwMeasuredCount"));
        Getter->ReconstructNode();

        UK2Node_CallFunction* Sum = CompilerTestUtils::SpawnNode<UK2Node_CallFunction>(Graph, 300, 300);
        Sum->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Add_IntInt), UKismetMathLibrary::StaticClass());
        Sum->ReconstructNode();
        return { Print, Branch, Getter, Sum };
    }

    inline bool Within(double Value, double Reference, double Tolerance)
    {
        return FMath::Abs(Value - Reference) <= Tolerance;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEdGraphNodeMeasuredSizesTest,
    "PinWright.layout.blueprint.MeasuredSizesMatchGraphPanel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEdGraphNodeMeasuredSizesTest::RunTest(const FString& Parameters)
{
    using namespace EdGraphNodeMeasureTest;

    if (!GraphLayout::CanMeasureEdGraphNodes())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-slate-renderer"),
            TEXT("Slate has no renderer here (commandlet), so node widgets cannot be measured"));
        return true;
    }
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("PwMeasureBP"));
    UEdGraph* Graph = (BP && BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!TestNotNull(TEXT("transient Blueprint with an event graph"), Graph))
    {
        return false;
    }
    const TArray<UEdGraphNode*> Nodes = SpawnFixture(BP, Graph);
    TestTrue(TEXT("Print String folds its advanced pins"), Nodes[0]->AdvancedPinDisplay == ENodeAdvancedPins::Hidden);

    // The same graph in a graph panel, built offscreen: the panel creates and prepasses a widget
    // per node, exactly as an open editor tab does.
    const TSharedRef<SGraphEditor> Editor = SNew(SGraphEditor).GraphToEdit(Graph);
    SGraphPanel* Panel = Editor->GetGraphPanel();
    if (!TestNotNull(TEXT("the graph editor has a panel"), Panel))
    {
        return false;
    }
    Panel->Update();

    const UBpirLayoutSettings* Settings = GetDefault<UBpirLayoutSettings>();
    const GraphLayout::FBlueprintNodeSizeAdapter Estimator(*Settings);
    for (UEdGraphNode* Node : Nodes)
    {
        const FString Name = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
        GraphLayout::FMeasuredNode Measured;
        if (!TestTrue(*FString::Printf(TEXT("%s is measured"), *Name), GraphLayout::MeasureEdGraphNode(Node, Measured)))
        {
            continue;
        }
        const TSharedPtr<SGraphNode> PanelNode = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
        if (!TestTrue(*FString::Printf(TEXT("the panel built a widget for %s"), *Name), PanelNode.IsValid()))
        {
            continue;
        }
        const FVector2D PanelSize(PanelNode->GetDesiredSize());
        const FVector2D Estimated = Estimator.EstimateNodeSize(Node);
        const UEdGraphPin* ExecIn = GetDefault<UEdGraphSchema_K2>()->FindExecutionPin(*Node, EGPD_Input);
        const double* ExecInY = ExecIn ? Measured.PinOffsetY.Find(ExecIn) : nullptr;
        AddInfo(FString::Printf(TEXT("%s: measured %.1f x %.1f, panel %.1f x %.1f, estimated %.1f x %.1f, %d pin rows measured, exec-in y %s (estimated %.1f)"),
            *Name, Measured.Size.X, Measured.Size.Y, PanelSize.X, PanelSize.Y, Estimated.X, Estimated.Y,
            Measured.PinOffsetY.Num(), ExecInY ? *FString::Printf(TEXT("%.1f"), *ExecInY) : TEXT("-"),
            ExecIn ? Estimator.PinOffsetY(ExecIn) : 0.0));

        TestTrue(*FString::Printf(TEXT("%s: measured size is within 2 px of the panel's"), *Name),
            Within(Measured.Size.X, PanelSize.X, 2.0) && Within(Measured.Size.Y, PanelSize.Y, 2.0));
        TestTrue(*FString::Printf(TEXT("%s: estimated size is within 15%% of the measured one"), *Name),
            Within(Estimated.X, Measured.Size.X, 0.15 * Measured.Size.X) && Within(Estimated.Y, Measured.Size.Y, 0.15 * Measured.Size.Y));
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            const double* PinY = Measured.PinOffsetY.Find(Pin);
            if (GraphLayout::FBlueprintNodeSizeAdapter::IsPinShown(Pin) && !GraphLayout::FBlueprintNodeSizeAdapter::IsPinInTitle(Pin))
            {
                TestTrue(*FString::Printf(TEXT("%s: shown pin %s has a measured row inside the node"), *Name, *Pin->PinName.ToString()),
                    PinY && *PinY > 0.0 && *PinY < Measured.Size.Y);
            }
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEdGraphNodeEstimatedNeverMeasuredTest,
    "PinWright.layout.blueprint.EstimatedSizesNeverReportedMeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEdGraphNodeEstimatedNeverMeasuredTest::RunTest(const FString& Parameters)
{
    using namespace EdGraphNodeMeasureTest;

    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("PwEstimateBP"));
    UEdGraph* Graph = (BP && BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!TestNotNull(TEXT("transient Blueprint with an event graph"), Graph))
    {
        return false;
    }
    const TArray<UEdGraphNode*> Nodes = SpawnFixture(BP, Graph);
    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    const GraphLayout::FBlueprintNodeSizeAdapter Estimator(*Settings);

    Settings->bMeasureNodeSizes = false;
    const PwGraphLayout::FEdGraphModel Estimated = PwGraphLayout::BuildEdGraphModel(Graph, TSet<UEdGraphNode*>(Nodes), *Settings);
    for (int32 Index = 0; Index < Estimated.Nodes.Num(); ++Index)
    {
        const PwGraphLayout::FLayoutNode& Node = Estimated.Layout.Nodes[Index];
        TestTrue(*FString::Printf(TEXT("%s is not reported as measured, and carries the estimate"), *Node.Key),
            !Node.bMeasuredSize && Node.Size.Equals(Estimator.EstimateNodeSize(Estimated.Nodes[Index])));
    }
    const PwGraphLayout::FArrangeReport EstimatedReport = PwGraphLayout::ArrangeEdGraph(Graph, Nodes, { Nodes[0] }, *Settings);
    TestTrue(TEXT("with measurement off the report counts every size as estimated"),
        EstimatedReport.MeasuredSizes == 0 && EstimatedReport.EstimatedSizes == Estimated.Nodes.Num());

    Settings->bMeasureNodeSizes = true;
    const bool bCanMeasure = GraphLayout::CanMeasureEdGraphNodes();
    const PwGraphLayout::FEdGraphModel Measured = PwGraphLayout::BuildEdGraphModel(Graph, TSet<UEdGraphNode*>(Nodes), *Settings);
    for (int32 Index = 0; Index < Measured.Nodes.Num(); ++Index)
    {
        GraphLayout::FMeasuredNode Widget;
        const bool bWidget = GraphLayout::MeasureEdGraphNode(Measured.Nodes[Index], Widget);
        TestTrue(*FString::Printf(TEXT("%s is reported measured exactly when a widget measured it"), *Measured.Layout.Nodes[Index].Key),
            Measured.Layout.Nodes[Index].bMeasuredSize == (bCanMeasure && bWidget)
            && (!bWidget || Measured.Layout.Nodes[Index].Size.Equals(Widget.Size)));
    }
    const PwGraphLayout::FArrangeReport MeasuredReport = PwGraphLayout::ArrangeEdGraph(Graph, Nodes, { Nodes[0] }, *Settings);
    TestTrue(TEXT("with measurement on the report counts measured sizes exactly when Slate can measure"),
        bCanMeasure
            ? MeasuredReport.MeasuredSizes == Measured.Nodes.Num() && MeasuredReport.EstimatedSizes == 0
            : MeasuredReport.MeasuredSizes == 0 && MeasuredReport.EstimatedSizes == Measured.Nodes.Num());
    return true;
}
