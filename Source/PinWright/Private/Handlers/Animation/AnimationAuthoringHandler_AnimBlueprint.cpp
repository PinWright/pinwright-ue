// Copyright (c) 2026 Alexander Penkin. MIT License.

// AnimationAuthoringHandler_AnimBlueprint.cpp - split from AnimationAuthoringHandler.cpp
//
// Animation Blueprint + State Machine + AnimGraph node + IK + ControlRig +
// IK Rig / Retargeter authoring RPCs under the animation.authoring namespace.
// Cross-cluster helpers live in AnimationAuthoringHelpers.{h,cpp}; the
// cluster-bound bone/sync-group helpers below stay file-local here.

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
#include "Utils/AssetUtils.h"

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

// IK Rig support (UE 5.0+). The public header ships at IKRig/Public/Rig/IKRigDefinition.h
// and the module exposes no extra PublicIncludePaths, so cross-module consumers (us) must
// include it as "Rig/IKRigDefinition.h" — the bare "IKRigDefinition.h" form only resolves
// intra-module. Prefer the prefixed path so the gate actually flips when the IKRig Build.cs
// dependency is present; keep the bare form as a fallback for any older layout.
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

// IK Rig editor controller (UIKRigController::AddRetargetChain) — drives real chain
// authoring on a UIKRigDefinition so add_ik_chain mutates the asset instead of echoing.
#if __has_include("RigEditor/IKRigController.h")
#include "RigEditor/IKRigController.h"
#define MCP_HAS_IKRIG_CONTROLLER 1
#else
#define MCP_HAS_IKRIG_CONTROLLER 0
#endif

// IK Retargeter editor controller (UIKRetargeterController::SetSourceChain) — drives the
// op-stack-scoped source->target chain mapping for set_retarget_chain_mapping.
#if __has_include("RetargetEditor/IKRetargeterController.h")
#include "RetargetEditor/IKRetargeterController.h"
#define MCP_HAS_IKRETARGET_CONTROLLER 1
#else
#define MCP_HAS_IKRETARGET_CONTROLLER 0
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
#include "Handlers/Animation/AnimGraphNodeAccessor.h"
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

#if MCP_HAS_TWO_BONE_IK || MCP_HAS_MODIFY_BONE
static bool AnimSkeletonHasBone(UAnimBlueprint* AnimBP, const FString& BoneName)
{
    if (!AnimBP || !AnimBP->TargetSkeleton || BoneName.IsEmpty())
    {
        return false;
    }
    return AnimBP->TargetSkeleton->GetReferenceSkeleton().FindBoneIndex(FName(*BoneName)) != INDEX_NONE;
}

static bool ParseBoneControlSpace(const FString& Text, EBoneControlSpace& OutSpace)
{
    FString SpaceText = Text;
    SpaceText.TrimStartAndEndInline();
    if (SpaceText.IsEmpty())
    {
        OutSpace = BCS_ComponentSpace;
        return true;
    }

    if (SpaceText.Equals(TEXT("World"), ESearchCase::IgnoreCase))
    {
        SpaceText = TEXT("WorldSpace");
    }
    else if (SpaceText.Equals(TEXT("Component"), ESearchCase::IgnoreCase))
    {
        SpaceText = TEXT("ComponentSpace");
    }
    else if (SpaceText.Equals(TEXT("ParentBone"), ESearchCase::IgnoreCase))
    {
        SpaceText = TEXT("ParentBoneSpace");
    }
    else if (SpaceText.Equals(TEXT("Bone"), ESearchCase::IgnoreCase))
    {
        SpaceText = TEXT("BoneSpace");
    }

    int64 ResolvedValue = INDEX_NONE;
    const UEnum* EnumType = StaticEnum<EBoneControlSpace>();
    if (!BlueprintEnumHelpers::TryResolveEnumLiteralToValue(EnumType, SpaceText, ResolvedValue) &&
        !SpaceText.StartsWith(TEXT("BCS_"), ESearchCase::IgnoreCase))
    {
        BlueprintEnumHelpers::TryResolveEnumLiteralToValue(
            EnumType, FString::Printf(TEXT("BCS_%s"), *SpaceText), ResolvedValue);
    }

    if (ResolvedValue != INDEX_NONE)
    {
        OutSpace = static_cast<EBoneControlSpace>(ResolvedValue);
        return true;
    }
    return false;
}

static FString BoneControlSpaceToString(EBoneControlSpace Space)
{
    switch (Space)
    {
    case BCS_WorldSpace:
        return TEXT("BCS_WorldSpace");
    case BCS_ParentBoneSpace:
        return TEXT("BCS_ParentBoneSpace");
    case BCS_BoneSpace:
        return TEXT("BCS_BoneSpace");
    case BCS_ComponentSpace:
    default:
        return TEXT("BCS_ComponentSpace");
    }
}
#endif

#if MCP_HAS_MODIFY_BONE
static bool ParseBoneModificationMode(const FString& Text, EBoneModificationMode& OutMode)
{
    FString ModeText = Text;
    ModeText.TrimStartAndEndInline();
    if (ModeText.Equals(TEXT("Ignore"), ESearchCase::IgnoreCase))
    {
        ModeText = TEXT("BMM_Ignore");
    }
    else if (ModeText.Equals(TEXT("Replace"), ESearchCase::IgnoreCase))
    {
        ModeText = TEXT("BMM_Replace");
    }
    else if (ModeText.Equals(TEXT("Additive"), ESearchCase::IgnoreCase))
    {
        ModeText = TEXT("BMM_Additive");
    }

    int64 ResolvedValue = INDEX_NONE;
    if (BlueprintEnumHelpers::TryResolveEnumLiteralToValue(
        StaticEnum<EBoneModificationMode>(), ModeText, ResolvedValue))
    {
        OutMode = static_cast<EBoneModificationMode>(ResolvedValue);
        return true;
    }
    return false;
}

static FString BoneModificationModeToString(EBoneModificationMode Mode)
{
    switch (Mode)
    {
    case BMM_Replace:
        return TEXT("Replace");
    case BMM_Additive:
        return TEXT("Additive");
    case BMM_Ignore:
    default:
        return TEXT("Ignore");
    }
}
#endif

#if MCP_HAS_ASSET_PLAYER_BASE
static bool ParseAnimSyncGroupRole(const FString& Text, EAnimGroupRole::Type& OutRole, bool& bOutStandalone)
{
    FString RoleText = Text;
    RoleText.TrimStartAndEndInline();
    if (RoleText.IsEmpty())
    {
        RoleText = TEXT("CanBeLeader");
    }

    bOutStandalone = false;
    if (RoleText.Equals(TEXT("Standalone"), ESearchCase::IgnoreCase))
    {
        OutRole = EAnimGroupRole::CanBeLeader;
        bOutStandalone = true;
        return true;
    }

    int64 ResolvedValue = INDEX_NONE;
    if (BlueprintEnumHelpers::TryResolveEnumLiteralToValue(
        StaticEnum<EAnimGroupRole::Type>(), RoleText, ResolvedValue))
    {
        OutRole = static_cast<EAnimGroupRole::Type>(ResolvedValue);
        return true;
    }
    return false;
}

static FString AnimSyncGroupRoleToString(EAnimGroupRole::Type Role)
{
    if (const UEnum* EnumType = StaticEnum<EAnimGroupRole::Type>())
    {
        return EnumType->GetNameStringByValue(static_cast<int64>(Role));
    }
    return TEXT("CanBeLeader");
}

// Sends the standard AMBIGUOUS_NODE error for a `nodeName` that matched more
// than one AnimGraph node. Single owner of the error code + message wording +
// candidate-join format so every `nodeName` mutator reports ambiguity
// identically; both ResolveAnimNodeOrSendError and the handlers that resolve a
// node by hand (e.g. set_layered_blend_layers, which needs its own
// WRONG_NODE_TYPE branch) route their ambiguous case through here.
static void SendAmbiguousNodeError(FHandlerContext& Ctx, const FString& NodeName, const TArray<FString>& Candidates)
{
    Ctx.SendError(TEXT("AMBIGUOUS_NODE"), FString::Printf(
        TEXT("nodeName '%s' matched %d AnimGraph nodes; pass a more specific substring or the exact node title. Candidates: %s"),
        *NodeName, Candidates.Num(), *FString::Join(Candidates, TEXT(", "))));
}

// Resolves a `nodeName` to a single AnimGraph node via the shared two-pass
// exact-then-substring resolver, sending the standard error on miss/ambiguity
// and returning nullptr in that case. On success the resolved node's actual
// list-view title is written to OutResolvedTitle so the handler can echo the
// real node it touched rather than the raw input substring (the verbatim-echo
// hazard). Centralises the NODE_NOT_FOUND / AMBIGUOUS_NODE handling shared by
// every animation.authoring `nodeName` mutator.
static UAnimGraphNode_Base* ResolveAnimNodeOrSendError(
    FHandlerContext& Ctx,
    UEdGraph* Graph,
    const FString& NodeName,
    FString& OutResolvedTitle)
{
    const AnimGraphConstructionUtils::FAnimNodeResolveResult Resolved =
        AnimGraphConstructionUtils::ResolveAnimGraphNodeByTitle(Graph, NodeName);

    if (Resolved.Status == AnimGraphConstructionUtils::EAnimNodeResolveStatus::Ambiguous)
    {
        SendAmbiguousNodeError(Ctx, NodeName, Resolved.Candidates);
        return nullptr;
    }
    if (Resolved.Status != AnimGraphConstructionUtils::EAnimNodeResolveStatus::Found || !Resolved.Node)
    {
        Ctx.SendError(TEXT("NODE_NOT_FOUND"), FString::Printf(TEXT("Node '%s' not found in AnimGraph"), *NodeName));
        return nullptr;
    }

    OutResolvedTitle = Resolved.Candidates.Num() > 0 ? Resolved.Candidates[0] : NodeName;
    return Resolved.Node;
}

static bool ApplyAssetPlayerSyncGroup(
    UAnimGraphNode_Base* AnimNode,
    const FString& NodeName,
    const FString& GroupNameText,
    const FString& RoleText,
    FString& OutErrorCode,
    FString& OutErrorMessage,
    FName& OutAppliedGroupName,
    EAnimGroupRole::Type& OutAppliedRole,
    EAnimSyncMethod& OutAppliedMethod)
{
    UAnimGraphNode_AssetPlayerBase* AssetPlayerNode = Cast<UAnimGraphNode_AssetPlayerBase>(AnimNode);
    if (!AssetPlayerNode)
    {
        const FString ActualClass = AnimNode ? AnimNode->GetClass()->GetName() : FString(TEXT("<null>"));
        OutErrorCode = TEXT("NODE_NOT_ASSET_PLAYER");
        OutErrorMessage = FString::Printf(TEXT("Node '%s' is %s, not a UAnimGraphNode_AssetPlayerBase"),
            *NodeName, *ActualClass);
        return false;
    }

    EAnimGroupRole::Type ParsedRole = EAnimGroupRole::CanBeLeader;
    bool bStandalone = false;
    if (!ParseAnimSyncGroupRole(RoleText, ParsedRole, bStandalone))
    {
        OutErrorCode = TEXT("INVALID_SYNC_GROUP_ROLE");
        OutErrorMessage = FString::Printf(
            TEXT("Unknown sync group role '%s'. Expected an EAnimGroupRole literal or Standalone"),
            *RoleText);
        return false;
    }

    FString TrimmedGroupName = GroupNameText;
    TrimmedGroupName.TrimStartAndEndInline();
    const FName NewGroupName = bStandalone ? NAME_None : FName(*TrimmedGroupName);
    if (NewGroupName.IsNone() && !bStandalone)
    {
        OutErrorCode = TEXT("INVALID_SYNC_GROUP_PAIR");
        OutErrorMessage = TEXT("groupName cannot be empty unless role is Standalone");
        return false;
    }

    FStructProperty* NodeProperty = PinWright::Anim::GetFNodeProperty(AssetPlayerNode);
    if (!NodeProperty || !NodeProperty->Struct ||
        !NodeProperty->Struct->IsChildOf(FAnimNode_AssetPlayerBase::StaticStruct()))
    {
        OutErrorCode = TEXT("NODE_NOT_ASSET_PLAYER");
        OutErrorMessage = FString::Printf(TEXT("Node '%s' is %s, but its runtime node is not FAnimNode_AssetPlayerBase"),
            *NodeName, *AssetPlayerNode->GetClass()->GetName());
        return false;
    }

    FAnimNode_AssetPlayerBase* RuntimeNode =
        NodeProperty->ContainerPtrToValuePtr<FAnimNode_AssetPlayerBase>(AssetPlayerNode);
    if (!RuntimeNode)
    {
        OutErrorCode = TEXT("SYNC_GROUP_WRITE_FAILED");
        OutErrorMessage = FString::Printf(TEXT("Could not access runtime asset-player node for '%s'"), *NodeName);
        return false;
    }

    const EAnimSyncMethod NewMethod = bStandalone
        ? EAnimSyncMethod::DoNotSync
        : EAnimSyncMethod::SyncGroup;
    const bool bSetGroupName = RuntimeNode->SetGroupName(NewGroupName);
    const bool bSetGroupRole = RuntimeNode->SetGroupRole(ParsedRole);
    const bool bSetGroupMethod = RuntimeNode->SetGroupMethod(NewMethod);
    if (!bSetGroupName || !bSetGroupRole || !bSetGroupMethod)
    {
        OutErrorCode = TEXT("SYNC_GROUP_WRITE_FAILED");
        OutErrorMessage = FString::Printf(
            TEXT("Node '%s' does not support all sync-group setters (SetGroupName=%s, SetGroupRole=%s, SetGroupMethod=%s)"),
            *NodeName,
            bSetGroupName ? TEXT("true") : TEXT("false"),
            bSetGroupRole ? TEXT("true") : TEXT("false"),
            bSetGroupMethod ? TEXT("true") : TEXT("false"));
        return false;
    }

    OutAppliedGroupName = RuntimeNode->GetGroupName();
    OutAppliedRole = RuntimeNode->GetGroupRole();
    OutAppliedMethod = RuntimeNode->GetGroupMethod();
    if (OutAppliedGroupName != NewGroupName || OutAppliedRole != ParsedRole || OutAppliedMethod != NewMethod)
    {
        OutErrorCode = TEXT("SYNC_GROUP_WRITE_FAILED");
        OutErrorMessage = FString::Printf(TEXT("Node '%s' did not retain the requested sync-group values"), *NodeName);
        return false;
    }
    return true;
}
#endif

// ===========================================================================
// 10.4 ANIMATION BLUEPRINTS
// ===========================================================================

