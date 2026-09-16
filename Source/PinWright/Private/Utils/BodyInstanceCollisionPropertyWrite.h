// Copyright (c) 2026 Alexander Penkin. MIT License.

// BodyInstanceCollisionPropertyWrite.h - the collision fields of UPrimitiveComponent::
// BodyInstance, which a raw reflection write stores WITHOUT reaching the live Chaos filter
// data, and which on an ISM/HISM do not decide collision at all.
//
// THE BUG THIS CLOSES, IN TWO LAYERS.
//
// Layer one, every primitive. actor.set_component_properties accepts
// {"BodyInstance": {...}} because BodyInstance is a reflected UPROPERTY
// (PrimitiveComponent.h:1443-1444), recurses per sub-property through the importer's
// FStructProperty branch, and stores. It never calls SetCollisionResponseToChannel /
// SetCollisionProfileName / SetCollisionEnabled, and the non-chain PostEditChangeProperty
// the handler fires afterwards cannot substitute: UPrimitiveComponent::
// PostEditChangeProperty (PrimitiveComponent.cpp:1541-1643) branches on
// LDMaxDrawDistance, bAllowCullDistanceVolume, bNeverDistanceCull, Mobility,
// MinDrawDistance, bLightAttachmentsAsGroup, FirstPersonPrimitiveType and
// CustomPrimitiveData - there is NO BodyInstance branch. So the field carries the new
// value, the package serialises it, and the live body keeps the old filter data. A
// CollisionProfileName write is worse than stale: the raw store writes the NAME without
// running LoadProfileData, so the profile's responses and CollisionEnabled are never
// applied and the component reports a profile it does not implement.
//
// Layer two, instanced primitives, and this is the one that survives a read-back. A
// UInstancedStaticMeshComponent draws and collides through PER-INSTANCE FBodyInstances.
// Its own inherited BodyInstance owns no shapes: it is a TEMPLATE copied into each
// instance body exactly once, at creation
// (FInstancedMeshComponentBodies::CreateAll -> Instance->CopyBodyInstancePropertiesFrom(
// &ReferenceBody), InstancedMeshComponentBodies.cpp:105), and never consulted again.
// Measured on /Game/Maps/PW_VegetationTest: ECC_Pawn -> Ignore on 12 HISM/foliage-ISM
// components (5527 instances) returned OK 12/12 with a REAL read-back of ECR_Ignore and a
// profile that flipped BlockAllDynamic -> Custom, and a pawn-profile capsule sweep taken
// immediately afterwards still named 7 of those 12 as blocking hits. Board ticket
// B-collision-write-skips-instance-bodies.
//
// WHY THE ENGINE SETTER ALONE IS NOT ENOUGH. UPrimitiveComponent::
// SetCollisionResponseToChannel (PrimitiveComponentPhysics.cpp:1352) writes the template
// and calls OnComponentCollisionSettingsChanged, which clears the overlap-skip cache,
// updates overlaps, refreshes navigation relevancy and broadcasts a delegate - it touches
// no body and no filter data - and neither InstancedStaticMesh.cpp nor
// HierarchicalInstancedStaticMesh.cpp overrides it. The engine's setter IS what the field
// pass used, and it left 7 of 12 components blocking.
//
// THE ENGINE'S OWN PROPAGATION. FBodyInstance::CopyRuntimeBodyInstancePropertiesFrom
// (BodyInstance.cpp:3047) copies exactly CollisionResponses, CollisionProfileName and
// CollisionEnabled from a template onto a live body and ends in UpdatePhysicsFilterData().
// It is the function FInstancedMeshComponentBodies::InitInstanceBody already uses for its
// runtime path (InstancedMeshComponentBodies.cpp:43), so this is the engine's answer to
// "push the component's collision settings onto an instance body", not a hand-rolled shape
// walk.
//
// A BARE UpdatePhysicsFilterData() LOOP - the shape of the ISM's own
// OnActorEnableCollisionChanged override (InstancedStaticMesh.cpp:5023-5034) - would NOT
// work here, and the reason is worth stating because that override looks like the fix.
// FBodyInstance::BuildBodyFilterData reads InInstance->CollisionResponses and
// InInstance->ObjectType, i.e. the INSTANCE body's own copies (BodyInstance.cpp:4779-4783).
// Rebuilding filter data from a stale copy reproduces the stale filter data. That override
// works only because the value it propagates - actor-level enable-collision - is read back
// off the OWNER through GetCollisionEnabled_CheckOwner (BodyInstance.cpp:927-945) rather
// than off the body. ObjectType is the one field CopyRuntimeBodyInstancePropertiesFrom does
// not carry, so it is pushed separately through FBodyInstance::SetObjectType.
//
// WHY NOT RecreatePhysicsState(). It works - it is what the measured workaround
// (SetCollisionEnabled(NoCollision) then (QueryAndPhysics)) achieves by accident - but it
// destroys and rebuilds every instance body for a filter change the engine updates in
// place, on components that routinely carry thousands of them. It is used here only where
// the engine itself uses it: a CollisionEnabled change, where the state must be created or
// destroyed rather than refiltered.
//
// THE RESTORE DANCE, AND WHY IT IS NOT OPTIONAL. Every engine collision setter early-outs
// on "the value is already that" - SetCollisionProfileName compares against
// GetCollisionProfileName(), SetResponseToChannels returns false from
// SetCollisionResponseContainer, SetCollisionEnabled compares against
// GetCollisionEnabled(). The importer has ALREADY stored the requested value by the time
// this code runs, so calling the setter straight afterwards is a guaranteed no-op. The four
// collision fields are therefore snapshotted, handed to the importer, read back, restored,
// and only then replayed through the setters. Restoring uses the reflected sub-properties
// of FBodyInstance because all four are declared `private` (BodyInstance.h:377, :392, :568,
// :586) - the reflected name is the stable identity anyway, and is what the importer landed
// on to produce the new value at all. Same reasoning as ShapeExtentPropertyWrite.h.
// Nothing observes the intermediate state: handlers run to completion on the game thread
// with no tick, no GC and no render sync between these statements.
//
// THE READ-BACK IS THE OTHER HALF. The measured pass read
// GetCollisionResponseToChannel(ECC_Pawn) off the COMPONENT and was told the write had
// landed - the read walked to the same memory the write did, and that memory is the half
// that was right. So the measurement published here is taken off the PER-INSTANCE BODIES,
// the objects the physics scene consults, and it is OMITTED rather than faked when they
// cannot be inspected (no physics state, no instances). Requested and measured are named
// separately and a disagreement is a warning; see docs/rpc-design.md.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

