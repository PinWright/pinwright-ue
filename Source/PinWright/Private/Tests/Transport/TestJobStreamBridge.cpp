// Copyright (c) 2026 Alexander Penkin. MIT License.

// FJobRegistry observer (OnJobEvent) tests — the job->stream bridge contract:
// event strings mirror the monitor-log vocabulary (started | progress |
// completed | failed | cancelled), payload/result plumbing, the
// bBypassRateLimit escape hatch on RecordProgress, and lock hygiene
// (broadcast fires OUTSIDE the registry mutex, so subscribers may call back
// into the registry). Pure registry tests — no sockets.
#include "Misc/AutomationTest.h"
#include "State/JobRegistry.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace PinWrightJobStreamBridgeTest
{
    // One observed OnJobEvent broadcast, recorded verbatim.
    struct FObservedEvent
    {
        FString TicketId;
        FString Event;
        TSharedPtr<FJsonObject> Payload;
        TSharedPtr<FJsonObject> Result;
    };

    inline FString SerializeObject(const TSharedPtr<FJsonObject>& Obj)
    {
        if (!Obj.IsValid())
        {
            return FString();
        }
        FString Out;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
        Writer->Close();
        return Out;
    }

    // A field may ride directly on the observer payload or nested one level
    // under "payload" — both placements satisfy the bridge contract.
    inline bool PayloadCarriesNumber(const TSharedPtr<FJsonObject>& Payload,
        const FString& Field, double Expected)
    {
        if (!Payload.IsValid())
        {
            return false;
        }
        double Value = 0.0;
        if (Payload->TryGetNumberField(Field, Value) && Value == Expected)
        {
            return true;
        }
        const TSharedPtr<FJsonObject>* Nested = nullptr;
        return Payload->TryGetObjectField(TEXT("payload"), Nested) && Nested &&
            (*Nested)->TryGetNumberField(Field, Value) && Value == Expected;
    }
}

