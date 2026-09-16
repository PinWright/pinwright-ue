// Copyright (c) 2026 Alexander Penkin. MIT License.

// Asset management handlers: import, duplicate, rename, move, delete, create folder,
// exists, get, list, search, validate.
// Migrated from PinWright_AssetWorkflowHandlers.cpp

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/Asset/AssetImportHandler.h"
#include "Handlers/Asset/AssetPathParamUtils.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Handlers/Asset/AssetManageHandlerTestHooks.h"
#endif
#include "Dispatch/SafePoint.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/AssetBatchResult.h"
#include "Utils/AssetImportPolicy.h"
#include "Utils/AssetUtils.h"
#include "Utils/AssetDeletePolicy.h"
#include "Utils/JsonBuilders.h"
#include "Utils/PieState.h"
#include "Utils/StringUtils.h"

#include "AutomatedAssetImportData.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Misc/EngineVersionComparison.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorAssetLibrary.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Engine/Blueprint.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

// ============================================================================
// Internal helpers
// ============================================================================
namespace
{

// Query the asset registry for packages that hard-reference AssetPath.
// AssetPath may be either a package path (/Game/Foo/Bar) or an object path
// (/Game/Foo/Bar.Bar). The package path is derived by stripping the object
// suffix. Self-references are excluded. Caller must be on the game thread.
static TArray<FString> GetReferencingPackages(const FString& AssetPath)
{
    TArray<FString> Result;
    // Derive the package name: strip everything from the first '.' onward.
    FString PackageName = AssetPath;
    int32 DotIndex;
    if (PackageName.FindChar(TEXT('.'), DotIndex))
    {
        PackageName = PackageName.Left(DotIndex);
    }

    IAssetRegistry& Registry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    TArray<FName> Referencers;
    Registry.GetReferencers(
        FName(*PackageName),
        Referencers,
        UE::AssetRegistry::EDependencyCategory::Package,
        UE::AssetRegistry::FDependencyQuery());
    for (const FName& Ref : Referencers)
    {
        const FString RefStr = Ref.ToString();
        if (RefStr != PackageName)
        {
            Result.AddUnique(RefStr);
        }
    }
    return Result;
}

// Derive the package name from an asset path: /Game/Foo/Bar.Bar -> /Game/Foo/Bar.
// Both forms reach the delete verb, whose `path` param accepts either.
static FString AssetPathToPackageName(const FString& AssetPath)
{
    FString PackageName = AssetPath;
    int32 DotIndex;
    if (PackageName.FindChar(TEXT('.'), DotIndex))
    {
        PackageName = PackageName.Left(DotIndex);
    }
    return PackageName;
}

// True when a content file for AssetPath's package is still on disk.
// The PIE-safe asset resolver answers a different question - it reads the asset REGISTRY -
// and the engine's delete path can unload a package and drop its registry row
// while leaving the file behind (a read-only .uasset the engine could not remove takes
// exactly that route). A registry-only post-check reads that surviving file as a clean
// delete. AssetUtils::DoesPackageFileExistOnDisk is the shared mount-aware probe (both
// content extensions, no Fatal on an unmounted root).
static bool DoesAssetPackageFileExistOnDisk(const FString& AssetPath)
{
    return DoesPackageFileExistOnDisk(AssetPathToPackageName(AssetPath));
}

// Return the path the post-operation probe actually observed. Prefer the loaded object, then
// the registry row, and use only the package name for a disk-only hit; an unresolved object path
// must not turn a failed read-back into another request echo.
static FString GetAssetReadbackPath(const FResolvedAsset& Readback)
{
    if (Readback.Object)
    {
        return Readback.Object->GetPathName();
    }
    if (Readback.AssetData.IsValid())
    {
        return Readback.AssetData.GetObjectPathString();
    }
    return Readback.bPackageFileExists ? Readback.PackageName.ToString() : FString();
}

static bool IsRedirectorReadback(const FResolvedAsset& Readback)
{
    return (Readback.AssetData.IsValid() && Readback.AssetData.IsRedirector())
        || FAssetData::IsRedirector(Readback.Object);
}

// Keep the postcondition evidence in the response, including on an error. The registry and disk
// probes can disagree during editor operations, so callers need both facts instead of a request
// echo that looks like proof of success.
static void AddAssetOperationReadback(
    const TSharedPtr<FJsonObject>& Result,
    const FString& SourcePath,
    const FString& DestinationPath,
    const FResolvedAsset& SourceReadback,
    const FResolvedAsset& DestinationReadback,
    bool bSourceAssetExistsAfter)
{
    if (!Result)
    {
        return;
    }

    const bool bSourceExistsOnDisk = DoesAssetPackageFileExistOnDisk(SourcePath);
    const bool bDestinationExistsOnDisk = DoesAssetPackageFileExistOnDisk(DestinationPath);
    const bool bDestinationExistsAfter = DestinationReadback.bRegistryOrMemoryExists
        || DestinationReadback.bPackageFileExists
        || bDestinationExistsOnDisk;

    Result->SetStringField(TEXT("sourceObservedPath"), GetAssetReadbackPath(SourceReadback));
    Result->SetBoolField(TEXT("sourceExistsAfter"), bSourceAssetExistsAfter);
    Result->SetBoolField(TEXT("sourceExistsOnDisk"), bSourceExistsOnDisk);
    Result->SetBoolField(TEXT("sourceIsRedirector"), IsRedirectorReadback(SourceReadback));
    Result->SetStringField(TEXT("destinationObservedPath"),
        GetAssetReadbackPath(DestinationReadback));
    Result->SetBoolField(TEXT("destinationExistsAfter"), bDestinationExistsAfter);
    Result->SetBoolField(TEXT("destinationExistsOnDisk"), bDestinationExistsOnDisk);
}

// True when the only thing still answering for AssetPath is an object the delete already
// finished with. ObjectTools::DeleteSingleObject clears RF_Public|RF_Standalone and notifies
// the registry, then CleanupAfterSuccessfulDelete removes the file and the registry row - but
// the UObject itself only goes on a GC, and the transaction buffer alone is enough to hold it
// past the collect that function runs. The PIE-safe asset resolver asks the registry with
// in-memory objects INCLUDED (GetAssetByObjectPath's bIncludeOnlyOnDiskAssets defaults to false),
// and the registry resolves that leftover object and answers "still there" for an asset
// whose row and file are both already gone - so a delete that fully succeeded reported
// existsAfter:true, deleted:false and success:false, and callers retried a delete that had
// nothing left to do. Measured on UE 5.4 with the full suite loaded, where the buffer is deep
// enough to keep the object alive; a short run collects it and the same code passes.
//
// UObject::IsAsset() is what separates the two cases: it is false for this leftover (no
// RF_Public), and true for a genuine unsaved in-memory asset, which must still count as
// surviving. The on-disk registry row is checked first so a merely-unloaded asset is never
// mistaken for a deleted one.
static bool IsDeletedAssetShellOnly(const FString& AssetPath)
{
    const FString PackageName = AssetPathToPackageName(AssetPath);
    FString AssetName;
    if (!PackageName.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::IgnoreCase,
            ESearchDir::FromEnd)
        || AssetName.IsEmpty())
    {
        return false;
    }

    const FSoftObjectPath ObjectPath(PackageName + TEXT(".") + AssetName);

    IAssetRegistry& Registry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    if (Registry.GetAssetByObjectPath(ObjectPath, /*bIncludeOnlyOnDiskAssets=*/true).IsValid())
    {
        return false;
    }

    const UObject* Leftover = ObjectPath.ResolveObject();
    return Leftover != nullptr && !Leftover->IsAsset();
}

// Name the in-memory holders keeping AssetPath's package alive, using the engine's own
// predicate: ObjectTools::CleanupAfterSuccessfulDelete calls
// GatherObjectReferencersForDeletion on the package and, when it reports a referencer,
// silently drops the package from its delete list - no unload, no file delete, no log line
// (ObjectTools.cpp, unchanged UE 5.3-5.8). GetReferencingPackages above answers the
// narrower on-disk question and returns nothing for a holder that exists only in memory,
// which is the case that actually blocks the delete. Internal referencers count too - any
// other RF_Standalone object inside the same package is what the engine culls on.
// Callers must keep this on the failure path: the gather walks every live UObject. It adds
// no new hazard there - the failed delete attempt already ran the same gather twice, in
// DeleteSingleObject and in CleanupAfterSuccessfulDelete.
static TArray<FString> GatherInMemoryPackageReferencers(const FString& AssetPath,
                                                        bool& bOutReferencedByUndo)
{
    TArray<FString> Result;
    bOutReferencedByUndo = false;

    UPackage* Package = FindPackage(nullptr, *AssetPathToPackageName(AssetPath));
    if (!Package)
    {
        return Result;
    }

    FReferencerInformationList Refs;
    bool bIsReferenced = false;
    ObjectTools::GatherObjectReferencersForDeletion(
        Package, bIsReferenced, bOutReferencedByUndo, &Refs);

    for (const FReferencerInformation& Ref : Refs.InternalReferences)
    {
        if (Ref.Referencer)
        {
            Result.AddUnique(Ref.Referencer->GetPathName());
        }
    }
    for (const FReferencerInformation& Ref : Refs.ExternalReferences)
    {
        if (Ref.Referencer)
        {
            Result.AddUnique(Ref.Referencer->GetPathName());
        }
    }
    return Result;
}

// Build a JSON array of strings from the given string list.
static TArray<TSharedPtr<FJsonValue>> MakeStringJsonArray(const TArray<FString>& Items)
{
    TArray<TSharedPtr<FJsonValue>> Array;
    for (const FString& Item : Items)
    {
        Array.Add(MakeShared<FJsonValueString>(Item));
    }
    return Array;
}

static bool RefuseAssetMutationDuringPie(FHandlerContext& Ctx, const TCHAR* Operation)
{
    if (!PinWrightPieState::IsPlayInEditorActive())
    {
        return false;
    }

    Ctx.SendError(ErrorCodes::ERR_PIE_ACTIVE,
        FString::Printf(TEXT("%s cannot run while the editor is in play mode; stop PIE and retry."),
            Operation));
    return true;
}

// Human-readable name for a Blueprint's compile state, reported by asset.reload so a
// caller learns whether the freshly-reloaded (from-disk) Blueprint still compiles.
static const TCHAR* BlueprintStatusToString(EBlueprintStatus Status)
{
    switch (Status)
    {
    case BS_UpToDate:             return TEXT("upToDate");
    case BS_Dirty:                return TEXT("dirty");
    case BS_Error:                return TEXT("error");
    case BS_BeingCreated:         return TEXT("beingCreated");
    case BS_UpToDateWithWarnings: return TEXT("upToDateWithWarnings");
    default:                      return TEXT("unknown");
    }
}

} // anonymous namespace

