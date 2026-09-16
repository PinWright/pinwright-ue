// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/Niagara/NiagaraGraphResetUtils.h"
#include "AssetDumpTestHelpers.h"
#include "NiagaraJsonAssertionHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"


#include "HAL/FileManager.h"
#include "Math/Float16.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_Niagara.h"
#include "EdGraph/EdGraphPin.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "Misc/EngineVersionComparison.h"
#include "NiagaraSystem.h"
// NiagaraTypeRegistry.h was split out of NiagaraTypes.h in UE 5.6; on UE 5.4 the class lives in NiagaraTypes.h below.
#if __has_include("NiagaraTypeRegistry.h")
#include "NiagaraTypeRegistry.h"
#endif
#include "NiagaraTypes.h"
#include "UObject/Package.h"

namespace
{
    using AssetDumpTestHelpers::HasDumpFile;
    using NiagaraJsonAssertionHelpers::HasArrayField;
    using NiagaraJsonAssertionHelpers::ArrayFieldNum;
    using NiagaraJsonAssertionHelpers::IssuesContainCode;

    FString MakeUniqueTestAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UNiagaraSystem* NewTransientNiagaraSystem(FString& OutObjectPath)
    {
        const FString AssetName = MakeUniqueTestAssetName(TEXT("NS_DumpBuilder"));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UNiagaraSystem* System = NewObject<UNiagaraSystem>(
            Package,
            UNiagaraSystem::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (System)
        {
            System->AddToRoot();
            OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        }
        return System;
    }

    UNiagaraEmitter* NewTransientNiagaraEmitter(FString& OutObjectPath)
    {
        const FString AssetName = MakeUniqueTestAssetName(TEXT("NE_DumpBuilder"));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(
            Package,
            UNiagaraEmitter::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (Emitter)
        {
            Emitter->AddToRoot();
            OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        }
        return Emitter;
    }

    UNiagaraScript* NewTransientNiagaraScript(FString& OutObjectPath)
    {
        const FString AssetName = MakeUniqueTestAssetName(TEXT("NM_DumpBuilder"));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UNiagaraScript* Script = NewObject<UNiagaraScript>(
            Package,
            UNiagaraScript::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (Script)
        {
            Script->SetUsage(ENiagaraScriptUsage::Module);
            Script->SetUsageId(FGuid::NewGuid());
            Script->AddToRoot();
            OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        }
        return Script;
    }

    bool GraphsHaveNodeWithPin(const TSharedPtr<FJsonObject>& GraphsJson)
    {
        const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
        if (!GraphsJson.IsValid() || !GraphsJson->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
        {
            TSharedPtr<FJsonObject> Graph = GraphValue.IsValid() ? GraphValue->AsObject() : nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
            if (!Graph.IsValid() || !Graph->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
            {
                continue;
            }

            for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
            {
                TSharedPtr<FJsonObject> Node = NodeValue.IsValid() ? NodeValue->AsObject() : nullptr;
                if (ArrayFieldNum(Node, TEXT("pins")) > 0)
                {
                    return true;
                }
            }
        }
        return false;
    }

    void AttachRepresentativeSystemGraph(UNiagaraSystem* System)
    {
        if (!System || !System->GetSystemSpawnScript() || !System->GetSystemUpdateScript())
        {
            return;
        }

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            System->GetSystemSpawnScript(),
            TEXT("DumpFixtureSystemScriptSource"),
            RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("DumpFixtureSystemScriptGraph"), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Graph)
        {
            return;
        }

        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;
        System->GetSystemSpawnScript()->SetLatestSource(Source);
        System->GetSystemUpdateScript()->SetLatestSource(Source);

        UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, UEdGraphNode::StaticClass(), TEXT("DumpFixtureNode"), RF_Transient | RF_Transactional);
        if (Node)
        {
            Node->NodeGuid = FGuid::NewGuid();
            Graph->AddNode(Node, false, false);
            Node->CreatePin(
                EGPD_Input,
                UEdGraphSchema_Niagara::TypeDefinitionToPinType(FNiagaraTypeDefinition::GetFloatDef()),
                FName(TEXT("FixtureFloat")));
        }
    }

    void AttachRepresentativeEmitterSource(UNiagaraEmitter* Emitter)
    {
        FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
        if (!EmitterData || EmitterData->GraphSource)
        {
            return;
        }

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Emitter, TEXT("DumpFixtureEmitterSource"), RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("DumpFixtureEmitterGraph"), RF_Transient | RF_Transactional)
            : nullptr;
        if (Graph)
        {
            Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
            Source->NodeGraph = Graph;
            EmitterData->GraphSource = Source;
        }
    }

    void AttachRepresentativeScriptSource(UNiagaraScript* Script)
    {
        if (!Script)
        {
            return;
        }

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            Script,
            TEXT("DumpFixtureStandaloneScriptSource"),
            RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("DumpFixtureStandaloneScriptGraph"), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Graph)
        {
            return;
        }

        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;
        Script->SetLatestSource(Source);
    }

    bool AttachCpuOnlyModuleToScript(UNiagaraScript* Script)
    {
        if (!Script)
        {
            return false;
        }

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            Script,
            TEXT("DumpFixtureCpuOnlySource"),
            RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("DumpFixtureCpuOnlyGraph"), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Graph)
        {
            return false;
        }

        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;
        Script->SetLatestSource(Source);

        UNiagaraNodeFunctionCall* ModuleNode = NewObject<UNiagaraNodeFunctionCall>(
            Graph,
            UNiagaraNodeFunctionCall::StaticClass(),
            TEXT("DumpCpuOnlyModuleNode"),
            RF_Transient | RF_Transactional);
        if (!ModuleNode)
        {
            return false;
        }

        ModuleNode->NodeGuid = FGuid::NewGuid();
        ModuleNode->NodePosX = 100;
        ModuleNode->NodePosY = 200;
        ModuleNode->Signature.Name = FName(TEXT("DumpCpuOnlyModule"));
        ModuleNode->Signature.bSupportsGPU = false;
        ModuleNode->Signature.Outputs.Add(FNiagaraVariableBase(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("Out"))));
        Graph->AddNode(ModuleNode, false, false);
        return true;
    }

    bool ConfigureGpuEmitterWithCpuOnlyModule(UNiagaraEmitter* Emitter)
    {
        FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
        if (!EmitterData)
        {
            return false;
        }

        if (!EmitterData->SpawnScriptProps.Script)
        {
            EmitterData->SpawnScriptProps.Script = NewObject<UNiagaraScript>(
                Emitter,
                TEXT("DumpFixtureParticleSpawnScript"),
                RF_Transient | RF_Transactional);
            if (!EmitterData->SpawnScriptProps.Script)
            {
                return false;
            }
            EmitterData->SpawnScriptProps.Script->SetUsage(ENiagaraScriptUsage::ParticleSpawnScript);
            EmitterData->SpawnScriptProps.Script->SetUsageId(FGuid::NewGuid());
        }

        EmitterData->SimTarget = ENiagaraSimTarget::GPUComputeSim;
        return AttachCpuOnlyModuleToScript(EmitterData->SpawnScriptProps.Script);
    }

    void PopulateRepresentativeSystemFixture(UNiagaraSystem* System)
    {
        AttachRepresentativeSystemGraph(System);

        UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(
            System,
            FName(TEXT("DumpFixtureEmitterAsset")),
            RF_Transient | RF_Transactional);
        if (Emitter)
        {
            AttachRepresentativeEmitterSource(Emitter);
            System->AddEmitterHandle(*Emitter, FName(TEXT("DumpFixtureEmitter")), Emitter->GetExposedVersion().VersionGuid);
        }

        const FNiagaraVariable UserFloat(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("User.DumpFixtureFloat")));
        System->GetExposedParameters().SetParameterValue(12.5f, UserFloat, true);
    }

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraDumpBuilderSystemAspectsShapeTest,
    "PinWright.Assets.Niagara.DumpBuilder.SystemAspectsShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpBuilderSystemAspectsShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientNiagaraSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    PopulateRepresentativeSystemFixture(System);

    TSharedPtr<FJsonObject> SystemJson = NiagaraDumpBuilder::BuildSystemJson(System);
    TestTrue(TEXT("system aspect has present flag"), SystemJson->GetBoolField(TEXT("present")));
    TestEqual(TEXT("system assetKind"), SystemJson->GetStringField(TEXT("assetKind")), FString(TEXT("NiagaraSystem")));
    TestEqual(TEXT("system path"), SystemJson->GetStringField(TEXT("path")), ObjectPath);
    TestTrue(TEXT("system aspect has emitterHandles array"), HasArrayField(SystemJson, TEXT("emitterHandles")));
    TestEqual(TEXT("system fixture has one emitter handle"), ArrayFieldNum(SystemJson, TEXT("emitterHandles")), 1);

    TSharedPtr<FJsonObject> EmittersJson = NiagaraDumpBuilder::BuildEmittersJson(System);
    TestEqual(TEXT("emitters assetKind"), EmittersJson->GetStringField(TEXT("assetKind")), FString(TEXT("NiagaraSystem")));
    TestEqual(TEXT("emitters systemPath"), EmittersJson->GetStringField(TEXT("systemPath")), ObjectPath);
    TestTrue(TEXT("emitters aspect has emitters array"), HasArrayField(EmittersJson, TEXT("emitters")));
    TestEqual(TEXT("emitters aspect includes fixture emitter"), ArrayFieldNum(EmittersJson, TEXT("emitters")), 1);

    TSharedPtr<FJsonObject> ParametersJson = NiagaraDumpBuilder::BuildParametersJson(System);
    TestTrue(TEXT("parameters aspect has user array"), HasArrayField(ParametersJson, TEXT("user")));
    TestTrue(TEXT("parameters aspect has emitters array"), HasArrayField(ParametersJson, TEXT("emitters")));
    TestTrue(TEXT("parameters aspect includes fixture user parameter"), ArrayFieldNum(ParametersJson, TEXT("user")) >= 1);

    TSharedPtr<FJsonObject> StackJson = NiagaraDumpBuilder::BuildStackJson(System);
    TestTrue(TEXT("stack aspect has modules array"), HasArrayField(StackJson, TEXT("modules")));

    TSharedPtr<FJsonObject> GraphsJson = NiagaraDumpBuilder::BuildGraphsJson(System);
    TestTrue(TEXT("graphs aspect has graphs array"), HasArrayField(GraphsJson, TEXT("graphs")));
    TestTrue(TEXT("graphs aspect includes representative node/pin data"), GraphsHaveNodeWithPin(GraphsJson));

    TSharedPtr<FJsonObject> CompileJson = NiagaraDumpBuilder::BuildCompileJson(System);
    TestTrue(TEXT("compile aspect has scripts array"), HasArrayField(CompileJson, TEXT("scripts")));
    TestTrue(TEXT("compile aspect has issues array"), HasArrayField(CompileJson, TEXT("issues")));
    TestFalse(TEXT("authored compile aspect omits live valid state"), CompileJson->HasField(TEXT("valid")));
    TestFalse(TEXT("authored compile aspect omits outstanding compilation state"), CompileJson->HasField(TEXT("hasOutstandingCompilationRequests")));
    TestFalse(TEXT("authored compile aspect omits active compilation state"), CompileJson->HasField(TEXT("hasActiveCompilations")));
    TestFalse(TEXT("authored compile aspect omits ready state"), CompileJson->HasField(TEXT("readyToRun")));
    TestFalse(TEXT("authored compile aspect omits global deferred-compile state"), CompileJson->HasField(TEXT("compileDeferredOnLoad")));

    const TArray<TSharedPtr<FJsonValue>>* CompileScripts = nullptr;
    if (CompileJson->TryGetArrayField(TEXT("scripts"), CompileScripts) && CompileScripts)
    {
        for (const TSharedPtr<FJsonValue>& ScriptValue : *CompileScripts)
        {
            const TSharedPtr<FJsonObject> ScriptObject = ScriptValue.IsValid() ? ScriptValue->AsObject() : nullptr;
            if (ScriptObject.IsValid())
            {
                TestFalse(TEXT("authored script entry omits live compile status"),
                    ScriptObject->HasField(TEXT("compileStatus")));
            }
        }
    }

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraDumpBuilderEmitterAspectsShapeTest,
    "PinWright.Assets.Niagara.DumpBuilder.EmitterAspectsShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpBuilderEmitterAspectsShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraEmitter* Emitter = NewTransientNiagaraEmitter(ObjectPath);
    TestNotNull(TEXT("Transient Niagara emitter created"), Emitter);
    if (!Emitter)
    {
        return false;
    }

    TSharedPtr<FJsonObject> EmitterJson = NiagaraDumpBuilder::BuildEmitterAssetJson(Emitter);
    TestEqual(TEXT("emitter assetKind"), EmitterJson->GetStringField(TEXT("assetKind")), FString(TEXT("NiagaraEmitter")));
    TestTrue(TEXT("emitter aspect has emitter object"), EmitterJson->HasTypedField<EJson::Object>(TEXT("emitter")));

    TSharedPtr<FJsonObject> ParametersJson = NiagaraDumpBuilder::BuildEmitterParametersJson(Emitter);
    TestEqual(TEXT("emitter parameters assetKind"), ParametersJson->GetStringField(TEXT("assetKind")), FString(TEXT("NiagaraEmitter")));
    TestEqual(TEXT("emitter parameters path"), ParametersJson->GetStringField(TEXT("emitterPath")), ObjectPath);
    TestTrue(TEXT("emitter parameters has rendererBindings array"), HasArrayField(ParametersJson, TEXT("rendererBindings")));

    TSharedPtr<FJsonObject> StackJson = NiagaraDumpBuilder::BuildEmitterStackJson(Emitter);
    TestTrue(TEXT("emitter stack has modules array"), HasArrayField(StackJson, TEXT("modules")));

    TSharedPtr<FJsonObject> GraphsJson = NiagaraDumpBuilder::BuildEmitterGraphsJson(Emitter);
    TestTrue(TEXT("emitter graphs has graphs array"), HasArrayField(GraphsJson, TEXT("graphs")));

    TSharedPtr<FJsonObject> CompileJson = NiagaraDumpBuilder::BuildEmitterCompileJson(Emitter);
    TestTrue(TEXT("emitter compile has scripts array"), HasArrayField(CompileJson, TEXT("scripts")));
    TestTrue(TEXT("emitter compile has issues array"), HasArrayField(CompileJson, TEXT("issues")));
    TestFalse(TEXT("authored emitter compile omits readyToRun"), CompileJson->HasField(TEXT("readyToRun")));

    Emitter->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraDumpBuilderAssetDumpWritesNiagaraAspectFilesTest,
    "PinWright.Assets.Niagara.AssetDump.WritesNiagaraAspectFiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpBuilderAssetDumpWritesNiagaraAspectFilesTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientNiagaraSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    PopulateRepresentativeSystemFixture(System);

    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("NiagaraDumpBuilderTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for transient Niagara system"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("properties.json remains present"), HasDumpFile(Result.WrittenPaths, DumpFileNames::Properties));
    TestFalse(TEXT("niagara_model.json is not written"), HasDumpFile(Result.WrittenPaths, TEXT("niagara_model.json")));
    TestFalse(TEXT("niagara_system.json is not written"), HasDumpFile(Result.WrittenPaths, TEXT("niagara_system.json")));
    TestFalse(TEXT("niagara_emitters.json is not written"), HasDumpFile(Result.WrittenPaths, TEXT("niagara_emitters.json")));
    TestFalse(TEXT("niagara_parameters.json is not written"), HasDumpFile(Result.WrittenPaths, DumpFileNames::NiagaraParameters));
    TestFalse(TEXT("niagara_stack.json is not written"), HasDumpFile(Result.WrittenPaths, DumpFileNames::NiagaraStack));
    TestFalse(TEXT("niagara_graphs.json is not written"), HasDumpFile(Result.WrittenPaths, DumpFileNames::NiagaraGraphs));
    TestTrue(TEXT("niagara_compile.json is written"), HasDumpFile(Result.WrittenPaths, DumpFileNames::NiagaraCompile));
    TestTrue(TEXT("nir.txt is written"), HasDumpFile(Result.WrittenPaths, DumpFileNames::Nir));
    TestTrue(TEXT("Niagara dump has more than generic meta/properties"), Result.WrittenPaths.Num() > 2);

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    System->RemoveFromRoot();
    return true;
}

