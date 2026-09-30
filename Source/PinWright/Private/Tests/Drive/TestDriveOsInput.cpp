// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the pure half of FDriveOsInput (the X11/XTEST injection path behind
// drive.click / drive.hover os_input): the interpolated motion path, the X button
// mapping and the point-ownership decision. The injection itself needs a live X display and is not covered here; these two
// are the parts that can be silently wrong — a path that does not end on the target, or a
// middle/right button swap, both of which look like "the click did nothing".

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveOsInput.h"

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