PinWrightAssetImportHandler::FResult PinWrightAssetImportHandler::Execute(
    const FRequest& Request,
    const AssetImportPolicy::FExecutionDependencies* Dependencies)
{
    const FString& SourcePath = Request.SourcePath;
    const FString& SourceName = Request.SourceName;
    const FString& DestPath = Request.DestinationPath;
    const FString& DestName = Request.DestinationName;
    const FString& RequestedAssetPath = Request.RequestedAssetPath;

    TArray<FString> DestinationPackages;
    FString DiscoveryError;
    if (!AssetImportPolicy::DiscoverDestinationPackages(
            DestPath, RequestedAssetPath,
            TArray<FString>{SourceName, DestName},
            DestinationPackages, DiscoveryError))
    {
        TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
        ErrorData->SetStringField(TEXT("assetPath"), RequestedAssetPath);
        ErrorData->SetStringField(TEXT("discoveryError"), DiscoveryError);
        return {
            false,
            ErrorCodes::ERR_OVERWRITE_UNSAFE,
            TEXT("Destination discovery could not prove the import target safe"),
            ErrorData};
    }

    const AssetImportPolicy::FDestinationDecision DestinationDecision =
        AssetImportPolicy::EvaluateDestination(
            !DestinationPackages.IsEmpty(), Request.bOverwrite);
    if (DestinationDecision.bRefused)
    {
        TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
        ErrorData->SetStringField(TEXT("assetPath"), RequestedAssetPath);
        ErrorData->SetBoolField(TEXT("overwrite"), false);
        ErrorData->SetArrayField(
            TEXT("conflicts"),
            JsonBuilders::BuildStringArrayJson(DestinationPackages));
        return {
            false,
            DestinationDecision.ErrorCode,
            FString::Printf(
                TEXT("Destination prefix is occupied for '%s'. Pass overwrite=true only for a supported same-object reimport."),
                *RequestedAssetPath),
            ErrorData};
    }

    if (!DestinationPackages.IsEmpty())
    {
        AssetImportPolicy::FValidatedTextureReimportPlan BuiltPlan;
        FString PlanError;
        if (!AssetImportPolicy::BuildValidatedTextureReimportPlan(
                SourcePath, BuiltPlan, PlanError))
        {
            UObject* ExistingLiveObject = StaticFindObject(
                UObject::StaticClass(), nullptr, *RequestedAssetPath);
            return {
                false,
                ErrorCodes::ERR_OVERWRITE_UNSAFE,
                TEXT("The occupied destination cannot use the bounded in-place texture lane"),
                AssetImportPolicy::BuildUnsafeOverwriteData(
                    RequestedAssetPath, SourcePath, ExistingLiveObject,
                    PlanError, DestinationPackages)};
        }
        const AssetImportPolicy::FValidatedTextureReimportPlan ValidatedPlan =
            MoveTemp(BuiltPlan);

        FString UnsafeReason;
        UObject* ResolvedObject = nullptr;
        UObject* ExistingObject =
            AssetImportPolicy::ResolveSafeInPlaceReimport(
                RequestedAssetPath, DestinationPackages,
                ResolvedObject, UnsafeReason);
        if (!ExistingObject)
        {
            return {
                false,
                ErrorCodes::ERR_OVERWRITE_UNSAFE,
                TEXT("The occupied destination cannot be updated without changing or risking its object identity"),
                AssetImportPolicy::BuildUnsafeOverwriteData(
                    RequestedAssetPath, SourcePath, ResolvedObject,
                    UnsafeReason, DestinationPackages)};
        }

        TStrongObjectPtr<UTexture2D> RetainedExisting(
            CastChecked<UTexture2D>(ExistingObject));
        AssetImportPolicy::FDestinationSnapshot Before;
        FString SnapshotError;
        if (!AssetImportPolicy::CaptureDestinationSnapshot(
                DestinationPackages, Before, SnapshotError))
        {
            return {
                false,
                ErrorCodes::ERR_OVERWRITE_UNSAFE,
                TEXT("The occupied destination could not be captured for safe in-place verification"),
                AssetImportPolicy::BuildUnsafeOverwriteData(
                    RequestedAssetPath, SourcePath, ExistingObject,
                    SnapshotError, DestinationPackages)};
        }

        AssetImportPolicy::FReimportOutcome ReimportOutcome =
            AssetImportPolicy::RunReimport(
                RetainedExisting.Get(), ValidatedPlan, Dependencies);
        FString ReimportFailure;
        bool bVerified = false;
        if (ReimportOutcome.bSucceeded)
        {
            bVerified = Dependencies && Dependencies->PostconditionVerifier
                ? Dependencies->PostconditionVerifier(
                    RetainedExisting.Get(), RequestedAssetPath,
                    Before, ReimportFailure)
                : AssetImportPolicy::VerifyInPlacePostconditions(
                    RetainedExisting.Get(), RequestedAssetPath,
                    Before, ReimportFailure);
            if (!bVerified)
            {
                if (ReimportFailure.IsEmpty())
                {
                    ReimportFailure =
                        TEXT("the in-place reimport postconditions failed");
                }
                AssetImportPolicy::RestoreFailedReimportState(
                    RetainedExisting.Get(),
                    ReimportOutcome.RecoverySnapshot,
                    /*bFailureMayHaveMutatedContent=*/true,
                    ReimportOutcome);
            }
        }
        else
        {
            ReimportFailure = ReimportOutcome.FailureReason;
        }

        AssetImportPolicy::FResponse ReimportResponse =
            AssetImportPolicy::BuildInPlaceResponse(
                RetainedExisting.Get(), Before,
                ReimportOutcome, bVerified, ReimportFailure);
        if (!ReimportResponse.bSuccess)
        {
            return {
                false,
                ReimportResponse.ErrorCode,
                FString::Printf(
                    TEXT("Could not safely reimport '%s' in place"),
                    *RequestedAssetPath),
                ReimportResponse.Data};
        }
        return {
            true, FString(), TEXT("Asset reimported in place"),
            ReimportResponse.Data};
    }

    TStrongObjectPtr<UAutomatedAssetImportData> ImportData(
        NewObject<UAutomatedAssetImportData>());
    AssetImportPolicy::ConfigureImportData(*ImportData.Get());
    ImportData->DestinationPath = DestPath;
    ImportData->Filenames = TArray<FString>{SourcePath};

    TArray<UObject*> ImportedAssets =
        AssetImportPolicy::RunImport(ImportData.Get(), Dependencies);
    UObject* PrimaryAsset = nullptr;
    for (UObject* ImportedObject : ImportedAssets)
    {
        if (ImportedObject)
        {
            PrimaryAsset = ImportedObject;
            break;
        }
    }

    FString PrimaryPathBeforeRename;
    bool bRenameRequested = false;
    bool bRenameSucceeded = true;
    if (IsValid(PrimaryAsset))
    {
        PrimaryPathBeforeRename = PrimaryAsset->GetPathName();
        bRenameRequested = PrimaryAsset->GetName() != DestName;
        if (bRenameRequested)
        {
            FString RenameError;
            bRenameSucceeded = AssetImportPolicy::PublishPrimaryOutput(
                PrimaryAsset, DestPath / DestName, DestName, RenameError);
        }
    }

    AssetImportPolicy::FResponse ImportResponse =
        AssetImportPolicy::BuildResponse(
            ImportedAssets,
            AssetImportPolicy::FDestinationSnapshot(),
            PrimaryPathBeforeRename, bRenameRequested, bRenameSucceeded);
    if (!ImportResponse.bSuccess)
    {
        const FString FailureMessage =
            ImportResponse.ErrorCode == ErrorCodes::ERR_IMPORT_FAILED
            ? FString::Printf(TEXT("Failed to import asset from '%s'"), *SourcePath)
            : FString::Printf(
                TEXT("Asset imported from '%s', but the primary output could not be renamed to '%s'. Actual imported paths are in results[]."),
                *SourcePath, *RequestedAssetPath);
        return {
            false, ImportResponse.ErrorCode,
            FailureMessage, ImportResponse.Data};
    }
    return {true, FString(), TEXT("Asset imported"), ImportResponse.Data};
}

// ============================================================================
// asset.import
// ============================================================================
REGISTER_RPC_HANDLER("asset.import", "asset", "Import a file from the local filesystem into the content browser using the matching UFactory (FBX, PNG, WAV, etc. — picked by extension). Equivalent of File > Import in the editor.",
    RPC_PARAMS(
        RPC_PARAM_REQ("sourcePath", "filepath", "Absolute or project-relative path to the source file on the local filesystem (e.g. 'C:/Art/foo.fbx')."),
        RPC_PARAM_REQ("destinationPath", "path", "Target /Game-relative folder where the imported asset(s) should be placed (e.g. '/Game/Imported')."),
        RPC_PARAM_OPT("overwrite", "boolean", "Allow a proven same-object texture reimport at one exact occupied destination. Defaults to false; every identity-changing or unsupported overwrite is refused before import.")
    ))
{
    FString DestinationPath = Ctx.GetString(TEXT("destinationPath"));
    FString SourcePath = Ctx.GetString(TEXT("sourcePath"));
    const bool bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);

    if (DestinationPath.IsEmpty() || SourcePath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("sourcePath and destinationPath required"));
        return true;
    }

    if (!FPaths::FileExists(SourcePath))
    {
        Ctx.SendError(ErrorCodes::ERR_SOURCE_NOT_FOUND,
            FString::Printf(TEXT("Source file not found: %s"), *SourcePath));
        return true;
    }

    FString SafeDestPath = SanitizeProjectRelativePath(DestinationPath);
    if (SafeDestPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, TEXT("Invalid destination path"));
        return true;
    }

    FString DestPath = FPaths::GetPath(SafeDestPath);
    FString DestName = FPaths::GetBaseFilename(SafeDestPath);

    if (FPaths::GetExtension(SafeDestPath).IsEmpty())
    {
        DestPath = SafeDestPath;
        DestName = FPaths::GetBaseFilename(SourcePath);
    }

    DestName = ObjectTools::SanitizeObjectName(DestName);
    if (DestName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, TEXT("Invalid destination asset name"));
        return true;
    }
    const FString SourceName = ObjectTools::SanitizeObjectName(
        FPaths::GetBaseFilename(SourcePath));
    const FString RequestedAssetPath = DestPath / DestName + TEXT(".") + DestName;

    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available for deferred import"));
        return true;
    }

    // UE 5.7+ Interchange import must start on a later stack to avoid TaskGraph
    // recursion. Keep the originating request serialized through that hop.
    return PinWrightSafePoint::DeferRequestToSafePoint(
        Ctx, TEXT("asset.import"),
        [SourcePath, SourceName, DestPath, DestName, RequestedAssetPath, bOverwrite](
            const PinWrightSafePoint::FSafePointResponder& Responder)
        {
            PinWrightAssetImportHandler::FRequest Request;
            Request.SourcePath = SourcePath;
            Request.SourceName = SourceName;
            Request.DestinationPath = DestPath;
            Request.DestinationName = DestName;
            Request.RequestedAssetPath = RequestedAssetPath;
            Request.bOverwrite = bOverwrite;
            PinWrightAssetImportHandler::FResult Result =
                PinWrightAssetImportHandler::Execute(Request);
            if (Result.bSuccess)
            {
                Responder.SendSuccess(Result.Message, Result.Data);
            }
            else
            {
                Responder.SendError(
                    Result.ErrorCode, Result.Message, Result.Data);
            }
        });
}

