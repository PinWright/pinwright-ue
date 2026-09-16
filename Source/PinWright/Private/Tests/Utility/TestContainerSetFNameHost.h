// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "TestContainerSetFNameHost.generated.h"

// Fixture UCLASS for TestContainerSetFNameLookup.cpp. Hosts a TSet<FName> so the
// container.set.contains / container.set.remove handlers can be driven end-to-end
// against a real reflected FName set (the same element type as the reported repro
// target UAssetManagerSettings.MetaDataTagsForAssetRegistry), without mutating an
// engine CDO.
UCLASS()
class UTestContainerSetFNameHost : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    TSet<FName> FNameSet;
};
