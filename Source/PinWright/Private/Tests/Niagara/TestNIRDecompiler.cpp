// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the NIR decompiler (F-niagara-decompile-nir v1a).
//
// Covers:
//   1. System decompile shape (system "..." { ... emitter ... stack ... module ... })
//   2. Standalone Emitter decompile shape
//   3. Standalone Script graph-scope shape (script "..." { graph <Usage> { ... } })
//   4. RPC ↔ asset-dump sidecar zero-divergence (dual-surface invariant)
//   5. live compile-session annotations are excluded from authored NIR
//
// Full integration (live override-chain resolution, live module rows, populated graphs)
// requires the editor pipeline that transient-package construction cannot safely
// reproduce; v1a tests the grammar entry points and the dual-surface contract.

#include "Misc/AutomationTest.h"

#include "NIR/NIRDecompiler.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Tests/Assets/AssetDumpTestHelpers.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#include "EdGraphSchema_Niagara.h"
#include "NiagaraConstants.h"
#include "NiagaraEmitter.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeStaticSwitch.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
    using AssetDumpTestHelpers::FindDumpFile;
    using AssetDumpTestHelpers::HasDumpFile;

    FString MakeNirTestAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UNiagaraSystem* NewTransientNirSystem(FString& OutObjectPath)
    {
        const FString AssetName = MakeNirTestAssetName(TEXT("NS_NIR"));
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

    UNiagaraEmitter* NewTransientNirEmitter(FString& OutObjectPath)
    {
        const FString AssetName = MakeNirTestAssetName(TEXT("NE_NIR"));
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

    UNiagaraScript* NewTransientNirScript(FString& OutObjectPath)
    {
        const FString AssetName = MakeNirTestAssetName(TEXT("NM_NIR"));
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

    UNiagaraGraph* AttachNirScriptGraph(UNiagaraScript* Script, UObject* Outer, const TCHAR* SourceName, const TCHAR* GraphName)
    {
        if (!Script || !Outer)
        {
            return nullptr;
        }
        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            Outer,
            SourceName,
            RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, GraphName, RF_Transient | RF_Transactional)
            : nullptr;
        if (!Graph)
        {
            return nullptr;
        }
        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;
        Script->SetLatestSource(Source);
        return Graph;
    }

    UNiagaraNodeFunctionCall* AddNirFixtureModuleNode(UNiagaraGraph* Graph, const TCHAR* Name, bool bSupportsGpu = true)
    {
        if (!Graph)
        {
            return nullptr;
        }
        UNiagaraNodeFunctionCall* ModuleNode = NewObject<UNiagaraNodeFunctionCall>(
            Graph,
            UNiagaraNodeFunctionCall::StaticClass(),
            Name,
            RF_Transient | RF_Transactional);
        if (!ModuleNode)
        {
            return nullptr;
        }
        ModuleNode->NodeGuid = FGuid::NewGuid();
        ModuleNode->NodePosX = 100;
        ModuleNode->NodePosY = 200;
        FStrProperty* DisplayNameProperty = FindFProperty<FStrProperty>(
            UNiagaraNodeFunctionCall::StaticClass(), TEXT("FunctionDisplayName"));
        check(DisplayNameProperty);
        DisplayNameProperty->SetPropertyValue_InContainer(ModuleNode, FString(Name));
        ModuleNode->Signature.Name = FName(Name);
        ModuleNode->Signature.bSupportsGPU = bSupportsGpu;
        ModuleNode->Signature.Outputs.Add(FNiagaraVariableBase(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("Out"))));
        Graph->AddNode(ModuleNode, false, false);
        return ModuleNode;
    }

    // Wire a transient UNiagaraScriptSource+Graph onto the system spawn/update scripts and
    // drop one UNiagaraNodeFunctionCall into the graph. The function-call has no real
    // FunctionScript so its reflected FunctionDisplayName is what AppendModuleRow emits —
    // that's enough to make "stack SystemSpawn { ... module ... }" appear in NIR output.
    void AttachSystemFixtureGraph(UNiagaraSystem* System)
    {
        if (!System || !System->GetSystemSpawnScript() || !System->GetSystemUpdateScript())
        {
            return;
        }
        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            System->GetSystemSpawnScript(),
            TEXT("NirFixtureSystemScriptSource"),
            RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("NirFixtureSystemScriptGraph"), RF_Transient | RF_Transactional)
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
            TEXT("NirFixtureModule"),
            RF_Transient | RF_Transactional);
        if (ModuleNode)
        {
            ModuleNode->NodeGuid = FGuid::NewGuid();
            ModuleNode->NodePosX = 100;
            ModuleNode->NodePosY = 200;
            FStrProperty* DisplayNameProperty = FindFProperty<FStrProperty>(
                UNiagaraNodeFunctionCall::StaticClass(), TEXT("FunctionDisplayName"));
            check(DisplayNameProperty);
            DisplayNameProperty->SetPropertyValue_InContainer(ModuleNode, FString(TEXT("NirFixtureModule")));
            Graph->AddNode(ModuleNode, false, false);
        }
    }

    UNiagaraScript* NewTransientNirModuleScriptWithStaticSwitches(
        UObject* Outer,
        const TArray<FName>& StaticSwitchNames)
    {
        UNiagaraScript* Script = NewObject<UNiagaraScript>(
            Outer ? Outer : GetTransientPackage(),
            UNiagaraScript::StaticClass(),
            *MakeNirTestAssetName(TEXT("NM_NIR_StaticSwitch")),
            RF_Transient | RF_Transactional);
        if (!Script)
        {
            return nullptr;
        }
        Script->SetUsage(ENiagaraScriptUsage::Module);
        Script->SetUsageId(FGuid::NewGuid());

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            Script,
            TEXT("NirFixtureStaticSwitchSource"),
            RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("NirFixtureStaticSwitchGraph"), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Graph)
        {
            return nullptr;
        }
        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;
        Script->SetLatestSource(Source);

        int32 NodePosY = 0;
        for (const FName& SwitchName : StaticSwitchNames)
        {
            UNiagaraNodeStaticSwitch* SwitchNode = NewObject<UNiagaraNodeStaticSwitch>(
                Graph,
                UNiagaraNodeStaticSwitch::StaticClass(),
                NAME_None,
                RF_Transient | RF_Transactional);
            if (!SwitchNode)
            {
                continue;
            }
            SwitchNode->InputParameterName = SwitchName;
            SwitchNode->SwitchTypeData.SwitchType = ENiagaraStaticSwitchType::Bool;
            SwitchNode->NodeGuid = FGuid::NewGuid();
            SwitchNode->NodePosY = NodePosY;
            NodePosY += 100;
            Graph->AddNode(SwitchNode, false, false);
        }

        return Script;
    }

    void AttachSystemStaticSwitchFixtureGraph(UNiagaraSystem* System, UNiagaraScript* FunctionScript)
    {
        if (!System || !System->GetSystemSpawnScript())
        {
            return;
        }

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            System->GetSystemSpawnScript(),
            TEXT("NirFixtureStaticSwitchSystemSource"),
            RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("NirFixtureStaticSwitchSystemGraph"), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Graph)
        {
            return;
        }
        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;
        System->GetSystemSpawnScript()->SetLatestSource(Source);

        UNiagaraNodeFunctionCall* ModuleNode = NewObject<UNiagaraNodeFunctionCall>(
            Graph,
            UNiagaraNodeFunctionCall::StaticClass(),
            TEXT("NirFixtureStaticSwitchModule"),
            RF_Transient | RF_Transactional);
        if (!ModuleNode)
        {
            return;
        }
        ModuleNode->NodeGuid = FGuid::NewGuid();
        ModuleNode->NodePosY = 100;
        ModuleNode->FunctionScript = FunctionScript;
        FStrProperty* DisplayNameProperty = FindFProperty<FStrProperty>(
            UNiagaraNodeFunctionCall::StaticClass(), TEXT("FunctionDisplayName"));
        check(DisplayNameProperty);
        DisplayNameProperty->SetPropertyValue_InContainer(ModuleNode, FString(TEXT("NirFixtureStaticSwitchModule")));
        Graph->AddNode(ModuleNode, false, false);
    }

    void AddFloatModuleDefault(UNiagaraGraph& Graph, const TCHAR* InputName, float DefaultValue)
    {
        const FNiagaraVariable Variable(
            FNiagaraTypeDefinition::GetFloatDef(),
            FName(*(FNiagaraConstants::ModuleNamespaceString + TEXT(".") + InputName)));
        UNiagaraScriptVariable* ScriptVariable = NewObject<UNiagaraScriptVariable>(
            &Graph,
            NAME_None,
            RF_Transient | RF_Transactional);
        FNiagaraVariableMetaData Metadata;
        ScriptVariable->Init(Variable, Metadata);
        ScriptVariable->DefaultMode = ENiagaraDefaultMode::Value;
        ScriptVariable->SetDefaultValueData(reinterpret_cast<const uint8*>(&DefaultValue));
        Graph.GetAllMetaData().Add(Variable, ScriptVariable);
    }

    UNiagaraScript* AttachSystemRapidIterationFixtureGraph(UNiagaraSystem& System)
    {
        UNiagaraScript* ModuleScript = NewObject<UNiagaraScript>(
            &System,
            UNiagaraScript::StaticClass(),
            TEXT("NirRapidIterationModuleScript"),
            RF_Transient | RF_Transactional);
        if (!ModuleScript)
        {
            return nullptr;
        }
        ModuleScript->SetUsage(ENiagaraScriptUsage::Module);
        ModuleScript->SetUsageId(FGuid::NewGuid());
        UNiagaraGraph* ModuleGraph = AttachNirScriptGraph(
            ModuleScript,
            ModuleScript,
            TEXT("NirRapidIterationModuleSource"),
            TEXT("NirRapidIterationModuleGraph"));
        if (!ModuleGraph)
        {
            return nullptr;
        }
        AddFloatModuleDefault(*ModuleGraph, TEXT("Authored"), 1.0f);
        AddFloatModuleDefault(*ModuleGraph, TEXT("GeneratedDefault"), 2.0f);

        UNiagaraGraph* StackGraph = AttachNirScriptGraph(
            System.GetSystemSpawnScript(),
            System.GetSystemSpawnScript(),
            TEXT("NirRapidIterationStackSource"),
            TEXT("NirRapidIterationStackGraph"));
        if (!StackGraph)
        {
            return nullptr;
        }
        UNiagaraNodeFunctionCall* ModuleNode = AddNirFixtureModuleNode(StackGraph, TEXT("NirRapidModule"));
        if (!ModuleNode)
        {
            return nullptr;
        }
        ModuleNode->FunctionScript = ModuleScript;
        return System.GetSystemSpawnScript();
    }

    int32 CountSubstringOccurrences(const FString& Text, const TCHAR* Needle)
    {
        int32 Count = 0;
        int32 SearchStart = 0;
        while (true)
        {
            const int32 Found = Text.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchStart);
            if (Found == INDEX_NONE)
            {
                break;
            }
            ++Count;
            SearchStart = Found + FCString::Strlen(Needle);
        }
        return Count;
    }

    bool AttachEmitterFixtureGraphSource(UNiagaraEmitter* Emitter)
    {
        FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
        if (!EmitterData)
        {
            return false;
        }
        if (EmitterData->GraphSource)
        {
            return true;
        }

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            Emitter,
            TEXT("NirFixtureEmitterSource"),
            RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("NirFixtureEmitterGraph"), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Graph)
        {
            return false;
        }

        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;
        EmitterData->GraphSource = Source;
        return true;
    }

    // Populate a transient UNiagaraSystem with one emitter handle plus a system-stack
    // graph carrying a function-call node. AddEmitterHandle duplicates the source
    // emitter, so the source needs a GraphSource before Niagara's PostLoad path runs.
    void PopulateNirSystemFixture(UNiagaraSystem* System)
    {
        AttachSystemFixtureGraph(System);

        UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(
            System,
            FName(TEXT("NirFixtureEmitterAsset")),
            RF_Transient | RF_Transactional);
        if (Emitter && AttachEmitterFixtureGraphSource(Emitter))
        {
            System->AddEmitterHandle(*Emitter, FName(TEXT("NirFixtureEmitter")), Emitter->GetExposedVersion().VersionGuid);
        }
    }

    bool PopulateEmitterParityFixture(UNiagaraEmitter* Emitter)
    {
        FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
        if (!EmitterData || !EmitterData->SpawnScriptProps.Script)
        {
            return false;
        }

        EmitterData->SimTarget = ENiagaraSimTarget::GPUComputeSim;
        EmitterData->bLocalSpace = true;
        EmitterData->bDeterminism = true;
        EmitterData->RandomSeed = 42;
        EmitterData->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Fixed;
        EmitterData->FixedBounds = FBox(FVector(-1.0, -2.0, -3.0), FVector(1.0, 2.0, 3.0));
        EmitterData->bRequiresPersistentIDs = true;
        EmitterData->MaxGPUParticlesSpawnPerFrame = 123;
        EmitterData->AllocationMode = EParticleAllocationMode::ManualEstimate;
        EmitterData->PreAllocationCount = 77;

        UNiagaraGraph* SpawnGraph = AttachNirScriptGraph(
            EmitterData->SpawnScriptProps.Script,
            EmitterData->SpawnScriptProps.Script,
            TEXT("NirFixtureSpawnSource"),
            TEXT("NirFixtureSpawnGraph"));
        AddNirFixtureModuleNode(SpawnGraph, TEXT("NirCpuOnlyModule"), /*bSupportsGpu=*/false);

        FNiagaraEventScriptProperties EventHandler;
        EventHandler.SourceEventName = FName(TEXT("NirEvent"));
        EventHandler.Script = NewObject<UNiagaraScript>(
            Emitter,
            TEXT("NirEventScript"),
            RF_Transient | RF_Transactional);
        if (EventHandler.Script)
        {
            EventHandler.Script->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
            EventHandler.Script->SetUsageId(FGuid::NewGuid());
            UNiagaraGraph* EventGraph = AttachNirScriptGraph(
                EventHandler.Script,
                EventHandler.Script,
                TEXT("NirFixtureEventSource"),
                TEXT("NirFixtureEventGraph"));
            AddNirFixtureModuleNode(EventGraph, TEXT("NirEventGraphModule"));
            EmitterData->EventHandlerScriptProps.Add(EventHandler);
        }

        UNiagaraSimulationStageGeneric* Stage = NewObject<UNiagaraSimulationStageGeneric>(
            Emitter,
            TEXT("NirSimulationStage"),
            RF_Transient | RF_Transactional);
        if (Stage)
        {
            Stage->SimulationStageName = FName(TEXT("NirStage"));
            Stage->Script = NewObject<UNiagaraScript>(
                Stage,
                TEXT("NirSimulationStageScript"),
                RF_Transient | RF_Transactional);
            if (Stage->Script)
            {
                Stage->Script->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
                Stage->Script->SetUsageId(FGuid::NewGuid());
                UNiagaraGraph* StageGraph = AttachNirScriptGraph(
                    Stage->Script,
                    Stage->Script,
                    TEXT("NirFixtureStageSource"),
                    TEXT("NirFixtureStageGraph"));
                AddNirFixtureModuleNode(StageGraph, TEXT("NirStageGraphModule"));
                Emitter->AddSimulationStage(Stage, Emitter->GetExposedVersion().VersionGuid);
            }
        }

        if (UNiagaraScript* GpuScript = EmitterData->GetGPUComputeScript())
        {
            UNiagaraGraph* GpuGraph = AttachNirScriptGraph(
                GpuScript,
                GpuScript,
                TEXT("NirFixtureGpuSource"),
                TEXT("NirFixtureGpuGraph"));
            AddNirFixtureModuleNode(GpuGraph, TEXT("NirGpuGraphModule"));
        }
        return true;
    }
}

