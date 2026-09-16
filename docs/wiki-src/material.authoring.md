# material.authoring

The convenience layer for material authoring: creation, asset-level configuration (blend mode, shading model, two-sided, domain), convenience nodes, parent-material parameters, instance setters, wiring, material functions, layered/landscape work, and inspection/removal. Use this page for normal material work; use the lower-level graph surface only when an expression class is not exposed here.

## Workflow

For a graph of more than a few nodes, prefer one `call("material.compile_mgir", ...)` text-IR call over the imperative `add_*` + `connect_nodes` chain below — it authors every node, every wire, and layout (`runLayout` defaults true) from a single text body, instead of one call per node plus one per wire. The imperative path in this section is best for small edits or touching an existing graph; reach for MGIR when building a multi-node graph from scratch. See [`material.mgir`](material.mgir.md) for the syntax.

Most authoring methods do not auto-compile. Call `compile_material` explicitly after a batch of edits, especially before reading shader-derived data or marshaling the asset to a packaging step.

Practical workflow:

1. Create the material with the domain-specific create call, or `create_material` plus explicit domain / blend / shading settings.
2. Add scalar, vector, texture, texture coordinate, math, mask, and custom nodes; keep returned node ids for wiring.
3. Prefer one `add_custom_expression` with named inputs for complex UV math or repeated arithmetic. UE creates a default `None` input at index 0 before named inputs; leave it unconnected or delete it manually in the editor.
4. Connect parameters to utility / custom nodes, then connect the final node to the material output pin.
5. Call `compile_material` once at the end, then inspect with `get_material_info`. It returns `shadingModel`, wired main-output `mainInputs[]` entries ({`name`, `connectedNodeId`, `outputIndex`, mask flags}), and typed `parameters[]` entries `{name, type, nodeId, sortPriority, defaultValue}` (plus optional `group`). Defaults echo the authored types (scalar → number, vector → `{r,g,b,a}`, static switch → bool, texture parameter → asset path), so the round-trip does not rely on a clean compile. For only the output node, use `get_material_node_details` (or `material.graph.get_node_details`) with `nodeId:"Main"`.
6. Create material instances for override values; typed `set_*_parameter_value` calls act on instances, not parent defaults.

Convenience-node coverage: most simple helpers wrap the lower-level factory used by `material.graph.add_expression`, retaining stable `material.authoring.*` names while applying node-specific defaults. Parent-material parameter methods (`add_scalar_parameter`, `add_vector_parameter`, `add_static_switch_parameter`) accept `sortPriority` (default 32) for UI ordering within the parameter group. Instance writes use the typed `set_scalar_parameter_value`, `set_vector_parameter_value`, `set_texture_parameter_value`, or `set_static_switch_parameter_value`; there is no generic set-parameter dispatcher.

## Common target pins

| Target | Input pins |
| --- | --- |
| Add / Subtract / Multiply / Divide | `A`, `B` |
| Lerp | `A`, `B`, `Alpha` |
| Clamp | `Input`, `Min`, `Max` |
| ComponentMask | `Input` |
| Sine / Cosine / OneMinus | `Input` |
| SphereMask | `A`, `B`, `Radius`, `Hardness` |
| Noise (`add_noise`) | `Position` (a **float3** — see Limitations), `FilterWidth` |
| Fresnel (`add_fresnel`) | `ExponentIn`, `BaseReflectFractionIn`, `Normal` |
| TextureSample / Texture2D parameter | `Coordinates`, not `UVs` |
| AppendVector | `A`, `B` |
| If | `A`, `B`, `AGreaterThanB`, `AEqualsB`, `ALessThanB` |
| Panner | `Coordinate`, `Time`, `Speed` |
| Main material, surface/lit | `BaseColor`, `Metallic`, `Specular`, `Roughness`, `EmissiveColor`, `Opacity`, `OpacityMask`, `Normal`, `WorldPositionOffset` |
| Main material, UI | `EmissiveColor`, `Opacity` |
| Custom HLSL | Named `inputs` from `add_custom_expression`; ignore default `None` |

## Limitations and reliability notes

- **A successful write verb is not a working shader, and until 2026-09 nothing in any response said so.** `nodeId`, `"Nodes connected."`, `blocksCompiled` and a clean `get_material_info` read-back are all produced identically by a material with malformed Custom HLSL or a mismatched `SamplerType` — which writes a valid `.uasset`, saves, and renders the engine Default Material. Every verb in this namespace now publishes a measured `shaderCompile` block; branch on `shaderCompile.status`, and read `notCompiled` as "no compile has run", never as a pass. `compile_material` is the verb that blocks and measures; `material.compile_mgir` and `material.graph.create_nodes` take `waitForShaderCompile: true` to fold that measurement into the write. Full contract: [`material.compile-state`](material.compile-state.md).

- **A light function material that is not atlas-compatible modulates opaque surfaces and contributes nothing to volumetric fog, and every other read-back looks healthy.** `get_material_info` returns a measured `lightFunctionAtlas` block for `LightFunction`-domain materials — `compatible` read off the compiled shader map, `atlasGeneration` measuring `r.LightFunctionAtlas` — and `set_light_function_atlas_compatible` is the override. Any texcoord manipulation (any `TextureCoordinate` node at all) or world-position / scene-depth read excludes a material by construction, so most animated light functions need the override.

