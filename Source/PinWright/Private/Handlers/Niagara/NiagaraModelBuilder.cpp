// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraModelBuilder.h"

#include "Compat/EngineVersionCompat.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/Niagara/NiagaraJsonHelpers.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"

#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"

class UNiagaraGraph;
class UNiagaraScript;
struct FVersionedNiagaraEmitterData;

namespace
{
    using namespace NiagaraJsonHelpers;

    constexpr const TCHAR* SchemaName = TEXT("pinwright.niagara-model.v1");
    constexpr const TCHAR* CompactProfile = TEXT("compact");

    TSharedPtr<FJsonObject> BuildAssetVersionModel(const FNiagaraAssetVersion& Version)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetNumberField(TEXT("major"), Version.MajorVersion);
        Obj->SetNumberField(TEXT("minor"), Version.MinorVersion);
        Obj->SetStringField(TEXT("guid"), Version.VersionGuid.ToString());
        Obj->SetBoolField(TEXT("visibleInVersionSelector"), Version.bIsVisibleInVersionSelector);
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> BuildRawDumpFileList()
    {
        // niagara_system.json / niagara_emitters.json were removed from asset.dump output
        // (see F-remove-niagara-json-sidecars). Provenance now lists only the sidecars
        // that are still emitted.
        TArray<TSharedPtr<FJsonValue>> Files;
        Files.Add(MakeStringValue(DumpFileNames::NiagaraParameters));
        Files.Add(MakeStringValue(DumpFileNames::NiagaraStack));
        Files.Add(MakeStringValue(DumpFileNames::NiagaraGraphs));
        Files.Add(MakeStringValue(DumpFileNames::NiagaraCompile));
        return Files;
    }

