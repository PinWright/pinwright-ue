// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Tests/TestUtils.h"
#include "Tests/AutomationSuiteMaintenance.h"

#include "AIGraph.h"
#include "AIGraphNode.h"
#include "EdGraph/EdGraphSchema.h"

#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/Contexts/EnvQueryContext_Querier.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_OnCircle.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_PathingGrid.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Distance.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Pathfinding.h"

namespace
{
    bool InvokeEqsHandler(FAutomationTestBase& Test, const FString& MethodName, const TSharedPtr<FJsonObject>& Payload, FTestResponseCapture& Capture)
    {
        const bool bFound = InvokeHandlerWithCapture(MethodName, Payload, Capture);
        Test.TestTrue(FString::Printf(TEXT("%s handler found"), *MethodName), bFound);
        if (!bFound)
        {
            return false;
        }
        if (!Capture.bSuccess)
        {
            Test.AddError(FString::Printf(TEXT("%s failed: %s %s"), *MethodName, *Capture.ErrorCode, *Capture.Message));
        }
        return Capture.bSuccess;
    }

    bool InvokeEqsHandlerExpectError(FAutomationTestBase& Test, const FString& MethodName, const TSharedPtr<FJsonObject>& Payload, FTestResponseCapture& Capture)
    {
        const bool bFound = InvokeHandlerWithCapture(MethodName, Payload, Capture);
        Test.TestTrue(FString::Printf(TEXT("%s handler found"), *MethodName), bFound);
        Test.TestTrue(FString::Printf(TEXT("%s sent a response"), *MethodName), Capture.bWasCalled);
        Test.TestFalse(FString::Printf(TEXT("%s failed"), *MethodName), Capture.bSuccess);
        Test.TestFalse(FString::Printf(TEXT("%s returned an error code"), *MethodName), Capture.ErrorCode.IsEmpty());
        return bFound && Capture.bWasCalled && !Capture.bSuccess && !Capture.ErrorCode.IsEmpty();
    }

