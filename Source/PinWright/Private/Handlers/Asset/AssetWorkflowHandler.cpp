// Copyright (c) 2026 Alexander Penkin. MIT License.

// Asset workflow handlers: fixup redirectors, bulk rename/delete, thumbnail,
// LOD generation, Nanite rebuild, source control, report generation.
// Migrated from PinWright_AssetWorkflowHandlers.cpp

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Asset/AssetPathParamUtils.h"
#include "Handlers/Asset/ThumbnailEncodeUtils.h"
#include "Handlers/Asset/ThumbnailFrameEvidence.h"
#include "Handlers/Asset/ThumbnailPreviewOverride.h"
#include "Handlers/Material/MaterialShaderState.h"
#include "Dispatch/ScopedUnattendedRpc.h"
#include "Dispatch/SafePoint.h"
#include "Utils/AssetBatchResult.h"
#include "Utils/AssetCompilePump.h"
#include "Utils/AssetUtils.h"
#include "Utils/MeshRebuildRenderGuard.h"
#include "Utils/RedirectorFixupPolicy.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"

#include "Dom/JsonObject.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Compat/EngineVersionCompat.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "IAssetTools.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "ObjectTools.h"
#include "SourceControlHelpers.h"
#include "SourceControlOperations.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    struct FAssetWorkflowSourceControlItem
    {
        int32 OutcomeIndex = INDEX_NONE;
        FString InputPath;
        FString PackageName;
        FString Filename;
    };

    // Source control needs the package file that actually exists on disk. Asking PackageName
    // which header exists preserves .umap for map packages instead of predicting .uasset.
    static bool AssetWorkflow_TryResolvePackageFilename(
        const FString& PackageName, FString& OutFilename)
    {
        OutFilename.Reset();
        return !PackageName.IsEmpty()
            && FPackageName::DoesPackageExist(PackageName, &OutFilename);
    }

    // Checkout and submit accept the same path array. Resolve every original element once and
    // retain its outcome index so neither handler can silently narrow the request again.
    static TArray<FAssetWorkflowSourceControlItem> AssetWorkflow_PreflightSourceControlItems(
        const TArray<TSharedPtr<FJsonValue>>& Inputs,
        PinWrightAssetBatch::FAssetBatchResult& Batch)
    {
        TArray<FAssetWorkflowSourceControlItem> Work;
        Work.Reserve(Inputs.Num());

        for (int32 InputIndex = 0; InputIndex < Inputs.Num(); ++InputIndex)
        {
            const TSharedPtr<FJsonValue>& Input = Inputs[InputIndex];
            const int32 OutcomeIndex = Batch.AddInput(Input);
            TSharedPtr<FJsonObject> Item = Batch.GetItem(OutcomeIndex);

            if (!Input.IsValid() || Input->Type != EJson::String
                || Input->AsString().IsEmpty())
            {
                Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(
                        TEXT("assetPaths[%d] must be a non-empty string"), InputIndex));
                continue;
            }

            const FString InputPath = Input->AsString();
            Item->SetStringField(TEXT("inputPath"), InputPath);

            const FResolvedAsset Resolved = ResolveAsset(InputPath);
            if (!Resolved.bExists)
            {
                Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_ASSET_NOT_FOUND,
                    Resolved.ErrorMessage.IsEmpty()
                        ? FString::Printf(TEXT("Asset not found: %s"), *InputPath)
                        : Resolved.ErrorMessage);
                continue;
            }

            const FString PackageName = Resolved.PackageName.ToString();
            FString Filename;
            if (!AssetWorkflow_TryResolvePackageFilename(PackageName, Filename))
            {
                Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_INVALID_PATH,
                    FString::Printf(
                        TEXT("Could not resolve an on-disk package filename for: %s"),
                        *InputPath));
                continue;
            }

            FString ResolvedPath = Resolved.ObjectPath.ToString();
            if (ResolvedPath.IsEmpty())
            {
                ResolvedPath = PackageName;
            }
            Item->SetStringField(TEXT("path"), ResolvedPath);
            Item->SetStringField(TEXT("packagePath"), PackageName);
            Item->SetStringField(TEXT("filename"), Filename);
            Batch.MarkReady(OutcomeIndex);

            FAssetWorkflowSourceControlItem& Entry = Work.AddDefaulted_GetRef();
            Entry.OutcomeIndex = OutcomeIndex;
            Entry.InputPath = InputPath;
            Entry.PackageName = PackageName;
            Entry.Filename = Filename;
        }

        return Work;
    }
}

namespace GenerateLodsCompileLifetime
{
    // Retain the same quiesce scope after a timed-out request until every mesh reaches terminal
    // state. generate_lods opts into bounded pumping from this safe core-ticker callback; callers
    // whose request already owned a compile pump can use observation-only retention instead.
    class FRetainedCompileGuard : public TSharedFromThis<FRetainedCompileGuard>
    {
    public:
        FRetainedCompileGuard(
            TSharedPtr<PinWrightMeshRebuild::FQuiesceScope> InQuiesce,
            const TArray<UStaticMesh*>& InMeshes,
            const bool bInPumpCompilation = true,
            const bool bInAllowTestPendingOverride = false)
            : Quiesce(MoveTemp(InQuiesce))
            , bPumpCompilation(bInPumpCompilation)
            , bAllowTestPendingOverride(bInAllowTestPendingOverride)
        {
            Meshes.Reserve(InMeshes.Num());
            for (UStaticMesh* Mesh : InMeshes)
            {
                if (Mesh)
                {
                    Meshes.Emplace(Mesh);
                }
            }
        }

        void Start()
        {
            const TSharedRef<FRetainedCompileGuard> Self = AsShared();
            FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda([Self](float /*DeltaSeconds*/)
                {
                    return Self->Tick();
                }),
                0.01f);
        }

    private:
        bool Tick()
        {
            FScopedUnattendedRpc UnattendedScope;
            bool bAnyCompiling = false;
            for (const TStrongObjectPtr<UStaticMesh>& Mesh : Meshes)
            {
                if (Mesh.IsValid() && PinWright::AssetCompile::IsCompilingForBoundedWait(
                        Mesh.Get(), bAllowTestPendingOverride))
                {
                    bAnyCompiling = true;
                    break;
                }
            }

            if (!bAnyCompiling)
            {
                // Release the render guard while the unattended scope is still active and only
                // after the manager reports every retained mesh as terminal.
                Quiesce.Reset();
                return false;
            }

            if (bPumpCompilation)
            {
                PinWright::AssetCompile::AdvanceOnGameThread();
            }
            return true;
        }

        TSharedPtr<PinWrightMeshRebuild::FQuiesceScope> Quiesce;
        TArray<TStrongObjectPtr<UStaticMesh>> Meshes;
        bool bPumpCompilation = true;
        bool bAllowTestPendingOverride = false;
    };

}

// ============================================================================
// asset.fixup_redirectors
// ============================================================================
REGISTER_RPC_HANDLER("asset.fixup_redirectors", "asset", "Walk redirectors left behind by asset.rename / asset.move, re-save assets that reference them with the resolved target path, then delete the redirectors. Run after batches of renames/moves to keep the asset graph clean.",
    RPC_PARAMS(
        RPC_PARAM_OPT("directoryPath", "path", "Content folder to scan (e.g. /Game/Foo); empty (default) scans the entire project."),
        RPC_PARAM_OPT("checkoutFiles", "boolean", "When true, attempts source-control checkout for any read-only referencing assets before re-saving; defaults to false.")
    ))
{
    FString DirectoryPath = Ctx.GetString(TEXT("directoryPath"));
    bool bCheckoutFiles = Ctx.GetBool(TEXT("checkoutFiles"), false);

    {
        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        // DECISION (B-tests-destroy-host-assets, 2026-08-21): the empty-directoryPath
        // default stays project-wide here, and was NOT narrowed alongside
        // asset.bulk_delete's. The two verbs are not the same case. This one's entire
        // job is the redirector sweep, so the caller who typed its name is asking for
        // exactly this; it takes an explicit scope parameter, so a caller can bound it;
        // and the default is documented (docs/wiki-src/asset.md). bulk_delete had none of
        // the three - it was named for a delete, exposed no scope, and ran the widest
        // possible sweep as a side effect. Narrowing this default would also break a
        // published contract ("empty scans the entire project") for callers who rely on
        // the post-batch cleanup, to remove a hazard the caller has already opted into.
        // Re-open the decision only if the field shows callers reaching this default by
        // accident rather than on purpose.
        FARFilter Filter;
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/CoreUObject"), TEXT("ObjectRedirector")));

        if (!DirectoryPath.IsEmpty())
        {
            FString NormalizedPath = DirectoryPath;
            if (NormalizedPath.StartsWith(TEXT("/Content"), ESearchCase::IgnoreCase))
            {
                NormalizedPath = FString::Printf(TEXT("/Game%s"), *NormalizedPath.RightChop(8));
            }
            Filter.PackagePaths.Add(FName(*NormalizedPath));
            Filter.bRecursivePaths = true;
        }

        TArray<FAssetData> RedirectorAssets;
        AssetRegistry.GetAssets(Filter, RedirectorAssets);

        if (RedirectorAssets.Num() == 0)
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("success"), true);
            Result->SetNumberField(TEXT("redirectorsFound"), 0);
            Result->SetNumberField(TEXT("redirectorsFixed"), 0);
            Ctx.SendSuccess(TEXT("No redirectors found"), Result);
            return true;
        }

        TArray<FString> RedirectorPaths;
        for (const FAssetData& Asset : RedirectorAssets)
        {
            RedirectorPaths.Add(Asset.ToSoftObjectPath().ToString());
        }

        if (bCheckoutFiles && ISourceControlModule::Get().IsEnabled())
        {
            ISourceControlProvider& SourceControlProvider =
                ISourceControlModule::Get().GetProvider();
            TArray<FString> PackageNames;
            for (const FAssetData& Asset : RedirectorAssets)
            {
                PackageNames.Add(Asset.PackageName.ToString());
            }
            SourceControlHelpers::CheckOutFiles(PackageNames, true);
        }

        TArray<UObjectRedirector*> Redirectors;
        for (const FAssetData& Asset : RedirectorAssets)
        {
            if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Asset.GetAsset()))
            {
                Redirectors.Add(Redirector);
            }
        }

        // RedirectorFixupPolicy, not IAssetTools::FixupReferencers: the engine call ends
        // in an unconditional modal report whose result is read with an unchecked
        // TOptional::GetValue(), which kills the editor once the unattended scope
        // cancels the window. See Utils/RedirectorFixupPolicy.h for the full chain.
        //
        // It also replaces the blanket delete that used to follow: this body deleted
        // every redirector it found regardless of whether its referencers had been
        // re-saved, which silently broke any reference the fixup could not repair. The
        // policy deletes only fully fixed-up redirectors.
        const RedirectorFixupPolicy::FResult Fixup =
            RedirectorFixupPolicy::FixupReferencers(Redirectors, /*bDeleteFixedUpRedirectors=*/true);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        Result->SetNumberField(TEXT("redirectorsFound"), RedirectorAssets.Num());
        Result->SetNumberField(TEXT("redirectorsFixed"), Fixup.RedirectorsDeleted);
        RedirectorFixupPolicy::AddReport(Result, Fixup);

        Ctx.SendSuccess(
            FString::Printf(TEXT("Fixed %d of %d redirectors"),
                Fixup.RedirectorsDeleted, RedirectorAssets.Num()),
            Result);
    }

    return true;
}

