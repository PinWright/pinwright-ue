# animation.authoring

Fine-grained authoring of skeletal-animation assets: anim sequences, montages, composites, anim blueprints with state machines, blend spaces / aim offsets, and control rigs / IK rigs / retargeters. Use this namespace when you need per-element control over an existing asset; reach for `call("animation")` when one top-level call can settle the whole operation.

## Sub-workflows

The cluster groups six workflows that share asset paths but compose independently:

- **Anim sequences** - `create_animation_sequence` -> `set_sequence_length`, bone tracks (`add_bone_track`, `set_bone_track_keys`), curves (`set_curve_key`, `list_curves`), notifies (`add_notify`, `add_notify_state`, `set_notify_state_property`, `list_notifies`), sync markers (`add_sync_marker`, `set_sync_markers`, `remove_sync_marker`, `list_sync_markers`), and additive/root-motion/interpolation settings.
- **Montages** - `create_montage` -> `add_montage_slot`, `add_montage_section`, `link_sections`, `set_section_timing`, `set_blend_in`, `set_blend_out`, `add_montage_notify`.
- **Anim composites** - `create_composite` + `add_composite_segment` stitch `UAnimSequenceBase` clips into one lightweight playback asset without montage slots or sections.
- **Anim blueprints + state machines** - `create_anim_blueprint`, state-machine/state/transition verbs, `set_transition_rules`, graph-node and blend/slot/pose/IK mutators, `bind_player_asset`, `set_sync_group`, and `set_anim_graph_node_value`.
- **Blend spaces / aim offsets** - `create_blend_space_1d`, `create_blend_space_2d`, or `create_aim_offset`, then `add_blend_sample` / `add_aim_offset_sample`.
- **Control rigs / IK** - `create_control_rig` + `call("controlrig.compile_crir")`; `create_ik_rig` + `add_ik_chain`; `create_ik_retargeter` + `set_retarget_chain_mapping`; `create_pose_library`. The removed per-element stubs (`add_control`, `add_rig_unit`, `connect_rig_elements`) were no-ops; CRIR is the canonical Control Rig graph surface.

## Inspect-after-mutate

Most authoring methods take an `asset_path` to an existing asset and mutate it in place. Use `get_animation_info` before mutating to confirm its shape. For `UAnimSequence`, the legacy numeric `frameRate` remains; dump-parity additions are `frameRateRational` (`{numerator, denominator}`), `additiveType`, `rawTrackCount`, per-bone `boneTracks` (below), and `skeletonAssetPath`. `frameRate` is not an object.

`get_animation_info` is namespaced here under `animation.authoring`, not at top level - there is no `animation.get_animation_info`. For the full dump-parity read of a sequence (the right reader when you just want a clip's length / frame count) use the top-level [`animation.describe_sequence`](animation.md) instead; it returns the complete `anim_sequence.json` shape, whereas `get_animation_info` is the thinner authoring introspection read.

`get_animation_info` on a `USkeleton` is bare: only `{assetType:"Skeleton"}`, with no bone list or hierarchy. To find `startBone` / `endBone` for `add_ik_chain`, use [`skeleton.list_bones`](skeleton.md), not `get_animation_info`.

Use `list_curves`, `list_notifies`, and `list_sync_markers` for the `anim_sequence.json` array shapes emitted by `asset.dump`. They delegate to `AnimSequenceDumpBuilder`: curves return Float / Transform `{name, type, keyCount}` (Transform `keyCount` sums translation, rotation, and scale X/Y/Z channels); notifies return sorted `{name, time, duration, branchingPoint}` for any `UAnimSequenceBase`; sync markers return sorted `{name, time}` for authored `UAnimSequence` markers.

**Sync-marker authoring**

Sync markers are timeline labels on an existing `UAnimSequence`. `add_sync_marker` appends one
from an integer `frame` and returns `assetPath`, measured `syncMarkers`, and the normal
save/verification fields. `set_sync_markers` atomically replaces the array of `{name, frame}`
objects; an empty array clears it. Every name and frame is validated before writing, and the
response returns the measured `{name, time}` dump shape, so a failed item cannot partially replace it.

Marker names may repeat at different frames, including the same label at two loop phases.
`remove_sync_marker` removes every occurrence of `markerName`, returns `removedCount`, and
includes the remaining measured `syncMarkers`. All three mutators take `assetPath` and optional
`save`, following the usual dirty/save report contract. After any write, PinWright refreshes
Unreal's authored-marker derived state and per-track links. `asset.dump` emits the same
`syncMarkers` array in `anim_sequence.json`, so moving a marker produces a mirror diff.

**Bone-track authoring and readback.** `add_bone_track` seeds a new track with reference-pose keys, not an empty track. `set_bone_track_keys` overwrites one track with dense local-space position, quaternion rotation, and scale arrays. Required `positions`, `rotations`, and `scales` must have equal non-empty lengths; each must contain `NumberOfFrames + 1` entries, with rotations as `[x, y, z, w]` quaternions. The response reports stored key and track counts.

Sparse `.pwanim` keys are separate: an unkeyed `at`, `rotate`, or `scale` channel fills from the bone's REFERENCE POSE, never identity. Zero-fill would teleport a keyed non-root bone to its parent's origin without raising an error. See `docs/pwanim-format.md` for the format rules.

Raw bone tracks are NOT UE curves: they use separate data-model storage and never appear in `list_curves`' `curves[]`. Verify them through sorted `boneTracks` entries `{boneName, keyCount}` (one per keyed bone; `keyCount` is that track's keyed-frame count). `list_curves` and `get_animation_info` surface this beside `rawTrackCount`, as do `describe_sequence` and the full `anim_sequence.json` dump shape; no generic property walk is needed.

