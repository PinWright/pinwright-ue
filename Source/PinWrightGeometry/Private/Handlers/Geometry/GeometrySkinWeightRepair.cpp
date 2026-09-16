// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometrySkinWeightRepair.cpp - see GeometrySkinWeightRepair.h for the defect, the engine
// source that produces it, and why provenance is decided from position + value.
#include "Handlers/Geometry/GeometrySkinWeightRepair.h"

#include "UDynamicMesh.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicVertexSkinWeightsAttribute.h"
#include "Spatial/PointHashGrid3.h"
// FTransferBoneWeights lives in the GeometryProcessing plugin's DynamicMesh module, which
// PinWrightGeometry.Build.cs already lists as a private dependency for FDynamicMesh3 itself.
#include "Operations/TransferBoneWeights.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif
// FGeometryScriptBoneWeightProfile, whose default is FSkeletalMeshAttributes::
// DefaultSkinWeightProfileName - the same profile every verb in this module reads and writes.
#include "GeometryScript/MeshBoneWeightFunctions.h"

using UE::Geometry::FDynamicMesh3;
using UE::Geometry::FDynamicMeshVertexSkinWeightsAttribute;
using UE::AnimationCore::FBoneWeights;

namespace
{
    // Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace
    // helper with a common name would collide with a sibling TU once Unity merges them.

    // Positions do not survive a boolean bit-identically even where the geometry is untouched:
    // FMeshBoolean scales both operands into a unit cube, works there, and scales the result back
    // (MeshBoolean.cpp:229-241), and it SNAPS matched vertices onto each other (:694). So the
    // match test needs a tolerance, and the tolerance has to stay far below any real vertex
    // spacing or a genuinely new vertex could be mistaken for a surviving one.
    //
    // A relative epsilon rather than an absolute one, because this runs on meshes measured in
    // centimetres and on meshes measured in metres. 1e-6 of the bounding box is ~2 micrometres on
    // a 2 m character - orders of magnitude above the double-precision round trip and orders of
    // magnitude below anything an author would call two vertices.
    //
    // A vertex the boolean SNAPPED further than this is classified as new and re-derived. That is
    // the correct call, not a miss: it did move, and the transfer answers with the weights of the
    // pre-op surface at the position it moved to.
    double GeometrySkinWeightRepair_MatchTolerance(const UE::Geometry::FAxisAlignedBox3d& Bounds)
    {
        return FMath::Max(UE_DOUBLE_KINDA_SMALL_NUMBER, Bounds.MaxDim() * 1e-6);
    }

    // Sized for roughly one vertex per occupied cell. Vertices sit on a surface, so they spread
    // over the square of the linear cell count rather than the cube - hence sqrt(N) divisions
    // rather than cbrt(N). Only a performance choice; any positive cell size is correct.
    double GeometrySkinWeightRepair_CellSize(
        const UE::Geometry::FAxisAlignedBox3d& Bounds, int32 VertexCount, double MinCellSize)
    {
        const double Divisions = FMath::Max(1.0, FMath::Sqrt(static_cast<double>(FMath::Max(1, VertexCount))));
        return FMath::Max(Bounds.MaxDim() / Divisions, MinCellSize);
    }
}

