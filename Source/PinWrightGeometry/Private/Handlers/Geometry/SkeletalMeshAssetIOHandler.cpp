// Copyright (c) 2026 Alexander Penkin. MIT License.

// SkeletalMeshAssetIOHandler.cpp - the SkeletalMesh <-> DynamicMesh round trip, plus the
// skin-binding step that makes it usable.
//
// WHY THIS FILE EXISTS
//
// The geometry namespace registered ~90 verbs on Geometry Script and NONE of them touched a
// USkeletalMesh: grepping SkeletalMesh|BoneWeight across Source/PinWrightGeometry/ returned
// zero matches. The typed surface was StaticMesh + DynamicMeshActor only. Separately, no verb
// in ANY namespace created a USkeletalMesh (NewObject<USkeletalMesh> appeared only under
// Private/Tests/), while skeleton.create_skeleton did exist - so an agent could create a
// USkeleton and then discover there was no way to give it a mesh. A dead end that looks like a
// working path. Every skinned character authored against this plugin therefore left the typed
// API and ran as raw Python through python.execute.
//
// The three verbs here are the pipeline those scripts converged on - it had already shipped
// four skinned meshes before it was promoted to typed RPCs:
//     create_from_skeletal_mesh  ->  (any geometry.* edits)  ->  bind_skin_weights  ->  convert_to_skeletal_mesh
//
// Engine calls. These are present unchanged on UE 5.3-5.8:
//   - UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromSkeletalMesh / CopyMeshToSkeletalMesh
//     (GeometryScriptingCore, MeshAssetFunctions.h:410,423 on 5.8; identical 6-param signature
//     back to 5.3 at :153,166). The class is named StaticMeshFunctions but the engine's own
//     comment at MeshAssetFunctions.h:227-229 calls that "a naming mistake"; it is the generic
//     asset-utils library and carries the skeletal entry points. Its script binding is
//     GeometryScript_AssetUtils, NOT GeometryScriptLibrary_AssetUtils
//     (UCLASS meta=(ScriptName=...) at :230).
//   - UGeometryScriptLibrary_CreateNewAssetFunctions::CreateNewSkeletalMeshAssetFromMesh
//     (GeometryScriptingEditor, CreateNewAssetUtilityFunctions.h:203; :166 on 5.3, same signature).
//   - UGeometryScriptLibrary_MeshBoneWeightFunctions::{MeshCreateBoneWeights,
//     ComputeSmoothBoneWeights, MeshHasBoneWeights, GetVertexBoneWeights}
//     (GeometryScriptingCore, MeshBoneWeightFunctions.h:280,430,266,326).
//
// KNOWN LIMITATION - THIS FILE DOES NOT COMPILE BELOW UE 5.6 AS WRITTEN.
//
// An earlier revision of this comment claimed the whole set was "present unchanged on UE 5.3-5.8
// (no UE_VERSION_* gating needed)". That was written against 5.8 headers only and is false; it was
// corrected after a direct survey of all six engine trees installed on the authoring machine.
// Three symbols do not exist on older engines, and the plugin claims 5.3-5.8 support:
//
//   - EGeometryScriptBoneHierarchyMismatchHandling and the matching
//     FGeometryScriptCopyMeshToAssetOptions::BoneHierarchyMismatchHandling field: 5.6+ only
//     (MeshAssetFunctions.h:21 / :117). On 5.5 the predecessor is the single bool
//     bRemapBoneIndicesToMatchAsset (:79), which covers only the remap mode and has no
//     CreateNewReferenceSkeleton equivalent. On 5.3/5.4 the options struct carries NO
//     bone-remap control of any kind, so the engine always behaves as DoNothing.
//   - CopyBonesFromSkeleton + FGeometryScriptCopyBonesFromMeshOptions: 5.5+ only
//     (MeshBoneWeightFunctions.h:476 / :237). 5.3/5.4 have only CopyBonesFromMesh (:336), which
//     takes a source UDynamicMesh rather than a USkeleton, so there is no drop-in substitute.
//     ComputeSmoothBoneWeights still takes the USkeleton directly there, so the bind itself would
//     still work - the mesh would simply carry no bone attributes.
//   - FGeometryScriptCopyMeshFromAssetOptions::bUseBuildScale: 5.4+ only (:36).
//
// Everything else the file touches was checked per version and is stable across all six, including
// bUseMeshBoneProportions, EGeometryScriptSmoothBoneWeightsType, GetAllBonesInfo, the
// FGeometryScriptSmoothBoneWeightsOptions field spellings (Weighing, not Weighting), and
// Engine/SkinnedAssetCommon.h. The GeometryScriptingCore/GeometryScriptingEditor split is also
// stable, so no Build.cs dependency differs per version - and PinWrightGeometry.Build.cs:30-32
// already handles 5.3 keeping the plugin under Engine/Plugins/Experimental rather than /Runtime.
//
// The first row IS now guarded, in SkeletalIOApplyMismatchHandling below, because a 5.5 host
// project exists to compile it against and 5.5's bRemapBoneIndicesToMatchAsset covers two of the
// three published tokens exactly. The other two rows are not: CopyBonesFromSkeleton and
// bUseBuildScale both exist from 5.5 up, so nothing on a buildable engine reaches them, and
// guarding them would add 5.3/5.4 branches no host here can compile.
//
// The support-matrix decision this comment asked for has since been made: 5.8 is what is built and
// tested, 5.3-5.7 is intended and deferred, and the plugin now says so rather than advertising a
// range it cannot build. These three symbols are rows in docs/engine-version-support.md, which
// collects every known blocker across the plugin; docs/rpc-design.md section 14 is the rule, and
// this comment is the per-call-site half it points at as the model.
//
// ---------------------------------------------------------------------------
// THE ORDERING TRAP - the reason bind_skin_weights is one verb and not three
// ---------------------------------------------------------------------------
//
// MeshCreateBoneWeights attaches a NEW FDynamicMeshVertexSkinWeightsAttribute, initialised
// against the mesh as it stands at that moment (MeshBoneWeightFunctions.cpp:208-209;
// DynamicVertexSkinWeightsAttribute.h:150-155). The attribute STORE does keep up with later
// geometry - the layer is registered as an external attribute
// (DynamicMeshAttributeSet.cpp:984-993) and its OnNewVertex resizes the backing array
// (DynamicVertexSkinWeightsAttribute.h:448-464) - but it pads with a DEFAULT-CONSTRUCTED,
// EMPTY FBoneWeights. So every vertex added after the bind exists in the attribute and carries
// zero influences. GetVertexBoneWeights reports bHasValidBoneWeights=false for exactly those
// vertices (the flag is literally `BoneWeightsOut.Num() > 0`, MeshBoneWeightFunctions.cpp:302),
// which is what makes them detectable at all.
//
// (Worth stating precisely, because the intuitive version of this bug is wrong: the store does
// NOT go stale and SetVertexBoneWeights on a post-bind vertex is not a no-op. Its only bounds
// check is Mesh.IsVertex(VertexID), MeshBoneWeightFunctions.cpp:371. The `is_valid=false` that
// this failure is usually reported through comes from a different gate - the profile lookup in
// SimpleMeshBoneWeightEdit, MeshBoneWeightFunctions.cpp:52-75, which leaves the out-param at
// its `false` default whenever the named profile does not exist. Both conditions land on the
// same output pin, so they are indistinguishable to a caller.)
//
// The asset write then fails in two different, equally unhelpful ways:
//   - the CREATE path checks only that the attribute EXISTS
//     (`...GetSkinWeightsAttributes().Num() == 0`, CreateNewAssetUtilityFunctions.cpp:370) and
//     reports "LOD 0 has no skin weight attributes" into the UGeometryScriptDebug object -
//     which every call site in this plugin passes as nullptr, so the reason is discarded;
//   - the OVERWRITE path does not fail at all, and under the DEFAULT options it does not even
//     fall back. CopyMeshToSkeletalMesh's skin-weight handling is gated on
//     BoneHierarchyMismatchHandling: with DoNothing it simply breaks out of the switch
//     (MeshAssetFunctions.cpp:1169-1174) while FSkeletalMeshAttributes::Register has already
//     installed a zero-filled skin weight attribute (SkeletalMeshAttributes.cpp:67-72). It then
//     commits and reports Success (MeshAssetFunctions.cpp:1277,1306). The root-binding fallback
//     the header comment at :1039-1041 describes only runs under
//     RemapGeometryToReferenceSkeleton / CreateNewReferenceSkeleton (:1176-1184, :1225-1241).
//     So the default-path outcome is a "successfully" written asset whose every vertex has
//     empty weights.
//
// So: exposing MeshCreateBoneWeights as its own verb would rebuild the same trap at a new
// address. Instead bind_skin_weights performs copy-bones + create-weights + compute-weights as
// one call, which makes the intra-call ordering unrepresentable, and convert_to_skeletal_mesh
// re-validates per-vertex coverage so the INTER-call reordering (bind, then edit geometry,
// then write) is rejected with a typed code instead of shipping a broken asset.
//
// ---------------------------------------------------------------------------
// BASE WEIGHTS, NOT A NAMED PROFILE - the difference from skeleton.*
// ---------------------------------------------------------------------------
//
// Every skeleton.* weight mutator (normalize_weights / prune_weights / set_vertex_weights /
// copy_weights) writes a named FSkinWeightProfileInfo - the engine's "alternate influences"
// channel - and never the LOD's base section skinning. These verbs write BASE weights, because
// CopyMeshToSkeletalMesh / CreateNewSkeletalMeshAssetFromMesh go through the MeshDescription
// import pipeline. That is why no `profile` parameter is exposed here: offering one would
// re-import exactly the ambiguity that makes the skeleton.* family misleading. The profile is
// always FGeometryScriptBoneWeightProfile's default, i.e.
// FSkeletalMeshAttributes::DefaultSkinWeightProfileName.
#include "Compat/EngineVersionCompat.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/Geometry/GeometrySkeletalAssetCreate.h"
#include "Handlers/Geometry/GeometryTarget.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Handlers/Geometry/GeometryNameParamUtils.h"
#include "Dom/JsonObject.h"

