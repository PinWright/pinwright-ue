// Copyright (c) 2026 Alexander Penkin. MIT License.

// The ambient progress sink — the slot that lets code PinWright did not write (a user
// script inside python.execute) report MCP progress without being handed a token.
//
// The contract under test is deliberately conservative in one direction: Report() must
// be a harmless no-op when nobody is listening. A script that calls report_progress
// unconditionally has to behave identically whether or not the caller opened a stream,
// because the alternative is a script that works when watched and throws when not.
#include "Misc/AutomationTest.h"
#include "PinWrightSettings.h"
#include "State/ActiveProgressSink.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"

// ============================================================================
// No ambient ticket: reporting is false and harmless, and nothing claims to be
// observed. This is the ordinary plain-JSON python.execute path.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActiveProgressSinkInactiveTest,
    "PinWright.state.progress_sink.ReportingWithNoListenerIsAHarmlessFalse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FActiveProgressSinkInactiveTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("no ambient ticket outside a scope"),
        PinWright::Progress::ActiveTicketId().IsEmpty());
    TestFalse(TEXT("nothing is observing"), PinWright::Progress::IsActive());
    TestFalse(TEXT("reporting with no listener returns false"),
        PinWright::Progress::Report(TEXT("step 1"), 1.0, 10.0));

    // An empty ticket id is the explicit "nobody is listening" publication — the
    // non-streaming handler path constructs the scope this way, so it must not become
    // active by accident.
    {
        // Braces, not parens: FScopedSink Sink(FString()) declares a function.
        PinWright::Progress::FScopedSink Sink{FString()};
        TestFalse(TEXT("an empty-ticket scope does not make progress observed"),
            PinWright::Progress::IsActive());
        TestFalse(TEXT("reporting inside an empty-ticket scope is still false"),
            PinWright::Progress::Report(TEXT("step 1"), 1.0, 10.0));
    }
    return true;
}

// ============================================================================
// Scope discipline: the slot is restored on exit, including from a nested
// scope. A leaked sink would address a later, unrelated RPC's progress to a
// finished ticket.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActiveProgressSinkScopingTest,
    "PinWright.state.progress_sink.ScopeRestoresThePreviousSinkIncludingNested",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FActiveProgressSinkScopingTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("clean slate"), PinWright::Progress::ActiveTicketId().IsEmpty());
    {
        PinWright::Progress::FScopedSink Outer(TEXT("ticket-outer"));
        TestEqual(TEXT("outer sink published"),
            PinWright::Progress::ActiveTicketId(), FString(TEXT("ticket-outer")));
        {
            PinWright::Progress::FScopedSink Inner(TEXT("ticket-inner"));
            TestEqual(TEXT("inner sink shadows the outer"),
                PinWright::Progress::ActiveTicketId(), FString(TEXT("ticket-inner")));
        }
        TestEqual(TEXT("outer sink restored after the inner scope"),
            PinWright::Progress::ActiveTicketId(), FString(TEXT("ticket-outer")));
    }
    TestTrue(TEXT("slot is empty again after the outer scope"),
        PinWright::Progress::ActiveTicketId().IsEmpty());
    return true;
}

// ============================================================================
// The live path: a real running ticket in the process registry receives the
// reported event, in the reporter's own units. Reporting against a ticket that
// has already finished returns false rather than resurrecting it.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActiveProgressSinkDeliversTest,
    "PinWright.state.progress_sink.ReportReachesTheRunningTicketAndStopsAtTerminal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FActiveProgressSinkDeliversTest::RunTest(const FString& Parameters)
{
    FJobRegistry& Reg = FPluginState::Get().GetJobRegistry();
    const FString TicketId = Reg.Start(TEXT("test.progress_sink"), MakeShared<FJsonObject>());

    {
        PinWright::Progress::FScopedSink Sink(TicketId);
        TestTrue(TEXT("a running ticket is observed"),
            PinWright::Progress::IsActive());
        TestTrue(TEXT("report accepted against the running ticket"),
            PinWright::Progress::Report(TEXT("actor 412 of 1660"), 412.0, 1660.0));

        FJobTicket Ticket;
        if (TestTrue(TEXT("ticket readable"), Reg.Get(TicketId, Ticket)) &&
            TestEqual(TEXT("the event landed on the ticket"), Ticket.Progress.Num(), 1))
        {
            const TSharedPtr<FJsonObject>& Payload = Ticket.Progress[0].Payload;
            double Value = 0.0;
            TestTrue(TEXT("the reporter's numerator was carried"),
                Payload.IsValid() && Payload->TryGetNumberField(TEXT("progress"), Value));
            TestEqual(TEXT("numerator value"), Value, 412.0);
            double Total = 0.0;
            TestTrue(TEXT("the reporter's denominator was carried"),
                Payload.IsValid() && Payload->TryGetNumberField(TEXT("total"), Total));
            TestEqual(TEXT("denominator value"), Total, 1660.0);
            TestEqual(TEXT("the message is the reporter's own text"),
                Ticket.Progress[0].Message, FString(TEXT("actor 412 of 1660")));
        }

        // Rate limiting is the point, not an accident: reporting every iteration of a
        // tight loop must be safe, so an immediate second report is throttled rather than
        // flooding the client. The sink necessarily uses the process registry, whose
        // interval is a user setting - so read it rather than assume the default, or this
        // test fails on a host that configured 0. (Zero-total omission is covered at the
        // registry level in PinWright.state.job_progress.CallerUnitsAndTotalArePublished,
        // which owns its own registry and can set the interval it wants.)
        const int32 IntervalMs = GetDefault<UPinWrightSettings>()->ProgressEventMinIntervalMs;
        const bool bSecondAccepted = PinWright::Progress::Report(TEXT("counting"), 413.0, 0.0);
        if (IntervalMs > 0)
        {
            TestFalse(TEXT("an immediate second report is rate-limited, not flooded"),
                bSecondAccepted);
            if (TestTrue(TEXT("ticket still readable"), Reg.Get(TicketId, Ticket)))
            {
                TestEqual(TEXT("the throttled report added no second event"),
                    Ticket.Progress.Num(), 1);
            }
        }
        else
        {
            TestTrue(TEXT("with throttling disabled the second report is accepted"),
                bSecondAccepted);
        }

        // Failure direction: once the job is terminal, reporting must stop succeeding.
        Reg.Complete(TicketId, /*bSuccess=*/true, MakeShared<FJsonObject>(), FString());
        TestFalse(TEXT("a completed ticket is no longer observed"),
            PinWright::Progress::IsActive());
        TestFalse(TEXT("reporting after completion returns false"),
            PinWright::Progress::Report(TEXT("too late"), 999.0, 1660.0));
    }
    return true;
}
