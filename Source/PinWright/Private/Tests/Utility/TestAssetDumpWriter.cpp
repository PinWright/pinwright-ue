// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Utils/AssetDumpWriter.h"
#include "PinWrightProjectSettings.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"


// ============================================================================
// AssetDumpWriter.ResolveDumpDir
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWriterResolveDumpDirTest,
    "PinWright.utils.asset_dump_writer.ResolveDumpDir",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWriterResolveDumpDirTest::RunTest(const FString& Parameters)
{
    // Default root: should produce <ProjectSavedDir>/PinWright/asset-dumps/Game/UI/WBP_HUD.
    // Clear the configurable root override so the assertion tests the built-in default
    // and not whatever AssetDumpRootDirectory is set in the project settings.
    {
        UPinWrightProjectSettings* Settings = GetMutableDefault<UPinWrightProjectSettings>();
        const FString SavedRoot = Settings ? Settings->AssetDumpRootDirectory : FString();
        if (Settings)
        {
            Settings->AssetDumpRootDirectory.Empty();
        }

        FString Result = AssetDumpWriter::ResolveDumpDir(TEXT("/Game/UI/WBP_HUD"), FString());
        FString ExpectedSuffix = TEXT("Saved/PinWright/asset-dumps/Game/UI/WBP_HUD");
        TestTrue(TEXT("Default root ends with expected suffix"),
            Result.EndsWith(ExpectedSuffix));
        TestTrue(TEXT("Default root is absolute (contains drive or starts with /)"),
            FPaths::IsRelative(Result) == false);
        TestTrue(TEXT("Default root uses forward slashes"),
            !Result.Contains(TEXT("\\")));

        if (Settings)
        {
            Settings->AssetDumpRootDirectory = SavedRoot;
        }
    }

    // Custom root override
    {
        FString CustomRoot = TEXT("C:/MyDumps");
        FString Result = AssetDumpWriter::ResolveDumpDir(TEXT("/Game/UI/WBP_HUD"), CustomRoot);
        TestTrue(TEXT("Custom root contains the custom prefix"),
            Result.StartsWith(TEXT("C:/MyDumps")));
        TestTrue(TEXT("Custom root ends with package path"),
            Result.EndsWith(TEXT("Game/UI/WBP_HUD")));
    }

    // /Engine/... path produces Engine/... (no leading slash) under the root
    {
        FString Result = AssetDumpWriter::ResolveDumpDir(TEXT("/Engine/Foo/Bar"), TEXT("C:/Root"));
        TestTrue(TEXT("Engine path ends with Engine/Foo/Bar"),
            Result.EndsWith(TEXT("Engine/Foo/Bar")));
        // Must NOT have a double-slash between root and relative segment
        TestFalse(TEXT("No double-slash in resolved engine path"),
            Result.Contains(TEXT("//")));
    }

    // Relative custom root resolves against the project dir, not the process
    // CWD (the editor's CWD is the engine Binaries folder — a CWD-relative
    // root would silently dump into the engine installation).
    {
        FString Result = AssetDumpWriter::ResolveDumpDir(TEXT("/Game/UI/WBP_HUD"), TEXT("asset-dumps"));
        FString ExpectedPrefix = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("asset-dumps"));
        FPaths::NormalizeDirectoryName(ExpectedPrefix);
        TestTrue(TEXT("Relative outRoot resolves under the project dir"),
            Result.StartsWith(ExpectedPrefix + TEXT("/")));
        TestTrue(TEXT("Relative outRoot result ends with package path"),
            Result.EndsWith(TEXT("Game/UI/WBP_HUD")));
    }

    return true;
}

// ============================================================================
// AssetDumpWriter.ResolveDumpRoot
// The relative-root rule is the documented contract: a relative outRoot is
// PROJECT-relative. What must not depend on anything else is where the join
// starts from — ProjectDir() is a "../../../.." path relative to the executable
// on hosts whose project sits outside the engine tree, so the join base is made
// absolute first and the result is always absolute.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWriterResolveDumpRootTest,
    "PinWright.utils.asset_dump_writer.ResolveDumpRoot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWriterResolveDumpRootTest::RunTest(const FString& Parameters)
{
    FString AbsProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    FPaths::NormalizeDirectoryName(AbsProjectDir);

    // Relative root: resolves under the ABSOLUTE project dir.
    {
        const FString Result = AssetDumpWriter::ResolveDumpRoot(TEXT("asset-dumps"));
        TestEqual(TEXT("Relative root resolves under the absolute project dir"),
            Result, AbsProjectDir / TEXT("asset-dumps"));
        TestFalse(TEXT("Relative root resolves to an absolute path"),
            FPaths::IsRelative(Result));
    }

    // Absolute root: passed through unchanged (normalization aside).
    {
        const FString Absolute = AbsProjectDir / TEXT("Intermediate/ResolveDumpRootTest");
        const FString Result = AssetDumpWriter::ResolveDumpRoot(Absolute);
        TestEqual(TEXT("Absolute root is untouched"), Result, Absolute);
    }

    // Failure direction: a root that walks UP must keep its segments and must not
    // collapse toward the filesystem root. A second project-dir prefix on an
    // already-BaseDir-relative root is exactly what produced "/src/..." instead of
    // "<mount>/src/..." on Linux hosts.
    {
        const FString Result = AssetDumpWriter::ResolveDumpRoot(TEXT("../sibling-dumps"));
        FString ExpectedParent = FPaths::GetPath(AbsProjectDir);
        TestEqual(TEXT("'../' root resolves beside the project, not at the filesystem root"),
            Result, ExpectedParent / TEXT("sibling-dumps"));
        TestTrue(TEXT("'../' root keeps the path segments above the project"),
            Result.StartsWith(ExpectedParent + TEXT("/")) && ExpectedParent.Len() > 1);
    }

    return true;
}