- **Some input pins require a specific vector width, and `connect_nodes` does not type-check.** No read surface reports a pin's expected type/dimension — `get_material_node_details` echoes each input's `name`/connections but not its expected type, and the pin table above lists names only. A dimension-mismatched wire is accepted silently at `connect_nodes` (the deferred-type-check behavior — graph correctness is reported by `compile_material`, not `connect_nodes`); it only fails at `compile_material` with an opaque HLSL overload error like `no matching function for call to 'MaterialExpression…'` that names the node, not the offending pin. The most common trap is **Noise's `Position` input, which is a float3** — wiring a float2 (e.g. a `TextureCoordinate`) into it compiles to invalid HLSL. Promote the float2 to a float3 first with an `AppendVector` (`A` = the float2, `B` = a `Constant` float1 for the Z component) before wiring it into `Position`.
- Material function call input pins resolve by their raw declared name. The factory's `FindExpressionInputByName` accepts both the decorated `"Name (TypeSuffix)"` form (what `UMaterialExpressionMaterialFunctionCall::GetInputName` returns) and the raw name (`GetInputNameWithType(idx, false)`). Function-call outputs are not decorated — use the raw `FunctionOutputs[]` name directly.
- Custom HLSL expressions are the most reliable path for complex math: pass `inputs` up front and reference those names in the code. Output types include `Float1`, `Float2`, `Float3`, `Float4`, and `CMOT Float4`.
- The generic `material.graph.add_node` cannot set TextureCoordinate tiling. Use `add_texture_coordinate` with explicit `uTiling` / `vTiling`.
- **`add_texture_sample`'s `texturePath` is read only at node creation — to assign or change the texture on an existing TextureSample, use `set_texture_sample_texture` (not `property.set`).** A textureless TextureSample fails `compile_material` with `(Node TextureSample) Missing input texture`. The node's `samplerType` must match the texture's *derived* sampler class: a `LinearColor` sampler requires a texture with `SRGB=false` (e.g. `TC_VectorDisplacementmap` data) — assigning an sRGB texture to a `LinearColor` sampler fails with `Sampler type is Linear Color, should be Color`. Let `set_texture_sample_texture` auto-derive `samplerType` from the texture (omit the param) so the sampler/texture pair is consistent by construction instead of hand-matching them.
- **A `nodeId` that resolves to several nodes is refused with `AMBIGUOUS_NODE`, in every verb that takes one.** `nodeId` accepts a GUID, an object name, a full path or a **parameter name**, and only the parameter name can name more than one node — legally: a triplanar or multi-band material samples one texture parameter on two or three projection planes, and those nodes share a single value at the instance level. Resolving that to the first match let a write land on one node and report plain success, with the siblings unchanged in a graph that still compiles clean; the read verbs resolved it the same way, so they agreed with the wrong answer. `set_texture_sample_texture`, `connect_nodes` (both `sourceNodeId` and `targetNodeId`), `get_material_node_details`, `set_material_layer_stack` and the whole `material.graph.*` node family now refuse instead, with `requestedNodeId`, `candidateCount` and a `candidates` array carrying each node's `nodeId` (GUID), `name`, `class` and `parameterName`. Re-issue once per node against a GUID or the full object name (`MaterialExpressionTextureSampleParameter2D_22` — the short `TextureSampleParameter2D_22` spelling does not resolve).
- Constant nodes may not be configurable after creation. Use scalar parameters with `defaultValue` when you need a non-zero constant.
- Use the typed `set_*_parameter_value` variants for instance parameter writes; there is no generic set-parameter dispatcher. Per-material cast-shadows toggling is not exposed.
- **No verb changes a parameter's default on a base `UMaterial` after the node exists.** Every `set_*_parameter_value` is `UMaterialInstanceConstant`-only and answers `UNSUPPORTED_ASSET_CLASS` on a master; `set_collection_parameter_default` writes a Material Parameter Collection, not a material. Defaults are settable **only at node creation** — `add_scalar_parameter` / `add_vector_parameter` / `add_static_switch_parameter` with `defaultValue`, `material.graph.add_expression` with `properties: {DefaultValue: …}`, or an MGIR block. To change an existing one, either author a material instance and override it there (the intended route), or reach the expression node's `DefaultValue` through generic `property.set` — which is on the no-push list below, so it will not reach a landscape on its own.
- **Verify main-output wiring with the `"Main"` node sentinel, not `property.get`.** The main material output is not a `UMaterialExpression`, so reading its inputs via `property.get` on `UMaterial`/`UMaterialEditorOnlyData` `BaseColor`/`EmissiveColor`/… reports `Expression:null` even when the pin is wired (the `FExpressionInput.Expression` pointer is not surfaced by the generic property reader) — that is **not** a valid wiring check. Instead pass the same documented `"Main"` (or empty) sentinel the write RPCs (`connect_nodes` / `break_connections`) accept to `get_material_node_details` or `material.graph.get_node_details`; the read side now resolves it to a `{nodeId:"Main", isMainOutput:true, shadingModel, inputs:[…]}` payload listing every wired main-output input. `material.decompile_mgir` remains the route for reading the whole graph's output stage at once.
- **An edit to a master material does not reach a landscape until a verb that PUSHES it runs — know which ones do.** Landscapes cache per-layer combination material instances in `ALandscapeProxy::MaterialInstanceConstantMap` that no material-change plumbing can see, so a master edit compiles cleanly and renders as a no-op until those instances are rebuilt. Measured on a real terrain with auto-exposure pinned off (`r.EyeAdaptationQuality 0`): wiring pure magenta straight into a landscape master's `BaseColor` and stopping there moved a fixed frame **0.04%** — the noise floor — while the identical edit followed by `compile_material` moved it **94.12%**. (A hue control measured per channel, so the result cannot be an exposure artefact; removing the best-fit scalar gain leaves the two at 0.05% and 99.92%.) The verbs that push, each reporting measured `consumerRefresh` coverage: **`material.authoring.compile_material`**, **`material.compile_mgir`**, and **`material.authoring.configure_layer_blend`**; `landscape.set_material` reaches the same rebuild through the assignment path. Every other graph mutator — `connect_nodes`, the `add_*` family, all of `material.graph.*`, `set_blend_mode` / `set_shading_model` / `set_material_domain` / `set_two_sided`, `set_material_layer_stack`, `add_collection_parameter_node`, and generic `property.set` on a `UMaterial` — leaves the edit durable but unreached, **by design**: forcing a full component-MIC rebuild per node insertion would make batch authoring pathological. **Finish a batch of edits to a landscape master with `compile_material` (or author it in one `compile_mgir` call) and check `consumerRefresh.complete`.** Do not use the old assign-away-and-back round-trip; it is no longer the fix for anything.
- **Close the Material Editor before mutating a material's graph.** `FMaterialEditor` edits a working *copy* of the graph; any external mutation made while the editor is open is clobbered when the editor writes its working copy back on the next sync/save. The graph/authoring mutators detect an open editor (`PinWright::Material::IsMaterialEditorOpen` → `AssetEditorSubsystem::FindEditorForAsset`) and fail loud with `EDITOR_OPEN` rather than appearing to succeed and then silently losing the edit. The guard is wired into the shared `LOAD_MATERIAL_OR_RETURN` macro — covering `add_custom_expression`, `connect_nodes`, every `add_*` expression mutator, and the MPC macro (the low-level graph verbs `material.graph.break_connections` / `material.graph.remove_node` carry their own equivalent guard). `create_*` handlers are intentionally exempt because they author new assets that no editor can have open. Separately, `material.graph.*` and `material.authoring.*` share param-name aliases via `MaterialHandlerUtils` (`assetPath`/`path`/`materialPath`; `expressionClass`/`nodeType`/`className`) so the sibling RPC families do not drift on required parameter names. The `create_*` verbs additionally accept the combined single-`assetPath` destination shape the operate verbs take (split server-side into name + folder via `MaterialCreatePathParamUtils`), while still honoring the split `name` + folder `path` form — see [`create_material`](material.authoring.create_material.md).

