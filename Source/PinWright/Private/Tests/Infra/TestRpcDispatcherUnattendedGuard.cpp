// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B0-a (B-asset-create-modal-deadlock): FRpcDispatcher must
// hold GIsRunningUnattendedScript for the duration of every dispatched handler body,
// and must restore the previous value afterwards.
//
// A direct "no modal appeared" assertion cannot be written: the automation harness
// runs -unattended, which already short-circuits FMessageDialog::Open at
// MessageDialog.cpp:157 before any observable delegate fires, so a dialog-free run
// proves nothing about the guard. The machine-checkable proxy is the flag itself,
// read from inside the handler body - that is the single input both engine
// suppression sites consult (MessageDialog.cpp:157 and
// SlateApplication.cpp's GIsRunningUnattendedScript window cancel).
//
// Counterfactual: delete the FScopedUnattendedRpc lines in RpcDispatcher.cpp and the
// inside-the-handler assertions fail while the after-dispatch ones still pass.

#include "Misc/AutomationTest.h"

#include "CoreGlobals.h"
#include "Dispatch/RpcDispatcher.h"
#include "Dispatch/ScopedUnattendedRpc.h"
#include "Dom/JsonObject.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Misc/ScopeExit.h"
#include "PinWrightSettings.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

// Probe state captured from INSIDE the dispatched handler body. File-scope statics
// rather than test locals because the handler is registered at static-init time and
// has no way to reach the running test instance.
static bool GPinWrightUnattendedProbeFlagInHandler = false;
static bool GPinWrightUnattendedProbeModeActiveInHandler = false;
static int32 GPinWrightUnattendedProbeCallCount = 0;

REGISTER_RPC_HANDLER("_test.unattended_probe", "_test",
    "Test-only probe that records GIsRunningUnattendedScript from inside a dispatched handler body",
    RPC_NO_PARAMS)
{
    GPinWrightUnattendedProbeFlagInHandler = GIsRunningUnattendedScript;
    GPinWrightUnattendedProbeModeActiveInHandler = PinWrightAutomationMode::IsActive();
    ++GPinWrightUnattendedProbeCallCount;
    Ctx.SendSuccess(TEXT("unattended probe ok"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRpcDispatcherUnattendedGuardTest,
    "PinWright.infra.dispatcher.UnattendedGuardScopesHandlerBody",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRpcDispatcherUnattendedGuardTest::RunTest(const FString& /*Parameters*/)
{
    UPinWrightSettings* Settings = GetMutableDefault<UPinWrightSettings>();
    if (!Settings)
    {
        AddError(TEXT("UPinWrightSettings CDO unavailable"));
        return false;
    }

    const bool bSavedSetting = Settings->bSuppressModalDialogsDuringRpc;
    const bool bSavedGlobal = GIsRunningUnattendedScript;
    ON_SCOPE_EXIT
    {
        PinWrightAutomationMode::ResetForTests();
        Settings->bSuppressModalDialogsDuringRpc = bSavedSetting;
        GIsRunningUnattendedScript = bSavedGlobal;
    };

    // Pin both ends of the observation: the kill switch on and the global off, so a
    // harness that already set GIsRunningUnattendedScript cannot make the assertion
    // pass for the wrong reason.
    PinWrightAutomationMode::ResetForTests();
    Settings->bSuppressModalDialogsDuringRpc = true;
    GIsRunningUnattendedScript = false;

    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    GPinWrightUnattendedProbeFlagInHandler = false;
    GPinWrightUnattendedProbeModeActiveInHandler = false;
    GPinWrightUnattendedProbeCallCount = 0;

    bool bSuccess = false;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("_test.unattended_probe"),
        TEXT("req-unattended-guard-on"), MakeShared<FJsonObject>(), bSuccess, ErrorCode);

    TestEqual(TEXT("probe handler ran exactly once"), GPinWrightUnattendedProbeCallCount, 1);
    TestTrue(FString::Printf(TEXT("probe dispatch succeeded (err='%s')"), *ErrorCode), bSuccess);

    TestTrue(TEXT("GIsRunningUnattendedScript is set INSIDE the dispatched handler"),
        GPinWrightUnattendedProbeFlagInHandler);
    TestTrue(TEXT("an engaged automation interval is open inside the dispatched handler"),
        GPinWrightUnattendedProbeModeActiveInHandler);
    TestFalse(TEXT("GIsRunningUnattendedScript is restored to its prior value after dispatch"),
        GIsRunningUnattendedScript);
    TestFalse(TEXT("no automation interval is left open after dispatch"),
        PinWrightAutomationMode::IsActive());

    // Kill switch off: the same dispatch must leave the process-global alone, so a
    // human debugging a handler interactively still gets the engine's prompts.
    Settings->bSuppressModalDialogsDuringRpc = false;
    GPinWrightUnattendedProbeFlagInHandler = true;
    GPinWrightUnattendedProbeCallCount = 0;

    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("_test.unattended_probe"),
        TEXT("req-unattended-guard-off"), MakeShared<FJsonObject>(), bSuccess, ErrorCode);

    TestEqual(TEXT("probe handler ran again with the kill switch off"),
        GPinWrightUnattendedProbeCallCount, 1);
    TestFalse(TEXT("kill switch off leaves GIsRunningUnattendedScript untouched"),
        GPinWrightUnattendedProbeFlagInHandler);
    TestFalse(TEXT("kill switch off engages no automation interval"),
        PinWrightAutomationMode::IsActive());

    return true;
}

// The scope nests: a handler that re-enters the dispatcher (the deferred / reentrant
// path) must not end the outer interval when its inner scope closes. FScopedUnattendedRpc
// is refcounted rather than a save/restore TGuardValue precisely for this.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRpcDispatcherUnattendedGuardNestsTest,
    "PinWright.infra.dispatcher.UnattendedGuardNestsSafely",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRpcDispatcherUnattendedGuardNestsTest::RunTest(const FString& /*Parameters*/)
{
    UPinWrightSettings* Settings = GetMutableDefault<UPinWrightSettings>();
    if (!Settings)
    {
        AddError(TEXT("UPinWrightSettings CDO unavailable"));
        return false;
    }

    const bool bSavedSetting = Settings->bSuppressModalDialogsDuringRpc;
    const bool bSavedGlobal = GIsRunningUnattendedScript;
    ON_SCOPE_EXIT
    {
        PinWrightAutomationMode::ResetForTests();
        Settings->bSuppressModalDialogsDuringRpc = bSavedSetting;
        GIsRunningUnattendedScript = bSavedGlobal;
    };

    PinWrightAutomationMode::ResetForTests();
    Settings->bSuppressModalDialogsDuringRpc = true;
    GIsRunningUnattendedScript = false;

    {
        FScopedUnattendedRpc Outer;
        TestTrue(TEXT("outer scope sets the flag"), GIsRunningUnattendedScript);
        {
            FScopedUnattendedRpc Inner;
            TestTrue(TEXT("inner scope keeps the flag set"), GIsRunningUnattendedScript);
        }
        TestTrue(TEXT("closing the inner scope does NOT punch a hole in the outer interval"),
            GIsRunningUnattendedScript);
    }
    TestFalse(TEXT("closing the outer scope restores the prior value"), GIsRunningUnattendedScript);

    return true;
}
