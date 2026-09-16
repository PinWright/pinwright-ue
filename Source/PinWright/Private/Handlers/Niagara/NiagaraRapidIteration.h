// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "NiagaraTypes.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UNiagaraEmitter;
class UNiagaraScript;
class UNiagaraSystem;
enum class ENiagaraScriptUsage : uint8;

namespace PinWrightNiagara
{
    // One rapid-iteration parameter store reachable from a Niagara asset, tagged with the label
    // the niagara.inspect `parameters` aspect publishes it under. The two emitter-stage stores
    // (emitterSpawn / emitterUpdate) have no niagara.set_parameter scope of their own — they are
    // reported so a write-through result can name every store it touched, not so a caller can
    // address them.
    struct FRapidIterationStoreRef
    {
        UNiagaraScript* Script = nullptr;
        FString Scope;
    };

    // Every rapid-iteration store the asset owns, in the order the engine's own compile-time
    // dependency copy consumes them (emitter-stage sources before the system-script mirrors they
    // feed). System may be null for a standalone Niagara Emitter asset and Emitter for a System.
    PINWRIGHT_API void CollectRapidIterationStores(
        UNiagaraSystem* System,
        UNiagaraEmitter* Emitter,
        TArray<FRapidIterationStoreRef>& OutStores);

    // The rapid-iteration constant a compiled script reads for one placed module input:
    // "Constants.<UniqueEmitterName>.<ModuleFunctionName>.<InputName>", or, for a module living in
    // a system stage, "Constants.<ModuleFunctionName>.<InputName>". AliasedInputName is the
    // "<ModuleFunctionName>.<InputName>" handle FNiagaraParameterHandle::CreateAliasedModuleParameterHandle
    // produces. Mirrors the unexported FNiagaraStackGraphUtilities::CreateRapidIterationParameter,
    // including its rule for when the emitter segment is omitted.
    PINWRIGHT_API FName MakeRapidIterationConstantName(
        FName AliasedInputName,
        const FString& UniqueEmitterName,
        ENiagaraScriptUsage OwningUsage);

    // What a module-input write or reset found and did in the rapid-iteration stores.
    struct FRapidIterationWriteThrough
    {
        // The constant that would shadow this input, whether or not one exists.
        FString ParameterName;
        // True when at least one store already held that constant.
        bool bShadowed = false;
        // What was done to it: "updated" (the input's value was written through), "removed" (the
        // constant was deleted, so the next compile regenerates it from the module's own default),
        // or empty when no constant existed and nothing was touched.
        FString Action;
        // Canonical pin-default text of the value the constant held before this call, from the
        // first store it was found in. Empty when nothing was shadowed or the value would not encode.
        FString PreviousValue;
        // Scope labels of the stores this call changed.
        TArray<FString> UpdatedScopes;
        // Scope labels of stores that hold the constant at a type the write could not match, so
        // the shadow is reported but survives. Empty on every ordinary path.
        TArray<FString> TypeMismatchScopes;
    };

    // Push a module input's literal into every rapid-iteration store that already holds the
    // matching constant, so the override pin and the store the simulation reads agree.
    //
    // Existing entries only: a store that does not already carry the constant is left alone. An
    // input whose constant was never generated is driven by its override pin alone (that is the
    // state a freshly added module's overridden inputs are in) and adding one would invent a
    // second source for it. ValueData is the input's user-facing bytes — the same encoding
    // UEdGraphSchema_Niagara::PinToNiagaraVariable yields for the override pin — and
    // FNiagaraParameterStore::SetParameterData applies the LWC conversion into the stored layout.
    PINWRIGHT_API void WriteThroughModuleInputConstant(
        UNiagaraSystem* System,
        UNiagaraEmitter* Emitter,
        FName ConstantName,
        const FNiagaraTypeDefinition& InputType,
        const uint8* ValueData,
        FRapidIterationWriteThrough& OutResult);

    // Delete a module input's rapid-iteration constant from every store that holds it, the
    // counterpart of the write-through for the paths that remove the override pin.
    //
    // Removal rather than a value restore, because the engine's own compile pass is what supplies
    // the replacement: `UNiagaraScriptSource::InitializeNewParameters` seeds a **missing** constant
    // from the module script's default pin, and only for an input with no override pin — which is
    // exactly the state a reset leaves. So the next compile regenerates the constant at the module
    // template default, the same value `UNiagaraStackFunctionInput::Reset` writes back through
    // `SetLocalValue(DefaultInputValues.LocalStruct)`. Leaving the constant standing instead would
    // invert the defect this mirrors: the graph reverts and the runtime keeps the last written value.
    PINWRIGHT_API void RemoveModuleInputConstant(
        UNiagaraSystem* System,
        UNiagaraEmitter* Emitter,
        FName ConstantName,
        FRapidIterationWriteThrough& OutResult);

    // Value snapshot of a system's rapid-iteration stores, taken before a compile and merged back
    // after it.
    //
    // A compile rebuilds each system script's store from that script's own graph traversal and
    // then overwrites it with the emitter-stage stores it depends on, so any value written
    // straight into a system-script store — the only store niagara.set_parameter can address for
    // an emitter-scoped module constant — is discarded and replaced by the emitter-side value.
    // The merge writes those values back onto the parameters that survived the rebuild.
    class PINWRIGHT_API FRapidIterationValueSnapshot
    {
    public:
        // Snapshot every rapid-iteration store of the system. Safe to call on a system with no
        // scripts; the snapshot is then empty and the merge a no-op.
        void Capture(UNiagaraSystem& System);

        // Write back every snapshotted value whose parameter still exists at the same type and
        // whose value the compile changed. Parameters the rebuild removed are NOT re-added:
        // re-introducing a parameter the traversal no longer reaches makes the next compile
        // detect a count mismatch and rebuild again. Returns how many values were restored.
        int32 MergeBack();

        int32 Num() const { return Entries.Num(); }

    private:
        struct FEntry
        {
            TWeakObjectPtr<UNiagaraScript> Script;
            FNiagaraVariable Variable;
            TArray<uint8> Data;
        };

        TArray<FEntry> Entries;
    };
}
