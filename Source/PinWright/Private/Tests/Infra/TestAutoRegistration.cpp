// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the auto-registration system (REGISTER_RPC_HANDLER macro, FAutoRegisterHandler)
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"

// ============================================================================
// Register two test-only handlers in this translation unit using __COUNTER__
// to verify that multiple registrations in the same file do not collide.
// ============================================================================

REGISTER_RPC_HANDLER("_test.alpha", "_test", "Test handler alpha",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "The name parameter"),
        RPC_PARAM_OPT("count", "integer", "Optional count")
    ))
{
    Ctx.SendSuccess(TEXT("alpha ok"));
    return true;
}

REGISTER_RPC_HANDLER("_test.beta", "_test", "Test handler beta", RPC_NO_PARAMS)
{
    Ctx.SendSuccess(TEXT("beta ok"));
    return true;
}

REGISTER_RPC_HANDLER("_test.gamma", "_test", "Test handler gamma",
    RPC_PARAMS(
        RPC_PARAM_DEF("mode", "string", "Operating mode", "default_mode")
    ))
{
    Ctx.SendSuccess(TEXT("gamma ok"));
    return true;
}

// ============================================================================
// Test: GetPendingRegistrations returns non-empty array
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutoRegNonEmptyTest,
    "PinWright.infra.auto_registration.PendingRegistrationsNonEmpty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAutoRegNonEmptyTest::RunTest(const FString& Parameters)
{
    const TArray<FHandlerRegistration>& Regs = FAutoRegisterHandler::GetPendingRegistrations();
    TestTrue(TEXT("Pending registrations array is non-empty"), Regs.Num() > 0);
    return true;
}

// ============================================================================
// Test: Registration records contain correct MethodName, Category, Summary
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutoRegFieldsTest,
    "PinWright.infra.auto_registration.CorrectFieldValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAutoRegFieldsTest::RunTest(const FString& Parameters)
{
    const TArray<FHandlerRegistration>& Regs = FAutoRegisterHandler::GetPendingRegistrations();

    const FHandlerRegistration* AlphaReg = nullptr;
    for (const FHandlerRegistration& Reg : Regs)
    {
        if (Reg.MethodName == TEXT("_test.alpha"))
        {
            AlphaReg = &Reg;
            break;
        }
    }

    TestTrue(TEXT("_test.alpha registration found"), AlphaReg != nullptr);
    if (AlphaReg)
    {
        TestEqual(TEXT("Category is _test"), AlphaReg->Category, TEXT("_test"));
        TestEqual(TEXT("Summary is correct"), AlphaReg->Summary, TEXT("Test handler alpha"));
    }

    return true;
}

// ============================================================================
// Test: Registration records contain correct FParamSpec data
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutoRegParamSpecTest,
    "PinWright.infra.auto_registration.ParamSpecData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAutoRegParamSpecTest::RunTest(const FString& Parameters)
{
    const TArray<FHandlerRegistration>& Regs = FAutoRegisterHandler::GetPendingRegistrations();

    const FHandlerRegistration* AlphaReg = nullptr;
    for (const FHandlerRegistration& Reg : Regs)
    {
        if (Reg.MethodName == TEXT("_test.alpha"))
        {
            AlphaReg = &Reg;
            break;
        }
    }

    TestTrue(TEXT("_test.alpha found for param check"), AlphaReg != nullptr);
    if (!AlphaReg) return true;

    TestEqual(TEXT("Alpha has 2 params"), AlphaReg->Params.Num(), 2);

    if (AlphaReg->Params.Num() >= 2)
    {
        // First param: required "name" of type "string"
        TestEqual(TEXT("Param 0 name"), AlphaReg->Params[0].Name, TEXT("name"));
        TestEqual(TEXT("Param 0 type"), AlphaReg->Params[0].Type, TEXT("string"));
        TestEqual(TEXT("Param 0 description"), AlphaReg->Params[0].Description, TEXT("The name parameter"));
        TestTrue(TEXT("Param 0 is required"), AlphaReg->Params[0].bRequired);

        // Second param: optional "count" of type "integer"
        TestEqual(TEXT("Param 1 name"), AlphaReg->Params[1].Name, TEXT("count"));
        TestEqual(TEXT("Param 1 type"), AlphaReg->Params[1].Type, TEXT("integer"));
        TestFalse(TEXT("Param 1 is optional"), AlphaReg->Params[1].bRequired);
    }

    return true;
}