## Instance API

Six RPCs targeting `UMaterialInstanceConstant` assets. All accept an optional `save` flag and operate exclusively on the instance — calling them against a `UMaterial` returns `UNSUPPORTED_ASSET_CLASS` (use `get_material_info` instead). The mirror now holds too: the `UMaterial`-only verbs answer `UNSUPPORTED_ASSET_CLASS` naming the class found (not `ASSET_NOT_FOUND`) when handed an instance, and the three whose property *is* overridable per-instance — `set_blend_mode`, `set_shading_model`, `set_two_sided` — name [`set_material_instance_base_property_overrides`](material.authoring.set_material_instance_base_property_overrides.md) in the message.

- `clear_parameter_override(assetPath, parameterName, parameterType ∈ {scalar|vector|texture|staticSwitch}, save?)` — removes a single override and returns the now-inherited parent default. Implementation note: UE 5.6 has no single-parameter `ClearParameterValueEditorOnly(FMaterialParameterInfo)`; the plural `ClearParameterValuesEditorOnly()` wipes everything. The handler does `TArray::RemoveAll` on the relevant `*ParameterValues` array (or `StaticParameters.StaticSwitchParameters` for switches) and runs `UpdateStaticPermutation` when needed.
- `set_static_switch_parameter_value(assetPath, parameterName, value, save?)` — wraps `SetStaticSwitchParameterValueEditorOnly` in `FMaterialInstanceParameterUpdateContext` so the destructor handles the permutation rebuild.
- `set_material_instance_parent(assetPath, parentMaterial, preserveOverrides? default true, save?)` — `SetParentEditorOnly(NewParent, /*RecacheUniforms*/true)`. When `preserveOverrides=false` the handler additionally calls `ClearParameterValuesEditorOnly()` to drop all existing overrides.
- `get_material_instance_info(assetPath)` — read-back surface: `{parent, overrides:{scalar,vector,texture,staticSwitch}, inherited:{...}, parameters:[{name,type,group,sortPriority}]}`. `inherited` values come from `Parent->Get*ParameterDefaultValue`; parameter metadata walks the parent's `ExpressionCollection.Expressions`.
- `set_material_instance_parameters(assetPath, scalar?, vector?, texture?, staticSwitch?, save?)` — batch write that holds one `FMaterialInstanceParameterUpdateContext` across the entire sweep so `PostEditChange` / shader recompile fires exactly once. Returns `{applied:[...], failed:[...]}`.
- `set_material_instance_base_property_overrides(assetPath, …, clear?, save?)` — overrides the base *material* properties (blend mode, shading model, two-sided, opacity mask clip value, …) rather than parameters, so a translucent or two-sided variant of a master needs an instance instead of a second master material. See [`set_material_instance_base_property_overrides`](material.authoring.set_material_instance_base_property_overrides.md).