class FProperty;
class UActorComponent;
class FJsonObject;

namespace PinWright
{
    enum class EBodyInstanceCollisionWrite : uint8
    {
        // Not a BodyInstance write on a primitive component, the request names no collision
        // field (a mass or damping write keeps its existing store-plus-notify path
        // untouched), or the collision fields could not be resolved by reflection. The
        // caller must fall through to its normal ApplyJsonValueToProperty path; nothing has
        // been written.
        NotApplicable,
        // Written: the non-collision sub-fields by the shared importer, the collision ones
        // through the engine setters, and - on an instanced component - pushed down onto the
        // per-instance bodies.
        Applied,
        // Rejected by the importer. OutError carries the reason in the same shape a generic
        // property failure uses, so callers can put it in `warnings` unchanged.
        Failed,
    };

    // One collision field the request actually changed, as a requested/measured pair.
    // Measured is read off the per-instance bodies; it stays empty when they could not be
    // inspected, and the field is then published without a measurement rather than with a
    // fabricated one.
    struct FBodyInstanceCollisionField
    {
        // "CollisionEnabled", "CollisionProfileName", "ObjectType", or the engine name of a
        // collision channel whose response changed ("ECC_Pawn").
        FString Name;
        FString Requested;
        FString Measured;
        bool bMeasured = false;
        // Every valid per-instance body reported the same Measured value. False means the
        // bodies disagree with each other, which is itself a partial-propagation signal.
        bool bUnanimous = true;
    };

    // What the objects that actually decide collision report after the write.
    struct FBodyInstanceCollisionMeasurement
    {
        // A collision field was written at all. Nothing is published when false.
        bool bCollisionWritten = false;
        // The target is a UInstancedStaticMeshComponent, so the per-instance bodies - not
        // the component's BodyInstance - are what the physics scene consults.
        bool bInstancedComponent = false;
        // At least one live per-instance body could be read back.
        bool bBodiesInspectable = false;
        // Why not, when bInstancedComponent && !bBodiesInspectable. Published as a warning.
        FString NotInspectableReason;
        // Valid per-instance bodies found, and how many the propagation ran on.
        int32 BodyCount = 0;
        int32 BodiesRefreshed = 0;
        FString ComponentName;
        TArray<FBodyInstanceCollisionField> Fields;
    };

