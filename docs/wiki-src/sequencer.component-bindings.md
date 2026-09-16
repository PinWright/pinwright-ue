# sequencer.component-bindings

Binding a Sequencer track to one `UActorComponent` rather than to a whole actor. Shipped 2026-08-19 as an optional `componentName` on `sequencer.add_actor` / `sequencer.add_actors`; this page is why the parent link is load-bearing, and how to prove a bound component actually animates.

## Where the binding verbs mint bindings

Four sites mint an actor possessable. All route through `SequencerBindingUtils::BindActor` in `Source/PinWright/Private/Handlers/Sequencer/SequencerBindingUtils.h`, which performs the `AddPossessable(Label, Class)` + `BindPossessableObject(Guid, Actor, EditorWorld)` pair:

| Call site | Verb |
|---|---|
| `SequenceHandler.cpp` | `sequencer.add_actor` |
| `SequenceHandler.cpp` | `sequencer.add_actors` |
| `SequenceHandler.cpp` | `sequencer.add_camera` |
| `SequencerHandler.cpp` | camera rig rail / crane actor bind |

`sequencer.add_actor` and `sequencer.add_actors` additionally take `componentName`, which switches them to `SequencerBindingUtils::BindComponent` in the same header. The other two do not: `add_camera` binds a camera actor it has just spawned, and the rig verbs scaffold float tracks on named properties of the rig actor itself — a component name has no meaning in either.

The two remaining binding-creation sites — `sequencer.add_spawnable_from_class` and the rig verbs' spawnable branch — use `UMovieScene::AddSpawnable` from a class CDO. That object-template path is deliberately outside the helper.

## The engine route

`ULevelSequence::FindOrAddBinding(UObject*)` — `LevelSequence.cpp:865`, declared `LevelSequence.h:129`, and headless-viable, unlike `FSequencerUtilities::CreateBinding`, which needs a live `ISequencer`. Handed a `UActorComponent` it:

- resolves `GetParentObject` to the owning actor (`LevelSequence.cpp:759-764`);
- recursively binds that actor (`:888-889`), returning an invalid `FGuid` if the parent cannot be bound (`:891-895`);
- `MovieScene->AddPossessable(NewName, InObject->GetClass())` for the component itself (`:922`);
- `ChildPossessable->SetParent(ParentGuid, MovieScene)` (`:933`);
- binds with the **parent actor** as context rather than the world (`:925`, `:937`) — `ULevelSequence` sets `bParentContextsAreSignificant = true` in its constructor (`:100`), so this branch is live.

That last point is the trap: mirroring the actor path (`AddPossessable` + `BindPossessableObject(Guid, Component, EditorWorld)`) produces a binding that looks correct in readback but resolves to nothing at evaluation. `MovieSceneHelpers::GetResolutionContext` (`MovieSceneCommonHelpers.cpp:1272-1297`) substitutes the resolved parent as locator context **only** when `Possessable->GetParent().IsValid() && Sequence->AreParentContextsSignificant()` — the parent link is the mechanism, not metadata. Line numbers are UE 5.8, `Engine/Source/Runtime/`.

`FindOrAddBinding` is `protected` on `ULevelSequence` (inside `#if WITH_EDITOR`). It is reached through the **public** base declaration `UMovieSceneSequence::CreatePossessable` (`MovieSceneSequence.h:294`), which `ULevelSequence::CreatePossessable` (`LevelSequence.cpp:943`) forwards to verbatim: C++ checks member access against the *static* type of the call expression, so calling it through a `UMovieSceneSequence*` is a supported entry to the derived protected override, not a workaround.

## What the verbs do with it

- `sequencer.add_actor` / `sequencer.add_actors` — optional `componentName`, matched exactly (case-insensitively) against `UActorComponent::GetName()` on the resolved actor. No substring fallback: a near miss is `COMPONENT_NOT_FOUND` carrying `availableComponents`, never a binding to something else.
- The response row carries `bindingGuid` (the component's) **and** `parentBindingGuid` / `parentBindingName`, both read back off `FMovieScenePossessable::GetParent()` after the write, plus `resolvesToTarget` / `resolvedObjectPath` from an actual resolution.
- Two post-write checks gate success, and both are hard failures: the parent must be valid and present on the movie scene (`BINDING_PARENT_NOT_SET`), and the new GUID must resolve, through `MovieSceneHelpers::GetBoundObjects` over a transient shared playback state, to the component that was named (`BINDING_UNRESOLVED`). A binding that fails either is removed again — including a parent actor binding `FindOrAddBinding` minted on the way — so a failed call leaves no placeholder for a later verb to key tracks onto.
- The actor's own binding is created if absent and **reused** if present, so binding two components of one actor yields one actor binding with two children.
- `sequencer.add_transform_track` needed no change: it takes only `sequencePath` + `bindingGuid`.
- `sequencer.get_bindings` emits `kind` and `parentId` (`FMovieScenePossessable::GetParent()`, empty for a top-level binding) on every row. Without `parentId` a nested binding is indistinguishable from an actor binding in the output, so a caller cannot tell from a readback whether the component route ran at all.

## How to verify it works — a created binding proves nothing

A nested binding that fails to resolve at runtime produces **no** error anywhere. The track is present, the GUID is valid, `sequencer.get_bindings` lists it, and the component simply never moves. `sequencer.get_binding_transform` does not catch it either: it interrogates the authored track through the MovieScene pipeline, so it reports a moving transform whether or not the GUID resolves to any object.

The verb's own `resolvesToTarget` closes most of this gap — it resolves the GUID through the runtime path, at bind time, and refuses rather than reporting a binding that resolves elsewhere. It is not the whole claim, though: it proves the binding names the component, not that the authored animation reaches it. For that, run the acceptance test below once per new rig, not once per binding.

The acceptance test is that the **bound component's own transform in the level changes across the sequence's frame range**:

1. `sequencer.set_playhead` with an explicit `frame`, at several frames spanning the playback range (leave `forceUpdate` at its `true` default, or the level can still be presenting the previous frame).
2. `actor.get_components` on the owning actor at each frame — it reports per-component relative location / rotation / scale.
3. Assert the bound component's values differ between frames.

Identical values at every frame mean the binding resolved to nothing, whatever the binding call returned.

## The consumer this unblocks

A representative case: a level sequence carrying 86 bindings, of which 16 are wheel actors (`*_W0` / `*_W1`), one `MovieScene3DTransformTrack` each. Those wheels are separate actors attached to their parent vehicle purely because a component could not be bound. Folding them into `StaticMeshComponent`s is now possible — but do it behind the acceptance test above, not behind the binding call's success: the failure mode being avoided is wheels that stop spinning with nothing anywhere reporting it. Any rig that split one object into several actors only to get per-part animation is the same shape, and collapses the same way.
