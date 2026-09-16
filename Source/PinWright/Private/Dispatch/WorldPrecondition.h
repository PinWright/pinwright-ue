// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class FHandlerContext;
struct FAsyncResponseToken;
class FJsonObject;
class UWorld;

namespace PinWrightWorldPrecondition
{
    inline constexpr TCHAR ParamName[] = TEXT("expectWorld");

    // Resolves the world targeted by the handler family for Method. Editor-world
    // mutators use the editor context; runtime widget and Niagara-spawn mutators
    // use the same PIE-aware selector as their handlers.
    PINWRIGHT_API UWorld* ResolveTargetWorldForMethod(const FString& Method);

    // Registration metadata is authoritative. This compatibility helper remains
    // for callers that need the old public symbol, but does not use a leaf-name
    // heuristic.
    PINWRIGHT_API bool IsMutatingMethod(const FString& Method);

    // Target-world identity for the default editor-world handler family. The
    // returned value is the full UWorld object path already exposed as worldPath
    // by editor/actor responses. Use GetWorldIdForMethod for a method-specific
    // response decorator.
    PINWRIGHT_API FString GetActiveWorldId();
    PINWRIGHT_API FString GetWorldIdForMethod(const FString& Method);

    // Enforces expectWorld immediately before a mutating handler body runs.
    // The caller may use either the full world object path or package path.
    PINWRIGHT_API bool Validate(FHandlerContext& Ctx,
                                const TSharedPtr<FJsonObject>& Params);
    PINWRIGHT_API bool Validate(FAsyncResponseToken& Token,
                                const TSharedPtr<FJsonObject>& Params);

    // Adds the current world identity to a mutating handler response.
    PINWRIGHT_API void AddWorldField(const TSharedPtr<FJsonObject>& Result);
    PINWRIGHT_API void AddWorldField(const TSharedPtr<FJsonObject>& Result,
                                     const FString& Method);
}