#include "Animation/Skeleton.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMeshActor.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
// FSkeletalMaterial is only FORWARD-declared by Engine/SkeletalMesh.h (:62 on 5.8), so reading
// GetMaterials()[i].MaterialSlotName needs the definition. Same include, same reason, as
// Handlers/Asset/SkeletalMeshDumpBuilder.cpp:6.
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "UDynamicMesh.h"
#include "Utils/AssetUtils.h"
#include "Utils/PathUtils.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif

#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshBoneWeightFunctions.h"

// Every helper below is prefixed SkeletalIO. The module builds with bUseUnity = true, so two
// anonymous-namespace statics sharing a name in different .cpp files become an ODR redefinition
// once the TUs are merged; helpers that several files need live in a named-namespace header
// instead (GeometryTarget.h).
namespace
{
    // Cap on the per-slot material detail array, matching MeshAssetIOHandler.cpp's bound so a
    // kitbashed asset cannot push the response toward the response spill threshold.
    // materialSlots always carries the true total.
    constexpr int32 SkeletalIOMaxMaterialDetail = 32;

    // Upper bound on the influences-per-vertex the smooth binder is allowed to produce. The
    // engine's own FBoneWeights inline limit is 12; anything above that is silently discarded
    // by FBoneWeights::Create, so accepting a larger number would be a lie in the echo.
    constexpr int32 SkeletalIOMaxInfluencesCeiling = 12;

    // lodType token -> enum. SkeletalMesh accepts the same vocabulary as StaticMesh so the two
    // halves of the namespace read alike, but the engine COLLAPSES MaxAvailable and
    // HiResSourceModel to SourceModel for skeletal reads (MeshAssetFunctions.cpp:845-848) - a
    // SkeletalMesh has no HiRes source model at all. The echo reports what was actually used.
    bool SkeletalIOParseLodType(const FString& Token, EGeometryScriptLODType& OutType)
    {
        if (Token.IsEmpty() || Token.Equals(TEXT("SourceModel"), ESearchCase::IgnoreCase))
        {
            OutType = EGeometryScriptLODType::SourceModel;
            return true;
        }
        if (Token.Equals(TEXT("MaxAvailable"), ESearchCase::IgnoreCase))
        {
            OutType = EGeometryScriptLODType::MaxAvailable;
            return true;
        }
        if (Token.Equals(TEXT("HiResSourceModel"), ESearchCase::IgnoreCase))
        {
            OutType = EGeometryScriptLODType::HiResSourceModel;
            return true;
        }
        if (Token.Equals(TEXT("RenderData"), ESearchCase::IgnoreCase))
        {
            OutType = EGeometryScriptLODType::RenderData;
            return true;
        }
        return false;
    }

    const TCHAR* SkeletalIOLodTypeName(EGeometryScriptLODType Type)
    {
        switch (Type)
        {
        case EGeometryScriptLODType::HiResSourceModel: return TEXT("HiResSourceModel");
        case EGeometryScriptLODType::MaxAvailable:     return TEXT("MaxAvailable");
        case EGeometryScriptLODType::RenderData:       return TEXT("RenderData");
        default:                                       return TEXT("SourceModel");
        }
    }

    // What the engine will ACTUALLY read after its own collapse, so the response never claims a
    // HiRes source model that a SkeletalMesh cannot have.
    const TCHAR* SkeletalIOEffectiveLodTypeName(EGeometryScriptLODType Type)
    {
        return (Type == EGeometryScriptLODType::RenderData) ? TEXT("RenderData") : TEXT("SourceModel");
    }

    bool SkeletalIOParseBindMethod(const FString& Token, EGeometryScriptSmoothBoneWeightsType& OutType)
    {
        if (Token.IsEmpty() || Token.Equals(TEXT("DirectDistance"), ESearchCase::IgnoreCase))
        {
            OutType = EGeometryScriptSmoothBoneWeightsType::DirectDistance;
            return true;
        }
        if (Token.Equals(TEXT("GeodesicVoxel"), ESearchCase::IgnoreCase))
        {
            OutType = EGeometryScriptSmoothBoneWeightsType::GeodesicVoxel;
            return true;
        }
        return false;
    }

    // The published `boneMismatchHandling` vocabulary, spelled plugin-side rather than as the
    // engine enum: EGeometryScriptBoneHierarchyMismatchHandling is 5.6+ only (see the survey at
    // the top of this file), so parsing straight into it would not compile on 5.3-5.5. The
    // token set is identical on every engine; what differs is which tokens the engine can
    // HONOUR, and that is decided once at the write site below.
    enum class ESkeletalIOBoneMismatch : uint8
    {
        DoNothing,
        RemapGeometryToReferenceSkeleton,
        CreateNewReferenceSkeleton
    };

    bool SkeletalIOParseMismatchHandling(const FString& Token, ESkeletalIOBoneMismatch& OutMode)
    {
        if (Token.IsEmpty() || Token.Equals(TEXT("DoNothing"), ESearchCase::IgnoreCase))
        {
            OutMode = ESkeletalIOBoneMismatch::DoNothing;
            return true;
        }
        if (Token.Equals(TEXT("RemapGeometryToReferenceSkeleton"), ESearchCase::IgnoreCase))
        {
            OutMode = ESkeletalIOBoneMismatch::RemapGeometryToReferenceSkeleton;
            return true;
        }
        if (Token.Equals(TEXT("CreateNewReferenceSkeleton"), ESearchCase::IgnoreCase))
        {
            OutMode = ESkeletalIOBoneMismatch::CreateNewReferenceSkeleton;
            return true;
        }
        return false;
    }

