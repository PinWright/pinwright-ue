// Copyright (c) 2026 Alexander Penkin. MIT License.

// As-of (state-at-time) lookups over a change-only series.
//
// Series are stored sorted by timestamp, so the last change point with Ts <= N is
// the value in effect at time N — a binary search carrying the most recent value
// forward. Generalized from the original integer-frame indexing to a double axis.
#pragma once

#include "CoreMinimal.h"
#include "RecorderSessionModel.h"

namespace RecorderAsOf
{

using namespace RecorderModel;

// Index of the last change point with Ts <= QueryTs, or INDEX_NONE when the series
// is empty or every point is after QueryTs.
inline int32 IndexAt(const TArray<FChangePoint>& Series, double QueryTs)
{
    int32 Lo = 0;
    int32 Hi = Series.Num() - 1;
    int32 Result = INDEX_NONE;
    while (Lo <= Hi)
    {
        const int32 Mid = Lo + ((Hi - Lo) >> 1);
        if (Series[Mid].Ts <= QueryTs)
        {
            Result = Mid;
            Lo = Mid + 1;
        }
        else
        {
            Hi = Mid - 1;
        }
    }
    return Result;
}

// The value in effect at QueryTs (carried forward from the last change <= N).
// Returns false when nothing exists at or before N.
inline bool ValueAt(const TArray<FChangePoint>& Series, double QueryTs, FChangePoint& OutPoint)
{
    const int32 Idx = IndexAt(Series, QueryTs);
    if (Idx == INDEX_NONE)
    {
        return false;
    }
    OutPoint = Series[Idx];
    return true;
}

// Index of the first change point with Ts >= QueryTs, or Num() when none.
inline int32 FirstAtLeast(const TArray<FChangePoint>& Series, double QueryTs)
{
    int32 Lo = 0;
    int32 Hi = Series.Num() - 1;
    int32 Result = Series.Num();
    while (Lo <= Hi)
    {
        const int32 Mid = Lo + ((Hi - Lo) >> 1);
        if (Series[Mid].Ts >= QueryTs)
        {
            Result = Mid;
            Hi = Mid - 1;
        }
        else
        {
            Lo = Mid + 1;
        }
    }
    return Result;
}

// Inclusive index span [First, Last] of change points whose Ts falls in [From, To].
// Returns a zero-count span (First > Last, encoded as {0, -1}) when the window is empty.
inline void SliceRange(const TArray<FChangePoint>& Series, double From, double To,
                       int32& OutFirst, int32& OutLast)
{
    const int32 First = FirstAtLeast(Series, From);
    const int32 Last = IndexAt(Series, To);
    if (First > Last)
    {
        OutFirst = 0;
        OutLast = -1;
        return;
    }
    OutFirst = First;
    OutLast = Last;
}

} // namespace RecorderAsOf
