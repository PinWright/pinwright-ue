// Copyright (c) 2026 Alexander Penkin. MIT License.

// FJobBindArgs::bCompletesInBind: a job whose bind delegate does its work inline must answer the
// caller AFTER that work, with its real outcome. The default StartJob reply is a "running"
// envelope sent BEFORE the delegate, which leaves the transport while the work is still on the
// game thread - editor.screenshot's caller read `requested_path` and found no file
// (B-editor-screenshot-returns-before-png-exists). Both tests fail if StartJob goes back to
// replying before the delegate: the captured reply would be the "running" envelope.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"

namespace
{
    // Runs a bCompletesInBind job whose delegate completes with the given outcome. bReplySentFirst
    // reports whether the reply had already been captured when the delegate ran.
    void RunInlineJob(bool bSuccess, const TSharedPtr<FJsonObject>& Result, const FString& Error,
        FResponseCapture& Capture, FString& OutTicketId, bool& bReplySentFirst)
    {
        const FHandlerContext Ctx = FHandlerContext::MakeTestContextWithCapture(
            TEXT("start-job-inline"), TEXT("test.inline_job"), MakeShared<FJsonObject>(), &Capture);
        FJobBindArgs Args;
        Args.Method = TEXT("test.inline_job");
        Args.StartedPayload = MakeShared<FJsonObject>();
        Args.StartedPayload->SetStringField(TEXT("requested_path"), TEXT("started-payload-value"));
        Args.bCompletesInBind = true;
        FResponseCapture* CapturePtr = &Capture;
        Args.BindNativeDelegate = [bSuccess, Result, Error, CapturePtr, &bReplySentFirst](FJobOnComplete OnComplete)
        {
            bReplySentFirst = CapturePtr->bWasCalled;
            OnComplete(bSuccess, Result, Error);
        };
        OutTicketId = Ctx.StartJob(Args);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStartJobCompletesInBindSuccessTest,
    "PinWright.infra.start_job.CompletesInBindRepliesWithResult",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStartJobCompletesInBindSuccessTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), TEXT("written/file.png"));

    FResponseCapture Capture;
    FString TicketId;
    bool bReplySentFirst = true;
    RunInlineJob(true, Result, FString(), Capture, TicketId, bReplySentFirst);

    TestFalse(TEXT("no reply leaves before the inline work ran"), bReplySentFirst);
    TestEqual(TEXT("exactly one reply"), Capture.CallCount, 1);
    TestTrue(TEXT("the reply is a success"), Capture.bSuccess);
    if (!TestTrue(TEXT("the reply carries a payload"), Capture.Result.IsValid()))
    {
        return false;
    }
    TestEqual(TEXT("the reply is terminal"), Capture.Result->GetStringField(TEXT("status")),
        FString(TEXT("completed")));
    TestEqual(TEXT("the reply carries the job's result"), Capture.Result->GetStringField(TEXT("path")),
        FString(TEXT("written/file.png")));
    TestEqual(TEXT("the reply keeps the started payload"),
        Capture.Result->GetStringField(TEXT("requested_path")), FString(TEXT("started-payload-value")));
    TestEqual(TEXT("the reply names the ticket"), Capture.Result->GetStringField(TEXT("ticket_id")), TicketId);

    FJobTicket Ticket;
    TestTrue(TEXT("the ticket stays readable"), FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket));
    TestEqual(TEXT("the ticket agrees with the reply"), Ticket.Status, FString(TEXT("completed")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStartJobCompletesInBindFailureTest,
    "PinWright.infra.start_job.CompletesInBindFailureIsAnError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStartJobCompletesInBindFailureTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(TEXT("message"), TEXT("shaders still compiling"));

    FResponseCapture Capture;
    FString TicketId;
    bool bReplySentFirst = true;
    RunInlineJob(false, Details, ErrorCodes::ERR_CAPTURE_NOT_READY, Capture, TicketId, bReplySentFirst);

    TestFalse(TEXT("no reply leaves before the inline work ran"), bReplySentFirst);
    TestEqual(TEXT("exactly one reply"), Capture.CallCount, 1);
    TestFalse(TEXT("a failed inline job is an error reply, not a success envelope"), Capture.bSuccess);
    TestEqual(TEXT("the error reply carries the job's error code"), Capture.ErrorCode,
        FString(ErrorCodes::ERR_CAPTURE_NOT_READY));
    TestEqual(TEXT("the error reply carries the job's message"), Capture.Message,
        FString(TEXT("shaders still compiling")));
    if (TestTrue(TEXT("the error reply carries data"), Capture.Result.IsValid()))
    {
        TestEqual(TEXT("the error data names the ticket"), Capture.Result->GetStringField(TEXT("ticket_id")), TicketId);
        TestEqual(TEXT("the error data is terminal"), Capture.Result->GetStringField(TEXT("status")),
            FString(TEXT("failed")));
    }
    return true;
}
