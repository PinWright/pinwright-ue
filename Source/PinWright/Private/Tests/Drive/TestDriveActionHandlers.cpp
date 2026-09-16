// Copyright (c) 2026 Alexander Penkin. MIT License.

// Registration + param-validation tests for the asynchronous drive.* ACTION verbs
// (drive.click / hover / scroll / type / key / drag) and drive.wait_for. They assert
// the methods are registered, that param validation rejects missing required args,
// and that with no live PIE a targeted action / wait returns a clean coded error
// rather than crashing or hanging. The handlers are invoked directly via the shared
// TestUtils helpers; none of these require a live PIE HUD. A non-existent handle can
// never re-resolve to Found, so the no-PIE action path always answers synchronously
// with a coded error (the async settle driver never starts).

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Drive/DriveActionCommon.h"
#include "Tests/TestUtils.h"

// ============================================================================
// Test: all seven async drive verbs are registered
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveActionsRegisteredTest,
    "PinWright.drive.actions.MethodsRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveActionsRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("drive.click is registered"), IsHandlerRegistered(TEXT("drive.click")));
    TestTrue(TEXT("drive.hover is registered"), IsHandlerRegistered(TEXT("drive.hover")));
    TestTrue(TEXT("drive.scroll is registered"), IsHandlerRegistered(TEXT("drive.scroll")));
    TestTrue(TEXT("drive.type is registered"), IsHandlerRegistered(TEXT("drive.type")));
    TestTrue(TEXT("drive.key is registered"), IsHandlerRegistered(TEXT("drive.key")));
    TestTrue(TEXT("drive.drag is registered"), IsHandlerRegistered(TEXT("drive.drag")));
    TestTrue(TEXT("drive.wait_for is registered"), IsHandlerRegistered(TEXT("drive.wait_for")));
    return true;
}

// ============================================================================
// Test: the observe mode DEFAULTS to None (compact action result) and the
// explicit list / list+screenshot tokens still re-enable the observation.
// This is the verbosity-fix default: an action no longer embeds a full element
// list / screenshot the caller did not ask for.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveObserveModeDefaultNoneTest,
    "PinWright.drive.actions.ObserveModeDefaultsToNone",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveObserveModeDefaultNoneTest::RunTest(const FString& Parameters)
{
    // Empty payload -> the param is absent -> the compact default.
    {
        FHandlerContext Ctx = FHandlerContext::MakeTestContext(
            TEXT("test-id"), TEXT("drive.click"), MakeShared<FJsonObject>());
        TestEqual(TEXT("absent observe param defaults to None"),
            static_cast<int32>(FDriveActionCommon::ParseObserveMode(Ctx)),
            static_cast<int32>(EDriveObserveMode::None));
    }

    // observe=list re-includes the element list (no screenshot).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("observe"), TEXT("list"));
        FHandlerContext Ctx = FHandlerContext::MakeTestContext(
            TEXT("test-id"), TEXT("drive.click"), Payload);
        TestEqual(TEXT("observe=list -> List"),
            static_cast<int32>(FDriveActionCommon::ParseObserveMode(Ctx)),
            static_cast<int32>(EDriveObserveMode::List));
    }

    // observe=list+screenshot re-includes the list and the screenshot.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("observe"), TEXT("list+screenshot"));
        FHandlerContext Ctx = FHandlerContext::MakeTestContext(
            TEXT("test-id"), TEXT("drive.click"), Payload);
        TestEqual(TEXT("observe=list+screenshot -> ListAndScreenshot"),
            static_cast<int32>(FDriveActionCommon::ParseObserveMode(Ctx)),
            static_cast<int32>(EDriveObserveMode::ListAndScreenshot));
    }

    // The full_diff param is discoverable on the action verbs.
    TestNotNull(TEXT("drive.click exposes full_diff"),
        GetRegisteredParamSpec(TEXT("drive.click"), TEXT("full_diff")));

    return true;
}

