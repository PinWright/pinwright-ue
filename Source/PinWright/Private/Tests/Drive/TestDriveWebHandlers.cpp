// Copyright (c) 2026 Alexander Penkin. MIT License.

// Integration tests for the surface=web branch of the drive.* handlers (FDriveWebHandlers).
// They exercise the wiring seam WITHOUT a live CEF browser:
//   - the verbs that branch to web (observe/expect/click/type/wait_for) are registered;
//   - with no discoverable UWebBrowser, the web verbs answer SYNCHRONOUSLY with a clean
//     WEB_BROWSER_NOT_FOUND instead of hanging or crashing;
//   - param/condition validation still rejects a web click with no handle and a web
//     expect/wait_for with no condition, all synchronously and browser-independent.
//
// The web path is asynchronous: with a live browser present (a host WebUI HUD that uses the
// UMG UWebBrowser widget), the verb opens an async token and returns no synchronous response,
// so the no-browser assertions are skipped with a warning rather than failing - and the test
// deliberately does NOT poke a live browser. None of these require a live browser.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Drive/DriveWebBridge.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

// Uniquely-named namespace (not anonymous) so a Unity TU merge can't ODR-clash these
// helpers with same-named ones in sibling test units.
namespace PinWrightDriveWebHandlersTest
{
    // A {surface:"web"} args object; callers add handle/text/condition as needed.
    TSharedPtr<FJsonObject> MakeWebPayload()
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("surface"), TEXT("web"));
        return P;
    }

    // True when there is no live UWebBrowser to drive (the deterministic no-browser path).
    bool NoLiveBrowser()
    {
        return FDriveWebBridge::DiscoverBrowsers().Num() == 0;
    }
}

using namespace PinWrightDriveWebHandlersTest;

// ============================================================================
// Test: the verbs that branch to the web path are registered
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebintMethodsRegisteredTest,
    "PinWright.drive.webint.MethodsRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebintMethodsRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("drive.observe is registered"), IsHandlerRegistered(TEXT("drive.observe")));
    TestTrue(TEXT("drive.expect is registered"), IsHandlerRegistered(TEXT("drive.expect")));
    TestTrue(TEXT("drive.click is registered"), IsHandlerRegistered(TEXT("drive.click")));
    TestTrue(TEXT("drive.type is registered"), IsHandlerRegistered(TEXT("drive.type")));
    TestTrue(TEXT("drive.wait_for is registered"), IsHandlerRegistered(TEXT("drive.wait_for")));
    // The action verbs that now also branch to the web path (scroll/drag/hover/key).
    TestTrue(TEXT("drive.scroll is registered"), IsHandlerRegistered(TEXT("drive.scroll")));
    TestTrue(TEXT("drive.drag is registered"), IsHandlerRegistered(TEXT("drive.drag")));
    TestTrue(TEXT("drive.hover is registered"), IsHandlerRegistered(TEXT("drive.hover")));
    TestTrue(TEXT("drive.key is registered"), IsHandlerRegistered(TEXT("drive.key")));
    return true;
}

