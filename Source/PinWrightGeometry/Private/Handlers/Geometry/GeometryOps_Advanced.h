// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryOps_Advanced.h - The two topology verbs (bridge, edge_split), the three spline/profile
// verbs (loft, sweep, extrude_along_spline) and the bulk upload (append_buffers), as pure
// functions over a UDynamicMesh.
//
// Three of these six took their inputs from LEVEL ACTORS - loft read cross-section actors, sweep
// and extrude_along_spline read a USplineComponent off a named actor - which is precisely why
// nothing but the RPC handler could ever call them. The actor -> data adapter therefore lives in
// the wrapper and the ops take the extracted data: profile samples for loft, uniform spline
// samples for the two sweeps. Nothing here names an AActor, a USplineComponent or an
// FHandlerContext.
//
// geometry.duplicate_along_spline deliberately has no entry: it duplicates actors through
// UEditorActorSubsystem and never touches mesh data, so it is actor management, not an op.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/Geometry/GeometryOps.h"

class UDynamicMesh;

namespace GeometryOps
{

// ---------------------------------------------------------------------------
// bridge
// ---------------------------------------------------------------------------

struct FBridgeParams
{
    // Indices into the mesh's boundary-loop list. Both are clamped into range rather than
    // rejected, and a collision moves B to the next loop, so no index value can fail the op.
    // A clamped EdgeGroupA appends an FOpResult warning; EdgeGroupB deliberately does not,
    // because the collision nudge can move it again afterwards and a "clamped to N" note would
    // then name a loop the op did not use.
    int32 EdgeGroupA = 0;
    int32 EdgeGroupB = 1;
};

// geometry.bridge's published `subdivisions` parameter has NO field here. The strip builder
// never read it - it is accepted and echoed and nothing else - so giving it a struct field
// would publish a knob to the .pwmodel front-end that silently does nothing. The RPC wrapper
// keeps echoing it from its own Ctx read.

struct FBridgeOutputs
{
    // Which branch ran and what it stitched; the handler echoes this as bridgeStatus.
    FString Status;
    int32 TrianglesCreated = 0;
};

// Stitches two boundary loops together with a triangle strip. Fewer than two boundary loops
// falls back to filling holes, which is what this verb has always done instead of erroring, so
// Status is the only place a caller learns which of the two happened.
FOpResult Bridge(UDynamicMesh* Mesh, const FBridgeParams& Params, FBridgeOutputs& Out);

// ---------------------------------------------------------------------------
// edge_split
// ---------------------------------------------------------------------------

struct FEdgeSplitParams
{
    // Edge IDs to split. Ids that are not live edges are skipped silently, so an empty or
    // wholly stale list is a successful no-op rather than a failure.
    TArray<int32> EdgeIndices;

    // Position of the new vertex along the edge, 0 at A and 1 at B. Not clamped.
    double SplitFactor = 0.5;

    bool bWeldVertices = true;
    double WeldTolerance = 0.0001;
};

struct FEdgeSplitOutputs
{
    // Counts triangle REPLACEMENTS, not edges: an interior edge borders two triangles and
    // increments this twice. Named for the response field the handler has always sent.
    int32 EdgesSplit = 0;
};

// Splits by hand - RemoveTriangle plus two AppendTriangle through a new midpoint vertex -
// rather than through FDynamicMesh3::SplitEdge. That is why EdgesSplit can be non-zero while
// the triangle count does not move: RemoveTriangle drops any corner the removed triangle was
// the last user of, and a replacement naming a dropped corner is then rejected. Verbatim
// pre-extraction behaviour; both outcomes are pinned in TestGeometryOpsAdvanced.cpp.
FOpResult EdgeSplit(UDynamicMesh* Mesh, const FEdgeSplitParams& Params, FEdgeSplitOutputs& Out);

// ---------------------------------------------------------------------------
// loft
// ---------------------------------------------------------------------------

// One cross-section profile, reduced to the data the loft actually consumes. The verb does not
// trace a profile's outline - it takes the profile's world location and derives a circular
// cross-section from its bounding-box extent - so this is the whole actor->data adapter.
struct FLoftProfileSample
{
    FVector Location = FVector::ZeroVector;

    // Bounding-box extent of the profile's mesh. Meaningful only when bHasMesh.
    FVector Extent = FVector::ZeroVector;

    // False for a profile actor that carries no mesh. The loft runs only when BOTH the first
    // and last profile have one, matching the handler's guard.
    bool bHasMesh = false;
};

struct FLoftParams
{
    // Path step count. Clamped to [2,64] on the profile branch and to [2,32] on the
    // bounding-box branch, each reported as an FOpResult warning - the response echoes the
    // value the caller asked for, so the warning is the only record that it moved.
    int32 Subdivisions = 8;
    bool bSmooth = true;
    bool bCap = true;

