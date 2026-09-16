// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlendSpace;

namespace BlendSpaceDumpBuilder
{
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildBlendSpaceJson(const UBlendSpace* BlendSpace);
}