// ============================================================================
// asset.duplicate
// ============================================================================
REGISTER_RPC_HANDLER("asset.duplicate", "asset", "Duplicate an asset or folder",
    RPC_PARAMS(
        RPC_PARAM_REQ("sourcePath", "path", "Source asset or folder path"),
        RPC_PARAM_REQ("destinationPath", "path", "Destination path"),
        RPC_PARAM_OPT("partial", "boolean", "For folder duplication, duplicate valid children when another child fails preflight. Defaults to false, which creates no destination before the full folder batch passes preflight.")
    ))
{
    FString SourcePath = Ctx.GetString(TEXT("sourcePath"));
    FString DestinationPath = Ctx.GetString(TEXT("destinationPath"));

    if (SourcePath.IsEmpty() || DestinationPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("sourcePath and destinationPath required"));
        return true;
    }

    if (RefuseAssetMutationDuringPie(Ctx, TEXT("asset.duplicate")))
    {
        return true;
    }

    // Auto-resolve simple name for destination
    if (!DestinationPath.IsEmpty() && FPaths::GetPath(DestinationPath).IsEmpty())
    {
        FString ParentDir = FPaths::GetPath(SourcePath);
        if (ParentDir.IsEmpty() || ParentDir == TEXT("/"))
            ParentDir = TEXT("/Game");

        DestinationPath = ParentDir / DestinationPath;
        UE_LOG(LogPinWrightSubsystem, Display,
               TEXT("asset.duplicate: Auto-resolved simple name destination to '%s'"),
               *DestinationPath);
    }

    // Deep duplication for directories
    if (DoesAssetDirectoryExist(SourcePath))
    {
        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*SourcePath));
        Filter.bRecursivePaths = true;

        TArray<FAssetData> Assets;
        AssetRegistryModule.Get().GetAssets(Filter, Assets);
        Assets.Sort([](const FAssetData& Left, const FAssetData& Right)
        {
            return Left.PackageName.ToString() < Right.PackageName.ToString();
        });

        // Gather referencers across all source assets (deduplicated) before duplicating.
        TArray<FString> AggregatedReferencers;
        for (const FAssetData& Asset : Assets)
        {
            for (const FString& Ref : GetReferencingPackages(Asset.PackageName.ToString()))
            {
                AggregatedReferencers.AddUnique(Ref);
            }
        }

        const bool bAllowPartial = Ctx.GetBool(TEXT("partial"), false);
        PinWrightAssetBatch::FAssetBatchResult Batch(bAllowPartial);

        struct FDuplicateFolderWork
        {
            int32 OutcomeIndex = INDEX_NONE;
            FString SourceAssetPath;
            FString TargetAssetPath;
            FString TargetFolderPath;
        };

        TArray<FDuplicateFolderWork> Work;
        Work.Reserve(Assets.Num());
        TSet<FName> RequestedTargets;
        RequestedTargets.Reserve(Assets.Num());
        for (const FAssetData& Asset : Assets)
        {
            const FString SourceAssetPath = Asset.PackageName.ToString();
            const int32 OutcomeIndex = Batch.AddItem();
            TSharedPtr<FJsonObject> Item = Batch.GetItem(OutcomeIndex);
            Item->SetStringField(TEXT("sourcePath"), SourceAssetPath);

            FString RelativePath;
            if (SourceAssetPath.StartsWith(SourcePath, ESearchCase::IgnoreCase))
            {
                RelativePath = SourceAssetPath.RightChop(SourcePath.Len());
                while (RelativePath.StartsWith(TEXT("/")))
                {
                    RelativePath.RightChopInline(1);
                }
            }
            else
            {
                Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_INVALID_PATH,
                    FString::Printf(TEXT("Discovered asset %s is outside source folder %s"),
                        *SourceAssetPath, *SourcePath));
                continue;
            }

            const FString TargetAssetPath = DestinationPath / RelativePath;
            Item->SetStringField(TEXT("destinationPath"), TargetAssetPath);
            const FString TargetFolderPath = FPackageName::GetLongPackagePath(TargetAssetPath);

            FText ValidationReason;
            if (!FPackageName::IsValidLongPackageName(TargetAssetPath,
                    /*bIncludeReadOnlyRoots=*/true, &ValidationReason))
            {
                Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_INVALID_PATH,
                    FString::Printf(TEXT("Invalid duplicate destination %s: %s"),
                        *TargetAssetPath, *ValidationReason.ToString()));
                continue;
            }

            // FName comparison canonicalizes package-path casing, so two discovered inputs
            // cannot race toward the same derived destination under partial:true either.
            const FName TargetKey(*TargetAssetPath);
            if (RequestedTargets.Contains(TargetKey))
            {
                Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_DESTINATION_EXISTS,
                    FString::Printf(TEXT("More than one folder item targets %s"),
                        *TargetAssetPath));
                continue;
            }
            RequestedTargets.Add(TargetKey);

            if (ResolveAsset(TargetAssetPath).bExists
                || DoesPackageFileExistOnDisk(TargetAssetPath))
            {
                Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_DESTINATION_EXISTS,
                    FString::Printf(TEXT("Duplicate destination already exists: %s"),
                        *TargetAssetPath));
                continue;
            }

            Batch.MarkReady(OutcomeIndex);
            FDuplicateFolderWork& Entry = Work.AddDefaulted_GetRef();
            Entry.OutcomeIndex = OutcomeIndex;
            Entry.SourceAssetPath = SourceAssetPath;
            Entry.TargetAssetPath = TargetAssetPath;
            Entry.TargetFolderPath = TargetFolderPath;
        }

        auto AddFolderDuplicateSummary = [&](const TSharedPtr<FJsonObject>& Result,
                                             int32 DuplicatedCount)
        {
            Result->SetStringField(TEXT("sourcePath"), SourcePath);
            Result->SetStringField(TEXT("destinationPath"), DestinationPath);
            Result->SetNumberField(TEXT("duplicatedCount"), DuplicatedCount);
            Result->SetArrayField(TEXT("referencingBlueprints"),
                MakeStringJsonArray(AggregatedReferencers));
            if (!AggregatedReferencers.IsEmpty())
            {
                Result->SetStringField(TEXT("warning"),
                    TEXT("One or more source assets are referenced by other packages. "
                         "Those packages still point at the original source assets."));
            }
        };

        if (Assets.Num() == 0)
        {
            TSharedPtr<FJsonObject> Result = Batch.MakeResult();
            AddFolderDuplicateSummary(Result, 0);
            Ctx.SendError(ErrorCodes::ERR_DUPLICATE_FAILED,
                TEXT("Source folder contains no assets"), Result);
            return true;
        }

        if (Batch.ShouldRefuseBeforeMutation())
        {
            Batch.RefuseReadyItems(ErrorCodes::ERR_BATCH_NOT_ATTEMPTED,
                TEXT("Not attempted because another batch item failed preflight"));
            TSharedPtr<FJsonObject> Result = Batch.MakeResult();
            AddFolderDuplicateSummary(Result, 0);
            Ctx.SendError(ErrorCodes::ERR_BATCH_PREFLIGHT_FAILED,
                TEXT("Folder duplicate failed preflight; no destination was created"), Result);
            return true;
        }

        int32 DuplicatedCount = 0;
        for (const FDuplicateFolderWork& Entry : Work)
        {
            if (!Batch.IsReady(Entry.OutcomeIndex))
            {
                continue;
            }

            if (!Entry.TargetFolderPath.IsEmpty()
                && !DoesAssetDirectoryExist(Entry.TargetFolderPath)
                && !UEditorAssetLibrary::MakeDirectory(Entry.TargetFolderPath))
            {
                Batch.FailRuntime(Entry.OutcomeIndex, ErrorCodes::ERR_CREATE_FAILED,
                    FString::Printf(TEXT("Could not create destination folder: %s"),
                        *Entry.TargetFolderPath));
                continue;
            }

            UObject* DuplicatedAsset = UEditorAssetLibrary::DuplicateAsset(
                Entry.SourceAssetPath, Entry.TargetAssetPath);
            const FResolvedAsset Readback = ResolveAsset(
                Entry.TargetAssetPath, /*bLoadObject=*/true);
            const FString ActualPath = DuplicatedAsset
                ? DuplicatedAsset->GetPathName()
                : FString();
            const bool bDuplicated = DuplicatedAsset && Readback.bExists
                && DuplicatedAsset->GetOutermost()->GetName() == Entry.TargetAssetPath;

            TSharedPtr<FJsonObject> Item = Batch.GetItem(Entry.OutcomeIndex);
            Item->SetStringField(TEXT("actualPath"), ActualPath);
            Item->SetBoolField(TEXT("existsAfter"), Readback.bExists);
            if (bDuplicated)
            {
                Batch.MarkSucceeded(Entry.OutcomeIndex);
                ++DuplicatedCount;
            }
            else
            {
                Batch.FailRuntime(Entry.OutcomeIndex, ErrorCodes::ERR_DUPLICATE_FAILED,
                    FString::Printf(TEXT("Duplicate was not observed at %s"),
                        *Entry.TargetAssetPath));
            }
        }

        TSharedPtr<FJsonObject> Result = Batch.MakeResult();
        AddFolderDuplicateSummary(Result, DuplicatedCount);
        if (Batch.IsEnvelopeSuccess())
        {
            Ctx.SendSuccess(Result);
        }
        else
        {
            Ctx.SendError(ErrorCodes::ERR_DUPLICATE_FAILED,
                TEXT("One or more folder assets were not duplicated; see items"), Result);
        }
        return true;
    }

    // Single-asset duplication
    const FResolvedAsset SourceBefore = ResolveAsset(
        SourcePath, /*bLoadObject=*/true);
    if (!SourceBefore.bExists)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("Source asset not found: %s"), *SourcePath));
        return true;
    }

    if (ResolveAsset(DestinationPath).bExists)
    {
        Ctx.SendError(ErrorCodes::ERR_DESTINATION_EXISTS,
            FString::Printf(TEXT("Destination asset already exists: %s"), *DestinationPath));
        return true;
    }

    // Query referencers before duplicating so the registry is still intact.
    TArray<FString> Referencers = GetReferencingPackages(SourcePath);

    UObject* DuplicatedAsset = UEditorAssetLibrary::DuplicateAsset(SourcePath, DestinationPath);
#if WITH_DEV_AUTOMATION_TESTS
    FResolvedAsset DestinationReadback = ResolveAsset(
        DestinationPath, /*bLoadObject=*/true);
    if (PinWrightAssetManageTestHooks::ConsumeForceMissingDestinationReadback())
    {
        DestinationReadback = FResolvedAsset();
    }
#else
    const FResolvedAsset DestinationReadback = ResolveAsset(
        DestinationPath, /*bLoadObject=*/true);
#endif
    const FString SourceObjectPath = SourceBefore.ObjectPath.IsValid()
        ? SourceBefore.ObjectPath.ToString()
        : SourcePath;
    const FResolvedAsset SourceReadback = ResolveAsset(
        SourceObjectPath, /*bLoadObject=*/true);
    const bool bDestinationExistsOnDisk = DoesAssetPackageFileExistOnDisk(DestinationPath);
    const bool bDestinationExistsAfter = DestinationReadback.bRegistryOrMemoryExists
        || DestinationReadback.bPackageFileExists
        || bDestinationExistsOnDisk;
    const bool bDestinationAtRequestedPath = DestinationReadback.Object
        && DestinationReadback.Object->GetOutermost()
        && DestinationReadback.Object->GetOutermost()->GetName().Equals(
            AssetPathToPackageName(DestinationPath), ESearchCase::IgnoreCase);
    const bool bSourceExistsAfter = SourceReadback.bRegistryOrMemoryExists
        || SourceReadback.bPackageFileExists
        || DoesAssetPackageFileExistOnDisk(SourceObjectPath);

    TSharedPtr<FJsonObject> Readback = MakeShared<FJsonObject>();
    AddAssetOperationReadback(Readback, SourceObjectPath, DestinationPath,
        SourceReadback, DestinationReadback, bSourceExistsAfter);
    if (!DuplicatedAsset || !DestinationReadback.Object || !bDestinationExistsAfter
        || !bDestinationAtRequestedPath || !bSourceExistsAfter)
    {
        Ctx.SendError(ErrorCodes::ERR_DUPLICATE_FAILED,
            FString::Printf(TEXT("Duplicate did not satisfy its postcondition at '%s'; "
                                 "readback fields report registry and disk state."),
                *DestinationPath), Readback);
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("assetPath"), DestinationReadback.Object->GetPathName());
    AddAssetVerification(Resp, DestinationReadback.Object);
    AddAssetOperationReadback(Resp, SourceObjectPath, DestinationPath,
        SourceReadback, DestinationReadback, bSourceExistsAfter);
    Resp->SetArrayField(TEXT("referencingBlueprints"),
        MakeStringJsonArray(Referencers));
    if (!Referencers.IsEmpty())
    {
        Resp->SetStringField(TEXT("warning"),
            TEXT("The source asset is referenced by other packages. "
                 "Those packages still point at the original source asset."));
    }
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// asset.rename
// ============================================================================
REGISTER_RPC_HANDLER("asset.rename", "asset", "Rename an asset and create a redirector at the old path so existing references keep resolving. Use asset.fixup_redirectors afterwards to re-save referencing assets and remove the redirector.",
    RPC_PARAMS(
        RPC_PARAM_REQ("sourcePath", "path", "Asset path of the existing asset to rename (e.g. /Game/Foo/SM_Old)."),
        RPC_PARAM_REQ("destinationPath", "path", "Either a new full asset path (/Game/Foo/SM_New) or just the new name segment if the folder is unchanged.")
    ))
{
    FString SourcePath = Ctx.GetString(TEXT("sourcePath"));
    FString DestinationPath = Ctx.GetString(TEXT("destinationPath"));

    if (SourcePath.IsEmpty() || DestinationPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("sourcePath and destinationPath required"));
        return true;
    }

    if (RefuseAssetMutationDuringPie(Ctx, TEXT("asset.rename")))
    {
        return true;
    }

    // Auto-resolve simple name for destination
    if (!DestinationPath.IsEmpty() && FPaths::GetPath(DestinationPath).IsEmpty())
    {
        FString ParentDir = FPaths::GetPath(SourcePath);
        if (ParentDir.IsEmpty() || ParentDir == TEXT("/"))
            ParentDir = TEXT("/Game");

        DestinationPath = ParentDir / DestinationPath;
        UE_LOG(LogPinWrightSubsystem, Display,
               TEXT("asset.rename: Auto-resolved simple name destination to '%s'"),
               *DestinationPath);
    }

    FString ResolvedSourcePath = ResolveAssetPath(SourcePath);
    if (ResolvedSourcePath.IsEmpty())
    {
        ResolvedSourcePath = SourcePath;
    }

    const FResolvedAsset SourceBefore = ResolveAsset(
        ResolvedSourcePath, /*bLoadObject=*/true);
    if (!SourceBefore.bExists)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("Source asset not found: %s"), *SourcePath));
        return true;
    }

    const FString SourceObjectPath = SourceBefore.ObjectPath.IsValid()
        ? SourceBefore.ObjectPath.ToString()
        : ResolvedSourcePath;

    UObject* RenamedAsset = nullptr;
    const bool bRenameReportedSuccess = UEditorAssetLibrary::RenameAsset(
        ResolvedSourcePath, DestinationPath);
#if WITH_DEV_AUTOMATION_TESTS
    FResolvedAsset DestinationReadback = ResolveAsset(
        DestinationPath, /*bLoadObject=*/true);
    if (PinWrightAssetManageTestHooks::ConsumeForceMissingDestinationReadback())
    {
        DestinationReadback = FResolvedAsset();
    }
#else
    const FResolvedAsset DestinationReadback = ResolveAsset(
        DestinationPath, /*bLoadObject=*/true);
