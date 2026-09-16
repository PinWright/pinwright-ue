// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/AssetImportPolicy.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Compat/EngineVersionCompat.h"
#include "AssetToolsModule.h"
#include "AutomatedAssetImportData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Handlers/ErrorCodes.h"
#include "IAssetTools.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/PackageSegment.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
#include "Serialization/BulkDataCookedIndex.h"
#endif
#include "UObject/Package.h"
#include "UObject/PackageResourceManager.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "Utils/AssetUtils.h"
#include "Utils/JsonBuilders.h"

UPinWrightTextureReimportFactory::UPinWrightTextureReimportFactory(
    const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    SupportedClass = UTexture2D::StaticClass();
    bCreateNew = false;
    bEditorImport = true;

    // FReimportHandler registers from its base constructor. This adapter is
    // direct-call only, and Remove is idempotent when its destructor repeats it.
    FReimportManager::Instance()->UnregisterHandler(*this);
}

void UPinWrightTextureReimportFactory::SetTargetTexture(UTexture2D* Texture)
{
    OriginalTexture = Texture;
}

void UPinWrightTextureReimportFactory::SetValidatedSource(
    const FString& SourcePath, TArray<uint8>&& SourceBytes)
{
    ValidatedSourcePath = SourcePath;
    ValidatedSourceBytes = MoveTemp(SourceBytes);
}

bool UPinWrightTextureReimportFactory::CanReimport(
    UObject* Object, TArray<FString>& OutFilenames)
{
    UTexture2D* Texture = Cast<UTexture2D>(Object);
    if (!Texture || Texture != OriginalTexture
        || Texture->GetClass() != UTexture2D::StaticClass()
        || !Texture->AssetImportData)
    {
        return false;
    }

    Texture->AssetImportData->ExtractFilenames(OutFilenames);
    return true;
}

void UPinWrightTextureReimportFactory::SetReimportPaths(
    UObject* Object, const TArray<FString>& NewReimportPaths)
{
    UTexture2D* Texture = Cast<UTexture2D>(Object);
    if (Texture && Texture == OriginalTexture
        && Texture->GetClass() == UTexture2D::StaticClass()
        && Texture->AssetImportData && NewReimportPaths.Num() == 1)
    {
        Texture->AssetImportData->UpdateFilenameOnly(NewReimportPaths[0]);
    }
}

UTexture2D* UPinWrightTextureReimportFactory::CreateTexture2D(
    UObject* InParent, FName Name, EObjectFlags /*Flags*/)
{
    if (OriginalTexture && OriginalTexture->GetOuter() == InParent
        && OriginalTexture->GetFName() == Name)
    {
        OriginalTexture->ReleaseResource();
        return OriginalTexture;
    }
    return nullptr;
}

bool UPinWrightTextureReimportFactory::IsAutomatedImport() const
{
    return Super::IsAutomatedImport() || IsAutomatedReimport();
}

EReimportResult::Type UPinWrightTextureReimportFactory::Reimport(
    UObject* Object)
{
    bReimportInvoked = true;
    UTexture2D* Texture = Cast<UTexture2D>(Object);
    if (!Texture || Texture != OriginalTexture
        || Texture->GetClass() != UTexture2D::StaticClass()
        || !Texture->AssetImportData)
    {
        return EReimportResult::Failed;
    }

    const FString SourcePath = Texture->AssetImportData->GetFirstFilename();
    if (SourcePath.IsEmpty() || ValidatedSourceBytes.IsEmpty()
        || !FPaths::IsSamePath(SourcePath, ValidatedSourcePath))
    {
        return EReimportResult::Failed;
    }

    TGuardValue<bool> ReimportGuard(bIsDoingAReimport, true);

    CompressionSettings = Texture->CompressionSettings;
    NoCompression = Texture->CompressionNone;
    NoAlpha = Texture->CompressionNoAlpha;
    bDeferCompression = Texture->DeferCompression;
    MipGenSettings = Texture->MipGenSettings;

    UTextureFactory::SuppressImportOverwriteDialog();
    FMD5 Md5;
    Md5.Update(ValidatedSourceBytes.GetData(), ValidatedSourceBytes.Num());
    FMD5Hash SourceHash;
    SourceHash.Set(Md5);
    TGuardValue<FString> FilenameGuard(CurrentFilename, SourcePath);
    TGuardValue<FMD5Hash> FileHashGuard(FileHash, SourceHash);

    const uint8* Buffer = ValidatedSourceBytes.GetData();
    UObject* Imported = FactoryCreateBinary(
        UTexture2D::StaticClass(), Texture->GetOuter(), Texture->GetFName(),
        RF_Public | RF_Standalone, nullptr, TEXT("png"), Buffer,
        ValidatedSourceBytes.GetData() + ValidatedSourceBytes.Num(), GWarn);
    if (Imported != Texture)
    {
        return EReimportResult::Failed;
    }

    Texture->AssetImportData->Update(SourcePath, SourceHash);
    Texture->GetOutermost()->MarkPackageDirty();
    return EReimportResult::Succeeded;
}

namespace
{
    using namespace AssetImportPolicy;

    bool HashFileStreaming(
        const FString& Filename,
        int64 ExpectedSize,
        const FDateTime& ExpectedTimestamp,
        FString& OutSha1)
    {
        IFileManager& FileManager = IFileManager::Get();
        if (FileManager.FileSize(*Filename) != ExpectedSize
            || FileManager.GetTimeStamp(*Filename) != ExpectedTimestamp)
        {
            return false;
        }
        TUniquePtr<FArchive> Reader(
            FileManager.CreateFileReader(*Filename));
        if (!Reader || Reader->TotalSize() != ExpectedSize)
        {
            return false;
        }

        FSHA1 Sha1;
        uint8 Buffer[64 * 1024];
        int64 Remaining = ExpectedSize;
        while (Remaining > 0)
        {
            const int64 ChunkSize = FMath::Min<int64>(Remaining, sizeof(Buffer));
            Reader->Serialize(Buffer, ChunkSize);
            if (Reader->IsError())
            {
                return false;
            }
            Sha1.Update(Buffer, ChunkSize);
            Remaining -= ChunkSize;
        }
        Sha1.Final();

        uint8 Digest[FSHA1::DigestSize];
        Sha1.GetHash(Digest);
        OutSha1 = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
        return FileManager.FileSize(*Filename) == ExpectedSize
            && FileManager.GetTimeStamp(*Filename) == ExpectedTimestamp;
    }

