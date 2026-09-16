// Copyright (c) 2026 Alexander Penkin. MIT License.

// Structural/unit regression coverage for B-asset-batch-mutators-silently-narrow-request.
// The production handlers are dispatched with fixtures whose default preflight must refuse before
// provider, rename, duplicate or rebuild work. Helper-unit and source ratchets supplement those
// wire-level checks without asking automation to perform source-control or asset mutations.
//
// Counterfactual: restoring the old filtered loops either removes the production helper this test
// compiles against or fails the per-handler wiring/order assertions, while a helper regression
// fails the exact three-input order, refusal and partial/runtime outcome assertions below.

#include "Misc/AutomationTest.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Handlers/ErrorCodes.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Utils/AssetBatchResult.h"

namespace AssetBatchMutatorTests
{
    inline bool ExtractHandlerBlock(const FString& Source, const FString& Method,
                                    FString& OutBlock)
    {
        const FString Needle = FString::Printf(
            TEXT("REGISTER_RPC_HANDLER(\"%s\""), *Method);
        const int32 Start = Source.Find(Needle);
        if (Start == INDEX_NONE)
        {
            return false;
        }

        int32 End = Source.Find(TEXT("REGISTER_RPC_HANDLER("),
            ESearchCase::CaseSensitive, ESearchDir::FromStart, Start + Needle.Len());
        if (End == INDEX_NONE)
        {
            End = Source.Len();
        }
        OutBlock = Source.Mid(Start, End - Start);
        return true;
    }

