// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UParticleSystem;

namespace CascadeDumpBuilder
{
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildCascadeJson(UParticleSystem* ParticleSystem);
}
