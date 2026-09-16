// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryUtils.h - Shared helpers and macros for geometry handlers
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "PinWrightHelpers.h"

class UDynamicMesh;
class UDynamicMeshComponent;
class USkeleton;
class AActor;
class FHandlerContext;

DECLARE_LOG_CATEGORY_EXTERN(LogMcpGeometryHandlersNew, Log, All);

// Helper macros for JSON field access (delegating to existing helpers)
#define GetStringFieldGeomNew GetJsonStringField
#define GetNumberFieldGeomNew GetJsonNumberField
#define GetBoolFieldGeomNew GetJsonBoolField
#define GetIntFieldGeomNew GetJsonIntField

// Safety limits for geometry operations to prevent OOM
static constexpr int32 GEOM_MAX_SEGMENTS = 256;

// Stair step counts have their OWN ceiling, because a step count is not a segment count and
// inheriting GEOM_MAX_SEGMENTS was inheritance rather than a decision. The number is derived
// from what the engine actually allocates, measured rather than guessed:
//
//   FLinearStairGenerator (the default, floating=false) builds each side as a filled staircase
//   silhouette, NumQuadsPerSide = TriangleNumber(NumSteps) (StairGenerator.cpp:63), so the mesh
//   is QUADRATIC in the step count: 2*n^2 + 10*n triangles. FFloatingStairGenerator is linear
//   (16*n - 4) and is nowhere near any bound.
//
// So the count does drive a quadratic allocation, which is why a bound belongs here at all - but
// 256 was chosen for a different growth curve and left a 300-step staircase (183,000 triangles,
// entirely affordable, and a plausible request for a tower) capped for no reason anyone measured.
// 400 solid steps is 324,000 triangles, under GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH with room for
// the generator appending into a mesh that already carries geometry; 512 would be 529,408 and
// would breach that ceiling on its own.
static constexpr int32 GEOM_MAX_STAIR_STEPS = 400;
static constexpr double GEOM_MAX_DIMENSION = 100000.0;
static constexpr double GEOM_MIN_DIMENSION = 0.01;

// Polygon count guards to prevent OOM crashes
static constexpr int32 GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH = 500000;
static constexpr int32 GEOM_MAX_SUBDIVIDE_ITERATIONS = 6;
static constexpr int32 GEOM_WARNING_TRIANGLE_THRESHOLD = 250000;

// Memory pressure thresholds
static constexpr float GEOM_MEMORY_PRESSURE_WARNING = 0.80f;
static constexpr float GEOM_MEMORY_PRESSURE_CRITICAL = 0.90f;

