// Copyright (c) 2026 Alexander Penkin. MIT License.

// AnimationAuthoringHelpers.h
//
// Cross-cluster helpers shared by the per-file animation.authoring RPC
// translation units (Sequence, BlendSpace, AnimBlueprint). Extracted from the
// former monolithic AnimationAuthoringHandler.cpp so the clusters could be
// split into separate files without duplicating these (Unity ODR safety).
// Uses a named namespace (mirroring AnimGraphConstructionUtils) rather than an
// anonymous namespace in a header, per the plugin's Unity-build convention.

#pragma once

#include "CoreMinimal.h"
#include "AlphaBlend.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimStateMachineTypes.h"  // ETransitionLogicType
#include "Utils/AssetSaveState.h"

class FJsonObject;
class USkeleton;
class USkeletalMesh;
class UAnimSequence;
class UAnimSequenceBase;
class UBlendSpace;
struct FAnimNotifyEvent;
struct FBlendParameter;

namespace AnimationAuthoringHelpers
{
    // Places a point notify event at an absolute time on a sequence/montage.
    // Link() sets the FAnimLinkableElement LinkValue so GetTime() == Time; the
    // readers (list_notifies / asset.dump / the editor notify panel) report
    // GetTime(), NOT TriggerTimeOffset. Writing only TriggerTimeOffset (a small
    // runtime fudge offset) leaves LinkValue at 0, so every notify reports time 0
    // regardless of the requested time. Single-sourced so the authoring handlers
    // (add_notify, add_montage_notify) stay in lock-step with the canonical
    // animation.add_notify pattern.
    void LinkNotifyAtTime(FAnimNotifyEvent& Event, UAnimSequenceBase* Asset, float Time);

    // Clamps Event.TrackIndex to a non-negative value and grows Asset->AnimNotifyTracks
    // until that index is valid, adding default tracks the way the editor does. A notify
    // whose TrackIndex is not present in AnimNotifyTracks trips an editor-only ensure inside
    // UAnimSequenceBase::RefreshCacheData; fresh/programmatic assets can start with no notify
    // tracks, so the authoring add_notify* handlers must guarantee the track before refreshing.
    void EnsureNotifyTrack(UAnimSequenceBase* Asset, FAnimNotifyEvent& Event);

    // Resolves an anim-notify class by its already-prefixed name via FindFirstObject, but
    // returns null for an ABSTRACT class: instantiating the abstract UAnimNotify/UAnimNotifyState
    // base trips an editor ensure (StaticAllocateObjectErrorTests) and is nulled out on save. A
    // null return means "no concrete class" -- the notify handlers use it to attach a valid
    // name-only notify, while add_notify_state (which has no valid name-only form) rejects on it.
    UClass* ResolveConcreteNotifyClassOrNull(const FString& FullClassName);

    // Loads a USkeleton by path (after NormalizeContentAssetPath). Returns null if absent.
    USkeleton* LoadSkeletonFromPathAnim(const FString& SkeletonPath);

    // Loads a USkeletalMesh by path (after NormalizeContentAssetPath). Returns null if absent.
    USkeletalMesh* LoadSkeletalMeshFromPathAnim(const FString& MeshPath);

    // Loads a UAnimSequence by path (after NormalizeContentAssetPath). Returns null if absent.
    UAnimSequence* LoadAnimSequenceFromPath(const FString& AnimPath);

    // Loads a UAnimSequenceBase by path (after NormalizeContentAssetPath). Returns null if absent.
    UAnimSequenceBase* LoadAnimSequenceBaseFromPath(const FString& AnimPath);

    // Returns the enum literal name for an EAdditiveAnimationType value, or empty
    // if the enum cannot be resolved.
    FString AdditiveAnimTypeToString(EAdditiveAnimationType Type);

    // Maps a case-insensitive blend-option name to EAlphaBlendOption. Returns
    // false (Out untouched) on an unknown name.
    bool ParseAlphaBlendOption(const FString& Text, EAlphaBlendOption& Out);

    // Returns the transition LogicType as the string vocabulary the
    // animation.authoring.set_transition_settings RPC accepts ('StandardBlend' /
    // 'Inertialization' / 'Custom') — the engine enum literal with the TLT_ prefix
    // stripped. Single-sourced here so the dump builder (forward) and the authoring
    // handler (inverse, via ParseTransitionLogicType) stay symmetric. Falls back to
    // the numeric value string if the enum cannot be reflected.
    FString TransitionLogicTypeToString(ETransitionLogicType::Type LogicType);

    // Maps a case-insensitive logic-type name (see TransitionLogicTypeToString) to
    // ETransitionLogicType. Returns false (Out untouched) on an unknown name.
    bool ParseTransitionLogicType(const FString& Text, ETransitionLogicType::Type& Out);

    // UE 5.7+ safe asset "save": marks the package dirty and notifies the asset
    // registry instead of writing to disk (avoids modal save dialogs). The helper
    // deliberately does not claim that the edit reached disk: callers route the
    // returned state through AddAssetSaveReport. A requested mark-dirty is Deferred;
    // a request that was not made is NotRequested. A null asset is NotPersistable.
    // The bool is retained for source compatibility and means that the request was
    // accepted, not that the package was written.
    bool SaveAnimAsset(UObject* Asset, bool bShouldSave, EAssetSaveState& OutSaveState);

    // Reads an FVector from a JSON object's x/y/z fields (missing fields default
    // to 0). Returns the zero vector for an invalid object.
    FVector GetVectorFromJsonAnim(const TSharedPtr<FJsonObject>& Obj);

    // Reads an FRotator from a JSON object, accepting either Euler
    // (pitch/yaw/roll) or quaternion (x/y/z/w) fields. Returns the zero rotator
    // for an invalid object or when neither field set is present.
    FRotator GetRotatorFromJsonAnim(const TSharedPtr<FJsonObject>& Obj);

    // Resolves the protected UBlendSpace::BlendParameters array for in-place
    // writing and returns a pointer to its first element (index 0 = horizontal,
    // index 1 = vertical for 2D spaces). UE 5.7 removed UBlendSpaceBase and left
    // BlendParameters protected with a const-only accessor, so the only portable
    // write path (UE 5.3-5.7) is FProperty reflection on the protected array.
    // Single-sourced here so every blend-space axis write — create_blend_space_1d,
    // create_blend_space_2d, and the animation.create_blend_space configuration
    // path — uses the same property-name string and resolution, and a future
    // engine rename only changes this one site. Returns nullptr (logging a
    // Warning) if BlendSpace is null or the property cannot be reflected.
    FBlendParameter* GetBlendParametersForWrite(UBlendSpace* BlendSpace);
}
