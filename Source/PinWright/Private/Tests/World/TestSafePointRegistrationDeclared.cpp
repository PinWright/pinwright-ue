// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tick-unsafety declared at the handler (REGISTER_RPC_HANDLER_TICK_UNSAFE) rather than
// in Dispatch/SafePoint.cpp's legacy name table. The probe below is registered ONLY
// through the macro - it is in no table - so every assertion here reads the
// registration flag through the unchanged PinWrightSafePoint read API and the real
// FRpcDispatcher::ProcessRequest gate.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dispatch/SafePoint.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

// Answers synchronously, so "did not respond on the unsafe stack" is observable.
REGISTER_RPC_HANDLER_TICK_UNSAFE("_test.safe_point_declared_tick_unsafe", "_test",
    "Test-only probe registered tick-unsafe through the registration macro", RPC_NO_PARAMS)
{
    Ctx.SendSuccess(TEXT("declared tick-unsafe probe ok"));
    return true;
}

namespace SafePointRegistrationDeclaredTests
{
    static const TCHAR* const GProbeMethod = TEXT("_test.safe_point_declared_tick_unsafe");

    struct FScopedForcedUnsafe
    {
        FScopedForcedUnsafe() { PinWrightSafePoint::SetForcedUnsafeForTests(true); }
        ~FScopedForcedUnsafe() { PinWrightSafePoint::SetForcedUnsafeForTests(false); }
    };
}

// Counterfactual: drop the DeclaredTickUnsafeMethodSet() term from
// PinWrightSafePoint::IsTickUnsafeMethod (or stop the macro passing bTickUnsafe) and the
// predicate, the sorted list and "did not run on the unsafe stack" all fail, because the
// probe answers inline.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafePointRegistrationDeclaredTickUnsafeTest,
    "PinWright.core.safe_point.RegistrationDeclaredTickUnsafeIsGated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSafePointRegistrationDeclaredTickUnsafeTest::RunTest(const FString& Parameters)
{
    using namespace SafePointRegistrationDeclaredTests;
    const FString Method(GProbeMethod);

    const FHandlerRegistration* Probe = nullptr;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName == Method)
        {
            Probe = &Reg;
            break;
        }
    }
    if (!TestNotNull(TEXT("the macro-registered probe is in the registry"), Probe))
    {
        return false;
    }
    TestTrue(TEXT("REGISTER_RPC_HANDLER_TICK_UNSAFE sets bTickUnsafe"), Probe->bTickUnsafe);
    TestTrue(TEXT("a tick-unsafe registration is mutating by default"), Probe->bMutating);

    TestTrue(TEXT("IsTickUnsafeMethod reads the registration flag"),
        PinWrightSafePoint::IsTickUnsafeMethod(Method));
    TestTrue(TEXT("GetTickUnsafeMethods lists the registration-declared verb"),
        PinWrightSafePoint::GetTickUnsafeMethods().Contains(Method));

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);
    {
        FScopedForcedUnsafe ForcedUnsafe;
        Dispatcher.ProcessRequest(TEXT("req-declared-tick-unsafe"), Method,
            MakeShared<FJsonObject>());
        TestFalse(TEXT("a registration-declared tick-unsafe verb does not run on an unsafe stack"),
            Sink->bWasCalled);
    }

    TestTrue(TEXT("the safe point reports safe once nothing is ticking"),
        PinWrightSafePoint::IsSafeNow());
    Dispatcher.ProcessPendingRequests();
    TestTrue(TEXT("the deferred request ran at the safe point"), Sink->bWasCalled);
    TestTrue(TEXT("the deferred request succeeded"), Sink->bSuccess);
    return true;
}
