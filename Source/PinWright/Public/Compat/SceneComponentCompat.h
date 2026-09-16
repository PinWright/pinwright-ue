// Copyright (c) 2026 Alexander Penkin. MIT License.

// SceneComponentCompat.h
// USceneComponent::GetMobility() was added in UE 5.6; before that the only spelling is the
// public `Mobility` UPROPERTY the accessor returns. The accessor is still the preferred call
// on 5.6+ (Mobility is part of the engine's ongoing member-privatization work), so this shim
// resolves the two once rather than pinning every call site to the field.

#pragma once

#include "Compat/EngineVersionCompat.h"
#include "Components/SceneComponent.h"

inline EComponentMobility::Type PinWrightGetMobility(const USceneComponent& Component)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    return Component.GetMobility();
#else
    return Component.Mobility;
#endif
}
