// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Tests/TestUtils.h"

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