    bool EnumeratePackageResources(
        const FString& PackageName,
        TArray<FPackageResource>& OutResources,
        FString& OutError)
    {
        OutResources.Reset();
        FPackagePath PackagePath;
        if (!FPackagePath::TryFromPackageName(PackageName, PackagePath))
        {
            OutError = FString::Printf(
                TEXT("Could not resolve package resource path for '%s'"),
                *PackageName);
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
        TSet<FString> SeenFilenames;
        IPackageResourceManager& Resources = IPackageResourceManager::Get();
        for (const EPackageSegment Segment : Segments)
        {
            FPackagePath UpdatedPath;
            if (!Resources.DoesPackageExist(
                    MCP_PACKAGE_RESOURCE_ARGS(PackagePath, Segment),
                    &UpdatedPath))
            {
                continue;
            }

            const FString Filename = Segment == EPackageSegment::Header
                ? UpdatedPath.GetLocalFullPath()
                : UpdatedPath.GetLocalFullPath(Segment);
            if (Filename.IsEmpty() || SeenFilenames.Contains(Filename))
            {
                continue;
            }

            FPackageResource& Resource = OutResources.AddDefaulted_GetRef();
            Resource.Filename = Filename;
            Resource.Size = Resources.FileSize(
                MCP_PACKAGE_RESOURCE_ARGS(PackagePath, Segment));
            if (Resource.Size < 0)
            {
                OutError = FString::Printf(
                    TEXT("Could not size package resource '%s'"), *Filename);
                return false;
            }
            Resource.Timestamp =
                IFileManager::Get().GetTimeStamp(*Resource.Filename);
            SeenFilenames.Add(Filename);
        }

        OutResources.Sort([](const FPackageResource& A, const FPackageResource& B)
        {
            return A.Filename < B.Filename;
        });
        return true;
    }

    bool HashPackageResources(
        TArray<FPackageResource>& Resources, FString& OutError)
    {
        for (FPackageResource& Resource : Resources)
        {
            if (!HashFileStreaming(
                    Resource.Filename, Resource.Size,
                    Resource.Timestamp, Resource.Sha1))
            {
                OutError = FString::Printf(
                    TEXT("Could not hash package resource '%s'"),
                    *Resource.Filename);
                return false;
            }
        }
        return true;
    }

    bool ArePackageResourcesUnchanged(
        const FString& PackageName,
        const TArray<FPackageResource>& Before,
        FString& OutError)
    {
        TArray<FPackageResource> After;
        if (!EnumeratePackageResources(PackageName, After, OutError))
        {
            return false;
        }
        if (After.Num() != Before.Num())
        {
            OutError = FString::Printf(
                TEXT("Package resource count changed for '%s'"), *PackageName);
            return false;
        }
        for (int32 Index = 0; Index < Before.Num(); ++Index)
        {
            const FPackageResource& Expected = Before[Index];
            FPackageResource& Actual = After[Index];
            if (!Expected.Filename.Equals(
                    Actual.Filename, ESearchCase::IgnoreCase)
                || Expected.Size != Actual.Size
                || Expected.Timestamp != Actual.Timestamp
                || !HashFileStreaming(
                    Actual.Filename, Actual.Size,
                    Actual.Timestamp, Actual.Sha1)
                || Expected.Sha1 != Actual.Sha1)
            {
                OutError = FString::Printf(
                    TEXT("asset.import changed package resource '%s' before save"),
                    *Expected.Filename);
                return false;
            }
        }
        return true;
    }

    bool IsSupportedTextureSource(const FString& SourcePath)
    {
        return FPaths::GetExtension(SourcePath).Equals(
            TEXT("png"), ESearchCase::IgnoreCase);
    }

    bool ValidateTextureSource(
        const FString& SourcePath,
        TArray<uint8>& OutSourceBytes,
        int32& OutWidth,
        int32& OutHeight,
        int64& OutDecodedByteCount,
        FString& OutError)
    {
        OutSourceBytes.Reset();
        OutWidth = 0;
        OutHeight = 0;
        OutDecodedByteCount = 0;
        OutError.Reset();
        if (!IsSupportedTextureSource(SourcePath))
        {
            OutError = TEXT("only PNG source files use the pinned texture lane");
            return false;
        }

        const FString BaseName = FPaths::GetBaseFilename(SourcePath);
        if (BaseName.Len() >= 5)
        {
            const FString Suffix = BaseName.Right(4);
            const TCHAR Separator = BaseName[BaseName.Len() - 5];
            const int32 TileNumber = FCString::Atoi(*Suffix);
            if ((Separator == TEXT('.') || Separator == TEXT('_'))
                && Suffix.IsNumeric() && TileNumber >= 1001)
            {
                OutError = TEXT("UDIM texture sources are not admitted to the single-object lane");
                return false;
            }
        }

        const int64 SourceSize = IFileManager::Get().FileSize(*SourcePath);
        if (SourceSize <= 0 || SourceSize > MaxTextureSourceByteCount)
        {
            OutError = FString::Printf(
                TEXT("PNG source size must be between 1 and %lld bytes"),
                MaxTextureSourceByteCount);
            return false;
        }

        TArray<uint8> SourceBytes;
        if (!FFileHelper::LoadFileToArray(SourceBytes, *SourcePath)
            || SourceBytes.Num() != SourceSize)
        {
            OutError = TEXT("the bounded PNG source could not be read exactly");
            return false;
        }

        IImageWrapperModule& ImageWrapperModule =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>(
                TEXT("ImageWrapper"));
        if (ImageWrapperModule.DetectImageFormat(
                SourceBytes.GetData(), SourceBytes.Num())
            != EImageFormat::PNG)
        {
            OutError = TEXT("the source content is not a PNG image");
            return false;
        }

        const TSharedPtr<IImageWrapper> Wrapper =
            ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        TArray64<uint8> RawPixels;
        if (!Wrapper
            || !Wrapper->SetCompressed(
                SourceBytes.GetData(), SourceBytes.Num()))
        {
            OutError = TEXT("the PNG source header could not be decoded");
            return false;
        }

        const int64 Width = Wrapper->GetWidth();
        const int64 Height = Wrapper->GetHeight();
        if (Width <= 0 || Height <= 0
            || Width > MaxTextureDecodedByteCount / 4 / Height)
        {
            OutError = FString::Printf(
                TEXT("decoded PNG data exceeds the %lld-byte safety limit"),
                MaxTextureDecodedByteCount);
            return false;
        }
        const int64 DecodedByteCount = Width * Height * 4;
        if (!Wrapper->GetRaw(ERGBFormat::RGBA, 8, RawPixels)
            || RawPixels.Num() != DecodedByteCount
            || RawPixels.IsEmpty())
        {
            OutError = TEXT("the PNG source failed a complete decode preflight");
            return false;
        }
        OutSourceBytes = MoveTemp(SourceBytes);
        OutWidth = static_cast<int32>(Width);
        OutHeight = static_cast<int32>(Height);
        OutDecodedByteCount = DecodedByteCount;
        return true;
    }

    IAssetRegistry& GetAssetRegistry()
    {
        return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
            TEXT("AssetRegistry")).Get();
    }

}

