// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/TileGridUtils.h"

#include "Math/RotationMatrix.h"
#include "Math/UnrealMathUtility.h"

namespace PinWrightTileGrid
{
    namespace
    {
        // Relative slack absorbed before a ceil, so a span that divides EXACTLY into whole tiles
        // does not gain a spurious extra tile from the last bit of the division.
        constexpr double CeilEpsilon = 1.0e-9;

        // Classify a unit direction onto a cardinal world axis. Both conditions must hold: the
        // dominant component is within Tolerance of +/-1 AND the other two are within Tolerance
        // of 0. A single dominance test would accept a 30-degree tilt.
        bool ClassifyCardinal(const FVector& Direction, double Tolerance, EWorldAxis& OutAxis, bool& bOutPositive)
        {
            const double Components[3] = { Direction.X, Direction.Y, Direction.Z };
            int32 Dominant = INDEX_NONE;
            for (int32 I = 0; I < 3; ++I)
            {
                if (FMath::Abs(Components[I]) >= 1.0 - Tolerance)
                {
                    if (Dominant != INDEX_NONE)
                    {
                        return false; // two near-unit components: not a direction at all
                    }
                    Dominant = I;
                }
            }
            if (Dominant == INDEX_NONE)
            {
                return false;
            }
            for (int32 I = 0; I < 3; ++I)
            {
                if (I != Dominant && FMath::Abs(Components[I]) > Tolerance)
                {
                    return false;
                }
            }
            OutAxis = static_cast<EWorldAxis>(Dominant);
            bOutPositive = Components[Dominant] > 0.0;
            return true;
        }
    }

    double GetAxisValue(const FVector& WorldPoint, EWorldAxis Axis)
    {
        switch (Axis)
        {
            case EWorldAxis::X: return WorldPoint.X;
            case EWorldAxis::Y: return WorldPoint.Y;
            case EWorldAxis::Z: return WorldPoint.Z;
            default: return 0.0;
        }
    }

    void SetAxisValue(FVector& WorldPoint, EWorldAxis Axis, double Value)
    {
        switch (Axis)
        {
            case EWorldAxis::X: WorldPoint.X = Value; break;
            case EWorldAxis::Y: WorldPoint.Y = Value; break;
            case EWorldAxis::Z: WorldPoint.Z = Value; break;
            default: break;
        }
    }

    const TCHAR* AxisName(EWorldAxis Axis)
    {
        switch (Axis)
        {
            case EWorldAxis::X: return TEXT("X");
            case EWorldAxis::Y: return TEXT("Y");
            case EWorldAxis::Z: return TEXT("Z");
            default: return TEXT("?");
        }
    }

    EWorldAxis FScreenAxisMapping::DepthAxis() const
    {
        if (!IsValid())
        {
            return EWorldAxis::X;
        }
        for (int32 I = 0; I < 3; ++I)
        {
            const EWorldAxis Candidate = static_cast<EWorldAxis>(I);
            if (Candidate != ScreenXAxis && Candidate != ScreenYAxis)
            {
                return Candidate;
            }
        }
        return EWorldAxis::X;
    }

    FScreenAxisMapping TopDownXUpYRight()
    {
        FScreenAxisMapping Mapping;
        Mapping.ScreenXAxis = EWorldAxis::Y;
        Mapping.bScreenXPositive = true;   // +Y right
        Mapping.ScreenYAxis = EWorldAxis::X;
        Mapping.bScreenYPositive = false;  // +X up
        return Mapping;
    }

    FScreenAxisMapping TopDownXRightYDown()
    {
        FScreenAxisMapping Mapping;
        Mapping.ScreenXAxis = EWorldAxis::X;
        Mapping.bScreenXPositive = true;   // +X right
        Mapping.ScreenYAxis = EWorldAxis::Y;
        Mapping.bScreenYPositive = true;   // +Y down
        return Mapping;
    }