namespace GeometryUtils
{

// Helper to read FVector from JSON (supports both object and array formats)
FVector ReadVectorFromPayload(const TSharedPtr<FJsonObject>& Payload, const TCHAR* FieldName, FVector Default = FVector::ZeroVector);

// Helper to read FRotator from JSON (supports both {pitch,yaw,roll} and {x,y,z} formats)
FRotator ReadRotatorFromPayload(const TSharedPtr<FJsonObject>& Payload, const TCHAR* FieldName, FRotator Default = FRotator::ZeroRotator);

// Helper to read FTransform from JSON
FTransform ReadTransformFromPayload(const TSharedPtr<FJsonObject>& Payload);

// Helper to create a dynamic mesh for operations
UDynamicMesh* GetOrCreateDynamicMesh(UObject* Outer);

// Check memory pressure before heavy operations
bool IsMemoryPressureSafe();

// Get current memory usage percentage
double GetMemoryUsagePercent();

// Give a dynamic mesh the skeleton's bone attributes (names, parent indices, reference pose), at
// the defaults of FGeometryScriptCopyBonesFromMeshOptions. A no-op on a null skeleton or mesh.
//
// UGeometryScriptLibrary_MeshBoneWeightFunctions::CopyBonesFromSkeleton and that options struct
// both arrived in UE 5.5; 5.4 has only CopyBonesFromMesh, which takes a source UDynamicMesh. On
// 5.4 this writes the same three attribute arrays directly through
// FDynamicMeshAttributeSet::EnableBones, which is what the 5.5 function does under its default
// options (no re-index, all bones, so the bone list is the raw reference skeleton in order).
void CopySkeletonBonesToMesh(USkeleton* Skeleton, UDynamicMesh* Mesh);

// Clamp segment count to safe range
int32 ClampSegments(int32 Value, int32 Default = 1);

// Clamp dimension value to safe range
double ClampDimension(double Value, double Default = 100.0);

// Runs the shared XAtlas auto-unwrap, notifies the component of the mesh change, and
// sends a success echoing {actorName, uvChannel} with SuccessMsg. The response shell
// shared by geometry.unwrap_uv and geometry.pack_uv_islands so those two verbs cannot
// drift. The caller resolves the mesh + component through its own target finder, then
// ExtraFields (may be null) lets a verb add fields (e.g. pack_uv_islands'
// textureResolution) before the success is sent.
//
// geometry.auto_uv is NOT one of them any more: it reaches the same unwrap through the
// pure op, calling GeometryOps::UnwrapUVXAtlas and building its own response
// (MeshOpsHandler.cpp), so the three verbs still cannot drift but only two of them come
// through here. The real count is TWO call sites in ONE file (MeshInfoHandler.cpp).
// Verify by grep before quoting a number; this comment has already been wrong once.
//
// The geometry itself is GeometryOps::UnwrapUVXAtlas (GeometryOps_Modeling.h); this is
// the Ctx-bound wrapper over it, and the whole of what it adds is the notify + respond
// steps. Call the op directly from anything that does not need to send a response.
void ApplyXAtlasUnwrap(
    const FHandlerContext& Ctx,
    UDynamicMesh* Mesh,
    UDynamicMeshComponent* Component,
    const FString& ActorName,
    int32 UVChannel,
    const FString& SuccessMsg,
    const TFunctionRef<void(const TSharedPtr<FJsonObject>&)>& ExtraFields);

// Overload with no extra result fields.
void ApplyXAtlasUnwrap(
    const FHandlerContext& Ctx,
    UDynamicMesh* Mesh,
    UDynamicMeshComponent* Component,
    const FString& ActorName,
    int32 UVChannel,
    const FString& SuccessMsg);

// Add the canonical identity of a resolved actor to a geometry response. With no Prefix the
// fields are actorPath and actorObjectName; with a role such as "target" they are
// targetActorPath and targetActorObjectName. Request-echo fields remain owned by each handler.
void AddResolvedActorIdentity(
    const TSharedPtr<FJsonObject>& Result,
    AActor* Actor,
    const TCHAR* Prefix = nullptr);

// Build the default destination used by geometry asset-bake verbs from the resolved actor's
// display label (falling back to its internal name), never from the caller's identifier/path.
FString MakeDefaultGeometryAssetPath(AActor* Actor);

// Reads the mesh's current vertex count off the handle. Centralizes the
// UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount accessor choice so
// every caller sources it from one place (the MeshQueryFunctions library has no
// triangle-count accessor in UE 5.7 — read triangles via UDynamicMesh::GetTriangleCount).
int32 GetMeshVertexCount(UDynamicMesh* Mesh);

// A vertex whose dominant bone is nearer than this is never flagged as out of reach, however the
// ordinal test below comes out. It exists so a degenerate or near-zero-extent mesh, where every
// bone sits on top of every vertex and the ordering is numeric noise, cannot produce a flag.
// One centimetre in Unreal units - far below any real skinning reach and far above float noise.
static constexpr double GEOM_MIN_FLAGGED_BONE_DISTANCE = 1.0;

// Result of an exhaustive scan of the mesh's default skin-weight profile.
struct FSkinWeightCoverage
{
    bool bHasProfile = false;
    int32 VertexCount = 0;
    int32 WeightedCount = 0;
    int32 UnweightedCount = 0;

    // ---- Dominant-bone reach --------------------------------------------------------------
    //
    // "Every vertex carries an influence" is not the same as "the influences make sense", and
    // the gap between them is where a corrupted skinned mesh ships. Measured case: a boolean over
    // SKM_Manny left vertices at the feet 0.934-weighted to `head` 162.6 cm away, with all 160
    // other bones nearer - and the write reported verticesUnweighted:0, fullyWeighted:true.
    //
    // THE RULE IS ORDINAL, NOT A DISTANCE THRESHOLD: a vertex is flagged when its dominant
    // influence names the bone that is FARTHEST FROM IT of every bone the mesh carries. That
    // needs no tuning and produces no judgement call - a vertex whose strongest influence is the
    // single most distant bone in the skeleton is wrong on any character, at any scale, in any
    // pose. (GEOM_MIN_FLAGGED_BONE_DISTANCE above is a degeneracy floor, not the criterion.)
    //
    // Distances are measured in the mesh's own space against the reference-pose bone positions
    // the mesh carries on its bone attributes, so the check is self-contained - but that also
    // means it is only meaningful while the mesh is still in the space its bones were authored
    // in. A caller that moved the mesh away from its skeleton gets flags it should ignore, which
    // is why this REPORTS and never blocks.
    bool bBoneReachChecked = false;
    int32 FarBoneCount = 0;
    int32 WorstFarBoneVertex = INDEX_NONE;
    FName WorstFarBoneName;
    double WorstFarBoneDistance = 0.0;

    // Every vertex carries at least one influence. Says nothing about whether the influences are
    // plausible - use IsWeightingTrustworthy for that.
    bool IsFullyWeighted() const { return bHasProfile && UnweightedCount == 0; }

