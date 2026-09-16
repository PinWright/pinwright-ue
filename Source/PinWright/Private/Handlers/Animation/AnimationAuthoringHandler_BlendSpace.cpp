// Copyright (c) 2026 Alexander Penkin. MIT License.

// AnimationAuthoringHandler_BlendSpace.cpp - split from AnimationAuthoringHandler.cpp
//
// BlendSpace (1D/2D) + AimOffset authoring RPCs under the animation.authoring
// namespace. Cross-cluster helpers live in AnimationAuthoringHelpers.{h,cpp}.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/PackagePathCompose.h"
#include "PinWrightHelpers.h"
#include "PinWrightGlobals.h"
#include "PinWrightSubsystem.h"
#include "Handlers/Asset/AnimSequenceDumpBuilder.h"
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

// ===========================================================================
// 10.3 BLEND SPACES
// ===========================================================================

// Uniquely-named namespace (not anonymous) so this file-local helper never ODR-collides with
// a same-named file static in a sibling handler TU when the unity build merges them.
namespace PinWrightBlendSpaceSampleAdd
{
    // Adds Animation at SampleValue to BlendSpace, capturing UBlendSpace::AddSample's int32
    // return. The engine returns INDEX_NONE when it rejects the sample: a UAimOffsetBlendSpace
    // accepts ONLY rotation-offset-mesh-space additive clips (IsValidAdditiveType), so a
    // non-additive clip is always rejected; a plain blend space rejects a skeleton/additive-type
    // incompatible clip or a coordinate that duplicates an existing sample. On rejection this
    // sends SAMPLE_REJECTED with an actionable hint and returns false; otherwise returns true.
    // Callers MUST NOT report "sample added" when this returns false — discarding this return was
    // the silent-false-success defect (B-blend-space-add-sample-silent-drop).
    static bool AddSampleOrReportRejection(const FHandlerContext& Ctx, UBlendSpace* BlendSpace,
        UAnimSequence* Animation, const FVector& SampleValue, const FString& AssetPath)
    {
        if (BlendSpace->AddSample(Animation, SampleValue) != INDEX_NONE)
        {
            return true;
        }

        const FString Cause = BlendSpace->IsA<UAimOffsetBlendSpace>()
            ? TEXT("an aim offset accepts only a rotation-offset-mesh-space additive clip (AAT_RotationOffsetMeshSpace) on a compatible skeleton, and the coordinate must be in range and not duplicate an existing sample")
            : TEXT("the clip's skeleton or additive type is incompatible with the blend space, or the coordinate duplicates an existing sample");
        Ctx.SendError(TEXT("SAMPLE_REJECTED"),
            FString::Printf(TEXT("Sample not added to '%s': the engine rejected it because %s."),
                *AssetPath, *Cause));
        return false;
    }
}