// ============================================================================
// asset.source_control_checkout
// ============================================================================
REGISTER_RPC_HANDLER("asset.source_control_checkout", "asset", "Check out assets from source control",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPaths", "array", "Array of asset paths to check out"),
        RPC_PARAM_OPT("partial", "boolean", "Process valid assets when another item fails preflight. Defaults to false, which refuses the whole batch before checkout.")
    ))
{
    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();

    const TArray<TSharedPtr<FJsonValue>>* AssetPathsArray = nullptr;
    if (!Payload->TryGetArrayField(TEXT("assetPaths"), AssetPathsArray) ||
        !AssetPathsArray || AssetPathsArray->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("assetPaths array required"));
        return true;
    }

    const bool bAllowPartial = Ctx.GetBool(TEXT("partial"), false);
    PinWrightAssetBatch::FAssetBatchResult Batch(bAllowPartial);
    const TArray<FAssetWorkflowSourceControlItem> Work =
        AssetWorkflow_PreflightSourceControlItems(*AssetPathsArray, Batch);

    if (!ISourceControlModule::Get().IsEnabled())
    {
        for (const FAssetWorkflowSourceControlItem& Entry : Work)
        {
            if (Batch.IsReady(Entry.OutcomeIndex))
            {
                Batch.FailPreflight(Entry.OutcomeIndex,
                    ErrorCodes::ERR_SOURCE_CONTROL_DISABLED,
                    TEXT("Source control is not enabled"));
            }
        }
    }

    if (Batch.ShouldRefuseBeforeMutation())
    {
        Batch.RefuseReadyItems(ErrorCodes::ERR_BATCH_NOT_ATTEMPTED,
            TEXT("Not attempted because another batch item failed preflight"));
        TSharedPtr<FJsonObject> Result = Batch.MakeResult();
        Result->SetNumberField(TEXT("checkedOut"), 0);
        Result->SetArrayField(TEXT("assets"), TArray<TSharedPtr<FJsonValue>>());
        Ctx.SendError(ErrorCodes::ERR_BATCH_PREFLIGHT_FAILED,
            TEXT("Checkout batch failed preflight; no assets were changed"), Result);
        return true;
    }

    if (!Batch.HasReadyItems())
    {
        TSharedPtr<FJsonObject> Result = Batch.MakeResult();
        Result->SetNumberField(TEXT("checkedOut"), 0);
        Result->SetArrayField(TEXT("assets"), TArray<TSharedPtr<FJsonValue>>());
        Ctx.SendError(ErrorCodes::ERR_BATCH_PREFLIGHT_FAILED,
            TEXT("Checkout batch contains no valid assets"), Result);
        return true;
    }

    TArray<FString> PackageNames;
    for (const FAssetWorkflowSourceControlItem& Entry : Work)
    {
        if (Batch.IsReady(Entry.OutcomeIndex))
        {
            PackageNames.Add(Entry.PackageName);
        }
    }

    const bool bProviderSucceeded =
        SourceControlHelpers::CheckOutFiles(PackageNames, true);
    ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

    TArray<TSharedPtr<FJsonValue>> CheckedOutPaths;
    for (const FAssetWorkflowSourceControlItem& Entry : Work)
    {
        if (!Batch.IsReady(Entry.OutcomeIndex))
        {
            continue;
        }

        const FSourceControlStatePtr State =
            Provider.GetState(Entry.Filename, EStateCacheUsage::ForceUpdate);
        const bool bExplicitlyCheckedOut =
            State.IsValid() && (State->IsCheckedOut() || State->IsAdded());
        const bool bProviderAcceptedWithoutCheckout =
            bProviderSucceeded && State.IsValid() && State->IsSourceControlled()
            && !State->CanCheckout() && !State->IsCheckedOutOther();
        const bool bCheckedOut =
            bExplicitlyCheckedOut || bProviderAcceptedWithoutCheckout;

        TSharedPtr<FJsonObject> Item = Batch.GetItem(Entry.OutcomeIndex);
        Item->SetBoolField(TEXT("stateReadback"), State.IsValid());
        Item->SetBoolField(TEXT("checkedOut"), bCheckedOut);
        if (bCheckedOut)
        {
            Batch.MarkSucceeded(Entry.OutcomeIndex);
            // Preserve the legacy assets[] echo; the canonical readback path lives on the item.
            CheckedOutPaths.Add(MakeShared<FJsonValueString>(Entry.InputPath));
        }
        else
        {
            Batch.FailRuntime(Entry.OutcomeIndex, ErrorCodes::ERR_CHECKOUT_FAILED,
                State.IsValid()
                    ? FString::Printf(TEXT("Checkout was not observed for %s"), *Entry.InputPath)
                    : FString::Printf(
                        TEXT("Checkout state readback was unavailable for %s"),
                        *Entry.InputPath));
        }
    }

    TSharedPtr<FJsonObject> Result = Batch.MakeResult();
    Result->SetNumberField(TEXT("checkedOut"), Batch.NumSucceeded());
    Result->SetArrayField(TEXT("assets"), CheckedOutPaths);

    if (Batch.IsEnvelopeSuccess())
    {
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_CHECKOUT_FAILED,
            TEXT("One or more assets were not checked out; see items"), Result);
    }
    return true;
}

// ============================================================================
// asset.source_control_submit
// ============================================================================
REGISTER_RPC_HANDLER("asset.source_control_submit", "asset", "Submit assets to source control",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPaths", "array", "Array of asset paths to submit"),
        RPC_PARAM_OPT("description", "string", "Changelist description"),
        RPC_PARAM_OPT("partial", "boolean", "Submit valid assets when another item fails preflight. Defaults to false, which refuses the whole batch before submit.")
    ))
{
    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();

    const TArray<TSharedPtr<FJsonValue>>* AssetPathsArray = nullptr;
    if (!Payload->TryGetArrayField(TEXT("assetPaths"), AssetPathsArray) ||
        !AssetPathsArray || AssetPathsArray->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("assetPaths array required"));
        return true;
    }

    FString Description = Ctx.GetString(TEXT("description"));
    if (Description.IsEmpty())
    {
        Description = TEXT("Automated submission via PinWright");
    }

    const bool bAllowPartial = Ctx.GetBool(TEXT("partial"), false);
    PinWrightAssetBatch::FAssetBatchResult Batch(bAllowPartial);
    const TArray<FAssetWorkflowSourceControlItem> Work =
        AssetWorkflow_PreflightSourceControlItems(*AssetPathsArray, Batch);

    if (!ISourceControlModule::Get().IsEnabled())
    {
        for (const FAssetWorkflowSourceControlItem& Entry : Work)
        {
            if (Batch.IsReady(Entry.OutcomeIndex))
            {
                Batch.FailPreflight(Entry.OutcomeIndex,
                    ErrorCodes::ERR_SOURCE_CONTROL_DISABLED,
                    TEXT("Source control is not enabled"));
            }
        }
    }

    if (Batch.ShouldRefuseBeforeMutation())
    {
        Batch.RefuseReadyItems(ErrorCodes::ERR_BATCH_NOT_ATTEMPTED,
            TEXT("Not attempted because another batch item failed preflight"));
        TSharedPtr<FJsonObject> Result = Batch.MakeResult();
        Result->SetNumberField(TEXT("submitted"), 0);
        Result->SetStringField(TEXT("description"), Description);
        Ctx.SendError(ErrorCodes::ERR_BATCH_PREFLIGHT_FAILED,
            TEXT("Submit batch failed preflight; no assets were submitted"), Result);
        return true;
    }

    if (!Batch.HasReadyItems())
    {
        TSharedPtr<FJsonObject> Result = Batch.MakeResult();
        Result->SetNumberField(TEXT("submitted"), 0);
        Result->SetStringField(TEXT("description"), Description);
        Ctx.SendError(ErrorCodes::ERR_BATCH_PREFLIGHT_FAILED,
            TEXT("Submit batch contains no valid assets"), Result);
        return true;
    }

    TArray<FString> FilePaths;
    TMap<int32, bool> PendingBeforeSubmit;
    TSet<int32> ValidStateBeforeSubmit;
    ISourceControlProvider& SourceControlProvider =
        ISourceControlModule::Get().GetProvider();
    for (const FAssetWorkflowSourceControlItem& Entry : Work)
    {
        if (!Batch.IsReady(Entry.OutcomeIndex))
        {
            continue;
        }

        FilePaths.Add(Entry.Filename);
        const FSourceControlStatePtr State =
            SourceControlProvider.GetState(Entry.Filename, EStateCacheUsage::ForceUpdate);
        if (State.IsValid())
        {
            ValidStateBeforeSubmit.Add(Entry.OutcomeIndex);
            PendingBeforeSubmit.Add(Entry.OutcomeIndex,
                State->IsModified() || State->IsAdded() || State->IsDeleted()
                || State->CanCheckIn());
        }
    }

    TSharedRef<FCheckIn, ESPMode::ThreadSafe> CheckInOperation =
        ISourceControlOperation::Create<FCheckIn>();
    CheckInOperation->SetDescription(FText::FromString(Description));

    const ECommandResult::Type ProviderResult =
        SourceControlProvider.Execute(CheckInOperation, FilePaths);
    const bool bProviderSucceeded = ProviderResult == ECommandResult::Succeeded;

    for (const FAssetWorkflowSourceControlItem& Entry : Work)
    {
        if (!Batch.IsReady(Entry.OutcomeIndex))
        {
            continue;
        }

        const FSourceControlStatePtr State =
            SourceControlProvider.GetState(Entry.Filename, EStateCacheUsage::ForceUpdate);
        const bool bPendingAfter = State.IsValid()
            && (State->IsModified() || State->IsAdded() || State->IsDeleted()
                || State->CanCheckIn());
        const bool bWasPending =
            PendingBeforeSubmit.FindRef(Entry.OutcomeIndex);
        const bool bReadbackShowsSubmission = State.IsValid() && !bPendingAfter
            && (bProviderSucceeded
                || (ValidStateBeforeSubmit.Contains(Entry.OutcomeIndex) && bWasPending));

        TSharedPtr<FJsonObject> Item = Batch.GetItem(Entry.OutcomeIndex);
        Item->SetBoolField(TEXT("stateReadback"), State.IsValid());
        Item->SetBoolField(TEXT("pendingBefore"), bWasPending);
        Item->SetBoolField(TEXT("pendingAfter"), bPendingAfter);
        Item->SetBoolField(TEXT("submitted"), bReadbackShowsSubmission);
        if (bReadbackShowsSubmission)
        {
            Batch.MarkSucceeded(Entry.OutcomeIndex);
        }
        else
        {
            Batch.FailRuntime(Entry.OutcomeIndex, ErrorCodes::ERR_SUBMIT_FAILED,
                State.IsValid()
                    ? FString::Printf(TEXT("Submit did not clear pending state for %s"),
                        *Entry.InputPath)
                    : FString::Printf(TEXT("Submit state readback was unavailable for %s"),
                        *Entry.InputPath));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = Batch.MakeResult();
    ResultObj->SetNumberField(TEXT("submitted"), Batch.NumSucceeded());
    ResultObj->SetStringField(TEXT("description"), Description);

    if (Batch.IsEnvelopeSuccess())
    {
        Ctx.SendSuccess(ResultObj);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_SUBMIT_FAILED,
            TEXT("One or more assets were not submitted; see items"), ResultObj);
    }
    return true;
}

