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
    PINWRIGHTRUNTIME_API UWorld* ResolveTargetWorldForMethod(const FString& Method);

    // editor.screenshot captures the game/PIE viewport whenever one exists and the level-editor
    // viewport otherwise, so the world it reports follows the same branch.
    PINWRIGHTRUNTIME_API UWorld* SelectScreenshotWorld(
        bool bHasGameViewport, UWorld* GameViewportWorld, UWorld* EditorWorld);

    // Registration metadata is authoritative. This compatibility helper remains
    // for callers that need the old public symbol, but does not use a leaf-name
    // heuristic.
    PINWRIGHTRUNTIME_API bool IsMutatingMethod(const FString& Method);

    // Target-world identity for the default editor-world handler family. The
    // returned value is the full UWorld object path already exposed as worldPath
    // by editor/actor responses. Use GetWorldIdForMethod for a method-specific
    // response decorator.
    PINWRIGHTRUNTIME_API FString GetActiveWorldId();
    PINWRIGHTRUNTIME_API FString GetWorldIdForMethod(const FString& Method);

    // Enforces expectWorld immediately before a mutating handler body runs.
    // The caller may use either the full world object path or package path.
    PINWRIGHTRUNTIME_API bool Validate(FHandlerContext& Ctx,
                                const TSharedPtr<FJsonObject>& Params);
    PINWRIGHTRUNTIME_API bool Validate(FAsyncResponseToken& Token,
                                const TSharedPtr<FJsonObject>& Params);

    // Adds the current world identity to a mutating handler response.
    PINWRIGHTRUNTIME_API void AddWorldField(const TSharedPtr<FJsonObject>& Result);
    PINWRIGHTRUNTIME_API void AddWorldField(const TSharedPtr<FJsonObject>& Result,
                                     const FString& Method);
}