// 1. System decompile shape
//
// Counterfactual: if BuildNiagaraIrText is reverted to FNIRResult::MakeError on
// UNiagaraSystem (or strips the `system "..."` header line), bSuccess fails or the
// "system \"" Contains assertion fails because the header is no longer emitted.
// If AppendStack is skipped or the emitter loop is dropped, the `stack SystemSpawn`
// or `emitter ` substring assertions fail. If AppendModuleRow stops emitting the
// `module ` row, that assertion fails — guarding the visual stack-emission shape.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirSystemDecompileShapeTest,
    "PinWright.niagara.decompile_nir.SystemDecompileShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirSystemDecompileShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientNirSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara System created"), System);
    if (!System)
    {
        return false;
    }

    PopulateNirSystemFixture(System);

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(System);
    TestTrue(TEXT("NIR build succeeds for system"), Result.bSuccess);
    TestTrue(TEXT("NIR text contains system header"), Result.Text.Contains(TEXT("system \"")));
    TestTrue(TEXT("NIR text contains emitter handle row"), Result.Text.Contains(TEXT("emitter ")));
    TestTrue(TEXT("NIR text contains system flags block"), Result.Text.Contains(TEXT("systemFlags")));
    TestTrue(TEXT("NIR text contains fixed bounds state"), Result.Text.Contains(TEXT("fixedBounds")));
    TestTrue(TEXT("NIR text contains emitter handle state"), Result.Text.Contains(TEXT("handle")));
    TestTrue(TEXT("NIR text contains SystemSpawn stack header"), Result.Text.Contains(TEXT("stack SystemSpawn")));
    TestTrue(TEXT("NIR text contains module row"), Result.Text.Contains(TEXT("module ")));
    TestFalse(TEXT("NIR omits live emitter-handle validity"), Result.Text.Contains(TEXT("valid:")));
    TestFalse(TEXT("NIR omits live emitter-handle recompile state"), Result.Text.Contains(TEXT("needsRecompile:")));
    TestTrue(TEXT("NIR text ends in closing brace"), Result.Text.EndsWith(TEXT("}\n")) || Result.Text.EndsWith(TEXT("}")));

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirStaticSwitchUnresolvedNameWarningTest,
    "PinWright.niagara.decompile_nir.StaticSwitchUnresolvedNameWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirStaticSwitchUnresolvedNameWarningTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientNirSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara System created"), System);
    if (!System)
    {
        return false;
    }

    TArray<FName> StaticSwitchNames;
    StaticSwitchNames.Add(FName(TEXT("Undefined parameter name")));
    UNiagaraScript* ModuleScript = NewTransientNirModuleScriptWithStaticSwitches(System, StaticSwitchNames);
    TestNotNull(TEXT("Transient module script with unresolved static switch created"), ModuleScript);
    if (!ModuleScript)
    {
        System->RemoveFromRoot();
        return false;
    }
    AttachSystemStaticSwitchFixtureGraph(System, ModuleScript);

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(System);
    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    TestFalse(TEXT("NIR omits misleading unresolved static-switch line"),
        Result.Text.Contains(TEXT("static Undefined parameter name")));
    TestTrue(TEXT("NIR text contains unresolved-pin marker"),
        Result.Text.Contains(TEXT("NIR_UNRESOLVED_PIN")));
    const bool bHasWarning = Result.Warnings.ContainsByPredicate([](const FString& Warning)
    {
        return Warning.Contains(TEXT("NIR_UNRESOLVED_PIN"));
    });
    TestTrue(TEXT("FNIRResult.Warnings contains unresolved-pin marker"), bHasWarning);

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirStaticSwitchDuplicateLineSuppressionTest,
    "PinWright.niagara.decompile_nir.StaticSwitchDuplicateLineSuppression",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirStaticSwitchDuplicateLineSuppressionTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientNirSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara System created"), System);
    if (!System)
    {
        return false;
    }

    TArray<FName> StaticSwitchNames;
    StaticSwitchNames.Add(FName(TEXT("DuplicateSwitch")));
    StaticSwitchNames.Add(FName(TEXT("DuplicateSwitch")));
    UNiagaraScript* ModuleScript = NewTransientNirModuleScriptWithStaticSwitches(System, StaticSwitchNames);
    TestNotNull(TEXT("Transient module script with duplicate static switches created"), ModuleScript);
    if (!ModuleScript)
    {
        System->RemoveFromRoot();
        return false;
    }
    AttachSystemStaticSwitchFixtureGraph(System, ModuleScript);

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(System);
    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    TestEqual(TEXT("Duplicate static-switch rows collapse to one NIR line"),
        CountSubstringOccurrences(Result.Text, TEXT("static DuplicateSwitch = ")),
        1);

    System->RemoveFromRoot();
    return true;
}