    // Writes the parsed mode onto the engine's options struct, whose shape is version-dependent:
    //
    //   5.6+  BoneHierarchyMismatchHandling, one enumerator per token - all three honoured.
    //   5.5   bRemapBoneIndicesToMatchAsset (bool). Covers DoNothing/Remap exactly; there is no
    //         CreateNewReferenceSkeleton behaviour to reach.
    //   5.3-4 no bone-remap control at all; the engine always behaves as DoNothing.
    //
    // Returns false for a token this engine cannot honour, having filled OutUnsupportedReason.
    // REFUSED rather than downgraded: silently serving DoNothing for a requested re-bind writes
    // an asset whose vertices carry zero influences and reports success, which is the exact
    // failure the ordering-trap comment above exists to keep callers out of.
    bool SkeletalIOApplyMismatchHandling(
        FGeometryScriptCopyMeshToAssetOptions& Options,
        ESkeletalIOBoneMismatch Mode,
        FString& OutUnsupportedReason)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        switch (Mode)
        {
        case ESkeletalIOBoneMismatch::RemapGeometryToReferenceSkeleton:
            Options.BoneHierarchyMismatchHandling =
                EGeometryScriptBoneHierarchyMismatchHandling::RemapGeometryToReferenceSkeleton;
            return true;
        case ESkeletalIOBoneMismatch::CreateNewReferenceSkeleton:
            Options.BoneHierarchyMismatchHandling =
                EGeometryScriptBoneHierarchyMismatchHandling::CreateNewReferenceSkeleton;
            return true;
        default:
            Options.BoneHierarchyMismatchHandling =
                EGeometryScriptBoneHierarchyMismatchHandling::DoNothing;
            return true;
        }
#elif UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        switch (Mode)
        {
        case ESkeletalIOBoneMismatch::RemapGeometryToReferenceSkeleton:
            Options.bRemapBoneIndicesToMatchAsset = true;
            return true;
        case ESkeletalIOBoneMismatch::CreateNewReferenceSkeleton:
            OutUnsupportedReason = TEXT("boneMismatchHandling 'CreateNewReferenceSkeleton' needs "
                "EGeometryScriptBoneHierarchyMismatchHandling, added in UE 5.6. This engine offers "
                "FGeometryScriptCopyMeshToAssetOptions::bRemapBoneIndicesToMatchAsset only, which "
                "has no replace-the-reference-skeleton mode. Use DoNothing or "
                "RemapGeometryToReferenceSkeleton.");
            return false;
        default:
            Options.bRemapBoneIndicesToMatchAsset = false;
            return true;
        }
#else
        if (Mode != ESkeletalIOBoneMismatch::DoNothing)
        {
            OutUnsupportedReason = TEXT("boneMismatchHandling other than 'DoNothing' needs a "
                "bone-remap control on FGeometryScriptCopyMeshToAssetOptions; this engine carries "
                "none and always behaves as DoNothing.");
            return false;
        }
        return true;
#endif
    }

    void SkeletalIOAddCoverageFields(
        const GeometryUtils::FSkinWeightCoverage& Coverage, const TSharedPtr<FJsonObject>& Result)
    {
        if (!Result.IsValid())
        {
            return;
        }
        Result->SetBoolField(TEXT("hasBoneWeights"), Coverage.bHasProfile);
        Result->SetNumberField(TEXT("verticesWeighted"), Coverage.WeightedCount);
        Result->SetNumberField(TEXT("verticesUnweighted"), Coverage.UnweightedCount);

        // IsWeightingTrustworthy, not IsFullyWeighted. `fullyWeighted` is the field callers gate
        // on, and answering true over a mesh whose vertices are dominated by the farthest bone in
        // the skeleton is how a visibly broken viewmodel shipped: every verb in the chain reported
        // success, the only signal was in skeleton.audit_skin_weights' influenceReach block, and
        // that block carries no verdict. A vertex weighted to a bone with every other bone nearer
        // is unweighted for every practical purpose, so it is counted as one here.
        Result->SetBoolField(TEXT("fullyWeighted"), Coverage.IsWeightingTrustworthy());

        // The reason, so `fullyWeighted:false` over a fully-covered mesh is legible rather than
        // baffling. Emitted only when the reach check actually ran (it needs the mesh's own bone
        // attributes and at least two bones), which keeps every existing response unchanged for
        // meshes that carry none.
        if (Coverage.bBoneReachChecked)
        {
            Result->SetNumberField(TEXT("verticesFarBone"), Coverage.FarBoneCount);
            if (Coverage.FarBoneCount > 0)
            {
                TSharedPtr<FJsonObject> Worst = MakeShared<FJsonObject>();
                Worst->SetNumberField(TEXT("vertex"), Coverage.WorstFarBoneVertex);
                Worst->SetStringField(TEXT("bone"), Coverage.WorstFarBoneName.ToString());
                Worst->SetNumberField(TEXT("distance"), Coverage.WorstFarBoneDistance);
                Result->SetObjectField(TEXT("farBoneWorst"), Worst);
            }
        }
    }

    // Bone count carried on the DynamicMesh's own bone attributes (0 when it carries none).
    // Read through GetAllBonesInfo rather than the attribute set directly so the accessor choice
    // stays inside the public Geometry Script surface.
    int32 SkeletalIOCountMeshBones(UDynamicMesh* Mesh)
    {
        if (!Mesh)
        {
            return 0;
        }
        TArray<FGeometryScriptBoneInfo> BonesInfo;
        UGeometryScriptLibrary_MeshBoneWeightFunctions::GetAllBonesInfo(Mesh, BonesInfo);
        return BonesInfo.Num();
    }

    // Echo the SkeletalMesh's material slots. Read off USkeletalMesh::GetMaterials(), the same
    // accessor SkeletalMeshDumpBuilder.cpp:34-40 uses, which is stable across UE 5.3-5.8.
    void SkeletalIOAddMaterialFields(const USkeletalMesh* Mesh, const TSharedPtr<FJsonObject>& Result)
    {
        if (!Mesh || !Result.IsValid())
        {
            return;
        }
        const TArray<FSkeletalMaterial>& Slots = Mesh->GetMaterials();
        Result->SetNumberField(TEXT("materialSlots"), Slots.Num());

        TArray<TSharedPtr<FJsonValue>> Materials;
        const int32 DetailCount = FMath::Min(Slots.Num(), SkeletalIOMaxMaterialDetail);
        for (int32 Index = 0; Index < DetailCount; ++Index)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetNumberField(TEXT("index"), Index);
            Entry->SetStringField(TEXT("slot"), Slots[Index].MaterialSlotName.ToString());
            Entry->SetStringField(TEXT("path"),
                Slots[Index].MaterialInterface ? Slots[Index].MaterialInterface->GetPathName() : FString());
            Materials.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Result->SetArrayField(TEXT("materials"), Materials);
        if (DetailCount < Slots.Num())
        {
            Result->SetBoolField(TEXT("materialsTruncated"), true);
        }
    }

    // Load a USkeleton from skeletonPath, or fall back to the skeleton of skeletalMeshPath.
    // Returns null WITHOUT sending an error - the caller decides whether a missing skeleton is
    // fatal, because convert_to_skeletal_mesh can still take it from the overwrite target.
    // OutResolvedFrom records which input answered, for the response echo.
    USkeleton* SkeletalIOResolveSkeleton(
        const FHandlerContext& Ctx, FString& OutSkeletonPath, FString& OutResolvedFrom)
    {
        OutResolvedFrom.Reset();

        const FString SkeletonPath = Ctx.GetString(TEXT("skeletonPath"));
        if (!SkeletonPath.IsEmpty())
        {
            const FString Sanitized = SanitizeProjectRelativePath(SkeletonPath);
            if (!Sanitized.IsEmpty())
            {
                if (USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *Sanitized))
                {
                    OutSkeletonPath = Skeleton->GetPathName();
                    OutResolvedFrom = TEXT("skeletonPath");
                    return Skeleton;
                }
            }
            return nullptr;
        }

        const FString MeshPath = Ctx.GetStringFirstOf(
            { TEXT("skeletalMeshPath"), TEXT("skeletonFromMesh") }, FString());
        if (!MeshPath.IsEmpty())
        {
            const FString Sanitized = SanitizeProjectRelativePath(MeshPath);
            if (!Sanitized.IsEmpty())
            {
                if (USkeletalMesh* SourceMesh = LoadObject<USkeletalMesh>(nullptr, *Sanitized))
                {
                    if (USkeleton* Skeleton = SourceMesh->GetSkeleton())
                    {
                        OutSkeletonPath = Skeleton->GetPathName();
                        OutResolvedFrom = TEXT("skeletalMeshPath");
                        return Skeleton;
                    }
                }
            }
        }
        return nullptr;
    }
}