#endif
    const FResolvedAsset SourceReadback = ResolveAsset(
        SourceObjectPath, /*bLoadObject=*/false);
    RenamedAsset = DestinationReadback.Object;

    const bool bDestinationExistsOnDisk = DoesAssetPackageFileExistOnDisk(DestinationPath);
    const bool bDestinationExistsAfter = DestinationReadback.bRegistryOrMemoryExists
        || DestinationReadback.bPackageFileExists
        || bDestinationExistsOnDisk;
    const bool bDestinationAtRequestedPath = DestinationReadback.Object
        && DestinationReadback.Object->GetOutermost()
        && DestinationReadback.Object->GetOutermost()->GetName().Equals(
            AssetPathToPackageName(DestinationPath), ESearchCase::IgnoreCase);
    const bool bSourceIsRedirector = IsRedirectorReadback(SourceReadback);
    const bool bSourceExistsOnDisk = DoesAssetPackageFileExistOnDisk(SourceObjectPath);
    const bool bSourceAssetExistsAfter = !bSourceIsRedirector
        && (SourceReadback.bRegistryOrMemoryExists
            || SourceReadback.bPackageFileExists
            || bSourceExistsOnDisk);

    TSharedPtr<FJsonObject> Readback = MakeShared<FJsonObject>();
    AddAssetOperationReadback(Readback, SourceObjectPath, DestinationPath,
        SourceReadback, DestinationReadback, bSourceAssetExistsAfter);
    if (!bRenameReportedSuccess || !RenamedAsset || !bDestinationExistsAfter
        || !bDestinationAtRequestedPath || bSourceAssetExistsAfter)
    {
        Ctx.SendError(ErrorCodes::ERR_RENAME_FAILED,
            FString::Printf(TEXT("Rename did not satisfy its postcondition at '%s'; "
                                 "readback fields report registry and disk state."),
                *DestinationPath), Readback);
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("assetPath"), RenamedAsset->GetPathName());
    AddAssetVerification(Resp, RenamedAsset);
    AddAssetOperationReadback(Resp, SourceObjectPath, DestinationPath,
        SourceReadback, DestinationReadback, bSourceAssetExistsAfter);
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// asset.move (alias for rename in Unreal)
// ============================================================================
REGISTER_RPC_HANDLER("asset.move", "asset", "Move an asset to a different folder and leave a redirector at the old path. Use asset.fixup_redirectors afterwards to re-save referencing assets and remove the redirector. destinationPath is read as an existing folder, a full object path, or a bare rename; the response reports which under destinationInterpretedAs, and assetPath is measured from the moved object instead of echoed from the request. A destinationPath whose folder does not exist is refused with DESTINATION_FOLDER_NOT_FOUND rather than guessed at, because guessing renames the asset after the missing folder.",
    RPC_PARAMS(
        RPC_PARAM_REQ("sourcePath", "path", "Existing asset path (e.g. /Game/Old/SM_Foo)."),
        RPC_PARAM_REQ("destinationPath", "path", "Where the asset should end up. Read one of three ways, and the response says which under destinationInterpretedAs: a bare name containing no '/' renames it inside its current folder ('nameOnly'); a path naming an EXISTING content folder (e.g. /Game/New) moves it into that folder under its current name ('folder'); anything else is a full destination object path (e.g. /Game/New/SM_Bar) whose parent folder must already exist ('objectPath'). When neither the path nor its parent is an existing folder both readings are wrong, so the call is refused with DESTINATION_FOLDER_NOT_FOUND and nothing is moved - create the folder with asset.create_folder first.")
    ))
{
    FString SourcePath = Ctx.GetString(TEXT("sourcePath"));
    const FString RequestedDestinationPath = Ctx.GetString(TEXT("destinationPath"));
    FString DestinationPath = RequestedDestinationPath;

    if (SourcePath.IsEmpty() || DestinationPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("sourcePath and destinationPath required"));
        return true;
    }

    if (RefuseAssetMutationDuringPie(Ctx, TEXT("asset.move")))
    {
        return true;
    }

    FString ResolvedSourcePath = ResolveAssetPath(SourcePath);
    if (ResolvedSourcePath.IsEmpty())
    {
        ResolvedSourcePath = SourcePath;
    }

    if (!ResolveAsset(ResolvedSourcePath).bExists)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("Source asset not found: %s"), *SourcePath));
        return true;
    }

    // How destinationPath is read, decided once and reported back.
    //
    // UEditorAssetLibrary::RenameAsset takes a full destination OBJECT path and unconditionally
    // splits the last segment off as the new object name (GetLongPackagePath +
    // ObjectPathToObjectName in UE::EditorAssetUtils::RenameLoadedAsset). Handing it a folder
    // path therefore does not move the asset into that folder - it renames the asset after the
    // folder. That is what the documented "when a folder is given, the asset retains its
    // original name" case used to do in practice, with nothing in the response saying so.
    const FString SourceLeafName =
        FPackageName::GetShortName(AssetPathToPackageName(ResolvedSourcePath));
    const TCHAR* DestinationInterpretedAs = TEXT("objectPath");
    const FString DestinationParentFolder = FPaths::GetPath(DestinationPath);

    if (DestinationParentFolder.IsEmpty())
    {
        // A bare name with no '/': rename in place, inside the source's own folder.
        FString ParentDir = FPaths::GetPath(SourcePath);
        if (ParentDir.IsEmpty() || ParentDir == TEXT("/"))
            ParentDir = TEXT("/Game");

        DestinationPath = ParentDir / DestinationPath;
        DestinationInterpretedAs = TEXT("nameOnly");
    }
    else if (DoesAssetDirectoryExist(DestinationPath))
    {
        // An existing folder wins over the object-path reading, even in the rare case where an
        // asset of the same name sits beside it: this is the only reading under which nothing is
        // renamed, so it is the one that cannot destroy information.
        DestinationPath = DestinationPath / SourceLeafName;
        DestinationInterpretedAs = TEXT("folder");
    }
    else if (!DoesAssetDirectoryExist(DestinationParentFolder))
    {
        // Neither reading is available: the path is not an existing folder, and the folder that
        // would have to hold the asset under the object-path reading does not exist either.
        //
        // Refusing is the point of this branch, and creating the folder is deliberately NOT the
        // alternative taken. Picking the object-path reading is the defect being fixed - it
        // silently renamed the asset after the missing folder's last segment and reported
        // success. Auto-creating the folder does not actually resolve the ambiguity, it only
        // makes both readings legal, so it still has to guess which one the caller meant - and
        // it adds a second silent write on top, manufacturing a content folder tree out of a
        // typo. That is the same class of defect one level up. asset.create_folder already
        // exists and is one call, so the caller loses nothing but a round trip.
        //
        // DoesDirectoryExist is the right probe rather than a bare disk check: it answers from
        // the asset registry first and falls back to the directory on disk
        // (UEditorAssetSubsystem::DoesDirectoryExist), so a folder holding only unsaved assets
        // still counts as existing and does not draw a false refusal.
        Ctx.SendError(ErrorCodes::ERR_DESTINATION_FOLDER_NOT_FOUND,
            FString::Printf(
                TEXT("Destination folder does not exist: '%s'. '%s' is neither an existing folder "
                     "to move '%s' into nor a path inside an existing folder to move it to, and "
                     "guessing would rename the asset to '%s'. Create the folder with "
                     "asset.create_folder first, then repeat this call. Nothing was moved."),
                *DestinationParentFolder, *RequestedDestinationPath, *SourceLeafName,
                *FPackageName::GetShortName(DestinationPath)));
        return true;
    }

    if (UEditorAssetLibrary::RenameAsset(ResolvedSourcePath, DestinationPath))
    {
        // Measured, not echoed. assetPath used to be the request string and the verification
        // block below was skipped in silence when the destination would not load, so the
        // response could assert a path the asset was not at. A null load here means the engine
        // reported a rename that did not put the asset where this handler aimed it.
        UObject* MovedAsset = ResolveAsset(DestinationPath, /*bLoadObject=*/true).Object;
        if (!MovedAsset)
        {
            Ctx.SendError(ErrorCodes::ERR_VERIFICATION_FAILED,
                FString::Printf(
                    TEXT("Move of '%s' reported success but nothing is loadable at '%s'. Do not "
                         "treat the asset as moved; locate it before retrying."),
                    *ResolvedSourcePath, *DestinationPath));
            return true;
        }

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("assetPath"), MovedAsset->GetPathName());
        Resp->SetStringField(TEXT("requestedDestinationPath"), RequestedDestinationPath);
        Resp->SetStringField(TEXT("resolvedDestinationPath"), DestinationPath);
        Resp->SetStringField(TEXT("destinationInterpretedAs"), DestinationInterpretedAs);
        // AddAssetVerification measures existsOnDisk through AssetUtils::DoesPackageFileExistOnDisk
        // and emits pendingSave. Both are worth reading here rather than assuming: the engine's
        // rename saves the destination package itself (FAssetRenameManager::PerformAssetRename ->
        // PromptForCheckoutAndSave), but a failed save there is collected only to revert source
        // control and never marks the rename failed, so RenameAssets can return true over a
        // package that never reached disk. Reported, not gated - the in-memory move is real
        // either way, and refusing on it would turn a recoverable unsaved state into an error.
        AddAssetVerification(Resp, MovedAsset);

        Ctx.SendSuccess(Resp);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_RENAME_FAILED,
            FString::Printf(TEXT("Failed to move asset. Check if destination '%s' already exists or source is locked."),
                            *DestinationPath));
    }
    return true;
}

