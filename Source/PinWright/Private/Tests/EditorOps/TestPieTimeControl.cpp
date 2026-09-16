// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the PIE clock control behind editor.pause / editor.resume / editor.step_frame
// (board E-pause-step-frame-does-not-freeze-umg).
//
// WHAT THE ANIMATION-CLOCK TESTS ACTUALLY PROVE. UMG widget animations advance from
// UUMGSequenceTickManager::TickWidgetAnimations, which the manager binds to
// FSlateApplication::OnPreTick (UMGSequenceTickManager.cpp:56) and which forwards its argument
// straight to UUserWidget::TickActionsAndAnimation (:288). So the delta a subscriber to that
// delegate receives IS the delta every playing widget animation advances by. Subscribing to the
// same delegate and ticking Slate once therefore measures the animation clock itself, with no
// live PIE session, no widget and no window - which is what makes it runnable in the headless
// suite, where a real HUD animation is not constructible.
#include "Misc/AutomationTest.h"

#include "Handlers/Editor/PieTimeControl.h"

#include "Compat/EngineVersionCompat.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestSkipReporting.h"

#include <limits>

namespace PieTimeControlTestHelpers
{
    // Slate must be up (a renderer-less -RenderOffScreen editor still has it) and must not
    // already be inside its own tick, because these tests drive one by hand.
    inline bool CanDriveSlateTick()
    {
        if (!FSlateApplication::IsInitialized())
        {
            return false;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        return !FSlateApplication::Get().IsTicking();
#else
        // FSlateApplication::IsTicking (and the bIsTicking member behind it) arrived in UE 5.4;
        // on 5.3 the engine tracks no such state, so there is nothing to query. The re-entrancy
        // it guards against cannot arise here either: automation test bodies run from
        // FAutomationTestFramework, which the engine loop ticks outside FSlateApplication::Tick.
        return true;
#endif
    }
}

// ============================================================================
// ValidateStepDelta - pure, no engine state
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPieTimeStepDeltaValidationTest,
    "PinWright.editor.step_frame.StepDeltaValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPieTimeStepDeltaValidationTest::RunTest(const FString& Parameters)
{
    double Delta = -1.0;
    FString Reason;

    // Absent / 0 means "one 60 Hz frame", which is what a caller stepping a HUD expects.
    TestTrue(TEXT("absent deltaSeconds is accepted"),
        PinWrightPieTime::ValidateStepDelta(0.0, Delta, Reason));
    TestEqual(TEXT("absent deltaSeconds resolves to the 1/60 default"),
        Delta, PinWrightPieTime::DefaultStepDeltaSeconds, 0.0);

    Delta = -1.0;
    TestTrue(TEXT("an explicit in-range delta is accepted"),
        PinWrightPieTime::ValidateStepDelta(0.05, Delta, Reason));
    TestEqual(TEXT("an explicit in-range delta is passed through unchanged"), Delta, 0.05, 0.0);

    Reason.Empty();
    TestFalse(TEXT("a negative delta is refused"),
        PinWrightPieTime::ValidateStepDelta(-0.01, Delta, Reason));
    TestTrue(TEXT("the negative-delta refusal carries a reason"), !Reason.IsEmpty());

    Reason.Empty();
    TestFalse(TEXT("a delta above the one-second ceiling is refused"),
        PinWrightPieTime::ValidateStepDelta(PinWrightPieTime::MaxStepDeltaSeconds + 0.001, Delta, Reason));
    // 16 as MILLISECONDS is the mistake the ceiling exists to catch, so it must be refused
    // rather than silently stepping the world 16 seconds.
    TestFalse(TEXT("16 (milliseconds mistaken for seconds) is refused"),
        PinWrightPieTime::ValidateStepDelta(16.0, Delta, Reason));

    TestFalse(TEXT("a non-finite delta is refused"),
        PinWrightPieTime::ValidateStepDelta(std::numeric_limits<double>::infinity(), Delta, Reason));

    return true;
}

// ============================================================================
// World clock (FApp fixed time step) - engage / restore round trip
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPieTimeWorldClockRoundTripTest,
    "PinWright.editor.step_frame.WorldClockRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPieTimeWorldClockRoundTripTest::RunTest(const FString& Parameters)
{
    const bool bBeforeUseFixed = FApp::UseFixedTimeStep();
    const double BeforeFixedDelta = FApp::GetFixedDeltaTime();

    ON_SCOPE_EXIT
    {
        // A leaked fixed time step detaches the whole editor's clock from wall time, so the
        // restore is guaranteed even if an assertion below returns early.
        PinWrightPieTime::Release();
    };

    PinWrightPieTime::SetWorldClockDelta(0.02);
    TestTrue(TEXT("the world clock reports engaged"), PinWrightPieTime::IsWorldClockEngaged());
    TestTrue(TEXT("FApp is on a fixed time step"), FApp::UseFixedTimeStep());
    TestEqual(TEXT("FApp's fixed delta is the requested step"), FApp::GetFixedDeltaTime(), 0.02, 0.0);

    // Re-engaging must move the delta without re-saving the (now modified) originals.
    PinWrightPieTime::SetWorldClockDelta(0.01);
    TestEqual(TEXT("re-engaging moves the delta"), FApp::GetFixedDeltaTime(), 0.01, 0.0);

    PinWrightPieTime::RestoreWorldClock();
    TestFalse(TEXT("the world clock reports released"), PinWrightPieTime::IsWorldClockEngaged());
    TestTrue(TEXT("FApp's fixed-time-step flag is restored"),
        FApp::UseFixedTimeStep() == bBeforeUseFixed);
    TestEqual(TEXT("FApp's fixed delta is restored"),
        FApp::GetFixedDeltaTime(), BeforeFixedDelta, 0.0);

    return true;
}

// ============================================================================
// UI clock (Slate.UseFixedDeltaTime + FSlateApplication::SetFixedDeltaTime)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPieTimeUiClockRoundTripTest,
    "PinWright.editor.pause.UiClockRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPieTimeUiClockRoundTripTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-slate"),
            TEXT("Skipped: Slate is not initialized, so the UMG animation clock has no lever to test."));
        return true;
    }

    IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Slate.UseFixedDeltaTime"));
    if (!CVar)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-slate-fixed-delta-cvar"),
            TEXT("Skipped: Slate.UseFixedDeltaTime is not registered in this build."));
        return true;
    }

    const bool bBeforeUseFixed = CVar->GetBool();
    const double BeforeFixedDelta = FSlateApplication::GetFixedDeltaTime();

    ON_SCOPE_EXIT
    {
        PinWrightPieTime::Release();
    };

    PinWrightPieTime::SetUiClockDelta(0.0);
    TestTrue(TEXT("the UI clock reports engaged"), PinWrightPieTime::IsUiClockEngaged());
    TestTrue(TEXT("Slate.UseFixedDeltaTime is on"), CVar->GetBool());
    TestEqual(TEXT("a freeze drives the Slate fixed delta to zero"),
        FSlateApplication::GetFixedDeltaTime(), 0.0, 0.0);

    PinWrightPieTime::SetUiClockDelta(0.025);
    TestEqual(TEXT("a step moves the Slate fixed delta to the step size"),
        FSlateApplication::GetFixedDeltaTime(), 0.025, 0.0);

    PinWrightPieTime::RestoreUiClock();
    TestFalse(TEXT("the UI clock reports released"), PinWrightPieTime::IsUiClockEngaged());
    TestTrue(TEXT("Slate.UseFixedDeltaTime is restored"), CVar->GetBool() == bBeforeUseFixed);
    TestEqual(TEXT("the Slate fixed delta is restored"),
        FSlateApplication::GetFixedDeltaTime(), BeforeFixedDelta, 0.0);

    return true;
}

