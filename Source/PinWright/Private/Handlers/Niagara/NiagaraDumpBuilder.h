// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UNiagaraEmitter;
class UNiagaraNodeFunctionCall;
class UNiagaraScript;
class UNiagaraSystem;
struct FVersionedNiagaraEmitterData;

namespace NiagaraDumpBuilder
{
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildStaticSwitchInputs(const UNiagaraNodeFunctionCall* Node);
    // Typed per-input schema for a placed module's stack inputs: each declared input's
    // name, Niagara type, current value mode (default/local/linked/dynamicInput), the
    // current value/linked-parameter/dynamic-input script, and enum options when the
    // input type is an enum. Surfaced under the per-module object in niagara.inspect's
    // stack aspect and the niagara_stack.json dump so agents can read the input schema
    // instead of guessing names/types against niagara.set_module_input.
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildModuleInputsJson(const UNiagaraNodeFunctionCall* Node);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildSystemScalabilityModel(const UNiagaraSystem* System);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildEmitterScalabilityModel(const FVersionedNiagaraEmitterData* EmitterData);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildSystemJson(const UNiagaraSystem* System);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildEmittersJson(const UNiagaraSystem* System);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildEmitterAssetJson(const UNiagaraEmitter* Emitter);
    // NameFilter, when non-empty, keeps only parameters whose name contains it
    // (case-insensitive substring) across every store, so niagara.inspect can read
    // back one User.* parameter inline instead of spilling the whole parameter set.
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildParametersJson(const UNiagaraSystem* System, const FString& NameFilter = FString());
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildEmitterParametersJson(const UNiagaraEmitter* Emitter, const FString& NameFilter = FString());
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildStackJson(const UNiagaraSystem* System);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildEmitterStackJson(const UNiagaraEmitter* Emitter);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildGraphsJson(const UNiagaraSystem* System);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildEmitterGraphsJson(const UNiagaraEmitter* Emitter);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildScriptGraphsJson(const UNiagaraScript* Script);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildCompileJson(const UNiagaraSystem* System);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildEmitterCompileJson(const UNiagaraEmitter* Emitter);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildScriptCompileJson(const UNiagaraScript* Script);
    // Live diagnostics used by niagara.inspect / niagara.validate. These retain
    // compile status, readiness, and on-demand compilation state intentionally.
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildCompileDiagnosticsJson(const UNiagaraSystem* System);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildEmitterCompileDiagnosticsJson(const UNiagaraEmitter* Emitter);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildScriptCompileDiagnosticsJson(const UNiagaraScript* Script);
}
