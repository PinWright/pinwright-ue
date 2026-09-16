// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/AI/EQSHandler.h"

#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "PinWrightHelpers.h"
#include "Utils/ClassUtils.h"
#include "Utils/JsonUtils.h"
#include "Utils/PathUtils.h"
#include "Utils/StringUtils.h"
#include "Utils/AssetUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryContext.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

namespace PinWrightEQS
{
    namespace
    {
        using PinWright::NormalizeToken;

        bool TryGetJsonNumberField(const TSharedPtr<FJsonObject>& Payload, const TCHAR* FieldName, double& OutValue)
        {
            return Payload.IsValid() && Payload->TryGetNumberField(FieldName, OutValue);
        }

        bool BuildAssetPaths(const FString& Path, const FString& Name, FString& OutPackagePath, FString& OutObjectPath, FString& OutError)
        {
            if (Name.IsEmpty())
            {
                OutError = TEXT("Missing name parameter");
                return false;
            }

            const FString RequestedPath = Path.IsEmpty() ? TEXT("/Game/AI/EQS") : Path;
            FString SanitizedPath = SanitizeProjectRelativePath(RequestedPath);
            while (SanitizedPath.EndsWith(TEXT("/")))
            {
                SanitizedPath.LeftChopInline(1);
            }
            if (SanitizedPath.IsEmpty())
            {
                OutError = FString::Printf(TEXT("Invalid path: %s"), *Path);
                return false;
            }

            OutPackagePath = SanitizedPath / Name;
            FText PackageNameError;
            if (!FPackageName::IsValidLongPackageName(OutPackagePath, false, &PackageNameError))
            {
                OutError = PackageNameError.ToString();
                return false;
            }

            OutObjectPath = FString::Printf(TEXT("%s.%s"), *OutPackagePath, *Name);
            return true;
        }

        UClass* ResolveClassByPathOrName(const FString& TypeOrPath, UClass* RequiredBase, const TMap<FString, FString>& BuiltIns)
        {
            const FString Key = NormalizeToken(TypeOrPath);
            const FString* BuiltInPath = BuiltIns.Find(Key);
            const FString ClassSpec = BuiltInPath ? *BuiltInPath : TypeOrPath;
            if (ClassSpec.IsEmpty())
            {
                return nullptr;
            }

            UClass* ResolvedClass = ResolveUClass(ClassSpec);
            if (!ResolvedClass)
            {
                ResolvedClass = ResolveClassByName(ClassSpec);
            }

            return ResolvedClass && ResolvedClass->IsChildOf(RequiredBase) ? ResolvedClass : nullptr;
        }

        const TMap<FString, FString>& GeneratorClassMap()
        {
            static const TMap<FString, FString> Map = {
                {TEXT("actorsofclass"), TEXT("/Script/AIModule.EnvQueryGenerator_ActorsOfClass")},
                {TEXT("actorsofclass_default"), TEXT("/Script/AIModule.EnvQueryGenerator_ActorsOfClass")},
                {TEXT("oncircle"), TEXT("/Script/AIModule.EnvQueryGenerator_OnCircle")},
                {TEXT("simplegrid"), TEXT("/Script/AIModule.EnvQueryGenerator_SimpleGrid")},
                {TEXT("pathinggrid"), TEXT("/Script/AIModule.EnvQueryGenerator_PathingGrid")},
                {TEXT("composite"), TEXT("/Script/AIModule.EnvQueryGenerator_Composite")},
                {TEXT("donut"), TEXT("/Script/AIModule.EnvQueryGenerator_Donut")},
                {TEXT("blueprintbase"), TEXT("/Script/AIModule.EnvQueryGenerator_BlueprintBase")}
            };
            return Map;
        }

        const TMap<FString, FString>& TestClassMap()
        {
            static const TMap<FString, FString> Map = {
                {TEXT("distance"), TEXT("/Script/AIModule.EnvQueryTest_Distance")},
                {TEXT("trace"), TEXT("/Script/AIModule.EnvQueryTest_Trace")},
                {TEXT("pathfinding"), TEXT("/Script/AIModule.EnvQueryTest_Pathfinding")},
                {TEXT("pathfindingbatch"), TEXT("/Script/AIModule.EnvQueryTest_PathfindingBatch")},
                {TEXT("dot"), TEXT("/Script/AIModule.EnvQueryTest_Dot")},
                {TEXT("gameplaytags"), TEXT("/Script/AIModule.EnvQueryTest_GameplayTags")},
                {TEXT("overlap"), TEXT("/Script/AIModule.EnvQueryTest_Overlap")},
                {TEXT("random"), TEXT("/Script/AIModule.EnvQueryTest_Random")},
                {TEXT("project"), TEXT("/Script/AIModule.EnvQueryTest_Project")},
                {TEXT("volume"), TEXT("/Script/AIModule.EnvQueryTest_Volume")}
            };
            return Map;
        }