## Workflow gotchas

Anim-blueprint authoring requires the target Skeleton before `create_anim_blueprint`. Blend-space axes are fixed at creation: `create_blend_space_1d` / `create_blend_space_2d` take axis min/max and grid-division bounds, so `add_blend_sample` works immediately on a fresh blend space.

## Which anim-BP creator?

`animation.authoring.create_anim_blueprint` is the authoring-path creator when
you need explicit save `path` and `parentClass`. It requires `skeletonPath` (it
has **no `meshPath`** auto-resolve) and rejects an in-session name collision with
`ASSET_EXISTS`.

For the meshPath-capable safe one-shot creator that derives the Skeleton from a
SkeletalMesh, use top-level [`animation.create_animation_bp`](animation.md); see
animation.md for the full two-creator breakdown.

## Naming and resolving AnimGraph nodes (the `nodeName` convention)

Every AnimGraph mutator with a `nodeName` param - including `bind_player_asset`,
`set_sync_group`, `set_anim_graph_node_value`, `set_anim_graph_pin_exposed` /
`set_anim_graph_pins_exposed`, and `set_layered_blend_layers` - routes through the
shared `ResolveAnimGraphNodeByTitle` resolver, with no separate rename verb. This
list is non-exhaustive; any future `nodeName` handler follows the same contract.
Read this before choosing a player-node name:

1. **`nodeName` matches the node's list-view title, not a stable id.** `ResolveAnimGraphNodeByTitle` (`Handlers/Animation/AnimGraphConstructionUtils.cpp`) first accepts one exact `GetNodeTitle(ENodeTitleType::ListView)` match, then one title containing `nodeName` as a substring.
2. **A player node's title is derived from its bound asset, and there is no rename verb.** A bound `UAnimGraphNode_SequencePlayer` is titled `Sequence Player '<AssetName>'`; an unbound one is exactly `Sequence Player`. No `animation.authoring` method sets an arbitrary title (`rename_node` / `set_node_title` do not exist), so a logical name such as `IdlePlayer` cannot work and two unbound players are indistinguishable.
3. **To make a node uniquely resolvable, bind a distinctly-named asset at creation, then resolve by a substring unique to that asset.** Pass `bindAsset` to `add_graph_node`, then resolve later `nodeName` values by a unique asset-name substring (e.g. `Dino_Idle`), never a logical name or shared `Sequence Player` prefix.
4. **Ambiguous `nodeName`s are rejected, not silently picked.** If multiple titles contain `nodeName` and none matches exactly, the resolver returns `AMBIGUOUS_NODE` and echoes candidate titles instead of mutating arbitrarily. Successful mutators echo the resolved title as `nodeName` so you can confirm the target.

**Animation setter validation is atomic.** `set_transition_settings` parses and resolves every supplied candidate before changing the transition. `set_layered_blend_layers` resolves the blend mode and mask, validates the complete layer array, and imports every supplied reflected option into scratch storage before changing the node. An invalid reflected value is rejected with `PROPERTY_SET_FAILED`; failed calls leave the node's authored fields unchanged.

