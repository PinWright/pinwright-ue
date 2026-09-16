// Copyright (c) 2026 Alexander Penkin. MIT License.

// SkinAuditHandler.cpp - skeleton.audit_skin_weights.
//
// The first numeric GATE in the skeleton namespace: it returns a verdict rather than a
// description. See SkinAuditAnalysis.h for what each check measures, which thresholds are
// derived versus caller-supplied, and what the audit cannot see.
//
// WHY THIS EXISTS ALONGSIDE skeleton.describe_skin_weights
//
// describe_skin_weights reads named skin-weight PROFILES. A mesh with no authored profile -
// every FBX import, and every mesh built by GeometryScript's
// create_new_skeletal_mesh_asset_from_mesh - has none, so that verb answers profileCount: 0
// with an empty array and a success status, having measured nothing at all about the skinning
// the mesh renders with. This verb reads the BASE skinning in FSkelMeshSection::SoftVertices,
// resolves every influence's SECTION-LOCAL bone slot through FSkelMeshSection::BoneMap into a
// real reference-skeleton bone, and reports a verdict per check.
//
// NO POSE, NO WORLD, NO RHI. Everything here is asset data, so the verb is safe to run on a
// mesh that is not placed in any level and cannot dirty anything.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Animation/SkinAuditAnalysis.h"
#include "Audit/AuditFramework.h" // the shared audit verdict
#include "Utils/JsonBuilders.h"

#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/RendererSettings.h"
// FSkeletalMeshLODInfo is only forward-declared through Engine/SkeletalMesh.h on UE 5.4;
// SkinAuditResolveInfluenceLimit dereferences it to read BuildSettings.
#include "Engine/SkinnedAssetCommon.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshTypes.h"
#include "GPUSkinPublicDefs.h"

// Names from PinWrightSkinAudit are qualified throughout rather than pulled in with a
// using-directive: this module builds with Unity enabled, where a file-scope using-directive
// leaks into every translation unit merged after it in the same blob.
namespace
{
    // Per-file unique name, matching the LoadSkeletalMeshFromPathSM / ...Morph / ...Phys
    // convention that keeps these file-local helpers from colliding under Unity.
    UObject* LoadAssetFromPathAudit(const FString& MeshPath, FString& OutError)
    {
        OutError.Reset();
        if (MeshPath.IsEmpty())
        {
            OutError = TEXT("Skeletal mesh path is required");
            return nullptr;
        }

        UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *MeshPath);
        if (!Asset)
        {
            OutError = FString::Printf(TEXT("Failed to load skeletal mesh: %s"), *MeshPath);
            return nullptr;
        }

        return Asset;
    }

    // Reference-pose transforms accumulated into component space. Parents always precede
    // children in an FReferenceSkeleton, so one forward pass is sufficient and correct.
    void SkinAuditBuildRefPoseComponentSpace(const FReferenceSkeleton& RefSkeleton, TArray<FTransform>& OutTransforms)
    {
        const TArray<FTransform>& LocalPose = RefSkeleton.GetRefBonePose();
        OutTransforms.SetNum(LocalPose.Num());
        for (int32 BoneIndex = 0; BoneIndex < LocalPose.Num(); ++BoneIndex)
        {
            const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
            OutTransforms[BoneIndex] = (ParentIndex >= 0 && ParentIndex < BoneIndex)
                ? LocalPose[BoneIndex] * OutTransforms[ParentIndex]
                : LocalPose[BoneIndex];
        }
    }

    // One segment per (bone, child) pair, plus a degenerate point segment for every leaf. A
    // bone with several children therefore contributes several segments and the reach pass
    // takes the minimum, which is the right answer for a pelvis or a clavicle whose skinned
    // volume spans more than one child direction.
    void SkinAuditBuildBoneSegments(const FReferenceSkeleton& RefSkeleton,
        const TArray<FTransform>& ComponentSpace, TArray<PinWrightSkinAudit::FBoneSegment>& OutSegments)
    {
        const int32 NumBones = ComponentSpace.Num();
        TArray<bool> HasChild;
        HasChild.Init(false, NumBones);

        for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
        {
            const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
            if (ParentIndex < 0 || ParentIndex >= NumBones)
            {
                continue;
            }
            HasChild[ParentIndex] = true;
            PinWrightSkinAudit::FBoneSegment Segment;
            Segment.BoneIndex = ParentIndex;
            Segment.Start = FVector3f(ComponentSpace[ParentIndex].GetLocation());
            Segment.End = FVector3f(ComponentSpace[BoneIndex].GetLocation());
            OutSegments.Add(Segment);
        }

        for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
        {
            if (HasChild[BoneIndex])
            {
                continue;
            }
            PinWrightSkinAudit::FBoneSegment Segment;
            Segment.BoneIndex = BoneIndex;
            Segment.Start = FVector3f(ComponentSpace[BoneIndex].GetLocation());
            Segment.End = Segment.Start;
            OutSegments.Add(Segment);
        }
    }

    // The effective per-vertex influence limit, resolved exactly as
    // FGPUBaseSkinVertexFactory::GetBoneInfluenceLimitForAsset does
    // (Engine/Private/GPUSkinVertexFactory.cpp): the LOD's own BuildSettings value wins, else
    // the project's DefaultBoneInfluenceLimit, else MAX_TOTAL_INFLUENCES. Replicated rather
    // than called so this file does not pull GPUSkinVertexFactory.h (and its RHI surface) in
    // for three lines. OutSource names which rule fired, because "12 influences is fine" and
    // "12 influences is fine BECAUSE NOBODY SET A LIMIT" are different answers and the caller
    // must be able to tell them apart.
    int32 SkinAuditResolveInfluenceLimit(const USkeletalMesh& Mesh, int32 LODIndex, FString& OutSource)
    {
        if (const FSkeletalMeshLODInfo* LODInfo = Mesh.GetLODInfo(LODIndex))
        {
            if (LODInfo->BuildSettings.BoneInfluenceLimit > 0)
            {
                OutSource = TEXT("asset");
                return LODInfo->BuildSettings.BoneInfluenceLimit;
            }
        }
        if (const URendererSettings* Settings = GetDefault<URendererSettings>())
        {
            const int32 ProjectLimit = Settings->DefaultBoneInfluenceLimit.GetValue();
            if (ProjectLimit > 0)
            {
                OutSource = TEXT("project");
                return ProjectLimit;
            }
        }
        OutSource = TEXT("engineMax");
        return MAX_TOTAL_INFLUENCES;
    }

    TSharedPtr<FJsonObject> SkinAuditMakeCheck(const TCHAR* Name, const TCHAR* Status)
    {
        TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
        Check->SetStringField(TEXT("name"), Name);
        Check->SetStringField(TEXT("status"), Status);
        return Check;
    }

    FString SkinAuditBoneName(const FReferenceSkeleton& RefSkeleton, int32 BoneIndex)
    {
        if (BoneIndex >= 0 && BoneIndex < RefSkeleton.GetNum())
        {
            return RefSkeleton.GetBoneName(BoneIndex).ToString();
        }
        return FString::Printf(TEXT("<bone %d>"), BoneIndex);
    }
}

