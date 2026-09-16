// Copyright (c) 2026 Alexander Penkin. MIT License.

// AssetSaveHandler.cpp - asset.save
//
// Generic single-asset save RPC. Mutators (blueprint.graph.set_pin_default_values,
// blueprint.set_default, material parameter edits, etc.) leave an asset dirty in
// memory with no per-asset flush; the only save surfaces are blunt (editor.save_all
// — every dirty package) or type-specific (level.save). asset.save is
// the universal single-asset persistence verb those mutation sources flow through.
//
// Persistence honesty: this routes through SaveAssetToDiskReportingPresence (NOT the
// mark-dirty McpSafeAssetSave, which writes nothing), so saved:true is
// gated on the .uasset actually landing on disk via ShouldTreatAssetSaveAsSuccess —
// mirroring niagara.save (B-niagara-save-no-disk-write). A throttled-skip / deferred
// no-write reports saved:false + pendingFlush instead of a false saved:true,sizeBytes:0.
// Note the gate is now freshness, not bare existence: a throttle-skipped save of a dirty
// package leaves the PREVIOUS revision on disk, which an existence probe alone accepts.
// Pass force:true to bypass the throttle when a same-asset save was issued <0.5s ago.
//
// Which of those a saved:false is, is `saveState` (B-asset-save-pie-failure-reports-pendingflush,
// B-asset-save-omits-savestate-pie-block). saved:false + pendingFlush:true alone cannot separate
// "the throttle skipped it, retry with force" from "the write is impossible right now" — and
// under PIE it was always the second, because the editor refuses every single-asset save while a
// play session is up. This handler measured the difference on every call and published only the
// bool, so the documented decision procedure ("read saveState before retrying") had no field to
// read and the natural reading of the payload was an unbounded retry loop. A PIE block now
// returns PIE_ACTIVE with pendingFlush:false plus the state, remedy, and pieActive/pieWorlds
// naming the session, which in a shared editor usually belongs to a different agent entirely.
//
// Integrity gate: SaveAssetToDiskReportingPresence inherits the Blueprint integrity
// gate inside SaveLoadedAssetThrottled (refuse-to-write on ValidateBlueprintGraphIntegrity
// failure). That helper now distinguishes the outcomes internally
// (ESaveLoadedAssetOutcome::Failed vs SkippedThrottledDirty vs NotPersistable), but
// SaveAssetToDiskReportingPresence collapses them back to a bool and exposes no failure
// list, so — like blueprint.compile — this handler
// calls ValidateBlueprintGraphIntegrity itself for UBlueprint assets to surface the
// verdict inline (integrityGate:"blocked", integrityFailures[]) that otherwise lives
// only in the compile handler and is lost on every other save path.
//
// Out-of-band gate (B-asset-save-clobbers-out-of-band-change): LoadObject above returns the
// RESIDENT object for an already-loaded package and never consults the file, so before the gate
// existed a git checkout / git reset --hard / external revert / second editor's write under a
// live editor was silently overwritten by the next save, with saved:true on the wire. The guard
// lives at the shared chokepoint (SaveLoadedAssetThrottled, via Utils/PackageDiskStateGuard.h)
// so every single-asset write site gets it once; this handler adds the caller-facing half —
// the SAVE_DISK_STATE_DIVERGED refusal, the measured diskState payload, and the explicit
// overwriteDiskChanges override. REFUSE rather than warn, because a warning attached to a
// response that already says saved:true cannot bring the discarded revision back, and because
// the ledger (UPackage::GetSavedHash vs the file's own summary) is maintained by the engine on
// every load and every save, so it does not trip on the editor's own writes.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Utils/AssetUtils.h"
#include "Utils/PackageDiskStateGuard.h"