For create + override in one round-trip, `create_material_instance` accepts an optional inline `parameters` object — the **same type-keyed `{scalar, vector, texture, staticSwitch}` shape** as `set_material_instance_parameters`; no follow-up set call is needed for the common create-and-tint case. See [`create_material_instance`](material.authoring.create_material_instance.md).

## `name` is a bare asset name, the folder is `path`

Applies to every creator in this namespace: `create_material`, `create_material_function`, `create_material_instance`, `create_landscape_material`, `create_decal_material`, `create_post_process_material`, `create_material_layer`, `create_material_layer_blend`, `create_parameter_collection`, and `add_landscape_layer` (whose leaf slot is `layerName`). The leaf is validated against the engine's own object-naming rules (`FName::IsValidXName` / `INVALID_OBJECTNAME_CHARACTERS`, which rejects `/`, `\`, `.`, `:` and `..`) and the composed `<path>/<name>` against `FPackageName::IsValidLongPackageName`, so a path-shaped name, a leading or trailing slash, or an unmounted root is refused `INVALID_ARGUMENT` with the engine's own reason text quoted. A folder that already ends in `/` is still accepted and means the same folder.

This is not pedantry about naming. A `name` containing `//` reaches `CreatePackage`, which logs that at **Fatal** — a verbosity that is not compiled out in any configuration — so the call did not fail: the editor **process** died, taking every unsaved package in that editor with it. The same defect was measured end-to-end on `foliage.add_type`; see that verb on the [`foliage`](foliage.md) page.

The eight verbs that split `name` + `path` share one destination resolver, so the rule is identical across them and the combined single-`assetPath` form is validated the same way. `create_material_instance` runs the check **before** it resolves `parentMaterial`, so a payload wrong in both ways reports the name first.

## See also

- [`material.mgir`](material.mgir.md) for the one-call bulk text-IR path (`material.compile_mgir`) — see the Workflow note above.
- [`material.compile-state`](material.compile-state.md) for the `shaderCompile` block every verb here publishes, the five statuses, and how to get a real verdict rather than a probe.
- [`material.graph`](material.graph.md) for node types not exposed here.
- [`blueprint.graph`](blueprint.graph.md) for the analogous low-level graph model used in Blueprints.
- [`niagara`](niagara.md) for assigning materials to Niagara particle renderers from the persistent Niagara workflow.

### material.authoring.compile_material

Force-compile the material's shader. Accepts a `UMaterial` **or** a `UMaterialInstanceConstant` — an instance is where a static-switch override lives, so it is where a new static permutation is created and most needs compiling, and the `notCompiled` hint on every instance response points here. Most authoring ops mark the material dirty but defer compilation until the editor next requests a render. Call this at the end of an authoring batch when you need:

- A definitive pass/fail signal on graph correctness (compile errors surface here, not on `connect_nodes`).
- Up-to-date packaged shader bytes before save / export.
- Material-derived stats (use `call("asset.get_material_stats", ...)` after compile).

The handler submits the material's permutations synchronously, then drains what is left of **this material's** compile against a 90 s ceiling, pumping `FAssetCompilingManager::ProcessAsyncTasks` while it waits, and reads the failed-permutation HLSL errors back off the `FMaterialResource`. It no longer calls `GShaderCompilingManager->FinishAllCompilation()`, which had no ceiling and blocked on every pending shader map in the editor plus the whole texture-compile queue — other callers' work, which contributed nothing to this verb's answer. `compiled` is always `true` and is **not** a measurement — read `compileStatus` and `compileSucceeded` instead:

- `compileStatus` (`string`): what actually happened. One of `completed`, `failed`, `timedOut`, `outstanding`, `notCompiled`. This is the single field to branch on.
- `compileSucceeded` (`bool`): the boolean verdict. `true` only when a compile **landed** and no permutation failed.
- `compileWaitedMs` (`number`): wall clock spent inside the drain.

**An empty `compileErrors` is not a clean compile.** A compile that was skipped (`GShaderCompilingManager->IsShaderCompilationSkipped()`), one whose wait expired, and one for which the engine has no material resource all report zero errors. `compileSucceeded` used to be exactly `compileErrors.length === 0`, so all three read as successes; it now additionally requires a complete game-thread shader map, and a response with `compileStatus != "completed"` carries a `warnings[]` entry naming the case (board `B-compile-material-blocks-and-mislabels`).

The two legacy fields remain for existing callers:

- `compiledWithErrors` (`bool`): `true` if any shader permutation failed. When `true` the material silently falls back to the Default Material in game — treat the op as failed.
- `compileErrors` (`string[]`): the HLSL error text (translation errors, or shader-compile errors like `FDFMatrix does not provide a subscript operator`). Empty on a clean compile — and equally empty when nothing compiled, which is what `compileStatus` disambiguates.

Compiling synchronously is expensive — large material trees with many permutations can take seconds. For batch authoring, defer compile until the last edit.

**On a material instance, read `shaderCompile.measuredSubject` before reading anything else.** An instance with a static-switch or base-property override owns its own permutation resource and is compiled directly (`instanceStaticPermutation`). An instance without one owns no shader at all: `UMaterialInstance::GetMaterialResource` forwards to the parent and `CacheShaders` submits nothing, so this verb compiles and reports the **parent's** resource (`parentInherited`, with `measuredMaterialPath` naming it and a `warnings[]` entry saying so). The parent is not dirtied, not consumer-refreshed and not saved — only the asset you named is saved — so a caller scoped to one instance can use this verb without touching the shared master. `consumerRefresh` is emitted for a `UMaterial` only.

