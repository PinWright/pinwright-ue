// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryOps_Modeling.h - the modeling, deformer, repair and UV operations of
// MeshOpsHandler.cpp as pure functions over a UDynamicMesh.
//
// Nothing here touches FHandlerContext, an AActor, or a dirty flag: an operation takes a mesh
// plus its F<Verb>Params struct and returns an FOpResult. That is what lets the .pwmodel
// compiler run the same code on a transient mesh with no level, no actor and no RPC request,
// and it is why the params structs are named for the geometry rather than for the JSON keys the
// RPC surface published (the wrapper translates its own legacy names onto these fields, so the
// published contract is unchanged while there is still exactly one parameter vocabulary).
//
// Ops that report a number the FOpResult counts cannot carry - a clamped iteration count, the
// number of holes actually filled - take a trailing F<Verb>Outcome* which may be null. That is
// deliberately not folded into FOpResult: FOpResult is shared by every op family and must not
// grow a field per verb.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/Geometry/GeometryOps.h"

class UDynamicMesh;

namespace GeometryOps
{
    // ------------------------------------------------------------------------
    // Shared vocabulary
    //
    // EMeshAxis is GeometryOps.h's - stretch and cylindrify here, mirror and array_radial in the
    // boolean family, all name the same three axes, so one enum serves them.
    // ------------------------------------------------------------------------

    // There is no spherical projection in Geometry Script; the three modes below are the
    // complete set SetMeshUVsFrom*Projection offers.
    enum class EUVProjectionMode : uint8
    {
        Box,
        Planar,
        Cylindrical
    };

    // ------------------------------------------------------------------------
    // Engine option enums, spelled locally.
    //
    // Every enum below mirrors a Geometry Script one value-for-value and in the same order, for
    // the reason EMeshAxis and ENoiseNormalSource already are: this header must stay free of
    // GeometryScripting includes so the .pwmodel front-end can compile against it without
    // pulling the plugin in. The .cpp does the one-line translation, and each translation is a
    // switch with no default so a value added upstream fails to compile rather than silently
    // mapping to the first enumerator.
    // ------------------------------------------------------------------------

    // EGeometryScriptPolyOperationArea - whether a face op treats the selection as one region,
    // one region per polygroup, or one per triangle. The engine's per-region split is what makes
    // `extrude` on a selection of several disjoint faces push each face along its own normal
    // instead of moving the whole set as one slab.
    enum class EPolyOperationArea : uint8
    {
        EntireSelection,
        PerPolygroup,
        PerTriangle
    };

    // EGeometryScriptMeshEditPolygroupMode.
    enum class EEditPolygroupMode : uint8
    {
        PreserveExisting,
        AutoGenerateNew,
        SetConstant
    };

    // FGeometryScriptMeshEditPolygroupOptions, FLATTENED rather than nested.
    //
    // FPwModelValue carries a number, a tuple, a tuple list, a string or an identifier and has
    // no map or sub-object member (PwModelAst.h), so a nested `group { … }` value is not
    // expressible in the format today. The four face ops that carry this therefore publish two
    // prefixed parameters - `group_mode` and `group_id` - and this struct is the single place
    // the pair is defined so they cannot drift apart across extrude / inset / outset /
    // offset_faces.
    struct FPolygroupEditSpec
    {
        EEditPolygroupMode GroupMode = EEditPolygroupMode::PreserveExisting;

        // Read only when GroupMode == SetConstant.
        int32 ConstantGroup = 0;
    };

    // EGeometryScriptOffsetFacesType - how the offset direction per vertex is derived.
    // ParallelFaceOffset keeps each face parallel to where it started, which is why it is the
    // engine's default and the only one that leaves a box a box.
    enum class EOffsetFacesType : uint8
    {
        VertexNormal,
        FaceNormal,
        ParallelFaceOffset
    };

    // EGeometryScriptLinearExtrudeDirection.
    enum class ELinearExtrudeDirection : uint8
    {
        FixedDirection,
        AverageFaceNormal
    };

    // EGeometryScriptFillHolesMethod.
    enum class EFillHolesMethod : uint8
    {
        Automatic,
        MinimalFill,
        PolygonTriangulation,
        TriangleFan,
        PlanarProjection
    };

    // EGeometryScriptRepairMeshMode - what RepairMeshDegenerateGeometry does with a triangle it
    // finds degenerate.
    enum class ERepairMeshMode : uint8
    {
        DeleteOnly,
        RepairOrDelete,
        RepairOrSkip
    };

    // EGeometryScriptTangentTypes. StandardMikkT falls back to FastMikkT where the standard
    // implementation is unavailable; on Win64 it is always available.
    enum class ETangentType : uint8
    {
        FastMikkT,
        PerTriangle,
        StandardMikkT
    };

    // EGeometryScriptFlareType - the displacement profile the taper/flare deformer sweeps.
    enum class EFlareType : uint8
    {
        SinMode,
        SinSquaredMode,
        TriangleMode
    };

    // Which faces a face op (extrude / inset / outset / offset_faces) runs on.
    //
    // bHasDirection == false leaves the engine selection empty, which Geometry Script reads as
    // "the whole mesh" - the pre-existing behaviour of every face verb called without a
    // faceDirection. This struct replaces the BuildFaceSelection helper that read the request
    // payload mid-operation: an op needs the selection RULE, not the JSON it arrived in.
    //
    // An explicit CreateSelectAllMeshSelection(Triangles) would NOT be equivalent-but-tidier: it
    // produces the identical all-triangles array the empty-selection path already runs, so the
    // only capability worth expressing here is the directional SUBSET.
    //
    // bHasDirection == true and NOTHING within AngleTolerance of Direction is a THIRD case, not
    // a variant of the first: the op does nothing at all and warns. The empty-selection ==
    // whole-mesh convention answers "no filter given"; reading it as the answer to "filter given,
    // matched nothing" is what let a mistyped direction operate on the entire mesh silently. See
    // GeometryOpsModeling_BuildSelection in the .cpp for the full rationale and for why this is a
    // warn-and-no-op rather than an error.
    struct FFaceSelectionSpec
    {
        bool bHasDirection = false;
        FVector Direction = FVector::UpVector;

        // Max normal-angle deviation in degrees for the directional selection.
        double AngleTolerance = 45.0;
    };

    // What the face op's selection actually resolved to.
    struct FFaceOpOutcome
    {
        // Number of triangles the face op ran on. 0 is AMBIGUOUS on its own and needs the flag
        // below to read: it is either the whole mesh (no direction given - the empty selection
        // Geometry Script expands to every triangle, which is what
        // FGeometryScriptMeshSelection::GetNumSelected reports as 0) or nothing at all (a
        // direction was given and matched no face, so the op did not run).
        int32 FacesSelected = 0;

        // True only for the second of those: an explicit face direction matched zero faces and
        // the op was skipped. Additive - it is false on every pre-existing path, including the
        // whole-mesh one, so `FacesSelected == 0 && !bFilterMatchedNothing` still means exactly
        // what the single field used to mean. The RPC wrappers echo it as
        // `faceFilterMatchedNothing` and only when true (MeshOpsHandler.cpp's ReportMeshChange).
        bool bFilterMatchedNothing = false;
    };

