// Copyright (c) 2026 Alexander Penkin. MIT License.

// SkinWeightTransferUtils.h
//
// Closest-vertex skin-weight transfer used by skeleton.copy_weights. Extracted
// into a named-namespace header (mirroring AnimationAuthoringHelpers) so the
// transfer math is exercised directly by unit tests without driving the full
// USkeletalMesh::Build() DDC pipeline, and so the production handler and the
// regression test share one implementation (no copy). Uses a named namespace
// rather than an anonymous one in a header, per the plugin's Unity-build convention.

#pragma once

#include "CoreMinimal.h"
#include "GPUSkinPublicDefs.h"                        // MAX_TOTAL_INFLUENCES
#include "SkeletalMeshTypes.h"                        // FSoftSkinVertex (full def via SkeletalMeshLODModel.h on newer UE)
#include "Engine/SkeletalMesh.h"                       // USkeletalMesh (WriteSkinWeightProfile)
#include "Rendering/SkeletalMeshLODModel.h"           // FSoftSkinVertex full definition (forward-declared only by SkeletalMeshTypes.h on UE 5.3); FSkeletalMeshLODModel (WriteSkinWeightProfile)
#include "Rendering/SkeletalMeshLODImporterData.h"    // SkeletalMeshImportData::FVertInfluence
// SkinWeightProfile.h (FRawSkinWeight) moved from Animation/ to Rendering/ in UE 5.8.
#if __has_include("Rendering/SkinWeightProfile.h")
#include "Rendering/SkinWeightProfile.h"
#elif __has_include("Animation/SkinWeightProfile.h")
#include "Animation/SkinWeightProfile.h"
#endif

namespace SkinWeightTransferUtils
{
    // FRawSkinWeight / FSoftSkinVertex store per-influence weights as uint16 fixed
    // point: 0 == no weight, 65535 == full weight. This scale converts between that
    // fixed-point form and the normalized [0,1] float SourceModelInfluences expects.
    // Shared so the forward (set_weights) and inverse (copy_weights rebuild) conversions
    // stay in lockstep instead of duplicating the bare 65535 literal across handlers.
    constexpr float RawSkinWeightScale = 65535.0f;

    // uint16 fixed-point influence weight -> normalized [0,1] float.
    inline float RawWeightToFloat(uint16 RawWeight)
    {
        return static_cast<float>(RawWeight) / RawSkinWeightScale;
    }

    // normalized [0,1] float -> uint16 fixed-point influence weight (clamped).
    inline uint16 FloatToRawWeight(float Weight)
    {
        return static_cast<uint16>(FMath::Clamp(Weight, 0.0f, 1.0f) * RawSkinWeightScale);
    }

    // Tolerance for "weights sum to 1.0". One uint16 quantization step is 1/65535 ~= 1.5e-5,
    // and a vertex packs up to MAX_TOTAL_INFLUENCES slots, so a few-LSB rounding band is
    // expected on a correctly-normalized profile. 1e-3 comfortably covers that. Declared
    // here (above the first user) so both the normalize/prune edits and the validity
    // summary share the one tolerance.
    constexpr float NormalizedWeightSumTolerance = 1.0e-3f;

    // ---- Bone index spaces -----------------------------------------------------------------
    //
    // TWO index spaces meet in this file and confusing them is silent, not loud.
    //
    //  * SECTION-LOCAL. FSoftSkinVertex::InfluenceBones and
    //    FImportedSkinWeightProfileData::SkinWeights both store a slot into
    //    FSkelMeshSection::BoneMap, which is itself "the bones which are used by the vertices
    //    of this section. Indices of bones in the USkeletalMesh::RefSkeleton array"
    //    (UE 5.8 Rendering/SkeletalMeshLODModel.h:74-75). The engine reads a profile's
    //    SkinWeights back through that map to recover a real bone -
    //    `Section.BoneMap[SkinWeight.InfluenceBones[i]]` at MeshUtilities.cpp:5774, with the
    //    section resolved at :5761-5762.
    //
    //  * REFERENCE-SKELETON. FImportedSkinWeightProfileData::SourceModelInfluences is the
    //    pre-chunking list ("This is the result of the imported data before the chunking",
    //    Rendering/SkinWeightProfile.h:75-77). Its BoneIndex values are unioned straight into
    //    the rebuilt chunk bone map by SkeletalMeshTools::ChunkSkinnedVertices
    //    (MeshUtilities.cpp:4216-4226), so they MUST be reference-skeleton indices.
    //
    // On a single-section mesh BoneMap is effectively the identity over the range in use, so
    // the confusion is invisible. On a multi-section mesh slot 3 of section 0 and slot 3 of
    // section 4 are different bones, and a mix-up re-chunks the mesh around the wrong ones
    // with no error and no failed return (board B-skin-weight-transfer-writes-section-local-bone-indices).
    //
    // Known engine inconsistency, recorded so nobody re-deriving this is misled: inside
    // FMeshUtilities::CreateImportDataFromLODModel, MeshUtilities.cpp:5774 resolves through
    // BoneMap for AlternateInfluence.Influences and :5778 does NOT for SourceModelInfluences,
    // in the same loop iteration. That is a legacy conversion path; the authoritative contract
    // is the ChunkSkinnedVertices consumer above.

