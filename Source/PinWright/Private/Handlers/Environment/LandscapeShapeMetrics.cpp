// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Environment/LandscapeShapeMetrics.h"
#include "Handlers/ErrorCodes.h"

namespace LandscapeShape
{
    const TArray<FCheckInfo>& AllChecks()
    {
        static const TArray<FCheckInfo> Checks = {
            { ECheck::AxisLocked, TEXT("axis_locked"),
              ErrorCodes::ERR_LANDSCAPE_SHAPE_AXIS_LOCKED, ESeverity::Error, true,
              TEXT("Fraction of stride-K band-boundary chords running within epsDeg of a cell "
                   "axis. A boundary written per cell from an axis-aligned region mask "
                   "approaches 1.0; a curve carried at sub-cell accuracy scores low whatever "
                   "its steepness.") },
            { ECheck::SteppedProfile, TEXT("stepped_profile"),
              ErrorCodes::ERR_LANDSCAPE_SHAPE_STEPPED, ESeverity::Error, true,
              TEXT("Fraction of the region's total height variation carried by ISOLATED risers "
                   "- a nonzero sample-to-sample delta with a flat sample on both sides. One "
                   "tier value per heightfield cell puts all of its rise there; a sculpted "
                   "slope puts none of it there.") },
        };
        static_assert(CheckCount == 2, "AllChecks must list every ECheck in order.");
        return Checks;
    }

    // CheckInfo / ParseCheckId / CheckBit / HasCheck / DefaultCheckMask are the shared
    // contract's, inlined in the header over this table. Unknown ids stay the caller's error
    // and never a silent skip: a typo in `checks` that ran nothing would be indistinguishable
    // from terrain that passed every check - see PinWrightAudit::ParseCheckId.
}

namespace LandscapeShape
{
    // ---- Banding ----------------------------------------------------------------------------
    //
    // The boundary this measures is the outline of the region's height BANDS. Banding is a
    // means of extracting boundaries, not the thing being scored: the verdict comes from the
    // DIRECTION of the resulting chords, so a band width that happens to match the content's
    // own quantisation cannot flatter it. (The metric this replaces scored conformance to a
    // step size and therefore rated quantised noise 0.918 - a statistic that restates the
    // quantisation it was handed cannot fail.)
    static int32 DeriveBandHeight(int32 Range, const FParams& Params)
    {
        if (Params.BandHeight > 0) { return Params.BandHeight; }
        const int32 Bands = FMath::Max(Params.BandCount, 1);
        return FMath::Max(1, FMath::DivideAndRoundUp(Range, Bands));
    }

    // ---- Boundary edges ---------------------------------------------------------------------
    //
    // Cell (x, y) is the unit square with corners (x, y)..(x+1, y+1); a boundary edge is the
    // unit lattice segment between two 4-adjacent cells of different band. Nothing is emitted
    // along the sampled region's own rectangle - a band clipped by the read window has no
    // neighbour there, so the window frame cannot contribute the long axis-aligned runs that
    // would inflate the reading.
    struct FEdgeGraph
    {
        int32 NodesX = 0;                       // lattice width  = SizeX + 1
        TMap<int32, TArray<int32>> Adjacency;   // node index -> neighbours
        TSet<uint64> Edges;                     // packed (lo, hi) node pair
        int32 SkippedMargin = 0;

        int32 Node(int32 X, int32 Y) const { return Y * NodesX + X; }
        static uint64 Key(int32 A, int32 B)
        {
            const int32 Lo = FMath::Min(A, B), Hi = FMath::Max(A, B);
            return (static_cast<uint64>(static_cast<uint32>(Lo)) << 32)
                 | static_cast<uint32>(Hi);
        }
        void Add(int32 A, int32 B)
        {
            if (Edges.Contains(Key(A, B))) { return; }
            Edges.Add(Key(A, B));
            Adjacency.FindOrAdd(A).AddUnique(B);
            Adjacency.FindOrAdd(B).AddUnique(A);
        }
    };

    static void BuildEdges(const TArray<int32>& Bands, int32 SizeX, int32 SizeY,
                           int32 Margin, FEdgeGraph& Out)
    {
        Out.NodesX = SizeX + 1;
        const int32 Lo = Margin;
        const int32 HiX = SizeX - Margin;
        const int32 HiY = SizeY - Margin;
        for (int32 Y = 0; Y < SizeY; ++Y)
        {
            const int32 Row = Y * SizeX;
            for (int32 X = 0; X < SizeX; ++X)
            {
                const int32 V = Bands[Row + X];
                if (X + 1 < SizeX && Bands[Row + X + 1] != V)
                {
                    const int32 PX = X + 1, PY = Y;
                    if (PX >= Lo && PX <= HiX && PY >= Lo && PY <= HiY)
                    {
                        Out.Add(Out.Node(PX, PY), Out.Node(PX, PY + 1));
                    }
                    else { ++Out.SkippedMargin; }
                }
                if (Y + 1 < SizeY && Bands[Row + SizeX + X] != V)
                {
                    const int32 PX = X, PY = Y + 1;
                    if (PX >= Lo && PX <= HiX && PY >= Lo && PY <= HiY)
                    {
                        Out.Add(Out.Node(PX, PY), Out.Node(PX + 1, PY));
                    }
                    else { ++Out.SkippedMargin; }
                }
            }
        }
    }
}

