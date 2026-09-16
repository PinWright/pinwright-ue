// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/RedirectorFixupPolicy.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Dispatch/ScopedUnattendedRpc.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "IAssetTools.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/WeakObjectPtrTemplates.h"

// File-scope helper names carry the RedirectorFixupPolicy_ prefix: Unity merges
// translation units, so a plain LoadReferencer() would collide with a same-named
// static in another file.

// The redirectors worth acting on: valid, and pointing somewhere. A redirector whose
// DestinationObject is null has no target path to rewrite referencers to, so it is
// reported as skipped rather than silently treated as fixed and deleted.
static void RedirectorFixupPolicy_Partition(
    const TArray<UObjectRedirector*>& In, TArray<UObjectRedirector*>& OutValid, int32& OutSkipped)
{
    for (UObjectRedirector* Redirector : In)
    {
        if (IsValid(Redirector) && Redirector->DestinationObject != nullptr && Redirector->GetOutermost() != nullptr)
        {
            OutValid.AddUnique(Redirector);
        }
        else
        {
            ++OutSkipped;
        }
    }
}

// Long package name for any of the spellings a caller may pass: "/Game/Foo/SM_A",
// "/Game/Foo/SM_A.SM_A", or the "/Content/Foo/SM_A" form the fixup verb already accepts.
// Returns an empty string for anything that is not a rooted path.
static FString RedirectorFixupPolicy_ToPackageName(const FString& AnyPath)
{
    FString PackageName = AnyPath;
    PackageName.TrimStartAndEndInline();

    int32 DotIndex = INDEX_NONE;
    if (PackageName.FindChar(TEXT('.'), DotIndex))
    {
        PackageName.LeftInline(DotIndex);
    }

    if (PackageName.StartsWith(TEXT("/Content"), ESearchCase::IgnoreCase))
    {
        PackageName = FString::Printf(TEXT("/Game%s"), *PackageName.RightChop(8));
    }

    return PackageName.StartsWith(TEXT("/")) ? PackageName : FString();
}

bool RedirectorFixupPolicy::ParseSweepScope(const FString& In, ESweepScope& OutScope)
{
    if (In.Equals(TEXT("paths"), ESearchCase::IgnoreCase))
    {
        OutScope = ESweepScope::RequestedPaths;
        return true;
    }
    if (In.Equals(TEXT("project"), ESearchCase::IgnoreCase))
    {
        OutScope = ESweepScope::Project;
        return true;
    }
    return false;
}

const TCHAR* RedirectorFixupPolicy::SweepScopeToString(ESweepScope Scope)
{
    return Scope == ESweepScope::Project ? TEXT("project") : TEXT("paths");
}

TArray<FName> RedirectorFixupPolicy::PackageFoldersForAssets(const TArray<FString>& AssetPaths)
{
    TArray<FName> Folders;
    for (const FString& AssetPath : AssetPaths)
    {
        const FString PackageName = RedirectorFixupPolicy_ToPackageName(AssetPath);
        if (PackageName.IsEmpty())
        {
            continue;
        }

        const FString Folder = FPackageName::GetLongPackagePath(PackageName);
        if (!Folder.IsEmpty() && Folder != TEXT("/"))
        {
            Folders.AddUnique(FName(*Folder));
        }
    }
    return Folders;
}

FARFilter RedirectorFixupPolicy::BuildSweepFilter(const TArray<FString>& AssetPaths, ESweepScope Scope)
{
    FARFilter Filter;
    Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/CoreUObject"), TEXT("ObjectRedirector")));

    if (Scope == ESweepScope::Project)
    {
        // PackagePaths deliberately left unset - that IS the project-wide query.
        return Filter;
    }

    Filter.PackagePaths = PackageFoldersForAssets(AssetPaths);
    Filter.bRecursivePaths = false;

    // An FARFilter with an empty PackagePaths array matches EVERY path, so "no folder
    // resolved" would silently become the project-wide sweep this scope exists to
    // prevent - the widest possible reading of the narrowest possible request. Pin it to
    // a path no mount point can produce instead, so an empty request matches nothing.
    if (Filter.PackagePaths.Num() == 0)
    {
        Filter.PackagePaths.Add(FName(TEXT("/PinWrightEmptySweepScope")));
    }

    return Filter;
}

bool RedirectorFixupPolicy::IsInPackageFolders(const FString& PackageName, const TArray<FName>& Folders)
{
    if (PackageName.IsEmpty() || Folders.Num() == 0)
    {
        return false;
    }

    const FString Normalized = RedirectorFixupPolicy_ToPackageName(PackageName);
    if (Normalized.IsEmpty())
    {
        return false;
    }

    return Folders.Contains(FName(*FPackageName::GetLongPackagePath(Normalized)));
}