// ============================================================================
// geometry.create_from_skeletal_mesh
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_from_skeletal_mesh", "geometry",
    "Load an existing SkeletalMesh asset into an editable DynamicMeshActor, carrying its bones and BASE skin weights, so every geometry.* op applies to it. The inverse of geometry.convert_to_skeletal_mesh, and the skeletal twin of geometry.create_from_static_mesh. Idempotent by default: a second call with the same name reloads the SAME actor (reuseExisting).",
    RPC_PARAMS(
        ParamAliasUtils::MakeAliasParamSpec(TEXT("assetPath"), TEXT("path"),
            TEXT("SkeletalMesh asset to load, e.g. /Game/Chars/SKM_Hero. The meshPath spelling the asset.* mesh verbs use is also accepted."),
            /*bRequired=*/true, TArray<FString>({TEXT("assetPath"), TEXT("meshPath")})),
        GeometryNameParamUtils::CreateNameParamOpt(
            TEXT("Label for the editable DynamicMeshActor (accepts the 'actorName' alias the operate verbs use). Default '<MeshName>_Edit'.")),
        RPC_PARAM_OPT("location", "object", "Spawn location {x, y, z}"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation {pitch, yaw, roll}"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale {x, y, z}"),
        RPC_PARAM_DEF("lodType", "string", "Which mesh to read: SourceModel | RenderData (MaxAvailable and HiResSourceModel are accepted but the engine collapses BOTH to SourceModel for skeletal assets - a SkeletalMesh has no HiRes source model). SourceModel reads the editable MeshDescription and preserves skin weights; RenderData reads the built render mesh, split at UV seams and hard-normal creases", "SourceModel"),
        RPC_PARAM_DEF("lodIndex", "integer", "LOD index. The engine SILENTLY CLAMPS this to the asset's source-model count, so the echoed lodIndex is what you asked for, not necessarily what was read", "0"),
        RPC_PARAM_DEF("applyBuildSettings", "boolean", "Apply the asset's Build Settings during the copy (default true)", "true"),
        RPC_PARAM_DEF("useBuildScale", "boolean", "Scale the copied mesh by the asset's Build Scale (default true)", "true"),
        RPC_PARAM_DEF("requestTangents", "boolean", "Request tangents on the copied mesh (default true)", "true"),
        RPC_PARAM_DEF("reuseExisting", "boolean", "When a DynamicMeshActor already carries the target label, reload into it instead of spawning a duplicate (default true). Deliberately unlike the geometry.create_* family, which always spawns", "true")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString AssetPath = Ctx.GetStringFirstOf({ TEXT("assetPath"), TEXT("meshPath") }, FString());
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("assetPath required"));
        return true;
    }

    // Same sanitizer convert_to_skeletal_mesh uses, so both halves of the round trip accept
    // exactly the same path vocabulary.
    const FString SanitizedAssetPath = SanitizeProjectRelativePath(AssetPath);
    if (SanitizedAssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_PATH,
            TEXT("Invalid assetPath - rejected due to security validation"));
        return true;
    }
    AssetPath = SanitizedAssetPath;

    EGeometryScriptLODType LodType = EGeometryScriptLODType::SourceModel;
    const FString LodTypeToken = Ctx.GetString(TEXT("lodType"));
    if (!SkeletalIOParseLodType(LodTypeToken, LodType))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Unknown lodType '%s'. Valid: SourceModel, RenderData, MaxAvailable, HiResSourceModel"),
                *LodTypeToken));
        return true;
    }
    const int32 LodIndex = FMath::Max(0, Ctx.GetInt(TEXT("lodIndex"), 0));

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!IsValid(World))
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_WORLD_NOT_AVAILABLE, TEXT("Editor world not available"));
        return true;
    }

    USkeletalMesh* SourceMesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath);
    if (!SourceMesh)
    {
        Ctx.SendError(ErrorCodes::ERR_MESH_NOT_FOUND,
            FString::Printf(TEXT("SkeletalMesh not found: %s"), *AssetPath));
        return true;
    }

    const FString ActorLabel = GeometryNameParamUtils::ResolveCreateName(
        Ctx, FString::Printf(TEXT("%s_Edit"), *SourceMesh->GetName()));

    // Guard before allocating: the MeshDescription->DynamicMesh conversion builds the whole
    // FDynamicMesh3 up front, so a hero character can be a large spike. Same pre-flight the
    // heavy mesh ops use.
    if (!GeometryUtils::IsMemoryPressureSafe())
    {
        Ctx.SendError(ErrorCodes::ERR_MEMORY_PRESSURE,
            TEXT("Insufficient memory headroom to copy a SkeletalMesh into a dynamic mesh"));
        return true;
    }

    // reuseExisting is what makes a re-run idempotent. ActorLabel is a CREATE LABEL, not an
    // actor identity: resolve it by exact display label among DynamicMeshActors only, so a
    // coincident internal name or object path cannot capture the request and duplicate labels
    // remain an explicit ambiguity. Default true because this verb's identity is the SOURCE
    // ASSET.
    const bool bReuseExisting = Ctx.GetBool(TEXT("reuseExisting"), true);
    McpActorUtils::FActorResolution ExistingResolution;
    if (bReuseExisting)
    {
        ExistingResolution = McpActorUtils::ResolveActorFiltered(World, ActorLabel,
            [](AActor* Actor)
            {
                return Actor && Actor->IsA(ADynamicMeshActor::StaticClass());
            }, McpActorUtils::EActorResolvePolicy::ExactLabel);
        if (ExistingResolution.IsAmbiguous())
        {
            ActorNameParamUtils::SendAmbiguousActorError(Ctx, ActorLabel, ExistingResolution);
            return true;
        }
    }
    ADynamicMeshActor* ExistingActor = ExistingResolution.IsResolved()
        ? Cast<ADynamicMeshActor>(ExistingResolution.Actor)
        : nullptr;
    UDynamicMeshComponent* ExistingComponent =
        ExistingActor ? ExistingActor->GetDynamicMeshComponent() : nullptr;
    if (ExistingActor && !ExistingComponent)
    {
        // Pathological: a DynamicMeshActor with no component. Fall through to a fresh spawn
        // rather than erroring, so a corrupt actor cannot wedge the verb.
        ExistingActor = nullptr;
    }

    // On the reuse path we copy straight into the component's own UDynamicMesh, because
    // CopyMeshFromSkeletalMesh REPLACES the destination's contents wholesale
    // (ToDynamicMesh->SetMesh(MoveTemp(NewMesh)), MeshAssetFunctions.cpp:899) - no clear step.
    UDynamicMesh* TargetMesh = ExistingComponent
        ? ExistingComponent->GetDynamicMesh()
        : GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());
    if (!TargetMesh)
    {
        Ctx.SendError(ErrorCodes::ERR_MESH_NOT_FOUND,
            TEXT("Could not allocate a destination dynamic mesh"));
        return true;
    }

    FGeometryScriptCopyMeshFromAssetOptions CopyOptions;
    CopyOptions.bApplyBuildSettings = Ctx.GetBool(TEXT("applyBuildSettings"), true);
    // FGeometryScriptCopyMeshFromAssetOptions::bUseBuildScale arrived in UE 5.4. On 5.3 the
    // converter applies the LOD's BuildScale3D unconditionally (GeometryScriptingCore
    // MeshAssetFunctions.cpp:156-159), i.e. it behaves exactly as bUseBuildScale=true, which is
    // the default here. A caller who asked for false is refused by name rather than silently
    // served a build-scaled mesh under a success.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    CopyOptions.bUseBuildScale = Ctx.GetBool(TEXT("useBuildScale"), true);
#else
    if (!Ctx.GetBool(TEXT("useBuildScale"), true))
    {
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION, TEXT(
            "useBuildScale=false needs FGeometryScriptCopyMeshFromAssetOptions::bUseBuildScale, "
            "added in UE 5.4. This engine always applies the LOD's build scale; omit the "
            "parameter or pass true."));
        return true;
    }
