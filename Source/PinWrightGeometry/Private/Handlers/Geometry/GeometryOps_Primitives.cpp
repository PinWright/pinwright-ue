// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryOps_Primitives.cpp - see GeometryOps_Primitives.h for why these are free functions
// over a UDynamicMesh rather than handler bodies.
//
// The null guard, the before/after count snapshot and the clamp-and-warn pair are
// GeometryOps.h's, shared with the other four families: this file no longer carries a private
// copy of any of them.
#include "Handlers/Geometry/GeometryOps_Primitives.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryClampDomains.h"
#include "Handlers/Geometry/GeometryUtils.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/ScopeExit.h"
#include "UDynamicMesh.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif

#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshBooleanFunctions.h"

namespace
{
    // Uniquely prefixed: this module builds with bUseUnity = true, so a plainly-named
    // anonymous-namespace helper would collide with a sibling TU once Unity merges them. Every
    // sibling file prefixes for the same reason (GeometryOpsBoolean_*, GeometryOpsAdvanced_*).

    // The revolve generators close their sweep at EXACTLY 360 degrees and nowhere else:
    // TotalRevolutionDegrees = Clamp(RevolveDegrees, 0.1, 360) and then
    // bSweepCurveIsClosed = (TotalRevolutionDegrees == 360) (RevolveGenerator.cpp:162/:181 for
    // the polygon form, :351/:370 for the path form). Anything at or above 360 therefore closes;
    // anything below it does not.
    constexpr double GeometryOpsPrimitives_ClosedRevolveDegrees = 360.0;

    // The step floor for a revolve, which is NOT one number.
    //
    // Both generators take Steps = Max(Steps, 2), and 2 is CORRECT for a partial sweep -
    // `arch major_steps=2 angle=180` is a sound half-arch with a real span between its two
    // sections. On a sweep that closes, the last cross-section wraps back onto the first
    // (NumSweepFrames = bSweepCurveIsClosed ? Steps : Steps + 1, RevolveGenerator.cpp:184), so
    // 2 steps leave two sections joined to each other twice and no interior at all. Measured on
    // 5.8: `torus major_segments=2` gives 16 triangles with 16 boundary edges and isClosed
    // false; `revolve steps=2 angle=360` gives 8 triangles, 10 boundary edges and 6 degenerate
    // triangles. Three is the first value that closes either - 48 and 24 triangles, 0 boundary
    // edges - which is why the floor is conditional on the sweep angle rather than a flat bump.
    //
    // Both degenerate cases used to answer success with no warning at all: nothing was clamped,
    // because 2 was inside the published range. The only diagnostic was PWMODEL_MESH_NOT_CLOSED
    // one stage later, which names the symptom rather than the parameter that produced it.
    int32 GeometryOpsPrimitives_RevolveStepFloor(double RevolveDegrees)
    {
        return (RevolveDegrees >= GeometryOpsPrimitives_ClosedRevolveDegrees) ? 3 : 2;
    }

    // How close the two ends of a profile have to be before the profile is read as a CLOSED
    // SECTION rather than an open path. The repeat is written by hand - a ring's author types
    // the first point again as the last - so any tolerance at all would do; this one exists only
    // to absorb the decimal text round-trip, and is far below the scale at which two DIFFERENT
    // profile points would ever be authored.
    constexpr double GeometryOpsPrimitives_ClosedProfileTolerance = 1e-4;

    // A profile whose first and last points coincide is a closed section loop, and it must NOT
    // be sent through AppendRevolvePath.
    //
    // FRevolvePlanarPathGenerator's ONLY route to a closed profile curve is its capping branch:
    // with bCapped it appends the projections of the last and first points onto the revolve axis
    // and then sets bProfileCurveIsClosed (RevolveGenerator.cpp:139-158), and with bCapped false
    // it leaves the curve open. So a ring's section comes out either
    //   - capped: two triangle fans spanning the bore, one from each repeated endpoint to the
    //     axis. They are coincident and oppositely wound, so they cancel EXACTLY in signedVolume
    //     while adding a solid membrane across the hole - and isClosed, boundaryEdges,
    //     orientationConsistent and signedVolume all still read clean; or
    //   - uncapped: two separate vertex rings at the seam, an open surface the seam was never
    //     welded across.
    // Neither is the ring that was asked for. FRevolvePlanarPolygonGenerator is the generator
    // that sweeps a closed profile - it sets bProfileCurveIsClosed unconditionally and adds no
    // axis points at all (RevolveGenerator.cpp:341) - and its engine contract says the endpoint
    // is NOT repeated (MeshPrimitiveFunctions.h, AppendRevolvePolygon), which is why the repeat
    // is dropped before the call. GeneratePipe above already builds its annulus this way.
    //
    // Four points minimum: AppendRevolvePolygon rejects fewer than three, and the repeat is the
    // fourth. A three-point "loop" is two distinct points and has no section to sweep, so it is
    // left on the path generator rather than being rejected here.
    bool GeometryOpsPrimitives_ProfileIsClosedSection(const TArray<FVector2D>& Profile)
    {
        return Profile.Num() >= 4
            && FVector2D::Distance(Profile[0], Profile.Last())
                <= GeometryOpsPrimitives_ClosedProfileTolerance;
    }

    // RevolveUtil::WeldPointsOnAxis' own tolerance (RevolveGenerator.cpp, 0.1): a profile point
    // inside this radius is welded onto the axis, and the "cap" from it is a zero-area fan.
    constexpr double GeometryOpsPrimitives_AxisWeldRadius = 0.1;

    // The closed section written WITHOUT the repeated endpoint - the spelling this op cannot
    // rescue, only name.
    //
    // A section loop left implicit reaches the path generator as an open path, and its two axis
    // caps are then taken at the same height (the seam's two ends are adjacent on the loop) and
    // from radii both off the axis. Two coincident, oppositely wound fans: no enclosed volume,
    // so signedVolume is the correct annulus figure, and a solid membrane across the bore that
    // isClosed, boundaryEdges, orientationConsistent and degenerateTriangles cannot see either.
    // The triangle count cannot separate it - 2 * (N-1) * Steps for the closed spelling equals
    // (N-2) quad edges * 2 * Steps plus two Steps-triangle fans - so nothing in any response
    // distinguishes it from the ring that was wanted.
    //
    // It is NOT auto-corrected, because it is genuinely ambiguous: a lathed silhouette that
    // happens to start and end at one height is a legitimate solid, and welding its seam would
    // hollow it. The author is told instead, and the remedy is one character - repeat the first
    // point as the last, which the branch above then handles.
    //
    // The predicate is deliberately narrow, so it cannot fire on an ordinary lathe: a vase whose
    // ends are at DIFFERENT heights gets two real caps at two real heights, encloses volume, and
    // is not warned about.
    bool GeometryOpsPrimitives_AxisCapsWouldBeAMembrane(const TArray<FVector2D>& Profile)
    {
        return Profile.Num() >= 3
            && FMath::Abs(Profile[0].X) > GeometryOpsPrimitives_AxisWeldRadius
            && FMath::Abs(Profile.Last().X) > GeometryOpsPrimitives_AxisWeldRadius
            && FMath::Abs(Profile[0].Y - Profile.Last().Y)
                <= GeometryOpsPrimitives_ClosedProfileTolerance;
    }
}

