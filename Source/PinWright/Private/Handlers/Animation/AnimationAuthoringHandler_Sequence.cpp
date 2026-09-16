// Copyright (c) 2026 Alexander Penkin. MIT License.

// AnimationAuthoringHandler_Sequence.cpp - split from AnimationAuthoringHandler.cpp
//
// Sequence + Montage/Composite authoring RPCs plus the get_animation_info
// reader, under the animation.authoring namespace. Cross-cluster helpers live
// in AnimationAuthoringHelpers.{h,cpp}.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/PackagePathCompose.h"
#include "Dom/JsonValue.h"
#include "PinWrightHelpers.h"
#include "PinWrightGlobals.h"
#include "PinWrightSubsystem.h"
#include "Handlers/Asset/AnimSequenceDumpBuilder.h"
#include "Handlers/Asset/BlendSpaceDumpBuilder.h"
#include "Handlers/Asset/AnimMontageDumpBuilder.h"
#include "Handlers/Animation/AnimGraphDumpBuilder.h"
#include "Utils/JsonBuilders.h"
#include "Handlers/Blueprint/BlueprintEnumHelpers.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Compat/EngineVersionCompat.h"
#include "Utils/AssetUtils.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/Skeleton.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Engine/SkeletalMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Factories/AnimSequenceFactory.h"
#include "Factories/AnimCompositeFactory.h"
#include "Factories/AnimMontageFactory.h"
#include "Factories/AnimBlueprintFactory.h"
#include "EditorAssetLibrary.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Utils/MovieSceneJsonUtils.h"

// Blend Space factories
#if __has_include("Factories/BlendSpaceFactoryNew.h") && __has_include("Factories/BlendSpaceFactory1D.h")
#include "Factories/BlendSpaceFactoryNew.h"
#include "Factories/BlendSpaceFactory1D.h"
#define MCP_HAS_BLENDSPACE_FACTORY 1
#else
#define MCP_HAS_BLENDSPACE_FACTORY 0
#endif

// Control Rig support (optional module)
#if __has_include("ControlRig.h")
#include "ControlRig.h"
#define MCP_HAS_CONTROLRIG 1
#else
#define MCP_HAS_CONTROLRIG 0
#endif

#if __has_include("ControlRigBlueprint.h") || __has_include("ControlRigBlueprintLegacy.h")
#include "Utils/ControlRigBlueprintCompat.h"
#define MCP_HAS_CONTROLRIG_BLUEPRINT 1
#else
#define MCP_HAS_CONTROLRIG_BLUEPRINT 0
#endif

// RigVM Blueprint Generated Class (needed for ControlRig creation fallback in UE 5.1-5.4)
#if __has_include("RigVMBlueprintGeneratedClass.h")
#include "RigVMBlueprintGeneratedClass.h"
#endif

// Control Rig Factory (for creating Control Rig assets)
// Note: ControlRigBlueprintFactory header is Public only in UE 5.5+
// For UE 5.4 we use a fallback implementation
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
  #include "ControlRigBlueprintFactory.h"
#endif

// IK Rig support (UE 5.0+). Cross-module consumers must include the public header as
// "Rig/IKRigDefinition.h" (the bare path only resolves intra-module). Keep this guard
// identical to AnimationAuthoringHandler_AnimBlueprint.cpp so the macro resolves to the
// same value across TUs under the unity build (a divergent value would be a redefinition).
#if __has_include("Rig/IKRigDefinition.h")
#include "Rig/IKRigDefinition.h"
#define MCP_HAS_IKRIG 1
#elif __has_include("IKRigDefinition.h")
#include "IKRigDefinition.h"
#define MCP_HAS_IKRIG 1
#else
#define MCP_HAS_IKRIG 0
#endif

// IK Rig Factory (for creating IK Rig assets)
#if __has_include("RigEditor/IKRigDefinitionFactory.h")
#include "RigEditor/IKRigDefinitionFactory.h"
#define MCP_HAS_IKRIG_FACTORY 1
#elif __has_include("IKRigDefinitionFactory.h")
#include "IKRigDefinitionFactory.h"
#define MCP_HAS_IKRIG_FACTORY 1
#else
#define MCP_HAS_IKRIG_FACTORY 0
#endif

// IK Retarget Factory
#if __has_include("RetargetEditor/IKRetargetFactory.h")
#include "RetargetEditor/IKRetargetFactory.h"
#define MCP_HAS_IKRETARGET_FACTORY 1
#elif __has_include("IKRetargetFactory.h")
#include "IKRetargetFactory.h"
#define MCP_HAS_IKRETARGET_FACTORY 1
#else
#define MCP_HAS_IKRETARGET_FACTORY 0
#endif

#if __has_include("Retargeter/IKRetargeter.h")
#include "Retargeter/IKRetargeter.h"
#define MCP_HAS_IKRETARGETER 1
#elif __has_include("IKRetargeter.h")
#include "IKRetargeter.h"
#define MCP_HAS_IKRETARGETER 1
#else
#define MCP_HAS_IKRETARGETER 0
#endif

// Pose Asset
#if __has_include("Animation/PoseAsset.h")
#include "Animation/PoseAsset.h"
#define MCP_HAS_POSEASSET 1
#else
#define MCP_HAS_POSEASSET 0
#endif

// Animation Blueprint Graph
#if __has_include("AnimationGraph.h")
#include "AnimationGraph.h"
#endif
#if __has_include("AnimGraphNode_Base.h")
#include "AnimGraphNode_Base.h"
#endif
#if __has_include("AnimGraphNode_StateMachine.h")
#include "AnimGraphNode_StateMachine.h"
#endif
#if __has_include("AnimGraphNode_TransitionResult.h")
#include "AnimGraphNode_TransitionResult.h"
#endif
#if __has_include("AnimStateNode.h")
#include "AnimStateNode.h"
#endif

// Additional AnimGraph node types for state machine implementation
#if __has_include("AnimStateTransitionNode.h")
#include "AnimStateTransitionNode.h"
#define MCP_HAS_ANIM_STATE_TRANSITION 1
#else
#define MCP_HAS_ANIM_STATE_TRANSITION 0
#endif

#if __has_include("AnimStateEntryNode.h")
#include "AnimStateEntryNode.h"
#endif

#if __has_include("AnimStateAliasNode.h")
#include "AnimStateAliasNode.h"
#endif

#include "Curves/CurveFloat.h"
#include "Animation/AnimStateMachineTypes.h"

#if __has_include("AnimationStateMachineGraph.h")
#include "AnimationStateMachineGraph.h"
#define MCP_HAS_ANIM_STATE_MACHINE_GRAPH 1
#else
#define MCP_HAS_ANIM_STATE_MACHINE_GRAPH 0
#endif

#if __has_include("AnimationStateMachineSchema.h")
#include "AnimationStateMachineSchema.h"
#define MCP_HAS_ANIM_STATE_MACHINE_SCHEMA 1
#else
#define MCP_HAS_ANIM_STATE_MACHINE_SCHEMA 0
#endif

// Blend node types
#if __has_include("AnimGraphNode_TwoWayBlend.h")
#include "AnimGraphNode_TwoWayBlend.h"
#define MCP_HAS_TWO_WAY_BLEND 1
#else
#define MCP_HAS_TWO_WAY_BLEND 0
#endif

#if __has_include("AnimGraphNode_LayeredBoneBlend.h")
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimNodes/AnimNode_LayeredBoneBlend.h"
#include "Animation/AnimData/BoneMaskFilter.h"
#include "Animation/BlendProfile.h"
#define MCP_HAS_LAYERED_BLEND 1
#else
#define MCP_HAS_LAYERED_BLEND 0
#endif

#if __has_include("AnimGraphNode_TwoBoneIK.h") && __has_include("BoneControllers/AnimNode_TwoBoneIK.h")
#include "AnimGraphNode_TwoBoneIK.h"
#include "BoneControllers/AnimNode_TwoBoneIK.h"
#define MCP_HAS_TWO_BONE_IK 1
#else
#define MCP_HAS_TWO_BONE_IK 0
#endif

#if __has_include("AnimGraphNode_ModifyBone.h") && __has_include("BoneControllers/AnimNode_ModifyBone.h")
#include "AnimGraphNode_ModifyBone.h"
#include "BoneControllers/AnimNode_ModifyBone.h"
#define MCP_HAS_MODIFY_BONE 1
#else
#define MCP_HAS_MODIFY_BONE 0
#endif

#if __has_include("AnimGraphNode_SaveCachedPose.h")
#include "AnimGraphNode_SaveCachedPose.h"
#define MCP_HAS_CACHED_POSE 1
#else
#define MCP_HAS_CACHED_POSE 0
#endif

#if __has_include("AnimGraphNode_Slot.h")
#include "AnimGraphNode_Slot.h"
#define MCP_HAS_SLOT_NODE 1
#else
#define MCP_HAS_SLOT_NODE 0
#endif

// Asset-player base class (covers Sequence/BlendSpace/PoseHandler/AimOffset)
#if __has_include("AnimGraphNode_AssetPlayerBase.h")
#include "AnimGraphNode_AssetPlayerBase.h"
#include "Animation/AnimNode_AssetPlayerBase.h"
#define MCP_HAS_ASSET_PLAYER_BASE 1
#else
#define MCP_HAS_ASSET_PLAYER_BASE 0
#endif

#include "Animation/AnimationAsset.h"
#include "UObject/UnrealType.h"

// Shared anim-graph construction helpers.
#include "AnimGraphConstructionUtils.h"
#include "Utils/ClassUtils.h"

#include "AnimationAuthoringHelpers.h"
#include "Handlers/Animation/AnimSequenceCreate.h"

namespace PwAnimSequenceAuthoringJson
{
    const TArray<TSharedPtr<FJsonValue>>* FindKeyArray(
        const FHandlerContext& Ctx,
        const TCHAR* Canonical,
        const TCHAR* AliasOne,
        const TCHAR* AliasTwo)
    {
        if (const TArray<TSharedPtr<FJsonValue>>* Values = Ctx.GetArray(Canonical))
        {
            return Values;
        }
        if (AliasOne)
        {
            if (const TArray<TSharedPtr<FJsonValue>>* Values = Ctx.GetArray(AliasOne))
            {
                return Values;
            }
        }
        return AliasTwo ? Ctx.GetArray(AliasTwo) : nullptr;
    }

