// Copyright (c) 2026 Alexander Penkin. MIT License.

// Measures whether the .uasset under a RESIDENT UPackage changed on disk since that package
// last read or wrote it, so a save can refuse instead of silently overwriting the change.
//
// Why this exists (board B-asset-save-clobbers-out-of-band-change): asset.save — and every
// other single-asset write in the plugin, all of which funnel through
// SaveLoadedAssetThrottled — writes the in-memory package unconditionally. LoadObject returns
// the resident object for an already-loaded package and never consults the file, and the only
// disk probe on that path (SaveAssetToDiskReportingPresence's FileSize/GetTimeStamp baseline)
// exists to judge whether THIS save moved the file, not whether somebody else moved it first.
// So a `git checkout`, a `git reset --hard`, an external revert, a teammate's sync or a second
// editor's write is discarded by the next save of that package, and the caller is told
// saved:true. The in-editor revert path was closed by
// B-source-control-revert-no-package-reload (Handlers/SourceControl/SourceControlPackageResync.h,
// which resynchronizes and reports a measured staleCount); this closes the out-of-editor one.
//
// THE LEDGER IS UPackage::GetSavedHash(), NOT AN mtime+size RECORD WE KEEP.
//
// The ticket named both candidates. The deciding question is not cost, it is whether the ledger
// trips on the editor's own writes — one that does is useless, because every ordinary save
// becomes a false positive.
//
//   * mtime+size kept by us. Cost per save is two stat calls, and
//     SaveAssetToDiskReportingPresence already makes both, so it looks free. It is not: WE would
//     have to own the record, seeded at load and updated after every write. Packages are written
//     from paths this plugin does not go through — Ctrl+S in the editor UI, editor.save_all's
//     UEditorAssetLibrary::SaveAsset, editor.quit's FEditorFileUtils::SaveDirtyPackages,
//     FEditorFileUtils::SaveLevel, the redirector fixup's PromptForCheckoutAndSave, the engine's
//     own asset-rename and source-control resave flows. Every write we fail to observe leaves our
//     record stale and makes the NEXT save a false refusal. Closing that means instrumenting a
//     set of save entry points we do not control and cannot enumerate, plus a per-package map to
//     keep alive across GC.
//
//   * UPackage::GetSavedHash() vs the file. Cost per save is one buffered read of the file's
//     FPackageFileSummary header (~1 KB, sub-millisecond) against a multi-millisecond package
//     write. The decisive property is that BOTH SIDES ARE MAINTAINED BY THE ENGINE, not by us:
//     FLinkerLoad::Serialize sets the package's hash from the file's summary on every load
//     (LinkerLoad.cpp: LinkerRootPackage->SetSavedHash(Summary.GetSavedHash())), and
//     UPackage::Save sets it from the bytes it just wrote on every save to a mounted path
//     (SavePackage2.cpp: Package->SetSavedHash(SaveContext.PackageSavedHash) under
//     IsUpdatingLoadedPath), embedding the same value in the header it writes. There is no save
//     path in or out of this plugin that can move the file without moving the package's hash with
//     it, so the ledger cannot go stale and there is no state for us to own.
//
// Autosave is the one engine write that does NOT update the hash (SAVE_FromAutosave clears
// IsUpdatingLoadedPath) — and it writes to Saved/Autosaves/, a different file, so it cannot
// produce a false positive on the asset's own path. Cooks and PIE duplicates likewise never
// write the asset's own file.
//
// WHAT IS DELIBERATELY NOT GUARDED. A package with a zero saved hash has never been read from or
// written to a file by this editor session, so there is no ledger to compare and the probe
// reports "not probed" rather than guessing. That covers every CreatePackage + factory create
// flow, including one that lands on a path where a .uasset already exists: overwriting there is
// the create verb's own semantics (AssetCreatePolicy), not this guard's business, and refusing it
// would break every create-and-save path in the plugin. A file that is ABSENT is likewise not
// divergence — the package is the only surviving copy and writing it back loses nothing.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UPackage;

namespace PinWrightPackageDiskState
{
    // One measurement of "does the file still hold what this package last read or wrote".
    //
    // bProbed is the honesty flag and gates everything below it: an unmounted package name, a
    // package with no saved-hash ledger, an unreadable or non-binary header, and an engine too
    // old to carry the ledger all report bProbed=false with a reason, and callers must OMIT the
    // measured fields rather than publish a fabricated "not diverged".
    struct FPackageDiskDivergence
    {
        // False means NO COMPARISON WAS MADE. Not "they match".
        bool bProbed = false;

        // Why not, when bProbed is false. Empty otherwise.
        FString UnprobedReason;

        // The verdict. Meaningful only when bProbed.
        bool bDiverged = false;

        FString PackageName;

        // The file the probe read. Empty when the package name has no registered mount root.
        FString Filename;

        // Measured, not assumed. A package whose file is gone is NOT diverged (see the header
        // comment) but the caller is told the file is missing.
        bool bFileExists = false;

        // ---- MEASURED DISK STATE (read out of the file this call opened) ----
        FString DiskSavedHash;
        int64 DiskSizeBytes = 0;
        FDateTime DiskModifiedTime = FDateTime::MinValue();

        // ---- IN-MEMORY STATE (what the resident package believes it last read or wrote) ----
        // Named separately from the disk fields on purpose: conflating the two is the defect.
        FString LoadedSavedHash;
    };

    // Measures Package against its file. Returns OutState.bDiverged, so a bare `if (Probe(...))`
    // reads as "if it diverged"; a probe that could not be made returns false and sets
    // OutState.bProbed=false. Logs at Warning — and only at Warning, this is a refusal signal and
    // not an engine error — when the two disagree.
    //
    // Game thread. Cost is one buffered header read (~1 KB) and is paid only after the cheap
    // in-memory checks (mount, ledger presence) have already passed.
    bool ProbePackageDiskDivergence(UPackage* Package, FPackageDiskDivergence& OutState);

    // Publishes the measurement on a response under `diskState`. When the probe could not be
    // made the object carries `probed:false` and a `reason` and NO measured field at all, so a
    // caller can never read an omitted measurement as a clean bill of health.
    void AddPackageDiskStateJson(const TSharedPtr<FJsonObject>& Result,
                                 const FPackageDiskDivergence& State);

    // One line naming the file and what changed, for a refusal message and for the log.
    FString DescribePackageDiskDivergence(const FPackageDiskDivergence& State);
}