namespace GeometryOps
{
namespace ClampDomains
{

// The declared domain of every clamp in this file. See GeometryClampDomains.h for why a clamped
// domain cannot travel in a parameter table's enforced min/max pair, and for the rule that a new
// clamp adds a row here in the same commit.
//
// Every number below is the one at the call site a few hundred lines down, and the test that
// keeps them equal does it by RUNNING each op rather than by reading this file - so a row that
// drifts is caught by the mesh, not by a second opinion about the source.
TArrayView<const FClampDomain> Entries()
{
    // Columns: method, label, floor(open sweep), floor(closed sweep), ceiling,
    //          0-means-unset, default, op's own default sweep degrees.
    static const FClampDomain Table[] =
    {
        // AppendBox floors its per-edge counts at 0 itself, so 0 is a legal "no subdivision"
        // value and there is no unset tier to read it as something else.
        { TEXT("geometry.create_box"),           TEXT("widthSegments"),      0, 0, GEOM_MAX_SEGMENTS, false,  0,   0.0 },
        { TEXT("geometry.create_box"),           TEXT("heightSegments"),     0, 0, GEOM_MAX_SEGMENTS, false,  0,   0.0 },
        { TEXT("geometry.create_box"),           TEXT("depthSegments"),      0, 0, GEOM_MAX_SEGMENTS, false,  0,   0.0 },

        // AppendSphereBox's real floor is 2: at 1 and 2 it emits the same mesh.
        { TEXT("geometry.create_sphere"),        TEXT("subdivisions"),       2, 2, GEOM_MAX_SEGMENTS, true,  16,   0.0 },

        { TEXT("geometry.create_cylinder"),      TEXT("segments"),           3, 3, GEOM_MAX_SEGMENTS, true,  16,   0.0 },
        { TEXT("geometry.create_cylinder"),      TEXT("heightSteps"),        0, 0, GEOM_MAX_SEGMENTS, false,  0,   0.0 },

        { TEXT("geometry.create_cone"),          TEXT("segments"),           3, 3, GEOM_MAX_SEGMENTS, true,  16,   0.0 },
        { TEXT("geometry.create_cone"),          TEXT("heightSteps"),        0, 0, GEOM_MAX_SEGMENTS, false,  0,   0.0 },

        // The cap steps a half-arc profile rather than a closed loop, so its floor is 2 where
        // every closed radial count's is 3.
        { TEXT("geometry.create_capsule"),       TEXT("hemisphereSteps"),    2, 2, GEOM_MAX_SEGMENTS, true,   4,   0.0 },
        { TEXT("geometry.create_capsule"),       TEXT("segments"),           3, 3, GEOM_MAX_SEGMENTS, true,  16,   0.0 },

        // A torus has no angle parameter, so its sweep ALWAYS closes and its major floor is the
        // closed one unconditionally. `arch` is the same engine call with the angle exposed.
        { TEXT("geometry.create_torus"),         TEXT("majorSegments"),      3, 3, GEOM_MAX_SEGMENTS, true,  16,   0.0 },
        { TEXT("geometry.create_torus"),         TEXT("minorSegments"),      3, 3, GEOM_MAX_SEGMENTS, true,   8,   0.0 },

        { TEXT("geometry.create_plane"),         TEXT("widthSubdivisions"),  0, 0, GEOM_MAX_SEGMENTS, false,  0,   0.0 },
        { TEXT("geometry.create_plane"),         TEXT("depthSubdivisions"),  0, 0, GEOM_MAX_SEGMENTS, false,  0,   0.0 },

        { TEXT("geometry.create_disc"),          TEXT("segments"),           3, 3, GEOM_MAX_SEGMENTS, true,  16,   0.0 },

        // A step count is not a segment count: the ceiling is the measured stair limit, not the
        // radial one, and the floor is 1 because one step is a staircase.
        { TEXT("geometry.create_stairs"),        TEXT("numSteps"),           1, 1, GEOM_MAX_STAIR_STEPS, true, 8,  0.0 },
        { TEXT("geometry.create_spiral_stairs"), TEXT("numSteps"),           1, 1, GEOM_MAX_STAIR_STEPS, true, 8,  0.0 },

        { TEXT("geometry.create_ring"),          TEXT("segments"),           3, 3, GEOM_MAX_SEGMENTS, true,  32,   0.0 },

        // The two conditional rows. `arch` defaults to a partial sweep, so an author who passes
        // no angle meets the OPEN floor of 2; at angle 360 the sweep closes and the floor is 3.
        { TEXT("geometry.create_arch"),          TEXT("majorSteps"),         2, 3, GEOM_MAX_SEGMENTS, true,  16, 180.0 },
        { TEXT("geometry.create_arch"),          TEXT("minorSteps"),         3, 3, GEOM_MAX_SEGMENTS, true,   8,   0.0 },

        { TEXT("geometry.create_pipe"),          TEXT("radialSteps"),        3, 3, GEOM_MAX_SEGMENTS, true,  24,   0.0 },
        { TEXT("geometry.create_pipe"),          TEXT("heightSteps"),        0, 0, GEOM_MAX_SEGMENTS, false,  0,   0.0 },

        // `revolve` is the same conditional rule as `arch` with the opposite default: its angle
        // defaults to 360, so an author who passes no angle meets the CLOSED floor of 3.
        { TEXT("geometry.revolve"),              TEXT("steps"),              2, 3, GEOM_MAX_SEGMENTS, true,  16, 360.0 },
    };
    return MakeArrayView(Table);
}

const FClampDomain* Find(const FString& RpcMethod, const FString& Label)
{
    for (const FClampDomain& Domain : Entries())
    {
        if (RpcMethod.Equals(Domain.RpcMethod) && Label.Equals(Domain.Label))
        {
            return &Domain;
        }
    }
    return nullptr;
}

} // namespace ClampDomains

FOpResult GenerateBox(UDynamicMesh* TargetMesh, FBoxParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    // Labels are the RPC's PUBLISHED parameter names, not this struct's field names. A caller
    // greps the warning text for the name it passed, and `size.x` / `steps.x` are spelled
    // nowhere on the wire: geometry.create_box publishes width/height/depth and
    // widthSegments/heightSegments/depthSegments (PrimitiveHandler.cpp). Every other op in this
    // file already labels with its RPC name; these six were the only holdouts, and no test
    // pinned them, which is how they drifted.
    //
    // `.pwmodel` spells the same two as a Vector3 `size` and a Vector3 `segments`, and this used
    // to be recorded here as an accepted single-surface compromise - the document's author read
    // a warning about `widthSegments`, a parameter their front-end does not have. It is no longer
    // a compromise: the ops layer stays the RPC's and knows nothing about its callers, and the
    // compiler rewrites the label on re-emission from PwModelWarningNames (Model/PwModelParser.h,
    // where the whole table and its reasoning live). A NEW clamp label here that `.pwmodel`
    // spells differently needs a row there - TestPwModelWarningNames fails on a missing one.
    Params.Size.X = ClampDimensionWarn(Params.Size.X, TEXT("width"), Result);
    Params.Size.Y = ClampDimensionWarn(Params.Size.Y, TEXT("height"), Result);
    Params.Size.Z = ClampDimensionWarn(Params.Size.Z, TEXT("depth"), Result);

    // A bare ClampRangeWarn, NOT the ClampSegmentsWarn/ClampRangeWarn pair the radial counts
    // use - the same choice GeneratePlane makes below, and for the same reason. AppendBox floors
    // these at 0 itself (`GridBoxGenerator.EdgeVertices = FIndex3i(FMath::Max(0, StepsX), ...)`,
    // MeshPrimitiveFunctions.cpp:236), exactly as AppendRectangleXY does, so 0 is a legal value
    // meaning "no subdivision" - while the pair reads <= 0 as "unset, substitute the verb's
    // default" and turned a deliberate 0 into 1.
    //
    // What that substitution actually cost is a false WARNING, not geometry: FGridBoxMeshGenerator
    // opens with `N.A = FMath::Max(2, N.A)` on a count that is VERTICES PER EDGE, not quads
    // (GridBoxMeshGenerator.h), so 0, 1 and 2 all build the same unsubdivided 12-triangle cube and
    // only 3 produces a second quad along an edge. The old clamp therefore reported
    // `steps.x clamped from 0 to 1` for a box it had not changed - a warning about a value the
    // caller was entitled to pass, naming a parameter the caller could not have written.
    // docs/pwmodel-format.md carries the same correction; its floor table used to claim this
    // path "silently subdivides once", which the generator's Max(2, N) rules out.
    Params.Steps.X = ClampRangeWarn(Params.Steps.X, 0, GEOM_MAX_SEGMENTS,
        TEXT("widthSegments"), Result);
    Params.Steps.Y = ClampRangeWarn(Params.Steps.Y, 0, GEOM_MAX_SEGMENTS,
        TEXT("heightSegments"), Result);
    Params.Steps.Z = ClampRangeWarn(Params.Steps.Z, 0, GEOM_MAX_SEGMENTS,
        TEXT("depthSegments"), Result);

    FGeometryScriptPrimitiveOptions Options;
    // Written out because geometry.bevel and the face-selection ops work on polygroup edges and
    // a box without them is unbevelable - NOT because the box is special. PerFace is already the
    // engine's default for this struct (MeshPrimitiveFunctions.h:59), so this line changes
    // nothing and every generator in this file gets PerFace whether it says so or not. What it
    // MEANS is per-generator: FGridBoxGenerator reads it as one group per box face, and the
    // revolve generators behind torus/arch/revolve do not read it at all and emit one group per
    // QUAD instead (SweepGenerator.cpp, PolygonId = SweepIndex * NumProfileSegments +
    // ProfileIndex). This comment used to claim per-face polygroups were "unique to the box
    // among these generators", which read as "the others have none" and is the opposite of true.
    Options.PolygroupMode = EGeometryScriptPrimitivePolygroupMode::PerFace;

    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        TargetMesh, Options, LocalTransform,
        Params.Size.X, Params.Size.Y, Params.Size.Z,
        Params.Steps.X, Params.Steps.Y, Params.Steps.Z,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);

