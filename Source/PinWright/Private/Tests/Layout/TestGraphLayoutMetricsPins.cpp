// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestGraphLayoutMetricsPins.cpp - GraphLayout::ComputeGraphLayoutMetrics backward edges and
// pin-anchored geometry, each in both failure directions: a reversed edge is counted and the
// corrected one is not; identical node rects score differently once their pins disagree; and a
// pin-to-pin crossing the centre basis cannot see is counted.

#include "Misc/AutomationTest.h"

#include "Layout/GraphLayoutMetrics.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutMetricsBackwardEdgeTest,
    "PinWright.layout.metrics.BackwardEdgeCounted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGraphLayoutMetricsBackwardEdgeTest::RunTest(const FString& Parameters)
{
    using namespace GraphLayout;

    // a -> b -> c, left to right; then the same graph with c moved left of b.
    const TArray<FGraphEdge> Edges = { FGraphEdge(TEXT("a"), TEXT("b")), FGraphEdge(TEXT("b"), TEXT("c")) };
    const TArray<FNodeRect> Forward = {
        FNodeRect(TEXT("a"), 0.0, 0.0, 160.0, 80.0),
        FNodeRect(TEXT("b"), 240.0, 0.0, 160.0, 80.0),
        FNodeRect(TEXT("c"), 480.0, 0.0, 160.0, 80.0) };
    const TArray<FNodeRect> Reversed = {
        FNodeRect(TEXT("a"), 0.0, 0.0, 160.0, 80.0),
        FNodeRect(TEXT("b"), 240.0, 0.0, 160.0, 80.0),
        FNodeRect(TEXT("c"), 240.0, 160.0, 160.0, 80.0) };

    const FGraphLayoutMetricsResult Good = ComputeGraphLayoutMetrics(Forward, Edges, 16.0);
    TestEqual(TEXT("the left-to-right chain has no backward edge"), Good.BackwardEdgeCount, 0);

    const FGraphLayoutMetricsResult Bad = ComputeGraphLayoutMetrics(Reversed, Edges, 16.0);
    TestEqual(TEXT("c left of b's output side is one backward edge"), Bad.BackwardEdgeCount, 1);
    TestTrue(TEXT("the backward edge is b -> c"),
        Bad.BackwardEdges.Num() == 1 && Bad.BackwardEdges[0].From == TEXT("b") && Bad.BackwardEdges[0].To == TEXT("c"));

    const FGraphLayoutMetricsResult Mirrored = ComputeGraphLayoutMetrics(Forward, Edges, 16.0, GraphLayout::EFlowDirection::RightToLeft);
    TestEqual(TEXT("read right to left, both edges of the forward chain run backwards"), Mirrored.BackwardEdgeCount, 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutMetricsPinAlignmentTest,
    "PinWright.layout.metrics.PinAlignmentChangesStraightness",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGraphLayoutMetricsPinAlignmentTest::RunTest(const FString& Parameters)
{
    using namespace GraphLayout;

    // Identical rects (identical centres); only the target pin row differs.
    const TArray<FNodeRect> Nodes = {
        FNodeRect(TEXT("a"), 0.0, 0.0, 160.0, 120.0),
        FNodeRect(TEXT("b"), 240.0, 0.0, 160.0, 120.0) };
    const FGraphLayoutMetricsResult Aligned =
        ComputeGraphLayoutMetrics(Nodes, { FGraphEdge(TEXT("a"), 40.0, TEXT("b"), 40.0) }, 16.0);
    const FGraphLayoutMetricsResult Misaligned =
        ComputeGraphLayoutMetrics(Nodes, { FGraphEdge(TEXT("a"), 40.0, TEXT("b"), 104.0) }, 16.0);
    const FGraphLayoutMetricsResult Centres =
        ComputeGraphLayoutMetrics(Nodes, { FGraphEdge(TEXT("a"), TEXT("b")) }, 16.0);

    TestEqual(TEXT("anchored edges report the pin basis"), Aligned.GeometryBasis, FString(TEXT("pins")));
    TestEqual(TEXT("unanchored edges report the centre basis"), Centres.GeometryBasis, FString(TEXT("centers")));
    TestTrue(TEXT("pins on one row give a zero row delta"),
        Aligned.PinRowDeltaPx.Num() == 1 && FMath::IsNearlyZero(Aligned.PinRowDeltaPx[0]));
    TestTrue(TEXT("pins two rows apart give a 64 px row delta"),
        Misaligned.PinRowDeltaPx.Num() == 1 && FMath::IsNearlyEqual(Misaligned.PinRowDeltaPx[0], 64.0));
    TestTrue(TEXT("the aligned wire is straighter than the misaligned one"), Aligned.Straightness > Misaligned.Straightness);
    TestTrue(TEXT("the aligned layout scores higher overall"), Aligned.CombinedScore > Misaligned.CombinedScore);
    TestTrue(TEXT("the centre basis cannot tell the two apart"),
        FMath::IsNearlyEqual(Centres.Straightness, 1.0) && FMath::IsNearlyZero(Centres.PinRowDeltaPx[0]));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutMetricsPinCrossingsTest,
    "PinWright.layout.metrics.PinCrossingsDifferFromCentres",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGraphLayoutMetricsPinCrossingsTest::RunTest(const FString& Parameters)
{
    using namespace GraphLayout;

    // a's top output feeds the lower node c and its bottom output the upper node b: the two wires
    // cross between the columns, but centre-to-centre lines share a's centre and never cross.
    const TArray<FNodeRect> Nodes = {
        FNodeRect(TEXT("a"), 0.0, 0.0, 160.0, 120.0),
        FNodeRect(TEXT("b"), 320.0, -40.0, 160.0, 80.0),
        FNodeRect(TEXT("c"), 320.0, 80.0, 160.0, 80.0) };
    const TArray<FGraphEdge> Pinned = {
        FGraphEdge(TEXT("a"), 40.0, TEXT("c"), 40.0),
        FGraphEdge(TEXT("a"), 80.0, TEXT("b"), 40.0) };
    const TArray<FGraphEdge> Centred = { FGraphEdge(TEXT("a"), TEXT("c")), FGraphEdge(TEXT("a"), TEXT("b")) };

    const FGraphLayoutMetricsResult ByPins = ComputeGraphLayoutMetrics(Nodes, Pinned, 16.0);
    const FGraphLayoutMetricsResult ByCentres = ComputeGraphLayoutMetrics(Nodes, Centred, 16.0);
    TestEqual(TEXT("pin-to-pin wires cross once"), ByPins.EdgeCrossingCount, 1);
    TestEqual(TEXT("centre-to-centre lines do not cross"), ByCentres.EdgeCrossingCount, 0);
    TestTrue(TEXT("the crossing lowers the crossing score"), ByPins.EdgeCrossings < ByCentres.EdgeCrossings);

    const TArray<FGraphEdge> Mixed = { FGraphEdge(TEXT("a"), 40.0, TEXT("c"), 40.0), FGraphEdge(TEXT("a"), TEXT("b")) };
    TestEqual(TEXT("partly anchored edges report the mixed basis"),
        ComputeGraphLayoutMetrics(Nodes, Mixed, 16.0).GeometryBasis, FString(TEXT("mixed")));
    return true;
}
