// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryOps_Primitives.h - the 16 generator ops behind geometry.create_* and geometry.revolve.
//
// These are the mesh-building half of those verbs. The actor half - label resolution, spawn,
// dirty ceremony, response JSON - stays in PrimitiveHandler.cpp, because two front-ends call
// these functions and only one of them has an actor: the RPC wrappers, and the .pwmodel
// compiler, which builds into a transient mesh and never touches a level.
//
// The F<Verb>Params structs are the SINGLE parameter vocabulary for both front-ends, so neither
// can drift from the other - they compile against the same fields. They are named for the axes
// they set rather than for the RPC's published width/height/depth, whose mapping the wiki itself
// flags as a trap ('height' is horizontal); the RPC wrapper translates its legacy names onto
// FVector Size so its published contract is byte-identical, and .pwmodel maps onto the fields.
//
// Params are taken by NON-CONST reference across the whole family. The safety clamps
// (GeometryUtils::ClampDimension / ClampSegments and the GEOM_* limits) moved into the ops, and
// four of the verbs echo the CLAMPED value in their response - geometry.create_box echoes
// width/height/depth, geometry.revolve echoes steps and profilePoints (after its default-profile
// substitution), and geometry.create_stairs / create_spiral_stairs echo numSteps. Normalizing
// the struct in place is what lets a wrapper echo the effective value without a second copy of
// the clamp logic sitting beside it and rotting - and it is what fixed the stairs echo, which
// returned `numSteps: 0` for a staircase the engine had floored to 1 step.
//
// Every clamp also appends to FOpResult::Warnings, and since the warnings sweep those reach an
// RPC caller as the response's `warnings` array (Handlers/Geometry/GeometryOpWarnings.h) as well
// as the .pwmodel compiler's PWMODEL_STAGE_WARNING stream.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/Geometry/GeometryOps.h"

class UDynamicMesh;

namespace GeometryOps
{
    // ------------------------------------------------------------------------------------
    // LocalTransform, on every Generate* below (load-bearing):
    //
    // It is handed straight to the engine's Append* call, so it BAKES INTO THE VERTICES. The
    // RPC wrappers pass FTransform::Identity and put the requested location/rotation/scale on
    // the spawned actor instead. Applying it in both places double-applies the location and
    // squares the scale - the full failure mode is documented on GeometryTarget::Spawn. The
    // .pwmodel compiler is the caller that passes a real transform, because it has no actor to
    // carry one.
    //
    // Every op appends to TargetMesh rather than replacing it: a .pwmodel part is a sequence of
    // generators merging into one mesh, and that is also what the engine's Append* calls do.
    // ------------------------------------------------------------------------------------

    // geometry.create_box. Size is the local X/Y/Z extent - Size.Z is the VERTICAL one.
    struct FBoxParams
    {
        FVector Size = FVector(100.0, 100.0, 100.0);
        FIntVector Steps = FIntVector(1, 1, 1);
    };
    FOpResult GenerateBox(UDynamicMesh* TargetMesh, FBoxParams& Params, const FTransform& LocalTransform);

    // geometry.create_sphere. Box-topology sphere (AppendSphereBox), not lat/long: a ROUNDED CUBE
    // whose six subdivided quad faces are projected onto the sphere. No poles, no seam, uniform
    // quad density, and a triangle count of 12*(Subdivisions-1)^2 - so a recipe ported from
    // append_sphere_lat_long yields a different polyhedron AND a different triangle count at the
    // same nominal value. Subdivisions is the step count on all three box axes and its EFFECTIVE
    // FLOOR IS 2: the op clamps 1 up to 2 with a warning, because AppendSphereBox draws the same
    // 12-triangle cube for both.
    struct FSphereParams
    {
        double Radius = 50.0;
        int32 Subdivisions = 16;
    };
    FOpResult GenerateSphere(UDynamicMesh* TargetMesh, FSphereParams& Params, const FTransform& LocalTransform);

    // geometry.create_cylinder. HeightSteps > 1 gives a downstream twist/bend/taper intermediate
    // side-wall loops to displace; at the default 1 those deformers have nothing to bend.
    // RadialSteps has an EFFECTIVE FLOOR OF 3 (AppendCylinder's AngleSamples); HeightSteps' floor
    // is 0, which is a legal count meaning "no intermediate loop". Both clamp with a warning.
    struct FCylinderParams
    {
        double Radius = 50.0;
        double Height = 100.0;
        int32 RadialSteps = 16;
        int32 HeightSteps = 1;
    };
    FOpResult GenerateCylinder(UDynamicMesh* TargetMesh, FCylinderParams& Params, const FTransform& LocalTransform);

    // geometry.create_cone. TopRadius > 0 makes a truncated cone (frustum). Same engine generator
    // as the cylinder and therefore the same floors: RadialSteps 3, HeightSteps 0.
    struct FConeParams
    {
        double BaseRadius = 50.0;
        double TopRadius = 0.0;
        double Height = 100.0;
        int32 RadialSteps = 16;
        int32 HeightSteps = 1;
    };
    FOpResult GenerateCone(UDynamicMesh* TargetMesh, FConeParams& Params, const FTransform& LocalTransform);

