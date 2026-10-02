// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Math/IntPoint.h"
#include "Math/Vector2D.h"
#include "Templates/Function.h"

#include "Handlers/Drive/DriveInput.h"   // EDriveMouseButton, FDriveInjectFailure

// One event of an os_input pointer gesture, due AtMs after the gesture starts.
struct FDriveOsGestureStep
{
    enum class EKind : uint8
    {
        Move,
        Press,
        Release,
        // Nothing is sent: the gesture ends here, after the app has had time to pump the release.
        End
    };

    EKind Kind = EKind::End;
    FIntPoint Point = FIntPoint::ZeroValue;
    double AtMs = 0.0;
};

// A real-pointer (XTEST) gesture as a timed step plan: approach the press point, press, hold,
// optionally move while pressed, release. FDriveOsInput::ClickAt sends a whole click from one
// call with real sleeps, so the editor's game thread is blocked and the engine pumps every event
// of the click in one frame. That is fine for Slate, which is event-driven, and useless for game
// code that polls the mouse per tick (a 3D gizmo drag, a box select): it never sees the button
// held across frames. Start() therefore runs the plan from a ticker, sending every step that is
// due at a tick in that tick, so the engine pumps frames between a press, the pressed motion and
// the release.
class FDriveOsGesture
{
public:
    // Pacing, the same as FDriveOsInput's click: motion steps 8 ms apart, a pause before the
    // press, ~80 ms held, and a pause after the release so the app pumps it before we report.
    static constexpr int32 StepMs = 8;
    static constexpr int32 ApproachSettleMs = 60;
    static constexpr int32 DefaultHoldMs = 80;
    static constexpr int32 ReleaseSettleMs = 120;
    // One pressed segment (to one waypoint) by default: FDriveOsInput::ComputeMotionPath's 24
    // steps at StepMs.
    static constexpr int32 DefaultSegmentMs = 192;

    // ---- Pure (unit-tested) ----

    // The plan from the pointer's current position: interpolated approach motion to Press,
    // Press, then after HoldMs one interpolated segment per PressedPath point (SegmentMs each),
    // Release at the last point reached (Press when the path is empty), End. Consecutive
    // duplicate points are dropped, so a pointer already on Press gets no approach motion and a
    // short wobble is a few distinct moves. HoldMs = 0 with no path puts Press and Release at
    // the same time, so they go out in the same tick.
    static TArray<FDriveOsGestureStep> BuildPlan(const FIntPoint& Pointer, const FIntPoint& Press,
        const TArray<FIntPoint>& PressedPath, int32 HoldMs, int32 SegmentMs);

    // Run every step of Plan due by NowMs, from InOutNext on. False, with OutFailure, when Execute
    // refused a step; InOutNext then still names it and no later step was run.
    using FExecuteStep = TFunctionRef<bool(const FDriveOsGestureStep&, FDriveInjectFailure&)>;
    static bool RunDueSteps(const TArray<FDriveOsGestureStep>& Plan, double NowMs, int32& InOutNext,
        FExecuteStep Execute, FDriveInjectFailure& OutFailure);

    // A game-viewport pixel (FViewport::GetSizeXY space, origin top-left) in absolute desktop
    // pixels, given the viewport widget's absolute rect. The rect may differ from the pixel size
    // (DPI scale), so the mapping is a per-axis ratio.
    static FVector2D ViewportPixelToScreen(const FVector2D& Pixel, const FIntPoint& ViewportSize,
        const FVector2D& WidgetScreenPos, const FVector2D& WidgetScreenSize);

    // ---- XTEST drivers (Linux/X11; elsewhere they refuse like FDriveOsInput) ----

    struct FResult
    {
        // GFrameCounter when the press / release went out, so a caller can see the gesture span
        // engine frames.
        uint64 PressFrame = 0;
        uint64 ReleaseFrame = 0;
        double ElapsedMs = 0.0;
        // The real pointer after the gesture, and whether it is on the release point.
        FIntPoint PointerAfter = FIntPoint::ZeroValue;
        bool bPointerOnRelease = false;
    };
    using FOnDone = TFunction<void(bool bOk, const FDriveInjectFailure& Failure, const FResult& Result)>;

    // Take the display lock, refuse like FDriveOsInput (OS_INPUT_BUSY, POINTER_GRABBED), then run
    // the plan from the core ticker and call OnDone once (synchronously on an up-front refusal).
    // The press gets FDriveOsInput::CheckPress (INPUT_FAILED / POINTER_MOVED, unpressed). The lock
    // is held until OnDone.
    static void Start(const FIntPoint& Press, const TArray<FIntPoint>& PressedPath,
        EDriveMouseButton Button, int32 HoldMs, int32 SegmentMs, FOnDone OnDone);

    // The same gesture paced with real sleeps on the calling thread (drive.drag os_input, whose
    // action flow injects synchronously). The engine sees it in one frame, as with ClickAt.
    static bool RunBlocking(const FIntPoint& Press, const TArray<FIntPoint>& PressedPath,
        EDriveMouseButton Button, int32 HoldMs, int32 SegmentMs, FDriveInjectFailure& OutFailure);
};
