// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for PinWrightTileGrid - the single georeference + tile-grid contract behind the
// reference-image workflow. The header is pure (no UObject, no editor, no RHI), so every case
// here runs headless on synthetic input.
//
// What is actually being defended:
//   * world -> pixel -> world is a true round trip, on every supported axis mapping;
//   * a point exactly on a tile seam lands in ONE tile, deterministically;
//   * the axis mapping derived from a rotation matches the empirically MEASURED mapping recorded
//     in docs/wiki-src/level-review.framing-math.md (pitch -90/yaw 0 -> +X up, +Y right;
//     pitch -90/yaw -90 -> +X right, +Y down). A derivation that drifted from the measurement
//     would mirror every coordinate a reader takes off an image, and a mirrored map looks
//     plausible.

#include "Misc/AutomationTest.h"

#include "Handlers/Render/TileGridUtils.h"

namespace
{
    using namespace PinWrightTileGrid;

    // 4 x 2 tiles of 512 px over 20000 x 10000 cm: image 2048 x 1024, 9.765625 cm/px on BOTH
    // axes (square pixels, so it is capturable). +X right, +Y down.
    FTileGrid PwTileGridMakeSquareGrid()
    {
        FTileGrid Grid;
        FString Err;
        if (!MakeTileGrid(MakeWorldExtent(-10000.0, -5000.0, 10000.0, 5000.0),
            TopDownXRightYDown(), 4, 2, 512, 512, Grid, Err))
        {
            // Leaves the default-constructed (invalid) grid, which every assertion below fails on
            // loudly rather than silently measuring against a fallback.
            return FTileGrid();
        }
        return Grid;
    }

    bool PwTileGridNearly(double A, double B, double Tolerance = 1.0e-6)
    {
        return FMath::Abs(A - B) <= Tolerance;
    }

    bool PwTileGridVectorNearly(const FVector& A, const FVector& B, double Tolerance = 1.0e-6)
    {
        return PwTileGridNearly(A.X, B.X, Tolerance) && PwTileGridNearly(A.Y, B.Y, Tolerance) &&
            PwTileGridNearly(A.Z, B.Z, Tolerance);
    }
}

// ============================================================================
// world -> pixel -> world round trip, and the anchor pixels of the extent
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwTileGridRoundTripTest,
    "PinWright.render.tile_grid.WorldPixelRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwTileGridRoundTripTest::RunTest(const FString& Parameters)
{
    const FTileGrid Grid = PwTileGridMakeSquareGrid();

    TestEqual(TEXT("image width"), Grid.ImageWidth(), 2048);
    TestEqual(TEXT("image height"), Grid.ImageHeight(), 1024);
    TestTrue(TEXT("square pixels"), Grid.HasSquarePixels());
    TestTrue(TEXT("cm per pixel across"), PwTileGridNearly(Grid.WorldUnitsPerPixelAcross(), 20000.0 / 2048.0));

    // The extent corners land on the image corners, not half a pixel off.
    TestTrue(TEXT("world min corner -> pixel (0,0)"),
        WorldToPixel(Grid, FVector(-10000.0, -5000.0, 0.0)).Equals(FVector2D(0.0, 0.0), 1.0e-6));
    TestTrue(TEXT("world max corner -> pixel (2048,1024)"),
        WorldToPixel(Grid, FVector(10000.0, 5000.0, 0.0)).Equals(FVector2D(2048.0, 1024.0), 1.0e-6));
    TestTrue(TEXT("world centre -> image centre"),
        WorldToPixel(Grid, FVector::ZeroVector).Equals(FVector2D(1024.0, 512.0), 1.0e-6));

    // Round trip on awkward, non-grid-aligned points. Depth is carried by the caller, not the
    // georeference, so it must come back exactly as supplied.
    const FVector Probes[] =
    {
        FVector(1234.5, -678.25, 42.0),
        FVector(-9999.0, 4999.5, -1250.0),
        FVector(0.0, 0.0, 0.0),
        FVector(7777.125, 1234.875, 999.0),
    };
    for (const FVector& Probe : Probes)
    {
        const FVector2D Pixel = WorldToPixel(Grid, Probe);
        const FVector Back = PixelToWorld(Grid, Pixel, Probe.Z);
        TestTrue(*FString::Printf(TEXT("world->pixel->world round trip for %s"), *Probe.ToString()),
            PwTileGridVectorNearly(Back, Probe, 1.0e-6));
    }

    // ...and the other direction, so neither map is merely the other's left inverse.
    const FVector2D PixelProbes[] = { FVector2D(0.0, 0.0), FVector2D(37.25, 991.75), FVector2D(2048.0, 1024.0) };
    for (const FVector2D& Pixel : PixelProbes)
    {
        const FVector World = PixelToWorld(Grid, Pixel, 0.0);
        TestTrue(*FString::Printf(TEXT("pixel->world->pixel round trip for %s"), *Pixel.ToString()),
            WorldToPixel(Grid, World).Equals(Pixel, 1.0e-6));
    }

    // PixelCentreToWorld addresses the CENTRE of an integer pixel, half a pixel off its corner.
    const FVector Corner = PixelToWorld(Grid, FVector2D(10.0, 20.0), 0.0);
    const FVector Centre = PixelCentreToWorld(Grid, 10, 20, 0.0);
    TestTrue(TEXT("pixel centre is half a pixel from its corner"),
        PwTileGridNearly(Centre.X - Corner.X, 0.5 * Grid.WorldUnitsPerPixelAcross()) &&
        PwTileGridNearly(Centre.Y - Corner.Y, 0.5 * Grid.WorldUnitsPerPixelDown()));

    return true;
}

