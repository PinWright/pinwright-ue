// Copyright (c) 2026 Alexander Penkin. MIT License.

// animation.retarget_animations - run the IK Retargeter batch export
// (UIKRetargetBatchOperation::RunRetarget) over AnimSequences and verify every output by reading
// it back. Lives beside the IK Rig / Retargeter authoring verbs (animation.authoring.*) in the
// main module, which already links IKRig + IKRigEditor (PinWright.Build.cs).
//
// Engine range: RunRetarget(FIKRetargetBatchOperationContext&) is the one entry point present
// 5.3-5.8 (RunBatchRetarget is 5.8-only and only wraps it; DuplicateAndRetarget is deprecated in
// 5.8), so the export itself needs no version guard. The op stack (GetNumRetargetOps /
// AddDefaultOps) is 5.6+ and is guarded below; see docs/engine-version-support.md.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Animation/AnimationHandlerTestHooks.h"
#include "Utils/AssetUtils.h"
#include "Utils/GuardedLoad.h"
#include "Utils/PieState.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"

#if __has_include("RetargetEditor/IKRetargetBatchOperation.h") \
    && __has_include("RetargetEditor/IKRetargeterController.h") \
    && __has_include("Retargeter/IKRetargeter.h") \
    && __has_include("Rig/IKRigDefinition.h")
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#define PW_HAS_IK_BATCH_RETARGET 1
#else
#define PW_HAS_IK_BATCH_RETARGET 0
#endif

namespace PwRetargetAnimations
{
    TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FString& Value : Values)
        {
            Out.Add(MakeShared<FJsonValueString>(Value));
        }
        return Out;
    }

    // Object paths of every asset registered directly under Folder (in-memory assets included),
    // so "what this call created" is a before/after measurement rather than a list of names the
    // engine was asked to produce.
    TSet<FString> AssetsInFolder(const FString& Folder)
    {
        IAssetRegistry& Registry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        TArray<FAssetData> Found;
        Registry.GetAssetsByPath(FName(*Folder), Found, /*bRecursive=*/false);
        TSet<FString> Paths;
        for (const FAssetData& Data : Found)
        {
            Paths.Add(Data.GetSoftObjectPath().ToString());
        }
        return Paths;
    }
}

