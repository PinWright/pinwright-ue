# material

Material asset authoring, material instance parameter editing, and material graph import/export. Use this namespace for top-level MGIR compile/decompile entry points; reach for `call("material.authoring")` for high-level workflows and `call("material.graph")` for raw expression-graph edits.

## How to use

Use `call("material.authoring")` for high-level material/instance work (creation, blend/shading/domain, nodes, typed setters, batch writes, and read-back), and `call("material.graph")` for low-level expression graphs or node types the authoring layer lacks. `call("material.compile_mgir")` and `call("material.decompile_mgir")` are bulk MGIR import/export; see [`material.mgir`](material.mgir.md) for syntax and modes.

Material instance parameters (the runtime tweakable values) go through the typed `material.authoring.set_*_parameter_value` setters or the batch `set_material_instance_parameters`. Parent-side declarations use the `add_*_parameter` variants. Use `clear_parameter_override` to drop a single override and `get_material_instance_info` for read-back.

## Material instance read-back

`material.authoring.get_material_instance_info` returns both instance overrides and the parent parameter metadata used to interpret them. Parameter order metadata must come from `UMaterialInterface::GetParameterSortPriority`; group metadata is separate and is not a substitute for `sortPriority`.

## Cross-cluster overlap

- **Light functions have a gate on the *material* side.** A material assigned as a light's `LightFunctionMaterial` reaches volumetric fog / translucency / single-layer water only through the light function atlas, which rejects most animated graphs. `material.authoring.get_material_info` reports the measured verdict and `material.authoring.set_light_function_atlas_compatible` is the override; the fog-side gate (`r.VolumetricFog`) belongs to `lighting.setup_volumetric_fog`.

## A successful call says nothing about the shader

`blocksCompiled`, `expressionsCreated`, `nodeId` and `"Nodes connected."` describe the **graph** write. A material with malformed Custom HLSL or a wrong `SamplerType` produces identical numbers, writes a valid `.uasset`, saves, reads back with the right domain and wired `mainInputs`, and renders the engine Default Material. Every verb here now publishes a measured `shaderCompile` block; branch on `shaderCompile.status`, and treat `notCompiled` as "nobody asked yet", not as a pass. Full contract, the five statuses and the two ways to force a real verdict: [`material.compile-state`](material.compile-state.md).

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