REGISTER_RPC_HANDLER("animation.authoring.create_anim_blueprint", "animation.authoring",
    "Create a UAnimBlueprint asset bound to a Skeleton (authoring path with explicit parentClass override). Largely overlaps with animation.create_animation_bp; pick this entry when you want full control over save path and parent.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset filename (no extension)."),
        RPC_PARAM_OPT("path", "path", "Content-browser folder; defaults to /Game/Blueprints."),
        RPC_PARAM_REQ("skeletonPath", "path", "Asset path to the bound Skeleton."),
        RPC_PARAM_OPT("parentClass", "classref", "Path or short name of an AnimInstance subclass; defaults to 'AnimInstance'."),
        RPC_PARAM_OPT("save", "boolean", "When true (default) marks the new asset dirty so the next editor save persists it.")
    ))
{
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Blueprints")));
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    FString ParentClass = Ctx.GetString(TEXT("parentClass"), TEXT("AnimInstance"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_NAME"), TEXT("Name is required"));
        return true;
    }

    USkeleton* Skeleton = AnimationAuthoringHelpers::LoadSkeletonFromPathAnim(SkeletonPath);
    if (!Skeleton)
    {
        Ctx.SendError(TEXT("SKELETON_NOT_FOUND"), FString::Printf(TEXT("Could not load skeleton: %s"), *SkeletonPath));
        return true;
    }

    // Create package and asset directly to avoid UI dialogs. The shared helper
    // performs the full duplicate-name guard around CreatePackage: a UBlueprint
    // of this name already existing (on disk or in memory) would otherwise trip
    // the fatal check inside FKismetEditorUtilities::CreateBlueprint and crash
    // the editor (see PrepareBlueprintPackageGuardingNameCollision).
    FString PackagePath = Path / Name;
    UPackage* Package = nullptr;
    FString AssetObjectPath;
    bool bNameCollision = false;
    FString GuardError;
    if (!PrepareBlueprintPackageGuardingNameCollision(PackagePath, Name, Package,
                                                      AssetObjectPath, bNameCollision, GuardError))
    {
        if (bNameCollision)
        {
            Ctx.SendError(TEXT("ASSET_EXISTS"),
                FString::Printf(TEXT("An Animation Blueprint already exists at: %s"), *AssetObjectPath));
        }
        else
        {
            Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package"));
        }
        return true;
    }

    UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
    Factory->TargetSkeleton = Skeleton;
    Factory->ParentClass = UAnimInstance::StaticClass();
    UAnimBlueprint* NewAnimBP = Cast<UAnimBlueprint>(
        Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), Package,
                                  FName(*Name), RF_Public | RF_Standalone,
                                  nullptr, GWarn));
    if (!NewAnimBP)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create animation blueprint"));
        return true;
    }

    // Compile the blueprint
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(NewAnimBP);

    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(NewAnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), Path / Name);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Animation Blueprint '%s' created"), *Name));
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_state_machine", "animation.authoring",
    "Add a state machine to an animation blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_REQ("stateMachineName", "string", "Name of the state machine"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString StateMachineName = Ctx.GetString(TEXT("stateMachineName"));
    double XD = 0.0, YD = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), XD)) return true;
    if (!Ctx.RequireNumber(TEXT("y"), YD)) return true;
    int32 NodePosX = static_cast<int32>(XD);
    int32 NodePosY = static_cast<int32>(YD);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (StateMachineName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_STATE_MACHINE_NAME"), TEXT("stateMachineName is required"));
        return true;
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH && MCP_HAS_ANIM_STATE_MACHINE_SCHEMA
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Could not find AnimGraph in blueprint"));
        return true;
    }

    UAnimGraphNode_StateMachine* SMNode = AnimGraphConstructionUtils::CreateStateMachine(
        AnimBP, AnimGraph, FName(*StateMachineName), FVector2D(NodePosX, NodePosY));
    if (!SMNode)
    {
        Ctx.SendError(TEXT("STATE_MACHINE_CREATE_FAILED"), FString::Printf(TEXT("Failed to create state machine '%s'"), *StateMachineName));
        return true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeName"), StateMachineName);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("State machine '%s' created with entry node"), *StateMachineName));
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    // AnimGraph headers not available - return error instead of fake success
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        FString::Printf(TEXT("Cannot create state machine '%s': AnimGraph module headers not available in this build. Rebuild with AnimGraph module enabled."), *StateMachineName));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_state", "animation.authoring",
    "Add a state to a state machine in an animation blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_REQ("stateMachineName", "string", "Name of the state machine"),
        RPC_PARAM_REQ("stateName", "string", "Name of the state to add"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString StateMachineName = Ctx.GetString(TEXT("stateMachineName"));
    FString StateName = Ctx.GetString(TEXT("stateName"));
    double XD = 0.0, YD = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), XD)) return true;
    if (!Ctx.RequireNumber(TEXT("y"), YD)) return true;
    int32 NodePosX = static_cast<int32>(XD);
    int32 NodePosY = static_cast<int32>(YD);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (StateName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_STATE_NAME"), TEXT("stateName is required"));
        return true;
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH && MCP_HAS_ANIM_STATE_MACHINE_SCHEMA
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Could not find AnimGraph in blueprint"));
        return true;
    }

    UAnimGraphNode_StateMachine* SMNode = AnimGraphConstructionUtils::FindStateMachineNode(AnimGraph, StateMachineName);
    if (!SMNode || !SMNode->EditorStateMachineGraph)
    {
        Ctx.SendError(TEXT("SM_NOT_FOUND"), FString::Printf(TEXT("State machine '%s' not found"), *StateMachineName));
        return true;
    }

    UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
    if (!SMGraph)
    {
        Ctx.SendError(TEXT("INVALID_GRAPH"), TEXT("Invalid state machine graph"));
        return true;
    }

    UAnimStateNode* StateNode = AnimGraphConstructionUtils::CreateState(SMGraph, FName(*StateName), FVector2D(NodePosX, NodePosY));
    if (!StateNode)
    {
        Ctx.SendError(TEXT("STATE_CREATE_FAILED"), FString::Printf(TEXT("Failed to create state '%s'"), *StateName));
        return true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("stateName"), StateName);
    Result->SetStringField(TEXT("stateMachine"), StateMachineName);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("State '%s' created in state machine '%s'"), *StateName, *StateMachineName));
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(FString::Printf(TEXT("State '%s' marked for creation (requires AnimGraph module)"), *StateName), Result);
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_transition", "animation.authoring",
    "Add a transition between two states in a state machine",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_REQ("stateMachineName", "string", "Name of the state machine"),
        RPC_PARAM_REQ("fromState", "string", "Source state name"),
        RPC_PARAM_REQ("toState", "string", "Target state name"),
        RPC_PARAM_OPT("crossfadeDuration", "number", "Crossfade duration (default 0.2)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString StateMachineName = Ctx.GetString(TEXT("stateMachineName"));
    FString FromState = Ctx.GetString(TEXT("fromState"));
    FString ToState = Ctx.GetString(TEXT("toState"));
    float CrossfadeDuration = static_cast<float>(Ctx.GetNumber(TEXT("crossfadeDuration"), 0.2));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (FromState.IsEmpty() || ToState.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_STATES"), TEXT("fromState and toState are required"));
        return true;
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH && MCP_HAS_ANIM_STATE_MACHINE_SCHEMA && MCP_HAS_ANIM_STATE_TRANSITION
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Could not find AnimGraph in blueprint"));
        return true;
    }

    UAnimGraphNode_StateMachine* SMNode = AnimGraphConstructionUtils::FindStateMachineNode(AnimGraph, StateMachineName);
    if (!SMNode || !SMNode->EditorStateMachineGraph)
    {
        Ctx.SendError(TEXT("SM_NOT_FOUND"), FString::Printf(TEXT("State machine '%s' not found"), *StateMachineName));
        return true;
    }

    UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
    if (!SMGraph)
    {
        Ctx.SendError(TEXT("INVALID_GRAPH"), TEXT("Invalid state machine graph"));
        return true;
    }

    UAnimStateNode* FromNode = AnimGraphConstructionUtils::FindStateNode(SMGraph, FromState);
    UAnimStateNode* ToNode = AnimGraphConstructionUtils::FindStateNode(SMGraph, ToState);

    if (!FromNode)
    {
        Ctx.SendError(TEXT("SOURCE_STATE_NOT_FOUND"), FString::Printf(TEXT("Source state '%s' not found"), *FromState));
        return true;
    }
    if (!ToNode)
    {
        Ctx.SendError(TEXT("TARGET_STATE_NOT_FOUND"), FString::Printf(TEXT("Target state '%s' not found"), *ToState));
        return true;
    }

    UAnimStateTransitionNode* TransNode = AnimGraphConstructionUtils::CreateTransition(FromNode, ToNode, FVector2D::ZeroVector);
    if (!TransNode)
    {
        Ctx.SendError(TEXT("TRANSITION_CREATE_FAILED"), FString::Printf(TEXT("Failed to create transition from '%s' to '%s'"), *FromState, *ToState));
        return true;
    }

    // Configure transition properties — preserved from pre-refactor behaviour.
    TransNode->CrossfadeDuration = CrossfadeDuration;
    TransNode->BlendMode = EAlphaBlendOption::Linear;

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("fromState"), FromState);
    Result->SetStringField(TEXT("toState"), ToState);
    Result->SetNumberField(TEXT("crossfadeDuration"), CrossfadeDuration);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Transition from '%s' to '%s' created"), *FromState, *ToState));
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    // AnimGraph headers not available - return error instead of fake success
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        FString::Printf(TEXT("Cannot create transition from '%s' to '%s': AnimGraph module headers not available in this build."), *FromState, *ToState));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_transition_rules", "animation.authoring",
    "Update transition rules between states in a state machine",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_REQ("stateMachineName", "string", "Name of the state machine"),
        RPC_PARAM_REQ("fromState", "string", "Source state name"),
        RPC_PARAM_REQ("toState", "string", "Target state name"),
        RPC_PARAM_OPT("crossfadeDuration", "number", "Crossfade duration"),
        RPC_PARAM_OPT("priorityOrder", "number", "Priority order"),
        RPC_PARAM_OPT("automaticRule", "boolean", "Use automatic rule (default false)"),
        RPC_PARAM_OPT("bidirectional", "boolean", "Bidirectional transition (default false)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString StateMachineName = Ctx.GetString(TEXT("stateMachineName"));
    FString FromState = Ctx.GetString(TEXT("fromState"));
    FString ToState = Ctx.GetString(TEXT("toState"));
    float CrossfadeDuration = static_cast<float>(Ctx.GetNumber(TEXT("crossfadeDuration"), -1.0));
    int32 PriorityOrder = Ctx.GetInt(TEXT("priorityOrder"), -1);
    bool bAutomatic = Ctx.GetBool(TEXT("automaticRule"), false);
    bool bBidirectional = Ctx.GetBool(TEXT("bidirectional"), false);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH && MCP_HAS_ANIM_STATE_MACHINE_SCHEMA && MCP_HAS_ANIM_STATE_TRANSITION
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Could not find AnimGraph in blueprint"));
        return true;
    }

    UAnimGraphNode_StateMachine* SMNode = AnimGraphConstructionUtils::FindStateMachineNode(AnimGraph, StateMachineName);
    if (!SMNode || !SMNode->EditorStateMachineGraph)
    {
        Ctx.SendError(TEXT("SM_NOT_FOUND"), FString::Printf(TEXT("State machine '%s' not found"), *StateMachineName));
        return true;
    }

    UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
    if (!SMGraph)
    {
        Ctx.SendError(TEXT("INVALID_GRAPH"), TEXT("Invalid state machine graph"));
        return true;
    }

    // Find the transition node between the specified states
    UAnimStateTransitionNode* TransNode = nullptr;
    for (UEdGraphNode* Node : SMGraph->Nodes)
    {
        if (UAnimStateTransitionNode* Trans = Cast<UAnimStateTransitionNode>(Node))
        {
            UAnimStateNodeBase* PrevState = Trans->GetPreviousState();
            UAnimStateNodeBase* NextState = Trans->GetNextState();
            if (PrevState && NextState &&
                PrevState->GetStateName() == FromState &&
                NextState->GetStateName() == ToState)
            {
                TransNode = Trans;
                break;
            }
        }
    }

    if (!TransNode)
    {
        Ctx.SendError(TEXT("TRANSITION_NOT_FOUND"), FString::Printf(TEXT("Transition from '%s' to '%s' not found"), *FromState, *ToState));
        return true;
    }

    // Update transition properties
    if (CrossfadeDuration >= 0.0f)
    {
        TransNode->CrossfadeDuration = CrossfadeDuration;
    }
    if (PriorityOrder >= 0)
    {
        TransNode->PriorityOrder = PriorityOrder;
    }
    TransNode->bAutomaticRuleBasedOnSequencePlayerInState = bAutomatic;
    TransNode->Bidirectional = bBidirectional;

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(FString::Printf(TEXT("Transition rules updated for '%s' -> '%s'"), *FromState, *ToState), Result);
#else
    // AnimGraph headers not available - return error instead of fake success
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        FString::Printf(TEXT("Cannot update transition rules for '%s' -> '%s': AnimGraph module headers not available in this build."), *FromState, *ToState));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_state_machine_entry", "animation.authoring",
    "Rewires a state machine's entry node to the named state",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_REQ("stateMachineName", "string", "Name of the state machine"),
        RPC_PARAM_REQ("stateName", "string", "Name of the state to set as entry"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString StateMachineName = Ctx.GetString(TEXT("stateMachineName"));
    FString StateName = Ctx.GetString(TEXT("stateName"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH && MCP_HAS_ANIM_STATE_MACHINE_SCHEMA
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Could not find AnimGraph in blueprint"));
        return true;
    }

    UAnimGraphNode_StateMachine* SMNode = AnimGraphConstructionUtils::FindStateMachineNode(AnimGraph, StateMachineName);
    if (!SMNode || !SMNode->EditorStateMachineGraph)
    {
        Ctx.SendError(TEXT("SM_NOT_FOUND"), FString::Printf(TEXT("State machine '%s' not found"), *StateMachineName));
        return true;
    }

    UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
    if (!SMGraph)
    {
        Ctx.SendError(TEXT("INVALID_GRAPH"), TEXT("Invalid state machine graph"));
        return true;
    }

    UAnimStateNode* TargetState = AnimGraphConstructionUtils::FindStateNode(SMGraph, StateName);
    if (!TargetState)
    {
        Ctx.SendError(TEXT("STATE_NOT_FOUND"), FString::Printf(TEXT("State '%s' not found"), *StateName));
        return true;
    }

    FString EntryErrorCode;
    FString EntryErrorMessage;
    if (!AnimGraphConstructionUtils::SetStateMachineEntry(SMGraph, TargetState, EntryErrorCode, EntryErrorMessage))
    {
        Ctx.SendError(EntryErrorCode, FString::Printf(TEXT("%s ('%s')"), *EntryErrorMessage, *StateName));
        return true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("stateMachine"), StateMachineName);
    Result->SetStringField(TEXT("stateName"), StateName);
    Result->SetBoolField(TEXT("success"), true);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        TEXT("Cannot set state machine entry: AnimGraph module headers not available in this build."));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_transition_settings", "animation.authoring",
    "Update advanced transition properties (logic type, blend mode, blend curve, disabled)",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_REQ("stateMachineName", "string", "Name of the state machine"),
        RPC_PARAM_REQ("fromState", "string", "Source state name"),
        RPC_PARAM_REQ("toState", "string", "Target state name"),
        RPC_PARAM_OPT("logicType", "string", "'StandardBlend' | 'Inertialization' | 'Custom'"),
        RPC_PARAM_OPT("blendMode", "string", "EAlphaBlendOption name"),
        RPC_PARAM_OPT("blendCurvePath", "path", "Path to a UCurveFloat for Custom blend"),
        RPC_PARAM_OPT("disabled", "boolean", "Set bDisabled to gate transition enterability"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString StateMachineName = Ctx.GetString(TEXT("stateMachineName"));
    FString FromState = Ctx.GetString(TEXT("fromState"));
    FString ToState = Ctx.GetString(TEXT("toState"));
    FString LogicTypeStr = Ctx.GetString(TEXT("logicType"));
    FString BlendModeStr = Ctx.GetString(TEXT("blendMode"));
    FString BlendCurvePath = Ctx.GetString(TEXT("blendCurvePath"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH && MCP_HAS_ANIM_STATE_MACHINE_SCHEMA && MCP_HAS_ANIM_STATE_TRANSITION
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Could not find AnimGraph in blueprint"));
        return true;
    }

    UAnimGraphNode_StateMachine* SMNode = AnimGraphConstructionUtils::FindStateMachineNode(AnimGraph, StateMachineName);
    if (!SMNode || !SMNode->EditorStateMachineGraph)
    {
        Ctx.SendError(TEXT("SM_NOT_FOUND"), FString::Printf(TEXT("State machine '%s' not found"), *StateMachineName));
        return true;
    }

    UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
    if (!SMGraph)
    {
        Ctx.SendError(TEXT("INVALID_GRAPH"), TEXT("Invalid state machine graph"));
        return true;
    }

    UAnimStateTransitionNode* TransNode = nullptr;
    for (UEdGraphNode* Node : SMGraph->Nodes)
    {
        if (UAnimStateTransitionNode* Trans = Cast<UAnimStateTransitionNode>(Node))
        {
            UAnimStateNodeBase* PrevState = Trans->GetPreviousState();
            UAnimStateNodeBase* NextState = Trans->GetNextState();
            if (PrevState && NextState &&
                PrevState->GetStateName() == FromState &&
                NextState->GetStateName() == ToState)
            {
                TransNode = Trans;
                break;
            }
        }
    }

    if (!TransNode)
    {
        Ctx.SendError(TEXT("TRANSITION_NOT_FOUND"), FString::Printf(TEXT("Transition from '%s' to '%s' not found"), *FromState, *ToState));
        return true;
    }

    bool bHasLogicType = false;
    ETransitionLogicType::Type LogicValue = TransNode->LogicType.GetValue();
    if (!LogicTypeStr.IsEmpty())
    {
        if (!AnimationAuthoringHelpers::ParseTransitionLogicType(LogicTypeStr, LogicValue))
        {
            Ctx.SendError(TEXT("INVALID_LOGIC_TYPE"), FString::Printf(TEXT("Unknown logicType '%s'"), *LogicTypeStr));
            return true;
        }
        bHasLogicType = true;
    }

    bool bHasBlendMode = false;
    EAlphaBlendOption BlendValue = TransNode->BlendMode;
    if (!BlendModeStr.IsEmpty())
    {
        if (!AnimationAuthoringHelpers::ParseAlphaBlendOption(BlendModeStr, BlendValue))
        {
            Ctx.SendError(TEXT("INVALID_BLEND_MODE"), FString::Printf(TEXT("Unknown blendMode '%s'"), *BlendModeStr));
            return true;
        }
        bHasBlendMode = true;
    }

    bool bHasBlendCurve = false;
    UCurveFloat* BlendCurve = nullptr;
    if (!BlendCurvePath.IsEmpty())
    {
        BlendCurve = Cast<UCurveFloat>(StaticLoadObject(UCurveFloat::StaticClass(), nullptr, *BlendCurvePath));
        if (!BlendCurve)
        {
            Ctx.SendError(TEXT("BLEND_CURVE_NOT_FOUND"), FString::Printf(TEXT("Could not load curve: %s"), *BlendCurvePath));
            return true;
        }
        bHasBlendCurve = true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const bool bHasDisabled = Payload.IsValid() && Payload->HasField(TEXT("disabled"));
    const bool bDisabled = bHasDisabled ? Ctx.GetBool(TEXT("disabled"), false) : false;

    // Every candidate above is parsed or resolved before the first live field write. This keeps
    // a late blend-mode or curve failure from retaining an earlier logic-type assignment.
    if (bHasLogicType)
    {
        TransNode->LogicType = TEnumAsByte<ETransitionLogicType::Type>(LogicValue);
    }
    if (bHasBlendMode)
    {
        TransNode->BlendMode = BlendValue;
    }
    if (bHasBlendCurve)
    {
        TransNode->CustomBlendCurve = BlendCurve;
    }

    if (bHasDisabled)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        TransNode->bDisabled = bDisabled;
#else
        // bDisabled is 5.6+; on 5.4 mirror the disable via the node enabled-state
        // (matches the read-side equivalence used in AGIRTextEmitter.cpp).
        TransNode->SetEnabledState(bDisabled
            ? ENodeEnabledState::Disabled : ENodeEnabledState::Enabled);
#endif
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("fromState"), FromState);
    Result->SetStringField(TEXT("toState"), ToState);
    Result->SetBoolField(TEXT("success"), true);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        TEXT("Cannot set transition settings: AnimGraph module headers not available in this build."));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_state_alias", "animation.authoring",
    "Add a UAnimStateAliasNode to a state machine (imperative peer to AGIR state_alias)",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_REQ("stateMachineName", "string", "Name of the state machine"),
        RPC_PARAM_REQ("aliasName", "string", "Name for the new alias node"),
        RPC_PARAM_OPT("aliases", "array", "List of existing state names this alias represents"),
        RPC_PARAM_OPT("globalAlias", "boolean", "If true, represents all states; ignores 'aliases' (default false)"),
        RPC_PARAM_REQ("x", "number", "X position in graph"),
        RPC_PARAM_REQ("y", "number", "Y position in graph"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString StateMachineName = Ctx.GetString(TEXT("stateMachineName"));
    FString AliasName = Ctx.GetString(TEXT("aliasName"));
    bool bGlobalAlias = Ctx.GetBool(TEXT("globalAlias"), false);
    double XD = 0.0, YD = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), XD)) return true;
    if (!Ctx.RequireNumber(TEXT("y"), YD)) return true;
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AliasName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("aliasName must be non-empty"));
        return true;
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH && MCP_HAS_ANIM_STATE_MACHINE_SCHEMA
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Could not find AnimGraph in blueprint"));
        return true;
    }

    UAnimGraphNode_StateMachine* SMNode = AnimGraphConstructionUtils::FindStateMachineNode(AnimGraph, StateMachineName);
    if (!SMNode || !SMNode->EditorStateMachineGraph)
    {
        Ctx.SendError(TEXT("SM_NOT_FOUND"), FString::Printf(TEXT("State machine '%s' not found"), *StateMachineName));
        return true;
    }

    UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
    if (!SMGraph)
    {
        Ctx.SendError(TEXT("INVALID_GRAPH"), TEXT("Invalid state machine graph"));
        return true;
    }

    UAnimStateAliasNode* AliasNode = AnimGraphConstructionUtils::CreateStateAlias(
        SMGraph, FName(*AliasName), FVector2D(XD, YD));
    if (!AliasNode)
    {
        Ctx.SendError(TEXT("ALIAS_CREATE_FAILED"), FString::Printf(TEXT("Could not create alias '%s'"), *AliasName));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Warnings;
    int32 AliasedCount = 0;
    if (bGlobalAlias)
    {
        AliasNode->bGlobalAlias = true;
    }
    else
    {
        const TArray<TSharedPtr<FJsonValue>>* AliasesArr = Ctx.GetArray(TEXT("aliases"));
        if (AliasesArr)
        {
        for (const TSharedPtr<FJsonValue>& Val : *AliasesArr)
        {
            if (!Val.IsValid() || Val->Type != EJson::String)
            {
                continue;
            }
            const FString Name = Val->AsString();
            UAnimStateNode* State = AnimGraphConstructionUtils::FindStateNode(SMGraph, Name);
            if (State)
            {
                AliasNode->GetAliasedStates().Add(TWeakObjectPtr<UAnimStateNodeBase>(State));
                ++AliasedCount;
            }
            else
            {
                Warnings.Add(MakeShared<FJsonValueString>(
                    FString::Printf(TEXT("State '%s' not found"), *Name)));
            }
        }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("aliasName"), AliasName);
    Result->SetNumberField(TEXT("aliasedCount"), AliasedCount);
    Result->SetBoolField(TEXT("globalAlias"), bGlobalAlias);
    Result->SetArrayField(TEXT("warnings"), Warnings);
    Result->SetBoolField(TEXT("success"), true);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        TEXT("Cannot add state alias: AnimGraph module headers not available in this build."));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_blend_node", "animation.authoring",
    "Add a blend node to an animation blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_OPT("blendType", "string", "TwoWayBlend or LayeredBlend"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString BlendType = Ctx.GetString(TEXT("blendType"), TEXT("TwoWayBlend"));
    double XD = 0.0, YD = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), XD)) return true;
    if (!Ctx.RequireNumber(TEXT("y"), YD)) return true;
    int32 NodePosX = static_cast<int32>(XD);
    int32 NodePosY = static_cast<int32>(YD);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH
    // Get the main AnimGraph
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Could not find AnimGraph in blueprint"));
        return true;
    }

    FString CreatedNodeType;

