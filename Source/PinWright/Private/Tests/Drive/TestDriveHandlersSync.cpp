// Copyright (c) 2026 Alexander Penkin. MIT License.

// Registration + dispatch tests for the synchronous drive.* RPC handlers
// (drive.observe / drive.expect / drive.events_since). These assert the methods are
// registered and dispatchable and that param validation / no-PIE paths return clean
// responses instead of crashing. They invoke the handler bodies directly via the
// shared TestUtils helpers, so they never require a live PIE HUD.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/ErrorCodes.h"
#include "Tests/TestUtils.h"

// ============================================================================
// Test: all three synchronous drive handlers are registered
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveHandlersRegisteredTest,
    "PinWright.drive.handlers.MethodsRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveHandlersRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("drive.observe is registered"), IsHandlerRegistered(TEXT("drive.observe")));
    TestTrue(TEXT("drive.expect is registered"), IsHandlerRegistered(TEXT("drive.expect")));
    TestTrue(TEXT("drive.events_since is registered"), IsHandlerRegistered(TEXT("drive.events_since")));
    return true;
}

// ============================================================================
// Test: drive.observe with no PIE returns a clean surface/UI error, not a crash
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveObserveNoPieCleanErrorTest,
    "PinWright.drive.handlers.ObserveNoPieCleanError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveObserveNoPieCleanErrorTest::RunTest(const FString& Parameters)
{
    // The live resolver reports "no capturable UI" as an error in the automation
    // editor (no PIE viewport); suppress the expected ambient log noise.
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.observe"), MakeShared<FJsonObject>(), Capture);

    TestTrue(TEXT("drive.observe handler found"), bFound);
    TestTrue(TEXT("drive.observe sent a response"), Capture.bWasCalled);
    // Either a viewport happened to be capturable (success) or a clean error was sent
    // with a non-empty error code — never a crash and never a fake success-with-no-code.
    TestTrue(TEXT("drive.observe is success or a clean coded error"),
        Capture.bSuccess || !Capture.ErrorCode.IsEmpty());
    return true;
}

// ============================================================================
// Test: drive.expect with a missing condition returns CONDITION_INVALID
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveExpectMissingConditionTest,
    "PinWright.drive.handlers.ExpectMissingConditionInvalid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveExpectMissingConditionTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    // Empty payload omits the required `condition` object.
    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.expect"), MakeShared<FJsonObject>(), Capture);

    TestTrue(TEXT("drive.expect handler found"), bFound);
    TestFalse(TEXT("drive.expect rejects a missing condition"), Capture.bSuccess);
    TestEqual(TEXT("error code is CONDITION_INVALID"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_CONDITION_INVALID));
    return true;
}

// ============================================================================
// Test: drive.expect with an unrecognized condition type returns CONDITION_INVALID
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveExpectInvalidConditionTypeTest,
    "PinWright.drive.handlers.ExpectInvalidConditionType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveExpectInvalidConditionTypeTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    // A condition object whose `type` token is unrecognized must fail to parse.
    TSharedPtr<FJsonObject> Condition = MakeShared<FJsonObject>();
    Condition->SetStringField(TEXT("type"), TEXT("not_a_real_condition_type"));
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("condition"), Condition);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.expect"), Payload, Capture);

    TestTrue(TEXT("drive.expect handler found"), bFound);
    TestFalse(TEXT("drive.expect rejects an invalid condition type"), Capture.bSuccess);
    TestEqual(TEXT("error code is CONDITION_INVALID"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_CONDITION_INVALID));
    return true;
}

// ============================================================================
// Test: drive.events_since with no live tail returns an empty delta, not an error
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveEventsSinceEmptyTest,
    "PinWright.drive.handlers.EventsSinceEmptyNoPie",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveEventsSinceEmptyTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.events_since"), MakeShared<FJsonObject>(), Capture);

    TestTrue(TEXT("drive.events_since handler found"), bFound);
    TestTrue(TEXT("drive.events_since succeeds with no PIE"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        double Cursor = -1.0;
        TestTrue(TEXT("response carries a cursor"), Capture.Result->TryGetNumberField(TEXT("cursor"), Cursor));
        TestEqual(TEXT("empty-delta cursor is 0"), Cursor, 0.0);

        const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
        TestTrue(TEXT("response carries an events array"), Capture.Result->TryGetArrayField(TEXT("events"), Events));
        if (Events)
        {
            TestEqual(TEXT("events array is empty"), Events->Num(), 0);
        }

        const TArray<TSharedPtr<FJsonValue>>* ChangedVariables = nullptr;
        TestTrue(TEXT("response carries a changed_variables array"),
            Capture.Result->TryGetArrayField(TEXT("changed_variables"), ChangedVariables));
        if (ChangedVariables)
        {
            TestEqual(TEXT("changed_variables array is empty"), ChangedVariables->Num(), 0);
        }
    }

    return true;
}
