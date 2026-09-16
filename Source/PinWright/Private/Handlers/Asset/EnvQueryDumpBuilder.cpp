// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/EnvQueryDumpBuilder.h"

#include "Dom/JsonValue.h"
#include "UObject/Class.h"

// AIModule is unconditionally linked (PinWright.Build.cs PublicDependencyModuleNames), so the
// EnvQuery authoring headers are always available — no __has_include gate, unlike the conditional
// StateTree dump builder.
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTypes.h"

#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonBuilders.h"
#include "Utils/JsonSidecarRegistry.h"

namespace
{
    using JsonBuilders::EnumMemberName;

    TSharedPtr<FJsonObject> BuildTestJson(const UEnvQueryTest* Test)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Test)
        {
            return Obj;
        }

        Obj->SetStringField(TEXT("testClass"), Test->GetClass()->GetName());
        Obj->SetNumberField(TEXT("testOrder"), Test->TestOrder);
        Obj->SetStringField(TEXT("purpose"),
            EnumMemberName(StaticEnum<EEnvTestPurpose::Type>(), static_cast<int64>(Test->TestPurpose.GetValue())));
        if (!Test->TestComment.IsEmpty())
        {
            Obj->SetStringField(TEXT("comment"), Test->TestComment);
        }

        const bool bScoring = Test->TestPurpose != EEnvTestPurpose::Filter;
        const bool bFiltering = Test->TestPurpose != EEnvTestPurpose::Score;

        if (bFiltering)
        {
            TSharedRef<FJsonObject> Filter = MakeShared<FJsonObject>();
            const EEnvTestFilterType::Type FilterType = Test->FilterType.GetValue();
            Filter->SetStringField(TEXT("type"),
                EnumMemberName(StaticEnum<EEnvTestFilterType::Type>(), static_cast<int64>(FilterType)));
            if (FilterType == EEnvTestFilterType::Match)
            {
                Filter->SetBoolField(TEXT("boolValue"), Test->BoolValue.DefaultValue);
            }
            else
            {
                if (FilterType == EEnvTestFilterType::Minimum || FilterType == EEnvTestFilterType::Range)
                {
                    Filter->SetNumberField(TEXT("floatMin"), Test->FloatValueMin.DefaultValue);
                }
                if (FilterType == EEnvTestFilterType::Maximum || FilterType == EEnvTestFilterType::Range)
                {
                    Filter->SetNumberField(TEXT("floatMax"), Test->FloatValueMax.DefaultValue);
                }
            }
            Obj->SetObjectField(TEXT("filter"), Filter);
        }

        if (bScoring)
        {
            TSharedRef<FJsonObject> Scoring = MakeShared<FJsonObject>();
            Scoring->SetStringField(TEXT("equation"),
                EnumMemberName(StaticEnum<EEnvTestScoreEquation::Type>(), static_cast<int64>(Test->ScoringEquation.GetValue())));
            Scoring->SetNumberField(TEXT("factor"), Test->ScoringFactor.DefaultValue);
            Obj->SetObjectField(TEXT("scoring"), Scoring);
        }

        return Obj;
    }

    TSharedPtr<FJsonObject> BuildOptionJson(const UEnvQueryOption* Option)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Option)
        {
            return Obj;
        }

        if (const UEnvQueryGenerator* Generator = Option->Generator)
        {
            Obj->SetStringField(TEXT("generatorClass"), Generator->GetClass()->GetName());
        }
        else
        {
            Obj->SetField(TEXT("generatorClass"), MakeShared<FJsonValueNull>());
        }

        TArray<TSharedPtr<FJsonValue>> Tests;
        Tests.Reserve(Option->Tests.Num());
        for (const UEnvQueryTest* Test : Option->Tests)
        {
            Tests.Add(MakeShared<FJsonValueObject>(BuildTestJson(Test)));
        }
        Obj->SetArrayField(TEXT("tests"), Tests);

        return Obj;
    }
}

namespace EnvQueryDumpBuilder
{
    TSharedPtr<FJsonObject> BuildEnvQueryJson(const UEnvQuery* Query)
    {
        if (!Query)
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("assetKind"), TEXT("EnvQuery"));
        Root->SetStringField(TEXT("path"), Query->GetPathName());
        Root->SetStringField(TEXT("queryName"), Query->GetQueryName().ToString());

        const TArray<UEnvQueryOption*>& Options = Query->GetOptions();
        TArray<TSharedPtr<FJsonValue>> OptionValues;
        OptionValues.Reserve(Options.Num());
        for (const UEnvQueryOption* Option : Options)
        {
            OptionValues.Add(MakeShared<FJsonValueObject>(BuildOptionJson(Option)));
        }
        Root->SetArrayField(TEXT("options"), OptionValues);

        return Root;
    }
}

namespace
{
    UClass* GetEnvQuerySidecarClass()
    {
        return UEnvQuery::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildEnvQuerySidecar(UObject* Asset)
    {
        return EnvQueryDumpBuilder::BuildEnvQueryJson(Cast<UEnvQuery>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("env_query"), DumpFileNames::EnvQuery,
    &GetEnvQuerySidecarClass, &BuildEnvQuerySidecar,
    nullptr, nullptr, TEXT("EnvQuery has no options."), 100);