#if MCP_HAS_TWO_WAY_BLEND
    if (BlendType == TEXT("TwoWayBlend") || BlendType == TEXT("Blend"))
    {
        FGraphNodeCreator<UAnimGraphNode_TwoWayBlend> NodeCreator(*AnimGraph);
        UAnimGraphNode_TwoWayBlend* BlendNode = NodeCreator.CreateNode();
        BlendNode->NodePosX = NodePosX;
        BlendNode->NodePosY = NodePosY;
        NodeCreator.Finalize();
        CreatedNodeType = TEXT("TwoWayBlend");
    }
    else
#endif
#if MCP_HAS_LAYERED_BLEND
    if (BlendType == TEXT("LayeredBlend") || BlendType == TEXT("LayeredBoneBlend"))
    {
        FGraphNodeCreator<UAnimGraphNode_LayeredBoneBlend> NodeCreator(*AnimGraph);
        UAnimGraphNode_LayeredBoneBlend* BlendNode = NodeCreator.CreateNode();
        BlendNode->NodePosX = NodePosX;
        BlendNode->NodePosY = NodePosY;
        NodeCreator.Finalize();
        CreatedNodeType = TEXT("LayeredBoneBlend");
    }
    else
#endif
    {
        // Default fallback to TwoWayBlend if available
#if MCP_HAS_TWO_WAY_BLEND
        FGraphNodeCreator<UAnimGraphNode_TwoWayBlend> NodeCreator(*AnimGraph);
        UAnimGraphNode_TwoWayBlend* BlendNode = NodeCreator.CreateNode();
        BlendNode->NodePosX = NodePosX;
        BlendNode->NodePosY = NodePosY;
        NodeCreator.Finalize();
        CreatedNodeType = TEXT("TwoWayBlend");
#else
        Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
            FString::Printf(TEXT("Cannot create blend node '%s': AnimGraph blend node headers not available in this build."), *BlendType));
        return true;