    // The three options every face-modeling option struct carries in common -
    // FGeometryScriptMeshLinearExtrudeOptions, ...MeshInsetOutsetFacesOptions and
    // ...MeshOffsetFacesOptions all spell AreaMode, GroupOptions and UVScale identically. One
    // struct so extrude / inset / outset / offset_faces / poke cannot end up with three
    // different spellings and three different defaults for the same engine field, and so both
    // front-ends read them through one helper each.
    //
    // Defaults are the engine's, so a params struct left alone reproduces today's calls exactly.
    struct FFaceOpCommonSpec
    {
        EPolyOperationArea AreaMode = EPolyOperationArea::EntireSelection;
        FPolygroupEditSpec Groups;

        // Scale applied to the UVs generated on the NEW faces the op introduces. Does not touch
        // the UVs already on the mesh.
        double UVScale = 1.0;
    };

    // ------------------------------------------------------------------------
    // Normals and tangents
    // ------------------------------------------------------------------------

    struct FRecalculateNormalsParams
    {
        bool bAreaWeighted = true;
        bool bAngleWeighted = true;
    };
    FOpResult RecalculateNormals(UDynamicMesh* Mesh, const FRecalculateNormalsParams& Params);

    struct FFlipNormalsParams
    {
    };
    FOpResult FlipNormals(UDynamicMesh* Mesh, const FFlipNormalsParams& Params);

    struct FRecomputeTangentsParams
    {
        // FGeometryScriptTangentsOptions::Type. FastMikkT is the engine default.
        ETangentType Type = ETangentType::FastMikkT;

        // Which UV layer the tangent basis is built from. Tangents are only meaningful relative
        // to a UV parameterisation, so a mesh whose normal map is authored against channel 1
        // needs this pointed at 1 or the tangent basis is built from the wrong unwrap and the
        // bake reads inverted along one axis.
        int32 UVLayer = 0;
    };
    FOpResult RecomputeTangents(UDynamicMesh* Mesh, const FRecomputeTangentsParams& Params);

    struct FSplitNormalsParams
    {
        // FGeometryScriptSplitNormalsOptions::OpeningAngleDeg. NOT the engine's 15.0: this
        // surface has published 60 since it shipped, and lowering it to the engine's value
        // would put a hard edge on every mesh that currently comes out smooth.
        double SplitAngle = 60.0;

        // Split where the dihedral angle exceeds SplitAngle. With both split predicates off the
        // op still runs and simply re-averages the normals it already has.
        bool bSplitByOpeningAngle = true;

        // Split along polygroup boundaries as well. Independent of the angle test - the engine
        // ORs them.
        bool bSplitByFaceGroup = false;

        // FGeometryScriptGroupLayer, flattened for the reason FPolygroupEditSpec is. Read only
        // when bSplitByFaceGroup.
        bool bUseDefaultGroupLayer = true;
        int32 ExtendedGroupLayerIndex = 0;
    };
    FOpResult SplitNormals(UDynamicMesh* Mesh, const FSplitNormalsParams& Params);

    // ------------------------------------------------------------------------
    // Topology budget
    // ------------------------------------------------------------------------

    // Mirrors EGeometryScriptRemoveMeshSimplificationType one-for-one, spelled locally for the
    // same reason EMeshAxis is: this header stays free of GeometryScripting includes so the
    // .pwmodel front-end can name a method without pulling the engine plugin's headers in.
    //
    // The DisplayName the editor shows for AttributeAware is "Normals Aware, Volume Preserving"
    // and AttributeAwareV2's is "Attribute Aware, Volume Preserving" - the enumerator names and
    // the UI labels are one step out of phase, which is a renaming UE 5.8 did in place. The
    // enumerator name is what both front-ends publish, because it is what the engine header says.
    enum class ESimplifyMethod : uint8
    {
        // Classic quadric error metric, no volume preservation.
        StandardQEM,
        // Classic QEM with volume preservation.
        VolumePreserving,
        // Volume preserving, and accounts for vertex normals. The engine default.
        AttributeAware,
        // Volume preserving, and accounts for normals/tangents/bitangents/color/UVs with seams.
        AttributeAwareV2
    };

    // Mirrors EGeometryScriptMeshSimplificationQuadricVariant.
    enum class ESimplifyQuadricVariant : uint8
    {
        PlaneQuadric,
        TriangleQuadric
    };

    struct FSimplifyMeshParams
    {
        // Percentage of the CURRENT triangle count to keep. The op never simplifies below one
        // triangle, matching the FMath::Max(1, ...) the handler has always applied.
        double TargetPercentage = 50.0;

        // Every field below mirrors FGeometryScriptSimplifyMeshOptions one-for-one and carries
        // the ENGINE's default, so a caller that sets none reaches the engine call with a
        // default-constructed options struct.
        //
        // Method is the one exception, and it is a deliberate correction rather than a
        // divergence: this op used to force StandardQEM over the engine's AttributeAware with
        // no comment, no doc and no test naming a reason, which silently discarded the normals
        // the attribute-aware quadric preserves. See the definition for the full note.
        ESimplifyMethod Method = ESimplifyMethod::AttributeAware;

        bool bAllowSeamCollapse = true;
        bool bAllowSeamSmoothing = true;
        bool bAllowSeamSplits = true;
        bool bPreserveVertexPositions = false;
        bool bRetainQuadricMemory = false;

        // Small non-zero values (the engine ships 1e-6) improve triangle quality in flat
        // regions. Named for the engine field rather than shortened, because it is the only
        // weight here that is not an attribute weight.
        double RegularizeWeight = 0.000001;

        bool bAutoCompact = true;

        ESimplifyQuadricVariant QuadricVariant = ESimplifyQuadricVariant::PlaneQuadric;

        // The four attribute weights are read only by the attribute-aware methods; the two
        // plain-QEM methods ignore them entirely.
        double NormalAttributeWeight = 16.0;
        double TangentAttributeWeight = 0.1;
        double ColorAttributeWeight = 0.1;
        double TexCoordAttributeWeight = 0.5;

        // Rebalances geometry error against attribute error for an object whose scale differs
        // from the one the weights above were calibrated at. Attribute-aware methods only.
        double ScaleCorrection = 1.0;

        // NOT carried, and not an oversight: FGeometryScriptSimplifyMeshOptions's three
        // FGeometryScriptWeightMapDensity fields (EdgeLengthWeightMap,
        // GeometricToleranceWeightMap, QuadricErrorWeightMap) each hold an
        // FGeometryScriptWeightMapHandle, which names a weight map that must already exist on
        // the mesh. Neither front-end can produce one: the RPC surface has no weight-map verb,
        // and .pwmodel's only identifier->resource binding is the material slot table
        // (PwModelAst.h). Their sibling RelativeDensity is meaningless without a handle - the
        // engine scales BY the map - so exposing the float alone would publish a parameter that
        // provably does nothing.
    };
    FOpResult SimplifyMesh(UDynamicMesh* Mesh, const FSimplifyMeshParams& Params);

