// Copyright (c) 2026 Alexander Penkin. MIT License.

// skeleton.edit_mesh_bones - add / remove / rename / reparent / move bones of a SkeletalMesh's own
// reference skeleton, with its skin weights remapped, and merge the result into its USkeleton.
//
// The skeleton.* bone verbs in SkeletonHandler.cpp edit only the USkeleton asset. This one drives the
// engine's USkeletonModifier (SkeletalMeshModifiers module), the tool behind the Skeletal Mesh
// editor's skeleton-editing mode. It is resolved by reflection because its plugin differs across the
// supported range (MeshModelingToolsetExp on 5.3-5.5, MeshModelingToolset on 5.6+), so no single
// link dependency covers it. Every op runs on the modifier's private copy of the hierarchy; nothing
// reaches the mesh until the whole batch validated, so a refused batch changes nothing.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/AssetUtils.h"
#include "Utils/JsonUtils.h"

#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Modules/ModuleManager.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/Script.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"

namespace PwEditMeshBones
{
    // The modifier class, or null when its module is absent or a UFUNCTION / parameter this verb
    // writes is not there under the name used below (so a signature drift refuses instead of
    // writing into a frame it does not match).
    UClass* ResolveModifierClass()
    {
        if (FModuleManager::Get().ModuleExists(TEXT("SkeletalMeshModifiers")))
        {
            FModuleManager::Get().LoadModule(TEXT("SkeletalMeshModifiers"));
        }
        UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/SkeletalMeshModifiers.SkeletonModifier"));
        if (!Class)
        {
            return nullptr;
        }
        static const TCHAR* const Signatures[][4] = {
            { TEXT("SetSkeletalMesh"), TEXT("InSkeletalMesh") },
            { TEXT("CommitSkeletonToSkeletalMesh") },
            { TEXT("AddBone"), TEXT("InBoneName"), TEXT("InParentName"), TEXT("InTransform") },
            { TEXT("RemoveBone"), TEXT("InBoneName"), TEXT("bRemoveChildren") },
            { TEXT("RenameBone"), TEXT("InOldBoneName"), TEXT("InNewBoneName") },
            { TEXT("ParentBone"), TEXT("InBoneName"), TEXT("InParentName") },
            { TEXT("SetBoneTransform"), TEXT("InBoneName"), TEXT("InNewTransform"), TEXT("bMoveChildren") },
            { TEXT("GetBoneTransform"), TEXT("InBoneName"), TEXT("bGlobal") },
            { TEXT("GetParentName"), TEXT("InBoneName") },
            { TEXT("GetAllBoneNames") },
        };
        for (const auto& Signature : Signatures)
        {
            const UFunction* Fn = Class->FindFunctionByName(Signature[0]);
            if (!Fn || !Fn->GetReturnProperty())
            {
                return nullptr;
            }
            for (int32 Index = 1; Index < 4 && Signature[Index]; ++Index)
            {
                if (!FindFProperty<FProperty>(Fn, Signature[Index]))
                {
                    return nullptr;
                }
            }
        }
        return Class;
    }

    // One reflected call. The frame is the UFunction's own parameter struct, so ProcessEvent reads
    // and writes exactly the layout the engine declared.
    class FCall
    {
    public:
        FCall(UObject* InTarget, const TCHAR* Name)
            : Target(InTarget), Fn(InTarget->FindFunction(Name)), Frame(Fn)
        {
        }

        template <typename T>
        FCall& Arg(const TCHAR* Param, const T& Value)
        {
            FindFProperty<FProperty>(Fn, Param)->SetValue_InContainer(Frame.GetStructMemory(), &Value);
            return *this;
        }

        template <typename T>
        T Run()
        {
            FEditorScriptExecutionGuard ScriptGuard;
            Target->ProcessEvent(Fn, Frame.GetStructMemory());
            T Out{};
            Fn->GetReturnProperty()->GetValue_InContainer(Frame.GetStructMemory(), &Out);
            return Out;
        }

    private:
        UObject* Target;
        UFunction* Fn;
        FStructOnScope Frame;
    };

    TArray<FName> BoneNames(UObject* Modifier)
    {
        return FCall(Modifier, TEXT("GetAllBoneNames")).Run<TArray<FName>>();
    }

