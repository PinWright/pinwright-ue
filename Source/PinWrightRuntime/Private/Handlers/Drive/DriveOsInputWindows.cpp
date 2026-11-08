// Copyright (c) 2026 Alexander Penkin. MIT License.

// Win32 SendInput backend for drive.click / drive.hover os_input. See DriveOsInputWindows.h.
//
// NOT YET COMPILED OR RUN ON WINDOWS: written on a Linux host against the Win32 docs and the
// UE 5.8 Windows application sources (F-drive-os-input-windows). The board ticket lists what a
// Windows tester must verify.
//
// Coordinates and DPI. Slate's absolute coordinates on Windows are virtual-desktop pixels in
// the process's DPI-awareness space; the editor is per-monitor DPI aware, so they are physical
// pixels. GetSystemMetrics(SM_*VIRTUALSCREEN), GetCursorPos and WindowFromPoint answer in the
// calling thread's awareness space, which is the same one, so no DPI conversion is applied: a
// conversion here would be exactly the 1707x960-vs-2560x1440 trap of a DPI-unaware injector.
// Secondary monitors left of / above the primary have negative origins; the normalization is
// relative to SM_XVIRTUALSCREEN / SM_YVIRTUALSCREEN, so they are covered.

#include "Handlers/Drive/DriveOsInputWindows.h"

#if PLATFORM_WINDOWS

#include "Handlers/ErrorCodes.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "CoreGlobals.h"
#include "Misc/Parse.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/HideWindowsPlatformTypes.h"

// Uniquely named (not anonymous, and no using-directive) so the plugin's Unity build cannot
// collide these with DriveOsInputLocal's helpers in DriveOsInput.cpp.
namespace DriveOsInputWindowsLocal
{
    // The same pacing as the X11 path in DriveOsInput.cpp: 24 motion steps ~8 ms apart, a pause
    // before the press, an ~80 ms hold, a pause after the release.
    constexpr int32 MotionStepMs = 8;
    constexpr int32 MotionSettleMs = 60;
    constexpr int32 PressHoldMs = 80;
    constexpr int32 ReleaseSettleMs = 120;

    void WinFail(FDriveInjectFailure& Out, const TCHAR* Code, const FString& Message,
        TSharedPtr<FJsonObject> Details = nullptr)
    {
        Out.Code = Code;
        Out.Message = Message;
        Out.Details = MoveTemp(Details);
    }

    // Sleep, then pump this thread's Win32 queue. In the editor's main loop Windows messages are
    // processed as they are pumped, not deferred (GPumpingMessagesOutsideOfMainLoop is false
    // there, see FWindowsApplication::DeferMessage), so every pumped WM_MOUSEMOVE / WM_INPUT
    // reaches Slate as its own event, paced. Without the pump the game thread sleeps through the
    // whole gesture and Windows coalesces the queued WM_MOUSEMOVEs into one: the app would see
    // one teleport, which is what the hand-like path exists to avoid.
    void PacedWait(int32 Ms)
    {
        FPlatformProcess::Sleep(static_cast<float>(Ms) / 1000.0f);
        if (IsInGameThread() && FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().PumpMessages();
        }
    }

    POINT ToPoint(const FVector2D& ScreenPos)
    {
        POINT Point;
        Point.x = FMath::RoundToInt(ScreenPos.X);
        Point.y = FMath::RoundToInt(ScreenPos.Y);
        return Point;
    }

    // The top-level window real pointer input at Point would go to (nullptr off every monitor).
    HWND RootWindowAt(const POINT& Point)
    {
        const HWND Hit = ::WindowFromPoint(Point);
        return Hit ? ::GetAncestor(Hit, GA_ROOT) : nullptr;
    }

    uint32 WindowPid(HWND Window)
    {
        ::DWORD Pid = 0;
        if (Window)
        {
            ::GetWindowThreadProcessId(Window, &Pid);
        }
        return static_cast<uint32>(Pid);
    }

    FString WindowTitle(HWND Window)
    {
        WCHAR Buffer[256] = {};
        if (Window)
        {
            ::GetWindowTextW(Window, Buffer, static_cast<int>(UE_ARRAY_COUNT(Buffer)));
        }
        return FString(Buffer);
    }