    // geometry.create_capsule. Length is the CYLINDRICAL section only (the engine's LineLength);
    // the two hemispherical caps add Radius at each end, so the total extent is Length + 2*Radius.
    // The two step counts have DIFFERENT floors: HemisphereSteps 2 (a half-arc profile),
    // RadialSteps 3 (a closed ring). Both clamp with a warning.
    struct FCapsuleParams
    {
        double Radius = 50.0;
        double Length = 100.0;
        int32 HemisphereSteps = 4;
        int32 RadialSteps = 16;
    };
    FOpResult GenerateCapsule(UDynamicMesh* TargetMesh, FCapsuleParams& Params, const FTransform& LocalTransform);

    // geometry.create_torus - a full 360-degree revolve. A partial one is FArchParams.
    // MajorSteps floors at 3 and MinorSteps at 3, but not for the same reason and not through
    // the same rule: the minor count is a closed profile circle, which the engine itself floors
    // at 3, while the major count is the revolve generator's, which the engine floors at 2 and
    // this layer raises to 3 because a torus's sweep always closes and 2 cannot close it.
    // FArchParams is the same engine call with the angle exposed and takes 2 below 360.
    struct FTorusParams
    {
        double MajorRadius = 50.0;
        double MinorRadius = 20.0;
        int32 MajorSteps = 16;
        int32 MinorSteps = 8;
    };
    FOpResult GenerateTorus(UDynamicMesh* TargetMesh, FTorusParams& Params, const FTransform& LocalTransform);

    // geometry.create_plane. A flat rectangle in the local XY plane - two-axis, so Size is a
    // 2-tuple rather than the 3-tuple every solid uses. Steps floors at 0, NOT at the radial
    // family's 2-or-3: AppendRectangleXY takes Max(0, Steps) per axis, and 0 is a meaningful
    // value (one quad, no interior loop), so a negative clamps up and warns while 0 is honoured.
    struct FPlaneParams
    {
        FVector2D Size = FVector2D(100.0, 100.0);
        FIntPoint Steps = FIntPoint(1, 1);
    };
    FOpResult GeneratePlane(UDynamicMesh* TargetMesh, FPlaneParams& Params, const FTransform& LocalTransform);

    // geometry.create_disc - a filled disc. The one with a hole is FRingParams. AngleSteps floors
    // at 3.
    struct FDiscParams
    {
        double Radius = 50.0;
        int32 AngleSteps = 16;
    };
    FOpResult GenerateDisc(UDynamicMesh* TargetMesh, FDiscParams& Params, const FTransform& LocalTransform);

    // geometry.create_stairs - a straight run. NumSteps floors at 1 (AppendLinearStairs takes
    // Max(1, NumSteps)) and the wrapper ECHOES it, so the clamp is what stops the echo lying.
    struct FStairsParams
    {
        float StepWidth = 100.0f;
        float StepHeight = 20.0f;
        float StepDepth = 30.0f;
        int32 NumSteps = 8;
        bool bFloating = false;
    };
    FOpResult GenerateStairs(UDynamicMesh* TargetMesh, FStairsParams& Params, const FTransform& LocalTransform);

    // geometry.create_spiral_stairs - a curved run. CurveAngle is the total sweep in degrees.
    // Same NumSteps floor of 1 as the straight run, and the same echoed value.
    struct FSpiralStairsParams
    {
        float StepWidth = 100.0f;
        float StepHeight = 20.0f;
        float InnerRadius = 150.0f;
        float CurveAngle = 90.0f;
        int32 NumSteps = 8;
        bool bFloating = false;
    };
    FOpResult GenerateSpiralStairs(UDynamicMesh* TargetMesh, FSpiralStairsParams& Params, const FTransform& LocalTransform);

    // geometry.create_ring - a disc with a concentric hole. Same AngleSteps floor of 3 as the
    // disc, but its own default of 32.
    struct FRingParams
    {
        double OuterRadius = 50.0;
        double InnerRadius = 25.0;
        int32 AngleSteps = 32;
    };
    FOpResult GenerateRing(UDynamicMesh* TargetMesh, FRingParams& Params, const FTransform& LocalTransform);

    // geometry.create_arch - a partial torus. Angle is the revolve sweep in degrees; at 360 this
    // is a torus, which is why the two share an engine call and not a params struct.
    //
    // MinorSteps floors at 3. MajorSteps floors at 2 below 360 and at 3 from 360 up, because
    // that is where the generator closes the sweep: a half-arch at 2 is sound, a full turn at 2
    // is an open shell. This is the only params struct in the file whose floor depends on
    // another of its own fields.
    struct FArchParams
    {
        double MajorRadius = 100.0;
        double MinorRadius = 25.0;
        double Angle = 180.0;
        int32 MajorSteps = 16;
        int32 MinorSteps = 8;
    };
    FOpResult GenerateArch(UDynamicMesh* TargetMesh, FArchParams& Params, const FTransform& LocalTransform);

