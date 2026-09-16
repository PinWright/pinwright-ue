// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for board B-asset-save-clobbers-out-of-band-change:
// asset.save wrote the RESIDENT in-memory package with no comparison against the .uasset on
// disk, so any out-of-band working-tree change under a live editor - a git checkout, a
// `git reset --hard` between fix-workflow iterations, an external revert, another tool's or a
// second editor's write - was silently overwritten by the next save of that package, and the
// caller got saved:true.
//
// The three tests are deliberately the three DIRECTIONS, not one:
//   1. ...UnchangedFileStillSaves         - no false positive. A package whose file nobody
//                                           touched still saves normally. Without this a fix
//                                           that refuses EVERY save would pass test 2.
//   2. ...OutOfBandChangeIsRefused        - no false negative. The out-of-band bytes survive and
//                                           the caller is told, with both hashes.
//   3. ...OverrideOverwritesDeliberately  - the escape hatch exists and is a SEPARATE flag from
//                                           `force`. Test 2 passes force:true and is still
//                                           refused, which is the half of that contract that can
//                                           regress silently.
//
// Everything runs through the asset.save RPC rather than the guard helper, so the tests compile
// and FAIL on the pre-fix tree instead of failing to build: pre-fix, test 2's asset.save answers
// a success with saved:true and the .uasset on disk is left holding the in-memory revision, so
// all three of its assertions (not-a-success, the error code, the untouched bytes) go red.
//
// The out-of-band write is simulated on a fixture USoundClass this file creates under a
// GUID-suffixed scratch folder and deletes in teardown - never on project content. It is a raw
// byte copy of a previously saved revision over the live .uasset, which is exactly what a
// `git checkout -- <file>` does and exactly what the editor is never told about.
//
// Assertions judge FILE BYTES, not the reported saved flag. The freshness verdict inside
// SaveAssetToDiskReportingPresence is a timestamp/size comparison, and two revisions of a
// USoundClass differing only in a float are the same length - so on a host whose filesystem
// timestamp granularity swallows the gap between two saves, `saved` can read false for a write
// that genuinely happened. The bytes cannot lie either way.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundClass.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

// Named namespace, not anonymous: with bUseUnity=true an anonymous-namespace fixture becomes
// visible at global scope once Unity merges this TU with its neighbours (see TestAssetTeardown.h).
namespace PinWrightAssetSaveDiskGuardFixture
{
    // Three distinct revisions, far enough apart that no tolerance question arises:
    //   Baseline - the revision an out-of-band writer puts back on disk.
    //   Resident - the revision the editor has in memory and on disk when the test starts.
    //   Edited   - a fresh caller edit made on top of Resident, the thing a save would write.
    constexpr float BaselineVolume = 0.125f;
    constexpr float ResidentVolume = 0.500f;
    constexpr float EditedVolume   = 0.875f;

    struct FDiskGuardAsset
    {
        FString FolderPath;
        FString PackagePath;
        FString AssetName;
        FString ObjectPath;
        FString PackageFilename;
        FString BaselineFilename;
        bool bBuilt = false;
    };

    inline bool SaveDiskGuardPackage(FAutomationTestBase& Test, const TCHAR* What,
                                     UPackage* Package, USoundClass* SoundClass,
                                     const FString& Filename)
    {
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        return Test.TestTrue(What, UPackage::SavePackage(Package, SoundClass, *Filename, SaveArgs));
    }

