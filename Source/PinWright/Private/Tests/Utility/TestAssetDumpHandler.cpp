// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpCache.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Asset/AssetDumpHandlerInternal.h"
#include "Utils/AssetDumpWriter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "State/PluginState.h"
#include "State/JobRegistry.h"
#include "State/AsyncFolderDumpState.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Tests/TestUtils.h"
#include "Tests/Utility/AssetDumpMismatchedNameFixture.h"
#include "Tests/TestSkipReporting.h"


#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "UObject/Package.h"
#include "Containers/Ticker.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"

// ============================================================================
// AssetDumpHandler.DumpSingleAsset_BadPath
// Calling DumpSingleAsset with a non-existent path returns an error, no files.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerBadPathTest,
    "PinWright.asset.dump.DumpSingleAsset_BadPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerBadPathTest::RunTest(const FString& Parameters)
{
    FString TestRoot = FPaths::ProjectIntermediateDir() / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(TEXT("/Game/NotReal/DoesNotExist"), TestRoot);

    TestFalse(TEXT("ErrorCode is non-empty for bad path"), Result.ErrorCode.IsEmpty());
    TestEqual(TEXT("WrittenPaths is empty"), Result.WrittenPaths.Num(), 0);

    // No directory should have been created for this asset.
    FString ExpectedDir = TestRoot / TEXT("Game/NotReal/DoesNotExist");
    TestFalse(TEXT("No dump directory created"), IFileManager::Get().DirectoryExists(*ExpectedDir));

    IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
    return true;
}

// ============================================================================
// AssetDumpHandler.DumpSingleAsset_GenericUObject
// Use a reliably-present engine CDO asset path. Fall back to an error-check
// if the path isn't loadable in this config.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerGenericUObjectTest,
    "PinWright.asset.dump.DumpSingleAsset_GenericUObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerGenericUObjectTest::RunTest(const FString& Parameters)
{
    FString TestRoot = FPaths::ProjectIntermediateDir() / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    // Use /Engine/EngineMaterials/WorldGridMaterial — reliably loadable on all UE installs.
    const FString AssetPath = TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");

    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(AssetPath, TestRoot);

    if (!Result.ErrorCode.IsEmpty())
    {
        // Asset not loadable in this project config — skip the positive assertions.
        AddInfo(FString::Printf(TEXT("DumpSingleAsset_GenericUObject: asset not loadable (%s: %s) — test skipped."),
            *Result.ErrorCode, *Result.ErrorMessage));
        IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
        return true;
    }

    TestTrue(TEXT("WrittenPaths is non-empty"), Result.WrittenPaths.Num() > 0);

    // meta.json and properties.json must be present.
    bool bHasMeta = false;
    bool bHasProps = false;
    for (const FString& Path : Result.WrittenPaths)
    {
        if (Path.EndsWith(DumpFileNames::Meta))       bHasMeta  = true;
        if (Path.EndsWith(DumpFileNames::Properties)) bHasProps = true;
    }
    TestTrue(TEXT("meta.json is in writtenPaths"),       bHasMeta);
    TestTrue(TEXT("properties.json is in writtenPaths"), bHasProps);

    // Files must exist on disk.
    for (const FString& Path : Result.WrittenPaths)
    {
        TestTrue(FString::Printf(TEXT("File exists on disk: %s"), *Path),
            IFileManager::Get().FileExists(*Path));
    }

    // Cleanup.
    IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
    return true;
}

// ============================================================================
// AssetDumpHandler.OverwriteReplacesStaleFiles
// Run DumpSingleAsset twice on the same asset. Between runs, drop a stale file
// into the dump dir. After the second run, stale file must be gone.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerOverwriteReplacesStaleFilesTest,
    "PinWright.asset.dump.OverwriteReplacesStaleFiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerOverwriteReplacesStaleFilesTest::RunTest(const FString& Parameters)
{
    FString TestRoot = FPaths::ProjectIntermediateDir() / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const FString AssetPath = TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");

    // First dump.
    AssetDumpHandler::FDumpSingleResult First =
        AssetDumpHandler::DumpSingleAsset(AssetPath, TestRoot);

    if (!First.ErrorCode.IsEmpty())
    {
        AddInfo(FString::Printf(TEXT("OverwriteReplacesStaleFiles: asset not loadable (%s) — skipped."),
            *First.ErrorCode));
        IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
        return true;
    }

    TestTrue(TEXT("First dump succeeded"), First.WrittenPaths.Num() > 0);

    // Drop a stale file into the dump directory.
    const FString StaleFile = First.DumpDir / TEXT("old_file.txt");
    FFileHelper::SaveStringToFile(TEXT("stale"), *StaleFile,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    TestTrue(TEXT("Stale file created"), IFileManager::Get().FileExists(*StaleFile));

    // Second dump — must purge stale file.
    AssetDumpHandler::FDumpSingleResult Second =
        AssetDumpHandler::DumpSingleAsset(AssetPath, TestRoot);

    TestTrue(TEXT("Second dump succeeded"), Second.ErrorCode.IsEmpty());
    TestFalse(TEXT("Stale file is gone after second dump"),
        IFileManager::Get().FileExists(*StaleFile));

    // meta.json must still be present.
    bool bHasMeta = false;
    for (const FString& Path : Second.WrittenPaths)
    {
        if (Path.EndsWith(DumpFileNames::Meta)) { bHasMeta = true; break; }
    }
    TestTrue(TEXT("meta.json remains after overwrite"), bHasMeta);

    // Cleanup.
    IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
    return true;
}

// ============================================================================
// AssetDumpHandler.RpcRegistration_AssetNotFound
// The asset.dump RPC handler must return a pinned domain error code for a bogus
// path. NOTE the code is ASSET_FILE_MISSING, not the ASSET_NOT_FOUND the test
// name suggests: "/Game/NoSuchAsset/DoesNotExist" is a syntactically VALID long
// package name, so NormalizeAssetPath (AssetUtils.cpp:83-89) accepts it and
// TryResolveAssetPath returns non-empty — the ASSET_NOT_FOUND branch at
// AssetDumpHandler.cpp:2874-2881 is only reachable for a path that fails
// normalization. Control reaches DumpSingleAsset, whose pre-LoadObject existence
// check (AssetDumpHandler.cpp:2092-2098) emits ASSET_FILE_MISSING.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerRpcAssetNotFoundTest,
    "PinWright.asset.dump.RpcRegistration_AssetNotFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerRpcAssetNotFoundTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("asset.dump is registered"), IsRegistered(TEXT("asset.dump")));

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/NoSuchAsset/DoesNotExist"));

    bool bFound = InvokeHandlerWithCapture(TEXT("asset.dump"), Payload, Capture);
    TestTrue(TEXT("Handler found in registry"), bFound);
    TestTrue(TEXT("Response was sent"), Capture.bWasCalled);
    TestFalse(TEXT("Response is an error"), Capture.bSuccess);
    // Equality, not a non-empty check: a non-empty assertion passes on ANY error code,
    // including a regression that swaps the domain code for a generic one.
    TestEqual(TEXT("asset.dump bogus-path error code"),
        Capture.ErrorCode, FString(AssetDumpErrorCodes::AssetFileMissing));

    return true;
}