// ============================================================================
// The animation clock actually advances by the step, and not at all when frozen
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPieTimeAnimationClockAdvancesByStepTest,
    "PinWright.editor.step_frame.AnimationClockAdvancesByStep",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPieTimeAnimationClockAdvancesByStepTest::RunTest(const FString& Parameters)
{
    if (!PieTimeControlTestHelpers::CanDriveSlateTick())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cannot-drive-slate-tick"),
            TEXT("Skipped: Slate is uninitialized or already ticking, so a hand-driven tick "
                 "cannot measure the widget-animation delta."));
        return true;
    }

    FSlateApplication& SlateApp = FSlateApplication::Get();

    float ObservedDelta = -1.0f;
    int32 ObservedTicks = 0;
    FDelegateHandle PreTickHandle = SlateApp.OnPreTick().AddLambda(
        [&ObservedDelta, &ObservedTicks](float Delta)
        {
            ObservedDelta = Delta;
            ++ObservedTicks;
        });

    ON_SCOPE_EXIT
    {
        SlateApp.OnPreTick().Remove(PreTickHandle);
        PinWrightPieTime::Release();
    };

    // A step: the animation clock must advance by exactly the requested delta, whatever the
    // real frame took.
    const double StepDelta = 0.0125;
    PinWrightPieTime::SetUiClockDelta(StepDelta);
    SlateApp.Tick(ESlateTickType::TimeAndWidgets);
    TestEqual(TEXT("the widget-animation tick fired once"), ObservedTicks, 1);
    TestEqual(TEXT("a stepped frame advances widget animations by exactly the step delta"),
        static_cast<double>(ObservedDelta), StepDelta, 1e-6);

    // A freeze: zero, so a playing animation holds its current frame across every RPC round
    // trip the caller makes while paused.
    ObservedDelta = -1.0f;
    PinWrightPieTime::SetUiClockDelta(0.0);
    SlateApp.Tick(ESlateTickType::TimeAndWidgets);
    TestEqual(TEXT("the widget-animation tick fired again"), ObservedTicks, 2);
    TestEqual(TEXT("a frozen frame advances widget animations by nothing"),
        static_cast<double>(ObservedDelta), 0.0, 1e-9);

    return true;
}

