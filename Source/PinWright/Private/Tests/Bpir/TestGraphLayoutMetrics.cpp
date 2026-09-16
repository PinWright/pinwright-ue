// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestGraphLayoutMetrics.cpp - Unit tests for the engine-agnostic FGraphLayoutMetrics util and the
// Blueprint node-size adapter (F-graph-layout-metrics-core).
//
// These exercise production code directly: GraphLayout::ComputeGraphLayoutMetrics and
// GraphLayout::FBlueprintNodeSizeAdapter (which wraps BpirLayout::EstimateNodeSize). They fail if
// the metrics util's overlap/crossing/score behaviour is reverted or the adapter seam is removed.

#include "Misc/AutomationTest.h"

#include "CompilerTestUtils.h"
#include "Layout/GraphLayoutMetrics.h"
#include "Layout/BlueprintNodeSizeAdapter.h"
#include "Compiler/NodeLayoutEngine.h"
#include "BpirLayoutSettings.h"
#include "K2Node_CustomEvent.h"
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
// BlueprintNodeSizeAdapter — the adapter seam routes a UEdGraphNode through to the real
// BpirLayout::EstimateNodeSize and returns the identical size.
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
    const FVector2D Direct = BpirLayout::EstimateNodeSize(Node, *Settings);

    TestEqual(TEXT("Adapter size matches the direct estimator (width)"), ViaAdapter.X, Direct.X);
    TestEqual(TEXT("Adapter size matches the direct estimator (height)"), ViaAdapter.Y, Direct.Y);
    TestTrue(TEXT("Adapter width respects 160px minimum"), ViaAdapter.X >= 160.0);

    // The seam must not crash on a null node; it returns the estimator's minimum bounds.
    const FVector2D NullSize = Adapter.EstimateNodeSize(nullptr);
    TestTrue(TEXT("Adapter tolerates null and returns finite width"), FMath::IsFinite(NullSize.X));
    TestTrue(TEXT("Adapter tolerates null and returns finite height"), FMath::IsFinite(NullSize.Y));

    return true;
}
