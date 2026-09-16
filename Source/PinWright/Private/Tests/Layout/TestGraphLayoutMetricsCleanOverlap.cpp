// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestGraphLayoutMetricsCleanOverlap.cpp - Regression coverage for F-graph-layout-metrics-core.
//
// Exercises the production GraphLayout::ComputeGraphLayoutMetrics directly (no copy).
// The ticket's acceptance bar: a clean, well-spaced graph must score HIGH with no
// overlap (Overlap sub-score == 1, empty overlapping-pair list), while a layout where
// two nodes overlap must report overlap (Overlap sub-score 0 + a non-empty pair list)
// AND a strictly LOWER combined score. If ComputeGraphLayoutMetrics's overlap detection
// or the overlap-dominant score blend were reverted, the assertions below would fail.
//
// Note: a second, complementary suite lives at Private/Tests/Bpir/TestGraphLayoutMetrics.cpp
// (CleanVsOverlap / EdgeCrossings / BlueprintNodeSizeAdapter). The two suites use distinct
// automation names and test-class names — and now distinct file basenames so the non-unity
// build's intermediate .obj outputs do not collide — so both register and run independently.

#include "Misc/AutomationTest.h"
#include "Layout/GraphLayoutMetrics.h"

namespace
{
    using GraphLayout::FNodeRect;
    using GraphLayout::FGraphEdge;
    using GraphLayout::FGraphLayoutMetricsResult;

    // A tidy 2x2 grid of equal 100x60 nodes on a 20px pitch, wired in an
    // axis-aligned chain (A-right->B, A-down->C, C-right->D). No two boxes touch.
    void BuildCleanGraph(TArray<FNodeRect>& Nodes, TArray<FGraphEdge>& Edges)
    {
        Nodes = {
            FNodeRect(TEXT("A"),   0.0,   0.0, 100.0, 60.0),
            FNodeRect(TEXT("B"), 200.0,   0.0, 100.0, 60.0),
            FNodeRect(TEXT("C"),   0.0, 160.0, 100.0, 60.0),
            FNodeRect(TEXT("D"), 200.0, 160.0, 100.0, 60.0),
        };
        Edges = {
            FGraphEdge(TEXT("A"), TEXT("B")),
            FGraphEdge(TEXT("A"), TEXT("C")),
            FGraphEdge(TEXT("C"), TEXT("D")),
        };
    }
}

// ============================================================================
// Clean graph scores high with zero overlap.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutMetricsCleanGraphTest,
    "PinWright.layout.metrics.CleanGraphScoresHigh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGraphLayoutMetricsCleanGraphTest::RunTest(const FString& Parameters)
{
    TArray<FNodeRect> Nodes;
    TArray<FGraphEdge> Edges;
    BuildCleanGraph(Nodes, Edges);

    const FGraphLayoutMetricsResult R =
        GraphLayout::ComputeGraphLayoutMetrics(Nodes, Edges, /*GridSizePx=*/20.0);

    TestFalse(TEXT("Clean grid reports no overlap"), R.HasOverlap());
    TestEqual(TEXT("Clean grid overlap sub-score is perfect"), R.Overlap, 1.0);
    TestEqual(TEXT("Clean grid has no overlapping pairs"), R.OverlappingPairs.Num(), 0);
    TestEqual(TEXT("Clean grid has no edge crossings"), R.EdgeCrossingCount, 0);
    TestTrue(TEXT("Clean grid combined score is high"), R.CombinedScore > 0.85);
    TestTrue(TEXT("Combined score stays in [0,1]"),
        R.CombinedScore >= 0.0 && R.CombinedScore <= 1.0);
    return true;
}

// ============================================================================
// An overlapping layout reports overlap and a strictly lower score than the
// clean baseline — this is the assertion that fails if the fix is reverted.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutMetricsOverlapPenalizedTest,
    "PinWright.layout.metrics.OverlapPenalized",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGraphLayoutMetricsOverlapPenalizedTest::RunTest(const FString& Parameters)
{
    TArray<FNodeRect> CleanNodes;
    TArray<FGraphEdge> Edges;
    BuildCleanGraph(CleanNodes, Edges);

    const FGraphLayoutMetricsResult Clean =
        GraphLayout::ComputeGraphLayoutMetrics(CleanNodes, Edges, /*GridSizePx=*/20.0);

    // Move node B so it lands squarely on top of node A (same id set / edges).
    TArray<FNodeRect> BadNodes = CleanNodes;
    for (FNodeRect& Node : BadNodes)
    {
        if (Node.NodeId == TEXT("B"))
        {
            Node.X = 40.0;  // overlaps A's [0,100]x[0,60] box
            Node.Y = 20.0;
        }
    }

    const FGraphLayoutMetricsResult Bad =
        GraphLayout::ComputeGraphLayoutMetrics(BadNodes, Edges, /*GridSizePx=*/20.0);

    TestTrue(TEXT("Overlapping layout reports overlap"), Bad.HasOverlap());
    TestEqual(TEXT("Overlapping layout's overlap sub-score collapses to 0"), Bad.Overlap, 0.0);
    TestTrue(TEXT("Overlapping pair is reported with positive area"),
        Bad.OverlappingPairs.Num() >= 1 && Bad.OverlappingPairs[0].Area > 0.0);
    TestTrue(TEXT("Overlapping layout scores strictly lower than the clean one"),
        Bad.CombinedScore < Clean.CombinedScore);
    return true;
}