// ============================================================================
// Step refuses cleanly with no session, and leaves no clock engaged
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPieTimeStepWithoutSessionTest,
    "PinWright.editor.step_frame.RefusesWithoutSession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPieTimeStepWithoutSessionTest::RunTest(const FString& Parameters)
{
    if (GEditor && GEditor->PlayWorld)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pie-session-live"),
            TEXT("Skipped: a PIE session is running, so the no-session refusal cannot be exercised."));
        return true;
    }

    FString ErrorCode;
    FString Reason;
    bool bCompletionRan = false;
    const bool bStarted = PinWrightPieTime::Step(PinWrightPieTime::DefaultStepDeltaSeconds,
        [&bCompletionRan](const PinWrightPieTime::FStepOutcome&) { bCompletionRan = true; },
        ErrorCode, Reason);

    TestFalse(TEXT("a step without a PIE session does not start"), bStarted);
    TestEqual(TEXT("the refusal is NO_ACTIVE_SESSION"), ErrorCode, FString(TEXT("NO_ACTIVE_SESSION")));
    TestFalse(TEXT("the completion callback is not invoked for a refused step"), bCompletionRan);
    // A refused step must not have touched the process clock on its way out - that is the leak
    // that would leave the whole editor on a fixed time step with nothing to restore it.
    TestFalse(TEXT("a refused step engages no world clock"), PinWrightPieTime::IsWorldClockEngaged());
    TestFalse(TEXT("a refused step engages no UI clock"), PinWrightPieTime::IsUiClockEngaged());

    return true;
}