    TSharedPtr<FJsonObject> BuildReferenceProvenance()
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("rawGraphFile"), DumpFileNames::NiagaraGraphs);
        Obj->SetArrayField(TEXT("rawDumpFiles"), BuildRawDumpFileList());
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildGraphReference(const UNiagaraGraph* Graph, const UEdGraphNode* Node)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("rawGraphFile"), DumpFileNames::NiagaraGraphs);
        if (!Graph)
        {
            Obj->SetBoolField(TEXT("present"), false);
            Obj->SetStringField(TEXT("reason"), TEXT("graph_not_available"));
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("name"), Graph->GetName());
        Obj->SetStringField(TEXT("objectPath"), Graph->GetPathName());
        if (Node)
        {
            Obj->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString());
        }
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildModuleProvenance(const UNiagaraGraph* Graph, const UNiagaraNodeFunctionCall* Node)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("rawGraphFile"), DumpFileNames::NiagaraGraphs);
        Obj->SetObjectField(TEXT("graph"), BuildGraphReference(Graph, Node));
        if (!Graph || !Node)
        {
            Obj->SetStringField(TEXT("diagnostic"), TEXT("module_graph_or_node_not_available"));
        }
        return Obj;
    }

    bool IsScratchPadScript(const UNiagaraScript* Script, const FVersionedNiagaraEmitterData* EmitterData)
    {
        if (!Script || !EmitterData)
        {
            return false;
        }
        return (EmitterData->ScratchPads && EmitterData->ScratchPads->Scripts.Contains(Script)) ||
            (EmitterData->ParentScratchPads && EmitterData->ParentScratchPads->Scripts.Contains(Script));
    }

    FString GetScriptSourceKind(const UNiagaraScript* Script, const FVersionedNiagaraEmitterData* EmitterData)
    {
        if (!Script)
        {
            return TEXT("missing");
        }
        if (IsScratchPadScript(Script, EmitterData))
        {
            return TEXT("scratchPad");
        }
        if (!Script->IsAsset())
        {
            return TEXT("owned");
        }
        return TEXT("externalAsset");
    }

    TSharedPtr<FJsonObject> BuildScriptReferenceModel(
        const UNiagaraScript* Script,
        const FString& Usage,
        const FGuid& SelectedVersion,
        const FVersionedNiagaraEmitterData* EmitterData,
        const UNiagaraGraph* Graph,
        const FString& OwnerKind,
        const FString& OwnerName,
        const UObject* Owner);

    TSharedPtr<FJsonObject> BuildEmitterStackModel(const FString& OwnerKind, const FString& OwnerName, const UObject* Owner, const FVersionedNiagaraEmitterData* EmitterData);
    TSharedPtr<FJsonObject> BuildEmitterSummaryModel(const FVersionedNiagaraEmitterData* EmitterData);
    TSharedPtr<FJsonObject> BuildEmitterFeatureModel(const FVersionedNiagaraEmitterData* EmitterData, const UObject* Owner, const FString& OwnerKind, const FString& OwnerName);

    TArray<TSharedPtr<FJsonValue>> BuildEmitterHandleModel(const UNiagaraSystem* System)
    {
        TArray<TSharedPtr<FJsonValue>> Emitters;
        if (!System)
        {
            return Emitters;
        }

        const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
        for (int32 Index = 0; Index < Handles.Num(); ++Index)
        {
            const FNiagaraEmitterHandle& Handle = Handles[Index];
            const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
            const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();

            TSharedPtr<FJsonObject> Obj = MakeObject();
            Obj->SetNumberField(TEXT("index"), Index);
            Obj->SetStringField(TEXT("name"), Handle.GetName().ToString());
            Obj->SetStringField(TEXT("id"), Handle.GetId().ToString());
            Obj->SetStringField(TEXT("idName"), Handle.GetIdName().ToString());
            Obj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
            Obj->SetBoolField(TEXT("valid"), Handle.IsValid());
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
            // FNiagaraEmitterHandle::GetEmitterMode()/ENiagaraEmitterMode are UE 5.4+; omit on 5.3.
            Obj->SetStringField(TEXT("mode"), EnumToString(Handle.GetEmitterMode()));
#endif
            Obj->SetStringField(TEXT("uniqueInstanceName"), Handle.GetUniqueInstanceName());
            Obj->SetBoolField(TEXT("needsRecompile"), Handle.NeedsRecompile());
            Obj->SetStringField(TEXT("emitterPath"), GetObjectPathSafe(Instance.Emitter));
            Obj->SetStringField(TEXT("version"), Instance.Version.ToString());
            if (Instance.Emitter)
            {
                Obj->SetStringField(TEXT("uniqueEmitterName"), Instance.Emitter->GetUniqueEmitterName());
            }

            TSharedPtr<FJsonObject> Source = MakeObject();
            Source->SetStringField(TEXT("lineage"), TEXT("resolvedSnapshot"));
            Source->SetObjectField(TEXT("instance"), NiagaraModelBuilder::BuildStructuredReference(
                TEXT("emitter_instance"),
                Handle.GetName().ToString(),
                Instance.Emitter,
                System,
                TEXT("NiagaraSystem")));
            if (EmitterData)
            {
                const FVersionedNiagaraEmitter Parent = EmitterData->GetParent();
                TSharedPtr<FJsonObject> ParentObj = MakeObject();
                ParentObj->SetBoolField(TEXT("present"), Parent.Emitter != nullptr);
                ParentObj->SetStringField(TEXT("path"), GetObjectPathSafe(Parent.Emitter));
                ParentObj->SetStringField(TEXT("version"), Parent.Version.ToString());
                if (Parent.Emitter)
                {
                    ParentObj->SetStringField(TEXT("uniqueEmitterName"), Parent.Emitter->GetUniqueEmitterName());
                }
                Source->SetObjectField(TEXT("parent"), ParentObj);
            }
            Obj->SetObjectField(TEXT("source"), Source);

            Obj->SetObjectField(TEXT("versionedEmitter"), BuildEmitterSummaryModel(EmitterData));
            Obj->SetObjectField(TEXT("scalability"), NiagaraDumpBuilder::BuildEmitterScalabilityModel(EmitterData));
            Obj->SetObjectField(TEXT("stack"), BuildEmitterStackModel(TEXT("emitter"), Handle.GetName().ToString(), Instance.Emitter, EmitterData));
            Obj->SetObjectField(TEXT("advancedFeatures"), BuildEmitterFeatureModel(EmitterData, Instance.Emitter, TEXT("emitter"), Handle.GetName().ToString()));
            Obj->SetArrayField(TEXT("renderers"), NiagaraModelBuilder::BuildRendererModel(Instance.Emitter));

            Emitters.Add(MakeObjectValue(Obj));
        }
        return Emitters;
    }

    TSharedPtr<FJsonObject> BuildScriptReferenceModel(
        const UNiagaraScript* Script,
        const FString& Usage,
        const FGuid& SelectedVersion,
        const FVersionedNiagaraEmitterData* EmitterData,
        const UNiagaraGraph* Graph,
        const FString& OwnerKind,
        const FString& OwnerName,
        const UObject* Owner)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("usage"), Usage);
        if (!Script)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("displayName"), Script->GetName());
        Obj->SetStringField(TEXT("objectPath"), Script->GetPathName());
        Obj->SetStringField(TEXT("class"), GetClassPathSafe(Script));
        Obj->SetStringField(TEXT("sourceKind"), GetScriptSourceKind(Script, EmitterData));
        Obj->SetStringField(TEXT("selectedVersion"), SelectedVersion.ToString());
        Obj->SetObjectField(TEXT("owner"), BuildOwnerRecord(OwnerKind, OwnerName, Owner));
        if (const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource()))
        {
            Obj->SetStringField(TEXT("sourcePath"), Source->GetPathName());
            if (Source->NodeGraph)
            {
                TSharedPtr<FJsonObject> GraphRef = MakeObject();
                GraphRef->SetStringField(TEXT("name"), Source->NodeGraph->GetName());
                GraphRef->SetStringField(TEXT("objectPath"), Source->NodeGraph->GetPathName());
                GraphRef->SetStringField(TEXT("rawGraphFile"), DumpFileNames::NiagaraGraphs);
                Obj->SetObjectField(TEXT("graph"), GraphRef);
            }
        }
        else if (Graph)
        {
            Obj->SetObjectField(TEXT("graph"), BuildGraphReference(Graph, nullptr));
        }
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildScriptReferenceModel(const UNiagaraScript* Script, const FString& Usage)
    {
        return BuildScriptReferenceModel(Script, Usage, FGuid(), nullptr, nullptr, FString(), FString(), nullptr);
    }

    TSharedPtr<FJsonObject> BuildEmptyStackModel(const FString& OwnerKind)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("ownerKind"), OwnerKind);
        Obj->SetArrayField(TEXT("systemSpawn"), TArray<TSharedPtr<FJsonValue>>());
        Obj->SetArrayField(TEXT("systemUpdate"), TArray<TSharedPtr<FJsonValue>>());
        Obj->SetArrayField(TEXT("emitterSpawn"), TArray<TSharedPtr<FJsonValue>>());
        Obj->SetArrayField(TEXT("emitterUpdate"), TArray<TSharedPtr<FJsonValue>>());
        Obj->SetArrayField(TEXT("particleSpawn"), TArray<TSharedPtr<FJsonValue>>());
        Obj->SetArrayField(TEXT("particleUpdate"), TArray<TSharedPtr<FJsonValue>>());
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildStackModuleModel(
        const UNiagaraNodeFunctionCall* Node,
        const UNiagaraGraph* Graph,
        const FString& OwnerKind,
        const FString& OwnerName,
        const UObject* Owner,
        const FString& ScriptUsage,
        const FString& StackKey,
        const FVersionedNiagaraEmitterData* EmitterData,
        int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("ownerKind"), OwnerKind);
        Obj->SetStringField(TEXT("ownerName"), OwnerName);
        Obj->SetStringField(TEXT("scriptUsage"), ScriptUsage);
        Obj->SetStringField(TEXT("semanticStack"), StackKey);
        Obj->SetNumberField(TEXT("index"), Index);
        if (!Node)
        {
            Obj->SetBoolField(TEXT("present"), false);
            Obj->SetObjectField(TEXT("provenance"), BuildModuleProvenance(Graph, nullptr));
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("entryId"), Node->NodeGuid.ToString());
        Obj->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString());
        Obj->SetStringField(TEXT("name"), Node->GetFunctionName());
        Obj->SetStringField(TEXT("nodeName"), Node->GetName());
        Obj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        Obj->SetStringField(TEXT("functionScript"), GetObjectPathSafe(Node->FunctionScript));
        Obj->SetStringField(TEXT("functionScriptAssetObjectPath"), Node->FunctionScriptAssetObjectPath.ToString());
        Obj->SetStringField(TEXT("selectedScriptVersion"), Node->SelectedScriptVersion.ToString());
        Obj->SetBoolField(TEXT("enabled"), Node->IsNodeEnabled());
        Obj->SetStringField(TEXT("desiredEnabledState"), LexToString(Node->GetDesiredEnabledState()));
        Obj->SetBoolField(TEXT("userSetEnabledState"), Node->HasUserSetTheEnabledState());

        TSharedPtr<FJsonObject> Position = MakeObject();
        Position->SetNumberField(TEXT("x"), Node->NodePosX);
        Position->SetNumberField(TEXT("y"), Node->NodePosY);
        Obj->SetObjectField(TEXT("position"), Position);
        Obj->SetNumberField(TEXT("posY"), Node->NodePosY);

        Obj->SetObjectField(TEXT("script"), BuildScriptReferenceModel(
            Node->FunctionScript,
            ScriptUsage,
            Node->SelectedScriptVersion,
            EmitterData,
            Graph,
            OwnerKind,
            OwnerName,
            Owner));
        Obj->SetArrayField(TEXT("dynamicInputs"), NiagaraModelBuilder::BuildDynamicInputModel(Node, Graph, Owner, OwnerKind, OwnerName));
        Obj->SetArrayField(TEXT("customHlsl"), NiagaraModelBuilder::BuildCustomHlslModel(Node, Graph, Owner, OwnerKind, OwnerName));
        Obj->SetArrayField(TEXT("staticSwitchInputs"), NiagaraDumpBuilder::BuildStaticSwitchInputs(Node));
        Obj->SetObjectField(TEXT("graph"), BuildGraphReference(Graph, Node));
        Obj->SetObjectField(TEXT("provenance"), BuildModuleProvenance(Graph, Node));
        return Obj;
    }

    void CollectUpstreamFunctionNodes(const UEdGraphNode* Node, TSet<const UEdGraphNode*>& VisitedNodes, TArray<const UNiagaraNodeFunctionCall*>& FunctionNodes)
    {
        if (!Node || VisitedNodes.Contains(Node))
        {
            return;
        }
        VisitedNodes.Add(Node);

        if (const UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node))
        {
            FunctionNodes.Add(FunctionCall);
        }

        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input)
            {
                continue;
            }
            for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                CollectUpstreamFunctionNodes(LinkedPin ? LinkedPin->GetOwningNode() : nullptr, VisitedNodes, FunctionNodes);
            }
        }
    }

    void CollectFunctionNodesForUsage(const UNiagaraGraph* Graph, ENiagaraScriptUsage ExpectedUsage, TArray<const UNiagaraNodeFunctionCall*>& FunctionNodes)
    {
        if (!Graph)
        {
            return;
        }

        // UNiagaraGraph::FindOutputNode is not exported by NiagaraEditor; iterate via the
        // base UEdGraph API to stay outside the engine's module-private surface.
        TArray<UNiagaraNodeOutput*> OutputNodes;
        const_cast<UNiagaraGraph*>(Graph)->GetNodesOfClass<UNiagaraNodeOutput>(OutputNodes);
        const UNiagaraNodeOutput* OutputNode = nullptr;
        for (const UNiagaraNodeOutput* Candidate : OutputNodes)
        {
            if (Candidate && Candidate->GetUsage() == ExpectedUsage)
            {
                OutputNode = Candidate;
                break;
            }
        }
        if (!OutputNode)
        {
            return;
        }

        TSet<const UEdGraphNode*> VisitedNodes;
        CollectUpstreamFunctionNodes(OutputNode, VisitedNodes, FunctionNodes);
    }

    void AddGraphStackModules(
        TSharedPtr<FJsonObject>& Stack,
        const FString& StackKey,
        const FString& OwnerKind,
        const FString& OwnerName,
        const UObject* Owner,
        const FString& ScriptUsage,
        ENiagaraScriptUsage ExpectedUsage,
        const FVersionedNiagaraEmitterData* EmitterData,
        const UNiagaraGraph* Graph)
    {
        TArray<TSharedPtr<FJsonValue>> Modules;
        if (Graph)
        {
            TArray<const UNiagaraNodeFunctionCall*> FunctionNodes;
            CollectFunctionNodesForUsage(Graph, ExpectedUsage, FunctionNodes);

            FunctionNodes.Sort([](const UNiagaraNodeFunctionCall& A, const UNiagaraNodeFunctionCall& B)
            {
                if (A.NodePosY != B.NodePosY)
                {
                    return A.NodePosY < B.NodePosY;
                }
                return A.NodeGuid.ToString() < B.NodeGuid.ToString();
            });

            for (int32 Index = 0; Index < FunctionNodes.Num(); ++Index)
            {
                Modules.Add(MakeObjectValue(BuildStackModuleModel(
                    FunctionNodes[Index],
                    Graph,
                    OwnerKind,
                    OwnerName,
                    Owner,
                    ScriptUsage,
                    StackKey,
                    EmitterData,
                    Index)));
            }
        }
        Stack->SetArrayField(StackKey, Modules);
    }

    void AddEmitterDataStacks(TSharedPtr<FJsonObject>& Stack, const FString& OwnerKind, const FString& OwnerName, const UObject* Owner, const FVersionedNiagaraEmitterData* EmitterData)
    {
        AddGraphStackModules(Stack, TEXT("emitterSpawn"), OwnerKind, OwnerName, Owner, TEXT("EmitterSpawnScript"), ENiagaraScriptUsage::EmitterSpawnScript, EmitterData, EmitterData ? GetGraphFromScript(EmitterData->EmitterSpawnScriptProps.Script) : nullptr);
        AddGraphStackModules(Stack, TEXT("emitterUpdate"), OwnerKind, OwnerName, Owner, TEXT("EmitterUpdateScript"), ENiagaraScriptUsage::EmitterUpdateScript, EmitterData, EmitterData ? GetGraphFromScript(EmitterData->EmitterUpdateScriptProps.Script) : nullptr);
        AddGraphStackModules(Stack, TEXT("particleSpawn"), OwnerKind, OwnerName, Owner, TEXT("ParticleSpawnScript"), ENiagaraScriptUsage::ParticleSpawnScript, EmitterData, EmitterData ? GetGraphFromScript(EmitterData->SpawnScriptProps.Script) : nullptr);
        AddGraphStackModules(Stack, TEXT("particleUpdate"), OwnerKind, OwnerName, Owner, TEXT("ParticleUpdateScript"), ENiagaraScriptUsage::ParticleUpdateScript, EmitterData, EmitterData ? GetGraphFromScript(EmitterData->UpdateScriptProps.Script) : nullptr);
    }

    TSharedPtr<FJsonObject> BuildSystemStackModel(const UNiagaraSystem* System)
    {
        TSharedPtr<FJsonObject> Obj = BuildEmptyStackModel(TEXT("system"));
        if (!System)
        {
            return Obj;
        }

        AddGraphStackModules(Obj, TEXT("systemSpawn"), TEXT("system"), System->GetName(), System, TEXT("SystemSpawnScript"), ENiagaraScriptUsage::SystemSpawnScript, nullptr, NiagaraJsonHelpers::GetGraphFromScript(System->GetSystemSpawnScript()));
        AddGraphStackModules(Obj, TEXT("systemUpdate"), TEXT("system"), System->GetName(), System, TEXT("SystemUpdateScript"), ENiagaraScriptUsage::SystemUpdateScript, nullptr, NiagaraJsonHelpers::GetGraphFromScript(System->GetSystemUpdateScript()));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmitterStackModel(const FString& OwnerKind, const FString& OwnerName, const UObject* Owner, const FVersionedNiagaraEmitterData* EmitterData)
    {
        TSharedPtr<FJsonObject> Obj = BuildEmptyStackModel(OwnerKind);
        AddEmitterDataStacks(Obj, OwnerKind, OwnerName, Owner, EmitterData);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmitterStackModel(const UNiagaraEmitter* Emitter)
    {
        const FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
        return BuildEmitterStackModel(TEXT("emitterAsset"), Emitter ? Emitter->GetName() : FString(), Emitter, EmitterData);
    }

    TSharedPtr<FJsonObject> BuildEmitterSourceModel(const UNiagaraEmitter* Emitter, const FVersionedNiagaraEmitterData* EmitterData, const UObject* Owner, const FString& OwnerKind)
    {
        TSharedPtr<FJsonObject> Source = MakeObject();
        Source->SetStringField(TEXT("lineage"), TEXT("resolvedSnapshot"));
        Source->SetObjectField(TEXT("instance"), NiagaraModelBuilder::BuildStructuredReference(
            TEXT("emitter_instance"),
            Emitter ? Emitter->GetName() : FString(),
            Emitter,
            Owner,
            OwnerKind));
        TSharedPtr<FJsonObject> ParentObj = MakeObject();
        ParentObj->SetBoolField(TEXT("present"), false);
        if (EmitterData)
        {
            const FVersionedNiagaraEmitter Parent = EmitterData->GetParent();
            ParentObj->SetBoolField(TEXT("present"), Parent.Emitter != nullptr);
            ParentObj->SetStringField(TEXT("path"), GetObjectPathSafe(Parent.Emitter));
            ParentObj->SetStringField(TEXT("version"), Parent.Version.ToString());
            if (Parent.Emitter)
            {
                ParentObj->SetStringField(TEXT("uniqueEmitterName"), Parent.Emitter->GetUniqueEmitterName());
            }
        }
        Source->SetObjectField(TEXT("parent"), ParentObj);
        return Source;
    }

    TSharedPtr<FJsonObject> BuildEmitterSummaryModel(const FVersionedNiagaraEmitterData* EmitterData)
    {
        TSharedPtr<FJsonObject> Summary = MakeObject();
        Summary->SetBoolField(TEXT("present"), EmitterData != nullptr);
        if (EmitterData)
        {
            Summary->SetObjectField(TEXT("version"), BuildAssetVersionModel(EmitterData->Version));
            Summary->SetBoolField(TEXT("deprecated"), EmitterData->bDeprecated);
            Summary->SetStringField(TEXT("deprecationMessage"), EmitterData->DeprecationMessage.ToString());
            Summary->SetBoolField(TEXT("localSpace"), EmitterData->bLocalSpace);
            Summary->SetBoolField(TEXT("determinism"), EmitterData->bDeterminism);
            Summary->SetStringField(TEXT("simTarget"), EnumToString(EmitterData->SimTarget));
            Summary->SetObjectField(TEXT("scalability"), NiagaraDumpBuilder::BuildEmitterScalabilityModel(EmitterData));
        }
        return Summary;
    }

    TSharedPtr<FJsonObject> BuildSimulationStageModel(const UNiagaraSimulationStageBase* Stage, int32 Index, const UObject* Owner, const FString& OwnerKind, const FString& OwnerName)
    {
        if (!Stage)
        {
            return NiagaraModelBuilder::BuildStructuredReference(
                TEXT("simulation_stage_not_lowered"),
                FString::Printf(TEXT("SimulationStage[%d]"), Index),
                nullptr,
                Owner,
                OwnerKind);
        }

        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("kind"), TEXT("simulation_stage"));
        Obj->SetNumberField(TEXT("index"), Index);
        Obj->SetStringField(TEXT("displayName"), Stage->SimulationStageName.ToString());
        Obj->SetStringField(TEXT("class"), GetClassPathSafe(Stage));
        Obj->SetStringField(TEXT("objectPath"), Stage->GetPathName());
        Obj->SetBoolField(TEXT("enabled"), Stage->bEnabled);
        Obj->SetObjectField(TEXT("script"), BuildScriptReferenceModel(
            Stage->Script,
            TEXT("ParticleSimulationStageScript"),
            FGuid(),
            nullptr,
            GetGraphFromScript(Stage->Script),
            OwnerKind,
            OwnerName,
            Owner));
        Obj->SetObjectField(TEXT("owner"), BuildOwnerRecord(OwnerKind, OwnerName, Owner));
        Obj->SetObjectField(TEXT("reference"), NiagaraModelBuilder::BuildStructuredReference(
            TEXT("simulation_stage_reference"),
            Stage->SimulationStageName.ToString(),
            Stage,
            Owner,
            OwnerKind,
            MakeObject(),
            MakeObject(),
            BuildGraphReference(GetGraphFromScript(Stage->Script), nullptr)));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEventHandlerModel(const FNiagaraEventScriptProperties& EventHandler, int32 Index, const UObject* Owner, const FString& OwnerKind, const FString& OwnerName)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("kind"), TEXT("event_handler"));
        Obj->SetNumberField(TEXT("index"), Index);
        Obj->SetStringField(TEXT("executionMode"), EnumToString(EventHandler.ExecutionMode));
        Obj->SetNumberField(TEXT("spawnNumber"), EventHandler.SpawnNumber);
        Obj->SetNumberField(TEXT("maxEventsPerFrame"), EventHandler.MaxEventsPerFrame);
        Obj->SetStringField(TEXT("sourceEmitterId"), EventHandler.SourceEmitterID.ToString());
        Obj->SetStringField(TEXT("sourceEventName"), EventHandler.SourceEventName.ToString());
        Obj->SetBoolField(TEXT("randomSpawnNumber"), EventHandler.bRandomSpawnNumber);
        Obj->SetNumberField(TEXT("minSpawnNumber"), EventHandler.MinSpawnNumber);
        Obj->SetBoolField(TEXT("updateAttributeInitialValues"), EventHandler.UpdateAttributeInitialValues);
        Obj->SetObjectField(TEXT("script"), BuildScriptReferenceModel(
            EventHandler.Script,
            TEXT("EventScript"),
            FGuid(),
            nullptr,
            GetGraphFromScript(EventHandler.Script),
            OwnerKind,
            OwnerName,
            Owner));
        Obj->SetObjectField(TEXT("owner"), BuildOwnerRecord(OwnerKind, OwnerName, Owner));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmitterFeatureModel(const FVersionedNiagaraEmitterData* EmitterData, const UObject* Owner, const FString& OwnerKind, const FString& OwnerName)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        TArray<TSharedPtr<FJsonValue>> SimulationStages;
        TArray<TSharedPtr<FJsonValue>> EventHandlers;

        if (!EmitterData)
        {
            Obj->SetArrayField(TEXT("simulationStages"), SimulationStages);
            Obj->SetArrayField(TEXT("eventHandlers"), EventHandlers);
            Obj->SetObjectField(TEXT("gpuScript"), BuildScriptReferenceModel(nullptr, TEXT("ParticleGPUComputeScript")));
            return Obj;
        }

        const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
        for (int32 Index = 0; Index < Stages.Num(); ++Index)
        {
            SimulationStages.Add(MakeObjectValue(BuildSimulationStageModel(Stages[Index], Index, Owner, OwnerKind, OwnerName)));
        }

        const TArray<FNiagaraEventScriptProperties>& Handlers = EmitterData->GetEventHandlers();
        for (int32 Index = 0; Index < Handlers.Num(); ++Index)
        {
            EventHandlers.Add(MakeObjectValue(BuildEventHandlerModel(Handlers[Index], Index, Owner, OwnerKind, OwnerName)));
        }

        Obj->SetArrayField(TEXT("simulationStages"), SimulationStages);
        Obj->SetArrayField(TEXT("eventHandlers"), EventHandlers);
        Obj->SetObjectField(TEXT("gpuScript"), BuildScriptReferenceModel(
            EmitterData->GetGPUComputeScript(),
            TEXT("ParticleGPUComputeScript"),
            FGuid(),
            EmitterData,
            NiagaraJsonHelpers::GetGraphFromScript(EmitterData->GetGPUComputeScript()),
            OwnerKind,
            OwnerName,
            Owner));
        return Obj;
    }

    void AddDiagnostic(TArray<TSharedPtr<FJsonValue>>& Diagnostics, const FString& Severity, const FString& Code, const FString& Message)
    {
        TSharedPtr<FJsonObject> Diagnostic = MakeObject();
        Diagnostic->SetStringField(TEXT("severity"), Severity);
        Diagnostic->SetStringField(TEXT("code"), Code);
        Diagnostic->SetStringField(TEXT("message"), Message);
        Diagnostics.Add(MakeObjectValue(Diagnostic));
    }

    TSharedPtr<FJsonObject> BuildProvenance(const UObject* Asset, const FString& AssetKind)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("builder"), TEXT("NiagaraModelBuilder"));
        Obj->SetStringField(TEXT("profile"), CompactProfile);
        Obj->SetArrayField(TEXT("rawDumpFiles"), BuildRawDumpFileList());
        Obj->SetObjectField(TEXT("asset"), NiagaraModelBuilder::BuildStructuredReference(
            TEXT("source_asset"),
            Asset ? Asset->GetName() : FString(),
            Asset,
            Asset,
            AssetKind));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildBaseModel(const UObject* Asset, const FString& AssetKind, TArray<TSharedPtr<FJsonValue>>& Diagnostics)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("schema"), SchemaName);
        Obj->SetStringField(TEXT("profile"), CompactProfile);
        Obj->SetStringField(TEXT("assetKind"), AssetKind);
        Obj->SetStringField(TEXT("assetPath"), GetObjectPathSafe(Asset));
        Obj->SetStringField(TEXT("assetName"), Asset ? Asset->GetName() : FString());
        Obj->SetStringField(TEXT("assetClass"), GetClassPathSafe(Asset));
        Obj->SetObjectField(TEXT("provenance"), BuildProvenance(Asset, AssetKind));
        if (!Asset)
        {
            AddDiagnostic(Diagnostics, TEXT("error"), TEXT("ASSET_MISSING"), TEXT("Niagara model builder received a null asset."));
        }
        return Obj;
    }
}

