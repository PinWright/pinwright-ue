// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Open-asset-editor guard for structural niagara.* mutations.
//
// A UNiagaraSystem reached through LoadObject and reshaped while FNiagaraSystemToolkit has it
// open is not merely clobbered by the toolkit's working copy: the toolkit's live widgets hold
// the emitter handles the mutation frees, and the next ordinary Slate redraw dereferences them
// (SNiagaraOverviewGraphTitleBar::IsUsingDeprecatedEmitter -> UNiagaraEmitter::GetEmitterData,
// EXCEPTION_ACCESS_VIOLATION, whole process). The fault lands in a redraw, not in the mutation,
// so the mutating call still returns success and nothing in its response can ever report it.
// Refusing up front is the only outcome the caller can observe.
//
// The check keys off the ASSET, never off "did this caller open it". Several agents share one
// editor process, so the toolkit that kills a write is routinely one somebody else left open --
// including the one render.capture_asset_preview leaves behind on closeAfterCapture: false.
//
// Material has the same guard shape in Handlers/Material/MaterialFinders.h (IsMaterialEditorOpen);
// this is the Niagara counterpart and is deliberately asset-class-agnostic so the remaining
// niagara.* mutators can adopt it without a second copy.
//
// ADOPTION IS PER VERB, NOT BLANKET. Most niagara.* mutators must NOT take this guard: the engine
// mutators they call announce the change on a delegate the toolkit already listens to
// (UNiagaraEmitter::OnRenderersChanged / OnSimStagesChanged / OnEventHandlersChanged,
// FNiagaraParameterStore::OnLayoutChange, UEdGraph::RemoveNode's GRAPHACTION_RemoveNode,
// UNiagaraSystem::PostEditChangeProperty -> FNiagaraSystemViewModel::RefreshAll), and the stack
// entries that survive hold TWeakObjectPtr. Refusing those costs the caller an edit and buys
// nothing, and in a shared editor process the open toolkit is routinely somebody else's. Adopt
// this only where the mutation invalidates something a live widget caches by bare pointer or by
// value snapshot AND no notification reaches it. Each call site states which.

