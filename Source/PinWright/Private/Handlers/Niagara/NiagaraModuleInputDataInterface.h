// Copyright (c) 2026 Alexander Penkin. MIT License.

// Resolve the UNiagaraDataInterface that backs a PLACED module's stack input.
//
// WHY THIS EXISTS. The curve DIs that drive real content are not parameter-store entries.
// `ScaleSpriteSize.Uniform Curve Sprite Scale` and `ScaleColor -> FloatFromCurve.FloatCurve`
// live on the module function-call node's override pin as a graph-local UNiagaraNodeInput, or —
// before anything overrides them — only as the module SCRIPT's own default object. Neither is
// reachable through FNiagaraParameterStore, which is why every scope/name spelling handed to
// niagara.set_curve_keys came back DATA_INTERFACE_NOT_FOUND
// (B-niagara-set-curve-keys-unreachable-module-input-di). Both rapid-iteration stores were
// measured on an emitter owning three curve DIs and held no data-interface entry at all.
//
// SCOPE. This helper answers only "which DI object does `inputName` name on this module, and may
// it be written". The (asset, emitter, script stage, module) half is NOT duplicated here: callers
// resolve it exactly as niagara.set_module_input does, with NiagaraEdit::ResolveTarget and
// ENiagaraEditTargetKind::Module, which fills Target.ModuleNode + Target.Graph.
//
// THE WRITE RULE THAT MAKES THIS SAFE. A module input still at its script default resolves to the
// module ASSET's own default object — shared by every placement of that module in the project,
// engine content included. Writing there would edit /Niagara/Modules/... itself. EResolveMode
// therefore separates the two intents: Read hands the shared default back marked non-writable,
// and WriteCreateOverride first gives this placement its own override DI (seeded from the default
// so an edit that touches one channel keeps the authored shape of the rest).
#pragma once

#include "CoreMinimal.h"

#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "NiagaraTypes.h"

class UNiagaraDataInterface;
class UNiagaraNodeInput;

namespace NiagaraModuleInputDI
{
    enum class EResolveMode : uint8
    {
        // Return whatever drives the input today, including the module script's shared default
        // object. Never mutates the graph.
        Read,
        // Same, except a default-valued input first gets its own override pin + DI copy, so the
        // returned object belongs to this emitter and is safe to write.
        WriteCreateOverride
    };

    struct FResolvedModuleInputDI
    {
        UNiagaraDataInterface* DataInterface = nullptr;
        // "data"    — an override DI on this module's override pin (this asset's own object).
        // "default" — the module script's shared default object (Read mode only).
        FString ValueMode;
        // True when this call created the override pin and its DI.
        bool bCreatedOverride = false;
        // False only for the shared module-script default. A caller must refuse to write then.
        bool bWritable = false;
        // The input's declared type; always a data-interface type on success.
        FNiagaraTypeDefinition DeclaredType;
        // The input name as the module declares it (callers match case-insensitively).
        FString ResolvedInputName;
    };

    // Read the DI off a UNiagaraNodeInput. UNiagaraNodeInput is UCLASS(MinimalAPI), so
    // GetDataInterface() is public C++ but not exported and cannot be linked from this DLL, and
    // the backing UPROPERTY is private — UE reflection is the only route that works from here.
    PINWRIGHT_API UNiagaraDataInterface* GetInputNodeDataInterface(const UNiagaraNodeInput* InputNode);

    // Target must already carry a resolved ModuleNode + Graph. On success Out.DataInterface is
    // non-null. Errors: MODULE_NOT_FOUND (unresolved target), INVALID_ARGUMENT (empty inputName),
    // MODULE_INPUT_NOT_FOUND (no such stack input — the message lists the module's DI-typed
    // inputs so a caller can discover the spelling), INCOMPATIBLE_DATA_INTERFACE (the input is
    // not a data interface), MODULE_INPUT_OVERRIDE_LINKED (a dynamic input / parameter link /
    // expression drives the input, so there is no DI to edit), DATA_INTERFACE_NOT_FOUND (Read of
    // an input whose module script declares no default object), CREATE_FAILED (the engine's
    // override-DI construction returned nothing).
    //
    // WriteCreateOverride mutates the graph, so the caller must already hold the transaction and
    // have called Modify() on the graph, and must notify the graph afterwards.
    PINWRIGHT_API FNiagaraEditError ResolveModuleInputDataInterface(
        FNiagaraResolvedTarget& Target,
        const FString& InputName,
        EResolveMode Mode,
        FResolvedModuleInputDI& Out);
}
