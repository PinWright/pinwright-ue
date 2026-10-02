// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for FDriveOsGesture (drive.os_gesture, drive.drag os_input): the step plan, the per-tick
// pacing that keeps a held button visible across engine frames, the viewport-pixel mapping, the
// verb's argument refusals, and the display-lock / pointer-grab gates applied before any motion.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveOsGesture.h"
#include "Handlers/Drive/DriveOsInput.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#if PLATFORM_LINUX
#include <dlfcn.h>
#endif

namespace DriveOsGestureTestLocal
{
    using EKind = FDriveOsGestureStep::EKind;

    int32 PlanIndexOf(const TArray<FDriveOsGestureStep>& Plan, EKind Kind)
    {
        return Plan.IndexOfByPredicate([Kind](const FDriveOsGestureStep& Step) { return Step.Kind == Kind; });
    }

    // Drive RunDueSteps the way the ticker does, one call per simulated frame of FrameMs, and
    // record which frame ran each step (in execution order).
    TArray<int32> RunInFrames(const TArray<FDriveOsGestureStep>& Plan, double FrameMs, TArray<int32>& OutExecuted)
    {
        TArray<int32> FrameOfStep;
        FrameOfStep.Init(INDEX_NONE, Plan.Num());
        int32 Next = 0;
        for (int32 Frame = 0; Frame < 10000 && Plan.IsValidIndex(Next); ++Frame)
        {
            FDriveInjectFailure Failure;
            FDriveOsGesture::RunDueSteps(Plan, Frame * FrameMs, Next,
                [&](const FDriveOsGestureStep& Step, FDriveInjectFailure&)
                {
                    const int32 Index = static_cast<int32>(&Step - Plan.GetData());
                    FrameOfStep[Index] = Frame;
                    OutExecuted.Add(Index);
                    return true;
                },
                Failure);
        }
        return FrameOfStep;
    }

    TSharedPtr<FJsonObject> MakePointJson(double X, double Y)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        return Obj;
    }
}