bool AssetImportPolicy::RestoreFailedReimportState(
    UTexture2D* ExistingTexture,
    const FReimportRecoverySnapshot& RecoverySnapshot,
    bool bFailureMayHaveMutatedContent,
    FReimportOutcome& Outcome)
{
    Outcome.bSourceMetadataRestored = false;
    Outcome.bDirtyStateRestored = false;
    Outcome.bFailureMayHaveMutatedContent =
        bFailureMayHaveMutatedContent;
    Outcome.bFailureOccurredBeforeContentMutation =
        !bFailureMayHaveMutatedContent;
    Outcome.bPackageLeftDirtyForPossibleMutation = false;
    Outcome.bInMemoryContentRestored = false;

    if (!IsValid(ExistingTexture) || !ExistingTexture->GetOutermost())
    {
        return false;
    }

    ExistingTexture->GetOutermost()->SetDirtyFlag(
        bFailureMayHaveMutatedContent
            ? true : RecoverySnapshot.bPackageWasDirty);
    Outcome.bPackageLeftDirtyForPossibleMutation =
        bFailureMayHaveMutatedContent
        && ExistingTexture->GetOutermost()->IsDirty();
    Outcome.bDirtyStateRestored = RecoverySnapshot.bCaptured
        && ExistingTexture->GetOutermost()->IsDirty()
            == RecoverySnapshot.bPackageWasDirty;

    if (!RecoverySnapshot.bCaptured || !ExistingTexture->AssetImportData)
    {
        return false;
    }

    // UE 5.8 has no SetSourceData API. SourceData is the public exact value
    // returned by GetSourceData, so assign that snapshot and measure read-back.
    ExistingTexture->AssetImportData->SourceData =
        RecoverySnapshot.SourceMetadata;
    Outcome.bSourceMetadataRestored =
        ExistingTexture->AssetImportData->GetSourceData().ToJson()
        == RecoverySnapshot.SourceMetadata.ToJson();
    return Outcome.bSourceMetadataRestored
        && (bFailureMayHaveMutatedContent
            ? Outcome.bPackageLeftDirtyForPossibleMutation
            : Outcome.bDirtyStateRestored);
}

void AssetImportPolicy::ConfigureImportData(UAutomatedAssetImportData& ImportData)
{
    ImportData.bReplaceExisting = false;
}

AssetImportPolicy::FDestinationDecision AssetImportPolicy::EvaluateDestination(
    bool bDestinationExists, bool bOverwrite)
{
    FDestinationDecision Decision;
    Decision.bRefused = bDestinationExists && !bOverwrite;
    if (Decision.bRefused)
    {
        Decision.ErrorCode = ErrorCodes::ERR_ASSET_ALREADY_EXISTS;
    }
    return Decision;
}

bool AssetImportPolicy::FValidatedTextureReimportPlan::IsValid() const
{
    return !SourcePath.IsEmpty()
        && !SourceBytes.IsEmpty()
        && SourceBytes.Num() <= MaxTextureSourceByteCount
        && SourceSha1.Len() == FSHA1::DigestSize * 2
        && Width > 0
        && Height > 0
        && DecodedByteCount == static_cast<int64>(Width) * Height * 4
        && DecodedByteCount <= MaxTextureDecodedByteCount;
}

bool AssetImportPolicy::BuildValidatedTextureReimportPlan(
    const FString& SourcePath,
    FValidatedTextureReimportPlan& OutPlan,
    FString& OutError)
{
    OutPlan = FValidatedTextureReimportPlan();
    TArray<uint8> SourceBytes;
    int32 Width = 0;
    int32 Height = 0;
    int64 DecodedByteCount = 0;
    if (!ValidateTextureSource(
            SourcePath, SourceBytes, Width, Height,
            DecodedByteCount, OutError))
    {
        return false;
    }

    uint8 Digest[FSHA1::DigestSize];
    FSHA1::HashBuffer(SourceBytes.GetData(), SourceBytes.Num(), Digest);
    OutPlan.SourcePath = SourcePath;
    OutPlan.SourceBytes = MoveTemp(SourceBytes);
    OutPlan.SourceSha1 = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
    OutPlan.Width = Width;
    OutPlan.Height = Height;
    OutPlan.DecodedByteCount = DecodedByteCount;
    if (!OutPlan.IsValid())
    {
        OutPlan = FValidatedTextureReimportPlan();
        OutError = TEXT("the completed PNG reimport plan is invalid");
        return false;
    }
    return true;
}

AssetImportPolicy::FObjectFingerprint AssetImportPolicy::CaptureObjectFingerprint(
    UObject* Object)
{
    FObjectFingerprint Fingerprint;
    if (!IsValid(Object))
    {
        return Fingerprint;
    }

    Fingerprint.Identity = Object;
    Fingerprint.ObjectClass = Object->GetClass();
    Fingerprint.Package = Object->GetOutermost();
    if (UTexture2D* Texture = Cast<UTexture2D>(Object);
        Texture && Texture->GetClass() == UTexture2D::StaticClass()
        && Texture->Source.IsValid())
    {
        Fingerprint.TextureContentId = Texture->Source.GetId();
        Fingerprint.bTextureContentIdValid =
            Fingerprint.TextureContentId.IsValid();
    }
    return Fingerprint;
}

bool AssetImportPolicy::ValidateCandidateBudget(
    const TArray<FString>& PackageNames, FString& OutError)
{
    OutError.Reset();
    if (PackageNames.Num() > MaxDestinationPackageCount)
    {
        OutError = FString::Printf(
            TEXT("Destination prefix matched %d packages; the safety limit is %d"),
            PackageNames.Num(), MaxDestinationPackageCount);
        return false;
    }
    return true;
}

