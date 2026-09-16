// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class USubsurfaceProfile;

namespace SubsurfaceProfileDumpBuilder
{
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildSubsurfaceProfileJson(const USubsurfaceProfile* Profile);
}
