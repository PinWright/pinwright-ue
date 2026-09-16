// Copyright (c) 2026 Alexander Penkin. MIT License.

// PhysicsAssetHandler.cpp - Migrated from PinWright_SkeletonHandlers.cpp
// Physics asset operations: create, bodies, constraints, configuration
//
// Phase 14 migration to auto-registration system.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"
#include "AnimationAuthoringHelpers.h"
#include "PinWrightHelpers.h"
#include "PinWrightGlobals.h"
#include "PinWrightSubsystem.h"
#include "Utils/JsonUtils.h"


#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "AnimationRuntime.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/BodySetup.h"
#if __has_include("PhysicsEngine/SkeletalBodySetup.h")
#include "PhysicsEngine/SkeletalBodySetup.h"
#endif
// USkeletalBodySetup is declared in PhysicsEngine/PhysicsAsset.h on UE versions that lack the above header
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace {

static FString DescribeAssetTypePhys(const UObject* Asset)
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
    if (Asset->IsA(UPhysicsAsset::StaticClass()))
    {
        return TEXT("UPhysicsAsset");
    }
    return Asset->GetClass()->GetName();
}

// Helper: Load skeletal mesh asset from path
static USkeletalMesh* LoadSkeletalMeshFromPathPhys(const FString& MeshPath, FString& OutError, bool* bOutWrongType = nullptr)
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
        OutError = FString::Printf(TEXT("Asset '%s' is a %s, not a USkeletalMesh. Pass a USkeletalMesh asset path."), *MeshPath, *DescribeAssetTypePhys(Asset));
        return nullptr;
    }

    return Mesh;
}

// Helper: Build a default ragdoll physics asset directly from a skeleton's
// reference-pose bones (no SkeletalMesh / render geometry required). Adds one
// capsule body per bone, sized from the bone-to-children span in component
// space — the mesh-free analogue of UPhysicsAssetFactory's "auto-generate
// capsule bodies from bone reference poses". Returns true on success, false on
// failure with OutError set; the caller reads the actual body count from
// PhysicsAsset->SkeletalBodySetups.Num(). MinBoneLength filters out tiny/leaf
// joints so the asset isn't cluttered with degenerate bodies; a leaf bone with
// no child span gets a default-sized capsule so the chain stays continuous.
static bool BuildBodiesFromSkeletonRefPose(UPhysicsAsset* PhysicsAsset, USkeleton* Skeleton,
    float MinBoneLength, FString& OutError)
{
    OutError.Reset();
    if (!PhysicsAsset || !Skeleton)
    {
        OutError = TEXT("Null physics asset or skeleton");
        return false;
    }

    const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
    const int32 NumBones = RefSkeleton.GetRawBoneNum();
    if (NumBones <= 0)
    {
        OutError = TEXT("Skeleton has no bones");
        return false;
    }

    const TArray<FTransform>& LocalPose = RefSkeleton.GetRawRefBonePose();

    // Accumulate component-space transforms via the engine helper (handles the
    // parent-ordering assumption and index guards).
    TArray<FTransform> ComponentSpace;
    FAnimationRuntime::FillUpComponentSpaceTransforms(RefSkeleton, LocalPose, ComponentSpace);

    // Single O(N) pass: each child contributes its distance-to-parent to the
    // parent's max-child span (parents always precede children in a reference
    // skeleton). This replaces a per-bone rescan of all bones.
    TArray<float> MaxChildDist;
    MaxChildDist.Init(0.0f, NumBones);
    for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
    {
        const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
        if (ComponentSpace.IsValidIndex(ParentIndex) && ComponentSpace.IsValidIndex(BoneIndex))
        {
            const float Dist = static_cast<float>(FVector::Dist(
                ComponentSpace[ParentIndex].GetLocation(),
                ComponentSpace[BoneIndex].GetLocation()));
            MaxChildDist[ParentIndex] = FMath::Max(MaxChildDist[ParentIndex], Dist);
        }
    }

    // Default capsule dimensions for leaf bones with no child span to measure.
    const float DefaultLength = FMath::Max(MinBoneLength, 5.0f);

    int32 BodiesCreated = 0;
    for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
    {
        const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);

        // Bone length = farthest child distance in component space.
        float BoneLength = MaxChildDist[BoneIndex];

        // Leaf bone (no children): give it a small default capsule so the chain
        // stays continuous. Non-leaf bones shorter than the threshold are skipped.
        if (BoneLength <= KINDA_SMALL_NUMBER)
        {
            BoneLength = DefaultLength;
        }
        else if (BoneLength < MinBoneLength)
        {
            continue;
        }

        if (PhysicsAsset->FindBodyIndex(BoneName) != INDEX_NONE)
        {
            continue;
        }

        USkeletalBodySetup* BodySetup = NewObject<USkeletalBodySetup>(PhysicsAsset, NAME_None, RF_Transactional);
        if (!BodySetup)
        {
            continue;
        }
        BodySetup->BoneName = BoneName;

        FKSphylElem CapsuleElem;
        CapsuleElem.Radius = FMath::Max(BoneLength * 0.25f, 1.0f);
        CapsuleElem.Length = BoneLength;
        // Capsule sits at the bone origin in bone-local space; callers refine
        // placement afterwards via skeleton.configure_physics_body.
        CapsuleElem.Center = FVector(0.0f, 0.0f, 0.0f);
        CapsuleElem.Rotation = FRotator::ZeroRotator;
        BodySetup->AggGeom.SphylElems.Add(CapsuleElem);

        PhysicsAsset->SkeletalBodySetups.Add(BodySetup);
        ++BodiesCreated;
    }

    PhysicsAsset->UpdateBodySetupIndexMap();
    PhysicsAsset->UpdateBoundsBodiesArray();

    if (BodiesCreated == 0)
    {
        OutError = TEXT("No physics bodies could be generated from the skeleton's bones");
        return false;
    }

    return true;
}

// Helper: Load physics asset from path
static UPhysicsAsset* LoadPhysicsAssetFromPath(const FString& PhysicsPath, FString& OutError, bool* bOutWrongType = nullptr)
{
    OutError.Reset();
    if (bOutWrongType)
    {
        *bOutWrongType = false;
    }
    if (PhysicsPath.IsEmpty())
    {
        OutError = TEXT("Physics asset path is required");
        return nullptr;
    }

    UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *PhysicsPath);
    if (!Asset)
    {
        OutError = FString::Printf(TEXT("Physics asset not found: %s"), *PhysicsPath);
        return nullptr;
    }

    UPhysicsAsset* PhysAsset = Cast<UPhysicsAsset>(Asset);
    if (!PhysAsset)
    {
        if (bOutWrongType)
        {
            *bOutWrongType = true;
        }
        OutError = FString::Printf(TEXT("Asset '%s' is a %s, not a UPhysicsAsset. Pass a physicsAssetPath."), *PhysicsPath, *DescribeAssetTypePhys(Asset));
        return nullptr;
    }

    return PhysAsset;
}

static void SendMeshPathErrorPhys(FHandlerContext& Ctx, const TCHAR* MissingCode, const FString& Error, bool bWrongType)
{
    Ctx.SendError(bWrongType ? TEXT("INVALID_ASSET_TYPE") : MissingCode, Error);
}

static void SendPhysicsPathErrorPhys(FHandlerContext& Ctx, const TCHAR* MissingCode, const FString& Error, bool bWrongType)
{
    Ctx.SendError(bWrongType ? TEXT("INVALID_ASSET_TYPE") : MissingCode, Error);
}

