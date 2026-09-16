// Copyright (c) 2026 Alexander Penkin. MIT License.

// SkinAuditAnalysis.h
//
// Pure analysis math behind skeleton.audit_skin_weights. No UObject, no world, no RHI, no
// posing, so every rule below is unit-testable headlessly; the handler supplies the numbers.
//
// WHAT THIS AUDIT ACTUALLY TESTS, AND WHY IT IS NOT skeleton.describe_skin_weights
//
// describe_skin_weights reads NAMED SKIN-WEIGHT PROFILES (Mesh->GetSkinWeightProfiles() ->
// FSkeletalMeshLODModel::SkinWeightProfiles). A mesh that has never had a profile authored on
// it — which is every mesh that came out of an FBX import or GeometryScript
// create_new_skeletal_mesh_asset_from_mesh — has NO profiles, so that verb returns
// profileCount: 0 with an empty array and a success status. That is a clean-looking response
// that measured nothing whatsoever about the skinning the mesh actually renders with. This
// audit reads the BASE skinning instead: FSkelMeshSection::SoftVertices, which is what the
// renderer uses when no profile is active.
//
// THE EXACT / STATISTICAL SPLIT — the reason some checks ship a default threshold and some
// deliberately do not
//
// Three of the four checks below have a threshold that is DERIVED, not chosen:
//
//   * zero-influence vertices. A vertex with no influence is transformed by nothing and
//     renders at the component origin. The correct count is 0. There is no tuning here.
//   * weight sums. Influences are stored as uint16 fixed point, so a correctly normalized
//     vertex sums to 1.0 within a few quantization steps of 1/65535 (~1.5e-5) times up to
//     MAX_TOTAL_INFLUENCES slots, i.e. under 2e-4. The shared 1e-3 tolerance is five times
//     that band; anything outside it is an authoring error, not rounding.
//   * coincident-vertex split. Two vertices at the SAME bind position with DIFFERENT
//     influences move apart under any pose that moves the bones they disagree on. The
//     separation is bounded exactly (see FCoincidentGroup::SplitCoefficient), so the
//     threshold is a statement about how many world units of seam split are acceptable, not
//     a guess about what a defect looks like.
//
// The fourth — influence reach — has NO derivable threshold, and this header does not invent
// one. "A foot bone weighting a hand vertex" is real and measurable, but the boundary between
// a long-reaching legitimate influence and a wrong one depends on the skeleton and the
// silhouette. So the reach pass reports a DISTRIBUTION (percentiles plus the worst offenders,
// named) and applies a verdict only when the caller supplies a threshold. Calibration recipe
// for a caller who wants one: run this on a mesh known to be correctly skinned to the same
// skeleton, take the p99.9 of normalizedDistance, and set the threshold at roughly twice it.
// Shipping a guessed default here would manufacture exactly the false confidence the rest of
// this file exists to prevent.
//
// WHAT IT CANNOT SEE, STATED PLAINLY
//
//  - Whether the skinning looks good. Smoothness, volume preservation and joint falloff are
//    all perfectly legal configurations of numbers that pass every check here.
//  - Anything about a named profile. This reads base skinning only; use
//    skeleton.describe_skin_weights for profiles, and note that the two can disagree.
//  - Anything that only manifests when posed (candy-wrapper twist collapse, self-
//    intersection). Those need a pose; see the design note for why they are a separate gate.
//  - Cloth sections, which are simulated rather than skinned and are reported separately
//    rather than folded into the vertex counts.

#pragma once

#include "CoreMinimal.h"
#include "GPUSkinPublicDefs.h"   // MAX_TOTAL_INFLUENCES

namespace PinWrightSkinAudit
{
    // Weight-sum band for "this vertex is normalized". Shared value with
    // SkinWeightTransferUtils::NormalizedWeightSumTolerance and derived the same way: one
    // uint16 quantization step is 1/65535 ~= 1.5e-5 and a vertex packs up to
    // MAX_TOTAL_INFLUENCES slots, so a correctly normalized vertex lands inside ~2e-4. 1e-3
    // is five times that. Not duplicated by include because this header must stay free of
    // engine skinning types so the analysis is unit-testable on bare arrays.
    constexpr double DefaultWeightSumTolerance = 1.0e-3;

