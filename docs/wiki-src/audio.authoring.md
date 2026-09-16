# audio.authoring

Authoring API for all UE sound assets: SoundCues, MetaSounds, SoundClasses & Mixes, attenuation/effects/dialogue, plus dump-parity describe readers for inspect-after-mutate workflows. Use this namespace for asset creation and mutation; reach for `call("audio")` for runtime playback against the assets created here.

## Inspect-after-mutate

Use `get_audio_info` only for compact compatibility output: a SoundClass returns `volume`/`pitch`/`parentClass`/`outputSubmix`, a SoundMix only `modifierCount`, and a DialogueVoice/DialogueWave only `type` (a wave also returns `contextCount`). For authored values, use `describe_sound_cue`, `describe_metasound`, and `describe_sound_wave` for dump-parity JSON, or `describe_attenuation`, `describe_sound_class`, `describe_sound_mix`, `describe_dialogue_voice`, and `describe_dialogue_wave` for hand-assembled readbacks whose keys mirror the authoring params. The method sections below give each reader's fields and traps.

## Workflow gotcha

SoundMix `classAdjusters` and SoundClass `parentClass` are accepted at `create_*` time as of the audio.* legacy drop — earlier flows had to follow up with `set_class_parent` / `add_mix_modifier`. Both still work, the create-time params just save a round-trip.

`create_source_effect_chain` and `add_source_effect` honor `save` through the shared measured asset-save contract. With `save:true` they force the package to disk and return `SAVE_FAILED` if no durable revision is established; the error data retains `saveRequested`, `saved`, `pendingFlush`, `saveState`, `saveDetail`, and asset verification. With `save:false` they leave the package dirty, report `saveRequested:false`, `saved:false`, `saveState:"notRequested"`, and expose `pendingSave:true` through asset verification without claiming persistence.

## Cross-cluster overlap

`call("audio")` handles runtime playback; `call("niagara")` handles VFX assets. `audio.synth` exports a SoundWave, while `audio.analysis` measures or decomposes any SoundWave; pass that asset path here for authoring.

## `name` is a bare asset name, `path` is the folder — on every create verb here

Every `create_*` verb in this namespace takes the destination as two arguments and they are not interchangeable. `name` is validated against the engine's own object-naming rules (`FName::IsValidXName` / `INVALID_OBJECTNAME_CHARACTERS`) and the composed `<path>/<name>` against `FPackageName::IsValidLongPackageName`, so a `/`, `\`, `.`, `..`, a leading or trailing slash or an unmounted root is refused `INVALID_ARGUMENT` with the engine's own reason text quoted. Choose the folder with `path`; each verb documents its own default.

This is not pedantry about naming. A `name` containing `//` reaches `CreatePackage`, which logs that at **Fatal** — a verbosity not compiled out in any configuration — so the call did not fail: the editor **process** died, taking every unsaved package in that editor with it. The same defect was measured end-to-end on `foliage.add_type`; see that verb on the [`foliage`](foliage.md) page. All fifteen asset-creating sites behind this namespace route through one shared guard, so the rule is identical on every `create_*` verb listed below — cues, classes, mixes, submixes, concurrency, MetaSounds and their patches and presets, attenuation, dialogue, and the reverb/source-effect assets.

`create_sound_concurrency` refuses a malformed `name` **before** it resolves `resolutionRule`, so a payload wrong in both ways reports the name first.

## See also

- [`asset`](asset.md) for the dump-parity policy shared by live describe calls and asset dump sidecars.
- [`audio.authoring.metasound_gotchas`](audio.authoring.metasound_gotchas.md) for UE 5.6 MetaSound engine-API traps when building graphs via this surface.

### audio.authoring.create_metasound

Create a new MetaSound source asset at the given path. The result is empty — no graph nodes, no inputs, no outputs. Build the graph via `add_metasound_input`, `add_metasound_node` (procedural / generator / DSP nodes), `add_metasound_output`, then wire pins with `connect_metasound_nodes`. Use `set_metasound_default` for input default values.