namespace LandscapeShape
{
    // ---- Path tracing -----------------------------------------------------------------------
    //
    // Walk the segment graph into polylines and loops; a node of degree != 2 ends a path.
    // Junctions and endpoints are seeded FIRST so a run is not started in its middle and split
    // into two shorter paths, which would cost chords at the stride.
    static void TracePaths(const FEdgeGraph& Graph, TArray<TArray<int32>>& OutPaths)
    {
        TSet<uint64> Unused = Graph.Edges;
        TArray<int32> Starts;
        Starts.Reserve(Graph.Adjacency.Num());
        for (const TPair<int32, TArray<int32>>& It : Graph.Adjacency)
        {
            if (It.Value.Num() != 2) { Starts.Add(It.Key); }
        }
        for (const TPair<int32, TArray<int32>>& It : Graph.Adjacency)
        {
            if (It.Value.Num() == 2) { Starts.Add(It.Key); }
        }

        for (int32 Start : Starts)
        {
            const TArray<int32>* Neighbours = Graph.Adjacency.Find(Start);
            if (!Neighbours) { continue; }
            for (int32 Next : *Neighbours)
            {
                if (!Unused.Contains(FEdgeGraph::Key(Start, Next))) { continue; }
                Unused.Remove(FEdgeGraph::Key(Start, Next));
                TArray<int32> Path = { Start, Next };
                int32 Prev = Start, Cur = Next;
                while (const TArray<int32>* Adj = Graph.Adjacency.Find(Cur))
                {
                    if (Adj->Num() != 2) { break; }
                    const int32 Step = ((*Adj)[0] == Prev) ? (*Adj)[1] : (*Adj)[0];
                    if (Step == Prev || !Unused.Contains(FEdgeGraph::Key(Cur, Step))) { break; }
                    Unused.Remove(FEdgeGraph::Key(Cur, Step));
                    Prev = Cur;
                    Cur = Step;
                    Path.Add(Cur);
                }
                OutPaths.Add(MoveTemp(Path));
            }
        }
    }

    // ---- Axis fraction ----------------------------------------------------------------------
    //
    // WHY CHORDS AT A STRIDE AND NOT PER SEGMENT. Every boundary segment of a raster is a unit
    // step on the cell lattice, so per segment a traced curve and a stamped mask both score
    // exactly 1.000 - a statistic that cannot fail, which is the defect class this whole verb
    // exists for. At a stride of K cells the chord direction becomes a property of the SHAPE:
    // a rasterised smooth curve spreads its chords across all directions, an outline built
    // from axis-aligned regions keeps them on the axes.
    static void MeasureAxisFraction(const FEdgeGraph& Graph, const TArray<TArray<int32>>& Paths,
                                    const FParams& Params, FMeasurement& Out)
    {
        const int32 Stride = FMath::Max(Params.Stride, 1);
        Out.AngleHistogram.Init(0, 18);
        int32 Axis = 0, Total = 0;
        for (const TArray<int32>& Path : Paths)
        {
            if (Path.Num() <= Stride) { continue; }
            for (int32 I = 0; I + Stride < Path.Num(); I += Stride)
            {
                const int32 AX = Path[I] % Graph.NodesX, AY = Path[I] / Graph.NodesX;
                const int32 BX = Path[I + Stride] % Graph.NodesX;
                const int32 BY = Path[I + Stride] / Graph.NodesX;
                const int32 DX = BX - AX, DY = BY - AY;
                if (DX == 0 && DY == 0) { continue; }
                double Ang = FMath::RadiansToDegrees(FMath::Atan2(double(DY), double(DX)));
                Ang = FMath::Fmod(FMath::Fmod(Ang, 90.0) + 90.0, 90.0);
                const double Dev = FMath::Min(Ang, 90.0 - Ang);
                ++Total;
                Out.AngleHistogram[FMath::Min(17, int32(Ang / 5.0))] += 1;
                if (Dev <= Params.EpsDeg) { ++Axis; }
            }
        }
        Out.Chords = Total;
        if (Total > 0) { Out.AxisFraction = double(Axis) / double(Total); }
    }
}

