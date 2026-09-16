// Copyright (c) 2026 Alexander Penkin. MIT License.

// Implementation of the largest-flat-region measurement. Pure with respect to the editor: no
// UObject, no world, no viewport, no RHI. It reads a pixel buffer and returns numbers, which is
// what makes it assertable against a synthetic frame with no GPU in the process.
#include "Handlers/Render/FlatRegionStats.h"

// EAllowShrinking (used by the flood-fill's Pop below) is an engine type only from UE 5.4;
// on 5.3 the compat header back-fills it.
#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"

namespace
{
    // The SAME Rec.709 coefficients and the same 0..255 quantisation
    // PinWrightRenderCapture::CalculateCaptureImageStats uses for its tone histogram. Spelled out
    // rather than shared through a header because that function consumes the whole buffer and
    // returns aggregates; what is needed here is one pixel at a time. If one moves, move both.
    FORCEINLINE int32 LuminanceLevel(const FColor& Color)
    {
        const double Luminance =
            (0.2126 * static_cast<double>(Color.R) +
             0.7152 * static_cast<double>(Color.G) +
             0.0722 * static_cast<double>(Color.B)) / 255.0;
        return FMath::Clamp(FMath::RoundToInt32(Luminance * 255.0), 0, 255);
    }

    // Half-open block bounds, so consecutive blocks tile the axis exactly and no pixel is counted
    // twice or missed at a boundary that does not divide evenly.
    FORCEINLINE int32 BlockEdge(int32 BlockIndex, int32 BlockCount, int32 AxisLength)
    {
        return static_cast<int32>((static_cast<int64>(BlockIndex) * AxisLength) / BlockCount);
    }

    struct FLuminanceBlock
    {
        int32 Level = 0;
        bool bFlat = false;
    };
}

PinWrightFlatRegion::FFrameDifferenceStats PinWrightFlatRegion::MeasureFrameDifference(
    TConstArrayView<FColor> Reference, TConstArrayView<FColor> Subject, int32 ChannelThreshold)
{
    FFrameDifferenceStats Stats;
    if (Reference.Num() == 0 || Reference.Num() != Subject.Num())
    {
        return Stats;
    }

    const int32 Threshold = FMath::Max(ChannelThreshold, 0);
    int64 TotalChannelDelta = 0;
    int64 ChangedPixels = 0;
    for (int32 Index = 0; Index < Reference.Num(); ++Index)
    {
        const FColor& Before = Reference[Index];
        const FColor& After = Subject[Index];
        const int32 DeltaR = FMath::Abs(static_cast<int32>(After.R) - static_cast<int32>(Before.R));
        const int32 DeltaG = FMath::Abs(static_cast<int32>(After.G) - static_cast<int32>(Before.G));
        const int32 DeltaB = FMath::Abs(static_cast<int32>(After.B) - static_cast<int32>(Before.B));
        TotalChannelDelta += DeltaR + DeltaG + DeltaB;
        Stats.MaxDelta = FMath::Max(Stats.MaxDelta, FMath::Max3(DeltaR, DeltaG, DeltaB));
        ChangedPixels += (DeltaR > Threshold || DeltaG > Threshold || DeltaB > Threshold) ? 1 : 0;
    }

    Stats.MeanAbsDelta = static_cast<double>(TotalChannelDelta)
        / static_cast<double>(Reference.Num() * 3);
    Stats.ChangedPixelFraction = static_cast<double>(ChangedPixels)
        / static_cast<double>(Reference.Num());
    Stats.bMeasured = true;
    return Stats;
}