    FScreenAxisMapping FrontYRightZUp()
    {
        FScreenAxisMapping Mapping;
        Mapping.ScreenXAxis = EWorldAxis::Y;
        Mapping.bScreenXPositive = true;
        Mapping.ScreenYAxis = EWorldAxis::Z;
        Mapping.bScreenYPositive = false;  // +Z up
        return Mapping;
    }

    FScreenAxisMapping SideXRightZUp()
    {
        FScreenAxisMapping Mapping;
        Mapping.ScreenXAxis = EWorldAxis::X;
        Mapping.bScreenXPositive = true;
        Mapping.ScreenYAxis = EWorldAxis::Z;
        Mapping.bScreenYPositive = false;
        return Mapping;
    }

    bool TryMakeAxisMappingFromEffectiveRotation(const FRotator& EffectiveRotation,
        FScreenAxisMapping& OutMapping, FString& OutErr, double Tolerance)
    {
        const FRotationMatrix Basis(EffectiveRotation);
        // Screen right is the rotation's Y basis; screen DOWN is the negated Z basis (screen Y
        // grows downward under the top-left pixel convention).
        const FVector ScreenRight = Basis.GetUnitAxis(EAxis::Y);
        const FVector ScreenDown = -Basis.GetUnitAxis(EAxis::Z);

        FScreenAxisMapping Mapping;
        if (!ClassifyCardinal(ScreenRight, Tolerance, Mapping.ScreenXAxis, Mapping.bScreenXPositive))
        {
            OutErr = FString::Printf(
                TEXT("NON_CARDINAL_ROTATION: screen-right vector (%s) for rotation (P=%.4f Y=%.4f R=%.4f) is not a world axis"),
                *ScreenRight.ToString(), EffectiveRotation.Pitch, EffectiveRotation.Yaw, EffectiveRotation.Roll);
            return false;
        }
        if (!ClassifyCardinal(ScreenDown, Tolerance, Mapping.ScreenYAxis, Mapping.bScreenYPositive))
        {
            OutErr = FString::Printf(
                TEXT("NON_CARDINAL_ROTATION: screen-down vector (%s) for rotation (P=%.4f Y=%.4f R=%.4f) is not a world axis"),
                *ScreenDown.ToString(), EffectiveRotation.Pitch, EffectiveRotation.Yaw, EffectiveRotation.Roll);
            return false;
        }
        if (!Mapping.IsValid())
        {
            OutErr = FString::Printf(TEXT("DEGENERATE_AXIS_MAPPING: screen X and screen Y both resolved to world %s"),
                AxisName(Mapping.ScreenXAxis));
            return false;
        }

        OutMapping = Mapping;
        OutErr.Reset();
        return true;
    }

    FVector CameraForwardForAxisMapping(const FScreenAxisMapping& Mapping)
    {
        if (!Mapping.IsValid())
        {
            return FVector::ZeroVector;
        }
        FVector Right = FVector::ZeroVector;
        SetAxisValue(Right, Mapping.ScreenXAxis, Mapping.bScreenXPositive ? 1.0 : -1.0);
        // Screen Y grows DOWNWARD, so the screen UP basis is the negation of the screen-Y
        // direction. Getting this negation wrong flips the frame vertically, which is the exact
        // failure the round-trip check below exists to catch.
        FVector Up = FVector::ZeroVector;
        SetAxisValue(Up, Mapping.ScreenYAxis, Mapping.bScreenYPositive ? -1.0 : 1.0);
        return FVector::CrossProduct(Right, Up);
    }

