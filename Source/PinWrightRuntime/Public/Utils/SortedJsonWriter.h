// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Compat/JsonKeyCompat.h"

// Sorted, deterministic, key-inlined JSON writer used by asset.dump output.
//
// Two requirements drive this writer:
// 1. Stable line order across runs — diffable on disk, round-trippable for tests.
//    `FJsonObject::Values` is a hash-ordered TMap, so the default
//    `FJsonSerializer::Serialize` produces non-deterministic output.
// 2. Inline primitive values with their key under TPrettyJsonPrintPolicy.
//    UE's `WriteIdentifierPrefix(Key)` + `WriteValue(value)` pair forces a
//    line break between the colon and the value. The two-arg `WriteValue(Key, value)`
//    overloads keep them on one line. Doubles vertical density for the LLM consumer.
//
// Header-exposed so test code can exercise the same path it ships in
// `AssetDumpHandler.cpp::AddJsonFile`.

namespace SortedJsonWriter
{
    template<class TJsonWriter>
    void WriteSortedValue(TSharedRef<TJsonWriter> Writer, const TSharedPtr<FJsonValue>& Value);

    template<class TJsonWriter>
    void WriteSortedObject(TSharedRef<TJsonWriter> Writer, const TSharedPtr<FJsonObject>& Obj)
    {
        if (!Obj.IsValid())
        {
            Writer->WriteObjectStart();
            Writer->WriteObjectEnd();
            return;
        }

        TArray<FString> Keys;
        for (const auto& Pair : Obj->Values)
        {
            Keys.Add(EARGCompat::JsonKeyToString(Pair.Key));
        }
        Keys.Sort();

        Writer->WriteObjectStart();
        for (const FString& Key : Keys)
        {
            const TSharedPtr<FJsonValue>& Val = Obj->Values[EARGCompat::JsonFieldKey(Key)];

            if (!Val.IsValid() || Val->IsNull())
            {
                Writer->WriteNull(Key);
                continue;
            }

            switch (Val->Type)
            {
            case EJson::String:
                Writer->WriteValue(Key, Val->AsString());
                break;
            case EJson::Number:
                Writer->WriteValue(Key, Val->AsNumber());
                break;
            case EJson::Boolean:
                Writer->WriteValue(Key, Val->AsBool());
                break;
            case EJson::Object:
            case EJson::Array:
                Writer->WriteIdentifierPrefix(Key);
                WriteSortedValue(Writer, Val);
                break;
            default:
                Writer->WriteNull(Key);
                break;
            }
        }
        Writer->WriteObjectEnd();
    }

    template<class TJsonWriter>
    void WriteSortedValue(TSharedRef<TJsonWriter> Writer, const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid() || Value->IsNull())
        {
            Writer->WriteNull();
            return;
        }

        switch (Value->Type)
        {
        case EJson::Object:
            WriteSortedObject(Writer, Value->AsObject());
            break;

        case EJson::Array:
            {
                const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
                Writer->WriteArrayStart();
                for (const TSharedPtr<FJsonValue>& Elem : Arr)
                {
                    WriteSortedValue(Writer, Elem);
                }
                Writer->WriteArrayEnd();
            }
            break;

        case EJson::String:
            Writer->WriteValue(Value->AsString());
            break;

        case EJson::Number:
            Writer->WriteValue(Value->AsNumber());
            break;

        case EJson::Boolean:
            Writer->WriteValue(Value->AsBool());
            break;

        default:
            Writer->WriteNull();
            break;
        }
    }

    inline FString SerializeSortedJsonObject(const TSharedPtr<FJsonObject>& Obj)
    {
        FString Out;
        TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
        WriteSortedObject(Writer, Obj);
        Writer->Close();
        return Out;
    }

    // Companion to SerializeSortedJsonObject for standalone values. Dispatches through
    // WriteSortedValue so both object and array roots emit with sorted keys; primitive
    // roots fall through to a bare value. UE's writer rejects a bare scalar at the root,
    // so a primitive is wrapped in a single-element array and unwrapped from the output.
    inline FString SerializeSortedJsonValue(const TSharedPtr<FJsonValue>& Value)
    {
        const bool bIsContainer = Value.IsValid() &&
            (Value->Type == EJson::Object || Value->Type == EJson::Array);

        FString Out;
        TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);

        if (bIsContainer)
        {
            WriteSortedValue(Writer, Value);
            Writer->Close();
            return Out;
        }

        // Wrap primitive/invalid roots in an array so the writer accepts them, then
        // strip the brackets and surrounding whitespace to recover the bare value.
        Writer->WriteArrayStart();
        WriteSortedValue(Writer, Value);
        Writer->WriteArrayEnd();
        Writer->Close();

        int32 Open = INDEX_NONE;
        int32 Close = INDEX_NONE;
        Out.FindChar(TEXT('['), Open);
        Out.FindLastChar(TEXT(']'), Close);
        if (Open != INDEX_NONE && Close != INDEX_NONE && Close > Open)
        {
            return Out.Mid(Open + 1, Close - Open - 1).TrimStartAndEnd();
        }
        return Out.TrimStartAndEnd();
    }
}