    // Flat LOD vertex index -> owning section index, or INDEX_NONE when the index falls
    // outside the LOD's sections. Deliberately NOT FSkeletalMeshLODModel::GetSectionFromVertexIndex:
    // that function's out-of-range guard is commented out (UE 5.8
    // SkeletalMeshLODModel.cpp:1008-1009), so it answers "last section, vertex 0" for an index
    // past the end and "section 0" for a LOD with no sections at all - a silent wrong answer
    // where every caller here needs a detectable one. Accumulates SoftVertices.Num() per
    // section because that is exactly how GetVertices() concatenates them
    // (SkeletalMeshLODModel.cpp:1036-1053).
    inline int32 FindSectionForVertex(const FSkeletalMeshLODModel& LODModel, int32 VertexIndex)
    {
        if (VertexIndex < 0)
        {
            return INDEX_NONE;
        }
        int32 Running = 0;
        for (int32 SectionIndex = 0; SectionIndex < LODModel.Sections.Num(); ++SectionIndex)
        {
            const int32 SectionVertices = LODModel.Sections[SectionIndex].SoftVertices.Num();
            if (VertexIndex < Running + SectionVertices)
            {
                return SectionIndex;
            }
            Running += SectionVertices;
        }
        return INDEX_NONE;
    }

    // Section-local influence slot -> reference-skeleton bone index for the section that owns
    // VertexIndex. Returns false (OutRefSkeletonBone left at INDEX_NONE) when the vertex is
    // outside the LOD's sections or the slot has no BoneMap entry.
    //
    // Refusing rather than falling back to bone 0 is the point: bone 0 is the root on every
    // skeleton, so a fallback would fabricate a plausible-looking root influence that passes
    // every downstream validity check. SkinAuditHandler.cpp:344-351 makes the same choice and
    // counts the refusals.
    inline bool ResolveSectionLocalBone(const FSkeletalMeshLODModel& LODModel, int32 VertexIndex,
        int32 SectionLocalBone, int32& OutRefSkeletonBone)
    {
        OutRefSkeletonBone = INDEX_NONE;
        const int32 SectionIndex = FindSectionForVertex(LODModel, VertexIndex);
        if (SectionIndex == INDEX_NONE)
        {
            return false;
        }
        const FSkelMeshSection& Section = LODModel.Sections[SectionIndex];
        if (!Section.BoneMap.IsValidIndex(SectionLocalBone))
        {
            return false;
        }
        OutRefSkeletonBone = static_cast<int32>(Section.BoneMap[SectionLocalBone]);
        return true;
    }

    // The inverse: reference-skeleton bone index -> the section-local slot the section owning
    // VertexIndex uses for it. Returns false when that section's BoneMap does not list the
    // bone. A section's BoneMap is fixed until the next re-chunk, so there is no slot to
    // invent; writing an arbitrary one would weight the vertex to a different bone entirely.
    // This is what lets skeleton.set_vertex_weights accept a caller-facing bone in ONE stated
    // space (reference-skeleton, the same space skeleton.list_bones reports) and still write
    // the section-local slot the storage requires.
    inline bool ResolveBoneToSectionLocalSlot(const FSkeletalMeshLODModel& LODModel, int32 VertexIndex,
        int32 RefSkeletonBone, int32& OutSectionLocalBone)
    {
        OutSectionLocalBone = INDEX_NONE;
        const int32 SectionIndex = FindSectionForVertex(LODModel, VertexIndex);
        if (SectionIndex == INDEX_NONE || RefSkeletonBone < 0)
        {
            return false;
        }
        const TArray<FBoneIndexType>& BoneMap = LODModel.Sections[SectionIndex].BoneMap;
        const int32 Slot = BoneMap.IndexOfByKey(static_cast<FBoneIndexType>(RefSkeletonBone));
        if (Slot == INDEX_NONE)
        {
            return false;
        }
        OutSectionLocalBone = Slot;
        return true;
    }

    // Walks one vertex's non-zero influences, invoking Fn(boneIndex, dequantizedWeight)
    // for each. Influences are packed largest-first and zero-padded, so the first zero
    // weight ends the list. Owns that packed-layout contract (largest-first, zero-
    // terminated, RawWeightToFloat de-quantization) so its readers don't each re-encode
    // it. The boneIndex handed to Fn is in whatever space the array holds - SECTION-LOCAL
    // for a profile's SkinWeights or a captured soft vertex, reference-skeleton for the
    // output of ReadBaseSkinningRefSkeletonSpace. Returns the non-zero influence count.
    inline int32 ForEachNonZeroInfluence(const FRawSkinWeight& Weight,
        TFunctionRef<void(int32 BoneIndex, float DequantizedWeight)> Fn)
    {
        int32 NumInfluences = 0;
        for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
        {
            const uint16 RawWeight = Weight.InfluenceWeights[Influence];
            if (RawWeight == 0)
            {
                break;  // packed largest-first: trailing slots are all zero
            }
            ++NumInfluences;
            Fn(static_cast<int32>(Weight.InfluenceBones[Influence]), RawWeightToFloat(RawWeight));
        }
        return NumInfluences;
    }

