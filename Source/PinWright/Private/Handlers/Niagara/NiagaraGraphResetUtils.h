// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UEdGraphPin;
class UNiagaraGraph;
class UNiagaraNode;
class UNiagaraNodeFunctionCall;
class UNiagaraNodeOutput;
class UNiagaraSystem;
enum class ENiagaraScriptUsage : uint8;

namespace PinWrightNiagara
{
    // Vendored substitute for FNiagaraStackGraphUtilities::ResetGraphForOutput. The engine
    // helper is declared in the NiagaraEditor module without NIAGARAEDITOR_API and so
    // cannot be linked from external modules.
    UNiagaraNodeOutput* ResetGraphForOutput(
        UNiagaraGraph& NiagaraGraph,
        ENiagaraScriptUsage ScriptUsage,
        const FGuid& ScriptUsageId,
        const FGuid& PreferredOutputNodeGuid = FGuid(),
        const FGuid& PreferredInputNodeGuid = FGuid());

    // Returns the first parameter-map-typed pin in the supplied range, or nullptr.
    UEdGraphPin* FindParameterMapPin(TArrayView<UEdGraphPin* const> Pins);

    // Returns the first parameter-map input pin of Node, or nullptr. Takes a const
    // reference (GetInputPins is non-const, so the const is cast away internally) so
    // both const readback walks and the non-const reset path can share one definition.
    UEdGraphPin* GetParameterMapInputPin(const UNiagaraNode& Node);

    // Walks upstream from OutputNode along the single ParameterMap input link, appending
    // each UNiagaraNodeFunctionCall in execution order (the order the stack runs and the
    // order move_module edits). A node is appended only once, and the walk stops at a pin
    // that is unlinked or has != 1 link, or when a cycle is detected. This is the single
    // canonical definition of stack execution order shared by the move_module edit path
    // (NiagaraEditHandler.cpp) and the inspect readback (NiagaraDumpBuilder.cpp); changing
    // how the chain is located must happen here so the two stay in lockstep.
    void CollectModuleNodesForOutput(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModuleNodes);

    // ---------------------------------------------------------------------------------------
    // System-graph emitter wiring
    //
    // An FNiagaraEmitterHandle on a UNiagaraSystem does nothing on its own. The system's
    // SystemSpawn / SystemUpdate graph must also carry a UNiagaraNodeEmitter that names the
    // handle and sits on the parameter-map chain feeding the matching UNiagaraNodeOutput; that
    // node is what invokes the emitter's spawn and update scripts. A system whose handle list
    // was populated without those nodes compiles, validates strict-clean, saves, lists the
    // emitter everywhere - and never spawns a particle, because nothing calls it.
    // ---------------------------------------------------------------------------------------

    // Which emitter handles the system graph actually invokes, read off the graph.
    struct FSystemEmitterWiring
    {
        // False when the system's spawn-script graph, either of its two output nodes, or the
        // UNiagaraNodeEmitter identity property could not be resolved. Nothing below is a
        // measurement in that case - an unanswerable question must not read as "nothing wired".
        bool bGraphReadable = false;

        // Handle ids reached by walking upstream from Output System Spawn / Output System Update.
        // A node present in the graph but off the chain is not invoked and is not recorded.
        TSet<FGuid> SpawnInvoked;
        TSet<FGuid> UpdateInvoked;

        bool IsHandleInvoked(const FGuid& HandleId) const
        {
            return bGraphReadable && SpawnInvoked.Contains(HandleId) && UpdateInvoked.Contains(HandleId);
        }
    };

    // Pure read; mutates nothing. Runs off the graph rather than off whatever the writer just
    // did, so it can contradict the write path.
    FSystemEmitterWiring ReadSystemEmitterWiring(const UNiagaraSystem& System);

    // Names of the system's emitter handles the system graph never invokes, in handle order.
    // Returns false when the graph could not be read at all, in which case OutUnwiredHandleNames
    // is left untouched and no verdict is available.
    bool FindUninvokedEmitterHandles(const UNiagaraSystem& System, TArray<FName>& OutUnwiredHandleNames);

    // Vendored substitute for FNiagaraStackGraphUtilities::RebuildEmitterNodes, which is also
    // declared without NIAGARAEDITOR_API. Drops every UNiagaraNodeEmitter, healing the chain
    // across each, then recreates a spawn/update pair per emitter handle spliced in ahead of the
    // system output nodes. Call it after any change to the system's emitter handle list.
    // Returns the number of nodes created, or INDEX_NONE when the system graph or the node's
    // reflected identity fields could not be resolved - in which case nothing was written.
    int32 RebuildSystemEmitterNodes(UNiagaraSystem& System);
}