    bool IsThisProcessForeground()
    {
        return WindowPid(::GetForegroundWindow()) == FPlatformProcess::GetCurrentProcessId();
    }

    // One absolute move. False when Windows inserted nothing: UIPI blocks injection while a
    // higher-integrity window is under the pointer, and the input desktop may not be ours.
    bool SendAbsoluteMove(const FIntPoint& To)
    {
        INPUT Input = {};
        Input.type = INPUT_MOUSE;
        Input.mi.dx = FDriveOsInput::NormalizeVirtualDeskCoord(To.X,
            ::GetSystemMetrics(SM_XVIRTUALSCREEN), ::GetSystemMetrics(SM_CXVIRTUALSCREEN));
        Input.mi.dy = FDriveOsInput::NormalizeVirtualDeskCoord(To.Y,
            ::GetSystemMetrics(SM_YVIRTUALSCREEN), ::GetSystemMetrics(SM_CYVIRTUALSCREEN));
        Input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        return ::SendInput(1, &Input, sizeof(INPUT)) == 1;
    }

    bool SendButtonEdge(EDriveMouseButton Button, bool bDown)
    {
        // SendInput's LEFT/RIGHT flags name PHYSICAL buttons, and Windows applies the user's
        // swap (SM_SWAPBUTTON) on top, so swap here to deliver the logical button asked for.
        const bool bSwapped = ::GetSystemMetrics(SM_SWAPBUTTON) != 0;
        if (bSwapped && Button != EDriveMouseButton::Middle)
        {
            Button = Button == EDriveMouseButton::Left ? EDriveMouseButton::Right : EDriveMouseButton::Left;
        }
        INPUT Input = {};
        Input.type = INPUT_MOUSE;
        switch (Button)
        {
            case EDriveMouseButton::Middle: Input.mi.dwFlags = bDown ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
            case EDriveMouseButton::Right:  Input.mi.dwFlags = bDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
            case EDriveMouseButton::Left:
            default:                        Input.mi.dwFlags = bDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        }
        return ::SendInput(1, &Input, sizeof(INPUT)) == 1;
    }