    inline const FParamSpec* FindParam(const FHandlerRegistration& Registration,
                                      const TCHAR* Name)
    {
        return Registration.Params.FindByPredicate(
            [Name](const FParamSpec& Param)
            {
                return Param.Name == Name;
            });
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAssetBatchMutatorsPreserveItemsAndRefusePartialTest,
    "PinWright.asset.batch_mutators.PreserveItemsAndRefusePartial",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetBatchMutatorsPreserveItemsAndRefusePartialTest::RunTest(
    const FString& /*Parameters*/)
{
    using namespace AssetBatchMutatorTests;
    using PinWrightAssetBatch::FAssetBatchResult;

    const TSharedPtr<FJsonValue> FirstInput =
        MakeShared<FJsonValueString>(TEXT("/Game/Batch/First"));
    const TSharedPtr<FJsonValue> MalformedInput =
        MakeShared<FJsonValueNumber>(17.0);
    const TSharedPtr<FJsonValue> ThirdInput =
        MakeShared<FJsonValueString>(TEXT("/Game/Batch/Third"));

    FAssetBatchResult DefaultBatch(/*bInAllowPartial=*/false);
    const int32 DefaultFirst = DefaultBatch.AddInput(FirstInput);
    const int32 DefaultMalformed = DefaultBatch.AddInput(MalformedInput);
    const int32 DefaultThird = DefaultBatch.AddInput(ThirdInput);
    DefaultBatch.MarkReady(DefaultFirst);
    DefaultBatch.FailPreflight(DefaultMalformed, ErrorCodes::ERR_INVALID_ARGUMENT,
        TEXT("assetPaths[1] must be a string"));
    DefaultBatch.MarkReady(DefaultThird);

    TestTrue(TEXT("default batch detects a preflight failure"),
        DefaultBatch.HasPreflightFailure());
    TestTrue(TEXT("default batch refuses before mutation"),
        DefaultBatch.ShouldRefuseBeforeMutation());
    DefaultBatch.RefuseReadyItems(ErrorCodes::ERR_BATCH_NOT_ATTEMPTED,
        TEXT("Not attempted because another batch item failed preflight"));

    const TSharedPtr<FJsonObject> DefaultResult = DefaultBatch.MakeResult();
    if (!TestTrue(TEXT("default result exists"), DefaultResult.IsValid()))
    {
        return false;
    }
    TestFalse(TEXT("a refused default batch has a failed envelope"),
        DefaultResult->GetBoolField(TEXT("success")));
    TestFalse(TEXT("default result reports partial false"),
        DefaultResult->GetBoolField(TEXT("partial")));
    TestEqual(TEXT("default result preserves requested count"),
        DefaultResult->GetNumberField(TEXT("requested")), 3.0);
    TestEqual(TEXT("default refusal attempts nothing"),
        DefaultResult->GetNumberField(TEXT("attempted")), 0.0);
    TestEqual(TEXT("default refusal succeeds nowhere"),
        DefaultResult->GetNumberField(TEXT("succeeded")), 0.0);

    const TArray<TSharedPtr<FJsonValue>>* DefaultItems = nullptr;
    if (!TestTrue(TEXT("default result exposes items"),
            DefaultResult->TryGetArrayField(TEXT("items"), DefaultItems)
                && DefaultItems != nullptr)
        || !TestEqual(TEXT("default items retain every original input"),
            DefaultItems ? DefaultItems->Num() : 0, 3))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> DefaultItem0 = (*DefaultItems)[0]->AsObject();
    const TSharedPtr<FJsonObject> DefaultItem1 = (*DefaultItems)[1]->AsObject();
    const TSharedPtr<FJsonObject> DefaultItem2 = (*DefaultItems)[2]->AsObject();
    TestEqual(TEXT("first raw input remains first"),
        DefaultItem0->GetStringField(TEXT("input")), FirstInput->AsString());
    TestEqual(TEXT("malformed raw input remains in the middle"),
        DefaultItem1->GetNumberField(TEXT("input")), 17.0);
    TestEqual(TEXT("third raw input remains third"),
        DefaultItem2->GetStringField(TEXT("input")), ThirdInput->AsString());
    TestEqual(TEXT("ready survivor is explicitly not attempted"),
        DefaultItem0->GetStringField(TEXT("code")),
        FString(ErrorCodes::ERR_BATCH_NOT_ATTEMPTED));
    TestEqual(TEXT("malformed item keeps its own cause"),
        DefaultItem1->GetStringField(TEXT("code")),
        FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    TestFalse(TEXT("malformed item keeps a useful error"),
        DefaultItem1->GetStringField(TEXT("error")).IsEmpty());
    TestEqual(TEXT("last ready survivor is explicitly not attempted"),
        DefaultItem2->GetStringField(TEXT("code")),
        FString(ErrorCodes::ERR_BATCH_NOT_ATTEMPTED));

    FAssetBatchResult PartialBatch(/*bInAllowPartial=*/true);
    const int32 PartialFirst = PartialBatch.AddInput(FirstInput);
    const int32 PartialMalformed = PartialBatch.AddInput(MalformedInput);
    const int32 PartialThird = PartialBatch.AddInput(ThirdInput);
    PartialBatch.MarkReady(PartialFirst);
    PartialBatch.FailPreflight(PartialMalformed, ErrorCodes::ERR_INVALID_ARGUMENT,
        TEXT("assetPaths[1] must be a string"));
    PartialBatch.MarkReady(PartialThird);
    PartialBatch.MarkSucceeded(PartialFirst);
    PartialBatch.FailRuntime(PartialThird, ErrorCodes::ERR_OPERATION_FAILED,
        TEXT("operation readback failed"));

    TestFalse(TEXT("partial:true does not refuse valid survivors at preflight"),
        PartialBatch.ShouldRefuseBeforeMutation());
    const TSharedPtr<FJsonObject> PartialResult = PartialBatch.MakeResult();
    TestTrue(TEXT("partial:true may succeed when one item completes"),
        PartialResult->GetBoolField(TEXT("success")));
    TestTrue(TEXT("partial result identifies explicit partial mode"),
        PartialResult->GetBoolField(TEXT("partial")));
    TestEqual(TEXT("partial result keeps all requested rows"),
        PartialResult->GetNumberField(TEXT("requested")), 3.0);
    TestEqual(TEXT("partial result counts attempted rows from outcomes"),
        PartialResult->GetNumberField(TEXT("attempted")), 2.0);
    TestEqual(TEXT("partial result counts measured successes"),
        PartialResult->GetNumberField(TEXT("succeeded")), 1.0);
    TestEqual(TEXT("partial result counts all unfinished rows"),
        PartialResult->GetNumberField(TEXT("failed")), 2.0);

    const TArray<TSharedPtr<FJsonValue>>* PartialItems = nullptr;
    if (!TestTrue(TEXT("partial result exposes items"),
            PartialResult->TryGetArrayField(TEXT("items"), PartialItems)
                && PartialItems != nullptr)
        || !TestEqual(TEXT("partial items retain every original input"),
            PartialItems ? PartialItems->Num() : 0, 3))
    {
        return false;
    }
    TestTrue(TEXT("first partial item succeeded"),
        (*PartialItems)[0]->AsObject()->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("partial mode retains the malformed failure"),
        (*PartialItems)[1]->AsObject()->GetStringField(TEXT("code")),
        FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    TestEqual(TEXT("partial mode retains the runtime failure"),
        (*PartialItems)[2]->AsObject()->GetStringField(TEXT("code")),
        FString(ErrorCodes::ERR_OPERATION_FAILED));
    TestFalse(TEXT("partial runtime failure retains a useful error"),
        (*PartialItems)[2]->AsObject()->GetStringField(TEXT("error")).IsEmpty());

    FAssetBatchResult FolderStyleBatch(/*bInAllowPartial=*/false);
    const int32 Child = FolderStyleBatch.AddItem();
    FolderStyleBatch.GetItem(Child)->SetStringField(
        TEXT("sourcePath"), TEXT("/Game/Source/Child"));
    FolderStyleBatch.GetItem(Child)->SetStringField(
        TEXT("destinationPath"), TEXT("/Game/Destination/Child"));
    FolderStyleBatch.MarkReady(Child);
    FolderStyleBatch.MarkSucceeded(Child);
    const TSharedPtr<FJsonObject> FolderResult = FolderStyleBatch.MakeResult();
    const TArray<TSharedPtr<FJsonValue>>& FolderItems =
        FolderResult->GetArrayField(TEXT("items"));
    TestEqual(TEXT("folder-style rows retain the discovered source child"),
        FolderItems[0]->AsObject()->GetStringField(TEXT("sourcePath")),
        FString(TEXT("/Game/Source/Child")));
    TestEqual(TEXT("folder-style rows retain the derived destination child"),
        FolderItems[0]->AsObject()->GetStringField(TEXT("destinationPath")),
        FString(TEXT("/Game/Destination/Child")));

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);
    const TMap<FString, FHandlerRegistration>& Registry =
        Dispatcher.GetAutoRegisteredHandlers();
    const TCHAR* const Methods[] = {
        TEXT("asset.source_control_checkout"),
        TEXT("asset.source_control_submit"),
        TEXT("asset.bulk_rename"),
        TEXT("asset.duplicate"),
        TEXT("asset.generate_lods")
    };
    for (const TCHAR* Method : Methods)
    {
        const FHandlerRegistration* Registration = Registry.Find(Method);
        if (!TestNotNull(*FString::Printf(TEXT("%s is registered"), Method),
                Registration))
        {
            return false;
        }
        const FParamSpec* PartialParam = FindParam(*Registration, TEXT("partial"));
        if (!TestNotNull(*FString::Printf(TEXT("%s declares partial"), Method),
                PartialParam))
        {
            return false;
        }
        TestEqual(*FString::Printf(TEXT("%s declares partial as boolean"), Method),
            PartialParam->Type, FString(TEXT("boolean")));
        TestFalse(*FString::Printf(TEXT("%s keeps partial optional"), Method),
            PartialParam->bRequired);
    }

    // These four array handlers all see the same shape: one real engine asset, a malformed
    // element, then a missing asset. The handler must return synchronously from default preflight
    // with all three rows and the real asset untouched. In a build with the old filtered loops,
    // the malformed and missing rows disappear and the surviving operation is attempted.
    const FString EngineCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
    const FString MissingAssetPath =
        TEXT("/Engine/BasicShapes/PW_BatchMutatorMissing.PW_BatchMutatorMissing");
    UStaticMesh* EngineCube = LoadObject<UStaticMesh>(nullptr, *EngineCubePath);
    if (!TestNotNull(TEXT("required engine cube fixture resolves"), EngineCube))
    {
        return false;
    }

    const auto MakeMixedInputs = [&]()
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(3);
        Values.Add(MakeShared<FJsonValueString>(EngineCubePath));
        Values.Add(MakeShared<FJsonValueNumber>(17.0));
        Values.Add(MakeShared<FJsonValueString>(MissingAssetPath));
        return Values;
    };

    const auto DispatchDefaultRefusal = [&](const TCHAR* Method, const FString& RequestId,
                                             const TSharedPtr<FJsonObject>& Params,
                                             const TCHAR* MissingCode)
        -> TSharedPtr<FJsonObject>
    {
        bool bSuccess = true;
        TSharedPtr<FJsonObject> Result;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method, RequestId, Params,
            bSuccess, Result, ErrorCode);

        TestTrue(*FString::Printf(TEXT("%s responds synchronously"), Method),
            Sink->bWasCalled);
        TestFalse(*FString::Printf(TEXT("%s default mixed batch fails"), Method),
            bSuccess);
        TestEqual(*FString::Printf(TEXT("%s reports batch preflight refusal"), Method),
            ErrorCode, FString(ErrorCodes::ERR_BATCH_PREFLIGHT_FAILED));
        if (!TestTrue(*FString::Printf(TEXT("%s returns its result envelope"), Method),
                Result.IsValid()))
        {
            return nullptr;
        }

        TestFalse(*FString::Printf(TEXT("%s result success is false"), Method),
            Result->GetBoolField(TEXT("success")));
        TestFalse(*FString::Printf(TEXT("%s result records default partial:false"), Method),
            Result->GetBoolField(TEXT("partial")));
        TestEqual(*FString::Printf(TEXT("%s preserves requested count"), Method),
            Result->GetNumberField(TEXT("requested")), 3.0);
        TestEqual(*FString::Printf(TEXT("%s attempts no survivor"), Method),
            Result->GetNumberField(TEXT("attempted")), 0.0);

        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        if (!TestTrue(*FString::Printf(TEXT("%s returns items"), Method),
                Result->TryGetArrayField(TEXT("items"), Items) && Items != nullptr)
            || !TestEqual(*FString::Printf(TEXT("%s keeps every input row"), Method),
                Items ? Items->Num() : 0, 3))
        {
            return nullptr;
        }

        const TSharedPtr<FJsonObject> ValidItem = (*Items)[0]->AsObject();
        const TSharedPtr<FJsonObject> MalformedItem = (*Items)[1]->AsObject();
        const TSharedPtr<FJsonObject> MissingItem = (*Items)[2]->AsObject();
        TestEqual(*FString::Printf(TEXT("%s keeps valid input first"), Method),
            ValidItem->GetStringField(TEXT("input")), EngineCubePath);
        TestFalse(*FString::Printf(TEXT("%s does not attempt the valid survivor"), Method),
            ValidItem->GetBoolField(TEXT("attempted")));
        TestEqual(*FString::Printf(TEXT("%s keeps malformed input second"), Method),
            MalformedItem->GetNumberField(TEXT("input")), 17.0);
        TestEqual(*FString::Printf(TEXT("%s identifies malformed row"), Method),
            MalformedItem->GetStringField(TEXT("code")),
            FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        TestEqual(*FString::Printf(TEXT("%s keeps missing input last"), Method),
            MissingItem->GetStringField(TEXT("input")), MissingAssetPath);
        TestEqual(*FString::Printf(TEXT("%s identifies missing row"), Method),
            MissingItem->GetStringField(TEXT("code")), FString(MissingCode));
        return Result;
    };

    for (const TCHAR* Method : { TEXT("asset.source_control_checkout"),
                                 TEXT("asset.source_control_submit") })
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetArrayField(TEXT("assetPaths"), MakeMixedInputs());
        if (!DispatchDefaultRefusal(Method,
                FString::Printf(TEXT("req-%s-default-refusal"), Method).Replace(
                    TEXT("."), TEXT("-")),
                Params, ErrorCodes::ERR_ASSET_NOT_FOUND).IsValid())
        {
            return false;
        }
    }

    const FString CubePathBeforeRename = EngineCube->GetPathName();
    TSharedPtr<FJsonObject> RenameParams = MakeShared<FJsonObject>();
    RenameParams->SetArrayField(TEXT("assetPaths"), MakeMixedInputs());
    RenameParams->SetStringField(TEXT("prefix"), TEXT("PW_BatchRefusal_"));
    if (!DispatchDefaultRefusal(TEXT("asset.bulk_rename"),
            TEXT("req-bulk-rename-default-refusal"), RenameParams,
            ErrorCodes::ERR_ASSET_NOT_FOUND).IsValid())
    {
        return false;
    }
    TestEqual(TEXT("bulk rename leaves the valid survivor at its old path"),
        EngineCube->GetPathName(), CubePathBeforeRename);

    const int32 CubeLodCountBefore = EngineCube->GetNumSourceModels();
    TSharedPtr<FJsonObject> LodParams = MakeShared<FJsonObject>();
    LodParams->SetArrayField(TEXT("assetPaths"), MakeMixedInputs());
    LodParams->SetNumberField(TEXT("lodCount"), CubeLodCountBefore);
    LodParams->SetBoolField(TEXT("save"), false);
    if (!DispatchDefaultRefusal(TEXT("asset.generate_lods"),
            TEXT("req-generate-lods-default-refusal"), LodParams,
            ErrorCodes::ERR_MESH_NOT_FOUND).IsValid())
    {
        return false;
    }
    TestEqual(TEXT("LOD refusal leaves the valid survivor's source models unchanged"),
        EngineCube->GetNumSourceModels(), CubeLodCountBefore);

    // A same-folder duplicate is a read-only production-branch fixture: every derived target
    // already exists, so default preflight must retain all discovered children and duplicate none.
    TSharedPtr<FJsonObject> DuplicateParams = MakeShared<FJsonObject>();
    DuplicateParams->SetStringField(TEXT("sourcePath"), TEXT("/Engine/BasicShapes"));
    DuplicateParams->SetStringField(TEXT("destinationPath"), TEXT("/Engine/BasicShapes"));
    bool bDuplicateSuccess = true;
    TSharedPtr<FJsonObject> DuplicateResult;
    FString DuplicateErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("asset.duplicate"),
        TEXT("req-folder-duplicate-default-refusal"), DuplicateParams, bDuplicateSuccess,
        DuplicateResult, DuplicateErrorCode);
    TestTrue(TEXT("folder duplicate responds synchronously"), Sink->bWasCalled);
    TestFalse(TEXT("same-folder duplicate is refused"), bDuplicateSuccess);
    TestEqual(TEXT("folder duplicate reports batch preflight refusal"), DuplicateErrorCode,
        FString(ErrorCodes::ERR_BATCH_PREFLIGHT_FAILED));
    if (!TestTrue(TEXT("folder duplicate returns result"), DuplicateResult.IsValid()))
    {
        return false;
    }
    const int32 DiscoveredCount = static_cast<int32>(
        DuplicateResult->GetNumberField(TEXT("requested")));
    TestTrue(TEXT("folder duplicate exercises discovered child rows"), DiscoveredCount > 0);
    TestEqual(TEXT("folder duplicate attempts no child"),
        DuplicateResult->GetNumberField(TEXT("attempted")), 0.0);
    TestEqual(TEXT("folder duplicate reports no duplicate"),
        DuplicateResult->GetNumberField(TEXT("duplicatedCount")), 0.0);
    const TArray<TSharedPtr<FJsonValue>>* DuplicateItems = nullptr;
    if (!TestTrue(TEXT("folder duplicate returns items"),
            DuplicateResult->TryGetArrayField(TEXT("items"), DuplicateItems)
                && DuplicateItems != nullptr)
        || !TestEqual(TEXT("folder duplicate retains every discovered child"),
            DuplicateItems ? DuplicateItems->Num() : 0, DiscoveredCount))
    {
        return false;
    }
    for (const TSharedPtr<FJsonValue>& ItemValue : *DuplicateItems)
    {
        const TSharedPtr<FJsonObject> Item = ItemValue->AsObject();
        TestFalse(TEXT("folder child was not attempted"),
            Item->GetBoolField(TEXT("attempted")));
        TestEqual(TEXT("same-folder target is the discovered source"),
            Item->GetStringField(TEXT("destinationPath")),
            Item->GetStringField(TEXT("sourcePath")));
        TestEqual(TEXT("existing folder target keeps its specific failure"),
            Item->GetStringField(TEXT("code")),
            FString(ErrorCodes::ERR_DESTINATION_EXISTS));
    }