bool AssetImportPolicy::DiscoverDestinationPackages(
    const FString& DestinationPath,
    const FString& RequestedAssetPath,
    const TArray<FString>& DestinationAssetPrefixes,
    TArray<FString>& OutPackageNames,
    FString& OutError)
{
    OutPackageNames.Reset();
    OutError.Reset();

    FString Root = DestinationPath;
    while (Root.Len() > 1 && Root.EndsWith(TEXT("/")))
    {
        Root.LeftChopInline(1, EAllowShrinking::No);
    }

    TSet<FName> CandidatePackageKeys;
    bool bCandidateOverflow = false;
    bool bExaminationOverflow = false;
    int32 EntriesExamined = 0;
    const auto AddPackage = [
        &CandidatePackageKeys, &OutPackageNames, &DestinationAssetPrefixes,
        &bCandidateOverflow](const FString& PackageName)
    {
        const FName PackageKey(*PackageName);
        const FString Leaf = FPackageName::GetLongPackageAssetName(PackageName);
        const bool bMatches = DestinationAssetPrefixes.ContainsByPredicate(
            [&Leaf](const FString& Prefix)
            {
                return !Prefix.IsEmpty()
                    && Leaf.StartsWith(Prefix, ESearchCase::IgnoreCase);
            });
        if (bMatches && !CandidatePackageKeys.Contains(PackageKey))
        {
            CandidatePackageKeys.Add(PackageKey);
            OutPackageNames.Add(PackageName);
            bCandidateOverflow =
                CandidatePackageKeys.Num() > MaxDestinationPackageCount;
        }
        return !bCandidateOverflow;
    };

    UObject* ExactLiveObject = StaticFindObject(
        UObject::StaticClass(), nullptr, *RequestedAssetPath);
    if (IsValid(ExactLiveObject))
    {
        AddPackage(
            FPackageName::ObjectPathToPackageName(RequestedAssetPath));
    }

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Root));
    Filter.bRecursivePaths = false;
    const bool bRegistryEnumerationValid =
        GetAssetRegistry().EnumerateAssets(Filter,
        [&AddPackage, &EntriesExamined, &bExaminationOverflow](
            const FAssetData& Asset)
        {
            if (++EntriesExamined > MaxDestinationEntriesExamined)
            {
                bExaminationOverflow = true;
                return false;
            }
            return AddPackage(Asset.PackageName.ToString());
        });
    if (!bRegistryEnumerationValid)
    {
        OutError = TEXT("Destination registry enumeration could not be completed");
        return false;
    }
    if (bCandidateOverflow || bExaminationOverflow)
    {
        OutError = bExaminationOverflow
            ? FString::Printf(
                TEXT("Destination discovery examined more than %d registry/disk entries"),
                MaxDestinationEntriesExamined)
            : FString::Printf(
                TEXT("Destination prefixes exceed the %d-package safety limit"),
                MaxDestinationPackageCount);
        return false;
    }

    FString DestinationDirectory;
    if (!FPackageName::TryConvertLongPackageNameToFilename(
            Root, DestinationDirectory))
    {
        OutError = FString::Printf(
            TEXT("Destination path '%s' could not be mapped to a directory"),
            *Root);
        return false;
    }
    IFileManager& FileManager = IFileManager::Get();
    if (FileManager.DirectoryExists(*DestinationDirectory))
    {
        const bool bDirectoryEnumerationCompleted = FileManager.IterateDirectory(
            *DestinationDirectory,
            [&AddPackage, &Root, &EntriesExamined, &bExaminationOverflow](
                const TCHAR* FilenameOrDirectory, bool bIsDirectory)
            {
                if (++EntriesExamined > MaxDestinationEntriesExamined)
                {
                    bExaminationOverflow = true;
                    return false;
                }
                if (bIsDirectory)
                {
                    return true;
                }
                const FString Extension = FPaths::GetExtension(
                    FilenameOrDirectory, /*bIncludeDot=*/true);
                if (!Extension.Equals(
                        FPackageName::GetAssetPackageExtension(),
                        ESearchCase::IgnoreCase)
                    && !Extension.Equals(
                        FPackageName::GetMapPackageExtension(),
                        ESearchCase::IgnoreCase))
                {
                    return true;
                }
                return AddPackage(
                    Root / FPaths::GetBaseFilename(FilenameOrDirectory));
            });
        if (!bDirectoryEnumerationCompleted
            && !bCandidateOverflow && !bExaminationOverflow)
        {
            OutError = FString::Printf(
                TEXT("Existing destination directory '%s' could not be completely enumerated"),
                *DestinationDirectory);
            return false;
        }
    }

    if (bCandidateOverflow || bExaminationOverflow)
    {
        OutError = bExaminationOverflow
            ? FString::Printf(
                TEXT("Destination discovery examined more than %d registry/disk entries"),
                MaxDestinationEntriesExamined)
            : FString::Printf(
                TEXT("Destination prefixes exceed the %d-package safety limit"),
                MaxDestinationPackageCount);
        return false;
    }

    OutPackageNames.Sort([](const FString& A, const FString& B)
    {
        const int32 CaseInsensitive = A.Compare(B, ESearchCase::IgnoreCase);
        return CaseInsensitive == 0 ? A < B : CaseInsensitive < 0;
    });
    return ValidateCandidateBudget(OutPackageNames, OutError);
}

bool AssetImportPolicy::CaptureDestinationSnapshot(
    const TArray<FString>& PackageNames,
    FDestinationSnapshot& OutSnapshot,
    FString& OutError)
{
    OutSnapshot = FDestinationSnapshot();
    OutError.Reset();
    if (!ValidateCandidateBudget(PackageNames, OutError))
    {
        return false;
    }
    int64 TotalResourceBytes = 0;
    for (const FString& PackageName : PackageNames)
    {
        UPackage* Package = FindPackage(nullptr, *PackageName);
        if (!Package)
        {
            Package = LoadPackage(nullptr, *PackageName, LOAD_None);
        }
        if (!Package)
        {
            OutError = FString::Printf(
                TEXT("Could not load overwrite candidate package '%s'"),
                *PackageName);
            return false;
        }

        TArray<FPackageResource>& Resources =
            OutSnapshot.PackageResources.Add(PackageName);
        if (!EnumeratePackageResources(PackageName, Resources, OutError))
        {
            return false;
        }
        for (const FPackageResource& Resource : Resources)
        {
            if (Resource.Size > MaxDestinationByteCount - TotalResourceBytes)
            {
                OutError = FString::Printf(
                    TEXT("Destination prefix exceeds the %lld-byte safety limit"),
                    MaxDestinationByteCount);
                return false;
            }
            TotalResourceBytes += Resource.Size;
        }

        TArray<UObject*> PackageObjects;
        GetObjectsWithPackage(
            Package, PackageObjects, MCP_FOREACH_EXCLUDE_NESTED_OBJECTS, RF_Transient,
            EInternalObjectFlags::Garbage);
        PackageObjects.RemoveAll([](UObject* Object)
        {
            return !IsValid(Object) || !Object->IsAsset();
        });
        PackageObjects.Sort([](const UObject& A, const UObject& B)
        {
            return A.GetPathName() < B.GetPathName();
        });
        if (PackageObjects.Num() != 1)
        {
            OutError = FString::Printf(
                TEXT("Overwrite candidate package '%s' must contain exactly one asset"),
                *PackageName);
            return false;
        }
        for (UObject* Object : PackageObjects)
        {
            const FObjectFingerprint Fingerprint = CaptureObjectFingerprint(Object);
            if (!Fingerprint.bTextureContentIdValid)
            {
                OutError = FString::Printf(
                    TEXT("Could not fingerprint overwrite candidate '%s'"),
                    *Object->GetPathName());
                return false;
            }
            OutSnapshot.ObjectFingerprints.Add(
                FName(*Object->GetPathName()), Fingerprint);
        }
    }

    // Only after every resource size has been admitted do we read and hash it.
    for (TPair<FString, TArray<FPackageResource>>& Pair
        : OutSnapshot.PackageResources)
    {
        if (!HashPackageResources(Pair.Value, OutError))
        {
            return false;
        }
    }
    return true;
}

