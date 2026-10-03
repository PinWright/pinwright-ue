// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveOsInput.h"

#include "Handlers/Drive/DriveOsInputWindows.h"
#include "Handlers/ErrorCodes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

#if PLATFORM_LINUX
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <unistd.h>
#endif

// File-local helpers live in a uniquely-named namespace (not an anonymous one): the
// plugin's Unity build merges translation units, and a distinct namespace keeps these
// from colliding with same-named helpers in sibling drive units.
namespace DriveOsInputLocal
{
    // Timings ported verbatim from the scratch XTEST injector that reproduced the
    // CommonUI capture bug where Slate injection could not. They are what makes the
    // gesture look like a hand to SDL: 24 motion steps ~8 ms apart, a pause before the
    // press, a real ~80 ms button hold, and a pause after the release so the app has
    // pumped the whole sequence before the settle loop samples it.
    constexpr int32 MotionSteps = 24;

    void Fail(FDriveInjectFailure& Out, const TCHAR* Code, const FString& Message,
        TSharedPtr<FJsonObject> Details = nullptr)
    {
        Out.Code = Code;
        Out.Message = Message;
        Out.Details = MoveTemp(Details);
    }

#if PLATFORM_LINUX

    // The pacing half of those timings; X11-only, so they sit inside the guard where the
    // only code that sleeps on them lives.
    constexpr int32 MotionStepMs = 8;
    constexpr int32 MotionSettleMs = 60;
    constexpr int32 PressHoldMs = 80;
    constexpr int32 ReleaseSettleMs = 120;

    // Opaque Xlib types: this file never dereferences either, so it needs no X11 headers
    // and therefore no include path or link dependency.
    using XDisplay = void*;
    using XWindow = unsigned long;
    using XAtom = unsigned long;
    using XErrorHandler = int (*)(XDisplay, void*);

    // Deepest window nesting the ownership descent follows; real stacks are 1-3 deep.
    constexpr int32 MaxDescent = 16;

    // The Xlib/XTEST entry points this file calls, resolved once. The display
    // connection is opened once and deliberately never closed: it is one socket held for
    // the editor's lifetime, and reopening it per click would cost more than it saves.
    struct FX11Api
    {
        XDisplay Display = nullptr;
        int (*Flush)(XDisplay) = nullptr;
        XWindow (*DefaultRootWindow)(XDisplay) = nullptr;
        int (*QueryPointer)(XDisplay, XWindow, XWindow*, XWindow*, int*, int*, int*, int*, unsigned int*) = nullptr;
        int (*FakeMotionEvent)(XDisplay, int, int, int, unsigned long) = nullptr;
        int (*FakeButtonEvent)(XDisplay, unsigned int, int, unsigned long) = nullptr;
        int (*TranslateCoordinates)(XDisplay, XWindow, XWindow, int, int, int*, int*, XWindow*) = nullptr;
        int (*GetWindowProperty)(XDisplay, XWindow, XAtom, long, long, int, XAtom,
            XAtom*, int*, unsigned long*, unsigned long*, unsigned char**) = nullptr;
        int (*FetchName)(XDisplay, XWindow, char**) = nullptr;
        int (*Free)(void*) = nullptr;
        XErrorHandler (*SetErrorHandler)(XErrorHandler) = nullptr;
        int (*GrabPointer)(XDisplay, XWindow, int, unsigned int, int, int, XWindow, unsigned long, unsigned long) = nullptr;
        int (*UngrabPointer)(XDisplay, unsigned long) = nullptr;
        int (*GrabKeyboard)(XDisplay, XWindow, int, int, int, unsigned long) = nullptr;
        int (*UngrabKeyboard)(XDisplay, unsigned long) = nullptr;
        XAtom NetWmPid = 0;

        // Caller-facing reason the API is unusable; empty once Display is open.
        FString Error;
        bool bLoaded = false;

        bool IsValid() const { return Display != nullptr; }
    };

