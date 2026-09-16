// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

PINWRIGHT_API bool ApplyJsonValueToProperty(void* TargetContainer, FProperty* Property,
                               const TSharedPtr<FJsonValue>& ValueField,
                               FString& OutError);

// Parse and atomically assign a JSON array into an already-initialized direct array value.
// The caller owns the direct value and controls its lifetime.
PINWRIGHT_API bool ApplyJsonValueToArrayDirect(
    FArrayProperty* ArrayProperty, void* DirectArrayValue,
    const TSharedPtr<FJsonValue>& ValueField, FString& OutError);

// True for reflected bool/numeric/enum fields whose scalar conversion can be staged without
// touching the containing object. Used by mutating handlers to validate before Modify().
PINWRIGHT_API bool IsJsonScalarProperty(const FProperty* Property);

// Parse and initialize a standalone scalar property value. The caller owns the returned value,
// and must destroy it with Property->DestroyValue() and release it with FMemory::Free().
PINWRIGHT_API bool TryStageJsonValueForProperty(FProperty* Property,
                                                const TSharedPtr<FJsonValue>& ValueField,
                                                void*& OutStagedValue,
                                                FString& OutError);

PINWRIGHT_API bool CoerceStringToPersistedFText(
    const FString& TextValue,
    const FText* ExistingText,
    FText& OutText,
    FString& OutError);

PINWRIGHT_API bool CoerceJsonValueToPersistedFText(
    const TSharedPtr<FJsonValue>& ValueField,
    const FText* ExistingText,
    FText& OutText,
    FString& OutError);

// Fallback property import using Unreal's ImportText_Direct.
// Handles ExportText format strings like "(R=1.0,G=0.5,B=0.0,A=1.0)" that
// ApplyJsonValueToProperty cannot parse. Returns true on success.
bool ImportTextToProperty(void* TargetContainer, FProperty* Property,
                          const FString& TextValue, FString& OutError);

// Coerce a flat string into a typed FJsonValue using the target FProperty as a
// shape guide. Shared between widget XML import and BPIR node_props replay so
// the per-property-type branch ladder does not drift between call sites.
PINWRIGHT_API TSharedPtr<FJsonValue> CoerceStringToJsonValueByProperty(
    const FString& StringValue, FProperty* TargetProperty);
