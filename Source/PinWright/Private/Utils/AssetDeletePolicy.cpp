// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/AssetDeletePolicy.h"

#include "Utils/AssetUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorAssetLibrary.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
// WeakObjectPtrTemplates.h declares TWeakObjectPtr but not its base FWeakObjectPtr; a
// strict-includes compile of this TU fails without the base header (same reason as
// Utils/PackageDirtyUtils.h).
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

// File-scope helper names are prefixed because Unity merges translation units.

// /Game/Foo/Bar.Bar -> /Game/Foo/Bar. Both forms reach the delete verbs.
static FString AssetDeletePolicy_PackageNameOf(const FString& AssetPath)
{
    FString PackageName = AssetPath;
    int32 DotIndex;
    if (PackageName.FindChar(TEXT('.'), DotIndex))
    {
        PackageName = PackageName.Left(DotIndex);
    }
    return PackageName;
}

// Names of every loaded package currently flagged dirty. Used only to bracket the force
// path, whose collateral the engine otherwise discards.
static void AssetDeletePolicy_CollectDirtyPackages(TSet<FString>& Out)
{
    for (TObjectIterator<UPackage> It; It; ++It)
    {
        if (IsValid(*It) && It->IsDirty())
        {
            Out.Add(It->GetName());
        }
    }
}

// Registry referencers of PackageName, self excluded, capped.
static void AssetDeletePolicy_CollectOnDiskReferencers(const FString& PackageName,
                                                      TArray<FString>& Out)
{
    IAssetRegistry& Registry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    TArray<FName> Referencers;
    Registry.GetReferencers(
        FName(*PackageName),
        Referencers,
        UE::AssetRegistry::EDependencyCategory::Package,
        UE::AssetRegistry::FDependencyQuery());

    for (const FName& Ref : Referencers)
    {
        const FString RefStr = Ref.ToString();
        if (RefStr == PackageName || Out.Num() >= AssetDeletePolicy::MaxReportedReferencers)
        {
            continue;
        }
        Out.AddUnique(RefStr);
    }
}

// Live holders of Asset, using the engine's own predicate over the ASSET OBJECT. This is
// the same call FAssetDeleteModel just made to refuse the delete, so the set reported is
// the set that blocked it. bInRequireReferencingProperties stays false: only the referencer
// objects are needed, and passing true re-runs the whole graph walk through IsReferenced()
// to fill in FProperty lists no payload here reports.
//
// A full FReferencerFinder walk over every live UObject - callers must keep it on the
// refusal path.
static void AssetDeletePolicy_CollectInMemoryReferencers(UObject* Asset, TArray<FString>& Out,
                                                        bool& bOutReferencedByUndo)
{
    if (!Asset)
    {
        return;
    }

    FReferencerInformationList Refs;
    bool bIsReferenced = false;
    ObjectTools::GatherObjectReferencersForDeletion(
        Asset, bIsReferenced, bOutReferencedByUndo, &Refs,
        /*bInRequireReferencingProperties=*/false);

    for (const FReferencerInformation& Ref : Refs.ExternalReferences)
    {
        if (Ref.Referencer && Out.Num() < AssetDeletePolicy::MaxReportedReferencers)
        {
            Out.AddUnique(Ref.Referencer->GetPathName());
        }
    }
}

// Shared refusal wording. Names what was NOT done as plainly as what was, because the whole
// point of the refusal is that the caller's referencers are still intact.
static FString AssetDeletePolicy_RefusalMessage(const FString& Subject, int32 OnDiskCount,
                                                int32 InMemoryCount, bool bReferencedByUndo)
{
    FString Message = FString::Printf(
        TEXT("'%s' is still referenced, so it was not deleted and nothing was changed: ")
        TEXT("%d package(s) reference it on disk and %d live object(s) hold it in memory. "),
        *Subject, OnDiskCount, InMemoryCount);

    if (OnDiskCount == 0 && InMemoryCount == 0)
    {
        Message = FString::Printf(
            TEXT("'%s' was not deleted and nothing was changed: the engine's safe delete ")
            TEXT("declined it and named no referencer. An open asset editor, a read-only ")
            TEXT("file, a level still using it, or an asset-registry scan in progress are ")
            TEXT("the causes that produce no referencer list. "),
            *Subject);
    }
    else if (bReferencedByUndo)
    {
        Message += TEXT("The transaction buffer is one of the holders (editor.undo history). ");
    }

    Message += TEXT(
        "Repoint or delete the referencers first, or pass force:true to delete anyway - "
        "force nulls every in-memory pointer to this asset editor-wide, irreversibly and "
        "without a transaction, and the .uasset can still survive that, leaving the "
        "referencers broken.");
    return Message;
}