TMap<FSoftObjectPath, FSoftObjectPath> RedirectorFixupPolicy::BuildRedirectorMap(
    const TArray<UObjectRedirector*>& Redirectors)
{
    TMap<FSoftObjectPath, FSoftObjectPath> Map;

    for (const UObjectRedirector* Redirector : Redirectors)
    {
        if (!IsValid(Redirector) || Redirector->DestinationObject == nullptr)
        {
            continue;
        }

        const FSoftObjectPath OldPath(Redirector);
        const FSoftObjectPath NewPath(Redirector->DestinationObject);
        Map.Add(OldPath, NewPath);

        // Mirrors AssetFixUpRedirectors.cpp:855-860. A Blueprint is referenced by three
        // distinct paths and a soft reference may hold any of them; rewriting only the
        // asset path leaves class and CDO references pointing at the old package.
        if (Cast<UBlueprint>(Redirector->DestinationObject) != nullptr)
        {
            Map.Add(FSoftObjectPath(FString::Printf(TEXT("%s_C"), *OldPath.ToString())),
                    FSoftObjectPath(FString::Printf(TEXT("%s_C"), *NewPath.ToString())));
            Map.Add(FSoftObjectPath(FString::Printf(TEXT("%s.Default__%s_C"),
                        *OldPath.GetLongPackageName(), *OldPath.GetAssetName())),
                    FSoftObjectPath(FString::Printf(TEXT("%s.Default__%s_C"),
                        *NewPath.GetLongPackageName(), *NewPath.GetAssetName())));
        }
    }

    return Map;
}