namespace LandscapeShape
{
    // ---- Stepped profile --------------------------------------------------------------------
    //
    // The companion check, and it exists because the axis metric has a stated blind spot: at
    // stride K a staircase whose risers are shorter than K cells averages out to the diagonal
    // it approximates and reads LOW (the retired gate measured an 8x-upsampled mask at ~0.41).
    // The per-cell case - one tier value written per heightfield cell - is exactly that shape.
    // So this measures the PROFILE instead: how much of the region's height variation is
    // carried by ISOLATED risers, a nonzero sample-to-sample delta with a flat sample on each
    // side. A tier written per cell puts all of its rise there; a sculpted slope puts none.
    //
    // MinStepUnits keeps the numerator honest on gentle organic terrain: a slope shallower than
    // one height unit per sample quantises into flat runs with single-unit risers, which is
    // arithmetic, not authoring. Sub-threshold risers still count in the DENOMINATOR (they are
    // real rise) and are reported separately, so the exclusion is visible rather than assumed.
    static void AccumulateLine(const TArray<uint16>& H, int32 Start, int32 Step, int32 Count,
                               const FParams& Params, FMeasurement& Out)
    {
        if (Count < 2) { return; }
        const int32 Deltas = Count - 1;
        for (int32 I = 0; I < Deltas; ++I)
        {
            const int32 D = int32(H[Start + (I + 1) * Step]) - int32(H[Start + I * Step]);
            if (D == 0) { continue; }
            Out.TotalRise += FMath::Abs(double(D));
            const bool bFlatBefore = (I == 0)
                || (int32(H[Start + I * Step]) - int32(H[Start + (I - 1) * Step])) == 0;
            const bool bFlatAfter = (I == Deltas - 1)
                || (int32(H[Start + (I + 2) * Step]) - int32(H[Start + (I + 1) * Step])) == 0;
            if (!bFlatBefore || !bFlatAfter) { continue; }
            if (FMath::Abs(D) < Params.MinStepUnits) { ++Out.SubThresholdSteps; continue; }
            ++Out.IsolatedSteps;
            Out.SteppedRise += FMath::Abs(double(D));
        }
    }

    void Measure(const TArray<uint16>& Heights, int32 SizeX, int32 SizeY,
                 const FParams& Params, FMeasurement& Out)
    {
        Out = FMeasurement();
        Out.SizeX = SizeX;
        Out.SizeY = SizeY;
        if (SizeX <= 0 || SizeY <= 0 || Heights.Num() != SizeX * SizeY) { return; }

        uint16 MinH = MAX_uint16, MaxH = 0;
        for (uint16 H : Heights) { MinH = FMath::Min(MinH, H); MaxH = FMath::Max(MaxH, H); }
        Out.MinHeight = MinH;
        Out.MaxHeight = MaxH;
        const int32 Range = int32(MaxH) - int32(MinH);
        Out.BandHeight = DeriveBandHeight(Range, Params);

        TArray<int32> Bands;
        Bands.SetNumUninitialized(Heights.Num());
        TSet<int32> Occupied;
        for (int32 I = 0; I < Heights.Num(); ++I)
        {
            Bands[I] = (int32(Heights[I]) - int32(MinH)) / Out.BandHeight;
            Occupied.Add(Bands[I]);
        }
        Out.OccupiedBands = Occupied.Num();

        FEdgeGraph Graph;
        BuildEdges(Bands, SizeX, SizeY, FMath::Max(Params.MarginCells, 0), Graph);
        Out.Edges = Graph.Edges.Num();
        Out.EdgesSkippedMargin = Graph.SkippedMargin;

        TArray<TArray<int32>> Paths;
        TracePaths(Graph, Paths);
        Out.Paths = Paths.Num();
        MeasureAxisFraction(Graph, Paths, Params, Out);

        for (int32 Y = 0; Y < SizeY; ++Y) { AccumulateLine(Heights, Y * SizeX, 1, SizeX, Params, Out); }
        for (int32 X = 0; X < SizeX; ++X) { AccumulateLine(Heights, X, SizeX, SizeY, Params, Out); }
        if (Out.TotalRise > 0.0) { Out.StepFraction = Out.SteppedRise / Out.TotalRise; }
    }
}

namespace LandscapeShape
{
    // ---- Evaluate ---------------------------------------------------------------------------
    //
    // An UNRUNNABLE row is the point of this function. A flat region, a boundary too short to
    // measure at this stride, a region smaller than one chord: none of those is terrain that
    // passed, and each one used to be reported as 0.000 - a fraction over an empty denominator,
    // which is the "I looked at nothing" green this project has already paid for once.
    static void AddFinding(TArray<FFinding>& Out, ECheck Check, EFindingStatus Status,
                           const TCHAR* Code, FString Message)
    {
        FFinding F;
        F.Check = Check;
        F.Status = Status;
        F.Severity = CheckInfo(Check).Severity;
        F.Code = Code;
        F.Message = MoveTemp(Message);
        Out.Add(MoveTemp(F));
    }

