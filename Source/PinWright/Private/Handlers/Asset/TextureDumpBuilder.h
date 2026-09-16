// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UTexture;
class IInterface_AsyncCompilation;

namespace TextureDumpBuilder
{
    PINWRIGHT_API bool IsAsyncCompilationInProgress(
        const IInterface_AsyncCompilation* AsyncCompilation);

    // Returns true while the texture is backed by Unreal's temporary default
    // stand-in during asynchronous platform-data compilation. Callers that
    // expose runtime size / pixel format must retry instead of serializing the
    // stand-in's 32x32 PF_B8G8R8A8 values.
    PINWRIGHT_API bool IsCompiling(const UTexture* Texture);

    PINWRIGHT_API TSharedPtr<FJsonObject> BuildTextureJson(const UTexture* Texture);
}
