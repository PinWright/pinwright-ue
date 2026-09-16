// Copyright (c) 2026 Alexander Penkin. MIT License.

// AssetSaveState.h - the one vocabulary for "did this edit reach the disk, and what do I do
// about it".
//
// Split out of AssetUtils.h so a plain data struct on the way to a response
// (FStaticMeshCreateResult, FPwModelCompileResult) can carry the verdict without pulling in
// AssetUtils.h, which drags Editor.h and a spawn template through every translation unit that
// includes it - including UHT-parsed headers.
#pragma once

#include "CoreMinimal.h"

// What a save request actually did. Deliberately NOT a bool, and deliberately not the same
// enum as ESaveLoadedAssetOutcome (AssetUtils.h): that one names what the ENGINE call did, this
// one names what the CALLER should believe and do. The two differ on every branch where the
// engine reports success and the disk disagrees.
//
// Collapsing this to a bool is what produced the defect this type exists to close:
// model.compile answered saved:false with no way to tell a throttle-deferred edit (flush it)
// from a failed write (do not flush, investigate) from a transient package (never flushable),
// and SaveAssetToDiskReportingPresence had measured all three before throwing the distinction
// away.
enum class EAssetSaveState : uint8
{
    // No save was requested. The edit, if any, is in memory only; nothing failed.
    NotRequested,

    // THIS call wrote the .uasset. Durable now.
    Written,

    // Nothing needed writing: the on-disk revision already equals memory. Durable now.
    // Distinct from Written because no bytes moved - a caller comparing file timestamps to
    // detect work must not read this as a write.
    AlreadyCurrent,

    // A save was requested, the edit is still only in memory, and a later flush WILL make it
    // durable. The throttle skip over a dirty package is the canonical producer.
    Deferred,

    // The save was attempted and did not produce a durable revision - it failed outright, was
    // refused (the Blueprint graph-integrity gate), or reported success while the file on disk
    // did not move. A plain flush is not the fix; the cause has to be cleared first.
    Failed,

    // REFUSED before any byte was written: a Play-In-Editor session is running, and the editor's
    // asset-save API refuses EVERY single-asset write while it is
    // (EditorScriptingHelpers::CheckIfInEditorAndPIE). Distinct from Failed because the cause is
    // environmental, external to the asset and to the caller - in a shared editor the session
    // usually belongs to somebody else - and because it clears by itself when PIE ends, so the
    // remedy is "wait, then re-issue" rather than "investigate a fault". Distinct from Deferred
    // because force:true and a flush BOTH hit the same refusal: retrying now cannot work.
    // See Utils/PieSaveBlockGuard.h and board
    // B-asset-save-pie-failure-reports-pendingflush.
    BlockedByPie,

    // Transient or unmounted package. No flush will ever persist it.
    NotPersistable,

    // REFUSED before any byte was written: the .uasset on disk changed since this package was
    // loaded or last saved, so writing the in-memory state would discard an out-of-band change
    // (a git checkout, an external revert, another tool's or another editor's write). Distinct
    // from Failed because nothing was attempted and nothing is wrong with the package - the
    // remedy is to re-read the file (asset.reload) or to overwrite it deliberately, not to
    // clear a fault. See Utils/PackageDiskStateGuard.h and board
    // B-asset-save-clobbers-out-of-band-change.
    DiskStateDiverged
};

// Stable lowerCamel wire spelling of a state, emitted as the `saveState` response field.
// Documented in docs/wiki-src/safe-mutation-save.md; treat these strings as contract.
PINWRIGHT_API const TCHAR* AssetSaveStateToWire(EAssetSaveState State);

// One short sentence telling the caller what to DO. For Deferred it names the flushing verb;
// for Failed and NotPersistable it says a flush is not the answer, and why.
PINWRIGHT_API const TCHAR* AssetSaveStateDetail(EAssetSaveState State);

// True only for the states where the asset's current content is on disk right now.
// The single definition of "durable" - `saved` in the wire report is this predicate.
PINWRIGHT_API bool IsAssetSaveStateDurable(EAssetSaveState State);