    void Evaluate(const FMeasurement& M, const FParams& Params, uint32 SelectedChecks,
                  TArray<FFinding>& OutFindings)
    {
        const bool bFlat = (M.MaxHeight == M.MinHeight);
        const int32 Stride = FMath::Max(Params.Stride, 1);

        if (HasCheck(SelectedChecks, ECheck::AxisLocked))
        {
            if (M.SizeX <= 0 || M.SizeY <= 0)
            {
                AddFinding(OutFindings, ECheck::AxisLocked, EFindingStatus::Unrunnable,
                    ErrorCodes::ERR_LANDSCAPE_SHAPE_REGION_TOO_SMALL,
                    TEXT("The region holds no samples, so no boundary was measured."));
            }
            else if (bFlat)
            {
                AddFinding(OutFindings, ECheck::AxisLocked, EFindingStatus::Unrunnable,
                    ErrorCodes::ERR_LANDSCAPE_SHAPE_FLAT_REGION,
                    FString::Printf(TEXT("Every sample in the %dx%d region sits at height %d, so "
                        "there is no band boundary to measure. Sample a region that spans the "
                        "shape you want checked."), M.SizeX, M.SizeY, M.MinHeight));
            }
            else if (M.Edges == 0)
            {
                AddFinding(OutFindings, ECheck::AxisLocked, EFindingStatus::Unrunnable,
                    ErrorCodes::ERR_LANDSCAPE_SHAPE_NO_BOUNDARY,
                    FString::Printf(TEXT("The region spans %d height units but produced no band "
                        "boundary at bandHeight %d (%d occupied band(s)); %d segment(s) fell in "
                        "the %d-cell margin. Lower bandHeight or widen the region."),
                        M.MaxHeight - M.MinHeight, M.BandHeight, M.OccupiedBands,
                        M.EdgesSkippedMargin, Params.MarginCells));
            }
            else if (M.Chords < Params.MinChords)
            {
                const bool bTooSmall = FMath::Min(M.SizeX, M.SizeY) <= Stride;
                AddFinding(OutFindings, ECheck::AxisLocked, EFindingStatus::Unrunnable,
                    bTooSmall ? ErrorCodes::ERR_LANDSCAPE_SHAPE_REGION_TOO_SMALL
                              : ErrorCodes::ERR_LANDSCAPE_SHAPE_NO_BOUNDARY,
                    FString::Printf(TEXT("Only %d chord(s) over %d boundary segment(s) at stride "
                        "%d, and %d are needed. A fraction over this few is noise wearing a "
                        "decimal point."), M.Chords, M.Edges, Stride, Params.MinChords));
            }
            else if (M.AxisFraction.IsSet() && M.AxisFraction.GetValue() > Params.MaxAxisFraction)
            {
                AddFinding(OutFindings, ECheck::AxisLocked, EFindingStatus::Flagged,
                    ErrorCodes::ERR_LANDSCAPE_SHAPE_AXIS_LOCKED,
                    FString::Printf(TEXT("%.3f of %d boundary chords run within %.1f deg of a "
                        "cell axis, and the bar is %.3f. The outline is following the cell "
                        "lattice rather than a traced curve - what a tier value written per cell "
                        "from an axis-aligned region mask produces, independent of how steep the "
                        "faces are."), M.AxisFraction.GetValue(), M.Chords, Params.EpsDeg,
                        Params.MaxAxisFraction));
            }
        }

        if (HasCheck(SelectedChecks, ECheck::SteppedProfile))
        {
            if (M.TotalRise <= 0.0)
            {
                AddFinding(OutFindings, ECheck::SteppedProfile, EFindingStatus::Unrunnable,
                    ErrorCodes::ERR_LANDSCAPE_SHAPE_FLAT_REGION,
                    TEXT("The region has zero total height variation, so there is no rise to "
                         "attribute to risers or to slopes."));
            }
            else if (M.StepFraction.IsSet() && M.StepFraction.GetValue() > Params.MaxStepFraction)
            {
                AddFinding(OutFindings, ECheck::SteppedProfile, EFindingStatus::Flagged,
                    ErrorCodes::ERR_LANDSCAPE_SHAPE_STEPPED,
                    FString::Printf(TEXT("%.3f of the region's height variation is carried by %d "
                        "isolated riser(s) of at least %d height units, and the bar is %.3f. That "
                        "is a per-cell staircase, not a sculpted slope (%d further riser(s) were "
                        "below the unit threshold and counted only in the denominator)."),
                        M.StepFraction.GetValue(), M.IsolatedSteps, Params.MinStepUnits,
                        Params.MaxStepFraction, M.SubThresholdSteps));
            }
        }
    }
}
