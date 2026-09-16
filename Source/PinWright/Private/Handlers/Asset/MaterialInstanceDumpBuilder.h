// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UMaterialInstanceConstant;

namespace MaterialInstanceDumpBuilder
{
    // Builds the structured material_instance.json sidecar payload for a UMaterialInstanceConstant.
    // Returns nullptr when Instance is null. Shared by AssetDumpHandler (material_instance.json)
    // and material.authoring.get_material_instance_info (RPC handler enriches with parent
    // parameter metadata + inherited values).
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildMaterialInstanceJson(const UMaterialInstanceConstant* Instance);
}