#endif
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeType"), CreatedNodeType);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Blend node '%s' created"), *CreatedNodeType));
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    // AnimGraph headers not available - return error instead of fake success
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        FString::Printf(TEXT("Cannot create blend node '%s': AnimGraph module headers not available in this build."), *BlendType));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_cached_pose", "animation.authoring",
    "Add a save cached pose node to an animation blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_REQ("cacheName", "string", "Name for the cached pose"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString CacheName = Ctx.GetString(TEXT("cacheName"));
    double XD = 0.0, YD = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), XD)) return true;
    if (!Ctx.RequireNumber(TEXT("y"), YD)) return true;
    int32 NodePosX = static_cast<int32>(XD);
    int32 NodePosY = static_cast<int32>(YD);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (CacheName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_CACHE_NAME"), TEXT("cacheName is required"));
        return true;
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH && MCP_HAS_CACHED_POSE
    // Get the main AnimGraph
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Could not find AnimGraph in blueprint"));
        return true;
    }

    // Create the Save Cached Pose node
    FGraphNodeCreator<UAnimGraphNode_SaveCachedPose> NodeCreator(*AnimGraph);
    UAnimGraphNode_SaveCachedPose* CachedPoseNode = NodeCreator.CreateNode();
    CachedPoseNode->NodePosX = NodePosX;
    CachedPoseNode->NodePosY = NodePosY;
    CachedPoseNode->CacheName = CacheName;
    NodeCreator.Finalize();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("cacheName"), CacheName);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Cached pose node '%s' created"), *CacheName));
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(FString::Printf(TEXT("Cached pose '%s' marked for creation (requires AnimGraph module)"), *CacheName), Result);
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_slot_node", "animation.authoring",
    "Add a slot node to an animation blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_REQ("slotName", "string", "Slot name"),
        RPC_PARAM_OPT("groupName", "string", "Group name (default DefaultGroup)"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString SlotName = Ctx.GetString(TEXT("slotName"));
    FString GroupName = Ctx.GetString(TEXT("groupName"), TEXT("DefaultGroup"));
    double XD = 0.0, YD = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), XD)) return true;
    if (!Ctx.RequireNumber(TEXT("y"), YD)) return true;
    int32 NodePosX = static_cast<int32>(XD);
    int32 NodePosY = static_cast<int32>(YD);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (SlotName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_SLOT_NAME"), TEXT("slotName is required"));
        return true;
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH && MCP_HAS_SLOT_NODE
    // Get the main AnimGraph
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Could not find AnimGraph in blueprint"));
        return true;
    }

    // Create the Slot node
    FGraphNodeCreator<UAnimGraphNode_Slot> NodeCreator(*AnimGraph);
    UAnimGraphNode_Slot* SlotNode = NodeCreator.CreateNode();
    SlotNode->NodePosX = NodePosX;
    SlotNode->NodePosY = NodePosY;

    // The slot node stores a BARE slot name; the group is NOT part of the name.
    // UAnimGraphNode_Slot derives the group at runtime via
    // USkeleton::GetSlotGroupName(Node.SlotName), so baking "Group.Slot" here
    // produces a slot name no montage slot ('DefaultSlot') can ever match.
    SlotNode->Node.SlotName = FName(*SlotName);

    // Honor groupName by registering the slot under that group at the skeleton
    // level (SetSlotGroupName creates the group if it does not yet exist).
    if (USkeleton* TargetSkeleton = AnimBP->TargetSkeleton)
    {
        TargetSkeleton->SetSlotGroupName(FName(*SlotName), FName(*GroupName));
    }

    NodeCreator.Finalize();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("slotName"), SlotName);
    Result->SetStringField(TEXT("groupName"), GroupName);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Slot node '%s' created"), *SlotName));
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    // AnimGraph headers not available - return error instead of fake success
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        FString::Printf(TEXT("Cannot create slot node '%s': AnimGraph module headers not available in this build."), *SlotName));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_layered_blend_per_bone", "animation.authoring",
    "Add a layered blend per bone node to an animation blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_OPT("boneName", "string", "Bone name for a single-layer single-filter shortcut (use 'layers' for multi-bone)"),
        RPC_PARAM_OPT("layers", "array", "Optional layer specs: [{branchFilters:[{boneName, blendDepth}]}]"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString BoneName = Ctx.GetString(TEXT("boneName"));
    double XD = 0.0, YD = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), XD)) return true;
    if (!Ctx.RequireNumber(TEXT("y"), YD)) return true;
    int32 NodePosX = static_cast<int32>(XD);
    int32 NodePosY = static_cast<int32>(YD);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH && MCP_HAS_LAYERED_BLEND
    // Get the main AnimGraph
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Could not find AnimGraph in blueprint"));
        return true;
    }

    // Create the Layered Bone Blend node. The default ctor's AddFirstPose hook
    // populates LayerSetup[0]/BlendPoses[0]/BlendWeights[0] (called by the
    // GraphNodeCreator path), so the single-bone shortcut can write directly
    // into LayerSetup[0].BranchFilters.
    FGraphNodeCreator<UAnimGraphNode_LayeredBoneBlend> NodeCreator(*AnimGraph);
    UAnimGraphNode_LayeredBoneBlend* BlendNode = NodeCreator.CreateNode();
    BlendNode->NodePosX = NodePosX;
    BlendNode->NodePosY = NodePosY;
    NodeCreator.Finalize();

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    if (Payload.IsValid() && Payload->HasTypedField<EJson::Array>(TEXT("layers")))
    {
        const TArray<TSharedPtr<FJsonValue>>* LayersJsonPtr = Ctx.GetArray(TEXT("layers"));
        TArray<TSharedPtr<FJsonValue>> LayersJson = LayersJsonPtr ? *LayersJsonPtr : TArray<TSharedPtr<FJsonValue>>();
        FString Err = AnimGraphConstructionUtils::WriteLayeredBlendLayers(BlendNode, LayersJson);
        if (!Err.IsEmpty())
        {
            Ctx.SendError(TEXT("INVALID_LAYERS"), Err);
            return true;
        }
        BlendNode->ReconstructNode();
    }
    else if (!BoneName.IsEmpty())
    {
        if (BlendNode->Node.LayerSetup.Num() == 0)
        {
            // Defensive fallback for builds where AddFirstPose didn't fire — synthesize
            // a one-layer one-filter LayersJson and route through the shared helper.
            TSharedPtr<FJsonObject> FilterObj = MakeShared<FJsonObject>();
            FilterObj->SetStringField(TEXT("boneName"), BoneName);
            FilterObj->SetNumberField(TEXT("blendDepth"), 0);
            TArray<TSharedPtr<FJsonValue>> Filters;
            Filters.Add(MakeShared<FJsonValueObject>(FilterObj));

            TSharedPtr<FJsonObject> LayerObj = MakeShared<FJsonObject>();
            LayerObj->SetArrayField(TEXT("branchFilters"), Filters);
            TArray<TSharedPtr<FJsonValue>> LayersJson;
            LayersJson.Add(MakeShared<FJsonValueObject>(LayerObj));

            FString Err = AnimGraphConstructionUtils::WriteLayeredBlendLayers(BlendNode, LayersJson);
            if (!Err.IsEmpty())
            {
                Ctx.SendError(TEXT("INVALID_LAYERS"), Err);
                return true;
            }
        }
        else
        {
            FBranchFilter Filter;
            Filter.BoneName = FName(*BoneName);
            Filter.BlendDepth = 0;
            BlendNode->Node.LayerSetup[0].BranchFilters.Emplace(MoveTemp(Filter));
        }
        BlendNode->ReconstructNode();
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(TEXT("Layered blend per bone node created"), Result);
#else
    // AnimGraph headers not available - return error instead of fake success
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        TEXT("Cannot create layered blend per bone node: AnimGraph module headers not available in this build."));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_layered_blend_layers", "animation.authoring",
    "Rewrite the LayerSetup / BlendMode / blend-mask asset on an existing UAnimGraphNode_LayeredBoneBlend",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_OPT("graphName", "string", "Anim graph name (default 'AnimGraph')"),
        RPC_PARAM_REQ("nodeName", "string", "Name of the LayeredBoneBlend AnimGraph node"),
        RPC_PARAM_REQ("layers", "array", "Layer specs: [{branchFilters:[{boneName, blendDepth}]}]"),
        RPC_PARAM_OPT("blendMode", "string", "'BranchFilter' (default) or 'BlendMask'"),
        RPC_PARAM_OPT("blendMaskPath", "path", "Path to a UBlendProfile asset (used when blendMode='BlendMask')"),
        RPC_PARAM_OPT("meshSpaceRotationBlend", "boolean", "If true, blend bone rotations in mesh space"),
        RPC_PARAM_OPT("meshSpaceScaleBlend", "boolean", "If true, blend bone scales in mesh space"),
        RPC_PARAM_OPT("curveBlendOption", "string", "ECurveBlendOption enumerator (Override, NormalizeByWeight, BlendByWeight, UseBasePose, UseMaxValue, UseMinValue)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString GraphName = Ctx.GetString(TEXT("graphName"), TEXT("AnimGraph"));
    FString NodeName = Ctx.GetString(TEXT("nodeName"));
    FString BlendModeStr = Ctx.GetString(TEXT("blendMode"));
    FString BlendMaskPath = Ctx.GetString(TEXT("blendMaskPath"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (NodeName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETERS"), TEXT("nodeName is required"));
        return true;
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH && MCP_HAS_LAYERED_BLEND
    UEdGraph* TargetGraph = AnimGraphConstructionUtils::ResolveAnimBlueprintGraph(AnimBP, GraphName);
    if (!TargetGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), FString::Printf(TEXT("Could not find graph '%s' in blueprint"), *GraphName));
        return true;
    }

    const AnimGraphConstructionUtils::FAnimNodeResolveResult Resolved =
        AnimGraphConstructionUtils::ResolveAnimGraphNodeByTitle(TargetGraph, NodeName);
    if (Resolved.Status == AnimGraphConstructionUtils::EAnimNodeResolveStatus::Ambiguous)
    {
        SendAmbiguousNodeError(Ctx, NodeName, Resolved.Candidates);
        return true;
    }
    // On Found, the resolver matched exactly one node; its actual title is
    // Candidates[0]. A null Node here means that single match exists but is not
    // a UAnimGraphNode_Base — still the "wrong type" path below.
    const FString ResolvedNodeTitle = Resolved.Candidates.Num() > 0 ? Resolved.Candidates[0] : NodeName;
    UAnimGraphNode_LayeredBoneBlend* Node = Cast<UAnimGraphNode_LayeredBoneBlend>(Resolved.Node);
    if (!Node)
    {
        if (Resolved.Status == AnimGraphConstructionUtils::EAnimNodeResolveStatus::Found)
        {
            Ctx.SendError(TEXT("WRONG_NODE_TYPE"), FString::Printf(TEXT("Node '%s' exists but is not a UAnimGraphNode_LayeredBoneBlend"), *ResolvedNodeTitle));
        }
        else
        {
            Ctx.SendError(TEXT("NODE_NOT_FOUND"), FString::Printf(TEXT("Node '%s' not found in graph '%s'"), *NodeName, *GraphName));
        }
        return true;
    }

    // Resolve blendMode without touching the node. WriteLayeredBlendLayers branches on the
    // selected mode when growing BlendMasks vs LayerSetup in lock-step.
    bool bHasBlendMode = false;
    ELayeredBoneBlendMode DesiredBlendMode = Node->Node.BlendMode;
    if (!BlendModeStr.IsEmpty())
    {
        if (BlendModeStr.Equals(TEXT("BranchFilter"), ESearchCase::IgnoreCase))
        {
            DesiredBlendMode = ELayeredBoneBlendMode::BranchFilter;
        }
        else if (BlendModeStr.Equals(TEXT("BlendMask"), ESearchCase::IgnoreCase))
        {
            DesiredBlendMode = ELayeredBoneBlendMode::BlendMask;
        }
        else
        {
            Ctx.SendError(TEXT("INVALID_BLEND_MODE"), FString::Printf(TEXT("Unknown blendMode '%s' (expected 'BranchFilter' or 'BlendMask')"), *BlendModeStr));
            return true;
        }
        bHasBlendMode = true;
    }

    UBlendProfile* BlendMaskProfile = nullptr;
    if (!BlendMaskPath.IsEmpty())
    {
        BlendMaskProfile = Cast<UBlendProfile>(StaticLoadObject(UBlendProfile::StaticClass(), nullptr, *BlendMaskPath));
        if (!BlendMaskProfile)
        {
            Ctx.SendError(TEXT("BONE_MASK_NOT_FOUND"), FString::Printf(TEXT("Could not load UBlendProfile: %s"), *BlendMaskPath));
            return true;
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* LayersJsonPtr = Ctx.GetArray(TEXT("layers"));
    TArray<TSharedPtr<FJsonValue>> LayersJson = LayersJsonPtr ? *LayersJsonPtr : TArray<TSharedPtr<FJsonValue>>();
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString CurveBlendStr;
    const bool bHasCurveBlendOption = Payload.IsValid() &&
        Payload->TryGetStringField(TEXT("curveBlendOption"), CurveBlendStr) &&
        !CurveBlendStr.IsEmpty();
    bool bMeshSpaceRotationBlend = false;
    const bool bHasMeshSpaceRotationBlend = Payload.IsValid() &&
        Payload->TryGetBoolField(TEXT("meshSpaceRotationBlend"), bMeshSpaceRotationBlend);
    bool bMeshSpaceScaleBlend = false;
    const bool bHasMeshSpaceScaleBlend = Payload.IsValid() &&
        Payload->TryGetBoolField(TEXT("meshSpaceScaleBlend"), bMeshSpaceScaleBlend);

    // Validate every input that can return an error before changing the live node. The writer
    // repeats layer validation defensively; every reflected option is also imported into scratch
    // storage so an invalid field cannot leave the mode/layers partially applied.
    const FString LayerValidationError = AnimGraphConstructionUtils::ValidateLayeredBlendLayers(LayersJson);
    if (!LayerValidationError.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_LAYERS"), LayerValidationError);
        return true;
    }

    auto SendReflectedFieldError = [&](const TCHAR* JsonField, const FString& Error)
    {
        Ctx.SendError(TEXT("PROPERTY_SET_FAILED"), FString::Printf(
            TEXT("Failed to set property '%s' on node '%s': %s"),
            JsonField, *ResolvedNodeTitle, *Error));
    };
    auto ValidateReflectedField = [&](bool bHasValue, const TCHAR* JsonField,
        const TCHAR* RuntimeField, const FString& ValueAsText)
    {
        if (!bHasValue)
        {
            return true;
        }

        const FString ValidationError = AnimGraphConstructionUtils::ValidateAnimNodeFieldByName(
            Node, FName(RuntimeField), ValueAsText);
        if (!ValidationError.IsEmpty())
        {
            SendReflectedFieldError(JsonField, ValidationError);
            return false;
        }
        return true;
    };

    const FString RotationBlendText = bMeshSpaceRotationBlend ? TEXT("true") : TEXT("false");
    const FString ScaleBlendText = bMeshSpaceScaleBlend ? TEXT("true") : TEXT("false");
    if (!ValidateReflectedField(
        bHasMeshSpaceRotationBlend, TEXT("meshSpaceRotationBlend"),
        TEXT("bMeshSpaceRotationBlend"), RotationBlendText) ||
        !ValidateReflectedField(
        bHasMeshSpaceScaleBlend, TEXT("meshSpaceScaleBlend"),
        TEXT("bMeshSpaceScaleBlend"), ScaleBlendText) ||
        !ValidateReflectedField(
        bHasCurveBlendOption, TEXT("curveBlendOption"),
        TEXT("CurveBlendOption"), CurveBlendStr))
    {
        return true;
    }

    // All reflected values above have passed the same ImportText path used for the live write.
    // Apply them before mode/layer changes so an unexpected engine-side disagreement can restore
    // these scalar fields without having to roll back the dynamic layer arrays.
    const ECurveBlendOption::Type PreviousCurveBlendOption = Node->Node.CurveBlendOption.GetValue();
    const bool PreviousMeshSpaceRotationBlend = Node->Node.bMeshSpaceRotationBlend;
    const bool PreviousMeshSpaceScaleBlend = Node->Node.bMeshSpaceScaleBlend;
    auto RestoreReflectedFields = [&]()
    {
        Node->Node.CurveBlendOption = PreviousCurveBlendOption;
        Node->Node.bMeshSpaceRotationBlend = PreviousMeshSpaceRotationBlend;
        Node->Node.bMeshSpaceScaleBlend = PreviousMeshSpaceScaleBlend;
    };
    auto WriteValidatedReflectedField = [&](bool bHasValue, const TCHAR* JsonField,
        const TCHAR* RuntimeField, const FString& ValueAsText)
    {
        if (!bHasValue)
        {
            return true;
        }

        const FString WriteError = AnimGraphConstructionUtils::WriteAnimNodeFieldByName(
            Node, FName(RuntimeField), ValueAsText);
        if (!WriteError.IsEmpty())
        {
            RestoreReflectedFields();
            SendReflectedFieldError(JsonField, WriteError);
            return false;
        }
        return true;
    };
    if (!WriteValidatedReflectedField(
        bHasMeshSpaceRotationBlend, TEXT("meshSpaceRotationBlend"),
        TEXT("bMeshSpaceRotationBlend"), RotationBlendText) ||
        !WriteValidatedReflectedField(
        bHasMeshSpaceScaleBlend, TEXT("meshSpaceScaleBlend"),
        TEXT("bMeshSpaceScaleBlend"), ScaleBlendText) ||
        !WriteValidatedReflectedField(
        bHasCurveBlendOption, TEXT("curveBlendOption"),
        TEXT("CurveBlendOption"), CurveBlendStr))
    {
        return true;
    }

    if (bHasBlendMode)
    {
        Node->Node.BlendMode = DesiredBlendMode;
    }

    AnimGraphConstructionUtils::ApplyValidatedLayeredBlendLayers(Node, LayersJson);

    // Assign the resolved blend mask to all entries after WriteLayeredBlendLayers
    // has sized BlendMasks. (BranchFilter mode leaves BlendMasks empty per the
    // lock-step rule, so the assignment is a no-op there — which is correct.)
    if (BlendMaskProfile)
    {
        for (TObjectPtr<UBlendProfile>& Mask : Node->Node.BlendMasks)
        {
            Mask = BlendMaskProfile;
        }
    }

    TArray<TSharedPtr<FJsonValue>> WarningsArray;

    Node->ReconstructNode();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeName"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
    Result->SetNumberField(TEXT("layers"), LayersJson.Num());
    Result->SetStringField(TEXT("blendMode"),
        Node->Node.BlendMode == ELayeredBoneBlendMode::BlendMask ? TEXT("BlendMask") : TEXT("BranchFilter"));
    Result->SetBoolField(TEXT("success"), true);
    Result->SetArrayField(TEXT("warnings"), WarningsArray);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        TEXT("Cannot set layered blend layers: AnimGraph module headers not available in this build."));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_anim_graph_node_value", "animation.authoring",
    "Set a property value on an AnimGraph node by name",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_REQ("nodeName", "string", "Name of the node in the AnimGraph"),
        RPC_PARAM_REQ("propertyName", "string", "Property name to set"),
        RPC_PARAM_REQ("value", "string", "Value to set (type-dependent)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    const TSharedPtr<FJsonObject>& Params = Ctx.GetRawPayload();
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString NodeName = Ctx.GetString(TEXT("nodeName"));
    FString PropertyName = Ctx.GetString(TEXT("propertyName"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (NodeName.IsEmpty() || PropertyName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETERS"), TEXT("nodeName and propertyName are required"));
        return true;
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH
    // Get the main AnimGraph
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Could not find AnimGraph in blueprint"));
        return true;
    }

    // Find the node by name
    UEdGraphNode* FoundNode = nullptr;
    for (UEdGraphNode* Node : AnimGraph->Nodes)
    {
        if (Node && Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(NodeName))
        {
            FoundNode = Node;
            break;
        }
    }

    if (!FoundNode)
    {
        Ctx.SendError(TEXT("NODE_NOT_FOUND"), FString::Printf(TEXT("Node '%s' not found in AnimGraph"), *NodeName));
        return true;
    }

    // Get the value from params first — both the flat and the nested-field
    // write paths below need it.
    TSharedPtr<FJsonValue> ValueField = Params->TryGetField(TEXT("value"));
    if (!ValueField.IsValid())
    {
        Ctx.SendError(TEXT("MISSING_VALUE"), TEXT("value parameter is required"));
        return true;
    }

    // Find and set the property using reflection. Most configurable defaults on
    // a stock AnimGraph node do NOT live as a direct UPROPERTY on the outer
    // UAnimGraphNode_* class — they live on the wrapped runtime FAnimNode_*
    // struct exposed as the public `Node` member (e.g. FAnimNode_TwoBoneIK::Alpha,
    // bone references, the *LocationSpace fields). So when the flat outer-class
    // lookup misses, fall back to the runtime-FAnimNode-field path the sibling
    // factory add_graph_node already uses (ResolveAnimNodeFieldByName over
    // GetFNodeProperty/GetFNode), making this generic post-hoc setter able to
    // edit the nested defaults too.
    FProperty* Property = FoundNode->GetClass()->FindPropertyByName(FName(*PropertyName));
    if (!Property)
    {
        // Flat outer-class lookup missed — fall back to the runtime-FAnimNode-field
        // path, then converge on the single shared success epilogue below.
        UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(FoundNode);
        FString RuntimeError;
        if (!AnimNode ||
            !AnimGraphConstructionUtils::ApplyJsonValueToAnimNodeFieldByName(
                AnimNode, FName(*PropertyName), ValueField, RuntimeError))
        {
            // ApplyJsonValueToAnimNodeFieldByName fails either because the field
            // could not be resolved (RuntimeError empty path → genuinely not found)
            // or because the field WAS resolved but the value was rejected
            // (RuntimeError populated). Surface the latter as PROPERTY_SET_FAILED so
            // a bad value on a valid nested field isn't misreported as 'not found',
            // matching the flat path below and the sibling add_graph_node.
            if (AnimNode && !RuntimeError.IsEmpty())
            {
                Ctx.SendError(TEXT("PROPERTY_SET_FAILED"), FString::Printf(TEXT("Failed to set property '%s' on node '%s': %s"), *PropertyName, *NodeName, *RuntimeError));
            }
            else
            {
                Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"), FString::Printf(TEXT("Property '%s' not found on node '%s'"), *PropertyName, *NodeName));
            }
            return true;
        }
    }
    else
    {
        FString ApplyError;
        if (!ApplyJsonValueToProperty(FoundNode, Property, ValueField, ApplyError))
        {
            Ctx.SendError(TEXT("PROPERTY_SET_FAILED"), FString::Printf(TEXT("Failed to set property: %s"), *ApplyError));
            return true;
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeName"), NodeName);
    Result->SetStringField(TEXT("propertyName"), PropertyName);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Property '%s' set on node '%s'"), *PropertyName, *NodeName));
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    // AnimGraph headers not available - return error
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        FString::Printf(TEXT("Cannot set node value on '%s': AnimGraph module headers not available in this build."), *NodeName));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_anim_graph_pin_exposed", "animation.authoring",
    "Toggle 'Expose as Pin' on an AnimGraph node's optional property (ShowPinForProperties entry). "
    "Materializes / removes the input pin so it can be wired to a variable getter via blueprint.graph.connect_pins.",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_OPT("graphName", "string", "Anim graph name (default 'AnimGraph')"),
        RPC_PARAM_REQ("nodeName", "string", "Name of the AnimGraph node to mutate"),
        RPC_PARAM_REQ("propertyName", "string", "FAnimNode_* field name to expose as a pin"),
        RPC_PARAM_REQ("exposed", "boolean", "true to show pin, false to hide"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString GraphName = Ctx.GetString(TEXT("graphName"));
    FString NodeName = Ctx.GetString(TEXT("nodeName"));
    FString PropertyName = Ctx.GetString(TEXT("propertyName"));
    bool bExposed = false;
    if (!Ctx.RequireBool(TEXT("exposed"), bExposed)) return true;
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (NodeName.IsEmpty() || PropertyName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETERS"), TEXT("nodeName and propertyName are required"));
        return true;
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::ResolveAnimBlueprintGraph(AnimBP, GraphName);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"),
            FString::Printf(TEXT("Could not find graph '%s' in blueprint"),
                GraphName.IsEmpty() ? TEXT("AnimGraph") : *GraphName));
        return true;
    }

    FString ResolvedNodeTitle;
    UAnimGraphNode_Base* AnimNode = ResolveAnimNodeOrSendError(Ctx, AnimGraph, NodeName, ResolvedNodeTitle);
    if (!AnimNode)
    {
        return true;
    }

    FString ToggleError;
    if (!AnimGraphConstructionUtils::ToggleOptionalPinExposed(AnimNode, FName(*PropertyName), bExposed, ToggleError))
    {
        Ctx.SendError(ToggleError, FString::Printf(TEXT("Could not toggle pin '%s' on node '%s'"), *PropertyName, *ResolvedNodeTitle));
        return true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeName"), ResolvedNodeTitle);
    Result->SetStringField(TEXT("propertyName"), PropertyName);
    Result->SetBoolField(TEXT("exposed"), bExposed);
    Result->SetBoolField(TEXT("success"), true);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        TEXT("Cannot toggle pin exposure: AnimGraph module headers not available in this build."));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_anim_graph_pins_exposed", "animation.authoring",
    "Batch variant of set_anim_graph_pin_exposed — applies one Modify+Save after iterating "
    "the pins[] array. Per-entry failures land in result.errors[] without aborting the batch.",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_OPT("graphName", "string", "Anim graph name (default 'AnimGraph')"),
        RPC_PARAM_REQ("nodeName", "string", "Name of the AnimGraph node to mutate"),
        RPC_PARAM_REQ("pins", "array", "List of {propertyName, exposed} entries"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString GraphName = Ctx.GetString(TEXT("graphName"));
    FString NodeName = Ctx.GetString(TEXT("nodeName"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
    if (!Ctx.RequireArray(TEXT("pins"), Pins)) return true;

    if (NodeName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETERS"), TEXT("nodeName is required"));
        return true;
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::ResolveAnimBlueprintGraph(AnimBP, GraphName);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"),
            FString::Printf(TEXT("Could not find graph '%s' in blueprint"),
                GraphName.IsEmpty() ? TEXT("AnimGraph") : *GraphName));
        return true;
    }

    FString ResolvedNodeTitle;
    UAnimGraphNode_Base* AnimNode = ResolveAnimNodeOrSendError(Ctx, AnimGraph, NodeName, ResolvedNodeTitle);
    if (!AnimNode)
    {
        return true;
    }

    int32 AppliedCount = 0;
    TArray<TSharedPtr<FJsonValue>> Errors;
    for (const TSharedPtr<FJsonValue>& Entry : *Pins)
    {
        const TSharedPtr<FJsonObject>* PinObj = nullptr;
        if (!Entry.IsValid() || !Entry->TryGetObject(PinObj) || !PinObj || !PinObj->IsValid())
        {
            TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
            Err->SetStringField(TEXT("error"), TEXT("INVALID_ENTRY"));
            Err->SetStringField(TEXT("message"), TEXT("pins[] entry must be a JSON object"));
            Errors.Add(MakeShared<FJsonValueObject>(Err));
            continue;
        }

        FString PropName;
        bool bPinExposed = false;
        if (!(*PinObj)->TryGetStringField(TEXT("propertyName"), PropName) || PropName.IsEmpty())
        {
            TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
            Err->SetStringField(TEXT("error"), TEXT("MISSING_PROPERTY_NAME"));
            Err->SetStringField(TEXT("message"), TEXT("pins[] entry missing 'propertyName'"));
            Errors.Add(MakeShared<FJsonValueObject>(Err));
            continue;
        }
        if (!(*PinObj)->TryGetBoolField(TEXT("exposed"), bPinExposed))
        {
            TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
            Err->SetStringField(TEXT("propertyName"), PropName);
            Err->SetStringField(TEXT("error"), TEXT("MISSING_EXPOSED"));
            Err->SetStringField(TEXT("message"), TEXT("pins[] entry missing 'exposed' boolean"));
            Errors.Add(MakeShared<FJsonValueObject>(Err));
            continue;
        }

        FString ToggleError;
        if (!AnimGraphConstructionUtils::ToggleOptionalPinExposed(AnimNode, FName(*PropName), bPinExposed, ToggleError))
        {
            TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
            Err->SetStringField(TEXT("propertyName"), PropName);
            Err->SetStringField(TEXT("error"), ToggleError);
            Errors.Add(MakeShared<FJsonValueObject>(Err));
            continue;
        }
        ++AppliedCount;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeName"), ResolvedNodeTitle);
    Result->SetNumberField(TEXT("applied"), AppliedCount);
    Result->SetArrayField(TEXT("errors"), Errors);
    Result->SetBoolField(TEXT("success"), true);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        TEXT("Cannot toggle pin exposure: AnimGraph module headers not available in this build."));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.bind_player_asset", "animation.authoring",
    "Bind a UAnimationAsset (Sequence/BlendSpace/PoseAsset/AimOffset) to an asset-player AnimGraph node. "
    "Dispatches on UAnimGraphNode_AssetPlayerBase::SetAnimationAsset and validates asset class via GetAnimationAssetClass. "
    "Optionally writes asset-player options {loop, playRate, startPosition} via the inner FAnimNode_* struct.",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_OPT("graphName", "string", "Anim graph name (default 'AnimGraph')"),
        RPC_PARAM_REQ("nodeName", "string", "Name of the asset-player AnimGraph node to bind"),
        RPC_PARAM_REQ("assetPath", "path", "Path to a UAnimationAsset (Sequence/BlendSpace/PoseAsset/AimOffset)"),
        RPC_PARAM_OPT("options", "object", "Optional {loop: bool, playRate: number, startPosition: number}"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString GraphName = Ctx.GetString(TEXT("graphName"));
    FString NodeName = Ctx.GetString(TEXT("nodeName"));
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (NodeName.IsEmpty() || AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETERS"), TEXT("nodeName and assetPath are required"));
        return true;
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::ResolveAnimBlueprintGraph(AnimBP, GraphName);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"),
            FString::Printf(TEXT("Could not find graph '%s' in blueprint"),
                GraphName.IsEmpty() ? TEXT("AnimGraph") : *GraphName));
        return true;
    }

    FString ResolvedNodeTitle;
    UAnimGraphNode_Base* AnimBaseNode = ResolveAnimNodeOrSendError(Ctx, AnimGraph, NodeName, ResolvedNodeTitle);
    if (!AnimBaseNode)
    {
        return true;
    }

#if MCP_HAS_ASSET_PLAYER_BASE
    UAnimGraphNode_AssetPlayerBase* AssetPlayerNode = Cast<UAnimGraphNode_AssetPlayerBase>(AnimBaseNode);
    if (!AssetPlayerNode)
    {
        Ctx.SendError(TEXT("NODE_NOT_ASSET_PLAYER"),
            FString::Printf(TEXT("Node '%s' is %s, not a UAnimGraphNode_AssetPlayerBase"),
                *ResolvedNodeTitle, *AnimBaseNode->GetClass()->GetName()));
        return true;
    }

    UAnimationAsset* Asset = Cast<UAnimationAsset>(StaticLoadObject(UAnimationAsset::StaticClass(), nullptr, *AssetPath));
    if (!Asset)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load animation asset: %s"), *AssetPath));
        return true;
    }

    TSubclassOf<UAnimationAsset> Expected = AssetPlayerNode->GetAnimationAssetClass();
    if (Expected.Get() && !Asset->IsA(Expected.Get()))
    {
        Ctx.SendError(TEXT("ASSET_CLASS_MISMATCH"),
            FString::Printf(TEXT("Expected %s, got %s"),
                *Expected->GetName(), *Asset->GetClass()->GetName()));
        return true;
    }

    AssetPlayerNode->SetAnimationAsset(Asset);

    if (AssetPlayerNode->GetAnimationAsset() != Asset)
    {
        Ctx.SendError(TEXT("BIND_FAILED"),
            FString::Printf(TEXT("SetAnimationAsset returned but GetAnimationAsset != requested asset on node '%s'"),
                *ResolvedNodeTitle));
        return true;
    }

    int32 OptionsApplied = 0;
    TArray<TSharedPtr<FJsonValue>> Warnings;

    TSharedPtr<FJsonObject> OptionsObj = Ctx.GetObject(TEXT("options"));
    if (OptionsObj.IsValid())
    {
        auto AddWarning = [&Warnings](const FString& Field, const FString& Err)
        {
            TSharedPtr<FJsonObject> W = MakeShared<FJsonObject>();
            W->SetStringField(TEXT("field"), Field);
            W->SetStringField(TEXT("error"), Err);
            Warnings.Add(MakeShared<FJsonValueObject>(W));
        };

        bool bLoop = false;
        if (OptionsObj->TryGetBoolField(TEXT("loop"), bLoop))
        {
            FString WriteErr = AnimGraphConstructionUtils::WriteAnimNodeFieldByName(
                AssetPlayerNode, FName(TEXT("bLoopAnimation")), bLoop ? TEXT("true") : TEXT("false"));
            if (WriteErr.IsEmpty()) { ++OptionsApplied; }
            else { AddWarning(TEXT("loop"), WriteErr); }
        }

        double PlayRate = 0.0;
        if (OptionsObj->TryGetNumberField(TEXT("playRate"), PlayRate))
        {
            FString WriteErr = AnimGraphConstructionUtils::WriteAnimNodeFieldByName(
                AssetPlayerNode, FName(TEXT("PlayRate")), FString::SanitizeFloat(PlayRate));
            if (WriteErr.IsEmpty()) { ++OptionsApplied; }
            else { AddWarning(TEXT("playRate"), WriteErr); }
        }

        double StartPosition = 0.0;
        if (OptionsObj->TryGetNumberField(TEXT("startPosition"), StartPosition))
        {
            FString WriteErr = AnimGraphConstructionUtils::WriteAnimNodeFieldByName(
                AssetPlayerNode, FName(TEXT("StartPosition")), FString::SanitizeFloat(StartPosition));
            if (WriteErr.IsEmpty()) { ++OptionsApplied; }
            else { AddWarning(TEXT("startPosition"), WriteErr); }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeName"), ResolvedNodeTitle);
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("expectedClass"), Expected.Get() ? Expected->GetName() : FString());
    Result->SetStringField(TEXT("boundAssetClass"), Asset->GetClass()->GetName());
    Result->SetNumberField(TEXT("optionsApplied"), OptionsApplied);
    Result->SetArrayField(TEXT("warnings"), Warnings);
    Result->SetBoolField(TEXT("success"), true);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("ASSET_PLAYER_BASE_UNAVAILABLE"),
        TEXT("Cannot bind asset: AnimGraphNode_AssetPlayerBase header not available in this build."));
#endif
#else
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        TEXT("Cannot bind asset: AnimGraph module headers not available in this build."));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_sync_group", "animation.authoring",
    "Set the sync group and role on an asset-player AnimGraph node via FAnimNode_AssetPlayerBase",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_OPT("graphName", "string", "Anim graph name (default 'AnimGraph')"),
        RPC_PARAM_REQ("nodeName", "string", "Name of the asset-player AnimGraph node to mutate"),
        RPC_PARAM_REQ("groupName", "string", "Sync group name; empty is valid only with role='Standalone'"),
        RPC_PARAM_OPT("role", "string", "Any EAnimGroupRole literal, or Standalone"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString GraphName = Ctx.GetString(TEXT("graphName"));
    FString NodeName = Ctx.GetString(TEXT("nodeName"));
    FString GroupName = Ctx.GetString(TEXT("groupName"));
    FString Role = Ctx.GetString(TEXT("role"), TEXT("CanBeLeader"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (NodeName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETERS"), TEXT("nodeName is required"));
        return true;
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::ResolveAnimBlueprintGraph(AnimBP, GraphName);
    if (!AnimGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"),
            FString::Printf(TEXT("Could not find graph '%s' in blueprint"),
                GraphName.IsEmpty() ? TEXT("AnimGraph") : *GraphName));
        return true;
    }

    FString ResolvedNodeTitle;
    UAnimGraphNode_Base* AnimBaseNode = ResolveAnimNodeOrSendError(Ctx, AnimGraph, NodeName, ResolvedNodeTitle);
    if (!AnimBaseNode)
    {
        return true;
    }

#if MCP_HAS_ASSET_PLAYER_BASE
    FString ErrorCode;
    FString ErrorMessage;
    FName AppliedGroupName = NAME_None;
    EAnimGroupRole::Type AppliedRole = EAnimGroupRole::CanBeLeader;
    EAnimSyncMethod AppliedMethod = EAnimSyncMethod::DoNotSync;
    if (!ApplyAssetPlayerSyncGroup(AnimBaseNode, ResolvedNodeTitle, GroupName, Role,
        ErrorCode, ErrorMessage, AppliedGroupName, AppliedRole, AppliedMethod))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeName"), ResolvedNodeTitle);
    Result->SetStringField(TEXT("groupName"), AppliedGroupName.IsNone() ? FString() : AppliedGroupName.ToString());
    Result->SetStringField(TEXT("role"), AnimSyncGroupRoleToString(AppliedRole));
    Result->SetStringField(TEXT("syncMethod"), AppliedMethod == EAnimSyncMethod::SyncGroup ? TEXT("SyncGroup") : TEXT("DoNotSync"));
    Result->SetBoolField(TEXT("success"), true);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("ASSET_PLAYER_BASE_UNAVAILABLE"),
        TEXT("Cannot set sync group: AnimGraphNode_AssetPlayerBase header not available in this build."));
#endif
#else
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        TEXT("Cannot set sync group: AnimGraph module headers not available in this build."));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_two_bone_ik", "animation.authoring",
    "Add a UAnimGraphNode_TwoBoneIK skeletal-control node with typed bone targets and location-space fields",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_OPT("graphName", "string", "Anim graph name (default 'AnimGraph')"),
        RPC_PARAM_REQ("ikBone", "string", "Bone controlled by the TwoBoneIK node"),
        RPC_PARAM_REQ("effectorBone", "string", "Target bone used as the effector reference"),
        RPC_PARAM_REQ("jointTargetBone", "string", "Target bone used as the joint target reference"),
        RPC_PARAM_OPT("effectorLocationSpace", "string", "BCS_WorldSpace, BCS_ComponentSpace, BCS_ParentBoneSpace, or BCS_BoneSpace"),
        RPC_PARAM_OPT("jointTargetLocationSpace", "string", "BCS_WorldSpace, BCS_ComponentSpace, BCS_ParentBoneSpace, or BCS_BoneSpace"),
        RPC_PARAM_REQ("x", "number", "X position in graph"),
        RPC_PARAM_REQ("y", "number", "Y position in graph"),
        RPC_PARAM_OPT("alpha", "number", "Skeletal-control alpha; defaults to the engine node default"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH && MCP_HAS_TWO_BONE_IK
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString GraphName = Ctx.GetString(TEXT("graphName"));
    FString IkBone = Ctx.GetString(TEXT("ikBone"));
    FString EffectorBone = Ctx.GetString(TEXT("effectorBone"));
    FString JointTargetBone = Ctx.GetString(TEXT("jointTargetBone"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    double X = 0.0;
    double Y = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), X)) { return true; }
    if (!Ctx.RequireNumber(TEXT("y"), Y)) { return true; }

    if (IkBone.IsEmpty() || EffectorBone.IsEmpty() || JointTargetBone.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETERS"), TEXT("ikBone, effectorBone, and jointTargetBone are required"));
        return true;
    }

    EBoneControlSpace EffectorLocationSpace = BCS_ComponentSpace;
    FString EffectorLocationSpaceText = Ctx.GetString(TEXT("effectorLocationSpace"));
    if (!EffectorLocationSpaceText.IsEmpty() && !ParseBoneControlSpace(EffectorLocationSpaceText, EffectorLocationSpace))
    {
        Ctx.SendError(TEXT("INVALID_BONE_CONTROL_SPACE"),
            FString::Printf(TEXT("Unknown effectorLocationSpace '%s'"), *EffectorLocationSpaceText));
        return true;
    }

    EBoneControlSpace JointTargetLocationSpace = BCS_ComponentSpace;
    FString JointTargetLocationSpaceText = Ctx.GetString(TEXT("jointTargetLocationSpace"));
    if (!JointTargetLocationSpaceText.IsEmpty() && !ParseBoneControlSpace(JointTargetLocationSpaceText, JointTargetLocationSpace))
    {
        Ctx.SendError(TEXT("INVALID_BONE_CONTROL_SPACE"),
            FString::Printf(TEXT("Unknown jointTargetLocationSpace '%s'"), *JointTargetLocationSpaceText));
        return true;
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }
    if (!AnimBP->TargetSkeleton)
    {
        Ctx.SendError(TEXT("SKELETON_NOT_FOUND"), FString::Printf(TEXT("Animation blueprint has no target skeleton: %s"), *BlueprintPath));
        return true;
    }

    auto ValidateBone = [&](const FString& FieldName, const FString& BoneName) -> bool
    {
        if (AnimSkeletonHasBone(AnimBP, BoneName))
        {
            return true;
        }
        Ctx.SendError(TEXT("BONE_NOT_FOUND"),
            FString::Printf(TEXT("%s bone '%s' was not found on skeleton '%s'"),
                *FieldName, *BoneName, *AnimBP->TargetSkeleton->GetName()));
        return false;
    };
    if (!ValidateBone(TEXT("ikBone"), IkBone) ||
        !ValidateBone(TEXT("effectorBone"), EffectorBone) ||
        !ValidateBone(TEXT("jointTargetBone"), JointTargetBone))
    {
        return true;
    }

    UEdGraph* Graph = AnimGraphConstructionUtils::ResolveAnimBlueprintGraph(AnimBP, GraphName);
    if (!Graph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"),
            FString::Printf(TEXT("Could not find graph '%s' in blueprint"),
                GraphName.IsEmpty() ? TEXT("AnimGraph") : *GraphName));
        return true;
    }

    UAnimGraphNode_TwoBoneIK* Node = Cast<UAnimGraphNode_TwoBoneIK>(
        AnimGraphConstructionUtils::CreateAnimNode(Graph, UAnimGraphNode_TwoBoneIK::StaticClass(), FVector2D(X, Y)));
    if (!Node)
    {
        Ctx.SendError(TEXT("NODE_CREATE_FAILED"), TEXT("CreateAnimNode returned null for UAnimGraphNode_TwoBoneIK"));
        return true;
    }

    Node->Node.IKBone.BoneName = FName(*IkBone);
    Node->Node.EffectorTarget.bUseSocket = false;
    Node->Node.EffectorTarget.BoneReference.BoneName = FName(*EffectorBone);
    Node->Node.JointTarget.bUseSocket = false;
    Node->Node.JointTarget.BoneReference.BoneName = FName(*JointTargetBone);
    Node->Node.EffectorLocationSpace = EffectorLocationSpace;
    Node->Node.JointTargetLocationSpace = JointTargetLocationSpace;
    Node->Node.Alpha = static_cast<float>(Ctx.GetNumber(TEXT("alpha"), Node->Node.Alpha));
    Node->ReconstructNode();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetName());
    Result->SetStringField(TEXT("nodeName"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
    Result->SetNumberField(TEXT("nodePosX"), X);
    Result->SetNumberField(TEXT("nodePosY"), Y);
    Result->SetStringField(TEXT("ikBone"), IkBone);
    Result->SetStringField(TEXT("effectorBone"), EffectorBone);
    Result->SetStringField(TEXT("jointTargetBone"), JointTargetBone);
    Result->SetStringField(TEXT("effectorLocationSpace"), BoneControlSpaceToString(Node->Node.EffectorLocationSpace.GetValue()));
    Result->SetStringField(TEXT("jointTargetLocationSpace"), BoneControlSpaceToString(Node->Node.JointTargetLocationSpace.GetValue()));
    Result->SetNumberField(TEXT("alpha"), Node->Node.Alpha);
    Result->SetBoolField(TEXT("success"), true);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        TEXT("Cannot create TwoBoneIK node: AnimGraph TwoBoneIK headers not available in this build."));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_modify_bone", "animation.authoring",
    "Add a UAnimGraphNode_ModifyBone skeletal-control node with typed transform spaces and modification modes",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_OPT("graphName", "string", "Anim graph name (default 'AnimGraph')"),
        RPC_PARAM_REQ("boneName", "string", "Bone modified by the node"),
        RPC_PARAM_OPT("translation", "object", "Optional {x,y,z} translation payload"),
        RPC_PARAM_OPT("rotation", "object", "Optional {pitch,yaw,roll} or quaternion {x,y,z,w} rotation payload"),
        RPC_PARAM_OPT("scale", "object", "Optional {x,y,z} scale payload"),
        RPC_PARAM_OPT("translationSpace", "string", "BCS_WorldSpace, BCS_ComponentSpace, BCS_ParentBoneSpace, or BCS_BoneSpace"),
        RPC_PARAM_OPT("rotationSpace", "string", "BCS_WorldSpace, BCS_ComponentSpace, BCS_ParentBoneSpace, or BCS_BoneSpace"),
        RPC_PARAM_OPT("scaleSpace", "string", "BCS_WorldSpace, BCS_ComponentSpace, BCS_ParentBoneSpace, or BCS_BoneSpace"),
        RPC_PARAM_OPT("translationMode", "string", "Ignore, Replace, Additive, or BMM_* equivalent"),
        RPC_PARAM_OPT("rotationMode", "string", "Ignore, Replace, Additive, or BMM_* equivalent"),
        RPC_PARAM_OPT("scaleMode", "string", "Ignore, Replace, Additive, or BMM_* equivalent"),
        RPC_PARAM_REQ("x", "number", "X position in graph"),
        RPC_PARAM_REQ("y", "number", "Y position in graph"),
        RPC_PARAM_OPT("alpha", "number", "Skeletal-control alpha; defaults to the engine node default"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH && MCP_HAS_MODIFY_BONE
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString GraphName = Ctx.GetString(TEXT("graphName"));
    FString BoneName = Ctx.GetString(TEXT("boneName"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    double X = 0.0;
    double Y = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), X)) { return true; }
    if (!Ctx.RequireNumber(TEXT("y"), Y)) { return true; }

    if (BoneName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETERS"), TEXT("boneName is required"));
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const bool bHasTranslationPayload = Payload.IsValid() && Payload->HasField(TEXT("translation"));
    const bool bHasRotationPayload = Payload.IsValid() && Payload->HasField(TEXT("rotation"));
    const bool bHasScalePayload = Payload.IsValid() && Payload->HasField(TEXT("scale"));

    TSharedPtr<FJsonObject> TranslationObj = Ctx.GetObject(TEXT("translation"));
    TSharedPtr<FJsonObject> RotationObj = Ctx.GetObject(TEXT("rotation"));
    TSharedPtr<FJsonObject> ScaleObj = Ctx.GetObject(TEXT("scale"));
    if ((bHasTranslationPayload && !TranslationObj.IsValid()) ||
        (bHasRotationPayload && !RotationObj.IsValid()) ||
        (bHasScalePayload && !ScaleObj.IsValid()))
    {
        Ctx.SendError(TEXT("INVALID_TRANSFORM_PAYLOAD"), TEXT("translation, rotation, and scale must be JSON objects when present"));
        return true;
    }

    EBoneControlSpace TranslationSpace = BCS_ComponentSpace;
    EBoneControlSpace RotationSpace = BCS_ComponentSpace;
    EBoneControlSpace ScaleSpace = BCS_ComponentSpace;
    auto ParseSpaceField = [&](const TCHAR* FieldName, EBoneControlSpace& OutSpace) -> bool
    {
        FString SpaceText = Ctx.GetString(FieldName);
        if (SpaceText.IsEmpty() || ParseBoneControlSpace(SpaceText, OutSpace))
        {
            return true;
        }
        Ctx.SendError(TEXT("INVALID_BONE_CONTROL_SPACE"),
            FString::Printf(TEXT("Unknown %s '%s'"), FieldName, *SpaceText));
        return false;
    };
    if (!ParseSpaceField(TEXT("translationSpace"), TranslationSpace) ||
        !ParseSpaceField(TEXT("rotationSpace"), RotationSpace) ||
        !ParseSpaceField(TEXT("scaleSpace"), ScaleSpace))
    {
        return true;
    }

    EBoneModificationMode TranslationMode = bHasTranslationPayload ? BMM_Replace : BMM_Ignore;
    EBoneModificationMode RotationMode = bHasRotationPayload ? BMM_Replace : BMM_Ignore;
    EBoneModificationMode ScaleMode = bHasScalePayload ? BMM_Replace : BMM_Ignore;
    auto ParseModeField = [&](const TCHAR* FieldName, EBoneModificationMode& OutMode) -> bool
    {
        FString ModeText = Ctx.GetString(FieldName);
        if (ModeText.IsEmpty() || ParseBoneModificationMode(ModeText, OutMode))
        {
            return true;
        }
        Ctx.SendError(TEXT("INVALID_BONE_MODIFICATION_MODE"),
            FString::Printf(TEXT("Unknown %s '%s'"), FieldName, *ModeText));
        return false;
    };
    if (!ParseModeField(TEXT("translationMode"), TranslationMode) ||
        !ParseModeField(TEXT("rotationMode"), RotationMode) ||
        !ParseModeField(TEXT("scaleMode"), ScaleMode))
    {
        return true;
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }
    if (!AnimBP->TargetSkeleton)
    {
        Ctx.SendError(TEXT("SKELETON_NOT_FOUND"), FString::Printf(TEXT("Animation blueprint has no target skeleton: %s"), *BlueprintPath));
        return true;
    }
    if (!AnimSkeletonHasBone(AnimBP, BoneName))
    {
        Ctx.SendError(TEXT("BONE_NOT_FOUND"),
            FString::Printf(TEXT("boneName '%s' was not found on skeleton '%s'"),
                *BoneName, *AnimBP->TargetSkeleton->GetName()));
        return true;
    }

    UEdGraph* Graph = AnimGraphConstructionUtils::ResolveAnimBlueprintGraph(AnimBP, GraphName);
    if (!Graph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"),
            FString::Printf(TEXT("Could not find graph '%s' in blueprint"),
                GraphName.IsEmpty() ? TEXT("AnimGraph") : *GraphName));
        return true;
    }

    UAnimGraphNode_ModifyBone* Node = Cast<UAnimGraphNode_ModifyBone>(
        AnimGraphConstructionUtils::CreateAnimNode(Graph, UAnimGraphNode_ModifyBone::StaticClass(), FVector2D(X, Y)));
    if (!Node)
    {
        Ctx.SendError(TEXT("NODE_CREATE_FAILED"), TEXT("CreateAnimNode returned null for UAnimGraphNode_ModifyBone"));
        return true;
    }

    Node->Node.BoneToModify.BoneName = FName(*BoneName);
    if (TranslationObj.IsValid())
    {
        Node->Node.Translation = AnimationAuthoringHelpers::GetVectorFromJsonAnim(TranslationObj);
    }
    if (RotationObj.IsValid())
    {
        Node->Node.Rotation = AnimationAuthoringHelpers::GetRotatorFromJsonAnim(RotationObj);
    }
    if (ScaleObj.IsValid())
    {
        Node->Node.Scale = FVector(
            GetJsonNumberField(ScaleObj, TEXT("x"), 1.0),
            GetJsonNumberField(ScaleObj, TEXT("y"), 1.0),
            GetJsonNumberField(ScaleObj, TEXT("z"), 1.0));
    }
    Node->Node.TranslationSpace = TranslationSpace;
    Node->Node.RotationSpace = RotationSpace;
    Node->Node.ScaleSpace = ScaleSpace;
    Node->Node.TranslationMode = TranslationMode;
    Node->Node.RotationMode = RotationMode;
    Node->Node.ScaleMode = ScaleMode;
    Node->Node.Alpha = static_cast<float>(Ctx.GetNumber(TEXT("alpha"), Node->Node.Alpha));
    Node->ReconstructNode();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetName());
    Result->SetStringField(TEXT("nodeName"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
    Result->SetNumberField(TEXT("nodePosX"), X);
    Result->SetNumberField(TEXT("nodePosY"), Y);
    Result->SetStringField(TEXT("boneName"), BoneName);
    Result->SetStringField(TEXT("translationSpace"), BoneControlSpaceToString(Node->Node.TranslationSpace.GetValue()));
    Result->SetStringField(TEXT("rotationSpace"), BoneControlSpaceToString(Node->Node.RotationSpace.GetValue()));
    Result->SetStringField(TEXT("scaleSpace"), BoneControlSpaceToString(Node->Node.ScaleSpace.GetValue()));
    Result->SetStringField(TEXT("translationMode"), BoneModificationModeToString(Node->Node.TranslationMode.GetValue()));
    Result->SetStringField(TEXT("rotationMode"), BoneModificationModeToString(Node->Node.RotationMode.GetValue()));
    Result->SetStringField(TEXT("scaleMode"), BoneModificationModeToString(Node->Node.ScaleMode.GetValue()));
    Result->SetNumberField(TEXT("alpha"), Node->Node.Alpha);
    Result->SetBoolField(TEXT("success"), true);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        TEXT("Cannot create ModifyBone node: AnimGraph ModifyBone headers not available in this build."));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_graph_node", "animation.authoring",
    "Generic typed creator for any UAnimGraphNode_* class on an AnimBlueprint graph. "
    "Resolves nodeClass via ClassUtils::ResolveClassByName, validates IsChildOf(UAnimGraphNode_Base), "
    "creates the node at (x, y), then optionally applies typed syncGroup/syncRole and reflection-based property writes, "
    "toggles bShowPin via ToggleOptionalPinExposed, and binds a UAnimationAsset via SetAnimationAsset "
    "(asset-player nodes) or reflective FObjectProperty assignment (other nodes).",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the animation blueprint"),
        RPC_PARAM_OPT("graphName", "string", "Anim graph name (default 'AnimGraph')"),
        RPC_PARAM_REQ("nodeClass", "classref", "AnimGraphNode_* class short name or full path"),
        RPC_PARAM_REQ("x", "number", "X position in graph"),
        RPC_PARAM_REQ("y", "number", "Y position in graph"),
        RPC_PARAM_OPT("properties", "object", "Property name -> JSON value map; asset-player syncGroup/syncRole route through typed sync-group handling before reflection"),
        RPC_PARAM_OPT("exposePins", "array", "List of FAnimNode_* property names to mark bShowPin=true"),
        RPC_PARAM_OPT("bindAsset", "path", "Asset path for player-style nodes (asset class derived from node)"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
#if MCP_HAS_ANIM_STATE_MACHINE_GRAPH
    FString BlueprintPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("blueprintPath")));
    FString GraphName = Ctx.GetString(TEXT("graphName"));
    FString NodeClassStr = Ctx.GetString(TEXT("nodeClass"));
    FString BindAsset = Ctx.GetString(TEXT("bindAsset"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    double X = 0.0;
    double Y = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), X)) { return true; }
    if (!Ctx.RequireNumber(TEXT("y"), Y)) { return true; }

    if (NodeClassStr.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETERS"), TEXT("nodeClass is required"));
        return true;
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ANIM_BP_NOT_FOUND"), FString::Printf(TEXT("Could not load animation blueprint: %s"), *BlueprintPath));
        return true;
    }

    UEdGraph* Graph = AnimGraphConstructionUtils::ResolveAnimBlueprintGraph(AnimBP, GraphName);
    if (!Graph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"),
            FString::Printf(TEXT("Could not find graph '%s' in blueprint"),
                GraphName.IsEmpty() ? TEXT("AnimGraph") : *GraphName));
        return true;
    }

    UClass* NodeClass = ResolveClassByName(NodeClassStr);
    if (!NodeClass || !NodeClass->IsChildOf(UAnimGraphNode_Base::StaticClass()))
    {
        Ctx.SendError(TEXT("INVALID_ANIM_NODE_CLASS"),
            FString::Printf(TEXT("nodeClass '%s' did not resolve to a UAnimGraphNode_Base descendant"),
                *NodeClassStr));
        return true;
    }

    UAnimGraphNode_Base* Node = AnimGraphConstructionUtils::CreateAnimNode(Graph, NodeClass, FVector2D(X, Y));
    if (!Node)
    {
        Ctx.SendError(TEXT("NODE_CREATE_FAILED"),
            FString::Printf(TEXT("CreateAnimNode returned null for class '%s'"), *NodeClass->GetName()));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> PropertiesApplied;
    TArray<TSharedPtr<FJsonValue>> PropertiesFailed;
    TArray<TSharedPtr<FJsonValue>> PinsExposed;
    TArray<TSharedPtr<FJsonValue>> PinsFailed;
    TArray<TSharedPtr<FJsonValue>> Warnings;

    const TSharedPtr<FJsonObject>& RawPayload = Ctx.GetRawPayload();
    const TSharedPtr<FJsonObject>* PropsObjPtr = nullptr;
    if (RawPayload.IsValid() && RawPayload->TryGetObjectField(TEXT("properties"), PropsObjPtr) && PropsObjPtr && PropsObjPtr->IsValid())
    {
        const TSharedPtr<FJsonObject>& PropsObj = *PropsObjPtr;
        auto AddPropertyFailure = [&PropertiesFailed](const FString& Name, const FString& Error)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Name);
            Entry->SetStringField(TEXT("error"), Error);
            PropertiesFailed.Add(MakeShared<FJsonValueObject>(Entry));
        };

        bool bHasSyncGroup = PropsObj->HasField(TEXT("syncGroup"));
        bool bHasSyncRole = PropsObj->HasField(TEXT("syncRole"));
        FString SyncGroup;
        FString SyncRole;
        bool bSyncPropertiesValid = true;
        if (bHasSyncGroup && !PropsObj->TryGetStringField(TEXT("syncGroup"), SyncGroup))
        {
            AddPropertyFailure(TEXT("syncGroup"), TEXT("syncGroup must be a string"));
            bSyncPropertiesValid = false;
        }
        if (bHasSyncRole && !PropsObj->TryGetStringField(TEXT("syncRole"), SyncRole))
        {
            AddPropertyFailure(TEXT("syncRole"), TEXT("syncRole must be a string"));
            bSyncPropertiesValid = false;
        }
        if (bSyncPropertiesValid && (bHasSyncGroup || bHasSyncRole))
        {
#if MCP_HAS_ASSET_PLAYER_BASE
            FString ErrorCode;
            FString ErrorMessage;
            FName AppliedGroupName = NAME_None;
            EAnimGroupRole::Type AppliedRole = EAnimGroupRole::CanBeLeader;
            EAnimSyncMethod AppliedMethod = EAnimSyncMethod::DoNotSync;
            if (ApplyAssetPlayerSyncGroup(Node, Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                SyncGroup, SyncRole, ErrorCode, ErrorMessage, AppliedGroupName, AppliedRole, AppliedMethod))
            {
                if (bHasSyncGroup)
                {
                    PropertiesApplied.Add(MakeShared<FJsonValueString>(TEXT("syncGroup")));
                }
                if (bHasSyncRole)
                {
                    PropertiesApplied.Add(MakeShared<FJsonValueString>(TEXT("syncRole")));
                }
            }
            else
            {
                AddPropertyFailure(bHasSyncRole ? FString(TEXT("syncRole")) : FString(TEXT("syncGroup")),
                    FString::Printf(TEXT("%s: %s"), *ErrorCode, *ErrorMessage));
            }
#else
            AddPropertyFailure(TEXT("syncGroup"), TEXT("ASSET_PLAYER_BASE_UNAVAILABLE"));
#endif
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : PropsObj->Values)
        {
            if (Pair.Key.Equals(TEXT("syncGroup"), ESearchCase::IgnoreCase) ||
                Pair.Key.Equals(TEXT("syncRole"), ESearchCase::IgnoreCase))
            {
                continue;
            }

            FProperty* P = Node->GetClass()->FindPropertyByName(FName(*Pair.Key));
            if (!P)
            {
                FString RuntimeErr;
                if (AnimGraphConstructionUtils::ApplyJsonValueToAnimNodeFieldByName(
                    Node, FName(*Pair.Key), Pair.Value, RuntimeErr))
                {
                    PropertiesApplied.Add(MakeShared<FJsonValueString>(Pair.Key));
                    continue;
                }

                AddPropertyFailure(Pair.Key, RuntimeErr.IsEmpty() ? FString(TEXT("property not found")) : RuntimeErr);
                continue;
            }

            FString ErrStr;
            if (!ApplyJsonValueToProperty(Node, P, Pair.Value, ErrStr))
            {
                AddPropertyFailure(Pair.Key, ErrStr);
                continue;
            }

            PropertiesApplied.Add(MakeShared<FJsonValueString>(Pair.Key));
        }
    }

    if (const TArray<TSharedPtr<FJsonValue>>* ExposePins = Ctx.GetArray(TEXT("exposePins")))
    {
        for (const TSharedPtr<FJsonValue>& Entry : *ExposePins)
        {
            if (!Entry.IsValid()) { continue; }
            FString PinName;
            if (!Entry->TryGetString(PinName) || PinName.IsEmpty()) { continue; }
            FString OutErr;
            if (AnimGraphConstructionUtils::ToggleOptionalPinExposed(Node, FName(*PinName), true, OutErr))
            {
                PinsExposed.Add(MakeShared<FJsonValueString>(PinName));
            }
            else
            {
                TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
                Failure->SetStringField(TEXT("name"), PinName);
                Failure->SetStringField(TEXT("error"), OutErr);
                PinsFailed.Add(MakeShared<FJsonValueObject>(Failure));
            }
        }
    }

    bool bAssetBound = false;
    if (!BindAsset.IsEmpty())
    {
        UAnimationAsset* Asset = Cast<UAnimationAsset>(StaticLoadObject(UAnimationAsset::StaticClass(), nullptr, *BindAsset));
        if (!Asset)
        {
            Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("ASSET_NOT_FOUND: %s"), *BindAsset)));
        }
        else
        {
#if MCP_HAS_ASSET_PLAYER_BASE
            if (UAnimGraphNode_AssetPlayerBase* APN = Cast<UAnimGraphNode_AssetPlayerBase>(Node))
            {
                TSubclassOf<UAnimationAsset> Exp = APN->GetAnimationAssetClass();
                if (Exp.Get() && !Asset->IsA(Exp.Get()))
                {
                    Warnings.Add(MakeShared<FJsonValueString>(
                        FString::Printf(TEXT("ASSET_CLASS_MISMATCH: expected %s, got %s"),
                            *Exp->GetName(), *Asset->GetClass()->GetName())));
                }
                else
                {
                    APN->SetAnimationAsset(Asset);
                    bAssetBound = true;
                }
            }
            else
#endif
            {
                FStructProperty* NodeProp = PinWright::Anim::GetFNodeProperty(Node);
                bool bSlotFound = false;
                if (NodeProp && NodeProp->Struct)
                {
                    void* NodeData = NodeProp->ContainerPtrToValuePtr<void>(Node);
                    for (TFieldIterator<FObjectProperty> It(NodeProp->Struct); It; ++It)
                    {
                        FObjectProperty* OP = *It;
                        if (OP && OP->PropertyClass &&
                            OP->PropertyClass->IsChildOf(UAnimationAsset::StaticClass()) &&
                            Asset->GetClass()->IsChildOf(OP->PropertyClass))
                        {
                            void* ValuePtr = OP->ContainerPtrToValuePtr<void>(NodeData);
                            OP->SetObjectPropertyValue(ValuePtr, Asset);
                            bAssetBound = true;
                            bSlotFound = true;
                            break;
                        }
                    }
                }
                if (!bSlotFound)
                {
                    Warnings.Add(MakeShared<FJsonValueString>(TEXT("BIND_ASSET_NO_SLOT")));
                }
            }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(AnimBP, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeClass"), NodeClass->GetName());
    Result->SetStringField(TEXT("nodeName"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
    Result->SetNumberField(TEXT("nodePosX"), X);
    Result->SetNumberField(TEXT("nodePosY"), Y);
    Result->SetArrayField(TEXT("propertiesApplied"), PropertiesApplied);
    Result->SetArrayField(TEXT("propertiesFailed"), PropertiesFailed);
    Result->SetArrayField(TEXT("pinsExposed"), PinsExposed);
    Result->SetArrayField(TEXT("pinsFailed"), PinsFailed);
    Result->SetBoolField(TEXT("assetBound"), bAssetBound);
    Result->SetArrayField(TEXT("warnings"), Warnings);
    Result->SetBoolField(TEXT("success"), true);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE"),
        TEXT("Cannot create anim graph node: AnimGraph module headers not available in this build."));
#endif
    return true;
}

// ===========================================================================
// 10.5 CONTROL RIG
// ===========================================================================

REGISTER_RPC_HANDLER("animation.authoring.create_control_rig", "animation.authoring",
    "Create a UControlRigBlueprint asset (procedural rig graph). UE 5.5+ only. When skeletalMeshPath is given the rig is initialized from that mesh's skeleton; otherwise the rig starts empty. Add controls and rig units afterwards via animation.authoring.add_control / add_rig_unit.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset filename (no extension)."),
        RPC_PARAM_OPT("path", "path", "Content-browser folder; defaults to /Game/ControlRigs."),
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Optional SkeletalMesh asset path used to seed the rig hierarchy from the mesh's skeleton."),
        RPC_PARAM_OPT("modularRig", "boolean", "When true creates a Modular Control Rig (UE 5.5+ feature for composable rigs); defaults to false."),
        RPC_PARAM_OPT("save", "boolean", "When true (default) marks the new asset dirty.")
    ))
{
// ControlRig factory static methods (CreateNewControlRigAsset, CreateControlRigFromSkeletalMeshOrSkeleton)
// are only available in UE 5.5+ where ControlRigBlueprintFactory.h is in Public folder
#if MCP_HAS_CONTROLRIG_BLUEPRINT && UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/ControlRigs")));
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    bool bModularRig = Ctx.GetBool(TEXT("modularRig"), false);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_NAME"), TEXT("Name is required"));
        return true;
    }

    UControlRigBlueprint* ControlRigBP = nullptr;
    FString FullPath = Path / Name;

    // If skeletal mesh provided, create from it; otherwise create empty
    if (!SkeletalMeshPath.IsEmpty())
    {
        USkeletalMesh* SkeletalMesh = AnimationAuthoringHelpers::LoadSkeletalMeshFromPathAnim(SkeletalMeshPath);
        if (!SkeletalMesh)
        {
            Ctx.SendError(TEXT("SKELETAL_MESH_NOT_FOUND"), FString::Printf(TEXT("Could not load skeletal mesh: %s"), *SkeletalMeshPath));
            return true;
        }

        // Use static factory method to create from skeletal mesh (UE 5.5+ only)
        ControlRigBP = UControlRigBlueprintFactory::CreateControlRigFromSkeletalMeshOrSkeleton(SkeletalMesh, bModularRig);
    }
    else
    {
        // Create empty control rig at specified path (UE 5.5+ only)
        ControlRigBP = UControlRigBlueprintFactory::CreateNewControlRigAsset(FullPath, bModularRig);
    }

    if (!ControlRigBP)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create Control Rig blueprint"));
        return true;
    }

    // Save if requested
    if (bSave)
    {
        // Use safe asset save helper
        FString AssetPath = ControlRigBP->GetPathName();
        int32 DotIndex = AssetPath.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
        if (DotIndex != INDEX_NONE) { AssetPath.LeftInline(DotIndex); }
        ControlRigBP->MarkPackageDirty();
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), ControlRigBP->GetPathName());
    Result->SetBoolField(TEXT("modularRig"), bModularRig);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Control Rig '%s' created successfully"), *Name));
    Ctx.SendSuccess(Result);
