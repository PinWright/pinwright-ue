// Copyright (c) 2026 Alexander Penkin. MIT License.

// Texture create/save helpers defined in TextureHandler.cpp and shared with
// TextureAuthorHandler.cpp, so every texture.* creator makes and persists its asset the same way
// (path guard, source format, honest save report). Namespaced so the generic names cannot clash
// with another module-global symbol; both files see this one declaration, so a signature change
// is a compile error rather than a link-time mismatch.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UObject;
class UTexture2D;

namespace PinWrightTextureAssets
{
    // Returns nullptr with OutErrorCode / OutError set on a refusal: an invalid path, or
    // ASSET_ALREADY_EXISTS when anything (on disk or loaded, any class) already lives at the
    // output path. Nothing is created on a refusal.
    UTexture2D* CreateEmptyTexture(const FString& PackagePath, const FString& TextureName, int32 Width, int32 Height, bool bHDR,
        FString& OutErrorCode, FString& OutError);
    void McpSaveTextureToDisk(const TSharedPtr<FJsonObject>& Response, UObject* Texture, bool bSave);
}