// ============================================================================
// asset.bulk_rename
// ============================================================================
REGISTER_RPC_HANDLER("asset.bulk_rename", "asset", "Apply prefix/suffix/search-replace transformations to many asset names in one call. Each rename creates a redirector at the old path; follow with asset.fixup_redirectors. Operations are applied in order: search-replace FIRST, then prefix, then suffix. NOT idempotent - prefix/suffix are added unconditionally, so re-running the same call double-prefixes (SM_SM_Rock). Preview on one asset before running a batch.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPaths", "array", "Array of asset paths to rename (e.g. ['/Game/Foo/SM_A','/Game/Foo/SM_B'])."),
        RPC_PARAM_OPT("prefix", "string", "String prepended to each asset's leaf name. Applied UNCONDITIONALLY - there is no already-present check, so re-running produces SM_SM_Rock. Applied AFTER search-replace."),
        RPC_PARAM_OPT("suffix", "string", "String appended to each asset's leaf name. Applied UNCONDITIONALLY - there is no already-present check, so re-running produces Rock_LOD0_LOD0. Applied last."),
        RPC_PARAM_OPT("searchText", "string", "Substring to find in each leaf name, matched case-INsensitively (ESearchCase::IgnoreCase), so 'sm_' also hits 'SM_'. Applied BEFORE prefix/suffix."),
        RPC_PARAM_OPT("replaceText", "string", "Replacement for searchText hits; empty string deletes the substring."),
        RPC_PARAM_OPT("checkoutFiles", "boolean", "Attempt source-control checkout before renaming when true; defaults to false."),
        RPC_PARAM_OPT("partial", "boolean", "Rename valid assets when another item fails preflight. Defaults to false, which refuses the whole batch before rename.")
    ))
{
    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();

    const TArray<TSharedPtr<FJsonValue>>* AssetPathsArray = nullptr;
    if (!Payload->TryGetArrayField(TEXT("assetPaths"), AssetPathsArray) ||
        !AssetPathsArray || AssetPathsArray->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("assetPaths array required"));
        return true;
    }

    FString Prefix, Suffix, SearchText, ReplaceText;
    Payload->TryGetStringField(TEXT("prefix"), Prefix);
    Payload->TryGetStringField(TEXT("suffix"), Suffix);
    Payload->TryGetStringField(TEXT("searchText"), SearchText);
    Payload->TryGetStringField(TEXT("replaceText"), ReplaceText);

    bool bCheckoutFiles = false;
    Payload->TryGetBoolField(TEXT("checkoutFiles"), bCheckoutFiles);

    const bool bAllowPartial = Ctx.GetBool(TEXT("partial"), false);
    PinWrightAssetBatch::FAssetBatchResult Batch(bAllowPartial);

    struct FRenameWork
    {
        int32 OutcomeIndex = INDEX_NONE;
        UObject* Asset = nullptr;
        FString PackageName;
        FString PackagePath;
        FString Filename;
        FString OldPath;
        FString NewPath;
        FString NewName;
    };

    TArray<FRenameWork> Work;
    Work.Reserve(AssetPathsArray->Num());
    TSet<FString> RequestedTargets;

    for (int32 InputIndex = 0; InputIndex < AssetPathsArray->Num(); ++InputIndex)
    {
        const TSharedPtr<FJsonValue>& Input = (*AssetPathsArray)[InputIndex];
        const int32 OutcomeIndex = Batch.AddInput(Input);
        TSharedPtr<FJsonObject> Item = Batch.GetItem(OutcomeIndex);

        if (!Input.IsValid() || Input->Type != EJson::String
            || Input->AsString().IsEmpty())
        {
            Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("assetPaths[%d] must be a non-empty string"),
                    InputIndex));
            continue;
        }

        const FString InputPath = Input->AsString();
        Item->SetStringField(TEXT("inputPath"), InputPath);
        const FResolvedAsset Resolved = ResolveAsset(InputPath, /*bLoadObject=*/true);
        UObject* Asset = Resolved.Object;
        if (!Resolved.bExists || !Asset)
        {
            Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_ASSET_NOT_FOUND,
                Resolved.ErrorMessage.IsEmpty()
                    ? FString::Printf(TEXT("Asset not found: %s"), *InputPath)
                    : Resolved.ErrorMessage);
            continue;
        }

        // Capture this before AssetTools mutates Asset; sampling the pointer afterwards reports
        // the new path and was the original oldPath bug.
        const FString OldPath = Asset->GetPathName();
        const FString PackageName = Asset->GetOutermost()->GetName();
        const FString PackagePath = FPackageName::GetLongPackagePath(PackageName);
        const FString CurrentName = Asset->GetName();
        FString NewName = CurrentName;

        if (!SearchText.IsEmpty())
        {
            NewName = NewName.Replace(*SearchText, *ReplaceText, ESearchCase::IgnoreCase);
        }
        if (!Prefix.IsEmpty())
        {
            NewName = Prefix + NewName;
        }
        if (!Suffix.IsEmpty())
        {
            NewName += Suffix;
        }

        const FString NewPackageName = PackagePath / NewName;
        const FString NewPath = FString::Printf(TEXT("%s.%s"), *NewPackageName, *NewName);
        Item->SetStringField(TEXT("oldPath"), OldPath);
        Item->SetStringField(TEXT("newPath"), NewPath);
        Item->SetStringField(TEXT("newName"), NewName);
        Item->SetBoolField(TEXT("changed"), false);

        if (NewName == CurrentName)
        {
            Batch.MarkSucceeded(OutcomeIndex, /*bAttempted=*/false);
            continue;
        }
        if (NewName.IsEmpty())
        {
            Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_INVALID_PATH,
                FString::Printf(TEXT("Rename target for %s has an empty asset name"),
                    *OldPath));
            continue;
        }

        FText ValidationReason;
        if (!FName::IsValidXName(NewName, INVALID_OBJECTNAME_CHARACTERS,
                &ValidationReason)
            || !FPackageName::IsValidLongPackageName(NewPackageName,
                /*bIncludeReadOnlyRoots=*/true, &ValidationReason))
        {
            Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_INVALID_PATH,
                FString::Printf(TEXT("Invalid rename target %s: %s"), *NewPath,
                    *ValidationReason.ToString()));
            continue;
        }

        if (RequestedTargets.Contains(NewPath))
        {
            Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_DESTINATION_EXISTS,
                FString::Printf(TEXT("More than one item targets %s"), *NewPath));
            continue;
        }
        RequestedTargets.Add(NewPath);

        const FResolvedAsset ExistingTarget = ResolveAsset(NewPath);
        if (ExistingTarget.bExists || DoesPackageFileExistOnDisk(NewPackageName))
        {
            Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_DESTINATION_EXISTS,
                FString::Printf(TEXT("Rename destination already exists: %s"), *NewPath));
            continue;
        }

        FString Filename;
        if (bCheckoutFiles
            && !AssetWorkflow_TryResolvePackageFilename(PackageName, Filename))
        {
            Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_INVALID_PATH,
                FString::Printf(TEXT("Could not resolve an on-disk package filename for %s"),
                    *OldPath));
            continue;
        }

        Batch.MarkReady(OutcomeIndex);
        FRenameWork& Entry = Work.AddDefaulted_GetRef();
        Entry.OutcomeIndex = OutcomeIndex;
        Entry.Asset = Asset;
        Entry.PackageName = PackageName;
        Entry.PackagePath = PackagePath;
        Entry.Filename = Filename;
        Entry.OldPath = OldPath;
        Entry.NewPath = NewPath;
        Entry.NewName = NewName;
    }

    if (bCheckoutFiles && !ISourceControlModule::Get().IsEnabled())
    {
        for (const FRenameWork& Entry : Work)
        {
            if (Batch.IsReady(Entry.OutcomeIndex))
            {
                Batch.FailPreflight(Entry.OutcomeIndex,
                    ErrorCodes::ERR_SOURCE_CONTROL_DISABLED,
                    TEXT("Source control is not enabled"));
            }
        }
    }

    if (Batch.ShouldRefuseBeforeMutation())
    {
        Batch.RefuseReadyItems(ErrorCodes::ERR_BATCH_NOT_ATTEMPTED,
            TEXT("Not attempted because another batch item failed preflight"));
        TSharedPtr<FJsonObject> Result = Batch.MakeResult();
        Result->SetNumberField(TEXT("renamed"), 0);
        Result->SetArrayField(TEXT("assets"), TArray<TSharedPtr<FJsonValue>>());
        Ctx.SendError(ErrorCodes::ERR_BATCH_PREFLIGHT_FAILED,
            TEXT("Rename batch failed preflight; no assets were renamed"), Result);
        return true;
    }

    if (bCheckoutFiles && Batch.HasReadyItems())
    {
        TArray<FString> PackageNames;
        for (const FRenameWork& Entry : Work)
        {
            if (Batch.IsReady(Entry.OutcomeIndex))
            {
                PackageNames.Add(Entry.PackageName);
            }
        }

        const bool bCheckoutReported = SourceControlHelpers::CheckOutFiles(PackageNames, true);
        ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
        for (const FRenameWork& Entry : Work)
        {
            if (!Batch.IsReady(Entry.OutcomeIndex))
            {
                continue;
            }

            const FSourceControlStatePtr State =
                Provider.GetState(Entry.Filename, EStateCacheUsage::ForceUpdate);
            const bool bCheckoutObserved = State.IsValid()
                && (State->IsCheckedOut() || State->IsAdded()
                    || (bCheckoutReported && State->IsSourceControlled()
                        && !State->CanCheckout() && !State->IsCheckedOutOther()));
            Batch.GetItem(Entry.OutcomeIndex)->SetBoolField(
                TEXT("checkoutObserved"), bCheckoutObserved);
            if (!bCheckoutObserved)
            {
                Batch.FailRuntime(Entry.OutcomeIndex, ErrorCodes::ERR_CHECKOUT_FAILED,
                    FString::Printf(TEXT("Checkout was not observed for %s"),
                        *Entry.OldPath));
            }
        }
    }

    IAssetTools& AssetTools =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    TArray<TSharedPtr<FJsonValue>> RenamedAssets;
    int32 RenamedCount = 0;

    for (const FRenameWork& Entry : Work)
    {
        if (!Batch.IsReady(Entry.OutcomeIndex))
        {
            continue;
        }

        TArray<FAssetRenameData> RenameData;
        RenameData.Emplace(Entry.Asset, Entry.PackagePath, Entry.NewName);
        const bool bRenameReported = AssetTools.RenameAssets(RenameData);
        const FString ActualPath = Entry.Asset ? Entry.Asset->GetPathName() : FString();
        const bool bRenamed = bRenameReported && ActualPath == Entry.NewPath;

        TSharedPtr<FJsonObject> Item = Batch.GetItem(Entry.OutcomeIndex);
        Item->SetBoolField(TEXT("renameReported"), bRenameReported);
        Item->SetStringField(TEXT("actualPath"), ActualPath);
        Item->SetBoolField(TEXT("changed"), bRenamed);
        if (bRenamed)
        {
            Batch.MarkSucceeded(Entry.OutcomeIndex);
            ++RenamedCount;

            TSharedPtr<FJsonObject> AssetInfo = MakeShared<FJsonObject>();
            AssetInfo->SetStringField(TEXT("oldPath"), Entry.OldPath);
            AssetInfo->SetStringField(TEXT("newPath"), ActualPath);
            AssetInfo->SetStringField(TEXT("newName"), Entry.NewName);
            RenamedAssets.Add(MakeShared<FJsonValueObject>(AssetInfo));
        }
        else
        {
            Batch.FailRuntime(Entry.OutcomeIndex, ErrorCodes::ERR_BULK_RENAME_FAILED,
                FString::Printf(TEXT("Rename was not observed from %s to %s"),
                    *Entry.OldPath, *Entry.NewPath));
        }
    }

    TSharedPtr<FJsonObject> Result = Batch.MakeResult();
    Result->SetNumberField(TEXT("renamed"), RenamedCount);
    Result->SetArrayField(TEXT("assets"), RenamedAssets);
    if (RenamedCount == 0 && Batch.IsEnvelopeSuccess())
    {
        Result->SetStringField(TEXT("message"), TEXT("No assets required renaming"));
    }

    if (Batch.IsEnvelopeSuccess())
    {
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_BULK_RENAME_FAILED,
            TEXT("One or more assets were not renamed; see items"), Result);
    }
    return true;
}

