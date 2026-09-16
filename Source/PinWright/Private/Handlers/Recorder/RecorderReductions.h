// Copyright (c) 2026 Alexander Penkin. MIT License.

// Scalar summary + series reductions over a (key, tag) change-point window.
//
// Summarize gives endpoints + extrema; Diff returns a capped, cursor-paginated
// change list; Downsample returns a peak-preserving point budget. A value's scalar
// magnitude is its component norm, so vector kinds get a meaningful min/max.
#pragma once

#include "CoreMinimal.h"
#include "RecorderSessionModel.h"

namespace RecorderReductions
{

using namespace RecorderModel;

// Endpoint + extremum summary of a series window. The *Set flags distinguish an
// absent value (window had no as-of point) from a real 0.0.
struct FSummary
{
    bool bStartSet = false;
    double Start = 0.0;
    bool bEndSet = false;
    double End = 0.0;
    bool bNetDeltaSet = false;
    double NetDelta = 0.0;
    bool bMinSet = false;
    double Min = 0.0;
    bool bMaxSet = false;
    double Max = 0.0;
    bool bMeanSet = false;
    double Mean = 0.0;
    bool bArgMaxSet = false;
    double ArgMaxTs = 0.0;
    bool bArgMinSet = false;
    double ArgMinTs = 0.0;
    int32 ChangeCount = 0;
};

// Summarize the change of Series over [From, To]: as-of endpoints (carried
// forward), net delta, min/max/mean of the magnitude, the timestamps where the
// extrema occurred, and the count of change points inside the window.
FSummary Summarize(const TArray<FChangePoint>& Series, double From, double To);

// Capped change list over [From, To]. Returns the change points starting at
// Cursor (an offset into the window), up to MaxPoints; sets OutNextCursor (>= 0)
// when more remain (INDEX_NONE otherwise) and reports the total window size and
// how many were elided.
void Diff(const TArray<FChangePoint>& Series, double From, double To,
          int32 MaxPoints, int32 Cursor,
          TArray<FChangePoint>& OutPoints, int32& OutTotal, int32& OutElided, int32& OutNextCursor);

// Reduce the window to at most MaxPoints points, preserving the first, last, and
// per-bucket magnitude extrema so peaks survive. Returns the original window
// unchanged when it already fits.
void Downsample(const TArray<FChangePoint>& Series, double From, double To,
                int32 MaxPoints,
                TArray<FChangePoint>& OutPoints, int32& OutTotal, int32& OutElided);

} // namespace RecorderReductions
