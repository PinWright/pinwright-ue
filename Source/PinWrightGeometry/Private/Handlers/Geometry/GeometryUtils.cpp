// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryUtils.cpp - Shared helpers and macros for geometry handlers
#include "Handlers/Geometry/GeometryUtils.h"
#include "Handlers/Geometry/GeometryOpWarnings.h"
#include "Handlers/Geometry/GeometryOps_Modeling.h"
#include "Handlers/HandlerContext.h"
#include "PinWrightHelpers.h"
#include "UDynamicMesh.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicBoneAttribute.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
// ScanDominantBoneReach reads the per-vertex FBoneWeights straight off the attribute; the
// attribute set only forward-declares it.
#include "DynamicMesh/DynamicVertexSkinWeightsAttribute.h"
#include "VectorTypes.h"
// MeasureMeshSelfIntersection: per-component AABB trees, the edge-connected decomposition
// they are built over, and the coplanar-aware triangle-triangle test.
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Selections/MeshConnectedComponents.h"
#include "Intersection/IntrTriangle3Triangle3.h"
#include "MeshQueries.h"
#include "IndexTypes.h"
#include "HAL/PlatformMemory.h"
#include "Compat/EngineVersionCompat.h"
#include "Utils/PathUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/Level.h"
// CopySkeletonBonesToMesh reads the reference skeleton directly on engines with no
// CopyBonesFromSkeleton.
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif
#include "GeometryScript/MeshBoneWeightFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"
#include "GeometryScript/MeshQueryFunctions.h"
#include "GeometryScript/GeometryScriptSelectionTypes.h"

DEFINE_LOG_CATEGORY(LogMcpGeometryHandlersNew);