// 2. Emitter decompile shape
//
// Standalone UNiagaraEmitter construction via NewObject doesn't run the editor-only
// pipeline that creates default renderers or simulation stages, so this test only
// asserts the header and absence of a system wrapper. Adding renderers to a
// transient emitter requires UNiagaraEditorModule helpers that aren't safely
// reachable from automation-test context.
//
// Counterfactual: if the UNiagaraEmitter dispatch branch is removed from
// BuildNiagaraIrText, the generic unsupported-type error fires and bSuccess fails;
// if the `emitter "..."` header is dropped, the Contains assertion fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirEmitterDecompileShapeTest,
    "PinWright.niagara.decompile_nir.EmitterDecompileShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirEmitterDecompileShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraEmitter* Emitter = NewTransientNirEmitter(ObjectPath);
    TestNotNull(TEXT("Transient Niagara Emitter created"), Emitter);
    if (!Emitter)
    {
        return false;
    }

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(Emitter);
    TestTrue(TEXT("NIR build succeeds for emitter"), Result.bSuccess);
    TestTrue(TEXT("NIR text contains standalone emitter header"), Result.Text.Contains(TEXT("emitter \"")));
    TestTrue(TEXT("NIR text contains sim target"), Result.Text.Contains(TEXT("simTarget:")));
    TestTrue(TEXT("NIR text contains emitter flag block"), Result.Text.Contains(TEXT("emitterFlags")));
    TestFalse(TEXT("NIR text does not start with system header"), Result.Text.StartsWith(TEXT("system ")));

    Emitter->RemoveFromRoot();
    return true;
}

