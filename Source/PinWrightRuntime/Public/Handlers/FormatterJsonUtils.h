// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Small JSON helpers shared across text formatters. Header-only (inline) so
// each translation unit gets its own copy — avoids cross-formatter linkage
// while keeping behavior identical across all formatters.
namespace FormatterJsonUtils
{
    // Compact stringification of a JSON value — matches the JS bridge's
    // `typeof v === 'string' ? v : JSON.stringify(v)` shape.
    inline FString StringifyValue(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid()) return TEXT("null");
        if (Value->Type == EJson::String) return Value->AsString();
        if (Value->Type == EJson::Number)
        {
            const double N = Value->AsNumber();
            if (FMath::IsFinite(N) && FMath::Floor(N) == N && FMath::Abs(N) < 1e15)
            {
                return FString::Printf(TEXT("%lld"), (int64)N);
            }
            return FString::SanitizeFloat(N);
        }
        if (Value->Type == EJson::Boolean)
        {
            return Value->AsBool() ? TEXT("true") : TEXT("false");
        }
        // Object / array / null — fall back to a JSON-serialized form.
        FString Out;
        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
        FJsonSerializer::Serialize(Value, TEXT(""), Writer);
        return Out;
    }

    // Returns a pointer to the array field if present, else nullptr.
    inline const TArray<TSharedPtr<FJsonValue>>* TryGetArrayField(
        const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
    {
        if (!Obj.IsValid())
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Obj->TryGetArrayField(Field, Arr))
        {
            return Arr;
        }
        return nullptr;
    }
}