// ============================================================================
// Event strings + payloads: started / progress / completed / cancelled /
// failed all fire with the right ticket, payload, and terminal result.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobStreamBridgeEventStringsTest,
    "PinWright.transport.job_stream_bridge.EventStringsAndPayloads",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobStreamBridgeEventStringsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightJobStreamBridgeTest;

    FJobRegistry Reg(/*TtlSeconds=*/3600, /*ProgressMinIntervalMs=*/0, /*MonitorLog=*/nullptr);
    TArray<FObservedEvent> Observed;
    Reg.OnJobEvent().AddLambda([&Observed](const FString& TicketId, const FString& Event,
        const TSharedPtr<FJsonObject>& Payload, const TSharedPtr<FJsonObject>& Result)
        {
            Observed.Add({TicketId, Event, Payload, Result});
        });

    // --- started ---
    const FString Id = Reg.Start(TEXT("test.method"), MakeShared<FJsonObject>());
    if (!TestEqual(TEXT("one event after Start"), Observed.Num(), 1))
    {
        return true;
    }
    TestEqual(TEXT("Start fires 'started'"), Observed[0].Event, FString(TEXT("started")));
    TestEqual(TEXT("'started' carries the ticket id"), Observed[0].TicketId, Id);
    TestFalse(TEXT("'started' carries no terminal result"), Observed[0].Result.IsValid());

    // --- progress ---
    TSharedPtr<FJsonObject> ProgressPayload = MakeShared<FJsonObject>();
    ProgressPayload->SetNumberField(TEXT("pct"), 50);
    TestTrue(TEXT("RecordProgress accepted"),
        Reg.RecordProgress(Id, TEXT("halfway there"), ProgressPayload));
    if (!TestEqual(TEXT("two events after RecordProgress"), Observed.Num(), 2))
    {
        return true;
    }
    TestEqual(TEXT("RecordProgress fires 'progress'"), Observed[1].Event, FString(TEXT("progress")));
    TestEqual(TEXT("'progress' carries the ticket id"), Observed[1].TicketId, Id);
    TestTrue(TEXT("'progress' carries a payload"), Observed[1].Payload.IsValid());
    TestTrue(TEXT("'progress' payload carries the caller's pct=50 (direct or nested)"),
        PayloadCarriesNumber(Observed[1].Payload, TEXT("pct"), 50.0) ||
        SerializeObject(Observed[1].Payload).Contains(TEXT("halfway there")));

    // --- completed (carries Result) ---
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("answer"), 42);
    Reg.Complete(Id, /*bSuccess=*/true, Result, FString());
    if (!TestEqual(TEXT("three events after Complete"), Observed.Num(), 3))
    {
        return true;
    }
    TestEqual(TEXT("Complete(success) fires 'completed'"),
        Observed[2].Event, FString(TEXT("completed")));
    TestTrue(TEXT("'completed' carries the terminal Result"),
        Observed[2].Result.IsValid() &&
        Observed[2].Result->GetNumberField(TEXT("answer")) == 42);

    // --- cancelled ---
    const FString Id2 = Reg.Start(TEXT("test.method2"), MakeShared<FJsonObject>());
    Reg.SetCancelCallback(Id2, []() {});
    TestTrue(TEXT("Cancel accepted"), Reg.Cancel(Id2) == EJobCancelResult::Requested);
    if (!TestEqual(TEXT("five events after second Start + Cancel"), Observed.Num(), 5))
    {
        return true;
    }
    TestEqual(TEXT("second Start fires 'started'"), Observed[3].Event, FString(TEXT("started")));
    TestEqual(TEXT("Cancel fires 'cancelled'"), Observed[4].Event, FString(TEXT("cancelled")));
    TestEqual(TEXT("'cancelled' carries its ticket id"), Observed[4].TicketId, Id2);

    // --- failed (error rides in the payload) ---
    const FString Id3 = Reg.Start(TEXT("test.method3"), MakeShared<FJsonObject>());
    Reg.Complete(Id3, /*bSuccess=*/false, nullptr, TEXT("BOOM"));
    if (!TestEqual(TEXT("seven events after third Start + failing Complete"), Observed.Num(), 7))
    {
        return true;
    }
    TestEqual(TEXT("Complete(failure) fires 'failed'"), Observed[6].Event, FString(TEXT("failed")));
    TestEqual(TEXT("'failed' carries its ticket id"), Observed[6].TicketId, Id3);
    TestTrue(TEXT("'failed' payload carries the error text"),
        Observed[6].Payload.IsValid() &&
        SerializeObject(Observed[6].Payload).Contains(TEXT("BOOM")));
    return true;
}

// ============================================================================
// Throttle: two RecordProgress calls inside ProgressMinIntervalMs with
// bBypassRateLimit=false fire the observer exactly once.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobStreamBridgeThrottleOnceTest,
    "PinWright.transport.job_stream_bridge.ThrottledProgressFiresOnce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobStreamBridgeThrottleOnceTest::RunTest(const FString& Parameters)
{
    FJobRegistry Reg(/*TtlSeconds=*/3600, /*ProgressMinIntervalMs=*/60000, /*MonitorLog=*/nullptr);
    int32 ProgressEvents = 0;
    Reg.OnJobEvent().AddLambda([&ProgressEvents](const FString&, const FString& Event,
        const TSharedPtr<FJsonObject>&, const TSharedPtr<FJsonObject>&)
        {
            if (Event == TEXT("progress"))
            {
                ++ProgressEvents;
            }
        });

    const FString Id = Reg.Start(TEXT("test.throttled"), MakeShared<FJsonObject>());
    TestTrue(TEXT("first progress accepted"),
        Reg.RecordProgress(Id, TEXT("p1"), nullptr, /*bBypassRateLimit=*/false));
    TestFalse(TEXT("second progress inside the interval is throttled"),
        Reg.RecordProgress(Id, TEXT("p2"), nullptr, /*bBypassRateLimit=*/false));
    TestEqual(TEXT("observer fired exactly once for progress"), ProgressEvents, 1);

    Reg.Complete(Id, true, MakeShared<FJsonObject>(), FString());
    return true;
}

