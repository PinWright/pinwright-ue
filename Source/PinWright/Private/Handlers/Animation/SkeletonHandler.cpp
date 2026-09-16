// Copyright (c) 2026 Alexander Penkin. MIT License.

// SkeletonHandler.cpp - Migrated from PinWright_SkeletonHandlers.cpp
// Core skeleton and bone operations: get info, list/add/remove bones, virtual bones, sockets
//
// Phase 14 migration to auto-registration system.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"
#include "PinWrightHelpers.h"
#include "PinWrightGlobals.h"
#include "PinWrightSubsystem.h"
#include "Utils/JsonUtils.h"


#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "ReferenceSkeleton.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace {

static FString DescribeAssetTypeSkel(const UObject* Asset)
{
    if (!Asset || !Asset->GetClass())
    {
        return TEXT("UObject");
    }
    if (Asset->IsA(USkeleton::StaticClass()))
    {
        return TEXT("USkeleton");
    }
    if (Asset->IsA(USkeletalMesh::StaticClass()))
    {
        return TEXT("USkeletalMesh");
    }
    return Asset->GetClass()->GetName();
}

// Helper: Load skeleton asset from path
static USkeleton* LoadSkeletonFromPathSkel(const FString& SkeletonPath, FString& OutError, bool* bOutWrongType = nullptr)
{
    OutError.Reset();
    if (bOutWrongType)
    {
        *bOutWrongType = false;
    }
    if (SkeletonPath.IsEmpty())
    {
        OutError = TEXT("Skeleton path is required");
        return nullptr;
    }

    UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *SkeletonPath);
    if (!Asset)
    {
        OutError = FString::Printf(TEXT("Skeleton asset not found: %s"), *SkeletonPath);
        return nullptr;
    }

    USkeleton* Skeleton = Cast<USkeleton>(Asset);
    if (!Skeleton)
    {
        if (bOutWrongType)
        {
            *bOutWrongType = true;
        }
        OutError = FString::Printf(TEXT("Asset '%s' is a %s, not a USkeleton. Pass a USkeleton asset path."), *SkeletonPath, *DescribeAssetTypeSkel(Asset));
        return nullptr;
    }

    return Skeleton;
}

// Helper: Load skeletal mesh asset from path
static USkeletalMesh* LoadSkeletalMeshFromPathSkel(const FString& MeshPath, FString& OutError, bool* bOutWrongType = nullptr)
{
    OutError.Reset();
    if (bOutWrongType)
    {
        *bOutWrongType = false;
    }
    if (MeshPath.IsEmpty())
    {
        OutError = TEXT("Skeletal mesh path is required");
        return nullptr;
    }

    UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *MeshPath);
    if (!Asset)
    {
        OutError = FString::Printf(TEXT("Skeletal mesh asset not found: %s"), *MeshPath);
        return nullptr;
    }

    USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset);
    if (!Mesh)
    {
        if (bOutWrongType)
        {
            *bOutWrongType = true;
        }
        OutError = FString::Printf(TEXT("Asset '%s' is a %s, not a USkeletalMesh. Pass a USkeletalMesh asset path."), *MeshPath, *DescribeAssetTypeSkel(Asset));
        return nullptr;
    }

    return Mesh;
}

// Helper: Load skeleton, falling back to skeletal mesh's skeleton
static USkeleton* LoadSkeletonOrFromMesh(const FString& AssetPath, FString& OutError, bool* bOutWrongType = nullptr)
{
    OutError.Reset();
    if (bOutWrongType)
    {
        *bOutWrongType = false;
    }

    if (AssetPath.IsEmpty())
    {
        OutError = TEXT("Skeleton or skeletal mesh path is required");
        return nullptr;
    }

    UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
    if (!Asset)
    {
        OutError = FString::Printf(TEXT("Skeleton or skeletal mesh asset not found: %s"), *AssetPath);
        return nullptr;
    }

    if (USkeleton* Skeleton = Cast<USkeleton>(Asset))
    {
        return Skeleton;
    }

    if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset))
    {
        if (USkeleton* Skeleton = Mesh->GetSkeleton())
        {
            return Skeleton;
        }

        OutError = FString::Printf(TEXT("Skeletal mesh '%s' has no bound USkeleton. Pass a skeletonPath or a mesh with a skeleton."), *AssetPath);
        return nullptr;
    }

    if (bOutWrongType)
    {
        *bOutWrongType = true;
    }
    OutError = FString::Printf(TEXT("Asset '%s' is a %s, not a USkeleton or USkeletalMesh. Pass a skeletonPath or skeletalMeshPath."), *AssetPath, *DescribeAssetTypeSkel(Asset));
    return nullptr;
}

static void SendTypedPathError(FHandlerContext& Ctx, const TCHAR* MissingCode, const FString& Error, bool bWrongType)
{
    Ctx.SendError(bWrongType ? TEXT("INVALID_ASSET_TYPE") : MissingCode, Error);
}

} // anonymous namespace


