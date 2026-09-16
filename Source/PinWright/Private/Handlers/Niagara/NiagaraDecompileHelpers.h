// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared Niagara decompile/inspect helpers. Promoted from the per-file anonymous
// namespaces in NiagaraDumpBuilder.cpp so NIRDecompiler can reuse them without
// duplicating the logic.

#pragma once

#include "CoreMinimal.h"

class UNiagaraGraph;
class UNiagaraNodeFunctionCall;
class UNiagaraScript;
struct FVersionedNiagaraEmitterData;

struct FNiagaraGpuIncompatibleModule
{
    FString ModuleName;
    FString NodeName;
    FString StackName;
};

namespace NiagaraDecompileHelpers
{
    // Reads the fx.Niagara.OnDemandCompile CVar. UE 5.6 defaults this on, deferring
    // Niagara compile until the editor opens the system or an FX component spawns it.
    PINWRIGHT_API bool IsNiagaraOnDemandCompileEnabled();

    // Collect all UNiagaraNodeFunctionCall nodes in Graph, sorted by NodePosY then
    // GUID. Stable order matches the visual stack ordering authors see in the editor.
    PINWRIGHT_API TArray<UNiagaraNodeFunctionCall*> CollectAndSortFunctionCalls(const UNiagaraGraph* Graph);

    PINWRIGHT_API bool IsGpuIncompatibleFunctionCall(const UNiagaraNodeFunctionCall* Node);
    PINWRIGHT_API TArray<FNiagaraGpuIncompatibleModule> CollectGpuIncompatibleModules(
        const FVersionedNiagaraEmitterData* EmitterData);
}