    FinishOp(TargetMesh, Result);
    return Result;
}

FOpResult GenerateSphere(UDynamicMesh* TargetMesh, FSphereParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    Params.Subdivisions = ClampSegmentsWarn(Params.Subdivisions, 16, TEXT("subdivisions"), Result);

    // AppendSphereBox's real floor is 2, not ClampSegments' 1. At subdivisions=1 and =2 it emits
    // the identical 12-triangle / 8-vertex cube - measured, both values, same document - so 1 is
    // accepted, silently does nothing, and leaves the author looking for a bug in the next op.
    // ClampSegments cannot express this: it only substitutes the default for <= 0, so 1 passes
    // through unchanged and unwarned. The second clamp is what reports it.
    Params.Subdivisions = ClampRangeWarn(Params.Subdivisions, 2, GEOM_MAX_SEGMENTS,
        TEXT("subdivisions"), Result);

    FGeometryScriptPrimitiveOptions Options;

    // AppendSphereBox, NOT AppendSphereLatLong: this primitive is a ROUNDED CUBE - six subdivided
    // quad faces projected onto the sphere - with no poles and no seam, and its triangle count is
    // 12*(N-1)^2 (measured: N=2 -> 12, 3 -> 48, 4 -> 108, 5 -> 192, 16 -> 2700). A recipe written
    // against append_sphere_lat_long ports to a DIFFERENT polyhedron AND a different triangle
    // count at the same nominal `subdivisions`. Documented rather than swapped: switching to
    // lat-long would silently change the geometry of every model that already uses `sphere`.
    //
    // AND AT THE FLOOR, `Radius` IS NOT THE HALF-EXTENT. EdgeVertices counts VERTICES per cube
    // edge (MeshPrimitiveFunctions.cpp:379-382), so N=2 leaves only the 8 cube corners, and
    // FBoxSphereGenerator projects a corner to sqrt(1/3) per axis before scaling by Radius
    // (Generators/BoxSphereGenerator.h:53,69) - a half-extent of Radius/sqrt(3) = 0.5774*Radius
    // on every axis, 42% under the number the caller wrote. From N=3 up, edge and face-centre
    // vertices reach Radius and the box matches. Not warned - every sphere at the floor would
    // warn, and the floor is the cheap primitive callers ask for on purpose - but published on
    // the `subdivisions` parameter text (PwModelParser.cpp, the `sphere` op) so an author placing
    // geometry against `radius=` is told before, not by a detached part after.
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereBox(
        TargetMesh, Options, LocalTransform, Params.Radius,
        Params.Subdivisions, Params.Subdivisions, Params.Subdivisions,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);

    FinishOp(TargetMesh, Result);
    return Result;
}

