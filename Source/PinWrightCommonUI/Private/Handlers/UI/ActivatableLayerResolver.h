// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"

class UObject;
class UCommonActivatableWidgetContainerBase;

// Resolves a CommonUI activatable-widget container from a CommonGame/Lyra UI layer
// gameplay tag (e.g. UI.Layer.Menu), the alternative addressing mode the ui.activatable_*
// methods accept beside host/stack. CommonGame's UPrimaryGameLayout is Lyra-only and is
// deliberately NOT linked by this plugin, so the whole path is reflection-based: it looks up
// the UPrimaryGameLayout UCLASS by /Script path and walks its reflectable `Layers`
// TMap<FGameplayTag, TObjectPtr<UCommonActivatableWidgetContainerBase>> UPROPERTY. On a host
// where CommonGame is absent, resolution degrades gracefully to LAYER_HOST_UNAVAILABLE.
namespace PinWrightUi
{
    // Walks the `Layers` FMapProperty on a resolved UPrimaryGameLayout instance (or any
    // object exposing that same-shaped map) and returns the container registered for
    // LayerTag. This is the reflection core, factored out so it can be exercised against an
    // in-code fixture without linking or instantiating CommonGame. Sets OutErrorCode to
    // LAYER_HOST_UNAVAILABLE (no/unexpected map), NOT_A_STACK (value is not a container), or
    // LAYER_NOT_FOUND (tag absent) on miss.
    UCommonActivatableWidgetContainerBase* ResolveStackFromLayersMap(
        UObject* LayoutInstance,
        const FGameplayTag& LayerTag,
        FString& OutErrorCode,
        FString& OutErrorMsg);

    // Full resolution for a live PIE session: resolves the UPrimaryGameLayout UCLASS by
    // reflection (LAYER_HOST_UNAVAILABLE if CommonGame is not present), finds the live
    // instance for PlayerIndex, converts LayerTagStr to a registered FGameplayTag
    // (LAYER_TAG_INVALID if unregistered), then delegates to ResolveStackFromLayersMap.
    UCommonActivatableWidgetContainerBase* ResolveStackByLayerTagInPie(
        const FString& LayerTagStr,
        int32 PlayerIndex,
        FString& OutErrorCode,
        FString& OutErrorMsg);
}