UObject* AssetImportPolicy::ResolveSafeInPlaceReimport(
    const FString& RequestedAssetPath,
    const TArray<FString>& DestinationPackages,
    UObject*& OutResolvedObject,
    FString& OutReason)
{
    OutResolvedObject = nullptr;
    OutReason.Reset();
    const FString RequestedPackage =
        FPackageName::ObjectPathToPackageName(RequestedAssetPath);
    if (DestinationPackages.Num() != 1
        || !DestinationPackages[0].Equals(
            RequestedPackage, ESearchCase::IgnoreCase))
    {
        OutReason = TEXT("overwrite owns more than the one exact requested package");
        return nullptr;
    }

    const FResolvedAsset Resolved =
        ResolveAsset(RequestedAssetPath, /*bLoadObject=*/true);
    UObject* ExistingObject = Resolved.Object;
    OutResolvedObject = ExistingObject;
    if (!IsValid(ExistingObject))
    {
        OutReason = TEXT("the exact requested object could not be resolved");
        return nullptr;
    }
    if (ExistingObject->GetOutermost()->GetName() != RequestedPackage
        || ExistingObject->GetPathName() != RequestedAssetPath)
    {
        OutReason = TEXT("the resolved object does not own the exact requested path");
        return nullptr;
    }

    TArray<UObject*> PackageAssets;
    GetObjectsWithPackage(
        ExistingObject->GetOutermost(), PackageAssets, MCP_FOREACH_EXCLUDE_NESTED_OBJECTS,
        RF_Transient, EInternalObjectFlags::Garbage);
    PackageAssets.RemoveAll([](UObject* Object)
    {
        return !IsValid(Object) || !Object->IsAsset();
    });
    if (PackageAssets.Num() != 1 || PackageAssets[0] != ExistingObject)
    {
        OutReason = TEXT("the occupied package contains collateral asset objects");
        return nullptr;
    }

    UTexture2D* ExistingTexture = Cast<UTexture2D>(ExistingObject);
    if (!ExistingTexture
        || ExistingObject->GetClass() != UTexture2D::StaticClass()
        || !ExistingTexture->AssetImportData)
    {
        OutReason = TEXT("only exact UTexture2D assets use the pinned reimport lane");
        return nullptr;
    }

    const FAssetData AssetData = GetAssetRegistry().GetAssetByObjectPath(
        FSoftObjectPath(RequestedAssetPath), /*bIncludeOnlyOnDiskAssets=*/false);
    if (!AssetData.IsValid() || AssetData.FastGetAsset(false) != ExistingObject)
    {
        OutReason = TEXT("the Asset Registry does not resolve the exact existing identity");
        return nullptr;
    }
    return ExistingObject;
}

TSharedPtr<FJsonObject> AssetImportPolicy::BuildUnsafeOverwriteData(
    const FString& RequestedAssetPath,
    const FString& SourcePath,
    UObject* ExistingObject,
    const FString& Reason,
    const TArray<FString>& Conflicts)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("assetPath"), RequestedAssetPath);
    Data->SetStringField(TEXT("sourcePath"), SourcePath);
    Data->SetStringField(TEXT("reason"), Reason);
    Data->SetArrayField(
        TEXT("conflicts"), JsonBuilders::BuildStringArrayJson(Conflicts));
    if (IsValid(ExistingObject))
    {
        Data->SetStringField(TEXT("existingPath"), ExistingObject->GetPathName());
        Data->SetStringField(
            TEXT("existingClass"), ExistingObject->GetClass()->GetPathName());
    }

    const FString PackageName =
        FPackageName::ObjectPathToPackageName(RequestedAssetPath);
    TArray<FName> ReferencerNames;
    GetAssetRegistry().GetReferencers(
        FName(*PackageName), ReferencerNames,
        UE::AssetRegistry::EDependencyCategory::Package,
        UE::AssetRegistry::FDependencyQuery());
    TSet<FName> UniqueReferencers;
    for (const FName ReferencerName : ReferencerNames)
    {
        if (!ReferencerName.IsNone()
            && ReferencerName != FName(*PackageName))
        {
            UniqueReferencers.Add(ReferencerName);
        }
    }
    TArray<FString> Referencers;
    Referencers.Reserve(FMath::Min(
        UniqueReferencers.Num(), MaxDiagnosticReferencerCount));
    const auto CompareReferencers = [](const FString& A, const FString& B)
    {
        const int32 CaseInsensitive = A.Compare(B, ESearchCase::IgnoreCase);
        return CaseInsensitive == 0 ? A.Compare(B) : CaseInsensitive;
    };
    for (const FName Referencer : UniqueReferencers)
    {
        FString Candidate = Referencer.ToString();
        if (Referencers.Num() < MaxDiagnosticReferencerCount)
        {
            Referencers.Add(MoveTemp(Candidate));
            continue;
        }

        int32 LargestIndex = 0;
        for (int32 Index = 1; Index < Referencers.Num(); ++Index)
        {
            if (CompareReferencers(
                    Referencers[LargestIndex], Referencers[Index]) < 0)
            {
                LargestIndex = Index;
            }
        }
        if (CompareReferencers(Candidate, Referencers[LargestIndex]) < 0)
        {
            Referencers[LargestIndex] = MoveTemp(Candidate);
        }
    }
    Referencers.Sort([&CompareReferencers](const FString& A, const FString& B)
    {
        return CompareReferencers(A, B) < 0;
    });
    const int32 ReferencerCount = UniqueReferencers.Num();
    Data->SetNumberField(TEXT("referencerCount"), ReferencerCount);
    Data->SetArrayField(
        TEXT("referencers"), JsonBuilders::BuildStringArrayJson(Referencers));
    Data->SetBoolField(
        TEXT("referencersTruncated"),
        ReferencerCount > MaxDiagnosticReferencerCount);
    return Data;
}