    bool ReadComponents(
        const TSharedPtr<FJsonValue>& Value,
        int32 ComponentCount,
        double* OutComponents,
        int32 KeyIndex,
        const TCHAR* FieldName,
        FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
        if (!Value.IsValid() || !Value->TryGetArray(Components) ||
            !Components || Components->Num() != ComponentCount)
        {
            OutError = FString::Printf(
                TEXT("%s[%d] must be an array of %d numbers"),
                FieldName, KeyIndex, ComponentCount);
            return false;
        }

        for (int32 ComponentIndex = 0; ComponentIndex < ComponentCount; ++ComponentIndex)
        {
            if (!(*Components)[ComponentIndex].IsValid() ||
                !(*Components)[ComponentIndex]->TryGetNumber(OutComponents[ComponentIndex]))
            {
                OutError = FString::Printf(
                    TEXT("%s[%d][%d] must be a number"),
                    FieldName, KeyIndex, ComponentIndex);
                return false;
            }
        }
        return true;
    }

    bool ReadVector3Keys(
        const TArray<TSharedPtr<FJsonValue>>* Values,
        const TCHAR* FieldName,
        TArray<FVector3f>& OutKeys,
        FString& OutError)
    {
        if (!Values)
        {
            OutError = FString::Printf(TEXT("%s array is required"), FieldName);
            return false;
        }

        OutKeys.Reset(Values->Num());
        for (int32 KeyIndex = 0; KeyIndex < Values->Num(); ++KeyIndex)
        {
            double Components[3] = {};
            if (!ReadComponents((*Values)[KeyIndex], 3, Components, KeyIndex, FieldName, OutError))
            {
                return false;
            }
            OutKeys.Add(FVector3f(
                static_cast<float>(Components[0]),
                static_cast<float>(Components[1]),
                static_cast<float>(Components[2])));
        }
        return true;
    }

    bool ReadQuaternionKeys(
        const TArray<TSharedPtr<FJsonValue>>* Values,
        const TCHAR* FieldName,
        TArray<FQuat4f>& OutKeys,
        FString& OutError)
    {
        if (!Values)
        {
            OutError = FString::Printf(TEXT("%s array is required"), FieldName);
            return false;
        }

        OutKeys.Reset(Values->Num());
        for (int32 KeyIndex = 0; KeyIndex < Values->Num(); ++KeyIndex)
        {
            double Components[4] = {};
            if (!ReadComponents((*Values)[KeyIndex], 4, Components, KeyIndex, FieldName, OutError))
            {
                return false;
            }
            OutKeys.Add(FQuat4f(
                static_cast<float>(Components[0]),
                static_cast<float>(Components[1]),
                static_cast<float>(Components[2]),
                static_cast<float>(Components[3])));
        }
        return true;
    }

    bool ReadSyncMarkerSpecs(
        const TArray<TSharedPtr<FJsonValue>>* Values,
        const UAnimSequence* Sequence,
        TArray<FPwSyncMarkerSpec>& OutMarkers,
        FString& OutError)
    {
        if (!Values)
        {
            OutError = TEXT("markers array is required");
            return false;
        }

        const IAnimationDataModel* Model = Sequence ? Sequence->GetDataModel() : nullptr;
        const int32 MaxFrame = Model ? Model->GetNumberOfFrames() : INDEX_NONE;
        const FFrameRate FrameRate = Sequence ? Sequence->GetSamplingFrameRate() : FFrameRate();
        if (!Model || FrameRate.Numerator <= 0 || FrameRate.Denominator <= 0)
        {
            OutError = TEXT("Animation sequence has no valid frame range");
            return false;
        }

        OutMarkers.Reset(Values->Num());
        for (int32 Index = 0; Index < Values->Num(); ++Index)
        {
            const TSharedPtr<FJsonObject> MarkerObject =
                (*Values)[Index].IsValid() ? (*Values)[Index]->AsObject() : nullptr;
            if (!MarkerObject.IsValid())
            {
                OutError = FString::Printf(TEXT("markers[%d] must be an object"), Index);
                return false;
            }

            FString MarkerName;
            if (!MarkerObject->TryGetStringField(TEXT("name"), MarkerName) || MarkerName.IsEmpty())
            {
                OutError = FString::Printf(
                    TEXT("markers[%d].name must be a non-empty string"), Index);
                return false;
            }

            double FrameNumber = 0.0;
            if (!MarkerObject->HasField(TEXT("frame"))
                || !MarkerObject->TryGetNumberField(TEXT("frame"), FrameNumber)
                || !FMath::IsFinite(FrameNumber)
                || !FMath::IsNearlyEqual(FrameNumber, FMath::RoundToDouble(FrameNumber)))
            {
                OutError = FString::Printf(
                    TEXT("markers[%d].frame must be an integer"), Index);
                return false;
            }

            const int32 Frame = FMath::RoundToInt(FrameNumber);
            if (Frame < 0 || Frame > MaxFrame)
            {
                OutError = FString::Printf(
                    TEXT("markers[%d].frame must be in 0 through %d, but found %d"),
                    Index, MaxFrame, Frame);
                return false;
            }

            FPwSyncMarkerSpec& Spec = OutMarkers.AddDefaulted_GetRef();
            Spec.MarkerName = FName(*MarkerName);
            Spec.Time = static_cast<float>(
                static_cast<double>(Frame) * FrameRate.Denominator
                / static_cast<double>(FrameRate.Numerator));
        }
        return true;
    }
}

// ===========================================================================
// 10.1 ANIMATION SEQUENCES
// ===========================================================================

