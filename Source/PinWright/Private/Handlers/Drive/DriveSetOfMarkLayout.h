// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"
#include "Math/Vector2D.h"

struct FDriveElement;

// One assigned Set-of-Mark label: a numbered badge anchored on an interactable
// element's on-screen rect. The Number doubles as the element's MarkIndex and
// feeds FDriveScreenshot::MarksDrawn when the badge is actually painted.
struct FDriveMark
{
    // 1-based mark number; equals the element's position in the input array.
    int32 Number = 0;
    // Element rect in frame pixels (desktop rect minus the frame origin), clamped to the
    // frame bounds [0,0]-[Width,Height].
    FBox2D Box = FBox2D(ForceInit);
    // Frame-space point where the number label is anchored (top-left corner of
    // Box, guaranteed inside the frame so the label stays visible).
    FVector2D LabelAnchor = FVector2D::ZeroVector;
    // Handle of the source element this mark addresses.
    FString ElementHandle;
};

// Result of laying out Set-of-Mark labels over a frame. `Marks` are the labels
// to draw; `Omitted` lists the 1-based numbers of elements that received no
// visible mark (fully offscreen, too small after clamping, or beyond the cap).
// Every input element's number appears in exactly one of Marks[].Number or
// Omitted, so the two sets mirror FDriveScreenshot's MarksDrawn / MarksOmitted.
struct FDriveMarkLayout
{
    TArray<FDriveMark> Marks;
    TArray<int32> Omitted;
    int32 OmittedCount = 0;
};

// Pure layout math for Set-of-Mark overlays. No Slate/CEF/UObject dependency and
// no image drawing -- the caller pre-filters `Elements` to interactables, then
// this assigns mark numbers, clamps each rect to the frame, and reports
// omissions. Deterministic: identical input always yields identical output.
class FDriveSetOfMarkLayout
{
public:
    // Minimum width AND height (in pixels) a clamped rect must retain to host a
    // visible mark; anything smaller is omitted as too small.
    static constexpr double MinVisibleSizePx = 8.0;

    // Lays out marks for `Elements` over a `FrameWidth` x `FrameHeight` frame whose
    // top-left pixel sits at `FrameOrigin`. Element rects are desktop space, and a captured
    // bitmap's (0,0) is the captured viewport's / window's desktop position, not the desktop
    // origin, so each rect is shifted by -FrameOrigin before the overlap test and the clamp.
    // Mark numbers are the 1-based input positions. Only the first `MarkCap`
    // input elements are eligible for a visible mark; the rest are omitted with
    // no silent truncation. Performs no randomness and no drawing.
    static FDriveMarkLayout BuildLayout(
        const TArray<FDriveElement>& Elements,
        const FVector2D& FrameOrigin,
        int32 FrameWidth,
        int32 FrameHeight,
        int32 MarkCap);
};