REGISTER_RPC_HANDLER("skeleton.audit_skin_weights", "skeleton",
    "Numeric pass/fail gate over a SkeletalMesh's BASE skinning (the section soft-vertices the renderer actually uses), not over named skin-weight profiles the way skeleton.describe_skin_weights does - a mesh with no authored profile makes that verb report profileCount 0 and measure nothing. Runs four exact checks per LOD with no pose, no world and no rendering: vertices with zero influences (they collapse to the component origin), influence weights that do not sum to 1.0, per-vertex influence counts above the LOD's own configured bone-influence limit, and coincident bind-pose vertices whose influences disagree (a UV or material seam that will visibly split under any pose). Two further checks report rather than gate by default. boneCoverage names every reference-skeleton bone that no vertex is weighted to - the defect that makes a hand-authored animation move nothing, invisible to every other check - and gives a per-bone influenced-vertex and rigidly-bound-vertex count, so a deliberately rigid part is directly readable instead of inferred; pass requireAllBonesInfluenced to gate on it. influenceReach measures every influence's bind-pose distance to its bone and reports the distribution plus the worst offenders by name; it gates only when you pass maxReachDistance, because no defensible default exists. A vertex rigidly bound to exactly one bone that is also the nearest bone to it is reported in a separate rigidBind bucket rather than judged, so a prop or weapon bound entirely to one attachment bone stops reading as a reach error while a rigid bind to the WRONG bone still fails. Every check reports status pass, fail, reported or unmeasured, and the top-level pass is false whenever any check could not be measured.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletalMeshPath", "path", "Path to the skeletal mesh"),
        RPC_PARAM_DEF("lodIndex", "integer", "LOD to audit. Defaults to 0 because a generated LOD legitimately carries different influence counts (reduction merges bones) and a verdict pooled across LODs is noise", "0"),
        RPC_PARAM_OPT("maxInfluences", "number", "Override the per-vertex influence limit. Omit to resolve it from the LOD's BuildSettings.BoneInfluenceLimit, then the project's DefaultBoneInfluenceLimit, then the engine maximum of 12"),
        RPC_PARAM_DEF("coincidentTolerance", "number", "Bind-pose distance in world centimetres under which two vertices count as the same point for the seam-split check", "0.01"),
        RPC_PARAM_DEF("maxSplitCoefficient", "number", "Largest acceptable weight disagreement between coincident vertices. Multiply by a bone's travel to get the world-unit seam split it permits", "0.001"),
        RPC_PARAM_DEF("checkReach", "bool", "Run the influence-reach measurement. It is the only check that is not linear in vertex count", "true"),
        RPC_PARAM_OPT("maxReachDistance", "number", "Turn reach into a gate: fail when any GATED influence's bind-pose distance to its bone exceeds this many median bone lengths. Rigid nearest-bone influences are reported in rigidBind and never judged. Omit to report the distribution without a verdict"),
        RPC_PARAM_DEF("requireAllBonesInfluenced", "bool", "Turn boneCoverage into a gate: fail when any reference-skeleton bone has no vertex weighted to it. Off by default because real rigs carry uninfluenced bones on purpose (IK targets, attachment bones, twist drivers, a motion-only root), so failing on their existence would be a false alarm rather than a finding", "false"),
        RPC_PARAM_DEF("minInfluenceWeight", "number", "The weight at or above which an influence counts as meaningful. Used by the reach check and by boneCoverage's influenced/rigid vertex counts - one epsilon, so the two cannot disagree about which influences matter", "0.01"),
        RPC_PARAM_DEF("reachVertexBudget", "number", "Vertex budget for the reach check; above it the vertex array is strided uniformly and both counts are reported", "50000"),
        RPC_PARAM_DEF("maxReported", "number", "Worst offenders listed per check", "8")
    ))
{
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    if (SkeletalMeshPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletalMeshPath is required"));
        return true;
    }

    FString LoadError;
    UObject* Asset = LoadAssetFromPathAudit(SkeletalMeshPath, LoadError);
    if (!Asset)
    {
        Ctx.SendError(TEXT("MESH_NOT_FOUND"), LoadError);
        return true;
    }

    if (USkeleton* Skeleton = Cast<USkeleton>(Asset))
    {
        const USkeletalMesh* PreviewMesh = static_cast<const USkeleton*>(Skeleton)->GetPreviewMesh();
        const FString PreviewPath = PreviewMesh ? PreviewMesh->GetPathName() : TEXT("(none)");
        const FString Recovery = PreviewMesh
            ? FString::Printf(TEXT("Retry with skeletalMeshPath set to '%s'."), *PreviewPath)
            : TEXT("Configure a preview mesh first with skeleton.set_preview_mesh, then retry with skeletalMeshPath set to that mesh path.");
        Ctx.SendError(TEXT("INVALID_ASSET_TYPE"),
            FString::Printf(
                TEXT("Asset '%s' is a USkeleton, not a USkeletalMesh. Its preview mesh is %s. %s"),
                *SkeletalMeshPath, *PreviewPath, *Recovery));
        return true;
    }

    USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset);
    if (!Mesh)
    {
        const FString ActualType = Asset->GetClass() ? Asset->GetClass()->GetName() : TEXT("UObject");
        Ctx.SendError(TEXT("INVALID_ASSET_TYPE"),
            FString::Printf(TEXT("Asset '%s' is a %s, not a USkeletalMesh. Pass a USkeletalMesh asset path."),
                *SkeletalMeshPath, *ActualType));
        return true;
    }

    const FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
    const int32 NumLODs = ImportedModel ? ImportedModel->LODModels.Num() : 0;
    if (NumLODs == 0)
    {
        // Not a pass with zero findings: there is nothing to read. A cooked-only mesh has no
        // imported model in the editor either, and reporting that as a clean audit is exactly
        // the false confidence this verb exists to prevent.
        Ctx.SendError(TEXT("NO_LOD_MODELS"),
            TEXT("Mesh has no imported LOD models, so its base skinning cannot be read. This audit needs editor "
                 "source data; a cooked-only or not-yet-built mesh cannot be audited."));
        return true;
    }

    const int32 LODIndex = Ctx.GetInt(TEXT("lodIndex"), 0);
    if (LODIndex < 0 || LODIndex >= NumLODs)
    {
        Ctx.SendError(TEXT("INVALID_LOD"),
            FString::Printf(TEXT("LOD index %d out of range (mesh has %d LODs)"), LODIndex, NumLODs));
        return true;
    }

    const double CoincidentTolerance = Ctx.GetNumber(TEXT("coincidentTolerance"),
        PinWrightSkinAudit::DefaultCoincidentTolerance);
    if (!(CoincidentTolerance > 0.0))
    {
        // Zero would make the hash grid divide by zero, and the seam check would silently
        // find nothing rather than everything.
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("coincidentTolerance must be greater than zero"));
        return true;
    }

    const double MaxSplitCoefficient = Ctx.GetNumber(TEXT("maxSplitCoefficient"),
        PinWrightSkinAudit::DefaultSplitCoefficientTolerance);
    if (MaxSplitCoefficient < 0.0)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("maxSplitCoefficient must not be negative"));
        return true;
    }

    const double MinInfluenceWeight = Ctx.GetNumber(TEXT("minInfluenceWeight"),
        PinWrightSkinAudit::DefaultMinReachWeight);
    if (MinInfluenceWeight < 0.0 || MinInfluenceWeight > 1.0)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("minInfluenceWeight must be in [0, 1]"));
        return true;
    }

    const int32 ReachVertexBudget = Ctx.GetInt(TEXT("reachVertexBudget"),
        PinWrightSkinAudit::DefaultReachVertexBudget);
    if (ReachVertexBudget <= 0)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("reachVertexBudget must be greater than zero"));
        return true;
    }

    const int32 MaxReported = FMath::Max(0, Ctx.GetInt(TEXT("maxReported"), 8));
    const bool bCheckReach = Ctx.GetBool(TEXT("checkReach"), true);
    const bool bRequireAllBonesInfluenced = Ctx.GetBool(TEXT("requireAllBonesInfluenced"), false);

    // maxReachDistance turns the reach measurement into a gate. Absent means "report only",
    // which is a deliberately different state from "threshold 0".
    const bool bHasReachThreshold = Ctx.GetRawPayload().IsValid()
        && Ctx.GetRawPayload()->HasField(TEXT("maxReachDistance"));
    const double MaxReachDistance = Ctx.GetNumber(TEXT("maxReachDistance"), 0.0);
    if (bHasReachThreshold && !(MaxReachDistance > 0.0))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("maxReachDistance must be greater than zero; omit it to report the reach distribution without a verdict"));
        return true;
    }

    int32 InfluenceLimitOverride = 0;
    FString InfluenceLimitSource;
    if (Ctx.GetRawPayload().IsValid() && Ctx.GetRawPayload()->HasField(TEXT("maxInfluences")))
    {
        InfluenceLimitOverride = Ctx.GetInt(TEXT("maxInfluences"), 0);
        if (InfluenceLimitOverride < 1 || InfluenceLimitOverride > MAX_TOTAL_INFLUENCES)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                FString::Printf(TEXT("maxInfluences must be in [1, %d]"), MAX_TOTAL_INFLUENCES));
            return true;
        }
        InfluenceLimitSource = TEXT("caller");
    }
    const int32 InfluenceLimit = InfluenceLimitOverride > 0
        ? InfluenceLimitOverride
        : SkinAuditResolveInfluenceLimit(*Mesh, LODIndex, InfluenceLimitSource);

    // ---- Flatten the LOD's base skinning, resolving section-local bone slots ----

    const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[LODIndex];
    const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
    const int32 NumBones = RefSkeleton.GetNum();

    TArray<PinWrightSkinAudit::FAuditVertex> Vertices;
    Vertices.Reserve(LODModel.NumVertices);

    TArray<TSharedPtr<FJsonValue>> SectionValues;
    int32 ClothSectionCount = 0;
    int32 DisabledSectionCount = 0;
    int32 UnmappedInfluenceCount = 0;

    for (int32 SectionIndex = 0; SectionIndex < LODModel.Sections.Num(); ++SectionIndex)
    {
        const FSkelMeshSection& Section = LODModel.Sections[SectionIndex];
        if (Section.HasClothingData())
        {
            ++ClothSectionCount;
        }
        if (Section.bDisabled)
        {
            ++DisabledSectionCount;
        }

        TSharedPtr<FJsonObject> SectionObject = MakeShared<FJsonObject>();
        SectionObject->SetNumberField(TEXT("index"), SectionIndex);
        SectionObject->SetNumberField(TEXT("vertexCount"), Section.SoftVertices.Num());
        SectionObject->SetNumberField(TEXT("boneMapSize"), Section.BoneMap.Num());
        SectionObject->SetNumberField(TEXT("maxBoneInfluences"), Section.MaxBoneInfluences);
        SectionObject->SetBoolField(TEXT("cloth"), Section.HasClothingData());
        SectionObject->SetBoolField(TEXT("disabled"), Section.bDisabled);
        SectionValues.Add(MakeShared<FJsonValueObject>(SectionObject));

        for (const FSoftSkinVertex& SoftVertex : Section.SoftVertices)
        {
            PinWrightSkinAudit::FAuditVertex Vertex;
            Vertex.Position = SoftVertex.Position;
            Vertex.SectionIndex = SectionIndex;

            for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
            {
                const uint16 RawWeight = SoftVertex.InfluenceWeights[Influence];
                if (RawWeight == 0)
                {
                    // Influences are packed largest-first and zero-padded, so the first zero
                    // ends the list.
                    break;
                }
                const int32 LocalBone = static_cast<int32>(SoftVertex.InfluenceBones[Influence]);
                if (!Section.BoneMap.IsValidIndex(LocalBone))
                {
                    // A section-local slot with no bone map entry. Counted and reported
                    // rather than silently resolved to bone 0, which would fabricate an
                    // influence on the root and quietly pass the reach check.
                    ++UnmappedInfluenceCount;
                    continue;
                }
                Vertex.BoneIndices[Vertex.InfluenceCount] = static_cast<int32>(Section.BoneMap[LocalBone]);
                Vertex.Weights[Vertex.InfluenceCount] =
                    static_cast<double>(RawWeight) / 65535.0;   // uint16 fixed point, see SkinWeightTransferUtils
                ++Vertex.InfluenceCount;
            }

            Vertices.Add(Vertex);
        }
    }

    // ---- Run the checks ----

    TArray<TSharedPtr<FJsonValue>> CheckValues;
    int32 FailedChecks = 0;
    int32 UnmeasuredChecks = 0;
    TArray<FString> Warnings;

    if (UnmappedInfluenceCount > 0)
    {
        Warnings.Add(FString::Printf(
            TEXT("%d influence slots referenced a section-local bone with no BoneMap entry and were dropped. "
                 "The mesh's section data is inconsistent; treat every count below as a lower bound."),
            UnmappedInfluenceCount));
    }

    PinWrightSkinAudit::FWeightSumStats WeightStats;
    PinWrightSkinAudit::AccumulateWeightSums(Vertices, PinWrightSkinAudit::DefaultWeightSumTolerance,
        MaxReported, WeightStats);

    const bool bAnyVertices = Vertices.Num() > 0;

    {
        // Zero-influence vertices. Exact: the correct count is zero, and there is nothing to
        // tune. A vertex with no influence is multiplied by no bone matrix and renders at the
        // component origin, which is the classic "the model exploded toward the feet" defect.
        TSharedPtr<FJsonObject> Check = SkinAuditMakeCheck(TEXT("zeroInfluence"),
            !bAnyVertices ? TEXT("unmeasured")
                          : (WeightStats.ZeroInfluenceCount == 0 ? TEXT("pass") : TEXT("fail")));
        if (!bAnyVertices)
        {
            Check->SetStringField(TEXT("reason"), TEXT("The LOD has no vertices, so nothing was measured."));
            ++UnmeasuredChecks;
        }
        else if (WeightStats.ZeroInfluenceCount != 0)
        {
            ++FailedChecks;
        }
        Check->SetNumberField(TEXT("vertexCount"), WeightStats.VertexCount);
        Check->SetNumberField(TEXT("zeroInfluenceVertices"), WeightStats.ZeroInfluenceCount);
        TArray<TSharedPtr<FJsonValue>> ZeroValues;
        for (int32 VertexIndex : WeightStats.ZeroInfluenceVertices)
        {
            ZeroValues.Add(MakeShared<FJsonValueNumber>(VertexIndex));
        }
        Check->SetArrayField(TEXT("offenders"), ZeroValues);
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    {
        // Weight sums. Exact within the uint16 quantization band; see
        // PinWrightSkinAudit::DefaultWeightSumTolerance for the derivation.
        const int32 Influenced = WeightStats.VertexCount - WeightStats.ZeroInfluenceCount;
        const bool bMeasured = Influenced > 0;
        TSharedPtr<FJsonObject> Check = SkinAuditMakeCheck(TEXT("weightSum"),
            !bMeasured ? TEXT("unmeasured")
                       : (WeightStats.UnnormalizedCount == 0 ? TEXT("pass") : TEXT("fail")));
        if (!bMeasured)
        {
            Check->SetStringField(TEXT("reason"),
                TEXT("No vertex carries any influence, so no weight sum exists to check."));
            ++UnmeasuredChecks;
        }
        else
        {
            if (WeightStats.UnnormalizedCount != 0)
            {
                ++FailedChecks;
            }
            Check->SetNumberField(TEXT("normalizedVertices"), WeightStats.NormalizedCount);
            Check->SetNumberField(TEXT("unnormalizedVertices"), WeightStats.UnnormalizedCount);
            Check->SetNumberField(TEXT("minWeightSum"), JsonBuilders::SanitizeFinite(WeightStats.MinWeightSum));
            Check->SetNumberField(TEXT("maxWeightSum"), JsonBuilders::SanitizeFinite(WeightStats.MaxWeightSum));
            Check->SetNumberField(TEXT("tolerance"), PinWrightSkinAudit::DefaultWeightSumTolerance);

            TArray<TSharedPtr<FJsonValue>> OffenderValues;
            for (const PinWrightSkinAudit::FWeightSumOffender& Offender : WeightStats.Offenders)
            {
                TSharedPtr<FJsonObject> OffenderObject = MakeShared<FJsonObject>();
                OffenderObject->SetNumberField(TEXT("vertexIndex"), Offender.VertexIndex);
                OffenderObject->SetNumberField(TEXT("weightSum"), JsonBuilders::SanitizeFinite(Offender.WeightSum));
                OffenderObject->SetNumberField(TEXT("influenceCount"), Offender.InfluenceCount);
                OffenderValues.Add(MakeShared<FJsonValueObject>(OffenderObject));
            }
            Check->SetArrayField(TEXT("offenders"), OffenderValues);
        }
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    {
        PinWrightSkinAudit::FInfluenceCountStats CountStats;
        PinWrightSkinAudit::AccumulateInfluenceCounts(Vertices, InfluenceLimit, MaxReported, CountStats);

        TSharedPtr<FJsonObject> Check = SkinAuditMakeCheck(TEXT("influenceCount"),
            !bAnyVertices ? TEXT("unmeasured")
                          : (CountStats.OverLimitCount == 0 ? TEXT("pass") : TEXT("fail")));
        if (!bAnyVertices)
        {
            Check->SetStringField(TEXT("reason"), TEXT("The LOD has no vertices, so nothing was measured."));
            ++UnmeasuredChecks;
        }
        else if (CountStats.OverLimitCount != 0)
        {
            ++FailedChecks;
        }
        Check->SetNumberField(TEXT("limit"), InfluenceLimit);
        // "12 influences is fine" and "12 influences is fine because nobody configured a
        // limit" are different answers; the source field is what separates them.
        Check->SetStringField(TEXT("limitSource"), InfluenceLimitSource);
        Check->SetNumberField(TEXT("maxInfluences"), CountStats.MaxInfluences);
        Check->SetNumberField(TEXT("overLimitVertices"), CountStats.OverLimitCount);

        TArray<TSharedPtr<FJsonValue>> HistogramValues;
        for (int32 Count = 0; Count <= MAX_TOTAL_INFLUENCES; ++Count)
        {
            HistogramValues.Add(MakeShared<FJsonValueNumber>(CountStats.Histogram[Count]));
        }
        Check->SetArrayField(TEXT("histogram"), HistogramValues);

        TArray<TSharedPtr<FJsonValue>> OverLimitValues;
        for (int32 VertexIndex : CountStats.OverLimitVertices)
        {
            OverLimitValues.Add(MakeShared<FJsonValueNumber>(VertexIndex));
        }
        Check->SetArrayField(TEXT("offenders"), OverLimitValues);
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    {
        PinWrightSkinAudit::FCoincidentStats CoincidentStats;
        PinWrightSkinAudit::FindCoincidentSplits(Vertices, CoincidentTolerance, MaxSplitCoefficient,
            MaxReported, CoincidentStats);

        // A mesh with no duplicated vertices makes this check VACUOUS, not passed. Reporting
        // "pass" there would tell a caller that seams were checked and found sound when in
        // fact there were no seams to check - and the same response would come back from a
        // mesh whose vertices failed to load.
        const bool bMeasured = CoincidentStats.CoincidentVertexCount > 0;
        TSharedPtr<FJsonObject> Check = SkinAuditMakeCheck(TEXT("coincidentSplit"),
            !bMeasured ? TEXT("unmeasured")
                       : (CoincidentStats.SplittingGroupCount == 0 ? TEXT("pass") : TEXT("fail")));
        if (!bMeasured)
        {
            Check->SetStringField(TEXT("reason"),
                TEXT("No two vertices share a bind position within coincidentTolerance, so there is no seam to "
                     "check. This is normal for a mesh with a single material and no UV splits."));
            ++UnmeasuredChecks;
        }
        else if (CoincidentStats.SplittingGroupCount != 0)
        {
            ++FailedChecks;
        }
        Check->SetNumberField(TEXT("coincidentVertices"), CoincidentStats.CoincidentVertexCount);
        Check->SetNumberField(TEXT("coincidentGroups"), CoincidentStats.GroupCount);
        Check->SetNumberField(TEXT("splittingGroups"), CoincidentStats.SplittingGroupCount);
        Check->SetNumberField(TEXT("maxSplitCoefficient"),
            JsonBuilders::SanitizeFinite(CoincidentStats.MaxSplitCoefficient));
        Check->SetNumberField(TEXT("tolerance"), MaxSplitCoefficient);

        TArray<TSharedPtr<FJsonValue>> GroupValues;
        for (const PinWrightSkinAudit::FCoincidentGroup& Group : CoincidentStats.Groups)
        {
            TSharedPtr<FJsonObject> GroupObject = MakeShared<FJsonObject>();
            GroupObject->SetObjectField(TEXT("position"),
                JsonBuilders::BuildVectorJson(FVector(Group.Position)));
            GroupObject->SetNumberField(TEXT("splitCoefficient"),
                JsonBuilders::SanitizeFinite(Group.SplitCoefficient));

            TArray<TSharedPtr<FJsonValue>> VertexValues;
            for (int32 VertexIndex : Group.VertexIndices)
            {
                VertexValues.Add(MakeShared<FJsonValueNumber>(VertexIndex));
            }
            GroupObject->SetArrayField(TEXT("vertexIndices"), VertexValues);

            TArray<TSharedPtr<FJsonValue>> BoneValues;
            for (int32 BoneIndex : Group.DisagreeingBones)
            {
                BoneValues.Add(MakeShared<FJsonValueString>(SkinAuditBoneName(RefSkeleton, BoneIndex)));
            }
            GroupObject->SetArrayField(TEXT("disagreeingBones"), BoneValues);
            GroupValues.Add(MakeShared<FJsonValueObject>(GroupObject));
        }
        Check->SetArrayField(TEXT("offenders"), GroupValues);
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    {
        // Bone coverage. A bone no vertex is weighted to is INVISIBLE to every check above:
        // the weights sum, nothing is zero-influenced, no seam splits, and the influence-count
        // histogram cannot name a bone. It is also the exact reason a hand-authored animation
        // moves nothing - the curve drives a bone that owns no geometry - so it is named here
        // rather than left to be inferred.
        PinWrightSkinAudit::FBoneCoverage Coverage;
        PinWrightSkinAudit::AccumulateBoneCoverage(Vertices, NumBones, MinInfluenceWeight, Coverage);

        // Zero bones or zero vertices is not a clean coverage report, it is no report: with
        // nothing to weight, "every bone is covered" and "nothing was looked at" are the same
        // arithmetic.
        const bool bMeasured = NumBones > 0 && bAnyVertices;
        const TCHAR* Status = TEXT("unmeasured");
        bool bFailed = false;
        if (bMeasured)
        {
            if (!bRequireAllBonesInfluenced)
            {
                // Reported, not gated. Uninfluenced bones are normal and deliberate on real
                // rigs (IK targets, attachment bones, twist drivers, a motion-only root), so a
                // default gate here would manufacture false alarms rather than find defects.
                Status = TEXT("reported");
            }
            else
            {
                bFailed = Coverage.BonesWithNoInfluence.Num() > 0;
                Status = bFailed ? TEXT("fail") : TEXT("pass");
            }
        }

        TSharedPtr<FJsonObject> Check = SkinAuditMakeCheck(TEXT("boneCoverage"), Status);
        if (!bMeasured)
        {
            Check->SetStringField(TEXT("reason"),
                TEXT("The mesh has no reference-skeleton bones, or the LOD has no vertices, so there is no "
                     "coverage to measure. Every bone would read as uninfluenced for want of geometry rather "
                     "than for want of weighting."));
            ++UnmeasuredChecks;
        }
        else if (bFailed)
        {
            ++FailedChecks;
        }
        Check->SetNumberField(TEXT("boneCount"), NumBones);
        Check->SetNumberField(TEXT("influencedBoneCount"), Coverage.InfluencedBoneCount);
        Check->SetNumberField(TEXT("bonesWithNoInfluenceCount"), Coverage.BonesWithNoInfluence.Num());
        Check->SetNumberField(TEXT("weightEpsilon"), JsonBuilders::SanitizeFinite(MinInfluenceWeight));
        Check->SetNumberField(TEXT("rigidVertexCount"), Coverage.RigidVertexCount);

        // Emitted ALWAYS, empty array included. An absent field cannot be told apart from a
        // build that never looked, which is the whole defect this check closes.
        TArray<TSharedPtr<FJsonValue>> UninfluencedValues;
        for (int32 BoneIndex : Coverage.BonesWithNoInfluence)
        {
            UninfluencedValues.Add(MakeShared<FJsonValueString>(SkinAuditBoneName(RefSkeleton, BoneIndex)));
        }
        Check->SetArrayField(TEXT("bonesWithNoInfluence"), UninfluencedValues);

        // One row per bone that owns geometry, worst-first by rigid count. Uncapped on
        // purpose: a per-bone table clipped to the top N cannot answer "which bone is this
        // part bound to", because the bone in question is as likely to rank fortieth as first.
        TArray<int32> InfluencedBones;
        InfluencedBones.Reserve(Coverage.InfluencedBoneCount);
        for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
        {
            if (Coverage.InfluencedVertexCountPerBone[BoneIndex] > 0)
            {
                InfluencedBones.Add(BoneIndex);
            }
        }
        InfluencedBones.Sort([&Coverage](int32 Lhs, int32 Rhs)
        {
            if (Coverage.RigidVertexCountPerBone[Lhs] != Coverage.RigidVertexCountPerBone[Rhs])
            {
                return Coverage.RigidVertexCountPerBone[Lhs] > Coverage.RigidVertexCountPerBone[Rhs];
            }
            if (Coverage.InfluencedVertexCountPerBone[Lhs] != Coverage.InfluencedVertexCountPerBone[Rhs])
            {
                return Coverage.InfluencedVertexCountPerBone[Lhs] > Coverage.InfluencedVertexCountPerBone[Rhs];
            }
            return Lhs < Rhs;
        });

        TArray<TSharedPtr<FJsonValue>> BoneValues;
        for (int32 BoneIndex : InfluencedBones)
        {
            TSharedPtr<FJsonObject> BoneObject = MakeShared<FJsonObject>();
            BoneObject->SetStringField(TEXT("bone"), SkinAuditBoneName(RefSkeleton, BoneIndex));
            BoneObject->SetNumberField(TEXT("boneIndex"), BoneIndex);
            BoneObject->SetNumberField(TEXT("influencedVertices"),
                Coverage.InfluencedVertexCountPerBone[BoneIndex]);
            // Vertices whose ONLY meaningful influence is this bone. A part with
            // rigidVertices == influencedVertices is bound to this bone outright.
            BoneObject->SetNumberField(TEXT("rigidVertices"),
                Coverage.RigidVertexCountPerBone[BoneIndex]);
            BoneValues.Add(MakeShared<FJsonValueObject>(BoneObject));
        }
        Check->SetArrayField(TEXT("bones"), BoneValues);
        Check->SetStringField(TEXT("boneIndexSpace"), TEXT("referenceSkeleton"));

        if (!bRequireAllBonesInfluenced && bMeasured)
        {
            Check->SetStringField(TEXT("note"),
                TEXT("Reported without a verdict: a bone with no influenced vertex is often deliberate (IK "
                     "targets, attachment bones, twist drivers, a motion-only root). Read bonesWithNoInfluence "
                     "against the bones your animation drives - a driven bone listed there moves nothing. Pass "
                     "requireAllBonesInfluenced:true to gate on it. A bone whose rigidVertices equals its "
                     "influencedVertices owns that geometry outright, which is what a deliberately rigid part "
                     "looks like."));
        }
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    if (bCheckReach)
    {
        TArray<FTransform> ComponentSpace;
        SkinAuditBuildRefPoseComponentSpace(RefSkeleton, ComponentSpace);
        TArray<PinWrightSkinAudit::FBoneSegment> Segments;
        SkinAuditBuildBoneSegments(RefSkeleton, ComponentSpace, Segments);

        PinWrightSkinAudit::FReachStats ReachStats;
        PinWrightSkinAudit::ComputeReach(Vertices, Segments, MinInfluenceWeight, ReachVertexBudget,
            MaxReported, ReachStats);

        const TCHAR* Status = TEXT("unmeasured");
        bool bFailed = false;
        if (ReachStats.bMeasurable)
        {
            if (!bHasReachThreshold)
            {
                // Reported, not gated. There is no defensible default here - see the header -
                // so the check contributes to neither the pass nor the fail count.
                Status = TEXT("reported");
            }
            else
            {
                // Only the gated bucket can produce a verdict. With it empty the answer is a
                // measured pass, not a silent one: gatedInfluences 0 sits in the payload
                // beside a populated rigidBind block saying exactly what was declined.
                bFailed = ReachStats.GatedInfluences > 0
                    && ReachStats.NormalizedDistancePercentiles[4] > MaxReachDistance;
                Status = bFailed ? TEXT("fail") : TEXT("pass");
            }
        }

        TSharedPtr<FJsonObject> Check = SkinAuditMakeCheck(TEXT("influenceReach"), Status);
        if (!ReachStats.bMeasurable)
        {
            Check->SetStringField(TEXT("reason"),
                TEXT("The skeleton has no measurable bone length, or no influence met minInfluenceWeight, so "
                     "reach has no scale to normalize against. Every reach number would be meaningless."));
            ++UnmeasuredChecks;
        }
        else
        {
            if (bFailed)
            {
                ++FailedChecks;
            }
            Check->SetNumberField(TEXT("sampledVertices"), ReachStats.SampledVertexCount);
            Check->SetNumberField(TEXT("totalVertices"), ReachStats.TotalVertexCount);
            Check->SetBoolField(TEXT("exhaustive"), ReachStats.SampledVertexCount >= ReachStats.TotalVertexCount);
            // The two buckets partition influencesConsidered exactly, so an influence that
            // fell out of both is arithmetically visible rather than quietly absent.
            Check->SetNumberField(TEXT("influencesConsidered"), ReachStats.InfluencesConsidered);
            Check->SetNumberField(TEXT("gatedInfluences"), ReachStats.GatedInfluences);
            Check->SetNumberField(TEXT("medianBoneLength"),
                JsonBuilders::SanitizeFinite(ReachStats.MedianSegmentLength));
            if (bHasReachThreshold)
            {
                Check->SetNumberField(TEXT("maxReachDistance"), MaxReachDistance);
            }

            static const TCHAR* PercentileNames[5] = { TEXT("p50"), TEXT("p90"), TEXT("p99"), TEXT("p999"), TEXT("max") };
            const auto BuildPercentiles = [](const double (&Values)[5]) -> TSharedPtr<FJsonObject>
            {
                TSharedPtr<FJsonObject> Percentiles = MakeShared<FJsonObject>();
                for (int32 Index = 0; Index < 5; ++Index)
                {
                    Percentiles->SetNumberField(PercentileNames[Index],
                        JsonBuilders::SanitizeFinite(Values[Index]));
                }
                return Percentiles;
            };
            const auto BuildOffenders = [&RefSkeleton](const TArray<PinWrightSkinAudit::FReachOffender>& Source)
            {
                TArray<TSharedPtr<FJsonValue>> OffenderValues;
                for (const PinWrightSkinAudit::FReachOffender& Offender : Source)
                {
                    TSharedPtr<FJsonObject> OffenderObject = MakeShared<FJsonObject>();
                    OffenderObject->SetNumberField(TEXT("vertexIndex"), Offender.VertexIndex);
                    OffenderObject->SetStringField(TEXT("bone"), SkinAuditBoneName(RefSkeleton, Offender.BoneIndex));
                    OffenderObject->SetNumberField(TEXT("weight"), JsonBuilders::SanitizeFinite(Offender.Weight));
                    OffenderObject->SetNumberField(TEXT("distance"), JsonBuilders::SanitizeFinite(Offender.Distance));
                    OffenderObject->SetNumberField(TEXT("normalizedDistance"),
                        JsonBuilders::SanitizeFinite(Offender.NormalizedDistance));
                    OffenderObject->SetNumberField(TEXT("closerBones"), Offender.CloserBoneCount);
                    OffenderValues.Add(MakeShared<FJsonValueObject>(OffenderObject));
                }
                return OffenderValues;
            };

            Check->SetObjectField(TEXT("normalizedDistance"),
                BuildPercentiles(ReachStats.NormalizedDistancePercentiles));
            Check->SetArrayField(TEXT("offenders"), BuildOffenders(ReachStats.Offenders));

            // The not-applicable bucket, measured and published rather than dropped. Silently
            // excluding it would be indistinguishable from a threshold quietly raised until
            // the alarm stopped, which is the failure this split exists to avoid.
            TSharedPtr<FJsonObject> RigidBind = MakeShared<FJsonObject>();
            RigidBind->SetNumberField(TEXT("influences"), ReachStats.RigidBindInfluences);
            RigidBind->SetNumberField(TEXT("vertices"), ReachStats.RigidBindVertices);
            RigidBind->SetObjectField(TEXT("normalizedDistance"),
                BuildPercentiles(ReachStats.RigidBindNormalizedDistancePercentiles));
            RigidBind->SetArrayField(TEXT("offenders"), BuildOffenders(ReachStats.RigidBindOffenders));
            RigidBind->SetStringField(TEXT("reason"),
                TEXT("Not applicable to a reach verdict: each of these vertices has exactly one influence at or "
                     "above minInfluenceWeight AND no bone lies strictly closer to it than the bone it names. "
                     "That is what a deliberately rigid part looks like - a prop, a weapon, an eyeball, a cape "
                     "on one attachment bone - and its distance to that bone is set by the part's silhouette, "
                     "not by a binding error. A rigid vertex bound to a bone something else is nearer to stays "
                     "in the gated bucket, so a wrong-bone bind is still caught however far away it is."));
            Check->SetObjectField(TEXT("rigidBind"), RigidBind);

            if (!bHasReachThreshold)
            {
                Check->SetStringField(TEXT("note"),
                    TEXT("Reported without a verdict because no threshold exists that is defensible across "
                         "skeletons. To gate on it, run this verb on a mesh you know is correctly skinned to the "
                         "same skeleton, take normalizedDistance.p999, and pass roughly twice that as "
                         "maxReachDistance. normalizedDistance covers the GATED influences only; rigidBind "
                         "carries the rest."));
            }
            else if (ReachStats.GatedInfluences == 0)
            {
                Check->SetStringField(TEXT("note"),
                    TEXT("Every measured influence was a rigid nearest-bone bind, so the threshold had nothing "
                         "to judge and this pass is vacuous rather than reassuring. Read rigidBind for what was "
                         "measured; a mesh that is entirely rigid parts is the expected shape of this result."));
            }
        }
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    // ---- Response ----

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("audit"), TEXT("skinWeights"));
    Result->SetStringField(TEXT("skeletalMeshPath"), SkeletalMeshPath);
    Result->SetStringField(TEXT("source"), TEXT("baseSkinning"));
    Result->SetNumberField(TEXT("lodIndex"), LODIndex);
    Result->SetNumberField(TEXT("lodCount"), NumLODs);
    Result->SetNumberField(TEXT("vertexCount"), Vertices.Num());
    Result->SetNumberField(TEXT("boneCount"), NumBones);
    Result->SetNumberField(TEXT("sectionCount"), LODModel.Sections.Num());
    Result->SetNumberField(TEXT("clothSectionCount"), ClothSectionCount);
    Result->SetNumberField(TEXT("disabledSectionCount"), DisabledSectionCount);
    Result->SetArrayField(TEXT("sections"), SectionValues);
    Result->SetArrayField(TEXT("checks"), CheckValues);
    Result->SetNumberField(TEXT("failedChecks"), FailedChecks);
    Result->SetNumberField(TEXT("unmeasuredChecks"), UnmeasuredChecks);

    // An unmeasured check counts against the verdict exactly as a failed one does. "I could
    // not look" and "I looked and it was fine" must never produce the same pass:true, which is
    // the whole reason every check carries its own status and reason string.
    //
    // Derived through the shared contract (Audit/AuditFramework.h) rather than restated here,
    // so this verb cannot drift away from the other three. It is the DEGENERATE case of that
    // rule and the mapping is exact rather than approximate: this audit publishes no severity
    // vocabulary and takes no failOn (a check either gates or it does not - see the header on
    // influence reach), so every failed check is an ErrorCount under the strictest bar; it
    // reads one LOD of one asset in one pass, so there is nothing to truncate. The UNRUNNABLE
    // term is the one that carries weight here, and it is the shared one.
    PinWrightAudit::FVerdict Verdict;
    Verdict.ErrorCount = FailedChecks;
    Verdict.UnrunnableCount = UnmeasuredChecks;
    const bool bPass = Verdict.DerivePass(PinWrightAudit::EFailOn::Error);
    Result->SetBoolField(TEXT("pass"), bPass);
    if (UnmeasuredChecks > 0)
    {
        Warnings.Add(FString::Printf(
            TEXT("%d of the checks could not be measured, so this result is not evidence of clean skinning. "
                 "Read each check's status and reason."), UnmeasuredChecks));
    }
    if (ClothSectionCount > 0)
    {
        Warnings.Add(TEXT("This LOD has cloth sections. Their vertices are simulated rather than skinned at "
                          "runtime, so findings inside them may describe the cloth bind rather than a defect."));
    }
    if (Warnings.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> WarningValues;
        for (const FString& Warning : Warnings)
        {
            WarningValues.Add(MakeShared<FJsonValueString>(Warning));
        }
        Result->SetArrayField(TEXT("warnings"), WarningValues);
    }

    Ctx.SendSuccess(Result);
    return true;
}
