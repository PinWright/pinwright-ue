// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveSetOfMarkLayout.h"

#include "Handlers/Drive/DriveTypes.h"
#include "Math/UnrealMathUtility.h"

FDriveMarkLayout FDriveSetOfMarkLayout::BuildLayout(
    const TArray<FDriveElement>& Elements,
    const FVector2D& FrameOrigin,
    int32 FrameWidth,
    int32 FrameHeight,
    int32 MarkCap)
{
    FDriveMarkLayout Result;

    // Frame bounds in frame pixels: [0,0] - [Width,Height].
    const double FrameMaxX = static_cast<double>(FrameWidth);
    const double FrameMaxY = static_cast<double>(FrameHeight);

    for (int32 Index = 0; Index < Elements.Num(); ++Index)
    {
        const FDriveElement& Element = Elements[Index];
        const int32 Number = Index + 1;

        // Beyond the mark cap: omit without geometry work. Reporting (not
        // truncating) keeps the omitted count honest.
        if (Index >= MarkCap)
        {
            Result.Omitted.Add(Number);
            continue;
        }

        // Desktop -> frame pixels.
        const FVector2D ElemMin = Element.AbsolutePosition - FrameOrigin;
        const FVector2D ElemMax = ElemMin + Element.AbsoluteSize;

        // Fully offscreen: no positive-area overlap with the frame. Strict
        // inequalities drop rects that merely touch an edge with zero overlap.
        const bool bOverlapsFrame =
            ElemMax.X > 0.0 && ElemMin.X < FrameMaxX &&
            ElemMax.Y > 0.0 && ElemMin.Y < FrameMaxY;
        if (!bOverlapsFrame)
        {
            Result.Omitted.Add(Number);
            continue;
        }

        // Clamp the rect to the frame bounds.
        const FVector2D ClampedMin(
            FMath::Clamp(ElemMin.X, 0.0, FrameMaxX),
            FMath::Clamp(ElemMin.Y, 0.0, FrameMaxY));
        const FVector2D ClampedMax(
            FMath::Clamp(ElemMax.X, 0.0, FrameMaxX),
            FMath::Clamp(ElemMax.Y, 0.0, FrameMaxY));
        const FBox2D Box(ClampedMin, ClampedMax);

        // Too small to host a readable mark after clamping.
        const double ClampedWidth = ClampedMax.X - ClampedMin.X;
        const double ClampedHeight = ClampedMax.Y - ClampedMin.Y;
        if (ClampedWidth < MinVisibleSizePx || ClampedHeight < MinVisibleSizePx)
        {
            Result.Omitted.Add(Number);
            continue;
        }

        FDriveMark Mark;
        Mark.Number = Number;
        Mark.Box = Box;
        // Top-left corner; within the clamped box and therefore inside the frame.
        Mark.LabelAnchor = ClampedMin;
        Mark.ElementHandle = Element.Handle;
        Result.Marks.Add(MoveTemp(Mark));
    }

    Result.OmittedCount = Result.Omitted.Num();
    return Result;
}
