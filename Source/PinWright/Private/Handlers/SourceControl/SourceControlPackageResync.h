// Copyright (c) 2026 Alexander Penkin. MIT License.

// Resynchronizes loaded UPackages with the bytes a source-control operation just wrote
// to the working tree, and MEASURES whether that actually happened.
//
// Why this exists (board B-source-control-revert-no-package-reload): running a raw
// provider FRevert against a .uasset that is currently resident changes the file and
// leaves the in-memory UObject holding the pre-revert values. Two consequences, both
// silent: every readback afterwards reports the un-reverted state while
// source_control.status reports the file clean, and the next save of that package
// writes the stale in-memory state back out, undoing the revert with a success receipt.
//
// The reload is NOT hand-rolled. USourceControlHelpers::ApplyOperationAndReloadPackages
// is the same entry point the Content Browser's revert flow uses
// (SSourceControlRevert.cpp -> SourceControlHelpers::RevertAndReloadPackages), and it
// carries edge cases a per-file UPackageTools::ReloadPackages call does not: it unlinks
// loaders before the operation runs, splits world from non-world packages, deletes and
// unloads assets whose file the operation removed (revert-of-add), and re-caches the
// source-control state afterwards.
//
// What is NOT reused is RevertAndReloadPackages itself: that wrapper leaves bInteractive
// at its default of true, which reaches UPackageTools::ReloadPackages in Interactive mode
// and opens a modal "these assets have been modified, reload anyway?" dialog for every
// dirty package — a hang on a headless RPC host. The lower-level entry point takes
// bInteractive explicitly, and false maps to EReloadPackagesInteractionMode::AssumePositive
// (SourceControlHelpers.cpp), i.e. the disk bytes win without a prompt — the same choice
// asset.reload already makes.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

namespace PinWrightSourceControlResync
{
    // Measured post-state for one targeted file.
    struct FPackageResyncEntry
    {
        FString Filename;
        FString PackageName;

        // Was a UPackage for this file resident BEFORE the operation ran? A file that
        // was not loaded has no in-memory state to go stale.
        bool bWasLoaded = false;

        // Measured, not assumed: the resident package was replaced by a fresh one read
        // from the post-operation disk bytes.
        bool bReloaded = false;

        // The operation removed the file and the package went with it (revert of an add).
        // In-memory and disk agree — there is nothing left of either.
        bool bRemoved = false;

        // The honesty flag. The package was resident, is still resident, and was NOT
        // replaced: the in-memory object still holds the pre-operation state and a
        // subsequent save of it would write that state back over the operation's result.
        bool bStale = false;
    };

    struct FPackageResyncReport
    {
        TArray<FPackageResyncEntry> Entries;

        // Result of the caller's operation itself (e.g. the provider FRevert).
        bool bOperationSucceeded = false;

        int32 LoadedCount = 0;
        int32 ReloadedCount = 0;
        int32 RemovedCount = 0;
        int32 StaleCount = 0;

        // Non-empty means NOTHING RAN. Loaded map packages, and loaded external packages
        // belonging to a loaded world, cannot be reloaded without tearing down the live
        // world; the engine helper refuses the whole batch for them, and so does the
        // pre-flight here — before the operation, so the working tree is left untouched
        // rather than changed under a world that keeps the old state.
        TArray<FString> BlockedPackages;
    };

    // Runs Operation over Filenames with every resident target unlinked first and
    // reloaded from the resulting disk bytes, then measures what actually happened.
    // Returns Operation's own result; the caller must still read OutReport.StaleCount
    // to learn whether the in-memory state is trustworthy. Returns false without running
    // Operation when OutReport.BlockedPackages is non-empty.
    // Game thread only (it reloads packages and touches the editor world).
    bool ApplyAndResyncPackages(const TArray<FString>& Filenames,
        const TFunctionRef<bool(const TArray<FString>&)>& Operation,
        FPackageResyncReport& OutReport);
}
