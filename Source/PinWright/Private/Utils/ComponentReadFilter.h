// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UActorComponent;

struct PINWRIGHT_API FComponentReadFilter
{
    FString NameMatch;
    UClass* ComponentClass = nullptr;

    bool Matches(const UActorComponent* Component) const;
    bool Matches(const FString& ComponentName, const UClass* CandidateClass) const;
};

PINWRIGHT_API bool TryParseComponentReadFilter(
    const TSharedPtr<FJsonObject>& Payload,
    FComponentReadFilter& OutFilter,
    FString& OutErrorCode,
    FString& OutErrorMessage);