    bool TryMakeCameraRotationForAxisMapping(const FScreenAxisMapping& Mapping,
        FRotator& OutRotation, FString& OutErr)
    {
        if (!Mapping.IsValid())
        {
            OutErr = FString::Printf(TEXT("DEGENERATE_AXIS_MAPPING: screen X and screen Y both map to world %s"),
                AxisName(Mapping.ScreenXAxis));
            return false;
        }

        FVector Right = FVector::ZeroVector;
        SetAxisValue(Right, Mapping.ScreenXAxis, Mapping.bScreenXPositive ? 1.0 : -1.0);
        FVector Up = FVector::ZeroVector;
        SetAxisValue(Up, Mapping.ScreenYAxis, Mapping.bScreenYPositive ? -1.0 : 1.0);
        const FVector Forward = FVector::CrossProduct(Right, Up);

        // FMatrix(InX, InY, InZ, InW): X is forward, Y is right, Z is up - the same basis
        // FRotationMatrix produces, so Rotator() inverts it.
        const FRotator Candidate = FMatrix(Forward, Right, Up, FVector::ZeroVector).Rotator();

        FScreenAxisMapping RoundTrip;
        FString RoundTripErr;
        if (!TryMakeAxisMappingFromEffectiveRotation(Candidate, RoundTrip, RoundTripErr))
        {
            OutErr = FString::Printf(
                TEXT("AXIS_MAPPING_ROUND_TRIP_FAILED: rotation (P=%.4f Y=%.4f R=%.4f) built for screen X %s%s / screen Y %s%s does not classify back to any mapping: %s"),
                Candidate.Pitch, Candidate.Yaw, Candidate.Roll,
                Mapping.bScreenXPositive ? TEXT("+") : TEXT("-"), AxisName(Mapping.ScreenXAxis),
                Mapping.bScreenYPositive ? TEXT("+") : TEXT("-"), AxisName(Mapping.ScreenYAxis),
                *RoundTripErr);
            return false;
        }
        if (RoundTrip != Mapping)
        {
            OutErr = FString::Printf(
                TEXT("AXIS_MAPPING_ROUND_TRIP_MISMATCH: rotation (P=%.4f Y=%.4f R=%.4f) renders screen X %s%s / screen Y %s%s, but screen X %s%s / screen Y %s%s was requested"),
                Candidate.Pitch, Candidate.Yaw, Candidate.Roll,
                RoundTrip.bScreenXPositive ? TEXT("+") : TEXT("-"), AxisName(RoundTrip.ScreenXAxis),
                RoundTrip.bScreenYPositive ? TEXT("+") : TEXT("-"), AxisName(RoundTrip.ScreenYAxis),
                Mapping.bScreenXPositive ? TEXT("+") : TEXT("-"), AxisName(Mapping.ScreenXAxis),
                Mapping.bScreenYPositive ? TEXT("+") : TEXT("-"), AxisName(Mapping.ScreenYAxis));
            return false;
        }

        OutRotation = Candidate;
        OutErr.Reset();
        return true;
    }

    FWorldExtent2D MakeWorldExtent(double MinAcross, double MinDown, double MaxAcross, double MaxDown)
    {
        FWorldExtent2D Extent;
        Extent.Min = FVector2D(FMath::Min(MinAcross, MaxAcross), FMath::Min(MinDown, MaxDown));
        Extent.Max = FVector2D(FMath::Max(MinAcross, MaxAcross), FMath::Max(MinDown, MaxDown));
        return Extent;
    }

    FWorldExtent2D MakeWorldExtentFromBounds(const FVector& BoundsMin, const FVector& BoundsMax,
        const FScreenAxisMapping& Mapping)
    {
        return MakeWorldExtent(
            GetAxisValue(BoundsMin, Mapping.ScreenXAxis),
            GetAxisValue(BoundsMin, Mapping.ScreenYAxis),
            GetAxisValue(BoundsMax, Mapping.ScreenXAxis),
            GetAxisValue(BoundsMax, Mapping.ScreenYAxis));
    }

    double FTileGrid::WorldUnitsPerPixelAcross() const
    {
        const int32 ImgW = ImageWidth();
        return (ImgW > 0) ? World.SpanAcross() / static_cast<double>(ImgW) : 0.0;
    }

    double FTileGrid::WorldUnitsPerPixelDown() const
    {
        const int32 ImgH = ImageHeight();
        return (ImgH > 0) ? World.SpanDown() / static_cast<double>(ImgH) : 0.0;
    }

