// Copyright (c) 2026 Alexander Penkin. MIT License.

// InstancedMeshUtils.h - the shared seams of the three per-instance verbs:
// actor.get_instances, actor.set_instance_transforms (Handlers/Actor/InstancedMeshHandler.cpp)
// and spatial.ground_instances (Handlers/Spatial/GroundPlacementHandler.cpp).
//
// WHY THESE VERBS EXIST AT ALL. An ISM/HISM instance is not addressable through the property
// layer. `property.set` / `container.array.set` on PerInstanceSMData[i].Transform DO store, and a
// readback DOES confirm the new matrix - and nothing moves. UInstancedStaticMeshComponent handles
// instance edits only in PostEditChangeChainProperty (InstancedStaticMesh.cpp:5577-5650), UObject
// calls chain -> non-chain and never the reverse, and the non-chain PostEditChangeProperty is the
// only notification this plugin emits (and must remain so: ISM dereferences
// PropertyChain.GetActiveMemberNode() unguarded, so a synthesised chain event crashes the editor).
// So the render tracker, the instance bodies, navigation and a HISM's cluster tree are never told.
// UInstancedStaticMeshComponent::UpdateInstanceTransform does all of that itself, and is the only
// write path any verb here uses.
//
// ONE FScopedTransaction PER BATCH, and the arithmetic that once argued against it was wrong.
// The retired rationale read: UpdateInstanceTransform calls Modify(), Modify() on an ISM
// serialises the ENTIRE PerInstanceSMData array, therefore a wrapped batch costs
// O(instances written x total instances). That multiplication does not happen.
// FTransaction::SaveObject (Editor/UnrealEd/Private/EditorTransaction.cpp) builds an FObjectRecord
// only when the object has none yet and merely increments SaveCount thereafter, so one
// FScopedTransaction around a whole batch costs exactly ONE full-object snapshot no matter how
// many instances it writes - ~1.3 MB on a 10k-instance scatter, the same price the editor already
// pays when a human drags one instance's gizmo. Both PostEditUndo overrides
// (InstancedStaticMesh.cpp, HierarchicalInstancedStaticMesh.cpp) already perform the same refresh
// FinishInstanceWrites does by hand, so Ctrl+Z reaches the state the verb reaches.
//
// The echoed pre-write record (movedInstances[]) is KEPT alongside it, because it does things a
// transaction stack cannot: selective and partial restore, restore past intervening edits, and a
// machine-readable diff of what a call did. It does NOT survive an editor restart - no handler
// persists an RPC response - it survives exactly as long as the caller's own transcript, which is
// a property of the caller.
//
// Because it can be LIFTED OUT of the response it arrived in, every row states its own space:
// a row is {index, space, previousTransform}, not {index, previousTransform}. The numbers are
// world- or component-local depending on the producing call's `space`, and the top-level echo is
// a sibling of the array rather than part of it - so a local-space record replayed at
// actor.set_instance_transforms' world default used to write component-local numbers as world
// coordinates, verify them in the same wrong space, and answer updated: N. See
// MakeMovedInstanceRow below and the row-space reconciliation in InstancedMeshHandler.cpp.
//
// Kept as inline functions in a NAMED namespace (not an anonymous one) so the handler translation
// units share one definition without an ODR error when Unity merges them - same rationale as the
// sibling SpawnMaterialUtils.h / ShapeSpawnUtils.h.
#pragma once

#include "CoreMinimal.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "Handlers/ErrorCodes.h"
#include "Misc/App.h"

namespace InstancedMeshUtils
{

// Which instanced component a call addresses, or why none could be chosen.
struct FInstancedComponentResolution
{
    UInstancedStaticMeshComponent* Component = nullptr;

    // Object names of every UInstancedStaticMeshComponent on the actor, in declaration order, so
    // a refusal can name what IS there instead of only what was not found.
    TArray<FString> Available;

