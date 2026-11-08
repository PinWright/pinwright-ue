// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UWorld;
class UGameViewportClient;

// World and viewport resolution shared by editor and game processes (docs/runtime-module.md).
namespace PinWright
{
    // Editor: the PIE world while PIE runs (GEditor->PlayWorld), else the editor world. Game: the game viewport's world.
    PINWRIGHTRUNTIME_API UWorld* GetTargetWorld();
    // Editor: the editor world (never PIE). Game: nullptr (callers return EDITOR_ONLY / NO_EDITOR_WORLD).
    PINWRIGHTRUNTIME_API UWorld* GetEditorWorldOrNull();
    // Game viewport client of the target world (PIE or game), nullptr in editor-only state.
    PINWRIGHTRUNTIME_API UGameViewportClient* GetTargetGameViewport();
    // True in a -game / packaged process.
    PINWRIGHTRUNTIME_API bool IsGameProcess();
}
