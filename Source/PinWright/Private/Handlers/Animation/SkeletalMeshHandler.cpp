// Copyright (c) 2026 Alexander Penkin. MIT License.

// SkeletalMeshHandler.cpp - Migrated from PinWright_SkeletonHandlers.cpp
// Skeletal mesh operations: skin weights (normalize, prune, set, copy, mirror, auto),
// cloth binding/assignment
//
// Phase 14 migration to auto-registration system.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Animation/SkinWeightTransferUtils.h"
// Bone coverage is computed by the SAME code skeleton.audit_skin_weights runs, so the
// descriptive readback and the gate cannot drift on what "this bone influences no vertex" or
// "this vertex is rigidly bound" means (docs/rpc-design.md 21).
#include "Handlers/Animation/SkinAuditAnalysis.h"
#include "Handlers/Asset/SkeletalMeshDumpBuilder.h"
#include "PinWrightHelpers.h"
#include "PinWrightGlobals.h"
#include "PinWrightSubsystem.h"


#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshTypes.h"
// SkinWeightProfile.h moved from Animation/ to Rendering/ in UE 5.8.
#if __has_include("Rendering/SkinWeightProfile.h")
#include "Rendering/SkinWeightProfile.h"
#elif __has_include("Animation/SkinWeightProfile.h")
#include "Animation/SkinWeightProfile.h"
#endif
#include "Misc/Paths.h"

// Cloth support (Chaos Cloth)
#if __has_include("ClothingAsset/ClothingAssetBase.h")
#include "ClothingAsset/ClothingAssetBase.h"
#elif __has_include("ClothingAssetBase.h")
#include "ClothingAssetBase.h"
#endif

// UClothingAssetCommon for GetNumLods() (UE 5.7+)
#if __has_include("ClothingAsset.h")
#include "ClothingAsset.h"
#elif __has_include("ClothingAssetCommon.h")
#include "ClothingAssetCommon.h"
#endif

// Editor-only cloth authoring: the factory abstraction
// (FClothingSystemEditorInterfaceModule::GetClothingAssetFactory ->
// UClothingAssetFactoryBase::CreateFromSkeletalMesh) lives in
// ClothingSystemEditorInterface. Guarded so the module still builds when the
// engine's clothing editor modules are absent (the create verb then reports
// CLOTH_CREATE_UNSUPPORTED rather than failing to compile).
#if __has_include("ClothingSystemEditorInterfaceModule.h")
#include "ClothingSystemEditorInterfaceModule.h"
#include "ClothingAssetFactoryInterface.h"
#include "Modules/ModuleManager.h"
#define PINWRIGHT_HAS_CLOTH_CREATE 1
#else
#define PINWRIGHT_HAS_CLOTH_CREATE 0
#endif

namespace {

static FString DescribeAssetTypeSM(const UObject* Asset)
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

// Helper: Load skeletal mesh asset from path
static USkeletalMesh* LoadSkeletalMeshFromPathSM(const FString& MeshPath, FString& OutError, bool* bOutWrongType = nullptr)
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
        OutError = FString::Printf(TEXT("Asset '%s' is a %s, not a USkeletalMesh. Pass a USkeletalMesh asset path."), *MeshPath, *DescribeAssetTypeSM(Asset));
        return nullptr;
    }

    return Mesh;
}

static void SendMeshPathErrorSM(FHandlerContext& Ctx, const TCHAR* MissingCode, const FString& Error, bool bWrongType)
{
    Ctx.SendError(bWrongType ? TEXT("INVALID_ASSET_TYPE") : MissingCode, Error);
}

// Helper: find an existing clothing asset on the mesh by name (returns nullptr if absent).
static UClothingAssetBase* FindClothAssetByNameSM(USkeletalMesh* Mesh, const FString& ClothAssetName)
{
    if (!Mesh)
    {
        return nullptr;
    }
    for (const auto& ClothAssetPtr : Mesh->GetMeshClothingAssets())
    {
        UClothingAssetBase* ClothAsset = ClothAssetPtr.Get();
        if (ClothAsset && ClothAsset->GetName() == ClothAssetName)
        {
            return ClothAsset;
        }
    }
    return nullptr;
}

// Shared bind-by-name-then-save-and-report path for the two cloth-attach verbs
// (skeleton.bind_cloth_to_skeletal_mesh's name branch and
// skeleton.assign_cloth_asset_to_mesh's attach mode). Both did the identical
// sequence — look the named asset up, bind it to the section, persist the mesh,
// then echo the same five result fields — so it lives here once instead of being
// copy-pasted into each handler where it could silently drift. Populates Result
// and sends the success/CLOTH_NOT_FOUND/BIND_FAILED response itself; the caller
// should `return true` immediately afterwards.
static void BindNamedClothToSection(
    FHandlerContext& Ctx,
    USkeletalMesh* Mesh,
    const FString& ClothAssetName,
    int32 MeshLodIndex,
    int32 SectionIndex,
    int32 AssetLodIndex,
    const TSharedPtr<FJsonObject>& Result)
{
    UClothingAssetBase* TargetClothAsset = FindClothAssetByNameSM(Mesh, ClothAssetName);
    if (!TargetClothAsset)
    {
        Ctx.SendError(TEXT("CLOTH_NOT_FOUND"),
            FString::Printf(TEXT("Cloth asset '%s' not found on mesh (create it with skeleton.create_cloth_from_section)"), *ClothAssetName));
        return;
    }

    const bool bSuccess = TargetClothAsset->BindToSkeletalMesh(Mesh, MeshLodIndex, SectionIndex, AssetLodIndex);
    if (!bSuccess)
    {
        Ctx.SendError(TEXT("BIND_FAILED"),
            FString::Printf(TEXT("Failed to bind cloth asset '%s' to LOD %d section %d"),
                *ClothAssetName, MeshLodIndex, SectionIndex));
        return;
    }

    McpSafeAssetSave(Mesh);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("clothAssetName"), ClothAssetName);
    Result->SetNumberField(TEXT("meshLodIndex"), MeshLodIndex);
    Result->SetNumberField(TEXT("sectionIndex"), SectionIndex);
    Result->SetNumberField(TEXT("assetLodIndex"), AssetLodIndex);

    Ctx.SendSuccess(Result);
}

// Bone name for a reference-skeleton index, or a printable placeholder when the index is out
// of range. Per-file unique name (the ...SM suffix convention used by the other file-local
// helpers here) so Unity merging cannot collide it with SkinAuditHandler's twin.
static FString BoneNameForRefIndexSM(const FReferenceSkeleton& RefSkeleton, int32 BoneIndex)
{
    if (BoneIndex >= 0 && BoneIndex < RefSkeleton.GetNum())
    {
        return RefSkeleton.GetBoneName(BoneIndex).ToString();
    }
    return FString::Printf(TEXT("<bone %d>"), BoneIndex);
}

// Which bones this LOD's base skinning actually moves, and which vertices belong to one bone
// outright, as a per-LOD JSON block.
//
// WHY IT IS HERE AND NOT ONLY ON THE AUDIT
//
// A bone that no vertex is weighted to is invisible in every other number this verb reports:
// the weight sums are fine, nothing is zero-filled, nothing is degenerate. It is also exactly
// what makes a hand-authored animation move nothing - the curve drives a bone that owns no
// geometry - and until now the only evidence was a coincidence in an influence-count
// histogram, which cannot name a bone. describe_skin_weights is where a caller goes to SEE
// what is there, so the naming belongs here; audit_skin_weights carries the same block as a
// check that can be gated. Both call PinWrightSkinAudit so they cannot answer differently.
//
// SkinWeights carry REFERENCE-SKELETON indices here (ReadBaseSkinningRefSkeletonSpace already
// resolved every section-local slot through the section BoneMap), so a bone index is directly
// nameable and comparable across sections.
static void AddBaseBoneCoverageFieldsSM(
    const TSharedPtr<FJsonObject>& LodObj,
    const TArray<FRawSkinWeight>& SkinWeights,
    const FReferenceSkeleton& RefSkeleton)
{
    PinWrightSkinAudit::FBoneCoverage Coverage;
    PinWrightSkinAudit::BeginBoneCoverage(RefSkeleton.GetNum(),
        PinWrightSkinAudit::DefaultMinReachWeight, Coverage);

    // Reused across vertices so a large LOD does not pay a heap allocation per vertex.
    TArray<int32, TInlineAllocator<MAX_TOTAL_INFLUENCES>> BoneIndices;
    TArray<double, TInlineAllocator<MAX_TOTAL_INFLUENCES>> Weights;
    for (const FRawSkinWeight& Weight : SkinWeights)
    {
        BoneIndices.Reset();
        Weights.Reset();
        SkinWeightTransferUtils::ForEachNonZeroInfluence(Weight,
            [&BoneIndices, &Weights](int32 BoneIndex, float DequantizedWeight)
            {
                BoneIndices.Add(BoneIndex);
                Weights.Add(static_cast<double>(DequantizedWeight));
            });
        PinWrightSkinAudit::AddBoneCoverageVertex(Coverage, BoneIndices, Weights);
    }
    PinWrightSkinAudit::EndBoneCoverage(Coverage);

    TSharedPtr<FJsonObject> CoverageObj = MakeShareable(new FJsonObject());
    CoverageObj->SetNumberField(TEXT("weightEpsilon"), Coverage.WeightEpsilon);
    CoverageObj->SetNumberField(TEXT("boneCount"), Coverage.BoneCount);
    CoverageObj->SetNumberField(TEXT("influencedBoneCount"), Coverage.InfluencedBoneCount);
    CoverageObj->SetNumberField(TEXT("bonesWithNoInfluenceCount"), Coverage.BonesWithNoInfluence.Num());
    CoverageObj->SetNumberField(TEXT("rigidVertexCount"), Coverage.RigidVertexCount);
    CoverageObj->SetStringField(TEXT("boneIndexSpace"), TEXT("referenceSkeleton"));

    // Emitted ALWAYS, empty array included: an absent field cannot be told apart from a build
    // that never looked, which is the whole point of naming these bones.
    TArray<TSharedPtr<FJsonValue>> UninfluencedArray;
    for (int32 BoneIndex : Coverage.BonesWithNoInfluence)
    {
        UninfluencedArray.Add(MakeShareable(new FJsonValueString(
            BoneNameForRefIndexSM(RefSkeleton, BoneIndex))));
    }
    CoverageObj->SetArrayField(TEXT("bonesWithNoInfluence"), UninfluencedArray);

    // One row per bone that owns geometry, most-rigid first. Uncapped: a table clipped to the
    // top N cannot answer "which bone is this part bound to", because that bone is as likely
    // to rank fortieth as first. Bounded by the skeleton's bone count either way.
    TArray<int32> InfluencedBones;
    InfluencedBones.Reserve(Coverage.InfluencedBoneCount);
    for (int32 BoneIndex = 0; BoneIndex < Coverage.BoneCount; ++BoneIndex)
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

    TArray<TSharedPtr<FJsonValue>> BonesArray;
    for (int32 BoneIndex : InfluencedBones)
    {
        TSharedPtr<FJsonObject> BoneObj = MakeShareable(new FJsonObject());
        BoneObj->SetStringField(TEXT("bone"), BoneNameForRefIndexSM(RefSkeleton, BoneIndex));
        BoneObj->SetNumberField(TEXT("boneIndex"), BoneIndex);
        BoneObj->SetNumberField(TEXT("influencedVertices"), Coverage.InfluencedVertexCountPerBone[BoneIndex]);
        // Vertices whose ONLY influence at or above weightEpsilon is this bone. A bone whose
        // rigidVertices equals its influencedVertices owns that geometry outright, which is
        // what a deliberately rigid part looks like stated as a number instead of inferred.
        BoneObj->SetNumberField(TEXT("rigidVertices"), Coverage.RigidVertexCountPerBone[BoneIndex]);
        BonesArray.Add(MakeShareable(new FJsonValueObject(BoneObj)));
    }
    CoverageObj->SetArrayField(TEXT("bones"), BonesArray);

    LodObj->SetObjectField(TEXT("boneCoverage"), CoverageObj);
}