TArray<UObject*> AssetImportPolicy::RunImport(
    UAutomatedAssetImportData* ImportData,
    const FExecutionDependencies* Dependencies)
{
    if (ImportData && Dependencies && Dependencies->ImportFactory)
    {
        ImportData->Factory = Dependencies->ImportFactory;
    }
    if (Dependencies && Dependencies->ImportExecutionProbe)
    {
        Dependencies->ImportExecutionProbe();
    }
    if (Dependencies && Dependencies->ImportRunner)
    {
        return Dependencies->ImportRunner(ImportData);
    }
    IAssetTools& AssetTools =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(
            TEXT("AssetTools")).Get();
    return AssetTools.ImportAssetsAutomated(ImportData);
}

AssetImportPolicy::FReimportOutcome AssetImportPolicy::RunReimport(
    UTexture2D* ExistingTexture,
    const FValidatedTextureReimportPlan& Plan,
    const FExecutionDependencies* Dependencies)
{
    FReimportOutcome Outcome;
    if (!ExistingTexture
        || ExistingTexture->GetClass() != UTexture2D::StaticClass()
        || !ExistingTexture->AssetImportData
        || !ExistingTexture->GetOutermost())
    {
        Outcome.FailureReason = TEXT("the pinned runner requires one exact UTexture2D");
        Outcome.bFailureOccurredBeforeContentMutation = true;
        return Outcome;
    }

    Outcome.RecoverySnapshot.SourceMetadata =
        ExistingTexture->AssetImportData->GetSourceData();
    Outcome.RecoverySnapshot.bPackageWasDirty =
        ExistingTexture->GetOutermost()->IsDirty();
    Outcome.RecoverySnapshot.bCaptured = true;
    const FGuid ContentIdBefore = ExistingTexture->Source.GetId();

    if (!Plan.IsValid())
    {
        Outcome.FailureReason = TEXT("the pinned runner requires a completed PNG reimport plan");
        RestoreFailedReimportState(
            ExistingTexture, Outcome.RecoverySnapshot,
            /*bFailureMayHaveMutatedContent=*/false, Outcome);
        return Outcome;
    }

    if (Dependencies && Dependencies->BeforeReimportAdapter
        && !Dependencies->BeforeReimportAdapter(ExistingTexture, Plan))
    {
        Outcome.FailureReason = TEXT("the request-local pre-adapter hook reported failure");
        RestoreFailedReimportState(
            ExistingTexture, Outcome.RecoverySnapshot,
            /*bFailureMayHaveMutatedContent=*/false, Outcome);
        return Outcome;
    }

    TStrongObjectPtr<UPinWrightTextureReimportFactory> Factory(
        NewObject<UPinWrightTextureReimportFactory>());
    if (!Factory)
    {
        Outcome.FailureReason = TEXT("the pinned texture reimport factory could not be created");
        RestoreFailedReimportState(
            ExistingTexture, Outcome.RecoverySnapshot,
            /*bFailureMayHaveMutatedContent=*/false, Outcome);
        return Outcome;
    }
    if (Dependencies && Dependencies->ReimportFactoryProbe)
    {
        Dependencies->ReimportFactoryProbe();
    }

    Factory->SetTargetTexture(ExistingTexture);
    TArray<uint8> ValidatedSourceBytes = Plan.GetSourceBytes();
    Factory->SetValidatedSource(
        Plan.GetSourcePath(), MoveTemp(ValidatedSourceBytes));
    TArray<FString> ExistingSources;
    if (!Factory->CanReimport(ExistingTexture, ExistingSources))
    {
        Outcome.FailureReason = TEXT("the pinned texture reimport factory rejected the exact texture");
        RestoreFailedReimportState(
            ExistingTexture, Outcome.RecoverySnapshot,
            /*bFailureMayHaveMutatedContent=*/false, Outcome);
        return Outcome;
    }

    Factory->SetReimportPaths(
        ExistingTexture, TArray<FString>{Plan.GetSourcePath()});
    if (Dependencies && Dependencies->ReimportExecutionProbe)
    {
        Dependencies->ReimportExecutionProbe();
    }
    const bool bManagerSucceeded =
        FReimportManager::Instance()->Reimport(
            ExistingTexture,
            /*bAskForNewFileIfMissing=*/false,
            /*bShowNotification=*/false,
            Plan.GetSourcePath(),
            Factory.Get(),
            /*SourceFileIndex=*/INDEX_NONE,
            /*bForceNewFile=*/false,
            /*bAutomated=*/true
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            // FReimportManager::Reimport gained bInForceShowDialog in UE 5.5; through 5.4 it has
            // no such parameter and never forces a dialog, which is what false asks for.
            , /*bInForceShowDialog=*/false
#endif
            );
    Outcome.bAdapterInvoked = Factory->WasReimportInvoked();
    const bool bRunnerSucceeded =
        bManagerSucceeded && Outcome.bAdapterInvoked;

    const FGuid ContentIdAfter = ExistingTexture->Source.GetId();
    const bool bContentChanged = ContentIdBefore.IsValid()
        && ContentIdAfter.IsValid() && ContentIdBefore != ContentIdAfter;
    if (!bRunnerSucceeded || !bContentChanged)
    {
        Outcome.FailureReason = !Outcome.bAdapterInvoked
            ? TEXT("the reimport manager did not invoke the pinned texture adapter")
            : bRunnerSucceeded
                ? TEXT("the pinned reimport produced no measured texture source change")
                : TEXT("the pinned texture reimport runner reported failure");
        RestoreFailedReimportState(
            ExistingTexture, Outcome.RecoverySnapshot,
            /*bFailureMayHaveMutatedContent=*/Outcome.bAdapterInvoked,
            Outcome);
        return Outcome;
    }

    Outcome.bSucceeded = true;
    return Outcome;
}

bool AssetImportPolicy::PublishPrimaryOutput(
    UObject* Object,
    const FString& DestinationPackageName,
    const FString& DestinationObjectName,
    FString& OutError)
{
    OutError.Reset();
    if (!IsValid(Object))
    {
        OutError = TEXT("Cannot publish an invalid primary output");
        return false;
    }

    UPackage* DestinationPackage = FindPackage(
        nullptr, *DestinationPackageName);
    if (!DestinationPackage)
    {
        DestinationPackage = CreatePackage(*DestinationPackageName);
    }
    if (!DestinationPackage)
    {
        OutError = FString::Printf(
            TEXT("Could not create destination package '%s'"),
            *DestinationPackageName);
        return false;
    }

    const FString DestinationObjectPath =
        DestinationPackageName + TEXT(".") + DestinationObjectName;
    UObject* Current = StaticFindObject(
        UObject::StaticClass(), nullptr, *DestinationObjectPath);
    if (Current && Current != Object)
    {
        OutError = FString::Printf(
            TEXT("Destination object already exists: '%s'"),
            *DestinationObjectPath);
        return false;
    }

    const FString OldObjectPath = Object->GetPathName();
    if (!Object->Rename(
            *DestinationObjectName, DestinationPackage,
            REN_DontCreateRedirectors | REN_NonTransactional))
    {
        OutError = FString::Printf(
            TEXT("Could not publish '%s' as '%s'"),
            *OldObjectPath, *DestinationObjectPath);
        return false;
    }
    FAssetRegistryModule::AssetRenamed(Object, OldObjectPath);
    DestinationPackage->SetDirtyFlag(true);
    return true;
}