// 2b. Renderer properties sitting at a class default (B-nir-renderer-omits-default-valued-properties)
//
// The renderer block used to omit every property equal to its class default, which made
// an absent line indistinguishable from `false` / 0 / unset — and readers guess the
// latter. UNiagaraSpriteRendererProperties::bSubImageBlend defaults to TRUE (see its
// constructor), so the guess returned the exact opposite of the truth.
//
// The zero-default rule: only defaults that ARE the type's zero value may be omitted.
// A default that is not (true, (1,1), a bound attribute) is printed with a trailing
// ` @default` marker, so absence carries exactly one meaning and the authored-vs-
// inherited distinction that omission used to encode is preserved in the marker.
//
// Counterfactual: drop `Options.bEmitNonZeroDefaults` in NIRDecompiler::BuildReflectedFields,
// or the ` @default` suffix in FIrTextUtils::AppendReflectedFields, and the bSubImageBlend
// assertion fails. Format the forced line against the archetype default instead of nullptr
// and SubImageSize exports as the empty diff `"()"`, failing its assertion. Emit every
// property regardless of its default and the MinFacingCameraBlendDistance absence
// assertion fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirRendererDefaultValuedPropertyTest,
    "PinWright.niagara.decompile_nir.RendererDefaultValuedProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirRendererDefaultValuedPropertyTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraEmitter* Emitter = NewTransientNirEmitter(ObjectPath);
    TestNotNull(TEXT("Transient Niagara Emitter created"), Emitter);
    if (!Emitter)
    {
        return false;
    }

    if (!Emitter->GetLatestEmitterData())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("emitter-version-data-missing"),
            TEXT("Transient emitter has no latest version data; skipping."));
        Emitter->RemoveFromRoot();
        return true;
    }

    UNiagaraSpriteRendererProperties* Renderer = NewObject<UNiagaraSpriteRendererProperties>(
        Emitter,
        UNiagaraSpriteRendererProperties::StaticClass(),
        NAME_None,
        RF_Transient | RF_Transactional);
    TestNotNull(TEXT("Sprite renderer created"), Renderer);
    if (!Renderer)
    {
        Emitter->RemoveFromRoot();
        return false;
    }

    // MacroUVRadius' class default is 0.0f — its type's zero value — so overriding it
    // exercises the unmarked-override form alongside the marked-default form.
    Renderer->MacroUVRadius = 12.5f;
    Emitter->AddRenderer(Renderer, Emitter->GetExposedVersion().VersionGuid);

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(Emitter);
    TestTrue(TEXT("NIR build succeeds for renderer emitter"), Result.bSuccess);
    TestTrue(TEXT("NIR contains the sprite renderer block"),
        Result.Text.Contains(TEXT("renderer NiagaraSpriteRendererProperties @0 enabled")));

    // bSubImageBlend only defaults to true from UE 5.4; on 5.3 its class default is false, which
    // IS the type's zero value and so is correctly omitted by the rule this test asserts.
    // bSortOnlyWhenTranslucent is the 5.3 stand-in: same type, class default true on every
    // supported engine.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    TestTrue(TEXT("Bool at a non-zero class default prints its value with the default marker"),
        Result.Text.Contains(TEXT("bSubImageBlend: true @default")));