// ============================================================================
// asset.delete
// ============================================================================
REGISTER_RPC_HANDLER("asset.delete", "asset", "Delete one or more assets or whole content folders. Provide path (alias: assetPath) for a single deletion or paths (alias: assetPaths) for a batch (one is required). Returns a per-entry results[] array plus deleted[]/failed[]; success is true only when every requested path is gone afterwards, and existsAfter reports whether anything survived. Every entry is probed against BOTH the asset registry and the .uasset on disk (existsOnDisk), because the engine's delete can tear the objects out of memory and still leave the file — those entries carry memoryDiskDivergence. Does NOT close open asset editors or force-GC first: a still-referenced, open, or read-only asset will fail its entry — read inMemoryReferencers (the live holders that blocked it), close its editor (editor.close_asset) and retry. Deletes are NOT forced by default: a path anything still references is refused with errorCode ASSET_IN_USE and refused:true, having changed nothing — pass force:true to delete anyway and read that parameter's description first, because force nulls every in-memory reference irreversibly and can still leave the file behind.",
    RPC_PARAMS(
        AssetPathParamUtils::DeleteSinglePathParamOpt(
            TEXT("Single asset or folder path to delete (e.g. '/Game/Foo/SM_Bar' or '/Game/Foo'). Alias: assetPath.")),
        AssetPathParamUtils::DeleteBatchPathsParamOpt(
            TEXT("Array of asset/folder paths for batch deletion; preferred for multi-asset cleanup. Alias: assetPaths.")),
        RPC_PARAM_OPT("force", "boolean", "Delete even when something still references the target; defaults to false, which refuses a referenced path with ASSET_IN_USE and changes nothing. force:true runs the engine's Force Delete: it replaces EVERY in-memory pointer to the asset with null editor-wide and marks each of those packages dirty BEFORE the engine decides whether the .uasset may go — irreversibly (no transaction, no undo), and the file can still survive, leaving the referencers broken. Entries deleted this way report referencesNulled; do not save any package named there, reload it with asset.reload instead.")
    ))
{
    // Both slots resolve through AssetPathParamUtils so the `assetPath` / `assetPaths`
    // aliases declared on the specs above actually reach this body — declaring the alias
    // on the FParamSpec only gets the payload past the dispatcher's gates.
    TArray<FString> PathsToDelete;
    AssetPathParamUtils::ResolveDeleteBatchPaths(Ctx, PathsToDelete);

    const FString SinglePath = AssetPathParamUtils::ResolveDeleteSinglePath(Ctx);
    if (!SinglePath.IsEmpty())
    {
        PathsToDelete.Add(SinglePath);
    }

    if (PathsToDelete.Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("No paths provided. Pass path (alias assetPath) for a single deletion, "
                 "or paths (alias assetPaths) for a batch."));
        return true;
    }

    if (RefuseAssetMutationDuringPie(Ctx, TEXT("asset.delete")))
    {
        return true;
    }

    // Opt-in destruction. Without this every path took ObjectTools::ForceDeleteObjects,
    // which nulls every in-memory pointer to the target editor-wide and dirties those
    // packages BEFORE the engine decides whether the .uasset may go - see
    // Utils/AssetDeletePolicy.h for the engine ordering and why nothing restores them.
    const bool bForce = Ctx.GetBool(TEXT("force"), false);

    // Query referencers BEFORE deleting — after deletion the registry would
    // have already purged those entries.
    TArray<FString> AggregatedReferencers;
    for (const FString& Path : PathsToDelete)
    {
        if (!DoesAssetDirectoryExist(Path))
        {
            for (const FString& Ref : GetReferencingPackages(Path))
            {
                AggregatedReferencers.AddUnique(Ref);
            }
        }
        // Directory paths: skip referencing lookup — individual asset paths
        // are not enumerated here to avoid a full scan inside delete.
    }

    // Per-entry outcomes. UEditorAssetLibrary::DeleteAsset returns false for a
    // still-referenced, open, or read-only asset, and the old response discarded that
    // and asserted existsAfter:false unconditionally — a positive claim that the asset
    // was gone, made on the exact path where it survives. success was DeletedCount > 0,
    // so one deletion out of ten read as success with no way to see which nine remained.
    // Sibling doing it right: Handlers/Actor/LifecycleHandler.cpp:51-94 (deleted/missing
    // arrays); the honest existence primitive is Utils/AssetUtils.cpp VerifyAssetExists.
    int32 DeletedCount = 0;
    TArray<FString> DeletedPaths;
    TArray<FString> FailedPaths;
    TArray<FString> MissingPaths;
    TArray<FString> AggregatedInMemoryReferencers;
    TArray<FString> AggregatedDirtiedPackages;
    TArray<TSharedPtr<FJsonValue>> EntryResults;
    bool bAnySurvived = false;
    bool bAnyDiverged = false;
    bool bAnyRefused = false;
    bool bAnyForced = false;

    for (const FString& Path : PathsToDelete)
    {
        const bool bIsDirectory = DoesAssetDirectoryExist(Path);
        // Classify before mutating. Without this a typo'd path probes as "gone"
        // afterwards and would be counted as a successful deletion — the same
        // never-fails shape in a new place.
        const bool bExistedBefore = bIsDirectory || ResolveAsset(Path).bExists;

        // The engine's return value is a claim about MEMORY, not about the file. On the
        // force path (UEditorAssetLibrary::DeleteAsset -> UEditorAssetSubsystem::DeleteAsset
        // -> ObjectTools::ForceDeleteObjects) the count comes from DeleteSingleObject (flag
        // clear + registry notify) and is final BEFORE CleanupAfterSuccessfulDelete decides
        // whether the .uasset may go - and that function silently drops any package still
        // referenced in memory. So this is recorded as a claim and reconciled against the
        // probe below; it is never emitted as the outcome.
        AssetDeletePolicy::FResult DeleteResult;
        if (bExistedBefore)
        {
            DeleteResult = bIsDirectory ? AssetDeletePolicy::DeleteDirectory(Path, bForce)
                                        : AssetDeletePolicy::DeleteAsset(Path, bForce);
        }
        const bool bEngineReportedDelete = DeleteResult.bDeleted;

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("path"), Path);
        Entry->SetStringField(TEXT("kind"), bIsDirectory ? TEXT("folder") : TEXT("asset"));
        Entry->SetBoolField(TEXT("existedBefore"), bExistedBefore);

        // Re-probe rather than trusting the engine's return: existsAfter is measured per
        // entry (VerifyAssetExists writes verifiedPath and returns the registry answer,
        // which is unioned with the on-disk probe below).
        // A folder is probed with DoesDirectoryExist, which is the matching question.
        bool bStillExists = false;
        bool bStillOnDisk = false;
        if (bIsDirectory)
        {
            bStillExists = DoesAssetDirectoryExist(Path);
            Entry->SetStringField(TEXT("verifiedPath"), Path);
            Entry->SetBoolField(TEXT("existsAfter"), bStillExists);
        }
        else
        {
            // Two probes, not one. VerifyAssetExists asks the asset REGISTRY; whether the
            // .uasset is still on disk is a separate question, and the engine's cleanup can
            // leave one without the other. existsAfter is the union - anything still on disk
            // is still there, whatever the registry now says.
            const bool bStillRegistered = VerifyAssetExists(Entry, Path);
            bStillOnDisk = DoesAssetPackageFileExistOnDisk(Path);
            // The registry half of that union answers for in-memory objects too, and a deleted
            // object outlives its own delete until a GC the transaction buffer can veto. Counting
            // that de-flagged leftover made a completed delete report as a survival, so it is
            // discounted - and only it: anything still on disk, and any live object that is still
            // an asset, keeps existsAfter true.
            const bool bRegisteredOnlyAsDeletedShell =
                bStillRegistered && !bStillOnDisk && IsDeletedAssetShellOnly(Path);
            bStillExists = (bStillRegistered && !bRegisteredOnlyAsDeletedShell) || bStillOnDisk;
            Entry->SetBoolField(TEXT("existsOnDisk"), bStillOnDisk);
            Entry->SetBoolField(TEXT("existsAfter"), bStillExists);
        }

        // "deleted" means THIS call removed something that was there. A path that was
        // already absent is reported as missing, not as a deletion this call performed.
        const bool bGone = !bStillExists;
        // The reconciled claim: the engine said it deleted AND the probe agrees. Emitting
        // the raw engine return here made the entry contradict its own existsAfter and read
        // as a success on the exact path where the file survives.
        Entry->SetBoolField(TEXT("deleteReported"), bEngineReportedDelete && bGone);
        Entry->SetBoolField(TEXT("deleted"), bExistedBefore && bGone);
        Entry->SetBoolField(TEXT("missing"), !bExistedBefore);

        if (DeleteResult.bForced)
        {
            // ForceDeleteObjects declares its FForceReplaceInfo as a scoped local and
            // discards it, so the caller can never learn which packages it damaged. The
            // policy recovers the same answer by diffing the editor-wide dirty set across
            // the call. Emitted even when empty: an empty array says the diff ran.
            Entry->SetBoolField(TEXT("forced"), true);
            Entry->SetArrayField(TEXT("referencesNulled"),
                MakeStringJsonArray(DeleteResult.DirtiedPackages));
            for (const FString& Package : DeleteResult.DirtiedPackages)
            {
                AggregatedDirtiedPackages.AddUnique(Package);
            }
            bAnyForced = true;
        }

        if (bExistedBefore && !bGone)
        {
            // Name the holders. On the failure path only - the gather walks every live
            // UObject, and this is the answer the caller needs exactly when the engine gave
            // them none.
            //
            // A refusal already gathered them, over the ASSET OBJECT and before anything was
            // touched, so reuse that list rather than re-walking. The package gather below is
            // for the force path's failures only: pre-delete it would report the (still
            // standalone, still undeleted) asset as its own internal referencer, because
            // GARBAGE_COLLECTION_KEEPFLAGS is RF_Standalone in the editor.
            bool bReferencedByUndo = DeleteResult.bReferencedByUndo;
            const TArray<FString> InMemoryReferencers =
                DeleteResult.bRefused
                    ? DeleteResult.InMemoryReferencers
                    : (bIsDirectory ? TArray<FString>()
                                    : GatherInMemoryPackageReferencers(Path, bReferencedByUndo));
            Entry->SetArrayField(TEXT("inMemoryReferencers"),
                MakeStringJsonArray(InMemoryReferencers));
            Entry->SetBoolField(TEXT("referencedByUndoBuffer"), bReferencedByUndo);
            for (const FString& Ref : InMemoryReferencers)
            {
                AggregatedInMemoryReferencers.AddUnique(Ref);
            }

            if (DeleteResult.bRefused)
            {
                // The refusal is the point of the gate: nothing was nulled, nothing was
                // dirtied, the file is untouched. Say so in the entry rather than letting it
                // read as an ordinary failed delete.
                Entry->SetBoolField(TEXT("refused"), true);
                Entry->SetStringField(TEXT("errorCode"), ErrorCodes::ERR_ASSET_IN_USE);
                Entry->SetStringField(TEXT("refusalReason"), DeleteResult.Message);
                Entry->SetBoolField(TEXT("referencesPreserved"), true);
                Entry->SetArrayField(TEXT("referencers"),
                    MakeStringJsonArray(DeleteResult.OnDiskReferencers));
                bAnyRefused = true;
            }

            // Memory and disk have diverged: the engine tore the objects down and left the
            // file. The editor's view and the content directory disagree until it restarts,
            // so state it rather than leaving the caller to infer it from two fields.
            if (bEngineReportedDelete && bStillOnDisk)
            {
                Entry->SetBoolField(TEXT("engineReportedDelete"), true);
                Entry->SetBoolField(TEXT("memoryDiskDivergence"), true);
                bAnyDiverged = true;
            }
        }
        if (!bExistedBefore)
        {
            MissingPaths.Add(Path);
        }
        else if (bGone)
        {
            DeletedCount++;
            DeletedPaths.Add(Path);
        }
        else
        {
            bAnySurvived = true;
            FailedPaths.Add(Path);
        }
        EntryResults.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    // success means the requested work happened, i.e. NOTHING survived. Partial batch
    // failure is a failure of the request the caller made.
    Resp->SetBoolField(TEXT("success"), !bAnySurvived);
    Resp->SetNumberField(TEXT("deletedCount"), DeletedCount);
    Resp->SetNumberField(TEXT("requestedCount"), PathsToDelete.Num());
    Resp->SetNumberField(TEXT("failedCount"), FailedPaths.Num());
    Resp->SetNumberField(TEXT("missingCount"), MissingPaths.Num());
    // existsAfter is now the measured answer to "did anything survive this delete",
    // aggregated from the per-entry probes above. It was the literal false.
    Resp->SetBoolField(TEXT("existsAfter"), bAnySurvived);
    Resp->SetArrayField(TEXT("results"), EntryResults);
    Resp->SetArrayField(TEXT("deleted"), MakeStringJsonArray(DeletedPaths));
    Resp->SetArrayField(TEXT("failed"), MakeStringJsonArray(FailedPaths));
    Resp->SetArrayField(TEXT("missing"), MakeStringJsonArray(MissingPaths));
    if (bAnySurvived)
    {
        // Emitted even when empty: an empty array says the in-memory referencer walk ran and
        // found nothing, which is a different answer from never having looked.
        Resp->SetArrayField(TEXT("inMemoryReferencers"),
            MakeStringJsonArray(AggregatedInMemoryReferencers));

        // The old hint named three causes (open editor / read-only file / referencing
        // Blueprint) and none of them is the common one. DeleteAsset counts objects torn
        // down in MEMORY, and ObjectTools::CleanupAfterSuccessfulDelete then skips the file
        // delete - with no log line - for any package still referenced in memory, including
        // by another RF_Standalone object in the same package. So the hint leads with that
        // and points at the fields that were actually measured.
        FString Hint = TEXT(
            "One or more paths still exist after the delete. The engine's delete reports objects "
            "torn down in MEMORY, then skips the .uasset removal - silently - for any package "
            "still referenced in memory. Per failed entry: inMemoryReferencers names the holders "
            "found by the engine's own GatherObjectReferencersForDeletion over the package "
            "(internal holders included), referencedByUndoBuffer flags the transaction buffer, and "
            "existsOnDisk says whether the file is still there; referencingBlueprints answers the "
            "narrower on-disk question and is often empty here. Release the named holders (close "
            "their editor with editor.close_asset, drop cached objects) and retry; if nothing is "
            "named, a native holder or a read-only file is left and an editor restart clears the "
            "former. This verb does not close editors or force-GC on your behalf.");
        if (bAnyDiverged)
        {
            Hint += TEXT(
                " memoryDiskDivergence is set on at least one entry: the engine reported those "
                "objects deleted while the .uasset is still on disk, so the editor's view and the "
                "content directory disagree until it restarts - do not re-save assets that "
                "referenced them.");
        }
        if (bAnyRefused)
        {
            Hint += TEXT(
                " At least one entry carries refused:true - that path was NOT force-deleted "
                "and nothing about it changed: no reference was nulled, no package was "
                "dirtied, the .uasset is intact. Read its referencers / inMemoryReferencers, "
                "repoint or delete those holders, and retry; pass force:true only after "
                "reading that parameter's description.");
        }
        Resp->SetStringField(TEXT("failureHint"), Hint);
    }
    if (bAnyRefused)
    {
        // Top-level too, because a batch's per-entry codes are easy to miss and this is the
        // one failure mode where retrying unchanged can never succeed.
        Resp->SetStringField(TEXT("errorCode"), ErrorCodes::ERR_ASSET_IN_USE);
    }
    if (bAnyForced)
    {
        Resp->SetBoolField(TEXT("forced"), true);
        Resp->SetArrayField(TEXT("referencesNulled"),
            MakeStringJsonArray(AggregatedDirtiedPackages));
        if (!AggregatedDirtiedPackages.IsEmpty())
        {
            Resp->SetStringField(TEXT("forceHint"),
                TEXT("force:true replaced every in-memory pointer to the deleted asset(s) with "
                     "null and marked the packages listed in referencesNulled dirty. That is not "
                     "transacted and cannot be undone. Do NOT save any of them - asset.save, "
                     "editor.save_all, editor.quit{save:true} and asset.fixup_redirectors would "
                     "all write the nulls to disk. Discard the damage with asset.reload on each "
                     "listed package."));
        }
    }
    Resp->SetArrayField(TEXT("referencingBlueprints"),
        MakeStringJsonArray(AggregatedReferencers));
    if (!AggregatedReferencers.IsEmpty() && DeletedCount > 0)
    {
        Resp->SetStringField(TEXT("warning"),
            TEXT("One or more deleted assets were referenced by other packages. "
                 "Those packages may now contain broken references."));
    }
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// asset.create_folder
// ============================================================================
REGISTER_RPC_HANDLER("asset.create_folder", "asset", "Create a folder in the content browser",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Folder path to create"),
        RPC_PARAM_OPT("directoryPath", "path", "Folder path to create (alias)")
    ))
{
    FString Path = Ctx.GetStringFirstOf({TEXT("path"), TEXT("directoryPath")});

    if (Path.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("path (or directoryPath) required"));
        return true;
    }

    FString SafePath = SanitizeProjectRelativePath(Path);
    if (SafePath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
            TEXT("Invalid path: must be project-relative and not contain '..'"));
        return true;
    }

    const bool bExistedBefore = DoesAssetDirectoryExist(SafePath);
    if (!bExistedBefore && RefuseAssetMutationDuringPie(Ctx, TEXT("asset.create_folder")))
    {
        return true;
    }

    if (bExistedBefore || UEditorAssetLibrary::MakeDirectory(SafePath))
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        const bool bDirectoryExists = DoesAssetDirectoryExist(SafePath);
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("path"), SafePath);
        Resp->SetStringField(TEXT("verifiedPath"), SafePath);
        Resp->SetBoolField(TEXT("existsAfter"), bDirectoryExists);
        Resp->SetStringField(TEXT("verificationType"), TEXT("directory"));
        Ctx.SendSuccess(Resp);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create folder"));
    }
    return true;
}