    struct FSubdivideOutcome
    {
        // Iterations actually run, after the GEOM_MAX_SUBDIVIDE_ITERATIONS clamp. The RPC
        // response echoes this rather than the requested value, so the wrapper needs it back.
        int32 EffectiveIterations = 0;
    };

    struct FSubdivideParams
    {
        int32 Iterations = 1;

        // FGeometryScriptPNTessellateOptions' only field. PN tessellation places the new
        // vertices on a curved patch, so the normals it recomputes are what makes the result
        // read as smooth; off keeps the faceted normals the coarse mesh had, which is what a
        // caller who is about to run `split_normals` or bake their own wants.
        bool bRecomputeNormals = true;
    };
    // Fails MEMORY_PRESSURE under memory pressure and POLYGON_LIMIT_EXCEEDED when the 4x-per-
    // iteration estimate would blow GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH. Both guards live here
    // rather than in the wrapper so the .pwmodel compiler cannot bypass them.
    FOpResult Subdivide(UDynamicMesh* Mesh, const FSubdivideParams& Params,
                        FSubdivideOutcome* Outcome = nullptr);

    // Mirrors EGeometryScriptUniformRemeshTargetType.
    enum class ERemeshTargetType : uint8
    {
        // TargetTriangleCount is a budget the remesher converts to an edge length. Approximate.
        TriangleCount,
        // TargetEdgeLength is the goal directly, and the triangle count falls out of it.
        TargetEdgeLength
    };

    // Mirrors EGeometryScriptRemeshSmoothingType.
    enum class ERemeshSmoothingType : uint8
    {
        // Most regular output, UVs ignored.
        Uniform,
        // Centroids projected onto the vertex normal, preserving UVs.
        UVPreserving,
        // UV-preserving with some tangential drift where a vertex would otherwise stick.
        Mixed
    };

    // Mirrors EGeometryScriptRemeshEdgeConstraintType. Applied per attribute boundary.
    enum class ERemeshEdgeConstraint : uint8
    {
        // Not flippable, splittable or collapsible; the vertices do not move.
        Fixed,
        // Splittable only; the vertices do not move.
        Refine,
        // Not flippable, but otherwise free to move.
        Free,
        // Unconstrained: the attribute is not considered at all.
        Ignore
    };

    struct FRemeshUniformParams
    {
        // ApplyUniformRemesh targets an EDGE LENGTH derived from this budget, so the achieved
        // triangle count can diverge substantially. FOpResult::TrianglesAfter is the achieved
        // figure. Read only when TargetType == TriangleCount.
        int32 TargetTriangleCount = 5000;

        // The remaining fields mirror FGeometryScriptRemeshOptions and
        // FGeometryScriptUniformRemeshOptions one-for-one and every default here is the
        // engine's, so a caller that sets none reaches ApplyUniformRemesh with two
        // default-constructed options structs.
        ERemeshTargetType TargetType = ERemeshTargetType::TriangleCount;

        // Read only when TargetType == TargetEdgeLength, and then it is the goal in Unreal
        // units. The engine's 1.0 is a centimetre, which is a very dense target for anything
        // built at the scale the primitives default to.
        double TargetEdgeLength = 1.0;

        // Discards ALL mesh attributes first, so UV and normal seams stop constraining the
        // remesh and per-vertex normals are recomputed. The cheapest way to get a regular mesh
        // and the fastest way to lose a UV layout.
        bool bDiscardAttributes = false;

        // Projects vertices back onto the input surface as the remesh runs, preserving shape.
        bool bReprojectToInputMesh = true;

        ERemeshSmoothingType SmoothingType = ERemeshSmoothingType::Mixed;

        // 0 disables smoothing entirely; the engine clamps the field to [0, 1].
        double SmoothingRate = 0.25;

        ERemeshEdgeConstraint MeshBoundaryConstraint = ERemeshEdgeConstraint::Free;
        ERemeshEdgeConstraint GroupBoundaryConstraint = ERemeshEdgeConstraint::Free;
        ERemeshEdgeConstraint MaterialBoundaryConstraint = ERemeshEdgeConstraint::Free;

        // Disabling flips markedly lowers output quality; disabling splits pins the density
        // from rising and disabling collapses pins it from falling, so clearing both makes the
        // triangle budget unreachable by construction.
        bool bAllowFlips = true;
        bool bAllowSplits = true;
        bool bAllowCollapses = true;

        bool bPreventNormalFlips = true;
        bool bPreventTinyTriangles = true;

        // The expensive full-pass strategy instead of the default edge queue: higher quality,
        // and it multiplies the cost of RemeshIterations.
        bool bUseFullRemeshPasses = false;

        int32 RemeshIterations = 20;

        bool bAutoCompact = true;
    };
    FOpResult RemeshUniform(UDynamicMesh* Mesh, const FRemeshUniformParams& Params);

    struct FPokeParams
    {
        double Offset = 0.0;

        // poke is offset_faces followed by ONE PN tessellation, so it carries both option
        // structs. These are the offset half.
        EOffsetFacesType OffsetType = EOffsetFacesType::ParallelFaceOffset;
        FFaceOpCommonSpec Common;
        bool bSolidsToShells = true;

        // ...and this is the tessellation half (FGeometryScriptPNTessellateOptions).
        bool bRecomputeNormals = true;
    };
    // Same two guards as Subdivide (offset_faces + one PN tessellation is a 4x explosion).
    FOpResult Poke(UDynamicMesh* Mesh, const FPokeParams& Params);

    // ------------------------------------------------------------------------
    // Face modeling
    // ------------------------------------------------------------------------

    struct FExtrudeParams
    {
        double Distance = 10.0;
        FVector Direction = FVector(0, 0, 1);
        FFaceSelectionSpec Faces;

        // FGeometryScriptMeshLinearExtrudeOptions::DirectionMode. FixedDirection is BOTH the
        // engine default and the value this op used to hardcode, so publishing it is a pure
        // widening: an omitted parameter still pins Direction. AverageFaceNormal ignores
        // Direction entirely and pushes each region along its own averaged normal, which is
        // what "extrude these faces outward" means on a curved surface and was unreachable.
        ELinearExtrudeDirection DirectionMode = ELinearExtrudeDirection::FixedDirection;

        FFaceOpCommonSpec Common;

        // Convert a closed solid into a shell when the extrusion would otherwise fold it
        // inside out. Engine default.
        bool bSolidsToShells = true;
    };
    FOpResult Extrude(UDynamicMesh* Mesh, const FExtrudeParams& Params,
                      FFaceOpOutcome* Outcome = nullptr);

    struct FInsetParams
    {
        // Positive shrinks the face inward. FInsetMeshRegion forces the inset direction toward
        // the region centroid and moves the boundary to Midpoint + Distance*InsetDir, and
        // reprojection is only honoured for the positive case - which is why inset and outset
        // are separate ops over one engine call rather than one op with a signed distance.
        double Distance = 5.0;
        FFaceSelectionSpec Faces;

        // Reproject the inset ring back onto the original surface. The op used to hardcode
        // true, which is ALSO the engine default, so this is a widening with no behaviour
        // change at the default - off leaves the ring on the plane of the region rather than
        // following curvature, which is what an inset on a sphere usually wants.
        bool bReproject = true;