    // Fully weighted AND no vertex is dominated by the farthest bone in the skeleton. This is the
    // predicate a skeletal-asset write should publish, because the other one answers true over a
    // mesh whose skinning is visibly broken.
    bool IsWeightingTrustworthy() const { return IsFullyWeighted() && FarBoneCount == 0; }
};

FSkinWeightCoverage ScanSkinWeightCoverage(UDynamicMesh* Mesh);

// Fills the dominant-bone-reach half of a coverage result. Called by ScanSkinWeightCoverage, so
// no caller of that needs it; exposed for the tests, which need to run the reach check over a
// mesh they built by hand without paying for the full coverage scan.
void ScanDominantBoneReach(UDynamicMesh* Mesh, FSkinWeightCoverage& Coverage);

// ---------------------------------------------------------------------------
// Mesh health
//
// The four defects that decide whether a mesh is a solid: an open boundary, a
// degenerate (near-zero-area) triangle, a bowtie vertex, and how many shells the
// result actually is.
//
// Shared rather than owned by geometry.check_health because it now has two callers
// with opposite failure modes. The RPC verb is asked, and reports. The .pwmodel
// compiler is NOT asked - it runs this as its own final gate, and every example
// model compiled clean while carrying degenerates and, at one point, 272 boundary
// edges: an open shell shipped as a successful asset. Per-example degenerate counts
// are not repeated here - the corpus reading lives in docs/wiki-src/model.examples.md
// and moves whenever an example is reworked.
//
// Two implementations of "is this mesh closed" would let the verb
// and the compiler answer differently about the same mesh, which is the failure this
// extraction removes.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Winding, separately from connectivity.
//
// THE DEFECT THIS EXISTS FOR: a closed mesh whose triangle winding is uniformly
// reversed renders IDENTICALLY to a correct one. Backface culling shows the camera
// whichever wall faces it, and the two walls carry opposite normals - so the
// silhouette is complete from every angle, the shading is unchanged, and every field
// of FMeshHealth below agrees between the two: closed, 0 boundary edges, 0 bowties,
// one component, same counts. An A/B luminance comparison of a render moves by 4e-6,
// a measurement that cannot fail, and it was twice quoted as proof a shipped mesh was
// fine. Nothing in this file could see it, which is why it shipped.
//
// It is not cosmetic, because winding - not shading - is what the OFFLINE consumers
// read. The mesh distance field decides inside from outside by counting backface hits
// (MeshDistanceFieldUtilities.cpp:261-281), so an inverted shell inverts its field and
// Lumen / DFAO then light the part as though the camera were inside it: invisible in
// any asset-preview capture, wrong in a lit level.
//
// Two independent signals, because they catch different faults:
//
//   SignedVolume            uniform inversion. Negative on an inverted closed shell,
//                           positive on a correct one, MEANINGLESS on an open one.
//   InconsistentEdges       PARTIAL inversion: neighbours that disagree. A uniformly
//                           inverted shell is perfectly consistent, so this one is
//                           blind to the case above - they do not substitute for each
//                           other.
// ---------------------------------------------------------------------------

struct FMeshOrientation
{
    // Edges adjacent to exactly one triangle. A closed (watertight) solid has none.
    int32 BoundaryEdges = 0;

    // Interior edges whose two triangles traverse the shared edge in the SAME direction
    // rather than in opposite ones - i.e. one of the pair is wound against the other.
    // Zero on any mesh with a coherent surface orientation, uniformly inverted or not.
    int32 InconsistentEdges = 0;

    // The divergence-theorem volume, in the engine's own facing-normal convention:
    // positive when the facing normals point OUT of the enclosed volume. Only meaningful
    // when IsClosed(); on an open surface it is the integral of an unclosed boundary and
    // means nothing. From TMeshQueries::GetVolumeArea, which is already signed - see the
    // definition for why it is not hand-rolled.
    double SignedVolume = 0.0;

    // Total triangle area. Reported with the volume because it is the scale the volume
    // has to be read against: |SignedVolume| near zero relative to Area^1.5 is a shell
    // that folds back on itself, not a solid.
    double SurfaceArea = 0.0;

    bool IsClosed() const { return BoundaryEdges == 0; }
    bool IsOrientationConsistent() const { return InconsistentEdges == 0; }

    // The gate for the defect at the top of this comment. Deliberately false on an open
    // mesh: an open surface is not inside out, it is open, and reporting it as inverted
    // would put a second wrong answer next to the first.
    bool IsInverted() const { return IsClosed() && SignedVolume < 0.0; }
};

// One pass over the edges plus one over the triangles. O(E + T), allocation-free,
// null-safe (a null mesh reports all-zero, which reads as closed and consistent -
// callers that care about emptiness check the triangle count first).
//
// Split out from MeasureMeshHealth so the .pwmodel compiler can measure it PER PART
// without paying for the bowtie sweep and the connected-component pass on every part.
// A single inverted part inside an otherwise correct model is the realistic case, and a
// model-wide signed volume averages it away - a correct 20-cube plus an inverted 10-cube
// sums to a healthy-looking +7000.
FMeshOrientation MeasureMeshOrientation(UDynamicMesh* Mesh);

struct FMeshHealth
{
    // Edges adjacent to exactly one triangle. A closed (watertight) solid has none.
    int32 BoundaryEdges = 0;