    const TSharedPtr<IPlugin> Plugin =
        IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    if (!TestTrue(TEXT("PinWright plugin is available for source ratchet"),
            Plugin.IsValid()))
    {
        return false;
    }

    const FString PrivateRoot =
        Plugin->GetBaseDir() / TEXT("Source/PinWright/Private");
    FString RawWorkflowSource;
    FString RawManageSource;
    FString RawGuardSource;
    if (!TestTrue(TEXT("read workflow handler source"), FFileHelper::LoadFileToString(
            RawWorkflowSource,
            *(PrivateRoot / TEXT("Handlers/Asset/AssetWorkflowHandler.cpp"))))
        || !TestTrue(TEXT("read manage handler source"), FFileHelper::LoadFileToString(
            RawManageSource,
            *(PrivateRoot / TEXT("Handlers/Asset/AssetManageHandler.cpp"))))
        || !TestTrue(TEXT("read mesh rebuild guard source"), FFileHelper::LoadFileToString(
            RawGuardSource,
            *(PrivateRoot / TEXT("Utils/MeshRebuildRenderGuard.h")))))
    {
        return false;
    }

    const FString WorkflowSource = NeutralizeSourceText(RawWorkflowSource);
    const FString ManageSource = NeutralizeSourceText(RawManageSource);
    const FString GuardSource = NeutralizeSourceText(RawGuardSource);
    TMap<FString, FString> Blocks;
    for (const TCHAR* Method : Methods)
    {
        FString Block;
        const FString& OwnerSource = FString(Method) == TEXT("asset.duplicate")
            ? ManageSource
            : WorkflowSource;
        if (!TestTrue(*FString::Printf(TEXT("isolate %s source"), Method),
                ExtractHandlerBlock(OwnerSource, Method, Block)))
        {
            return false;
        }
        Blocks.Add(Method, MoveTemp(Block));
    }

