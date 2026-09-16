// Copyright (c) 2026 Alexander Penkin. MIT License.

// Non-interactive redirector fix-up. Every entry point here is guaranteed never to
// open a dialog, and never calls IAssetTools::FixupReferencers.
//
// Why this exists: IAssetTools::FixupReferencers is FATAL under automation on UE 5.8.
// FAssetFixUpRedirectors::ExecuteFixUp ends with an UNCONDITIONAL modal report -
//
//     AssetFixUpRedirectors.cpp:938  SNew(SFixupRedirectorsReport, ...)
//     AssetFixUpRedirectors.cpp:939  Dialog->ShowModalDialog(...)
//
// - and SModalEditorDialog<bool>::ShowModalDialog (Dialogs.h:274-281) reads the
// dialog's answer with an unchecked TOptional::GetValue():
//
//     TOptional<ResultType> Result;
//     OnFinished.BindLambda([&Result](ResultType In){ Result.Emplace(...); });
//     UE::Private::ShowModalDialogWindow(Window.ToSharedRef());
//     return MoveTemp(Result.GetValue());          // Dialogs.h:280
//
// GIsRunningUnattendedScript makes Slate cancel the window instead of showing it
// (SlateApplication.cpp:2134), so OnFinished never fires, Result stays unset, and
// GetValue() hard-asserts at Optional.h:372 - the editor dies. The unattended guard
// converts the hang into a crash; it cannot make this call safe. Reproduced
// 2026-08-19 16:05:49 through asset.bulk_delete.
//
// There is no unattended-safe route through the engine call. All three
// ERedirectFixupMode values reach line 938 (the mode only picks bCanDelete, passed
// into the dialog), and bCheckoutDialogPrompt only suppresses an earlier, unrelated
// checkout prompt. SFixupRedirectorsReport is the only SModalEditorDialog subclass in
// UE 5.8, so this is the engine's single instance of the defect - but it sits on the
// only public redirector fix-up API.
//
// So this reimplements the non-interactive subset of ExecuteFixUp out of public API,
// mirroring the engine step for step:
//
//   engine :606  GetReferencers               -> FindReferencingPackages
//   engine :677  FindPackage / LoadPackage    -> LoadReferencingPackages
//   engine :745  ResetLoadersForWorldAsset    -> same
//   engine :847  RedirectorMap + blueprint _C -> BuildRedirectorMap
//   engine :863  RenameReferencingSoftObjectPaths (public on IAssetTools) -> same
//   engine :875  PromptForCheckoutAndSave     -> same (its own GIsRunningUnattendedScript
//                                                branch at FileHelpers.cpp:4658 makes it
//                                                silent, so the engine already treats
//                                                this half as automation-safe)
//   engine :921  ScanPathsSynchronous         -> same
//   engine :938  MODAL REPORT                 -> DROPPED (returns FResult instead)
//   engine :945  delete fully fixed-up redirectors -> DeleteFullyFixedRedirectors
//
// Deliberate differences from the engine, all of them dialog-avoidance:
//   - Packages loaded to be re-saved are NOT unloaded afterwards. The engine's
//     UPackageTools::UnloadPackages tail opens an FMessageDialog on failure
//     (AssetFixUpRedirectors.cpp:711-721); leaving them resident costs memory only.
//   - Collections are not re-saved (ICollectionManager::HandleRedirectorDeleted,
//     engine :901). PinWright does not depend on the CollectionManager module. A
//     collection that names a deleted redirector keeps a stale entry.
//   - A redirector is deleted only when EVERY referencing package it has was
//     re-saved. The engine reaches the same conclusion through its report dialog.
#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/ARFilter.h"
#include "UObject/SoftObjectPath.h"

class FJsonObject;
class UObjectRedirector;

namespace RedirectorFixupPolicy
{
    // How wide a redirector sweep is allowed to reach.
    //
    // Why this is a value and not an omission in a filter: an FARFilter carrying only
    // ClassPaths is a PROJECT-WIDE query - GetAssets returns every ObjectRedirector in
    // every mounted content root - and the fix-up that follows DELETES them and re-saves
    // their referencers. asset.bulk_delete ran exactly that query on its default path and
    // destroyed four host packages the caller never named, rewriting thirteen more
    // (board B-tests-destroy-host-assets, docs/defect-backlog.md D-72). The width
    // is now something a caller chooses, and the wide choice has a name.
    enum class ESweepScope : uint8
    {
        // Only the package folders the caller's own asset paths live in. The default.
        RequestedPaths,
        // Every mounted content root. Explicit opt-in only.
        Project
    };

    // Wire spellings: "paths" and "project", case-insensitive. Returns false on anything
    // else - an unrecognised scope must be an error, never a fall-back to the wider
    // sweep (docs/rpc-design.md §3).
    PINWRIGHT_API bool ParseSweepScope(const FString& In, ESweepScope& OutScope);
    PINWRIGHT_API const TCHAR* SweepScopeToString(ESweepScope Scope);

