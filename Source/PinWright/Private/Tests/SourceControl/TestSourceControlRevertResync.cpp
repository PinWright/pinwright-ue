// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for board B-source-control-revert-no-package-reload:
// source_control.revert reverted the .uasset on disk and left the LOADED UPackage holding
// the pre-revert values, so (a) every readback afterwards reported the un-reverted state
// while source_control.status reported the file clean, and (b) the next save of that
// package wrote the stale in-memory state back out, silently undoing the revert.
//
// The two tests below are the two halves of that:
//   1. ...ReloadsInMemoryPackage      - after the revert, memory must match disk.
//   2. ...SurvivesFollowUpSave        - after the revert, a save must not re-persist the
//                                      pre-revert bytes.
//
// Both drive PinWrightSourceControlResync::ApplyAndResyncPackages directly rather than the
// source_control.revert RPC, for one reason: the handler is gated on an ENABLED provider
// (RequireEnabledProvider), which an automation host does not have, so a handler-level test
// would only ever exercise the SOURCE_CONTROL_DISABLED branch and could not fail on this
// defect at all. What the provider contributes to a revert is the on-disk half - restore the
// committed bytes over the working copy - and the fixture below performs exactly that as the
// applied operation. The defect under test was never in the provider; it was that nothing
// resynchronized the resident package with whatever the provider had written.
//
// Pre-fix these fail: the shared resync symbol does not exist (the handler called
// Provider->Execute(FRevert) and returned), and with the raw operation in its place the
// resident USoundClass still reads the modified volume and a follow-up save writes it back.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "Handlers/SourceControl/SourceControlPackageResync.h"

#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "Sound/SoundClass.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

// Named namespace, not anonymous: with bUseUnity=true an anonymous-namespace fixture becomes
// visible at global scope once Unity merges this TU with its neighbours (see TestAssetTeardown.h).
namespace PinWrightRevertResyncFixture
{
    // The value the "committed" .uasset carries, and the local edit a revert must discard.
    // Far enough apart that no tolerance question arises.
    constexpr float BaselineVolume = 0.25f;
    constexpr float ModifiedVolume = 0.875f;

    struct FRevertResyncAsset
    {
        FString FolderPath;
        FString PackagePath;
        FString AssetName;
        FString ObjectPath;
        FString PackageFilename;
        FString BaselineFilename;
        bool bBuilt = false;
    };

    // Produces a saved-on-disk USoundClass in the state source_control.revert is asked to act
    // on: the .uasset on disk carries ModifiedVolume, a byte copy of the committed baseline
    // (BaselineVolume) is parked aside, and the resident UObject carries ModifiedVolume too.
    inline bool BuildRevertResyncAsset(FAutomationTestBase& Test, FRevertResyncAsset& Out)
    {
        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Out.FolderPath = FString::Printf(TEXT("/Game/__PW_SCRevertTests/%s"), *Suffix);
        Out.AssetName = FString::Printf(TEXT("PW_RevertResync_%s"), *Suffix);
        Out.PackagePath = FString::Printf(TEXT("%s/%s"), *Out.FolderPath, *Out.AssetName);
        Out.ObjectPath = FString::Printf(TEXT("%s.%s"), *Out.PackagePath, *Out.AssetName);

        if (!Test.TestTrue(TEXT("fixture package path resolves to a filename"),
                FPackageName::TryConvertLongPackageNameToFilename(
                    Out.PackagePath, Out.PackageFilename, FPackageName::GetAssetPackageExtension())))
        {
            return false;
        }

        Out.BaselineFilename = FPaths::ProjectIntermediateDir()
            / TEXT("PinWrightRevertResync") / (Out.AssetName + TEXT(".baseline"));
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

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;

        SoundClass->Properties.Volume = BaselineVolume;
        if (!Test.TestTrue(TEXT("baseline package saved to disk"),
                UPackage::SavePackage(Package, SoundClass, *Out.PackageFilename, SaveArgs)))
        {
            return false;
        }

        if (!Test.TestTrue(TEXT("baseline bytes copied aside"),
                IFileManager::Get().Copy(*Out.BaselineFilename, *Out.PackageFilename,
                    /*Replace=*/true, /*EvenIfReadOnly=*/true) == COPY_OK))
        {
            return false;
        }

        // The local change the revert is meant to discard - persisted, so the working tree is
        // genuinely dirty rather than merely the memory copy.
        SoundClass->Properties.Volume = ModifiedVolume;
        return Test.TestTrue(TEXT("modified package saved to disk"),
            UPackage::SavePackage(Package, SoundClass, *Out.PackageFilename, SaveArgs));
    }

    inline void DestroyRevertResyncAsset(const FRevertResyncAsset& Asset)
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

    // The on-disk half of a provider revert: put the committed bytes back over the working
    // copy. This is what FRevert does to the file, and all this test needs from a provider.
    inline bool RestoreRevertResyncBaseline(const FRevertResyncAsset& Asset)
    {
        return IFileManager::Get().Copy(*Asset.PackageFilename, *Asset.BaselineFilename,
            /*Replace=*/true, /*EvenIfReadOnly=*/true) == COPY_OK;
    }

