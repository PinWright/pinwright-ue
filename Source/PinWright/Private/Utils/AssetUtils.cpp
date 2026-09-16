// Copyright (c) 2026 Alexander Penkin. MIT License.

// Asset resolution, loading, saving, and editor helpers for PinWright
#include "Utils/AssetUtils.h"

#include "Runtime/Launch/Resources/Version.h"
#include "State/PluginState.h"
#include "PinWrightGlobals.h"
#include "PinWrightSubsystem.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Utils/PackageDiskStateGuard.h"
#include "Utils/PieSaveBlockGuard.h"

#include "AssetRegistry/AssetData.h"
#include "Containers/ScriptArray.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformTime.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "UObject/UnrealType.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "Blueprint/BlueprintSupport.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectIterator.h"
#include "RenderingThread.h"
#include "Engine/Blueprint.h"

#if __has_include("EditorAssetLibrary.h")
#include "EditorAssetLibrary.h"
#else
#include "Editor/EditorAssetLibrary.h"
#endif

#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsEngine/PhysicsAsset.h"
// FPhysicsAssetUtils::CreateFromSkeletalMesh + FPhysAssetCreateParams (PhysicsUtilities module).
#include "PhysicsAssetUtils.h"

// ============================================================================
// Asset Path Normalization
// ============================================================================

FNormalizedAssetPath NormalizeAssetPath(const FString& InPath)
{
    FNormalizedAssetPath Result;
    Result.bIsValid = false;

    if (InPath.IsEmpty())
    {
        Result.ErrorMessage = TEXT("Asset path is empty");
        return Result;
    }

    FString CleanPath = InPath;

    // Remove trailing slashes
    while (CleanPath.EndsWith(TEXT("/")))
    {
        CleanPath.RemoveAt(CleanPath.Len() - 1);
    }

    // Handle object paths (extract package name)
    // Object paths look like: /Game/Package.Object:SubObject
    FString PackageName = FPackageName::ObjectPathToPackageName(CleanPath);
    if (!PackageName.IsEmpty())
    {
        CleanPath = PackageName;
    }

    // If path doesn't start with '/', try prepending /Game/
    if (!CleanPath.StartsWith(TEXT("/")))
    {
        CleanPath = TEXT("/Game/") + CleanPath;
    }

    // Validate using engine API
    FText Reason;
    if (FPackageName::IsValidLongPackageName(CleanPath, true, &Reason))
    {
        Result.Path = CleanPath;
        Result.bIsValid = true;
        return Result;
    }

    // AN INVALID PATH IS NOW REPORTED, NOT RE-ROOTED. There used to be a fallback here that,
    // when the caller's path failed the check above, threw away the entire folder chain, kept
    // only the leaf segment, and retried it under /Game/, /Engine/ and /Script/ - returning
    // bIsValid=true naming a DIFFERENT PACKAGE, with nothing in FNormalizedAssetPath saying a
    // substitution had happened. Three separate reasons it had to go:
    //
    //   1. It is a data-loss path, not a convenience. sequencer.export_anim_sequence pairs this
    //      function with `overwrite`, so a re-rooted result names an asset the caller never
    //      asked for and the export rewrites it. The fallback's own DoesPackageExist test made
    //      that WORSE rather than safer: it returned a path only when the different package
    //      genuinely existed, i.e. only when there was something real to clobber.
    //   2. It laundered a lethal input into a valid one. "/Game//A/B" fails the check above,
    //      but the leaf "B" retried as "/Game/B" is valid - so a "//" argument came back
    //      bIsValid=true pointing somewhere else entirely.
    //   3. A caller re-checking the OUTPUT could not catch either, because the fallback only
    //      ever emitted paths that pass IsValidLongPackageName by construction. That is exactly
    //      the check SequencerBakeHandler.cpp adds on top of bIsValid.
    //
    // A bare name ("MyMesh") is unaffected: it is prepended with /Game/ at the top of this
    // function, BEFORE the validity check, and never reached the fallback. Only re-rooting of an
    // already-rooted path is gone.

    // Return what we have, with the validation error
    Result.Path = CleanPath;
    Result.ErrorMessage = FString::Printf(
        TEXT("Invalid asset path '%s': %s. Expected format: "
             "/Game/Folder/AssetName or /Engine/Folder/AssetName"),
        *InPath, *Reason.ToString());
    return Result;
}

FResolvedAssetPackage ResolveAssetPathToPackage(const FString& InPath)
{
    FResolvedAssetPackage Out;

    const FString Trimmed = InPath.TrimStartAndEnd();
    if (Trimmed.IsEmpty())
    {
        Out.ErrorMessage = TEXT("Asset path is empty");
        return Out;
    }

    // Step 1 is SYNTAX ONLY. NormalizeAssetPath strips an object/sub-object
    // suffix and validates the long-package-name form against the mounted roots;
    // it proves nothing about the asset being present. That gap is the whole
    // reason this function exists - see the header comment for the exact
    // accept/reject contract.
    const FNormalizedAssetPath Normalized = NormalizeAssetPath(Trimmed);
    if (!Normalized.bIsValid)
    {
        Out.ErrorMessage = Normalized.ErrorMessage.IsEmpty()
            ? FString::Printf(TEXT("Invalid asset path '%s'"), *InPath)
            : Normalized.ErrorMessage;
        return Out;
    }

    const FString PackagePath = Normalized.Path;
    const FName PackageFName(*PackagePath);

    // The asset-name half of the caller's own spelling, kept only so the verdict
    // can report WHICH row matched. Skipped when NormalizeAssetPath rewrote the
    // package half - today only its /Game/ prepend for a bare name - because then
    // the suffix no longer belongs to the package that was resolved. (It used to
    // also cover the re-rooting fallback, which has been deleted; the guard is
    // still needed for the prepend.)
    FString RequestedAssetName;
    if (FPackageName::ObjectPathToPackageName(Trimmed) == PackagePath)
    {
        int32 DotIndex = INDEX_NONE;
        if (Trimmed.FindChar(TEXT('.'), DotIndex))
        {
            RequestedAssetName = Trimmed.Mid(DotIndex + 1);
            int32 SubObjectIndex = INDEX_NONE;
            if (RequestedAssetName.FindChar(TEXT(':'), SubObjectIndex))
            {
                RequestedAssetName.LeftInline(SubObjectIndex);
            }
        }
    }

    IAssetRegistry& AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

    // Probe 1 - the registry rows of the package, queried BY PACKAGE.
    //
    // The reference verbs used to call GetAssetByObjectPath(FSoftObjectPath(Path))
    // instead, and that is what made the short "/Game/Foo/Bar" form work for some
    // assets and fail for others. FSoftObjectPath::SetPath parses a string with no
    // '.' as {PackageName=/Game/Foo/Bar, AssetName=None} ("No delimiter, package
    // name only", SoftObjectPath.cpp), and FAssetRegistryState holds no row keyed
    // on a None asset name - so the state lookup always missed. The only reason it
    // ever succeeded is the fast path at the top of GetAssetByObjectPath, which
    // does FindObject<UObject> on the stringified path first: for an already-LOADED
    // package that finds the UPackage itself and FAssetData(UPackage) comes back
    // valid. Load state is not something the caller controls, hence the
    // works-sometimes behaviour. Querying by package name removes the dependence
    // on load state and, unlike appending ".<leaf>", still resolves a package whose
    // asset name differs from its leaf name.
    TArray<FAssetData> PackageAssets;
    AssetRegistry.GetAssetsByPackageName(PackageFName, PackageAssets,
        /*bIncludeOnlyOnDiskAssets=*/false, /*bSkipARFilteredAssets=*/false);
    if (PackageAssets.Num() > 0)
    {
        const FAssetData* Match = nullptr;
        if (!RequestedAssetName.IsEmpty())
        {
            Match = PackageAssets.FindByPredicate([&RequestedAssetName](const FAssetData& Data)
            {
                return Data.AssetName.ToString().Equals(RequestedAssetName, ESearchCase::IgnoreCase);
            });
        }
        if (!Match)
        {
            // No suffix, or a suffix naming something this package does not hold:
            // fall back to the package's primary asset, else its first row. The
            // dependency graph is per package, so this cannot pick a wrong node.
            Match = PackageAssets.FindByPredicate([](const FAssetData& Data) { return Data.IsUAsset(); });
        }
        Out.PackageName = PackageFName;
        Out.AssetName = Match ? Match->AssetName : PackageAssets[0].AssetName;
        Out.bIsValid = true;
        return Out;
    }

    // Probe 2 - a package that is live in memory or has a file on disk but that
    // the registry has not indexed yet (scan in flight, or an asset created this
    // session). Calling that "not found" would be a false negative in the
    // dangerous direction for a caller checking references before a delete, so it
    // resolves with AssetName left None. FindPackage does no disk I/O.
    if (FindPackage(nullptr, *PackagePath) != nullptr || FPackageName::DoesPackageExist(PackagePath))
    {
        Out.PackageName = PackageFName;
        Out.bIsValid = true;
        return Out;
    }

    Out.ErrorMessage = FString::Printf(
        TEXT("Asset not found: '%s' (resolved package '%s' has no asset registry row, ")
        TEXT("is not loaded, and has no package file on disk)"),
        *InPath, *PackagePath);
    return Out;
}

FResolvedAsset ResolveAsset(const FString& InPath, bool bLoadObject)
{
    FResolvedAsset Out;
    const FString Trimmed = InPath.TrimStartAndEnd();
    const FNormalizedAssetPath Normalized = NormalizeAssetPath(Trimmed);
    if (!Normalized.bIsValid)
    {
        Out.ErrorMessage = Normalized.ErrorMessage;
        return Out;
    }

    Out.PackageName = FName(*Normalized.Path);

    const FString ExportObjectPath = FPackageName::ExportTextPathToObjectPath(Trimmed);
    FString RequestedAssetName;
    int32 DotIndex = INDEX_NONE;
    if (ExportObjectPath.FindChar(TEXT('.'), DotIndex))
    {
        RequestedAssetName = ExportObjectPath.Mid(DotIndex + 1);
        int32 SubObjectIndex = INDEX_NONE;
        if (RequestedAssetName.FindChar(TEXT(':'), SubObjectIndex))
        {
            RequestedAssetName.LeftInline(SubObjectIndex);
        }
    }

    IAssetRegistry& AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    TArray<FAssetData> PackageAssets;
    AssetRegistry.GetAssetsByPackageName(
        Out.PackageName, PackageAssets,
        /*bIncludeOnlyOnDiskAssets=*/false, /*bSkipARFilteredAssets=*/false);

    const FAssetData* Match = nullptr;
    if (!RequestedAssetName.IsEmpty())
    {
        Match = PackageAssets.FindByPredicate([&RequestedAssetName](const FAssetData& Data)
        {
            return Data.AssetName.ToString().Equals(RequestedAssetName, ESearchCase::IgnoreCase);
        });
    }
    else
    {
        Match = PackageAssets.FindByPredicate([](const FAssetData& Data)
        {
            return Data.IsUAsset();
        });
        if (!Match && PackageAssets.Num() > 0)
        {
            Match = &PackageAssets[0];
        }
    }

    FString ObjectPath;
    if (Match)
    {
        Out.AssetData = *Match;
        Out.AssetName = Match->AssetName;
        Out.ObjectPath = Match->GetSoftObjectPath();
        ObjectPath = Out.ObjectPath.ToString();
        Out.bExists = true;
        Out.bRegistryOrMemoryExists = true;
    }
    else
    {
        const FString AssetName = RequestedAssetName.IsEmpty()
            ? FPackageName::GetShortName(Normalized.Path)
            : RequestedAssetName;
        ObjectPath = FString::Printf(TEXT("%s.%s"), *Normalized.Path, *AssetName);
        Out.AssetName = FName(*AssetName);
        Out.ObjectPath = FSoftObjectPath(ObjectPath);
    }

    UObject* Object = FindObject<UObject>(nullptr, *ObjectPath);
    if (!Object && bLoadObject)
    {
        Out.bLoadAttempted = true;
        Object = LoadObject<UObject>(nullptr, *ObjectPath);
    }
    if (Object)
    {
        Out.Object = Object;
        Out.bExists = true;
        Out.bRegistryOrMemoryExists = true;
        Out.PackageName = Object->GetOutermost()->GetFName();
        Out.AssetName = Object->GetFName();
        Out.ObjectPath = FSoftObjectPath(Object);
        if (!Out.AssetData.IsValid())
        {
            Out.AssetData = FAssetData(Object);
        }
        return Out;
    }

    if (Out.bExists)
    {
        if (bLoadObject)
        {
            Out.ErrorMessage = FString::Printf(TEXT("Asset exists but could not be loaded: %s"), *InPath);
        }
        return Out;
    }

    if (RequestedAssetName.IsEmpty() && FPackageName::DoesPackageExist(Normalized.Path))
    {
        Out.bExists = true;
        Out.bPackageFileExists = true;
        if (bLoadObject)
        {
            Out.ErrorMessage = FString::Printf(TEXT("Asset package exists but could not be loaded: %s"), *InPath);
        }
        return Out;
    }

    Out.ErrorMessage = FString::Printf(TEXT("Asset not found: %s"), *InPath);
    return Out;
}

