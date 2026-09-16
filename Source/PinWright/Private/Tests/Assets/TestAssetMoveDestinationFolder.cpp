// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behavioural regression guard for B-asset-move-to-missing-folder-silently-renames.
//
// The defect: asset.move handed destinationPath verbatim to UEditorAssetLibrary::RenameAsset,
// whose second parameter is a full destination OBJECT path. UE::EditorAssetUtils::RenameLoadedAsset
// splits it unconditionally into GetLongPackagePath + ObjectPathToObjectName
// (EditorAssetSubsystem.cpp), and nothing on that path - not IsAValidPathForCreateNewAsset, not
// DoesPackageExist, not FAssetRenameManager, not ObjectTools::RenameSingleObject, which calls
// CreatePackage on the destination unconditionally - ever asks whether the destination FOLDER
// exists. So a caller asking to move an asset into /Game/New got the asset RENAMED to "New", and
// the response reported success with the requested path echoed straight back out of the request
// string. The handler's own verification block was skipped in silence whenever the destination
// would not load, so the one measured field was also the field that went missing exactly when it
// mattered.
//
// What these two tests judge on is where the asset actually is afterwards, never the response's
// echo of the request.
//
//   MissingDestinationFolderIsRefused - the reported case. Revert the fix and the asset is renamed
//   to "Grass" and moved into a folder that did not exist, so "the source asset is still at its
//   original path under its original name" and "nothing landed at the requested destination" both
//   go red, and the call reports success instead of DESTINATION_FOLDER_NOT_FOUND.
//
//   ExistingFolderKeepsTheAssetName - the documented case the handler never implemented
//   ("when a folder is given, the asset retains its original name"). Revert the fix and the asset
//   is renamed after the folder's leaf name instead of moved into it, so it is not at the expected
//   path and destinationInterpretedAs is absent entirely. It is also the guard against the refusal
//   above being over-broad: an existing folder must still be movable into.
//
// The fixture is saved to disk rather than left as a never-saved in-memory package because
// FAssetRenameManager checks out and re-saves the packages it touches; a rename measured against
// an unsaved fixture would not be measuring the production path this ticket was reported on.

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"

#include "Dom/JsonObject.h"

#include "Dispatch/ScopedUnattendedRpc.h"
#include "Tests/TestUtils.h"
#include "Tests/Utility/AssetDumpMismatchedNameFixture.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

// File-scope helpers carry the AssetMoveDestinationTest namespace: bUseUnity merges translation
// units, so an unprefixed helper collides with a same-named static in a neighbouring test file.
namespace AssetMoveDestinationTest
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
        return FString::Printf(TEXT("/Game/__PW_MoveTests/%s"), *Suffix);
    }

    // A saved, registry-scanned asset in its own package. No Parent on the instance: it keeps the
    // fixture self-contained and off the shader-compilation path.
    inline bool BuildSavedAsset(FAutomationTestBase& Test, const TCHAR* NamePrefix, FFixture& Out)
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

    // Wipes the whole fixture folder tree. Named paths are force-deleted first so the packages
    // leave memory; the directory removal is the backstop for anything the rename left behind
    // (a redirector, or a destination package written by a reverted fix).
    inline void TearDownFolder(const FString& FolderPath, const TArray<FString>& PackagePaths)
    {
        for (const FString& PackagePath : PackagePaths)
        {
            CleanupTestAsset(PackagePath);
        }
        FString FolderFilename;
        if (FPackageName::TryConvertLongPackageNameToFilename(FolderPath, FolderFilename, TEXT("")))
        {
            IFileManager::Get().DeleteDirectory(*FolderFilename, /*RequireExists=*/false, /*Tree=*/true);
        }
    }

    inline FString StringField(const TSharedPtr<FJsonObject>& Response, const TCHAR* FieldName)
    {
        FString Value;
        if (Response.IsValid())
        {
            Response->TryGetStringField(FieldName, Value);
        }
        return Value;
    }
}