#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// ---- asset.save ----
REGISTER_RPC_HANDLER("asset.save", "asset",
    "Persist ONE loaded asset to disk. The targeted single-asset save that graph/property "
    "mutators (blueprint.graph.set_pin_default_values, blueprint.set_default, material edits) "
    "do not perform — without it you must use the blunt editor.save_all (every dirty package) "
    "or a type-specific save. saved:true is gated on the .uasset actually reaching disk; a "
    "throttled/deferred no-write reports saved:false + pendingFlush. ALWAYS read saveState, not "
    "pendingFlush, before retrying: 'deferred' is fixed by a flush, 'failed' and 'notPersistable' "
    "never are. A PIE block is a PIE_ACTIVE error with saveState:'blockedByPie' and "
    "pendingFlush:false because force:true and editor.save_all hit the same refusal; wait for "
    "PIE to end (the response's "
    "pieWorlds names the session; editor.pie_status polls it). sizeBytes is the file's size after "
    "the call, so on a saved:false it is the PREVIOUS revision and carries sizeBytesIsStale:true. "
    "For UBlueprint assets the "
    "Blueprint integrity gate fires and a block is surfaced inline (integrityGate:\"blocked\", "
    "integrityFailures). REFUSES with SAVE_DISK_STATE_DIVERGED when the .uasset on disk changed "
    "since the package was loaded or last saved (a git checkout, an external revert, another "
    "tool or a second editor) — writing would discard it. The refusal carries a diskState block "
    "with the measured on-disk hash and the resident package's own; re-read with asset.reload, "
    "or pass overwriteDiskChanges:true to discard the on-disk revision deliberately.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the loaded asset to save."),
        RPC_PARAM_DEF("force",     "boolean", "Bypass the save throttle (mark dirty + force write). Does NOT bypass the out-of-band disk-state gate — use overwriteDiskChanges for that.", "false"),
        RPC_PARAM_DEF("overwriteDiskChanges", "boolean", "Save even when the .uasset on disk changed under the loaded package, discarding the on-disk revision. Only for a caller that has read the refusal's diskState block and decided the in-memory state wins.", "false")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath)) return true;

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("Could not load asset '%s'."), *AssetPath));
        return true;
    }

    const bool bForce = Ctx.GetBool(TEXT("force"), false);
    if (bForce)
    {
        Asset->MarkPackageDirty();
    }

    // Surface the Blueprint integrity verdict inline. SaveAssetToDiskReportingPresence
    // (via SaveLoadedAssetThrottled) already refuses to write a corrupt Blueprint, but
    // returns a bare false with no failure list — so for UBlueprint assets we run the
    // same check the compile handler does and report integrityGate/integrityFailures
    // directly, short-circuiting the save when the gate blocks it.
    TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure> IntegrityFailures;
    if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
    {
        // ValidateBlueprintGraphIntegrity only appends to the (empty) array, so a
        // non-empty IntegrityFailures is exactly the gate-blocked condition.
        BlueprintHandlerUtils::ValidateBlueprintGraphIntegrity(Blueprint, IntegrityFailures);
    }
    const bool bIntegrityGateBlockedSave = IntegrityFailures.Num() > 0;

    // Default the package name from the asset; the real-save path overwrites it
    // with the same value (SaveAssetToDiskReportingPresence sets it identically).
    FString PackageName = Asset->GetOutermost()->GetName();
    int64 SizeBytes = 0;
    bool bSaved = false;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    // A separate flag from force on purpose: force means "bypass the 0.5s throttle" and must not
    // silently acquire "and discard whatever another writer put on disk"
    // (B-asset-save-clobbers-out-of-band-change).
    const bool bOverwriteDiskChanges = Ctx.GetBool(TEXT("overwriteDiskChanges"), false);
    if (!bIntegrityGateBlockedSave)
    {
        // Real-save helper that gates saved on the .uasset reaching disk and reports
        // package/sizeBytes (the disk-presence contract from B-niagara-save-no-disk-write).
        // bForce bypasses the throttle.
        bSaved = SaveAssetToDiskReportingPresence(Asset, bForce, &PackageName, &SizeBytes,
            &SaveState, bOverwriteDiskChanges);
    }

    if (SaveState == EAssetSaveState::DiskStateDiverged)
    {
        // The shared guard refused before writing a byte. Re-probe to publish what it measured:
        // this is the only response that needs the numbers, so the happy path pays nothing for
        // them, and nothing has written the file between the refusal and here.
        PinWrightPackageDiskState::FPackageDiskDivergence DiskState;
        PinWrightPackageDiskState::ProbePackageDiskDivergence(Asset->GetOutermost(), DiskState);

        TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
        ErrorData->SetStringField(TEXT("assetPath"), AssetPath);
        ErrorData->SetStringField(TEXT("package"),   PackageName);
        ErrorData->SetBoolField(TEXT("saved"),       false);
        ErrorData->SetStringField(TEXT("saveState"), AssetSaveStateToWire(SaveState));
        ErrorData->SetStringField(TEXT("saveDetail"), AssetSaveStateDetail(SaveState));
        PinWrightPackageDiskState::AddPackageDiskStateJson(ErrorData, DiskState);

        // The re-probe names the file and both hashes. If it somehow could not be repeated the
        // state's own detail sentence still says what happened and what to do — never an empty
        // refusal message.
        FString Message = PinWrightPackageDiskState::DescribePackageDiskDivergence(DiskState);
        if (Message.IsEmpty())
        {
            Message = FString::Printf(TEXT("Refused to save '%s': %s"),
                *PackageName, AssetSaveStateDetail(SaveState));
        }

        Ctx.SendError(ErrorCodes::ERR_SAVE_DISK_STATE_DIVERGED, Message, ErrorData);
        return true;
    }

    if (bIntegrityGateBlockedSave)
    {
        // The gate refused the write, so a save WAS requested and produced no durable revision.
        // EAssetSaveState::Failed names exactly that case (its own definition lists the
        // Blueprint graph-integrity refusal); leaving NotRequested here would claim no save was
        // asked for.
        SaveState = EAssetSaveState::Failed;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("package"),   PackageName);

    // The shared save-report emitter, NOT a hand-rolled {saved, pendingFlush} pair
    // (B-asset-save-omits-savestate-pie-block). This verb computed a full EAssetSaveState on
    // every path and then published only the bool, so a PIE-blocked no-op was wire-identical to
    // a throttled write whose documented remedy is the retry that cannot work. Routing through
    // AddAssetSaveReport gives asset.save the same {saveRequested, saved, pendingFlush,
    // saveState, saveDetail} shape as every verb that saves on the caller's behalf, and the same
    // pieActive/pieWorlds block naming the session that is holding the editor.
    AddAssetSaveReport(Result, /*bSaveRequested=*/true, bSaved, SaveState);
    // sizeBytes is a measurement of the FILE, not of this call's write, so a non-durable save
    // over an existing asset reports the previous revision's size. Emitted through the shared
    // helper, which labels that case rather than letting a plausible byte count read as proof.
    AddAssetSaveSizeReport(Result, SizeBytes, bSaved);

    if (SaveState == EAssetSaveState::BlockedByPie)
    {
        Ctx.SendError(ErrorCodes::ERR_PIE_ACTIVE,
            FString::Printf(TEXT("Refused to save '%s': %s"),
                *PackageName, AssetSaveStateDetail(SaveState)),
            Result);
        return true;
    }

    if (IntegrityFailures.Num() > 0)
    {
        Result->SetArrayField(TEXT("integrityFailures"),
            BlueprintHandlerUtils::BuildBlueprintIntegrityFailuresJson(IntegrityFailures));
    }

    if (bIntegrityGateBlockedSave)
    {
        Result->SetStringField(TEXT("integrityGate"), TEXT("blocked"));
        Result->SetStringField(TEXT("integrityMessage"),
            FString::Printf(TEXT("Blueprint integrity check failed: %d integrity failure(s) detected — refused to save. See 'integrityFailures'."),
                IntegrityFailures.Num()));
    }

    Ctx.SendSuccess(Result);
    return true;
}