namespace GeometryUtils
{

FVector ReadVectorFromPayload(const TSharedPtr<FJsonObject>& Payload, const TCHAR* FieldName, FVector Default)
{
    if (!Payload.IsValid())
        return Default;

    // Try array format first [x, y, z]
    const TArray<TSharedPtr<FJsonValue>>* ArrayPtr;
    if (Payload->TryGetArrayField(FieldName, ArrayPtr) && ArrayPtr->Num() >= 3)
    {
        return FVector(
            (*ArrayPtr)[0]->AsNumber(),
            (*ArrayPtr)[1]->AsNumber(),
            (*ArrayPtr)[2]->AsNumber()
        );
    }

    // Try object format {x, y, z}
    const TSharedPtr<FJsonObject>* ObjPtr;
    if (Payload->TryGetObjectField(FieldName, ObjPtr))
    {
        return FVector(
            GetNumberFieldGeomNew((*ObjPtr), TEXT("x")),
            GetNumberFieldGeomNew((*ObjPtr), TEXT("y")),
            GetNumberFieldGeomNew((*ObjPtr), TEXT("z"))
        );
    }

    return Default;
}

FRotator ReadRotatorFromPayload(const TSharedPtr<FJsonObject>& Payload, const TCHAR* FieldName, FRotator Default)
{
    if (!Payload.IsValid())
        return Default;

    // Try array format first [pitch, yaw, roll]
    const TArray<TSharedPtr<FJsonValue>>* ArrayPtr;
    if (Payload->TryGetArrayField(FieldName, ArrayPtr) && ArrayPtr->Num() >= 3)
    {
        return FRotator(
            (*ArrayPtr)[0]->AsNumber(),
            (*ArrayPtr)[1]->AsNumber(),
            (*ArrayPtr)[2]->AsNumber()
        );
    }

    // Try object format {pitch, yaw, roll} or {x, y, z}
    const TSharedPtr<FJsonObject>* ObjPtr;
    if (Payload->TryGetObjectField(FieldName, ObjPtr))
    {
        // Check for {pitch, yaw, roll} format first
        if ((*ObjPtr)->HasField(TEXT("pitch")) || (*ObjPtr)->HasField(TEXT("yaw")) || (*ObjPtr)->HasField(TEXT("roll")))
        {
            return FRotator(
                GetNumberFieldGeomNew((*ObjPtr), TEXT("pitch"), 0.0),
                GetNumberFieldGeomNew((*ObjPtr), TEXT("yaw"), 0.0),
                GetNumberFieldGeomNew((*ObjPtr), TEXT("roll"), 0.0)
            );
        }
        // Fallback to {x, y, z} format (x=Pitch, y=Yaw, z=Roll)
        return FRotator(
            GetNumberFieldGeomNew((*ObjPtr), TEXT("x")),
            GetNumberFieldGeomNew((*ObjPtr), TEXT("y")),
            GetNumberFieldGeomNew((*ObjPtr), TEXT("z"))
        );
    }

    return Default;
}

FTransform ReadTransformFromPayload(const TSharedPtr<FJsonObject>& Payload)
{
    FVector Location = ReadVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    FRotator Rotation = ReadRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    FVector Scale = ReadVectorFromPayload(Payload, TEXT("scale"), FVector::OneVector);

    return FTransform(
        Rotation,
        Location,
        Scale
    );
}

UDynamicMesh* GetOrCreateDynamicMesh(UObject* Outer)
{
    return NewObject<UDynamicMesh>(Outer);
}

bool IsMemoryPressureSafe()
{
#if PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX
    FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
    double UsagePercent = static_cast<double>(MemStats.UsedPhysical) /
                          static_cast<double>(MemStats.TotalPhysical);
    return UsagePercent < GEOM_MEMORY_PRESSURE_CRITICAL;
#else
    return true;
#endif
}

double GetMemoryUsagePercent()
{
#if PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX
    FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
    return static_cast<double>(MemStats.UsedPhysical) /
           static_cast<double>(MemStats.TotalPhysical) * 100.0;
#else
    return 0.0;
#endif
}

void CopySkeletonBonesToMesh(USkeleton* Skeleton, UDynamicMesh* Mesh)
{
    if (!Skeleton || !Mesh)
    {
        return;
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    FGeometryScriptCopyBonesFromMeshOptions BoneOptions;
    UGeometryScriptLibrary_MeshBoneWeightFunctions::CopyBonesFromSkeleton(
        Skeleton, Mesh, BoneOptions, nullptr);
#else
    // See the header: no CopyBonesFromSkeleton on this engine. Under the 5.5 defaults that
    // function copies the RAW reference skeleton in index order - names, parent indices and the
    // reference pose - onto a freshly enabled bone attribute set, which is exactly this.
    const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
    const int32 NumBones = RefSkeleton.GetRawBoneNum();
    if (NumBones <= 0)
    {
        return;
    }

    const TArray<FMeshBoneInfo>& BoneInfo = RefSkeleton.GetRawRefBoneInfo();
    const TArray<FTransform>& BonePose = RefSkeleton.GetRawRefBonePose();

    Mesh->EditMesh([&](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        if (!EditMesh.HasAttributes())
        {
            EditMesh.EnableAttributes();
        }
        EditMesh.Attributes()->EnableBones(NumBones);

        UE::Geometry::FDynamicMeshBoneNameAttribute* Names = EditMesh.Attributes()->GetBoneNames();
        UE::Geometry::FDynamicMeshBoneParentIndexAttribute* Parents =
            EditMesh.Attributes()->GetBoneParentIndices();
        UE::Geometry::FDynamicMeshBonePoseAttribute* Poses = EditMesh.Attributes()->GetBonePoses();
        if (!Names || !Parents || !Poses)
        {
            return;
        }

        for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
        {
            Names->SetValue(BoneIndex, BoneInfo[BoneIndex].Name);
            Parents->SetValue(BoneIndex, BoneInfo[BoneIndex].ParentIndex);
            Poses->SetValue(BoneIndex, BonePose[BoneIndex]);
        }
    }, EDynamicMeshChangeType::AttributeEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
#endif
}

int32 ClampSegments(int32 Value, int32 Default)
{
    return FMath::Clamp(Value <= 0 ? Default : Value, 1, GEOM_MAX_SEGMENTS);
}

double ClampDimension(double Value, double Default)
{
    if (Value <= 0.0) Value = Default;
    return FMath::Clamp(Value, GEOM_MIN_DIMENSION, GEOM_MAX_DIMENSION);
}

void ApplyXAtlasUnwrap(
    const FHandlerContext& Ctx,
    UDynamicMesh* Mesh,
    UDynamicMeshComponent* Component,
    const FString& ActorName,
    int32 UVChannel,
    const FString& SuccessMsg,
    const TFunctionRef<void(const TSharedPtr<FJsonObject>&)>& ExtraFields)
{
    // The op itself is GeometryOps::UnwrapUVXAtlas; this function is now only the
    // resolve-nothing / notify / respond shell around it, so the three verbs that call it and
    // the .pwmodel compiler run the same code. The error code and message it forwards are the
    // ones this function used to build itself (INVALID_ARGUMENT plus the channel-range text),
    // which is what keeps its callers' responses byte-identical.
    const GeometryOps::FOpResult Op = GeometryOps::UnwrapUVXAtlas(Mesh, UVChannel);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return;
    }

    MarkGeometryActorModified(Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    AddResolvedActorIdentity(Result, Component ? Component->GetOwner() : nullptr);
    Result->SetNumberField(TEXT("uvChannel"), UVChannel);
    ExtraFields(Result);
    // One call covers unwrap_uv, the remaining verb that reaches the op only through this shell.
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(SuccessMsg, Result);
}

void AddResolvedActorIdentity(
    const TSharedPtr<FJsonObject>& Result,
    AActor* Actor,
    const TCHAR* Prefix)
{
    if (!Result.IsValid() || !Actor)
    {
        return;
    }

    const FString FieldPrefix = Prefix && *Prefix
        ? FString(Prefix) + TEXT("Actor")
        : FString(TEXT("actor"));
    Result->SetStringField(FieldPrefix + TEXT("Path"), Actor->GetPathName());
    Result->SetStringField(FieldPrefix + TEXT("ObjectName"), Actor->GetName());
}

FString MakeDefaultGeometryAssetPath(AActor* Actor)
{
    const FString DisplayLabel = Actor ? Actor->GetActorLabel() : FString();
    const FString SourceName = !DisplayLabel.IsEmpty()
        ? DisplayLabel
        : (Actor ? Actor->GetFName().ToString() : FString());
    return FString::Printf(TEXT("/Game/GeneratedMeshes/%s"), *SanitizeAssetName(SourceName));
}

void ApplyXAtlasUnwrap(
    const FHandlerContext& Ctx,
    UDynamicMesh* Mesh,
    UDynamicMeshComponent* Component,
    const FString& ActorName,
    int32 UVChannel,
    const FString& SuccessMsg)
{
    ApplyXAtlasUnwrap(Ctx, Mesh, Component, ActorName, UVChannel, SuccessMsg,
        [](const TSharedPtr<FJsonObject>&) {});
}

int32 GetMeshVertexCount(UDynamicMesh* Mesh)
{
    return UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(Mesh);
}

FSkinWeightCoverage ScanSkinWeightCoverage(UDynamicMesh* Mesh)
{
    FSkinWeightCoverage Coverage;
    if (!Mesh)
    {
        return Coverage;
    }

    UGeometryScriptLibrary_MeshBoneWeightFunctions::MeshHasBoneWeights(Mesh, Coverage.bHasProfile);
    if (!Coverage.bHasProfile)
    {
        const UE::Geometry::FDynamicMesh3& ReadMesh = Mesh->GetMeshRef();
        Coverage.VertexCount = ReadMesh.VertexCount();
        Coverage.UnweightedCount = Coverage.VertexCount;
        return Coverage;
    }

    // Snapshot IDs before probing weights. GetVertexBoneWeights re-enters the mesh through
    // ProcessMesh, and FDynamicMesh3 vertex IDs are not dense after vertex deletion.
    TArray<int32> VertexIDs;
    {
        const UE::Geometry::FDynamicMesh3& ReadMesh = Mesh->GetMeshRef();
        VertexIDs.Reserve(ReadMesh.VertexCount());
        for (int32 VertexID = 0; VertexID < ReadMesh.MaxVertexID(); ++VertexID)
        {
            if (ReadMesh.IsVertex(VertexID))
            {
                VertexIDs.Add(VertexID);
            }
        }
    }
    Coverage.VertexCount = VertexIDs.Num();

    // Read only bHasValidBoneWeights. The current engine implementation grows the output array
    // from an initial SetNum and then appends the same entries, so its contents cannot support an
    // influence histogram even though the boolean is the correct per-vertex predicate.
    TArray<FGeometryScriptBoneWeight> ScratchWeights;
    for (const int32 VertexID : VertexIDs)
    {
        ScratchWeights.Reset();
        bool bVertexHasWeights = false;
        UGeometryScriptLibrary_MeshBoneWeightFunctions::GetVertexBoneWeights(
            Mesh, VertexID, ScratchWeights, bVertexHasWeights);
        if (bVertexHasWeights)
        {
            ++Coverage.WeightedCount;
        }
    }
    Coverage.UnweightedCount = Coverage.VertexCount - Coverage.WeightedCount;

    ScanDominantBoneReach(Mesh, Coverage);
    return Coverage;
}

void ScanDominantBoneReach(UDynamicMesh* Mesh, FSkinWeightCoverage& Coverage)
{
    if (!Mesh || !Coverage.bHasProfile)
    {
        return;
    }

    // Reference-pose bone positions in the mesh's own space, off the mesh's own bone attributes.
    // GetAllBonesInfo composes each bone's LocalTransform up the parent chain into WorldTransform
    // (MeshBoneWeightFunctions.cpp:1346-1354) and returns an empty array for a mesh with no bone
    // attributes, which is the no-op that leaves bBoneReachChecked false.
    TArray<FGeometryScriptBoneInfo> BonesInfo;
    UGeometryScriptLibrary_MeshBoneWeightFunctions::GetAllBonesInfo(Mesh, BonesInfo);

    // Two bones minimum: "the farthest bone of them all" says nothing on a single-bone mesh,
    // where the only influence is trivially both nearest and farthest.
    if (BonesInfo.Num() < 2)
    {
        return;
    }

    TArray<FVector3d> BonePositions;
    BonePositions.Reserve(BonesInfo.Num());
    for (const FGeometryScriptBoneInfo& Bone : BonesInfo)
    {
        BonePositions.Add(Bone.WorldTransform.GetLocation());
    }

    const FName ProfileName = FGeometryScriptBoneWeightProfile().GetProfileName();

    Mesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        // Read straight off the attribute rather than through GetVertexBoneWeights: that
        // accessor's output array cannot be trusted for per-influence data (see the note in
        // ScanSkinWeightCoverage above), and the dominant influence is exactly per-influence data.
        const UE::Geometry::FDynamicMeshVertexSkinWeightsAttribute* Weights =
            ReadMesh.HasAttributes() ? ReadMesh.Attributes()->GetSkinWeightsAttribute(ProfileName) : nullptr;
        if (!Weights)
        {
            return;
        }
        Coverage.bBoneReachChecked = true;

        for (const int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            UE::AnimationCore::FBoneWeights VertexWeights;
            Weights->GetValue(VertexID, VertexWeights);
            if (VertexWeights.Num() == 0)
            {
                continue;
            }

            // Scanned rather than assuming entry 0 is the largest: FBoneWeights sorts on
            // construction, but nothing stops a caller writing an unsorted container.
            int32 DominantBone = INDEX_NONE;
            uint16 DominantRawWeight = 0;
            for (const UE::AnimationCore::FBoneWeight& Influence : VertexWeights)
            {
                if (DominantBone == INDEX_NONE || Influence.GetRawWeight() > DominantRawWeight)
                {
                    DominantBone = static_cast<int32>(Influence.GetBoneIndex());
                    DominantRawWeight = Influence.GetRawWeight();
                }
            }
            // An index past the mesh's own bone list is a different defect (a weight indexed
            // against another skeleton) and is not something this check can judge.
            if (!BonePositions.IsValidIndex(DominantBone))
            {
                continue;
            }

            const FVector3d Position = ReadMesh.GetVertex(VertexID);
            const double DominantDistance =
                UE::Geometry::Distance(Position, BonePositions[DominantBone]);
            if (DominantDistance <= GEOM_MIN_FLAGGED_BONE_DISTANCE)
            {
                continue;
            }

            // Early-outs on the first bone that is no nearer than the dominant one, which for a
            // correctly weighted vertex is almost always the first bone tested - so the nominal
            // O(vertices x bones) is O(vertices) in practice.
            bool bDominantIsFarthest = true;
            for (int32 BoneIndex = 0; BoneIndex < BonePositions.Num(); ++BoneIndex)
            {
                if (BoneIndex == DominantBone)
                {
                    continue;
                }
                if (UE::Geometry::Distance(Position, BonePositions[BoneIndex]) >= DominantDistance)
                {
                    bDominantIsFarthest = false;
                    break;
                }
            }
            if (!bDominantIsFarthest)
            {
                continue;
            }

            ++Coverage.FarBoneCount;
            if (DominantDistance > Coverage.WorstFarBoneDistance)
            {
                Coverage.WorstFarBoneDistance = DominantDistance;
                Coverage.WorstFarBoneVertex = VertexID;
                Coverage.WorstFarBoneName = BonesInfo[DominantBone].Name;
            }
        }
    });
}

namespace
{
    // Which way triangle Tri traverses the undirected edge (A, B): +1 for A->B, -1 for
    // B->A, 0 when the triangle does not carry both corners.
    //
    // Two triangles sharing an edge are consistently wound exactly when they traverse it
    // in OPPOSITE directions - that is the definition of a coherently oriented surface,
    // and it is what makes the two products below multiply to -1.
    int32 GeometryUtils_EdgeTraversalSign(const UE::Geometry::FIndex3i& Tri, int32 A, int32 B)
    {
        for (int32 Corner = 0; Corner < 3; ++Corner)
        {
            const int32 Current = Tri[Corner];
            const int32 Next = Tri[(Corner + 1) % 3];
            if (Current == A && Next == B) { return 1; }
            if (Current == B && Next == A) { return -1; }
        }
        return 0;
    }
}

FMeshOrientation MeasureMeshOrientation(UDynamicMesh* Mesh)
{
    FMeshOrientation Orientation;
    if (!Mesh)
    {
        return Orientation;
    }

    const UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();

    for (int32 EdgeID : EditMesh.EdgeIndicesItr())
    {
        if (EditMesh.IsBoundaryEdge(EdgeID))
        {
            ++Orientation.BoundaryEdges;
            continue;
        }

        // Hand-rolled because the engine has no read-only equivalent. The one orientation
        // utility in GeometryProcessing is FMeshRepairOrientation
        // (DynamicMesh/Public/Operations/RepairOrientation.h:22), whose only public entry
        // points - OrientComponents(:47) and SolveGlobalOrientation(:50) - both MUTATE the
        // mesh, and FDynamicMesh3 has no IsSameOrientation. Measuring by repairing would
        // destroy the evidence and change the asset.
        const UE::Geometry::FIndex2i EdgeV = EditMesh.GetEdgeV(EdgeID);
        const UE::Geometry::FIndex2i EdgeT = EditMesh.GetEdgeT(EdgeID);
        const int32 SignA =
            GeometryUtils_EdgeTraversalSign(EditMesh.GetTriangle(EdgeT.A), EdgeV.A, EdgeV.B);
        const int32 SignB =
            GeometryUtils_EdgeTraversalSign(EditMesh.GetTriangle(EdgeT.B), EdgeV.A, EdgeV.B);

        // A zero on either side means a triangle did not carry the edge's two corners,
        // which cannot happen for an edge the mesh reports as adjacent to it. Requiring
        // the product to be exactly -1 (rather than testing != -1) keeps such an
        // impossible state OUT of the defect count, so a connectivity bug elsewhere
        // cannot surface here as a phantom winding fault.
        if (SignA * SignB == 1)
        {
            ++Orientation.InconsistentEdges;
        }
    }

    // TMeshQueries::GetVolumeArea (GeometryCore/Public/MeshQueries.h:134) rather than a
    // hand-rolled sum. It is ALREADY signed - the per-triangle term is
    // `N.X * (V0.X + V1.X + V2.X)` with `N = (V2-V0) x (V1-V0)`, and there is no abs()
    // anywhere in the volume accumulation (only the area term uses .Length()). That cross
    // product is the engine's facing normal, the NEGATION of the right-hand rule, because
    // Unreal is left-handed (VectorUtil::Normal, VectorUtil.h:80-87) - which is exactly
    // what makes .X positive for a shell whose facing normals point outward and negative
    // for the same shell wound inside out.
    //
    // .X is the volume with the 1/6 already applied; .Y is the surface area and is
    // unsigned.
    const FVector2d VolumeArea =
        UE::Geometry::TMeshQueries<UE::Geometry::FDynamicMesh3>::GetVolumeArea(EditMesh);
    Orientation.SignedVolume = VolumeArea.X;
    Orientation.SurfaceArea = VolumeArea.Y;

    return Orientation;
}

FMeshHealth MeasureMeshHealth(UDynamicMesh* Mesh)
{
    FMeshHealth Health;
    if (!Mesh)
    {
        return Health;
    }

    const UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();

    // Boundary edges and both orientation signals come from the one shared walk, so the
    // per-part measurement the .pwmodel compiler runs and this whole-mesh one cannot
    // disagree about the same mesh.
    const FMeshOrientation Orientation = MeasureMeshOrientation(Mesh);
    Health.BoundaryEdges = Orientation.BoundaryEdges;
    Health.InconsistentEdges = Orientation.InconsistentEdges;
    Health.SignedVolume = Orientation.SignedVolume;

    // Area = 0.5 * |(v1-v0) x (v2-v0)|, from the three vertex positions.
    for (int32 TriangleID : EditMesh.TriangleIndicesItr())
    {
        FVector3d V0, V1, V2;
        EditMesh.GetTriVertices(TriangleID, V0, V1, V2);
        const double TriArea = 0.5 * FVector3d::CrossProduct(V1 - V0, V2 - V0).Length();
        if (TriArea < DegenerateAreaEpsilon)
        {
            ++Health.DegenerateTriangles;
        }
    }

    for (int32 VertexID : EditMesh.VertexIndicesItr())
    {
        if (EditMesh.IsBowtieVertex(VertexID))
        {
            ++Health.NonManifoldVertices;
        }

        // Folded into the bowtie sweep rather than given its own pass: both are one test per
        // live vertex, and this walk is documented as O(E + T + V) and is run per part as well
        // as per model. IsReferencedVertex is the engine's own accessor for it (refcount > 1 -
        // allocation takes one reference, an edge or triangle takes the rest), so this cannot
        // drift from what the mesh itself calls referenced.
        if (!EditMesh.IsReferencedVertex(VertexID))
        {
            ++Health.UnreferencedVertices;
        }
    }

    Health.ComponentCount = UGeometryScriptLibrary_MeshQueryFunctions::GetNumConnectedComponents(Mesh);
    Health.VertexCount = EditMesh.VertexCount();
    Health.TriangleCount = EditMesh.TriangleCount();

    return Health;
}

FMeshSelfIntersection MeasureMeshSelfIntersection(UDynamicMesh* Mesh)
{
    FMeshSelfIntersection Out;
    if (!Mesh)
    {
        return Out;
    }

    const UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();
    const int32 TriangleCount = EditMesh.TriangleCount();
    if (TriangleCount == 0 || TriangleCount > SelfIntersectionMaxTriangles)
    {
        return Out;
    }

    // Degenerate triangles are excluded from the QUERY, not from the tree. A zero-area
    // triangle has no normal, and the engine's separating-axis test then projects every
    // candidate onto a zero axis and finds no separation - it answers "intersecting" for
    // every pair it is handed (IntrTriangle3Triangle3.h: UnitCross of two collinear edges
    // is the zero vector). Two shipped example models carry degenerates, 6 and 70, so
    // leaving them in would report those models as self-intersecting wherever a sliver
    // sits. A triangle with no area has no surface to embed. Same threshold as
    // FMeshHealth::DegenerateTriangles, so the two cannot disagree about which triangles
    // those are.
    TBitArray<> Usable(false, EditMesh.MaxTriangleID());
    for (const int32 TriangleID : EditMesh.TriangleIndicesItr())
    {
        FVector3d V0, V1, V2;
        EditMesh.GetTriVertices(TriangleID, V0, V1, V2);
        const double TriArea = 0.5 * FVector3d::CrossProduct(V1 - V0, V2 - V0).Length();
        Usable[TriangleID] = TriArea >= DegenerateAreaEpsilon;
    }

    UE::Geometry::FMeshConnectedComponents Connected(&EditMesh);
    Connected.FindConnectedTriangles();

    Out.bMeasured = true;

    // Coplanar reporting is ON, and that is the whole reason the membrane is visible: the
    // engine's tri-tri routine returns FALSE for two triangles in the same plane unless it
    // is asked otherwise (IntrTriangle3Triangle3.h, Test(): "if
    // (!bReportCoplanarIntersection) return false"), and the membrane is two coincident,
    // oppositely wound fans - coplanar by construction. Test() before Find() is the
    // engine's own ordering, kept: the cheap separating-axis rejection runs on every
    // candidate pair and the intersection geometry is computed only for the few that
    // survive it.
    const TFunction<bool(UE::Geometry::FIntrTriangle3Triangle3d&)> IntersectFn =
        [](UE::Geometry::FIntrTriangle3Triangle3d& Intr)
        {
            Intr.SetReportCoplanarIntersection(true);
            return UE::Geometry::FDynamicMeshAABBTree3::TriangleIntersection(Intr);
        };

    UE::Geometry::IMeshSpatial::FQueryOptions Options;
    Options.TriangleFilterF = [&Usable](int TriangleID)
    {
        return Usable.IsValidIndex(TriangleID) && Usable[TriangleID];
    };

    // Keyed on the triangle pair rather than counted per reported record: one crossing
    // between two triangles can be reported as up to three segments, and the answer is
    // "how many pairs of triangles cross", not "how many segments the engine emitted".
    TSet<uint64> Pairs;
    for (int32 ComponentIndex = 0; ComponentIndex < Connected.Num(); ++ComponentIndex)
    {
        const TArray<int32>& ComponentTriangles =
            Connected.GetComponent(ComponentIndex).Indices;
        if (ComponentTriangles.Num() < 2)
        {
            continue;
        }

        UE::Geometry::FDynamicMeshAABBTree3 Tree(&EditMesh, /*bAutoBuild=*/false);
        Tree.Build(ComponentTriangles);

        const MeshIntersection::FIntersectionsQueryResult Hits =
            Tree.FindAllSelfIntersections(/*bIgnoreTopoConnected=*/true, Options, IntersectFn);

        bool bComponentCrossesItself = false;
        auto Consider = [&Out, &Pairs, &bComponentCrossesItself](
            int32 TriangleA, int32 TriangleB, const FVector3d& Point)
        {
            bComponentCrossesItself = true;
            if (Out.bTruncated)
            {
                return;
            }
            const uint64 Key =
                (static_cast<uint64>(static_cast<uint32>(FMath::Min(TriangleA, TriangleB))) << 32)
                | static_cast<uint64>(static_cast<uint32>(FMath::Max(TriangleA, TriangleB)));
            bool bAlreadyPresent = false;
            Pairs.Add(Key, &bAlreadyPresent);
            if (bAlreadyPresent)
            {
                return;
            }
            if (!Out.bHasWitness)
            {
                Out.bHasWitness = true;
                Out.Witness = Point;
            }
            if (Pairs.Num() >= SelfIntersectionMaxPairs)
            {
                Out.bTruncated = true;
            }
        };

        for (const MeshIntersection::FPointIntersection& Hit : Hits.Points)
        {
            Consider(Hit.TriangleID[0], Hit.TriangleID[1], Hit.Point);
        }
        for (const MeshIntersection::FSegmentIntersection& Hit : Hits.Segments)
        {
            Consider(Hit.TriangleID[0], Hit.TriangleID[1], Hit.Point[0]);
        }
        for (const MeshIntersection::FPolygonIntersection& Hit : Hits.Polygons)
        {
            Consider(Hit.TriangleID[0], Hit.TriangleID[1], Hit.Point[0]);
        }

        if (bComponentCrossesItself)
        {
            ++Out.SelfIntersectingComponents;
        }
    }

    Out.PairCount = Pairs.Num();
    return Out;
}

void SetMeshCountFields(UDynamicMesh* Mesh, const TSharedPtr<FJsonObject>& Result)
{
    Result->SetNumberField(TEXT("vertexCount"), GetMeshVertexCount(Mesh));
    // UDynamicMesh::GetTriangleCount() — the MeshQueryFunctions library accessor
    // for triangle count is commented out (does not exist) in UE 5.7.
    Result->SetNumberField(TEXT("triangleCount"), Mesh ? Mesh->GetTriangleCount() : 0);
}

bool MeshHasUVElementsInChannel(UDynamicMesh* Mesh, int32 UVChannel)
{
    if (!Mesh || UVChannel < 0)
    {
        return false;
    }

    const UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();
    if (!EditMesh.HasAttributes())
    {
        return false;
    }

    const UE::Geometry::FDynamicMeshAttributeSet* Attributes = EditMesh.Attributes();
    if (!Attributes || Attributes->NumUVLayers() <= UVChannel)
    {
        return false;
    }

    // A layer that exists but carries no elements is the case this is written for: the bake's
    // MikkT pass reads channel 0 and indexes a size-0 UV array (see MeshHasUsableUVs), and the
    // compiler's merge ships a part untextured off exactly the same shape. Require at least one
    // element, not just a layer.
    const UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = Attributes->GetUVLayer(UVChannel);
    return UVOverlay != nullptr && UVOverlay->ElementCount() > 0;
}

bool MeshHasUsableUVs(UDynamicMesh* Mesh)
{
    // MikkT tangent generation only reads the primary UV layer (channel 0), so this guard is
    // channel 0 and nothing else. Defined on the shared predicate rather than re-deriving the
    // attribute walk, so the two can never answer differently for channel 0.
    return MeshHasUVElementsInChannel(Mesh, 0);
}

bool MeshHasUVsOnEveryTriangle(UDynamicMesh* Mesh, int32 UVChannel)
{
    // The O(1) half first: a null mesh, no attribute set, an absent channel or an element-less
    // one all fail here without touching a triangle.
    if (!MeshHasUVElementsInChannel(Mesh, UVChannel))
    {
        return false;
    }

    const UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();
    const UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = EditMesh.Attributes()->GetUVLayer(UVChannel);

    // IsSetTriangle indexes ElementTriangles[3*TID] with no bounds test of its own, so the
    // storage-coverage question has to be answered BEFORE the sweep, not inside it. UE 5.8
    // only - see docs/engine-version-support.md, which prescribes exactly this backport: on
    // 5.3-5.7 the predicate cannot be reimplemented from outside the class (ElementTriangles is
    // protected with no public accessor), so the sweep below runs unguarded there, as every
    // engine caller other than MeshBakeFunctions does.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    if (!UVOverlay->IsTriangleStorageValid())
    {
        return false;
    }
#endif

    for (const int32 TriangleID : EditMesh.TriangleIndicesItr())
    {
        if (!UVOverlay->IsSetTriangle(TriangleID))
        {
            return false;
        }
    }

    return true;
}

bool EnsureMeshHasUVs(UDynamicMesh* Mesh)
{
    if (!Mesh)
    {
        return false;
    }
    if (MeshHasUsableUVs(Mesh))
    {
        return true;
    }

    // Guarantee UV channel 0 exists to project into (enables mesh attributes + a UV
    // layer; the layer is still element-less until the projection fills it).
    //
    // Grow-only through EnsureMeshHasUVChannel, NEVER the bare
    // SetNumUVSets(Mesh, 1, nullptr) this used to call: SetNumUVSets sets the layer count
    // EXACTLY, so it truncated every layer above 0. The reachable case is not theoretical -
    // a mesh with an authored lightmap in channel 1 and an empty channel 0 is exactly the
    // shape that gets here (an empty channel 0 is what MeshHasUsableUVs rejects), so
    // convert_to_static_mesh destroyed channel 1 on its way past this guard and
    // MarkGeometryActorModified then committed the loss to the live actor.
    EnsureMeshHasUVChannel(Mesh, 0);

    // Frame the box projection on the mesh bounding box so the whole mesh lands in
    // ~[0,1] UV space and the baked asset is texturable. Clamp each axis to a
    // positive minimum so a flat/degenerate (or empty) mesh cannot yield a zero-scale
    // frame — the projection stays finite and cannot divide by zero.
    const UE::Geometry::FAxisAlignedBox3d Bounds = Mesh->GetMeshRef().GetBounds();
    const FVector Center = Bounds.Center();
    FVector BoxSize = Bounds.Diagonal();
    BoxSize.X = FMath::Max(FMath::Abs(BoxSize.X), 1.0);
    BoxSize.Y = FMath::Max(FMath::Abs(BoxSize.Y), 1.0);
    BoxSize.Z = FMath::Max(FMath::Abs(BoxSize.Z), 1.0);

    // Deterministic per-triangle box projection (no solver, cannot hang) into an
    // empty selection == the entire mesh. Mirrors the geometry.project_uv handler.
    const FTransform BoxFrame(FQuat::Identity, Center, BoxSize);
    UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromBoxProjection(
        Mesh, /*UVSetIndex=*/0, BoxFrame, FGeometryScriptMeshSelection(), /*MinIslandTriCount=*/2, nullptr);

    return MeshHasUsableUVs(Mesh);
}

bool EnsureMeshHasUVChannel(UDynamicMesh* Mesh, int32 UVChannel)
{
    if (!Mesh || UVChannel < 0)
    {
        return false;
    }

    // GeometryScript's SetNumUVSets sets the layer count EXACTLY (it truncates when
    // asked for fewer than exist), so guard it grow-only: only widen when the
    // requested channel is beyond the current set, never shrink away higher UV
    // layers a mesh may already carry. SetNumUVSets enables attributes and creates
    // the missing layers (element-less); the caller's projection/unwrap fills them.
    const int32 RequiredSets = UVChannel + 1;
    if (UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(Mesh) < RequiredSets)
    {
        UGeometryScriptLibrary_MeshUVFunctions::SetNumUVSets(Mesh, RequiredSets, nullptr);
    }

    // Report whether the channel now exists — SetNumUVSets rejects >8 sets, so an
    // out-of-range channel stays absent and the caller can still detect the no-op.
    return UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(Mesh) >= RequiredSets;
}

bool MeshNormalParentsAreValid(UDynamicMesh* Mesh)
{
    if (!Mesh)
    {
        return true;
    }

    const UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();
    if (!EditMesh.HasAttributes())
    {
        return true;
    }

    const UE::Geometry::FDynamicMeshNormalOverlay* Normals = EditMesh.Attributes()->PrimaryNormals();
    if (Normals == nullptr)
    {
        return true;
    }

    // IsVertex, not `!= InvalidID`. InvalidID is the only value the overlay itself writes, but
    // IsVertex is the predicate the crashing read actually needs (GetVertex's own checkSlow), so
    // a stale positive parent would be caught too rather than waved through.
    for (const int32 ElementID : Normals->ElementIndicesItr())
    {
        if (!EditMesh.IsVertex(Normals->GetParentVertex(ElementID)))
        {
            return false;
        }
    }

    return true;
}

bool EnsureMeshNormalParentsAreValid(UDynamicMesh* Mesh, int32& OutFreedElements)
{
    OutFreedElements = 0;

    // Answers the null / no-attributes / no-normal-layer cases too, so the edit below can assume
    // an overlay exists. Every mesh that has never been remeshed or generator-copied lands here.
    if (MeshNormalParentsAreValid(Mesh))
    {
        return true;
    }

    Mesh->EditMesh([&OutFreedElements](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        UE::Geometry::FDynamicMeshNormalOverlay* Normals = EditMesh.Attributes()->PrimaryNormals();
        const int32 ElementsBefore = Normals->ElementCount();

        // FreeUnusedElements decrements every element whose refcount is still 1 - allocated but
        // claimed by no triangle - which frees it and leaves IsElement false, so the deformers'
        // ParallelFor skips it. It does not touch an element any triangle uses.
        Normals->FreeUnusedElements();

        OutFreedElements = ElementsBefore - Normals->ElementCount();
    },
    EDynamicMeshChangeType::AttributeEdit, EDynamicMeshAttributeChangeFlags::NormalsTangents, false);

    // Re-asked against the mesh rather than inferred from the count: the caller must fail rather
    // than call the engine op if anything survived, and "how many were freed" does not answer that.
    return MeshNormalParentsAreValid(Mesh);
}

void MarkGeometryActorModified(UDynamicMeshComponent* Component, bool bNotifyMesh)
{
    if (!Component)
    {
        return;
    }

    AActor* Owner = Component->GetOwner();

    // Modify() with GUndo null (always, here — the geometry namespace opens no
    // transaction) is exactly MarkPackageDirty(): SaveToTransactionBuffer marks the
    // package and skips GUndo->SaveObject. It is spelled Modify() to match the house
    // pattern and to become a real snapshot for free if an outer transaction is ever
    // introduced. Only the ACTOR is modified — a mesh edit leaves the actor's own
    // UPROPERTYs untouched, so a post-hoc snapshot of them is state-neutral, whereas
    // Modify()-ing the UDynamicMesh would serialize the whole mesh (see the header).
    if (Owner)
    {
        Owner->Modify();
    }

    if (bNotifyMesh)
    {
        Component->NotifyMeshUpdated();
    }

    // Belt-and-braces: Modify() only dirties when the object carries RF_Transactional.
    // The level must end up dirty regardless, or level.save silently no-ops and the
    // user's edit is lost when the editor closes — which is the entire bug.
    if (Owner)
    {
        Owner->MarkPackageDirty();
    }
    else
    {
        Component->MarkPackageDirty();
    }
}

void MarkGeometryActorSpawned(AActor* Actor)
{
    if (!Actor)
    {
        return;
    }

    Actor->Modify();
    Actor->MarkPackageDirty();

    // The level's actor list changed too. Under OFPA that is a different package from
    // the actor's own; on a classic map it is the same one and this is a no-op.
    if (ULevel* Level = Actor->GetLevel())
    {
        Level->MarkPackageDirty();
    }
}

} // namespace GeometryUtils
