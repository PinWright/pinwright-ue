// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "TestContainerIntegerScalarCoercionHost.generated.h"

// Fixture UCLASS for TestContainerIntegerScalarCoercion.cpp. Hosts integer-keyed
// map and integer set properties so malformed scalar inputs can be checked against
// real reflected container handlers without mutating an engine asset.
UCLASS()
class UTestContainerIntegerScalarCoercionHost : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    TMap<int32, int32> IntKeyMap;

    UPROPERTY()
    TSet<int32> IntSet;
};
