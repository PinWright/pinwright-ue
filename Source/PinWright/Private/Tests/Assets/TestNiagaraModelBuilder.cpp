// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Niagara/NiagaraModelBuilder.h"
#include "NiagaraJsonAssertionHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"


#include "Misc/Guid.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraEffectType.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/Package.h"

namespace
{
    using NiagaraJsonAssertionHelpers::HasArrayField;
    using NiagaraJsonAssertionHelpers::ArrayFieldNum;

    FString MakeUniqueNiagaraModelTestAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    template <typename TObject>
    TObject* NewTransientNiagaraModelTestAsset(const TCHAR* Prefix, FString& OutObjectPath)
    {
        const FString AssetName = MakeUniqueNiagaraModelTestAssetName(Prefix);
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        TObject* Asset = NewObject<TObject>(
            Package,
            TObject::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (Asset)
        {
            Asset->AddToRoot();
            OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        }
        return Asset;
    }

    TSharedPtr<FJsonObject> GetObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
    {
        const TSharedPtr<FJsonObject>* Field = nullptr;
        return Object.IsValid() && Object->TryGetObjectField(FieldName, Field) && Field ? *Field : nullptr;
    }

    TSharedPtr<FJsonObject> GetFirstArrayObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values || Values->Num() == 0)
        {
            return nullptr;
        }
        return (*Values)[0].IsValid() ? (*Values)[0]->AsObject() : nullptr;
    }

    void AttachModelTestEmitterGraphSource(UNiagaraEmitter* Emitter)
    {
        FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
        if (!EmitterData || EmitterData->GraphSource)
        {
            return;
        }

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Emitter, TEXT("ModelTestEmitterSource"), RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source ? NewObject<UNiagaraGraph>(Source, TEXT("ModelTestEmitterGraph"), RF_Transient | RF_Transactional) : nullptr;
        if (Graph)
        {
            Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
            Source->NodeGraph = Graph;
            EmitterData->GraphSource = Source;
        }
    }

    void AttachModelTestSystemGraph(UNiagaraSystem* System)
    {
        if (!System || !System->GetSystemSpawnScript() || !System->GetSystemUpdateScript())
        {
            return;
        }

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            System->GetSystemSpawnScript(),
            TEXT("ModelTestSystemScriptSource"),
            RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("ModelTestSystemScriptGraph"), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Graph)
        {
            return;
        }

        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;
        System->GetSystemSpawnScript()->SetLatestSource(Source);
        System->GetSystemUpdateScript()->SetLatestSource(Source);

        UNiagaraNodeFunctionCall* ModuleNode = NewObject<UNiagaraNodeFunctionCall>(
            Graph,
            UNiagaraNodeFunctionCall::StaticClass(),
            TEXT("ModelTestModule"),
            RF_Transient | RF_Transactional);
        if (ModuleNode)
        {
            ModuleNode->NodeGuid = FGuid::NewGuid();
            ModuleNode->NodePosX = 120;
            ModuleNode->NodePosY = 240;
            Graph->AddNode(ModuleNode, false, false);
        }
    }

    void PopulateModelTestSystem(UNiagaraSystem* System)
    {
        AttachModelTestSystemGraph(System);

        UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(
            System,
            FName(TEXT("ModelTestEmitterAsset")),
            RF_Transient | RF_Transactional);
        if (Emitter)
        {
            AttachModelTestEmitterGraphSource(Emitter);
            System->AddEmitterHandle(*Emitter, FName(TEXT("ModelTestEmitter")), Emitter->GetExposedVersion().VersionGuid);
        }

        const FNiagaraVariable UserFloat(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("User.ModelTestFloat")));
        System->GetExposedParameters().SetParameterValue(8.0f, UserFloat, true);
    }

    bool JsonObjectContainsUnsupportedTrue(const TSharedPtr<FJsonObject>& Object)
    {
        if (!Object.IsValid())
        {
            return false;
        }

        bool bUnsupported = false;
        if (Object->TryGetBoolField(TEXT("unsupported"), bUnsupported) && bUnsupported)
        {
            return true;
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : Object->Values)
        {
            if (!Pair.Value.IsValid())
            {
                continue;
            }
            if (Pair.Value->Type == EJson::Object && JsonObjectContainsUnsupportedTrue(Pair.Value->AsObject()))
            {
                return true;
            }
            if (Pair.Value->Type == EJson::Array)
            {
                const TArray<TSharedPtr<FJsonValue>>& Values = Pair.Value->AsArray();
                for (const TSharedPtr<FJsonValue>& Value : Values)
                {
                    if (Value.IsValid() && Value->Type == EJson::Object && JsonObjectContainsUnsupportedTrue(Value->AsObject()))
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraModelBuilderSystemOutputShapeTest,
    "PinWright.Assets.Niagara.ModelBuilder.SystemOutputShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraModelBuilderSystemOutputShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientNiagaraModelTestAsset<UNiagaraSystem>(TEXT("NS_ModelBuilder"), ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    PopulateModelTestSystem(System);

    TSharedPtr<FJsonObject> ModelJson = NiagaraModelBuilder::BuildSystemModelJson(System);
    TestNotNull(TEXT("Niagara model JSON built"), ModelJson.Get());
    if (!ModelJson.IsValid())
    {
        System->RemoveFromRoot();
        return false;
    }

    TestEqual(TEXT("schema"), ModelJson->GetStringField(TEXT("schema")), FString(TEXT("pinwright.niagara-model.v1")));
    TestEqual(TEXT("compact profile"), ModelJson->GetStringField(TEXT("profile")), FString(TEXT("compact")));
    TestEqual(TEXT("assetKind"), ModelJson->GetStringField(TEXT("assetKind")), FString(TEXT("NiagaraSystem")));
    TestEqual(TEXT("systemPath"), ModelJson->GetStringField(TEXT("systemPath")), ObjectPath);
    TestTrue(TEXT("diagnostics array present"), HasArrayField(ModelJson, TEXT("diagnostics")));
    TestTrue(TEXT("provenance object present"), ModelJson->HasTypedField<EJson::Object>(TEXT("provenance")));
    TestTrue(TEXT("emitters array present"), HasArrayField(ModelJson, TEXT("emitters")));
    TestEqual(TEXT("fixture emitter handle represented"), ArrayFieldNum(ModelJson, TEXT("emitters")), 1);
    TestTrue(TEXT("parameters object present"), ModelJson->HasTypedField<EJson::Object>(TEXT("parameters")));
    TestTrue(TEXT("stack object present"), ModelJson->HasTypedField<EJson::Object>(TEXT("stack")));
    TestFalse(TEXT("compact model does not embed raw graphs array"), ModelJson->HasTypedField<EJson::Array>(TEXT("graphs")));

    TSharedPtr<FJsonObject> Stack = GetObjectField(ModelJson, TEXT("stack"));
    TestNotNull(TEXT("stack object readable"), Stack.Get());
    if (Stack.IsValid())
    {
        TestTrue(TEXT("systemSpawn module array present"), HasArrayField(Stack, TEXT("systemSpawn")));
        TestTrue(TEXT("systemUpdate module array present"), HasArrayField(Stack, TEXT("systemUpdate")));
        TestEqual(TEXT("unconnected fixture module is not misbucketed into systemSpawn"), ArrayFieldNum(Stack, TEXT("systemSpawn")), 0);
        TestEqual(TEXT("unconnected fixture module is not misbucketed into systemUpdate"), ArrayFieldNum(Stack, TEXT("systemUpdate")), 0);
    }

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraModelBuilderEmitterOutputShapeTest,
    "PinWright.Assets.Niagara.ModelBuilder.EmitterOutputShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraModelBuilderEmitterOutputShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraEmitter* Emitter = NewTransientNiagaraModelTestAsset<UNiagaraEmitter>(TEXT("NE_ModelBuilder"), ObjectPath);
    TestNotNull(TEXT("Transient Niagara emitter created"), Emitter);
    if (!Emitter)
    {
        return false;
    }

    AttachModelTestEmitterGraphSource(Emitter);

    TSharedPtr<FJsonObject> ModelJson = NiagaraModelBuilder::BuildEmitterModelJson(Emitter);
    TestNotNull(TEXT("Niagara emitter model JSON built"), ModelJson.Get());
    if (!ModelJson.IsValid())
    {
        Emitter->RemoveFromRoot();
        return false;
    }

    TestEqual(TEXT("schema"), ModelJson->GetStringField(TEXT("schema")), FString(TEXT("pinwright.niagara-model.v1")));
    TestEqual(TEXT("compact profile"), ModelJson->GetStringField(TEXT("profile")), FString(TEXT("compact")));
    TestEqual(TEXT("assetKind"), ModelJson->GetStringField(TEXT("assetKind")), FString(TEXT("NiagaraEmitter")));
    TestEqual(TEXT("emitterPath"), ModelJson->GetStringField(TEXT("emitterPath")), ObjectPath);
    TestTrue(TEXT("diagnostics array present"), HasArrayField(ModelJson, TEXT("diagnostics")));
    TestTrue(TEXT("provenance object present"), ModelJson->HasTypedField<EJson::Object>(TEXT("provenance")));
    TestTrue(TEXT("source object present"), ModelJson->HasTypedField<EJson::Object>(TEXT("source")));
    TestTrue(TEXT("versioned emitter object present"), ModelJson->HasTypedField<EJson::Object>(TEXT("versionedEmitter")));
    TestTrue(TEXT("stack object present"), ModelJson->HasTypedField<EJson::Object>(TEXT("stack")));
    TestTrue(TEXT("renderers array present"), HasArrayField(ModelJson, TEXT("renderers")));
    TestTrue(TEXT("parameters object present"), ModelJson->HasTypedField<EJson::Object>(TEXT("parameters")));
    TestFalse(TEXT("compact model does not embed raw graphs array"), ModelJson->HasTypedField<EJson::Array>(TEXT("graphs")));

    Emitter->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraModelBuilderEmitterScalabilityShapeTest,
    "PinWright.Assets.Niagara.ModelBuilder.EmitterScalabilityShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraModelBuilderEmitterScalabilityShapeTest::RunTest(const FString& Parameters)
{
    FString SystemObjectPath;
    UNiagaraSystem* System = NewTransientNiagaraModelTestAsset<UNiagaraSystem>(TEXT("NS_ModelScalability"), SystemObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    PopulateModelTestSystem(System);

    const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
    TestEqual(TEXT("fixture system has one emitter"), Handles.Num(), 1);
    FVersionedNiagaraEmitterData* SystemEmitterData = Handles.Num() > 0 ? Handles[0].GetEmitterData() : nullptr;
    TestNotNull(TEXT("fixture system emitter data present"), SystemEmitterData);
    if (SystemEmitterData)
    {
        FNiagaraEmitterScalabilityOverride Override;
        Override.bOverrideSpawnCountScale = 1;
        Override.bScaleSpawnCount = 1;
        Override.SpawnCountScale = 0.25f;
        SystemEmitterData->ScalabilityOverrides.Overrides.Reset();
        SystemEmitterData->ScalabilityOverrides.Overrides.Add(Override);
    }

    TSharedPtr<FJsonObject> SystemModelJson = NiagaraModelBuilder::BuildSystemModelJson(System);
    TestNotNull(TEXT("Niagara system model JSON built"), SystemModelJson.Get());

    TSharedPtr<FJsonObject> SystemEmitterRecord = GetFirstArrayObjectField(SystemModelJson, TEXT("emitters"));
    TestNotNull(TEXT("system model direct emitter record present"), SystemEmitterRecord.Get());
    if (SystemEmitterRecord.IsValid())
    {
        TestTrue(TEXT("nested versioned emitter scalability remains present"), GetObjectField(GetObjectField(SystemEmitterRecord, TEXT("versionedEmitter")), TEXT("scalability")).IsValid());
        TSharedPtr<FJsonObject> DirectScalability = GetObjectField(SystemEmitterRecord, TEXT("scalability"));
        TestNotNull(TEXT("system emitter direct scalability object present"), DirectScalability.Get());

        const TArray<TSharedPtr<FJsonValue>>* DirectOverrides = nullptr;
        const bool bHasDirectOverrides = DirectScalability.IsValid() && DirectScalability->TryGetArrayField(TEXT("emitterScalability"), DirectOverrides);
        TestTrue(TEXT("system emitter direct emitterScalability array present"), bHasDirectOverrides);
        if (bHasDirectOverrides && DirectOverrides)
        {
            TestEqual(TEXT("system emitter direct emitterScalability has one entry"), DirectOverrides->Num(), 1);
            if (DirectOverrides->Num() == 1)
            {
                const TSharedPtr<FJsonObject> DirectOverride = (*DirectOverrides)[0]->AsObject();
                TestTrue(TEXT("system emitter direct emitterScalability entry is object"), DirectOverride.IsValid());
                if (DirectOverride.IsValid())
                {
                    bool bOverrideSpawnCountScale = false;
                    DirectOverride->TryGetBoolField(TEXT("bOverrideSpawnCountScale"), bOverrideSpawnCountScale);
                    TestTrue(TEXT("system emitter direct bOverrideSpawnCountScale true"), bOverrideSpawnCountScale);

                    double SpawnCountScale = 0.0;
                    DirectOverride->TryGetNumberField(TEXT("spawnCountScale"), SpawnCountScale);
                    TestEqual(TEXT("system emitter direct spawnCountScale round-trip"), SpawnCountScale, 0.25);
                }
            }
        }
    }

    FString EmitterObjectPath;
    UNiagaraEmitter* Emitter = NewTransientNiagaraModelTestAsset<UNiagaraEmitter>(TEXT("NE_ModelScalability"), EmitterObjectPath);
    TestNotNull(TEXT("Transient Niagara emitter created"), Emitter);
    if (!Emitter)
    {
        System->RemoveFromRoot();
        return false;
    }

    FVersionedNiagaraEmitterData* StandaloneEmitterData = Emitter->GetLatestEmitterData();
    TestNotNull(TEXT("standalone emitter data present"), StandaloneEmitterData);
    if (StandaloneEmitterData)
    {
        FNiagaraEmitterScalabilityOverride Override;
        Override.bOverrideSpawnCountScale = 1;
        Override.bScaleSpawnCount = 1;
        Override.SpawnCountScale = 0.5f;
        StandaloneEmitterData->ScalabilityOverrides.Overrides.Reset();
        StandaloneEmitterData->ScalabilityOverrides.Overrides.Add(Override);
    }

    TSharedPtr<FJsonObject> EmitterModelJson = NiagaraModelBuilder::BuildEmitterModelJson(Emitter);
    TestNotNull(TEXT("Niagara emitter model JSON built"), EmitterModelJson.Get());
    if (EmitterModelJson.IsValid())
    {
        TestTrue(TEXT("standalone nested versioned emitter scalability remains present"), GetObjectField(GetObjectField(EmitterModelJson, TEXT("versionedEmitter")), TEXT("scalability")).IsValid());
        TSharedPtr<FJsonObject> DirectScalability = GetObjectField(EmitterModelJson, TEXT("scalability"));
        TestNotNull(TEXT("standalone emitter direct root scalability object present"), DirectScalability.Get());

        const TArray<TSharedPtr<FJsonValue>>* DirectOverrides = nullptr;
        const bool bHasDirectOverrides = DirectScalability.IsValid() && DirectScalability->TryGetArrayField(TEXT("emitterScalability"), DirectOverrides);
        TestTrue(TEXT("standalone emitter direct emitterScalability array present"), bHasDirectOverrides);
        if (bHasDirectOverrides && DirectOverrides)
        {
            TestEqual(TEXT("standalone emitter direct emitterScalability has one entry"), DirectOverrides->Num(), 1);
            if (DirectOverrides->Num() == 1)
            {
                const TSharedPtr<FJsonObject> DirectOverride = (*DirectOverrides)[0]->AsObject();
                TestTrue(TEXT("standalone emitter direct emitterScalability entry is object"), DirectOverride.IsValid());
                if (DirectOverride.IsValid())
                {
                    bool bOverrideSpawnCountScale = false;
                    DirectOverride->TryGetBoolField(TEXT("bOverrideSpawnCountScale"), bOverrideSpawnCountScale);
                    TestTrue(TEXT("standalone emitter direct bOverrideSpawnCountScale true"), bOverrideSpawnCountScale);

                    double SpawnCountScale = 0.0;
                    DirectOverride->TryGetNumberField(TEXT("spawnCountScale"), SpawnCountScale);
                    TestEqual(TEXT("standalone emitter direct spawnCountScale round-trip"), SpawnCountScale, 0.5);
                }
            }
        }
    }

    Emitter->RemoveFromRoot();
    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraModelBuilderStructuredReferenceShapeTest,
    "PinWright.Assets.Niagara.ModelBuilder.StructuredReferenceShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraModelBuilderStructuredReferenceShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientNiagaraModelTestAsset<UNiagaraSystem>(TEXT("NS_ModelRef"), ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Refs = MakeShared<FJsonObject>();
    Refs->SetStringField(TEXT("rawGraphFile"), TEXT("niagara_graphs.json"));
    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetStringField(TEXT("propertySource"), TEXT("test"));
    TSharedPtr<FJsonObject> Provenance = MakeShared<FJsonObject>();
    Provenance->SetStringField(TEXT("rawGraphFile"), TEXT("niagara_graphs.json"));

    TSharedPtr<FJsonObject> Reference = NiagaraModelBuilder::BuildStructuredReference(
        TEXT("structured_reference_test"),
        TEXT("ModelRef"),
        System,
        System,
        TEXT("NiagaraSystem"),
        Refs,
        Properties,
        Provenance);

    TestNotNull(TEXT("structured reference built"), Reference.Get());
    if (Reference.IsValid())
    {
        TestEqual(TEXT("structured reference kind"), Reference->GetStringField(TEXT("kind")), FString(TEXT("reference")));
        TestEqual(TEXT("structured reference reason"), Reference->GetStringField(TEXT("reason")), FString(TEXT("structured_reference_test")));
        TestEqual(TEXT("structured reference displayName"), Reference->GetStringField(TEXT("displayName")), FString(TEXT("ModelRef")));
        TestTrue(TEXT("structured reference has class"), !Reference->GetStringField(TEXT("class")).IsEmpty());
        TestEqual(TEXT("structured reference object path"), Reference->GetStringField(TEXT("objectPath")), ObjectPath);
        TestTrue(TEXT("structured reference owner present"), Reference->HasTypedField<EJson::Object>(TEXT("owner")));
        TestTrue(TEXT("structured reference refs present"), Reference->HasTypedField<EJson::Object>(TEXT("refs")));
        TestTrue(TEXT("structured reference properties present"), Reference->HasTypedField<EJson::Object>(TEXT("properties")));
        TestTrue(TEXT("structured reference provenance present"), Reference->HasTypedField<EJson::Object>(TEXT("provenance")));
        TestFalse(TEXT("structured reference does not emit unsupported: true"), JsonObjectContainsUnsupportedTrue(Reference));
    }

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraModelBuilderDirectCoverageTest,
    "PinWright.Assets.Niagara.ModelBuilder.DirectCoverage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraModelBuilderDirectCoverageTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientNiagaraModelTestAsset<UNiagaraSystem>(TEXT("NS_ModelDump"), ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    PopulateModelTestSystem(System);

    TSharedPtr<FJsonObject> ModelJson = NiagaraModelBuilder::BuildSystemModelJson(System);
    TestNotNull(TEXT("Niagara system model JSON built directly"), ModelJson.Get());
    if (ModelJson.IsValid())
    {
        TestEqual(TEXT("direct model schema"), ModelJson->GetStringField(TEXT("schema")), FString(TEXT("pinwright.niagara-model.v1")));
        TestEqual(TEXT("direct model assetKind"), ModelJson->GetStringField(TEXT("assetKind")), FString(TEXT("NiagaraSystem")));
        TestEqual(TEXT("direct model systemPath"), ModelJson->GetStringField(TEXT("systemPath")), ObjectPath);
        TestTrue(TEXT("direct model emitters array present"), HasArrayField(ModelJson, TEXT("emitters")));
    }

    System->RemoveFromRoot();
    return true;
}