        // Inset only the boundary loop of the region rather than every face in it.
        bool bBoundaryOnly = false;

        // Smooths the inset ring toward its neighbours; 0 is a hard inset.
        double Softness = 0.0;

        // Scales the inset distance by the region's area, so large and small faces inset
        // proportionally rather than by the same absolute amount.
        double AreaScale = 1.0;

        FFaceOpCommonSpec Common;
    };
    FOpResult Inset(UDynamicMesh* Mesh, const FInsetParams& Params,
                    FFaceOpOutcome* Outcome = nullptr);

    struct FOutsetParams
    {
        // Positive grows the footprint outward; the op negates it for the engine.
        double Distance = 5.0;
        FFaceSelectionSpec Faces;

        // NO bReproject here, deliberately, and it is the one field of the shared engine
        // struct outset does not publish. FInsetMeshRegion honours reprojection only for a
        // POSITIVE distance, and outset always passes a negative one, so a published
        // `reproject` on this op would be a knob that provably reaches the engine and provably
        // changes nothing - the exact defect this pass exists to remove. inset publishes it.
        bool bBoundaryOnly = false;
        double Softness = 0.0;
        double AreaScale = 1.0;

        FFaceOpCommonSpec Common;
    };
    FOpResult Outset(UDynamicMesh* Mesh, const FOutsetParams& Params,
                     FFaceOpOutcome* Outcome = nullptr);

    struct FOffsetFacesParams
    {
        double Distance = 5.0;
        FFaceSelectionSpec Faces;

        // How the per-vertex offset direction is derived. ParallelFaceOffset (the engine
        // default) moves each face along its own normal by exactly Distance, so a box stays a
        // box; VertexNormal averages at the corners and rounds them off instead.
        EOffsetFacesType OffsetType = EOffsetFacesType::ParallelFaceOffset;

        FFaceOpCommonSpec Common;
        bool bSolidsToShells = true;
    };
    FOpResult OffsetFaces(UDynamicMesh* Mesh, const FOffsetFacesParams& Params,
                          FFaceOpOutcome* Outcome = nullptr);

    struct FBevelParams
    {
        double Distance = 5.0;
        int32 Subdivisions = 0;

        // Roundness across the bevel face. IGNORED when Subdivisions == 0, because there is no
        // interior loop to place - which is why the pair only becomes useful together.
        double RoundWeight = 1.0;

        // Take the new faces' material from the two faces either side of the bevelled edge
        // when they agree; SetMaterialID is used when they do not, or when this is false.
        bool bInferMaterialID = false;
        int32 SetMaterialID = 0;

        // Restrict the bevel to polygroup edges touching this box, in MESH space. This is the
        // only edge filter the engine offers, and it is the answer to the "bevel chamfers
        // EVERY polygroup edge" trap the warnings below describe: a filter box around the
        // silhouette bevels the silhouette and leaves the interior quad boundaries alone.
        //
        // FGeometryScriptMeshBevelOptions::FilterBoxTransform is deliberately NOT carried. The
        // box is already expressed in the mesh's own space, so a transform on top of it adds
        // no shape a caller cannot spell by moving FilterBoxMin/Max, and it would need four
        // more published parameters to express.
        bool bApplyFilterBox = false;
        FVector FilterBoxMin = FVector::ZeroVector;
        FVector FilterBoxMax = FVector::ZeroVector;

        // True bevels only edges FULLY inside the box; false bevels any edge with a vertex in
        // it. Engine default is true.
        bool bFullyContained = true;

        // Group IDs allocated by a previous bevel on this mesh. The .pwmodel compiler supplies
        // this history so the preflight can leave a second bevel off the first bevel's strips and
        // corner patches; the geometry.* RPC leaves it empty because it has no document history.
        TSet<int32> PriorBevelGroupIDs;
    };
    // Bevels POLYGROUP EDGES only, and it bevels ALL of them. Both ends of that are traps and
    // both warn:
    //   - No polygroups: returned unchanged. The single most confusing silent no-op here.
    //   - One polygroup per QUAD: every interior quad boundary is chamfered, which reads as
    //     surface damage rather than as a chamfer and costs roughly 3x the triangles. This is
    //     what torus / arch / revolve hand you - the revolve generators ignore PolygroupMode and
    //     group per quad - so `torus` + `bevel` is the shape that produces it.
    // Fails UNSUPPORTED_ENGINE_VERSION on UE < 5.4 when Subdivisions > 0.
    FOpResult Bevel(UDynamicMesh* Mesh, const FBevelParams& Params);

    struct FShellParams
    {
        // Wall thickness; the op offsets inward by this amount.
        double Thickness = 5.0;

        // FGeometryScriptMeshOffsetOptions' remaining five fields. ApplyMeshShell does not
        // translate the surface rigidly - it runs an iterative solve, and these are its knobs,
        // so a shell that self-intersects on a concave mesh is usually fixed here rather than
        // by changing Thickness.

        // Pin the boundary loops in place instead of offsetting them.
        bool bFixedBoundary = false;

        // Iterations of the offset solve. More is slower and closer to a true uniform offset.
        int32 SolveSteps = 5;

        // Smoothing weight per solve step.
        double SmoothAlpha = 0.1;

        // Re-project onto the source surface between smoothing steps.
        bool bReprojectDuringSmoothing = false;

        // Smoothing weight at the boundary. The engine's own comment reads "should not be
        // > 0.9", which is the ceiling both front-ends publish.
        double BoundaryAlpha = 0.2;
    };
    // Fails NO_UV_ELEMENTS on an OPEN mesh whose UV channel 0 does not cover EVERY triangle
    // (absent, element-less, or set on only some triangles). That is a crash guard, not a
    // preference: the engine's boundary stitcher dereferences the primary UV overlay unchecked
    // and reads a specific triangle's elements without an IsSetTriangle test, taking the editor
    // down with an access violation. An EMPTY mesh succeeds with a warning and bChanged=false -
    // IsClosed() is false on an empty mesh, so it has to be excluded before the UV test or it
    // would be told to add UVs to a mesh with no triangles. See the comment on the definition
    // for the callstack, the reproducing document and why the closed case is exempt.
    FOpResult Shell(UDynamicMesh* Mesh, const FShellParams& Params);

    // ------------------------------------------------------------------------
    // Warp deformers
    //
    // Extent on all three warps is a SYMMETRIC HALF-EXTENT in world units, measured along
    // Frame.Axis about Frame.Center and spanning [-Extent, +Extent] (all three set
    // bSymmetricExtents). A mesh of height H is therefore only fully covered at Extent ~= H/2; a
    // caller who passes H gets a warp that saturates well outside the mesh and looks like it did
    // nothing.
    //
    // Frame.Axis and Frame.Center default to Z and the origin, which is the FTransform::Identity
    // all three used to hardcode - so an existing call is unchanged - but they are what makes a
    // warp reachable on geometry that is not Z-aligned and origin-centred. See FWarpFrameSpec.
    // ------------------------------------------------------------------------