    // For each target vertex, finds the closest source vertex by position and
    // copies its influence bones/weights into OutSkinWeights[i] — the
    // closest-vertex transfer skeleton.copy_weights advertises. Both
    // FSoftSkinVertex and FRawSkinWeight store influences in the same
    // uint16[MAX_TOTAL_INFLUENCES] layout, so a matched source vertex's influences
    // copy straight across as a verbatim array copy. Returns the number of target
    // vertices written (== the target vertex count when the source is non-empty).
    // When SourceVertices is empty, OutSkinWeights is left untouched and 0 is
    // returned — the caller must treat that as a failed (not zeroed) transfer.
    //
    // THE COPIED BONE SLOTS ARE IN THE SOURCE MESH'S SECTION-LOCAL SPACE. They are copied
    // verbatim, which is correct here and wrong everywhere downstream: they index the SOURCE
    // LOD's section BoneMaps, not the target's. Run TranslateCopiedWeightsBetweenLODs over the
    // result before persisting it, and pass OutMatchedSourceVertex so that function knows which
    // source vertex (and therefore which source section) each target vertex inherited from.
    inline int32 CopyClosestVertexWeights(
        const TArray<FSoftSkinVertex>& SourceVertices,
        const TArray<FSoftSkinVertex>& TargetVertices,
        TArray<FRawSkinWeight>& OutSkinWeights,
        TArray<int32>* OutMatchedSourceVertex = nullptr)
    {
        if (SourceVertices.Num() == 0)
        {
            return 0;
        }

        OutSkinWeights.SetNum(TargetVertices.Num());
        if (OutMatchedSourceVertex)
        {
            OutMatchedSourceVertex->SetNum(TargetVertices.Num());
        }

        for (int32 TargetIdx = 0; TargetIdx < TargetVertices.Num(); ++TargetIdx)
        {
            const FVector3f& TargetPos = TargetVertices[TargetIdx].Position;

            int32 ClosestSourceIdx = 0;
            float ClosestDistSq = TNumericLimits<float>::Max();
            for (int32 SourceIdx = 0; SourceIdx < SourceVertices.Num(); ++SourceIdx)
            {
                const float DistSq = FVector3f::DistSquared(TargetPos, SourceVertices[SourceIdx].Position);
                if (DistSq < ClosestDistSq)
                {
                    ClosestDistSq = DistSq;
                    ClosestSourceIdx = SourceIdx;
                }
            }

            const FSoftSkinVertex& MatchedSource = SourceVertices[ClosestSourceIdx];
            FRawSkinWeight& TargetWeight = OutSkinWeights[TargetIdx];
            static_assert(sizeof(TargetWeight.InfluenceBones) == sizeof(MatchedSource.InfluenceBones),
                "FRawSkinWeight and FSoftSkinVertex influence-bone layouts must match for a verbatim copy");
            static_assert(sizeof(TargetWeight.InfluenceWeights) == sizeof(MatchedSource.InfluenceWeights),
                "FRawSkinWeight and FSoftSkinVertex influence-weight layouts must match for a verbatim copy");
            FMemory::Memcpy(TargetWeight.InfluenceBones, MatchedSource.InfluenceBones, sizeof(TargetWeight.InfluenceBones));
            FMemory::Memcpy(TargetWeight.InfluenceWeights, MatchedSource.InfluenceWeights, sizeof(TargetWeight.InfluenceWeights));

            if (OutMatchedSourceVertex)
            {
                (*OutMatchedSourceVertex)[TargetIdx] = ClosestSourceIdx;
            }
        }

        // Every target vertex is written unconditionally above, so the count is the
        // full target vertex count (the empty-source case already returned 0).
        return TargetVertices.Num();
    }


    // Result of an in-place per-vertex weight edit (normalize / prune). Lets the
    // caller emit a machine-readable count so a no-op is observable instead of a
    // bare success echo (the B-skeleton-auto-skin-weights-noop-rebuild defect:
    // the handlers used to call Build() and report success without touching a
    // single influence).
    struct FWeightEditResult
    {
        // Non-zero influences dropped by a prune pass (0 for a pure normalize).
        int32 InfluencesRemoved = 0;
        // True if any bone/weight byte of this vertex changed.
        bool bChanged = false;
    };

    // Renormalizes one vertex's non-zero influences so their de-quantized weights
    // sum to 1.0, writing back the re-quantized uint16 weights in place. A vertex
    // with no influence, or one already summing to ~1.0, is left untouched
    // (bChanged=false). The largest-first / zero-terminated packing is preserved
    // because only the existing non-zero slots are rescaled. Pure over the raw
    // weight struct so it is unit-testable without driving USkeletalMesh::Build().
    inline FWeightEditResult NormalizeRawSkinWeight(FRawSkinWeight& Weight)
    {
        FWeightEditResult Out;

        float Sum = 0.0f;
        int32 NumInfluences = 0;
        for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
        {
            const uint16 RawWeight = Weight.InfluenceWeights[Influence];
            if (RawWeight == 0)
            {
                break;  // packed largest-first: trailing slots are all zero
            }
            ++NumInfluences;
            Sum += RawWeightToFloat(RawWeight);
        }

        // No influence to normalize, or already normalized within quantization band.
        if (NumInfluences == 0 || FMath::IsNearlyEqual(Sum, 1.0f, NormalizedWeightSumTolerance) || Sum <= 0.0f)
        {
            return Out;
        }

        const float InvSum = 1.0f / Sum;
        for (int32 Influence = 0; Influence < NumInfluences; ++Influence)
        {
            const float Rescaled = RawWeightToFloat(Weight.InfluenceWeights[Influence]) * InvSum;
            const uint16 NewRaw = FloatToRawWeight(Rescaled);
            if (NewRaw != Weight.InfluenceWeights[Influence])
            {
                Weight.InfluenceWeights[Influence] = NewRaw;
                Out.bChanged = true;
            }
        }
        return Out;
    }

