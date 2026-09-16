// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for the shared limit validation used by Niagara search verbs.
// These tests exercise the pure parameter helper and the two handler paths without PIE.
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Niagara/NiagaraSearchHandler.h"

namespace
{
    TSharedPtr<FJsonObject> MakeNiagaraSearchLimitPayload(double Limit)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("limit"), static_cast<double>(Limit));
        return Payload;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSearchLimitHelperTest,
    "PinWright.niagara.search.LimitHelper",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSearchLimitHelperTest::RunTest(const FString& Parameters)
{
    struct FLimitProbe
    {
        const TCHAR* Label;
        double Requested;
        bool bAccepted;
        int32 Expected;
    };

    const FLimitProbe Probes[] = {
        { TEXT("negative limit"), -1.0, false, 0 },
        { TEXT("zero limit"), 0.0, true, 0 },
        { TEXT("positive limit"), 1.0, true, 1 },
        { TEXT("huge limit"), 1.0e12, true, NiagaraSearch::MaxSearchLimit },
    };

    for (const FLimitProbe& Probe : Probes)
    {
        FTestResponseCapture Capture;
        FHandlerContext Context = FHandlerContext::MakeTestContextWithCapture(
            TEXT("niagara-search-limit-helper"), TEXT("_test.niagara.search_limit"),
            MakeNiagaraSearchLimitPayload(Probe.Requested), &Capture);

        int32 ResolvedLimit = -1;
        const bool bAccepted = NiagaraSearch::ResolveSearchLimit(Context, ResolvedLimit);
        TestEqual(*FString::Printf(TEXT("%s acceptance"), Probe.Label), bAccepted, Probe.bAccepted);

        if (Probe.bAccepted)
        {
            TestEqual(*FString::Printf(TEXT("%s resolved value"), Probe.Label),
                ResolvedLimit, Probe.Expected);
            TestFalse(*FString::Printf(TEXT("%s emits no response"), Probe.Label),
                Capture.bWasCalled);
        }
        else
        {
            TestTrue(TEXT("negative limit emits a response"), Capture.bWasCalled);
            TestFalse(TEXT("negative limit is rejected"), Capture.bSuccess);
            TestEqual(TEXT("negative limit error code"), Capture.ErrorCode,
                FString(ErrorCodes::ERR_INVALID_ARGUMENT));
            TestTrue(TEXT("negative limit error explains the lower bound"),
                Capture.Message.Contains(TEXT(">= 0")));
        }
    }

    // Keep the helper aligned with FHandlerContext::GetInt and the dispatcher gate:
    // numeric strings, booleans, and fractional numbers are existing accepted forms.
    auto TestCoercedLimit = [this](const TCHAR* Label,
                                   const TSharedPtr<FJsonObject>& Payload,
                                   int32 Expected)
    {
        FTestResponseCapture Capture;
        FHandlerContext Context = FHandlerContext::MakeTestContextWithCapture(
            TEXT("niagara-search-limit-coercion"), TEXT("_test.niagara.search_limit"),
            Payload, &Capture);

        int32 ResolvedLimit = -1;
        TestTrue(*FString::Printf(TEXT("%s is accepted"), Label),
            NiagaraSearch::ResolveSearchLimit(Context, ResolvedLimit));
        TestEqual(*FString::Printf(TEXT("%s uses GetInt-compatible value"), Label),
            ResolvedLimit, Expected);
        TestFalse(*FString::Printf(TEXT("%s emits no response"), Label), Capture.bWasCalled);
    };

    TSharedPtr<FJsonObject> NumericStringPayload = MakeShared<FJsonObject>();
    NumericStringPayload->SetStringField(TEXT("limit"), TEXT("12"));
    TestCoercedLimit(TEXT("numeric string limit"), NumericStringPayload, 12);

    TSharedPtr<FJsonObject> BooleanTruePayload = MakeShared<FJsonObject>();
    BooleanTruePayload->SetBoolField(TEXT("limit"), true);
    TestCoercedLimit(TEXT("true boolean limit"), BooleanTruePayload, 1);

    TSharedPtr<FJsonObject> BooleanFalsePayload = MakeShared<FJsonObject>();
    BooleanFalsePayload->SetBoolField(TEXT("limit"), false);
    TestCoercedLimit(TEXT("false boolean limit"), BooleanFalsePayload, 0);

    TSharedPtr<FJsonObject> FractionalPayload = MakeShared<FJsonObject>();
    FractionalPayload->SetNumberField(TEXT("limit"), 1.75);
    TestCoercedLimit(TEXT("fractional limit"), FractionalPayload, 1);

    TSharedPtr<FJsonObject> NegativeFractionalPayload = MakeShared<FJsonObject>();
    NegativeFractionalPayload->SetNumberField(TEXT("limit"), -0.5);
    TestCoercedLimit(TEXT("negative fractional limit"), NegativeFractionalPayload, 0);

    TSharedPtr<FJsonObject> HugeNumericStringPayload = MakeShared<FJsonObject>();
    HugeNumericStringPayload->SetStringField(TEXT("limit"), TEXT("1000000000000"));
    TestCoercedLimit(TEXT("huge numeric string limit"), HugeNumericStringPayload,
        NiagaraSearch::MaxSearchLimit);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSearchLimitHandlersTest,
    "PinWright.niagara.search.LimitHandlers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSearchLimitHandlersTest::RunTest(const FString& Parameters)
{
    // search_ops must reject a negative limit before reading the op catalog.
    {
        TSharedPtr<FJsonObject> Payload = MakeNiagaraSearchLimitPayload(-1.0);
        Payload->SetStringField(TEXT("query"), TEXT("Add"));

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("niagara.graph.search_ops"), Payload, Capture);
        TestTrue(TEXT("search_ops negative-limit handler found"), bFound);
        TestTrue(TEXT("search_ops negative-limit response sent"), Capture.bWasCalled);
        TestFalse(TEXT("search_ops negative limit rejected"), Capture.bSuccess);
        TestEqual(TEXT("search_ops negative-limit error code"), Capture.ErrorCode,
            FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    }

    // Zero remains a valid cap and preserves the full match count for pagination callers.
    {
        TSharedPtr<FJsonObject> Payload = MakeNiagaraSearchLimitPayload(0.0);
        Payload->SetStringField(TEXT("query"), TEXT("Add"));

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("niagara.graph.search_ops"), Payload, Capture);
        TestTrue(TEXT("search_ops zero-limit handler found"), bFound);
        TestTrue(TEXT("search_ops zero-limit succeeds"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
            TestTrue(TEXT("search_ops zero-limit result has results"),
                Capture.Result->TryGetArrayField(TEXT("results"), Results) && Results);
            if (Results)
            {
                TestEqual(TEXT("search_ops zero-limit returns no rows"), Results->Num(), 0);
            }

            double TotalMatches = 0.0;
            TestTrue(TEXT("search_ops zero-limit result has totalMatches"),
                Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatches));
            TestTrue(TEXT("search_ops zero-limit preserves matching total"), TotalMatches > 0.0);
        }
    }

    // The static op catalog is non-empty, so the positive cap assertion is exact. The huge
    // request's exact clamp is asserted by LimitHelper before the handler-level shape check.
    struct FDispatchProbe
    {
        const TCHAR* Label;
        double Requested;
        int32 ExpectedMaximum;
    };

    const FDispatchProbe DispatchProbes[] = {
        { TEXT("positive"), 1.0, 1 },
        { TEXT("huge"), 1.0e12, NiagaraSearch::MaxSearchLimit },
    };

    for (const FDispatchProbe& Probe : DispatchProbes)
    {
        TSharedPtr<FJsonObject> Payload = MakeNiagaraSearchLimitPayload(Probe.Requested);
        if (Probe.ExpectedMaximum == 1)
        {
            Payload->SetStringField(TEXT("query"), TEXT(""));
        }
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("niagara.graph.search_ops"), Payload, Capture);
        TestTrue(*FString::Printf(TEXT("search_ops %s limit handler found"), Probe.Label), bFound);
        TestTrue(*FString::Printf(TEXT("search_ops %s limit succeeds"), Probe.Label), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
            TestTrue(*FString::Printf(TEXT("search_ops %s limit result has results"), Probe.Label),
                Capture.Result->TryGetArrayField(TEXT("results"), Results) && Results);
            if (Results)
            {
                if (Probe.ExpectedMaximum == 1)
                {
                    TestEqual(*FString::Printf(TEXT("search_ops %s limit returns one row"), Probe.Label),
                        Results->Num(), Probe.ExpectedMaximum);
                }
                else
                {
                    TestTrue(*FString::Printf(TEXT("search_ops %s limit is capped"), Probe.Label),
                        Results->Num() <= Probe.ExpectedMaximum);
                }
            }
        }
    }

    // The module-search handler uses the same guard before walking the asset registry. Its
    // catalog is editor-provided rather than a deterministic test fixture: zero is exact, while
    // the huge case checks a successful bounded response shape and LimitHelper proves the exact
    // 500-row resolution independently of asset availability.
    struct FModuleLimitProbe
    {
        const TCHAR* Label;
        double Requested;
        bool bExpectZeroRows;
    };

    const FModuleLimitProbe ModuleProbes[] = {
        { TEXT("negative"), -1.0, false },
        { TEXT("zero"), 0.0, true },
        { TEXT("huge"), 1.0e12, false },
    };

    for (const FModuleLimitProbe& Probe : ModuleProbes)
    {
        TSharedPtr<FJsonObject> Payload = MakeNiagaraSearchLimitPayload(Probe.Requested);
        Payload->SetStringField(TEXT("usage"), TEXT("Module"));
        Payload->SetStringField(TEXT("query"), TEXT(""));

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("niagara.search_modules"), Payload, Capture);
        TestTrue(*FString::Printf(TEXT("search_modules %s-limit handler found"), Probe.Label), bFound);
        TestTrue(*FString::Printf(TEXT("search_modules %s-limit response sent"), Probe.Label),
            Capture.bWasCalled);

        if (Probe.bExpectZeroRows)
        {
            TestTrue(TEXT("search_modules zero limit succeeds"), Capture.bSuccess);
            if (Capture.bSuccess && Capture.Result.IsValid())
            {
                const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
                TestTrue(TEXT("search_modules zero-limit result has results"),
                    Capture.Result->TryGetArrayField(TEXT("results"), Results) && Results);
                if (Results)
                {
                    TestEqual(TEXT("search_modules zero limit returns no rows"), Results->Num(), 0);
                }
            }
        }
        else if (Probe.Requested < 0.0)
        {
            TestFalse(TEXT("search_modules negative limit rejected"), Capture.bSuccess);
            TestEqual(TEXT("search_modules negative-limit error code"), Capture.ErrorCode,
                FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        }
        else
        {
            TestTrue(TEXT("search_modules huge limit succeeds"), Capture.bSuccess);
            if (Capture.bSuccess && Capture.Result.IsValid())
            {
                const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
                TestTrue(TEXT("search_modules huge-limit result has results"),
                    Capture.Result->TryGetArrayField(TEXT("results"), Results) && Results);
                if (Results)
                {
                    TestTrue(TEXT("search_modules huge limit stays within maximum"),
                        Results->Num() <= NiagaraSearch::MaxSearchLimit);
                }
            }
        }
    }

    return true;
}