// ===========================================================================
// skeleton.get_info - Get information about a skeleton
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.get_info", "skeleton",
    "Read summary metadata about a USkeleton: bone count, socket count, virtual bone count, plus the asset path. One of skeletonPath or skeletalMeshPath is required (the latter resolves to its bound Skeleton).",
    RPC_PARAMS(
        RPC_PARAM_OPT("skeletonPath", "path", "Asset path to a USkeleton; primary input."),
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Asset path to a USkeletalMesh; used as a fallback to find the mesh's Skeleton when skeletonPath is omitted.")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString SkeletonPath = Ctx.GetStringFirstOf({TEXT("skeletonPath"), TEXT("skeletalMeshPath")});

    FString Error;
    bool bWrongType = false;
    USkeleton* Skeleton = LoadSkeletonOrFromMesh(SkeletonPath, Error, &bWrongType);

    if (!Skeleton)
    {
        SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    AddAssetVerification(Result, Skeleton);

    const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
    Result->SetNumberField(TEXT("boneCount"), RefSkeleton.GetRawBoneNum());
    Result->SetNumberField(TEXT("virtualBoneCount"), Skeleton->GetVirtualBones().Num());
    Result->SetNumberField(TEXT("socketCount"), Skeleton->Sockets.Num());

    // An empty string means the slot is unset, which is what every freshly built skeleton
    // carries: an empty preview viewport, and no way to see the rig at all. Read through the
    // CONST overload deliberately -- the non-const USkeleton::GetPreviewMesh runs a
    // compatibility check and RESETS the stored pointer when it fails, so a read verb calling
    // it would mutate the asset it was asked to describe. The const overload skips that fixup
    // and reports what is actually stored.
    const USkeletalMesh* PreviewMesh = static_cast<const USkeleton*>(Skeleton)->GetPreviewMesh();
    Result->SetStringField(TEXT("previewMesh"),
        PreviewMesh ? PreviewMesh->GetPathName() : FString());

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.list_bones - List all bones in a skeleton
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.list_bones", "skeleton",
    "Enumerate bones in a USkeleton's reference skeleton, returning name, index, parent index/name, and reference-pose location per bone. Pass skeletonPath, or skeletalMeshPath to look up the mesh's bound Skeleton. Any normal rig (~200 bones) exceeds the inline display budget and spills to a file; narrow it inline with nameFilter= (substring), limit= (max rows, totalCount keeps reporting the untruncated total), or namesOnly=true / fields=[...] to drop the per-bone location.",
    RPC_PARAMS(
        RPC_PARAM_OPT("skeletonPath", "path", "Path to the skeleton asset"),
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Path to a skeletal mesh (used as fallback)"),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("nameFilter"), TEXT("string"),
            TEXT("Substring filter (case-insensitive) applied to each bone name; omit to return all bones. Use it to discover every bone matching a fragment (e.g. \"hand\" -> hand_r/hand_l) when you do not know the exact name — skeleton.get_bone_transform only resolves one exact name. Snake_case name_filter / boneName / bone_name accepted."),
            /*bRequired=*/false, TArray<FString>({TEXT("nameFilter"), TEXT("name_filter"),
                TEXT("boneName"), TEXT("bone_name")})),
        RPC_PARAM_DEF("limit", "number", "Max bones to return after filtering. 0 (default) = all. totalCount always reports the full untruncated match count so elision is detectable.", "0"),
        RPC_PARAM_OPT_ALIAS("fields", "array|string", "Case-insensitive allow-list of per-bone keys to return (valid keys: name, index, parentIndex, parentName, location); e.g. [\"name\"] to drop the per-bone location object. Omit for all keys. A single string is also accepted.", "field"),
        RPC_PARAM_OPT_ALIAS("namesOnly", "bool", "When true, returns only name per bone (drops index/parentIndex/parentName/location — the bulk of the bytes) — shorthand for the common 'which bone names exist?' read. Snake_case names_only accepted. Ignored when fields is supplied.", "names_only")
    ))
{
    FString SkeletonPath = Ctx.GetStringFirstOf({TEXT("skeletonPath"), TEXT("skeletalMeshPath")});

    FString Error;
    bool bWrongType = false;
    USkeleton* Skeleton = LoadSkeletonOrFromMesh(SkeletonPath, Error, &bWrongType);

    if (!Skeleton)
    {
        SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    // Substring filter on the bone name (case-insensitive). Accepts snake_case and
    // boneName aliases so a caller need not guess the exact key. Serves the
    // discover-many-bones-by-fragment case get_bone_transform's exact lookup cannot.
    const FString NameFilter = Ctx.GetStringFirstOf(
        {TEXT("nameFilter"), TEXT("name_filter"), TEXT("boneName"), TEXT("bone_name")});
    const bool bFiltering = !NameFilter.IsEmpty();

    // Per-bone field projection: an explicit fields allow-list (array or bare string)
    // wins; otherwise namesOnly collapses to name only. An empty set means "no
    // projection": emit every key, so unprojected output is byte-identical to the prior
    // shape. Probe keys are lowercase to match the lowercased set ReadFieldProjection returns.
    const TSet<FString> Fields = Ctx.ReadFieldProjection({TEXT("name")});
    const bool bProject = Fields.Num() > 0;
    const auto Wants = [&Fields, bProject](const TCHAR* Key) {
        return !bProject || Fields.Contains(FString(Key));
    };
    const bool bWantName = Wants(TEXT("name"));
    const bool bWantIndex = Wants(TEXT("index"));
    const bool bWantParentIndex = Wants(TEXT("parentindex"));
    const bool bWantParentName = Wants(TEXT("parentname"));
    const bool bWantLocation = Wants(TEXT("location"));

    // limit truncates the returned array after filtering; 0 = all. totalCount below
    // always reports the full filtered count regardless of the cap.
    const int32 Limit = Ctx.GetInt(TEXT("limit"), 0);

    const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
    const int32 RawBoneNum = RefSkeleton.GetRawBoneNum();
    TArray<TSharedPtr<FJsonValue>> BoneArray;
    int32 TotalMatched = 0;

    for (int32 i = 0; i < RawBoneNum; ++i)
    {
        // Only stringify the name when the filter must test it; the unfiltered path
        // defers naming to the emit step below, so tail bones past the cap cost no alloc.
        FString BoneName;
        if (bFiltering)
        {
            BoneName = RefSkeleton.GetBoneName(i).ToString();
            if (!BoneName.Contains(NameFilter)) continue;
        }

        ++TotalMatched;
        if (Limit > 0 && BoneArray.Num() >= Limit)
        {
            // Unfiltered: every remaining bone matches, so the untruncated total is just
            // the raw count — stop scanning instead of counting bone-by-bone.
            if (!bFiltering)
            {
                TotalMatched = RawBoneNum;
                break;
            }
            continue; // filtered: keep scanning to count the rest of the matches
        }

        TSharedPtr<FJsonObject> BoneObj = MakeShareable(new FJsonObject());
        if (bWantName)
            BoneObj->SetStringField(TEXT("name"),
                bFiltering ? BoneName : RefSkeleton.GetBoneName(i).ToString());
        if (bWantIndex)
            BoneObj->SetNumberField(TEXT("index"), i);

        const int32 ParentIndex = RefSkeleton.GetParentIndex(i);
        if (bWantParentIndex)
            BoneObj->SetNumberField(TEXT("parentIndex"), ParentIndex);
        if (bWantParentName && ParentIndex >= 0)
            BoneObj->SetStringField(TEXT("parentName"), RefSkeleton.GetBoneName(ParentIndex).ToString());

        if (bWantLocation)
        {
            const FTransform& RefPose = RefSkeleton.GetRefBonePose()[i];
            TSharedPtr<FJsonObject> TransformObj = MakeShareable(new FJsonObject());
            TransformObj->SetNumberField(TEXT("x"), RefPose.GetLocation().X);
            TransformObj->SetNumberField(TEXT("y"), RefPose.GetLocation().Y);
            TransformObj->SetNumberField(TEXT("z"), RefPose.GetLocation().Z);
            BoneObj->SetObjectField(TEXT("location"), TransformObj);
        }

        BoneArray.Add(MakeShareable(new FJsonValueObject(BoneObj)));
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetArrayField(TEXT("bones"), BoneArray);
    // count = rows returned (unchanged semantics); totalCount = full untruncated match
    // count so a caller can tell the list was capped by limit. totalCount + truncated is
    // the shared filter+limit vocabulary of the sibling list readers that emit totalCount
    // (skeleton.list_physics_bodies, volume.get_volumes_info); actor.list / list_objects
    // spell the same untruncated total totalMatches, not totalCount.
    Result->SetNumberField(TEXT("count"), BoneArray.Num());
    Result->SetNumberField(TEXT("totalCount"), TotalMatched);
    Result->SetBoolField(TEXT("truncated"), BoneArray.Num() < TotalMatched);
    if (bFiltering)
        Result->SetStringField(TEXT("filter"), NameFilter);
    AddAssetVerification(Result, Skeleton);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.get_bone_transform - Get transform of a specific bone
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.get_bone_transform", "skeleton",
    "Read the reference (bind) pose transform of a single bone by name. Returns location, rotation, and scale in the bone's local space (relative to its parent).",
    RPC_PARAMS(
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Path to the skeletal mesh"),
        RPC_PARAM_OPT("skeletonPath", "path", "Path to the skeleton asset"),
        RPC_PARAM_REQ("boneName", "string", "Name of the bone"),
        RPC_PARAM_OPT("lodIndex", "integer", "LOD index (default 0)")
    ))
{
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    FString BoneName = Ctx.GetString(TEXT("boneName"));

    if (BoneName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("boneName is required"));
        return true;
    }

    const FReferenceSkeleton* RefSkeleton = nullptr;

    if (!SkeletalMeshPath.IsEmpty())
    {
        FString Error;
        bool bWrongType = false;
        USkeletalMesh* Mesh = LoadSkeletalMeshFromPathSkel(SkeletalMeshPath, Error, &bWrongType);
        if (!Mesh)
        {
            SendTypedPathError(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
            return true;
        }
        RefSkeleton = &Mesh->GetRefSkeleton();
    }
    else if (!SkeletonPath.IsEmpty())
    {
        FString Error;
        bool bWrongType = false;
        USkeleton* Skeleton = LoadSkeletonFromPathSkel(SkeletonPath, Error, &bWrongType);
        if (!Skeleton)
        {
            SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
            return true;
        }
        RefSkeleton = &Skeleton->GetReferenceSkeleton();
    }
    else
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletalMeshPath or skeletonPath is required"));
        return true;
    }

    int32 BoneIndex = RefSkeleton->FindBoneIndex(FName(*BoneName));
    if (BoneIndex == INDEX_NONE)
    {
        Ctx.SendError(TEXT("BONE_NOT_FOUND"), FString::Printf(TEXT("Bone '%s' not found"), *BoneName));
        return true;
    }

    FTransform BoneTransform = RefSkeleton->GetRefBonePose()[BoneIndex];
    FVector Location = BoneTransform.GetLocation();
    FRotator Rotation = BoneTransform.Rotator();
    FVector Scale = BoneTransform.GetScale3D();

    int32 ParentIndex = RefSkeleton->GetParentIndex(BoneIndex);
    FString ParentName = ParentIndex != INDEX_NONE ?
        RefSkeleton->GetBoneName(ParentIndex).ToString() : TEXT("");

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("boneName"), BoneName);
    Result->SetNumberField(TEXT("boneIndex"), BoneIndex);
    Result->SetStringField(TEXT("parentBone"), ParentName);
    Result->SetNumberField(TEXT("parentIndex"), ParentIndex);

    TSharedPtr<FJsonObject> LocationObj = MakeShareable(new FJsonObject());
    LocationObj->SetNumberField(TEXT("x"), Location.X);
    LocationObj->SetNumberField(TEXT("y"), Location.Y);
    LocationObj->SetNumberField(TEXT("z"), Location.Z);
    Result->SetObjectField(TEXT("location"), LocationObj);

    TSharedPtr<FJsonObject> RotationObj = MakeShareable(new FJsonObject());
    RotationObj->SetNumberField(TEXT("pitch"), Rotation.Pitch);
    RotationObj->SetNumberField(TEXT("yaw"), Rotation.Yaw);
    RotationObj->SetNumberField(TEXT("roll"), Rotation.Roll);
    Result->SetObjectField(TEXT("rotation"), RotationObj);

    TSharedPtr<FJsonObject> ScaleObj = MakeShareable(new FJsonObject());
    ScaleObj->SetNumberField(TEXT("x"), Scale.X);
    ScaleObj->SetNumberField(TEXT("y"), Scale.Y);
    ScaleObj->SetNumberField(TEXT("z"), Scale.Z);
    Result->SetObjectField(TEXT("scale"), ScaleObj);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.set_bone_transform - Set the reference pose transform for a bone
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.set_bone_transform", "skeleton",
    "Overwrite the reference (bind) pose transform of a bone in a USkeleton. Pass skeletonPath for the Skeleton or skeletalMeshPath to resolve its bound Skeleton. Affects all skeletal meshes bound to the skeleton — use sparingly, normally only during initial rig authoring.",
    RPC_PARAMS(
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Path to a skeletal mesh whose bound Skeleton will be edited"),
        RPC_PARAM_OPT("skeletonPath", "path", "Path to the Skeleton to edit"),
        RPC_PARAM_REQ("boneName", "string", "Name of the bone"),
        RPC_PARAM_OPT("location", "object", "Location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "Rotation {pitch,yaw,roll}"),
        RPC_PARAM_OPT("scale", "object", "Scale {x,y,z}")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    const FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    FString BoneName = Ctx.GetString(TEXT("boneName"));

    if ((SkeletalMeshPath.IsEmpty() && SkeletonPath.IsEmpty()) || BoneName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletalMeshPath or skeletonPath and boneName are required"));
        return true;
    }

    if (!SkeletalMeshPath.IsEmpty() && !SkeletonPath.IsEmpty())
    {
        Ctx.SendError(TEXT("AMBIGUOUS_TARGET"), TEXT("Pass skeletalMeshPath or skeletonPath, not both; they can resolve different Skeleton assets"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeleton* Skeleton = nullptr;
    if (!SkeletalMeshPath.IsEmpty())
    {
        USkeletalMesh* Mesh = LoadSkeletalMeshFromPathSkel(SkeletalMeshPath, Error, &bWrongType);
        if (!Mesh)
        {
            SendTypedPathError(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
            return true;
        }
        Skeleton = Mesh->GetSkeleton();
        if (!Skeleton)
        {
            Ctx.SendError(TEXT("SKELETON_NOT_FOUND"), FString::Printf(TEXT("Skeletal mesh '%s' has no bound USkeleton. Pass skeletonPath or a mesh with a bound Skeleton."), *SkeletalMeshPath));
            return true;
        }
    }
    else
    {
        Skeleton = LoadSkeletonFromPathSkel(SkeletonPath, Error, &bWrongType);
        if (!Skeleton)
        {
            SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
            return true;
        }
    }

    const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
    int32 BoneIndex = RefSkeleton.FindBoneIndex(FName(*BoneName));

    if (BoneIndex == INDEX_NONE)
    {
        Ctx.SendError(TEXT("BONE_NOT_FOUND"), FString::Printf(TEXT("Bone '%s' not found"), *BoneName));
        return true;
    }

    FVector Location = ParseVectorFromJson(Payload, TEXT("location"));
    FRotator Rotation = ParseRotatorFromJson(Payload, TEXT("rotation"));
    FVector Scale = ParseVectorFromJson(Payload, TEXT("scale"), FVector::OneVector);

    FTransform NewTransform(Rotation, Location, Scale);

    FReferenceSkeletonModifier Modifier(Skeleton);
    Modifier.UpdateRefPoseTransform(BoneIndex, NewTransform);

    McpSafeAssetSave(Skeleton);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("boneName"), BoneName);
    Result->SetNumberField(TEXT("boneIndex"), BoneIndex);
    Result->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());

    // McpSafeAssetSave only marks the package dirty; it never writes. Report that
    // measured rather than leaving the caller to assume a bare success reached disk.
    AddMarkDirtySaveReport(Result, Skeleton, /*bSaveRequested=*/true);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.rename_bone - Rename a bone (virtual bones only)
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.rename_bone", "skeleton",
    "Rename a bone on a USkeleton. Only virtual bones can be renamed safely; renaming real bones invalidates animations bound to the old name and is rejected by this handler.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletonPath", "path", "Path to the skeleton asset"),
        RPC_PARAM_REQ("boneName", "string", "Current bone name"),
        RPC_PARAM_REQ("newBoneName", "string", "New bone name")
    ))
{
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    FString BoneName = Ctx.GetString(TEXT("boneName"));
    FString NewBoneName = Ctx.GetString(TEXT("newBoneName"));

    if (SkeletonPath.IsEmpty() || BoneName.IsEmpty() || NewBoneName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletonPath, boneName, and newBoneName are required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeleton* Skeleton = LoadSkeletonFromPathSkel(SkeletonPath, Error, &bWrongType);
    if (!Skeleton)
    {
        SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    const TArray<FVirtualBone>& VirtualBones = Skeleton->GetVirtualBones();
    bool bIsVirtualBone = false;
    for (const FVirtualBone& VB : VirtualBones)
    {
        if (VB.VirtualBoneName == FName(*BoneName))
        {
            bIsVirtualBone = true;
            break;
        }
    }

    if (bIsVirtualBone)
    {
        Skeleton->RenameVirtualBone(FName(*BoneName), FName(*NewBoneName));
        McpSafeAssetSave(Skeleton);

        TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
        Result->SetStringField(TEXT("oldName"), BoneName);
        Result->SetStringField(TEXT("newName"), NewBoneName);
        Result->SetBoolField(TEXT("isVirtualBone"), true);

        // McpSafeAssetSave only marks the package dirty; it never writes. Report that
        // measured rather than leaving the caller to assume a bare success reached disk.
        AddMarkDirtySaveReport(Result, Skeleton, /*bSaveRequested=*/true);

        Ctx.SendSuccess(Result);
        return true;
    }

    Ctx.SendError(TEXT("OPERATION_NOT_SUPPORTED"),
        TEXT("Renaming non-virtual bones is not supported. Only virtual bones can be renamed at runtime. To rename regular bones, reimport the skeletal mesh with updated bone names."));
    return true;
}


// ===========================================================================
// skeleton.create_skeleton - Create a new skeleton asset
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.create_skeleton", "skeleton",
    "Create an empty USkeleton asset with a single root bone. Add bones afterwards via skeleton.add_bone; sockets via skeleton.create_socket.",
    RPC_PARAMS(
        ParamAliasUtils::MakeAliasParamSpec(TEXT("path"), TEXT("path"),
            TEXT("Full asset path for the new Skeleton (e.g. /Game/Characters/SK_Foo); 'skeletonPath' alias accepted."),
            /*bRequired=*/true, TArray<FString>({TEXT("path"), TEXT("skeletonPath")})),
        RPC_PARAM_OPT("rootBoneName", "string", "Identifier of the initial root bone; defaults to 'Root'.")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString SkeletonPath = Ctx.GetStringFirstOf({TEXT("path"), TEXT("skeletonPath")});
    FString RootBoneName = Ctx.GetString(TEXT("rootBoneName"), TEXT("Root"));

    if (SkeletonPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("path or skeletonPath is required"));
        return true;
    }

    // Validate path to prevent path traversal attacks
    if (!IsValidMountPoint(SkeletonPath) && !SkeletonPath.StartsWith(TEXT("/Temp/")))
    {
        Ctx.SendError(TEXT("INVALID_PATH"), TEXT("Invalid path. Path must start with a valid mount point or /Temp/"));
        return true;
    }

    if (SkeletonPath.Contains(TEXT("..")) || SkeletonPath.Contains(TEXT("//")) || SkeletonPath.Contains(TEXT("\\")))
    {
        Ctx.SendError(TEXT("INVALID_PATH"), TEXT("Invalid path. Path contains illegal characters or traversal sequences"));
        return true;
    }

    FPackageName::EErrorCode ErrorCode;
    if (!FPackageName::IsValidLongPackageName(SkeletonPath, false, &ErrorCode))
    {
        FString ErrorMsg = FPackageName::FormatErrorAsString(SkeletonPath, ErrorCode);
        Ctx.SendError(TEXT("INVALID_PATH"), FString::Printf(TEXT("Invalid package path: %s"), *ErrorMsg));
        return true;
    }

    FString PackagePath = FPaths::GetPath(SkeletonPath);
    FString SkeletonName = FPaths::GetBaseFilename(SkeletonPath);
    FString FullPackagePath = PackagePath / SkeletonName;

    // CreatePackage (UObjectGlobals.cpp:1086-1120) logs at Fatal - a verbosity that is not
    // compiled out in any configuration, so it ends the PROCESS - on a name containing "//"
    // or on one that resolves to empty. The checks above are on SkeletonPath; this one is on
    // the string that is actually handed to CreatePackage, which is a RECOMPOSITION of it.
    // Today the two are provably identical (SkeletonPath has already been rejected for "//",
    // a trailing slash and INVALID_LONGPACKAGE_CHARACTERS - which contains '.' - so
    // GetPath()/GetBaseFilename() split and rejoin it without loss), and no caller string is
    // known that reaches the Fatal here. That is exactly why the check belongs on this line:
    // the identity is a property of the three lines above it, not of the call below it, and
    // an edit to the composition would silently reopen the door.
    // bIncludeReadOnlyRoots=true so this can only ever refuse what the stricter check above
    // already accepted - it narrows nothing that works today
    // (board B-createpackage-unvalidated-paths-plugin-wide).
    FText ComposedPathReason;
    if (!FPackageName::IsValidLongPackageName(FullPackagePath, /*bIncludeReadOnlyRoots=*/true,
            &ComposedPathReason))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("'%s' does not compose a valid package path ('%s'): %s"),
                *SkeletonPath, *FullPackagePath, *ComposedPathReason.ToString()));
        return true;
    }

    UPackage* Package = CreatePackage(*FullPackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package"));
        return true;
    }

    USkeleton* NewSkeleton = NewObject<USkeleton>(Package, *SkeletonName, RF_Public | RF_Standalone);
    if (!NewSkeleton)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create skeleton object"));
        return true;
    }

    FReferenceSkeletonModifier Modifier(NewSkeleton);
    FMeshBoneInfo RootBone;
    RootBone.Name = FName(*RootBoneName);
    RootBone.ParentIndex = INDEX_NONE;
    RootBone.ExportName = RootBoneName;
    Modifier.Add(RootBone, FTransform::Identity, true);

    McpSafeAssetSave(NewSkeleton);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("skeletonPath"), NewSkeleton->GetPathName());
    Result->SetStringField(TEXT("rootBoneName"), RootBoneName);
    Result->SetNumberField(TEXT("boneCount"), 1);

    // McpSafeAssetSave only marks the package dirty; it never writes. Report that
    // measured rather than leaving the caller to assume a bare success reached disk.
    AddMarkDirtySaveReport(Result, NewSkeleton, /*bSaveRequested=*/true);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.add_bone - Add a bone to a skeleton
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.add_bone", "skeleton",
    "Append a new bone to a USkeleton's reference skeleton, optionally as a child of an existing bone. Position/rotation/scale form the bone's reference (bind) pose.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletonPath", "path", "Asset path to the target Skeleton."),
        RPC_PARAM_REQ("boneName", "string", "Identifier for the new bone; must be unique within the skeleton."),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("parentBone"), TEXT("string"),
            TEXT("Existing bone name to parent under; omit for root-attached. Snake_case 'parentBoneName' alias accepted."),
            /*bRequired=*/false, TArray<FString>({TEXT("parentBone"), TEXT("parentBoneName")})),
        RPC_PARAM_OPT("location", "object", "Bone-space location of the new bone in the bind pose, {x,y,z}; defaults to (0,0,0)."),
        RPC_PARAM_OPT("rotation", "object", "Bone-space rotation in degrees, {pitch,yaw,roll}; defaults to identity."),
        RPC_PARAM_OPT("scale", "object", "Bone-space scale, {x,y,z}; defaults to (1,1,1).")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    FString BoneName = Ctx.GetString(TEXT("boneName"));
    FString ParentName = Ctx.GetStringFirstOf({TEXT("parentBone"), TEXT("parentBoneName")});

    if (SkeletonPath.IsEmpty() || BoneName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletonPath and boneName are required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeleton* Skeleton = LoadSkeletonFromPathSkel(SkeletonPath, Error, &bWrongType);
    if (!Skeleton)
    {
        SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();

    if (RefSkeleton.FindBoneIndex(FName(*BoneName)) != INDEX_NONE)
    {
        Ctx.SendError(TEXT("BONE_EXISTS"), FString::Printf(TEXT("Bone '%s' already exists"), *BoneName));
        return true;
    }

    int32 ParentIndex = INDEX_NONE;
    if (!ParentName.IsEmpty())
    {
        ParentIndex = RefSkeleton.FindBoneIndex(FName(*ParentName));
        if (ParentIndex == INDEX_NONE)
        {
            Ctx.SendError(TEXT("PARENT_NOT_FOUND"), FString::Printf(TEXT("Parent bone '%s' not found"), *ParentName));
            return true;
        }
    }
    else if (RefSkeleton.GetRawBoneNum() > 0)
    {
        Ctx.SendError(TEXT("PARENT_REQUIRED"), TEXT("Cannot add root bone; Skeleton already has bones. Specify parentBone."));
        return true;
    }

    FVector Location = ParseVectorFromJson(Payload, TEXT("location"));
    FRotator Rotation = ParseRotatorFromJson(Payload, TEXT("rotation"));
    FVector Scale = ParseVectorFromJson(Payload, TEXT("scale"), FVector::OneVector);
    FTransform BoneTransform(Rotation, Location, Scale);

    FReferenceSkeletonModifier Modifier(Skeleton);
    FMeshBoneInfo NewBone;
    NewBone.Name = FName(*BoneName);
    NewBone.ParentIndex = ParentIndex;
    NewBone.ExportName = BoneName;

    bool bAllowMultipleRoots = ParentIndex == INDEX_NONE && RefSkeleton.GetRawBoneNum() == 0;
    Modifier.Add(NewBone, BoneTransform, bAllowMultipleRoots);

    McpSafeAssetSave(Skeleton);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("boneName"), BoneName);
    Result->SetStringField(TEXT("parentBone"), ParentName);
    Result->SetNumberField(TEXT("boneCount"), Skeleton->GetReferenceSkeleton().GetRawBoneNum());

    // McpSafeAssetSave only marks the package dirty; it never writes. Report that
    // measured rather than leaving the caller to assume a bare success reached disk.
    AddMarkDirtySaveReport(Result, Skeleton, /*bSaveRequested=*/true);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.remove_bone - Remove a bone from a skeleton
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.remove_bone", "skeleton",
    "Delete a bone from a USkeleton's reference skeleton. Children of the removed bone are reparented to its parent. Requires UE 5.1+ for the underlying skeleton-modifier API.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletonPath", "path", "Path to the skeleton asset"),
        RPC_PARAM_REQ("boneName", "string", "Name of the bone to remove"),
        RPC_PARAM_OPT("removeChildren", "boolean", "Also remove child bones (default false)")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    FString BoneName = Ctx.GetString(TEXT("boneName"));
    bool bRemoveChildren = Ctx.GetBool(TEXT("removeChildren"), false);

    if (SkeletonPath.IsEmpty() || BoneName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletonPath and boneName are required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeleton* Skeleton = LoadSkeletonFromPathSkel(SkeletonPath, Error, &bWrongType);
    if (!Skeleton)
    {
        SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
    int32 BoneIndex = RefSkeleton.FindBoneIndex(FName(*BoneName));

    if (BoneIndex == INDEX_NONE)
    {
        Ctx.SendError(TEXT("BONE_NOT_FOUND"), FString::Printf(TEXT("Bone '%s' not found"), *BoneName));
        return true;
    }

    if (BoneIndex == 0)
    {
        Ctx.SendError(TEXT("CANNOT_REMOVE_ROOT"), TEXT("Cannot remove root bone"));
        return true;
    }

    FReferenceSkeletonModifier Modifier(Skeleton);
    Modifier.Remove(FName(*BoneName), bRemoveChildren);
    McpSafeAssetSave(Skeleton);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("removedBone"), BoneName);
    Result->SetBoolField(TEXT("childrenRemoved"), bRemoveChildren);
    Result->SetNumberField(TEXT("boneCount"), Skeleton->GetReferenceSkeleton().GetRawBoneNum());

    // McpSafeAssetSave only marks the package dirty; it never writes. Report that
    // measured rather than leaving the caller to assume a bare success reached disk.
    AddMarkDirtySaveReport(Result, Skeleton, /*bSaveRequested=*/true);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.set_bone_parent - Change bone's parent
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.set_bone_parent", "skeleton",
    "Reparent a bone to a different parent within the skeleton hierarchy. Recomputes the bone's local transform so the world position is preserved. Requires UE 5.1+.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletonPath", "path", "Path to the skeleton asset"),
        RPC_PARAM_REQ("boneName", "string", "Name of the bone to reparent"),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("parentBone"), TEXT("string"),
            TEXT("New parent bone name (or newParentBone). Empty to make root."),
            /*bRequired=*/false, TArray<FString>({TEXT("parentBone"), TEXT("newParentBone")}))
    ))
{
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    FString BoneName = Ctx.GetString(TEXT("boneName"));
    FString NewParentName = Ctx.GetStringFirstOf({TEXT("parentBone"), TEXT("newParentBone")});

    if (SkeletonPath.IsEmpty() || BoneName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletonPath and boneName are required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeleton* Skeleton = LoadSkeletonFromPathSkel(SkeletonPath, Error, &bWrongType);
    if (!Skeleton)
    {
        SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
    int32 BoneIndex = RefSkeleton.FindBoneIndex(FName(*BoneName));

    if (BoneIndex == INDEX_NONE)
    {
        Ctx.SendError(TEXT("BONE_NOT_FOUND"), FString::Printf(TEXT("Bone '%s' not found"), *BoneName));
        return true;
    }

    FReferenceSkeletonModifier Modifier(Skeleton);
    FName ParentFName = NewParentName.IsEmpty() ? NAME_None : FName(*NewParentName);
    int32 NewBoneIndex = Modifier.SetParent(FName(*BoneName), ParentFName, true);

    if (NewBoneIndex == INDEX_NONE)
    {
        Ctx.SendError(TEXT("SET_PARENT_FAILED"),
            FString::Printf(TEXT("Failed to set parent. New parent '%s' may not exist or operation invalid."), *NewParentName));
        return true;
    }

    McpSafeAssetSave(Skeleton);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("boneName"), BoneName);
    Result->SetStringField(TEXT("newParent"), NewParentName.IsEmpty() ? TEXT("(none - root)") : NewParentName);
    Result->SetNumberField(TEXT("newBoneIndex"), NewBoneIndex);

    // McpSafeAssetSave only marks the package dirty; it never writes. Report that
    // measured rather than leaving the caller to assume a bare success reached disk.
    AddMarkDirtySaveReport(Result, Skeleton, /*bSaveRequested=*/true);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.create_virtual_bone - Create a virtual bone between two bones
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.create_virtual_bone", "skeleton",
    "Create a virtual bone — a bone that interpolates between source and target bones at runtime, useful for IK / aim helpers — without modifying the underlying skeletal mesh. Virtual bones can be renamed (unlike real bones) and removed via skeleton.delete_virtual_bone.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletonPath", "path", "Asset path to the target Skeleton."),
        RPC_PARAM_REQ("sourceBoneName", "string", "Existing bone whose pose drives the virtual bone."),
        RPC_PARAM_REQ("targetBoneName", "string", "Existing bone the virtual bone aims toward."),
        RPC_PARAM_OPT("boneName", "string", "Custom name for the virtual bone; defaults to UE's auto-generated 'VB <source>_<target>'.")
    ))
{
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    FString SourceBone = Ctx.GetString(TEXT("sourceBoneName"));
    FString TargetBone = Ctx.GetString(TEXT("targetBoneName"));
    FString VirtualBoneName = Ctx.GetString(TEXT("boneName"));

    if (SkeletonPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletonPath is required"));
        return true;
    }

    if (SourceBone.IsEmpty() || TargetBone.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("sourceBoneName and targetBoneName are required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeleton* Skeleton = LoadSkeletonFromPathSkel(SkeletonPath, Error, &bWrongType);
    if (!Skeleton)
    {
        SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    if (VirtualBoneName.IsEmpty())
    {
        VirtualBoneName = FString::Printf(TEXT("VB_%s_to_%s"), *SourceBone, *TargetBone);
    }

    FName NewVirtualBoneName;
    bool bSuccess = Skeleton->AddNewVirtualBone(FName(*SourceBone), FName(*TargetBone), NewVirtualBoneName);

    if (!bSuccess)
    {
        Ctx.SendError(TEXT("VIRTUAL_BONE_FAILED"),
            TEXT("Failed to create virtual bone. Check that source and target bones exist."));
        return true;
    }

    if (!VirtualBoneName.IsEmpty() && NewVirtualBoneName.ToString() != VirtualBoneName)
    {
        Skeleton->RenameVirtualBone(NewVirtualBoneName, FName(*VirtualBoneName));
        NewVirtualBoneName = FName(*VirtualBoneName);
    }

    McpSafeAssetSave(Skeleton);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("virtualBoneName"), NewVirtualBoneName.ToString());
    Result->SetStringField(TEXT("sourceBone"), SourceBone);
    Result->SetStringField(TEXT("targetBone"), TargetBone);
    Result->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());

    // McpSafeAssetSave only marks the package dirty; it never writes. Report that
    // measured rather than leaving the caller to assume a bare success reached disk.
    AddMarkDirtySaveReport(Result, Skeleton, /*bSaveRequested=*/true);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.list_virtual_bones - List all virtual bones
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.list_virtual_bones", "skeleton",
    "Enumerate every virtual bone defined on a USkeleton. Returns each virtual bone's name, source bone, and target bone. Real reference-skeleton bones are not included; use skeleton.list_bones for those.",
    RPC_PARAMS(
        RPC_PARAM_OPT("skeletonPath", "path", "Path to the skeleton asset"),
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Path to a skeletal mesh (used as fallback)")
    ))
{
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));

    USkeleton* Skeleton = nullptr;

    if (!SkeletonPath.IsEmpty())
    {
        FString Error;
        bool bWrongType = false;
        Skeleton = LoadSkeletonFromPathSkel(SkeletonPath, Error, &bWrongType);
        if (!Skeleton)
        {
            SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
            return true;
        }
    }
    else if (!SkeletalMeshPath.IsEmpty())
    {
        FString Error;
        bool bWrongType = false;
        USkeletalMesh* Mesh = LoadSkeletalMeshFromPathSkel(SkeletalMeshPath, Error, &bWrongType);
        if (!Mesh)
        {
            SendTypedPathError(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
            return true;
        }
        Skeleton = Mesh->GetSkeleton();
        if (!Skeleton)
        {
            Ctx.SendError(TEXT("SKELETON_NOT_FOUND"), FString::Printf(TEXT("Skeletal mesh '%s' has no bound USkeleton. Pass skeletonPath or a mesh with a bound Skeleton."), *SkeletalMeshPath));
            return true;
        }
    }

    if (!Skeleton)
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletonPath or skeletalMeshPath is required"));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> VirtualBoneArray;
    for (const FVirtualBone& VB : Skeleton->GetVirtualBones())
    {
        TSharedPtr<FJsonObject> VBObj = MakeShareable(new FJsonObject());
        VBObj->SetStringField(TEXT("name"), VB.VirtualBoneName.ToString());
        VBObj->SetStringField(TEXT("sourceBone"), VB.SourceBoneName.ToString());
        VBObj->SetStringField(TEXT("targetBone"), VB.TargetBoneName.ToString());
        VirtualBoneArray.Add(MakeShareable(new FJsonValueObject(VBObj)));
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
    Result->SetArrayField(TEXT("virtualBones"), VirtualBoneArray);
    Result->SetNumberField(TEXT("count"), VirtualBoneArray.Num());

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.delete_virtual_bone - Remove a virtual bone
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.delete_virtual_bone", "skeleton",
    "Remove a virtual bone from a USkeleton by name. Counterpart to skeleton.create_virtual_bone. Real reference-skeleton bones cannot be deleted via this handler; use skeleton.remove_bone for those.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletonPath", "path", "Path to the skeleton asset"),
        RPC_PARAM_REQ("virtualBoneName", "string", "Name of the virtual bone to remove")
    ))
{
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    FString VirtualBoneName = Ctx.GetString(TEXT("virtualBoneName"));

    if (SkeletonPath.IsEmpty() || VirtualBoneName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletonPath and virtualBoneName are required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeleton* Skeleton = LoadSkeletonFromPathSkel(SkeletonPath, Error, &bWrongType);
    if (!Skeleton)
    {
        SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    const TArray<FVirtualBone>& VirtualBones = Skeleton->GetVirtualBones();
    int32 FoundIndex = INDEX_NONE;
    for (int32 i = 0; i < VirtualBones.Num(); ++i)
    {
        if (VirtualBones[i].VirtualBoneName == FName(*VirtualBoneName))
        {
            FoundIndex = i;
            break;
        }
    }

    if (FoundIndex == INDEX_NONE)
    {
        Ctx.SendError(TEXT("VBONE_NOT_FOUND"), FString::Printf(TEXT("Virtual bone '%s' not found"), *VirtualBoneName));
        return true;
    }

    TArray<FName> BonesToRemove;
    BonesToRemove.Add(FName(*VirtualBoneName));
    Skeleton->RemoveVirtualBones(BonesToRemove);
    McpSafeAssetSave(Skeleton);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("skeletonPath"), SkeletonPath);
    Result->SetStringField(TEXT("virtualBoneName"), VirtualBoneName);
    Result->SetNumberField(TEXT("remainingVirtualBones"), Skeleton->GetVirtualBones().Num());

    // McpSafeAssetSave only marks the package dirty; it never writes. Report that
    // measured rather than leaving the caller to assume a bare success reached disk.
    AddMarkDirtySaveReport(Result, Skeleton, /*bSaveRequested=*/true);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.list_sockets - List all sockets in a skeleton/skeletal mesh
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.list_sockets", "skeleton",
    "Enumerate every USkeletalMeshSocket visible on a Skeleton, or on a SkeletalMesh together with the Skeleton it is bound to. Returns name, parent bone, relative transform and owning asset per socket. A socket can live on either asset: a mesh-only socket travels with that one mesh, a skeleton socket is shared by every mesh bound to the skeleton. Pass skeletalMeshPath to see both - the mesh's own sockets are listed first, matching the order the engine resolves attachment names in, so an earlier mesh-only socket shadows a skeleton socket of the same name.",
    RPC_PARAMS(
        RPC_PARAM_OPT("skeletonPath", "path", "Path to the Skeleton asset. Lists that skeleton's sockets only; a mesh-only socket is not visible from here because no mesh is named."),
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Path to a SkeletalMesh. Lists that mesh's own sockets followed by its bound Skeleton's. Use this whenever you need the sockets a given mesh actually resolves.")
    ))
{
    const FString MeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    const FString SkeletonPathParam = Ctx.GetString(TEXT("skeletonPath"));
    if (!MeshPath.IsEmpty() && !SkeletonPathParam.IsEmpty())
    {
        Ctx.SendError(TEXT("AMBIGUOUS_TARGET"), TEXT("Pass skeletonPath or skeletalMeshPath, not both; socket storage is different on the two assets"));
        return true;
    }

    FString Error;
    USkeleton* Skeleton = nullptr;
    // Skeleton->Sockets alone under-reports a mesh that carries any. That silence is the
    // dangerous direction: a caller auditing what it is about to destroy sees fewer sockets
    // than exist and concludes the missing ones were never there.
    USkeletalMesh* Mesh = nullptr;
    if (!MeshPath.IsEmpty())
    {
        bool bWrongType = false;
        Mesh = LoadSkeletalMeshFromPathSkel(MeshPath, Error, &bWrongType);
        if (!Mesh)
        {
            SendTypedPathError(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
            return true;
        }
        Skeleton = Mesh->GetSkeleton();
        if (!Skeleton)
        {
            Ctx.SendError(TEXT("SKELETON_NOT_FOUND"), FString::Printf(TEXT("Skeletal mesh '%s' has no bound USkeleton. Pass skeletonPath or a mesh with a bound Skeleton."), *MeshPath));
            return true;
        }
    }
    else if (!SkeletonPathParam.IsEmpty())
    {
        bool bWrongType = false;
        Skeleton = LoadSkeletonFromPathSkel(SkeletonPathParam, Error, &bWrongType);
        if (!Skeleton)
        {
            SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
            return true;
        }
    }
    else
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletonPath or skeletalMeshPath is required"));
        return true;
    }

    // A mesh's own sockets live in USkeletalMesh::Sockets, NOT on the Skeleton, so walking
    // Skeleton->Sockets alone under-reports a mesh that carries any. That silence is the
    // dangerous direction: a caller auditing what it is about to destroy sees fewer sockets
    // than exist and concludes the missing ones were never there.

    // Built with explicit loops rather than Append: both engine containers are
    // TArray<TObjectPtr<USkeletalMeshSocket>>, and the per-element conversion in a ranged-for is
    // the shape the rest of this file already relies on.
    TArray<USkeletalMeshSocket*> Ordered;
    if (Mesh)
    {
        // GetActiveSocketList() order: mesh sockets first, then the skeleton's. That order is
        // the resolution order FindSocket uses, so preserving it is what makes a shadowed
        // skeleton socket visible as shadowed rather than merely duplicated.
        for (USkeletalMeshSocket* MeshSocket : Mesh->GetMeshOnlySocketList())
        {
            Ordered.Add(MeshSocket);
        }
    }
    const int32 MeshOnlyCount = Ordered.Num();
    for (USkeletalMeshSocket* SkeletonSocket : Skeleton->Sockets)
    {
        Ordered.Add(SkeletonSocket);
    }

    TArray<TSharedPtr<FJsonValue>> SocketArray;
    for (USkeletalMeshSocket* Socket : Ordered)
    {
        if (!Socket) continue;

        TSharedPtr<FJsonObject> SocketObj = MakeShareable(new FJsonObject());
        SocketObj->SetStringField(TEXT("name"), Socket->SocketName.ToString());
        SocketObj->SetStringField(TEXT("boneName"), Socket->BoneName.ToString());
        const bool bMeshOwned = Mesh && Socket->GetOuter() == Mesh;
        SocketObj->SetStringField(TEXT("owner"), bMeshOwned ? TEXT("mesh") : TEXT("skeleton"));
        SocketObj->SetStringField(TEXT("ownedBy"),
            bMeshOwned ? Mesh->GetPathName() : Skeleton->GetPathName());

        TSharedPtr<FJsonObject> LocObj = MakeShareable(new FJsonObject());
        LocObj->SetNumberField(TEXT("x"), Socket->RelativeLocation.X);
        LocObj->SetNumberField(TEXT("y"), Socket->RelativeLocation.Y);
        LocObj->SetNumberField(TEXT("z"), Socket->RelativeLocation.Z);
        SocketObj->SetObjectField(TEXT("relativeLocation"), LocObj);

        TSharedPtr<FJsonObject> RotObj = MakeShareable(new FJsonObject());
        RotObj->SetNumberField(TEXT("pitch"), Socket->RelativeRotation.Pitch);
        RotObj->SetNumberField(TEXT("yaw"), Socket->RelativeRotation.Yaw);
        RotObj->SetNumberField(TEXT("roll"), Socket->RelativeRotation.Roll);
        SocketObj->SetObjectField(TEXT("relativeRotation"), RotObj);

        TSharedPtr<FJsonObject> ScaleObj = MakeShareable(new FJsonObject());
        ScaleObj->SetNumberField(TEXT("x"), Socket->RelativeScale.X);
        ScaleObj->SetNumberField(TEXT("y"), Socket->RelativeScale.Y);
        ScaleObj->SetNumberField(TEXT("z"), Socket->RelativeScale.Z);
        SocketObj->SetObjectField(TEXT("relativeScale"), ScaleObj);

        SocketArray.Add(MakeShareable(new FJsonValueObject(SocketObj)));
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetArrayField(TEXT("sockets"), SocketArray);
    Result->SetNumberField(TEXT("count"), SocketArray.Num());
    // Split the count so a caller can gate on "this mesh carries sockets of its own" without
    // walking the array. Both are always emitted: a meshOnlyCount that appeared only when it
    // was non-zero would have to be known about to be missed.
    Result->SetNumberField(TEXT("meshOnlyCount"), MeshOnlyCount);
    Result->SetNumberField(TEXT("skeletonCount"), SocketArray.Num() - MeshOnlyCount);
    Result->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
    Result->SetStringField(TEXT("skeletalMeshPath"), Mesh ? Mesh->GetPathName() : FString());

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.create_socket - Create a new socket on a skeleton
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.create_socket", "skeleton",
    "Add a USkeletalMeshSocket at a relative offset from a bone, as an attachment anchor for weapons, props or particles. WHICH ASSET receives it is the choice this verb makes: skeletonPath puts it on the Skeleton, where every mesh bound to that skeleton gets it; skeletalMeshPath puts it on that one mesh alone. Pass exactly one. A rig shared across characters - any Mannequin-derived skeleton - makes the difference load-bearing, because a socket meant for one character otherwise appears on all of them.",
    RPC_PARAMS(
        RPC_PARAM_OPT("skeletonPath", "path", "Asset path to a Skeleton. The socket is added to the SKELETON and is therefore visible on every SkeletalMesh bound to it."),
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Asset path to a SkeletalMesh. The socket is added to THAT MESH only and travels with it; no other mesh sharing the skeleton is touched."),
        RPC_PARAM_REQ("socketName", "string", "Identifier for the new socket; must not collide with a socket already on the target asset, nor with one on the Skeleton a named mesh is bound to."),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("attachBoneName"), TEXT("string"),
            TEXT("Existing bone the socket is anchored to. 'boneName' alias accepted."),
            /*bRequired=*/true, TArray<FString>({TEXT("attachBoneName"), TEXT("boneName")})),
        RPC_PARAM_OPT("relativeLocation", "object", "Bone-space offset for the socket as {x,y,z}; defaults to origin."),
        RPC_PARAM_OPT("relativeRotation", "object", "Bone-space rotation as {pitch,yaw,roll}; defaults to identity."),
        RPC_PARAM_OPT("relativeScale", "object", "Bone-space scale as {x,y,z}; defaults to (1,1,1).")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const FString MeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    const FString SkeletonPathParam = Ctx.GetString(TEXT("skeletonPath"));

    FString SocketName = Ctx.GetString(TEXT("socketName"));
    FString BoneName = Ctx.GetStringFirstOf({TEXT("attachBoneName"), TEXT("boneName")});

    if (SocketName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("socketName is required"));
        return true;
    }

    if (BoneName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("attachBoneName or boneName is required"));
        return true;
    }

    if (MeshPath.IsEmpty() && SkeletonPathParam.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"),
            TEXT("skeletonPath or skeletalMeshPath is required - and which one you pass decides "
                 "whether the socket lands on the shared Skeleton or on that one SkeletalMesh"));
        return true;
    }

    // Both named is refused rather than resolved by precedence. The two produce different
    // assets - one shared, one not - so a silent winner would write the socket somewhere the
    // caller did not choose, which is the failure this verb's documentation now warns about.
    if (!MeshPath.IsEmpty() && !SkeletonPathParam.IsEmpty())
    {
        Ctx.SendError(TEXT("AMBIGUOUS_TARGET"),
            TEXT("Pass skeletonPath OR skeletalMeshPath, not both: the first adds the socket to "
                 "the Skeleton, where every mesh bound to it inherits the socket, and the second "
                 "adds it to that mesh alone"));
        return true;
    }

    // A mesh path means the MESH owns the socket. Resolving it to the mesh's Skeleton - which
    // is what this verb used to do for every caller - writes into an asset shared by every
    // other mesh on that rig, so a socket meant for one character appeared on all of them.
    FString Error;
    bool bWrongType = false;
    USkeletalMesh* TargetMesh = nullptr;
    USkeleton* Skeleton = nullptr;
    if (!MeshPath.IsEmpty())
    {
        TargetMesh = LoadSkeletalMeshFromPathSkel(MeshPath, Error, &bWrongType);
        if (!TargetMesh)
        {
            SendTypedPathError(Ctx, TEXT("SKELETAL_MESH_NOT_FOUND"), Error, bWrongType);
            return true;
        }
        Skeleton = TargetMesh->GetSkeleton();
        if (!Skeleton)
        {
            Ctx.SendError(TEXT("SKELETON_NOT_FOUND"),
                FString::Printf(TEXT("Skeletal mesh '%s' has no bound USkeleton. Pass skeletonPath or a mesh with a bound Skeleton."), *MeshPath));
            return true;
        }
    }
    else
    {
        Skeleton = LoadSkeletonFromPathSkel(SkeletonPathParam, Error, &bWrongType);
        if (!Skeleton)
        {
            SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
            return true;
        }
    }

    if (Skeleton->GetReferenceSkeleton().FindBoneIndex(FName(*BoneName)) == INDEX_NONE)
    {
        Ctx.SendError(TEXT("BONE_NOT_FOUND"),
            FString::Printf(TEXT("Bone '%s' is not present on Skeleton '%s'; no socket was created. Pass an existing bone name in attachBoneName."),
                *BoneName, *Skeleton->GetPathName()));
        return true;
    }

    // Uniqueness is checked across BOTH lists even when only one is the write target.
    // USkeletalMesh::FindSocket returns the mesh's match first, so a mesh-only socket sharing a
    // name with a skeleton socket silently shadows it - attachment then resolves to a different
    // transform than the skeleton says, with nothing reporting the collision.
    // Generic over the container: USkeleton::Sockets and USkeletalMesh::GetMeshOnlySocketList()
    // are both TArray<TObjectPtr<USkeletalMeshSocket>>, which no single TArrayView spells.
    auto NameCollides = [&SocketName](const auto& List) -> bool
    {
        for (const auto& Existing : List)
        {
            if (Existing && Existing->SocketName == FName(*SocketName))
            {
                return true;
            }
        }
        return false;
    };

    if (NameCollides(Skeleton->Sockets))
    {
        Ctx.SendError(TEXT("SOCKET_EXISTS"),
            FString::Printf(TEXT("Socket '%s' already exists on Skeleton '%s'"),
                *SocketName, *Skeleton->GetPathName()));
        return true;
    }
    if (TargetMesh && NameCollides(TargetMesh->GetMeshOnlySocketList()))
    {
        Ctx.SendError(TEXT("SOCKET_EXISTS"),
            FString::Printf(TEXT("Socket '%s' already exists on SkeletalMesh '%s'"),
                *SocketName, *TargetMesh->GetPathName()));
        return true;
    }

    // The engine requires a socket's Outer to be the asset that owns it.
    UObject* SocketOuter = TargetMesh ? static_cast<UObject*>(TargetMesh) : static_cast<UObject*>(Skeleton);
    USkeletalMeshSocket* NewSocket = NewObject<USkeletalMeshSocket>(SocketOuter);
    if (!NewSocket)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create socket object"));
        return true;
    }
    NewSocket->SocketName = FName(*SocketName);
    NewSocket->BoneName = FName(*BoneName);
    NewSocket->RelativeLocation = ParseVectorFromJson(Payload, TEXT("relativeLocation"));
    NewSocket->RelativeRotation = ParseRotatorFromJson(Payload, TEXT("relativeRotation"));
    NewSocket->RelativeScale = ParseVectorFromJson(Payload, TEXT("relativeScale"), FVector::OneVector);

    // delete_socket already transacts its socket-list edit; create and configure did not,
    // so the two halves of the same round trip were undoable and non-undoable.
    UObject* WrittenAsset = SocketOuter;
    if (TargetMesh)
    {
        TargetMesh->Modify();
        TargetMesh->GetMeshOnlySocketList().Add(NewSocket);
    }
    else
    {
        Skeleton->Modify();
        Skeleton->Sockets.Add(NewSocket);
    }
    McpSafeAssetSave(WrittenAsset);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("socketName"), SocketName);
    Result->SetStringField(TEXT("boneName"), BoneName);
    Result->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
    // Report the asset that was actually written, not just the skeleton that was resolved.
    // A caller cannot otherwise tell a mesh-only socket from a shared one after the fact.
    Result->SetStringField(TEXT("owner"), TargetMesh ? TEXT("mesh") : TEXT("skeleton"));
    Result->SetStringField(TEXT("ownedBy"), WrittenAsset->GetPathName());
    Result->SetStringField(TEXT("skeletalMeshPath"),
        TargetMesh ? TargetMesh->GetPathName() : FString());

    // McpSafeAssetSave only marks the package dirty; it never writes. Report that
    // measured rather than leaving the caller to assume a bare success reached disk.
    AddMarkDirtySaveReport(Result, WrittenAsset, /*bSaveRequested=*/true);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.configure_socket - Modify an existing socket's properties
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.configure_socket", "skeleton",
    "Update an existing USkeletalMeshSocket's parent bone, relative transform, or scale. Lookup is by socket name. With skeletalMeshPath the mesh's OWN sockets are searched before the Skeleton's - the order the engine resolves attachment names in - and the response's 'owner' field says which asset was actually edited, because editing a skeleton socket changes every mesh bound to that skeleton. Use skeleton.create_socket to add new sockets, skeleton.delete_socket to remove.",
    RPC_PARAMS(
        RPC_PARAM_OPT("skeletonPath", "path", "Path to the Skeleton asset. Only that skeleton's own sockets are searched."),
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Path to a SkeletalMesh. Searches the mesh's own sockets first, then its bound Skeleton's."),
        RPC_PARAM_REQ("socketName", "string", "Name of the socket to configure"),
        RPC_PARAM_OPT("attachBoneName", "string", "New bone to attach to"),
        RPC_PARAM_OPT("relativeLocation", "object", "Relative location {x,y,z}"),
        RPC_PARAM_OPT("relativeRotation", "object", "Relative rotation {pitch,yaw,roll}"),
        RPC_PARAM_OPT("relativeScale", "object", "Relative scale {x,y,z}")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const FString MeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    const FString SkeletonPathParam = Ctx.GetString(TEXT("skeletonPath"));

    FString SocketName = Ctx.GetString(TEXT("socketName"));
    if (SocketName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("socketName is required"));
        return true;
    }

    const FString NewBoneName = Ctx.GetString(TEXT("attachBoneName"));
    const bool bHasAttachBone = Payload->HasField(TEXT("attachBoneName")) && !NewBoneName.IsEmpty();
    const bool bHasLocation = Payload->HasField(TEXT("relativeLocation"));
    const bool bHasRotation = Payload->HasField(TEXT("relativeRotation"));
    const bool bHasScale = Payload->HasField(TEXT("relativeScale"));
    if (!bHasAttachBone && !bHasLocation && !bHasRotation && !bHasScale)
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("At least one of attachBoneName, relativeLocation, relativeRotation, or relativeScale is required to change the socket"));
        return true;
    }

    if (MeshPath.IsEmpty() && SkeletonPathParam.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletonPath or skeletalMeshPath is required"));
        return true;
    }
    if (!MeshPath.IsEmpty() && !SkeletonPathParam.IsEmpty())
    {
        Ctx.SendError(TEXT("AMBIGUOUS_TARGET"), TEXT("Pass skeletonPath or skeletalMeshPath, not both; socket storage is different on the two assets"));
        return true;
    }

    FString Error;
    // The mesh's own sockets are searched FIRST, matching USkeletalMesh::FindSocket. Searching
    // only the skeleton reported SOCKET_NOT_FOUND for a socket the mesh plainly has, and - worse
    // where the name exists on both - edited the shared skeleton copy while the mesh's own copy
    // is the one that actually resolves, so the caller saw success and no visible change.
    USkeletalMesh* TargetMesh = nullptr;
    USkeleton* Skeleton = nullptr;
    bool bWrongType = false;
    if (!MeshPath.IsEmpty())
    {
        TargetMesh = LoadSkeletalMeshFromPathSkel(MeshPath, Error, &bWrongType);
        if (!TargetMesh)
        {
            SendTypedPathError(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
            return true;
        }
        Skeleton = TargetMesh->GetSkeleton();
        if (!Skeleton)
        {
            Ctx.SendError(TEXT("SKELETON_NOT_FOUND"), FString::Printf(TEXT("Skeletal mesh '%s' has no bound USkeleton. Pass skeletonPath or a mesh with a bound Skeleton."), *MeshPath));
            return true;
        }
    }
    else
    {
        Skeleton = LoadSkeletonFromPathSkel(SkeletonPathParam, Error, &bWrongType);
        if (!Skeleton)
        {
            SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
            return true;
        }
    }

    USkeletalMeshSocket* Socket = nullptr;
    bool bMeshOwned = false;
    if (TargetMesh)
    {
        for (USkeletalMeshSocket* S : TargetMesh->GetMeshOnlySocketList())
        {
            if (S && S->SocketName == FName(*SocketName))
            {
                Socket = S;
                bMeshOwned = true;
                break;
            }
        }
    }
    if (!Socket)
    {
        for (USkeletalMeshSocket* S : Skeleton->Sockets)
        {
            if (S && S->SocketName == FName(*SocketName))
            {
                Socket = S;
                break;
            }
        }
    }

    if (!Socket)
    {
        Ctx.SendError(TEXT("SOCKET_NOT_FOUND"), FString::Printf(TEXT("Socket '%s' not found"), *SocketName));
        return true;
    }

    if (bHasAttachBone)
    {
        const FReferenceSkeleton& RefSkeleton = bMeshOwned
            ? TargetMesh->GetRefSkeleton()
            : Skeleton->GetReferenceSkeleton();
        if (RefSkeleton.FindBoneIndex(FName(*NewBoneName)) == INDEX_NONE)
        {
            Ctx.SendError(TEXT("BONE_NOT_FOUND"),
                FString::Printf(TEXT("Bone '%s' is not present on the socket's target asset; no change was made. Pass an existing bone name in attachBoneName."), *NewBoneName));
            return true;
        }
    }

    // Dirty and save the asset that actually owns the socket, not the skeleton it was reached
    // through: a mesh-only edit saved against the skeleton leaves the real change unsaved.
    UObject* OwningAsset = bMeshOwned ? static_cast<UObject*>(TargetMesh) : static_cast<UObject*>(Skeleton);
    OwningAsset->Modify();
    Socket->Modify();

    if (bHasAttachBone)
    {
        Socket->BoneName = FName(*NewBoneName);
    }

    if (Payload->HasField(TEXT("relativeLocation")))
    {
        Socket->RelativeLocation = ParseVectorFromJson(Payload, TEXT("relativeLocation"));
    }

    if (Payload->HasField(TEXT("relativeRotation")))
    {
        Socket->RelativeRotation = ParseRotatorFromJson(Payload, TEXT("relativeRotation"));
    }

    if (Payload->HasField(TEXT("relativeScale")))
    {
        Socket->RelativeScale = ParseVectorFromJson(Payload, TEXT("relativeScale"), FVector::OneVector);
    }

    McpSafeAssetSave(OwningAsset);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("socketName"), SocketName);
    Result->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
    Result->SetStringField(TEXT("owner"), bMeshOwned ? TEXT("mesh") : TEXT("skeleton"));
    Result->SetStringField(TEXT("ownedBy"), OwningAsset->GetPathName());

    // McpSafeAssetSave only marks the package dirty; it never writes. Report that
    // measured rather than leaving the caller to assume a bare success reached disk.
    AddMarkDirtySaveReport(Result, OwningAsset, /*bSaveRequested=*/true);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.delete_socket - Remove a socket from skeleton
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.delete_socket", "skeleton",
    "Remove a USkeletalMeshSocket by name. Counterpart to skeleton.create_socket. With skeletalMeshPath the mesh's OWN sockets are removed first and only a name the mesh does not carry falls through to its Skeleton - where the removal is visible on every mesh bound to that skeleton, so check the response's 'owner' field to see which happened. Attachments referencing the socket fall back to the parent bone.",
    RPC_PARAMS(
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Path to a SkeletalMesh. Removes that mesh's own socket if it has one; otherwise falls through to the shared Skeleton."),
        RPC_PARAM_OPT("skeletonPath", "path", "Path to a Skeleton. Removes the socket from the skeleton, affecting every mesh bound to it."),
        RPC_PARAM_REQ("socketName", "string", "Name of the socket to delete")
    ))
{
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
    FString SocketName = Ctx.GetString(TEXT("socketName"));

    if (SocketName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("socketName is required"));
        return true;
    }

    if (!SkeletalMeshPath.IsEmpty() && !SkeletonPath.IsEmpty())
    {
        Ctx.SendError(TEXT("AMBIGUOUS_TARGET"), TEXT("Pass skeletonPath or skeletalMeshPath, not both; socket storage is different on the two assets"));
        return true;
    }

    if (!SkeletalMeshPath.IsEmpty())
    {
        FString Error;
        bool bWrongType = false;
        USkeletalMesh* Mesh = LoadSkeletalMeshFromPathSkel(SkeletalMeshPath, Error, &bWrongType);
        if (!Mesh)
        {
            SendTypedPathError(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
            return true;
        }

        // The mesh's OWN sockets are searched first. Going straight to the skeleton - which this
        // branch used to do - means "delete this socket from this mesh" deleted it from the
        // SHARED skeleton instead, removing it from every other mesh on the rig, while the
        // mesh's own socket of that name survived and kept resolving. Success was reported for
        // a destructive edit to assets the caller never named.
        int32 MeshSocketIndex = Mesh->GetMeshOnlySocketList().IndexOfByPredicate(
            [&SocketName](const USkeletalMeshSocket* S) { return S && S->SocketName == FName(*SocketName); });

        if (MeshSocketIndex != INDEX_NONE)
        {
            Mesh->Modify();
            Mesh->GetMeshOnlySocketList().RemoveAt(MeshSocketIndex);
            McpSafeAssetSave(Mesh);

            TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
            Result->SetStringField(TEXT("socketName"), SocketName);
            Result->SetStringField(TEXT("owner"), TEXT("mesh"));
            Result->SetStringField(TEXT("ownedBy"), Mesh->GetPathName());
            Result->SetStringField(TEXT("skeletalMeshPath"), Mesh->GetPathName());
            Result->SetNumberField(TEXT("remainingSockets"), Mesh->GetMeshOnlySocketList().Num());

            AddMarkDirtySaveReport(Result, Mesh, /*bSaveRequested=*/true);

            Ctx.SendSuccess(Result);
            return true;
        }

        USkeleton* Skeleton = Mesh->GetSkeleton();
        if (!Skeleton)
        {
            Ctx.SendError(TEXT("SKELETON_NOT_FOUND"), FString::Printf(TEXT("Skeletal mesh '%s' has no bound USkeleton. Pass skeletonPath or a mesh with a bound Skeleton."), *SkeletalMeshPath));
            return true;
        }
        int32 SocketIndex = Skeleton->Sockets.IndexOfByPredicate(
            [&SocketName](const USkeletalMeshSocket* S) { return S && S->SocketName == FName(*SocketName); });

        if (SocketIndex != INDEX_NONE)
        {
            Skeleton->Modify();
            Skeleton->Sockets.RemoveAt(SocketIndex);
            McpSafeAssetSave(Skeleton);

            TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
            Result->SetStringField(TEXT("socketName"), SocketName);
            Result->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
            // Names the shared asset that was actually edited. Reached through a mesh path,
            // this deletion is visible on every other mesh bound to that skeleton.
            Result->SetStringField(TEXT("owner"), TEXT("skeleton"));
            Result->SetStringField(TEXT("ownedBy"), Skeleton->GetPathName());
            Result->SetNumberField(TEXT("remainingSockets"), Skeleton->Sockets.Num());

            // McpSafeAssetSave only marks the package dirty; it never writes. Report that
            // measured rather than leaving the caller to assume a bare success reached disk.
            AddMarkDirtySaveReport(Result, Skeleton, /*bSaveRequested=*/true);

            Ctx.SendSuccess(Result);
            return true;
        }

        Ctx.SendError(TEXT("SOCKET_NOT_FOUND"), FString::Printf(TEXT("Socket '%s' not found"), *SocketName));
        return true;
    }
    else if (!SkeletonPath.IsEmpty())
    {
        FString Error;
        bool bWrongType = false;
        USkeleton* Skeleton = LoadSkeletonFromPathSkel(SkeletonPath, Error, &bWrongType);
        if (!Skeleton)
        {
            SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
            return true;
        }

        int32 SocketIndex = Skeleton->Sockets.IndexOfByPredicate(
            [&SocketName](const USkeletalMeshSocket* S) { return S && S->SocketName == FName(*SocketName); });

        if (SocketIndex != INDEX_NONE)
        {
            Skeleton->Modify();
            Skeleton->Sockets.RemoveAt(SocketIndex);
            McpSafeAssetSave(Skeleton);

            TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
            Result->SetStringField(TEXT("socketName"), SocketName);
            Result->SetStringField(TEXT("skeletonPath"), SkeletonPath);
            Result->SetNumberField(TEXT("remainingSockets"), Skeleton->Sockets.Num());

            // McpSafeAssetSave only marks the package dirty; it never writes. Report that
            // measured rather than leaving the caller to assume a bare success reached disk.
            AddMarkDirtySaveReport(Result, Skeleton, /*bSaveRequested=*/true);

            Ctx.SendSuccess(Result);
            return true;
        }

        Ctx.SendError(TEXT("SOCKET_NOT_FOUND"), FString::Printf(TEXT("Socket '%s' not found"), *SocketName));
        return true;
    }

    Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletalMeshPath or skeletonPath is required"));
    return true;
}


