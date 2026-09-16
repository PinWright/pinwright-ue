// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/PackageDiskStateGuard.h"

#include "PinWrightSubsystem.h"

#include "HAL/FileManager.h"
#include "IO/IoHash.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/PackageName.h"
#include "Serialization/Archive.h"
#include "UObject/ObjectVersion.h"
#include "UObject/Package.h"
#include "UObject/PackageFileSummary.h"

namespace PinWrightPackageDiskState
{
// Everything in this block is reachable only on the engine versions that carry the saved-hash
// ledger. Guarding the whole block, rather than each helper, keeps a 5.3 host from compiling
// helpers nothing can call.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
namespace
{
    // Resolve the package's file the way the rest of AssetUtils does: both content extensions,
    // because a UWorld's package is .umap and probing only .uasset reports a perfectly present
    // level as absent. Returns false when the package name has no registered mount root —
    // TryConvert, never LongPackageNameToFilename, which is UE_LOG(Fatal) on an unmounted root.
    bool PwDiskStateResolvePackageFilename(const FString& PackageName, FString& OutFilename,
                                           bool& bOutExists)
    {
        OutFilename.Reset();
        bOutExists = false;

        const FString Extensions[] = {
            FPackageName::GetAssetPackageExtension(),
            FPackageName::GetMapPackageExtension()
        };

        bool bAnyResolved = false;
        for (const FString& Extension : Extensions)
        {
            FString Candidate;
            if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, Candidate, Extension))
            {
                continue;
            }
            if (!bAnyResolved)
            {
                // Remember the first resolvable spelling so a refusal can still name a path when
                // neither extension is present on disk.
                OutFilename = Candidate;
                bAnyResolved = true;
            }
            if (IFileManager::Get().FileSize(*Candidate) >= 0)
            {
                OutFilename = Candidate;
                bOutExists = true;
                return true;
            }
        }
        return bAnyResolved;
    }

    // Read the saved hash out of the file's own FPackageFileSummary. This is the value
    // UPackage::Save embedded when it wrote the file (SavePackage2.cpp writes it back over the
    // placeholder at FPackageFileSummary::GetSavedHashRelativeOffset), and the value
    // FLinkerLoad hands to SetSavedHash on load — so reading it here compares like with like.
    //
    // Deserializing the summary rather than seeking to the fixed offset is deliberate: the offset
    // is a function of the legacy file version, and the operator<< is the same one the linker
    // uses, so a format change moves both together. The read is bounded — the summary is the head
    // of the file and the archive is buffered.
    bool PwDiskStateReadSummarySavedHash(const FString& Filename, FIoHash& OutHash,
                                         FString& OutFailureReason)
    {
        TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename));
        if (!Reader)
        {
            OutFailureReason = TEXT("the .uasset could not be opened for reading");
            return false;
        }

        FPackageFileSummary Summary;
        *Reader << Summary;

        if (Reader->IsError() || Summary.Tag != PACKAGE_FILE_TAG)
        {
            // A truncated file, a mid-write file, or a text-format package (whose header is not
            // this binary summary). None of those is a measurement, so say so instead of
            // inventing a hash and refusing a save over it.
            OutFailureReason = TEXT("the file header is not a readable binary package summary");
            return false;
        }

        OutHash = Summary.GetSavedHash();
        if (OutHash.IsZero())
        {
            OutFailureReason = TEXT("the .uasset on disk carries no saved hash");
            return false;
        }
        return true;
    }
}
#endif