    // Bind-pose distance under which two vertices count as occupying the same point, in world
    // centimetres at the mesh's authored scale. Vertices duplicated at a UV or material seam
    // are written at BIT-IDENTICAL positions by every UE import path, so the honest value here
    // is 0. It is not 0 because a mesh that went through a scale/units conversion or a
    // GeometryScript round trip can carry float drift in the last bits, and a check that
    // silently stops finding seam pairs is worse than one that occasionally merges two
    // genuinely distinct vertices 0.01 uu apart — the latter is reported and inspectable, the
    // former reports a clean pass on a splitting mesh.
    constexpr double DefaultCoincidentTolerance = 0.01;

    // Per-bone weight disagreement between two coincident vertices that counts as a real
    // split rather than quantization. See FCoincidentGroup::SplitCoefficient for the bound
    // this implies: at 1e-3, two vertices sharing a bone that travels 1000 uu can separate by
    // at most 1 uu, which is under a pixel at any review framing.
    constexpr double DefaultSplitCoefficientTolerance = 1.0e-3;

    // Influences below this weight are ignored by the reach pass. A 1% influence contributes
    // at most 1% of a bone's displacement to the vertex, so on a bone travelling 500 uu it
    // moves the vertex 5 uu — below the noise floor of "is this vertex attached to the wrong
    // limb". Including them would swamp the worst-offender list with harmless falloff tails.
    constexpr double DefaultMinReachWeight = 0.01;

    // Vertex budget for the reach pass, which is O(vertices x bones) and therefore the only
    // part of this audit that is not linear. Above this the pass strides the vertex array
    // uniformly and reports how many it actually looked at, so a caller can never mistake a
    // sampled result for an exhaustive one. The three exact checks always run on every vertex.
    constexpr int32 DefaultReachVertexBudget = 50000;

    // One vertex's resolved skinning. Bone indices are REFERENCE-SKELETON indices, already
    // resolved through FSkelMeshSection::BoneMap by the caller — the raw InfluenceBones on a
    // soft vertex are SECTION-LOCAL and comparing those across sections silently compares two
    // different bones that happen to share a slot number.
    struct FAuditVertex
    {
        FVector3f Position = FVector3f::ZeroVector;   // bind pose, component space
        int32 SectionIndex = 0;
        int32 InfluenceCount = 0;                      // non-zero influences only
        int32 BoneIndices[MAX_TOTAL_INFLUENCES] = {};  // reference-skeleton indices
        double Weights[MAX_TOTAL_INFLUENCES] = {};     // de-quantized [0,1], largest first
    };

    // A bone's occupied space in the bind pose: the segment from its own joint to a child's
    // joint. A bone with several children contributes one segment each and a leaf bone
    // contributes a degenerate segment (Start == End), which distance-to-segment handles as a
    // point. Distance to the SEGMENT rather than to the joint matters: a vertex at the knee is
    // most of a thigh's length away from the hip joint but sits directly on the thigh bone, and
    // a joint-distance metric would flag every correctly skinned limb midpoint.
    struct FBoneSegment
    {
        int32 BoneIndex = INDEX_NONE;
        FVector3f Start = FVector3f::ZeroVector;
        FVector3f End = FVector3f::ZeroVector;
    };

    // Shortest distance from P to the segment, treating Start == End as a point.
    double DistanceToSegment(const FVector3f& P, const FBoneSegment& Segment);

    // Median length of the non-degenerate segments, used as the scale normalizer for reach.
    // Median rather than mean because a skeleton's segment lengths are strongly skewed by
    // facial and finger bones, which are numerous and tiny, and by a root-to-pelvis segment,
    // which is single and large. Returns 0 when no segment has length, and callers must then
    // report reach as unmeasurable rather than dividing by a fallback.
    double MedianSegmentLength(TConstArrayView<FBoneSegment> Segments);