    // OS_INPUT_BUSY when another injector outlasted the wait, INPUT_FAILED when the mutex itself
    // could not be used. True when the lock is held.
    bool LockHeldOrFail(const FDriveOsInput::FDisplayLock& Lock, FDriveInjectFailure& Out)
    {
        if (Lock.IsHeld())
        {
            return true;
        }
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("lock_file"), FDriveOsInput::DisplayLockPath());
        // A named mutex does not record its owner.
        Details->SetNumberField(TEXT("holder_pid"), 0);
        WinFail(Out, Lock.bTimedOut ? ErrorCodes::ERR_OS_INPUT_BUSY : ErrorCodes::ERR_INPUT_FAILED, Lock.Error, Details);
        return false;
    }

    // The point must belong to this process, and this process must be the foreground one: the
    // capture / lock behavior os_input exists to reproduce needs the app active, and Windows
    // routes real input to whatever owns the point. Brings the window forward when it can
    // (FWindowsWindow::HACK_ForceToFront is the same SetForegroundWindow call). Sends nothing.
    bool EnsureOwnedAndForeground(const POINT& Point, FDriveInjectFailure& Out)
    {
        const HWND Root = RootWindowAt(Point);
        const uint32 Pid = WindowPid(Root);
        if (!Root || Pid != FPlatformProcess::GetCurrentProcessId())
        {
            TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
            Details->SetNumberField(TEXT("x"), Point.x);
            Details->SetNumberField(TEXT("y"), Point.y);
            Details->SetStringField(TEXT("occluding_window"), WindowTitle(Root));
            Details->SetNumberField(TEXT("occluding_window_id"), static_cast<double>(reinterpret_cast<UPTRINT>(Root)));
            Details->SetNumberField(TEXT("occluding_pid"), Pid);
            WinFail(Out, ErrorCodes::ERR_TARGET_OCCLUDED,
                FString::Printf(TEXT("Window '%s' (pid %u) owns (%d, %d), not this editor, so it would receive the real input. Nothing was injected."),
                    *WindowTitle(Root), Pid, static_cast<int32>(Point.x), static_cast<int32>(Point.y)),
                Details);
            return false;
        }

        if (!IsThisProcessForeground())
        {
            ::SetForegroundWindow(Root);
            PacedWait(MotionStepMs);
        }
        if (!IsThisProcessForeground())
        {
            const HWND Foreground = ::GetForegroundWindow();
            TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
            Details->SetStringField(TEXT("foreground_window"), WindowTitle(Foreground));
            Details->SetNumberField(TEXT("foreground_pid"), WindowPid(Foreground));
            WinFail(Out, ErrorCodes::ERR_FOREGROUND_LOCKED,
                FString::Printf(TEXT("Windows refused to bring this editor to the foreground (foreground lock: only the process that received the last input may change it); '%s' (pid %u) stays in front. Nothing was injected. Click or Alt+Tab to the editor once, then retry."),
                    *WindowTitle(Foreground), WindowPid(Foreground)),
                Details);
            return false;
        }
        return true;
    }

    // The motion half of MoveTo, for a caller that already holds the lock.
    bool MoveUnlocked(const FVector2D& ScreenPos, FDriveInjectFailure& Out)
    {
        POINT From;
        if (!::GetCursorPos(&From))
        {
            WinFail(Out, ErrorCodes::ERR_INPUT_FAILED,
                FString::Printf(TEXT("GetCursorPos failed (error %u): the input desktop is not this session's (locked workstation, secure desktop, disconnected session). Nothing was sent."),
                    static_cast<uint32>(::GetLastError())));
            return false;
        }

        const FIntPoint To(FMath::RoundToInt(ScreenPos.X), FMath::RoundToInt(ScreenPos.Y));
        for (const FIntPoint& Step : FDriveOsInput::ComputeMotionPath(FIntPoint(From.x, From.y), To))
        {
            if (!SendAbsoluteMove(Step))
            {
                WinFail(Out, ErrorCodes::ERR_INPUT_FAILED,
                    TEXT("SendInput inserted no event: UIPI blocks injection while a higher-integrity (elevated) window is under the pointer, or the input desktop is not this session's. The motion stopped; no button was pressed."));
                return false;
            }
            PacedWait(MotionStepMs);
        }
        PacedWait(MotionSettleMs);
        return true;
    }
}

bool DriveOsInputWindows::IsAvailable(FString& OutError)
{
    if (IsRunningCommandlet())
    {
        OutError = TEXT("os_input needs a visible editor window and this is a commandlet run. Omit os_input to use the Slate injection path.");
        return false;
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("RenderOffScreen")))
    {
        OutError = TEXT("os_input needs a visible window, and this editor runs offscreen (-RenderOffScreen: a null platform application with no real windows). Start the editor with visible:true, or omit os_input.");
        return false;
    }
    if (!FApp::CanEverRender())
    {
        OutError = TEXT("os_input needs a visible window, and this editor cannot render (-nullrhi). Start the editor with visible:true, or omit os_input.");
        return false;
    }
    if (!FSlateApplication::IsInitialized())
    {
        OutError = TEXT("os_input needs Slate's windows and FSlateApplication is not initialized.");
        return false;
    }
    if (::GetSystemMetrics(SM_CXVIRTUALSCREEN) <= 0 || ::GetSystemMetrics(SM_CYVIRTUALSCREEN) <= 0)
    {
        OutError = TEXT("os_input found no desktop to inject into (the virtual screen is empty).");
        return false;
    }
    // Locked workstation, UAC secure desktop, or a disconnected session: SendInput would drop
    // every event without an error, so refuse up front.
    const HDESK InputDesktop = ::OpenInputDesktop(0, 0, DESKTOP_READOBJECTS);
    if (!InputDesktop)
    {
        OutError = TEXT("os_input cannot reach the interactive desktop (workstation locked, a secure desktop up, or the session disconnected), so injected input would be dropped.");
        return false;
    }
    ::CloseDesktop(InputDesktop);
    OutError.Reset();
    return true;
}