        const TMap<FString, FString>& ContextClassMap()
        {
            static const TMap<FString, FString> Map = {
                {TEXT("querier"), TEXT("/Script/AIModule.EnvQueryContext_Querier")},
                {TEXT("item"), TEXT("/Script/AIModule.EnvQueryContext_Item")},
                {TEXT("navigationdata"), TEXT("/Script/AIModule.EnvQueryContext_NavigationData")},
                {TEXT("blueprintbase"), TEXT("/Script/AIModule.EnvQueryContext_BlueprintBase")}
            };
            return Map;
        }

        // Joins the keys of a built-in class map into a "a, b, c" hint string, skipping any
        // alias keys in HideKeys (alternate spellings that resolve to the same class and would
        // be noise in a "Valid: ..." list). Deriving the hint from the same map the resolver
        // (ResolveClassByPathOrName) walks keeps the map the single source of truth, so a new
        // built-in is discoverable the moment it is added — mirroring the established pattern in
        // Material/MainInputBindings.h GetValidMainInputNames(), which joins the same table its
        // resolver iterates rather than maintaining a second parallel literal.
        FString JoinBuiltInTokens(const TMap<FString, FString>& Map, TConstArrayView<FString> HideKeys = {})
        {
            TArray<FString> Keys;
            Map.GenerateKeyArray(Keys);
            Keys.RemoveAll([&HideKeys](const FString& Key) { return HideKeys.Contains(Key); });
            Keys.Sort();
            return FString::Join(Keys, TEXT(", "));
        }

        // Human-readable lists of the accepted built-in tokens for each resolution kind, returned
        // in the "Unsupported EQS <thing>: <X>. Valid: ..." rejection hints so an agent that
        // mistyped a token learns the right spelling in band, matching the "Valid: a, b, c"
        // error-hint convention used elsewhere (e.g. LevelHandler "Unknown lighting quality" and
        // MaterialAuthoringHandler "Unknown input"). The three class-map-backed lists derive from
        // their maps so the hint can never drift from what the resolver accepts; only the alias
        // "actorsofclass_default" is hidden (it resolves to the same class as "actorsofclass").
        const FString& GeneratorTokenList()
        {
            static const FString HideAlias = TEXT("actorsofclass_default");
            static const FString List = JoinBuiltInTokens(GeneratorClassMap(), MakeArrayView(&HideAlias, 1));
            return List;
        }

        const FString& TestTokenList()
        {
            static const FString List = JoinBuiltInTokens(TestClassMap());
            return List;
        }

        const FString& ContextTokenList()
        {
            static const FString List = JoinBuiltInTokens(ContextClassMap());
            return List;
        }

        // Filter-kind and scoring-equation tokens have no enumerable map — ParseFilterType /
        // ParseScoringEquation are if/else ladders — so these stay literals. Keep them in lockstep
        // with those parsers: the displayed canonical token must be one the parser accepts.
        const FString& FilterKindTokenList()
        {
            static const FString List = TEXT("minimum (min), maximum (max), range (float_range), match (bool / boolean)");
            return List;
        }

        const FString& ScoringEquationTokenList()
        {
            static const FString List = TEXT("linear, inverse_linear, square, square_root, constant");
            return List;
        }