// ============================================================================
// asset.exists
// ============================================================================
REGISTER_RPC_HANDLER("asset.exists", "asset", "PIE-safe existence probe via the asset registry, loaded objects, and mounted package files; returns without loading the asset. Use asset.validate to additionally confirm the asset is loadable.",
    RPC_PARAMS(
        AssetPathParamUtils::AssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Asset path to test (e.g. /Game/Foo/SM_Bar). Both /Game/X and /Game/X.X forms accepted. Alias: path."))
    ))
{
    FString AssetPath;
    if (!AssetPathParamUtils::RequireAssetPathRaw(Ctx, AssetPath)) return true;

    const bool bExists = ResolveAsset(AssetPath).bExists;

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("exists"), bExists);
    Resp->SetStringField(TEXT("assetPath"), AssetPath);
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// asset.get
// ============================================================================
REGISTER_RPC_HANDLER("asset.get", "asset", "Read summary metadata (name, class, package, asset registry tags) for one asset by path. Loads the asset to populate fields; for a deep dump use asset.dump or asset.dump_folder instead.",
    RPC_PARAMS(
        AssetPathParamUtils::AssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Asset path to inspect (e.g. /Game/Foo/SM_Bar). Alias: path."))
    ))
{
    FString AssetPath;
    if (!AssetPathParamUtils::RequireAssetPathRaw(Ctx, AssetPath)) return true;

    const FResolvedAsset Resolved = ResolveAsset(AssetPath, /*bLoadObject=*/true);
    if (!Resolved.bExists)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, Resolved.ErrorMessage);
        return true;
    }

    const FAssetData& AssetData = Resolved.AssetData;
    if (!AssetData.IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_DATA_INVALID, TEXT("Failed to find asset data"));
        return true;
    }

    TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
    AssetObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
    AssetObj->SetStringField(TEXT("path"), AssetData.GetSoftObjectPath().ToString());
    AssetObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
    AssetObj->SetStringField(TEXT("packagePath"), AssetData.PackagePath.ToString());

    // Asset-registry tags as a typed name->value map. The registered summary
    // advertises "asset registry tags" as part of asset.get's output; emit them
    // here so the doc is honest and the obvious "read one asset" verb carries
    // registry values (e.g. a material's BlendMode, a mesh's NaniteEnabled) in a
    // single shot. Shares BuildAssetRegistryTagsObject with asset.get_metadata so
    // the two verbs' tags maps are guaranteed identical in shape.
    AssetObj->SetObjectField(TEXT("tags"), BuildAssetRegistryTagsObject(AssetData));

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetObjectField(TEXT("result"), AssetObj);

    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// asset.list