    // Leaves the fixture in the state every test starts from: the .uasset on disk and the
    // resident UPackage both hold ResidentVolume (so UPackage::GetSavedHash and the file's own
    // summary hash agree), and a byte copy of an OLDER revision (BaselineVolume) is parked aside
    // ready to be dropped over the file as the out-of-band write.
    inline bool BuildDiskGuardAsset(FAutomationTestBase& Test, FDiskGuardAsset& Out)
    {
        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Out.FolderPath = FString::Printf(TEXT("/Game/__PW_AssetSaveDiskGuard/%s"), *Suffix);
        Out.AssetName  = FString::Printf(TEXT("PW_DiskGuard_%s"), *Suffix);
        Out.PackagePath = FString::Printf(TEXT("%s/%s"), *Out.FolderPath, *Out.AssetName);
        Out.ObjectPath  = FString::Printf(TEXT("%s.%s"), *Out.PackagePath, *Out.AssetName);

        if (!Test.TestTrue(TEXT("fixture package path resolves to a filename"),
                FPackageName::TryConvertLongPackageNameToFilename(
                    Out.PackagePath, Out.PackageFilename, FPackageName::GetAssetPackageExtension())))
        {
            return false;
        }

        Out.BaselineFilename = FPaths::ProjectIntermediateDir()
            / TEXT("PinWrightAssetSaveDiskGuard") / (Out.AssetName + TEXT(".baseline"));
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Out.BaselineFilename), /*Tree=*/true);

        UPackage* Package = CreatePackage(*Out.PackagePath);
        if (!Test.TestNotNull(TEXT("fixture package created"), Package))
        {
            return false;
        }
        Out.bBuilt = true;

        USoundClass* SoundClass = NewObject<USoundClass>(
            Package, FName(*Out.AssetName), RF_Public | RF_Standalone);
        if (!Test.TestNotNull(TEXT("fixture sound class created"), SoundClass))
        {
            return false;
        }

        // Revision 1: the bytes an out-of-band writer will later restore.
        SoundClass->Properties.Volume = BaselineVolume;
        if (!SaveDiskGuardPackage(Test, TEXT("baseline revision saved to disk"),
                Package, SoundClass, Out.PackageFilename))
        {
            return false;
        }
        if (!Test.TestTrue(TEXT("baseline bytes copied aside"),
                IFileManager::Get().Copy(*Out.BaselineFilename, *Out.PackageFilename,
                    /*Replace=*/true, /*EvenIfReadOnly=*/true) == COPY_OK))
        {
            return false;
        }

        // Revision 2: the state the editor and the file agree on when a test begins. This save is
        // what puts the matching hash into both UPackage::GetSavedHash and the file's summary.
        SoundClass->Properties.Volume = ResidentVolume;
        return SaveDiskGuardPackage(Test, TEXT("resident revision saved to disk"),
            Package, SoundClass, Out.PackageFilename);
    }

    inline void DestroyDiskGuardAsset(const FDiskGuardAsset& Asset)
    {
        if (Asset.bBuilt)
        {
            CleanupTestAsset(Asset.ObjectPath);
        }
        if (!Asset.BaselineFilename.IsEmpty())
        {
            IFileManager::Get().Delete(*Asset.BaselineFilename,
                /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
        }
        FString FolderFilename;
        if (!Asset.FolderPath.IsEmpty()
            && FPackageName::TryConvertLongPackageNameToFilename(Asset.FolderPath, FolderFilename, TEXT("")))
        {
            IFileManager::Get().DeleteDirectory(*FolderFilename, /*RequireExists=*/false, /*Tree=*/true);
        }
    }

    inline USoundClass* FindResidentDiskGuardAsset(const FDiskGuardAsset& Asset)
    {
        UPackage* Package = FindPackage(nullptr, *Asset.PackagePath);
        return Package ? FindObject<USoundClass>(Package, *Asset.AssetName) : nullptr;
    }

    // The out-of-band write: put an older revision's bytes back over the live .uasset with no
    // reload, no source-control operation and nothing telling the editor. `git checkout --`,
    // `git reset --hard`, a manual revert and another editor's save all look like this.
    inline bool WriteOutOfBandRevision(const FDiskGuardAsset& Asset)
    {
        return IFileManager::Get().Copy(*Asset.PackageFilename, *Asset.BaselineFilename,
            /*Replace=*/true, /*EvenIfReadOnly=*/true) == COPY_OK;
    }

    inline bool ReadPackageBytes(const FDiskGuardAsset& Asset, TArray<uint8>& Out)
    {
        Out.Reset();
        return FFileHelper::LoadFileToArray(Out, *Asset.PackageFilename);
    }

    // Dirty the resident package with a fresh caller edit - the thing a save is being asked to
    // persist. Returns false when the fixture asset is no longer resident.
    inline bool ApplyResidentEdit(FAutomationTestBase& Test, const FDiskGuardAsset& Asset)
    {
        USoundClass* Resident = FindResidentDiskGuardAsset(Asset);
        if (!Test.TestNotNull(TEXT("fixture asset is resident"), Resident))
        {
            return false;
        }
        Resident->Properties.Volume = EditedVolume;
        Resident->MarkPackageDirty();
        return true;
    }

    inline TSharedPtr<FJsonObject> InvokeAssetSave(const FDiskGuardAsset& Asset,
        bool bForce, bool bOverwriteDiskChanges, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        // ObjectPath form: LoadObject then resolves the RESIDENT object without touching the
        // file, which is the exact condition the defect lives in.
        Params->SetStringField(TEXT("assetPath"), Asset.ObjectPath);
        Params->SetBoolField(TEXT("force"), bForce);
        if (bOverwriteDiskChanges)
        {
            Params->SetBoolField(TEXT("overwriteDiskChanges"), true);
        }
        InvokeHandlerWithCapture(TEXT("asset.save"), Params, Capture);
        return Capture.Result;
    }
}