    bool FTileGrid::HasSquarePixels(double RelativeTolerance) const
    {
        const double Across = WorldUnitsPerPixelAcross();
        const double Down = WorldUnitsPerPixelDown();
        if (Across <= 0.0 || Down <= 0.0)
        {
            return false;
        }
        const double Larger = FMath::Max(Across, Down);
        return FMath::Abs(Across - Down) <= RelativeTolerance * Larger;
    }

    bool FTileGrid::IsValid() const
    {
        FString Unused;
        return ValidateTileGrid(*this, Unused);
    }

    bool ValidateTileGrid(const FTileGrid& Grid, FString& OutErr)
    {
        if (!Grid.Axes.IsValid())
        {
            OutErr = FString::Printf(TEXT("DEGENERATE_AXIS_MAPPING: screen X and screen Y both map to world %s"),
                AxisName(Grid.Axes.ScreenXAxis));
            return false;
        }
        if (Grid.Cols <= 0 || Grid.Rows <= 0)
        {
            OutErr = FString::Printf(TEXT("INVALID_TILE_COUNT: cols=%d rows=%d, both must be > 0"), Grid.Cols, Grid.Rows);
            return false;
        }
        if (Grid.TilePixelWidth <= 0 || Grid.TilePixelHeight <= 0)
        {
            OutErr = FString::Printf(TEXT("INVALID_TILE_SIZE: tile %dx%d px, both must be > 0"),
                Grid.TilePixelWidth, Grid.TilePixelHeight);
            return false;
        }
        if (!Grid.World.IsValid())
        {
            OutErr = FString::Printf(TEXT("INVALID_WORLD_EXTENT: span %.4f x %.4f cm, both must be > 0"),
                Grid.World.SpanAcross(), Grid.World.SpanDown());
            return false;
        }
        OutErr.Reset();
        return true;
    }

    bool MakeTileGrid(const FWorldExtent2D& World, const FScreenAxisMapping& Mapping,
        int32 Cols, int32 Rows, int32 TilePixelWidth, int32 TilePixelHeight,
        FTileGrid& OutGrid, FString& OutErr)
    {
        FTileGrid Grid;
        Grid.World = World;
        Grid.Axes = Mapping;
        Grid.Cols = Cols;
        Grid.Rows = Rows;
        Grid.TilePixelWidth = TilePixelWidth;
        Grid.TilePixelHeight = TilePixelHeight;

        if (!ValidateTileGrid(Grid, OutErr))
        {
            return false;
        }
        OutGrid = Grid;
        return true;
    }