bool DoesAssetDirectoryExist(const FString& InPath)
{
    FString DirectoryPath = InPath.TrimStartAndEnd();
    while (DirectoryPath.Len() > 1 && DirectoryPath.EndsWith(TEXT("/")))
    {
        DirectoryPath.LeftChopInline(1);
    }

    FText Reason;
    if (!FPackageName::IsValidLongPackageName(DirectoryPath, true, &Reason))
    {
        return false;
    }

    IAssetRegistry& AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    if (AssetRegistry.PathExists(FName(*DirectoryPath)))
    {
        return true;
    }

    FString DirectoryFilename;
    return FPackageName::TryConvertLongPackageNameToFilename(DirectoryPath, DirectoryFilename) &&
        IFileManager::Get().DirectoryExists(*DirectoryFilename);
}

bool GetAssetPathsUnderDirectory(const FString& InPath, bool bRecursive, TArray<FString>& OutPaths)
{
    OutPaths.Reset();

    FString DirectoryPath = InPath.TrimStartAndEnd();
    while (DirectoryPath.Len() > 1 && DirectoryPath.EndsWith(TEXT("/")))
    {
        DirectoryPath.LeftChopInline(1);
    }

    FText Reason;
    if (!FPackageName::IsValidLongPackageName(DirectoryPath, true, &Reason))
    {
        return false;
    }

    if (!DoesAssetDirectoryExist(DirectoryPath))
    {
        return false;
    }

    // ListAssets ensures the registry has completed its search before enumerating. Match that
    // contract here so a deletion policy never treats an incomplete registry as an empty folder.
    ScanPathSynchronous(DirectoryPath, bRecursive);

    IAssetRegistry& AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    TArray<FAssetData> Assets;
    if (!AssetRegistry.GetAssetsByPath(FName(*DirectoryPath), Assets, bRecursive,
            /*bIncludeOnlyOnDiskAssets=*/false))
    {
        return false;
    }

    for (const FAssetData& Asset : Assets)
    {
        if (Asset.IsValid())
        {
            OutPaths.Add(Asset.GetSoftObjectPath().ToString());
        }
    }

    return true;
}

FString TryResolveAssetPath(const FString& InPath,
                            FString* OutResolvedPath,
                            FString* OutError)
{
    FNormalizedAssetPath Norm = NormalizeAssetPath(InPath);
    if (OutResolvedPath)
    {
        *OutResolvedPath = Norm.Path;
    }
    if (OutError && !Norm.bIsValid)
    {
        *OutError = Norm.ErrorMessage;
    }
    return Norm.bIsValid ? Norm.Path : FString();
}

FString ResolveAssetPath(const FString& InputPath)
{
    if (InputPath.IsEmpty())
        return FString();

    // 1. Exact match check
    if (ResolveAsset(InputPath).bExists)
    {
        return InputPath;
    }

    // 2. Exact match with /Game/ prepended if it looks like a relative path but
    // missing root
    if (!InputPath.StartsWith(TEXT("/")))
    {
        FString GamePath = TEXT("/Game/") + InputPath;
        if (ResolveAsset(GamePath).bExists)
        {
            return GamePath;
        }
    }

    // 3. Search by name if it's a short name (no slashes)
    if (!InputPath.Contains(TEXT("/")))
    {
        FString ShortName = FPaths::GetBaseFilename(InputPath);

        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        TArray<FAssetData> FoundAssets;
        TArray<FAssetData> AllGameAssets;

        // Use GetAssetsByPath with recursive search - more efficient than GetAllAssets
        AssetRegistry.GetAssetsByPath(FName(TEXT("/Game")), AllGameAssets, /*bRecursive=*/true);

        // Filter by name match (case-insensitive)
        for (const FAssetData& Asset : AllGameAssets)
        {
            if (Asset.AssetName.ToString().Equals(ShortName, ESearchCase::IgnoreCase))
            {
                FoundAssets.Add(Asset);
            }
        }

        // Return unique match
        if (FoundAssets.Num() == 1)
        {
            return FoundAssets[0].PackageName.ToString();
        }

        // Multiple matches - prefer /Game/ assets
        if (FoundAssets.Num() > 1)
        {
            for (const FAssetData& Data : FoundAssets)
            {
                if (Data.PackageName.ToString().StartsWith(TEXT("/Game/")))
                {
                    return Data.PackageName.ToString();
                }
            }
            // Return first match if none start with /Game/
            return FoundAssets[0].PackageName.ToString();
        }
    }

    return FString();
}

// ============================================================================
// Asset Save Helpers
// ============================================================================

// Mount-aware .uasset-on-disk probe shared by IsAssetPersistedToDisk and the
// verification helpers. TryConvertLongPackageNameToFilename (not the
// LongPackageNameToFilename variant, which is UE_LOG(Fatal) on an unregistered
// mount) so the transient package and any exotic root report false instead of
// killing the editor. Declared in AssetUtils.h: asset.delete's post-check asks the
// same question, and a registry-only answer there reads a surviving file as deleted.
bool DoesPackageFileExistOnDisk(const FString& PackageName)
{
    if (PackageName.IsEmpty())
    {
        return false;
    }
    // Both content extensions: a UWorld's package is .umap and everything else is
    // .uasset, so probing only .uasset would report a perfectly persisted level as
    // absent — a false negative is the same defect class in the other direction.
    const FString Extensions[] = {
        FPackageName::GetAssetPackageExtension(),
        FPackageName::GetMapPackageExtension()
    };
    IFileManager& FileManager = IFileManager::Get();
    for (const FString& Extension : Extensions)
    {
        FString PackageFilename;
        if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, PackageFilename, Extension)
            && FileManager.FileSize(*PackageFilename) >= 0)
        {
            return true;
        }
    }
    return false;
}

void McpSafeAssetSave(UObject* Asset)
{
    if (!Asset)
        return;

    // UE 5.7+ Fix: Do not immediately save newly created assets to disk.
    // Saving immediately causes bulkdata corruption and crashes.
    // Instead, mark the package dirty and notify the asset registry.
    //
    // Returns void: there is no success to report. The mark always succeeds and
    // never persists anything, so a bool return could only ever have been the
    // constant `true` — which is exactly what callers were publishing as `saved`.
    // Report through AddMarkDirtySaveReport, which measures.
    Asset->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(Asset);
}

bool IsAssetPersistedToDisk(const UObject* Asset)
{
    if (!Asset)
    {
        return false;
    }
    const UPackage* Package = Asset->GetPackage();
    if (!Package || Package == GetTransientPackage() || Package->HasAnyFlags(RF_Transient))
    {
        return false;
    }
    // Both halves are load-bearing. On-disk alone is the existence-not-freshness
    // hole: a stale .uasset from an earlier save satisfies it while the newest
    // edit sits unwritten in memory. Clean alone is not enough either: a
    // FactoryCreateNew package is clean and has never been written.
    return !Package->IsDirty() && DoesPackageFileExistOnDisk(Package->GetName());
}

void AddMarkDirtySaveReport(const TSharedPtr<FJsonObject>& Result, UObject* Asset,
                            bool bSaveRequested)
{
    if (!Result)
    {
        return;
    }

    const bool bPersisted = IsAssetPersistedToDisk(Asset);

    Result->SetBoolField(TEXT("saveRequested"), bSaveRequested);
    // markedForSave records what this path actually did — dirty the package and
    // notify the registry — so a caller reading saved:false can tell a deferred
    // write apart from a save that never happened.
    Result->SetBoolField(TEXT("markedForSave"), bSaveRequested && Asset != nullptr);
    Result->SetBoolField(TEXT("saved"), bSaveRequested && bPersisted);
    if (bSaveRequested && !bPersisted)
    {
        // Matches AddAssetSaveReport's contract: the edit needs an editor.save_all /
        // asset.save before it survives a restart.
        Result->SetBoolField(TEXT("pendingFlush"), true);
        // ...and that flush is refused outright while PIE is up, so say so here rather than
        // letting the caller discover it one verb later.
        PinWrightPieSaveBlock::AddPieSaveBlockJsonIfBlocked(Result);
    }
}

void AddAssetSaveSizeReport(const TSharedPtr<FJsonObject>& Result, int64 SizeBytesOnDisk,
                            bool bSavedToDisk)
{
    if (!Result)
    {
        return;
    }
    Result->SetNumberField(TEXT("sizeBytes"), static_cast<double>(SizeBytesOnDisk));

    // Only when a pre-existing file supplied the count. A non-durable save that reports 0 has
    // no file behind it and nothing to mislabel, and flagging it would claim a revision exists.
    if (!bSavedToDisk && SizeBytesOnDisk > 0)
    {
        Result->SetBoolField(TEXT("sizeBytesIsStale"), true);
    }
}

FPhysicsAssetCreateResult McpCreatePhysicsAssetFromSkeletalMeshHeadless(
    UObject* Outer, FName AssetName, USkeletalMesh* Mesh, bool bSave)
{
    FPhysicsAssetCreateResult Out;

    UPhysicsAsset* PhysicsAsset =
        NewObject<UPhysicsAsset>(Outer, AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!PhysicsAsset)
    {
        Out.ErrorMessage = TEXT("Failed to create physics asset object");
        return Out;
    }

    FText CreateError;
    FPhysAssetCreateParams CreateParams;
#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // UE 5.3: CreateFromSkeletalMesh has no bShowProgress parameter (added in 5.4) and shows no
    // progress UI at all, so the modal-hang concern bShowProgress=false guards does not apply.
    if (!FPhysicsAssetUtils::CreateFromSkeletalMesh(PhysicsAsset, Mesh, CreateParams, CreateError,
            /*bSetToMesh=*/false))
#else
    if (!FPhysicsAssetUtils::CreateFromSkeletalMesh(PhysicsAsset, Mesh, CreateParams, CreateError,
            /*bSetToMesh=*/false, /*bShowProgress=*/false))
#endif
    {
        Out.ErrorMessage = CreateError.IsEmpty()
            ? TEXT("Failed to generate physics asset bodies from skeletal mesh")
            : CreateError.ToString();
        return Out;
    }

    // Register + mark dirty (mirrors what AssetTools/UPhysicsAssetFactory did on success).
    McpSafeAssetSave(PhysicsAsset);

    Out.Asset = PhysicsAsset;
    Out.AssetPath = PhysicsAsset->GetPathName();
    Out.PackageName = PhysicsAsset->GetOutermost()
        ? PhysicsAsset->GetOutermost()->GetName() : FString();
    if (bSave)
    {
        Out.bSavedToDisk = SaveAssetToDiskReportingPresence(
            PhysicsAsset, /*bForce=*/true, &Out.PackageName, &Out.SizeBytes, &Out.SaveState);
    }
    Out.bPendingFlush = bSave && !Out.bSavedToDisk;
    Out.bSuccess = true;
    return Out;
}

bool DoesRequestedLevelMatchCurrentWorld(
    const FString& RequestedLevelPath,
    const FString& CurrentWorldPackageName,
    const FString& CurrentMapName)
{
    if (RequestedLevelPath.TrimStartAndEnd().IsEmpty() || CurrentWorldPackageName.TrimStartAndEnd().IsEmpty())
    {
        return false;
    }

    FString Requested = RequestedLevelPath.TrimStartAndEnd();
    const FString Extension = FPackageName::GetMapPackageExtension();

    FString PackageFromObjectPath = FPackageName::ObjectPathToPackageName(Requested);
    if (!PackageFromObjectPath.IsEmpty())
    {
        Requested = PackageFromObjectPath;
    }

    if (FPackageName::IsPackageFilename(Requested))
    {
        FString ConvertedPackageName;
        if (FPackageName::TryConvertFilenameToLongPackageName(Requested, ConvertedPackageName))
        {
            Requested = ConvertedPackageName;
        }
    }
    else if (Requested.StartsWith(TEXT("/")) && Requested.EndsWith(Extension, ESearchCase::IgnoreCase))
    {
        Requested = FPaths::ChangeExtension(Requested, TEXT(""));
    }

    if (CurrentWorldPackageName.Equals(Requested, ESearchCase::IgnoreCase))
    {
        return true;
    }

    FString RequestedShortName = FPackageName::GetShortName(Requested);
    if (RequestedShortName.IsEmpty())
    {
        RequestedShortName = FPaths::GetBaseFilename(Requested);
    }

    if (!RequestedShortName.IsEmpty())
    {
        const FString CurrentWorldShortName = FPackageName::GetShortName(CurrentWorldPackageName);
        if (CurrentWorldShortName.Equals(RequestedShortName, ESearchCase::IgnoreCase))
        {
            return true;
        }

        if (CurrentMapName.Equals(RequestedShortName, ESearchCase::IgnoreCase))
        {
            return true;
        }
    }

    return false;
}

