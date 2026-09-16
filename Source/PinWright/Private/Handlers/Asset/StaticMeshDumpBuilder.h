// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UStaticMesh;

namespace StaticMeshDumpBuilder
{
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildStaticMeshJson(const UStaticMesh* Mesh);
}