REGISTER_RPC_HANDLER("animation.authoring.create_animation_sequence", "animation.authoring",
    "Create an empty UAnimSequence asset bound to a Skeleton with caller-specified length and frame rate. Add bone tracks via animation.authoring.add_bone_track after creation.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset filename (no extension) for the new sequence."),
        RPC_PARAM_OPT("path", "path", "Content-browser folder; defaults to /Game/Animations."),
        RPC_PARAM_REQ("skeletonPath", "path", "Asset path to the Skeleton this sequence will animate."),
        RPC_PARAM_OPT("numFrames", "number", "Total frame count for the sequence; defaults to 30."),
        RPC_PARAM_OPT("frameRate", "integer", "Sample rate in fps; defaults to 30. Sequence length = numFrames / frameRate seconds."),
        RPC_PARAM_OPT("save", "boolean", "When true (default) the new asset is marked dirty so the next editor save persists it.")
    ))
{
    const TSharedPtr<FJsonObject>& Params = Ctx.GetRawPayload();
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Animations")));
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    int32 NumFrames = Ctx.GetInt(TEXT("numFrames"), 30);
    int32 FrameRate = Ctx.GetInt(TEXT("frameRate"), 30);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (Name.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_NAME, TEXT("Name is required"));
        return true;
    }

    // The destination is composed and checked HERE, above the skeleton load, not at the
    // CreatePackage call below. `name` used to be concatenated onto the folder and handed straight
    // to CreatePackage, and CreatePackage logs at Fatal (UObjectGlobals.cpp:1094-1096) for a name
    // containing "//" - a verbosity compiled out in no configuration, so it ends the editor PROCESS
    // and every unsaved package in it rather than failing the call (measured on board
    // B-foliage-add-type-name-with-slash-kills-the-editor). The ORDERING is load-bearing for the
    // regression test: it pairs a bad `name` with a well-formed skeletonPath naming no asset, so a
    // build without this check is refused SKELETON_NOT_FOUND above the concatenation and goes red
    // on the wrong code instead of taking the test host down with it. Do not move it below the
    // skeleton load.
    FString PackagePath;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(Path, Name, PackagePath, PathError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("%s Pass a bare asset name and choose the folder with path "
                                 "(default /Game/Animations)."), *PathError));
        return true;
    }

    USkeleton* Skeleton = AnimationAuthoringHelpers::LoadSkeletonFromPathAnim(SkeletonPath);
    if (!Skeleton)
    {
        Ctx.SendError(ErrorCodes::ERR_SKELETON_NOT_FOUND, FString::Printf(TEXT("Could not load skeleton: %s"), *SkeletonPath));
        return true;
    }

    // Create package and asset directly to avoid UI dialogs
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    UAnimSequenceFactory* Factory = NewObject<UAnimSequenceFactory>();
    Factory->TargetSkeleton = Skeleton;
    UAnimSequence* NewSequence = Cast<UAnimSequence>(
        Factory->FactoryCreateNew(UAnimSequence::StaticClass(), Package,
                                  FName(*Name), RF_Public | RF_Standalone,
                                  nullptr, GWarn));
    if (!NewSequence)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create animation sequence"));
        return true;
    }

    // Set sequence length
    float Duration = static_cast<float>(NumFrames) / static_cast<float>(FrameRate);

    // UE 5.1+: Use SetNumberOfFrames with FFrameNumber
    NewSequence->GetController().SetFrameRate(FFrameRate(FrameRate, 1));
    NewSequence->GetController().SetNumberOfFrames(FFrameNumber(NumFrames));

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(NewSequence, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), Path / Name);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Animation sequence '%s' created"), *Name));
    AddAssetVerification(Result, NewSequence);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_sequence_length", "animation.authoring",
    "Resize an existing AnimSequence by setting frame count and frame rate. Existing keyframes outside the new range are dropped; total duration becomes numFrames/frameRate seconds.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path to the AnimSequence to resize."),
        RPC_PARAM_OPT("numFrames", "number", "New total frame count; defaults to 30."),
        RPC_PARAM_OPT("frameRate", "integer", "New sample rate in fps; defaults to 30."),
        RPC_PARAM_OPT("save", "boolean", "When true (default) marks the asset dirty so the next editor save persists the change.")
    ))
{
    const TSharedPtr<FJsonObject>& Params = Ctx.GetRawPayload();
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    int32 NumFrames = Ctx.GetInt(TEXT("numFrames"), 30);
    int32 FrameRate = Ctx.GetInt(TEXT("frameRate"), 30);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAnimSequence* Sequence = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(AssetPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, FString::Printf(TEXT("Could not load animation sequence: %s"), *AssetPath));
        return true;
    }

    float Duration = static_cast<float>(NumFrames) / static_cast<float>(FrameRate);

    // UE 5.1+: Use SetNumberOfFrames with FFrameNumber
    Sequence->GetController().SetFrameRate(FFrameRate(FrameRate, 1));
    Sequence->GetController().SetNumberOfFrames(FFrameNumber(NumFrames));
    if (Params->HasField(TEXT("frameRate")))
    {
        // Frame rate already set above
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Sequence, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), TEXT("Sequence length updated"));
    AddAssetVerification(Result, Sequence);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_bone_track", "animation.authoring",
    "Add a bone track to an animation sequence",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the animation sequence"),
        RPC_PARAM_REQ("boneName", "string", "Name of the bone"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString BoneName = Ctx.GetString(TEXT("boneName"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (BoneName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_BONE_NAME, TEXT("boneName is required"));
        return true;
    }

    UAnimSequence* Sequence = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(AssetPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, FString::Printf(TEXT("Could not load animation sequence: %s"), *AssetPath));
        return true;
    }

    // UE 5.1+ uses IAnimationDataController with IsValidBoneTrackName and AddBoneCurve.
    IAnimationDataController& Controller = Sequence->GetController();
    FName BoneFName(*BoneName);
    const bool bAlreadyExisted = Controller.GetModel()->IsValidBoneTrackName(BoneFName);
    const bool bCreated = !bAlreadyExisted;

    int32 KeyCount = 0;
    if (bAlreadyExisted)
    {
        if (const IAnimationDataModel* Model = Controller.GetModel())
        {
PRAGMA_DISABLE_DEPRECATION_WARNINGS
            const FBoneAnimationTrack& Track = Model->GetBoneTrackByName(BoneFName);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
            KeyCount = Track.InternalTrackData.PosKeys.Num();
        }
    }

    // AddBoneCurve creates no keys.  Seed a new or previously empty track from the reference
    // pose so this legacy verb cannot report success while leaving a bone that evaluates as
    // identity.  Populated tracks are read-only here; set_bone_track_keys owns authored edits.
    if (bCreated || KeyCount == 0)
    {
        USkeleton* Skeleton = Sequence->GetSkeleton();
        const int32 BoneIndex = Skeleton
            ? Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneFName)
            : INDEX_NONE;
        if (BoneIndex == INDEX_NONE)
        {
            Ctx.SendError(ErrorCodes::ERR_BONE_NOT_FOUND,
                FString::Printf(TEXT("Bone '%s' is not present in the sequence skeleton"), *BoneName));
            return true;
        }

        const IAnimationDataModel* Model = Controller.GetModel();
        const int32 NumberOfKeys = Model ? Model->GetNumberOfFrames() + 1 : 0;
        if (NumberOfKeys <= 0)
        {
            Ctx.SendError(ErrorCodes::ERR_ANIMATION_INVALID,
                TEXT("The sequence has no positive NumberOfFrames + 1 key count"));
            return true;
        }

        const FTransform& RefPose = Skeleton->GetReferenceSkeleton().GetRefBonePose()[BoneIndex];
        const FVector Translation = RefPose.GetTranslation();
        const FQuat Rotation = RefPose.GetRotation();
        const FVector Scale = RefPose.GetScale3D();
        TArray<FVector3f> Pos;
        TArray<FQuat4f> Rot;
        TArray<FVector3f> ScaleKeys;
        Pos.Init(FVector3f(Translation.X, Translation.Y, Translation.Z), NumberOfKeys);
        Rot.Init(FQuat4f(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W), NumberOfKeys);
        ScaleKeys.Init(FVector3f(Scale.X, Scale.Y, Scale.Z), NumberOfKeys);

        {
#if WITH_EDITOR
            IAnimationDataController::FScopedBracket Bracket(
                Controller, FText::FromString(TEXT("Seed animation bone track")), false);
#endif
            if (bCreated && !Controller.AddBoneCurve(BoneFName, false))
            {
                Ctx.SendError(ErrorCodes::ERR_TRACK_CREATION_FAILED,
                    FString::Printf(TEXT("Could not add a track for bone '%s'"), *BoneName));
                return true;
            }
            if (!Controller.SetBoneTrackKeys(BoneFName, Pos, Rot, ScaleKeys, false))
            {
                Ctx.SendError(ErrorCodes::ERR_TRACK_OP_FAILED,
                    FString::Printf(TEXT("Could not seed the reference-pose track for bone '%s'"), *BoneName));
                return true;
            }
        }

    }

    // Report the track as stored, not the requested reference-pose array length.
    if (const IAnimationDataModel* Model = Controller.GetModel())
    {
PRAGMA_DISABLE_DEPRECATION_WARNINGS
        const FBoneAnimationTrack& Track = Model->GetBoneTrackByName(BoneFName);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
        KeyCount = Track.InternalTrackData.PosKeys.Num();
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Sequence, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetBoolField(TEXT("created"), bCreated);
    Result->SetBoolField(TEXT("alreadyExisted"), bAlreadyExisted);
    Result->SetNumberField(TEXT("keyCount"), KeyCount);
    Result->SetStringField(TEXT("message"), FString::Printf(
        TEXT("Bone track '%s' %s"), *BoneName,
        bCreated ? TEXT("created") : TEXT("already existed")));
    AddAssetVerification(Result, Sequence);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_bone_track_keys", "animation.authoring",
    "Write dense local-space position, quaternion rotation, and scale keys to one AnimSequence bone track. The arrays must contain NumberOfFrames + 1 entries.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the AnimSequence."),
        RPC_PARAM_REQ("boneName", "string", "Skeleton bone name."),
        RPC_PARAM_REQ("positions", "array", "Per-key [x, y, z] local positions. Aliases: posKeys, pos."),
        RPC_PARAM_REQ("rotations", "array", "Per-key [x, y, z, w] local quaternions. Aliases: rotKeys, rot."),
        RPC_PARAM_REQ("scales", "array", "Per-key [x, y, z] local scales. Aliases: scaleKeys, scale."),
        RPC_PARAM_OPT("save", "boolean", "Write the package to disk when true (default true).")
    ))
{
    FString AssetPath;
    FString BoneName;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath) ||
        !Ctx.RequireString(TEXT("boneName"), BoneName))
    {
        return true;
    }

    AssetPath = NormalizeContentAssetPath(AssetPath);
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_PATH, TEXT("assetPath is required"));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* Positions =
        PwAnimSequenceAuthoringJson::FindKeyArray(
            Ctx, TEXT("positions"), TEXT("posKeys"), TEXT("pos"));
    const TArray<TSharedPtr<FJsonValue>>* Rotations =
        PwAnimSequenceAuthoringJson::FindKeyArray(
            Ctx, TEXT("rotations"), TEXT("rotKeys"), TEXT("rot"));
    const TArray<TSharedPtr<FJsonValue>>* Scales =
        PwAnimSequenceAuthoringJson::FindKeyArray(
            Ctx, TEXT("scales"), TEXT("scaleKeys"), TEXT("scale"));

    FPwBoneTrackSpec Track;
    Track.BoneName = FName(*BoneName);
    FString ParseError;
    if (!PwAnimSequenceAuthoringJson::ReadVector3Keys(
            Positions, TEXT("positions"), Track.Pos, ParseError) ||
        !PwAnimSequenceAuthoringJson::ReadQuaternionKeys(
            Rotations, TEXT("rotations"), Track.Rot, ParseError) ||
        !PwAnimSequenceAuthoringJson::ReadVector3Keys(
            Scales, TEXT("scales"), Track.Scale, ParseError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, ParseError);
        return true;
    }

    UAnimSequence* Sequence = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(AssetPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_ANIMATION_NOT_FOUND,
            FString::Printf(TEXT("Could not load animation sequence: %s"), *AssetPath));
        return true;
    }

    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    const TArrayView<const FPwBoneTrackSpec> TrackView(&Track, 1);
    const FAnimSequenceWriteResult WriteResult = WriteBoneTracks(Sequence, TrackView, bSave);
    if (!WriteResult.bSuccess)
    {
        Ctx.SendError(
            WriteResult.ErrorCode.IsEmpty() ? FString(ErrorCodes::ERR_ASSET_DATA_INVALID)
                                             : WriteResult.ErrorCode,
            WriteResult.ErrorMessage);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("assetPath"), WriteResult.AssetPath);
    Result->SetStringField(TEXT("boneName"), BoneName);
    Result->SetNumberField(TEXT("assetNumberOfKeys"), WriteResult.AssetNumberOfKeys);
    Result->SetNumberField(TEXT("trackKeyCount"), WriteResult.TrackKeyCount);
    Result->SetNumberField(TEXT("assetTrackCount"), WriteResult.AssetTrackCount);

    TArray<TSharedPtr<FJsonValue>> KeyCounts;
    KeyCounts.Reserve(WriteResult.AssetKeyCountPerTrack.Num());
    for (const int32 KeyCount : WriteResult.AssetKeyCountPerTrack)
    {
        KeyCounts.Add(MakeShared<FJsonValueNumber>(static_cast<double>(KeyCount)));
    }
    Result->SetArrayField(TEXT("assetKeyCountPerTrack"), MoveTemp(KeyCounts));
    AddAssetVerification(Result, Sequence);
    AddAssetSaveReport(
        Result, bSave, IsAssetSaveStateDurable(WriteResult.SaveState), WriteResult.SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_curve_key", "animation.authoring",
    "Set a curve key value at a specific frame",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the animation sequence"),
        RPC_PARAM_REQ("curveName", "string", "Name of the curve"),
        RPC_PARAM_OPT("frame", "integer", "Frame number (default 0)"),
        RPC_PARAM_OPT("value", "number", "Curve value (default 0)"),
        RPC_PARAM_OPT("createIfMissing", "boolean", "Create curve if missing (default true)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString CurveName = Ctx.GetString(TEXT("curveName"));
    int32 Frame = Ctx.GetInt(TEXT("frame"), 0);
    float Value = static_cast<float>(Ctx.GetNumber(TEXT("value"), 0.0));
    bool bCreateIfMissing = Ctx.GetBool(TEXT("createIfMissing"), true);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (CurveName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_CURVE_NAME, TEXT("curveName is required"));
        return true;
    }

    UAnimSequence* Sequence = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(AssetPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, FString::Printf(TEXT("Could not load animation sequence: %s"), *AssetPath));
        return true;
    }

    // UE 5.1+ API - FAnimationCurveIdentifier takes FName directly
    IAnimationDataController& Controller = Sequence->GetController();
    FAnimationCurveIdentifier CurveId(FName(*CurveName), ERawCurveTrackTypes::RCT_Float);

    // Find or create curve
    const FFloatCurve* ExistingCurve = Sequence->GetDataModel()->FindFloatCurve(CurveId);
    if (!ExistingCurve && bCreateIfMissing)
    {
        Controller.AddCurve(CurveId, AACF_DefaultCurve);
    }

    // Set key value
    float FrameTime = static_cast<float>(Frame) / Sequence->GetSamplingFrameRate().AsDecimal();
    Controller.SetCurveKey(CurveId, FRichCurveKey(FrameTime, Value));

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Sequence, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Curve key set at frame %d"), Frame));
    AddAssetVerification(Result, Sequence);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.list_curves", "animation.authoring",
    "List Float and Transform curves on an animation sequence",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the animation sequence")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));

    UAnimSequence* Sequence = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(AssetPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, FString::Printf(TEXT("Could not load animation sequence: %s"), *AssetPath));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetArrayField(TEXT("curves"), AnimSequenceDumpBuilder::BuildCurvesArrayJson(Sequence));
    // Raw bone tracks are not curves and never appear in curves[], so surface them here as
    // {boneName,keyCount} — the per-bone cross-check the inspect-after-mutate loop reaches
    // list_curves for after keying bones.
    Result->SetArrayField(TEXT("boneTracks"), AnimSequenceDumpBuilder::BuildBoneTracksArrayJson(Sequence));
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.list_notifies", "animation.authoring",
    "List anim notifies on an animation sequence or montage",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the animation asset")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));

    UAnimSequenceBase* Asset = Cast<UAnimSequenceBase>(StaticLoadObject(UAnimSequenceBase::StaticClass(), nullptr, *AssetPath));
    if (!Asset)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(TEXT("Could not load animation asset: %s"), *AssetPath));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetArrayField(TEXT("notifies"), AnimSequenceDumpBuilder::BuildNotifiesArrayJson(Asset));
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_notify", "animation.authoring",
    "Add an anim notify to an animation asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the animation asset"),
        RPC_PARAM_OPT("notifyClass", "classref", "Notify class name (default AnimNotify)"),
        RPC_PARAM_OPT("frame", "integer", "Frame number (default 0)"),
        RPC_PARAM_OPT("trackIndex", "integer", "Track index (default 0)"),
        RPC_PARAM_OPT("notifyName", "string", "Optional notify name"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString NotifyClass = Ctx.GetString(TEXT("notifyClass"), TEXT("AnimNotify"));
    int32 Frame = Ctx.GetInt(TEXT("frame"), 0);
    int32 TrackIndex = Ctx.GetInt(TEXT("trackIndex"), 0);
    FString NotifyName = Ctx.GetString(TEXT("notifyName"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAnimSequenceBase* AnimAsset = Cast<UAnimSequenceBase>(StaticLoadObject(UAnimSequenceBase::StaticClass(), nullptr, *AssetPath));
    if (!AnimAsset)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(TEXT("Could not load animation asset: %s"), *AssetPath));
        return true;
    }

    // Find notify class
    FString FullClassName = NotifyClass;
    if (!FullClassName.StartsWith(TEXT("AnimNotify_")))
    {
        FullClassName = TEXT("AnimNotify_") + NotifyClass;
    }

    // Resolve the notify class, but NEVER instantiate an abstract one. The default
    // notifyClass "AnimNotify" (and any unresolved name) would otherwise fall through to the
    // abstract UAnimNotify base: NewObject on an abstract class trips an editor ensure in
    // StaticAllocateObjectErrorTests and the object is nulled out on save. A null class means
    // a valid name-only notify -- the generic notify the default request asks for, matching
    // the non-authoring animation.add_notify path (AnimationHandler.cpp).
    UClass* NotifyUClass = AnimationAuthoringHelpers::ResolveConcreteNotifyClassOrNull(FullClassName);

    // Calculate time from frame
    float FrameRate = 30.0f;
    if (UAnimSequence* Seq = Cast<UAnimSequence>(AnimAsset))
    {
        FrameRate = Seq->GetSamplingFrameRate().AsDecimal();
    }
    float TriggerTime = static_cast<float>(Frame) / FrameRate;

    // Create notify (name-only when no concrete class resolved).
    UAnimNotify* NewNotify = NotifyUClass ? NewObject<UAnimNotify>(AnimAsset, NotifyUClass) : nullptr;
    FAnimNotifyEvent& NotifyEvent = AnimAsset->Notifies.AddDefaulted_GetRef();
    NotifyEvent.Notify = NewNotify;
    AnimationAuthoringHelpers::LinkNotifyAtTime(NotifyEvent, AnimAsset, TriggerTime);
    NotifyEvent.TrackIndex = TrackIndex;

    if (!NotifyName.IsEmpty())
    {
        NotifyEvent.NotifyName = FName(*NotifyName);
    }

    AnimationAuthoringHelpers::EnsureNotifyTrack(AnimAsset, NotifyEvent);
    AnimAsset->RefreshCacheData();

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimAsset, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), TEXT("Notify added"));
    AddAssetVerification(Result, AnimAsset);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_notify_state", "animation.authoring",
    "Add an anim notify state to an animation asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the animation asset"),
        RPC_PARAM_OPT("notifyClass", "classref", "Notify state class name (default AnimNotifyState)"),
        RPC_PARAM_OPT("startFrame", "integer", "Start frame (default 0)"),
        RPC_PARAM_OPT("endFrame", "integer", "End frame (default 10)"),
        RPC_PARAM_OPT("trackIndex", "integer", "Track index (default 0)"),
        RPC_PARAM_OPT("notifyName", "string", "Optional notify name"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString NotifyClass = Ctx.GetString(TEXT("notifyClass"), TEXT("AnimNotifyState"));
    int32 StartFrame = Ctx.GetInt(TEXT("startFrame"), 0);
    int32 EndFrame = Ctx.GetInt(TEXT("endFrame"), 10);
    int32 TrackIndex = Ctx.GetInt(TEXT("trackIndex"), 0);
    FString NotifyName = Ctx.GetString(TEXT("notifyName"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAnimSequenceBase* AnimAsset = Cast<UAnimSequenceBase>(StaticLoadObject(UAnimSequenceBase::StaticClass(), nullptr, *AssetPath));
    if (!AnimAsset)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(TEXT("Could not load animation asset: %s"), *AssetPath));
        return true;
    }

    // Find notify state class
    FString FullClassName = NotifyClass;
    if (!FullClassName.StartsWith(TEXT("AnimNotifyState_")))
    {
        FullClassName = TEXT("AnimNotifyState_") + NotifyClass;
    }

    // A notify state must be backed by a CONCRETE class -- unlike a plain notify there is no
    // valid name-only notify state. The default notifyClass "AnimNotifyState" and any
    // unresolved name resolve to the abstract UAnimNotifyState base; instantiating it would
    // trip an editor ensure and be nulled out on save, so reject instead of corrupting.
    UClass* NotifyStateClass = AnimationAuthoringHelpers::ResolveConcreteNotifyClassOrNull(FullClassName);
    if (!NotifyStateClass)
    {
        Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND,
            FString::Printf(TEXT("Could not resolve a concrete AnimNotifyState class for '%s' (looked up '%s'). Pass a concrete AnimNotifyState subclass, e.g. notifyClass=\"TimedParticleEffect\"."),
                *NotifyClass, *FullClassName));
        return true;
    }

    // Calculate times from frames
    float FrameRate = 30.0f;
    if (UAnimSequence* Seq = Cast<UAnimSequence>(AnimAsset))
    {
        FrameRate = Seq->GetSamplingFrameRate().AsDecimal();
    }
    float StartTime = static_cast<float>(StartFrame) / FrameRate;
    float EndTime = static_cast<float>(EndFrame) / FrameRate;
    float Duration = EndTime - StartTime;

    // Create notify state
    UAnimNotifyState* NewNotifyState = NewObject<UAnimNotifyState>(AnimAsset, NotifyStateClass);
    if (NewNotifyState)
    {
        FAnimNotifyEvent& NotifyEvent = AnimAsset->Notifies.AddDefaulted_GetRef();
        NotifyEvent.NotifyStateClass = NewNotifyState;
        NotifyEvent.TriggerTimeOffset = StartTime;
        NotifyEvent.SetDuration(Duration);
        NotifyEvent.TrackIndex = TrackIndex;

        if (!NotifyName.IsEmpty())
        {
            NotifyEvent.NotifyName = FName(*NotifyName);
        }

        AnimationAuthoringHelpers::EnsureNotifyTrack(AnimAsset, NotifyEvent);
        AnimAsset->RefreshCacheData();
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimAsset, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), TEXT("Notify state added"));
    AddAssetVerification(Result, AnimAsset);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_notify_state_property", "animation.authoring",
    "Set a reflected property value on an anim notify-state instance",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the animation asset"),
        RPC_PARAM_OPT("notifyIndex", "integer", "Index into the asset's Notifies array"),
        RPC_PARAM_OPT("notifyName", "string", "Notify-state event name to match when notifyIndex is omitted"),
        RPC_PARAM_REQ("propertyPath", "string", "Property path on the notify-state instance; supports nested object paths"),
        RPC_PARAM_REQ("value", "any", "Value to set (type-dependent)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    const TSharedPtr<FJsonObject>& Params = Ctx.GetRawPayload();
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString NotifyName = Ctx.GetString(TEXT("notifyName"));
    FString PropertyPath = Ctx.GetString(TEXT("propertyPath"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (PropertyPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAMETERS, TEXT("propertyPath is required"));
        return true;
    }

    TSharedPtr<FJsonValue> ValueField;
    if (Params.IsValid())
    {
        ValueField = Params->TryGetField(TEXT("value"));
    }
    if (!ValueField.IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_VALUE, TEXT("value parameter is required"));
        return true;
    }

    UAnimSequenceBase* AnimAsset = AnimationAuthoringHelpers::LoadAnimSequenceBaseFromPath(AssetPath);
    if (!AnimAsset)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(TEXT("Could not load animation asset: %s"), *AssetPath));
        return true;
    }

    double NotifyIndexNumber = 0.0;
    const bool bHasNotifyIndex = Params.IsValid() && Params->TryGetNumberField(TEXT("notifyIndex"), NotifyIndexNumber);
    const int32 NotifyIndex = bHasNotifyIndex ? static_cast<int32>(NotifyIndexNumber) : INDEX_NONE;
    if (!bHasNotifyIndex && NotifyName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAMETERS, TEXT("notifyIndex or notifyName is required"));
        return true;
    }

    FAnimNotifyEvent* TargetEvent = nullptr;
    int32 MatchedNotifyIndex = INDEX_NONE;
    if (bHasNotifyIndex)
    {
        if (!AnimAsset->Notifies.IsValidIndex(NotifyIndex))
        {
            Ctx.SendError(ErrorCodes::ERR_NOTIFY_STATE_NOT_FOUND,
                FString::Printf(TEXT("Notify-state index %d is out of range"), NotifyIndex));
            return true;
        }

        FAnimNotifyEvent& Candidate = AnimAsset->Notifies[NotifyIndex];
        if (!Candidate.NotifyStateClass)
        {
            Ctx.SendError(ErrorCodes::ERR_NOTIFY_STATE_NOT_FOUND,
                FString::Printf(TEXT("Notify event at index %d is not a notify state"), NotifyIndex));
            return true;
        }

        TargetEvent = &Candidate;
        MatchedNotifyIndex = NotifyIndex;
    }
    else
    {
        for (int32 Index = 0; Index < AnimAsset->Notifies.Num(); ++Index)
        {
            FAnimNotifyEvent& Candidate = AnimAsset->Notifies[Index];
            UAnimNotifyState* NotifyState = Candidate.NotifyStateClass;
            if (!NotifyState)
            {
                continue;
            }

            const bool bEventNameMatches = Candidate.NotifyName.ToString().Equals(NotifyName, ESearchCase::IgnoreCase);
            const bool bStateNameMatches = NotifyState->GetNotifyName().Equals(NotifyName, ESearchCase::IgnoreCase);
            if (bEventNameMatches || bStateNameMatches)
            {
                TargetEvent = &Candidate;
                MatchedNotifyIndex = Index;
                break;
            }
        }

        if (!TargetEvent)
        {
            Ctx.SendError(ErrorCodes::ERR_NOTIFY_STATE_NOT_FOUND,
                FString::Printf(TEXT("Notify state named '%s' not found"), *NotifyName));
            return true;
        }
    }

    UAnimNotifyState* NotifyState = TargetEvent->NotifyStateClass;
    void* TargetContainer = nullptr;
    FString ResolveError;
    FProperty* Property = ResolveNestedPropertyPath(NotifyState, PropertyPath, TargetContainer, ResolveError);
    if (!Property || !TargetContainer)
    {
        Ctx.SendError(ErrorCodes::ERR_PROPERTY_NOT_FOUND,
            FString::Printf(TEXT("Failed to resolve notify-state property path '%s': %s"), *PropertyPath, *ResolveError));
        return true;
    }

    FString ApplyError;
    if (!ApplyJsonValueToProperty(TargetContainer, Property, ValueField, ApplyError))
    {
        Ctx.SendError(ErrorCodes::ERR_PROPERTY_SET_FAILED, FString::Printf(TEXT("Failed to set property: %s"), *ApplyError));
        return true;
    }

    AnimAsset->RefreshCacheData();
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimAsset, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Notify-state property '%s' set"), *PropertyPath));
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetNumberField(TEXT("notifyIndex"), MatchedNotifyIndex);
    Result->SetStringField(TEXT("propertyPath"), PropertyPath);
    AddAssetVerification(Result, AnimAsset);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_sync_marker", "animation.authoring",
    "Add a sync marker to an animation sequence",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the animation sequence"),
        RPC_PARAM_REQ("markerName", "string", "Name of the sync marker"),
        RPC_PARAM_OPT("frame", "integer", "Frame number (default 0)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString MarkerName = Ctx.GetString(TEXT("markerName"));
    int32 Frame = Ctx.GetInt(TEXT("frame"), 0);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (MarkerName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_MARKER_NAME, TEXT("markerName is required"));
        return true;
    }

    UAnimSequence* Sequence = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(AssetPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, FString::Printf(TEXT("Could not load animation sequence: %s"), *AssetPath));
        return true;
    }

    TArray<FPwSyncMarkerSpec> Replacement = ReadAnimSequenceSyncMarkers(Sequence);
    FPwSyncMarkerSpec& NewMarker = Replacement.AddDefaulted_GetRef();
    NewMarker.MarkerName = FName(*MarkerName);
    const FFrameRate FrameRate = Sequence->GetSamplingFrameRate();
    if (FrameRate.Numerator <= 0 || FrameRate.Denominator <= 0)
    {
        Ctx.SendError(ErrorCodes::ERR_ANIMATION_INVALID,
            TEXT("Animation sequence has no valid sampling frame rate"));
        return true;
    }
    NewMarker.Time = static_cast<float>(
        static_cast<double>(Frame) * FrameRate.Denominator
        / static_cast<double>(FrameRate.Numerator));

    const FAnimSequenceSyncMarkerWriteResult WriteResult = SetAnimSequenceSyncMarkers(
        Sequence, TArrayView<const FPwSyncMarkerSpec>(Replacement));
    if (!WriteResult.bSuccess)
    {
        Ctx.SendError(*WriteResult.ErrorCode, WriteResult.ErrorMessage);
        return true;
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Sequence, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Sync marker '%s' added"), *MarkerName));
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetArrayField(TEXT("syncMarkers"), AnimSequenceDumpBuilder::BuildSyncMarkersArrayJson(Sequence));
    AddAssetVerification(Result, Sequence);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_sync_markers", "animation.authoring",
    "Replace all sync markers on an animation sequence atomically",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the animation sequence"),
        RPC_PARAM_REQ("markers", "array", "Replacement markers as [{name, frame}, ...]; an empty array clears them"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    const FString AssetPath = NormalizeContentAssetPath(
        Ctx.GetString(TEXT("assetPath")));
    const bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAnimSequence* Sequence = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(AssetPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND,
            FString::Printf(TEXT("Could not load animation sequence: %s"), *AssetPath));
        return true;
    }

    FString ParseError;
    TArray<FPwSyncMarkerSpec> Markers;
    if (!PwAnimSequenceAuthoringJson::ReadSyncMarkerSpecs(
            Ctx.GetArray(TEXT("markers")), Sequence, Markers, ParseError))
    {
        Ctx.SendError(ErrorCodes::ERR_ANIMATION_INVALID, ParseError);
        return true;
    }

    const FAnimSequenceSyncMarkerWriteResult WriteResult = SetAnimSequenceSyncMarkers(
        Sequence, TArrayView<const FPwSyncMarkerSpec>(Markers));
    if (!WriteResult.bSuccess)
    {
        Ctx.SendError(*WriteResult.ErrorCode, WriteResult.ErrorMessage);
        return true;
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Sequence, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), TEXT("Sync marker set replaced"));
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetArrayField(TEXT("syncMarkers"), AnimSequenceDumpBuilder::BuildSyncMarkersArrayJson(Sequence));
    AddAssetVerification(Result, Sequence);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.remove_sync_marker", "animation.authoring",
    "Remove every occurrence of a named sync marker from an animation sequence",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the animation sequence"),
        RPC_PARAM_REQ("markerName", "string", "Name of the sync marker; all occurrences are removed"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    const FString AssetPath = NormalizeContentAssetPath(
        Ctx.GetString(TEXT("assetPath")));
    const FString MarkerName = Ctx.GetString(TEXT("markerName"));
    const bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (MarkerName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_MARKER_NAME, TEXT("markerName is required"));
        return true;
    }

    UAnimSequence* Sequence = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(AssetPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND,
            FString::Printf(TEXT("Could not load animation sequence: %s"), *AssetPath));
        return true;
    }

    const FAnimSequenceSyncMarkerWriteResult WriteResult = RemoveAnimSequenceSyncMarkers(
        Sequence, FName(*MarkerName));
    if (!WriteResult.bSuccess)
    {
        Ctx.SendError(*WriteResult.ErrorCode, WriteResult.ErrorMessage);
        return true;
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Sequence, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(
        TEXT("Removed %d sync marker occurrence(s) named '%s'"),
        WriteResult.RemovedCount, *MarkerName));
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetNumberField(TEXT("removedCount"), WriteResult.RemovedCount);
    Result->SetArrayField(TEXT("syncMarkers"), AnimSequenceDumpBuilder::BuildSyncMarkersArrayJson(Sequence));
    AddAssetVerification(Result, Sequence);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.list_sync_markers", "animation.authoring",
    "List sync markers on an animation sequence",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the animation sequence")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));

    UAnimSequence* Sequence = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(AssetPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, FString::Printf(TEXT("Could not load animation sequence: %s"), *AssetPath));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetArrayField(TEXT("syncMarkers"), AnimSequenceDumpBuilder::BuildSyncMarkersArrayJson(Sequence));
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_root_motion_settings", "animation.authoring",
    "Configure root motion settings on an animation sequence",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the animation sequence"),
        RPC_PARAM_OPT("enableRootMotion", "boolean", "Enable root motion (default true)"),
        RPC_PARAM_OPT("rootMotionRootLock", "string", "Root lock type: RefPose, AnimFirstFrame, Zero"),
        RPC_PARAM_OPT("forceRootLock", "boolean", "Force root lock (default false)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    bool bEnableRootMotion = Ctx.GetBool(TEXT("enableRootMotion"), true);
    FString RootMotionRootLock = Ctx.GetString(TEXT("rootMotionRootLock"), TEXT("RefPose"));
    bool bForceRootLock = Ctx.GetBool(TEXT("forceRootLock"), false);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAnimSequence* Sequence = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(AssetPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, FString::Printf(TEXT("Could not load animation sequence: %s"), *AssetPath));
        return true;
    }

    Sequence->bEnableRootMotion = bEnableRootMotion;
    Sequence->bForceRootLock = bForceRootLock;

    // Set root motion lock type
    if (RootMotionRootLock == TEXT("AnimFirstFrame"))
    {
        Sequence->RootMotionRootLock = ERootMotionRootLock::AnimFirstFrame;
    }
    else if (RootMotionRootLock == TEXT("Zero"))
    {
        Sequence->RootMotionRootLock = ERootMotionRootLock::Zero;
    }
    else
    {
        Sequence->RootMotionRootLock = ERootMotionRootLock::RefPose;
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Sequence, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), TEXT("Root motion settings updated"));
    AddAssetVerification(Result, Sequence);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_additive_settings", "animation.authoring",
    "Configure additive animation settings on a sequence",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the animation sequence"),
        RPC_PARAM_OPT("additiveAnimType", "string", "NoAdditive, LocalSpaceAdditive, or MeshSpaceAdditive"),
        RPC_PARAM_OPT("basePoseType", "string", "RefPose, AnimationFrame, or AnimationScaled"),
        RPC_PARAM_OPT("basePoseAnimation", "path", "Path to base pose animation"),
        RPC_PARAM_OPT("basePoseFrame", "integer", "Base pose frame index"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString AdditiveAnimType = Ctx.GetString(TEXT("additiveAnimType"), TEXT("NoAdditive"));
    FString BasePoseType = Ctx.GetString(TEXT("basePoseType"), TEXT("RefPose"));
    FString BasePoseAnimation = Ctx.GetString(TEXT("basePoseAnimation"));
    int32 BasePoseFrame = Ctx.GetInt(TEXT("basePoseFrame"), 0);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAnimSequence* Sequence = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(AssetPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, FString::Printf(TEXT("Could not load animation sequence: %s"), *AssetPath));
        return true;
    }

    // Set additive anim type
    if (AdditiveAnimType == TEXT("LocalSpaceAdditive"))
    {
        Sequence->AdditiveAnimType = AAT_LocalSpaceBase;
    }
    else if (AdditiveAnimType == TEXT("MeshSpaceAdditive"))
    {
        Sequence->AdditiveAnimType = AAT_RotationOffsetMeshSpace;
    }
    else
    {
        Sequence->AdditiveAnimType = AAT_None;
    }

    // Set base pose type
    if (BasePoseType == TEXT("AnimationFrame"))
    {
        Sequence->RefPoseType = ABPT_AnimFrame;
        Sequence->RefFrameIndex = BasePoseFrame;
    }
    else if (BasePoseType == TEXT("AnimationScaled"))
    {
        Sequence->RefPoseType = ABPT_AnimScaled;
    }
    else
    {
        Sequence->RefPoseType = ABPT_RefPose;
    }

    // Set base pose animation if provided
    if (!BasePoseAnimation.IsEmpty())
    {
        UAnimSequence* BaseAnim = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(BasePoseAnimation);
        if (BaseAnim)
        {
            Sequence->RefPoseSeq = BaseAnim;
        }
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Sequence, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), TEXT("Additive settings updated"));
    AddAssetVerification(Result, Sequence);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ===========================================================================