// ============================================================================
// Test: Multiple registrations in same file don't collide (__COUNTER__ safety)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutoRegNoCollisionTest,
    "PinWright.infra.auto_registration.NoCounterCollision",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAutoRegNoCollisionTest::RunTest(const FString& Parameters)
{
    const TArray<FHandlerRegistration>& Regs = FAutoRegisterHandler::GetPendingRegistrations();

    bool bFoundAlpha = false;
    bool bFoundBeta = false;

    for (const FHandlerRegistration& Reg : Regs)
    {
        if (Reg.MethodName == TEXT("_test.alpha")) bFoundAlpha = true;
        if (Reg.MethodName == TEXT("_test.beta"))  bFoundBeta = true;
    }

    TestTrue(TEXT("_test.alpha registered"), bFoundAlpha);
    TestTrue(TEXT("_test.beta registered"), bFoundBeta);

    return true;
}

// ============================================================================
// Test: Handler function pointer is callable and receives correct context
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutoRegCallableTest,
    "PinWright.infra.auto_registration.HandlerCallable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAutoRegCallableTest::RunTest(const FString& Parameters)
{
    const TArray<FHandlerRegistration>& Regs = FAutoRegisterHandler::GetPendingRegistrations();

    const FHandlerRegistration* BetaReg = nullptr;
    for (const FHandlerRegistration& Reg : Regs)
    {
        if (Reg.MethodName == TEXT("_test.beta"))
        {
            BetaReg = &Reg;
            break;
        }
    }

    TestTrue(TEXT("_test.beta found"), BetaReg != nullptr);
    if (!BetaReg) return true;

    TestTrue(TEXT("Func pointer is non-null"), BetaReg->Func != nullptr);

    if (BetaReg->Func)
    {
        FHandlerContext Ctx = FHandlerContext::MakeTestContext(
            TEXT("test-call-id"), TEXT("_test.beta"), MakeShared<FJsonObject>());

        bool bResult = BetaReg->Func(Ctx);
        TestTrue(TEXT("Handler returns true"), bResult);
    }

    return true;
}

// ============================================================================
// Test: RPC_PARAM_DEF sets the Default field
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutoRegParamDefDefaultTest,
    "PinWright.infra.auto_registration.ParamDefDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAutoRegParamDefDefaultTest::RunTest(const FString& Parameters)
{
    const TArray<FHandlerRegistration>& Regs = FAutoRegisterHandler::GetPendingRegistrations();

    const FHandlerRegistration* GammaReg = nullptr;
    for (const FHandlerRegistration& Reg : Regs)
    {
        if (Reg.MethodName == TEXT("_test.gamma"))
        {
            GammaReg = &Reg;
            break;
        }
    }

    TestTrue(TEXT("_test.gamma found"), GammaReg != nullptr);
    if (!GammaReg) return true;

    TestEqual(TEXT("Gamma has 1 param"), GammaReg->Params.Num(), 1);
    if (GammaReg->Params.Num() >= 1)
    {
        TestEqual(TEXT("Param name is mode"), GammaReg->Params[0].Name, TEXT("mode"));
        TestFalse(TEXT("Param is optional"), GammaReg->Params[0].bRequired);
        TestEqual(TEXT("Default is default_mode"), GammaReg->Params[0].Default, TEXT("default_mode"));
    }

    return true;
}