// Regression test for F-remove-niagara-json-sidecars:
// standalone UNiagaraEmitter dumps keep NIR plus niagara_compile.json,
// but no longer write the niagara_model / niagara_emitters / niagara_parameters /
// niagara_stack / niagara_graphs JSON sidecars.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpStandaloneEmitterScopedNiagaraFilesTest,
    "PinWright.Asset.Dump.StandaloneEmitter.ScopedNiagaraFiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDumpStandaloneEmitterScopedNiagaraFilesTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraEmitter* Emitter = NewTransientNiagaraEmitter(ObjectPath);
    TestNotNull(TEXT("Transient Niagara emitter created"), Emitter);
    if (!Emitter)
    {
        return false;
    }

    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("NiagaraDumpBuilderTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for transient standalone NiagaraEmitter"), Result.ErrorCode.IsEmpty());
    TestFalse(TEXT("standalone NiagaraEmitter dump does NOT write niagara_model.json"),
        HasDumpFile(Result.WrittenPaths, TEXT("niagara_model.json")));
    TestFalse(TEXT("standalone NiagaraEmitter dump does NOT write niagara_system.json"),
        HasDumpFile(Result.WrittenPaths, TEXT("niagara_system.json")));
    TestFalse(TEXT("standalone NiagaraEmitter dump does NOT write niagara_emitters.json"),
        HasDumpFile(Result.WrittenPaths, TEXT("niagara_emitters.json")));
    TestFalse(TEXT("standalone NiagaraEmitter dump does NOT write niagara_parameters.json"),
        HasDumpFile(Result.WrittenPaths, DumpFileNames::NiagaraParameters));
    TestFalse(TEXT("standalone NiagaraEmitter dump does NOT write niagara_stack.json"),
        HasDumpFile(Result.WrittenPaths, DumpFileNames::NiagaraStack));
    TestFalse(TEXT("standalone NiagaraEmitter dump does NOT write niagara_graphs.json"),
        HasDumpFile(Result.WrittenPaths, DumpFileNames::NiagaraGraphs));
    TestTrue(TEXT("standalone NiagaraEmitter dump writes niagara_compile.json"),
        HasDumpFile(Result.WrittenPaths, DumpFileNames::NiagaraCompile));
    TestTrue(TEXT("standalone NiagaraEmitter dump writes nir.txt"),
        HasDumpFile(Result.WrittenPaths, DumpFileNames::Nir));

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    Emitter->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraDumpBuilderGpuIncompatibleModuleCompileIssueTest,
    "PinWright.Assets.Niagara.DumpBuilder.GpuIncompatibleModuleCompileIssue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpBuilderGpuIncompatibleModuleCompileIssueTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraEmitter* Emitter = NewTransientNiagaraEmitter(ObjectPath);
    TestNotNull(TEXT("Transient Niagara emitter created"), Emitter);
    if (!Emitter)
    {
        return false;
    }

    TestTrue(TEXT("GPU emitter fixture has CPU-only module"), ConfigureGpuEmitterWithCpuOnlyModule(Emitter));

    TSharedPtr<FJsonObject> CompileJson = NiagaraDumpBuilder::BuildEmitterCompileJson(Emitter);
    TestTrue(TEXT("niagara_compile.json issues include GPU_INCOMPATIBLE_MODULE"),
        IssuesContainCode(CompileJson, TEXT("GPU_INCOMPATIBLE_MODULE")));

    Emitter->RemoveFromRoot();
    return true;
}

