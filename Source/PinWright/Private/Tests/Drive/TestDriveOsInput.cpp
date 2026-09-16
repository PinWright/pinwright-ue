// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the pure half of FDriveOsInput (the X11/XTEST injection path behind
// drive.click / drive.hover os_input): the interpolated motion path and the X button
// mapping. The injection itself needs a live X display and is not covered here; these two
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
