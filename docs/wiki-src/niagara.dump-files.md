# niagara.dump-files

Read-side reference for the Niagara sidecars written by `asset.dump`, plus the asset-registry tags and op-info structures consumed by `niagara.search_modules` / `niagara.list_node_types`. Use it to interpret cached dumps, choose a sidecar, or build no-load registry filters.

## Dump files written by `asset.dump` for Niagara assets

- `nir.txt` — durable authored-state Niagara text IR for systems, emitters, and scripts. It is built by the same `NIRDecompiler::BuildNiagaraIrText(UObject*)` path as `niagara.decompile_nir` and carries system/emitter flags, stacks, renderers, event handlers, simulation stages, and GPU script graphs. **Renderer and simStage property listings are filtered**: a line ending ` @default` is at its class default, and an absent line means the property holds its type's zero value (`false` / `0` / empty / null). Read the zero-default rule on [niagara.nir](niagara.nir.md) before parsing a renderer block in bulk — never infer a bool from its absence. It omits live compile readiness and emitter-handle validity/recompile flags. It does not statically flag GPU-incompatible modules (GPU/CPU is per-script-usage, not per-emitter — see [niagara.nir](niagara.nir.md)); use `niagara.inspect` or `niagara.validate` when current compiler diagnostics matter.
- `niagara_parameters.json` — user/system/emitter/particle parameters, types, namespaces, defaults, raw values, and bindings where accessible. A rapid-iteration entry whose module input also carries a graph override pin is marked `overridden` with an `override` object naming that pin; the document's `rapidIterationNote` states what this store can and cannot see.
- `niagara_stack.json` — system and emitter stack entries in execution order, module/function-call ids, enabled state, script assets, selected versions, and input override summary. Each entry names its stage in `scriptUsage` (`EmitterSpawnScript` / `EmitterUpdateScript` / `ParticleSpawnScript` / `ParticleUpdateScript` / `SystemSpawnScript` / `SystemUpdateScript`, or empty for a module on no parameter-map chain) and its `index` is the position within that stage. A module is listed once per owner, not once per script — the same shape `niagara.inspect {includeStack:true}` returns.
- `niagara_graphs.json` — graphs by system/emitter/script usage with nodes, pins, defaults, links, GUIDs, and function-call script refs.
- `niagara_compile.json` — authored asset/script identities plus stable issues derived from authored structure. It omits live validity, readiness, outstanding-compilation, recompile, and compile-status fields.

Standalone `UNiagaraScript` assets (modules, functions, and dynamic inputs) write `nir.txt`, graph, and authored-only compile sidecars. Live `niagara.inspect`, `niagara.graph.get`, and `niagara.validate` are separate diagnostic surfaces: they may report current editor compile state absent from the mirror, and do not synthesize system/emitter properties or stack data for scripts.

`asset.dump` no longer writes `niagara_model.json`, `niagara_system.json`, or `niagara_emitters.json`. The compact model/system/emitter JSON builders remain live read surfaces through Niagara RPCs; treat them as RPC payloads, not current dump sidecar files.

Compact model scalability is duplicated intentionally for easier callers: system `emitters[]` records and standalone emitter model roots expose direct `scalability` objects, while the older nested `versionedEmitter.scalability` location remains present for compatibility. Prefer the direct field in new consumers and treat the nested field as a compatibility mirror.

Parameter-store values are not always byte-for-byte reflected struct memory. Niagara can keep LWC and custom struct parameters in simulation/SWC storage, so dump paths that need `UScriptStruct` fields must copy through `FNiagaraParameterStore::CopyParameterData` before reflecting properties. Reading `GetParameterData` directly is only safe for the explicit primitive/vector cases handled by the dumper.

Struct-backed Niagara booleans should still serialize as JSON booleans. The dumper and compact parameter model match bool-like Niagara types with `FNiagaraTypeDefinition::IsSameBaseDefinition(FNiagaraTypeDefinition::GetBoolDef())`, not strict type equality, so `FNiagaraBool` and store-normalized bool representations follow the same path.

`niagara.set_parameter` follows the inverse path for reflected script structs: it allocates initialized source struct memory, applies JSON fields with the shared property conversion helpers, then calls `FNiagaraParameterStore::SetParameterData`. That lets Niagara run its converter back into simulation storage instead of treating caller JSON as the store's internal byte layout. Registered Niagara script structs can be addressed by registered type name, struct name, or registered struct path.