// ============================================================================
// asset.bulk_delete
// ============================================================================
REGISTER_RPC_HANDLER("asset.bulk_delete", "asset", "Delete many assets in a single call. By default also runs a redirector fixup scoped to the deleted assets' own folders, so the deletions don't leave dangling redirectors beside them. Returns per-asset success arrays plus the redirector packages the fixup removed.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPaths", "array", "Array of asset paths to delete (e.g. ['/Game/Foo/SM_A','/Game/Foo/SM_B'])."),
        RPC_PARAM_OPT("showConfirmation", "boolean", "When true the editor shows the standard delete confirmation dialog; defaults to false (silent delete)."),
        RPC_PARAM_OPT("fixupRedirectors", "boolean", "Run redirector fixup automatically after deletion; defaults to true."),
        RPC_PARAM_DEF("fixupScope", "string", "How wide the redirector fixup may reach. 'paths' (default) sweeps only the package folders the deleted assets live in, non-recursively. 'project' sweeps every mounted content root, which fixes up and DELETES every ObjectRedirector in the project including ones unrelated to this call - opt in deliberately. Any other value is rejected. Redirectors removed outside the deleted assets' folders are always listed in redirectorsDeletedOutsideScope.", "paths")
    ))
{
    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();

    const TArray<TSharedPtr<FJsonValue>>* AssetPathsArray = nullptr;
    if (!Payload->TryGetArrayField(TEXT("assetPaths"), AssetPathsArray) ||
        !AssetPathsArray || AssetPathsArray->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("assetPaths array required"));
        return true;
    }

    bool bShowConfirmation = false;
    Payload->TryGetBoolField(TEXT("showConfirmation"), bShowConfirmation);

    bool bFixupRedirectors = true;
    Payload->TryGetBoolField(TEXT("fixupRedirectors"), bFixupRedirectors);

    // Parsed before anything is deleted. A bad scope rejected AFTER the delete would
    // charge the caller an irreversible half of the operation for a typo, and there is no
    // safe reading of an unrecognised value here - the wrong guess is a project-wide
    // purge (docs/rpc-design.md §3).
    FString FixupScopeArg = TEXT("paths");
    Payload->TryGetStringField(TEXT("fixupScope"), FixupScopeArg);
    RedirectorFixupPolicy::ESweepScope FixupScope =
        RedirectorFixupPolicy::ESweepScope::RequestedPaths;
    if (!RedirectorFixupPolicy::ParseSweepScope(FixupScopeArg, FixupScope))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(
                TEXT("fixupScope must be 'paths' (the deleted assets' own folders) or ")
                TEXT("'project' (every mounted content root); got '%s'"),
                *FixupScopeArg));
        return true;
    }

    TArray<FString> AssetPaths;
    for (const TSharedPtr<FJsonValue>& Val : *AssetPathsArray)
    {
        if (Val.IsValid() && Val->Type == EJson::String)
        {
            AssetPaths.Add(Val->AsString());
        }
    }

    // One record per path the CALLER named, classified before anything is mutated.
    // bExistedBefore has to be sampled now: a typo'd path probes as "gone" afterwards and
    // would otherwise be scored as a deletion this call performed. bAttempted records
    // whether the path was actually handed to the engine - a path dropped by the
    // DoesAssetExist / LoadAsset filter below used to vanish from both ValidPaths and the
    // reported `requested`, leaving the caller no field that could say it was never tried.
    struct FRequestedDelete
    {
        FString Path;
        bool bExistedBefore = false;
        bool bAttempted = false;
    };

    TArray<UObject*> ObjectsToDelete;
    TArray<FString> ValidPaths;
    TArray<FRequestedDelete> RequestedDeletes;
    RequestedDeletes.Reserve(AssetPaths.Num());

    for (const FString& AssetPath : AssetPaths)
    {
        FRequestedDelete Requested;
        Requested.Path = AssetPath;
        const FResolvedAsset Resolved = ResolveAsset(AssetPath, /*bLoadObject=*/true);
        Requested.bExistedBefore = Resolved.bExists;

        if (Resolved.Object)
        {
            ObjectsToDelete.Add(Resolved.Object);
            ValidPaths.Add(AssetPath);
            Requested.bAttempted = true;
        }
        RequestedDeletes.Add(MoveTemp(Requested));
    }

    if (ObjectsToDelete.Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_VALID_ASSETS, TEXT("No valid assets found"));
        return true;
    }

    // The engine's return is a COUNT and nothing in it is per-path, so it is recorded as a
    // claim and reconciled against a probe below - never emitted as the outcome. On this
    // route (DeleteObjects -> DeleteItems -> FAssetDeleteModel::DoDelete ->
    // DeleteObjectsUnchecked) an object is skipped silently when
    // ObjectTools::MakeReadOnlyPackageWritable answers its own dialog with the unattended
    // default, or when an FEditorDelegates::OnAssetsCanDelete handler refuses it inside
    // DeleteSingleObject; neither logs. CleanupAfterSuccessfulDelete then runs AFTER the
    // count is final and drops any package whose filename FPackageName::DoesPackageExist
    // cannot resolve, and discards the return of IFileManager::Delete - both leave a
    // .uasset the count already scored as deleted. In the other direction
    // AddExtraObjectsToDelete appends secondary and external-package objects, so the count
    // can exceed the request outright.
    const int32 EngineDeletedCount =
        ObjectTools::DeleteObjects(ObjectsToDelete, bShowConfirmation);

    // The folders the caller actually named. Kept outside the fixup branch because the
    // collateral report below is computed against them either way.
    const TArray<FName> RequestedFolders =
        RedirectorFixupPolicy::PackageFoldersForAssets(ValidPaths);

    RedirectorFixupPolicy::FResult FixupResult;
    if (bFixupRedirectors && EngineDeletedCount > 0)
    {
        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        // Scoped by construction. This filter used to carry ClassPaths only, which made
        // GetAssets return every ObjectRedirector in the project - all of which were then
        // fixed up and DELETED. It destroyed four packages in the development project
        // and rewrote thirteen more that nobody had named; see
        // Utils/RedirectorFixupPolicy.h ESweepScope, board B-tests-destroy-host-assets,
        // and docs/defect-backlog.md D-72. The wide sweep is still reachable, but
        // only through fixupScope:"project".
        const FARFilter Filter =
            RedirectorFixupPolicy::BuildSweepFilter(ValidPaths, FixupScope);

        TArray<FAssetData> RedirectorAssets;
        AssetRegistry.GetAssets(Filter, RedirectorAssets);

        if (RedirectorAssets.Num() > 0)
        {
            TArray<UObjectRedirector*> Redirectors;
            for (const FAssetData& Asset : RedirectorAssets)
            {
                if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Asset.GetAsset()))
                {
                    Redirectors.Add(Redirector);
                }
            }

            // Never IAssetTools::FixupReferencers - it ends in a modal report that
            // hard-asserts once the unattended scope cancels it, which is the crash
            // this call site produced. See Utils/RedirectorFixupPolicy.h.
            FixupResult = RedirectorFixupPolicy::FixupReferencers(
                Redirectors, /*bDeleteFixedUpRedirectors=*/true);
        }
    }

    auto MakeStringArray = [](const TArray<FString>& Items)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FString& Item : Items)
        {
            Array.Add(MakeShared<FJsonValueString>(Item));
        }
        return Array;
    };

    // Reconcile every requested path against the world, not against the request. deleted[]
    // used to be ValidPaths - the caller's own list, filled in before the delete ran and
    // never revisited - so a partial batch was not merely unreported, it was
    // unrepresentable: one deletion out of twenty read as success with all twenty names in
    // deleted[].
    int32 DeletedCount = 0;
    TArray<FString> DeletedPaths;
    TArray<FString> FailedPaths;
    TArray<FString> MissingPaths;
    TArray<TSharedPtr<FJsonValue>> EntryResults;
    bool bAnySurvived = false;
    bool bAnyDiverged = false;

    for (const FRequestedDelete& Requested : RequestedDeletes)
    {
        // Two probes, not one. DoesAssetExist asks the asset REGISTRY; whether the .uasset
        // is still on disk is a separate question, and the engine's cleanup can leave one
        // without the other. existsAfter is the union - anything still on disk is still
        // there, whatever the registry now says.
        const bool bStillRegistered = ResolveAsset(Requested.Path).bRegistryOrMemoryExists;
        const bool bStillOnDisk = DoesPackageFileExistOnDisk(
            FPackageName::ObjectPathToPackageName(Requested.Path));
        const bool bStillExists = bStillRegistered || bStillOnDisk;
        const bool bGone = !bStillExists;

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("path"), Requested.Path);
        Entry->SetBoolField(TEXT("existedBefore"), Requested.bExistedBefore);
        Entry->SetBoolField(TEXT("attempted"), Requested.bAttempted);
        Entry->SetBoolField(TEXT("existsAfter"), bStillExists);
        Entry->SetBoolField(TEXT("existsOnDisk"), bStillOnDisk);
        // "deleted" means THIS call removed something that was there. A path that was
        // already absent is missing, not a deletion this call performed.
        Entry->SetBoolField(TEXT("deleted"), Requested.bExistedBefore && bGone);
        Entry->SetBoolField(TEXT("missing"), !Requested.bExistedBefore);

        // Memory and disk have diverged: the registry row is gone and the file is not.
        // The editor's view and the content directory disagree until it restarts, so state
        // it rather than leaving the caller to infer it from two fields.
        if (Requested.bAttempted && !bStillRegistered && bStillOnDisk)
        {
            Entry->SetBoolField(TEXT("memoryDiskDivergence"), true);
            bAnyDiverged = true;
        }

        if (!Requested.bExistedBefore)
        {
            MissingPaths.Add(Requested.Path);
        }
        else if (bGone)
        {
            DeletedCount++;
            DeletedPaths.Add(Requested.Path);
        }
        else
        {
            bAnySurvived = true;
            FailedPaths.Add(Requested.Path);
        }
        EntryResults.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    // success means the requested work happened, i.e. NOTHING the caller named survived.
    // It was DeletedCount > 0, which scored a 1-of-20 batch as a clean success.
    Result->SetBoolField(TEXT("success"), !bAnySurvived);
    Result->SetArrayField(TEXT("results"), EntryResults);
    Result->SetArrayField(TEXT("deleted"), MakeStringArray(DeletedPaths));
    Result->SetArrayField(TEXT("failed"), MakeStringArray(FailedPaths));
    Result->SetArrayField(TEXT("missing"), MakeStringArray(MissingPaths));
    Result->SetNumberField(TEXT("deletedCount"), DeletedCount);
    Result->SetNumberField(TEXT("failedCount"), FailedPaths.Num());
    Result->SetNumberField(TEXT("missingCount"), MissingPaths.Num());
    // The caller's array length, not the count that loaded. `requested` used to be the
    // latter and is gone: a caller reading it could not tell a two-path request with one
    // typo from a one-path request.
    Result->SetNumberField(TEXT("requestedCount"), AssetPaths.Num());
    Result->SetNumberField(TEXT("attemptedCount"), ObjectsToDelete.Num());
    // Kept as an engine-reported figure only, deliberately named apart from the probed
    // deletedCount because AddExtraObjectsToDelete means it can exceed the request.
    Result->SetNumberField(TEXT("engineDeletedCount"), EngineDeletedCount);
    Result->SetBoolField(TEXT("existsAfter"), bAnySurvived);
    if (bAnySurvived)
    {
        FString Hint = TEXT(
            "One or more requested paths still exist after the delete. This verb's engine "
            "route returns only a COUNT: DeleteObjectsUnchecked silently skips an object "
            "whose package is read-only (its dialog answers No unattended) or whose "
            "OnAssetsCanDelete handler refuses it, and CleanupAfterSuccessfulDelete then "
            "runs after the count is final, dropping any package whose filename it cannot "
            "resolve and discarding the return of IFileManager::Delete. Read results[] per "
            "entry: existsOnDisk separates a surviving file from a surviving registry row, "
            "and attempted:false means the path was never handed to the engine. asset.delete "
            "names the in-memory holders that blocked an entry (inMemoryReferencers); "
            "release them, or close the asset's editor with editor.close_asset, and retry.");
        if (bAnyDiverged)
        {
            Hint += TEXT(
                " memoryDiskDivergence is set on at least one entry: its registry row is "
                "gone while the .uasset is still on disk, so the editor's view and the "
                "content directory disagree until it restarts - do not re-save assets that "
                "referenced them.");
        }
        Result->SetStringField(TEXT("failureHint"), Hint);
    }
    if (bFixupRedirectors)
    {
        RedirectorFixupPolicy::AddReport(Result, FixupResult);
        Result->SetStringField(TEXT("fixupScope"),
            RedirectorFixupPolicy::SweepScopeToString(FixupScope));

        // The collateral, emitted even when empty. The caller named assets, not
        // redirectors, so every redirector package removed outside their folders is work
        // this verb did that was not asked for - and until now the caller got no signal
        // at all that it had happened. An always-present array says "I checked"; an
        // omitted one is indistinguishable from not looking.
        TArray<TSharedPtr<FJsonValue>> OutsideScope;
        for (const FString& PackageName : FixupResult.DeletedRedirectorPackages)
        {
            if (!RedirectorFixupPolicy::IsInPackageFolders(PackageName, RequestedFolders))
            {
                OutsideScope.Add(MakeShared<FJsonValueString>(PackageName));
            }
        }
        Result->SetArrayField(TEXT("redirectorsDeletedOutsideScope"), OutsideScope);
    }

    // The envelope follows the probe too. It used to be DeletedCount > 0, so a caller who
    // only checked the envelope was told a partial batch succeeded; a surviving asset is a
    // failure of the request that was made, whatever the engine counted. Result travels
    // with the error so results[] / failed[] are readable on exactly the path that needs
    // them.
    if (bAnySurvived)
    {
        Ctx.SendError(ErrorCodes::ERR_BULK_DELETE_FAILED,
            FString::Printf(
                TEXT("Deleted %d of %d requested assets; %d still exist after the delete"),
                DeletedCount, AssetPaths.Num(), FailedPaths.Num()),
            Result);
    }
    else
    {
        Ctx.SendSuccess(Result);
    }
    return true;
}

