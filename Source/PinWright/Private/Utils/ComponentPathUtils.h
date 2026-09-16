// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"

class UActorComponent;

namespace PinWrightRpc::ComponentPath
{
    /**
     * Resolves a component path of either:
     *  - "/Game/Path/BP_X.BP_X_C:SubobjectName" — Blueprint CDO subobject component (archetype).
     *    "/Game/Path/BP_X:SubobjectName" (the Blueprint asset) and
     *    "/Game/Path/BP_X.Default__BP_X_C:SubobjectName" (the CDO instance, the shape
     *    blueprint.scs.get and asset.dump emit) resolve to the same component.
     *  - "ActorLabel:ComponentName" or "ActorName.ComponentName" — live component on a world actor.
     * Returns nullptr and populates OutError on failure.
     */
    PINWRIGHT_API UActorComponent* Resolve(const FString& Path, FString& OutError);
}