    // Set exactly when Component is null. An ErrorCodes.h ERR_* value.
    const TCHAR* ErrorCode = nullptr;
    FString Error;
};

// Resolve the instanced component to address on Actor.
//
// An explicit ComponentName wins, matched case-insensitively against the component's object name.
// With none given, the actor's ONE instanced component is used - and if it carries more than one,
// the call is REFUSED. There is no default beyond that, deliberately.
//
// There used to be one: the component carrying the MOST instances. "Most instances" is a property
// of the LEVEL, not of the caller, and on AInstancedFoliageActor - a per-level singleton holding
// one component per foliage type for EVERY caller in the level - it resolves to whichever type
// happens to be biggest at that moment, which is not even stable between two calls while another
// caller paints. Because UFoliageInstancedStaticMeshComponent derives from
// UInstancedStaticMeshComponent, the sweep below enumerates the whole level's foliage, so the
// heuristic reached across caller boundaries by construction. Three incidents in one session
// relocated 2,048 instances of other callers' authored scatters that way, two of them
// unrecoverably, because the component was named in the response only AFTER the move.
//
// The refusal is the scoping surface, not merely a rejection: it names every candidate WITH its
// instance count, which is the "what could I have meant?" answer no other verb publishes, and the
// caller re-issues against one of them. GroundPlacement::FindInstancedHolder still resolves
// largest-wins, but only to NAME a component inside a refusal - a wrong guess there costs a
// confusing error string, and a wrong guess here cost someone else's scatter.
//
// A one-component actor keeps the convenience: there is nothing to disambiguate. That does not
// make a one-component AInstancedFoliageActor private - every caller's instances of that foliage
// type still live in it - but that is the mesh-keyed type name's problem, not this picker's.
//
// UHierarchicalInstancedStaticMeshComponent and UFoliageInstancedStaticMeshComponent both derive
// from UInstancedStaticMeshComponent, so one query covers every scatter shape. Unlike
// FindInstancedHolder this does NOT require two or more instances: a one-instance component is
// still addressable by index, it simply is not a scatter worth refusing.
inline FInstancedComponentResolution ResolveInstancedComponent(AActor* Actor,
                                                               const FString& ComponentName)
{
    FInstancedComponentResolution Resolution;
    if (!Actor)
    {
        Resolution.ErrorCode = ErrorCodes::ERR_ACTOR_NOT_FOUND;
        Resolution.Error = TEXT("No actor to read instances from.");
        return Resolution;
    }

    TArray<UInstancedStaticMeshComponent*> Instanced;
    Actor->GetComponents<UInstancedStaticMeshComponent>(Instanced);

    UInstancedStaticMeshComponent* Sole = nullptr;
    UInstancedStaticMeshComponent* Named = nullptr;
    for (UInstancedStaticMeshComponent* Component : Instanced)
    {
        if (!Component)
        {
            continue;
        }
        Resolution.Available.Add(Component->GetName());
        if (!ComponentName.IsEmpty() && !Named
            && Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
        {
            Named = Component;
        }
        if (!Sole)
        {
            Sole = Component;
        }
    }

    if (Resolution.Available.Num() == 0)
    {
        Resolution.ErrorCode = ErrorCodes::ERR_NO_INSTANCED_COMPONENT;
        Resolution.Error = FString::Printf(
            TEXT("'%s' carries no InstancedStaticMesh/HierarchicalInstancedStaticMesh component, so "
                 "it has no per-instance transforms. actor.get_components lists what it does carry; "
                 "an ordinary mesh actor is moved with actor.set_transform."),
            *Actor->GetActorLabel());
        return Resolution;
    }

    if (!ComponentName.IsEmpty() && !Named)
    {
        Resolution.ErrorCode = ErrorCodes::ERR_COMPONENT_NOT_FOUND;
        Resolution.Error = FString::Printf(
            TEXT("'%s' has no instanced component named '%s'. It carries: %s."),
            *Actor->GetActorLabel(), *ComponentName,
            *FString::Join(Resolution.Available, TEXT(", ")));
        return Resolution;
    }

    // The whole point of the ticket this guard closes: an omitted component on a multi-tenant
    // actor is a scope the caller never stated. Refused BEFORE any measurement or write, so a
    // mutating verb discloses what it would have addressed instead of reporting it afterwards.
    if (ComponentName.IsEmpty() && Resolution.Available.Num() > 1)
    {
        TArray<FString> Candidates;
        Candidates.Reserve(Resolution.Available.Num());
        for (const UInstancedStaticMeshComponent* Component : Instanced)
        {
            if (Component)
            {
                Candidates.Add(FString::Printf(TEXT("'%s' (%d instance(s))"),
                    *Component->GetName(), Component->GetInstanceCount()));
            }
        }
        Resolution.ErrorCode = ErrorCodes::ERR_AMBIGUOUS_INSTANCED_COMPONENT;
        Resolution.Error = FString::Printf(
            TEXT("'%s' carries %d instanced components, so 'component' is REQUIRED - name the one "
                 "you mean. NOTHING WAS READ OR WRITTEN. It carries: %s. There is no default: the "
                 "component with the most instances is a property of the level rather than of this "
                 "call, and on a shared holder - AInstancedFoliageActor keeps one component per "
                 "foliage type for every caller in the level - it is routinely someone else's "
                 "scatter. spatial.ground_actors and spatial.verify_grounding name the component "
                 "in their HOLDER_NOT_SEATABLE refusal, and actor.get_components lists them all."),
            *Actor->GetActorLabel(), Resolution.Available.Num(),
            *FString::Join(Candidates, TEXT(", ")));
        return Resolution;
    }

    Resolution.Component = ComponentName.IsEmpty() ? Sole : Named;
    return Resolution;
}

// Close out a batch of per-instance transform writes.
//
// Every write goes through UpdateInstanceTransform with bMarkRenderStateDirty=false, so the batch
// pays ONE render-state recreation instead of N.
//
// The HISM rebuild is not optional and not cosmetic. HISM's own UpdateInstanceTransform override
// queues an ASYNC, non-forced BuildTreeIfOutdated per moved instance
// (HierarchicalInstancedStaticMesh.cpp:2358-2363), so a batch leaves N async builds racing and the
// cluster tree - which culling and per-instance collision read - stale at the moment the verb
// answers. One synchronous forced rebuild collapses them, and it is the exact call, in the exact
// FApp::CanEverRender() guard, that Utils/ComponentAssetPropertyWrite.cpp:156-163 makes after a
// mesh swap and that HISM's own chain-property branch makes for a details-panel edit.
//
// MarkPackageDirty last, completing the house pattern
// Modify() -> mutate -> MarkRenderStateDirty() -> MarkPackageDirty(). Modify() is already done
// per instance inside UpdateInstanceTransform.
inline void FinishInstanceWrites(UInstancedStaticMeshComponent* Component)
{
    if (!Component)
    {
        return;
    }
    Component->MarkRenderStateDirty();
    if (UHierarchicalInstancedStaticMeshComponent* Hism =
            Cast<UHierarchicalInstancedStaticMeshComponent>(Component))
    {
        if (FApp::CanEverRender())
        {
            Hism->BuildTreeIfOutdated(/*Async*/ false, /*ForceUpdate*/ true);
        }
    }
    Component->MarkPackageDirty();
}

// {location:{x,y,z}, rotation:{pitch,yaw,roll}, scale:{x,y,z}} - the decomposed form, which is the
// point of these verbs: the reflected FInstancedStaticMeshInstanceData::Transform a caller reaches
// through property.get is a raw FMatrix in COMPONENT-LOCAL space, and every caller was decomposing
// it and composing the component transform by hand before comparing it to a raycast hit.
inline TSharedPtr<FJsonObject> TransformToJson(const FTransform& Transform)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
    const FVector Location = Transform.GetLocation();
    LocObj->SetNumberField(TEXT("x"), Location.X);
    LocObj->SetNumberField(TEXT("y"), Location.Y);
    LocObj->SetNumberField(TEXT("z"), Location.Z);
    Obj->SetObjectField(TEXT("location"), LocObj);

    TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
    const FRotator Rotation = Transform.GetRotation().Rotator();
    RotObj->SetNumberField(TEXT("pitch"), Rotation.Pitch);
    RotObj->SetNumberField(TEXT("yaw"), Rotation.Yaw);
    RotObj->SetNumberField(TEXT("roll"), Rotation.Roll);
    Obj->SetObjectField(TEXT("rotation"), RotObj);

    TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
    const FVector Scale = Transform.GetScale3D();
    ScaleObj->SetNumberField(TEXT("x"), Scale.X);
    ScaleObj->SetNumberField(TEXT("y"), Scale.Y);
    ScaleObj->SetNumberField(TEXT("z"), Scale.Z);
    Obj->SetObjectField(TEXT("scale"), ScaleObj);

    return Obj;
}

// The one spelling of the space token, shared by the echoes and by the per-row stamp so a
// producer and a consumer cannot disagree about the string.
inline const TCHAR* SpaceToken(bool bWorldSpace)
{
    return bWorldSpace ? TEXT("world") : TEXT("local");
}

// One movedInstances[] row: {index, space, previousTransform}.
//
// `space` sits INSIDE the row rather than only in the response's top-level echo, because the
// array is routinely lifted out of the response it arrived in - pasted into a script, stored,
// handed to another agent - and a bare {index, previousTransform} row cannot say whether its
// numbers are world or component-local. actor.set_instance_transforms reads the stamp back and
// either adopts it (when the replay states no `space` of its own) or REFUSES the batch (when it
// states a different one), so a record that cannot restore the scatter fails loudly instead of
// writing local numbers as world coordinates and reporting updated: N.
//
// Both producers - actor.set_instance_transforms and spatial.ground_instances - build rows here,
// so the stamp cannot be present on one verb's record and absent from the other's.
inline TSharedPtr<FJsonObject> MakeMovedInstanceRow(int32 Index, const FTransform& Previous,
                                                    bool bWorldSpace)
{
    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
    Row->SetNumberField(TEXT("index"), Index);
    Row->SetStringField(TEXT("space"), SpaceToken(bWorldSpace));
    Row->SetObjectField(TEXT("previousTransform"), TransformToJson(Previous));
    return Row;
}

// Identity fields every per-instance response repeats, so the three verbs cannot name the same
// component differently.
inline void WriteComponentIdentity(const TSharedPtr<FJsonObject>& Data, AActor* Actor,
                                   UInstancedStaticMeshComponent* Component)
{
    if (Actor)
    {
        Data->SetStringField(TEXT("actor"), Actor->GetActorLabel());
        Data->SetStringField(TEXT("actorPath"), Actor->GetPathName());
    }
    if (Component)
    {
        Data->SetStringField(TEXT("component"), Component->GetName());
        Data->SetStringField(TEXT("componentClass"),
            Component->GetClass() ? Component->GetClass()->GetName() : FString());
        Data->SetNumberField(TEXT("instanceCount"), Component->GetInstanceCount());
    }
}

} // namespace InstancedMeshUtils