#endif
    CopyOptions.bRequestTangents = Ctx.GetBool(TEXT("requestTangents"), true);

    FGeometryScriptMeshReadLOD ReadLOD;
    ReadLOD.LODType = LodType;
    ReadLOD.LODIndex = LodIndex;

    EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;

    // Debug is nullptr to match every other geometry callsite; the cost is that the engine's
    // FText reason is discarded and we report a generic CONVERSION_FAILED.
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromSkeletalMesh(
        SourceMesh, TargetMesh, CopyOptions, ReadLOD, Outcome, nullptr);

    if (Outcome != EGeometryScriptOutcomePins::Success)
    {
        if (!ExistingComponent && TargetMesh)
        {
            TargetMesh->MarkAsGarbage();
        }
        Ctx.SendError(ErrorCodes::ERR_CONVERSION_FAILED,
            FString::Printf(TEXT("Could not copy '%s' (lodType %s, lodIndex %d) into a dynamic mesh"),
                *AssetPath, SkeletalIOLodTypeName(LodType), LodIndex));
        return true;
    }

    // Post-copy budget check, done AFTER the copy for the same reason as
    // create_from_static_mesh: no LODType gives a reliable pre-count, and enforcing it here
    // still avoids spawning a renderer for a mesh the rest of the namespace refuses to touch.
    const int32 TriangleCount = TargetMesh->GetTriangleCount();
    if (TriangleCount > GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH)
    {
        if (!ExistingComponent && TargetMesh)
        {
            TargetMesh->MarkAsGarbage();
        }
        Ctx.SendError(ErrorCodes::ERR_POLYGON_LIMIT_EXCEEDED,
            FString::Printf(TEXT("'%s' has %d triangles at this LOD, over the %d dynamic-mesh limit. Load a coarser lodIndex."),
                *AssetPath, TriangleCount, GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH));
        return true;
    }

    FTransform SpawnTransform = FTransform::Identity;
    if (Payload.IsValid())
    {
        if (Payload->HasField(TEXT("location")))
        {
            SpawnTransform.SetLocation(
                GeometryUtils::ReadVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector));
        }
        if (Payload->HasField(TEXT("rotation")))
        {
            SpawnTransform.SetRotation(FQuat(
                GeometryUtils::ReadRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator)));
        }
        if (Payload->HasField(TEXT("scale")))
        {
            SpawnTransform.SetScale3D(
                GeometryUtils::ReadVectorFromPayload(Payload, TEXT("scale"), FVector::OneVector));
        }
    }

    AActor* ResultActor = ExistingActor;
    if (ExistingComponent)
    {
        GeometryUtils::MarkGeometryActorModified(ExistingComponent);
    }
    else
    {
        // This verb never fell back to the EditorActorSubsystem's world the way the create_*
        // verbs do; keep it that way.
        GeometryTarget::FSpawnOptions SpawnOptions;
        SpawnOptions.bFallBackToActorSubsystemWorld = false;

        ResultActor = GeometryTarget::Spawn(Ctx, TargetMesh, SpawnTransform, ActorLabel, SpawnOptions);
        if (!ResultActor)
        {
            return true;  // spawn helper already sent the error
        }
    }

    // Coverage is reported on LOAD, not just on write, so an agent learns immediately that a
    // source asset it inherited has unweighted vertices - rather than discovering it only when
    // convert_to_skeletal_mesh refuses the write several edits later.
    const GeometryUtils::FSkinWeightCoverage Coverage = GeometryUtils::ScanSkinWeightCoverage(TargetMesh);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ResultActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetBoolField(TEXT("reused"), ExistingComponent != nullptr);
    Result->SetStringField(TEXT("lodType"), SkeletalIOLodTypeName(LodType));
    // What the engine actually read after collapsing MaxAvailable/HiResSourceModel.
    Result->SetStringField(TEXT("effectiveLodType"), SkeletalIOEffectiveLodTypeName(LodType));
    Result->SetNumberField(TEXT("lodIndex"), LodIndex);
    GeometryUtils::SetMeshCountFields(TargetMesh, Result);
    {
        const UE::Geometry::FDynamicMesh3& ReadMesh = TargetMesh->GetMeshRef();
        Result->SetBoolField(TEXT("hasNormals"),
            ReadMesh.HasAttributes() && ReadMesh.Attributes()->PrimaryNormals() != nullptr);
        Result->SetBoolField(TEXT("hasUVs"), GeometryUtils::MeshHasUsableUVs(TargetMesh));
        Result->SetBoolField(TEXT("hasColors"),
            ReadMesh.HasAttributes() && ReadMesh.Attributes()->HasPrimaryColors());
    }
    SkeletalIOAddCoverageFields(Coverage, Result);
    Result->SetNumberField(TEXT("boneCount"), SkeletalIOCountMeshBones(TargetMesh));
    if (const USkeleton* SourceSkeleton = SourceMesh->GetSkeleton())
    {
        Result->SetStringField(TEXT("skeletonPath"), SourceSkeleton->GetPathName());
    }
    SkeletalIOAddMaterialFields(SourceMesh, Result);
    AddActorVerification(Result, ResultActor);
    Ctx.SendSuccess(TEXT("SkeletalMesh loaded into an editable DynamicMesh"), Result);
    return true;
}