// ============================================================================
// Tile seams: one tile, never two, never none
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwTileGridSeamTest,
    "PinWright.render.tile_grid.TileSeamLandsInExactlyOneTile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwTileGridSeamTest::RunTest(const FString& Parameters)
{
    const FTileGrid Grid = PwTileGridMakeSquareGrid();

    FTilePixel Just;
    TestTrue(TEXT("just left of the seam resolves"), PixelToTilePixel(Grid, FVector2D(511.99, 10.0), Just));
    TestEqual(TEXT("just left of the seam is column 0"), Just.Tile.Col, 0);

    FTilePixel OnSeam;
    TestTrue(TEXT("exactly on the seam resolves"), PixelToTilePixel(Grid, FVector2D(512.0, 10.0), OnSeam));
    TestEqual(TEXT("seam belongs to the tile on its right"), OnSeam.Tile.Col, 1);
    TestTrue(TEXT("seam sits at pixel 0 of that tile"), PwTileGridNearly(OnSeam.PixelInTile.X, 0.0));

    FTilePixel RowSeam;
    TestTrue(TEXT("row seam resolves"), PixelToTilePixel(Grid, FVector2D(10.0, 512.0), RowSeam));
    TestEqual(TEXT("row seam belongs to the tile below"), RowSeam.Tile.Row, 1);
    TestTrue(TEXT("row seam sits at pixel 0 of that tile"), PwTileGridNearly(RowSeam.PixelInTile.Y, 0.0));

    // The far image edge has no tile to its right, so it clamps into the LAST tile rather than
    // falling out of the grid - the extent's Max corner has to stay addressable.
    FTilePixel FarEdge;
    TestTrue(TEXT("far edge resolves"), PixelToTilePixel(Grid, FVector2D(2048.0, 1024.0), FarEdge));
    TestEqual(TEXT("far edge is the last column"), FarEdge.Tile.Col, 3);
    TestEqual(TEXT("far edge is the last row"), FarEdge.Tile.Row, 1);
    TestTrue(TEXT("far edge sits at the tile's far pixel"),
        PwTileGridNearly(FarEdge.PixelInTile.X, 512.0) && PwTileGridNearly(FarEdge.PixelInTile.Y, 512.0));

    // Outside the image is a refusal, not a clamped in-frame answer.
    FTilePixel Outside;
    TestFalse(TEXT("negative pixel is outside"), PixelToTilePixel(Grid, FVector2D(-0.01, 10.0), Outside));
    TestFalse(TEXT("past the far edge is outside"), PixelToTilePixel(Grid, FVector2D(2048.01, 10.0), Outside));

    // The same rule reached from world space: X = -5000 is exactly the col-0/col-1 seam.
    FTilePixel WorldSeam;
    TestTrue(TEXT("world seam resolves"), WorldToTilePixel(Grid, FVector(-5000.0, 0.0, 0.0), WorldSeam));
    TestEqual(TEXT("world seam is column 1"), WorldSeam.Tile.Col, 1);
    TestTrue(TEXT("world seam is at pixel 0 of column 1"), PwTileGridNearly(WorldSeam.PixelInTile.X, 0.0));

    // Round trip through the tile-local frame.
    const FVector Probe(3456.0, -1234.0, 77.0);
    FTilePixel Split;
    TestTrue(TEXT("probe resolves to a tile"), WorldToTilePixel(Grid, Probe, Split));
    TestTrue(TEXT("tile-local round trip"),
        PwTileGridVectorNearly(TilePixelToWorld(Grid, Split.Tile, Split.PixelInTile, Probe.Z), Probe, 1.0e-6));

    // Every tile's world sub-extent tiles the whole extent without gaps or overlap.
    FWorldExtent2D First;
    FWorldExtent2D Last;
    TestTrue(TEXT("first tile extent"), TileWorldExtent(Grid, FTileIndex(0, 0), First));
    TestTrue(TEXT("last tile extent"), TileWorldExtent(Grid, FTileIndex(3, 1), Last));
    TestTrue(TEXT("first tile starts at the world min"),
        PwTileGridNearly(First.Min.X, -10000.0) && PwTileGridNearly(First.Min.Y, -5000.0));
    TestTrue(TEXT("last tile ends at the world max"),
        PwTileGridNearly(Last.Max.X, 10000.0) && PwTileGridNearly(Last.Max.Y, 5000.0));
    TestTrue(TEXT("tile spans are one tile wide"),
        PwTileGridNearly(First.SpanAcross(), 5000.0) && PwTileGridNearly(First.SpanDown(), 5000.0));

    FWorldExtent2D OutOfRange;
    TestFalse(TEXT("tile outside the grid is refused"), TileWorldExtent(Grid, FTileIndex(4, 0), OutOfRange));

    return true;
}

