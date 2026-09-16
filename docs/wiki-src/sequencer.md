# sequencer

Author and play back `ULevelSequence` assets — UE's track-based timeline for cinematics, gameplay-driven animations, and cutscenes (52 methods covering create/save, actor and spawnable binding, typed and generic track addition, keyframes, display rate / playback speed, editor-preview playback, deterministic playhead scrubbing, and actor-binding repointing). Use this namespace for the full level-sequence lifecycle; reach for `call("animation.authoring")` for raw `UAnimSequence` creation outside Sequencer and `call("mrq")` for offline render queueing.

## Namespace consolidation

This namespace consolidates what used to be split across `sequence.*` and `sequencer.*`; every method now lives at `sequencer.*`. It contains both typed-track conveniences and general track management, and `add_keyframe` has two registered call shapes; see its H3 below for disambiguation.

## Typed track wrappers vs. the general add_track

Use the typed wrappers (`add_animation_track`, `add_camera_track`, `add_transform_track`) when you know exactly what kind of track you want and the wrapper's params match. The wrappers handle binding lookup, track-class resolution, and section creation in one call. For other track classes (audio, particle, cinematic shot, custom) call `add_track` after first calling `list_track_types` to discover the registered class names.

## Live readers and dump parity

The live read APIs share the asset-dump MovieScene JSON helpers where they overlap, so tooling can compare `sequencer.*` output with `level_sequence.json` without bespoke adapters.

- `sequencer.list_tracks` enumerates root/master tracks (`MovieScene->GetTracks()`) and per-binding tracks, **plus** the camera-cut track — the camera-cut entry carries `isCameraCutTrack: true` and `isMasterTrack: true` (it lives on the dedicated `GetCameraCutTrack()` slot, not in `GetTracks()`). For the full dump-shaped camera-cut object (sections, channels) use `sequencer.get_camera_cut_track`; for just its sections use `sequencer.list_sections`.
- `sequencer.get_properties` includes `tickResolution`, `bindingCount`, `spawnableCount`, and `possessableCount` alongside the existing playback range and frame-rate fields.
- `sequencer.list_sections` emits sections from master tracks, binding tracks, and the camera-cut track. Its optional `trackName` filter takes the same identifier `list_tracks` / `add_track` report (matched as a substring against object name or display name). Each row carries track identity (`trackName`, `trackClass`, `bindingGuid`) plus section details (`range`, `blendType`, `rowIndex`, `channels`) and, when applicable, the referenced animation/audio/camera asset fields described in [`asset.dump` sidecars](asset.dump-sidecars.md). Each channel entry is `{type, keyCount}` by default; pass `includeKeys=true` to also get `keys: [{frame, value?}, ...]` per channel (per-key frame numbers always, plus the scalar `value` for double/float/bool/integer channels), so an `add_keyframe` write can be verified against the exact frames and values read back. Curve channels (double/float) additionally carry `interp`, `tangentMode`, and the two solved tangent numbers `arriveTangent` / `leaveTangent` — the last two matter because a cubic/auto key whose tangents were never computed reads back identically to a correct one on every other field while evaluating flat-in/flat-out (the subject stops dead at the key). The flag is off by default to keep the default payload compact. An unassigned section asset is represented by JSON `null` rather than an omitted key.
- `sequencer.get_camera_cut_track` returns a dump-shaped `cameraCutTrack` object when present, or JSON `null` when the sequence has no camera-cut track.

## FBX import and export outcomes

`sequencer.export_fbx` never writes through the caller's final path. It exports to a unique sibling staging file, verifies that the stage contains readable non-empty bytes, then publishes those bytes atomically. An existing destination is refused with `ALREADY_EXISTS` unless `overwrite:true` is explicit; a failed replacement returns `EXPORT_FAILED` and leaves the old file untouched. A successful response reports the measured `fileSize` and whether publication `replaced` an existing file.

`sequencer.import_fbx` does not trust the engine's boolean result because Unreal returns true even when no FBX node matches a binding. The handler snapshots track, section, key, and signed-object state around the import. If nothing changes it cancels the transaction and returns `NOTHING_IMPORTED` with `tracksBefore` / `tracksAfter`, `sectionsBefore` / `sectionsAfter`, and `keysBefore` / `keysAfter`; a success carries the same measured fields.

## UE 5.6 API notes for sequencer handlers

These are engine-side traps caught while extending the sequencer namespace. They affect anyone touching `Handlers/Sequencer/SequencerHandler.cpp` or any new sibling handler file.

**`AddMasterTrack` / `FindMasterTrack` were renamed.** UE 5.6 collapsed the "master track" naming: `UMovieScene::AddMasterTrack<T>()` / `FindMasterTrack<T>()` are gone. Use the no-arg overloads of `AddTrack<T>()` / `FindTrack<T>()` — the binding-attached overloads still take a `FGuid` parameter; the no-arg ones create or look up a root/master-equivalent track. Reference: `Engine/Source/Runtime/MovieScene/Private/MovieScene.cpp:1140-1278`.