FOpResult GenerateCylinder(UDynamicMesh* TargetMesh, FCylinderParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    // AppendCylinder floors its own counts and reports neither: AngleSamples = Max(3, RadialSteps)
    // and LengthSamples = Max(0, HeightSteps) (MeshPrimitiveFunctions.cpp:658-659), which is why
    // segments=0, 1, 2 and 3 all returned the same 18-triangle prism, successfully, with no
    // diagnostic. Same pair as the sphere: ClampSegmentsWarn substitutes the verb's default for
    // <= 0 and caps the top, ClampRangeWarn catches what is above 0 but under the engine floor.
    Params.RadialSteps = ClampSegmentsWarn(Params.RadialSteps, 16, TEXT("segments"), Result);
    Params.RadialSteps = ClampRangeWarn(Params.RadialSteps, 3, GEOM_MAX_SEGMENTS,
        TEXT("segments"), Result);

    // 0 is a LEGAL height step count - no intermediate side-wall loop - so it keeps its meaning
    // and only the negatives and the oversized values move.
    Params.HeightSteps = ClampRangeWarn(Params.HeightSteps, 0, GEOM_MAX_SEGMENTS,
        TEXT("heightSteps"), Result);

    FGeometryScriptPrimitiveOptions Options;

    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
        TargetMesh, Options, LocalTransform, Params.Radius, Params.Height,
        Params.RadialSteps, Params.HeightSteps, true,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);

    FinishOp(TargetMesh, Result);
    return Result;
}

FOpResult GenerateCone(UDynamicMesh* TargetMesh, FConeParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    // AppendCone builds the same FCylinderGenerator as AppendCylinder and floors it identically -
    // Max(3, RadialSteps) / Max(0, HeightSteps), MeshPrimitiveFunctions.cpp:695-696.
    Params.RadialSteps = ClampSegmentsWarn(Params.RadialSteps, 16, TEXT("segments"), Result);
    Params.RadialSteps = ClampRangeWarn(Params.RadialSteps, 3, GEOM_MAX_SEGMENTS,
        TEXT("segments"), Result);
    Params.HeightSteps = ClampRangeWarn(Params.HeightSteps, 0, GEOM_MAX_SEGMENTS,
        TEXT("heightSteps"), Result);

    FGeometryScriptPrimitiveOptions Options;

    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCone(
        TargetMesh, Options, LocalTransform, Params.BaseRadius, Params.TopRadius, Params.Height,
        Params.RadialSteps, Params.HeightSteps, true,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);

    FinishOp(TargetMesh, Result);
    return Result;
}