// Accepted wire keys for skeleton.create_physics_asset's required source-path
// slot, canonical first. `skeletalMeshPath` is the documented canonical;
// `skeletonPath` is accepted as an alias for the meshless-USkeleton workflow.
// Single source of truth used by BOTH the FParamSpec alias registration and the
// body-side GetStringFirstOf read, mirroring the per-family *ParamUtils Keys()
// helpers (GeometryNameParamUtils, LevelNameParamUtils, ...) so the schema and
// the handler can never drift out of sync.
static const TArray<FString>& SkeletonSourcePathKeys()
{
    static const TArray<FString> Keys = { TEXT("skeletalMeshPath"), TEXT("skeletonPath") };
    return Keys;
}

} // anonymous namespace


// ===========================================================================
// skeleton.create_physics_asset - Create a new physics asset for a skeletal mesh
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.create_physics_asset", "skeleton",
    "Create a new UPhysicsAsset (collision capsules + joint constraints) for a SkeletalMesh OR a bare authored USkeleton, suitable for ragdoll / cloth interaction. Given a SkeletalMesh it auto-generates capsule bodies from the mesh; given a meshless USkeleton (e.g. one built via skeleton.create_skeleton + add_bone) it generates one capsule body per bone directly from the skeleton's reference-pose bone span — no SkeletalMesh required. Refine afterwards via skeleton.add_physics_body / configure_physics_body / add_physics_constraint.",
    RPC_PARAMS(
        // skeletonPath is a schema-registered alias of the required skeletalMeshPath
        // slot (not merely the body-side GetStringFirstOf resolution below) so a
        // skeletonPath-only payload — which the summary above advertises as accepted —
        // passes the dispatcher's ValidateHandlerParams required-param check instead of
        // hard-failing MISSING_REQUIRED_PARAM before the handler body ever runs.
        ParamAliasUtils::MakeAliasParamSpec(TEXT("skeletalMeshPath"), TEXT("path"),
            TEXT("Asset path to a SkeletalMesh, OR a bare USkeleton path (the 'skeletonPath' alias is also accepted). A SkeletalMesh path uses the mesh; a USkeleton path builds bodies from the skeleton's reference-pose bones."),
            /*bRequired=*/true, SkeletonSourcePathKeys()),
        RPC_PARAM_OPT("outputPath", "path", "Asset path for the new PhysicsAsset; defaults to '<SourcePath>_PhysicsAsset' beside the source mesh/skeleton."),
        RPC_PARAM_OPT("minBoneLength", "number", "Skeleton-only: bones whose reference-pose span is shorter than this (cm) are skipped when generating bodies. Default 5."),
        RPC_PARAM_DEF("save", "boolean", "Write the new PhysicsAsset and any assigned SkeletalMesh to disk. false marks packages dirty only; the response reports the measured save state.", "true")
    ))
{
    FString SkeletalMeshPath = Ctx.GetStringFirstOf(SkeletonSourcePathKeys());
    FString OutputPath = Ctx.GetString(TEXT("outputPath"));
    const bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (SkeletalMeshPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletalMeshPath (or skeletonPath) is required"));
        return true;
    }

    // A caller-supplied outputPath is composed and CHECKED HERE, ahead of the source-asset
    // resolution below, for two reasons.
    // (1) The composed path is what kills the process: it used to flow into CreatePackage with
    //     no validation of any kind, and CreatePackage logs at Fatal - a verbosity not compiled
    //     out in any configuration, so it ends the editor and every unsaved package in it - on a
    //     name containing "//" or one that resolves to empty (UObjectGlobals.cpp:1086-1120).
    //     outputPath: "//Game/X" survives the GetPath()/GetBaseFilename() split and rejoin below
    //     as "//Game/X"; outputPath: "/" splits to two empty halves and composes to "". Both were
    //     one-argument editor kills (board B-createpackage-unvalidated-paths-plugin-wide).
    // (2) The ordering is load-bearing for the regression test: it drives a malformed outputPath
    //     together with a skeletalMeshPath that resolves to nothing, so on a build where this
    //     check is absent or moved below the source-asset resolution the call is refused for the
    //     WRONG reason and the test goes red - instead of reaching CreatePackage and taking the
    //     test host down with it. Do not move this below the source-asset resolution.
    // bIncludeReadOnlyRoots=true so a read-only mount is not refused here for a reason unrelated
    // to the hazard; an unwritable destination still fails later, without ending the process.
    FString FullPackagePath;
    if (!OutputPath.IsEmpty())
    {
        FullPackagePath = FPaths::GetPath(OutputPath) / FPaths::GetBaseFilename(OutputPath);
        FText OutputPathReason;
        if (!FPackageName::IsValidLongPackageName(FullPackagePath,
                /*bIncludeReadOnlyRoots=*/true, &OutputPathReason))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                FString::Printf(
                    TEXT("outputPath '%s' does not compose a valid package path ('%s'): %s"),
                    *OutputPath, *FullPackagePath, *OutputPathReason.ToString()));
            return true;
        }
    }

    FString Error;
    bool bWrongType = false;
    USkeletalMesh* SkeletalMesh = LoadSkeletalMeshFromPathPhys(SkeletalMeshPath, Error, &bWrongType);

    // Fallback: if the path is not a SkeletalMesh, it may be a bare authored
    // USkeleton. Build the physics asset directly from its reference-pose bones
    // so the "author a skeleton, then create a physics asset for it" workflow is
    // reachable end-to-end without an imported mesh.
    USkeleton* BareSkeleton = nullptr;
    if (!SkeletalMesh)
    {
        // Fall back to the shared, path-normalizing animation loader (accepts
        // /Content-prefixed and backslash paths the raw StaticLoadObject does not).
        BareSkeleton = AnimationAuthoringHelpers::LoadSkeletonFromPathAnim(SkeletalMeshPath);
        if (!BareSkeleton)
        {
            if (bWrongType)
            {
                Ctx.SendError(TEXT("INVALID_ASSET_TYPE"), Error);
            }
            else
            {
                Ctx.SendError(TEXT("MESH_NOT_FOUND"),
                    FString::Printf(TEXT("No SkeletalMesh or USkeleton found at path: %s"), *SkeletalMeshPath));
            }
            return true;
        }
    }

    if (OutputPath.IsEmpty())
    {
        OutputPath = FPaths::GetPath(SkeletalMeshPath);
        FString MeshName = FPaths::GetBaseFilename(SkeletalMeshPath);
        OutputPath = FString::Printf(TEXT("%s/%s_PhysicsAsset"), *OutputPath, *MeshName);

        // The DERIVED default is composed from skeletalMeshPath, which is caller text too, so it
        // gets the same check rather than being trusted for having a source asset behind it. It
        // cannot be hoisted with the branch above: on a malformed skeletalMeshPath the caller is
        // better served by the source-asset error the resolution above already produced.
        FullPackagePath = FPaths::GetPath(OutputPath) / FPaths::GetBaseFilename(OutputPath);
        FText DerivedPathReason;
        if (!FPackageName::IsValidLongPackageName(FullPackagePath,
                /*bIncludeReadOnlyRoots=*/true, &DerivedPathReason))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                FString::Printf(
                    TEXT("the default output path derived from '%s' is not a valid package path "
                         "('%s'): %s. Pass outputPath explicitly."),
                    *SkeletalMeshPath, *FullPackagePath, *DerivedPathReason.ToString()));
            return true;
        }
    }

    // Package creation is identical for both source types — do it once. FullPackagePath was
    // composed and validated above, on whichever of the two branches produced it.
    FString AssetName = FPaths::GetBaseFilename(OutputPath);

    UPackage* Package = CreatePackage(*FullPackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package"));
        return true;
    }

    // === Bare-skeleton path: build bodies from reference-pose bones. ===
    if (BareSkeleton)
    {
        UPhysicsAsset* PhysicsAsset = NewObject<UPhysicsAsset>(Package, *AssetName,
            RF_Public | RF_Standalone | RF_Transactional);
        if (!PhysicsAsset)
        {
            Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create physics asset object"));
            return true;
        }

        const float MinBoneLength = static_cast<float>(Ctx.GetNumber(TEXT("minBoneLength"), 5.0));
        FString BuildError;
        if (!BuildBodiesFromSkeletonRefPose(PhysicsAsset, BareSkeleton, MinBoneLength, BuildError))
        {
            Ctx.SendError(TEXT("CREATE_FAILED"), BuildError);
            return true;
        }

        McpSafeAssetSave(PhysicsAsset);

        FString PackageName = PhysicsAsset->GetOutermost()
            ? PhysicsAsset->GetOutermost()->GetName() : FString();
        int64 SizeBytes = 0;
        EAssetSaveState SaveState = EAssetSaveState::NotRequested;
        bool bSavedToDisk = false;
        if (bSave)
        {
            bSavedToDisk = SaveAssetToDiskReportingPresence(
                PhysicsAsset, /*bForce=*/true, &PackageName, &SizeBytes, &SaveState);
        }

        TSharedPtr<FJsonObject> SkelResult = MakeShareable(new FJsonObject());
        SkelResult->SetStringField(TEXT("physicsAssetPath"), PhysicsAsset->GetPathName());
        SkelResult->SetStringField(TEXT("skeletonPath"), BareSkeleton->GetPathName());
        SkelResult->SetStringField(TEXT("package"), PackageName);
        SkelResult->SetNumberField(TEXT("bodyCount"), PhysicsAsset->SkeletalBodySetups.Num());
        SkelResult->SetNumberField(TEXT("constraintCount"), PhysicsAsset->ConstraintSetup.Num());
        SkelResult->SetBoolField(TEXT("fromSkeleton"), true);
        SkelResult->SetBoolField(TEXT("savedToDisk"), bSavedToDisk);
        SkelResult->SetBoolField(TEXT("pendingFlush"), bSave && !bSavedToDisk);
        AddAssetSaveSizeReport(SkelResult, SizeBytes, bSavedToDisk);
        AddAssetSaveReport(SkelResult, bSave, bSavedToDisk, SaveState);

        Ctx.SendSuccess(SkelResult);
        return true;
    }

    // Mesh-backed path: create the physics asset headlessly via the shared helper, which
    // bypasses UPhysicsAssetFactory's interactive "New Physics Asset" body-generation modal
    // that would wedge the game thread in this non-unattended MCP editor
    // (B-physics-asset-factory-modal-hang). See McpCreatePhysicsAssetFromSkeletalMeshHeadless.
    const FPhysicsAssetCreateResult Created = McpCreatePhysicsAssetFromSkeletalMeshHeadless(
        Package, *AssetName, SkeletalMesh, bSave);
    if (!Created.bSuccess || !Created.Asset)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), Created.ErrorMessage);
        return true;
    }
    UPhysicsAsset* PhysicsAsset = Created.Asset;

    SkeletalMesh->SetPhysicsAsset(PhysicsAsset);
    McpSafeAssetSave(SkeletalMesh);

    FString MeshPackageName = SkeletalMesh->GetOutermost()
        ? SkeletalMesh->GetOutermost()->GetName() : FString();
    int64 MeshSizeBytes = 0;
    EAssetSaveState MeshSaveState = EAssetSaveState::NotRequested;
    bool bMeshSavedToDisk = false;
    if (bSave)
    {
        bMeshSavedToDisk = SaveAssetToDiskReportingPresence(
            SkeletalMesh, /*bForce=*/true, &MeshPackageName, &MeshSizeBytes, &MeshSaveState);
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("physicsAssetPath"), PhysicsAsset->GetPathName());
    Result->SetStringField(TEXT("skeletalMeshPath"), SkeletalMesh->GetPathName());
    Result->SetStringField(TEXT("package"), Created.PackageName);
    Result->SetNumberField(TEXT("bodyCount"), PhysicsAsset->SkeletalBodySetups.Num());
    Result->SetNumberField(TEXT("constraintCount"), PhysicsAsset->ConstraintSetup.Num());
    Result->SetBoolField(TEXT("savedToDisk"), Created.bSavedToDisk);
    Result->SetBoolField(TEXT("pendingFlush"), Created.bPendingFlush);
    AddAssetSaveSizeReport(Result, Created.SizeBytes, Created.bSavedToDisk);
    AddAssetSaveReport(Result, bSave, Created.bSavedToDisk, Created.SaveState);

    TSharedPtr<FJsonObject> MeshSave = MakeShared<FJsonObject>();
    MeshSave->SetStringField(TEXT("assetPath"), SkeletalMesh->GetPathName());
    MeshSave->SetStringField(TEXT("package"), MeshPackageName);
    MeshSave->SetBoolField(TEXT("savedToDisk"), bMeshSavedToDisk);
    MeshSave->SetBoolField(TEXT("pendingFlush"), bSave && !bMeshSavedToDisk);
    AddAssetSaveSizeReport(MeshSave, MeshSizeBytes, bMeshSavedToDisk);
    AddAssetSaveReport(MeshSave, bSave, bMeshSavedToDisk, MeshSaveState);
    Result->SetObjectField(TEXT("skeletalMeshSave"), MeshSave);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.list_physics_bodies - List all physics bodies in a physics asset
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.list_physics_bodies", "skeleton",
    "Enumerate physics bodies in a UPhysicsAsset, returning per-body bone name, primitive count (capsules / spheres / boxes), and physics type (Default / Kinematic / Simulated). Use to audit a generated ragdoll before refining it. Any normal ragdoll (~30+ bodies) exceeds the inline display budget and spills to a file; narrow it inline with boneName= (substring), limit= (max rows, totalCount keeps reporting the untruncated total), or namesOnly=true / fields=[...] to drop the per-body primitive counts.",
    RPC_PARAMS(
        RPC_PARAM_OPT("physicsAssetPath", "path", "Path to the physics asset"),
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Path to skeletal mesh (to auto-find physics asset)"),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("boneName"), TEXT("string"),
            TEXT("Substring filter (case-insensitive) applied to each body's bone name; omit to return all bodies. Snake_case bone_name / nameFilter / name_filter accepted."),
            /*bRequired=*/false, TArray<FString>({TEXT("boneName"), TEXT("bone_name"),
                TEXT("nameFilter"), TEXT("name_filter")})),
        RPC_PARAM_DEF("limit", "number", "Max bodies to return after filtering. 0 (default) = all. totalCount always reports the full untruncated match count so elision is detectable.", "0"),
        RPC_PARAM_OPT_ALIAS("fields", "array|string", "Case-insensitive allow-list of per-body keys to return (valid keys: boneName, considerForBounds, collisionType, sphereCount, boxCount, capsuleCount, convexCount); e.g. [\"boneName\"] to drop the primitive counts. Omit for all keys. A single string is also accepted.", "field"),
        RPC_PARAM_OPT_ALIAS("namesOnly", "bool", "When true, returns only boneName per body (drops considerForBounds/collisionType and the four primitive-count fields — the bulk of the bytes) — shorthand for the common 'which bones already have bodies?' read. Snake_case names_only accepted. Ignored when fields is supplied.", "names_only")
    ))
{
    FString PhysicsAssetPath = Ctx.GetString(TEXT("physicsAssetPath"));
    if (PhysicsAssetPath.IsEmpty())
    {
        FString MeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
        if (!MeshPath.IsEmpty())
        {
            FString Error;
            bool bWrongType = false;
            USkeletalMesh* Mesh = LoadSkeletalMeshFromPathPhys(MeshPath, Error, &bWrongType);
            if (!Mesh)
            {
                SendMeshPathErrorPhys(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
                return true;
            }
            if (Mesh->GetPhysicsAsset())
            {
                PhysicsAssetPath = Mesh->GetPhysicsAsset()->GetPathName();
            }
            else
            {
                Ctx.SendError(TEXT("PHYSICS_ASSET_NOT_FOUND"),
                    FString::Printf(TEXT("Skeletal mesh '%s' has no physics asset. Pass physicsAssetPath or assign a physics asset first."), *MeshPath));
                return true;
            }
        }
    }

    if (PhysicsAssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("physicsAssetPath or skeletalMeshPath is required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    UPhysicsAsset* PhysicsAsset = LoadPhysicsAssetFromPath(PhysicsAssetPath, Error, &bWrongType);
    if (!PhysicsAsset)
    {
        SendPhysicsPathErrorPhys(Ctx, TEXT("PHYSICS_ASSET_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    // Substring filter on the bone name (case-insensitive). Accepts the snake_case
    // and nameFilter aliases so a caller need not guess the exact key.
    const FString BoneFilter = Ctx.GetStringFirstOf(
        {TEXT("boneName"), TEXT("bone_name"), TEXT("nameFilter"), TEXT("name_filter")});
    const bool bFiltering = !BoneFilter.IsEmpty();

    // Per-body field projection: an explicit fields allow-list (array or bare
    // string) wins; otherwise namesOnly collapses to boneName only. An empty set
    // means "no projection": emit every key, so unprojected output is byte-identical
    // to the prior shape. The probe keys are lowercase to match the lowercased set
    // ReadFieldProjection returns.
    const TSet<FString> Fields = Ctx.ReadFieldProjection({TEXT("bonename")});
    const bool bProject = Fields.Num() > 0;
    const auto Wants = [&Fields, bProject](const TCHAR* Key) {
        return !bProject || Fields.Contains(FString(Key));
    };
    const bool bWantBoneName = Wants(TEXT("bonename"));
    const bool bWantConsiderForBounds = Wants(TEXT("considerforbounds"));
    const bool bWantCollisionType = Wants(TEXT("collisiontype"));
    const bool bWantSphereCount = Wants(TEXT("spherecount"));
    const bool bWantBoxCount = Wants(TEXT("boxcount"));
    const bool bWantCapsuleCount = Wants(TEXT("capsulecount"));
    const bool bWantConvexCount = Wants(TEXT("convexcount"));

    // limit truncates the returned array after filtering; 0 = all. totalCount below
    // always reports the full filtered count regardless of the cap.
    const int32 Limit = Ctx.GetInt(TEXT("limit"), 0);

    TArray<TSharedPtr<FJsonValue>> BodyArray;
    int32 TotalMatched = 0;
    for (USkeletalBodySetup* BodySetup : PhysicsAsset->SkeletalBodySetups)
    {
        if (!BodySetup) continue;

        const FString BoneName = BodySetup->BoneName.ToString();
        if (bFiltering && !BoneName.Contains(BoneFilter)) continue;

        ++TotalMatched;
        if (Limit > 0 && BodyArray.Num() >= Limit) continue; // keep counting, stop appending

        TSharedPtr<FJsonObject> BodyObj = MakeShareable(new FJsonObject());
        if (bWantBoneName)
            BodyObj->SetStringField(TEXT("boneName"), BoneName);
        if (bWantConsiderForBounds)
            BodyObj->SetBoolField(TEXT("considerForBounds"), BodySetup->bConsiderForBounds);

        if (bWantCollisionType)
        {
            FString CollisionType;
            switch (BodySetup->CollisionTraceFlag)
            {
                case CTF_UseDefault: CollisionType = TEXT("Default"); break;
                case CTF_UseSimpleAndComplex: CollisionType = TEXT("SimpleAndComplex"); break;
                case CTF_UseSimpleAsComplex: CollisionType = TEXT("SimpleAsComplex"); break;
                case CTF_UseComplexAsSimple: CollisionType = TEXT("ComplexAsSimple"); break;
            }
            BodyObj->SetStringField(TEXT("collisionType"), CollisionType);
        }

        if (bWantSphereCount)
            BodyObj->SetNumberField(TEXT("sphereCount"), BodySetup->AggGeom.SphereElems.Num());
        if (bWantBoxCount)
            BodyObj->SetNumberField(TEXT("boxCount"), BodySetup->AggGeom.BoxElems.Num());
        if (bWantCapsuleCount)
            BodyObj->SetNumberField(TEXT("capsuleCount"), BodySetup->AggGeom.SphylElems.Num());
        if (bWantConvexCount)
            BodyObj->SetNumberField(TEXT("convexCount"), BodySetup->AggGeom.ConvexElems.Num());

        BodyArray.Add(MakeShareable(new FJsonValueObject(BodyObj)));
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetArrayField(TEXT("physicsBodies"), BodyArray);
    // count = rows returned (unchanged semantics); totalCount = full untruncated
    // match count so a caller can tell the list was capped by limit. totalCount +
    // truncated is the shared filter+limit vocabulary of the sibling list readers.
    Result->SetNumberField(TEXT("count"), BodyArray.Num());
    Result->SetNumberField(TEXT("totalCount"), TotalMatched);
    Result->SetBoolField(TEXT("truncated"), BodyArray.Num() < TotalMatched);
    Result->SetNumberField(TEXT("constraintCount"), PhysicsAsset->ConstraintSetup.Num());
    if (bFiltering)
        Result->SetStringField(TEXT("filter"), BoneFilter);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.add_physics_body - Add a physics body to a physics asset
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.add_physics_body", "skeleton",
    "Create a new physics body on a UPhysicsAsset for the given bone, with a default primitive shape (capsule / sphere / box) sized from the bone's bind pose. Existing body for the same bone is left in place — use skeleton.configure_physics_body to modify.",
    RPC_PARAMS(
        RPC_PARAM_REQ("physicsAssetPath", "path", "Path to the physics asset"),
        RPC_PARAM_REQ("boneName", "string", "Bone to create body for"),
        RPC_PARAM_OPT("bodyType", "string", "Geometry type: Sphere, Box, Capsule (default Capsule)"),
        RPC_PARAM_OPT("radius", "number", "Radius (default 10)"),
        RPC_PARAM_OPT("length", "number", "Length for capsule (default 20)"),
        RPC_PARAM_OPT("width", "number", "Width for box (default 10)"),
        RPC_PARAM_OPT("height", "number", "Height for box (default 10)"),
        RPC_PARAM_OPT("depth", "number", "Depth for box (default 10)"),
        RPC_PARAM_OPT("center", "object", "Center offset {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "Rotation {pitch,yaw,roll}")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString PhysicsAssetPath = Ctx.GetString(TEXT("physicsAssetPath"));
    FString BoneName = Ctx.GetString(TEXT("boneName"));
    FString BodyType = Ctx.GetString(TEXT("bodyType"));

    if (PhysicsAssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("physicsAssetPath is required"));
        return true;
    }

    if (BoneName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("boneName is required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    UPhysicsAsset* PhysicsAsset = LoadPhysicsAssetFromPath(PhysicsAssetPath, Error, &bWrongType);
    if (!PhysicsAsset)
    {
        SendPhysicsPathErrorPhys(Ctx, TEXT("PHYSICS_ASSET_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    int32 BodyIndex = PhysicsAsset->FindBodyIndex(FName(*BoneName));
    USkeletalBodySetup* BodySetup = nullptr;
    bool bCreated = false;

    if (BodyIndex == INDEX_NONE)
    {
        BodySetup = NewObject<USkeletalBodySetup>(PhysicsAsset, NAME_None, RF_Transactional);
        if (!BodySetup)
        {
            Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create physics body setup"));
            return true;
        }
        BodySetup->BoneName = FName(*BoneName);
        PhysicsAsset->SkeletalBodySetups.Add(BodySetup);
        bCreated = true;
        BodyIndex = PhysicsAsset->SkeletalBodySetups.Num() - 1;
    }
    else
    {
        BodySetup = PhysicsAsset->SkeletalBodySetups[BodyIndex];
    }

    if (BodyType.IsEmpty()) BodyType = TEXT("Capsule");

    double Radius = 10.0;
    double Length = 20.0;
    double Width = 10.0, Height = 10.0, Depth = 10.0;

    Payload->TryGetNumberField(TEXT("radius"), Radius);
    Payload->TryGetNumberField(TEXT("length"), Length);
    Payload->TryGetNumberField(TEXT("width"), Width);
    Payload->TryGetNumberField(TEXT("height"), Height);
    Payload->TryGetNumberField(TEXT("depth"), Depth);

    FVector Center = ParseVectorFromJson(Payload, TEXT("center"));
    FRotator Rotation = ParseRotatorFromJson(Payload, TEXT("rotation"));

    if (BodyType.Equals(TEXT("Sphere"), ESearchCase::IgnoreCase))
    {
        FKSphereElem SphereElem;
        SphereElem.Radius = static_cast<float>(Radius);
        SphereElem.Center = Center;
        BodySetup->AggGeom.SphereElems.Add(SphereElem);
    }
    else if (BodyType.Equals(TEXT("Box"), ESearchCase::IgnoreCase))
    {
        FKBoxElem BoxElem;
        BoxElem.X = static_cast<float>(Width);
        BoxElem.Y = static_cast<float>(Depth);
        BoxElem.Z = static_cast<float>(Height);
        BoxElem.Center = Center;
        BoxElem.Rotation = Rotation;
        BodySetup->AggGeom.BoxElems.Add(BoxElem);
    }
    else if (BodyType.Equals(TEXT("Capsule"), ESearchCase::IgnoreCase) ||
             BodyType.Equals(TEXT("Sphyl"), ESearchCase::IgnoreCase))
    {
        FKSphylElem CapsuleElem;
        CapsuleElem.Radius = static_cast<float>(Radius);
        CapsuleElem.Length = static_cast<float>(Length);
        CapsuleElem.Center = Center;
        CapsuleElem.Rotation = Rotation;
        BodySetup->AggGeom.SphylElems.Add(CapsuleElem);
    }
    else
    {
        FKSphylElem CapsuleElem;
        CapsuleElem.Radius = static_cast<float>(Radius);
        CapsuleElem.Length = static_cast<float>(Length);
        CapsuleElem.Center = Center;
        BodySetup->AggGeom.SphylElems.Add(CapsuleElem);
    }

    PhysicsAsset->UpdateBodySetupIndexMap();
    PhysicsAsset->UpdateBoundsBodiesArray();
    McpSafeAssetSave(PhysicsAsset);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("boneName"), BoneName);
    Result->SetStringField(TEXT("bodyType"), BodyType);
    Result->SetNumberField(TEXT("bodyIndex"), BodyIndex);
    Result->SetBoolField(TEXT("created"), bCreated);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.configure_physics_body - Configure properties of a physics body
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.configure_physics_body", "skeleton",
    "Update settings on an existing physics body in a UPhysicsAsset: physics type, mass scale, linear/angular damping, collision response. Lookup is by bone name. Use after skeleton.add_physics_body to tune ragdoll behavior.",
    RPC_PARAMS(
        RPC_PARAM_REQ("physicsAssetPath", "path", "Path to the physics asset"),
        RPC_PARAM_REQ("boneName", "string", "Bone name of the body"),
        RPC_PARAM_OPT("mass", "number", "Mass override"),
        RPC_PARAM_OPT("linearDamping", "number", "Linear damping"),
        RPC_PARAM_OPT("angularDamping", "number", "Angular damping"),
        RPC_PARAM_OPT("collisionEnabled", "boolean", "Enable collision"),
        RPC_PARAM_OPT("simulatePhysics", "boolean", "Enable physics simulation")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString PhysicsAssetPath = Ctx.GetString(TEXT("physicsAssetPath"));
    FString BoneName = Ctx.GetString(TEXT("boneName"));

    if (PhysicsAssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("physicsAssetPath is required"));
        return true;
    }

    if (BoneName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("boneName is required"));
        return true;
    }

    if (!Payload->HasField(TEXT("mass")) &&
        !Payload->HasField(TEXT("linearDamping")) &&
        !Payload->HasField(TEXT("angularDamping")) &&
        !Payload->HasField(TEXT("collisionEnabled")) &&
        !Payload->HasField(TEXT("simulatePhysics")))
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("At least one body setting is required: mass, linearDamping, angularDamping, collisionEnabled, or simulatePhysics"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    UPhysicsAsset* PhysicsAsset = LoadPhysicsAssetFromPath(PhysicsAssetPath, Error, &bWrongType);
    if (!PhysicsAsset)
    {
        SendPhysicsPathErrorPhys(Ctx, TEXT("PHYSICS_ASSET_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    int32 BodyIndex = PhysicsAsset->FindBodyIndex(FName(*BoneName));
    if (BodyIndex == INDEX_NONE)
    {
        Ctx.SendError(TEXT("BODY_NOT_FOUND"), FString::Printf(TEXT("No physics body found for bone '%s'"), *BoneName));
        return true;
    }

    USkeletalBodySetup* BodySetup = PhysicsAsset->SkeletalBodySetups[BodyIndex];

    double Mass = 0.0;
    if (Payload->TryGetNumberField(TEXT("mass"), Mass))
    {
        // SetMassOverride writes BOTH bOverrideMass and MassInKgOverride, so the
        // requested mass actually takes effect. Flipping bOverrideMass alone left
        // MassInKgOverride at the engine default of 100 kg.
        BodySetup->DefaultInstance.SetMassOverride(static_cast<float>(Mass), true);
    }

    double LinearDamping = 0.0;
    if (Payload->TryGetNumberField(TEXT("linearDamping"), LinearDamping))
    {
        BodySetup->DefaultInstance.LinearDamping = static_cast<float>(LinearDamping);
    }

    double AngularDamping = 0.0;
    if (Payload->TryGetNumberField(TEXT("angularDamping"), AngularDamping))
    {
        BodySetup->DefaultInstance.AngularDamping = static_cast<float>(AngularDamping);
    }

    bool bCollisionEnabled = true;
    if (Payload->TryGetBoolField(TEXT("collisionEnabled"), bCollisionEnabled))
    {
        BodySetup->DefaultInstance.SetCollisionEnabled(bCollisionEnabled ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    }

    bool bSimulatePhysics = true;
    if (Payload->TryGetBoolField(TEXT("simulatePhysics"), bSimulatePhysics))
    {
        BodySetup->DefaultInstance.bSimulatePhysics = bSimulatePhysics;
    }

    McpSafeAssetSave(PhysicsAsset);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("boneName"), BoneName);
    Result->SetNumberField(TEXT("bodyIndex"), BodyIndex);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.remove_physics_body - Remove a physics body from a physics asset
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.remove_physics_body", "skeleton",
    "Delete a physics body from a UPhysicsAsset by bone name. Constraints involving that body are also removed. Counterpart to skeleton.add_physics_body.",
    RPC_PARAMS(
        RPC_PARAM_REQ("physicsAssetPath", "path", "Path to the physics asset"),
        RPC_PARAM_REQ("boneName", "string", "Bone name of the body to remove")
    ))
{
    FString PhysicsAssetPath = Ctx.GetString(TEXT("physicsAssetPath"));
    FString BoneName = Ctx.GetString(TEXT("boneName"));

    if (PhysicsAssetPath.IsEmpty() || BoneName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("physicsAssetPath and boneName are required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    UPhysicsAsset* PhysAsset = LoadPhysicsAssetFromPath(PhysicsAssetPath, Error, &bWrongType);
    if (!PhysAsset)
    {
        SendPhysicsPathErrorPhys(Ctx, TEXT("PHYSICS_ASSET_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    int32 BodyIndex = INDEX_NONE;
    for (int32 i = 0; i < PhysAsset->SkeletalBodySetups.Num(); ++i)
    {
        if (PhysAsset->SkeletalBodySetups[i] &&
            PhysAsset->SkeletalBodySetups[i]->BoneName == FName(*BoneName))
        {
            BodyIndex = i;
            break;
        }
    }

    if (BodyIndex == INDEX_NONE)
    {
        Ctx.SendError(TEXT("BODY_NOT_FOUND"),
            FString::Printf(TEXT("No physics body found for bone: %s"), *BoneName));
        return true;
    }

    PhysAsset->Modify();

    FName BoneFName(*BoneName);
    for (int32 i = PhysAsset->ConstraintSetup.Num() - 1; i >= 0; --i)
    {
        UPhysicsConstraintTemplate* Constraint = PhysAsset->ConstraintSetup[i];
        if (Constraint)
        {
            FConstraintInstance& CI = Constraint->DefaultInstance;
            if (CI.ConstraintBone1 == BoneFName || CI.ConstraintBone2 == BoneFName)
            {
                PhysAsset->ConstraintSetup.RemoveAt(i);
            }
        }
    }

    PhysAsset->SkeletalBodySetups.RemoveAt(BodyIndex);
    PhysAsset->UpdateBoundsBodiesArray();
    PhysAsset->UpdateBodySetupIndexMap();
    PhysAsset->MarkPackageDirty();
    McpSafeAssetSave(PhysAsset);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("physicsAssetPath"), PhysicsAssetPath);
    Result->SetStringField(TEXT("boneName"), BoneName);
    Result->SetNumberField(TEXT("remainingBodies"), PhysAsset->SkeletalBodySetups.Num());

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.add_physics_constraint - Add a constraint between two physics bodies
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.add_physics_constraint", "skeleton",
    "Create a joint constraint between two existing physics bodies in a UPhysicsAsset, identified by bone name pair. Default limits are unlocked; refine via skeleton.configure_constraint_limits.",
    RPC_PARAMS(
        RPC_PARAM_REQ("physicsAssetPath", "path", "Path to the physics asset"),
        RPC_PARAM_REQ("bodyA", "string", "First body bone name"),
        RPC_PARAM_REQ("bodyB", "string", "Second body bone name"),
        RPC_PARAM_OPT("constraintName", "string", "Constraint name"),
        RPC_PARAM_OPT("limits", "object", "Limit settings {swing1LimitAngle, swing2LimitAngle, twistLimitAngle}")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString PhysicsAssetPath = Ctx.GetString(TEXT("physicsAssetPath"));
    FString BodyA = Ctx.GetString(TEXT("bodyA"));
    FString BodyB = Ctx.GetString(TEXT("bodyB"));
    FString ConstraintName = Ctx.GetString(TEXT("constraintName"));

    if (PhysicsAssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("physicsAssetPath is required"));
        return true;
    }

    if (BodyA.IsEmpty() || BodyB.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("bodyA and bodyB are required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    UPhysicsAsset* PhysicsAsset = LoadPhysicsAssetFromPath(PhysicsAssetPath, Error, &bWrongType);
    if (!PhysicsAsset)
    {
        SendPhysicsPathErrorPhys(Ctx, TEXT("PHYSICS_ASSET_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    if (PhysicsAsset->FindBodyIndex(FName(*BodyA)) == INDEX_NONE)
    {
        Ctx.SendError(TEXT("BODY_NOT_FOUND"),
            FString::Printf(TEXT("Body '%s' not found in physics asset"), *BodyA));
        return true;
    }

    if (PhysicsAsset->FindBodyIndex(FName(*BodyB)) == INDEX_NONE)
    {
        Ctx.SendError(TEXT("BODY_NOT_FOUND"),
            FString::Printf(TEXT("Body '%s' not found in physics asset"), *BodyB));
        return true;
    }

    UPhysicsConstraintTemplate* Constraint = NewObject<UPhysicsConstraintTemplate>(PhysicsAsset, NAME_None, RF_Transactional);
    if (!Constraint)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create physics constraint"));
        return true;
    }

    Constraint->DefaultInstance.ConstraintBone1 = FName(*BodyA);
    Constraint->DefaultInstance.ConstraintBone2 = FName(*BodyB);

    if (!ConstraintName.IsEmpty())
    {
        Constraint->DefaultInstance.JointName = FName(*ConstraintName);
    }

    PhysicsAsset->ConstraintSetup.Add(Constraint);

    const TSharedPtr<FJsonObject>* LimitsObj = nullptr;
    if (Payload->TryGetObjectField(TEXT("limits"), LimitsObj) && LimitsObj && LimitsObj->IsValid())
    {
        double Swing1 = 45.0, Swing2 = 45.0, Twist = 45.0;
        (*LimitsObj)->TryGetNumberField(TEXT("swing1LimitAngle"), Swing1);
        (*LimitsObj)->TryGetNumberField(TEXT("swing2LimitAngle"), Swing2);
        (*LimitsObj)->TryGetNumberField(TEXT("twistLimitAngle"), Twist);

        Constraint->DefaultInstance.SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Limited, static_cast<float>(Swing1));
        Constraint->DefaultInstance.SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Limited, static_cast<float>(Swing2));
        Constraint->DefaultInstance.SetAngularTwistLimit(EAngularConstraintMotion::ACM_Limited, static_cast<float>(Twist));
    }
    else
    {
        Constraint->DefaultInstance.SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Limited, 45.0f);
        Constraint->DefaultInstance.SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Limited, 45.0f);
        Constraint->DefaultInstance.SetAngularTwistLimit(EAngularConstraintMotion::ACM_Limited, 45.0f);
    }

    PhysicsAsset->UpdateBodySetupIndexMap();
    McpSafeAssetSave(PhysicsAsset);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("bodyA"), BodyA);
    Result->SetStringField(TEXT("bodyB"), BodyB);
    Result->SetNumberField(TEXT("constraintIndex"), PhysicsAsset->ConstraintSetup.Num() - 1);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.configure_constraint_limits - Configure angular/linear limits on a constraint
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.configure_constraint_limits", "skeleton",
    "Set angular swing/twist and linear translation limits on an existing physics constraint between two bones. Use to lock joints to anatomically plausible ranges (e.g. elbow swing 0..150°).",
    RPC_PARAMS(
        RPC_PARAM_REQ("physicsAssetPath", "path", "Path to the physics asset"),
        RPC_PARAM_REQ("bodyA", "string", "First body bone name"),
        RPC_PARAM_REQ("bodyB", "string", "Second body bone name"),
        RPC_PARAM_OPT("limits", "object", "Limits object {swing1LimitAngle, swing2LimitAngle, twistLimitAngle, swing1Motion, swing2Motion, twistMotion}"),
        RPC_PARAM_OPT("swing1LimitAngle", "number", "Swing 1 limit angle (direct param)"),
        RPC_PARAM_OPT("swing2LimitAngle", "number", "Swing 2 limit angle (direct param)"),
        RPC_PARAM_OPT("twistLimitAngle", "number", "Twist limit angle (direct param)")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString PhysicsAssetPath = Ctx.GetString(TEXT("physicsAssetPath"));
    FString BodyA = Ctx.GetString(TEXT("bodyA"));
    FString BodyB = Ctx.GetString(TEXT("bodyB"));

    if (PhysicsAssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("physicsAssetPath is required"));
        return true;
    }

    if (BodyA.IsEmpty() || BodyB.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("bodyA and bodyB are required to identify constraint"));
        return true;
    }

    if (!Payload->HasField(TEXT("limits")) &&
        !Payload->HasField(TEXT("swing1LimitAngle")) &&
        !Payload->HasField(TEXT("swing2LimitAngle")) &&
        !Payload->HasField(TEXT("twistLimitAngle")))
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("limits or at least one direct limit angle is required to change the constraint"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    UPhysicsAsset* PhysicsAsset = LoadPhysicsAssetFromPath(PhysicsAssetPath, Error, &bWrongType);
    if (!PhysicsAsset)
    {
        SendPhysicsPathErrorPhys(Ctx, TEXT("PHYSICS_ASSET_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    UPhysicsConstraintTemplate* Constraint = nullptr;
    for (UPhysicsConstraintTemplate* C : PhysicsAsset->ConstraintSetup)
    {
        if (C &&
            C->DefaultInstance.ConstraintBone1 == FName(*BodyA) &&
            C->DefaultInstance.ConstraintBone2 == FName(*BodyB))
        {
            Constraint = C;
            break;
        }
        if (C &&
            C->DefaultInstance.ConstraintBone1 == FName(*BodyB) &&
            C->DefaultInstance.ConstraintBone2 == FName(*BodyA))
        {
            Constraint = C;
            break;
        }
    }

    if (!Constraint)
    {
        Ctx.SendError(TEXT("CONSTRAINT_NOT_FOUND"),
            FString::Printf(TEXT("No constraint found between '%s' and '%s'"), *BodyA, *BodyB));
        return true;
    }

    const TSharedPtr<FJsonObject>* LimitsObj = nullptr;
    if (Payload->TryGetObjectField(TEXT("limits"), LimitsObj) && LimitsObj && LimitsObj->IsValid())
    {
        double Swing1 = 45.0, Swing2 = 45.0, Twist = 45.0;
        (*LimitsObj)->TryGetNumberField(TEXT("swing1LimitAngle"), Swing1);
        (*LimitsObj)->TryGetNumberField(TEXT("swing2LimitAngle"), Swing2);
        (*LimitsObj)->TryGetNumberField(TEXT("twistLimitAngle"), Twist);

        FString Swing1Motion, Swing2Motion, TwistMotion;
        (*LimitsObj)->TryGetStringField(TEXT("swing1Motion"), Swing1Motion);
        (*LimitsObj)->TryGetStringField(TEXT("swing2Motion"), Swing2Motion);
        (*LimitsObj)->TryGetStringField(TEXT("twistMotion"), TwistMotion);

        auto ParseMotion = [](const FString& Motion) -> EAngularConstraintMotion {
            if (Motion.Equals(TEXT("Free"), ESearchCase::IgnoreCase)) return EAngularConstraintMotion::ACM_Free;
            if (Motion.Equals(TEXT("Locked"), ESearchCase::IgnoreCase)) return EAngularConstraintMotion::ACM_Locked;
            return EAngularConstraintMotion::ACM_Limited;
        };

        Constraint->DefaultInstance.SetAngularSwing1Limit(ParseMotion(Swing1Motion), static_cast<float>(Swing1));
        Constraint->DefaultInstance.SetAngularSwing2Limit(ParseMotion(Swing2Motion), static_cast<float>(Swing2));
        Constraint->DefaultInstance.SetAngularTwistLimit(ParseMotion(TwistMotion), static_cast<float>(Twist));
    }
    else
    {
        double Swing1 = 0.0, Swing2 = 0.0, Twist = 0.0;
        if (Payload->TryGetNumberField(TEXT("swing1LimitAngle"), Swing1))
        {
            Constraint->DefaultInstance.SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Limited, static_cast<float>(Swing1));
        }
        if (Payload->TryGetNumberField(TEXT("swing2LimitAngle"), Swing2))
        {
            Constraint->DefaultInstance.SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Limited, static_cast<float>(Swing2));
        }
        if (Payload->TryGetNumberField(TEXT("twistLimitAngle"), Twist))
        {
            Constraint->DefaultInstance.SetAngularTwistLimit(EAngularConstraintMotion::ACM_Limited, static_cast<float>(Twist));
        }
    }

    McpSafeAssetSave(PhysicsAsset);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("bodyA"), BodyA);
    Result->SetStringField(TEXT("bodyB"), BodyB);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.set_physics_asset - Assign existing physics asset to skeletal mesh
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.set_physics_asset", "skeleton",
    "Assign a UPhysicsAsset to a SkeletalMesh's PhysicsAsset slot, controlling which collision/constraint rig is used at runtime. Counterpart to skeleton.create_physics_asset which authors a new one.",
    RPC_PARAMS(
        ParamAliasUtils::MakeAliasParamSpec(TEXT("skeletalMeshPath"), TEXT("path"),
            TEXT("Path to the skeletal mesh (or meshPath)"),
            /*bRequired=*/true, TArray<FString>({TEXT("skeletalMeshPath"), TEXT("meshPath")})),
        RPC_PARAM_REQ("physicsAssetPath", "path", "Path to the physics asset")
    ))
{
    FString SkeletalMeshPath = Ctx.GetStringFirstOf({TEXT("skeletalMeshPath"), TEXT("meshPath")});
    FString PhysicsAssetPath = Ctx.GetString(TEXT("physicsAssetPath"));

    if (SkeletalMeshPath.IsEmpty() || PhysicsAssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletalMeshPath and physicsAssetPath are required"));
        return true;
    }

    FString Error;
    bool bMeshWrongType = false;
    USkeletalMesh* Mesh = LoadSkeletalMeshFromPathPhys(SkeletalMeshPath, Error, &bMeshWrongType);
    if (!Mesh)
    {
        SendMeshPathErrorPhys(Ctx, TEXT("MESH_NOT_FOUND"), Error, bMeshWrongType);
        return true;
    }

    bool bPhysicsWrongType = false;
    UPhysicsAsset* PhysAsset = LoadPhysicsAssetFromPath(PhysicsAssetPath, Error, &bPhysicsWrongType);
    if (!PhysAsset)
    {
        SendPhysicsPathErrorPhys(Ctx, TEXT("PHYSICS_ASSET_NOT_FOUND"), Error, bPhysicsWrongType);
        return true;
    }

    Mesh->SetPhysicsAsset(PhysAsset);
    Mesh->MarkPackageDirty();
    McpSafeAssetSave(Mesh);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("skeletalMeshPath"), SkeletalMeshPath);
    Result->SetStringField(TEXT("physicsAssetPath"), PhysicsAssetPath);
    Result->SetStringField(TEXT("physicsAssetName"), PhysAsset->GetName());

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.get_physics_asset_info - Get detailed info about a physics asset
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.get_physics_asset_info", "skeleton",
    "Read summary metadata about a UPhysicsAsset: numBodies, numConstraints, numPrimitives (total sphere/box/capsule/convex shapes across all bodies), plus skeletalMeshPath (the bound preview SkeletalMesh) when the asset is referenced. This default summary stays inline; pass includeBodies=true (alias verbose) to additionally dump the full per-body bodies[] and per-constraint constraints[] arrays (a large payload that may spill to a HttpResponses file). For the full per-body enumeration use skeleton.list_physics_bodies.",
    RPC_PARAMS(
        RPC_PARAM_OPT("physicsAssetPath", "path", "Path to the physics asset"),
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Path to skeletal mesh (to auto-find physics asset)"),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("includeBodies"), TEXT("boolean"),
            TEXT("Include the full per-body bodies[] and per-constraint constraints[] arrays (default false; alias verbose). Off by default so the summary stays inline."),
            /*bRequired=*/false, TArray<FString>({TEXT("includeBodies"), TEXT("verbose")}))
    ))
{
    FString PhysicsAssetPath = Ctx.GetString(TEXT("physicsAssetPath"));
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    const bool bIncludeBodies = Ctx.GetBoolFirstOf({TEXT("includeBodies"), TEXT("verbose")}, false);

    UPhysicsAsset* PhysAsset = nullptr;
    // The bound mesh the verb resolved against (when called with skeletalMeshPath),
    // used to report the documented "bound SkeletalMesh path".
    USkeletalMesh* BoundMesh = nullptr;

    if (!PhysicsAssetPath.IsEmpty())
    {
        FString Error;
        bool bWrongType = false;
        PhysAsset = LoadPhysicsAssetFromPath(PhysicsAssetPath, Error, &bWrongType);
        if (!PhysAsset)
        {
            SendPhysicsPathErrorPhys(Ctx, TEXT("PHYSICS_ASSET_NOT_FOUND"), Error, bWrongType);
            return true;
        }
    }
    else if (!SkeletalMeshPath.IsEmpty())
    {
        FString Error;
        bool bWrongType = false;
        USkeletalMesh* Mesh = LoadSkeletalMeshFromPathPhys(SkeletalMeshPath, Error, &bWrongType);
        if (!Mesh)
        {
            SendMeshPathErrorPhys(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
            return true;
        }
        BoundMesh = Mesh;
        PhysAsset = Mesh->GetPhysicsAsset();
        if (!PhysAsset)
        {
            Ctx.SendError(TEXT("PHYSICS_ASSET_NOT_FOUND"),
                FString::Printf(TEXT("Skeletal mesh '%s' has no physics asset. Pass physicsAssetPath or assign a physics asset first."), *SkeletalMeshPath));
            return true;
        }
    }
    else
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("physicsAssetPath or skeletalMeshPath is required"));
        return true;
    }

    if (!PhysAsset)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Physics asset not found. Provide physicsAssetPath or skeletalMeshPath"));
        return true;
    }

    // Fall back to the asset's own preview/bound mesh when not resolved via skeletalMeshPath.
    if (!BoundMesh)
    {
        BoundMesh = PhysAsset->GetPreviewMesh();
    }

    int32 NumPrimitives = 0;
    TArray<TSharedPtr<FJsonValue>> BodiesArray;
    for (USkeletalBodySetup* BodySetup : PhysAsset->SkeletalBodySetups)
    {
        if (BodySetup)
        {
            const int32 NumSpheres = BodySetup->AggGeom.SphereElems.Num();
            const int32 NumBoxes = BodySetup->AggGeom.BoxElems.Num();
            const int32 NumCapsules = BodySetup->AggGeom.SphylElems.Num();
            const int32 NumConvex = BodySetup->AggGeom.ConvexElems.Num();
            NumPrimitives += NumSpheres + NumBoxes + NumCapsules + NumConvex;

            if (bIncludeBodies)
            {
                TSharedPtr<FJsonObject> BodyObj = MakeShareable(new FJsonObject());
                BodyObj->SetStringField(TEXT("boneName"), BodySetup->BoneName.ToString());
                BodyObj->SetStringField(TEXT("physicsType"),
                    BodySetup->PhysicsType == EPhysicsType::PhysType_Kinematic ? TEXT("Kinematic") :
                    BodySetup->PhysicsType == EPhysicsType::PhysType_Simulated ? TEXT("Simulated") : TEXT("Default"));
                BodyObj->SetNumberField(TEXT("numSpheres"), NumSpheres);
                BodyObj->SetNumberField(TEXT("numBoxes"), NumBoxes);
                BodyObj->SetNumberField(TEXT("numCapsules"), NumCapsules);
                BodyObj->SetNumberField(TEXT("numConvex"), NumConvex);
                BodiesArray.Add(MakeShareable(new FJsonValueObject(BodyObj)));
            }
        }
    }

    TArray<TSharedPtr<FJsonValue>> ConstraintsArray;
    if (bIncludeBodies)
    {
        for (UPhysicsConstraintTemplate* Constraint : PhysAsset->ConstraintSetup)
        {
            if (Constraint)
            {
                TSharedPtr<FJsonObject> ConObj = MakeShareable(new FJsonObject());
                const FConstraintInstance& CI = Constraint->DefaultInstance;
                ConObj->SetStringField(TEXT("name"), Constraint->GetName());
                ConObj->SetStringField(TEXT("bone1"), CI.ConstraintBone1.ToString());
                ConObj->SetStringField(TEXT("bone2"), CI.ConstraintBone2.ToString());
                ConstraintsArray.Add(MakeShareable(new FJsonValueObject(ConObj)));
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("physicsAssetPath"), PhysAsset->GetPathName());
    Result->SetStringField(TEXT("name"), PhysAsset->GetName());
    Result->SetNumberField(TEXT("numBodies"), PhysAsset->SkeletalBodySetups.Num());
    Result->SetNumberField(TEXT("numConstraints"), PhysAsset->ConstraintSetup.Num());
    // "total primitive count" the doc names — the sum of all bodies' sphere/box/capsule/convex shapes.
    Result->SetNumberField(TEXT("numPrimitives"), NumPrimitives);
    // "bound SkeletalMesh path if the asset is referenced" the doc names.
    if (BoundMesh)
    {
        Result->SetStringField(TEXT("skeletalMeshPath"), BoundMesh->GetPathName());
    }
    // Full per-body / per-constraint arrays are opt-in (includeBodies / verbose) so the
    // default response is the promised lightweight summary that stays inline.
    if (bIncludeBodies)
    {
        Result->SetArrayField(TEXT("bodies"), BodiesArray);
        Result->SetArrayField(TEXT("constraints"), ConstraintsArray);
    }

    Ctx.SendSuccess(Result);
    return true;
}
