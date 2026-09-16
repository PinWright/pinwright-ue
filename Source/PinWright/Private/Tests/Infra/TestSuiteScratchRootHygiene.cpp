// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for board B-suite-run-dirties-host-repo: a full automation run left the
// HOST project's git tree dirty. Two independent mechanisms, one test each here plus the
// suite-end gate:
//
//   1. editor.save_all takes no arguments and has no dry-run or package filter, so the suite's
//      own shape test for it flushed every dirty package in the editor - including the host's
//      open startup map. FScopedForeignDirtyPackageSuspension scopes that call's dirty set to
//      the fixture scratch root and puts the flags back afterwards.
//   2. /Game/PinWrightTests is the suite-wide fixture scratch root and had no suite-level sweep:
//      per-asset teardown removes files one at a time, never directories, and gives up silently
//      when a linker is still attached. SweepScratchRoot is that missing sweep.
//
// The third test is the gate: it runs the sweep at the end of the run and fails if anything
// survived. It is also the ONLY caller allowed to sweep the whole root -- the sweep detaches and
// deletes, so run mid-suite it would take fixtures their owning tests are still using, which is
// why the second test passes SweepScratchRoot's SubDirectory argument. Its id starts with "zz_"
// ON PURPOSE. The automation controller sorts the
// batch by display name before inserting it into the report tree
// (AutomationControllerManager.cpp:1047) and executes leaves in that order, so a "z" first
// segment is what puts this test after every other PinWright.* test and therefore after every
// fixture that could still write into the scratch root. Rename it into an alphabetically earlier
// namespace and the gate keeps passing while it stops covering the tests that now run after it.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/AutomationSuiteMaintenance.h"
#include "Tests/TestAssetTeardown.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#if WITH_AUTOMATION_TESTS

// Named namespace, not anonymous: with bUseUnity=true an anonymous-namespace helper becomes
// visible at global scope once Unity merges this TU with its neighbours (see TestAssetTeardown.h).
namespace PinWrightScratchRootHygieneFixture
{
    // A never-saved /Game package holding one asset, which is the minimum
    // FEditorFileUtils::GetDirtyContentPackages enumerates: a root package, not transient, not
    // compiled-in, dirty, and under a mounted writable root.
    inline UPackage* CreateDirtyProbePackage(const FString& PackageName, const FString& AssetName)
    {
        UPackage* Package = CreatePackage(*PackageName);
        if (Package == nullptr)
        {
            return nullptr;
        }
        NewObject<UCurveFloat>(Package, FName(*AssetName), RF_Public | RF_Standalone);
        Package->SetDirtyFlag(true);
        return Package;
    }

    inline void DeDirtyProbePackage(UPackage* Package)
    {
        if (Package != nullptr)
        {
            // A probe left dirty is exactly the leak this file exists to stop.
            Package->SetDirtyFlag(false);
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSuiteMaintenanceForeignDirtySuspensionTest,
    "PinWright.infra.contract.SuiteMaintenance.ForeignDirtyPackagesSuspendedAndRestored",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuiteMaintenanceForeignDirtySuspensionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ForeignAssetName = FString::Printf(TEXT("PW_ForeignDirty_%s"), *Suffix);
    const FString ForeignPackageName = FString::Printf(TEXT("/Game/%s"), *ForeignAssetName);
    const FString ForeignObjectPath =
        FString::Printf(TEXT("%s.%s"), *ForeignPackageName, *ForeignAssetName);

    const FString ScratchAssetName = FString::Printf(TEXT("PW_ScratchDirty_%s"), *Suffix);
    const FString ScratchPackageName = FString::Printf(TEXT("%s/%s"),
        PinWrightSuiteMaintenance::ScratchRootPackagePath(), *ScratchAssetName);
    const FString ScratchObjectPath =
        FString::Printf(TEXT("%s.%s"), *ScratchPackageName, *ScratchAssetName);

    UPackage* Foreign = PinWrightScratchRootHygieneFixture::CreateDirtyProbePackage(
        ForeignPackageName, ForeignAssetName);
    UPackage* Scratch = PinWrightScratchRootHygieneFixture::CreateDirtyProbePackage(
        ScratchPackageName, ScratchAssetName);

    ON_SCOPE_EXIT
    {
        // De-dirty both BEFORE the first discard: DiscardCreatedAssetByObjectPath collects
        // garbage on the way out, and the pointers must not be read across that.
        PinWrightScratchRootHygieneFixture::DeDirtyProbePackage(Foreign);
        PinWrightScratchRootHygieneFixture::DeDirtyProbePackage(Scratch);
        // Neither probe was ever saved, so teardown is in-memory only and leaves no .uasset.
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ForeignObjectPath);
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ScratchObjectPath);
    };

    if (!TestTrue(TEXT("both dirty probe packages were created"),
            Foreign != nullptr && Scratch != nullptr))
    {
        return false;
    }
    if (!TestTrue(TEXT("both probe packages start dirty"), Foreign->IsDirty() && Scratch->IsDirty()))
    {
        return false;
    }

    {
        PinWrightSuiteMaintenance::FScopedForeignDirtyPackageSuspension Suspension;

        TestFalse(
            TEXT("a dirty package outside the scratch root is suspended for the scope, so an "
                 "editor-wide save-all cannot write it"),
            Foreign->IsDirty());
        TestTrue(
            TEXT("a dirty fixture package inside the scratch root stays dirty, so save-all still "
                 "has a real dirty set to report on"),
            Scratch->IsDirty());
        TestTrue(TEXT("the suspension names the package it suspended"),
            Suspension.SuspendedPackageNames().Contains(ForeignPackageName));
        TestFalse(TEXT("the suspension does not name the fixture package"),
            Suspension.SuspendedPackageNames().Contains(ScratchPackageName));
    }