#elif MCP_HAS_CONTROLRIG_BLUEPRINT
    // Factory static methods not available in UE 5.1-5.4 (header is in Private folder)
    // Use McpCreateControlRigBlueprint() from AssetUtils as fallback
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/ControlRigs")));
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_NAME"), TEXT("Name is required"));
        return true;
    }

    FString FullPath = Path / Name;

    // Create Control Rig Blueprint using FKismetEditorUtilities (works in all UE 5.x versions).
    // The destination is composed through the checked helper rather than by bare concatenation:
    // CreatePackage logs at Fatal (UObjectGlobals.cpp:1094-1096) for a name containing "//" - a
    // verbosity compiled out in no configuration - so an unvalidated `name` did not fail the call,
    // it ended the editor PROCESS and every unsaved package in it (measured on board
    // B-foliage-add-type-name-with-slash-kills-the-editor). Unlike create_pose_library below,
    // this branch resolves nothing before the composition, so a bad `name` here reached the Fatal
    // with no earlier refusal to intercept it - which is also why the regression test drives
    // create_pose_library and not this verb.
    FString FullPackageName;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(Path, Name, FullPackageName, PathError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("%s Pass a bare asset name and choose the folder with path "
                                 "(default /Game/ControlRigs)."), *PathError));
        return true;
    }

    // Create the package
    UPackage* Package = CreatePackage(*FullPackageName);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_CREATE_FAILED"), FString::Printf(TEXT("Failed to create package: %s"), *FullPackageName));
        return true;
    }

    Package->FullyLoad();

    // Create the Control Rig Blueprint using FKismetEditorUtilities
    UControlRigBlueprint* ControlRigBP = Cast<UControlRigBlueprint>(
        FKismetEditorUtilities::CreateBlueprint(
            UControlRig::StaticClass(),
            Package,
            *Name,
            BPTYPE_Normal,
            UControlRigBlueprint::StaticClass(),
            URigVMBlueprintGeneratedClass::StaticClass(),
            NAME_None));

    if (!ControlRigBP)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create Control Rig Blueprint"));
        return true;
    }

    // Set the target skeleton if provided (via skeletal mesh)
    if (!SkeletalMeshPath.IsEmpty())
    {
        USkeletalMesh* SkeletalMesh = AnimationAuthoringHelpers::LoadSkeletalMeshFromPathAnim(SkeletalMeshPath);
        if (SkeletalMesh && SkeletalMesh->GetSkeleton())
        {
            USkeletalMesh* PreviewMesh = SkeletalMesh->GetSkeleton()->GetPreviewMesh();
            if (PreviewMesh)
            {
                ControlRigBP->SetPreviewMesh(PreviewMesh);
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), ControlRigBP->GetPathName());
    Result->SetBoolField(TEXT("modularRig"), false);  // Not supported in fallback
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Control Rig '%s' created successfully (UE 5.1-5.4 compatible mode)"), *Name));
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("NOT_SUPPORTED"), TEXT("Control Rig module not available"));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.create_pose_library", "animation.authoring",
    "Create a pose library (pose asset)",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the pose library"),
        RPC_PARAM_OPT("path", "path", "Save path (default /Game/Animations)"),
        RPC_PARAM_REQ("skeletonPath", "path", "Path to the skeleton asset"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
#if MCP_HAS_POSEASSET
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Animations")));
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
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
    FString FullPackageName;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(Path, Name, FullPackageName, PathError))
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

    // Actually create the UPoseAsset so the returned assetPath resolves to a real asset
    // (previously this branch synthesized a success envelope without creating anything —
    // ticket B-create-pose-library-noop-fake-success). A fresh empty pose library bound to
    // the skeleton is a valid asset; animators add poses afterward. UPoseAssetFactory is not
    // usable headlessly (its ConfigureProperties pops a modal asset picker and FactoryCreateNew
    // requires a SourceAnimation), so we replicate what the factory does internally: NewObject +
    // SetSkeleton, mirroring create_ik_rig / create_control_rig in this file.
    UPackage* Package = CreatePackage(*FullPackageName);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_CREATE_FAILED"), FString::Printf(TEXT("Failed to create package: %s"), *FullPackageName));
        return true;
    }
    Package->FullyLoad();

    UPoseAsset* PoseAsset = NewObject<UPoseAsset>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
    if (!PoseAsset)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), FString::Printf(TEXT("Failed to create pose asset '%s'"), *Name));
        return true;
    }

    PoseAsset->SetSkeleton(Skeleton);

    // Mark dirty + notify the asset registry (SaveAnimAsset deliberately does not write to disk,
    // to avoid modal save dialogs under automation). asset.exists on the returned path is now true.
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    AnimationAuthoringHelpers::SaveAnimAsset(PoseAsset, bSave, SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), PoseAsset->GetPathName());
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Pose library '%s' created (empty; add poses via the Pose Asset editor)"), *Name));
    AddAssetVerification(Result, PoseAsset);
    AddAssetSaveReport(Result, bSave, /*bSavedToDisk=*/false, SaveState);
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("NOT_SUPPORTED"), TEXT("Pose Asset not available in this engine version"));
#endif
    return true;
}