// ============================================================================
// asset.generate_thumbnail
// ============================================================================
REGISTER_RPC_HANDLER("asset.generate_thumbnail", "asset", "Render an asset's thumbnail offscreen and optionally write it to disk. The on-disk format follows the outputPath extension (.png by default, .jpg/.jpeg for JPEG). Read-only: the asset is never modified or dirtied.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the asset"),
        RPC_PARAM_OPT("width", "number", "Thumbnail width (default 512). Treated as a MAXIMUM by some asset types, which preserve their own aspect ratio; the response reports the size actually rendered."),
        RPC_PARAM_OPT("height", "number", "Thumbnail height (default 512). See width."),
        RPC_PARAM_OPT("outputPath", "filepath", "File path to save the thumbnail to. The extension selects the format: .jpg/.jpeg write JPEG, everything else writes PNG. Omit for metadata only (no bytes are returned)."),
        RPC_PARAM_OPT("primitive", "string", "Material assets only: preview shape — sphere, cube, plane, cylinder, shaderBall, or mesh (with primitiveMesh). Defaults to the asset's own configured shape. The engine substitutes a flat plane for a UI-domain material whatever is requested; the response reports the shape actually drawn plus requestedPrimitive and primitiveReason when it differs."),
        RPC_PARAM_OPT("primitiveMesh", "path", "StaticMesh asset path used when primitive is 'mesh'."),
        RPC_PARAM_OPT("azimuth", "number", "Horizontal camera angle in degrees (0 on +X, increasing toward +Y), matching camera.frame_actor. Defaults to the asset's own stored angle. Ignored when the preview shape is the plane (see elevation)."),
        RPC_PARAM_OPT("elevation", "number", "Camera angle in degrees above the horizon. Defaults to the asset's own stored angle. Ignored when the preview shape is the plane: the thumbnail plane is a zero-thickness quad pinned to one fixed attitude, so orbiting it renders an edge-on sliver. The response then carries elevationApplied/azimuthApplied false and cameraReason."),
        RPC_PARAM_OPT("zoom", "number", "Camera distance offset in world units; negative moves closer. Defaults to the asset's own stored zoom."),
        PinWright::MaterialShaderState::AllowFallbackParamSpec()
    ))
{
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("assetPath required"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    const bool bAllowFallback = Ctx.GetBool(
        PinWright::MaterialShaderState::AllowFallbackParamName(), false);
    int32 Width = 512;
    int32 Height = 512;

    double TempWidth = 0, TempHeight = 0;
    if (Payload->TryGetNumberField(TEXT("width"), TempWidth))
        Width = static_cast<int32>(TempWidth);
    if (Payload->TryGetNumberField(TEXT("height"), TempHeight))
        Height = static_cast<int32>(TempHeight);

    if (Width <= 0 || Height <= 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("width and height must be greater than zero"));
        return true;
    }

    FString OutputPath = Ctx.GetString(TEXT("outputPath"));

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

    // ---- Optional preview overrides (shape + camera). Every field is opt-in; with none of
    // them passed the guard is inert and the render is byte-identical to the old behaviour.
    PinWrightThumbnail::FPreviewOverrideRequest Override;
    const FString PrimitiveName = Ctx.GetString(TEXT("primitive"));
    const FString PrimitiveMeshPath = Ctx.GetString(TEXT("primitiveMesh"));

    if (!PrimitiveName.IsEmpty())
    {
        bool bIsCustomMesh = false;
        if (!PinWrightThumbnail::ParsePrimitiveName(PrimitiveName, Override.PrimitiveType, bIsCustomMesh))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("Unknown primitive '%s'; expected one of: %s"),
                    *PrimitiveName, *PinWrightThumbnail::PrimitiveNameList()));
            return true;
        }
        Override.bHasPrimitive = true;

        if (bIsCustomMesh)
        {
            if (PrimitiveMeshPath.IsEmpty())
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    TEXT("primitive 'mesh' requires primitiveMesh (a StaticMesh asset path)"));
                return true;
            }

            // Loaded EXPLICITLY, and checked to be a StaticMesh, because the engine resolves
            // this field with ResolveObject() and never TryLoad(), and casts the result to
            // UStaticMesh only. An unloaded mesh, or a SkeletalMesh, would silently degrade the
            // preview to a flat plane and report success - a wrong picture, not an error.
            UObject* PreviewMeshAsset =
                ResolveAsset(PrimitiveMeshPath, /*bLoadObject=*/true).Object;
            if (!PreviewMeshAsset)
            {
                Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                    FString::Printf(TEXT("primitiveMesh not found: %s"), *PrimitiveMeshPath));
                return true;
            }
            if (!PreviewMeshAsset->IsA<UStaticMesh>())
            {
                Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET,
                    FString::Printf(
                        TEXT("primitiveMesh must be a StaticMesh; '%s' is a %s (the engine's preview "
                             "primitive silently falls back to a plane for anything else)"),
                        *PrimitiveMeshPath, *PreviewMeshAsset->GetClass()->GetName()));
                return true;
            }
            Override.PreviewMesh = FSoftObjectPath(PreviewMeshAsset);
        }
    }
    else if (!PrimitiveMeshPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("primitiveMesh requires primitive:'mesh'"));
        return true;
    }

    double TempAngle = 0.0;
    if (Payload->TryGetNumberField(TEXT("azimuth"), TempAngle))
    {
        Override.bHasAzimuth = true;
        Override.Azimuth = static_cast<float>(TempAngle);
    }
    if (Payload->TryGetNumberField(TEXT("elevation"), TempAngle))
    {
        Override.bHasElevation = true;
        Override.Elevation = static_cast<float>(TempAngle);
    }
    if (Payload->TryGetNumberField(TEXT("zoom"), TempAngle))
    {
        Override.bHasZoom = true;
        Override.Zoom = static_cast<float>(TempAngle);
    }

    // ---- Frame readiness. Every parameter is validated by this point, and the override guard
    // below has not been armed yet — both deliberate.
    //
    // RenderThumbnail is called with NeverFlush, so nothing waits for this asset's async build,
    // its shader map or the mips of the textures it samples unless this does — which is why the
    // FIRST call after a compile or a fresh load returned a blank or half-drawn frame while an
    // identical second call returned the finished one (B-thumbnail-cold-first-frame-no-stats).
    // These are the engine's own per-subject thumbnail pre-waits, scoped to this asset; see
    // ThumbnailFrameEvidence.h.
    //
    // It must stay ABOVE the guard: FScopedPreviewOverride's safety argument rests on nothing
    // observing the asset while its ThumbnailInfo is swapped, and this wait can pump. Waiting
    // first keeps that window exactly as narrow as it was.
    PinWrightThumbnail::FThumbnailReadinessReport Readiness;
    PinWrightThumbnail::WaitForThumbnailSubjectReadiness(Asset, Readiness);

    FString OverrideErrorCode;
    FString OverrideErrorMessage;
    // Scoped: every field it touches is restored when this leaves scope, on every path
    // including the error returns below, and the package is never marked dirty.
    PinWrightThumbnail::FScopedPreviewOverride PreviewOverride(
        Asset, Override, OverrideErrorCode, OverrideErrorMessage);
    if (!PreviewOverride.IsValid())
    {
        Ctx.SendError(OverrideErrorCode, OverrideErrorMessage);
        return true;
    }

    int32 ImageWidth = 0;
    int32 ImageHeight = 0;
    TArray<FColor> ColorData;

    // One render pass, read back into the buffer the statistics AND the encoder both use, so a
    // published measurement always describes the bytes that were written.
    auto RenderThumbnailPass = [&]()
    {
        FObjectThumbnail ObjectThumbnail;
        ThumbnailTools::RenderThumbnail(
            Asset, Width, Height,
            ThumbnailTools::EThumbnailTextureFlushMode::NeverFlush, nullptr,
            &ObjectThumbnail);

        // The engine treats the requested size as a MAXIMUM for some asset types (textures keep
        // their source aspect ratio and call SetImageSize with the reduced values), so the pixel
        // buffer can be smaller than what was asked for. Encoding at the REQUESTED size then feeds
        // the encoder short data. Use what was actually rendered.
        ImageWidth = ObjectThumbnail.GetImageWidth();
        ImageHeight = ObjectThumbnail.GetImageHeight();
        ColorData = PinWrightThumbnail::ThumbnailBytesToColors(
            ObjectThumbnail.GetUncompressedImageData());
    };

    RenderThumbnailPass();

    // Measured on EVERY call, with or without an outputPath: a caller that asked for metadata
    // only still has to be able to tell a finished frame from a cold one.
    PinWrightRenderCapture::FCaptureImageStats FrameStats =
        PinWrightRenderCapture::CalculateCaptureImageStats(ColorData);

    // The second pass is gated on the MEASUREMENT and bounded at one, never a fixed "render N and
    // hope" warm-up: a frame that already resolves a readable range pays for one render, and only
    // a degenerate one is drawn again. Both passes' numbers are published, so the settle is a
    // reported fact rather than an assumption.
    int32 RenderPasses = 1;
    PinWrightThumbnail::FColdFrameRetryReport ColdRetry;
    if (ImageWidth > 0 && ImageHeight > 0 && PinWrightThumbnail::FrameIsDegenerate(FrameStats))
    {
        ColdRetry.bRetried = true;
        ColdRetry.FirstPassStats = FrameStats;
        const TArray<FColor> FirstPassPixels = MoveTemp(ColorData);

        RenderThumbnailPass();
        RenderPasses = 2;
        FrameStats = PinWrightRenderCapture::CalculateCaptureImageStats(ColorData);
        ColdRetry.DifferingPixels =
            PinWrightThumbnail::CountDifferingPixels(FirstPassPixels, ColorData);
        ColdRetry.ComparedPixels = FMath::Min(FirstPassPixels.Num(), ColorData.Num());
    }

    bool bSuccess = ImageWidth > 0 && ImageHeight > 0;
    FString OutputFormat;

    if (bSuccess && !OutputPath.IsEmpty())
    {
        if (ColorData.Num() > 0)
        {
            FString AbsolutePath = OutputPath;
            if (FPaths::IsRelative(OutputPath))
            {
                AbsolutePath =
                    FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), OutputPath);
            }

            // Encoder chosen by the output extension, and alpha stamped opaque inside it.
            // Never FImageUtils::ThumbnailCompressImageArray, which emits JPEG for anything
            // >= 8x8 and used to write JPEG/JFIF bytes into the caller's .png path while
            // reporting success.
            TArray<uint8> EncodedData;
            bSuccess = PinWrightThumbnail::EncodeByExtension(
                           AbsolutePath, ImageWidth, ImageHeight, ColorData, EncodedData, OutputFormat)
                       && FFileHelper::SaveArrayToFile(EncodedData, *AbsolutePath);
        }
        else
        {
            bSuccess = false;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), bSuccess);
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    // The size actually rendered, which is what the file on disk carries.
    Result->SetNumberField(TEXT("width"), ImageWidth > 0 ? ImageWidth : Width);
    Result->SetNumberField(TEXT("height"), ImageHeight > 0 ? ImageHeight : Height);
    if (ImageWidth > 0 && (ImageWidth != Width || ImageHeight != Height))
    {
        // Say so rather than letting the caller assume the request was honoured exactly.
        Result->SetNumberField(TEXT("requestedWidth"), Width);
        Result->SetNumberField(TEXT("requestedHeight"), Height);
    }
    if (!PrimitiveName.IsEmpty())
    {
        // The shape actually DRAWN, resolved inside the override scope, not the one asked for.
        // FMaterialThumbnailScene substitutes a flat plane for some materials whatever is
        // requested and discards the PrimitiveType it just read, so echoing the request here
        // named a shape the pixels did not show — and a caller comparing a master against an
        // instance read the difference as a material change. Same shape as requestedWidth /
        // requestedHeight above: report the realised value, keep the request beside it only
        // when the two disagree.
        const bool bSubstituted =
            PreviewOverride.WasForcedToPlane() &&
            !PrimitiveName.Equals(TEXT("plane"), ESearchCase::IgnoreCase);
        Result->SetStringField(TEXT("primitive"),
            PreviewOverride.WasForcedToPlane() ? TEXT("plane") : PrimitiveName);
        if (bSubstituted)
        {
            Result->SetStringField(TEXT("requestedPrimitive"), PrimitiveName);
            Result->SetStringField(TEXT("primitiveReason"), PreviewOverride.GetForcePlaneReason());
        }
    }

    // A camera angle that was accepted and not applied. The thumbnail plane has one fixed
    // attitude, so an orbit away from it renders a zero-thickness quad edge on — an empty frame
    // that reads as a broken material. The angles are dropped rather than honoured into
    // uselessness, and the drop reports itself.
    if (PreviewOverride.WerePlaneCameraAnglesIgnored())
    {
        if (Override.bHasElevation)
        {
            Result->SetBoolField(TEXT("elevationApplied"), false);
        }
        if (Override.bHasAzimuth)
        {
            Result->SetBoolField(TEXT("azimuthApplied"), false);
        }
        Result->SetStringField(TEXT("cameraReason"),
            TEXT("The preview shape is the engine's thumbnail plane, a zero-thickness quad "
                 "pinned to one fixed attitude; any orbit away from it renders the plane edge "
                 "on. The asset's own stored angles were used instead. Use a solid primitive "
                 "(cube, cylinder, sphere) when an angle is meaningful."));
    }

    if (!OutputPath.IsEmpty())
    {
        Result->SetStringField(TEXT("outputPath"), OutputPath);
        Result->SetStringField(TEXT("format"), OutputFormat);
    }

    // What the pixels actually are: `imageStats`, the `blank` / `crushed` / `blownOut` verdicts,
    // `frameWarning`, `renderPasses`, `coldFrameRetry` and `readiness`. Until this landed, a cold
    // blank frame and the finished one returned byte-for-byte identical payloads, so a caller
    // comparing two captures measured the warm-up artefact and read it as motion.
    PinWrightThumbnail::AddFrameEvidenceFields(
        FrameStats, ColdRetry, RenderPasses, Readiness, Result);

    // Rendering can be the event that finishes (or fails) a deferred shader compile. Probe after
    // the final retry so the wire block reports the latest state observed after the pixels were
    // produced. An incomplete state remains uncertain rather than being promoted to a failure.
    const PinWright::MaterialShaderState::FCaptureReadiness MaterialReadiness =
        PinWright::MaterialShaderState::ProbeCaptureAsset(Asset,
            PinWright::MaterialShaderState::ECaptureMeshUsagePolicy::ThumbnailLod0Sections);

    if (!bSuccess)
    {
        PinWright::MaterialShaderState::AddCaptureReadiness(
            Result, MaterialReadiness, bAllowFallback);
        Ctx.SendError(ErrorCodes::ERR_THUMBNAIL_GENERATION_FAILED,
            TEXT("Thumbnail generation failed"), Result);
        return true;
    }

    if (!PinWright::MaterialShaderState::ApplyCaptureFallbackPolicy(
            Result, MaterialReadiness, bAllowFallback))
    {
        Result->SetBoolField(TEXT("success"), false);
        Ctx.SendError(ErrorCodes::ERR_MATERIAL_FALLBACK,
            TEXT("A rendered thumbnail material used the engine Default Material because shader "
                 "compilation failed or a rendered material slot was unassigned. Fix the known "
                 "fallback named in materialReadiness.subjects, or "
                 "pass allowFallback:true to retain the fallback image explicitly."),
            Result);
        return true;
    }

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// asset.generate_lods
// ============================================================================
REGISTER_RPC_HANDLER("asset.generate_lods", "asset", "Auto-generate UE's standard LOD chain for one or more StaticMesh assets using the default reduction settings (decreasing triangle counts per level). Takes either assetPath for a single mesh or assetPaths for a batch.",
    RPC_PARAMS(
        AssetPathParamUtils::GenerateLodsSingleMeshParamOpt(TEXT("Asset path to a single StaticMesh. Aliases: meshPath; landscapePath (legacy misnomer kept for backward compatibility).")),
        RPC_PARAM_OPT("assetPaths", "array", "Array of StaticMesh asset paths for batch LOD generation."),
        RPC_PARAM_OPT("lodCount", "integer", "Total LOD count to generate including LOD0; defaults to 4 (i.e. LOD0..LOD3)."),
        RPC_PARAM_OPT("numLODs", "number", "Alias for lodCount; whichever is provided wins (lodCount preferred when both are set)."),
        RPC_PARAM_OPT("partial", "boolean", "Generate LODs for valid meshes when another item fails preflight. Defaults to false, which refuses the whole batch before rebuilding."),
        RPC_PARAM_DEF("save", "boolean", "Write generated LODs to disk; false leaves the package dirty and reports saveRequested:false (default true).", "true"),
        RPC_PARAM_DEF("timeoutSeconds", "number", "Maximum request-wide time to wait for StaticMesh builds (0-60 seconds). On expiry the item reports timedOut:true and is not counted as completed.", "60")
    ))
{
    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();

    const TArray<TSharedPtr<FJsonValue>>* AssetPathsArray = nullptr;
    Payload->TryGetArrayField(TEXT("assetPaths"), AssetPathsArray);

    int32 NumLODs = 4;
    Payload->TryGetNumberField(TEXT("lodCount"), NumLODs);
    Payload->TryGetNumberField(TEXT("numLODs"), NumLODs);
    constexpr int32 MaxGenerateLods = MAX_STATIC_MESH_LODS;
    if (NumLODs > MaxGenerateLods)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("lodCount must not exceed %d (UE MAX_STATIC_MESH_LODS)."),
                MaxGenerateLods));
        return true;
    }
    NumLODs = FMath::Clamp(NumLODs, 1, MaxGenerateLods);

    constexpr double DefaultCompileTimeoutSeconds = 60.0;
    constexpr double MinCompileTimeoutSeconds = 0.0;
    constexpr double MaxCompileTimeoutSeconds = 60.0;
    double CompileTimeoutSeconds = DefaultCompileTimeoutSeconds;
    if (Payload->HasField(TEXT("timeoutSeconds")))
    {
        if (!Ctx.RequireNumber(TEXT("timeoutSeconds"), CompileTimeoutSeconds))
        {
            return true;
        }
        if (!FMath::IsFinite(CompileTimeoutSeconds)
            || CompileTimeoutSeconds < MinCompileTimeoutSeconds
            || CompileTimeoutSeconds > MaxCompileTimeoutSeconds)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("timeoutSeconds must be between %.2f and %.0f."),
                    MinCompileTimeoutSeconds, MaxCompileTimeoutSeconds));
            return true;
        }
    }

    TArray<TSharedPtr<FJsonValue>> Inputs;
    // Keep the single-mesh slot first, matching the legacy execution order, but retain its raw
    // JSON value so a malformed alias is represented instead of narrowed away.
    const TSharedPtr<FJsonValue> SingleMeshInput =
        Ctx.GetJsonValueFirstOf(AssetPathParamUtils::GenerateLodsSingleMeshKeys());
    if (SingleMeshInput.IsValid())
    {
        Inputs.Add(SingleMeshInput);
    }
    if (AssetPathsArray)
    {
        Inputs.Append(*AssetPathsArray);
    }

    if (Inputs.Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("assetPath (single) or assetPaths (batch) required"));
        return true;
    }

    const bool bAllowPartial = Ctx.GetBool(TEXT("partial"), false);
    PinWrightAssetBatch::FAssetBatchResult Batch(bAllowPartial);
    TArray<FString> Paths;
    TArray<int32> OutcomeIndices;
    Paths.Reserve(Inputs.Num());
    OutcomeIndices.Reserve(Inputs.Num());

    for (int32 InputIndex = 0; InputIndex < Inputs.Num(); ++InputIndex)
    {
        const TSharedPtr<FJsonValue>& Input = Inputs[InputIndex];
        const int32 OutcomeIndex = Batch.AddInput(Input);
        TSharedPtr<FJsonObject> Item = Batch.GetItem(OutcomeIndex);
        if (!Input.IsValid() || Input->Type != EJson::String
            || Input->AsString().IsEmpty())
        {
            Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("LOD input %d must be a non-empty string"), InputIndex));
            continue;
        }

        const FString InputPath = Input->AsString();
        Item->SetStringField(TEXT("inputPath"), InputPath);
        const FString SafePath = SanitizeProjectRelativePath(InputPath);
        if (SafePath.IsEmpty())
        {
            Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_SECURITY_VIOLATION,
                FString::Printf(TEXT("Invalid or unsafe asset path: %s"), *InputPath));
            continue;
        }

        // Keep loading at the safe point. Registry/class preflight is enough to refuse the
        // default batch without moving LoadObject ahead of the existing render-safe boundary.
        const FResolvedAsset Resolved = ResolveAsset(SafePath);
        const bool bIsStaticMesh = Resolved.bExists
            && ((Resolved.Object && Resolved.Object->IsA<UStaticMesh>())
                || (Resolved.AssetData.IsValid()
                    && Resolved.AssetData.AssetClassPath
                        == UStaticMesh::StaticClass()->GetClassPathName()));
        if (!bIsStaticMesh)
        {
            Batch.FailPreflight(OutcomeIndex, ErrorCodes::ERR_MESH_NOT_FOUND,
                FString::Printf(TEXT("StaticMesh not found: %s"), *InputPath));
            continue;
        }

        FString RebuildPath = Resolved.ObjectPath.ToString();
        if (RebuildPath.IsEmpty())
        {
            RebuildPath = SafePath;
        }
        Item->SetStringField(TEXT("path"), RebuildPath);
        Item->SetNumberField(TEXT("requestedLodCount"), NumLODs);
        Batch.MarkReady(OutcomeIndex);
        Paths.Add(RebuildPath);
        OutcomeIndices.Add(OutcomeIndex);
    }

    if (Batch.ShouldRefuseBeforeMutation())
    {
        Batch.RefuseReadyItems(ErrorCodes::ERR_BATCH_NOT_ATTEMPTED,
            TEXT("Not attempted because another batch item failed preflight"));
        TSharedPtr<FJsonObject> Result = Batch.MakeResult();
        Result->SetNumberField(TEXT("processed"), 0);
        Result->SetNumberField(TEXT("lodCount"), NumLODs);
        Result->SetBoolField(TEXT("timedOut"), false);
        Result->SetNumberField(TEXT("timeoutSeconds"), CompileTimeoutSeconds);
        Ctx.SendError(ErrorCodes::ERR_BATCH_PREFLIGHT_FAILED,
            TEXT("LOD batch failed preflight; no meshes were rebuilt"), Result);
        return true;
    }

    if (!Batch.HasReadyItems())
    {
        TSharedPtr<FJsonObject> Result = Batch.MakeResult();
        Result->SetNumberField(TEXT("processed"), 0);
        Result->SetNumberField(TEXT("lodCount"), NumLODs);
        Result->SetBoolField(TEXT("timedOut"), false);
        Result->SetNumberField(TEXT("timeoutSeconds"), CompileTimeoutSeconds);
        Ctx.SendError(ErrorCodes::ERR_BATCH_PREFLIGHT_FAILED,
            TEXT("LOD batch contains no valid StaticMesh assets"), Result);
        return true;
    }

    const bool bSingleInputRequest = Batch.NumRequested() == 1;
    const double CompileDeadline = FPlatformTime::Seconds() + CompileTimeoutSeconds;
    return PinWrightMeshRebuild::RunGuardedStaticMeshRebuild(Ctx,
        TEXT("asset.generate_lods"), Paths, PinWrightMeshRebuild::PreserveInputSlots,
        [Batch = MoveTemp(Batch), OutcomeIndices = MoveTemp(OutcomeIndices), NumLODs,
          bSingleInputRequest, bSave = Ctx.GetBool(TEXT("save"), true),
          CompileTimeoutSeconds, CompileDeadline](
            const PinWrightSafePoint::FSafePointResponder& Responder,
            const PinWrightMeshRebuild::FPreservedStaticMeshBatch& MeshBatch) mutable
    {
        if (!MeshBatch.bCanRebuild)
        {
            for (int32 OutcomeIndex : OutcomeIndices)
            {
                Batch.FailRuntime(OutcomeIndex, MeshBatch.ErrorCode, MeshBatch.Error);
            }

            TSharedPtr<FJsonObject> Result = Batch.MakeResult();
            Result->SetNumberField(TEXT("processed"), 0);
            Result->SetNumberField(TEXT("lodCount"), NumLODs);
            Result->SetBoolField(TEXT("timedOut"), false);
            Result->SetNumberField(TEXT("timeoutSeconds"), CompileTimeoutSeconds);
            Responder.SendError(MeshBatch.ErrorCode, MeshBatch.Error, Result);
            return;
        }

        bool bAnyTimedOut = false;
        bool bHasSingleSaveReport = false;
        bool bSingleHasSizeReport = false;
        bool bSingleSaveRequested = false;
        FString SinglePackageName;
        int64 SingleSizeBytes = 0;
        bool bSingleSavedToDisk = false;
        EAssetSaveState SingleSaveState = EAssetSaveState::NotRequested;
        TArray<UStaticMesh*> TimedOutMeshes;

        for (int32 WorkIndex = 0; WorkIndex < OutcomeIndices.Num(); ++WorkIndex)
        {
            const int32 OutcomeIndex = OutcomeIndices[WorkIndex];
            UStaticMesh* Mesh = MeshBatch.Meshes.IsValidIndex(WorkIndex)
                ? MeshBatch.Meshes[WorkIndex]
                : nullptr;
            if (!Mesh)
            {
                Batch.FailRuntime(OutcomeIndex, ErrorCodes::ERR_MESH_NOT_FOUND,
                    TEXT("StaticMesh disappeared after preflight and before LOD generation"));
                continue;
            }

            const FString Path = Mesh->GetPathName();
            TSharedPtr<FJsonObject> Item = Batch.GetItem(OutcomeIndex);
            Item->SetBoolField(TEXT("timedOut"), false);
            Item->SetNumberField(TEXT("timeoutSeconds"), CompileTimeoutSeconds);
            UE_LOG(LogPinWrightSubsystem, Log,
                   TEXT("Generating %d LODs for static mesh %s"), NumLODs, *Path);

            // The request-wide deadline was captured before entering the safe-point callback.
            // In particular, do not call PostEditChange here: UObject's edit-change path can
            // synchronously FinishCompilation an existing build before the bounded wait runs.
            const auto WaitForCompilation = [Mesh, CompileDeadline](
                const bool bAllowTestPendingOverride) -> bool
            {
                while (PinWright::AssetCompile::IsCompilingForBoundedWait(
                    Mesh, bAllowTestPendingOverride))
                {
                    if (FPlatformTime::Seconds() >= CompileDeadline)
                    {
                        return false;
                    }

                    PinWright::AssetCompile::AdvanceOnGameThread();
                    if (!PinWright::AssetCompile::IsCompilingForBoundedWait(
                        Mesh, bAllowTestPendingOverride))
                    {
                        return FPlatformTime::Seconds() < CompileDeadline;
                    }

                    // Re-read after pumping so a completed build never receives a stale or
                    // negative sleep interval at the deadline boundary.
                    const double RemainingSeconds = CompileDeadline - FPlatformTime::Seconds();
                    if (RemainingSeconds <= 0.0)
                    {
                        return false;
                    }

                    FPlatformProcess::SleepNoStats(static_cast<float>(
                        FMath::Min(RemainingSeconds, 0.01)));
                }
                return true;
            };

            bool bCompilationTerminal = false;
            bool bTimedOut = FPlatformTime::Seconds() >= CompileDeadline;
            if (!bTimedOut)
            {
                bCompilationTerminal = WaitForCompilation(/*bAllowTestPendingOverride=*/false);
                bTimedOut = !bCompilationTerminal
                    || FPlatformTime::Seconds() >= CompileDeadline;
            }
            if (!bTimedOut)
            {
                Mesh->Modify();
                // A mesh created through BuildFromMeshDescriptions with bFastBuild carries
                // bDoFastBuild, and that path builds every LOD straight from its OWN mesh
                // description instead of running the reducer - so a reduction LOD added here
                // would never be generated. On UE 5.3/5.4 it is worse than a no-op: the fast
                // path dereferences GetMeshDescription(LodIndex) unchecked
                // (StaticMesh.cpp:3140 on 5.4), and a source model added below has none, so the
                // build worker takes an access violation and ends the editor. 5.8 guards that
                // read with IsMeshDescriptionValid. Clearing the flag puts the request on the
                // real mesh builder, which is the only path that can honour it.
                Mesh->bDoFastBuild = false;
                Mesh->SetNumSourceModels(NumLODs);

                for (int32 LODIndex = 1; LODIndex < NumLODs; ++LODIndex)
                {
                    FStaticMeshSourceModel& SourceModel = Mesh->GetSourceModel(LODIndex);
                    FMeshReductionSettings& ReductionSettings = SourceModel.ReductionSettings;
                    const float ReductionPercent =
                        1.0f / FMath::Pow(2.0f, static_cast<float>(LODIndex));
                    ReductionSettings.PercentTriangles = ReductionPercent;
                    ReductionSettings.PercentVertices = ReductionPercent;
                    SourceModel.BuildSettings.bRecomputeNormals = false;
                    SourceModel.BuildSettings.bRecomputeTangents = false;
                    SourceModel.BuildSettings.bUseMikkTSpace = true;
                }

                if (FPlatformTime::Seconds() >= CompileDeadline)
                {
                    bTimedOut = true;
                }
                else
                {
                    // UStaticMesh::Build is the single established trigger that queues the
                    // async build without the unbounded FinishCompilation in PostEditChange.
                    Mesh->Build(/*bSilent=*/true);
                    bCompilationTerminal = WaitForCompilation(/*bAllowTestPendingOverride=*/true);
                    bTimedOut = !bCompilationTerminal
                        || FPlatformTime::Seconds() >= CompileDeadline;
                }
            }

            Item->SetBoolField(TEXT("timedOut"), bTimedOut);
            if (bTimedOut)
            {
                bAnyTimedOut = true;
                // No write was attempted, so use the explicit non-attempted state rather than
                // reporting a failed/pending save that a later flush could not repair.
                AddAssetSaveReport(Item, /*bSaveRequested=*/false,
                    /*bSavedToDisk=*/false, EAssetSaveState::NotRequested);
                if (bSingleInputRequest)
                {
                    SinglePackageName = Mesh->GetOutermost()
                        ? Mesh->GetOutermost()->GetName()
                        : FString();
                    SingleSaveState = EAssetSaveState::NotRequested;
                    bSingleSaveRequested = false;
                    bHasSingleSaveReport = true;
                }
                if (Mesh->IsCompiling())
                {
                    TimedOutMeshes.AddUnique(Mesh);
                }
                Batch.FailRuntime(OutcomeIndex, ErrorCodes::ERR_OPERATION_FAILED,
                    FString::Printf(TEXT("StaticMesh compilation for %s did not finish within "
                        "%.2f seconds; no LOD save was attempted"),
                        *Path, CompileTimeoutSeconds));
                continue;
            }

            const int32 ActualLodCount = Mesh->GetNumLODs();
            Item->SetNumberField(TEXT("actualLodCount"), ActualLodCount);

            FString PackageName = Mesh->GetOutermost()
                ? Mesh->GetOutermost()->GetName()
                : FString();
            if (!bCompilationTerminal || ActualLodCount != NumLODs)
            {
                // A source-model count can echo SetNumSourceModels even when render data did not
                // build. Do not save or report success unless the compiled render LOD count agrees.
                AddAssetSaveReport(Item, /*bSaveRequested=*/false,
                    /*bSavedToDisk=*/false, EAssetSaveState::NotRequested);
                if (bSingleInputRequest)
                {
                    SinglePackageName = PackageName;
                    SingleSaveState = EAssetSaveState::NotRequested;
                    bSingleSaveRequested = false;
                    bHasSingleSaveReport = true;
                }
                Batch.FailRuntime(OutcomeIndex, ErrorCodes::ERR_OPERATION_FAILED,
                    FString::Printf(TEXT("StaticMesh render LOD count readback for %s was %d, "
                        "expected %d; no LOD save was attempted"),
                        *Path, ActualLodCount, NumLODs));
                continue;
            }

            Mesh->MarkPackageDirty();

            int64 SizeBytes = 0;
            bool bSavedToDisk = false;
            EAssetSaveState SaveState = EAssetSaveState::NotRequested;
            if (bSave)
            {
                bSavedToDisk = SaveAssetToDiskReportingPresence(
                    Mesh, /*bForce=*/true, &PackageName, &SizeBytes, &SaveState);
            }
            else
            {
                FString PackageFilename;
                if (FPackageName::TryConvertLongPackageNameToFilename(
                        PackageName, PackageFilename, FPackageName::GetAssetPackageExtension()))
                {
                    SizeBytes = FMath::Max<int64>(IFileManager::Get().FileSize(*PackageFilename), 0);
                }
            }

            Item->SetStringField(TEXT("package"), PackageName);
            AddAssetSaveSizeReport(Item, SizeBytes, bSavedToDisk);
            AddAssetSaveReport(Item, bSave, bSavedToDisk, SaveState);
            if (bSingleInputRequest)
            {
                SinglePackageName = PackageName;
                SingleSizeBytes = SizeBytes;
                bSingleSavedToDisk = bSavedToDisk;
                SingleSaveState = SaveState;
                bSingleSaveRequested = bSave;
                bSingleHasSizeReport = true;
                bHasSingleSaveReport = true;
            }

            if (bCompilationTerminal && ActualLodCount == NumLODs && (!bSave || bSavedToDisk))
            {
                Batch.MarkSucceeded(OutcomeIndex);
            }
            else if (ActualLodCount == NumLODs && bSave && !bSavedToDisk)
            {
                Batch.FailRuntime(OutcomeIndex, ErrorCodes::ERR_OPERATION_FAILED,
                    FString::Printf(TEXT("LOD generation for %s completed in memory, but its "
                        "requested save was not durable (saveState=%s)"),
                        *Path, AssetSaveStateToWire(SaveState)));
            }
            else
            {
                Batch.FailRuntime(OutcomeIndex, ErrorCodes::ERR_OPERATION_FAILED,
                    FString::Printf(TEXT("LOD count readback for %s was %d, expected %d"),
                        *Path, ActualLodCount, NumLODs));
            }
        }

        TSharedPtr<GenerateLodsCompileLifetime::FRetainedCompileGuard> RetainedCompileGuard;
        if (TimedOutMeshes.Num() > 0 && MeshBatch.Quiesce.IsValid())
        {
            RetainedCompileGuard = MakeShared<GenerateLodsCompileLifetime::FRetainedCompileGuard>(
                MeshBatch.Quiesce, TimedOutMeshes);
        }

        TSharedPtr<FJsonObject> Result = Batch.MakeResult();
        Result->SetNumberField(TEXT("processed"), Batch.NumSucceeded());
        Result->SetNumberField(TEXT("lodCount"), NumLODs);
        Result->SetBoolField(TEXT("timedOut"), bAnyTimedOut);
        Result->SetNumberField(TEXT("timeoutSeconds"), CompileTimeoutSeconds);
        if (bSingleInputRequest && bHasSingleSaveReport)
        {
            Result->SetStringField(TEXT("package"), SinglePackageName);
            if (bSingleHasSizeReport)
            {
                AddAssetSaveSizeReport(Result, SingleSizeBytes, bSingleSavedToDisk);
            }
            AddAssetSaveReport(Result, bSingleSaveRequested,
                bSingleSavedToDisk, SingleSaveState);
        }
        if (Batch.IsEnvelopeSuccess())
        {
            Responder.SendSuccess(TEXT("LOD generation completed"), Result);
        }
        else
        {
            Responder.SendError(ErrorCodes::ERR_OPERATION_FAILED,
                TEXT("One or more meshes did not complete LOD generation; see items"), Result);
        }
        if (RetainedCompileGuard.IsValid())
        {
            RetainedCompileGuard->Start();
        }
    });
}

