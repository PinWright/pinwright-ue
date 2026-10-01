// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestGraphLayoutMetrics.cpp - Unit tests for the engine-agnostic FGraphLayoutMetrics util and the
// Blueprint node-size adapter (F-graph-layout-metrics-core).
//
// These exercise production code directly: GraphLayout::ComputeGraphLayoutMetrics and
// GraphLayout::FBlueprintNodeSizeAdapter (the UEdGraphNode size estimator). They fail if the
// metrics util's overlap/crossing/score behaviour is reverted or the adapter seam is removed.

#include "Misc/AutomationTest.h"

#include "CompilerTestUtils.h"
#include "Layout/GraphLayoutMetrics.h"
#include "Layout/BlueprintNodeSizeAdapter.h"
#include "EdGraph/EdGraphPin.h"
#include "BpirLayoutSettings.h"
#include "K2Node_CustomEvent.h"
#include "Kismet/KismetMathLibrary.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"

// ============================================================================
// CleanVsOverlap — a well-spaced graph scores high; an overlapping one fails overlap and scores
// strictly lower. This is the ticket's required clean-high / overlap-low assertion.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutMetricsCleanVsOverlapTest,
    "PinWright.layout.metrics.CleanVsOverlap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGraphLayoutMetricsCleanVsOverlapTest::RunTest(const FString& Parameters)
{
    using namespace GraphLayout;

    // Four uniformly spaced, non-overlapping 100x60 nodes in a horizontal chain on a 16px grid.
    TArray<FNodeRect> CleanNodes = {
        FNodeRect(TEXT("a"),   0.0, 0.0, 100.0, 60.0),
        FNodeRect(TEXT("b"), 160.0, 0.0, 100.0, 60.0),
        FNodeRect(TEXT("c"), 320.0, 0.0, 100.0, 60.0),
        FNodeRect(TEXT("d"), 480.0, 0.0, 100.0, 60.0),
    };
    TArray<FGraphEdge> ChainEdges = {
        FGraphEdge(TEXT("a"), TEXT("b")),
        FGraphEdge(TEXT("b"), TEXT("c")),
        FGraphEdge(TEXT("c"), TEXT("d")),
    };

    const FGraphLayoutMetricsResult Clean = ComputeGraphLayoutMetrics(CleanNodes, ChainEdges, 16.0);

    TestFalse(TEXT("Clean layout reports no overlap"), Clean.HasOverlap());
    TestEqual(TEXT("Clean overlap sub-score is 1"), Clean.Overlap, 1.0);
    TestTrue(TEXT("Clean combined score is high"), Clean.CombinedScore > 0.8);

    // Same node set but b is shoved on top of a (positive-area bbox intersection).
    TArray<FNodeRect> OverlapNodes = CleanNodes;
    OverlapNodes[1] = FNodeRect(TEXT("b"), 40.0, 10.0, 100.0, 60.0); // overlaps "a".

    const FGraphLayoutMetricsResult Bad = ComputeGraphLayoutMetrics(OverlapNodes, ChainEdges, 16.0);

    TestTrue(TEXT("Overlapping layout reports overlap (overlap > 0)"), Bad.HasOverlap());
    TestEqual(TEXT("Overlap sub-score collapses to 0"), Bad.Overlap, 0.0);
    TestTrue(TEXT("Overlapping pair is reported"), Bad.OverlappingPairs.Num() >= 1);
    TestTrue(TEXT("Reported overlap area is positive"),
        Bad.OverlappingPairs.Num() >= 1 && Bad.OverlappingPairs[0].Area > 0.0);
    TestTrue(TEXT("Overlapping layout scores strictly below the clean one"),
        Bad.CombinedScore < Clean.CombinedScore);

    return true;
}

// ============================================================================
// EdgeCrossings — crossing edges are detected and penalize the score versus a non-crossing layout
// over the identical node set.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutMetricsEdgeCrossingsTest,
    "PinWright.layout.metrics.EdgeCrossings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGraphLayoutMetricsEdgeCrossingsTest::RunTest(const FString& Parameters)
{
    using namespace GraphLayout;

    // Four nodes at the corners of a square.
    TArray<FNodeRect> Nodes = {
        FNodeRect(TEXT("tl"),   0.0,   0.0, 40.0, 40.0),
        FNodeRect(TEXT("tr"), 400.0,   0.0, 40.0, 40.0),
        FNodeRect(TEXT("bl"),   0.0, 400.0, 40.0, 40.0),
        FNodeRect(TEXT("br"), 400.0, 400.0, 40.0, 40.0),
    };

    // The two diagonals cross in the middle.
    TArray<FGraphEdge> CrossingEdges = {
        FGraphEdge(TEXT("tl"), TEXT("br")),
        FGraphEdge(TEXT("tr"), TEXT("bl")),
    };
    const FGraphLayoutMetricsResult Crossed = ComputeGraphLayoutMetrics(Nodes, CrossingEdges, 16.0);
    TestEqual(TEXT("Two crossing diagonals report one crossing"), Crossed.EdgeCrossingCount, 1);
    TestTrue(TEXT("Crossing penalizes the edge-crossing sub-score"), Crossed.EdgeCrossings < 1.0);

    // The two top/bottom edges do not cross.
    TArray<FGraphEdge> ParallelEdges = {
        FGraphEdge(TEXT("tl"), TEXT("tr")),
        FGraphEdge(TEXT("bl"), TEXT("br")),
    };
    const FGraphLayoutMetricsResult Parallel = ComputeGraphLayoutMetrics(Nodes, ParallelEdges, 16.0);
    TestEqual(TEXT("Parallel edges report no crossing"), Parallel.EdgeCrossingCount, 0);
    TestEqual(TEXT("No crossing keeps the edge-crossing sub-score at 1"), Parallel.EdgeCrossings, 1.0);

    TestTrue(TEXT("Non-crossing layout scores at least as high"),
        Parallel.CombinedScore >= Crossed.CombinedScore);

    return true;
}

