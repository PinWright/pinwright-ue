// Copyright (c) 2026 Alexander Penkin. MIT License.

// JSON field helper utilities for PinWright
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Math/Float16.h"
#include "Math/Vector2D.h"

// Safely get a string field from a JSON object with a default value.
PINWRIGHT_API FString GetJsonStringField(const TSharedPtr<FJsonObject>& Obj, const FString& Field, const FString& Default = TEXT(""));

// Safely get a number field from a JSON object with a default value.
PINWRIGHT_API double GetJsonNumberField(const TSharedPtr<FJsonObject>& Obj, const FString& Field, double Default = 0.0);

// Safely get a boolean field from a JSON object with a default value.
PINWRIGHT_API bool GetJsonBoolField(const TSharedPtr<FJsonObject>& Obj, const FString& Field, bool Default = false);

// Safely get an integer field from a JSON object with a default value.
PINWRIGHT_API int32 GetJsonIntField(const TSharedPtr<FJsonObject>& Obj, const FString& Field, int32 Default = 0);

// Strict scalar parsers for generic reflected-property writes. String input follows the
// ParamTypeCheck literal grammar (no surrounding whitespace, exponent, suffix, or units),
// while JSON numbers are required to be finite and integral where requested. The integer
// overloads enforce the supplied inclusive range before returning a converted value.
PINWRIGHT_API bool TryParseStrictJsonNumber(const TSharedPtr<FJsonValue>& Value,
                                            double& OutValue, FString& OutError);
PINWRIGHT_API bool TryParseStrictJsonInteger(const TSharedPtr<FJsonValue>& Value,
                                             int64 MinValue, int64 MaxValue,
                                             int64& OutValue, FString& OutError);
PINWRIGHT_API bool TryParseStrictJsonUnsignedInteger(const TSharedPtr<FJsonValue>& Value,
                                                      uint64 MaxValue,
                                                      uint64& OutValue, FString& OutError);
PINWRIGHT_API bool TryParseStrictJsonBoolean(const TSharedPtr<FJsonValue>& Value,
                                             bool& OutValue, FString& OutError);

// Backward-compatible mutating helpers used by legacy call sites in split handlers.
PINWRIGHT_API void ReadVectorField(const TSharedPtr<FJsonObject>& Obj,
                                                    const TCHAR* FieldName,
                                                    FVector& Out,
                                                    const FVector& Default);
PINWRIGHT_API void ReadRotatorField(const TSharedPtr<FJsonObject>& Obj,
                                                     const TCHAR* FieldName,
                                                     FRotator& Out,
                                                     const FRotator& Default);

// Extracts a FVector from a JSON object field, returning a default when the field is absent or invalid.
// Supports object form with x/y/z keys or array form [x, y, z].
PINWRIGHT_API FVector ExtractVectorField(const TSharedPtr<FJsonObject>& Source,
                                                          const TCHAR* FieldName,
                                                          const FVector& DefaultValue);

// Parse an FVector from a JSON object field. Object form with x/y/z keys only; returns Default when absent or invalid.
PINWRIGHT_API FVector ParseVectorFromJson(const TSharedPtr<FJsonObject>& JsonObj,
                                                            const FString& FieldName,
                                                            const FVector& Default = FVector::ZeroVector);

// Parse an FRotator from a JSON object field. Object form with pitch/yaw/roll keys only; returns Default when absent or invalid.
PINWRIGHT_API FRotator ParseRotatorFromJson(const TSharedPtr<FJsonObject>& JsonObj,
                                                              const FString& FieldName,
                                                              const FRotator& Default = FRotator::ZeroRotator);

// Extracts a rotator value from a JSON object field, returning the provided default
// when the field is absent or cannot be parsed.
// Supports object form with pitch/yaw/roll keys or array form [pitch, yaw, roll].
PINWRIGHT_API FRotator ExtractRotatorField(const TSharedPtr<FJsonObject>& Source,
                                                            const TCHAR* FieldName,
                                                            const FRotator& DefaultValue);

// Builds a JSON object value of the form { "x": ..., "y": ... } for an FVector2f.
PINWRIGHT_API TSharedPtr<FJsonValue> JsonVec2(const FVector2f& Value);

// Builds a JSON number value from an FFloat16 (decoded to float for transport).
PINWRIGHT_API TSharedPtr<FJsonValue> JsonHalf(FFloat16 Value);

// Builds a JSON object value of the form { "x": ..., "y": ... } from two FFloat16 components.
PINWRIGHT_API TSharedPtr<FJsonValue> JsonHalfVec2(FFloat16 X, FFloat16 Y);

// Builds a JSON object value of the form { "x": ..., "y": ..., "z": ... } from three FFloat16 components.
PINWRIGHT_API TSharedPtr<FJsonValue> JsonHalfVec3(FFloat16 X, FFloat16 Y, FFloat16 Z);

// Builds a JSON object value of the form { "x": ..., "y": ..., "z": ..., "w": ... } from four FFloat16 components.
PINWRIGHT_API TSharedPtr<FJsonValue> JsonHalfVec4(FFloat16 X, FFloat16 Y, FFloat16 Z, FFloat16 W);

// Extracts an FVector2f from a JSON object field.
// Supports object form with x/y keys or array form [x, y]; returns true on success.
PINWRIGHT_API bool ExtractVector2fField(const TSharedPtr<FJsonObject>& Source,
                                                         const FString& FieldName,
                                                         FVector2f& Out);

// Extracts an FFloat16 from a JSON number field; returns true on success.
// Accepts any numeric value and converts via FFloat16::Set.
PINWRIGHT_API bool ExtractFloat16Field(const TSharedPtr<FJsonObject>& Source,
                                                        const FString& FieldName,
                                                        FFloat16& Out);

enum class ERejectUnknownKeysMode : uint8
{
    First,
    AllSorted
};

// Collect keys Obj contains that Allowed does not. First preserves the JSON object's iteration
// order and stops at the first miss; AllSorted returns every miss in lexical order.
PINWRIGHT_API bool RejectUnknownKeys(const TSharedPtr<FJsonObject>& Obj,
                                     const TArray<FString>& Allowed,
                                     TArray<FString>& OutUnknown,
                                     ERejectUnknownKeysMode Mode = ERejectUnknownKeysMode::First,
                                     ESearchCase::Type SearchCase = ESearchCase::CaseSensitive);

// Extracts top-level JSON objects from a string that may contain one or more
// JSON objects mixed with other text.
PINWRIGHT_API TArray<FString> ExtractTopLevelJsonObjects(const FString& In);

// Produce a lowercase hexadecimal representation of the UTF-8 encoding of a string.
PINWRIGHT_API FString HexifyUtf8(const FString& In);

// Build a JSON object { objectPath, className } describing a single UObject ref.
// Returns an empty JSON object when Object is nullptr.
PINWRIGHT_API TSharedPtr<FJsonObject> EmitObjectRef(const UObject* Object);

// Convert a TArray<FString> into a JSON value array (one FJsonValueString per element).
// The standard way to emit string lists (e.g. droppedTags, tagsAdded) in SendError data
// and result payloads.
PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> EmitStringArray(const TArray<FString>& Strings);
