// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwSkelAssetCreate.cpp - see PwSkelAssetCreate.h for the callable creation seam.
#include "PwSkel/PwSkelAssetCreate.h"

#include "Compat/EngineVersionCompat.h"
#include "Handlers/ErrorCodes.h"
#include "PwSkel/PwSkelDiagnostic.h"
#include "PwSource/PwSourceAssetUserData.h"
#include "PwSource/PwSourceRecompileGuard.h"
#include "PwSource/PwValueRead.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/AssetUtils.h"

#include "Animation/Skeleton.h"
#include "Animation/AnimCurveMetadata.h"
#include "Animation/BoneReference.h"
#include "Animation/BlendProfile.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/DataAsset.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Interfaces/Interface_AssetUserData.h"
#include "Misc/PackageName.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
    struct FPwSkelAssetCreate_BoneRecord
    {
        FName Name;
        FString ExportName;
        int32 ParentIndex = INDEX_NONE;
        FTransform LocalTransform = FTransform::Identity;
        EBoneTranslationRetargetingMode::Type RetargetMode =
            EBoneTranslationRetargetingMode::Animation;
    };

    FSkeletonCreateResult PwSkelAssetCreate_Fail(
        const TCHAR* ErrorCode, FString ErrorMessage)
    {
        FSkeletonCreateResult Result;
        Result.ErrorCode = ErrorCode;
        Result.ErrorMessage = MoveTemp(ErrorMessage);
        return Result;
    }

    const UPwSourceAssetUserData* PwSkelAssetCreate_ReadProvenance(UObject* ExistingObject)
    {
        IInterface_AssetUserData* UserDataOwner = Cast<IInterface_AssetUserData>(ExistingObject);
        if (!UserDataOwner)
        {
            return nullptr;
        }

        return Cast<UPwSourceAssetUserData>(
            UserDataOwner->GetAssetUserDataOfClass(UPwSourceAssetUserData::StaticClass()));
    }

    void PwSkelAssetCreate_WriteProvenance(
        USkeleton* Skeleton, const FString& SourcePath, const FString& SourceHash,
        TArrayView<const FPwSourceStateEntry> GeneratedState)
    {
        if (!Skeleton || SourcePath.IsEmpty())
        {
            return;
        }

        IInterface_AssetUserData* UserDataOwner = Cast<IInterface_AssetUserData>(Skeleton);
        if (!UserDataOwner)
        {
            return;
        }

        UPwSourceAssetUserData* Stamp = Cast<UPwSourceAssetUserData>(
            UserDataOwner->GetAssetUserDataOfClass(UPwSourceAssetUserData::StaticClass()));
        if (!Stamp)
        {
            Stamp = NewObject<UPwSourceAssetUserData>(Skeleton);
            UserDataOwner->AddAssetUserData(Stamp);
        }

        Stamp->SourcePath = SourcePath;
        Stamp->SourceHash = SourceHash;
        Stamp->GeneratedStateVersion = PwSourceRecompileGuard::StateVersion;
        Stamp->GeneratedState = PwSourceRecompileGuard::MakeStateMap(GeneratedState);
    }

    EBoneTranslationRetargetingMode::Type PwSkelAssetCreate_RetargetMode(
        const TMap<FString, FPwValue>& Params)
    {
        const FString Value = PwValueRead::GetIdentifier(Params, TEXT("retarget"), TEXT("animation"));
        if (Value == TEXT("skeleton"))
        {
            return EBoneTranslationRetargetingMode::Skeleton;
        }
        if (Value == TEXT("animation_scaled"))
        {
            return EBoneTranslationRetargetingMode::AnimationScaled;
        }
        if (Value == TEXT("animation_relative"))
        {
            return EBoneTranslationRetargetingMode::AnimationRelative;
        }
        if (Value == TEXT("orient_and_scale"))
        {
            return EBoneTranslationRetargetingMode::OrientAndScale;
        }
        return EBoneTranslationRetargetingMode::Animation;
    }

    FString PwSkelAssetCreate_RetargetName(EBoneTranslationRetargetingMode::Type Mode)
    {
        switch (Mode)
        {
        case EBoneTranslationRetargetingMode::Skeleton:          return TEXT("skeleton");
        case EBoneTranslationRetargetingMode::AnimationScaled:   return TEXT("animation_scaled");
        case EBoneTranslationRetargetingMode::AnimationRelative: return TEXT("animation_relative");
        case EBoneTranslationRetargetingMode::OrientAndScale:    return TEXT("orient_and_scale");
        default:                                                  return TEXT("animation");
        }
    }

    bool PwSkelAssetCreate_CollectBone(
        const FPwBone& SourceBone,
        int32 ParentIndex,
        TSet<FName>& SeenNames,
        TArray<FPwSkelAssetCreate_BoneRecord>& OutBones,
        FString& OutError)
    {
        if (SourceBone.Name.IsEmpty())
        {
            OutError = TEXT("A skeleton bone name must not be empty");
            return false;
        }

        const FName BoneName(*SourceBone.Name);
        if (BoneName.IsNone())
        {
            OutError = FString::Printf(
                TEXT("Bone name '%s' does not produce a valid Unreal bone name"),
                *SourceBone.Name);
            return false;
        }
        if (SeenNames.Contains(BoneName))
        {
            OutError = FString::Printf(
                TEXT("Bone '%s' is declared more than once"), *SourceBone.Name);
            return false;
        }

        SeenNames.Add(BoneName);

        FPwSkelAssetCreate_BoneRecord& Record = OutBones.AddDefaulted_GetRef();
        Record.Name = BoneName;
        Record.ExportName = SourceBone.Name;
        Record.ParentIndex = ParentIndex;
        Record.LocalTransform = PwValueRead::ReadTransformParams(SourceBone.Transform);
        Record.RetargetMode = PwSkelAssetCreate_RetargetMode(SourceBone.Transform);

        const int32 ThisBoneIndex = OutBones.Num() - 1;
        for (const FPwBone& Child : SourceBone.Children)
        {
            if (!PwSkelAssetCreate_CollectBone(
                    Child, ThisBoneIndex, SeenNames, OutBones, OutError))
            {
                return false;
            }
        }

        return true;
    }

    FString PwSkelAssetCreate_CurveValue(const FPwSkelCurveMetaData& Curve)
    {
        TArray<FString> Bones = Curve.LinkedBones;
        Bones.Sort();
        return FString::Printf(TEXT("material=%s,morph_target=%s,max_lod=%d,linked_bones=[%s]"),
            Curve.bMaterial ? TEXT("true") : TEXT("false"),
            Curve.bMorphTarget ? TEXT("true") : TEXT("false"), Curve.MaxLod,
            *FString::Join(Bones, TEXT(",")));
    }

    FString PwSkelAssetCreate_CurveValue(FName Name, const FCurveMetaData& Curve)
    {
        TArray<FString> Bones;
        Bones.Reserve(Curve.LinkedBones.Num());
        for (const FBoneReference& Bone : Curve.LinkedBones)
        {
            Bones.Add(Bone.BoneName.ToString());
        }
        Bones.Sort();
        const int32 MaxLod = Curve.MaxLOD == MAX_uint8 ? -1 : static_cast<int32>(Curve.MaxLOD);
        return FString::Printf(TEXT("material=%s,morph_target=%s,max_lod=%d,linked_bones=[%s]"),
            Curve.Type.bMaterial ? TEXT("true") : TEXT("false"),
            Curve.Type.bMorphtarget ? TEXT("true") : TEXT("false"), MaxLod,
            *FString::Join(Bones, TEXT(",")));
    }

    FPwSourceStateEntry PwSkelAssetCreate_State(
        FString Key, FString Field, FString Value, FString Origin)
    {
        FPwSourceStateEntry Entry;
        Entry.Key = MoveTemp(Key);
        Entry.Field = MoveTemp(Field);
        Entry.Value = MoveTemp(Value);
        Entry.Origin = MoveTemp(Origin);
        return Entry;
    }

    TArray<FPwSourceStateEntry> PwSkelAssetCreate_DesiredState(
        const FPwSkelDocument& Document, const USkeletalMesh* PreviewMesh,
        const TArray<FPwSkelAssetCreate_BoneRecord>& Bones)
    {
        TArray<FPwSourceStateEntry> State;
        State.Add(PwSkelAssetCreate_State(TEXT("preview_mesh"), TEXT("previewMesh"),
            PreviewMesh ? PreviewMesh->GetPathName() : FString(),
            TEXT("previewMesh can be written by skeleton.set_preview_mesh or the Skeleton editor")));
        for (const FPwSkelAssetCreate_BoneRecord& Bone : Bones)
        {
            State.Add(PwSkelAssetCreate_State(
                TEXT("retarget:") + Bone.Name.ToString(),
                FString::Printf(TEXT("translationRetargetingMode[%s]"), *Bone.Name.ToString()),
                PwSkelAssetCreate_RetargetName(Bone.RetargetMode),
                TEXT("per-bone retargeting modes can be written by rig-authoring tools or the Skeleton editor")));
        }
        for (const FPwSkelCurveMetaData& Curve : Document.Curves)
        {
            State.Add(PwSkelAssetCreate_State(TEXT("curve:") + Curve.Name,
                FString::Printf(TEXT("curveMetadata[%s]"), *Curve.Name),
                PwSkelAssetCreate_CurveValue(Curve),
                TEXT("curve metadata is authored through the Skeleton editor or curve-authoring APIs")));
        }
        return State;
    }

    TArray<FPwSourceStateEntry> PwSkelAssetCreate_CurrentState(const USkeleton* Skeleton)
    {
        TArray<FPwSourceStateEntry> State;
        if (!Skeleton)
        {
            return State;
        }

        const USkeletalMesh* Preview = static_cast<const USkeleton*>(Skeleton)->GetPreviewMesh();
        State.Add(PwSkelAssetCreate_State(TEXT("preview_mesh"), TEXT("previewMesh"),
            Preview ? Preview->GetPathName() : FString(),
            TEXT("previewMesh can be written by skeleton.set_preview_mesh or the Skeleton editor")));

        const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
        for (int32 Index = 0; Index < Ref.GetRawBoneNum(); ++Index)
        {
            const FName Name = Ref.GetBoneName(Index);
            State.Add(PwSkelAssetCreate_State(TEXT("retarget:") + Name.ToString(),
                FString::Printf(TEXT("translationRetargetingMode[%s]"), *Name.ToString()),
                PwSkelAssetCreate_RetargetName(
                    Skeleton->GetBoneTranslationRetargetingMode(Index)),
                TEXT("per-bone retargeting modes can be written by rig-authoring tools or the Skeleton editor")));
        }

        Skeleton->ForEachCurveMetaData([&State](FName Name, const FCurveMetaData& Curve)
        {
            State.Add(PwSkelAssetCreate_State(TEXT("curve:") + Name.ToString(),
                FString::Printf(TEXT("curveMetadata[%s]"), *Name.ToString()),
                PwSkelAssetCreate_CurveValue(Name, Curve),
                TEXT("curve metadata is authored through the Skeleton editor or curve-authoring APIs")));
        });

        for (const USkeletalMeshSocket* Socket : Skeleton->Sockets)
        {
            if (Socket)
            {
                State.Add(PwSkelAssetCreate_State(TEXT("socket:") + Socket->SocketName.ToString(),
                    FString::Printf(TEXT("socket[%s]"), *Socket->SocketName.ToString()),
                    Socket->BoneName.ToString(),
                    TEXT("sockets are written by skeleton.create_socket, skeleton.configure_socket, or the Skeleton editor")));
            }
        }
        for (const FVirtualBone& Bone : Skeleton->GetVirtualBones())
        {
            State.Add(PwSkelAssetCreate_State(TEXT("virtual_bone:") + Bone.VirtualBoneName.ToString(),
                FString::Printf(TEXT("virtualBone[%s]"), *Bone.VirtualBoneName.ToString()),
                Bone.SourceBoneName.ToString() + TEXT("->") + Bone.TargetBoneName.ToString(),
                TEXT("virtual bones are written by skeleton.create_virtual_bone or the Skeleton editor")));
        }
        for (const TPair<FName, FReferencePose>& Pair : Skeleton->AnimRetargetSources)
        {
            State.Add(PwSkelAssetCreate_State(TEXT("retarget_source:") + Pair.Key.ToString(),
                FString::Printf(TEXT("retargetSource[%s]"), *Pair.Key.ToString()), TEXT("present"),
                TEXT("retarget sources are authored on the Skeleton asset outside .pwskel")));
        }
        for (const UBlendProfile* Profile : Skeleton->BlendProfiles)
        {
            if (Profile)
            {
                State.Add(PwSkelAssetCreate_State(TEXT("blend_profile:") + Profile->GetName(),
                    FString::Printf(TEXT("blendProfile[%s]"), *Profile->GetName()), TEXT("present"),
                    TEXT("blend profiles are authored on the Skeleton asset outside .pwskel")));
            }
        }
        for (const FAnimSlotGroup& Group : Skeleton->GetSlotGroups())
        {
            State.Add(PwSkelAssetCreate_State(TEXT("slot_group:") + Group.GroupName.ToString(),
                FString::Printf(TEXT("slotGroup[%s]"), *Group.GroupName.ToString()),
                FString::Printf(TEXT("%d slot(s)"), Group.SlotNames.Num()),
                TEXT("slot groups are authored on the Skeleton asset outside .pwskel")));
        }
        for (const TSoftObjectPtr<USkeleton>& Compatible : Skeleton->GetCompatibleSkeletons())
        {
            State.Add(PwSkelAssetCreate_State(TEXT("compatible_skeleton:") + Compatible.ToSoftObjectPath().ToString(),
                TEXT("compatibleSkeleton"), Compatible.ToSoftObjectPath().ToString(),
                TEXT("compatible skeletons are authored on the Skeleton asset outside .pwskel")));
        }
        // USkeleton::bUseRetargetModesFromCompatibleSkeleton and its accessors are UE 5.6+.
        // Older skeletons carry no such setting, so there is no state here to report.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        if (Skeleton->GetUseRetargetModesFromCompatibleSkeleton())
        {
            State.Add(PwSkelAssetCreate_State(TEXT("use_compatible_retarget"),
                TEXT("useRetargetModesFromCompatibleSkeleton"), TEXT("true"),
                TEXT("this compatibility setting is authored on the Skeleton asset outside .pwskel")));
        }
#endif
#if WITH_EDITORONLY_DATA
        if (const UDataAsset* Additional = Skeleton->GetAdditionalPreviewSkeletalMeshes())
        {
            State.Add(PwSkelAssetCreate_State(TEXT("additional_preview_meshes"),
                TEXT("additionalPreviewMeshes"), Additional->GetPathName(),
                TEXT("additional preview meshes are authored in the Skeleton editor")));
        }
        for (const FName Name : Skeleton->AnimationNotifies)
        {
            State.Add(PwSkelAssetCreate_State(TEXT("notify_name:") + Name.ToString(),
                FString::Printf(TEXT("notifyName[%s]"), *Name.ToString()), TEXT("present"),
                TEXT("notify names are accumulated by animation authoring outside .pwskel")));
        }
        if (Skeleton->PreviewAttachedAssetContainer.Num() > 0)
        {
            State.Add(PwSkelAssetCreate_State(TEXT("preview_attachments"),
                TEXT("previewAttachments"),
                FString::Printf(TEXT("%d attachment(s)"), Skeleton->PreviewAttachedAssetContainer.Num()),
                TEXT("preview attachments are authored in the Skeleton editor")));
        }
#endif
        return State;
    }

    void PwSkelAssetCreate_ResetSourceOwnedAndUnsupportedState(USkeleton* Skeleton)
    {
        Skeleton->SetPreviewMesh(nullptr, false);
        Skeleton->Sockets.Reset();

        TArray<FName> VirtualBoneNames;
        for (const FVirtualBone& Bone : Skeleton->GetVirtualBones())
        {
            VirtualBoneNames.Add(Bone.VirtualBoneName);
        }
        Skeleton->RemoveVirtualBones(VirtualBoneNames);
        Skeleton->AnimRetargetSources.Reset();
        Skeleton->BlendProfiles.Reset();

        TArray<FName> SlotGroups;
        for (const FAnimSlotGroup& Group : Skeleton->GetSlotGroups())
        {
            SlotGroups.Add(Group.GroupName);
        }
        for (const FName GroupName : SlotGroups)
        {
            Skeleton->RemoveSlotGroup(GroupName);
        }

        const TArray<TSoftObjectPtr<USkeleton>> CompatibleSkeletons =
            Skeleton->GetCompatibleSkeletons();
        for (const TSoftObjectPtr<USkeleton>& Compatible : CompatibleSkeletons)
        {
            Skeleton->RemoveCompatibleSkeleton(Compatible);
        }
        // 5.6+ only - see the reporting site above; nothing to reset on older engines.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        Skeleton->SetUseRetargetModesFromCompatibleSkeleton(false);
#endif

#if WITH_EDITORONLY_DATA
        Skeleton->SetAdditionalPreviewSkeletalMeshes(nullptr);
        Skeleton->AnimationNotifies.Reset();
        Skeleton->PreviewAttachedAssetContainer.ClearAllAttachedObjects();
#endif

        TArray<FName> CurveNames;
        Skeleton->GetCurveMetaDataNames(CurveNames);
        Skeleton->RemoveCurveMetaData(CurveNames);
    }

    bool PwSkelAssetCreate_ApplyCurves(
        USkeleton* Skeleton, const FPwSkelDocument& Document, FString& OutError)
    {
        for (const FPwSkelCurveMetaData& Curve : Document.Curves)
        {
            const FName CurveName(*Curve.Name);
            if (!MCP_ADD_CURVE_META_DATA(Skeleton, CurveName))
            {
                OutError = FString::Printf(TEXT("Could not add curve metadata '%s'"), *Curve.Name);
                return false;
            }
            UAnimCurveMetaData* MetaData = Skeleton->GetAssetUserData<UAnimCurveMetaData>();
            if (!MetaData)
            {
                OutError = FString::Printf(TEXT("Curve metadata '%s' was not created"), *Curve.Name);
                return false;
            }
            // USkeleton only gained the forwarding setters in UE 5.8; the metadata object exposes
            // them on every supported version.
            MetaData->SetCurveMetaDataMaterial(CurveName, Curve.bMaterial);
            MetaData->SetCurveMetaDataMorphTarget(CurveName, Curve.bMorphTarget);

            TArray<FBoneReference> BoneLinks;
            for (const FString& BoneName : Curve.LinkedBones)
            {
                FBoneReference& Bone = BoneLinks.AddDefaulted_GetRef();
                Bone.BoneName = FName(*BoneName);
                Bone.Initialize(Skeleton);
            }
            MetaData->SetCurveMetaDataBoneLinks(CurveName, BoneLinks,
                Curve.MaxLod < 0 ? MAX_uint8 : static_cast<uint8>(Curve.MaxLod), Skeleton);
        }
        return true;
    }

    USkeletalMesh* PwSkelAssetCreate_LoadPreviewMesh(
        const FPwSkelDocument& Document, FString& OutError)
    {
        if (!Document.PreviewMesh.IsSet())
        {
            return nullptr;
        }

        USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(
            nullptr, *Document.PreviewMesh->AssetPath);
        if (!Mesh)
        {
            OutError = FString::Printf(TEXT("preview_mesh path '%s' did not resolve to a USkeletalMesh"),
                *Document.PreviewMesh->AssetPath);
        }
        return Mesh;
    }

    bool PwSkelAssetCreate_FlattenDocument(
        const FPwSkelDocument& Document,
        TArray<FPwSkelAssetCreate_BoneRecord>& OutBones,
        FString& OutError)
    {
        OutBones.Reset();

        if (Document.Roots.Num() == 0)
        {
            OutError = TEXT("A skeleton document must contain one root bone");
            return false;
        }
        if (Document.Roots.Num() != 1)
        {
            OutError = FString::Printf(
                TEXT("A skeleton document must contain exactly one root bone; found %d"),
                Document.Roots.Num());
            return false;
        }

        TSet<FName> SeenNames;
        return PwSkelAssetCreate_CollectBone(
            Document.Roots[0], INDEX_NONE, SeenNames, OutBones, OutError);
    }

    bool PwSkelAssetCreate_RebuildHierarchy(
        USkeleton* Skeleton,
        const TArray<FPwSkelAssetCreate_BoneRecord>& Bones,
        FString& OutError)
    {
        if (!Skeleton || Bones.Num() == 0)
        {
            OutError = TEXT("Cannot build an empty skeleton hierarchy");
            return false;
        }

        TArray<FName> ExistingBoneNames;
        const FReferenceSkeleton& ExistingReference = Skeleton->GetReferenceSkeleton();
        ExistingBoneNames.Reserve(ExistingReference.GetRawBoneNum());
        for (const FMeshBoneInfo& BoneInfo : ExistingReference.GetRawRefBoneInfo())
        {
            ExistingBoneNames.Add(BoneInfo.Name);
        }

        {
            FReferenceSkeletonModifier Modifier(Skeleton);

            if (ExistingBoneNames.Num() == 0)
            {
                for (int32 BoneIndex = 0; BoneIndex < Bones.Num(); ++BoneIndex)
                {
                    FMeshBoneInfo BoneInfo;
                    BoneInfo.Name = Bones[BoneIndex].Name;
                    BoneInfo.ParentIndex = Bones[BoneIndex].ParentIndex;
                    BoneInfo.ExportName = Bones[BoneIndex].ExportName;
                    Modifier.Add(BoneInfo, Bones[BoneIndex].LocalTransform, BoneIndex == 0);
                }
            }
            else
            {
                // Keep the object and its package in place so references survive a source
                // recompile.  Remove descendants deepest-first; the root cannot be removed
                // from a one-root reference skeleton, so rename/update it instead.
                for (int32 BoneIndex = ExistingBoneNames.Num() - 1; BoneIndex >= 1; --BoneIndex)
                {
                    Modifier.Remove(ExistingBoneNames[BoneIndex], true);
                }

                if (ExistingBoneNames[0] != Bones[0].Name)
                {
                    Modifier.Rename(ExistingBoneNames[0], Bones[0].Name);
                }
                Modifier.UpdateRefPoseTransform(0, Bones[0].LocalTransform);

                for (int32 BoneIndex = 1; BoneIndex < Bones.Num(); ++BoneIndex)
                {
                    FMeshBoneInfo BoneInfo;
                    BoneInfo.Name = Bones[BoneIndex].Name;
                    BoneInfo.ParentIndex = Bones[BoneIndex].ParentIndex;
                    BoneInfo.ExportName = Bones[BoneIndex].ExportName;
                    Modifier.Add(BoneInfo, Bones[BoneIndex].LocalTransform, false);
                }
            }
        }

        const FReferenceSkeleton& ResultReference = Skeleton->GetReferenceSkeleton();
        if (ResultReference.GetRawBoneNum() != Bones.Num())
        {
            OutError = FString::Printf(
                TEXT("Created skeleton read back %d bones; expected %d"),
                ResultReference.GetRawBoneNum(), Bones.Num());
            return false;
        }

        const TArray<FMeshBoneInfo>& ResultBoneInfo = ResultReference.GetRawRefBoneInfo();
        for (int32 BoneIndex = 0; BoneIndex < Bones.Num(); ++BoneIndex)
        {
            if (!ResultBoneInfo.IsValidIndex(BoneIndex) ||
                ResultBoneInfo[BoneIndex].Name != Bones[BoneIndex].Name ||
                ResultBoneInfo[BoneIndex].ParentIndex != Bones[BoneIndex].ParentIndex)
            {
                OutError = FString::Printf(
                    TEXT("Created skeleton hierarchy does not match source bone %d ('%s')"),
                    BoneIndex, *Bones[BoneIndex].Name.ToString());
                return false;
            }
        }

        return true;
    }

    bool PwSkelAssetCreate_ResetBoneTree(
        USkeleton* Skeleton, int32 BoneCount, FString& OutError)
    {
        FArrayProperty* BoneTreeProperty = FindFProperty<FArrayProperty>(
            USkeleton::StaticClass(), TEXT("BoneTree"));
        const FStructProperty* BoneNodeProperty = BoneTreeProperty
            ? CastField<FStructProperty>(BoneTreeProperty->Inner)
            : nullptr;
        if (!BoneTreeProperty || !BoneNodeProperty ||
            BoneNodeProperty->Struct != FBoneNode::StaticStruct())
        {
            OutError = TEXT("Could not initialize the Skeleton bone retargeting table");
            return false;
        }

        FScriptArrayHelper BoneTree(
            BoneTreeProperty, BoneTreeProperty->ContainerPtrToValuePtr<void>(Skeleton));
        BoneTree.EmptyAndAddValues(BoneCount);
        if (BoneTree.Num() != BoneCount)
        {
            OutError = FString::Printf(
                TEXT("Initialized %d Skeleton retargeting entries; expected %d"),
                BoneTree.Num(), BoneCount);
            return false;
        }
        return true;
    }

    int32 PwSkelAssetCreate_BoneTreeCount(const USkeleton* Skeleton)
    {
        FArrayProperty* BoneTreeProperty = FindFProperty<FArrayProperty>(
            USkeleton::StaticClass(), TEXT("BoneTree"));
        if (!BoneTreeProperty || !Skeleton)
        {
            return INDEX_NONE;
        }

        FScriptArrayHelper BoneTree(
            BoneTreeProperty,
            BoneTreeProperty->ContainerPtrToValuePtr<void>(const_cast<USkeleton*>(Skeleton)));
        return BoneTree.Num();
    }

    bool PwSkelAssetCreate_AnnounceHierarchyChange(
        USkeleton* Skeleton, int32 ExpectedBoneCount, FString& OutError)
    {
        // The engine's full hierarchy-change method is protected. Its public bone-removal path
        // is the supported route that regenerates the skeleton GUID, clears linkup caches,
        // refreshes remapping, and validates loaded animations. Add and remove one internal bone
        // so the finished source hierarchy is unchanged while that public path performs the
        // required announcement.
        FName Sentinel(TEXT("__PinWrightHierarchyRefresh"));
        for (int32 Suffix = 1;
             Skeleton->GetReferenceSkeleton().FindRawBoneIndex(Sentinel) != INDEX_NONE;
             ++Suffix)
        {
            Sentinel = FName(*FString::Printf(
                TEXT("__PinWrightHierarchyRefresh_%d"), Suffix));
        }

        {
            FReferenceSkeletonModifier Modifier(Skeleton);
            FMeshBoneInfo BoneInfo;
            BoneInfo.Name = Sentinel;
            BoneInfo.ExportName = Sentinel.ToString();
            BoneInfo.ParentIndex = 0;
            Modifier.Add(BoneInfo, FTransform::Identity, false);
        }

        FArrayProperty* BoneTreeProperty = FindFProperty<FArrayProperty>(
            USkeleton::StaticClass(), TEXT("BoneTree"));
        if (!BoneTreeProperty)
        {
            OutError = TEXT("Could not announce the compiled Skeleton hierarchy change");
            return false;
        }
        FScriptArrayHelper BoneTree(
            BoneTreeProperty, BoneTreeProperty->ContainerPtrToValuePtr<void>(Skeleton));
        BoneTree.AddValue();

        Skeleton->RemoveBonesFromSkeleton({ Sentinel }, /*bRemoveChildBones=*/true);
        if (Skeleton->GetReferenceSkeleton().FindRawBoneIndex(Sentinel) != INDEX_NONE
            || Skeleton->GetReferenceSkeleton().GetRawBoneNum() != ExpectedBoneCount
            || PwSkelAssetCreate_BoneTreeCount(Skeleton) != ExpectedBoneCount)
        {
            OutError = TEXT("Could not restore the compiled Skeleton after refreshing dependent animations");
            return false;
        }
        return true;
    }

    bool PwSkelAssetCreate_VerifyPostCondition(
        const USkeleton* Skeleton,
        const TArray<FPwSkelAssetCreate_BoneRecord>& Bones,
        TArrayView<const FPwSourceStateEntry> DesiredState,
        FString& OutError)
    {
        if (!Skeleton)
        {
            OutError = TEXT("Compiled Skeleton post-condition failed: no asset was created");
            return false;
        }

        const FReferenceSkeleton& Reference = Skeleton->GetReferenceSkeleton();
        if (Reference.GetRawBoneNum() != Bones.Num())
        {
            OutError = FString::Printf(
                TEXT("Compiled Skeleton post-condition failed: the asset has %d reference bones but the source describes %d"),
                Reference.GetRawBoneNum(), Bones.Num());
            return false;
        }

        const int32 BoneTreeCount = PwSkelAssetCreate_BoneTreeCount(Skeleton);
        if (BoneTreeCount != Bones.Num())
        {
            OutError = FString::Printf(
                TEXT("Compiled Skeleton post-condition failed: BoneTree has %d entries but the source describes %d bones"),
                BoneTreeCount, Bones.Num());
            return false;
        }

        const TArray<FMeshBoneInfo>& BoneInfo = Reference.GetRawRefBoneInfo();
        const TArray<FTransform>& BonePose = Reference.GetRawRefBonePose();
        for (int32 BoneIndex = 0; BoneIndex < Bones.Num(); ++BoneIndex)
        {
            const FPwSkelAssetCreate_BoneRecord& Expected = Bones[BoneIndex];
            if (!BoneInfo.IsValidIndex(BoneIndex)
                || BoneInfo[BoneIndex].Name != Expected.Name
                || BoneInfo[BoneIndex].ParentIndex != Expected.ParentIndex
                || !BonePose.IsValidIndex(BoneIndex)
                || !BonePose[BoneIndex].Equals(Expected.LocalTransform, UE_KINDA_SMALL_NUMBER)
                || Skeleton->GetBoneTranslationRetargetingMode(BoneIndex)
                    != Expected.RetargetMode)
            {
                OutError = FString::Printf(
                    TEXT("Compiled Skeleton post-condition failed at source bone %d ('%s')"),
                    BoneIndex, *Expected.Name.ToString());
                return false;
            }
        }

        const TMap<FString, FString> Desired =
            PwSourceRecompileGuard::MakeStateMap(DesiredState);
        const TMap<FString, FString> Current = PwSourceRecompileGuard::MakeStateMap(
            PwSkelAssetCreate_CurrentState(Skeleton));
        for (const TPair<FString, FString>& Expected : Desired)
        {
            const FString* Actual = Current.Find(Expected.Key);
            if (!Actual || *Actual != Expected.Value)
            {
                OutError = FString::Printf(
                    TEXT("Compiled Skeleton post-condition failed for generated field '%s'"),
                    *Expected.Key);
                return false;
            }
        }
        return true;
    }

}