    // The deduplicated package FOLDERS a set of asset paths lives in: "/Game/Foo/SM_A"
    // and "/Game/Foo/SM_A.SM_A" both yield "/Game/Foo". A "/Content/..." spelling is
    // normalised to "/Game/..." the way asset.fixup_redirectors already accepts it.
    PINWRIGHT_API TArray<FName> PackageFoldersForAssets(const TArray<FString>& AssetPaths);

    // The registry query a sweep runs. ESweepScope::Project leaves PackagePaths unset,
    // which is the project-wide query; ESweepScope::RequestedPaths sets PackagePaths to
    // PackageFoldersForAssets(AssetPaths) with bRecursivePaths deliberately FALSE.
    //
    // The non-recursive choice is the point of the scoping, not an oversight. A recursive
    // sweep rooted at the folder of one deleted asset re-widens to everything beneath it,
    // and for an asset sitting at the top of a content root that is the whole project
    // again - the same blast radius under a different spelling. The redirectors a delete
    // can plausibly strand are the ones beside the deleted asset, which is exactly the
    // non-recursive set. A caller who wants more asks for it by name.
    PINWRIGHT_API FARFilter BuildSweepFilter(const TArray<FString>& AssetPaths, ESweepScope Scope);

    // True when a long package name ("/Game/Foo/SM_A") sits directly in one of Folders.
    // Compares the whole folder, never a string prefix: a prefix test folds the sibling
    // "/Game/FooOther" into "/Game/Foo" and would under-report collateral. Matches
    // BuildSweepFilter's bRecursivePaths=false, so the two cannot disagree about what
    // "inside the requested set" means.
    PINWRIGHT_API bool IsInPackageFolders(const FString& PackageName, const TArray<FName>& Folders);

    struct FResult
    {
        // Redirectors that were valid and had a DestinationObject. A redirector with a
        // null destination is counted in SkippedRedirectors instead: it points nowhere,
        // so there is no path to rewrite referencers to.
        int32 RedirectorsConsidered = 0;
        int32 SkippedRedirectors = 0;

        // Referencing packages found in the asset registry, and how many of those were
        // successfully re-saved against the resolved target path.
        int32 ReferencingPackagesFound = 0;
        int32 ReferencingPackagesSaved = 0;

        int32 RedirectorsDeleted = 0;

        // Long package names of the redirector packages this call actually removed.
        // Measured after the delete - a weak pointer that stopped resolving - not
        // predicted from the candidate list, because ObjectTools::DeleteObjects refuses
        // objects still referenced in memory and only lowers its returned count.
        // RedirectorsDeleted is the engine's count; this is the identity behind it, and
        // it is what lets a caller see WHICH packages went. A count alone is what made
        // the host-content deletion in B-tests-destroy-host-assets invisible.
        TArray<FString> DeletedRedirectorPackages;

        // Long package names that reference a redirector and could not be re-saved.
        // Their redirectors are left in place on purpose - deleting one would break
        // the reference the package still holds.
        TArray<FString> FailedPackages;

        // Referencing packages that are compiled in (PKG_CompiledIn). Code references
        // cannot be fixed up from the editor at all; the engine reports these the same
        // way, as a per-redirector failure.
        TArray<FString> CodeReferences;

        bool AllReferencersSaved() const
        {
            return FailedPackages.Num() == 0 && CodeReferences.Num() == 0;
        }
    };

    // Re-save every package that references one of Redirectors so it points at the
    // resolved target, then (when bDeleteFixedUpRedirectors) delete the redirectors
    // whose referencers all saved. Opens no dialog on any branch, and takes its own
    // FScopedUnattendedRpc so it is safe from a deferred continuation that runs
    // outside FRpcDispatcher's scope.
    PINWRIGHT_API FResult FixupReferencers(const TArray<UObjectRedirector*>& Redirectors,
                                           bool bDeleteFixedUpRedirectors);

    // Old path -> new path for each redirector, plus the two extra spellings a
    // Blueprint destination needs (`<Path>_C` and `<Package>.Default__<Asset>_C`).
    // Split out so the mapping is testable without touching packages on disk.
    PINWRIGHT_API TMap<FSoftObjectPath, FSoftObjectPath> BuildRedirectorMap(
        const TArray<UObjectRedirector*>& Redirectors);

    // Additive response fields: redirectorsConsidered, referencingPackagesFound,
    // referencingPackagesSaved, redirectorsDeleted, and the redirectorsDeletedPaths /
    // failedPackages / codeReferences arrays when non-empty.
    PINWRIGHT_API void AddReport(const TSharedPtr<FJsonObject>& Out, const FResult& Result);
}