    // ---- Check 1: weight sums and zero-influence vertices ----

    struct FWeightSumOffender
    {
        int32 VertexIndex = INDEX_NONE;
        double WeightSum = 0.0;
        int32 InfluenceCount = 0;
    };

    struct FWeightSumStats
    {
        int32 VertexCount = 0;
        int32 NormalizedCount = 0;
        // No influence at all. These render at the component origin, which is why they are
        // counted apart from "sums to the wrong number" rather than lumped in with it.
        int32 ZeroInfluenceCount = 0;
        int32 UnnormalizedCount = 0;
        double MinWeightSum = 0.0;      // over vertices with at least one influence
        double MaxWeightSum = 0.0;
        // Worst |sum - 1|, largest first. Zero-influence vertices are deliberately NOT listed
        // here: they have no sum to be wrong about and they are reported by their own check,
        // so mixing them in would let a single zero-fill crowd every genuinely un-normalized
        // vertex out of the list.
        TArray<FWeightSumOffender> Offenders;
        TArray<int32> ZeroInfluenceVertices;   // first MaxOffenders of them
    };

    void AccumulateWeightSums(TConstArrayView<FAuditVertex> Vertices, double Tolerance,
        int32 MaxOffenders, FWeightSumStats& OutStats);

    // ---- Check 2: influence counts ----

    struct FInfluenceCountStats
    {
        int32 VertexCount = 0;
        int32 MaxInfluences = 0;
        // Histogram[i] = vertices with exactly i non-zero influences, i in [0, MAX_TOTAL_INFLUENCES].
        int32 Histogram[MAX_TOTAL_INFLUENCES + 1] = {};
        // Vertices exceeding the effective limit. Zero when no limit applies.
        int32 OverLimitCount = 0;
        TArray<int32> OverLimitVertices;   // first MaxOffenders of them
    };

    void AccumulateInfluenceCounts(TConstArrayView<FAuditVertex> Vertices, int32 InfluenceLimit,
        int32 MaxOffenders, FInfluenceCountStats& OutStats);

    // ---- Check 3: coincident-vertex split ----

    // A set of vertices sharing one bind position whose influences do not agree.
    struct FCoincidentGroup
    {
        FVector3f Position = FVector3f::ZeroVector;
        TArray<int32> VertexIndices;
        // Sum over bones of the largest weight disagreement between any two members of the
        // group. This is an exact upper bound on how far the group's members can separate,
        // scaled by bone displacement: skinning is linear in the weights, so two vertices at
        // the same bind point with weight vectors w and w' land at most
        // sum_b |w_b - w'_b| * |displacement of bone b| apart. A group with coefficient 0
        // provably never splits under any pose; a group with coefficient 0.5 splits by up to
        // half the travel of the bones it disagrees on.
        double SplitCoefficient = 0.0;
        // Reference-skeleton indices of the bones the members disagree on, worst first.
        TArray<int32> DisagreeingBones;
    };

    struct FCoincidentStats
    {
        // Vertices that share a bind position with at least one other vertex. Zero here means
        // the mesh has no duplicated vertices at all, which makes the check vacuous rather
        // than passed — a caller must be able to tell those apart.
        int32 CoincidentVertexCount = 0;
        int32 GroupCount = 0;              // coincident groups found
        int32 SplittingGroupCount = 0;     // of those, groups whose influences disagree
        double MaxSplitCoefficient = 0.0;
        TArray<FCoincidentGroup> Groups;   // worst first, capped at MaxGroups
    };

    // Groups vertices by quantized bind position (a uniform grid of cell size
    // PositionTolerance, checking the 27 neighbouring cells so a pair straddling a cell
    // boundary is not missed) and compares each group's influence vectors. Runs over the
    // whole LOD rather than per section on purpose: a material seam IS a section boundary, so
    // the duplicated vertices that matter most live in DIFFERENT sections and a per-section
    // pass (or FSkelMeshSection::OverlappingVertices, which is section-local) cannot see them.
    void FindCoincidentSplits(TConstArrayView<FAuditVertex> Vertices, double PositionTolerance,
        double SplitTolerance, int32 MaxGroups, FCoincidentStats& OutStats);