Typical sequence (all params are camelCase; copy each `className` from `search_metasound_nodes`, never guess a display name):

```
call("audio.authoring.create_metasound", { name: "MS_Engine", path: "/Game/Audio" })
call("audio.authoring.add_metasound_input", { assetPath: "...", inputName: "RPM", inputType: "Float" })
call("audio.authoring.search_metasound_nodes", { query: "Sine" })            // -> className "UE.Sine.Audio"
call("audio.authoring.add_metasound_node", { assetPath: "...", nodeClassName: "UE.Sine.Audio" })  // returns its nodeId
call("audio.authoring.add_metasound_output", { assetPath: "...", outputName: "Out", outputType: "Audio" })
call("audio.authoring.connect_metasound_nodes", { assetPath: "...", sourceNodeId: "<add_node nodeId>", sourceOutputName: "Audio", targetNodeId: "<output nodeId>", targetInputName: "Out" })
```

className convention: nodes are keyed `UE.<Name>.<Output>` (e.g. `UE.Sine.Audio`, `UE.Multiply.Audio by Float`, `UE.Add.Float`, `UE.Wave Player.Mono`) — the dotted form `search_metasound_nodes` reports. A bare display name like `WaveTableOscillator` (no namespace) does not resolve and returns `NODE_CLASS_NOT_FOUND`; always pass a `className` copied verbatim from `search_metasound_nodes`, or use the `nodeType` shorthands (`oscillator`, `gain`, `add`, `waveplayer`). `create_metasound` takes `name`+`path` (not `assetPath`); every other call here takes `assetPath`.

MetaSounds compile lazily — graph errors surface only when the asset is opened or referenced. Use `describe_metasound` after wiring to verify the graph snapshot.

### audio.authoring.create_metasound_preset

Create a preset of an existing MetaSound. The new asset mirrors the parent's inputs and outputs and inherits its input defaults; override individual defaults afterwards with `set_metasound_default`. The parent's class picks the produced class: a `UMetaSoundSource` parent yields a source preset, a `UMetaSoundPatch` parent a patch preset.

**Presetness is verified, not assumed.** On success the response carries `isPreset: true` and `presetParentAssetPath`, both read back off the created asset's document after creation — not echoed from the request. If the parent link fails to reach the document the verb returns `PRESET_NOT_APPLIED` instead of a success, because the underlying factory happily returns a valid but *empty* MetaSound in that case and a blank asset labelled a preset is worse than an error. `referencedAssetPath` is retained as the request echo; `presetParentAssetPath` is the verified value.

**Two path shapes in one response — do not compare them as strings.** `assetPath` is the **package** path (`/Game/Audio/MS_Preset`), because the shared verification block rewrites it to the handle you feed back into follow-up verbs. `presetParentAssetPath` and `referencedAssetPath` are **object** paths (`/Game/Audio/MS_Parent.MS_Parent`). Both forms load through the `audio.authoring.*` resolvers, so the difference only bites when something compares the two strings, or resolves `assetPath` with a raw `FindObject`-style lookup — which returns the outer `UPackage`, not the MetaSound.

**Known gap — a source preset does not inherit the parent's SoundWave settings.** The Content Browser's "Create Preset" duplicates the parent asset, so attenuation, modulation, sound class and quality settings carry over. This verb creates through `UMetaSoundSourceFactory` instead, which does not, so those properties come out at their class defaults. The graph, interface and input defaults are unaffected. Set the wave-level properties explicitly after creating if they matter.

### audio.authoring.describe_metasound

Return the live structured graph JSON for a MetaSound Source or Patch. The payload is the same shape as `asset.dump`'s `metasound.json`: `assetKind`, `assetPath`, `rootGraph`, `nodes`, `edges`, and `variables`.

