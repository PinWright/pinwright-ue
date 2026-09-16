// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behavioural regression coverage for B-asset-rename-duplicate-unverified-destination.
// These tests exercise the handlers through the test harness and compare their read-back
// fields with the live asset registry and package files instead of trusting request echoes.

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"

#include "Dom/JsonObject.h"

#include "Dispatch/ScopedUnattendedRpc.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Handlers/Asset/AssetManageHandlerTestHooks.h"
#endif
#include "Tests/TestUtils.h"
#include "Tests/Utility/AssetDumpMismatchedNameFixture.h"
#include "Utils/AssetUtils.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace AssetRenameDuplicateVerificationTest
{
    struct FFixture
    {
        FString FolderPath;
        FString AssetName;
        FString PackagePath;
        FString ObjectPath;
        FString PackageFilename;
    };

    inline FString MakeFolderPath(const FString& Suffix)
    {
        return FString::Printf(TEXT("/Game/__PW_RenameDuplicateTests/%s"), *Suffix);
    }

    inline bool BuildSavedAsset(FAutomationTestBase& Test, const TCHAR* NamePrefix,
                                FFixture& Out)
    {
        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Out.FolderPath = MakeFolderPath(Suffix);
        Out.AssetName = FString::Printf(TEXT("%s_%s"), NamePrefix, *Suffix);
        Out.PackagePath = FString::Printf(TEXT("%s/%s"), *Out.FolderPath, *Out.AssetName);
        Out.ObjectPath = FString::Printf(TEXT("%s.%s"), *Out.PackagePath, *Out.AssetName);

        if (!Test.TestTrue(TEXT("fixture package path resolves to a filename"),
                FPackageName::TryConvertLongPackageNameToFilename(
                    Out.PackagePath, Out.PackageFilename, FPackageName::GetAssetPackageExtension())))
        {
            return false;
        }

        UPackage* Package = CreatePackage(*Out.PackagePath);
        if (!Test.TestNotNull(TEXT("fixture package created"), Package))
        {
            return false;
        }

        UMaterialInstanceConstant* Asset = NewObject<UMaterialInstanceConstant>(
            Package, FName(*Out.AssetName), RF_Public | RF_Standalone);
        if (!Test.TestNotNull(TEXT("fixture asset created"), Asset))
        {
            return false;
        }

        bool bSaved = false;
        {
            AssetDumpMismatchedNameFixture::FScopedDisableValidateOnSaveReflect DisableValidation;
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            SaveArgs.SaveFlags = SAVE_NoError;
            bSaved = UPackage::SavePackage(Package, Asset, *Out.PackageFilename, SaveArgs);
        }
        if (!Test.TestTrue(TEXT("fixture package saved to disk"), bSaved))
        {
            return false;
        }

        IAssetRegistry& Registry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        Registry.ScanFilesSynchronous({Out.PackageFilename}, /*bForceRescan=*/true);

        return Test.TestTrue(TEXT("fixture asset is registered at its own path"),
            UEditorAssetLibrary::DoesAssetExist(Out.PackagePath));
    }

    inline void TearDownFolder(const FString& FolderPath, const TArray<FString>& PackagePaths)
    {
        for (const FString& PackagePath : PackagePaths)
        {
            CleanupTestAsset(PackagePath);
        }

        FString FolderFilename;
        if (FPackageName::TryConvertLongPackageNameToFilename(FolderPath, FolderFilename, TEXT("")))
        {
            IFileManager::Get().DeleteDirectory(*FolderFilename,
                /*RequireExists=*/false, /*Tree=*/true);
        }
    }

    inline bool ReadBool(const TSharedPtr<FJsonObject>& Result, const TCHAR* FieldName,
                         bool& OutValue)
    {
        return Result.IsValid() && Result->TryGetBoolField(FieldName, OutValue);
    }

    inline FString ReadString(const TSharedPtr<FJsonObject>& Result, const TCHAR* FieldName)
    {
        FString Value;
        if (Result.IsValid())
        {
            Result->TryGetStringField(FieldName, Value);
        }
        return Value;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDuplicateReportsRegistryAndDiskReadbackTest,
    "PinWright.asset.duplicate.ReportsRegistryAndDiskReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDuplicateReportsRegistryAndDiskReadbackTest::RunTest(const FString& Parameters)
{
    using namespace AssetRenameDuplicateVerificationTest;

    bSuppressLogErrors = true;

    FFixture Source;
    FString DestinationPath;
    ON_SCOPE_EXIT
    {
        if (!Source.FolderPath.IsEmpty())
        {
            TearDownFolder(Source.FolderPath, {DestinationPath, Source.PackagePath});
        }
    };

    if (!BuildSavedAsset(*this, TEXT("MI_DuplicateSource"), Source))
    {
        return true;
    }

    DestinationPath = FString::Printf(TEXT("%s/MI_DuplicateCopy"), *Source.FolderPath);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sourcePath"), Source.PackagePath);
    Payload->SetStringField(TEXT("destinationPath"), DestinationPath);

    FTestResponseCapture Capture;
    {
        FScopedUnattendedRpc UnattendedScope;
        if (!TestTrue(TEXT("asset.duplicate handler found"),
                InvokeHandlerWithCapture(TEXT("asset.duplicate"), Payload, Capture)))
        {
            return true;
        }
    }

    if (!TestTrue(TEXT("asset.duplicate sent a response"), Capture.bWasCalled)
        || !TestTrue(FString::Printf(TEXT("asset.duplicate succeeds (code '%s': %s)"),
                *Capture.ErrorCode, *Capture.Message), Capture.bSuccess))
    {
        return true;
    }

    const FResolvedAsset SourceReadback = ResolveAsset(
        Source.ObjectPath, /*bLoadObject=*/true);
    const FResolvedAsset DestinationReadback = ResolveAsset(
        DestinationPath, /*bLoadObject=*/true);
    const bool bSourceOnDisk = DoesPackageFileExistOnDisk(Source.PackagePath);
    const bool bDestinationOnDisk = DoesPackageFileExistOnDisk(DestinationPath);

    TestTrue(TEXT("duplicate source remains registered"), SourceReadback.bRegistryOrMemoryExists);
    TestTrue(TEXT("duplicate source remains loadable"), SourceReadback.Object != nullptr);
    TestTrue(TEXT("duplicate destination is registered"),
        DestinationReadback.bRegistryOrMemoryExists);
    TestTrue(TEXT("duplicate destination is loadable"), DestinationReadback.Object != nullptr);

    bool bSourceExistsAfter = false;
    bool bSourceExistsOnDisk = false;
    bool bDestinationExistsAfter = false;
    bool bDestinationExistsOnDisk = false;
    TestTrue(TEXT("response reports source existence state"),
        ReadBool(Capture.Result, TEXT("sourceExistsAfter"), bSourceExistsAfter));
    TestTrue(TEXT("response reports source disk state"),
        ReadBool(Capture.Result, TEXT("sourceExistsOnDisk"), bSourceExistsOnDisk));
    TestTrue(TEXT("response reports destination existence state"),
        ReadBool(Capture.Result, TEXT("destinationExistsAfter"), bDestinationExistsAfter));
    TestTrue(TEXT("response reports destination disk state"),
        ReadBool(Capture.Result, TEXT("destinationExistsOnDisk"), bDestinationExistsOnDisk));
    TestEqual(TEXT("source existence readback matches the registry/disk probe"),
        bSourceExistsAfter, SourceReadback.bRegistryOrMemoryExists || bSourceOnDisk);
    TestEqual(TEXT("source disk readback matches the package probe"),
        bSourceExistsOnDisk, bSourceOnDisk);
    TestEqual(TEXT("destination existence readback matches the registry/disk probe"),
        bDestinationExistsAfter, DestinationReadback.bRegistryOrMemoryExists || bDestinationOnDisk);
    TestEqual(TEXT("destination disk readback matches the package probe"),
        bDestinationExistsOnDisk, bDestinationOnDisk);
    TestEqual(TEXT("assetPath is the observed duplicate object path"),
        ReadString(Capture.Result, TEXT("assetPath")), DestinationReadback.Object->GetPathName());
    TestEqual(TEXT("destinationObservedPath is the observed duplicate object path"),
        ReadString(Capture.Result, TEXT("destinationObservedPath")),
        DestinationReadback.Object->GetPathName());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetRenameReportsDestinationAndSourceReadbackTest,
    "PinWright.asset.rename.ReportsDestinationAndSourceReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetRenameReportsDestinationAndSourceReadbackTest::RunTest(const FString& Parameters)
{
    using namespace AssetRenameDuplicateVerificationTest;

    bSuppressLogErrors = true;

    FFixture Source;
    FString DestinationPath;
    ON_SCOPE_EXIT
    {
        if (!Source.FolderPath.IsEmpty())
        {
            TearDownFolder(Source.FolderPath, {DestinationPath, Source.PackagePath});
        }
    };

    if (!BuildSavedAsset(*this, TEXT("MI_RenameSource"), Source))
    {
        return true;
    }

    DestinationPath = FString::Printf(TEXT("%s/MI_RenamedAsset"), *Source.FolderPath);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sourcePath"), Source.PackagePath);
    Payload->SetStringField(TEXT("destinationPath"), DestinationPath);

    FTestResponseCapture Capture;
    {
        FScopedUnattendedRpc UnattendedScope;
        if (!TestTrue(TEXT("asset.rename handler found"),
                InvokeHandlerWithCapture(TEXT("asset.rename"), Payload, Capture)))
        {
            return true;
        }
    }

    if (!TestTrue(TEXT("asset.rename sent a response"), Capture.bWasCalled)
        || !TestTrue(FString::Printf(TEXT("asset.rename succeeds (code '%s': %s)"),
                *Capture.ErrorCode, *Capture.Message), Capture.bSuccess))
    {
        return true;
    }

    const FResolvedAsset SourceReadback = ResolveAsset(
        Source.ObjectPath, /*bLoadObject=*/false);
    const FResolvedAsset DestinationReadback = ResolveAsset(
        DestinationPath, /*bLoadObject=*/true);
    const bool bSourceIsRedirector =
        (SourceReadback.AssetData.IsValid() && SourceReadback.AssetData.IsRedirector())
        || FAssetData::IsRedirector(SourceReadback.Object);
    const bool bSourceOnDisk = DoesPackageFileExistOnDisk(Source.PackagePath);
    const bool bDestinationOnDisk = DoesPackageFileExistOnDisk(DestinationPath);

    TestTrue(TEXT("renamed destination is registered"),
        DestinationReadback.bRegistryOrMemoryExists);
    TestTrue(TEXT("renamed destination is loadable"), DestinationReadback.Object != nullptr);
    TestTrue(TEXT("source path no longer contains the original asset"),
        !SourceReadback.bRegistryOrMemoryExists || bSourceIsRedirector);

    bool bSourceExistsAfter = true;
    bool bSourceExistsOnDisk = false;
    bool bSourceReportedRedirector = false;
    bool bDestinationExistsAfter = false;
    bool bDestinationExistsOnDisk = false;
    TestTrue(TEXT("response reports renamed source state"),
        ReadBool(Capture.Result, TEXT("sourceExistsAfter"), bSourceExistsAfter));
    TestTrue(TEXT("response reports renamed source disk state"),
        ReadBool(Capture.Result, TEXT("sourceExistsOnDisk"), bSourceExistsOnDisk));
    TestTrue(TEXT("response reports redirector state"),
        ReadBool(Capture.Result, TEXT("sourceIsRedirector"), bSourceReportedRedirector));
    TestTrue(TEXT("response reports renamed destination state"),
        ReadBool(Capture.Result, TEXT("destinationExistsAfter"), bDestinationExistsAfter));
    TestTrue(TEXT("response reports renamed destination disk state"),
        ReadBool(Capture.Result, TEXT("destinationExistsOnDisk"), bDestinationExistsOnDisk));
    TestFalse(TEXT("rename reports the original source asset as gone"), bSourceExistsAfter);
    TestEqual(TEXT("rename reports whether the source package remains on disk"),
        bSourceExistsOnDisk, bSourceOnDisk);
    TestEqual(TEXT("rename reports the source redirector state"),
        bSourceReportedRedirector, bSourceIsRedirector);
    TestEqual(TEXT("rename destination existence matches the registry/disk probe"),
        bDestinationExistsAfter, DestinationReadback.bRegistryOrMemoryExists || bDestinationOnDisk);
    TestEqual(TEXT("rename destination disk state matches the package probe"),
        bDestinationExistsOnDisk, bDestinationOnDisk);
    TestEqual(TEXT("assetPath is the observed renamed object path"),
        ReadString(Capture.Result, TEXT("assetPath")), DestinationReadback.Object->GetPathName());
    TestEqual(TEXT("destinationObservedPath is the observed renamed object path"),
        ReadString(Capture.Result, TEXT("destinationObservedPath")),
        DestinationReadback.Object->GetPathName());
    return true;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDuplicateRejectsMissingDestinationReadbackTest,
    "PinWright.asset.duplicate.RejectsMissingDestinationReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDuplicateRejectsMissingDestinationReadbackTest::RunTest(const FString& Parameters)
{
    using namespace AssetRenameDuplicateVerificationTest;

    bSuppressLogErrors = true;

    FFixture Source;
    FString DestinationPath;
    ON_SCOPE_EXIT
    {
        if (!Source.FolderPath.IsEmpty())
        {
            TearDownFolder(Source.FolderPath, {DestinationPath, Source.PackagePath});
        }
    };

    if (!BuildSavedAsset(*this, TEXT("MI_DuplicateReadbackFailure"), Source))
    {
        return true;
    }

    DestinationPath = FString::Printf(TEXT("%s/MI_DuplicateReadbackCopy"), *Source.FolderPath);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sourcePath"), Source.PackagePath);
    Payload->SetStringField(TEXT("destinationPath"), DestinationPath);

    FTestResponseCapture Capture;
    {
        PinWrightAssetManageTestHooks::FScopedForceMissingDestinationReadback ForceReadback;
        FScopedUnattendedRpc UnattendedScope;
        TestTrue(TEXT("asset.duplicate handler found"),
            InvokeHandlerWithCapture(TEXT("asset.duplicate"), Payload, Capture));
    }

    TestTrue(TEXT("asset.duplicate sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("asset.duplicate rejects missing destination readback"), Capture.bSuccess);
    TestEqual(TEXT("asset.duplicate reports its typed failure code"),
        Capture.ErrorCode, FString(TEXT("DUPLICATE_FAILED")));

    const FResolvedAsset SourceReadback = ResolveAsset(Source.ObjectPath, /*bLoadObject=*/true);
    const FResolvedAsset ActualDestination = ResolveAsset(
        DestinationPath, /*bLoadObject=*/true);
    const bool bSourceOnDisk = DoesPackageFileExistOnDisk(Source.PackagePath);
    const bool bDestinationOnDisk = DoesPackageFileExistOnDisk(DestinationPath);

    TestTrue(TEXT("duplicate source remains registered after failure"),
        SourceReadback.bRegistryOrMemoryExists);
    TestTrue(TEXT("duplicate destination remains observable for cleanup"),
        ActualDestination.bRegistryOrMemoryExists || bDestinationOnDisk);

    bool bSourceExistsAfter = false;
    bool bSourceExistsOnDisk = false;
    bool bDestinationExistsAfter = true;
    bool bDestinationExistsOnDisk = false;
    TestTrue(TEXT("failure reports source existence state"),
        ReadBool(Capture.Result, TEXT("sourceExistsAfter"), bSourceExistsAfter));
    TestTrue(TEXT("failure reports source disk state"),
        ReadBool(Capture.Result, TEXT("sourceExistsOnDisk"), bSourceExistsOnDisk));
    TestTrue(TEXT("failure reports destination existence state"),
        ReadBool(Capture.Result, TEXT("destinationExistsAfter"), bDestinationExistsAfter));
    TestTrue(TEXT("failure reports destination disk state"),
        ReadBool(Capture.Result, TEXT("destinationExistsOnDisk"), bDestinationExistsOnDisk));
    TestEqual(TEXT("failure source existence matches registry/disk probe"),
        bSourceExistsAfter, SourceReadback.bRegistryOrMemoryExists || bSourceOnDisk);
    TestEqual(TEXT("failure source disk state matches package probe"),
        bSourceExistsOnDisk, bSourceOnDisk);
    TestEqual(TEXT("failure destination existence reports the disk probe after forced miss"),
        bDestinationExistsAfter, bDestinationOnDisk);
    TestEqual(TEXT("failure destination disk state matches package probe"),
        bDestinationExistsOnDisk, bDestinationOnDisk);
    TestTrue(TEXT("failure destination observed path is empty after forced miss"),
        ReadString(Capture.Result, TEXT("destinationObservedPath")).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetRenameRejectsMissingDestinationReadbackTest,
    "PinWright.asset.rename.RejectsMissingDestinationReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetRenameRejectsMissingDestinationReadbackTest::RunTest(const FString& Parameters)
{
    using namespace AssetRenameDuplicateVerificationTest;

    bSuppressLogErrors = true;

    FFixture Source;
    FString DestinationPath;
    ON_SCOPE_EXIT
    {
        if (!Source.FolderPath.IsEmpty())
        {
            TearDownFolder(Source.FolderPath, {DestinationPath, Source.PackagePath});
        }
    };

    if (!BuildSavedAsset(*this, TEXT("MI_RenameReadbackFailure"), Source))
    {
        return true;
    }

    DestinationPath = FString::Printf(TEXT("%s/MI_RenameReadbackTarget"), *Source.FolderPath);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sourcePath"), Source.PackagePath);
    Payload->SetStringField(TEXT("destinationPath"), DestinationPath);

    FTestResponseCapture Capture;
    {
        PinWrightAssetManageTestHooks::FScopedForceMissingDestinationReadback ForceReadback;
        FScopedUnattendedRpc UnattendedScope;
        TestTrue(TEXT("asset.rename handler found"),
            InvokeHandlerWithCapture(TEXT("asset.rename"), Payload, Capture));
    }

    TestTrue(TEXT("asset.rename sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("asset.rename rejects missing destination readback"), Capture.bSuccess);
    TestEqual(TEXT("asset.rename reports its typed failure code"),
        Capture.ErrorCode, FString(TEXT("RENAME_FAILED")));

    const FResolvedAsset SourceReadback = ResolveAsset(
        Source.ObjectPath, /*bLoadObject=*/false);
    const FResolvedAsset ActualDestination = ResolveAsset(
        DestinationPath, /*bLoadObject=*/true);
    const bool bSourceIsRedirector =
        (SourceReadback.AssetData.IsValid() && SourceReadback.AssetData.IsRedirector())
        || FAssetData::IsRedirector(SourceReadback.Object);
    const bool bSourceOnDisk = DoesPackageFileExistOnDisk(Source.PackagePath);
    const bool bDestinationOnDisk = DoesPackageFileExistOnDisk(DestinationPath);

    TestTrue(TEXT("rename destination remains observable for cleanup"),
        ActualDestination.bRegistryOrMemoryExists || bDestinationOnDisk);
    TestTrue(TEXT("rename source is gone or a redirector after failure"),
        !SourceReadback.bRegistryOrMemoryExists || bSourceIsRedirector);

    bool bSourceExistsAfter = true;
    bool bSourceExistsOnDisk = false;
    bool bSourceReportedRedirector = false;
    bool bDestinationExistsAfter = true;
    bool bDestinationExistsOnDisk = false;
    TestTrue(TEXT("failure reports renamed source state"),
        ReadBool(Capture.Result, TEXT("sourceExistsAfter"), bSourceExistsAfter));
    TestTrue(TEXT("failure reports renamed source disk state"),
        ReadBool(Capture.Result, TEXT("sourceExistsOnDisk"), bSourceExistsOnDisk));
    TestTrue(TEXT("failure reports source redirector state"),
        ReadBool(Capture.Result, TEXT("sourceIsRedirector"), bSourceReportedRedirector));
    TestTrue(TEXT("failure reports destination existence state"),
        ReadBool(Capture.Result, TEXT("destinationExistsAfter"), bDestinationExistsAfter));
    TestTrue(TEXT("failure reports destination disk state"),
        ReadBool(Capture.Result, TEXT("destinationExistsOnDisk"), bDestinationExistsOnDisk));
    TestFalse(TEXT("failure reports the original source asset as gone"), bSourceExistsAfter);
    TestEqual(TEXT("failure source disk state matches package probe"),
        bSourceExistsOnDisk, bSourceOnDisk);
    TestEqual(TEXT("failure source redirector state matches registry probe"),
        bSourceReportedRedirector, bSourceIsRedirector);
    TestEqual(TEXT("failure destination existence reports the disk probe after forced miss"),
        bDestinationExistsAfter, bDestinationOnDisk);
    TestEqual(TEXT("failure destination disk state matches package probe"),
        bDestinationExistsOnDisk, bDestinationOnDisk);
    TestTrue(TEXT("failure destination observed path is empty after forced miss"),
        ReadString(Capture.Result, TEXT("destinationObservedPath")).IsEmpty());
    return true;
}
#endif