REGISTER_RPC_HANDLER("animation.retarget_animations", "animation",
    "Retarget AnimSequences from sourceMesh to targetMesh through an IK Retargeter (the engine's "
    "batch export, UIKRetargetBatchOperation). Refuses unmapped target chains, an empty op stack "
    "(UE 5.6+), skeleton mismatches and existing outputs before any write; reads every output "
    "back (skeleton, frames, bone tracks) and deletes everything it created on a partial result.",
    RPC_PARAMS(
        RPC_PARAM_REQ("retargeter", "path", "UIKRetargeter asset with source and target IK Rigs assigned."),
        RPC_PARAM_REQ("sourceMesh", "path", "SkeletalMesh the clips are evaluated on; its Skeleton must be compatible with every clip's."),
        RPC_PARAM_REQ("targetMesh", "path", "SkeletalMesh to retarget onto; outputs get its Skeleton."),
        RPC_PARAM_REQ("assets", "array", "AnimSequence object paths to retarget."),
        RPC_PARAM_REQ("outputPath", "path", "Content folder for the outputs (e.g. /Game/Anims/Retargeted)."),
        RPC_PARAM_OPT("prefix", "string", "Prepended to each output name."),
        RPC_PARAM_OPT("suffix", "string", "Appended to each output name."),
        RPC_PARAM_DEF("requireCompleteMapping", "boolean", "Refuse with RETARGET_CHAINS_UNMAPPED when any target chain has no source chain.", "true"),
        RPC_PARAM_DEF("seedDefaultOps", "boolean", "UE 5.6+: add the engine's default op stack (AddDefaultOps) to a retargeter that has none, instead of refusing RETARGETER_NO_OPS. Mutates the retargeter.", "false")
    ))
{
#if PW_HAS_IK_BATCH_RETARGET
    using namespace PwRetargetAnimations;

    if (PinWrightPieState::IsPlayInEditorActive())
    {
        Ctx.SendError(ErrorCodes::ERR_PIE_ACTIVE,
            TEXT("animation.retarget_animations cannot run while the editor is in play mode; stop PIE and retry."));
        return true;
    }

    FString RetargeterPath, SourceMeshPath, TargetMeshPath, OutputPath;
    if (!Ctx.RequireAssetPath(TEXT("retargeter"), RetargeterPath)
        || !Ctx.RequireAssetPath(TEXT("sourceMesh"), SourceMeshPath)
        || !Ctx.RequireAssetPath(TEXT("targetMesh"), TargetMeshPath)
        || !Ctx.RequireAssetPath(TEXT("outputPath"), OutputPath))
    {
        return true;
    }
    OutputPath.RemoveFromEnd(TEXT("/"));
    if (OutputPath.Contains(TEXT("."))
        || FPackageName::GetPackageMountPoint(OutputPath).IsNone()
        || !FPackageName::IsValidLongPackageName(OutputPath))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, FString::Printf(
            TEXT("outputPath '%s' is not a writable content folder (expected e.g. /Game/Anims/Retargeted)."),
            *OutputPath));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* AssetValues = Ctx.GetArray(TEXT("assets"));
    TArray<FString> AssetPaths;
    if (AssetValues)
    {
        for (const TSharedPtr<FJsonValue>& Value : *AssetValues)
        {
            FString Path;
            if (!Value.IsValid() || !Value->TryGetString(Path) || Path.TrimStartAndEnd().IsEmpty())
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    TEXT("assets must be a non-empty array of AnimSequence object paths (strings)."));
                return true;
            }
            AssetPaths.Add(Path.TrimStartAndEnd());
        }
    }
    if (AssetPaths.Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("assets is empty; pass at least one AnimSequence object path."));
        return true;
    }

    if (SourceMeshPath == TargetMeshPath)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("sourceMesh and targetMesh are the same asset; the batch export requires two different meshes."));
        return true;
    }

    const FString Prefix = Ctx.GetString(TEXT("prefix"));
    const FString Suffix = Ctx.GetString(TEXT("suffix"));
    const bool bRequireCompleteMapping = Ctx.GetBool(TEXT("requireCompleteMapping"), true);
    const bool bSeedDefaultOps = Ctx.GetBool(TEXT("seedDefaultOps"), false);

    UIKRetargeter* Retargeter = LoadObject<UIKRetargeter>(nullptr, *RetargeterPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
    USkeletalMesh* SourceMesh = LoadObject<USkeletalMesh>(nullptr, *SourceMeshPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
    USkeletalMesh* TargetMesh = LoadObject<USkeletalMesh>(nullptr, *TargetMeshPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
    {
        TArray<FString> Missing;
        if (!Retargeter) { Missing.Add(FString::Printf(TEXT("retargeter '%s' (UIKRetargeter)"), *RetargeterPath)); }
        if (!SourceMesh) { Missing.Add(FString::Printf(TEXT("sourceMesh '%s' (SkeletalMesh)"), *SourceMeshPath)); }
        if (!TargetMesh) { Missing.Add(FString::Printf(TEXT("targetMesh '%s' (SkeletalMesh)"), *TargetMeshPath)); }
        if (Missing.Num() > 0)
        {
            Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                FString::Printf(TEXT("Not found: %s."), *FString::Join(Missing, TEXT(", "))));
            return true;
        }
    }
    USkeleton* SourceSkeleton = SourceMesh->GetSkeleton();
    USkeleton* TargetSkeleton = TargetMesh->GetSkeleton();
    if (!SourceSkeleton || !TargetSkeleton)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
            TEXT("%s has no Skeleton."), !SourceSkeleton ? *SourceMeshPath : *TargetMeshPath));
        return true;
    }

    // Load and vet every clip before anything is written.
    TArray<UAnimSequence*> Sources;
    TArray<FString> MissingAssets, WrongTypeAssets, MismatchedAssets;
    for (const FString& Path : AssetPaths)
    {
        UObject* Loaded = PinWrightGuardedLoad::LoadObjectChecked<UObject>(Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
        UAnimSequence* Sequence = Cast<UAnimSequence>(Loaded);
        if (!Loaded)
        {
            MissingAssets.Add(Path);
        }
        else if (!Sequence)
        {
            WrongTypeAssets.Add(FString::Printf(TEXT("%s (%s)"), *Path, *Loaded->GetClass()->GetName()));
        }
        else if (!Sequence->GetSkeleton() || !SourceSkeleton->IsCompatibleForEditor(Sequence->GetSkeleton()))
        {
            MismatchedAssets.Add(FString::Printf(TEXT("%s (skeleton %s)"), *Path,
                Sequence->GetSkeleton() ? *Sequence->GetSkeleton()->GetPathName() : TEXT("none")));
        }
        else
        {
            Sources.AddUnique(Sequence);
        }
    }
    if (MissingAssets.Num() > 0)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetArrayField(TEXT("missingAssets"), ToJsonStrings(MissingAssets));
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(
            TEXT("Animation assets not found: %s."), *FString::Join(MissingAssets, TEXT(", "))), Data);
        return true;
    }
    if (WrongTypeAssets.Num() > 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_TYPE, FString::Printf(
            TEXT("Only AnimSequence assets are retargeted by this verb: %s."), *FString::Join(WrongTypeAssets, TEXT(", "))));
        return true;
    }
    if (MismatchedAssets.Num() > 0)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetArrayField(TEXT("mismatchedAssets"), ToJsonStrings(MismatchedAssets));
        Data->SetStringField(TEXT("sourceSkeleton"), SourceSkeleton->GetPathName());
        Ctx.SendError(ErrorCodes::ERR_SKELETON_MISMATCH, FString::Printf(
            TEXT("These clips are not on a skeleton compatible with sourceMesh's '%s': %s. Pass the mesh the clips were authored for as sourceMesh."),
            *SourceSkeleton->GetPathName(), *FString::Join(MismatchedAssets, TEXT(", "))), Data);
        return true;
    }

    UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
    const UIKRigDefinition* SourceRig = Controller ? Controller->GetIKRig(ERetargetSourceOrTarget::Source) : nullptr;
    const UIKRigDefinition* TargetRig = Controller ? Controller->GetIKRig(ERetargetSourceOrTarget::Target) : nullptr;
    if (!SourceRig || !TargetRig)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
            TEXT("IK Retargeter '%s' has no %s IK Rig assigned; assign both (animation.authoring.create_ik_retargeter sourceIKRigPath/targetIKRigPath)."),
            *RetargeterPath, !SourceRig ? TEXT("source") : TEXT("target")));
        return true;
    }

    // UE 5.6 moved chain mapping and every retarget step onto an op stack; a retargeter without
    // ops retargets nothing. Older retargeters have no op stack, so there is nothing to seed.
    bool bSeededDefaultOps = false;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    if (Controller->GetNumRetargetOps() == 0)
    {
        if (!bSeedDefaultOps)
        {
            Ctx.SendError(ErrorCodes::ERR_RETARGETER_NO_OPS, FString::Printf(
                TEXT("IK Retargeter '%s' has no retarget ops, so the export would copy nothing. Retry with seedDefaultOps:true to add the engine's default op stack (UIKRetargeterController::AddDefaultOps)."),
                *RetargeterPath));
            return true;
        }
        Controller->AddDefaultOps();
        Retargeter->MarkPackageDirty();
        bSeededDefaultOps = Controller->GetNumRetargetOps() > 0;
    }
