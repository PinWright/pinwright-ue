// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveOsInput.h"

#include "HAL/PlatformProcess.h"

#if PLATFORM_LINUX
#include <dlfcn.h>
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
        XAtom (*InternAtom)(XDisplay, const char*, int) = reinterpret_cast<XAtom (*)(XDisplay, const char*, int)>(dlsym(Xlib, "XInternAtom"));

        if (!OpenDisplay || !Api.Flush || !Api.DefaultRootWindow || !Api.QueryPointer
            || !Api.FakeMotionEvent || !Api.FakeButtonEvent || !Api.TranslateCoordinates
            || !Api.GetWindowProperty || !Api.FetchName || !Api.Free || !Api.SetErrorHandler || !InternAtom)
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

// ────────────────────────────────────────────────────────────────────────────
// Injection
// ────────────────────────────────────────────────────────────────────────────

bool FDriveOsInput::IsAvailable(FString& OutError)
{
#if PLATFORM_LINUX
    FX11Api& Api = GetApi();
    OutError = Api.Error;
    return Api.IsValid();
#else
    OutError = TEXT("os_input is Linux/X11-only (it injects through XTEST); this editor is not running on Linux. Omit os_input to use the Slate injection path.");
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
#else
    (void)ScreenPos;
    (void)Out;
    return false;
#endif
}

bool FDriveOsInput::MoveTo(const FVector2D& ScreenPos, FString& OutError)
{
#if PLATFORM_LINUX
    FX11Api& Api = GetApi();
    if (!Api.IsValid())
    {
        OutError = Api.Error;
        return false;
    }

    FIntPoint From;
    if (!QueryPointerPos(Api, From))
    {
        OutError = TEXT("XQueryPointer failed; the pointer position could not be read, so no motion path was sent.");
        return false;
    }

    const FIntPoint To(FMath::RoundToInt(ScreenPos.X), FMath::RoundToInt(ScreenPos.Y));
    for (const FIntPoint& Step : ComputeMotionPath(From, To))
    {
        // Screen -1 = the screen the pointer is already on; 0 = CurrentTime.
        Api.FakeMotionEvent(Api.Display, -1, Step.X, Step.Y, 0);
        FlushAndWait(Api, MotionStepMs);
    }
    FlushAndWait(Api, MotionSettleMs);
    return true;
#else
    (void)ScreenPos;
    return IsAvailable(OutError);
#endif
}

bool FDriveOsInput::ClickAt(const FVector2D& ScreenPos, EDriveMouseButton Button, FString& OutError)
{
#if PLATFORM_LINUX
    if (!MoveTo(ScreenPos, OutError))
    {
        return false;
    }

    // The motion took ~0.25 s; a window raised over the point meanwhile must not get the press.
    FForeignWindow Foreign;
    if (FindForeignWindowAt(ScreenPos, Foreign))
    {
        OutError = FString::Printf(TEXT("X window 0x%llx '%s' (pid %u) was raised over the target during the motion; the button was not pressed."),
            Foreign.WindowId, *Foreign.Title, Foreign.Pid);
        return false;
    }

    FX11Api& Api = GetApi();
    const unsigned int XButton = static_cast<unsigned int>(ButtonToXButton(Button));
    Api.FakeButtonEvent(Api.Display, XButton, /*is_press*/ 1, 0);
    FlushAndWait(Api, PressHoldMs);
    Api.FakeButtonEvent(Api.Display, XButton, /*is_press*/ 0, 0);
    FlushAndWait(Api, ReleaseSettleMs);
    return true;
#else
    (void)ScreenPos;
    (void)Button;
    return IsAvailable(OutError);
#endif
}