REGISTER_RPC_HANDLER("animation.authoring.create_blend_space_1d", "animation.authoring",
    "Create a 1D blend space asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the blend space"),
        RPC_PARAM_OPT("path", "path", "Save path (default /Game/Animations)"),
        RPC_PARAM_REQ("skeletonPath", "path", "Path to the skeleton asset"),
        RPC_PARAM_OPT("axisName", "string", "Axis display name (default Speed)"),
        RPC_PARAM_OPT("axisMin", "number", "Axis minimum (default 0)"),
        RPC_PARAM_OPT("axisMax", "number", "Axis maximum (default 600)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
#if MCP_HAS_BLENDSPACE_FACTORY
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Animations")));
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    FString AxisName = Ctx.GetString(TEXT("axisName"), TEXT("Speed"));
    float AxisMin = static_cast<float>(Ctx.GetNumber(TEXT("axisMin"), 0.0));
    float AxisMax = static_cast<float>(Ctx.GetNumber(TEXT("axisMax"), 600.0));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_NAME"), TEXT("Name is required"));
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
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("%s Pass a bare asset name and choose the folder with path "
                                 "(default /Game/Animations)."), *PathError));
        return true;
    }

    USkeleton* Skeleton = AnimationAuthoringHelpers::LoadSkeletonFromPathAnim(SkeletonPath);
    if (!Skeleton)
    {
        Ctx.SendError(TEXT("SKELETON_NOT_FOUND"), FString::Printf(TEXT("Could not load skeleton: %s"), *SkeletonPath));
        return true;
    }

    // Create package and asset directly to avoid UI dialogs
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package"));
        return true;
    }

    UBlendSpaceFactory1D* Factory = NewObject<UBlendSpaceFactory1D>();
    Factory->TargetSkeleton = Skeleton;
    UBlendSpace1D* NewBlendSpace = Cast<UBlendSpace1D>(
        Factory->FactoryCreateNew(UBlendSpace1D::StaticClass(), Package,
                                  FName(*Name), RF_Public | RF_Standalone,
                                  nullptr, GWarn));
    if (!NewBlendSpace)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create blend space 1D"));
        return true;
    }

    // Configure axis - use reflection since BlendParameters is protected
    FBlendParameter NewParam;
    NewParam.DisplayName = AxisName;
    NewParam.Min = AxisMin;
    NewParam.Max = AxisMax;
    NewParam.GridNum = 4;
    NewParam.bSnapToGrid = false;
    NewParam.bWrapInput = false;

    // Write the protected BlendParameters array via the shared reflection helper
    NewBlendSpace->Modify();
    if (FBlendParameter* BlendParamsPtr = AnimationAuthoringHelpers::GetBlendParametersForWrite(NewBlendSpace))
    {
        BlendParamsPtr[0] = NewParam;
    }

    NewBlendSpace->PostEditChange();

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(NewBlendSpace, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), Path / Name);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Blend Space 1D '%s' created"), *Name));
    AddAssetVerification(Result, NewBlendSpace);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("NOT_SUPPORTED"), TEXT("Blend space factory not available"));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.create_blend_space_2d", "animation.authoring",
    "Create a 2D blend space asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the blend space"),
        RPC_PARAM_OPT("path", "path", "Save path (default /Game/Animations)"),
        RPC_PARAM_REQ("skeletonPath", "path", "Path to the skeleton asset"),
        RPC_PARAM_OPT("horizontalAxisName", "string", "Horizontal axis name (default Direction)"),
        RPC_PARAM_OPT("horizontalMin", "number", "Horizontal min (default -180)"),
        RPC_PARAM_OPT("horizontalMax", "number", "Horizontal max (default 180)"),
        RPC_PARAM_OPT("verticalAxisName", "string", "Vertical axis name (default Speed)"),
        RPC_PARAM_OPT("verticalMin", "number", "Vertical min (default 0)"),
        RPC_PARAM_OPT("verticalMax", "number", "Vertical max (default 600)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
#if MCP_HAS_BLENDSPACE_FACTORY
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Animations")));
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    FString HorizontalAxisName = Ctx.GetString(TEXT("horizontalAxisName"), TEXT("Direction"));
    float HorizontalMin = static_cast<float>(Ctx.GetNumber(TEXT("horizontalMin"), -180.0));
    float HorizontalMax = static_cast<float>(Ctx.GetNumber(TEXT("horizontalMax"), 180.0));
    FString VerticalAxisName = Ctx.GetString(TEXT("verticalAxisName"), TEXT("Speed"));
    float VerticalMin = static_cast<float>(Ctx.GetNumber(TEXT("verticalMin"), 0.0));
    float VerticalMax = static_cast<float>(Ctx.GetNumber(TEXT("verticalMax"), 600.0));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_NAME"), TEXT("Name is required"));
        return true;
    }

    // Composed and checked above the skeleton load, for the reason spelled out at
    // create_blend_space_1d: an unvalidated `name` reaches CreatePackage's Fatal and ends the
    // editor process, and this ordering is what keeps the regression test off that path.
    FString PackagePath;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(Path, Name, PackagePath, PathError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("%s Pass a bare asset name and choose the folder with path "
                                 "(default /Game/Animations)."), *PathError));
        return true;
    }

    USkeleton* Skeleton = AnimationAuthoringHelpers::LoadSkeletonFromPathAnim(SkeletonPath);
    if (!Skeleton)
    {
        Ctx.SendError(TEXT("SKELETON_NOT_FOUND"), FString::Printf(TEXT("Could not load skeleton: %s"), *SkeletonPath));
        return true;
    }

    // Create package and asset directly to avoid UI dialogs
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package"));
        return true;
    }

    UBlendSpaceFactoryNew* Factory = NewObject<UBlendSpaceFactoryNew>();
    Factory->TargetSkeleton = Skeleton;
    UBlendSpace* NewBlendSpace = Cast<UBlendSpace>(
        Factory->FactoryCreateNew(UBlendSpace::StaticClass(), Package,
                                  FName(*Name), RF_Public | RF_Standalone,
                                  nullptr, GWarn));
    if (!NewBlendSpace)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create blend space 2D"));
        return true;
    }

    // Configure axes using reflection since BlendParameters is protected in UE 5.7+
    FBlendParameter HParam;
    HParam.DisplayName = HorizontalAxisName;
    HParam.Min = HorizontalMin;
    HParam.Max = HorizontalMax;
    HParam.GridNum = 4;
    HParam.bSnapToGrid = false;
    HParam.bWrapInput = false;

    FBlendParameter VParam;
    VParam.DisplayName = VerticalAxisName;
    VParam.Min = VerticalMin;
    VParam.Max = VerticalMax;
    VParam.GridNum = 4;
    VParam.bSnapToGrid = false;
    VParam.bWrapInput = false;

    // Write the protected BlendParameters array via the shared reflection helper
    NewBlendSpace->Modify();
    if (FBlendParameter* BlendParamsPtr = AnimationAuthoringHelpers::GetBlendParametersForWrite(NewBlendSpace))
    {
        BlendParamsPtr[0] = HParam;
        BlendParamsPtr[1] = VParam;
    }

    NewBlendSpace->PostEditChange();

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(NewBlendSpace, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), Path / Name);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Blend Space 2D '%s' created"), *Name));
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("NOT_SUPPORTED"), TEXT("Blend space factory not available"));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_blend_sample", "animation.authoring",
    "Add an animation sample to a blend space",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the blend space"),
        RPC_PARAM_REQ("animationPath", "path", "Path to the animation"),
        RPC_PARAM_OPT("sampleValue", "number", "1D sample value or {x,y} object for 2D"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    const TSharedPtr<FJsonObject>& Params = Ctx.GetRawPayload();
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString AnimationPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("animationPath")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UBlendSpace* BlendSpace2D = Cast<UBlendSpace>(StaticLoadObject(UBlendSpace::StaticClass(), nullptr, *AssetPath));
    UBlendSpace1D* BlendSpace1D = Cast<UBlendSpace1D>(StaticLoadObject(UBlendSpace1D::StaticClass(), nullptr, *AssetPath));

    UBlendSpace* BlendSpace = BlendSpace2D ? BlendSpace2D : BlendSpace1D;
    if (!BlendSpace)
    {
        Ctx.SendError(TEXT("BLENDSPACE_NOT_FOUND"), FString::Printf(TEXT("Could not load blend space: %s"), *AssetPath));
        return true;
    }

    UAnimSequence* Animation = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(AnimationPath);
    if (!Animation)
    {
        Ctx.SendError(TEXT("ANIMATION_NOT_FOUND"), FString::Printf(TEXT("Could not load animation: %s"), *AnimationPath));
        return true;
    }

    // Get sample value
    FVector SampleValue = FVector::ZeroVector;
    if (Params->HasField(TEXT("sampleValue")))
    {
        TSharedPtr<FJsonValue> SampleVal = Params->TryGetField(TEXT("sampleValue"));
        if (SampleVal.IsValid())
        {
            if (SampleVal->Type == EJson::Number)
            {
                // 1D blend space
                SampleValue.X = SampleVal->AsNumber();
            }
            else if (SampleVal->Type == EJson::Object)
            {
                // 2D blend space
                TSharedPtr<FJsonObject> SampleObj = SampleVal->AsObject();
                SampleValue.X = GetJsonNumberField(SampleObj, TEXT("x"), 0.0);
                SampleValue.Y = GetJsonNumberField(SampleObj, TEXT("y"), 0.0);
            }
        }
    }

    // Add sample — capture AddSample's int32 return; INDEX_NONE means the engine rejected it.
    if (!PinWrightBlendSpaceSampleAdd::AddSampleOrReportRejection(Ctx, BlendSpace, Animation, SampleValue, AssetPath))
    {
        return true;
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(BlendSpace, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), TEXT("Blend sample added"));
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_interpolation_settings", "animation.authoring",
    "Configure interpolation settings on a blend space",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the blend space"),
        RPC_PARAM_OPT("interpolationType", "string", "Interpolation type (default Lerp)"),
        RPC_PARAM_OPT("targetWeightInterpolationSpeed", "number", "Target weight speed (default 5)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    const TSharedPtr<FJsonObject>& Params = Ctx.GetRawPayload();
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UBlendSpace* BlendSpace2D = Cast<UBlendSpace>(StaticLoadObject(UBlendSpace::StaticClass(), nullptr, *AssetPath));
    UBlendSpace1D* BlendSpace1D = Cast<UBlendSpace1D>(StaticLoadObject(UBlendSpace1D::StaticClass(), nullptr, *AssetPath));

    UBlendSpace* BlendSpace = BlendSpace2D ? BlendSpace2D : BlendSpace1D;
    if (!BlendSpace)
    {
        Ctx.SendError(TEXT("BLENDSPACE_NOT_FOUND"), FString::Printf(TEXT("Could not load blend space: %s"), *AssetPath));
        return true;
    }

    // Apply the interpolation curve type to every input axis, but only when the
    // caller actually supplied it. The type lives in InterpolationParam[axis]
    // (EFilterInterpolationType), NOT in TargetWeightInterpolationSpeedPerSec.
    if (Params->HasField(TEXT("interpolationType")))
    {
        auto ParseInterpolationType = [](const FString& In, EFilterInterpolationType& Out) -> bool
        {
            const FString N = In.Replace(TEXT(" "), TEXT("")).Replace(TEXT("/"), TEXT(""));
            if (N.Equals(TEXT("Averaged"), ESearchCase::IgnoreCase) || N.Equals(TEXT("Average"), ESearchCase::IgnoreCase)) { Out = BSIT_Average; return true; }
            if (N.Equals(TEXT("Linear"), ESearchCase::IgnoreCase) || N.Equals(TEXT("Lerp"), ESearchCase::IgnoreCase)) { Out = BSIT_Linear; return true; }
            if (N.Equals(TEXT("Cubic"), ESearchCase::IgnoreCase)) { Out = BSIT_Cubic; return true; }
            if (N.Equals(TEXT("EaseInOut"), ESearchCase::IgnoreCase)) { Out = BSIT_EaseInOut; return true; }
            if (N.Equals(TEXT("Exponential"), ESearchCase::IgnoreCase) || N.Equals(TEXT("ExponentialDecay"), ESearchCase::IgnoreCase)) { Out = BSIT_ExponentialDecay; return true; }
            if (N.Equals(TEXT("SpringDamper"), ESearchCase::IgnoreCase)) { Out = BSIT_SpringDamper; return true; }
            return false;
        };

        const FString InterpolationType = Ctx.GetString(TEXT("interpolationType"));
        EFilterInterpolationType ParsedType = BSIT_Average;
        if (!ParseInterpolationType(InterpolationType, ParsedType))
        {
            Ctx.SendError(TEXT("INVALID_INTERPOLATION_TYPE"),
                FString::Printf(TEXT("Unknown interpolationType '%s' (expected Averaged, Linear, Cubic, EaseInOut, Exponential, or SpringDamper)"), *InterpolationType));
            return true;
        }
        for (FInterpolationParameter& AxisParam : BlendSpace->InterpolationParam)
        {
            AxisParam.InterpolationType = ParsedType;
        }
    }

    // Only overwrite the target-weight interpolation speed when the caller asked
    // for it (it previously clobbered to the default 5.0 on every call).
    if (Params->HasField(TEXT("targetWeightInterpolationSpeed")))
    {
        BlendSpace->TargetWeightInterpolationSpeedPerSec =
            static_cast<float>(Ctx.GetNumber(TEXT("targetWeightInterpolationSpeed"), 5.0));
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(BlendSpace, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), TEXT("Interpolation settings updated"));
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.create_aim_offset", "animation.authoring",
    "Create an aim offset blend space",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the aim offset"),
        RPC_PARAM_OPT("path", "path", "Save path (default /Game/Animations)"),
        RPC_PARAM_REQ("skeletonPath", "path", "Path to the skeleton asset"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
#if MCP_HAS_BLENDSPACE_FACTORY
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Animations")));
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_NAME"), TEXT("Name is required"));
        return true;
    }

    // Composed and checked above the skeleton load, for the reason spelled out at
    // create_blend_space_1d: an unvalidated `name` reaches CreatePackage's Fatal and ends the
    // editor process, and this ordering is what keeps the regression test off that path.
    FString PackagePath;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(Path, Name, PackagePath, PathError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("%s Pass a bare asset name and choose the folder with path "
                                 "(default /Game/Animations)."), *PathError));
        return true;
    }

    USkeleton* Skeleton = AnimationAuthoringHelpers::LoadSkeletonFromPathAnim(SkeletonPath);
    if (!Skeleton)
    {
        Ctx.SendError(TEXT("SKELETON_NOT_FOUND"), FString::Printf(TEXT("Could not load skeleton: %s"), *SkeletonPath));
        return true;
    }

    // Create package and asset directly to avoid UI dialogs
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package"));
        return true;
    }

    UBlendSpaceFactoryNew* Factory = NewObject<UBlendSpaceFactoryNew>();
    Factory->TargetSkeleton = Skeleton;
    UAimOffsetBlendSpace* NewAimOffset = Cast<UAimOffsetBlendSpace>(
        Factory->FactoryCreateNew(UAimOffsetBlendSpace::StaticClass(), Package,
                                  FName(*Name), RF_Public | RF_Standalone,
                                  nullptr, GWarn));
    if (!NewAimOffset)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create aim offset"));
        return true;
    }

    // Configure default aim offset axes (Yaw and Pitch)
    NewAimOffset->PostEditChange();
    NewAimOffset->MarkPackageDirty();

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(NewAimOffset, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), Path / Name);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Aim Offset '%s' created"), *Name));
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("NOT_SUPPORTED"), TEXT("Blend space factory not available"));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_aim_offset_sample", "animation.authoring",
    "Add an animation sample to an aim offset",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the aim offset"),
        RPC_PARAM_REQ("animationPath", "path", "Path to the animation"),
        RPC_PARAM_OPT("yaw", "number", "Yaw value (default 0)"),
        RPC_PARAM_OPT("pitch", "number", "Pitch value (default 0)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString AnimationPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("animationPath")));
    float Yaw = static_cast<float>(Ctx.GetNumber(TEXT("yaw"), 0.0));
    float Pitch = static_cast<float>(Ctx.GetNumber(TEXT("pitch"), 0.0));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAimOffsetBlendSpace* AimOffset = Cast<UAimOffsetBlendSpace>(StaticLoadObject(UAimOffsetBlendSpace::StaticClass(), nullptr, *AssetPath));
    if (!AimOffset)
    {
        // Try as regular blend space
        UBlendSpace* BlendSpace = Cast<UBlendSpace>(StaticLoadObject(UBlendSpace::StaticClass(), nullptr, *AssetPath));
        if (BlendSpace)
        {
            UAnimSequence* Animation = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(AnimationPath);
            if (!Animation)
            {
                Ctx.SendError(TEXT("ANIMATION_NOT_FOUND"), FString::Printf(TEXT("Could not load animation: %s"), *AnimationPath));
                return true;
            }

            FVector SampleValue(Yaw, Pitch, 0.0f);
            if (!PinWrightBlendSpaceSampleAdd::AddSampleOrReportRejection(Ctx, BlendSpace, Animation, SampleValue, AssetPath))
            {
                return true;
            }

            EAssetSaveState SaveState = EAssetSaveState::NotRequested;
            AnimationAuthoringHelpers::SaveAnimAsset(BlendSpace, bSave, SaveState);

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
            Ctx.SendSuccess(TEXT("Aim offset sample added"), Result);
            return true;
        }

        Ctx.SendError(TEXT("AIMOFFSET_NOT_FOUND"), FString::Printf(TEXT("Could not load aim offset: %s"), *AssetPath));
        return true;
    }

    UAnimSequence* Animation = AnimationAuthoringHelpers::LoadAnimSequenceFromPath(AnimationPath);
    if (!Animation)
    {
        Ctx.SendError(TEXT("ANIMATION_NOT_FOUND"), FString::Printf(TEXT("Could not load animation: %s"), *AnimationPath));
        return true;
    }

    // Add sample with yaw/pitch coordinates — INDEX_NONE means the engine rejected the clip
    // (an aim offset always rejects a non-additive clip).
    FVector SampleValue(Yaw, Pitch, 0.0f);
    if (!PinWrightBlendSpaceSampleAdd::AddSampleOrReportRejection(Ctx, AimOffset, Animation, SampleValue, AssetPath))
    {
        return true;
    }

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AimOffset, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(TEXT("Aim offset sample added"), Result);
    return true;
}
