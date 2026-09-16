// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared pre-swap refusal for every verb that hands the engine a world swap.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Utils/MapSwapDirtyWorldGuard.h"

// Runs the resident-world survivor probe and answers the caller when the swap must not
// proceed. Returns true when it responded and the handler must stop.
//
// Single-sourced because the fatal it prevents lives in EditorDestroyWorld, which every
// swap entry point reaches — Map_Load (level.load and its aliases, and a `Open <path>`
// console command) and NewMap (level.create, lighting.create_lighting_enabled_level).
// Copying the block into one of them is how the next one gets missed.
//
// bTransactionBufferWillBeCleared must match the entry point the caller is about to reach:
// true for Map_Load, which calls ResetTransaction BEFORE destroying the world, so a world
// held only by the undo buffer is released in time; false for NewMap, which resets it only
// after CheckForWorldGCLeaks has already fired, making that same world a real killer.
//
// LevelPath is reported back as the requested path; the probe itself takes no incoming
// package, because on these paths either there is none or the target is Map_Load's own
// business (see Utils/MapSwapDirtyWorldGuard.h).
inline bool RefuseIfWorldsSurviveMapSwap(
    const FHandlerContext& Ctx, const FString& LevelPath, bool bTransactionBufferWillBeCleared)
{
    const PinWrightMapSwapGuard::FWorldSurvivorProbeResult Probe =
        PinWrightMapSwapGuard::ProbeResidentWorldSurvivors(
            FString(), bTransactionBufferWillBeCleared);

    if (Probe.bProbeUnavailable)
    {
        // Not "clear to swap": the probe measured nothing. Reporting success here would
        // hand the caller exactly the crash it exists to prevent, so this refuses on a
        // retryable code instead.
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_READY,
            FString::Printf(
                TEXT("Cannot verify the map swap is survivable right now: %s. Nothing was "
                     "changed; retry when the editor is idle."),
                *Probe.UnavailableReason),
            PinWrightMapSwapGuard::BuildSurvivorErrorData(LevelPath, Probe));
        return true;
    }
    if (Probe.IsBlocked())
    {
        Ctx.SendError(ErrorCodes::ERR_DIRTY_WORLD_BLOCKS_MAP_SWAP,
            PinWrightMapSwapGuard::DescribeSurvivorRefusal(Probe),
            PinWrightMapSwapGuard::BuildSurvivorErrorData(LevelPath, Probe));
        return true;
    }
    return false;
}
