# audio.authoring.metasound_gotchas

Non-obvious `FMetaSoundFrontendDocumentBuilder` / MetaSound engine-API behaviours, verified against engine source. Entries naming a 5.8 symbol were verified against UE 5.8; the rest against UE 5.6. Consult this page when debugging unexpected failures while building MetaSound graphs via the `audio.authoring.*` RPC surface.

## Engine-API behaviours

**`DefaultLiteral` is deprecated — use `InitDefault` before `add_metasound_input`.** `FMetasoundFrontendClassInput::DefaultLiteral` is deprecated in UE 5.5+ and writing to it does NOT populate the internal `Defaults` array. `Builder.AddGraphInput(ClassInput)` will then crash the editor because `FindConstDefaultChecked` hits `check(Literal)` at `MetasoundFrontendDocument.cpp:1297`. The handler calls `ClassInput.InitDefault(Literal)` before `AddGraphInput`; if you bypass the handler and touch the builder directly, follow the same order.

**Rename APIs preserve edges — prefer them over remove+re-add.** `FMetaSoundFrontendDocumentBuilder` exposes `SetGraphInputName(FName Old, FName New)` and `SetGraphOutputName(FName Old, FName New)` (both return `bool`). These preserve edges, `AccessType`, and the entire `Defaults` array. A remove+re-add pattern drops all incoming edges and resets defaults.

**`UMetaSoundFactory` creates a Patch, not a generic MetaSound.** Despite its name, `UMetaSoundFactory::FactoryCreateNew` always returns `UMetaSoundPatch` (its `SupportedClass = UMetaSoundPatch`). `UMetaSoundSourceFactory` is the source factory.

**Presets travel on the factory's `Template`, and `ReferencedMetaSoundObject` is a silent no-op (5.8).** `UMetaSoundBaseFactory::ReferencedMetaSoundObject` is `UPROPERTY(Transient, meta = (Deprecated = 5.8, DeprecationMessage = "Use document template instead"))` (`MetasoundFactory.h:23-24`) with **zero readers left in the MetaSound plugin**. `meta=(Deprecated=...)` emits no compiler warning, so assigning it builds clean and does nothing — the factory hands back an ordinary *empty* patch/source. The live path is `Template`: `FactoryCreateNew` forwards `Template` + `SelectedObjects` into `UMetaSoundEditorSubsystem::InitAsset` (`MetasoundFactory.cpp:41-45`, `:66-70`), which stores the template on the document and runs `FMetaSoundFrontendPresetTemplate::ConfigureDocument` → `FRebuildPresetRootGraph`. Canonical construction (the engine's own, `MetasoundEditorSubsystem.cpp:323-324`):

```cpp
TInstancedStruct<FMetaSoundFrontendDocumentTemplate> PresetTemplate =
    TInstancedStruct<FMetaSoundFrontendDocumentTemplate>::Make<FMetaSoundFrontendPresetTemplate>();
PresetTemplate.GetMutable<FMetaSoundFrontendPresetTemplate>().Parent = ParentMetaSound;  // UObject*
Factory->Template = MoveTemp(PresetTemplate);
Factory->SelectedObjects.Add(ParentMetaSound);
```

`PinWright::MetaSound::MakeMetaSoundPresetTemplate` (`Handlers/Audio/MetaSound/MetaSoundPathUtils.h`) is the shared version of that, and `FindMetaSoundPresetParent` is its inverse.

**`RootGraph.PresetOptions.bIsPreset` is always `false` on 5.8 — do not test presetness with it.** The field is `meta=(DeprecatedProperty)` (`MetasoundFrontendDocument.h:1901`) and 5.8's document versioning reads it once to migrate presetness into `Document.Template`, then *clears* it; nothing sets it again. Reading it compiles clean and answers "not a preset" for every preset. Test `Document.Template.GetPtr<FMetaSoundFrontendPresetTemplate>() != nullptr` (what `FMetaSoundFrontendDocumentBuilder::IsPreset()` does, `MetasoundFrontendDocumentBuilder.cpp:4423-4428`) — in this plugin, `PinWright::MetaSound::IsMetaSoundDocumentPreset`.

**Interface operations use frontend interface names.** List/add/remove work from the MetaSound frontend search engine and document interface, not UObject class names. The relevant capability gates are independent: document editing requires `MCP_HAS_METASOUND_DOCUMENT_INTERFACE`, frontend builder types require `MCP_HAS_METASOUND_FRONTEND`, and interface registry/list support additionally requires `MCP_HAS_METASOUND_SEARCH_ENGINE`.

**MSIR is decompile-only; authoring still goes through the imperative builder.** MetaSound now has a flat text IR (MSIR) exposed by `decompile_metasound` and the `msir.txt` asset-dump sidecar — both surfaces share one builder (`FMSIRDecompiler::BuildMetaSoundIrText`) so the live RPC and dump output cannot drift. There is no `compile_msir` round-trip yet, so every mutation must still go through the imperative `FMetaSoundFrontendDocumentBuilder` API. Destructive RPCs (`remove_metasound_node`, `disconnect_metasound_nodes`, `remove_metasound_input`, `remove_metasound_output`) remain non-negotiable until the authoring follow-up lands.