// ============================================================================
// AssetDumpWriter.ResolveDiffDir
// Diff output lives under <ProjectSavedDir>/PinWright/asset-dump-diffs and is
// independent of the mirror root override.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWriterResolveDiffDirTest,
    "PinWright.utils.asset_dump_writer.ResolveDiffDir",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWriterResolveDiffDirTest::RunTest(const FString& Parameters)
{
    FString Result = AssetDumpWriter::ResolveDiffDir(TEXT("/Game/UI/WBP_HUD"));
    TestTrue(TEXT("Diff dir ends with expected suffix"),
        Result.EndsWith(TEXT("Saved/PinWright/asset-dump-diffs/Game/UI/WBP_HUD")));
    TestFalse(TEXT("Diff dir is absolute"), FPaths::IsRelative(Result));
    TestFalse(TEXT("No double-slash in resolved diff dir"), Result.Contains(TEXT("//")));

    FString Root = AssetDumpWriter::ResolveDiffRoot();
    TestTrue(TEXT("Diff dir is a strict subpath of the diff root"),
        Result.StartsWith(Root + TEXT("/")));

    return true;
}

// ============================================================================
// AssetDumpWriter.RejectLongPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWriterRejectLongPathTest,
    "PinWright.utils.asset_dump_writer.RejectLongPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWriterRejectLongPathTest::RunTest(const FString& Parameters)
{
    // Build a path well over 260 characters.
    FString LongPath = TEXT("C:/");
    LongPath += FString::ChrN(270, TEXT('A'));

    FString OutError;
    const bool bOk = AssetDumpWriter::CheckPathLength(LongPath, OutError);
    TestFalse(TEXT("CheckPathLength returns false for >260 char path"), bOk);
    TestTrue(TEXT("OutError is non-empty"), !OutError.IsEmpty());
    TestTrue(TEXT("OutError contains the offending path"), OutError.Contains(LongPath));

    // A short path should pass.
    FString ShortPath = TEXT("C:/short/path");
    FString NoError;
    const bool bShortOk = AssetDumpWriter::CheckPathLength(ShortPath, NoError);
    TestTrue(TEXT("CheckPathLength returns true for short path"), bShortOk);
    TestTrue(TEXT("No error for short path"), NoError.IsEmpty());

    return true;
}

