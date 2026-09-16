// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-no-response-handler-guardrail: an auto-registered
// handler that returns true without calling Ctx.SendSuccess / Ctx.SendError
// must surface a NO_HANDLER_RESPONSE error and diagnostic instead of leaving
// the transport waiting for a completion that never arrives.
#include "Misc/AutomationTest.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Utils/LogUtils.h"
#include "Dom/JsonObject.h"

// Auto-registered handler that deliberately returns true without sending any
// response. Drained by every DrainAutoRegistrations(nullptr) call; the unique
// method name keeps it isolated from the other _test.* handlers.
REGISTER_RPC_HANDLER("_test.no_response_guard", "_test",
    "Handler that returns true without responding (guardrail fixture)",
    RPC_NO_PARAMS)
{
    return true;
}

// ============================================================================
// Test: auto-registered handler returning true without a response →
//       NO_HANDLER_RESPONSE + error log, without an ensure crash report
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherAutoRegNoResponseGuardTest,
    "PinWright.core.dispatcher.AutoRegNoResponseGuard",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherAutoRegNoResponseGuardTest::RunTest(const FString& Parameters)
{
    // The guardrail deliberately emits one error-level diagnostic. Declare that
    // log so automation accepts it, while the capture below verifies its content.
    AddExpectedError(TEXT("_test.no_response_guard"),
        EAutomationExpectedErrorFlags::Contains, 1);

    FMcpOutputCapture LogCapture;
    GLog->AddOutputDevice(&LogCapture);

    // Initialize BEFORE draining so the bridge lambda captures a live sink.
    bool bCompletionFired = false;
    bool bCapturedSuccess = true;
    FString CapturedErrorCode;
    FString CapturedMessage;
    FRpcDispatcher Dispatcher;
    Dispatcher.Initialize(FResponseSink(
        [&bCompletionFired, &bCapturedSuccess, &CapturedErrorCode, &CapturedMessage]
        (const FString&, bool bSuccess, const FString& Message,
         const TSharedPtr<FJsonObject>&, const FString& ErrorCode)
        {
            bCompletionFired = true;
            bCapturedSuccess = bSuccess;
            CapturedErrorCode = ErrorCode;
            CapturedMessage = Message;
        }));
    Dispatcher.DrainAutoRegistrations(nullptr);

    Dispatcher.DispatchMethod(
        TEXT("_test.no_response_guard"), TEXT("req-no-response"), MakeShared<FJsonObject>());

    GLog->Flush();
    GLog->RemoveOutputDevice(&LogCapture);

    TestTrue(TEXT("Completion fired"), bCompletionFired);
    TestFalse(TEXT("Completion reports failure"), bCapturedSuccess);
    TestEqual(TEXT("Error code is NO_HANDLER_RESPONSE"),
        CapturedErrorCode, TEXT("NO_HANDLER_RESPONSE"));
    TestTrue(TEXT("Message names the method"),
        CapturedMessage.Contains(TEXT("_test.no_response_guard")));

    bool bErrorLogged = false;
    for (const FString& Line : LogCapture.Lines)
    {
        if (Line.Contains(TEXT("_test.no_response_guard")) &&
            Line.Contains(TEXT("without sending a response")))
        {
            bErrorLogged = true;
            break;
        }
    }
    TestTrue(TEXT("Missing-response error was logged"), bErrorLogged);

    return true;
}