bool IsLevelPackagePresentInMemoryOrRegistry(const FString& LevelPathOrPackageName)
{
    FString PackageName = LevelPathOrPackageName.TrimStartAndEnd();
    if (PackageName.IsEmpty())
    {
        return false;
    }

    // Normalise object path (/Game/Maps/M.M) and a trailing .umap down to the
    // bare package name (/Game/Maps/M) so the lookups below match the UPackage.
    const FString MapExtension = FPackageName::GetMapPackageExtension();
    if (PackageName.EndsWith(MapExtension, ESearchCase::IgnoreCase))
    {
        PackageName = FPaths::ChangeExtension(PackageName, TEXT(""));
    }
    const FString FromObjectPath = FPackageName::ObjectPathToPackageName(PackageName);
    if (!FromObjectPath.IsEmpty())
    {
        PackageName = FromObjectPath;
    }

    // In-memory: a live UPackage covers a newly-created / loaded-but-unsaved world.
    // A UWorld in memory cannot exist without its outermost UPackage also being
    // live, so this single FindPackage subsumes any FindObject<UWorld> at the same
    // path. FindPackage does no disk I/O.
    if (FindPackage(nullptr, *PackageName))
    {
        return true;
    }

    // Registry: a World asset listed at this package even with no .umap on disk —
    // the exact discrepancy level.list surfaces while level.load returns missing.
    IAssetRegistry& AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
    TArray<FAssetData> Assets;
    AssetRegistry.GetAssetsByPackageName(FName(*PackageName), Assets, /*bIncludeOnlyOnDiskAssets=*/false);
    for (const FAssetData& Asset : Assets)
    {
        if (Asset.AssetClassPath == FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("World")))
        {
            return true;
        }
    }
    return false;
}

bool ResolveLevelPackageToMapFilename(
    const FString& LevelPackageName,
    FString& OutMapFilename)
{
    OutMapFilename.Reset();

    // Strip an optional trailing .umap so we hand TryConvert a clean package name.
    FString PackageName = LevelPackageName;
    const FString MapExtension = FPackageName::GetMapPackageExtension();
    if (PackageName.EndsWith(MapExtension, ESearchCase::IgnoreCase))
    {
        PackageName = FPaths::ChangeExtension(PackageName, TEXT(""));
    }

    // Mount-aware conversion: /Game -> <Project>/Content, /Engine -> <Engine>/Content,
    // plugin roots -> their real content dirs. This is the same resolver the sync
    // sibling level.load uses (LevelHandler.cpp), and the only correct one here.
    return FPackageName::TryConvertLongPackageNameToFilename(
        PackageName, OutMapFilename, MapExtension);
}

bool DoesLevelMapExistOnDisk(const FString& LevelPath)
{
    if (LevelPath.IsEmpty())
    {
        return false;
    }
    FString MapFilename;
    if (ResolveLevelPackageToMapFilename(LevelPath, MapFilename)
        && IFileManager::Get().FileExists(*MapFilename))
    {
        return true;
    }
    return FPackageName::DoesPackageExist(LevelPath);
}

bool ShouldTreatLevelSaveAsSuccess(
    bool bSaveReportedSuccess,
    bool bFileExistsOnDisk,
    bool bPackageExists,
    bool bAssetExists,
    bool bPackageClean)
{
    if (!bSaveReportedSuccess)
    {
        return false;
    }

    return bFileExistsOnDisk || bPackageExists || bAssetExists || bPackageClean;
}

bool ShouldTreatCreateLevelSaveAsSuccess(bool bSaveReportedSuccess, bool bFileExistsOnDisk)
{
    // For a fresh create-to-new-path, the cleared dirty flag / registry entry are
    // not honest persistence signals; only the .umap on disk is. Both must hold.
    return bSaveReportedSuccess && bFileExistsOnDisk;
}

bool VerifyLevelSavedToDisk(
    const FString& LevelPackageOrSavePath,
    bool bSaveReportedSuccess,
    FString& OutMapFilename,
    FString& OutErrorCode)
{
    OutMapFilename.Reset();
    OutErrorCode.Reset();

    // Mount-aware resolve (strips a trailing .umap) + direct file probe. A missing
    // mount leaves OutMapFilename empty and bFileOnDisk false, which correctly
    // fails the re-gate below.
    const bool bFileOnDisk =
        ResolveLevelPackageToMapFilename(LevelPackageOrSavePath, OutMapFilename)
        && IFileManager::Get().FileExists(*OutMapFilename);

    const bool bOk = ShouldTreatCreateLevelSaveAsSuccess(bSaveReportedSuccess, bFileOnDisk);
    if (!bOk)
    {
        OutErrorCode = bSaveReportedSuccess
            ? TEXT("SAVE_VERIFICATION_FAILED")  // reported success but no .umap landed (in-memory-only / write blocked)
            : TEXT("SAVE_FAILED");
    }
    return bOk;
}

// Log-only name for a raw engine-call outcome. Distinct from AssetSaveStateToWire: that one
// is the caller-facing verdict and is contract, this one is the internal branch that produced
// it and exists so a log line reads as a fact instead of "outcome=4".
//
// Prefixed rather than placed in an anonymous namespace because Unity merges this TU with its
// neighbours (see CLAUDE.md, Build notes).
static const TCHAR* AssetUtils_SaveOutcomeName(ESaveLoadedAssetOutcome Outcome)
{
    switch (Outcome)
    {
    case ESaveLoadedAssetOutcome::Saved:                 return TEXT("Saved");
    case ESaveLoadedAssetOutcome::SkippedAlreadyClean:   return TEXT("SkippedAlreadyClean");
    case ESaveLoadedAssetOutcome::SkippedThrottledDirty: return TEXT("SkippedThrottledDirty");
    case ESaveLoadedAssetOutcome::NotPersistable:        return TEXT("NotPersistable");
    case ESaveLoadedAssetOutcome::Failed:                return TEXT("Failed");
    case ESaveLoadedAssetOutcome::RefusedDiskStateDiverged:
        return TEXT("RefusedDiskStateDiverged");
    case ESaveLoadedAssetOutcome::RefusedBlockedByPie:
        return TEXT("RefusedBlockedByPie");
    }
    return TEXT("Unknown");
}

const TCHAR* AssetSaveStateToWire(EAssetSaveState State)
{
    switch (State)
    {
    case EAssetSaveState::NotRequested:   return TEXT("notRequested");
    case EAssetSaveState::Written:        return TEXT("written");
    case EAssetSaveState::AlreadyCurrent: return TEXT("alreadyCurrent");
    case EAssetSaveState::Deferred:       return TEXT("deferred");
    case EAssetSaveState::Failed:         return TEXT("failed");
    case EAssetSaveState::BlockedByPie:   return TEXT("blockedByPie");
    case EAssetSaveState::NotPersistable: return TEXT("notPersistable");
    case EAssetSaveState::DiskStateDiverged: return TEXT("diskStateDiverged");
    }
    // An enumerator added without updating this switch must not read as durable.
    return TEXT("failed");
}

const TCHAR* AssetSaveStateDetail(EAssetSaveState State)
{
    switch (State)
    {
    case EAssetSaveState::NotRequested:
        return TEXT("No save was requested; the edit is in memory only. ")
               TEXT("Run editor.save_all, or asset.save on this path, to make it durable.");
    case EAssetSaveState::Written:
        return TEXT("This call wrote the .uasset; the asset is durable on disk.");
    case EAssetSaveState::AlreadyCurrent:
        return TEXT("Nothing needed writing; the .uasset on disk already matches memory.");
    case EAssetSaveState::Deferred:
        return TEXT("The edit is in memory only and the package is still dirty: the 0.5s ")
               TEXT("per-asset save throttle skipped this write, and nothing else is in the ")
               TEXT("way. Run editor.save_all, or asset.save on this path with force:true to ")
               TEXT("bypass the throttle, to make it durable. This is the ONE not-durable ")
               TEXT("state a retry fixes right now.");
    case EAssetSaveState::Failed:
        return TEXT("The save was attempted and produced no durable revision; a flush will not ")
               TEXT("help until the cause is cleared. The editor log carries a ")
               TEXT("SaveAssetToDiskReportingPresence line with the outcome, sizes and timestamps.");
    case EAssetSaveState::BlockedByPie:
        return TEXT("Nothing was written: the editor is in play mode, and it refuses every ")
               TEXT("single-asset save while a PIE session is running. Retrying cannot help ")
               TEXT("and neither can force:true or editor.save_all - all three hit the same ")
               TEXT("refusal. The edit is intact in memory: wait for the session to end, then ")
               TEXT("re-issue the save and confirm saveState:\"written\". The response's ")
               TEXT("pieWorlds names the running session, which in a shared editor is often ")
               TEXT("another agent's; editor.pie_status polls it, and editor.stop ends it.");
    case EAssetSaveState::NotPersistable:
        return TEXT("This package is transient or unmounted, so it can never be written and no ")
               TEXT("flush will ever persist it. Recreate the asset under a mounted content path.");
    case EAssetSaveState::DiskStateDiverged:
        return TEXT("Nothing was written: the .uasset on disk changed since this package was ")
               TEXT("loaded or last saved, so saving would have discarded it. A flush repeats ")
               TEXT("the refusal. Re-read the file with asset.reload and redo the edit, or ")
               TEXT("re-issue the save with overwriteDiskChanges:true to discard the on-disk ")
               TEXT("revision deliberately. The response's diskState block carries both hashes.");
    }
    return TEXT("The save state is unrecognised; treat the asset as not durable.");
}

bool IsAssetSaveStateDurable(EAssetSaveState State)
{
    // The ONE definition of durable. `saved` on the wire is this predicate, and
    // SaveAssetToDiskReportingPresence's bool return is required to equal it.
    return State == EAssetSaveState::Written || State == EAssetSaveState::AlreadyCurrent;
}

bool ShouldTreatAssetSaveAsSuccess(bool bSaveReportedSuccess, bool bFileExistsOnDisk)
{
    // An asset-save RPC that promises disk persistence must only report
    // saved:true when the .uasset is actually on disk. The shared mark-dirty
    // helper McpSafeAssetSave writes nothing (deferred to dodge the
    // bulkdata-corruption vector), so the report cannot trust a save-attempted
    // signal alone — the on-disk file is the only honest persistence evidence.
    return bSaveReportedSuccess && bFileExistsOnDisk;
}