RedirectorFixupPolicy::FResult RedirectorFixupPolicy::FixupReferencers(
    const TArray<UObjectRedirector*>& Redirectors, bool bDeleteFixedUpRedirectors)
{
    FResult Result;

    TArray<UObjectRedirector*> Valid;
    RedirectorFixupPolicy_Partition(Redirectors, Valid, Result.SkippedRedirectors);
    Result.RedirectorsConsidered = Valid.Num();
    if (Valid.Num() == 0)
    {
        return Result;
    }

    // Own scope rather than relying on the dispatcher's: asset.fixup_redirectors runs
    // its body from an AsyncTask continuation, which resumes on a later tick outside
    // FRpcDispatcher's FScopedUnattendedRpc. Nothing below raises a dialog by design,
    // but ObjectTools::DeleteObjects and the engine save path each still consult
    // GIsRunningUnattendedScript on their internal branches.
    FScopedUnattendedRpc UnattendedScope;

    IAssetRegistry& Registry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    IAssetTools& AssetTools =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

    // 1. Referencing packages, per redirector so a partial failure can be attributed.
    TMap<UObjectRedirector*, TArray<FName>> PerRedirectorReferencers;
    TSet<FName> AllReferencerNames;
    for (UObjectRedirector* Redirector : Valid)
    {
        const FName RedirectorPackageName = Redirector->GetOutermost()->GetFName();

        TArray<FName> Referencers;
        Registry.GetReferencers(RedirectorPackageName, Referencers,
            UE::AssetRegistry::EDependencyCategory::Package,
            UE::AssetRegistry::FDependencyQuery());
        Referencers.Remove(RedirectorPackageName);

        PerRedirectorReferencers.Add(Redirector, Referencers);
        AllReferencerNames.Append(Referencers);
    }

    // 2. Load them. A package that will not load, or that is compiled in, is recorded
    //    as a failure for every redirector it references so that redirector survives.
    TSet<FString> FailedNames;
    TSet<FString> CodeNames;
    TArray<UPackage*> ReferencingPackages;
    for (const FName& ReferencerName : AllReferencerNames)
    {
        const FString PackageName = ReferencerName.ToString();

        UPackage* Package = FindPackage(nullptr, *PackageName);
        if (Package == nullptr)
        {
            Package = LoadPackage(nullptr, *PackageName, LOAD_None);
        }

        if (Package == nullptr)
        {
            FailedNames.Add(PackageName);
            Result.FailedPackages.AddUnique(PackageName);
            continue;
        }

        if (Package->HasAnyPackageFlags(PKG_CompiledIn))
        {
            CodeNames.Add(PackageName);
            Result.CodeReferences.AddUnique(PackageName);
            continue;
        }

        ReferencingPackages.AddUnique(Package);
    }
    Result.ReferencingPackagesFound = ReferencingPackages.Num();

    // 3. Root everything in the referencing packages so the saves below cannot be
    //    GC'd out from under us (AssetFixUpRedirectors.cpp:729-737).
    TArray<TStrongObjectPtr<UObject>> RootedObjects;
    for (UPackage* Package : ReferencingPackages)
    {
        TArray<UObject*> ObjectsInPackage;
        GetObjectsWithPackage(Package, ObjectsInPackage);
        for (UObject* Object : ObjectsInPackage)
        {
            RootedObjects.Emplace(Object);
        }
    }

    // 4. A world referenced through a Level Instance keeps a loader open that blocks
    //    the re-save (AssetFixUpRedirectors.cpp:741-757).
    {
        TSet<FName> WorldAssetsNeedingLoadersReset;
        for (UPackage* Package : ReferencingPackages)
        {
            TArray<FAssetData> PackageAssets;
            if (Registry.GetAssetsByPackageName(Package->GetFName(), PackageAssets, /*bIncludeOnlyOnDiskAssets=*/true))
            {
                for (const FAssetData& Asset : PackageAssets)
                {
                    if (!Asset.GetOptionalOuterPathName().IsNone())
                    {
                        WorldAssetsNeedingLoadersReset.Add(
                            FSoftObjectPath(Asset.GetOptionalOuterPathName().ToString()).GetLongPackageFName());
                    }
                }
            }
        }
        for (const FName& WorldAsset : WorldAssetsNeedingLoadersReset)
        {
            ULevelInstanceSubsystem::ResetLoadersForWorldAsset(WorldAsset.ToString());
        }
    }

    // 5. Rewrite soft object paths. Dirty world/content packages are swept in as well
    //    because an unsaved level can hold a soft reference to the old path that would
    //    otherwise be written back out after the redirector is gone
    //    (AssetFixUpRedirectors.cpp:838-864). RenameReferencingSoftObjectPaths only
    //    serialises objects - it opens nothing (AssetRenameManager.cpp:1581-1616).
    {
        TArray<UPackage*> PackagesToCheck = ReferencingPackages;

        TArray<UPackage*> DirtyPackages;
        FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);
        FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);
        for (UPackage* Package : DirtyPackages)
        {
            PackagesToCheck.AddUnique(Package);
        }

        AssetTools.RenameReferencingSoftObjectPaths(PackagesToCheck, BuildRedirectorMap(Valid));
    }

    // 6. Save. bCheckDirty=false because loading a package resolves its hard imports
    //    through the redirector without dirtying it; bPromptToSave=false is what keeps
    //    the save dialog out. Under GIsRunningUnattendedScript this whole call short-
    //    circuits to UEditorLoadingAndSavingUtils::SavePackages (FileHelpers.cpp:4658),
    //    which forces the flag on again around the checkout (FileHelpers.cpp:5919).
    if (ReferencingPackages.Num() > 0)
    {
        TArray<UPackage*> FailedToSave;
        const FEditorFileUtils::EPromptReturnCode SaveCode =
            FEditorFileUtils::PromptForCheckoutAndSave(ReferencingPackages,
                /*bCheckDirty=*/false, /*bPromptToSave=*/false, &FailedToSave);

        for (UPackage* Package : FailedToSave)
        {
            if (Package != nullptr)
            {
                FailedNames.Add(Package->GetName());
                Result.FailedPackages.AddUnique(Package->GetName());
            }
        }

        // The unattended branch reports pass/fail for the batch only and never fills
        // FailedToSave, so a bare failure has to be attributed to every package or a
        // redirector would be deleted on the strength of a save that did not happen.
        if (SaveCode != FEditorFileUtils::PR_Success && FailedToSave.Num() == 0)
        {
            for (UPackage* Package : ReferencingPackages)
            {
                FailedNames.Add(Package->GetName());
                Result.FailedPackages.AddUnique(Package->GetName());
            }
        }

        for (UPackage* Package : ReferencingPackages)
        {
            if (!FailedNames.Contains(Package->GetName()))
            {
                ++Result.ReferencingPackagesSaved;
            }
        }
    }

    // 7. Let the registry catch up before anything reads the fixed-up graph
    //    (AssetFixUpRedirectors.cpp:908-922).
    {
        TArray<FString> AssetPaths;
        for (UObjectRedirector* Redirector : Valid)
        {
            AssetPaths.AddUnique(
                FPackageName::GetLongPackagePath(Redirector->GetOutermost()->GetName()) / TEXT(""));

            for (const FName& ReferencerName : PerRedirectorReferencers[Redirector])
            {
                AssetPaths.AddUnique(
                    FPackageName::GetLongPackagePath(ReferencerName.ToString()) / TEXT(""));
            }
        }
        if (AssetPaths.Num() > 0)
        {
            Registry.ScanPathsSynchronous(AssetPaths, /*bForceRescan=*/true);
        }
    }

    if (!bDeleteFixedUpRedirectors)
    {
        return Result;
    }

    // 8. Delete only the redirectors whose every referencer was re-saved. This is the
    //    decision the engine's report dialog asks a human to confirm; with no human to
    //    ask, "all clear" is the only answer that cannot break a live reference.
    // Candidate identity captured BEFORE the delete, because after it there is nothing
    // left to read a name off. The weak pointer is what turns "I asked for this to go"
    // into "this went" (docs/rpc-design.md §4).
    struct FDeleteCandidate
    {
        TWeakObjectPtr<UObjectRedirector> Redirector;
        FString PackageName;
    };
    TArray<FDeleteCandidate> DeleteCandidates;

    TArray<UObject*> ObjectsToDelete;
    for (UObjectRedirector* Redirector : Valid)
    {
        bool bFullyFixed = true;
        for (const FName& ReferencerName : PerRedirectorReferencers[Redirector])
        {
            const FString ReferencerString = ReferencerName.ToString();
            if (FailedNames.Contains(ReferencerString) || CodeNames.Contains(ReferencerString))
            {
                bFullyFixed = false;
                break;
            }
        }

        if (!bFullyFixed)
        {
            continue;
        }

        // Every redirector in the package went through the same fix-up, so the whole
        // package goes - unless it also holds a real asset (AssetFixUpRedirectors.cpp:952-975).
        UPackage* RedirectorPackage = Redirector->GetOutermost();
        TArray<UObject*> ObjectsInPackage;
        // Defaults to EGetObjectsFlags::IncludeNestedObjects; the bool overload is
        // deprecated in 5.8.
        GetObjectsWithOuter(RedirectorPackage, ObjectsInPackage);

        bool bContainsAtLeastOneOtherAsset = false;
        for (UObject* Object : ObjectsInPackage)
        {
            if (UObjectRedirector* PackagedRedirector = Cast<UObjectRedirector>(Object))
            {
                PackagedRedirector->RemoveFromRoot();
                ObjectsToDelete.AddUnique(PackagedRedirector);
                DeleteCandidates.Add({ PackagedRedirector, RedirectorPackage->GetName() });
            }
            else
            {
                bContainsAtLeastOneOtherAsset = true;
            }
        }

        if (!bContainsAtLeastOneOtherAsset)
        {
            RedirectorPackage->RemoveFromRoot();
            ObjectsToDelete.AddUnique(RedirectorPackage);
        }
    }

    if (ObjectsToDelete.Num() > 0)
    {
        // bShowConfirmation=false. DeleteObjects still runs its own in-memory reference
        // check, whose refusal notice is an FMessageDialog - suppressed to its default
        // by the scope above, and a refusal simply lowers the returned count.
        Result.RedirectorsDeleted = ObjectTools::DeleteObjects(ObjectsToDelete, false);

        // Name what went. A refused object stays resolvable, so the weak pointer sorts
        // the two apart without trusting the request or the count.
        for (const FDeleteCandidate& Candidate : DeleteCandidates)
        {
            if (!Candidate.Redirector.IsValid() && !Candidate.PackageName.IsEmpty())
            {
                Result.DeletedRedirectorPackages.AddUnique(Candidate.PackageName);
            }
        }
    }

    return Result;
}

