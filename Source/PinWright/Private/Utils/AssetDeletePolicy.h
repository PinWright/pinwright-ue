// Copyright (c) 2026 Alexander Penkin. MIT License.

// AssetDeletePolicy.h - non-forcing delete for the verbs that remove content.
//
// Why this exists (board B-force-delete-nulls-referencers): every delete reachable from
// UEditorAssetLibrary force-deletes. UEditorAssetSubsystem::DeleteAsset, DeleteLoadedAsset,
// DeleteLoadedAssets and DeleteDirectory all call ObjectTools::ForceDeleteObjects with
// bShowConfirmation=false (UE 5.8 EditorAssetSubsystem.cpp:769/792/814/864), so there is no
// safe entry point in that library at all and a verb built on it forces whether or not
// anyone asked.
//
// What force costs. ForceDeleteObjects runs ForceReplaceReferences(nullptr, ObjectsToReplace,
// ...) BEFORE it knows whether the package may go (ObjectTools.cpp:3953). With an empty
// ObjectsToReplaceWithin that walk visits every live UObject in the editor, replaces every
// pointer to the doomed asset with null, and calls MarkPackageDirty() on each object it
// touched (:1470). It is not transacted and the replace archive issues no Modify(), so it
// cannot be undone. Several steps later CleanupAfterSuccessfulDelete re-asks
// GatherObjectReferencersForDeletion and, if the package is still referenced, executes
// PackagesToDelete.RemoveAt() (:2551) - no log line, no return-value adjustment, no file
// delete. The .uasset survives with all its data and only the pointers to it are destroyed.
// Epic names the window in ForceDeleteObjects' own terminal ensureMsgf: the reference
// replacement and IsReferenced disagree about subobjects.
//
// The split below is the engine's, not a second opinion invented here.
// FAssetDeleteModel::CanDelete() is literally !CanForceDelete() (AssetDeleteModel.cpp:280),
// and ObjectTools::DeleteObjects routes through that model: a referenced target returns 0
// having mutated nothing, a clean one reaches DeleteObjectsUnchecked, which never calls
// ForceReplaceReferences. That is the same primitive asset.bulk_delete already uses, which
// is why the batch verb was safe while the single verb was not.
//
// force:true is still offered. ForceDeleteObjects is the only way to delete a referenced
// asset, the interactive editor offers exactly that behind a separate red-button confirm,
// and removing it would leave callers with no way to do something the editor can. It CANNOT
// be made safe from here - the replacement/IsReferenced mismatch is inside ObjectTools - so
// all this policy does for that path is make it opt-in, named, and reported: FResult
// ::DirtiedPackages recovers the collateral ForceDeleteObjects discards.
#pragma once

#include "CoreMinimal.h"

namespace AssetDeletePolicy
{
    // Referencer lists are capped so a refusal payload cannot blow the response budget on a
    // heavily-referenced asset.
    inline constexpr int32 MaxReportedReferencers = 25;

    struct FResult
    {
        // The engine reported the target gone. Callers still probe existence themselves -
        // this is a claim, not a measurement.
        bool bDeleted = false;

        // The safe path declined and mutated NOTHING: no reference was nulled, no package
        // was dirtied, no file was touched. Referencer lists below say why.
        bool bRefused = false;

        // The force path ran (caller passed force:true).
        bool bForced = false;

        // Refusal only: packages that hard-reference the target according to the asset
        // registry (on disk), capped at MaxReportedReferencers.
        TArray<FString> OnDiskReferencers;

        // Refusal only: live holders found by the engine's own
        // GatherObjectReferencersForDeletion over the ASSET OBJECT, capped. Deliberately not
        // over the package: GARBAGE_COLLECTION_KEEPFLAGS is RF_Standalone in the editor, so a
        // pre-delete gather on a package always reports the asset inside it as an internal
        // referencer and would refuse every delete.
        TArray<FString> InMemoryReferencers;

        // Refusal only: the transaction buffer is one of the holders.
        bool bReferencedByUndo = false;

        // Force path only: packages that were clean before the call and dirty after it.
        // ForceDeleteObjects declares its FForceReplaceInfo as a scoped local and discards
        // it, so DirtiedPackages is unreachable from the caller; diffing the editor-wide
        // dirty set across the call recovers the same answer. Every package named here had
        // a pointer replaced with null and must NOT be saved - asset.reload re-reads it from
        // disk and is the only in-plugin way back.
        TArray<FString> DirtiedPackages;

        // Printable reason, non-empty whenever bRefused or a load failed.
        FString Message;
    };

    // Delete one asset. bForce=false never reaches ObjectTools::ForceDeleteObjects.
    PINWRIGHT_API FResult DeleteAsset(const FString& AssetPath, bool bForce);

    // Delete a content folder and everything under it. The safe path gates the folder's
    // assets as ONE batch, which is what lets assets inside the folder reference each other
    // (FAssetDeleteModel discounts references from other pending deletes); a single asset
    // held from outside the folder therefore refuses the whole folder. Partial folder
    // deletes are available by naming paths instead.
    PINWRIGHT_API FResult DeleteDirectory(const FString& DirectoryPath, bool bForce);
}