**`AddTrack` already calls `Modify()` per add.** A trailing `MovieScene->Modify()` after one or more `AddTrack`s is redundant. The engine flips it inside the add itself, so skip the extra call (verify at `MovieScene.cpp:1140-1156` and `:1260-1278`). Same for the binding-attached overload. The handler still needs an enclosing `FScopedTransaction` and any owning `ULevelSequence::Modify()` before structural edits — only the `MovieScene` `Modify()` next to `AddTrack` is the dead call.

**Camera rig rail/crane tracks don't have dedicated classes.** Anyone porting a spec that prescribes `UMovieSceneCameraRigRailTrack` / `UMovieSceneCameraRigCraneTrack` should stop and grep: those classes don't exist in UE 5.6. Animate `ACameraRig_Rail` / `ACameraRig_Crane` via generic 3D transform tracks (rail current-position-on-spline is a float property track on `CurrentPositionOnRail`, crane yaw/pitch are property tracks on `CraneYaw` / `CranePitch`).

## Section authoring quirks

Section-level mutation has a few surfaces that are easy to mis-implement against the public API.

**`UMovieSceneAudioSection` has no public volume/pitch setters.** Both fields are private `FMovieSceneFloatChannel` members. Author them by writing channel defaults via the channel proxy:

```cpp
Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0)->SetDefault(VolumeValue);
Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(1)->SetDefault(PitchValue);
```

Channel order is fixed at `volume=0`, `pitch=1` (established in `Engine/Source/Runtime/MovieSceneTracks/Private/Sections/MovieSceneAudioSection.cpp:126-127`). Use `SetDefault` for a constant value; add keyframes via `AddKey` on the same channel for animation.

**`INDEFINITELY_LOOPING_DURATION` is the engine sentinel for unknown sound length.** When authoring an audio section and falling back to a `USoundBase`'s duration, `SoundBase->GetDuration()` returns `INDEFINITELY_LOOPING_DURATION` (`10000.0f`, defined at `Runtime/AudioMixerCore/Public/AudioDefines.h:16`) for looping or streaming sounds. Always guard `if (Duration != INDEFINITELY_LOOPING_DURATION) { … }` before using the value; fall back to a sensible default (engine code uses `1.0s` — see `Editor/MovieSceneTools/Private/TrackEditors/AudioTrackEditor.cpp:245`) when the sentinel fires.

**Use `UMovieSceneSection::GetSignature()` as a stable section key for sub-sequence handlers.** `GetSignature()` returns an `FGuid` that survives the editor session and lets a mutation handler look up the same section across multiple RPC calls. This is the right key for sub-section round-trip identity (better than indices, which shift when other sections are inserted/removed).

## Driving the editor viewport without PIE

A `SkeletalMeshActor` dropped in a level does **not** animate in the editor viewport on its own. The switch that makes it tick, `USkeletalMeshComponent::bUpdateAnimationInEditor`, is `transient` and `EditInstanceOnly` and defaults to `false` — so it cannot be saved into an asset and there is nothing to toggle up front. Sequencer sets it while a sequence that binds the mesh is being evaluated, which makes scrubbing a sequence the supported way to pose skeletal meshes in-editor. The same evaluation puts bound Niagara components into age-addressable mode rather than free-running wall-clock simulation.

`sequencer.set_playhead` is the deterministic entry point for that. It moves the playhead to an exact position and (by default) forces one immediate re-evaluation, so the level reflects the requested frame before the call returns.

**A scrub ticks the whole world, so it is gated on Niagara tick safety.** Moving the playhead makes the editor request a real-time frame, which promotes the next tick to a full tick-group pass over every Niagara simulation in the level — not just the ones the sequence binds. A system whose compiled data-interface count differs from its resolved one asserts inside the VectorVM on a worker thread when ticked, which is an `appError` that kills the editor process for every session attached to it. `set_playhead` (and the capture-subject providers that drive it) therefore sweep the open level first and refuse with `NIAGARA_DATA_INTERFACE_MISMATCH`, naming the system(s) and their live component counts, before the sequence is opened or evaluated. Clear it with `niagara.compile` on the named systems, or detach/deactivate their components; `niagara.audit_level` reports the same set with owning actors named.

**Frame bursts.** To capture N frames at a fixed interval, loop `set_playhead` → capture, stepping `frame` by a constant. Drive the loop in **frames**, not seconds: `frame` is exact and reproducible, while a seconds value accumulates rounding across a long burst. Playback is not involved — do not call `sequencer.play` first; a playing sequence advances on its own between your capture calls and the burst stops being reproducible.

## Binding a component instead of an actor