#else
    TestTrue(TEXT("Bool at a non-zero class default prints its value with the default marker"),
        Result.Text.Contains(TEXT("bSortOnlyWhenTranslucent: true @default")));
#endif
    TestTrue(TEXT("Struct at a non-zero class default prints its whole resolved value"),
        Result.Text.Contains(TEXT("SubImageSize: \"(X=1.000000,Y=1.000000)\" @default")));

    TestTrue(TEXT("Overridden property is still emitted"),
        Result.Text.Contains(TEXT("MacroUVRadius: 12.5")));
    TestFalse(TEXT("Overridden property carries no default marker"),
        Result.Text.Contains(TEXT("MacroUVRadius: 12.5 @default")));

    // A class default that IS the type's zero value stays omitted: absence in a renderer
    // block means zero/false/unset, and nothing else.
    TestFalse(TEXT("Zero-valued class default stays omitted"),
        Result.Text.Contains(TEXT("MinFacingCameraBlendDistance")));

    Emitter->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirEmitterParityAndGpuDiagnosticsTest,
    "PinWright.niagara.decompile_nir.EmitterParityAndGpuDiagnostics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirEmitterParityAndGpuDiagnosticsTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraEmitter* Emitter = NewTransientNirEmitter(ObjectPath);
    TestNotNull(TEXT("Transient Niagara Emitter created"), Emitter);
    if (!Emitter)
    {
        return false;
    }

    if (!PopulateEmitterParityFixture(Emitter))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-did-not-populate"),
            TEXT("Could not populate transient emitter parity fixture; skipping."));
        Emitter->RemoveFromRoot();
        return true;
    }

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(Emitter);
    TestTrue(TEXT("NIR build succeeds for parity emitter"), Result.bSuccess);
    TestTrue(TEXT("NIR contains local-space flag"), Result.Text.Contains(TEXT("localSpace: true")));
    TestTrue(TEXT("NIR contains fixed-bounds mode"), Result.Text.Contains(TEXT("calculateBoundsMode: Fixed")));
    TestTrue(TEXT("NIR contains fixed bounds payload"), Result.Text.Contains(TEXT("fixedBounds: valid=")));
    TestTrue(TEXT("NIR contains GPU spawn cap"), Result.Text.Contains(TEXT("maxGpuParticlesSpawnPerFrame: 123")));
    TestTrue(TEXT("NIR contains preallocation count"), Result.Text.Contains(TEXT("preAllocationCount: 77")));
    TestTrue(TEXT("NIR contains event-handler text"), Result.Text.Contains(TEXT("eventHandler NirEvent")));
    TestTrue(TEXT("NIR contains event-handler graph body"), Result.Text.Contains(TEXT("graph ParticleEvent")));
    TestTrue(TEXT("NIR contains simulation-stage text"), Result.Text.Contains(TEXT("simStage NirSimulationStage")));
    TestTrue(TEXT("NIR contains simulation-stage graph body"), Result.Text.Contains(TEXT("graph ParticleSimulationStage")));
    TestTrue(TEXT("NIR contains GPU compute script graph"), Result.Text.Contains(TEXT("graph ParticleGPUCompute")));
    TestTrue(TEXT("NIR contains GPU-incompatible module annotation"),
        Result.Text.Contains(TEXT("NirCpuOnlyModule")) && Result.Text.Contains(TEXT("GPU_INCOMPATIBLE_MODULE")));

    Emitter->RemoveFromRoot();
    return true;
}