using namespace DriveOsGestureTestLocal;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsGesturePlanClickTest,
    "PinWright.drive.os_gesture.Plan.ApproachPressHoldRelease",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsGesturePlanClickTest::RunTest(const FString& Parameters)
{
    const FIntPoint Press(240, 120);
    const TArray<FDriveOsGestureStep> Plan = FDriveOsGesture::BuildPlan(FIntPoint(0, 0), Press, {}, 80, FDriveOsGesture::DefaultSegmentMs);

    const int32 PressIndex = PlanIndexOf(Plan, EKind::Press);
    const int32 ReleaseIndex = PlanIndexOf(Plan, EKind::Release);
    if (!TestTrue(TEXT("the plan has a press and a release"), PressIndex > 0 && ReleaseIndex > PressIndex))
    {
        return false;
    }
    for (int32 Index = 0; Index < PressIndex; ++Index)
    {
        TestTrue(TEXT("every step before the press is approach motion"), Plan[Index].Kind == EKind::Move);
    }
    TestEqual(TEXT("the approach ends on the press point"), Plan[PressIndex - 1].Point, Press);
    TestEqual(TEXT("the press is at the press point"), Plan[PressIndex].Point, Press);
    TestEqual(TEXT("the press waits the approach settle after the last motion"),
        Plan[PressIndex].AtMs - Plan[PressIndex - 1].AtMs, static_cast<double>(FDriveOsGesture::ApproachSettleMs));
    TestEqual(TEXT("nothing moves while pressed: the release follows the press"), ReleaseIndex, PressIndex + 1);
    TestEqual(TEXT("the release is where the press was"), Plan[ReleaseIndex].Point, Press);
    TestEqual(TEXT("the button is held hold_ms"), Plan[ReleaseIndex].AtMs - Plan[PressIndex].AtMs, 80.0);
    TestTrue(TEXT("the plan ends with End, after the release settle"),
        Plan.Last().Kind == EKind::End && Plan.Last().AtMs == Plan[ReleaseIndex].AtMs + FDriveOsGesture::ReleaseSettleMs);
    for (int32 Index = 1; Index < Plan.Num(); ++Index)
    {
        TestTrue(TEXT("steps are in time order"), Plan[Index].AtMs >= Plan[Index - 1].AtMs);
    }

    const TArray<FDriveOsGestureStep> AlreadyThere = FDriveOsGesture::BuildPlan(Press, Press, {}, 80, FDriveOsGesture::DefaultSegmentMs);
    TestEqual(TEXT("a pointer already on the press point gets no approach motion"), PlanIndexOf(AlreadyThere, EKind::Press), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsGesturePlanPressedPathTest,
    "PinWright.drive.os_gesture.Plan.PressedPathVisitsEveryPoint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsGesturePlanPressedPathTest::RunTest(const FString& Parameters)
{
    // An unsteady click: 4-8 px of wobble while pressed, coming back to the press point.
    const FIntPoint Press(100, 100);
    const TArray<FIntPoint> Path = { FIntPoint(104, 100), FIntPoint(104, 108), FIntPoint(100, 100) };
    const TArray<FDriveOsGestureStep> Plan = FDriveOsGesture::BuildPlan(Press, Press, Path, 0, FDriveOsGesture::DefaultSegmentMs);

    const int32 PressIndex = PlanIndexOf(Plan, EKind::Press);
    const int32 ReleaseIndex = PlanIndexOf(Plan, EKind::Release);
    if (!TestTrue(TEXT("press, pressed motion, release"), PressIndex == 0 && ReleaseIndex > PressIndex + 1))
    {
        return false;
    }
    TestEqual(TEXT("hold_ms 0: the pressed motion starts with the press"), Plan[PressIndex + 1].AtMs, Plan[PressIndex].AtMs);

    int32 Waypoint = 0;
    for (int32 Index = PressIndex + 1; Index < ReleaseIndex; ++Index)
    {
        TestTrue(TEXT("only motion between press and release"), Plan[Index].Kind == EKind::Move);
        TestTrue(TEXT("no motion to the point the pointer is already on"), Plan[Index].Point != Plan[Index - 1].Point);
        if (Path.IsValidIndex(Waypoint) && Plan[Index].Point == Path[Waypoint])
        {
            ++Waypoint;
        }
    }
    TestEqual(TEXT("every path point is reached, in order"), Waypoint, Path.Num());
    TestEqual(TEXT("the release is at the last path point"), Plan[ReleaseIndex].Point, Path.Last());
    TestTrue(TEXT("the release comes after the last motion"), Plan[ReleaseIndex].AtMs > Plan[ReleaseIndex - 1].AtMs);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsGestureTickPacingTest,
    "PinWright.drive.os_gesture.TickPacing.HeldButtonSpansFrames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsGestureTickPacingTest::RunTest(const FString& Parameters)
{
    // The defect this pacing exists for: a gesture sent from one blocking call reaches the engine
    // in a single frame, so per-frame mouse polling never sees the button held. At 60 fps the
    // press, the pressed motion and the release of a drag must land on different frames.
    const TArray<FDriveOsGestureStep> Drag = FDriveOsGesture::BuildPlan(
        FIntPoint(0, 0), FIntPoint(100, 0), { FIntPoint(300, 0) }, 80, FDriveOsGesture::DefaultSegmentMs);
    TArray<int32> Executed;
    const TArray<int32> Frame = RunInFrames(Drag, 16.0, Executed);

    TArray<int32> InOrder;
    for (int32 Index = 0; Index < Drag.Num(); ++Index)
    {
        InOrder.Add(Index);
    }
    TestTrue(TEXT("every step runs exactly once, in plan order"), Executed == InOrder);

    const int32 PressIndex = PlanIndexOf(Drag, EKind::Press);
    const int32 ReleaseIndex = PlanIndexOf(Drag, EKind::Release);
    TSet<int32> PressedMotionFrames;
    for (int32 Index = PressIndex + 1; Index < ReleaseIndex; ++Index)
    {
        PressedMotionFrames.Add(Frame[Index]);
    }
    TestTrue(TEXT("the first pressed motion is on a later frame than the press (80 ms hold)"), Frame[PressIndex + 1] > Frame[PressIndex]);
    TestTrue(TEXT("the pressed motion spans several frames"), PressedMotionFrames.Num() >= 3);
    TestTrue(TEXT("the release is on a later frame than the press"), Frame[ReleaseIndex] > Frame[PressIndex]);
    TestTrue(TEXT("the approach motion spans several frames too"), Frame[PressIndex - 1] > Frame[0]);

    // A zero-hold click with no motion is the one gesture that does go out in one frame.
    const TArray<FDriveOsGestureStep> Tap = FDriveOsGesture::BuildPlan(FIntPoint(5, 5), FIntPoint(5, 5), {}, 0, FDriveOsGesture::DefaultSegmentMs);
    TArray<int32> TapExecuted;
    const TArray<int32> TapFrame = RunInFrames(Tap, 16.0, TapExecuted);
    TestEqual(TEXT("hold_ms 0: press and release in the same frame"),
        TapFrame[PlanIndexOf(Tap, EKind::Release)], TapFrame[PlanIndexOf(Tap, EKind::Press)]);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsGestureRefusedPressTest,
    "PinWright.drive.os_gesture.TickPacing.RefusedPressStopsTheGesture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsGestureRefusedPressTest::RunTest(const FString& Parameters)
{
    // CheckPress refusing (POINTER_MOVED, a raised foreign window) must leave the button up: no
    // release, no pressed motion, and the cursor stays on the refused step.
    const TArray<FDriveOsGestureStep> Plan = FDriveOsGesture::BuildPlan(
        FIntPoint(0, 0), FIntPoint(50, 0), { FIntPoint(80, 0) }, 0, FDriveOsGesture::DefaultSegmentMs);
    TArray<EKind> Ran;
    int32 Next = 0;
    FDriveInjectFailure Failure;
    const bool bOk = FDriveOsGesture::RunDueSteps(Plan, 1.0e9, Next,
        [&Ran](const FDriveOsGestureStep& Step, FDriveInjectFailure& OutFailure)
        {
            if (Step.Kind == EKind::Press)
            {
                OutFailure.Code = TEXT("POINTER_MOVED");
                return false;
            }
            Ran.Add(Step.Kind);
            return true;
        },
        Failure);
    TestFalse(TEXT("the run reports the refusal"), bOk);
    TestEqual(TEXT("... with the step's failure"), Failure.Code, FString(TEXT("POINTER_MOVED")));
    TestEqual(TEXT("... stopped on the press"), Next, PlanIndexOf(Plan, EKind::Press));
    TestFalse(TEXT("... and nothing after it ran"), Ran.Contains(EKind::Release));
    TestEqual(TEXT("... only the approach motion ran"), Ran.Num(), PlanIndexOf(Plan, EKind::Press));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsGestureViewportMappingTest,
    "PinWright.drive.os_gesture.ViewportPixelToScreen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsGestureViewportMappingTest::RunTest(const FString& Parameters)
{
    // A 1280x720 viewport drawn into a 640x360 desktop rect at (100, 50): a 0.5 DPI ratio.
    const FIntPoint Size(1280, 720);
    const FVector2D Pos(100.0, 50.0);
    const FVector2D Rect(640.0, 360.0);
    TestTrue(TEXT("the viewport origin is the widget's corner"),
        FDriveOsGesture::ViewportPixelToScreen(FVector2D(0, 0), Size, Pos, Rect).Equals(Pos));
    TestTrue(TEXT("the center maps through the ratio"),
        FDriveOsGesture::ViewportPixelToScreen(FVector2D(640, 360), Size, Pos, Rect).Equals(FVector2D(420.0, 230.0)));
    TestTrue(TEXT("at 1:1 it is a pure offset"),
        FDriveOsGesture::ViewportPixelToScreen(FVector2D(10, 20), Size, Pos, FVector2D(1280.0, 720.0)).Equals(FVector2D(110.0, 70.0)));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsGestureArgsTest,
    "PinWright.drive.os_gesture.ArgsRefusedBeforeInjecting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsGestureArgsTest::RunTest(const FString& Parameters)
{
    // Each malformed request is refused with INVALID_ARGUMENT naming the field, before PIE, X or
    // the pointer are touched, so these run on any host.
    const auto Expect = [this](const TCHAR* What, const TSharedPtr<FJsonObject>& Payload, const TCHAR* Mentions)
    {
        FTestResponseCapture Capture;
        TestTrue(FString::Printf(TEXT("%s: drive.os_gesture is registered"), What), InvokeHandlerWithCapture(TEXT("drive.os_gesture"), Payload, Capture));
        TestFalse(FString::Printf(TEXT("%s: refused"), What), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("%s: INVALID_ARGUMENT"), What), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        TestTrue(FString::Printf(TEXT("%s: the message names %s (%s)"), What, Mentions, *Capture.Message), Capture.Message.Contains(Mentions));
    };

    TSharedPtr<FJsonObject> BadButton = MakeShared<FJsonObject>();
    BadButton->SetObjectField(TEXT("at"), MakePointJson(10, 10));
    BadButton->SetStringField(TEXT("button"), TEXT("fourth"));
    Expect(TEXT("unknown button"), BadButton, TEXT("button"));

    TSharedPtr<FJsonObject> BadHold = MakeShared<FJsonObject>();
    BadHold->SetObjectField(TEXT("at"), MakePointJson(10, 10));
    BadHold->SetNumberField(TEXT("hold_ms"), -1);
    Expect(TEXT("negative hold"), BadHold, TEXT("hold_ms"));

    TSharedPtr<FJsonObject> HalfPoint = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> OnlyX = MakeShared<FJsonObject>();
    OnlyX->SetNumberField(TEXT("x"), 10);
    HalfPoint->SetObjectField(TEXT("at"), OnlyX);
    Expect(TEXT("at without y"), HalfPoint, TEXT("at must be"));

    TSharedPtr<FJsonObject> BothForms = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> Mixed = MakePointJson(10, 10);
    Mixed->SetStringField(TEXT("actor"), TEXT("Cube"));
    BothForms->SetObjectField(TEXT("at"), Mixed);
    Expect(TEXT("at with both a pixel and an actor"), BothForms, TEXT("at must be"));

    TSharedPtr<FJsonObject> UnknownKey = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> WithZ = MakePointJson(10, 10);
    WithZ->SetNumberField(TEXT("z"), 3);
    UnknownKey->SetObjectField(TEXT("to"), WithZ);
    UnknownKey->SetObjectField(TEXT("at"), MakePointJson(10, 10));
    Expect(TEXT("to with an unknown key"), UnknownKey, TEXT("to must be"));

    TSharedPtr<FJsonObject> BadPath = MakeShared<FJsonObject>();
    BadPath->SetObjectField(TEXT("at"), MakePointJson(10, 10));
    TSharedPtr<FJsonObject> OnlyDx = MakeShared<FJsonObject>();
    OnlyDx->SetNumberField(TEXT("dx"), 4);
    BadPath->SetArrayField(TEXT("waypoints"), { MakeShared<FJsonValueObject>(OnlyDx) });
    Expect(TEXT("waypoint without dy"), BadPath, TEXT("waypoints[0]"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsGestureNoPieTest,
    "PinWright.drive.os_gesture.RefusedWithoutPie",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsGestureNoPieTest::RunTest(const FString& Parameters)
{
    if (GEditor && GEditor->PlayWorld)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pie-running"),
            TEXT("a PIE session is running, so the no-PIE refusal cannot be observed."));
        return true;
    }
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("at"), MakePointJson(10, 10));
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("drive.os_gesture"), Payload, Capture);
    TestFalse(TEXT("a well-formed gesture with no PIE is refused"), Capture.bSuccess);
    TestEqual(TEXT("... with PIE_NOT_ACTIVE"), Capture.ErrorCode, FString(TEXT("PIE_NOT_ACTIVE")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsGestureParamsDeclaredTest,
    "PinWright.drive.os_gesture.ParamsDeclared",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsGestureParamsDeclaredTest::RunTest(const FString& Parameters)
{
    // The dispatcher refuses an undeclared key before the body runs, so a gesture parameter the
    // body reads but the registration omits would be unreachable for every real caller.
    for (const TCHAR* Key : { TEXT("at"), TEXT("to"), TEXT("waypoints"), TEXT("button"), TEXT("hold_ms"), TEXT("world") })
    {
        TestTrue(FString::Printf(TEXT("drive.os_gesture accepts %s"), Key), ParamSpecTestHelpers::IsParamAccepted(TEXT("drive.os_gesture"), Key));
    }
    TestTrue(TEXT("drive.drag accepts os_input"), ParamSpecTestHelpers::IsParamAccepted(TEXT("drive.drag"), TEXT("os_input")));
    return true;
}

#if PLATFORM_LINUX

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsGestureWaitsForDisplayLockTest,
    "PinWright.drive.os_gesture.WaitsForDisplayLock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsGestureWaitsForDisplayLockTest::RunTest(const FString& Parameters)
{
    // A gesture holds the same per-display lock as drive.click for its whole span. With a stand-in
    // peer holding it, Start refuses with OS_INPUT_BUSY before any motion, synchronously. Needs no
    // X display: the lock is taken first. Costs LockTimeoutSeconds.
    const FDriveOsInput::FDisplayLock Peer(FDriveOsInput::DisplayLockPath(), FDriveOsInput::LockTimeoutSeconds);
    if (!TestTrue(TEXT("the stand-in peer holds the display lock"), Peer.IsHeld()))
    {
        AddError(Peer.Error);
        return false;
    }
    // Shared, captured by value: if the gate regressed, Start would run the gesture from the
    // ticker after this test returned, and a by-reference capture would dangle.
    struct FOutcome { bool bCalled = false; bool bOk = true; FDriveInjectFailure Failure; };
    const TSharedRef<FOutcome> Outcome = MakeShared<FOutcome>();
    FDriveOsGesture::Start(FIntPoint(-100000, -100000), {}, EDriveMouseButton::Left, 0, FDriveOsGesture::DefaultSegmentMs,
        [Outcome](bool bInOk, const FDriveInjectFailure& Failure, const FDriveOsGesture::FResult&)
        {
            Outcome->bCalled = true;
            Outcome->bOk = bInOk;
            Outcome->Failure = Failure;
        });
    TestTrue(TEXT("the refusal is reported synchronously"), Outcome->bCalled);
    TestFalse(TEXT("the gesture is refused"), Outcome->bOk);
    TestEqual(TEXT("... with OS_INPUT_BUSY"), Outcome->Failure.Code, FString(TEXT("OS_INPUT_BUSY")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsGesturePointerGrabTest,
    "PinWright.drive.os_gesture.PointerGrabRefusesBeforeMotion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsGesturePointerGrabTest::RunTest(const FString& Parameters)
{
    // While another X client holds a pointer grab, a gesture must refuse with POINTER_GRABBED
    // before the approach motion, leaving the real pointer where it was.
    FString Unavailable;
    if (FPlatformMisc::GetEnvironmentVariable(TEXT("DISPLAY")).IsEmpty() || !FDriveOsInput::IsAvailable(Unavailable))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-x-display"),
            FString::Printf(TEXT("the grab gate needs a live X display and this editor has none (%s)."), *Unavailable));
        return true;
    }

    using XDisplay = void*;
    void* Xlib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!TestNotNull(TEXT("libX11 loads"), Xlib))
    {
        return false;
    }
    const auto OpenDisplay = reinterpret_cast<XDisplay (*)(const char*)>(dlsym(Xlib, "XOpenDisplay"));
    const auto CloseDisplay = reinterpret_cast<int (*)(XDisplay)>(dlsym(Xlib, "XCloseDisplay"));
    const auto RootWindow = reinterpret_cast<unsigned long (*)(XDisplay)>(dlsym(Xlib, "XDefaultRootWindow"));
    const auto GrabPointer = reinterpret_cast<int (*)(XDisplay, unsigned long, int, unsigned int, int, int, unsigned long, unsigned long, unsigned long)>(dlsym(Xlib, "XGrabPointer"));
    const auto UngrabPointer = reinterpret_cast<int (*)(XDisplay, unsigned long)>(dlsym(Xlib, "XUngrabPointer"));
    const auto Sync = reinterpret_cast<int (*)(XDisplay, int)>(dlsym(Xlib, "XSync"));
    XDisplay Holder = OpenDisplay ? OpenDisplay(nullptr) : nullptr;
    if (!TestNotNull(TEXT("the stand-in grab holder opens its own X connection"), Holder))
    {
        return false;
    }

    FDriveOsInput::FGrabState Before;
    FDriveOsInput::ProbeGrabs(Before);
    if (Before.bPointerGrabbed)
    {
        CloseDisplay(Holder);
        PinWrightTestSkip::SkipAssertions(*this, TEXT("x-grab-already-held"),
            TEXT("another X client already holds a pointer grab on this display, so the free-to-grabbed transition cannot be staged."));
        return true;
    }

    // GrabModeAsync = 1, CurrentTime = 0, GrabSuccess = 0.
    const bool bHeld = GrabPointer(Holder, RootWindow(Holder), 0, 0, 1, 1, 0, 0, 0) == 0;
    Sync(Holder, 0);

    FIntPoint PointerBefore(-1, -1);
    FDriveOsInput::IsPointerAt(FIntPoint(-100000, -100000), PointerBefore);
    // Shared, captured by value: if the gate regressed, Start would run the gesture from the
    // ticker after this test returned, and a by-reference capture would dangle.
    struct FOutcome { bool bCalled = false; bool bOk = true; FDriveInjectFailure Failure; };
    const TSharedRef<FOutcome> Outcome = MakeShared<FOutcome>();
    FDriveOsGesture::Start(PointerBefore + FIntPoint(40, 40), {}, EDriveMouseButton::Left, 0, FDriveOsGesture::DefaultSegmentMs,
        [Outcome](bool bInOk, const FDriveInjectFailure& Failure, const FDriveOsGesture::FResult&)
        {
            Outcome->bCalled = true;
            Outcome->bOk = bInOk;
            Outcome->Failure = Failure;
        });
    FIntPoint PointerAfter(-1, -1);
    FDriveOsInput::IsPointerAt(FIntPoint(-100000, -100000), PointerAfter);

    UngrabPointer(Holder, 0);
    Sync(Holder, 0);
    CloseDisplay(Holder);

    TestTrue(TEXT("the stand-in holder took the pointer grab"), bHeld);
    TestTrue(TEXT("the refusal is reported synchronously"), Outcome->bCalled);
    TestFalse(TEXT("the gesture is refused"), Outcome->bOk);
    TestEqual(TEXT("... with POINTER_GRABBED"), Outcome->Failure.Code, FString(TEXT("POINTER_GRABBED")));
    TestEqual(TEXT("... and the pointer was not moved"), PointerAfter, PointerBefore);
    return true;
}

#endif // PLATFORM_LINUX