AssetDeletePolicy::FResult AssetDeletePolicy::DeleteAsset(const FString& AssetPath, bool bForce)
{
    FResult Result;

    if (bForce)
    {
        Result.bForced = true;

        TSet<FString> DirtyBefore;
        AssetDeletePolicy_CollectDirtyPackages(DirtyBefore);

        // The engine's force path, unchanged: UEditorAssetLibrary::DeleteAsset ->
        // UEditorAssetSubsystem::DeleteAsset -> ObjectTools::ForceDeleteObjects.
        Result.bDeleted = UEditorAssetLibrary::DeleteAsset(AssetPath);

        TSet<FString> DirtyAfter;
        AssetDeletePolicy_CollectDirtyPackages(DirtyAfter);
        for (const FString& Name : DirtyAfter)
        {
            if (!DirtyBefore.Contains(Name))
            {
                Result.DirtiedPackages.Add(Name);
            }
        }
        Result.DirtiedPackages.Sort();
        return Result;
    }

    UObject* Asset = ResolveAsset(AssetPath, /*bLoadObject=*/true).Object;
    if (!Asset)
    {
        Result.Message = FString::Printf(
            TEXT("'%s' could not be loaded, so nothing was deleted."), *AssetPath);
        return Result;
    }

    // Held weakly across the call: ObjectTools::DeleteObjects runs a CollectGarbage of its
    // own, and on the success path Asset is destroyed by it.
    const TWeakObjectPtr<UObject> WeakAsset(Asset);
    const FString PackageName = AssetDeletePolicy_PackageNameOf(AssetPath);

    TArray<UObject*> ObjectsToDelete;
    ObjectsToDelete.Add(Asset);

    // CancelNotAllowed: an RPC has no user at the progress dialog, so consulting
    // GWarn->ReceivedUserCancel() could only ever produce a spurious abort.
    const int32 NumDeleted = ObjectTools::DeleteObjects(
        ObjectsToDelete, /*bShowConfirmation=*/false,
        ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
    if (NumDeleted > 0)
    {
        Result.bDeleted = true;
        return Result;
    }

    // Refused, having mutated nothing. Name the holders so the caller can release them or
    // opt into force. Both walks run ONLY here.
    Result.bRefused = true;
    AssetDeletePolicy_CollectOnDiskReferencers(PackageName, Result.OnDiskReferencers);
    AssetDeletePolicy_CollectInMemoryReferencers(
        WeakAsset.Get(), Result.InMemoryReferencers, Result.bReferencedByUndo);
    Result.Message = AssetDeletePolicy_RefusalMessage(
        AssetPath, Result.OnDiskReferencers.Num(), Result.InMemoryReferencers.Num(),
        Result.bReferencedByUndo);
    return Result;
}

AssetDeletePolicy::FResult AssetDeletePolicy::DeleteDirectory(const FString& DirectoryPath,
                                                             bool bForce)
{
    FResult Result;

    if (bForce)
    {
        Result.bForced = true;

        TSet<FString> DirtyBefore;
        AssetDeletePolicy_CollectDirtyPackages(DirtyBefore);

        Result.bDeleted = UEditorAssetLibrary::DeleteDirectory(DirectoryPath);

        TSet<FString> DirtyAfter;
        AssetDeletePolicy_CollectDirtyPackages(DirtyAfter);
        for (const FString& Name : DirtyAfter)
        {
            if (!DirtyBefore.Contains(Name))
            {
                Result.DirtiedPackages.Add(Name);
            }
        }
        Result.DirtiedPackages.Sort();
        return Result;
    }

    // Gate the contents first. Calling UEditorAssetLibrary::DeleteDirectory up front would
    // force-delete everything inside the folder before anything could be checked.
    TArray<FString> Contents;
    if (!GetAssetPathsUnderDirectory(DirectoryPath, /*bRecursive=*/true, Contents))
    {
        Result.bRefused = true;
        Result.Message = FString::Printf(
            TEXT("Could not enumerate assets under '%s'; directory delete was refused."),
            *DirectoryPath);
        return Result;
    }

    TArray<UObject*> ObjectsToDelete;
    for (const FString& ContainedPath : Contents)
    {
        if (UObject* Asset = ResolveAsset(ContainedPath, /*bLoadObject=*/true).Object)
        {
            ObjectsToDelete.Add(Asset);
        }
    }

    // Weak copies survive DeleteObjects' internal CollectGarbage; the raw array does not.
    TArray<TWeakObjectPtr<UObject>> WeakContents;
    for (UObject* Asset : ObjectsToDelete)
    {
        WeakContents.Add(Asset);
    }

    if (ObjectsToDelete.Num() > 0)
    {
        ObjectTools::DeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/false,
            ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
    }

    TArray<FString> Remaining;
    if (!GetAssetPathsUnderDirectory(DirectoryPath, /*bRecursive=*/true, Remaining))
    {
        Result.bRefused = true;
        Result.Message = FString::Printf(
            TEXT("Could not verify assets under '%s' after deletion; directory removal was "
                 "refused."),
            *DirectoryPath);
        return Result;
    }
    if (Remaining.Num() == 0)
    {
        // Empty now, so DeleteDirectory's own ForceDeleteObjects has nothing to force: it
        // only clears the registry path and removes the directory from disk.
        Result.bDeleted = UEditorAssetLibrary::DeleteDirectory(DirectoryPath);
        return Result;
    }

    // Something survived. Do NOT fall through to DeleteDirectory - it would force-delete
    // exactly the survivors this gate refused.
    Result.bRefused = true;
    for (const FString& SurvivorPath : Remaining)
    {
        AssetDeletePolicy_CollectOnDiskReferencers(
            AssetDeletePolicy_PackageNameOf(SurvivorPath), Result.OnDiskReferencers);
    }
    for (const TWeakObjectPtr<UObject>& WeakAsset : WeakContents)
    {
        bool bReferencedByUndo = false;
        AssetDeletePolicy_CollectInMemoryReferencers(
            WeakAsset.Get(), Result.InMemoryReferencers, bReferencedByUndo);
        Result.bReferencedByUndo = Result.bReferencedByUndo || bReferencedByUndo;
    }
    Result.Message = AssetDeletePolicy_RefusalMessage(
        DirectoryPath, Result.OnDiskReferencers.Num(), Result.InMemoryReferencers.Num(),
        Result.bReferencedByUndo);
    Result.Message += FString::Printf(
        TEXT(" %d asset(s) under this folder were kept; a folder is gated as one batch, so "
             "name individual paths to delete the rest."), Remaining.Num());
    return Result;
}