    // Drops every influence whose de-quantized weight is below Threshold, repacks
    // the survivors largest-first (zero-terminated), then renormalizes them to sum
    // 1.0. Returns how many influences were removed and whether anything changed.
    // A vertex with no surviving influence (all below threshold) is left untouched
    // and reports 0 removed, so a too-aggressive threshold can never strip a vertex
    // to an unskinned zero-fill. Pure over the raw weight struct (unit-testable).
    inline FWeightEditResult PruneRawSkinWeight(FRawSkinWeight& Weight, float Threshold)
    {
        FWeightEditResult Out;

        // Gather surviving (bone, weight) pairs in their existing largest-first order.
        uint16 KeepBones[MAX_TOTAL_INFLUENCES];
        uint16 KeepWeights[MAX_TOTAL_INFLUENCES];
        int32 NumKept = 0;
        int32 NumInfluences = 0;
        for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
        {
            const uint16 RawWeight = Weight.InfluenceWeights[Influence];
            if (RawWeight == 0)
            {
                break;  // packed largest-first: trailing slots are all zero
            }
            ++NumInfluences;
            if (RawWeightToFloat(RawWeight) >= Threshold)
            {
                KeepBones[NumKept] = Weight.InfluenceBones[Influence];
                KeepWeights[NumKept] = RawWeight;
                ++NumKept;
            }
        }

        // NumKept == 0: every influence is below threshold; pruning them all would leave
        // the vertex unskinned (a destructive zero-fill), so refuse and report no change.
        // NumKept == NumInfluences: nothing crossed the threshold, so there is nothing to
        // drop. Either way leave the vertex untouched.
        if (NumKept == 0 || NumKept == NumInfluences)
        {
            return Out;
        }

        Out.InfluencesRemoved = NumInfluences - NumKept;

        // Rewrite the packed influence arrays from the survivors, zero-padding the tail.
        for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
        {
            const uint16 NewBone = Influence < NumKept ? KeepBones[Influence] : 0;
            const uint16 NewWeight = Influence < NumKept ? KeepWeights[Influence] : 0;
            if (Weight.InfluenceBones[Influence] != NewBone || Weight.InfluenceWeights[Influence] != NewWeight)
            {
                Out.bChanged = true;
            }
            Weight.InfluenceBones[Influence] = NewBone;
            Weight.InfluenceWeights[Influence] = NewWeight;
        }

        // Renormalize the survivors so the pruned vertex still sums to 1.0.
        const FWeightEditResult NormResult = NormalizeRawSkinWeight(Weight);
        Out.bChanged = Out.bChanged || NormResult.bChanged;
        return Out;
    }