    for (const TCHAR* Method : Methods)
    {
        const FString& Block = Blocks.FindChecked(Method);
        TestTrue(*FString::Printf(TEXT("%s reads partial with a false default"), Method),
            Block.Contains(TEXT("Ctx.GetBool(TEXT(\"partial\"), false)")));
        TestTrue(*FString::Printf(TEXT("%s uses the production batch helper"), Method),
            Block.Contains(TEXT("PinWrightAssetBatch::FAssetBatchResult")));
        TestTrue(*FString::Printf(TEXT("%s refuses failed default preflight"), Method),
            Block.Contains(TEXT("Batch.ShouldRefuseBeforeMutation()")));
        TestTrue(*FString::Printf(TEXT("%s returns helper-built items and counts"), Method),
            Block.Contains(TEXT("Batch.MakeResult()")));
        TestTrue(*FString::Printf(TEXT("%s names unattempted survivors"), Method),
            Block.Contains(TEXT("ErrorCodes::ERR_BATCH_NOT_ATTEMPTED")));
    }

    TestTrue(TEXT("checkout uses the shared source-control preflight"),
        Blocks.FindChecked(TEXT("asset.source_control_checkout")).Contains(
            TEXT("AssetWorkflow_PreflightSourceControlItems")));
    TestTrue(TEXT("submit uses the shared source-control preflight"),
        Blocks.FindChecked(TEXT("asset.source_control_submit")).Contains(
            TEXT("AssetWorkflow_PreflightSourceControlItems")));
    TestTrue(TEXT("source control resolves the real on-disk package extension"),
        WorkflowSource.Contains(
            TEXT("FPackageName::DoesPackageExist(PackageName, &OutFilename)")));

