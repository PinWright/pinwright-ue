// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveOsInput.h"

#include "Handlers/Drive/DriveOsInputWindows.h"
#include "Handlers/ErrorCodes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/FileManager.h"
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
        int (*Sync)(XDisplay, int) = nullptr;
        unsigned char (*KeysymToKeycode)(XDisplay, unsigned long) = nullptr;
        int (*DisplayKeycodes)(XDisplay, int*, int*) = nullptr;
        unsigned long* (*GetKeyboardMapping)(XDisplay, unsigned int, int, int*) = nullptr;
        int (*ChangeKeyboardMapping)(XDisplay, int, int, unsigned long*, int) = nullptr;
        int (*FakeKeyEvent)(XDisplay, unsigned int, int, unsigned long) = nullptr;
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
        Api.Sync = reinterpret_cast<decltype(Api.Sync)>(dlsym(Xlib, "XSync"));
        Api.KeysymToKeycode = reinterpret_cast<decltype(Api.KeysymToKeycode)>(dlsym(Xlib, "XKeysymToKeycode"));
        Api.DisplayKeycodes = reinterpret_cast<decltype(Api.DisplayKeycodes)>(dlsym(Xlib, "XDisplayKeycodes"));
        Api.GetKeyboardMapping = reinterpret_cast<decltype(Api.GetKeyboardMapping)>(dlsym(Xlib, "XGetKeyboardMapping"));
        Api.ChangeKeyboardMapping = reinterpret_cast<decltype(Api.ChangeKeyboardMapping)>(dlsym(Xlib, "XChangeKeyboardMapping"));
        Api.FakeKeyEvent = reinterpret_cast<decltype(Api.FakeKeyEvent)>(dlsym(Xtst, "XTestFakeKeyEvent"));
        XAtom (*InternAtom)(XDisplay, const char*, int) = reinterpret_cast<XAtom (*)(XDisplay, const char*, int)>(dlsym(Xlib, "XInternAtom"));

        if (!OpenDisplay || !Api.Flush || !Api.DefaultRootWindow || !Api.QueryPointer
            || !Api.FakeMotionEvent || !Api.FakeButtonEvent || !Api.TranslateCoordinates
            || !Api.GetWindowProperty || !Api.FetchName || !Api.Free || !Api.SetErrorHandler || !InternAtom
            || !Api.GrabPointer || !Api.UngrabPointer || !Api.GrabKeyboard || !Api.UngrabKeyboard
            || !Api.Sync || !Api.KeysymToKeycode || !Api.DisplayKeycodes || !Api.GetKeyboardMapping
            || !Api.ChangeKeyboardMapping || !Api.FakeKeyEvent)
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

    // The bytes a file gained past Offset, or empty when it could not be read.
    FString ReadFileFrom(const FString& Path, int64 Offset)
    {
        FILE* File = fopen(TCHAR_TO_UTF8(*Path), "rb");
        if (!File)
        {
            return FString();
        }
        TArray<ANSICHAR> Bytes;
        if (fseeko(File, Offset, SEEK_SET) == 0)
        {
            ANSICHAR Chunk[65536];
            size_t Read = 0;
            while ((Read = fread(Chunk, 1, sizeof(Chunk), File)) > 0)
            {
                Bytes.Append(Chunk, static_cast<int32>(Read));
            }
        }
        fclose(File);
        Bytes.Add('\0');
        return UTF8_TO_TCHAR(Bytes.GetData());
    }

    // The pointer-grab holder an Xorg grab dump named, or why none was named.
    struct FGrabHolder
    {
        bool bNamed = false;
        uint32 Pid = 0;
        FString Cmdline;
        FString LogPath;
        FString Reason;     // when !bNamed: what stopped the dump from naming one
        bool bAttempted = false;   // XF86LogGrabInfo was pressed (the costly part ran)
    };

    // X has no request that names a grab's owner, but XKB's XF86LogGrabInfo action makes an
    // Xorg server print every active grab, with each holder's pid and command line, to its log.
    // Press that keysym through XTEST, bound to a spare keycode for the press when the keymap
    // has none (as `xdotool key` does), and parse the dump from the log that grew. On a GNOME
    // desktop the dump stalls the X server ~0.4 s and appends ~600 KB to its log (most of it
    // the passive-grab list), and the bind and unbind make every X client reload its keymap,
    // so the caller rate-limits it.
    FGrabHolder DumpGrabHolder(FX11Api& Api)
    {
        FGrabHolder Out;
        if (FDriveOsInput::SessionType() == TEXT("wayland"))
        {
            Out.Reason = TEXT("this is a Wayland session, and XWayland writes no grab dump");
            return Out;
        }
        const TArray<FString> Logs = FDriveOsInput::XorgLogCandidates(
            FPlatformMisc::GetEnvironmentVariable(TEXT("DISPLAY")),
            FPlatformMisc::GetEnvironmentVariable(TEXT("XDG_DATA_HOME")),
            FPlatformMisc::GetEnvironmentVariable(TEXT("HOME")));
        TArray<int64> Sizes;
        for (const FString& Log : Logs)
        {
            Sizes.Add(IFileManager::Get().FileSize(*Log));
        }
        if (!Sizes.ContainsByPredicate([](int64 Size) { return Size >= 0; }))
        {
            Out.Reason = FString::Printf(TEXT("no Xorg log of this display exists (%s; an Xvfb or Xephyr server writes none)"),
                *FString::Join(Logs, TEXT(", ")));
            return Out;
        }

        // A server error must not reach Xlib's default handler, which exits the process.
        const XErrorHandler Previous = Api.SetErrorHandler(&IgnoreXError);
        constexpr unsigned long LogGrabInfoKeysym = 0x1008FE25;   // XF86XK_LogGrabInfo
        unsigned int Keycode = Api.KeysymToKeycode(Api.Display, LogGrabInfoKeysym);
        const bool bScratch = Keycode == 0;
        if (bScratch)
        {
            int Min = 0;
            int Max = 0;
            int PerKeycode = 0;
            Api.DisplayKeycodes(Api.Display, &Min, &Max);
            unsigned long* Map = Api.GetKeyboardMapping(Api.Display, static_cast<unsigned int>(Min), Max - Min + 1, &PerKeycode);
            for (int Code = Max; Map && Code >= Min && Keycode == 0; --Code)
            {
                bool bUnbound = true;
                for (int Index = 0; Index < PerKeycode; ++Index)
                {
                    bUnbound &= Map[(Code - Min) * PerKeycode + Index] == 0;
                }
                Keycode = bUnbound ? static_cast<unsigned int>(Code) : 0;
            }
            if (Map)
            {
                Api.Free(Map);
            }
            if (Keycode == 0)
            {
                Api.SetErrorHandler(Previous);
                Out.Reason = TEXT("the keymap has no spare keycode to bind XF86LogGrabInfo to");
                return Out;
            }
            unsigned long Bound[2] = { LogGrabInfoKeysym, LogGrabInfoKeysym };
            Api.ChangeKeyboardMapping(Api.Display, static_cast<int>(Keycode), 2, Bound, 1);
            Api.Sync(Api.Display, 0);
        }
        Out.bAttempted = true;
        Api.FakeKeyEvent(Api.Display, Keycode, /*is_press*/ 1, 0);
        Api.FakeKeyEvent(Api.Display, Keycode, /*is_press*/ 0, 0);
        Api.Sync(Api.Display, 0);

        // The server runs the key's action when it processes its input queue, not by the time
        // XSync returns, so poll. Only the active-grab section is needed, and it ends long
        // before the passive-grab list does.
        FString Dump;
        const double Deadline = FPlatformTime::Seconds() + 1.0;
        while (Dump.IsEmpty() && FPlatformTime::Seconds() < Deadline)
        {
            FPlatformProcess::Sleep(0.02f);
            for (int32 Index = 0; Index < Logs.Num() && Dump.IsEmpty(); ++Index)
            {
                const int64 From = FMath::Max<int64>(Sizes[Index], 0);
                if (IFileManager::Get().FileSize(*Logs[Index]) > From)
                {
                    const FString Grown = ReadFileFrom(Logs[Index], From);
                    if (Grown.Contains(TEXT("End list of active device grabs")))
                    {
                        Dump = Grown;
                        Out.LogPath = Logs[Index];
                    }
                }
            }
        }

        // Unbind only now: an unbind processed before the queued press would leave it no action.
        if (bScratch)
        {
            unsigned long Unbound[2] = { 0, 0 };
            Api.ChangeKeyboardMapping(Api.Display, static_cast<int>(Keycode), 2, Unbound, 1);
            Api.Sync(Api.Display, 0);
        }
        Api.SetErrorHandler(Previous);

        if (Dump.IsEmpty())
        {
            Out.Reason = FString::Printf(TEXT("no Xorg log of this display (%s) took the XF86LogGrabInfo dump within 1 s"),
                *FString::Join(Logs, TEXT(", ")));
        }
        else if (FDriveOsInput::ParseGrabHolder(Dump, Out.Pid, Out.Cmdline))
        {
            Out.bNamed = true;
        }
        else
        {
            Out.Reason = FString::Printf(TEXT("the X server's grab dump (%s) lists no client holding the core pointer"), *Out.LogPath);
        }
        return Out;
    }

    // Retries must not repeat the dump's stall, log growth and keymap reloads: a refusal
    // within this window of the last dump reuses its result, a failed one included.
    constexpr double GrabDumpCooldownSeconds = 30.0;

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
        FString Advice = TEXT("Release the game's mouse capture (Shift+F1 with X focus on the editor, or stop PIE), or use the Slate path (omit os_input).");
        if (!bOurs.Get(false))
        {
            // Callers hold the display lock, which serializes this cache within the process too.
            // Only a pressed dump is cached: the failures before the press cost nothing to redo.
            static FGrabHolder Cached;
            static double DumpedAt = 0.0;
            static bool bCached = false;
            FGrabHolder Holder;
            double Age = 0.0;
            if (bCached && FPlatformTime::Seconds() - DumpedAt < GrabDumpCooldownSeconds)
            {
                Holder = Cached;
                Age = FPlatformTime::Seconds() - DumpedAt;
            }
            else
            {
                Holder = DumpGrabHolder(Api);
                if (Holder.bAttempted)
                {
                    Cached = Holder;
                    DumpedAt = FPlatformTime::Seconds();
                    bCached = true;
                }
            }
            Details->SetNumberField(TEXT("holder_dump_age_s"), Age);
            const FString Ago = Age < 1.0 ? FString()
                : FString::Printf(TEXT(" This comes from a grab dump taken %.0f s ago, reused for up to %.0f s; the holder may have changed since."),
                    Age, GrabDumpCooldownSeconds);
            if (Holder.bNamed)
            {
                Details->SetNumberField(TEXT("holder_pid"), Holder.Pid);
                Details->SetStringField(TEXT("holder_cmdline"), Holder.Cmdline);
                Advice = FString::Printf(TEXT("The X server's grab dump (%s) names the holder: pid %u '%s'.%s %s"),
                    *Holder.LogPath, Holder.Pid, *Holder.Cmdline, *Ago,
                    *FDriveOsInput::GrabHolderHint(Holder.Pid, Holder.Cmdline, FPlatformProcess::GetCurrentProcessId()));
            }
            else
            {
                Details->SetField(TEXT("holder_pid"), MakeShared<FJsonValueNull>());
                Details->SetField(TEXT("holder_cmdline"), MakeShared<FJsonValueNull>());
                Advice = FString::Printf(TEXT("Its holder could not be named: %s.%s Find it on a screenshot of the display, or with `xdotool key XF86LogGrabInfo` and the X server's log, and release it there."),
                    *Holder.Reason, *Ago);
            }
        }
        Fail(OutFailure, ErrorCodes::ERR_POINTER_GRABBED,
            FString::Printf(TEXT("Another X client holds an active pointer grab on this display (%s), so real input would go to the grabber, not the target. Nothing was moved or pressed. %s"),
                !bOurs.IsSet() ? TEXT("this editor's SDL state is unreadable")
                    : bOurs.GetValue() ? TEXT("this editor's SDL holds it: a captured game viewport in relative mouse mode")
                    : TEXT("not this editor's SDL"),
                *Advice),
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

    // The mapped windows containing the point, from the root's child down, top of the stacking
    // order first at each level: the windows the X server routes a real press through. The
    // caller installs IgnoreXError, since a window can vanish between the round trips.
    TArray<XWindow> WindowPathAt(FX11Api& Api, const FVector2D& ScreenPos)
    {
        const XWindow Root = Api.DefaultRootWindow(Api.Display);
        const int X = FMath::RoundToInt(ScreenPos.X);
        const int Y = FMath::RoundToInt(ScreenPos.Y);
        TArray<XWindow> Path;
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
            Parent = Child;
        }
        return Path;
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

bool FDriveOsInput::TopChanged(const TArray<uint64>& Before, const TArray<uint64>& After)
{
    return Before.IsEmpty() || After.IsEmpty() || Before[0] != After[0];
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

TArray<FString> FDriveOsInput::XorgLogCandidates(const FString& Display, const FString& XdgDataHome, const FString& Home)
{
    // "host:display.screen" -> "display".
    int32 Colon = INDEX_NONE;
    if (!Display.FindLastChar(TEXT(':'), Colon))
    {
        return {};
    }
    FString Number = Display.Mid(Colon + 1);
    int32 Dot = INDEX_NONE;
    if (Number.FindChar(TEXT('.'), Dot))
    {
        Number.LeftInline(Dot);
    }
    if (Number.IsEmpty() || !Number.IsNumeric())
    {
        return {};
    }
    // A rootless Xorg (the GNOME default) logs under $XDG_DATA_HOME/xorg, falling back to
    // ~/.local/share/xorg; a root one under /var/log.
    const FString File = FString::Printf(TEXT("Xorg.%s.log"), *Number);
    TArray<FString> Logs;
    if (!XdgDataHome.IsEmpty())
    {
        Logs.Add(XdgDataHome / TEXT("xorg") / File);
    }
    else if (!Home.IsEmpty())
    {
        Logs.Add(Home / TEXT(".local/share/xorg") / File);
    }
    Logs.Add(FString(TEXT("/var/log")) / File);
    return Logs;
}

bool FDriveOsInput::ParseGrabHolder(const FString& Dump, uint32& OutPid, FString& OutCmdline)
{
    // "Active grab 0x200000 (xi2) on device 'Virtual core pointer' (2):", then
    // "      client pid 2134 /usr/bin/gnome-shell ". The passive grabs listed after the
    // "End list of active device grabs" line name clients too and must not match.
    TArray<FString> Lines;
    Dump.ParseIntoArrayLines(Lines);
    const FString ClientPid = TEXT("client pid ");
    for (int32 Index = 0; Index + 1 < Lines.Num(); ++Index)
    {
        if (Lines[Index].Contains(TEXT("End list of active device grabs")))
        {
            return false;
        }
        if (!Lines[Index].Contains(TEXT("Active grab ")) || !Lines[Index].Contains(TEXT("on device 'Virtual core pointer'")))
        {
            continue;
        }
        const int32 At = Lines[Index + 1].Find(ClientPid);
        if (At == INDEX_NONE)
        {
            return false;
        }
        const FString Rest = Lines[Index + 1].Mid(At + ClientPid.Len());
        int32 Digits = 0;
        while (Digits < Rest.Len() && FChar::IsDigit(Rest[Digits]))
        {
            ++Digits;
        }
        if (Digits == 0)
        {
            return false;
        }
        OutPid = static_cast<uint32>(FCString::Strtoui64(*Rest.Left(Digits), nullptr, 10));
        OutCmdline = Rest.Mid(Digits).TrimStartAndEnd();
        return true;
    }
    return false;
}

FString FDriveOsInput::GrabHolderHint(uint32 Pid, const FString& Cmdline, uint32 SelfPid)
{
    if (Pid == SelfPid)
    {
        return TEXT("That is this editor process, on an X connection other than SDL's: code inside the editor (a plugin or tool) grabbed the pointer. End that grab or restart the editor.");
    }
    if (Cmdline.Contains(TEXT("UnrealEditor")))
    {
        return TEXT("Another Unreal editor holds it: a captured game viewport in relative mouse mode. Its owner releases it with Shift+F1 or by stopping PIE; do not kill that editor.");
    }
    if (Cmdline.Contains(TEXT("gnome-shell")))
    {
        return TEXT("GNOME Shell grabs the pointer for a system-modal dialog (an 'Authentication required' keyring or polkit prompt), the Activities overview, or an open shell menu. Dismiss it (Escape, or answer the prompt) and retry.");
    }
    return TEXT("Release it in that application (close its menu, dialog or drag) and retry.");
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

    // The windows a real press at the point would be routed through, found BEFORE anything is
    // injected.
    const XWindow Root = Api.DefaultRootWindow(Api.Display);
    const XErrorHandler Previous = Api.SetErrorHandler(&IgnoreXError);
    const TArray<XWindow> Path = WindowPathAt(Api, ScreenPos);
    TArray<uint32> Pids;
    for (const XWindow Window : Path)
    {
        Pids.Add(ReadPid(Api, Window));
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

uint64 FDriveOsInput::XWindowIdOf(const TSharedPtr<FGenericWindow>& Native)
{
#if PLATFORM_LINUX
    // Linux's OS handle is the SDL_Window; SDL3 publishes its X11 window as a window property
    // (SDL_video.h, SDL_PROP_WINDOW_X11_WINDOW_NUMBER), unset under Wayland or the dummy driver.
    // Resolved from the global scope, where ApplicationCore exports SDL, as SdlHoldsGrab does.
    using FGetProps = uint32 (*)(void*);
    using FGetNumber = int64 (*)(uint32, const char*, int64);
    static const FGetProps GetProps = reinterpret_cast<FGetProps>(dlsym(RTLD_DEFAULT, "SDL_GetWindowProperties"));
    static const FGetNumber GetNumber = reinterpret_cast<FGetNumber>(dlsym(RTLD_DEFAULT, "SDL_GetNumberProperty"));
    void* Sdl = Native.IsValid() ? Native->GetOSWindowHandle() : nullptr;
    if (!Sdl || !GetProps || !GetNumber)
    {
        return 0;
    }
    const uint32 Props = GetProps(Sdl);
    return Props ? static_cast<uint64>(GetNumber(Props, "SDL.window.x11.window", 0)) : 0;
#else
    (void)Native;
    return 0;
#endif
}

TArray<uint64> FDriveOsInput::XWindowPathAt(const FVector2D& ScreenPos)
{
    TArray<uint64> Out;
#if PLATFORM_LINUX
    FX11Api& Api = GetApi();
    if (Api.IsValid())
    {
        const XErrorHandler Previous = Api.SetErrorHandler(&IgnoreXError);
        for (const XWindow Window : WindowPathAt(Api, ScreenPos))
        {
            Out.Add(static_cast<uint64>(Window));
        }
        Api.SetErrorHandler(Previous);
    }
#else
    (void)ScreenPos;
#endif
    return Out;
}

bool FDriveOsInput::IsXWindowOnTopAt(const FVector2D& ScreenPos, uint64 WindowId)
{
    return WindowId != 0 && XWindowPathAt(ScreenPos).Contains(WindowId);
}

uint64 FDriveOsInput::XInputFocus()
{
#if PLATFORM_LINUX
    FX11Api& Api = GetApi();
    using FGetFocus = int (*)(XDisplay, XWindow*, int*);
    static void* const Xlib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    static const FGetFocus GetFocus = Xlib ? reinterpret_cast<FGetFocus>(dlsym(Xlib, "XGetInputFocus")) : nullptr;
    if (!Api.IsValid() || !GetFocus)
    {
        return 0;
    }
    XWindow Focus = 0;
    int Revert = 0;
    GetFocus(Api.Display, &Focus, &Revert);
    return static_cast<uint64>(Focus);
#else
    return 0;
#endif
}

bool FDriveOsInput::FocusXWindow(uint64 WindowId)
{
#if PLATFORM_LINUX
    FX11Api& Api = GetApi();
    using FSetFocus = int (*)(XDisplay, XWindow, int, unsigned long);
    static void* const Xlib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    static const FSetFocus SetFocus = Xlib ? reinterpret_cast<FSetFocus>(dlsym(Xlib, "XSetInputFocus")) : nullptr;
    if (!Api.IsValid() || !SetFocus || WindowId == 0)
    {
        return false;
    }
    // RevertToParent = 2, CurrentTime = 0. What SDL itself does to a shown window when no window
    // manager runs (SDL_x11window.c, X11_ShowWindow).
    const XErrorHandler Previous = Api.SetErrorHandler(&IgnoreXError);
    SetFocus(Api.Display, static_cast<XWindow>(WindowId), 2, 0);
    Api.Flush(Api.Display);
    Api.SetErrorHandler(Previous);
    return true;
#else
    (void)WindowId;
    return false;
#endif
}

bool FDriveOsInput::ActivateXWindow(uint64 WindowId)
{
#if PLATFORM_LINUX
    FX11Api& Api = GetApi();
    using FIntern = XAtom (*)(XDisplay, const char*, int);
    using FSend = int (*)(XDisplay, XWindow, int, long, void*);
    static void* const Xlib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    static const FIntern Intern = Xlib ? reinterpret_cast<FIntern>(dlsym(Xlib, "XInternAtom")) : nullptr;
    static const FSend Send = Xlib ? reinterpret_cast<FSend>(dlsym(Xlib, "XSendEvent")) : nullptr;
    if (!Api.IsValid() || !Intern || !Send || WindowId == 0)
    {
        return false;
    }
    // XClientMessageEvent on LP64: type@0, send_event@16, window@32, message_type@40, format@48,
    // data.l[5]@56; the XEvent union is 192 bytes. Source 2 (pager) with CurrentTime: mutter
    // honours it, where it declines SDL's application-source request from a background editor.
    alignas(8) uint8 Event[192] = {};
    const auto Put = [&Event](int32 Offset, auto Value) { FMemory::Memcpy(Event + Offset, &Value, sizeof(Value)); };
    Put(0, int32(33));                                       // ClientMessage
    Put(16, int32(1));                                       // send_event
    Put(32, static_cast<XWindow>(WindowId));
    Put(40, Intern(Api.Display, "_NET_ACTIVE_WINDOW", /*only_if_exists*/ 0));
    Put(48, int32(32));                                      // format
    Put(56, long(2));                                        // source indication: pager
    Put(64, long(0));                                        // CurrentTime
    const XErrorHandler Previous = Api.SetErrorHandler(&IgnoreXError);
    Send(Api.Display, Api.DefaultRootWindow(Api.Display), /*propagate*/ 0,
        (1L << 20) | (1L << 19), Event);                     // SubstructureRedirect | SubstructureNotify
    Api.Flush(Api.Display);
    Api.SetErrorHandler(Previous);
    return true;
#else
    (void)WindowId;
    return false;
#endif
}

bool FDriveOsInput::HasSdlKeyboardFocus(const TSharedPtr<FGenericWindow>& Native)
{
#if PLATFORM_LINUX
    // SDL moves its keyboard focus when it pumps the X FocusIn, in the same pump the editor
    // handles it, so this says the editor has seen the focus change, not just the window manager.
    using FGetWindow = void* (*)();
    static const FGetWindow GetKeyboardFocus = reinterpret_cast<FGetWindow>(dlsym(RTLD_DEFAULT, "SDL_GetKeyboardFocus"));
    return GetKeyboardFocus && Native.IsValid() && GetKeyboardFocus() == Native->GetOSWindowHandle();
#else
    (void)Native;
    return false;
#endif
}

uint64 FDriveOsInput::XActiveWindow(bool* bOutWmReports)
{
    if (bOutWmReports)
    {
        *bOutWmReports = false;
    }
#if PLATFORM_LINUX
    FX11Api& Api = GetApi();
    using FIntern = XAtom (*)(XDisplay, const char*, int);
    static void* const Xlib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    static const FIntern Intern = Xlib ? reinterpret_cast<FIntern>(dlsym(Xlib, "XInternAtom")) : nullptr;
    if (!Api.IsValid() || !Intern)
    {
        return 0;
    }
    static const XAtom NetActiveWindow = Intern(Api.Display, "_NET_ACTIVE_WINDOW", /*only_if_exists*/ 0);
    XAtom Type = 0;
    int Format = 0;
    unsigned long Count = 0;
    unsigned long After = 0;
    unsigned char* Data = nullptr;
    uint64 Active = 0;
    // 33 = XA_WINDOW, 0 = Success. Format-32 items come back as C longs.
    if (Api.GetWindowProperty(Api.Display, Api.DefaultRootWindow(Api.Display), NetActiveWindow, 0, 1, /*delete*/ 0, 33,
            &Type, &Format, &Count, &After, &Data) == 0 && Data)
    {
        if (Format == 32 && Count == 1)
        {
            Active = static_cast<uint64>(*reinterpret_cast<unsigned long*>(Data));
        }
        Api.Free(Data);
    }
    // Type None: the root has no _NET_ACTIVE_WINDOW at all, i.e. no EWMH window manager runs
    // (a bare Xvfb / Xephyr from editor_start), as opposed to one reporting no active window.
    if (bOutWmReports)
    {
        *bOutWmReports = Type != 0;
    }
    return Active;
#else
    return 0;
#endif
}

bool FDriveOsInput::RaiseXWindow(uint64 WindowId)
{
#if PLATFORM_LINUX
    // From PinWright's own connection, so nothing sends _NET_ACTIVE_WINDOW: SDL_RaiseWindow does
    // (SDL_x11window.c X11_RaiseWindow, hint SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED), which can take
    // keyboard focus or, on GNOME, show an "is ready" notification (LinuxApplication.cpp, the engine
    // clears that hint around its own raise for this reason). A reparenting window manager receives
    // this restack as a ConfigureRequest and may decline it; whether it then still flags the window
    // as demanding attention is up to the window manager (not verified for mutter).
    FX11Api& Api = GetApi();
    using FRaise = int (*)(XDisplay, XWindow);
    static void* const Xlib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    static const FRaise Raise = Xlib ? reinterpret_cast<FRaise>(dlsym(Xlib, "XRaiseWindow")) : nullptr;
    if (!Api.IsValid() || !Raise || WindowId == 0)
    {
        return false;
    }
    const XErrorHandler Previous = Api.SetErrorHandler(&IgnoreXError);
    Raise(Api.Display, static_cast<XWindow>(WindowId));
    Api.Flush(Api.Display);
    Api.SetErrorHandler(Previous);
    return true;
#else
    (void)WindowId;
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

bool FDriveOsInput::CheckPointerFree(FDriveInjectFailure& OutFailure)
{
#if PLATFORM_LINUX
    const FString LockPath = DisplayLockPath();
    const FDisplayLock Lock(LockPath, LockTimeoutSeconds);
    if (!LockHeldOrFail(Lock, LockPath, OutFailure))
    {
        return false;
    }
    FX11Api& Api = GetApi();
    if (!Api.IsValid())
    {
        Fail(OutFailure, ErrorCodes::ERR_INPUT_FAILED, Api.Error);
        return false;
    }
    return !RefuseIfPointerGrabbed(Api, OutFailure);
#else
    (void)OutFailure;
    return true;
#endif
}

bool FDriveOsInput::IsActiveEnough(bool bWmReportsActive, uint64 Active, uint64 ActivationXWindow, bool bSdlFocus)
{
    return bSdlFocus && (!bWmReportsActive || Active == ActivationXWindow);
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
    if (!LockHeldOrFail(Lock, LockPath, OutFailure))
    {
        return false;
    }
    // The caller checked, this frame, that the target's X window is on this path; an unchanged
    // top-level window after the motion means it still is, whichever process a new window has.
    const TArray<uint64> PathBefore = XWindowPathAt(ScreenPos);
    // Likewise the active window: the caller waited for the target to be it (#432), and a press
    // into a window that lost it during the motion would only focus that window again.
    const uint64 ActiveBefore = XActiveWindow();
    if (!MoveUnlocked(ScreenPos, OutFailure))
    {
        return false;
    }

    if (!CheckPress(FIntPoint(FMath::RoundToInt(ScreenPos.X), FMath::RoundToInt(ScreenPos.Y)), OutFailure))
    {
        return false;
    }
    if (TopChanged(PathBefore, XWindowPathAt(ScreenPos)))
    {
        Fail(OutFailure, ErrorCodes::ERR_INPUT_FAILED,
            TEXT("A window was stacked over the target during the motion; the button was not pressed."));
        return false;
    }
    const uint64 ActiveAfter = XActiveWindow();
    if (ActiveAfter != ActiveBefore)
    {
        Fail(OutFailure, ErrorCodes::ERR_INPUT_FAILED,
            FString::Printf(TEXT("The active window changed during the motion (0x%llx to 0x%llx); a press now would only focus a window, so the button was not pressed. Retry."),
                ActiveBefore, ActiveAfter));
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