// ============================================================================
// Test: drive.observe surface=web with no live browser returns WEB_BROWSER_NOT_FOUND
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebintObserveNoBrowserTest,
    "PinWright.drive.webint.ObserveWebNoBrowserCleanError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebintObserveNoBrowserTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    if (!NoLiveBrowser())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("live-browser-present"),
            TEXT("Live CEF browser present; skipping surface=web no-browser test to "
                 "avoid driving the live HUD (the web path would go async)."));
        return true;
    }

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.observe"), MakeWebPayload(), Capture);

    TestTrue(TEXT("drive.observe handler found"), bFound);
    TestTrue(TEXT("observe web answered synchronously (no hang)"), Capture.bWasCalled);
    TestFalse(TEXT("observe web no-browser is not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("observe web error is WEB_BROWSER_NOT_FOUND"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_WEB_BROWSER_NOT_FOUND));
    return true;
}

// ============================================================================
// Test: drive.click surface=web (valid handle) with no live browser returns
// WEB_BROWSER_NOT_FOUND
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebintClickNoBrowserTest,
    "PinWright.drive.webint.ClickWebNoBrowserCleanError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebintClickNoBrowserTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    if (!NoLiveBrowser())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("live-browser-present"),
            TEXT("Live CEF browser present; skipping surface=web no-browser click test."));
        return true;
    }

    // A handle is supplied so the handler clears its required-param check and reaches the
    // browser selection, which fails cleanly with no live browser.
    TSharedPtr<FJsonObject> Payload = MakeWebPayload();
    Payload->SetStringField(TEXT("handle"), TEXT("pw-1"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.click"), Payload, Capture);

    TestTrue(TEXT("drive.click handler found"), bFound);
    TestTrue(TEXT("click web answered synchronously (no hang)"), Capture.bWasCalled);
    TestFalse(TEXT("click web no-browser is not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("click web error is WEB_BROWSER_NOT_FOUND"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_WEB_BROWSER_NOT_FOUND));
    return true;
}

// ============================================================================
// Test: drive.wait_for surface=web (valid condition) with no live browser returns
// WEB_BROWSER_NOT_FOUND (the pre-check, not a full-timeout poll)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebintWaitForNoBrowserTest,
    "PinWright.drive.webint.WaitForWebNoBrowserCleanError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebintWaitForNoBrowserTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    if (!NoLiveBrowser())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("live-browser-present"),
            TEXT("Live CEF browser present; skipping surface=web no-browser wait_for test."));
        return true;
    }

    TSharedPtr<FJsonObject> Condition = MakeShared<FJsonObject>();
    Condition->SetStringField(TEXT("type"), TEXT("widget_present"));
    Condition->SetStringField(TEXT("target"), TEXT("AnyWidget"));
    TSharedPtr<FJsonObject> Payload = MakeWebPayload();
    Payload->SetObjectField(TEXT("condition"), Condition);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.wait_for"), Payload, Capture);

    TestTrue(TEXT("drive.wait_for handler found"), bFound);
    TestTrue(TEXT("wait_for web answered synchronously (no full-timeout poll)"), Capture.bWasCalled);
    TestFalse(TEXT("wait_for web no-browser is not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("wait_for web error is WEB_BROWSER_NOT_FOUND"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_WEB_BROWSER_NOT_FOUND));
    return true;
}

// ============================================================================
// Test: drive.click surface=web without a handle is rejected (param validation,
// browser-independent and always synchronous)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebintClickMissingHandleTest,
    "PinWright.drive.webint.ClickWebMissingHandleRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebintClickMissingHandleTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.click"), MakeWebPayload(), Capture);

    TestTrue(TEXT("drive.click handler found"), bFound);
    TestTrue(TEXT("click web missing-handle answered synchronously"), Capture.bWasCalled);
    TestFalse(TEXT("click web rejects a missing handle"), Capture.bSuccess);
    TestTrue(TEXT("click web missing-handle error is coded"), !Capture.ErrorCode.IsEmpty());
    return true;
}

// ============================================================================
// Test: drive.expect surface=web without a condition returns CONDITION_INVALID
// (browser-independent and always synchronous)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebintExpectMissingConditionTest,
    "PinWright.drive.webint.ExpectWebMissingConditionInvalid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebintExpectMissingConditionTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.expect"), MakeWebPayload(), Capture);

    TestTrue(TEXT("drive.expect handler found"), bFound);
    TestFalse(TEXT("expect web rejects a missing condition"), Capture.bSuccess);
    TestEqual(TEXT("expect web error is CONDITION_INVALID"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_CONDITION_INVALID));
    return true;
}