// Counterfactual: if the UNiagaraScript branch in AssetDumpHandler.cpp is reverted,
// the asset falls through the generic path and both sidecar assertions fail because
// Result.WrittenPaths lacks niagara_graphs.json and niagara_compile.json.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpStandaloneNiagaraScriptWritesGraphAndCompileSidecarsTest,
    "PinWright.Asset.Dump.StandaloneNiagaraScript.WritesGraphAndCompileSidecars",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDumpStandaloneNiagaraScriptWritesGraphAndCompileSidecarsTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraScript* Script = NewTransientNiagaraScript(ObjectPath);
    TestNotNull(TEXT("Transient Niagara script created"), Script);
    if (!Script)
    {
        return false;
    }

    AttachRepresentativeScriptSource(Script);

    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("NiagaraDumpBuilderTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for transient standalone NiagaraScript"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("standalone NiagaraScript dump writes niagara_graphs.json"), HasDumpFile(Result.WrittenPaths, DumpFileNames::NiagaraGraphs));
    TestTrue(TEXT("standalone NiagaraScript dump writes niagara_compile.json"), HasDumpFile(Result.WrittenPaths, DumpFileNames::NiagaraCompile));
    TestTrue(TEXT("standalone NiagaraScript dump writes nir.txt"), HasDumpFile(Result.WrittenPaths, DumpFileNames::Nir));
    TestFalse(TEXT("standalone NiagaraScript dump does NOT write niagara_system.json"),
        HasDumpFile(Result.WrittenPaths, TEXT("niagara_system.json")));
    TestFalse(TEXT("standalone NiagaraScript dump does NOT write niagara_emitters.json"),
        HasDumpFile(Result.WrittenPaths, TEXT("niagara_emitters.json")));
    TestFalse(TEXT("standalone NiagaraScript dump does NOT write niagara_parameters.json"),
        HasDumpFile(Result.WrittenPaths, DumpFileNames::NiagaraParameters));
    TestFalse(TEXT("standalone NiagaraScript dump does NOT write niagara_stack.json"),
        HasDumpFile(Result.WrittenPaths, DumpFileNames::NiagaraStack));

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    Script->RemoveFromRoot();
    return true;
}

