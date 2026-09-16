// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the RPC_NO_PARAMS unknown-arg gate in
// FRpcDispatcher::ValidateHandlerParams.
//
// Before the fix, the unknown-parameter rejection block was gated on
// Reg.Params.Num() > 0, so a handler declared with RPC_NO_PARAMS skipped the check
// entirely and silently accepted any caller-supplied field. Removing that gate makes
// empty-spec handlers reject undeclared args the same way declared-param handlers
// already do, while still accepting an empty args:{} and the reserved transport
// fields (wait / _format / the spill skip flag) that StripInternalDispatchFields peels
// off inside ProcessRequest BEFORE validation runs.
//
// _test.beta (Tests/Infra/TestAutoRegistration.cpp) is the shared RPC_NO_PARAMS
// fixture; MakeDispatcher / Dispatch (Tests/Infra/DispatcherTestHelpers.h) route
// through FRpcDispatcher::ProcessRequest so the production validation + reserved-field
// strip path both run.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// ============================================================================
// Test: an RPC_NO_PARAMS handler now rejects an UNDECLARED arg with UNKNOWN_PARAMS
//       (previously it silently passed).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoParamHandlerRejectsUnknownArgTest,
    "PinWright.core.dispatcher.NoParamHandlerRejectsUnknownArg",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNoParamHandlerRejectsUnknownArgTest::RunTest(const FString& Parameters)
{
    // The unknown-param rejection logs a Warning; suppress it so the expected
    // rejection path doesn't trip the automation warning gate.
    bSuppressLogWarnings = true;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // _test.beta declares RPC_NO_PARAMS; an undeclared field must now be rejected.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("bogus"), TEXT("value"));

    bool bSuccess = true;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("_test.beta"),
        TEXT("req-noparam-unknown"), Params, bSuccess, ErrorCode);

    TestTrue(TEXT("Completion fired"), Sink->bWasCalled);
    TestFalse(TEXT("Undeclared arg is rejected (not a success)"), bSuccess);
    TestEqual(TEXT("Error code is UNKNOWN_PARAMS"), ErrorCode, TEXT("UNKNOWN_PARAMS"));
    TestTrue(TEXT("Message names the offending field"),
        Sink->Message.Contains(TEXT("bogus")));
    TestTrue(TEXT("Empty-spec message reads 'takes no parameters'"),
        Sink->Message.Contains(TEXT("takes no parameters")));
    TestFalse(TEXT("Empty-spec message omits the 'Valid parameters' list phrasing"),
        Sink->Message.Contains(TEXT("Valid parameters")));

    return true;
}

// ============================================================================
// Test: the same RPC_NO_PARAMS handler still SUCCEEDS with an empty args:{}
//       (no fields to reject).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoParamHandlerAcceptsEmptyArgsTest,
    "PinWright.core.dispatcher.NoParamHandlerAcceptsEmptyArgs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNoParamHandlerAcceptsEmptyArgsTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // An empty args:{} has no fields to reject and must reach the handler body.
    TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("_test.beta"),
        TEXT("req-noparam-empty"), EmptyParams, bSuccess, ErrorCode);

    TestTrue(TEXT("Completion fired"), Sink->bWasCalled);
    TestTrue(TEXT("Empty args succeeds"), bSuccess);
    TestEqual(TEXT("No error code on success"), ErrorCode, FString());

    return true;
}

// ============================================================================
// Test (regression lock from the audit): a reserved transport field (wait:false)
//       sent to a parameterless handler still SUCCEEDS — reserved fields are not
//       rejected as unknown.
//
// The strip happens inside FRpcDispatcher::ProcessRequest via
// StripInternalDispatchFields, ABOVE ValidateHandlerParams. Dispatch() routes through
// ProcessRequest, so this exercises the strip at the dispatcher-test level: wait is
// removed before validation and never counts as an unknown param.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoParamHandlerToleratesReservedFieldTest,
    "PinWright.core.dispatcher.NoParamHandlerToleratesReservedField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNoParamHandlerToleratesReservedFieldTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> WaitParams = MakeShared<FJsonObject>();
    WaitParams->SetBoolField(TEXT("wait"), false);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("_test.beta"),
        TEXT("req-noparam-wait"), WaitParams, bSuccess, ErrorCode);

    TestTrue(TEXT("Completion fired"), Sink->bWasCalled);
    TestTrue(TEXT("Reserved wait:false is stripped, not rejected"), bSuccess);
    TestEqual(TEXT("No error code for reserved-field-only args"), ErrorCode, FString());

    return true;
}
