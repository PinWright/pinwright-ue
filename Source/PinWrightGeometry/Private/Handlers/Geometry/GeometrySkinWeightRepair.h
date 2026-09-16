// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometrySkinWeightRepair.h - carries a skinned mesh's bone weights through an op that adds,
// deletes and re-uses vertices.
//
// ---------------------------------------------------------------------------
// THE DEFECT THIS EXISTS FOR
// ---------------------------------------------------------------------------
//
// A boolean over a skinned FDynamicMesh3 hands back vertices weighted to bones on the far side
// of the character. Measured on stock SKM_Manny (161 bones), which audits perfectly clean
// (splittingGroups 0, influence reach max 7.63): one boolean_subtract with a central column box
// produced vertices at the FEET 0.934-weighted to `head` 162.6 cm away, with all 160 other bones
// nearer, and `spine_04` / `neck_*` / `pelvis` influences appearing on geometry that is nowhere
// near them. Nothing in the chain reports it - the boolean says changed:true, the skeletal write
// says fullyWeighted:true, verticesUnweighted:0 - and the asset renders as triangles stretched
// the length of the character the moment it is posed.
//
// It is not the hole filler (a controlled re-run with fillHoles:false changed 34 triangles and
// reproduced maxSplitCoefficient to 15 decimal places), and it is not the cut interpolation.
//
// THE MECHANISM, per engine source on UE 5.8:
//
//   1. FMeshBoolean deletes the triangles it does not keep, with bRemoveIsolatedVertices=true
//      (MeshBoolean.cpp:532). Those vertex IDs go onto FDynamicMesh3's refcount free list.
//   2. It then appends the OTHER operand's surviving geometry into the same mesh
//      (MeshBoolean.cpp:733, FDynamicMeshEditor::AppendMesh). Each new vertex comes from
//      VertexRefCounts.Allocate() (DynamicMesh3_Edits.cpp:11), which RE-USES the IDs freed in
//      step 1.
//   3. FDynamicMesh3::AppendVertex notifies the attribute set, and the skin-weight layer's hook
//      is TDynamicVertexSkinWeightsAttribute::OnNewVertex -> ResizeAttribStoreIfNeeded
//      (DynamicVertexSkinWeightsAttribute.h:447-464). It GROWS the store when the ID is past the
//      end and does nothing at all when the ID is a re-used slot. It never clears the slot.
//   4. AppendMesh only writes skin weights for profiles the APPENDED mesh carries
//      (DynamicMeshEditor.cpp:2073-2106). A cutting box carries none, so nothing is written.
//
// So every vertex of the tool wall - and every hole-fill vertex - silently inherits the bone
// weights of whichever deleted vertex last occupied its slot. A box cut down the middle of a
// character deletes head, neck, spine and pelvis vertices and hands their weights to the wall
// vertices at the feet. That is exactly the observed failure, and it also explains why the
// engine reports the mesh fully weighted: the stale weights are non-empty.
//
// The interpolating hooks are fine and are deliberately left in charge: OnSplitEdge lerps along
// the split edge and OnPokeTriangle blends barycentrically (:430, :467), so a genuine cut vertex
// on an original edge already gets the right answer from the engine.
//
// ---------------------------------------------------------------------------
// THE REPAIR
// ---------------------------------------------------------------------------
//
// Snapshot the mesh before the op; afterwards classify every result vertex and re-derive only
// the ones whose weights cannot be accounted for:
//
//   CARRIED THROUGH - there is a snapshot vertex at the same position (within a tolerance
//     derived from the bounds) carrying exactly these bone weights. Either it IS that vertex, or
//     it is a duplicate of it; either way the value is the pre-op mesh's own and is left ALONE.
//     This is what keeps an untouched region byte-identical, so a clean source mesh cannot be
//     degraded by running the repair over it.
//
//   TRANSFERRED - everything else: tool-wall vertices on re-used slots, hole-fill vertices, cut
//     vertices the engine interpolated (whose interpolated value is reproduced by the transfer,
//     since the closest point on the pre-op surface to a point ON a pre-op edge is that point).
//     Re-derived with the engine's FTransferBoneWeights in ClosestPointOnSurface mode against the
//     snapshot, which barycentrically blends the containing triangle's corner weights and
//     renormalizes (TransferBoneWeights.cpp:37-60). Nearest-surface is the right transfer for a
//     cut cap: the cap across a forearm should skin like the forearm skin that rings it.
//
// Provenance is decided from position + value rather than from a marker attribute precisely
// BECAUSE of the defect above: any per-vertex marker would ride the same OnNewVertex hook and
// inherit the same stale slot, so a marker cannot distinguish a re-used slot from a real one.
//
// FTransferBoneWeights::TransferWeightsToMesh is NOT used even though it takes a
// TargetVerticesSubset: on 5.8 that path sizes MatchedVertices by the SUBSET length and then
// indexes it by mesh vertex ID (TransferBoneWeights.cpp:257, :285), which is an out-of-bounds
// write for any subset that is not a dense prefix. The per-point entry point the same header
// documents as the alternative has no such bug and is what this uses.
#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