// ============================================================================
// geometry.bind_skin_weights
// ============================================================================
REGISTER_RPC_HANDLER("geometry.bind_skin_weights", "geometry",
    "Bind a DynamicMeshActor's mesh to a skeleton, writing BASE skin weights for every vertex in one call. RUN THIS LAST, after all geometry edits: the skin-weight attribute is sized against the mesh as it stands, so vertices added afterwards come out unweighted and geometry.convert_to_skeletal_mesh will refuse to write them. Combines copy-bones + create-weights + smooth-bind so that ordering cannot be got wrong within the call.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor to bind"),
        RPC_PARAM_OPT("skeletonPath", "path", "USkeleton asset to bind to, e.g. /Game/Chars/SK_Hero_Skeleton. Provide this or skeletalMeshPath."),
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Take the skeleton from this SkeletalMesh asset instead of naming the USkeleton directly"),
        RPC_PARAM_DEF("maxInfluences", "integer", "Maximum bones contributing to each vertex, 1-12. 1 is a rigid binding; 4 is the usual game-character value and what this project shipped. The engine's own default is 5", "4"),
        RPC_PARAM_DEF("stiffness", "number", "How rigid the binding is, 0-1. Higher values weight nearer bones more strongly. The engine default is 0.2", "0.2"),
        RPC_PARAM_DEF("method", "string", "DirectDistance (Euclidean bone-to-vertex distance) or GeodesicVoxel (distance along the surface). DirectDistance is the default and the only variant this project has exercised end to end; GeodesicVoxel is slower, allocates a voxel grid, and its quality depends on voxelResolution", "DirectDistance"),
        RPC_PARAM_DEF("voxelResolution", "integer", "Voxel grid resolution for GeodesicVoxel. Ignored by DirectDistance. Higher is more faithful but slower and hungrier", "128")
    ))
{
    const FString ActorName = Ctx.GetString(TEXT("actorName"));

    // This half of the namespace reports a missing editor world as EDITOR_WORLD_NOT_AVAILABLE,
    // not NO_WORLD.
    GeometryTarget::FResolveOptions ResolveOptions;
    ResolveOptions.NoWorldCode = ErrorCodes::ERR_EDITOR_WORLD_NOT_AVAILABLE;
    ResolveOptions.NoWorldMessage = TEXT("Editor world not available");

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target, ResolveOptions))
    {
        return true;
    }

    FString SkeletonPath;
    FString SkeletonSource;
    USkeleton* Skeleton = SkeletalIOResolveSkeleton(Ctx, SkeletonPath, SkeletonSource);
    if (!Skeleton)
    {
        Ctx.SendError(ErrorCodes::ERR_SKELETON_NOT_FOUND,
            TEXT("Provide skeletonPath (a USkeleton asset) or skeletalMeshPath (a SkeletalMesh whose skeleton to reuse). skeleton.create_skeleton can make one."));
        return true;
    }

    // The binder builds its transform hierarchy straight off the reference skeleton
    // (FSkinBindingOp::SetTransformHierarchyFromReferenceSkeleton, MeshBoneWeightFunctions.cpp:640).
    // A bone-less skeleton would give it nothing to solve against, so reject it by name here
    // rather than letting the op fail somewhere inside.
    if (Skeleton->GetReferenceSkeleton().GetRawBoneNum() <= 0)
    {
        Ctx.SendError(ErrorCodes::ERR_SKELETON_HAS_NO_BONES,
            FString::Printf(TEXT("Skeleton '%s' has no bones - add some with skeleton.add_bone before binding"),
                *SkeletonPath));
        return true;
    }

    // A mesh with no vertices has nothing to bind and would leave the caller with a
    // success-shaped response describing zero work.
    const int32 VertexCountBefore = GeometryUtils::GetMeshVertexCount(Target.Mesh);
    if (VertexCountBefore <= 0)
    {
        Ctx.SendError(ErrorCodes::ERR_MESH_EMPTY,
            FString::Printf(TEXT("'%s' has no vertices - build the geometry BEFORE binding skin weights"),
                *ActorName));
        return true;
    }

    const int32 MaxInfluences =
        FMath::Clamp(Ctx.GetInt(TEXT("maxInfluences"), 4), 1, SkeletalIOMaxInfluencesCeiling);
    const float Stiffness = static_cast<float>(FMath::Clamp(Ctx.GetNumber(TEXT("stiffness"), 0.2), 0.0, 1.0));
    const int32 VoxelResolution = FMath::Clamp(Ctx.GetInt(TEXT("voxelResolution"), 128), 8, 1024);

    EGeometryScriptSmoothBoneWeightsType BindMethod = EGeometryScriptSmoothBoneWeightsType::DirectDistance;
    const FString MethodToken = Ctx.GetString(TEXT("method"));
    if (!SkeletalIOParseBindMethod(MethodToken, BindMethod))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Unknown method '%s'. Valid: DirectDistance, GeodesicVoxel"), *MethodToken));
        return true;
    }

    if (!GeometryUtils::IsMemoryPressureSafe())
    {
        Ctx.SendError(ErrorCodes::ERR_MEMORY_PRESSURE,
            TEXT("Insufficient memory headroom to compute a skin binding"));
        return true;
    }

    // --- Step 1: give the mesh the skeleton's bone attributes. ---------------------------
    // Without this the mesh has no bone names, so CreateNewSkeletalMeshAssetFromMesh takes its
    // "no bone attributes" branch (CreateNewAssetUtilityFunctions.cpp:447-458) and skips the
    // re-indexing that would have caught a bone mismatch. Copying them first means a mismatch
    // becomes a reported error instead of silently-wrong weights.
    GeometryUtils::CopySkeletonBonesToMesh(Skeleton, Target.Mesh);

    // --- Step 2: (re)create the base skin-weight attribute, NOW. --------------------------
    // bReplaceExistingProfile=true is deliberate. The attribute is allocated against the
    // current mesh, so re-running this verb after further geometry edits must start clean
    // rather than inherit an attribute sized to the old vertex set. This placement - after all
    // geometry, immediately before the solve - is the entire reason the three steps are one
    // verb.
    bool bProfileExisted = false;
    UGeometryScriptLibrary_MeshBoneWeightFunctions::MeshCreateBoneWeights(
        Target.Mesh, bProfileExisted, /*bReplaceExistingProfile=*/true);

    // --- Step 3: solve. -------------------------------------------------------------------
    FGeometryScriptSmoothBoneWeightsOptions SmoothOptions;
    // Note the engine's spelling: DistanceWeighingType, not DistanceWeightingType.
    SmoothOptions.DistanceWeighingType = BindMethod;
    SmoothOptions.Stiffness = Stiffness;
    SmoothOptions.MaxInfluences = MaxInfluences;
    SmoothOptions.VoxelResolution = VoxelResolution;

    UGeometryScriptLibrary_MeshBoneWeightFunctions::ComputeSmoothBoneWeights(
        Target.Mesh, Skeleton, SmoothOptions, FGeometryScriptBoneWeightProfile(), nullptr);

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    // --- Verify, do not assume. -----------------------------------------------------------
    // The solve is reported back as measured coverage, not as an echo of the request. A
    // partially-covered result is still a SUCCESS response (the mesh really was mutated, and
    // saying otherwise after the fact would be worse) but it carries fullyWeighted:false, and
    // convert_to_skeletal_mesh will refuse to write it unless the caller opts in explicitly.
    const GeometryUtils::FSkinWeightCoverage Coverage = GeometryUtils::ScanSkinWeightCoverage(Target.Mesh);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    Result->SetStringField(TEXT("skeletonPath"), SkeletonPath);
    Result->SetStringField(TEXT("skeletonResolvedFrom"), SkeletonSource);
    Result->SetNumberField(TEXT("skeletonBoneCount"), Skeleton->GetReferenceSkeleton().GetRawBoneNum());
    Result->SetNumberField(TEXT("meshBoneCount"), SkeletalIOCountMeshBones(Target.Mesh));
    Result->SetStringField(TEXT("method"),
        BindMethod == EGeometryScriptSmoothBoneWeightsType::GeodesicVoxel
            ? TEXT("GeodesicVoxel") : TEXT("DirectDistance"));
    Result->SetNumberField(TEXT("maxInfluences"), MaxInfluences);
    Result->SetNumberField(TEXT("stiffness"), Stiffness);
    if (BindMethod == EGeometryScriptSmoothBoneWeightsType::GeodesicVoxel)
    {
        Result->SetNumberField(TEXT("voxelResolution"), VoxelResolution);
    }
    // True when this call reset a binding that was already there - a re-bind after further
    // geometry edits, which is the supported way out of the ordering trap.
    Result->SetBoolField(TEXT("rebound"), bProfileExisted);
    // Read off the struct rather than hardcoded, so the echo cannot drift from whatever
    // FSkeletalMeshAttributes::DefaultSkinWeightProfileName actually is on this engine (it is
    // "Default" on 5.8). These verbs never write a named alternate profile - see the file
    // header - so this is always the base one.
    Result->SetStringField(TEXT("profile"),
        FGeometryScriptBoneWeightProfile().GetProfileName().ToString());
    GeometryUtils::SetMeshCountFields(Target.Mesh, Result);
    SkeletalIOAddCoverageFields(Coverage, Result);
    if (!Coverage.IsFullyWeighted())
    {
        Result->SetStringField(TEXT("warning"),
            FString::Printf(TEXT("%d of %d vertices carry no influence after the bind. geometry.convert_to_skeletal_mesh will refuse to write this mesh unless allowPartialSkinning:true."),
                Coverage.UnweightedCount, Coverage.VertexCount));
    }
    else if (Coverage.FarBoneCount > 0)
    {
        // Covered but not credible. Reported rather than refused: the reach check measures the
        // mesh against the reference pose its own bone attributes carry, so a caller that
        // deliberately moved the mesh away from its skeleton would be blocked by a false alarm.
        Result->SetStringField(TEXT("warning"),
            FString::Printf(TEXT("%d of %d vertices are dominated by the bone FARTHEST from them (worst: vertex %d bound to '%s' %.1f uu away). That is the signature of skin weights damaged by a topology edit; re-bind, or audit with skeleton.audit_skin_weights before baking."),
                Coverage.FarBoneCount, Coverage.VertexCount,
                Coverage.WorstFarBoneVertex, *Coverage.WorstFarBoneName.ToString(),
                Coverage.WorstFarBoneDistance));
    }
    AddActorVerification(Result, Target.Actor);
    Result->SetStringField(TEXT("actorName"), ActorName);
    Ctx.SendSuccess(TEXT("Skin weights bound"), Result);
    return true;
}

