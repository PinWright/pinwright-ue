// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utils/ComponentReadFilter.h"

class AActor;

// Read-shaping options for ActorDescribeBuilder::BuildActorJson. Both are
// optional and additive: with the defaults (empty Fields, bIncludeComponents
// true) the output is byte-identical to the historical full describe shape.
struct FActorDescribeOptions
{
    // Case-insensitive allow-list of top-level keys to retain in the output
    // (e.g. {"label","transform"}). Empty = emit every key (no projection).
    // "schema" and "storage" are always retained as the response envelope.
    TArray<FString> Fields;

    // When false, the "components" array is omitted entirely (the
    // includeComponents:false / componentsMode:"none" shorthand). Ignored when
    // an explicit Fields allow-list is supplied (Fields is then authoritative).
    bool bIncludeComponents = true;

    bool HasFieldProjection() const { return Fields.Num() > 0; }
};

namespace ActorDescribeBuilder
{
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildActorJson(
        AActor* Actor,
        const FString& Storage = TEXT("live"),
        const FComponentReadFilter& ComponentFilter = FComponentReadFilter(),
        const FActorDescribeOptions& Options = FActorDescribeOptions());

    PINWRIGHT_API TSharedPtr<FJsonObject> BuildActorManifestEntry(
        AActor* Actor,
        const FString& RelativeFileName);

    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildComponentsJson(
        AActor* Actor,
        const FComponentReadFilter& ComponentFilter = FComponentReadFilter());
}
