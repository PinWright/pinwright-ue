// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UNiagaraEmitter;
class UNiagaraGraph;
class UNiagaraNodeFunctionCall;
class UNiagaraSystem;
struct FNiagaraParameterStore;

namespace NiagaraModelBuilder
{
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildSystemModelJson(const UNiagaraSystem* System);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildEmitterModelJson(const UNiagaraEmitter* Emitter);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildSystemParameterModel(const UNiagaraSystem* System);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildEmitterParameterModel(const UNiagaraEmitter* Emitter);
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildParameterStoreModel(const FNiagaraParameterStore& Store, const FString& Scope, const UObject* Owner);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildStructuredReference(
        const FString& Reason,
        const FString& DisplayName,
        const UObject* Object,
        const UObject* Owner,
        const FString& OwnerKind,
        TSharedPtr<FJsonObject> Refs = TSharedPtr<FJsonObject>(),
        TSharedPtr<FJsonObject> Properties = TSharedPtr<FJsonObject>(),
        TSharedPtr<FJsonObject> Provenance = TSharedPtr<FJsonObject>());
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildRendererModel(const UNiagaraEmitter* Emitter);
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildCustomHlslModel(const UNiagaraNodeFunctionCall* ModuleNode, const UNiagaraGraph* Graph, const UObject* Owner, const FString& OwnerKind, const FString& OwnerName);
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildDynamicInputModel(const UNiagaraNodeFunctionCall* ModuleNode, const UNiagaraGraph* Graph, const UObject* Owner, const FString& OwnerKind, const FString& OwnerName);
}
