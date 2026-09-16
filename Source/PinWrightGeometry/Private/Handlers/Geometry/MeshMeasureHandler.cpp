// Copyright (c) 2026 Alexander Penkin. MIT License.

// MeshMeasureHandler.cpp - Non-mutating dynamic-mesh inspection RPCs:
// geometry.measure (bbox/dimensions/volume/area/counts) and geometry.check_health
// (manifold/boundary/degenerate report). Both are READ-ONLY: they never call
// NotifyMeshUpdated and never mutate the mesh.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Geometry/GeometryTarget.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Utils/JsonBuilders.h"
#include "Dom/JsonObject.h"

#include "Components/DynamicMeshComponent.h"
// FGeometryTarget::Actor is ADynamicMeshActor*: the complete type is required here even with no
// literal ADynamicMeshActor spelled in this file - Target.Actor is member-called / converted to
// AActor*, and a derived-to-base conversion needs the definition, not a forward declaration.
#include "DynamicMeshActor.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "Editor.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif

#include "GeometryScript/MeshQueryFunctions.h"


// ============================================================================
// measure
// ============================================================================
REGISTER_RPC_HANDLER("geometry.measure", "geometry",
    "Measure a dynamic mesh (read-only): bounding box {min,max,center,size}, volume, surface area, and vertex/triangle/edge counts. space='world' reports the actor-transformed box; nothing is mutated.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_DEF("space", "string", "Coordinate space for the bounding box: 'local' (mesh space, default) or 'world' (bbox/center transformed by the actor transform)", "local")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    const FString SpaceArg = Ctx.GetString(TEXT("space"), TEXT("local"));
    const bool bWorldSpace = SpaceArg.Equals(TEXT("world"), ESearchCase::IgnoreCase);

    // Direct read-only access to the mesh for the edge count (MeshQueryFunctions has
    // no edge-count accessor) and the topology counts.
    const UE::Geometry::FDynamicMesh3& Mesh = Target.Mesh->GetMeshRef();
    const int32 VertexCount = Mesh.VertexCount();
    const int32 TriangleCount = Mesh.TriangleCount();
    const int32 EdgeCount = Mesh.EdgeCount();

    // Local-space bounding box. An empty/degenerate mesh yields the inverted "empty"
    // FBox (Min=+DBL_MAX, Max=-DBL_MAX), whose GetSize()/GetCenter() overflow to a
    // non-finite value that serializes to an invalid JSON token. Report a zeroed box
    // for that case, matching geometry.get_mesh_info's defensive handling.
    const FBox LocalBounds = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshBoundingBox(Target.Mesh);
    const bool bEmptyBounds = (VertexCount == 0) || (LocalBounds.IsValid == 0);
    const FBox SafeLocalBounds = bEmptyBounds ? FBox(FVector::ZeroVector, FVector::ZeroVector) : LocalBounds;

    // For world space, re-fit an axis-aligned box around the actor-transformed corners
    // of the local box (FBox::TransformBy). volume/area stay in mesh-local units.
    const FBox ReportBounds = bWorldSpace
        ? SafeLocalBounds.TransformBy(Target.Actor->GetActorTransform())
        : SafeLocalBounds;

    float SurfaceArea = 0.f;
    float Volume = 0.f;
    UGeometryScriptLibrary_MeshQueryFunctions::GetMeshVolumeArea(Target.Mesh, SurfaceArea, Volume);

    // size = full extents (Max - Min) = 2 * half-extent.
    TSharedPtr<FJsonObject> BBox = MakeShared<FJsonObject>();
    BBox->SetObjectField(TEXT("min"), JsonBuilders::BuildVectorJson(ReportBounds.Min));
    BBox->SetObjectField(TEXT("max"), JsonBuilders::BuildVectorJson(ReportBounds.Max));
    BBox->SetObjectField(TEXT("center"), JsonBuilders::BuildVectorJson(ReportBounds.GetCenter()));
    BBox->SetObjectField(TEXT("size"), JsonBuilders::BuildVectorJson(ReportBounds.GetSize()));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetObjectField(TEXT("bbox"), BBox);
    Result->SetNumberField(TEXT("volume"), Volume);
    Result->SetNumberField(TEXT("area"), SurfaceArea);
    Result->SetNumberField(TEXT("vertexCount"), VertexCount);
    Result->SetNumberField(TEXT("triangleCount"), TriangleCount);
    Result->SetNumberField(TEXT("edgeCount"), EdgeCount);
    Result->SetStringField(TEXT("space"), bWorldSpace ? TEXT("world") : TEXT("local"));
    Result->SetStringField(TEXT("units"), TEXT("cm"));

    Ctx.SendSuccess(TEXT("Mesh measured"), Result);
    return true;
}