namespace GeometrySkinWeights
{

FPreEditSnapshot::FPreEditSnapshot(UDynamicMesh* Mesh)
{
    if (!Mesh)
    {
        return;
    }

    ProfileName = FGeometryScriptBoneWeightProfile().GetProfileName();

    Mesh->ProcessMesh([this](const FDynamicMesh3& ReadMesh)
    {
        if (!ReadMesh.HasAttributes())
        {
            return;
        }
        if (ReadMesh.Attributes()->GetSkinWeightsAttribute(ProfileName) == nullptr)
        {
            return;
        }
        // The whole mesh, not just the weights: the transfer needs the pre-op SURFACE to project
        // cap and wall vertices onto, and the match test needs the pre-op positions.
        SourceMesh = MakeUnique<FDynamicMesh3>(ReadMesh);
    });
}

// Out of line so the header can forward-declare FDynamicMesh3 rather than include it.
FPreEditSnapshot::~FPreEditSnapshot() = default;

FRepairReport FPreEditSnapshot::Repair(UDynamicMesh* Mesh) const
{
    FRepairReport Report;
    if (!Mesh || !SourceMesh.IsValid())
    {
        return Report;
    }
    Report.bSkinned = true;

    const FDynamicMesh3& Source = *SourceMesh;
    const FDynamicMeshVertexSkinWeightsAttribute* SourceWeights =
        Source.Attributes()->GetSkinWeightsAttribute(ProfileName);
    if (!SourceWeights)
    {
        return Report;
    }

    const UE::Geometry::FAxisAlignedBox3d SourceBounds = Source.GetBounds(true);
    const double MatchTolerance = GeometrySkinWeightRepair_MatchTolerance(SourceBounds);

    UE::Geometry::TPointHashGrid3d<int32> SourcePoints(
        GeometrySkinWeightRepair_CellSize(SourceBounds, Source.VertexCount(), MatchTolerance),
        INDEX_NONE);
    SourcePoints.Reserve(Source.VertexCount());
    for (const int32 SourceVertexID : Source.VertexIndicesItr())
    {
        SourcePoints.InsertPointUnsafe(SourceVertexID, Source.GetVertex(SourceVertexID));
    }

    // Classified in a read pass and rewritten in an edit pass, rather than both at once, so the
    // classification cannot see a value the repair itself has just written.
    TArray<int32> NeedTransfer;
    bool bResultHasAttribute = false;

    Mesh->ProcessMesh([&](const FDynamicMesh3& ReadMesh)
    {
        const FDynamicMeshVertexSkinWeightsAttribute* ResultWeights =
            ReadMesh.HasAttributes() ? ReadMesh.Attributes()->GetSkinWeightsAttribute(ProfileName) : nullptr;
        bResultHasAttribute = (ResultWeights != nullptr);

        NeedTransfer.Reserve(ReadMesh.VertexCount());
        for (const int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            if (!ResultWeights)
            {
                // The op dropped the profile outright. Nothing on this mesh can be trusted, and
                // nothing can be compared either - every vertex is re-derived.
                NeedTransfer.Add(VertexID);
                continue;
            }

            FBoneWeights ResultValue;
            ResultWeights->GetValue(VertexID, ResultValue);

            const FVector3d Position = ReadMesh.GetVertex(VertexID);

            // Nearest pre-op vertex within tolerance THAT CARRIES THESE EXACT WEIGHTS. The
            // weight equality is the half that matters: a stale re-used slot sitting at a
            // position that happens to coincide with a pre-op vertex is rejected by it, and so is
            // a cut vertex the engine snapped onto an original vertex without giving it that
            // vertex's weights.
            const TPair<int32, double> Match = SourcePoints.FindNearestInRadius(
                Position, MatchTolerance,
                [&Source, &Position](const int32& SourceVertexID)
                {
                    return UE::Geometry::DistanceSquared(Position, Source.GetVertex(SourceVertexID));
                },
                [SourceWeights, &ResultValue](const int32& SourceVertexID)
                {
                    FBoneWeights SourceValue;
                    SourceWeights->GetValue(SourceVertexID, SourceValue);
                    return SourceValue != ResultValue;
                });

            if (Match.Key != INDEX_NONE)
            {
                ++Report.VerticesCarriedThrough;
            }
            else
            {
                NeedTransfer.Add(VertexID);
            }
        }
    });

    if (NeedTransfer.Num() == 0)
    {
        return Report;
    }

    // Built AFTER the classification, not before: the constructor builds an AABB tree over the
    // whole snapshot (TransferBoneWeights.cpp:136-140), which is by far the most expensive thing
    // in this function, and a boolean that touched nothing - a disjoint subtract, say - has
    // nothing to transfer and should not pay for it.
    UE::Geometry::FTransferBoneWeights Transfer(&Source, ProfileName);
    // Pure closest-point copy: the header's own instructions for "simply copy weights over from
    // the closest points" are SearchRadius and NormalThreshold both negative
    // (TransferBoneWeights.h:60-62). A radius would leave far vertices unresolved and a normal
    // threshold would reject the cap, whose normal points into the cut and matches nothing.
    Transfer.TransferMethod =
        UE::Geometry::FTransferBoneWeights::ETransferBoneWeightsMethod::ClosestPointOnSurface;
    Transfer.SearchRadius = -1.0;
    Transfer.NormalThreshold = -1.0;
    // No re-indexing, and none is needed: the result mesh is built from a copy of this very mesh
    // (MeshBoolean.cpp:223), and the one path that can extend the bone list -
    // FDynamicMeshAttributeSet::AppendBonesUnique via AppendMesh (DynamicMeshEditor.cpp:2070) -
    // APPENDS, so every index the snapshot uses still names the same bone in the result. Setting
    // the flag also lets the repair serve a mesh that carries weights but no bone attributes,
    // which FTransferBoneWeights::Validate would otherwise refuse (TransferBoneWeights.cpp:176).
    Transfer.bIgnoreBoneAttributes = true;
    if (Transfer.Validate() != UE::Geometry::EOperationValidationResult::Ok)
    {
        // Counted, not swallowed: these vertices keep whatever the engine left on them, and the
        // caller has to hear that rather than read a zero and assume the mesh is clean.
        Report.VerticesUnresolved += NeedTransfer.Num();
        return Report;
    }

    Mesh->EditMesh([&](FDynamicMesh3& EditMesh)
    {
        if (!EditMesh.HasAttributes())
        {
            EditMesh.EnableAttributes();
        }
        if (!bResultHasAttribute)
        {
            EditMesh.Attributes()->AttachSkinWeightsAttribute(
                ProfileName, new FDynamicMeshVertexSkinWeightsAttribute(&EditMesh));
        }

        FDynamicMeshVertexSkinWeightsAttribute* TargetWeights =
            EditMesh.Attributes()->GetSkinWeightsAttribute(ProfileName);
        if (!TargetWeights)
        {
            Report.VerticesUnresolved += NeedTransfer.Num();
            return;
        }

        for (const int32 VertexID : NeedTransfer)
        {
            if (!EditMesh.IsVertex(VertexID))
            {
                continue;
            }

            FBoneWeights Weights;
            // Barycentric blend of the containing pre-op triangle's corner weights, renormalized
            // and pruned to the engine's influence cap inside InterpolateBoneWeights
            // (TransferBoneWeights.cpp:51-59), so nothing here needs to normalize afterwards.
            if (Transfer.TransferWeightsToPoint(Weights, EditMesh.GetVertex(VertexID)))
            {
                TargetWeights->SetValue(VertexID, Weights);
                ++Report.VerticesTransferred;
            }
            else
            {
                ++Report.VerticesUnresolved;
            }
        }
    });

    return Report;
}

}