bool SaveAssetToDiskReportingPresence(UObject* Asset, bool bForce,
                                      FString* OutPackageName, int64* OutSizeBytes,
                                      EAssetSaveState* OutState,
                                      bool bAllowDivergedOverwrite)
{
    if (OutPackageName) OutPackageName->Empty();
    if (OutSizeBytes)   *OutSizeBytes = 0;
    // Failed, not NotRequested: reaching this function IS the request. NotRequested is set by
    // the caller's own no-save branch, never here.
    if (OutState)       *OutState = EAssetSaveState::Failed;

    if (!Asset)
    {
        // Every early return below used to be a silent `return false`, which is how a
        // saved:false could arrive with nothing at all in the log to explain it.
        UE_LOG(LogPinWrightSubsystem, Warning,
               TEXT("SaveAssetToDiskReportingPresence: null asset, nothing to save."));
        return false;
    }

    UPackage* Package = Asset->GetOutermost();
    if (!Package)
    {
        UE_LOG(LogPinWrightSubsystem, Warning,
               TEXT("SaveAssetToDiskReportingPresence: '%s' has no outermost package, nothing to save."),
               *Asset->GetName());
        return false;
    }

    const FString PackageName = Package->GetName();
    if (OutPackageName) *OutPackageName = PackageName;

    // TryConvert, not LongPackageNameToFilename: the latter is UE_LOG(Fatal) when the
    // package has no registered mount root, so a transient/unmounted asset used to be
    // an editor kill rather than a false report.
    FString PackageFilename;
    const bool bHasFilename = FPackageName::TryConvertLongPackageNameToFilename(
        PackageName, PackageFilename, FPackageName::GetAssetPackageExtension());
    if (!bHasFilename)
    {
        // No registered mount root: there is no filename to write and no flush that could ever
        // produce one. NotPersistable rather than Failed, because the remedy is a different
        // package, not a retry.
        if (OutState) *OutState = EAssetSaveState::NotPersistable;
        UE_LOG(LogPinWrightSubsystem, Warning,
               TEXT("SaveAssetToDiskReportingPresence: package '%s' has no registered mount root, "
                    "so it can never be written to disk."),
               *PackageName);
        return false;
    }

    // Freshness baseline, mirroring McpSafeLevelSave's (see the bFreshWrite block in
    // this file). The post-save probe below only asks whether a .uasset exists, and a
    // leftover file from an earlier save satisfies that — which is precisely how the
    // throttle skip produced saved:true for an edit that never left memory
    // (existence, not freshness). Capture the pre-save state so the check can prove
    // THIS save wrote.
    IFileManager& FileManager = IFileManager::Get();
    const int64 SizeBefore = FileManager.FileSize(*PackageFilename);
    const bool bExistedBefore = SizeBefore >= 0;
    const FDateTime TimeStampBefore =
        bExistedBefore ? FileManager.GetTimeStamp(*PackageFilename) : FDateTime::MinValue();
    const bool bPackageDirtyBefore = Package->IsDirty();

    const ESaveLoadedAssetOutcome Outcome =
        SaveLoadedAssetThrottled(Asset, /*ThrottleSecondsOverride=*/-1.0, bForce,
                                 bAllowDivergedOverwrite);
    const bool bSaveReported = WasSavePersisted(Outcome);

    const int64 RawSize = FileManager.FileSize(*PackageFilename);
    const bool bFileExistsOnDisk = RawSize >= 0;
    if (OutSizeBytes) *OutSizeBytes = bFileExistsOnDisk ? RawSize : 0;

    // Freshness is only evidence when there was something to write: a clean package
    // legitimately rewrites byte-identical content (and the throttle may skip it
    // entirely), so requiring a changed stamp there would fail an honest save — the
    // same disease in the other direction. Identical guard shape to McpSafeLevelSave.
    //
    // Split into named probes only so the verdict can be logged as VALUES: the old warning
    // asserted "stamp+size unchanged" without printing either, so nobody could tell which of
    // the two probes disagreed. The expression is otherwise unchanged.
    const FDateTime TimeStampAfter = FileManager.GetTimeStamp(*PackageFilename);
    const bool bStampMoved = TimeStampAfter != TimeStampBefore || RawSize != SizeBefore;
    const bool bFreshWrite = !bExistedBefore || !bPackageDirtyBefore || bStampMoved;

    // Which of the six caller-facing states this is. Every ESaveLoadedAssetOutcome and every
    // freshness branch lands on exactly one, so a caller can act instead of guessing.
    EAssetSaveState State = EAssetSaveState::Failed;
    switch (Outcome)
    {
    case ESaveLoadedAssetOutcome::NotPersistable:
        State = EAssetSaveState::NotPersistable;
        break;

    case ESaveLoadedAssetOutcome::SkippedThrottledDirty:
        // The edit is intact in memory and the package is still dirty, so the next flush
        // writes it. This is the one not-durable answer a plain editor.save_all fixes.
        // Unreachable when bForce is set - SaveLoadedAssetThrottled skips the whole throttle
        // branch then - which is why the throttler cannot produce a saved:false on any forced
        // path, model.compile's included.
        State = EAssetSaveState::Deferred;
        break;

    case ESaveLoadedAssetOutcome::Failed:
        State = EAssetSaveState::Failed;
        break;

    case ESaveLoadedAssetOutcome::RefusedDiskStateDiverged:
        // Nothing was attempted, so none of the freshness probes below apply: the file is
        // exactly what the out-of-band writer left, and the caller's edit is intact in memory.
        State = EAssetSaveState::DiskStateDiverged;
        break;

    case ESaveLoadedAssetOutcome::RefusedBlockedByPie:
        // Same shape as the divergence refusal: nothing was attempted, the file is untouched
        // and the edit is intact in memory. Kept apart from Failed because the remedy is "wait
        // for PIE to end", and apart from Deferred because a flush repeats the refusal.
        State = EAssetSaveState::BlockedByPie;
        break;

    case ESaveLoadedAssetOutcome::Saved:
    case ESaveLoadedAssetOutcome::SkippedAlreadyClean:
        if (!bFileExistsOnDisk)
        {
            // The engine reported a persisted state and there is no file. NOT Deferred: a
            // reported save clears the dirty flag, so a flush finds nothing to write and the
            // caller would loop on a no-op.
            State = EAssetSaveState::Failed;
        }
        else if (!bExistedBefore || bStampMoved)
        {
            State = EAssetSaveState::Written;
        }
        else if (!bPackageDirtyBefore)
        {
            // Nothing was owed: the package held no unsaved changes going in and the file is
            // present, so the on-disk revision already equals memory. Distinct from Written
            // because no bytes moved.
            State = EAssetSaveState::AlreadyCurrent;
        }
        else
        {
            // Dirty going in, engine said saved, file did not move. Failed, not Deferred, for
            // the same reason as the no-file branch above.
            State = EAssetSaveState::Failed;
        }
        break;
    }

    const bool bDurable = bFreshWrite && ShouldTreatAssetSaveAsSuccess(bSaveReported, bFileExistsOnDisk);

    // Postcondition promised in the header, enforced rather than trusted: the bool return and
    // the state are two routes to one answer and must never disagree.
    ensureMsgf(IsAssetSaveStateDurable(State) == bDurable,
        TEXT("SaveAssetToDiskReportingPresence: state '%s' and durable=%d disagree for '%s'"),
        AssetSaveStateToWire(State), bDurable ? 1 : 0, *PackageName);

    if (!bDurable)
    {
        // Values, not adjectives. Without the numbers a false saved:false (the file did move,
        // the probe did not see it) is indistinguishable from a genuinely failed write - which
        // is exactly what made model.compile's intermittent saved:false undiagnosable. Print
        // every probe the verdict was computed from, including which engine branch ran.
        UE_LOG(LogPinWrightSubsystem, Warning,
               TEXT("SaveAssetToDiskReportingPresence: '%s' is NOT durable after this save. ")
               TEXT("state=%s outcome=%s forced=%s dirtyBefore=%s existedBefore=%s ")
               TEXT("sizeBefore=%lld sizeAfter=%lld stampBefore=%s stampAfter=%s file='%s'. %s"),
               *PackageName,
               AssetSaveStateToWire(State),
               AssetUtils_SaveOutcomeName(Outcome),
               bForce ? TEXT("true") : TEXT("false"),
               bPackageDirtyBefore ? TEXT("true") : TEXT("false"),
               bExistedBefore ? TEXT("true") : TEXT("false"),
               SizeBefore, RawSize,
               *TimeStampBefore.ToIso8601(), *TimeStampAfter.ToIso8601(),
               *PackageFilename,
               AssetSaveStateDetail(State));
    }

    if (OutState) *OutState = State;
    return bDurable;
}

void AddAssetSaveReport(const TSharedPtr<FJsonObject>& Result, bool bSaveRequested, bool bSavedToDisk,
                        const TOptional<EAssetSaveState>& State)
{
    if (!Result)
    {
        return;
    }
    Result->SetBoolField(TEXT("saveRequested"), bSaveRequested);
    Result->SetBoolField(TEXT("saved"), bSaveRequested && bSavedToDisk);
    if (bSaveRequested && !bSavedToDisk)
    {
        // A PIE refusal is not queued work: neither a flush nor force:true can write until the
        // play session ends. Keep the legacy requested-and-not-durable meaning for unthreaded
        // and other states, but make the known hard block explicit instead of retryable-looking.
        const bool bBlockedByPie = State.IsSet()
            && State.GetValue() == EAssetSaveState::BlockedByPie;
        Result->SetBoolField(TEXT("pendingFlush"), !bBlockedByPie);

        // Name the one blocker that is knowable without a threaded state, and that a caller
        // cannot deduce from its own actions because it belongs to whoever started PIE. Adds
        // nothing when the editor is not in play mode, so the legacy payload is unchanged.
        PinWrightPieSaveBlock::AddPieSaveBlockJsonIfBlocked(Result);
    }

    if (!State.IsSet())
    {
        return;
    }

    // saveState / saveDetail are additive for every state except BlockedByPie, whose
    // pendingFlush:false is part of its refuse-until-PIE-ends contract. Call sites that pass no
    // state keep the legacy response shape.
    const EAssetSaveState Reported = State.GetValue();
    if (bSaveRequested && IsAssetSaveStateDurable(Reported) != bSavedToDisk)
    {
        // Two measurements that disagree; report both as measured and make the drift loud
        // rather than picking one and hiding it.
        UE_LOG(LogPinWrightSubsystem, Warning,
               TEXT("AddAssetSaveReport: saveState '%s' (durable=%s) disagrees with savedToDisk=%s."),
               AssetSaveStateToWire(Reported),
               IsAssetSaveStateDurable(Reported) ? TEXT("true") : TEXT("false"),
               bSavedToDisk ? TEXT("true") : TEXT("false"));
    }

    Result->SetStringField(TEXT("saveState"), AssetSaveStateToWire(Reported));
    Result->SetStringField(TEXT("saveDetail"), AssetSaveStateDetail(Reported));
}

bool AddPieSaveRefusalReport(const TSharedPtr<FJsonObject>& Result, FString& OutDescription)
{
    PinWrightPieSaveBlock::FPieSaveBlock Block;
    if (!PinWrightPieSaveBlock::ProbePieSaveBlock(Block))
    {
        return false;
    }

    // Routed through the shared emitter rather than hand-written, so a refusal raised before the
    // work and a report raised after the write carry one shape. It re-probes and publishes the
    // pieActive / editorMode / pieWorlds block itself; the measurement here exists for the
    // message, and the two cannot disagree on one game-thread frame.
    AddAssetSaveReport(Result, /*bSaveRequested=*/true, /*bSavedToDisk=*/false,
                       EAssetSaveState::BlockedByPie);

    OutDescription = PinWrightPieSaveBlock::DescribePieSaveBlock(Block);
    return true;
}