**A fresh `UMetaSoundSource` ships with the standard source interface already attached.** A newly created Source carries `UE.Source` (input `UE.Source.OnPlay`, output `UE.Source.OneShot.OnFinished`) plus an output-format interface (e.g. `UE.OutputFormat.Mono`, output `UE.OutputFormat.Mono.Audio`) before you author anything, so `describe_metasound` / the `metasound.json` sidecar list those vertices in `rootGraph.interface` interleaved with your own I/O. To answer "exactly one user output named X" filter the entries by `isInterfaceMember == false` (each interface vertex carries that boolean plus the owning `interfaceName`) — do not rely on subtracting the literal built-in names by hand.

**`FinishBuilding()` must be called before save.** The V2 (UE 5.4+) `FMetaSoundFrontendDocumentBuilder` constructor requires `FinishBuilding()` to commit internal caches and dirty flags. The `save` on these verbs is a real disk write, so a save taken ahead of `FinishBuilding()` serializes the pre-edit document and the edit is silently absent from the `.uasset`. Order on the success path is `FinishBuilding()` → `PinWright::MetaSound::SaveMetaSoundAndReport` → `Ctx.SendSuccess`; calling `FinishBuilding()` after `SendSuccess` means side-effects fire after the RPC has already completed.

**`save:true` writes the `.uasset`, and the response says whether it landed.** Every MetaSound verb that takes `save` routes it through one helper (`PinWright::MetaSound::SaveMetaSoundAndReport`) onto the plugin-wide real-save path, and answers the same `saveRequested` / `saved` / `pendingFlush` (+ `saveState`, `saveDetail`) report every other create verb answers. `saved:true` means the file is on disk right now; `pendingFlush:true` means a save was asked for and did not become durable, and `saveState` says whether flushing is the remedy. It used to only mark the package dirty, so an entire graph could be authored, report success on all 44 calls, and be absent from disk after an editor restart. Pass `save:false` to batch a long authoring run and finish with one `asset.save {force:true}`.

**An array-typed input or pin takes `arrayValue`, and nothing else.** `Float:Array`, `Int32:Array`, `Bool:Array`, `String:Array` and object arrays such as `WaveAsset:Array` are populated by passing `arrayValue` as a JSON array — numbers, booleans, strings, or asset paths matching the element type. The scalar params cannot carry a list, and the registry rejects `objectValue` for an array type outright (an array entry sets only `bIsProxyArrayParsable`), so `arrayValue` is the only way in; both `set_metasound_default` and `set_metasound_node_input_default` accept it, as do `add_metasound_variable` / `set_metasound_variable_default`. Entry kind is decided by the declared data type, not by the JSON, so a `Float:Array` refuses `"0.25"` and an `Int32:Array` refuses `1.7` rather than coercing them; an empty array is a legal value that clears the default. This is what makes the `Array.*` family reachable — `Array.Random Get`'s `In Array` pin and its `Float:Array` `Weights` pin included.

**A graph input's default and a node pin's literal are different state.** `set_metasound_default` writes the default of a *graph input* (the vertex a caller sets at runtime); `set_metasound_node_input_default` writes the literal on a *node's input pin* inside the graph. A pin with no graph input wired to it can only be given a value by the second verb — which is why a fixed per-stem Wave Player needs it rather than a `WaveAsset` graph input plus an edge.

**`objectValue` is the object; `assetPath` is always the MetaSound.** Every mutator in this namespace takes `assetPath` for the document being edited, so the object-valued default parameter had to be named something else. It is `objectValue` (alias `objectPath`). Passing the wave path as `assetPath` edits the wrong asset, or fails to load one at all.

**Data-type and pin names come from the registry, not from display text.** The registry keys are `Float`, `Int32`, `Bool`, `String`, `Audio`, `Trigger`, `Time`, `WaveAsset`, `AudioBusAsset`, `WaveTable`, `Enum:*` (`MetasoundPrimitives.cpp`, `MetasoundEngineModule.cpp`). The authoring verbs additionally accept `Int` and `Boolean` as convenience aliases and canonicalize them; everything else must be the registered spelling. Node pin names keep their spaces exactly as declared — the Wave Player's is `"Wave Asset"`, not `WaveAsset`. An unknown type name is rejected with `INVALID_TYPE` carrying registry-derived `suggestions`; an unknown pin name is rejected with `INPUT_NOT_FOUND` carrying `availableInputs`.

**A `USoundWave`'s own loop metadata does not loop anything.** The decoder and mixer ignore it. Sample-accurate looping is a MetaSound Wave Player feature: drive its `Loop` (Bool), `Loop Start` and `Loop Duration` (both `Time`, i.e. seconds as `floatValue`) pins. A stem rendered offline to a SoundWave therefore loops only once it is inside a Wave Player with those pins set.

## See also

- [`audio.authoring`](audio.authoring.md) — the MetaSound and audio-asset authoring verbs these gotchas apply to.
- [`audio`](audio.md) — playback, mixing, and the rest of the audio surface.