A freshly created `UMetaSoundSource` already carries the standard source interface, so `rootGraph.interface.inputs`/`outputs` interleave auto-attached interface vertices (`UE.Source.OnPlay`, `UE.Source.OneShot.OnFinished`, `UE.OutputFormat.Mono.Audio`, ...) with your user-added I/O. Each interface entry carries `isInterfaceMember` (and, when true, the owning `interfaceName`) so a "how many outputs did I add" check filters `isInterfaceMember == false` rather than recognizing the built-in vertex names. See [`audio.authoring.metasound_gotchas`](audio.authoring.metasound_gotchas.md) for the list of auto-attached source vertices.

### audio.authoring.decompile_metasound

Return MSIR text for a MetaSound Source or Patch. MSIR is a flat node-and-wire text IR analogous to BPIR (`call("bpir")`), AGIR (`call("anim")`), MGIR (`call("material.mgir")`), and SCIR: one block per asset, with class refs as backtick-quoted `Namespace.Name v=Major.Minor` tokens, decompiler-assigned `nN` locals, and `$Name` sigils for graph I/O. Decompile-only in v1 — round-trip authoring (`compile_msir`) is a future follow-up.

The live RPC and the `asset.dump` `msir.txt` sidecar both call `FMSIRDecompiler::BuildMetaSoundIrText`, so prompt review can use the live response without drifting from dump output. The response includes `assetPath`, `ir`, `text`, and `warnings`; `ir` and `text` contain the same MSIR payload.

### audio.authoring.list_metasound_interfaces

Return the registered MetaSound frontend interface names from `Metasound::Frontend::ISearchEngine::FindAllInterfaces`. Use those names with `add_metasound_interface` and `remove_metasound_interface`; UObject class names such as `MetaSoundSource` or `MetaSoundPatch` are asset classes, not interface names.

### audio.authoring.add_metasound_interface

Attach a registered frontend interface to a MetaSound document through `IMetaSoundDocumentInterface`. This path is document-interface based so both sources and patches can be modified when the engine exposes the required document support.

### audio.authoring.remove_metasound_interface

Detach a registered frontend interface through the same document-interface path as add. With search-engine support available, the handler resolves and verifies the frontend interface name before mutating the document, then reports `INTERFACE_NOT_FOUND` when the registered interface is absent from the asset.

### audio.authoring.disconnect_metasound_nodes

Disconnect a specific edge by source node/output and target node/input. `FMetaSoundFrontendDocumentBuilder::RemoveNamedEdges` can return true for a valid mutation path even when the requested edge is absent, so success must be based on the `RemovedEdges` out parameter containing at least one removed edge.

### audio.authoring.connect_cue_nodes

Wires `targetNodeId` into `sourceNodeId`'s child slot `childIndex`. The slot is grown through the node's own `USoundNode::InsertChildNode`, which is what keeps the arrays a node type holds parallel to `ChildNodes` correct — `USoundNodeRandom::Weights` (and `HasBeenUsed`), `USoundNodeMixer` / `USoundNodeConcatenator::InputVolume`, `USoundNodeGroupControl::GroupSizes`. Those are not decoration: a Random node whose `Weights` sum to zero always returns child 0 from `ChooseNodeIndex` (its selection loop cannot break), and a Mixer whose `InputVolume` is shorter than its children reads out of bounds in `ParseNodes` at playback. Both used to happen to every cue built here and neither was visible in any readback.

`childIndex` must be below the source node type's max child count (1 for modulator/looping/attenuation/delay, `MAX_ALLOWED_CHILD_NODES` for random/mixer/concatenator) — a slot past it is `INVALID_CHILD_INDEX`, not a silently unreachable extra child.

`sourceNodeId: "Output"` (or `"Root"`) is reserved: it roots the cue at the target instead of parenting anything, the same write `set_cue_root` performs.

### audio.authoring.set_cue_root

