// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryOps_Elements.h - The element-edit half of the geometry verb set as pure functions.
//
// These are the ten verbs that address a mesh by ELEMENT INDEX rather than by shape: single
// vertices, single triangles, one vertex's color or UV, and the whole-mesh translate that
// belongs with them because it is the one transform the element verbs share. Eight of them
// have a pure function here; unwrap_uv and pack_uv_islands stay routed through
// GeometryUtils::ApplyXAtlasUnwrap, which now owns only the RESPONSE - the geometry under it
// is the modeling family's GeometryOps::UnwrapUVXAtlas, which geometry.auto_uv calls
// directly. So no verb in this family still runs un-extracted code (see MeshInfoHandler.cpp).
//
// Contract, identical across every GeometryOps_<family> header:
//   - The op takes a UDynamicMesh* and one F<Verb>Params struct. It never sees FHandlerContext,
//     never sees an AActor, never marks a package dirty, and never sends a response.
//   - It returns FOpResult carrying the SAME ErrorCodes.h value the handler emitted at that
//     failure point before extraction. The RPC wrapper forwards it verbatim, which is the
//     single property that keeps the dispatcher tests blind to the split.
//   - An op that produces a value the wrapper echoes takes it as a trailing out-parameter.
//     FOpResult carries only what every op shares, so a per-op payload does not belong in it.
//
// Parameter names are the ONE vocabulary both front-ends compile against. Where the published
// RPC contract uses a different spelling the wrapper translates - set_vertex_color's four
// r/g/b/a numbers become one FLinearColor, set_uvs' u/v become one FVector2D - so the JSON
// contract is unchanged while .pwmodel maps onto the struct fields directly. actorName is
// plumbing and deliberately has no field anywhere in this header.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/Geometry/GeometryOps.h"

class UDynamicMesh;

namespace GeometryOps
{
    // --- set_vertex_position ------------------------------------------------------------
    struct FSetVertexPositionParams
    {
        int32 VertexIndex = INDEX_NONE;
        FVector Position = FVector::ZeroVector;
    };

    // INVALID_VERTEX when VertexIndex is not a live vertex ID. bChanged is false when the
    // vertex already sat at Position.
    FOpResult SetVertexPosition(UDynamicMesh* Mesh, const FSetVertexPositionParams& Params);

    // --- append_vertex ------------------------------------------------------------------
    struct FAppendVertexParams
    {
        FVector Position = FVector::ZeroVector;
    };

    FOpResult AppendVertex(UDynamicMesh* Mesh, const FAppendVertexParams& Params, int32& OutVertexIndex);

    // --- delete_vertex ------------------------------------------------------------------
    struct FDeleteVertexParams
    {
        int32 VertexIndex = INDEX_NONE;
    };

    // INVALID_VERTEX when VertexIndex is not a live vertex ID. A vertex whose removal the mesh
    // refuses (RemoveVertex != Ok) is NOT an error - it reports success with bChanged false,
    // which is the `success:false` inside a successful response the verb has always returned.
    // bChanged here is the mesh's own verdict, deliberately NOT the count delta: a refused
    // removal can still have deleted incident triangles first, so the two disagree.
    FOpResult DeleteVertex(UDynamicMesh* Mesh, const FDeleteVertexParams& Params);

    // --- append_triangle ----------------------------------------------------------------
    struct FAppendTriangleParams
    {
        FVector V0 = FVector(0, 0, 0);
        FVector V1 = FVector(100, 0, 0);
        FVector V2 = FVector(50, 100, 0);
        int32 GroupID = 0;
    };

    struct FAppendTriangleIndices
    {
        int32 TriangleIndex = INDEX_NONE;
        int32 VertexIndices[3] = { INDEX_NONE, INDEX_NONE, INDEX_NONE };
    };

    // Appends three unshared vertices and one triangle over them. The three vertices are
    // appended unconditionally, so a rejected triangle (AppendTriangle returns a negative
    // EMeshResult code) still leaves them behind - preserved as-is because callers build
    // meshes vertex-by-vertex against exactly that behaviour.
    FOpResult AppendTriangle(UDynamicMesh* Mesh, const FAppendTriangleParams& Params, FAppendTriangleIndices& OutIndices);

