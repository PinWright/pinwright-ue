// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behavioral regression coverage for the asset.import overwrite and complete-
// output contract. Every fixture uses a GUID root under /Game/PinWrightTests.

#include "Misc/AutomationTest.h"

#include "AutomatedAssetImportData.h"
#include "Compat/EngineVersionCompat.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Curves/CurveFloat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Handlers/Asset/AssetImportHandler.h"
#include "Handlers/ErrorCodes.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/PackageSegment.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
#include "Serialization/BulkDataCookedIndex.h"
#endif
#include "Tests/Assets/PinWrightAssetImportReferenceActor.h"
#include "Tests/Assets/PinWrightAssetImportTestFactory.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "UObject/Package.h"
#include "UObject/PackageResourceManager.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Utils/AssetImportPolicy.h"
#include "Utils/AssetUtils.h"

namespace AssetImportPolicyTests
{
    struct FTextureFixture
    {
        FString Guid;
        FString AssetRoot;
        FString PackageName;
        FString ObjectPath;
        FString SourceDirectory;
        FString SourcePath;
        UTexture2D* Texture = nullptr;
        AssetImportPolicy::FDestinationSnapshot SavedSnapshot;
    };

    FString NewGuid()
    {
        return FGuid::NewGuid().ToString(EGuidFormats::Digits);
    }