// ============================================================================
// AssetDumpWriter.PurgeAndWrite
// Verifies stale files are removed only after the complete intended baseline is
// available, unchanged files retain their mtimes, and cache bookkeeping survives.
// Also covers TmpRenameClean (no .tmp files left after success).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWriterPurgeAndWriteTest,
    "PinWright.utils.asset_dump_writer.PurgeAndWrite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWriterPurgeAndWriteTest::RunTest(const FString& Parameters)
{
    // Create a unique temp dir under ProjectIntermediateDir so tests don't collide.
    FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()) / TEXT("AssetDumpTests");
    FString Guid = FGuid::NewGuid().ToString();
    FString DumpDir = TestRoot / Guid / TEXT("MyAsset");

    IFileManager& FM = IFileManager::Get();
    FM.MakeDirectory(*DumpDir, /*Tree=*/true);

    // Pre-populate with stale files.
    FFileHelper::SaveStringToFile(TEXT("stale"), *(DumpDir / TEXT("tree.xml")),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FFileHelper::SaveStringToFile(TEXT("stale"), *(DumpDir / TEXT("old.json")),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    const FString MetaPath = DumpDir / TEXT("meta.json");
    FFileHelper::SaveStringToFile(TEXT("A"), *MetaPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    const FDateTime StableTimestamp(2001, 2, 3, 4, 5, 6);
    FM.SetTimeStamp(*MetaPath, StableTimestamp);
    FFileHelper::SaveStringToFile(TEXT("cache-sentinel"),
        *(DumpDir / TEXT(".dumpcache.json")),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    TArray<AssetDumpWriter::FDumpFile> Files;
    Files.Add({TEXT("meta.json"),       TEXT("A")});
    Files.Add({TEXT("properties.json"), TEXT("B")});

    FString DumpRoot = TestRoot / Guid;
    FString OutError;
    AssetDumpWriter::FWriteResult WriteResult =
        AssetDumpWriter::WriteAssetDump(DumpDir, DumpRoot, Files, OutError);

    TestTrue(TEXT("OutError is empty on success"), OutError.IsEmpty());
    TestEqual(TEXT("WrittenPaths count"), WriteResult.WrittenPaths.Num(), 2);
    TestTrue(TEXT("Errors array is empty"), WriteResult.Errors.Num() == 0);

    // Stale files must be gone.
    TestFalse(TEXT("tree.xml (stale) is gone"), FM.FileExists(*(DumpDir / TEXT("tree.xml"))));
    TestFalse(TEXT("old.json (stale) is gone"), FM.FileExists(*(DumpDir / TEXT("old.json"))));
    TestTrue(TEXT(".dumpcache.json is preserved"),
        FM.FileExists(*(DumpDir / TEXT(".dumpcache.json"))));

    // New files must exist with correct content.
    FString MetaContent;
    FFileHelper::LoadFileToString(MetaContent, *MetaPath);
    TestEqual(TEXT("meta.json content"), MetaContent, FString(TEXT("A")));
    TestEqual(TEXT("unchanged meta.json keeps its timestamp"),
        FM.GetTimeStamp(*MetaPath), StableTimestamp);

    FString CacheContent;
    FFileHelper::LoadFileToString(CacheContent, *(DumpDir / TEXT(".dumpcache.json")));
    TestEqual(TEXT(".dumpcache.json content is untouched"),
        CacheContent, FString(TEXT("cache-sentinel")));

    FString PropsContent;
    FFileHelper::LoadFileToString(PropsContent, *(DumpDir / TEXT("properties.json")));
    TestEqual(TEXT("properties.json content"), PropsContent, FString(TEXT("B")));

    // WrittenPaths must be absolute.
    for (const FString& Path : WriteResult.WrittenPaths)
    {
        TestFalse(TEXT("WrittenPath is absolute (not relative)"), FPaths::IsRelative(Path));
    }

    // No .tmp files left after successful write (TmpRenameClean).
    TArray<FString> TmpFiles;
    FM.FindFiles(TmpFiles, *(DumpDir / TEXT("*.tmp")), /*Files=*/true, /*Dirs=*/false);
    TestEqual(TEXT("No .tmp files remain"), TmpFiles.Num(), 0);

    // …and no transaction DIRECTORIES either. WriteAssetDump stages into
    // StageRoot = AbsDir + ".tmp-<guid>" and BackupRoot = AbsDir + ".bak-<guid>"
    // (AssetDumpWriter.cpp:422-424) — those are SIBLINGS of DumpDir inside DumpRoot, not
    // files inside DumpDir, so the in-dir scan above can never see them. This enumeration
    // of DumpRoot's direct child directories is what pins the success-path cleanup at
    // AssetDumpWriter.cpp:593-594 (CleanupTransactionDirectory on StageRoot and BackupRoot).
    TArray<FString> RootChildDirs;
    FM.FindFiles(RootChildDirs, *(DumpRoot / TEXT("*")), /*Files=*/false, /*Dirs=*/true);
    FString TransactionResidue;
    int32 TransactionResidueCount = 0;
    for (const FString& ChildDir : RootChildDirs)
    {
        if (ChildDir.Contains(TEXT(".tmp-")) || ChildDir.Contains(TEXT(".bak-")))
        {
            ++TransactionResidueCount;
            if (!TransactionResidue.IsEmpty())
            {
                TransactionResidue += TEXT(", ");
            }
            TransactionResidue += ChildDir;
        }
    }
    TestEqual(FString::Printf(
                  TEXT("No .tmp-/.bak- transaction dirs remain beside the dump dir (found: %s)"),
                  *TransactionResidue),
        TransactionResidueCount, 0);

    // Cleanup.
    FM.DeleteDirectory(*(TestRoot / Guid), /*RequireExists=*/false, /*Tree=*/true);

    return true;
}

// ============================================================================
// AssetDumpWriter.PruneSkipsNestedAssetDumpDirs
// Asset /P/X and a sibling folder /P/X/ share one mirror dir, so the folder's
// assets dump INSIDE X's dump dir. X's prune must leave those nested dump dirs
// alone (board B-asset-dump-dir-nested-inside-sibling-asset-dir), while a
// subdirectory with no dump marker is still X's own stale output and is pruned.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWriterPruneSkipsNestedAssetDumpDirsTest,
    "PinWright.utils.asset_dump_writer.PruneSkipsNestedAssetDumpDirs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWriterPruneSkipsNestedAssetDumpDirsTest::RunTest(const FString& Parameters)
{
    const FString DumpRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("AssetDumpTests") / FGuid::NewGuid().ToString();
    const FString DumpDir = DumpRoot / TEXT("Game/Fonts/X");
    const FString ChildDir = DumpDir / TEXT("X_Regular");
    const FString MarkerOnlyChildDir = DumpDir / TEXT("X_Bold");
    const FString StaleSubDir = DumpDir / TEXT("stale_aspect");

    auto Write = [](const FString& Path)
    {
        FFileHelper::SaveStringToFile(TEXT("{}"), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    };
    IFileManager& FM = IFileManager::Get();
    FM.MakeDirectory(*ChildDir, /*Tree=*/true);
    FM.MakeDirectory(*MarkerOnlyChildDir, /*Tree=*/true);
    FM.MakeDirectory(*StaleSubDir, /*Tree=*/true);
    Write(DumpDir / TEXT("old.json"));
    Write(StaleSubDir / TEXT("old.json"));
    Write(ChildDir / TEXT("meta.json"));
    Write(ChildDir / TEXT("properties.json"));
    Write(ChildDir / TEXT(".dumpcache.json"));
    // A child whose sidecars are gone but whose cache record survives (the state the
    // pre-fix prune left behind) is still another asset's dir.
    Write(MarkerOnlyChildDir / TEXT(".dumpcache.json"));

    TArray<AssetDumpWriter::FDumpFile> Files;
    Files.Add({TEXT("meta.json"), TEXT("A")});
    Files.Add({TEXT("properties.json"), TEXT("B")});
    FString OutError;
    AssetDumpWriter::WriteAssetDump(DumpDir, DumpRoot, Files, OutError);

    TestTrue(TEXT("No error"), OutError.IsEmpty());
    TestTrue(TEXT("Nested child meta.json survives the parent's prune"),
        FM.FileExists(*(ChildDir / TEXT("meta.json"))));
    TestTrue(TEXT("Nested child properties.json survives the parent's prune"),
        FM.FileExists(*(ChildDir / TEXT("properties.json"))));
    TestTrue(TEXT("Nested child .dumpcache.json survives"),
        FM.FileExists(*(ChildDir / TEXT(".dumpcache.json"))));
    TestTrue(TEXT("Marker-only nested child dir survives"),
        FM.FileExists(*(MarkerOnlyChildDir / TEXT(".dumpcache.json"))));
    TestFalse(TEXT("Parent's own stale top-level file is pruned"),
        FM.FileExists(*(DumpDir / TEXT("old.json"))));
    TestFalse(TEXT("Parent's own stale file in an unmarked subdir is pruned"),
        FM.FileExists(*(StaleSubDir / TEXT("old.json"))));

    FM.DeleteDirectory(*DumpRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWriterBinaryUnchangedMtimeTest,
    "PinWright.utils.asset_dump_writer.BinaryUnchangedMtime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWriterBinaryUnchangedMtimeTest::RunTest(const FString& Parameters)
{
    const FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()) / TEXT("AssetDumpTests");
    const FString Guid = FGuid::NewGuid().ToString();
    const FString DumpRoot = TestRoot / Guid;
    const FString DumpDir = DumpRoot / TEXT("MyAsset");
    const FString PreviewPath = DumpDir / TEXT("preview.png");

    IFileManager& FM = IFileManager::Get();
    FM.MakeDirectory(*DumpDir, /*Tree=*/true);
    const TArray<uint8> PreviewBytes = {0x89, 0x50, 0x4e, 0x47, 0x01, 0x02};
    FFileHelper::SaveArrayToFile(PreviewBytes, *PreviewPath);
    const FDateTime StableTimestamp(2002, 3, 4, 5, 6, 8);
    FM.SetTimeStamp(*PreviewPath, StableTimestamp);

    TArray<AssetDumpWriter::FDumpFile> Files;
    Files.Add({TEXT("meta.json"), TEXT("{}")});
    TMap<FString, TArray<uint8>> BinaryFiles;
    BinaryFiles.Add(TEXT("preview.png"), PreviewBytes);

    FString OutError;
    const AssetDumpWriter::FWriteResult Result = AssetDumpWriter::WriteAssetDump(
        DumpDir, DumpRoot, Files, BinaryFiles, OutError);

    TestTrue(TEXT("No error"), OutError.IsEmpty());
    TestEqual(TEXT("Complete sidecar set returned"), Result.WrittenPaths.Num(), 2);
    TestEqual(TEXT("Unchanged binary keeps its timestamp"),
        FM.GetTimeStamp(*PreviewPath), StableTimestamp);

    TArray<uint8> ActualBytes;
    FFileHelper::LoadFileToArray(ActualBytes, *PreviewPath);
    TestTrue(TEXT("Unchanged binary keeps exact bytes"), ActualBytes == PreviewBytes);

    FM.DeleteDirectory(*DumpRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWriterCommitFailureRollsBackTest,
    "PinWright.utils.asset_dump_writer.CommitFailureRollsBack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWriterCommitFailureRollsBackTest::RunTest(const FString& Parameters)
{
    const FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()) / TEXT("AssetDumpTests");
    const FString Guid = FGuid::NewGuid().ToString();
    const FString DumpRoot = TestRoot / Guid;
    const FString DumpDir = DumpRoot / TEXT("MyAsset");

    IFileManager& FM = IFileManager::Get();
    FM.MakeDirectory(*DumpDir, /*Tree=*/true);
    const FString FirstPath = DumpDir / TEXT("a.txt");
    const FString BlockingPath = DumpDir / TEXT("blocked");
    const FString StalePath = DumpDir / TEXT("stale.txt");
    FFileHelper::SaveStringToFile(TEXT("old-a"), *FirstPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FFileHelper::SaveStringToFile(TEXT("blocking-file"), *BlockingPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FFileHelper::SaveStringToFile(TEXT("old-stale"), *StalePath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    // a.txt commits first. The second destination needs `blocked` to be a
    // directory, but the existing file makes that commit fail. The transaction
    // must restore a.txt and must not prune either pre-existing sidecar.
    TArray<AssetDumpWriter::FDumpFile> Files;
    Files.Add({TEXT("a.txt"), TEXT("new-a")});
    Files.Add({TEXT("blocked/child.txt"), TEXT("new-child")});

    FString OutError;
    const AssetDumpWriter::FWriteResult Result =
        AssetDumpWriter::WriteAssetDump(DumpDir, DumpRoot, Files, OutError);

    TestFalse(TEXT("Commit failure reports an error"), OutError.IsEmpty());
    TestTrue(TEXT("Commit failure reports a file error"), Result.Errors.Num() > 0);
    TestEqual(TEXT("Failed transaction returns no successful sidecar set"),
        Result.WrittenPaths.Num(), 0);

    FString Content;
    FFileHelper::LoadFileToString(Content, *FirstPath);
    TestEqual(TEXT("Earlier changed file is rolled back"), Content, FString(TEXT("old-a")));
    FFileHelper::LoadFileToString(Content, *BlockingPath);
    TestEqual(TEXT("Blocking prior sidecar is preserved"),
        Content, FString(TEXT("blocking-file")));
    FFileHelper::LoadFileToString(Content, *StalePath);
    TestEqual(TEXT("Stale sidecar is not pruned after failed writes"),
        Content, FString(TEXT("old-stale")));
    TestFalse(TEXT("Failed child sidecar is absent"),
        FM.FileExists(*(DumpDir / TEXT("blocked/child.txt"))));

    FM.DeleteDirectory(*DumpRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWriterRejectsPathCollisionsTest,
    "PinWright.utils.asset_dump_writer.RejectsPathCollisions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWriterRejectsPathCollisionsTest::RunTest(const FString& Parameters)
{
    const FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()) / TEXT("AssetDumpTests");
    const FString Guid = FGuid::NewGuid().ToString();
    const FString DumpRoot = TestRoot / Guid;
    const FString DumpDir = DumpRoot / TEXT("MyAsset");

    TArray<AssetDumpWriter::FDumpFile> Files;
    Files.Add({TEXT("foo"), TEXT("file")});
    Files.Add({TEXT("foo-bar"), TEXT("sorts between the conflicting paths")});
    Files.Add({TEXT("foo/child.txt"), TEXT("child")});

    FString OutError;
    const AssetDumpWriter::FWriteResult Result =
        AssetDumpWriter::WriteAssetDump(DumpDir, DumpRoot, Files, OutError);

    TestFalse(TEXT("Non-adjacent file/directory collision is rejected"), OutError.IsEmpty());
    TestEqual(TEXT("Rejected collision writes no files"), Result.WrittenPaths.Num(), 0);
    TestFalse(TEXT("Ancestor file was not created"),
        IFileManager::Get().FileExists(*(DumpDir / TEXT("foo"))));

#if PLATFORM_WINDOWS
    Files.Reset();
    Files.Add({TEXT("Case.json"), TEXT("first")});
    Files.Add({TEXT("case.json"), TEXT("second")});
    OutError.Reset();
    const AssetDumpWriter::FWriteResult CaseResult =
        AssetDumpWriter::WriteAssetDump(DumpDir, DumpRoot, Files, OutError);
    TestFalse(TEXT("Windows case-insensitive duplicate is rejected"), OutError.IsEmpty());
    TestEqual(TEXT("Rejected case duplicate writes no files"), CaseResult.WrittenPaths.Num(), 0);
#endif

    IFileManager::Get().DeleteDirectory(*DumpRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// ============================================================================
// AssetDumpWriter.SanityCheckRejectsPurgingRoot
// DumpDir == DumpRoot must be rejected: OutError populated, root untouched.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWriterSanityCheckRejectsPurgingRootTest,
    "PinWright.utils.asset_dump_writer.SanityCheckRejectsPurgingRoot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWriterSanityCheckRejectsPurgingRootTest::RunTest(const FString& Parameters)
{
    FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()) / TEXT("AssetDumpTests");
    FString Guid = FGuid::NewGuid().ToString();
    FString SentinelRoot = TestRoot / Guid;

    IFileManager& FM = IFileManager::Get();
    FM.MakeDirectory(*SentinelRoot, /*Tree=*/true);

    // Place a sentinel file to confirm the root is untouched after the rejected call.
    FString SentinelFile = SentinelRoot / TEXT("sentinel.txt");
    FFileHelper::SaveStringToFile(TEXT("keep"), *SentinelFile,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    TArray<AssetDumpWriter::FDumpFile> Files;
    Files.Add({TEXT("meta.json"), TEXT("X")});

    FString OutError;
    // DumpDir == DumpRoot: strict subpath check must reject.
    AssetDumpWriter::FWriteResult Result =
        AssetDumpWriter::WriteAssetDump(SentinelRoot, SentinelRoot, Files, OutError);

    TestFalse(TEXT("OutError is non-empty on root==dir"), OutError.IsEmpty());
    TestEqual(TEXT("WrittenPaths is empty"), Result.WrittenPaths.Num(), 0);

    // Root must be untouched: sentinel file still exists.
    TestTrue(TEXT("Sentinel file still exists after rejected call"),
        FM.FileExists(*SentinelFile));

    // Cleanup.
    FM.DeleteDirectory(*SentinelRoot, /*RequireExists=*/false, /*Tree=*/true);

    return true;
}

// ============================================================================
// AssetDumpWriter.WriteAssetDumpDiff.IdenticalNoWrites
// One aspect with Old == New writes nothing except meta.json.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWriteAssetDumpDiffIdenticalNoWritesTest,
    "PinWright.utils.asset_dump_writer.WriteAssetDumpDiff.IdenticalNoWrites",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWriteAssetDumpDiffIdenticalNoWritesTest::RunTest(const FString& Parameters)
{
    FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()) / TEXT("AssetDumpTests");
    FString Guid = FGuid::NewGuid().ToString();
    FString DumpDir = TestRoot / Guid / TEXT("MyAsset");
    FString DumpRoot = TestRoot / Guid;

    TArray<AssetDumpWriter::FBaselineDumpFile> Aspects;
    Aspects.Add({TEXT("bpir.txt"), TEXT("same content"), TEXT("same content")});

    FString OutError;
    AssetDumpWriter::FWriteResult Result =
        AssetDumpWriter::WriteAssetDumpDiff(DumpDir, DumpRoot, TEXT("{}"), Aspects, OutError);

    TestTrue(TEXT("No error"), OutError.IsEmpty());

    // meta.json is always written; that's the only written path.
    TestEqual(TEXT("Only meta.json written"), Result.WrittenPaths.Num(), 1);
    TestTrue(TEXT("Written path is meta.json"),
        Result.WrittenPaths.Num() == 1 && Result.WrittenPaths[0].EndsWith(TEXT("meta.json")));

    IFileManager& FM = IFileManager::Get();
    TestFalse(TEXT("bpir_new.txt not created"),
        FM.FileExists(*(DumpDir / TEXT("bpir_new.txt"))));
    TestFalse(TEXT("bpir_diff.txt not created"),
        FM.FileExists(*(DumpDir / TEXT("bpir_diff.txt"))));

    FM.DeleteDirectory(*(TestRoot / Guid), /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// ============================================================================
// AssetDumpWriter.WriteAssetDumpDiff.AspectAdded
// OldContent empty, NewContent non-empty -> _new file + _diff file written.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWriteAssetDumpDiffAspectAddedTest,
    "PinWright.utils.asset_dump_writer.WriteAssetDumpDiff.AspectAdded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWriteAssetDumpDiffAspectAddedTest::RunTest(const FString& Parameters)
{
    FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()) / TEXT("AssetDumpTests");
    FString Guid = FGuid::NewGuid().ToString();
    FString DumpDir = TestRoot / Guid / TEXT("MyAsset");
    FString DumpRoot = TestRoot / Guid;

    TArray<AssetDumpWriter::FBaselineDumpFile> Aspects;
    Aspects.Add({TEXT("properties.json"), TEXT(""), TEXT("line1\nline2\n")});

    FString OutError;
    AssetDumpWriter::FWriteResult Result =
        AssetDumpWriter::WriteAssetDumpDiff(DumpDir, DumpRoot, TEXT("{}"), Aspects, OutError);

    TestTrue(TEXT("No error"), OutError.IsEmpty());

    IFileManager& FM = IFileManager::Get();
    FString NewFilePath  = DumpDir / TEXT("properties_new.json");
    FString DiffFilePath = DumpDir / TEXT("properties_diff.txt");

    TestTrue(TEXT("_new file exists"), FM.FileExists(*NewFilePath));
    TestTrue(TEXT("_diff file exists"), FM.FileExists(*DiffFilePath));

    FString NewContent;
    FFileHelper::LoadFileToString(NewContent, *NewFilePath);
    TestEqual(TEXT("_new file has correct content"), NewContent, FString(TEXT("line1\nline2\n")));

    FString DiffContent;
    FFileHelper::LoadFileToString(DiffContent, *DiffFilePath);
    TestFalse(TEXT("_diff file is non-empty"), DiffContent.IsEmpty());
    TestTrue(TEXT("_diff contains +line1"), DiffContent.Contains(TEXT("+line1")));

    FM.DeleteDirectory(*(TestRoot / Guid), /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWriteAssetDumpDiffRejectsEscapingAspectPathTest,
    "PinWright.utils.asset_dump_writer.WriteAssetDumpDiff.RejectsEscapingAspectPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWriteAssetDumpDiffRejectsEscapingAspectPathTest::RunTest(const FString& Parameters)
{
    FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()) / TEXT("AssetDumpTests");
    FString Guid = FGuid::NewGuid().ToString();
    FString DumpDir = TestRoot / Guid / TEXT("MyAsset");
    FString DumpRoot = TestRoot / Guid;

    TArray<AssetDumpWriter::FBaselineDumpFile> Aspects;
    Aspects.Add({TEXT("actors/../escape.json"), TEXT("old"), TEXT("new")});

    FString OutError;
    AssetDumpWriter::FWriteResult Result =
        AssetDumpWriter::WriteAssetDumpDiff(DumpDir, DumpRoot, TEXT("{}"), Aspects, OutError);

    TestTrue(TEXT("Root-level call still succeeds"), OutError.IsEmpty());
    TestTrue(TEXT("Unsafe aspect is reported"), Result.Errors.Num() > 0);
    TestFalse(TEXT("Escaped _new file is not written"),
        IFileManager::Get().FileExists(*(DumpDir / TEXT("escape_new.json"))));
    TestFalse(TEXT("Escaped _diff file is not written"),
        IFileManager::Get().FileExists(*(DumpDir / TEXT("escape_diff.txt"))));

    IFileManager::Get().DeleteDirectory(*(TestRoot / Guid), /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// ============================================================================
// AssetDumpWriter.WriteAssetDumpDiff.AspectRemoved
// OldContent non-empty, NewContent empty -> no _new file, _diff file written.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWriteAssetDumpDiffAspectRemovedTest,
    "PinWright.utils.asset_dump_writer.WriteAssetDumpDiff.AspectRemoved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWriteAssetDumpDiffAspectRemovedTest::RunTest(const FString& Parameters)
{
    FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()) / TEXT("AssetDumpTests");
    FString Guid = FGuid::NewGuid().ToString();
    FString DumpDir = TestRoot / Guid / TEXT("MyAsset");
    FString DumpRoot = TestRoot / Guid;

    TArray<AssetDumpWriter::FBaselineDumpFile> Aspects;
    Aspects.Add({TEXT("properties.json"), TEXT("line1\nline2\n"), TEXT("")});

    FString OutError;
    AssetDumpWriter::FWriteResult Result =
        AssetDumpWriter::WriteAssetDumpDiff(DumpDir, DumpRoot, TEXT("{}"), Aspects, OutError);

    TestTrue(TEXT("No error"), OutError.IsEmpty());

    IFileManager& FM = IFileManager::Get();
    FString NewFilePath  = DumpDir / TEXT("properties_new.json");
    FString DiffFilePath = DumpDir / TEXT("properties_diff.txt");

    TestFalse(TEXT("_new file NOT created"), FM.FileExists(*NewFilePath));
    TestTrue(TEXT("_diff file exists"), FM.FileExists(*DiffFilePath));

    FString DiffContent;
    FFileHelper::LoadFileToString(DiffContent, *DiffFilePath);
    TestFalse(TEXT("_diff file is non-empty"), DiffContent.IsEmpty());
    TestTrue(TEXT("_diff contains -line1"), DiffContent.Contains(TEXT("-line1")));

    FM.DeleteDirectory(*(TestRoot / Guid), /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// ============================================================================
// AssetDumpWriter.WriteAssetDumpDiff.WipesPreviousArtifacts
// The diff output dir is wiped on every call: stale artifacts from a previous
// diff run are gone; only the current run's files remain.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWriteAssetDumpDiffWipesPreviousArtifactsTest,
    "PinWright.utils.asset_dump_writer.WriteAssetDumpDiff.WipesPreviousArtifacts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWriteAssetDumpDiffWipesPreviousArtifactsTest::RunTest(const FString& Parameters)
{
    FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()) / TEXT("AssetDumpTests");
    FString Guid = FGuid::NewGuid().ToString();
    FString DumpDir = TestRoot / Guid / TEXT("MyAsset");
    FString DumpRoot = TestRoot / Guid;

    IFileManager& FM = IFileManager::Get();
    FM.MakeDirectory(*DumpDir, /*Tree=*/true);

    // Leftover from a hypothetical previous diff run.
    FString StalePath = DumpDir / TEXT("bpir_new.txt");
    FFileHelper::SaveStringToFile(TEXT("stale"), *StalePath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    TArray<AssetDumpWriter::FBaselineDumpFile> Aspects;
    Aspects.Add({TEXT("bpir.txt"), TEXT("same"), TEXT("same")}); // identical -> skipped

    FString OutError;
    AssetDumpWriter::WriteAssetDumpDiff(DumpDir, DumpRoot, TEXT("{}"), Aspects, OutError);

    TestTrue(TEXT("No error"), OutError.IsEmpty());
    TestFalse(TEXT("Stale bpir_new.txt was wiped"), FM.FileExists(*StalePath));
    TestTrue(TEXT("meta.json written after wipe"),
        FM.FileExists(*(DumpDir / TEXT("meta.json"))));

    FM.DeleteDirectory(*(TestRoot / Guid), /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// ============================================================================
// AssetDumpWriter.WriteAssetDumpDiff.OverwritesMetaJson
// Pre-existing meta.json is replaced with the new content.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWriteAssetDumpDiffOverwritesMetaJsonTest,
    "PinWright.utils.asset_dump_writer.WriteAssetDumpDiff.OverwritesMetaJson",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWriteAssetDumpDiffOverwritesMetaJsonTest::RunTest(const FString& Parameters)
{
    FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()) / TEXT("AssetDumpTests");
    FString Guid = FGuid::NewGuid().ToString();
    FString DumpDir = TestRoot / Guid / TEXT("MyAsset");
    FString DumpRoot = TestRoot / Guid;

    IFileManager& FM = IFileManager::Get();
    FM.MakeDirectory(*DumpDir, /*Tree=*/true);

    FString MetaPath = DumpDir / TEXT("meta.json");
    FFileHelper::SaveStringToFile(TEXT("sentinel-meta-content"), *MetaPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    TArray<AssetDumpWriter::FBaselineDumpFile> Aspects;
    FString OutError;
    AssetDumpWriter::WriteAssetDumpDiff(DumpDir, DumpRoot, TEXT("new-meta-content"), Aspects, OutError);

    TestTrue(TEXT("No error"), OutError.IsEmpty());

    FString MetaContent;
    FFileHelper::LoadFileToString(MetaContent, *MetaPath);
    TestEqual(TEXT("meta.json overwritten with new content"), MetaContent,
        FString(TEXT("new-meta-content")));

    FM.DeleteDirectory(*(TestRoot / Guid), /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// ============================================================================
// AssetDumpWriter.WriteAssetDumpDiff.RejectsPurgingRoot
// DumpDir == DumpRoot -> non-empty OutError, empty WrittenPaths, no writes.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWriteAssetDumpDiffRejectsPurgingRootTest,
    "PinWright.utils.asset_dump_writer.WriteAssetDumpDiff.RejectsPurgingRoot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWriteAssetDumpDiffRejectsPurgingRootTest::RunTest(const FString& Parameters)
{
    FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()) / TEXT("AssetDumpTests");
    FString Guid = FGuid::NewGuid().ToString();
    FString SentinelRoot = TestRoot / Guid;

    IFileManager& FM = IFileManager::Get();
    FM.MakeDirectory(*SentinelRoot, /*Tree=*/true);

    FString SentinelFile = SentinelRoot / TEXT("sentinel.txt");
    FFileHelper::SaveStringToFile(TEXT("keep"), *SentinelFile,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    TArray<AssetDumpWriter::FBaselineDumpFile> Aspects;
    FString OutError;
    AssetDumpWriter::FWriteResult Result =
        AssetDumpWriter::WriteAssetDumpDiff(SentinelRoot, SentinelRoot, TEXT("{}"), Aspects, OutError);

    TestFalse(TEXT("OutError is non-empty"), OutError.IsEmpty());
    TestEqual(TEXT("WrittenPaths is empty"), Result.WrittenPaths.Num(), 0);
    TestTrue(TEXT("Sentinel file still exists"), FM.FileExists(*SentinelFile));

    FM.DeleteDirectory(*SentinelRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// ============================================================================
// AssetDumpWriter.EnsureDumpRootScaffold
// Fresh root: .gitignore/.gitattributes/CLAUDE.md/AGENTS.md seeded. Existing
// files are never overwritten (create-if-missing contract, user customizations
// survive).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWriterEnsureDumpRootScaffoldTest,
    "PinWright.utils.asset_dump_writer.EnsureDumpRootScaffold",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWriterEnsureDumpRootScaffoldTest::RunTest(const FString& Parameters)
{
    FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()) / TEXT("AssetDumpTests");
    FString Guid = FGuid::NewGuid().ToString();
    FString DumpRoot = TestRoot / Guid;

    IFileManager& FM = IFileManager::Get();

    // 1. Fresh root: both scaffold files are created with the expected content.
    AssetDumpWriter::EnsureDumpRootScaffold(DumpRoot);

    FString GitignorePath     = DumpRoot / TEXT(".gitignore");
    FString GitattributesPath = DumpRoot / TEXT(".gitattributes");
    FString ClaudeMdPath      = DumpRoot / TEXT("CLAUDE.md");
    FString AgentsMdPath      = DumpRoot / TEXT("AGENTS.md");
    TestTrue(TEXT(".gitignore created"), FM.FileExists(*GitignorePath));
    TestTrue(TEXT(".gitattributes created"), FM.FileExists(*GitattributesPath));
    TestTrue(TEXT("CLAUDE.md created"), FM.FileExists(*ClaudeMdPath));
    TestTrue(TEXT("AGENTS.md created"), FM.FileExists(*AgentsMdPath));

    FString GitignoreContent;
    FFileHelper::LoadFileToString(GitignoreContent, *GitignorePath);
    TestTrue(TEXT(".gitignore lists .dumpcache.json"),
        GitignoreContent.Contains(TEXT(".dumpcache.json")));
    TestTrue(TEXT(".gitignore lists *.tmp"),
        GitignoreContent.Contains(TEXT("*.tmp")));

    FString GitattributesContent;
    FFileHelper::LoadFileToString(GitattributesContent, *GitattributesPath);
    TestTrue(TEXT(".gitattributes disables text normalization"),
        GitattributesContent.Contains(TEXT("* -text")));

    FString ClaudeMdContent;
    FFileHelper::LoadFileToString(ClaudeMdContent, *ClaudeMdPath);
    TestTrue(TEXT("CLAUDE.md describes the dump cache"),
        ClaudeMdContent.Contains(TEXT("# PinWright asset-dump cache")));

    FString AgentsMdContent;
    FFileHelper::LoadFileToString(AgentsMdContent, *AgentsMdPath);
    TestEqual(TEXT("AGENTS.md content is identical to CLAUDE.md"),
        AgentsMdContent, ClaudeMdContent);

    // 2. Create-if-missing: customized files must survive a second call.
    FFileHelper::SaveStringToFile(TEXT("user-customized-sentinel"), *GitignorePath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FFileHelper::SaveStringToFile(TEXT("claude-md-sentinel"), *ClaudeMdPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FFileHelper::SaveStringToFile(TEXT("agents-md-sentinel"), *AgentsMdPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    AssetDumpWriter::EnsureDumpRootScaffold(DumpRoot);

    FString SentinelContent;
    FFileHelper::LoadFileToString(SentinelContent, *GitignorePath);
    TestEqual(TEXT("Customized .gitignore is untouched"), SentinelContent,
        FString(TEXT("user-customized-sentinel")));
    FString ClaudeMdSentinel;
    FFileHelper::LoadFileToString(ClaudeMdSentinel, *ClaudeMdPath);
    TestEqual(TEXT("Customized CLAUDE.md is untouched"), ClaudeMdSentinel,
        FString(TEXT("claude-md-sentinel")));
    FString AgentsMdSentinel;
    FFileHelper::LoadFileToString(AgentsMdSentinel, *AgentsMdPath);
    TestEqual(TEXT("Customized AGENTS.md is untouched"), AgentsMdSentinel,
        FString(TEXT("agents-md-sentinel")));
    TestTrue(TEXT(".gitattributes still exists after second call"),
        FM.FileExists(*GitattributesPath));

    // Cleanup.
    FM.DeleteDirectory(*DumpRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}