    void LoadApi(FX11Api& Api)
    {
        void* Xlib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
        void* Xtst = dlopen("libXtst.so.6", RTLD_LAZY | RTLD_LOCAL);
        if (!Xlib || !Xtst)
        {
            Api.Error = TEXT("OS input needs libX11.so.6 and libXtst.so.6; one of them could not be loaded (is this an X11 session?).");
            return;
        }

        XDisplay (*OpenDisplay)(const char*) = reinterpret_cast<XDisplay (*)(const char*)>(dlsym(Xlib, "XOpenDisplay"));
        Api.Flush = reinterpret_cast<decltype(Api.Flush)>(dlsym(Xlib, "XFlush"));
        Api.DefaultRootWindow = reinterpret_cast<decltype(Api.DefaultRootWindow)>(dlsym(Xlib, "XDefaultRootWindow"));
        Api.QueryPointer = reinterpret_cast<decltype(Api.QueryPointer)>(dlsym(Xlib, "XQueryPointer"));
        Api.FakeMotionEvent = reinterpret_cast<decltype(Api.FakeMotionEvent)>(dlsym(Xtst, "XTestFakeMotionEvent"));
        Api.FakeButtonEvent = reinterpret_cast<decltype(Api.FakeButtonEvent)>(dlsym(Xtst, "XTestFakeButtonEvent"));
        Api.TranslateCoordinates = reinterpret_cast<decltype(Api.TranslateCoordinates)>(dlsym(Xlib, "XTranslateCoordinates"));
        Api.GetWindowProperty = reinterpret_cast<decltype(Api.GetWindowProperty)>(dlsym(Xlib, "XGetWindowProperty"));
        Api.FetchName = reinterpret_cast<decltype(Api.FetchName)>(dlsym(Xlib, "XFetchName"));
        Api.Free = reinterpret_cast<decltype(Api.Free)>(dlsym(Xlib, "XFree"));
        Api.SetErrorHandler = reinterpret_cast<decltype(Api.SetErrorHandler)>(dlsym(Xlib, "XSetErrorHandler"));
        Api.GrabPointer = reinterpret_cast<decltype(Api.GrabPointer)>(dlsym(Xlib, "XGrabPointer"));
        Api.UngrabPointer = reinterpret_cast<decltype(Api.UngrabPointer)>(dlsym(Xlib, "XUngrabPointer"));
        Api.GrabKeyboard = reinterpret_cast<decltype(Api.GrabKeyboard)>(dlsym(Xlib, "XGrabKeyboard"));
        Api.UngrabKeyboard = reinterpret_cast<decltype(Api.UngrabKeyboard)>(dlsym(Xlib, "XUngrabKeyboard"));
        XAtom (*InternAtom)(XDisplay, const char*, int) = reinterpret_cast<XAtom (*)(XDisplay, const char*, int)>(dlsym(Xlib, "XInternAtom"));

        if (!OpenDisplay || !Api.Flush || !Api.DefaultRootWindow || !Api.QueryPointer
            || !Api.FakeMotionEvent || !Api.FakeButtonEvent || !Api.TranslateCoordinates
            || !Api.GetWindowProperty || !Api.FetchName || !Api.Free || !Api.SetErrorHandler || !InternAtom
            || !Api.GrabPointer || !Api.UngrabPointer || !Api.GrabKeyboard || !Api.UngrabKeyboard)
        {
            Api.Error = TEXT("libX11 / libXtst loaded but an expected symbol is missing; OS input is unavailable.");
            return;
        }

        // NULL reads DISPLAY from the editor's own environment, so the events go to the
        // display the editor is rendering to.
        Api.Display = OpenDisplay(nullptr);
        if (!Api.Display)
        {
            Api.Error = TEXT("XOpenDisplay(NULL) failed: this process has no X display (DISPLAY unset, or access denied).");
            return;
        }
        Api.NetWmPid = InternAtom(Api.Display, "_NET_WM_PID", /*only_if_exists*/ 0);
    }

    FX11Api& GetApi()
    {
        static FX11Api Api;
        if (!Api.bLoaded)
        {
            Api.bLoaded = true;
            LoadApi(Api);
        }
        return Api;
    }

    // Flush the queued events to the server, then wait: the pacing IS the point, a burst
    // the server delivers in one go is not what a moving hand produces.
    void FlushAndWait(FX11Api& Api, int32 Ms)
    {
        Api.Flush(Api.Display);
        FPlatformProcess::Sleep(static_cast<float>(Ms) / 1000.0f);
    }