`usage` is not on this response; it is on [`get_material_info`](material.authoring.get_material_info.md) and `get_material_instance_info`. A material whose consumer's `EMaterialUsage` is undeclared renders the engine Default Material with a clean compile here — see [`material.compile-state`](material.compile-state.md).

**A compiled master is not a refreshed world.** `UMaterial::PostEditChange` regenerates the master's `StateId` and recompiles it but creates no `FMaterialUpdateContext`, so loaded instances keep the previous static-permutation shader map; landscapes also cache per-layer "combination materials" in `ALandscapeProxy::MaterialInstanceConstantMap` that no update context can see. Before this fix, a landscape edit compiled cleanly with `compileSucceeded: true` but changed nothing on screen: with auto-exposure pinned off, the measured frame movement was 0.04% before `compile_material` versus 94.12% after it (full provenance is in [Limitations and reliability notes](material.authoring.md#limitations-and-reliability-notes)).

`compile_material` now closes both gaps itself: it scopes the change in an `FMaterialUpdateContext` and rebuilds the component material instances of every landscape in the level that renders with the material. **The round-trip workaround is no longer needed.** Coverage is reported, measured, in `consumerRefresh`:

- `measured` (`bool`): whether consumers were enumerated at all. `false` (no editor world) means the numbers below are not measurements.
- `consumersFound` / `consumersRefreshed` / `consumersWithNothingToRefresh` (`int`): landscapes rendering with the material, of those the ones whose component material instances were rebuilt, and of those the ones that cached nothing to rebuild.
- `subObjectsRefreshed` (`int`): landscape components that received a *new* material instance. Measured by object identity — the engine allocates a fresh `ULandscapeMaterialInstanceConstant` per component per rebuild — so this number cannot be faked by the code that issues the rebuild.
- `refreshed` (`string[]`): the landscape actor names, so you can check the one you care about is there rather than trusting a count.
- `complete` (`bool`): **branch on this.** It is the conjunction of "we looked" and "we got them all", so an unmeasured run cannot score like a clean one.

When `complete` is false the response also carries a `warnings[]` entry naming what was missed and the remedy (`landscape.set_material` on the affected landscape). A compile whose consumers were not refreshed is not a plain success — the shaders are current and the screen is not.

Cross-ref: `call("blueprint.compile")` is the analogous explicit-compile call for blueprints.

### material.authoring.connect_nodes

`inputName` accepts either the pin's displayed name or its internal `FExpressionInput` property name. For example, UE 5.8 displays a `StaticSwitchParameter`'s `A`/`B` inputs as `True`/`False`, so all four spellings are valid; the detail response keeps `name` as the display label and adds `internalName` when the reflected spelling differs. An unknown name is refused before the graph changes with `PIN_NOT_FOUND`; `result.candidates` lists every accepted spelling. A successful response is sent only after read-back confirms that the selected input references the requested source node, including the main material output path.

