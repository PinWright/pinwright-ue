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
// LINUX/X11 AND WINDOWS. libX11 / libXtst are resolved lazily with dlopen/dlsym so the module
// needs no new link dependency in PinWright.Build.cs. On Windows the entry points delegate to
// the SendInput backend in DriveOsInputWindows.cpp (input_path "os_win32"; written on Linux,
// not yet compiled or run on Windows). On every other platform each entry point fails with a
// caller-facing reason. MOUSE ONLY: drive.key has no OS variant and keys
// stay on the Slate path (FDriveInput::PressKey). XTEST key events DO reach the editor's SDL
// window (xdotool Ctrl+Z/Y/C/V, held across engine ticks with X focus on the editor, fired a
// PIE game's hotkeys), so this is an unimplemented path, not an impossible one.
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
    // geometry.absolute already reports) along ComputeMotionPath. Blocks ~250 ms, plus up to
    // LockTimeoutSeconds waiting for another injector on this display (OS_INPUT_BUSY).
    // Refuses with POINTER_GRABBED, moving nothing, while any client holds an X pointer grab.
    static bool MoveTo(const FVector2D& ScreenPos, FDriveInjectFailure& OutFailure);

    // MoveTo, then a real button press held ~80 ms and released, all under the display lock.
    // Blocks ~450 ms. Refuses (no press) when a foreign window took the point during the
    // motion (INPUT_FAILED), or when the pointer is not on the target right before the press
    // (POINTER_MOVED: a human, a raw xdotool script, or a pointer grab moved or clamped it).
    static bool ClickAt(const FVector2D& ScreenPos, EDriveMouseButton Button, FDriveInjectFailure& OutFailure);

    // How long an injection waits for another process's injection on the same display.
    static constexpr double LockTimeoutSeconds = 5.0;

    // Cross-process exclusive lock serializing os_input on one X display. A display has ONE
    // core pointer, so two editors injecting at once interleave motion and presses and a
    // click lands wherever the other one left the pointer. flock on a per-display file; the
    // lock belongs to the open file, so two instances in one process exclude each other too.
    // The holder writes its pid into the file so a peer that times out can name it.
    // On Windows a named mutex per logon session instead (HolderPid stays 0: a mutex does not
    // record its owner). Elsewhere it is never held.
    class FDisplayLock
    {
    public:
        // Blocks up to TimeoutSeconds for the lock.
        FDisplayLock(const FString& Path, double TimeoutSeconds);
        ~FDisplayLock();
        FDisplayLock(const FDisplayLock&) = delete;
        FDisplayLock& operator=(const FDisplayLock&) = delete;

        bool IsHeld() const { return Fd >= 0 || Mutex != nullptr; }

        // When not held: whether another holder outlasted the timeout (vs. an I/O failure),
        // the pid that holder recorded (0 = unknown), and the caller-facing reason.
        bool bTimedOut = false;
        uint32 HolderPid = 0;
        FString Error;

    private:
        int32 Fd = -1;
        void* Mutex = nullptr;  // Windows: the owned named-mutex HANDLE
    };

    // The lock file for this editor's display ($DISPLAY and uid).
    static FString DisplayLockPath();

    // Whether the real pointer is exactly on Target; OutPointer receives where it is.
    // False when X is unavailable or the pointer cannot be read.
    static bool IsPointerAt(const FIntPoint& Target, FIntPoint& OutPointer);

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

    // Active X grabs on this display. X does not say who holds a grab, so each flag is a
    // probe: PinWright's own X connection (a separate client from SDL's) tries XGrabPointer /
    // XGrabKeyboard on the root and releases at once; AlreadyGrabbed or GrabFrozen means
    // another client holds it. While the pointer is grabbed, real input goes to the grabber,
    // so an XTEST click lands nowhere near its target.
    struct FGrabState
    {
        bool bPointerGrabbed = false;
        bool bKeyboardGrabbed = false;
        // SDL in this process holds a grab (a window grab, or relative mouse mode on its
        // input-focus window, which SDL implements as a confining XGrabPointer). Unset when
        // this engine's SDL lacks the entry points to tell.
        TOptional<bool> bHeldByThisEditor;
    };

    // False when X is unavailable (non-Linux, no display); Out is then left untouched.
    static bool ProbeGrabs(FGrabState& Out);

    // ---- Gesture primitives: FDriveOsGesture paces these across engine ticks ----

    // Take the display lock and run MoveTo's refusals before anything moves (OS_INPUT_BUSY,
    // POINTER_GRABBED, INPUT_FAILED). Null on refusal; otherwise the caller holds the returned
    // lock for the whole gesture, and OutPointer is where the real pointer is now. Does not block
    // beyond the lock wait.
    static TSharedPtr<FDisplayLock> BeginGesture(FIntPoint& OutPointer, FDriveInjectFailure& OutFailure);

    // ClickAt's refusals right before a press at Target: INPUT_FAILED when another process's
    // window took the point, POINTER_MOVED when the real pointer is not on it.
    static bool CheckPress(const FIntPoint& Target, FDriveInjectFailure& OutFailure);

    // One XTEST motion event / button edge, flushed at once. No checks and no pacing: the caller
    // holds BeginGesture's lock and checked the press with CheckPress.
    static void SendMotion(const FIntPoint& Point);
    static void SendButton(EDriveMouseButton Button, bool bPress);

    // ---- Pure helpers (no X11 dependency; unit-tested) ----

    // Interpolated pointer path from From to To in whole screen pixels. Excludes From and
    // ends exactly on To, so the app sees a hand-like sequence of motion deltas instead of
    // one teleport that confinement / relative mode would digest differently.
    static TArray<FIntPoint> ComputeMotionPath(const FIntPoint& From, const FIntPoint& To);

    // Lock file for a display: /tmp/pinwright-os-input-<uid>-<display>.lock. Fixed under /tmp
    // rather than $XDG_RUNTIME_DIR so two editors launched with different environments still
    // meet on one file. The screen suffix is dropped (":0" and ":0.0" share one pointer, so
    // they must share one lock).
    static FString LockPathFor(const FString& Display, uint32 Uid);

    // X button number for a drive button: left 1, middle 2, right 3.
    static int32 ButtonToXButton(EDriveMouseButton Button);

    // SendInput's MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK coordinate (0..65535) for a
    // virtual-desktop pixel, given the virtual screen's origin (SM_XVIRTUALSCREEN, negative when
    // a monitor sits left of / above the primary) and extent (SM_CXVIRTUALSCREEN). Pixels off
    // the desktop clamp to its edge.
    static int32 NormalizeVirtualDeskCoord(int32 Pixel, int32 Origin, int32 Extent);

    // The response's input_path for os_input on this platform: "os_win32" on Windows, else "os_x11".
    static const TCHAR* InputPathLabel();

    // Who owns input at a point, from the _NET_WM_PID values (0 = unset) of the X windows
    // containing it, outermost (the root's child) first. The DEEPEST window naming a pid
    // decides: a reparenting window manager's frame sits above the client window and may
    // carry the WM's own pid. Sets OutOwnerIndex to that window (INDEX_NONE when none names
    // a pid) and returns whether it is SelfPid; an unnamed point is never ours, because
    // SDL stamps _NET_WM_PID on every window it creates.
    static bool IsPointOwnedBy(const TArray<uint32>& PathPids, uint32 SelfPid, int32& OutOwnerIndex);

    // "wayland" when WAYLAND_DISPLAY is set or XDG_SESSION_TYPE is "wayland", else "x11". On a
    // Wayland desktop the editor's X display is XWayland, which passes IsAvailable, but whether
    // XTEST events, pointer grabs and confinement behave there is unverified (compositor-mediated).
    static FString SessionTypeFor(const FString& WaylandDisplay, const FString& XdgSessionType);

    // SessionTypeFor this editor's own environment.
    static FString SessionType();
};