// ============================================================================
// Axis mappings, including the two empirically measured top-down poses
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwTileGridAxisMappingTest,
    "PinWright.render.tile_grid.AxisMappingMatchesMeasuredPoses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwTileGridAxisMappingTest::RunTest(const FString& Parameters)
{
    FScreenAxisMapping Mapping;
    FString Err;

    // Measured in docs/wiki-src/level-review.framing-math.md: pitch -90 / yaw 0 -> +X up, +Y right.
    TestTrue(TEXT("pitch -90 yaw 0 derives"),
        TryMakeAxisMappingFromEffectiveRotation(FRotator(-90.0f, 0.0f, 0.0f), Mapping, Err));
    TestTrue(TEXT("pitch -90 yaw 0 is +X up, +Y right"), Mapping == TopDownXUpYRight());

    // Measured: pitch -90 / yaw -90 -> +X right, +Y down.
    TestTrue(TEXT("pitch -90 yaw -90 derives"),
        TryMakeAxisMappingFromEffectiveRotation(FRotator(-90.0f, -90.0f, 0.0f), Mapping, Err));
    TestTrue(TEXT("pitch -90 yaw -90 is +X right, +Y down"), Mapping == TopDownXRightYDown());

    // Horizontal ortho views put world Z on screen Y.
    TestTrue(TEXT("yaw 0 (looking along +X) derives"),
        TryMakeAxisMappingFromEffectiveRotation(FRotator(0.0f, 0.0f, 0.0f), Mapping, Err));
    TestTrue(TEXT("yaw 0 is +Y right, +Z up"), Mapping == FrontYRightZUp());
    TestEqual(TEXT("yaw 0 depth axis is world X"), static_cast<int32>(Mapping.DepthAxis()),
        static_cast<int32>(EWorldAxis::X));

    TestTrue(TEXT("yaw -90 (looking along -Y) derives"),
        TryMakeAxisMappingFromEffectiveRotation(FRotator(0.0f, -90.0f, 0.0f), Mapping, Err));
    TestTrue(TEXT("yaw -90 is +X right, +Z up"), Mapping == SideXRightZUp());

    // A tilted pose has NO axis mapping. Snapping to the nearest one is exactly the quiet
    // wrongness this refusal exists to prevent.
    TestFalse(TEXT("a 45-degree pitch is refused"),
        TryMakeAxisMappingFromEffectiveRotation(FRotator(-45.0f, 0.0f, 0.0f), Mapping, Err));
    TestTrue(TEXT("the refusal is typed"), Err.Contains(TEXT("NON_CARDINAL_ROTATION")));
    TestFalse(TEXT("an off-axis yaw is refused"),
        TryMakeAxisMappingFromEffectiveRotation(FRotator(-90.0f, 30.0f, 0.0f), Mapping, Err));

    // Each mapping actually moves pixels the way its name claims. Same square world box under
    // two different mappings; the screen direction of a +X step is opposite.
    const FWorldExtent2D Square = MakeWorldExtent(-1000.0, -1000.0, 1000.0, 1000.0);
    FTileGrid UpRight;
    FTileGrid RightDown;
    TestTrue(TEXT("+X up grid builds"),
        MakeTileGrid(Square, TopDownXUpYRight(), 2, 2, 256, 256, UpRight, Err));
    TestTrue(TEXT("+X right grid builds"),
        MakeTileGrid(Square, TopDownXRightYDown(), 2, 2, 256, 256, RightDown, Err));

    const FVector2D UpRightOrigin = WorldToPixel(UpRight, FVector::ZeroVector);
    const FVector2D UpRightPlusX = WorldToPixel(UpRight, FVector(500.0, 0.0, 0.0));
    TestTrue(TEXT("+X up: a +X step moves UP the image"), UpRightPlusX.Y < UpRightOrigin.Y);
    TestTrue(TEXT("+X up: a +X step does not move sideways"),
        PwTileGridNearly(UpRightPlusX.X, UpRightOrigin.X));
    const FVector2D UpRightPlusY = WorldToPixel(UpRight, FVector(0.0, 500.0, 0.0));
    TestTrue(TEXT("+X up: a +Y step moves RIGHT"), UpRightPlusY.X > UpRightOrigin.X);

    const FVector2D RightDownOrigin = WorldToPixel(RightDown, FVector::ZeroVector);
    const FVector2D RightDownPlusX = WorldToPixel(RightDown, FVector(500.0, 0.0, 0.0));
    TestTrue(TEXT("+X right: a +X step moves RIGHT"), RightDownPlusX.X > RightDownOrigin.X);
    const FVector2D RightDownPlusY = WorldToPixel(RightDown, FVector(0.0, 500.0, 0.0));
    TestTrue(TEXT("+X right: a +Y step moves DOWN"), RightDownPlusY.Y > RightDownOrigin.Y);

    // A side view round-trips through world Z on screen Y, with world Y as the untouched depth.
    FTileGrid Side;
    TestTrue(TEXT("side grid builds"),
        MakeTileGrid(MakeWorldExtent(-2000.0, -500.0, 2000.0, 1500.0), SideXRightZUp(), 4, 2, 256, 256, Side, Err));
    const FVector SideProbe(123.0, 9999.0, 456.0);
    const FVector SideBack = PixelToWorld(Side, WorldToPixel(Side, SideProbe), SideProbe.Y);
    TestTrue(TEXT("side view round trip keeps X and Z"),
        PwTileGridNearly(SideBack.X, SideProbe.X) && PwTileGridNearly(SideBack.Z, SideProbe.Z));
    TestTrue(TEXT("side view depth is the caller's world Y"), PwTileGridNearly(SideBack.Y, SideProbe.Y));

    // A mapping that puts one world axis on both screen axes is unrepresentable as a grid.
    FScreenAxisMapping Degenerate;
    Degenerate.ScreenXAxis = EWorldAxis::X;
    Degenerate.ScreenYAxis = EWorldAxis::X;
    FTileGrid Bad;
    TestFalse(TEXT("degenerate mapping is refused"),
        MakeTileGrid(Square, Degenerate, 2, 2, 256, 256, Bad, Err));
    TestTrue(TEXT("the degenerate refusal is typed"), Err.Contains(TEXT("DEGENERATE_AXIS_MAPPING")));

    return true;
}