    // Per-run unique package path. CleanupTestAsset is a no-op on UE < 5.5 (it skips the
    // ObjectTools force-delete that crashes 5.4), so fixed paths would collide with assets
    // left on disk by a prior run and eqs.create would reject them as ALREADY_EXISTS. A GUID
    // suffix makes each run self-isolating without depending on cleanup, mirroring the
    // blueprint struct tests' MakeUniqueAssetPath.
    FString MakeUniqueEqsPackagePath(const TCHAR* Stem)
    {
        return FString::Printf(
            TEXT("/Game/McpTests/EQS/%s_%s"),
            Stem,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UEnvQuery* CreateTempQuery(FAutomationTestBase& Test, const FString& PackagePath)
    {
        CleanupTestAsset(PackagePath);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(PackagePath));
        Payload->SetStringField(TEXT("path"), FPackageName::GetLongPackagePath(PackagePath));

        FTestResponseCapture Capture;
        if (!InvokeEqsHandler(Test, TEXT("eqs.create"), Payload, Capture))
        {
            return nullptr;
        }

        FString QueryPath;
        if (!Capture.Result.IsValid() || !Capture.Result->TryGetStringField(TEXT("queryPath"), QueryPath))
        {
            Test.AddError(TEXT("eqs.create did not return queryPath"));
            return nullptr;
        }

        UEnvQuery* Query = LoadObject<UEnvQuery>(nullptr, *QueryPath);
        Test.TestNotNull(TEXT("Created EQS query is loadable"), Query);
        return Query;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEqsNamespaceAuthoringPersistsOptionsTest,
    "PinWright.eqs.AuthoringPersistsOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEqsNamespaceAuthoringPersistsOptionsTest::RunTest(const FString& Parameters)
{
    const FString QueryPackagePath = MakeUniqueEqsPackagePath(TEXT("EQS_PersistOptions"));
    UEnvQuery* Query = CreateTempQuery(*this, QueryPackagePath);
    if (!Query)
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> AddGeneratorPayload = MakeShared<FJsonObject>();
    AddGeneratorPayload->SetStringField(TEXT("queryPath"), Query->GetPathName());
    AddGeneratorPayload->SetStringField(TEXT("generatorType"), TEXT("PathingGrid"));

    FTestResponseCapture AddGeneratorCapture;
    if (!InvokeEqsHandler(*this, TEXT("eqs.add_generator"), AddGeneratorPayload, AddGeneratorCapture))
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> AddTestPayload = MakeShared<FJsonObject>();
    AddTestPayload->SetStringField(TEXT("queryPath"), Query->GetPathName());
    AddTestPayload->SetNumberField(TEXT("generatorIndex"), 0);
    AddTestPayload->SetStringField(TEXT("testType"), TEXT("Pathfinding"));
    AddTestPayload->SetStringField(TEXT("purpose"), TEXT("filter_and_score"));

    FTestResponseCapture AddTestCapture;
    if (!InvokeEqsHandler(*this, TEXT("eqs.add_test"), AddTestPayload, AddTestCapture))
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    const TArray<UEnvQueryOption*>& Options = Query->GetOptions();
    TestEqual(TEXT("Generator persisted as an EQS option"), Options.Num(), 1);
    UEnvQueryOption* Option = Options.IsValidIndex(0) ? Options[0] : nullptr;
    TestNotNull(TEXT("Persisted option exists"), Option);
    TestTrue(TEXT("PathingGrid generator class resolved"), Option && Option->Generator && Option->Generator->IsA<UEnvQueryGenerator_PathingGrid>());
    TestEqual(TEXT("Test persisted under option"), Option ? Option->Tests.Num() : 0, 1);

    UEnvQueryTest* Test = Option && Option->Tests.IsValidIndex(0) ? Option->Tests[0] : nullptr;
    TestTrue(TEXT("Pathfinding test class resolved"), Test && Test->IsA<UEnvQueryTest_Pathfinding>());
    TestEqual(TEXT("Purpose persisted as FilterAndScore"), Test ? Test->TestPurpose.GetValue() : EEnvTestPurpose::Score, EEnvTestPurpose::FilterAndScore);

    CleanupTestAsset(QueryPackagePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEqsSetTestScoringInvalidClampDoesNotPartiallyMutateTest,
    "PinWright.eqs.SetTestScoringInvalidClampDoesNotPartiallyMutate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEqsSetTestScoringInvalidClampDoesNotPartiallyMutateTest::RunTest(const FString& Parameters)
{
    const FString QueryPackagePath = MakeUniqueEqsPackagePath(TEXT("EQS_InvalidClampNoPartialMutation"));
    UEnvQuery* Query = CreateTempQuery(*this, QueryPackagePath);
    if (!Query)
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> AddGeneratorPayload = MakeShared<FJsonObject>();
    AddGeneratorPayload->SetStringField(TEXT("queryPath"), Query->GetPathName());
    AddGeneratorPayload->SetStringField(TEXT("generatorType"), TEXT("OnCircle"));

    FTestResponseCapture AddGeneratorCapture;
    if (!InvokeEqsHandler(*this, TEXT("eqs.add_generator"), AddGeneratorPayload, AddGeneratorCapture))
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> AddTestPayload = MakeShared<FJsonObject>();
    AddTestPayload->SetStringField(TEXT("queryPath"), Query->GetPathName());
    AddTestPayload->SetNumberField(TEXT("generatorIndex"), 0);
    AddTestPayload->SetStringField(TEXT("testType"), TEXT("Distance"));

    FTestResponseCapture AddTestCapture;
    if (!InvokeEqsHandler(*this, TEXT("eqs.add_test"), AddTestPayload, AddTestCapture))
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    // UEnvQueryTest_Distance has no export-API macro in AIModule, so its GetPrivateStaticClass symbol is not
    // exported. Resolve via reflection for the IsA check; static_cast retains the concrete type for member access.
    UClass* DistanceTestClass = FindObject<UClass>(nullptr, TEXT("/Script/AIModule.EnvQueryTest_Distance"));
    if (!DistanceTestClass)
    {
        AddError(TEXT("Failed to resolve UEnvQueryTest_Distance via reflection"));
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    const TArray<UEnvQueryOption*>& Options = Query->GetOptions();
    UEnvQueryOption* Option = Options.IsValidIndex(0) ? Options[0] : nullptr;
    UEnvQueryTest* RawTest = Option && Option->Tests.IsValidIndex(0) ? Option->Tests[0] : nullptr;
    UEnvQueryTest_Distance* DistanceTest =
        (RawTest && RawTest->IsA(DistanceTestClass)) ? static_cast<UEnvQueryTest_Distance*>(RawTest) : nullptr;
    TestNotNull(TEXT("Distance test exists"), DistanceTest);
    if (!DistanceTest)
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    DistanceTest->ScoringEquation = EEnvTestScoreEquation::Linear;
    DistanceTest->ScoringFactor.DefaultValue = 1.0f;
    DistanceTest->ScoreClampMin.DefaultValue = 2.0f;
    DistanceTest->ScoreClampMax.DefaultValue = 3.0f;
    DistanceTest->ReferenceValue.DefaultValue = 4.0f;
    DistanceTest->bDefineReferenceValue = false;
    DistanceTest->ClampMinType = EEnvQueryTestClamping::None;
    DistanceTest->ClampMaxType = EEnvQueryTestClamping::None;

    TSharedPtr<FJsonObject> Scoring = MakeShared<FJsonObject>();
    Scoring->SetStringField(TEXT("equation"), TEXT("square"));
    Scoring->SetNumberField(TEXT("factor"), 9.0);
    Scoring->SetStringField(TEXT("clampMinType"), TEXT("specified_value"));
    Scoring->SetNumberField(TEXT("clampMin"), 10.0);
    Scoring->SetStringField(TEXT("clampMaxType"), TEXT("not_a_clamp"));
    Scoring->SetNumberField(TEXT("clampMax"), 11.0);
    Scoring->SetNumberField(TEXT("referenceValue"), 12.0);

    TSharedPtr<FJsonObject> SetScoringPayload = MakeShared<FJsonObject>();
    SetScoringPayload->SetStringField(TEXT("queryPath"), Query->GetPathName());
    SetScoringPayload->SetNumberField(TEXT("generatorIndex"), 0);
    SetScoringPayload->SetNumberField(TEXT("testIndex"), 0);
    SetScoringPayload->SetObjectField(TEXT("scoring"), Scoring);

    FTestResponseCapture SetScoringCapture;
    if (!InvokeEqsHandlerExpectError(*this, TEXT("eqs.set_test_scoring"), SetScoringPayload, SetScoringCapture))
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    TestEqual(TEXT("invalid clamp reports INVALID_ARGUMENT"), SetScoringCapture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestEqual(TEXT("scoring equation is unchanged after invalid clamp"), DistanceTest->ScoringEquation.GetValue(), EEnvTestScoreEquation::Linear);
    TestEqual(TEXT("scoring factor is unchanged after invalid clamp"), DistanceTest->ScoringFactor.DefaultValue, 1.0f);
    TestEqual(TEXT("clamp min value is unchanged after invalid clamp"), DistanceTest->ScoreClampMin.DefaultValue, 2.0f);
    TestEqual(TEXT("clamp max value is unchanged after invalid clamp"), DistanceTest->ScoreClampMax.DefaultValue, 3.0f);
    TestEqual(TEXT("reference value is unchanged after invalid clamp"), DistanceTest->ReferenceValue.DefaultValue, 4.0f);
    TestFalse(TEXT("reference value flag is unchanged after invalid clamp"), DistanceTest->bDefineReferenceValue);
    TestEqual(TEXT("clamp min type is unchanged after invalid clamp"), DistanceTest->ClampMinType.GetValue(), EEnvQueryTestClamping::None);
    TestEqual(TEXT("clamp max type is unchanged after invalid clamp"), DistanceTest->ClampMaxType.GetValue(), EEnvQueryTestClamping::None);

    CleanupTestAsset(QueryPackagePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEqsNamespaceMutatesContextFilterAndScoringTest,
    "PinWright.eqs.MutatesContextFilterAndScoring",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEqsNamespaceMutatesContextFilterAndScoringTest::RunTest(const FString& Parameters)
{
    const FString QueryPackagePath = MakeUniqueEqsPackagePath(TEXT("EQS_MutateFields"));
    UEnvQuery* Query = CreateTempQuery(*this, QueryPackagePath);
    if (!Query)
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> AddGeneratorPayload = MakeShared<FJsonObject>();
    AddGeneratorPayload->SetStringField(TEXT("queryPath"), Query->GetPathName());
    AddGeneratorPayload->SetStringField(TEXT("generatorType"), TEXT("OnCircle"));

    FTestResponseCapture AddGeneratorCapture;
    if (!InvokeEqsHandler(*this, TEXT("eqs.add_generator"), AddGeneratorPayload, AddGeneratorCapture))
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> AddTestPayload = MakeShared<FJsonObject>();
    AddTestPayload->SetStringField(TEXT("queryPath"), Query->GetPathName());
    AddTestPayload->SetNumberField(TEXT("generatorIndex"), 0);
    AddTestPayload->SetStringField(TEXT("testType"), TEXT("Distance"));
    AddTestPayload->SetStringField(TEXT("purpose"), TEXT("filter_and_score"));

    FTestResponseCapture AddTestCapture;
    if (!InvokeEqsHandler(*this, TEXT("eqs.add_test"), AddTestPayload, AddTestCapture))
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> SetContextPayload = MakeShared<FJsonObject>();
    SetContextPayload->SetStringField(TEXT("queryPath"), Query->GetPathName());
    SetContextPayload->SetNumberField(TEXT("generatorIndex"), 0);
    SetContextPayload->SetStringField(TEXT("propertyName"), TEXT("CircleCenter"));
    SetContextPayload->SetStringField(TEXT("contextClass"), TEXT("Querier"));

    FTestResponseCapture SetContextCapture;
    if (!InvokeEqsHandler(*this, TEXT("eqs.set_context_class"), SetContextPayload, SetContextCapture))
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> Filter = MakeShared<FJsonObject>();
    Filter->SetStringField(TEXT("kind"), TEXT("float_range"));
    Filter->SetNumberField(TEXT("min"), 125.0);
    Filter->SetNumberField(TEXT("max"), 650.0);

    TSharedPtr<FJsonObject> SetFilterPayload = MakeShared<FJsonObject>();
    SetFilterPayload->SetStringField(TEXT("queryPath"), Query->GetPathName());
    SetFilterPayload->SetNumberField(TEXT("generatorIndex"), 0);
    SetFilterPayload->SetNumberField(TEXT("testIndex"), 0);
    SetFilterPayload->SetObjectField(TEXT("filter"), Filter);

    FTestResponseCapture SetFilterCapture;
    if (!InvokeEqsHandler(*this, TEXT("eqs.set_test_filter"), SetFilterPayload, SetFilterCapture))
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> Scoring = MakeShared<FJsonObject>();
    Scoring->SetStringField(TEXT("equation"), TEXT("square"));
    Scoring->SetNumberField(TEXT("factor"), 2.5);
    Scoring->SetStringField(TEXT("clampMinType"), TEXT("specified_value"));
    Scoring->SetNumberField(TEXT("clampMin"), 10.0);
    Scoring->SetStringField(TEXT("clampMaxType"), TEXT("filter_threshold"));
    Scoring->SetNumberField(TEXT("referenceValue"), 300.0);

    TSharedPtr<FJsonObject> SetScoringPayload = MakeShared<FJsonObject>();
    SetScoringPayload->SetStringField(TEXT("queryPath"), Query->GetPathName());
    SetScoringPayload->SetNumberField(TEXT("generatorIndex"), 0);
    SetScoringPayload->SetNumberField(TEXT("testIndex"), 0);
    SetScoringPayload->SetObjectField(TEXT("scoring"), Scoring);

    FTestResponseCapture SetScoringCapture;
    if (!InvokeEqsHandler(*this, TEXT("eqs.set_test_scoring"), SetScoringPayload, SetScoringCapture))
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    // UEnvQueryTest_Distance has no export-API macro in AIModule, so its GetPrivateStaticClass symbol is not
    // exported. Resolve via reflection for the IsA check; static_cast retains the concrete type for member access.
    UClass* DistanceTestClass = FindObject<UClass>(nullptr, TEXT("/Script/AIModule.EnvQueryTest_Distance"));
    if (!DistanceTestClass)
    {
        AddError(TEXT("Failed to resolve UEnvQueryTest_Distance via reflection"));
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    const TArray<UEnvQueryOption*>& Options = Query->GetOptions();
    UEnvQueryOption* Option = Options.IsValidIndex(0) ? Options[0] : nullptr;
    UEnvQueryGenerator_OnCircle* Generator = Option ? Cast<UEnvQueryGenerator_OnCircle>(Option->Generator) : nullptr;
    UEnvQueryTest* RawDistanceTest = Option && Option->Tests.IsValidIndex(0) ? Option->Tests[0] : nullptr;
    UEnvQueryTest_Distance* DistanceTest =
        (RawDistanceTest && RawDistanceTest->IsA(DistanceTestClass)) ? static_cast<UEnvQueryTest_Distance*>(RawDistanceTest) : nullptr;

    TestEqual(TEXT("Context class assigned to CircleCenter"), Generator ? Generator->CircleCenter.Get() : nullptr, UEnvQueryContext_Querier::StaticClass());
    TestEqual(TEXT("Purpose remains FilterAndScore"), DistanceTest ? DistanceTest->TestPurpose.GetValue() : EEnvTestPurpose::Score, EEnvTestPurpose::FilterAndScore);
    TestEqual(TEXT("Filter type mutated"), DistanceTest ? DistanceTest->FilterType.GetValue() : EEnvTestFilterType::Match, EEnvTestFilterType::Range);
    TestEqual(TEXT("Filter min mutated"), DistanceTest ? DistanceTest->FloatValueMin.DefaultValue : 0.0f, 125.0f);
    TestEqual(TEXT("Filter max mutated"), DistanceTest ? DistanceTest->FloatValueMax.DefaultValue : 0.0f, 650.0f);
    TestEqual(TEXT("Scoring equation mutated"), DistanceTest ? DistanceTest->ScoringEquation.GetValue() : EEnvTestScoreEquation::Linear, EEnvTestScoreEquation::Square);
    TestEqual(TEXT("Scoring factor mutated"), DistanceTest ? DistanceTest->ScoringFactor.DefaultValue : 0.0f, 2.5f);
    TestEqual(TEXT("Clamp min type mutated"), DistanceTest ? DistanceTest->ClampMinType.GetValue() : EEnvQueryTestClamping::None, EEnvQueryTestClamping::SpecifiedValue);
    TestEqual(TEXT("Clamp min value mutated"), DistanceTest ? DistanceTest->ScoreClampMin.DefaultValue : 0.0f, 10.0f);
    TestEqual(TEXT("Clamp max type mutated"), DistanceTest ? DistanceTest->ClampMaxType.GetValue() : EEnvQueryTestClamping::None, EEnvQueryTestClamping::FilterThreshold);
    TestTrue(TEXT("Reference value enabled"), DistanceTest && DistanceTest->bDefineReferenceValue);
    TestEqual(TEXT("Reference value mutated"), DistanceTest ? DistanceTest->ReferenceValue.DefaultValue : 0.0f, 300.0f);

    CleanupTestAsset(QueryPackagePath);
    return true;
}

// Regression for E-eqs-builtin-token-discovery: every "Unsupported EQS <thing>" rejection
// must enumerate the accepted built-in tokens in band, so an agent that mistyped a token
// (e.g. "points_grid" instead of "simplegrid") learns the right spelling from the error
// alone — without falling back to reading EQSHandler.cpp. Reverting the "Valid:" hints
// reduces each message back to a bare echo of the bad input and fails these assertions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEqsUnsupportedTokenErrorsEnumerateValidTokensTest,
    "PinWright.eqs.UnsupportedTokenErrorsEnumerateValidTokens",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEqsUnsupportedTokenErrorsEnumerateValidTokensTest::RunTest(const FString& Parameters)
{
    const FString QueryPackagePath = MakeUniqueEqsPackagePath(TEXT("EQS_TokenHintErrors"));
    UEnvQuery* Query = CreateTempQuery(*this, QueryPackagePath);
    if (!Query)
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    // Asserts the rejection message echoes the bad token AND lists the correct one. Both
    // checks matter: the correct-token presence guards against a hint that drifts from the
    // map (e.g. losing "simplegrid"), and is the substring a confused agent actually needs.
    //
    // CorrectToken alone is NOT sufficient for every sub-case: where the correct token is a
    // substring of the bad one ("linetrace" contains "trace", "querier_actor" contains
    // "querier"), a bare echo of the bad input satisfies the assertion and the reverted hint
    // passes. Two extra probes close that hole:
    //   HintPrefix         - the literal separator the hint clause introduces, which a bare
    //                        echo cannot contain ("Valid built-in names:" for the three
    //                        class-map-backed lists, "Valid:" for the two literal ladders).
    //   UnrelatedValidToken- another accepted token that is NOT a substring of BadToken, so
    //                        the assertion needs the enumerated list to actually be present.
    const auto ExpectHintedRejection =
        [this](const TCHAR* Method, const TSharedPtr<FJsonObject>& Payload, const TCHAR* BadToken, const TCHAR* CorrectToken,
            const TCHAR* HintPrefix, const TCHAR* UnrelatedValidToken)
        {
            FTestResponseCapture Capture;
            if (!InvokeEqsHandlerExpectError(*this, Method, Payload, Capture))
            {
                return;
            }
            TestEqual(FString::Printf(TEXT("%s bad token reports INVALID_ARGUMENT"), Method),
                Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
            TestTrue(FString::Printf(TEXT("%s error echoes the bad token '%s' (msg: %s)"), Method, BadToken, *Capture.Message),
                Capture.Message.Contains(BadToken));
            TestTrue(FString::Printf(TEXT("%s error lists valid tokens (expected '%s' in msg: %s)"), Method, CorrectToken, *Capture.Message),
                Capture.Message.Contains(CorrectToken));
            TestTrue(FString::Printf(TEXT("%s error carries the hint separator '%s' (msg: %s)"), Method, HintPrefix, *Capture.Message),
                Capture.Message.Contains(HintPrefix));
            TestTrue(FString::Printf(TEXT("%s error enumerates unrelated valid token '%s' (msg: %s)"), Method, UnrelatedValidToken, *Capture.Message),
                Capture.Message.Contains(UnrelatedValidToken));
        };

    // First add a valid generator + test so the filter/scoring rejections reach the
    // token-resolution branch rather than failing earlier on a missing test index.
    {
        TSharedPtr<FJsonObject> AddGeneratorPayload = MakeShared<FJsonObject>();
        AddGeneratorPayload->SetStringField(TEXT("queryPath"), Query->GetPathName());
        AddGeneratorPayload->SetStringField(TEXT("generatorType"), TEXT("simplegrid"));
        FTestResponseCapture AddGeneratorCapture;
        if (!InvokeEqsHandler(*this, TEXT("eqs.add_generator"), AddGeneratorPayload, AddGeneratorCapture))
        {
            CleanupTestAsset(QueryPackagePath);
            return false;
        }

        TSharedPtr<FJsonObject> AddTestPayload = MakeShared<FJsonObject>();
        AddTestPayload->SetStringField(TEXT("queryPath"), Query->GetPathName());
        AddTestPayload->SetNumberField(TEXT("generatorIndex"), 0);
        AddTestPayload->SetStringField(TEXT("testType"), TEXT("distance"));
        FTestResponseCapture AddTestCapture;
        if (!InvokeEqsHandler(*this, TEXT("eqs.add_test"), AddTestPayload, AddTestCapture))
        {
            CleanupTestAsset(QueryPackagePath);
            return false;
        }
    }

    // generatorType: natural-language "points_grid" -> correct is "simplegrid".
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("queryPath"), Query->GetPathName());
        Payload->SetStringField(TEXT("generatorType"), TEXT("points_grid"));
        ExpectHintedRejection(TEXT("eqs.add_generator"), Payload, TEXT("points_grid"), TEXT("simplegrid"),
            TEXT("Valid built-in names:"), TEXT("actorsofclass"));
    }

    // testType: "linetrace" -> correct is "trace". "trace" is a substring of "linetrace", so
    // the discriminating probes here are the hint separator and "pathfindingbatch".
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("queryPath"), Query->GetPathName());
        Payload->SetNumberField(TEXT("generatorIndex"), 0);
        Payload->SetStringField(TEXT("testType"), TEXT("linetrace"));
        ExpectHintedRejection(TEXT("eqs.add_test"), Payload, TEXT("linetrace"), TEXT("trace"),
            TEXT("Valid built-in names:"), TEXT("pathfindingbatch"));
    }

    // contextClass: "querier_actor" -> correct is "querier". "querier" is a substring of
    // "querier_actor", so the separator and "navigationdata" are what actually discriminate.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("queryPath"), Query->GetPathName());
        Payload->SetNumberField(TEXT("generatorIndex"), 0);
        Payload->SetStringField(TEXT("propertyName"), TEXT("GenerateAround"));
        Payload->SetStringField(TEXT("contextClass"), TEXT("querier_actor"));
        ExpectHintedRejection(TEXT("eqs.set_context_class"), Payload, TEXT("querier_actor"), TEXT("querier"),
            TEXT("Valid built-in names:"), TEXT("navigationdata"));
    }

    // filter.kind: "inside" -> the valid set includes "float_range".
    {
        TSharedPtr<FJsonObject> Filter = MakeShared<FJsonObject>();
        Filter->SetStringField(TEXT("kind"), TEXT("inside"));
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("queryPath"), Query->GetPathName());
        Payload->SetNumberField(TEXT("generatorIndex"), 0);
        Payload->SetNumberField(TEXT("testIndex"), 0);
        Payload->SetObjectField(TEXT("filter"), Filter);
        ExpectHintedRejection(TEXT("eqs.set_test_filter"), Payload, TEXT("inside"), TEXT("float_range"),
            TEXT("Valid:"), TEXT("minimum"));
    }

    // scoring.equation: "inverse" -> correct is "inverse_linear".
    {
        TSharedPtr<FJsonObject> Scoring = MakeShared<FJsonObject>();
        Scoring->SetStringField(TEXT("equation"), TEXT("inverse"));
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("queryPath"), Query->GetPathName());
        Payload->SetNumberField(TEXT("generatorIndex"), 0);
        Payload->SetNumberField(TEXT("testIndex"), 0);
        Payload->SetObjectField(TEXT("scoring"), Scoring);
        ExpectHintedRejection(TEXT("eqs.set_test_scoring"), Payload, TEXT("inverse"), TEXT("inverse_linear"),
            TEXT("Valid:"), TEXT("square_root"));
    }

    CleanupTestAsset(QueryPackagePath);
    return true;
}

namespace TestEqsTestEditingHelpers
{
    FString MakeScratchEqsPackagePath(const TCHAR* Stem)
    {
        return FString::Printf(TEXT("%s/EQS/%s_%s"),
            PinWrightSuiteMaintenance::ScratchRootPackagePath(),
            Stem,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Creates the query, one OnCircle generator, and one test per entry of TestSpecs
    // ({testType, purpose}). Returns the option, or nullptr after reporting the failure.
    UEnvQueryOption* BuildQuery(FAutomationTestBase& Test, const FString& PackagePath,
        const TArray<TPair<FString, FString>>& TestSpecs, UEnvQuery*& OutQuery)
    {
        OutQuery = CreateTempQuery(Test, PackagePath);
        if (!OutQuery)
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> AddGeneratorPayload = MakeShared<FJsonObject>();
        AddGeneratorPayload->SetStringField(TEXT("queryPath"), OutQuery->GetPathName());
        AddGeneratorPayload->SetStringField(TEXT("generatorType"), TEXT("OnCircle"));
        FTestResponseCapture AddGeneratorCapture;
        if (!InvokeEqsHandler(Test, TEXT("eqs.add_generator"), AddGeneratorPayload, AddGeneratorCapture))
        {
            return nullptr;
        }

        for (const TPair<FString, FString>& Spec : TestSpecs)
        {
            TSharedPtr<FJsonObject> AddTestPayload = MakeShared<FJsonObject>();
            AddTestPayload->SetStringField(TEXT("queryPath"), OutQuery->GetPathName());
            AddTestPayload->SetNumberField(TEXT("generatorIndex"), 0);
            AddTestPayload->SetStringField(TEXT("testType"), Spec.Key);
            AddTestPayload->SetStringField(TEXT("purpose"), Spec.Value);
            FTestResponseCapture AddTestCapture;
            if (!InvokeEqsHandler(Test, TEXT("eqs.add_test"), AddTestPayload, AddTestCapture))
            {
                return nullptr;
            }
        }

        UEnvQueryOption* Option = OutQuery->GetOptions().IsValidIndex(0) ? OutQuery->GetOptions()[0] : nullptr;
        if (!Option || Option->Tests.Num() != TestSpecs.Num())
        {
            Test.AddError(FString::Printf(TEXT("Fixture precondition: expected 1 option with %d tests"), TestSpecs.Num()));
            return nullptr;
        }
        return Option;
    }

    // Builds the editor graph exactly as FEnvironmentQueryEditor does on first open. The graph
    // class is not exported, so create it by reflection and drive it through UAIGraph's virtuals.
    UAIGraph* BuildEditorGraph(FAutomationTestBase& Test, UEnvQuery* Query)
    {
        UClass* GraphClass = FindObject<UClass>(nullptr, TEXT("/Script/EnvironmentQueryEditor.EnvironmentQueryGraph"));
        if (!GraphClass)
        {
            Test.AddError(TEXT("Fixture precondition: EnvironmentQueryGraph class not found"));
            return nullptr;
        }
        UAIGraph* Graph = NewObject<UAIGraph>(Query, GraphClass, NAME_None, RF_Transactional);
        Query->EdGraph = Graph;
        Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
        Graph->OnCreated();
        Graph->Initialize();
        return Graph;
    }
}

// F-eqs-set-test-purpose-and-remove-test #1: a test created as a filter can be turned into a
// score without rebuilding the query, and an unknown purpose is refused rather than silently
// falling back to score.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEqsSetTestPurposeConvertsFilterToScoreTest,
    "PinWright.eqs.SetTestPurposeConvertsFilterToScore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEqsSetTestPurposeConvertsFilterToScoreTest::RunTest(const FString& Parameters)
{
    const FString QueryPackagePath = TestEqsTestEditingHelpers::MakeScratchEqsPackagePath(TEXT("EQS_SetPurpose"));
    UEnvQuery* Query = nullptr;
    UEnvQueryOption* Option = TestEqsTestEditingHelpers::BuildQuery(*this, QueryPackagePath,
        { {TEXT("trace"), TEXT("filter")}, {TEXT("trace"), TEXT("filter")} }, Query);
    if (!Option)
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }
    UEnvQueryTest* Target = Option->Tests[1];
    UEnvQueryTest* Sibling = Option->Tests[0];
    TestEqual(TEXT("Fixture precondition: target starts as Filter"), Target->TestPurpose.GetValue(), EEnvTestPurpose::Filter);

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("queryPath"), Query->GetPathName());
        Payload->SetNumberField(TEXT("generatorIndex"), 0);
        Payload->SetNumberField(TEXT("testIndex"), 1);
        Payload->SetStringField(TEXT("purpose"), TEXT("score"));
        FTestResponseCapture Capture;
        if (InvokeEqsHandler(*this, TEXT("eqs.set_test_purpose"), Payload, Capture) && Capture.Result.IsValid())
        {
            TestEqual(TEXT("previousPurpose reports filter"), Capture.Result->GetStringField(TEXT("previousPurpose")), FString(TEXT("filter")));
            TestEqual(TEXT("purpose reports score"), Capture.Result->GetStringField(TEXT("purpose")), FString(TEXT("score")));
            TestTrue(TEXT("changed is true"), Capture.Result->GetBoolField(TEXT("changed")));
        }
    }
    TestEqual(TEXT("Target test is now Score"), Target->TestPurpose.GetValue(), EEnvTestPurpose::Score);
    TestEqual(TEXT("Sibling test is untouched"), Sibling->TestPurpose.GetValue(), EEnvTestPurpose::Filter);

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("queryPath"), Query->GetPathName());
        Payload->SetNumberField(TEXT("testIndex"), 0);
        Payload->SetStringField(TEXT("purpose"), TEXT("scoring"));
        FTestResponseCapture Capture;
        if (InvokeEqsHandlerExpectError(*this, TEXT("eqs.set_test_purpose"), Payload, Capture))
        {
            TestEqual(TEXT("Unknown purpose is INVALID_ARGUMENT"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        }
    }
    TestEqual(TEXT("Rejected purpose leaves the test as Filter"), Sibling->TestPurpose.GetValue(), EEnvTestPurpose::Filter);

    CleanupTestAsset(QueryPackagePath);
    return true;
}

// F-eqs-set-test-purpose-and-remove-test #2: removing a test shifts later indices down, and the
// removal survives the EQS editor's graph rebuild. UEnvironmentQueryGraph::UpdateAsset rebuilds
// Option->Tests from the graph's subnodes, so a removal that only touched Option->Tests comes
// back the next time the query is edited in the EQS editor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEqsRemoveTestSurvivesEditorGraphRebuildTest,
    "PinWright.eqs.RemoveTestSurvivesEditorGraphRebuild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEqsRemoveTestSurvivesEditorGraphRebuildTest::RunTest(const FString& Parameters)
{
    const FString QueryPackagePath = TestEqsTestEditingHelpers::MakeScratchEqsPackagePath(TEXT("EQS_RemoveTest"));
    UEnvQuery* Query = nullptr;
    UEnvQueryOption* Option = TestEqsTestEditingHelpers::BuildQuery(*this, QueryPackagePath,
        { {TEXT("distance"), TEXT("score")}, {TEXT("trace"), TEXT("filter")}, {TEXT("dot"), TEXT("score")} }, Query);
    if (!Option)
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }
    UEnvQueryTest* First = Option->Tests[0];
    UEnvQueryTest* Removed = Option->Tests[1];
    UEnvQueryTest* Last = Option->Tests[2];

    UAIGraph* Graph = TestEqsTestEditingHelpers::BuildEditorGraph(*this, Query);
    if (!Graph)
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    int32 SubNodesBefore = 0;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UAIGraphNode* AINode = Cast<UAIGraphNode>(Node))
        {
            SubNodesBefore += AINode->SubNodes.Num();
        }
    }
    if (!TestEqual(TEXT("Fixture precondition: graph mirrors the three tests"), SubNodesBefore, 3))
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("queryPath"), Query->GetPathName());
    Payload->SetNumberField(TEXT("generatorIndex"), 0);
    Payload->SetNumberField(TEXT("testIndex"), 1);
    FTestResponseCapture Capture;
    if (InvokeEqsHandler(*this, TEXT("eqs.remove_test"), Payload, Capture) && Capture.Result.IsValid())
    {
        TestEqual(TEXT("testCount reports 2"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("testCount"))), 2);
        TestEqual(TEXT("editorGraphNodesRemoved reports 1"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("editorGraphNodesRemoved"))), 1);
    }

    const auto ExpectRemaining = [&](const TCHAR* When)
    {
        UEnvQueryOption* Current = Query->GetOptions().IsValidIndex(0) ? Query->GetOptions()[0] : nullptr;
        if (!TestNotNull(FString::Printf(TEXT("%s: option exists"), When), Current))
        {
            return;
        }
        TestEqual(FString::Printf(TEXT("%s: two tests remain"), When), Current->Tests.Num(), 2);
        TestFalse(FString::Printf(TEXT("%s: removed test is gone"), When), Current->Tests.Contains(Removed));
        if (Current->Tests.Num() == 2)
        {
            TestTrue(FString::Printf(TEXT("%s: index 0 is still the first test"), When), Current->Tests[0] == First);
            TestTrue(FString::Printf(TEXT("%s: the last test shifted to index 1"), When), Current->Tests[1] == Last);
            TestEqual(FString::Printf(TEXT("%s: shifted test's TestOrder follows its index"), When), Last->TestOrder, 1);
        }
    };
    ExpectRemaining(TEXT("after remove_test"));

    Graph->UpdateAsset();
    ExpectRemaining(TEXT("after the EQS editor graph rebuild"));

    Query->EdGraph = nullptr;
    CleanupTestAsset(QueryPackagePath);
    return true;
}