    // The two extent fields all three warps share, and the reason the header comment above
    // says "symmetric". bSymmetricExtents was hardcoded true on all three - which is also the
    // engine default, so publishing it changes nothing at the default - and LowerExtent was
    // therefore dead. Together they are what turns the warp from [-Extent, +Extent] into the
    // ASYMMETRIC [-LowerExtent, +Extent] a mesh that does not straddle the origin needs.
    struct FWarpExtentSpec
    {
        bool bSymmetricExtents = true;

        // Read only when bSymmetricExtents is false. Engine default.
        double LowerExtent = 10.0;
    };

    // WHERE the warp is measured from, and along WHICH axis - the second half of the extent
    // story, and the half all three warps used to hardcode.
    //
    // Every one of these ops drives an FMeshSpaceDeformerOp, which measures its extent along the
    // GIZMO FRAME's Z (`GizmoPos4[2]`, TwistMeshOp.cpp:83 and its two siblings) and centres that
    // extent on the frame's ORIGIN. The three ops passed FTransform::Identity for that frame, so
    // the only geometry they could warp was geometry already aligned to part-local +Z and already
    // straddling the part-local origin. A horizontal beam could not be tapered along its own
    // length at all, and a limb placed away from the origin got a warp centred somewhere off in
    // space - neither of which is expressible by any combination of extent and lowerExtent,
    // because both of those are measured ALONG the axis this spec chooses.
    //
    // The parameters and their meaning are harmonic_deform's, deliberately: it is the sibling
    // deformer, it already publishes exactly these two under exactly these names, and a family
    // where one member spells its axis `axis` and another spells it something else is a family an
    // author has to learn twice. `Center` is the axis LINE, not a bounding-box centre, for the
    // reason FHarmonicDeformParams::Center gives - a box-derived centre moves whenever an earlier
    // op changes the mesh's extent, so the same document would deform differently depending on
    // what ran before it.
    //
    // Defaults are behaviour-preserving BY ARITHMETIC, not by a special case: Axis=Z with
    // Center=(0,0,0) builds the identity basis and the zero translation, which is
    // FTransform::Identity, which is what the three ops passed before this spec existed.
    struct FWarpFrameSpec
    {
        // The axis the extent is measured along, and the axis a twist twists about.
        EMeshAxis Axis = EMeshAxis::Z;

        // The point the extent is centred on, in the same part-local space the mesh is in.
        FVector Center = FVector::ZeroVector;
    };

    struct FBendParams
    {
        double Angle = 45.0;
        double Extent = 50.0;
        FWarpExtentSpec Extents;
        FWarpFrameSpec Frame;

        // False makes the bend start at the lower extent and leave everything below it
        // untouched, rather than rigidly transforming both regions about the origin. Engine
        // default is true, which is what the op hardcoded.
        bool bBidirectional = true;
    };
    FOpResult Bend(UDynamicMesh* Mesh, const FBendParams& Params);

    struct FTwistParams
    {
        double Angle = 45.0;
        double Extent = 50.0;
        FWarpExtentSpec Extents;
        FWarpFrameSpec Frame;
        bool bBidirectional = true;
    };
    FOpResult Twist(UDynamicMesh* Mesh, const FTwistParams& Params);

    struct FTaperParams
    {
        // FlareX and FlareY are per PERPENDICULAR axis of the warp frame, taken cyclically from
        // Frame.Axis: X is (Axis + 1) % 3 and Y is (Axis + 2) % 3, the same handedness
        // harmonic_deform measures its azimuth with. At the default Axis=Z they are world X and
        // world Y, which is what they have always been.
        double FlareX = 50.0;
        double FlareY = 50.0;
        double Extent = 50.0;
        FWarpExtentSpec Extents;
        FWarpFrameSpec Frame;

        // The displacement profile swept over the extent. SinMode is the engine default;
        // SinSquaredMode is the one to reach for when the flare has to blend into unwarped
        // geometry, because its normal derivative is continuous at both ends and SinMode's is
        // not - a visible crease at the extent boundary is this parameter, not the mesh.
        EFlareType FlareType = EFlareType::SinMode;
    };
    FOpResult Taper(UDynamicMesh* Mesh, const FTaperParams& Params);

    // Which per-vertex normals a normal-aligned noise displacement rides. Mirrors
    // EGeometryScriptPerVertexNormalSource one-for-one; spelled locally for the same reason
    // EMeshAxis is, so this header stays free of GeometryScripting includes.
    enum class ENoiseNormalSource : uint8
    {
        // Recompute normals from the geometry.
        Computed,
        // Average the existing normal-overlay elements instead.
        AverageFromOverlay
    };

    // Perlin noise is a SPATIAL field, not a per-call random draw: the displacement of a vertex
    // is a pure function of its position, Seed and FrequencyShift. Two ops with the same Seed
    // therefore agree wherever they overlap in space and differ wherever they do not - which is
    // why parts placed at different `at=` already deform differently on one seed, and why two
    // whole MODELS built from the same recipe cannot differ at all until Seed does. That second
    // case is the one a seeded variant family needs.
    //
    // Seed and FrequencyShift are two ways to move through the same field. Seed jumps to an
    // unrelated region and is the right knob for "another variant"; FrequencyShift slides the
    // sampling window continuously and is the right knob for sweeping or animating one.
    // What Magnitude MEASURES. Not a second noise field and not a second knob to tune against
    // the first: the same field, read with a different ruler.
    enum class ENoiseMagnitudeMode : uint8
    {
        // Magnitude is a DISPLACEMENT IN WORLD UNITS, identical everywhere on the mesh. The
        // engine's own reading, and the default, so an existing call is unchanged.
        //
        // Its limit is a mesh whose feature sizes differ. One pass sized for a thick form erases a
        // thin one, and one pass sized for the thin form leaves the thick one visibly smooth -
        // because a displacement that reads as surface detail on a 40-unit form is most of the
        // radius of a 2-unit one. The only workaround was to split the mesh into one part per
        // feature size and noise each separately, which costs a part and an anchor primitive per
        // amplitude.
        Absolute,

        // Magnitude is a FRACTION OF THE VERTEX'S OWN MEAN ONE-RING EDGE LENGTH, so the
        // displacement follows local feature size and one pass can cover a whole mixed-scale
        // model. Magnitude 0.5 displaces every vertex by half the average length of the edges
        // meeting at it, whatever that length happens to be there.
        //
        // Mean one-ring edge length is the mean length of the edges INCIDENT TO THE VERTEX -
        // GetVtxEdgeCount edges, each measured to its far end - and it is a proxy for local
        // feature size rather than a measurement of it. That proxy holds while the tessellation
        // tracks the form (a revolve or a sweep gives fine edges on a thin section and coarse
        // ones on a thick section, which is the normal case); it does NOT hold on a mesh that has
        // been uniformly remeshed to one target edge length (what remesh_uniform produces), where
        // every vertex reports the same number and relative degenerates into absolute with a
        // rescaled Magnitude. Measure the mesh's edge-length spread before assuming, or pick
        // Absolute and split the mesh.
        //
        // Two vertex shapes need saying because neither is a special case in the code:
        //
        //   - A BOUNDARY vertex's one-ring is an open fan. Its incident edges include the two
        //     boundary edges, so the mean is over however many edges there are and is a smaller
        //     sample, not a different quantity. It is NOT extrapolated across the hole and NOT
        //     excluded: an open mesh noises to its edge.
        //
        //   - A SPLIT / DUPLICATED vertex - two vertices at the same position with disjoint
        //     one-rings, which is what append_buffers writes for a hard seam and what an import
        //     of unwelded geometry produces - gets ONE mean PER VERTEX, from its own edges only.
        //     If the two sides are tessellated differently the two halves of the seam displace by
        //     different amounts and the seam OPENS. That is inherent to a per-vertex local
        //     measure, not a defect of this one, and merge_vertices before the noise is the fix.
        //     Absolute mode does not have this failure, which is a reason to keep it.
        //
        // A vertex with NO incident edges has no one-ring and no defined mean; it is left exactly
        // where it is rather than displaced by an invented number.
        Relative
    };