    bool PlanTileGrid(const FWorldExtent2D& RequestedWorld, const FScreenAxisMapping& Mapping,
        int32 TilePixelWidth, int32 TilePixelHeight, double TargetWorldUnitsPerPixel,
        int32 MaxTiles, FTileGridPlan& OutPlan, FString& OutErr)
    {
        if (!Mapping.IsValid())
        {
            OutErr = FString::Printf(TEXT("DEGENERATE_AXIS_MAPPING: screen X and screen Y both map to world %s"),
                AxisName(Mapping.ScreenXAxis));
            return false;
        }
        if (!RequestedWorld.IsValid())
        {
            OutErr = FString::Printf(TEXT("INVALID_WORLD_EXTENT: span %.4f x %.4f cm, both must be > 0"),
                RequestedWorld.SpanAcross(), RequestedWorld.SpanDown());
            return false;
        }
        if (TilePixelWidth <= 0 || TilePixelHeight <= 0)
        {
            OutErr = FString::Printf(TEXT("INVALID_TILE_SIZE: tile %dx%d px, both must be > 0"),
                TilePixelWidth, TilePixelHeight);
            return false;
        }
        if (!(TargetWorldUnitsPerPixel > 0.0))
        {
            OutErr = FString::Printf(TEXT("INVALID_RESOLUTION: worldUnitsPerPixel %.6f must be > 0"),
                TargetWorldUnitsPerPixel);
            return false;
        }

        const double TileWorldAcross = TargetWorldUnitsPerPixel * static_cast<double>(TilePixelWidth);
        const double TileWorldDown = TargetWorldUnitsPerPixel * static_cast<double>(TilePixelHeight);
        const double ExactCols = RequestedWorld.SpanAcross() / TileWorldAcross;
        const double ExactRows = RequestedWorld.SpanDown() / TileWorldDown;
        if (ExactCols > static_cast<double>(MaxTilesPerAxis) || ExactRows > static_cast<double>(MaxTilesPerAxis))
        {
            OutErr = FString::Printf(
                TEXT("GRID_TOO_LARGE: %.1f x %.1f tiles needed at %.4f cm/px, per-axis ceiling is %d"),
                ExactCols, ExactRows, TargetWorldUnitsPerPixel, MaxTilesPerAxis);
            return false;
        }

        const int32 Cols = FMath::Max(1, FMath::CeilToInt(ExactCols - CeilEpsilon));
        const int32 Rows = FMath::Max(1, FMath::CeilToInt(ExactRows - CeilEpsilon));
        const int64 Tiles = static_cast<int64>(Cols) * static_cast<int64>(Rows);
        if (MaxTiles > 0 && Tiles > static_cast<int64>(MaxTiles))
        {
            OutErr = FString::Printf(TEXT("TILE_BUDGET_EXCEEDED: %lld tiles (%d x %d) needed at %.4f cm/px, maxTiles is %d"),
                Tiles, Cols, Rows, TargetWorldUnitsPerPixel, MaxTiles);
            return false;
        }

        const FVector2D Centre = RequestedWorld.Centre();
        const double HalfAcross = 0.5 * TileWorldAcross * static_cast<double>(Cols);
        const double HalfDown = 0.5 * TileWorldDown * static_cast<double>(Rows);

        FTileGridPlan Plan;
        Plan.RequestedWorld = RequestedWorld;
        Plan.WorldUnitsPerPixel = TargetWorldUnitsPerPixel;
        const FWorldExtent2D Covered = MakeWorldExtent(
            Centre.X - HalfAcross, Centre.Y - HalfDown, Centre.X + HalfAcross, Centre.Y + HalfDown);
        Plan.bExtentExpanded =
            Covered.SpanAcross() > RequestedWorld.SpanAcross() * (1.0 + CeilEpsilon) ||
            Covered.SpanDown() > RequestedWorld.SpanDown() * (1.0 + CeilEpsilon);

        if (!MakeTileGrid(Covered, Mapping, Cols, Rows, TilePixelWidth, TilePixelHeight, Plan.Grid, OutErr))
        {
            return false;
        }

        OutPlan = Plan;
        OutErr.Reset();
        return true;
    }

    FVector2D WorldToPixel(const FTileGrid& Grid, const FVector& WorldPoint)
    {
        FString Unused;
        if (!ValidateTileGrid(Grid, Unused))
        {
            return FVector2D::ZeroVector;
        }

        const double Across = GetAxisValue(WorldPoint, Grid.Axes.ScreenXAxis);
        const double Down = GetAxisValue(WorldPoint, Grid.Axes.ScreenYAxis);

        const double U = Grid.Axes.bScreenXPositive
            ? (Across - Grid.World.Min.X) / Grid.World.SpanAcross()
            : (Grid.World.Max.X - Across) / Grid.World.SpanAcross();
        const double V = Grid.Axes.bScreenYPositive
            ? (Down - Grid.World.Min.Y) / Grid.World.SpanDown()
            : (Grid.World.Max.Y - Down) / Grid.World.SpanDown();

        return FVector2D(U * static_cast<double>(Grid.ImageWidth()), V * static_cast<double>(Grid.ImageHeight()));
    }