    // Routes UPrimitiveComponent::BodyInstance collision sub-field writes through the engine
    // setters and pushes them onto the per-instance bodies of an instanced component. Every
    // other sub-field of the struct keeps the existing raw-reflection behaviour, so a mass or
    // damping write is unaffected.
    EBodyInstanceCollisionWrite ApplyBodyInstanceCollisionProperty(
        UActorComponent* Component,
        FProperty* Property,
        const TSharedPtr<FJsonValue>& ValueField,
        FBodyInstanceCollisionMeasurement& OutMeasurement,
        FString& OutError);

    // Dot-path form of the routine above, for the verbs that address a sub-field by path
    // instead of handing over the whole struct: PropertyPath is "BodyInstance" with an
    // object value, or "BodyInstance.CollisionEnabled" (any depth) with that leaf's own
    // value. The path tail is folded back into the nested-object shape and handed to
    // ApplyBodyInstanceCollisionProperty, so both spellings resolve to ONE implementation
    // and cannot drift apart. A path whose head is not BodyInstance, or whose leaf is not
    // a collision field, returns NotApplicable with nothing written.
    //
    // WHY A BLUEPRINT COMPONENT TEMPLATE NEEDS THIS AS MUCH AS A LIVE COMPONENT, which is
    // not obvious because a template has no physics state to refilter. On a template the
    // collision fields are not stored state at all - they are RE-DERIVED FROM
    // CollisionProfileName on every instance the template spawns, and the derivation runs
    // AFTER the archetype's values have been copied in, so it is the last writer:
    //
    //   USCS_Node::ExecuteNodeOnActor (SCS_Node.cpp:84-103)
    //     -> AActor::CreateComponentFromTemplate (ActorConstruction.cpp:1112-1153)
    //     -> StaticDuplicateObjectEx (:1140) - a serialise-based duplicate whose FlagMask
    //        strips RF_ArchetypeObject (:1138), so the new component is NOT a template
    //     -> ConditionalPostLoad, run for exactly the non-template duplicates
    //        (UObjectGlobals.cpp:3152-3159)
    //     -> UPrimitiveComponent::PostLoad -> BodyInstance.FixupData(this), whose guard is
    //        `!IsTemplate()` and therefore fires (PrimitiveComponent.cpp:1810-1821)
    //     -> FBodyInstance::LoadProfileData(false) (BodyInstance.cpp:4566, :4475-4535)
    //     -> UCollisionProfile::ReadConfig, which assigns CollisionEnabled, ObjectType and
    //        the whole response container straight off the profile template
    //        (CollisionProfile.cpp:197-199).
    //
    // The default profile is a real one - every UPrimitiveComponent constructor runs
    // SetCollisionProfileName(BlockAll_ProfileName) (PrimitiveComponent.cpp:361) - so the
    // profile that gets re-applied is BlockAll, i.e. QueryAndPhysics. A raw reflection store
    // of CollisionEnabled therefore lands on the template, serialises, reads back correctly
    // off the template, and is overwritten on every single spawned instance. That is the
    // reported symptom exactly: `set_property` returning success while the spawned component
    // still reports QUERY_AND_PHYSICS.
    //
    // The invariant that makes the write survive is the one the engine setters maintain and
    // a raw store breaks. SetCollisionEnabled, SetObjectType and SetResponseToChannel(s) all
    // call InvalidateCollisionProfileName() (BodyInstance.cpp:564-569, :571-593, :675-680,
    // :742-748), which moves the profile to "Custom" - and "Custom" is one of the two names
    // IsValidCollisionProfileName rejects (:4470-4473), so LoadProfileData finds nothing to
    // re-derive from and the stored value stands. SetCollisionProfileName is the mirror
    // case: it runs LoadProfileData itself (:698-717), so the profile's CollisionEnabled and
    // responses are actually applied instead of a name being stored that the component does
    // not implement. Board ticket B-scs-set-property-bodyinstance-collision-silent-noop.
    EBodyInstanceCollisionWrite ApplyBodyInstanceCollisionPropertyPath(
        UActorComponent* Component,
        const FString& PropertyPath,
        const TSharedPtr<FJsonValue>& ValueField,
        FBodyInstanceCollisionMeasurement& OutMeasurement,
        FString& OutError);

    // Publishes the measurement on a handler response as `instanceBodies`, and appends a
    // warning for each disagreement between requested and measured plus one for bodies that
    // could not be inspected. Adds nothing at all when no collision field was written, and
    // omits the `measured` object rather than inventing values.
    void AddBodyInstanceCollisionReport(
        const FBodyInstanceCollisionMeasurement& Measurement,
        const TSharedPtr<FJsonObject>& Data,
        TArray<FString>& OutWarnings);
}