bool McpSafeLevelSave(ULevel* Level, const FString& FullPath, int32 MaxRetries)
{
    if (!Level)
    {
        UE_LOG(LogTemp, Error, TEXT("McpSafeLevelSave: Level is null"));
        return false;
    }

    FString PackagePath = FullPath;
    if (!IsValidMountPoint(PackagePath))
    {
        // `TEXT("/Game") / P`, not `TEXT("/Game/") + P`: string concatenation MANUFACTURES a "//"
        // whenever P already carries a leading slash, and that byte sequence is what CreatePackage
        // logs Fatal on. FString::operator/ routes through PathAppend, which absorbs the duplicate
        // separator, so both "Maps/L_X" and "/Maps/L_X" compose to "/Game/Maps/L_X".
        //
        // This was latent while IsValidMountPoint's StartsWith short-circuits let a leading-slash
        // path through; a stricter IsValidMountPoint makes the prepend fire on inputs it never
        // fired on before, which is exactly when the old spelling would have started killing the
        // editor.
        PackagePath = TEXT("/Game") / PackagePath;
    }

    // Ensure path has proper format
    if (PackagePath.Contains(TEXT(".")))
    {
        PackagePath = PackagePath.Left(PackagePath.Find(TEXT(".")));
    }

    // SaveLevel's DefaultFilename is a FILESYSTEM path: the engine forwards it as
    // ForceFilename and execs `OBJ SAVEPACKAGE ... FILE="<it>"`. A long package name
    // looks rooted to FPaths::IsRelative because of the leading '/', so Windows
    // resolved it against the current drive and the .umap landed extensionless at
    // C:\Game\Maps\... outside the project — and since that save genuinely succeeded,
    // the dirty flag cleared and nothing ever prompted
    // (B-level-save-package-path-as-filename).
    FString MapFilename;
    if (!ResolveLevelPackageToMapFilename(PackagePath, MapFilename))
    {
        UE_LOG(LogTemp, Error,
            TEXT("McpSafeLevelSave: Cannot resolve package path to a map filename (unregistered mount root?): %s"),
            *PackagePath);
        return false;
    }

    // A never-saved world's package is /Temp/Untitled_N. Before the conversion fix
    // that case simply failed; now it would resolve and silently persist under
    // Saved/Temp, where the content browser can never see it. Refuse instead, and
    // name the verb that writes to a real content path.
    if (PackagePath.StartsWith(TEXT("/Temp/")))
    {
        UE_LOG(LogTemp, Error,
            TEXT("McpSafeLevelSave: Refusing to save into the /Temp mount: %s. Saved/Temp is invisible to the content browser — use level.save_as with a /Game/... path."),
            *PackagePath);
        return false;
    }

    // Freshness baseline. The post-save probe only asks whether a .umap exists, so a
    // leftover file from an earlier save satisfies it and masks a save that wrote
    // nowhere; capture the pre-save state so the check can prove THIS save wrote
    // (B-level-save-verify-existence-not-freshness).
    IFileManager& FileManager = IFileManager::Get();
    const bool bExistedBefore = FileManager.FileExists(*MapFilename);
    const FDateTime TimeStampBefore =
        bExistedBefore ? FileManager.GetTimeStamp(*MapFilename) : FDateTime::MinValue();
    const int64 FileSizeBefore = bExistedBefore ? FileManager.FileSize(*MapFilename) : -1;
    const UPackage* OuterPackageBefore = Level->GetOutermost();
    const bool bPackageDirtyBefore = OuterPackageBefore && OuterPackageBefore->IsDirty();

    bool bSaveSucceeded = false;
    // Increased initial delay for Intel GPU driver stability (MONZA DdiThreadingContext)
    float DelayMs = 250.0f;

    for (int32 Retry = 0; Retry < MaxRetries; ++Retry)
    {
        // CRITICAL: Flush rendering commands to prevent Intel driver race condition
        FlushRenderingCommands();

        // Additional delay after flush to ensure GPU is completely idle
        FPlatformProcess::Sleep(0.050f); // 50ms additional wait

        // Perform the actual save after flushing render commands. OutSavedFilename
        // reports where the engine really wrote, which is the only way to catch a
        // redirected destination from inside this call.
        FString SavedFilename;
        bSaveSucceeded = FEditorFileUtils::SaveLevel(Level, MapFilename, &SavedFilename);

        if (bSaveSucceeded)
        {
            // Small delay before verification to allow file system to flush
            FPlatformProcess::Sleep(0.100f); // 100ms

            // Verify save outcome using disk and package-level checks.
            const bool bFileExistsOnDisk = FileManager.FileExists(*MapFilename);

            const bool bPackageExists = FPackageName::DoesPackageExist(PackagePath);
            const bool bAssetExists = ResolveAsset(PackagePath).bRegistryOrMemoryExists;
            bool bPackageClean = false;
            if (UPackage* LevelPackage = Level->GetOutermost())
            {
                bPackageClean =
                    !LevelPackage->IsDirty() &&
                    LevelPackage->GetName().Equals(PackagePath, ESearchCase::IgnoreCase);
            }

            // An engine build that leaves the out-param empty tells us nothing about
            // the destination, so treat unknown as "as requested" rather than
            // regressing every save on that version into a false failure.
            const bool bLandedWhereExpected =
                SavedFilename.IsEmpty() ||
                FPaths::IsSamePath(
                    FPaths::ConvertRelativePathToFull(SavedFilename),
                    FPaths::ConvertRelativePathToFull(MapFilename));

            // Freshness is only evidence when there was something to write: a clean
            // package legitimately rewrites byte-identical content, so requiring a
            // changed stamp there would fail an honest save.
            const bool bFreshWrite =
                !bExistedBefore ||
                FileManager.GetTimeStamp(*MapFilename) != TimeStampBefore ||
                FileManager.FileSize(*MapFilename) != FileSizeBefore ||
                !bPackageDirtyBefore;

            if (bLandedWhereExpected && bFreshWrite && ShouldTreatLevelSaveAsSuccess(
                    bSaveSucceeded, bFileExistsOnDisk, bPackageExists, bAssetExists, bPackageClean))
            {
                UE_LOG(LogTemp, Log,
                    TEXT("McpSafeLevelSave: Successfully saved level after %d attempt(s): %s (file=%s, savedFilename=%s, landed=%s, fresh=%s, disk=%s, package=%s, asset=%s, clean=%s)"),
                    Retry + 1, *PackagePath, *MapFilename, *SavedFilename,
                    bLandedWhereExpected ? TEXT("true") : TEXT("false"),
                    bFreshWrite ? TEXT("true") : TEXT("false"),
                    bFileExistsOnDisk ? TEXT("true") : TEXT("false"),
                    bPackageExists ? TEXT("true") : TEXT("false"),
                    bAssetExists ? TEXT("true") : TEXT("false"),
                    bPackageClean ? TEXT("true") : TEXT("false"));
                return true;
            }

            UE_LOG(LogTemp, Warning,
                TEXT("McpSafeLevelSave: Save reported success but verification failed (attempt %d/%d): package=%s file=%s savedFilename=%s landed=%s fresh=%s disk=%s packageExists=%s assetExists=%s packageClean=%s"),
                Retry + 1, MaxRetries, *PackagePath, *MapFilename, *SavedFilename,
                bLandedWhereExpected ? TEXT("true") : TEXT("false"),
                bFreshWrite ? TEXT("true") : TEXT("false"),
                bFileExistsOnDisk ? TEXT("true") : TEXT("false"),
                bPackageExists ? TEXT("true") : TEXT("false"),
                bAssetExists ? TEXT("true") : TEXT("false"),
                bPackageClean ? TEXT("true") : TEXT("false"));
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("McpSafeLevelSave: Save attempt %d/%d failed for: %s"),
                Retry + 1, MaxRetries, *PackagePath);
        }

        // Exponential backoff before retry (250ms -> 500ms -> 1000ms -> 2000ms -> 4000ms)
        if (Retry < MaxRetries - 1)
        {
            FPlatformProcess::Sleep(DelayMs / 1000.0f);
            DelayMs *= 2.0f;
        }
    }

    UE_LOG(LogTemp, Error, TEXT("McpSafeLevelSave: All %d save attempts failed for: %s"), MaxRetries, *PackagePath);
    return false;
}

UMaterialInterface* McpLoadMaterialWithFallback(
    const FString& MaterialPath,
    bool bSilent)
{
    // Try requested path first if provided
    if (!MaterialPath.IsEmpty())
    {
        UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
        if (Material)
        {
            return Material;
        }
        if (!bSilent)
        {
            UE_LOG(LogTemp, Warning, TEXT("McpLoadMaterialWithFallback: Requested material not found: %s"), *MaterialPath);
        }
    }

    // Fallback chain for engine materials (order matters - most common first)
    const TCHAR* FallbackPaths[] = {
        TEXT("/Engine/EngineMaterials/DefaultMaterial"),
        TEXT("/Engine/EngineMaterials/WorldGridMaterial"),
        TEXT("/Engine/EngineMaterials/DefaultDeferredDecalMaterial"),
        TEXT("/Engine/EngineMaterials/DefaultTextMaterialOpaque")
    };

    for (const TCHAR* FallbackPath : FallbackPaths)
    {
        UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, FallbackPath);
        if (Material)
        {
            if (!bSilent && !MaterialPath.IsEmpty())
            {
                UE_LOG(LogTemp, Log, TEXT("McpLoadMaterialWithFallback: Using fallback '%s' for '%s'"),
                    FallbackPath, *MaterialPath);
            }
            return Material;
        }
    }

    UE_LOG(LogTemp, Error, TEXT("McpLoadMaterialWithFallback: All fallback materials unavailable - engine content may be missing"));
    return nullptr;
}

bool WasSavePersisted(ESaveLoadedAssetOutcome Outcome)
{
    return Outcome == ESaveLoadedAssetOutcome::Saved
        || Outcome == ESaveLoadedAssetOutcome::SkippedAlreadyClean;
}

ESaveLoadedAssetOutcome SaveLoadedAssetThrottled(UObject* Asset, double ThrottleSecondsOverride,
                              bool bForce, bool bAllowDivergedOverwrite)
{
    if (!Asset)
        return ESaveLoadedAssetOutcome::Failed;

    UPackage* Package = Asset->GetOutermost();
    if (!Package || Package == GetTransientPackage() || Package->HasAnyFlags(RF_Transient))
    {
        // A transient asset can never reach disk. This used to return true, which is
        // how create verbs that land their asset in the transient package reported
        // saved:true for something that is discarded at editor shutdown.
        return ESaveLoadedAssetOutcome::NotPersistable;
    }

    FString Key = Asset->GetPathName();
    if (Key.IsEmpty())
        Key = Asset->GetName();

    // The PIE gate, ahead of the throttle on purpose (board
    // B-asset-save-pie-failure-reports-pendingflush). UEditorAssetLibrary::SaveLoadedAsset
    // refuses unconditionally while a play session is up, so a write that would be attempted
    // here cannot land - and a throttle skip reported first would tell the caller to flush,
    // which hits the same refusal. Reporting the cause the engine already knows, instead of the
    // channel we happened to notice it through, is the whole fix.
    //
    // Gated on there being a write to refuse: a clean, unforced save has nothing for PIE to
    // block, and the on-disk revision already matches memory, so it keeps its existing
    // already-clean verdict rather than being turned into a false refusal.
    if (Package->IsDirty() || bForce)
    {
        PinWrightPieSaveBlock::FPieSaveBlock PieBlock;
        if (PinWrightPieSaveBlock::ProbePieSaveBlock(PieBlock))
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("SaveLoadedAssetThrottled: refusing to save '%s' — %s. The edit is intact "
                     "in memory; re-issue the save once the session ends (force:true and "
                     "editor.save_all are refused identically)."),
                *Key, *PinWrightPieSaveBlock::DescribePieSaveBlock(PieBlock));
            return ESaveLoadedAssetOutcome::RefusedBlockedByPie;
        }
    }

    FSaveThrottler& Throttler = FPluginState::Get().SaveThrottle();

    // When not forced, check whether we should throttle
    if (!bForce)
    {
        const double Throttle = (ThrottleSecondsOverride >= 0.0)
                                    ? ThrottleSecondsOverride
                                    : Throttler.ThrottleSecondsRef();

        const double Elapsed = Throttler.GetElapsedSinceLastSave(Key);
        if (Elapsed >= 0.0 && Elapsed < Throttle)
        {
            // The skip itself is fine; claiming success for it is not. A dirty package
            // means the caller's edit is still only in memory, and the throttle window
            // (0.5s, State/SaveThrottler.h) is well inside the rate an agent issues
            // sequential edits to one asset. Splitting the two cases is what makes the
            // downstream disk probe able to see this failure at all: existence alone
            // cannot, because the file from the previous save is right there.
            const bool bDirty = Package->IsDirty();
            if (bDirty)
            {
                UE_LOG(LogPinWrightSubsystem, Warning,
                       TEXT("SaveLoadedAssetThrottled: skipping save for '%s' while the package is "
                            "DIRTY (last=%.3fs, throttle=%.3fs) — the edit is not on disk; the "
                            "caller must report saved:false / pendingFlush:true"),
                       *Key, Elapsed, Throttle);
                return ESaveLoadedAssetOutcome::SkippedThrottledDirty;
            }
            UE_LOG(LogPinWrightSubsystem, VeryVerbose,
                   TEXT("SaveLoadedAssetThrottled: skipping save for '%s' "
                        "(last=%.3fs, throttle=%.3fs, package already clean)"),
                   *Key, Elapsed, Throttle);
            return ESaveLoadedAssetOutcome::SkippedAlreadyClean;
        }
    }

    if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
    {
        TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure> IntegrityFailures;
        if (!BlueprintHandlerUtils::ValidateBlueprintGraphIntegrity(Blueprint, IntegrityFailures))
        {
            const FString Reason = IntegrityFailures.Num() > 0
                ? IntegrityFailures[0].Reason
                : FString(TEXT("unknown integrity failure"));
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("SaveLoadedAssetThrottled: refusing to save '%s' due to Blueprint integrity failure: %s"),
                *Key,
                *Reason);
            return ESaveLoadedAssetOutcome::Failed;
        }
    }

    // The out-of-band-overwrite guard, applied here because this is the single place the plugin
    // reaches UEditorAssetLibrary::SaveLoadedAsset — one guard for every single-asset write site
    // instead of a copy per verb (board B-asset-save-clobbers-out-of-band-change).
    //
    // Placed after the throttle (a skipped save writes nothing and need not pay a file read) and
    // after the in-memory integrity gate (cheaper, and a corrupt Blueprint keeps reporting the
    // fault it always reported). It runs regardless of the dirty flag: a forced save writes with
    // bOnlyIfIsDirty=false and would overwrite the file even from a clean package.
    if (!bAllowDivergedOverwrite)
    {
        PinWrightPackageDiskState::FPackageDiskDivergence DiskState;
        if (PinWrightPackageDiskState::ProbePackageDiskDivergence(Package, DiskState))
        {
            // Short on purpose: ProbePackageDiskDivergence has already logged both hashes, the
            // size and the timestamp at Warning. This line only adds the refusing call site and
            // the asset path, which that one does not carry (it names the package).
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("SaveLoadedAssetThrottled: refusing to save '%s' — the .uasset changed on "
                     "disk since this package read it (see the preceding disk-state line)."),
                *Key);
            return ESaveLoadedAssetOutcome::RefusedDiskStateDiverged;
        }
    }

    // Perform the save and record timestamp on success. A forced save writes
    // unconditionally (bOnlyIfIsDirty=false): create-and-save flows (e.g.
    // audio.authoring.create_metasound_patch/_preset) call FactoryCreateNew, which
    // does NOT leave the new package dirty, so the default bOnlyIfIsDirty=true would
    // make SaveLoadedAsset report "All files are already saved" and write nothing,
    // leaving the asset in memory only (B-metasound-create-save-no-disk-write).
    // Non-forced edit-saves keep bOnlyIfIsDirty=true so an already-clean asset isn't
    // needlessly rewritten.
    const bool bWasDirty = Package->IsDirty();
    const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Asset, /*bOnlyIfIsDirty=*/!bForce);
    if (!bSaved)
    {
        UE_LOG(LogPinWrightSubsystem, Warning,
               TEXT("SaveLoadedAssetThrottled: failed to save '%s'"), *Key);
        return ESaveLoadedAssetOutcome::Failed;
    }

    Throttler.RecordSave(Key);
    UE_LOG(LogPinWrightSubsystem, VeryVerbose,
           TEXT("SaveLoadedAssetThrottled: saved '%s' (throttle reset)"), *Key);

    // A non-forced SaveLoadedAsset on an already-clean package reports success and
    // writes nothing ("All files are already saved" — see the bForce comment above).
    // That is a real outcome, not a failure, but it is not this call writing, so name
    // it accurately: SkippedAlreadyClean, which SaveAssetToDiskReportingPresence then
    // gates on the .uasset actually being present.
    if (!bForce && !bWasDirty)
    {
        return ESaveLoadedAssetOutcome::SkippedAlreadyClean;
    }
    return ESaveLoadedAssetOutcome::Saved;
}

