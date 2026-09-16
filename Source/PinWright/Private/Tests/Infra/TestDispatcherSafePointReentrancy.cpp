// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behaviour coverage for safe-point work retained by the active dispatcher request.
// A deferred body still belongs to its outer RPC, so queued requests cannot interleave.

#include "Misc/AutomationTest.h"

#include "Containers/Ticker.h"
#include "Dispatch/RpcDispatcher.h"
#include "Dispatch/SafePoint.h"
#include "Dom/JsonObject.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Misc/ScopeExit.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherSafePointReentrancyTest,
    "PinWright.infra.dispatcher.SafePointContinuationRetainsRequestScope",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherSafePointReentrancyTest::RunTest(const FString& /*Parameters*/)
{
    FRpcDispatcher Dispatcher;
    TArray<FString> Events;
    bool bDeferredBodyWasSafe = false;
    bool bGuardWasActiveInDeferredBody = false;
    bool bResponderReportedDeferred = false;
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();

    Dispatcher.RegisterHandler(TEXT("test.queued"),
        [&Events](const FString&, const FString&, const TSharedPtr<FJsonObject>&) -> bool
        {
            Events.Add(TEXT("queued"));
            return true;
        });
    Dispatcher.RegisterHandler(TEXT("test.interleaved"),
        [&Events](const FString&, const FString&, const TSharedPtr<FJsonObject>&) -> bool
        {
            Events.Add(TEXT("interleaved"));
            return true;
        });
    Dispatcher.RegisterHandler(TEXT("test.inner"),
        [&Dispatcher, &Events, &bDeferredBodyWasSafe, &bGuardWasActiveInDeferredBody,
         &bResponderReportedDeferred, Capture]
        (const FString& RequestId, const FString& Action,
         const TSharedPtr<FJsonObject>& Payload) -> bool
        {
            Events.Add(TEXT("inner.schedule"));
            FHandlerContext Ctx = FHandlerContext::MakeTestContextWithSharedCapture(
                RequestId, Action, Payload, Capture);
            Ctx.SetDispatcherForTesting(&Dispatcher);
            return PinWrightSafePoint::RunAtSafePoint(Ctx, TEXT("dispatcher reentrancy test"),
                [&Dispatcher, &Events, &bDeferredBodyWasSafe,
                 &bGuardWasActiveInDeferredBody, &bResponderReportedDeferred]
                (const PinWrightSafePoint::FSafePointResponder& Responder)
                {
                    Events.Add(TEXT("deferred.begin"));
                    bDeferredBodyWasSafe = PinWrightSafePoint::IsSafeNow();
                    bGuardWasActiveInDeferredBody = Dispatcher.IsProcessingRequestForTesting();
                    bResponderReportedDeferred = Responder.IsDeferred();
                    Dispatcher.ProcessRequest(TEXT("interleaved-id"), TEXT("test.interleaved"),
                        MakeShared<FJsonObject>());
                    Events.Add(TEXT("deferred.end"));
                    Responder.SendSuccess(MakeShared<FJsonObject>());
                });
        });
    Dispatcher.RegisterHandler(TEXT("test.outer"),
        [&Dispatcher, &Events]
        (const FString& RequestId, const FString&, const TSharedPtr<FJsonObject>& Payload) -> bool
        {
            Events.Add(TEXT("outer.begin"));
            const bool bHandled = Dispatcher.DispatchMethod(TEXT("test.inner"), RequestId, Payload);
            Events.Add(TEXT("outer.end"));
            return bHandled;
        });

    PinWrightSafePoint::SetForcedUnsafeForTests(true);
    ON_SCOPE_EXIT { PinWrightSafePoint::SetForcedUnsafeForTests(false); };
    Dispatcher.ProcessRequest(TEXT("outer-id"), TEXT("test.outer"), MakeShared<FJsonObject>());
    PinWrightSafePoint::SetForcedUnsafeForTests(false);

    TestTrue(TEXT("outer request scope remains active while its continuation is pending"),
        Dispatcher.IsProcessingRequestForTesting());
    Dispatcher.ProcessRequest(TEXT("queued-id"), TEXT("test.queued"), MakeShared<FJsonObject>());
    Dispatcher.ProcessPendingRequests();
    TestEqual(TEXT("queued request cannot run while the continuation is pending"),
        Events.Num(), 3);

    FTSTicker::GetCoreTicker().Tick(0.0f);

    TestTrue(TEXT("deferred body runs at a safe point"), bDeferredBodyWasSafe);
    TestTrue(TEXT("dispatcher guard remains active inside the deferred body"),
        bGuardWasActiveInDeferredBody);
    TestTrue(TEXT("RunAtSafePoint reports the deferred response path"),
        bResponderReportedDeferred);
    TestTrue(TEXT("deferred response reached its shared capture"), Capture->bWasCalled);
    TestFalse(TEXT("request scope closes after the deferred body"),
        Dispatcher.IsProcessingRequestForTesting());
    TestEqual(TEXT("deferred body finishes before either queued request"),
        FString::Join(Events, TEXT(",")),
        FString(TEXT("outer.begin,inner.schedule,outer.end,deferred.begin,deferred.end,queued,interleaved")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherJobSafePointContinuationRetainsRequestScopeTest,
    "PinWright.infra.dispatcher.JobSafePointContinuationRetainsRequestScope",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherJobSafePointContinuationRetainsRequestScopeTest::RunTest(
    const FString& /*Parameters*/)
{
    FRpcDispatcher Dispatcher;
    TArray<FString> Events;
    bool bJobBodyRan = false;
    bool bHelperWasCalledFromSafeStack = false;
    bool bDeferredBodyWasSafe = false;
    bool bGuardWasActiveInDeferredBody = false;
    bool bRequestInterleavedInsideBody = false;
    int32 OnCompleteCallCount = 0;
    TSharedRef<FString> JobTicketHolder = MakeShared<FString>();
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();

    Dispatcher.RegisterHandler(TEXT("test.queued"),
        [&Events](const FString&, const FString&, const TSharedPtr<FJsonObject>&) -> bool
        {
            Events.Add(TEXT("queued"));
            return true;
        });
    Dispatcher.RegisterHandler(TEXT("test.interleaved"),
        [&Events](const FString&, const FString&, const TSharedPtr<FJsonObject>&) -> bool
        {
            Events.Add(TEXT("interleaved"));
            return true;
        });
    Dispatcher.RegisterHandler(TEXT("test.job"),
        [&Dispatcher, &Events, &bJobBodyRan, &bHelperWasCalledFromSafeStack,
         &bDeferredBodyWasSafe,
         &bGuardWasActiveInDeferredBody, &bRequestInterleavedInsideBody,
         &OnCompleteCallCount, JobTicketHolder, Capture]
        (const FString& RequestId, const FString& Action,
         const TSharedPtr<FJsonObject>& Payload) -> bool
        {
            Events.Add(TEXT("job.schedule"));
            FHandlerContext Ctx = FHandlerContext::MakeTestContextWithSharedCapture(
                RequestId, Action, Payload, Capture);
            Ctx.SetDispatcherForTesting(&Dispatcher);

            FJobBindArgs Args;
            Args.Method = TEXT("test.job");
            Args.BindNativeDelegate =
                [Ctx, &Dispatcher, &Events, &bJobBodyRan,
                 &bHelperWasCalledFromSafeStack, &bDeferredBodyWasSafe,
                 &bGuardWasActiveInDeferredBody, &bRequestInterleavedInsideBody,
                 &OnCompleteCallCount](FJobOnComplete OnComplete)
            {
                Events.Add(TEXT("job.bound"));
                bHelperWasCalledFromSafeStack = PinWrightSafePoint::IsSafeNow();
                PinWrightSafePoint::DeferJobToSafePoint(
                    Ctx, TEXT("dispatcher job continuation test"),
                    [&Dispatcher, &Events, &bJobBodyRan, &bDeferredBodyWasSafe,
                     &bGuardWasActiveInDeferredBody, &bRequestInterleavedInsideBody,
                     &OnCompleteCallCount, OnComplete]() mutable
                    {
                        Events.Add(TEXT("deferred.begin"));
                        bJobBodyRan = true;
                        bDeferredBodyWasSafe = PinWrightSafePoint::IsSafeNow();
                        bGuardWasActiveInDeferredBody =
                            Dispatcher.IsProcessingRequestForTesting();
                        Dispatcher.ProcessRequest(
                            TEXT("interleaved-id"), TEXT("test.interleaved"),
                            MakeShared<FJsonObject>());
                        bRequestInterleavedInsideBody = Events.Contains(TEXT("interleaved"));
                        Events.Add(TEXT("deferred.end"));
                        ++OnCompleteCallCount;
                        OnComplete(true, MakeShared<FJsonObject>(), FString());
                    });
            };

            *JobTicketHolder = Ctx.StartJob(Args);
            return true;
        });

    Dispatcher.ProcessRequest(TEXT("job-id"), TEXT("test.job"), MakeShared<FJsonObject>());

    TestTrue(TEXT("job helper is invoked from an initially safe stack"),
        bHelperWasCalledFromSafeStack);
    FString ResponseStatus;
    FString ResponseTicketId;
    const bool bHasRunningResponse = Capture->bWasCalled && Capture->bSuccess
        && Capture->Result.IsValid()
        && Capture->Result->TryGetStringField(TEXT("status"), ResponseStatus)
        && Capture->Result->TryGetStringField(TEXT("ticket_id"), ResponseTicketId);
    TestTrue(TEXT("StartJob sends an immediate successful ticket response"),
        bHasRunningResponse);
    TestEqual(TEXT("the immediate ticket is running"), ResponseStatus,
        FString(TEXT("running")));
    TestTrue(TEXT("the immediate ticket id is nonempty"), !ResponseTicketId.IsEmpty());
    TestEqual(TEXT("StartJob returns the same ticket id it sends"), *JobTicketHolder,
        ResponseTicketId);
    TestFalse(TEXT("job body does not run before the handler returns"), bJobBodyRan);
    TestTrue(TEXT("request scope remains active while the job continuation is pending"),
        Dispatcher.IsProcessingRequestForTesting());

    Dispatcher.ProcessRequest(TEXT("queued-id"), TEXT("test.queued"),
        MakeShared<FJsonObject>());
    Dispatcher.ProcessPendingRequests();
    TestEqual(TEXT("an explicit queue drain cannot enter while the continuation is pending"),
        FString::Join(Events, TEXT(",")), FString(TEXT("job.schedule,job.bound")));

    FTSTicker::GetCoreTicker().Tick(0.0f);

    TestTrue(TEXT("deferred job body runs at a safe point"), bDeferredBodyWasSafe);
    TestTrue(TEXT("dispatcher guard remains active inside the deferred job body"),
        bGuardWasActiveInDeferredBody);
    TestFalse(TEXT("a request submitted inside the job body does not interleave"),
        bRequestInterleavedInsideBody);
    TestEqual(TEXT("job completion callback runs exactly once"), OnCompleteCallCount, 1);
    TestFalse(TEXT("request scope closes after the deferred job body"),
        Dispatcher.IsProcessingRequestForTesting());
    TestEqual(TEXT("deferred job finishes before either queued request"),
        FString::Join(Events, TEXT(",")),
        FString(TEXT("job.schedule,job.bound,deferred.begin,deferred.end,queued,interleaved")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherJobSafePointAbandonsWhenDispatcherEndsTest,
    "PinWright.infra.dispatcher.JobSafePointAbandonsWhenDispatcherEnds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherJobSafePointAbandonsWhenDispatcherEndsTest::RunTest(
    const FString& /*Parameters*/)
{
    bool bSaveWorkRan = false;
    int32 AbandonmentCallCount = 0;
    int32 OnCompleteCallCount = 0;
    TSharedRef<FString> JobTicketHolder = MakeShared<FString>();
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    TUniquePtr<FRpcDispatcher> Dispatcher = MakeUnique<FRpcDispatcher>();

    Dispatcher->RegisterHandler(TEXT("test.save_then_destroy"),
        [&Dispatcher, &bSaveWorkRan, &AbandonmentCallCount, &OnCompleteCallCount,
         JobTicketHolder, Capture]
        (const FString& RequestId, const FString& Action,
         const TSharedPtr<FJsonObject>& Payload) -> bool
        {
            FHandlerContext Ctx = FHandlerContext::MakeTestContextWithSharedCapture(
                RequestId, Action, Payload, Capture);
            Ctx.SetDispatcherForTesting(Dispatcher.Get());

            FJobBindArgs Args;
            Args.Method = TEXT("test.save_then_destroy");
            Args.BindNativeDelegate =
                [Ctx, &bSaveWorkRan, &AbandonmentCallCount,
                 &OnCompleteCallCount](FJobOnComplete OnComplete)
            {
                PinWrightSafePoint::DeferJobToSafePoint(
                    Ctx, TEXT("destroyed dispatcher job continuation test"),
                    [&bSaveWorkRan, &OnCompleteCallCount, OnComplete]() mutable
                    {
                        bSaveWorkRan = true;
                        ++OnCompleteCallCount;
                        OnComplete(true, MakeShared<FJsonObject>(), FString());
                    },
                    [&AbandonmentCallCount, &OnCompleteCallCount,
                     OnComplete]() mutable
                    {
                        ++AbandonmentCallCount;
                        ++OnCompleteCallCount;
                        OnComplete(false, nullptr, FString::Printf(
                            TEXT("%s: Dispatcher/request context ended before the "
                                 "deferred save began."),
                            ErrorCodes::ERR_SAVE_FAILED));
                    });
            };

            *JobTicketHolder = Ctx.StartJob(Args);
            return true;
        });

    Dispatcher->ProcessRequest(
        TEXT("destroy-job-id"), TEXT("test.save_then_destroy"),
        MakeShared<FJsonObject>());
    TestTrue(TEXT("StartJob creates a nonempty ticket before dispatcher teardown"),
        !JobTicketHolder->IsEmpty());
    TestFalse(TEXT("save work remains deferred before dispatcher teardown"),
        bSaveWorkRan);
    TestTrue(TEXT("the request guard is retained before dispatcher teardown"),
        Dispatcher->IsProcessingRequestForTesting());

    Dispatcher.Reset();
    FTSTicker::GetCoreTicker().Tick(0.0f);

    TestFalse(TEXT("destroying the dispatcher prevents deferred save work"),
        bSaveWorkRan);
    TestEqual(TEXT("the abandonment callback runs exactly once"),
        AbandonmentCallCount, 1);
    TestEqual(TEXT("the job completion callback runs exactly once"),
        OnCompleteCallCount, 1);

    FJobTicket Ticket;
    const bool bFoundTicket = FPluginState::Get().GetJobRegistry().Get(
        *JobTicketHolder, Ticket);
    TestTrue(TEXT("the abandoned job ticket remains queryable"), bFoundTicket);
    if (bFoundTicket)
    {
        TestEqual(TEXT("the abandoned job reaches terminal failure"),
            Ticket.Status, FString(TEXT("failed")));
        TestTrue(TEXT("the terminal error uses the registered save failure code"),
            Ticket.Error.StartsWith(ErrorCodes::ERR_SAVE_FAILED));
        TestTrue(TEXT("the terminal error explains the dispatcher/request teardown"),
            Ticket.Error.Contains(TEXT("Dispatcher/request context ended")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherSafePointDestroyedOwnerTest,
    "PinWright.infra.dispatcher.DestroyedOwnerDropsSafePointContinuation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherSafePointDestroyedOwnerTest::RunTest(const FString& /*Parameters*/)
{
    bool bDeferredWorkRan = false;
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    TUniquePtr<FRpcDispatcher> Dispatcher = MakeUnique<FRpcDispatcher>();

    Dispatcher->RegisterHandler(TEXT("test.defer_then_destroy"),
        [&Dispatcher, &bDeferredWorkRan, Capture]
        (const FString& RequestId, const FString& Action,
         const TSharedPtr<FJsonObject>& Payload) -> bool
        {
            FHandlerContext Ctx = FHandlerContext::MakeTestContextWithSharedCapture(
                RequestId, Action, Payload, Capture);
            Ctx.SetDispatcherForTesting(Dispatcher.Get());
            return PinWrightSafePoint::RunAtSafePoint(Ctx, TEXT("dispatcher lifetime test"),
                [&bDeferredWorkRan](const PinWrightSafePoint::FSafePointResponder& Responder)
                {
                    bDeferredWorkRan = true;
                    Responder.SendSuccess(MakeShared<FJsonObject>());
                });
        });

    PinWrightSafePoint::SetForcedUnsafeForTests(true);
    ON_SCOPE_EXIT { PinWrightSafePoint::SetForcedUnsafeForTests(false); };
    Dispatcher->ProcessRequest(
        TEXT("destroy-id"), TEXT("test.defer_then_destroy"), MakeShared<FJsonObject>());
    PinWrightSafePoint::SetForcedUnsafeForTests(false);
    Dispatcher.Reset();

    FTSTicker::GetCoreTicker().Tick(0.0f);
    TestFalse(TEXT("a continuation cannot touch work owned by a destroyed dispatcher"),
        bDeferredWorkRan);
    TestFalse(TEXT("destroyed dispatcher emits no late response"), Capture->bWasCalled);
    return true;
}