// ============================================================================
// Test: drive.scroll surface=web (valid handle) with no live browser returns
// WEB_BROWSER_NOT_FOUND
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebintScrollNoBrowserTest,
    "PinWright.drive.webint.ScrollWebNoBrowserCleanError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebintScrollNoBrowserTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    if (!NoLiveBrowser())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("live-browser-present"),
            TEXT("Live CEF browser present; skipping surface=web no-browser scroll test."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeWebPayload();
    Payload->SetStringField(TEXT("handle"), TEXT("pw-1"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.scroll"), Payload, Capture);

    TestTrue(TEXT("drive.scroll handler found"), bFound);
    TestTrue(TEXT("scroll web answered synchronously (no hang)"), Capture.bWasCalled);
    TestFalse(TEXT("scroll web no-browser is not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("scroll web error is WEB_BROWSER_NOT_FOUND"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_WEB_BROWSER_NOT_FOUND));
    return true;
}

// ============================================================================
// Test: drive.hover surface=web (valid handle) with no live browser returns
// WEB_BROWSER_NOT_FOUND
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebintHoverNoBrowserTest,
    "PinWright.drive.webint.HoverWebNoBrowserCleanError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebintHoverNoBrowserTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    if (!NoLiveBrowser())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("live-browser-present"),
            TEXT("Live CEF browser present; skipping surface=web no-browser hover test."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeWebPayload();
    Payload->SetStringField(TEXT("handle"), TEXT("pw-1"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.hover"), Payload, Capture);

    TestTrue(TEXT("drive.hover handler found"), bFound);
    TestTrue(TEXT("hover web answered synchronously (no hang)"), Capture.bWasCalled);
    TestFalse(TEXT("hover web no-browser is not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("hover web error is WEB_BROWSER_NOT_FOUND"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_WEB_BROWSER_NOT_FOUND));
    return true;
}

// ============================================================================
// Test: drive.key surface=web (valid key) with no live browser returns
// WEB_BROWSER_NOT_FOUND. The key "ArrowLeft" is a DOM key name and NOT a native FKey,
// so reaching the browser-selection step (rather than an INVALID_KEY rejection) also
// proves the web branch runs before the native FKey validation.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebintKeyNoBrowserTest,
    "PinWright.drive.webint.KeyWebNoBrowserCleanError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebintKeyNoBrowserTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    if (!NoLiveBrowser())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("live-browser-present"),
            TEXT("Live CEF browser present; skipping surface=web no-browser key test."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeWebPayload();
    Payload->SetStringField(TEXT("key"), TEXT("ArrowLeft"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.key"), Payload, Capture);

    TestTrue(TEXT("drive.key handler found"), bFound);
    TestTrue(TEXT("key web answered synchronously (no hang)"), Capture.bWasCalled);
    TestFalse(TEXT("key web no-browser is not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("key web error is WEB_BROWSER_NOT_FOUND"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_WEB_BROWSER_NOT_FOUND));
    return true;
}

// ============================================================================
// Test: drive.drag surface=web (handle + to_handle) with no live browser returns
// WEB_BROWSER_NOT_FOUND
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebintDragNoBrowserTest,
    "PinWright.drive.webint.DragWebNoBrowserCleanError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebintDragNoBrowserTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    if (!NoLiveBrowser())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("live-browser-present"),
            TEXT("Live CEF browser present; skipping surface=web no-browser drag test."));
        return true;
    }

    // Both handles are supplied so the handler clears its required/argument checks and reaches
    // the browser selection, which fails cleanly with no live browser.
    TSharedPtr<FJsonObject> Payload = MakeWebPayload();
    Payload->SetStringField(TEXT("handle"), TEXT("pw-1"));
    Payload->SetStringField(TEXT("to_handle"), TEXT("pw-2"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.drag"), Payload, Capture);

    TestTrue(TEXT("drive.drag handler found"), bFound);
    TestTrue(TEXT("drag web answered synchronously (no hang)"), Capture.bWasCalled);
    TestFalse(TEXT("drag web no-browser is not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("drag web error is WEB_BROWSER_NOT_FOUND"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_WEB_BROWSER_NOT_FOUND));
    return true;
}

// ============================================================================
// Test: drive.drag surface=web with a handle but no to_handle is rejected with
// INVALID_ARGUMENT (web drag is DOM-element-to-element; browser-independent and
// always synchronous, since the to_handle check precedes browser selection)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebintDragMissingToHandleTest,
    "PinWright.drive.webint.DragWebMissingToHandleInvalid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebintDragMissingToHandleTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    TSharedPtr<FJsonObject> Payload = MakeWebPayload();
    Payload->SetStringField(TEXT("handle"), TEXT("pw-1"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.drag"), Payload, Capture);

    TestTrue(TEXT("drive.drag handler found"), bFound);
    TestTrue(TEXT("drag web missing-to_handle answered synchronously"), Capture.bWasCalled);
    TestFalse(TEXT("drag web rejects a missing to_handle"), Capture.bSuccess);
    TestEqual(TEXT("drag web missing-to_handle error is INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    return true;
}
