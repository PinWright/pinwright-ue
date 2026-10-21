// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the pure half of FDriveOsInput (the X11/XTEST injection path behind
// drive.click / drive.hover os_input): the interpolated motion path, the X button
// mapping and the point-ownership decision. The injection itself needs a live X display and is not covered here; these two
// are the parts that can be silently wrong — a path that does not end on the target, or a
// middle/right button swap, both of which look like "the click did nothing".

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveOsInput.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#if PLATFORM_LINUX
#include <dlfcn.h>
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputMotionPathTest,
    "PinWright.drive.os_input.MotionPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputMotionPathTest::RunTest(const FString& Parameters)
{
    const FIntPoint From(100, 200);
    const FIntPoint To(340, 440);
    const TArray<FIntPoint> Path = FDriveOsInput::ComputeMotionPath(From, To);

    TestEqual(TEXT("the path is 24 steps"), Path.Num(), 24);
    TestFalse(TEXT("the path excludes the start point"), Path.Contains(From));
    TestEqual(TEXT("the path ends exactly on the target x"), Path.Last().X, To.X);
    TestEqual(TEXT("the path ends exactly on the target y"), Path.Last().Y, To.Y);

    // Monotonic advance toward the target: a step that goes backwards would make the
    // motion deltas nonsense to whatever is reading them.
    FIntPoint Prev = From;
    for (const FIntPoint& Step : Path)
    {
        TestTrue(TEXT("x advances monotonically"), Step.X >= Prev.X);
        TestTrue(TEXT("y advances monotonically"), Step.Y >= Prev.Y);
        Prev = Step;
    }

    // A zero-length move still emits its steps, all sitting on the target.
    const TArray<FIntPoint> Stationary = FDriveOsInput::ComputeMotionPath(From, From);
    TestEqual(TEXT("a stationary move still has 24 steps"), Stationary.Num(), 24);
    TestEqual(TEXT("a stationary move stays put in x"), Stationary.Last().X, From.X);
    TestEqual(TEXT("a stationary move stays put in y"), Stationary.Last().Y, From.Y);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputButtonMappingTest,
    "PinWright.drive.os_input.ButtonMapping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputButtonMappingTest::RunTest(const FString& Parameters)
{
    // X numbers middle 2 and right 3 — the opposite order from EDriveMouseButton.
    TestEqual(TEXT("left maps to X button 1"), FDriveOsInput::ButtonToXButton(EDriveMouseButton::Left), 1);
    TestEqual(TEXT("middle maps to X button 2"), FDriveOsInput::ButtonToXButton(EDriveMouseButton::Middle), 2);
    TestEqual(TEXT("right maps to X button 3"), FDriveOsInput::ButtonToXButton(EDriveMouseButton::Right), 3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputPointOwnershipTest,
    "PinWright.drive.os_input.PointOwnership",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputPointOwnershipTest::RunTest(const FString& Parameters)
{
    // The guard that keeps os_input from pressing into another process's window on a shared
    // X display: the pids are the _NET_WM_PID of each window containing the point, outermost
    // first, 0 where a window names none (a WM frame, the desktop).
    constexpr uint32 Self = 1000;
    constexpr uint32 Peer = 2000;
    constexpr uint32 Wm = 3000;
    int32 Owner = 0;

    TestTrue(TEXT("our own override-redirect window (no frame) is ours"),
        FDriveOsInput::IsPointOwnedBy({ Self }, Self, Owner));
    TestEqual(TEXT("... and it is the deciding window"), Owner, 0);

    TestTrue(TEXT("our client under an unnamed WM frame is ours"),
        FDriveOsInput::IsPointOwnedBy({ 0, Self }, Self, Owner));
    TestEqual(TEXT("... decided by the client, not the frame"), Owner, 1);

    TestTrue(TEXT("a frame carrying the WM's pid does not steal our client"),
        FDriveOsInput::IsPointOwnedBy({ Wm, Self }, Self, Owner));
    TestEqual(TEXT("... the deepest named window decides"), Owner, 1);

    TestFalse(TEXT("a peer editor's window stacked over ours is foreign"),
        FDriveOsInput::IsPointOwnedBy({ 0, Peer }, Self, Owner));
    TestEqual(TEXT("... and names the peer's window"), Owner, 1);

    TestFalse(TEXT("a point on a window frame with no pid is not ours"),
        FDriveOsInput::IsPointOwnedBy({ 0 }, Self, Owner));
    TestEqual(TEXT("... with no deciding window"), Owner, static_cast<int32>(INDEX_NONE));

    TestFalse(TEXT("bare root (nothing mapped at the point) is not ours"),
        FDriveOsInput::IsPointOwnedBy({}, Self, Owner));
    TestEqual(TEXT("... with no deciding window"), Owner, static_cast<int32>(INDEX_NONE));
    return true;
}

// ---- Cross-process serialization on a shared X display (B-os-input-shared-pointer-race) ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputDisplayLockPathTest,
    "PinWright.drive.os_input.DisplayLockPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputDisplayLockPathTest::RunTest(const FString& Parameters)
{
    // ":0" and ":0.0" are one pointer; two editors that spell DISPLAY differently must still
    // take the same lock, or they are not serialized at all.
    TestEqual(TEXT("the screen suffix is dropped"),
        FDriveOsInput::LockPathFor(TEXT(":0.0"), 1000),
        FDriveOsInput::LockPathFor(TEXT(":0"), 1000));
    TestEqual(TEXT("the file sits in /tmp with the uid in its name"),
        FDriveOsInput::LockPathFor(TEXT(":0"), 1000),
        FString(TEXT("/tmp/pinwright-os-input-1000-_0.lock")));
    TestNotEqual(TEXT("different displays get different locks"),
        FDriveOsInput::LockPathFor(TEXT(":1"), 1000),
        FDriveOsInput::LockPathFor(TEXT(":0"), 1000));
    TestEqual(TEXT("a remote display keeps its host, minus the screen"),
        FDriveOsInput::LockPathFor(TEXT("localhost:10.0"), 1000),
        FString(TEXT("/tmp/pinwright-os-input-1000-localhost_10.lock")));
    return true;
}

#if PLATFORM_LINUX

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputDisplayLockExcludesTest,
    "PinWright.drive.os_input.DisplayLockExcludes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputDisplayLockExcludesTest::RunTest(const FString& Parameters)
{
    // Two acquirers of one lock file. flock binds the open file, not the process, so a second
    // instance in this process contends exactly as a peer editor's would.
    const FString Path = FPaths::CreateTempFilename(FPlatformProcess::UserTempDir(), TEXT("pw-os-input-lock-"), TEXT(".lock"));
    {
        const FDriveOsInput::FDisplayLock First(Path, 1.0);
        TestTrue(TEXT("the first acquirer holds the lock"), First.IsHeld());

        const double Start = FPlatformTime::Seconds();
        const FDriveOsInput::FDisplayLock Second(Path, 0.2);
        const double Waited = FPlatformTime::Seconds() - Start;
        TestFalse(TEXT("the second acquirer does not get it while the first holds it"), Second.IsHeld());
        TestTrue(TEXT("... and reports a timeout, not an I/O failure"), Second.bTimedOut);
        TestTrue(TEXT("... after waiting out its timeout"), Waited >= 0.19);
        TestTrue(TEXT("... but not much longer"), Waited < 2.0);
        TestEqual(TEXT("... naming the holder's pid from the lock file"),
            Second.HolderPid, static_cast<uint32>(FPlatformProcess::GetCurrentProcessId()));
        TestTrue(TEXT("... in its message"),
            Second.Error.Contains(FString::Printf(TEXT("pid %u"), FPlatformProcess::GetCurrentProcessId())));
    }
    {
        const FDriveOsInput::FDisplayLock Third(Path, 0.2);
        TestTrue(TEXT("the lock is free again once the holder is gone"), Third.IsHeld());
    }
    IFileManager::Get().Delete(*Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputClickWaitsForDisplayLockTest,
    "PinWright.drive.os_input.ClickWaitsForDisplayLock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputClickWaitsForDisplayLockTest::RunTest(const FString& Parameters)
{
    // Hold this display's real lock, standing in for a peer editor mid-click, and ask for a
    // click: it must refuse with OS_INPUT_BUSY after the bounded wait, having touched nothing.
    // The target is far off-screen, so even an unserialized ClickAt cannot press anything
    // (the foreign-window gate refuses an empty point); it would fail with INPUT_FAILED.
    // Costs LockTimeoutSeconds. Needs no X display: the lock is taken before X is touched.
    const FDriveOsInput::FDisplayLock Peer(FDriveOsInput::DisplayLockPath(), FDriveOsInput::LockTimeoutSeconds);
    if (!TestTrue(TEXT("the stand-in peer holds the display lock"), Peer.IsHeld()))
    {
        AddError(Peer.Error);
        return false;
    }

    FDriveInjectFailure Failure;
    const bool bClicked = FDriveOsInput::ClickAt(FVector2D(-100000.0, -100000.0), EDriveMouseButton::Left, Failure);
    TestFalse(TEXT("the click is refused"), bClicked);
    TestEqual(TEXT("... with OS_INPUT_BUSY"), Failure.Code, FString(TEXT("OS_INPUT_BUSY")));
    double HolderPid = 0.0;
    TestTrue(TEXT("... with holder_pid in the payload"),
        Failure.Details.IsValid() && Failure.Details->TryGetNumberField(TEXT("holder_pid"), HolderPid));
    TestEqual(TEXT("... naming the holder"), static_cast<uint32>(HolderPid),
        static_cast<uint32>(FPlatformProcess::GetCurrentProcessId()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputPrePressPointerCheckTest,
    "PinWright.drive.os_input.PrePressPointerCheck",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputPrePressPointerCheckTest::RunTest(const FString& Parameters)
{
    // The check ClickAt runs right before the press. It reads the real pointer, so it needs a
    // live X display; it moves and presses nothing.
    FString Unavailable;
    if (FPlatformMisc::GetEnvironmentVariable(TEXT("DISPLAY")).IsEmpty() || !FDriveOsInput::IsAvailable(Unavailable))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-x-display"),
            FString::Printf(TEXT("the pre-press pointer check reads the real X pointer and this editor has no X display (%s)."), *Unavailable));
        return true;
    }

    // Keep peer PinWright injectors off the pointer between the two reads.
    const FDriveOsInput::FDisplayLock Lock(FDriveOsInput::DisplayLockPath(), FDriveOsInput::LockTimeoutSeconds);
    if (!Lock.IsHeld())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("os-input-busy"), Lock.Error);
        return true;
    }

    // No pointer can be at a point this far off every screen: the refusal path, deterministic.
    FIntPoint Pointer(-1, -1);
    TestFalse(TEXT("a pointer that is not on the target refuses the press"),
        FDriveOsInput::IsPointerAt(FIntPoint(-100000, -100000), Pointer));
    TestTrue(TEXT("... and reports where the pointer really is"), Pointer.X >= 0 && Pointer.Y >= 0);

    FIntPoint Again(-1, -1);
    TestTrue(TEXT("a pointer on the target lets the press through"), FDriveOsInput::IsPointerAt(Pointer, Again));
    TestEqual(TEXT("... at the same point"), Again, Pointer);
    return true;
}

// ---- An X pointer grab swallows real input (E-input-state-blind-to-x-pointer-grab) ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputPointerGrabRefusesTest,
    "PinWright.drive.os_input.PointerGrabRefuses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputPointerGrabRefusesTest::RunTest(const FString& Parameters)
{
    // A second X client stands in for whoever holds the grab (in the field: this editor's SDL
    // in relative mouse mode). While it holds pointer + keyboard grabs, drive.input_state must
    // report both and an os_input click must refuse with POINTER_GRABBED, moving nothing.
    // Both grabs last only a few milliseconds, but they are real on the shared display.
    FString Unavailable;
    if (FPlatformMisc::GetEnvironmentVariable(TEXT("DISPLAY")).IsEmpty() || !FDriveOsInput::IsAvailable(Unavailable))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-x-display"),
            FString::Printf(TEXT("the grab probe needs a live X display and this editor has none (%s)."), *Unavailable));
        return true;
    }

    using XDisplay = void*;
    void* Xlib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!TestNotNull(TEXT("libX11 loads (IsAvailable already loaded it)"), Xlib))
    {
        return false;
    }
    const auto OpenDisplay = reinterpret_cast<XDisplay (*)(const char*)>(dlsym(Xlib, "XOpenDisplay"));
    const auto CloseDisplay = reinterpret_cast<int (*)(XDisplay)>(dlsym(Xlib, "XCloseDisplay"));
    const auto RootWindow = reinterpret_cast<unsigned long (*)(XDisplay)>(dlsym(Xlib, "XDefaultRootWindow"));
    const auto GrabPointer = reinterpret_cast<int (*)(XDisplay, unsigned long, int, unsigned int, int, int, unsigned long, unsigned long, unsigned long)>(dlsym(Xlib, "XGrabPointer"));
    const auto UngrabPointer = reinterpret_cast<int (*)(XDisplay, unsigned long)>(dlsym(Xlib, "XUngrabPointer"));
    const auto GrabKeyboard = reinterpret_cast<int (*)(XDisplay, unsigned long, int, int, int, unsigned long)>(dlsym(Xlib, "XGrabKeyboard"));
    const auto UngrabKeyboard = reinterpret_cast<int (*)(XDisplay, unsigned long)>(dlsym(Xlib, "XUngrabKeyboard"));
    const auto Sync = reinterpret_cast<int (*)(XDisplay, int)>(dlsym(Xlib, "XSync"));
    XDisplay Holder = OpenDisplay ? OpenDisplay(nullptr) : nullptr;
    if (!TestNotNull(TEXT("the stand-in grab holder opens its own X connection"), Holder))
    {
        return false;
    }

    FDriveOsInput::FGrabState Before;
    TestTrue(TEXT("the probe runs with an X display"), FDriveOsInput::ProbeGrabs(Before));
    if (Before.bPointerGrabbed || Before.bKeyboardGrabbed)
    {
        CloseDisplay(Holder);
        PinWrightTestSkip::SkipAssertions(*this, TEXT("x-grab-already-held"),
            TEXT("another X client already holds a pointer or keyboard grab on this display, so the free-to-grabbed transition cannot be staged."));
        return true;
    }

    // GrabModeAsync = 1, CurrentTime = 0, GrabSuccess = 0.
    const unsigned long Root = RootWindow(Holder);
    const bool bPointerHeld = GrabPointer(Holder, Root, 0, 0, 1, 1, 0, 0, 0) == 0;
    const bool bKeyboardHeld = GrabKeyboard(Holder, Root, 0, 1, 1, 0) == 0;
    Sync(Holder, 0);

    FIntPoint PointerBefore(-1, -1);
    FDriveOsInput::IsPointerAt(FIntPoint(-100000, -100000), PointerBefore);

    FDriveOsInput::FGrabState During;
    FDriveOsInput::ProbeGrabs(During);
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("drive.input_state"), MakeShared<FJsonObject>(), Capture);
    FDriveInjectFailure Failure;
    const bool bClicked = FDriveOsInput::ClickAt(FVector2D(PointerBefore.X + 40, PointerBefore.Y + 40), EDriveMouseButton::Left, Failure);
    // A retry inside the dump cooldown reuses the first dump instead of taking another.
    FPlatformProcess::Sleep(0.1f);
    FDriveInjectFailure Retry;
    FDriveOsInput::ClickAt(FVector2D(PointerBefore.X + 40, PointerBefore.Y + 40), EDriveMouseButton::Left, Retry);
    FIntPoint PointerAfter(-1, -1);
    FDriveOsInput::IsPointerAt(FIntPoint(-100000, -100000), PointerAfter);

    UngrabKeyboard(Holder, 0);
    UngrabPointer(Holder, 0);
    Sync(Holder, 0);

    FDriveOsInput::FGrabState After;
    FDriveOsInput::ProbeGrabs(After);
    CloseDisplay(Holder);

    TestTrue(TEXT("the stand-in holder took the pointer grab"), bPointerHeld);
    TestTrue(TEXT("the stand-in holder took the keyboard grab"), bKeyboardHeld);
    TestTrue(TEXT("the probe sees the pointer grab"), During.bPointerGrabbed);
    TestTrue(TEXT("the probe sees the keyboard grab"), During.bKeyboardGrabbed);
    TestFalse(TEXT("this editor's SDL is not the holder"), During.bHeldByThisEditor.Get(false));

    const TSharedPtr<FJsonObject>* OsGrab = nullptr;
    TestTrue(TEXT("drive.input_state succeeds"), Capture.bSuccess);
    if (TestTrue(TEXT("drive.input_state reports os_grab"),
            Capture.Result.IsValid() && Capture.Result->TryGetObjectField(TEXT("os_grab"), OsGrab)))
    {
        TestTrue(TEXT("... with the pointer grabbed"), (*OsGrab)->GetBoolField(TEXT("pointer")));
        TestTrue(TEXT("... and the keyboard grabbed"), (*OsGrab)->GetBoolField(TEXT("keyboard")));
        TestTrue(TEXT("... and held_by_this_editor present"), (*OsGrab)->HasField(TEXT("held_by_this_editor")));
    }

    TestFalse(TEXT("the os_input click is refused"), bClicked);
    TestEqual(TEXT("... with POINTER_GRABBED"), Failure.Code, FString(TEXT("POINTER_GRABBED")));
    TestTrue(TEXT("... with held_by_this_editor in the payload"),
        Failure.Details.IsValid() && Failure.Details->HasField(TEXT("held_by_this_editor")));
    // The stand-in holder is this process's second X connection, so on an X11 session whose Xorg
    // log exists the grab dump must name this editor's pid; elsewhere (Xvfb, XWayland) it is null.
    TestTrue(TEXT("... with holder_pid in the payload"),
        Failure.Details.IsValid() && Failure.Details->HasField(TEXT("holder_pid")));
    const bool bDumpExpected = FDriveOsInput::SessionType() == TEXT("x11")
        && FDriveOsInput::XorgLogCandidates(FPlatformMisc::GetEnvironmentVariable(TEXT("DISPLAY")),
            FPlatformMisc::GetEnvironmentVariable(TEXT("XDG_DATA_HOME")), FPlatformMisc::GetEnvironmentVariable(TEXT("HOME")))
            .ContainsByPredicate([](const FString& Log) { return IFileManager::Get().FileSize(*Log) >= 0; });
    double HolderPid = 0;
    const bool bHolderNamed = Failure.Details.IsValid() && Failure.Details->TryGetNumberField(TEXT("holder_pid"), HolderPid);
    if (bDumpExpected || bHolderNamed)
    {
        if (TestTrue(FString::Printf(TEXT("... naming the holder from this display's Xorg log (%s)"), *Failure.Message), bHolderNamed))
        {
            TestEqual(TEXT("... as this editor"), static_cast<uint32>(HolderPid), FPlatformProcess::GetCurrentProcessId());
            TestTrue(TEXT("... and saying so in the message"), Failure.Message.Contains(TEXT("this editor process")));
        }
    }
    else
    {
        AddInfo(FString::Printf(TEXT("No Xorg log of this display can take the grab dump, so holder_pid is null: %s"), *Failure.Message));
    }
    double FirstAge = -1;
    double RetryAge = -1;
    TestTrue(TEXT("... with holder_dump_age_s in the payload"),
        Failure.Details.IsValid() && Failure.Details->TryGetNumberField(TEXT("holder_dump_age_s"), FirstAge));
    TestEqual(TEXT("the retry is refused with POINTER_GRABBED too"), Retry.Code, FString(TEXT("POINTER_GRABBED")));
    TestTrue(TEXT("... with holder_dump_age_s in its payload"),
        Retry.Details.IsValid() && Retry.Details->TryGetNumberField(TEXT("holder_dump_age_s"), RetryAge));
    // Only a pressed dump is cached, so the age grows only where a dump can be taken.
    if (bDumpExpected)
    {
        TestTrue(FString::Printf(TEXT("... and reuses the first dump, ~0.1 s older (%.3f -> %.3f)"), FirstAge, RetryAge),
            RetryAge >= FirstAge + 0.09);
    }
    TestEqual(TEXT("... and the pointer was not moved"), PointerAfter, PointerBefore);

    TestFalse(TEXT("the pointer grab reads free again once released"), After.bPointerGrabbed);
    TestFalse(TEXT("the keyboard grab reads free again once released"), After.bKeyboardGrabbed);
    return true;
}

