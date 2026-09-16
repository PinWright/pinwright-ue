// Copyright (c) 2026 Alexander Penkin. MIT License.

// Measures whether a Play-In-Editor session is refusing every single-asset write in this
// process, so a save report can NAME the blocker instead of answering with an unattributed
// "not durable".
//
// Why this exists (board B-asset-save-pie-failure-reports-pendingflush and its duplicate
// B-asset-save-omits-savestate-pie-block): the plugin reaches disk for a single asset through
// exactly one engine call - UEditorAssetLibrary::SaveLoadedAsset, from SaveLoadedAssetThrottled
// - and that API opens with EditorScriptingHelpers::CheckIfInEditorAndPIE()
// (UnrealEd/Private/EditorScriptingHelpers.cpp:143), which returns false, logging
// "LogUtils: Error: The Editor is currently in a play mode.", whenever
// `GEditor->PlayWorld || GIsPlayInEditorWorld`.
//
// THE REFUSAL IS TOTAL AND IS NOT A PROPERTY OF THE ASSET. It does not consult the package,
// the dirty flag, the throttle, force:true, or which caller asked. So while any PIE session is
// up, every single-asset save in the editor is a silent no-op - including saves issued by an
// agent that neither started PIE nor can see that anyone did, which is the whole reason the
// symptom was undiagnosable from the response.
//
// PRE-CHECK, NOT POST-CLASSIFY. editor.save_all's diagnostic
// (Handlers/Editor/EditorSaveAllDiagnostic.h) attempts the save and then asks "was PIE up?" to
// label the failure. That is the right shape there, because save_all's failures have several
// causes and PIE is only one guess among them. Here it would be the wrong shape: the engine's
// gate is a documented, unconditional precondition, so measuring it BEFORE the call turns a
// guess into a fact, keeps a refusal out of the throttle-named failure channel the board
// complains about ("SaveLoadedAssetThrottled: failed to save" is emitted for a cause the
// throttle had nothing to do with), and skips the wasted disk-state header read and engine call
// that were never going to write anything.
//
// WHAT IS DELIBERATELY NOT BLOCKED. A clean package with nothing to write is not blocked by
// PIE - there is no write for PIE to refuse, and the on-disk revision already matches memory.
// The chokepoint therefore consults this guard only when a write would actually be attempted
// (the package is dirty, or the save is forced); a clean unforced save keeps its existing
// already-clean verdict rather than being turned into a false failure.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace PinWrightPieSaveBlock
{
    // One live PIE world, reduced to what a caller needs to identify whose session is holding
    // the editor. Mirrors the fields editor.pie_status publishes, so an agent that reads the
    // save response and an agent that probes PIE directly see the same names for the same world.
    struct FPieWorldIdentity
    {
        int32 PieInstance = INDEX_NONE;
        // The map with the UEDPIE_N_ prefix stripped, e.g. "T_UI".
        FString MapName;
        // The full PIE world object path, e.g. "/Game/FPS/Test/UEDPIE_0_T_UI.T_UI".
        FString WorldPath;
    };

    // One measurement of "is the editor's asset-save API refusing writes right now".
    struct FPieSaveBlock
    {
        // The engine's own precondition, mirrored exactly:
        // GEditor->PlayWorld || GIsPlayInEditorWorld.
        bool bActive = false;

        // Every live PIE world context, in engine order. Can legitimately be EMPTY while
        // bActive is true (GIsPlayInEditorWorld set with no resolvable world context), which is
        // why the verdict is bActive and never Worlds.Num().
        TArray<FPieWorldIdentity> Worlds;
    };

    // Measures the block and returns OutBlock.bActive, so a bare `if (ProbePieSaveBlock(B))`
    // reads as "if PIE is blocking saves". Game thread.
    //
    // Cheap on the common path: when the two globals say no PIE it returns before touching the
    // engine's world-context array, so the chokepoint pays two pointer reads per save.
    bool ProbePieSaveBlock(FPieSaveBlock& OutBlock);

    // Publishes the measurement on a response. Writes NOTHING when the block is inactive, so a
    // payload from an editor that is not in play mode is byte-identical to what it always was.
    //
    // The contract that absence encodes, and which safe-mutation-save.md states: on a save
    // report that already says a save was requested and is not durable, the ABSENCE of
    // `pieActive` means PIE was not running. It is a positive marker of a measured condition,
    // not an omitted measurement.
    //
    // Fields, present only when blocked:
    //   pieActive  true
    //   editorMode "PIE"  - the same spelling editor.save_all uses, so the two save verbs
    //                       answer the environmental question in one vocabulary
    //   pieWorlds  [{pieInstance, mapName, worldPath}] - which session is holding the editor
    void AddPieSaveBlockJson(const TSharedPtr<FJsonObject>& Result, const FPieSaveBlock& Block);

    // Convenience for the report emitters: probe and publish in one call. No-op when not
    // blocked.
    void AddPieSaveBlockJsonIfBlocked(const TSharedPtr<FJsonObject>& Result);

    // One line naming the blocking worlds, for a log line and for a refusal message. Empty
    // when the block is inactive.
    FString DescribePieSaveBlock(const FPieSaveBlock& Block);
}