// ===========================================================================
// skeleton.set_preview_mesh - Choose the mesh a Skeleton's viewport opens with
// ===========================================================================
//
// This remains useful for imported and hand-authored Skeletons. For a .pwskel-generated asset,
// put preview_mesh in the source before its next compile; the recompile guard refuses to erase
// a value written by this verb unless overwrite=true grants that loss explicitly.
//
// The engine has a trap here that decides the verb's shape. USkeleton::SetPreviewMesh
// validates NOTHING - it calls Modify() and assigns - but the non-const
// USkeleton::GetPreviewMesh runs IsCompatibleForEditor on the mesh's own skeleton and, when
// that fails, Reset()s the stored pointer without dirtying anything. So an incompatible mesh
// stores, saves, and is then silently erased by the next reader. That is a write the engine
// undoes, so this verb refuses it up front rather than reporting a success that will not
// survive being looked at.
REGISTER_RPC_HANDLER("skeleton.set_preview_mesh", "skeleton",
    "Set the USkeletalMesh a USkeleton opens with in its preview viewport. A skeleton with no preview mesh - which is every freshly built one - opens empty, so there is nothing to look at and nothing to check a rig against. The mesh must be bound to this skeleton or to one it accepts as compatible: the engine silently clears a mismatched preview reference the next time anything reads the slot, so this verb refuses the mismatch rather than storing a value that will not survive.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletonPath", "path", "Asset path to the USkeleton whose preview slot is being set."),
        RPC_PARAM_REQ("skeletalMeshPath", "path", "Asset path to the USkeletalMesh to preview with. Its bound Skeleton must be this skeleton, or one this skeleton accepts as compatible.")
    ))
{
    FString SkeletonPath;
    FString MeshPath;
    if (!Ctx.RequireString(TEXT("skeletonPath"), SkeletonPath) ||
        !Ctx.RequireString(TEXT("skeletalMeshPath"), MeshPath))
    {
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeleton* Skeleton = LoadSkeletonFromPathSkel(SkeletonPath, Error, &bWrongType);
    if (!Skeleton)
    {
        SendTypedPathError(Ctx, TEXT("SKELETON_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    bWrongType = false;
    USkeletalMesh* Mesh = LoadSkeletalMeshFromPathSkel(MeshPath, Error, &bWrongType);
    if (!Mesh)
    {
        SendTypedPathError(Ctx, TEXT("SKELETAL_MESH_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    USkeleton* MeshSkeleton = Mesh->GetSkeleton();
    if (!MeshSkeleton)
    {
        Ctx.SendError(TEXT("SKELETON_MISMATCH"),
            FString::Printf(
                TEXT("SkeletalMesh '%s' has no bound Skeleton, so nothing ties it to '%s'. The ")
                TEXT("engine clears a preview reference whose mesh has no skeleton on the next read."),
                *MeshPath, *SkeletonPath));
        return true;
    }

    // The gate is IsCompatibleForEditor and not the structural IsCompatibleMesh on purpose:
    // IsCompatibleForEditor is the exact predicate USkeleton::GetPreviewMesh uses when it
    // decides whether to keep or discard the stored reference, so it is the only check that
    // predicts whether this write survives. A structurally identical mesh bound to an
    // unrelated skeleton passes IsCompatibleMesh and is still erased.
    if (!Skeleton->IsCompatibleForEditor(MeshSkeleton))
    {
        Ctx.SendError(TEXT("SKELETON_MISMATCH"),
            FString::Printf(
                TEXT("SkeletalMesh '%s' is bound to Skeleton '%s', which '%s' does not accept as ")
                TEXT("compatible. Storing it would be an unreported no-op: the engine clears a ")
                TEXT("mismatched preview reference the next time anything reads the slot. Pass a ")
                TEXT("mesh bound to this skeleton, or declare the two skeletons compatible first."),
                *MeshPath, *MeshSkeleton->GetPathName(), *SkeletonPath));
        return true;
    }

    // Read the previous occupant through the CONST overload, which reports what is stored
    // without running the fixup that would clear it. The caller needs it to put the slot back.
    const USkeletalMesh* PreviousMesh =
        static_cast<const USkeleton*>(Skeleton)->GetPreviewMesh();
    const FString PreviousPath = PreviousMesh ? PreviousMesh->GetPathName() : FString();
    const bool bChanged = (PreviousMesh != Mesh);

    // Skip the write when the slot already holds this mesh, matching the engine's own
    // FEditableSkeleton::SetPreviewMesh. SetPreviewMesh(mesh, true) calls Modify(), which
    // dirties the package, so writing an unchanged value would leave a caller with an
    // unsaved asset and nothing to save.
    if (bChanged)
    {
        // bMarkAsDirty=true makes SetPreviewMesh call Modify() before assigning, which is the
        // undo record. It does not save, so McpSafeAssetSave still has to run.
        Skeleton->SetPreviewMesh(Mesh, /*bMarkAsDirty=*/true);
        McpSafeAssetSave(Skeleton);
    }

    // Verification runs through the NON-const overload deliberately. That is the engine's own
    // fixup path, not a readback of the pointer this handler just assigned: if the engine
    // disagrees with the compatibility gate above it clears the slot here and returns null,
    // and this reports the rejection rather than the assignment.
    const USkeletalMesh* Applied = Skeleton->GetPreviewMesh();
    const bool bApplied = (Applied == Mesh);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
    Result->SetStringField(TEXT("skeletalMeshPath"), Mesh->GetPathName());
    Result->SetStringField(TEXT("previousPreviewMesh"), PreviousPath);
    Result->SetStringField(TEXT("previewMesh"), Applied ? Applied->GetPathName() : FString());
    Result->SetBoolField(TEXT("previewMeshApplied"), bApplied);
    Result->SetBoolField(TEXT("changed"), bChanged);

    if (!bApplied)
    {
        Ctx.SendError(TEXT("VERIFICATION_FAILED"),
            FString::Printf(
                TEXT("Skeleton '%s' did not keep preview mesh '%s': reading the slot back ")
                TEXT("returned '%s'. The engine rejected the reference after it was assigned."),
                *SkeletonPath, *MeshPath,
                Applied ? *Applied->GetPathName() : TEXT("(none)")),
            Result);
        return true;
    }

    AddAssetVerification(Result, Skeleton);

    // McpSafeAssetSave only marks the package dirty; it never writes. Report that measured
    // rather than leaving the caller to assume a bare success reached disk. bSaveRequested
    // follows bChanged, because an unchanged slot asked for no write at all.
    AddMarkDirtySaveReport(Result, Skeleton, /*bSaveRequested=*/bChanged);

    Ctx.SendSuccess(Result);
    return true;
}