        UEnvQuery* LoadQueryOrSendError(FHandlerContext& Ctx, const FString& QueryPath)
        {
            UEnvQuery* Query = LoadObject<UEnvQuery>(nullptr, *QueryPath);
            if (!Query)
            {
                Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("EQS Query not found: %s"), *QueryPath));
            }
            return Query;
        }

        UEnvQueryOption* GetOptionOrSendError(FHandlerContext& Ctx, UEnvQuery* Query, int32 GeneratorIndex)
        {
            TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
            if (!Options.IsValidIndex(GeneratorIndex) || !Options[GeneratorIndex])
            {
                Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Invalid generatorIndex: %d"), GeneratorIndex));
                return nullptr;
            }
            return Options[GeneratorIndex];
        }

        UEnvQueryTest* GetTestOrSendError(FHandlerContext& Ctx, UEnvQueryOption* Option, int32 TestIndex)
        {
            if (!Option->Tests.IsValidIndex(TestIndex) || !Option->Tests[TestIndex])
            {
                Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Invalid testIndex: %d"), TestIndex));
                return nullptr;
            }
            return Option->Tests[TestIndex];
        }

        EEnvTestPurpose::Type ParsePurpose(const FString& Purpose)
        {
            const FString Value = NormalizeToken(Purpose);
            if (Value == TEXT("filter"))
            {
                return EEnvTestPurpose::Filter;
            }
            if (Value == TEXT("filter_and_score") || Value == TEXT("filterandscore"))
            {
                return EEnvTestPurpose::FilterAndScore;
            }
            return EEnvTestPurpose::Score;
        }

        bool ParseFilterType(const FString& Type, EEnvTestFilterType::Type& OutType)
        {
            const FString Value = NormalizeToken(Type);
            if (Value == TEXT("minimum") || Value == TEXT("min"))
            {
                OutType = EEnvTestFilterType::Minimum;
                return true;
            }
            if (Value == TEXT("maximum") || Value == TEXT("max"))
            {
                OutType = EEnvTestFilterType::Maximum;
                return true;
            }
            if (Value == TEXT("range") || Value == TEXT("float_range"))
            {
                OutType = EEnvTestFilterType::Range;
                return true;
            }
            if (Value == TEXT("match") || Value == TEXT("bool") || Value == TEXT("boolean"))
            {
                OutType = EEnvTestFilterType::Match;
                return true;
            }
            return false;
        }

        bool ParseScoringEquation(const FString& Equation, EEnvTestScoreEquation::Type& OutEquation)
        {
            const FString Value = NormalizeToken(Equation);
            if (Value == TEXT("linear"))
            {
                OutEquation = EEnvTestScoreEquation::Linear;
                return true;
            }
            if (Value == TEXT("inverse_linear") || Value == TEXT("inverselinear"))
            {
                OutEquation = EEnvTestScoreEquation::InverseLinear;
                return true;
            }
            if (Value == TEXT("square"))
            {
                OutEquation = EEnvTestScoreEquation::Square;
                return true;
            }
            if (Value == TEXT("square_root") || Value == TEXT("squareroot"))
            {
                OutEquation = EEnvTestScoreEquation::SquareRoot;
                return true;
            }
            if (Value == TEXT("constant"))
            {
                OutEquation = EEnvTestScoreEquation::Constant;
                return true;
            }
            return false;
        }

        bool ParseClampType(const FString& ClampType, EEnvQueryTestClamping::Type& OutClampType)
        {
            const FString Value = NormalizeToken(ClampType);
            if (Value == TEXT("none"))
            {
                OutClampType = EEnvQueryTestClamping::None;
                return true;
            }
            if (Value == TEXT("specified_value") || Value == TEXT("specifiedvalue") || Value == TEXT("specified"))
            {
                OutClampType = EEnvQueryTestClamping::SpecifiedValue;
                return true;
            }
            if (Value == TEXT("filter_threshold") || Value == TEXT("filterthreshold"))
            {
                OutClampType = EEnvQueryTestClamping::FilterThreshold;
                return true;
            }
            return false;
        }

        UObject* ResolveContextTarget(FHandlerContext& Ctx, UEnvQueryOption* Option, int32 TestIndex)
        {
            if (TestIndex >= 0)
            {
                return GetTestOrSendError(Ctx, Option, TestIndex);
            }
            return Option->Generator;
        }

        FClassProperty* FindContextClassProperty(UObject* Target, const FString& PropertyName)
        {
            if (!Target)
            {
                return nullptr;
            }

            if (!PropertyName.IsEmpty())
            {
                FClassProperty* Property = FindFProperty<FClassProperty>(Target->GetClass(), *PropertyName);
                return Property && Property->MetaClass && Property->MetaClass->IsChildOf(UEnvQueryContext::StaticClass())
                    ? Property
                    : nullptr;
            }

            for (TFieldIterator<FClassProperty> It(Target->GetClass()); It; ++It)
            {
                FClassProperty* Property = *It;
                if (Property && Property->MetaClass && Property->MetaClass->IsChildOf(UEnvQueryContext::StaticClass()))
                {
                    return Property;
                }
            }
            return nullptr;
        }

        TSharedPtr<FJsonObject> MakeQueryResult(UEnvQuery* Query)
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("queryPath"), Query->GetPathName());
            Result->SetNumberField(TEXT("optionCount"), Query->GetOptionsMutable().Num());
            AddAssetVerification(Result, Query);
            return Result;
        }
    }

    bool HandleCreate(FHandlerContext& Ctx)
    {
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        const FString Name = GetJsonStringField(Payload, TEXT("name"));
        const FString Path = GetJsonStringField(Payload, TEXT("path"), TEXT("/Game/AI/EQS"));
        const bool bSave = GetJsonBoolField(Payload, TEXT("save"), true);

        FString PackagePath;
        FString ObjectPath;
        FString Error;
        if (!BuildAssetPaths(Path, Name, PackagePath, ObjectPath, Error))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), Error);
            return true;
        }

        if (ResolveAsset(ObjectPath).bExists || FindObject<UEnvQuery>(nullptr, *ObjectPath))
        {
            Ctx.SendError(TEXT("ALREADY_EXISTS"), FString::Printf(TEXT("EQS Query already exists: %s"), *ObjectPath));
            return true;
        }

        FScopedTransaction Transaction(NSLOCTEXT("PinWrightEQS", "CreateEQSQuery", "Create EQS Query"));

        UPackage* Package = CreatePackage(*PackagePath);
        UEnvQuery* Query = NewObject<UEnvQuery>(Package, UEnvQuery::StaticClass(), FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
        if (!Query)
        {
            Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create EQS Query asset"));
            return true;
        }

        Query->Modify();
        FAssetRegistryModule::AssetCreated(Query);
        Query->MarkPackageDirty();
        if (bSave)
        {
            McpSafeAssetSave(Query);
        }

        TSharedPtr<FJsonObject> Result = MakeQueryResult(Query);
        Result->SetBoolField(TEXT("saved"), bSave);
        Ctx.SendSuccess(Result);
        return true;
    }

    bool HandleAddGenerator(FHandlerContext& Ctx)
    {
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        const FString QueryPath = GetJsonStringField(Payload, TEXT("queryPath"));
        const FString GeneratorType = GetJsonStringField(Payload, TEXT("generatorType"));
        const bool bSave = GetJsonBoolField(Payload, TEXT("save"), false);

        if (QueryPath.IsEmpty() || GeneratorType.IsEmpty())
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("queryPath and generatorType are required"));
            return true;
        }

        UEnvQuery* Query = LoadQueryOrSendError(Ctx, QueryPath);
        if (!Query)
        {
            return true;
        }

        UClass* GeneratorClass = ResolveClassByPathOrName(GeneratorType, UEnvQueryGenerator::StaticClass(), GeneratorClassMap());
        if (!GeneratorClass)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Unsupported EQS generator type or class: %s. Valid built-in names: %s (or pass a full generator class path)."), *GeneratorType, *GeneratorTokenList()));
            return true;
        }

        FScopedTransaction Transaction(NSLOCTEXT("PinWrightEQS", "AddEQSGenerator", "Add EQS Generator"));
        Query->Modify();

        UEnvQueryOption* Option = NewObject<UEnvQueryOption>(Query, UEnvQueryOption::StaticClass(), NAME_None, RF_Transactional);
        UEnvQueryGenerator* Generator = NewObject<UEnvQueryGenerator>(Option, GeneratorClass, NAME_None, RF_Transactional);
        if (!Option || !Generator)
        {
            Ctx.SendError(TEXT("CREATION_FAILED"), FString::Printf(TEXT("Failed to create generator: %s"), *GeneratorType));
            return true;
        }

        Option->Modify();
        Generator->Modify();
        Option->Generator = Generator;
        const int32 GeneratorIndex = Query->GetOptionsMutable().Add(Option);
        Query->MarkPackageDirty();
        if (bSave)
        {
            McpSafeAssetSave(Query);
        }

        TSharedPtr<FJsonObject> Result = MakeQueryResult(Query);
        Result->SetNumberField(TEXT("generatorIndex"), GeneratorIndex);
        Result->SetStringField(TEXT("generatorClass"), GeneratorClass->GetPathName());
        Result->SetBoolField(TEXT("saved"), bSave);
        Ctx.SendSuccess(Result);
        return true;
    }

    bool HandleAddTest(FHandlerContext& Ctx)
    {
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        const FString QueryPath = GetJsonStringField(Payload, TEXT("queryPath"));
        const FString TestType = GetJsonStringField(Payload, TEXT("testType"));
        const int32 GeneratorIndex = static_cast<int32>(GetJsonNumberField(Payload, TEXT("generatorIndex"), 0.0));
        const FString Purpose = GetJsonStringField(Payload, TEXT("purpose"), TEXT("score"));
        const bool bSave = GetJsonBoolField(Payload, TEXT("save"), false);

        if (QueryPath.IsEmpty() || TestType.IsEmpty())
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("queryPath and testType are required"));
            return true;
        }

        UEnvQuery* Query = LoadQueryOrSendError(Ctx, QueryPath);
        if (!Query)
        {
            return true;
        }

        UEnvQueryOption* Option = GetOptionOrSendError(Ctx, Query, GeneratorIndex);
        if (!Option)
        {
            return true;
        }

        UClass* TestClass = ResolveClassByPathOrName(TestType, UEnvQueryTest::StaticClass(), TestClassMap());
        if (!TestClass)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Unsupported EQS test type or class: %s. Valid built-in names: %s (or pass a full test class path)."), *TestType, *TestTokenList()));
            return true;
        }

        FScopedTransaction Transaction(NSLOCTEXT("PinWrightEQS", "AddEQSTest", "Add EQS Test"));
        Query->Modify();
        Option->Modify();

        UEnvQueryTest* Test = NewObject<UEnvQueryTest>(Option, TestClass, NAME_None, RF_Transactional);
        if (!Test)
        {
            Ctx.SendError(TEXT("CREATION_FAILED"), FString::Printf(TEXT("Failed to create test: %s"), *TestType));
            return true;
        }

        Test->Modify();
        Test->TestOrder = Option->Tests.Num();
        Test->TestPurpose = ParsePurpose(Purpose);
        const int32 TestIndex = Option->Tests.Add(Test);
        Query->MarkPackageDirty();
        if (bSave)
        {
            McpSafeAssetSave(Query);
        }

        TSharedPtr<FJsonObject> Result = MakeQueryResult(Query);
        Result->SetNumberField(TEXT("generatorIndex"), GeneratorIndex);
        Result->SetNumberField(TEXT("testIndex"), TestIndex);
        Result->SetStringField(TEXT("testClass"), TestClass->GetPathName());
        Result->SetStringField(TEXT("purpose"), Purpose);
        Result->SetBoolField(TEXT("saved"), bSave);
        Ctx.SendSuccess(Result);
        return true;
    }

    bool HandleSetContextClass(FHandlerContext& Ctx)
    {
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        const FString QueryPath = GetJsonStringField(Payload, TEXT("queryPath"));
        const int32 GeneratorIndex = static_cast<int32>(GetJsonNumberField(Payload, TEXT("generatorIndex"), 0.0));
        const int32 TestIndex = static_cast<int32>(GetJsonNumberField(Payload, TEXT("testIndex"), -1.0));
        const FString PropertyName = GetJsonStringField(Payload, TEXT("propertyName"));
        const FString ContextClassName = GetJsonStringField(Payload, TEXT("contextClass"), GetJsonStringField(Payload, TEXT("contextType")));
        const bool bSave = GetJsonBoolField(Payload, TEXT("save"), false);

        if (QueryPath.IsEmpty() || ContextClassName.IsEmpty())
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("queryPath and contextClass are required"));
            return true;
        }

        UEnvQuery* Query = LoadQueryOrSendError(Ctx, QueryPath);
        if (!Query)
        {
            return true;
        }

        UEnvQueryOption* Option = GetOptionOrSendError(Ctx, Query, GeneratorIndex);
        if (!Option)
        {
            return true;
        }

        UObject* Target = ResolveContextTarget(Ctx, Option, TestIndex);
        if (!Target)
        {
            return true;
        }

        FClassProperty* Property = FindContextClassProperty(Target, PropertyName);
        if (!Property)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), PropertyName.IsEmpty()
                ? TEXT("No UEnvQueryContext class property found on target")
                : FString::Printf(TEXT("Context class property not found: %s"), *PropertyName));
            return true;
        }

        UClass* ContextClass = ResolveClassByPathOrName(ContextClassName, UEnvQueryContext::StaticClass(), ContextClassMap());
        if (!ContextClass)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Unsupported EQS context class: %s. Valid built-in names: %s (or pass a full UEnvQueryContext class path; project-defined contexts can be located via asset.list)."), *ContextClassName, *ContextTokenList()));
            return true;
        }

        FScopedTransaction Transaction(NSLOCTEXT("PinWrightEQS", "SetEQSContextClass", "Set EQS Context Class"));
        Query->Modify();
        Option->Modify();
        Target->Modify();
        Property->SetPropertyValue_InContainer(Target, ContextClass);
        Query->MarkPackageDirty();
        EAssetSaveState SaveState = bSave
            ? EAssetSaveState::Failed
            : EAssetSaveState::NotRequested;
        bool bSaved = false;
        if (bSave)
        {
            bSaved = SaveAssetToDiskReportingPresence(
                Query, /*bForce=*/true, nullptr, nullptr, &SaveState);
        }

        TSharedPtr<FJsonObject> Result = MakeQueryResult(Query);
        Result->SetNumberField(TEXT("generatorIndex"), GeneratorIndex);
        if (TestIndex >= 0)
        {
            Result->SetNumberField(TEXT("testIndex"), TestIndex);
        }
        Result->SetStringField(TEXT("targetClass"), Target->GetClass()->GetPathName());
        Result->SetStringField(TEXT("propertyName"), Property->GetName());
        Result->SetStringField(TEXT("contextClass"), ContextClass->GetPathName());
        AddAssetSaveReport(Result, bSave, bSaved, SaveState);
        if (bSave && !bSaved)
        {
            Ctx.SendError(TEXT("SAVE_FAILED"), FString::Printf(
                TEXT("EQS context changed in memory, but the query save was not durable (saveState=%s)."),
                AssetSaveStateToWire(SaveState)), Result);
            return true;
        }
        Ctx.SendSuccess(Result);
        return true;
    }

    bool HandleSetTestFilter(FHandlerContext& Ctx)
    {
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        const FString QueryPath = GetJsonStringField(Payload, TEXT("queryPath"));
        const int32 GeneratorIndex = static_cast<int32>(GetJsonNumberField(Payload, TEXT("generatorIndex"), 0.0));
        const int32 TestIndex = static_cast<int32>(GetJsonNumberField(Payload, TEXT("testIndex"), -1.0));
        const bool bSave = GetJsonBoolField(Payload, TEXT("save"), false);

        const TSharedPtr<FJsonObject>* FilterObjectPtr = nullptr;
        TSharedPtr<FJsonObject> FilterObject = Payload;
        if (Payload.IsValid() && Payload->TryGetObjectField(TEXT("filter"), FilterObjectPtr) && FilterObjectPtr && FilterObjectPtr->IsValid())
        {
            FilterObject = *FilterObjectPtr;
        }

        const bool bHasKind = FilterObject.IsValid() && FilterObject->HasField(TEXT("kind"));
        const bool bHasFilterType = FilterObject.IsValid() && FilterObject->HasField(TEXT("filterType"));
        if (bHasKind && bHasFilterType)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                TEXT("filter must use exactly one discriminator: kind or filterType, not both"));
            return true;
        }

        const TCHAR* Discriminator = bHasFilterType ? TEXT("filterType") : TEXT("kind");
        const FString Kind = GetJsonStringField(FilterObject, Discriminator);
        if (QueryPath.IsEmpty() || TestIndex < 0 || Kind.IsEmpty())
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("queryPath, generatorIndex, testIndex, and filter.kind are required"));
            return true;
        }

        EEnvTestFilterType::Type FilterType;
        if (!ParseFilterType(Kind, FilterType))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Unsupported EQS filter kind: %s. Valid: %s."), *Kind, *FilterKindTokenList()));
            return true;
        }

        TArray<FString> Allowed = { Discriminator };
        if (FilterType == EEnvTestFilterType::Match)
        {
            Allowed.Add(TEXT("value"));
        }
        else if (FilterType == EEnvTestFilterType::Minimum)
        {
            Allowed.Add(TEXT("min"));
        }
        else if (FilterType == EEnvTestFilterType::Maximum)
        {
            Allowed.Add(TEXT("max"));
        }
        else
        {
            Allowed.Add(TEXT("min"));
            Allowed.Add(TEXT("max"));
        }

        TArray<FString> Unknown;
        if (!::RejectUnknownKeys(FilterObject, Allowed, Unknown, ERejectUnknownKeysMode::AllSorted, ESearchCase::IgnoreCase))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(
                TEXT("filter kind '%s' does not accept: %s. Valid keys: %s"),
                *Kind, *FString::Join(Unknown, TEXT(", ")), *FString::Join(Allowed, TEXT(", "))));
            return true;
        }

        bool BoolValue = true;
        double MinValue = 0.0;
        double MaxValue = 0.0;
        if (FilterType == EEnvTestFilterType::Match && !FilterObject->HasField(TEXT("value")))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("filter.value is required for Match filters"));
            return true;
        }
        if ((FilterType == EEnvTestFilterType::Minimum || FilterType == EEnvTestFilterType::Range)
            && !FilterObject->HasField(TEXT("min")))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("filter.min is required for Minimum and Range filters"));
            return true;
        }
        if ((FilterType == EEnvTestFilterType::Maximum || FilterType == EEnvTestFilterType::Range)
            && !FilterObject->HasField(TEXT("max")))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("filter.max is required for Maximum and Range filters"));
            return true;
        }
        if (FilterType == EEnvTestFilterType::Match
            && !FilterObject->TryGetBoolField(TEXT("value"), BoolValue))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("filter.value must be a boolean"));
            return true;
        }
        if ((FilterType == EEnvTestFilterType::Minimum || FilterType == EEnvTestFilterType::Range)
            && !FilterObject->TryGetNumberField(TEXT("min"), MinValue))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("filter.min must be a number"));
            return true;
        }
        if ((FilterType == EEnvTestFilterType::Maximum || FilterType == EEnvTestFilterType::Range)
            && !FilterObject->TryGetNumberField(TEXT("max"), MaxValue))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("filter.max must be a number"));
            return true;
        }

        UEnvQuery* Query = LoadQueryOrSendError(Ctx, QueryPath);
        if (!Query)
        {
            return true;
        }

        UEnvQueryOption* Option = GetOptionOrSendError(Ctx, Query, GeneratorIndex);
        if (!Option)
        {
            return true;
        }

        UEnvQueryTest* Test = GetTestOrSendError(Ctx, Option, TestIndex);
        if (!Test)
        {
            return true;
        }

        FScopedTransaction Transaction(NSLOCTEXT("PinWrightEQS", "SetEQSTestFilter", "Set EQS Test Filter"));
        Query->Modify();
        Option->Modify();
        Test->Modify();

        Test->FilterType = FilterType;
        if (FilterType == EEnvTestFilterType::Match)
        {
            Test->BoolValue.DefaultValue = BoolValue;
        }
        else if (FilterType == EEnvTestFilterType::Minimum)
        {
            Test->FloatValueMin.DefaultValue = static_cast<float>(MinValue);
        }
        else if (FilterType == EEnvTestFilterType::Maximum)
        {
            Test->FloatValueMax.DefaultValue = static_cast<float>(MaxValue);
        }
        else
        {
            Test->FloatValueMin.DefaultValue = static_cast<float>(MinValue);
            Test->FloatValueMax.DefaultValue = static_cast<float>(MaxValue);
        }

        Query->MarkPackageDirty();
        if (bSave)
        {
            McpSafeAssetSave(Query);
        }

        TSharedPtr<FJsonObject> Result = MakeQueryResult(Query);
        Result->SetNumberField(TEXT("generatorIndex"), GeneratorIndex);
        Result->SetNumberField(TEXT("testIndex"), TestIndex);
        Result->SetStringField(TEXT("filterKind"), Kind);
        Result->SetBoolField(TEXT("saved"), bSave);
        Ctx.SendSuccess(Result);
        return true;
    }

    bool HandleSetTestScoring(FHandlerContext& Ctx)
    {
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        const FString QueryPath = GetJsonStringField(Payload, TEXT("queryPath"));
        const int32 GeneratorIndex = static_cast<int32>(GetJsonNumberField(Payload, TEXT("generatorIndex"), 0.0));
        const int32 TestIndex = static_cast<int32>(GetJsonNumberField(Payload, TEXT("testIndex"), -1.0));
        const bool bSave = GetJsonBoolField(Payload, TEXT("save"), false);

        const TSharedPtr<FJsonObject>* ScoringObjectPtr = nullptr;
        TSharedPtr<FJsonObject> ScoringObject = Payload;
        if (Payload.IsValid() && Payload->TryGetObjectField(TEXT("scoring"), ScoringObjectPtr) && ScoringObjectPtr && ScoringObjectPtr->IsValid())
        {
            ScoringObject = *ScoringObjectPtr;
        }

        if (QueryPath.IsEmpty() || TestIndex < 0)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("queryPath, generatorIndex, and testIndex are required"));
            return true;
        }
        if ((Payload.IsValid() && Payload->HasField(TEXT("curve"))) || (ScoringObject.IsValid() && ScoringObject->HasField(TEXT("curve"))))
        {
            Ctx.SendError(TEXT("UNSUPPORTED_ARGUMENT"), TEXT("curve is not supported because UE 5.6 UEnvQueryTest has no ScoringCurve field"));
            return true;
        }

        UEnvQuery* Query = LoadQueryOrSendError(Ctx, QueryPath);
        if (!Query)
        {
            return true;
        }

        UEnvQueryOption* Option = GetOptionOrSendError(Ctx, Query, GeneratorIndex);
        if (!Option)
        {
            return true;
        }

        UEnvQueryTest* Test = GetTestOrSendError(Ctx, Option, TestIndex);
        if (!Test)
        {
            return true;
        }

        const FString Equation = GetJsonStringField(ScoringObject, TEXT("equation"), GetJsonStringField(ScoringObject, TEXT("scoringEquation")));
        EEnvTestScoreEquation::Type ScoringEquation = EEnvTestScoreEquation::Linear;
        const bool bHasEquation = !Equation.IsEmpty();
        if (bHasEquation && !ParseScoringEquation(Equation, ScoringEquation))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Unsupported EQS scoring equation: %s. Valid: %s."), *Equation, *ScoringEquationTokenList()));
            return true;
        }

        const FString ClampMinTypeName = GetJsonStringField(ScoringObject, TEXT("clampMinType"));
        EEnvQueryTestClamping::Type ClampMinType = EEnvQueryTestClamping::None;
        const bool bHasClampMinType = !ClampMinTypeName.IsEmpty();
        if (bHasClampMinType && !ParseClampType(ClampMinTypeName, ClampMinType))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Unsupported EQS clampMinType: %s"), *ClampMinTypeName));
            return true;
        }

        const FString ClampMaxTypeName = GetJsonStringField(ScoringObject, TEXT("clampMaxType"));
        EEnvQueryTestClamping::Type ClampMaxType = EEnvQueryTestClamping::None;
        const bool bHasClampMaxType = !ClampMaxTypeName.IsEmpty();
        if (bHasClampMaxType && !ParseClampType(ClampMaxTypeName, ClampMaxType))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Unsupported EQS clampMaxType: %s"), *ClampMaxTypeName));
            return true;
        }

        FScopedTransaction Transaction(NSLOCTEXT("PinWrightEQS", "SetEQSTestScoring", "Set EQS Test Scoring"));
        Query->Modify();
        Option->Modify();
        Test->Modify();

        if (!Equation.IsEmpty())
        {
            Test->ScoringEquation = ScoringEquation;
        }

        double NumericValue = 0.0;
        if (TryGetJsonNumberField(ScoringObject, TEXT("factor"), NumericValue) || TryGetJsonNumberField(ScoringObject, TEXT("scoringFactor"), NumericValue))
        {
            Test->ScoringFactor.DefaultValue = static_cast<float>(NumericValue);
        }
        if (TryGetJsonNumberField(ScoringObject, TEXT("clampMin"), NumericValue) || TryGetJsonNumberField(ScoringObject, TEXT("scoreClampMin"), NumericValue))
        {
            Test->ScoreClampMin.DefaultValue = static_cast<float>(NumericValue);
        }
        if (TryGetJsonNumberField(ScoringObject, TEXT("clampMax"), NumericValue) || TryGetJsonNumberField(ScoringObject, TEXT("scoreClampMax"), NumericValue))
        {
            Test->ScoreClampMax.DefaultValue = static_cast<float>(NumericValue);
        }
        if (TryGetJsonNumberField(ScoringObject, TEXT("referenceValue"), NumericValue))
        {
            Test->ReferenceValue.DefaultValue = static_cast<float>(NumericValue);
            Test->bDefineReferenceValue = true;
        }

        if (bHasClampMinType)
        {
            Test->ClampMinType = ClampMinType;
        }

        if (bHasClampMaxType)
        {
            Test->ClampMaxType = ClampMaxType;
        }

        Query->MarkPackageDirty();
        if (bSave)
        {
            McpSafeAssetSave(Query);
        }

        TSharedPtr<FJsonObject> Result = MakeQueryResult(Query);
        Result->SetNumberField(TEXT("generatorIndex"), GeneratorIndex);
        Result->SetNumberField(TEXT("testIndex"), TestIndex);
        Result->SetBoolField(TEXT("saved"), bSave);
        Ctx.SendSuccess(Result);
        return true;
    }
}