// 10.2 ANIMATION MONTAGES
// ===========================================================================

REGISTER_RPC_HANDLER("animation.authoring.create_montage", "animation.authoring",
    "Create a new UAnimMontage asset bound to a Skeleton with one slot and an empty 'Default' section. Build out sections and notifies via animation.authoring.add_montage_section / add_montage_notify after creation.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset filename (no extension) for the new montage."),
        RPC_PARAM_OPT("path", "path", "Content-browser folder; defaults to /Game/Animations."),
        RPC_PARAM_REQ("skeletonPath", "path", "Asset path to the Skeleton this montage will animate."),
        RPC_PARAM_OPT("slotName", "string", "Identifier of the montage slot used by AnimGraph slot nodes; defaults to 'DefaultSlot'."),
        RPC_PARAM_OPT("save", "boolean", "When true (default) the new asset is marked dirty so the next editor save persists it.")
    ))
{
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Animations")));
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    FString SlotName = Ctx.GetString(TEXT("slotName"), TEXT("DefaultSlot"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (Name.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_NAME, TEXT("Name is required"));
        return true;
    }

    // Composed and checked above the skeleton load, for the reason spelled out at
    // create_animation_sequence: an unvalidated `name` reaches CreatePackage's Fatal and ends the
    // editor process, and this ordering is what keeps the regression test off that path.
    FString PackagePath;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(Path, Name, PackagePath, PathError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("%s Pass a bare asset name and choose the folder with path "
                                 "(default /Game/Animations)."), *PathError));
        return true;
    }

    USkeleton* Skeleton = AnimationAuthoringHelpers::LoadSkeletonFromPathAnim(SkeletonPath);
    if (!Skeleton)
    {
        Ctx.SendError(ErrorCodes::ERR_SKELETON_NOT_FOUND, FString::Printf(TEXT("Could not load skeleton: %s"), *SkeletonPath));
        return true;
    }

    // Create package and asset directly to avoid UI dialogs
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
    Factory->TargetSkeleton = Skeleton;
    UAnimMontage* NewMontage = Cast<UAnimMontage>(
        Factory->FactoryCreateNew(UAnimMontage::StaticClass(), Package,
                                  FName(*Name), RF_Public | RF_Standalone,
                                  nullptr, GWarn));
    if (!NewMontage)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create montage"));
        return true;
    }

    // The UAnimMontage constructor (via UAnimMontageFactory) seeds exactly one empty
    // slot named "DefaultSlot", so the montage already has the single slot the contract
    // promises. Rename that seeded slot to the requested name rather than appending a
    // second one (the old unconditional AddDefaulted_GetRef() produced a duplicate-named
    // slot, numSlots:2 on a fresh montage). On a freshly factory-created montage this is
    // always SlotAnimTracks[0]; the default slotName="DefaultSlot" path is a no-op rename.
    if (!SlotName.IsEmpty() && NewMontage->SlotAnimTracks.Num() > 0)
    {
        NewMontage->SlotAnimTracks[0].SlotName = FName(*SlotName);
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(NewMontage, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), Path / Name);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Montage '%s' created"), *Name));
    AddAssetVerification(Result, NewMontage);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_montage_section", "animation.authoring",
    "Add a section to an animation montage",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the montage"),
        RPC_PARAM_REQ("sectionName", "string", "Name of the section"),
        RPC_PARAM_OPT("startTime", "number", "Section start time (default 0)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString SectionName = Ctx.GetString(TEXT("sectionName"));
    float StartTime = static_cast<float>(Ctx.GetNumber(TEXT("startTime"), 0.0));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (SectionName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_SECTION_NAME, TEXT("sectionName is required"));
        return true;
    }

    UAnimMontage* Montage = Cast<UAnimMontage>(StaticLoadObject(UAnimMontage::StaticClass(), nullptr, *AssetPath));
    if (!Montage)
    {
        Ctx.SendError(ErrorCodes::ERR_MONTAGE_NOT_FOUND, FString::Printf(TEXT("Could not load montage: %s"), *AssetPath));
        return true;
    }

    // Add new section
    int32 SectionIndex = Montage->AddAnimCompositeSection(FName(*SectionName), StartTime);

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Montage, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Section '%s' added at index %d"), *SectionName, SectionIndex));
    AddAssetVerification(Result, Montage);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_montage_slot", "animation.authoring",
    "Add an animation to a montage slot",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the montage"),
        RPC_PARAM_REQ("animationPath", "path", "Path to the animation to add"),
        RPC_PARAM_OPT("slotName", "string", "Slot name (default DefaultSlot)"),
        RPC_PARAM_OPT("startTime", "number", "Start time in montage (default 0)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString AnimationPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("animationPath")));
    FString SlotName = Ctx.GetString(TEXT("slotName"), TEXT("DefaultSlot"));
    float StartTime = static_cast<float>(Ctx.GetNumber(TEXT("startTime"), 0.0));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAnimMontage* Montage = Cast<UAnimMontage>(StaticLoadObject(UAnimMontage::StaticClass(), nullptr, *AssetPath));
    if (!Montage)
    {
        Ctx.SendError(ErrorCodes::ERR_MONTAGE_NOT_FOUND, FString::Printf(TEXT("Could not load montage: %s"), *AssetPath));
        return true;
    }

    UAnimSequence* Animation = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(AnimationPath);
    if (!Animation)
    {
        Ctx.SendError(ErrorCodes::ERR_ANIMATION_NOT_FOUND, FString::Printf(TEXT("Could not load animation: %s"), *AnimationPath));
        return true;
    }

    // Find or create slot track
    FSlotAnimationTrack* SlotTrack = nullptr;
    for (FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
    {
        if (Track.SlotName == FName(*SlotName))
        {
            SlotTrack = &Track;
            break;
        }
    }

    if (!SlotTrack)
    {
        SlotTrack = &Montage->SlotAnimTracks.AddDefaulted_GetRef();
        SlotTrack->SlotName = FName(*SlotName);
    }

    // Add animation to slot track
    FAnimSegment& Segment = SlotTrack->AnimTrack.AnimSegments.AddDefaulted_GetRef();
    Segment.SetAnimReference(Animation);
    Segment.StartPos = StartTime;
    Segment.AnimStartTime = 0.0f;
    Segment.AnimEndTime = Animation->GetPlayLength();
    Segment.AnimPlayRate = 1.0f;
    Segment.LoopingCount = 1;

    // Adding a slot segment is what gives a montage its real duration, but the segment append
    // alone leaves the montage's SequenceLength stale at 0 (get_animation_info / the montage
    // timeline read GetPlayLength()). Recompute the slot-segment-derived length and persist it
    // the same way the montage editor does (the composite verb already uses this idiom at the
    // add_composite_segment handler). CalculateSequenceLength() takes the max slot-track length;
    // SetCompositeLength() writes it back through the WITH_EDITOR data-model controller.
    Montage->SetCompositeLength(Montage->CalculateSequenceLength());

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Montage, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), TEXT("Animation added to montage slot"));
    Result->SetNumberField(TEXT("montageLength"), Montage->GetPlayLength());
    AddAssetVerification(Result, Montage);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_section_timing", "animation.authoring",
    "Update section timing in a montage",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the montage"),
        RPC_PARAM_REQ("sectionName", "string", "Name of the section"),
        RPC_PARAM_OPT("startTime", "number", "New start time"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    const TSharedPtr<FJsonObject>& Params = Ctx.GetRawPayload();
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString SectionName = Ctx.GetString(TEXT("sectionName"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (SectionName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_SECTION_NAME, TEXT("sectionName is required"));
        return true;
    }

    UAnimMontage* Montage = Cast<UAnimMontage>(StaticLoadObject(UAnimMontage::StaticClass(), nullptr, *AssetPath));
    if (!Montage)
    {
        Ctx.SendError(ErrorCodes::ERR_MONTAGE_NOT_FOUND, FString::Printf(TEXT("Could not load montage: %s"), *AssetPath));
        return true;
    }

    int32 SectionIndex = Montage->GetSectionIndex(FName(*SectionName));
    if (SectionIndex == INDEX_NONE)
    {
        Ctx.SendError(ErrorCodes::ERR_SECTION_NOT_FOUND, FString::Printf(TEXT("Section not found: %s"), *SectionName));
        return true;
    }

    // Update section timing if startTime is provided
    if (Params->HasField(TEXT("startTime")))
    {
        float StartTime = static_cast<float>(GetJsonNumberField(Params, TEXT("startTime")));
        FCompositeSection& Section = Montage->CompositeSections[SectionIndex];
        Section.SetTime(StartTime);
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Montage, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), TEXT("Section timing updated"));
    AddAssetVerification(Result, Montage);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_montage_notify", "animation.authoring",
    "Add a notify to a montage",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the montage"),
        RPC_PARAM_OPT("notifyClass", "classref", "Notify class name (default AnimNotify)"),
        RPC_PARAM_OPT("time", "number", "Trigger time (default 0)"),
        RPC_PARAM_OPT("trackIndex", "integer", "Track index (default 0)"),
        RPC_PARAM_OPT("notifyName", "string", "Optional notify name"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString NotifyClass = Ctx.GetString(TEXT("notifyClass"), TEXT("AnimNotify"));
    float Time = static_cast<float>(Ctx.GetNumber(TEXT("time"), 0.0));
    int32 TrackIndex = Ctx.GetInt(TEXT("trackIndex"), 0);
    FString NotifyName = Ctx.GetString(TEXT("notifyName"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAnimMontage* Montage = Cast<UAnimMontage>(StaticLoadObject(UAnimMontage::StaticClass(), nullptr, *AssetPath));
    if (!Montage)
    {
        Ctx.SendError(ErrorCodes::ERR_MONTAGE_NOT_FOUND, FString::Printf(TEXT("Could not load montage: %s"), *AssetPath));
        return true;
    }

    // Find notify class
    FString FullClassName = NotifyClass;
    if (!FullClassName.StartsWith(TEXT("AnimNotify_")))
    {
        FullClassName = TEXT("AnimNotify_") + NotifyClass;
    }

    // Resolve the notify class, but NEVER instantiate an abstract one -- the default
    // notifyClass "AnimNotify" (and any unresolved name) would otherwise fall through to the
    // abstract UAnimNotify base, tripping an editor ensure in StaticAllocateObjectErrorTests
    // and getting nulled out on save. A null class means a valid name-only notify.
    UClass* NotifyUClass = AnimationAuthoringHelpers::ResolveConcreteNotifyClassOrNull(FullClassName);

    // Create notify (name-only when no concrete class resolved).
    UAnimNotify* NewNotify = NotifyUClass ? NewObject<UAnimNotify>(Montage, NotifyUClass) : nullptr;
    FAnimNotifyEvent& NotifyEvent = Montage->Notifies.AddDefaulted_GetRef();
    NotifyEvent.Notify = NewNotify;
    AnimationAuthoringHelpers::LinkNotifyAtTime(NotifyEvent, Montage, Time);
    NotifyEvent.TrackIndex = TrackIndex;

    if (!NotifyName.IsEmpty())
    {
        NotifyEvent.NotifyName = FName(*NotifyName);
    }

    AnimationAuthoringHelpers::EnsureNotifyTrack(Montage, NotifyEvent);
    Montage->RefreshCacheData();

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Montage, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), TEXT("Montage notify added"));
    AddAssetVerification(Result, Montage);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_blend_in", "animation.authoring",
    "Set blend-in settings for a montage",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the montage"),
        RPC_PARAM_OPT("blendTime", "number", "Blend time in seconds (default 0.25)"),
        RPC_PARAM_OPT("blendOption", "string", "Blend option: Linear, Cubic, Sinusoidal"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    float BlendTime = static_cast<float>(Ctx.GetNumber(TEXT("blendTime"), 0.25));
    FString BlendOption = Ctx.GetString(TEXT("blendOption"), TEXT("Linear"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAnimMontage* Montage = Cast<UAnimMontage>(StaticLoadObject(UAnimMontage::StaticClass(), nullptr, *AssetPath));
    if (!Montage)
    {
        Ctx.SendError(ErrorCodes::ERR_MONTAGE_NOT_FOUND, FString::Printf(TEXT("Could not load montage: %s"), *AssetPath));
        return true;
    }

    Montage->BlendIn.SetBlendTime(BlendTime);

    // Set blend option
    if (BlendOption == TEXT("Cubic"))
    {
        Montage->BlendIn.SetBlendOption(EAlphaBlendOption::Cubic);
    }
    else if (BlendOption == TEXT("Sinusoidal"))
    {
        Montage->BlendIn.SetBlendOption(EAlphaBlendOption::Sinusoidal);
    }
    else
    {
        Montage->BlendIn.SetBlendOption(EAlphaBlendOption::Linear);
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Montage, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), TEXT("Blend in settings updated"));
    AddAssetVerification(Result, Montage);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_blend_out", "animation.authoring",
    "Set blend-out settings for a montage",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the montage"),
        RPC_PARAM_OPT("blendTime", "number", "Blend time in seconds (default 0.25)"),
        RPC_PARAM_OPT("blendOption", "string", "Blend option: Linear, Cubic, Sinusoidal"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    float BlendTime = static_cast<float>(Ctx.GetNumber(TEXT("blendTime"), 0.25));
    FString BlendOption = Ctx.GetString(TEXT("blendOption"), TEXT("Linear"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAnimMontage* Montage = Cast<UAnimMontage>(StaticLoadObject(UAnimMontage::StaticClass(), nullptr, *AssetPath));
    if (!Montage)
    {
        Ctx.SendError(ErrorCodes::ERR_MONTAGE_NOT_FOUND, FString::Printf(TEXT("Could not load montage: %s"), *AssetPath));
        return true;
    }

    Montage->BlendOut.SetBlendTime(BlendTime);

    // Set blend option
    if (BlendOption == TEXT("Cubic"))
    {
        Montage->BlendOut.SetBlendOption(EAlphaBlendOption::Cubic);
    }
    else if (BlendOption == TEXT("Sinusoidal"))
    {
        Montage->BlendOut.SetBlendOption(EAlphaBlendOption::Sinusoidal);
    }
    else
    {
        Montage->BlendOut.SetBlendOption(EAlphaBlendOption::Linear);
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Montage, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), TEXT("Blend out settings updated"));
    AddAssetVerification(Result, Montage);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.link_sections", "animation.authoring",
    "Link two montage sections (set next section)",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the montage"),
        RPC_PARAM_REQ("fromSection", "string", "Source section name"),
        RPC_PARAM_REQ("toSection", "string", "Target section name"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString FromSection = Ctx.GetString(TEXT("fromSection"));
    FString ToSection = Ctx.GetString(TEXT("toSection"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (FromSection.IsEmpty() || ToSection.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_SECTIONS, TEXT("fromSection and toSection are required"));
        return true;
    }

    UAnimMontage* Montage = Cast<UAnimMontage>(StaticLoadObject(UAnimMontage::StaticClass(), nullptr, *AssetPath));
    if (!Montage)
    {
        Ctx.SendError(ErrorCodes::ERR_MONTAGE_NOT_FOUND, FString::Printf(TEXT("Could not load montage: %s"), *AssetPath));
        return true;
    }

    // Set next section using section index-based API
    int32 FromSectionIndex = Montage->GetSectionIndex(FName(*FromSection));
    int32 ToSectionIndex = Montage->GetSectionIndex(FName(*ToSection));
    if (FromSectionIndex != INDEX_NONE && ToSectionIndex != INDEX_NONE)
    {
        Montage->CompositeSections[FromSectionIndex].NextSectionName = FName(*ToSection);
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Montage, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Linked '%s' to '%s'"), *FromSection, *ToSection));
    AddAssetVerification(Result, Montage);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ===========================================================================
// 10.3 ANIMATION COMPOSITES
// ===========================================================================

REGISTER_RPC_HANDLER("animation.authoring.create_composite", "animation.authoring",
    "Create an empty UAnimComposite asset bound to a Skeleton. Add sequence/composite clips with animation.authoring.add_composite_segment.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset filename (no extension) for the new composite."),
        RPC_PARAM_OPT("path", "path", "Content-browser folder; defaults to /Game/Animations."),
        RPC_PARAM_REQ("skeletonPath", "path", "Asset path to the Skeleton this composite will animate."),
        RPC_PARAM_OPT("save", "boolean", "When true (default) the new asset is marked dirty so the next editor save persists it.")
    ))
{
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Animations")));
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (Name.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_NAME, TEXT("Name is required"));
        return true;
    }

    if (SkeletonPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_SKELETON_PATH, TEXT("skeletonPath is required"));
        return true;
    }

    // Composed and checked above the skeleton load, for the reason spelled out at
    // create_animation_sequence: an unvalidated `name` reaches CreatePackage's Fatal and ends the
    // editor process, and this ordering is what keeps the regression test off that path.
    FString PackagePath;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(Path, Name, PackagePath, PathError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("%s Pass a bare asset name and choose the folder with path "
                                 "(default /Game/Animations)."), *PathError));
        return true;
    }

    USkeleton* Skeleton = AnimationAuthoringHelpers::LoadSkeletonFromPathAnim(SkeletonPath);
    if (!Skeleton)
    {
        Ctx.SendError(ErrorCodes::ERR_SKELETON_NOT_FOUND, FString::Printf(TEXT("Could not load skeleton: %s"), *SkeletonPath));
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    UAnimCompositeFactory* Factory = NewObject<UAnimCompositeFactory>();
    Factory->TargetSkeleton = Skeleton;
    UAnimComposite* NewComposite = Cast<UAnimComposite>(
        Factory->FactoryCreateNew(UAnimComposite::StaticClass(), Package,
                                  FName(*Name), RF_Public | RF_Standalone,
                                  nullptr, GWarn));
    if (!NewComposite)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create animation composite"));
        return true;
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(NewComposite, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), Path / Name);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Animation composite '%s' created"), *Name));
    AddAssetVerification(Result, NewComposite);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_composite_segment", "animation.authoring",
    "Append an animation segment to a UAnimComposite track and update the composite length.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the UAnimComposite asset."),
        RPC_PARAM_REQ("animationPath", "path", "Path to a UAnimSequenceBase asset to add as a segment."),
        RPC_PARAM_OPT("startPos", "number", "Start position in the composite track; defaults to the current composite length."),
        RPC_PARAM_OPT("animPlayRate", "number", "Segment playback rate; defaults to 1.0."),
        RPC_PARAM_OPT("animStartTime", "number", "Start time inside the referenced animation; defaults to 0.0."),
        RPC_PARAM_OPT("animEndTime", "number", "End time inside the referenced animation; defaults to the referenced animation length."),
        RPC_PARAM_OPT("loopingCount", "integer", "Number of loops for this segment; defaults to 1."),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true).")
    ))
{
    const TSharedPtr<FJsonObject>& Params = Ctx.GetRawPayload();
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString AnimationPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("animationPath")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_ASSET_PATH, TEXT("assetPath is required"));
        return true;
    }

    if (AnimationPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_ANIMATION_PATH, TEXT("animationPath is required"));
        return true;
    }

    UAnimComposite* Composite = Cast<UAnimComposite>(StaticLoadObject(UAnimComposite::StaticClass(), nullptr, *AssetPath));
    if (!Composite)
    {
        Ctx.SendError(ErrorCodes::ERR_COMPOSITE_NOT_FOUND, FString::Printf(TEXT("Could not load animation composite: %s"), *AssetPath));
        return true;
    }

    UAnimSequenceBase* Animation = AnimationAuthoringHelpers::LoadAnimSequenceBaseFromPath(AnimationPath);
    if (!Animation)
    {
        Ctx.SendError(ErrorCodes::ERR_ANIMATION_NOT_FOUND, FString::Printf(TEXT("Could not load animation: %s"), *AnimationPath));
        return true;
    }

    USkeleton* CompositeSkeleton = Composite->GetSkeleton();
    USkeleton* AnimationSkeleton = Animation->GetSkeleton();
    if (!CompositeSkeleton || !AnimationSkeleton || !CompositeSkeleton->IsCompatibleForEditor(AnimationSkeleton))
    {
        Ctx.SendError(ErrorCodes::ERR_SKELETON_MISMATCH, FString::Printf(
            TEXT("Animation '%s' is not compatible with composite '%s'"),
            *AnimationPath, *AssetPath));
        return true;
    }

    FText InvalidReason;
    if (!Composite->AnimationTrack.IsValidToAdd(Animation, &InvalidReason))
    {
        FString Reason = InvalidReason.ToString();
        if (Reason.IsEmpty())
        {
            Reason = FString::Printf(TEXT("Animation '%s' cannot be added to composite '%s'"), *AnimationPath, *AssetPath);
        }
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEGMENT_ANIMATION, Reason);
        return true;
    }

    FAnimSegment Segment;
    Segment.SetAnimReference(Animation, true);
    if (Params.IsValid() && Params->HasField(TEXT("startPos")))
    {
        Segment.StartPos = static_cast<float>(GetJsonNumberField(Params, TEXT("startPos")));
    }
    else
    {
        Segment.StartPos = Composite->AnimationTrack.GetLength();
    }
    if (Params.IsValid() && Params->HasField(TEXT("animPlayRate")))
    {
        Segment.AnimPlayRate = static_cast<float>(GetJsonNumberField(Params, TEXT("animPlayRate")));
    }
    if (Params.IsValid() && Params->HasField(TEXT("animStartTime")))
    {
        Segment.AnimStartTime = static_cast<float>(GetJsonNumberField(Params, TEXT("animStartTime")));
    }
    if (Params.IsValid() && Params->HasField(TEXT("animEndTime")))
    {
        Segment.AnimEndTime = static_cast<float>(GetJsonNumberField(Params, TEXT("animEndTime")));
    }
    if (Params.IsValid() && Params->HasField(TEXT("loopingCount")))
    {
        Segment.LoopingCount = Ctx.GetInt(TEXT("loopingCount"), Segment.LoopingCount);
    }

    struct FTrackedCompositeSegment
    {
        FAnimSegment Segment;
        int32 OriginalIndex = INDEX_NONE;
    };

    TArray<FTrackedCompositeSegment> TrackedSegments;
    TrackedSegments.Reserve(Composite->AnimationTrack.AnimSegments.Num() + 1);
    for (int32 Index = 0; Index < Composite->AnimationTrack.AnimSegments.Num(); ++Index)
    {
        FTrackedCompositeSegment TrackedSegment;
        TrackedSegment.Segment = Composite->AnimationTrack.AnimSegments[Index];
        TrackedSegment.OriginalIndex = Index;
        TrackedSegments.Add(TrackedSegment);
    }
    const int32 InsertedOriginalIndex = Composite->AnimationTrack.AnimSegments.Num();
    FTrackedCompositeSegment InsertedSegment;
    InsertedSegment.Segment = Segment;
    InsertedSegment.OriginalIndex = InsertedOriginalIndex;
    TrackedSegments.Add(InsertedSegment);
    TrackedSegments.Sort(
        [](const FTrackedCompositeSegment& A, const FTrackedCompositeSegment& B)
        {
            if (!FMath::IsNearlyEqual(A.Segment.StartPos, B.Segment.StartPos))
            {
                return A.Segment.StartPos < B.Segment.StartPos;
            }
            return A.OriginalIndex < B.OriginalIndex;
        });

    int32 SegmentIndex = INDEX_NONE;
    Composite->AnimationTrack.AnimSegments.Reset(TrackedSegments.Num());
    for (int32 Index = 0; Index < TrackedSegments.Num(); ++Index)
    {
        if (TrackedSegments[Index].OriginalIndex == InsertedOriginalIndex)
        {
            SegmentIndex = Index;
        }
        Composite->AnimationTrack.AnimSegments.Add(TrackedSegments[Index].Segment);
    }
    if (Composite->AnimationTrack.AnimSegments.Num() > 0)
    {
        Composite->AnimationTrack.AnimSegments[0].StartPos = 0.0f;
        for (int32 Index = 0; Index < Composite->AnimationTrack.AnimSegments.Num(); ++Index)
        {
            FAnimSegment& AnimSegment = Composite->AnimationTrack.AnimSegments[Index];
            if (Index > 0)
            {
                AnimSegment.StartPos =
                    Composite->AnimationTrack.AnimSegments[Index - 1].StartPos +
                    Composite->AnimationTrack.AnimSegments[Index - 1].GetLength();
            }

            const UAnimSequenceBase* AnimReference = AnimSegment.GetAnimReference();
            if (AnimReference && AnimSegment.AnimEndTime > AnimReference->GetPlayLength())
            {
                AnimSegment.AnimEndTime = AnimReference->GetPlayLength();
            }
        }
    }
    Composite->SetCompositeLength(Composite->AnimationTrack.GetLength());

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(Composite, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), TEXT("Composite segment added"));
    Result->SetNumberField(TEXT("segmentIndex"), SegmentIndex);
    Result->SetNumberField(TEXT("segmentCount"), Composite->AnimationTrack.AnimSegments.Num());
    Result->SetNumberField(TEXT("compositeLength"), Composite->GetPlayLength());
    AddAssetVerification(Result, Composite);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ===========================================================================