    struct FNoiseDeformParams
    {
        // Read according to MagnitudeMode: world units under Absolute, a fraction of the
        // vertex's mean one-ring edge length under Relative.
        double Magnitude = 5.0;
        double Frequency = 0.25;
        FVector FrequencyShift = FVector::ZeroVector;
        int32 Seed = 0;
        bool bApplyAlongNormal = true;
        ENoiseNormalSource NormalSource = ENoiseNormalSource::Computed;
        ENoiseMagnitudeMode MagnitudeMode = ENoiseMagnitudeMode::Absolute;
    };
    // Absolute mode is the ENGINE CALL, unchanged and untouched by this op's own loop, so
    // existing content is reproduced bit for bit rather than approximately.
    //
    // Relative mode is a local loop, because the engine has no per-vertex magnitude. It reads the
    // SAME noise field through the engine's own public ComputePerlinNoise (same seed, same
    // frequency, same frequency shift, same offsets) and the SAME per-vertex normals through
    // FMeshNormals, so only the magnitude term differs between the two modes.
    //
    // Fails INVALID_ARGUMENT on Relative together with bApplyAlongNormal = false. The engine's
    // vector-displacement branch reads three DECORRELATED noise fields whose offsets come from a
    // helper with no public entry point, so a local loop could only guess at them - and a guess
    // that silently drifts from the engine's field on the next engine version is worse than a
    // refusal that names the combination.
    FOpResult NoiseDeform(UDynamicMesh* Mesh, const FNoiseDeformParams& Params);

    // ------------------------------------------------------------------------
    // Azimuthal harmonic displacement
    //
    // Not an engine op: there is no Geometry Script node for it. It is a local vertex loop in
    // the same shape as Spherify / Cylindrify below, and it exists because the two shapes the
    // .pwmodel primitives could not express - a scalloped conifer rim and a lumpy crown - are
    // both one periodic function of the AZIMUTH about an axis, and nothing else.
    // ------------------------------------------------------------------------

    // One term of the sum. Deliberately three plain scalars rather than a curve or an
    // expression: the format has no variables, loops or expressions by standing decision
    // (docs/wiki-src/model.authoring.md), so the only declarable form is a literal term list.
    struct FHarmonicTerm
    {
        // Cycles per full revolution. Must be >= 1 and integral, or the displacement does not
        // close on itself at theta = 2*pi and leaves a seam of tangled triangles at the wrap.
        // A NON-INTEGRAL order is the one input that cannot be made safe by shrinking the
        // amplitude, which is why it is refused rather than clamped.
        int32 Order = 1;

        // Radial: a FRACTION of the vertex's own perpendicular radius. Axial: UNREAL UNITS.
        // The split is not an oversight - a radial modulation has a local scale to be a
        // fraction OF (which is what keeps a scallop proportional as a revolve profile's
        // radius changes along the axis), and an axial one has none.
        double Amplitude = 0.0;

        // DEGREES, matching every other angle the format publishes.
        double PhaseDegrees = 0.0;
    };

    enum class EHarmonicTarget : uint8
    {
        // Scale the perpendicular radius by (1 + sum). Orientation-safe by construction while
        // the sum stays above -1: a strictly positive radial scale preserves both the angle and
        // the sign of every triangle's facing normal, which is the same argument Spherify's
        // comment makes below.
        Radial,

        // Add the sum to the axis coordinate. A shear along the axis driven by azimuth alone.
        //
        // It has NO analogue of the radial ceiling, and cannot: the displacement is absolute, so
        // no bound on it is a bound on anything. What it does have is a gradient that is worst
        // NEAREST THE AXIS - the displacement per azimuth step is fixed in world units while the
        // spacing between two adjacent vertices shrinks to zero as their radius does, so a
        // pole-adjacent ring is where an axial displacement first outruns its own sampling.
        // Keep the amplitude small against the smallest ring's circumference, or keep the
        // deformed band away from the axis.
        Axial
    };

    struct FHarmonicDeformParams
    {
        EMeshAxis Axis = EMeshAxis::Z;

        // The axis LINE, not a bounding-box centre. Spherify and Cylindrify fit to the bounding
        // box because their target shape is derived from it; this op's axis is the one the
        // geometry was revolved about, which is the part-local origin, and a box-derived centre
        // would silently move whenever an earlier op changed the mesh's extent - so the same
        // document would deform differently depending on what ran before it.
        FVector Center = FVector::ZeroVector;

        EHarmonicTarget Target = EHarmonicTarget::Radial;

        TArray<FHarmonicTerm> Terms;
    };

    // Fails with INVALID_ARGUMENT on a non-integral or below-1 order, and - on Radial only - on
    // a total amplitude of 1 or more, where the radial scale reaches zero or turns negative and
    // inverts the surface at those angles. Refusing is the point of the op: it is being added so
    // two INVERTED models can be rebuilt from primitives, and an op that could invert them again
    // would be worth nothing.
    //
    // Vertices within KINDA_SMALL_NUMBER of the axis are left where they are, on both targets:
    // their azimuth is undefined, and displacing an apex by the arbitrary theta = 0 value is
    // exactly what tangles the fan around it.
    FOpResult HarmonicDeform(UDynamicMesh* Mesh, const FHarmonicDeformParams& Params);

    // EGeometryScriptEmptySelectionBehavior is the one field of
    // FGeometryScriptIterativeMeshSmoothingOptions (and of FGeometryScriptPerlinNoiseOptions)
    // that these three ops deliberately DO NOT publish, on all three.
    //
    // None of them builds a selection - each passes a default-constructed
    // FGeometryScriptMeshSelection, unconditionally. EmptyBehavior decides what an EMPTY
    // selection means, so its default (FullMeshSelection) is what makes the op run on the whole
    // mesh, and its only other value (EmptySelection) makes the op an unconditional no-op on
    // every possible input. Publishing it would add a knob whose sole non-default setting is
    // "do nothing" - which is a worse defect than the one this pass removes, not a fix for it.
    // It becomes a real parameter the moment these ops gain a selection, and not before.
    struct FSmoothParams
    {
        int32 Iterations = 10;
        double Alpha = 0.2;
    };
    FOpResult Smooth(UDynamicMesh* Mesh, const FSmoothParams& Params);