    const FString& RenameBlock = Blocks.FindChecked(TEXT("asset.bulk_rename"));
    const int32 OldPathCapture = RenameBlock.Find(
        TEXT("const FString OldPath = Asset->GetPathName()"));
    const int32 RenameCall = RenameBlock.Find(TEXT("AssetTools.RenameAssets("));
    TestTrue(TEXT("bulk rename captures oldPath before the mutation call"),
        OldPathCapture != INDEX_NONE && RenameCall > OldPathCapture);
    TestTrue(TEXT("bulk rename checkout uses the actual package filename helper"),
        RenameBlock.Contains(TEXT("AssetWorkflow_TryResolvePackageFilename")));

    const FString& DuplicateBlock = Blocks.FindChecked(TEXT("asset.duplicate"));
    const int32 DuplicateRefusal = DuplicateBlock.Find(
        TEXT("Batch.ShouldRefuseBeforeMutation()"));
    const int32 FirstDirectoryCreate = DuplicateBlock.Find(
        TEXT("UEditorAssetLibrary::MakeDirectory("));
    const int32 FirstDuplicate = DuplicateBlock.Find(
        TEXT("UEditorAssetLibrary::DuplicateAsset("));
    TestTrue(TEXT("folder duplicate creates no directory before default preflight passes"),
        DuplicateRefusal != INDEX_NONE && FirstDirectoryCreate > DuplicateRefusal);
    TestTrue(TEXT("folder duplicate changes no asset before default preflight passes"),
        DuplicateRefusal != INDEX_NONE && FirstDuplicate > DuplicateRefusal);
    const int32 DuplicateTargetCheck = DuplicateBlock.Find(
        TEXT("RequestedTargets.Contains(TargetKey)"));
    TestTrue(TEXT("folder duplicate rejects repeated canonical targets during preflight"),
        DuplicateBlock.Contains(TEXT("const FName TargetKey"))
        && DuplicateTargetCheck != INDEX_NONE && DuplicateTargetCheck < DuplicateRefusal);