void ScanPathSynchronous(const FString& InPath, bool bRecursive)
{
    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    TArray<FString> PathsToScan;
    PathsToScan.Add(InPath);
    AssetRegistry.ScanPathsSynchronous(PathsToScan, bRecursive);
}

// ============================================================================
// Blueprint Helpers
// ============================================================================

UBlueprint* LoadBlueprintAsset(const FString& Req,
                               FString& OutNormalized,
                               FString& OutError)
{
    OutNormalized.Empty();
    OutError.Empty();
    if (Req.IsEmpty())
    {
        OutError = TEXT("Empty request");
        return nullptr;
    }

    // Method 4 below hands a caller-derived string straight to LoadObject, which reaches
    // CreatePackage's Fatal for a name containing "//" (UObjectGlobals.cpp:1094-1096) - process
    // death, not an error return, so the `if (BP)` after it is never reached.
    //
    // Tested on Req, the RAW argument, NOT on the post-prepend `Path` below: the "/Game/" prepend
    // fires only when Req has no leading slash, so it can neither introduce a "//" that Req lacked
    // nor hide one that Req carried. Checking Req keeps the refusal message quoting what the
    // caller actually sent.
    //
    // Distinct wording from the "Blueprint asset not found" miss at the bottom of this function, on
    // purpose: this is a malformed argument, not an absent asset, and support needs to grep them
    // apart across ~53 call sites.
    if (CanReachCreatePackageFatal(Req))
    {
        OutError = FString::Printf(
            TEXT("Blueprint path '%s' contains '//' and was refused: a doubled slash can reach "
                 "CreatePackage's Fatal and end the editor process."), *Req);
        return nullptr;
    }

    // Build normalized paths
    FString Path = Req;
    if (!Path.StartsWith(TEXT("/")))
    {
        Path = TEXT("/Game/") + Path;
    }

    FString ObjectPath = Path;
    FString PackagePath = Path;

    if (Path.Contains(TEXT(".")))
    {
        PackagePath = Path.Left(Path.Find(TEXT(".")));
    }
    else
    {
        FString AssetName = FPaths::GetBaseFilename(Path);
        ObjectPath = Path + TEXT(".") + AssetName;
    }

    FString AssetName = FPaths::GetBaseFilename(PackagePath);

    // Method 1: FindObject with full object path (fastest for in-memory)
    if (UBlueprint* BP = FindObject<UBlueprint>(nullptr, *ObjectPath))
    {
        OutNormalized = PackagePath;
        return BP;
    }

    // Method 2: Find package first, then find asset within it
    if (UPackage* Package = FindPackage(nullptr, *PackagePath))
    {
        if (UBlueprint* BP = FindObject<UBlueprint>(Package, *AssetName))
        {
            OutNormalized = PackagePath;
            return BP;
        }
    }

    // Method 3: TObjectIterator fallback
    for (TObjectIterator<UBlueprint> It; It; ++It)
    {
        UBlueprint* BP = *It;
        if (BP)
        {
            FString BPPath = BP->GetPathName();
            if (BPPath.Equals(ObjectPath, ESearchCase::IgnoreCase) ||
                BPPath.Equals(PackagePath, ESearchCase::IgnoreCase) ||
                BPPath.Equals(Path, ESearchCase::IgnoreCase) ||
                BPPath.Equals(Req, ESearchCase::IgnoreCase))
            {
                OutNormalized = PackagePath;
                return BP;
            }
            FString BPPackagePath = BPPath;
            if (BPPackagePath.Contains(TEXT(".")))
            {
                BPPackagePath = BPPackagePath.Left(BPPackagePath.Find(TEXT(".")));
            }
            if (BPPackagePath.Equals(PackagePath, ESearchCase::IgnoreCase))
            {
                OutNormalized = PackagePath;
                return BP;
            }
        }
    }

    // Method 4: shared path-safe, PIE-safe asset resolution.
    if (UBlueprint* BP = Cast<UBlueprint>(
            ResolveAsset(ObjectPath, /*bLoadObject=*/true).Object))
    {
        OutNormalized = PackagePath;
        return BP;
    }

    // Method 5: Asset Registry lookup
    FAssetRegistryModule& ARM =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    FAssetData Found;
    TArray<FAssetData> Results;
    ARM.Get().GetAssetsByPackageName(FName(*PackagePath), Results);
    if (Results.Num() > 0)
    {
        Found = Results[0];
    }

    if (Found.IsValid())
    {
        UBlueprint* BP = Cast<UBlueprint>(Found.GetAsset());
        if (!BP)
        {
            const FString PathStr = Found.ToSoftObjectPath().ToString();
            BP = LoadObject<UBlueprint>(nullptr, *PathStr);
        }
        if (BP)
        {
            OutNormalized = Found.ToSoftObjectPath().ToString();
            if (OutNormalized.Contains(TEXT(".")))
                OutNormalized = OutNormalized.Left(OutNormalized.Find(TEXT(".")));
            return BP;
        }
    }

    OutError = FString::Printf(TEXT("Blueprint asset not found: %s"), *Req);
    return nullptr;
}

bool FindBlueprintNormalizedPath(const FString& Req, FString& OutNormalized)
{
    OutNormalized.Empty();
    if (Req.IsEmpty())
        return false;

    // Use lightweight existence check - DO NOT use LoadBlueprintAsset here
    // as it causes Editor hangs when called repeatedly in polling loops
    FString CheckPath = Req;

    // Ensure path starts with /Game if it doesn't have a valid root
    if (!IsValidMountPoint(CheckPath))
    {
        if (CheckPath.StartsWith(TEXT("/")))
        {
            CheckPath = TEXT("/Game") + CheckPath;
        }
        else
        {
            CheckPath = TEXT("/Game/") + CheckPath;
        }
    }

    // Remove .uasset extension if present
    if (CheckPath.EndsWith(TEXT(".uasset")))
    {
        CheckPath = CheckPath.LeftChop(7);
    }

    // Remove object path suffix (e.g., /Game/BP.BP -> /Game/BP)
    int32 DotIdx;
    if (CheckPath.FindLastChar(TEXT('.'), DotIdx))
    {
        // Check if this looks like an object path (PackagePath.ObjectName)
        FString AfterDot = CheckPath.Mid(DotIdx + 1);
        FString BeforeDot = CheckPath.Left(DotIdx);
        int32 LastSlashIdx;
        if (BeforeDot.FindLastChar(TEXT('/'), LastSlashIdx))
        {
            FString LocalAssetName = BeforeDot.Mid(LastSlashIdx + 1);
            if (LocalAssetName.Equals(AfterDot, ESearchCase::IgnoreCase))
            {
                CheckPath = BeforeDot;
            }
        }
    }

    if (ResolveAsset(CheckPath).bExists)
    {
        OutNormalized = CheckPath;
        return true;
    }
    return false;
}

// Refuse a package target CreatePackage would die on, before it is handed over.
//
// CreatePackage (UObjectGlobals.cpp:1087-1120) logs at Fatal — a verbosity that is not compiled
// out in any configuration — for a package name containing "//" (:1094-1096) and for one that
// resolves to empty (:1118). Fatal ends the PROCESS, so an unvalidated string reaching it does
// not fail the call: it kills the editor and every unsaved package in it. The `if (!Package)`
// check after each call below can never fire, because nothing after the call is reached. The
// only place to stop it is here, at the argument. Measured mechanism: board
// B-foliage-add-type-name-with-slash-kills-the-editor; this file's two sites came from the sweep
// on B-createpackage-unvalidated-paths-plugin-wide.
//
// Both helpers below are SHARED, so the guard lives inside them rather than at their callers:
// every caller then inherits it, and each keeps its own error-code vocabulary because this
// reports through OutError instead of answering the request itself.
//
// This is the Utils-layer counterpart of PinWrightComposeAssetPackagePath
// (Handlers/PackagePathCompose.h) and applies the same two engine rules, but it is deliberately
// not that helper: that one COMPOSES "<folder>/<name>", while both callers here already hold a
// finished package path (PrepareBlueprintPackageGuardingNameCollision is handed one and its
// AssetName is a separate argument; McpCreateControlRigBlueprint normalizes its folder first and
// composes with FString::operator/). What is needed is a check on a finished path plus its bare
// name, not a second composition — and pulling a Handlers/ header down into Utils/ for a half-fit
// is the wrong direction to boot.
//
// The engine's own rules answer both halves:
//   * FName::IsValidXName + INVALID_OBJECTNAME_CHARACTERS (which contains '/', '.' and ':') is
//     what UObject naming itself rejects, so a path-shaped name is refused as a name.
//   * FPackageName::IsValidLongPackageName is what CreatePackage's input must satisfy: it rejects
//     "//", a missing leading slash, a trailing slash, a too-short name,
//     INVALID_LONGPACKAGE_CHARACTERS (where '\' is caught) and an unmounted root.
// Both engine reason texts are surfaced verbatim, so a refused caller is told which rule it broke.
//
// Distinctively named because this module builds with Unity on: a short static helper name would
// collide with another translation unit's once the .cpp files are merged.
static bool AssetUtils_ValidatePackageTargetForCreatePackage(
    const FString& PackagePath,
    const FString& AssetName,
    FString& OutError)
{
    FText Reason;
    if (!FName::IsValidXName(AssetName, INVALID_OBJECTNAME_CHARACTERS, &Reason))
    {
        OutError = FString::Printf(TEXT("'%s' is not a bare asset name: %s"),
                                   *AssetName, *Reason.ToString());
    }
    else if (!FPackageName::IsValidLongPackageName(PackagePath, /*bIncludeReadOnlyRoots=*/true,
                                                   &Reason))
    {
        OutError = FString::Printf(TEXT("'%s' is not a valid package path: %s"),
                                   *PackagePath, *Reason.ToString());
    }
    else
    {
        return true;
    }

    // Warning, not Error: a malformed argument is a refusal, not a plugin fault. Logged here
    // rather than left to the caller because a caller that drops OutError (the anim-blueprint
    // creator maps this failure to a fixed "Failed to create package" message) would otherwise
    // leave no record of which rule was broken.
    UE_LOG(LogPinWrightSubsystem, Warning,
           TEXT("Refused a CreatePackage target: %s"), *OutError);
    return false;
}

bool PrepareBlueprintPackageGuardingNameCollision(
    const FString& PackagePath,
    const FString& AssetName,
    UPackage*& OutPackage,
    FString& OutAssetObjectPath,
    bool& bOutNameCollision,
    FString& OutError)
{
    OutPackage = nullptr;
    bOutNameCollision = false;
    OutAssetObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);

    // Refuse a target CreatePackage would die on FIRST — above the existence probe below, so the
    // refusal is about the caller's argument rather than about what happens to be on disk, and so
    // no malformed string is handed to the asset-registry lookups either. bOutNameCollision stays
    // false: this is a malformed argument, not an existing asset.
    if (!AssetUtils_ValidatePackageTargetForCreatePackage(PackagePath, AssetName, OutError))
    {
        return false;
    }

    // Guard against a name collision before touching the raw factory. When a
    // UBlueprint of this name already exists (on disk or loaded in memory), the
    // factory's FactoryCreateNew -> FKismetEditorUtilities::CreateBlueprint fires
    // a fatal check(FindObject<UBlueprint>(Outer, ...) == 0) that crashes the editor.
    if (ResolveAsset(OutAssetObjectPath, /*bLoadObject=*/true).bExists)
    {
        bOutNameCollision = true;
        OutError = FString::Printf(TEXT("An asset already exists at: %s"), *OutAssetObjectPath);
        return false;
    }

    OutPackage = CreatePackage(*PackagePath);
    if (!OutPackage)
    {
        OutError = FString::Printf(TEXT("Failed to create package: %s"), *PackagePath);
        return false;
    }

    // Final guard against a duplicate object name within an existing in-memory
    // package (covers the case where the package is loaded but the asset-path
    // lookups above missed it) — same fatal CreateBlueprint check otherwise.
    if (FindObject<UBlueprint>(OutPackage, *AssetName) != nullptr)
    {
        OutPackage = nullptr;
        bOutNameCollision = true;
        OutError = FString::Printf(TEXT("An asset already exists at: %s"), *OutAssetObjectPath);
        return false;
    }

    return true;
}