FOpResult GenerateCapsule(UDynamicMesh* TargetMesh, FCapsuleParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    // The two counts have DIFFERENT engine floors, so they cannot share one clamp:
    // NumHemisphereArcSteps = Max(2, HemisphereSteps) and NumCircleSteps = Max(3, CircleSteps)
    // (MeshPrimitiveFunctions.cpp:418-419). The arc is a half-profile, not a closed loop, which
    // is why 2 is enough for it and 3 is the minimum for the radial ring.
    Params.HemisphereSteps = ClampSegmentsWarn(Params.HemisphereSteps, 4, TEXT("hemisphereSteps"), Result);
    Params.HemisphereSteps = ClampRangeWarn(Params.HemisphereSteps, 2, GEOM_MAX_SEGMENTS,
        TEXT("hemisphereSteps"), Result);
    Params.RadialSteps = ClampSegmentsWarn(Params.RadialSteps, 16, TEXT("segments"), Result);
    Params.RadialSteps = ClampRangeWarn(Params.RadialSteps, 3, GEOM_MAX_SEGMENTS,
        TEXT("segments"), Result);

    FGeometryScriptPrimitiveOptions Options;

#if UE_VERSION_OLDER_THAN(5,5,0)
    // UE 5.4: AppendCapsule does not have the SegmentSteps parameter
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCapsule(
        TargetMesh, Options, LocalTransform, Params.Radius, Params.Length,
        Params.HemisphereSteps, Params.RadialSteps,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
#else
    // UE 5.5+: AppendCapsule gains SegmentSteps parameter before Origin
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCapsule(
        TargetMesh, Options, LocalTransform, Params.Radius, Params.Length,
        Params.HemisphereSteps, Params.RadialSteps,
        1,  // SegmentSteps
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
#endif

    FinishOp(TargetMesh, Result);
    return Result;
}

FOpResult GenerateTorus(UDynamicMesh* TargetMesh, FTorusParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    // The two floors are NOT the same, and neither is the engine's own pair. AppendTorus builds
    // the minor profile as FPolygon2d::MakeCircle(..., Max(3, MinorSteps)) - a closed loop,
    // floor 3 - and then hands MajorSteps to AppendRevolvePolygon, whose
    // FRevolvePlanarPolygonGenerator takes Steps = Max(Steps, 2) (MeshPrimitiveFunctions.cpp:720
    // and :757). That 2-and-3 pair is exactly the 6 triangles measured at major=minor=0.
    //
    // The major floor here is 3, NOT the engine's 2: a torus passes a default
    // FGeometryScriptRevolveOptions, whose RevolveDegrees is 360 (MeshPrimitiveFunctions.h:79),
    // so this sweep ALWAYS closes and 2 cannot close it. See
    // GeometryOpsPrimitives_RevolveStepFloor for the measurements. `arch` is the same engine
    // call with the angle exposed, and it takes the conditional floor rather than this one.
    Params.MajorSteps = ClampSegmentsWarn(Params.MajorSteps, 16, TEXT("majorSegments"), Result);
    Params.MajorSteps = ClampRangeWarn(Params.MajorSteps,
        GeometryOpsPrimitives_RevolveStepFloor(GeometryOpsPrimitives_ClosedRevolveDegrees),
        GEOM_MAX_SEGMENTS, TEXT("majorSegments"), Result);
    Params.MinorSteps = ClampSegmentsWarn(Params.MinorSteps, 8, TEXT("minorSegments"), Result);
    Params.MinorSteps = ClampRangeWarn(Params.MinorSteps, 3, GEOM_MAX_SEGMENTS,
        TEXT("minorSegments"), Result);

    FGeometryScriptPrimitiveOptions Options;

    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendTorus(
        TargetMesh, Options, LocalTransform, FGeometryScriptRevolveOptions(),
        Params.MajorRadius, Params.MinorRadius, Params.MajorSteps, Params.MinorSteps,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);

    FinishOp(TargetMesh, Result);
    return Result;
}

FOpResult GeneratePlane(UDynamicMesh* TargetMesh, FPlaneParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    // AppendRectangleXY floors both counts silently - WidthVertexCount = Max(0, StepsWidth) and
    // HeightVertexCount = Max(0, StepsHeight) (MeshPrimitiveFunctions.cpp:1184-1185) - and had
    // NO upper bound at all, so widthSubdivisions=1000000 was an unguarded allocation on the
    // one code path GEOM_MAX_SEGMENTS exists to guard.
    //
    // A bare ClampRangeWarn, NOT the ClampSegmentsWarn/ClampRangeWarn pair the radial counts
    // use: 0 is a LEGAL subdivision count here (a single quad, no interior loop), and the pair
    // reads <= 0 as "unset, substitute the verb's default", which would silently turn a
    // deliberate 0 into 1. Same reasoning, same shape as the cylinder's heightSteps.
    Params.Steps.X = ClampRangeWarn(Params.Steps.X, 0, GEOM_MAX_SEGMENTS,
        TEXT("widthSubdivisions"), Result);
    Params.Steps.Y = ClampRangeWarn(Params.Steps.Y, 0, GEOM_MAX_SEGMENTS,
        TEXT("depthSubdivisions"), Result);

    FGeometryScriptPrimitiveOptions Options;

    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendRectangleXY(
        TargetMesh, Options, LocalTransform,
        Params.Size.X, Params.Size.Y, Params.Steps.X, Params.Steps.Y, nullptr);

    FinishOp(TargetMesh, Result);
    return Result;
}

FOpResult GenerateDisc(UDynamicMesh* TargetMesh, FDiscParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    // AppendDisc's AngleSamples = Max(3, AngleSteps) (MeshPrimitiveFunctions.cpp:1295): a disc
    // below 3 is the same triangle, drawn silently.
    Params.AngleSteps = ClampSegmentsWarn(Params.AngleSteps, 16, TEXT("segments"), Result);
    Params.AngleSteps = ClampRangeWarn(Params.AngleSteps, 3, GEOM_MAX_SEGMENTS,
        TEXT("segments"), Result);

    FGeometryScriptPrimitiveOptions Options;

    // SpokeSteps 1 and HoleRadius 0 - a solid disc. GenerateRing is the same engine call with
    // SpokeSteps 0 and a real hole radius; the two spoke counts are what the verbs shipped with.
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendDisc(
        TargetMesh, Options, LocalTransform, Params.Radius, Params.AngleSteps, 1,
        0.0f, 360.0f, 0.0f, nullptr);

    FinishOp(TargetMesh, Result);
    return Result;
}

FOpResult GenerateStairs(UDynamicMesh* TargetMesh, FStairsParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    // AppendLinearStairs floors NumSteps at 1 and reports nothing
    // (MeshPrimitiveFunctions.cpp:1484). This is the one verb where the silence was an outright
    // LIE rather than an omission: geometry.create_stairs ECHOES numSteps, so numSteps=0
    // returned `numSteps: 0` for a staircase the engine built with 1 step. Clamping here writes
    // back through the Params& reference, which is what turns that echo into the effective
    // value; the wrapper reads the struct after the call.
    //
    // ClampSegmentsWarn alone, not the pair: ClampSegments is Clamp(Value <= 0 ? Default :
    // Value, 1, GEOM_MAX_SEGMENTS), so its own floor of 1 already IS the engine floor and a
    // trailing ClampRangeWarn(1, GEOM_MAX_SEGMENTS) could never fire. The pair exists for the
    // radial counts, whose engine floor (2 or 3) sits ABOVE ClampSegments' 1. Same single-call
    // shape as GenerateBox.    //
    // ClampCountWarn, not ClampSegmentsWarn: a step count is not a segment count, and taking
    // the segment ceiling by inheritance capped a 300-step staircase at 256 for a reason
    // nobody had measured. GEOM_MAX_STAIR_STEPS carries that measurement (GeometryUtils.h) -
    // the solid generator IS quadratic in NumSteps, so a ceiling belongs here, just not that
    // one. Everything else is unchanged, the <= 0 substitution and the warning text included.
    Params.NumSteps = ClampCountWarn(Params.NumSteps, 8, GEOM_MAX_STAIR_STEPS, TEXT("numSteps"), Result);

    FGeometryScriptPrimitiveOptions Options;

    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendLinearStairs(
        TargetMesh, Options, LocalTransform, Params.StepWidth, Params.StepHeight,
        Params.StepDepth, Params.NumSteps, Params.bFloating, nullptr);

    FinishOp(TargetMesh, Result);
    return Result;
}

FOpResult GenerateSpiralStairs(UDynamicMesh* TargetMesh, FSpiralStairsParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    // AppendCurvedStairs carries the identical NumSteps = Max(1, NumSteps) floor as the linear
    // generator (MeshPrimitiveFunctions.cpp:1519), and geometry.create_spiral_stairs echoes
    // numSteps too - so the same lie was reachable through this verb. See GenerateStairs for
    // why this is one call and not the clamp pair, and why the ceiling is GEOM_MAX_STAIR_STEPS
    // rather than the segment limit.
    Params.NumSteps = ClampCountWarn(Params.NumSteps, 8, GEOM_MAX_STAIR_STEPS, TEXT("numSteps"), Result);

    FGeometryScriptPrimitiveOptions Options;

    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCurvedStairs(
        TargetMesh, Options, LocalTransform, Params.StepWidth, Params.StepHeight,
        Params.InnerRadius, Params.CurveAngle, Params.NumSteps, Params.bFloating, nullptr);

    FinishOp(TargetMesh, Result);
    return Result;
}

FOpResult GenerateRing(UDynamicMesh* TargetMesh, FRingParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    // Same AppendDisc floor as the disc - Max(3, AngleSteps) - reached through the punctured
    // generator instead. The verb's own default is 32, not the disc's 16.
    Params.AngleSteps = ClampSegmentsWarn(Params.AngleSteps, 32, TEXT("segments"), Result);
    Params.AngleSteps = ClampRangeWarn(Params.AngleSteps, 3, GEOM_MAX_SEGMENTS,
        TEXT("segments"), Result);

    FGeometryScriptPrimitiveOptions Options;

    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendDisc(
        TargetMesh, Options, LocalTransform, Params.OuterRadius, Params.AngleSteps, 0,
        0.0f, 360.0f, Params.InnerRadius, nullptr);

    FinishOp(TargetMesh, Result);
    return Result;
}

FOpResult GenerateArch(UDynamicMesh* TargetMesh, FArchParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    // Arch is AppendTorus with the revolve angle exposed, so it inherits the torus's split
    // floors - minor 3, major from the sweep. Clamping the torus and not the arch would leave
    // the same silent floor reachable through the other verb.
    //
    // The major floor is CONDITIONAL, and this is the verb that makes it worth being so:
    // `arch major_steps=2 angle=180` is a legitimate half-arch (measured closed, 44 triangles,
    // no diagnostics) and must keep working, while `arch angle=360` is a torus and cannot close
    // at 2. GeometryOpsPrimitives_RevolveStepFloor carries the measurements.
    Params.MajorSteps = ClampSegmentsWarn(Params.MajorSteps, 16, TEXT("majorSteps"), Result);
    Params.MajorSteps = ClampRangeWarn(Params.MajorSteps,
        GeometryOpsPrimitives_RevolveStepFloor(Params.Angle), GEOM_MAX_SEGMENTS,
        TEXT("majorSteps"), Result);
    Params.MinorSteps = ClampSegmentsWarn(Params.MinorSteps, 8, TEXT("minorSteps"), Result);
    Params.MinorSteps = ClampRangeWarn(Params.MinorSteps, 3, GEOM_MAX_SEGMENTS,
        TEXT("minorSteps"), Result);

    FGeometryScriptPrimitiveOptions Options;

    FGeometryScriptRevolveOptions RevolveOptions;
    RevolveOptions.RevolveDegrees = Params.Angle;

    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendTorus(
        TargetMesh, Options, LocalTransform, RevolveOptions,
        Params.MajorRadius, Params.MinorRadius, Params.MajorSteps, Params.MinorSteps,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);

    FinishOp(TargetMesh, Result);
    return Result;
}

FOpResult GeneratePipe(UDynamicMesh* TargetMesh, FPipeParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    // Pipe carries the cylinder's floors verbatim. It is the annular cylinder and an author
    // reaches for the two verbs interchangeably, so a count that is legal on one and silently
    // floored on the other would be the same trap the clamp layer exists to close.
    Params.RadialSteps = ClampSegmentsWarn(Params.RadialSteps, 24, TEXT("radialSteps"), Result);
    Params.RadialSteps = ClampRangeWarn(Params.RadialSteps, 3, GEOM_MAX_SEGMENTS,
        TEXT("radialSteps"), Result);
    Params.HeightSteps = ClampRangeWarn(Params.HeightSteps, 0, GEOM_MAX_SEGMENTS,
        TEXT("heightSteps"), Result);

    // The profile below is the annulus (innerRadius..outerRadius) x (-height/2..+height/2), and
    // a revolve cannot express it unless the radii are ordered and the inner one is off the
    // axis: inner >= outer turns the rectangle inside out and sweeps a self-intersecting solid,
    // inner <= 0 collapses its inner edge ONTO the revolve axis as a ring of zero-area
    // triangles. Refused rather than clamped because there is no non-arbitrary value to clamp
    // to - every substitute is a different pipe from the one that was asked for. The boolean
    // form this replaced accepted both and returned an empty or single-walled mesh in silence.
    if (!(Params.InnerRadius > 0.0 && Params.InnerRadius < Params.OuterRadius))
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("pipe requires 0 < innerRadius < outerRadius; got innerRadius=%g, outerRadius=%g"),
            Params.InnerRadius, Params.OuterRadius));
    }

    // A REVOLVED ANNULUS, not a boolean of two cylinders - which is what this was, and which
    // never closed. The old pair appended an UNCAPPED outer shell and subtracted an inner
    // cylinder that shared its walls' span, so nothing ever formed the annular end caps: the
    // result was two open tubes, 4 triangles per radial step where a closed annulus needs 8,
    // and it rendered see-through with no diagnostic. Sweeping the cross-section instead makes
    // closure a property of the construction rather than something a boolean has to reconstruct:
    // FRevolvePlanarPolygonGenerator sets bProfileCurveIsClosed and, at the default 360 degrees,
    // bSweepCurveIsClosed (RevolveGenerator.cpp:341, :370), so the output is a closed manifold by
    // definition. It also costs one scratch mesh instead of two, and no boolean at all.
    //
    // Counter-clockwise in (radius, height): the engine's own contract is "+X is towards the
    // outside of the revolve donut, +Y is up, polygon counter-clockwise or it will be inside-out"
    // (MeshPrimitiveFunctions.h:440-442). This winding puts outward normals on the shell and
    // INWARD ones on the bore, which is what makes the hole read as a hole.
    //
    // heightSteps keeps the cylinder's meaning - ADDITIONAL wall loops, not total
    // (SweepGenerator.cpp:547, FCylinderGenerator's LengthSamples) - so a pipe and a cylinder at
    // the same heightSteps still have the same number of loops up the wall.
    const double HalfHeight = Params.Height * 0.5;
    const int32 WallSegments = Params.HeightSteps + 1;

    TArray<FVector2D> Profile;
    Profile.Reserve(2 * WallSegments + 2);
    Profile.Add(FVector2D(Params.InnerRadius, -HalfHeight));
    for (int32 Step = 0; Step < WallSegments; ++Step)
    {
        Profile.Add(FVector2D(Params.OuterRadius,
            -HalfHeight + Params.Height * (double)Step / (double)WallSegments));
    }
    Profile.Add(FVector2D(Params.OuterRadius, HalfHeight));
    for (int32 Step = 0; Step < WallSegments; ++Step)
    {
        Profile.Add(FVector2D(Params.InnerRadius,
            HalfHeight - Params.Height * (double)Step / (double)WallSegments));
    }

    FGeometryScriptPrimitiveOptions Options;
    FGeometryScriptRevolveOptions RevolveOptions;   // 360 degrees: the sweep closes on itself.

    // Built in a SCRATCH mesh at IDENTITY, then appended under LocalTransform. Two reasons, and
    // both are load-bearing. Every generator in this family appends
    // (GeometryOps_Primitives.h), so the polygroup pass below must not reach whatever already
    // ran into TargetMesh; and that pass classifies triangles against the local Z axis, which is
    // only the revolve axis while the mesh is still unrotated. AppendMesh bakes the transform on
    // the way in and reverses orientation on a negative determinant
    // (MeshBasicEditFunctions.cpp:615-618), so the placement is identical to handing the
    // transform to the generator.
    UDynamicMesh* PipeMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    // Cleanup as a scope guard, not a statement before the success return. The MarkAsGarbage
    // call used to sit on the success path ONLY, so the first early return added anywhere below
    // this point - a guard, a polygon-limit check - would have leaked the scratch mesh into the
    // transient package for the rest of the editor session, and nothing in the function's shape
    // would have said so. Latent then, structural now.
    ON_SCOPE_EXIT
    {
        if (PipeMesh) PipeMesh->MarkAsGarbage();
    };

    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendRevolvePolygon(
        PipeMesh, Options, FTransform::Identity, Profile, RevolveOptions,
        0.0f, Params.RadialSteps, nullptr);

    // FOUR polygroups - outer wall, bore, top rim, bottom rim - assigned here because the revolve
    // generators do not honour PrimitiveOptions.PolygroupMode at all. AppendRevolvePolygon never
    // maps it onto FBaseRevolveGenerator::PolygonGroupingMode, which defaults to
    // EProfileSweepPolygonGrouping::PerFace - and for the sweep generators "PerFace" means one
    // group per QUAD (SweepGenerator.h:228-229). Left alone, a pipe would arrive with a polygroup
    // per quad, which is the exact shape that makes `bevel` chamfer every interior quad boundary
    // and return a grid of notches at ~3x the triangle cost. That is the standing trap on `torus`
    // and `arch`; it is not being extended to `pipe`.
    //
    // Classified by face normal rather than by triangle index because the index layout is the
    // generator's business: at any radialSteps and any heightSteps a pipe has exactly these four
    // faces, and the test asserts the count.
    PipeMesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        if (!EditMesh.HasTriangleGroups())
        {
            EditMesh.EnableTriangleGroups();
        }
        for (const int32 TriangleID : EditMesh.TriangleIndicesItr())
        {
            const FVector3d Normal = EditMesh.GetTriNormal(TriangleID);
            int32 Group;
            if (Normal.Z > 0.5)         { Group = 2; }   // top rim
            else if (Normal.Z < -0.5)   { Group = 3; }   // bottom rim
            else
            {
                // Outward-vs-inward on the wall: a radial normal agreeing with the triangle's own
                // radial position is the shell, disagreeing is the bore.
                const FVector3d Centroid = EditMesh.GetTriCentroid(TriangleID);
                Group = (Normal.X * Centroid.X + Normal.Y * Centroid.Y >= 0.0) ? 0 : 1;
            }
            EditMesh.SetTriangleGroup(TriangleID, Group);
        }
    }, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

    // Only the finished pipe reaches TargetMesh, so prior geometry is left exactly as it was.
    FGeometryScriptAppendMeshOptions AppendOptions;
    UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(
        TargetMesh, PipeMesh, LocalTransform, false, AppendOptions, nullptr);

    FinishOp(TargetMesh, Result);
    return Result;
}