Assigns `USoundCue::FirstNode` — the node the audio device starts playback from — and relinks the Sound Cue editor's Output pin to it. `add_cue_node` creates every node detached and `connect_cue_nodes` only wires nodes to each other, so a cue built the documented way is a correct, complete, **mute** tree until this verb roots it; `decompile_sound_cue` marks the state with `orphan` (versus `root`) and a `SoundCue has no FirstNode` warning that names this verb.

The relink is half the point. Writing `FirstNode` alone (via `property.set`) leaves the editor's Output node unconnected, and the next graph edit a human makes recompiles `FirstNode` back to null from the unlinked graph.

```
call("audio.authoring.create_sound_cue", { name: "SC_Impact_Concrete", path: "/Game/Audio/Cues" })
call("audio.authoring.add_cue_node",     { assetPath: "...", nodeType: "modulator", volume: 0.9, pitch: 1.0 })   // -> SoundNodeModulator_0
call("audio.authoring.add_cue_node",     { assetPath: "...", nodeType: "random" })                               // -> SoundNodeRandom_0
call("audio.authoring.add_cue_node",     { assetPath: "...", nodeType: "wave_player", wavePath: ".../SW_A" })     // -> SoundNodeWavePlayer_0
call("audio.authoring.add_cue_node",     { assetPath: "...", nodeType: "wave_player", wavePath: ".../SW_B" })     // -> SoundNodeWavePlayer_1
call("audio.authoring.connect_cue_nodes",{ assetPath: "...", sourceNodeId: "SoundNodeRandom_0",    targetNodeId: "SoundNodeWavePlayer_0", childIndex: 0 })
call("audio.authoring.connect_cue_nodes",{ assetPath: "...", sourceNodeId: "SoundNodeRandom_0",    targetNodeId: "SoundNodeWavePlayer_1", childIndex: 1 })
call("audio.authoring.connect_cue_nodes",{ assetPath: "...", sourceNodeId: "SoundNodeModulator_0", targetNodeId: "SoundNodeRandom_0",     childIndex: 0 })
call("audio.authoring.set_cue_root",     { assetPath: "...", nodeId: "SoundNodeModulator_0" })
```

### audio.authoring.describe_sound_cue

Return the live structured graph JSON for a SoundCue. The payload matches the SoundCue dump sidecar shape produced by `asset.dump`, so callers can verify graph edits without writing a dump folder.

### audio.authoring.describe_sound_wave

Return the live `sound_wave.json` payload for a `USoundWave`: format / sample rate / channels / duration / loop flag / compression settings. Delegates to the same `SoundWaveDumpBuilder` used by `asset.dump`, so the JSON shape matches the `sound_wave.json` sidecar exactly. Use this for raw-wave inspection after import or compression-setting edits, when writing a full dump folder is overkill.

### audio.authoring.describe_attenuation