// ============================================================================
// SCS Node Helpers
// ============================================================================

USCS_Node* FindScsNodeByName(USimpleConstructionScript* SCS, const FString& Name)
{
    if (!SCS || Name.IsEmpty())
        return nullptr;

    // Attempt to find an array property named "AllNodes" on the SCS
    if (UClass* SCSClass = SCS->GetClass())
    {
        if (FArrayProperty* ArrayProp =
                FindFProperty<FArrayProperty>(SCSClass, TEXT("AllNodes")))
        {
            FScriptArrayHelper Helper(ArrayProp,
                                      ArrayProp->ContainerPtrToValuePtr<void>(SCS));
            for (int32 Idx = 0; Idx < Helper.Num(); ++Idx)
            {
                void* ElemPtr = Helper.GetRawPtr(Idx);
                if (!ElemPtr)
                    continue;
                if (FObjectProperty* ObjProp =
                        CastField<FObjectProperty>(ArrayProp->Inner))
                {
                    UObject* ElemObj = ObjProp->GetObjectPropertyValue(ElemPtr);
                    if (!ElemObj)
                        continue;
                    // Match by the node's variable-name property. On UE5 the USCS_Node
                    // UPROPERTY is InternalVariableName (surfaced via GetVariableName());
                    // older engines named it VariableName. Try the modern name first so
                    // freshly-created, not-yet-compiled nodes (whose object name is an
                    // auto-generated "SCS_Node_N", not the variable name) still resolve.
                    FProperty* VarProp = ElemObj->GetClass()->FindPropertyByName(
                        TEXT("InternalVariableName"));
                    if (!VarProp)
                    {
                        VarProp = ElemObj->GetClass()->FindPropertyByName(
                            TEXT("VariableName"));
                    }
                    if (VarProp)
                    {
                        if (FNameProperty* NP = CastField<FNameProperty>(VarProp))
                        {
                            const FName V = NP->GetPropertyValue_InContainer(ElemObj);
                            if (!V.IsNone() &&
                                V.ToString().Equals(Name, ESearchCase::IgnoreCase))
                            {
                                return reinterpret_cast<USCS_Node*>(ElemObj);
                            }
                        }
                    }
                    // Fallback: match the object name
                    if (ElemObj->GetName().Equals(Name, ESearchCase::IgnoreCase))
                    {
                        return reinterpret_cast<USCS_Node*>(ElemObj);
                    }
                }
            }
        }
    }
    return nullptr;
}

// ============================================================================
// String Conversion Helpers
// ============================================================================

FString ConvertToString(const FString& In) { return In; }
FString ConvertToString(const FName& In) { return In.ToString(); }
FString ConvertToString(const FText& In) { return In.ToString(); }

// ============================================================================
// Verification Helpers
// ============================================================================

void AddActorVerification(TSharedPtr<FJsonObject> Response, AActor* Actor)
{
    if (!Response || !Actor) return;

    // actorPath is the actor's OBJECT path (e.g.
    // /Game/Maps/<Map>.<Map>:PersistentLevel.<ObjectName>) — the value that
    // uniquely identifies WHICH actor was mutated and that callers chain into
    // find_by_*/get/property.*. Matching the sibling AddChainableActorFields, the
    // spawn/duplicate responses, and the find_by_* verbs. The map PACKAGE path
    // (GetPackage()->GetPathName()) was the previous value but is shared by every
    // actor in the map, so it can't disambiguate colliding labels; it is now
    // surfaced separately under mapPath.
    Response->SetStringField(TEXT("actorPath"), Actor->GetPathName());
    if (UPackage* ActorPackage = Actor->GetPackage())
    {
        Response->SetStringField(TEXT("mapPath"), ActorPackage->GetPathName());
    }
    // Both identities, always. `actorName` is the DISPLAY LABEL and is kept for backward
    // compatibility, but a label is not unique, so a response carrying only that cannot be
    // fed back into a lookup reliably. `actorLabel` names the same value honestly, and
    // `actorObjectName` is the internal FName - unique within the level and the key the
    // resolver's deterministic tier matches on.
    Response->SetStringField(TEXT("actorName"), Actor->GetActorLabel());
    Response->SetStringField(TEXT("actorLabel"), Actor->GetActorLabel());
    Response->SetStringField(TEXT("actorObjectName"), Actor->GetName());
    Response->SetStringField(TEXT("actorGuid"), Actor->GetActorGuid().ToString());
    Response->SetBoolField(TEXT("existsAfter"), true);
    Response->SetStringField(TEXT("actorClass"), Actor->GetClass()->GetName());
}

void AddActorNameDeduplicationSignal(TSharedPtr<FJsonObject> Response, AActor* Actor, const FString& RequestedName)
{
    if (!Response || !Actor) return;

    // Collision signal: UE's Requested NameMode silently deduplicates the object
    // name to <name>_0 when the requested name is already taken, while the label
    // (= actorName, set by AddActorVerification) is allowed to collide. Surface
    // the unique object name and a flag the verification helper does not set, so a
    // caller can detect the rename and key follow-up calls on the round-trippable
    // actorObjectName instead of the shared, non-unique actorName.
    const FString ObjectName = Actor->GetName();
    Response->SetStringField(TEXT("requestedName"), RequestedName);
    Response->SetStringField(TEXT("actorObjectName"), ObjectName);
    Response->SetBoolField(TEXT("nameWasDeduplicated"), ObjectName != RequestedName);
}

void AddChainableActorFields(TSharedPtr<FJsonObject> Response, AActor* Actor)
{
    if (!Response || !Actor) return;

    Response->SetStringField(TEXT("actorPath"), Actor->GetPathName());
    Response->SetStringField(TEXT("actorLabel"), Actor->GetActorLabel());
    Response->SetStringField(TEXT("className"), Actor->GetClass()->GetName());
}

void AddComponentVerification(TSharedPtr<FJsonObject> Response, USceneComponent* Component)
{
    if (!Response || !Component) return;

    Response->SetStringField(TEXT("componentName"), Component->GetName());
    Response->SetStringField(TEXT("componentClass"), Component->GetClass()->GetName());
    // componentPath is the object path callers chain property.get / property.set
    // against — component UPROPERTYs aren't reachable via the owning actorPath.
    Response->SetStringField(TEXT("componentPath"), Component->GetPathName());
    if (AActor* Owner = Component->GetOwner())
    {
        // ownerActorPath names WHICH actor owns this component, so it must be the
        // owner's OBJECT path (matching AddActorVerification's actorPath and
        // AddChainableActorFields) — not the map PACKAGE path, which is shared by
        // every actor in the map and so can't disambiguate colliding labels. The
        // map package path is surfaced separately under ownerMapPath.
        Response->SetStringField(TEXT("ownerActorPath"), Owner->GetPathName());
        if (UPackage* OwnerPackage = Owner->GetPackage())
        {
            Response->SetStringField(TEXT("ownerMapPath"), OwnerPackage->GetPathName());
        }
    }
}

// The verification assetPath must be a handle the caller can feed straight back
// into the asset's own follow-up methods. The package path (/Game/Path/Asset) is a
// loadable object path only when the asset IS its package's top-level object; we branch
// on that STRUCTURAL property rather than any concrete class. A top-level asset keeps the
// bare package path. A sub-object asset — e.g. a ULevelScriptBlueprint, which is a
// sub-object of the .umap whose package path is the bare map path (/Game/Maps/<Map>) that
// LoadObject<UBlueprint> cannot resolve (every blueprint.graph.* method then rejects it
// with ASSET_NOT_FOUND) — instead gets its full object path (…:PersistentLevel.<Map>) from
// GetPathName(), which round-trips. Keying on the outer (not a class allowlist) fixes every
// sub-object asset that flows through here in one rule. (See E-open-level-blueprint-unusable-assetpath;
// the same anti-pattern was fixed for actors via AddActorVerification's actorPath/mapPath split.)
static FString ResolveVerificationAssetPath(UObject* Asset)
{
    UObject* Outer = Asset->GetOuter();
    const bool bIsTopLevelObject = Outer && Outer->IsA<UPackage>();
    if (bIsTopLevelObject)
    {
        return Asset->GetPackage() ? Asset->GetPackage()->GetPathName() : Asset->GetPathName();
    }
    return Asset->GetPathName();
}

// The single measured body behind AddAssetVerification / AddAssetVerificationNested.
//
// existsAfter used to be the literal `true`, fifteen lines above a real probe
// (VerifyAssetExists) emitting the SAME field name. That constant corroborated the
// equally constant saved:true from McpSafeAssetSave, so the write path and the verify
// path agreed with each other while both disagreed with the disk.
//
// Three separate facts are now reported instead of one fabricated one:
//   existsAfter  - resolvable right now, via the asset registry, keyed on the PACKAGE
//                  name. Package name, not the assetPath above: ResolveVerificationAssetPath
//                  deliberately returns a sub-object path for e.g. a ULevelScriptBlueprint,
//                  and UEditorAssetLibrary::DoesAssetExist cannot resolve those — probing
//                  with it would report existsAfter:false for a perfectly good asset.
//   existsOnDisk - a .uasset for that package is on disk.
//   pendingSave  - the package holds unsaved changes (emitted only when true).
// A transient-package asset, which can never be persisted, now reports false/false
// instead of the old unconditional true.
//
// assetPath and the handler's own assignment. This used to be an unconditional write, and
// AddAssetVerification runs AFTER the handler has built its payload, so any handler that had
// already set assetPath - roughly fifty production call sites, most of them from
// GetPathName() - had its value silently replaced by the package path. Object path and package
// path are not interchangeable across the whole verb surface, so a handler that correctly
// reported an object path had its response downgraded to a form the next verb might reject, and
// nothing in the response said the substitution had happened. Which ordering a verb got was
// decided by accident: two of the three asset-creating audio verbs were bitten, the third
// happened to assign after the helper and passed.
//
// Blanket "don't overwrite a populated field" is the wrong fix, because some of those call sites
// echo a caller-supplied request string rather than a resolved handle, and preserving that would
// trade a silent downgrade for a silent echo of unverified input. So the rule is narrower and
// checkable: the handler's value is KEPT when it denotes the same package as the object that was
// actually verified, and OVERWRITTEN otherwise - with requestedAssetPath and
// assetPathSubstituted:true recording that it happened, because a substitution the caller cannot
// see is the defect, not the substitution itself.
static void WriteMeasuredAssetVerification(const TSharedPtr<FJsonObject>& Target, UObject* Asset)
{
    const FString MeasuredAssetPath = ResolveVerificationAssetPath(Asset);
    UPackage* AssetPackage = Asset->GetPackage();
    const FString AssetPackageName = AssetPackage ? AssetPackage->GetName() : FString();

    FString ExistingAssetPath;
    const bool bHadAssetPath =
        Target->TryGetStringField(TEXT("assetPath"), ExistingAssetPath) && !ExistingAssetPath.IsEmpty();
    bool bKeepExisting = false;
    if (bHadAssetPath && !AssetPackageName.IsEmpty())
    {
        // ObjectPathToPackageName is a pure string operation: it returns the input unchanged
        // when there is no '.', and otherwise trims at the first one - so a package path, an
        // object path and a sub-object path for the same asset all reduce to one package name.
        const FString ExistingPackageName =
            FPackageName::ObjectPathToPackageName(ExistingAssetPath);
        bKeepExisting = ExistingPackageName.Equals(AssetPackageName, ESearchCase::IgnoreCase);
    }

    if (!bKeepExisting)
    {
        Target->SetStringField(TEXT("assetPath"), MeasuredAssetPath);
        if (bHadAssetPath)
        {
            Target->SetStringField(TEXT("requestedAssetPath"), ExistingAssetPath);
            Target->SetBoolField(TEXT("assetPathSubstituted"), true);
        }
    }
    // The package handle, under its own key, always. The comment on existsAfter below says
    // "Package name, not the assetPath above" - a sentence that only made sense if the author
    // believed they were writing a second field. Now they are, and a caller needing the package
    // no longer has to guess which of the two forms assetPath happens to carry. The key is
    // packageName, NOT packagePath: asset.list / asset.search rows already spell the containing
    // FOLDER packagePath, and asset.references / asset.dependencies already spell this exact
    // value packageName.
    if (!AssetPackageName.IsEmpty())
    {
        Target->SetStringField(TEXT("packageName"), AssetPackageName);
    }
    Target->SetStringField(TEXT("assetName"), Asset->GetName());
    Target->SetStringField(TEXT("assetClass"), Asset->GetClass()->GetName());

    UPackage* Package = AssetPackage;
    // RF_Transient on the OBJECT means the same non-durability as a transient package - the package
    // writer skips it - so neither existence fact can be keyed on its package name: both would be
    // answering about whatever else that name denotes. The probe is also not merely wrong there but
    // unsafe on the older engines: asking the registry about a package it has no on-disk record for
    // makes it build FAssetData from the in-memory object, and through UE 5.5
    // UNiagaraSystem::GetAssetRegistryTags answers that by running EnsureFullyLoaded() on a
    // half-built system (NiagaraSystem.cpp:653 ensure, then a null deref). UE 5.6 stopped doing it.
    const bool bTransient =
        !Package || Package == GetTransientPackage() || Package->HasAnyFlags(RF_Transient)
        || Asset->HasAnyFlags(RF_Transient);
    const FString PackageName = (Package && !bTransient) ? Package->GetName() : FString();

    const bool bOnDisk = !PackageName.IsEmpty() && DoesPackageFileExistOnDisk(PackageName);
    const bool bRegistered = !PackageName.IsEmpty() &&
        ResolveAsset(PackageName).bRegistryOrMemoryExists;

    Target->SetBoolField(TEXT("existsAfter"), bOnDisk || bRegistered);
    Target->SetBoolField(TEXT("existsOnDisk"), bOnDisk);
    if (Package && Package->IsDirty())
    {
        Target->SetBoolField(TEXT("pendingSave"), true);
    }
}

