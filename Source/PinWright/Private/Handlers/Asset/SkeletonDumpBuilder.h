// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class USkeleton;
class USkeletalMeshSocket;
struct FVirtualBone;

namespace SkeletonDumpBuilder
{
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildSkeletonJson(const USkeleton* Skel);
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildVirtualBonesJson(TConstArrayView<FVirtualBone> VirtualBones);
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildSocketsJson(TConstArrayView<USkeletalMeshSocket*> Sockets);
}
