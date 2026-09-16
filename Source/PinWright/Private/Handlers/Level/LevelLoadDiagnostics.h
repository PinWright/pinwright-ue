// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared no-disk-file diagnostics for level loading.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/HandlerContext.h"
#include "PinWrightHelpers.h"

// When a level has no .umap on disk, split the verdict for level.load and its
// alias editor.open_level identically: a world that is live in memory or listed
// in the asset registry but never persisted gets LEVEL_NOT_PERSISTED (save it or
// discard the orphan, do not retry path forms), while a genuinely-absent path
// keeps FILE_NOT_FOUND. Single-sourced here so the user-facing message and the
// in-memory/registry probe live in one place rather than being copy-pasted into
// both handlers. FileNotFoundDisplayPath is the path shown in the FILE_NOT_FOUND
// message (the resolved on-disk filename for open_level, the package path for
// level.load). Always sends an error and returns true (handler-done).
inline bool SendLevelNotLoadableError(
    const FHandlerContext& Ctx,
    const FString& LevelPath,
    const FString& FileNotFoundDisplayPath)
{
    if (IsLevelPackagePresentInMemoryOrRegistry(LevelPath))
    {
        Ctx.SendError(TEXT("LEVEL_NOT_PERSISTED"),
            FString::Printf(TEXT("Level '%s' is registered/loaded in memory but was never saved to disk, "
                "so it cannot be loaded from disk. Save it (level.save / level.save_as) or discard the "
                "orphaned world; do not retry path forms."), *LevelPath));
        return true;
    }
    Ctx.SendError(TEXT("FILE_NOT_FOUND"),
        FString::Printf(TEXT("Level file not found: %s"), *FileNotFoundDisplayPath));
    return true;
}

// Companion classifier for the read-only inspection getters (level.get_info /
// get_actors / get_bounds), kept here next to SendLevelNotLoadableError so both
// "level path with no usable level -> pick the right verdict + actionable message"
// classifiers live in one place. Those getters resolve levelPath through
// FindLevelByPathLevel, which walks only the active world's loaded levels, so a
// map that genuinely exists on disk but is not loaded used to surface a misleading
// LEVEL_NOT_FOUND ("Level not found") — contradicting level.list (which lists it)
// and level.load (which opens it). Distinguish the two conditions via the shared
// DoesLevelMapExistOnDisk probe: a path with a real .umap on disk is
// LEVEL_NOT_LOADED with an actionable message; a path with no map on disk stays
// LEVEL_NOT_FOUND. Always sends an error and returns true (handler-done).
inline bool SendLevelInspectMissError(
    const FHandlerContext& Ctx,
    const FString& LevelPath)
{
    if (DoesLevelMapExistOnDisk(LevelPath))
    {
        Ctx.SendError(TEXT("LEVEL_NOT_LOADED"),
            FString::Printf(TEXT("Level '%s' exists on disk but is not loaded into the active world; "
                "these getters read only loaded levels. level.load it first, or pass no levelPath "
                "to read the active level."), *LevelPath));
        return true;
    }
    Ctx.SendError(TEXT("LEVEL_NOT_FOUND"),
        FString::Printf(TEXT("Level not found: %s"), *LevelPath));
    return true;
}