namespace NiagaraModelBuilder
{
    TSharedPtr<FJsonObject> BuildSystemModelJson(const UNiagaraSystem* System)
    {
        TArray<TSharedPtr<FJsonValue>> Diagnostics;
        TSharedPtr<FJsonObject> Obj = BuildBaseModel(System, TEXT("NiagaraSystem"), Diagnostics);
        Obj->SetStringField(TEXT("systemPath"), GetObjectPathSafe(System));
        if (System)
        {
            Obj->SetStringField(TEXT("systemName"), System->GetName());
            TSharedPtr<FJsonObject> Scripts = MakeObject();
            Scripts->SetObjectField(TEXT("systemSpawn"), BuildScriptReferenceModel(System->GetSystemSpawnScript(), TEXT("SystemSpawnScript")));
            Scripts->SetObjectField(TEXT("systemUpdate"), BuildScriptReferenceModel(System->GetSystemUpdateScript(), TEXT("SystemUpdateScript")));
            Obj->SetObjectField(TEXT("scripts"), Scripts);
            Obj->SetObjectField(TEXT("stack"), BuildSystemStackModel(System));
        }
        Obj->SetObjectField(TEXT("parameters"), NiagaraModelBuilder::BuildSystemParameterModel(System));
        Obj->SetObjectField(TEXT("scalability"), NiagaraDumpBuilder::BuildSystemScalabilityModel(System));
        Obj->SetArrayField(TEXT("emitters"), BuildEmitterHandleModel(System));
        Obj->SetArrayField(TEXT("diagnostics"), Diagnostics);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmitterModelJson(const UNiagaraEmitter* Emitter)
    {
        TArray<TSharedPtr<FJsonValue>> Diagnostics;
        TSharedPtr<FJsonObject> Obj = BuildBaseModel(Emitter, TEXT("NiagaraEmitter"), Diagnostics);
        Obj->SetStringField(TEXT("emitterPath"), GetObjectPathSafe(Emitter));
        const FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
        if (Emitter)
        {
            Obj->SetStringField(TEXT("emitterName"), Emitter->GetName());
            Obj->SetStringField(TEXT("uniqueEmitterName"), Emitter->GetUniqueEmitterName());
        }
        Obj->SetObjectField(TEXT("source"), BuildEmitterSourceModel(Emitter, EmitterData, Emitter, TEXT("NiagaraEmitter")));
        Obj->SetObjectField(TEXT("versionedEmitter"), BuildEmitterSummaryModel(EmitterData));
        Obj->SetObjectField(TEXT("scalability"), NiagaraDumpBuilder::BuildEmitterScalabilityModel(EmitterData));
        Obj->SetObjectField(TEXT("stack"), BuildEmitterStackModel(Emitter));
        Obj->SetObjectField(TEXT("advancedFeatures"), BuildEmitterFeatureModel(EmitterData, Emitter, TEXT("NiagaraEmitter"), Emitter ? Emitter->GetName() : FString()));
        Obj->SetArrayField(TEXT("renderers"), NiagaraModelBuilder::BuildRendererModel(Emitter));
        Obj->SetObjectField(TEXT("parameters"), NiagaraModelBuilder::BuildEmitterParameterModel(Emitter));
        Obj->SetArrayField(TEXT("diagnostics"), Diagnostics);
        return Obj;
    }
}