// ============================================================================
// A non-square extent, and the square-pixel requirement an ortho capture imposes
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwTileGridNonSquareTest,
    "PinWright.render.tile_grid.NonSquareExtent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwTileGridNonSquareTest::RunTest(const FString& Parameters)
{
    FString Err;

    // 2:1 world extent carried by a 2:1 tile layout -> square pixels, capturable.
    const FTileGrid Wide = PwTileGridMakeSquareGrid();
    TestTrue(TEXT("2:1 extent over a 2:1 tile layout has square pixels"), Wide.HasSquarePixels());

    // The SAME 2:1 extent over a square tile layout: the georeference is still a valid linear
    // map (and still round-trips), but the pixels are not square.
    FTileGrid Stretched;
    TestTrue(TEXT("stretched grid builds"),
        MakeTileGrid(MakeWorldExtent(-10000.0, -5000.0, 10000.0, 5000.0), TopDownXRightYDown(),
            4, 4, 512, 512, Stretched, Err));
    TestFalse(TEXT("stretched grid does not have square pixels"), Stretched.HasSquarePixels());
    TestTrue(TEXT("cm/px differs by exactly 2x"),
        PwTileGridNearly(Stretched.WorldUnitsPerPixelAcross(), 2.0 * Stretched.WorldUnitsPerPixelDown()));

    const FVector Probe(-3333.0, 1111.0, 5.0);
    TestTrue(TEXT("stretched grid still round-trips"),
        PwTileGridVectorNearly(PixelToWorld(Stretched, WorldToPixel(Stretched, Probe), Probe.Z), Probe, 1.0e-6));

    // ...but it cannot be captured: an orthographic frame has only orthoWidth, so its vertical
    // world span is forced to orthoWidth * height / width.
    FTileCapturePlan Plan;
    TestFalse(TEXT("a non-square grid refuses a capture plan"),
        ComputeTileCapturePlan(Stretched, FTileIndex(0, 0), 5000.0, Plan, Err));
    TestTrue(TEXT("the refusal names the reason"), Err.Contains(TEXT("NON_SQUARE_PIXELS")));

    return true;
}