Return the full live `FSoundAttenuationSettings` surface for a `USoundAttenuation` as structured JSON: `distanceAlgorithm`, `falloffDistance`, `innerRadius` (the value `configure_distance_attenuation`'s `innerRadius` writes — stored in `AttenuationShapeExtents.X`), `spatialize`, `spatializationAlgorithm`, the occlusion block (`enableOcclusion`, `occlusionLowPassFilterFrequency`, `occlusionVolumeAttenuation`, `occlusionInterpolationTime`), and the reverb-send block (`enableReverbSend`, `reverbWetLevelMin/Max`, `reverbDistanceMin/Max`). `get_audio_info` returns only `falloffDistance`+`spatialize` for a SoundAttenuation; use `describe_attenuation` to confirm what the four `configure_*` verbs wrote without falling back to `asset.dump`'s `properties.json`.

### audio.authoring.describe_sound_class

Return the full live `FSoundClassProperties` + hierarchy surface for a `USoundClass` as structured JSON: `volume`, `pitch`, `lowPassFilterFrequency`, `lfeBleed`, `voiceCenterChannelVolume` (the fields `set_class_properties` writes), `parentClass`, `outputSubmix`, and a `childClasses` array of the parent-side links `set_class_parent` / `create_sound_class` maintain. `get_audio_info` returns only `volume`/`pitch`/`parentClass`/`outputSubmix` for a SoundClass; use `describe_sound_class` to confirm a `voiceCenterChannelVolume` tune or the child-side hierarchy without falling back to `asset.dump`'s `properties.json`.

### audio.authoring.describe_sound_mix

Return the full live readback for a `USoundMix` as structured JSON: `modifierCount` plus an `adjusters` array — one object per `SoundClassEffects` entry with `soundClass`, `volumeAdjuster`, `pitchAdjuster`, `applyToChildren` (the values `add_mix_modifier` and `create_sound_mix`'s `classAdjusters` write) — plus the mix-level `applyEQ`, `eqPriority`, and an `eqSettings` object carrying the full 4-band `frequencyCenter0..3` / `gain0..3` / `bandwidth0..3` ladder `configure_mix_eq` writes (the same shape `configure_mix_eq` echoes in its own success response). `get_audio_info` returns only `modifierCount` for a SoundMix; use `describe_sound_mix` to confirm what each adjuster actually does, and to read back tuned EQ bands, without falling back to `property.get` on the mix's `SoundClassEffects` array. `FSoundClassAdjuster` has no per-adjuster fade member, so the `adjusters` objects carry no fade keys; mix-level fade times are not surfaced because no authoring verb writes them.

### audio.authoring.describe_dialogue_voice

Return the live readback for a `UDialogueVoice` as structured JSON: `gender` (one of `Neuter` / `Masculine` / `Feminine` / `Mixed`) and `plurality` (`Singular` / `Plural`) — the two fields `create_dialogue_voice` writes. `get_audio_info` recognizes neither DialogueVoice nor DialogueWave: both fall through to its final `else` and return `type:"Unknown"` with nothing else, so use `describe_dialogue_voice` to confirm a created voice's grammatical gender/plurality without falling back to `property.get { propertyName: "Gender" }` (the voice gender is omitted even from `asset.dump`'s `properties.json`).

### audio.authoring.describe_dialogue_wave

Return the live readback for a `UDialogueWave` as structured JSON: `spokenText` plus `contextCount` and a `contexts` array — one object per `ContextMappings` entry with `speaker` (the speaker DialogueVoice path), a `targets` array of target-voice paths, optional `soundWave`, and `localizationKeyFormat` — the wiring `set_dialogue_context` writes. `get_audio_info` returns `type:"Unknown"` for a DialogueWave and echoes none of this; use `describe_dialogue_wave` to confirm the spoken text and the speaker/target context wiring in-namespace without falling back to `asset.dump`'s `properties.json`. The reader emits each mapping's raw `targets` list (a null entry shows as an empty-string path), so a stray-null target is observable here.

### audio.authoring.create_sound_class

Create a new `USoundClass` at `asset_path`. Optional `parentClass` (path to an existing SoundClass) makes the new class a child — cumulative volume/pitch flow from parent down. Without `parentClass` the class is a root.

Use `set_class_properties` for non-parent fields (output target, voice center adjuster, etc.) and `set_class_parent` to assign or change the parent later.

### audio.authoring.create_sound_mix

Create a new `USoundMix` at `asset_path`. Optional `classAdjusters` is an array of per-class adjusters applied at activation time. Each entry takes `soundClass`, `volumeAdjuster`, `pitchAdjuster`, and an optional `applyToChildren` (default `true`, matching `add_mix_modifier`) controlling whether the adjuster also ducks the class's child classes:

```
call("audio.authoring.create_sound_mix", {
  asset_path: "/Game/Audio/Mix_Combat",
  classAdjusters: [
    { soundClass: "/Game/Audio/SC_Music",  volumeAdjuster: 0.4, pitchAdjuster: 1.0 },
    { soundClass: "/Game/Audio/SC_Ambient", volumeAdjuster: 0.6, pitchAdjuster: 1.0, applyToChildren: false }
  ]
})
```

Activate the mix at runtime with `call("audio.push_sound_mix", { asset_path: "..." })`. Append more adjusters post-create with `add_mix_modifier`. The seed path and `add_mix_modifier` agree on the `applyToChildren` default (`true`), so an adjuster reads back identically whichever path created it.

### audio.authoring.set_metasound_default

Write the default literal of a **graph input** — the vertex a caller of the MetaSound sets at runtime. For a pin *inside* the graph that has no graph input wired to it, use `set_metasound_node_input_default` instead.

Exactly one value param is required: `floatValue`, `intValue`, `boolValue`, `stringValue`, or `objectValue`. There is no default default — a call with no value is `MISSING_VALUE` and writes nothing. (It used to fall back to a float zero, which wrote `0.0` into whatever type the input actually was and reported success.)

`objectValue` takes the **asset path of the object to bind** and is the alias-free name to use; `assetPath` names the MetaSound being edited, never the object. The object is validated against the input's declared data type through the MetaSound data-type registry, so a `WaveAsset` input accepts a `USoundWave` and rejects anything else with `INVALID_ASSET_TYPE`, and a path that resolves to nothing is `OBJECT_NOT_FOUND`. Both rejections write nothing.

```
call("audio.authoring.add_metasound_input",  { assetPath: "/Game/Audio/MS_Music", inputName: "Stem_Drums", inputType: "WaveAsset" })
call("audio.authoring.set_metasound_default", { assetPath: "/Game/Audio/MS_Music", inputName: "Stem_Drums", objectValue: "/Game/Audio/Stems/SW_Drums" })
```

The response carries `storedDefault` — the literal read back off the document rather than an echo of the value you passed — plus `readBack`. A write the document does not confirm is reported as `SET_DEFAULT_FAILED`, not as a success.

### audio.authoring.set_metasound_node_input_default

Write the literal on a **node's input pin** inside the graph. This is the other half of the default surface: `set_metasound_default` writes a graph input's default, this writes a pin's, and only this one can bind a value to a pin that no graph input feeds.

The decisive case is a Wave Player: a per-stem player wants its wave fixed in the graph, not exposed as a runtime input.

```
call("audio.authoring.add_metasound_node", { assetPath: "/Game/Audio/MS_Music", nodeType: "waveplayer" })   // -> nodeId
call("audio.authoring.set_metasound_node_input_default", {
  assetPath: "/Game/Audio/MS_Music", nodeId: "<nodeId>", inputName: "Wave Asset", objectValue: "/Game/Audio/Stems/SW_Drums" })
call("audio.authoring.set_metasound_node_input_default", {
  assetPath: "/Game/Audio/MS_Music", nodeId: "<nodeId>", inputName: "Loop", boolValue: true })
```

Pin names are the registry's, spaces included — `"Wave Asset"`, `"Loop Start"`, `"Loop Duration"`, not the run-together forms. A wrong name returns `INPUT_NOT_FOUND` carrying `availableInputs`, the pin names that node actually has; a wrong `nodeId` returns `NODE_NOT_FOUND`, so the two mistakes are distinguishable without parsing a message. Value params, object validation and the `storedDefault` readback are identical to `set_metasound_default`. Verify from a second surface with `describe_metasound` and `nodeIds: ["<nodeId>"]`, which reports the same literal as `nodes[].inputs[].defaultLiteral`.

Sample-accurate looping lives here, not on the `USoundWave`: bare SoundWave loop metadata is ignored by the decoder and mixer, so a looping stem must drive the Wave Player's `Loop` / `Loop Start` / `Loop Duration` pins (`Loop Start` and `Loop Duration` are `Time`-typed — pass `floatValue` in seconds).