#endif // PLATFORM_LINUX

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputWaylandSessionTest,
    "PinWright.drive.os_input.WaylandSessionDetected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputWaylandSessionTest::RunTest(const FString& Parameters)
{
    // XWayland sets DISPLAY, so IsAvailable passes there; this is what flags the session so
    // os_input results can say delivery is unverified (board E-os-input-wayland-unverified).
    TestEqual(TEXT("WAYLAND_DISPLAY alone marks a Wayland session"),
        FDriveOsInput::SessionTypeFor(TEXT("wayland-0"), TEXT("")), FString(TEXT("wayland")));
    TestEqual(TEXT("XDG_SESSION_TYPE=wayland alone marks a Wayland session"),
        FDriveOsInput::SessionTypeFor(TEXT(""), TEXT("wayland")), FString(TEXT("wayland")));
    TestEqual(TEXT("an x11 session type is X11"),
        FDriveOsInput::SessionTypeFor(TEXT(""), TEXT("x11")), FString(TEXT("x11")));
    TestEqual(TEXT("no session hints (a private Xvfb, an ssh shell) is X11"),
        FDriveOsInput::SessionTypeFor(TEXT(""), TEXT("")), FString(TEXT("x11")));
    TestEqual(TEXT("SessionType reads this editor's own environment"),
        FDriveOsInput::SessionType(),
        FDriveOsInput::SessionTypeFor(FPlatformMisc::GetEnvironmentVariable(TEXT("WAYLAND_DISPLAY")),
            FPlatformMisc::GetEnvironmentVariable(TEXT("XDG_SESSION_TYPE"))));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputGrabHolderFromXorgDumpTest,
    "PinWright.drive.os_input.GrabHolderFromXorgDump",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputGrabHolderFromXorgDumpTest::RunTest(const FString& Parameters)
{
    // POINTER_GRABBED names the holder from the X server's XF86LogGrabInfo dump. The excerpt is
    // the field case (#425): GNOME Shell's keyring prompt held pointer and keyboard. The keyboard
    // grab is listed first here, and the passive-grab list after the end marker names clients
    // too, so a parser that takes the first "client pid" names the wrong grab or the wrong pid.
    const FString Dump = TEXT(
        "[1180801.511] (II) Printing all currently active device grabs:\n"
        "[1180801.512] Active grab 0x300000 (xi2) on device 'Virtual core keyboard' (3):\n"
        "[1180801.512]       client pid 4457 /usr/bin/sunshine \n"
        "[1180801.512]       at 658119929 (from active grab) (device thawed, state 1)\n"
        "[1180801.511] Active grab 0x200000 (xi2) on device 'Virtual core pointer' (2):\n"
        "[1180801.511]       client pid 2134 /usr/bin/gnome-shell \n"
        "[1180801.512]       at 658119928 (from active grab) (device thawed, state 1)\n"
        "[1180801.512] (II) End list of active device grabs\n"
        "[1180801.512] Printing all currently registered grabs\n"
        "[1180801.512]   Printing all registered grabs of client pid 999 /usr/bin/other \n");
    uint32 Pid = 0;
    FString Cmdline;
    TestTrue(TEXT("the core-pointer grab is found"), FDriveOsInput::ParseGrabHolder(Dump, Pid, Cmdline));
    TestEqual(TEXT("... held by gnome-shell's pid, not the keyboard grabber's"), Pid, 2134u);
    TestEqual(TEXT("... with its command line, trimmed"), Cmdline, FString(TEXT("/usr/bin/gnome-shell")));

    const FString KeyboardOnly = TEXT(
        "[1] (II) Printing all currently active device grabs:\n"
        "[1] Active grab 0x300000 (xi2) on device 'Virtual core keyboard' (3):\n"
        "[1]       client pid 4457 /usr/bin/sunshine \n"
        "[1] (II) End list of active device grabs\n"
        "[1] Active grab 0x1 (xi2) on device 'Virtual core pointer' (2):\n"
        "[1]       client pid 999 /usr/bin/other \n");
    TestFalse(TEXT("no pointer grab in the active section names no holder"), FDriveOsInput::ParseGrabHolder(KeyboardOnly, Pid, Cmdline));
    TestFalse(TEXT("an empty dump names no holder"), FDriveOsInput::ParseGrabHolder(FString(), Pid, Cmdline));

    TestTrue(TEXT("a gnome-shell holder gets the system-modal hint"),
        FDriveOsInput::GrabHolderHint(2134, TEXT("/usr/bin/gnome-shell"), 77).Contains(TEXT("keyring")));
    TestTrue(TEXT("this editor's own pid is called out"),
        FDriveOsInput::GrabHolderHint(77, TEXT("/x/UnrealEditor"), 77).Contains(TEXT("this editor process")));
    const FString PeerHint = FDriveOsInput::GrabHolderHint(5, TEXT("/sdb-disk/UE_5.8/Engine/Binaries/Linux/UnrealEditor /x/PDS.uproject"), 77);
    TestTrue(TEXT("a peer Unreal editor is released with Shift+F1 or by stopping PIE"), PeerHint.Contains(TEXT("Shift+F1")));
    TestTrue(TEXT("... and is not to be killed"), PeerHint.Contains(TEXT("do not kill")));
    TestFalse(TEXT("the generic hint does not tell anyone to end the process"),
        FDriveOsInput::GrabHolderHint(5, TEXT("/usr/bin/xterm"), 77).Contains(TEXT("end that process")));
    TestFalse(TEXT("another application gets the generic hint"),
        FDriveOsInput::GrabHolderHint(5, TEXT("/usr/bin/xterm"), 77).Contains(TEXT("GNOME")));

    const TArray<FString> Rootless = FDriveOsInput::XorgLogCandidates(TEXT(":0"), TEXT(""), TEXT("/home/u"));
    if (TestEqual(TEXT(":0 has a rootless and a root log"), Rootless.Num(), 2))
    {
        TestEqual(TEXT("... rootless under ~/.local/share/xorg"), Rootless[0], FString(TEXT("/home/u/.local/share/xorg/Xorg.0.log")));
        TestEqual(TEXT("... root under /var/log"), Rootless[1], FString(TEXT("/var/log/Xorg.0.log")));
    }
    const TArray<FString> Xdg = FDriveOsInput::XorgLogCandidates(TEXT("localhost:10.0"), TEXT("/data"), TEXT("/home/u"));
    TestTrue(TEXT("XDG_DATA_HOME wins, and the screen suffix is dropped"),
        Xdg.Num() == 2 && Xdg[0] == TEXT("/data/xorg/Xorg.10.log"));
    TestEqual(TEXT("a DISPLAY with no number has no log"), FDriveOsInput::XorgLogCandidates(TEXT("wayland-0"), TEXT(""), TEXT("/home/u")).Num(), 0);
    return true;
}
