// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Math/IntPoint.h"
#include "Math/Vector2D.h"

#include "Handlers/Drive/DriveInput.h"   // EDriveMouseButton

// OS-level mouse injection for the drive capability — the deliberate opposite of
// FDriveInput, which synthesizes the same gestures INSIDE Slate.
//
// FDriveInput enters at FSlateApplication::ProcessMouse*Event after moving the cursor
// with ICursor::SetPosition, and forces SetHandleDeviceInputWhenApplicationNotActive(true)
// while it does. That is the right trade for driving editor chrome, but it makes a whole
// class of behavior untestable: the OS and SDL layers are skipped entirely, so SDL mouse
// confinement (EMouseLockMode::LockOnCapture) and relative mode
// (UseHighPrecisionMouseMovement) never engage, and forcing the inactive-input flag
// perturbs every FSlateApplication::IsActive()-dependent path — which is exactly where a
// CommonUI mouse-capture bug lives.
//
// This class instead asks the X server to generate REAL pointer events (XTEST), so they
// arrive over the same X -> SDL -> engine route a human's mouse takes. It never touches
// FSlateApplication, ICursor, or the inactive-input flag.
//
// LINUX/X11 ONLY. libX11 / libXtst are resolved lazily with dlopen/dlsym so the module
// needs no new link dependency in PinWright.Build.cs; on every other platform each entry
// point fails with a caller-facing reason. MOUSE ONLY: XTEST key events were observed not
// to reach the editor's SDL window in this setup even with X focus on it, so keys stay on
// the Slate path (FDriveInput::PressKey) and drive.key has no OS variant.
//
// Every entry point BLOCKS the calling thread: the motion path and the button hold are
// paced with real sleeps, so a click costs roughly half a second.
class FDriveOsInput
{
public:
    // Whether OS-level injection is compiled in AND the X display could be opened. Fills
    // OutError with the caller-facing reason when it is not, so a handler can refuse the
    // request up front instead of failing deep inside the injection.
    static bool IsAvailable(FString& OutError);

    // Move the real pointer to ScreenPos (absolute desktop pixels, the space an element's
    // geometry.absolute already reports) along ComputeMotionPath. Blocks ~250 ms.
    static bool MoveTo(const FVector2D& ScreenPos, FString& OutError);

    // MoveTo, then a real button press held ~80 ms and released. Blocks ~450 ms. Refuses
    // (no press) when a foreign window took the point during the motion.
    static bool ClickAt(const FVector2D& ScreenPos, EDriveMouseButton Button, FString& OutError);

    // The X window that would receive real pointer input at a point when it does NOT belong
    // to this process: a peer editor, a game or a desktop panel stacked over the target.
    struct FForeignWindow
    {
        uint64 WindowId = 0;
        uint32 Pid = 0;     // 0 when no window at the point names a _NET_WM_PID
        FString Title;
    };

    // True, filling Out, when the top-most X window at ScreenPos is not this editor's, so
    // XTEST input there would reach another application. False when this process owns the
    // point, and when X is unavailable (IsAvailable already refused os_input then).
    static bool FindForeignWindowAt(const FVector2D& ScreenPos, FForeignWindow& Out);

    // ---- Pure helpers (no X11 dependency; unit-tested) ----

    // Interpolated pointer path from From to To in whole screen pixels. Excludes From and
    // ends exactly on To, so the app sees a hand-like sequence of motion deltas instead of
    // one teleport that confinement / relative mode would digest differently.
    static TArray<FIntPoint> ComputeMotionPath(const FIntPoint& From, const FIntPoint& To);

    // X button number for a drive button: left 1, middle 2, right 3.
    static int32 ButtonToXButton(EDriveMouseButton Button);

    // Who owns input at a point, from the _NET_WM_PID values (0 = unset) of the X windows
    // containing it, outermost (the root's child) first. The DEEPEST window naming a pid
    // decides: a reparenting window manager's frame sits above the client window and may
    // carry the WM's own pid. Sets OutOwnerIndex to that window (INDEX_NONE when none names
    // a pid) and returns whether it is SelfPid; an unnamed point is never ours, because
    // SDL stamps _NET_WM_PID on every window it creates.
    static bool IsPointOwnedBy(const TArray<uint32>& PathPids, uint32 SelfPid, int32& OutOwnerIndex);
};