// ============================================================================
// asset.nanite_rebuild_mesh
// ============================================================================
namespace
{
    constexpr double DefaultNaniteCompileTimeoutSeconds = 120.0;
    constexpr double MinNaniteCompileTimeoutSeconds = 0.0;
    constexpr double MaxNaniteCompileTimeoutSeconds = 120.0;

    // The shape-preservation technique asset.nanite_rebuild_mesh was asked for, resolved before
    // the engine-version split. UE 5.7 replaced FMeshNaniteSettings' two-state `bPreserveArea`
    // bit with the three-valued ENaniteShapePreservation (EngineTypes.h); parsing into a local
    // request enum lets the pre-5.7 branch reject `voxelize` by name instead of silently
    // downgrading it to one of the two states that engine can express.
    enum class ENaniteShapePreservationRequest : uint8
    {
        Unspecified,
        None,
        PreserveArea,
        Voxelize
    };

    bool ParseNaniteShapePreservation(const FString& Value, ENaniteShapePreservationRequest& OutMode)
    {
        FString Normalized = Value;
        Normalized.TrimStartAndEndInline();
        Normalized.ReplaceInline(TEXT("-"), TEXT("_"));
        Normalized.ReplaceInline(TEXT(" "), TEXT("_"));
        Normalized.ToLowerInline();

        if (Normalized == TEXT("none"))
        {
            OutMode = ENaniteShapePreservationRequest::None;
            return true;
        }
        if (Normalized == TEXT("preserve_area") || Normalized == TEXT("preservearea"))
        {
            OutMode = ENaniteShapePreservationRequest::PreserveArea;
            return true;
        }
        if (Normalized == TEXT("voxelize"))
        {
            OutMode = ENaniteShapePreservationRequest::Voxelize;
            return true;
        }
        return false;
    }
}