    struct FRelaxParams
    {
        int32 Iterations = 3;
        double Strength = 0.5;
    };
    FOpResult Relax(UDynamicMesh* Mesh, const FRelaxParams& Params);

    struct FStretchParams
    {
        EMeshAxis Axis = EMeshAxis::Z;
        double Factor = 1.5;
    };
    FOpResult Stretch(UDynamicMesh* Mesh, const FStretchParams& Params);

    // Both of these are the same radial map, f(d) = (1-t)*d + t*R, and differ only in what d
    // and R are. It is strictly increasing in d for t in [0,1] and preserves direction, so
    // NEITHER OP CAN FOLD A CONVEX SURFACE - a black cavity after `box -> spherify ->
    // noise_deform` is the displacement op folding on triangles these ops made size-uneven, not
    // these ops folding. Both warn above an anisotropy of 1/3; the metric, the measurements
    // behind the threshold and the remedy are in GeometryOps_Modeling.cpp above Spherify.
    struct FSpherifyParams
    {
        // Lerp weight toward the sphere of radius GetExtent().GetMax() - the LARGEST HALF-EXTENT
        // (75 for a 150x118x92 box), NOT the bounding sphere (105.93 for that box), which is what
        // this comment used to claim. The distinction is the whole behaviour of the op: the
        // target sphere touches the two most distant faces and leaves the corners OUTSIDE it, so
        // corners are pulled in while the flat centres of the short faces are pushed out. Clamped
        // to [0,1] with a warning.
        double Factor = 1.0;
    };
    FOpResult Spherify(UDynamicMesh* Mesh, const FSpherifyParams& Params);

    struct FCylindrifyOutcome
    {
        // The MEAN perpendicular radius over the mesh's vertices - a different fitting
        // convention from spherify's max half-extent, in the sibling op, undocumented until
        // now. It is a measurement, not a bound: vertices sit on both sides of it.
        double AverageRadius = 0.0;
        int32 VerticesModified = 0;
    };

    struct FCylindrifyParams
    {
        EMeshAxis Axis = EMeshAxis::Z;
        // Lerp weight toward the fitted cylinder; clamped to [0,1] with a warning.
        //
        // AT EXACTLY 1.0 - which is this default - the map stops being injective: every vertex
        // is placed at perpendicular radius AverageRadius, so any two vertices sharing an
        // angular direction but not a radius collapse onto each other. On a box that is
        // guaranteed by the two cap faces perpendicular to the axis. Measured on a 100^3 cube
        // at segments (5,5,4): factor 0.82 gives 0 degenerate triangles, factor 1.0 gives 24
        // degenerate triangles, 28 inverted triangles, 144 self-intersecting triangle pairs and
        // a zero-length edge. This is a DIFFERENT mechanism from the anisotropy the warning
        // reports - it is not aspect-driven and fires on a perfect cube - and is not yet
        // diagnosed. Pass anything below 1.0 to stay injective.
        double Factor = 1.0;
    };
    FOpResult Cylindrify(UDynamicMesh* Mesh, const FCylindrifyParams& Params,
                         FCylindrifyOutcome* Outcome = nullptr);

    // ------------------------------------------------------------------------
    // Repair
    // ------------------------------------------------------------------------

    struct FWeldVerticesParams
    {
        // Welds open boundary EDGES whose vertices coincide. The proximity weld of arbitrary
        // coincident vertices is MergeVertices, a different op.
        double Tolerance = 0.0001;
        bool bOnlyUniquePairs = true;
    };
    FOpResult WeldVertices(UDynamicMesh* Mesh, const FWeldVerticesParams& Params);

    struct FFillHolesOutcome
    {
        int32 FilledHoles = 0;
        int32 FailedHoles = 0;
    };

    struct FFillHolesParams
    {
        // Automatic picks per hole and is the engine default; the four explicit methods are the
        // escape hatch for the holes it picks badly. PolygonTriangulation and PlanarProjection
        // are the ones worth naming: a large planar hole filled by the automatic minimal-fill
        // path comes back as a fan of slivers, and either of those two produces a usable
        // triangulation instead.
        EFillHolesMethod FillMethod = EFillHolesMethod::Automatic;

        // Floating disconnected triangles read as holes that cannot be filled, so the engine
        // deletes them first. Off keeps them and lets FailedHoles report them.
        bool bDeleteIsolatedTriangles = true;
    };
    FOpResult FillHoles(UDynamicMesh* Mesh, const FFillHolesParams& Params,
                        FFillHolesOutcome* Outcome = nullptr);

    struct FRemoveDegeneratesParams
    {
        // RepairOrDelete is the engine default and what the op hardcoded. DeleteOnly skips the
        // collapse attempt entirely, which is what a mesh about to be welded wants; RepairOrSkip
        // never deletes, so the triangle count cannot drop.
        ERepairMeshMode Mode = ERepairMeshMode::RepairOrDelete;

        // A triangle under this area is degenerate. Engine default.
        double MinTriangleArea = 0.001;

        // An edge under this length is degenerate. Engine default. Note it is 10x SMALLER than
        // MinTriangleArea's default, and both are absolute world units - on a mesh authored in
        // metres rather than centimetres both defaults are effectively zero and the op looks
        // like it does nothing.
        double MinEdgeLength = 0.0001;

        bool bCompactOnCompletion = true;
    };
    FOpResult RemoveDegenerates(UDynamicMesh* Mesh, const FRemoveDegeneratesParams& Params);

    struct FMergeVerticesOutcome
    {
        // Vertices actually welded away (EMeshResult::Ok). Equal to the before/after delta only
        // when compaction ran, so it is reported directly.
        int32 Merged = 0;
    };

    struct FMergeVerticesParams
    {
        double Tolerance = 0.001;
        bool bCompact = true;
    };
    FOpResult MergeVertices(UDynamicMesh* Mesh, const FMergeVerticesParams& Params,
                            FMergeVerticesOutcome* Outcome = nullptr);

    // ------------------------------------------------------------------------
    // UV
    // ------------------------------------------------------------------------

    // XAtlas auto-unwrap into UVChannel, growing the UV layer set first.
    //
    // Takes a bare channel rather than a params struct because it is also the shared
    // implementation behind GeometryUtils::ApplyXAtlasUnwrap, the Ctx-bound response shell the
    // unwrap verbs share. Fails INVALID_ARGUMENT with the channel-range message when the layer
    // cannot be created: AutoGenerateXAtlasMeshUVs silently no-ops on a missing layer and sends
    // its complaint only to the GeometryScriptDebug argument, which every call site passes null.
    FOpResult UnwrapUVXAtlas(UDynamicMesh* Mesh, int32 UVChannel);

    struct FProjectUVParams
    {
        EUVProjectionMode Projection = EUVProjectionMode::Box;