FSkeletonCreateResult CreateSkeleton(
    const FPwSkelDocument& Document, const FSkeletonCreateSpec& Spec)
{
    TArray<FPwSkelAssetCreate_BoneRecord> Bones;
    FString FlattenError;
    if (!PwSkelAssetCreate_FlattenDocument(Document, Bones, FlattenError))
    {
        return PwSkelAssetCreate_Fail(ErrorCodes::ERR_INVALID_ARGUMENT, MoveTemp(FlattenError));
    }

    if (Spec.AssetPath.IsEmpty())
    {
        return PwSkelAssetCreate_Fail(
            ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("assetPath is required"));
    }

    FText BadPackageNameReason;
    if (!FPackageName::IsValidTextForLongPackageName(Spec.AssetPath, &BadPackageNameReason))
    {
        return PwSkelAssetCreate_Fail(
            ErrorCodes::ERR_INVALID_ASSET_PATH,
            FString::Printf(TEXT("Invalid skeleton asset path '%s': %s"),
                *Spec.AssetPath, *BadPackageNameReason.ToString()));
    }
    if (Spec.AssetPath.StartsWith(TEXT("/Engine/")) &&
        !Spec.AssetPath.StartsWith(TEXT("/Engine/Transient")))
    {
        return PwSkelAssetCreate_Fail(
            ErrorCodes::ERR_SECURITY_VIOLATION,
            TEXT("Refusing to write a generated Skeleton into /Engine/ - target a /Game/ path"));
    }

    const FString PackageName = Spec.AssetPath;
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
    if (AssetName.IsEmpty())
    {
        return PwSkelAssetCreate_Fail(
            ErrorCodes::ERR_INVALID_ASSET_PATH,
            FString::Printf(TEXT("Skeleton asset path '%s' has no asset name"), *Spec.AssetPath));
    }

    FString PreviewMeshError;
    USkeletalMesh* PreviewMesh = PwSkelAssetCreate_LoadPreviewMesh(Document, PreviewMeshError);
    if (!PreviewMeshError.IsEmpty())
    {
        return PwSkelAssetCreate_Fail(
            ErrorCodes::ERR_ASSET_NOT_FOUND, MoveTemp(PreviewMeshError));
    }

    const TArray<FPwSourceStateEntry> DesiredState =
        PwSkelAssetCreate_DesiredState(Document, PreviewMesh, Bones);

    FSkeletonCreateResult Out;
    const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
        PackageName, AssetName, USkeleton::StaticClass(), /*bOverwriteRequested=*/false);
    if (Resolution.IsRejected())
    {
        Out.ErrorCode = Resolution.ErrorCode;
        Out.ErrorMessage = Resolution.ErrorMessage;
        return Out;
    }

    const bool bUpdateInPlace = Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace;
    const UPwSourceAssetUserData* ExistingStamp = bUpdateInPlace
        ? PwSkelAssetCreate_ReadProvenance(Resolution.Existing) : nullptr;
    const bool bSameSource = ExistingStamp && !Spec.SourcePath.IsEmpty() &&
        ExistingStamp->SourcePath == Spec.SourcePath;

    USkeleton* Skeleton = bUpdateInPlace ? Cast<USkeleton>(Resolution.Existing) : nullptr;
    if (bUpdateInPlace && !Skeleton)
    {
        return PwSkelAssetCreate_Fail(
            ErrorCodes::ERR_ASSET_CREATION_FAILED,
            FString::Printf(TEXT("The existing asset at %s could not be loaded as a Skeleton"),
                *PackageName));
    }

    bool bGuardAllowsWrite = true;
    if (bUpdateInPlace)
    {
        FPwSourceRecompileGuardRequest Guard;
        Guard.FormatName = TEXT(".pwskel");
        Guard.AssetPath = PackageName;
        Guard.bSameSourceRecompile = bSameSource;
        Guard.bTakeover = !bSameSource;
        Guard.bOverwrite = Spec.bOverwrite;
        Guard.CurrentSourcePath = ExistingStamp ? ExistingStamp->SourcePath : FString();
        Guard.RequestedSourcePath = Spec.SourcePath;
        Guard.Line = Document.Header.VersionLine;
        Guard.Column = Document.Header.VersionColumn;
        Guard.BaselineVersion = ExistingStamp ? ExistingStamp->GeneratedStateVersion : 0;
        Guard.Baseline = ExistingStamp ? &ExistingStamp->GeneratedState : nullptr;
        Guard.Current = PwSkelAssetCreate_CurrentState(Skeleton);
        Guard.Desired = DesiredState;
        bGuardAllowsWrite = PwSourceRecompileGuard::Check(Guard, Out.Diagnostics);
    }

    if (bUpdateInPlace)
    {
        if (!bSameSource && !Spec.bOverwrite)
        {
            FString Why;
            if (!ExistingStamp)
            {
                Why = FString::Printf(
                    TEXT("A Skeleton already exists at %s and carries no PinWright source provenance stamp"),
                    *PackageName);
            }
            else
            {
                Why = FString::Printf(
                    TEXT("The skeleton at %s was generated from '%s', not '%s'"),
                    *PackageName, *ExistingStamp->SourcePath, *Spec.SourcePath);
            }
            Out.ErrorCode = ErrorCodes::ERR_ASSET_ALREADY_EXISTS;
            Out.ErrorMessage = Why;
            if (!bGuardAllowsWrite && Out.Diagnostics.Num() > 0)
            {
                Out.ErrorMessage += TEXT(". ") + Out.Diagnostics.Last().Message;
            }
            else
            {
                Out.ErrorMessage +=
                    TEXT(" - pass overwrite=true to rebuild it in place (its referencers are preserved)");
            }
            return Out;
        }
    }

    if (!bGuardAllowsWrite)
    {
        Out.ErrorCode = ErrorCodes::ERR_ASSET_DATA_INVALID;
        Out.ErrorMessage = Out.Diagnostics.Last().Message;
        return Out;
    }

    if (PreviewMesh && Skeleton)
    {
        USkeleton* MeshSkeleton = PreviewMesh->GetSkeleton();
        if (!MeshSkeleton || !Skeleton->IsCompatibleForEditor(MeshSkeleton))
        {
            return PwSkelAssetCreate_Fail(
                ErrorCodes::ERR_ASSET_DATA_INVALID,
                FString::Printf(
                    TEXT("preview_mesh '%s' is not bound to this Skeleton or to a compatible Skeleton"),
                    *PreviewMesh->GetPathName()));
        }
    }

    if (!Skeleton)
    {
        UPackage* Package = CreatePackage(*PackageName);
        if (!Package)
        {
            return PwSkelAssetCreate_Fail(
                ErrorCodes::ERR_ASSET_CREATION_FAILED,
                FString::Printf(TEXT("Could not create skeleton package '%s'"), *PackageName));
        }

        Skeleton = NewObject<USkeleton>(Package, *AssetName, RF_Public | RF_Standalone);
        if (!Skeleton)
        {
            return PwSkelAssetCreate_Fail(
                ErrorCodes::ERR_ASSET_CREATION_FAILED,
                FString::Printf(TEXT("Could not create Skeleton '%s'"), *Spec.AssetPath));
        }
    }

    PwSkelAssetCreate_ResetSourceOwnedAndUnsupportedState(Skeleton);

    FString HierarchyError;
    if (!PwSkelAssetCreate_RebuildHierarchy(Skeleton, Bones, HierarchyError))
    {
        return PwSkelAssetCreate_Fail(ErrorCodes::ERR_ASSET_DATA_INVALID, MoveTemp(HierarchyError));
    }

    if (!PwSkelAssetCreate_ResetBoneTree(Skeleton, Bones.Num(), HierarchyError))
    {
        return PwSkelAssetCreate_Fail(ErrorCodes::ERR_ASSET_DATA_INVALID, MoveTemp(HierarchyError));
    }

    for (int32 BoneIndex = 0; BoneIndex < Bones.Num(); ++BoneIndex)
    {
        Skeleton->SetBoneTranslationRetargetingMode(
            BoneIndex, Bones[BoneIndex].RetargetMode, false);
    }

    FString CurveError;
    if (!PwSkelAssetCreate_ApplyCurves(Skeleton, Document, CurveError))
    {
        return PwSkelAssetCreate_Fail(ErrorCodes::ERR_ASSET_DATA_INVALID, MoveTemp(CurveError));
    }

    if (PreviewMesh)
    {
        USkeleton* MeshSkeleton = PreviewMesh->GetSkeleton();
        if (!MeshSkeleton || !Skeleton->IsCompatibleForEditor(MeshSkeleton))
        {
            return PwSkelAssetCreate_Fail(
                ErrorCodes::ERR_ASSET_DATA_INVALID,
                FString::Printf(
                    TEXT("preview_mesh '%s' is not bound to this Skeleton or to a compatible Skeleton"),
                    *PreviewMesh->GetPathName()));
        }
        Skeleton->SetPreviewMesh(PreviewMesh, false);
    }