    FName ParentOf(UObject* Modifier, FName Bone)
    {
        return FCall(Modifier, TEXT("GetParentName")).Arg(TEXT("InBoneName"), Bone).Run<FName>();
    }

    // Mirrors FReferenceSkeletonCompatibilityChecker::IsCompatibleReferenceSkeleton (private to
    // SkeletonModifier.cpp): when it fails, CommitSkeletonToSkeletalMesh opens a modal merge dialog
    // (USkeletonModifier::PreCommitSkeleton -> SCustomDialog::ShowModal). Every edited bone the
    // Skeleton knows, or else its nearest ancestor the Skeleton knows, must have the same parent
    // chain by name in both, and at least one bone must match. Returns the first bone that fails,
    // or NAME_None when compatible.
    FName FindIncompatibleBone(const FReferenceSkeleton& Skeleton, const TArray<FName>& Names, const TArray<int32>& Parents)
    {
        auto ParentChainMatches = [&](int32 SkeletonIndex)
        {
            if (SkeletonIndex == 0)
            {
                return Skeleton.GetBoneName(0) == Names[0];
            }
            int32 MeshIndex = Names.IndexOfByKey(Skeleton.GetBoneName(SkeletonIndex));
            if (MeshIndex == INDEX_NONE)
            {
                return false;
            }
            while (true)
            {
                const int32 SkeletonParent = Skeleton.GetParentIndex(SkeletonIndex);
                const int32 MeshParent = Parents[MeshIndex];
                if (SkeletonParent == INDEX_NONE || MeshParent == INDEX_NONE)
                {
                    return SkeletonParent == MeshParent;
                }
                if (Skeleton.GetBoneName(SkeletonParent) != Names[MeshParent])
                {
                    return false;
                }
                SkeletonIndex = SkeletonParent;
                MeshIndex = MeshParent;
            }
        };

        bool bAnyMatch = false;
        for (int32 Index = 0; Index < Names.Num(); ++Index)
        {
            int32 SkeletonIndex = Skeleton.FindBoneIndex(Names[Index]);
            bAnyMatch |= SkeletonIndex != INDEX_NONE;
            for (int32 Ancestor = Parents[Index]; SkeletonIndex == INDEX_NONE && Ancestor != INDEX_NONE; Ancestor = Parents[Ancestor])
            {
                SkeletonIndex = Skeleton.FindBoneIndex(Names[Ancestor]);
            }
            if (SkeletonIndex == INDEX_NONE || !ParentChainMatches(SkeletonIndex))
            {
                return Names[Index];
            }
        }
        return (bAnyMatch || Names.IsEmpty()) ? FName() : Names[0];
    }

    // Reference-skeleton indices that some section's vertices are weighted to (FSkelMeshSection::BoneMap).
    TSet<int32> SkinnedBoneIndices(const USkeletalMesh* Mesh)
    {
        TSet<int32> Skinned;
        if (const FSkeletalMeshModel* Model = Mesh->GetImportedModel())
        {
            for (const FSkeletalMeshLODModel& Lod : Model->LODModels)
            {
                for (const FSkelMeshSection& Section : Lod.Sections)
                {
                    for (const FBoneIndexType Bone : Section.BoneMap)
                    {
                        Skinned.Add(Bone);
                    }
                }
            }
        }
        return Skinned;
    }

    TSharedPtr<FJsonValue> BoneEntry(FName Name, FName Parent)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Name.ToString());
        Entry->SetStringField(TEXT("parent"), Parent.IsNone() ? FString() : Parent.ToString());
        return MakeShared<FJsonValueObject>(Entry);
    }
}