PinWrightFlatRegion::FFlatRegionStats PinWrightFlatRegion::MeasureLargestFlatRegion(
    TConstArrayView<FColor> ColorData, int32 Width, int32 Height)
{
    FFlatRegionStats Stats;

    if (Width <= 0 || Height <= 0)
    {
        Stats.NotMeasuredReason = FString::Printf(
            TEXT("the frame reported no dimensions (%d x %d)"), Width, Height);
        return Stats;
    }
    const int64 PixelCount = static_cast<int64>(Width) * static_cast<int64>(Height);
    if (static_cast<int64>(ColorData.Num()) < PixelCount)
    {
        Stats.NotMeasuredReason = FString::Printf(
            TEXT("the pixel buffer holds %d entries, short of the %lld a %d x %d frame needs"),
            ColorData.Num(), static_cast<long long>(PixelCount), Width, Height);
        return Stats;
    }

    // Fewer blocks on a small frame rather than degenerate ones: at one pixel per block every
    // block is trivially flat and the whole frame would read as a single void.
    const int32 BlocksX = FMath::Clamp(FMath::Min(MaxBlocksPerAxis, Width / MinBlockEdgePixels),
        1, MaxBlocksPerAxis);
    const int32 BlocksY = FMath::Clamp(FMath::Min(MaxBlocksPerAxis, Height / MinBlockEdgePixels),
        1, MaxBlocksPerAxis);
    if (BlocksX < 2 || BlocksY < 2)
    {
        // A 1xN partition makes "the largest connected flat region" either the whole frame or a
        // stripe of it, neither of which is a statement about spatial structure.
        Stats.NotMeasuredReason = FString::Printf(
            TEXT("a %d x %d frame partitions into %d x %d blocks of at least %d px, too few to "
                 "locate a region in"),
            Width, Height, BlocksX, BlocksY, MinBlockEdgePixels);
        return Stats;
    }

    // Pass 1: per-block luminance variance. Min/max rejected the ticket's real shallow-gradient
    // void because sparse dither/outliers widened otherwise uniform blocks. Standard deviation
    // measures the local field instead, still in one cache-friendly read of every pixel.
    TArray<FLuminanceBlock> Blocks;
    Blocks.SetNum(BlocksX * BlocksY);
    for (int32 By = 0; By < BlocksY; ++By)
    {
        const int32 Y0 = BlockEdge(By, BlocksY, Height);
        const int32 Y1 = BlockEdge(By + 1, BlocksY, Height);
        for (int32 Bx = 0; Bx < BlocksX; ++Bx)
        {
            const int32 X0 = BlockEdge(Bx, BlocksX, Width);
            const int32 X1 = BlockEdge(Bx + 1, BlocksX, Width);
            int64 LevelSum = 0;
            int64 LevelSquaredSum = 0;
            int64 BlockPixelCount = 0;
            for (int32 Y = Y0; Y < Y1; ++Y)
            {
                const int64 RowStart = static_cast<int64>(Y) * static_cast<int64>(Width);
                for (int32 X = X0; X < X1; ++X)
                {
                    const int32 Level = LuminanceLevel(ColorData[static_cast<int32>(RowStart + X)]);
                    LevelSum += Level;
                    LevelSquaredSum += static_cast<int64>(Level) * static_cast<int64>(Level);
                    ++BlockPixelCount;
                }
            }
            FLuminanceBlock& Block = Blocks[By * BlocksX + Bx];
            if (BlockPixelCount == 0)
            {
                // An empty block (possible only when an axis rounds a block to zero width); it
                // carries no pixels, so it carries no evidence either way.
                Block.bFlat = false;
                continue;
            }
            const double MeanLevel =
                static_cast<double>(LevelSum) / static_cast<double>(BlockPixelCount);
            const double Variance = FMath::Max(0.0,
                static_cast<double>(LevelSquaredSum) / static_cast<double>(BlockPixelCount)
                    - MeanLevel * MeanLevel);
            Block.bFlat = Variance <= FlatBlockStdDevLevels * FlatBlockStdDevLevels;
            Block.Level = FMath::Clamp(FMath::RoundToInt32(MeanLevel), 0, 255);
        }
    }

    Stats.BlocksX = BlocksX;
    Stats.BlocksY = BlocksY;
    Stats.BlockCount = static_cast<int64>(BlocksX) * static_cast<int64>(BlocksY);
    for (const FLuminanceBlock& Block : Blocks)
    {
        Stats.FlatBlockCount += Block.bFlat ? 1 : 0;
    }
    Stats.FlatBlockFraction = static_cast<double>(Stats.FlatBlockCount)
        / static_cast<double>(Stats.BlockCount);

    // Pass 2: grow 4-connected regions of flat blocks, each anchored to its SEED's level so a
    // gradient cannot be walked one level at a time into a single region (see the header).
    TArray<bool> Visited;
    Visited.Init(false, Blocks.Num());
    TArray<int32> Frontier;
    int64 BestPixels = 0;
    for (int32 SeedIndex = 0; SeedIndex < Blocks.Num(); ++SeedIndex)
    {
        if (Visited[SeedIndex] || !Blocks[SeedIndex].bFlat)
        {
            continue;
        }
        const int32 SeedLevel = Blocks[SeedIndex].Level;
        int64 RegionPixels = 0;
        int32 MinBx = BlocksX;
        int32 MinBy = BlocksY;
        int32 MaxBx = -1;
        int32 MaxBy = -1;

        Frontier.Reset();
        Frontier.Add(SeedIndex);
        Visited[SeedIndex] = true;
        while (Frontier.Num() > 0)
        {
            const int32 Index = Frontier.Pop(EAllowShrinking::No);
            const int32 Bx = Index % BlocksX;
            const int32 By = Index / BlocksX;
            const int32 X0 = BlockEdge(Bx, BlocksX, Width);
            const int32 X1 = BlockEdge(Bx + 1, BlocksX, Width);
            const int32 Y0 = BlockEdge(By, BlocksY, Height);
            const int32 Y1 = BlockEdge(By + 1, BlocksY, Height);
            RegionPixels += static_cast<int64>(X1 - X0) * static_cast<int64>(Y1 - Y0);
            MinBx = FMath::Min(MinBx, Bx);
            MinBy = FMath::Min(MinBy, By);
            MaxBx = FMath::Max(MaxBx, Bx);
            MaxBy = FMath::Max(MaxBy, By);

            const int32 NeighbourX[4] = { Bx - 1, Bx + 1, Bx, Bx };
            const int32 NeighbourY[4] = { By, By, By - 1, By + 1 };
            for (int32 N = 0; N < 4; ++N)
            {
                if (NeighbourX[N] < 0 || NeighbourX[N] >= BlocksX
                    || NeighbourY[N] < 0 || NeighbourY[N] >= BlocksY)
                {
                    continue;
                }
                const int32 NeighbourIndex = NeighbourY[N] * BlocksX + NeighbourX[N];
                if (Visited[NeighbourIndex] || !Blocks[NeighbourIndex].bFlat)
                {
                    continue;
                }
                if (FMath::Abs(Blocks[NeighbourIndex].Level - SeedLevel)
                    > FlatRegionToleranceLevels)
                {
                    continue;
                }
                Visited[NeighbourIndex] = true;
                Frontier.Add(NeighbourIndex);
            }
        }

        if (RegionPixels > BestPixels)
        {
            BestPixels = RegionPixels;
            Stats.LargestRegionLevel = SeedLevel;
            Stats.RegionMinX = static_cast<double>(BlockEdge(MinBx, BlocksX, Width))
                / static_cast<double>(Width);
            Stats.RegionMaxX = static_cast<double>(BlockEdge(MaxBx + 1, BlocksX, Width))
                / static_cast<double>(Width);
            Stats.RegionMinY = static_cast<double>(BlockEdge(MinBy, BlocksY, Height))
                / static_cast<double>(Height);
            Stats.RegionMaxY = static_cast<double>(BlockEdge(MaxBy + 1, BlocksY, Height))
                / static_cast<double>(Height);
        }
    }

    Stats.LargestRegionFraction = static_cast<double>(BestPixels) / static_cast<double>(PixelCount);
    Stats.bLargeFlatRegion = Stats.LargestRegionFraction >= LargeFlatRegionFraction;
    // Last, so that every early return above leaves it false and no partial reading can be taken
    // for a measurement.
    Stats.bMeasured = true;
    return Stats;
}