    // Rewrites weights produced by CopyClosestVertexWeights from the SOURCE LOD's section-local
    // bone slots into the TARGET LOD's, going through reference-skeleton indices in between.
    //
    // This is the step a cross-mesh transfer cannot skip. CopyClosestVertexWeights memcpys the
    // matched source vertex's influence slots, and those slots index the SOURCE section's
    // BoneMap. Storing them unchanged on the target means slot 2 of some source section is read
    // as slot 2 of whatever target section owns the target vertex - two different real bones on
    // any mesh with more than one section, with no error and no failed return.
    //
    // Both meshes must share a reference skeleton for this to be meaningful; that is already
    // this verb's documented precondition ("source and target should share a compatible
    // skeleton"). An influence whose bone is absent from the target section's BoneMap is DROPPED
    // and counted rather than substituted: a section's BoneMap is fixed until the next re-chunk,
    // so there is no honest slot to write, and defaulting to 0 would silently weight the vertex
    // to the root. Survivors are repacked so the largest-first, zero-terminated layout holds.
    //
    // Returns the number of influences dropped. A non-zero return means the transfer is partial
    // and the caller must say so.
    inline int32 TranslateCopiedWeightsBetweenLODs(
        const FSkeletalMeshLODModel& SourceLOD,
        const FSkeletalMeshLODModel& TargetLOD,
        const TArray<int32>& MatchedSourceVertex,
        TArray<FRawSkinWeight>& InOutSkinWeights)
    {
        int32 DroppedInfluences = 0;

        for (int32 TargetIdx = 0; TargetIdx < InOutSkinWeights.Num(); ++TargetIdx)
        {
            const int32 SourceIdx = MatchedSourceVertex.IsValidIndex(TargetIdx)
                ? MatchedSourceVertex[TargetIdx]
                : INDEX_NONE;

            FRawSkinWeight& Weight = InOutSkinWeights[TargetIdx];

            uint16 KeptBones[MAX_TOTAL_INFLUENCES] = {};
            uint16 KeptWeights[MAX_TOTAL_INFLUENCES] = {};
            int32 NumKept = 0;
            int32 DroppedOnThisVertex = 0;

            for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
            {
                const uint16 RawWeight = Weight.InfluenceWeights[Influence];
                if (RawWeight == 0)
                {
                    break;  // packed largest-first: trailing slots are all zero
                }

                int32 RefSkeletonBone = INDEX_NONE;
                int32 TargetLocalBone = INDEX_NONE;
                if (SourceIdx == INDEX_NONE
                    || !ResolveSectionLocalBone(SourceLOD, SourceIdx,
                        static_cast<int32>(Weight.InfluenceBones[Influence]), RefSkeletonBone)
                    || !ResolveBoneToSectionLocalSlot(TargetLOD, TargetIdx, RefSkeletonBone, TargetLocalBone))
                {
                    ++DroppedInfluences;
                    ++DroppedOnThisVertex;
                    continue;
                }

                KeptBones[NumKept] = static_cast<uint16>(TargetLocalBone);
                KeptWeights[NumKept] = RawWeight;
                ++NumKept;
            }

            for (int32 Slot = 0; Slot < MAX_TOTAL_INFLUENCES; ++Slot)
            {
                Weight.InfluenceBones[Slot] = Slot < NumKept ? static_cast<FBoneIndexType>(KeptBones[Slot]) : FBoneIndexType(0);
                Weight.InfluenceWeights[Slot] = Slot < NumKept ? KeptWeights[Slot] : uint16(0);
            }

            // Dropping influences leaves THIS vertex's survivors summing below 1.0, which reads
            // as degenerate rather than as "some influences were lost". Renormalize so the
            // vertex is at least valid skinning; the drop count is what tells the caller the
            // transfer was partial. Gated on the per-vertex count, not the running total, so a
            // single bad vertex does not drag every later one through a pointless rescale.
            if (NumKept > 0 && DroppedOnThisVertex > 0)
            {
                NormalizeRawSkinWeight(Weight);
            }
        }

        return DroppedInfluences;
    }

    // Aggregate result of a whole-array normalize / prune pass, for the handler's
    // machine-readable response (so the no-op is observable per the
    // B-skeleton-auto-skin-weights-noop-rebuild defect).
    struct FWeightArrayEditResult
    {
        int32 VerticesChanged = 0;     // vertices whose influences were rewritten
        int32 InfluencesRemoved = 0;   // total influences dropped (prune only)
    };

    // Renormalizes every vertex's influences in SkinWeights in place to sum 1.0.
    // Returns how many vertices were actually changed (already-normalized vertices
    // are skipped), so a caller can report verticesNormalized instead of a bare echo.
    inline FWeightArrayEditResult NormalizeSkinWeights(TArray<FRawSkinWeight>& SkinWeights)
    {
        FWeightArrayEditResult Out;
        for (FRawSkinWeight& Weight : SkinWeights)
        {
            if (NormalizeRawSkinWeight(Weight).bChanged)
            {
                ++Out.VerticesChanged;
            }
        }
        return Out;
    }

    // Prunes (then renormalizes) every vertex's influences in SkinWeights in place,
    // dropping influences below Threshold. Returns the count of changed vertices and
    // the total influences removed, so prune_weights can report influencesRemoved
    // and stop accept-then-discarding its threshold.
    inline FWeightArrayEditResult PruneSkinWeights(TArray<FRawSkinWeight>& SkinWeights, float Threshold)
    {
        FWeightArrayEditResult Out;
        for (FRawSkinWeight& Weight : SkinWeights)
        {
            const FWeightEditResult VertResult = PruneRawSkinWeight(Weight, Threshold);
            if (VertResult.bChanged)
            {
                ++Out.VerticesChanged;
            }
            Out.InfluencesRemoved += VertResult.InfluencesRemoved;
        }
        return Out;
    }

    // Captures a LOD's current base skinning (the per-vertex influences from
    // GetVertices()'s FSoftSkinVertex array) into a flat FRawSkinWeight array, one
    // entry per vertex. FSoftSkinVertex and FRawSkinWeight share the same
    // uint16[MAX_TOTAL_INFLUENCES] influence layout, so the bones/weights copy across
    // verbatim. This is the read side that lets normalize/prune operate on the real
    // base skinning (which lives in the section soft-vertices, not in a named profile)
    // and then persist the edit through a named profile that Build() re-chunks.
    //
    // The captured bone slots are SECTION-LOCAL, unchanged from the soft vertices, because
    // that is the space a profile's SkinWeights must be stored in. Do not surface them to a
    // caller as bone indices: use ReadBaseSkinningRefSkeletonSpace for anything a human or an
    // agent will read. RebuildSourceModelInfluences does the crossing on the way out.
    inline void CaptureBaseSkinWeights(
        const TArray<FSoftSkinVertex>& Vertices,
        TArray<FRawSkinWeight>& OutSkinWeights)
    {
        OutSkinWeights.SetNum(Vertices.Num());
        for (int32 VertIdx = 0; VertIdx < Vertices.Num(); ++VertIdx)
        {
            const FSoftSkinVertex& Source = Vertices[VertIdx];
            FRawSkinWeight& Dest = OutSkinWeights[VertIdx];
            static_assert(sizeof(Dest.InfluenceBones) == sizeof(Source.InfluenceBones),
                "FRawSkinWeight and FSoftSkinVertex influence-bone layouts must match for a verbatim capture");
            static_assert(sizeof(Dest.InfluenceWeights) == sizeof(Source.InfluenceWeights),
                "FRawSkinWeight and FSoftSkinVertex influence-weight layouts must match for a verbatim capture");
            FMemory::Memcpy(Dest.InfluenceBones, Source.InfluenceBones, sizeof(Dest.InfluenceBones));
            FMemory::Memcpy(Dest.InfluenceWeights, Source.InfluenceWeights, sizeof(Dest.InfluenceWeights));
        }
    }