class UDynamicMesh;

// Forward-declared rather than included: FDynamicMesh3 is a heavy header and the only thing this
// one needs from it is a TUniquePtr member, whose destructor is defined out of line below. Spelt
// in the engine's own nested form to match DynamicMeshAttributeSet.h:38.
namespace UE { namespace Geometry { class FDynamicMesh3; } }

namespace GeometrySkinWeights
{
    // What a repair pass did. All zero, with bSkinned false, when the mesh carries no skin
    // weights at all - which is every .pwmodel boolean and every unskinned RPC call, so the
    // repair is invisible to them.
    struct FRepairReport
    {
        // The op ran on a mesh carrying a skin-weight profile, i.e. the repair had work to do.
        bool bSkinned = false;

        // Result vertices whose weights matched a pre-op vertex at the same position exactly and
        // were left untouched.
        int32 VerticesCarriedThrough = 0;

        // Result vertices re-derived from the pre-op surface. A caller that sees this at zero on
        // a boolean that changed the triangle count should be suspicious of the classification,
        // not reassured.
        int32 VerticesTransferred = 0;

        // Result vertices the transfer could not resolve (no closest point on the pre-op
        // surface, which needs an empty or degenerate snapshot). Left exactly as the engine left
        // them, and counted so the caller is not told they were fixed.
        int32 VerticesUnresolved = 0;
    };

    // A copy of a skinned mesh taken before a topology-changing op, used afterwards as the
    // weight source. Constructing it over an unskinned mesh is cheap and legal: IsValid() comes
    // back false and Repair() is a no-op that reports bSkinned false.
    class FPreEditSnapshot
    {
    public:
        explicit FPreEditSnapshot(UDynamicMesh* Mesh);
        ~FPreEditSnapshot();

        FPreEditSnapshot(const FPreEditSnapshot&) = delete;
        FPreEditSnapshot& operator=(const FPreEditSnapshot&) = delete;

        bool IsValid() const { return SourceMesh.IsValid(); }

        // Classify every vertex of Mesh against the snapshot and re-derive the ones that cannot
        // be accounted for. Safe to call on the same handle the snapshot was taken from - that
        // is the intended use, since the boolean verbs mutate their target in place.
        FRepairReport Repair(UDynamicMesh* Mesh) const;

    private:
        // Null when the source carried no skin weights, which is what IsValid() reports.
        TUniquePtr<UE::Geometry::FDynamicMesh3> SourceMesh;

        // The profile the snapshot was taken from and the profile Repair() writes back into.
        // Always FSkeletalMeshAttributes::DefaultSkinWeightProfileName in this module - see the
        // BASE WEIGHTS note in SkeletalMeshAssetIOHandler.cpp for why no verb here exposes a
        // profile parameter - but carried rather than re-derived so the two halves cannot drift.
        FName ProfileName;
    };
}