// ---------------------------------------------------------------------------
// 1. No false positive: an untouched file still saves.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSaveDiskGuardUnchangedFileSavesTest,
    "PinWright.assets.AssetSaveDiskGuard.UnchangedFileStillSaves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAssetSaveDiskGuardUnchangedFileSavesTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAssetSaveDiskGuardFixture;

    FDiskGuardAsset Asset;
    ON_SCOPE_EXIT { DestroyDiskGuardAsset(Asset); };
    if (!BuildDiskGuardAsset(*this, Asset))
    {
        return true;
    }

    TArray<uint8> BytesBefore;
    if (!TestTrue(TEXT("pre-save .uasset readable"), ReadPackageBytes(Asset, BytesBefore)))
    {
        return true;
    }

    if (!ApplyResidentEdit(*this, Asset))
    {
        return true;
    }

    FTestResponseCapture Capture;
    InvokeAssetSave(Asset, /*bForce=*/false, /*bOverwriteDiskChanges=*/false, Capture);

    TestTrue(TEXT("asset.save on an untouched file sent a response"), Capture.bWasCalled);
    // The assertion that kills a fix which simply refuses everything.
    TestNotEqual(TEXT("an untouched file is NOT reported as diverged"),
        Capture.ErrorCode, FString(TEXT("SAVE_DISK_STATE_DIVERGED")));
    TestTrue(TEXT("asset.save on an untouched file is a success"), Capture.bSuccess);

    TArray<uint8> BytesAfter;
    if (!TestTrue(TEXT("post-save .uasset readable"), ReadPackageBytes(Asset, BytesAfter)))
    {
        return true;
    }
    // Judge the file, not the reported flag: the edit must actually have landed.
    TestTrue(TEXT("the save wrote the edit to the .uasset"), BytesAfter != BytesBefore);
    return true;
}

// ---------------------------------------------------------------------------
// 2. No false negative: an out-of-band change is refused, and survives.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSaveDiskGuardRefusesOutOfBandTest,
    "PinWright.assets.AssetSaveDiskGuard.OutOfBandChangeIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAssetSaveDiskGuardRefusesOutOfBandTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAssetSaveDiskGuardFixture;

#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // The refusal this test asserts cannot exist on 5.3, and that is a decision rather than a
    // gap in the fix: UPackage::GetSavedHash and FPackageFileSummary::GetSavedHash both arrive in
    // UE 5.4, so ProbePackageDiskDivergence has no per-save ledger to compare and declines to
    // measure at all (Utils/PackageDiskStateGuard.cpp, the pre-5.4 branch). 5.3's summary carries
    // only PersistentGuid, which does not change per save and so cannot tell an out-of-band write
    // from the revision the editor itself last wrote. Refusing on a fabricated verdict would be
    // worse than saving, so on this engine asset.save still writes and this test measures nothing
    // -- marked rather than silently green, because a skipped assertion and a passed one are
    // otherwise indistinguishable in the suite totals.
    PinWrightTestSkip::SkipAssertions(*this, TEXT("saved-hash-ledger-requires-5.4"),
        TEXT("UPackage::GetSavedHash arrived in UE 5.4; on 5.3 ProbePackageDiskDivergence reports "
             "unprobed and SAVE_DISK_STATE_DIVERGED is unreachable by design."));
    return true;