REGISTER_RPC_HANDLER("eqs.create", "eqs",
    "Create a new EQS Query asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the new EQS Query"),
        RPC_PARAM_DEF("path", "path", "Content path for the asset", "/Game/AI/EQS"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset after creation", "true")
    ))
{
    return PinWrightEQS::HandleCreate(Ctx);
}

REGISTER_RPC_HANDLER("eqs.add_generator", "eqs",
    "Add a persisted generator option to an EQS Query",
    RPC_PARAMS(
        RPC_PARAM_REQ("queryPath", "path", "Path to the EQS Query asset"),
        RPC_PARAM_REQ("generatorType", "classref", "Built-in generator name or generator class path"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset after mutation", "false")
    ))
{
    return PinWrightEQS::HandleAddGenerator(Ctx);
}

REGISTER_RPC_HANDLER("eqs.add_test", "eqs",
    "Add a persisted test under an EQS generator option",
    RPC_PARAMS(
        RPC_PARAM_REQ("queryPath", "path", "Path to the EQS Query asset"),
        RPC_PARAM_DEF("generatorIndex", "integer", "Generator option index", "0"),
        RPC_PARAM_REQ("testType", "classref", "Built-in test name or test class path"),
        RPC_PARAM_DEF("purpose", "string", "filter, score, or filter_and_score", "score"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset after mutation", "false")
    ))
{
    return PinWrightEQS::HandleAddTest(Ctx);
}

REGISTER_RPC_HANDLER("eqs.set_context_class", "eqs",
    "Assign a UEnvQueryContext subclass to a generator or test context property",
    RPC_PARAMS(
        RPC_PARAM_REQ("queryPath", "path", "Path to the EQS Query asset"),
        RPC_PARAM_DEF("generatorIndex", "integer", "Generator option index", "0"),
        RPC_PARAM_OPT("testIndex", "integer", "Test index; omitted targets the generator"),
        RPC_PARAM_OPT("propertyName", "string", "Context property name; omitted picks the first context property"),
        RPC_PARAM_REQ("contextClass", "classref", "Built-in context name or context class path"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset after mutation", "false")
    ))
{
    return PinWrightEQS::HandleSetContextClass(Ctx);
}

REGISTER_RPC_HANDLER("eqs.set_test_filter", "eqs",
    "Set EQS test filter fields",
    RPC_PARAMS(
        RPC_PARAM_REQ("queryPath", "path", "Path to the EQS Query asset"),
        RPC_PARAM_DEF("generatorIndex", "integer", "Generator option index", "0"),
        RPC_PARAM_REQ("testIndex", "integer", "Test index"),
        RPC_PARAM_REQ("filter", "object", "Closed branch schema with exactly one discriminator, kind or filterType: match/bool requires boolean value; minimum/min requires numeric min; maximum/max requires numeric max; range/float_range requires numeric min and max"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset after mutation", "false")
    ))
{
    return PinWrightEQS::HandleSetTestFilter(Ctx);
}

REGISTER_RPC_HANDLER("eqs.set_test_scoring", "eqs",
    "Set EQS test scoring fields; curve is rejected on UE 5.6",
    RPC_PARAMS(
        RPC_PARAM_REQ("queryPath", "path", "Path to the EQS Query asset"),
        RPC_PARAM_DEF("generatorIndex", "integer", "Generator option index", "0"),
        RPC_PARAM_REQ("testIndex", "integer", "Test index"),
        RPC_PARAM_OPT("scoring", "object", "Scoring object with equation, factor, clamps, referenceValue"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset after mutation", "false")
    ))
{
    return PinWrightEQS::HandleSetTestScoring(Ctx);
}