// ============================================================================
// Per-tile capture plan: camera on the tile centre, orthoWidth = the tile's world span
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwTileGridCapturePlanTest,
    "PinWright.render.tile_grid.TileCapturePlan",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwTileGridCapturePlanTest::RunTest(const FString& Parameters)
{
    const FTileGrid Grid = PwTileGridMakeSquareGrid();
    FString Err;

    FTileCapturePlan Plan;
    TestTrue(TEXT("tile (0,0) plans"), ComputeTileCapturePlan(Grid, FTileIndex(0, 0), 25000.0, Plan, Err));
    // Ortho frames are centred on the camera POSITION, not a look-at point.
    TestTrue(TEXT("camera sits on the tile centre"),
        PwTileGridVectorNearly(Plan.CameraLocation, FVector(-7500.0, -2500.0, 25000.0), 1.0e-6));
    TestTrue(TEXT("orthoWidth is the tile's world span"), PwTileGridNearly(Plan.OrthoWidth, 5000.0, 1.0e-3));
    TestEqual(TEXT("plan pixel width"), Plan.PixelWidth, 512);
    TestEqual(TEXT("plan pixel height"), Plan.PixelHeight, 512);
    // There is no orthoHeight: the vertical span is orthoWidth * height / width, and that must
    // equal the tile's own world span or the capture would render a different area than the
    // georeference reports.
    const double DerivedVerticalSpan = static_cast<double>(Plan.OrthoWidth) *
        static_cast<double>(Plan.PixelHeight) / static_cast<double>(Plan.PixelWidth);
    TestTrue(TEXT("derived vertical span matches the tile extent"),
        PwTileGridNearly(DerivedVerticalSpan, Plan.World.SpanDown(), 1.0e-3));

    FTileCapturePlan Far;
    TestTrue(TEXT("tile (3,1) plans"), ComputeTileCapturePlan(Grid, FTileIndex(3, 1), 25000.0, Far, Err));
    TestTrue(TEXT("far tile camera sits on its own centre"),
        PwTileGridVectorNearly(Far.CameraLocation, FVector(7500.0, 2500.0, 25000.0), 1.0e-6));

    FTileCapturePlan Refused;
    TestFalse(TEXT("a tile outside the grid is refused"),
        ComputeTileCapturePlan(Grid, FTileIndex(4, 0), 0.0, Refused, Err));
    TestTrue(TEXT("the refusal is typed"), Err.Contains(TEXT("TILE_OUT_OF_RANGE")));
    TestFalse(TEXT("a default-constructed tile index is refused"),
        ComputeTileCapturePlan(Grid, FTileIndex(), 0.0, Refused, Err));

    return true;
}