bool AssetImportPolicy::VerifyInPlacePostconditions(
    UObject* ExistingObject,
    const FString& RequestedAssetPath,
    const FDestinationSnapshot& Before,
    FString& OutError)
{
    OutError.Reset();
    const FObjectFingerprint* Expected =
        Before.ObjectFingerprints.Find(FName(*RequestedAssetPath));
    if (!Expected || !IsValid(ExistingObject)
        || Expected->Identity != ExistingObject
        || Expected->ObjectClass != ExistingObject->GetClass()
        || Expected->Package != ExistingObject->GetOutermost())
    {
        OutError = TEXT("the retained existing identity is no longer valid");
        return false;
    }

    const FString PackageName =
        FPackageName::ObjectPathToPackageName(RequestedAssetPath);
    if (ExistingObject->GetPathName() != RequestedAssetPath
        || ExistingObject->GetOutermost()->GetName() != PackageName
        || StaticFindObject(
            UObject::StaticClass(), nullptr, *RequestedAssetPath)
            != ExistingObject)
    {
        OutError = TEXT("the reimport changed object path, package, or identity");
        return false;
    }

    const FAssetData AssetData = GetAssetRegistry().GetAssetByObjectPath(
        FSoftObjectPath(RequestedAssetPath), /*bIncludeOnlyOnDiskAssets=*/false);
    if (!AssetData.IsValid() || AssetData.FastGetAsset(false) != ExistingObject)
    {
        OutError = TEXT("the Asset Registry no longer resolves the retained identity");
        return false;
    }
    if (!ExistingObject->GetOutermost()->IsDirty())
    {
        OutError = TEXT("the successful in-place reimport did not leave a pending save");
        return false;
    }

    UTexture2D* Texture = Cast<UTexture2D>(ExistingObject);
    if (!Texture || Texture->GetClass() != UTexture2D::StaticClass()
        || !Expected->bTextureContentIdValid
        || !Texture->Source.GetId().IsValid()
        || Texture->Source.GetId() == Expected->TextureContentId)
    {
        OutError = TEXT("the texture source content identity did not change");
        return false;
    }

    const TArray<FPackageResource>* ExpectedResources =
        Before.PackageResources.Find(PackageName);
    if (!ExpectedResources
        || !ArePackageResourcesUnchanged(
            PackageName, *ExpectedResources, OutError))
    {
        if (OutError.IsEmpty())
        {
            OutError = TEXT("the pre-import package resource snapshot is missing");
        }
        return false;
    }
    return true;
}

AssetImportPolicy::FResponse AssetImportPolicy::BuildResponse(
    const TArray<UObject*>& ImportedObjects,
    const FDestinationSnapshot& Before,
    const FString& AssetPathBeforeRename,
    bool bRenameRequested,
    bool bRenameSucceeded)
{
    FResponse Response;
    Response.Data = MakeShared<FJsonObject>();

    bool bInvalidOutput = false;
    TArray<TSharedPtr<FJsonValue>> Results;
    TSharedPtr<FJsonObject> PrimaryVerification;
    TSharedPtr<FJsonObject> PrimaryEntry;
    for (UObject* Object : ImportedObjects)
    {
        if (!Object)
        {
            continue;
        }

        const bool bIsPrimary = Results.IsEmpty();
        const FString ActualPath = Object->GetPathName();
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("assetPath"), ActualPath);
        Entry->SetBoolField(TEXT("valid"), IsValid(Object));
        Entry->SetStringField(TEXT("mode"), TEXT("created"));
        if (bIsPrimary)
        {
            Entry->SetBoolField(TEXT("renameRequested"), bRenameRequested);
            Entry->SetBoolField(TEXT("renameSucceeded"), bRenameSucceeded);
            PrimaryEntry = Entry;
        }
        AddAssetSaveReport(
            Entry, /*bSaveRequested=*/false, /*bSavedToDisk=*/false,
            EAssetSaveState::NotRequested);

        const FObjectFingerprint* BeforeFingerprint =
            Before.ObjectFingerprints.Find(FName(*ActualPath));
        if (!BeforeFingerprint && bIsPrimary && !AssetPathBeforeRename.IsEmpty())
        {
            BeforeFingerprint =
                Before.ObjectFingerprints.Find(FName(*AssetPathBeforeRename));
        }
        const bool bExistedBefore = BeforeFingerprint != nullptr;
        Entry->SetBoolField(TEXT("existedBefore"), bExistedBefore);
        if (!IsValid(Object) || ActualPath.IsEmpty()
            || StaticFindObject(
                UObject::StaticClass(), nullptr, *ActualPath) != Object)
        {
            Entry->SetBoolField(TEXT("resolved"), false);
            Entry->SetBoolField(TEXT("replaced"), false);
            Entry->SetBoolField(TEXT("replacementMeasured"), false);
            Entry->SetStringField(
                TEXT("replacementEvidence"), TEXT("invalidOutput"));
            Entry->SetStringField(
                TEXT("verificationFailure"),
                IsValid(Object) ? TEXT("unresolvableObject")
                                : TEXT("invalidObject"));
            Results.Add(MakeShared<FJsonValueObject>(Entry));
            bInvalidOutput = true;
            continue;
        }

        Entry->SetBoolField(TEXT("resolved"), true);
        const bool bIdentityChanged =
            bExistedBefore && BeforeFingerprint->Identity != Object;
        const bool bReplacementMeasured = !bExistedBefore || bIdentityChanged;
        const bool bReplaced = bIdentityChanged;

        FString ReplacementEvidence = TEXT("newOutput");
        if (bExistedBefore)
        {
            ReplacementEvidence = bIdentityChanged
                ? TEXT("identityChanged")
                : TEXT("unmeasured");
        }
        Entry->SetBoolField(TEXT("replaced"), bReplaced);
        Entry->SetBoolField(
            TEXT("replacementMeasured"), bReplacementMeasured);
        Entry->SetStringField(
            TEXT("replacementEvidence"), ReplacementEvidence);

        TSharedPtr<FJsonObject> Verification = MakeShared<FJsonObject>();
        Verification->SetStringField(TEXT("assetPath"), ActualPath);
        AddAssetVerification(Verification, Object);
        JsonBuilders::MergeMissingFields(Entry, Verification);
        if (bIsPrimary)
        {
            PrimaryVerification = Verification;
        }
        Results.Add(MakeShared<FJsonValueObject>(Entry));
    }

    Response.Data->SetNumberField(TEXT("resultCount"), Results.Num());
    Response.Data->SetArrayField(TEXT("results"), Results);
    if (PrimaryVerification)
    {
        JsonBuilders::MergeMissingFields(Response.Data, PrimaryVerification);
    }
    else if (PrimaryEntry)
    {
        FString PrimaryPath;
        PrimaryEntry->TryGetStringField(TEXT("assetPath"), PrimaryPath);
        Response.Data->SetStringField(TEXT("assetPath"), PrimaryPath);
    }
    if (bInvalidOutput || !PrimaryEntry.IsValid())
    {
        Response.Data->SetBoolField(TEXT("success"), false);
        Response.ErrorCode = ErrorCodes::ERR_IMPORT_FAILED;
        return Response;
    }

    Response.bSuccess = !bRenameRequested || bRenameSucceeded;
    Response.Data->SetBoolField(TEXT("success"), Response.bSuccess);
    if (!Response.bSuccess)
    {
        Response.ErrorCode = ErrorCodes::ERR_RENAME_FAILED;
    }
    return Response;
}

