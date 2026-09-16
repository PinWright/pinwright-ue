// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared policy and response construction for asset.import. Empty destinations
// use AssetTools import. Occupied destinations are admitted only to the narrow
// same-object texture reimport lane proven by the UE 5.8 implementations.
#pragma once

#include "CoreMinimal.h"
#include "EditorFramework/AssetImportData.h"
#include "EditorReimportHandler.h"
#include "Factories/TextureFactory.h"
#include "AssetImportPolicy.generated.h"

class FJsonObject;
class UAutomatedAssetImportData;
class UClass;
class UFactory;
class UObject;
class UPackage;
class UTexture2D;

// UReimportTextureFactory is not exported by UnrealEd. This module-local
// equivalent keeps the audited UE 5.8 behavior that reuses the exact texture
// object instead of routing through UFactory::CreateOrOverwriteAsset.
UCLASS(Transient)
class PINWRIGHT_API UPinWrightTextureReimportFactory final
    : public UTextureFactory
    , public FReimportHandler
{
    GENERATED_BODY()

public:
    UPinWrightTextureReimportFactory(
        const FObjectInitializer& ObjectInitializer);

    void SetTargetTexture(UTexture2D* Texture);
    void SetValidatedSource(
        const FString& SourcePath, TArray<uint8>&& SourceBytes);
    bool WasReimportInvoked() const { return bReimportInvoked; }
    virtual bool CanReimport(
        UObject* Object, TArray<FString>& OutFilenames) override;
    virtual void SetReimportPaths(
        UObject* Object, const TArray<FString>& NewReimportPaths) override;
    virtual EReimportResult::Type Reimport(UObject* Object) override;
    virtual bool IsAutomatedImport() const override;

private:
    virtual UTexture2D* CreateTexture2D(
        UObject* InParent, FName Name, EObjectFlags Flags) override;

    UPROPERTY(Transient)
    TObjectPtr<UTexture2D> OriginalTexture;

    FString ValidatedSourcePath;
    TArray<uint8> ValidatedSourceBytes;
    bool bReimportInvoked = false;
};

namespace AssetImportPolicy
{
    inline constexpr int32 MaxDestinationPackageCount = 64;
    inline constexpr int32 MaxDestinationEntriesExamined = 4096;
    inline constexpr int64 MaxDestinationByteCount = 512ll * 1024ll * 1024ll;
    inline constexpr int64 MaxTextureSourceByteCount = 64ll * 1024ll * 1024ll;
    inline constexpr int64 MaxTextureDecodedByteCount = 256ll * 1024ll * 1024ll;
    inline constexpr int32 MaxDiagnosticReferencerCount = 64;

    struct FDestinationDecision
    {
        bool bRefused = false;
        FString ErrorCode;
    };

    struct FResponse
    {
        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Data;
    };

    // Construct only through BuildValidatedTextureReimportPlan. The handler
    // carries the completed plan as const data so execution consumes the exact
    // bytes that passed the bounded PNG decode preflight.
    class PINWRIGHT_API FValidatedTextureReimportPlan
    {
    public:
        FValidatedTextureReimportPlan() = default;
        bool IsValid() const;
        const FString& GetSourcePath() const { return SourcePath; }
        const TArray<uint8>& GetSourceBytes() const { return SourceBytes; }
        const FString& GetSourceSha1() const { return SourceSha1; }
        int32 GetWidth() const { return Width; }
        int32 GetHeight() const { return Height; }
        int64 GetDecodedByteCount() const { return DecodedByteCount; }

    private:
        friend PINWRIGHT_API bool BuildValidatedTextureReimportPlan(
            const FString& InSourcePath,
            FValidatedTextureReimportPlan& OutPlan,
            FString& OutError);

        FString SourcePath;
        TArray<uint8> SourceBytes;
        FString SourceSha1;
        int32 Width = 0;
        int32 Height = 0;
        int64 DecodedByteCount = 0;
    };

    struct FObjectFingerprint
    {
        UObject* Identity = nullptr;
        UClass* ObjectClass = nullptr;
        UPackage* Package = nullptr;
        FGuid TextureContentId;
        bool bTextureContentIdValid = false;
    };

    struct FPackageResource
    {
        FString Filename;
        int64 Size = 0;
        FDateTime Timestamp;
        FString Sha1;
    };

    struct FDestinationSnapshot
    {
        TMap<FName, FObjectFingerprint> ObjectFingerprints;
        TMap<FString, TArray<FPackageResource>> PackageResources;
    };

    struct FReimportRecoverySnapshot
    {
        FAssetImportInfo SourceMetadata;
        bool bPackageWasDirty = false;
        bool bCaptured = false;
    };

    using FImportRunner = TFunction<TArray<UObject*>(UAutomatedAssetImportData*)>;
    using FBeforeReimportAdapter = TFunction<bool(
        UTexture2D*, const FValidatedTextureReimportPlan&)>;
    using FPostconditionVerifier = TFunction<bool(
        UObject*, const FString&, const FDestinationSnapshot&, FString&)>;
    using FExecutionProbe = TFunction<void()>;