bool ProbePackageDiskDivergence(UPackage* Package, FPackageDiskDivergence& OutState)
{
    OutState = FPackageDiskDivergence();

    if (!Package)
    {
        OutState.UnprobedReason = TEXT("no package");
        return false;
    }
    OutState.PackageName = Package->GetName();

#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    // UPackage::GetSavedHash / FPackageFileSummary::GetSavedHash arrived in UE 5.4. On 5.3 there
    // is no ledger to read and no honest substitute, so the probe is not made at all — never a
    // fabricated "not diverged", and never a refusal the host cannot justify.
    OutState.UnprobedReason = TEXT("the package saved-hash ledger requires UE 5.4 or newer");
    return false;
#else
    if (Package == GetTransientPackage() || Package->HasAnyFlags(RF_Transient))
    {
        OutState.UnprobedReason = TEXT("the package is transient and has no file");
        return false;
    }

    const FIoHash LoadedHash = Package->GetSavedHash();
    if (LoadedHash.IsZero())
    {
        // Never read from or written to a file by this session: there is nothing to compare
        // against. Every CreatePackage + factory create flow lands here, which is why a
        // create-and-save over an existing path is not this guard's refusal to make.
        OutState.UnprobedReason =
            TEXT("the package has never been read from or written to a file, so it has no disk ledger");
        return false;
    }
    OutState.LoadedSavedHash = LexToString(LoadedHash);

    if (!PwDiskStateResolvePackageFilename(OutState.PackageName, OutState.Filename, OutState.bFileExists))
    {
        OutState.UnprobedReason = TEXT("the package name has no registered mount root");
        return false;
    }

    if (!OutState.bFileExists)
    {
        // The file is gone (a revert of an add, an out-of-band delete). The resident package is
        // the only surviving copy, so writing it back destroys nothing. Measured, reported, and
        // deliberately not a refusal.
        OutState.bProbed = true;
        OutState.bDiverged = false;
        return false;
    }

    IFileManager& FileManager = IFileManager::Get();
    OutState.DiskSizeBytes = FMath::Max<int64>(FileManager.FileSize(*OutState.Filename), 0);
    OutState.DiskModifiedTime = FileManager.GetTimeStamp(*OutState.Filename);

    FIoHash DiskHash;
    FString FailureReason;
    if (!PwDiskStateReadSummarySavedHash(OutState.Filename, DiskHash, FailureReason))
    {
        OutState.UnprobedReason = FailureReason;
        return false;
    }
    OutState.DiskSavedHash = LexToString(DiskHash);

    OutState.bProbed = true;
    OutState.bDiverged = DiskHash != LoadedHash;

    if (OutState.bDiverged)
    {
        // Warning, not Error: this is a refusal signal about the working tree, not a fault in
        // the editor. Values, not adjectives, so the log alone settles which side moved.
        UE_LOG(LogPinWrightSubsystem, Warning,
               TEXT("Package disk state diverged for '%s': the file was written by something "
                    "other than this package since it was loaded or last saved. "
                    "diskSavedHash=%s loadedSavedHash=%s diskSizeBytes=%lld diskModified=%s file='%s'"),
               *OutState.PackageName, *OutState.DiskSavedHash, *OutState.LoadedSavedHash,
               OutState.DiskSizeBytes, *OutState.DiskModifiedTime.ToIso8601(), *OutState.Filename);
    }

    return OutState.bDiverged;
#endif
}

void AddPackageDiskStateJson(const TSharedPtr<FJsonObject>& Result,
                             const FPackageDiskDivergence& State)
{
    if (!Result)
    {
        return;
    }

    TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
    Block->SetBoolField(TEXT("probed"), State.bProbed);

    if (!State.bProbed)
    {
        // No measured field is emitted here, on purpose. A caller must be able to tell "the
        // comparison was not made" from "the comparison was made and they matched"; publishing
        // diverged:false for an unprobed package would erase exactly that difference.
        Block->SetStringField(TEXT("reason"), State.UnprobedReason);
        Result->SetObjectField(TEXT("diskState"), Block);
        return;
    }

    Block->SetBoolField(TEXT("diverged"), State.bDiverged);
    Block->SetStringField(TEXT("file"), State.Filename);
    Block->SetBoolField(TEXT("fileExists"), State.bFileExists);
    // What the RESIDENT package believes it last read or wrote. Always present once probed.
    Block->SetStringField(TEXT("loadedSavedHash"), State.LoadedSavedHash);

    if (State.bFileExists)
    {
        // What is ACTUALLY on disk, read out of the file by this call.
        Block->SetStringField(TEXT("diskSavedHash"), State.DiskSavedHash);
        Block->SetNumberField(TEXT("diskSizeBytes"), static_cast<double>(State.DiskSizeBytes));
        Block->SetStringField(TEXT("diskModified"), State.DiskModifiedTime.ToIso8601());
    }

    Result->SetObjectField(TEXT("diskState"), Block);
}

FString DescribePackageDiskDivergence(const FPackageDiskDivergence& State)
{
    if (!State.bProbed || !State.bDiverged)
    {
        return FString();
    }
    return FString::Printf(
        TEXT("The .uasset on disk changed since '%s' was loaded or last saved, so writing the "
             "in-memory package would discard it. File '%s' (%lld bytes, modified %s) carries "
             "savedHash %s; the resident package last read or wrote savedHash %s. Re-read the "
             "file with asset.reload (or source_control.revert, which resynchronizes) and redo "
             "the edit, or re-issue this save with overwriteDiskChanges:true to discard what is "
             "on disk deliberately."),
        *State.PackageName, *State.Filename, State.DiskSizeBytes,
        *State.DiskModifiedTime.ToIso8601(), *State.DiskSavedHash, *State.LoadedSavedHash);
}
}
