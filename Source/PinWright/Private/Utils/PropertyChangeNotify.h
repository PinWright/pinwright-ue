// Copyright (c) 2026 Alexander Penkin. MIT License.

// PropertyChangeNotify.h - fire the engine's own change notification for a UPROPERTY
// that was just written by reflection.
//
// THE BUG THIS CLOSES. A generic `properties: {Name: value}` loop resolves an FProperty
// by name and stores through ApplyJsonValueToProperty. That store is the WHOLE of the
// write: for a large class of engine properties the observable effect does not live in
// the field at all, it lives in the class's PostEditChangeProperty override, and a raw
// store runs none of it. The write lands, the read-back is correct, MarkPackageDirty
// records a change - and nothing the property controls actually moves.
//
// Measured instance. actor.set_component_properties assigning a WaterBodyRiver's
// WaterMaterial to None and back rendered PIXEL-IDENTICAL (mean luma 0.4493 before and
// after a 2x Scattering change). The same assignment driven through Python
// set_editor_property rebuilt the MID (WaterMID_0 -> WaterMID_3) and moved mean luma to
// 0.4743. The chain the raw store misses is exact:
//   UWaterBodyComponent::PostEditChangeProperty (WaterBodyComponent.cpp:1368)
//     -> OnPostEditChangeProperty (:1244), WaterMaterial branch (:1252)
//     -> UpdateMaterialInstances (:1019) -> CreateOrUpdateWaterMID (:1057)
//     -> FWaterUtils::GetOrCreateTransientMID (:1062)
// UpdateMaterialInstances is also the only caller of MarkOwningWaterZoneForRebuild
// (:1027), so the handler's MarkRenderStateDirty() cannot substitute for it: the water
// zone renders the body from the stale MID no matter how often the component's own
// scene proxy is recreated.
//
// WHY set_editor_property WORKS AND THE SETTER DID NOT. Python's UObject setter routes
// through FPropertyAccessUtil, which builds a real change notify
// (PropertyAccessUtil.cpp:599, EPropertyChangeType::ValueSet) and emits
// PostEditChangeChainProperty (:795). The plugin's reflection path emits nothing -
// ApplyJsonValueToProperty takes a `void* TargetContainer` (PropertyImport.h:10) and so
// cannot notify even in principle; only the call site holds the UObject.
//
// WHY THE NON-CHAIN FORM, AND ONLY THE NON-CHAIN FORM. Synthesising the chain form is
// unsafe: UInstancedStaticMeshComponent::PostEditChangeChainProperty dereferences
// PropertyChain.GetActiveMemberNode() with no null check (InstancedStaticMesh.cpp:5638),
// so a hand-built empty chain crashes the editor for any property outside its three
// known branches. FPropertyChangedEvent(Property, ValueSet) sets MemberProperty =
// Property (UnrealType.h:6977-6985), which is the shape every name-matched engine branch
// reads, and UObject::PostEditChangeProperty never touches a chain.
//
// WHY NO PreEditChange. UActorComponent::PreEditChange unregisters the component and
// calls FlushRenderingCommands (ActorComponent.cpp:1327, :1336-1339) - the cost
// EnvironmentDirtyUtils.h:14-16 rejected, correctly, for loop-driven verbs. It also
// populates EditReregisterContexts, which is the ONLY thing that makes
// ConsolidatedPostEditChange rerun the owner's construction scripts
// (ActorComponent.cpp:1437-1446). Notifying without it therefore costs no flush, no
// component re-registration and no construction-script rerun - so the target object
// cannot be destroyed out from under the caller by its own notification.
//
// NOT A SUBSTITUTE FOR A TYPED SETTER. Where the engine exposes one, route to it
// instead: it is the superset and cannot be mis-shaped (Utils/ComponentAssetPropertyWrite.h
// for StaticMesh / SkinnedAsset, docs/rpc-design.md 5c). This header is for the far
// larger set of properties that have no reachable setter and whose entire effect is the
// notification.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"

namespace PinWright
{
    // Emits Object->PostEditChangeProperty(FPropertyChangedEvent(Property, ChangeType)).
    //
    // Call ONLY after a raw reflection store / container mutation on Object that
    // succeeded.
    //
    // Property is the LEAF that was written. MemberProperty is the TOP-LEVEL UPROPERTY
    // of Object that contains it, and matters because engine overrides split on BOTH
    // names: GetPropertyName() reads Property, GetMemberPropertyName() reads
    // MemberProperty, and a nested write that names only the leaf misses every
    // member-matched branch (UWaterBodyComponent's LayerWeightmapSettings /
    // StaticMeshSettings branches, WaterBodyComponent.cpp:1248, :1277). Pass nullptr for
    // a top-level write: FPropertyChangedEvent's constructor already sets
    // MemberProperty = Property (UnrealType.h:6977-6985), which is the shape a
    // single-segment write needs.
    //
    // ChangeType tells the override WHAT happened, not just where. UE's own property
    // editor uses ValueSet for a value write and ArrayAdd / ArrayRemove / ArrayClear for
    // an array, set OR map shape change (PropertyHandleImpl.cpp:1274-1278, :1438-1443,
    // :1616-1626, :1911-1915), and overrides branch on it: passing the wrong one runs a
    // rebuild the edit did not call for, or skips the one it did.
    //
    // Returns true when a notification was emitted; false for a null object/property or
    // an object already pending kill. The return is the fact a caller may report - it
    // says the engine's change path RAN, not that anything downstream changed.
    inline bool NotifyPropertyChanged(UObject* Object, FProperty* Property,
                                      FProperty* MemberProperty = nullptr,
                                      EPropertyChangeType::Type ChangeType = EPropertyChangeType::ValueSet)
    {
#if WITH_EDITOR
        if (!Object || !Property || !IsValid(Object))
        {
            return false;
        }

        FPropertyChangedEvent ChangedEvent(Property, ChangeType);
        if (MemberProperty && MemberProperty != Property)
        {
            ChangedEvent.SetActiveMemberProperty(MemberProperty);
        }
        Object->PostEditChangeProperty(ChangedEvent);
        return true;
#else
        return false;
#endif
    }
}