// ============================================================================
// AssetDumpHandler.FolderReconcileDeletesOrphan
// Create a fake orphan dir with meta.json and a live dir with meta.json.
// Run ReconcileMirrorSubtree. Orphan must be deleted; live dir must survive.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerFolderReconcileDeletesOrphanTest,
    "PinWright.asset.dump.FolderReconcileDeletesOrphan",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerFolderReconcileDeletesOrphanTest::RunTest(const FString& Parameters)
{
    FString TestRoot = FPaths::ProjectIntermediateDir() / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();
    FString SweptRoot = FPaths::ConvertRelativePathToFull(TestRoot / TEXT("Game/Swept"));

    FString OrphanDir = FPaths::ConvertRelativePathToFull(SweptRoot / TEXT("Obsolete/DoesNotExist"));
    FString LiveDir   = FPaths::ConvertRelativePathToFull(SweptRoot / TEXT("Live/Asset1"));

    IFileManager::Get().MakeDirectory(*OrphanDir, /*Tree=*/true);
    IFileManager::Get().MakeDirectory(*LiveDir, /*Tree=*/true);

    FFileHelper::SaveStringToFile(TEXT("{}"), *(OrphanDir / DumpFileNames::Meta),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FFileHelper::SaveStringToFile(TEXT("{}"), *(LiveDir / DumpFileNames::Meta),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    TSet<FString> LiveDirs;
    LiveDirs.Add(LiveDir);

    TArray<FString> Deleted = AssetDumpHandler::ReconcileMirrorSubtree(SweptRoot, LiveDirs);

    TestFalse(TEXT("Orphan dir is deleted"),
        IFileManager::Get().DirectoryExists(*OrphanDir));
    TestTrue(TEXT("Live dir survives"),
        IFileManager::Get().DirectoryExists(*LiveDir));
    TestTrue(TEXT("Deleted list contains orphan path"),
        Deleted.Contains(OrphanDir));

    IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
    return true;
}

// ============================================================================
// AssetDumpHandler.FolderReconcileScopeBoundary
// An in-scope orphan (inside SweptRoot, not in LiveDirs) must be deleted.
// An out-of-scope dir (outside SweptRoot) must not be touched.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerFolderReconcileScopeBoundaryTest,
    "PinWright.asset.dump.FolderReconcileScopeBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerFolderReconcileScopeBoundaryTest::RunTest(const FString& Parameters)
{
    FString TestRoot = FPaths::ProjectIntermediateDir() / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();
    FString SweptRoot     = FPaths::ConvertRelativePathToFull(TestRoot / TEXT("Game/Swept"));
    FString OutOfScope    = FPaths::ConvertRelativePathToFull(TestRoot / TEXT("Game/OtherArea"));

    // Inside SweptRoot: one orphan (not live), one that IS live.
    FString OrphanDir     = FPaths::ConvertRelativePathToFull(SweptRoot / TEXT("Obsolete"));
    FString LiveDir       = FPaths::ConvertRelativePathToFull(SweptRoot / TEXT("StillAlive"));
    // Outside SweptRoot entirely.
    FString OutOfScopeDir = FPaths::ConvertRelativePathToFull(OutOfScope / TEXT("SomeAsset"));

    IFileManager::Get().MakeDirectory(*OrphanDir, /*Tree=*/true);
    IFileManager::Get().MakeDirectory(*LiveDir, /*Tree=*/true);
    IFileManager::Get().MakeDirectory(*OutOfScopeDir, /*Tree=*/true);

    FFileHelper::SaveStringToFile(TEXT("{}"), *(OrphanDir / DumpFileNames::Meta),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FFileHelper::SaveStringToFile(TEXT("{}"), *(LiveDir / DumpFileNames::Meta),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FFileHelper::SaveStringToFile(TEXT("{}"), *(OutOfScopeDir / DumpFileNames::Meta),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    TSet<FString> LiveDirs;
    LiveDirs.Add(LiveDir); // only LiveDir is alive inside SweptRoot
    TArray<FString> Deleted = AssetDumpHandler::ReconcileMirrorSubtree(SweptRoot, LiveDirs);

    // In-scope orphan must be deleted.
    TestFalse(TEXT("Orphan inside sweep is deleted"),
        IFileManager::Get().DirectoryExists(*OrphanDir));
    TestTrue(TEXT("Deleted list contains orphan path"), Deleted.Contains(OrphanDir));

    // In-scope live dir must survive.
    TestTrue(TEXT("Live dir inside sweep survives"),
        IFileManager::Get().DirectoryExists(*LiveDir));

    // Out-of-scope dir must not be touched.
    TestTrue(TEXT("Out-of-scope dir survives untouched"),
        IFileManager::Get().DirectoryExists(*OutOfScopeDir));

    IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
    return true;
}

// ============================================================================
// AssetDumpHandler.LongPathReturnsError
// ResolveDumpDir on a very long package path produces a dir > 260 chars.
// CheckPathLength must return false and include the offending path in OutError.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerLongPathReturnsErrorTest,
    "PinWright.asset.dump.LongPathReturnsError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerLongPathReturnsErrorTest::RunTest(const FString& Parameters)
{
    // Build a package path long enough that the resolved dump dir exceeds 260 chars.
    // We use a fixed custom OutRoot that is itself short, then add a deep package path.
    //
    // MAX_PATH is 260. We need:
    //   OutRoot.Len() + 1 (separator) + PackageRelative.Len() > 260
    //
    // Use a 50-char fixed root and a 215-char package path to guarantee overflow.
    const FString ShortRoot = TEXT("C:/AssetDumpTest");
    FString DeepPackagePath = TEXT("/Game/");
    // Each segment is "LongSegmentName" (15 chars) + "/" (1 char) = 16 chars per level.
    // Add 15 levels to get ~240 chars in the package path alone.
    for (int32 i = 0; i < 15; ++i)
    {
        DeepPackagePath += TEXT("LongSegmentName/");
    }
    DeepPackagePath += TEXT("FinalAsset");

    FString DumpDir = AssetDumpWriter::ResolveDumpDir(DeepPackagePath, ShortRoot);

    // Verify the generated path is actually > 260 chars (our invariant).
    // If the machine has a short ProjectDir and the root doesn't push us over,
    // we still validate the function's contract: false + non-empty error mentioning path.
    if (DumpDir.Len() <= 260)
    {
        // The path didn't exceed MAX_PATH on this configuration — skip the negative assertion.
        // Log so the result is transparent.
        AddInfo(FString::Printf(
            TEXT("LongPathReturnsError: generated path is %d chars (<=260) with this root — "
                 "CheckPathLength positive-path contract only."), DumpDir.Len()));
    }

    FString OutError;
    bool bOk = AssetDumpWriter::CheckPathLength(DumpDir, OutError);

    if (DumpDir.Len() > 260)
    {
        TestFalse(TEXT("CheckPathLength returns false for long path"), bOk);
        TestFalse(TEXT("OutError is non-empty"), OutError.IsEmpty());
        TestTrue(TEXT("OutError contains the offending path"), OutError.Contains(DumpDir));
    }
    else
    {
        // Path is within limit — function must return true.
        TestTrue(TEXT("CheckPathLength returns true for short path"), bOk);
        TestTrue(TEXT("OutError is empty for short path"), OutError.IsEmpty());
    }

    return true;
}

// ============================================================================
// AssetDumpHandler.LoadFailureSkipsViaSingleHelper
// DumpSingleAsset on a non-existent path returns a load-failure error code and
// does NOT create any directory on disk under ScratchRoot.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerLoadFailureSkipsViaSingleHelperTest,
    "PinWright.asset.dump.LoadFailureSkipsViaSingleHelper",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerLoadFailureSkipsViaSingleHelperTest::RunTest(const FString& Parameters)
{
    FString ScratchRoot = FPaths::ProjectIntermediateDir() / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(TEXT("/Game/DefinitelyNotReal/NoAssetHere"), ScratchRoot);

    // Per E-asset-dump-distinguish-file-missing-from-load-failed, paths with no .uasset on
    // disk and no in-memory package report ASSET_FILE_MISSING. Pinned by equality, not a
    // disjunction with ASSET_LOAD_FAILED: the equality is what fails if the pre-LoadObject
    // existence check at AssetDumpHandler.cpp:2092-2098 is removed and control falls through
    // to the LoadObject failure branch at :2100-2103.
    TestEqual(TEXT("ErrorCode is ASSET_FILE_MISSING"),
        Result.ErrorCode, FString(AssetDumpErrorCodes::AssetFileMissing));

    // No file system artifacts must have been created under ScratchRoot.
    TestFalse(TEXT("No directory created under ScratchRoot"),
        IFileManager::Get().DirectoryExists(*ScratchRoot));

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

// ============================================================================
// AssetDumpHandler.FolderReconcileIgnoresUserFiles
// A directory without meta.json must never be deleted by reconciliation.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerFolderReconcileIgnoresUserFilesTest,
    "PinWright.asset.dump.FolderReconcileIgnoresUserFiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerFolderReconcileIgnoresUserFilesTest::RunTest(const FString& Parameters)
{
    FString TestRoot  = FPaths::ProjectIntermediateDir() / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();
    FString SweptRoot = FPaths::ConvertRelativePathToFull(TestRoot / TEXT("Game"));
    FString UserDir   = FPaths::ConvertRelativePathToFull(SweptRoot / TEXT("UserDir"));

    IFileManager::Get().MakeDirectory(*UserDir, /*Tree=*/true);
    FFileHelper::SaveStringToFile(TEXT("hello"), *(UserDir / TEXT("readme.txt")),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    TSet<FString> LiveDirs; // empty — nothing is live
    TArray<FString> Deleted = AssetDumpHandler::ReconcileMirrorSubtree(SweptRoot, LiveDirs);

    TestTrue(TEXT("User readme.txt survives"),
        IFileManager::Get().FileExists(*(UserDir / TEXT("readme.txt"))));
    TestTrue(TEXT("UserDir itself survives"),
        IFileManager::Get().DirectoryExists(*UserDir));
    TestEqual(TEXT("Nothing deleted"), Deleted.Num(), 0);

    IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
    return true;
}

// ============================================================================
// AssetDumpHandler.FolderReconcilePrunesEmptyDirs
// Post-order prune removes intermediate dirs that contain no meta.json and no
// user files, while preserving SweptRoot, live dump dirs, and dirs with files.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerFolderReconcilePrunesEmptyDirsTest,
    "PinWright.asset.dump.FolderReconcilePrunesEmptyDirs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerFolderReconcilePrunesEmptyDirsTest::RunTest(const FString& Parameters)
{
    FString TestRoot  = FPaths::ProjectIntermediateDir() / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();
    FString SweptRoot = FPaths::ConvertRelativePathToFull(TestRoot / TEXT("Game"));
    FString EmptyDir  = FPaths::ConvertRelativePathToFull(SweptRoot / TEXT("Empty"));
    FString NestedDir = FPaths::ConvertRelativePathToFull(EmptyDir / TEXT("Nested"));
    FString HasFileDir = FPaths::ConvertRelativePathToFull(SweptRoot / TEXT("HasFile"));
    FString LiveDir   = FPaths::ConvertRelativePathToFull(SweptRoot / TEXT("Live") / TEXT("Asset1"));

    IFileManager::Get().MakeDirectory(*NestedDir, /*Tree=*/true);
    IFileManager::Get().MakeDirectory(*HasFileDir, /*Tree=*/true);
    IFileManager::Get().MakeDirectory(*LiveDir, /*Tree=*/true);

    FFileHelper::SaveStringToFile(TEXT("note"), *(HasFileDir / TEXT("note.txt")),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FFileHelper::SaveStringToFile(TEXT("{}"), *(LiveDir / TEXT("meta.json")),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    TSet<FString> LiveDirs;
    LiveDirs.Add(LiveDir);

    TArray<FString> Deleted = AssetDumpHandler::ReconcileMirrorSubtree(SweptRoot, LiveDirs);

    TestFalse(TEXT("Empty/Nested pruned"),
        IFileManager::Get().DirectoryExists(*NestedDir));
    TestFalse(TEXT("Empty pruned"),
        IFileManager::Get().DirectoryExists(*EmptyDir));
    TestTrue(TEXT("SweptRoot preserved"),
        IFileManager::Get().DirectoryExists(*SweptRoot));
    TestTrue(TEXT("HasFile preserved"),
        IFileManager::Get().DirectoryExists(*HasFileDir));
    TestTrue(TEXT("LiveDir preserved"),
        IFileManager::Get().DirectoryExists(*LiveDir));
    TestTrue(TEXT("Deleted contains Empty"), Deleted.Contains(EmptyDir));
    TestTrue(TEXT("Deleted contains Empty/Nested"), Deleted.Contains(NestedDir));

    IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
    return true;
}

// ============================================================================
// AssetDumpHandler.DiffMode_NoBaselineErrors
// Calling DumpSingleAsset with bDiff=true when no baseline exists returns
// ASSET_NO_BASELINE and writes nothing to disk.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerDiffModeNoBaselineErrorsTest,
    "PinWright.asset.dump.DiffMode_NoBaselineErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerDiffModeNoBaselineErrorsTest::RunTest(const FString& Parameters)
{
    FString ScratchRoot = FPaths::ProjectIntermediateDir() / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const FString AssetPath = TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");

    // Clear diff-dir leftovers from any earlier run so the absence check below is meaningful.
    const FString DiffDir = AssetDumpWriter::ResolveDiffDir(TEXT("/Engine/EngineMaterials/WorldGridMaterial"));
    IFileManager::Get().DeleteDirectory(*DiffDir, /*RequireExists=*/false, /*Tree=*/true);

    AssetDumpHandler::FDumpSingleResult R =
        AssetDumpHandler::DumpSingleAsset(AssetPath, ScratchRoot, /*bDiff=*/true);

    TestEqual(TEXT("ErrorCode is ASSET_NO_BASELINE"), R.ErrorCode, FString(TEXT("ASSET_NO_BASELINE")));
    TestFalse(TEXT("ErrorMessage is non-empty"), R.ErrorMessage.IsEmpty());
    TestEqual(TEXT("WrittenPaths is empty"), R.WrittenPaths.Num(), 0);

    // The dump dir must not exist, or if it does, must be empty (no files written).
    if (IFileManager::Get().DirectoryExists(*R.DumpDir))
    {
        TArray<FString> FilesInDir;
        IFileManager::Get().FindFilesRecursive(FilesInDir, *R.DumpDir, TEXT("*"), true, false);
        TestEqual(TEXT("No files written in dump dir"), FilesInDir.Num(), 0);
    }

    // The failed diff run must not have created the per-asset diff dir either.
    TestFalse(TEXT("Diff dir not created on ASSET_NO_BASELINE"),
        IFileManager::Get().DirectoryExists(*DiffDir));

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

namespace
{
    template <typename AssetType>
    AssetType* NewPackageBackedTestAsset(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }

        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        return NewObject<AssetType>(
            Package,
            FName(*AssetName),
            RF_Public | RF_Standalone | RF_Transient);
    }

    FString MakeObjectPath(const FString& PackagePath)
    {
        return FString::Printf(
            TEXT("%s.%s"),
            *PackagePath,
            *FPackageName::GetLongPackageAssetName(PackagePath));
    }
}

// ============================================================================
// AssetDumpHandler.WritesMaterialMGIRAspect
// Package-backed materials and material functions write mgir.txt alongside
// the generic meta/properties aspects, and unchanged diff mode keeps MGIR quiet.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerWritesMaterialMGIRAspectTest,
    "PinWright.asset.dump.WritesMaterialMGIRAspect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerWritesMaterialMGIRAspectTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / Suffix;
    const FString MaterialPath = FString::Printf(
        TEXT("/Engine/Transient/M_AssetDumpMGIR_%s"), *Suffix);
    const FString FunctionPath = FString::Printf(
        TEXT("/Engine/Transient/MF_AssetDumpMGIR_%s"), *Suffix);

    UMaterial* Material = NewPackageBackedTestAsset<UMaterial>(MaterialPath);
    UMaterialFunction* Function = NewPackageBackedTestAsset<UMaterialFunction>(FunctionPath);
    TestNotNull(TEXT("Package-backed material created"), Material);
    TestNotNull(TEXT("Package-backed material function created"), Function);
    if (!Material || !Function)
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    AssetDumpHandler::FDumpSingleResult MaterialDump =
        AssetDumpHandler::DumpSingleAsset(MakeObjectPath(MaterialPath), ScratchRoot);
    TestTrue(TEXT("Material dump succeeds"), MaterialDump.ErrorCode.IsEmpty());
    TestTrue(TEXT("Material properties.json exists"),
        IFileManager::Get().FileExists(*(MaterialDump.DumpDir / DumpFileNames::Properties)));

    FString MaterialMGIR;
    TestTrue(TEXT("Material mgir.txt exists"),
        FFileHelper::LoadFileToString(MaterialMGIR, *(MaterialDump.DumpDir / DumpFileNames::Mgir)));
    TestTrue(TEXT("Material MGIR contains material entry"),
        MaterialMGIR.Contains(TEXT("entry material `")));

    AssetDumpHandler::FDumpSingleResult FunctionDump =
        AssetDumpHandler::DumpSingleAsset(MakeObjectPath(FunctionPath), ScratchRoot);
    TestTrue(TEXT("Material function dump succeeds"), FunctionDump.ErrorCode.IsEmpty());
    TestTrue(TEXT("Material function properties.json exists"),
        IFileManager::Get().FileExists(*(FunctionDump.DumpDir / DumpFileNames::Properties)));

    FString FunctionMGIR;
    TestTrue(TEXT("Material function mgir.txt exists"),
        FFileHelper::LoadFileToString(FunctionMGIR, *(FunctionDump.DumpDir / DumpFileNames::Mgir)));
    TestTrue(TEXT("Material function MGIR contains function entry"),
        FunctionMGIR.Contains(TEXT("entry function `")));

    // Diff artifacts land in the per-asset diff dir, never the mirror.
    AssetDumpHandler::FDumpSingleResult MaterialDiff =
        AssetDumpHandler::DumpSingleAsset(MakeObjectPath(MaterialPath), ScratchRoot, /*bDiff=*/true);
    TestTrue(TEXT("Unchanged material diff succeeds"), MaterialDiff.ErrorCode.IsEmpty());
    TestEqual(TEXT("Material diff DumpDir is the per-asset diff dir"),
        MaterialDiff.DumpDir, AssetDumpWriter::ResolveDiffDir(MaterialPath));
    TestFalse(TEXT("Unchanged material diff does not report mgir_new.txt"),
        IFileManager::Get().FileExists(*(MaterialDiff.DumpDir / TEXT("mgir_new.txt"))));
    TestFalse(TEXT("Unchanged material diff does not report mgir_diff.txt"),
        IFileManager::Get().FileExists(*(MaterialDiff.DumpDir / TEXT("mgir_diff.txt"))));

    AssetDumpHandler::FDumpSingleResult FunctionDiff =
        AssetDumpHandler::DumpSingleAsset(MakeObjectPath(FunctionPath), ScratchRoot, /*bDiff=*/true);
    TestTrue(TEXT("Unchanged material function diff succeeds"), FunctionDiff.ErrorCode.IsEmpty());
    TestEqual(TEXT("Material function diff DumpDir is the per-asset diff dir"),
        FunctionDiff.DumpDir, AssetDumpWriter::ResolveDiffDir(FunctionPath));
    TestFalse(TEXT("Unchanged material function diff does not report mgir_new.txt"),
        IFileManager::Get().FileExists(*(FunctionDiff.DumpDir / TEXT("mgir_new.txt"))));
    TestFalse(TEXT("Unchanged material function diff does not report mgir_diff.txt"),
        IFileManager::Get().FileExists(*(FunctionDiff.DumpDir / TEXT("mgir_diff.txt"))));

    IFileManager::Get().DeleteDirectory(*MaterialDiff.DumpDir, false, true);
    IFileManager::Get().DeleteDirectory(*FunctionDiff.DumpDir, false, true);
    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

// ============================================================================
// AssetDumpHandler.DiffMode_IdenticalYieldsNoNewFiles
// Running diff mode immediately after a normal dump (no changes) must not emit
// any _new or _diff files. meta.json is rewritten and appears in WrittenPaths.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerDiffModeIdenticalYieldsNoNewFilesTest,
    "PinWright.asset.dump.DiffMode_IdenticalYieldsNoNewFiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerDiffModeIdenticalYieldsNoNewFilesTest::RunTest(const FString& Parameters)
{
    FString ScratchRoot = FPaths::ProjectIntermediateDir() / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const FString AssetPath = TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");

    // Establish baseline.
    AssetDumpHandler::FDumpSingleResult Baseline =
        AssetDumpHandler::DumpSingleAsset(AssetPath, ScratchRoot, /*bDiff=*/false);

    if (!Baseline.ErrorCode.IsEmpty())
    {
        AddInfo(FString::Printf(TEXT("DiffMode_IdenticalYieldsNoNewFiles: asset not loadable (%s) — skipped."),
            *Baseline.ErrorCode));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    // Snapshot the baseline dir so we can prove the diff run leaves the mirror alone.
    TArray<FString> BaselineFilesBefore;
    IFileManager::Get().FindFilesRecursive(BaselineFilesBefore, *Baseline.DumpDir, TEXT("*"), true, false);
    BaselineFilesBefore.Sort();
    FString BaselineMetaBefore;
    FFileHelper::LoadFileToString(BaselineMetaBefore, *(Baseline.DumpDir / DumpFileNames::Meta));

    // Run diff immediately — nothing has changed.
    AssetDumpHandler::FDumpSingleResult R =
        AssetDumpHandler::DumpSingleAsset(AssetPath, ScratchRoot, /*bDiff=*/true);

    TestTrue(TEXT("Diff run succeeded"), R.ErrorCode.IsEmpty());
    TestEqual(TEXT("Mode is diff"), R.Mode, FString(TEXT("diff")));
    TestEqual(TEXT("Diff DumpDir is the per-asset diff dir"),
        R.DumpDir, AssetDumpWriter::ResolveDiffDir(TEXT("/Engine/EngineMaterials/WorldGridMaterial")));

    // No _new or _diff files should exist in the diff dir.
    TArray<FString> AllFiles;
    IFileManager::Get().FindFilesRecursive(AllFiles, *R.DumpDir, TEXT("*"), true, false);
    for (const FString& FilePath : AllFiles)
    {
        FString FileName = FPaths::GetCleanFilename(FilePath);
        TestFalse(FString::Printf(TEXT("No _new file: %s"), *FileName),
            FileName.Contains(TEXT("_new")));
        TestFalse(FString::Printf(TEXT("No _diff file: %s"), *FileName),
            FileName.Contains(TEXT("_diff")));
    }

    // meta.json must appear in WrittenPaths, land under the diff dir, and exist on disk.
    bool bMetaWritten = false;
    for (const FString& P : R.WrittenPaths)
    {
        if (P.EndsWith(DumpFileNames::Meta))
        {
            bMetaWritten = true;
            TestTrue(TEXT("meta.json written under the diff dir"), P.StartsWith(R.DumpDir));
            break;
        }
    }
    TestTrue(TEXT("meta.json is in WrittenPaths"), bMetaWritten);

    // The baseline mirror is untouched: same file set, same meta.json bytes.
    TArray<FString> BaselineFilesAfter;
    IFileManager::Get().FindFilesRecursive(BaselineFilesAfter, *Baseline.DumpDir, TEXT("*"), true, false);
    BaselineFilesAfter.Sort();
    TestEqual(TEXT("Baseline file set unchanged after diff"),
        FString::Join(BaselineFilesAfter, TEXT("\n")), FString::Join(BaselineFilesBefore, TEXT("\n")));
    FString BaselineMetaAfter;
    FFileHelper::LoadFileToString(BaselineMetaAfter, *(Baseline.DumpDir / DumpFileNames::Meta));
    TestEqual(TEXT("Baseline meta.json bytes unchanged after diff"), BaselineMetaAfter, BaselineMetaBefore);

    IFileManager::Get().DeleteDirectory(*R.DumpDir, false, true);
    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

// ============================================================================
// AssetDumpHandler.DiffMode_ChangedBaselineProducesDiff
// Mutate one canonical file in the baseline, then run diff mode. The mutated
// file must have _new and _diff siblings; unchanged files must not.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerDiffModeChangedBaselineProducesDiffTest,
    "PinWright.asset.dump.DiffMode_ChangedBaselineProducesDiff",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerDiffModeChangedBaselineProducesDiffTest::RunTest(const FString& Parameters)
{
    FString ScratchRoot = FPaths::ProjectIntermediateDir() / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const FString AssetPath = TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");

    // Establish baseline.
    AssetDumpHandler::FDumpSingleResult Baseline =
        AssetDumpHandler::DumpSingleAsset(AssetPath, ScratchRoot, /*bDiff=*/false);

    if (!Baseline.ErrorCode.IsEmpty())
    {
        AddInfo(FString::Printf(TEXT("DiffMode_ChangedBaselineProducesDiff: asset not loadable (%s) — skipped."),
            *Baseline.ErrorCode));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    // Verify properties.json was written (required for this test).
    FString PropertiesPath = Baseline.DumpDir / DumpFileNames::Properties;
    if (!IFileManager::Get().FileExists(*PropertiesPath))
    {
        AddInfo(TEXT("DiffMode_ChangedBaselineProducesDiff: properties.json not present for this asset — skipped."));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    // Overwrite properties.json with garbage to simulate drift.
    FFileHelper::SaveStringToFile(TEXT("SENTINEL\nLINE\n"), *PropertiesPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    // Run diff mode.
    AssetDumpHandler::FDumpSingleResult R =
        AssetDumpHandler::DumpSingleAsset(AssetPath, ScratchRoot, /*bDiff=*/true);

    TestTrue(TEXT("Diff run succeeded"), R.ErrorCode.IsEmpty());
    TestEqual(TEXT("Mode is diff"), R.Mode, FString(TEXT("diff")));
    TestEqual(TEXT("Diff DumpDir is the per-asset diff dir"),
        R.DumpDir, AssetDumpWriter::ResolveDiffDir(TEXT("/Engine/EngineMaterials/WorldGridMaterial")));

    // properties_new.json and properties_diff.txt must exist in the diff dir,
    // never as siblings inside the baseline mirror.
    FString NewPropsPath  = R.DumpDir / TEXT("properties_new.json");
    FString DiffPropsPath = R.DumpDir / TEXT("properties_diff.txt");
    TestTrue(TEXT("properties_new.json exists"), IFileManager::Get().FileExists(*NewPropsPath));
    TestTrue(TEXT("properties_diff.txt exists"), IFileManager::Get().FileExists(*DiffPropsPath));
    TestFalse(TEXT("No properties_new.json in the baseline mirror"),
        IFileManager::Get().FileExists(*(Baseline.DumpDir / TEXT("properties_new.json"))));
    TestFalse(TEXT("No properties_diff.txt in the baseline mirror"),
        IFileManager::Get().FileExists(*(Baseline.DumpDir / TEXT("properties_diff.txt"))));

    // The diff file must be non-empty.
    FString DiffContent;
    FFileHelper::LoadFileToString(DiffContent, *DiffPropsPath);
    TestFalse(TEXT("properties_diff.txt is non-empty"), DiffContent.IsEmpty());

    // The baseline mirror is untouched: the mutated properties.json keeps its bytes.
    FString MirrorProps;
    FFileHelper::LoadFileToString(MirrorProps, *PropertiesPath);
    TestEqual(TEXT("Baseline properties.json bytes unchanged after diff"),
        MirrorProps, FString(TEXT("SENTINEL\nLINE\n")));

    // meta.json must NOT have a _new/_diff sibling (it is always rewritten verbatim).
    TestFalse(TEXT("No meta_new.json"),  IFileManager::Get().FileExists(*(R.DumpDir / TEXT("meta_new.json"))));
    TestFalse(TEXT("No meta_diff.txt"),  IFileManager::Get().FileExists(*(R.DumpDir / TEXT("meta_diff.txt"))));

    IFileManager::Get().DeleteDirectory(*R.DumpDir, false, true);
    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

// ============================================================================
// AssetDumpHandler.NormalModeCleansUpDiffArtifacts
// After a diff run produces _new/_diff files, running normal mode must purge them.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerNormalModeCleansUpDiffArtifactsTest,
    "PinWright.asset.dump.NormalModeCleansUpDiffArtifacts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerNormalModeCleansUpDiffArtifactsTest::RunTest(const FString& Parameters)
{
    FString ScratchRoot = FPaths::ProjectIntermediateDir() / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const FString AssetPath = TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");

    // Step 1: establish baseline.
    AssetDumpHandler::FDumpSingleResult Baseline =
        AssetDumpHandler::DumpSingleAsset(AssetPath, ScratchRoot, /*bDiff=*/false);

    if (!Baseline.ErrorCode.IsEmpty())
    {
        AddInfo(FString::Printf(TEXT("NormalModeCleansUpDiffArtifacts: asset not loadable (%s) — skipped."),
            *Baseline.ErrorCode));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    FString PropertiesPath = Baseline.DumpDir / DumpFileNames::Properties;
    if (!IFileManager::Get().FileExists(*PropertiesPath))
    {
        AddInfo(TEXT("NormalModeCleansUpDiffArtifacts: properties.json not present for this asset — skipped."));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    // Step 2: mutate baseline and run diff to produce _new/_diff artifacts.
    FFileHelper::SaveStringToFile(TEXT("SENTINEL\nLINE\n"), *PropertiesPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    AssetDumpHandler::FDumpSingleResult DiffRun =
        AssetDumpHandler::DumpSingleAsset(AssetPath, ScratchRoot, /*bDiff=*/true);

    if (!DiffRun.ErrorCode.IsEmpty())
    {
        AddInfo(FString::Printf(TEXT("NormalModeCleansUpDiffArtifacts: diff run failed (%s) — skipped."),
            *DiffRun.ErrorCode));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    // Verify the _new/_diff files exist in the diff dir before the normal run.
    FString NewPropsPath  = DiffRun.DumpDir / TEXT("properties_new.json");
    FString DiffPropsPath = DiffRun.DumpDir / TEXT("properties_diff.txt");
    TestTrue(TEXT("properties_new.json exists before normal run"), IFileManager::Get().FileExists(*NewPropsPath));
    TestTrue(TEXT("properties_diff.txt exists before normal run"), IFileManager::Get().FileExists(*DiffPropsPath));

    // Step 3: run normal mode — a fresh baseline deletes the asset's diff dir.
    AssetDumpHandler::FDumpSingleResult Normal =
        AssetDumpHandler::DumpSingleAsset(AssetPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("Normal run succeeded"), Normal.ErrorCode.IsEmpty());

    // The whole diff dir must be gone.
    TestFalse(TEXT("Diff dir gone after normal run"),
        IFileManager::Get().DirectoryExists(*DiffRun.DumpDir));

    // Canonical files must still exist.
    bool bHasMeta = false;
    bool bHasProps = false;
    for (const FString& P : Normal.WrittenPaths)
    {
        if (P.EndsWith(DumpFileNames::Meta))       bHasMeta  = true;
        if (P.EndsWith(DumpFileNames::Properties)) bHasProps = true;
    }
    TestTrue(TEXT("meta.json still present after normal run"),       bHasMeta);
    TestTrue(TEXT("properties.json still present after normal run"), bHasProps);

    // Best-effort: only present when an assertion above failed.
    IFileManager::Get().DeleteDirectory(*DiffRun.DumpDir, false, true);
    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

namespace
{
    TSharedPtr<FJsonObject> LoadJsonObjectFromFile(const FString& Path);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerWorldWritesActorFilesTest,
    "PinWright.asset.dump.WorldWritesActorFiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerWorldWritesActorFilesTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();
    const FString AssetPath = TEXT("/Engine/Maps/Entry.Entry");

    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(AssetPath, ScratchRoot, /*bDiff=*/false);

    if (!Result.ErrorCode.IsEmpty())
    {
        AddInfo(FString::Printf(TEXT("WorldWritesActorFiles: level not loadable (%s) - skipped."),
            *Result.ErrorCode));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    const FString ManifestPath = Result.DumpDir / DumpFileNames::ActorsManifest;
    TestTrue(TEXT("actors/manifest.json exists"), IFileManager::Get().FileExists(*ManifestPath));

    TSharedPtr<FJsonObject> Manifest = LoadJsonObjectFromFile(ManifestPath);
    TestTrue(TEXT("actors/manifest.json parses"), Manifest.IsValid());
    if (!Manifest.IsValid())
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    double EmbeddedActorCount = 0;
    Manifest->TryGetNumberField(TEXT("embeddedActorCount"), EmbeddedActorCount);
    TestTrue(TEXT("Entry level has at least one embedded actor"), EmbeddedActorCount > 0);

    const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
    TestTrue(TEXT("Manifest has actors array"), Manifest->TryGetArrayField(TEXT("actors"), Actors));
    bool bFoundActorFile = false;
    TSharedPtr<FJsonObject> EmbeddedActor;
    if (Actors)
    {
        for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
        {
            TSharedPtr<FJsonObject> ActorObj = ActorValue.IsValid() ? ActorValue->AsObject() : nullptr;
            FString Storage;
            FString File;
            if (ActorObj.IsValid()
                && ActorObj->TryGetStringField(TEXT("storage"), Storage)
                && Storage == TEXT("embedded")
                && ActorObj->TryGetStringField(TEXT("file"), File))
            {
                const FString ActorFilePath = Result.DumpDir / File;
                bFoundActorFile = IFileManager::Get().FileExists(*ActorFilePath);
                if (bFoundActorFile)
                {
                    EmbeddedActor = LoadJsonObjectFromFile(ActorFilePath);
                }
                break;
            }
        }
    }
    TestTrue(TEXT("At least one embedded actor file exists"), bFoundActorFile);
    TestTrue(TEXT("Embedded actor file parses"), EmbeddedActor.IsValid());
    if (EmbeddedActor.IsValid())
    {
        FString Schema;
        FString Storage;
        FString Name;
        FString Path;
        FString ClassPath;
        FString Level;
        const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
        const TSharedPtr<FJsonObject>* Properties = nullptr;

        TestTrue(TEXT("Embedded actor has actor describe schema"),
            EmbeddedActor->TryGetStringField(TEXT("schema"), Schema)
            && Schema == TEXT("pinwright.actor-describe.v1"));
        TestTrue(TEXT("Embedded actor has embedded storage"),
            EmbeddedActor->TryGetStringField(TEXT("storage"), Storage)
            && Storage == TEXT("embedded"));
        TestTrue(TEXT("Embedded actor has name"), EmbeddedActor->TryGetStringField(TEXT("name"), Name));
        TestTrue(TEXT("Embedded actor has path"), EmbeddedActor->TryGetStringField(TEXT("path"), Path));
        TestTrue(TEXT("Embedded actor has class"), EmbeddedActor->TryGetStringField(TEXT("class"), ClassPath));
        TestTrue(TEXT("Embedded actor has level"), EmbeddedActor->TryGetStringField(TEXT("level"), Level));
        TestTrue(TEXT("Embedded actor has properties object"),
            EmbeddedActor->TryGetObjectField(TEXT("properties"), Properties));
        TestTrue(TEXT("Embedded actor has components array"),
            EmbeddedActor->TryGetArrayField(TEXT("components"), Components));
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerWorldActorManifestDiffTest,
    "PinWright.asset.dump.WorldActorManifestDiff",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerWorldActorManifestDiffTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();
    const FString AssetPath = TEXT("/Engine/Maps/Entry.Entry");

    AssetDumpHandler::FDumpSingleResult Baseline =
        AssetDumpHandler::DumpSingleAsset(AssetPath, ScratchRoot, /*bDiff=*/false);

    if (!Baseline.ErrorCode.IsEmpty())
    {
        AddInfo(FString::Printf(TEXT("WorldActorManifestDiff: level not loadable (%s) - skipped."),
            *Baseline.ErrorCode));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    const FString ManifestPath = Baseline.DumpDir / DumpFileNames::ActorsManifest;
    if (!IFileManager::Get().FileExists(*ManifestPath))
    {
        AddInfo(TEXT("WorldActorManifestDiff: actor manifest absent - skipped."));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    TSharedPtr<FJsonObject> Manifest = LoadJsonObjectFromFile(ManifestPath);
    FString ActorFile;
    const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
    if (Manifest.IsValid() && Manifest->TryGetArrayField(TEXT("actors"), Actors))
    {
        for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
        {
            TSharedPtr<FJsonObject> ActorObj = ActorValue.IsValid() ? ActorValue->AsObject() : nullptr;
            FString Storage;
            if (ActorObj.IsValid()
                && ActorObj->TryGetStringField(TEXT("storage"), Storage)
                && Storage == TEXT("embedded")
                && ActorObj->TryGetStringField(TEXT("file"), ActorFile))
            {
                break;
            }
        }
    }
    if (ActorFile.IsEmpty())
    {
        AddInfo(TEXT("WorldActorManifestDiff: no embedded actor file in manifest - skipped."));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    FString ManifestContent;
    FFileHelper::LoadFileToString(ManifestContent, *ManifestPath);
    ManifestContent.InsertAt(1, TEXT("\"sentinel\":true,"));
    FFileHelper::SaveStringToFile(ManifestContent, *ManifestPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FFileHelper::SaveStringToFile(TEXT("{\"actorSentinel\":true}\n"), *(Baseline.DumpDir / ActorFile),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    AssetDumpHandler::FDumpSingleResult Diff =
        AssetDumpHandler::DumpSingleAsset(AssetPath, ScratchRoot, /*bDiff=*/true);

    // Diff artifacts land in the per-asset diff dir; the baseline mirror keeps
    // only the (mutated) canonical files.
    TestTrue(TEXT("Diff run succeeds"), Diff.ErrorCode.IsEmpty());
    TestEqual(TEXT("Diff DumpDir is the per-asset diff dir"),
        Diff.DumpDir, AssetDumpWriter::ResolveDiffDir(TEXT("/Engine/Maps/Entry")));
    TestTrue(TEXT("actors/manifest_new.json exists"),
        IFileManager::Get().FileExists(*(Diff.DumpDir / TEXT("actors/manifest_new.json"))));
    TestTrue(TEXT("actors/manifest_diff.txt exists"),
        IFileManager::Get().FileExists(*(Diff.DumpDir / TEXT("actors/manifest_diff.txt"))));
    TestFalse(TEXT("No actors/manifest_new.json in the baseline mirror"),
        IFileManager::Get().FileExists(*(Baseline.DumpDir / TEXT("actors/manifest_new.json"))));

    const FString ActorDir = FPaths::GetPath(ActorFile);
    const FString ActorBase = FPaths::GetBaseFilename(ActorFile, /*bRemovePath=*/true);
    TestTrue(TEXT("Actor _new file exists"),
        IFileManager::Get().FileExists(*(Diff.DumpDir / ActorDir / (ActorBase + TEXT("_new.json")))));
    TestTrue(TEXT("Actor _diff file exists"),
        IFileManager::Get().FileExists(*(Diff.DumpDir / ActorDir / (ActorBase + TEXT("_diff.txt")))));

    IFileManager::Get().DeleteDirectory(*Diff.DumpDir, false, true);
    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

namespace
{
    TSharedPtr<FJsonObject> LoadJsonObjectFromFile(const FString& Path)
    {
        FString Content;
        if (!FFileHelper::LoadFileToString(Content, *Path))
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> Object;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
        if (!FJsonSerializer::Deserialize(Reader, Object))
        {
            return nullptr;
        }
        return Object;
    }

    // Pumps the core ticker until the folder-dump state goes idle or
    // MaxTicks is reached. Returns the actual tick count (useful for
    // asserting "did not hit the cap"). MaxTicks is intentionally tight
    // because the slow async tests now target a single-asset folder.
    int32 DrainFolderDump(int32 MaxTicks = 64)
    {
        int32 Ticks = 0;
        while (AssetDumpHandler::GetAsyncFolderDumpStatus().bInProgress && Ticks < MaxTicks)
        {
            FTSTicker::GetCoreTicker().Tick(0.01f);
            ++Ticks;
        }
        return Ticks;
    }

    // Folder used by the async kickoff/concurrent tests. /Engine/EngineDamageTypes
    // ships with a single DamageType asset, which keeps the async dump O(1) so
    // these tests stay under a second instead of pumping hundreds of materials.
    constexpr const TCHAR* AsyncDumpTestFolder = TEXT("/Engine/EngineDamageTypes");

    bool SaveJsonObjectToFile(const TSharedPtr<FJsonObject>& Object, const FString& Path)
    {
        if (!Object.IsValid())
        {
            return false;
        }

        FString Content;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Content);
        if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
        {
            return false;
        }

        return FFileHelper::SaveStringToFile(Content, *Path,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }

    TArray<FString> FindFilesNamed(const FString& Root, const FString& LeafName)
    {
        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(
            Files, *Root, *LeafName,
            /*Files=*/true, /*Directories=*/false, /*bClearFileNames=*/true);
        Files.Sort();
        return Files;
    }

    FString AttachFolderDumpJobTicket()
    {
        const TSharedRef<FJsonObject> StartedParams = MakeShared<FJsonObject>();
        const FString JobTicketId =
            FPluginState::Get().GetJobRegistry().Start(TEXT("asset.dump_folder"), StartedParams);
        FPluginState::Get().GetFolderDump().JobTicketId = JobTicketId;
        return JobTicketId;
    }

    bool CompleteCurrentFolderDump(FJobTicket& OutTicket)
    {
        const FString JobTicketId = AttachFolderDumpJobTicket();
        if (JobTicketId.IsEmpty())
        {
            return false;
        }

        DrainFolderDump();
        return FPluginState::Get().GetJobRegistry().Get(JobTicketId, OutTicket)
            && OutTicket.Result.IsValid();
    }

    bool ReadFirstDumpCacheRecord(
        const FString& ScratchRoot,
        FString& OutDumpDir,
        FString& OutCachePath,
        AssetDumpCache::FAssetDumpCacheRecord& OutRecord)
    {
        const TArray<FString> CacheFiles = FindFilesNamed(ScratchRoot, AssetDumpCache::DumpCacheFileName);
        if (CacheFiles.IsEmpty())
        {
            return false;
        }

        OutCachePath = CacheFiles[0];
        OutDumpDir = FPaths::GetPath(OutCachePath);
        FString Error;
        return AssetDumpCache::ReadCacheRecord(OutDumpDir, OutRecord, Error);
    }

    bool StartInvalidatedCacheRun(
        const FString& ScratchRoot,
        AssetDumpHandler::FFolderDumpStart& OutStart,
        FJobTicket& OutTicket)
    {
        OutStart = AssetDumpHandler::StartAsyncFolderDump(
            AsyncDumpTestFolder, /*bRecursive=*/false, ScratchRoot);
        if (!OutStart.ErrorCode.IsEmpty() || OutStart.AssetCount == 0)
        {
            return false;
        }
        return CompleteCurrentFolderDump(OutTicket);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncFolderDumpCacheWritesAndHitsTest,
    "PinWright.asset.dump.AsyncFolderDump.CacheWritesAndHits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncFolderDumpCacheWritesAndHitsTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const AssetDumpHandler::FFolderDumpStart First =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, ScratchRoot);
    TestTrue(TEXT("First start must succeed"), First.ErrorCode.IsEmpty());
    if (First.AssetCount == 0)
    {
        AddInfo(FString::Printf(TEXT("No assets under %s in this configuration; skipping cache test."), AsyncDumpTestFolder));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    TestEqual(TEXT("First start queues every candidate"), First.QueuedCount, First.AssetCount);
    TestEqual(TEXT("First start has no cache hits"), First.UnchangedCount, 0);

    FJobTicket FirstTicket;
    TestTrue(TEXT("First folder dump completes with job result"), CompleteCurrentFolderDump(FirstTicket));
    if (FirstTicket.Result.IsValid())
    {
        TestEqual(TEXT("First result assetCount"), FirstTicket.Result->GetIntegerField(TEXT("assetCount")), First.AssetCount);
        TestEqual(TEXT("First result queued"), FirstTicket.Result->GetIntegerField(TEXT("queued")), First.AssetCount);
        TestEqual(TEXT("First result dumped"), FirstTicket.Result->GetIntegerField(TEXT("dumped")), First.AssetCount);
        TestEqual(TEXT("First result unchanged"), FirstTicket.Result->GetIntegerField(TEXT("unchanged")), 0);
        TestEqual(TEXT("First result skipCount"), FirstTicket.Result->GetIntegerField(TEXT("skipCount")), 0);
    }

    const TArray<FString> MetaFiles = FindFilesNamed(ScratchRoot, DumpFileNames::Meta);
    const TArray<FString> CacheFiles = FindFilesNamed(ScratchRoot, AssetDumpCache::DumpCacheFileName);
    TestEqual(TEXT("First folder dump writes one meta.json per asset"), MetaFiles.Num(), First.AssetCount);
    TestEqual(TEXT("First folder dump writes one .dumpcache.json per asset"), CacheFiles.Num(), First.AssetCount);
    if (CacheFiles.IsEmpty())
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    const FString LiveDumpDir = FPaths::GetPath(CacheFiles[0]);
    const FString OrphanDir = First.RootDir / TEXT("__CacheHitOrphan");
    IFileManager::Get().MakeDirectory(*OrphanDir, /*Tree=*/true);
    FFileHelper::SaveStringToFile(TEXT("{}"), *(OrphanDir / DumpFileNames::Meta),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    const AssetDumpHandler::FFolderDumpStart Second =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, ScratchRoot);
    TestTrue(TEXT("Second start must succeed"), Second.ErrorCode.IsEmpty());
    if (!Second.ErrorCode.IsEmpty())
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }
    TestEqual(TEXT("Second start assetCount"), Second.AssetCount, First.AssetCount);
    TestEqual(TEXT("Second start queues zero stale assets"), Second.QueuedCount, 0);
    TestEqual(TEXT("Second start reports every candidate unchanged"), Second.UnchangedCount, Second.AssetCount);

    FJobTicket SecondTicket;
    TestTrue(TEXT("Second folder dump completes with job result"), CompleteCurrentFolderDump(SecondTicket));
    if (SecondTicket.Result.IsValid())
    {
        const int32 AssetCount = SecondTicket.Result->GetIntegerField(TEXT("assetCount"));
        const int32 Queued = SecondTicket.Result->GetIntegerField(TEXT("queued"));
        const int32 Dumped = SecondTicket.Result->GetIntegerField(TEXT("dumped"));
        const int32 Unchanged = SecondTicket.Result->GetIntegerField(TEXT("unchanged"));
        const int32 SkipCount = SecondTicket.Result->GetIntegerField(TEXT("skipCount"));
        TestEqual(TEXT("Second result assetCount"), AssetCount, Second.AssetCount);
        TestEqual(TEXT("Second result queued"), Queued, 0);
        TestEqual(TEXT("Second result dumped"), Dumped, 0);
        TestEqual(TEXT("Second result unchanged"), Unchanged, Second.AssetCount);
        TestEqual(TEXT("Second result skipCount"), SkipCount, 0);
        TestEqual(TEXT("Second result counter invariant"), AssetCount, Dumped + Unchanged + SkipCount);
    }

    TestTrue(TEXT("Cache-hit dump dir survives reconciliation"),
        IFileManager::Get().DirectoryExists(*LiveDumpDir));
    TestFalse(TEXT("Orphan dump dir is pruned during cache-hit reconciliation"),
        IFileManager::Get().DirectoryExists(*OrphanDir));

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncFolderDumpForceRequeuesFreshCacheTest,
    "PinWright.asset.dump.AsyncFolderDump.ForceRequeuesFreshCache",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncFolderDumpForceRequeuesFreshCacheTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const AssetDumpHandler::FFolderDumpStart Seed =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, ScratchRoot);
    TestTrue(TEXT("Seed start must succeed"), Seed.ErrorCode.IsEmpty());
    if (Seed.AssetCount == 0)
    {
        AddInfo(FString::Printf(TEXT("No assets under %s in this configuration; skipping force cache test."), AsyncDumpTestFolder));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    FJobTicket SeedTicket;
    TestTrue(TEXT("Seed folder dump completes"), CompleteCurrentFolderDump(SeedTicket));
    // Exact count, not > 0: the seed sweep dumps every candidate fresh, so it must write
    // one .dumpcache.json per asset — the same relation already pinned for the first sweep
    // in AsyncFolderDump.CacheWritesAndHits above.
    TestEqual(TEXT("Seed writes one .dumpcache.json per asset"),
        FindFilesNamed(ScratchRoot, AssetDumpCache::DumpCacheFileName).Num(), Seed.AssetCount);

    const AssetDumpHandler::FFolderDumpStart Forced =
        AssetDumpHandler::StartAsyncFolderDump(
            AsyncDumpTestFolder,
            /*bRecursive=*/false,
            ScratchRoot,
            /*bIncludeLevels=*/false,
            /*bIncludeWidgetScreenshot=*/false,
            /*bForce=*/true);
    TestTrue(TEXT("Forced start must succeed"), Forced.ErrorCode.IsEmpty());
    if (!Forced.ErrorCode.IsEmpty())
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }
    TestEqual(TEXT("force=true queues every candidate"), Forced.QueuedCount, Forced.AssetCount);
    TestEqual(TEXT("force=true reports no unchanged cache hits"), Forced.UnchangedCount, 0);

    FJobTicket ForcedTicket;
    TestTrue(TEXT("Forced folder dump completes"), CompleteCurrentFolderDump(ForcedTicket));
    if (ForcedTicket.Result.IsValid())
    {
        TestEqual(TEXT("Forced result dumped"), ForcedTicket.Result->GetIntegerField(TEXT("dumped")), Forced.AssetCount);
        TestEqual(TEXT("Forced result unchanged"), ForcedTicket.Result->GetIntegerField(TEXT("unchanged")), 0);
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncFolderDumpCacheVersionInvalidationTest,
    "PinWright.asset.dump.AsyncFolderDump.CacheVersionInvalidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncFolderDumpCacheVersionInvalidationTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const AssetDumpHandler::FFolderDumpStart Seed =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, ScratchRoot);
    TestTrue(TEXT("Seed start must succeed"), Seed.ErrorCode.IsEmpty());
    if (Seed.AssetCount == 0)
    {
        AddInfo(FString::Printf(TEXT("No assets under %s in this configuration; skipping cache invalidation test."), AsyncDumpTestFolder));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    FJobTicket SeedTicket;
    TestTrue(TEXT("Seed folder dump completes"), CompleteCurrentFolderDump(SeedTicket));

    auto RunStaleStartCheck = [this, &ScratchRoot](const TCHAR* Label)
    {
        AssetDumpHandler::FFolderDumpStart StaleStart;
        FJobTicket StaleTicket;
        const bool bCompleted = StartInvalidatedCacheRun(ScratchRoot, StaleStart, StaleTicket);
        TestTrue(FString::Printf(TEXT("%s dump completes"), Label), bCompleted);
        TestTrue(FString::Printf(TEXT("%s start succeeds"), Label), StaleStart.ErrorCode.IsEmpty());
        // Exact count, not > 0: AsyncDumpTestFolder ships exactly one asset
        // (/Engine/EngineDamageTypes/DmgTypeBP_Environmental), and an invalidated cache must
        // re-queue it — so 1 is the only correct value and > 0 cannot distinguish a partial
        // re-queue from a complete one.
        TestEqual(FString::Printf(TEXT("%s queues the single stale asset"), Label), StaleStart.QueuedCount, 1);
        TestEqual(FString::Printf(TEXT("%s start counter invariant"), Label),
            StaleStart.AssetCount, StaleStart.QueuedCount + StaleStart.UnchangedCount);
        if (StaleTicket.Result.IsValid())
        {
            const int32 AssetCount = StaleTicket.Result->GetIntegerField(TEXT("assetCount"));
            const int32 Dumped = StaleTicket.Result->GetIntegerField(TEXT("dumped"));
            const int32 Unchanged = StaleTicket.Result->GetIntegerField(TEXT("unchanged"));
            const int32 SkipCount = StaleTicket.Result->GetIntegerField(TEXT("skipCount"));
            TestEqual(FString::Printf(TEXT("%s final counter invariant"), Label),
                AssetCount, Dumped + Unchanged + SkipCount);
        }
    };

    FString DumpDir;
    FString CachePath;
    AssetDumpCache::FAssetDumpCacheRecord Record;
    TestTrue(TEXT("Seed cache can be read"),
        ReadFirstDumpCacheRecord(ScratchRoot, DumpDir, CachePath, Record));

    TSharedPtr<FJsonObject> CacheJson = LoadJsonObjectFromFile(CachePath);
    TestTrue(TEXT("cacheVersion JSON parses"), CacheJson.IsValid());
    if (CacheJson.IsValid())
    {
        CacheJson->SetNumberField(TEXT("cacheVersion"), AssetDumpCache::AssetDumpCacheVersion + 1);
        TestTrue(TEXT("cacheVersion JSON rewrite succeeds"), SaveJsonObjectToFile(CacheJson, CachePath));
        RunStaleStartCheck(TEXT("cacheVersion mismatch"));
    }

    TestTrue(TEXT("Cache can be read after cacheVersion invalidation"),
        ReadFirstDumpCacheRecord(ScratchRoot, DumpDir, CachePath, Record));
    CacheJson = LoadJsonObjectFromFile(CachePath);
    const TSharedPtr<FJsonObject>* DumperPtr = nullptr;
    TSharedPtr<FJsonObject> Dumper;
    if (CacheJson.IsValid() && CacheJson->TryGetObjectField(TEXT("dumper"), DumperPtr) && DumperPtr)
    {
        Dumper = *DumperPtr;
    }
    TestTrue(TEXT("dumper JSON parses"), Dumper.IsValid());
    if (CacheJson.IsValid() && Dumper.IsValid())
    {
        Dumper->SetNumberField(TEXT("dumpCoreVersion"), AssetDumpCache::AssetDumpCoreVersion + 1);
        TestTrue(TEXT("dumpCoreVersion JSON rewrite succeeds"), SaveJsonObjectToFile(CacheJson, CachePath));
        RunStaleStartCheck(TEXT("dumpCoreVersion mismatch"));
    }

    TestTrue(TEXT("Cache can be read after dumpCoreVersion invalidation"),
        ReadFirstDumpCacheRecord(ScratchRoot, DumpDir, CachePath, Record));
    CacheJson = LoadJsonObjectFromFile(CachePath);
    DumperPtr = nullptr;
    Dumper.Reset();
    if (CacheJson.IsValid() && CacheJson->TryGetObjectField(TEXT("dumper"), DumperPtr) && DumperPtr)
    {
        Dumper = *DumperPtr;
    }
    const TSharedPtr<FJsonObject>* AspectVersionsPtr = nullptr;
    TSharedPtr<FJsonObject> AspectVersions;
    if (Dumper.IsValid()
        && Dumper->TryGetObjectField(TEXT("aspectVersions"), AspectVersionsPtr)
        && AspectVersionsPtr)
    {
        AspectVersions = *AspectVersionsPtr;
    }
    TestTrue(TEXT("aspectVersions JSON parses"), AspectVersions.IsValid());
    if (CacheJson.IsValid() && AspectVersions.IsValid())
    {
        FString FirstAspect;
        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : AspectVersions->Values)
        {
            FirstAspect = Pair.Key;
            break;
        }
        TestFalse(TEXT("aspectVersions has at least one entry"), FirstAspect.IsEmpty());
        if (!FirstAspect.IsEmpty())
        {
            // Bump the cached version off whatever the live dumper currently reports
            // for this aspect — not off AssetDumpDefaultAspectVersion. Per-aspect
            // bumps in GetAspectVersion() can leave the default+1 collision-free no
            // longer, which would make the freshness check still accept the cache.
            double CurrentAspectVersion = 0.0;
            AspectVersions->TryGetNumberField(FirstAspect, CurrentAspectVersion);
            AspectVersions->SetNumberField(FirstAspect, CurrentAspectVersion + 1);
            TestTrue(TEXT("aspectVersions JSON rewrite succeeds"), SaveJsonObjectToFile(CacheJson, CachePath));
            RunStaleStartCheck(TEXT("aspectVersions mismatch"));
        }
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncFolderDumpDirtyPackageInvalidatesCacheTest,
    "PinWright.asset.dump.AsyncFolderDump.DirtyPackageInvalidatesCache",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncFolderDumpDirtyPackageInvalidatesCacheTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const AssetDumpHandler::FFolderDumpStart Seed =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, ScratchRoot);
    TestTrue(TEXT("Seed start must succeed"), Seed.ErrorCode.IsEmpty());
    if (Seed.AssetCount == 0)
    {
        AddInfo(FString::Printf(TEXT("No assets under %s in this configuration; skipping dirty package cache test."), AsyncDumpTestFolder));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    FJobTicket SeedTicket;
    TestTrue(TEXT("Seed folder dump completes"), CompleteCurrentFolderDump(SeedTicket));

    FString DumpDir;
    FString CachePath;
    AssetDumpCache::FAssetDumpCacheRecord Record;
    if (!ReadFirstDumpCacheRecord(ScratchRoot, DumpDir, CachePath, Record))
    {
        AddError(TEXT("Dirty package test could not read seed cache."));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    UPackage* Package = FindPackage(nullptr, *Record.PackageName);
    if (!Package)
    {
        AddInfo(FString::Printf(TEXT("Package %s is not loaded; skipping dirty package cache test."), *Record.PackageName));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    const bool bWasDirty = Package->IsDirty();
    Package->SetDirtyFlag(true);
    const AssetDumpHandler::FFolderDumpStart DirtyStart =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, ScratchRoot);
    Package->SetDirtyFlag(bWasDirty);

    TestTrue(TEXT("Dirty start must succeed"), DirtyStart.ErrorCode.IsEmpty());
    if (!DirtyStart.ErrorCode.IsEmpty())
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }
    // Exact count, not > 0: AsyncDumpTestFolder ships exactly one asset
    // (/Engine/EngineDamageTypes/DmgTypeBP_Environmental) and it is the package just
    // dirtied, so the dirty sweep must re-queue exactly that one.
    TestEqual(TEXT("Dirty loaded package queues the single stale asset"), DirtyStart.QueuedCount, 1);
    TestEqual(TEXT("Dirty start counter invariant"),
        DirtyStart.AssetCount, DirtyStart.QueuedCount + DirtyStart.UnchangedCount);

    FJobTicket DirtyTicket;
    TestTrue(TEXT("Dirty folder dump completes"), CompleteCurrentFolderDump(DirtyTicket));

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// ============================================================================
// AssetDumpHandler.CacheWrite.BaselineAwareEligibility
// Unit coverage of the baseline-aware cache-write gate exposed through
// AssetDumpHandlerInternal.h. A dirty package vetoes the cache write ONLY when
// it was already dirty at sweep start (in BaselineDirty = user edits); dirt
// acquired after the baseline snapshot (compile-on-load side effects) no
// longer suppresses the record — the fix for the 17 perpetually re-dumped
// Blueprints. Fingerprint fabrication idioms from TestAssetDumpCache.cpp.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerCacheWriteBaselineAwareEligibilityTest,
    "PinWright.asset.dump.CacheWrite.BaselineAwareEligibility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerCacheWriteBaselineAwareEligibilityTest::RunTest(const FString& Parameters)
{
    // --- Pure predicate: IsCacheEligibleSource over fabricated fingerprints ---
    const FString FabricatedPackage = TEXT("/Game/Test/Foo");
    AssetDumpCache::FAssetDumpPackageFacts Facts;
    Facts.PackageName = FabricatedPackage;
    Facts.CorrectCasePackageName = FabricatedPackage;
    Facts.PackageExtension = TEXT("Asset");
    Facts.PackageSavedHash = TEXT("1111111111111111111111111111111111111111");
    Facts.RegistryDiskSize = 42;
    Facts.bHasPackageData = true;

    const AssetDumpCache::FAssetDumpSourceFingerprint CleanSource =
        AssetDumpCache::BuildSourceFingerprintFromFacts(Facts);
    AssetDumpCache::FAssetDumpSourceFingerprint DirtySource = CleanSource;
    DirtySource.bPackageIsDirty = true;
    const AssetDumpCache::FAssetDumpSourceFingerprint UncachedSource; // default Kind == Uncached

    const TSet<FName> EmptyBaseline;
    TSet<FName> BaselineWithFoo;
    BaselineWithFoo.Add(FName(*FabricatedPackage));

    TestTrue(TEXT("Clean source with empty baseline is eligible"),
        AssetDumpHandler::IsCacheEligibleSource(CleanSource, FabricatedPackage, EmptyBaseline));
    TestTrue(TEXT("Dirty source NOT in baseline is eligible (load-induced dirt)"),
        AssetDumpHandler::IsCacheEligibleSource(DirtySource, FabricatedPackage, EmptyBaseline));
    TestFalse(TEXT("Dirty source IN baseline is vetoed (user dirt)"),
        AssetDumpHandler::IsCacheEligibleSource(DirtySource, FabricatedPackage, BaselineWithFoo));
    TestTrue(TEXT("Clean source IN baseline is eligible (veto applies to dirty only)"),
        AssetDumpHandler::IsCacheEligibleSource(CleanSource, FabricatedPackage, BaselineWithFoo));
    TestFalse(TEXT("Uncached source kind is never eligible"),
        AssetDumpHandler::IsCacheEligibleSource(UncachedSource, FabricatedPackage, EmptyBaseline));

    // --- Full writer: WriteDumpCacheForSuccessfulBaseline on a real package ---
    const FString PackageName = TEXT("/Engine/EngineMaterials/WorldGridMaterial");
    UObject* Asset = LoadObject<UObject>(
        nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
    if (!Asset)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("WorldGridMaterial not loadable in this configuration; predicate coverage only."));
        return true;
    }
    UPackage* Package = FindPackage(nullptr, *PackageName);
    if (!Package)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("WorldGridMaterial package not resident; predicate coverage only."));
        return true;
    }

    FAssetRegistryModule& ARM =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AR = ARM.Get();
    const AssetDumpCache::FAssetDumpSourceFingerprint Probe =
        AssetDumpCache::BuildSourceFingerprintFromRegistry(AR, PackageName, /*bIsMapOrWorldPackage=*/false);
    if (Probe.Kind == AssetDumpCache::EAssetDumpSourceFingerprintKind::Uncached)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("WorldGridMaterial has no cacheable registry fingerprint here; predicate coverage only."));
        return true;
    }

    const FString ScratchDir = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();
    IFileManager::Get().MakeDirectory(*ScratchDir, /*Tree=*/true);
    FFileHelper::SaveStringToFile(TEXT("{}"), *(ScratchDir / DumpFileNames::Meta),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    const TArray<FString> WrittenPaths{ScratchDir / DumpFileNames::Meta};
    const FString CachePath = AssetDumpCache::GetCachePath(ScratchDir);

    const bool bWasDirty = Package->IsDirty();
    Package->SetDirtyFlag(true);

    TSet<FName> BaselineWithPackage;
    BaselineWithPackage.Add(Package->GetFName());
    TestFalse(TEXT("Dirty package IN baseline: cache write refused"),
        AssetDumpHandler::WriteDumpCacheForSuccessfulBaseline(
            PackageName, ScratchDir, WrittenPaths, /*bIncludeWidgetScreenshot=*/false, BaselineWithPackage));
    TestFalse(TEXT("No cache record on disk after refused write"),
        IFileManager::Get().FileExists(*CachePath));

    // Counterfactual: before the baseline-aware gate, this call returned false
    // for ANY dirty package, so the record below was never written and every
    // sweep re-queued the asset forever.
    const TSet<FName> NoBaseline;
    TestTrue(TEXT("Dirty package NOT in baseline: cache record written"),
        AssetDumpHandler::WriteDumpCacheForSuccessfulBaseline(
            PackageName, ScratchDir, WrittenPaths, /*bIncludeWidgetScreenshot=*/false, NoBaseline));
    TestTrue(TEXT("Cache record exists on disk after eligible write"),
        IFileManager::Get().FileExists(*CachePath));

    AssetDumpCache::FAssetDumpCacheRecord Record;
    FString ReadError;
    TestTrue(TEXT("Written cache record reads back"),
        AssetDumpCache::ReadCacheRecord(ScratchDir, Record, ReadError));
    TestEqual(TEXT("Record packageName matches"), Record.PackageName, PackageName);
    TestFalse(TEXT("Success record is not a skip stub"), Record.bSkipped);

    IFileManager::Get().Delete(*CachePath);
    Package->SetDirtyFlag(false);
    TestTrue(TEXT("Clean package IN baseline: cache record written"),
        AssetDumpHandler::WriteDumpCacheForSuccessfulBaseline(
            PackageName, ScratchDir, WrittenPaths, /*bIncludeWidgetScreenshot=*/false, BaselineWithPackage));

    Package->SetDirtyFlag(bWasDirty);
    IFileManager::Get().DeleteDirectory(*ScratchDir, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// ============================================================================
// AssetDumpHandler.AsyncFolderDump.MidSweepDirtyStillWritesCache
// Integration for the baseline-aware gate on the async path: a package that
// becomes dirty AFTER StartAsyncFolderDump snapshots BaselineDirty (the
// compile-on-load pattern) still gets a .dumpcache.json, the per-tick guard
// clears the transient dirt, and the NEXT sweep reports the asset unchanged.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncFolderDumpMidSweepDirtyStillWritesCacheTest,
    "PinWright.asset.dump.AsyncFolderDump.MidSweepDirtyStillWritesCache",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncFolderDumpMidSweepDirtyStillWritesCacheTest::RunTest(const FString& Parameters)
{
    const FString SeedRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    // Seed sweep: learns the target package name and leaves it loaded.
    const AssetDumpHandler::FFolderDumpStart Seed =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, SeedRoot);
    TestTrue(TEXT("Seed start must succeed"), Seed.ErrorCode.IsEmpty());
    if (Seed.AssetCount == 0)
    {
        AddInfo(FString::Printf(TEXT("No assets under %s in this configuration; skipping mid-sweep dirty test."), AsyncDumpTestFolder));
        IFileManager::Get().DeleteDirectory(*SeedRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }
    FJobTicket SeedTicket;
    TestTrue(TEXT("Seed folder dump completes"), CompleteCurrentFolderDump(SeedTicket));

    FString SeedDumpDir;
    FString SeedCachePath;
    AssetDumpCache::FAssetDumpCacheRecord SeedRecord;
    if (!ReadFirstDumpCacheRecord(SeedRoot, SeedDumpDir, SeedCachePath, SeedRecord))
    {
        AddError(TEXT("Mid-sweep dirty test could not read seed cache."));
        IFileManager::Get().DeleteDirectory(*SeedRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    UPackage* Package = FindPackage(nullptr, *SeedRecord.PackageName);
    if (!Package)
    {
        AddInfo(FString::Printf(TEXT("Package %s is not loaded; skipping mid-sweep dirty test."), *SeedRecord.PackageName));
        IFileManager::Get().DeleteDirectory(*SeedRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    const bool bWasDirty = Package->IsDirty();
    // Clean before start so the package is NOT captured in BaselineDirty.
    Package->SetDirtyFlag(false);

    const FString FreshRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();
    const AssetDumpHandler::FFolderDumpStart Start =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, FreshRoot);
    TestTrue(TEXT("Fresh start must succeed"), Start.ErrorCode.IsEmpty());
    TestEqual(TEXT("Fresh root queues every candidate"), Start.QueuedCount, Start.AssetCount);

    // Dirty AFTER the baseline snapshot — mimics LoadObject compile-on-load
    // dirtying the package mid-sweep.
    Package->SetDirtyFlag(true);

    FJobTicket Ticket;
    TestTrue(TEXT("Mid-sweep-dirty folder dump completes"), CompleteCurrentFolderDump(Ticket));

    // Counterfactual: pre-fix the dirty flag alone vetoed the cache write, so
    // no .dumpcache.json appeared for the dirtied package and the next sweep
    // re-queued it — the 17-Blueprint staleness loop.
    const TArray<FString> CacheFiles = FindFilesNamed(FreshRoot, AssetDumpCache::DumpCacheFileName);
    TestEqual(TEXT("Every candidate got a cache record despite mid-sweep dirt"),
        CacheFiles.Num(), Start.AssetCount);
    bool bFoundTargetRecord = false;
    for (const FString& CacheFile : CacheFiles)
    {
        AssetDumpCache::FAssetDumpCacheRecord Record;
        FString ReadError;
        if (AssetDumpCache::ReadCacheRecord(FPaths::GetPath(CacheFile), Record, ReadError)
            && Record.PackageName == SeedRecord.PackageName)
        {
            bFoundTargetRecord = true;
            TestFalse(TEXT("Mid-sweep-dirty record is not a skip stub"), Record.bSkipped);
        }
    }
    TestTrue(TEXT("Cache record written for the mid-sweep-dirtied package"), bFoundTargetRecord);

    TestFalse(TEXT("Per-tick guard cleared the post-baseline dirt"), Package->IsDirty());

    const AssetDumpHandler::FFolderDumpStart Second =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, FreshRoot);
    TestTrue(TEXT("Second start must succeed"), Second.ErrorCode.IsEmpty());
    TestEqual(TEXT("Second sweep queues nothing"), Second.QueuedCount, 0);
    TestEqual(TEXT("Second sweep reports every candidate unchanged"),
        Second.UnchangedCount, Second.AssetCount);
    FJobTicket SecondTicket;
    TestTrue(TEXT("Second folder dump completes"), CompleteCurrentFolderDump(SecondTicket));

    Package->SetDirtyFlag(bWasDirty);
    IFileManager::Get().DeleteDirectory(*SeedRoot, /*RequireExists=*/false, /*Tree=*/true);
    IFileManager::Get().DeleteDirectory(*FreshRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// ============================================================================
// AssetDumpHandler.AsyncFolderDump.BaselineDirtySkipsCacheWrite
// Paired negative: a package already dirty BEFORE StartAsyncFolderDump (user
// edits, captured in BaselineDirty) is still dumped but gets NO cache record,
// stays dirty through the sweep (guard preserves baseline packages), and the
// next sweep re-queues it — user dirt keeps invalidating until saved.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncFolderDumpBaselineDirtySkipsCacheWriteTest,
    "PinWright.asset.dump.AsyncFolderDump.BaselineDirtySkipsCacheWrite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncFolderDumpBaselineDirtySkipsCacheWriteTest::RunTest(const FString& Parameters)
{
    const FString SeedRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const AssetDumpHandler::FFolderDumpStart Seed =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, SeedRoot);
    TestTrue(TEXT("Seed start must succeed"), Seed.ErrorCode.IsEmpty());
    if (Seed.AssetCount == 0)
    {
        AddInfo(FString::Printf(TEXT("No assets under %s in this configuration; skipping baseline-dirty test."), AsyncDumpTestFolder));
        IFileManager::Get().DeleteDirectory(*SeedRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }
    FJobTicket SeedTicket;
    TestTrue(TEXT("Seed folder dump completes"), CompleteCurrentFolderDump(SeedTicket));

    FString SeedDumpDir;
    FString SeedCachePath;
    AssetDumpCache::FAssetDumpCacheRecord SeedRecord;
    if (!ReadFirstDumpCacheRecord(SeedRoot, SeedDumpDir, SeedCachePath, SeedRecord))
    {
        AddError(TEXT("Baseline-dirty test could not read seed cache."));
        IFileManager::Get().DeleteDirectory(*SeedRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    UPackage* Package = FindPackage(nullptr, *SeedRecord.PackageName);
    if (!Package)
    {
        AddInfo(FString::Printf(TEXT("Package %s is not loaded; skipping baseline-dirty test."), *SeedRecord.PackageName));
        IFileManager::Get().DeleteDirectory(*SeedRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    const bool bWasDirty = Package->IsDirty();
    // Dirty BEFORE start: captured in BaselineDirty as user dirt.
    Package->SetDirtyFlag(true);

    const FString FreshRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();
    const AssetDumpHandler::FFolderDumpStart Start =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, FreshRoot);
    TestTrue(TEXT("Baseline-dirty start must succeed"), Start.ErrorCode.IsEmpty());
    TestEqual(TEXT("Fresh root queues every candidate"), Start.QueuedCount, Start.AssetCount);

    FJobTicket Ticket;
    TestTrue(TEXT("Baseline-dirty folder dump completes"), CompleteCurrentFolderDump(Ticket));

    // The dump itself happened (sidecars exist)…
    TestEqual(TEXT("Every candidate got a meta.json"),
        FindFilesNamed(FreshRoot, DumpFileNames::Meta).Num(), Start.AssetCount);
    // …but the user-dirty package must have NO cache record.
    bool bFoundTargetRecord = false;
    for (const FString& CacheFile : FindFilesNamed(FreshRoot, AssetDumpCache::DumpCacheFileName))
    {
        AssetDumpCache::FAssetDumpCacheRecord Record;
        FString ReadError;
        if (AssetDumpCache::ReadCacheRecord(FPaths::GetPath(CacheFile), Record, ReadError)
            && Record.PackageName == SeedRecord.PackageName)
        {
            bFoundTargetRecord = true;
        }
    }
    TestFalse(TEXT("No cache record for the baseline-dirty package"), bFoundTargetRecord);

    TestTrue(TEXT("Guard preserves baseline (user) dirt through the sweep"), Package->IsDirty());

    // Still stale on the next sweep: user dirt invalidates until the package is saved.
    const AssetDumpHandler::FFolderDumpStart Second =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, FreshRoot);
    TestTrue(TEXT("Second start must succeed"), Second.ErrorCode.IsEmpty());
    // Exact count, not > 0: AsyncDumpTestFolder ships exactly one asset
    // (/Engine/EngineDamageTypes/DmgTypeBP_Environmental) and it is the user-dirty package,
    // so the second sweep must re-queue exactly that one rather than cache-hit it.
    TestEqual(TEXT("Second sweep re-queues the user-dirty package"), Second.QueuedCount, 1);
    FJobTicket SecondTicket;
    TestTrue(TEXT("Second folder dump completes"), CompleteCurrentFolderDump(SecondTicket));

    Package->SetDirtyFlag(bWasDirty);
    IFileManager::Get().DeleteDirectory(*SeedRoot, /*RequireExists=*/false, /*Tree=*/true);
    IFileManager::Get().DeleteDirectory(*FreshRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// ============================================================================
// AssetDumpHandler.AsyncFolderDump.MismatchedInnerNameDumps
// End-to-end regression for the "Bake Out Materials" package shape: an
// on-disk package whose single inner asset name differs from the package tail
// (fixture: PkgTail_<guid> containing MI_Inner_<guid>) must produce a REAL
// dump (object-path load), a cache record, and a cache-fresh second sweep —
// not the pre-fix ASSET_LOAD_FAILED skip stub written forever.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncFolderDumpMismatchedInnerNameDumpsTest,
    "PinWright.asset.dump.AsyncFolderDump.MismatchedInnerNameDumps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncFolderDumpMismatchedInnerNameDumpsTest::RunTest(const FString& Parameters)
{
    AssetDumpMismatchedNameFixture::FMismatchedNameAsset Fixture;
    FString FixtureError;
    if (!AssetDumpMismatchedNameFixture::Create(Fixture, FixtureError))
    {
        AddError(FString::Printf(TEXT("Mismatched-name fixture creation failed: %s"), *FixtureError));
        AssetDumpMismatchedNameFixture::Cleanup(Fixture);
        return true;
    }

    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const AssetDumpHandler::FFolderDumpStart Start =
        AssetDumpHandler::StartAsyncFolderDump(Fixture.FolderPath, /*bRecursive=*/false, ScratchRoot);
    TestTrue(TEXT("Fixture sweep start must succeed"), Start.ErrorCode.IsEmpty());
    TestEqual(TEXT("Fixture folder holds exactly one candidate"), Start.AssetCount, 1);
    TestEqual(TEXT("Fixture package is queued"), Start.QueuedCount, 1);
    if (Start.ErrorCode.IsEmpty() && Start.AssetCount > 0)
    {
        FJobTicket Ticket;
        TestTrue(TEXT("Fixture folder dump completes"), CompleteCurrentFolderDump(Ticket));
        if (Ticket.Result.IsValid())
        {
            TestEqual(TEXT("Fixture dump result dumped"), Ticket.Result->GetIntegerField(TEXT("dumped")), 1);
            TestEqual(TEXT("Fixture dump result skipCount"), Ticket.Result->GetIntegerField(TEXT("skipCount")), 0);
        }

        // Counterfactual: pre-fix the queue carried the bare package name, whose
        // LoadObject finds no object named "PkgTail_<guid>" — the sweep wrote a
        // skipped:true stub and no sidecars, failing every assertion below.
        const FString DumpDir = AssetDumpWriter::ResolveDumpDir(Fixture.PackagePath, ScratchRoot);
        TSharedPtr<FJsonObject> Meta = LoadJsonObjectFromFile(DumpDir / DumpFileNames::Meta);
        TestTrue(TEXT("meta.json exists in the package-named dump dir"), Meta.IsValid());
        if (Meta.IsValid())
        {
            TestFalse(TEXT("Real dump: meta.json carries no skipped marker"),
                Meta->HasField(TEXT("skipped")));
        }
        TestTrue(TEXT("properties.json written for the mismatched-name asset"),
            IFileManager::Get().FileExists(*(DumpDir / DumpFileNames::Properties)));
        TestTrue(TEXT("material_instance.json written (primary = MIC)"),
            IFileManager::Get().FileExists(*(DumpDir / DumpFileNames::MaterialInstance)));
        // The mirror stays package-named: no object-path-suffixed sibling dir.
        TestFalse(TEXT("No dump dir under the object-path (suffixed) shape"),
            IFileManager::Get().DirectoryExists(
                *AssetDumpWriter::ResolveDumpDir(Fixture.ObjectPath, ScratchRoot)));

        AssetDumpCache::FAssetDumpCacheRecord Record;
        FString ReadError;
        TestTrue(TEXT("Cache record written for the mismatched-name package"),
            AssetDumpCache::ReadCacheRecord(DumpDir, Record, ReadError));
        TestEqual(TEXT("Record packageName is the package (not the object path)"),
            Record.PackageName, Fixture.PackagePath);
        TestFalse(TEXT("Record is not a skip stub"), Record.bSkipped);

        const AssetDumpHandler::FFolderDumpStart Second =
            AssetDumpHandler::StartAsyncFolderDump(Fixture.FolderPath, /*bRecursive=*/false, ScratchRoot);
        TestTrue(TEXT("Second fixture sweep start must succeed"), Second.ErrorCode.IsEmpty());
        TestEqual(TEXT("Second sweep queues nothing"), Second.QueuedCount, 0);
        TestEqual(TEXT("Second sweep reports the package unchanged"), Second.UnchangedCount, 1);
        FJobTicket SecondTicket;
        TestTrue(TEXT("Second fixture folder dump completes"), CompleteCurrentFolderDump(SecondTicket));
    }

    AssetDumpMismatchedNameFixture::Cleanup(Fixture);
    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerDirectDumpIgnoresFreshFolderCacheTest,
    "PinWright.asset.dump.DirectDumpIgnoresFreshFolderCache",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerDirectDumpIgnoresFreshFolderCacheTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const AssetDumpHandler::FFolderDumpStart Seed =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, ScratchRoot);
    TestTrue(TEXT("Seed start must succeed"), Seed.ErrorCode.IsEmpty());
    if (Seed.AssetCount == 0)
    {
        AddInfo(FString::Printf(TEXT("No assets under %s in this configuration; skipping direct dump cache test."), AsyncDumpTestFolder));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    FJobTicket SeedTicket;
    TestTrue(TEXT("Seed folder dump completes"), CompleteCurrentFolderDump(SeedTicket));

    FString DumpDir;
    FString CachePath;
    AssetDumpCache::FAssetDumpCacheRecord Record;
    if (!ReadFirstDumpCacheRecord(ScratchRoot, DumpDir, CachePath, Record))
    {
        AddError(TEXT("Direct dump test could not read seed cache."));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    const FString MetaPath = DumpDir / DumpFileNames::Meta;
    FString MetaBody;
    if (!FFileHelper::LoadFileToString(MetaBody, *MetaPath))
    {
        AddError(TEXT("Direct dump test could not read seed meta.json."));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }
    if (!MetaBody.StartsWith(TEXT("{")))
    {
        AddError(TEXT("Direct dump test seed meta.json was not an object."));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }
    MetaBody.InsertAt(1, TEXT("\"directDumpSentinel\":true,"));
    TestTrue(TEXT("Sentinel meta.json rewrite succeeds"), FFileHelper::SaveStringToFile(
        MetaBody, *MetaPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

    AssetDumpHandler::FDumpSingleResult Direct =
        AssetDumpHandler::DumpSingleAsset(Record.PackageName, ScratchRoot, /*bDiff=*/false);
    TestTrue(TEXT("Direct asset.dump succeeds"), Direct.ErrorCode.IsEmpty());
    for (const FString& WrittenPath : Direct.WrittenPaths)
    {
        TestFalse(TEXT("Direct writtenPaths excludes .dumpcache.json"),
            WrittenPath.EndsWith(AssetDumpCache::DumpCacheFileName));
    }

    FString RewrittenMeta;
    TestTrue(TEXT("Rewritten meta.json loads"), FFileHelper::LoadFileToString(RewrittenMeta, *MetaPath));
    TestFalse(TEXT("Direct asset.dump rewrites stale public sidecar despite fresh cache"),
        RewrittenMeta.Contains(TEXT("directDumpSentinel")));

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerDiffDumpLeavesCacheUntouchedTest,
    "PinWright.asset.dump.DiffDumpLeavesCacheUntouched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerDiffDumpLeavesCacheUntouchedTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const AssetDumpHandler::FFolderDumpStart Seed =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, ScratchRoot);
    TestTrue(TEXT("Seed start must succeed"), Seed.ErrorCode.IsEmpty());
    if (Seed.AssetCount == 0)
    {
        AddInfo(FString::Printf(TEXT("No assets under %s in this configuration; skipping diff cache test."), AsyncDumpTestFolder));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    FJobTicket SeedTicket;
    TestTrue(TEXT("Seed folder dump completes"), CompleteCurrentFolderDump(SeedTicket));

    FString DumpDir;
    FString CachePath;
    AssetDumpCache::FAssetDumpCacheRecord Record;
    if (!ReadFirstDumpCacheRecord(ScratchRoot, DumpDir, CachePath, Record))
    {
        AddError(TEXT("Diff dump test could not read seed cache."));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    FString CacheBefore;
    TestTrue(TEXT("Cache file loads before diff"), FFileHelper::LoadFileToString(CacheBefore, *CachePath));

    AssetDumpHandler::FDumpSingleResult Diff =
        AssetDumpHandler::DumpSingleAsset(Record.PackageName, ScratchRoot, /*bDiff=*/true);
    TestTrue(TEXT("Diff asset.dump succeeds"), Diff.ErrorCode.IsEmpty());
    // Guard the loop below (and the cache-unchanged check that follows) against a
    // vacuous pass: with zero written paths the loop asserts nothing and the cache
    // trivially matches, so a diff run that wrote nothing at all would read as green.
    TestTrue(TEXT("diff wrote at least one path"), Diff.WrittenPaths.Num() > 0);
    for (const FString& WrittenPath : Diff.WrittenPaths)
    {
        TestFalse(TEXT("Diff writtenPaths excludes .dumpcache.json"),
            WrittenPath.EndsWith(AssetDumpCache::DumpCacheFileName));
    }

    FString CacheAfter;
    TestTrue(TEXT("Cache file loads after diff"), FFileHelper::LoadFileToString(CacheAfter, *CachePath));
    TestEqual(TEXT("Diff asset.dump leaves .dumpcache.json unchanged"), CacheAfter, CacheBefore);

    // The diff run wrote its artifacts to the per-asset diff dir; clean it up.
    IFileManager::Get().DeleteDirectory(
        *AssetDumpWriter::ResolveDiffDir(Record.PackageName), /*RequireExists=*/false, /*Tree=*/true);
    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncFolderDumpWorldPackagesStayQueuedTest,
    "PinWright.asset.dump.AsyncFolderDump.WorldPackagesStayQueued",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncFolderDumpWorldPackagesStayQueuedTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const AssetDumpHandler::FFolderDumpStart First =
        AssetDumpHandler::StartAsyncFolderDump(
            TEXT("/Engine/Maps"),
            /*bRecursive=*/false,
            ScratchRoot,
            /*bIncludeLevels=*/true);
    TestTrue(TEXT("First world-inclusive start must succeed"), First.ErrorCode.IsEmpty());
    if (First.AssetCount == 0)
    {
        AddInfo(TEXT("/Engine/Maps has no assets in this configuration; skipping world cache test."));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    FJobTicket FirstTicket;
    TestTrue(TEXT("First world-inclusive dump completes"), CompleteCurrentFolderDump(FirstTicket));

    const AssetDumpHandler::FFolderDumpStart Second =
        AssetDumpHandler::StartAsyncFolderDump(
            TEXT("/Engine/Maps"),
            /*bRecursive=*/false,
            ScratchRoot,
            /*bIncludeLevels=*/true);
    TestTrue(TEXT("Second world-inclusive start must succeed"), Second.ErrorCode.IsEmpty());
    if (!Second.ErrorCode.IsEmpty())
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }
    TestTrue(TEXT("World/map package remains queued instead of cache-hit"), Second.QueuedCount > 0);
    TestEqual(TEXT("Second world-inclusive start counter invariant"),
        Second.AssetCount, Second.QueuedCount + Second.UnchangedCount);

    FJobTicket SecondTicket;
    TestTrue(TEXT("Second world-inclusive dump completes"), CompleteCurrentFolderDump(SecondTicket));

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncFolderDumpStatusIdleByDefaultTest,
    "PinWright.asset.dump.AsyncFolderDump.StatusIdleByDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncFolderDumpStatusIdleByDefaultTest::RunTest(const FString& Parameters)
{
    const AssetDumpHandler::FFolderDumpStatus S = AssetDumpHandler::GetAsyncFolderDumpStatus();
    TestFalse(TEXT("Idle by default: bInProgress must be false"), S.bInProgress);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerPendingOrderTest,
    "PinWright.asset.dump.AsyncFolderDump.DeterministicPendingOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerPendingOrderTest::RunTest(const FString& Parameters)
{
    TArray<FPendingDumpEntry> Pending;
    for (const FString& Path : {FString(TEXT("/Game/Z.Z")), FString(TEXT("/Game/A.A")), FString(TEXT("/Game/M.M"))})
    {
        FPendingDumpEntry Entry;
        Entry.ObjectPath = Path;
        Entry.PackageName = FPackageName::ObjectPathToPackageName(Path);
        Pending.Add(MoveTemp(Entry));
    }

    AssetDumpHandler::SortPendingAssetsForProcessing(Pending);
    TestEqual(TEXT("Pop 1 is lexical first"), Pending.Pop().ObjectPath, FString(TEXT("/Game/A.A")));
    TestEqual(TEXT("Pop 2 is lexical second"), Pending.Pop().ObjectPath, FString(TEXT("/Game/M.M")));
    TestEqual(TEXT("Pop 3 is lexical last"), Pending.Pop().ObjectPath, FString(TEXT("/Game/Z.Z")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerCompileWaitBoundaryTest,
    "PinWright.asset.dump.AsyncFolderDump.CompileWaitBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerCompileWaitBoundaryTest::RunTest(const FString& Parameters)
{
    TestFalse(TEXT("Wait below timeout is deferred"),
        AssetDumpHandler::HasAsyncCompilationTimedOut(10.0, 129.999, 120.0));
    TestTrue(TEXT("Wait at timeout is skipped"),
        AssetDumpHandler::HasAsyncCompilationTimedOut(10.0, 130.0, 120.0));
    TestFalse(TEXT("Unset wait start never times out"),
        AssetDumpHandler::HasAsyncCompilationTimedOut(0.0, 1000.0, 120.0));
    return true;
}

// ============================================================================
// AssetDumpHandler.AsyncFolderDump.DeferredRequeueIndex / ReleaseSkipsDeferred
// A still-compiling asset used to be requeued at index 0 - the far end of a
// Pop()-consumed queue - so its single retry landed at the end of the sweep, by
// which time the release step had unloaded it and its wait had expired.
// Board: B-dump-folder-deferred-assets-time-out-en-masse.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerDeferredRequeueIndexTest,
    "PinWright.asset.dump.AsyncFolderDump.DeferredRequeueIndex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerDeferredRequeueIndexTest::RunTest(const FString& Parameters)
{
    // The retry must come back within Backlog pops, never at the far end of the queue.
    TestEqual(TEXT("long queue requeues Backlog entries from the pop end"),
        AssetDumpHandler::ComputeDeferredRequeueIndex(1000, 32), 968);
    TestEqual(TEXT("queue shorter than the backlog requeues at the front of the array"),
        AssetDumpHandler::ComputeDeferredRequeueIndex(10, 32), 0);
    TestEqual(TEXT("empty queue requeues at 0"),
        AssetDumpHandler::ComputeDeferredRequeueIndex(0, 32), 0);
    // Failure direction: index 0 on a long queue is the old end-of-sweep behaviour.
    TestTrue(TEXT("long queue never requeues at index 0"),
        AssetDumpHandler::ComputeDeferredRequeueIndex(1000) > 0);
    // Insert() rejects an index past Num(), and a backlog of 0 would re-pop the same
    // entry forever.
    TestEqual(TEXT("backlog clamps to at least one intervening entry"),
        AssetDumpHandler::ComputeDeferredRequeueIndex(5, 0), 4);

    // Applied to a real queue: the deferred entry comes back after Backlog others,
    // so Backlog assets are in flight at once.
    TArray<int32> Queue;
    for (int32 i = 0; i < 100; ++i)
    {
        Queue.Add(i);
    }
    const int32 Deferred = Queue.Pop(EAllowShrinking::No);
    Queue.Insert(Deferred, AssetDumpHandler::ComputeDeferredRequeueIndex(Queue.Num(), 4));
    TestEqual(TEXT("4 other entries pop before the retry"), Queue.Pop(), 98);
    TestEqual(TEXT("4 other entries pop before the retry"), Queue.Pop(), 97);
    TestEqual(TEXT("4 other entries pop before the retry"), Queue.Pop(), 96);
    TestEqual(TEXT("4 other entries pop before the retry"), Queue.Pop(), 95);
    TestEqual(TEXT("deferred entry is retried next"), Queue.Pop(), Deferred);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerReleaseSkipsDeferredPackagesTest,
    "PinWright.asset.dump.AsyncFolderDump.ReleaseSkipsDeferredPackages",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerReleaseSkipsDeferredPackagesTest::RunTest(const FString& Parameters)
{
    TSet<FName> Tracked;
    Tracked.Add(FName(TEXT("/Game/A")));
    Tracked.Add(FName(TEXT("/Game/B")));
    Tracked.Add(FName(TEXT("/Game/C")));

    // Deferrals are keyed by object path; tracking is by package name.
    const TArray<FString> Deferred = {TEXT("/Game/B.B")};
    const TSet<FName> Releasable =
        AssetDumpHandler::FilterReleasablePackages(Tracked, Deferred);

    TestEqual(TEXT("one deferral holds back exactly one package"), Releasable.Num(), 2);
    TestTrue(TEXT("undeferred package is releasable"), Releasable.Contains(FName(TEXT("/Game/A"))));
    TestTrue(TEXT("undeferred package is releasable"), Releasable.Contains(FName(TEXT("/Game/C"))));
    // Failure direction: releasing this one is the defect - the reloaded copy reports
    // compiling again and the retry skips it instantly.
    TestFalse(TEXT("deferred package is never released"),
        Releasable.Contains(FName(TEXT("/Game/B"))));

    // Inner-object paths ("/Game/Pkg.Pkg:SubObject") and bare package names both resolve.
    const TSet<FName> FromInnerPath = AssetDumpHandler::FilterReleasablePackages(
        Tracked, {TEXT("/Game/A.A:Inner"), TEXT("/Game/C")});
    TestEqual(TEXT("inner-object and bare-package keys both hold back their package"),
        FromInnerPath.Num(), 1);
    TestTrue(TEXT("only the undeferred package remains releasable"),
        FromInnerPath.Contains(FName(TEXT("/Game/B"))));

    TestEqual(TEXT("no deferrals releases everything"),
        AssetDumpHandler::FilterReleasablePackages(Tracked, {}).Num(), 3);

    return true;
}

// ============================================================================
// AssetDumpHandler.AsyncFolderDump.ReleaseStepTrigger
// The pure count/watermark decision behind the folder sweep's release step.
// Board: B-dump-folder-sweep-never-gcs-ooms-editor.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerReleaseStepTriggerTest,
    "PinWright.asset.dump.AsyncFolderDump.ReleaseStepTrigger",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerReleaseStepTriggerTest::RunTest(const FString& Parameters)
{
    constexpr uint64 TotalPhysical = 64ull * 1024 * 1024 * 1024;
    constexpr uint64 Quarter = TotalPhysical / 4;
    constexpr uint64 ThreeQuarters = (TotalPhysical / 4) * 3;

    // Count trigger.
    TestFalse(TEXT("below the interval does not release"),
        AssetDumpHandler::ShouldRunDumpReleaseStep(199, 200, Quarter, TotalPhysical, 0.5f));
    TestTrue(TEXT("at the interval releases"),
        AssetDumpHandler::ShouldRunDumpReleaseStep(200, 200, Quarter, TotalPhysical, 0.5f));
    TestTrue(TEXT("past the interval releases"),
        AssetDumpHandler::ShouldRunDumpReleaseStep(201, 200, Quarter, TotalPhysical, 0.5f));

    // Watermark trigger, well below the count interval.
    TestTrue(TEXT("over the watermark releases early"),
        AssetDumpHandler::ShouldRunDumpReleaseStep(
            AssetDumpHandler::DumpReleaseMinAssetsBetweenSteps, 200,
            ThreeQuarters, TotalPhysical, 0.5f));
    TestFalse(TEXT("under the watermark waits for the count"),
        AssetDumpHandler::ShouldRunDumpReleaseStep(
            AssetDumpHandler::DumpReleaseMinAssetsBetweenSteps, 200,
            Quarter, TotalPhysical, 0.5f));

    // The floor keeps a watermark the sweep cannot get back under from turning into
    // one full drain-and-collect per asset.
    TestFalse(TEXT("watermark respects the minimum asset floor"),
        AssetDumpHandler::ShouldRunDumpReleaseStep(
            AssetDumpHandler::DumpReleaseMinAssetsBetweenSteps - 1, 200,
            ThreeQuarters, TotalPhysical, 0.5f));

    // Disabling knobs.
    TestFalse(TEXT("interval 0 disables the count trigger"),
        AssetDumpHandler::ShouldRunDumpReleaseStep(100000, 0, Quarter, TotalPhysical, 0.5f));
    TestFalse(TEXT("watermark 0 disables the watermark trigger"),
        AssetDumpHandler::ShouldRunDumpReleaseStep(100, 200, ThreeQuarters, TotalPhysical, 0.0f));
    TestFalse(TEXT("both knobs 0 never releases"),
        AssetDumpHandler::ShouldRunDumpReleaseStep(100000, 0, ThreeQuarters, TotalPhysical, 0.0f));

    // Nothing processed means nothing to release, whatever the memory reads.
    TestFalse(TEXT("zero assets since the last release never releases"),
        AssetDumpHandler::ShouldRunDumpReleaseStep(0, 200, ThreeQuarters, TotalPhysical, 0.5f));

    // An unreadable TotalPhysical must not divide-by-zero into a permanent release.
    TestFalse(TEXT("unknown physical memory disables the watermark"),
        AssetDumpHandler::ShouldRunDumpReleaseStep(100, 200, ThreeQuarters, 0, 0.5f));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerCancellationPreservesMirrorTest,
    "PinWright.asset.dump.AsyncFolderDump.CancelPreservesMirror",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerCancellationPreservesMirrorTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();
    const AssetDumpHandler::FFolderDumpStart Start =
        AssetDumpHandler::StartAsyncFolderDump(
            AsyncDumpTestFolder, /*bRecursive=*/false, ScratchRoot, false, false, true);
    TestTrue(TEXT("Folder dump starts"), Start.ErrorCode.IsEmpty());
    if (!Start.ErrorCode.IsEmpty() || Start.AssetCount == 0)
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    const FString PreservedDir = Start.RootDir / TEXT("PriorMirror");
    const FString PreservedMeta = PreservedDir / DumpFileNames::Meta;
    IFileManager::Get().MakeDirectory(*PreservedDir, /*Tree=*/true);
    FFileHelper::SaveStringToFile(
        TEXT("{}"), *PreservedMeta, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    const TSharedRef<FJsonObject> StartedParams = MakeShared<FJsonObject>();
    FJobRegistry& Registry = FPluginState::Get().GetJobRegistry();
    const FString TicketId = Registry.Start(TEXT("asset.dump_folder"), StartedParams);
    TestTrue(TEXT("Ticket attaches cancellation callback"),
        AssetDumpHandler::AttachJobTicketToAsyncDump(TicketId));

    const AssetDumpHandler::FFolderDumpStatus Running =
        AssetDumpHandler::GetAsyncFolderDumpStatus();
    TestEqual(TEXT("Initial phase is visible"), Running.CurrentPhase, FString(TEXT("queued")));
    TestTrue(TEXT("Phase start time is populated"), Running.CurrentPhaseStartedAt.GetTicks() > 0);

    TestTrue(TEXT("Cancellation accepted"),
        Registry.Cancel(TicketId) == EJobCancelResult::Requested);
    TestFalse(TEXT("Dump becomes idle immediately"),
        AssetDumpHandler::GetAsyncFolderDumpStatus().bInProgress);
    TestTrue(TEXT("Partial cancellation does not reconcile prior mirror"),
        IFileManager::Get().FileExists(*PreservedMeta));

    // A queued ticker callback after cancellation must be inert and must not
    // complete the already-cancelled ticket.
    FTSTicker::GetCoreTicker().Tick(0.01f);
    FJobTicket Ticket;
    TestTrue(TEXT("Cancelled ticket remains queryable"), Registry.Get(TicketId, Ticket));
    TestEqual(TEXT("Cancelled status is terminal"), Ticket.Status, FString(TEXT("cancelled")));
    TestTrue(TEXT("Cancelled sweep still did not reconcile"),
        IFileManager::Get().FileExists(*PreservedMeta));

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncSingleWorldDumpKickoffRunsAndCompletesTest,
    "PinWright.asset.dump.AsyncSingleWorldDump.KickoffRunsAndCompletes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncSingleWorldDumpKickoffRunsAndCompletesTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();
    const FString AssetPath = TEXT("/Engine/Maps/Entry");

    if (!AssetDumpHandler::IsWorldAssetPath(AssetPath))
    {
        AddInfo(TEXT("/Engine/Maps/Entry is not available as a UWorld in this configuration; skipped."));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    const AssetDumpHandler::FSingleAssetDumpStart R =
        AssetDumpHandler::StartAsyncSingleAssetDump(AssetPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("Start must succeed (no error)"), R.ErrorCode.IsEmpty());
    TestEqual(TEXT("Start assetPath must match"), R.AssetPath, AssetPath);

    AssetDumpHandler::FFolderDumpStatus S = AssetDumpHandler::GetAsyncFolderDumpStatus();
    TestTrue(TEXT("Status must report in-progress immediately after start"), S.bInProgress);
    TestEqual(TEXT("Status assetPath must match"), S.AssetPath, AssetPath);

    const int32 Ticks = DrainFolderDump();
    TestTrue(TEXT("World dump should complete within MaxTicks"), Ticks < 64);
    TestFalse(TEXT("Status must report idle after completion"),
        AssetDumpHandler::GetAsyncFolderDumpStatus().bInProgress);

    const FString ManifestPath = R.DumpDir / DumpFileNames::ActorsManifest;
    TestTrue(TEXT("UWorld async dump writes actors/manifest.json"),
        IFileManager::Get().FileExists(*ManifestPath));

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncSingleWorldDumpRejectsConcurrentTest,
    "PinWright.asset.dump.AsyncSingleWorldDump.RejectsConcurrent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncSingleWorldDumpRejectsConcurrentTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();
    const FString AssetPath = TEXT("/Engine/Maps/Entry");

    if (!AssetDumpHandler::IsWorldAssetPath(AssetPath))
    {
        AddInfo(TEXT("/Engine/Maps/Entry is not available as a UWorld in this configuration; skipped."));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    const AssetDumpHandler::FSingleAssetDumpStart First =
        AssetDumpHandler::StartAsyncSingleAssetDump(AssetPath, ScratchRoot, /*bDiff=*/false);
    TestTrue(TEXT("First start must succeed (no error)"), First.ErrorCode.IsEmpty());

    const AssetDumpHandler::FSingleAssetDumpStart Second =
        AssetDumpHandler::StartAsyncSingleAssetDump(AssetPath, ScratchRoot, /*bDiff=*/false);
    TestFalse(TEXT("Second start must report error"), Second.ErrorCode.IsEmpty());
    TestEqual(TEXT("ErrorCode must be DUMP_IN_PROGRESS"),
        Second.ErrorCode, FString(TEXT("DUMP_IN_PROGRESS")));
    TestTrue(TEXT("ErrorMessage must name the running assetPath"),
        Second.ErrorMessage.Contains(AssetPath));

    DrainFolderDump();

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncFolderDumpInvalidFolderPathTest,
    "PinWright.asset.dump.AsyncFolderDump.InvalidFolderPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncFolderDumpInvalidFolderPathTest::RunTest(const FString& Parameters)
{
    const AssetDumpHandler::FFolderDumpStart R =
        AssetDumpHandler::StartAsyncFolderDump(TEXT("NoLeadingSlash"), true, FString());

    TestFalse(TEXT("ErrorCode must be non-empty"), R.ErrorCode.IsEmpty());
    TestEqual(TEXT("ErrorCode must be INVALID_FOLDER"), R.ErrorCode, FString(TEXT("INVALID_FOLDER")));

    const AssetDumpHandler::FFolderDumpStatus S = AssetDumpHandler::GetAsyncFolderDumpStatus();
    TestFalse(TEXT("State must remain idle after rejected start"), S.bInProgress);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncFolderDumpKickoffRunsAndCompletesTest,
    "PinWright.asset.dump.AsyncFolderDump.KickoffRunsAndCompletes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncFolderDumpKickoffRunsAndCompletesTest::RunTest(const FString& Parameters)
{
    // Scratch outRoot so we never touch the canonical dump tree.
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    // Non-recursive sweep of a tiny single-asset engine folder so the async
    // pipeline finishes in O(1) ticks (was multi-second on /Engine/EngineMaterials).
    const AssetDumpHandler::FFolderDumpStart R =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, ScratchRoot);

    TestTrue(TEXT("Start must succeed (no error)"), R.ErrorCode.IsEmpty());
    TestTrue(TEXT("At least one asset should be queued"), R.AssetCount > 0);

    // If no assets were queued (e.g. engine stripped in headless test mode),
    // the API contract says state stays idle. Bail out cleanly.
    if (R.AssetCount == 0)
    {
        AddInfo(FString::Printf(TEXT("No assets under %s in this configuration; skipping tick test."), AsyncDumpTestFolder));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    AssetDumpHandler::FFolderDumpStatus S = AssetDumpHandler::GetAsyncFolderDumpStatus();
    TestTrue(TEXT("Status must report in-progress immediately after start"), S.bInProgress);
    TestEqual(TEXT("Status folderPath must match"), S.FolderPath, FString(AsyncDumpTestFolder));

    const int32 Ticks = DrainFolderDump();
    TestTrue(TEXT("Dump should complete within MaxTicks"), Ticks < 64);

    S = AssetDumpHandler::GetAsyncFolderDumpStatus();
    TestFalse(TEXT("Status must report idle after completion"), S.bInProgress);

    // One meta.json per queued asset must exist under the scratch root. Exact count, not
    // > 0: the sweep queued R.AssetCount candidates and none of them can be skipped, so a
    // partially-drained sweep must fail here rather than pass on a single stray file.
    // Mirrors the same relation in AsyncFolderDump.CacheWritesAndHits.
    TArray<FString> MetaFiles;
    IFileManager::Get().FindFilesRecursive(
        MetaFiles, *ScratchRoot, DumpFileNames::Meta,
        /*Files=*/true, /*Directories=*/false, /*bClearFileNames=*/true);
    TestEqual(TEXT("Every queued asset was dumped to scratch root"), MetaFiles.Num(), R.AssetCount);

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncFolderDumpRejectsConcurrentTest,
    "PinWright.asset.dump.AsyncFolderDump.RejectsConcurrent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncFolderDumpRejectsConcurrentTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    // Non-recursive sweep of a tiny single-asset engine folder. The first call
    // flips state to running and registers the ticker; we issue the second
    // call before any tick, so the in-progress reject path is covered without
    // pumping a heavy folder.
    const AssetDumpHandler::FFolderDumpStart First =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, ScratchRoot);

    TestTrue(TEXT("First start must succeed (no error)"), First.ErrorCode.IsEmpty());

    if (First.AssetCount == 0)
    {
        AddInfo(FString::Printf(TEXT("No assets under %s; cannot validate concurrent reject."), AsyncDumpTestFolder));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    const AssetDumpHandler::FFolderDumpStart Second =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, ScratchRoot);

    TestFalse(TEXT("Second start must report error"), Second.ErrorCode.IsEmpty());
    TestEqual(TEXT("ErrorCode must be DUMP_IN_PROGRESS"),
        Second.ErrorCode, FString(TEXT("DUMP_IN_PROGRESS")));
    TestTrue(TEXT("ErrorMessage must name the running folderPath"),
        Second.ErrorMessage.Contains(AsyncDumpTestFolder));

    // Drain the running sweep before returning so the next test starts clean.
    DrainFolderDump();

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// ============================================================================
// AssetDumpHandler.Handlers.AssetDump.FolderSweepPendingObjectPathShape
// Constructs real FAssetData rows and calls MakePendingDumpPath — the production
// helper used by StartAsyncFolderDump to build each queue entry's load path.
// Post object-path-loading fix the helper returns the FULL OBJECT PATH
// (GetObjectPathString()), so packages whose inner asset name differs from the
// package tail (engine "Bake Out Materials" output: M_<mesh>_<mat>_<GUID>
// containing MI_M_...) load the exact registry object. The dump DIRECTORY
// stays package-named: DumpSingleAsset derives it from the loaded asset's
// outermost package, not from the pending path.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpFolderPendingObjectPathShapeTest,
    "PinWright.asset.dump.FolderSweepPendingObjectPathShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpFolderPendingObjectPathShapeTest::RunTest(const FString& Parameters)
{
    // Ordinary top-level asset: inner name equals the package tail.
    const FName PackageName = FName(TEXT("/SampleGame/Machines/Alpha/Parent/Alpha_ParentBP"));
    const FName PackagePath = FName(TEXT("/SampleGame/Machines/Alpha/Parent"));
    const FName AssetName   = FName(TEXT("Alpha_ParentBP"));
    const FTopLevelAssetPath AssetClassPath(TEXT("/Script/Engine"), TEXT("Blueprint"));

    FAssetData Data(PackageName, PackagePath, AssetName, AssetClassPath);

    // Counterfactual: if the body of MakePendingDumpPath (AssetDumpHandler.cpp) is
    // reverted to Data.PackageName.ToString(), the mismatched bake row below
    // yields a bare package name whose LoadObject finds no object named after the
    // package tail — ASSET_LOAD_FAILED and a perpetual skip-stub loop.
    const FString PendingPath = AssetDumpHandler::MakePendingDumpPath(Data);
    TestEqual(TEXT("MakePendingDumpPath returns the full object path"),
        PendingPath, FString(TEXT("/SampleGame/Machines/Alpha/Parent/Alpha_ParentBP.Alpha_ParentBP")));
    TestEqual(TEXT("MakePendingDumpPath matches GetObjectPathString"),
        PendingPath, Data.GetObjectPathString());

    // Mismatched-inner-name row (bake-out shape): the object path must carry the
    // registry asset name, which a package-name reduction cannot reconstruct.
    const FAssetData BakeData(
        FName(TEXT("/Game/Meshes/M_Gear_Mat_1234ABCD")),
        FName(TEXT("/Game/Meshes")),
        FName(TEXT("MI_M_Gear_Mat_1234ABCD")),
        FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("MaterialInstanceConstant")));
    const FString BakePendingPath = AssetDumpHandler::MakePendingDumpPath(BakeData);
    TestEqual(TEXT("Mismatched inner name resolves to its own object path"),
        BakePendingPath, FString(TEXT("/Game/Meshes/M_Gear_Mat_1234ABCD.MI_M_Gear_Mat_1234ABCD")));

    // Dump-dir invariant: reducing the pending object path back to its package
    // name yields the same package-named dump dir as the bare package string —
    // the on-disk mirror shape is unchanged by the object-path queue entries.
    const FString Root = TEXT("C:/Root");
    const FString DirFromPendingPackage = AssetDumpWriter::ResolveDumpDir(
        FPackageName::ObjectPathToPackageName(BakePendingPath), Root);
    const FString DirFromPackageName = AssetDumpWriter::ResolveDumpDir(
        FString(TEXT("/Game/Meshes/M_Gear_Mat_1234ABCD")), Root);
    TestEqual(TEXT("Pending path reduces to the package-named dump dir"),
        DirFromPendingPackage, DirFromPackageName);
    TestFalse(TEXT("Dump dir does not carry the .InnerName object suffix"),
        DirFromPendingPackage.Contains(TEXT(".MI_M_Gear_Mat_1234ABCD")));

    return true;
}

// ============================================================================
// AssetDumpHandler.SelectPrimaryAssetData
// Pure unit test of the deterministic primary-asset rule used by the folder
// sweep queue build and the skip-stub className: (1) exactly one row wins;
// (2) a row whose AssetName equals the package tail wins; (3) a case-variant
// tail match (FAssetData::IsUAsset semantics) still wins over unrelated rows;
// (4) otherwise the stable total order over (AssetName, AssetClassPath) picks
// the first row, independent of registry return order.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpSelectPrimaryAssetDataTest,
    "PinWright.asset.dump.SelectPrimaryAssetData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpSelectPrimaryAssetDataTest::RunTest(const FString& Parameters)
{
    const auto MakeRow = [](const TCHAR* Package, const TCHAR* Asset, const TCHAR* ClassName)
    {
        return FAssetData(
            FName(Package),
            FName(*FPaths::GetPath(FString(Package))),
            FName(Asset),
            FTopLevelAssetPath(TEXT("/Script/Engine"), ClassName));
    };

    // Rule 1: a single row is the primary even when its name mismatches the tail.
    {
        const TArray<FAssetData> Rows{
            MakeRow(TEXT("/Game/M_Mesh_Mat_GUID"), TEXT("MI_M_Mesh_Mat_GUID"), TEXT("MaterialInstanceConstant"))};
        const FAssetData Primary =
            AssetDumpHandler::SelectPrimaryAssetData(Rows, TEXT("/Game/M_Mesh_Mat_GUID"));
        TestEqual(TEXT("Single row is the primary"),
            Primary.AssetName, FName(TEXT("MI_M_Mesh_Mat_GUID")));
    }

    // Rule 2: the row whose AssetName equals the package tail wins, in both
    // input orders.
    {
        const FAssetData Tail  = MakeRow(TEXT("/Game/Stuff/PkgTail_X"), TEXT("PkgTail_X"), TEXT("Material"));
        const FAssetData Other = MakeRow(TEXT("/Game/Stuff/PkgTail_X"), TEXT("AAA_Other"), TEXT("Texture2D"));
        const FAssetData PrimaryA = AssetDumpHandler::SelectPrimaryAssetData(
            {Other, Tail}, TEXT("/Game/Stuff/PkgTail_X"));
        const FAssetData PrimaryB = AssetDumpHandler::SelectPrimaryAssetData(
            {Tail, Other}, TEXT("/Game/Stuff/PkgTail_X"));
        TestEqual(TEXT("Tail-named row wins (order A)"), PrimaryA.AssetName, FName(TEXT("PkgTail_X")));
        TestEqual(TEXT("Tail-named row wins (order B)"), PrimaryB.AssetName, FName(TEXT("PkgTail_X")));
    }

    // Rule 3: a case-variant tail match still wins over an unrelated row. The
    // sort fallback would pick "AAA_Other" (lexicographically first), so this
    // pins that tail matching — exact or via IsUAsset case-insensitivity —
    // takes precedence over the total-order fallback.
    {
        const FAssetData CaseTail = MakeRow(TEXT("/Game/Stuff/PkgTail_X"), TEXT("PKGTAIL_x"), TEXT("Material"));
        const FAssetData Other    = MakeRow(TEXT("/Game/Stuff/PkgTail_X"), TEXT("AAA_Other"), TEXT("Texture2D"));
        const FAssetData Primary = AssetDumpHandler::SelectPrimaryAssetData(
            {Other, CaseTail}, TEXT("/Game/Stuff/PkgTail_X"));
        TestEqual(TEXT("Case-variant tail match beats the sort fallback"),
            Primary.AssetName, FName(TEXT("PKGTAIL_x")));
    }

    // Rule 4: no tail match at all (bake-out shape: MI_ + T_ rows) — the stable
    // total order over (AssetName, AssetClassPath) picks MI_ over T_ regardless
    // of registry return order. This is the order-instability the old
    // Assets[0] className lookup flipped on.
    {
        const FAssetData Mi = MakeRow(
            TEXT("/Game/M_Mesh_Mat_GUID"), TEXT("MI_M_Mesh_Mat_GUID"), TEXT("MaterialInstanceConstant"));
        const FAssetData Tex = MakeRow(
            TEXT("/Game/M_Mesh_Mat_GUID"), TEXT("T_M_Mesh_Mat_GUID_BaseColor"), TEXT("Texture2D"));
        const FAssetData PrimaryA = AssetDumpHandler::SelectPrimaryAssetData(
            {Tex, Mi}, TEXT("/Game/M_Mesh_Mat_GUID"));
        const FAssetData PrimaryB = AssetDumpHandler::SelectPrimaryAssetData(
            {Mi, Tex}, TEXT("/Game/M_Mesh_Mat_GUID"));
        TestEqual(TEXT("Sort fallback picks MI_ over T_ (order A)"),
            PrimaryA.AssetName, FName(TEXT("MI_M_Mesh_Mat_GUID")));
        TestEqual(TEXT("Sort fallback picks MI_ over T_ (order B)"),
            PrimaryB.AssetName, FName(TEXT("MI_M_Mesh_Mat_GUID")));
    }

    // Rule 4 tiebreak: identical AssetName falls through to AssetClassPath so
    // the order is still total.
    {
        const FAssetData DupMat = MakeRow(TEXT("/Game/DupPkg"), TEXT("Dup"), TEXT("Material"));
        const FAssetData DupTex = MakeRow(TEXT("/Game/DupPkg"), TEXT("Dup"), TEXT("Texture2D"));
        const FAssetData PrimaryA = AssetDumpHandler::SelectPrimaryAssetData(
            {DupTex, DupMat}, TEXT("/Game/DupPkg"));
        const FAssetData PrimaryB = AssetDumpHandler::SelectPrimaryAssetData(
            {DupMat, DupTex}, TEXT("/Game/DupPkg"));
        TestEqual(TEXT("Class path breaks AssetName ties (order A)"),
            PrimaryA.AssetClassPath.ToString(), FString(TEXT("/Script/Engine.Material")));
        TestEqual(TEXT("Class path breaks AssetName ties (order B)"),
            PrimaryB.AssetClassPath.ToString(), FString(TEXT("/Script/Engine.Material")));
    }

    return true;
}

// ============================================================================
// AssetDumpHandler.FolderSweepSkipsStandaloneMapBuildDataAndGeneratedMeshes
// UE-managed and map-generated helper packages must not be queued by broad
// folder sweeps unless map data is explicitly opted in where applicable.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpFolderSkipsStandaloneMapBuildDataAndGeneratedMeshesTest,
    "PinWright.asset.dump.FolderSweepSkipsStandaloneMapBuildDataAndGeneratedMeshes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpFolderSkipsStandaloneMapBuildDataAndGeneratedMeshesTest::RunTest(const FString& Parameters)
{
    const FTopLevelAssetPath AssetClassPath(TEXT("/Script/Engine"), TEXT("Blueprint"));

    const FAssetData ExternalActor(
        FName(TEXT("/SampleGame/__ExternalActors__/Maps/A/ABCDEF")),
        FName(TEXT("/SampleGame/__ExternalActors__/Maps/A")),
        FName(TEXT("ABCDEF")),
        AssetClassPath);
    TestTrue(TEXT("Exact __ExternalActors__ segment is skipped"),
        AssetDumpHandler::ShouldSkipFolderDumpAsset(ExternalActor));

    const FAssetData ExternalObject(
        FName(TEXT("/Game/__ExternalObjects__/Maps/A/GHIJKL")),
        FName(TEXT("/Game/__ExternalObjects__/Maps/A")),
        FName(TEXT("GHIJKL")),
        AssetClassPath);
    TestTrue(TEXT("Exact __ExternalObjects__ segment is skipped"),
        AssetDumpHandler::ShouldSkipFolderDumpAsset(ExternalObject));

    const FAssetData SimilarName(
        FName(TEXT("/SampleGame/__ExternalActorsBackup__/Maps/A/RegularAsset")),
        FName(TEXT("/SampleGame/__ExternalActorsBackup__/Maps/A")),
        FName(TEXT("RegularAsset")),
        AssetClassPath);
    TestFalse(TEXT("Similar folder names are not skipped"),
        AssetDumpHandler::ShouldSkipFolderDumpAsset(SimilarName));

    const FAssetData AssetWithReservedName(
        FName(TEXT("/SampleGame/RegularFolder/__ExternalActors__")),
        FName(TEXT("/SampleGame/RegularFolder")),
        FName(TEXT("__ExternalActors__")),
        AssetClassPath);
    TestFalse(TEXT("Asset names alone do not trigger the folder skip"),
        AssetDumpHandler::ShouldSkipFolderDumpAsset(AssetWithReservedName));

    const FAssetData RegularAsset(
        FName(TEXT("/SampleGame/Machines/Alpha/Parent/Alpha_ParentBP")),
        FName(TEXT("/SampleGame/Machines/Alpha/Parent")),
        FName(TEXT("Alpha_ParentBP")),
        AssetClassPath);
    TestFalse(TEXT("Regular assets are not skipped"),
        AssetDumpHandler::ShouldSkipFolderDumpAsset(RegularAsset));

    const FAssetData WorldAsset(
        FName(TEXT("/Engine/Maps/Entry")),
        FName(TEXT("/Engine/Maps")),
        FName(TEXT("Entry")),
        FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("World")));
    TestTrue(TEXT("World assets are skipped by default"),
        AssetDumpHandler::ShouldSkipFolderDumpAsset(WorldAsset));
    TestFalse(TEXT("World assets are included when opted in"),
        AssetDumpHandler::ShouldSkipFolderDumpAsset(WorldAsset, /*bIncludeLevels=*/true));

    // Map-package alias: a non-World asset (e.g. MapBuildDataRegistry embedded in
    // the level package) whose PackageFlags carry PKG_ContainsMap. Queueing this
    // row would queue the same package as the World and LoadObject would resolve
    // back to the World, defeating the class-path filter. Repro from board
    // F-asset-dump-folder-skip-levels-by-default #5.
    const FAssetData MapPackageAlias(
        FName(TEXT("/Game/Maps/Cliffs/Assets_Cliffs")),
        FName(TEXT("/Game/Maps/Cliffs")),
        FName(TEXT("Mountains_BuiltData")),
        FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("MapBuildDataRegistry")),
        FAssetDataTagMap(),
        TArrayView<const int32>(),
        /*InPackageFlags=*/PKG_ContainsMap);
    TestTrue(TEXT("Map-package alias rows are skipped by default"),
        AssetDumpHandler::ShouldSkipFolderDumpAsset(MapPackageAlias));
    TestFalse(TEXT("Map-package alias rows are included when opted in"),
        AssetDumpHandler::ShouldSkipFolderDumpAsset(MapPackageAlias, /*bIncludeLevels=*/true));

    // Counterfactual: if the class-path and Maps/_GENERATED checks in
    // AssetDumpHandler.cpp are reverted, the standalone _BuiltData and generated
    // helper mesh assertions fail because both rows pass the existing World and
    // PKG_ContainsMap filters.
    const FAssetData StandaloneBuiltData(
        FName(TEXT("/Game/Maps/Cliffs/Assets_Cliffs_BuiltData")),
        FName(TEXT("/Game/Maps/Cliffs")),
        FName(TEXT("Assets_Cliffs_BuiltData")),
        FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("MapBuildDataRegistry")));
    TestTrue(TEXT("Standalone _BuiltData rows (no PKG_ContainsMap) are skipped by default"),
        AssetDumpHandler::ShouldSkipFolderDumpAsset(StandaloneBuiltData));
    TestFalse(TEXT("Standalone _BuiltData rows are included when opted in"),
        AssetDumpHandler::ShouldSkipFolderDumpAsset(StandaloneBuiltData, /*bIncludeLevels=*/true));

    const FTopLevelAssetPath StaticMeshClassPath(TEXT("/Script/Engine"), TEXT("StaticMesh"));

    const FAssetData GeneratedMapHelperMesh(
        FName(TEXT("/Game/Maps/_GENERATED/L_SampleMap/LightmassImportanceVolume_0Mesh_BC9F3DE5")),
        FName(TEXT("/Game/Maps/_GENERATED/L_SampleMap")),
        FName(TEXT("LightmassImportanceVolume_0Mesh_BC9F3DE5")),
        StaticMeshClassPath);
    TestTrue(TEXT("StaticMesh rows under /Maps/_GENERATED/ are skipped"),
        AssetDumpHandler::ShouldSkipFolderDumpAsset(GeneratedMapHelperMesh));

    const FAssetData SimilarGeneratedElsewhere(
        FName(TEXT("/Game/Props/_GENERATED/L_SampleMap/RegularMesh")),
        FName(TEXT("/Game/Props/_GENERATED/L_SampleMap")),
        FName(TEXT("RegularMesh")),
        StaticMeshClassPath);
    TestFalse(TEXT("_GENERATED outside /Maps/_GENERATED/ is not skipped"),
        AssetDumpHandler::ShouldSkipFolderDumpAsset(SimilarGeneratedElsewhere));

    const FAssetData SimilarMapsGeneratedName(
        FName(TEXT("/Game/Maps/_GENERATEDBackup/L_SampleMap/RegularMesh")),
        FName(TEXT("/Game/Maps/_GENERATEDBackup/L_SampleMap")),
        FName(TEXT("RegularMesh")),
        StaticMeshClassPath);
    TestFalse(TEXT("Adjacent Maps segment with a different generated folder name is not skipped"),
        AssetDumpHandler::ShouldSkipFolderDumpAsset(SimilarMapsGeneratedName));

    return true;
}

// ============================================================================
// AssetDumpHandler.AsyncFolderDump.LevelsExcludedByDefault
// /Engine/Maps non-recursive includes Entry.umap (UWorld), and some engine
// installs also include sidecar assets such as Entry_BuiltData.uasset. With the
// default bIncludeLevels=false, the UWorld entry is filtered out; opting in
// queues at least one additional asset and the dump runs.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncFolderDumpLevelsExcludedByDefaultTest,
    "PinWright.asset.dump.AsyncFolderDump.LevelsExcludedByDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncFolderDumpLevelsExcludedByDefaultTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    // Default: bIncludeLevels=false. The UWorld entry is filtered out, but the
    // folder may still contain non-level sidecar assets.
    const AssetDumpHandler::FFolderDumpStart Excluded =
        AssetDumpHandler::StartAsyncFolderDump(
            TEXT("/Engine/Maps"), /*bRecursive=*/false, ScratchRoot, /*bIncludeLevels=*/false);

    TestTrue(TEXT("Default exclude must succeed"), Excluded.ErrorCode.IsEmpty());
    if (Excluded.AssetCount > 0)
    {
        const int32 DefaultTicks = DrainFolderDump();
        TestTrue(TEXT("Default dump should complete within MaxTicks"), DefaultTicks < 64);
        TestFalse(TEXT("Status must report idle after default completion"),
            AssetDumpHandler::GetAsyncFolderDumpStatus().bInProgress);
    }
    else
    {
        TestFalse(TEXT("State must remain idle when nothing was queued"),
            AssetDumpHandler::GetAsyncFolderDumpStatus().bInProgress);
    }

    // Opt-in: bIncludeLevels=true. Same folder, but now the level is queued.
    const AssetDumpHandler::FFolderDumpStart Included =
        AssetDumpHandler::StartAsyncFolderDump(
            TEXT("/Engine/Maps"), /*bRecursive=*/false, ScratchRoot, /*bIncludeLevels=*/true);

    TestTrue(TEXT("Opt-in start must succeed"), Included.ErrorCode.IsEmpty());

    // /Engine/Maps may be stripped in some headless configurations; if so,
    // bail cleanly as the other async tests in this file do.
    if (Included.AssetCount == Excluded.AssetCount)
    {
        AddInfo(TEXT("/Engine/Maps exposed no additional level assets in this configuration; skipping opt-in tick test."));
        if (Included.AssetCount > 0)
        {
            DrainFolderDump();
        }
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    TestTrue(TEXT("Opt-in must queue at least one additional level asset"),
        Included.AssetCount > Excluded.AssetCount);
    TestTrue(TEXT("Status must be in-progress after opt-in start"),
        AssetDumpHandler::GetAsyncFolderDumpStatus().bInProgress);

    const int32 Ticks = DrainFolderDump();
    TestTrue(TEXT("Opt-in dump should complete within MaxTicks"), Ticks < 64);
    TestFalse(TEXT("Status must report idle after opt-in completion"),
        AssetDumpHandler::GetAsyncFolderDumpStatus().bInProgress);

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAsyncFolderDumpDeduplicatesByPackagePathTest,
    "PinWright.asset.dump.AsyncFolderDump.DeduplicatesByPackagePath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAsyncFolderDumpDeduplicatesByPackagePathTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const AssetDumpHandler::FFolderDumpStart R =
        AssetDumpHandler::StartAsyncFolderDump(
            TEXT("/Engine/Maps"), /*bRecursive=*/false, ScratchRoot, /*bIncludeLevels=*/true);

    TestTrue(TEXT("Start must succeed (no error)"), R.ErrorCode.IsEmpty());

    // /Engine/Maps may be absent in some stripped engine configurations.
    if (R.AssetCount == 0)
    {
        AddInfo(TEXT("DeduplicatesByPackagePath: /Engine/Maps has no assets in this configuration; skipping."));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    const int32 KickoffCount = R.AssetCount;
    TestEqual(TEXT("Dedup start queues every unique package"), R.QueuedCount, KickoffCount);
    TestEqual(TEXT("Dedup start has no cache hits on fresh root"), R.UnchangedCount, 0);

    TestTrue(TEXT("Status must be in-progress after start"),
        AssetDumpHandler::GetAsyncFolderDumpStatus().bInProgress);
    const FString JobTicketId = AttachFolderDumpJobTicket();
    TestFalse(TEXT("JobTicketId must be allocated"), JobTicketId.IsEmpty());

    const int32 Ticks = DrainFolderDump();
    TestTrue(TEXT("Dump should complete within MaxTicks"), Ticks < 64);
    TestFalse(TEXT("Status must report idle after completion"),
        AssetDumpHandler::GetAsyncFolderDumpStatus().bInProgress);

    FJobTicket OutTicket;
    if (FPluginState::Get().GetJobRegistry().Get(JobTicketId, OutTicket) && OutTicket.Result.IsValid())
    {
        const int32 AssetCount = OutTicket.Result->GetIntegerField(TEXT("assetCount"));
        const int32 Queued = OutTicket.Result->GetIntegerField(TEXT("queued"));
        const int32 Dumped = OutTicket.Result->GetIntegerField(TEXT("dumped"));
        const int32 Unchanged = OutTicket.Result->GetIntegerField(TEXT("unchanged"));
        const int32 SkipCount = OutTicket.Result->GetIntegerField(TEXT("skipCount"));
        TestEqual(TEXT("Dedup result assetCount"), AssetCount, KickoffCount);
        TestEqual(TEXT("Dedup result queued"), Queued, KickoffCount);
        TestEqual(TEXT("Dedup result unchanged"), Unchanged, 0);
        TestEqual(TEXT("Dedup result skipCount"), SkipCount, 0);
        TestEqual(TEXT("Dedup result counter invariant"), AssetCount, Dumped + Unchanged + SkipCount);
    }
    else
    {
        AddError(TEXT("Dedup job ticket lookup failed or Result missing"));
    }

    // Count meta.json files written to disk.
    TArray<FString> MetaFiles;
    IFileManager::Get().FindFilesRecursive(
        MetaFiles, *ScratchRoot, DumpFileNames::Meta,
        /*Files=*/true, /*Directories=*/false, /*bClearFileNames=*/true);

    const int32 MetaJsonCount = MetaFiles.Num();

    // Primary assertion: kickoff count == on-disk asset count.
    // A mismatch means the Pending array contained duplicate package paths that
    // the dedup guard should have dropped.
    TestEqual(TEXT("Kickoff AssetCount must match on-disk meta.json count"),
        KickoffCount, MetaJsonCount);

    // Independent assertion: each meta.json must live in a unique parent directory
    // (one asset == one dump directory).
    TSet<FString> ParentDirs;
    for (const FString& MetaPath : MetaFiles)
    {
        ParentDirs.Add(FPaths::GetPath(MetaPath));
    }
    TestEqual(TEXT("Unique parent directories must match meta.json count"),
        ParentDirs.Num(), MetaJsonCount);

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// ============================================================================
// AssetDumpHandler.AsyncFolder.SkipStub
// Failed-load assets in the async folder sweep get a tiny stub meta.json on
// disk and an aggregated skipCount in the completion result.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerSkipStubTest,
    "PinWright.asset.dump.AsyncFolder.SkipStub",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerSkipStubTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    // Kick off a real folder dump so RootDir and the ticker are wired.
    const AssetDumpHandler::FFolderDumpStart Start =
        AssetDumpHandler::StartAsyncFolderDump(AsyncDumpTestFolder, /*bRecursive=*/false, ScratchRoot);
    TestTrue(TEXT("StartAsyncFolderDump must succeed"), Start.ErrorCode.IsEmpty());

    // StartAsyncFolderDump does not allocate a JobTicketId; that is wired by the
    // asset.dump_folder RPC handler via Ctx.StartJob. This test bypasses the RPC
    // layer, so allocate a ticket directly the same way the handler does, then
    // store it on the shared state so FinalizeAsyncDump completes it.
    const TSharedRef<FJsonObject> StartedParams = MakeShared<FJsonObject>();
    const FString JobTicketId =
        FPluginState::Get().GetJobRegistry().Start(TEXT("asset.dump_folder"), StartedParams);
    TestFalse(TEXT("JobTicketId must be allocated"), JobTicketId.IsEmpty());
    FPluginState::Get().GetFolderDump().JobTicketId = JobTicketId;

    // Inject a synthetic unloadable entry. The load will fail, exercising the
    // skip-stub branch in TickFolderDump. The entry mirrors what the queue
    // build produces from a registry row: full object path + package identity.
    const FString SyntheticPath = TEXT("/Engine/EngineDamageTypes/__SyntheticUnloadable_SkipStubTest");
    {
        FAsyncFolderDumpState& State = FPluginState::Get().GetFolderDump();
        FPendingDumpEntry Entry;
        Entry.ObjectPath = SyntheticPath + TEXT(".__SyntheticUnloadable_SkipStubTest");
        Entry.PackageName = SyntheticPath;
        Entry.ClassPath = FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("DamageType"));
        State.PendingAssets.Add(MoveTemp(Entry));
        State.TotalAssetCount++;
    }

    DrainFolderDump();

    // Counterfactual: if the stub-write + SkipCount-increment in TickFolderDump's failure
    // branch and the skipCount line in FinalizeAsyncDump are reverted, no stub meta.json
    // appears on disk and OutTicket.Result.skipCount is missing/zero.

    // The stub dir mirrors the package path under the scratch root, with the leading
    // '/' stripped (matches AssetDumpWriter::ResolveDumpDir).
    const FString StubDir = AssetDumpWriter::ResolveDumpDir(SyntheticPath, ScratchRoot);
    const FString StubMetaPath = StubDir / DumpFileNames::Meta;
    const FString StubPropsPath = StubDir / DumpFileNames::Properties;

    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    TestTrue(TEXT("Stub meta.json must exist for the synthetic unloadable asset"),
        PF.FileExists(*StubMetaPath));
    TestFalse(TEXT("No properties.json beside the skip stub"),
        PF.FileExists(*StubPropsPath));

    // The synthetic package has no registry row and no file on disk, so its
    // source fingerprint is Uncached — the stub cache writer's eligibility gate
    // must refuse it. A stub whose source can never be fingerprinted has to
    // keep re-queuing rather than being cached as permanently "fresh".
    // (Stub records for fingerprintable packages are covered by
    // AsyncFolder.SkipStubCacheRecord below.)
    TestFalse(TEXT("No .dumpcache.json beside the stub for an unfingerprintable package"),
        PF.FileExists(*(StubDir / AssetDumpCache::DumpCacheFileName)));

    // Validate the stub contents match the expected skipped:true shape.
    {
        FString StubBody;
        if (FFileHelper::LoadFileToString(StubBody, *StubMetaPath))
        {
            TSharedPtr<FJsonObject> StubMeta;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(StubBody);
            if (FJsonSerializer::Deserialize(Reader, StubMeta) && StubMeta.IsValid())
            {
                TestTrue(TEXT("Stub meta has skipped:true"),
                    StubMeta->HasField(TEXT("skipped")) && StubMeta->GetBoolField(TEXT("skipped")));
                // Per E-asset-dump-distinguish-file-missing-from-load-failed, the synthetic
                // path lacks both an on-disk .uasset and an in-memory UPackage, so the
                // pre-LoadObject existence check fires and the stub records ASSET_FILE_MISSING.
                // Equality, not a disjunction: this is what fails if the pre-check at
                // AssetDumpHandler.cpp:2092-2098 is removed and the stub falls through to
                // ASSET_LOAD_FAILED from the LoadObject branch at :2100-2103.
                TestEqual(TEXT("Stub meta.skipReason is ASSET_FILE_MISSING"),
                    StubMeta->GetStringField(TEXT("skipReason")),
                    FString(AssetDumpErrorCodes::AssetFileMissing));
                // Prefix check: the queue entry now carries both the package
                // name and the object path; either form identifies the package.
                TestTrue(TEXT("Stub meta.assetPath identifies the synthetic package"),
                    StubMeta->GetStringField(TEXT("assetPath")).StartsWith(SyntheticPath));
            }
            else
            {
                AddError(TEXT("Stub meta.json could not be parsed as JSON"));
            }
        }
        else
        {
            AddError(FString::Printf(TEXT("Stub meta.json not readable at %s"), *StubMetaPath));
        }
    }

    FJobTicket OutTicket;
    if (FPluginState::Get().GetJobRegistry().Get(JobTicketId, OutTicket) && OutTicket.Result.IsValid())
    {
        const int32 SkipCountField = (int32)OutTicket.Result->GetIntegerField(TEXT("skipCount"));
        TestTrue(TEXT("Completion result skipCount must be >= 1"), SkipCountField >= 1);

        const TArray<TSharedPtr<FJsonValue>>* SkippedEntries = nullptr;
        TestTrue(TEXT("Completion result carries skipped asset details"),
            OutTicket.Result->TryGetArrayField(TEXT("skipped"), SkippedEntries)
                && SkippedEntries != nullptr
                && SkippedEntries->Num() >= 1);
        if (SkippedEntries && SkippedEntries->Num() > 0)
        {
            const TSharedPtr<FJsonObject> Skip = (*SkippedEntries)[0]->AsObject();
            TestTrue(TEXT("Skipped detail is an object"), Skip.IsValid());
            if (Skip.IsValid())
            {
                TestEqual(TEXT("Skipped detail asset path"),
                    Skip->GetStringField(TEXT("assetPath")),
                    SyntheticPath + TEXT(".__SyntheticUnloadable_SkipStubTest"));
                // Equality pins the pre-LoadObject existence check at
                // AssetDumpHandler.cpp:2092-2098; a disjunction with ASSET_LOAD_FAILED would
                // still pass with that pre-check deleted.
                TestEqual(TEXT("Skipped detail carries the dump error code"),
                    Skip->GetStringField(TEXT("code")),
                    FString(AssetDumpErrorCodes::AssetFileMissing));
                TestFalse(TEXT("Skipped detail carries the dump error message"),
                    Skip->GetStringField(TEXT("message")).IsEmpty());
            }
        }
    }
    else
    {
        AddError(TEXT("Job ticket lookup failed or Result missing"));
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// ============================================================================
// AssetDumpHandler.AsyncFolder.SkipStubCacheRecord
// A skip stub for a FINGERPRINTABLE package (on-disk, registry-known, but the
// queued object fails to load) gets a .dumpcache.json with skipped:true, so
// the next sweep does NOT re-queue it. A genuine source change (content
// re-save, new registry saved-hash) re-queues it, and the following dump
// self-heals the stub into a real dump with a non-stub record.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerSkipStubCacheRecordTest,
    "PinWright.asset.dump.AsyncFolder.SkipStubCacheRecord",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerSkipStubCacheRecordTest::RunTest(const FString& Parameters)
{
    AssetDumpMismatchedNameFixture::FMismatchedNameAsset Fixture;
    FString FixtureError;
    if (!AssetDumpMismatchedNameFixture::Create(Fixture, FixtureError))
    {
        AddError(FString::Printf(TEXT("Skip-stub fixture creation failed: %s"), *FixtureError));
        AssetDumpMismatchedNameFixture::Cleanup(Fixture);
        return true;
    }

    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpHandlerTests") / FGuid::NewGuid().ToString();

    const AssetDumpHandler::FFolderDumpStart Start =
        AssetDumpHandler::StartAsyncFolderDump(Fixture.FolderPath, /*bRecursive=*/false, ScratchRoot);
    TestTrue(TEXT("Fixture sweep start must succeed"), Start.ErrorCode.IsEmpty());
    TestEqual(TEXT("Fixture package is queued"), Start.QueuedCount, 1);
    if (!Start.ErrorCode.IsEmpty() || Start.QueuedCount != 1)
    {
        // Drain any sweep the successful start left running so the next test
        // does not hit DUMP_IN_PROGRESS.
        if (Start.ErrorCode.IsEmpty() && Start.AssetCount > 0)
        {
            DrainFolderDump();
        }
        AssetDumpMismatchedNameFixture::Cleanup(Fixture);
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    // Swap the queued entry for one whose object path names a nonexistent
    // inner object of the SAME on-disk package: the package file exists (so
    // the pre-LoadObject existence check passes and the fingerprint is
    // cacheable) but LoadObject fails — the fingerprintable skip-stub case.
    {
        FAsyncFolderDumpState& State = FPluginState::Get().GetFolderDump();
        TestEqual(TEXT("Sweep queued exactly one pending entry"), State.PendingAssets.Num(), 1);
        State.PendingAssets.Reset();
        FPendingDumpEntry Bogus;
        Bogus.ObjectPath = Fixture.PackagePath + TEXT(".__PW_NoSuchInner");
        Bogus.PackageName = Fixture.PackagePath;
        Bogus.ClassPath = FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("MaterialInstanceConstant"));
        State.PendingAssets.Add(MoveTemp(Bogus));
    }

    FJobTicket Ticket;
    TestTrue(TEXT("Skip-stub folder dump completes"), CompleteCurrentFolderDump(Ticket));
    if (Ticket.Result.IsValid())
    {
        TestTrue(TEXT("Completion result skipCount must be >= 1"),
            (int32)Ticket.Result->GetIntegerField(TEXT("skipCount")) >= 1);
    }

    const FString StubDir = AssetDumpWriter::ResolveDumpDir(Fixture.PackagePath, ScratchRoot);
    TSharedPtr<FJsonObject> StubMeta = LoadJsonObjectFromFile(StubDir / DumpFileNames::Meta);
    TestTrue(TEXT("Stub meta.json parses"), StubMeta.IsValid());
    if (StubMeta.IsValid())
    {
        TestTrue(TEXT("Stub meta has skipped:true"),
            StubMeta->HasField(TEXT("skipped")) && StubMeta->GetBoolField(TEXT("skipped")));
        TestEqual(TEXT("Stub meta.skipReason is ASSET_LOAD_FAILED"),
            StubMeta->GetStringField(TEXT("skipReason")), FString(AssetDumpErrorCodes::AssetLoadFailed));
        // className comes from the CARRIED entry ClassPath, not an
        // order-unstable registry Assets[0] lookup.
        TestTrue(TEXT("Stub meta.className reflects the carried class path"),
            StubMeta->GetStringField(TEXT("className")).Contains(TEXT("MaterialInstanceConstant")));
        TestTrue(TEXT("Stub meta.assetPath identifies the package"),
            StubMeta->GetStringField(TEXT("assetPath")).StartsWith(Fixture.PackagePath));
    }

    // The stub cache record: skipped marker set, meta.json the only written file.
    AssetDumpCache::FAssetDumpCacheRecord StubRecord;
    FString ReadError;
    TestTrue(TEXT("Stub cache record written for the fingerprintable package"),
        AssetDumpCache::ReadCacheRecord(StubDir, StubRecord, ReadError));
    TestTrue(TEXT("Stub record has bSkipped"), StubRecord.bSkipped);
    TestEqual(TEXT("Stub record skipReason"),
        StubRecord.SkipReason, FString(AssetDumpErrorCodes::AssetLoadFailed));
    TestEqual(TEXT("Stub record packageName"), StubRecord.PackageName, Fixture.PackagePath);
    TestEqual(TEXT("Stub record lists exactly one written file"), StubRecord.WrittenFiles.Num(), 1);
    TestTrue(TEXT("Stub record written file is meta.json"),
        StubRecord.WrittenFiles.Contains(FString(DumpFileNames::Meta)));

    // Counterfactual: pre-fix the skip branch never wrote a cache record, so
    // this second sweep re-queued the package on every run — the perpetual
    // stub-rewrite loop for the 4 bake packages.
    const AssetDumpHandler::FFolderDumpStart Second =
        AssetDumpHandler::StartAsyncFolderDump(Fixture.FolderPath, /*bRecursive=*/false, ScratchRoot);
    TestTrue(TEXT("Second sweep start must succeed"), Second.ErrorCode.IsEmpty());
    TestEqual(TEXT("Second sweep does not re-queue the stubbed package"), Second.QueuedCount, 0);
    TestEqual(TEXT("Second sweep reports the stub unchanged"), Second.UnchangedCount, 1);
    FJobTicket SecondTicket;
    TestTrue(TEXT("Second folder dump completes"), CompleteCurrentFolderDump(SecondTicket));

    // Genuine source change: content re-save gives the package a new registry
    // saved-hash, which must invalidate the stub record and re-queue.
    FString ResaveError;
    if (!AssetDumpMismatchedNameFixture::ResaveWithContentChange(Fixture, ResaveError))
    {
        AddError(FString::Printf(TEXT("Fixture content re-save failed: %s"), *ResaveError));
        AssetDumpMismatchedNameFixture::Cleanup(Fixture);
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    const AssetDumpHandler::FFolderDumpStart Third =
        AssetDumpHandler::StartAsyncFolderDump(Fixture.FolderPath, /*bRecursive=*/false, ScratchRoot);
    TestTrue(TEXT("Third sweep start must succeed"), Third.ErrorCode.IsEmpty());
    TestEqual(TEXT("Source change re-queues the stubbed package"), Third.QueuedCount, 1);
    FJobTicket ThirdTicket;
    TestTrue(TEXT("Third folder dump completes"), CompleteCurrentFolderDump(ThirdTicket));

    // The re-dump self-heals: queue rebuilt from the registry row carries the
    // REAL object path, so the stub is replaced by a real dump + non-stub record.
    TSharedPtr<FJsonObject> HealedMeta = LoadJsonObjectFromFile(StubDir / DumpFileNames::Meta);
    TestTrue(TEXT("Healed meta.json parses"), HealedMeta.IsValid());
    if (HealedMeta.IsValid())
    {
        TestFalse(TEXT("Healed meta.json carries no skipped marker"),
            HealedMeta->HasField(TEXT("skipped")));
    }
    TestTrue(TEXT("Healed dump wrote properties.json"),
        IFileManager::Get().FileExists(*(StubDir / DumpFileNames::Properties)));
    AssetDumpCache::FAssetDumpCacheRecord HealedRecord;
    TestTrue(TEXT("Healed cache record reads back"),
        AssetDumpCache::ReadCacheRecord(StubDir, HealedRecord, ReadError));
    TestFalse(TEXT("Healed record is not a skip stub"), HealedRecord.bSkipped);

    AssetDumpMismatchedNameFixture::Cleanup(Fixture);
    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}
