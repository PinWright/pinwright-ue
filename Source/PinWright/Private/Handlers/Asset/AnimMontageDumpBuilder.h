// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UAnimMontage;

namespace AnimMontageDumpBuilder
{
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildAnimMontageJson(const UAnimMontage* Montage);
}