    // geometry.create_pipe - a hollow tube, swept as an annular cross-section around local Z.
    //
    // CENTRE-PLACED like every other generator here: a pipe of Height H spans z -H/2..+H/2, the
    // same span a cylinder of that height covers. It used to be the family's ONE base-placed
    // exception (z 0..H), which displaced every pipe by half its height relative to the cylinder
    // it is otherwise interchangeable with, and silently mis-covered the twist/taper/bend
    // extents - those are symmetric about the mesh ORIGIN, so a base-placed pipe got the deform
    // applied to its lower half only. Nothing documented it and nothing tested it.
    //
    // Closed manifold: the outer wall, the bore and BOTH annular end caps. It was two open
    // walls - 4 triangles per radial step where an annulus needs 8 - for as long as it was built
    // as an uncapped cylinder minus a bore, and it rendered see-through with no diagnostic.
    // Triangle count is exactly 2 * RadialSteps * (2 * HeightSteps + 4).
    //
    // Cylinder floors apply: RadialSteps 3, HeightSteps 0, and HeightSteps keeps the cylinder's
    // ADDITIONAL-loop meaning so the two verbs stay comparable. 0 < InnerRadius < OuterRadius is
    // required rather than clamped - see the op.
    struct FPipeParams
    {
        double OuterRadius = 50.0;
        double InnerRadius = 40.0;
        double Height = 100.0;
        int32 RadialSteps = 24;
        int32 HeightSteps = 1;
    };
    FOpResult GeneratePipe(UDynamicMesh* TargetMesh, FPipeParams& Params, const FTransform& LocalTransform);

    // geometry.create_ramp - a wedge: a right triangle of Length x Height extruded by Width.
    // ------------------------------------------------------------------------------------
    // Cross-section validity, for any polygon about to be handed to a CAPPED generator.
    //
    // Geometry Script's capped extrude and sweep entry points (AppendSimpleExtrudePolygon,
    // AppendSweepPolygon) check only that the caller passed three or more points. The cap is then
    // produced by PolygonTriangulation::TriangulateSimplePolygon, which is documented for SIMPLE
    // polygons and whose ear clipper, when it can find no ear, "just treats the current vertex as
    // an ear" rather than failing. So a self-intersecting or degenerate cross-section still yields
    // exactly N-2 cap triangles, at the expected triangle count, with overlapping and
    // self-cancelling geometry - and nothing is written to the debug channel, so the call reports
    // success. The result renders as a solid until something is seen through it.
    //
    // A post-condition cannot recover this. The cap reuses the end-section vertices, so a
    // geometrically wrong cap is still topologically CLOSED and an open-boundary count reads
    // clean; an enclosed-volume check has no reference to compare against. The only place the
    // defect is detectable is the input, which is where these two live.
    //
    // Signed area by the shoelace formula. Positive is counter-clockwise. Near-zero means the
    // outline encloses nothing - collinear or coincident points - whatever its point count.
    double PolygonSignedArea(const TArray<FVector2D>& Polygon);

    // True when Polygon is a simple polygon: at least three points, no coincident consecutive
    // points, a non-negligible enclosed area, and no two non-adjacent edges crossing. On false,
    // OutReason names the specific violation - and the offending indices where there are any - so
    // a refusal tells the caller which point to move. O(n^2) in the point count, which is the
    // right cost for a cross-section and the wrong one for a mesh.
    bool PolygonIsSimple(const TArray<FVector2D>& Polygon, FString& OutReason);

    struct FRampParams
    {
        double Width = 100.0;
        double Length = 200.0;
        double Height = 50.0;
    };
    FOpResult GenerateRamp(UDynamicMesh* TargetMesh, FRampParams& Params, const FTransform& LocalTransform);

    // geometry.revolve - sweeps an open 2D path around the local Z axis. Profile points are
    // (radius, height); fewer than two is not a path, and the op substitutes a demo vase profile
    // IN PLACE so the caller can report what was actually revolved. Steps is echoed, same as the
    // two stair verbs, and floors at 2 below 360 (AppendRevolvePath takes Max(Steps, 2)) and at
    // 3 from 360 up, where the sweep closes - the same conditional floor FArchParams takes, and
    // Angle defaults to 360, so the default takes the higher one.
    struct FRevolveParams
    {
        TArray<FVector2D> Profile;
        double Angle = 360.0;
        int32 Steps = 16;
        bool bCapped = true;
    };
    FOpResult GenerateRevolve(UDynamicMesh* TargetMesh, FRevolveParams& Params, const FTransform& LocalTransform);

    // geometry.create_procedural_mesh - the empty generator. It appends nothing by design: the
    // verb exists to hand back a mesh that later element-edit verbs (append_vertex,
    // append_triangle, append_buffers) build up from nothing. It is still an op rather than a
    // bare NewObject so the family has one entry point per verb and the count/warning reporting
    // is uniform. The verb's enableCollision is component plumbing, like actorName, and has no
    // field here.
    struct FEmptyMeshParams
    {
    };
    FOpResult GenerateEmpty(UDynamicMesh* TargetMesh, FEmptyMeshParams& Params, const FTransform& LocalTransform);
}
