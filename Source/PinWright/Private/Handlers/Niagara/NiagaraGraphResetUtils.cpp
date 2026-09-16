// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraGraphResetUtils.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraCommon.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
// NiagaraNodeEmitter.h has lived in NiagaraEditor/Private on every supported engine (5.3-5.8);
// PinWright.Build.cs puts that directory on the private include path for exactly this reason
// (NiagaraNodeStaticSwitch.h is reached the same way).
#include "NiagaraNodeEmitter.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/UnrealType.h"

namespace PinWrightNiagara
{
    UEdGraphPin* FindParameterMapPin(TArrayView<UEdGraphPin* const> Pins)
    {
        for (UEdGraphPin* Pin : Pins)
        {
            if (!Pin)
            {
                continue;
            }
            const UEdGraphSchema_Niagara* NiagaraSchema = Cast<UEdGraphSchema_Niagara>(Pin->GetSchema());
            if (!NiagaraSchema)
            {
                continue;
            }
            if (NiagaraSchema->PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef())
            {
                return Pin;
            }
        }
        return nullptr;
    }

    UEdGraphPin* GetParameterMapInputPin(const UNiagaraNode& Node)
    {
        TArray<UEdGraphPin*> InputPins;
        const_cast<UNiagaraNode&>(Node).GetInputPins(InputPins);
        return FindParameterMapPin(InputPins);
    }

    void CollectModuleNodesForOutput(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModuleNodes)
    {
        TSet<const UNiagaraNode*> Visited;
        UNiagaraNode* CurrentNode = &OutputNode;
        while (CurrentNode && !Visited.Contains(CurrentNode))
        {
            Visited.Add(CurrentNode);
            UEdGraphPin* InputPin = GetParameterMapInputPin(*CurrentNode);
            if (!InputPin || InputPin->LinkedTo.Num() != 1 || !InputPin->LinkedTo[0])
            {
                return;
            }

            UNiagaraNode* PreviousNode = Cast<UNiagaraNode>(InputPin->LinkedTo[0]->GetOwningNode());
            if (UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(PreviousNode))
            {
                OutModuleNodes.Insert(ModuleNode, 0);
            }
            CurrentNode = PreviousNode;
        }
    }

    namespace Internal
    {
        UEdGraphPin* GetParameterMapInputPin(UNiagaraNode& Node)
        {
            return PinWrightNiagara::GetParameterMapInputPin(Node);
        }

        UEdGraphPin* GetParameterMapOutputPin(UNiagaraNode& Node)
        {
            TArray<UEdGraphPin*> OutputPins;
            Node.GetOutputPins(OutputPins);
            return FindParameterMapPin(OutputPins);
        }

        void MakeLinkTo(UEdGraphPin* PinA, UEdGraphPin* PinB)
        {
            if (!PinA || !PinB)
            {
                return;
            }
            PinA->MakeLinkTo(PinB);
            if (UEdGraphNode* OwnerA = PinA->GetOwningNode())
            {
                OwnerA->PinConnectionListChanged(PinA);
            }
            if (UEdGraphNode* OwnerB = PinB->GetOwningNode())
            {
                OwnerB->PinConnectionListChanged(PinB);
            }
        }
    }