`/Script/CoreUObject.Vector` is intentionally treated as the existing safe vec3 alias for parameter validation and mutation. Do not route it through the generic reflected-struct fallback; the safe path expects `{x,y,z}` or `[x,y,z]` and writes the Niagara vec3 representation.

Structured reference records keep the compact model usable when a Niagara feature is not fully lowered. They identify the feature, explain the reason, and point back to raw data without emitting only an opaque unsupported marker:

Emitter models include `advancedFeatures` with simulation-stage records, event-handler records, and GPU script references. System models expose the same data on each emitter handle entry, plus renderer summaries for the resolved emitter snapshot.

```json
{
  "kind": "reference",
  "reason": "dynamic_input_not_semantically_lowered",
  "displayName": "Curl Noise Force",
  "class": "/Script/NiagaraEditor.NiagaraNodeFunctionCall",
  "objectPath": "/Game/FX/NS_Sparks.NS_Sparks:ParticleUpdate.CurlNoise",
  "owner": { "kind": "emitter", "name": "Sparks" },
  "refs": {
    "script": "/Niagara/Modules/Forces/CurlNoiseForce.CurlNoiseForce",
    "graph": "ParticleUpdate",
    "nodeGuid": "7D6A8C4A4B4F4B12A22A21F4F201C421"
  },
  "properties": {},
  "provenance": { "rawGraphFile": "niagara_graphs.json" }
}
```

Cascade `UParticleSystem` assets are dump-only and write `cascade.json`. There are no Cascade edit RPCs.

## UNiagaraScript asset-registry tags (no-load module search)

`UNiagaraScript::Usage` and `ModuleUsageBitmask` are both readable from `FAssetData` without loading the asset:

- `Usage`: emitted by default reflection-based asset tags. Read via `AssetData.GetTagValueRef<FString>(GET_MEMBER_NAME_CHECKED(UNiagaraScript, Usage))`; value is the enum name string (e.g. `"Module"`).
- `ModuleUsageBitmask`: an explicit custom tag emitted in `UNiagaraScript::GetAssetRegistryTags` (NiagaraScript.cpp:3530), stored as integer-stringified `FString`.

Other no-load tags: `Description`, `Category`, `Keywords`, `LibraryVisibility`, `bDeprecated`, `bSuggested`, `ProvidedDependencies`. This means `niagara.search_modules` can filter the entire asset registry on `Usage` + stage-bitmask without any `LoadObject` calls.

Decode the bitmask: `UNiagaraScript::GetSupportedUsageContextsForBitmask(int32)` → `TArray<ENiagaraScriptUsage>`; check compatibility with `UNiagaraScript::IsSupportedUsageContextForBitmask(int32, ENiagaraScriptUsage)`. The input/output signature is **not** a registry tag, so loading is required for that field — defer to a lazy/opt-in path.

## FNiagaraOpInfo structure (niagara.list_node_types / niagara.search_ops)

`FNiagaraOpInfo` (NiagaraEditorCommon.h:21-93) has **no** `gpuOnly` field — GPU-only filtering happens in the HLSL translator, not at the op-info level. Do not include `gpuOnly` in any RPC response shape.

Fields available: `Name (FName)`, `AlternateSearchName (TOptional<FName>)`, `Category (FText)`, `FriendlyName (FText)`, `Description (FText)`, `Keywords (FText)`, `Inputs/Outputs (TArray<FNiagaraOpInOutInfo>)`, `bSupportsAddedInputs`, `bNumericsCanBeIntegers`, `bNumericsCanBeFloats`.

`FNiagaraOpInOutInfo` (NiagaraCommon.h:323) carries `Name`, `DataType (FNiagaraTypeDefinition)`, `FriendlyName`, `Description`, `Default`, `HlslSnippet` — enough to build `"Add(float, float) -> float"` signature strings.

The op registry is accessed via `static const TArray<FNiagaraOpInfo>& FNiagaraOpInfo::GetOpInfoArray()` (line 77), which lazy-initializes via `FNiagaraOpInfo::Init()` at NiagaraEditor module startup. Do not call `Init()` directly; the array is populated by the time any editor RPC runs.

## See also

- [`asset`](asset.md) for the folder-dump lifecycle, persistent mirror contract, and job progress fields.
- [`niagara`](niagara.md) for the read-first Niagara inspection and mutation workflow.
- [`niagara.nir`](niagara.nir.md) for NIR grammar and authored-state stability rules.
- [`asset.dump-sidecars`](asset.dump-sidecars.md) for cross-asset sidecar schemas and aspect-version history.