// ============================================================================
// geometry.convert_to_skeletal_mesh
// ============================================================================
REGISTER_RPC_HANDLER("geometry.convert_to_skeletal_mesh", "geometry",
    "Bake a DynamicMeshActor into a SkeletalMesh asset and force it to disk. Creates a new asset by default and REFUSES an occupied assetPath (ASSET_EXISTS); overwrite:true rewrites the EXISTING asset's LOD in place, PRESERVING its material slots. Both branches repair null or missing slots so every imported section has an in-range material. Refuses to write a mesh whose vertices are not all skinned, so the classic 'bound the weights before finishing the geometry' mistake is a typed error rather than a broken asset. The inverse of geometry.create_from_skeletal_mesh.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor to bake"),
        RPC_PARAM_OPT("assetPath", "path", "Asset path for the SkeletalMesh (default /Game/GeneratedMeshes/<actorName>)"),
        RPC_PARAM_OPT("skeletonPath", "path", "USkeleton the new asset binds to. REQUIRED when creating; on overwrite it defaults to the target asset's own skeleton"),
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Take the skeleton from this SkeletalMesh asset instead of naming the USkeleton directly"),
        RPC_PARAM_DEF("overwrite", "boolean", "DESTRUCTIVE, opt-in: rewrite the geometry of the EXISTING asset at assetPath instead of creating a new one. Preserves that asset's material slots; both branches ensure every imported section has an in-range, non-null slot. Required whenever assetPath is occupied - the create path refuses with ASSET_EXISTS rather than let the engine silently wipe the asset's LODs, materials, reference skeleton and physics asset in place. /Engine/ assets are refused outright. Default false", "false"),
        RPC_PARAM_DEF("lodIndex", "integer", "Target LOD for the overwrite path (ignored when creating; a created asset always gets LOD 0). Writing the HiRes source is not supported for skeletal assets", "0"),
        RPC_PARAM_DEF("boneMismatchHandling", "string", "Overwrite path only. What to do when the mesh's bone hierarchy differs from the target asset's reference skeleton: DoNothing (default - fastest, and correct when the mesh was bound to this very skeleton) | RemapGeometryToReferenceSkeleton (re-bind the weights onto the asset's skeleton) | CreateNewReferenceSkeleton (replace the asset's reference skeleton with the mesh's; drops virtual bones)", "DoNothing"),
        RPC_PARAM_DEF("allowPartialSkinning", "boolean", "ESCAPE HATCH, leave false. Write even when some vertices carry no influence. The engine will not stop you: the create path only checks that a skin-weight ATTRIBUTE exists, and the overwrite path does not check at all - under the default boneMismatchHandling it commits zero-filled weights and reports success. verticesUnweighted is reported either way", "false"),
        RPC_PARAM_DEF("save", "boolean", "Write the .uasset to disk (default true). Pass false to leave the package dirty-in-memory when batching many bakes before one editor.save_all", "true")
    ))
{
    const FString ActorName = Ctx.GetString(TEXT("actorName"));

    // This half of the namespace reports a missing editor world as EDITOR_WORLD_NOT_AVAILABLE,
    // not NO_WORLD.
    GeometryTarget::FResolveOptions ResolveOptions;
    ResolveOptions.NoWorldCode = ErrorCodes::ERR_EDITOR_WORLD_NOT_AVAILABLE;
    ResolveOptions.NoWorldMessage = TEXT("Editor world not available");

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target, ResolveOptions))
    {
        return true;
    }

    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        AssetPath = GeometryUtils::MakeDefaultGeometryAssetPath(Target.Actor);
    }
    const FString SanitizedAssetPath = SanitizeProjectRelativePath(AssetPath);
    if (SanitizedAssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_PATH,
            TEXT("Invalid assetPath - rejected due to security validation"));
        return true;
    }
    AssetPath = SanitizedAssetPath;

    const bool bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), true);
    const bool bAllowPartialSkinning = Ctx.GetBool(TEXT("allowPartialSkinning"), false);
    const int32 LodIndex = FMath::Max(0, Ctx.GetInt(TEXT("lodIndex"), 0));

    ESkeletalIOBoneMismatch MismatchHandling = ESkeletalIOBoneMismatch::DoNothing;
    const FString MismatchToken = Ctx.GetString(TEXT("boneMismatchHandling"));
    if (!SkeletalIOParseMismatchHandling(MismatchToken, MismatchHandling))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Unknown boneMismatchHandling '%s'. Valid: DoNothing, RemapGeometryToReferenceSkeleton, CreateNewReferenceSkeleton"),
                *MismatchToken));
        return true;
    }

    // --- Crash guard, hoisted above the create/overwrite fork. ----------------------------
    // Both branches run the MikkT tangent pass (FStaticMeshOperations::ComputeMikktTangents),
    // which indexes [0] into the baked UV array. A mesh authored via append_buffers/
    // append_vertex without UVs has a size-0 array -> Array.h OOB assert -> hard editor crash
    // on the async build worker. Toggling the recompute flags does not prevent it; the mesh
    // must actually carry UVs. Identical treatment to convert_to_static_mesh
    // (MeshOpsHandler.cpp:406-429), including only dirtying the level when the guard actually
    // had to add the layer.
    const bool bMeshHadUVsBeforeGuard = GeometryUtils::MeshHasUsableUVs(Target.Mesh);
    const bool bHasUVs = GeometryUtils::EnsureMeshHasUVs(Target.Mesh);
    if (!bMeshHadUVsBeforeGuard && bHasUVs)
    {
        GeometryUtils::MarkGeometryActorModified(Target.Component);
    }

    // --- The ordering-trap gate. ----------------------------------------------------------
    // Everything above this point is shared with the StaticMesh bake. This is the part that is
    // specific to skinned meshes, and it is the reason this verb exists as a typed RPC at all.
    // Neither engine entry point protects the caller here:
    //   - CreateNewSkeletalMeshAssetFromMesh checks only that a skin-weight ATTRIBUTE is
    //     present (CreateNewAssetUtilityFunctions.cpp:370) and reports its refusal into the
    //     UGeometryScriptDebug object, which we (like every geometry callsite) pass as nullptr;
    //   - CopyMeshToSkeletalMesh does not check at all. Under this verb's default
    //     boneMismatchHandling (DoNothing) its skin-weight switch breaks out immediately
    //     (MeshAssetFunctions.cpp:1169-1174), the zero-filled attribute that
    //     FSkeletalMeshAttributes::Register installed (SkeletalMeshAttributes.cpp:67-72) is
    //     committed as-is, and Success is reported (:1277, :1306).
    // Both outcomes are silent. This scan makes them loud.
    const GeometryUtils::FSkinWeightCoverage Coverage = GeometryUtils::ScanSkinWeightCoverage(Target.Mesh);
    if (!Coverage.bHasProfile && !bAllowPartialSkinning)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_SKIN_WEIGHTS,
            FString::Printf(TEXT("'%s' carries no skin weights. Run geometry.bind_skin_weights on it first (after all geometry edits)."),
                *ActorName));
        return true;
    }
    if (Coverage.UnweightedCount > 0 && !bAllowPartialSkinning)
    {
        Ctx.SendError(ErrorCodes::ERR_SKIN_WEIGHTS_INCOMPLETE,
            FString::Printf(TEXT("%d of %d vertices on '%s' carry no influence - the skin binding predates the current geometry. Re-run geometry.bind_skin_weights now that the mesh is final."),
                Coverage.UnweightedCount, Coverage.VertexCount, *ActorName));
        return true;
    }

    if (!GeometryUtils::IsMemoryPressureSafe())
    {
        Ctx.SendError(ErrorCodes::ERR_MEMORY_PRESSURE,
            TEXT("Insufficient memory headroom to bake a SkeletalMesh asset"));
        return true;
    }

    USkeletalMesh* BakedMesh = nullptr;
    bool bUpdatedInPlace = false;
    FSkeletalMeshCreateResult CreateResult;
    FSkeletalMeshMaterialCoverage MaterialCoverage;
    bool bCreatedThroughSeam = false;
    EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;

    FString SkeletonPath;
    FString SkeletonSource;
    USkeleton* Skeleton = SkeletalIOResolveSkeleton(Ctx, SkeletonPath, SkeletonSource);

    if (bOverwrite)
    {
        // In-place write-back. CopyMeshToSkeletalMesh takes an EXISTING USkeletalMesh and
        // rewrites one LOD's MeshDescription under its own transaction + Modify(), leaving the
        // asset's material slots alone. That is the whole point of this branch: it preserves
        // existing bindings while the create seam repairs missing/null slots after its build.
        BakedMesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath);
        if (!BakedMesh)
        {
            Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                FString::Printf(TEXT("overwrite:true but no SkeletalMesh exists at %s - omit overwrite to create it"),
                    *AssetPath));
            return true;
        }
        // Pre-empt the engine's own refusal (MeshAssetFunctions.cpp:989-994) with a message
        // that names the fix. Without this the engine returns Failure with the reason only in
        // the discarded Debug object and the caller sees a bare CONVERSION_FAILED.
        if (BakedMesh->GetPathName().StartsWith(TEXT("/Engine/")))
        {
            Ctx.SendError(ErrorCodes::ERR_SECURITY_VIOLATION,
                TEXT("Refusing to overwrite a built-in /Engine asset - asset.duplicate it into /Game first"));
            return true;
        }
        if (!Skeleton)
        {
            // Not an error on this branch: the target already knows its own skeleton.
            Skeleton = BakedMesh->GetSkeleton();
            if (Skeleton)
            {
                SkeletonPath = Skeleton->GetPathName();
                SkeletonSource = TEXT("targetAsset");
            }
        }

        FGeometryScriptCopyMeshToAssetOptions WriteOptions;
        WriteOptions.bEnableRecomputeNormals = bHasUVs;
        WriteOptions.bEnableRecomputeTangents = bHasUVs;
        // Materials/slots on the target are preserved; replacing them is a separate concern.
        WriteOptions.bReplaceMaterials = false;
        WriteOptions.bEmitTransaction = true;
        FString MismatchUnsupportedReason;
        if (!SkeletalIOApplyMismatchHandling(WriteOptions, MismatchHandling, MismatchUnsupportedReason))
        {
            Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION, MismatchUnsupportedReason);
            return true;
        }

        FGeometryScriptMeshWriteLOD WriteTarget;
        // Never true for skeletal: CopyMeshToSkeletalMesh rejects the HiRes source outright
        // (MeshAssetFunctions.cpp:981-985). Set explicitly so a future edit cannot flip it in.
        WriteTarget.bWriteHiResSource = false;
        WriteTarget.LODIndex = LodIndex;

        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToSkeletalMesh(
            Target.Mesh, BakedMesh, WriteOptions, WriteTarget, Outcome, nullptr);

        if (Outcome != EGeometryScriptOutcomePins::Success)
        {
            Ctx.SendError(ErrorCodes::ERR_CONVERSION_FAILED,
                FString::Printf(TEXT("Failed to write the dynamic mesh into the existing SkeletalMesh asset %s (lodIndex %d)"),
                    *AssetPath, LodIndex));
            return true;
        }
        if (!EnsureSkeletalMeshMaterialSlots(BakedMesh, MaterialCoverage))
        {
            Ctx.SendError(ErrorCodes::ERR_ASSET_DATA_INVALID,
                FString::Printf(TEXT("Updated SkeletalMesh at %s has no readable material coverage"),
                    *AssetPath));
            return true;
        }
        bUpdatedInPlace = true;
    }
    else
    {
        // The value-in/value-out seam owns the occupied-path guard, skeleton association,
        // single engine build, registry publication, provenance, and durable save. The response
        // reuses its measured package result rather than saving the created mesh a second time.
        if (!Skeleton)
        {
            Ctx.SendError(ErrorCodes::ERR_SKELETON_NOT_FOUND,
                TEXT("Creating a SkeletalMesh needs a skeleton: pass skeletonPath (a USkeleton) or skeletalMeshPath (a SkeletalMesh whose skeleton to reuse)."));
            return true;
        }

        FSkeletalMeshCreateSpec CreateSpec;
        CreateSpec.AssetPath = AssetPath;
        CreateSpec.Skeleton = Skeleton;
        CreateSpec.bRecomputeNormals = bHasUVs;
        CreateSpec.bRecomputeTangents = bHasUVs;
        CreateSpec.bSave = bSaveRequested;

        CreateResult = CreateSkeletalMesh(Target.Mesh, CreateSpec);
        if (!CreateResult.bSuccess)
        {
            Ctx.SendError(
                CreateResult.ErrorCode.IsEmpty()
                    ? FString(ErrorCodes::ERR_ASSET_CREATION_FAILED)
                    : CreateResult.ErrorCode,
                CreateResult.ErrorMessage.IsEmpty()
                    ? FString::Printf(TEXT("Failed to create SkeletalMesh asset at %s"), *AssetPath)
                    : CreateResult.ErrorMessage);
            return true;
        }

        BakedMesh = CreateResult.Asset;
        MaterialCoverage = CreateResult.MaterialCoverage;
        bUpdatedInPlace = CreateResult.bUpdatedInPlace;
        bCreatedThroughSeam = true;
    }

    // The create seam owns its one build, registry publication, and (when requested) durable
    // mesh/skeleton saves. The legacy overwrite branch still uses the shared real-save wrapper
    // below; both branches report persistence from a disk measurement rather than a helper's
    // return convention.
    FString BakedPackageName;
    int64 BakedSizeBytes = 0;
    bool bSavedToDisk = false;
    if (bCreatedThroughSeam)
    {
        BakedPackageName = CreateResult.PackageName;
        BakedSizeBytes = CreateResult.SizeBytes;
        bSavedToDisk = CreateResult.bSavedToDisk;
    }
    else if (bSaveRequested)
    {
        SaveAssetToDiskReportingPresence(BakedMesh, /*bForce=*/true, &BakedPackageName, &BakedSizeBytes);
        bSavedToDisk = BakedSizeBytes > 0;
    }
    else
    {
        BakedMesh->MarkPackageDirty();
        BakedPackageName = BakedMesh->GetOutermost() ? BakedMesh->GetOutermost()->GetName() : FString();
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    // exists/created track actual on-disk persistence, not the in-memory create.
    Result->SetBoolField(TEXT("exists"), bSavedToDisk);
    Result->SetBoolField(TEXT("created"), bSavedToDisk && !bUpdatedInPlace);
    Result->SetBoolField(TEXT("updated"), bUpdatedInPlace && bSavedToDisk);
    Result->SetBoolField(TEXT("overwrite"), bOverwrite);
    Result->SetStringField(TEXT("package"), BakedPackageName);
    Result->SetNumberField(TEXT("sizeBytes"), static_cast<double>(BakedSizeBytes));
    Result->SetStringField(TEXT("class"), BakedMesh->GetClass()->GetName());
    Result->SetStringField(TEXT("skeletonPath"), SkeletonPath);
    Result->SetStringField(TEXT("skeletonResolvedFrom"), SkeletonSource);
    Result->SetNumberField(TEXT("lodIndex"), bUpdatedInPlace ? LodIndex : 0);
    GeometryUtils::SetMeshCountFields(Target.Mesh, Result);
    // The measured skinning that was actually written, so `created: true` never implies a
    // skinned asset the caller did not verify. allowPartialSkinning is echoed too, because a
    // partial write is only legitimate when it was asked for in so many words.
    SkeletalIOAddCoverageFields(Coverage, Result);
    Result->SetBoolField(TEXT("allowPartialSkinning"), bAllowPartialSkinning);
    Result->SetBoolField(TEXT("recomputeNormals"), bHasUVs);
    Result->SetBoolField(TEXT("recomputeTangents"), bHasUVs);
    // Only meaningful (and only true) in-place: the create path builds a fresh slot array and
    // repairs it from the converted sections rather than preserving a target asset.
    Result->SetBoolField(TEXT("materialsPreserved"), bUpdatedInPlace);
    if (bUpdatedInPlace)
    {
        Result->SetStringField(TEXT("boneMismatchHandling"),
            MismatchToken.IsEmpty() ? FString(TEXT("DoNothing")) : MismatchToken);
    }
    Result->SetNumberField(TEXT("materialSlots"), MaterialCoverage.MaterialSlotCount);
    AddAssetSaveReport(Result, bSaveRequested, bSavedToDisk);
    if (bCreatedThroughSeam)
    {
        // A skeletal create can also touch its referenced skeleton. Name that package and expose
        // the same measured save state as model.compile so callers do not mistake the named mesh
        // for the only package this verb may have changed.
        Result->SetStringField(TEXT("skeletonPackage"), CreateResult.SkeletonPackageName);
        Result->SetBoolField(TEXT("skeletonSavedToDisk"),
            IsAssetSaveStateDurable(CreateResult.SkeletonSaveState));
        Result->SetStringField(TEXT("skeletonSaveState"),
            AssetSaveStateToWire(CreateResult.SkeletonSaveState));
    }
    Ctx.SendSuccess(bUpdatedInPlace
            ? TEXT("Existing SkeletalMesh updated in place from DynamicMesh")
            : TEXT("SkeletalMesh created from DynamicMesh"),
        Result);
    return true;
}