`sequencer.add_actor` and `sequencer.add_actors` take an optional `componentName`. With it the binding they create is the **component's** — a nested possessable parented to the actor's own binding — so a transform track keyed to the returned `bindingGuid` animates that one component and leaves the rest of the actor still. Reach for it whenever one object has parts that must move independently (a cart's wheels, a turret ring, a door leaf); reach for the plain actor binding whenever the object moves as one. Splitting a part into its own attached actor purely so Sequencer can animate it is no longer necessary. `sequencer.add_transform_track` and every other binding-GUID verb take a component binding unchanged.

**A component binding that fails to resolve reports nothing** — the track and GUID look correct and the component never moves — because the parent link is what makes the locator resolve against the actor instead of the world. So the verbs refuse rather than half-succeed: the parent is read back off the sequence and the new binding is resolved through the runtime path before either verb reports success, and a binding that fails either check is removed again (`BINDING_PARENT_NOT_SET`, `BINDING_UNRESOLVED`). A `componentName` that is not on the named actor is `COMPONENT_NOT_FOUND` carrying `availableComponents`, and binds nothing. `sequencer.get_bindings` emits `parentId` and `kind` on every row, so the hierarchy is visible in a readback rather than inferred from names. Call sites, engine route, and the end-to-end acceptance test: [`sequencer.component-bindings`](sequencer.component-bindings.md).

## Baking to and from a Control Rig track

The Control Rig loop has four authoring verbs (`add_controlrig_track`, `list_controls`, `key_controls`, `get_control_value`) and three bake verbs. `bake_to_controlrig` turns a binding's **evaluated** animation into an editable Control Rig track; `export_anim_sequence` turns a binding's **evaluated** performance into an AnimSequence asset; `bake_control_space` preserves one control's world motion while changing its parent space. The first two drive the engine's own headless-capable bake and need the binding to resolve to a **live** skeletal-mesh actor in the editor world. Space baking uses the engine's open-Sequencer-only API instead and refuses a closed editor explicitly.

**Both verbs report what landed, and refuse a zero.** A bake whose engine call returns true while writing nothing is the failure that survives every response-shape check, so neither verb trusts that bool: `bake_to_controlrig` counts the keys on the new section's float channels and the frames they span, `export_anim_sequence` walks the written asset's data model bone track by bone track, and either one fails `NO_KEYS_WRITTEN` — carrying the measured counts — rather than reporting a success full of zeroes. Read `keysWritten` / `boneKeysWritten` before believing a bake happened; a held pose legitimately collapses to one key per bone track in the sequencer data model, so a low per-track count is normal and a zero is not.

**`bake_to_controlrig` is destructive and `export_anim_sequence` is not.** The engine bake removes every existing Control Rig track on the binding and disables its skeletal animation track — `replacedControlRigTracks` reports how many tracks the call destroyed, measured before the call because afterwards there is nothing left to count. Export is read-only with respect to the sequence: nothing is linked back in, so an exported asset never turns into a new track you did not ask for. Export owns the destination asset instead — it creates it at `outAssetPath`, refuses `ASSET_ALREADY_EXISTS` unless `overwrite` is true, and saves unless `save:false`.

**Space-switch bake is explicit about its editor dependency.** `UControlRigSequencerEditorLibrary::BakeControlRigSpace` operates on the open Sequencer's **focused** sequence, so `sequencer.bake_control_space` returns `EDITOR_NOT_OPEN` when the requested root is not open and `SEQUENCE_NOT_OPEN` when that editor is focused inside a different subsequence. `allowFocusedSequenceMismatch:true` explicitly accepts the latter context. The verb creates the control's missing space channel itself, validates the typed target and parent switch, and measures the before/after key delta around the engine call. It never turns pre-existing keys or a silent no-op into success. It also refuses `RIG_STATE_INVALID` before mutation if the section no longer has a live rig/hierarchy or the target control has both opposed current transform caches dirty; that state cannot be read safely by the engine bake.

## See also

- [`sequencer.component-bindings`](sequencer.component-bindings.md) — the `FindOrAddBinding` route component bindings run on, what the parent link is load-bearing for, and how to prove a bound component actually animates.
- [`niagara`](niagara.md) for persistent Niagara VFX asset edits before sequencing or previewing effects.
- [`animation.authoring`](animation.authoring.md) for raw animation asset creation outside Sequencer.
- [`effect`](effect.md) for temporary runtime/editor preview helpers.
- [`mrq`](mrq.md) for queueing and rendering authored Level Sequences offline via Movie Render Queue.
- [`level-review`](level-review.md) — sampling motion rather than assuming it, using `sequencer.set_playhead` for deterministic scrubbing.

### sequencer.add_camera

Spawn an `ACameraActor`, bind it to the sequence as a possessable, and return the binding plus the spawned actor's path. Use this to seed a camera before cutting to it.

Workflow — "add a camera then cut to it":

1. Spawn + bind: `call("sequencer.add_camera", { path: "/Game/Cines/Intro.Intro" })`. The response carries `bindingGuid`, `actorLabel`, and — the field that links the two steps — `cameraActorPath` (the spawned camera's loadable object path) plus its unique `actorName`.
2. Cut to it: `call("sequencer.add_camera_track", { sequencePath: "...", cameraActorPath: "<the cameraActorPath from step 1>", startTime: 0, endTime: 5 })`.

Pass `cameraActorPath` straight through — `add_camera_track` resolves it with `LoadObject<ACameraActor>` and rejects a bare label with `CAMERA_LOAD_FAILED`. Do **not** pass the `actorLabel` (`"SequenceCamera"`): it is a fixed, non-unique constant (every `add_camera` uses it), so it is neither loadable nor sufficient to disambiguate multiple cameras. There is no need to drop into `actor.find_by_name` / `actor.list` to recover the path — the spawn handler hands it back directly. `add_camera` binds the spawned camera into the sequence, so this workflow satisfies `add_camera_track`'s binding requirement (see its H3): the cut section is bound to the exact camera you name, not an arbitrary one.

### sequencer.add_camera_track

Add a `UMovieSceneCameraCutTrack` (reusing the existing camera-cut track if the sequence already has one) with a section over `[startTime, endTime]` seconds, bound to the camera named by `cameraActorPath`.

The named camera **must already be bound in the sequence**. The section binding is resolved by object identity via `ULevelSequence::FindBindingFromObject(camera, editorWorld)` — the reverse of `BindPossessableObject` — so the cut points at exactly the camera you name, not the first camera-class binding it happens to find. Bind the camera first with `sequencer.add_camera` (spawns + binds in one call) or `sequencer.add_actors` (binds an existing scene camera). If the named camera has no binding, the handler returns `CAMERA_NOT_BOUND` and does **not** create a dangling unbound section. `CAMERA_LOAD_FAILED` means `cameraActorPath` did not resolve to an `ACameraActor` at all (e.g. a bare label was passed).

### sequencer.remove_actors

Remove actor bindings from a level sequence by name. Each `actorNames` entry is matched against the binding names `sequencer.get_bindings` reports, resolving internal object names through the same resolver `sequencer.add_actors` uses. Both **possessable and spawnable** bindings are removed (possessable via `RemovePossessable`, falling through to `RemoveSpawnable` when the matched GUID is a spawnable). The response is per-actor: `removedActors[i].success` is `true` only when a binding was actually removed, and `bindingsProcessed` counts the real removals — a name that matches no binding reports `success:false` with a diagnostic `error` rather than a silent success.

### sequencer.add_animation_track

Add a `UMovieSceneSkeletalAnimationTrack` to an existing object binding in the sequence and create an animation section that plays a given `UAnimSequence`. Use this for character animation playback inside cinematics.

Workflow:

1. Bind the actor: `call("sequencer.add_actor", { sequencePath: "/Game/Cines/Intro.Intro", actorName: "Hero" })`. Capture the returned `bindingGuid`.
2. Add the track + section: `call("sequencer.add_animation_track", { sequencePath: "...", bindingGuid: "...", animationPath: "/Game/Anims/A_Hero_Wave.A_Hero_Wave", startFrame: 0, durationFrames: 60 })`.

For non-skeletal animation (transforms, materials, visibility) use the corresponding typed wrapper or `add_track` with the right track class.

### sequencer.add_keyframe

**Two distinct call shapes share this method name** because the rename in plan chunk 1B collided two pre-existing handlers (the modern `SequencerHandler` version and the legacy `SequenceHandler` version, both originally registered at this path). Pick by parameter shape:

- **Seconds-based, float-only float-track writer** (from `SequencerHandler.cpp`): required `sequencePath`, `bindingGuid`, `propertyName`, `time` (seconds), `value` (number). Use this for writing values on an existing float property track when you know the binding GUID and property name and want to express time in seconds.
- **Frame-numbered, broader-track writer** (from `SequenceHandler.cpp`): optional `path` / `bindingId` / `actorName` / `property`, required `frame` (frame number), optional `value` (object / number / boolean). Use this for transform / vector / object-typed tracks, or when you only have an actor label and not a binding GUID, or when you prefer to express time in frames.

Which one wins at dispatch time depends on registration order — call the wiki against this method (omit `params`) for the live param schema. If you need deterministic behavior, prefer the seconds-based form's exact param set; missing required keys force the legacy form's path.

### sequencer.set_playhead

Move the Sequencer playhead to an exact position and evaluate the level there, in the editor, with no PIE. See `## Driving the editor viewport without PIE` on the namespace page for why this is the route for posing skeletal meshes and addressing Niagara ages.

**`frame` is authoritative.** Supply the position as `frame` (a display-rate frame number) or `time` (seconds); when both are present `frame` wins, because it is exact and reproducible frame-to-frame. `time` is converted to a display-rate frame time with its sub-frame remainder preserved, so a burst stepping by a non-integral interval stays on its grid instead of drifting. The response echoes the resolved position in every unit — `frame` (display rate), `time` (seconds), `tickFrame` (tick resolution) — plus `displayRate` and `tickResolution`, so a caller can verify what was actually applied rather than assuming.

The engine's editor playhead APIs address whichever sequence is **open in Sequencer**, never an arbitrary asset. `set_playhead` therefore opens the named sequence when it is not the open one; pass `open: false` to opt out, which rejects with `SEQUENCE_NOT_OPEN` instead of applying nothing and reporting success. `opened: true` in the response means this call had to open it.

`forceUpdate` (default `true`) forces one immediate full re-evaluation after the move. Leave it on for capture bursts: a scrub evaluates, but the editor can still present the previous frame until the next tick, which silently records an off-by-one frame. Pass `false` to skip the refresh when a plain scrub is already sufficient.

`updateMethod` selects how the position is applied — `scrub` (default, evaluates as a user scrub), `jump` (no intervening events), or `play` (fires events between the old and new position). An unrecognized value is rejected by name rather than falling back to the default. On UE 5.3 the engine exposes only the jumping `SetCurrentTime(int32)` setter, so the position is rounded to the nearest display frame and the response reports `updateMethod: "jump"` regardless of what was requested — check the echoed value, do not assume.

Accepts `sequencePath` as an alias for `path`, so the same key works whether you came from the track-authoring verbs or the transport verbs.

**Refuses with `NIAGARA_DATA_INTERFACE_MISMATCH` when the open level holds a Niagara system that is fatal on its next tick**, before anything is opened or evaluated — a scrub ticks the world, and ticking such a system kills the editor process. The message names each system, how many live components hold it, and the offending scripts with both counts. See `## Driving the editor viewport without PIE` on the namespace page.

### sequencer.add_track

The generic track-addition entry. Pair with `sequencer.list_track_types` to discover the registered `MovieSceneTrack` class names, then pass the class path to `add_track` along with the binding GUID. Most users do not need this — the typed wrappers (`add_animation_track`, `add_camera_track`, `add_transform_track`) cover the common cases. Reach for `add_track` when the typed wrappers do not include the track class you want (e.g. third-party plugin tracks, audio tracks, sub-sequence tracks).

**`trackName` in the request and `trackName` in the response are different things.** The response's `trackName` is the created track's object name (`MovieSceneAudioTrack_0`), read back off the track — it is the identifier `add_section`, `set_track_muted`, `set_track_solo`, `set_track_locked`, `remove_track` and the `list_sections` filter resolve by, and it is what `list_tracks` reports. The request's optional `trackName` is a *display name*: it is written to the track's `DisplayName` and echoed back as the response's separate `displayName` field. Track lookups accept either string (they match object name or display name, substring), but only the response's `trackName` is guaranteed unique.

A `trackName` that cannot be stored — the resolved class is not a `UMovieSceneNameableTrack` — is rejected with `TRACK_NAME_NOT_APPLIED` and **no track is added**; a `trackType` that resolves to no class is `CLASS_NOT_FOUND`. Neither leaves anything behind. (Before this, the request's `trackName` was accepted, never applied, and echoed back as the response's `trackName`, so the next call resolved nothing.)

### sequencer.add_actor

Bind one actor — or one named component of it — into the sequence as a possessable, and return the binding GUID a track verb needs.

Omit `componentName` and this is the actor binding it has always been: `{results: [{name, success, bindingGuid}]}`.

Pass `componentName` and the binding is the component's. `componentName` is matched **exactly** (case-insensitively) against `UActorComponent::GetName()` — the internal name the Details panel shows, unique within one actor, unlike the actor display label. There is no substring fallback: a name that is not on the actor is refused with `COMPONENT_NOT_FOUND`, whose payload lists every component the actor does have, and **nothing is bound** — not even the actor binding that would otherwise have been minted on the way.

The row a component binding returns carries the created binding *and its place in the hierarchy*, all read back off the sequence after the write:

| Field | Meaning |
|---|---|
| `bindingGuid` | the component's binding — the GUID to key tracks onto |
| `parentBindingGuid` | the actor's binding, read from `FMovieScenePossessable::GetParent()` |
| `parentBindingName` | that parent binding's stored name |
| `bindingKind` | `"component"` |
| `componentName` | the component name as resolved on the actor |
| `resolvesToTarget` | measured: the new binding resolved to the component that was named |
| `resolvedObjectPath` | the object it actually resolved to |

The actor's own binding is created if it does not exist yet and **reused** if it does, so binding two components of the same actor produces one actor binding with two children, not two actor bindings.

Both post-write checks are hard failures, because the failure they catch is invisible at runtime: a component possessable whose parent link is missing resolves against the world rather than against its actor, finds nothing, and every track on it silently drives nothing. A binding that fails either check is removed again and the call returns `BINDING_PARENT_NOT_SET` or `BINDING_UNRESOLVED` rather than a GUID that animates nothing.

### sequencer.add_actors

The batch form of `add_actor`. One `componentName` applies to **every** actor named in `actorNames`, which is the shape a repeated multi-part object wants: bind `Wheel0` across six cart actors in one call, then `Wheel1` in a second.

Failures are per item, not per batch: an actor missing that component gets `success: false` with `errorCode: "COMPONENT_NOT_FOUND"` and its own `availableComponents` list, while the rest of the batch still binds. Successful rows carry the same component fields `add_actor` returns.

### sequencer.get_bindings

List every object binding in the sequence with its place in the binding hierarchy.

Each row is `{id, name, kind, parentId}`. `kind` is `"possessable"` or `"spawnable"`. `parentId` is the GUID of the binding this one is nested under, read from `FMovieScenePossessable::GetParent()`, and is the empty string for a top-level binding — so a component binding is a possessable with a non-empty `parentId`, and an actor binding is one without.

Read `parentId` rather than guessing from `name`: a component binding's stored name is the component's object name, which says nothing about which actor owns it, and without the field a nested binding is indistinguishable from an actor binding in this output. An empty `parentId` on a binding you created with `componentName` means the hierarchy is not there and the track on it will animate nothing.

### sequencer.repoint_actor

Replace the object locator for one existing actor binding while preserving the binding identity and authored Sequencer data. This is a narrow repair operation, not a way to convert binding kinds or rebuild a sequence.

**Request:** The required fields are `path`, `bindingGuid`, `oldActorName`, and `newActorName`. `path` identifies the `ULevelSequence`; `bindingGuid` is authoritative and must be parsed before the sequence is looked up; the actor names use the same editor actor resolver as `sequencer.add_actor`. The names must be non-empty and different. The binding must be a top-level `FMovieScenePossessable` with an invalid parent and exactly one non-empty, non-custom locator.

**Success:** The response is `success: true` with `bindingGuid`, `bindingName`, `parentBindingGuid`, `locatorCount`, `authoredClass`, `newObjectClass`, `oldObjectPath`, `resolvedObjectPath`, `readbackMeasured`, `resolvesToNewActor`, `oldObjectUnbound`, `classMismatch`, and `warnings`. The original binding GUID, name, authored class metadata, and top-level hierarchy remain unchanged; `parentBindingGuid` is empty and `locatorCount` is `1`. The binding's track classes, object names, sections, ranges, channels, key frames, and key values remain unchanged. The old actor must resolve before the write, the new actor must resolve after it, and the old actor must no longer resolve through the binding.

**Class mismatch:** A different actor class is allowed when the binding is otherwise valid. `classMismatch` is `true`, `authoredClass` remains the original possessable class, `newObjectClass` reports the replacement actor class, and `warnings` contains the concrete warning `possessable authored class /Script/Engine.StaticMeshActor differs from new object class /Script/Engine.CameraActor; authored class metadata retained` for that example. An exact-class replacement reports `classMismatch: false` and `warnings: []`.

**Unsupported bindings:** A component child or any other parented possessable, a spawnable, a custom/non-possessable binding, and a binding with more than one locator are all refused with `UNSUPPORTED_OPERATION` before the engine replacement. These cases do not change the binding hierarchy, locator list, tracks, sections, or keys.

**Failure:** Failures return `success: false`, a top-level `errorCode` and `message`, and the complete stable `details` object: `bindingGuid`, `bindingName`, `parentBindingGuid`, `locatorCount`, `oldActorName`, `newActorName`, `authoredClass`, `newObjectClass`, `oldObjectPath`, `resolvedObjectPath`, `readbackMeasured`, `readbackPhase`, `resolvesToNewActor`, `oldObjectUnbound`, `classMismatch`, `warnings`, `rollbackAttempted`, `rollbackSucceeded`, `restoredResolutionStatus`, and `restoredResolvedObjectPath`. Errors are selected in this order: missing/empty fields, malformed GUID, or equal actor names (`INVALID_ARGUMENT`); unusable sequence after GUID parsing (`SEQUENCE_NOT_FOUND`); absent binding (`BINDING_NOT_FOUND`); unsupported binding shape (`UNSUPPORTED_OPERATION`); missing old actor, then missing new actor (`ACTOR_NOT_FOUND`); old-object ownership failure (`BINDING_UNRESOLVED`); invalid or different replacement GUID (`BINDING_FAILED`); and post-write readback failure (`BINDING_UNRESOLVED`). A valid parsed GUID remains in `details.bindingGuid` for later failures; it is empty only when the input GUID is malformed.

**Rollback:** After the old-object readback has passed, an engine-write failure or post-write verification failure restores the exact saved locator/reference at index zero and restores the authored name and class metadata. `rollbackAttempted` is true only when that restoration is attempted. The helper measures the restored binding independently: `rollbackSucceeded` is true only when `SourceActor` resolves again and `TargetActor` is absent, with `restoredResolutionStatus: "restored_old_actor"` and the old path in `restoredResolvedObjectPath`; otherwise the status is `"not_restored"`. A failed readback remains a failure even when rollback succeeds, and pre-write failures report `rollbackAttempted: false`, `rollbackSucceeded: false`, and `restoredResolutionStatus: "not_attempted"`.

**Example:** In a transient fixture, bind `SourceActor` and `TargetActor` as ordinary cube-bearing actors using `/Engine/BasicShapes/Cube.Cube`, then call:

```json
{
  "path": "/Engine/Transient/RepointSequence_<id>",
  "bindingGuid": "<bindingGuid>",
  "oldActorName": "SourceActor",
  "newActorName": "TargetActor"
}
```

### sequencer.measure_motion

Every other reader in this namespace reports **authored** numbers — `list_sections {includeKeys:true}` gives per-key `frame` / `value` / `interp` / `tangentMode` / `arriveTangent` / `leaveTangent`, `get_binding_transform` gives a composited pose at one frame. None of them reports a **derivative**, so a path can be correct in every published quantity and still read as jagged. This is the verb that answers *how fast*, *where does it accelerate*, *where does the path kink*, and *does the loop close in velocity*.

**Units: everything reported is per SECOND; everything you WRITE is per TICK.** Tangents are stored as curve value per tick (`FCubicBezierInterpolation` builds `P1 = P0 + LeaveTangent * DX / 3` with `DX` in tick-resolution frames), so the write verbs take per-tick slopes while this verb reports `uu/s`, `uu/s^2`, `uu/s^3`, `deg/s`, `deg/s^2` and a turn radius in `uu`. The response carries a `units` block and the `ticksPerSecond` the conversion used; to feed a measured speed back into `arriveTangent` / `leaveTangent`, divide by `ticksPerSecond`. At the 24000-tick default, forgetting that is a 24000x error.

**Derivatives are analytic, not differenced.** The evaluator reproduces the engine's own cubic Bezier segment construction from the keys and tangents already on the channel. Differencing a dense bake gives clean first derivatives and useless second ones — channel values are float32, so the noise floor is divided by `dt^2` and then by `dt^3`. Segment jerk is therefore **exact**: it is constant within a cubic segment rather than sampled.

**It is not a smoothness score, and that is deliberate.** A camera with uniformly continuous velocity and zero jerk is a floaty camera with no opinion; real camera work decelerates into a subject, accelerates out of a beat, holds, and cuts. `speed`, `acceleration`, `jerk` and `angular` are always status `reported` — read the distribution and ask what beat each outlier is. `curvature` becomes pass/fail only when you supply `minTurnRadius`. `loopSeam` is the one genuine pass/fail, because a seam is a hidden cut and any mismatch there announces the join.

**`loopSeam` fails on a STOPPED seam as well as a mismatched one.** The mismatch number alone proves nothing: a camera halted on both sides of the wrap has a mismatch of exactly zero, which is how a stopping camera ships while passing a seam check. `AutoSetTangents` forces the first key's leave tangent and the last key's arrive tangent flat, so an RPC-authored loop does exactly that unless both are written explicitly with `tangentMode: "user"`. The check therefore reports `arriveSpeed`, `leaveSpeed` and `stopsDead` beside `mismatch`, and fails on either condition. One-sided velocities are exact, so there is no sampling step and no convergence ratio to interpret. `positionGap` is reported and never gated — a move that deliberately ends elsewhere is not a defect.

**`curvature` is the check with no equivalent anywhere else.** Shrinking a key's tangent is the obvious way to author a deceleration, and `arriveTangent` / `leaveTangent` make it a one-liner — but shrinking a tangent at a key where the path **also changes direction** concentrates the whole direction change into a tight corner at that key. Every authored number still reads as intended; only the turn radius shows the damage. Compare `minTurnRadius` against the distance to the subject, and read the `tangentialAcceleration` / `normalAcceleration` split at that frame: the normal component is what a hairpin costs.

**What it cannot see, stated plainly.** It measures ONE transform section's own channels, not the composited pose — a parent attachment, an overlapping blended section or a camera-rig rail is invisible here (`source` says so in every response, and a multi-section track emits a warning). Weighted tangents (`RCTWM_Weighted*`) reparameterize a segment through a different basis, so every derivative check is reported `unmeasured` rather than handed numbers from the unweighted one. Outside the outermost key a channel holds constant, so the measured range is the authored key extent rather than the playback range.

`pass` is false whenever any check failed **or** could not be measured: "I could not look" and "I looked and it was fine" must never produce the same verdict. A check that looked and found nothing to report — a binding with no rotation keys, a path with no corner — stays `reported` and does not drag the verdict down.

### sequencer.bake_to_controlrig

Turns the binding's **evaluated** animation into a Control Rig track you can then key with `sequencer.key_controls`. Reach for it when an animation exists as a skeletal animation track (or as any composition of tracks) and you need to edit the pose rather than replace the clip; reach for `sequencer.add_controlrig_track` instead when you want an empty rig to author onto.

**It replaces, it does not layer.** The engine bake removes every Control Rig track already on the binding and disables the binding's skeletal animation track. `replacedControlRigTracks` is the count of tracks the call destroyed, measured before it ran — so a non-zero value on a binding you thought was clean is the warning that hand-authored control keys were just thrown away. There is no dry run; snapshot with `sequencer.list_controls` first if the existing track matters.

**Trust `keysWritten`, not success.** The verb reports `keysWritten`, `channelsWithKeys` / `channelCount`, `controlCount`, and `keyRange` (the display-frame span the keys actually cover, distinct from the `playbackRange` the bake was asked to sample). A bake that produced a track and no keys is refused with `NO_KEYS_WRITTEN` carrying those same numbers rather than reported as a success — the usual cause is a binding that resolves to nothing over the playback range.

`rigClass` defaults to the FK Control Rig, whose controls are one per bone and therefore always available for a skeletal mesh. A custom rig class must support a backwards-solve event or the engine bake cannot invert the pose onto its controls; that failure surfaces as `BAKE_FAILED`. `reduceKeys` runs the engine's older reduction pass over the baked curves, and `tolerance` is only reported back when it was actually used.

### sequencer.export_anim_sequence

Bakes the binding's **evaluated** performance — Control Rig track, skeletal animation track, attachments, blends, all of it composed — into an AnimSequence asset. This is the way out of Sequencer: the exported asset plays anywhere an AnimSequence plays, including inside another sequence.

**The sequence is not modified.** The engine's export can optionally link the new asset back into the sequence as a skeletal animation track; this verb never does, so exporting is safe to repeat and never leaves a track you did not ask for.

**The destination asset is this verb's responsibility, not the engine's.** `outAssetPath` is created when absent; an existing asset is refused with `ASSET_ALREADY_EXISTS` unless `overwrite:true`, and an existing non-AnimSequence at that path is refused with `INVALID_ASSET_TYPE` rather than clobbered. `save` defaults to true and the response carries the usual `saveRequested` / `saved` / `pendingFlush` / `saveState` verdict, so a `saved:false` is visible rather than assumed.

**`outAssetPath` is checked before anything else the verb does**, above the sequence and binding resolution, so a malformed destination answers `INVALID_PATH` naming the offending value rather than `SEQUENCE_NOT_FOUND`. A relative path is resolved under `/Game/`; a trailing slash is trimmed; a `//`, a `\`, a `..` or an unmounted root is refused with the engine's own `FPackageName::IsValidLongPackageName` reason quoted. The ordering is deliberate and load-bearing: this path becomes the argument to `CreatePackage`, which logs a double slash at **Fatal** — a verbosity not compiled out in any configuration, so it ends the editor **process** and every unsaved package in it instead of failing the call.

**`boneTrackCount` and `boneKeysWritten` are read back off the written asset's data model**, not derived from the range that was requested. `numberOfFrames`, `frameRate` and `playLengthSeconds` come from the same read. A held pose collapses to one key per bone track in UE's sequencer data model, so a per-track count of one is a correct export of a static pose, not a truncated one; an export that produced no tracks or no keys at all is refused with `NO_KEYS_WRITTEN`.

`startFrame` / `endFrame` are **display-rate** frames and must be supplied together — a half-specified range would quietly widen to the whole playback range and produce an asset that does not match the request. They map onto `UAnimSeqExportOption`'s custom time range, which arrived in UE 5.5; on 5.3 and 5.4 the exporter can only cover the sequence's whole playback range, so a range request there is refused as unsupported rather than silently widened. `requestedRange` echoes what was asked for and is named separately from the measured frame count.

### sequencer.bake_control_space

Bakes one existing Control Rig control into another parent space while preserving its evaluated world motion. This mutating editor operation is supported on UE 5.3 through 5.8: the requested Level Sequence must be the current open Sequencer root, the binding must already have a usable Control Rig track and section, and `control` must name a transform control on that rig. A closed or different root editor returns `EDITOR_NOT_OPEN` before the section changes. If that editor is focused inside a subsequence, the call returns `SEQUENCE_NOT_OPEN` unless `allowFocusedSequenceMismatch:true` explicitly opts into the engine's focused context; the default is false.

`targetSpaceName` and `targetSpaceType` form the engine's typed rig-element key. The accepted types are `Bone`, `Null`, `Control`, and `Reference`, plus `Socket` on UE 5.4 and newer; use `{targetSpaceName:"WorldSpace", targetSpaceType:"Reference"}` or `{targetSpaceName:"DefaultParent", targetSpaceType:"Reference"}` for the hierarchy's special spaces. An absent ordinary target is `TARGET_NOT_FOUND`, and a hierarchy-rejected parent switch is `INVALID_ARGUMENT` with the engine's reason.

`startFrame` and `endFrame` are required inclusive display-rate frames, with the end strictly after the start. `keyMode` is `allFrames` by default or `keysOnly`; `frameIncrement` (default `1`), `reduceKeys` (default `false`), and `tolerance` (default `0.001`) configure the all-frames bake. `keysOnly` refuses a non-default increment or reduction instead of accepting ignored settings.

The handler snapshots the sequence, MovieScene, Control Rig track, section, rig, and hierarchy before it creates a missing space channel and calls `BakeControlRigSpace`. Success requires a positive net-new key count plus both transform keys and a space key in the requested range. `attemptedTransformKeyDelta`, `attemptedSpaceKeyDelta`, and `keysWritten` are measured from before/after counts; absolute pre-existing totals cannot prove this request baked anything. An engine `false` is `BAKE_FAILED`, zero added keys is `NOTHING_BAKED`, and a positive delta without a complete transform/space result is `NO_KEYS_WRITTEN`. Every post-mutation failure applies and discards the transaction, rebuilds the section's channel proxy, and reevaluates the open Sequencer. `rollbackSucceeded` is true only when the target control's complete ordered transform-channel and space-channel key times and values match their pre-bake snapshot; only then is the package's original dirty state restored. A verification mismatch reports rollback failure and forces the package dirty so residual edits cannot be hidden as a clean failure. The restored `transformKeysAfter` / `spaceKeysAfter` counts and attempted counts are reported separately.

Immediately before that mutation boundary, the handler re-resolves the section's current rig, hierarchy, and named control. A missing live rig/hierarchy, or a control whose current local and global transform representations are both dirty, is `RIG_STATE_INVALID`. This validation reads only hierarchy dirty flags—not a transform—because `URigHierarchy::GetTransform` itself cannot recover from the both-dirty state.

Unattended automation covers registration and schema plus `EDITOR_NOT_OPEN` with exact no-mutation assertions on a valid transient transform-control section (`PinWright.Sequencer.ControlRigBake.SpaceNoEditorNoMutation`). It deliberately does not open an asset editor or call `BakeControlRigSpace`: a bare `UControlRig` whose hierarchy is extended only at runtime is not a stable engine space-bake fixture, because construction and evaluation can replace or reset that transient hierarchy. Physical space-key output and world-space-pose preservation therefore require an interactive editor test with a stable asset-backed Control Rig.