void PinWrightFlatRegion::AddFlatRegionStatsFields(const FFlatRegionStats& Stats,
    const TSharedPtr<FJsonObject>& ImageStats)
{
    if (!ImageStats.IsValid() || !Stats.bMeasured)
    {
        return;
    }
    ImageStats->SetNumberField(TEXT("flatRegionFraction"), Stats.LargestRegionFraction);
    ImageStats->SetNumberField(TEXT("flatRegionLevel"), Stats.LargestRegionLevel);
    ImageStats->SetNumberField(TEXT("flatBlockFraction"), Stats.FlatBlockFraction);

    TSharedPtr<FJsonObject> Bounds = MakeShared<FJsonObject>();
    Bounds->SetNumberField(TEXT("minX"), Stats.RegionMinX);
    Bounds->SetNumberField(TEXT("minY"), Stats.RegionMinY);
    Bounds->SetNumberField(TEXT("maxX"), Stats.RegionMaxX);
    Bounds->SetNumberField(TEXT("maxY"), Stats.RegionMaxY);
    ImageStats->SetObjectField(TEXT("flatRegionBounds"), Bounds);
}

FString PinWrightFlatRegion::DescribeFlatRegion(const FFlatRegionStats& Stats)
{
    if (!Stats.bMeasured)
    {
        return FString();
    }
    return FString::Printf(
        TEXT("%.1f%% of the frame at luminance level %d, spanning %.0f-%.0f%% horizontally and ")
        TEXT("%.0f-%.0f%% vertically (top-left origin)"),
        Stats.LargestRegionFraction * 100.0,
        Stats.LargestRegionLevel,
        Stats.RegionMinX * 100.0, Stats.RegionMaxX * 100.0,
        Stats.RegionMinY * 100.0, Stats.RegionMaxY * 100.0);
}