    UNiagaraNodeOutput* ResetGraphForOutput(
        UNiagaraGraph& NiagaraGraph,
        ENiagaraScriptUsage ScriptUsage,
        const FGuid& ScriptUsageId,
        const FGuid& PreferredOutputNodeGuid,
        const FGuid& PreferredInputNodeGuid)
    {
        // Vendored verbatim from FNiagaraStackGraphUtilities::ResetGraphForOutput (UE 5.6,
        // NiagaraStackGraphUtilities.cpp). The engine version lacks NIAGARAEDITOR_API and
        // cannot be linked. The SystemSpawn/SystemUpdate RebuildEmitterNodes branch is
        // intentionally omitted — callers of THIS function only invoke for ParticleEvent /
        // ParticleSimulationStage usages, never the system-graph cases. The system-graph case
        // lives in RebuildSystemEmitterNodes below, which is its own vendoring.
        NiagaraGraph.Modify();

        UNiagaraNodeOutput* OutputNode = nullptr;
        for (UEdGraphNode* GraphNode : NiagaraGraph.Nodes)
        {
            if (UNiagaraNodeOutput* Candidate = Cast<UNiagaraNodeOutput>(GraphNode))
            {
                if (Candidate->GetUsage() == ScriptUsage && Candidate->GetUsageId() == ScriptUsageId)
                {
                    OutputNode = Candidate;
                    break;
                }
            }
        }

        UEdGraphPin* OutputNodeInputPin = OutputNode != nullptr ? Internal::GetParameterMapInputPin(*OutputNode) : nullptr;
        if (OutputNode != nullptr && OutputNodeInputPin == nullptr)
        {
            NiagaraGraph.RemoveNode(OutputNode);
            OutputNode = nullptr;
        }

        if (OutputNode == nullptr)
        {
            FGraphNodeCreator<UNiagaraNodeOutput> OutputNodeCreator(NiagaraGraph);
            OutputNode = OutputNodeCreator.CreateNode();
            OutputNode->SetUsage(ScriptUsage);
            OutputNode->SetUsageId(ScriptUsageId);
            OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
            OutputNodeCreator.Finalize();

            if (PreferredOutputNodeGuid.IsValid())
            {
                OutputNode->NodeGuid = PreferredOutputNodeGuid;
            }

            OutputNodeInputPin = Internal::GetParameterMapInputPin(*OutputNode);
        }
        else
        {
            OutputNode->Modify();
        }

        FGraphNodeCreator<UNiagaraNodeInput> InputNodeCreator(NiagaraGraph);
        UNiagaraNodeInput* InputNode = InputNodeCreator.CreateNode();
        InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
        InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
        InputNodeCreator.Finalize();

        if (PreferredInputNodeGuid.IsValid())
        {
            InputNode->NodeGuid = PreferredInputNodeGuid;
        }

        UEdGraphPin* InputNodeOutputPin = Internal::GetParameterMapOutputPin(*InputNode);
        if (OutputNodeInputPin)
        {
            OutputNodeInputPin->BreakAllPinLinks(true);
        }
        Internal::MakeLinkTo(OutputNodeInputPin, InputNodeOutputPin);

        return OutputNode;
    }

    namespace Internal
    {
        // The SystemSpawn / SystemUpdate node graph. Both system scripts share one graph and the
        // spawn script is the way in, the same route FNiagaraStackGraphUtilities::RebuildEmitterNodes
        // takes. Const is widened once here: every pin helper below, and the rebuild itself, need
        // the mutable graph.
        UNiagaraGraph* ResolveSystemNodeGraph(const UNiagaraSystem& System)
        {
            UNiagaraScript* SpawnScript = const_cast<UNiagaraSystem&>(System).GetSystemSpawnScript();
            if (!SpawnScript)
            {
                return nullptr;
            }
            UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(SpawnScript->GetLatestSource());
            return ScriptSource ? ScriptSource->NodeGraph : nullptr;
        }

        // UNiagaraGraph::FindOutputNode is declared without NIAGARAEDITOR_API, so walk the nodes.
        UNiagaraNodeOutput* FindOutputNodeForUsage(UNiagaraGraph& Graph, ENiagaraScriptUsage Usage)
        {
            for (UEdGraphNode* GraphNode : Graph.Nodes)
            {
                UNiagaraNodeOutput* Candidate = Cast<UNiagaraNodeOutput>(GraphNode);
                if (Candidate && Candidate->GetUsage() == Usage)
                {
                    return Candidate;
                }
            }
            return nullptr;
        }

        // UNiagaraNodeEmitter is UCLASS(MinimalAPI): StaticClass() is exported, but
        // SetOwnerSystem / SetEmitterHandleId / GetEmitterHandleId are plain members of another
        // module and are not, so the node's identity is written and read through its UPROPERTYs.
        // A missing property means this engine no longer spells the node the way this code does,
        // and the callers turn that into a refusal rather than a half-built node.
        const FStructProperty* FindEmitterNodeHandleIdProperty()
        {
            const FStructProperty* HandleIdProperty = CastField<FStructProperty>(
                UNiagaraNodeEmitter::StaticClass()->FindPropertyByName(TEXT("EmitterHandleId")));
            return (HandleIdProperty && HandleIdProperty->Struct == TBaseStructure<FGuid>::Get())
                ? HandleIdProperty
                : nullptr;
        }