    bool QueryPointerPos(FX11Api& Api, FIntPoint& Out)
    {
        const XWindow Root = Api.DefaultRootWindow(Api.Display);
        XWindow RootReturn = 0;
        XWindow ChildReturn = 0;
        int RootX = 0;
        int RootY = 0;
        int WinX = 0;
        int WinY = 0;
        unsigned int Mask = 0;
        if (!Api.QueryPointer(Api.Display, Root, &RootReturn, &ChildReturn, &RootX, &RootY, &WinX, &WinY, &Mask))
        {
            return false;
        }
        Out = FIntPoint(RootX, RootY);
        return true;
    }

    // Xlib's default error handler exits the process, and a window can vanish between the
    // descent's round trips (a splash closing). The descent runs under this handler, so a
    // BadWindow just fails that one request instead of taking the editor down.
    int IgnoreXError(XDisplay, void*)
    {
        return 0;
    }

    // A window's _NET_WM_PID, or 0 when it has none (or vanished).
    uint32 ReadPid(FX11Api& Api, XWindow Window)
    {
        XAtom Type = 0;
        int Format = 0;
        unsigned long Count = 0;
        unsigned long After = 0;
        unsigned char* Data = nullptr;
        uint32 Pid = 0;
        // 6 = XA_CARDINAL, 0 = Success. Format-32 items come back as C longs.
        if (Api.GetWindowProperty(Api.Display, Window, Api.NetWmPid, 0, 1, /*delete*/ 0, 6,
                &Type, &Format, &Count, &After, &Data) == 0 && Data)
        {
            if (Format == 32 && Count == 1)
            {
                Pid = static_cast<uint32>(*reinterpret_cast<unsigned long*>(Data));
            }
            Api.Free(Data);
        }
        return Pid;
    }

