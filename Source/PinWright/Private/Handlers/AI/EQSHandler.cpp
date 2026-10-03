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

#include "AIGraphNode.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryContext.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"

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

        bool TryParsePurpose(const FString& Purpose, EEnvTestPurpose::Type& OutPurpose)
        {
            const FString Value = NormalizeToken(Purpose);
            if (Value == TEXT("filter"))
            {
                OutPurpose = EEnvTestPurpose::Filter;
                return true;
            }
            if (Value == TEXT("score"))
            {
                OutPurpose = EEnvTestPurpose::Score;
                return true;
            }
            if (Value == TEXT("filter_and_score") || Value == TEXT("filterandscore"))
            {
                OutPurpose = EEnvTestPurpose::FilterAndScore;
                return true;
            }
            return false;
        }

        // eqs.add_test's historical lenient parse: an unrecognized purpose falls back to Score.
        EEnvTestPurpose::Type ParsePurpose(const FString& Purpose)
        {
            EEnvTestPurpose::Type Parsed = EEnvTestPurpose::Score;
            TryParsePurpose(Purpose, Parsed);
            return Parsed;
        }

        const TCHAR* PurposeToWire(EEnvTestPurpose::Type Purpose)
        {
            switch (Purpose)
            {
            case EEnvTestPurpose::Filter: return TEXT("filter");
            case EEnvTestPurpose::FilterAndScore: return TEXT("filter_and_score");
            default: return TEXT("score");
            }
        }

        // UEnvironmentQueryGraph::UpdateAsset rebuilds every option's Tests array from the graph's
        // test subnodes, and SpawnMissingNodes only ADDS nodes for tests the graph lacks. So a test
        // removed from Option->Tests alone comes back on the next edit in the EQS editor. Drop the
        // graph's subnode for it too. Returns the number of subnodes removed (0 when the query has
        // never been opened in the EQS editor and so has no graph).
        int32 RemoveTestFromEditorGraph(UEnvQuery* Query, const UEnvQueryTest* Test)
        {
            int32 Removed = 0;
#if WITH_EDITORONLY_DATA
            if (!Query->EdGraph)
            {
                return 0;
            }
            for (UEdGraphNode* Node : Query->EdGraph->Nodes)
            {
                UAIGraphNode* OptionNode = Cast<UAIGraphNode>(Node);
                if (!OptionNode)
                {
                    continue;
                }
                for (int32 SubIdx = OptionNode->SubNodes.Num() - 1; SubIdx >= 0; --SubIdx)
                {
                    UAIGraphNode* SubNode = OptionNode->SubNodes[SubIdx];
                    if (SubNode && SubNode->NodeInstance == Test)
                    {
                        OptionNode->RemoveSubNode(SubNode);
                        ++Removed;
                    }
                }
            }
            if (Removed > 0)
            {
                Query->EdGraph->NotifyGraphChanged();
            }
#endif
            return Removed;
        }

        // Measured save tail shared by the edit verbs: saved:true only when the package write
        // reached disk; a failed write is SAVE_FAILED carrying the same report.
        bool SendMutationWithSaveReport(FHandlerContext& Ctx, UEnvQuery* Query, bool bSave, const TSharedPtr<FJsonObject>& Result, const TCHAR* What)
        {
            EAssetSaveState SaveState = bSave
                ? EAssetSaveState::Failed
                : EAssetSaveState::NotRequested;
            bool bSaved = false;
            if (bSave)
            {
                bSaved = SaveAssetToDiskReportingPresence(
                    Query, /*bForce=*/true, nullptr, nullptr, &SaveState);
            }
            AddAssetSaveReport(Result, bSave, bSaved, SaveState);
            if (bSave && !bSaved)
            {
                Ctx.SendError(TEXT("SAVE_FAILED"), FString::Printf(
                    TEXT("%s in memory, but the query save was not durable (saveState=%s)."),
                    What, AssetSaveStateToWire(SaveState)), Result);
                return true;
            }
            Ctx.SendSuccess(Result);
            return true;
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

    bool HandleCreate(FHandlerContext& Ctx, bool bOverwrite)
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

        UEnvQuery* Existing = nullptr;
        if (ResolveAsset(ObjectPath).bExists || FindObject<UEnvQuery>(nullptr, *ObjectPath))
        {
            Existing = bOverwrite ? Cast<UEnvQuery>(LoadObject<UObject>(nullptr, *ObjectPath)) : nullptr;
            if (!Existing)
            {
                Ctx.SendError(TEXT("ALREADY_EXISTS"), bOverwrite
                    ? FString::Printf(TEXT("Asset already exists and is not an EQS Query: %s (overwrite only clears an existing EQS Query)"), *ObjectPath)
                    : FString::Printf(TEXT("EQS Query already exists: %s (eqs.create overwrite:true clears it in place)"), *ObjectPath));
                return true;
            }
        }

        FScopedTransaction Transaction(NSLOCTEXT("PinWrightEQS", "CreateEQSQuery", "Create EQS Query"));

        UEnvQuery* Query = Existing;
        int32 PreviousOptionCount = 0;
        int32 ClosedEditorCount = 0;
        bool bEditorGraphDropped = false;
        if (Query)
        {
            // Clear in place so the UEnvQuery object (and every referencer) survives. An open EQS
            // editor would write its stale graph back over the cleared options, so close it, and
            // drop the graph so the next open regenerates it from Options.
            // Closing the editor synchronously is not tick-unsafe here: the EQS editor is a plain
            // graph editor with no SEditorViewport, so no viewport client is torn down mid-tick.
            if (GEditor)
            {
                if (UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
                {
                    ClosedEditorCount = AssetEditors->FindEditorsForAsset(Query).Num();
                    AssetEditors->CloseAllEditorsForAsset(Query);
                }
            }
            Query->Modify();
            PreviousOptionCount = Query->GetOptionsMutable().Num();
            Query->GetOptionsMutable().Reset();
#if WITH_EDITORONLY_DATA
            bEditorGraphDropped = Query->EdGraph != nullptr;
            Query->EdGraph = nullptr;
#endif
        }
        else
        {
            UPackage* Package = CreatePackage(*PackagePath);
            Query = NewObject<UEnvQuery>(Package, UEnvQuery::StaticClass(), FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
            if (!Query)
            {
                Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create EQS Query asset"));
                return true;
            }

            Query->Modify();
            FAssetRegistryModule::AssetCreated(Query);
        }
        Query->MarkPackageDirty();
        if (Existing)
        {
            // The overwrite edits a query that already exists, like the other edit verbs, so it
            // gets their measured save rather than the mark-dirty deferral used for a new asset.
            TSharedPtr<FJsonObject> Result = MakeQueryResult(Query);
            Result->SetBoolField(TEXT("overwritten"), true);
            Result->SetNumberField(TEXT("previousOptionCount"), PreviousOptionCount);
            Result->SetNumberField(TEXT("closedEditorCount"), ClosedEditorCount);
            Result->SetBoolField(TEXT("editorGraphDropped"), bEditorGraphDropped);
            return SendMutationWithSaveReport(Ctx, Query, bSave, Result, TEXT("EQS query cleared"));
        }
        if (bSave)
        {
            McpSafeAssetSave(Query);
        }

        TSharedPtr<FJsonObject> Result = MakeQueryResult(Query);
        Result->SetBoolField(TEXT("saved"), bSave);
        Result->SetBoolField(TEXT("overwritten"), false);
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

    bool HandleSetTestPurpose(FHandlerContext& Ctx)
    {
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        const FString QueryPath = GetJsonStringField(Payload, TEXT("queryPath"));
        const int32 GeneratorIndex = static_cast<int32>(GetJsonNumberField(Payload, TEXT("generatorIndex"), 0.0));
        const int32 TestIndex = static_cast<int32>(GetJsonNumberField(Payload, TEXT("testIndex"), -1.0));
        const FString PurposeName = GetJsonStringField(Payload, TEXT("purpose"));
        const bool bSave = GetJsonBoolField(Payload, TEXT("save"), false);

        EEnvTestPurpose::Type NewPurpose = EEnvTestPurpose::Score;
        if (!TryParsePurpose(PurposeName, NewPurpose))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Unsupported EQS test purpose: %s. Valid: filter, score, filter_and_score."), *PurposeName));
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

        const EEnvTestPurpose::Type PreviousPurpose = Test->TestPurpose;

        if (PreviousPurpose != NewPurpose)
        {
            FScopedTransaction Transaction(NSLOCTEXT("PinWrightEQS", "SetEQSTestPurpose", "Set EQS Test Purpose"));
            Query->Modify();
            Option->Modify();
            Test->Modify();
            Test->TestPurpose = NewPurpose;
            Query->MarkPackageDirty();
        }

        TSharedPtr<FJsonObject> Result = MakeQueryResult(Query);
        Result->SetNumberField(TEXT("generatorIndex"), GeneratorIndex);
        Result->SetNumberField(TEXT("testIndex"), TestIndex);
        Result->SetStringField(TEXT("testClass"), Test->GetClass()->GetPathName());
        Result->SetStringField(TEXT("previousPurpose"), PurposeToWire(PreviousPurpose));
        Result->SetStringField(TEXT("purpose"), PurposeToWire(Test->TestPurpose));
        Result->SetBoolField(TEXT("changed"), Test->TestPurpose != PreviousPurpose);
        return SendMutationWithSaveReport(Ctx, Query, bSave, Result, TEXT("EQS test purpose changed"));
    }

    bool HandleRemoveTest(FHandlerContext& Ctx)
    {
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        const FString QueryPath = GetJsonStringField(Payload, TEXT("queryPath"));
        const int32 GeneratorIndex = static_cast<int32>(GetJsonNumberField(Payload, TEXT("generatorIndex"), 0.0));
        const int32 TestIndex = static_cast<int32>(GetJsonNumberField(Payload, TEXT("testIndex"), -1.0));
        const bool bSave = GetJsonBoolField(Payload, TEXT("save"), false);

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

        FScopedTransaction Transaction(NSLOCTEXT("PinWrightEQS", "RemoveEQSTest", "Remove EQS Test"));
        Query->Modify();
        Option->Modify();
        Option->Tests.RemoveAt(TestIndex);
        // Keep TestOrder equal to the array index. UEnvironmentQueryGraph::UpdateAsset uses the
        // graph subnode index instead, which also counts disabled tests (absent from Tests), so the
        // two can differ by a gap; TestOrder only sorts, and the next editor rebuild re-derives it.
        for (int32 Idx = TestIndex; Idx < Option->Tests.Num(); ++Idx)
        {
            if (UEnvQueryTest* Remaining = Option->Tests[Idx])
            {
                Remaining->Modify();
                Remaining->TestOrder = Idx;
            }
        }
        const int32 GraphNodesRemoved = RemoveTestFromEditorGraph(Query, Test);
        Query->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = MakeQueryResult(Query);
        Result->SetNumberField(TEXT("generatorIndex"), GeneratorIndex);
        Result->SetNumberField(TEXT("removedTestIndex"), TestIndex);
        Result->SetStringField(TEXT("removedTestClass"), Test->GetClass()->GetPathName());
        Result->SetNumberField(TEXT("testCount"), Option->Tests.Num());
        Result->SetNumberField(TEXT("editorGraphNodesRemoved"), GraphNodesRemoved);
        return SendMutationWithSaveReport(Ctx, Query, bSave, Result, TEXT("EQS test removed"));
    }
}

REGISTER_RPC_HANDLER("eqs.create", "eqs",
    "Create a new EQS Query asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the new EQS Query"),
        RPC_PARAM_DEF("path", "path", "Content path for the asset", "/Game/AI/EQS"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset after creation", "true"),
        RPC_PARAM_DEF("overwrite", "boolean", "When an EQS Query already exists at path, clear its generators and tests in place (same object, so Behavior Tree and other referencers stay valid) instead of returning ALREADY_EXISTS. Unlike blueprint.create's overwrite it never deletes the asset.", "false")
    ))
{
    return PinWrightEQS::HandleCreate(Ctx, GetJsonBoolField(Ctx.GetRawPayload(), TEXT("overwrite"), false));
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

REGISTER_RPC_HANDLER("eqs.set_test_purpose", "eqs",
    "Change an existing EQS test's purpose (filter, score, or filter_and_score)",
    RPC_PARAMS(
        RPC_PARAM_REQ("queryPath", "path", "Path to the EQS Query asset"),
        RPC_PARAM_DEF("generatorIndex", "integer", "Generator option index", "0"),
        RPC_PARAM_REQ("testIndex", "integer", "Test index"),
        RPC_PARAM_REQ("purpose", "string", "filter, score, or filter_and_score; anything else is rejected"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset after mutation", "false")
    ))
{
    return PinWrightEQS::HandleSetTestPurpose(Ctx);
}

REGISTER_RPC_HANDLER("eqs.remove_test", "eqs",
    "Remove a test from an EQS generator option; later test indices shift down by one",
    RPC_PARAMS(
        RPC_PARAM_REQ("queryPath", "path", "Path to the EQS Query asset"),
        RPC_PARAM_DEF("generatorIndex", "integer", "Generator option index", "0"),
        RPC_PARAM_REQ("testIndex", "integer", "Test index to remove"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset after mutation", "false")
    ))
{
    return PinWrightEQS::HandleRemoveTest(Ctx);
}