    FVector PixelToWorld(const FTileGrid& Grid, const FVector2D& Pixel, double DepthCoordinate)
    {
        FVector World = FVector::ZeroVector;
        FString Unused;
        if (!ValidateTileGrid(Grid, Unused))
        {
            return World;
        }

        const double U = Pixel.X / static_cast<double>(Grid.ImageWidth());
        const double V = Pixel.Y / static_cast<double>(Grid.ImageHeight());

        const double Across = Grid.Axes.bScreenXPositive
            ? Grid.World.Min.X + U * Grid.World.SpanAcross()
            : Grid.World.Max.X - U * Grid.World.SpanAcross();
        const double Down = Grid.Axes.bScreenYPositive
            ? Grid.World.Min.Y + V * Grid.World.SpanDown()
            : Grid.World.Max.Y - V * Grid.World.SpanDown();

        SetAxisValue(World, Grid.Axes.ScreenXAxis, Across);
        SetAxisValue(World, Grid.Axes.ScreenYAxis, Down);
        SetAxisValue(World, Grid.Axes.DepthAxis(), DepthCoordinate);
        return World;
    }

    FVector PixelCentreToWorld(const FTileGrid& Grid, int32 PixelX, int32 PixelY, double DepthCoordinate)
    {
        return PixelToWorld(Grid, FVector2D(static_cast<double>(PixelX) + 0.5, static_cast<double>(PixelY) + 0.5),
            DepthCoordinate);
    }

    bool IsPixelInsideImage(const FTileGrid& Grid, const FVector2D& Pixel)
    {
        FString Unused;
        if (!ValidateTileGrid(Grid, Unused))
        {
            return false;
        }
        return Pixel.X >= 0.0 && Pixel.Y >= 0.0 &&
            Pixel.X <= static_cast<double>(Grid.ImageWidth()) &&
            Pixel.Y <= static_cast<double>(Grid.ImageHeight());
    }

    bool PixelToTilePixel(const FTileGrid& Grid, const FVector2D& Pixel, FTilePixel& Out)
    {
        if (!IsPixelInsideImage(Grid, Pixel))
        {
            return false;
        }

        // Half-open per tile: floor puts a seam coordinate in the tile to its right / below. The
        // far image edge has no such tile, so it clamps back into the last one and keeps
        // PixelInTile == the tile size rather than wrapping to 0 of a tile that does not exist.
        int32 Col = FMath::FloorToInt(Pixel.X / static_cast<double>(Grid.TilePixelWidth));
        int32 Row = FMath::FloorToInt(Pixel.Y / static_cast<double>(Grid.TilePixelHeight));
        Col = FMath::Clamp(Col, 0, Grid.Cols - 1);
        Row = FMath::Clamp(Row, 0, Grid.Rows - 1);

        Out.Tile = FTileIndex(Col, Row);
        Out.PixelInTile = FVector2D(
            Pixel.X - static_cast<double>(Col) * static_cast<double>(Grid.TilePixelWidth),
            Pixel.Y - static_cast<double>(Row) * static_cast<double>(Grid.TilePixelHeight));
        return true;
    }

    FVector2D TilePixelToPixel(const FTileGrid& Grid, const FTileIndex& Tile, const FVector2D& PixelInTile)
    {
        return FVector2D(
            static_cast<double>(Tile.Col) * static_cast<double>(Grid.TilePixelWidth) + PixelInTile.X,
            static_cast<double>(Tile.Row) * static_cast<double>(Grid.TilePixelHeight) + PixelInTile.Y);
    }

    bool WorldToTilePixel(const FTileGrid& Grid, const FVector& WorldPoint, FTilePixel& Out)
    {
        return PixelToTilePixel(Grid, WorldToPixel(Grid, WorldPoint), Out);
    }

    FVector TilePixelToWorld(const FTileGrid& Grid, const FTileIndex& Tile, const FVector2D& PixelInTile,
        double DepthCoordinate)
    {
        return PixelToWorld(Grid, TilePixelToPixel(Grid, Tile, PixelInTile), DepthCoordinate);
    }

