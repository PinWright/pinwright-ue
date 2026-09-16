# controlrig

Text-IR compile and decompile for `UControlRigBlueprint` RigVM graphs — CRIR is the RigVM-graph equivalent of BPIR (Blueprint), MGIR (Material), AGIR (Anim Blueprint), and the other text IRs. Use this namespace to author or review a Control Rig as text, bulk-rebind a pin across many rigs, diff a rig change in `git`, or feed a procedurally generated graph into the editor in one call; reach for `call("animation.authoring")` for procedural rig-asset creation and IK setup that is not graph-shaped (`create_control_rig`, `create_ik_rig`, `create_ik_retargeter` live there).

## Sidecar registration

`asset.dump` writes the same decompile output to `crir.txt`; `IrSidecarRegistry` keeps the dump baseline and live `controlrig.decompile_crir` on one decompiler path.

## Scope

CRIR covers the full RigVM execution graph, the per-asset function library, nested collapse/sub-graphs, and the rig-element hierarchy with mutating control authoring:

- **Top-level blocks.** `rig_graph "<ModelName>" { ... }` (one per `URigVMGraph` top-level model, default name `"RigVMModel"`) holds executable graphs; `rig_function "<Name>" { ... }` holds function-library definitions (emitted alphabetically after `rig_graph` blocks); `rig_hierarchy { ... }` holds bones / nulls / controls / sockets.
- **Nested block.** `rig_subgraph "<Name>" { ... }` inside any graph/function/subgraph body materializes the contained graph of a `URigVMCollapseNode` or `URigVMFunctionReferenceNode`.
- **Node opcodes.** Full RigVM coverage: `unit`, `var`, `reroute`, `comment`, `if`, `select`, `enum`, `invoke_entry`, `template`, `dispatch`, `collapse`, `function_ref`, `function_entry`, `function_return`.
- **Hierarchy opcodes.** `bone`, `null`, `control`, `socket` all round-trip. The `control` opcode carries a typed-prefix `type=`/`value=` grammar and an optional `{ ... }` sub-block of `FRigControlSettings` fields (~20 alphabetically-sorted keys, defaults elided).
- **Pin wiring.** `wire_in_<PinName>=%sourceNode.outpin` (sink-side, decompiler emits this form) or `wire_out_<PinName>=%targetNode.inpin` (source-side, also accepted on input).

Out of scope: `curve`, `reference`, `connector`, and physics element kinds inside `rig_hierarchy` (decompiler emits `# TODO unsupported element kind: <ElementType>`). Full grammar reference: `docs/crir-language-reference.md` in the plugin folder.

## `controlrig.compile_crir`

`controlrig.compile_crir` accepts:

- `text` — CRIR document text.
- `context` — target Control Rig Blueprint asset path. Must resolve to a `UControlRigBlueprint`.
- `mode` — `replace` (default) clears every existing node in each named model before adding the CRIR's nodes. `extend` appends without clearing.
- `runLayout` — auto-position nodes that lack `@(x, y)` (default true).
- `save` — mark the asset dirty and save once compile finishes (default false).

Returns `{ mode, assetPath, blocksCompiled, nodesCreated, warnings[] }`; `blocksCompiled` counts consumed top-level blocks (`rig_graph` + `rig_function` + `rig_hierarchy`), and `nodesCreated` counts graph instructions plus hierarchy element creations.

Compilation is atomic: if any hierarchy, function-library, or graph block fails, `compile_crir` restores the target rig's pre-call hierarchy and Blueprint state, tears down nodes created by the failed call, and restores any replace-cleared baseline graph through RigVM undo. Rollback has distinct owners: hierarchy and Blueprint state remain transaction-backed, but RigVM graph/node UObjects are excluded from the editor transaction. Replace-mode graph deletion is one action-backed controller operation so RigVM undo can re-import live nodes instead of relying on UObject transaction snapshots. Editor mirrors are not restored by transaction `PostTransacted`; failure cleanup suspends Control Rig asset and controller notifications, removes failed-attempt nodes, applies and cancels the editor transaction, prunes stale graph entries, releases controller guards, rebinds surviving `URigVMEdGraph` listeners with `InitializeFromAsset`, resumes through the guarded rebuild, and then restores auto-recompile. This guarantee applies to both `replace` and `extend` mode when an editor transactor is present, as in normal `UnrealEditor` and `UnrealEditor-Cmd` hosts. A true commandlet without an editor transactor only gets cancel behavior and cannot restore mutations. Do not manually call `URigVMController::ResendAllNotifications` during rollback: raw replay can re-enter editor recompilation with stale references to removed nodes, and overlapping graph UObject snapshots can corrupt RigVM action undo.

## `controlrig.decompile_crir`

`controlrig.decompile_crir` accepts required `assetPath` (the rig BP path) and returns `{ assetPath, text, warnings[] }`. The same path writes `<ProjectSavedDir>/PinWright/asset-dumps/<asset-path>/crir.txt` when `asset.dump` or `asset.dump_folder` covers a Control Rig, so prefer the dump cache where possible.

`warnings[]` lists any `# TODO unsupported node kind: ...` or `# TODO unsupported element kind: ...` lines emitted into the text, surfaced as structured warnings for tooling.

## Removed legacy stubs

These earlier `animation.authoring.*` stubs were removed when CRIR shipped:

- `animation.authoring.add_control`
- `animation.authoring.add_rig_unit`
- `animation.authoring.connect_rig_elements`

All three were no-op `Ctx.SendSuccess(...)` calls returning "requires manual rig setup"; they never wrote anything. Use `controlrig.compile_crir` for graph mutation. The procedural rig-asset creators (`animation.authoring.create_control_rig`, `animation.authoring.create_ik_rig`) remain.

## Round-trip is logical, not visual

CRIR follows the logical-equivalence contract of MGIR / BPIR / AGIR / PCGIR: two consecutive decompiles and a decompile → compile → decompile round-trip are byte-equal. Comments and node colors are dropped, but node positions are preserved by `@(x, y)` suffixes on `unit` / `var` lines; `runLayout` is therefore a no-op for nodes with explicit positions.

## See also

- [`anim`](anim.md) — AGIR text IR for `UAnimBlueprint` animation graphs (sibling IR, similar shape).
- [`bpir`](bpir.md) — BPIR, the Blueprint text IR that CRIR mirrors; the shared IR conventions are written up there.
- [`animation.authoring`](animation.authoring.md) — the rest of the Control-Rig and IK authoring surface that is not graph-shaped.
- [`asset`](asset.md) — asset-dump sidecar registration and diff-baseline behavior.
- [`material.mgir`](material.mgir.md) — closest prior art for graph-IR conventions, sugar forms, and round-trip semantics.
- `docs/crir-language-reference.md` and `docs/ir-authoring.md` in the plugin folder — full CRIR grammar and opcode reference, and the cross-IR authoring overview. Maintainer documents shipped beside the plugin, not wiki pages.