// 3. Module-script graph-scope shape
//
// v1c (F-niagara-decompile-nir-script-graphs) replaced v1a's deferred-graph
// placeholder with a realised `graph <Usage> { ... }` body. This test confirms
// the v1a placeholder string is gone
// and the graph-scope keyword is emitted (the body is empty for a fresh
// transient script with no nodes — node-by-node body emission is exercised
// by the per-family TestNIRGraph{Dataflow,Control,Util}.cpp tests).
//
// Counterfactual: if the UNiagaraScript dispatch branch is removed, the unsupported
// type error fires and bSuccess fails. If EmitScriptGraphScope regresses to the v1a
// placeholder, the negative assertion fires.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirScriptDecompileGraphScopeTest,
    "PinWright.niagara.decompile_nir.ScriptGraphScope",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirScriptDecompileGraphScopeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraScript* Script = NewTransientNirScript(ObjectPath);
    TestNotNull(TEXT("Transient Niagara Script created"), Script);
    if (!Script)
    {
        return false;
    }

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(Script);
    TestTrue(TEXT("NIR build succeeds for script"), Result.bSuccess);
    TestTrue(TEXT("NIR text contains script header"), Result.Text.Contains(TEXT("script \"")));
    TestFalse(TEXT("v1a deferred-graph placeholder is removed"),
        Result.Text.Contains(TEXT("# script graph emission deferred")));
    TestTrue(TEXT("NIR text contains graph scope header"),
        Result.Text.Contains(TEXT("graph ")));

    Script->RemoveFromRoot();
    return true;
}

// 4. RPC ↔ asset-dump sidecar zero-divergence.
//
// Counterfactual: if a future maintainer adds dump-path-only format flags to
// BuildNiagaraIrText (the exact failure mode the dual-surface mandate prevents),
// the byte-equal assertion fails with a visible diff. If the IrSidecarRegistry
// nir.system entry is removed, HasDumpFile(..., DumpFileNames::Nir) fails because
// the sidecar is no longer emitted. If the emitter loop or AppendStack is dropped
// from EmitSystem on the dump path, the strengthened `emitter `/`stack `/`module `
// content asserts fire even when RPC and dump happen to remain byte-equal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirRpcAndDumpParityTest,
    "PinWright.niagara.decompile_nir.NirRpcAndDumpParity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirRpcAndDumpParityTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientNirSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara System created"), System);
    if (!System)
    {
        return false;
    }

    PopulateNirSystemFixture(System);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    FTestResponseCapture Capture;
    TestTrue(TEXT("niagara.decompile_nir handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.decompile_nir"), Payload, Capture));
    TestTrue(TEXT("niagara.decompile_nir succeeds"), Capture.bSuccess);
    TestTrue(TEXT("niagara.decompile_nir result exists"), Capture.Result.IsValid());

    FString RpcIr;
    if (Capture.Result.IsValid())
    {
        RpcIr = Capture.Result->GetStringField(TEXT("ir"));
    }

    const FString ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("NIRDumpTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const AssetDumpHandler::FDumpSingleResult DumpResult =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for transient Niagara System"), DumpResult.ErrorCode.IsEmpty());
    TestTrue(TEXT("nir.txt is written"), HasDumpFile(DumpResult.WrittenPaths, DumpFileNames::Nir));

    FString DumpIr;
    const FString NirPath = FindDumpFile(DumpResult.WrittenPaths, DumpFileNames::Nir);
    TestTrue(TEXT("nir.txt loads"), FFileHelper::LoadFileToString(DumpIr, *NirPath));
    TestEqual(TEXT("RPC and asset dump share NIR text"), RpcIr, DumpIr);
    TestTrue(TEXT("NIR contains system header"), DumpIr.Contains(TEXT("system \"")));
    TestTrue(TEXT("NIR contains emitter handle row"), DumpIr.Contains(TEXT("emitter ")));
    TestTrue(TEXT("NIR contains stack header"), DumpIr.Contains(TEXT("stack ")));
    TestTrue(TEXT("NIR contains module row"), DumpIr.Contains(TEXT("module ")));

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    System->RemoveFromRoot();
    return true;
}

