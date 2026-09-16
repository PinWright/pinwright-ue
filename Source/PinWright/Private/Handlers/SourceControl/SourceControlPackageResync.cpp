// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/SourceControl/SourceControlPackageResync.h"

#include "PinWrightSubsystem.h"

#include "SourceControlHelpers.h"

#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtrTemplates.h"

namespace PinWrightSourceControlResync
{
namespace
{
    // Pre-operation snapshot of one target. The package is held WEAKLY on purpose: after
    // UPackageTools::ReloadPackages the old UPackage is renamed out of the way and
    // garbage-collected, so a raw pointer kept across the call is dangling and comparing
    // its address risks an ABA hit against a freshly allocated package. A weak pointer
    // resolves to null once the old object dies, which is exactly the signal wanted.
    struct FPwResyncPreState
    {
        TWeakObjectPtr<UPackage> PreviousPackage;
    };

    // Mirrors the bReloadWorld=false filter inside
    // USourceControlHelpers::ApplyOperationAndReloadPackages: a loaded world package, or a
    // loaded external package (one-file-per-actor / external object) whose owning world is
    // itself loaded, is one the engine will not reload — it aborts the whole batch before
    // running the operation and only logs a warning.
    bool PwResyncIsBlockedFromReload(UPackage* Package)
    {
        if (UWorld::FindWorldInPackage(Package))
        {
            return true;
        }
        if (const UObject* Asset = Package->FindAssetInPackage())
        {
            if (Asset->IsPackageExternal() && Asset->GetWorld() && Asset->GetWorld()->GetPackage())
            {
                return true;
            }
        }
        return false;
    }
}

bool ApplyAndResyncPackages(const TArray<FString>& Filenames,
    const TFunctionRef<bool(const TArray<FString>&)>& Operation,
    FPackageResyncReport& OutReport)
{
    OutReport = FPackageResyncReport();

    TArray<FPwResyncPreState> PreStates;
    PreStates.Reserve(Filenames.Num());
    OutReport.Entries.Reserve(Filenames.Num());

    for (const FString& Filename : Filenames)
    {
        FPackageResyncEntry Entry;
        Entry.Filename = Filename;

        // Same normalization the engine helper performs, so the package name measured here
        // is the one it will operate on: a filename that does not map into a mounted content
        // root is passed through as-is (it may already be a long package name).
        FString PackageName;
        Entry.PackageName = FPackageName::TryConvertFilenameToLongPackageName(Filename, PackageName)
            ? PackageName
            : Filename;

        FPwResyncPreState PreState;
        if (UPackage* Package = FindPackage(nullptr, *Entry.PackageName))
        {
            Entry.bWasLoaded = true;
            PreState.PreviousPackage = Package;
            ++OutReport.LoadedCount;

            if (PwResyncIsBlockedFromReload(Package))
            {
                OutReport.BlockedPackages.Add(Entry.PackageName);
            }
        }

        PreStates.Add(MoveTemp(PreState));
        OutReport.Entries.Add(MoveTemp(Entry));
    }

    if (OutReport.BlockedPackages.Num() > 0)
    {
        // Refuse BEFORE the operation. Running it would change the working tree under a
        // live world that keeps the old state — the exact silent-stale outcome this helper
        // exists to prevent, except unrecoverable because the file no longer matches.
        UE_LOG(LogPinWrightSubsystem, Warning,
            TEXT("Source-control package resync refused: %d loaded map/external package(s) cannot be reloaded (first: %s). Nothing was applied."),
            OutReport.BlockedPackages.Num(), *OutReport.BlockedPackages[0]);
        return false;
    }

    OutReport.bOperationSucceeded = SourceControlHelpers::ApplyOperationAndReloadPackages(
        Filenames, Operation, /*bReloadWorld=*/false, /*bInteractive=*/false);

    // Measure the outcome instead of inferring it from the return value: the engine helper
    // returns the OPERATION's result, and logs reload failures rather than reporting them.
    // A package that is still the same object at the same name was not re-read from disk.
    for (int32 Index = 0; Index < OutReport.Entries.Num(); ++Index)
    {
        FPackageResyncEntry& Entry = OutReport.Entries[Index];
        if (!Entry.bWasLoaded)
        {
            continue;
        }

        const UPackage* NowResident = FindPackage(nullptr, *Entry.PackageName);
        if (NowResident == nullptr)
        {
            Entry.bRemoved = true;
            ++OutReport.RemovedCount;
        }
        else if (PreStates[Index].PreviousPackage.Get() != NowResident)
        {
            Entry.bReloaded = true;
            ++OutReport.ReloadedCount;
        }
        else
        {
            Entry.bStale = true;
            ++OutReport.StaleCount;
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("Source-control package resync did not reload %s; the in-memory object still holds the pre-operation state and saving it would overwrite the result on disk."),
                *Entry.PackageName);
        }
    }

    return OutReport.bOperationSucceeded;
}
}