        const FObjectPropertyBase* FindEmitterNodeOwnerSystemProperty()
        {
            return CastField<FObjectPropertyBase>(
                UNiagaraNodeEmitter::StaticClass()->FindPropertyByName(TEXT("OwnerSystem")));
        }

        // Walks upstream from OutputNode along the single parameter-map link and records the
        // handle id of every emitter node the chain actually reaches. An emitter node sitting in
        // the graph but off the chain is never executed and is deliberately not recorded.
        void GatherInvokedHandleIds(
            UNiagaraNodeOutput& OutputNode,
            const FStructProperty& HandleIdProperty,
            TSet<FGuid>& OutHandleIds)
        {
            TSet<const UNiagaraNode*> Visited;
            UNiagaraNode* CurrentNode = &OutputNode;
            while (CurrentNode && !Visited.Contains(CurrentNode))
            {
                Visited.Add(CurrentNode);
                if (const UNiagaraNodeEmitter* EmitterNode = Cast<UNiagaraNodeEmitter>(CurrentNode))
                {
                    if (const FGuid* HandleId = HandleIdProperty.ContainerPtrToValuePtr<FGuid>(EmitterNode))
                    {
                        if (HandleId->IsValid())
                        {
                            OutHandleIds.Add(*HandleId);
                        }
                    }
                }

                UEdGraphPin* InputPin = PinWrightNiagara::GetParameterMapInputPin(*CurrentNode);
                if (!InputPin || InputPin->LinkedTo.Num() != 1 || !InputPin->LinkedTo[0])
                {
                    return;
                }
                CurrentNode = Cast<UNiagaraNode>(InputPin->LinkedTo[0]->GetOwningNode());
            }
        }
    }

    FSystemEmitterWiring ReadSystemEmitterWiring(const UNiagaraSystem& System)
    {
        FSystemEmitterWiring Wiring;

        UNiagaraGraph* SystemGraph = Internal::ResolveSystemNodeGraph(System);
        const FStructProperty* HandleIdProperty = Internal::FindEmitterNodeHandleIdProperty();
        if (!SystemGraph || !HandleIdProperty)
        {
            return Wiring;
        }

        UNiagaraNodeOutput* SpawnOutputNode = Internal::FindOutputNodeForUsage(*SystemGraph, ENiagaraScriptUsage::SystemSpawnScript);
        UNiagaraNodeOutput* UpdateOutputNode = Internal::FindOutputNodeForUsage(*SystemGraph, ENiagaraScriptUsage::SystemUpdateScript);
        if (!SpawnOutputNode || !UpdateOutputNode)
        {
            return Wiring;
        }

        Internal::GatherInvokedHandleIds(*SpawnOutputNode, *HandleIdProperty, Wiring.SpawnInvoked);
        Internal::GatherInvokedHandleIds(*UpdateOutputNode, *HandleIdProperty, Wiring.UpdateInvoked);
        Wiring.bGraphReadable = true;
        return Wiring;
    }

    bool FindUninvokedEmitterHandles(const UNiagaraSystem& System, TArray<FName>& OutUnwiredHandleNames)
    {
        const FSystemEmitterWiring Wiring = ReadSystemEmitterWiring(System);
        if (!Wiring.bGraphReadable)
        {
            return false;
        }

        for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
        {
            if (!Wiring.IsHandleInvoked(Handle.GetId()))
            {
                OutUnwiredHandleNames.Add(Handle.GetName());
            }
        }
        return true;
    }