// Serializes one FSkinWeightProfileSummary into the per-LOD JSON shape shared by the base
// readback and the alternate-profile readback, resolving every sampled influence to a bone
// NAME. bSamplesAreRefSkeletonSpace picks how the sampled slot is interpreted: the base
// readback already carries reference-skeleton indices, whereas a profile's SkinWeights hold
// SECTION-LOCAL slots and must go through the vertex's section BoneMap first. Emitting a bare
// number without saying which space it is in is the exact ambiguity this verb exists to end,
// so every sampled influence carries boneIndex, boneName and the block carries boneIndexSpace.
static void AddSkinWeightSummaryFieldsSM(
    const TSharedPtr<FJsonObject>& LodObj,
    const SkinWeightTransferUtils::FSkinWeightProfileSummary& Summary,
    const FSkeletalMeshLODModel& LODModel,
    const FReferenceSkeleton& RefSkeleton,
    bool bSamplesAreRefSkeletonSpace)
{
    LodObj->SetNumberField(TEXT("vertexCount"), Summary.VertexCount);
    LodObj->SetNumberField(TEXT("maxInfluencesPerVertex"), Summary.MaxInfluencesPerVertex);
    LodObj->SetNumberField(TEXT("normalizedVertexCount"), Summary.NormalizedVertexCount);
    LodObj->SetNumberField(TEXT("zeroWeightVertexCount"), Summary.ZeroWeightVertexCount);
    LodObj->SetNumberField(TEXT("degenerateVertexCount"), Summary.DegenerateVertexCount);
    LodObj->SetStringField(TEXT("boneIndexSpace"),
        bSamplesAreRefSkeletonSpace ? TEXT("referenceSkeleton") : TEXT("sectionLocal"));

    if (Summary.Samples.Num() == 0)
    {
        return;
    }

    TArray<TSharedPtr<FJsonValue>> SamplesArray;
    for (const SkinWeightTransferUtils::FSkinWeightVertexSample& Sample : Summary.Samples)
    {
        TSharedPtr<FJsonObject> SampleObj = MakeShareable(new FJsonObject());
        SampleObj->SetNumberField(TEXT("vertexIndex"), Sample.VertexIndex);
        SampleObj->SetNumberField(TEXT("weightSum"), Sample.WeightSum);

        TArray<TSharedPtr<FJsonValue>> InfluencesArray;
        for (int32 InfluenceIdx = 0; InfluenceIdx < Sample.BoneIndices.Num(); ++InfluenceIdx)
        {
            const int32 StoredBone = Sample.BoneIndices[InfluenceIdx];
            int32 RefSkeletonBone = StoredBone;
            bool bResolved = true;
            if (!bSamplesAreRefSkeletonSpace)
            {
                bResolved = SkinWeightTransferUtils::ResolveSectionLocalBone(
                    LODModel, Sample.VertexIndex, StoredBone, RefSkeletonBone);
            }

            TSharedPtr<FJsonObject> InfluenceObj = MakeShareable(new FJsonObject());
            InfluenceObj->SetNumberField(TEXT("boneIndex"), StoredBone);
            InfluenceObj->SetNumberField(TEXT("weight"), Sample.Weights[InfluenceIdx]);
            if (bResolved)
            {
                InfluenceObj->SetNumberField(TEXT("refSkeletonBoneIndex"), RefSkeletonBone);
                InfluenceObj->SetStringField(TEXT("boneName"),
                    BoneNameForRefIndexSM(RefSkeleton, RefSkeletonBone));
            }
            else
            {
                // Named rather than silently resolved to bone 0 (the root on every skeleton),
                // which would read as a plausible influence and pass every validity check.
                InfluenceObj->SetStringField(TEXT("boneName"), TEXT("<unmapped>"));
            }
            InfluencesArray.Add(MakeShareable(new FJsonValueObject(InfluenceObj)));
        }
        SampleObj->SetArrayField(TEXT("influences"), InfluencesArray);
        SamplesArray.Add(MakeShareable(new FJsonValueObject(SampleObj)));
    }
    LodObj->SetArrayField(TEXT("sample"), SamplesArray);
}

// Shared scaffold for the whole-LOD weight edits (normalize_weights / prune_weights):
// resolve + range-check the LOD, capture its base skinning, run the per-op edit
// (EditFn) over the captured FRawSkinWeight array, then persist the result through the
// named profile so it survives Build(). Sends NO_LOD_MODELS / INVALID_LOD itself and
// returns false (the caller should `return true` immediately). The only thing that
// differs between the two handlers is EditFn and the result fields they emit, so the
// LOD-resolution / capture / profile-persist / Build+save pipeline lives here once
// instead of being copy-pasted into each handler (where it could silently drift).
static bool ApplyWeightEditToProfile(
    FHandlerContext& Ctx,
    USkeletalMesh& Mesh,
    int32 LODIndex,
    const FString& ProfileName,
    TFunctionRef<SkinWeightTransferUtils::FWeightArrayEditResult(TArray<FRawSkinWeight>&, const FSkeletalMeshLODModel&)> EditFn,
    SkinWeightTransferUtils::FWeightArrayEditResult& OutResult,
    int32& OutVertexCount,
    int32& OutDroppedInfluences)
{
    FSkeletalMeshModel* ImportedModel = Mesh.GetImportedModel();
    if (!ImportedModel || ImportedModel->LODModels.Num() == 0)
    {
        Ctx.SendError(TEXT("NO_LOD_MODELS"), TEXT("Mesh has no LOD models"));
        return false;
    }

    if (LODIndex < 0 || LODIndex >= ImportedModel->LODModels.Num())
    {
        Ctx.SendError(TEXT("INVALID_LOD"),
            FString::Printf(TEXT("LOD index %d out of range (max: %d)"), LODIndex, ImportedModel->LODModels.Num() - 1));
        return false;
    }

    FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[LODIndex];

    // Seed from the target profile if it already holds authored influences, else from the
    // LOD's base section skinning; see SeedWeightEditSource for why the seed source matters.
    // The result is persisted through the named profile that Build() re-chunks (a bare Build()
    // would only re-derive render data from the unchanged imported weights, so no edit survives).
    TArray<FRawSkinWeight> SkinWeights;
    SkinWeightTransferUtils::SeedWeightEditSource(LODModel, FName(*ProfileName), SkinWeights);

    // EditFn receives the LOD model as well as the weight buffer: the buffer's bone slots are
    // section-local, so any edit that has to name a real bone (set_vertex_weights) needs the
    // section BoneMaps to translate. Edits that only rescale existing influences ignore it.
    OutResult = EditFn(SkinWeights, LODModel);
    OutVertexCount = SkinWeights.Num();

    OutDroppedInfluences =
        SkinWeightTransferUtils::WriteSkinWeightProfile(Mesh, LODModel, FName(*ProfileName), SkinWeights);

    Mesh.Build();
    McpSafeAssetSave(&Mesh);
    return true;
}