bool DriveOsInputWindows::FindForeignWindowAt(const FVector2D& ScreenPos, FDriveOsInput::FForeignWindow& Out)
{
    using namespace DriveOsInputWindowsLocal;
    const HWND Root = RootWindowAt(ToPoint(ScreenPos));
    const uint32 Pid = WindowPid(Root);
    if (Root && Pid == FPlatformProcess::GetCurrentProcessId())
    {
        return false;
    }
    Out.WindowId = static_cast<uint64>(reinterpret_cast<UPTRINT>(Root));
    Out.Pid = Pid;
    Out.Title = WindowTitle(Root);
    return true;
}

FString DriveOsInputWindows::LockName()
{
    // "Local\" is per logon session, and a session has one pointer: the Win32 twin of the X11
    // per-display lock.
    return TEXT("Local\\PinWright-os-input");
}

void* DriveOsInputWindows::AcquireLock(const FString& Name, double TimeoutSeconds, bool& bOutTimedOut, FString& OutError)
{
    // ponytail: a Win32 mutex is recursive for its owning thread, so two locks taken on ONE
    // thread do not exclude each other (flock on Linux does). Every gesture takes one lock on the
    // game thread and never nests; cross-process / cross-thread exclusion is what matters.
    const HANDLE Mutex = ::CreateMutexW(nullptr, 0, *Name);
    if (!Mutex)
    {
        OutError = FString::Printf(TEXT("The os_input lock '%s' could not be opened (error %u); nothing was injected."),
            *Name, static_cast<uint32>(::GetLastError()));
        return nullptr;
    }
    const ::DWORD Wait = ::WaitForSingleObject(Mutex, static_cast<::DWORD>(FMath::Max(0.0, TimeoutSeconds) * 1000.0));
    // WAIT_ABANDONED: the previous holder exited mid-gesture; ownership passes to us.
    if (Wait == WAIT_OBJECT_0 || Wait == WAIT_ABANDONED)
    {
        return Mutex;
    }
    ::CloseHandle(Mutex);
    if (Wait == WAIT_TIMEOUT)
    {
        bOutTimedOut = true;
        OutError = FString::Printf(
            TEXT("Another os_input injection in this Windows session held the pointer lock '%s' for more than %.1f s, so nothing was injected. Retry once it finishes; a stuck holder releases the lock when its process exits."),
            *Name, TimeoutSeconds);
    }
    else
    {
        OutError = FString::Printf(TEXT("Waiting on the os_input lock '%s' failed (error %u); nothing was injected."),
            *Name, static_cast<uint32>(::GetLastError()));
    }
    return nullptr;
}

void DriveOsInputWindows::ReleaseLock(void* Mutex)
{
    if (Mutex)
    {
        ::ReleaseMutex(static_cast<HANDLE>(Mutex));
        ::CloseHandle(static_cast<HANDLE>(Mutex));
    }
}

bool DriveOsInputWindows::IsPointerAt(const FIntPoint& Target, FIntPoint& OutPointer)
{
    POINT Pointer;
    if (!::GetCursorPos(&Pointer))
    {
        return false;
    }
    OutPointer = FIntPoint(Pointer.x, Pointer.y);
    return OutPointer == Target;
}

bool DriveOsInputWindows::MoveTo(const FVector2D& ScreenPos, FDriveInjectFailure& OutFailure)
{
    using namespace DriveOsInputWindowsLocal;
    const FDriveOsInput::FDisplayLock Lock(FDriveOsInput::DisplayLockPath(), FDriveOsInput::LockTimeoutSeconds);
    return LockHeldOrFail(Lock, OutFailure) && MoveUnlocked(ScreenPos, OutFailure);
}