double PolygonSignedArea(const TArray<FVector2D>& Polygon)
{
    const int32 N = Polygon.Num();
    if (N < 3)
    {
        return 0.0;
    }
    double Twice = 0.0;
    for (int32 i = 0, j = N - 1; i < N; j = i++)
    {
        Twice += (Polygon[j].X * Polygon[i].Y) - (Polygon[i].X * Polygon[j].Y);
    }
    return 0.5 * Twice;
}

namespace
{
    // Orientation sign of (A, B, C). Zero within tolerance means collinear.
    double GeometryOpsPrimitives_Cross2D(const FVector2D& A, const FVector2D& B, const FVector2D& C)
    {
        return (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
    }

    // Proper crossing only: the two segments' endpoints strictly straddle each other. Shared
    // endpoints and touching are handled by the coincident-point and adjacency rules in the
    // caller, so a merely touching pair is not reported here as a crossing.
    bool GeometryOpsPrimitives_SegmentsProperlyCross(
        const FVector2D& P1, const FVector2D& P2, const FVector2D& Q1, const FVector2D& Q2)
    {
        const double D1 = GeometryOpsPrimitives_Cross2D(P1, P2, Q1);
        const double D2 = GeometryOpsPrimitives_Cross2D(P1, P2, Q2);
        const double D3 = GeometryOpsPrimitives_Cross2D(Q1, Q2, P1);
        const double D4 = GeometryOpsPrimitives_Cross2D(Q1, Q2, P2);
        return ((D1 > 0.0) != (D2 > 0.0)) && ((D3 > 0.0) != (D4 > 0.0))
            && !FMath::IsNearlyZero(D1) && !FMath::IsNearlyZero(D2)
            && !FMath::IsNearlyZero(D3) && !FMath::IsNearlyZero(D4);
    }
}

bool PolygonIsSimple(const TArray<FVector2D>& Polygon, FString& OutReason)
{
    const int32 N = Polygon.Num();
    if (N < 3)
    {
        OutReason = FString::Printf(
            TEXT("a cross-section needs at least 3 points; got %d"), N);
        return false;
    }

    // Coincident consecutive points first: they produce a zero-length edge, which makes every
    // orientation test involving it meaningless, so the crossing scan below would report nothing.
    for (int32 i = 0, j = N - 1; i < N; j = i++)
    {
        if (FVector2D::Distance(Polygon[j], Polygon[i]) < UE_KINDA_SMALL_NUMBER)
        {
            OutReason = FString::Printf(
                TEXT("points %d and %d are the same position (%.4f, %.4f), which is a zero-length edge"),
                j, i, Polygon[i].X, Polygon[i].Y);
            return false;
        }
    }

    for (int32 i = 0; i < N; ++i)
    {
        const FVector2D& P1 = Polygon[i];
        const FVector2D& P2 = Polygon[(i + 1) % N];
        // Start at i+2 and stop before the edge that closes back onto i: adjacent edges legitimately
        // share an endpoint, and edge 0 is adjacent to edge N-1.
        for (int32 j = i + 2; j < N; ++j)
        {
            if (i == 0 && j == N - 1)
            {
                continue;
            }
            if (GeometryOpsPrimitives_SegmentsProperlyCross(P1, P2, Polygon[j], Polygon[(j + 1) % N]))
            {
                OutReason = FString::Printf(
                    TEXT("edge %d-%d crosses edge %d-%d, so the outline is self-intersecting"),
                    i, (i + 1) % N, j, (j + 1) % N);
                return false;
            }
        }
    }

    // Check crossings before signed area: a self-intersecting outline such as a bowtie can have
    // cancelling lobes and therefore zero shoelace area. The crossing is the useful diagnosis,
    // while "no area" would hide the actual defect and its offending edges.
    const double Area = PolygonSignedArea(Polygon);
    if (FMath::IsNearlyZero(Area, UE_KINDA_SMALL_NUMBER))
    {
        OutReason = FString::Printf(
            TEXT("the %d points enclose no area (they are collinear), so there is no face to cap"), N);
        return false;
    }

    OutReason.Reset();
    return true;
}

FOpResult GenerateRamp(UDynamicMesh* TargetMesh, FRampParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    FGeometryScriptPrimitiveOptions Options;

    TArray<FVector2D> RampPolygon;
    RampPolygon.Add(FVector2D(0, 0));
    RampPolygon.Add(FVector2D(Params.Length, 0));
    RampPolygon.Add(FVector2D(Params.Length, Params.Height));

    // The wedge profile is synthesised, but its shape is the caller's: length or height at or
    // near zero collapses these three points onto a line, and AppendSimpleExtrudePolygon caps by
    // flat triangulation with a forced-ear fallback that emits N-2 triangles regardless and
    // reports nothing. Left alone, create_ramp(height=0) returns a successful mesh whose cap is
    // degenerate garbage. Refuse at the input, where the defect is still detectable - see
    // PolygonIsSimple.
    FString PolygonReason;
    if (!PolygonIsSimple(RampPolygon, PolygonReason))
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
            TEXT("ramp cross-section is not a cappable outline (length %.4f, height %.4f): %s"),
            Params.Length, Params.Height, *PolygonReason));
    }

    // A negative length or height mirrors the wedge and reverses its winding, so the extrude caps
    // the outline inside-out. Refused rather than silently absolute-valued: the caller asked for a
    // shape that does not exist, and quietly building its mirror is the same class of lie.
    if (Params.Length <= 0.0 || Params.Height <= 0.0 || Params.Width <= 0.0)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
            TEXT("ramp width, length and height must all be positive; got width %.4f, length %.4f, height %.4f"),
            Params.Width, Params.Length, Params.Height));
    }

    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSimpleExtrudePolygon(
        TargetMesh, Options, LocalTransform, RampPolygon, Params.Width, 0, true,
        EGeometryScriptPrimitiveOriginMode::Base, nullptr);

    FinishOp(TargetMesh, Result);
    return Result;
}

