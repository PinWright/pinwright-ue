# material

Material asset authoring, material instance parameter editing, and material graph import/export. Use this namespace for top-level MGIR compile/decompile entry points; reach for `call("material.authoring")` for high-level workflows and `call("material.graph")` for raw expression-graph edits.

## How to use

Use `call("material.authoring")` for high-level material/instance work (creation, blend/shading/domain, nodes, typed setters, batch writes, and read-back), and `call("material.graph")` for low-level expression graphs or node types the authoring layer lacks. `call("material.compile_mgir")` and `call("material.decompile_mgir")` are bulk MGIR import/export; see [`material.mgir`](material.mgir.md) for syntax and modes. `call("material.audit")` validates material graphs read-only (islands, missing textures and functions, parameter problems, blend-mode/pin mismatches).

Material instance parameters (the runtime tweakable values) go through the typed `material.authoring.set_*_parameter_value` setters or the batch `set_material_instance_parameters`. Parent-side declarations use the `add_*_parameter` variants. Use `clear_parameter_override` to drop a single override and `get_material_instance_info` for read-back.

## Material instance read-back

`material.authoring.get_material_instance_info` returns both instance overrides and the parent parameter metadata used to interpret them. Parameter order metadata must come from `UMaterialInterface::GetParameterSortPriority`; group metadata is separate and is not a substitute for `sortPriority`.

## Cross-cluster overlap

- **Light functions have a gate on the *material* side.** A material assigned as a light's `LightFunctionMaterial` reaches volumetric fog / translucency / single-layer water only through the light function atlas, which rejects most animated graphs. `material.authoring.get_material_info` reports the measured verdict and `material.authoring.set_light_function_atlas_compatible` is the override; the fog-side gate (`r.VolumetricFog`) belongs to `lighting.setup_volumetric_fog`.

## A successful call says nothing about the shader

`blocksCompiled`, `expressionsCreated`, `nodeId` and `"Nodes connected."` describe the **graph** write. A material with malformed Custom HLSL or a wrong `SamplerType` produces identical numbers, writes a valid `.uasset`, saves, reads back with the right domain and wired `mainInputs`, and renders the engine Default Material. Every verb here now publishes a measured `shaderCompile` block; branch on `shaderCompile.status`, and treat `onDemand` and `notCompiled` as "no full verdict yet", not as a pass. Full contract, the six statuses and the two ways to force a real verdict: [`material.compile-state`](material.compile-state.md).

Workflow gotcha: most authoring methods do not auto-compile. End an edit batch with `call("material.authoring.compile_material", ...)` before reading shader-derived data or packaging.

The same call is also what makes the edit *visible*. A graph mutator changes the asset and stops there; only `compile_material`, `material.compile_mgir` and `configure_layer_blend` push the result into what already renders with the material, which is why a landscape can keep drawing the old shader while every call reports success. The push / no-push list is under [Limitations and reliability notes](material.authoring.md#limitations-and-reliability-notes).

## See also

- [`material.authoring`](material.authoring.md) for the generated high-level authoring methods behind material and material instance workflows.
- [`material.graph`](material.graph.md) for raw expression-graph operations, similar to `call("blueprint.graph")` and `call("niagara.graph")`.
- [`material.mgir`](material.mgir.md) for MGIR bulk text import/export syntax.
- [`material.compile-state`](material.compile-state.md) for the `shaderCompile` block every verb here publishes: what each status means, and how to get a real verdict instead of a probe.
- [`asset`](asset.md) for moving, renaming, and fixing references on materials and material instances.
- [`landscape`](landscape.md) for landscape material assignment.
- [`asset-audit`](asset-audit.md) — repeatable text mirrors of materials for analysis and diffing.

### material.audit

A §18 audit (`Audit/AuditFramework.h`) over the expression graph of each `UMaterial` named in
`assets` or found under `folder`. It writes nothing and has no fix mode: findings carry the `nodeId`
that `material.graph.remove_node` accepts, so a caller removes islands itself.

| check | flags | severity |
| --- | --- | --- |
| `island` | an expression no root reaches | warning |
| `null_texture` | a texture node with no texture and no `TextureObject` input | error if reachable, else warning |
| `null_function` | a `MaterialFunctionCall` with no function | error if reachable, else warning |
| `unused_param` | a parameter node no root reaches | warning |
| `duplicate_param` | one parameter name on several nodes whose types differ (error) or whose defaults differ (warning); identical copies are one shared parameter and pass | error / warning |
| `blend_output_mismatch` | Masked without OpacityMask (error), PostProcess without EmissiveColor (error), Translucent without Opacity (warning), any connected pin the domain / blend mode / shading model ignores (warning, from `UMaterial::IsPropertyActiveInEditor`) | per rule |
| `uv_width` | texture-sample coordinates of a definite width the texture does not take: too narrow (error if reachable), too wide and silently truncated (warning) | per rule |
| `expression_budget` | more than 200 expressions | warning |
| `shader_compile` | off by default; `includeShaderCompile: true` selects it. Blocks on a full compile (`material.compile-state`); failed permutations are errors, no final verdict is unrunnable | error |

**Roots.** Reachability starts at every material-property input (including CustomizedUVs and
FrontMaterial), every `MaterialExpressionCustomOutput` (vertex interpolators, RVT output, landscape
grass / physical-material output), and follows named-reroute usages to their declarations. Composite
scaffolding (the composite node, its pin bases and their reroutes) is never reported.

**Reading the report.** The default `failOn` is `error`, so a material whose only findings are
islands, unused parameters or ignored pins passes; pass `failOn: "any"` to fail on warnings. Each
material lands in exactly one of `clean` / `flagged` / `unrunnable` / `not_applicable` (a non-material
asset such as a material instance), and the per-check rows carry the same buckets.
`blend_output_mismatch` is **unrunnable** on a material that uses material attributes: which properties
are written is decided inside the attributes graph, which this check does not evaluate. Omit it from
`checks` to audit such a material. A path that loads nothing is unrunnable (`MATERIAL_AUDIT_UNLOADABLE`);
an unknown id in `checks` is `AUDIT_UNKNOWN_CHECK`; a folder that matches nothing is `NO_ASSETS_MATCHED`.
More matches than `limit` (default 100) audits the first `limit` by path and reports `truncated: true`,
which fails `pass`.