    // Triangles whose area is below DegenerateAreaEpsilon below. Common and usually
    // survivable - the static-mesh build welds most of them away - but they are what
    // an exact-tangency boolean produces, so a jump in this count is the tell that two
    // operands were placed to touch rather than to overlap.
    int32 DegenerateTriangles = 0;

    // Bowtie vertices: two or more triangle fans meeting at one vertex. FDynamicMesh3
    // edges hold at most 2 triangles by construction, so non-manifold EDGES cannot
    // exist in this representation and the bowtie vertex is the detectable defect.
    int32 NonManifoldVertices = 0;

    // Connected components. More than one is legitimate (a model of separate pieces)
    // and is reported rather than judged.
    int32 ComponentCount = 0;

    int32 VertexCount = 0;
    int32 TriangleCount = 0;

    // Allocated vertices that no triangle names. FDynamicMesh3 keeps a vertex live after the
    // last triangle using it is removed, so a cut or a boolean can strand one: it is counted in
    // VertexCount and is part of no surface. The .pwmodel compiler's reported box is reduced over
    // TRIANGLES so such a vertex cannot widen it - which leaves VertexCount and that box
    // describing the same mesh differently, correctly, with nothing naming the difference. This
    // field is that name, and it is also the tell for the defect class that produced the
    // reduction: an op removed geometry without cleaning up after itself.
    //
    // Deliberately NOT part of IsHealthy(). An orphan is inert - the static-mesh build is driven
    // by triangles, so it reaches no asset and changes nothing that lands on disk - and failing a
    // mesh on it would refuse models that are correct. Reported, not judged.
    int32 UnreferencedVertices = 0;

    // Interior edges whose two triangles disagree on winding. See FMeshOrientation.
    int32 InconsistentEdges = 0;

    // Signed enclosed volume, positive when the facing normals point outward. Only
    // meaningful when IsClosed(). See FMeshOrientation.
    double SignedVolume = 0.0;

    bool IsClosed() const { return BoundaryEdges == 0; }
    bool IsOrientationConsistent() const { return InconsistentEdges == 0; }
    bool IsInverted() const { return IsClosed() && SignedVolume < 0.0; }

    // Orientation is part of the verdict, not an extra. Before it was, an inside-out
    // closed shell answered `healthy: true` with every other field agreeing with a
    // correct one - a verdict that could not fail on the one defect nothing else in this
    // struct can see. The two added terms are no-ops on every mesh that was healthy
    // before: a correct closed solid has 0 inconsistent edges and a positive volume, and
    // an OPEN mesh is already unhealthy on IsClosed() before IsInverted() is consulted.
    bool IsHealthy() const
    {
        return IsClosed() && DegenerateTriangles == 0 && NonManifoldVertices == 0
            && IsOrientationConsistent() && !IsInverted();
    }
};

// The area below which a triangle counts as degenerate. Named rather than inlined so
// the compiler's diagnostic can quote the same threshold the RPC verb reports against.
inline constexpr double DegenerateAreaEpsilon = 1e-6;

// Walks the mesh once per defect class. O(E + T + V) and allocation-free; safe to run
// at the end of a compile. Null-safe: a null mesh reports all-zero, which reads as
// closed - callers that care about emptiness check the triangle count first, as both
// callers do.
//
// The boundary-edge and orientation halves come from MeasureMeshOrientation rather than
// from a second copy of the same walk, so the per-part measurement and this one cannot
// answer differently about the same mesh.
FMeshHealth MeasureMeshHealth(UDynamicMesh* Mesh);

// ---------------------------------------------------------------------------
// EMBEDDING, separately from closure and from winding.
//
// THE DEFECT THIS EXISTS FOR: the documented gate `IsClosed() && SignedVolume > 0` is
// GREEN on two meshes that are not solids, and every other field of FMeshHealth agrees
// with a correct one on both.
//
//   THE MEMBRANE. A revolve of a section whose two ends sit off the axis at the same
//   height caps BOTH ends to the axis, so a disc spans the bore. The two fans are
//   oppositely wound and their contributions to SignedVolume cancel EXACTLY - the
//   number that comes back is the correct annulus figure, not an approximation of it.
//   BoundaryEdges is 0 because the surface really is closed, InconsistentEdges is 0,
//   DegenerateTriangles is 0, NonManifoldVertices is 0, ComponentCount is 1. Nothing
//   above moves, and the ring has a lid across its bore.
//
//   THE PINCH. A section swept along a rotating path sags at each ruled quad's centre
//   by about half_width * dtheta / 2, against a budget of the section's HALF THICKNESS;
//   past that the top wall passes THROUGH the bottom one. SignedVolume then degrades
//   SMOOTHLY - measured at 62% and 25% of the analytic swept volume with every other
//   field still green - and only changes sign long after the mesh stopped being a solid.
//   There is no threshold on it at which anything can be said.
//
// Both are one statement: the surface is not an EMBEDDED boundary, because it passes
// through itself. That is one measurement, and it is the one below - not two checks
// bolted together.
//
// WHY IT IS COUNTED PER COMPONENT AND NOT ACROSS THE WHOLE MESH. Two separate closed
// shells that interpenetrate are the ordinary way a model is built here - a shape per
// lobe, a shape per limb, appended and never unioned - and that arrangement is already
// reported separately at bounding-box granularity. A count that included those pairs
// would be non-zero on a large fraction of CORRECT models and would be turned off
// wholesale, which is worse than no check at all. Restricting it to pairs inside ONE
// edge-connected component holds every legitimate arrangement at zero - a hollow shell
// (two nested shells), a torus, two disjoint parts, an open sheet, a lathe whose axis
// cap is coplanar with its base face - and leaves exactly the fault above: a single
// surface crossing itself.
// ---------------------------------------------------------------------------

// The largest mesh this measurement is attempted on. The tree-vs-itself descent visits
// every overlapping box pair, so a mesh that overlaps itself everywhere is quadratic;
// above this the answer is DECLINED (bMeasured stays false) rather than approximated.
// Well under GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH, which is the size at which that
// pathology is affordable to nobody.
inline constexpr int32 SelfIntersectionMaxTriangles = 200000;

// The most distinct crossing pairs recorded. Past it the count is a FLOOR and says so;
// the verdict is unaffected, because a floor above zero is still above zero.
inline constexpr int32 SelfIntersectionMaxPairs = 4096;

struct FMeshSelfIntersection
{
    // False when there was nothing to measure (null or empty mesh) or when the mesh is
    // over SelfIntersectionMaxTriangles. NOTHING below means anything when it is false,
    // and false must never be read as "clean" - reading a missing signal as a passing
    // one is the exact failure this struct exists to remove.
    bool bMeasured = false;