FOpResult GenerateRevolve(UDynamicMesh* TargetMesh, FRevolveParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    // AppendRevolvePath hands the count to FRevolvePlanarPathGenerator as Steps =
    // FMath::Max(Steps, 2) (MeshPrimitiveFunctions.cpp:871), so steps=1 silently built the
    // 2-step sweep - and, like the radial counts before today's pass, the value was unbounded
    // above. geometry.revolve echoes `steps`, so the clamp has to write back through Params&
    // for the echo to stay the effective value.
    //
    // The floor is 2 for a partial sweep and 3 at 360, where the sweep closes and 2 leaves 8
    // triangles with 10 boundary edges and 6 degenerates - see
    // GeometryOpsPrimitives_RevolveStepFloor. `angle` defaults to 360, so the default document
    // takes the higher floor.
    Params.Steps = ClampSegmentsWarn(Params.Steps, 16, TEXT("steps"), Result);
    Params.Steps = ClampRangeWarn(Params.Steps, GeometryOpsPrimitives_RevolveStepFloor(Params.Angle),
        GEOM_MAX_SEGMENTS, TEXT("steps"), Result);

    // Substituted IN the params so the caller reports the profile that was actually revolved
    // rather than the one it asked for - geometry.revolve echoes profilePoints.
    if (Params.Profile.Num() < 2)
    {
        Result.Warnings.Add(FString::Printf(
            TEXT("profile has %d point(s); substituted the 6-point default profile"), Params.Profile.Num()));
        Params.Profile.Empty();
        Params.Profile.Add(FVector2D(10, 0));
        Params.Profile.Add(FVector2D(30, 0));
        Params.Profile.Add(FVector2D(50, 25));
        Params.Profile.Add(FVector2D(50, 75));
        Params.Profile.Add(FVector2D(30, 100));
        Params.Profile.Add(FVector2D(10, 100));
    }

    FGeometryScriptPrimitiveOptions Options;

    FGeometryScriptRevolveOptions RevolveOptions;
    RevolveOptions.RevolveDegrees = Params.Angle;

    // The closed-section branch. See GeometryOpsPrimitives_ProfileIsClosedSection for what the
    // path generator does to a repeated endpoint and why no value of `capped` survives it.
    if (GeometryOpsPrimitives_ProfileIsClosedSection(Params.Profile))
    {
        // Dropped IN the params, like the default-profile substitution above, so profilePoints
        // echoes the profile that was actually swept.
        Params.Profile.Pop();

        // `capped` keeps the meaning its documentation gives it - the ends of a PARTIAL sweep -
        // and loses the meaning it never advertised, the caps to the axis. At 360 the sweep
        // closes on itself and the generator ignores the flag entirely
        // (bProfileCurveIsClosed && !bSweepCurveIsClosed gates the cap fill,
        // RevolveGenerator.cpp:231).
        RevolveOptions.bFillPartialRevolveEndcaps = Params.bCapped;

        Result.Warnings.Add(FString::Printf(
            TEXT("profile's first and last points coincide, so it is revolved as a closed section: "
                 "the repeated endpoint is dropped (%d points swept) and no cap to the revolve axis "
                 "is emitted, which would have filled the bore"),
            Params.Profile.Num()));

        UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendRevolvePolygon(
            TargetMesh, Options, LocalTransform, Params.Profile, RevolveOptions,
            0.0f, Params.Steps, nullptr);

        FinishOp(TargetMesh, Result);
        return Result;
    }

    // The same defect one spelling away, where the op can only name it - see
    // GeometryOpsPrimitives_AxisCapsWouldBeAMembrane for why this is a warning and not a fix.
    if (Params.bCapped && GeometryOpsPrimitives_AxisCapsWouldBeAMembrane(Params.Profile))
    {
        Result.Warnings.Add(
            TEXT("both profile ends sit off the revolve axis at the same height, so `capped` adds "
                 "two coincident, oppositely wound caps to the axis: a membrane across the bore "
                 "that encloses no volume and moves no health field. If this profile is a closed "
                 "section, repeat its first point as the last point; if it is a lathed solid, "
                 "ignore this"));
    }

    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendRevolvePath(
        TargetMesh, Options, LocalTransform, Params.Profile, RevolveOptions,
        Params.Steps, Params.bCapped, nullptr);

    FinishOp(TargetMesh, Result);
    return Result;
}

FOpResult GenerateEmpty(UDynamicMesh* TargetMesh, FEmptyMeshParams& Params, const FTransform& LocalTransform)
{
    FOpResult Result;
    if (!BeginOp(TargetMesh, Result)) return Result;

    FinishOp(TargetMesh, Result);
    return Result;
}

}