The call still does not type-check the wire, so a dimension mismatch surfaces later as an opaque HLSL overload error from `compile_material` (see the `Noise.Position` float3 case in [Limitations and reliability notes](material.authoring.md#limitations-and-reliability-notes)); and it does not push the edit to a landscape already rendering the material. Finish a batch with [`compile_material`](material.authoring.compile_material.md) and branch on `consumerRefresh.complete`; the full push/no-push list is above.

### material.authoring.add_if

`add_if` creates a `MaterialExpressionIf` — a numeric **comparator**, not a clean bool true/false selector. (`add_switch` is an exact alias that mints the identical `UMaterialExpressionIf`.) The node's pins are the `If` row in the [Common target pins](material.authoring.md#common-target-pins) table above: `A`, `B`, `AGreaterThanB`, `AEqualsB`, `ALessThanB`.

Usage recipe — to drive it as a true/false switch you must supply the comparison convention yourself:

- bool/condition driver → input `A`,
- numeric reference value (a 0/1 selector) → input `B`,
- "true" branch → `AGreaterThanB`,
- "false" branch → `ALessThanB` and/or `AEqualsB`.

Because `B` is a numeric reference, driving the comparator from a `StaticBoolParameter` (which reads as 0 or 1) needs a `0`/`1` reference value on `B` (e.g. a Constant node) plus a selector value — the bool does not pick a branch by itself. There is no "value picks A or B" semantics here; the result is chosen purely by the `A` vs `B` numeric comparison.

If you want a genuine compile-time bool switch with `A`/`B`/`Value` (true→A, false→B) pins, use `add_static_switch_parameter` (a `UMaterialExpressionStaticSwitchParameter`), or `material.graph.add_expression` with `UMaterialExpressionStaticSwitch` (pass the full class name to get the non-parameter `StaticSwitch` rather than the parameter variant).

### material.authoring.add_switch

`add_switch` is an exact alias of `add_if`: it creates the numeric-comparator `MaterialExpressionIf` with pins `A`, `B`, `AGreaterThanB`, `AEqualsB`, and `ALessThanB`, **not** a bool true/false switch. See [`add_if`](material.authoring.add_if.md) for the comparator convention and true bool-switch alternatives.

### material.authoring.set_texture_sample_texture

Assign or change the texture on an **existing** `TextureSample` / `TextureSampleParameter2D` node — the in-place write `add_texture_sample` lacks (`add_texture_sample`'s `texturePath` is read only at node creation). Use this instead of a raw `property.set` on the node's `Texture` field.

Params: `assetPath`, `nodeId` (GUID, node name — the full `MaterialExpressionTextureSampleParameter2D_22` spelling, not the `TextureSample…` short form — or parameter name), `texturePath`, optional `samplerType` (`Color|LinearColor|Normal|Masks|Alpha`).

- **Omit `samplerType` to auto-derive it from the texture** (`UMaterialExpressionTextureBase::GetSamplerTypeForTexture`, the same mapping the shader compiler's `VerifySamplerType` enforces). This is the recommended path: it makes the sampler/texture pair consistent by construction and avoids the `Sampler type is Linear Color, should be Color` compile error you get when a hand-picked `samplerType` doesn't match the texture's sRGB/compression.
- Pass `samplerType` only to force a specific sampler (it is stamped verbatim — you are then responsible for picking a compatible texture).
- **A parameter name shared by several nodes is refused, not applied to one of them.** Two or three sample nodes carrying one texture-parameter name is the normal shape of a triplanar or multi-band material (they share a single value at the instance level), so `nodeId: "AOAtlas"` can name more than one node. Repointing whichever iterated first left the siblings on the old texture in a graph that still compiles clean — nothing in the payload, the compile status or the read verbs could reveal it. Such a call now fails `AMBIGUOUS_NODE`; re-issue once per node with a `nodeId` from the error's `candidates` (each carries `nodeId`, `name`, `class` and `parameterName`).

Returns `{nodeId, texturePath, samplerType, nodesChanged}` (the resulting sampler, so you can confirm the auto-derived class without a re-read; `nodesChanged` is always `1`, since a wider match is refused). Errors: `NOT_FOUND` (no such node), `AMBIGUOUS_NODE` (the `nodeId` named several nodes — payload carries `requestedNodeId`, `candidateCount`, `candidates`), `WRONG_NODE_TYPE` (node is not a TextureSample), `TEXTURE_NOT_FOUND` (texture path won't load), `EDITOR_OPEN` (close the Material Editor first — see [Limitations](material.authoring.md#limitations-and-reliability-notes)).

### material.authoring.create_material

Create a new `UMaterial` asset. The destination accepts **either shape**, so the same path you use everywhere else in the namespace works here too:

- **Split** — `name` (the asset leaf) + optional `path` (the destination *folder*, default `/Game/Materials`). This is the historical shape; existing callers are unchanged.
- **Combined** — one fully-qualified `assetPath` (e.g. `/Game/Pickups/Materials/M_StylizedLeaf`), the same single-path slot every *operate* verb in `material.authoring` takes. It is split server-side into the leaf name + parent folder, so you do not have to pre-split a path you already have in hand. (`assetName` is also accepted as a synonym of `name`.)

Pass one or the other: an explicit `name` wins (with `path`/default as the folder); otherwise the combined `assetPath` is split. The folder `path` slot always means a folder — only `assetPath` is treated as a combined leaf+folder. The same `name`-or-`assetPath` contract applies to the whole `create_*` family (`create_material_function`, `create_material_instance`, `create_landscape_material`, `create_decal_material`, `create_post_process_material`, `create_material_layer`, `create_material_layer_blend`).

Optional `materialDomain`, `blendMode`, `shadingModel`, `twoSided`, and `save` (default true) configure the asset at creation.

**Re-running on an existing material updates it in place.** When a `UMaterial` already lives at the target path the call succeeds and returns that asset with `existing: true` and `mode: "updated_in_place"`; a first create returns `existing: false`, `mode: "created"`. Only the optional properties you actually passed are applied — **the expression graph is left untouched**, because authoring scripts rebuild it themselves through `material.graph.*`, and every material instance and mesh slot pointing at the asset keeps resolving.

Pass `overwrite: true` for the old wipe-and-recreate semantics. The asset is deleted and rebuilt only when nothing references it; otherwise the call fails with `ASSET_IN_USE`, whose error data carries `referencers` / `referencerCount` (referencing packages from the asset registry) and `referencingActors` / `referencingActorCount` (live level actors holding it in memory, which the registry cannot see), both lists capped at 25 with uncapped counts. Dropping `overwrite` is the fix — that re-run updates in place and keeps every reference. No delete is attempted on the registry branch, so nothing is orphaned.

A *different* asset class occupying the path is never replaced, whatever `overwrite` says: the call fails with `ASSET_ALREADY_EXISTS` naming the class that is actually there.

Both codes come from the non-interactive create policy shared by every `create_*` verb — see [Running PinWright unattended](unattended.md) for why a create verb must never let an existing asset reach the engine's `CanCreateAsset` path.

### material.authoring.create_material_function

Creates a reusable `UMaterialFunction` asset. Authoring a complete function is three phases, all node-level (no text-IR required):

1. **Inputs/outputs** — `add_function_input` (one `UMaterialExpressionFunctionInput` per parameter) and `add_function_output` (one `UMaterialExpressionFunctionOutput` per result).
2. **Body** — the same node-add / connect / remove RPCs that author a `UMaterial` graph accept a material-function `assetPath` too: `add_math_node`, `material.graph.add_node` / `add_expression` / `connect_nodes` / `remove_node`. Build the math, then wire `input → math → output` with `connect_nodes` (the function has no `Main` pseudo-node — wire into a `FunctionOutput` node by passing its node id as `targetNodeId`).
3. **Call it** — `use_material_function` drops a `MaterialFunctionCall` node into a calling material.

**A caller's wire into a function pin survives on one thing: the pin's GUID.** `UMaterialExpressionMaterialFunctionCall` marks its pointers to the function's pins `transient` and serializes only `FFunctionExpressionInput::ExpressionInputId` / `FFunctionExpressionOutput::ExpressionOutputId`, copied from `UMaterialExpressionFunctionInput::Id` / `UMaterialExpressionFunctionOutput::Id`. On load, `UpdateFromFunctionResource` re-links purely by that GUID; a GUID that matches nothing is **not an error** — the input connection is simply not carried over and any material input reading that output is reset to `{Expression: null, OutputIndex: -1}`. The material then compiles and renders as if the wire had never been authored.

Both pin kinds get a persistent ID at creation — `add_function_input`, `add_function_output`, the generic expression factory (`material.graph.add_node` / `add_expression`) and the MGIR emitter — and every PinWright save of a material function **repairs** an anonymous pin before writing, so no route here saves one. `ConditionallyGenerateId(false)` fills only invalid IDs, so a valid caller-supplied ID is preserved.

**Recompiling a function does not renumber its pins.** `material.compile_mgir` in `Append` mode empties the function graph and re-emits it, and a re-emitted pin is a new object with a new GUID; the compiler therefore captures pin-name → GUID before the clear and hands each rebuilt pin its previous identity back. Rename a pin and it is treated as a new pin (its callers lose that one wire) — which is the engine's own rule in reverse, since `UpdateFromFunctionResource` propagates a new *name* over a matched GUID.

**Detection.** `get_material_info` emits a `functionCallIdentity` block — `{unstable: [{nodeId, functionPath, pinKind, pinName, reason}], warning}` — **only when something is wrong**; its presence is the signal. `reason` is `missing-persistent-id` (the pin has no GUID), `stale-persistent-id` (the cached GUID names no pin of that function any more, so the wire is already doomed) or `duplicate-persistent-id` (two pins share a GUID, so the wire resolves to the first and can silently move). Remedy: re-save the material **function** first, then re-wire and re-save the consumer — saving the consumer alone re-records the same doomed GUID.

**Binding repairs a legacy function, and always writes it.** `use_material_function` re-saves the function at `functionPath` on **every** call, before the call node caches its GUIDs, and reports that write in a `functionIdentity` block (`assetPath`, `saved`, `saveState`, `saveDetail`); a write that did not land also raises a `warnings[]` entry, because the node's wires are then not durable. The write is unconditional because the state needing it is invisible: `PostLoad` calls `ConditionallyGenerateId(false)` on load, so a function whose `.uasset` holds an all-zero pin GUID still presents a valid one in memory, and an in-memory check reports "nothing to repair" for exactly the asset that needs repairing. One bind therefore ends the every-load loop for a function authored before this guard. The material itself is not saved by this verb.

**Limit.** The `functionCallIdentity` read above is in-memory, so an asset saved with anonymous pins *before* this guard shipped still reads clean there — `PostLoad` replaces the invalid GUID and `UpdateFromFunctionResource` discards the caller's stale GUID during load, both before any verb can look. Binding repairs the function; the wires it already dropped cannot be recovered and must be re-authored, and each already-saved consumer must be re-wired and re-saved once.

`auto_layout` accepts a function path too, so you can lay the body out after building it.

### material.authoring.add_function_input

Adds a `UMaterialExpressionFunctionInput` to a material function. **By default the input is REQUIRED**: a material that calls the function but leaves this input unwired fails its *own* `compile_material` with `Missing function input <name>`. Make a multi-input reusable function callable without wiring every input at every call site by marking inputs optional at creation:

- `optional: true` — sets the engine field `bUsePreviewValueAsDefault=true` so an unconnected input falls back to its `PreviewValue` (zero unless you also pass `defaultValue`).
- `defaultValue: [x,y,z,w]` (or a single number for a scalar) — populates `PreviewValue` and **implies `optional:true`**. Components beyond the input's width are ignored; unspecified components stay 0. Not applicable to `Texture2D`/`TextureCube`/`MaterialAttributes` inputs.

The response echoes `{nodeId, optional}` so you can confirm the input is optional without a re-read. Without these knobs the only workaround was setting `bUsePreviewValueAsDefault` + `PreviewValue` per input via raw `property.set` (an engine-header dive); prefer the typed params here.

### material.authoring.create_material_instance

Create a `UMaterialInstanceConstant` child of a parent `UMaterial`. **This is the preferred creator**; `path` is optional (default `/Game/Materials`).

Params: `name` (req — or a combined `assetPath` split into name + folder, same as `create_material`), `parentMaterial` (req), `path` (opt folder, default `/Game/Materials`), `parameters` (opt), `save` (opt, default true).

- **`parameters`** — optional inline overrides applied at creation, the **same type-keyed shape** as `set_material_instance_parameters`: `{scalar:{Name:num}, vector:{Name:{r,g,b,a}}, texture:{Name:assetPath}, staticSwitch:{Name:bool}}`. All sets share one `FMaterialInstanceParameterUpdateContext` (one recompile). Use this for the common **create-and-tint** case in a single round-trip; the response then carries `{applied:[...], failed:[...]}` alongside the asset verification. Omit it and call the typed `set_*_parameter_value` / `set_material_instance_parameters` methods afterward if you prefer to author overrides separately.
  **Those four bucket names are the whole first level and anything else is refused** with `UNKNOWN_NESTED_PARAMS` naming the key. Until that gate landed, a payload one level too shallow (`{"r":1,"g":0,"b":0}` — the correct vector shape written where a bucket belongs) or a misspelt bucket (`vectors`, `static_switch`) matched nothing and returned **success with `applied:[]` AND `failed:[]`**; two empty arrays were the only evidence the caller got. The material parameter names *inside* each bucket are yours and are not checked — a name your parent material does not carry lands in `failed[]`, which is a different answer.

With the former `asset.create_material_instance` duplicate creator removed, this verb is the creator for a `UMaterialInstanceConstant`: `path` is optional and overrides can be set inline at creation via `parameters`. `parentMaterial` must be a `UMaterial` master — an instance path is rejected with `UNSUPPORTED_ASSET_CLASS` naming the class, even though the engine's own `UMaterialInstance::Parent` is a `UMaterialInterface` and would allow an instance-of-an-instance.

### material.authoring.set_blend_mode

`UMaterial`-only, and the choice of asset is the whole decision. Setting the blend mode on a master changes **every** instance of it. To make one translucent variant of an opaque master, do not edit the master and do not author a second master — override it on an instance with [`set_material_instance_base_property_overrides`](material.authoring.set_material_instance_base_property_overrides.md). Handing this verb an instance path answers `UNSUPPORTED_ASSET_CLASS` and names that verb.

An unrecognised `blendMode` is now rejected with `INVALID_BLEND_MODE` listing the accepted names; it previously left the material untouched and reported success. The same holds for `set_shading_model` and `set_material_domain`.

Landscape masters: this verb is on the **no-push** list — the edit is durable but a landscape keeps rendering the old permutation until `compile_material` runs. See Limitations and reliability notes on the namespace page.

### material.authoring.set_material_instance_base_property_overrides

The escape hatch from "I need a translucent version of this master". `BasePropertyOverrides` is the per-instance override block for the properties that are otherwise fixed by the master — blend mode, shading model, two-sided, opacity mask clip value, and the tessellation / displacement / velocity / Lumen flags. Before this verb existed the only route to a translucent variant of an opaque master was a whole second master material.

**Each slot is independent and omission means "leave alone".** Omitting `twoSided` does not clear a `twoSided` override; it does not touch it. That makes the verb safely re-runnable and lets you write one slot without reading the other twelve first.

**Clearing is a separate spelling: `clear`.** Pass the override names to turn back off — `clear: ["blendMode", "twoSided"]` — or `clear: ["all"]` for every slot this verb writes. A cleared slot inherits from the parent again. `clear` is applied **before** the value params, so naming a slot in both ends up *set*, not cleared. A name in `clear` that is not an override slot comes back in `unknownClear` rather than failing the call — check that field if a clear looks like it did nothing.

**Read the response, not a follow-up call.** It returns `overridden` (the slots whose `bOverride_*` is live *after* the write), `applied` / `cleared` (what this call did), `effective` (the resolved values, read through the engine's own `GetBlendMode` / `IsTwoSided` / … accessors, so a cleared slot reports the parent's value), and a `basePropertyOverrides` block field-for-field identical to the one `get_material_instance_info` and `asset.dump` emit.

Gotchas:

- **A base-property override changes the static permutation, so this is a shader recompile, not a field write.** The handler routes through `FMaterialInstanceParameterUpdateContext` (the same vehicle `set_static_switch_parameter_value` uses) because a direct assignment to `BasePropertyOverrides` reads back correctly in memory while the compiled material keeps the parent's values — the classic "verified in memory, renders nothing" failure.
- **A value field is meaningless without its flag — read `overridden` / `effective`, never a bare value.** `UMaterialInstance::UpdateOverridableBaseProperties` back-fills every *un*-overridden value field from the parent, so after clearing `blendMode` the `basePropertyOverrides.BlendMode` field still reads a real blend mode (the parent's) next to `bOverride_BlendMode: false`. Diffing the value field alone cannot tell an override from an inheritance.
- **`TranslucentColoredTransmittance` is Substrate-only.** With Substrate off the engine sanitizes it to `Translucent` inside the same update pass, so `effective.blendMode` reads `Translucent`. With Substrate on, legacy blend modes are converted against the *resolved* shading model. Either way that is the engine, not a lost write — which is why `effective` is read back through the accessors instead of echoed from the request.
- **A shading-model override is applied verbatim; only `FromMaterialExpression` is special.** The engine refuses that one as an override and falls back to the parent (`MaterialInstance.cpp`), so this verb does not expose it. Every other name in the list is taken as given, including one the parent's graph was never authored for — the compile is where that surfaces, not this call.
- **Not exposed:** `DisplacementScaling` / `DisplacementFadeRange` (struct-valued) and the `UsageFlags` bitmask, which has its own engine API. There is also no translucency-lighting-mode slot — that property is not in `FMaterialInstanceBasePropertyOverrides` on UE 5.8 at all, so no verb can override it per-instance; change it on the master with `property.set`.
- Overriding a property on an instance is invisible to `get_material_info`, which reads the **master**. Use `get_material_instance_info` (or this verb's own response) for the instance's view.