// ============================================================================
// BlueprintNodeSizeAdapter — the adapter seam sizes a UEdGraphNode from its title lines, shown pin
// rows and value boxes, and places pin centres the way the editor draws them (deeper header under a
// subtitle, centred pins on compact nodes).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutBlueprintAdapterTest,
    "PinWright.layout.metrics.BlueprintNodeSizeAdapter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGraphLayoutBlueprintAdapterTest::RunTest(const FString& Parameters)
{
    using namespace GraphLayout;

    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestLayoutAdapterBP"));
    if (!BP) { AddError(TEXT("Failed to create transient test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("Transient Blueprint has no ubergraph page")); return false; }

    UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(EventGraph);
    Node->CreateNewGuid();
    Node->CustomFunctionName = TEXT("TestEventForAdapter");
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();
    EventGraph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);

    UBpirLayoutSettings* Settings = NewObject<UBpirLayoutSettings>();
    if (!Settings) { AddError(TEXT("Failed to instantiate UBpirLayoutSettings")); return false; }

    const FBlueprintNodeSizeAdapter Adapter(*Settings);
    const FVector2D ViaAdapter = Adapter.EstimateNodeSize(Node);

    const double Row = Settings->PinRowHeightPx;
    int32 Inputs = 0;
    int32 Outputs = 0;
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (FBlueprintNodeSizeAdapter::IsPinShown(Pin) && !FBlueprintNodeSizeAdapter::IsPinInTitle(Pin))
        {
            ++(Pin->Direction == EGPD_Input ? Inputs : Outputs);
        }
    }
    const int32 Rows = FMath::Max(Inputs, Outputs);
    TestTrue(TEXT("Custom event shows at least one pin row"), Rows >= 1);

    // A custom event's title has a "Custom Event" subtitle line, which the editor draws as a deeper
    // header: its exec output sits one title line (16 px) lower than a plain node's first row.
    const double SubtitledHeader = Settings->HeaderHeightPx + 16.0;
    TestEqual(TEXT("Height is the subtitled header plus the shown pin rows"),
        ViaAdapter.Y, FMath::Max(64.0, SubtitledHeader + (Rows + 0.5) * Row));
    const UEdGraphPin* Then = Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    TestTrue(TEXT("The event's exec output sits half a row under the subtitled header"),
        Then && Adapter.PinOffsetY(Then) == SubtitledHeader + 0.5 * Row);

    // A plain call: exec input on the first row under a one-line header; a long literal on an
    // unlinked input widens the node by its value box.
    UK2Node_CallFunction* Print = CompilerTestUtils::SpawnPrintStringCall(EventGraph, 0, 0);
    const double ShortWidth = Adapter.EstimateNodeSize(Print).X;
    Print->FindPin(TEXT("InString"))->DefaultValue = TEXT("a deliberately long literal that the value box has to show in full");
    TestTrue(TEXT("A long default value widens the node"), Adapter.EstimateNodeSize(Print).X > ShortWidth + 100.0);
    TestEqual(TEXT("A plain node's exec input sits half a row under the header"),
        Adapter.PinOffsetY(Print->FindPin(UEdGraphSchema_K2::PN_Execute)), Settings->HeaderHeightPx + 0.5 * Row);

    // A compact pure node has no header and centres its pins.
    UK2Node_CallFunction* Add = CompilerTestUtils::SpawnNode<UK2Node_CallFunction>(EventGraph, 0, 0);
    Add->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Add_IntInt), UKismetMathLibrary::StaticClass());
    Add->ReconstructNode();
    const UEdGraphPin* Sum = Add->FindPin(UEdGraphSchema_K2::PN_ReturnValue);
    TestTrue(TEXT("A compact node's single output sits at its vertical centre"),
        Sum && FMath::IsNearlyEqual(Adapter.PinOffsetY(Sum), 0.5 * Adapter.EstimateNodeSize(Add).Y));
    TestTrue(TEXT("Adapter width respects 160px minimum"), ViaAdapter.X >= 160.0);
    const UEdGraphPin* Delegate = Node->FindPin(UK2Node_Event::DelegateOutputName, EGPD_Output);
    TestTrue(TEXT("The event's delegate output sits in the title bar"),
        Delegate && FBlueprintNodeSizeAdapter::IsPinInTitle(Delegate));

    // The seam must not crash on a null node; it returns the estimator's minimum bounds.
    const FVector2D NullSize = Adapter.EstimateNodeSize(nullptr);
    TestTrue(TEXT("Adapter tolerates null and returns finite width"), FMath::IsFinite(NullSize.X));
    TestTrue(TEXT("Adapter tolerates null and returns finite height"), FMath::IsFinite(NullSize.Y));

    return true;
}