// ============================================================================
// The PIE world context's own tick - the lever that actually sizes a PIE world tick
// ============================================================================
//
// WHAT THIS PROVES. UEditorEngine::Tick does not
// tick a PIE world with the engine frame delta when the world context carries its own:
//   if (PieContext.PIEFixedTickSeconds > 0.f) TickDeltaSeconds = PieContext.PIEFixedTickSeconds;
//   else                                      TickDeltaSeconds = DeltaSeconds;
// (EditorEngine.cpp:2135-2143, the tick itself at :2169). While a step set only the FApp
// process fixed time step, that field stayed 0 and every step bought the editor's wall-clock
// frame instead of the requested delta - the same number for a 0.017 s and a 0.9 s request.
// Driving the engaged world clock against a world context this test owns measures exactly that
// field, with no PIE session and no engine world list involved.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPieTimeContextTickDeltaTest,
    "PinWright.editor.step_frame.PieContextTickDelta",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPieTimeContextTickDeltaTest::RunTest(const FString& Parameters)
{
    const bool bBeforeUseFixed = FApp::UseFixedTimeStep();
    const double BeforeFixedDelta = FApp::GetFixedDeltaTime();

    // Seeded as a session already running on a fixed PIE FPS (the Client/Server fixed-FPS play
    // settings do exactly this), so the restore has something other than the default to put back
    // and a step cannot pass by leaving the field at zero.
    FWorldContext Context;
    Context.WorldType = EWorldType::PIE;
    Context.ContextHandle = FName(TEXT("PinWrightPieTimeTestContext"));
    Context.PIEFixedTickSeconds = 1.0f / 20.0f;
    Context.PIEAccumulatedTickSeconds = 0.031f;

    ON_SCOPE_EXIT
    {
        PinWrightPieTime::ReleaseWorldClock(&Context);
        PinWrightPieTime::Release();
    };

    PinWrightPieTime::EngageWorldClock(0.15, &Context);
    TestTrue(TEXT("the world clock reports engaged"), PinWrightPieTime::IsWorldClockEngaged());
    TestEqual(TEXT("the process fixed delta is the requested step"),
        FApp::GetFixedDeltaTime(), 0.15, 0.0);
    TestEqual(TEXT("the PIE context's own tick is the requested step"),
        Context.PIEFixedTickSeconds, 0.15f, 0.0f);
    // Drained so the frames that follow buy one tick of the requested length, not that tick plus
    // whatever a previous fixed-FPS remainder had already banked.
    TestEqual(TEXT("the PIE context's tick accumulator is drained"),
        Context.PIEAccumulatedTickSeconds, 0.0f, 0.0f);

    // A second step, ~9x smaller, must move the tick the editor will use. This is the assertion
    // the reported defect fails: two requests a factor apart bought the same world advance.
    PinWrightPieTime::EngageWorldClock(0.017, &Context);
    TestEqual(TEXT("a second step re-sizes the PIE context's tick"),
        Context.PIEFixedTickSeconds, 0.017f, 0.0f);
    TestEqual(TEXT("a second step re-sizes the process fixed delta"),
        FApp::GetFixedDeltaTime(), 0.017, 0.0);

    PinWrightPieTime::ReleaseWorldClock(&Context);
    TestFalse(TEXT("the world clock reports released"), PinWrightPieTime::IsWorldClockEngaged());
    TestEqual(TEXT("the PIE context's own tick is restored, not zeroed"),
        Context.PIEFixedTickSeconds, 1.0f / 20.0f, 0.0f);
    TestEqual(TEXT("the PIE context's tick accumulator is restored"),
        Context.PIEAccumulatedTickSeconds, 0.031f, 0.0f);
    TestTrue(TEXT("FApp's fixed-time-step flag is restored"),
        FApp::UseFixedTimeStep() == bBeforeUseFixed);
    TestEqual(TEXT("FApp's fixed delta is restored"),
        FApp::GetFixedDeltaTime(), BeforeFixedDelta, 0.0);

    return true;
}

// ============================================================================
// What the level will actually allow for one tick, and how the response names it
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPieTimeWorldStepBudgetTest,
    "PinWright.editor.step_frame.WorldStepBudget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPieTimeWorldStepBudgetTest::RunTest(const FString& Parameters)
{
    // No world settings: the request stands and nothing may be blamed for clamping it.
    const PinWrightPieTime::FWorldStepBudget NoSettings =
        PinWrightPieTime::ResolveWorldStepBudget(nullptr, 0.05);
    TestEqual(TEXT("a settings-less world reports the request unchanged"),
        NoSettings.ExpectedSeconds, 0.05, 0.0);
    TestEqual(TEXT("a settings-less world reports no clamp"),
        FString(NoSettings.ClampSource), FString(TEXT("none")));

    // A step under MinUndilatedFrameTime buys MORE than asked - the direction the verb's own
    // documentation used to deny was possible, and the one a caller summing requested deltas
    // cannot detect.
    TestEqual(TEXT("an advance above the request names the world-settings floor"),
        FString(PinWrightPieTime::ClassifyWorldStepClamp(0.001, 0.01, 1.0)),
        FString(TEXT("worldSettings.MinUndilatedFrameTime")));
    TestEqual(TEXT("an advance below the request names the world-settings ceiling"),
        FString(PinWrightPieTime::ClassifyWorldStepClamp(0.9, 0.4, 1.0)),
        FString(TEXT("worldSettings.MaxUndilatedFrameTime")));
    TestEqual(TEXT("an unclamped step under dilation names the dilation"),
        FString(PinWrightPieTime::ClassifyWorldStepClamp(0.025, 0.025, 0.5)),
        FString(TEXT("timeDilation")));
    TestEqual(TEXT("an unclamped undilated step names nothing"),
        FString(PinWrightPieTime::ClassifyWorldStepClamp(0.05, 0.05, 1.0)),
        FString(TEXT("none")));

    return true;
}