    const FString& LodBlock = Blocks.FindChecked(TEXT("asset.generate_lods"));
    const int32 LodRefusal = LodBlock.Find(TEXT("Batch.ShouldRefuseBeforeMutation()"));
    const int32 LodGuardCall = LodBlock.Find(
        TEXT("PinWrightMeshRebuild::RunGuardedStaticMeshRebuild("));
    TestTrue(TEXT("LOD default preflight refusal precedes the safe-point rebuild"),
        LodRefusal != INDEX_NONE && LodGuardCall > LodRefusal);
    TestTrue(TEXT("LOD requests the slot-preserving render guard overload"),
        LodBlock.Contains(TEXT("PinWrightMeshRebuild::PreserveInputSlots")));
    TestTrue(TEXT("LOD hands the guard its retained resolved object path"),
        LodBlock.Contains(TEXT("Paths.Add(RebuildPath)")));
    TestTrue(TEXT("mesh guard retains the original filtered overload"),
        GuardSource.Contains(TEXT("using FStaticMeshRebuildWork")));
    TestTrue(TEXT("mesh guard also exposes the slot-preserving overload"),
        GuardSource.Contains(TEXT("struct FPreserveInputSlotsTag"))
        && GuardSource.Contains(TEXT("FPreservedStaticMeshRebuildWork")));

    return true;
}
