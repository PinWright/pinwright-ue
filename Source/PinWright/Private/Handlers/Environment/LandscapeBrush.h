// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Brush geometry for landscape.sculpt, kept out of the handler so the parts that decide
// the SHAPE of an edit can be exercised without a live ALandscape, an RHI or an editor
// world. The handler calls these same functions (rpc-design.md §12: a regression test
// that re-implements the fix as a local lambda still passes after reverting production
// code), so the falloff a test measures is the falloff a stamp writes.
//
// Two decisions live here and nowhere else:
//
//  1. **The brush centre is fractional.** The pre-2026-08 handler did
//     `CenterX = FMath::RoundToInt(LocalPos.X)`, which snapped every stamp to a
//     heightfield vertex. A traced curve is then sampled onto the lattice before the
//     falloff is ever evaluated, so a shallow diagonal degrades into 90-degree treads —
//     the defect that got two terrains rejected on sight. Distances here are measured
//     from a `double` centre, so a centre 0.4 cells along produces a different alpha
//     field from one 0.6 cells along.
//
//  2. **Distance is measured in WORLD units through both draw scales.** The old code
//     converted the radius with `BrushRadius / ScaleX` alone and then measured in vertex
//     units, so on a landscape with ScaleX != ScaleY the brush was an ellipse in the
//     world while reporting a circle's radius. Local->world for a landscape is
//     `World = Rot * (Scale * Local) + Trans` with Rot a rotation (length-preserving),
//     and a brush spans only local X/Y, so the world distance between two vertices is
//     exactly `sqrt((ScaleX*dX)^2 + (ScaleY*dY)^2)` under ANY actor rotation. That is
//     the metric used throughout; no rotation term is needed and none is approximated.

namespace PinWright::LandscapeBrush
{
    // ---- Falloff profiles ----------------------------------------------------------
    //
    // All four are the engine's own curves, transcribed from the Landscape Ed Mode brush
    // classes in UE 5.8 `LandscapeEdModeBrushes.cpp` (Linear :1020, Smooth :1048,
    // Spherical :1076, Tip :1112) so a sculpt through the RPC matches what a human gets
    // dragging the same brush. The engine parameterises them as an inner plateau
    // `Radius` plus an extra `Falloff` WIDTH beyond it; this verb's long-standing
    // parameterisation is an OUTER `brushRadius` with `brushFalloff` as a 0..1 fraction
    // of it. The two are related by InnerRadius = R*(1-F), FalloffWidth = R*F, which is
    // why `Linear` here reproduces the previous handler's ramp exactly rather than
    // approximately - `linear` remains the default and no existing call changes shape.
    enum class EFalloffProfile : uint8
    {
        Linear,     // alpha = t                      (the pre-existing, and still default, ramp)
        Smooth,     // alpha = t*t*(3-2t)             smoothstep: zero slope at both ends
        Spherical,  // alpha = sqrt(1-(1-t)^2)        dome shoulder: rises fast off the rim
        Tip,        // alpha = 1-sqrt(1-t*t)          inverse dome: hugs the rim, spikes at the plateau
    };

    // Sculpt operations. `Smooth` is a local averaging kernel over the heights the verb
    // already read back, NOT the engine's FLandscapeToolStrokeSmooth (which lives inside
    // the LandscapeEditor module behind an FEdModeLandscape and is not reachable from a
    // headless RPC). Documented as such wherever it is offered.
    enum class EToolMode : uint8
    {
        Raise,
        Lower,
        Flatten,
        Smooth,
    };

    // Parse helpers return false on an unrecognised spelling and leave Out untouched.
    // rpc-design.md §3: an unknown enum value errors, it never falls back - the old
    // handler matched three literals and let anything else fall through to a zero delta,
    // reporting success for a typo.
    bool ParseToolMode(const FString& In, EToolMode& Out);
    bool ParseFalloffProfile(const FString& In, EFalloffProfile& Out);

    const TCHAR* ToolModeName(EToolMode Mode);
    const TCHAR* FalloffProfileName(EFalloffProfile Profile);

    // Comma-separated valid spellings, for the message on a typed rejection.
    FString ValidToolModes();
    FString ValidFalloffProfiles();

    // Brush weight at a world-space distance from the stroke.
    //  DistanceUu     - world centimetres from the query point to the nearest point of the stroke
    //  RadiusUu       - the brush's outer radius in world centimetres (alpha is 0 beyond it)
    //  FalloffFraction- 0..1; the outer fraction of the radius occupied by the ramp.
    //                   0 is a hard edge (alpha 1 everywhere inside), 1 ramps from the centre.
    // Returns 0..1. Deterministic and side-effect free.
    float EvaluateFalloff(EFalloffProfile Profile, double DistanceUu, double RadiusUu, double FalloffFraction);

    // One vertex of the stroke, in LANDSCAPE-LOCAL fractional heightfield coordinates for
    // X/Y (so 12.5 is legal and means half a cell past vertex 12) and WORLD centimetres
    // for Z. Z travels in world units because it is a flatten/ramp target, not a lattice
    // position: interpolating it along the stroke is what lets one call cut a graded
    // river bed or a sloped road rather than a series of flat pads.
    struct FStrokeVertex
    {
        double X = 0.0;
        double Y = 0.0;
        double WorldZ = 0.0;
    };

    // Nearest point on one stroke segment to a query vertex.
    //
    //  Px, Py         - query point in local fractional heightfield coordinates
    //  A, B           - segment endpoints (a zero-length segment is legal and is the
    //                   single-stamp case: `location` is a one-vertex stroke, so the point
    //                   form and the path form go through this one function and cannot
    //                   drift apart - rpc-design.md §2, one writer)
    //  ScaleX, ScaleY - the landscape actor's draw scale, used to convert the local
    //                   separation into world centimetres (see the note at the top)
    //
    // OutDistanceUu receives the world-space XY distance; OutWorldZ receives Z linearly
    // interpolated between A and B at the closest point, which is what makes a stroke a
    // ramp rather than a stack of pads. The segment parameter is solved in WORLD space,
    // not local space, so a non-uniform draw scale does not skew where along the segment
    // the closest point falls.
    void ClosestPointOnSegment(
        double Px, double Py,
        const FStrokeVertex& A, const FStrokeVertex& B,
        double ScaleX, double ScaleY,
        double& OutDistanceUu, double& OutWorldZ);

    // Inclusive local-vertex bounding box covering everything a stroke segment can reach
    // at RadiusUu. Returned as doubles; the caller floors/ceils and clamps to the
    // landscape extent. Used to keep the swept-brush cost linear in path length: each
    // segment only visits its own footprint instead of every vertex of the union box,
    // so a 200-segment stroke is 200 small rectangles rather than 200 full-box sweeps.
    void SegmentBounds(
        const FStrokeVertex& A, const FStrokeVertex& B,
        double RadiusUu, double ScaleX, double ScaleY,
        double& OutMinX, double& OutMinY, double& OutMaxX, double& OutMaxY);
}