    // OS_INPUT_BUSY when another injector outlasted the wait, INPUT_FAILED when the lock file
    // itself could not be used. True when the lock is held.
    bool LockHeldOrFail(const FDriveOsInput::FDisplayLock& Lock, const FString& LockPath, FDriveInjectFailure& Out)
    {
        if (Lock.IsHeld())
        {
            return true;
        }
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("lock_file"), LockPath);
        Details->SetNumberField(TEXT("holder_pid"), Lock.HolderPid);
        Fail(Out, Lock.bTimedOut ? ErrorCodes::ERR_OS_INPUT_BUSY : ErrorCodes::ERR_INPUT_FAILED, Lock.Error, Details);
        return false;
    }

    // Whether another X client holds the pointer (or keyboard) grab: try to take it on the
    // root from this connection and give it straight back. A successful probe costs the
    // window under the pointer a NotifyGrab/NotifyUngrab crossing (or focus) pair, which SDL
    // ignores. GrabModeAsync = 1, CurrentTime = 0, GrabSuccess = 0.
    bool IsGrabbedByAnother(FX11Api& Api, bool bPointer)
    {
        const XWindow Root = Api.DefaultRootWindow(Api.Display);
        const int Status = bPointer
            ? Api.GrabPointer(Api.Display, Root, /*owner_events*/ 0, /*event_mask*/ 0, 1, 1, /*confine_to*/ 0, /*cursor*/ 0, 0)
            : Api.GrabKeyboard(Api.Display, Root, /*owner_events*/ 0, 1, 1, 0);
        if (Status == 0)
        {
            bPointer ? Api.UngrabPointer(Api.Display, 0) : Api.UngrabKeyboard(Api.Display, 0);
            Api.Flush(Api.Display);
        }
        // AlreadyGrabbed = 1, GrabFrozen = 4. GrabNotViewable / GrabInvalidTime cannot
        // happen for the root at CurrentTime.
        return Status == 1 || Status == 4;
    }

    // SDL's own view: a window grab, or relative mouse mode on the input-focus window (SDL
    // implements that as a confining XGrabPointer). Resolved from the process's global scope,
    // where ApplicationCore exports SDL; unset when an entry point is missing (an older SDL).
    TOptional<bool> SdlHoldsGrab()
    {
        using FGetWindow = void* (*)();
        using FGetRelative = bool (*)(void*);
        static const FGetWindow GetGrabbedWindow = reinterpret_cast<FGetWindow>(dlsym(RTLD_DEFAULT, "SDL_GetGrabbedWindow"));
        static const FGetWindow GetKeyboardFocus = reinterpret_cast<FGetWindow>(dlsym(RTLD_DEFAULT, "SDL_GetKeyboardFocus"));
        static const FGetRelative GetRelative = reinterpret_cast<FGetRelative>(dlsym(RTLD_DEFAULT, "SDL_GetWindowRelativeMouseMode"));
        if (!GetGrabbedWindow || !GetKeyboardFocus || !GetRelative)
        {
            return {};
        }
        void* Focus = GetKeyboardFocus();
        return GetGrabbedWindow() != nullptr || (Focus && GetRelative(Focus));
    }

    // Under a pointer grab every real event goes to the grabber: motion is clamped to its
    // confine window and a press never reaches the target, with no error anywhere. True,
    // filling OutFailure with POINTER_GRABBED, when another client holds one.
    bool RefuseIfPointerGrabbed(FX11Api& Api, FDriveInjectFailure& OutFailure)
    {
        if (!IsGrabbedByAnother(Api, /*bPointer*/ true))
        {
            return false;
        }
        const TOptional<bool> bOurs = SdlHoldsGrab();
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        if (bOurs.IsSet())
        {
            Details->SetBoolField(TEXT("held_by_this_editor"), bOurs.GetValue());
        }
        else
        {
            Details->SetField(TEXT("held_by_this_editor"), MakeShared<FJsonValueNull>());
        }
        Fail(OutFailure, ErrorCodes::ERR_POINTER_GRABBED,
            FString::Printf(TEXT("Another X client holds an active pointer grab on this display (%s), so real input would go to the grabber, not the target. Nothing was moved or pressed. %s"),
                !bOurs.IsSet() ? TEXT("this editor's SDL state is unreadable")
                    : bOurs.GetValue() ? TEXT("this editor's SDL holds it: a captured game viewport in relative mouse mode")
                    : TEXT("not this editor's SDL"),
                bOurs.Get(false)
                    ? TEXT("Release the game's mouse capture (Shift+F1 with X focus on the editor, or stop PIE), or use the Slate path (omit os_input).")
                    : TEXT("Find the holder with `xdotool key XF86LogGrabInfo` (written to the Xorg log) and release it there.")),
            Details);
        return true;
    }

    // The motion half of MoveTo, for a caller that already holds the display lock.
    bool MoveUnlocked(const FVector2D& ScreenPos, FDriveInjectFailure& OutFailure)
    {
        FX11Api& Api = GetApi();
        if (!Api.IsValid())
        {
            Fail(OutFailure, ErrorCodes::ERR_INPUT_FAILED, Api.Error);
            return false;
        }

        if (RefuseIfPointerGrabbed(Api, OutFailure))
        {
            return false;
        }

        FIntPoint From;
        if (!QueryPointerPos(Api, From))
        {
            Fail(OutFailure, ErrorCodes::ERR_INPUT_FAILED,
                TEXT("XQueryPointer failed; the pointer position could not be read, so no motion path was sent."));
            return false;
        }

        const FIntPoint To(FMath::RoundToInt(ScreenPos.X), FMath::RoundToInt(ScreenPos.Y));
        for (const FIntPoint& Step : FDriveOsInput::ComputeMotionPath(From, To))
        {
            // Screen -1 = the screen the pointer is already on; 0 = CurrentTime.
            Api.FakeMotionEvent(Api.Display, -1, Step.X, Step.Y, 0);
            FlushAndWait(Api, MotionStepMs);
        }
        FlushAndWait(Api, MotionSettleMs);
        return true;
    }

    FString ReadTitle(FX11Api& Api, XWindow Window)
    {
        char* Name = nullptr;
        FString Title;
        if (Api.FetchName(Api.Display, Window, &Name) && Name)
        {
            Title = UTF8_TO_TCHAR(Name);
            Api.Free(Name);
        }
        return Title;
    }

#endif // PLATFORM_LINUX
}

using namespace DriveOsInputLocal;

// ────────────────────────────────────────────────────────────────────────────
// Pure helpers (no X11 dependency)
// ────────────────────────────────────────────────────────────────────────────

TArray<FIntPoint> FDriveOsInput::ComputeMotionPath(const FIntPoint& From, const FIntPoint& To)
{
    TArray<FIntPoint> Path;
    Path.Reserve(MotionSteps);
    for (int32 Step = 1; Step <= MotionSteps; ++Step)
    {
        // Integer interpolation against the ORIGINAL endpoints (not the previous step),
        // so rounding cannot drift and the last point is exactly To.
        Path.Add(FIntPoint(
            From.X + (To.X - From.X) * Step / MotionSteps,
            From.Y + (To.Y - From.Y) * Step / MotionSteps));
    }
    return Path;
}