// ============================================================================
REGISTER_RPC_HANDLER("asset.list", "asset", "Browse a FOLDER: assets plus subfolders under one package path, with filtering and pagination. If you already know roughly what the asset is CALLED, do not list a folder and read it — call asset.search instead, which matches the name case-insensitively and is capped at 50 compact rows. Each row here carries name+path+class+packagePath and a verbose per-asset tags array, so on a populated tree even a small page spills to a file (/Engine/EngineMaterials is ~3200 rows, several MB); narrow it inline with pagination.limit, a tighter filter.class/path, or namesOnly=true / fields=[...] to drop the per-row tags weight and return just the identity you need to pick an asset.",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Package path to list (default /Game)"),
        RPC_PARAM_OPT("filter", "object", "Filter object with path, class, tag, pathStartsWith. 'class' is matched case-INSENSITIVELY against both the short class name and the full class path, the same policy as asset.search's classFilterMode:'exact'; the resolved policy is echoed back as classFilterMode + classFilterCaseSensitive."),
        RPC_PARAM_OPT("recursive", "boolean", "Recurse into subfolders (default true)"),
        RPC_PARAM_OPT("pagination", "object", "Pagination: {offset, limit}. offset defaults to 0; limit defaults to -1 = UNLIMITED, so an omitted pagination returns every match and reliably spills the inline response budget on a populated tree. Pass a limit. Read the response's truncated flag to detect withheld rows."),
        RPC_PARAM_OPT("depth", "number", "Max subfolder depth (-1 = unlimited)"),
        RPC_PARAM_OPT_ALIAS("fields", "array|string", "Case-insensitive allow-list of per-row keys to return (valid keys: name, path, class, packagePath, tags); e.g. [\"name\",\"path\"] to drop the verbose tags array. Omit for all five. A single string is also accepted.", "field"),
        RPC_PARAM_OPT_ALIAS("namesOnly", "bool", "When true, returns only name+path per asset (drops class, packagePath, and the verbose tags array) — shorthand for the common 'list assets so I can pick one' read. Snake_case names_only accepted. Ignored when fields is supplied.", "names_only")
    ))
{
    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();

    // Parse filters
    FString PathFilter;
    FString ClassFilter;
    FString TagFilter;
    FString PathStartsWith;

    const TSharedPtr<FJsonObject>* FilterObj;
    if (Payload->TryGetObjectField(TEXT("filter"), FilterObj) && FilterObj)
    {
        (*FilterObj)->TryGetStringField(TEXT("path"), PathFilter);
        (*FilterObj)->TryGetStringField(TEXT("class"), ClassFilter);
        (*FilterObj)->TryGetStringField(TEXT("tag"), TagFilter);
        (*FilterObj)->TryGetStringField(TEXT("pathStartsWith"), PathStartsWith);
    }
    // Honor top-level `path` whenever the filter didn't supply a path (schema advertises it unconditionally).
    if (PathFilter.IsEmpty() && PathStartsWith.IsEmpty())
    {
        Payload->TryGetStringField(TEXT("path"), PathFilter);
    }

    // Sanitize PathFilter
    if (PathFilter.Len() > 1 && PathFilter.EndsWith(TEXT("/")))
    {
        PathFilter.RemoveAt(PathFilter.Len() - 1);
    }

    bool bRecursive = true;
    Payload->TryGetBoolField(TEXT("recursive"), bRecursive);

    // Parse pagination
    int32 Offset = 0;
    int32 Limit = -1;
    const TSharedPtr<FJsonObject>* PaginationObj;
    if (Payload->TryGetObjectField(TEXT("pagination"), PaginationObj) && PaginationObj)
    {
        (*PaginationObj)->TryGetNumberField(TEXT("offset"), Offset);
        (*PaginationObj)->TryGetNumberField(TEXT("limit"), Limit);
    }

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    FARFilter Filter;
    Filter.bRecursivePaths = bRecursive;
    Filter.bRecursiveClasses = true;

    if (!PathFilter.IsEmpty())
    {
        Filter.PackagePaths.Add(FName(*PathFilter));
    }
    else if (!PathStartsWith.IsEmpty())
    {
        Filter.PackagePaths.Add(FName(*PathStartsWith));
    }
    else
    {
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
    }

    // Ensure registry is up to date
    TArray<FString> ScanPaths;
    for (const FName& Path : Filter.PackagePaths)
    {
        ScanPaths.Add(Path.ToString());
    }
    AssetRegistry.ScanPathsSynchronous(ScanPaths, true);

    if (!ClassFilter.IsEmpty())
    {
        // ResolveUClass handles both full paths and short names without firing
        // the FTopLevelAssetPath short-name ensure. Unresolved names fall through
        // to the post-filter AssetClassName.Equals(ClassFilter) path below.
        if (UClass* ResolvedClass = ResolveUClass(ClassFilter))
        {
            FTopLevelAssetPath ClassPath(ResolvedClass->GetPathName());
            if (ClassPath.IsValid())
            {
                Filter.ClassPaths.Add(ClassPath);
            }
        }
    }

    TArray<FAssetData> AssetList;
    AssetRegistry.GetAssets(Filter, AssetList);

    // Post-filtering
    if (!ClassFilter.IsEmpty() || !TagFilter.IsEmpty())
    {
        AssetList.RemoveAll([&](const FAssetData& Asset) {
            if (!ClassFilter.IsEmpty())
            {
                FString AssetClass = Asset.AssetClassPath.ToString();
                FString AssetClassName = Asset.AssetClassPath.GetAssetName().ToString();
                // ESearchCase is explicit here because FString::Equals defaults to
                // CaseSensitive while FString::Contains defaults to IgnoreCase - relying on
                // either default is how the same conceptual filter ended up with opposite
                // behaviour on two sibling verbs. IgnoreCase is the converged policy: it is
                // what asset.search's classFilterMode already does in all three of its modes,
                // what NameMatch::FFilter defaults to plugin-wide, and what this verb ITSELF
                // already did one step earlier, since ResolveUClass above resolves the class
                // name case-insensitively when it builds the FARFilter. The case-sensitive
                // fallback therefore contradicted the verb's own front half, and a merely
                // mis-cased filter.class returned an empty page that reads as "no such assets
                // exist" rather than "your filter's case was wrong".
                if (!AssetClass.Equals(ClassFilter, ESearchCase::IgnoreCase) &&
                    !AssetClassName.Equals(ClassFilter, ESearchCase::IgnoreCase))
                {
                    return true;
                }
            }
            if (!TagFilter.IsEmpty())
            {
                if (!Asset.TagsAndValues.Contains(FName(*TagFilter)))
                {
                    return true;
                }
            }
            return false;
        });
    }

    // Filter by Depth
    int32 Depth = -1;
    Payload->TryGetNumberField(TEXT("depth"), Depth);

    if (Depth >= 0 && bRecursive && !PathFilter.IsEmpty())
    {
        FString BasePath = PathFilter;
        if (BasePath.EndsWith(TEXT("/")))
        {
            BasePath.RemoveAt(BasePath.Len() - 1);
        }
        int32 BaseSlashCount = 0;
        for (const TCHAR* P = *BasePath; *P; ++P)
        {
            if (*P == TEXT('/'))
                BaseSlashCount++;
        }

        AssetList.RemoveAll([&](const FAssetData& Asset) {
            FString PkgPath = Asset.PackagePath.ToString();
            int32 SlashCount = 0;
            for (const TCHAR* P = *PkgPath; *P; ++P)
            {
                if (*P == TEXT('/'))
                    SlashCount++;
            }
            return (SlashCount - BaseSlashCount) > Depth;
        });
    }

    int32 TotalCount = AssetList.Num();

    // Apply pagination
    if (Offset > 0)
    {
        if (Offset >= AssetList.Num())
        {
            AssetList.Empty();
        }
        else
        {
            AssetList.RemoveAt(0, Offset);
        }
    }

    if (Limit >= 0 && AssetList.Num() > Limit)
    {
        AssetList.SetNum(Limit);
    }

    // Fetch sub-folders
    TArray<FString> SubPathList;
    if (!PathFilter.IsEmpty())
    {
        AssetRegistry.GetSubPaths(PathFilter, SubPathList, false);
    }

    // Per-row field projection mirrors actor.list: an explicit `fields` allow-list
    // (array or bare string) wins; otherwise namesOnly drops everything except the
    // identity (name+path) — the "list assets so I can pick one" shape that must not
    // spill on the verbose per-row tags array. Default keeps all five keys so the
    // unprojected output is byte-identical to the prior shape. Parse via the shared
    // FHandlerContext::ReadFieldProjection; the namesOnly column set (name+path) is the
    // argument. The probe keys are lowercase to match the lowercased set it returns
    // (packagePath -> packagepath).
    const TSet<FString> Fields = Ctx.ReadFieldProjection({TEXT("name"), TEXT("path")});
    const bool bProject = Fields.Num() > 0;
    const auto Wants = [&Fields, bProject](const TCHAR* Key) {
        return !bProject || Fields.Contains(FString(Key));
    };
    const bool bWantName = Wants(TEXT("name"));
    const bool bWantPath = Wants(TEXT("path"));
    const bool bWantClass = Wants(TEXT("class"));
    const bool bWantPackagePath = Wants(TEXT("packagepath"));
    const bool bWantTags = Wants(TEXT("tags"));

    TArray<TSharedPtr<FJsonValue>> AssetsArray;
    for (const FAssetData& Asset : AssetList)
    {
        TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
        if (bWantName)
        {
            AssetObj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        }
        if (bWantPath)
        {
            AssetObj->SetStringField(TEXT("path"), Asset.GetSoftObjectPath().ToString());
        }
        if (bWantClass)
        {
            AssetObj->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
        }
        if (bWantPackagePath)
        {
            AssetObj->SetStringField(TEXT("packagePath"), Asset.PackagePath.ToString());
        }
        if (bWantTags)
        {
            TArray<TSharedPtr<FJsonValue>> Tags;
            for (auto TagPair : Asset.TagsAndValues)
            {
                Tags.Add(MakeShared<FJsonValueString>(TagPair.Key.ToString()));
            }
            AssetObj->SetArrayField(TEXT("tags"), Tags);
        }

        AssetsArray.Add(MakeShared<FJsonValueObject>(AssetObj));
    }

    TArray<TSharedPtr<FJsonValue>> FoldersJson;
    for (const FString& SubPath : SubPathList)
    {
        FoldersJson.Add(MakeShared<FJsonValueString>(SubPath));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetArrayField(TEXT("assets"), AssetsArray);
    Resp->SetArrayField(TEXT("folders"), FoldersJson);
    Resp->SetNumberField(TEXT("totalCount"), TotalCount);
    Resp->SetNumberField(TEXT("count"), AssetsArray.Num());
    Resp->SetNumberField(TEXT("offset"), Offset);
    // Explicit elision signal, matching the count/totalMatches/truncated vocabulary of
    // the sibling bounded-list verbs (actor.list, asset.search). Previously a caller had
    // to compute offset + count < totalCount by hand to notice rows were withheld.
    Resp->SetBoolField(TEXT("truncated"), (Offset + AssetsArray.Num()) < TotalCount);
    if (!ClassFilter.IsEmpty())
    {
        // Same three fields asset.search echoes for the same concept, so a caller moving
        // between the two verbs can see that the semantics did not change under them.
        Resp->SetStringField(TEXT("classFilter"), ClassFilter);
        Resp->SetStringField(TEXT("classFilterMode"), TEXT("exact"));
        Resp->SetBoolField(TEXT("classFilterCaseSensitive"), false);
    }

    // Diagnostics for empty results
    if (TotalCount == 0 && FoldersJson.Num() == 0)
    {
        TSharedPtr<FJsonObject> DiagObj = MakeShared<FJsonObject>();

        FString EffectivePath = PathFilter;
        if (EffectivePath.IsEmpty() && !PathStartsWith.IsEmpty())
        {
            EffectivePath = PathStartsWith;
        }
        if (EffectivePath.IsEmpty())
        {
            EffectivePath = TEXT("/Game");
        }
        DiagObj->SetStringField(TEXT("scannedPath"), EffectivePath);

        const bool bPathExists = AssetRegistry.PathExists(FName(*EffectivePath));
        DiagObj->SetBoolField(TEXT("pathExistsInRegistry"), bPathExists);

        if (!bPathExists)
        {
            DiagObj->SetStringField(TEXT("hint"),
                FString::Printf(TEXT("Path '%s' is not registered in the Asset Registry. "
                    "Valid paths typically start with /Game, /Engine, or a plugin mount point. "
                    "Use asset.list with path=/Game to discover available paths."),
                    *EffectivePath));
        }

        if (!ClassFilter.IsEmpty())
        {
            DiagObj->SetStringField(TEXT("classFilter"), ClassFilter);
        }
        if (!TagFilter.IsEmpty())
        {
            DiagObj->SetStringField(TEXT("tagFilter"), TagFilter);
        }

        Resp->SetObjectField(TEXT("diagnostics"), DiagObj);
    }

    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// asset.search (simple name-based search)
// ============================================================================
REGISTER_RPC_HANDLER("asset.search", "asset", "FIND AN ASSET BY NAME — the answer to \"which asset is called something like X?\". Case-insensitive substring by default, or wildcard when the query contains * / ?. Bounded by construction: limit defaults to 50 and rows are name+path+class+packagePath only, so the answer stays inline where asset.list on the same folder spills megabytes to a file. Scope with path (/Game, /Engine, a plugin mount) and narrow with classFilter. For a class-only list with no name in hand use asset.list or asset.search_assets; for UMetaData tags use asset.find_by_tag.",
    RPC_PARAMS(
        SearchQueryParamUtils::SearchQueryParamReq(TEXT("query"), TEXT("string"),
            TEXT("Name pattern to match against the asset NAME (not its path). Plain text is a case-insensitive substring match; a query containing * or ? is matched as a case-insensitive wildcard instead ('BP_Enemy*' anchors the start, '*Grey*' is the explicit form of the substring default). Not a regex. Alias: pattern.")),
        RPC_PARAM_OPT("path", "path", "Scope path to search within"),
        RPC_PARAM_OPT("classFilter", "array|string", "Filter by class name. One name, or an array of names matched as OR (e.g. [\"StaticMesh\",\"SkeletalMesh\"]) — the same array spelling blueprint.build_api_index takes. An array holding a non-string or an empty string, and an empty array, are REJECTED with INVALID_ARGUMENT rather than dropped."),
        RPC_PARAM_OPT("classPathFilter", "classref", "Filter by full class path (for precise subtype scoping)"),
        RPC_PARAM_OPT("parentClassPath", "classref", "Filter by parent class (native or BP) — uses asset registry recursive class filter. Accepts /Script/Module.ClassName or short name."),
        RPC_PARAM_OPT("classFilterMode", "string", "How classFilter / classPathFilter are matched: 'exact' (default), 'prefix' (alias 'starts_with') or 'contains' (alias 'substring'). All three are case-INSENSITIVE, and 'exact' is the same policy asset.list applies to filter.class. Any other value is REJECTED with INVALID_MODE; it is never silently run as 'exact'."),
        RPC_PARAM_OPT("limit", "number", "Max rows to RETURN (default 50, max 500). The scan is unaffected: totalMatches always counts every match and truncated says whether rows were withheld."),
        RPC_PARAM_OPT_ALIAS("fields", "array|string", "Case-insensitive allow-list of per-row keys to return (valid keys: name, path, class, packagePath); e.g. [\"name\",\"path\"]. Omit for all four. A single string is also accepted.", "field"),
        RPC_PARAM_OPT_ALIAS("namesOnly", "bool", "When true, returns only name+path per row (drops class and packagePath) — use it when a 500-row limit over deep /Game paths would otherwise overflow the inline budget. Snake_case names_only accepted. Ignored when fields is supplied.", "names_only")
    ))
{
    FString Query = SearchQueryParamUtils::ResolveSearchQuery(Ctx);
    if (Query.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Missing required 'query' parameter."));
        return true;
    }

    FString ScopePath = Ctx.GetString(TEXT("path"));
    // classFilter takes one class name OR an array of them (OR-matched), the same array
    // spelling blueprint.build_api_index declares for this parameter name. It used to be
    // read with Ctx.GetString alone: an array reached FJsonValue::AsString, which returns
    // "" on a type mismatch, so the filter never ran, every asset was kept, and the reply
    // still carried classFilterMode while the classFilter echo below was suppressed by the
    // very emptiness that caused it — a full result set reported as a filtered one
    // (B-asset-search-array-class-filter-silently-dropped). Nothing in the array shape is
    // dropped now: a non-string element, an empty entry and an empty array are refused.
    TArray<FString> ClassFilters;
    const TArray<TSharedPtr<FJsonValue>>* ClassFilterArray = Ctx.GetArray(TEXT("classFilter"));
    if (ClassFilterArray)
    {
        for (const TSharedPtr<FJsonValue>& Element : *ClassFilterArray)
        {
            if (!Element.IsValid() || Element->Type != EJson::String || Element->AsString().IsEmpty())
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    TEXT("'classFilter' holds an element that is not a non-empty string. Every ")
                    TEXT("entry names a class, e.g. [\"StaticMesh\",\"SkeletalMesh\"]."));
                return true;
            }
            ClassFilters.AddUnique(Element->AsString());
        }
        if (ClassFilters.Num() == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("'classFilter' is an empty array, which filters nothing. Omit the ")
                TEXT("parameter to search without a class filter."));
            return true;
        }
    }
    else
    {
        const FString SingleClassFilter = Ctx.GetString(TEXT("classFilter"));
        if (!SingleClassFilter.IsEmpty())
        {
            ClassFilters.Add(SingleClassFilter);
        }
    }
    FString ClassPathFilter = Ctx.GetString(TEXT("classPathFilter"));
    FString ParentClassPath = Ctx.GetString(TEXT("parentClassPath"));
    // An unrecognised classFilterMode used to fall through to the exact branch at the
    // bottom of MatchesClassByMode, so classFilterMode:"regex" or a typo silently ran a
    // different comparison than the caller asked for and the echoed value said so without
    // anyone noticing. Refuse it instead: an accepted parameter is a promise.
    // NormalizeToken folds case and '-' -> '_' exactly as NameMatch::Parse does.
    const FString RawClassFilterMode = Ctx.GetString(TEXT("classFilterMode"));
    FString ClassFilterMode = PinWright::NormalizeToken(RawClassFilterMode);
    if (ClassFilterMode.IsEmpty())
    {
        ClassFilterMode = TEXT("exact");
    }
    else if (ClassFilterMode == TEXT("substring"))
    {
        ClassFilterMode = TEXT("contains");
    }
    else if (ClassFilterMode == TEXT("starts_with"))
    {
        ClassFilterMode = TEXT("prefix");
    }
    if (ClassFilterMode != TEXT("exact") && ClassFilterMode != TEXT("prefix") &&
        ClassFilterMode != TEXT("contains"))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_MODE,
            FString::Printf(
                TEXT("Unknown classFilterMode '%s'. Valid values: exact (default), prefix ")
                TEXT("(alias starts_with), contains (alias substring). All three are ")
                TEXT("case-insensitive. Rejected rather than silently running 'exact'."),
                *RawClassFilterMode));
        return true;
    }

    if (!ParentClassPath.IsEmpty() && !ClassPathFilter.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("cannot use classPathFilter and parentClassPath together"));
        return true;
    }

    int32 Limit = 50;
    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    if (Payload->HasField(TEXT("limit")))
    {
        Limit = FMath::Clamp(static_cast<int32>(Payload->GetNumberField(TEXT("limit"))), 1, 500);
    }

    IAssetRegistry& AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

    TArray<FAssetData> AllAssets;
    if (!ParentClassPath.IsEmpty())
    {
        // Unresolvable parent class yields empty results, matching the unknown-class behaviour of other handlers.
        UClass* ResolvedParent = ResolveUClass(ParentClassPath);
        if (ResolvedParent)
        {
            // FARFilter with bRecursiveClasses matches actors, CDOs, and assets whose own
            // class is in the subclass tree (WP external-actor instances). BP .uasset records
            // are covered separately by AppendBlueprintAssetsDerivedFromNativeClass below.
            FARFilter Filter;
            Filter.ClassPaths.Add(ResolvedParent->GetClassPathName());
            Filter.bRecursiveClasses = true;
            if (!ScopePath.IsEmpty())
            {
                Filter.PackagePaths.Add(FName(*ScopePath));
                Filter.bRecursivePaths = true;
            }
            TArray<FName> BpPackagePaths;
            if (!ScopePath.IsEmpty()) BpPackagePaths.Add(FName(*ScopePath));
            AppendBlueprintAssetsDerivedFromNativeClass(
                AssetRegistry, ResolvedParent, BpPackagePaths,
                /*bRecursivePaths=*/!ScopePath.IsEmpty(),
                /*MaxResults=*/-1, AllAssets);

            TArray<FAssetData> NativeAssets;
            AssetRegistry.GetAssets(Filter, NativeAssets);

            TSet<FString> SeenObjectPaths;
            SeenObjectPaths.Reserve(AllAssets.Num());
            for (const FAssetData& Asset : AllAssets)
            {
                SeenObjectPaths.Add(Asset.GetSoftObjectPath().ToString());
            }
            for (const FAssetData& Asset : NativeAssets)
            {
                const FString ObjectPath = Asset.GetSoftObjectPath().ToString();
                bool bAlreadySeen = false;
                SeenObjectPaths.Add(ObjectPath, &bAlreadySeen);
                if (!bAlreadySeen)
                {
                    AllAssets.Add(Asset);
                }
            }
        }
        // else: AllAssets remains empty — unresolved parent class yields 0 results.
    }
    else if (!ScopePath.IsEmpty())
    {
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*ScopePath));
        Filter.bRecursivePaths = true;
        AssetRegistry.GetAssets(Filter, AllAssets);
    }
    else
    {
        AssetRegistry.GetAllAssets(AllAssets);
    }

    const bool bIsWildcard = Query.Contains(TEXT("*")) || Query.Contains(TEXT("?"));
    auto MatchesClassByMode = [&](const FString& Candidate, const FString& Filter) -> bool
    {
        if (Filter.IsEmpty())
        {
            return true;
        }
        if (ClassFilterMode == TEXT("contains"))
        {
            return Candidate.Contains(Filter, ESearchCase::IgnoreCase);
        }
        if (ClassFilterMode == TEXT("prefix"))
        {
            return Candidate.StartsWith(Filter, ESearchCase::IgnoreCase);
        }
        // Reached only for "exact": every other spelling was rejected above, so this is
        // no longer the branch that swallows an unrecognised mode.
        return Candidate.Equals(Filter, ESearchCase::IgnoreCase);
    };

    // Per-row projection mirrors asset.list / actor.list: an explicit `fields`
    // allow-list wins, otherwise namesOnly collapses the row to its identity.
    // It exists because the DEFAULT row is not free — see the truncation note
    // below for why a 500-row page over deep /Game paths needs this lever.
    const TSet<FString> Fields = Ctx.ReadFieldProjection({TEXT("name"), TEXT("path")});
    const bool bProject = Fields.Num() > 0;
    const auto Wants = [&Fields, bProject](const TCHAR* Key) {
        return !bProject || Fields.Contains(FString(Key));
    };
    const bool bWantName = Wants(TEXT("name"));
    const bool bWantPath = Wants(TEXT("path"));
    const bool bWantClass = Wants(TEXT("class"));
    const bool bWantPackagePath = Wants(TEXT("packagepath"));

    // TotalMatches counts EVERY match; Results collects at most Limit of them.
    // The loop deliberately does not break at the cap any more: the previous
    // version stopped scanning the moment it had Limit rows, so it could not
    // know — and did not report — whether anything was withheld, and a response
    // reading {count:50, limit:50} was indistinguishable from "exactly 50 exist"
    // and "50 of 4000". Continuing the walk costs one substring compare per
    // remaining FAssetData over an array the verb has already materialised.
    int32 TotalMatches = 0;
    TArray<TSharedPtr<FJsonValue>> Results;
    for (const FAssetData& Asset : AllAssets)
    {
        const FString AssetClassName = Asset.AssetClassPath.GetAssetName().ToString();
        const FString AssetClassPath = Asset.AssetClassPath.ToString();
        if (!ClassPathFilter.IsEmpty())
        {
            if (!MatchesClassByMode(AssetClassPath, ClassPathFilter))
            {
                continue;
            }
        }
        else if (ClassFilters.Num() > 0)
        {
            bool bMatchesAnyClass = false;
            for (const FString& OneClassFilter : ClassFilters)
            {
                if (MatchesClassByMode(AssetClassName, OneClassFilter) ||
                    MatchesClassByMode(AssetClassPath, OneClassFilter))
                {
                    bMatchesAnyClass = true;
                    break;
                }
            }
            if (!bMatchesAnyClass)
            {
                continue;
            }
        }

        const FString AssetName = Asset.AssetName.ToString();
        bool bMatches = false;
        if (bIsWildcard)
        {
            bMatches = AssetName.MatchesWildcard(Query, ESearchCase::IgnoreCase);
        }
        else
        {
            bMatches = AssetName.Contains(Query, ESearchCase::IgnoreCase);
        }

        if (bMatches)
        {
            ++TotalMatches;
            if (Results.Num() >= Limit)
            {
                continue;   // keep counting; stop emitting
            }

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            if (bWantName)
            {
                Entry->SetStringField(TEXT("name"), AssetName);
            }
            if (bWantPath)
            {
                Entry->SetStringField(TEXT("path"), Asset.GetObjectPathString());
            }
            if (bWantClass)
            {
                Entry->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
            }
            if (bWantPackagePath)
            {
                Entry->SetStringField(TEXT("packagePath"), Asset.PackagePath.ToString());
            }
            Results.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("assets"), Results);
    Result->SetNumberField(TEXT("count"), Results.Num());
    Result->SetNumberField(TEXT("totalMatches"), TotalMatches);
    // Measured from the two counts rather than assumed from the cap: a run that
    // matched exactly Limit assets reports truncated:false, which is the case a
    // count-vs-limit comparison at the call site gets wrong.
    Result->SetBoolField(TEXT("truncated"), TotalMatches > Results.Num());
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetStringField(TEXT("query"), Query);
    if (!ScopePath.IsEmpty())
    {
        Result->SetStringField(TEXT("scopePath"), ScopePath);
    }
    // Echoed in the shape it was supplied in, so the reply reports the filter that ran
    // rather than a normalisation of it.
    if (ClassFilters.Num() > 0)
    {
        if (ClassFilterArray)
        {
            TArray<TSharedPtr<FJsonValue>> ClassFilterEcho;
            for (const FString& OneClassFilter : ClassFilters)
            {
                ClassFilterEcho.Add(MakeShared<FJsonValueString>(OneClassFilter));
            }
            Result->SetArrayField(TEXT("classFilter"), ClassFilterEcho);
        }
        else
        {
            Result->SetStringField(TEXT("classFilter"), ClassFilters[0]);
        }
    }
    if (!ClassPathFilter.IsEmpty())
    {
        Result->SetStringField(TEXT("classPathFilter"), ClassPathFilter);
    }
    if (!ParentClassPath.IsEmpty())
    {
        Result->SetStringField(TEXT("parentClassPath"), ParentClassPath);
    }
    // Gated on a class filter existing, the same way the classFilter echo above is. It
    // used to be emitted unconditionally, so a response could describe how a filter was
    // matched when no filter ran — the field that made the silent drop above look like a
    // filtered result.
    if (ClassFilters.Num() > 0 || !ClassPathFilter.IsEmpty())
    {
        Result->SetStringField(TEXT("classFilterMode"), ClassFilterMode);
    }
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// asset.validate
// ============================================================================
REGISTER_RPC_HANDLER("asset.validate", "asset", "Validate if an asset exists and can be loaded",
    RPC_PARAMS(
        AssetPathParamUtils::AssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Path to the asset to validate. Alias: path."))
    ))
{
    FString AssetPath;
    if (!AssetPathParamUtils::RequireAssetPathRaw(Ctx, AssetPath)) return true;

    const FResolvedAsset Resolved = ResolveAsset(AssetPath, /*bLoadObject=*/true);
    if (!Resolved.bExists)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, Resolved.ErrorMessage);
        return true;
    }

    UObject* Asset = Resolved.Object;
    if (!Asset)
    {
        Ctx.SendError(ErrorCodes::ERR_LOAD_FAILED, TEXT("Failed to load asset"));
        return true;
    }

    const bool bIsValid = true;
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), bIsValid);
    Resp->SetStringField(TEXT("assetPath"), AssetPath);
    Resp->SetBoolField(TEXT("isValid"), bIsValid);

    Ctx.SendSuccess(TEXT("Asset validated"), Resp);
    return true;
}

