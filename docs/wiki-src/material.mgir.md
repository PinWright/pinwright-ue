# material.mgir

MGIR is the bulk text path for material graph authoring, backed by `material.compile_mgir` for import and `material.decompile_mgir` for inspection. Use this page when a graph is easier to create or review as text than as repeated `material.graph` calls, and consult it for syntax, mode semantics, Substrate sugar, and round-trip behavior.

## Compile / decompile

`asset.dump` writes the same decompile output to `mgir.txt`; MGIR is registered as a uniform text-IR sidecar beside the decompile handler, so dump baselines and live `material.decompile_mgir` stay on the same decompiler path.

`material.compile_mgir` accepts:

- `text` - MGIR document text.
- `mode` - `Append` clears the target graph before applying the document; `Extend` adds to existing graph state. `Extend` **only adds** - see [Extend cannot update an existing expression](#extend-cannot-update-an-existing-expression).
- `context` - fallback target asset path for an unnamed entry block.
- `runLayout` - assigns positions to expressions that do not specify `@(x, y)`.
- `save` - writes modified assets to disk when true (default). When false, the graph remains an in-memory edit.
- `waitForShaderCompile` - block until the shaders of every material this document wrote finish compiling, and report the real verdict rather than the non-blocking probe.

**"compile" in `compile_mgir` means the GRAPH, not the shader.** `blocksCompiled` and `expressionsCreated` count expressions placed and wired; a document whose Custom node holds malformed HLSL, or that samples a texture with the wrong `SamplerType`, returns the same numbers as one that renders. The response's `shaderCompile` block is the measurement — branch on `shaderCompile.status`, treat `notCompiled` as "no compile has run" rather than as clean, and pass `waitForShaderCompile: true` (or finish with [`material.authoring.compile_material`](material.authoring.compile_material.md)) for a real answer. `shaderCompile.materials[]` breaks the verdict down per asset for a multi-entry document, and a failure is also raised into `warnings[]`. See [`material.compile-state`](material.compile-state.md).

`material.decompile_mgir` accepts `assetPath`, optional `includeReferencedFunctions`, and optional `emitSubstrateSugar`.

`material.compile_mgir` returns `consumerRefresh` alongside `blocksCompiled` / `expressionsCreated` / `assetPaths`: the measured rebuild of every landscape rendering with a master this call compiled, summed over the document's material blocks. **Branch on `consumerRefresh.complete`**; when it is false the response also carries a `warnings[]` entry naming what was missed and the remedy. Field meanings are documented once on [`material.authoring.compile_material`](material.authoring.compile_material.md).

The response also carries the shared save contract: `saveRequested`, `saved`, `saveState`, `saveDetail`, and `pendingFlush`. If PIE blocks the write after the graph compile succeeds, the call remains successful and preserves the compile, shader, and consumer fields; it reports `saved:false`, `pendingFlush:false`, `saveState:"blockedByPie"`, and `saveError:"PIE_ACTIVE"`. The graph is already current in memory, and no flush can write until PIE ends, so do not rerun an `Append` document: wait for PIE to end, then save the paths in `assetPaths`. `MGIR_SAVE_FAILED` remains a hard error for non-PIE save failures.

This matters most in `Append` mode, which empties and rebuilds the whole expression collection — exactly the layer-allocation change a landscape's cached combination materials are keyed on. Before this was measured, compiling a landscape master through MGIR wrote a correct `.uasset` to disk, returned `blocksCompiled: 1`, and left the terrain rendering the previous shader map: on a real terrain the identical MGIR document moved a fixed frame **0.79%** (noise floor) before the fix and **93.70%** after it, with no round-trip and no separate `compile_material` call.

Example:

```text
entry material `/Game/Materials/M_MGIR_Test` {
    %base = constant Float3(0.2, 0.4, 1.0) @(0, 0)
    output BaseColor: %base
}
```

MGIR entry targets are name tokens, not double-quoted strings. Paths normally appear backtick-delimited because `/` and `.` are not bare-token characters.

`call` instructions use qualified material-expression class path name tokens, for example ``call `/Script/Engine.MaterialExpressionAdd`()``. Short aliases such as `call Add()` are intentionally rejected inside MGIR text; the RPC material authoring helpers may still accept those aliases outside MGIR.

## Substrate sugar

Canonical MGIR still decompiles Substrate nodes as ordinary `call` instructions with qualified Unreal class paths. This remains the default because it is the least ambiguous round-trip form.

Set `emitSubstrateSugar: true` on `material.decompile_mgir` to present common Substrate topology nodes with short assigned-call sugar:

```text
entry material `/Game/Materials/M_Substrate` {
    %front = slab(DiffuseAlbedo: %albedo)
    output FrontMaterial: %front
}
```

The parser accepts the assigned sugar form and lowers it to canonical `call` instructions internally. These aliases are recognized:

| Sugar | Canonical class |
|---|---|
| `slab` | `/Script/Engine.MaterialExpressionSubstrateSlabBSDF` |
| `mix_h` | `/Script/Engine.MaterialExpressionSubstrateHorizontalMixing` |
| `mix_v` | `/Script/Engine.MaterialExpressionSubstrateVerticalLayering` |
| `layer` | `/Script/Engine.MaterialExpressionSubstrateVerticalLayering` |
| `weight` | `/Script/Engine.MaterialExpressionSubstrateWeight` |
| `add` | `/Script/Engine.MaterialExpressionSubstrateAdd` |
| `select` | `/Script/Engine.MaterialExpressionSubstrateSelect` |

Do not write `call slab(...)`. Sugar is only the assigned shorthand `%name = slab(...)`; canonical calls still require a qualified material-expression class path. If the graph contains any Substrate expression while `r.Substrate=0`, decompile still succeeds and returns a warning in the response payload.

## Material-level properties

An `entry material` block carries the material's own settings as `property Name: Value` lines beside the graph:

```text
entry material `/Game/Materials/M_Glass` {
    property MaterialDomain: MD_Surface
    property BlendMode: BLEND_Translucent
    property ShadingModel: MSM_DefaultLit
    property TwoSided: true
    property bIsThinSurface: false
    property TranslucencyLightingMode: TLM_SurfacePerPixelLighting
    property bUseMaterialAttributes: false
    property OpacityMaskClipValue: 0.333300
    property NumCustomizedUVs: 0
    %o = constant Float1(0.4)
    output Opacity: %o
}
```

Nine properties are carried. The tokens are `UMaterial`'s own reflected `UPROPERTY` names, resolved through the property system rather than a mapping table, so an MGIR token cannot drift from the engine field it writes:

| Property | Values | Why it is carried |
|---|---|---|
| `MaterialDomain` | `MD_*` | A wrong domain discards the whole surface configuration |
| `BlendMode` | `BLEND_*` | An opaque material discards a connected `Opacity` pin |
| `ShadingModel` | `MSM_*` | Includes `MSM_FromMaterialExpression` |
| `TwoSided` | bool | |
| `bIsThinSurface` | bool | Substrate thin surface; also disables subsurface profiles |
| `TranslucencyLightingMode` | `TLM_*` | |
| `bUseMaterialAttributes` | bool | Selects the `MaterialAttributes` root over the individual pins |
| `OpacityMaskClipValue` | float | The masked cutoff |
| `NumCustomizedUVs` | int | How many `CustomizedUV` pins are exposed |

Names match case-insensitively and a leading `b` is optional (`UseMaterialAttributes` resolves the same slot as `bUseMaterialAttributes`). Enum values accept the canonical member (`BLEND_Translucent`), the prefix-less short form (`Translucent`), or the scoped form (`EBlendMode::BLEND_Translucent`); booleans accept `true`/`false`/`yes`/`no`/`1`/`0`. `_MAX` and `_NUM` sentinels are refused.

`material.decompile_mgir` emits all nine every time, including ones sitting at their engine default - that is what makes a decompiled document reproduce the material exactly. `material.compile_mgir` applies **only the properties the document names**, in both `Append` and `Extend`, so an absent property leaves the target's current value alone and a hand-written fragment cannot silently reset a master's blend mode.

Every `property` line is validated **before the target asset is loaded**. An unknown name fails with `MGIR_UNKNOWN_PROPERTY` and an unresolvable value with `MGIR_BAD_PROPERTY_VALUE`, in both cases without `Append` having emptied the target graph first.

`property` is valid only inside `entry material`. A material function has no material-level properties, and a `property` line in an `entry function` block is refused rather than ignored.

Do not confuse `property ShadingModel: MSM_DefaultLit` (the enum on the material) with `output ShadingModel: %x` (the graph pin that feeds `MSM_FromMaterialExpression`). Same word, different instruction, both round-trip.

## Extend cannot update an existing expression

`Extend` adds new expressions to a graph. It has no update path: the compiler's emitter can only create, so a symbol that names an expression **already in the target graph** cannot be rewritten in place.

Naming one is now refused with `MGIR_EXTEND_CANNOT_UPDATE` rather than silently duplicated. This matters because the failure it replaces was invisible: compiling a decompiled document in `Extend` with one value edited used to create a **second** expression carrying the new value, leave the handle you named holding its old value and still wired to the output, and return `blocksCompiled: 1`, `expressionsCreated: 1`, `consumerRefresh.complete: true`. Every field read as success while the shader was unchanged — on a real terrain that showed up as a fixed frame moving 0.73% against a 0.95% noise floor, and was caught only by a destructive control.

The handle is the point of exposure: `material.decompile_mgir` names every expression `n<first 12 hex digits of MaterialExpressionGuid>`, which persists in the asset. So in a decompile → edit → compile round trip **every** symbol names something that already exists, and `Extend` refuses the whole document.

To change an expression that already exists, pick one:

- **`Append` with the full document** - clears and rebuilds the graph from the text, which is the mode round-trip editing is for. Note it reallocates the expression collection, so read `consumerRefresh` (above).
- **`material.authoring.set_*_parameter_value`** - if the expression is a *parameter* (scalar, vector, texture, static switch), these edit it in place by parameter name.
- **`material.graph.remove_node` + `add_expression` + `connect_nodes`** - replaces the node. Verbose, and you re-make its wires by hand.

**Recompiling an `entry function` block in `Append` keeps its pin identity.** A material function's inputs and outputs are addressed by GUID, and a rebuilt pin is a new object with a new GUID — which would silently disconnect every material already saved against the function, without touching those materials and without any error. The compiler captures pin-name → GUID before the clear and hands each rebuilt pin its previous identity back, so a pin that keeps its name keeps its callers. Renaming a pin in the document *is* a new pin: its callers lose that one wire, and the compile names the orphaned pin in `warnings[]` at the moment the disconnection is created — the only point at which it is visible, since the affected materials are not touched and report nothing until they are next loaded. Full contract: [`material.authoring`](material.authoring.md).

There is deliberately no fourth option: **PinWright has no generic "set a property on an existing material expression" verb.** `blueprint.graph.set_node_property` has no material counterpart, so a plain `Constant3Vector` cannot be re-valued in place by any single call. That gap is what made `Extend` look like the right tool for a job it cannot do.

Adding to an existing graph is unaffected: a document whose symbols are all new compiles in `Extend` as before.

## Round-trip is logical, not visual

MGIR (and BPIR, and the Niagara/AnimGraph IRs) guarantee **logical equivalence** of the graph on round-trip — same nodes, same connections, same property values — plus the nine material-level properties listed above. Visual organisation is out of scope: comment boxes, node colors, node order in the editor list, and pure-layout reroute knots are not preserved. If a decompile→compile→decompile cycle drops your hand-placed comment boxes, that is intentional behaviour; do not file it as a bug.

**That guarantee used to be false of the material itself, in silence.** The decompiler emitted the expression graph and nothing else, and the grammar had no syntax for a material-level property, so a translucent two-sided master decompiled and recompiled came back `BLEND_Opaque` / `TwoSided: false` with `blocksCompiled: 1` and no warning — an `Opacity` pin wired into an opaque material, which the renderer discards. An additive or translucent master is the shape to watch: its blend mode and two-sided flag live on the `UMaterial`, not in the expression graph the decompiler walked, so nothing in the emitted document recorded them. Documents pinned before the `property` opcode shipped carry no property lines, so recompiling one still leaves the target at whatever it already was; re-decompile to pick them up.

**What is not preserved.** Only the nine properties in the table are carried; every other `UMaterial` field is left at whatever the target already holds. That includes the Nanite override and tessellation/displacement settings, the translucency block beyond `TranslucencyLightingMode` (`TranslucencyPass`, self-shadowing, sort priority, `bScreenSpaceReflections`, `bContactShadows`, `bEnableResponsiveAA`), the advanced material flags (`DitheredLODTransition`, `DitherOpacityMask`, `bCastDynamicShadowAsMasked`, `bAllowNegativeEmissiveColor`, `bHasPixelAnimation`), the `bUsedWith*` usage flags, `SubsurfaceProfile`, `MaterialDecalResponse`, refraction mode and depth bias, the physical material and physical material mask, and the Lightmass settings. Compiling into a **fresh** path therefore leaves those at engine defaults, not at the source material's values. Two further limits:

- **Floats are emitted at `SanitizeFloat` precision** (six decimals). A value needing more than that does not survive exactly - true of `OpacityMaskClipValue` and of every numeric expression property.
- **Under Substrate** (`r.Substrate=1`) with `FrontMaterial` connected, `UMaterial::RebuildShadingModelField` recomputes `ShadingModel`, `MaterialDomain` and sometimes `BlendMode` from the graph after the compile. MGIR writes what the document says and the engine may then overwrite it, so those three are engine-derived rather than round-tripped in that configuration.

Root pins `ClearCoat`, `ClearCoatRoughness`, `ShadingModel` and `CustomizedUV0`-`CustomizedUV7` are now accepted by `output` as well as emitted by the decompiler. Previously the decompiler emitted them and the compiler rejected them with `MGIR_INPUT_NOT_FOUND`, so a clear-coat or customized-UV material could not survive its own decompile. `CustomizedUV<n>` writes the input whether or not the pin is exposed - set `NumCustomizedUVs` as well, or the editor does not show it.

Vector-parameter defaults decompile as `DefaultValue: [r, g, b, a]`. That array is accepted directly by `material.compile_mgir`; it recreates the expression's `FLinearColor` default without requiring an object-shaped rewrite or a follow-up imperative setter.

Collection-parameter calls stay human-readable: `material.decompile_mgir` emits `Collection` and `ParameterName`, and `material.compile_mgir` resolves the engine-only `ParameterId` from that pair. A missing collection or a name that does not exist in the named collection is rejected with `INVALID_PARAMS`; it is not accepted as a node that will fail only when the shader translates its branch.

## Material instances are not MGIR

`material.decompile_mgir` refuses a `UMaterialInstanceConstant` with `UNSUPPORTED_ASSET_CLASS`, naming the class and the parent (it used to say `ASSET_NOT_FOUND`, which reads as a bad path for an asset that loads fine). MGIR describes a material **graph**; an instance has no graph of its own, only a parent plus an override set. Decompiling one could only ever emit its parent's graph under the instance's path, and recompiling that would create a second master.

Read an instance with `material.authoring.get_material_instance_info` or `asset.dump`; write its parameters with `material.authoring.set_material_instance_parameters` and its per-instance blend mode / shading model / two-sided / opacity-mask-clip with `material.authoring.set_material_instance_base_property_overrides`. Reach for that last one before adding a second master material: an instance that only needs a different blend mode from its parent does not need one.

## Composites flatten on decompile

Composite material expressions are flattened at decompile in v1 — there is no `subgraph "Name" { ... }` form left in MGIR output, and the older `MGIR_COMPOSITE_NOT_SUPPORTED` compile-side rejection is gone. Composite content is emitted inline in the parent graph, mirroring the BPIR composite-inline prior art. If you have an older `subgraph` form in pinned text, regenerate it via `material.decompile_mgir`.

## String literals and escapes

A double-quoted MGIR literal carries the standard IR escape set — `\\`, `\"`, `\n`, `\r`, `\t`, `\uNNNN` — and nothing else. `material.decompile_mgir` writes it and `material.compile_mgir` reads it, through one encoder/decoder pair, so any literal the decompiler emits recompiles to the same string.

MGIR is line-oriented: an instruction ends at the newline, so a **multi-line value must be written with `\n`**, never as a real line break. That is how a Custom HLSL program, a multi-line `Desc`, or any other multi-line string travels.

Escapes were previously decoded for `\"` and `\\` only, so a decompiled `Code: "a\nb"` recompiled into a node holding a literal backslash-n: a whole HLSL program collapsed onto one line, every `#define` was destroyed, and `compile_mgir` reported `blocksCompiled: 1`. Text authored around that (real spaces instead of `\n`, doubled backslashes) still compiles, but now means exactly what it says.

## Custom HLSL nodes

A `MaterialExpressionCustom` node's input pins are a runtime array, not fixed fields, so **the call's named pin arguments are the declaration**: each one creates an input with that name, in that order, and wires it.

```text
%uv = call `/Script/Engine.MaterialExpressionTextureCoordinate`() @(-900, 0)
%s  = constant Float1(2) @(-900, 200)
%c  = call `/Script/Engine.MaterialExpressionCustom`(
        UV: %uv, Scale: %s,
        Code: "float2 d = UV.xy - 0.5;\nreturn length(d) * Scale;",
        OutputType: "CMOT_Float1") @(-500, 0)
```

The names are the parameter names the generated HLSL function receives, so the node's `Code` addresses `UV` and `Scale` directly. Consequences worth knowing:

- **Names come from the same text that carries the connections,** so the two cannot disagree. There is no separate declaration list: `Inputs: [...]` is refused with `MGIR_INVALID_INPUT_DECLARATION`, and `decompile_mgir` no longer emits it. A pinned document that still carries one is refused rather than half-applied; re-decompile it.
- **A declared input must be wired.** The engine refuses to translate a named Custom input with nothing connected (`Custom material … missing input`), so MGIR has no syntax for one.
- **An unnamed input cannot be expressed and is not a loss.** `UMaterialExpressionCustom::Compile` skips every unnamed input, so its connection never reaches the shader. The decompiler names a legacy unnamed-but-connected pin `Input0`, `Input1`, … and recompiling makes those names real.
- Duplicate or empty input names are refused: both produce an HLSL parameter the node's own code cannot address.

Previously the pins were never created. A document naming any input but `Input` failed its own decompile with `MGIR_INPUT_NOT_FOUND`; one naming `Input` compiled, saved, and produced a Custom function with **no parameters**, so the shader failed on the first identifier the code named with nothing in the response saying so. `blocksCompiled` is not evidence that a material's shader compiles — [`material.authoring.compile_material`](material.authoring.compile_material.md) is what reports that. `material.authoring.add_custom_expression` is the imperative equivalent, taking an explicit `inputs` array.

## TArray-typed `UMaterialExpression` properties

The MGIR decompiler whitelist accepts `FArrayProperty` for reflected expression properties. Custom HLSL multi-output, defines, includes, and DynamicParameter channel-names round-trip; previously these were silently dropped. If a round-trip diff surfaces unexpected array properties on other expression types, that is the whitelist now passing them through correctly — not a regression.

## See also

- [`material.compile-state`](material.compile-state.md) for the `shaderCompile` block, the five statuses, and why a successful MGIR compile is not a shader compile.
- [`asset`](asset.md) for asset dump sidecar registration and diff-baseline behavior.
