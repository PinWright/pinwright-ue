# audio

Runtime audio playback and mix control for a live editor or PIE world: one-shot sounds, spawned or attached audio components, ambient actors, fades, priming, and the active SoundMix stack. Use this namespace for runtime-world audio operations; reach for `call("audio.authoring")` to author SoundCues, SoundClasses, SoundMixes, MetaSounds, attenuation, dialogue, reverb, source effects, and submix effects.

## Legacy creators removed

Earlier builds exposed `audio.create_sound_cue`, `audio.create_sound_class`, `audio.create_sound_mix`, `audio.create_dialogue_voice`, `audio.create_dialogue_wave`, `audio.create_reverb_effect`, `audio.create_source_effect_chain`, `audio.create_submix_effect`, `audio.add_source_effect`, and `audio.set_dialogue_context`. Those duplicated `audio.authoring.*` while missing fields (`parentClass` on SoundClass, `classAdjusters` on SoundMix). Use `call("audio.authoring.create_sound_cue")`, `call("audio.authoring.create_sound_class")`, etc. — the superset path.

## Cross-cluster overlap

`call("audio.authoring")` for assets; `call("niagara")` for visual FX asset inspection and one-operation edits.

### audio.spawn_sound_at_location

Spawns a live `UAudioComponent` parented to the level's `AWorldSettings` info actor, **not** to a placed actor you can find by label. Because `AWorldSettings` is not a placed level actor, the `actor.*` read verbs cannot resolve it — `actor.get_component_property` / `actor.get_components` against `WorldSettings_1` (by label, name, or full object path) return `[ACTOR_NOT_FOUND]`, even though `actor.find_by_class WorldSettings` / `actor.list` see it.

**Read it back via the `componentPath` in this verb's response, not the actor route.** The success response already carries the spawned component's full object path as `componentPath` (plus `componentName` / `componentClass`); feed that path straight to `call("system.inspect.inspect_object", { objectPath: "<componentPath>" })` or `call("property.get", { objectPath: "<componentPath>", propertyName: "..." })` to confirm location / sound binding / volume. Do not reconstruct the path from `componentName` + owner and do not guess `AudioComponent_N` indices — use the returned `componentPath` verbatim.

### audio.create_audio_component

Same WorldSettings ownership as `audio.spawn_sound_at_location`: the new `UAudioComponent` is parented to the level `AWorldSettings`, which the `actor.*` read verbs can't resolve (`[ACTOR_NOT_FOUND]`). The response returns the component's full object path as `componentPath` — read the component back with `call("system.inspect.inspect_object", { objectPath: "<componentPath>" })` or `call("property.get", { objectPath: "<componentPath>", ... })`, not through `actor.get_components` / `actor.get_component_property`.

### audio.create_ambient_sound

Spawns an `AAmbientSound` actor (resolvable by label via the `actor.*` verbs through its `actorPath` / `actorLabel`), but the audible `UAudioComponent` it drives is also returned as `componentPath` in the response. For a direct component readback (location / volume / sound binding) feed that `componentPath` to `call("system.inspect.inspect_object", ...)` / `call("property.get", ...)`.
