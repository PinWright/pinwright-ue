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

    // The six Xlib/XTEST entry points this file calls, resolved once. The display
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

        if (!OpenDisplay || !Api.Flush || !Api.DefaultRootWindow || !Api.QueryPointer
            || !Api.FakeMotionEvent || !Api.FakeButtonEvent)
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
        }
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