// Regression test for B-niagara-bool-vec2-struct-rawbytes:
// Verifies typed parameter dump cases for bool, Vec2, FNiagaraID, FNiagaraSpawnInfo,
// half / half2 / half3 / half4. Each parameter must emerge in its typed JSON shape
// (bool -> JSON true/false, Vec2 -> {x,y}, FNiagaraID -> {index, acquireTag}, etc.).
//
// Counterfactual: if any of the typed cases in BuildParameterValueJson is reverted,
// the corresponding assertion fails because the parameter emerges as
// { "_kind": "rawBytes", "sizeBytes": N, "hex": "..." } (the fallback) instead.
namespace
{
    TSharedPtr<FJsonObject> FindUserParameterEntry(const TSharedPtr<FJsonObject>& ParametersJson, const FString& Name)
    {
        const TArray<TSharedPtr<FJsonValue>>* Users = nullptr;
        if (!ParametersJson.IsValid() || !ParametersJson->TryGetArrayField(TEXT("user"), Users) || !Users)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& EntryValue : *Users)
        {
            TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
            if (Entry.IsValid() && Entry->GetStringField(TEXT("name")).Equals(Name))
            {
                return Entry;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraDumpBuilderTypedParameterCoverageTest,
    "PinWright.niagara.dump_builder.TypedParameterCoverage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpBuilderTypedParameterCoverageTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientNiagaraSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    FNiagaraParameterStore& Store = System->GetExposedParameters();

    // Bool
    const FNiagaraVariable BoolVar(FNiagaraTypeDefinition::GetBoolDef(), FName(TEXT("User.TestBool")));
    const FNiagaraBool BoolValue(true);
    Store.SetParameterValue(BoolValue, BoolVar, /*bAdd=*/true);

    // Vec2
    const FNiagaraVariable Vec2Var(FNiagaraTypeDefinition::GetVec2Def(), FName(TEXT("User.TestVec2")));
    const FVector2f Vec2Value(1.5f, 2.5f);
    Store.SetParameterValue(Vec2Value, Vec2Var, /*bAdd=*/true);

    // FNiagaraID
    const FNiagaraVariable IdVar(FNiagaraTypeDefinition::GetIDDef(), FName(TEXT("User.TestId")));
    const FNiagaraID IdValue(7, 42);
    Store.SetParameterValue(IdValue, IdVar, /*bAdd=*/true);

    // FNiagaraSpawnInfo
    const FNiagaraVariable SpawnVar(FNiagaraTypeDefinition(FNiagaraSpawnInfo::StaticStruct()), FName(TEXT("User.TestSpawn")));
    const FNiagaraSpawnInfo SpawnValue(11, 0.25f, 0.5f, 3);
    Store.SetParameterValue(SpawnValue, SpawnVar, /*bAdd=*/true);

    // half
    const FNiagaraVariable HalfVar(FNiagaraTypeDefinition::GetHalfDef(), FName(TEXT("User.TestHalf")));
    FFloat16 HalfEncoded; HalfEncoded.Set(0.5f);
    FNiagaraHalf HalfValue; HalfValue.Value = HalfEncoded.Encoded;
    Store.SetParameterValue(HalfValue, HalfVar, /*bAdd=*/true);

    // half2
    const FNiagaraVariable Half2Var(FNiagaraTypeDefinition::GetHalfVec2Def(), FName(TEXT("User.TestHalf2")));
    FFloat16 Half2X; Half2X.Set(0.25f);
    FFloat16 Half2Y; Half2Y.Set(0.75f);
    FNiagaraHalfVector2 Half2Value; Half2Value.x = Half2X.Encoded; Half2Value.y = Half2Y.Encoded;
    Store.SetParameterValue(Half2Value, Half2Var, /*bAdd=*/true);

    // half3
    const FNiagaraVariable Half3Var(FNiagaraTypeDefinition::GetHalfVec3Def(), FName(TEXT("User.TestHalf3")));
    FFloat16 Half3X; Half3X.Set(1.0f);
    FFloat16 Half3Y; Half3Y.Set(2.0f);
    FFloat16 Half3Z; Half3Z.Set(3.0f);
    FNiagaraHalfVector3 Half3Value; Half3Value.x = Half3X.Encoded; Half3Value.y = Half3Y.Encoded; Half3Value.z = Half3Z.Encoded;
    Store.SetParameterValue(Half3Value, Half3Var, /*bAdd=*/true);

    // half4
    const FNiagaraVariable Half4Var(FNiagaraTypeDefinition::GetHalfVec4Def(), FName(TEXT("User.TestHalf4")));
    FFloat16 Half4X; Half4X.Set(0.5f);
    FFloat16 Half4Y; Half4Y.Set(1.0f);
    FFloat16 Half4Z; Half4Z.Set(1.5f);
    FFloat16 Half4W; Half4W.Set(2.0f);
    FNiagaraHalfVector4 Half4Value;
    Half4Value.x = Half4X.Encoded; Half4Value.y = Half4Y.Encoded;
    Half4Value.z = Half4Z.Encoded; Half4Value.w = Half4W.Encoded;
    Store.SetParameterValue(Half4Value, Half4Var, /*bAdd=*/true);

    TSharedPtr<FJsonObject> ParametersJson = NiagaraDumpBuilder::BuildParametersJson(System);
    TestTrue(TEXT("parameters json is valid"), ParametersJson.IsValid());

    // Bool -> JSON boolean true/false (NOT _kind: rawBytes).
    {
        TSharedPtr<FJsonObject> Entry = FindUserParameterEntry(ParametersJson, TEXT("User.TestBool"));
        TestNotNull(TEXT("bool entry present"), Entry.Get());
        if (Entry.IsValid())
        {
            TSharedPtr<FJsonValue> ValueField = Entry->TryGetField(TEXT("value"));
            TestTrue(TEXT("bool value field is JSON boolean"), ValueField.IsValid() && ValueField->Type == EJson::Boolean);
            if (ValueField.IsValid() && ValueField->Type == EJson::Boolean)
            {
                TestTrue(TEXT("bool value is true"), ValueField->AsBool());
            }
        }
    }

    // Vec2 -> {x, y}
    {
        TSharedPtr<FJsonObject> Entry = FindUserParameterEntry(ParametersJson, TEXT("User.TestVec2"));
        TestNotNull(TEXT("vec2 entry present"), Entry.Get());
        if (Entry.IsValid())
        {
            const TSharedPtr<FJsonObject>* ValueObj = nullptr;
            const bool bIsObject = Entry->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid();
            TestTrue(TEXT("vec2 value is JSON object"), bIsObject);
            if (bIsObject)
            {
                TestFalse(TEXT("vec2 value is NOT _kind:rawBytes"), (*ValueObj)->HasField(TEXT("_kind")));
                TestEqual(TEXT("vec2.x"), (*ValueObj)->GetNumberField(TEXT("x")), 1.5);
                TestEqual(TEXT("vec2.y"), (*ValueObj)->GetNumberField(TEXT("y")), 2.5);
            }
        }
    }

    // FNiagaraID -> {index, acquireTag}
    {
        TSharedPtr<FJsonObject> Entry = FindUserParameterEntry(ParametersJson, TEXT("User.TestId"));
        TestNotNull(TEXT("niagara id entry present"), Entry.Get());
        if (Entry.IsValid())
        {
            const TSharedPtr<FJsonObject>* ValueObj = nullptr;
            const bool bIsObject = Entry->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid();
            TestTrue(TEXT("niagara id value is JSON object"), bIsObject);
            if (bIsObject)
            {
                TestFalse(TEXT("niagara id is NOT _kind:rawBytes"), (*ValueObj)->HasField(TEXT("_kind")));
                TestEqual(TEXT("niagara id index"), static_cast<int32>((*ValueObj)->GetNumberField(TEXT("index"))), 7);
                TestEqual(TEXT("niagara id acquireTag"), static_cast<int32>((*ValueObj)->GetNumberField(TEXT("acquireTag"))), 42);
            }
        }
    }

    // FNiagaraSpawnInfo -> {count, interpStartDt, intervalDt, spawnGroup}
    {
        TSharedPtr<FJsonObject> Entry = FindUserParameterEntry(ParametersJson, TEXT("User.TestSpawn"));
        TestNotNull(TEXT("spawn info entry present"), Entry.Get());
        if (Entry.IsValid())
        {
            const TSharedPtr<FJsonObject>* ValueObj = nullptr;
            const bool bIsObject = Entry->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid();
            TestTrue(TEXT("spawn info value is JSON object"), bIsObject);
            if (bIsObject)
            {
                TestFalse(TEXT("spawn info is NOT _kind:rawBytes"), (*ValueObj)->HasField(TEXT("_kind")));
                TestEqual(TEXT("spawn info count"), static_cast<int32>((*ValueObj)->GetNumberField(TEXT("count"))), 11);
                TestEqual(TEXT("spawn info interpStartDt"), (*ValueObj)->GetNumberField(TEXT("interpStartDt")), 0.25);
                TestEqual(TEXT("spawn info intervalDt"), (*ValueObj)->GetNumberField(TEXT("intervalDt")), 0.5);
                TestEqual(TEXT("spawn info spawnGroup"), static_cast<int32>((*ValueObj)->GetNumberField(TEXT("spawnGroup"))), 3);
            }
        }
    }

    // half -> JSON number
    {
        TSharedPtr<FJsonObject> Entry = FindUserParameterEntry(ParametersJson, TEXT("User.TestHalf"));
        TestNotNull(TEXT("half entry present"), Entry.Get());
        if (Entry.IsValid())
        {
            TSharedPtr<FJsonValue> ValueField = Entry->TryGetField(TEXT("value"));
            TestTrue(TEXT("half value is JSON number"), ValueField.IsValid() && ValueField->Type == EJson::Number);
            if (ValueField.IsValid() && ValueField->Type == EJson::Number)
            {
                TestEqual(TEXT("half value"), ValueField->AsNumber(), 0.5);
            }
        }
    }

    // half2 -> {x, y}
    {
        TSharedPtr<FJsonObject> Entry = FindUserParameterEntry(ParametersJson, TEXT("User.TestHalf2"));
        TestNotNull(TEXT("half2 entry present"), Entry.Get());
        if (Entry.IsValid())
        {
            const TSharedPtr<FJsonObject>* ValueObj = nullptr;
            const bool bIsObject = Entry->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid();
            TestTrue(TEXT("half2 value is JSON object"), bIsObject);
            if (bIsObject)
            {
                TestFalse(TEXT("half2 is NOT _kind:rawBytes"), (*ValueObj)->HasField(TEXT("_kind")));
                TestEqual(TEXT("half2.x"), (*ValueObj)->GetNumberField(TEXT("x")), 0.25);
                TestEqual(TEXT("half2.y"), (*ValueObj)->GetNumberField(TEXT("y")), 0.75);
            }
        }
    }

    // half3 -> {x, y, z}
    {
        TSharedPtr<FJsonObject> Entry = FindUserParameterEntry(ParametersJson, TEXT("User.TestHalf3"));
        TestNotNull(TEXT("half3 entry present"), Entry.Get());
        if (Entry.IsValid())
        {
            const TSharedPtr<FJsonObject>* ValueObj = nullptr;
            const bool bIsObject = Entry->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid();
            TestTrue(TEXT("half3 value is JSON object"), bIsObject);
            if (bIsObject)
            {
                TestFalse(TEXT("half3 is NOT _kind:rawBytes"), (*ValueObj)->HasField(TEXT("_kind")));
                TestEqual(TEXT("half3.x"), (*ValueObj)->GetNumberField(TEXT("x")), 1.0);
                TestEqual(TEXT("half3.y"), (*ValueObj)->GetNumberField(TEXT("y")), 2.0);
                TestEqual(TEXT("half3.z"), (*ValueObj)->GetNumberField(TEXT("z")), 3.0);
            }
        }
    }

    // half4 -> {x, y, z, w}
    {
        TSharedPtr<FJsonObject> Entry = FindUserParameterEntry(ParametersJson, TEXT("User.TestHalf4"));
        TestNotNull(TEXT("half4 entry present"), Entry.Get());
        if (Entry.IsValid())
        {
            const TSharedPtr<FJsonObject>* ValueObj = nullptr;
            const bool bIsObject = Entry->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid();
            TestTrue(TEXT("half4 value is JSON object"), bIsObject);
            if (bIsObject)
            {
                TestFalse(TEXT("half4 is NOT _kind:rawBytes"), (*ValueObj)->HasField(TEXT("_kind")));
                TestEqual(TEXT("half4.x"), (*ValueObj)->GetNumberField(TEXT("x")), 0.5);
                TestEqual(TEXT("half4.y"), (*ValueObj)->GetNumberField(TEXT("y")), 1.0);
                TestEqual(TEXT("half4.z"), (*ValueObj)->GetNumberField(TEXT("z")), 1.5);
                TestEqual(TEXT("half4.w"), (*ValueObj)->GetNumberField(TEXT("w")), 2.0);
            }
        }
    }

    System->RemoveFromRoot();
    return true;
}

// Regression test for B-niagara-move-module-noop:
// niagara.inspect's includeStack readback must report module `index` in
// ParameterMap execution-chain order (the order move_module edits), NOT a sort by
// the visual NodePosY. The fixture chains three module nodes C -> B -> A -> Output
// (so execution order is C, B, A) while assigning NodePosY in the OPPOSITE order
// (A=0, B=100, C=200). The old NodePosY sort would emit A, B, C; the chain walk
// emits C, B, A. We key the assertion on entryId (NodeGuid), which the readback
// exposes verbatim, and confirm posY runs counter to index — proving the order is
// derived from connections, not position.
//
// Counterfactual: if AddGraphStackModules (NiagaraDumpBuilder.cpp) reverts to the
// NodePosY sort, the readback emits A, B, C and the index-order assertions fail.
namespace
{
    UEdGraphPin* AddParameterMapPin(UNiagaraNode* Node, EEdGraphPinDirection Direction, const TCHAR* PinName)
    {
        if (!Node)
        {
            return nullptr;
        }
        return Node->CreatePin(
            Direction,
            UEdGraphSchema_Niagara::TypeDefinitionToPinType(FNiagaraTypeDefinition::GetParameterMapDef()),
            FName(PinName));
    }

    UNiagaraNodeFunctionCall* AddChainedModuleNode(UNiagaraGraph* Graph, const TCHAR* NodeName, int32 PosY)
    {
        UNiagaraNodeFunctionCall* Node = NewObject<UNiagaraNodeFunctionCall>(
            Graph,
            UNiagaraNodeFunctionCall::StaticClass(),
            NodeName,
            RF_Transient | RF_Transactional);
        if (!Node)
        {
            return nullptr;
        }
        Node->NodeGuid = FGuid::NewGuid();
        Node->NodePosX = 0;
        Node->NodePosY = PosY;
        Node->Signature.Name = FName(NodeName);
        AddParameterMapPin(Node, EGPD_Input, TEXT("InputMap"));
        AddParameterMapPin(Node, EGPD_Output, TEXT("OutputMap"));
        Graph->AddNode(Node, false, false);
        return Node;
    }

    void LinkParameterMap(UNiagaraNode* From, UNiagaraNode* To)
    {
        // From's OutputMap pin -> To's InputMap pin.
        UEdGraphPin* FromOut = nullptr;
        UEdGraphPin* ToIn = nullptr;
        TArray<UEdGraphPin*> OutPins;
        From->GetOutputPins(OutPins);
        FromOut = PinWrightNiagara::FindParameterMapPin(OutPins);
        TArray<UEdGraphPin*> InPins;
        To->GetInputPins(InPins);
        ToIn = PinWrightNiagara::FindParameterMapPin(InPins);
        if (FromOut && ToIn)
        {
            FromOut->MakeLinkTo(ToIn);
        }
    }

    // Builds a ParticleUpdate script graph on Emitter chained C -> B -> A -> Output,
    // returning the per-node GUIDs in execution order (C, B, A).
    bool AttachChainedParticleUpdateStack(UNiagaraEmitter* Emitter, TArray<FString>& OutExecutionOrderGuids)
    {
        FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
        if (!EmitterData)
        {
            return false;
        }

        UNiagaraScript* Script = NewObject<UNiagaraScript>(
            Emitter,
            TEXT("ChainOrderParticleUpdateScript"),
            RF_Transient | RF_Transactional);
        if (!Script)
        {
            return false;
        }
        Script->SetUsage(ENiagaraScriptUsage::ParticleUpdateScript);
        Script->SetUsageId(FGuid::NewGuid());
        EmitterData->UpdateScriptProps.Script = Script;

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Script, TEXT("ChainOrderSource"), RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("ChainOrderGraph"), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Graph)
        {
            return false;
        }
        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;
        Script->SetLatestSource(Source);

        UNiagaraNodeOutput* OutputNode = NewObject<UNiagaraNodeOutput>(Graph, UNiagaraNodeOutput::StaticClass(), TEXT("ChainOutput"), RF_Transient | RF_Transactional);
        if (!OutputNode)
        {
            return false;
        }
        OutputNode->NodeGuid = FGuid::NewGuid();
        OutputNode->SetUsage(ENiagaraScriptUsage::ParticleUpdateScript);
        OutputNode->NodePosY = 1000;
        AddParameterMapPin(OutputNode, EGPD_Input, TEXT("InputMap"));
        Graph->AddNode(OutputNode, false, false);

        // Execution order C, B, A; NodePosY deliberately reversed (A=0, B=100, C=200).
        UNiagaraNodeFunctionCall* NodeA = AddChainedModuleNode(Graph, TEXT("ChainModuleA"), 0);
        UNiagaraNodeFunctionCall* NodeB = AddChainedModuleNode(Graph, TEXT("ChainModuleB"), 100);
        UNiagaraNodeFunctionCall* NodeC = AddChainedModuleNode(Graph, TEXT("ChainModuleC"), 200);
        if (!NodeA || !NodeB || !NodeC)
        {
            return false;
        }

        LinkParameterMap(NodeC, NodeB);
        LinkParameterMap(NodeB, NodeA);
        LinkParameterMap(NodeA, OutputNode);

        OutExecutionOrderGuids = { NodeC->NodeGuid.ToString(), NodeB->NodeGuid.ToString(), NodeA->NodeGuid.ToString() };
        return true;
    }

    bool CollectParticleUpdateStackEntries(
        const TSharedPtr<FJsonObject>& StackJson,
        const TArray<FString>& ChainGuids,
        TArray<FString>& OutGuidsByIndex,
        TArray<double>& OutPosYByIndex)
    {
        const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
        if (!StackJson.IsValid() || !StackJson->TryGetArrayField(TEXT("modules"), Modules) || !Modules)
        {
            return false;
        }

        // The readback lists modules from all four emitter scripts; restrict to the
        // chain we built. Modules are emitted in index order within each script, and
        // the chain GUIDs are unique, so filtering to them preserves the reported order.
        TSet<FString> ChainSet(ChainGuids);
        for (const TSharedPtr<FJsonValue>& ModuleValue : *Modules)
        {
            TSharedPtr<FJsonObject> Module = ModuleValue.IsValid() ? ModuleValue->AsObject() : nullptr;
            if (!Module.IsValid())
            {
                continue;
            }
            FString EntryId;
            if (Module->TryGetStringField(TEXT("entryId"), EntryId) && ChainSet.Contains(EntryId))
            {
                OutGuidsByIndex.Add(EntryId);
                OutPosYByIndex.Add(Module->GetNumberField(TEXT("posY")));
            }
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraDumpBuilderStackOrderFollowsExecutionChainTest,
    "PinWright.Assets.Niagara.DumpBuilder.StackOrderFollowsExecutionChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpBuilderStackOrderFollowsExecutionChainTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraEmitter* Emitter = NewTransientNiagaraEmitter(ObjectPath);
    TestNotNull(TEXT("Transient Niagara emitter created"), Emitter);
    if (!Emitter)
    {
        return false;
    }

    TArray<FString> ExecutionOrderGuids;
    const bool bAttached = AttachChainedParticleUpdateStack(Emitter, ExecutionOrderGuids);
    TestTrue(TEXT("Chained ParticleUpdate stack fixture attached"), bAttached);
    TestEqual(TEXT("Fixture built three chained modules"), ExecutionOrderGuids.Num(), 3);
    if (!bAttached || ExecutionOrderGuids.Num() != 3)
    {
        Emitter->RemoveFromRoot();
        return false;
    }

    TSharedPtr<FJsonObject> StackJson = NiagaraDumpBuilder::BuildEmitterStackJson(Emitter);
    TestTrue(TEXT("emitter stack has modules array"), HasArrayField(StackJson, TEXT("modules")));

    TArray<FString> GuidsByIndex;
    TArray<double> PosYByIndex;
    TestTrue(TEXT("collected chain module entries from stack readback"),
        CollectParticleUpdateStackEntries(StackJson, ExecutionOrderGuids, GuidsByIndex, PosYByIndex));
    TestEqual(TEXT("readback lists all three chained modules"), GuidsByIndex.Num(), 3);

    if (GuidsByIndex.Num() == 3)
    {
        // index order must equal the execution chain C, B, A — not the NodePosY sort A, B, C.
        TestEqual(TEXT("index 0 is execution-chain head (Module C)"), GuidsByIndex[0], ExecutionOrderGuids[0]);
        TestEqual(TEXT("index 1 is middle module (Module B)"), GuidsByIndex[1], ExecutionOrderGuids[1]);
        TestEqual(TEXT("index 2 is execution-chain tail (Module A)"), GuidsByIndex[2], ExecutionOrderGuids[2]);

        // posY runs counter to index, proving the order is NOT a NodePosY sort.
        TestTrue(TEXT("readback order is not a NodePosY sort (posY descends as index ascends)"),
            PosYByIndex[0] > PosYByIndex[1] && PosYByIndex[1] > PosYByIndex[2]);
    }

    Emitter->RemoveFromRoot();
    return true;
}

// Regression test for B-niagara-inspect-stack-modules-emitted-four-times:
// an emitter's four scripts share one UNiagaraScriptSource, so GetGraphFromScript hands back
// the SAME UNiagaraGraph for each of them, and that one graph holds the output nodes of all
// four stages. The stack readback used to walk it once per script and append the whole graph
// every time, so every module came back four times, byte-identical, with no field telling the
// copies apart. The fixture below builds exactly that shape — one shared source/graph, four
// output nodes (EmitterSpawn / EmitterUpdate / ParticleSpawn / ParticleUpdate), one module
// chained into each — so the old code emits 16 entries where 4 are distinct.
//
// Counterfactual: revert BuildEmitterStackArray to one AddGraphStackModules call per script
// and `modules` returns 16 entries with each entryId counted four times, failing both the
// array-length and the once-per-module assertions.
namespace
{
    // Creates a UNiagaraScript of Usage that shares Source, plus an output node of that usage in
    // Source's graph with one module chained into it. Returns the module node's GUID.
    FString AttachSharedSourceStage(UNiagaraEmitter* Emitter, UNiagaraScriptSource* Source, ENiagaraScriptUsage Usage, const TCHAR* BaseName, UNiagaraScript*& OutScript)
    {
        UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
        if (!Emitter || !Graph)
        {
            return FString();
        }

        UNiagaraScript* Script = NewObject<UNiagaraScript>(
            Emitter,
            *FString::Printf(TEXT("%sScript"), BaseName),
            RF_Transient | RF_Transactional);
        if (!Script)
        {
            return FString();
        }
        Script->SetUsage(Usage);
        Script->SetUsageId(FGuid::NewGuid());
        Script->SetLatestSource(Source);

        UNiagaraNodeOutput* OutputNode = NewObject<UNiagaraNodeOutput>(
            Graph,
            UNiagaraNodeOutput::StaticClass(),
            *FString::Printf(TEXT("%sOutput"), BaseName),
            RF_Transient | RF_Transactional);
        if (!OutputNode)
        {
            return FString();
        }
        OutputNode->NodeGuid = FGuid::NewGuid();
        OutputNode->SetUsage(Usage);
        OutputNode->NodePosY = 1000;
        AddParameterMapPin(OutputNode, EGPD_Input, TEXT("InputMap"));
        Graph->AddNode(OutputNode, false, false);

        UNiagaraNodeFunctionCall* ModuleNode = AddChainedModuleNode(Graph, *FString::Printf(TEXT("%sModule"), BaseName), 0);
        if (!ModuleNode)
        {
            return FString();
        }
        LinkParameterMap(ModuleNode, OutputNode);

        OutScript = Script;
        return ModuleNode->NodeGuid.ToString();
    }

    // Points all four of the emitter's scripts at one shared source whose single graph carries
    // one stage output node each. Fills OutUsageByModuleGuid with module GUID -> stage name.
    bool AttachSharedGraphFourStageStack(UNiagaraEmitter* Emitter, TMap<FString, FString>& OutUsageByModuleGuid)
    {
        FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
        if (!EmitterData)
        {
            return false;
        }

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Emitter, TEXT("SharedStageSource"), RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("SharedStageGraph"), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Graph)
        {
            return false;
        }
        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;

        UNiagaraScript* EmitterSpawn = nullptr;
        UNiagaraScript* EmitterUpdate = nullptr;
        UNiagaraScript* ParticleSpawn = nullptr;
        UNiagaraScript* ParticleUpdate = nullptr;
        const FString EmitterSpawnGuid = AttachSharedSourceStage(Emitter, Source, ENiagaraScriptUsage::EmitterSpawnScript, TEXT("SharedEmitterSpawn"), EmitterSpawn);
        const FString EmitterUpdateGuid = AttachSharedSourceStage(Emitter, Source, ENiagaraScriptUsage::EmitterUpdateScript, TEXT("SharedEmitterUpdate"), EmitterUpdate);
        const FString ParticleSpawnGuid = AttachSharedSourceStage(Emitter, Source, ENiagaraScriptUsage::ParticleSpawnScript, TEXT("SharedParticleSpawn"), ParticleSpawn);
        const FString ParticleUpdateGuid = AttachSharedSourceStage(Emitter, Source, ENiagaraScriptUsage::ParticleUpdateScript, TEXT("SharedParticleUpdate"), ParticleUpdate);
        if (!EmitterSpawn || !EmitterUpdate || !ParticleSpawn || !ParticleUpdate)
        {
            return false;
        }

        EmitterData->EmitterSpawnScriptProps.Script = EmitterSpawn;
        EmitterData->EmitterUpdateScriptProps.Script = EmitterUpdate;
        EmitterData->SpawnScriptProps.Script = ParticleSpawn;
        EmitterData->UpdateScriptProps.Script = ParticleUpdate;

        OutUsageByModuleGuid.Add(EmitterSpawnGuid, TEXT("EmitterSpawnScript"));
        OutUsageByModuleGuid.Add(EmitterUpdateGuid, TEXT("EmitterUpdateScript"));
        OutUsageByModuleGuid.Add(ParticleSpawnGuid, TEXT("ParticleSpawnScript"));
        OutUsageByModuleGuid.Add(ParticleUpdateGuid, TEXT("ParticleUpdateScript"));
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraDumpBuilderStackListsEachModuleOncePerStageTest,
    "PinWright.Assets.Niagara.DumpBuilder.StackListsEachModuleOncePerStage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpBuilderStackListsEachModuleOncePerStageTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraEmitter* Emitter = NewTransientNiagaraEmitter(ObjectPath);
    TestNotNull(TEXT("Transient Niagara emitter created"), Emitter);
    if (!Emitter)
    {
        return false;
    }

    TMap<FString, FString> UsageByModuleGuid;
    const bool bAttached = AttachSharedGraphFourStageStack(Emitter, UsageByModuleGuid);
    TestTrue(TEXT("Shared-graph four-stage stack fixture attached"), bAttached);
    TestEqual(TEXT("Fixture built one module per stage"), UsageByModuleGuid.Num(), 4);
    if (!bAttached || UsageByModuleGuid.Num() != 4)
    {
        Emitter->RemoveFromRoot();
        return false;
    }

    TSharedPtr<FJsonObject> StackJson = NiagaraDumpBuilder::BuildEmitterStackJson(Emitter);
    const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
    const bool bHasModules = StackJson.IsValid() && StackJson->TryGetArrayField(TEXT("modules"), Modules) && Modules;
    TestTrue(TEXT("emitter stack has modules array"), bHasModules);
    if (!bHasModules)
    {
        Emitter->RemoveFromRoot();
        return false;
    }

    // Four distinct modules on one shared graph: four entries, not four per script.
    TestEqual(TEXT("stack lists one entry per module, not one per module per script"), Modules->Num(), 4);

    TMap<FString, int32> CountByEntryId;
    for (const TSharedPtr<FJsonValue>& ModuleValue : *Modules)
    {
        TSharedPtr<FJsonObject> Module = ModuleValue.IsValid() ? ModuleValue->AsObject() : nullptr;
        if (!Module.IsValid())
        {
            continue;
        }
        FString EntryId;
        if (!Module->TryGetStringField(TEXT("entryId"), EntryId))
        {
            continue;
        }
        ++CountByEntryId.FindOrAdd(EntryId);

        const FString* ExpectedUsage = UsageByModuleGuid.Find(EntryId);
        TestNotNull(*FString::Printf(TEXT("entry %s is one of the fixture's modules"), *EntryId), ExpectedUsage);
        if (ExpectedUsage)
        {
            FString ScriptUsage;
            Module->TryGetStringField(TEXT("scriptUsage"), ScriptUsage);
            TestEqual(*FString::Printf(TEXT("module %s names the stage it sits in"), *EntryId), ScriptUsage, *ExpectedUsage);
            TestEqual(*FString::Printf(TEXT("module %s is index 0 of its own stage"), *EntryId),
                static_cast<int32>(Module->GetNumberField(TEXT("index"))), 0);
        }
    }

    for (const TPair<FString, FString>& Expected : UsageByModuleGuid)
    {
        const int32* Count = CountByEntryId.Find(Expected.Key);
        TestEqual(*FString::Printf(TEXT("module %s (%s) appears exactly once"), *Expected.Key, *Expected.Value),
            Count ? *Count : 0, 1);
    }

    Emitter->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraDumpBuilderNiagaraBoolBaseTypeTest,
    "PinWright.niagara.dump_builder.NiagaraBoolBaseType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpBuilderNiagaraBoolBaseTypeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientNiagaraSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    FNiagaraParameterStore& Store = System->GetExposedParameters();
    const FNiagaraTypeDefinition NiagaraBoolType(FNiagaraBool::StaticStruct());

    const FNiagaraVariable TrueVar(NiagaraBoolType, FName(TEXT("User.TestNiagaraBoolTrue")));
    const FNiagaraBool TrueValue(true);
    Store.SetParameterData(reinterpret_cast<const uint8*>(&TrueValue), TrueVar, /*bAdd=*/true);

    const FNiagaraVariable FalseVar(NiagaraBoolType, FName(TEXT("User.TestNiagaraBoolFalse")));
    const FNiagaraBool FalseValue(false);
    Store.SetParameterData(reinterpret_cast<const uint8*>(&FalseValue), FalseVar, /*bAdd=*/true);

    TSharedPtr<FJsonObject> ParametersJson = NiagaraDumpBuilder::BuildParametersJson(System);

    {
        TSharedPtr<FJsonObject> Entry = FindUserParameterEntry(ParametersJson, TEXT("User.TestNiagaraBoolTrue"));
        TestNotNull(TEXT("NiagaraBool true entry present"), Entry.Get());
        if (Entry.IsValid())
        {
            TSharedPtr<FJsonValue> ValueField = Entry->TryGetField(TEXT("value"));
            TestTrue(TEXT("NiagaraBool true value is JSON boolean"), ValueField.IsValid() && ValueField->Type == EJson::Boolean);
            if (ValueField.IsValid() && ValueField->Type == EJson::Boolean)
            {
                TestTrue(TEXT("NiagaraBool true value is true"), ValueField->AsBool());
            }
        }
    }

    {
        TSharedPtr<FJsonObject> Entry = FindUserParameterEntry(ParametersJson, TEXT("User.TestNiagaraBoolFalse"));
        TestNotNull(TEXT("NiagaraBool false entry present"), Entry.Get());
        if (Entry.IsValid())
        {
            TSharedPtr<FJsonValue> ValueField = Entry->TryGetField(TEXT("value"));
            TestTrue(TEXT("NiagaraBool false value is JSON boolean"), ValueField.IsValid() && ValueField->Type == EJson::Boolean);
            if (ValueField.IsValid() && ValueField->Type == EJson::Boolean)
            {
                TestFalse(TEXT("NiagaraBool false value is false"), ValueField->AsBool());
            }
        }
    }

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraDumpBuilderCustomStructConverterJsonTest,
    "PinWright.Assets.Niagara.DumpBuilder.CustomStructConverterJson",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpBuilderCustomStructConverterJsonTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientNiagaraSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    const FName ParameterName(TEXT("User.TestConvertedTransform"));
    UScriptStruct* TransformStruct = TBaseStructure<FTransform>::Get();
    FNiagaraTypeHelper::GetSWCStruct(TransformStruct);
    const FNiagaraVariable TransformVar(FNiagaraTypeRegistry::GetTypeForStruct(TransformStruct), ParameterName);
    const FTransform SourceValue(
        FQuat::Identity,
        FVector(12.25, -3.5, 88.0),
        FVector(2.0, 3.0, 4.0));
    FNiagaraParameterStore& Store = System->GetExposedParameters();
    Store.SetParameterData(reinterpret_cast<const uint8*>(&SourceValue), TransformVar, /*bAdd=*/true);

    TSharedPtr<FJsonObject> ParametersJson = NiagaraDumpBuilder::BuildParametersJson(System);
    TSharedPtr<FJsonObject> Entry = FindUserParameterEntry(ParametersJson, ParameterName.ToString());
    TestNotNull(TEXT("converted struct entry present"), Entry.Get());
    if (Entry.IsValid())
    {
        const TSharedPtr<FJsonObject>* ValueObj = nullptr;
        const bool bIsObject = Entry->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid();
        TestTrue(TEXT("converted struct value is JSON object"), bIsObject);
        if (bIsObject)
        {
            TestFalse(TEXT("converted struct is NOT _kind:rawBytes"), (*ValueObj)->GetStringField(TEXT("_kind")).Equals(TEXT("rawBytes")));
            TestEqual(TEXT("converted struct kind"), (*ValueObj)->GetStringField(TEXT("_kind")), FString(TEXT("struct")));
            TestEqual(TEXT("converted struct path"), (*ValueObj)->GetStringField(TEXT("scriptStruct")), TransformStruct->GetPathName());

            const TSharedPtr<FJsonObject>* Fields = nullptr;
            const bool bHasFields = (*ValueObj)->TryGetObjectField(TEXT("fields"), Fields) && Fields && (*Fields).IsValid();
            TestTrue(TEXT("converted struct has fields"), bHasFields);
            if (bHasFields)
            {
                const TArray<TSharedPtr<FJsonValue>>* Translation = nullptr;
                const bool bHasTranslation = (*Fields)->TryGetArrayField(TEXT("Translation"), Translation) && Translation && Translation->Num() >= 3;
                TestTrue(TEXT("converted struct has translated source fields"), bHasTranslation);
                if (bHasTranslation)
                {
                    TestEqual(TEXT("converted struct Translation.X"), (*Translation)[0]->AsNumber(), SourceValue.GetTranslation().X);
                    TestEqual(TEXT("converted struct Translation.Y"), (*Translation)[1]->AsNumber(), SourceValue.GetTranslation().Y);
                    TestEqual(TEXT("converted struct Translation.Z"), (*Translation)[2]->AsNumber(), SourceValue.GetTranslation().Z);
                }
            }
        }
    }

    System->RemoveFromRoot();
    return true;
}