// ============================================================================
// Planning a grid from a target resolution
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwTileGridPlanTest,
    "PinWright.render.tile_grid.PlanFromResolution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwTileGridPlanTest::RunTest(const FString& Parameters)
{
    FString Err;
    FTileGridPlan Plan;

    // 20480 x 10240 cm at 10 cm/px in 1024 px tiles divides EXACTLY: 2 x 1 tiles, no expansion.
    TestTrue(TEXT("exact plan succeeds"),
        PlanTileGrid(MakeWorldExtent(0.0, 0.0, 20480.0, 10240.0), TopDownXRightYDown(),
            1024, 1024, 10.0, 0, Plan, Err));
    TestEqual(TEXT("exact plan cols"), Plan.Grid.Cols, 2);
    TestEqual(TEXT("exact plan rows"), Plan.Grid.Rows, 1);
    TestFalse(TEXT("exact plan does not expand the extent"), Plan.bExtentExpanded);
    TestTrue(TEXT("exact plan hits the requested resolution"), PwTileGridNearly(Plan.WorldUnitsPerPixel, 10.0));

    // A remainder is absorbed by EXPANDING the extent about its centre, never by coarsening the
    // scale - a coarser scale is the measurement error this workflow exists to remove.
    TestTrue(TEXT("inexact plan succeeds"),
        PlanTileGrid(MakeWorldExtent(0.0, 0.0, 21000.0, 10240.0), TopDownXRightYDown(),
            1024, 1024, 10.0, 0, Plan, Err));
    TestEqual(TEXT("inexact plan rounds columns up"), Plan.Grid.Cols, 3);
    TestTrue(TEXT("inexact plan reports the expansion"), Plan.bExtentExpanded);
    TestTrue(TEXT("inexact plan keeps the requested resolution"), PwTileGridNearly(Plan.WorldUnitsPerPixel, 10.0));
    TestTrue(TEXT("expansion is centred on the request"),
        PwTileGridNearly(Plan.Grid.World.Centre().X, 10500.0) &&
        PwTileGridNearly(Plan.Grid.World.Centre().Y, 5120.0));
    TestTrue(TEXT("expanded extent covers the request"),
        Plan.Grid.World.Min.X <= 0.0 && Plan.Grid.World.Max.X >= 21000.0);

    // The tile budget is a refusal, not a silent downgrade.
    TestFalse(TEXT("tile budget refuses"),
        PlanTileGrid(MakeWorldExtent(0.0, 0.0, 21000.0, 10240.0), TopDownXRightYDown(),
            1024, 1024, 10.0, 2, Plan, Err));
    TestTrue(TEXT("the budget refusal is typed"), Err.Contains(TEXT("TILE_BUDGET_EXCEEDED")));

    // Nonsense inputs are refused before any arithmetic runs.
    TestFalse(TEXT("zero resolution refused"),
        PlanTileGrid(MakeWorldExtent(0.0, 0.0, 100.0, 100.0), TopDownXRightYDown(), 256, 256, 0.0, 0, Plan, Err));
    TestFalse(TEXT("empty extent refused"),
        PlanTileGrid(MakeWorldExtent(0.0, 0.0, 0.0, 100.0), TopDownXRightYDown(), 256, 256, 1.0, 0, Plan, Err));
    TestFalse(TEXT("zero tile size refused"),
        PlanTileGrid(MakeWorldExtent(0.0, 0.0, 100.0, 100.0), TopDownXRightYDown(), 0, 256, 1.0, 0, Plan, Err));
    TestFalse(TEXT("an absurd span/scale ratio refused before it overflows"),
        PlanTileGrid(MakeWorldExtent(0.0, 0.0, 1.0e12, 1.0e12), TopDownXRightYDown(), 8, 8, 0.001, 0, Plan, Err));
    TestTrue(TEXT("the overflow refusal is typed"), Err.Contains(TEXT("GRID_TOO_LARGE")));

    return true;
}

