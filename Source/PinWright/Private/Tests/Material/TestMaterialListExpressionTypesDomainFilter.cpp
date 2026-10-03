// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-material-domain-filter-fallback: an unparseable domainFilter used to
// fall back to NO filter and return the whole catalog as a success, so a typo read as proof that
// every row was valid for the misspelled domain.

#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Dom/JsonObject.h"
#include "Handlers/ErrorCodes.h"
#include "Tests/TestUtils.h"

namespace TestMaterialListExpressionTypesDomainFilterHelpers
{
    FTestResponseCapture List(const TCHAR* DomainFilter)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        if (DomainFilter)
        {
            Payload->SetStringField(TEXT("domainFilter"), DomainFilter);
        }
        Payload->SetBoolField(TEXT("namesOnly"), true);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("material.graph.list_expression_types"), Payload, Capture);
        return Capture;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialListExpressionTypesDomainFilterTypoTest,
    "PinWright.material.graph.list_expression_types.DomainFilterTypoIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialListExpressionTypesDomainFilterTypoTest::RunTest(const FString& Parameters)
{
    using namespace TestMaterialListExpressionTypesDomainFilterHelpers;
    const FTestResponseCapture Capture = List(TEXT("PostProces"));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("a misspelled domainFilter is not a success"), Capture.bSuccess);
    TestEqual(TEXT("refused as INVALID_ARGUMENT"), Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    TestTrue(TEXT("the refusal names the rejected value"), Capture.Message.Contains(TEXT("PostProces")));
    TestTrue(TEXT("the refusal lists the valid domains"),
        Capture.Message.Contains(TEXT("PostProcess")) && Capture.Message.Contains(TEXT("LightFunction")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialListExpressionTypesDomainFilterEchoTest,
    "PinWright.material.graph.list_expression_types.DomainFilterEchoesAppliedDomain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialListExpressionTypesDomainFilterEchoTest::RunTest(const FString& Parameters)
{
    using namespace TestMaterialListExpressionTypesDomainFilterHelpers;

    const FTestResponseCapture Unfiltered = List(nullptr);
    if (!TestTrue(TEXT("unfiltered list succeeds"), Unfiltered.bSuccess && Unfiltered.Result.IsValid())) return true;
    TestFalse(TEXT("an omitted domainFilter echoes no applied domain"),
        Unfiltered.Result->HasField(TEXT("appliedDomainFilter")));

    // Case-insensitive input, canonical echo.
    const FTestResponseCapture Filtered = List(TEXT("postprocess"));
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    TestFalse(TEXT("domainFilter is refused where IsAllowedIn does not exist"), Filtered.bSuccess);
    TestEqual(TEXT("refused as UNSUPPORTED_ENGINE_VERSION"), Filtered.ErrorCode,
        FString(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION));
#else
    if (!TestTrue(TEXT("a valid domainFilter succeeds"), Filtered.bSuccess && Filtered.Result.IsValid())) return true;
    FString Applied;
    TestTrue(TEXT("a filtered response echoes appliedDomainFilter"),
        Filtered.Result->TryGetStringField(TEXT("appliedDomainFilter"), Applied));
    TestEqual(TEXT("the echo is the canonical spelling"), Applied, FString(TEXT("PostProcess")));
    TestTrue(TEXT("the filter is applied: IsAllowedIn rejects FunctionInput/FunctionOutput in any material, so fewer rows match"),
        Filtered.Result->GetIntegerField(TEXT("totalMatches")) < Unfiltered.Result->GetIntegerField(TEXT("totalMatches")));
#endif
    return true;
}