    TSharedPtr<FJsonObject> MakeImportPayload(
        const FString& SourcePath,
        const FString& DestinationPath,
        bool bIncludeOverwrite,
        bool bOverwrite)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("sourcePath"), SourcePath);
        Payload->SetStringField(TEXT("destinationPath"), DestinationPath);
        if (bIncludeOverwrite)
        {
            Payload->SetBoolField(TEXT("overwrite"), bOverwrite);
        }
        return Payload;
    }

    TSharedRef<FTestResponseCapture> InvokeImport(
        FAutomationTestBase& Test,
        const FString& SourcePath,
        const FString& DestinationPath,
        bool bIncludeOverwrite = false,
        bool bOverwrite = false)
    {
        TSharedRef<FTestResponseCapture> Capture =
            MakeShared<FTestResponseCapture>();
        Test.TestTrue(
            TEXT("asset.import handler is registered"),
            InvokeHandlerWithSharedCapture(
                TEXT("asset.import"),
                MakeImportPayload(
                    SourcePath, DestinationPath,
                    bIncludeOverwrite, bOverwrite),
                Capture));
        PumpUntilCaptured(*Capture, 30.0);
        Test.TestTrue(TEXT("asset.import responded"), Capture->bWasCalled);
        return Capture;
    }

    bool WritePng(const FString& Filename, const FColor& Color)
    {
        TArray<FColor> Pixels;
        Pixels.Init(Color, 16);
        TArray64<uint8> Compressed;
        FImageUtils::PNGCompressImageArray(
            4, 4,
            TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()),
            Compressed);
        return !Compressed.IsEmpty()
            && FFileHelper::SaveArrayToFile(Compressed, *Filename);
    }

    bool CaptureSavedSnapshot(
        const FString& PackageName,
        AssetImportPolicy::FDestinationSnapshot& OutSnapshot,
        FString& OutError)
    {
        TArray<FString> Packages{PackageName};
        return AssetImportPolicy::CaptureDestinationSnapshot(
            Packages, OutSnapshot, OutError);
    }

    FTextureFixture CreateSavedTextureFixture(
        FAutomationTestBase& Test, const FString& Guid)
    {
        FTextureFixture Fixture;
        Fixture.Guid = Guid;
        Fixture.AssetRoot = TEXT("/Game/PinWrightTests/") + Fixture.Guid;
        Fixture.PackageName = Fixture.AssetRoot + TEXT("/PW_ImportedTexture");
        Fixture.ObjectPath =
            Fixture.PackageName + TEXT(".PW_ImportedTexture");
        Fixture.SourceDirectory = FPaths::Combine(
            FPaths::ProjectSavedDir(), TEXT("PinWrightTests"), Fixture.Guid);
        Fixture.SourcePath = FPaths::Combine(
            Fixture.SourceDirectory, TEXT("PW_ImportedTexture.png"));

        Test.TestTrue(
            TEXT("source fixture directory created"),
            IFileManager::Get().MakeDirectory(
                *Fixture.SourceDirectory, /*Tree=*/true));
        if (!Test.TestTrue(
                TEXT("initial PNG source written"),
                WritePng(Fixture.SourcePath, FColor::Red)))
        {
            return Fixture;
        }

        const TSharedRef<FTestResponseCapture> ImportCapture =
            InvokeImport(Test, Fixture.SourcePath, Fixture.AssetRoot);
        if (!Test.TestTrue(
                TEXT("initial production texture import succeeds"),
                ImportCapture->bSuccess))
        {
            return Fixture;
        }

        Fixture.Texture = LoadObject<UTexture2D>(
            nullptr, *Fixture.ObjectPath);
        if (!Test.TestNotNull(
                TEXT("initial import produced UTexture2D"),
                Fixture.Texture))
        {
            return Fixture;
        }
        if (!Test.TestTrue(
                TEXT("initial texture saved to disk"),
                UEditorAssetLibrary::SaveLoadedAsset(
                    Fixture.Texture, /*bOnlyIfIsDirty=*/false)))
        {
            return Fixture;
        }

        FString SnapshotError;
        Test.TestTrue(
            TEXT("saved texture snapshot captured"),
            CaptureSavedSnapshot(
                Fixture.PackageName, Fixture.SavedSnapshot,
                SnapshotError));
        Test.TestTrue(
            TEXT("saved texture snapshot has package resources"),
            Fixture.SavedSnapshot.PackageResources.Contains(
                Fixture.PackageName));
        return Fixture;
    }

    bool ArePackageResourcesAbsent(const FString& PackageName)
    {
        FPackagePath PackagePath;
        if (!FPackagePath::TryFromPackageName(PackageName, PackagePath))
        {
            return false;
        }
        static constexpr EPackageSegment Segments[] = {
            EPackageSegment::Header,
            EPackageSegment::Exports,
            EPackageSegment::BulkDataDefault,
            EPackageSegment::BulkDataOptional,
            EPackageSegment::BulkDataMemoryMapped,
            EPackageSegment::PayloadSidecar,
        };
        IPackageResourceManager& Resources = IPackageResourceManager::Get();
        for (const EPackageSegment Segment : Segments)
        {
            if (Resources.DoesPackageExist(
                    MCP_PACKAGE_RESOURCE_ARGS(PackagePath, Segment)))
            {
                return false;
            }
        }
        return true;
    }

    bool AssertRootRegistryAbsent(
        FAutomationTestBase& Test, const FString& AssetRoot)
    {
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*AssetRoot));
        Filter.bRecursivePaths = true;
        TArray<FAssetData> RemainingAssets;
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
            TEXT("AssetRegistry")).Get().GetAssets(Filter, RemainingAssets);
        const bool bRegistryEmpty = RemainingAssets.IsEmpty();
        Test.TestTrue(TEXT("fixture registry root is empty"), bRegistryEmpty);
        return bRegistryEmpty;
    }

    bool DeleteRootUnforced(
        FAutomationTestBase& Test,
        const FString& AssetRoot,
        TArray<FString> ExpectedPackageNames)
    {
        if (AssetRoot.IsEmpty())
        {
            return true;
        }
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), AssetRoot);
        Payload->SetBoolField(TEXT("force"), false);
        TSharedRef<FTestResponseCapture> Capture =
            MakeShared<FTestResponseCapture>();
        const bool bInvoked = InvokeHandlerWithSharedCapture(
            TEXT("asset.delete"), Payload, Capture);
        Test.TestTrue(TEXT("unforced fixture delete handler invoked"), bInvoked);
        if (bInvoked)
        {
            PumpUntilCaptured(*Capture, 30.0);
        }
        Test.TestTrue(TEXT("unforced fixture delete responded"), Capture->bWasCalled);
        Test.TestTrue(TEXT("unforced fixture delete succeeded"), Capture->bSuccess);

        bool bRegistryAndResourcesAbsent =
            AssertRootRegistryAbsent(Test, AssetRoot);
        IAssetRegistry& Registry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
                TEXT("AssetRegistry")).Get();
        for (const FString& PackageName : ExpectedPackageNames)
        {
            TArray<FAssetData> PackageAssets;
            Registry.GetAssetsByPackageName(
                FName(*PackageName), PackageAssets,
                /*bIncludeOnlyOnDiskAssets=*/false,
                /*bSkipARFilteredAssets=*/false);
            const bool bRegistryPackageAbsent = PackageAssets.IsEmpty();
            const bool bResourcesAbsent =
                ArePackageResourcesAbsent(PackageName);
            Test.TestTrue(
                *FString::Printf(
                    TEXT("deleted package '%s' has no registry rows"),
                    *PackageName),
                bRegistryPackageAbsent);
            Test.TestTrue(
                *FString::Printf(
                    TEXT("deleted package '%s' has no package resources"),
                    *PackageName),
                bResourcesAbsent);
            bRegistryAndResourcesAbsent = bRegistryAndResourcesAbsent
                && bRegistryPackageAbsent && bResourcesAbsent;
        }

        FString RootDirectory;
        const bool bHasDiskMapping =
            FPackageName::TryConvertLongPackageNameToFilename(
                AssetRoot, RootDirectory);
        bool bDirectoryRemoved = bHasDiskMapping;
        if (bHasDiskMapping
            && IFileManager::Get().DirectoryExists(*RootDirectory))
        {
            bDirectoryRemoved = IFileManager::Get().DeleteDirectory(
                *RootDirectory, /*RequireExists=*/true, /*Tree=*/false);
        }
        bDirectoryRemoved = bDirectoryRemoved
            && !IFileManager::Get().DirectoryExists(*RootDirectory);
        Test.TestTrue(
            TEXT("unique empty fixture content directory removed explicitly"),
            bDirectoryRemoved);
        Test.TestFalse(
            TEXT("unique fixture content directory is absent"),
            bHasDiskMapping
                && IFileManager::Get().DirectoryExists(*RootDirectory));
        return Capture->bSuccess
            && bRegistryAndResourcesAbsent && bDirectoryRemoved;
    }

    void CleanupTextureFixture(
        FAutomationTestBase& Test, const FTextureFixture& Fixture)
    {
        DeleteRootUnforced(
            Test, Fixture.AssetRoot, TArray<FString>{Fixture.PackageName});
        if (!Fixture.SourcePath.IsEmpty())
        {
            const bool bDeleted = IFileManager::Get().Delete(
                *Fixture.SourcePath, /*RequireExists=*/false,
                /*EvenReadOnly=*/true);
            Test.TestTrue(
                TEXT("fixture source file removed"),
                bDeleted || !IFileManager::Get().FileExists(*Fixture.SourcePath));
        }
        if (!Fixture.SourceDirectory.IsEmpty())
        {
            const bool bDeleted = IFileManager::Get().DeleteDirectory(
                *Fixture.SourceDirectory,
                /*RequireExists=*/false,
                /*Tree=*/true);
            Test.TestTrue(
                TEXT("fixture source directory removed"),
                bDeleted
                    || !IFileManager::Get().DirectoryExists(
                        *Fixture.SourceDirectory));
        }
    }

    APinWrightAssetImportReferenceActor* SpawnReferenceActor(
        UTexture2D* Texture)
    {
        UWorld* World = GEditor
            ? GEditor->GetEditorWorldContext().World()
            : nullptr;
        if (!World)
        {
            return nullptr;
        }
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.ObjectFlags = RF_Transient;
        APinWrightAssetImportReferenceActor* Actor =
            World->SpawnActor<APinWrightAssetImportReferenceActor>(
                FVector::ZeroVector, FRotator::ZeroRotator,
                SpawnParameters);
        if (Actor && Texture)
        {
            Actor->HardReference = Texture;
            Actor->SoftReference = TSoftObjectPtr<UTexture2D>(Texture);
        }
        return Actor;
    }

    UObject* ResolveRegistryObject(const FString& ObjectPath)
    {
        IAssetRegistry& Registry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
                TEXT("AssetRegistry")).Get();
        const FAssetData AssetData = Registry.GetAssetByObjectPath(
            FSoftObjectPath(ObjectPath),
            /*bIncludeOnlyOnDiskAssets=*/false);
        return AssetData.IsValid()
            ? AssetData.FastGetAsset(false)
            : nullptr;
    }

    bool ResourcesMatchSavedSnapshot(
        const FTextureFixture& Fixture)
    {
        AssetImportPolicy::FDestinationSnapshot CurrentSnapshot;
        FString SnapshotError;
        if (!CaptureSavedSnapshot(
                Fixture.PackageName, CurrentSnapshot, SnapshotError))
        {
            return false;
        }
        const TArray<AssetImportPolicy::FPackageResource>* Expected =
            Fixture.SavedSnapshot.PackageResources.Find(
                Fixture.PackageName);
        const TArray<AssetImportPolicy::FPackageResource>* Current =
            CurrentSnapshot.PackageResources.Find(Fixture.PackageName);
        if (!Expected || !Current || Expected->Num() != Current->Num())
        {
            return false;
        }
        for (int32 Index = 0; Index < Expected->Num(); ++Index)
        {
            if (!(*Expected)[Index].Filename.Equals(
                    (*Current)[Index].Filename, ESearchCase::IgnoreCase)
                || (*Expected)[Index].Size != (*Current)[Index].Size
                || (*Expected)[Index].Timestamp != (*Current)[Index].Timestamp
                || (*Expected)[Index].Sha1 != (*Current)[Index].Sha1)
            {
                return false;
            }
        }
        return true;
    }

    struct FSavedPackageFileSnapshot
    {
        FString Filename;
        int64 Size = 0;
        FDateTime Timestamp;
        TArray<uint8> Bytes;
    };

    bool CaptureSavedPackageFile(
        const FString& PackageName,
        FSavedPackageFileSnapshot& OutSnapshot)
    {
        OutSnapshot = FSavedPackageFileSnapshot();
        OutSnapshot.Filename = FPackageName::LongPackageNameToFilename(
            PackageName, FPackageName::GetAssetPackageExtension());
        OutSnapshot.Size =
            IFileManager::Get().FileSize(*OutSnapshot.Filename);
        if (OutSnapshot.Size < 0
            || OutSnapshot.Size > AssetImportPolicy::MaxDestinationByteCount)
        {
            return false;
        }
        OutSnapshot.Timestamp =
            IFileManager::Get().GetTimeStamp(*OutSnapshot.Filename);
        return FFileHelper::LoadFileToArray(
                OutSnapshot.Bytes, *OutSnapshot.Filename)
            && OutSnapshot.Bytes.Num() == OutSnapshot.Size;
    }

    bool SavedPackageFileMatches(
        const FSavedPackageFileSnapshot& Expected)
    {
        TArray<uint8> CurrentBytes;
        return IFileManager::Get().FileSize(*Expected.Filename)
                == Expected.Size
            && IFileManager::Get().GetTimeStamp(*Expected.Filename)
                == Expected.Timestamp
            && FFileHelper::LoadFileToArray(
                CurrentBytes, *Expected.Filename)
            && CurrentBytes == Expected.Bytes;
    }

    TSharedPtr<FJsonObject> GetResultEntry(
        const TSharedPtr<FJsonObject>& Data,
        int32 Index)
    {
        const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
        if (!Data
            || !Data->TryGetArrayField(TEXT("results"), Results)
            || !Results
            || !Results->IsValidIndex(Index)
            || !(*Results)[Index])
        {
            return nullptr;
        }
        return (*Results)[Index]->AsObject();
    }

    bool HasLegacyStageFields(const TSharedPtr<FJsonObject>& Data)
    {
        if (!Data)
        {
            return false;
        }
        static constexpr const TCHAR* LegacyFields[] = {
            TEXT("staged"), TEXT("restored"), TEXT("removedCreated"),
            TEXT("stagePackage"), TEXT("restoreSucceeded"),
            TEXT("rolledBack"), TEXT("rollbackSucceeded"),
        };
        for (const TCHAR* Field : LegacyFields)
        {
            if (Data->HasField(Field))
            {
                return true;
            }
        }
        const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
        if (Data->TryGetArrayField(TEXT("results"), Results) && Results)
        {
            for (const TSharedPtr<FJsonValue>& Result : *Results)
            {
                if (Result && HasLegacyStageFields(Result->AsObject()))
                {
                    return true;
                }
            }
        }
        return false;
    }

    bool JsonArrayContains(
        const TSharedPtr<FJsonObject>& Data,
        const TCHAR* Field,
        const FString& Expected)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Data
            || !Data->TryGetArrayField(Field, Values)
            || !Values)
        {
            return false;
        }
        return Values->ContainsByPredicate(
            [&Expected](const TSharedPtr<FJsonValue>& Value)
            {
                FString Actual;
                return Value
                    && Value->TryGetString(Actual)
                    && Actual.Equals(
                        Expected, ESearchCase::IgnoreCase);
            });
    }

    PinWrightAssetImportHandler::FRequest MakeHandlerRequest(
        const FString& SourcePath,
        const FString& DestinationPath,
        bool bOverwrite)
    {
        PinWrightAssetImportHandler::FRequest Request;
        Request.SourcePath = SourcePath;
        Request.SourceName = FPaths::GetBaseFilename(SourcePath);
        Request.DestinationPath = FPaths::GetPath(DestinationPath);
        Request.DestinationName = FPaths::GetBaseFilename(DestinationPath);
        if (FPaths::GetExtension(DestinationPath).IsEmpty())
        {
            Request.DestinationPath = DestinationPath;
            Request.DestinationName = Request.SourceName;
        }
        Request.RequestedAssetPath = Request.DestinationPath
            / Request.DestinationName + TEXT(".") + Request.DestinationName;
        Request.bOverwrite = bOverwrite;
        return Request;
    }

    struct FExecutionCounts
    {
        int32 ImportExecutions = 0;
        int32 ReimportExecutions = 0;
        int32 ReimportFactoryCreations = 0;
        int32 PreAdapterHooks = 0;
        int32 PostconditionVerifications = 0;
    };

    AssetImportPolicy::FExecutionDependencies MakeCountingDependencies(
        const TSharedRef<FExecutionCounts>& Counts)
    {
        AssetImportPolicy::FExecutionDependencies Dependencies;
        Dependencies.ImportExecutionProbe = [Counts]()
        {
            ++Counts->ImportExecutions;
        };
        Dependencies.ReimportExecutionProbe = [Counts]()
        {
            ++Counts->ReimportExecutions;
        };
        Dependencies.ReimportFactoryProbe = [Counts]()
        {
            ++Counts->ReimportFactoryCreations;
        };
        Dependencies.BeforeReimportAdapter =
            [Counts](
                UTexture2D*,
                const AssetImportPolicy::FValidatedTextureReimportPlan&)
            {
                ++Counts->PreAdapterHooks;
                return true;
            };
        return Dependencies;
    }

    inline constexpr int32 MaxStageSnapshotEntries = 1024;
    const FString StageRoot(TEXT("/Game/__PinWrightAssetImportStage"));

    FCriticalSection& GetStageSnapshotMutex()
    {
        static FCriticalSection Mutex;
        return Mutex;
    }

    struct FStageSnapshot
    {
        TSet<FString> Entries;
        bool bComplete = false;
    };

    bool CaptureStageSnapshot(FStageSnapshot& OutSnapshot)
    {
        OutSnapshot = FStageSnapshot();
        int32 EntriesExamined = 0;
        bool bOverflow = false;
        const auto AddEntry =
            [&OutSnapshot, &EntriesExamined, &bOverflow](FString Entry)
            {
                if (++EntriesExamined > MaxStageSnapshotEntries)
                {
                    bOverflow = true;
                    return false;
                }
                OutSnapshot.Entries.Add(MoveTemp(Entry));
                return true;
            };

        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*StageRoot));
        Filter.bRecursivePaths = true;
        const bool bRegistryValid =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
                TEXT("AssetRegistry")).Get().EnumerateAssets(
                    Filter,
                    [&AddEntry](const FAssetData& Asset)
                    {
                        return AddEntry(
                            TEXT("registry:") + Asset.GetObjectPathString());
                    });
        if (!bRegistryValid || bOverflow)
        {
            return false;
        }

        FString StageDirectory;
        if (!FPackageName::TryConvertLongPackageNameToFilename(
                StageRoot, StageDirectory))
        {
            return false;
        }
        if (IFileManager::Get().DirectoryExists(*StageDirectory))
        {
            const bool bDiskComplete =
                IFileManager::Get().IterateDirectoryRecursively(
                    *StageDirectory,
                    [&AddEntry](
                        const TCHAR* FilenameOrDirectory,
                        bool /*bIsDirectory*/)
                    {
                        FString Normalized(FilenameOrDirectory);
                        FPaths::MakeStandardFilename(Normalized);
                        return AddEntry(TEXT("disk:") + Normalized);
                    });
            if (!bDiskComplete || bOverflow)
            {
                return false;
            }
        }
        OutSnapshot.bComplete = true;
        return true;
    }

    bool StageSnapshotsMatch(
        const FStageSnapshot& A, const FStageSnapshot& B)
    {
        if (!A.bComplete || !B.bComplete
            || A.Entries.Num() != B.Entries.Num())
        {
            return false;
        }
        for (const FString& Entry : A.Entries)
        {
            if (!B.Entries.Contains(Entry))
            {
                return false;
            }
        }
        return true;
    }

    bool StageSnapshotContainsGuid(
        const FStageSnapshot& Snapshot, const FString& Guid)
    {
        for (const FString& Entry : Snapshot.Entries)
        {
            if (Entry.Contains(Guid, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    class FScopedStageAbsenceCheck final
    {
    public:
        FScopedStageAbsenceCheck(
            FAutomationTestBase& InTest, const FString& InGuid)
            : Test(InTest)
            , Guid(InGuid)
            , Lock(&GetStageSnapshotMutex())
        {
            Test.TestTrue(
                TEXT("bounded stage baseline snapshot completed"),
                CaptureStageSnapshot(Before));
            Test.TestFalse(
                TEXT("stage baseline contains no fixture GUID"),
                StageSnapshotContainsGuid(Before, Guid));
        }

        ~FScopedStageAbsenceCheck()
        {
            FStageSnapshot After;
            Test.TestTrue(
                TEXT("bounded stage final snapshot completed"),
                CaptureStageSnapshot(After));
            Test.TestTrue(
                TEXT("preexisting stage entries are unchanged"),
                StageSnapshotsMatch(Before, After));
            Test.TestFalse(
                TEXT("stage final snapshot contains no fixture GUID"),
                StageSnapshotContainsGuid(After, Guid));
        }

        FScopedStageAbsenceCheck(const FScopedStageAbsenceCheck&) = delete;
        FScopedStageAbsenceCheck& operator=(
            const FScopedStageAbsenceCheck&) = delete;

    private:
        FAutomationTestBase& Test;
        FString Guid;
        FScopeLock Lock;
        FStageSnapshot Before;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAssetImportSavedLoadedOverwritePreservesLiveReferencesTest,
    "PinWright.asset.import.SavedLoadedOverwritePreservesLiveReferences",
    EAutomationTestFlags::EditorContext
        | EAutomationTestFlags::ProductFilter)

bool FAssetImportSavedLoadedOverwritePreservesLiveReferencesTest::RunTest(
    const FString& /*Parameters*/)
{
    using namespace AssetImportPolicyTests;

    const FString Guid = NewGuid();
    FScopedStageAbsenceCheck StageCheck(*this, Guid);
    FTextureFixture Fixture = CreateSavedTextureFixture(*this, Guid);
    ON_SCOPE_EXIT
    {
        CleanupTextureFixture(*this, Fixture);
    };
    FScopedEditorWorldActorGuard WorldGuard;
    if (!Fixture.Texture)
    {
        return false;
    }

    APinWrightAssetImportReferenceActor* ReferenceActor =
        SpawnReferenceActor(Fixture.Texture);
    if (!TestNotNull(
            TEXT("transient reflected reference actor spawned"),
            ReferenceActor))
    {
        return false;
    }

    UObject* const OriginalIdentity = Fixture.Texture;
    UClass* const OriginalClass = Fixture.Texture->GetClass();
    UPackage* const OriginalPackage = Fixture.Texture->GetOutermost();
    const FString OriginalPath = Fixture.Texture->GetPathName();
    const FString OriginalSoftPath =
        ReferenceActor->SoftReference.ToSoftObjectPath().ToString();
    UObject* const OriginalRegistryObject =
        ResolveRegistryObject(OriginalPath);
    const FGuid OriginalContentId = Fixture.Texture->Source.GetId();
    TArray64<uint8> OriginalPixels;
    TestTrue(TEXT("initial texture source pixels are readable"),
        Fixture.Texture->Source.GetMipData(OriginalPixels, 0));
    TestFalse(TEXT("saved fixture starts clean"), OriginalPackage->IsDirty());
    TestTrue(
        TEXT("replacement PNG source written"),
        WritePng(Fixture.SourcePath, FColor::Blue));

    const TSharedRef<FTestResponseCapture> Capture = InvokeImport(
        *this, Fixture.SourcePath, Fixture.ObjectPath,
        /*bIncludeOverwrite=*/true, /*bOverwrite=*/true);
    TestTrue(TEXT("supported overwrite succeeds"), Capture->bSuccess);
    TestTrue(TEXT("success has no error code"), Capture->ErrorCode.IsEmpty());
    if (!Capture->bSuccess || !Capture->Result)
    {
        return false;
    }

    TestTrue(TEXT("returned object keeps original identity"),
        StaticFindObject(
            UObject::StaticClass(), nullptr, *OriginalPath)
            == OriginalIdentity);
    TestTrue(TEXT("Asset Registry keeps original identity"),
        ResolveRegistryObject(OriginalPath) == OriginalRegistryObject
            && OriginalRegistryObject == OriginalIdentity);
    TestTrue(TEXT("hard reflected reference stays valid"),
        ReferenceActor->HardReference == OriginalIdentity);
    TestEqual(TEXT("soft reference path stays canonical"),
        ReferenceActor->SoftReference.ToSoftObjectPath().ToString(),
        OriginalSoftPath);
    TestTrue(TEXT("soft reference resolves to original identity"),
        ReferenceActor->SoftReference.Get() == OriginalIdentity);
    TestTrue(TEXT("class stays unchanged"),
        Fixture.Texture->GetClass() == OriginalClass);
    TestEqual(TEXT("object path stays unchanged"),
        Fixture.Texture->GetPathName(), OriginalPath);
    TestTrue(TEXT("package stays unchanged"),
        Fixture.Texture->GetOutermost() == OriginalPackage);
    TestTrue(TEXT("disk bytes size and timestamp stay unchanged"),
        ResourcesMatchSavedSnapshot(Fixture));
    TestTrue(TEXT("in-place update is pending save"),
        OriginalPackage->IsDirty());
    const FGuid UpdatedContentId = Fixture.Texture->Source.GetId();
    TArray64<uint8> UpdatedPixels;
    TestTrue(TEXT("updated texture source pixels are readable"),
        Fixture.Texture->Source.GetMipData(UpdatedPixels, 0));
    TestTrue(TEXT("production texture source identity changed"),
        OriginalContentId.IsValid() && UpdatedContentId.IsValid()
            && OriginalContentId != UpdatedContentId);
    TestTrue(TEXT("production texture pixel readback changed"),
        !OriginalPixels.IsEmpty() && !UpdatedPixels.IsEmpty()
            && OriginalPixels != UpdatedPixels);

    FString Mode;
    bool bIdentityPreserved = false;
    bool bUpdatedInPlace = false;
    bool bPendingSave = false;
    bool bReplaced = true;
    bool bContentChangeMeasured = false;
    bool bTextureContentChanged = false;
    Capture->Result->TryGetStringField(TEXT("mode"), Mode);
    Capture->Result->TryGetBoolField(
        TEXT("identityPreserved"), bIdentityPreserved);
    Capture->Result->TryGetBoolField(
        TEXT("updatedInPlace"), bUpdatedInPlace);
    Capture->Result->TryGetBoolField(
        TEXT("pendingSave"), bPendingSave);
    Capture->Result->TryGetBoolField(TEXT("replaced"), bReplaced);
    Capture->Result->TryGetBoolField(
        TEXT("contentChangeMeasured"), bContentChangeMeasured);
    Capture->Result->TryGetBoolField(
        TEXT("textureContentChanged"), bTextureContentChanged);
    TestEqual(TEXT("response names in-place reimport mode"),
        Mode, FString(TEXT("inPlaceReimport")));
    TestTrue(TEXT("response measures preserved identity"),
        bIdentityPreserved);
    TestTrue(TEXT("response measures update in place"),
        bUpdatedInPlace);
    TestTrue(TEXT("response reports pending save"), bPendingSave);
    TestFalse(TEXT("response does not invent replacement identity"),
        bReplaced);
    TestTrue(TEXT("response measured texture content"),
        bContentChangeMeasured);
    TestTrue(TEXT("response reports changed texture content"),
        bTextureContentChanged);
    TestFalse(TEXT("no staging report fields are emitted"),
        HasLegacyStageFields(Capture->Result));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAssetImportOverwriteFalseRefusesSavedLoadedAssetTest,
    "PinWright.asset.import.OverwriteFalseRefusesSavedLoadedAsset",
    EAutomationTestFlags::EditorContext
        | EAutomationTestFlags::ProductFilter)

bool FAssetImportOverwriteFalseRefusesSavedLoadedAssetTest::RunTest(
    const FString& /*Parameters*/)
{
    using namespace AssetImportPolicyTests;

    const FString Guid = NewGuid();
    FScopedStageAbsenceCheck StageCheck(*this, Guid);
    FTextureFixture Fixture = CreateSavedTextureFixture(*this, Guid);
    ON_SCOPE_EXIT
    {
        CleanupTextureFixture(*this, Fixture);
    };
    FScopedEditorWorldActorGuard WorldGuard;
    if (!Fixture.Texture)
    {
        return false;
    }
    APinWrightAssetImportReferenceActor* ReferenceActor =
        SpawnReferenceActor(Fixture.Texture);
    if (!TestNotNull(TEXT("reference actor spawned"), ReferenceActor))
    {
        return false;
    }

    UObject* const OriginalIdentity = Fixture.Texture;
    UObject* const OriginalRegistry =
        ResolveRegistryObject(Fixture.ObjectPath);
    const FString OriginalSoftPath =
        ReferenceActor->SoftReference.ToSoftObjectPath().ToString();
    const bool bOriginalDirty = Fixture.Texture->GetOutermost()->IsDirty();
    const TSharedRef<FExecutionCounts> Counts = MakeShared<FExecutionCounts>();
    AssetImportPolicy::FExecutionDependencies Dependencies =
        MakeCountingDependencies(Counts);
    const PinWrightAssetImportHandler::FResult Result =
        PinWrightAssetImportHandler::Execute(
            MakeHandlerRequest(
                Fixture.SourcePath, Fixture.ObjectPath,
                /*bOverwrite=*/false),
            &Dependencies);

    TestFalse(TEXT("occupied destination is refused"), Result.bSuccess);
    TestEqual(TEXT("refusal uses established exists code"),
        Result.ErrorCode,
        FString(ErrorCodes::ERR_ASSET_ALREADY_EXISTS));
    TestEqual(TEXT("refusal runs no general import"),
        Counts->ImportExecutions, 0);
    TestEqual(TEXT("refusal runs no reimport"),
        Counts->ReimportExecutions, 0);
    TestEqual(TEXT("refusal creates no reimport factory"),
        Counts->ReimportFactoryCreations, 0);
    TestEqual(TEXT("refusal reaches no pre-adapter hook"),
        Counts->PreAdapterHooks, 0);
    TestTrue(TEXT("identity stays unchanged"),
        StaticFindObject(
            UObject::StaticClass(), nullptr, *Fixture.ObjectPath)
            == OriginalIdentity);
    TestTrue(TEXT("registry identity stays unchanged"),
        ResolveRegistryObject(Fixture.ObjectPath) == OriginalRegistry);
    TestTrue(TEXT("hard reference stays unchanged"),
        ReferenceActor->HardReference == OriginalIdentity);
    TestEqual(TEXT("soft path stays unchanged"),
        ReferenceActor->SoftReference.ToSoftObjectPath().ToString(),
        OriginalSoftPath);
    TestTrue(TEXT("soft reference still resolves"),
        ReferenceActor->SoftReference.Get() == OriginalIdentity);
    TestEqual(TEXT("dirty state stays unchanged"),
        Fixture.Texture->GetOutermost()->IsDirty(), bOriginalDirty);
    TestTrue(TEXT("disk resources stay unchanged"),
        ResourcesMatchSavedSnapshot(Fixture));
    TestFalse(TEXT("refusal has no stage fields"),
        HasLegacyStageFields(Result.Data));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAssetImportPostStageFailureRestoresSavedLoadedAssetTest,
    "PinWright.asset.import.PostStageFailureRestoresSavedLoadedAsset",
    EAutomationTestFlags::EditorContext
        | EAutomationTestFlags::ProductFilter)

bool FAssetImportPostStageFailureRestoresSavedLoadedAssetTest::RunTest(
    const FString& /*Parameters*/)
{
    using namespace AssetImportPolicyTests;

    const FString Guid = NewGuid();
    FScopedStageAbsenceCheck StageCheck(*this, Guid);
    FTextureFixture Fixture = CreateSavedTextureFixture(*this, Guid);
    ON_SCOPE_EXIT
    {
        CleanupTextureFixture(*this, Fixture);
    };
    FScopedEditorWorldActorGuard WorldGuard;
    if (!Fixture.Texture)
    {
        return false;
    }
    APinWrightAssetImportReferenceActor* ReferenceActor =
        SpawnReferenceActor(Fixture.Texture);
    if (!TestNotNull(TEXT("reference actor spawned"), ReferenceActor))
    {
        return false;
    }

    UObject* const OriginalIdentity = Fixture.Texture;
    UObject* const OriginalRegistry =
        ResolveRegistryObject(Fixture.ObjectPath);
    const FString OriginalPath = Fixture.Texture->GetPathName();
    const FString OriginalSoftPath =
        ReferenceActor->SoftReference.ToSoftObjectPath().ToString();
    const bool bOriginalDirty = Fixture.Texture->GetOutermost()->IsDirty();
    const FAssetImportInfo OriginalSourceMetadata =
        Fixture.Texture->AssetImportData->GetSourceData();
    const FGuid OriginalContentId = Fixture.Texture->Source.GetId();
    const TSharedRef<FExecutionCounts> Counts = MakeShared<FExecutionCounts>();
    AssetImportPolicy::FExecutionDependencies Dependencies =
        MakeCountingDependencies(Counts);
    Dependencies.BeforeReimportAdapter =
        [Counts](
            UTexture2D* Texture,
            const AssetImportPolicy::FValidatedTextureReimportPlan& Plan)
        {
            ++Counts->PreAdapterHooks;
            Texture->AssetImportData->UpdateFilenameOnly(
                Plan.GetSourcePath() + TEXT(".injected-failure"));
            Texture->GetOutermost()->SetDirtyFlag(true);
            return false;
        };
    const PinWrightAssetImportHandler::FResult Result =
        PinWrightAssetImportHandler::Execute(
            MakeHandlerRequest(
                Fixture.SourcePath, Fixture.ObjectPath,
                /*bOverwrite=*/true),
            &Dependencies);

    TestFalse(TEXT("injected reimport failure is returned"),
        Result.bSuccess);
    TestEqual(TEXT("pre-adapter failure uses import failed code"),
        Result.ErrorCode, FString(ErrorCodes::ERR_IMPORT_FAILED));
    TestEqual(TEXT("failure runs no general import"),
        Counts->ImportExecutions, 0);
    TestEqual(TEXT("failure runs no reimport adapter"),
        Counts->ReimportExecutions, 0);
    TestEqual(TEXT("failure creates no reimport factory"),
        Counts->ReimportFactoryCreations, 0);
    TestEqual(TEXT("pre-adapter hook is called exactly once"),
        Counts->PreAdapterHooks, 1);
    TestTrue(TEXT("exact source metadata is restored"),
        Fixture.Texture->AssetImportData->GetSourceData().ToJson()
            == OriginalSourceMetadata.ToJson());
    TestEqual(TEXT("texture source content stays unchanged"),
        Fixture.Texture->Source.GetId(), OriginalContentId);
    TestTrue(TEXT("original never left its path"),
        StaticFindObject(
            UObject::StaticClass(), nullptr, *OriginalPath)
            == OriginalIdentity);
    TestTrue(TEXT("registry identity stays unchanged"),
        ResolveRegistryObject(OriginalPath) == OriginalRegistry);
    TestTrue(TEXT("hard reference stays unchanged"),
        ReferenceActor->HardReference == OriginalIdentity);
    TestEqual(TEXT("soft path stays unchanged"),
        ReferenceActor->SoftReference.ToSoftObjectPath().ToString(),
        OriginalSoftPath);
    TestTrue(TEXT("soft reference still resolves"),
        ReferenceActor->SoftReference.Get() == OriginalIdentity);
    TestEqual(TEXT("dirty state stays unchanged"),
        Fixture.Texture->GetOutermost()->IsDirty(), bOriginalDirty);
    TestTrue(TEXT("disk resources stay unchanged"),
        ResourcesMatchSavedSnapshot(Fixture));

    if (!TestTrue(TEXT("failure response data exists"),
            Result.Data.IsValid()))
    {
        return false;
    }

    bool bFailureMayHaveMutatedAsset = true;
    bool bInMemoryContentRestored = true;
    bool bSourceMetadataRestored = false;
    bool bPackageDirtyStateRestored = false;
    bool bAdapterInvoked = true;
    bool bFailureOccurredBeforeContentMutation = false;
    bool bPackageLeftDirtyForPossibleMutation = true;
    Result.Data->TryGetBoolField(
        TEXT("failureMayHaveMutatedAsset"),
        bFailureMayHaveMutatedAsset);
    Result.Data->TryGetBoolField(
        TEXT("inMemoryContentRestored"),
        bInMemoryContentRestored);
    Result.Data->TryGetBoolField(
        TEXT("sourceMetadataRestored"), bSourceMetadataRestored);
    Result.Data->TryGetBoolField(
        TEXT("packageDirtyStateRestored"), bPackageDirtyStateRestored);
    Result.Data->TryGetBoolField(
        TEXT("adapterInvoked"), bAdapterInvoked);
    Result.Data->TryGetBoolField(
        TEXT("failureOccurredBeforeContentMutation"),
        bFailureOccurredBeforeContentMutation);
    Result.Data->TryGetBoolField(
        TEXT("packageLeftDirtyForPossibleMutation"),
        bPackageLeftDirtyForPossibleMutation);
    TestFalse(TEXT("failure response reports no possible content mutation"),
        bFailureMayHaveMutatedAsset);
    TestFalse(TEXT("failure response does not claim restored content"),
        bInMemoryContentRestored);
    TestTrue(TEXT("failure response reports metadata restoration"),
        bSourceMetadataRestored);
    TestTrue(TEXT("failure response reports dirty-state restoration"),
        bPackageDirtyStateRestored);
    TestFalse(TEXT("failure response reports no adapter invocation"),
        bAdapterInvoked);
    TestTrue(TEXT("failure is classified before content mutation"),
        bFailureOccurredBeforeContentMutation);
    TestFalse(TEXT("failure leaves no possible mutation dirty state"),
        bPackageLeftDirtyForPossibleMutation);
    TestFalse(TEXT("failure response has no stage fields"),
        HasLegacyStageFields(Result.Data));

    TArray<FString> Packages;
    FString DiscoveryError;
    TestTrue(TEXT("post-failure destination discovery succeeds"),
        AssetImportPolicy::DiscoverDestinationPackages(
            Fixture.AssetRoot, Fixture.ObjectPath,
            TArray<FString>{TEXT("PW_ImportedTexture")},
            Packages, DiscoveryError));
    TestEqual(TEXT("no collateral output survives"), Packages.Num(), 1);
    if (!Packages.IsEmpty())
    {
        TestEqual(TEXT("only original package remains"),
            Packages[0], Fixture.PackageName);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAssetImportPostflightFailureRestoresSavedLoadedAssetTest,
    "PinWright.asset.import.PostflightFailureRestoresSavedLoadedAsset",
    EAutomationTestFlags::EditorContext
        | EAutomationTestFlags::ProductFilter)

bool FAssetImportPostflightFailureRestoresSavedLoadedAssetTest::RunTest(
    const FString& /*Parameters*/)
{
    using namespace AssetImportPolicyTests;

    const FString Guid = NewGuid();
    FScopedStageAbsenceCheck StageCheck(*this, Guid);
    FTextureFixture Fixture = CreateSavedTextureFixture(*this, Guid);
    ON_SCOPE_EXIT
    {
        CleanupTextureFixture(*this, Fixture);
    };
    FScopedEditorWorldActorGuard WorldGuard;
    if (!Fixture.Texture)
    {
        return false;
    }
    APinWrightAssetImportReferenceActor* ReferenceActor =
        SpawnReferenceActor(Fixture.Texture);
    if (!TestNotNull(TEXT("reference actor spawned"), ReferenceActor))
    {
        return false;
    }

    UObject* const OriginalIdentity = Fixture.Texture;
    UClass* const OriginalClass = Fixture.Texture->GetClass();
    UPackage* const OriginalPackage = Fixture.Texture->GetOutermost();
    UObject* const OriginalRegistry =
        ResolveRegistryObject(Fixture.ObjectPath);
    const FString OriginalPath = Fixture.Texture->GetPathName();
    const FString OriginalSoftPath =
        ReferenceActor->SoftReference.ToSoftObjectPath().ToString();
    const FAssetImportInfo OriginalSourceMetadata =
        Fixture.Texture->AssetImportData->GetSourceData();
    const FGuid OriginalContentId = Fixture.Texture->Source.GetId();
    TestFalse(TEXT("saved fixture starts clean"), OriginalPackage->IsDirty());
    TestTrue(TEXT("replacement PNG source written"),
        WritePng(Fixture.SourcePath, FColor::Blue));

    const FString InjectedFailure =
        TEXT("injected failure after successful reimport postflight");
    const TSharedRef<FExecutionCounts> Counts = MakeShared<FExecutionCounts>();
    AssetImportPolicy::FExecutionDependencies Dependencies =
        MakeCountingDependencies(Counts);
    Dependencies.PostconditionVerifier =
        [Counts, InjectedFailure](
            UObject* ExistingObject,
            const FString& RequestedAssetPath,
            const AssetImportPolicy::FDestinationSnapshot& Before,
            FString& OutError)
        {
            ++Counts->PostconditionVerifications;
            FString RealPostflightError;
            if (!AssetImportPolicy::VerifyInPlacePostconditions(
                    ExistingObject, RequestedAssetPath,
                    Before, RealPostflightError))
            {
                OutError = TEXT("real postflight failed before injection: ")
                    + RealPostflightError;
                return false;
            }
            OutError = InjectedFailure;
            return false;
        };
    const PinWrightAssetImportHandler::FResult Result =
        PinWrightAssetImportHandler::Execute(
            MakeHandlerRequest(
                Fixture.SourcePath, Fixture.ObjectPath,
                /*bOverwrite=*/true),
            &Dependencies);

    TestFalse(TEXT("injected postflight failure is returned"),
        Result.bSuccess);
    TestEqual(TEXT("postflight failure uses import failed code"),
        Result.ErrorCode, FString(ErrorCodes::ERR_IMPORT_FAILED));
    TestEqual(TEXT("late failure runs no general import"),
        Counts->ImportExecutions, 0);
    TestEqual(TEXT("real reimport executes exactly once"),
        Counts->ReimportExecutions, 1);
    TestEqual(TEXT("pinned reimport factory is created exactly once"),
        Counts->ReimportFactoryCreations, 1);
    TestEqual(TEXT("pre-adapter pass hook is reached exactly once"),
        Counts->PreAdapterHooks, 1);
    TestEqual(TEXT("postcondition verifier is reached exactly once"),
        Counts->PostconditionVerifications, 1);
    TestTrue(TEXT("exact source metadata is restored after postflight"),
        Fixture.Texture->AssetImportData->GetSourceData().ToJson()
            == OriginalSourceMetadata.ToJson());
    TestTrue(TEXT("real adapter changed texture source content"),
        OriginalContentId.IsValid()
            && Fixture.Texture->Source.GetId().IsValid()
            && Fixture.Texture->Source.GetId() != OriginalContentId);
    TestTrue(TEXT("original pointer remains at exact path"),
        StaticFindObject(
            UObject::StaticClass(), nullptr, *OriginalPath)
            == OriginalIdentity);
    TestTrue(TEXT("original class remains exact"),
        Fixture.Texture->GetClass() == OriginalClass);
    TestTrue(TEXT("original package remains exact"),
        Fixture.Texture->GetOutermost() == OriginalPackage);
    TestEqual(TEXT("original object path remains exact"),
        Fixture.Texture->GetPathName(), OriginalPath);
    TestTrue(TEXT("registry identity remains exact"),
        ResolveRegistryObject(OriginalPath) == OriginalRegistry
            && OriginalRegistry == OriginalIdentity);
    TestTrue(TEXT("hard reference remains valid"),
        ReferenceActor->HardReference == OriginalIdentity);
    TestEqual(TEXT("soft reference path remains exact"),
        ReferenceActor->SoftReference.ToSoftObjectPath().ToString(),
        OriginalSoftPath);
    TestTrue(TEXT("soft reference resolves to original identity"),
        ReferenceActor->SoftReference.Get() == OriginalIdentity);
    TestTrue(TEXT("possible content mutation remains dirty"),
        OriginalPackage->IsDirty());
    TestTrue(TEXT("saved disk resources remain unchanged"),
        ResourcesMatchSavedSnapshot(Fixture));

    if (!TestTrue(TEXT("postflight failure response data exists"),
            Result.Data.IsValid()))
    {
        return false;
    }
    FString FailureReason;
    bool bSourceMetadataRestored = false;
    bool bPackageDirtyStateRestored = true;
    bool bAdapterInvoked = false;
    bool bFailureOccurredBeforeContentMutation = true;
    bool bFailureMayHaveMutatedContent = false;
    bool bPackageLeftDirtyForPossibleMutation = false;
    bool bInMemoryContentRestored = true;
    bool bTextureContentChanged = false;
    Result.Data->TryGetStringField(TEXT("failureReason"), FailureReason);
    Result.Data->TryGetBoolField(
        TEXT("sourceMetadataRestored"), bSourceMetadataRestored);
    Result.Data->TryGetBoolField(
        TEXT("packageDirtyStateRestored"), bPackageDirtyStateRestored);
    Result.Data->TryGetBoolField(TEXT("adapterInvoked"), bAdapterInvoked);
    Result.Data->TryGetBoolField(
        TEXT("failureOccurredBeforeContentMutation"),
        bFailureOccurredBeforeContentMutation);
    Result.Data->TryGetBoolField(
        TEXT("failureMayHaveMutatedContent"),
        bFailureMayHaveMutatedContent);
    Result.Data->TryGetBoolField(
        TEXT("packageLeftDirtyForPossibleMutation"),
        bPackageLeftDirtyForPossibleMutation);
    Result.Data->TryGetBoolField(
        TEXT("inMemoryContentRestored"), bInMemoryContentRestored);
    Result.Data->TryGetBoolField(
        TEXT("textureContentChanged"), bTextureContentChanged);
    TestEqual(TEXT("response preserves injected failure reason"),
        FailureReason, InjectedFailure);
    TestTrue(TEXT("response measures source metadata restoration"),
        bSourceMetadataRestored);
    TestFalse(TEXT("response does not claim clean-state restoration"),
        bPackageDirtyStateRestored);
    TestTrue(TEXT("response proves adapter invocation"), bAdapterInvoked);
    TestFalse(TEXT("failure is not classified before content mutation"),
        bFailureOccurredBeforeContentMutation);
    TestTrue(TEXT("response reports possible content mutation"),
        bFailureMayHaveMutatedContent);
    TestTrue(TEXT("response reports dirty package protection"),
        bPackageLeftDirtyForPossibleMutation);
    TestFalse(TEXT("response does not claim content rollback"),
        bInMemoryContentRestored);
    TestTrue(TEXT("response reports retained content change"),
        bTextureContentChanged);
    TestFalse(TEXT("postflight failure has no stage fields"),
        HasLegacyStageFields(Result.Data));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAssetImportProductionMultiOutputReportsEveryOutputTest,
    "PinWright.asset.import.ProductionMultiOutputReportsEveryOutput",
    EAutomationTestFlags::EditorContext
        | EAutomationTestFlags::ProductFilter)

bool FAssetImportProductionMultiOutputReportsEveryOutputTest::RunTest(
    const FString& /*Parameters*/)
{
    using namespace AssetImportPolicyTests;

    const FString Guid = NewGuid();
    FScopedStageAbsenceCheck StageCheck(*this, Guid);
    const FString AssetRoot =
        TEXT("/Game/PinWrightTests/") + Guid;
    const FString SourceDirectory = FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("PinWrightTests"), Guid);
    const FString SourcePath = FPaths::Combine(
        SourceDirectory, TEXT("FactorySource.pwmulti"));
    const FString RequestedName = TEXT("RequestedAsset");
    const FString RequestedPackage = AssetRoot / RequestedName;
    const FString RequestedObjectPath = RequestedPackage
        + TEXT(".") + RequestedName;
    const FString CollateralPackage =
        AssetRoot / TEXT("FactorySource_Collateral");
    const FString CollateralObjectPath = CollateralPackage
        + TEXT(".FactorySource_Collateral");
    const FString SecondRequestedName = TEXT("RequestedAsset2");
    const FString SecondRequestedPackage =
        AssetRoot / SecondRequestedName;
    const FString SecondRequestedObjectPath = SecondRequestedPackage
        + TEXT(".") + SecondRequestedName;
    const FString ExactCollisionName = TEXT("PW_ExactNonTexture");
    const FString ExactCollisionPackage =
        AssetRoot / ExactCollisionName;
    const FString ExactCollisionObjectPath = ExactCollisionPackage
        + TEXT(".") + ExactCollisionName;
    const FString ExactCollisionSourcePath = FPaths::Combine(
        SourceDirectory, ExactCollisionName + TEXT(".png"));
    ON_SCOPE_EXIT
    {
        DeleteRootUnforced(
            *this, AssetRoot,
            TArray<FString>{
                RequestedPackage,
                CollateralPackage,
                SecondRequestedPackage,
                ExactCollisionPackage});
        const bool bSourceDeleted = IFileManager::Get().Delete(
            *SourcePath, /*RequireExists=*/false,
            /*EvenReadOnly=*/true);
        TestTrue(TEXT("multi-output source file removed"),
            bSourceDeleted || !IFileManager::Get().FileExists(*SourcePath));
        const bool bDirectoryDeleted = IFileManager::Get().DeleteDirectory(
            *SourceDirectory, /*RequireExists=*/false,
            /*Tree=*/true);
        TestTrue(TEXT("multi-output source directory removed"),
            bDirectoryDeleted
                || !IFileManager::Get().DirectoryExists(*SourceDirectory));
    };
    FScopedEditorWorldActorGuard WorldGuard;

    TestTrue(TEXT("factory source directory created"),
        IFileManager::Get().MakeDirectory(
            *SourceDirectory, /*Tree=*/true));
    TestTrue(TEXT("factory source file written"),
        FFileHelper::SaveStringToFile(TEXT("pinwright"), *SourcePath));

    TStrongObjectPtr<UPinWrightAssetImportTestFactory> ImportFactory(
        NewObject<UPinWrightAssetImportTestFactory>());
    if (!TestNotNull(TEXT("test import factory created"), ImportFactory.Get()))
    {
        return false;
    }
    const TSharedRef<FExecutionCounts> ImportCounts =
        MakeShared<FExecutionCounts>();
    AssetImportPolicy::FExecutionDependencies ImportDependencies =
        MakeCountingDependencies(ImportCounts);
    ImportDependencies.ImportFactory = ImportFactory.Get();
    const PinWrightAssetImportHandler::FResult Result =
        PinWrightAssetImportHandler::Execute(
            MakeHandlerRequest(
                SourcePath, RequestedObjectPath,
                /*bOverwrite=*/false),
            &ImportDependencies);

    TestTrue(TEXT("production AssetTools import succeeds"),
        Result.bSuccess);
    TestEqual(TEXT("general import executes exactly once"),
        ImportCounts->ImportExecutions, 1);
    TestEqual(TEXT("general import runs no reimport"),
        ImportCounts->ReimportExecutions, 0);
    TestEqual(TEXT("general import creates no reimport factory"),
        ImportCounts->ReimportFactoryCreations, 0);
    TestEqual(TEXT("general import reaches no pre-adapter hook"),
        ImportCounts->PreAdapterHooks, 0);
    TestEqual(TEXT("test factory executes exactly once"),
        ImportFactory->CreateCallCount, 1);
    if (!Result.bSuccess || !Result.Data)
    {
        return false;
    }

    double ResultCount = 0.0;
    Result.Data->TryGetNumberField(TEXT("resultCount"), ResultCount);
    TestEqual(TEXT("every non-null factory output is reported"),
        ResultCount, 2.0);
    TSharedPtr<FJsonObject> Primary = GetResultEntry(Result.Data, 0);
    TSharedPtr<FJsonObject> Collateral = GetResultEntry(Result.Data, 1);
    if (!TestTrue(TEXT("primary result row exists"), Primary.IsValid())
        || !TestTrue(TEXT("collateral result row exists"),
            Collateral.IsValid()))
    {
        return false;
    }

    FString PrimaryPath;
    FString SecondaryPath;
    bool bRenameRequested = false;
    bool bRenameSucceeded = false;
    bool bPrimaryPendingSave = false;
    bool bSecondaryPendingSave = false;
    bool bPrimaryReplaced = true;
    bool bSecondaryReplaced = true;
    bool bPrimaryValid = false;
    bool bSecondaryValid = false;
    bool bPrimaryResolved = false;
    bool bSecondaryResolved = false;
    bool bPrimaryExistedBefore = true;
    bool bSecondaryExistedBefore = true;
    Primary->TryGetStringField(TEXT("assetPath"), PrimaryPath);
    Collateral->TryGetStringField(TEXT("assetPath"), SecondaryPath);
    Primary->TryGetBoolField(
        TEXT("renameRequested"), bRenameRequested);
    Primary->TryGetBoolField(
        TEXT("renameSucceeded"), bRenameSucceeded);
    Primary->TryGetBoolField(
        TEXT("pendingSave"), bPrimaryPendingSave);
    Collateral->TryGetBoolField(
        TEXT("pendingSave"), bSecondaryPendingSave);
    Primary->TryGetBoolField(TEXT("replaced"), bPrimaryReplaced);
    Collateral->TryGetBoolField(TEXT("replaced"), bSecondaryReplaced);
    Primary->TryGetBoolField(TEXT("valid"), bPrimaryValid);
    Collateral->TryGetBoolField(TEXT("valid"), bSecondaryValid);
    Primary->TryGetBoolField(TEXT("resolved"), bPrimaryResolved);
    Collateral->TryGetBoolField(TEXT("resolved"), bSecondaryResolved);
    Primary->TryGetBoolField(
        TEXT("existedBefore"), bPrimaryExistedBefore);
    Collateral->TryGetBoolField(
        TEXT("existedBefore"), bSecondaryExistedBefore);
    TestEqual(TEXT("primary row reports actual renamed path"),
        PrimaryPath, RequestedObjectPath);
    TestEqual(TEXT("collateral row reports actual factory path"),
        SecondaryPath, CollateralObjectPath);
    TestTrue(TEXT("reported output paths are unique"),
        PrimaryPath != SecondaryPath);
    TestTrue(TEXT("primary row owns rename request"), bRenameRequested);
    TestTrue(TEXT("primary rename succeeds"), bRenameSucceeded);
    TestFalse(TEXT("collateral has no renameRequested metadata"),
        Collateral->HasField(TEXT("renameRequested")));
    TestTrue(TEXT("primary reports pending save"),
        bPrimaryPendingSave);
    TestTrue(TEXT("collateral reports pending save"),
        bSecondaryPendingSave);
    TestFalse(TEXT("new primary is not called a replacement"),
        bPrimaryReplaced);
    TestFalse(TEXT("new collateral is not called a replacement"),
        bSecondaryReplaced);
    TestTrue(TEXT("primary output is valid and resolved"),
        bPrimaryValid && bPrimaryResolved);
    TestTrue(TEXT("collateral output is valid and resolved"),
        bSecondaryValid && bSecondaryResolved);
    TestFalse(TEXT("primary did not exist before import"),
        bPrimaryExistedBefore);
    TestFalse(TEXT("collateral did not exist before import"),
        bSecondaryExistedBefore);
    UObject* const PrimaryObject =
        ResolveRegistryObject(RequestedObjectPath);
    UObject* const CollateralObject =
        ResolveRegistryObject(CollateralObjectPath);
    if (!TestNotNull(TEXT("primary resolves in registry"), PrimaryObject)
        || !TestNotNull(
            TEXT("collateral resolves in registry"), CollateralObject))
    {
        return false;
    }
    TStrongObjectPtr<UObject> RetainedPrimary(PrimaryObject);
    TStrongObjectPtr<UObject> RetainedCollateral(CollateralObject);
    APinWrightAssetImportReferenceActor* ReferenceActor =
        SpawnReferenceActor(nullptr);
    if (!TestNotNull(TEXT("multi-output reference actor spawned"),
            ReferenceActor))
    {
        return false;
    }
    ReferenceActor->HardReferences.Add(PrimaryObject);
    ReferenceActor->HardReferences.Add(CollateralObject);
    ReferenceActor->SoftReferences.Add(TSoftObjectPtr<UObject>(PrimaryObject));
    ReferenceActor->SoftReferences.Add(
        TSoftObjectPtr<UObject>(CollateralObject));
    TestTrue(TEXT("primary output saved to disk"),
        UEditorAssetLibrary::SaveLoadedAsset(
            PrimaryObject, /*bOnlyIfIsDirty=*/false));
    TestTrue(TEXT("collateral output saved to disk"),
        UEditorAssetLibrary::SaveLoadedAsset(
            CollateralObject, /*bOnlyIfIsDirty=*/false));
    FSavedPackageFileSnapshot PrimarySavedFile;
    FSavedPackageFileSnapshot CollateralSavedFile;
    TestTrue(TEXT("primary saved package file captured"),
        CaptureSavedPackageFile(RequestedPackage, PrimarySavedFile));
    TestTrue(TEXT("collateral saved package file captured"),
        CaptureSavedPackageFile(CollateralPackage, CollateralSavedFile));
    TestTrue(TEXT("primary hard reference retains exact output"),
        ReferenceActor->HardReferences[0] == PrimaryObject);
    TestTrue(TEXT("collateral hard reference retains exact output"),
        ReferenceActor->HardReferences[1] == CollateralObject);
    TestTrue(TEXT("primary soft reference resolves exact output"),
        ReferenceActor->SoftReferences[0].Get() == PrimaryObject);
    TestTrue(TEXT("collateral soft reference resolves exact output"),
        ReferenceActor->SoftReferences[1].Get() == CollateralObject);
    TestFalse(TEXT("multi-output response has no stage fields"),
        HasLegacyStageFields(Result.Data));

    const TSharedRef<FExecutionCounts> CollisionCounts =
        MakeShared<FExecutionCounts>();
    AssetImportPolicy::FExecutionDependencies CollisionDependencies =
        MakeCountingDependencies(CollisionCounts);
    CollisionDependencies.ImportFactory = ImportFactory.Get();
    const PinWrightAssetImportHandler::FResult CollisionResult =
        PinWrightAssetImportHandler::Execute(
            MakeHandlerRequest(
                SourcePath, SecondRequestedObjectPath,
                /*bOverwrite=*/true),
            &CollisionDependencies);

    TestFalse(TEXT("occupied collateral is refused"),
        CollisionResult.bSuccess);
    TestEqual(TEXT("occupied collateral uses unsafe code"),
        CollisionResult.ErrorCode,
        FString(ErrorCodes::ERR_OVERWRITE_UNSAFE));
    TestEqual(TEXT("unsafe collision runs no general import"),
        CollisionCounts->ImportExecutions, 0);
    TestEqual(TEXT("unsafe collision runs no reimport"),
        CollisionCounts->ReimportExecutions, 0);
    TestEqual(TEXT("unsafe collision creates no reimport factory"),
        CollisionCounts->ReimportFactoryCreations, 0);
    TestEqual(TEXT("unsafe collision reaches no pre-adapter hook"),
        CollisionCounts->PreAdapterHooks, 0);
    TestEqual(TEXT("unsafe collision does not invoke import factory"),
        ImportFactory->CreateCallCount, 1);
    TestNull(TEXT("unsafe collision creates no requested object"),
        ResolveRegistryObject(SecondRequestedObjectPath));
    TestTrue(TEXT("unsafe diagnostics name collateral conflict"),
        JsonArrayContains(
            CollisionResult.Data, TEXT("conflicts"),
            CollateralPackage));
    TestFalse(TEXT("unsafe collision has no stage fields"),
        HasLegacyStageFields(CollisionResult.Data));

    TestTrue(TEXT("exact-collision PNG source written"),
        WritePng(ExactCollisionSourcePath, FColor::Green));
    UPackage* const ExactCollisionOuter =
        CreatePackage(*ExactCollisionPackage);
    TStrongObjectPtr<UCurveFloat> ExactCollisionAsset(
        ExactCollisionOuter
            ? NewObject<UCurveFloat>(
                ExactCollisionOuter, FName(*ExactCollisionName),
                RF_Public | RF_Standalone | RF_Transactional)
            : nullptr);
    if (!TestNotNull(TEXT("exact non-texture asset created"),
            ExactCollisionAsset.Get()))
    {
        return false;
    }
    ExactCollisionAsset->FloatCurve.AddKey(0.0f, 3.0f);
    ExactCollisionOuter->SetDirtyFlag(true);
    FAssetRegistryModule::AssetCreated(ExactCollisionAsset.Get());
    TestTrue(TEXT("exact non-texture asset saved to disk"),
        UEditorAssetLibrary::SaveLoadedAsset(
            ExactCollisionAsset.Get(), /*bOnlyIfIsDirty=*/false));
    FSavedPackageFileSnapshot ExactCollisionSavedFile;
    TestTrue(TEXT("exact non-texture saved file captured"),
        CaptureSavedPackageFile(
            ExactCollisionPackage, ExactCollisionSavedFile));
    ReferenceActor->HardReferences.Add(ExactCollisionAsset.Get());
    ReferenceActor->SoftReferences.Add(
        TSoftObjectPtr<UObject>(ExactCollisionAsset.Get()));

    const TSharedRef<FExecutionCounts> ExactCollisionCounts =
        MakeShared<FExecutionCounts>();
    AssetImportPolicy::FExecutionDependencies ExactCollisionDependencies =
        MakeCountingDependencies(ExactCollisionCounts);
    ExactCollisionDependencies.ImportFactory = ImportFactory.Get();
    const PinWrightAssetImportHandler::FResult ExactCollisionResult =
        PinWrightAssetImportHandler::Execute(
            MakeHandlerRequest(
                ExactCollisionSourcePath, ExactCollisionObjectPath,
                /*bOverwrite=*/true),
            &ExactCollisionDependencies);

    TestFalse(TEXT("exact occupied non-texture is refused"),
        ExactCollisionResult.bSuccess);
    TestEqual(TEXT("exact non-texture uses unsafe code"),
        ExactCollisionResult.ErrorCode,
        FString(ErrorCodes::ERR_OVERWRITE_UNSAFE));
    TestEqual(TEXT("exact non-texture runs no general import"),
        ExactCollisionCounts->ImportExecutions, 0);
    TestEqual(TEXT("exact non-texture runs no reimport"),
        ExactCollisionCounts->ReimportExecutions, 0);
    TestEqual(TEXT("exact non-texture creates no reimport factory"),
        ExactCollisionCounts->ReimportFactoryCreations, 0);
    TestEqual(TEXT("exact non-texture reaches no pre-adapter hook"),
        ExactCollisionCounts->PreAdapterHooks, 0);
    TestEqual(TEXT("exact non-texture does not invoke import factory"),
        ImportFactory->CreateCallCount, 1);
    if (!TestTrue(TEXT("exact refusal response data exists"),
            ExactCollisionResult.Data.IsValid()))
    {
        return false;
    }
    FString ExactAssetPath;
    FString ExistingPath;
    FString ExistingClass;
    FString ExactReason;
    ExactCollisionResult.Data->TryGetStringField(
        TEXT("assetPath"), ExactAssetPath);
    ExactCollisionResult.Data->TryGetStringField(
        TEXT("existingPath"), ExistingPath);
    ExactCollisionResult.Data->TryGetStringField(
        TEXT("existingClass"), ExistingClass);
    ExactCollisionResult.Data->TryGetStringField(
        TEXT("reason"), ExactReason);
    TestEqual(TEXT("exact refusal reports requested path"),
        ExactAssetPath, ExactCollisionObjectPath);
    TestEqual(TEXT("exact refusal reports existing path"),
        ExistingPath, ExactCollisionObjectPath);
    TestEqual(TEXT("exact refusal reports existing class"),
        ExistingClass, UCurveFloat::StaticClass()->GetPathName());
    TestTrue(TEXT("exact refusal reports non-texture reason"),
        ExactReason.Contains(TEXT("only exact UTexture2D")));
    TestTrue(TEXT("exact refusal reports exact package conflict"),
        JsonArrayContains(
            ExactCollisionResult.Data, TEXT("conflicts"),
            ExactCollisionPackage));
    TestTrue(TEXT("exact non-texture pointer remains at path"),
        StaticFindObject(
            UObject::StaticClass(), nullptr, *ExactCollisionObjectPath)
            == ExactCollisionAsset.Get());
    TestTrue(TEXT("exact non-texture registry identity unchanged"),
        ResolveRegistryObject(ExactCollisionObjectPath)
            == ExactCollisionAsset.Get());
    TestTrue(TEXT("exact non-texture hard reference remains valid"),
        ReferenceActor->HardReferences[2]
            == ExactCollisionAsset.Get());
    TestTrue(TEXT("exact non-texture soft reference remains valid"),
        ReferenceActor->SoftReferences[2].Get()
            == ExactCollisionAsset.Get());
    TestTrue(TEXT("exact non-texture disk file remains unchanged"),
        SavedPackageFileMatches(ExactCollisionSavedFile));
    TestTrue(TEXT("saved primary output remains unchanged"),
        SavedPackageFileMatches(PrimarySavedFile));
    TestTrue(TEXT("saved collateral output remains unchanged"),
        SavedPackageFileMatches(CollateralSavedFile));
    TestFalse(TEXT("exact refusal has no stage fields"),
        HasLegacyStageFields(ExactCollisionResult.Data));
    return true;
}