#include "CoreMinimal.h"
#include "Editor.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace PinWrightNiagara
{
    // The asset editor currently holding Asset open, or nullptr when none is. Also nullptr when
    // Asset is null or GEditor / UAssetEditorSubsystem are unavailable, so a host with no asset
    // editors at all (a commandlet) never refuses a mutation it has no reason to refuse.
    inline IAssetEditorInstance* FindOpenAssetEditorForNiagaraAsset(UObject* Asset)
    {
        if (!Asset || !GEditor)
        {
            return nullptr;
        }

        UAssetEditorSubsystem* AssetEditorSS = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (!AssetEditorSS)
        {
            return nullptr;
        }

        return AssetEditorSS->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false);
    }

    // True when any asset editor holds Asset open.
    inline bool IsNiagaraAssetEditorOpen(UObject* Asset)
    {
        return FindOpenAssetEditorForNiagaraAsset(Asset) != nullptr;
    }

    // Why a particular verb is refused. The message has to name what would actually go wrong, or
    // the caller cannot tell a genuine hazard from a blanket refusal; a verb that reshapes the
    // emitter-handle array and a verb the stack silently reverts fail for different reasons.
    namespace HazardClause
    {
        inline constexpr const TCHAR* EmitterHandleReshape =
            TEXT("Reshaping the emitter handles underneath a live Niagara toolkit frees handles its ")
            TEXT("widgets still reference (FNiagaraEmitterHandleViewModel::EmitterHandle is a raw ")
            TEXT("pointer into that array) and takes the editor down on the next redraw, after this ")
            TEXT("call has already reported success.");

        inline constexpr const TCHAR* EventHandlerStackSnapshot =
            TEXT("The open stack's UNiagaraStackEventWrapper holds a copy of EventHandlerScriptProps ")
            TEXT("taken when the Event Handler row was first built, and writes that whole copy back ")
            TEXT("over the emitter on the next event-handler property edit. Nothing re-takes the ")
            TEXT("snapshot, so this write would be reported as succeeding and then silently reverted.");
    }

    // Refuse a structural edit while an asset editor holds Asset open, naming the open editor, what
    // the edit would break (Hazard, one of the HazardClause constants) and the close route. Returns
    // true when the handler must return immediately (EDITOR_OPEN has already been sent); false when
    // nothing has the asset open and the mutation may proceed.
    inline bool RejectStructuralEditWhileAssetEditorOpen(
        FHandlerContext& Ctx,
        UObject* Asset,
        const FString& AssetPath,
        const TCHAR* Hazard = HazardClause::EmitterHandleReshape)
    {
        IAssetEditorInstance* OpenEditor = FindOpenAssetEditorForNiagaraAsset(Asset);
        if (!OpenEditor)
        {
            return false;
        }

        Ctx.SendError(ErrorCodes::ERR_EDITOR_OPEN, FString::Printf(
            TEXT("'%s' is open in the '%s' asset editor. %s Close the asset first ")
            TEXT("(editor.close_asset), then retry. The editor may have been opened by another ")
            TEXT("caller sharing this editor process."),
            *AssetPath, *OpenEditor->GetEditorName().ToString(), Hazard));
        return true;
    }

    // ------------------------------------------------------------------------------------------
    // ASSET-KIND GUARD: any write to an EMITTER ASSET whose toolkit is open.
    //
    // An emitter toolkit does not edit the asset it was opened on.
    // FNiagaraSystemToolkit::InitializeWithEmitter builds a TRANSIENT UNiagaraSystem and hands the
    // emitter to FNiagaraSystemViewModel::AddEmitter, which copies it -- the engine says so in
    // place: "Adding the emitter to the system has made a copy of it". Every edit the user makes
    // lands on that copy. FNiagaraSystemToolkit::UpdateOriginalEmitter (Apply, and the save /
    // close paths that route through it) then does
    //     Source = StaticDuplicateObject(EditableEmitter.Emitter, Source->GetOuter(), Source->GetFName(), ...)
    // under the engine's own comment "overwrite the original script in place by constructing a new
    // one with the same name". That REPLACES the original wholesale; nothing merges the original's
    // current state back in. So a write that reaches the original emitter asset while its toolkit
    // is open is invisible to the toolkit and is destroyed by the next Apply, having already
    // reported success -- exactly the failure the caller cannot observe.
    //
    // Unlike RejectStructuralEditWhileAssetEditorOpen above this is NOT a per-verb hazard to adopt
    // where it applies: it holds for every write whatever verb makes it, so it is checked once at
    // the resolve step instead. It is keyed on the asset KIND -- a UNiagaraSystem is unaffected,
    // because FNiagaraSystemToolkit::Initialize edits the system asset itself, not a duplicate.
    //
    // Like the check above it keys off the asset rather than the editor class, so a shared editor
    // process refuses on somebody else's open toolkit too.
    //
    // Returns true and fills OutMessage when the write must be refused; returns false, leaving
    // OutMessage untouched, when nothing holds EmitterAsset open.
    inline bool RefuseEmitterAssetEditWhileToolkitOpen(
        UObject* EmitterAsset,
        const FString& AssetPath,
        FString& OutMessage)
    {
        IAssetEditorInstance* OpenEditor = FindOpenAssetEditorForNiagaraAsset(EmitterAsset);
        if (!OpenEditor)
        {
            return false;
        }

        OutMessage = FString::Printf(
            TEXT("Emitter asset '%s' is open in the '%s' asset editor. An emitter toolkit edits a ")
            TEXT("DUPLICATE of the emitter and overwrites this asset wholesale on Apply ")
            TEXT("(FNiagaraSystemToolkit::UpdateOriginalEmitter re-duplicates the toolkit's copy ")
            TEXT("over the original; it does not merge). This write would land on the original, be ")
            TEXT("reported as succeeding, and then be destroyed by the next Apply with nothing to ")
            TEXT("report the loss. Close the asset first (editor.close_asset), then retry. The ")
            TEXT("editor may have been opened by another caller sharing this editor process."),
            *AssetPath, *OpenEditor->GetEditorName().ToString());
        return true;
    }
}
