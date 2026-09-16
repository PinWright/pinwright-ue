// Copyright (c) 2026 Alexander Penkin. MIT License.

// The MCP `progress` number FJobRegistry publishes, against the two rules the spec
// states as MUSTs (2025-06-18, Progress): the value increases with every notification,
// and it is expressed in whatever units the reporter chose.
//
// The shipped defect these pin: no verb anywhere set a `progress` number, so every frame
// fell back to the ticket's Progress array length — which is ring-trimmed to the newest
// 50 events. A long job (system.run_tests emits ~380 heartbeats) therefore counted 1..50
// and then reported 50 forever, which is precisely a "MUST increase" violation on the
// jobs that most need progress. The failure was invisible because the number was still
// well-formed.
#include "Misc/AutomationTest.h"
#include "State/JobRegistry.h"

namespace PinWrightJobProgressTest
{
    // The published progress number for the most recent broadcast, or -1 if none.
    struct FProgressWatcher
    {
        double LastProgress = -1.0;
        double LastTotal = -1.0;
        bool bLastHadTotal = false;
        int32 Count = 0;
        bool bMonotonic = true;

        void Observe(const TSharedPtr<FJsonObject>& Payload)
        {
            if (!Payload.IsValid())
            {
                return;
            }
            double Value = 0.0;
            if (!Payload->TryGetNumberField(TEXT("progress"), Value))
            {
                return;
            }
            if (Count > 0 && Value <= LastProgress)
            {
                bMonotonic = false;
            }
            LastProgress = Value;
            ++Count;
            bLastHadTotal = Payload->TryGetNumberField(TEXT("total"), LastTotal);
        }
    };
}

// ============================================================================
// The value must keep climbing well past the 50-event ring buffer. 120 events
// on the old code reported 50; the assertion is on the last value, so it fails
// loudly rather than merely losing resolution.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobProgressOutlastsRingBufferTest,
    "PinWright.state.job_progress.ValueKeepsIncreasingPastTheRingBuffer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobProgressOutlastsRingBufferTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightJobProgressTest;

    FJobRegistry Reg(/*TtlSeconds=*/3600, /*ProgressMinIntervalMs=*/0, /*MonitorLog=*/nullptr);
    FProgressWatcher Watcher;
    Reg.OnJobEvent().AddLambda([&Watcher](const FString&, const FString& Event,
        const TSharedPtr<FJsonObject>& Payload, const TSharedPtr<FJsonObject>&)
        {
            if (Event == TEXT("progress"))
            {
                Watcher.Observe(Payload);
            }
        });

    const FString Id = Reg.Start(TEXT("test.longjob"), MakeShared<FJsonObject>());

    constexpr int32 EventCount = 120;   // well past MaxProgressEvents (50)
    for (int32 Index = 0; Index < EventCount; ++Index)
    {
        Reg.RecordProgress(Id, FString::Printf(TEXT("step %d"), Index), nullptr,
            /*bBypassRateLimit=*/true);
    }

    TestEqual(TEXT("every event was broadcast"), Watcher.Count, EventCount);
    TestTrue(TEXT("the published progress value never failed to increase"), Watcher.bMonotonic);
    TestEqual(TEXT("the last value counts all events, not the trimmed ring"),
        Watcher.LastProgress, static_cast<double>(EventCount));

    // The ring itself is still trimmed — that is the memory bound, and it must stay.
    FJobTicket Ticket;
    if (TestTrue(TEXT("ticket readable"), Reg.Get(Id, Ticket)))
    {
        TestEqual(TEXT("the stored event ring is still capped at 50"), Ticket.Progress.Num(), 50);
    }

    Reg.Complete(Id, true, MakeShared<FJsonObject>(), FString());
    return true;
}

// ============================================================================
// A reporter's own units and denominator are published verbatim, and a total
// is OMITTED when the reporter does not know one — a zero total renders as a
// finished bar, which is the wrong lie to tell about an unknown denominator.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobProgressCallerUnitsTest,
    "PinWright.state.job_progress.CallerUnitsAndTotalArePublished",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobProgressCallerUnitsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightJobProgressTest;

    FJobRegistry Reg(/*TtlSeconds=*/3600, /*ProgressMinIntervalMs=*/0, /*MonitorLog=*/nullptr);
    FProgressWatcher Watcher;
    Reg.OnJobEvent().AddLambda([&Watcher](const FString&, const FString& Event,
        const TSharedPtr<FJsonObject>& Payload, const TSharedPtr<FJsonObject>&)
        {
            if (Event == TEXT("progress"))
            {
                Watcher.Observe(Payload);
            }
        });

    const FString Id = Reg.Start(TEXT("test.units"), MakeShared<FJsonObject>());

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("progress"), 412);
    Payload->SetNumberField(TEXT("total"), 1660);
    Reg.RecordProgress(Id, TEXT("grounding actor 412 of 1660"), Payload, /*bBypassRateLimit=*/true);

    TestEqual(TEXT("the caller's numerator is published, not an event count"),
        Watcher.LastProgress, 412.0);
    TestTrue(TEXT("the caller's denominator is published"), Watcher.bLastHadTotal);
    TestEqual(TEXT("denominator value"), Watcher.LastTotal, 1660.0);

    // No total supplied: the field must be absent, not zero.
    TSharedPtr<FJsonObject> NoTotal = MakeShared<FJsonObject>();
    NoTotal->SetNumberField(TEXT("progress"), 413);
    Reg.RecordProgress(Id, TEXT("no denominator known"), NoTotal, /*bBypassRateLimit=*/true);
    TestFalse(TEXT("an unknown total is omitted rather than sent as 0"), Watcher.bLastHadTotal);

    Reg.Complete(Id, true, MakeShared<FJsonObject>(), FString());
    return true;
}