// ============================================================================
// A destination whose folder does not exist is refused, not silently renamed.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetMoveMissingFolderRefusedTest,
    "PinWright.asset.move.MissingDestinationFolderIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetMoveMissingFolderRefusedTest::RunTest(const FString& Parameters)
{
    using namespace AssetMoveDestinationTest;

    // The refusal is an error response, which the handler logs.
    bSuppressLogErrors = true;

    FFixture Source;
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);

    // Teardown is registered before anything is created so LIFO ordering puts it last. The
    // destination package path is listed too: it is what a reverted fix would create.
    FString MissingFolder;
    FString RequestedDestination;
    ON_SCOPE_EXIT
    {
        if (!Source.FolderPath.IsEmpty())
        {
            TearDownFolder(Source.FolderPath, {Source.PackagePath, RequestedDestination});
        }
    };

    if (!BuildSavedAsset(*this, TEXT("MI_MoveSrc"), Source))
    {
        return true;
    }

    // The caller's typo: a folder that has never existed, with a leaf the old code would have
    // taken as the asset's new name.
    MissingFolder = FString::Printf(TEXT("%s/NoSuchFolder"), *Source.FolderPath);
    RequestedDestination = FString::Printf(TEXT("%s/Grass"), *MissingFolder);

    if (!TestFalse(TEXT("the destination folder really does not exist"),
            UEditorAssetLibrary::DoesDirectoryExist(MissingFolder)))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sourcePath"), Source.PackagePath);
    Payload->SetStringField(TEXT("destinationPath"), RequestedDestination);

    FTestResponseCapture Capture;
    {
        FScopedUnattendedRpc UnattendedScope;
        if (!TestTrue(TEXT("asset.move handler found"),
                InvokeHandlerWithCapture(TEXT("asset.move"), Payload, Capture)))
        {
            return true;
        }
    }

    if (!TestTrue(TEXT("asset.move sent a response"), Capture.bWasCalled))
    {
        return true;
    }

    // What the caller is told.
    TestFalse(TEXT("a move into a folder that does not exist is not reported as a success"),
        Capture.bSuccess);
    TestEqual(TEXT("the refusal carries the typed code"),
        Capture.ErrorCode, FString(TEXT("DESTINATION_FOLDER_NOT_FOUND")));
    TestTrue(TEXT("the message names the folder that is missing"),
        Capture.Message.Contains(MissingFolder));

    // What actually happened to the asset. These are the assertions the defect fails.
    TestFalse(TEXT("nothing landed at the requested destination"),
        UEditorAssetLibrary::DoesAssetExist(RequestedDestination));
    TestTrue(TEXT("the source asset is still registered at its original path"),
        UEditorAssetLibrary::DoesAssetExist(Source.PackagePath));

    UObject* StillThere = UEditorAssetLibrary::LoadAsset(Source.ObjectPath);
    if (TestNotNull(TEXT("the source asset is still loadable at its original path"), StillThere))
    {
        // LoadAsset follows a redirector, so this reads the object wherever the verb put it: a
        // rename shows up here as the new name even when a redirector hides the move.
        TestEqual(TEXT("the asset was not renamed after the missing folder's last segment"),
            StillThere->GetName(), Source.AssetName);
    }

    return true;
}

// ============================================================================
// An existing folder destination moves the asset and keeps its name.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetMoveExistingFolderKeepsNameTest,
    "PinWright.asset.move.ExistingFolderKeepsTheAssetName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetMoveExistingFolderKeepsNameTest::RunTest(const FString& Parameters)
{
    using namespace AssetMoveDestinationTest;

    // The rename path logs about checkout and re-save of the fixture packages.
    bSuppressLogErrors = true;

    FFixture Source;
    FString ExpectedPackagePath;
    ON_SCOPE_EXIT
    {
        if (!Source.FolderPath.IsEmpty())
        {
            TearDownFolder(Source.FolderPath, {ExpectedPackagePath, Source.PackagePath});
        }
    };

    if (!BuildSavedAsset(*this, TEXT("MI_MoveKeep"), Source))
    {
        return true;
    }

    const FString DestinationFolder = FString::Printf(TEXT("%s/Dest"), *Source.FolderPath);
    ExpectedPackagePath = FString::Printf(TEXT("%s/%s"), *DestinationFolder, *Source.AssetName);

    if (!TestTrue(TEXT("the destination folder was created"),
            UEditorAssetLibrary::MakeDirectory(DestinationFolder)
                || UEditorAssetLibrary::DoesDirectoryExist(DestinationFolder)))
    {
        return true;
    }
    if (!TestTrue(TEXT("the destination folder exists before the move"),
            UEditorAssetLibrary::DoesDirectoryExist(DestinationFolder)))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sourcePath"), Source.PackagePath);
    Payload->SetStringField(TEXT("destinationPath"), DestinationFolder);

    FTestResponseCapture Capture;
    {
        FScopedUnattendedRpc UnattendedScope;
        if (!TestTrue(TEXT("asset.move handler found"),
                InvokeHandlerWithCapture(TEXT("asset.move"), Payload, Capture)))
        {
            return true;
        }
    }

    if (!TestTrue(TEXT("asset.move sent a response"), Capture.bWasCalled))
    {
        return true;
    }
    if (!TestTrue(
            FString::Printf(TEXT("moving into an existing folder succeeds (code '%s': %s)"),
                *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess))
    {
        return true;
    }

    // The response says which reading it took, so the caller never has to infer it.
    TestEqual(TEXT("the destination was read as a folder"),
        StringField(Capture.Result, TEXT("destinationInterpretedAs")), FString(TEXT("folder")));
    TestEqual(TEXT("the resolved destination keeps the asset's own name"),
        StringField(Capture.Result, TEXT("resolvedDestinationPath")), ExpectedPackagePath);
    TestEqual(TEXT("the request string is echoed only under its own key"),
        StringField(Capture.Result, TEXT("requestedDestinationPath")), DestinationFolder);
    TestTrue(TEXT("assetPath is measured from the moved object, not echoed from the request"),
        StringField(Capture.Result, TEXT("assetPath")).Contains(ExpectedPackagePath));

    // Where the asset actually is.
    TestTrue(TEXT("the asset is registered inside the destination folder under its own name"),
        UEditorAssetLibrary::DoesAssetExist(ExpectedPackagePath));

    UObject* Moved = UEditorAssetLibrary::LoadAsset(ExpectedPackagePath);
    if (TestNotNull(TEXT("the asset is loadable at the expected path"), Moved))
    {
        TestEqual(TEXT("the asset kept its name across the move"),
            Moved->GetName(), Source.AssetName);
    }

    return true;
}