// ===========================================================================
// 10.6 RETARGETING
// ===========================================================================

REGISTER_RPC_HANDLER("animation.authoring.create_ik_rig", "animation.authoring",
    "Create a UIKRigDefinition asset (modern IK + retargeting source). Add chains via animation.authoring.add_ik_chain, then pair two IK Rigs into an IK Retargeter via create_ik_retargeter for cross-skeleton retargeting workflows.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset filename (no extension)."),
        RPC_PARAM_OPT("path", "path", "Content-browser folder; defaults to /Game/Retargeting."),
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Optional SkeletalMesh asset path used as the rig's preview mesh and source of bone hierarchy."),
        RPC_PARAM_OPT("save", "boolean", "When true (default) marks the new asset dirty.")
    ))
{
#if MCP_HAS_IKRIG_FACTORY && MCP_HAS_IKRIG
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Retargeting")));
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_NAME"), TEXT("Name is required"));
        return true;
    }

    // Create the IK Rig asset. The static UIKRigDefinitionFactory::CreateNewIKRigAsset
    // convenience method only exists on UE 5.6+; on older versions replicate its body
    // (the engine's own implementation) via AssetTools + the UFactory.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    UIKRigDefinition* IKRig = UIKRigDefinitionFactory::CreateNewIKRigAsset(Path, Name);