    // Optional dependencies are owned by one request/test call. Production
    // callers pass nullptr and cannot observe another request's test behavior.
    struct FExecutionDependencies
    {
        // Tests may supply a strongly retained per-call factory while still
        // exercising IAssetTools::ImportAssetsAutomated. Production leaves null.
        UFactory* ImportFactory = nullptr;
        FImportRunner ImportRunner;
        FBeforeReimportAdapter BeforeReimportAdapter;
        FPostconditionVerifier PostconditionVerifier;
        FExecutionProbe ImportExecutionProbe;
        FExecutionProbe ReimportExecutionProbe;
        FExecutionProbe ReimportFactoryProbe;
    };

    struct FReimportOutcome
    {
        FReimportRecoverySnapshot RecoverySnapshot;
        bool bSucceeded = false;
        bool bSourceMetadataRestored = false;
        bool bDirtyStateRestored = false;
        bool bAdapterInvoked = false;
        bool bFailureOccurredBeforeContentMutation = false;
        bool bFailureMayHaveMutatedContent = false;
        bool bPackageLeftDirtyForPossibleMutation = false;
        bool bInMemoryContentRestored = false;
        FString FailureReason;
    };

    // General AssetTools import is allowed only for an empty destination.
    PINWRIGHT_API void ConfigureImportData(UAutomatedAssetImportData& ImportData);

    PINWRIGHT_API FDestinationDecision EvaluateDestination(
        bool bDestinationExists, bool bOverwrite);

    PINWRIGHT_API bool BuildValidatedTextureReimportPlan(
        const FString& SourcePath,
        FValidatedTextureReimportPlan& OutPlan,
        FString& OutError);

    // Conservative prefix discovery protects against factories that may publish
    // secondary objects whose names begin with either the source or requested leaf.
    PINWRIGHT_API bool DiscoverDestinationPackages(
        const FString& DestinationPath,
        const FString& RequestedAssetPath,
        const TArray<FString>& DestinationAssetPrefixes,
        TArray<FString>& OutPackageNames,
        FString& OutError);

    PINWRIGHT_API bool ValidateCandidateBudget(
        const TArray<FString>& PackageNames, FString& OutError);

    PINWRIGHT_API bool CaptureDestinationSnapshot(
        const TArray<FString>& PackageNames,
        FDestinationSnapshot& OutSnapshot,
        FString& OutError);

    PINWRIGHT_API FObjectFingerprint CaptureObjectFingerprint(UObject* Object);

    // Returns the exact occupied object only for the proven same-class,
    // same-identity UTexture2D reimport lane. Every other occupied shape fails
    // before either production runner executes.
    PINWRIGHT_API UObject* ResolveSafeInPlaceReimport(
        const FString& RequestedAssetPath,
        const TArray<FString>& DestinationPackages,
        UObject*& OutResolvedObject,
        FString& OutReason);

    PINWRIGHT_API TSharedPtr<FJsonObject> BuildUnsafeOverwriteData(
        const FString& RequestedAssetPath,
        const FString& SourcePath,
        UObject* ExistingObject,
        const FString& Reason,
        const TArray<FString>& Conflicts);

    PINWRIGHT_API TArray<UObject*> RunImport(
        UAutomatedAssetImportData* ImportData,
        const FExecutionDependencies* Dependencies = nullptr);

    // Reuses the pre-adapter request snapshot for both runner failures and
    // handler postflight failures, and reports restoration only after read-back.
    PINWRIGHT_API bool RestoreFailedReimportState(
        UTexture2D* ExistingTexture,
        const FReimportRecoverySnapshot& RecoverySnapshot,
        bool bFailureMayHaveMutatedContent,
        FReimportOutcome& Outcome);

    // Uses the pinned local equivalent of UE 5.8 UReimportTextureFactory and the
    // exact bytes in a completed plan. Source metadata is restored on failure.
    // A previously clean package is restored only for proven pre-content failure;
    // possible adapter mutation is left dirty unless full content rollback exists.
    PINWRIGHT_API FReimportOutcome RunReimport(
        UTexture2D* ExistingTexture,
        const FValidatedTextureReimportPlan& Plan,
        const FExecutionDependencies* Dependencies = nullptr);

    // Primary-only publication for an empty destination. This never displaces an
    // existing object and never performs a global reference walk.
    PINWRIGHT_API bool PublishPrimaryOutput(
        UObject* Object,
        const FString& DestinationPackageName,
        const FString& DestinationObjectName,
        FString& OutError);

    PINWRIGHT_API bool VerifyInPlacePostconditions(
        UObject* ExistingObject,
        const FString& RequestedAssetPath,
        const FDestinationSnapshot& Before,
        FString& OutError);

    // Enumerates every non-null factory output in order. Rename metadata belongs
    // only to the primary output.
    PINWRIGHT_API FResponse BuildResponse(
        const TArray<UObject*>& ImportedObjects,
        const FDestinationSnapshot& Before,
        const FString& AssetPathBeforeRename,
        bool bRenameRequested,
        bool bRenameSucceeded);

    PINWRIGHT_API FResponse BuildInPlaceResponse(
        UObject* ExistingObject,
        const FDestinationSnapshot& Before,
        const FReimportOutcome& ReimportOutcome,
        bool bPostconditionsSucceeded,
        const FString& FailureReason);
}