bool FDriveOsInput::IsPointOwnedBy(const TArray<uint32>& PathPids, uint32 SelfPid, int32& OutOwnerIndex)
{
    OutOwnerIndex = PathPids.FindLastByPredicate([](uint32 Pid) { return Pid != 0; });
    return OutOwnerIndex != INDEX_NONE && PathPids[OutOwnerIndex] == SelfPid;
}

FString FDriveOsInput::SessionTypeFor(const FString& WaylandDisplay, const FString& XdgSessionType)
{
    return !WaylandDisplay.IsEmpty() || XdgSessionType.Equals(TEXT("wayland"), ESearchCase::IgnoreCase)
        ? TEXT("wayland")
        : TEXT("x11");
}

FString FDriveOsInput::SessionType()
{
    return SessionTypeFor(FPlatformMisc::GetEnvironmentVariable(TEXT("WAYLAND_DISPLAY")),
        FPlatformMisc::GetEnvironmentVariable(TEXT("XDG_SESSION_TYPE")));
}

FString FDriveOsInput::LockPathFor(const FString& Display, uint32 Uid)
{
    // "host:display.screen": everything after the last ':' up to the '.' names the display.
    FString Key = Display;
    int32 Colon = INDEX_NONE;
    if (Key.FindLastChar(TEXT(':'), Colon))
    {
        const int32 Dot = Key.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromStart, Colon);
        if (Dot != INDEX_NONE)
        {
            Key.LeftInline(Dot);
        }
    }
    for (int32 Index = 0; Index < Key.Len(); ++Index)
    {
        if (!FChar::IsAlnum(Key[Index]))
        {
            Key[Index] = TEXT('_');
        }
    }
    return FString::Printf(TEXT("/tmp/pinwright-os-input-%u-%s.lock"), Uid, *Key);
}

int32 FDriveOsInput::ButtonToXButton(EDriveMouseButton Button)
{
    // X numbers the middle button 2 and the right button 3 — the opposite order from
    // EDriveMouseButton, so this mapping is never an identity shift.
    switch (Button)
    {
        case EDriveMouseButton::Middle: return 2;
        case EDriveMouseButton::Right:  return 3;
        case EDriveMouseButton::Left:
        default:                        return 1;
    }
}

int32 FDriveOsInput::NormalizeVirtualDeskCoord(int32 Pixel, int32 Origin, int32 Extent)
{
    if (Extent <= 0)
    {
        return 0;
    }
    // Windows maps a normalized n back to Origin + (n * Extent) >> 16, so the smallest n that
    // lands on Pixel is ceil((Pixel - Origin) * 65536 / Extent). The naive (Pixel - Origin) *
    // 65535 / Extent rounds down and lands one pixel short across most of the desktop.
    const int64 Offset = FMath::Clamp<int64>(static_cast<int64>(Pixel) - Origin, 0, Extent - 1);
    return static_cast<int32>(FMath::Min<int64>((Offset * 65536 + Extent - 1) / Extent, 65535));
}

const TCHAR* FDriveOsInput::InputPathLabel()
{
#if PLATFORM_WINDOWS
    return TEXT("os_win32");
#else
    return TEXT("os_x11");
#endif
}

// ────────────────────────────────────────────────────────────────────────────
// Injection
// ────────────────────────────────────────────────────────────────────────────

bool FDriveOsInput::IsAvailable(FString& OutError)
{
#if PLATFORM_LINUX
    FX11Api& Api = GetApi();
    OutError = Api.Error;
    return Api.IsValid();
#elif PLATFORM_WINDOWS
    return DriveOsInputWindows::IsAvailable(OutError);
#else
    OutError = TEXT("os_input is Linux/X11 and Windows only (it injects through XTEST / SendInput); this editor runs on neither. Omit os_input to use the Slate injection path.");
    return false;
#endif
}

