// Copyright (c) 2026 Alexander Penkin. MIT License.

// NiagaraJsonHelpers.h
//
// Shared helpers extracted from the per-file anonymous namespaces in:
//   NiagaraDumpBuilder.cpp, NiagaraModelBuilder.cpp, NiagaraModelParameters.cpp,
//   NiagaraModelReferences.cpp, NiagaraEditHandler.cpp, NiagaraAdvancedEditHandler.cpp
//
// Under unity builds UBT can merge multiple Niagara .cpp files into one TU.
// Identical anonymous-namespace helpers (MakeObject, GetObjectPathSafe, etc.)
// then collide at link time with C2084. Consolidating into a single named
// namespace yields one definition per program. Mirrors the AGIRCompilerHelpers
// fix applied to the cliff-handler family.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utils/JsonBuilders.h"

class FHandlerContext;
class UEdGraphPin;
class UNiagaraGraph;
class UNiagaraScript;
class UObject;
struct FNiagaraEditError;
struct FNiagaraEditOptions;
struct FNiagaraResolvedTarget;
struct FNiagaraTypeDefinition;

namespace NiagaraJsonHelpers
{
// MakeObject and BuildLinearColorJson are deduped against the canonical copies
// in JsonBuilders. Re-export them into this namespace so the per-file
// `using namespace NiagaraJsonHelpers;` consumers resolve them unqualified.
using JsonBuilders::MakeObject;
using JsonBuilders::BuildLinearColorJson;

// Wrap an FJsonObject in an FJsonValueObject for insertion into an array or field.
inline TSharedPtr<FJsonValue> MakeObjectValue(const TSharedPtr<FJsonObject>& Object)
{
    return MakeShared<FJsonValueObject>(Object);
}

// Wrap a string in an FJsonValueString for insertion into an array.
inline TSharedPtr<FJsonValue> MakeStringValue(const FString& Value)
{
    return MakeShared<FJsonValueString>(Value);
}

// Return the object's full path or an empty string if Object is null. Used
// across dump/model builders to populate `objectPath` fields without crashing
// on missing owners.
inline FString GetObjectPathSafe(const UObject* Object)
{
    return Object ? Object->GetPathName() : FString();
}

// Return the object's class path or an empty string if Object or its class is
// missing. Used for `class` fields in JSON payloads.
inline FString GetClassPathSafe(const UObject* Object)
{
    return Object && Object->GetClass() ? Object->GetClass()->GetPathName() : FString();
}

// Hex-encode a byte buffer (no separators, uppercase). Used to render raw
// parameter blobs in dump output.
inline FString BytesToHexString(const uint8* Data, int32 Size)
{
    static const TCHAR* Hex = TEXT("0123456789ABCDEF");
    FString Result;
    Result.Reserve(Size * 2);
    for (int32 Index = 0; Index < Size; ++Index)
    {
        const uint8 Byte = Data[Index];
        Result.AppendChar(Hex[(Byte >> 4) & 0x0F]);
        Result.AppendChar(Hex[Byte & 0x0F]);
    }
    return Result;
}

// Resolve a UEnum reflection for TEnum and emit the value's name. Falls back
// to the numeric value as a string if the enum isn't registered. Templates
// must live in the header so each TU instantiates them as needed.
template <typename TEnum>
FString EnumToString(TEnum Value)
{
    if (const UEnum* Enum = StaticEnum<TEnum>())
    {
        return Enum->GetNameStringByValue(static_cast<int64>(Value));
    }
    return FString::FromInt(static_cast<int32>(Value));
}

// Build a {kind, displayName, objectPath, class} record. Three-arg form used
// by NiagaraModelBuilder/NiagaraModelReferences. NiagaraModelParameters keeps
// a local two-arg overload (displayName derived from Owner->GetName()).
TSharedPtr<FJsonObject> BuildOwnerRecord(const FString& Kind, const FString& DisplayName, const UObject* Owner);

// Vec/quat JSON builders for the single-precision Niagara math types. Same
// shape as the per-file copies. (Color uses JsonBuilders::BuildLinearColorJson,
// re-exported above.)
TSharedPtr<FJsonObject> BuildVector3fJson(const FVector3f& Value);
TSharedPtr<FJsonObject> BuildVector4fJson(const FVector4f& Value);
TSharedPtr<FJsonObject> BuildQuat4fJson(const FQuat4f& Value);

// Forward NiagaraEdit errors to the request context. Returns true (and sends
// the JSON-RPC error) when Error.HasError(), false otherwise. Shared by
// NiagaraEditHandler and NiagaraAdvancedEditHandler.
bool SendNiagaraEditError(FHandlerContext& Ctx, const FNiagaraEditError& Error);

// Editor-only: walk a UNiagaraScript to its underlying graph via its source.
UNiagaraGraph* GetGraphFromScript(UNiagaraScript* Script);
const UNiagaraGraph* GetGraphFromScript(const UNiagaraScript* Script);

// Editor-only: serialize an FNiagaraTypeDefinition (name, size, kind flags,
// struct/class/enum path).
TSharedPtr<FJsonObject> BuildTypeModel(const FNiagaraTypeDefinition& Type);

// True when Pin is a UNiagaraNodeWithDynamicPins '+' add pin: a Misc-category pin
// carrying the "DynamicAddPin" subcategory. UNiagaraNodeWithDynamicPins::AddPinSubCategory
// is not NIAGARAEDITOR_API-exported (referencing it fails to link on 5.6), so we match the
// literal the engine initializes it to. Single definition shared by the Niagara graph
// handler and the NIR dataflow emitter (each formerly carried an identical local copy).
bool IsDynamicAddPin(const UEdGraphPin* Pin);

// Engine-side preamble shared by all six advanced-edit handlers and the curve
// handlers: stop live preview instances and mark system + emitter as
// transactionally modified. Caller still opens the FScopedTransaction so the
// scope's lifetime matches the call site's mutation block. Lifted out of
// per-file anonymous namespaces to avoid Unity-build ODR collisions (C2084).
//
// The instances it stops include the one backing an open Niagara toolkit's preview
// viewport, so the number stopped is recorded into Target.QuiescedInstances and
// reported by EndEmitterMutationScope as `quiescedInstances`. Target is non-const
// for that reason (B-niagara-mutation-scope-blanks-open-preview).
void BeginEmitterMutationScope(FNiagaraResolvedTarget& Target);

// Shared post-mutation tail: dirty + notify + compile/save. Result-shaping
// stays at the call site because each handler emits different fields.
TSharedPtr<FJsonObject> EndEmitterMutationScope(
    const TCHAR* Operation,
    FNiagaraResolvedTarget& Target,
    const FNiagaraEditOptions& Options);
} // namespace NiagaraJsonHelpers