// Adds the fields every weight mutator must emit so its response can never be mistaken for a
// base-skinning edit. The four mutators write a named ALTERNATE profile - the engine's
// "alternate influences" channel - and leave FSkelMeshSection::SoftVertices, the skinning the
// renderer uses by default, untouched. Before this the responses said only "skin weights",
// which a caller reasonably reads as base skinning; and the verb documented as the
// verification counterpart read the same profile back, so the write and the check agreed with
// each other while both disagreed with the mesh.
static void AddProfileWriteDisclosureSM(const TSharedPtr<FJsonObject>& Result, int32 DroppedInfluences)
{
    Result->SetStringField(TEXT("wrote"), TEXT("alternateSkinWeightProfile"));
    Result->SetBoolField(TEXT("baseSkinningModified"), false);
    Result->SetStringField(TEXT("verifyWith"),
        TEXT("skeleton.describe_skin_weights (profiles[] for this write, baseSkinning for what the renderer uses)"));
    if (DroppedInfluences > 0)
    {
        // Not a warning about style: an influence whose section-local slot had no BoneMap entry
        // was DROPPED from SourceModelInfluences, so the rebuilt profile is a lower bound.
        Result->SetNumberField(TEXT("droppedInfluences"), DroppedInfluences);
        Result->SetStringField(TEXT("warning"),
            FString::Printf(TEXT("%d influence slots named a section-local bone with no BoneMap entry and were dropped from the rebuilt profile. The LOD's section data is inconsistent; treat the counts above as a lower bound."),
                DroppedInfluences));
    }
}

// Read-only LOD-model lookup for the set_vertex_weights pre-flight. The mutating scaffold
// resolves the same LOD again; this exists so a caller's bad vertexIndex / boneName /
// boneIndex is rejected BEFORE a profile is written and Build() is run, instead of after.
static const FSkeletalMeshLODModel* FindLODModelSM(USkeletalMesh& Mesh, int32 LODIndex)
{
    FSkeletalMeshModel* ImportedModel = Mesh.GetImportedModel();
    if (!ImportedModel || !ImportedModel->LODModels.IsValidIndex(LODIndex))
    {
        return nullptr;
    }
    return &ImportedModel->LODModels[LODIndex];
}

// One vertex's fully-resolved override: the SECTION-LOCAL slots the profile storage requires,
// already checked against the bone map of the section that owns the vertex, and already sorted
// largest-weight-first because every reader of FRawSkinWeight in this codebase treats the
// influence arrays as packed largest-first and zero-terminated (see ForEachNonZeroInfluence).
struct FResolvedVertexEditSM
{
    int32 VertexIndex = INDEX_NONE;
    TArray<uint16, TInlineAllocator<MAX_TOTAL_INFLUENCES>> SectionLocalBones;
    TArray<uint16, TInlineAllocator<MAX_TOTAL_INFLUENCES>> RawWeights;
};

// Validates and resolves the whole `weights` payload. Returns false having already sent a
// typed error when ANY entry is unusable — the call is all-or-nothing on purpose: a
// half-applied weight edit leaves a mesh whose skinning nobody asked for, and the old
// silently-skip behaviour reported the same success either way.
static bool ResolveVertexWeightEditsSM(
    FHandlerContext& Ctx,
    USkeletalMesh& Mesh,
    const FSkeletalMeshLODModel& LODModel,
    const TArray<TSharedPtr<FJsonValue>>& WeightsArray,
    TArray<FResolvedVertexEditSM>& OutEdits)
{
    const FReferenceSkeleton& RefSkeleton = Mesh.GetRefSkeleton();
    OutEdits.Reset();
    OutEdits.Reserve(WeightsArray.Num());

    for (int32 EntryIndex = 0; EntryIndex < WeightsArray.Num(); ++EntryIndex)
    {
        const TSharedPtr<FJsonObject>* WeightObj = nullptr;
        if (!WeightsArray[EntryIndex].IsValid() || !WeightsArray[EntryIndex]->TryGetObject(WeightObj)
            || !WeightObj || !WeightObj->IsValid())
        {
            Ctx.SendError(TEXT("INVALID_PAYLOAD"),
                FString::Printf(TEXT("weights[%d] is not an object; expected {vertexIndex, influences:[...]}"), EntryIndex));
            return false;
        }

        int32 VertexIndex = 0;
        if (!(*WeightObj)->TryGetNumberField(TEXT("vertexIndex"), VertexIndex))
        {
            Ctx.SendError(TEXT("MISSING_PARAM"),
                FString::Printf(TEXT("weights[%d] has no vertexIndex"), EntryIndex));
            return false;
        }

        // FindSectionForVertex doubles as the range check: it returns INDEX_NONE for anything
        // outside the LOD's concatenated section vertices, which is exactly the flat index
        // space describe_skin_weights and audit_skin_weights report.
        const int32 SectionIndex = SkinWeightTransferUtils::FindSectionForVertex(LODModel, VertexIndex);
        if (SectionIndex == INDEX_NONE)
        {
            Ctx.SendError(TEXT("INDEX_OUT_OF_RANGE"),
                FString::Printf(TEXT("weights[%d].vertexIndex %d is outside LOD's %d vertices"),
                    EntryIndex, VertexIndex, LODModel.NumVertices));
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* InfluencesArray = nullptr;
        if (!(*WeightObj)->TryGetArrayField(TEXT("influences"), InfluencesArray) || !InfluencesArray)
        {
            Ctx.SendError(TEXT("MISSING_PARAM"),
                FString::Printf(TEXT("weights[%d] has no influences array"), EntryIndex));
            return false;
        }
        if (InfluencesArray->Num() == 0)
        {
            // An empty influence list would leave the vertex weighted to nothing, which renders
            // it at the component origin. Refused rather than written, matching the "never
            // zero-fill" rule prune_weights already follows.
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                FString::Printf(TEXT("weights[%d].influences is empty; a vertex with no influence renders at the component origin. Remove the entry to leave the vertex as seeded."),
                    EntryIndex));
            return false;
        }
        if (InfluencesArray->Num() > MAX_TOTAL_INFLUENCES)
        {
            // Previously the surplus was silently dropped past MAX_TOTAL_INFLUENCES.
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                FString::Printf(TEXT("weights[%d].influences has %d entries; the per-vertex maximum is %d"),
                    EntryIndex, InfluencesArray->Num(), MAX_TOTAL_INFLUENCES));
            return false;
        }

        FResolvedVertexEditSM Edit;
        Edit.VertexIndex = VertexIndex;

        TArray<int32, TInlineAllocator<MAX_TOTAL_INFLUENCES>> SeenRefBones;
        for (int32 InfIndex = 0; InfIndex < InfluencesArray->Num(); ++InfIndex)
        {
            const TSharedPtr<FJsonObject>* InfluenceObj = nullptr;
            if (!(*InfluencesArray)[InfIndex].IsValid()
                || !(*InfluencesArray)[InfIndex]->TryGetObject(InfluenceObj)
                || !InfluenceObj || !InfluenceObj->IsValid())
            {
                Ctx.SendError(TEXT("INVALID_PAYLOAD"),
                    FString::Printf(TEXT("weights[%d].influences[%d] is not an object; expected {boneName|boneIndex, weight}"),
                        EntryIndex, InfIndex));
                return false;
            }

            // boneName wins when both are supplied: a name is unambiguous across re-imports,
            // an index is not.
            int32 RefSkeletonBone = INDEX_NONE;
            FString BoneName;
            if ((*InfluenceObj)->TryGetStringField(TEXT("boneName"), BoneName) && !BoneName.IsEmpty())
            {
                RefSkeletonBone = RefSkeleton.FindBoneIndex(FName(*BoneName));
                if (RefSkeletonBone == INDEX_NONE)
                {
                    Ctx.SendError(TEXT("BONE_NOT_FOUND"),
                        FString::Printf(TEXT("weights[%d].influences[%d] names bone '%s', which is not in this mesh's reference skeleton (%d bones). List them with skeleton.list_bones."),
                            EntryIndex, InfIndex, *BoneName, RefSkeleton.GetNum()));
                    return false;
                }
            }
            else if ((*InfluenceObj)->TryGetNumberField(TEXT("boneIndex"), RefSkeletonBone))
            {
                if (RefSkeletonBone < 0 || RefSkeletonBone >= RefSkeleton.GetNum())
                {
                    Ctx.SendError(TEXT("BONE_NOT_FOUND"),
                        FString::Printf(TEXT("weights[%d].influences[%d].boneIndex %d is outside the reference skeleton's %d bones. boneIndex is a REFERENCE-SKELETON index (the one skeleton.list_bones reports), not a section-local slot."),
                            EntryIndex, InfIndex, RefSkeletonBone, RefSkeleton.GetNum()));
                    return false;
                }
            }
            else
            {
                Ctx.SendError(TEXT("MISSING_PARAM"),
                    FString::Printf(TEXT("weights[%d].influences[%d] has neither boneName nor boneIndex"),
                        EntryIndex, InfIndex));
                return false;
            }

            if (SeenRefBones.Contains(RefSkeletonBone))
            {
                // Two entries for one bone would produce two influence slots naming the same
                // bone, which every consumer treats as two independent influences.
                Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                    FString::Printf(TEXT("weights[%d] names bone '%s' more than once"),
                        EntryIndex, *BoneNameForRefIndexSM(RefSkeleton, RefSkeletonBone)));
                return false;
            }
            SeenRefBones.Add(RefSkeletonBone);

            // The crossing into storage space. A section's BoneMap is fixed until the next
            // re-chunk, so a bone it does not carry has no slot; inventing one would weight the
            // vertex to a different bone entirely.
            int32 SectionLocalBone = INDEX_NONE;
            if (!SkinWeightTransferUtils::ResolveBoneToSectionLocalSlot(
                    LODModel, VertexIndex, RefSkeletonBone, SectionLocalBone))
            {
                Ctx.SendError(TEXT("BONE_NOT_IN_SECTION"),
                    FString::Printf(TEXT("weights[%d]: vertex %d lives in section %d, whose bone map does not carry bone '%s' (reference-skeleton index %d), so that bone cannot influence it without a re-chunk. Section bone maps are reported by skeleton.audit_skin_weights."),
                        EntryIndex, VertexIndex, SectionIndex,
                        *BoneNameForRefIndexSM(RefSkeleton, RefSkeletonBone), RefSkeletonBone));
                return false;
            }

            double Weight = 0.0;
            if (!(*InfluenceObj)->TryGetNumberField(TEXT("weight"), Weight))
            {
                Ctx.SendError(TEXT("MISSING_PARAM"),
                    FString::Printf(TEXT("weights[%d].influences[%d] has no weight"), EntryIndex, InfIndex));
                return false;
            }
            // A zero weight is not "no influence" in this storage: influences are packed
            // largest-first and zero-TERMINATED, so a zero in the middle hides every influence
            // after it. Reject rather than silently truncate the vertex.
            if (!(Weight > 0.0) || Weight > 1.0)
            {
                Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                    FString::Printf(TEXT("weights[%d].influences[%d].weight must be in (0, 1]; got %f"),
                        EntryIndex, InfIndex, Weight));
                return false;
            }

            Edit.SectionLocalBones.Add(static_cast<uint16>(SectionLocalBone));
            Edit.RawWeights.Add(SkinWeightTransferUtils::FloatToRawWeight(static_cast<float>(Weight)));
        }

        // Repack largest-first. Selection sort over at most MAX_TOTAL_INFLUENCES entries, kept
        // explicit rather than routed through a paired-array sort helper that does not exist.
        for (int32 A = 0; A < Edit.RawWeights.Num(); ++A)
        {
            int32 Largest = A;
            for (int32 B = A + 1; B < Edit.RawWeights.Num(); ++B)
            {
                if (Edit.RawWeights[B] > Edit.RawWeights[Largest])
                {
                    Largest = B;
                }
            }
            if (Largest != A)
            {
                Swap(Edit.RawWeights[A], Edit.RawWeights[Largest]);
                Swap(Edit.SectionLocalBones[A], Edit.SectionLocalBones[Largest]);
            }
        }

        OutEdits.Add(MoveTemp(Edit));
    }

    return true;
}

} // anonymous namespace