#else
    UIKRigDefinition* IKRig = nullptr;
    {
        FString DesiredPackagePath = Path;
        if (!DesiredPackagePath.EndsWith(TEXT("/")))
        {
            DesiredPackagePath.Append(TEXT("/"));
        }
        DesiredPackagePath = DesiredPackagePath / Name;

        FString UniqueAssetName = Name;
        FString UniquePackageName;
        const FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
        AssetToolsModule.Get().CreateUniqueAssetName(DesiredPackagePath, TEXT(""), UniquePackageName, UniqueAssetName);
        if (UniquePackageName.EndsWith(UniqueAssetName))
        {
            UniquePackageName = UniquePackageName.LeftChop(UniqueAssetName.Len() + 1);
        }

        UIKRigDefinitionFactory* Factory = NewObject<UIKRigDefinitionFactory>();
        UObject* NewAsset = AssetToolsModule.Get().CreateAsset(*UniqueAssetName, *UniquePackageName, nullptr, Factory);
        IKRig = Cast<UIKRigDefinition>(NewAsset);
    }
#endif

    if (!IKRig)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create IK Rig asset"));
        return true;
    }

    // If skeletal mesh path provided, set the preview mesh
    if (!SkeletalMeshPath.IsEmpty())
    {
        USkeletalMesh* SkeletalMesh = AnimationAuthoringHelpers::LoadSkeletalMeshFromPathAnim(SkeletalMeshPath);
        if (SkeletalMesh)
        {
            IKRig->SetPreviewMesh(SkeletalMesh);
        }
    }

    // Save if requested
    if (bSave)
    {
        FString AssetPathStr = IKRig->GetPathName();
        int32 DotIndex = AssetPathStr.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
        if (DotIndex != INDEX_NONE) { AssetPathStr.LeftInline(DotIndex); }
        IKRig->MarkPackageDirty();
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), IKRig->GetPathName());
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("IK Rig '%s' created successfully"), *Name));
    Ctx.SendSuccess(Result);