## `name` is a BARE asset name, never a path

Applies to every creator here that composes its own package path: `create_animation_sequence`, `create_montage`, `create_composite`, `create_blend_space_1d`, `create_blend_space_2d`, `create_aim_offset`, `create_pose_library` and `create_ik_retargeter`. `name` is validated against the engine's own object-naming rules (`FName::IsValidXName` / `INVALID_OBJECTNAME_CHARACTERS`) and the composed `<path>/<name>` against `FPackageName::IsValidLongPackageName`, so a `/`, `\`, `.`, `..`, a leading or trailing slash, a space or an unmounted root is rejected `INVALID_ARGUMENT` with the engine's own reason text quoted. `path` is the only argument that chooses a folder. This is not pedantry about naming: `name` used to be concatenated onto `path` and handed straight to `CreatePackage`, which logs a double slash at **Fatal** — a verbosity that is not compiled out in any configuration — so the call did not fail, the editor **process** died, taking every unsaved package in that editor with it. The same defect was measured end-to-end on `foliage.add_type`; see that verb on the [`foliage`](foliage.md) page.

`..` is refused for a second reason worth knowing: it resolves to an *empty* package name, which is a separate `CreatePackage` Fatal from the double-slash one. A guard that only rejected slashes would let it through.

**`path` is checked too, and a trailing slash on it is still fine.** The composed `<path>/<name>` is what gets validated, so `path: "/Game//Animations"` is rejected `INVALID_ARGUMENT` even with a perfectly bare `name` — an interior `//` in the folder was the same process-killer. A *trailing* slash is not: `"/Game/Animations/"` and `"/Game/Animations"` are the same request, as they always were.

The check runs **before** `skeletonPath` is resolved, so a call carrying both a path-shaped `name` and an unresolvable skeleton is refused `INVALID_ARGUMENT`, not `SKELETON_NOT_FOUND`.

## See also

- [`animation`](animation.md) for the top-level convenience layer.
- [`asset`](asset.md) for `asset.dump` and dump sidecar schemas.
- [`controlrig`](controlrig.md) for CRIR - the text IR that replaces the removed `add_control` / `add_rig_unit` / `connect_rig_elements` stubs.
- [`blueprint.graph`](blueprint.graph.md) for general graph editing in the anim-blueprint event graph after `create_anim_blueprint`.
- [`pose_search`](pose_search.md) for Pose Search schema/database authoring used by Motion Matching graph nodes.
- [`sequencer`](sequencer.md) for driving animation assets from Level Sequence tracks.

### animation.authoring.create_montage

Create a `UAnimMontage` bound to a Skeleton with `name`, `skeletonPath`, optional `path`, and optional `slotName`. It already has one slot (default `DefaultSlot`) and an empty `Default` section - it is NOT empty, so do not re-add either to bootstrap it.

Montage verbs use **camelCase** params. The example uses the exact handler keys. `set_section_timing` has no `endTime`/`end_time` param: a section runs until the next section's start time, so set only `startTime` and chain sections to bound them.

Typical sequence after creation:

```
call("animation.authoring.add_montage_slot", { assetPath: "...", animationPath: "...", slotName: "DefaultSlot" })
call("animation.authoring.add_montage_section", { assetPath: "...", sectionName: "Default", startTime: 0.0 })
call("animation.authoring.set_section_timing", { assetPath: "...", sectionName: "Default", startTime: 0.0 })
call("animation.authoring.set_blend_in", { assetPath: "...", blendTime: 0.25 })
call("animation.authoring.set_blend_out", { assetPath: "...", blendTime: 0.25 })
```

Use `call("animation.authoring.link_sections")` to chain sections and `call("animation.authoring.add_montage_notify")` for embedded events.

### animation.authoring.create_composite

Create a `UAnimComposite` with `name`, `skeletonPath`, optional `path`, and optional `save`. It starts with an empty `AnimationTrack`.

Append clips with [`animation.authoring.add_composite_segment`](animation.authoring.add_composite_segment.md).

```
call("animation.authoring.create_composite", {
  "name": "AC_RunPreview",
  "path": "/Game/Animations",
  "skeletonPath": "/Game/Characters/Heroes/Mannequin/Meshes/SK_Mannequin_Skeleton.SK_Mannequin_Skeleton"
})
```

### animation.authoring.add_composite_segment