#else
    FDiskGuardAsset Asset;
    ON_SCOPE_EXIT { DestroyDiskGuardAsset(Asset); };
    if (!BuildDiskGuardAsset(*this, Asset))
    {
        return true;
    }

    if (!TestTrue(TEXT("out-of-band revision written over the live .uasset"),
            WriteOutOfBandRevision(Asset)))
    {
        return true;
    }

    TArray<uint8> OutOfBandBytes;
    if (!TestTrue(TEXT("out-of-band .uasset readable"), ReadPackageBytes(Asset, OutOfBandBytes)))
    {
        return true;
    }

    if (!ApplyResidentEdit(*this, Asset))
    {
        return true;
    }

    // force:true on purpose. force means "bypass the 0.5s save throttle" and must NOT have
    // quietly acquired "and discard whatever is on disk"; a refusal here is that contract.
    FTestResponseCapture Capture;
    InvokeAssetSave(Asset, /*bForce=*/true, /*bOverwriteDiskChanges=*/false, Capture);

    TestTrue(TEXT("asset.save over an out-of-band change sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("asset.save over an out-of-band change is not a success"), Capture.bSuccess);
    TestEqual(TEXT("asset.save over an out-of-band change refuses with SAVE_DISK_STATE_DIVERGED"),
        Capture.ErrorCode, FString(TEXT("SAVE_DISK_STATE_DIVERGED")));
    TestFalse(TEXT("the refusal message is not empty"), Capture.Message.IsEmpty());

    if (Capture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* DiskState = nullptr;
        if (TestTrue(TEXT("the refusal carries a diskState block"),
                Capture.Result->TryGetObjectField(TEXT("diskState"), DiskState))
            && DiskState && DiskState->IsValid())
        {
            bool bProbed = false;
            (*DiskState)->TryGetBoolField(TEXT("probed"), bProbed);
            TestTrue(TEXT("diskState reports a measurement was made"), bProbed);

            bool bDiverged = false;
            (*DiskState)->TryGetBoolField(TEXT("diverged"), bDiverged);
            TestTrue(TEXT("diskState reports diverged:true"), bDiverged);

            // The measured disk state and the in-memory state are named separately, and they
            // must actually differ - a block that reported the same value on both sides would
            // be a fabricated verdict.
            FString DiskHash, LoadedHash;
            const bool bHasDisk =
                (*DiskState)->TryGetStringField(TEXT("diskSavedHash"), DiskHash);
            const bool bHasLoaded =
                (*DiskState)->TryGetStringField(TEXT("loadedSavedHash"), LoadedHash);
            TestTrue(TEXT("diskState publishes the measured on-disk saved hash"), bHasDisk);
            TestTrue(TEXT("diskState publishes the resident package's saved hash"), bHasLoaded);
            TestTrue(TEXT("the two hashes disagree, which is what diverged means"),
                bHasDisk && bHasLoaded && DiskHash != LoadedHash);

            FString File;
            (*DiskState)->TryGetStringField(TEXT("file"), File);
            TestFalse(TEXT("the refusal names the file"), File.IsEmpty());
        }
    }
    else
    {
        AddError(TEXT("asset.save over an out-of-band change returned no payload"));
    }

    // The point of the whole ticket: the out-of-band revision is still on disk.
    TArray<uint8> BytesAfter;
    if (!TestTrue(TEXT("post-refusal .uasset readable"), ReadPackageBytes(Asset, BytesAfter)))
    {
        return true;
    }
    TestTrue(TEXT("the out-of-band bytes were NOT overwritten"), BytesAfter == OutOfBandBytes);
    return true;
#endif
}

// ---------------------------------------------------------------------------
// 3. The override exists, is separate from force, and does overwrite.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSaveDiskGuardOverrideOverwritesTest,
    "PinWright.assets.AssetSaveDiskGuard.OverrideOverwritesDeliberately",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAssetSaveDiskGuardOverrideOverwritesTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAssetSaveDiskGuardFixture;

    FDiskGuardAsset Asset;
    ON_SCOPE_EXIT { DestroyDiskGuardAsset(Asset); };
    if (!BuildDiskGuardAsset(*this, Asset))
    {
        return true;
    }

    if (!TestTrue(TEXT("out-of-band revision written over the live .uasset"),
            WriteOutOfBandRevision(Asset)))
    {
        return true;
    }

    TArray<uint8> OutOfBandBytes;
    if (!TestTrue(TEXT("out-of-band .uasset readable"), ReadPackageBytes(Asset, OutOfBandBytes)))
    {
        return true;
    }

    if (!ApplyResidentEdit(*this, Asset))
    {
        return true;
    }

    FTestResponseCapture Capture;
    InvokeAssetSave(Asset, /*bForce=*/true, /*bOverwriteDiskChanges=*/true, Capture);

    TestTrue(TEXT("overridden asset.save sent a response"), Capture.bWasCalled);
    TestNotEqual(TEXT("the override is not itself refused"),
        Capture.ErrorCode, FString(TEXT("SAVE_DISK_STATE_DIVERGED")));
    TestTrue(TEXT("overridden asset.save is a success"), Capture.bSuccess);

    TArray<uint8> BytesAfter;
    if (!TestTrue(TEXT("post-override .uasset readable"), ReadPackageBytes(Asset, BytesAfter)))
    {
        return true;
    }
    TestTrue(TEXT("the override deliberately replaced the on-disk revision"),
        BytesAfter != OutOfBandBytes);
    return true;
}
