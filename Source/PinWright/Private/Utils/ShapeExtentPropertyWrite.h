// Copyright (c) 2026 Alexander Penkin. MIT License.

// ShapeExtentPropertyWrite.h - the UShapeComponent size UPROPERTYs whose reflection write
// updates every representation EXCEPT the live physics body, and the engine setters that
// close that half.
//
// THE BUG THIS CLOSES. actor.set_component_properties stores by reflection, fires
// PostEditChangeProperty, then commits with MarkRenderStateDirty() +
// UpdateComponentToWorld(). For UBoxComponent::BoxExtent that produces a component whose
// renderer, bounds and actor.get_bounding_box all report the NEW size while every scene
// query - line trace, sweep, overlap, anything that reaches FBodyInstance - still measures
// the OLD one. Measured on a TriggerBox: BoxExtent 40 -> 400 returned
// {"applied":["BoxExtent"],"notified":["BoxExtent"]}, get_bounding_box returned 400, a
// fixed-pose capture showed the wireframe grow from 46 px to filling a 768 px frame, and a
// raycast at the new face came back BYTE-IDENTICAL to the pre-write baseline. Shrinking is
// the worse direction: 400 -> 150 leaves a body wider than anything drawn - an invisible
// collider standing in empty space that no capture can show.
//
// WHY THE NOTIFICATION IS NOT ENOUGH. UShapeComponent::PostEditChangeProperty
// (ShapeComponent.cpp:157-164) does exactly one thing, UpdateBodySetup(), and that half was
// never the stale half: UShapeComponent::GetBodySetup() calls UpdateBodySetup() on the way
// past (:100-103), so AggGeom self-repairs for any reader. What is stale is the Chaos
// geometry already handed to the scene, built once at CreatePhysicsState() time from the
// AggGeom of that moment. Nothing in the notification path re-reads it, and
// MarkRenderStateDirty() rebuilds the scene PROXY - the half that was already correct.
//
// THE CALL THAT ACTUALLY CLOSES IT. FBodyInstance::UpdateBodyScale(Scale3D, bForceUpdate)
// (BodyInstance.cpp:2204) walks the live FPhysicsShapeHandles, reads each shape's
// FKShapeElem back out of its user data - the AggGeom element UpdateBodySetup() has just
// refreshed - and rebuilds the Chaos implicit geometry from it. bForceUpdate is
// load-bearing: an extent edit does not change the component scale, so without it the
// function returns early at :2214 and nothing moves. That is why the measured workaround -
// calling UBoxComponent::SetBoxExtent with the value the reflection write had ALREADY
// stored - resynchronises the body: the work is the body update, not the assignment.
//
// WHY THE TYPED SETTER RATHER THAN A HANDLER-SIDE UpdateBodyScale. Same trade as
// ComponentAssetPropertyWrite.h: the setter is the superset (UpdateBounds,
// MarkRenderStateDirty, UpdateBodySetup, UpdateBodyScale, UpdateOverlaps) in the engine's
// own order and under its own bPhysicsStateCreated guard, and cannot be mis-shaped. A
// per-class UpdateBodyScale call from the handler would have to re-derive that order and
// re-guard it, for each class, from outside. RecreatePhysicsState() also closes the gap but
// destroys and rebuilds the whole body for a size change the engine handles in place, and
// would have to fire on every notified write to be a principled rule rather than a
// property list.
//
// THE WHOLE SET, ENUMERATED FROM ENGINE SOURCE. UE 5.8 has exactly three concrete
// UShapeComponent subclasses (BoxComponent.h:18, SphereComponent.h:17,
// CapsuleComponent.h:16), carrying four size UPROPERTYs between them. Each has a typed
// setter ending in the same bPhysicsStateCreated -> UpdateBodyScale(..., true) tail:
//   * UBoxComponent::BoxExtent          -> SetBoxExtent          (BoxComponent.cpp:28-47)
//   * USphereComponent::SphereRadius    -> SetSphereRadius       (SphereComponent.cpp:83-100)
//   * UCapsuleComponent::CapsuleHalfHeight -> SetCapsuleHalfHeight (CapsuleComponent.h:197)
//   * UCapsuleComponent::CapsuleRadius     -> SetCapsuleRadius     (CapsuleComponent.h:192)
// Matching is by concrete component cast, so engine and plugin subclasses
// (UDrawSphereComponent and friends) are covered by their base.
//
// THE CAPSULE DIVERGENCE, STATED BECAUSE IT IS A REAL BEHAVIOUR CHANGE. For CapsuleRadius
// the setter is NOT a superset of the notification, it is a different rule.
// UCapsuleComponent::PostEditChangeProperty clamps the radius DOWN to the existing
// half-height (CapsuleComponent.cpp:160-163); SetCapsuleRadius -> SetCapsuleSize keeps the
// requested radius and grows the half-height UP to match (:171-172). Routing takes the
// setter's rule deliberately: it is the engine's own Blueprint-exposed API for that
// property, it is the only one of the two that also updates the body, and honouring the
// requested value beats silently discarding half of it. CapsuleHalfHeight has no
// divergence - both routes are Max3(0, NewHalfHeight, CapsuleRadius).
//
// RESPONSE SHAPE. These four move from `notified` to `applied`, exactly as StaticMesh and
// SkinnedAsset did, because the setter supersedes the notification (docs/rpc-design.md 5c).
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

class FProperty;
class UActorComponent;

namespace PinWright
{
    enum class EShapeExtentWrite : uint8
    {
        // Not a shape size property on this component. The caller must fall through to
        // its normal ApplyJsonValueToProperty path; nothing has been written.
        NotApplicable,
        // Written through the engine setter, so field, bounds, render state, body setup
        // AND live body all describe the new size.
        Applied,
        // Rejected. OutError carries the reason in the same shape a generic property
        // failure uses, so callers can put it in `warnings` unchanged.
        Failed,
    };

    // Routes UShapeComponent size writes through the engine's typed setters. The JSON
    // value is resolved by the SHARED importer (landed on the real property and taken
    // straight back out) so every accepted syntax and every error string stays identical
    // to a generic property write - duplicating that ladder here is how the two drift.
    EShapeExtentWrite ApplyShapeExtentProperty(
        UActorComponent* Component,
        FProperty* Property,
        const TSharedPtr<FJsonValue>& ValueField,
        FString& OutError);
}