`add_composite_segment` accepts `assetPath`, `animationPath`, optional `startPos`, `animPlayRate`, `animStartTime`, `animEndTime`, `loopingCount`, and `save`. Without `startPos`, it appends at the current composite length. The animation can be any `UAnimSequenceBase` accepted by `FAnimTrack::IsValidToAdd`, but its skeleton must match the composite or the call returns `SKELETON_MISMATCH`.

`segmentIndex` is the index after sorting by requested start position and rewriting contiguous start times, not the append index. A non-tail insert can return `segmentIndex: 0` when the track already has segments.

```
call("animation.authoring.add_composite_segment", {
  "assetPath": "/Game/Animations/AC_RunPreview.AC_RunPreview",
  "animationPath": "/Game/Animations/AS_RunStart.AS_RunStart"
})
```

### animation.authoring.add_state_machine

Add a state machine to an existing anim blueprint (`create_anim_blueprint`). Use `add_state` and `add_transition` to build its topology, then `set_transition_rules` for coarse transition properties (crossfade, priority, sequence-player auto-rule toggle, bidirectional). It does NOT author the rule expression (the `bCanEnterTransition` body comparing a value such as `Speed`); see that method page for the raw-graph escape hatch.

The entry state is implicit: the first state added with `add_state` becomes the default. Transitions reference states by name, so plan names before wiring.

### animation.authoring.set_transition_rules

Update only a transition's coarse properties: `crossfadeDuration`, `priorityOrder`, `automaticRule` (the engine's `bAutomaticRuleBasedOnSequencePlayerInState` toggle), and `bidirectional`.

**Limitation - this does NOT author the rule expression.** `set_transition_rules` never writes the `bCanEnterTransition` body (the boolean expression such as `Speed > threshold`). `automaticRule` is the sequence-player auto-transition toggle, not a user condition. The call returns `success` after setting the four coarse fields while the rule body remains empty; it is not how to set the Speed rule.

There is currently **no RPC that authors the inline rule body.** The convenience helper (`set_transition_rule`, singular) was proposed and deferred (only 3 of its 5 internals RPCs shipped). AGIR (`anim.compile_agir`) also excludes it by design: its `rule` token carries only the bound-graph reference name, not the expression. Today, drive the transition's rule subgraph with raw `blueprint.graph.*` verbs (add a variable getter and comparator, then wire them to the rule result node).

Discoverability caveat: a fresh state machine's rule subgraph is not surfaced by `blueprint.graph.list_graphs` / `blueprint.inspect` (they expose only top-level graphs), and it has no `graphName` resolver. Guessed names such as `'Idle to Walk'` or `'Transition'` return `GRAPH_NOT_FOUND`; reach it through the transition node returned by `add_transition`.

### animation.authoring.set_anim_graph_node_value

Sets a static default on an anim-graph node identified by the graph path returned from `add_blend_node`, `add_slot_node`, `add_layered_blend_per_bone`, and similar calls. Runtime values come from anim-BP variables (`call("blueprint.set_default", ...)`). For bulk edits, prefer `call("anim.compile_agir")` with AGIR text.

### animation.authoring.add_graph_node

Naming note: `add_graph_node` has no title/name param; a player title comes from its bound asset (see **Naming and resolving AnimGraph nodes**). Pass `bindAsset` to give each player a distinct, resolvable title up front.

**Property writes.** Generic property dictionaries must cover both layers: editable `UAnimGraphNode_*` UObject properties and runtime `FAnimNode_*` fields reached through `UAnimGraphNode_Base::GetFNodeProperty()` / `GetFNode()`. Asset-player fields often live only in the runtime struct; `SequencePlayer` `PlayRate` is an `FAnimNode_SequencePlayer` field, so a UObject-only reflection write can report success without changing generated behavior.

### animation.authoring.set_anim_graph_pin_exposed

**Also covers the batch variant [`animation.authoring.set_anim_graph_pins_exposed`](animation.authoring.set_anim_graph_pins_exposed.md).**

Toggles "Expose as Pin" on an AnimGraph node's optional pin. `add_graph_node` also accepts `exposePins` so new nodes can be pre-exposed.

Use the engine API `UAnimGraphNode_Base::SetPinVisibility(bool bVisible, int32 OptionalPinIndex)` (`AnimGraphNode_Base.h:468`). Hand-rolling `Entry.bShowPin = X; Node->ReconstructNode()` skips `CacheShownPins` / `EvaluateOldShownPins`, which preserve bindings and connections through reconstruction. `ToggleOptionalPinExposed` in `Handlers/Animation/AnimGraphConstructionUtils.h` is the shared entry point for single and batch variants.