AssetImportPolicy::FResponse AssetImportPolicy::BuildInPlaceResponse(
    UObject* ExistingObject,
    const FDestinationSnapshot& Before,
    const FReimportOutcome& ReimportOutcome,
    bool bPostconditionsSucceeded,
    const FString& FailureReason)
{
    FResponse Response;
    Response.bSuccess =
        ReimportOutcome.bSucceeded && bPostconditionsSucceeded;
    Response.ErrorCode = Response.bSuccess
        ? FString()
        : FString(ErrorCodes::ERR_IMPORT_FAILED);
    Response.Data = MakeShared<FJsonObject>();
    Response.Data->SetBoolField(TEXT("success"), Response.bSuccess);

    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    const FString AssetPath = IsValid(ExistingObject)
        ? ExistingObject->GetPathName()
        : FString();
    const FObjectFingerprint* Expected =
        Before.ObjectFingerprints.Find(FName(*AssetPath));
    const bool bIdentityPreserved = IsValid(ExistingObject)
        && Expected
        && Expected->Identity == ExistingObject
        && Expected->ObjectClass == ExistingObject->GetClass()
        && Expected->Package == ExistingObject->GetOutermost();
    const UTexture2D* Texture = Cast<UTexture2D>(ExistingObject);
    const bool bContentChangeMeasured = Texture && Expected
        && Expected->bTextureContentIdValid
        && Texture->Source.GetId().IsValid();
    const bool bTextureContentChanged = bContentChangeMeasured
        && Texture->Source.GetId() != Expected->TextureContentId;
    Entry->SetStringField(TEXT("assetPath"), AssetPath);
    Entry->SetBoolField(TEXT("valid"), IsValid(ExistingObject));
    Entry->SetBoolField(TEXT("resolved"), bIdentityPreserved);
    Entry->SetBoolField(TEXT("existedBefore"), true);
    Entry->SetBoolField(TEXT("replaced"), false);
    Entry->SetBoolField(TEXT("replacementMeasured"), true);
    Entry->SetStringField(
        TEXT("replacementEvidence"),
        bIdentityPreserved ? TEXT("sameIdentity")
                           : TEXT("identityVerificationFailed"));
    Entry->SetStringField(TEXT("mode"), TEXT("inPlaceReimport"));
    Entry->SetBoolField(TEXT("identityPreserved"), bIdentityPreserved);
    Entry->SetBoolField(TEXT("contentChangeMeasured"), bContentChangeMeasured);
    Entry->SetBoolField(TEXT("textureContentChanged"), bTextureContentChanged);
    Entry->SetBoolField(TEXT("updatedInPlace"), Response.bSuccess);
    AddAssetSaveReport(
        Entry, /*bSaveRequested=*/false, /*bSavedToDisk=*/false,
        EAssetSaveState::NotRequested);
    if (IsValid(ExistingObject))
    {
        AddAssetVerification(Entry, ExistingObject);
    }
    if (!Response.bSuccess)
    {
        const FString EffectiveFailure = !FailureReason.IsEmpty()
            ? FailureReason : ReimportOutcome.FailureReason;
        const bool bFailureMayHaveMutatedContent =
            ReimportOutcome.bFailureMayHaveMutatedContent
            || ReimportOutcome.bSucceeded || bTextureContentChanged;
        Entry->SetStringField(TEXT("failureReason"), EffectiveFailure);
        Entry->SetBoolField(
            TEXT("sourceMetadataRestored"),
            ReimportOutcome.bSourceMetadataRestored);
        Entry->SetBoolField(
            TEXT("packageDirtyStateRestored"),
            ReimportOutcome.bDirtyStateRestored);
        Entry->SetBoolField(
            TEXT("adapterInvoked"),
            ReimportOutcome.bAdapterInvoked);
        Entry->SetBoolField(
            TEXT("failureOccurredBeforeContentMutation"),
            ReimportOutcome.bFailureOccurredBeforeContentMutation);
        Entry->SetBoolField(
            TEXT("failureMayHaveMutatedAsset"),
            bFailureMayHaveMutatedContent);
        Entry->SetBoolField(
            TEXT("failureMayHaveMutatedContent"),
            bFailureMayHaveMutatedContent);
        Entry->SetBoolField(
            TEXT("packageLeftDirtyForPossibleMutation"),
            ReimportOutcome.bPackageLeftDirtyForPossibleMutation
                || (bFailureMayHaveMutatedContent
                    && IsValid(ExistingObject)
                    && ExistingObject->GetOutermost()->IsDirty()));
        Entry->SetBoolField(
            TEXT("inMemoryContentRestored"),
            ReimportOutcome.bInMemoryContentRestored);
    }

    TArray<TSharedPtr<FJsonValue>> Results;
    Results.Add(MakeShared<FJsonValueObject>(Entry));
    Response.Data->SetStringField(TEXT("assetPath"), AssetPath);
    Response.Data->SetStringField(TEXT("mode"), TEXT("inPlaceReimport"));
    Response.Data->SetBoolField(TEXT("identityPreserved"), bIdentityPreserved);
    Response.Data->SetBoolField(TEXT("updatedInPlace"), Response.bSuccess);
    Response.Data->SetNumberField(TEXT("resultCount"), 1);
    Response.Data->SetArrayField(TEXT("results"), MoveTemp(Results));
    JsonBuilders::MergeMissingFields(Response.Data, Entry);
    return Response;
}
