# niagara.authoring

Editorial guide to persistent Niagara asset authoring through the generic top-level `niagara.*` read/edit/validate workflow. Use this page for the read-edit-validate sequencing, emitter-ownership rules, and the un-exported NiagaraEditor helper inventory; reach for `call("niagara")` for the full RPC surface and `call("niagara.graph")` for focused graph reads.

## Workflow

1. Read current asset state with `asset.dump` or `niagara.inspect`.
2. Apply one individual `niagara.*` edit RPC.
3. Run `niagara.validate`.

Use `niagara.set_property`, `niagara.set_parameter`, `niagara.add_parameter`, `niagara.remove_parameter`, renderer edits, module/stack edits, and graph pin edits for persistent authoring. Grouped operations, operation arrays, and `niagara.apply_patch` are not supported.

```json
{
  "assetPath": "/Game/FX/NS_Fire",
  "scope": "user",
  "name": "User.Speed",
  "type": "float",
  "defaultValue": 1200.0,
  "compile": true,
  "save": true
}
```

Call this as `niagara.add_parameter` to add one user parameter.

```json
{
  "assetPath": "/Game/FX/NS_Fire",
  "emitter": "Smoke",
  "entryId": "ParticleUpdate:Color",
  "enabled": false,
  "compile": true,
  "save": true
}
```

Call this as `niagara.set_stack_enabled` to enable or disable one stack entry.

Emitter ownership gotcha: emitters are owned assets. Editing an emitter can affect every system that includes that emitter asset; duplicate the emitter first if the change should be local to one effect.

## Module inputs: literal value vs linked parameter

`niagara.set_module_input` accepts two value shapes:

- A **literal** value (`bool` / `number` / vector array or object / color object) writes a hardcoded default into the module's override store. A literal that happens to equal a `User.*` parameter's current value is **not** a live binding — changing the parameter later does not retint/retune the input. Use a literal only when the value is genuinely fixed.
- A **linked parameter** value of the form `{ "link": "User.WispColor" }` (alias `{ "parameter": "..." }`) binds the input to *read from* that parameter, so editing the parameter drives the effect. This is the canonical "expose a tunable" pattern (color/spawn-rate/lifetime/speed driven by `User.*`). The response carries `linked: true, parameter, parameterType`.

```json
{
  "assetPath": "/Game/FX/NS_ArcaneWisp",
  "emitter": "Wisp",
  "entryId": "ParticleSpawn:Color",
  "inputName": "Color",
  "value": { "link": "User.WispColor" },
  "compile": true,
  "save": true
}
```

Validation: a `User.*` parameter must already exist (create it first with `niagara.add_parameter`) and its type must match the input — otherwise the call returns `PARAMETER_NOT_FOUND` or `PARAMETER_TYPE_MISMATCH` (no silent literal fallback). Engine/system-scope reads (`Engine.*`, `System.*`, particle attributes) are linked using the module input's declared type. The NIR decompiler reads a linked input back as `$Namespace.Name`; a literal reads back as its serialised default.

## Engine-helper non-export gotcha (UE 5.3-5.7)

The following NiagaraEditor helpers are *not* `NIAGARAEDITOR_API`-exported on any UE 5.3-5.7 branch and cannot be called from a plugin. The symptom is a clean compile but link errors against `unresolved external symbol`.

| Helper | Header location | Plugin workaround |
|---|---|---|
| `FNiagaraStackGraphUtilities::ResetGraphForOutput` | NiagaraStackGraphUtilities.h:? | Vendored inline in `niagara.add/remove_simulation_stage` handler |
| `FNiagaraEditorUtilities::KillSystemInstances` | NiagaraEditorUtilities.h | Route via `UNiagaraStackViewModel` (used by `niagara.add/remove_event_handler`) |
| `FNiagaraStackGraphUtilities::GetStackFunctionOverrideNode` | NiagaraStackGraphUtilities.h:208 | Inline-walk from `UNiagaraNodeFunctionCall&`: get input-map pin → `LinkedTo[0]->GetOwningNode()`, lazy-resolve `UNiagaraNodeParameterMapSet` class via `FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"))` + `IsA` + `static_cast` (header is in `NiagaraEditor/Private`, not includable) |
| `FNiagaraStackGraphUtilities::RemoveNodesForStackFunctionInputOverridePin` | NiagaraStackGraphUtilities.h (adjacent to above) | Inline chain-walk in `NiagaraResetModuleInput::RemoveOverridePinAndChainedNodes` |

Rule: inline-vendor anything new that needs these helpers. Do not add fresh includes of the source headers expecting the link to resolve. If you need test coverage for an inline helper, move it to a named namespace (e.g. `NiagaraResetModuleInput::`) — anonymous-namespace versions are file-local and unreachable from test code.

## See also

- [`niagara`](niagara.md) for the full edit list, dump schema, override-node internals, and script-swap sequence.
- [`niagara.graph`](niagara.graph.md) for focused graph reads.
- [`asset`](asset.md) for `asset.dump`.
- [`material.authoring`](material.authoring.md) for particle materials.