REGISTER_RPC_HANDLER("asset.nanite_rebuild_mesh", "asset", "Toggle Nanite and/or rebuild its data for a StaticMesh. UE 5+ only. Triangle/fallback percentages control how aggressively the cluster representation is decimated and what fallback geometry is kept for non-Nanite paths. shapePreservation, positionPrecision and fallbackPercent are left at the mesh's stored values when omitted, so a call that only toggles Nanite does not restyle the mesh; every reported setting is read back off the asset after the write rather than echoed from the request.",
    RPC_PARAMS(
        RPC_PARAM_REQ("meshPath", "path", "Asset path to the StaticMesh."),
        RPC_PARAM_OPT("enableNanite", "boolean", "When true (default) enables Nanite on the mesh; false disables it."),
        RPC_PARAM_OPT("shapePreservation", "string", "Technique for keeping the mesh's shape at distance: none (the engine default), preserve_area (maintain surface area - the engine's own comment labels this the legacy foliage technique), or voxelize (simplify triangles to voxels; the engine recommends it for foliage that thins out otherwise). Omit to leave the mesh's stored value untouched. Requires UE 5.7+; earlier engines carry only the two-state bPreserveArea bit and reject voxelize."),
        RPC_PARAM_OPT("preserveArea", "boolean", "Deprecated two-state alias for shapePreservation: true means preserve_area, false means none, and voxelize is unreachable through it. Ignored when shapePreservation is supplied. Omit both to leave the mesh's stored value untouched."),
        RPC_PARAM_OPT("positionPrecision", "number", "Nanite position precision in bits; the quantization step is 2^(-positionPrecision) cm and the builder clamps it to -20..43. Omit to leave the mesh's stored value untouched (the engine default is automatic precision, reported as positionPrecisionAuto)."),
        RPC_PARAM_OPT("trianglePercent", "number", "Percentage of source triangles to retain in the Nanite representation, 0-100; defaults to 100."),
        RPC_PARAM_OPT("fallbackPercent", "number", "Percentage of source triangles for the non-Nanite fallback mesh, 0-100. When supplied it also sets GenerateFallback: Enabled above 0, PlatformDefault at 0. Omit to leave both at the mesh's stored values."),
        RPC_PARAM_OPT("save", "boolean", "Persist the rebuilt StaticMesh to disk (default true)."),
        RPC_PARAM_DEF("timeoutSeconds", "number", "Maximum time to wait for StaticMesh compilation (0-120 seconds). A timeout does not save.", "120")
    ))
{
    FString MeshPath = Ctx.GetString(TEXT("meshPath"));
    if (MeshPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("meshPath is required"));
        return true;
    }

    UStaticMesh* LoadedMesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
    if (!LoadedMesh)
    {
        Ctx.SendError(ErrorCodes::ERR_MESH_NOT_FOUND,
            FString::Printf(TEXT("Static mesh not found: %s"), *MeshPath));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    const bool bSave = Ctx.GetBool(TEXT("save"), true);

    double CompileTimeoutSeconds = DefaultNaniteCompileTimeoutSeconds;
    if (Payload->HasField(TEXT("timeoutSeconds")))
    {
        if (!Ctx.RequireNumber(TEXT("timeoutSeconds"), CompileTimeoutSeconds))
        {
            return true;
        }
        if (!FMath::IsFinite(CompileTimeoutSeconds)
            || CompileTimeoutSeconds < MinNaniteCompileTimeoutSeconds
            || CompileTimeoutSeconds > MaxNaniteCompileTimeoutSeconds)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("timeoutSeconds must be between %.2f and %.0f."),
                    MinNaniteCompileTimeoutSeconds, MaxNaniteCompileTimeoutSeconds));
            return true;
        }
    }

    bool bEnableNanite = true;
    Payload->TryGetBoolField(TEXT("enableNanite"), bEnableNanite);

    // shapePreservation wins over the deprecated preserveArea alias; when neither is supplied the
    // request stays Unspecified and the mesh's stored technique is left alone. Defaulting it to
    // PreserveArea, as this verb used to, flipped every mesh off the engine's None default on a
    // call made only to toggle Nanite or set a triangle percentage.
    ENaniteShapePreservationRequest ShapeRequest = ENaniteShapePreservationRequest::Unspecified;
    FString ShapePreservationValue;
    if (Payload->TryGetStringField(TEXT("shapePreservation"), ShapePreservationValue))
    {
        if (!ParseNaniteShapePreservation(ShapePreservationValue, ShapeRequest))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("Unsupported shapePreservation '%s'. Expected none, preserve_area, or voxelize"),
                    *ShapePreservationValue));
            return true;
        }
    }
    else
    {
        bool bPreserveArea = false;
        if (Payload->TryGetBoolField(TEXT("preserveArea"), bPreserveArea))
        {
            ShapeRequest = bPreserveArea
                ? ENaniteShapePreservationRequest::PreserveArea
                : ENaniteShapePreservationRequest::None;
        }
    }

    double TrianglePercent = 100.0;
    Payload->TryGetNumberField(TEXT("trianglePercent"), TrianglePercent);
    TrianglePercent = FMath::Clamp(TrianglePercent, 0.0, 100.0);

    double FallbackPercent = 0.0;
    const bool bFallbackPercentSupplied = Payload->TryGetNumberField(TEXT("fallbackPercent"), FallbackPercent);
    FallbackPercent = FMath::Clamp(FallbackPercent, 0.0, 100.0);

    int32 PositionPrecision = 0;
    const bool bPositionPrecisionSupplied = Payload->TryGetNumberField(TEXT("positionPrecision"), PositionPrecision);

#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    // 5.3-5.6 have only the bPreserveArea bit, so voxelize has no representation here. Refuse it
    // before entering the deferred callback, whose responder is the only valid response path.
    if (ShapeRequest == ENaniteShapePreservationRequest::Voxelize)
    {
        Ctx.SendUnsupportedEngineVersion(TEXT("5.7"), TEXT("Nanite shapePreservation=voxelize"));
        return true;
    }
