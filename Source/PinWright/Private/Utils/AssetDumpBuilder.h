// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UObjectRedirector;
class UWidgetBlueprint;
struct FBpirWarning;

namespace AssetDumpBuilder
{
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildMetaJson(UObject* Asset);
    PINWRIGHT_API FString BuildBpirText(UBlueprint* Blueprint);

    // True when AssetDumpHandler should emit bpir.txt for this blueprint. Returns false only when
    // the blueprint's parent class has no graph-bearing root in its ancestry AND every
    // ubergraph/function/macro graph is empty — preserves the empty-marker disambiguation for
    // graph-bearing parents (UUserWidget, AActor, UActorComponent, UAnimInstance, etc.).
    PINWRIGHT_API bool ShouldEmitBpirText(const UBlueprint* Blueprint);
    PINWRIGHT_API FString BuildOverriddenWidgetXml(UWidgetBlueprint* WidgetBlueprint);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildScsJson(UBlueprint* Blueprint);

    // Returns the redirect-target path for a UObjectRedirector. Empty string when Redirector is
    // null or DestinationObject is null — UObjectRedirector exposes no `DestinationName` fallback,
    // so consumers must treat empty as "redirect target unresolved".
    PINWRIGHT_API FString ResolveRedirectorTarget(const UObjectRedirector* Redirector);

    // Append `# BPIR_WARN:` / `# BPIR_ERROR:` marker lines for each decompiler warning.
    // Reachable from tests so the severity→prefix mapping can be unit-pinned without
    // standing up a full Blueprint asset to drive BuildBpirText().
    PINWRIGHT_API void FormatBpirWarningMarkers(const TArray<FBpirWarning>& Warnings, FString& Output);
}