    // ---- Check 4: bone coverage and rigid binding ----
    //
    // WHY A BONE WITH NO INFLUENCED VERTEX IS ITS OWN MEASUREMENT
    //
    // A hand-authored animation that drives a bone no vertex is weighted to moves NOTHING, and
    // every other check here passes on that mesh: the weights sum to 1, no vertex is
    // zero-influenced, no seam splits. The rig looks correct in every viewport. The only
    // evidence used to be a coincidence in an influence-count histogram, which cannot name a
    // bone. So coverage is measured directly and the uninfluenced bones are named.
    //
    // It is REPORTED, not gated, by default, and that is not timidity. Real skeletons carry
    // bones no vertex is ever weighted to on purpose: IK targets, attachment/socket bones,
    // twist drivers, a root that only carries motion. Failing on their existence would
    // manufacture exactly the false alarm the rigid-bind bucket below exists to remove. The
    // caller who knows their rig has no such bones turns it into a gate.
    //
    // WHY RIGID VERTICES ARE COUNTED PER BONE RATHER THAN SUMMED
    //
    // "This part is deliberately rigid" is a claim about ONE bone owning a contiguous set of
    // vertices outright. A single total ("412 rigid vertices") cannot separate that from 412
    // vertices scattered one-each across 40 bones, which is a broken bind wearing the same
    // number. The per-bone breakdown separates them; the total cannot.

    // A vertex counts as INFLUENCED by a bone when that influence's de-quantized weight is at
    // or above the weight epsilon, and RIGIDLY bound to it when that is its only such
    // influence. The epsilon is the caller's minInfluenceWeight (DefaultMinReachWeight), the
    // same one the reach pass uses to decide an influence is meaningful - deliberately not a
    // second constant, because two epsilons for "does this influence matter" drift apart and
    // the answer then depends on which check you asked.
    struct FBoneCoverage
    {
        double WeightEpsilon = DefaultMinReachWeight;
        int32 BoneCount = 0;
        int32 VertexCount = 0;
        // Vertices with exactly one influence at or above the epsilon. Summed over bones this
        // equals the sum of RigidVertexCountPerBone.
        int32 RigidVertexCount = 0;
        int32 InfluencedBoneCount = 0;
        // Indexed by reference-skeleton bone index, both sized BoneCount.
        TArray<int32> InfluencedVertexCountPerBone;
        TArray<int32> RigidVertexCountPerBone;
        // Reference-skeleton indices of every bone with zero influenced vertices, ascending.
        // Deliberately UNCAPPED: an answer to "which bones move nothing" that is clipped to
        // the worst N is not an answer, because the bone the caller needs is as likely to be
        // fortieth as first. Bounded by the skeleton's bone count, which is asset-bounded.
        TArray<int32> BonesWithNoInfluence;
    };

    // Incremental so the audit (which has already flattened the LOD into FAuditVertex) and the
    // profile readback (which holds engine FRawSkinWeight rows) feed ONE implementation rather
    // than two that agree today. Call Begin once, Add per vertex, End once.
    void BeginBoneCoverage(int32 BoneCount, double WeightEpsilon, FBoneCoverage& OutCoverage);
    void AddBoneCoverageVertex(FBoneCoverage& InOutCoverage, TConstArrayView<int32> BoneIndices,
        TConstArrayView<double> Weights);
    void EndBoneCoverage(FBoneCoverage& InOutCoverage);

    // Begin + Add over every vertex + End, for the already-flattened case.
    void AccumulateBoneCoverage(TConstArrayView<FAuditVertex> Vertices, int32 BoneCount,
        double WeightEpsilon, FBoneCoverage& OutCoverage);