// F-eqs-set-test-purpose-and-remove-test #3: eqs.create overwrite:true clears an existing query in
// place, keeping the same UEnvQuery object so referencers stay valid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEqsCreateOverwriteClearsInPlaceTest,
    "PinWright.eqs.CreateOverwriteClearsInPlace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEqsCreateOverwriteClearsInPlaceTest::RunTest(const FString& Parameters)
{
    const FString QueryPackagePath = TestEqsTestEditingHelpers::MakeScratchEqsPackagePath(TEXT("EQS_Overwrite"));
    UEnvQuery* Query = nullptr;
    if (!TestEqsTestEditingHelpers::BuildQuery(*this, QueryPackagePath, { {TEXT("trace"), TEXT("filter")} }, Query))
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(QueryPackagePath));
    Payload->SetStringField(TEXT("path"), FPackageName::GetLongPackagePath(QueryPackagePath));

    {
        FTestResponseCapture Capture;
        if (InvokeEqsHandlerExpectError(*this, TEXT("eqs.create"), Payload, Capture))
        {
            TestEqual(TEXT("Without overwrite an existing query is ALREADY_EXISTS"), Capture.ErrorCode, FString(TEXT("ALREADY_EXISTS")));
        }
        TestEqual(TEXT("Refused create leaves the option in place"), Query->GetOptions().Num(), 1);
    }

    // A query that was opened in the EQS editor carries a graph whose UpdateAsset rebuilds Options
    // from its nodes, so the overwrite must drop it or the next open restores the cleared options.
    if (!TestEqsTestEditingHelpers::BuildEditorGraph(*this, Query))
    {
        CleanupTestAsset(QueryPackagePath);
        return false;
    }

    // save defaults to true; the overwrite's save must be measured, not echoed from the request.
    Payload->SetBoolField(TEXT("overwrite"), true);
    FTestResponseCapture Capture;
    if (InvokeEqsHandler(*this, TEXT("eqs.create"), Payload, Capture) && Capture.Result.IsValid())
    {
        TestTrue(TEXT("overwritten is true"), Capture.Result->GetBoolField(TEXT("overwritten")));
        TestEqual(TEXT("previousOptionCount is 1"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("previousOptionCount"))), 1);
        TestEqual(TEXT("Same query object path"), Capture.Result->GetStringField(TEXT("queryPath")), Query->GetPathName());
        TestTrue(TEXT("editorGraphDropped is true"), Capture.Result->GetBoolField(TEXT("editorGraphDropped")));
        TestEqual(TEXT("closedEditorCount is 0 (no editor open)"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("closedEditorCount"))), 0);
        FString SaveState;
        TestTrue(TEXT("response carries saveState"), Capture.Result->TryGetStringField(TEXT("saveState"), SaveState));
        TestEqual(TEXT("overwrite reports a disk write"), SaveState, FString(TEXT("written")));
        TestTrue(TEXT("saved is true"), Capture.Result->GetBoolField(TEXT("saved")));
    }
    TestTrue(TEXT("The existing UEnvQuery object survived"), LoadObject<UEnvQuery>(nullptr, *Query->GetPathName()) == Query);
    TestEqual(TEXT("Options were cleared in place"), Query->GetOptions().Num(), 0);
    TestNull(TEXT("Overwrite dropped the editor graph"), Query->EdGraph.Get());
    TestFalse(TEXT("Saved overwrite leaves the package clean"), Query->GetPackage()->IsDirty());
    Query->EdGraph = nullptr;

    CleanupTestAsset(QueryPackagePath);
    return true;
}

