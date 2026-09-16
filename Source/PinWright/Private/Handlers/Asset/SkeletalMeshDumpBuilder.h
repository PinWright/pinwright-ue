// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class USkeletalMesh;

namespace SkeletalMeshDumpBuilder
{
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildSkeletalMeshJson(const USkeletalMesh* Mesh);
}
