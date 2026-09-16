// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "TestContainerMapGapHost.generated.h"

// Fixture UCLASS for TestContainerMapGapIteration.cpp. Hosts a TMap<FString,int32>
// and a TSet<FName> so the container.map.* / container.set.* handlers can be driven
// end-to-end against real reflected containers. The test removes a NON-LAST key so a
// surviving entry is left at an internal index >= Num() after the RemoveAt leaves a
// sparse-array gap — the case where a Num()-bounded scan loop under-iterates and
// silently drops the survivor (B-container-map-remove-no-rehash).
UCLASS()
class UTestContainerMapGapHost : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    TMap<FString, int32> StrIntMap;

    UPROPERTY()
    TSet<FName> NameSet;
};