    // True when the caller named profile actors at all - NOT whether any resolved. It separates
    // "no profiles requested" (sweep the mesh's own bounding box along Z) from "profiles
    // requested, none resolved" (do nothing), which produce different meshes.
    bool bUseProfiles = false;
};

struct FLoftOutputs
{
    int32 ProfilesUsed = 0;
};

FOpResult Loft(UDynamicMesh* Mesh, const FLoftParams& Params,
               const TArray<FLoftProfileSample>& Profiles, FLoftOutputs& Out);

// ---------------------------------------------------------------------------
// sweep / extrude_along_spline
// ---------------------------------------------------------------------------

// Number of path STEPS for a spline-driven sweep; the path is this many steps plus one sample.
// Shared so the wrapper's spline sampling loop and the op's reported step count cannot drift.
// Silent by design: the wrapper calls it to SIZE a buffer, not to rewrite a caller value.
// Sweep's own fallback clamps Steps against the same bounds through ClampRangeWarn, so the
// branch that actually rewrites what the caller asked for is the branch that reports it.
int32 SplinePathStepCount(int32 Steps);

// Everything sweep needs off the caller's spline actor.
struct FSweepPath
{
    // Uniform location+rotation samples along the spline - SplinePathStepCount(Steps) + 1 of
    // them, at equal distances. Twist and scale are NOT baked in: the op applies those so the
    // spline path and the linear fallback share one implementation. Fewer than two samples
    // selects the fallback.
    TArray<FTransform> Samples;

    float SplineLength = 0.0f;

    // The caller named a spline actor that carries no USplineComponent. Runs the linear
    // fallback but under its own status wording, which is what the handler has always sent.
    bool bActorHasNoSplineComponent = false;
};

// The 2D cross-section a sweep pushes along its path, in the path frame's YZ-equivalent plane -
// the same shape AppendSweepPolygon takes. Fewer than three vertices selects the op's own
// circular section, derived from the mesh's bounding box, which is the ONLY section the verb had
// before and is therefore what an empty list still produces. The engine call has always accepted
// an arbitrary polygon; narrowing it to a circle was the op's doing, not the engine's, and the
// .pwmodel front-end authors a profile literally (`profile=[(u, v), …]`) where the RPC front-end
// has no way to carry one. Nothing in the RPC path sets this, so that path is unchanged.
struct FSweepProfile
{
    TArray<FVector2D> Vertices;
};

// A cross-section scale LAW along the path, as (alpha, scale) knots with alpha the normalised
// position from the first frame (0) to the last (1). Piecewise-linear between knots and HELD
// outside the outermost pair, so a curve that does not start at 0 or reach 1 still answers
// everywhere rather than extrapolating past what the author wrote.
//
// WHY THIS EXISTS AT ALL. Both ops build one FTransform per frame and had only
// Lerp(ScaleStart, ScaleEnd, alpha) to put in its scale slot, so a swept tube could taper only
// STRAIGHT. Every radius law that is not a straight line - a power-law trunk taper, an
// exponential flare, a bulge - was therefore unreachable along a path, and the documented way
// round it was to split the shape into one sweep per span and match the radii at each join by
// hand. That is more geometry, more caps buried inside each other, and one arithmetic slip away
// from a visible ledge at every joint. The engine never required any of it: the scale slot has
// always been per-frame, and only the caller's two scalars were not.
//
// Fewer than two knots is NOT a curve and selects the ScaleStart -> ScaleEnd ramp, which is what
// both ops did before and what every document that does not mention this still gets.
struct FSweepScaleCurve
{
    // (alpha, scale). Callers are expected to have validated alpha strictly ascending within
    // [0, 1] and every scale positive; Evaluate is defensive but reports nothing.
    TArray<FVector2D> Knots;

    bool IsSet() const { return Knots.Num() >= 2; }

    // Piecewise-linear, clamped at both ends. Returns 1.0 on an unset curve so a caller that
    // forgets IsSet() gets the identity rather than a zero-scale collapse.
    double Evaluate(double Alpha) const;
};

struct FSweepParams
{
    int32 Steps = 16;
    double Twist = 0.0;
    double ScaleStart = 1.0;
    double ScaleEnd = 1.0;
    bool bCap = true;

    // Empty: the bounding-box circle described on FSweepProfile.
    FSweepProfile Profile;

    // Set: supersedes ScaleStart / ScaleEnd entirely. See FSweepScaleCurve. Nothing in the RPC
    // path sets this, so that path is unchanged - the same arrangement Profile has.
    FSweepScaleCurve ScaleCurve;
};

struct FSweepOutputs
{
    // Which path was swept and how; the handler echoes this as sweepStatus.
    FString Status;
    int32 PathSteps = 0;
    int32 ProfileVertices = 0;
};

// Sweeps Params.Profile - or, when that is empty, a circular cross-section sized from the mesh's
// own bounding box and NOT from the mesh's outline - along Path, or along a vertical line through
// the mesh when Path carries no samples. The swept surface is APPENDED to Mesh; the geometry that
// sized the section is still there afterwards.
//
// WARNS on a degenerate path and builds it anyway. The cross-section lands in each frame's local
// Y-Z plane and the sweep advances along the frame's local +X, so a frame whose +X is
// PERPENDICULAR to the segment leaving it slides the section along inside its own plane and
// sweeps no volume. That failure is otherwise invisible: the result reports success, isClosed
// true and zero boundary edges at the same triangle count a correct sweep gives, differing only
// in a degenerate-triangle count and a bounding box with no thickness. Frames left at rotation
// (0,0,0) on a path that runs along Z are the common way in. Both warnings lead with `path`.
//
// The VERTICAL FALLBACK used to be an instance of that same defect, and is fixed rather than
// warned about: its frames were rotations about world Z, which left their local +X across the
// direction of travel, so the branch geometry.sweep runs when no spline resolves swept a flat
// ribbon. It now orients each frame's +X along the path. `steps` sizes that fallback path and is
// the one value this op clamps.
FOpResult Sweep(UDynamicMesh* Mesh, const FSweepParams& Params, const FSweepPath& Path,
                FSweepOutputs& Out);

struct FExtrudeAlongSplineParams
{
    int32 Segments = 16;
    bool bCap = true;
    double ScaleStart = 1.0;
    double ScaleEnd = 1.0;
    double Twist = 0.0;