// ============================================================================
// Failure direction: a reporter that goes backwards must not reorder the job in
// the client's eyes — but it must not be papered over with invented movement
// either. The value is HELD at the previous one, and the audit line still shows
// what was actually reported. Holding silently would hide a real miscount.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobProgressBackwardsClampedTest,
    "PinWright.state.job_progress.BackwardsValueIsHeldNotInvented",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobProgressBackwardsClampedTest::RunTest(const FString& Parameters)
{
    FJobRegistry Reg(/*TtlSeconds=*/3600, /*ProgressMinIntervalMs=*/0, /*MonitorLog=*/nullptr);
    TArray<TSharedPtr<FJsonObject>> Payloads;
    Reg.OnJobEvent().AddLambda([&Payloads](const FString&, const FString& Event,
        const TSharedPtr<FJsonObject>& Payload, const TSharedPtr<FJsonObject>&)
        {
            if (Event == TEXT("progress"))
            {
                Payloads.Add(Payload);
            }
        });

    const FString Id = Reg.Start(TEXT("test.backwards"), MakeShared<FJsonObject>());

    TSharedPtr<FJsonObject> First = MakeShared<FJsonObject>();
    First->SetNumberField(TEXT("progress"), 10);
    Reg.RecordProgress(Id, TEXT("ten"), First, /*bBypassRateLimit=*/true);

    // An inner loop restarting its own counter — the realistic way this happens.
    TSharedPtr<FJsonObject> Second = MakeShared<FJsonObject>();
    Second->SetNumberField(TEXT("progress"), 5);
    Reg.RecordProgress(Id, TEXT("five"), Second, /*bBypassRateLimit=*/true);

    if (!TestEqual(TEXT("both events broadcast"), Payloads.Num(), 2))
    {
        return true;
    }
    double Emitted = 0.0;
    TestTrue(TEXT("second frame carries a progress number"),
        Payloads[1]->TryGetNumberField(TEXT("progress"), Emitted));
    // Held at 10 — NOT advanced to 11. Inventing movement for work that did not happen is
    // the defect this whole codebase has been removing; a repeated value is the truth.
    TestEqual(TEXT("the wire value is held at the previous one, not invented past it"),
        Emitted, 10.0);

    // The disclosure half: the record must not quietly replace what was reported.
    double Reported = 0.0;
    TestTrue(TEXT("the caller's raw value is preserved for the audit line"),
        Payloads[1]->TryGetNumberField(TEXT("reportedProgress"), Reported));
    TestEqual(TEXT("preserved value is what the caller passed"), Reported, 5.0);
    bool bFlagged = false;
    TestTrue(TEXT("the hold is flagged, not silent"),
        Payloads[1]->TryGetBoolField(TEXT("progressHeldAtPrevious"), bFlagged) && bFlagged);

    // A stalled job repeating its numerator is NOT a hold — the value did not go
    // backwards, so nothing is flagged and nothing is rewritten. This is the case that
    // made holding the right rule: asset.dump_folder sits flat while shaders compile.
    TSharedPtr<FJsonObject> Flat = MakeShared<FJsonObject>();
    Flat->SetNumberField(TEXT("progress"), 10);
    Reg.RecordProgress(Id, TEXT("still ten, waiting on compilation"), Flat,
        /*bBypassRateLimit=*/true);
    bool bFlatFlagged = false;
    TestFalse(TEXT("an honestly flat value is not flagged as held"),
        Payloads[2]->TryGetBoolField(TEXT("progressHeldAtPrevious"), bFlatFlagged));
    double FlatEmitted = 0.0;
    TestTrue(TEXT("flat frame carries a progress number"),
        Payloads[2]->TryGetNumberField(TEXT("progress"), FlatEmitted));
    TestEqual(TEXT("the flat value is published unchanged"), FlatEmitted, 10.0);

    // And an honest increase is untouched — otherwise the flag means nothing.
    TSharedPtr<FJsonObject> Third = MakeShared<FJsonObject>();
    Third->SetNumberField(TEXT("progress"), 900);
    Reg.RecordProgress(Id, TEXT("nine hundred"), Third, /*bBypassRateLimit=*/true);
    bool bUnused = false;
    TestFalse(TEXT("an honestly increasing value carries no hold flag"),
        Payloads[3]->TryGetBoolField(TEXT("progressHeldAtPrevious"), bUnused));
    double Advanced = 0.0;
    TestTrue(TEXT("advanced frame carries a progress number"),
        Payloads[3]->TryGetNumberField(TEXT("progress"), Advanced));
    TestEqual(TEXT("the advanced value is published unchanged"), Advanced, 900.0);

    Reg.Complete(Id, true, MakeShared<FJsonObject>(), FString());
    return true;
}
