// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UPhysicsAsset;

namespace PhysicsAssetDumpBuilder
{
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildPhysicsAssetJson(const UPhysicsAsset* Asset);
}