bool FDriveOsInput::FindForeignWindowAt(const FVector2D& ScreenPos, FForeignWindow& Out)
{
#if PLATFORM_LINUX
    FX11Api& Api = GetApi();
    if (!Api.IsValid())
    {
        return false;
    }

    // Walk down the window tree through every mapped window containing the point, top of
    // the stacking order first at each level: the same windows the X server would route a
    // real press through, found BEFORE anything is injected.
    const XWindow Root = Api.DefaultRootWindow(Api.Display);
    const int X = FMath::RoundToInt(ScreenPos.X);
    const int Y = FMath::RoundToInt(ScreenPos.Y);
    TArray<XWindow> Path;
    TArray<uint32> Pids;
    const XErrorHandler Previous = Api.SetErrorHandler(&IgnoreXError);
    XWindow Parent = Root;
    for (int32 Depth = 0; Depth < MaxDescent; ++Depth)
    {
        int ChildX = 0;
        int ChildY = 0;
        XWindow Child = 0;
        if (!Api.TranslateCoordinates(Api.Display, Root, Parent, X, Y, &ChildX, &ChildY, &Child) || Child == 0)
        {
            break;
        }
        Path.Add(Child);
        Pids.Add(ReadPid(Api, Child));
        Parent = Child;
    }

    int32 OwnerIndex = INDEX_NONE;
    const bool bOwned = IsPointOwnedBy(Pids, FPlatformProcess::GetCurrentProcessId(), OwnerIndex);
    if (!bOwned)
    {
        const XWindow Occluder = OwnerIndex != INDEX_NONE ? Path[OwnerIndex] : (Path.Num() > 0 ? Path[0] : Root);
        Out.WindowId = Occluder;
        Out.Pid = OwnerIndex != INDEX_NONE ? Pids[OwnerIndex] : 0;
        Out.Title = ReadTitle(Api, Occluder);
    }
    Api.SetErrorHandler(Previous);
    return !bOwned;
#elif PLATFORM_WINDOWS
    return DriveOsInputWindows::FindForeignWindowAt(ScreenPos, Out);
#else
    (void)ScreenPos;
    (void)Out;
    return false;
#endif
}

bool FDriveOsInput::ProbeGrabs(FGrabState& Out)
{
#if PLATFORM_LINUX
    FX11Api& Api = GetApi();
    if (!Api.IsValid())
    {
        return false;
    }
    Out.bPointerGrabbed = IsGrabbedByAnother(Api, /*bPointer*/ true);
    Out.bKeyboardGrabbed = IsGrabbedByAnother(Api, /*bPointer*/ false);
    Out.bHeldByThisEditor = SdlHoldsGrab();
    return true;
#else
    (void)Out;
    return false;
#endif
}

FString FDriveOsInput::DisplayLockPath()
{
#if PLATFORM_LINUX
    return LockPathFor(FPlatformMisc::GetEnvironmentVariable(TEXT("DISPLAY")), static_cast<uint32>(getuid()));
#elif PLATFORM_WINDOWS
    return DriveOsInputWindows::LockName();
#else
    return FString();
#endif
}