    // Chooses the correct SOURCE for an in-place weight edit (normalize_weights /
    // prune_weights) that persists through the named profile ProfileName on this LOD.
    // If that profile already holds authored per-vertex influences (e.g. an earlier
    // set_vertex_weights populated it), the edit must run over THOSE — WriteSkinWeightProfile
    // overwrites the profile wholesale, so re-seeding from base skinning would silently
    // discard the authored edits (and, because base skinning already sums to ~1.0, a
    // normalize pass would report verticesNormalized:0 while replacing the whole profile).
    // Only when the profile is absent or empty on this LOD do we fall back to the LOD's
    // base section skinning via CaptureBaseSkinWeights. Returns true when it seeded from the
    // existing profile, false when it fell back to base skinning (lets a caller/test tell
    // which path ran). Kept next to CaptureBaseSkinWeights / WriteSkinWeightProfile so the
    // seed/edit/persist contract lives in one place; pure over the LOD model (no Build()),
    // so it is unit-testable directly.
    inline bool SeedWeightEditSource(
        const FSkeletalMeshLODModel& LODModel,
        FName ProfileName,
        TArray<FRawSkinWeight>& OutSkinWeights)
    {
        const FImportedSkinWeightProfileData* ExistingProfile = LODModel.SkinWeightProfiles.Find(ProfileName);
        if (ExistingProfile && ExistingProfile->SkinWeights.Num() > 0)
        {
            OutSkinWeights = ExistingProfile->SkinWeights;
            return true;
        }

        TArray<FSoftSkinVertex> Vertices;
        LODModel.GetVertices(Vertices);
        CaptureBaseSkinWeights(Vertices, OutSkinWeights);
        return false;
    }

    // Per-vertex influence sample for skeleton.describe_skin_weights spot-checking:
    // one vertex's non-zero (boneIndex, weight) influences, de-quantized to [0,1].
    struct FSkinWeightVertexSample
    {
        int32 VertexIndex = INDEX_NONE;
        // Non-zero influences only (trailing zero-padded slots dropped), largest-first
        // as stored. Parallel arrays so the handler can emit {boneIndex, weight} objects.
        // Inline storage (max influences fits on the stack) avoids a per-sample heap alloc.
        TArray<int32, TInlineAllocator<MAX_TOTAL_INFLUENCES>> BoneIndices;
        TArray<float, TInlineAllocator<MAX_TOTAL_INFLUENCES>> Weights;
        // Sum of this vertex's de-quantized influence weights (~1.0 when normalized).
        float WeightSum = 0.0f;
    };

    // Read-only validity summary of one skin-weight profile's per-vertex influences.
    // Backs skeleton.describe_skin_weights so a zero-filled or un-normalized profile
    // (e.g. the masked B-skeleton-copy-weights-noop-zero-fill defect) is observable
    // through the API. Pure over the raw weight array so it is unit-testable directly,
    // mirroring CopyClosestVertexWeights / RebuildSourceModelInfluences.
    struct FSkinWeightProfileSummary
    {
        int32 VertexCount = 0;
        // Highest count of non-zero influences on any single vertex.
        int32 MaxInfluencesPerVertex = 0;
        // Vertices whose de-quantized influence weights sum to ~1.0 (a valid, normalized
        // skinning vertex).
        int32 NormalizedVertexCount = 0;
        // Vertices with no influence at all (all weights zero) — the zero-fill signature.
        int32 ZeroWeightVertexCount = 0;
        // Vertices that have influence but do not sum to ~1.0 (un-normalized/degenerate).
        int32 DegenerateVertexCount = 0;
        // First N vertices' influences, for spot-checking (N == requested sample budget).
        TArray<FSkinWeightVertexSample> Samples;
    };