    // ---- Check 5: influence reach (statistical, no default verdict) ----

    struct FReachOffender
    {
        int32 VertexIndex = INDEX_NONE;
        int32 BoneIndex = INDEX_NONE;
        double Weight = 0.0;
        double Distance = 0.0;             // world centimetres to the bone's segment
        double NormalizedDistance = 0.0;   // Distance / median segment length
        int32 CloserBoneCount = 0;         // bones strictly nearer this vertex than BoneIndex
    };

    struct FReachStats
    {
        // False when the skeleton has no measurable segment length (a single-bone skeleton, or
        // every bone coincident). The caller must then report reach as unmeasured; every
        // number below is meaningless in that case.
        bool bMeasurable = false;
        int32 SampledVertexCount = 0;
        int32 TotalVertexCount = 0;
        // Every influence at or above MinWeight whose bone contributed a segment. Equals
        // GatedInfluences + RigidBindInfluences exactly - the two buckets partition it, so an
        // influence that fell out of both is arithmetically visible.
        int32 InfluencesConsidered = 0;
        // The influences the percentiles and the verdict below are computed over.
        int32 GatedInfluences = 0;
        // The NOT-APPLICABLE bucket (docs/rpc-design.md 18): influences on a vertex that is
        // rigidly bound to exactly one bone AND whose bone is the nearest bone to it. See
        // ComputeReach for why that pair, and only that pair, is not a reach defect.
        int32 RigidBindInfluences = 0;
        int32 RigidBindVertices = 0;
        double MedianSegmentLength = 0.0;
        // Percentiles of NormalizedDistance over the GATED influences, in the order
        // {p50, p90, p99, p999, max}.
        double NormalizedDistancePercentiles[5] = {};
        // The same percentiles over the rigid-bind bucket. Reported, never gated: excluding a
        // measurement silently is how a threshold gets raised until the alarm stops, which is
        // the failure this split exists to avoid.
        double RigidBindNormalizedDistancePercentiles[5] = {};
        // Worst influences by NormalizedDistance, largest first, per bucket.
        TArray<FReachOffender> Offenders;
        TArray<FReachOffender> RigidBindOffenders;
    };

    // For every sampled vertex and every influence at or above MinWeight, measures the
    // distance from the vertex's bind position to that bone's segment, normalized by the
    // skeleton's median segment length, plus how many bones lie strictly closer. Strides the
    // vertex array uniformly when it exceeds VertexBudget and records both counts so a sampled
    // run is never mistaken for an exhaustive one.
    //
    // THE RIGID-BIND SPLIT, AND WHY IT IS NOT A RAISED THRESHOLD
    //
    // A part bound deliberately and entirely to one bone - a prop, a weapon, an eyeball, a
    // cape on one attachment bone - has every one of its vertices at whatever distance the
    // part's silhouette puts them from that bone, and a leaf bone's segment is a POINT, which
    // makes the number larger still. Fed to a distance gate whose verdict reads the MAXIMUM,
    // that intended authoring fails the audit and fills the offender list, crowding out the
    // defect a caller was actually looking for.
    //
    // The discriminator is not distance. It is that a deliberately rigid part is bound to the
    // bone NEAREST it: a weapon in a hand names the hand, and no bone is closer. A vertex
    // rigidly bound to the WRONG bone - the classic hand-vertex-on-the-foot-bone - has bones
    // strictly closer to it, so it stays in the gated bucket however far away it is. Both
    // terms are required: rigidity alone would excuse the wrong-bone defect, and nearness
    // alone would excuse a blended influence that should have been caught.
    //
    // Excluded influences are not dropped. They are measured, percentiled and listed in their
    // own bucket, so a caller can see exactly what the gate declined to judge and disagree
    // with it.
    void ComputeReach(TConstArrayView<FAuditVertex> Vertices, TConstArrayView<FBoneSegment> Segments,
        double MinWeight, int32 VertexBudget, int32 MaxOffenders, FReachStats& OutStats);
}