void RedirectorFixupPolicy::AddReport(const TSharedPtr<FJsonObject>& Out, const FResult& Result)
{
    if (!Out.IsValid())
    {
        return;
    }

    Out->SetNumberField(TEXT("redirectorsConsidered"), Result.RedirectorsConsidered);
    Out->SetNumberField(TEXT("referencingPackagesFound"), Result.ReferencingPackagesFound);
    Out->SetNumberField(TEXT("referencingPackagesSaved"), Result.ReferencingPackagesSaved);
    Out->SetNumberField(TEXT("redirectorsDeleted"), Result.RedirectorsDeleted);

    if (Result.SkippedRedirectors > 0)
    {
        Out->SetNumberField(TEXT("redirectorsSkipped"), Result.SkippedRedirectors);
    }

    if (Result.DeletedRedirectorPackages.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> DeletedPaths;
        for (const FString& PackageName : Result.DeletedRedirectorPackages)
        {
            DeletedPaths.Add(MakeShared<FJsonValueString>(PackageName));
        }
        Out->SetArrayField(TEXT("redirectorsDeletedPaths"), DeletedPaths);
    }

    if (Result.FailedPackages.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> Failed;
        for (const FString& PackageName : Result.FailedPackages)
        {
            Failed.Add(MakeShared<FJsonValueString>(PackageName));
        }
        Out->SetArrayField(TEXT("failedPackages"), Failed);
    }

    if (Result.CodeReferences.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> CodeRefs;
        for (const FString& PackageName : Result.CodeReferences)
        {
            CodeRefs.Add(MakeShared<FJsonValueString>(PackageName));
        }
        Out->SetArrayField(TEXT("codeReferences"), CodeRefs);
    }
}
