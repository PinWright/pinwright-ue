// Copyright (c) 2026 Alexander Penkin. MIT License.

// Game-safe half of the asset helpers (resolution, Blueprint lookup, actor spawn, verification).
// The editor-only half (saves, level saves, asset creation) stays in the editor module's Utils/AssetUtils.
#include "Utils/AssetResolveUtils.h"

#include "Runtime/Launch/Resources/Version.h"
#include "Compat/EngineVersionCompat.h"
#include "Dispatch/PinWrightWorld.h"
#include "Utils/PieSaveBlockGuard.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#if WITH_EDITOR
#include "Editor.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#endif

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
    // Load only what can load: a registry hit, or a package file on disk the registry has not
    // indexed yet. Anything else is a guaranteed miss - in-memory objects, transient ones
    // included, were already found above - and LoadObject would only log a user-visible
    // "Failed to find object" warning and leave an empty in-memory UPackage behind, which a
    // later package-existence probe then mistakes for a real asset.
    if (!Object && bLoadObject && (Out.bExists || FPackageName::DoesPackageExist(Normalized.Path)))
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
void ScanPathSynchronous(const FString& InPath, bool bRecursive)
{
    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    TArray<FString> PathsToScan;
    PathsToScan.Add(InPath);
    AssetRegistry.ScanPathsSynchronous(PathsToScan, bRecursive);
}
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
// ============================================================================
// String Conversion Helpers
// ============================================================================

FString ConvertToString(const FString& In) { return In; }
FString ConvertToString(const FName& In) { return In.ToString(); }
FString ConvertToString(const FText& In) { return In.ToString(); }

// ============================================================================
// Actor Spawning
// ============================================================================

AActor* SpawnActorInActiveWorldImpl(UClass* ActorClass, const FVector& Location,
                                    const FRotator& Rotation, const FString& OptionalLabel)
{
    if (!ActorClass)
        return nullptr;

    AActor* Spawned = nullptr;
    auto SpawnDirectInWorld = [ActorClass, &Location, &Rotation](UWorld* World) -> AActor*
    {
        if (!World)
        {
            return nullptr;
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
        return World->SpawnActor(ActorClass, &Location, &Rotation, SpawnParams);
    };

#if WITH_EDITOR
    if (GIsEditor)
    {
        if (!GEditor)
            return nullptr;

        // Check if PIE is active
        UWorld* TargetWorld = GEditor->PlayWorld;

        if (TargetWorld)
        {
            // PIE Path: Use World->SpawnActor for proper world context
            Spawned = SpawnDirectInWorld(TargetWorld);
        }
        else
        {
            // Editor Path:
            // - In unattended/null-RHI runs, avoid EditorActorSubsystem placement code
            //   because it depends on viewport hit-proxy state and can crash.
            // - In normal interactive editor, keep subsystem spawning behavior.
            const bool bNullRHI = FParse::Param(FCommandLine::Get(), TEXT("NullRHI"));
            const bool bPreferDirectEditorSpawn =
                IsRunningCommandlet() || FApp::IsUnattended() || bNullRHI;

            if (bPreferDirectEditorSpawn)
            {
                UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
                Spawned = SpawnDirectInWorld(EditorWorld);
            }
            else
            {
                UEditorActorSubsystem* ActorSS =
                    GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
                if (ActorSS)
                {
                    Spawned = ActorSS->SpawnActorFromClass(ActorClass, Location, Rotation);
                    if (Spawned)
                    {
                        // Explicit transform to ensure proper placement and registration
                        Spawned->SetActorLocationAndRotation(Location, Rotation, false, nullptr,
                                                             ETeleportType::TeleportPhysics);
                    }
                }
                if (!Spawned)
                {
                    UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
                    Spawned = SpawnDirectInWorld(EditorWorld);
                }
            }
        }

        // Set optional label for easy identification in World Outliner
        if (Spawned && !OptionalLabel.IsEmpty())
        {
            Spawned->SetActorLabel(OptionalLabel);
        }
        return Spawned;
    }
#endif

    // Game process: the game world; labels do not exist there, so OptionalLabel is unused.
    return SpawnDirectInWorld(PinWright::GetTargetWorld());
}

// ============================================================================
// Verification Helpers
// ============================================================================

// Display label in the editor; game builds carry no labels, so the object name stands in.
static FString AssetResolve_ActorLabelOrName(const AActor* Actor)
{
#if WITH_EDITOR
    return Actor->GetActorLabel();
#else
    return Actor->GetName();
#endif
}

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
    const FString Label = AssetResolve_ActorLabelOrName(Actor);
    Response->SetStringField(TEXT("actorName"), Label);
    Response->SetStringField(TEXT("actorLabel"), Label);
    Response->SetStringField(TEXT("actorObjectName"), Actor->GetName());
#if WITH_EDITOR
    // ActorGuid is editor-only data: game builds omit the key.
    Response->SetStringField(TEXT("actorGuid"), Actor->GetActorGuid().ToString());
#endif
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
    Response->SetStringField(TEXT("actorLabel"), AssetResolve_ActorLabelOrName(Actor));
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
