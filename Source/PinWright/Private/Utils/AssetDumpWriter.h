// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

namespace AssetDumpWriter
{
    PINWRIGHT_API FString ResolveDumpRoot(const FString& OutRoot);
    PINWRIGHT_API FString ResolveDumpDir(const FString& PackagePath, const FString& OutRoot);
    // Root and per-asset dir for diff-mode output. Diff artifacts never land in the
    // baseline mirror (which may be committed to git in host projects); they live
    // under <ProjectSavedDir>/PinWright/asset-dump-diffs/<PackagePath>.
    PINWRIGHT_API FString ResolveDiffRoot();
    PINWRIGHT_API FString ResolveDiffDir(const FString& PackagePath);
    PINWRIGHT_API bool CheckPathLength(const FString& AbsDir, FString& OutError);

    // Files under DumpDir that belong to DumpDir's own asset, as absolute normalized
    // paths. A dump dir is NOT a leaf: asset /P/X and a sibling folder /P/X/ map to the
    // same mirror dir, so the folder's assets dump into subdirectories of X's dump dir.
    // Any subdirectory holding a meta.json or .dumpcache.json is another asset's dump
    // dir and is not descended into. Every walk that treats a dump dir's contents as one
    // asset's (freshness, prune, reconcile) must enumerate through this.
    PINWRIGHT_API void FindOwnDumpFiles(const FString& DumpDir, TArray<FString>& OutFiles);

    // Seeds .gitignore / .gitattributes / CLAUDE.md into the dump root so a future
    // `git init` of the dump tree is clean (.dumpcache.json carries volatile fields
    // and must never be committed) and agents reading the tree get usage guidance.
    // Create-if-missing only: existing files are never overwritten, even if their
    // content differs. Write failures are non-fatal.
    PINWRIGHT_API void EnsureDumpRootScaffold(const FString& DumpRoot);

    struct FDumpFile
    {
        FString Name;
        FString Content;
    };

    struct FWriteResult
    {
        TArray<FString> WrittenPaths;
        TArray<TPair<FString, FString>> Errors; // filename -> error message
    };

    PINWRIGHT_API FWriteResult WriteAssetDump(const FString& DumpDir, const FString& DumpRoot,
                                                               const TArray<FDumpFile>& Files, FString& OutError);

    // Transactional baseline writer for assets that also emit binary sidecars.
    // Text and binary files are prepared before any existing mirror file is
    // replaced; stale sidecars are pruned only after every intended write can be
    // committed. Unchanged files are left untouched so their mtimes remain stable.
    // .dumpcache.json is bookkeeping owned by AssetDumpCache and is preserved.
    PINWRIGHT_API FWriteResult WriteAssetDump(
        const FString& DumpDir,
        const FString& DumpRoot,
        const TArray<FDumpFile>& Files,
        const TMap<FString, TArray<uint8>>& BinaryFiles,
        FString& OutError);

    struct FBaselineDumpFile
    {
        FString Name;        // e.g. "bpir.txt"
        FString OldContent;  // loaded from disk; empty when baseline absent
        FString NewContent;  // produced by current run; empty when run omits this aspect
    };

    // Writes diff-mode artifacts (meta.json plus per-changed-aspect <stem>_new<ext>
    // and <stem>_diff.txt) into DumpDir. Callers pass the DIFF output dir/root (see
    // ResolveDiffDir/ResolveDiffRoot), never the baseline mirror — the dir is wiped
    // and recreated on every call so each diff run replaces the previous one.
    PINWRIGHT_API FWriteResult WriteAssetDumpDiff(
        const FString& DumpDir,
        const FString& DumpRoot,
        const FString& LatestMetaJson,
        const TArray<FBaselineDumpFile>& Aspects,
        FString& OutError);

    // Content-aware atomic tmp+move writer for binary outputs (e.g. preview.png).
    // Identical bytes leave the existing file untouched. Prefer the transactional
    // WriteAssetDump overload above for baseline mirrors so binary failures can roll
    // back text changes as one operation. Caller must have created AbsDumpDir.
    //
    // RelName is the destination filename relative to AbsDumpDir; may contain
    // forward-slash subdirectories. Rejected if it escapes AbsDumpDir.
    //
    // Returns false with OutError populated on any I/O failure. Sets OutAbsPath
    // to the final absolute path on success.
    PINWRIGHT_API bool WriteAssetDumpBinaryFile(
        const FString& AbsDumpDir,
        const FString& RelName,
        TArrayView<const uint8> Bytes,
        FString& OutAbsPath,
        FString& OutError);
}
