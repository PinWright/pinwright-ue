// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UNiagaraSystem;
class UNiagaraComponent;
class UNiagaraEmitter;

namespace PinWrightNiagara
{
    // Replacement for FNiagaraEditorUtilities::KillSystemInstances. The engine helper is
    // not exported with NIAGARAEDITOR_API, so we replicate its TObjectIterator logic here.
    //
    // Every mutation that swaps compiled bytecode or emitter handles under a system must run
    // this FIRST. UNiagaraComponent::DestroyInstance() drains the in-flight concurrent tick
    // (FNiagaraSystemInstance::Deactivate(bImmediate=true) -> WaitForConcurrentTickAndFinalize)
    // before releasing the instance, so the simulation cannot end up executing freshly compiled
    // bytecode against the data sets the old exec context allocated. That mismatch asserts
    // (`DataSetIdx < ExecCtx->DataSets.Num()`) inside the VectorVM on a task-graph worker, which
    // is an appError and takes the whole editor process down.
    //
    // Re-activation afterwards is the engine's job and is safe: RequestCompile ends by running an
    // FNiagaraSystemUpdateContext that reinitializes auto-activate components, and
    // UNiagaraComponent::Activate parks on HasOutstandingCompilationRequests() until the compile
    // lands, so the component restarts against the new compiled data rather than racing it.
    //
    // Returns how many RUNNING instances were stopped, i.e. how many of the swept components held
    // a valid FNiagaraSystemInstanceController at the moment of the sweep. Components merely
    // bound to the asset are swept too, but DestroyInstance on those stops nothing and the count
    // must not claim it. Callers surface the number as `quiescedInstances` so a caller whose
    // preview viewport went blank has a field naming the cause
    // (B-niagara-mutation-scope-blanks-open-preview, E-niagara-mutation-result-no-quiesced-count).
    int32 KillSystemInstances(const UNiagaraSystem& System);

    // The same sweep, counting instead of killing: how many components hold a live
    // FNiagaraSystemInstanceController for System right now.
    //
    // This is the "is anything going to tick it" question, and it has to be answerable WITHOUT
    // quiescing, because the answer decides whether quiescing is warranted at all. A write that
    // leaves a system data-interface-mismatched is only fatal when something ticks it; on a
    // system nothing is running, the cheap batch workflow (many edits, then one compile) stays
    // correct and must not be paid for with a destroyed preview per edit.
    int32 CountLiveSystemInstances(const UNiagaraSystem& System);

    // Same guard, widened to the reach of UNiagaraSystem::RequestCompileForEmitter: that call
    // recompiles EVERY loaded system using the emitter, so quiescing only the edited asset is not
    // enough. Kills the live instances of every loaded system referencing Emitter at VersionGuid.
    // Counts stopped instances the same way KillSystemInstances does.
    int32 KillSystemInstancesUsingEmitter(const UNiagaraEmitter& Emitter, const FGuid& VersionGuid);

    // The set of parameterType strings the runtime niagara.modify_parameter /
    // effect.set_niagara_parameter setters can actually write. This is intentionally the
    // narrow Float/Vector/Color/Bool subset of the broader asset-side
    // PinWrightNiagara::ResolveNiagaraParameterType vocabulary: the component setters only expose
    // SetFloatParameter/SetVectorParameter/SetColorParameter/SetBoolParameter, so accepting
    // Int/Vec2/Half/etc. here would validate a type the write dispatch cannot apply.
    // Single source of truth for both the "is this type valid" gate and ClassifyComponentParameter.
    bool IsSupportedRuntimeParameterType(const FString& ParameterType);

    // Three-way outcome of resolving a runtime parameter against the component's exposed
    // (User.) parameter store. Lets callers map a single result to INVALID_ARGUMENT vs
    // PARAMETER_NOT_FOUND without re-running the type allow-list themselves.
    enum class EParameterLookup : uint8
    {
        InvalidType, // ParameterType is not one of the runtime-supported strings
        NotFound,    // Valid type, but no matching name on the system's User. store
        Found        // Valid type and a matching exposed parameter exists
    };

    // Classifies whether a parameter named ParamName, matching the requested ParameterType
    // string ("Float" / "Vector" / "Color" / "Bool", case-insensitive), is declared on the
    // component's Niagara system's exposed (User.) parameter store.
    //
    // The runtime component setters (UNiagaraComponent::SetVariable*/SetFloatParameter/...)
    // forward to OverrideParameters.SetParameterValue(..., bAdd=true), which silently CREATES
    // an entry for any unknown name instead of rejecting it — so a typo'd / wrong-prefix /
    // wrong-type name is a no-op the engine never flags. Handlers must call this first to
    // distinguish a real tune from a silent no-op. Mirrors the asset-side validation used by
    // niagara.set_parameter (validates the name against System->GetExposedParameters()).
    //
    // Returns NotFound (fail closed) when Component or its asset is missing, and InvalidType
    // when ParameterType is unrecognized, so callers can report the right error code.
    EParameterLookup ClassifyComponentParameter(const UNiagaraComponent* Component, FName ParamName, const FString& ParameterType);
}