// ============================================================================
// Test: drive.click without a handle is rejected (param validation)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveClickMissingHandleTest,
    "PinWright.drive.actions.ClickMissingHandleRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveClickMissingHandleTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.click"), MakeShared<FJsonObject>(), Capture);

    TestTrue(TEXT("drive.click handler found"), bFound);
    TestTrue(TEXT("drive.click sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("drive.click rejects a missing handle"), Capture.bSuccess);
    TestTrue(TEXT("drive.click missing-handle error is coded"), !Capture.ErrorCode.IsEmpty());
    return true;
}

// ============================================================================
// Test: drive.type without text is rejected (param validation)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveTypeMissingTextTest,
    "PinWright.drive.actions.TypeMissingTextRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveTypeMissingTextTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    // Provide a handle but omit the required `text`.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("handle"), TEXT("SomeHandle"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.type"), Payload, Capture);

    TestTrue(TEXT("drive.type handler found"), bFound);
    TestTrue(TEXT("drive.type sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("drive.type rejects a missing text"), Capture.bSuccess);
    return true;
}

// ============================================================================
// Test: drive.key without a key name is rejected (param validation)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveKeyMissingKeyTest,
    "PinWright.drive.actions.KeyMissingKeyRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveKeyMissingKeyTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.key"), MakeShared<FJsonObject>(), Capture);

    TestTrue(TEXT("drive.key handler found"), bFound);
    TestTrue(TEXT("drive.key sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("drive.key rejects a missing key"), Capture.bSuccess);
    return true;
}

// ============================================================================
// Test: drive.key with an unknown key name returns INVALID_KEY
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveKeyInvalidKeyTest,
    "PinWright.drive.actions.KeyInvalidKeyName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveKeyInvalidKeyTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("key"), TEXT("__not_a_real_key__"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.key"), Payload, Capture);

    TestTrue(TEXT("drive.key handler found"), bFound);
    TestFalse(TEXT("drive.key rejects an unknown key"), Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_KEY"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_KEY));
    return true;
}

// ============================================================================
// Test: drive.drag without a release point returns INVALID_ARGUMENT
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveDragMissingTargetTest,
    "PinWright.drive.actions.DragMissingReleasePoint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveDragMissingTargetTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    // A from-handle but neither to_handle nor to_x/to_y. This is a synchronous
    // argument error reached before any live resolve, so it never needs PIE.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("handle"), TEXT("FromHandle"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.drag"), Payload, Capture);

    TestTrue(TEXT("drive.drag handler found"), bFound);
    TestFalse(TEXT("drive.drag rejects a missing release point"), Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    return true;
}

// ============================================================================
// Test: drive.wait_for without a condition returns CONDITION_INVALID
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWaitForMissingConditionTest,
    "PinWright.drive.actions.WaitForMissingConditionInvalid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWaitForMissingConditionTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.wait_for"), MakeShared<FJsonObject>(), Capture);

    TestTrue(TEXT("drive.wait_for handler found"), bFound);
    TestFalse(TEXT("drive.wait_for rejects a missing condition"), Capture.bSuccess);
    TestEqual(TEXT("error code is CONDITION_INVALID"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_CONDITION_INVALID));
    return true;
}

// ============================================================================
// Test: drive.click with no live PIE returns a clean coded error (no hang/crash)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveClickNoPieCleanErrorTest,
    "PinWright.drive.actions.ClickNoPieCleanError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveClickNoPieCleanErrorTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    // A handle that cannot exist forces a non-Found re-resolve, so the action
    // answers synchronously (the async settle driver never starts) with the
    // resolver's coded TARGET/UI error instead of hanging.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("handle"), TEXT("__pinwright_no_such_handle__"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.click"), Payload, Capture);

    TestTrue(TEXT("drive.click handler found"), bFound);
    TestTrue(TEXT("drive.click answered synchronously (no hang)"), Capture.bWasCalled);
    TestFalse(TEXT("drive.click no-PIE is not a fake success"), Capture.bSuccess);
    TestTrue(TEXT("drive.click no-PIE error is coded"), !Capture.ErrorCode.IsEmpty());
    return true;
}

// ============================================================================
// Test: drive.wait_for with no live PIE returns a clean coded error (no hang)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWaitForNoPieCleanErrorTest,
    "PinWright.drive.actions.WaitForNoPieCleanError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWaitForNoPieCleanErrorTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    // A valid condition gets past parsing to the live-UI pre-check, which fails
    // cleanly with no PIE and answers synchronously instead of polling the timeout.
    TSharedPtr<FJsonObject> Condition = MakeShared<FJsonObject>();
    Condition->SetStringField(TEXT("type"), TEXT("widget_present"));
    Condition->SetStringField(TEXT("target"), TEXT("AnyWidget"));
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("condition"), Condition);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.wait_for"), Payload, Capture);

    TestTrue(TEXT("drive.wait_for handler found"), bFound);
    // No PIE -> the pre-check fails synchronously; if a viewport happened to be
    // present the settle driver runs async and no synchronous response lands. Either
    // way there is no crash or hang. When a synchronous response did land, it must
    // be a clean coded error, never a fake success.
    if (Capture.bWasCalled)
    {
        TestFalse(TEXT("drive.wait_for no-PIE is not a fake success"), Capture.bSuccess);
        TestTrue(TEXT("drive.wait_for no-PIE error is coded"), !Capture.ErrorCode.IsEmpty());
    }
    return true;
}