### animation.authoring.bind_player_asset

Binds a `UAnimationAsset` to an AnimGraph player node; `add_graph_node` uses the same asset-bind branch.

`nodeName` uses the shared list-view-title convention (see **Naming and resolving AnimGraph nodes**). There is no rename verb; bind a distinctly-named asset at `add_graph_node` time and resolve by a unique asset-name substring, not a logical name or shared `Sequence Player` prefix.

Dispatch uses `UAnimGraphNode_AssetPlayerBase::SetAnimationAsset(UAnimationAsset*)` (`AnimGraphNode_AssetPlayerBase.h:44`). Eight stock subclasses override it: `SequencePlayer`, `SequenceEvaluator`, `BlendSpacePlayer`, `BlendSpaceEvaluator`, `RotationOffsetBlendSpace`, `AimOffsetLookAt`, `PoseByName`, and `PoseHandler`; no per-class property-name table is needed.

**Critical gotcha:** each override internally `Cast<ExpectedClass>(Asset)`; a class mismatch silently no-ops while `SetAnimationAsset` appears successful. The handler MUST validate `Asset->IsA(Node->GetAnimationAssetClass())` BEFORE calling it, or a bad bind reports success while retaining the previous asset.

`LinkedAnimGraph`, `LinkedAnimLayer`, and `ControlRig` do NOT derive from `UAnimGraphNode_AssetPlayerBase`; they bind a `UClass` through a separate API and are out of scope here.

### animation.authoring.set_sync_group

Sets `GroupName`, `GroupRole`, and `GroupMethod` on a `UAnimGraphNode_AssetPlayerBase` descendant using the same list-view-title `nodeName` resolution as `bind_player_asset` (see **Naming and resolving AnimGraph nodes**), then the inner `FAnimNode_AssetPlayerBase` virtual setters. It must write and read back all three; partial support or a retained-value mismatch is `SYNC_GROUP_WRITE_FAILED`.

Valid roles are `CanBeLeader`, `AlwaysLeader`, `AlwaysFollower`, `TransitionLeader`, `TransitionFollower`, and synthetic `Standalone`. Non-standalone roles set `GroupMethod` to `SyncGroup`; `Standalone` clears the group, resets the role to `CanBeLeader`, and sets `GroupMethod` to `DoNotSync`. Empty `groupName` with another role is rejected. The response includes `syncMethod`, exposing a silent `DoNotSync` failure.

`add_graph_node` also accepts `properties.syncGroup` and `properties.syncRole` for asset-player nodes; those keys use this typed path before remaining `properties` entries use generic reflection.

### animation.authoring.set_notify_state_property

Use `set_notify_state_property` after `add_notify_state` when the notify-state class owns editable reflected properties. Select by `notifyIndex` or `notifyName`; `propertyPath` traverses nested instanced objects. In UE 5.6 Motion Warping, `UAnimNotifyState_MotionWarping` owns an instanced `RootMotionModifier`; configure its default Skew Warp modifier with paths such as `RootMotionModifier.WarpTargetName`, `RootMotionModifier.bWarpTranslation`, and `RootMotionModifier.RotationType`:

```
call("animation.authoring.set_notify_state_property", {
  "assetPath": "/Game/Animations/Attack_Montage.Attack_Montage",
  "notifyName": "WarpWindow",
  "propertyPath": "RootMotionModifier.WarpTargetName",
  "value": "AttackTarget"
})
```

### animation.authoring.add_two_bone_ik

Use this typed helper for skeletal-control nodes whose nested runtime fields are awkward through `add_graph_node` property dictionaries.

`add_two_bone_ik` creates `UAnimGraphNode_TwoBoneIK`, validates `ikBone`, `effectorBone`, and `jointTargetBone` against the target skeleton, writes `IKBone`, `EffectorTarget.BoneReference`, `JointTarget.BoneReference`, optional effector/joint `EBoneControlSpace`, and optional `alpha`, then reconstructs the node. Missing bones return `BONE_NOT_FOUND`.

```
call("animation.authoring.add_two_bone_ik", {
  "blueprintPath": "/Game/Animations/ABP_Character.ABP_Character",
  "ikBone": "foot_l",
  "effectorBone": "foot_l",
  "jointTargetBone": "calf_l",
  "effectorLocationSpace": "BCS_BoneSpace",
  "x": 300,
  "y": 120
})
```

