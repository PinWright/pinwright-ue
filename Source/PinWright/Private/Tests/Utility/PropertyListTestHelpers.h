// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Shared helper for Tests/Utility/* tests that inspect property.list responses.
// Extracted because the plugin uses Unity builds: file-static / anonymous-namespace
// helpers with the same name across .cpp files in the same folder produce ODR
// collisions when Unity merges them into one translation unit.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace PropertyListTestHelpers
{
    inline TSharedPtr<FJsonObject> FindPropertyEntry(
        const TSharedPtr<FJsonObject>& ListResult,
        const FString& PropertyName)
    {
        const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
        if (!ListResult.IsValid() ||
            !ListResult->TryGetArrayField(TEXT("properties"), Properties) ||
            !Properties)
        {
            return nullptr;
        }

        for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
        {
            TSharedPtr<FJsonObject> PropertyObject =
                PropertyValue.IsValid() && PropertyValue->Type == EJson::Object
                    ? PropertyValue->AsObject()
                    : nullptr;
            if (!PropertyObject.IsValid())
            {
                continue;
            }

            FString Name;
            if (PropertyObject->TryGetStringField(TEXT("name"), Name) && Name == PropertyName)
            {
                return PropertyObject;
            }
        }
        return nullptr;
    }
}