// ============================================================================
// asset.reload
// ============================================================================
// asset.validate above re-reads the ALREADY-RESIDENT in-memory UObject (LoadAsset
// returns the loaded object, no eviction), so every downstream read (blueprint.decompile,
// blueprint.graph.*, property getters) reflects live memory, never the persisted bytes.
// asset.reload closes that verification gap: it force-evicts the package and re-serializes
// it from the on-disk .uasset, so a subsequent read reflects what was actually saved —
// letting an agent prove a saved asset reloads to the same shape (persistence / corruption
// verification) within one editor session.
REGISTER_RPC_HANDLER("asset.reload", "asset",
    "Force-evict a loaded asset package and re-read it from disk within the current editor "
    "session, so a subsequent decompile / graph / property read reflects the persisted bytes "
    "rather than the live in-memory UObject. Discards unsaved in-memory edits to the package "
    "(the on-disk .uasset wins). Use after asset.save to prove the saved asset reloads to the "
    "same shape (persistence / corruption verification). For a level (.umap) use "
    "editor.open_level instead — this refuses the active editor level package. Returns reloaded "
    "(bool) and wasLoaded (bool); for a Blueprint also compiled (bool) + blueprintStatus.",
    RPC_PARAMS(
        AssetPathParamUtils::AssetPathParamReq(TEXT("assetPath"), TEXT("path"),
            TEXT("Path to the asset whose package should be evicted and reloaded from disk. Alias: path."))
    ))
{
    // SYNCHRONOUS ON PURPOSE — do not re-introduce an AsyncTask / FAsyncResponseToken
    // continuation here. This body used to run inside
    // AsyncTask(ENamedThreads::GameThread, ...), and that detached continuation is what
    // killed a shared editor (board B-asset-reload-blueprint-package-crash /
    // B-asset-reload-access-violation-kills-editor):
    //
    //   1. The marshal was REDUNDANT. FRpcDispatcher::ProcessRequest already marshals every
    //      request to the game thread before invoking a handler (RpcDispatcher.cpp, the
    //      `if (!IsInGameThread())` hop at the top of ProcessRequest), so a handler body is
    //      game-thread by construction. All the AsyncTask bought was a different *position*.
    //   2. It ran the reload OUTSIDE the dispatcher's `bProcessingRequest` reentrancy guard.
    //      The handler returned immediately, the guard cleared, and the actual reload ran
    //      later on a bare named-thread pump with nothing serialising it against other RPCs.
    //   3. UPackageTools::ReloadPackages pumps repeatedly while it works: FlushAsyncLoading,
    //      FGlobalComponentReregisterContext (render-state recreation + render flush),
    //      FScopedSlowTask progress frames, and TWO CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS)
    //      passes (PackageReload.cpp:821, :838). Meanwhile ::ReloadPackages holds a RAW
    //      `TArray<UObject*> PotentialReferencers` snapshot of the whole object graph
    //      (PackageReload.cpp:744, the slow path — `PackageReload.EnableFastPath` defaults
    //      false) and serialises each entry through FReplaceObjectReferencesArchive
    //      (PackageReload.cpp:774). Any OTHER RPC drained during one of those pumps mutates
    //      the object graph under that snapshot; a UFunction whose FProperties were freed by
    //      the interleaved work then walks its own bytecode and dereferences a dangling
    //      FField* in FPropertyProxyArchive::operator<< (PropertyProxyArchive.h:46) — the
    //      reported EXCEPTION_ACCESS_VIOLATION.
    //
    // Running the body inline keeps the whole reload inside the dispatcher's reentrancy
    // guard, so a request arriving during those pumps is queued rather than executed
    // underneath it. The complementary half of the fix is the `asset.reload` entry in
    // Dispatch/SafePoint.cpp's tick-unsafe table, which keeps this (now synchronous) body
    // off UWorld::Tick and off a named-thread pump in the first place — the two only work
    // together, because a table entry cannot gate work a handler hands to another queue.
    FString AssetPath;
    if (!AssetPathParamUtils::RequireAssetPathRaw(Ctx, AssetPath)) return true;

    if (RefuseAssetMutationDuringPie(Ctx, TEXT("asset.reload")))
    {
        return true;
    }

    if (!ResolveAsset(AssetPath).bExists)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, TEXT("Asset not found"));
        return true;
    }

    // Derive the package name from the (possibly object-path) asset path via the
    // engine's shared object-path->package-name mapping (matches NormalizeAssetPath /
    // DoesRequestedLevelMatchCurrentWorld) instead of a hand-rolled dot split.
    const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);

    // Refuse to reload the active editor level package: ReloadPackages on the live
    // world tears down actors the editor still references (a crash surface on a
    // fuzzing host). editor.open_level is the correct way to reload a map.
    if (GEditor)
    {
        const UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
        if (EditorWorld && EditorWorld->GetOutermost() &&
            EditorWorld->GetOutermost()->GetName() == PackageName)
        {
            Ctx.SendError(ErrorCodes::ERR_CANNOT_RELOAD_ACTIVE_LEVEL,
                TEXT("Refusing to reload the active editor level package; use editor.open_level to reload a map."));
            return true;
        }
    }

    bool bReloaded = false;
    FString FailureText;
    const UObject* Reloaded = nullptr;
    UPackage* Package = FindPackage(nullptr, *PackageName);
    const bool bWasLoaded = (Package != nullptr);

    if (bWasLoaded)
    {
        // Evict the resident package and re-serialize it from the on-disk .uasset.
        // AssumePositive answers the "discard unsaved changes?" prompt so the disk
        // bytes always win — that is the whole point of a from-disk reload.
        TArray<UPackage*> ToReload;
        ToReload.Add(Package);
        FText ReloadError;
        bReloaded = UPackageTools::ReloadPackages(ToReload, ReloadError,
            EReloadPackagesInteractionMode::AssumePositive);
        FailureText = ReloadError.ToString();
        if (bReloaded)
        {
            // Re-resolve the freshly reloaded object (the pre-reload pointer is
            // stale after eviction) so we can report post-load compile state.
            Reloaded = ResolveAsset(AssetPath, /*bLoadObject=*/true).Object;
        }
    }
    else
    {
        // Not currently resident — a single LoadAsset brings the on-disk bytes into
        // memory fresh AND returns the object (no separate LoadPackage needed), which
        // is exactly the from-disk state the caller wants.
        Reloaded = ResolveAsset(AssetPath, /*bLoadObject=*/true).Object;
        bReloaded = (Reloaded != nullptr);
        if (!bReloaded)
        {
            FailureText = TEXT("LoadAsset returned null");
        }
    }

    if (!bReloaded)
    {
        Ctx.SendError(ErrorCodes::ERR_RELOAD_FAILED,
            FailureText.IsEmpty() ? TEXT("Failed to reload package from disk") : FailureText);
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("assetPath"), AssetPath);
    Resp->SetBoolField(TEXT("reloaded"), bReloaded);
    Resp->SetBoolField(TEXT("wasLoaded"), bWasLoaded);

    if (const UBlueprint* BP = Cast<UBlueprint>(Reloaded))
    {
        const bool bCompiled =
            (BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings);
        Resp->SetBoolField(TEXT("compiled"), bCompiled);
        Resp->SetStringField(TEXT("blueprintStatus"), BlueprintStatusToString(BP->Status));
    }

    Ctx.SendSuccess(TEXT("Asset reloaded from disk"), Resp);
    return true;
}