void AddAssetVerification(TSharedPtr<FJsonObject> Response, UObject* Asset)
{
    if (!Response || !Asset) return;

    WriteMeasuredAssetVerification(Response, Asset);
}

void AddAssetVerificationNested(TSharedPtr<FJsonObject> Response, const FString& FieldName, UObject* Asset)
{
    if (!Response || !Asset) return;

    TSharedPtr<FJsonObject> VerificationObj = MakeShared<FJsonObject>();
    WriteMeasuredAssetVerification(VerificationObj, Asset);
    Response->SetObjectField(FieldName, VerificationObj);
}

bool VerifyAssetExists(TSharedPtr<FJsonObject> Response, const FString& AssetPath)
{
    const bool bExists = ResolveAsset(AssetPath).bRegistryOrMemoryExists;
    if (Response)
    {
        Response->SetStringField(TEXT("verifiedPath"), AssetPath);
        Response->SetBoolField(TEXT("existsAfter"), bExists);
    }
    return bExists;
}

TSharedPtr<FJsonObject> BuildAssetRegistryTagsObject(const FAssetData& AssetData)
{
    TSharedPtr<FJsonObject> TagsObj = MakeShared<FJsonObject>();
    for (const TPair<FName, FAssetTagValueRef>& TagPair : AssetData.TagsAndValues)
    {
        TagsObj->SetStringField(TagPair.Key.ToString(), TagPair.Value.AsString());
    }
    return TagsObj;
}

// ============================================================================
// CreateControlRigBlueprint (moved from Subsystem)
// ============================================================================

#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"

#include "Utils/ControlRigBlueprintCompat.h"
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
  #include "ControlRigBlueprintFactory.h"
#endif

#include "EdGraph/RigVMEdGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

// Gives a freshly created Control Rig Blueprint the root RigVM ed graph that mirrors its default
// model, which FKismetEditorUtilities::CreateBlueprint does not create on its own.
//
// The engine's own factory does exactly this: UControlRigBlueprintFactory::FactoryCreateNew calls
// FRigVMEditorModule::CreateRootGraphIfRequired right after CreateBlueprint, because the other
// route to that graph - the RigVM editor module's on-blueprint-created callback - runs only inside
// CreateBlueprint's `if (Settings->bSpawnDefaultBlueprintNodes)` block (Kismet2.cpp) and so is not
// guaranteed. Without the root graph the asset carries a RigVM model with no editable mirror:
// URigVMBlueprint::GetEdGraph(Model) returns null and the Control Rig opens with no graph.
// That module entry point is not reachable from here (its header is private on 5.3/5.4 and its
// signature changed in 5.7), so this reproduces its body through public Blueprint API.
//
// No-op whenever an ed graph of the blueprint's own RigVM graph class is already present, which is
// the same early-out the engine's version uses.
static void AssetUtils_EnsureControlRigRootGraph(UControlRigBlueprint* Blueprint)
{
    URigVMBlueprint* RigVMBlueprint = Blueprint;
    if (!RigVMBlueprint)
    {
        return;
    }

    UClass* EdGraphClass = RigVMBlueprint->GetRigVMEdGraphClass();
    UClass* EdGraphSchemaClass = RigVMBlueprint->GetRigVMEdGraphSchemaClass();
    if (!EdGraphClass || !EdGraphSchemaClass)
    {
        return;
    }

    for (const UEdGraph* EdGraph : RigVMBlueprint->UbergraphPages)
    {
        if (EdGraph && EdGraph->IsA(EdGraphClass))
        {
            return;
        }
    }

    const URigVMEdGraphSchema* SchemaCDO =
        Cast<URigVMEdGraphSchema>(EdGraphSchemaClass->GetDefaultObject());
    if (!SchemaCDO)
    {
        return;
    }

    UEdGraph* RootGraph = FBlueprintEditorUtils::CreateNewGraph(
        RigVMBlueprint, SchemaCDO->GetRootGraphName(), EdGraphClass, EdGraphSchemaClass);
    if (!RootGraph)
    {
        return;
    }
    RootGraph->bAllowDeletion = false;
    FBlueprintEditorUtils::AddUbergraphPage(RigVMBlueprint, RootGraph);
    RigVMBlueprint->LastEditedDocuments.AddUnique(RootGraph);
    // Binds the new ed graph to the blueprint's model, the way the engine's version does: the
    // blueprint's PostLoad runs InitializeModelIfRequired, which calls InitializeFromBlueprint on
    // every RigVM ubergraph page.
    RigVMBlueprint->PostLoad();
}

UBlueprint* McpCreateControlRigBlueprint(
    const FString& AssetName, const FString& PackagePath,
    USkeleton* TargetSkeleton, FString& OutError)
{
    if (AssetName.IsEmpty())
    {
        OutError = TEXT("Asset name cannot be empty");
        return nullptr;
    }

    if (PackagePath.IsEmpty())
    {
        OutError = TEXT("Package path cannot be empty");
        return nullptr;
    }

    FString NormalizedPath = PackagePath;
    NormalizedPath.ReplaceInline(TEXT("/Content"), TEXT("/Game"));
    NormalizedPath.ReplaceInline(TEXT("\\"), TEXT("/"));

    if (!IsValidMountPoint(NormalizedPath))
    {
        NormalizedPath = TEXT("/Game") / NormalizedPath;
    }

    while (NormalizedPath.EndsWith(TEXT("/")))
    {
        NormalizedPath.LeftChopInline(1);
    }

    FString FullPackageName = NormalizedPath / AssetName;

    // The normalization above does not make this safe: FString::operator/ only avoids DOUBLING a
    // slash it introduces itself, and neither half is checked for a "//" it already carries — a
    // single AssetName of "a//b" reaches CreatePackage's Fatal from any caller. Refuse here.
    if (!AssetUtils_ValidatePackageTargetForCreatePackage(FullPackageName, AssetName, OutError))
    {
        return nullptr;
    }

    UPackage* Package = CreatePackage(*FullPackageName);
    if (!Package)
    {
        OutError =
            FString::Printf(TEXT("Failed to create package: %s"), *FullPackageName);
        return nullptr;
    }

    Package->FullyLoad();

    UControlRigBlueprint* NewBlueprint = Cast<UControlRigBlueprint>(
        FKismetEditorUtilities::CreateBlueprint(
            UControlRig::StaticClass(),
            Package,
            *AssetName,
            BPTYPE_Normal,
            UControlRigBlueprint::StaticClass(),
            URigVMBlueprintGeneratedClass::StaticClass(),
            NAME_None));

    if (!NewBlueprint)
    {
        OutError = TEXT("Factory failed to create Control Rig Blueprint");
        return nullptr;
    }

    AssetUtils_EnsureControlRigRootGraph(NewBlueprint);

    if (TargetSkeleton)
    {
        USkeletalMesh* PreviewMesh = TargetSkeleton->GetPreviewMesh();
        if (PreviewMesh)
        {
            NewBlueprint->SetPreviewMesh(PreviewMesh);
        }
    }

    FAssetRegistryModule::AssetCreated(NewBlueprint);
    NewBlueprint->MarkPackageDirty();
    McpSafeAssetSave(NewBlueprint);

    UE_LOG(LogPinWrightSubsystem, Log,
           TEXT("Created Control Rig Blueprint: %s"), *FullPackageName);

    return NewBlueprint;
}

void AppendBlueprintAssetsDerivedFromNativeClass(
    IAssetRegistry& AssetRegistry,
    UClass* RootNativeClass,
    TConstArrayView<FName> PackagePaths,
    bool bRecursivePaths,
    int32 MaxResults,
    TArray<FAssetData>& InOutAssets)
{
    if (!RootNativeClass) return;

    TSet<FTopLevelAssetPath> DerivedPaths;
    {
        TSet<FTopLevelAssetPath> Excluded;
        AssetRegistry.GetDerivedClassNames(
            { RootNativeClass->GetClassPathName() }, Excluded, DerivedPaths);
    }
    DerivedPaths.Add(RootNativeClass->GetClassPathName());

    FARFilter BpFilter;
    BpFilter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    BpFilter.bRecursiveClasses = true; // covers WidgetBlueprint / AnimBlueprint / ...
    for (const FName& Path : PackagePaths)
    {
        BpFilter.PackagePaths.Add(Path);
    }
    BpFilter.bRecursivePaths = bRecursivePaths;

    TArray<FAssetData> BpAssets;
    AssetRegistry.GetAssets(BpFilter, BpAssets);

    TSet<FSoftObjectPath> Seen;
    Seen.Reserve(InOutAssets.Num());
    for (const FAssetData& A : InOutAssets)
    {
        Seen.Add(A.GetSoftObjectPath());
    }

    for (const FAssetData& BpAsset : BpAssets)
    {
        if (MaxResults >= 0 && InOutAssets.Num() >= MaxResults) break;

        FString RawTag;
        if (!BpAsset.GetTagValue(FBlueprintTags::ParentClassPath, RawTag)) continue;

        // Tag is written by FObjectPropertyBase::GetExportPath as "Class'/Script/Module.Name'".
        // ExportTextPathToObjectPath unwraps it; if the tag was already a bare path it returns it unchanged.
        const FString ObjectPath = FPackageName::ExportTextPathToObjectPath(RawTag);
        const FTopLevelAssetPath TagPath(ObjectPath);
        if (!DerivedPaths.Contains(TagPath)) continue;

        const FSoftObjectPath SoftPath = BpAsset.GetSoftObjectPath();
        bool bAlreadySeen = false;
        Seen.Add(SoftPath, &bAlreadySeen);
        if (bAlreadySeen) continue;

        InOutAssets.Add(BpAsset);
    }
}

// ============================================================================
// Generic UObject Resolution
// ============================================================================

UObject* ResolveUObjectByPath(const FString& Path, FString& OutError)
{
    if (Path.IsEmpty())
    {
        OutError = TEXT("Object path is empty");
        return nullptr;
    }

    // The StaticLoadObject below reaches CreatePackage's Fatal for a name containing "//"
    // (UObjectGlobals.cpp:1094-1096) - process death, so `if (Loaded)` never runs. The
    // StaticFindObject above it is safe (ResolveName2 with Create=false, :620), which is why the
    // guard can sit here rather than needing to be split. Distinct wording from the "Object not
    // found" miss below: malformed argument, not absent object.
    if (CanReachCreatePackageFatal(Path))
    {
        OutError = FString::Printf(
            TEXT("Object path '%s' contains '//' and was refused: a doubled slash can reach "
                 "CreatePackage's Fatal and end the editor process."), *Path);
        return nullptr;
    }

    // StaticFindObject catches transient objects, editor subsystems, and any
    // already-loaded UObject — including non-asset instances that StaticLoadObject
    // would skip.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    UObject* Found = StaticFindObject(UObject::StaticClass(), nullptr, *Path, EFindObjectFlags::None);
#else
    UObject* Found = StaticFindObject(UObject::StaticClass(), nullptr, *Path, /*ExactClass*/false);
#endif
    if (Found)
    {
        return Found;
    }

    // Fall back to LoadObject for on-disk assets that are not yet loaded.
    UObject* Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
    if (Loaded)
    {
        return Loaded;
    }

    OutError = FString::Printf(TEXT("Object not found: %s"), *Path);
    return nullptr;
}
