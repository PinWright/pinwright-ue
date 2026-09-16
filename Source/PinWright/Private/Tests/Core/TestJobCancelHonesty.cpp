// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract tests for system.job_cancel — the same defect class as the sibling Core TU
// TestAssetSaveHonesty.cpp: a verb whose report did not match what happened.
//
// system.job_cancel returned {cancelled:true} for any ticket that happened to be in "running",
// regardless of whether anything could stop the work. Only 4 of the 19 ticketed verbs register a
// cancel hook (asset.dump, asset.dump_folder, localization.gather, localization.compile, plus
// pcg.generate on 5.4+); the other fifteen — level.build_lighting, level.build_all,
// level.build_navigation, level.save, level.save_as, lighting.build_lighting,
// navigation.rebuild_navigation, system.run_ubt, system.run_tests, editor.screenshot,
// blueprint.build_api_index, performance.run_benchmark, performance.optimize_shaders,
// render.nanite_rebuild_mesh, mrq.run_jobs — have no mechanism at all.
//
// This is the worst possible place in the plugin for a false success, because cancelling is what
// an agent does IN RESPONSE TO A HANG. The verb answered "cancelled", the agent proceeded on the
// belief that the editor was idle, and one recorded run wrote ~19,700 files after the
// "successful" cancel.
//
// Making the fifteen genuinely cancellable is 200-500 lines each (chunking work loops that have
// no engine completion hook), so the fix is to report the truth, not to fake the capability.
// Every assertion below is written so it FAILS if the verb goes back to answering success.

#include "Misc/AutomationTest.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Handlers/ErrorCodes.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

namespace
{
    // Dispatch system.job_cancel for TicketId against the live plugin registry (the handler
    // reads FPluginState::Get().GetJobRegistry(), so the test must use the same instance).
    void DispatchJobCancel(const FString& TicketId, bool& bOutSuccess,
        TSharedPtr<FJsonObject>& OutResult, FString& OutErrorCode)
    {
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::FSinkPtr Sink;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("ticket_id"), TicketId);
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("system.job_cancel"),
            TEXT("job-cancel-honesty"), Params, bOutSuccess, OutResult, OutErrorCode);
    }
}

// THE regression. A ticket for a verb with no cancel hook must not come back as a success.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobCancelUnsupportedIsAnErrorTest,
    "PinWright.system.job_cancel.UncancellableVerbReportsUnsupported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FJobCancelUnsupportedIsAnErrorTest::RunTest(const FString& Parameters)
{
    FJobRegistry& Registry = FPluginState::Get().GetJobRegistry();
    // No SetCancelCallback — the shape of level.build_lighting and fourteen siblings.
    const FString TicketId =
        Registry.Start(TEXT("level.build_lighting"), MakeShared<FJsonObject>());

    bool bSuccess = true;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    DispatchJobCancel(TicketId, bSuccess, Result, ErrorCode);

    // An error, not a success carrying a flag. A caller that only checks isError — which is how
    // an agent reacting to a hang reads this — must not be told the work stopped.
    TestFalse(TEXT("an uncancellable job does NOT report success"), bSuccess);
    TestEqual(TEXT("it reports JOB_CANCEL_UNSUPPORTED"),
        ErrorCode, FString(ErrorCodes::ERR_JOB_CANCEL_UNSUPPORTED));

    // And the ticket must still say "running", because it is. If this flips, system.job_status
    // starts corroborating the false success the caller was just refused.
    FJobTicket Ticket;
    TestTrue(TEXT("ticket still present"), Registry.Get(TicketId, Ticket));
    TestEqual(TEXT("ticket still reports 'running'"),
        Ticket.Status, FString(TEXT("running")));

    // Leave no live ticket behind for the rest of the suite.
    Registry.Complete(TicketId, true, MakeShared<FJsonObject>(), FString());
    return true;
}

// The capability path still works, and says what it actually proved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobCancelSupportedReportsRequestedTest,
    "PinWright.system.job_cancel.CancellableVerbReportsRequested",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FJobCancelSupportedReportsRequestedTest::RunTest(const FString& Parameters)
{
    FJobRegistry& Registry = FPluginState::Get().GetJobRegistry();
    const FString TicketId = Registry.Start(TEXT("asset.dump_folder"), MakeShared<FJsonObject>());
    bool bHookFired = false;
    Registry.SetCancelCallback(TicketId, [&bHookFired]() { bHookFired = true; });

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    DispatchJobCancel(TicketId, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("a cancellable job succeeds"), bSuccess);
    TestTrue(TEXT("the verb's cancel hook actually ran"), bHookFired);
    if (TestTrue(TEXT("result present"), Result.IsValid()))
    {
        bool bCancelled = false;
        TestTrue(TEXT("`cancelled` present"), Result->TryGetBoolField(TEXT("cancelled"), bCancelled));
        TestTrue(TEXT("cancelled=true"), bCancelled);
        // "requested" and not "stopped": the hook has been invoked, how promptly the verb
        // unwinds is the verb's business, and the ticket stays the source of truth.
        FString Cancellation;
        TestTrue(TEXT("`cancellation` present"),
            Result->TryGetStringField(TEXT("cancellation"), Cancellation));
        TestEqual(TEXT("cancellation is 'requested'"), Cancellation, FString(TEXT("requested")));
    }
    return true;
}

// The two rejection paths a bare bool also collapsed into `false`. They are separate codes
// because their recoveries differ: NotRunning means job_status still holds the result,
// TICKET_NOT_FOUND means it never will.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobCancelRejectionCodesTest,
    "PinWright.system.job_cancel.RejectionsAreDistinguished",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FJobCancelRejectionCodesTest::RunTest(const FString& Parameters)
{
    bool bSuccess = true;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;

    DispatchJobCancel(TEXT("j_no_such_ticket_00000000"), bSuccess, Result, ErrorCode);
    TestFalse(TEXT("unknown ticket is not a success"), bSuccess);
    TestEqual(TEXT("unknown ticket reports TICKET_NOT_FOUND"),
        ErrorCode, FString(ErrorCodes::ERR_TICKET_NOT_FOUND));

    FJobRegistry& Registry = FPluginState::Get().GetJobRegistry();
    const FString TicketId = Registry.Start(TEXT("asset.dump"), MakeShared<FJsonObject>());
    Registry.Complete(TicketId, true, MakeShared<FJsonObject>(), FString());

    bSuccess = true;
    DispatchJobCancel(TicketId, bSuccess, Result, ErrorCode);
    TestFalse(TEXT("terminal ticket is not a success"), bSuccess);
    TestEqual(TEXT("terminal ticket reports JOB_NOT_RUNNING"),
        ErrorCode, FString(ErrorCodes::ERR_JOB_NOT_RUNNING));
    return true;
}
