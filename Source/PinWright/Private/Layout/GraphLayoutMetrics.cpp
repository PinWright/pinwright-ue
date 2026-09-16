// Copyright (c) 2026 Alexander Penkin. MIT License.

// GraphLayoutMetrics.cpp - Pure geometry implementation of engine-agnostic layout-quality metrics.

#include "Layout/GraphLayoutMetrics.h"
#include "Math/Box2D.h"

namespace GraphLayout
{
    namespace
    {
        // Sub-score blend weights (sum to 1). Overlap dominates because any overlap is the hard
        // fail the dependents care about most; the relative metrics share the remainder.
        constexpr double WOverlap = 0.40;
        constexpr double WSpacing = 0.15;
        constexpr double WStraightness = 0.15;
        constexpr double WCrossings = 0.20;
        constexpr double WGrid = 0.10;

        // Positive-area intersection of two axis-aligned rects (0 if they only touch or are apart).
        // Delegates to Core's double-precision FBox2D rather than hand-rolling corner math; its
        // Overlap() returns a zero-area box when the rects merely touch, so the caller's Area > 0.0
        // gate preserves the positive-area-only semantics.
        double IntersectionArea(const FNodeRect& A, const FNodeRect& B)
        {
            const FBox2D BoxA(FVector2D(A.X, A.Y), FVector2D(A.Right(), A.Bottom()));
            const FBox2D BoxB(FVector2D(B.X, B.Y), FVector2D(B.Right(), B.Bottom()));
            return BoxA.Overlap(BoxB).GetArea();
        }

        // Sign of the cross product of (B-A) x (C-A); 0 = collinear.
        double Orient(const FVector2D& A, const FVector2D& B, const FVector2D& C)
        {
            return (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
        }

        // True if open segments P1P2 and P3P4 cross at an interior point. Shared endpoints (edges
        // meeting at a node) and collinear overlaps are deliberately NOT counted as crossings.
        bool SegmentsProperlyIntersect(
            const FVector2D& P1, const FVector2D& P2,
            const FVector2D& P3, const FVector2D& P4)
        {
            const double D1 = Orient(P3, P4, P1);
            const double D2 = Orient(P3, P4, P2);
            const double D3 = Orient(P1, P2, P3);
            const double D4 = Orient(P1, P2, P4);

            // Strict opposite signs on both segments => proper interior crossing.
            return ((D1 > 0.0 && D2 < 0.0) || (D1 < 0.0 && D2 > 0.0))
                && ((D3 > 0.0 && D4 < 0.0) || (D3 < 0.0 && D4 > 0.0));
        }

        // Map a non-negative deviation to a [0,1] score that is 1 at deviation 0 and decays toward
        // 0 as the deviation grows past Scale. Smooth, monotonic, bounded.
        double DecayScore(double Deviation, double Scale)
        {
            if (Scale <= 0.0)
            {
                return Deviation <= 0.0 ? 1.0 : 0.0;
            }
            return Scale / (Scale + FMath::Max(0.0, Deviation));
        }
    }