    TestTrue(TEXT("the foreign dirty flag is restored on scope exit, so an unsaved host edit "
                  "is neither written nor discarded"),
        Foreign->IsDirty());
    TestTrue(TEXT("the fixture dirty flag is untouched by the guard"), Scratch->IsDirty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSuiteMaintenanceScratchSweepRemovesSavedFixtureTest,
    "PinWright.infra.contract.SuiteMaintenance.ScratchRootSweepRemovesSavedFixtureFile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuiteMaintenanceScratchSweepRemovesSavedFixtureTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetName = FString::Printf(TEXT("PW_SweepProbe_%s"), *Suffix);
    // Own subfolder, and the sweep below is scoped to it. This test runs mid-suite -- PinWright.
    // infra.* is well after PinWright.editor.* -- and by then other fixtures are sitting on disk
    // under the scratch root, so a whole-root sweep here would detach and delete assets their
    // owning tests are not finished with. Only the suite-end gate sweeps the whole root.
    const FString SubDirectory = FString::Printf(TEXT("PW_Sweep_%s"), *Suffix);
    const FString PackageName = FString::Printf(TEXT("%s/%s/%s"),
        PinWrightSuiteMaintenance::ScratchRootPackagePath(), *SubDirectory, *AssetName);

    FString Filename;
    if (!TestTrue(TEXT("the scratch probe package path resolves to a filename"),
            FPackageName::TryConvertLongPackageNameToFilename(
                PackageName, Filename, FPackageName::GetAssetPackageExtension())))
    {
        return false;
    }
    // The sweep reports absolute paths (it walks an absolute root); normalise so the membership
    // check below compares like with like rather than a relative spelling of the same file.
    Filename = FPaths::ConvertRelativePathToFull(Filename);

    UPackage* Package = CreatePackage(*PackageName);
    if (!TestTrue(TEXT("the scratch probe package was created"), Package != nullptr))
    {
        return false;
    }
    UCurveFloat* Probe = NewObject<UCurveFloat>(
        Package, FName(*AssetName), RF_Public | RF_Standalone);
    FAssetRegistryModule::AssetCreated(Probe);

    ON_SCOPE_EXIT
    {
        // If the sweep declined the file, this is what stops the probe itself from becoming the
        // residue the test is about.
        CleanupTestAsset(PackageName);
    };

    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    if (!TestTrue(TEXT("the scratch probe fixture saved to disk"),
            UPackage::SavePackage(Package, Probe, *Filename, SaveArgs)))
    {
        return false;
    }
    if (!TestTrue(TEXT("the scratch probe .uasset is on disk before the sweep"),
            IFileManager::Get().FileSize(*Filename) >= 0))
    {
        return false;
    }
    TestTrue(TEXT("the scratch root exists on disk before the sweep"),
        PinWrightSuiteMaintenance::ScratchRootExistsOnDisk());

    const FString ProbeDir = FPaths::GetPath(Filename);

    TArray<FString> Remaining;
    const int32 RemovedCount =
        PinWrightSuiteMaintenance::SweepScratchRoot(&Remaining, SubDirectory);

    // Exactly one: the scoped sweep saw the probe subtree and nothing else, which is the property
    // that keeps this test from disturbing fixtures elsewhere under the root.
    TestEqual(TEXT("the scoped sweep removed the probe file and only it"), RemovedCount, 1);
    TestTrue(TEXT("the scratch probe .uasset is gone from disk after the sweep"),
        IFileManager::Get().FileSize(*Filename) < 0);
    TestFalse(TEXT("the sweep does not report the probe as a file it could not remove"),
        Remaining.Contains(Filename));
    TestFalse(TEXT("the emptied probe directory is removed too, not left standing as the empty "
                   "folder tree per-asset teardown leaves behind"),
        IFileManager::Get().DirectoryExists(*ProbeDir));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSuiteEndScratchRootIsEmptyTest,
    "PinWright.zz_suite_end.ScratchRootIsEmptyOnDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuiteEndScratchRootIsEmptyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString RootDir = PinWrightSuiteMaintenance::ScratchRootContentDir();
    if (!PinWrightSuiteMaintenance::ScratchRootExistsOnDisk())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("scratch-root-never-created"),
            FString::Printf(
                TEXT("'%s' does not exist, so this run wrote no fixture file there and the sweep "
                     "measured nothing."),
                *RootDir));
        return true;
    }

    TArray<FString> Remaining;
    const int32 RemovedCount = PinWrightSuiteMaintenance::SweepScratchRoot(&Remaining);
    AddInfo(FString::Printf(
        TEXT("Suite-end scratch sweep removed %d fixture file(s) from '%s'."),
        RemovedCount, *RootDir));

    FString RemainingList;
    for (const FString& File : Remaining)
    {
        RemainingList += FString::Printf(TEXT("\n  %s"), *File);
    }
    TestEqual(
        *FString::Printf(
            TEXT("no fixture file survives the suite-end scratch sweep; a survivor means the test "
                 "that wrote it still holds its linker or its package open:%s"),
            *RemainingList),
        Remaining.Num(), 0);
    TestFalse(TEXT("the scratch root directory does not survive the suite"),
        PinWrightSuiteMaintenance::ScratchRootExistsOnDisk());
    return true;
}

#endif