FDriveOsInput::FDisplayLock::FDisplayLock(const FString& Path, double TimeoutSeconds)
{
#if PLATFORM_LINUX
    // O_NOFOLLOW: /tmp is a shared directory.
    Fd = open(TCHAR_TO_UTF8(*Path), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (Fd < 0)
    {
        Error = FString::Printf(TEXT("The os_input lock file '%s' could not be opened (errno %d); nothing was injected."), *Path, errno);
        return;
    }

    // Poll rather than block in flock: a blocking flock cannot time out.
    const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
    while (flock(Fd, LOCK_EX | LOCK_NB) != 0)
    {
        if (errno != EWOULDBLOCK && errno != EINTR)
        {
            Error = FString::Printf(TEXT("flock on the os_input lock file '%s' failed (errno %d); nothing was injected."), *Path, errno);
            close(Fd);
            Fd = -1;
            return;
        }
        if (FPlatformTime::Seconds() >= Deadline)
        {
            char Buf[16] = {};
            if (pread(Fd, Buf, sizeof(Buf) - 1, 0) > 0)
            {
                HolderPid = static_cast<uint32>(strtoul(Buf, nullptr, 10));
            }
            close(Fd);
            Fd = -1;
            bTimedOut = true;
            Error = FString::Printf(
                TEXT("Another os_input injection on this X display (pid %u) held the pointer lock '%s' for more than %.1f s, so nothing was injected. Retry once it finishes; a stuck holder releases the lock when its process exits."),
                HolderPid, *Path, TimeoutSeconds);
            return;
        }
        FPlatformProcess::Sleep(0.01f);
    }

    char Buf[16];
    const int Len = snprintf(Buf, sizeof(Buf), "%d\n", static_cast<int>(getpid()));
    // Best effort: a missing pid only makes a peer's timeout message say "pid 0".
    if (ftruncate(Fd, 0) == 0)
    {
        (void)!pwrite(Fd, Buf, Len, 0);
    }
#elif PLATFORM_WINDOWS
    Mutex = DriveOsInputWindows::AcquireLock(Path, TimeoutSeconds, bTimedOut, Error);
#else
    (void)Path;
    (void)TimeoutSeconds;
    Error = TEXT("The os_input display lock is Linux-only.");
#endif
}

FDriveOsInput::FDisplayLock::~FDisplayLock()
{
#if PLATFORM_LINUX
    if (Fd >= 0)
    {
        // Closing the last descriptor of the open file releases the flock.
        close(Fd);
    }
#elif PLATFORM_WINDOWS
    DriveOsInputWindows::ReleaseLock(Mutex);
#endif
}

bool FDriveOsInput::IsPointerAt(const FIntPoint& Target, FIntPoint& OutPointer)
{
#if PLATFORM_LINUX
    FX11Api& Api = GetApi();
    return Api.IsValid() && QueryPointerPos(Api, OutPointer) && OutPointer == Target;
#elif PLATFORM_WINDOWS
    return DriveOsInputWindows::IsPointerAt(Target, OutPointer);
#else
    (void)Target;
    (void)OutPointer;
    return false;
#endif
}

bool FDriveOsInput::MoveTo(const FVector2D& ScreenPos, FDriveInjectFailure& OutFailure)
{
    // A Slate-path hover hold forces the inactive-input flag; real input must not run under it.
    FDriveInput::ReleaseHoverHold();
#if PLATFORM_LINUX
    const FString LockPath = DisplayLockPath();
    const FDisplayLock Lock(LockPath, LockTimeoutSeconds);
    return LockHeldOrFail(Lock, LockPath, OutFailure) && MoveUnlocked(ScreenPos, OutFailure);
#elif PLATFORM_WINDOWS
    return DriveOsInputWindows::MoveTo(ScreenPos, OutFailure);
#else
    (void)ScreenPos;
    FString Error;
    IsAvailable(Error);
    Fail(OutFailure, ErrorCodes::ERR_INPUT_FAILED, Error);
    return false;
#endif
}

bool FDriveOsInput::ClickAt(const FVector2D& ScreenPos, EDriveMouseButton Button, FDriveInjectFailure& OutFailure)
{
    // A Slate-path hover hold forces the inactive-input flag; real input must not run under it.
    FDriveInput::ReleaseHoverHold();
#if PLATFORM_LINUX
    // One lock across motion, checks, press and release: no peer injector may move the
    // shared pointer between our motion and our press, or during the hold.
    const FString LockPath = DisplayLockPath();
    const FDisplayLock Lock(LockPath, LockTimeoutSeconds);
    if (!LockHeldOrFail(Lock, LockPath, OutFailure) || !MoveUnlocked(ScreenPos, OutFailure))
    {
        return false;
    }

    if (!CheckPress(FIntPoint(FMath::RoundToInt(ScreenPos.X), FMath::RoundToInt(ScreenPos.Y)), OutFailure))
    {
        return false;
    }

    FX11Api& Api = GetApi();
    const unsigned int XButton = static_cast<unsigned int>(ButtonToXButton(Button));
    Api.FakeButtonEvent(Api.Display, XButton, /*is_press*/ 1, 0);
    FlushAndWait(Api, PressHoldMs);
    Api.FakeButtonEvent(Api.Display, XButton, /*is_press*/ 0, 0);
    FlushAndWait(Api, ReleaseSettleMs);
    return true;
#elif PLATFORM_WINDOWS
    return DriveOsInputWindows::ClickAt(ScreenPos, Button, OutFailure);
#else
    (void)ScreenPos;
    (void)Button;
    FString Error;
    IsAvailable(Error);
    Fail(OutFailure, ErrorCodes::ERR_INPUT_FAILED, Error);
    return false;
#endif
}

bool FDriveOsInput::CheckPress(const FIntPoint& Target, FDriveInjectFailure& OutFailure)
{
    // The motion took ~0.25 s; a window raised over the point meanwhile must not get the press.
    FForeignWindow Foreign;
    if (FindForeignWindowAt(FVector2D(Target), Foreign))
    {
        Fail(OutFailure, ErrorCodes::ERR_INPUT_FAILED,
            FString::Printf(TEXT("X window 0x%llx '%s' (pid %u) was raised over the target during the motion; the button was not pressed."),
                Foreign.WindowId, *Foreign.Title, Foreign.Pid));
        return false;
    }

    // The lock only binds PinWright injectors; a human's mouse or a raw xdotool script can
    // still move the pointer, and a pointer grab can clamp the motion short of the target.
    // XTEST presses wherever the pointer IS, so check it is on the target right before.
    FIntPoint Pointer(0, 0);
    if (!IsPointerAt(Target, Pointer))
    {
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetNumberField(TEXT("x"), Target.X);
        Details->SetNumberField(TEXT("y"), Target.Y);
        Details->SetNumberField(TEXT("pointer_x"), Pointer.X);
        Details->SetNumberField(TEXT("pointer_y"), Pointer.Y);
        Fail(OutFailure, ErrorCodes::ERR_POINTER_MOVED,
            FString::Printf(TEXT("The pointer is at (%d, %d), not on the target (%d, %d), after the motion: another X client moved it, or a pointer grab confined it. The button was not pressed."),
                Pointer.X, Pointer.Y, Target.X, Target.Y),
            Details);
        return false;
    }
    return true;
}

TSharedPtr<FDriveOsInput::FDisplayLock> FDriveOsInput::BeginGesture(FIntPoint& OutPointer, FDriveInjectFailure& OutFailure)
{
    // A Slate-path hover hold forces the inactive-input flag; real input must not run under it.
    FDriveInput::ReleaseHoverHold();
#if PLATFORM_LINUX
    const FString LockPath = DisplayLockPath();
    TSharedPtr<FDisplayLock> Lock = MakeShared<FDisplayLock>(LockPath, LockTimeoutSeconds);
    FX11Api& Api = GetApi();
    if (!LockHeldOrFail(*Lock, LockPath, OutFailure))
    {
        return nullptr;
    }
    if (!Api.IsValid())
    {
        Fail(OutFailure, ErrorCodes::ERR_INPUT_FAILED, Api.Error);
        return nullptr;
    }
    if (RefuseIfPointerGrabbed(Api, OutFailure))
    {
        return nullptr;
    }
    if (!QueryPointerPos(Api, OutPointer))
    {
        Fail(OutFailure, ErrorCodes::ERR_INPUT_FAILED,
            TEXT("XQueryPointer failed; the pointer position could not be read, so nothing was injected."));
        return nullptr;
    }
    return Lock;
#elif PLATFORM_WINDOWS
    TSharedPtr<FDisplayLock> Lock = MakeShared<FDisplayLock>(DisplayLockPath(), LockTimeoutSeconds);
    return DriveOsInputWindows::BeginGesture(*Lock, OutPointer, OutFailure) ? Lock : nullptr;
#else
    (void)OutPointer;
    FString Error;
    IsAvailable(Error);
    Fail(OutFailure, ErrorCodes::ERR_INPUT_FAILED, Error);
    return nullptr;
#endif
}

void FDriveOsInput::SendMotion(const FIntPoint& Point)
{
#if PLATFORM_LINUX
    FX11Api& Api = GetApi();
    if (Api.IsValid())
    {
        Api.FakeMotionEvent(Api.Display, -1, Point.X, Point.Y, 0);
        Api.Flush(Api.Display);
    }
#elif PLATFORM_WINDOWS
    DriveOsInputWindows::SendMotion(Point);
#else
    (void)Point;
#endif
}

void FDriveOsInput::SendButton(EDriveMouseButton Button, bool bPress)
{
#if PLATFORM_LINUX
    FX11Api& Api = GetApi();
    if (Api.IsValid())
    {
        Api.FakeButtonEvent(Api.Display, static_cast<unsigned int>(ButtonToXButton(Button)), bPress ? 1 : 0, 0);
        Api.Flush(Api.Display);
    }
#elif PLATFORM_WINDOWS
    DriveOsInputWindows::SendButton(Button, bPress);
#else
    (void)Button;
    (void)bPress;
#endif
}
