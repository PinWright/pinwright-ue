// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "State/JobRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobRegistryStartTest,
    "PinWright.state.job_registry.Start",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobRegistryStartTest::RunTest(const FString& Parameters)
{
    FJobRegistry Reg(/*TtlSeconds=*/3600, /*ProgressMinIntervalMs=*/0, /*MonitorLog=*/nullptr);
    const FString Id = Reg.Start(TEXT("test.method"), MakeShared<FJsonObject>());
    TestFalse(TEXT("ticket id non-empty"), Id.IsEmpty());

    FJobTicket Ticket;
    TestTrue(TEXT("ticket retrievable"), Reg.Get(Id, Ticket));
    TestEqual(TEXT("status running"), Ticket.Status, FString(TEXT("running")));
    TestEqual(TEXT("method matches"), Ticket.Method, FString(TEXT("test.method")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobRegistryCompleteTest,
    "PinWright.state.job_registry.Complete",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobRegistryCompleteTest::RunTest(const FString& Parameters)
{
    FJobRegistry Reg(3600, 0, nullptr);
    const FString Id = Reg.Start(TEXT("foo"), MakeShared<FJsonObject>());

    auto Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("count"), 42);
    Reg.Complete(Id, /*bSuccess=*/true, Result, FString());

    FJobTicket Ticket;
    TestTrue(TEXT("found"), Reg.Get(Id, Ticket));
    TestEqual(TEXT("completed"), Ticket.Status, FString(TEXT("completed")));
    TestTrue(TEXT("result preserved"), Ticket.Result.IsValid()
        && Ticket.Result->GetNumberField(TEXT("count")) == 42);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobRegistryFailTest,
    "PinWright.state.job_registry.Fail",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobRegistryFailTest::RunTest(const FString& Parameters)
{
    FJobRegistry Reg(3600, 0, nullptr);
    const FString Id = Reg.Start(TEXT("foo"), MakeShared<FJsonObject>());
    Reg.Complete(Id, /*bSuccess=*/false, nullptr, TEXT("BOOM"));

    FJobTicket Ticket;
    Reg.Get(Id, Ticket);
    TestEqual(TEXT("failed"), Ticket.Status, FString(TEXT("failed")));
    TestEqual(TEXT("error preserved"), Ticket.Error, FString(TEXT("BOOM")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobRegistryCancelTest,
    "PinWright.state.job_registry.Cancel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobRegistryCancelTest::RunTest(const FString& Parameters)
{
    FJobRegistry Reg(3600, 0, nullptr);
    const FString Id = Reg.Start(TEXT("foo"), MakeShared<FJsonObject>());

    bool bCancelCalled = false;
    Reg.SetCancelCallback(Id, [&]() { bCancelCalled = true; });
    TestTrue(TEXT("cancel is honoured for a verb that registered a hook"),
        Reg.Cancel(Id) == EJobCancelResult::Requested);
    TestTrue(TEXT("callback fired"), bCancelCalled);

    FJobTicket Ticket;
    Reg.Get(Id, Ticket);
    TestEqual(TEXT("cancelled status"), Ticket.Status, FString(TEXT("cancelled")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobRegistryEvictTest,
    "PinWright.state.job_registry.Evict",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobRegistryEvictTest::RunTest(const FString& Parameters)
{
    FJobRegistry Reg(/*TtlSeconds=*/0, 0, nullptr);
    const FString Id = Reg.Start(TEXT("foo"), MakeShared<FJsonObject>());
    Reg.Complete(Id, true, MakeShared<FJsonObject>(), FString());
    Reg.EvictExpired(FDateTime::UtcNow() + FTimespan::FromSeconds(1));

    FJobTicket Ticket;
    TestFalse(TEXT("evicted"), Reg.Get(Id, Ticket));
    return true;
}

// ---------------------------------------------------------------------------
// Cancellation honesty. system.job_cancel used to answer {cancelled:true} for any ticket that
// happened to be in "running", whether or not anything could stop the work — 16 of 19 ticketed
// verbs register no cancel hook at all. That is the worst possible place for a false success,
// because cancelling is what an agent does IN RESPONSE TO A HANG: it reported "stopped", the
// agent proceeded believing the editor was idle, and one recorded run wrote ~19,700 more files.
//
// These tests assert the FAILURE direction — that an uncancellable job says so — plus the two
// state consequences that make the lie expensive rather than merely untidy.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobRegistryCancelUnsupportedTest,
    "PinWright.state.job_registry.CancelReportsUnsupportedWithoutHook",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobRegistryCancelUnsupportedTest::RunTest(const FString& Parameters)
{
    FJobRegistry Reg(3600, 0, nullptr);
    // No SetCancelCallback: this is the shape of level.build_lighting, system.run_ubt,
    // mrq.run_jobs, render.nanite_rebuild_mesh and the eleven other verbs that cannot be stopped.
    const FString Id = Reg.Start(TEXT("level.build_lighting"), MakeShared<FJsonObject>());

    TestTrue(TEXT("a verb with no cancel hook reports Unsupported, not success"),
        Reg.Cancel(Id) == EJobCancelResult::Unsupported);

    // Consequence 1: the ticket must NOT be marked cancelled. The work is still running, and a
    // ticket that reads "cancelled" is the same lie one layer down — system.job_status would
    // corroborate the false success the caller was just refused.
    FJobTicket Ticket;
    TestTrue(TEXT("ticket still retrievable"), Reg.Get(Id, Ticket));
    TestEqual(TEXT("an uncancellable job stays 'running'"),
        Ticket.Status, FString(TEXT("running")));

    // Consequence 2: because the ticket was left running, the job's REAL outcome still lands.
    // Complete() refuses to overwrite a terminal ticket, so the old flip-to-cancelled silently
    // discarded the result of every job it failed to stop.
    auto Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("built"), 7);
    Reg.Complete(Id, true, Result, FString());
    TestTrue(TEXT("ticket retrievable after completion"), Reg.Get(Id, Ticket));
    TestEqual(TEXT("the real outcome is not discarded"),
        Ticket.Status, FString(TEXT("completed")));
    TestTrue(TEXT("the real result survives"),
        Ticket.Result.IsValid() && Ticket.Result->GetNumberField(TEXT("built")) == 7);
    return true;
}

// An unsupported cancel must be inspection-only: no observer event, no monitor-log line. A
// "cancelled" broadcast would make the SSE bridge terminate the caller's stream with
// JOB_CANCELLED (PinWrightSubsystem) for a job that is still running.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobRegistryCancelUnsupportedIsSilentTest,
    "PinWright.state.job_registry.UnsupportedCancelBroadcastsNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobRegistryCancelUnsupportedIsSilentTest::RunTest(const FString& Parameters)
{
    FJobRegistry Reg(3600, 0, nullptr);
    TArray<FString> Events;
    Reg.OnJobEvent().AddLambda(
        [&Events](const FString&, const FString& Event,
                  const TSharedPtr<FJsonObject>&, const TSharedPtr<FJsonObject>&)
        {
            Events.Add(Event);
        });

    const FString Id = Reg.Start(TEXT("mrq.run_jobs"), MakeShared<FJsonObject>());
    TestTrue(TEXT("cancel is unsupported"), Reg.Cancel(Id) == EJobCancelResult::Unsupported);

    TestEqual(TEXT("only the 'started' event fired"), Events.Num(), 1);
    if (Events.Num() > 0)
    {
        TestEqual(TEXT("no 'cancelled' event"), Events[0], FString(TEXT("started")));
    }
    return true;
}

// The two remaining outcomes a bare bool also collapsed together. They need different
// recoveries: NotRunning means job_status has the answer, NotFound means it never will.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobRegistryCancelRejectionsTest,
    "PinWright.state.job_registry.CancelDistinguishesMissingFromTerminal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobRegistryCancelRejectionsTest::RunTest(const FString& Parameters)
{
    FJobRegistry Reg(3600, 0, nullptr);

    TestTrue(TEXT("an unknown ticket id is NotFound"),
        Reg.Cancel(TEXT("j_does_not_exist")) == EJobCancelResult::NotFound);

    const FString Id = Reg.Start(TEXT("foo"), MakeShared<FJsonObject>());
    Reg.SetCancelCallback(Id, [](){});
    Reg.Complete(Id, true, MakeShared<FJsonObject>(), FString());
    TestTrue(TEXT("an already-terminal ticket is NotRunning"),
        Reg.Cancel(Id) == EJobCancelResult::NotRunning);

    // Cancelling twice must not double-fire the hook. Complete() clears the callback, and a
    // terminal ticket is refused before the hook is ever reached.
    int32 HookCalls = 0;
    const FString Id2 = Reg.Start(TEXT("bar"), MakeShared<FJsonObject>());
    Reg.SetCancelCallback(Id2, [&HookCalls](){ ++HookCalls; });
    TestTrue(TEXT("first cancel is honoured"),
        Reg.Cancel(Id2) == EJobCancelResult::Requested);
    TestTrue(TEXT("second cancel is NotRunning"),
        Reg.Cancel(Id2) == EJobCancelResult::NotRunning);
    TestEqual(TEXT("cancel hook fired exactly once"), HookCalls, 1);
    return true;
}

// Characterization of a SEPARATE, unfixed finding, pinned here so it cannot change by accident.
//
// EvictExpired skips tickets whose status is "running", and a running ticket's CompletedAt is
// never written, so there is no registry-level deadline that can retire one. A job whose
// completion delegate never fires therefore leaks a "running" ticket for the lifetime of the
// editor process. This was flagged as a suspected cause of tickets stuck in "running", but no
// live stuck ticket was ever demonstrated, and any deadline would have to be long enough not to
// kill a legitimate multi-hour Lightmass build — so the behaviour is deliberately left as-is and
// merely documented. If a deadline is ever added, this test is the thing that should be updated
// on purpose rather than discovered by surprise.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobRegistryRunningTicketSurvivesEvictionTest,
    "PinWright.state.job_registry.RunningTicketIsNeverEvicted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobRegistryRunningTicketSurvivesEvictionTest::RunTest(const FString& Parameters)
{
    FJobRegistry Reg(/*TtlSeconds=*/0, 0, nullptr);
    const FString Id = Reg.Start(TEXT("foo"), MakeShared<FJsonObject>());
    // Far past any plausible TTL.
    Reg.EvictExpired(FDateTime::UtcNow() + FTimespan::FromDays(30));

    FJobTicket Ticket;
    TestTrue(TEXT("a running ticket survives eviction regardless of age"), Reg.Get(Id, Ticket));
    TestEqual(TEXT("and is still running"), Ticket.Status, FString(TEXT("running")));
    return true;
}
