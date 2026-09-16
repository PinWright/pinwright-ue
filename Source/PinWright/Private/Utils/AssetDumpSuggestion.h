// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

namespace AssetDumpSuggestion
{
    enum class EDumpSubjectKind : uint8
    {
        Asset,
        Level
    };

    // Empty string when the subject's dump mirror exists and is fresh; otherwise an
    // imperative hint proposing asset.dump_folder targets derived from PackagePath.
    PINWRIGHT_API FString BuildDumpSuggestionHint(const FString& PackagePath, EDumpSubjectKind Kind);
}