// ============================================================================
// Grid snapping, negatives included
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwTileGridSnapTest,
    "PinWright.render.tile_grid.SnapToWorldGrid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwTileGridSnapTest::RunTest(const FString& Parameters)
{
    // Positive side.
    TestTrue(TEXT("0 snaps to 0"), PwTileGridNearly(SnapToWorldGrid(0.0, 100.0), 0.0));
    TestTrue(TEXT("49 snaps down"), PwTileGridNearly(SnapToWorldGrid(49.0, 100.0), 0.0));
    TestTrue(TEXT("50 snaps up (tie toward +inf)"), PwTileGridNearly(SnapToWorldGrid(50.0, 100.0), 100.0));
    TestTrue(TEXT("150 snaps up"), PwTileGridNearly(SnapToWorldGrid(150.0, 100.0), 200.0));
    TestTrue(TEXT("12345 snaps to 12300"), PwTileGridNearly(SnapToWorldGrid(12345.0, 100.0), 12300.0));

    // Negative side - the branch that goes wrong when someone truncates toward zero.
    TestTrue(TEXT("-49 snaps to 0"), PwTileGridNearly(SnapToWorldGrid(-49.0, 100.0), 0.0));
    TestTrue(TEXT("-50 snaps to 0 (tie toward +inf)"), PwTileGridNearly(SnapToWorldGrid(-50.0, 100.0), 0.0));
    TestTrue(TEXT("-51 snaps to -100"), PwTileGridNearly(SnapToWorldGrid(-51.0, 100.0), -100.0));
    TestTrue(TEXT("-149 snaps to -100"), PwTileGridNearly(SnapToWorldGrid(-149.0, 100.0), -100.0));
    TestTrue(TEXT("-150 snaps to -100 (tie toward +inf)"), PwTileGridNearly(SnapToWorldGrid(-150.0, 100.0), -100.0));
    TestTrue(TEXT("-151 snaps to -200"), PwTileGridNearly(SnapToWorldGrid(-151.0, 100.0), -200.0));
    TestTrue(TEXT("-12345 snaps to -12300"), PwTileGridNearly(SnapToWorldGrid(-12345.0, 100.0), -12300.0));

    // A snapped value is always an exact multiple, on both signs.
    for (double Value = -1000.0; Value <= 1000.0; Value += 37.5)
    {
        const double Snapped = SnapToWorldGrid(Value, 100.0);
        TestTrue(*FString::Printf(TEXT("%.1f snaps onto a multiple of 100"), Value),
            PwTileGridNearly(FMath::Fmod(Snapped, 100.0), 0.0, 1.0e-9));
        TestTrue(*FString::Printf(TEXT("%.1f moves less than half a cell"), Value),
            FMath::Abs(Snapped - Value) <= 50.0 + 1.0e-9);
    }

    // Non-100 grids, and the default the map work uses.
    TestTrue(TEXT("-300 on a 250 grid"), PwTileGridNearly(SnapToWorldGrid(-300.0, 250.0), -250.0));
    TestTrue(TEXT("the 100 uu default is a value, not a hardcode"),
        PwTileGridNearly(SnapToWorldGrid(1234.0, DefaultWorldSnapGridCm), 1200.0));

    // A zero or negative grid size has no meaningful snap; the value passes through untouched.
    TestTrue(TEXT("grid 0 passes through"), PwTileGridNearly(SnapToWorldGrid(1234.5, 0.0), 1234.5));
    TestTrue(TEXT("negative grid passes through"), PwTileGridNearly(SnapToWorldGrid(1234.5, -100.0), 1234.5));

    // Vector overloads snap every component independently.
    const FVector Snapped = SnapToWorldGrid(FVector(149.0, -149.0, 12345.0), 100.0);
    TestTrue(TEXT("vector snap"),
        PwTileGridVectorNearly(Snapped, FVector(100.0, -100.0, 12300.0), 1.0e-9));
    const FVector2D Snapped2D = SnapToWorldGrid(FVector2D(-51.0, 51.0), 100.0);
    TestTrue(TEXT("vector2d snap"), Snapped2D.Equals(FVector2D(-100.0, 100.0), 1.0e-9));

    return true;
}