// UTILITY
// ===========================================================================

REGISTER_RPC_HANDLER("animation.authoring.get_animation_info", "animation.authoring",
    "Get detailed information about an animation asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the animation asset")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));

    UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
    if (!Asset)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(TEXT("Could not load asset: %s"), *AssetPath));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> AnimInfo = MakeShared<FJsonObject>();

    if (UAnimSequence* Sequence = Cast<UAnimSequence>(Asset))
    {
        AnimInfo->SetStringField(TEXT("assetType"), TEXT("AnimSequence"));
        const USkeleton* Skeleton = Sequence->GetSkeleton();
        const IAnimationDataModel* DataModel = Sequence->GetDataModel();
        const FFrameRate FrameRate = DataModel ? DataModel->GetFrameRate() : Sequence->GetSamplingFrameRate();
        if (Sequence->GetSkeleton())
        {
            AnimInfo->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
        }
        AnimInfo->SetStringField(TEXT("skeletonAssetPath"), Skeleton ? Skeleton->GetPathName() : FString());
        AnimInfo->SetNumberField(TEXT("duration"), Sequence->GetPlayLength());
        AnimInfo->SetNumberField(TEXT("numFrames"), Sequence->GetNumberOfSampledKeys());
        AnimInfo->SetNumberField(TEXT("frameRate"), FrameRate.AsDecimal());
        AnimInfo->SetObjectField(TEXT("frameRateRational"), MovieSceneJsonUtils::MakeFrameRateObject(FrameRate));
        AnimInfo->SetNumberField(TEXT("numNotifies"), Sequence->Notifies.Num());
        AnimInfo->SetBoolField(TEXT("isAdditive"), Sequence->AdditiveAnimType != AAT_None);
        AnimInfo->SetStringField(TEXT("additiveType"), AnimationAuthoringHelpers::AdditiveAnimTypeToString(Sequence->AdditiveAnimType));
        AnimInfo->SetBoolField(TEXT("hasRootMotion"), Sequence->bEnableRootMotion);
        int32 RawTrackCount = 0;
        if (DataModel)
        {
            RawTrackCount = DataModel->GetNumBoneTracks();
        }
        AnimInfo->SetNumberField(TEXT("rawTrackCount"), RawTrackCount);
        // Per-bone-track readback alongside the aggregate rawTrackCount: which bones were keyed
        // and how many keys each carries, so bone-track state is verifiable without an
        // asset.dump fallback. Raw bone tracks are not curves and never appear in list_curves.
        AnimInfo->SetArrayField(TEXT("boneTracks"), AnimSequenceDumpBuilder::BuildBoneTracksArrayJson(Sequence));
    }
    else if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
    {
        AnimInfo->SetStringField(TEXT("assetType"), TEXT("AnimMontage"));
        if (Montage->GetSkeleton())
        {
            AnimInfo->SetStringField(TEXT("skeletonPath"), Montage->GetSkeleton()->GetPathName());
        }
        AnimInfo->SetNumberField(TEXT("duration"), Montage->GetPlayLength());
        AnimInfo->SetNumberField(TEXT("numSections"), Montage->CompositeSections.Num());
        AnimInfo->SetNumberField(TEXT("numSlots"), Montage->SlotAnimTracks.Num());
        AnimInfo->SetNumberField(TEXT("numNotifies"), Montage->Notifies.Num());
        // Read-back parity with the asset.dump anim_montage.json sidecar: delegate to the
        // shared dump builder so the live introspection shape matches the on-disk one. Merge
        // every builder field (sections[], slots[], notifies[], blendIn/blendOut, …) that the
        // handler has not already set, so the section names, per-section nextSectionName link
        // map and startTime — the very structure link_sections/set_section_timing/
        // add_montage_section author — become verifiable through get_animation_info without an
        // asset.dump fallback. assetType/skeletonPath/duration/numSections/numSlots/numNotifies
        // above are kept for back-compat and take precedence.
        JsonBuilders::MergeMissingFields(AnimInfo, AnimMontageDumpBuilder::BuildAnimMontageJson(Montage));
    }
    else if (UBlendSpace* BlendSpace = Cast<UBlendSpace>(Asset))
    {
        AnimInfo->SetStringField(TEXT("assetType"), Cast<UBlendSpace1D>(Asset) ? TEXT("BlendSpace1D") : TEXT("BlendSpace2D"));
        if (BlendSpace->GetSkeleton())
        {
            AnimInfo->SetStringField(TEXT("skeletonPath"), BlendSpace->GetSkeleton()->GetPathName());
        }
        AnimInfo->SetNumberField(TEXT("numSamples"), BlendSpace->GetBlendSamples().Num());
        // Read-back parity with the asset.dump blend_space.json sidecar: delegate to the
        // shared dump builder so the live introspection shape matches the on-disk one. Merge
        // every builder field (class, axes[], samples[], interpolation[]) that the handler has
        // not already set, so future BuildBlendSpaceJson fields flow through automatically and
        // the AimOffset-disambiguating "class" field reaches the live readback too.
        // assetType/skeletonPath/numSamples above are kept for back-compat and take precedence.
        JsonBuilders::MergeMissingFields(AnimInfo, BlendSpaceDumpBuilder::BuildBlendSpaceJson(BlendSpace));
    }
    else if (UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Asset))
    {
        AnimInfo->SetStringField(TEXT("assetType"), TEXT("AnimBlueprint"));
        if (AnimBP->TargetSkeleton)
        {
            AnimInfo->SetStringField(TEXT("skeletonPath"), AnimBP->TargetSkeleton->GetPathName());
        }
        AnimInfo->SetStringField(TEXT("parentClass"), AnimBP->ParentClass ? AnimBP->ParentClass->GetName() : TEXT(""));
        // Read-back parity with the asset.dump anim_graph.json sidecar: delegate to the shared
        // dump builder so the live introspection shape matches the on-disk one. Merge every
        // builder field (pages[], state_machines[] with states[]/transitions[]/conduits[],
        // anim_node_classes[]) the handler has not already set, so the AnimGraph structure that
        // add_state_machine/add_state/add_transition/add_slot_node author becomes verifiable
        // through get_animation_info without an asset.dump fallback. This is the last branch of
        // the get_animation_info parity family (AnimSequence/Montage/BlendSpace already merge).
        // assetType/skeletonPath/parentClass above are kept for back-compat and take precedence.
        JsonBuilders::MergeMissingFields(AnimInfo, AnimGraphDumpBuilder::BuildAnimGraphJson(AnimBP));
    }
    else
    {
        AnimInfo->SetStringField(TEXT("assetType"), Asset->GetClass()->GetName());
    }

    Result->SetObjectField(TEXT("animationInfo"), AnimInfo);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), TEXT("Animation info retrieved"));
    Ctx.SendSuccess(Result);
    return true;
}