    // Re-resolves the asset through the package name. Mandatory after a reload: the pre-reload
    // UObject* is dead, and reading through it is exactly the mistake this fix is about.
    inline USoundClass* FindResidentRevertResyncAsset(const FRevertResyncAsset& Asset)
    {
        UPackage* Package = FindPackage(nullptr, *Asset.PackagePath);
        return Package ? FindObject<USoundClass>(Package, *Asset.AssetName) : nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceControlRevertResyncReloadsPackageTest,
    "PinWright.source_control.RevertResyncReloadsInMemoryPackage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSourceControlRevertResyncReloadsPackageTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRevertResyncFixture;

    FRevertResyncAsset Asset;
    ON_SCOPE_EXIT { DestroyRevertResyncAsset(Asset); };
    if (!BuildRevertResyncAsset(*this, Asset))
    {
        return true;
    }

    const USoundClass* BeforeRevert = FindResidentRevertResyncAsset(Asset);
    if (!TestNotNull(TEXT("fixture asset is resident before the revert"), BeforeRevert))
    {
        return true;
    }
    TestEqual(TEXT("resident volume carries the local change before the revert"),
        BeforeRevert->Properties.Volume, ModifiedVolume);

    PinWrightSourceControlResync::FPackageResyncReport Report;
    const bool bApplied = PinWrightSourceControlResync::ApplyAndResyncPackages(
        {Asset.PackageFilename},
        [&Asset](const TArray<FString>&) { return RestoreRevertResyncBaseline(Asset); },
        Report);

    TestTrue(TEXT("the on-disk revert ran"), bApplied);
    TestEqual(TEXT("nothing was blocked from reload"), Report.BlockedPackages.Num(), 0);
    TestEqual(TEXT("the target was measured as resident"), Report.LoadedCount, 1);
    TestEqual(TEXT("the resident package was measured as reloaded"), Report.ReloadedCount, 1);
    TestEqual(TEXT("no package was left holding pre-revert state"), Report.StaleCount, 0);

    // The defect: without the reload this still reads ModifiedVolume while the file on disk
    // holds BaselineVolume - status says clean, the readback says modified.
    const USoundClass* AfterRevert = FindResidentRevertResyncAsset(Asset);
    if (!TestNotNull(TEXT("fixture asset is resident after the revert"), AfterRevert))
    {
        return true;
    }
    TestEqual(TEXT("in-memory volume matches the reverted bytes on disk"),
        AfterRevert->Properties.Volume, BaselineVolume);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceControlRevertResyncSurvivesResaveTest,
    "PinWright.source_control.RevertResyncSurvivesFollowUpSave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSourceControlRevertResyncSurvivesResaveTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRevertResyncFixture;

    FRevertResyncAsset Asset;
    ON_SCOPE_EXIT { DestroyRevertResyncAsset(Asset); };
    if (!BuildRevertResyncAsset(*this, Asset))
    {
        return true;
    }

    PinWrightSourceControlResync::FPackageResyncReport Report;
    PinWrightSourceControlResync::ApplyAndResyncPackages(
        {Asset.PackageFilename},
        [&Asset](const TArray<FString>&) { return RestoreRevertResyncBaseline(Asset); },
        Report);

    // Whatever asset.save / editor.save_all / an autosave / the shutdown prompt would do next.
    // Re-resolved, because the package the resync reloaded is a different object.
    UPackage* SavedPackage = FindPackage(nullptr, *Asset.PackagePath);
    USoundClass* Reverted = FindResidentRevertResyncAsset(Asset);
    if (!TestNotNull(TEXT("reverted package is resident"), SavedPackage)
        || !TestNotNull(TEXT("reverted asset is resident"), Reverted))
    {
        return true;
    }

    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    if (!TestTrue(TEXT("package re-saved after the revert"),
            UPackage::SavePackage(SavedPackage, Reverted, *Asset.PackageFilename, SaveArgs)))
    {
        return true;
    }

    // Judge the FILE, not memory: force the bytes back through a load. Pre-fix the save above
    // re-persists the pre-revert volume and this reads ModifiedVolume - the silent un-revert.
    TArray<UPackage*> ToReload;
    ToReload.Add(SavedPackage);
    FText ReloadError;
    if (!TestTrue(TEXT("saved package re-read from disk for verification"),
            UPackageTools::ReloadPackages(ToReload, ReloadError,
                EReloadPackagesInteractionMode::AssumePositive)))
    {
        return true;
    }

    const USoundClass* FromDisk = FindResidentRevertResyncAsset(Asset);
    if (!TestNotNull(TEXT("asset re-read from disk"), FromDisk))
    {
        return true;
    }
    TestEqual(TEXT("the .uasset on disk still holds the reverted value after a follow-up save"),
        FromDisk->Properties.Volume, BaselineVolume);
    return true;
}