    // --- delete_triangle ----------------------------------------------------------------
    struct FDeleteTriangleParams
    {
        int32 TriangleIndex = INDEX_NONE;
    };

    // INVALID_TRIANGLE when TriangleIndex is not a live triangle ID. As with DeleteVertex, a
    // refused removal is success-with-bChanged-false rather than an error.
    FOpResult DeleteTriangle(UDynamicMesh* Mesh, const FDeleteTriangleParams& Params);

    // --- set_vertex_color ---------------------------------------------------------------
    struct FSetVertexColorParams
    {
        // Ignored when bSetAll is true.
        int32 VertexIndex = INDEX_NONE;
        FLinearColor Color = FLinearColor::White;
        bool bSetAll = false;

        // Which components of Color actually land. Defaults to All, so a caller that names no
        // channels writes all four exactly as before. Anything narrower leaves the other
        // components of each touched element at the value they already carried, which is what
        // makes an RGB tint and an alpha mask co-exist on one mesh.
        EColorChannels Channels = EColorChannels::All;
    };

    // Writes the mesh attribute-overlay PrimaryColors channel, never the legacy
    // FDynamicMesh3 per-vertex color buffer: the overlay is the only channel
    // GetHasVertexColors, the DynamicMeshComponent renderer, and the StaticMesh bake read,
    // and it is the only one carrying alpha. INVALID_VERTEX when bSetAll is false and
    // VertexIndex is not a live vertex ID. OutVerticesModified is 0 when the target vertex
    // owns no overlay element, which is a real outcome on a sparse or seamed overlay.
    // OutVerticesModified is the number of DISTINCT VERTICES that now carry the colour, counted
    // from the overlay elements actually written - not the mesh's vertex count. Under bSetAll it
    // used to be the latter unconditionally, which reported a whole mesh painted while the loop
    // had reached only the elements that happened to exist.
    //
    // OutElementsCreated, when supplied, receives how many colour elements had to be CREATED to
    // cover triangles that carried none. It is non-zero exactly when the mesh had grown since the
    // last colour write - an append, bevel, shell or boolean brings in triangles with no colour
    // element, and those used to be unreachable by every later set_all. Optional so the .pwmodel
    // compiler's call site is unchanged.
    FOpResult SetVertexColor(UDynamicMesh* Mesh, const FSetVertexColorParams& Params,
        int32& OutVerticesModified, int32* OutElementsCreated = nullptr);

    // --- bake_ambient_occlusion ---------------------------------------------------------
    //
    // Belongs in the element family because what it produces is a per-corner colour write - the
    // same overlay, the same seam rules and the same channel mask as SetVertexColor. What it adds
    // in front of that write is the measurement, and the measurement is the engine's: the ray
    // integral is FMeshOcclusionMapEvaluator driven by FMeshVertexBaker over a
    // FDynamicMeshAABBTree3 of the same mesh, not a sampler written here.
    struct FBakeAmbientOcclusionParams
    {
        // Maximum ray length, in the mesh's OWN LOCAL UNITS - the space the vertices are in, not
        // world space, so an actor scale does not enter into it. No default: it is the parameter
        // that decides whether a part junction occludes while the far side of the same mesh does
        // not, it is a length so it cannot be scale-free, and an unbounded ray produces a global
        // bent-normal darkening that says nothing about local contact.
        double OcclusionRadius = 0.0;

        // Rays per corner. The estimator is a visible-fraction average, so this trades noise for
        // time linearly; the cost is Samples x colour-element count ray casts.
        int32 Samples = 64;

        // Degrees. Rays arriving within this angle of the surface's own tangent plane have their
        // weight rolled off, which is what stops a faceted surface reading its neighbouring facet
        // as an occluder and drawing acne along every hard edge. The engine evaluator's
        // BiasAngleDeg, and its default.
        double BiasAngleDegrees = 15.0;

        // Where the result goes. None is refused rather than defaulted: RGB and A each already
        // carry a signal on a real mesh, so there is no channel this can pick without possibly
        // destroying something the caller wanted.
        EColorChannels Channels = EColorChannels::None;