    FGraphLayoutMetricsResult ComputeGraphLayoutMetrics(
        const TArray<FNodeRect>& Nodes,
        const TArray<FGraphEdge>& Edges,
        double GridSizePx)
    {
        FGraphLayoutMetricsResult Result;

        const int32 N = Nodes.Num();
        // A graph with 0 or 1 node is trivially well-laid-out; nothing to score.
        if (N <= 1)
        {
            Result.CombinedScore = 1.0;
            return Result;
        }

        // --- Overlap: any positive-area bbox intersection is a hard fail. ---
        for (int32 i = 0; i < N; ++i)
        {
            for (int32 j = i + 1; j < N; ++j)
            {
                const double Area = IntersectionArea(Nodes[i], Nodes[j]);
                if (Area > 0.0)
                {
                    FOverlapPair Pair;
                    Pair.A = Nodes[i].NodeId;
                    Pair.B = Nodes[j].NodeId;
                    Pair.Area = Area;
                    Result.OverlappingPairs.Add(Pair);
                }
            }
        }
        Result.Overlap = Result.OverlappingPairs.Num() > 0 ? 0.0 : 1.0;

        // Each node center is constant for the whole call, so compute it once and reuse it for
        // both the spacing nearest-neighbour pass and the edge-metric center lookup below.
        TArray<FVector2D> Centers;
        Centers.Reserve(N);
        for (const FNodeRect& Node : Nodes)
        {
            Centers.Add(Node.Center());
        }

        // --- Spacing: regularity of nearest-neighbour center distances. Low coefficient of
        // variation (uniform spacing) scores high; wildly uneven gaps score low. ---
        {
            // N >= 2 here (see early return), so every node has a nearest neighbour and NearestDist
            // ends up with exactly N entries.
            TArray<double> NearestDist;
            NearestDist.Reserve(N);
            for (int32 i = 0; i < N; ++i)
            {
                double BestSq = TNumericLimits<double>::Max();
                const FVector2D Ci = Centers[i];
                for (int32 j = 0; j < N; ++j)
                {
                    if (i == j) continue;
                    BestSq = FMath::Min(BestSq, FVector2D::DistSquared(Ci, Centers[j]));
                }
                // One sqrt per node on the winning squared distance, not per inner comparison.
                NearestDist.Add(FMath::Sqrt(BestSq));
            }

            double Mean = 0.0;
            for (double D : NearestDist) Mean += D;
            Mean /= NearestDist.Num();

            if (Mean <= KINDA_SMALL_NUMBER)
            {
                // Coincident centers => no spacing at all.
                Result.Spacing = 0.0;
            }
            else
            {
                double Variance = 0.0;
                for (double D : NearestDist)
                {
                    const double Dev = D - Mean;
                    Variance += Dev * Dev;
                }
                Variance /= NearestDist.Num();
                const double CoV = FMath::Sqrt(Variance) / Mean; // coefficient of variation.
                Result.Spacing = DecayScore(CoV, 1.0);
            }
        }

        // --- Build a center lookup for edge-based metrics (reusing the centers computed above). ---
        TMap<FString, FVector2D> CenterById;
        CenterById.Reserve(N);
        for (int32 k = 0; k < N; ++k)
        {
            CenterById.Add(Nodes[k].NodeId, Centers[k]);
        }

        // Resolved edge segments (both endpoints known and distinct).
        struct FSeg { FVector2D P0; FVector2D P1; };
        TArray<FSeg> Segments;
        Segments.Reserve(Edges.Num());
        for (const FGraphEdge& Edge : Edges)
        {
            const FVector2D* A = CenterById.Find(Edge.From);
            const FVector2D* B = CenterById.Find(Edge.To);
            if (A && B && !A->Equals(*B))
            {
                Segments.Add({ *A, *B });
            }
        }

        // --- Straightness: how axis-aligned each edge is. A perfectly horizontal or vertical
        // run scores 1; a 45-degree diagonal scores lowest. Approximated from node centers. ---
        if (Segments.Num() > 0)
        {
            double SumScore = 0.0;
            for (const FSeg& S : Segments)
            {
                const double Dx = FMath::Abs(S.P1.X - S.P0.X);
                const double Dy = FMath::Abs(S.P1.Y - S.P0.Y);
                const double Minor = FMath::Min(Dx, Dy);
                const double Major = FMath::Max(Dx, Dy);
                // Minor/Major is 0 for a perfectly axis-aligned edge, 1 for a 45-deg diagonal.
                const double OffAxis = (Major > KINDA_SMALL_NUMBER) ? (Minor / Major) : 0.0;
                SumScore += (1.0 - OffAxis);
            }
            Result.Straightness = SumScore / Segments.Num();
        }
        else
        {
            Result.Straightness = 1.0;
        }

        // --- Edge crossings: count proper interior crossings of center-to-center segments. ---
        int32 Crossings = 0;
        for (int32 a = 0; a < Segments.Num(); ++a)
        {
            for (int32 b = a + 1; b < Segments.Num(); ++b)
            {
                if (SegmentsProperlyIntersect(Segments[a].P0, Segments[a].P1,
                                              Segments[b].P0, Segments[b].P1))
                {
                    ++Crossings;
                }
            }
        }
        Result.EdgeCrossingCount = Crossings;
        if (Segments.Num() >= 2)
        {
            // Normalize crossings by the number of segment pairs so the score is edge-count
            // independent; even a few crossings should pull the score down noticeably.
            const double Pairs = static_cast<double>(Segments.Num()) * (Segments.Num() - 1) * 0.5;
            const double CrossingRatio = (Pairs > 0.0) ? (Crossings / Pairs) : 0.0;
            // Scale chosen so one crossing among a handful of edges is already a clear penalty.
            Result.EdgeCrossings = DecayScore(CrossingRatio, 0.1);
        }
        else
        {
            Result.EdgeCrossings = 1.0;
        }

        // --- Grid: how close node origins sit to a GridSizePx lattice. ---
        if (GridSizePx > 0.0)
        {
            // Loop-invariant and strictly positive under the GridSizePx > 0.0 guard.
            const double HalfGrid = GridSizePx * 0.5;
            double SumScore = 0.0;
            for (const FNodeRect& Node : Nodes)
            {
                const double Rx = FMath::Abs(Node.X - FMath::RoundToDouble(Node.X / GridSizePx) * GridSizePx);
                const double Ry = FMath::Abs(Node.Y - FMath::RoundToDouble(Node.Y / GridSizePx) * GridSizePx);
                // Per-axis residual is in [0, GridSizePx/2]; normalize to [0,1] off-grid amount.
                const double OffX = Rx / HalfGrid;
                const double OffY = Ry / HalfGrid;
                SumScore += 1.0 - 0.5 * (OffX + OffY);
            }
            Result.Grid = FMath::Clamp(SumScore / N, 0.0, 1.0);
        }
        else
        {
            Result.Grid = 1.0;
        }

        // --- Combined weighted score. ---
        Result.CombinedScore = FMath::Clamp(
            WOverlap * Result.Overlap
            + WSpacing * Result.Spacing
            + WStraightness * Result.Straightness
            + WCrossings * Result.EdgeCrossings
            + WGrid * Result.Grid,
            0.0, 1.0);

        return Result;
    }
}