    // Summarizes one profile LOD's SkinWeights: per-vertex normalized/zero/degenerate
    // classification, max influence count, and the first SampleCount vertices' influences.
    // SampleCount <= 0 collects no samples. Pure — no engine state touched.
    inline FSkinWeightProfileSummary SummarizeSkinWeights(
        const TArray<FRawSkinWeight>& SkinWeights,
        int32 SampleCount)
    {
        FSkinWeightProfileSummary Summary;
        Summary.VertexCount = SkinWeights.Num();

        for (int32 VertIdx = 0; VertIdx < SkinWeights.Num(); ++VertIdx)
        {
            const FRawSkinWeight& Weight = SkinWeights[VertIdx];

            // Collect this vertex's influences for sampling in the same single pass that
            // classifies it, so the de-quantized weights are computed once.
            const bool bSampleVertex = VertIdx < SampleCount;
            FSkinWeightVertexSample Sample;
            float WeightSum = 0.0f;
            const int32 NumInfluences = ForEachNonZeroInfluence(Weight,
                [&](int32 BoneIndex, float DequantizedWeight)
                {
                    WeightSum += DequantizedWeight;
                    if (bSampleVertex)
                    {
                        Sample.BoneIndices.Add(BoneIndex);
                        Sample.Weights.Add(DequantizedWeight);
                    }
                });

            Summary.MaxInfluencesPerVertex = FMath::Max(Summary.MaxInfluencesPerVertex, NumInfluences);

            if (NumInfluences == 0)
            {
                ++Summary.ZeroWeightVertexCount;
            }
            else if (FMath::IsNearlyEqual(WeightSum, 1.0f, NormalizedWeightSumTolerance))
            {
                ++Summary.NormalizedVertexCount;
            }
            else
            {
                ++Summary.DegenerateVertexCount;
            }

            if (bSampleVertex)
            {
                Sample.VertexIndex = VertIdx;
                Sample.WeightSum = WeightSum;
                Summary.Samples.Add(MoveTemp(Sample));
            }
        }

        return Summary;
    }

    // Rebuilds the pre-chunking FVertInfluence list (SourceModelInfluences) from a
    // populated SkinWeights array. USkeletalMesh::Build() re-chunks the profile from
    // SourceModelInfluences, so a profile whose SkinWeights are set but whose
    // SourceModelInfluences are empty comes back empty after Build(). Influences are
    // stored packed largest-first and zero-padded, so the first zero weight ends the
    // per-vertex influence list. OutInfluences is reset and repopulated.
    //
    // THE INDEX-SPACE CROSSING LIVES HERE, and it is why LODModel is a parameter rather
    // than a convenience. SkinWeights holds SECTION-LOCAL slots; SourceModelInfluences must
    // hold REFERENCE-SKELETON indices (see the "Bone index spaces" note above). Every
    // influence is therefore mapped through its vertex's FSkelMeshSection::BoneMap on the way
    // out, mirroring what the engine does at MeshUtilities.cpp:5761-5774.
    //
    // LODModel is a REQUIRED leading parameter on purpose. The previous signature took only
    // the weight array, and a second call site (skeleton.copy_weights) had its own copy of the
    // persist logic - so a fix applied in the shared helper would have compiled cleanly while
    // silently missing that verb. Requiring the LOD model turns "missed a call site" from a
    // silent wrong answer into a compile error.
    //
    // Returns the number of influences DROPPED because their section-local slot had no BoneMap
    // entry. A non-zero return means the LOD's section data is inconsistent and the rebuilt
    // list is a lower bound; callers should surface it rather than treat the write as complete.
    inline int32 RebuildSourceModelInfluences(
        const FSkeletalMeshLODModel& LODModel,
        const TArray<FRawSkinWeight>& SkinWeights,
        TArray<SkeletalMeshImportData::FVertInfluence>& OutInfluences)
    {
        OutInfluences.Reset();
        OutInfluences.Reserve(SkinWeights.Num());  // >=1 influence per vertex in the common case

        int32 DroppedInfluences = 0;
        for (int32 VertIdx = 0; VertIdx < SkinWeights.Num(); ++VertIdx)
        {
            ForEachNonZeroInfluence(SkinWeights[VertIdx],
                [&](int32 SectionLocalBone, float DequantizedWeight)
                {
                    int32 RefSkeletonBone = INDEX_NONE;
                    if (!ResolveSectionLocalBone(LODModel, VertIdx, SectionLocalBone, RefSkeletonBone))
                    {
                        // Dropped, never defaulted to 0: bone 0 is the root on every skeleton,
                        // so a fallback would fabricate a root influence that reads as valid.
                        ++DroppedInfluences;
                        return;
                    }

                    SkeletalMeshImportData::FVertInfluence VertInfluence;
                    VertInfluence.VertIndex = static_cast<uint32>(VertIdx);
                    VertInfluence.BoneIndex = static_cast<FBoneIndexType>(RefSkeletonBone);
                    VertInfluence.Weight = DequantizedWeight;
                    OutInfluences.Add(VertInfluence);
                });
        }
        return DroppedInfluences;
    }