// ===========================================================================
// skeleton.describe_mesh - Describe skeletal mesh dump metadata
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.describe_mesh", "skeleton",
    "Return read-only SkeletalMesh metadata using the same JSON shape as skeletal_mesh.json asset dumps: bounds, materials, LOD stats, physics asset, skeleton links, active sockets, and virtual bones.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletalMeshPath", "path", "Path to the skeletal mesh")
    ))
{
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));

    if (SkeletalMeshPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletalMeshPath is required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeletalMesh* Mesh = LoadSkeletalMeshFromPathSM(SkeletalMeshPath, Error, &bWrongType);
    if (!Mesh)
    {
        SendMeshPathErrorSM(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    Ctx.SendSuccess(SkeletalMeshDumpBuilder::BuildSkeletalMeshJson(Mesh));
    return true;
}


// ===========================================================================
// skeleton.describe_skin_weights - Read back BASE skinning and alternate profiles
// ===========================================================================
//
// THE CLOSED LOOP THIS VERB USED TO CLOSE, AND WHY BASE SKINNING IS NOW REPORTED FIRST
//
// The four weight mutators (normalize/prune/set_vertex/copy) write a NAMED ALTERNATE
// PROFILE - FImportedSkinWeightProfileData - and never touch FSkelMeshSection::SoftVertices,
// the base skinning the renderer uses when no profile is activated on a component. This verb
// read the same named profiles back. So the write path and the verification path agreed with
// each other while both disagreed with the mesh: you could normalize weights, read them back,
// see exactly what you asked for, and the mesh underneath was untouched. And on a mesh with no
// authored profile at all - every FBX import, and every mesh built by
// geometry.convert_to_skeletal_mesh - the answer was profileCount:0 with a success status,
// having measured nothing whatsoever about the skinning the mesh actually renders with.
//
// So the readback now reports BOTH, labelled, in one response:
//   * baseSkinning - read straight out of the LOD's section soft-vertices, with every
//     influence resolved through FSkelMeshSection::BoneMap into a real reference-skeleton bone
//     and named. This is what the renderer uses, and it is what any base-weight write
//     (geometry.convert_to_skeletal_mesh, an FBX re-import, a DCC round trip) changes.
//   * profiles - the alternate influence sets, explicitly marked as such, with their
//     section-local bone slots ALSO resolved to names so the two blocks can be compared.
// Every block states its boneIndexSpace, because a bare bone number that does not say which
// space it is in is the exact ambiguity that let this survive.
//
// Related: skeleton.audit_skin_weights also reads base skinning, but returns a numeric verdict
// (zero-influence vertices, weight sums, influence-count limits, coincident-seam splits)
// rather than a description. Use this verb to see what is there, that one to gate on it.

REGISTER_RPC_HANDLER("skeleton.describe_skin_weights", "skeleton",
    "Read-only readback of BOTH halves of a SkeletalMesh's skinning, so a weight write is verifiable against what the renderer actually uses. baseSkinning reports the LOD's section soft-vertices - the skinning in effect when no profile is activated - with every influence resolved through the section BoneMap to a named reference-skeleton bone. profiles reports the named ALTERNATE skin-weight profiles, which is what skeleton.normalize_weights / prune_weights / set_vertex_weights / copy_weights write and which do NOT change base skinning. Each block reports vertex count, max influences per vertex and a sum-to-1.0 validity breakdown (normalized vs zero-filled vs degenerate), and states its boneIndexSpace. Every baseSkinning LOD additionally carries boneCoverage: bonesWithNoInfluence names every reference-skeleton bone no vertex is weighted to — the defect that makes a hand-authored animation move nothing while every weight number looks correct — and bones[] gives each influenced bone's influencedVertices and rigidVertices (vertices whose only meaningful influence is that bone), so a deliberately rigid part is readable rather than inferred from an influence histogram. A mesh with no authored profile reports profileCount 0 and still returns full baseSkinning. Pass profileName to inspect one profile, lodIndex for one LOD, sampleCount for a first-N per-vertex influence sample.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletalMeshPath", "path", "Path to the skeletal mesh"),
        RPC_PARAM_OPT("profileName", "string", "Limit the profiles block to one named alternate profile (default: all). Does not affect baseSkinning"),
        RPC_PARAM_OPT("lodIndex", "integer", "Limit to one LOD index (default: all LODs)"),
        RPC_PARAM_DEF("sampleCount", "integer", "Per-LOD first-N vertex influence sample size, emitted for baseSkinning and for each profile", "0"),
        RPC_PARAM_DEF("includeBaseSkinning", "bool", "Report the LOD's real base skinning. Leave true unless you specifically only want profile contents - it is the only block that describes what the mesh renders with", "true")
    ))
{
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    if (SkeletalMeshPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletalMeshPath is required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeletalMesh* Mesh = LoadSkeletalMeshFromPathSM(SkeletalMeshPath, Error, &bWrongType);
    if (!Mesh)
    {
        SendMeshPathErrorSM(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    const FString FilterProfileName = Ctx.GetString(TEXT("profileName"));
    const bool bFilterProfile = !FilterProfileName.IsEmpty();
    const int32 SampleCount = FMath::Max(0, Ctx.GetInt(TEXT("sampleCount"), 0));
    const bool bIncludeBase = Ctx.GetBool(TEXT("includeBaseSkinning"), true);

    const FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
    const int32 NumLODs = ImportedModel ? ImportedModel->LODModels.Num() : 0;
    // Bone names come off the MESH's own reference skeleton, not the USkeleton's: section
    // BoneMap entries index USkeletalMesh::RefSkeleton (Rendering/SkeletalMeshLODModel.h:74-75).
    const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();

    // No imported model means base skinning cannot be read at all (a cooked-only or not-yet-built
    // mesh). Reported as a refusal rather than an empty-but-successful block, so "I could not
    // look" never renders the same as "I looked and there was nothing".
    if (bIncludeBase && NumLODs == 0)
    {
        Ctx.SendError(TEXT("NO_LOD_MODELS"),
            TEXT("Mesh has no imported LOD models, so neither its base skinning nor its profile contents can be "
                 "read. This readback needs editor source data; a cooked-only or not-yet-built mesh cannot be "
                 "described. Pass includeBaseSkinning:false to list registered profile names only."));
        return true;
    }

    // Optional single-LOD filter. -1 (the default) means "all LODs".
    int32 FilterLODIndex = -1;
    if (Ctx.GetRawPayload()->HasField(TEXT("lodIndex")))
    {
        FilterLODIndex = Ctx.GetInt(TEXT("lodIndex"), 0);
        if (FilterLODIndex < 0 || FilterLODIndex >= NumLODs)
        {
            Ctx.SendError(TEXT("INVALID_LOD"),
                FString::Printf(TEXT("LOD index %d out of range (mesh has %d LODs)"), FilterLODIndex, NumLODs));
            return true;
        }
    }

    // Iterate the one filtered LOD directly when lodIndex is supplied (>= 0, already
    // range-validated above), else all LODs — avoids spinning over every LOD just to
    // skip all but one.
    const int32 FirstLOD = FilterLODIndex >= 0 ? FilterLODIndex : 0;
    const int32 LastLOD = FilterLODIndex >= 0 ? FilterLODIndex + 1 : NumLODs;

    TArray<TSharedPtr<FJsonValue>> ProfilesArray;
    int32 MatchedProfiles = 0;

    for (const FSkinWeightProfileInfo& ProfileInfo : Mesh->GetSkinWeightProfiles())
    {
        const FString ProfileName = ProfileInfo.Name.ToString();
        if (bFilterProfile && ProfileName != FilterProfileName)
        {
            continue;
        }
        ++MatchedProfiles;

        TSharedPtr<FJsonObject> ProfileObj = MakeShareable(new FJsonObject());
        ProfileObj->SetStringField(TEXT("name"), ProfileName);

        TArray<TSharedPtr<FJsonValue>> PerLodArray;
        for (int32 LODIndex = FirstLOD; LODIndex < LastLOD; ++LODIndex)
        {
            const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[LODIndex];
            const FImportedSkinWeightProfileData* ProfileData = LODModel.SkinWeightProfiles.Find(ProfileInfo.Name);

            TSharedPtr<FJsonObject> LodObj = MakeShareable(new FJsonObject());
            LodObj->SetNumberField(TEXT("lodIndex"), LODIndex);
            // A profile registered at the mesh level may have no imported data on a given
            // LOD; report present=false rather than fabricating a zeroed summary.
            LodObj->SetBoolField(TEXT("present"), ProfileData != nullptr);

            if (ProfileData)
            {
                const SkinWeightTransferUtils::FSkinWeightProfileSummary Summary =
                    SkinWeightTransferUtils::SummarizeSkinWeights(ProfileData->SkinWeights, SampleCount);

                // A profile's SkinWeights hold SECTION-LOCAL bone slots (the engine reads them
                // back through Section.BoneMap at MeshUtilities.cpp:5774), so the sampled
                // indices are resolved rather than emitted bare.
                AddSkinWeightSummaryFieldsSM(LodObj, Summary, LODModel, RefSkeleton,
                    /*bSamplesAreRefSkeletonSpace*/ false);
            }

            PerLodArray.Add(MakeShareable(new FJsonValueObject(LodObj)));
        }

        ProfileObj->SetArrayField(TEXT("lods"), PerLodArray);
        ProfilesArray.Add(MakeShareable(new FJsonValueObject(ProfileObj)));
    }

    if (bFilterProfile && MatchedProfiles == 0)
    {
        Ctx.SendError(TEXT("PROFILE_NOT_FOUND"),
            FString::Printf(TEXT("Skin-weight profile '%s' not found on mesh"), *FilterProfileName));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("skeletalMeshPath"), SkeletalMeshPath);
    Result->SetNumberField(TEXT("lods"), NumLODs);
    Result->SetNumberField(TEXT("profileCount"), ProfilesArray.Num());
    Result->SetArrayField(TEXT("profiles"), ProfilesArray);
    // Named, not implied: everything in profiles[] is an ALTERNATE influence set that the
    // renderer ignores unless a component activates it by name.
    Result->SetStringField(TEXT("profileKind"), TEXT("alternateSkinWeightProfile"));

    // ---- Base skinning: what the mesh actually renders with ----------------------------
    //
    // Emitted even when there are no profiles at all, which is the case this verb used to
    // answer with profileCount:0 and a success status while measuring nothing.
    if (bIncludeBase)
    {
        TArray<TSharedPtr<FJsonValue>> BaseLodArray;
        int32 TotalUnmapped = 0;
        for (int32 LODIndex = FirstLOD; LODIndex < LastLOD; ++LODIndex)
        {
            const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[LODIndex];

            SkinWeightTransferUtils::FBaseSkinningReadback BaseReadback;
            SkinWeightTransferUtils::ReadBaseSkinningRefSkeletonSpace(LODModel, BaseReadback);
            TotalUnmapped += BaseReadback.UnmappedInfluences;

            const SkinWeightTransferUtils::FSkinWeightProfileSummary BaseSummary =
                SkinWeightTransferUtils::SummarizeSkinWeights(BaseReadback.SkinWeights, SampleCount);

            TSharedPtr<FJsonObject> BaseLodObj = MakeShareable(new FJsonObject());
            BaseLodObj->SetNumberField(TEXT("lodIndex"), LODIndex);
            BaseLodObj->SetNumberField(TEXT("sectionCount"), BaseReadback.SectionCount);
            BaseLodObj->SetNumberField(TEXT("clothSectionCount"), BaseReadback.ClothSectionCount);
            BaseLodObj->SetNumberField(TEXT("disabledSectionCount"), BaseReadback.DisabledSectionCount);
            BaseLodObj->SetNumberField(TEXT("unmappedInfluences"), BaseReadback.UnmappedInfluences);
            AddSkinWeightSummaryFieldsSM(BaseLodObj, BaseSummary, LODModel, RefSkeleton,
                /*bSamplesAreRefSkeletonSpace*/ true);
            // Which bones this LOD actually moves, and which vertices one bone owns outright.
            // Base skinning only: profiles[] store section-local slots the renderer ignores
            // unless a component activates them, so coverage there would describe a set the
            // mesh does not render with.
            AddBaseBoneCoverageFieldsSM(BaseLodObj, BaseReadback.SkinWeights, RefSkeleton);
            BaseLodArray.Add(MakeShareable(new FJsonValueObject(BaseLodObj)));
        }

        TSharedPtr<FJsonObject> BaseObj = MakeShareable(new FJsonObject());
        BaseObj->SetStringField(TEXT("source"), TEXT("sectionSoftVertices"));
        BaseObj->SetNumberField(TEXT("boneCount"), RefSkeleton.GetNum());
        BaseObj->SetArrayField(TEXT("lods"), BaseLodArray);
        if (TotalUnmapped > 0)
        {
            BaseObj->SetStringField(TEXT("warning"),
                FString::Printf(TEXT("%d influence slots referenced a section-local bone with no BoneMap entry and were dropped. The LOD's section data is inconsistent; treat every count here as a lower bound."),
                    TotalUnmapped));
        }
        Result->SetObjectField(TEXT("baseSkinning"), BaseObj);
    }

    // The distinction, stated in the payload rather than left to the reader, because the two
    // blocks disagreeing is the normal case rather than an anomaly: the weight mutators only
    // ever write profiles[].
    Result->SetStringField(TEXT("note"),
        TEXT("baseSkinning is what the renderer uses. profiles[] are alternate influence sets written by "
             "skeleton.normalize_weights / prune_weights / set_vertex_weights / copy_weights, which do NOT "
             "change base skinning. The two disagreeing is expected after any of those verbs. To change base "
             "skinning use the geometry round trip (geometry.create_from_skeletal_mesh -> "
             "geometry.bind_skin_weights -> geometry.convert_to_skeletal_mesh with overwrite:true), then read "
             "baseSkinning here or gate on skeleton.audit_skin_weights."));

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.normalize_weights - Normalize skin weights
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.normalize_weights", "skeleton",
    "Renormalize each vertex's influences so they sum to 1.0 and write the result into a NAMED ALTERNATE skin-weight profile (default NormalizedWeights). This does NOT change the mesh's base skinning - the section soft-vertices the renderer uses when no profile is activated on a component are left byte-for-byte untouched, and the response says so with baseSkinningModified:false. It seeds from the target profile's existing weights when it is already authored, otherwise from a copy of base skinning. Returns verticesNormalized. Verify with skeleton.describe_skin_weights, reading profiles[] for this write and baseSkinning for what the mesh renders with. To change base skinning, use the geometry round trip named on the skeleton wiki page.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletalMeshPath", "path", "Path to the skeletal mesh"),
        RPC_PARAM_OPT("profileName", "string", "Named ALTERNATE skin-weight profile to write the normalized result into (default NormalizedWeights). Base skinning is never the target"),
        RPC_PARAM_OPT("lodIndex", "integer", "LOD index (default 0)")
    ))
{
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));

    if (SkeletalMeshPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletalMeshPath is required"));
        return true;
    }

    FString ProfileName = Ctx.GetString(TEXT("profileName"));
    if (ProfileName.IsEmpty())
    {
        ProfileName = TEXT("NormalizedWeights");
    }

    FString Error;
    bool bWrongType = false;
    USkeletalMesh* Mesh = LoadSkeletalMeshFromPathSM(SkeletalMeshPath, Error, &bWrongType);
    if (!Mesh)
    {
        SendMeshPathErrorSM(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    const int32 LODIndex = Ctx.GetInt(TEXT("lodIndex"), 0);

    // Renormalize every vertex so its influences sum to 1.0; the shared scaffold owns
    // the LOD-resolution / capture / profile-persist / Build+save pipeline.
    SkinWeightTransferUtils::FWeightArrayEditResult EditResult;
    int32 VertexCount = 0;
    int32 DroppedInfluences = 0;
    if (!ApplyWeightEditToProfile(Ctx, *Mesh, LODIndex, ProfileName,
            [](TArray<FRawSkinWeight>& Weights, const FSkeletalMeshLODModel&)
            { return SkinWeightTransferUtils::NormalizeSkinWeights(Weights); },
            EditResult, VertexCount, DroppedInfluences))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("skeletalMeshPath"), SkeletalMeshPath);
    Result->SetStringField(TEXT("profileName"), ProfileName);
    Result->SetNumberField(TEXT("lodIndex"), LODIndex);
    Result->SetNumberField(TEXT("vertexCount"), VertexCount);
    Result->SetNumberField(TEXT("verticesNormalized"), EditResult.VerticesChanged);
    AddProfileWriteDisclosureSM(Result, DroppedInfluences);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.prune_weights - Remove bone influences below a threshold
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.prune_weights", "skeleton",
    "Drop bone influences below a weight threshold, renormalize the survivors, and write the result into a NAMED ALTERNATE skin-weight profile (default PrunedWeights). This does NOT change the mesh's base skinning - the section soft-vertices the renderer uses when no profile is activated are left untouched, and the response says so with baseSkinningModified:false. Seeds from the target profile when it is already authored, otherwise from a copy of base skinning. Returns influencesRemoved and verticesAffected; a vertex whose influences would all be pruned is left intact (never zero-filled). Verify with skeleton.describe_skin_weights.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletalMeshPath", "path", "Path to the skeletal mesh"),
        RPC_PARAM_OPT("threshold", "number", "Weight threshold (default 0.01)"),
        RPC_PARAM_OPT("profileName", "string", "Named ALTERNATE skin-weight profile to write the pruned result into (default PrunedWeights). Base skinning is never the target"),
        RPC_PARAM_OPT("lodIndex", "integer", "LOD index (default 0)")
    ))
{
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    double Threshold = Ctx.GetNumber(TEXT("threshold"), 0.01);

    if (SkeletalMeshPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletalMeshPath is required"));
        return true;
    }

    // A threshold at/above 1.0 would strip every vertex (every influence is <= 1.0);
    // reject rather than silently no-op the whole mesh.
    if (Threshold < 0.0 || Threshold >= 1.0)
    {
        Ctx.SendError(TEXT("INVALID_THRESHOLD"),
            FString::Printf(TEXT("threshold must be in [0, 1); got %f"), Threshold));
        return true;
    }

    FString ProfileName = Ctx.GetString(TEXT("profileName"));
    if (ProfileName.IsEmpty())
    {
        ProfileName = TEXT("PrunedWeights");
    }

    FString Error;
    bool bWrongType = false;
    USkeletalMesh* Mesh = LoadSkeletalMeshFromPathSM(SkeletalMeshPath, Error, &bWrongType);
    if (!Mesh)
    {
        SendMeshPathErrorSM(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    const int32 LODIndex = Ctx.GetInt(TEXT("lodIndex"), 0);

    // Prune influences below threshold (then renormalize the survivors); the shared
    // scaffold owns the LOD-resolution / capture / profile-persist / Build+save pipeline.
    // threshold is now actually applied (it was accept-then-discarded before), and a
    // count of removed influences is returned so the operation is observable.
    const float ThresholdF = static_cast<float>(Threshold);
    SkinWeightTransferUtils::FWeightArrayEditResult EditResult;
    int32 VertexCount = 0;
    int32 DroppedInfluences = 0;
    if (!ApplyWeightEditToProfile(Ctx, *Mesh, LODIndex, ProfileName,
            [ThresholdF](TArray<FRawSkinWeight>& Weights, const FSkeletalMeshLODModel&)
            { return SkinWeightTransferUtils::PruneSkinWeights(Weights, ThresholdF); },
            EditResult, VertexCount, DroppedInfluences))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("skeletalMeshPath"), SkeletalMeshPath);
    Result->SetStringField(TEXT("profileName"), ProfileName);
    Result->SetNumberField(TEXT("lodIndex"), LODIndex);
    Result->SetNumberField(TEXT("threshold"), Threshold);
    Result->SetNumberField(TEXT("vertexCount"), VertexCount);
    Result->SetNumberField(TEXT("verticesAffected"), EditResult.VerticesChanged);
    Result->SetNumberField(TEXT("influencesRemoved"), EditResult.InfluencesRemoved);
    AddProfileWriteDisclosureSM(Result, DroppedInfluences);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.set_vertex_weights - Set skin weights for specific vertices
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.set_vertex_weights", "skeleton",
    "Overwrite the influences of named vertices in a NAMED ALTERNATE skin-weight profile (default CustomWeights). This does NOT change base skinning; the response reports baseSkinningModified:false. Name each influence's bone by boneName, or by boneIndex as a REFERENCE-SKELETON index (the same index skeleton.list_bones reports) - never a section-local slot. Both are validated against the mesh's reference skeleton and against the bone map of the section that owns the vertex, so an unusable value is a typed error instead of an influence silently attached to the wrong bone. vertexIndex is a flat LOD render-vertex index, matching skeleton.describe_skin_weights and skeleton.audit_skin_weights. Vertices the call does not name keep their seeded influences.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletalMeshPath", "path", "Path to the skeletal mesh"),
        RPC_PARAM_OPT("profileName", "string", "Named ALTERNATE skin-weight profile to author into (default CustomWeights). Base skinning is never the target"),
        RPC_PARAM_REQ("weights", "array", "Array of {vertexIndex, influences:[{boneName | boneIndex, weight}]}. vertexIndex is a flat LOD render-vertex index; boneIndex is a REFERENCE-SKELETON index; boneName wins when both are given"),
        RPC_PARAM_OPT("lodIndex", "integer", "LOD index (default 0)")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    FString ProfileName = Ctx.GetString(TEXT("profileName"));
    if (ProfileName.IsEmpty())
    {
        ProfileName = TEXT("CustomWeights");
    }

    if (SkeletalMeshPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletalMeshPath is required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeletalMesh* Mesh = LoadSkeletalMeshFromPathSM(SkeletalMeshPath, Error, &bWrongType);
    if (!Mesh)
    {
        SendMeshPathErrorSM(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* WeightsArray = nullptr;
    if (!Payload->TryGetArrayField(TEXT("weights"), WeightsArray) || !WeightsArray)
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("weights array is required"));
        return true;
    }

    const int32 LODIndex = Ctx.GetInt(TEXT("lodIndex"), 0);

    // PRE-FLIGHT, BEFORE ANYTHING IS WRITTEN. The old body resolved nothing and validated
    // nothing: it cast the caller's boneIndex straight to FBoneIndexType and stored it, in an
    // index space the schema never named. Because the buffer it writes is SECTION-LOCAL while
    // the list rebuilt from it is reference-skeleton, no value a caller could pass was correct
    // in both places (board B-set-vertex-weights-boneindex-unvalidated-index-space), and a bad
    // vertexIndex or a bone the section does not carry was silently skipped or silently
    // attached to the wrong bone. Resolution now happens up front, against the mesh's own
    // reference skeleton and the bone map of the section that owns each vertex, and ANY
    // unusable entry rejects the whole call rather than half-applying it.
    const FSkeletalMeshLODModel* PreflightLOD = FindLODModelSM(*Mesh, LODIndex);
    if (!PreflightLOD)
    {
        Ctx.SendError(TEXT("INVALID_LOD"),
            FString::Printf(TEXT("LOD index %d out of range (mesh has %d LOD models)"),
                LODIndex, Mesh->GetImportedModel() ? Mesh->GetImportedModel()->LODModels.Num() : 0));
        return true;
    }

    TArray<FResolvedVertexEditSM> ResolvedEdits;
    if (!ResolveVertexWeightEditsSM(Ctx, *Mesh, *PreflightLOD, *WeightsArray, ResolvedEdits))
    {
        return true;
    }

    // Author the resolved overrides on top of the seeded buffer; the shared
    // scaffold owns the LOD-resolution / seed / profile-persist / Build+save pipeline (so this
    // path can't drift from normalize/prune_weights). The seed matters: the profile's SkinWeights
    // array is DENSE (one FRawSkinWeight per LOD vertex), so every vertex — including the ones
    // this call does not name — must hold a valid influence set. SeedWeightEditSource (inside the
    // scaffold) captures the LOD's base section skinning for a fresh profile (so un-authored
    // vertices read back as their real, normalized base influences) and preserves any
    // previously-authored profile influences on a repeat edit — instead of the old hand-rolled
    // SetNum, which left the un-authored entries as uninitialized garbage that describe_skin_weights
    // classified as degenerate (B-skeleton-describe-skin-weights-garbage-on-fresh-profile).
    SkinWeightTransferUtils::FWeightArrayEditResult EditResult;
    int32 VertexCount = 0;
    int32 DroppedInfluences = 0;
    if (!ApplyWeightEditToProfile(Ctx, *Mesh, LODIndex, ProfileName,
            [&ResolvedEdits](TArray<FRawSkinWeight>& SkinWeights, const FSkeletalMeshLODModel&)
            {
                SkinWeightTransferUtils::FWeightArrayEditResult Out;
                for (const FResolvedVertexEditSM& Edit : ResolvedEdits)
                {
                    if (!SkinWeights.IsValidIndex(Edit.VertexIndex))
                    {
                        // Only reachable if the seed buffer is shorter than the LOD's section
                        // vertex count, i.e. a pre-existing profile of the wrong length. Skipped
                        // rather than written out of bounds; the count below then disagrees with
                        // the request, which is the observable signal.
                        continue;
                    }
                    FRawSkinWeight& SkinWeight = SkinWeights[Edit.VertexIndex];
                    FMemory::Memzero(&SkinWeight, sizeof(FRawSkinWeight));
                    for (int32 Slot = 0; Slot < Edit.SectionLocalBones.Num(); ++Slot)
                    {
                        SkinWeight.InfluenceBones[Slot] = Edit.SectionLocalBones[Slot];
                        SkinWeight.InfluenceWeights[Slot] = Edit.RawWeights[Slot];
                    }
                    ++Out.VerticesChanged;
                }
                return Out;
            },
            EditResult, VertexCount, DroppedInfluences))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("skeletalMeshPath"), SkeletalMeshPath);
    Result->SetStringField(TEXT("profileName"), ProfileName);
    Result->SetNumberField(TEXT("verticesModified"), EditResult.VerticesChanged);
    Result->SetNumberField(TEXT("verticesRequested"), ResolvedEdits.Num());
    Result->SetNumberField(TEXT("vertexCount"), VertexCount);
    Result->SetNumberField(TEXT("lodIndex"), LODIndex);
    Result->SetStringField(TEXT("boneIndexSpace"), TEXT("referenceSkeleton"));
    AddProfileWriteDisclosureSM(Result, DroppedInfluences);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.auto_skin_weights - Rebuild mesh with recalculated skin weights
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.auto_skin_weights", "skeleton",
    "NOT SUPPORTED through this RPC: Mesh->Build() only re-derives render data from the existing imported weights, so this method rejects rather than reporting a fake rebuild. For a genuine from-bind-pose auto-skin that changes BASE skinning, use the geometry round trip: geometry.create_from_skeletal_mesh, then geometry.bind_skin_weights (smooth binding against the skeleton), then geometry.convert_to_skeletal_mesh with overwrite:true. The skeleton.* weight verbs are not an alternative to that - they only author named alternate profiles.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletalMeshPath", "path", "Path to the skeletal mesh")
    ))
{
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));

    if (SkeletalMeshPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletalMeshPath is required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeletalMesh* Mesh = LoadSkeletalMeshFromPathSM(SkeletalMeshPath, Error, &bWrongType);
    if (!Mesh)
    {
        SendMeshPathErrorSM(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    // A bare Mesh->Build() only re-derives render data from the *existing* imported
    // weights — it does not recompute skin weights from bind-pose geometry, despite
    // the old name/docs. Rather than fake-success a {rebuilt:true} no-op (the
    // B-skeleton-auto-skin-weights-noop-rebuild defect), reject honestly and point
    // callers at the path that really does re-skin.
    //
    // The old message pointed at normalize/prune/set_vertex_weights, which was misleading in
    // the same way the whole family was: those write a named ALTERNATE profile and never touch
    // base skinning, so following that advice produced no re-skin at all. The geometry round
    // trip does, because CopyMeshToSkeletalMesh rewrites the LOD's MeshDescription, which is
    // where base skinning lives.
    Ctx.SendError(TEXT("UNSUPPORTED_OPERATION"),
        TEXT("auto_skin_weights (from-bind-pose re-skin) is not supported through this RPC; "
             "Mesh->Build() only re-derives render data from existing imported weights. "
             "To recompute BASE skin weights, run geometry.create_from_skeletal_mesh, then "
             "geometry.bind_skin_weights, then geometry.convert_to_skeletal_mesh with overwrite:true, "
             "and verify with skeleton.audit_skin_weights. The skeleton.* weight verbs "
             "(normalize/prune/set_vertex/copy) only author named alternate profiles and leave base "
             "skinning unchanged."));
    return true;
}


// ===========================================================================
// skeleton.copy_weights - Copy skin weights between meshes
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.copy_weights", "skeleton",
    "Transfer skin weights from a source SkeletalMesh to a target SkeletalMesh by closest vertex, writing the result into a NAMED ALTERNATE skin-weight profile on the target (default CopiedWeights). This does NOT change the target's base skinning; the response reports baseSkinningModified:false. Source and target must share a reference skeleton: every copied influence is translated out of the source section's bone map and back into the target section's, and an influence naming a bone the target section does not carry is dropped and reported as influencesDropped rather than silently attached to another bone. Re-running with the same profileName overwrites that profile instead of registering a second one. Verify with skeleton.describe_skin_weights.",
    RPC_PARAMS(
        RPC_PARAM_REQ("sourceMeshPath", "path", "Source mesh path"),
        RPC_PARAM_REQ("targetMeshPath", "path", "Target mesh path"),
        RPC_PARAM_OPT("profileName", "string", "Named ALTERNATE skin-weight profile to write on the target (default CopiedWeights). Base skinning is never the target"),
        RPC_PARAM_OPT("lodIndex", "integer", "LOD index (default 0)")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString SourceMeshPath = Ctx.GetString(TEXT("sourceMeshPath"));
    FString TargetMeshPath = Ctx.GetString(TEXT("targetMeshPath"));
    FString ProfileName = Ctx.GetString(TEXT("profileName"));
    if (ProfileName.IsEmpty())
    {
        ProfileName = TEXT("CopiedWeights");
    }
    int32 LODIndex = Ctx.GetInt(TEXT("lodIndex"), 0);

    if (SourceMeshPath.IsEmpty() || TargetMeshPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("sourceMeshPath and targetMeshPath are required"));
        return true;
    }

    FString Error;
    bool bSourceWrongType = false;
    USkeletalMesh* SourceMesh = LoadSkeletalMeshFromPathSM(SourceMeshPath, Error, &bSourceWrongType);
    if (!SourceMesh)
    {
        Ctx.SendError(bSourceWrongType ? TEXT("INVALID_ASSET_TYPE") : TEXT("SOURCE_NOT_FOUND"), Error);
        return true;
    }

    bool bTargetWrongType = false;
    USkeletalMesh* TargetMesh = LoadSkeletalMeshFromPathSM(TargetMeshPath, Error, &bTargetWrongType);
    if (!TargetMesh)
    {
        Ctx.SendError(bTargetWrongType ? TEXT("INVALID_ASSET_TYPE") : TEXT("TARGET_NOT_FOUND"), Error);
        return true;
    }

    FSkeletalMeshModel* SourceModel = SourceMesh->GetImportedModel();
    FSkeletalMeshModel* TargetModel = TargetMesh->GetImportedModel();

    // LODIndex < 0 is checked here and not only against the upper bound: a negative index
    // reached TArray::operator[] directly on both models.
    if (!SourceModel || !TargetModel || LODIndex < 0 ||
        LODIndex >= SourceModel->LODModels.Num() ||
        LODIndex >= TargetModel->LODModels.Num())
    {
        Ctx.SendError(TEXT("INVALID_LOD"),
            FString::Printf(TEXT("LOD index %d is not present on both meshes (source has %d, target has %d)"),
                LODIndex,
                SourceModel ? SourceModel->LODModels.Num() : 0,
                TargetModel ? TargetModel->LODModels.Num() : 0));
        return true;
    }

    FSkeletalMeshLODModel& SourceLOD = SourceModel->LODModels[LODIndex];
    FSkeletalMeshLODModel& TargetLOD = TargetModel->LODModels[LODIndex];

    // Gather both meshes' soft-skin vertices (position + per-vertex influences).
    TArray<FSoftSkinVertex> SourceVertices;
    SourceLOD.GetVertices(SourceVertices);
    TArray<FSoftSkinVertex> TargetVertices;
    TargetLOD.GetVertices(TargetVertices);

    if (SourceVertices.Num() == 0)
    {
        Ctx.SendError(TEXT("NO_SOURCE_WEIGHTS"),
            TEXT("Source LOD has no vertices to copy weights from"));
        return true;
    }

    // Closest-vertex transfer into a LOCAL buffer, then one shared persist call.
    //
    // This verb used to keep its own copy of the profile-persist logic: an unconditional
    // AddSkinWeightProfile (USkeletalMesh::AddSkinWeightProfile is a bare TArray::Add, so every
    // re-run appended a duplicate FSkinWeightProfileInfo - board
    // B-copy-weights-duplicates-profile-info) followed by its own RebuildSourceModelInfluences.
    // That second call site is exactly the drift the shared helper existed to prevent: a fix
    // landed in WriteSkinWeightProfile would have missed this verb with no compile error and no
    // failing test. Routing through WriteSkinWeightProfile removes the duplication rather than
    // patching it, and RebuildSourceModelInfluences now requires the LOD model, so a future
    // attempt to re-open a second path fails to compile instead of failing silently.
    TArray<FRawSkinWeight> CopiedWeights;
    TArray<int32> MatchedSourceVertex;
    const int32 VerticesCopied = SkinWeightTransferUtils::CopyClosestVertexWeights(
        SourceVertices, TargetVertices, CopiedWeights, &MatchedSourceVertex);

    // The copied influence slots index the SOURCE section's bone map. Storing them unchanged on
    // the target reads slot N of a source section as slot N of a target section - different real
    // bones on any multi-section mesh. Translate through reference-skeleton indices; anything
    // the target section cannot carry is dropped and counted rather than mis-assigned.
    const int32 InfluencesDropped = SkinWeightTransferUtils::TranslateCopiedWeightsBetweenLODs(
        SourceLOD, TargetLOD, MatchedSourceVertex, CopiedWeights);

    const int32 UnmappedOnWrite = SkinWeightTransferUtils::WriteSkinWeightProfile(
        *TargetMesh, TargetLOD, FName(*ProfileName), CopiedWeights);

    TargetMesh->Build();
    McpSafeAssetSave(TargetMesh);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("sourceMeshPath"), SourceMeshPath);
    Result->SetStringField(TEXT("targetMeshPath"), TargetMeshPath);
    Result->SetStringField(TEXT("profileName"), ProfileName);
    Result->SetNumberField(TEXT("lodIndex"), LODIndex);
    Result->SetNumberField(TEXT("verticesCopied"), VerticesCopied);
    Result->SetNumberField(TEXT("sourceVertices"), SourceVertices.Num());
    // A partial transfer is reported, not hidden: a non-zero count means the two meshes'
    // sections do not carry the same bones and some influences had nowhere to go.
    Result->SetNumberField(TEXT("influencesDropped"), InfluencesDropped);
    if (InfluencesDropped > 0)
    {
        Result->SetStringField(TEXT("transferWarning"),
            FString::Printf(TEXT("%d influences named a bone the target section's bone map does not carry and were dropped. Source and target must share a reference skeleton; check both with skeleton.audit_skin_weights."),
                InfluencesDropped));
    }
    AddProfileWriteDisclosureSM(Result, UnmappedOnWrite);

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.create_cloth_from_section - Author a NEW UClothingAsset from a mesh section
// ===========================================================================
//
// This is the editor's "Create Clothing Data from Section" path
// (UClothingAssetFactoryBase::CreateFromSkeletalMesh). Before this verb existed,
// the cloth namespace could only bind/list an already-existing asset, so a mesh
// that ships with zero clothing assets could never be given simulation through
// the API. The new asset is added to the mesh; pass bindToSection=true (the
// default) to also bind it to the source section so it simulates immediately.

REGISTER_RPC_HANDLER("skeleton.create_cloth_from_section", "skeleton",
    "Author a NEW UClothingAsset from a SkeletalMesh section (editor 'Create Clothing Data from Section'), add it to the mesh, and optionally bind it to that section so Chaos Cloth simulates immediately. This is the only verb that creates cloth; bind_cloth_to_skeletal_mesh and assign_cloth_asset_to_mesh only attach an asset that already exists.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletalMeshPath", "path", "Path to the skeletal mesh"),
        RPC_PARAM_REQ("clothAssetName", "string", "Name for the new cloth asset"),
        RPC_PARAM_OPT("meshLodIndex", "integer", "Source mesh LOD index to extract the section from (default 0)"),
        RPC_PARAM_OPT("sectionIndex", "integer", "Source section index within the LOD to extract (default 0)"),
        RPC_PARAM_OPT("removeFromMesh", "bool", "Remove the source section from the renderable mesh (default false; enable when driving a high-poly mesh with a low-poly cloth section)"),
        RPC_PARAM_OPT("bindToSection", "bool", "Bind the new asset back to the source section so it simulates immediately (default true)")
    ))
{
#if !PINWRIGHT_HAS_CLOTH_CREATE
    Ctx.SendError(TEXT("CLOTH_CREATE_UNSUPPORTED"),
        TEXT("Cloth-asset creation requires the engine's ClothingSystemEditor module, which is not available in this build."));
    return true;
#else
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    FString ClothAssetName = Ctx.GetString(TEXT("clothAssetName"));
    int32 MeshLodIndex = Ctx.GetInt(TEXT("meshLodIndex"), 0);
    int32 SectionIndex = Ctx.GetInt(TEXT("sectionIndex"), 0);
    bool bRemoveFromMesh = Ctx.GetBool(TEXT("removeFromMesh"), false);
    bool bBindToSection = Ctx.GetBool(TEXT("bindToSection"), true);

    if (SkeletalMeshPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletalMeshPath is required"));
        return true;
    }
    if (ClothAssetName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("clothAssetName is required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeletalMesh* Mesh = LoadSkeletalMeshFromPathSM(SkeletalMeshPath, Error, &bWrongType);
    if (!Mesh)
    {
        SendMeshPathErrorSM(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    // Reject a duplicate name up front: AddClothingAsset would otherwise silently
    // produce a second asset that the name-based bind/assign verbs can't disambiguate.
    if (FindClothAssetByNameSM(Mesh, ClothAssetName))
    {
        Ctx.SendError(TEXT("CLOTH_NAME_IN_USE"),
            FString::Printf(TEXT("A cloth asset named '%s' already exists on the mesh"), *ClothAssetName));
        return true;
    }

    FClothingSystemEditorInterfaceModule& ClothingEditorModule =
        FModuleManager::LoadModuleChecked<FClothingSystemEditorInterfaceModule>(TEXT("ClothingSystemEditorInterface"));
    UClothingAssetFactoryBase* AssetFactory = ClothingEditorModule.GetClothingAssetFactory();
    if (!AssetFactory)
    {
        Ctx.SendError(TEXT("CLOTH_CREATE_UNSUPPORTED"),
            TEXT("No clothing asset factory is registered (Chaos Cloth editor support unavailable)."));
        return true;
    }

    Mesh->Modify();

    FSkeletalMeshClothBuildParams Params;
    Params.AssetName = ClothAssetName;
    Params.LodIndex = MeshLodIndex;
    Params.SourceSection = SectionIndex;
    Params.bRemoveFromMesh = bRemoveFromMesh;
    Params.bRemapParameters = false;
    Params.TargetLod = 0;

    UClothingAssetBase* NewClothAsset = AssetFactory->CreateFromSkeletalMesh(Mesh, Params);
    if (!NewClothAsset)
    {
        Ctx.SendError(TEXT("CLOTH_CREATE_FAILED"),
            FString::Printf(TEXT("Failed to create cloth asset from LOD %d section %d (is the section index valid?)"),
                MeshLodIndex, SectionIndex));
        return true;
    }

    Mesh->AddClothingAsset(NewClothAsset);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("skeletalMeshPath"), SkeletalMeshPath);
    Result->SetStringField(TEXT("clothAssetName"), NewClothAsset->GetName());
    Result->SetNumberField(TEXT("meshLodIndex"), MeshLodIndex);
    Result->SetNumberField(TEXT("sectionIndex"), SectionIndex);

    bool bBound = false;
    if (bBindToSection)
    {
        bBound = NewClothAsset->BindToSkeletalMesh(Mesh, MeshLodIndex, SectionIndex, /*AssetLodIndex*/ 0);
        if (!bBound)
        {
            // Creation succeeded but binding did not; keep the new asset and report
            // the partial outcome rather than discarding the authored cloth.
            Result->SetStringField(TEXT("bindWarning"),
                TEXT("Cloth asset created and added to the mesh, but binding to the source section failed; bind it explicitly with skeleton.bind_cloth_to_skeletal_mesh."));
        }
    }
    Result->SetBoolField(TEXT("bound"), bBound);

    Mesh->Build();
    McpSafeAssetSave(Mesh);

    Ctx.SendSuccess(Result);
    return true;
#endif // PINWRIGHT_HAS_CLOTH_CREATE
}


// ===========================================================================
// skeleton.bind_cloth_to_skeletal_mesh - Bind cloth asset to skeletal mesh
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.bind_cloth_to_skeletal_mesh", "skeleton",
    "Bind an ALREADY-EXISTING UClothingAsset (named by clothAssetName, must already be on the mesh else CLOTH_NOT_FOUND) to a section, or list the mesh's existing clothing assets when clothAssetName is omitted. Does NOT create a cloth asset - no MCP verb does.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletalMeshPath", "path", "Path to the skeletal mesh"),
        RPC_PARAM_OPT("clothAssetName", "string", "Name of the cloth asset to bind"),
        RPC_PARAM_OPT("meshLodIndex", "integer", "Mesh LOD index (default 0)"),
        RPC_PARAM_OPT("sectionIndex", "integer", "Section index (default 0)"),
        RPC_PARAM_OPT("assetLodIndex", "integer", "Asset LOD index (default 0)")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    FString ClothAssetName = Ctx.GetString(TEXT("clothAssetName"));
    int32 MeshLodIndex = Ctx.GetInt(TEXT("meshLodIndex"), 0);
    int32 SectionIndex = Ctx.GetInt(TEXT("sectionIndex"), 0);
    int32 AssetLodIndex = Ctx.GetInt(TEXT("assetLodIndex"), 0);

    if (SkeletalMeshPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletalMeshPath is required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeletalMesh* Mesh = LoadSkeletalMeshFromPathSM(SkeletalMeshPath, Error, &bWrongType);
    if (!Mesh)
    {
        SendMeshPathErrorSM(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("skeletalMeshPath"), SkeletalMeshPath);

    if (!ClothAssetName.IsEmpty())
    {
        BindNamedClothToSection(Ctx, Mesh, ClothAssetName, MeshLodIndex, SectionIndex, AssetLodIndex, Result);
        return true;
    }
    else
    {
        const auto& ClothingAssets = Mesh->GetMeshClothingAssets();

        TArray<TSharedPtr<FJsonValue>> ClothingArray;
        for (const auto& ClothAssetPtr : ClothingAssets)
        {
            UClothingAssetBase* ClothAsset = ClothAssetPtr.Get();
            if (!ClothAsset) continue;

            TSharedPtr<FJsonObject> ClothObj = MakeShareable(new FJsonObject());
            ClothObj->SetStringField(TEXT("name"), ClothAsset->GetName());
            if (UClothingAssetCommon* ClothAssetCommon = Cast<UClothingAssetCommon>(ClothAsset))
            {
                ClothObj->SetNumberField(TEXT("numLods"), ClothAssetCommon->GetNumLods());
            }
            ClothingArray.Add(MakeShareable(new FJsonValueObject(ClothObj)));
        }

        Result->SetArrayField(TEXT("availableClothAssets"), ClothingArray);
        Result->SetNumberField(TEXT("clothingAssetCount"), ClothingAssets.Num());

        Ctx.SendSuccess(Result);
    }

    return true;
}


// ===========================================================================
// skeleton.assign_cloth_asset_to_mesh - List/assign cloth assets
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.assign_cloth_asset_to_mesh", "skeleton",
    "Attach an existing UClothingAsset (named by clothAssetName) to a specific section of a SkeletalMesh, or share one cloth asset across sections by calling once per section. Omit clothAssetName to list the mesh's existing clothing assets. The asset must already exist (create it with skeleton.create_cloth_from_section); this verb does not create cloth.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletalMeshPath", "path", "Path to the skeletal mesh"),
        RPC_PARAM_OPT("clothAssetName", "string", "Name of an existing cloth asset to attach; omit to list the mesh's clothing assets"),
        RPC_PARAM_OPT("sectionIndex", "integer", "Section index to attach the asset to (default 0)"),
        RPC_PARAM_OPT("meshLodIndex", "integer", "Mesh LOD index of the section (default 0)"),
        RPC_PARAM_OPT("assetLodIndex", "integer", "Cloth asset LOD index to bind (default 0)")
    ))
{
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    FString ClothAssetName = Ctx.GetString(TEXT("clothAssetName"));
    int32 SectionIndex = Ctx.GetInt(TEXT("sectionIndex"), 0);
    int32 MeshLodIndex = Ctx.GetInt(TEXT("meshLodIndex"), 0);
    int32 AssetLodIndex = Ctx.GetInt(TEXT("assetLodIndex"), 0);

    if (SkeletalMeshPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("skeletalMeshPath is required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeletalMesh* Mesh = LoadSkeletalMeshFromPathSM(SkeletalMeshPath, Error, &bWrongType);
    if (!Mesh)
    {
        SendMeshPathErrorSM(Ctx, TEXT("MESH_NOT_FOUND"), Error, bWrongType);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("skeletalMeshPath"), SkeletalMeshPath);

    // Attach mode: bind the named existing asset to the requested section
    // (same find/bind/save/report path as bind_cloth_to_skeletal_mesh).
    if (!ClothAssetName.IsEmpty())
    {
        BindNamedClothToSection(Ctx, Mesh, ClothAssetName, MeshLodIndex, SectionIndex, AssetLodIndex, Result);
        return true;
    }

    // List mode: enumerate the mesh's existing clothing assets.
    TArray<TSharedPtr<FJsonValue>> ClothingArray;
    for (const auto& ClothAssetPtr : Mesh->GetMeshClothingAssets())
    {
        UClothingAssetBase* ClothAsset = ClothAssetPtr.Get();
        if (!ClothAsset) continue;

        TSharedPtr<FJsonObject> ClothObj = MakeShareable(new FJsonObject());
        ClothObj->SetStringField(TEXT("name"), ClothAsset->GetName());
        ClothingArray.Add(MakeShareable(new FJsonValueObject(ClothObj)));
    }

    Result->SetArrayField(TEXT("clothingAssets"), ClothingArray);
    Result->SetNumberField(TEXT("count"), ClothingArray.Num());

    Ctx.SendSuccess(Result);
    return true;
}