#endif

    TArray<FString> RebuildPaths;
    RebuildPaths.Add(MeshPath);
    return PinWrightMeshRebuild::RunGuardedStaticMeshRebuild(Ctx,
        TEXT("asset.nanite_rebuild_mesh"), RebuildPaths,
        PinWrightMeshRebuild::PreserveInputSlots,
        [MeshPath, bSave, bEnableNanite, ShapeRequest, bPositionPrecisionSupplied,
         PositionPrecision, TrianglePercent, bFallbackPercentSupplied, FallbackPercent,
         CompileTimeoutSeconds](
            const PinWrightSafePoint::FSafePointResponder& Responder,
            const PinWrightMeshRebuild::FPreservedStaticMeshBatch& MeshBatch)
    {
        if (!MeshBatch.bCanRebuild)
        {
            Responder.SendError(MeshBatch.ErrorCode, MeshBatch.Error);
            return;
        }
        if (MeshBatch.Meshes.Num() == 0 || !MeshBatch.Meshes[0])
        {
            Responder.SendError(ErrorCodes::ERR_MESH_NOT_FOUND,
                FString::Printf(TEXT("Static mesh not found: %s"), *MeshPath));
            return;
        }

        UStaticMesh* StaticMesh = MeshBatch.Meshes[0];
        const double CompileDeadline = FPlatformTime::Seconds() + CompileTimeoutSeconds;

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    FMeshNaniteSettings Settings = StaticMesh->GetNaniteSettings();
    Settings.bEnabled = bEnableNanite;
    if (bPositionPrecisionSupplied)
    {
        Settings.PositionPrecision = PositionPrecision;
    }

    switch (ShapeRequest)
    {
    case ENaniteShapePreservationRequest::None:
        Settings.ShapePreservation = ENaniteShapePreservation::None;
        break;
    case ENaniteShapePreservationRequest::PreserveArea:
        Settings.ShapePreservation = ENaniteShapePreservation::PreserveArea;
        break;
    case ENaniteShapePreservationRequest::Voxelize:
        Settings.ShapePreservation = ENaniteShapePreservation::Voxelize;
        break;
    case ENaniteShapePreservationRequest::Unspecified:
        break;
    }

    Settings.KeepPercentTriangles = static_cast<float>(TrianglePercent / 100.0);
    if (bFallbackPercentSupplied)
    {
        Settings.FallbackPercentTriangles = static_cast<float>(FallbackPercent / 100.0);
        Settings.GenerateFallback = FallbackPercent > 0.0
            ? ENaniteGenerateFallback::Enabled
            : ENaniteGenerateFallback::PlatformDefault;
    }
    StaticMesh->SetNaniteSettings(Settings);
    StaticMesh->NotifyNaniteSettingsChanged();
#else
    StaticMesh->NaniteSettings.bEnabled = bEnableNanite;
    if (bPositionPrecisionSupplied)
    {
        StaticMesh->NaniteSettings.PositionPrecision = PositionPrecision;
    }
    if (ShapeRequest != ENaniteShapePreservationRequest::Unspecified)
    {
        StaticMesh->NaniteSettings.bPreserveArea = (ShapeRequest == ENaniteShapePreservationRequest::PreserveArea);
    }
    StaticMesh->NaniteSettings.KeepPercentTriangles = static_cast<float>(TrianglePercent / 100.0);
    if (bFallbackPercentSupplied)
    {
        StaticMesh->NaniteSettings.FallbackPercentTriangles = static_cast<float>(FallbackPercent / 100.0);
    }
    StaticMesh->Build(/*bSilent=*/true);
#endif

    StaticMesh->MarkPackageDirty();
    bool bCompilationTerminal = true;
    while (PinWright::AssetCompile::IsCompilingForBoundedWait(
        StaticMesh, /*bAllowTestOverride=*/true))
    {
        if (FPlatformTime::Seconds() >= CompileDeadline)
        {
            bCompilationTerminal = false;
            break;
        }

        PinWright::AssetCompile::AdvanceOnGameThread();
        if (!PinWright::AssetCompile::IsCompilingForBoundedWait(
            StaticMesh, /*bAllowTestOverride=*/true))
        {
            bCompilationTerminal = FPlatformTime::Seconds() < CompileDeadline;
            break;
        }

        const double RemainingSeconds = CompileDeadline - FPlatformTime::Seconds();
        if (RemainingSeconds <= 0.0)
        {
            bCompilationTerminal = false;
            break;
        }
        FPlatformProcess::SleepNoStats(static_cast<float>(
            FMath::Min(RemainingSeconds, 0.01)));
    }
    bCompilationTerminal = bCompilationTerminal
        && FPlatformTime::Seconds() < CompileDeadline;

    if (!bCompilationTerminal)
    {
        if (MeshBatch.Quiesce.IsValid())
        {
            TArray<UStaticMesh*> TimedOutMeshes = {StaticMesh};
            MakeShared<GenerateLodsCompileLifetime::FRetainedCompileGuard>(
                MeshBatch.Quiesce, TimedOutMeshes,
                /*bPumpCompilation=*/false,
                /*bAllowTestPendingOverride=*/true)->Start();
        }

        TSharedPtr<FJsonObject> TimeoutResult = MakeShared<FJsonObject>();
        TimeoutResult->SetStringField(TEXT("meshPath"), MeshPath);
        TimeoutResult->SetBoolField(TEXT("rebuilt"), false);
        TimeoutResult->SetBoolField(TEXT("timedOut"), true);
        TimeoutResult->SetNumberField(TEXT("timeoutSeconds"), CompileTimeoutSeconds);
        AddAssetSaveReport(TimeoutResult, /*bSaveRequested=*/false,
            /*bSavedToDisk=*/false, EAssetSaveState::NotRequested);
        Responder.SendError(ErrorCodes::ERR_OPERATION_FAILED,
            FString::Printf(TEXT("StaticMesh compilation for %s did not finish within %.2f seconds; "
                "no Nanite save was attempted"), *MeshPath, CompileTimeoutSeconds),
            TimeoutResult);
        return;
    }

    FString PackageName = StaticMesh->GetOutermost()->GetName();
    int64 SizeBytes = 0;
    bool bSavedToDisk = false;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    if (bSave)
    {
        bSavedToDisk = SaveAssetToDiskReportingPresence(
            StaticMesh, /*bForce=*/true, &PackageName, &SizeBytes, &SaveState);
    }
    else
    {
        FString PackageFilename;
        if (FPackageName::TryConvertLongPackageNameToFilename(
                PackageName, PackageFilename, FPackageName::GetAssetPackageExtension()))
        {
            SizeBytes = FMath::Max<int64>(IFileManager::Get().FileSize(*PackageFilename), 0);
        }
    }

    // Every setting below is read back off the asset after the write. Echoing the parsed request
    // would report the settings the caller already knows and hide the ones it left alone.
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("meshPath"), MeshPath);
    Resp->SetStringField(TEXT("meshName"), StaticMesh->GetName());

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    const FMeshNaniteSettings& Applied = StaticMesh->GetNaniteSettings();
    const TCHAR* AppliedShapeName = TEXT("none");
    if (Applied.ShapePreservation == ENaniteShapePreservation::PreserveArea)
    {
        AppliedShapeName = TEXT("preserve_area");
    }
    else if (Applied.ShapePreservation == ENaniteShapePreservation::Voxelize)
    {
        AppliedShapeName = TEXT("voxelize");
    }

    Resp->SetBoolField(TEXT("naniteEnabled"), Applied.bEnabled != 0);
    Resp->SetStringField(TEXT("shapePreservation"), AppliedShapeName);
    Resp->SetBoolField(TEXT("preserveArea"), Applied.ShapePreservation == ENaniteShapePreservation::PreserveArea);
    Resp->SetNumberField(TEXT("positionPrecision"), Applied.PositionPrecision);
    Resp->SetBoolField(TEXT("positionPrecisionAuto"), Applied.PositionPrecision == MIN_int32);
    Resp->SetNumberField(TEXT("trianglePercent"), Applied.KeepPercentTriangles * 100.0f);
    Resp->SetNumberField(TEXT("fallbackPercent"), Applied.FallbackPercentTriangles * 100.0f);
    Resp->SetStringField(TEXT("generateFallback"),
        Applied.GenerateFallback == ENaniteGenerateFallback::Enabled ? TEXT("Enabled") : TEXT("PlatformDefault"));

    // FMeshRayTracingProxySettings::FoliageOverOcclusionBias outranks PreserveArea in the builder:
    // Developer/NaniteBuilder/Private/Cluster.cpp takes the bias branch first and only falls
    // through to Simplifier.PreserveSurfaceArea() when the bias is zero. No verb writes the bias,
    // so report it - otherwise a caller cannot tell that the technique it just stored is dead data.
    // Voxelize is not affected: it is consumed separately (Cluster.cpp bAllowVoxels, ClusterDAG).
    const float FoliageOverOcclusionBias = StaticMesh->GetRayTracingProxySettings().FoliageOverOcclusionBias;
    Resp->SetNumberField(TEXT("foliageOverOcclusionBias"), FoliageOverOcclusionBias);
    if (FoliageOverOcclusionBias > 0.0f && Applied.ShapePreservation == ENaniteShapePreservation::PreserveArea)
    {
        TArray<TSharedPtr<FJsonValue>> Warnings;
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("shapePreservation=preserve_area is stored but will not be applied: RayTracingProxySettings.FoliageOverOcclusionBias is %g (> 0), and the Nanite builder takes the foliage-bias path instead (Developer/NaniteBuilder/Private/Cluster.cpp). Set the bias to 0, or use shapePreservation=voxelize, if the shape technique must take effect."),
            FoliageOverOcclusionBias)));
        Resp->SetArrayField(TEXT("warnings"), Warnings);
    }
#else
    Resp->SetBoolField(TEXT("naniteEnabled"), StaticMesh->NaniteSettings.bEnabled != 0);
    Resp->SetStringField(TEXT("shapePreservation"),
        StaticMesh->NaniteSettings.bPreserveArea ? TEXT("preserve_area") : TEXT("none"));
    Resp->SetBoolField(TEXT("preserveArea"), StaticMesh->NaniteSettings.bPreserveArea != 0);
    Resp->SetNumberField(TEXT("positionPrecision"), StaticMesh->NaniteSettings.PositionPrecision);
    Resp->SetBoolField(TEXT("positionPrecisionAuto"), StaticMesh->NaniteSettings.PositionPrecision == MIN_int32);
    Resp->SetNumberField(TEXT("trianglePercent"), StaticMesh->NaniteSettings.KeepPercentTriangles * 100.0f);
    Resp->SetNumberField(TEXT("fallbackPercent"), StaticMesh->NaniteSettings.FallbackPercentTriangles * 100.0f);
#endif

    Resp->SetStringField(TEXT("package"), PackageName);
    AddAssetSaveSizeReport(Resp, SizeBytes, bSavedToDisk);
    AddAssetSaveReport(Resp, bSave, bSavedToDisk, SaveState);
    Responder.SendSuccess(Resp);
    });
}

// ============================================================================
// asset.find_objects_by_tag (actors/components in the world by tag)
// ============================================================================
REGISTER_RPC_HANDLER("asset.find_objects_by_tag", "asset", "Find actors and components in the world by tag",
    RPC_PARAMS(
        RPC_PARAM_REQ("tag", "string", "Tag name to search for"),
        RPC_PARAM_OPT("maxResults", "number", "Max results (default 100)"),
        RPC_PARAM_OPT("searchActors", "boolean", "Search actors (default true)"),
        RPC_PARAM_OPT("searchComponents", "boolean", "Search components (default false)")
    ))
{
    FString Tag = Ctx.GetString(TEXT("tag"));
    if (Tag.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("tag field is required"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();

    FName TagName(*Tag);
    TArray<TSharedPtr<FJsonValue>> Results;
    int32 MaxResults = 100;
    Payload->TryGetNumberField(TEXT("maxResults"), MaxResults);
    MaxResults = FMath::Clamp(MaxResults, 1, 1000);

    bool bSearchActors = true;
    bool bSearchComponents = false;
    Payload->TryGetBoolField(TEXT("searchActors"), bSearchActors);
    Payload->TryGetBoolField(TEXT("searchComponents"), bSearchComponents);

    if (GEditor && bSearchActors)
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (World)
        {
            for (TActorIterator<AActor> It(World); It && Results.Num() < MaxResults; ++It)
            {
                AActor* Actor = *It;
                if (Actor && Actor->ActorHasTag(TagName))
                {
                    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
                    ResultObj->SetStringField(TEXT("type"), TEXT("Actor"));
                    ResultObj->SetStringField(TEXT("name"), Actor->GetName());
                    ResultObj->SetStringField(TEXT("label"), Actor->GetActorLabel());
                    ResultObj->SetStringField(TEXT("path"), Actor->GetPathName());
                    ResultObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

                    const FVector Location = Actor->GetActorLocation();
                    TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
                    LocObj->SetNumberField(TEXT("x"), Location.X);
                    LocObj->SetNumberField(TEXT("y"), Location.Y);
                    LocObj->SetNumberField(TEXT("z"), Location.Z);
                    ResultObj->SetObjectField(TEXT("location"), LocObj);

                    Results.Add(MakeShared<FJsonValueObject>(ResultObj));
                }
            }
        }
    }

    if (bSearchComponents && GEditor && Results.Num() < MaxResults)
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (World)
        {
            for (TActorIterator<AActor> It(World); It && Results.Num() < MaxResults; ++It)
            {
                AActor* Actor = *It;
                if (Actor)
                {
                    TInlineComponentArray<UActorComponent*> Components;
                    Actor->GetComponents(Components);
                    for (UActorComponent* Component : Components)
                    {
                        if (Component && Component->ComponentHasTag(TagName))
                        {
                            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
                            ResultObj->SetStringField(TEXT("type"), TEXT("Component"));
                            ResultObj->SetStringField(TEXT("name"), Component->GetName());
                            ResultObj->SetStringField(TEXT("class"), Component->GetClass()->GetName());
                            ResultObj->SetStringField(TEXT("owner"), Actor->GetName());
                            ResultObj->SetStringField(TEXT("path"), Component->GetPathName());
                            Results.Add(MakeShared<FJsonValueObject>(ResultObj));
                        }
                    }
                }
            }
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("tag"), Tag);
    Resp->SetNumberField(TEXT("count"), Results.Num());
    Resp->SetArrayField(TEXT("results"), Results);

    Ctx.SendSuccess(Resp);
    return true;
}