bool DriveOsInputWindows::ClickAt(const FVector2D& ScreenPos, EDriveMouseButton Button, FDriveInjectFailure& OutFailure)
{
    using namespace DriveOsInputWindowsLocal;

    // One lock across the checks, motion, press and release, as on X11.
    const FDriveOsInput::FDisplayLock Lock(FDriveOsInput::DisplayLockPath(), FDriveOsInput::LockTimeoutSeconds);
    const POINT Target = ToPoint(ScreenPos);
    if (!LockHeldOrFail(Lock, OutFailure)
        || !EnsureOwnedAndForeground(Target, OutFailure)
        || !MoveUnlocked(ScreenPos, OutFailure))
    {
        return false;
    }

    // The motion took ~0.25 s; a window raised over the point meanwhile must not get the press.
    FDriveOsInput::FForeignWindow Foreign;
    if (DriveOsInputWindows::FindForeignWindowAt(ScreenPos, Foreign))
    {
        WinFail(OutFailure, ErrorCodes::ERR_INPUT_FAILED,
            FString::Printf(TEXT("Window 0x%llx '%s' (pid %u) was raised over the target during the motion; the button was not pressed."),
                Foreign.WindowId, *Foreign.Title, Foreign.Pid));
        return false;
    }
    if (!IsThisProcessForeground())
    {
        WinFail(OutFailure, ErrorCodes::ERR_FOREGROUND_LOCKED,
            TEXT("Another process took the foreground during the motion; the button was not pressed."));
        return false;
    }

    // The lock binds only PinWright injectors; a human's mouse, another automation tool, or a
    // ClipCursor confinement (EMouseLockMode::LockOnCapture) can leave the pointer elsewhere, and
    // SendInput presses wherever the pointer IS.
    const FIntPoint TargetPoint(Target.x, Target.y);
    FIntPoint Pointer(0, 0);
    if (!DriveOsInputWindows::IsPointerAt(TargetPoint, Pointer))
    {
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetNumberField(TEXT("x"), TargetPoint.X);
        Details->SetNumberField(TEXT("y"), TargetPoint.Y);
        Details->SetNumberField(TEXT("pointer_x"), Pointer.X);
        Details->SetNumberField(TEXT("pointer_y"), Pointer.Y);
        WinFail(OutFailure, ErrorCodes::ERR_POINTER_MOVED,
            FString::Printf(TEXT("The pointer is at (%d, %d), not on the target (%d, %d), after the motion: another process or a human moved it, or a cursor clip (ClipCursor) confined it. The button was not pressed."),
                Pointer.X, Pointer.Y, TargetPoint.X, TargetPoint.Y),
            Details);
        return false;
    }

    if (!SendButtonEdge(Button, /*bDown*/ true))
    {
        WinFail(OutFailure, ErrorCodes::ERR_INPUT_FAILED,
            TEXT("SendInput refused the button press (UIPI, or the input desktop changed); nothing was pressed."));
        return false;
    }
    PacedWait(PressHoldMs);
    // Always attempt the release once the press went in: a stuck button breaks every later input.
    const bool bReleased = SendButtonEdge(Button, /*bDown*/ false);
    PacedWait(ReleaseSettleMs);
    if (!bReleased)
    {
        WinFail(OutFailure, ErrorCodes::ERR_INPUT_FAILED,
            TEXT("SendInput pressed the button but refused its release; the button may still be held down."));
        return false;
    }
    return true;
}

bool DriveOsInputWindows::BeginGesture(const FDriveOsInput::FDisplayLock& Lock, FIntPoint& OutPointer, FDriveInjectFailure& OutFailure)
{
    using namespace DriveOsInputWindowsLocal;
    if (!LockHeldOrFail(Lock, OutFailure))
    {
        return false;
    }
    POINT Pointer;
    if (!::GetCursorPos(&Pointer))
    {
        WinFail(OutFailure, ErrorCodes::ERR_INPUT_FAILED,
            TEXT("GetCursorPos failed: the input desktop is not this session's (locked workstation, secure desktop, disconnected session). Nothing was injected."));
        return false;
    }
    OutPointer = FIntPoint(Pointer.x, Pointer.y);
    return true;
}

// The gesture ticker runs these between engine frames, so the main loop pumps each event: no
// PacedWait here.
void DriveOsInputWindows::SendMotion(const FIntPoint& Point)
{
    DriveOsInputWindowsLocal::SendAbsoluteMove(Point);
}

void DriveOsInputWindows::SendButton(EDriveMouseButton Button, bool bPress)
{
    DriveOsInputWindowsLocal::SendButtonEdge(Button, bPress);
}

#endif // PLATFORM_WINDOWS