### animation.authoring.add_modify_bone

Use this typed helper for skeletal-control nodes whose nested runtime fields are awkward through `add_graph_node` property dictionaries.

`add_modify_bone` creates `UAnimGraphNode_ModifyBone`, validates `boneName`, and writes optional `translation`, `rotation`, `scale`, per-axis spaces/modes, and `alpha`. Modes accept `Ignore`, `Replace`, `Additive`, and corresponding `BMM_*` names. A supplied transform with no mode defaults to `Replace`; an omitted transform defaults to `Ignore`.

```
call("animation.authoring.add_modify_bone", {
  "blueprintPath": "/Game/Animations/ABP_Character.ABP_Character",
  "boneName": "spine_01",
  "rotation": { "pitch": 0, "yaw": 20, "roll": 0 },
  "rotationSpace": "BCS_ParentBoneSpace",
  "rotationMode": "Additive",
  "x": 520,
  "y": 120
})
```

### animation.authoring.create_ik_rig

**Also covers [`animation.authoring.add_ik_chain`](animation.authoring.add_ik_chain.md), [`animation.authoring.create_ik_retargeter`](animation.authoring.create_ik_retargeter.md), and [`animation.authoring.set_retarget_chain_mapping`](animation.authoring.set_retarget_chain_mapping.md).**

The IK Rig / IK Retargeter family authors modern cross-skeleton assets (`UIKRigDefinition` + `UIKRetargeter`). It is the only surface for structurally dissimilar rigs; for simple skeleton-compatible clip retargeting use top-level `animation.setup_retargeting`.

**Module prerequisite.** This family uses the engine IKRig plugin (`IKRig` + `IKRigEditor`, shipped with UE 5.0+). Without those dependencies every verb returns `[NOT_SUPPORTED]`; custom builds must keep both modules in `PinWright.Build.cs`.

Typical flow:

```
call("animation.authoring.create_ik_rig", {
  "name": "IKR_Spider",
  "skeletalMeshPath": "/Game/.../SK_Spider.SK_Spider",
  "path": "/Game/Retargeting"
})

call("animation.authoring.add_ik_chain", {
  "assetPath": "/Game/Retargeting/IKR_Spider.IKR_Spider",
  "chainName": "LegFL",
  "startBone": "thigh_fl",
  "endBone": "foot_fl"
})

call("animation.authoring.create_ik_retargeter", {
  "name": "RTG_SpiderToDino",
  "sourceIKRigPath": "/Game/Retargeting/IKR_Spider",
  "targetIKRigPath": "/Game/Retargeting/IKR_DinoDragon",
  "path": "/Game/Retargeting"
})

call("animation.authoring.set_retarget_chain_mapping", {
  "assetPath": "/Game/Retargeting/RTG_SpiderToDino.RTG_SpiderToDino",
  "sourceChain": "LegFL",
  "targetChain": "LegFL"
})
```

**`add_ik_chain` requires `startBone` and `endBone`**: a retarget chain is a bone span mapped through `UIKRigController::AddRetargetChain`. Missing bones return `CHAIN_NOT_ADDED`. The chain survives `asset.dump`.

**`set_retarget_chain_mapping` is op-stack-scoped in UE 5.6+.** `UIKRetargeterController::SetSourceChain` applies only to ops whose mapping already exposes the target chain (derived from the target IK Rig). If none does, the call returns `CHAIN_MAP_NOT_APPLIED` rather than fake-succeeding; author the target rig's chains/op stack first. A fully op-stack-aware author-then-map flow (auto-adding the FK-chains op and populating maps) remains follow-up work.

### animation.authoring.add_ik_chain

`add_ik_chain` needs exact `startBone` / `endBone` names; bad names return `CHAIN_NOT_ADDED`.

**Where the `startBone`/`endBone` bone names come from:** `add_ik_chain`, `get_animation_info`, and `asset.get` do not provide a Skeleton / SkeletalMesh bone list. Call `call("skeleton.list_bones", {skeletalMeshPath})` (or `{skeletonPath}`) to enumerate every reference-skeleton bone (`name` / `index` / `parentIndex` / `parentName` / `location`) and use those names. A missing name returns `CHAIN_NOT_ADDED`; after that rejection, confirm it with [`skeleton.list_bones`](skeleton.md) instead of dumping the Skeleton and reading `skeleton.json`.