#else
    (void)bSeedDefaultOps;
#endif

    TArray<FString> UnmappedTargetChains;
    for (const FBoneChain& Chain : TargetRig->GetRetargetChains())
    {
        if (Controller->GetSourceChain(Chain.ChainName).IsNone())
        {
            UnmappedTargetChains.Add(Chain.ChainName.ToString());
        }
    }
    if (bRequireCompleteMapping && UnmappedTargetChains.Num() > 0)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetArrayField(TEXT("unmappedTargetChains"), ToJsonStrings(UnmappedTargetChains));
        Data->SetBoolField(TEXT("seededDefaultOps"), bSeededDefaultOps);
        Ctx.SendError(ErrorCodes::ERR_RETARGET_CHAINS_UNMAPPED, FString::Printf(
            TEXT("Target chains with no source chain: %s. Map them (animation.authoring.set_retarget_chain_mapping) or pass requireCompleteMapping:false to leave them at the retarget pose."),
            *FString::Join(UnmappedTargetChains, TEXT(", "))), Data);
        return true;
    }

    // Refuse existing outputs: the engine would silently pick a numbered name instead.
    const TSet<FString> Before = AssetsInFolder(OutputPath);
    TArray<FString> ExpectedOutputs;
    TArray<FString> Collisions;
    for (const UAnimSequence* Source : Sources)
    {
        const FString Name = Prefix + Source->GetName() + Suffix;
        const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *OutputPath, *Name, *Name);
        if (Before.Contains(ObjectPath) || ExpectedOutputs.Contains(ObjectPath))
        {
            Collisions.Add(ObjectPath);
        }
        ExpectedOutputs.Add(ObjectPath);
    }
    if (Collisions.Num() > 0)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetArrayField(TEXT("existingOutputs"), ToJsonStrings(Collisions));
        Ctx.SendError(ErrorCodes::ERR_ASSET_EXISTS, FString::Printf(
            TEXT("Output already exists or two inputs share a name: %s. Choose another outputPath/prefix/suffix or delete them first."),
            *FString::Join(Collisions, TEXT(", "))), Data);
        return true;
    }

    FIKRetargetBatchOperationContext Context;
    for (UAnimSequence* Source : Sources)
    {
        Context.AssetsToRetarget.Add(Source);
    }
    Context.SourceMesh = SourceMesh;
    Context.TargetMesh = TargetMesh;
    Context.IKRetargetAsset = Retargeter;
    Context.NameRule.Prefix = Prefix;
    Context.NameRule.Suffix = Suffix;
    Context.NameRule.FolderPath = OutputPath;

    UIKRetargetBatchOperation* BatchOperation = NewObject<UIKRetargetBatchOperation>();
    BatchOperation->AddToRoot();
    BatchOperation->RunRetarget(Context);
    BatchOperation->RemoveFromRoot();

    // What the export created, measured. Includes any referenced asset the engine duplicated too.
    TArray<FString> Created = AssetsInFolder(OutputPath).Difference(Before).Array();
    Created.Sort();

    TArray<TSharedPtr<FJsonValue>> CreatedJson;
    TArray<FString> Failures;
    TArray<UObject*> CreatedObjects;
    for (const FString& Path : Created)
    {
        if (UObject* Object = FindObject<UObject>(nullptr, *Path))
        {
            CreatedObjects.Add(Object);
        }
    }
    for (int32 Index = 0; Index < Sources.Num(); ++Index)
    {
        const FString& Expected = ExpectedOutputs[Index];
        UAnimSequence* Output = Created.Contains(Expected)
            ? FindObject<UAnimSequence>(nullptr, *Expected) : nullptr;
        if (!Output)
        {
            Failures.Add(FString::Printf(TEXT("%s: no output at %s"), *Sources[Index]->GetPathName(), *Expected));
            continue;
        }
        const IAnimationDataModel* SourceModel = Sources[Index]->GetDataModel();
        const IAnimationDataModel* OutputModel = Output->GetDataModel();
        const int32 SourceFrames = SourceModel ? SourceModel->GetNumberOfFrames() : -1;
        const int32 Frames = OutputModel ? OutputModel->GetNumberOfFrames() : -1;
        const int32 BoneTracks = OutputModel ? OutputModel->GetNumBoneTracks() : 0;
        const bool bOnTarget = Output->GetSkeleton() == TargetSkeleton;
        bool bVerified = bOnTarget && BoneTracks > 0 && Frames == SourceFrames;
#if WITH_DEV_AUTOMATION_TESTS
        if (Index == AnimationHandlerTestHooks::RetargetVerifyFailureIndex())
        {
            bVerified = false;
        }
#endif
        if (!bVerified)
        {
            Failures.Add(FString::Printf(TEXT("%s: skeleton %s (want %s), frames %d (source %d), boneTracks %d"),
                *Expected, Output->GetSkeleton() ? *Output->GetSkeleton()->GetPathName() : TEXT("none"),
                *TargetSkeleton->GetPathName(), Frames, SourceFrames, BoneTracks));
            continue;
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("path"), Expected);
        Row->SetStringField(TEXT("source"), Sources[Index]->GetPathName());
        Row->SetStringField(TEXT("skeleton"), Output->GetSkeleton()->GetPathName());
        Row->SetNumberField(TEXT("frames"), Frames);
        Row->SetNumberField(TEXT("boneTracks"), BoneTracks);
        CreatedJson.Add(MakeShared<FJsonValueObject>(Row));
    }

    if (Failures.Num() > 0)
    {
        // Partial result: delete everything this call created, then measure what is left.
        if (CreatedObjects.Num() > 0)
        {
            ObjectTools::ForceDeleteObjects(CreatedObjects, /*ShowConfirmation=*/false);
        }
        const TSet<FString> After = AssetsInFolder(OutputPath);
        TArray<FString> Removed, Remaining;
        for (const FString& Path : Created)
        {
            (After.Contains(Path) ? Remaining : Removed).Add(Path);
        }
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetArrayField(TEXT("failures"), ToJsonStrings(Failures));
        Data->SetArrayField(TEXT("removedOutputs"), ToJsonStrings(Removed));
        Data->SetArrayField(TEXT("outputsRemaining"), ToJsonStrings(Remaining));
        Data->SetBoolField(TEXT("seededDefaultOps"), bSeededDefaultOps);
        Ctx.SendError(ErrorCodes::ERR_RETARGET_INCOMPLETE, FString::Printf(
            TEXT("%d of %d outputs failed verification; deleted %d created output(s), %d remain. First failure: %s"),
            Failures.Num(), Sources.Num(), Removed.Num(), Remaining.Num(), *Failures[0]), Data);
        return true;
    }

    bool bAllSaved = true;
    TArray<FString> Unsaved;
    for (UObject* Object : CreatedObjects)
    {
        if (!SaveAssetToDiskReportingPresence(Object, /*bForce=*/true))
        {
            bAllSaved = false;
            Unsaved.Add(Object->GetPathName());
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("created"), CreatedJson);
    Result->SetArrayField(TEXT("allCreatedAssets"), ToJsonStrings(Created));
    Result->SetStringField(TEXT("targetSkeleton"), TargetSkeleton->GetPathName());
    Result->SetBoolField(TEXT("mappingComplete"), UnmappedTargetChains.Num() == 0);
    Result->SetArrayField(TEXT("unmappedTargetChains"), ToJsonStrings(UnmappedTargetChains));
    Result->SetBoolField(TEXT("seededDefaultOps"), bSeededDefaultOps);
    if (Unsaved.Num() > 0)
    {
        Result->SetArrayField(TEXT("unsavedAssets"), ToJsonStrings(Unsaved));
    }
    AddAssetSaveReport(Result, /*bSaveRequested=*/true, bAllSaved);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(ErrorCodes::ERR_NOT_SUPPORTED,
        TEXT("animation.retarget_animations needs the IKRig / IKRigEditor modules, which this build does not link."));
#endif
    return true;
}