    int32 RebuildSystemEmitterNodes(UNiagaraSystem& System)
    {
        UNiagaraGraph* SystemGraph = Internal::ResolveSystemNodeGraph(System);
        const FStructProperty* HandleIdProperty = Internal::FindEmitterNodeHandleIdProperty();
        const FObjectPropertyBase* OwnerSystemProperty = Internal::FindEmitterNodeOwnerSystemProperty();
        if (!SystemGraph || !HandleIdProperty || !OwnerSystemProperty)
        {
            return INDEX_NONE;
        }

        UNiagaraNodeOutput* OutputNodes[2] = {
            Internal::FindOutputNodeForUsage(*SystemGraph, ENiagaraScriptUsage::SystemSpawnScript),
            Internal::FindOutputNodeForUsage(*SystemGraph, ENiagaraScriptUsage::SystemUpdateScript)
        };
        if (!OutputNodes[0] || !OutputNodes[1])
        {
            return INDEX_NONE;
        }

        SystemGraph->Modify();

        // Drop every existing emitter node first, healing the chain across each, so a handle that
        // was removed leaves no node naming it and a re-run cannot duplicate a pair. This is the
        // engine's order too.
        TArray<UNiagaraNodeEmitter*> ExistingEmitterNodes;
        SystemGraph->GetNodesOfClass<UNiagaraNodeEmitter>(ExistingEmitterNodes);
        for (UNiagaraNodeEmitter* ExistingNode : ExistingEmitterNodes)
        {
            if (!ExistingNode)
            {
                continue;
            }
            ExistingNode->Modify();
            UEdGraphPin* NodeInputPin = GetParameterMapInputPin(*ExistingNode);
            UEdGraphPin* NodeOutputPin = Internal::GetParameterMapOutputPin(*ExistingNode);
            UEdGraphPin* UpstreamPin = (NodeInputPin && NodeInputPin->LinkedTo.Num() == 1) ? NodeInputPin->LinkedTo[0] : nullptr;
            UEdGraphPin* DownstreamPin = (NodeOutputPin && NodeOutputPin->LinkedTo.Num() == 1) ? NodeOutputPin->LinkedTo[0] : nullptr;
            ExistingNode->DestroyNode();
            if (UpstreamPin && DownstreamPin)
            {
                Internal::MakeLinkTo(UpstreamPin, DownstreamPin);
            }
        }

        int32 CreatedNodes = 0;
        const TArray<FNiagaraEmitterHandle>& Handles = System.GetEmitterHandles();
        for (int32 HandleIndex = 0; HandleIndex < Handles.Num(); ++HandleIndex)
        {
            const FGuid HandleId = Handles[HandleIndex].GetId();
            for (int32 UsageIndex = 0; UsageIndex < 2; ++UsageIndex)
            {
                UNiagaraNodeEmitter* EmitterNode = nullptr;
                {
                    // Nothing may return between CreateNode and Finalize: ~FGraphNodeCreator
                    // asserts on an unfinalized node and that assert takes the editor down.
                    FGraphNodeCreator<UNiagaraNodeEmitter> EmitterNodeCreator(*SystemGraph);
                    EmitterNode = EmitterNodeCreator.CreateNode();
                    OwnerSystemProperty->SetObjectPropertyValue_InContainer(EmitterNode, &System);
                    *HandleIdProperty->ContainerPtrToValuePtr<FGuid>(EmitterNode) = HandleId;
                    EmitterNode->SetUsage(static_cast<ENiagaraScriptUsage>(
                        UsageIndex + static_cast<int32>(ENiagaraScriptUsage::EmitterSpawnScript)));
                    EmitterNode->NodePosX = -400 - 200 * (Handles.Num() - 1 - HandleIndex);
                    EmitterNode->NodePosY = UsageIndex * 150;
                    // Finalize allocates the InputMap / OutputMap pins the splice below needs.
                    EmitterNodeCreator.Finalize();
                }

                // Splice the node in immediately ahead of the system output node, so the chain
                // reads ...existing system stack... -> this emitter -> Output System Spawn/Update.
                UEdGraphPin* OutputNodeInputPin = GetParameterMapInputPin(*OutputNodes[UsageIndex]);
                UEdGraphPin* UpstreamPin = (OutputNodeInputPin && OutputNodeInputPin->LinkedTo.Num() > 0)
                    ? OutputNodeInputPin->LinkedTo[0]
                    : nullptr;
                if (OutputNodeInputPin)
                {
                    OutputNodeInputPin->BreakAllPinLinks(true);
                }
                if (UpstreamPin)
                {
                    Internal::MakeLinkTo(UpstreamPin, GetParameterMapInputPin(*EmitterNode));
                }
                Internal::MakeLinkTo(Internal::GetParameterMapOutputPin(*EmitterNode), OutputNodeInputPin);

                // Picks up the handle's display name and enabled state - the work the unlinkable
                // SetOwnerSystem / SetEmitterHandleId would otherwise have done. Called through
                // the base declaration because the override lives in a MinimalAPI class and only
                // its vtable entry is reachable from here.
                UNiagaraNode* EmitterNodeAsNiagaraNode = EmitterNode;
                EmitterNodeAsNiagaraNode->RefreshFromExternalChanges();
                ++CreatedNodes;
            }
        }

        return CreatedNodes;
    }
}