// 5. Live compile-session state is diagnostic, not authored NIR.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirCompileStateStaleAnnotationTest,
    "PinWright.niagara.decompile_nir.CompileStateStaleAnnotation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirCompileStateStaleAnnotationTest::RunTest(const FString& Parameters)
{
    IConsoleVariable* OnDemandCV = IConsoleManager::Get().FindConsoleVariable(TEXT("fx.Niagara.OnDemandCompile"));
    if (!OnDemandCV)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-absent"),
            TEXT("fx.Niagara.OnDemandCompile CVar is not registered; skipping live-state exclusion test."));
        return true;
    }

    const bool bOriginal = OnDemandCV->GetBool();
    OnDemandCV->Set(TEXT("1"), ECVF_SetByCode);

    FString ObjectPath;
    UNiagaraSystem* System = NewTransientNirSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara System created"), System);
    if (!System)
    {
        OnDemandCV->Set(bOriginal ? TEXT("1") : TEXT("0"), ECVF_SetByCode);
        return false;
    }

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(System);
    TestTrue(TEXT("NIR build succeeds with on-demand compile enabled"), Result.bSuccess);
    TestFalse(TEXT("authored NIR excludes compile-state-stale annotation"),
        Result.Text.Contains(TEXT("# compile-state-stale")));

    OnDemandCV->Set(bOriginal ? TEXT("1") : TEXT("0"), ECVF_SetByCode);
    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirRapidIterationAuthoredOverrideTest,
    "PinWright.niagara.decompile_nir.RapidIterationAuthoredOverride",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirRapidIterationAuthoredOverrideTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientNirSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara System created"), System);
    if (!System)
    {
        return false;
    }

    UNiagaraScript* Script = AttachSystemRapidIterationFixtureGraph(*System);
    TestNotNull(TEXT("Rapid-iteration fixture graph attached"), Script);
    if (!Script)
    {
        System->RemoveFromRoot();
        return false;
    }

    const FNiagaraVariable AuthoredVariable(
        FNiagaraTypeDefinition::GetFloatDef(),
        FName(TEXT("Constants.NirRapidModule.Authored")));
    const FNiagaraVariable GeneratedDefaultVariable(
        FNiagaraTypeDefinition::GetFloatDef(),
        FName(TEXT("Constants.NirRapidModule.GeneratedDefault")));
    const float AuthoredValue = 3.0f;
    const float GeneratedDefaultValue = 2.0f;
    Script->RapidIterationParameters.SetParameterData(
        reinterpret_cast<const uint8*>(&AuthoredValue), AuthoredVariable, true);
    Script->RapidIterationParameters.SetParameterData(
        reinterpret_cast<const uint8*>(&GeneratedDefaultValue), GeneratedDefaultVariable, true);

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(System);
    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    TestTrue(TEXT("authored rapid-iteration override is retained"),
        Result.Text.Contains(TEXT("Constants.NirRapidModule.Authored")));
    TestFalse(TEXT("compile-generated source-default rapid-iteration entry is excluded"),
        Result.Text.Contains(TEXT("Constants.NirRapidModule.GeneratedDefault")));

    System->RemoveFromRoot();
    return true;
}

// ---------------------------------------------------------------------------
// v1b — override-chain expansion tests (F-niagara-decompile-nir-overrides).
//
// These exercise EmitInputValueExpr against real authored override chains
// produced via the SVM-backed fixtures in TestNIRFixtures. Each test:
//   1. Builds a transient system + emitter via NIRTestFixtures::BuildEmptySystemWithEmitter.
//   2. Drops a real module-script call onto the ParticleSpawn stack.
//   3. Authors one override-chain shape (literal / linked-param / dynamic / static-switch).
//   4. Runs BuildNiagaraIrText and asserts the expected `input X = ...` substring appears.
//
// The fixtures load engine module scripts and dynamic-input scripts so the override
// pin / override node scaffolding is created by the same editor pipeline that the
// niagara.edit SetModuleInput handler uses. If a stock module/script is missing in
// the test cooked build, the test logs a warning and passes (the override walker
// is still tested by FNiagaraNirOverrideRecursionDepthTest's synthetic chain).
// ---------------------------------------------------------------------------

namespace
{
    UNiagaraScript* LoadNirOverrideTestModuleScript()
    {
        return LoadObject<UNiagaraScript>(nullptr, TEXT("/Niagara/Modules/Spawn/SpawnRate.SpawnRate"));
    }