        // False replaces the masked channels with the bake; true multiplies it into whatever
        // they already carry, which is how a bake is layered onto an existing tint.
        bool bMultiply = false;

        // Lerps the bake toward "fully exposed": 1 writes the measurement, 0 writes white and
        // therefore darkens nothing. Clamped to [0,1] with a warning.
        double Strength = 1.0;
    };

    // Min/mean/max over a set of values, plus how many there were. Count 0 means nothing was
    // measured, which is a different statement from "measured and came out 0".
    struct FValueStatistics
    {
        double Min = 0.0;
        double Mean = 0.0;
        double Max = 0.0;
        int32 Count = 0;
    };

    struct FBakeAmbientOcclusionOutputs
    {
        int32 VerticesModified = 0;
        int32 ElementsWritten = 0;
        int32 ElementsCreated = 0;

        // Live colour elements the engine baker could not reach because its result image is
        // sized by live element COUNT while it is indexed by element ID - so on an overlay with
        // ID gaps the elements above the count have no pixel. Non-zero raises a warning; those
        // elements keep the colour they had.
        int32 ElementsUnreachable = 0;

        // Colour elements no triangle referenced, freed before the bake. They are not merely
        // useless: FMeshVertexBaker::SampleSurface indexes the element's triangle list at [0]
        // with no empty guard, so one of these terminates the editor rather than being skipped.
        int32 OrphanElementsFreed = 0;

        // Triangles the primary normal overlay did not cover, given per-vertex normals before
        // the bake. Also not cosmetic - see the comment at the call site. Non-zero raises a
        // warning, because it is a mesh edit the caller did not ask for.
        int32 TrianglesGivenNormals = 0;

        // The raw occlusion term, before Strength and before bMultiply: 1 is fully exposed, 0 is
        // fully enclosed. This is the flatness detector - a bake that produced nothing reads
        // min == mean == max == 1 here whatever the written channels end up looking like, which
        // a per-channel reading of a multiply blend cannot tell you.
        FValueStatistics Occlusion;

        // Per r/g/b/a, over the values actually stored. Count stays 0 for a channel the mask did
        // not name.
        FValueStatistics Written[4];
    };

    // MESH_EMPTY on a mesh with no triangles (there is nothing to occlude and nothing to
    // measure), INVALID_ARGUMENT on an empty channel mask, a non-positive OcclusionRadius or
    // Samples below 1, BAKE_FAILED when the engine baker returns no result. Never a bare success
    // that measured nothing.
    FOpResult BakeAmbientOcclusion(UDynamicMesh* Mesh, const FBakeAmbientOcclusionParams& Params,
        FBakeAmbientOcclusionOutputs& Outputs);

    // --- set_uvs ------------------------------------------------------------------------
    struct FSetUVsParams
    {
        int32 VertexIndex = INDEX_NONE;
        FVector2D UV = FVector2D::ZeroVector;
        int32 UVChannel = 0;
    };

    // Grows the UV layer set so UVChannel exists, then writes UV into every overlay element
    // parented to VertexIndex. UV_LAYER_ERROR for a negative UVChannel, which is the only
    // channel FDynamicMeshAttributeSet::GetUVLayer refuses - unlike the geometry-script
    // SetNumUVSets wrapper, the attribute set itself has no 8-layer ceiling, so a large
    // channel is grown rather than rejected. INVALID_VERTEX for a dead vertex ID.
    // NO_UV_ELEMENTS when the vertex is live but carries no UV element on that channel,
    // which is what a freshly grown layer always looks like.
    FOpResult SetUVs(UDynamicMesh* Mesh, const FSetUVsParams& Params, int32& OutElementsModified);

    // --- translate_mesh -----------------------------------------------------------------
    struct FTranslateMeshParams
    {
        FVector Translation = FVector::ZeroVector;
    };

    // Bakes Translation into every vertex position. This is a MESH edit, not an actor
    // transform - the actor does not move, so it composes with the spawn-transform
    // convention in GeometryTarget.h rather than fighting it.
    FOpResult TranslateMesh(UDynamicMesh* Mesh, const FTranslateMeshParams& Params);
}