    // Distinct pairs of triangles in the SAME edge-connected component whose surfaces
    // cross or overlap. Pairs that share a vertex are excluded: neighbours in any
    // tessellation meet along their shared edge by construction, and a cone apex has a
    // whole fan meeting at one point - counting those would report every mesh.
    int32 PairCount = 0;

    // How many components carry at least one such pair. 1 says one shell is at fault
    // rather than the whole model; it is the nearest thing to "which part" this
    // measurement can say, since triangle ids do not map back to source lines.
    int32 SelfIntersectingComponents = 0;

    // PairCount reached SelfIntersectionMaxPairs and is a floor rather than a total.
    bool bTruncated = false;

    // A point on the first crossing found. An interior surface is invisible in a render
    // and has no line number, so a coordinate is the only locator an author gets.
    bool bHasWitness = false;
    FVector3d Witness = FVector3d::ZeroVector;

    // The gate. Deliberately false on an unmeasured mesh, for the reason on bMeasured.
    bool IsEmbedded() const { return bMeasured && PairCount == 0; }
};

// One connected-component walk, then one AABB tree and one tree-vs-itself descent PER
// COMPONENT. Roughly O(T log T), with the triangle-triangle test reached only by pairs
// whose boxes overlap - NOT the O(E + T + V) of MeasureMeshHealth, which is why this is
// a separate call rather than folded into it: MeasureMeshHealth is documented as
// allocation-free and is run per part as well as per model. Null-safe.
//
// The .pwmodel merge stage already builds one AABB tree per connected component at the
// same point, for the floating-geometry pass, so the marginal cost where this matters
// most is one more set of tree builds and the descents.
FMeshSelfIntersection MeasureMeshSelfIntersection(UDynamicMesh* Mesh);

// Echoes the mesh's current vertexCount/triangleCount onto a success Result,
// matching the keys geometry.get_mesh_info uses so callers can confirm an op
// didn't collapse the mesh without a second readback. Null-safe. Shared by the
// deformer verbs (twist/taper/bend/etc.) and the array verbs so the accessor
// choice and key names cannot drift across handlers in different files.
void SetMeshCountFields(UDynamicMesh* Mesh, const TSharedPtr<FJsonObject>& Result);

// True only when the mesh carries a UV layer at UVChannel that actually has ELEMENTS.
// Element presence rather than layer presence is the whole point: SetNumUVSets creates an
// element-less layer, and it is the missing elements that ship an untextured asset.
// Null-safe (false on null, on a negative channel, and on a channel the mesh does not have).
// O(1).
//
// This is the shared body of both channel-0 guards below, and the only place in the module
// that should answer "does this channel have elements": a hand-rolled copy that drifted from
// these guards is how a crash guard stops guarding.
bool MeshHasUVElementsInChannel(UDynamicMesh* Mesh, int32 UVChannel);

// True only when the mesh carries a primary UV layer (channel 0) that actually
// has elements. Null-safe (false on null). Used by EnsureMeshHasUVs to decide
// whether a UV projection is needed before a StaticMesh bake. Channel 0 is hard-coded
// because MikkT reads channel 0 and no other; == MeshHasUVElementsInChannel(Mesh, 0).
bool MeshHasUsableUVs(UDynamicMesh* Mesh);

// Strictly stronger than MeshHasUsableUVs: EVERY triangle of the mesh must carry set UV
// elements on UVChannel, not merely SOME. Defaults to channel 0. Null-safe (false on null).
//
// MeshHasUsableUVs answers ElementCount() > 0, which is the right question for the MikkT bake
// (a size-0 UV array is what that indexes out of range) and the WRONG one for any engine
// routine that reads a specific triangle's UVs. FDynamicMeshUVOverlay stores -1 in
// ElementTriangles for an unset triangle, and GetTriElements feeds ElementTriangles[3*tid]
// straight into GetElement as `ElementID * ElementSize` (DynamicMeshOverlay.h:820 -> :501), so
// an unset triangle indexes Elements[-2]. TDynamicVector::operator[] takes a uint32 and its
// only bounds test is a checkSlow (DynamicVector.h:176-180), compiled out in Development - so
// -2 becomes 4294967294, a huge block index, and the read is out of bounds with no assert.
//
// A partially-set channel 0 is therefore as unsafe as an absent one for the engine's boundary
// stitcher, which reads UVs per triangle with no IsSetTriangle check at all
// (JoinMeshLoops.cpp:54, inside CalculateAverageUVScale). shell is the op that reaches it.
//
// Uses the overlay's own IsTriangleStorageValid() before the per-triangle sweep because
// IsSetTriangle itself indexes ElementTriangles unchecked: an overlay whose per-triangle
// storage was never grown to the parent's MaxTriangleID would be read out of bounds BY THE
// GUARD. Cost is O(triangles); the O(1) element-presence half runs first and short-circuits it.
bool MeshHasUVsOnEveryTriangle(UDynamicMesh* Mesh, int32 UVChannel = 0);

// Guarantees the mesh has a usable UV0 layer before a DynamicMesh->StaticMesh bake,
// returning whether it does after the call. A mesh authored via append_buffers /
// append_vertex without uvs has a size-0 UV array; the StaticMesh render build then
// runs MikkT (FStaticMeshOperations::ComputeMikktTangents) which indexes [0] into
// that array -> Array.h OOB assert -> HARD editor crash on the async build worker.
// The crash fires whenever bRecomputeTangents OR bRecomputeNormals is set (both
// invoke MikkT), so toggling only the geometry-script tangent option does NOT
// prevent it — the mesh must actually carry UVs. When MeshHasUsableUVs is false
// this applies a DETERMINISTIC box projection (EnsureMeshHasUVChannel(Mesh, 0) +
// SetMeshUVsFromBoxProjection, framed on the mesh bounding box) — a pure
// per-triangle projection with no solver, unlike XAtlas auto-unwrap which can hang
// on degenerate meshes. The channel guard is EnsureMeshHasUVChannel and not a bare
// SetNumUVSets(Mesh, 1) precisely because the latter sets the count EXACTLY and so
// truncated an authored lightmap in channel 1 off any mesh whose channel 0 was
// empty. Null-safe (false on null). Used by the DynamicMesh->StaticMesh bake site
// (convert_to_static_mesh) so the crash guard stays in one place.
//
// SETTLED, DO NOT RE-INVESTIGATE: this gate is MeshHasUsableUVs (ElementCount() > 0) and NOT
// MeshHasUVsOnEveryTriangle, and that is correct rather than an oversight. A partially-set
// channel 0 - elements present, some triangles at -1 - is reachable and is genuinely dangerous
// for shell, but it CANNOT reach MikkT with a -1, because MikkT never sees a UV ELEMENT ID.
// Read from UE 5.8 source, statically, in Aug 2026:
//   - Every converter path substitutes (0,0) for an unset triangle before MikkT runs.
//     FDynamicMeshToMeshDescription::Convert dispatches to Convert_NoSharedInstances
//     (DynamicMeshToMeshDescription.cpp:466-472; the Convert_SharedInstances call on :469 is
//     commented out), and that path tests !IsSetTriangle and appends one shared zero UV element
//     instead of reading the overlay (:1107-1118). The Update/UpdateAttributes helper does the
//     same with FVector2f::ZeroVector (SetAttributesFromOverlay, :44-72).
//   - MikkT then indexes UVs by FVertexInstanceID, never by overlay element ID:
//     VertexInstanceUVs[TriangleVertexInstanceIDs[FaceIdx*3 + VertIdx]]
//     (StaticMeshOperations.cpp:1744-1750), bound at :1786-1793.
//   - The DynamicMesh-side MikkT wrapper, which DOES read the overlay directly, tests for
//     InvalidID and writes 0 (DynamicMeshMikkTWrapper.cpp:69-83).
// What the ZERO-ELEMENT case still is, and why this gate stays: ComputeMikktTangents binds
// GetRawArray(0) with no channel check (StaticMeshOperations.cpp:1790) while its siblings test
// GetNumChannels() > 0 (:168-171, :1446-1450), and GetRawArray early-outs on zero ELEMENTS but
// not on zero CHANNELS (MeshAttributeArray.h:1093-1102 -> the bare ArrayForChannels[Index] at
// :731). Zero UV layers on the DynamicMesh reaches that state through
// FDynamicMeshAttributeSet::SetNumUVLayers(0), which append_buffers with no uvs= produces.
// The cost of the partial case is visual, not fatal: every unset triangle becomes a degenerate
// zero-area UV triangle sharing ONE element, so it inherits a neighbour's tangent frame
// (MikkTSpace ships binary-only here; contract at ThirdParty/MikkTSpace/inc/mikktspace.h:44-49).
// The compiler prevents that upstream (FCompiler::ReconcileUVChannelsForAppend and
// FillMissingUVChannels), which is the right layer for a cosmetic defect.
bool EnsureMeshHasUVs(UDynamicMesh* Mesh);

// Grows the mesh's UV layer set so channel UVChannel exists before a UV-generation
// op (project_uv / XAtlas unwrap) writes into it, returning whether that channel
// exists afterward. GeometryScript's projection and XAtlas routines silently no-op
// when the target UV layer is absent: their ApplyMeshUVEditorOperation guard returns
// without running the edit, and the "UVSetIndex does not exist" complaint goes only
// to the GeometryScriptDebug arg (passed nullptr at these call sites), so an
// unguarded call reports success while creating ZERO UV elements on a hand-authored
// (append_buffers, no uvs) mesh. Grow-only and UVChannel-aware: only widens when the
// requested channel is out of range, so — unlike a bare SetNumUVSets(Mesh, 1) — it
// never truncates pre-existing higher UV sets and can address channels above 0. The
// layer starts element-less; the caller's projection/unwrap then populates it.
// Distinct from EnsureMeshHasUVs, which is a channel-0-only bake guard that also
// box-projects. Null-safe (false on null mesh or negative channel).
bool EnsureMeshHasUVChannel(UDynamicMesh* Mesh, int32 UVChannel);

// ---------------------------------------------------------------------------
// Normal-overlay parentage.
//
// The same question the UV pair above asks, for the other overlay, and it exists for the same
// reason: an engine routine reads it with no check and the read is out of bounds rather than an
// assert. FMeshSpaceDeformerOp's three subclasses - FFlareMeshOp, FBendMeshOp, FTwistMeshOp -
// each walk the primary normal overlay to rotate its elements and take the PARENT VERTEX of an
// element straight into FDynamicMesh3::GetVertex:
//
//   FlareMeshOp.cpp:71-78    if (!Normals->IsElement(ElID)) return;
//                            auto VertexID = Normals->GetParentVertex(ElID);
//                            const FVector3d& SrcPos = ResultMesh->GetVertex(VertexID);
//   BendMeshOp.cpp:106-113   identical
//   TwistMeshOp.cpp:51-58    identical
//
// IsElement is the only guard, and it does NOT imply a parent. TDynamicMeshOverlay::AppendElement
// allocates the element (refcount 1) and writes FDynamicMesh3::InvalidID into ParentVertices
// (DynamicMeshOverlay.cpp:40-53); the parent is filled in only when a triangle claims the element
// (InternalSetTriangle, :654-668). So an element that was appended and never assigned to a
// triangle passes IsElement and answers -1. GetVertex's own bounds test is a checkSlow, compiled
// out in Development, and TDynamicVector::operator[] takes a **uint32** (DynamicVector.h:176-180)
// - so -1 becomes 4294967295, block index 8388607, and the element read lands at
// (Index & 511) * sizeof(FVector3d) = 511 * 24 = 0x2FE8 off whatever that block pointer read
// returned. That constant is why six crash dumps in Saved/Crashes all read the identical address
// 0x0000000000002fe8: EXCEPTION_ACCESS_VIOLATION, inside a ParallelFor, no assert, editor gone.
// ---------------------------------------------------------------------------

// True when every ALLOCATED element of the mesh's primary normal overlay has a parent vertex that
// is a live vertex of the mesh - i.e. when the three space deformers can be handed this mesh.
// Cost is O(normal elements).
//
// Answers TRUE, not false, for a mesh with no attribute set and for one whose attribute set has
// no normal layer: neither has an element that could be unparented, and the null-layer case is a
// DIFFERENT crash (PrimaryNormals() returns nullptr and the deformers call MaxElementID() on it)
// that the caller has to answer separately. Null-safe (true on null).
bool MeshNormalParentsAreValid(UDynamicMesh* Mesh);

// Repair for the above, and the reason the deformers do not simply refuse. An allocated element
// with no parent is a LEGAL, engine-produced FDynamicMesh3 state, not corruption: the overlay API
// documents it as transient and names the cleanup itself - SetTriangle/UnsetTriangle with
// bAllowElementFreeing=false "should eventually be followed by a call to FreeUnusedElements()"
// (DynamicMeshOverlay.h:340,358) - and FDynamicMesh3::Copy(FMeshShapeGenerator*) leaves one behind
// for every entry of Generator->Normals that Generator->TriangleNormals never names
// (DynamicMesh3.cpp:174-186). Refusing it would refuse valid modelling input that no .pwmodel op
// can fix, so the deformers run the engine's own cleanup instead.
//
// The repair is lossless by construction. Every site that invalidates a parent also drops the
// element's last reference (DynamicMeshOverlay.cpp:300-302, :344, :599, :648, :1204), so
// {allocated AND unparented} is exactly {allocated AND referenced by no triangle} - which is
// precisely what FreeUnusedElements frees. No triangle can lose a normal it was using.
//
// Returns whether the mesh is safe AFTERWARDS; OutFreedElements is how many orphans were dropped
// (0 when the mesh was already clean, including when there was nothing to check). A false return
// means an element survived the sweep still unparented - a mesh no repair here can fix - and the
// caller must fail rather than call the engine op. Null-safe (true on null).
bool EnsureMeshNormalParentsAreValid(UDynamicMesh* Mesh, int32& OutFreedElements);

// ---------------------------------------------------------------------------
// Dirty ceremony for level DynamicMeshActors.
//
// Every geometry verb that edits a live ADynamicMeshActor used to mutate the mesh,
// call NotifyMeshUpdated() so the viewport refreshed, and mark NOTHING dirty. The
// mesh serializes fine when the package is actually saved (MeshObject is
// UPROPERTY(Instanced); UDynamicMesh::Serialize does `Ar << *Mesh`) — but nothing
// told the editor the level had changed, so `level.save` no-opped and the edit was
// silently lost when the editor closed. These two helpers are the single choke
// point that fixes that for the whole namespace.
//
// Deliberately NOT used: PreEditChange/PostEditChangeProperty. UActorComponent::
// PreEditChange flushes rendering commands on every call, which is unacceptable in
// verbs an agent runs in a loop.
//
// Deliberately NOT used: FScopedTransaction, and Modify() is never called on the
// UDynamicMesh. A transactional Modify() on the mesh serializes the whole
// FDynamicMesh3 into the undo buffer per call — at this module's own
// GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH ceiling that is tens of MB per verb, and it
// would evict unrelated user undos. Opening a transaction WITHOUT snapshotting the
// mesh would be worse still: Ctrl+Z would roll the actor back while leaving the
// edited mesh in place. Undo for geometry verbs needs incremental FMeshChange
// records and is a separate feature; this is a data-loss fix only.
// ---------------------------------------------------------------------------

// Commits an in-place edit to a level DynamicMeshActor: Modify() -> render refresh
// -> MarkPackageDirty(), the house ceremony (spline.set_spline_mesh_material,
// environment.create_procedural_terrain) with NotifyMeshUpdated() standing in for
// MarkRenderStateDirty() — NotifyMeshUpdated's ResetProxy() is the stronger form and
// is what this cluster has always used as its commit step.
//
// Dirties the OWNING ACTOR's package, not the world's. Under World Partition / OFPA
// the actor lives in its own external package, so dirtying the world package (what
// spline.* does) would mark the wrong one; UObject::MarkPackageDirty resolves the
// outermost package, which is correct in both layouts.
//
// bNotifyMesh=false skips the render refresh for edits that only touch the
// component's UBodySetup (the generate_collision family) — NotifyMeshUpdated() would
// otherwise force a full render-proxy rebuild for a collision-only change.
//
// Null-safe. Takes the COMPONENT, not the actor, because that is what is in scope at
// every commit site — and because the transient scratch meshes built with
// NewObject<UDynamicMesh>(GetTransientPackage()) in GeometryTransformHandler.cpp have
// no component and so cannot reach this function by construction.
void MarkGeometryActorModified(UDynamicMeshComponent* Component, bool bNotifyMesh = true);

// Commits a freshly spawned DynamicMeshActor. UWorld::SpawnActor only dirties the
// level when a transaction is open (`if (GUndo) ModifyLevel(LevelToSpawnIn);`,
// LevelActor.cpp:735-739 on 5.8 / :706-709 on 5.3) and these handlers open none, so
// a spawned actor otherwise leaves the level clean and vanishes on close. Dirties
// BOTH the actor's package and its level's: a spawn changes the level's actor list
// too, which under OFPA is a different package from the actor's own. On a non-WP map
// both resolve to the same UPackage and MarkPackageDirty() is idempotent. Null-safe.
void MarkGeometryActorSpawned(AActor* Actor);

} // namespace GeometryUtils
