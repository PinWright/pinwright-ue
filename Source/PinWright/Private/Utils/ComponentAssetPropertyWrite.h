// Copyright (c) 2026 Alexander Penkin. MIT License.

// ComponentAssetPropertyWrite.h - the small set of component UPROPERTYs that a raw
// reflection write CORRUPTS, and the engine setters that must be used instead.
//
// THE BUG THIS CLOSES. A generic `properties: {Name: value}` loop resolves an
// FProperty by name and writes it through ApplyJsonValueToProperty. For almost every
// property that is correct. For three engine properties it is not: the engine keeps a
// PRIVATE shadow copy of the assigned asset, updates the shadow only from its own
// setter, and ensures whenever the two diverge. Writing the property directly leaves
// the shadow stale, so the component's cached render/streaming/physics state still
// describes the OLD asset while the serialized property holds the new one - a
// divergence that survives into the saved package.
//
// Observed, on the production map, from actor.set_component_properties:
//   Ensure condition failed: KnownStaticMesh == StaticMesh
//   StaticMesh property overwritten for component HierarchicalInstancedStaticMeshComponent
//   ...HISM_Foliage_Conifer without a call to NotifyIfStaticMeshChanged().
// raised at StaticMeshComponent.cpp:744 from the UpdateComponentToWorld() call the
// handler made straight after the write (ComponentHandler.cpp, the SceneComponent
// commit block).
//
// THE WHOLE CLASS, ENUMERATED FROM ENGINE SOURCE. UE 5.8 has exactly three
// shadow-copy properties, found by grepping the ensure text
// "without a call to Notify":
//   * UStaticMeshComponent::StaticMesh   / KnownStaticMesh   (StaticMeshComponent.cpp:746)
//   * USkinnedMeshComponent::SkinnedAsset/ KnownSkinnedAsset (SkinnedMeshComponent.cpp:6052)
//   * UTexture::CompositeTexture         / KnownCompositeTexture (Texture.cpp:1235)
// The first two are component properties and are handled here. The third is an ASSET
// property, unreachable from any component-property verb; a verb that writes UTexture
// fields generically would need the same treatment and does not get it here.
//
// WHY NOT PostEditChangeProperty FOR EVERYTHING. UStaticMeshComponent::
// PostEditChangeProperty does call NotifyIfStaticMeshChanged(), so the details-panel
// path is safe - but routing every generic write through it is the wrong trade twice
// over. It would not be enough (the HISM cluster-tree rebuild for a mesh swap lives in
// PostEditChangeChainProperty, HierarchicalInstancedStaticMesh.cpp:2110-2130), and the
// chain form is actively unsafe to synthesize: UInstancedStaticMeshComponent::
// PostEditChangeChainProperty dereferences PropertyChain.GetActiveMemberNode() without
// a null check (InstancedStaticMesh.cpp:5638), so a hand-built empty chain crashes the
// editor for any property outside its known set. The engine's own typed setter does
// strictly more than the notification (PSO precache, physics state, streaming, bounds,
// navigation) and cannot be mis-shaped, so that is what this routes to. It also leaves
// the house pattern - Modify() -> mutate -> MarkRenderStateDirty() -> MarkPackageDirty()
// with no PreEditChange (SpawnMaterialUtils.h, EnvironmentDirtyUtils.h, GeometryUtils.h)
// - deliberately intact: no FComponentReregisterContext, no FlushRenderingCommands.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

class FProperty;
class UActorComponent;

namespace PinWright
{
    enum class EComponentAssetWrite : uint8
    {
        // Not one of the shadow-copy properties. The caller must fall through to
        // its normal ApplyJsonValueToProperty path; nothing has been written.
        NotApplicable,
        // Written through the engine setter AND verified against engine state.
        Applied,
        // Rejected. OutError carries the reason, in the same shape a generic
        // property failure uses, so callers can put it in `warnings` unchanged.
        Failed,
    };

    // Routes StaticMesh / SkinnedAsset writes through UStaticMeshComponent::SetStaticMesh
    // and USkinnedMeshComponent::SetSkinnedAssetAndUpdate respectively, and confirms the
    // result by READING THE COMPONENT BACK - both setters can decline silently (a Static
    // mobility component during play, an asset of the wrong class), and a caller that
    // trusted the void/bool return would report success over an unchanged component.
    EComponentAssetWrite ApplyComponentAssetProperty(
        UActorComponent* Component,
        FProperty* Property,
        const TSharedPtr<FJsonValue>& ValueField,
        FString& OutError);
}
