// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Widgets/CommonActivatableWidgetContainer.h"
#include "ActivatableLayerTagFixture.generated.h"

// In-code stand-in for CommonGame's UPrimaryGameLayout. It exposes a `Layers` UPROPERTY with
// the exact TMap<FGameplayTag, TObjectPtr<UCommonActivatableWidgetContainerBase>> shape (same
// property name, key type, and value type) that UPrimaryGameLayout uses, so the production
// reflection walk (PinWrightUi::ResolveStackFromLayersMap) can be exercised end-to-end without
// linking, enabling, or instantiating the Lyra-only CommonGame module — which is absent on this
// host. Test-only fixture.
UCLASS()
class UActivatableLayerTagFixture : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    TMap<FGameplayTag, TObjectPtr<UCommonActivatableWidgetContainerBase>> Layers;
};
