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

#endif // PLATFORM_LINUX