// ============================================================================
// check_health
// ============================================================================
REGISTER_RPC_HANDLER("geometry.check_health", "geometry",
    "Report geometry defects of a dynamic mesh (read-only): boundary/open edges, degenerate (near-zero-area) triangles, non-manifold (bowtie) vertices, orphaned (triangle-unreferenced) vertices, and WINDING - a signed volume that goes negative on a closed shell that is inside out, plus a count of adjacent triangles that disagree on winding. Also a connected-component count and a single healthy verdict. checkSelfIntersection=true adds EMBEDDING - whether the surface passes through ITSELF - which no other field can see; 'selfIntersectionsMeasured' always states whether it was measured, so a missing answer can never be read as a clean one. Nothing is mutated.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_DEF("checkSelfIntersection", "boolean",
            "Also measure whether the surface crosses itself, reporting the same "
            "selfIntersections / selfIntersectingComponents / selfIntersectionsTruncated fields "
            "model.compile and model.validate publish on their 'health' block. OFF by default "
            "because it is a different cost class from every other field here: one AABB tree per "
            "connected component plus a tree-vs-itself descent, against the allocation-free "
            "O(E + T + V) walk the rest comes from. Read the verdict off "
            "'selfIntersectionsMeasured', which is false both when you did not ask and when the "
            "measurement declined (empty mesh, or over its triangle budget) - the counts are "
            "absent rather than zero in either case.", "false")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    // One walk, shared with the .pwmodel compiler's final validation stage. It used to
    // live inline here, so the compiler had no way to ask the same question and shipped
    // open shells and degenerate triangles as successful assets. GeometryUtils::FMeshHealth
    // carries the definitions of all four defects and the DegenerateAreaEpsilon threshold.
    const GeometryUtils::FMeshHealth Health = GeometryUtils::MeasureMeshHealth(Target.Mesh);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetBoolField(TEXT("isClosed"), Health.IsClosed());
    Result->SetNumberField(TEXT("boundaryEdges"), Health.BoundaryEdges);
    Result->SetNumberField(TEXT("degenerateTriangles"), Health.DegenerateTriangles);
    Result->SetNumberField(TEXT("nonManifoldVertices"), Health.NonManifoldVertices);
    Result->SetNumberField(TEXT("componentCount"), Health.ComponentCount);
    Result->SetNumberField(TEXT("vertexCount"), Health.VertexCount);
    Result->SetNumberField(TEXT("triangleCount"), Health.TriangleCount);

    // ORPHANS. Already measured by the walk above - folded into its bowtie sweep - and reported
    // here so this response and the .pwmodel `health` block carry the same field set. The two
    // answer about the same mesh from the same struct, and a caller who learned the field from
    // the model.* docs finding it missing here is the disagreement this emit removes.
    //
    // `vertexCount` is the vertex buffer; a vertex no triangle names is counted in it and is part
    // of no surface, so it is the only field that reconciles that count with any triangle-reduced
    // measurement of the same mesh. Non-zero is also a signal on its own: an op removed geometry
    // and left the leftovers behind. Deliberately NOT part of `healthy` - an orphan is inert
    // against a static-mesh build, which is driven by triangles, so failing a mesh on it would
    // refuse models that are correct.
    Result->SetNumberField(TEXT("unreferencedVertices"), Health.UnreferencedVertices);

    // WINDING, which every field above is blind to. A closed mesh wound inside out matches a
    // correct one on all of them - closed, 0 boundary edges, 0 bowties, same counts - and
    // renders identically, because backface culling shows the camera whichever wall faces it.
    // signedVolume is the field that separates them; it is only meaningful when isClosed, so
    // the gate is `isClosed && signedVolume > 0` and never signedVolume alone.
    // orientationConsistent is the independent second signal: it catches neighbours that
    // disagree, which a UNIFORM inversion does not produce.
    Result->SetNumberField(TEXT("signedVolume"), Health.SignedVolume);
    Result->SetBoolField(TEXT("orientationConsistent"), Health.IsOrientationConsistent());
    Result->SetNumberField(TEXT("inconsistentEdges"), Health.InconsistentEdges);
    Result->SetBoolField(TEXT("inverted"), Health.IsInverted());

    // EMBEDDING, which every field above is blind to and which walks straight through the
    // `isClosed && signedVolume > 0` gate they document. A membrane spanning a bore is two
    // oppositely wound fans whose volume contributions cancel EXACTLY - the number that comes
    // back is the figure the intended solid would have had - and a sweep whose walls were pushed
    // through each other degrades signedVolume smoothly, with no threshold anywhere on it.
    //
    // OPT-IN, and that is not timidity: MeasureMeshSelfIntersection builds an AABB tree per
    // connected component and descends it against itself, a different cost class from the
    // allocation-free O(E + T + V) walk everything above comes from and which the .pwmodel
    // compiler also runs per part. Charging four callers for a measurement they did not ask for
    // is what kept it out of MeasureMeshHealth in the first place.
    //
    // `selfIntersectionsMeasured` is emitted UNCONDITIONALLY, and the counts only when it is
    // true. That pairing is the point of the block: a caller who read the three-term gate off
    // the model.* docs must be able to tell "no crossings" from "nobody looked", and a `0` would
    // say the first while meaning the second - the same cannot-fail gate `signedVolume` exists to
    // remove. It reads false both when the caller did not opt in and when the measurement itself
    // declined (empty mesh, or above SelfIntersectionMaxTriangles), because neither is a signal.
    //
    // NOT folded into `healthy`, which stays FMeshHealth::IsHealthy() - the verdict must not
    // change shape with a parameter, and the model.* side publishes these as gate TERMS with no
    // verdict of its own for the same reason. The documented gate is
    // `isClosed && signedVolume > 0 && selfIntersections === 0`, evaluated by the caller.
    GeometryUtils::FMeshSelfIntersection SelfIntersection;
    if (Ctx.GetBool(TEXT("checkSelfIntersection"), false))
    {
        SelfIntersection = GeometryUtils::MeasureMeshSelfIntersection(Target.Mesh);
    }
    Result->SetBoolField(TEXT("selfIntersectionsMeasured"), SelfIntersection.bMeasured);
    if (SelfIntersection.bMeasured)
    {
        Result->SetNumberField(TEXT("selfIntersections"), SelfIntersection.PairCount);
        Result->SetNumberField(TEXT("selfIntersectingComponents"),
            SelfIntersection.SelfIntersectingComponents);
        Result->SetBoolField(TEXT("selfIntersectionsTruncated"), SelfIntersection.bTruncated);
    }

    Result->SetBoolField(TEXT("healthy"), Health.IsHealthy());

    Ctx.SendSuccess(TEXT("Mesh health checked"), Result);
    return true;
}
