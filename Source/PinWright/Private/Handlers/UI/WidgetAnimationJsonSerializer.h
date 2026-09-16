// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UWidgetBlueprint;

namespace WidgetAnimationJson
{
    enum class EImportMode : uint8
    {
        Replace,
        Merge
    };

    bool ParseImportMode(const FString& RawMode, EImportMode& OutMode, FString& OutError);

    PINWRIGHT_API bool ExportAnimations(
        UWidgetBlueprint* WidgetBlueprint,
        TSharedPtr<FJsonObject>& OutDocument,
        TArray<FString>& OutWarnings,
        FString& OutError,
        bool bIncludeEventMetadata = false);

    bool ParseDocument(
        const TSharedPtr<FJsonObject>& Document,
        FString& OutError);

    bool ApplyAnimations(
        UWidgetBlueprint* WidgetBlueprint,
        const TSharedPtr<FJsonObject>& Document,
        EImportMode Mode,
        TArray<FString>& OutWarnings,
        FString& OutError);
}
