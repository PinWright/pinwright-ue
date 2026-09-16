// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace CleanupTestAssetTeardownTestHelpers
{
    bool ExtractHelperBody(
        const FString& Source, const FString& SignatureText, FString& OutBody)
    {
        const FString Neutralized = NeutralizeSourceText(Source);
        const int32 Signature = Neutralized.Find(SignatureText);
        const int32 OpenBrace = Signature == INDEX_NONE
            ? INDEX_NONE
            : Neutralized.Find(TEXT("{"), ESearchCase::CaseSensitive,
                ESearchDir::FromStart, Signature);
        if (OpenBrace == INDEX_NONE)
        {
            return false;
        }

        int32 Depth = 0;
        for (int32 Index = OpenBrace; Index < Neutralized.Len(); ++Index)
        {
            if (Neutralized[Index] == TEXT('{'))
            {
                ++Depth;
            }
            else if (Neutralized[Index] == TEXT('}') && --Depth == 0)
            {
                OutBody = Neutralized.Mid(OpenBrace + 1, Index - OpenBrace - 1);
                return true;
            }
        }
        return false;
    }

    TStrongObjectPtr<UCurveFloat> CreateCurveFixture(
        FAutomationTestBase& Test,
        const TCHAR* NamePrefix,
        FString& OutPackagePath,
        FString& OutObjectPath,
        FString& OutFilename)
    {
        const FString AssetName = FString::Printf(
            TEXT("%s_%s"), NamePrefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        OutPackagePath = TEXT("/Game/__PW_GatewayTests/") + AssetName;
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *OutPackagePath, *AssetName);
        if (!Test.TestTrue(TEXT("fixture package path resolves to a .uasset filename"),
                FPackageName::TryConvertLongPackageNameToFilename(
                    OutPackagePath, OutFilename, FPackageName::GetAssetPackageExtension())))
        {
            return TStrongObjectPtr<UCurveFloat>(nullptr);
        }

        UPackage* Package = CreatePackage(*OutPackagePath);
        if (!Test.TestNotNull(TEXT("fixture package created"), Package))
        {
            return TStrongObjectPtr<UCurveFloat>(nullptr);
        }

        TStrongObjectPtr<UCurveFloat> Asset(NewObject<UCurveFloat>(
            Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional));
        if (!Test.TestNotNull(TEXT("fixture curve asset created"), Asset.Get()))
        {
            return TStrongObjectPtr<UCurveFloat>(nullptr);
        }
        Asset->FloatCurve.AddKey(0.0f, 1.0f);
        FAssetRegistryModule::AssetCreated(Asset.Get());
        return Asset;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCleanupTestAssetHasNoForceDeleteTest,
    "PinWright.infra.contract.TestAssetTeardown.CleanupTestAssetHasNoForceDelete",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCleanupTestAssetHasNoForceDeleteTest::RunTest(const FString& Parameters)
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    if (!TestTrue(TEXT("PinWright plugin resolved for CleanupTestAsset source contract"),
            Plugin.IsValid()))
    {
        return false;
    }

    FString TestUtilsSource;
    const FString TestUtilsHeader = Plugin->GetBaseDir()
        / TEXT("Source/PinWright/Private/Tests/TestUtils.h");
    if (!TestTrue(TEXT("Read TestUtils.h for the CleanupTestAsset contract"),
            FFileHelper::LoadFileToString(TestUtilsSource, *TestUtilsHeader)))
    {
        return false;
    }

    FString HelperBody;
    if (!TestTrue(TEXT("Located CleanupTestAsset implementation"),
            CleanupTestAssetTeardownTestHelpers::ExtractHelperBody(
                TestUtilsSource, TEXT("inline void CleanupTestAsset(const FString& PackagePath)"),
                HelperBody)))
    {
        return false;
    }

    TestFalse(TEXT("CleanupTestAsset cannot call ObjectTools::ForceDeleteObjects"),
        HelperBody.Contains(TEXT("ForceDeleteObjects")));
    TestFalse(TEXT("CleanupTestAsset cannot call DeleteLoadedAsset"),
        HelperBody.Contains(TEXT("DeleteLoadedAsset")));
    TestFalse(TEXT("CleanupTestAsset cannot call DeleteAsset"),
        HelperBody.Contains(TEXT("DeleteAsset(")));
    TestFalse(TEXT("CleanupTestAsset cannot open a modal/dialog path"),
        HelperBody.Contains(TEXT("MessageDialog")) || HelperBody.Contains(TEXT("Modal"))
            || HelperBody.Contains(TEXT("OpenDialog")));
    TestFalse(TEXT("CleanupTestAsset cannot call CollectGarbage"),
        HelperBody.Contains(TEXT("CollectGarbage(")));
    TestFalse(TEXT("CleanupTestAsset cannot call FlushAsyncLoading"),
        HelperBody.Contains(TEXT("FlushAsyncLoading(")));
    TestFalse(TEXT("CleanupTestAsset cannot call package unload semantics"),
        HelperBody.Contains(TEXT("MarkAsUnloaded(")));
    TestFalse(TEXT("CleanupTestAsset has no runtime diagnostic logging"),
        HelperBody.Contains(TEXT("PW_TEARDOWN_DIAG")));
    TestTrue(TEXT("CleanupTestAsset delegates in-memory discard to the no-GC core"),
        HelperBody.Contains(TEXT("DiscardLoadedAssetNoGc")));
    TestTrue(TEXT("CleanupTestAsset stops when the no-GC core declines an in-memory asset"),
        HelperBody.Contains(TEXT("if (InMemory && !Package)")));
    const int32 NoGcCall = HelperBody.Find(TEXT("DiscardLoadedAssetNoGc"));
    const int32 NullGuard = HelperBody.Find(TEXT("if (InMemory && !Package)"));
    const int32 FilenameProbe = HelperBody.Find(TEXT("TryConvertLongPackageNameToFilename"));
    const int32 FileSizeProbe = HelperBody.Find(TEXT("IFileManager::Get().FileSize"));
    const int32 OriginalPackage = HelperBody.Find(TEXT("UPackage* OriginalPackage"));
    const int32 ActiveWorldGuard = HelperBody.Find(
        TEXT("OriginalPackage == EditorWorld->GetOutermost()"));
    const int32 ResetLoadersCall = HelperBody.Find(TEXT("ResetLoaders(OriginalPackage)"));
    const int32 FileDelete = HelperBody.Find(TEXT("IFileManager::Get().Delete"));
    TestTrue(TEXT("CleanupTestAsset resolves the filename before discarding the in-memory asset"),
        FilenameProbe != INDEX_NONE && FileSizeProbe > FilenameProbe && NoGcCall > FileSizeProbe);
    TestTrue(TEXT("CleanupTestAsset resets the source package linker before discard"),
        OriginalPackage != INDEX_NONE && ActiveWorldGuard > OriginalPackage
            && ResetLoadersCall > FileSizeProbe && ResetLoadersCall < NoGcCall);
    TestTrue(TEXT("CleanupTestAsset performs the quiet delete after its pre-discard release"),
        FileDelete > NoGcCall);
    const int32 MaxDeleteAttempts = HelperBody.Find(
        TEXT("constexpr int32 MaxDeleteAttempts = 40"));
    const int32 ImmediateDelete = HelperBody.Find(TEXT("bool bDeleted = TryDelete()"));
    const int32 BoundedRetry = HelperBody.Find(
        TEXT("for (int32 Attempt = 0; Attempt < MaxDeleteAttempts; ++Attempt)"));
    const int32 SleepRetry = HelperBody.Find(TEXT("FPlatformProcess::Sleep(0.05f)"));
    TestTrue(TEXT("CleanupTestAsset keeps one immediate delete and a bounded 40-attempt retry"),
        MaxDeleteAttempts != INDEX_NONE && ImmediateDelete > MaxDeleteAttempts
            && BoundedRetry > ImmediateDelete && SleepRetry > BoundedRetry);
    // Keep the source contract explicit: a quiet delete failure must be checked before cleanup
    // can publish a registry notification.
    const int32 DeleteResultIdentifier = HelperBody.Find(TEXT("bDeleted"));
    const int32 DeleteResultCheck = HelperBody.Find(TEXT("if (!bDeleted)"));
    const int32 DeleteFailureReturn = HelperBody.Find(TEXT("return;"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, DeleteResultCheck);
    const int32 PackageDeleted = HelperBody.Find(TEXT("PackageDeleted("));
    const int32 ScanModifiedAssetFiles = HelperBody.Find(TEXT("ScanModifiedAssetFiles("));
    TestTrue(TEXT("CleanupTestAsset checks Delete failure before registry notification"),
        DeleteResultIdentifier != INDEX_NONE && DeleteResultCheck > DeleteResultIdentifier
            && DeleteFailureReturn > DeleteResultCheck
            && PackageDeleted != INDEX_NONE && DeleteFailureReturn < PackageDeleted
            && ScanModifiedAssetFiles != INDEX_NONE
            && DeleteFailureReturn < ScanModifiedAssetFiles);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCleanupRemovesSavedFixtureFromDiskAndRegistryTest,
    "PinWright.infra.contract.TestAssetTeardown.CleanupRemovesSavedFixtureFromDiskAndRegistry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCleanupRemovesSavedFixtureFromDiskAndRegistryTest::RunTest(const FString& Parameters)
{
    using namespace CleanupTestAssetTeardownTestHelpers;

    FString PackagePath;
    FString ObjectPath;
    FString Filename;
    TStrongObjectPtr<UCurveFloat> Asset = CreateCurveFixture(
        *this, TEXT("Curve_CleanupSaved"), PackagePath, ObjectPath, Filename);
    if (!Asset.IsValid())
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TestTrue(TEXT("saved fixture is absent before its first save"),
        IFileManager::Get().FileSize(*Filename) < 0);
    Asset->MarkPackageDirty();
    TestTrue(TEXT("saved fixture writes through SaveLoadedAsset"),
        UEditorAssetLibrary::SaveLoadedAsset(Asset.Get(), /*bOnlyIfIsDirty=*/false));
    TestTrue(TEXT("saved fixture .uasset exists before cleanup"),
        IFileManager::Get().FileSize(*Filename) > 0);
    TestTrue(TEXT("saved fixture is registered before cleanup"),
        UEditorAssetLibrary::DoesAssetExist(PackagePath));

    CleanupTestAsset(PackagePath);

    TestTrue(TEXT("cleanup removes the saved fixture file"),
        IFileManager::Get().FileSize(*Filename) < 0);
    TestFalse(TEXT("cleanup removes the saved fixture registry entry"),
        UEditorAssetLibrary::DoesAssetExist(PackagePath));
    TestNull(TEXT("cleanup removes the saved fixture from its original object path"),
        FindObject<UObject>(nullptr, *ObjectPath));
    TestNull(TEXT("cleanup detaches the saved fixture package from its original path"),
        FindPackage(nullptr, *PackagePath));

    {
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(PackagePath);
        };

        const FString ReplacementAssetName =
            FPackageName::GetLongPackageAssetName(PackagePath);
        const FString ReplacementObjectPath = FString::Printf(
            TEXT("%s.%s"), *PackagePath, *ReplacementAssetName);
        UPackage* ReplacementPackage = CreatePackage(*PackagePath);
        if (!TestNotNull(TEXT("replacement package recreated at the same package path"),
                ReplacementPackage))
        {
            return false;
        }
        TStrongObjectPtr<UCurveFloat> ReplacementAsset(NewObject<UCurveFloat>(
            ReplacementPackage, FName(*ReplacementAssetName),
            RF_Public | RF_Standalone | RF_Transactional));
        if (!TestNotNull(TEXT("replacement curve asset recreated"), ReplacementAsset.Get()))
        {
            return false;
        }
        ReplacementAsset->FloatCurve.AddKey(0.0f, 2.0f);
        FAssetRegistryModule::AssetCreated(ReplacementAsset.Get());
        ReplacementAsset->MarkPackageDirty();
        const bool bReplacementSaved = UEditorAssetLibrary::SaveLoadedAsset(
            ReplacementAsset.Get(), /*bOnlyIfIsDirty=*/false);
        TestTrue(TEXT("replacement fixture saves at the same package path"),
            bReplacementSaved);
        TestNotNull(TEXT("replacement asset is findable at the same object path"),
            FindObject<UObject>(nullptr, *ReplacementObjectPath));
        TestTrue(TEXT("replacement fixture .uasset exists before cleanup"),
            IFileManager::Get().FileSize(*Filename) > 0);

        CleanupTestAsset(PackagePath);
        TestTrue(TEXT("cleanup removes the replacement fixture file"),
            IFileManager::Get().FileSize(*Filename) < 0);
        TestFalse(TEXT("cleanup removes the replacement fixture registry entry"),
            UEditorAssetLibrary::DoesAssetExist(PackagePath));
        TestNull(TEXT("cleanup removes the replacement fixture object"),
            FindObject<UObject>(nullptr, *ReplacementObjectPath));
        TestNull(TEXT("cleanup detaches the replacement package"),
            FindPackage(nullptr, *PackagePath));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCleanupDiscardsNeverSavedFixtureTest,
    "PinWright.infra.contract.TestAssetTeardown.CleanupDiscardsNeverSavedFixture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCleanupDiscardsNeverSavedFixtureTest::RunTest(const FString& Parameters)
{
    using namespace CleanupTestAssetTeardownTestHelpers;

    FString PackagePath;
    FString ObjectPath;
    FString Filename;
    TStrongObjectPtr<UCurveFloat> Asset = CreateCurveFixture(
        *this, TEXT("Curve_CleanupUnsaved"), PackagePath, ObjectPath, Filename);
    if (!Asset.IsValid())
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TestTrue(TEXT("never-saved fixture has no .uasset before cleanup"),
        IFileManager::Get().FileSize(*Filename) < 0);
    TestNotNull(TEXT("never-saved fixture is findable before cleanup"),
        FindObject<UObject>(nullptr, *ObjectPath));

    CleanupTestAsset(PackagePath);

    TestTrue(TEXT("cleanup does not create a .uasset for a never-saved fixture"),
        IFileManager::Get().FileSize(*Filename) < 0);
    TestNull(TEXT("cleanup removes the never-saved fixture from its original object path"),
        FindObject<UObject>(nullptr, *ObjectPath));
    TestNull(TEXT("cleanup detaches the never-saved fixture package from its original path"),
        FindPackage(nullptr, *PackagePath));
    return true;
}