    // Empty: the bounding-box circle described on FSweepProfile.
    FSweepProfile Profile;

    // Set: supersedes ScaleStart / ScaleEnd entirely. See FSweepScaleCurve.
    FSweepScaleCurve ScaleCurve;
};

// Same cross-section construction as Sweep - Params.Profile, else the bounding-box circle - and
// there is no fallback path: with no samples the engine call runs against an empty frame list.
// PathSamples carry location+rotation only, as for Sweep, and the same two `path` warnings apply.
//
// OPEN OR CLOSED IS READ OFF THE PATH, not fixed. A path whose last frame returns to the first
// (within 0.01 uu, four frames minimum) is swept as a closed loop: the repeated frame is dropped,
// the engine wraps the ring itself, and `cap` / `scale_start` / `scale_end` do nothing because a
// loop has no ends - which the op says in a warning rather than leaving to be discovered. Every
// other path is swept OPEN and honours all three.
//
// This used to be an unconditional closed loop, which made `cap` unreachable at every path
// length and turned an open path into a tube that ran back from its last frame to its first.
FOpResult ExtrudeAlongSpline(UDynamicMesh* Mesh, const FExtrudeAlongSplineParams& Params,
                             const TArray<FTransform>& PathSamples);

// ---------------------------------------------------------------------------
// append_buffers
// ---------------------------------------------------------------------------

struct FAppendBuffersParams
{
    // Mesh-local positions. Required and non-empty.
    TArray<FVector> Vertices;

    // Index triples into Vertices, CCW facing outward. May be empty to append loose vertices.
    TArray<FIntVector> Triangles;

    // Optional per-vertex attributes. Each must either be empty or carry exactly Vertices.Num()
    // entries; a partial array is a rejection, never a silent pad.
    TArray<FVector> Normals;
    TArray<FVector2D> UVs;
    TArray<FLinearColor> Colors;

    // Applied to every appended triangle in the batch.
    int32 GroupId = 0;
    int32 MaterialId = 0;
};

struct FAppendBuffersOutputs
{
    int32 AppendedVertices = 0;
    int32 AppendedTriangles = 0;
};

// The three data rules append_buffers rejects on, exposed individually so the RPC wrapper can
// apply each at the exact point in its JSON parse where it has always applied it - on the raw
// `vertices` array before a single element is parsed, inside the triangle loop, and before an
// optional array's elements are parsed. A payload carrying several faults therefore still
// reports the same one it reported before extraction. The messages live here once and NEITHER
// front-end spells them out: BulkEditHandler used to carry its own copy of the empty-vertices
// sentence, which is the way two copies of one wire message start drifting.
FOpResult ValidateAppendBufferVertexCount(int32 VertexCount);
FOpResult ValidateAppendBufferTriangle(int32 TriangleIndex, const FIntVector& Triangle,
                                       int32 VertexCount);
FOpResult ValidateAppendBufferAttributeCount(const TCHAR* AttributeName, int32 AttributeCount,
                                             int32 VertexCount);

// Every rule the three above enforce, over already-parsed data. AppendBuffers calls this itself,
// so a front-end that has no incremental parse to interleave with needs only the op.
FOpResult ValidateAppendBuffers(const FAppendBuffersParams& Params);

// Params is a SINK - taken by value so a caller holding freshly parsed buffers can MoveTemp them
// straight into the engine's buffer struct. This verb exists to avoid per-element round-trips,
// and taking a const& would have added a full copy of the payload to every bulk upload.
//
// Preserves the TARGET mesh's UV layers. The engine's AppendBuffersToMesh sets the target's UV
// layer count to whatever the INCOMING buffers carry, so appending UV-less geometry to a mesh
// that has UVs deletes the UVs off the geometry that was already there. This op pads the
// incoming buffers with placeholder (0,0) UV sets so that cannot happen, and warns when it did
// - see the comment on the definition. The RPC response shape is unaffected: the wrapper does
// not surface Warnings.
FOpResult AppendBuffers(UDynamicMesh* Mesh, FAppendBuffersParams Params,
                        FAppendBuffersOutputs& Out);

} // namespace GeometryOps
