// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "TestPropertyListNameFilter.generated.h"

// Fixture for TestPropertyListNameFilter.cpp. Exercises the nameMatch /
// propertyNames filters on the property.list handler.
UCLASS()
class UTestPropertyListNameFilterHost : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    float ForcedAltitude = 0.f;

    UPROPERTY()
    float ForcedSpeed = 0.f;

    UPROPERTY()
    float ForcedHeading = 0.f;

    UPROPERTY()
    bool bIsActive = false;

    UPROPERTY()
    bool bIsReady = false;

    UPROPERTY()
    FString Description;
};