    // Persists a SkinWeights array into a named skin-weight profile so it survives
    // USkeletalMesh::Build(). This is the single "profile-write contract" the weight
    // mutators share: (1) ensure the mesh carries an FSkinWeightProfileInfo named
    // ProfileName (idempotent — AddSkinWeightProfile only when absent, so re-runs
    // don't duplicate the entry; USkeletalMesh::AddSkinWeightProfile is a bare
    // TArray::Add and does not dedupe), (2) FindOrAdd the LOD's
    // FImportedSkinWeightProfileData, (3) assign the SkinWeights, (4) rebuild
    // SourceModelInfluences (the pre-chunking list Build() re-chunks from — without it the
    // profile comes back empty), mapping section-local slots into reference-skeleton indices.
    // Kept here next to CaptureBaseSkinWeights / RebuildSourceModelInfluences so a future
    // change to how a profile is made to survive Build() lives in exactly one place.
    //
    // WHAT THIS DOES NOT DO: it does not touch the LOD's BASE skinning
    // (FSkelMeshSection::SoftVertices), which is what the renderer uses when no profile is
    // activated on a component. A named profile is the engine's "alternate influences" channel
    // (FLODUtilities::UpdateAlternateSkinWeights). Base skinning lives in the LOD's
    // FMeshDescription and is written by committing that; see the skeleton wiki page.
    //
    // Returns the number of influences dropped for want of a BoneMap entry (0 on a
    // consistent LOD) so a caller can report a partial write instead of a clean success.
    inline int32 WriteSkinWeightProfile(
        USkeletalMesh& Mesh,
        FSkeletalMeshLODModel& LODModel,
        FName ProfileName,
        const TArray<FRawSkinWeight>& SkinWeights)
    {
        const bool bHasProfile = Mesh.GetSkinWeightProfiles().ContainsByPredicate(
            [ProfileName](const FSkinWeightProfileInfo& Info) { return Info.Name == ProfileName; });
        if (!bHasProfile)
        {
            FSkinWeightProfileInfo NewProfile;
            NewProfile.Name = ProfileName;
            Mesh.AddSkinWeightProfile(NewProfile);
        }

        FImportedSkinWeightProfileData& ProfileData = LODModel.SkinWeightProfiles.FindOrAdd(ProfileName);
        ProfileData.SkinWeights = SkinWeights;
        return RebuildSourceModelInfluences(LODModel, ProfileData.SkinWeights, ProfileData.SourceModelInfluences);
    }

    // ---- Base-skinning readback ------------------------------------------------------------

    // One LOD's BASE skinning, read straight out of FSkelMeshSection::SoftVertices with every
    // influence resolved through that section's BoneMap.
    //
    // This is the READ shape and it is deliberately a different space from everything the
    // mutators write: SkinWeights[v].InfluenceBones here hold REFERENCE-SKELETON indices, so
    // they can be named with FReferenceSkeleton::GetBoneName and compared across sections.
    // Never feed this array back into WriteSkinWeightProfile - a profile's SkinWeights must be
    // section-local (MeshUtilities.cpp:5774) and writing reference-skeleton indices there would
    // corrupt the profile in the opposite direction.
    struct FBaseSkinningReadback
    {
        // One entry per LOD vertex, in GetVertices() order (sections concatenated,
        // SkeletalMeshLODModel.cpp:1036-1053). Bone slots are reference-skeleton indices.
        TArray<FRawSkinWeight> SkinWeights;
        // Influence slots that named a section-local bone with no BoneMap entry. Dropped
        // rather than resolved to bone 0; a non-zero count means every number derived from
        // this readback is a lower bound.
        int32 UnmappedInfluences = 0;
        int32 SectionCount = 0;
        int32 ClothSectionCount = 0;      // simulated, not skinned - findings inside may be the cloth bind
        int32 DisabledSectionCount = 0;
    };

    // Fills Out from the LOD's sections. Pure over the LOD model: no Build(), no world, no RHI,
    // so it is safe on a mesh that is not placed anywhere and cannot dirty the asset.
    inline void ReadBaseSkinningRefSkeletonSpace(
        const FSkeletalMeshLODModel& LODModel,
        FBaseSkinningReadback& Out)
    {
        Out.SkinWeights.Reset();
        Out.UnmappedInfluences = 0;
        Out.SectionCount = LODModel.Sections.Num();
        Out.ClothSectionCount = 0;
        Out.DisabledSectionCount = 0;

        int32 TotalVertices = 0;
        for (const FSkelMeshSection& Section : LODModel.Sections)
        {
            TotalVertices += Section.SoftVertices.Num();
        }
        Out.SkinWeights.Reserve(TotalVertices);

        for (const FSkelMeshSection& Section : LODModel.Sections)
        {
            if (Section.HasClothingData())
            {
                ++Out.ClothSectionCount;
            }
            if (Section.bDisabled)
            {
                ++Out.DisabledSectionCount;
            }

            for (const FSoftSkinVertex& SoftVertex : Section.SoftVertices)
            {
                FRawSkinWeight& Dest = Out.SkinWeights.AddDefaulted_GetRef();
                FMemory::Memzero(&Dest, sizeof(FRawSkinWeight));

                // Repacked, not copied in place: dropping an unmappable slot has to close the
                // gap or the zero it leaves would terminate the packed list early and hide
                // every influence after it.
                int32 DestSlot = 0;
                for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
                {
                    const uint16 RawWeight = SoftVertex.InfluenceWeights[Influence];
                    if (RawWeight == 0)
                    {
                        break;  // packed largest-first: trailing slots are all zero
                    }
                    const int32 LocalBone = static_cast<int32>(SoftVertex.InfluenceBones[Influence]);
                    if (!Section.BoneMap.IsValidIndex(LocalBone))
                    {
                        ++Out.UnmappedInfluences;
                        continue;
                    }
                    Dest.InfluenceBones[DestSlot] = Section.BoneMap[LocalBone];
                    Dest.InfluenceWeights[DestSlot] = RawWeight;
                    ++DestSlot;
                }
            }
        }
    }
}