// ============================================================================
// Bypass: the same two back-to-back calls with bBypassRateLimit=true fire
// the observer both times (streamed jobs want every event).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobStreamBridgeBypassFiresBothTest,
    "PinWright.transport.job_stream_bridge.BypassRateLimitFiresBoth",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobStreamBridgeBypassFiresBothTest::RunTest(const FString& Parameters)
{
    FJobRegistry Reg(/*TtlSeconds=*/3600, /*ProgressMinIntervalMs=*/60000, /*MonitorLog=*/nullptr);
    int32 ProgressEvents = 0;
    Reg.OnJobEvent().AddLambda([&ProgressEvents](const FString&, const FString& Event,
        const TSharedPtr<FJsonObject>&, const TSharedPtr<FJsonObject>&)
        {
            if (Event == TEXT("progress"))
            {
                ++ProgressEvents;
            }
        });

    const FString Id = Reg.Start(TEXT("test.bypass"), MakeShared<FJsonObject>());
    TestTrue(TEXT("first bypass progress accepted"),
        Reg.RecordProgress(Id, TEXT("p1"), nullptr, /*bBypassRateLimit=*/true));
    TestTrue(TEXT("second bypass progress accepted despite the interval"),
        Reg.RecordProgress(Id, TEXT("p2"), nullptr, /*bBypassRateLimit=*/true));
    TestEqual(TEXT("observer fired for both bypassed events"), ProgressEvents, 2);

    Reg.Complete(Id, true, MakeShared<FJsonObject>(), FString());
    return true;
}

// ============================================================================
// Lock hygiene: the broadcast happens OUTSIDE the registry mutex, so a
// subscriber that calls back into the registry (Get) must return normally.
// (On a platform with non-reentrant mutexes, an inside-the-lock broadcast
// deadlocks here; on re-entrant ones this still documents the contract.)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobStreamBridgeReentrantGetTest,
    "PinWright.transport.job_stream_bridge.BroadcastOutsideLockReentrantGet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobStreamBridgeReentrantGetTest::RunTest(const FString& Parameters)
{
    FJobRegistry Reg(/*TtlSeconds=*/3600, /*ProgressMinIntervalMs=*/0, /*MonitorLog=*/nullptr);
    bool bGetReturnedInsideProgress = false;
    FString StatusInsideProgress;
    bool bGetReturnedInsideCompleted = false;
    FString StatusInsideCompleted;

    Reg.OnJobEvent().AddLambda([&](const FString& TicketId, const FString& Event,
        const TSharedPtr<FJsonObject>&, const TSharedPtr<FJsonObject>&)
        {
            FJobTicket Ticket;
            if (Event == TEXT("progress") && Reg.Get(TicketId, Ticket))
            {
                bGetReturnedInsideProgress = true;
                StatusInsideProgress = Ticket.Status;
            }
            if (Event == TEXT("completed") && Reg.Get(TicketId, Ticket))
            {
                bGetReturnedInsideCompleted = true;
                StatusInsideCompleted = Ticket.Status;
            }
        });

    const FString Id = Reg.Start(TEXT("test.reentrant"), MakeShared<FJsonObject>());
    Reg.RecordProgress(Id, TEXT("step"));
    TestTrue(TEXT("Get() returned inside the progress callback (no deadlock)"),
        bGetReturnedInsideProgress);
    TestEqual(TEXT("ticket is 'running' during the progress broadcast"),
        StatusInsideProgress, FString(TEXT("running")));

    Reg.Complete(Id, true, MakeShared<FJsonObject>(), FString());
    TestTrue(TEXT("Get() returned inside the completed callback (no deadlock)"),
        bGetReturnedInsideCompleted);
    TestEqual(TEXT("ticket is 'completed' during the completed broadcast"),
        StatusInsideCompleted, FString(TEXT("completed")));
    return true;
}