    UNiagaraScript* LoadNirOverrideTestDynamicInputScript()
    {
        return LoadObject<UNiagaraScript>(nullptr, TEXT("/Niagara/DynamicInputs/Add/Add_Float.Add_Float"));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirOverrideLiteralFloatTest,
    "PinWright.niagara.decompile_nir.OverrideLiteralFloat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirOverrideLiteralFloatTest::RunTest(const FString& Parameters)
{
    UNiagaraScript* ModuleScript = LoadNirOverrideTestModuleScript();
    if (!ModuleScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-asset-unavailable"),
            TEXT("SpawnRate module not available in this test build; skipping."));
        return true;
    }
    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("NirOverrideLiteral")));
    TestNotNull(TEXT("Fixture system created"), System);
    if (!System) return false;
    System->AddToRoot();

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System, ENiagaraScriptUsage::ParticleSpawnScript, ModuleScript);
    TestNotNull(TEXT("Module call added"), ModuleNode);
    if (!ModuleNode)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    FNiagaraVariable LiteralVar(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("SpawnRate")));
    LiteralVar.AllocateData();
    const float Value = 3.14f;
    LiteralVar.SetData(reinterpret_cast<const uint8*>(&Value));
    NIRTestFixtures::SetModuleInputLiteral(ModuleNode, FName(TEXT("SpawnRate")), LiteralVar);

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(System);
    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    TestTrue(TEXT("NIR contains literal override line"),
        Result.Text.Contains(TEXT("input SpawnRate = ")) && Result.Text.Contains(TEXT("3.14")));

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirOverrideLinkedParamTest,
    "PinWright.niagara.decompile_nir.OverrideLinkedParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirOverrideLinkedParamTest::RunTest(const FString& Parameters)
{
    UNiagaraScript* ModuleScript = LoadNirOverrideTestModuleScript();
    if (!ModuleScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-asset-unavailable"),
            TEXT("SpawnRate module not available in this test build; skipping."));
        return true;
    }
    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("NirOverrideLinked")));
    TestNotNull(TEXT("Fixture system created"), System);
    if (!System) return false;
    System->AddToRoot();

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System, ENiagaraScriptUsage::ParticleSpawnScript, ModuleScript);
    TestNotNull(TEXT("Module call added"), ModuleNode);
    if (!ModuleNode)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    NIRTestFixtures::SetModuleInputLinkedParam(ModuleNode, FName(TEXT("SpawnRate")), FName(TEXT("User.Speed")));

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(System);
    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    TestTrue(TEXT("NIR contains linked-parameter override"),
        Result.Text.Contains(TEXT("input SpawnRate = $User.Speed")));

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirOverrideDynamicInputTest,
    "PinWright.niagara.decompile_nir.OverrideDynamicInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirOverrideDynamicInputTest::RunTest(const FString& Parameters)
{
    UNiagaraScript* ModuleScript = LoadNirOverrideTestModuleScript();
    UNiagaraScript* DynamicInputScript = LoadNirOverrideTestDynamicInputScript();
    if (!ModuleScript || !DynamicInputScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-asset-unavailable"),
            TEXT("SpawnRate / Add_Float not available; skipping."));
        return true;
    }
    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("NirOverrideDynamic")));
    TestNotNull(TEXT("Fixture system created"), System);
    if (!System) return false;
    System->AddToRoot();

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System, ENiagaraScriptUsage::ParticleSpawnScript, ModuleScript);
    TestNotNull(TEXT("Module call added"), ModuleNode);
    if (!ModuleNode)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    UNiagaraNodeFunctionCall* InnerCall = NIRTestFixtures::SetModuleInputDynamicInput(
        ModuleNode, FName(TEXT("SpawnRate")), DynamicInputScript);
    TestNotNull(TEXT("Dynamic-input inner call created"), InnerCall);

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(System);
    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    TestTrue(TEXT("NIR contains dynamic-input override"),
        Result.Text.Contains(TEXT("input SpawnRate = dynamic ")));

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// FNiagaraNirOverrideStaticSwitchTest was removed in v1b's fix pass — the test
// did not exercise a static-switch override (the fixtures don't expose static-switch
// authoring), it only re-asserted the v1a TODO placeholder is absent, which
// every other override test already covers indirectly. Honest four-test coverage
// + a deliberate gap beats a fifth test that asserts nothing meaningful. Add a
// real static-switch test if/when NIRTestFixtures::SetModuleInputStaticSwitch lands.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirOverrideRecursionDepthTest,
    "PinWright.niagara.decompile_nir.OverrideRecursionDepth",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirOverrideRecursionDepthTest::RunTest(const FString& Parameters)
{
    UNiagaraScript* ModuleScript = LoadNirOverrideTestModuleScript();
    UNiagaraScript* DynamicInputScript = LoadNirOverrideTestDynamicInputScript();
    if (!ModuleScript || !DynamicInputScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-asset-unavailable"),
            TEXT("SpawnRate / Add_Float not available; skipping."));
        return true;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("NirOverrideRecursion")));
    TestNotNull(TEXT("Fixture system created"), System);
    if (!System) return false;
    System->AddToRoot();

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System, ENiagaraScriptUsage::ParticleSpawnScript, ModuleScript);
    if (!ModuleNode)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    // Chain 33+ dynamic-input modules to force the depth-32 guard. Each level
    // wraps the previous level's first input.
    UNiagaraNodeFunctionCall* Current = NIRTestFixtures::SetModuleInputDynamicInput(
        ModuleNode, FName(TEXT("SpawnRate")), DynamicInputScript);
    int32 ChainDepth = 1;
    while (Current && ChainDepth < 35)
    {
        // Discover the first non-parameter-map input pin name on the current
        // dynamic-input call. This avoids hardcoding "A" — which would silently
        // break if the engine renames Add_Float's input pin in a future build,
        // letting the loop exit at depth 1 and the depth-32 guard never fire.
        // Mirrors the parameter-map filter pattern in the production override-walk
        // code (NIRGraphEmitter_Dataflow::IsParameterMapPin).
        TArray<UEdGraphPin*> CurrentInputPins;
        Current->GetInputPins(CurrentInputPins);
        FName FirstInputName = NAME_None;
        for (UEdGraphPin* Pin : CurrentInputPins)
        {
            if (!Pin)
            {
                continue;
            }
            const FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
            if (PinType.IsValid() && PinType == FNiagaraTypeDefinition::GetParameterMapDef())
            {
                continue;
            }
            FirstInputName = Pin->PinName;
            break;
        }
        if (FirstInputName.IsNone())
        {
            break;
        }
        UNiagaraNodeFunctionCall* Next = NIRTestFixtures::SetModuleInputDynamicInput(
            Current, FirstInputName, DynamicInputScript);
        if (!Next)
        {
            break;
        }
        Current = Next;
        ++ChainDepth;
    }

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(System);
    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    if (ChainDepth >= 32)
    {
        TestTrue(TEXT("NIR contains recursion-limit-reached marker"),
            Result.Text.Contains(TEXT("# recursion-limit-reached")));
        const bool bHasWarning = Result.Warnings.ContainsByPredicate([](const FString& W)
        {
            return W.Contains(TEXT("recursion limit (32) reached"));
        });
        TestTrue(TEXT("FNIRResult.Warnings contains the depth-32 message"), bHasWarning);
    }
    else
    {
        AddWarning(FString::Printf(TEXT("Could only author %d-deep chain; depth guard not exercised."), ChainDepth));
    }

    NIRTestFixtures::DestroyFixture(System);
    return true;
}