    bool TileWorldExtent(const FTileGrid& Grid, const FTileIndex& Tile, FWorldExtent2D& OutExtent)
    {
        FString Unused;
        if (!ValidateTileGrid(Grid, Unused))
        {
            return false;
        }
        if (Tile.Col < 0 || Tile.Row < 0 || Tile.Col >= Grid.Cols || Tile.Row >= Grid.Rows)
        {
            return false;
        }

        // Derived through PixelToWorld rather than re-deriving the affine map, so the tile extent
        // cannot drift from what WorldToPixel reports for the same corners.
        const FVector TopLeft = PixelToWorld(Grid, TilePixelToPixel(Grid, Tile, FVector2D::ZeroVector), 0.0);
        const FVector BottomRight = PixelToWorld(Grid,
            TilePixelToPixel(Grid, Tile,
                FVector2D(static_cast<double>(Grid.TilePixelWidth), static_cast<double>(Grid.TilePixelHeight))),
            0.0);

        OutExtent = MakeWorldExtent(
            GetAxisValue(TopLeft, Grid.Axes.ScreenXAxis),
            GetAxisValue(TopLeft, Grid.Axes.ScreenYAxis),
            GetAxisValue(BottomRight, Grid.Axes.ScreenXAxis),
            GetAxisValue(BottomRight, Grid.Axes.ScreenYAxis));
        return true;
    }

    bool ComputeTileCapturePlan(const FTileGrid& Grid, const FTileIndex& Tile, double DepthCoordinate,
        FTileCapturePlan& OutPlan, FString& OutErr)
    {
        if (!ValidateTileGrid(Grid, OutErr))
        {
            return false;
        }
        if (Tile.Col < 0 || Tile.Row < 0 || Tile.Col >= Grid.Cols || Tile.Row >= Grid.Rows)
        {
            OutErr = FString::Printf(TEXT("TILE_OUT_OF_RANGE: tile (%d,%d) is outside a %d x %d grid"),
                Tile.Col, Tile.Row, Grid.Cols, Grid.Rows);
            return false;
        }
        if (!Grid.HasSquarePixels())
        {
            OutErr = FString::Printf(
                TEXT("NON_SQUARE_PIXELS: %.6f cm/px across vs %.6f cm/px down; an orthographic frame has no orthoHeight, so its vertical span is forced to orthoWidth * height / width"),
                Grid.WorldUnitsPerPixelAcross(), Grid.WorldUnitsPerPixelDown());
            return false;
        }

        FWorldExtent2D TileExtent;
        if (!TileWorldExtent(Grid, Tile, TileExtent))
        {
            OutErr = FString::Printf(TEXT("TILE_OUT_OF_RANGE: tile (%d,%d) is outside a %d x %d grid"),
                Tile.Col, Tile.Row, Grid.Cols, Grid.Rows);
            return false;
        }

        const FVector2D Centre = TileExtent.Centre();
        FVector CameraLocation = FVector::ZeroVector;
        SetAxisValue(CameraLocation, Grid.Axes.ScreenXAxis, Centre.X);
        SetAxisValue(CameraLocation, Grid.Axes.ScreenYAxis, Centre.Y);
        SetAxisValue(CameraLocation, Grid.Axes.DepthAxis(), DepthCoordinate);

        OutPlan.Tile = Tile;
        OutPlan.World = TileExtent;
        OutPlan.CameraLocation = CameraLocation;
        OutPlan.OrthoWidth = static_cast<float>(TileExtent.SpanAcross());
        OutPlan.PixelWidth = Grid.TilePixelWidth;
        OutPlan.PixelHeight = Grid.TilePixelHeight;
        OutErr.Reset();
        return true;
    }

    double SnapToWorldGrid(double Value, double GridSize)
    {
        if (!(GridSize > 0.0))
        {
            return Value;
        }
        return FMath::FloorToDouble(Value / GridSize + 0.5) * GridSize;
    }

    FVector2D SnapToWorldGrid(const FVector2D& Value, double GridSize)
    {
        return FVector2D(SnapToWorldGrid(Value.X, GridSize), SnapToWorldGrid(Value.Y, GridSize));
    }

    FVector SnapToWorldGrid(const FVector& Value, double GridSize)
    {
        return FVector(SnapToWorldGrid(Value.X, GridSize), SnapToWorldGrid(Value.Y, GridSize),
            SnapToWorldGrid(Value.Z, GridSize));
    }
}
