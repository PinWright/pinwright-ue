// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

// AIModule is unconditionally linked, so the EnvQuery authoring headers are always available
// (no __has_include gate like the conditional StateTree test).
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Asset/EnvQueryDumpBuilder.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "AssetDumpTestHelpers.h"

#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    using AssetDumpTestHelpers::HasDumpFile;
    using AssetDumpTestHelpers::FindDumpFile;
    using AssetDumpTestHelpers::LoadJsonFile;

    // Authors a transient UEnvQuery with one Option carrying a SimpleGrid generator and a single
    // Distance test (FilterAndScore: Range filter 100..900 + InverseLinear scoring factor 2.0).
    // Generator/test classes are resolved by reflection so the test does not hard-link concrete
    // EQS node types. OutTestClass/OutGeneratorClass report what was actually attached (empty if
    // the engine class was unresolvable, in which case those assertions are skipped).
    UEnvQuery* NewTransientEnvQuery(FString& OutObjectPath, FString& OutGeneratorClass, FString& OutTestClass)
    {
        const FString AssetName = FString::Printf(TEXT("EQS_EnvQueryDump_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        // /Engine/Transient/ is an in-memory mount, so DumpSingleAsset's DoesPackageExist gate
        // does not short-circuit on a /Game/ package that was never saved to disk.
        const FString PackageName = FString::Printf(TEXT("/Engine/Transient/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);

        UEnvQuery* Query = NewObject<UEnvQuery>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transient);
        if (!Query)
        {
            return nullptr;
        }

        UEnvQueryOption* Option = NewObject<UEnvQueryOption>(Query);

        OutGeneratorClass.Empty();
        if (UClass* GeneratorClass = FindObject<UClass>(nullptr, TEXT("/Script/AIModule.EnvQueryGenerator_SimpleGrid")))
        {
            UEnvQueryGenerator* Generator = NewObject<UEnvQueryGenerator>(Option, GeneratorClass);
            Option->Generator = Generator;
            OutGeneratorClass = GeneratorClass->GetName();
        }

        OutTestClass.Empty();
        if (UClass* TestClass = FindObject<UClass>(nullptr, TEXT("/Script/AIModule.EnvQueryTest_Distance")))
        {
            UEnvQueryTest* Test = NewObject<UEnvQueryTest>(Option, TestClass);
            Test->TestOrder = 0;
            Test->TestPurpose = EEnvTestPurpose::FilterAndScore;
            Test->FilterType = EEnvTestFilterType::Range;
            Test->FloatValueMin.DefaultValue = 100.0f;
            Test->FloatValueMax.DefaultValue = 900.0f;
            Test->ScoringEquation = EEnvTestScoreEquation::InverseLinear;
            Test->ScoringFactor.DefaultValue = 2.0f;
            Test->TestComment = TEXT("prefer mid-range distance");
            Option->Tests.Add(Test);
            OutTestClass = TestClass->GetName();
        }

        Query->GetOptionsMutable().Add(Option);

        Query->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return Query;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvQueryDumpBuilderShapeTest,
    "PinWright.Assets.EnvQuery.DumpBuilder.Shape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvQueryDumpBuilderShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    FString GeneratorClass;
    FString TestClass;
    UEnvQuery* Query = NewTransientEnvQuery(ObjectPath, GeneratorClass, TestClass);
    TestNotNull(TEXT("Transient UEnvQuery created"), Query);
    if (!Query)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Json = EnvQueryDumpBuilder::BuildEnvQueryJson(Query);
    TestTrue(TEXT("BuildEnvQueryJson returns non-null"), Json.IsValid());
    if (!Json.IsValid())
    {
        Query->RemoveFromRoot();
        return false;
    }

    TestEqual(TEXT("assetKind"), Json->GetStringField(TEXT("assetKind")), FString(TEXT("EnvQuery")));

    const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
    TestTrue(TEXT("options array exists"), Json->TryGetArrayField(TEXT("options"), Options));
    TestTrue(TEXT("one option present"), Options && Options->Num() == 1);
    if (!Options || Options->Num() != 1)
    {
        Query->RemoveFromRoot();
        return false;
    }

    TSharedPtr<FJsonObject> OptionJson = (*Options)[0]->AsObject();
    TestTrue(TEXT("option is object"), OptionJson.IsValid());
    if (!OptionJson.IsValid())
    {
        Query->RemoveFromRoot();
        return false;
    }

    // The generator class — previously hidden behind the opaque Options object-ref — must surface.
    if (!GeneratorClass.IsEmpty())
    {
        TestEqual(TEXT("option generatorClass surfaced"),
            OptionJson->GetStringField(TEXT("generatorClass")), GeneratorClass);
    }

    const TArray<TSharedPtr<FJsonValue>>* Tests = nullptr;
    TestTrue(TEXT("option.tests array exists"), OptionJson->TryGetArrayField(TEXT("tests"), Tests));

    // When the Distance test class was resolvable, its purpose / filter bounds / scoring — none of
    // which appear in the bare Options object-ref dump — must be expanded in the sidecar.
    if (!TestClass.IsEmpty())
    {
        TestTrue(TEXT("one test present"), Tests && Tests->Num() == 1);
        if (Tests && Tests->Num() == 1)
        {
            TSharedPtr<FJsonObject> TestJson = (*Tests)[0]->AsObject();
            TestTrue(TEXT("test entry is object"), TestJson.IsValid());
            if (TestJson.IsValid())
            {
                TestEqual(TEXT("test testClass surfaced"),
                    TestJson->GetStringField(TEXT("testClass")), TestClass);
                TestEqual(TEXT("test purpose is FilterAndScore"),
                    TestJson->GetStringField(TEXT("purpose")), FString(TEXT("FilterAndScore")));
                TestEqual(TEXT("test comment surfaced"),
                    TestJson->GetStringField(TEXT("comment")), FString(TEXT("prefer mid-range distance")));

                const TSharedPtr<FJsonObject>* FilterObj = nullptr;
                TestTrue(TEXT("test.filter object exists"), TestJson->TryGetObjectField(TEXT("filter"), FilterObj));
                if (FilterObj)
                {
                    TestEqual(TEXT("filter type is Range"),
                        (*FilterObj)->GetStringField(TEXT("type")), FString(TEXT("Range")));
                    TestEqual(TEXT("filter floatMin"), (*FilterObj)->GetNumberField(TEXT("floatMin")), 100.0);
                    TestEqual(TEXT("filter floatMax"), (*FilterObj)->GetNumberField(TEXT("floatMax")), 900.0);
                }

                const TSharedPtr<FJsonObject>* ScoringObj = nullptr;
                TestTrue(TEXT("test.scoring object exists"), TestJson->TryGetObjectField(TEXT("scoring"), ScoringObj));
                if (ScoringObj)
                {
                    TestEqual(TEXT("scoring equation is InverseLinear"),
                        (*ScoringObj)->GetStringField(TEXT("equation")), FString(TEXT("InverseLinear")));
                    TestEqual(TEXT("scoring factor"), (*ScoringObj)->GetNumberField(TEXT("factor")), 2.0);
                }
            }
        }
    }

    Query->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvQueryAssetDumpWritesEnvQueryAspectFileTest,
    "PinWright.Assets.EnvQuery.AssetDump.WritesEnvQueryAspectFile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvQueryAssetDumpWritesEnvQueryAspectFileTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    FString GeneratorClass;
    FString TestClass;
    UEnvQuery* Query = NewTransientEnvQuery(ObjectPath, GeneratorClass, TestClass);
    TestNotNull(TEXT("Transient UEnvQuery created"), Query);
    if (!Query)
    {
        return false;
    }

    const FString ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("EnvQueryDumpBuilderTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for transient EnvQuery"), Result.ErrorCode.IsEmpty());
    // The whole point of the fix: the env_query.json sidecar is emitted, where before the dump
    // surfaced only the opaque Options object-ref in properties.json.
    TestTrue(TEXT("env_query.json is written"), HasDumpFile(Result.WrittenPaths, DumpFileNames::EnvQuery));

    const FString EnvQueryPath = FindDumpFile(Result.WrittenPaths, DumpFileNames::EnvQuery);
    TSharedPtr<FJsonObject> Json = LoadJsonFile(EnvQueryPath);
    TestTrue(TEXT("env_query.json parses"), Json.IsValid());
    if (Json.IsValid())
    {
        TestEqual(TEXT("env_query.json assetKind"),
            Json->GetStringField(TEXT("assetKind")), FString(TEXT("EnvQuery")));
        const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
        TestTrue(TEXT("env_query.json options array exists"), Json->TryGetArrayField(TEXT("options"), Options));
        TestTrue(TEXT("env_query.json options non-empty"), Options && Options->Num() >= 1);
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    Query->RemoveFromRoot();
    return true;
}