// ===========================================================================
// skeleton.edit_mesh_bones - Edit a SkeletalMesh's bones (ref skeleton + skin weights)
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.edit_mesh_bones", "skeleton",
    "Edit the bones of a SkeletalMesh itself - its reference skeleton and skin weights - as one validated batch, then merge the result into the mesh's USkeleton. The skeleton.add_bone family edits only the USkeleton; use this to give an imported mesh a new IK, twist or weapon bone. A batch that would leave the mesh incompatible with its Skeleton is refused SKELETON_EDIT_INCOMPATIBLE before anything changes.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletalMeshPath", "path", "USkeletalMesh to edit. Engine content (/Engine/...) and meshes bound to an engine Skeleton are refused; duplicate both first."),
        RPC_PARAM_REQ_NESTED("ops", "array", "Non-empty ordered array of {op, bone, parent, newName, location, rotation, scale, removeChildren}; each op sees the ones before it. op: add (bone, parent required; location/rotation/scale are the bind pose relative to parent, default identity), remove (bone; removeChildren default false re-parents the children to the removed bone's parent; a removed bone's skin weights move to the root bone), rename (bone, newName), reparent (bone, parent), set_transform (bone; location {x,y,z}, rotation {pitch,yaw,roll}, scale {x,y,z} relative to parent, omitted parts keep their current value; children move with the bone). These eight keys are the whole op schema and any other key inside an op is refused with UNKNOWN_NESTED_PARAMS.",
            TEXT("op"), TEXT("bone"), TEXT("parent"), TEXT("newName"), TEXT("location"), TEXT("rotation"), TEXT("scale"), TEXT("removeChildren")),
        RPC_PARAM_OPT("dryRun", "boolean", "Validate the whole batch, including the Skeleton compatibility check, and report the resulting bones without changing any asset. Default false.")
    ))
{
    using namespace PwEditMeshBones;

    const FString MeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    const TArray<TSharedPtr<FJsonValue>>* Ops = Ctx.GetArray(TEXT("ops"));
    const bool bDryRun = Ctx.GetBool(TEXT("dryRun"), false);
    if (!Ops || Ops->IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("ops must be a non-empty array of bone operations"));
        return true;
    }

    UObject* Asset = MeshPath.IsEmpty() ? nullptr : StaticLoadObject(UObject::StaticClass(), nullptr, *MeshPath);
    if (!Asset)
    {
        Ctx.SendError(ErrorCodes::ERR_SKELETAL_MESH_NOT_FOUND, FString::Printf(TEXT("Skeletal mesh asset not found: %s"), *MeshPath));
        return true;
    }
    USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset);
    if (!Mesh)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_TYPE, FString::Printf(
            TEXT("Asset '%s' is a %s, not a USkeletalMesh. Pass a USkeletalMesh asset path."), *MeshPath, *Asset->GetClass()->GetName()));
        return true;
    }
    USkeleton* Skeleton = Mesh->GetSkeleton();
    if (!Skeleton)
    {
        Ctx.SendError(ErrorCodes::ERR_SKELETON_NOT_FOUND, FString::Printf(
            TEXT("Skeletal mesh '%s' has no USkeleton; give it one with skeleton.create_skeleton fromSkeletalMesh first."), *Mesh->GetPathName()));
        return true;
    }
    if (Mesh->GetPathName().StartsWith(TEXT("/Engine/")) || Skeleton->GetPathName().StartsWith(TEXT("/Engine/")))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
            TEXT("'%s' or its Skeleton '%s' is engine content, which this verb does not edit. Duplicate the mesh and its Skeleton under /Game and bind the copy to the copied Skeleton first."),
            *Mesh->GetPathName(), *Skeleton->GetPathName()));
        return true;
    }

    UClass* ModifierClass = ResolveModifierClass();
    if (!ModifierClass)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_SUPPORTED,
            TEXT("USkeletonModifier (module SkeletalMeshModifiers, from the Mesh Modeling Toolset plugin) is not available in this editor, or its API differs from the one this verb drives. Enable the Modeling Tools Editor Mode plugin and restart the editor."));
        return true;
    }
    TStrongObjectPtr<UObject> Modifier(NewObject<UObject>(GetTransientPackage(), ModifierClass));
    if (!FCall(Modifier.Get(), TEXT("SetSkeletalMesh")).Arg(TEXT("InSkeletalMesh"), Mesh).Run<bool>())
    {
        Ctx.SendError(ErrorCodes::ERR_OPERATION_FAILED, FString::Printf(
            TEXT("USkeletonModifier could not load '%s'; it needs the mesh's LOD 0 source mesh description."), *Mesh->GetPathName()));
        return true;
    }

    // Removed bones are tracked by their name on the unedited mesh, so a bone renamed and then
    // removed in one batch is still checked against the mesh's skin weights.
    const FReferenceSkeleton& MeshRef = Mesh->GetRefSkeleton();
    TArray<FName> OriginalNames;
    TMap<FName, FName> OriginalNameOf;
    for (int32 Index = 0; Index < MeshRef.GetRawBoneNum(); ++Index)
    {
        OriginalNames.Add(MeshRef.GetBoneName(Index));
        OriginalNameOf.Add(MeshRef.GetBoneName(Index), MeshRef.GetBoneName(Index));
    }
    TArray<FName> RemovedOriginals;

    for (int32 OpIndex = 0; OpIndex < Ops->Num(); ++OpIndex)
    {
        const TSharedPtr<FJsonObject>* OpObject = nullptr;
        if (!(*Ops)[OpIndex].IsValid() || !(*Ops)[OpIndex]->TryGetObject(OpObject))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(TEXT("ops[%d] is not an object. Nothing was changed."), OpIndex));
            return true;
        }
        const TSharedPtr<FJsonObject>& Op = *OpObject;
        FString Kind, BoneString, ParentString, NewNameString;
        Op->TryGetStringField(TEXT("op"), Kind);
        Op->TryGetStringField(TEXT("bone"), BoneString);
        Op->TryGetStringField(TEXT("parent"), ParentString);
        Op->TryGetStringField(TEXT("newName"), NewNameString);
        const FName Bone(*BoneString);
        const FName Parent(*ParentString);
        const FName NewName(*NewNameString);

        auto Refuse = [&](const TCHAR* Code, const FString& Why)
        {
            Ctx.SendError(Code, FString::Printf(TEXT("ops[%d] (%s '%s'): %s. Nothing was changed."), OpIndex, *Kind, *BoneString, *Why));
            return true;
        };

        const TArray<FName> Names = BoneNames(Modifier.Get());
        if (!TArray<FString>({ TEXT("add"), TEXT("remove"), TEXT("rename"), TEXT("reparent"), TEXT("set_transform") }).Contains(Kind))
        {
            return Refuse(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("op must be one of add, remove, rename, reparent, set_transform"));
        }
        if (BoneString.IsEmpty())
        {
            return Refuse(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("bone is required"));
        }
        const bool bIsAdd = Kind == TEXT("add");
        if (!bIsAdd && !Names.Contains(Bone))
        {
            return Refuse(ErrorCodes::ERR_BONE_NOT_FOUND, FString::Printf(TEXT("the mesh has no bone '%s' at this point of the batch"), *BoneString));
        }
        auto CheckParent = [&]() -> bool
        {
            if (ParentString.IsEmpty())
            {
                return Refuse(ErrorCodes::ERR_PARENT_REQUIRED, TEXT("parent is required"));
            }
            if (!Names.Contains(Parent))
            {
                return Refuse(ErrorCodes::ERR_PARENT_NOT_FOUND, FString::Printf(TEXT("the mesh has no bone '%s' to parent under"), *ParentString));
            }
            return false;
        };

        bool bApplied = false;
        if (bIsAdd)
        {
            if (Names.Contains(Bone))
            {
                return Refuse(ErrorCodes::ERR_BONE_EXISTS, TEXT("a bone with this name already exists"));
            }
            if (CheckParent())
            {
                return true;
            }
            const FTransform Transform(ParseRotatorFromJson(Op, TEXT("rotation")), ParseVectorFromJson(Op, TEXT("location")),
                ParseVectorFromJson(Op, TEXT("scale"), FVector::OneVector));
            bApplied = FCall(Modifier.Get(), TEXT("AddBone")).Arg(TEXT("InBoneName"), Bone).Arg(TEXT("InParentName"), Parent)
                .Arg(TEXT("InTransform"), Transform).Run<bool>();
        }
        else if (Kind == TEXT("remove"))
        {
            if (Bone == Names[0])
            {
                return Refuse(ErrorCodes::ERR_CANNOT_REMOVE_ROOT, TEXT("the root bone cannot be removed"));
            }
            bool bRemoveChildren = false;
            Op->TryGetBoolField(TEXT("removeChildren"), bRemoveChildren);
            bApplied = FCall(Modifier.Get(), TEXT("RemoveBone")).Arg(TEXT("InBoneName"), Bone).Arg(TEXT("bRemoveChildren"), bRemoveChildren).Run<bool>();
            const TArray<FName> After = BoneNames(Modifier.Get());
            for (const FName& Name : Names)
            {
                const FName Original = After.Contains(Name) ? FName() : OriginalNameOf.FindRef(Name);
                if (!Original.IsNone())
                {
                    RemovedOriginals.Add(Original);
                }
            }
        }
        else if (Kind == TEXT("rename"))
        {
            if (NewNameString.IsEmpty())
            {
                return Refuse(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("newName is required"));
            }
            if (Names.Contains(NewName))
            {
                return Refuse(ErrorCodes::ERR_BONE_EXISTS, FString::Printf(TEXT("a bone named '%s' already exists"), *NewNameString));
            }
            bApplied = FCall(Modifier.Get(), TEXT("RenameBone")).Arg(TEXT("InOldBoneName"), Bone).Arg(TEXT("InNewBoneName"), NewName).Run<bool>();
            FName Original;
            if (bApplied && OriginalNameOf.RemoveAndCopyValue(Bone, Original))
            {
                OriginalNameOf.Add(NewName, Original);
            }
        }
        else if (Kind == TEXT("reparent"))
        {
            if (Bone == Names[0])
            {
                return Refuse(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("the root bone cannot be reparented"));
            }
            if (CheckParent())
            {
                return true;
            }
            for (FName Up = Parent; !Up.IsNone(); Up = ParentOf(Modifier.Get(), Up))
            {
                if (Up == Bone)
                {
                    return Refuse(ErrorCodes::ERR_PARENT_CYCLE, FString::Printf(TEXT("'%s' is the bone itself or one of its descendants"), *ParentString));
                }
            }
            bApplied = FCall(Modifier.Get(), TEXT("ParentBone")).Arg(TEXT("InBoneName"), Bone).Arg(TEXT("InParentName"), Parent).Run<bool>();
        }
        else // set_transform
        {
            const bool bGlobal = false;
            const FTransform Current = FCall(Modifier.Get(), TEXT("GetBoneTransform")).Arg(TEXT("InBoneName"), Bone)
                .Arg(TEXT("bGlobal"), bGlobal).Run<FTransform>();
            const FTransform Transform(ParseRotatorFromJson(Op, TEXT("rotation"), Current.Rotator()),
                ParseVectorFromJson(Op, TEXT("location"), Current.GetLocation()), ParseVectorFromJson(Op, TEXT("scale"), Current.GetScale3D()));
            const bool bMoveChildren = true;
            bApplied = FCall(Modifier.Get(), TEXT("SetBoneTransform")).Arg(TEXT("InBoneName"), Bone).Arg(TEXT("InNewTransform"), Transform)
                .Arg(TEXT("bMoveChildren"), bMoveChildren).Run<bool>();
        }
        if (!bApplied)
        {
            return Refuse(ErrorCodes::ERR_OPERATION_FAILED, TEXT("USkeletonModifier refused the operation (see the LogAnimation output)"));
        }
    }

    const TArray<FName> NewNames = BoneNames(Modifier.Get());
    TArray<int32> NewParents;
    for (const FName& Name : NewNames)
    {
        NewParents.Add(NewNames.IndexOfByKey(ParentOf(Modifier.Get(), Name)));
    }

    const FName Incompatible = FindIncompatibleBone(Skeleton->GetReferenceSkeleton(), NewNames, NewParents);
    if (!Incompatible.IsNone())
    {
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("bone"), Incompatible.ToString());
        Details->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
        Ctx.SendError(ErrorCodes::ERR_SKELETON_EDIT_INCOMPATIBLE, FString::Printf(
            TEXT("After this batch, bone '%s' of '%s' would no longer match its Skeleton '%s' (a different parent chain by name). The engine commit would stop on a modal merge dialog, so the batch was refused and nothing was changed. Make the same hierarchy change on the Skeleton first (skeleton.add_bone / skeleton.set_bone_parent / skeleton.remove_bone), then repeat this batch."),
            *Incompatible.ToString(), *Mesh->GetPathName(), *Skeleton->GetPathName()), Details);
        return true;
    }

    // ponytail: USkeletonModifier rewrites only LOD 0's mesh description, so on a multi-LOD mesh
    // only batches that keep every existing bone index are safe; remap LODs 1..N here if needed.
    bool bIndicesKept = NewNames.Num() >= OriginalNames.Num();
    for (int32 Index = 0; bIndicesKept && Index < OriginalNames.Num(); ++Index)
    {
        bIndicesKept = OriginalNameOf.FindRef(NewNames[Index]) == OriginalNames[Index];
    }
    if (Mesh->GetLODNum() > 1 && !bIndicesKept)
    {
        Ctx.SendError(ErrorCodes::ERR_OPERATION_NOT_SUPPORTED, FString::Printf(
            TEXT("'%s' has %d LODs and this batch moves existing bone indices (remove or reparent). The engine's skeleton modifier remaps skin weights on LOD 0 only, which would leave LODs 1+ weighted to the wrong bones, so the batch was refused and nothing was changed. Adds, renames and transform edits are accepted on multi-LOD meshes."),
            *Mesh->GetPathName(), Mesh->GetLODNum()));
        return true;
    }

    const TSet<int32> Skinned = SkinnedBoneIndices(Mesh);
    TArray<TSharedPtr<FJsonValue>> SkinnedRemoved;
    for (const FName& Original : RemovedOriginals)
    {
        if (Skinned.Contains(OriginalNames.IndexOfByKey(Original)))
        {
            SkinnedRemoved.Add(MakeShared<FJsonValueString>(Original.ToString()));
        }
    }

    bool bCommitted = false;
    if (!bDryRun)
    {
        FScopedTransaction Transaction(NSLOCTEXT("PinWright", "EditMeshBones", "Edit Skeletal Mesh Bones"));
        bCommitted = FCall(Modifier.Get(), TEXT("CommitSkeletonToSkeletalMesh")).Run<bool>();
        if (!bCommitted)
        {
            Transaction.Cancel();
            Ctx.SendError(ErrorCodes::ERR_OPERATION_FAILED, FString::Printf(
                TEXT("USkeletonModifier::CommitSkeletonToSkeletalMesh refused '%s' (it does so when the batch changes nothing; see the LogAnimation output)."), *Mesh->GetPathName()));
            return true;
        }
        McpSafeAssetSave(Mesh);
    }

    TArray<TSharedPtr<FJsonValue>> Bones;
    if (bCommitted)
    {
        const FReferenceSkeleton& Edited = Mesh->GetRefSkeleton();
        for (int32 Index = 0; Index < Edited.GetRawBoneNum(); ++Index)
        {
            const int32 ParentIndex = Edited.GetRawParentIndex(Index);
            Bones.Add(BoneEntry(Edited.GetBoneName(Index), ParentIndex == INDEX_NONE ? FName() : Edited.GetBoneName(ParentIndex)));
        }
    }
    else
    {
        for (int32 Index = 0; Index < NewNames.Num(); ++Index)
        {
            Bones.Add(BoneEntry(NewNames[Index], NewParents[Index] == INDEX_NONE ? FName() : NewNames[NewParents[Index]]));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("skeletalMeshPath"), Mesh->GetPathName());
    Result->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("committed"), bCommitted);
    Result->SetNumberField(TEXT("opCount"), Ops->Num());
    Result->SetNumberField(TEXT("boneCount"), Bones.Num());
    Result->SetArrayField(TEXT("bones"), Bones);
    Result->SetArrayField(TEXT("skinnedBonesRemoved"), SkinnedRemoved);
    if (bCommitted)
    {
        Result->SetNumberField(TEXT("skeletonBoneCount"), Skeleton->GetReferenceSkeleton().GetRawBoneNum());
        Result->SetBoolField(TEXT("skeletonCompatible"), Skeleton->IsCompatibleMesh(Mesh));
        AddMarkDirtySaveReport(Result, Mesh, /*bSaveRequested=*/true);
    }
    Ctx.SendSuccess(Result);
    return true;
}
