// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for niagara.search_modules and its NiagaraSearch:: helpers.
// Covers: ClassifySource, ScoreModuleMatch, and dispatcher registration.
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "Handlers/Niagara/NiagaraSearchHandler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSearchModulesClassifyAndScoreTest,
    "PinWright.niagara.search_modules.ClassifyAndScore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSearchModulesClassifyAndScoreTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraSearch;

    // ------------------------------------------------------------------
    // Dispatcher registration
    // ------------------------------------------------------------------

    TestTrue(TEXT("niagara.search_modules is registered"),
        IsRegistered(TEXT("niagara.search_modules")));

    // ------------------------------------------------------------------
    // ClassifySource
    // ------------------------------------------------------------------

    TestEqual(TEXT("ClassifySource: /Niagara/ path -> engine"),
        ClassifySource(TEXT("/Niagara/Modules/Forces/CurlNoiseForce")),
        FString(TEXT("engine")));

    TestEqual(TEXT("ClassifySource: /Game/ path -> project"),
        ClassifySource(TEXT("/Game/FX/MyModule")),
        FString(TEXT("project")));

    TestEqual(TEXT("ClassifySource: plugin path -> plugin"),
        ClassifySource(TEXT("/Some_Plugin/Modules/X")),
        FString(TEXT("plugin")));

    // False-positive guard: a project asset whose path contains "/Niagara/" as a
    // subdirectory must NOT be classified as "engine" — catches the Contains() bug.
    TestEqual(TEXT("ClassifySource: /Game/Niagara/Foo -> project (not engine)"),
        ClassifySource(TEXT("/Game/Niagara/Foo")),
        FString(TEXT("project")));

    // ------------------------------------------------------------------
    // ScoreModuleMatch
    // ------------------------------------------------------------------

    // Query matching name prefix → score > 0
    TestTrue(TEXT("ScoreModuleMatch: 'Curl' matches 'CurlNoise' > 0"),
        ScoreModuleMatch(TEXT("Curl"), TEXT("CurlNoise"), TEXT(""), TEXT("")) > 0);

    // Query with no match → score 0
    TestEqual(TEXT("ScoreModuleMatch: 'zzz' vs 'CurlNoise' = 0"),
        ScoreModuleMatch(TEXT("zzz"), TEXT("CurlNoise"), TEXT(""), TEXT("")),
        0);

    // Empty query → score 0 (pre-filter logic handles inclusion; ranking is skipped)
    TestEqual(TEXT("ScoreModuleMatch: empty query = 0"),
        ScoreModuleMatch(TEXT(""), TEXT("AnyName"), TEXT(""), TEXT("")),
        0);


    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSearchModulesStageAliasTest,
    "PinWright.niagara.search_modules.StageAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSearchModulesStageAliasTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("usage"), TEXT("Module"));
    Payload->SetStringField(TEXT("stage"), TEXT("ParticleUpdate"));
    Payload->SetStringField(TEXT("sourceFilter"), TEXT("engine"));
    Payload->SetStringField(TEXT("query"), TEXT("__unlikely_no_match__"));
    Payload->SetNumberField(TEXT("limit"), 1);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("niagara.search_modules"), Payload, Capture);
    TestTrue(TEXT("niagara.search_modules handler found"), bFound);
    TestTrue(TEXT("ParticleUpdate alias call responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("ParticleUpdate alias succeeds"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("search_modules error: %s %s"), *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    if (TestTrue(TEXT("ParticleUpdate alias result returned"), Capture.Result.IsValid()))
    {
        const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
        TestTrue(TEXT("ParticleUpdate alias result has results"),
            Capture.Result->TryGetArrayField(TEXT("results"), Results) && Results != nullptr);

        double TotalMatches = 0.0;
        TestTrue(TEXT("ParticleUpdate alias result has totalMatches"),
            Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatches));
    }

    TSharedPtr<FJsonObject> BadPayload = MakeShared<FJsonObject>();
    BadPayload->SetStringField(TEXT("usage"), TEXT("Module"));
    BadPayload->SetStringField(TEXT("stage"), TEXT("DefinitelyNotAStage"));
    BadPayload->SetStringField(TEXT("sourceFilter"), TEXT("engine"));
    BadPayload->SetStringField(TEXT("query"), TEXT("__unlikely_no_match__"));
    BadPayload->SetNumberField(TEXT("limit"), 1);

    FTestResponseCapture BadCapture;
    const bool bBadFound = InvokeHandlerWithCapture(TEXT("niagara.search_modules"), BadPayload, BadCapture);
    TestTrue(TEXT("niagara.search_modules bad-stage handler found"), bBadFound);
    TestTrue(TEXT("bad stage call responded"), BadCapture.bWasCalled);
    TestFalse(TEXT("bad stage fails"), BadCapture.bSuccess);
    TestEqual(TEXT("bad stage error code"), BadCapture.ErrorCode, FString(TEXT("INVALID_STAGE")));

    return true;
}
