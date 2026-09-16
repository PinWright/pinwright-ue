// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared filesystem-to-asset mapping for PinWright source formats.
#pragma once

#include "CoreMinimal.h"

namespace PinWrightPwSourcePaths
{
    // Derive /Game/<relative path>/<source basename> only for a source directly below the
    // current project's Content directory and carrying ExpectedExtension (including the dot).
    // Returns false with a reader-facing reason for every case where choosing a target would
    // require a guess. Explicit outputPath callers do not use this policy.
    PINWRIGHT_API bool TryDeriveOutputAssetPath(
        const FString& ResolvedSourcePath,
        const FString& ExpectedExtension,
        FString& OutAssetPath,
        FString& OutReason);
}
