// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/ZFightingAnalysis.h"

namespace PinWrightZFighting
{
    bool IsPowerOfTwoRatio(double Ratio)
    {
        // NaN and non-positive values fail the first comparison and are not powers of two.
        if (!(Ratio > 0.0))
        {
            return false;
        }

        // Fold into [1, 2) by repeated halving and doubling. Multiplying a double by 0.5 or
        // 2.0 is EXACT in IEEE-754 (it only shifts the exponent), so a genuine power of two
        // lands on precisely 1.0 with no accumulated error and the final comparison can be
        // exact rather than tolerance-based. The iteration guards bound the loop for
        // infinities and denormals, both of which then fail the exact test and are correctly
        // reported as not powers of two.
        double Value = Ratio;
        for (int32 Guard = 0; Value >= 2.0 && Guard < 4096; ++Guard)
        {
            Value *= 0.5;
        }
        for (int32 Guard = 0; Value < 1.0 && Guard < 4096; ++Guard)
        {
            Value *= 2.0;
        }
        return Value == 1.0;
    }

    int32 AccumulateChannelDifference(TConstArrayView<FLinearColor> CaptureA,
        TConstArrayView<FLinearColor> CaptureB, double Threshold, TArray<uint8>& InOutMask)
    {
        const int32 Count = FMath::Min3(CaptureA.Num(), CaptureB.Num(), InOutMask.Num());
        int32 Flagged = 0;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const FLinearColor& A = CaptureA[Index];
            const FLinearColor& B = CaptureB[Index];
            const double Delta = FMath::Max3(
                static_cast<double>(FMath::Abs(A.R - B.R)),
                static_cast<double>(FMath::Abs(A.G - B.G)),
                static_cast<double>(FMath::Abs(A.B - B.B)));
            if (Delta > Threshold)
            {
                InOutMask[Index] |= MASK_Flagged;
                ++Flagged;
            }
        }
        return Flagged;
    }

    int32 ApplyDepthRangeExclusion(TConstArrayView<FLinearColor> Depth, double MinDepth,
        double MaxDepth, TArray<uint8>& InOutMask)
    {
        const int32 Count = FMath::Min(Depth.Num(), InOutMask.Num());
        int32 Excluded = 0;
        const double LowerLimit = MinDepth * 1.02;
        const double UpperLimit = MaxDepth > 0.0 ? MaxDepth * 0.98 : 0.0;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const double PixelDepth = static_cast<double>(Depth[Index].R);
            // Non-positive depth means the pixel carries no usable measurement (nothing was
            // rasterised into it, or the readback produced a degenerate value). Excluding it
            // is honest; treating it as depth 0 would exclude the whole frame.
            const bool bOutOfRange = !(PixelDepth > 0.0)
                || PixelDepth < LowerLimit
                || (UpperLimit > 0.0 && PixelDepth > UpperLimit);
            if (bOutOfRange)
            {
                InOutMask[Index] |= MASK_ClipExcluded;
                ++Excluded;
            }
        }
        return Excluded;
    }

    int32 CountAffected(const TArray<uint8>& Mask)
    {
        int32 Affected = 0;
        for (const uint8 Flags : Mask)
        {
            if (IsAffected(Flags))
            {
                ++Affected;
            }
        }
        return Affected;
    }

    void FindRegions(const TArray<uint8>& Mask, int32 Width, int32 Height, int32 MaxRegions,
        TArray<FRegion>& OutRegions, int32& OutTotalRegions)
    {
        OutRegions.Reset();
        OutTotalRegions = 0;
        if (Width <= 0 || Height <= 0 || Mask.Num() < Width * Height)
        {
            return;
        }

        TArray<bool> Visited;
        Visited.SetNumZeroed(Width * Height);

        // Explicit stack rather than recursion: a single seam can span hundreds of thousands
        // of connected pixels at analysis resolution, which is a stack overflow as a
        // recursive flood fill.
        //
        // The stack carries its own top index instead of using TArray::Pop so the buffer is
        // never shrunk mid-fill (Pop's default reallocates) and so this compiles unchanged
        // across UE 5.3-5.8, where the bAllowShrinking argument changed type.
        TArray<int32> Stack;
        int32 StackNum = 0;
        const auto PushPixel = [&Stack, &StackNum](int32 Value)
        {
            if (StackNum < Stack.Num())
            {
                Stack[StackNum] = Value;
            }
            else
            {
                Stack.Add(Value);
            }
            ++StackNum;
        };

        TArray<FRegion> Found;

        for (int32 Y = 0; Y < Height; ++Y)
        {
            for (int32 X = 0; X < Width; ++X)
            {
                const int32 Seed = Y * Width + X;
                if (Visited[Seed] || !IsAffected(Mask[Seed]))
                {
                    continue;
                }

                FRegion Region;
                Region.MinX = X;
                Region.MaxX = X;
                Region.MinY = Y;
                Region.MaxY = Y;
                double SumX = 0.0;
                double SumY = 0.0;

                StackNum = 0;
                PushPixel(Seed);
                Visited[Seed] = true;

                while (StackNum > 0)
                {
                    const int32 Index = Stack[--StackNum];
                    const int32 PixelX = Index % Width;
                    const int32 PixelY = Index / Width;

                    ++Region.Pixels;
                    SumX += PixelX;
                    SumY += PixelY;
                    Region.MinX = FMath::Min(Region.MinX, PixelX);
                    Region.MaxX = FMath::Max(Region.MaxX, PixelX);
                    Region.MinY = FMath::Min(Region.MinY, PixelY);
                    Region.MaxY = FMath::Max(Region.MaxY, PixelY);

                    // 8-connected: a z-fighting seam is frequently a diagonal speckle one
                    // pixel wide, which 4-connectivity would shatter into hundreds of
                    // single-pixel regions and hide the one seam a caller needs to see.
                    for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
                    {
                        for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
                        {
                            if (OffsetX == 0 && OffsetY == 0)
                            {
                                continue;
                            }
                            const int32 NeighbourX = PixelX + OffsetX;
                            const int32 NeighbourY = PixelY + OffsetY;
                            if (NeighbourX < 0 || NeighbourX >= Width || NeighbourY < 0 || NeighbourY >= Height)
                            {
                                continue;
                            }
                            const int32 NeighbourIndex = NeighbourY * Width + NeighbourX;
                            if (!Visited[NeighbourIndex] && IsAffected(Mask[NeighbourIndex]))
                            {
                                Visited[NeighbourIndex] = true;
                                PushPixel(NeighbourIndex);
                            }
                        }
                    }
                }

                Region.CentroidX = SumX / FMath::Max(1, Region.Pixels);
                Region.CentroidY = SumY / FMath::Max(1, Region.Pixels);
                Found.Add(Region);
            }
        }

        OutTotalRegions = Found.Num();
        Found.Sort([](const FRegion& Lhs, const FRegion& Rhs) { return Lhs.Pixels > Rhs.Pixels; });

        const int32 Keep = FMath::Clamp(MaxRegions, 0, Found.Num());
        OutRegions.Append(Found.GetData(), Keep);
    }

    FIntPoint ResolveMaskSize(int32 Width, int32 Height, int32 LongEdge)
    {
        if (Width <= 0 || Height <= 0)
        {
            return FIntPoint(0, 0);
        }
        const int32 SourceLongEdge = FMath::Max(Width, Height);
        if (LongEdge <= 0 || SourceLongEdge <= LongEdge)
        {
            return FIntPoint(Width, Height);
        }
        const double Scale = static_cast<double>(LongEdge) / static_cast<double>(SourceLongEdge);
        return FIntPoint(
            FMath::Max(1, FMath::RoundToInt(Width * Scale)),
            FMath::Max(1, FMath::RoundToInt(Height * Scale)));
    }

    void BuildMaskBitmap(const TArray<uint8>& Mask, int32 Width, int32 Height,
        int32 MaskWidth, int32 MaskHeight, TArray<FColor>& OutBitmap)
    {
        OutBitmap.Reset();
        if (Width <= 0 || Height <= 0 || MaskWidth <= 0 || MaskHeight <= 0
            || Mask.Num() < Width * Height)
        {
            return;
        }

        OutBitmap.SetNumUninitialized(MaskWidth * MaskHeight);

        for (int32 OutY = 0; OutY < MaskHeight; ++OutY)
        {
            const int32 StartY = static_cast<int32>(static_cast<int64>(OutY) * Height / MaskHeight);
            const int32 EndY = FMath::Max(StartY + 1,
                static_cast<int32>(static_cast<int64>(OutY + 1) * Height / MaskHeight));
            for (int32 OutX = 0; OutX < MaskWidth; ++OutX)
            {
                const int32 StartX = static_cast<int32>(static_cast<int64>(OutX) * Width / MaskWidth);
                const int32 EndX = FMath::Max(StartX + 1,
                    static_cast<int32>(static_cast<int64>(OutX + 1) * Width / MaskWidth));

                bool bAffected = false;
                bool bExcluded = false;
                for (int32 SourceY = StartY; SourceY < EndY && !bAffected; ++SourceY)
                {
                    for (int32 SourceX = StartX; SourceX < EndX; ++SourceX)
                    {
                        const uint8 Flags = Mask[SourceY * Width + SourceX];
                        if (IsAffected(Flags))
                        {
                            bAffected = true;
                            break;
                        }
                        if ((Flags & MASK_ClipExcluded) != 0)
                        {
                            bExcluded = true;
                        }
                    }
                }

                const FColor Colour = bAffected
                    ? FColor(255, 48, 48, 255)
                    : (bExcluded ? FColor(0, 0, 96, 255) : FColor(0, 0, 0, 255));
                OutBitmap[OutY * MaskWidth + OutX] = Colour;
            }
        }
    }
}
