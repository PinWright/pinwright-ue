// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace StaticMeshTextEmitter
{
    PINWRIGHT_API FString BuildText(const TSharedPtr<FJsonObject>& StaticMeshJson);
}
