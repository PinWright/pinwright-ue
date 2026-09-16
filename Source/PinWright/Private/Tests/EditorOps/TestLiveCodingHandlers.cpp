// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red test for F-live-coding-trigger: PinWright must expose in-process Live Coding
// RPCs so agents iterating on C++ can trigger the editor's own Live Coding compile
// (Ctrl+Alt+F11 equivalent) and read whether the patch applied — instead of only
// system.run_ubt, which spawns an EXTERNAL UBT process that cannot patch the running
// editor. The ticket's proposed surface (over ILiveCodingModule, UE 5.8 parity with
// LiveCodingToolset) is two verbs:
//   - system.live_coding_compile()  — trigger ILiveCodingModule::Compile as a job.
//   - system.live_coding_status()   — enabled?, session started?, last compile result.
//
// This test asserts the CORRECT behavior the ticket requires: both verbs are
// registered handlers on the dispatch surface. Pre-fix, grepping LiveCoding across
// Source/ is zero and neither method is registered, so both assertions fail
// (Result={Fail}) — the defect reproduced. Once the handlers are added with
// REGISTER_RPC_HANDLER, the auto-registration list carries both names and this test
// flips green by construction, regardless of the response shape the implementer
// chooses for status/compile.
//
// Registration-presence is the same contract the sibling system-domain long-running
// job handler uses (see TestSystemHandlers.cpp's FSystemRunTestsNoCrashTest, which
// asserts IsHandlerRegistered("system.run_tests")): invoking a Live-Coding /
// UBT-spawning handler in the automation environment would trigger real toolchain
// work, so the registered-surface check is the honest, deterministic guard.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerRegistration.h"
#include "Tests/TestUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemLiveCodingRpcHandlersRegisteredTest,
    "PinWright.system.live_coding.RpcHandlersRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemLiveCodingRpcHandlersRegisteredTest::RunTest(const FString& Parameters)
{
    // The in-process Live Coding status readback the ticket requires.
    TestTrue(TEXT("system.live_coding_status handler is registered"),
        IsHandlerRegistered(TEXT("system.live_coding_status")));

    // The in-process Live Coding compile trigger the ticket requires.
    TestTrue(TEXT("system.live_coding_compile handler is registered"),
        IsHandlerRegistered(TEXT("system.live_coding_compile")));

    return true;
}

// Strengthens the registration-presence assertion above with the response-shape
// assertion the red-test author invited: system.live_coding_status must actually
// respond with the documented availability contract, not merely exist as a
// registered name. Drives the REAL production handler via InvokeHandlerWithCapture
// (the status handler reads FModuleManager + FPluginState only — no Ctx.GetSubsystem()
// deref — so the null-subsystem test context is safe) and never triggers a compile,
// so it is safe even on a host where Live Coding happens to be enabled for the
// session. Only system.live_coding_status is exercised here — invoking
// system.live_coding_compile is deliberately avoided (it would start real toolchain
// work if Live Coding were enabled). Pre-fix the handler is unregistered, so
// InvokeHandlerWithCapture returns false and this fails alongside the red test above.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemLiveCodingStatusReportsAvailabilityTest,
    "PinWright.system.live_coding.StatusReportsAvailability",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemLiveCodingStatusReportsAvailabilityTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TestTrue(TEXT("system.live_coding_status handler is registered and invoked"),
        InvokeHandlerWithCapture(TEXT("system.live_coding_status"), MakeShared<FJsonObject>(), Capture));
    TestTrue(TEXT("status responded"), Capture.bWasCalled);
    // A status query always reports — it never errors, regardless of whether Live
    // Coding is compiled in / loaded / enabled.
    TestTrue(TEXT("status is a success"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    // The documented availability contract: these booleans are always present.
    bool bField = false;
    TestTrue(TEXT("reports 'available'"), Capture.Result->TryGetBoolField(TEXT("available"), bField));
    TestTrue(TEXT("reports 'moduleLoaded'"), Capture.Result->TryGetBoolField(TEXT("moduleLoaded"), bField));
    TestTrue(TEXT("reports 'enabledForSession'"), Capture.Result->TryGetBoolField(TEXT("enabledForSession"), bField));
    TestTrue(TEXT("reports 'compiling'"), Capture.Result->TryGetBoolField(TEXT("compiling"), bField));

    // The durable last-compile result the status RPC reads back from FPluginState
    // (defaults to "None" before any compile runs this session).
    FString LastResult;
    TestTrue(TEXT("reports 'lastCompileResult'"),
        Capture.Result->TryGetStringField(TEXT("lastCompileResult"), LastResult));
    TestFalse(TEXT("'lastCompileResult' is non-empty"), LastResult.IsEmpty());
    return true;
}
