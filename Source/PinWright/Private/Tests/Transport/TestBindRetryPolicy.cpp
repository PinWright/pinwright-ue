// Copyright (c) 2026 Alexander Penkin. MIT License.

// Pins the bounded bind-retry schedule (Transport/BindRetryPolicy.h).
//
// THE DEFECT THIS PINS. FSocketHttpServer::Start bound the listener port exactly once. A
// measured sequence: an editor died in the render thread, a replacement booted before the
// dead process released the socket, the bind lost, one Error was logged, and that editor
// served MCP never again - a live process that reads to every caller as "editor down" while
// tasklist shows it running.
//
// The fix is a *timing policy*, so this test drives the policy directly with a fake clock.
// A test that could only observe the retry through a real socket would need a contested port,
// a second process and ten minutes of wall clock, and would therefore be skipped - which is
// how the missing retry survived. Every assertion below fails if the schedule is flattened,
// unbounded, or silently stops re-logging.

#include "Misc/AutomationTest.h"

#include "Transport/BindRetryPolicy.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightBindRetryTest
{
    // Drives Advance/NoteAttemptFailed from a fake clock and records the clock reading of
    // every authorised bind attempt. Never binds anything.
    struct FSimulation
    {
        TArray<double> AttemptTimes;
        TArray<double> RelogTimes;
        bool bExhausted = false;
        int32 FailedAttempts = 0;
    };

    // Steps a state from StartSeconds to StartSeconds + DurationSeconds in StepSeconds ticks,
    // failing every attempt it is offered. Mirrors UPinWrightSubsystem::TickBindRetry.
    FSimulation RunAllFailing(PinWrightBindRetry::FState& State, double StartSeconds,
                              double DurationSeconds, double StepSeconds)
    {
        FSimulation Sim;
        // Stepped by an integer index rather than by accumulating StepSeconds: the accumulated
        // form drifts by a few ULPs over thousands of ticks and can miss the final scheduled
        // attempt, which would make this test flaky in the direction that hides the bug.
        const int32 Steps = FMath::CeilToInt(DurationSeconds / StepSeconds);
        for (int32 Step = 0; Step <= Steps; ++Step)
        {
            const double Now = StartSeconds + (static_cast<double>(Step) * StepSeconds);
            switch (State.Advance(Now))
            {
            case PinWrightBindRetry::EAction::Attempt:
                Sim.AttemptTimes.Add(Now);
                State.NoteAttemptFailed(Now);
                break;
            case PinWrightBindRetry::EAction::Relog:
                Sim.RelogTimes.Add(Now);
                break;
            default:
                break;
            }
        }
        Sim.bExhausted = State.IsExhausted();
        Sim.FailedAttempts = State.GetFailedAttempts();
        return Sim;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBindRetryBackoffScheduleTest,
    "PinWright.transport.bind.Retry.BackoffScheduleDoublesAndCaps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBindRetryBackoffScheduleTest::RunTest(const FString& Parameters)
{
    PinWrightBindRetry::FPolicy Policy;
    Policy.FirstDelaySeconds = 2.0;
    Policy.MaxDelaySeconds = 30.0;

    // 2, 4, 8, 16, 32->capped 30, then flat at the cap. A flattened schedule (every delay
    // equal) or a runaway one (no cap) both fail here.
    TestEqual(TEXT("retry 1 waits the first delay"), Policy.DelayForAttempt(1), 2.0);
    TestEqual(TEXT("retry 2 doubles"), Policy.DelayForAttempt(2), 4.0);
    TestEqual(TEXT("retry 3 doubles again"), Policy.DelayForAttempt(3), 8.0);
    TestEqual(TEXT("retry 4 doubles again"), Policy.DelayForAttempt(4), 16.0);
    TestEqual(TEXT("retry 5 clamps to the cap instead of 32"), Policy.DelayForAttempt(5), 30.0);
    TestEqual(TEXT("retry 40 is still the cap"), Policy.DelayForAttempt(40), 30.0);

    // Attempt 0 is not a thing the owner asks for, but it must not produce a negative or
    // unbounded wait if it is.
    TestEqual(TEXT("a zeroth attempt is not scheduled sooner than the first"),
        Policy.DelayForAttempt(0), 2.0);

    // A cap below the first delay must clamp the first delay too, not invert the schedule.
    PinWrightBindRetry::FPolicy Tight;
    Tight.FirstDelaySeconds = 10.0;
    Tight.MaxDelaySeconds = 5.0;
    TestEqual(TEXT("the cap wins over a larger first delay"), Tight.DelayForAttempt(1), 5.0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBindRetryAttemptsFireOnCadenceTest,
    "PinWright.transport.bind.Retry.AttemptsFireOnTheBackoffCadence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBindRetryAttemptsFireOnCadenceTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightBindRetryTest;

    PinWrightBindRetry::FPolicy Policy;
    Policy.FirstDelaySeconds = 2.0;
    Policy.MaxDelaySeconds = 30.0;
    Policy.TotalBudgetSeconds = 600.0;

    PinWrightBindRetry::FState State;
    State.Begin(1000.0, Policy);

    // The startup bind counts as failure #1, so nothing is due at t=0.
    TestEqual(TEXT("the startup bind counts as the first failure"), State.GetFailedAttempts(), 1);
    TestEqual(TEXT("no attempt is due immediately"),
        static_cast<int32>(State.Advance(1000.0)),
        static_cast<int32>(PinWrightBindRetry::EAction::None));
    TestEqual(TEXT("no attempt is due one tick later"),
        static_cast<int32>(State.Advance(1000.1)),
        static_cast<int32>(PinWrightBindRetry::EAction::None));

    // Tick at the real ticker's 0.1s over the first ~minute.
    const FSimulation Sim = RunAllFailing(State, 1000.0, 70.0, 0.1);

    if (!TestTrue(TEXT("the first minute produces at least five attempts"),
                  Sim.AttemptTimes.Num() >= 5))
    {
        return false;
    }

    // Attempts land at +2, +6, +14, +30, +60 relative to the failed startup bind. Anything
    // that retries every tick (or never) misses these.
    const double Expected[] = { 1002.0, 1006.0, 1014.0, 1030.0, 1060.0 };
    for (int32 Index = 0; Index < 5; ++Index)
    {
        TestTrue(*FString::Printf(TEXT("attempt %d lands at t+%.1f (got t+%.1f)"),
                     Index + 1, Expected[Index] - 1000.0, Sim.AttemptTimes[Index] - 1000.0),
            FMath::IsNearlyEqual(Sim.AttemptTimes[Index], Expected[Index], 0.15));
    }

    // Bounded from below as well: a 0.1s ticker over 70s is 700 ticks, and the whole point is
    // that it does NOT try 700 times.
    TestTrue(TEXT("the first minute does not retry once per tick"), Sim.AttemptTimes.Num() < 10);
    TestFalse(TEXT("one minute does not exhaust a ten-minute budget"), Sim.bExhausted);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBindRetryGivesUpAtTheBudgetTest,
    "PinWright.transport.bind.Retry.GivesUpAtTheBudgetAndKeepsRelogging",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBindRetryGivesUpAtTheBudgetTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightBindRetryTest;

    PinWrightBindRetry::FPolicy Policy;
    Policy.FirstDelaySeconds = 2.0;
    Policy.MaxDelaySeconds = 30.0;
    Policy.TotalBudgetSeconds = 600.0;
    Policy.ExhaustedRelogSeconds = 300.0;

    PinWrightBindRetry::FState State;
    State.Begin(0.0, Policy);

    // Half an hour of ticks against a port nobody ever releases.
    const FSimulation Sim = RunAllFailing(State, 0.0, 1800.0, 0.5);

    TestTrue(TEXT("the budget is spent"), Sim.bExhausted);

    // Every attempt must be inside the budget: an unbounded retry keeps a permanently taken
    // port alive forever and hides a configuration problem behind infinite hope.
    for (double AttemptTime : Sim.AttemptTimes)
    {
        TestTrue(*FString::Printf(TEXT("attempt at t+%.1f is inside the %.0fs budget"),
                     AttemptTime, Policy.TotalBudgetSeconds),
            AttemptTime <= Policy.TotalBudgetSeconds);
    }

    // ...and the budget must actually be used, not abandoned after a couple of tries.
    TestTrue(TEXT("the last attempt is in the final third of the budget"),
        Sim.AttemptTimes.Num() > 0 && Sim.AttemptTimes.Last() > Policy.TotalBudgetSeconds * 0.66);

    // Attempt count: 2,4,8,16 then 30s steps to 600s -> roughly two dozen. Bounded on both
    // sides so a schedule change has to be deliberate.
    TestTrue(*FString::Printf(TEXT("attempt count %d is between 15 and 30"), Sim.AttemptTimes.Num()),
        Sim.AttemptTimes.Num() >= 15 && Sim.AttemptTimes.Num() <= 30);

    // The re-log is the whole discoverability story: one line at startup is invisible to
    // anyone who attaches to the log later. It must keep firing, on its cadence.
    TestTrue(TEXT("the terminal error is re-logged more than once"), Sim.RelogTimes.Num() >= 3);
    for (int32 Index = 1; Index < Sim.RelogTimes.Num(); ++Index)
    {
        const double Gap = Sim.RelogTimes[Index] - Sim.RelogTimes[Index - 1];
        TestTrue(*FString::Printf(TEXT("re-log gap %d is the %.0fs cadence (got %.1fs)"),
                     Index, Policy.ExhaustedRelogSeconds, Gap),
            FMath::IsNearlyEqual(Gap, Policy.ExhaustedRelogSeconds, 1.0));
    }

    // Exhausted is not the same as finished: the owner still reports PortInUse and still has
    // a re-log to emit, so the schedule stays live.
    TestTrue(TEXT("an exhausted schedule is still retrying-state"), State.IsRetrying());
    TestEqual(TEXT("no further attempt is ever authorised"),
        static_cast<int32>(State.Advance(100000.0)),
        static_cast<int32>(PinWrightBindRetry::EAction::Relog));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBindRetryStopsOnSuccessTest,
    "PinWright.transport.bind.Retry.SuccessMidSequenceStopsTheSchedule",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBindRetryStopsOnSuccessTest::RunTest(const FString& Parameters)
{
    PinWrightBindRetry::FPolicy Policy;
    Policy.FirstDelaySeconds = 2.0;
    Policy.MaxDelaySeconds = 30.0;
    Policy.TotalBudgetSeconds = 600.0;

    PinWrightBindRetry::FState State;
    State.Begin(0.0, Policy);

    // Two failed retries, then the dead process finally releases the socket.
    TestEqual(TEXT("retry 1 is authorised at t+2"),
        static_cast<int32>(State.Advance(2.0)),
        static_cast<int32>(PinWrightBindRetry::EAction::Attempt));
    State.NoteAttemptFailed(2.0);
    TestEqual(TEXT("retry 2 is authorised at t+6"),
        static_cast<int32>(State.Advance(6.0)),
        static_cast<int32>(PinWrightBindRetry::EAction::Attempt));

    State.NoteSucceeded();

    TestFalse(TEXT("a bound transport stops retrying"), State.IsRetrying());
    TestFalse(TEXT("a bound transport is not exhausted"), State.IsExhausted());
    TestEqual(TEXT("no attempt is authorised after success, however late"),
        static_cast<int32>(State.Advance(100000.0)),
        static_cast<int32>(PinWrightBindRetry::EAction::None));

    // A failure report arriving after success (a tick that raced the bind) must not resurrect
    // the schedule.
    State.NoteAttemptFailed(7.0);
    TestFalse(TEXT("a late failure report does not restart retrying"), State.IsRetrying());

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