#elif MCP_HAS_IKRIG
    // Factory not available, fall back to informative message
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Retargeting")));
    FString FullPath = Path / Name;
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), FullPath);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("IK Rig '%s' creation requires IKRigEditor module"), *Name));
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("NOT_SUPPORTED"), TEXT("IK Rig module not available"));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.add_ik_chain", "animation.authoring",
    "Add a retarget chain to an IK Rig (UIKRigDefinition). The chain spans from startBone to "
    "endBone along the rig's skeleton and is the unit cross-skeleton retargeting maps between "
    "source and target rigs. The chain survives asset.dump. Requires the IKRig editor module.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the IK Rig (UIKRigDefinition)."),
        RPC_PARAM_REQ("chainName", "string", "Name of the retarget chain to add."),
        RPC_PARAM_REQ("startBone", "string", "Name of the first bone in the chain (must exist on the rig skeleton)."),
        RPC_PARAM_REQ("endBone", "string", "Name of the last bone in the chain (must exist on the rig skeleton)."),
        RPC_PARAM_OPT("goalName", "string", "Optional IK goal name to associate with the chain.")
    ))
{
#if MCP_HAS_IKRIG_CONTROLLER && MCP_HAS_IKRIG
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString ChainName = Ctx.GetString(TEXT("chainName"));
    FString StartBone = Ctx.GetString(TEXT("startBone"));
    FString EndBone = Ctx.GetString(TEXT("endBone"));
    FString GoalName = Ctx.GetString(TEXT("goalName"));

    if (ChainName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_CHAIN_NAME"), TEXT("chainName is required"));
        return true;
    }
    if (StartBone.IsEmpty() || EndBone.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_BONES"), TEXT("startBone and endBone are required to define the chain span"));
        return true;
    }

    UIKRigDefinition* IKRig = Cast<UIKRigDefinition>(
        StaticLoadObject(UIKRigDefinition::StaticClass(), nullptr, *AssetPath));
    if (!IKRig)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("IK Rig not found at '%s'"), *AssetPath));
        return true;
    }

    UIKRigController* Controller = UIKRigController::GetController(IKRig);
    if (!Controller)
    {
        Ctx.SendError(TEXT("CONTROLLER_UNAVAILABLE"), TEXT("Failed to get IK Rig controller for the asset"));
        return true;
    }

    // AddRetargetChain returns the (possibly de-duplicated) name actually assigned, or NAME_None on failure.
    const FName Added = Controller->AddRetargetChain(
        FName(*ChainName), FName(*StartBone), FName(*EndBone),
        GoalName.IsEmpty() ? NAME_None : FName(*GoalName));

    if (Added.IsNone())
    {
        Ctx.SendError(TEXT("CHAIN_NOT_ADDED"),
            FString::Printf(TEXT("AddRetargetChain rejected chain '%s' (startBone '%s' / endBone '%s' may not exist on the rig skeleton, or the chain is invalid)"),
                *ChainName, *StartBone, *EndBone));
        return true;
    }

    IKRig->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), IKRig->GetPathName());
    Result->SetStringField(TEXT("chainName"), Added.ToString());
    Result->SetStringField(TEXT("startBone"), StartBone);
    Result->SetStringField(TEXT("endBone"), EndBone);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"),
        FString::Printf(TEXT("Retarget chain '%s' added to IK Rig"), *Added.ToString()));
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("NOT_SUPPORTED"), TEXT("IK Rig module not available"));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.create_ik_retargeter", "animation.authoring",
    "Create an IK Retargeter asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the IK Retargeter"),
        RPC_PARAM_OPT("path", "path", "Save path (default /Game/Retargeting)"),
        RPC_PARAM_OPT("sourceIKRigPath", "path", "Path to source IK Rig"),
        RPC_PARAM_OPT("targetIKRigPath", "path", "Path to target IK Rig"),
        RPC_PARAM_OPT("save", "boolean", "Mark asset dirty (default true)")
    ))
{
#if MCP_HAS_IKRETARGET_FACTORY && MCP_HAS_IKRETARGETER && MCP_HAS_IKRETARGET_CONTROLLER
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Retargeting")));
    FString SourceIKRigPath = Ctx.GetString(TEXT("sourceIKRigPath"));
    FString TargetIKRigPath = Ctx.GetString(TEXT("targetIKRigPath"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_NAME"), TEXT("Name is required"));
        return true;
    }

    // Create the IK Retargeter using factory. The destination is composed through the checked
    // helper rather than by bare concatenation: CreatePackage logs at Fatal
    // (UObjectGlobals.cpp:1094-1096) for a name containing "//" - a verbosity compiled out in no
    // configuration - so an unvalidated `name` did not fail the call, it ended the editor PROCESS
    // and every unsaved package in it (measured on board
    // B-foliage-add-type-name-with-slash-kills-the-editor). Both IK Rig arguments are optional and
    // nothing is resolved before this point, so a bad `name` here reached the Fatal with no earlier
    // refusal to intercept it - which is why the regression test drives create_pose_library, whose
    // skeleton load sits above the composition, and not this verb.
    FString PackageName;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(Path, Name, PackageName, PathError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("%s Pass a bare asset name and choose the folder with path "
                                 "(default /Game/Retargeting)."), *PathError));
        return true;
    }

    UPackage* Package = CreatePackage(*PackageName);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package for IK Retargeter"));
        return true;
    }

    UIKRetargetFactory* Factory = NewObject<UIKRetargetFactory>();

    // Factory->SourceIKRig is private and is only consulted by the interactive create dialog,
    // not by FactoryCreateNew (it creates an empty retargeter). Drive the source/target rigs
    // through the public controller API instead (below), which also lets us set the target rig
    // the factory field cannot.
    UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Factory->FactoryCreateNew(
        UIKRetargeter::StaticClass(),
        Package,
        FName(*Name),
        RF_Public | RF_Standalone,
        nullptr,
        GWarn
    ));

    if (!Retargeter)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create IK Retargeter"));
        return true;
    }

    // Assign source/target IK Rigs via the controller (the correct headless API). SetIKRig is a
    // no-op for a null rig, so unspecified paths leave the slot unset.
    if (!SourceIKRigPath.IsEmpty() || !TargetIKRigPath.IsEmpty())
    {
        if (UIKRetargeterController* RetargetController = UIKRetargeterController::GetController(Retargeter))
        {
            if (!SourceIKRigPath.IsEmpty())
            {
                if (UIKRigDefinition* SourceRig = Cast<UIKRigDefinition>(StaticLoadObject(UIKRigDefinition::StaticClass(), nullptr, *SourceIKRigPath)))
                {
                    RetargetController->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
                }
            }
            if (!TargetIKRigPath.IsEmpty())
            {
                if (UIKRigDefinition* TargetRig = Cast<UIKRigDefinition>(StaticLoadObject(UIKRigDefinition::StaticClass(), nullptr, *TargetIKRigPath)))
                {
                    RetargetController->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);
                }
            }
        }
    }

    // Save if requested
    if (bSave)
    {
        FString AssetPathStr = Retargeter->GetPathName();
        int32 DotIndex = AssetPathStr.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
        if (DotIndex != INDEX_NONE) { AssetPathStr.LeftInline(DotIndex); }
        Retargeter->MarkPackageDirty();
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), Retargeter->GetPathName());
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("IK Retargeter '%s' created successfully"), *Name));
    Ctx.SendSuccess(Result);
#elif MCP_HAS_IKRETARGETER
    // Factory not available, fall back to informative message
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Retargeting")));
    FString FullPath = Path / Name;
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), FullPath);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("IK Retargeter '%s' creation requires IKRigEditor module"), *Name));
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("NOT_SUPPORTED"), TEXT("IK Retargeter module not available"));
#endif
    return true;
}

// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER("animation.authoring.set_retarget_chain_mapping", "animation.authoring",
    "Map a source retarget chain to a target chain in an IK Retargeter "
    "(UIKRetargeterController::SetSourceChain). In UE 5.6+ the mapping is op-stack-scoped: it "
    "only applies to ops whose chain mapping already exposes the target chain (derived from the "
    "target IK Rig's retarget chains). If no op exposes the target chain this reports "
    "CHAIN_MAP_NOT_APPLIED honestly rather than fake-succeed. The full op-stack-aware "
    "author-then-map flow (auto-adding the FK-chains op and populating chain maps) is follow-up work.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the IK Retargeter"),
        RPC_PARAM_REQ("sourceChain", "string", "Source chain name (animation is copied FROM this chain)."),
        RPC_PARAM_REQ("targetChain", "string", "Target chain name (animation is copied TO this chain)."),
        RPC_PARAM_OPT("opName", "string", "Optional retarget op name to scope the mapping to; applies to all ops with a chain mapping when omitted.")
    ))
{
#if MCP_HAS_IKRETARGET_CONTROLLER && MCP_HAS_IKRETARGETER
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString SourceChain = Ctx.GetString(TEXT("sourceChain"));
    FString TargetChain = Ctx.GetString(TEXT("targetChain"));
    FString OpName = Ctx.GetString(TEXT("opName"));

    if (SourceChain.IsEmpty() || TargetChain.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_CHAINS"), TEXT("sourceChain and targetChain are required"));
        return true;
    }

    UIKRetargeter* Retargeter = Cast<UIKRetargeter>(
        StaticLoadObject(UIKRetargeter::StaticClass(), nullptr, *AssetPath));
    if (!Retargeter)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("IK Retargeter not found at '%s'"), *AssetPath));
        return true;
    }

    UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
    if (!Controller)
    {
        Ctx.SendError(TEXT("CONTROLLER_UNAVAILABLE"), TEXT("Failed to get IK Retargeter controller for the asset"));
        return true;
    }

    // SetSourceChain only mutates ops whose chain mapping already contains the target chain.
    // It returns false when no op exposes the target chain — report that honestly instead of
    // claiming a mapping that did not take effect.
    // The op-stack (OpName) overload only exists on UE 5.6+; pre-5.6 retargeters have no
    // op stack, so the chain mapping is global and OpName is ignored.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    const bool bApplied = Controller->SetSourceChain(
        FName(*SourceChain), FName(*TargetChain),
        OpName.IsEmpty() ? NAME_None : FName(*OpName));
#else
    const bool bApplied = Controller->SetSourceChain(
        FName(*SourceChain), FName(*TargetChain));
    (void)OpName;  // pre-5.6 retargeters have no op stack; OpName is unused on this path
#endif

    if (!bApplied)
    {
        Ctx.SendError(TEXT("CHAIN_MAP_NOT_APPLIED"),
            FString::Printf(TEXT("No retarget op exposes target chain '%s' (the chain map is derived from the target IK Rig's retarget chains). Author the target chain / op stack first, then map."),
                *TargetChain));
        return true;
    }

    Retargeter->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), Retargeter->GetPathName());
    Result->SetStringField(TEXT("sourceChain"), SourceChain);
    Result->SetStringField(TEXT("targetChain"), TargetChain);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"),
        FString::Printf(TEXT("Chain mapping '%s' -> '%s' applied"), *SourceChain, *TargetChain));
    Ctx.SendSuccess(Result);
#else
    Ctx.SendError(TEXT("NOT_SUPPORTED"), TEXT("IK Retargeter module not available"));
#endif
    return true;
}
