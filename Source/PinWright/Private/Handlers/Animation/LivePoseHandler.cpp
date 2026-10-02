// Copyright (c) 2026 Alexander Penkin. MIT License.

// LivePoseHandler.cpp - animation.get_live_pose.
//
// skeleton.get_bone_transform reads the reference pose. This reads what a placed skeletal mesh
// component is actually showing: its current component-space read buffer (the array
// GetSocketTransform resolves bones through), plus optional AnimInstance state. It is a passive
// read - it never ticks, evaluates or refreshes the component - so it reports how fresh the
// buffer is instead of assuming it is.

#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Utils/ActorUtils.h"
#include "Utils/JsonBuilders.h"

#include "Animation/AnimClassInterface.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimStateMachineTypes.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "ReferenceSkeleton.h"

namespace
{
    // ponytail: fixed cap; add a caller-set limit if a rig over 1000 bones needs paging.
    constexpr int32 LivePoseMaxBones = 1000;

    TArray<TSharedPtr<FJsonValue>> LivePoseStrings(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FString& Value : Values)
        {
            Out.Add(MakeShared<FJsonValueString>(Value));
        }
        return Out;
    }
}

REGISTER_RPC_HANDLER("animation.get_live_pose", "animation",
    "Read the CURRENT evaluated pose of a placed skeletal mesh component (PIE or editor world): "
    "per-bone {name, index, parentName, transform} from the component's component-space read "
    "buffer - the same buffer GetSocketTransform resolves bones through - in the requested space. "
    "Optionally adds AnimInstance state: current state per state machine, the active montage and "
    "section, and current curve values. Passive: it never ticks or re-evaluates the component, so "
    "poseBuffer reports frameCounter, tickedThisFrame and revision (GetBoneTransformRevisionNumber; "
    "unchanged across two calls = the pose did not update). Read-only; leaves every package's "
    "dirty flag unchanged. For the reference (bind) pose use skeleton.get_bone_transform.",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"),
            TEXT("Actor holding the skeletal mesh component: object path, internal name or label. An ambiguous label is refused with AMBIGUOUS_ACTOR_NAME and its candidates.")),
        RPC_PARAM_REQ("space", "string",
            "Transform space: 'world', 'component' (relative to the component), or 'local' (relative to the parent bone). Required - no default is safe."),
        RPC_PARAM_DEF("world", "string",
            "World to search: 'pie', 'editor', or 'auto' (PIE when active, else editor). 'pie' with no PIE session is refused with PIE_NOT_ACTIVE.",
            "auto"),
        RPC_PARAM_OPT("component", "string",
            "SkeletalMeshComponent name on the actor. Required when the actor has more than one (TARGET_AMBIGUOUS lists them)."),
        RPC_PARAM_OPT("bones", "array",
            "Bone names to return. Omit for every bone (capped at 1000; truncated reports the cap). An unknown name is refused with BONE_NOT_FOUND."),
        RPC_PARAM_OPT("include", "array",
            "AnimInstance extras: 'stateMachines' (current state per machine), 'montage' (active montage, section, position), 'curves' (current curve values). Each is null when the component has no AnimInstance.")
    ))
{
    // ---- arguments (all validated before any lookup) ----
    const FString Space = Ctx.GetString(TEXT("space")).ToLower();
    if (Space != TEXT("world") && Space != TEXT("component") && Space != TEXT("local"))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown space '%s'. Valid: world, component, local."), *Space));
        return true;
    }

    const FString WorldMode = Ctx.GetString(TEXT("world"), TEXT("auto")).ToLower();
    if (WorldMode != TEXT("auto") && WorldMode != TEXT("pie") && WorldMode != TEXT("editor"))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown world '%s'. Valid: auto, pie, editor."), *WorldMode));
        return true;
    }

    bool bWantStateMachines = false;
    bool bWantMontage = false;
    bool bWantCurves = false;
    if (const TArray<TSharedPtr<FJsonValue>>* Include = Ctx.GetArray(TEXT("include")))
    {
        for (const TSharedPtr<FJsonValue>& Value : *Include)
        {
            FString Token;
            if (Value.IsValid() && Value->TryGetString(Token))
            {
                if (Token.Equals(TEXT("stateMachines"), ESearchCase::IgnoreCase)) { bWantStateMachines = true; continue; }
                if (Token.Equals(TEXT("montage"), ESearchCase::IgnoreCase))       { bWantMontage = true; continue; }
                if (Token.Equals(TEXT("curves"), ESearchCase::IgnoreCase))        { bWantCurves = true; continue; }
            }
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("Unknown include entry '%s'. Valid: stateMachines, montage, curves."), *Token));
            return true;
        }
    }

    // ---- world / actor / component ----
    FString ResolvedMode;
    UWorld* World = McpActorUtils::ResolveQueryWorld(WorldMode, ResolvedMode);
    if (!World)
    {
        Ctx.SendError(WorldMode == TEXT("pie") ? ErrorCodes::ERR_PIE_NOT_ACTIVE : ErrorCodes::ERR_EDITOR_WORLD_NOT_AVAILABLE,
            WorldMode == TEXT("pie")
                ? TEXT("world:'pie' was requested but no Play-In-Editor session is running. Start PIE or pass world:'editor'.")
                : TEXT("No world is available to search."));
        return true;
    }

    AActor* Actor = nullptr;
    if (!ActorNameParamUtils::RequireResolvedActor(Ctx, World, Actor))
    {
        return true;
    }

    TArray<USkeletalMeshComponent*> Components;
    Actor->GetComponents(Components);
    TArray<FString> ComponentNames;
    for (const USkeletalMeshComponent* Candidate : Components)
    {
        ComponentNames.Add(Candidate->GetName());
    }

    USkeletalMeshComponent* Component = nullptr;
    const FString ComponentName = Ctx.GetString(TEXT("component"));
    if (!ComponentName.IsEmpty())
    {
        for (USkeletalMeshComponent* Candidate : Components)
        {
            if (Candidate->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
            {
                Component = Candidate;
                break;
            }
        }
    }
    else if (Components.Num() == 1)
    {
        Component = Components[0];
    }
    else if (Components.Num() > 1)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetArrayField(TEXT("candidates"), LivePoseStrings(ComponentNames));
        Ctx.SendError(ErrorCodes::ERR_TARGET_AMBIGUOUS,
            FString::Printf(TEXT("Actor '%s' has %d SkeletalMeshComponents (%s); pass component to pick one."),
                *Actor->GetActorLabel(), Components.Num(), *FString::Join(ComponentNames, TEXT(", "))),
            Data);
        return true;
    }
    if (!Component)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetArrayField(TEXT("candidates"), LivePoseStrings(ComponentNames));
        Ctx.SendError(ErrorCodes::ERR_COMPONENT_NOT_FOUND,
            ComponentName.IsEmpty()
                ? FString::Printf(TEXT("Actor '%s' has no SkeletalMeshComponent."), *Actor->GetActorLabel())
                : FString::Printf(TEXT("Actor '%s' has no SkeletalMeshComponent named '%s'. Candidates: [%s]."),
                    *Actor->GetActorLabel(), *ComponentName, *FString::Join(ComponentNames, TEXT(", "))),
            Data);
        return true;
    }

    // ---- pose buffer ----
    const USkeletalMesh* Mesh = Component->GetSkeletalMeshAsset();
    const TArray<FTransform>& ComponentSpace = Component->GetComponentSpaceTransforms();
    if (!Mesh || ComponentSpace.Num() != Mesh->GetRefSkeleton().GetNum())
    {
        // A follower (leader-pose) component or one never initialised has no pose of its own.
        Ctx.SendError(ErrorCodes::ERR_INVALID_STATE,
            FString::Printf(TEXT("Component '%s' has no pose buffer to read (mesh: %s, %d component-space transforms). ")
                TEXT("A leader-pose follower reads its leader's pose: target the leader component instead."),
                *Component->GetName(), Mesh ? *Mesh->GetPathName() : TEXT("none"), ComponentSpace.Num()));
        return true;
    }
    const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();

    TArray<int32> BoneIndices;
    bool bTruncated = false;
    if (const TArray<TSharedPtr<FJsonValue>>* BoneValues = Ctx.GetArray(TEXT("bones")))
    {
        TArray<FString> Missing;
        for (const TSharedPtr<FJsonValue>& Value : *BoneValues)
        {
            FString BoneName;
            const int32 BoneIndex = (Value.IsValid() && Value->TryGetString(BoneName))
                ? RefSkeleton.FindBoneIndex(FName(*BoneName)) : INDEX_NONE;
            if (BoneIndex == INDEX_NONE)
            {
                Missing.Add(BoneName);
                continue;
            }
            BoneIndices.AddUnique(BoneIndex);
        }
        if (Missing.Num() > 0)
        {
            TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
            Data->SetArrayField(TEXT("missing"), LivePoseStrings(Missing));
            Ctx.SendError(ErrorCodes::ERR_BONE_NOT_FOUND,
                FString::Printf(TEXT("Bone(s) not on mesh '%s': [%s]."), *Mesh->GetPathName(), *FString::Join(Missing, TEXT(", "))),
                Data);
            return true;
        }
        if (BoneIndices.Num() > LivePoseMaxBones)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("bones names %d bones; the limit is %d."), BoneIndices.Num(), LivePoseMaxBones));
            return true;
        }
    }
    else
    {
        const int32 Count = FMath::Min(ComponentSpace.Num(), LivePoseMaxBones);
        bTruncated = Count < ComponentSpace.Num();
        for (int32 BoneIndex = 0; BoneIndex < Count; ++BoneIndex)
        {
            BoneIndices.Add(BoneIndex);
        }
    }

    // Same math GetSocketTransform uses for a bone name in each space.
    const FTransform ComponentToWorld = Component->GetComponentTransform();
    TArray<TSharedPtr<FJsonValue>> BoneRows;
    for (const int32 BoneIndex : BoneIndices)
    {
        const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
        FTransform Transform = ComponentSpace[BoneIndex];
        if (Space == TEXT("world"))
        {
            Transform = Transform * ComponentToWorld;
        }
        else if (Space == TEXT("local") && ParentIndex != INDEX_NONE)
        {
            Transform = Transform.GetRelativeTransform(ComponentSpace[ParentIndex]);
        }

        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), RefSkeleton.GetBoneName(BoneIndex).ToString());
        Row->SetNumberField(TEXT("index"), BoneIndex);
        if (ParentIndex != INDEX_NONE)
        {
            Row->SetStringField(TEXT("parentName"), RefSkeleton.GetBoneName(ParentIndex).ToString());
        }
        else
        {
            Row->SetField(TEXT("parentName"), MakeShared<FJsonValueNull>());
        }
        Row->SetObjectField(TEXT("transform"), JsonBuilders::BuildTransformJson(Transform));
        BoneRows.Add(MakeShared<FJsonValueObject>(Row));
    }

    TSharedPtr<FJsonObject> PoseBuffer = MakeShared<FJsonObject>();
    PoseBuffer->SetNumberField(TEXT("frameCounter"), static_cast<double>(GFrameCounter));
    PoseBuffer->SetBoolField(TEXT("tickedThisFrame"), Component->PoseTickedThisFrame());
    PoseBuffer->SetNumberField(TEXT("revision"), Component->GetBoneTransformRevisionNumber());
    PoseBuffer->SetBoolField(TEXT("componentTickEnabled"), Component->IsComponentTickEnabled());

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("world"), ResolvedMode);
    Result->SetStringField(TEXT("worldName"), World->GetName());
    Result->SetStringField(TEXT("actorLabel"), Actor->GetActorLabel());
    Result->SetStringField(TEXT("actorObjectName"), Actor->GetName());
    Result->SetStringField(TEXT("actorPath"), Actor->GetPathName());
    Result->SetStringField(TEXT("component"), Component->GetName());
    Result->SetStringField(TEXT("skeletalMeshPath"), Mesh->GetPathName());
    Result->SetStringField(TEXT("space"), Space);
    Result->SetObjectField(TEXT("poseBuffer"), PoseBuffer);
    Result->SetNumberField(TEXT("totalBones"), ComponentSpace.Num());
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    Result->SetArrayField(TEXT("bones"), BoneRows);

    // ---- AnimInstance extras ----
    UAnimInstance* AnimInstance = Component->GetAnimInstance();
    if (AnimInstance)
    {
        Result->SetStringField(TEXT("animInstanceClass"), AnimInstance->GetClass()->GetPathName());
    }
    else
    {
        Result->SetField(TEXT("animInstanceClass"), MakeShared<FJsonValueNull>());
    }

    if (bWantStateMachines)
    {
        const IAnimClassInterface* AnimClass = AnimInstance ? IAnimClassInterface::GetFromClass(AnimInstance->GetClass()) : nullptr;
        if (AnimClass)
        {
            TArray<TSharedPtr<FJsonValue>> Machines;
            const TArray<FBakedAnimationStateMachine>& Baked = AnimClass->GetBakedStateMachines();
            for (int32 MachineIndex = 0; MachineIndex < Baked.Num(); ++MachineIndex)
            {
                TSharedPtr<FJsonObject> Machine = MakeShared<FJsonObject>();
                Machine->SetStringField(TEXT("machine"), Baked[MachineIndex].MachineName.ToString());
                Machine->SetStringField(TEXT("currentState"), AnimInstance->GetCurrentStateName(MachineIndex).ToString());
                Machines.Add(MakeShared<FJsonValueObject>(Machine));
            }
            Result->SetArrayField(TEXT("stateMachines"), Machines);
        }
        else
        {
            Result->SetField(TEXT("stateMachines"), MakeShared<FJsonValueNull>());
        }
    }

    if (bWantMontage)
    {
        UAnimMontage* Montage = AnimInstance ? AnimInstance->GetCurrentActiveMontage() : nullptr;
        if (Montage)
        {
            TSharedPtr<FJsonObject> MontageJson = MakeShared<FJsonObject>();
            MontageJson->SetStringField(TEXT("asset"), Montage->GetPathName());
            MontageJson->SetStringField(TEXT("section"), AnimInstance->Montage_GetCurrentSection(Montage).ToString());
            MontageJson->SetNumberField(TEXT("position"), AnimInstance->Montage_GetPosition(Montage));
            MontageJson->SetBoolField(TEXT("playing"), AnimInstance->Montage_IsPlaying(Montage));
            Result->SetObjectField(TEXT("montage"), MontageJson);
        }
        else
        {
            Result->SetField(TEXT("montage"), MakeShared<FJsonValueNull>());
        }
    }

    if (bWantCurves)
    {
        if (AnimInstance)
        {
            // AttributeCurve holds every evaluated curve; MorphTargetCurve is the subset that
            // reached morph targets this evaluation.
            const TMap<FName, float>& MorphCurves = AnimInstance->GetAnimationCurveList(EAnimCurveType::MorphTargetCurve);
            TArray<TSharedPtr<FJsonValue>> Curves;
            for (const TPair<FName, float>& Pair : AnimInstance->GetAnimationCurveList(EAnimCurveType::AttributeCurve))
            {
                TSharedPtr<FJsonObject> Curve = MakeShared<FJsonObject>();
                Curve->SetStringField(TEXT("name"), Pair.Key.ToString());
                Curve->SetNumberField(TEXT("value"), Pair.Value);
                Curve->SetBoolField(TEXT("drivesMorph"), MorphCurves.Contains(Pair.Key));
                Curves.Add(MakeShared<FJsonValueObject>(Curve));
            }
            Result->SetArrayField(TEXT("curves"), Curves);
        }
        else
        {
            Result->SetField(TEXT("curves"), MakeShared<FJsonValueNull>());
        }
    }

    Ctx.SendSuccess(Result);
    return true;
}
