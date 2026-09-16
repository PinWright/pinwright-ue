// Copyright (c) 2026 Alexander Penkin. MIT License.

// Thin re-export header for NiagaraResetModuleInput::FindStackFunctionOverrideNode
// and NiagaraResetModuleInput::RemoveOverridePinAndChainedNodes.
// Included by tests that need to call these helpers directly.
// UNiagaraNodeParameterMapSet is forward-declared (its definition lives in a private
// Niagara editor header); callers only ever hold and pass pointers to it.
#pragma once

#include "CoreMinimal.h"

class UEdGraphPin;
class UNiagaraDataInterface;
class UNiagaraGraph;
class UNiagaraNodeFunctionCall;
class UNiagaraNodeParameterMapSet;
class FNiagaraParameterHandle;

namespace NiagaraResetModuleInput
{
    // Walk ModuleNode's parameter-map input pin to its upstream override node.
    // Returns null if no override node is connected (rapid-iteration path not taken).
    PINWRIGHT_API UNiagaraNodeParameterMapSet* FindStackFunctionOverrideNode(
        UNiagaraNodeFunctionCall& ModuleNode);

    // Remove OverridePin from the override node and recursively delete any chained
    // upstream nodes (dynamic-input function-call chains, NiagaraNodeInput linked-param
    // nodes). Records removed UNiagaraDataInterface objects in OutRemovedDI.
    // Reimplementation of FNiagaraStackGraphUtilities::RemoveNodesForStackFunctionInputOverridePin
    // which is not NIAGARAEDITOR_API.
    PINWRIGHT_API void RemoveOverridePinAndChainedNodes(
        UEdGraphPin& OverridePin,
        UNiagaraGraph& Graph,
        TArray<TWeakObjectPtr<UNiagaraDataInterface>>& OutRemovedDI);

    // Find this module input's override pin (matched by AliasedInputHandle) on the module's
    // override node and tear it down via RemoveOverridePinAndChainedNodes. Returns true if a
    // matching override pin was found and removed, false if there was nothing to clear.
    PINWRIGHT_API bool ClearModuleInputOverride(
        UNiagaraNodeFunctionCall& ModuleNode,
        const FNiagaraParameterHandle& AliasedInputHandle,
        UNiagaraGraph& Graph);
}
