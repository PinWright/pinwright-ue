// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UPrimitiveComponent;

// The single "what collision does this thing actually have?" seam.
//
// Two features need the same answer and must not derive it twice: spatial.raycast's
// simpleCollisionShapes / renderGeometryHit fields (why did a complex trace resolve against
// render triangles?) and editor.set_view_mode's collision report (why is this mesh invisible
// in a collision view?). A second copy of the AggGeom/trace-flag reading is how the two
// silently disagree, so both route through here.
namespace PinWrightCollisionSummary
{
    // Which collision view is being asked about, in the user's vocabulary rather than the
    // engine's channel names ("Player Collision" says nothing about simple vs complex, and
    // the mapping inverts for a UseComplexAsSimple mesh).
    //
    //   Simple  -> VMI_CollisionPawn       ("Player Collision"),     draws the AggGeom primitives
    //   Complex -> VMI_CollisionVisibility ("Visibility Collision"), draws per-triangle geometry
    enum class EDrawChannel : uint8
    {
        Simple,
        Complex
    };

    // Resolved collision trace behaviour. CTF_UseDefault never appears here: it is resolved
    // against the project's default shape complexity by UBodySetupCore::GetCollisionTraceFlag,
    // and a classifier that read the raw field instead would bucket every defaulted mesh wrong.
    enum class ETraceKind : uint8
    {
        SimpleAndComplex,
        SimpleAsComplex,   // CTF_UseSimpleAsComplex - simple shapes answer complex queries
        ComplexAsSimple    // CTF_UseComplexAsSimple - per-triangle geometry answers simple queries
    };

    // One component's collision, in the shape both consumers need.
    struct FComponentCollisionSummary
    {
        // False when the component pointer was null; every other field is at its default.
        bool bValid = false;

        // The component exposes a UBodySetup at all. False for Landscape heightfield collision
        // components and most non-mesh primitives, where there is no simple/complex split to
        // report. This is NOT "has no collision" - it is "the simple/complex question does not
        // apply", and the two must stay distinguishable or a landscape gets reported as a
        // collisionless mesh.
        bool bHasBodySetup = false;

        // UPrimitiveComponent::IsCollisionEnabled(). False means the component is invisible in
        // EVERY collision view no matter what geometry it owns.
        bool bCollisionEnabled = false;

        // Engine enum name for the above ("NoCollision" / "QueryOnly" / "PhysicsOnly" /
        // "QueryAndPhysics"), for the response payload.
        FString CollisionEnabledName;

        ETraceKind Trace = ETraceKind::SimpleAndComplex;
        FString TraceName;

        // AggGeom.GetElementCount() verbatim, or -1 when there is no body setup. This is the
        // number spatial.raycast reports as simpleCollisionShapes, so it must stay the engine's
        // own total (which sums NINE element arrays, more than the five broken out below) rather
        // than the sum of the per-kind counts.
        int32 SimpleShapeCount = -1;

        // The five element kinds this codebase already emits in static_mesh dumps. Their sum can
        // legitimately be less than SimpleShapeCount; see above.
        int32 SphereCount = 0;
        int32 BoxCount = 0;
        int32 SphylCount = 0;
        int32 ConvexCount = 0;
        int32 TaperedCapsuleCount = 0;

        bool bRespondsToPawn = false;
        bool bRespondsToVisibility = false;
    };

    // Count of SIMPLE collision primitives on the component's body setup, or -1 when it exposes
    // no body setup. Engine-verbatim (UBodySetup->AggGeom.GetElementCount()); spatial.raycast's
    // simpleCollisionShapes field is this value, so it must not be redefined as "sum of the
    // kinds we happen to break out".
    //
    // 0 is the load-bearing value: the component has NO simple collision, so a bTraceComplex
    // query resolved against its RENDER triangles - the silent wrong-height trap behind board
    // ticket B-trace-complex-hits-render-geometry.
    int32 CountSimpleShapes(UPrimitiveComponent* Component);

    FComponentCollisionSummary Summarize(UPrimitiveComponent* Component);

    // Whether the component responds at all to the channel the given collision view queries
    // (ECC_Pawn for Simple, ECC_Visibility for Complex). A mesh with perfectly good collision
    // that ignores the queried channel draws NOTHING in that view.
    bool RespondsToChannel(const FComponentCollisionSummary& Summary, EDrawChannel Channel);

    // Whether this view would draw the component's SIMPLE primitives / COMPLEX geometry.
    // Mirrors the engine's own rule (UE 5.8 Runtime/Engine/Private/Rendering/NaniteResources.cpp
    // :2527-2528), including the UseComplexAsSimple / UseSimpleAsComplex inversion:
    //
    //   bDrawComplex = (Visibility && Trace != SimpleAsComplex) || (Pawn && Trace == ComplexAsSimple)
    //   bDrawSimple  = (Pawn       && Trace != ComplexAsSimple) || (Visibility && Trace == SimpleAsComplex)
    bool DrawsSimpleInChannel(const FComponentCollisionSummary& Summary, EDrawChannel Channel);
    bool DrawsComplexInChannel(const FComponentCollisionSummary& Summary, EDrawChannel Channel);

    // Whether the component puts ANY pixels on screen in the given collision view.
    //
    // This is the question the picture cannot answer: three independent conditions each make a
    // component invisible (collision disabled, no primitives to draw, no response on the queried
    // channel), and an empty collision view is pixel-identical to a fully-collided scene. False
    // here with bValid true is exactly the set a caller must read as data rather than look for.
    //
    // Caveat, stated rather than hidden: the presence of per-triangle collision DATA is not
    // cheaply checkable without cooking state, so a component whose trace flag selects complex
    // geometry is assumed to have some. Under-reporting invisibility is the safe direction -
    // it never claims a mesh is fine when it is missing.
    bool DrawsInChannel(const FComponentCollisionSummary& Summary, EDrawChannel Channel);

    // How an actor lands in a collision view, aggregated over its primitive components.
    struct FActorCollisionReport
    {
        FString Name;    // display label
        FString Class;

        // Aggregate over the actor's primitive components.
        int32 PrimitiveComponents = 0;
        int32 SimpleShapeTotal = 0;         // summed AggGeom counts across components with a body setup
        bool bAnyBodySetup = false;
        bool bAnyCollisionEnabled = false;
        bool bAnyRespondsToChannel = false;
        bool bAnyDrawnInThisChannel = false;

        // Classification buckets, mutually exclusive in the order tested by Classify().
        bool bNoCollision = false;      // enabled somewhere but zero simple primitives anywhere
        bool bComplexOnly = false;      // its geometry answers only as complex (UseComplexAsSimple)
        bool bComplexAsSimple = false;  // at least one component carries CTF_UseComplexAsSimple
        bool bUnknown = false;          // no body setup anywhere (landscape heightfields etc.)
    };

    // Build the per-actor report for one collision view. An actor with no primitive components
    // at all (pure logic actors, lights, volumes with no shape) comes back with
    // PrimitiveComponents == 0 and every bucket false - callers must skip those rather than
    // report them as collisionless, or the noCollision list fills with things that were never
    // meant to have collision.
    FActorCollisionReport BuildActorReport(AActor* Actor, EDrawChannel Channel);
}