// ============================================================================
// Validation: a default-constructed grid is a failure, not an empty success
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwTileGridValidationTest,
    "PinWright.render.tile_grid.ValidationRejectsDegenerateGrids",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwTileGridValidationTest::RunTest(const FString& Parameters)
{
    FString Err;
    const FTileGrid Empty;
    TestFalse(TEXT("a default-constructed grid is invalid"), ValidateTileGrid(Empty, Err));
    TestFalse(TEXT("IsValid agrees"), Empty.IsValid());
    TestTrue(TEXT("the failure names a reason"), !Err.IsEmpty());

    // An invalid grid does not silently produce plausible coordinates.
    TestTrue(TEXT("WorldToPixel on an invalid grid is zero"),
        WorldToPixel(Empty, FVector(1.0, 2.0, 3.0)).Equals(FVector2D::ZeroVector));
    TestFalse(TEXT("IsPixelInsideImage on an invalid grid is false"),
        IsPixelInsideImage(Empty, FVector2D(0.0, 0.0)));

    FTileGrid Grid;
    TestFalse(TEXT("zero columns refused"),
        MakeTileGrid(MakeWorldExtent(0.0, 0.0, 100.0, 100.0), TopDownXRightYDown(), 0, 1, 64, 64, Grid, Err));
    TestTrue(TEXT("column refusal is typed"), Err.Contains(TEXT("INVALID_TILE_COUNT")));
    TestFalse(TEXT("zero tile pixels refused"),
        MakeTileGrid(MakeWorldExtent(0.0, 0.0, 100.0, 100.0), TopDownXRightYDown(), 1, 1, 0, 64, Grid, Err));
    TestTrue(TEXT("tile size refusal is typed"), Err.Contains(TEXT("INVALID_TILE_SIZE")));
    TestFalse(TEXT("zero-span extent refused"),
        MakeTileGrid(MakeWorldExtent(5.0, 0.0, 5.0, 100.0), TopDownXRightYDown(), 1, 1, 64, 64, Grid, Err));
    TestTrue(TEXT("extent refusal is typed"), Err.Contains(TEXT("INVALID_WORLD_EXTENT")));

    // MakeWorldExtent normalises a reversed corner pair rather than producing a negative span.
    const FWorldExtent2D Reversed = MakeWorldExtent(100.0, 200.0, -100.0, -200.0);
    TestTrue(TEXT("reversed corners normalise"), Reversed.IsValid());
    TestTrue(TEXT("reversed corners give the same box"),
        PwTileGridNearly(Reversed.Min.X, -100.0) && PwTileGridNearly(Reversed.Max.Y, 200.0));

    // Bounds extraction drops the depth axis and keeps the in-plane pair.
    const FWorldExtent2D FromBounds = MakeWorldExtentFromBounds(
        FVector(-100.0, -200.0, -300.0), FVector(100.0, 200.0, 300.0), TopDownXRightYDown());
    TestTrue(TEXT("bounds extraction uses X across and Y down"),
        PwTileGridNearly(FromBounds.SpanAcross(), 200.0) && PwTileGridNearly(FromBounds.SpanDown(), 400.0));

    const FWorldExtent2D FromBoundsSide = MakeWorldExtentFromBounds(
        FVector(-100.0, -200.0, -300.0), FVector(100.0, 200.0, 300.0), SideXRightZUp());
    TestTrue(TEXT("side bounds extraction uses X across and Z down"),
        PwTileGridNearly(FromBoundsSide.SpanAcross(), 200.0) && PwTileGridNearly(FromBoundsSide.SpanDown(), 600.0));

    return true;
}