        // U/V scale of the projection frame. Two-component because that is what the projection
        // expresses and what `uv ... scale=(1, 1)` publishes.
        //
        // The frame is 3D, so ProjectUV gives its third axis Scale.X. That reproduces the uniform
        // frame the scalar Scale this field replaced always produced, which is what keeps
        // geometry.project_uv's output byte-identical.
        //
        // There is no longer a second frame to disagree with: FCompiler::ApplyUVOp used to
        // open-code this projection with the third axis pinned to 1.0, so the two front-ends
        // diverged for box and cylindrical projection at any non-uniform or non-1.0 scale
        // (planar ignores the third axis entirely) and agreed only at the published default
        // scale=(1, 1). ApplyUVOp now routes its three projection modes through this op, so
        // .pwmodel inherited the RPC's third axis - intended, and the only .pwmodel UVs it can
        // move are those of a document that sets a scale other than (1, 1).
        FVector2D Scale = FVector2D(1.0, 1.0);

        int32 UVChannel = 0;

        // Cylindrical only: the opening angle in degrees above which the cylinder's UV seam
        // splits. Ignored by the box and planar modes.
        double SplitAngle = 45.0;

        // Carried for the .pwmodel `uv` op, whose vocabulary is wider than the three projection
        // modes above: `max_iterations` belongs to `mode=xatlas` (UnwrapUVXAtlas), which takes a
        // bare channel and so has nowhere else to keep it. No projection reads it. The default
        // is FGeometryScriptXAtlasOptions::MaxIterations, so an unset value is the engine's.
        //
        // `texture_resolution` used to sit here too, for `mode=layout`. It now lives on
        // FLayoutUVParams below with the rest of that mode's vocabulary - one struct per engine
        // options struct, so there is no second copy of a default to drift.
        int32 MaxIterations = 2;
    };
    FOpResult ProjectUV(UDynamicMesh* Mesh, const FProjectUVParams& Params);

    // Mirrors EGeometryScriptUVLayoutType.
    enum class EUVLayoutType : uint8
    {
        // Apply Scale and Translation to every UV value, packing nothing.
        Transform,
        // Fit each island into the unit square individually, so they overlap.
        Stack,
        // Pack the islands collectively into the unit square with no overlap. The default.
        Repack,
        // Normalize island area to an average texel density.
        Normalize
    };

    // FUVPacker::StandardPack clamps to this domain. Keep the parser and both layout front-ends
    // on these production values so invalid resolutions never reach the int32-to-uint32 path.
    inline constexpr int32 LayoutUVTextureResolutionDefault = 1024;
    inline constexpr int32 LayoutUVTextureResolutionMin = 2;
    inline constexpr int32 LayoutUVTextureResolutionMax = 16384;

    // FGeometryScriptLayoutUVsOptions, one field for one field, at the engine's defaults.
    //
    // UDIMResolutions is NOT carried: it is a TMap<int32, int32>, and FPwModelValue has no map
    // member (PwModelAst.h), so a per-tile resolution table has no literal in the format. The
    // bEnableUDIMLayout flag below is still exposed - UDIM-aware packing at the single
    // TextureResolution is the reachable half and is useful on its own.
    struct FLayoutUVParams
    {
        int32 UVChannel = 0;

        EUVLayoutType LayoutType = EUVLayoutType::Repack;

        // Drives the gutter left between islands, not the size of anything written.
        int32 TextureResolution = LayoutUVTextureResolutionDefault;

        // Applied to the UVs AFTER packing. Uniform, unlike FProjectUVParams::Scale, because
        // that is the shape the engine field has.
        double Scale = 1.0;
        FVector2D Translation = FVector2D::ZeroVector;

        // Repack only. Preserving scale can push the packing outside the unit square and
        // preserving rotation costs space, which is why the engine defaults both off.
        bool bPreserveScale = false;
        bool bPreserveRotation = false;

        // Lets the packer mirror an island to save space. Off by default because a flipped
        // island breaks downstream operations that assume winding.
        bool bAllowFlips = false;

        // Keeps islands inside their originating UDIM tile.
        bool bEnableUDIMLayout = false;
    };
    // Repack / stack / normalize an EXISTING UV layout in place. It creates no UVs: on a channel
    // with no elements it packs nothing and reports success, which is why both front-ends check
    // the channel afterwards. Same INVALID_ARGUMENT channel-range failure as ProjectUV.
    FOpResult LayoutUV(UDynamicMesh* Mesh, const FLayoutUVParams& Params);

    // FGeometryScriptPatchBuilderOptions, one field for one field, at the engine's defaults.
    //
    // Two of its ten members are nested option structs and are FLATTENED here rather than
    // nested, for the reason FPolygroupEditSpec is: FPwModelValue has no sub-object member, so
    // a nested block is not expressible. FGeometryScriptExpMapUVOptions becomes the two
    // ExpMapNormalSmoothing* fields and FGeometryScriptRepackUVsOptions becomes the two
    // Packing* fields; the prefixes are what keep them identifiable as the sub-struct's.
    //
    // GroupLayer (FGeometryScriptGroupLayer) is NOT carried. Its ExtendedLayerIndex names a
    // named polygroup layer that must already exist on the mesh, and neither front-end can
    // create one - the format has no polygroup-layer table and the RPC surface has no verb that
    // adds an extended layer - so every reachable value other than the default would select a
    // layer that is not there.
    struct FPatchBuilderUVParams
    {
        int32 UVChannel = 0;

        // Seed patch count for the clustering pass, not the island count of the result: the
        // merging pass below collapses patches, so the output usually has fewer.
        int32 InitialPatchCount = 100;
        int32 MinPatchSize = 2;

        double PatchCurvatureAlignmentWeight = 1.0;
        double PatchMergingMetricThresh = 1.5;
        double PatchMergingAngleThresh = 45.0;

        // FGeometryScriptExpMapUVOptions, flattened. Smoothing the normals first produces
        // rounder patches at the cost of following the surface less closely.
        int32 ExpMapNormalSmoothingRounds = 0;
        double ExpMapNormalSmoothingAlpha = 0.25;

        // Start from the mesh's existing polygroups instead of clustering from scratch.
        bool bRespectInputGroups = false;

        // Pack the patches into the unit square once they are built. With this off the two
        // Packing* fields below are read by nothing.
        bool bAutoPack = true;

        // FGeometryScriptRepackUVsOptions, flattened. Note the engine's 512 here is NOT
        // FLayoutUVParams::TextureResolution's 1024 - they are different structs feeding
        // different packers, and matching them up would be inventing a default.
        int32 PackingTargetImageWidth = 512;
        bool bPackingOptimizeIslandRotation = true;
    };
    // Segment the mesh into patches and flatten each one, optionally packing the result. Unlike
    // LayoutUV this CREATES UVs. Same INVALID_ARGUMENT channel-range failure as ProjectUV.
    FOpResult AutoUVPatchBuilder(UDynamicMesh* Mesh, const FPatchBuilderUVParams& Params);

    struct FTransformUVsParams
    {
        int32 UVChannel = 0;
        FVector2D Translate = FVector2D::ZeroVector;
        FVector2D Scale = FVector2D(1.0, 1.0);
        double Rotation = 0.0;
    };
    // Each of the three transforms is skipped when it is the identity, so a call that changes
    // nothing touches nothing.
    FOpResult TransformUVs(UDynamicMesh* Mesh, const FTransformUVsParams& Params);
}