#if WITH_DEV_AUTOMATION_TESTS
    if (Spec.BeforePostConditionForTest)
    {
        Spec.BeforePostConditionForTest(Skeleton);
    }
#endif

    FString PostConditionError;
    if (!PwSkelAssetCreate_VerifyPostCondition(
            Skeleton, Bones, DesiredState, PostConditionError))
    {
        Out.ErrorCode = ErrorCodes::ERR_ASSET_DATA_INVALID;
        Out.ErrorMessage = PostConditionError;
        Out.Diagnostics.Add(FPwDiagnostic::MakeError(
            PwSkelDiagnosticCodes::PWSKEL_ASSET_POSTCONDITION_FAILED,
            Document.Header.VersionLine, Document.Header.VersionColumn,
            MoveTemp(PostConditionError)));
        return Out;
    }

    // FReferenceSkeletonModifier only announces a reference-array edit. A hierarchy change also
    // needs a new generation and refreshed loaded animations; otherwise consumers can keep using
    // bone mappings from before this successful compile until the editor restarts.
    if (!PwSkelAssetCreate_AnnounceHierarchyChange(
            Skeleton, Bones.Num(), PostConditionError)
        || !PwSkelAssetCreate_VerifyPostCondition(
            Skeleton, Bones, DesiredState, PostConditionError))
    {
        Out.ErrorCode = ErrorCodes::ERR_ASSET_DATA_INVALID;
        Out.ErrorMessage = PostConditionError;
        Out.Diagnostics.Add(FPwDiagnostic::MakeError(
            PwSkelDiagnosticCodes::PWSKEL_ASSET_POSTCONDITION_FAILED,
            Document.Header.VersionLine, Document.Header.VersionColumn,
            MoveTemp(PostConditionError)));
        return Out;
    }

    const TArray<FPwSourceStateEntry> GeneratedState =
        PwSkelAssetCreate_CurrentState(Skeleton);
    PwSkelAssetCreate_WriteProvenance(
        Skeleton, Spec.SourcePath, Spec.SourceHash, GeneratedState);

    if (!bUpdateInPlace)
    {
        FAssetRegistryModule::AssetCreated(Skeleton);
    }

    // The modifier does not itself promise a dirty package. Mark it before both the durable and
    // deferred paths so SaveAssetToDiskReportingPresence can distinguish a real write from a
    // throttle skip.
    Skeleton->MarkPackageDirty();

    Out.Asset = Skeleton;
    Out.AssetPath = Skeleton->GetPathName();
    Out.PackageName = Skeleton->GetOutermost() ? Skeleton->GetOutermost()->GetName() : FString();
    Out.bUpdatedInPlace = bUpdateInPlace;
    Out.BoneCount = Skeleton->GetReferenceSkeleton().GetRawBoneNum();

    if (Spec.bSave)
    {
        Out.bSavedToDisk = SaveAssetToDiskReportingPresence(
            Skeleton, /*bForce=*/true, &Out.PackageName, &Out.SizeBytes, &Out.SaveState);
    }
    else
    {
        Out.SaveState = EAssetSaveState::NotRequested;
    }
    Out.bPendingFlush = Spec.bSave && !Out.bSavedToDisk;

    Out.bSuccess = true;
    return Out;
}
