# niagara

Niagara VFX asset inspection and authoring: systems, emitters, renderers, parameters, stack/module order, graphs, NIR, and validation. Use this namespace for top-level Niagara reads, edits, and validation; reach for `call("niagara.authoring")` for high-level authoring workflow notes and `call("niagara.graph")` for focused graph reads.

## Workflow

Use a read-first workflow for every persistent Niagara asset edit:

1. Read current state with `asset.dump` or `niagara.inspect`.
2. Apply one individual `niagara.*` edit RPC.
3. Run `niagara.validate` before relying on the asset.

Do not send grouped operation arrays, multi-edit patches, or `niagara.apply_patch`; they are not supported. Each edit RPC performs exactly one operation so validation, transactions, dirtying, compile, and save behavior stay attributable to that operation.

Creation RPCs start blank: `niagara.create_system` creates a `UNiagaraSystem` with no emitter handles, so add an emitter explicitly with `niagara.create_emitter` and `niagara.add_emitter`; `niagara.create_emitter` also has no default stack modules. A fresh system without an emitter renders nothing.

## See also

- [`niagara.nir`](niagara.nir.md) for the NIR text-IR grammar, script-graph body classification, and editor-internal gotchas.
- [`niagara.dump-files`](niagara.dump-files.md) for the current `asset.dump` Niagara sidecar reference, live JSON read separation, asset-registry tags, and op-info structure.
- [`niagara.compile-state`](niagara.compile-state.md) for compile-state honesty, static-switch and stack-module input override storage, and the script-swap reconciliation sequence.
- [`niagara.authoring`](niagara.authoring.md) for the un-exported NiagaraEditor helper inventory and emitter-ownership gotcha.
- [`niagara.graph`](niagara.graph.md) for focused graph reads when the emitter and script usage are already known.
- [`niagara.forces`](niagara.forces.md) for the closed-form equilibrium of the stock `VortexForce` + `PointAttractionForce` + `Drag` stack, and why `AttractionStrength`, `Speed Limit` and `Drag.Ignore Mass` behave the way they do.
- [`asset`](asset.md) for `asset.dump` and the Niagara dump-file schema.
- [`material.authoring`](material.authoring.md) for materials assigned to particle renderers.
- [`sequencer`](sequencer.md) for VFX driven from cinematics.
- [`effect`](effect.md) for runtime preview helpers.

## Individual edit examples

```json
{
  "assetPath": "/Game/FX/NS_Fire",
  "target": { "kind": "renderer", "emitter": "Smoke", "index": 0 },
  "propertyPath": "Material",
  "value": "/Game/FX/M_Smoke",
  "compile": true,
  "save": true
}
```

Call as `niagara.set_property` to set one reflected property on a resolved system, emitter handle, versioned emitter data, renderer, data interface, script, graph node, or module/function-call UObject. See [set_property target kinds](#set_property-target-kinds) for the accepted `target.kind` string values and which property class each owns.

```json
{
  "assetPath": "/Game/FX/NS_Fire",
  "scope": "user",
  "name": "User.Color",
  "type": "LinearColor",
  "value": { "r": 1, "g": 0.2, "b": 0.1, "a": 1 },
  "compile": true,
  "save": true
}
```

Call as `niagara.set_parameter` to set one Niagara parameter value. Use the same read-first workflow before `niagara.add_parameter` or `niagara.remove_parameter`.

## set_property target kinds

`niagara.set_property` (and the other `target`-based edit RPCs) resolves
`target.kind` to one scope; each kind owns a distinct class of reflected
property, so picking the wrong kind yields `UNSUPPORTED_TARGET` or
`PROPERTY_NOT_FOUND`. Accepted `kind` string values (case-insensitive;
snake_case aliases also accepted):

| `kind` | Resolves to | Owns properties such as | Extra `target` fields |
|--------|-------------|-------------------------|-----------------------|
| `system` | `UNiagaraSystem` | `bFixedBounds`, `WarmupTime`, `FixedBounds` | — |
| `emitter` / `emitterHandle` | `FNiagaraEmitterHandle` (handle on the system) | `bIsEnabled` | `emitter` (handle name) |
| `emitterData` | `FVersionedNiagaraEmitterData` | `bLocalSpace`, `SimTarget`, bounds/determinism | `emitter` |
| `renderer` | `UNiagaraRendererProperties` at `index` | `Material`, sort/visibility flags | `emitter`, `index` |
| `parameterStore` | parameter store (use `niagara.set_parameter` instead for values) | — | `emitter`, `scope` |
| `dataInterface` | `UNiagaraDataInterface` | DI-specific properties | `emitter`, `index`/identity |
| `module` (`functionCall`) | `UNiagaraNodeFunctionCall` stack node | node properties | `emitter`, `scriptUsage`, `entryId` |
| `eventHandler` | event-handler entry | event-handler properties | `emitter`, `index`/`entryId` |
| `simulationStage` | simulation-stage entry | sim-stage properties | `emitter`, `index` |
| `node` / `pin` | graph node / pin | node/pin properties | `emitter`, `scriptUsage`, `nodeId`/`pin` |

`emitter`/`emitterHandle` and `emitterData` are the two most-confused scopes:
the **handle** owns `bIsEnabled` (whether the emitter runs in the system); the
**versioned emitter data** owns simulation properties like `bLocalSpace`. A
property on one is not reachable through the other. Module-input values use
`niagara.set_module_input`, and parameter-store values use
`niagara.set_parameter`, not `set_property`.

## Cross-cluster overlap

Runtime preview helpers are separate from asset authoring: `niagara.spawn_actor`, `niagara.modify_parameter`, and `effect.*` affect spawned or live editor/runtime state, while persistent changes belong under individual `niagara.*` edit RPCs.

`niagara.modify_parameter` writes the spawned actor's per-component `OverrideParameters` store, and its result **echoes the override read back off the live component** as `value` (with `overrideStored:true`), so confirm a runtime override from the write result directly — no second call. Do **not** verify with `niagara.inspect`: it reads the **asset's parameter defaults only** (`assetPath`-only, `LoadObject`s the asset), never the per-actor override store, so it will show the unchanged default. If you must read an override you did *not* just set, call `object.call_function` on the actor's `UNiagaraComponent` with `GetVariableFloat` / `GetVariableBool` / `GetVariableVec3` / `GetVariableColor` (name = `User.<name>`) — note the color verb is `GetVariableColor`, **not** `GetVariableLinearColor`.

## Imperative Niagara graph-node creation

New `UNiagaraNode*` subclasses must be authored imperatively (NIR is decompile-only in v1), following the sequence used by `EdGraphSchema_Niagara`:

```
Graph->Modify();
UNiagaraNodeFoo* N = NewObject<UNiagaraNodeFoo>(Graph);
N->NodePosX = X;  N->NodePosY = Y;
N->CreateNewGuid();
// Set payload BEFORE AllocateDefaultPins — pin set depends on it:
//   UNiagaraNodeOp:    N->OpName = ...
//   UNiagaraNodeInput: N->Input = ...; N->Usage = ...
//   UNiagaraNodeStaticSwitch: N->InputParameterName = ...; N->SwitchTypeData = ...
N->PostPlacedNewNode();
if (N->Pins.Num() == 0) N->AllocateDefaultPins();
Graph->AddNode(N, /*bFromUI*/false, /*bSelectNewNode*/false);
Graph->NotifyGraphChanged();
```

Exception: `UNiagaraNodeCustomHlsl` — call `SetCustomHlsl(hlslText)` **after** `AllocateDefaultPins` because it manages pins internally. Nodes without required payload (`UNiagaraNodeReroute`, `UNiagaraNodeIf`, `UNiagaraNodeOutput`, `UNiagaraNodeConvert`) can skip that step.

Class resolution: use `NiagaraEdit::ResolveNiagaraSubclassByPath(UNiagaraNode::StaticClass(), ClassPath, TEXT("NiagaraEditor"))`, which accepts both short names (`"NiagaraNodeOp"`) and fully-qualified paths (`"/Script/NiagaraEditor.NiagaraNodeOp"`). See `niagara.graph.create_node` in `NiagaraGraphHandler.cpp` for a full worked example.

## Renaming a Niagara system parameter (niagara.rename_parameter)

In UE 5.6, `FNiagaraSystemViewModel::RenameParameter` is public in the header but not exported from `NiagaraEditor`, so this plugin must not call it directly. `niagara.rename_parameter` mirrors the reachable parts of that path through exported APIs and keeps the response explicit about which legs ran.

Current v1 scope is same-namespace user-parameter renames on `UNiagaraSystem` assets. Standalone emitter assets and cross-namespace renames are rejected.

The exported path is:

1. Pre-check the exposed-parameter store for `oldName` and reject `newName` collisions before mutation.
2. Wrap mutation in `FScopedTransaction`.
3. Rename `System->GetExposedParameters()` with `FNiagaraParameterStore::RenameParameter`.
4. Rename user metadata with `UNiagaraSystemEditorData::RenameUserScriptVariable` when matching metadata exists.
5. Collect available system, emitter, and event graphs and call `UNiagaraGraph::RenameParameter`.
6. Rename reachable assignment nodes with `UNiagaraNodeAssignment::RenameAssignmentTarget` followed by `RefreshFromExternalChanges`.
7. Call exported `UNiagaraSystem::HandleVariableRenamed(OldVar, NewVar, true)` so engine hooks update renderer, data-interface, simulation-stage, and emitter references that are reachable through the system hook.

The response reports booleans/counts such as `userStore`, `userScriptMetadata`, `graphs`, `assignmentTargets`, and `systemRenameHook`. Do not claim direct SVM delegation, and do not report renderer bindings as `"engine-handled"`; renderer/data-interface/sim-stage coverage is represented only by `systemRenameHook: true` after the exported hook is actually invoked.

## `compile: true` stops live previews for the duration of the compile

Every `niagara.*` edit verb that carries `compile` — and `niagara.compile` itself — destroys the
running system instances of the target asset before requesting the recompile. Without that, an
`ANiagaraActor` playing the system keeps ticking on a worker thread while the scripts are swapped
underneath it, executes the new bytecode against the old data sets, and asserts inside the VectorVM.
That is an `appError` on a background thread, so it kills the editor process outright.

The engine restarts auto-activating components once the compile lands, so a preview spawned with
`niagara.spawn_actor` comes back on its own — you do not need to delete and re-spawn it around an
edit. Editing an emitter asset with `compile: true` quiesces every loaded system that uses that
emitter, not only the one you are looking at.

Edit verbs returning the shared mutation envelope report `quiescedInstances`: how many running
system instances that call stopped, counting the advanced-edit and curve verbs' unconditional
pre-mutation quiesce as well as the compile path's. It is the field to read when a preview viewport
goes blank — including somebody else's, since the sweep takes down the instance backing an open
Niagara toolkit's preview and a component that does not auto-activate will not come back on its own
(`effect.activate_niagara` restarts it). `0` means nothing was playing, not that no sweep ran:
components bound to the asset but idle are swept and not counted.

`niagara.add_emitter`, `niagara.remove_emitter`, `niagara.rename_parameter`, `niagara.compile` and
the `niagara.graph.*` verbs build their own responses and do not carry the field; they quiesce just
the same, as documented in their own sections.

## `niagara.compile {wait:true}` is synchronous, and `compile` + `save` no longer races

`niagara.compile {wait: true}` synchronously pumps Niagara's registered engine compile manager until
the requested CPU-script compile lands or `timeoutSeconds` expires. The budget defaults to 60 seconds
and accepts 0.01-60. On expiry the response is explicit: `completed: false`, `compiled: false`,
`timedOut: true`, `status: "timedOut"`, and `stillCompiling[]` names the affected systems that remain
active. The call still occupies the game thread while pumping, so in a shared editor use
`wait: false` and poll the compact `niagara.compile_status` verb instead. `wait: false` returns after
the request and reports `status: "outstanding", compiled: false` while work remains.

Shared mutation verbs keep `compile: true, save: false` fire-and-forget and report
`compiled: false`, because they did not wait and cannot claim completion. Use `niagara.compile_status`
for the readback. The custom add/remove/refresh emitter verbs do wait and report their observation.

The same wait now runs inside every `niagara.*` edit verb that carries both `compile: true` and
`save: true`, because that pair used to be a data-corrupting footgun: `RequestCompile` is
asynchronous, so the save ran while the compile was in flight, presave serialised 0 compiled
`DataInterfaceInfos` against N resolved ones, and the asset asserted inside the VectorVM on its next
tick — killing the editor process minutes later, from an unrelated caller. The save runs only when
at least one engine compile request was issued and no CPU-script compile remains outstanding when
the bounded pump returns. For emitter edits, the wait covers every loaded system using the emitter,
including one whose current request returned false because an older compile was already active.
Otherwise the save is **refused** and the shared persistence fields distinguish it as
`saveState: "failed"`, with `saved: false`, `pendingFlush: false`, and an actionable `saveDetail`. A
terminal refusal is not flushable; clear the cause named by `saveDetail` and re-issue the operation.

## Every edit verb gates the save on a data-interface check, and says so

Edit verbs returning the shared mutation envelope report `dataInterfaceCheck`, the same three-way
verdict `niagara.add_emitter` publishes: `consistent`, `mismatched`, or `unverified`. It is measured
after the compile wait and before the write, so a compile requested on the same call is what it
reflects.

On `mismatched` the save is **refused** — `saved: false` beside `saveRequested: true` — and
`mismatchedScripts[]` names each offending script in the same shape the rest of the namespace uses
(`scriptPath`, `emitter`, `compiledDataInterfaces`, `resolvedDataInterfaces`). The in-memory edit
still happened, which is why this is a `success: true` response and not an error: recompile
(`niagara.compile`) or repair (`niagara.remove_orphan_data_interfaces`), then save. Persisting the
asset instead is what arms the VectorVM assert that kills the editor on its next tick.

`unverified` is not a pass. It means nothing MEANINGFUL could be compared, and it covers three
states: an edit addressed at a standalone Niagara Emitter asset (the check compares a system's
scripts, and an emitter asset has no system to compare); a system that has no resolved
data-interface set at all; and a system whose every comparable script has had its compiled results
invalidated, so the equality being reported is 0-compiled against 0-resolved on bytecode that was
thrown away. That last one used to publish as `consistent`, which is indistinguishable on the wire
from a system that really was compared. None of the three blocks the save. To get a verdict on an
emitter's effect, run the check on a system that uses it (`niagara.validate`).

**`dataInterfaceDelta` — which call armed it.**

`dataInterfaceCheck` is the state the call ENDS in, and on its own it cannot answer the only
question a caller has: did I just do this? An emitter-scoped edit invalidates that emitter's
compiled scripts unconditionally — value-identically, on first touch, one emitter per call — so
`mismatched` came back on ordinary writes with nothing saying whether it was inherited or created.

Every mutation envelope now also carries `dataInterfaceDelta`:

- `before` — the verdict measured before the mutation ran, or `"unmeasured"` when the verb reached
  the response without passing a pre-mutation seam. `"unmeasured"` is not `"unverified"`.
- `after` — the same value as `dataInterfaceCheck`.
- `changed` — `before` and `after` differ, and `before` was measured.
- `armedByThisWrite` — `before` was a real pass and `after` is `mismatched`. This call is the
  author. False on an inherited mismatch and false on an unmeasured before, because neither is
  evidence of authorship.
- `repair` — `not_needed`, `recompiled` or `failed` (see below), plus `liveInstancesAtRepair` when
  a repair ran.

**A write that arms the mismatch on a LIVE system repairs itself.**

Refusing the save is not enough when something is already ticking the system: the arming step is
in-memory, and the next tick of a live instance is what kills the editor. So when a write is the
one that armed the mismatch (`armedByThisWrite`) **and** the system has live instances, the edit
verb stops those instances and requests a compile, waits for it, and re-measures before returning —
`repair: "recompiled"` when the system came back consistent, `repair: "failed"` when it did not
(the instances are still gone, so nothing ticks it, but the save stays refused).

It recompiles rather than refusing because the resolved data-interface set is rebuilt in exactly one
engine place — `UNiagaraSystem::InitScriptCompiledData`, called only after a compile completes — so
"re-resolve it" and "compile it" are the same operation, and refusing instead would refuse ordinary
parameter writes on any system an actor in the level happens to be playing.

It is narrow by construction: a compile is not free, and it rebuilds the system-scope rapid-iteration
stores (`niagara.compile {wait:true}` merges the authored values back and reports
`rapidIterationPreserved`; a `wait:false` compile and a compile issued from an edit verb's
`compile: true` do not). With nothing live, no repair runs and the
cheap batch order still holds — all emitter-scoped writes, then **one**
`niagara.compile {force:false, wait:true}`, then the system-scope writes, then save.

`niagara.rename_parameter` and the `niagara.graph.*` verbs build their own responses and do not
carry the field; the save gate still applies to them, so a refusal there shows as `saved: false`
with the cause only in the editor log.

## An emitter handle is not a running emitter

A `UNiagaraEmitterHandle` does nothing on its own. The system's SystemSpawn / SystemUpdate
graph must also carry a `UNiagaraNodeEmitter` naming that handle, on the parameter-map chain
feeding the system output nodes — that node is what calls the emitter's spawn and update
scripts. `add_emitter` and `remove_emitter` rebuild those nodes for you and refuse to report
success, or to save, if the result invokes fewer emitters than the system has handles.

This matters because the failure has no other tell. An emitter the system graph does not
invoke compiles clean, passes `validate level:strict` with zero issues, saves, and appears in
`inspect`, `graph.get` and every write echo — and spawns not one particle. Assets authored
before this was fixed are still out there: run `niagara.validate` on one and it now reports
`EMITTER_NOT_IN_SYSTEM_GRAPH` per dead handle.

## A system's emitter is a **child** of the asset, not the asset

`niagara.add_emitter` takes an `emitterPath`, and the system ends up holding a **copy** of that
emitter — the engine has no mode in which a system references an emitter asset directly. What decides
whether later edits to the asset ever reach the system is whether that copy keeps a **parent** link
back to it. `inherit` defaults **true** (the copy carries `Parent` = the asset, as the Niagara
editor's *Add Emitter* produces); `inherit: false` is the editor's *Remove Parent Emitter*, an
unlinked snapshot frozen at the instant you added it.

**Even an inherited copy does not follow the asset on its own.** The merge runs on load, or when
`niagara.refresh_emitter` asks for it — which is why a system could sit on stale content while
`compile` said `completed`, `asset.save` said `saved`, `validate level:"strict"` said
`valid: true, errors: []`, and the `.uasset` grew on every re-save. Effects shipped missing modules
their emitter assets had carried the whole time.

Three reads answer it, and they cannot disagree — they are the same measurement:
`niagara.add_emitter`'s `emitterSource` / `parentEmitterPath`; `niagara.inspect` →
`emitters[].versionedEmitterData.parent` (`inherited`, `path`, `synchronized`); and `niagara.validate`
→ an `EMITTER_PARENT_STALE` warning per stale handle. The old workaround — *add emitters last, or
re-add after every emitter edit* — is retired: read `synchronized`, and `refresh_emitter` when false.

## `entryId` is scoped to one emitter — carry `entryKey` instead

`entryId` looks like a primary key and is not one. It is the raw `UEdGraphNode::NodeGuid`, and
duplicating an emitter copies its graph verbatim — NodeGuids included — so emitters descended
from one template carry **byte-identical** `entryId`s for their corresponding modules. On a
duplicated `SimpleExplosion`, 2 of 36 ids appeared under more than one emitter. Which ids collide
is not predictable from the module.

The module resolver is emitter-scoped, so a bare id only addresses a module together with the
right `emitter`; paired with the wrong one it used to resolve *that* emitter's identically-keyed
module and write it, echoing the id back unchanged.

Every stack entry therefore also carries **`entryKey`**: `"<ownerName>:<entryId>"`, e.g.
`"UpwardMeshBurst:5DDC9AC54A37F08F656EB0AED8F19794"`. Store and replay that, not `entryId`.
Every verb taking `entryId` — `set_module_input`, `reset_module_input`, `set_static_switch`,
`clear_module_overrides`, `remove_module`, `move_module`, `set_module_script`,
`set_stack_enabled` — accepts the qualified form in the same `entryId` field, uses its owner to
select the emitter when `emitter` is omitted, and refuses it against a different `emitter` with
`MODULE_OWNER_MISMATCH`. Bare ids keep working exactly as before.

Mutation responses echo the owner they resolved to as `emitter`, and module verbs echo
`entryKey`, so a wrong-emitter write is visible in the result rather than silent. `entryId` in
the response is still the bare node guid.

## Editing while a Niagara asset editor is open

`niagara.add_emitter`, `niagara.remove_emitter`, `niagara.add_event_handler` and
`niagara.remove_event_handler` refuse with `EDITOR_OPEN` while any asset editor holds the target
asset open, and the message names the open editor and what the edit would have broken. The check
keys off the asset, not off who opened it: in a shared editor process the toolkit is routinely one
another caller left behind. Close it with `editor.close_asset` and retry.

Every other `niagara.*` mutator is deliberately allowed while an editor is open. Those go through
engine mutators that announce the change on a delegate the toolkit already listens to, so the open
stack refreshes itself; refusing them would cost an edit and prevent nothing.

## Short parameter spellings these verbs accept

Six slots accept a second, shorter wire spelling on the same parameter. These are aliases, not
extra parameters: send either name, never both, and the response is identical.

**`name` for `parameterName`** — on `niagara.add_data_interface`, `niagara.remove_data_interface`,
`niagara.set_curve_keys` and `niagara.bind_curve_asset`. All four share one parser, which has
always read `name` as the fallback; only the declaration was missing, so a caller who used the
short form was refused with `UNKNOWN_PARAMS` before the handler ran.

**`index` for `eventHandlerIndex` / `stageIndex`** — on `niagara.remove_event_handler` and
`niagara.remove_simulation_stage` respectively. The long spelling wins if both are sent.

The short spellings stop there. `niagara.add_event_handler` and `niagara.add_simulation_stage` do
**not** take `index` in any spelling — they append, and `add_simulation_stage` takes `atIndex` for
an insertion position. `niagara.add_renderer` likewise appends and takes no `index`; only
`niagara.remove_renderer` and `niagara.move_renderer` address a renderer by ordinal.
`niagara.remove_parameter` takes no `type`: removal matches by name alone, so a type sent with it
would be ignored rather than honoured.

### niagara.search_modules

Searches Niagara module scripts by usage, stage, and keywords. The optional `limit` defaults to 50 and follows PinWright's integer coercion (numeric strings and booleans are accepted; fractional numbers truncate toward zero). The resulting integer must be non-negative. A limit of `0` returns no rows while preserving `totalMatches`; values above 500 are clamped to 500. Negative values are rejected with `INVALID_ARGUMENT` before the Asset Registry is walked.

### niagara.inspect

A full `niagara.inspect` of a real system serializes every aspect (system/emitter/renderer props, the whole parameter set, the stack, all graphs, and the compile summary) and routinely exceeds the ~10k inline display budget, so it spills to a `Saved/.../HttpResponses/*.json` file you then have to Read off disk. To keep a **parameter readback inline**, narrow it instead of dumping everything:

- `parametersOnly: true` returns only the `parameters` aspect (drops system/emitter/renderer props, stack, graphs, and compile) — the bulk of the bytes.
- `parameterName: "SpawnRateScale"` (case-insensitive substring, any store) keeps only matching parameters. Pair the two to read back one value: `{ assetPath, parametersOnly: true, parameterName: "User.SpawnRateScale" }` returns just that parameter's name/type/value.
- If you just wrote the value, prefer confirming from the setter's own echo (`niagara.set_module_input` and `niagara.modify_parameter` both echo what they wrote) over a second inspect — see the notes on those methods.
- Without the narrowing params, `includeStack:false includeGraphs:false includeCompile:false` is the smallest whole-system inspect, but it still emits every parameter and may spill.

The `parameters` aspect reports the value **stored on the script**, which for a module input is not necessarily the only source for that input. Every rapid-iteration entry whose module input also carries a graph override pin is marked `overridden: true` with `override: { valueMode, value, emitter, module, input }` naming that pin, so the `parameters` and `stack` aspects can be read side by side without appearing to contradict each other. The document also carries a one-line `rapidIterationNote` restating that, plus the second trap: an input a static switch has routed around may still publish its unused constant while the variant the switch selected has no constant at all, so a missing name here is not a missing value.

Each `stack.modules[].moduleInputs[]` entry carries, beyond `name` / `type` / `typeInfo` / `enumOptions`:

- `valueMode` — `local` / `linked` / `data` / `objectAsset` / `expression` / `dynamicInput` / `connected` / `default`, with `value`, `linkedParameter` or `dynamicInput` alongside.
- `defaultMode` on a `valueMode: "default"` entry — the module script's `ENiagaraDefaultMode` (`Value`, `Binding`, `Custom`, `FailIfPreviouslyNotSet`), plus `defaultValue` (the same canonical pin-default string `value` uses, so a stated and a defaulted value diff without a type-aware branch) or `defaultBinding` (the bound attribute, e.g. `Particles.Age`). `Value` with no `defaultValue` means the declaration carries no allocated data; nothing is invented for it. **`default` is not "unset and therefore zero"** — most of a stock module's inputs are unwritten, and this is what they actually run with.
- `reachable` — `false` when the input's only consumers in the module script graph are static-switch branch pins the switch does not currently select, i.e. the compiler drops the value as dead code. `gatedBy: { switch, value, requiredValue, branchTaken }` then names the switch (as `staticSwitchInputs[]` spells it), its current value, and the value that would route the input into the graph. Set that switch with `niagara.set_static_switch` before or after writing the input. The walk is deliberately conservative — a consumer that is not a switch branch or a pass-through (reroute / convert) node reads the input unconditionally and the entry stays `reachable: true`.

In the `graphs` aspect, a Parameter Map Get's fallback pins are unnamed inputs; each now carries `defaultForOutputPin` / `defaultForPinId` naming the output parameter it backs. Do not pair them by position — the order is forward on some nodes and reversed on others.

Every `versionedEmitterData` object inspect emits — on a system's `emitters[]` / `emitterHandles[]` and on a standalone emitter asset — carries `parent`: `{ inherited, path, version, synchronized, parentAtLastMergePath }`. `inherited: false` is an unlinked snapshot; `synchronized: false` is an inherited copy the parent asset has moved on from, i.e. this system is running older content than the asset holds. See *A system's emitter is a child of the asset, not the asset* on the namespace page.

On a Niagara System, inspect also carries `componentActivation` (plus `componentCount` and a `components` array) — the same measured placed-component block `niagara.validate` publishes, described under *Is anything in the level running it?* below. It answers "is anything in the open level actually running this" from the verb you already called, with no issue attached: inspect reports, validate judges. `parametersOnly: true` drops it along with every other aspect, since that projection exists to keep a single-parameter readback inline.

### niagara.validate

`level` (default `basic`) controls issue **severity**, not which checks run. Under `level: strict`, three normally-`warning` structural codes are promoted to hard `error` (so the top-level `valid` flips to `false`): `NO_EMITTERS`, `DISABLED_EMITTER`, and `NO_RENDERERS`. At `basic` those same three stay `warning` and `valid` stays `true`. One further code, `NIAGARA_NO_ACTIVE_COMPONENT`, is layered the same way — see *Is anything in the level running it?* below. No other codes change between levels.

Those three fire **by design** on freshly-authored assets, so `strict` on an in-progress system hard-errors on structure rather than on the edit you meant to check:

- `NO_EMITTERS` — a system from `niagara.create_system` starts with no emitter handles, so `strict` validate on it **always** returns a hard `NO_EMITTERS` error until you `niagara.create_emitter` + `niagara.add_emitter`.
- `NO_RENDERERS` — a blank `niagara.create_emitter` emitter ships no renderer, so `strict` validate **always** hard-errors `NO_RENDERERS` on that emitter until you `niagara.add_renderer`.
- `DISABLED_EMITTER` — a disabled emitter handle hard-errors under `strict`.

So when validating an in-progress or deliberately-empty system for *other* issues (e.g. checking for dangling parameter references after `niagara.rename_parameter`), stay on the default `basic` level: a `strict` `NO_EMITTERS`/`NO_RENDERERS` there is inherent to the unfinished asset, not a regression from your edit. Reserve `strict` for a system you expect to be render-complete (emitters added, each with a renderer, none disabled).

Severity layering: the top-level `errors` / `warnings` / `issues` arrays apply this `level` normalization and are **authoritative**. The nested `compile.issues` block carries the raw pre-normalization dumper severity, so the same code can read `warning` inside `compile.issues` while the top-level reports it as `error` under `strict` — trust the top-level `errors` / `valid`, not the nested `compile.issues` severity.

`EMITTER_NOT_IN_SYSTEM_GRAPH` is an **error at every level**, not one of the three codes
`level: strict` escalates. An emitter handle the system graph never invokes cannot run under
any reading of the asset, so there is no level at which it is a warning. One issue is raised
per dead handle, named by emitter; a single issue with the same code is raised instead when the
system's graph could not be read at all, so "no verdict" never comes back looking like "clean".

`EMITTER_PARENT_STALE` is a **warning at every level**, one per handle, naming the handle and the
parent asset. It means the handle inherits from an emitter asset that has changed since the system
last merged from it, so the system is running older content than the asset now holds. Not an error:
the system is runnable and its content is a deliberate earlier state of the parent until someone
asks for the newer one — `niagara.refresh_emitter` merges it in without discarding the system's own
per-handle overrides. A **snapshot** handle (no parent at all) is deliberately silent here: it is not
stale, it is unlinked. `niagara.inspect` → `emitters[].versionedEmitterData.parent` is where that
distinction is readable.

On a Niagara System, validate also runs the compiled-vs-resolved data-interface check and publishes
its verdict as `dataInterfaceCheck`: `consistent`, `mismatched` or `unverified`. A system whose
compiled data-interface count disagrees with its resolved one asserts inside the VectorVM on a
worker thread the next time anything ticks it, which is an appError that kills the editor process —
minutes after, and unrelated to, whatever wrote the asset. Every mutating `niagara.*` verb refuses
to save a system in that state, but one that got there before those gates shipped is still on disk.

- `mismatched` — one `NIAGARA_DATA_INTERFACE_MISMATCH` **error** per offending script, naming the
  script and both counts, at `basic` as well as `strict`. Like `EMITTER_NOT_IN_SYSTEM_GRAPH` this is
  not one of the three codes `level: strict` escalates: a system that cannot be ticked is not
  acceptable under any reading of the asset. The way out is
  `niagara.list_orphan_data_interfaces` to see the resolved entries the bytecode no longer
  references and `niagara.remove_orphan_data_interfaces` to drop them, or a recompile.
- `unverified` — one `NIAGARA_DATA_INTERFACE_UNVERIFIED` **warning**. Nothing could be compared,
  normally because the system has not been compiled in this session, so `valid` does not cover this
  invariant and an empty mismatch list is not evidence there are none. It is a warning rather than
  an error because having no resolved set is the ordinary state of an asset nothing has ticked yet.
  Run `niagara.compile` and validate again to get a verdict.

The field is absent on Niagara Emitter and Niagara Script assets — the check is system-level, so a
verdict there would be invented rather than measured.

#### Did its scripts compile?

`valid` is the conjunction of every check on this page, and one of them is the last compile status
of **every** script the asset owns — system spawn/update, each emitter's emitter and particle
spawn/update, plus every event-handler, simulation-stage and (on a GPU emitter) GPU compute script.
The per-script verdict is `compile.scripts[]`, whose entries carry `ownerKind` / `ownerName` /
`scriptUsage` / `compileStatus`, and `compileErrors` when the compile failed.

- `scriptCompileCheck: "passed"` — every script reported a terminal successful status.
- `scriptCompileCheck: "failed"` — at least one script is at `NCS_Error`. One
  `NIAGARA_SCRIPT_COMPILE_ERROR` **error** per failing script, at `basic` as well as `strict`, naming
  `scriptUsage` / `scriptPath` and carrying the compiler's own messages in `compileErrors`. Like
  `EMITTER_NOT_IN_SYSTEM_GRAPH` this is not one of the three codes `level: strict` escalates: the
  engine refuses to instance a system with a failed script, so it renders nothing under any reading
  of the asset. This is the state that used to validate `valid: true` with an empty `errors` array
  while `compile.valid` was `false` and ten particle scripts were at `NCS_Error` — two shipped
  systems that then returned `active: false` from `effect.activate_niagara`.
- `scriptCompileCheck: "unverified"` — at least one script did not report a terminal status, i.e. all
  or part of the asset has not compiled in this session (the ordinary post-load state under
  `fx.Niagara.OnDemandCompile`) or is `NCS_Dirty`. Known-good sibling scripts do not turn an unknown
  or stale script into a pass.
  **Not a pass.** No separate issue is raised: the nested block already reports it as
  `COMPILE_STATE_UNINITIALIZED`. Run `niagara.compile` and validate again to get a verdict.

Only `NCS_Error` fails the verdict. The two `…WithWarnings` statuses are terminal successes.
`NCS_Dirty` (edited since the last compile) may still have runnable cached bytecode, but it is
**unverified**, not passed, because that bytecode does not prove the current authored graph compiled.

On a Niagara System the result also carries `pendingCompile`. When it is `true` a compile is still
in flight, every status in the response describes the **previous** compile, and one
`NIAGARA_COMPILE_PENDING` **error** is raised at both levels so `valid` is `false` rather than a
verdict on an asset that no longer exists in the form measured. Validate does not wait it out — use
`niagara.compile_status` to poll without blocking, or `niagara.compile {wait: true}` for Niagara's
synchronous bounded manager-pump wait. The field is absent on Niagara Emitter and Niagara Script assets:
the compile queue is a system-level property, so a verdict there would be invented rather than
measured.

`compile.valid` is **not** promoted into the top-level verdict, and is not the field to branch on.
It is `UNiagaraSystem::IsValid()`, which is equally `false` for a system with no emitter handles and
for one whose scripts failed; promoting it would turn the `basic`-level `NO_EMITTERS` warning into
an unconditional error. Both states it conflates are reported separately — `NO_EMITTERS` above,
`NIAGARA_SCRIPT_COMPILE_ERROR` here.

#### Is anything in the level running it?

Every check above describes the **asset**. Whether anything in a level actually runs it is a property
of the placed `UNiagaraComponent`, so a perfectly healthy system whose placed components are all
inactive used to validate `valid: true` with an empty `errors` *and* an empty `warnings` array while
the level rendered nothing — three shipped systems stayed dark for a day that way, and the only
signal that disagreed was a raw reflected `object.call_function IsActive`.

On a Niagara System, validate now surveys the **open editor world** (never the PIE world, never the
asset editor's preview) for placed components bound to the system and publishes
`componentActivation`:

- `active` — at least one placed component is running it. Field only.
- `no_components` — the open level's loaded actors carry none. Field only, **no issue**: an unloaded
  World Partition cell, or simply not having the level open, is indistinguishable from there being
  none, so "found none" is never reported as a finding.
- `none_active` — placed components exist and not one is active. One `NIAGARA_NO_ACTIVE_COMPONENT`
  **warning** at `basic`, **error** at `strict` — the same layering the three structural codes use,
  because a system nothing activates is legitimate whenever gameplay spawns it, while `strict`
  already means "I expect this asset to be render-complete".
- `unverified` — there is no editor world to read. One `NIAGARA_COMPONENT_ACTIVATION_UNVERIFIED`
  **warning** at both levels, and **no** `componentCount` / `components`: publishing zero there would
  read as "the level places none".

For the three measured verdicts the result also carries `componentCount` and a `components` array of
`{actor, component, componentPath, isActive, autoActivate}`. The array is capped at 16 entries —
compare its length against `componentCount` to see whether it was truncated. `componentPath` is in
the shape `actor.set_component_properties` and `object.call_function` take, so the remedy is direct:
restore `bAutoActivate` (a CDO default, so it is only serialised into the `.umap` when something
overrode it) or run `effect.activate_niagara` on the actor.

`niagara.inspect` publishes the identical block on a Niagara System (except under
`parametersOnly`), so the question can be answered from either verb; only validate turns the
verdict into an issue.

#### Does a SubUV renderer's frame grid match its atlas?

`SubImageSize` on a sprite or mesh renderer declares the grid the vertex factory slices the
assigned texture into. Nothing else in this response — or in `niagara.inspect`, which reports the
property and the material without joining them — compares that number against the texture actually
sampled, so a renderer set to `8 x 8` over a `6 x 6` atlas used to be reported healthy by every
read there is. The failure is total and silent: `NiagaraSpriteVertexFactory.ush` remaps `TexCoord0`
into `(SubImageCol + u, SubImageRow + v) * SubImageSize.zw` unconditionally, so every sprite samples
a window that straddles cell boundaries and shows inter-cell gutter with a clipped fragment shoved
against one edge, never a centred frame — and the `SubUVAnimation` module compounds it by taking its
frame count from the same wrong property.

On a Niagara System, validate checks every **enabled** renderer of every **enabled** emitter handle
whose `SubImageSize` is other than `(1,1)`, and publishes `subUVAtlasCheck`:

- `not_applicable` — no renderer declares a grid. Field only.
- `consistent` — every declared grid was checked against the texture it slices and nothing
  disagreed.
- `mismatched` — at least one disagrees. One `NIAGARA_SUBUV_ATLAS_SIZE_MISMATCH` per renderer,
  naming the emitter, `rendererIndex`, `texturePath`, `declaredColumns`/`declaredRows`,
  `detectedColumns`/`detectedRows`, `populatedColumns`/`populatedRows` and `emptyTileCount`.
- `unverified` — at least one renderer's texture could not be resolved or read. One
  `NIAGARA_SUBUV_ATLAS_SIZE_INDETERMINATE` **warning** per renderer, carrying the reason. Never
  silence: staying quiet there would read as "checked and fine".

Reported at **both levels**, not as a strict-only escalation: a grid that does not match its atlas
renders wrong under any reading of the asset. The two codes are **not** exclusive — a renderer whose
name disagrees *and* whose pixels could not be read emits both, because dropping the second would
report a heuristic finding as if the atlas had been measured.

**Severity tracks the evidence, not the fault.** `NIAGARA_SUBUV_ATLAS_SIZE_MISMATCH` is an **error**
only when the atlas's own cell period was measured and disagrees; every other signal raises it as a
**warning**, and the message says which produced it. The four signals, weakest first:

- **Divisibility** (warning) — the source dimensions are not whole multiples of the declared grid,
  so no cell lines up with whole texels. Weak: `1024 / 8 = 128` passes it, which is why it alone
  would not have caught the case this check was written for.
- **Name** (warning) — an `NxM` spelled in the texture's asset name (`T_Smoke_8x8`) that disagrees
  with the declared grid. Only the last such run in the name counts, both sides must be 1-2 digits
  in 1-64, and the run must not be part of a longer number, so a `1024x1024` reads as a pixel size
  and is ignored.
- **Trailing-empty cells** (warning) — the atlas is read through the same tile-grid pass
  `texture.get_pixel_stats` exposes, at the *declared* `columns x rows`, and the declared grid's
  wholly-empty trailing columns and rows are dropped; `populatedColumns`/`populatedRows` is what is
  left. A cell counts as empty only when it carries no pixel content at all (peak channel at or
  below 2/255, or fully transparent), so dim frames are not mistaken for blank ones. **This never
  proves a different cell size, and it over-reports in a known direction:** an 8x8 sheet holding 56
  authored frames leaves its last row blank by design, and an alpha-faded flipbook's final row can
  be fully transparent. Both are sound content, and both trip this signal — which is why it is a
  warning and not an error.
- **Cell period** (error) — two 1-D energy profiles, one entry per texel column and one per texel
  row of the source mip (mean luminance scaled by mean alpha, so an additive sheet and an
  alpha-masked one both read). Each candidate division is scored by the *brightest* sample landing
  on any of its interior cut lines: a real grid puts every cut in a gutter, a wrong one drives at
  least one cut through a frame. The largest division whose cuts are all at or below 10 % of the
  profile mean — and whose drawn slices form a contiguous prefix of at least two — is the answer,
  reported as `detectedColumns`/`detectedRows`. Taking the largest matters: a divisor of the true
  grid also lands its cuts in real gutters, so the smallest passing division would read 2 for every
  atlas. Trailing blank slices are allowed on purpose, so a sheet holding fewer frames than it has
  cells still reads at its real grid rather than at one of its divisors. This is the signal that
  catches a **fully packed** sheet at the wrong grid, which every other signal here is blind to.

**What the period signal does not catch:** an atlas whose frames are drawn edge to edge, with no
inter-cell gutter at all, has no period to read — `detectedColumns` comes back 0 and the reason is
recorded. A `consistent` verdict therefore means "nothing measurable disagrees", not "the grid was
proven correct". A mip wider or taller than 4096 texels is sub-sampled into 4096 profile strips,
which widens the effective gutter test rather than breaking it.

**Which texture is checked**, in resolution order: the renderer's own
`MaterialParameters.TextureParameters` entry when there is exactly one (a MID is built from it per
emitter instance, so it overrides the material asset); otherwise the renderer's sole material is
read and its graph walked — including material functions — for a `ParticleSubUV` /
`TextureSampleParameterSubUV` node, falling back to the material's sole sampled texture when there
is exactly one. Anything ambiguous (several materials outside a running instance, several SubUV
nodes, several samplers and no SubUV node) is `unverified` rather than a guess: a wrong texture
produces a confident finding about the wrong asset. When the sole SubUV sampler is a *parameter*, a
renderer binding on the same name or a material-instance override is resolved before measuring.

Textures with no editable source data, or a compressed / float source layout, are `unverified` —
the measurement reads `FTextureSource`, the same access `texture.get_pixel_stats` uses. The field
is absent on Niagara Emitter and Niagara Script assets: the walk is over a system's emitter handles.

### niagara.set_module_input

`set_module_input` writes a **graph override pin** on the module's stack node, and — on the literal path — reconciles the rapid-iteration constant that shadows the same input when one already exists. Verify it as follows:

- The result reports the value read back from the override pin as `value` (the canonical pin-default string, e.g. `"160.0"`) on the literal path, alongside `pinId`/`index`/`linked`. For typed Quat writes the handler encodes through Niagara's schema, decodes the actual pin with the schema, and verifies every component before reporting success; do not treat raw text equality as typed validation. (The linked path — `value: { link: "User.X" }` — reports `linked: true`/`parameter`/`parameterType` instead.)
- Matrix and quaternion literals use the placed input's declared Niagara type. A `NiagaraMatrix` takes exactly 16 finite numbers in row-major order (`[m00,m01,m02,m03, ... m30,m31,m32,m33]`); a `Quat4f` takes exactly four finite numbers as `[x,y,z,w]` or `{x,y,z,w}` and is normalized before it is written. Wrong arity, non-numeric components, and a zero quaternion are refused with `INVALID_VALUE` before an override is created. In UE 5.8 the Matrix type utility has no schema pin-default encoder/decoder, so Matrix literals are deterministically refused with `INVALID_INPUT_VALUE` before graph mutation; the handler must not accept a truncated or identity-decoded Matrix. Quat writes use the schema codec and echo the decoded canonical pin-default string.
- `inputName` is the **bare top-level name** published by `niagara.inspect {includeStack:true}` at `stack.modules[].moduleInputs[].name`. The handler resolves that name from the placed module stack (`GetStackFunctionInputs`); it does not address a nested value by concatenating dotted path segments. An unknown name, including a dotted sub-input such as `SpawnRate.NotARealInput`, is rejected before the override write with `MODULE_INPUT_NOT_FOUND`, and the error lists the module's available stack inputs.
- Linked values use the declared type of that placed stack input. `User.*` links must resolve an existing user parameter and type-match it; engine/system/particle attributes such as `Particles.NormalizedAge` use the module input's declared type and are wired as links. Failures are reported as `PARAMETER_NOT_FOUND` or `PARAMETER_TYPE_MISMATCH`, not as a literal fallback.
- **A literal cannot be written over an override pin that already has an inbound link**, and the verb refuses rather than pretending. An override pin driven by a dynamic input, a parameter binding (`User.*`, `Engine.*`) or a data interface is not a value slot — the graph evaluates the link and never reads the pin's default — so the write is rejected with `MODULE_INPUT_OVERRIDE_LINKED`, naming the `valueMode` and the driving parameter or dynamic-input script. To replace the link with a literal, pass `breakExistingLink: true`: it deletes the link and its now-orphaned upstream chain first, and the response adds `replacedOverride: { valueMode, source }` naming what the literal displaced. Screen a pin before writing with `niagara.inspect {includeStack:true}` → `stack.modules[].moduleInputs[].valueMode` (`local` is a plain literal; anything else is a link).
- **A `link` or `dynamicInput` write replaces an existing override without asking — and says so.** Only the literal path refuses (bullet above): a literal over a link would never be read, so refusing costs nothing, whereas assigning a parameter binding or a dynamic input takes effect exactly as asked — gating it would refuse the ordinary re-bind (`User.A` → `User.B`) and the idempotent retry of a verb named *set*. Both therefore proceed and delete whatever the pin carried — an artist's dynamic-input chain, another agent's binding, a literal — and both report it as `replacedOverride: { valueMode, source, value }`: `source` names the displaced linked parameter or dynamic-input script, `value` carries the displaced literal's pin default (a literal has no source, and that text is what you would restore), and the whole object is absent when the input had no override pin at all. The classification is the same walk `moduleInputs[].valueMode` uses, so the write's account and a later `niagara.inspect` cannot disagree. Screen with `niagara.inspect {includeStack:true}` first if you need to know what you are about to displace — the report arrives after the chain is already gone, and only an editor undo brings it back.
- **A literal write also reconciles the rapid-iteration constant, and reports what it found.** A module input's value can live in two places: the override pin this verb writes, and the constant `Constants.<Emitter>.<Module>.<Input>` a compile generates, which is what the compiled simulation reads. Writing only the pin leaves an input whose constant already existed running the constant's old value with nothing saying so. The literal path therefore pushes the same bytes into every rapid-iteration store that already holds that constant — the Niagara editor's own behaviour, which writes the constant on every affected script — and the result carries `rapidIteration: { parameter, shadowed, action: "updated", previousValue, updatedScopes }`. `shadowed: false` means no constant existed and the pin is the only source (the state a freshly added module's overridden inputs are in). Constants are never created here: an input that had none keeps none. `niagara.reset_module_input` and `niagara.clear_module_overrides` return the same object with `action: "removed"` — undoing the pin without the constant would leave the runtime on the last written value.
- Reading the input back from `niagara.inspect`'s `parameters` aspect is no longer a trap, but it is still not the whole picture: that aspect reports the value **stored on the script**, and an entry whose module input also carries an override pin is marked `overridden` with an `override` object naming that pin's mode and value. For the live override value read the `graphs` aspect's override pin (`defaultValue`) — and only a pin with no inbound link carries `defaultValue` at all. A linked pin reports `rawDefaultValue` plus a `defaultValueError` naming `MODULE_INPUT_OVERRIDE_LINKED` instead, because the link governs and the stored literal is never read; the effective value is the driving source, which `stack.modules[].moduleInputs[].valueMode` names.
- **A correct write can still be dead code, and the response says so.** The result carries `reachable`; when it is `false` it also carries `gatedBy: { switch, value, requiredValue, branchTaken }`. That is the case where the input name is right, the override pin is the right one, and the value lands where it belongs — but the input's only consumer in the module script graph is a static-switch branch the switch does not take, so the compiler drops it. The write is kept (staging a value before flipping the switch is legitimate), so act on it with `niagara.set_static_switch` on the named switch. The same two fields appear on `niagara.inspect`'s `moduleInputs[]` entry, so a later readback cannot report the dead override as configuration either.
- For graph-level diagnosis, `niagara.inspect {includeGraphs:true}` already distinguishes the two mechanisms: a rapid-iteration `UNiagaraNodeInput` reports `inputUsage: "RapidIterationParameter"`, while a module override is a pin on `NiagaraNodeParameterMapSet`. The graph serializer needs no extra marker; use those existing fields when checking whether a write reached a live override.

### niagara.add_emitter

**The system takes a child of the emitter asset, not a reference to it — and by default that child
keeps its parent link.** The full contract, the three ways to read the relationship back, and why
this used to be invisible are on the namespace page under *A system's emitter is a child of the
asset, not the asset*. In brief:

- `inherit` defaults **true**: the copy carries `Parent` = `emitterPath`, and later edits to the
  asset are merged in by `niagara.refresh_emitter` (they do **not** arrive on their own).
  The response reports `emitterSource: "inherited"` and `parentEmitterPath`.
- `inherit: false` takes an unlinked snapshot instead — the editor's *Remove Parent Emitter*.
  `emitterSource: "snapshot"`, no `parentEmitterPath`. Frozen at add time, permanently.

**`EMITTER_NOT_INHERITABLE` — the default refused because the source asset declines to be a
parent.** `UNiagaraEmitter::bIsInheritable` is false on it, which is how every **stock Niagara
template and behaviour-example emitter** ships, and `asset.duplicate` preserves it. The engine
strips the parent link off the copy in that case, so honouring `inherit: true` was impossible; the
handle is rolled back out and nothing is written. Two ways forward, and the error names both: set
`bIsInheritable` on the emitter asset and retry (the editor's own emitter wizard does exactly that
to any asset it creates from a template), or pass `inherit: false` and accept the snapshot
knowingly. `niagara.create_emitter` produces an inheritable asset, so this does not arise on
emitters made through it.

Two guards run on this verb, and each refuses rather than leaving the editor in a state that dies
later. Both also apply to `niagara.remove_emitter`.

**`EDITOR_OPEN` — refused while any asset editor holds `systemPath` open.** `AddEmitterHandle`
reshapes the system's emitter-handle array; an open `FNiagaraSystemToolkit`'s widgets still point at
the old handles, and the next ordinary Slate redraw dereferences freed memory and takes the whole
editor process down (`SNiagaraOverviewGraphTitleBar::IsUsingDeprecatedEmitter` →
`UNiagaraEmitter::GetEmitterData`, `EXCEPTION_ACCESS_VIOLATION`). The fault lands in a **redraw**, so
before the guard the call itself returned `{emitterCount, compiled}` and nothing in its response could
ever report the crash.

The check keys off the **asset**, not off who opened the editor. In a shared editor process the
toolkit that kills your write is routinely one somebody else left open — including the one
`render.capture_asset_preview {closeAfterCapture: false}` leaves behind, which makes the documented
workaround for that verb the trigger for this one. Close it first (`editor.close_asset`) and retry;
the error names the open editor.

To prove a placed system without ever opening an asset editor, use `effect.step_and_capture` for
one exact instant; it composes activation, settled exposure, frozen-world advancement and the
level capture into one call. For manual multi-call debugging, the lower-level path remains
`niagara.spawn_actor` + `effect.activate_niagara` + `effect.advance_simulation` +
`render.capture_open_level`.

**The stock `SimpleExplosion` is an early-burst fixture, not a 0.2-second-lifetime guarantee.**
The `/Niagara/DefaultAssets/Templates/Systems/SimpleExplosion.SimpleExplosion` system can finish
by a 0.2-second manual sample, so tests that need active effect pixels sample it at 0.05 seconds
and assert `UNiagaraComponent::IsActive()` before checking images. A nonblank whole frame can be
viewport background rather than effect output; compare against an inactive baseline and require
an effect-specific pixel change. After the structural checks, the behavior test enumerates every
enabled emitter renderer material and probes each `MaterialShaderState` under one shared deadline.
An empty material set is a failure; a non-`completed` shader map skips only pixel assertions with
reason `niagara renderer material shader map not compiled on this host`. Headless/RHI presence alone
is not a skip condition: `simulatedSeconds` and `IsActive()` remain required.

**`dataInterfaceCheck` — a post-write data-interface check gates the save and the success response:**

- `"consistent"` — every script that has a resolved data-interface set agrees with its compiled one.
- `"unverified"` — nothing could be compared (the system has not resolved its data interfaces in
  this session). **This is not a pass.** It is the normal verdict for `compile: false`.
- The verb never returns success with `"mismatched"`. A mismatch is refused with
  `NIAGARA_DATA_INTERFACE_MISMATCH`, the asset is **not** saved, and the error payload carries
  `mismatchedScripts[]` (`scriptPath`, `emitter`, `compiledDataInterfaces`, `resolvedDataInterfaces`)
  plus the mutation facts, so you still learn what landed in memory.

Why it refuses instead of warning: a system whose compiled data-interface count differs from its
resolved count asserts inside the VectorVM (`DataSetIdx < ExecCtx->DataSets.Num()`) on a concurrent
worker the next time *anything* ticks it — an `appError` that kills the shared editor. The engine's
own mitigation is a `LogNiagara` Warning plus `InvalidateCompileResults` during presave, and it is
not enough: the system keeps ticking. The fault is latent, so the caller that detonates it (often an
unrelated `sequencer.set_playhead`) is not the one that caused it.

**The usual cause is passing `compile: true` and `save: true` on the same call.** The compile is
asynchronous, so the save persists an invalidated compile. Separating them —
`niagara.compile {force, wait}`, then `asset.save` — took one system from 44 mismatch warnings to
zero. On a refusal, compile the system and retry. If it still refuses, the resolved set carries data
interfaces the compiled scripts no longer reference — typically template leftovers from an emitter
duplicated from a stock template. Name them with `niagara.list_orphan_data_interfaces` and drop them
with `niagara.remove_orphan_data_interfaces`; both are documented below.

**The handle is only half the write — the system graph is rebuilt too.** Adding a
`UNiagaraEmitterHandle` does not make an emitter run; the system's SystemSpawn/SystemUpdate graph
must carry a `UNiagaraNodeEmitter` naming it. This verb rebuilds those nodes and then reads the graph
back before it compiles or saves anything. Two response fields are measurements off that read, not
echoes of the request:

- `emittersInvokedBySystemGraph` — how many of the system's handles the SystemSpawn and SystemUpdate
  chains actually reach. On success it equals `emitterCount`; compare them if you want the check
  without relying on the verb to enforce it.
- `emitterNodesRebuilt` — nodes created, `2 x emitterCount` (a spawn and an update node per handle),
  because the whole set is rebuilt rather than appended to. `-1` means the system graph could not be
  resolved and nothing was written.

`NIAGARA_EMITTER_NOT_IN_SYSTEM_GRAPH` means the rebuild could not place the nodes — normally a system
whose SystemSpawn/SystemUpdate output nodes are missing. The handle is in memory and the package is
dirty, but nothing was saved; the error payload carries `uninvokedEmitters` and `systemGraphReadable`.
Recovery is to rebuild the system from a complete stock system with `asset.duplicate`, not to retry.

Because the rebuild covers every handle, one `add_emitter` (or `remove_emitter`) on a system authored
before this shipped re-wires its existing emitters too — so it is also the repair route for an asset
whose emitters never ran.

**Both emitter verbs stop live previews before they touch the handle array — including `compile: false`.**
A running `FNiagaraEmitterInstance` caches its *position* in the handle list and reads the handle back
through that index unchecked, so appending (which can reallocate the array) is exposed exactly like
removing (which shifts the indices). Every `UNiagaraComponent` playing the system has its instance
destroyed first, matching what the Niagara editor itself does on both paths. Practical consequence: a
preview or level component playing `systemPath` stops when you add or remove an emitter, even on a call
that compiles nothing — re-activate it afterwards (`effect.activate_niagara`).

### niagara.remove_emitter

Same `EDITOR_OPEN` guard and the same `dataInterfaceCheck` /
`NIAGARA_DATA_INTERFACE_MISMATCH` contract as `niagara.add_emitter` above:
`RemoveEmitterHandlesById` reshapes the emitter-handle array a live toolkit is still holding, and the
same latent data-interface mismatch is possible on the way out. Close the asset before removing a
handle. The pre-mutation quiesce described under `add_emitter` applies here identically.

Removing a handle also rebuilds the system graph's emitter nodes, so the removed handle leaves no node
naming a dead id behind. The response carries the same measured `emittersInvokedBySystemGraph` /
`emitterNodesRebuilt` pair as `add_emitter`, and the same pre-save refusal.

### niagara.refresh_emitter

Merges changes made to a **parent emitter asset** into the system handles that inherit from it. This
is the verb that closes the loop `niagara.add_emitter` opens: the system holds a child of the asset,
and nothing carries a later edit of the asset across until something asks for the merge.

`emitter` (a handle Guid or name) is optional — omit it to refresh every inherited handle in the
system, which is the usual unit of work after finishing a set of emitters.

**It merges, it does not re-add, and that difference is the point.** `remove_emitter` +
`add_emitter` also picks up the parent's current content, but it discards every edit made to the
system's own copy of that emitter — the per-handle spawn counts, sprite sizes and radii an author
sets in the system rather than in the asset. `refresh_emitter` applies the parent's diff on top of
those overrides, which is what the Niagara editor itself does when it loads a system whose parent
has moved on. A caller who hits staleness mid-iteration has a non-destructive route.

Response, all measured rather than echoed:

- `refreshed[]` — one entry per inherited handle: `emitter`, `handleId`, `parentEmitterPath`,
  `wasStale` (read **before** the merge, so you can tell a refresh that did something from one that
  had nothing to do), `synchronizedWithParent` (read **after**, the same value `niagara.inspect` and
  `niagara.validate` report), `graphModified`, `merged`, and `errors[]` when a merge failed.
- `skipped[]` — handles with no parent, each with `reason: "no_parent"`. A snapshot handle is not
  stale, it is unlinked; there is nothing to merge from.
- `mergesApplied` / `mergesFailed`. A run with any failure is **not saved**, even with `save: true`.

`EMITTER_NOT_INHERITED` means nothing in the system (or nothing under the named handle) has a parent
at all — an error rather than a zero-item success, because the caller asked for work that cannot
happen. Read `niagara.inspect` → `emitters[].versionedEmitterData.parent` to see which handles are
inherited.

When `emitter` names a snapshot, the refusal also carries the resolved `emitter` and `handleId` in
top-level error data and names that handle in the message. A snapshot cannot be re-linked
non-destructively because the original source identity is absent. Its migration path is destructive:
preserve or manually reconcile the system-copy overrides first, ensure the source emitter asset is
inheritable, then use `niagara.remove_emitter` + `niagara.add_emitter` with `inherit: true`.

Same `EDITOR_OPEN` refusal and the same pre-save `dataInterfaceCheck` gate as the two handle verbs:
the merge replaces the emitter's scripts and graph through `UpdateFromMergedCopy`, so an open
toolkit's stack widgets would be left holding the outgoing ones, and the rewritten scripts can leave
the compiled and resolved data-interface sets disagreeing. Live instances of the system are stopped
first, exactly as `add_emitter` stops them. **A merge changes the graph but compiles nothing:** pass
`compile: true` (or run `niagara.compile` afterwards) and then save, or the refreshed content never
reaches a running effect.

### niagara.list_orphan_data_interfaces

**This is not the same set as `niagara.remove_data_interface`, and the two cannot substitute for
each other.** `add_data_interface` / `remove_data_interface` address **parameter-store** entries:
authored, named, visible in the Niagara editor's parameter panel, addressed by `scope` +
`parameterName`. This verb and its sibling address a script's **resolved** set
(`FNiagaraScriptRuntimeCompiledData::ResolvedDataInterfaces`) plus the compile-time cached defaults
it is rebuilt from — derived plumbing nothing in the editor UI shows, and the set
`NIAGARA_DATA_INTERFACE_MISMATCH` counts. Deleting a parameter-store entry cannot clear that
refusal, and these two cannot delete an authored parameter.

Takes `systemPath` only, writes nothing. An **orphan** is a resolved entry whose `compileName` has
no counterpart left in its script's compiled `DataInterfaceInfo` list — a leftover the bytecode no
longer references, usually from an emitter duplicated off a stock template whose compiled results
were adopted rather than recompiled.

Response fields:

- `dataInterfaceCheck` — the same three-way verdict `add_emitter` reports. `"unverified"` means
  nothing could be compared (the system has not resolved its data interfaces this session), so an
  empty `orphans[]` **is not evidence there are none**. Compile the system and ask again.
- `orphans[]` — `scriptPath`, `emitter`, `resolvedIndex`, `name`, `compileName`, `type`,
  `dataInterfaceClass`, `internal`. A `name` carrying an `__INTERNAL__.` prefix is the resolve step
  saying it could not bind that entry to any parameter, which is the usual shape of a leftover.
- `mismatchedScripts[]` — the count-level view, in the same field spelling the
  `NIAGARA_DATA_INTERFACE_MISMATCH` payload uses, so the refusal that sent you here is directly
  comparable.

The two lists answer different questions and neither implies the other. A system can report
`"consistent"` and still list orphans: that is the count-equal case where one leftover stands in for
one entry the bytecode does want, and it is precisely the case removal refuses to act on.

### niagara.remove_orphan_data_interfaces

Drops every orphan `niagara.list_orphan_data_interfaces` reports, and prunes the cached default
entry each one was built from so the next resolve pass does not put it back. This is the recovery
route out of `NIAGARA_DATA_INTERFACE_MISMATCH` when recompiling has not cleared it; before it
existed the only way out was rebuilding the system from a stock one with `asset.duplicate`.

Params: `systemPath` (required), `save` (default false).

**Planned in full before anything is written.** Unless dropping a script's orphans brings its
resolved count back to its compiled count, the whole call is refused with
`NIAGARA_ORPHAN_REMOVAL_UNSAFE` and **no script is touched** — the payload carries `blockedScripts[]`
in the same shape as `mismatchedScripts[]`. Two states land there: the compiled list is the longer
one, so no removal can close the gap; or the two lists agree in count while disagreeing by name, so
a leftover is standing in for an entry the bytecode still wants and removing it would arm the
VectorVM assert rather than disarm it. Recompile instead.

Response fields:

- `outcome` — `"removed"`, `"nothing_to_remove"`, or `"unverified"` (the refusal never returns
  success). `"nothing_to_remove"` is a success, not a failure: the verb is idempotent, so a repeat
  of a repair that already landed converges rather than erroring.
- `dataInterfaceCheck` — re-measured **after** the removal, not inferred from it. This is the field
  that says whether the system is safe to tick now.
- `removed[]`, `removedCount`, `scriptsTouched` — what went.
- `scriptsPrunedDurably` — of `scriptsTouched`, how many also had their serialized
  `CachedDefaultDataInterfaces` list pruned. That list is what the next resolve rebuilds the
  resolved set from, so a script counted in `scriptsTouched` but **not** here regrows its orphans on
  the system's next compile. The pruning is skipped for a script whose two lists have already
  drifted in length, because there is then no way to tell which cached entry produced which resolved
  one and a guess would delete a live data interface.
- `saveRequested` / `saved` — `save` only writes when something was actually removed, so
  `saveRequested: true, saved: false` beside `outcome: "nothing_to_remove"` is the honest report.

**Live previews stop, but only on a call that has something to remove.** When orphans are found,
every `UNiagaraComponent` playing the system has its instance destroyed first: a ticking
`FNiagaraSystemInstance` reads the resolved set this rewrites, on a worker thread. Re-activate
afterwards with `effect.activate_niagara`. A call that finds nothing leaves previews alone. There is
deliberately no `EDITOR_OPEN` guard —
the resolved set is derived plumbing no Niagara stack widget snapshots, and the repair runs the
engine's own `OnCompiledDataInterfaceChanged` notification, which an open toolkit listens to.

### niagara.compile

Reports what happened, in fields that no longer contradict each other:

| field | meaning |
|---|---|
| `requested` | a compile was issued to the engine |
| `compiled` / `completed` | a compile was issued **and** observed to finish within the wait budget |
| `waited` / `waitedMs` | whether the handler blocked, and for how long |
| `timeoutSeconds` / `timedOut` | the effective wait budget, and whether it expired with work active |
| `stillCompiling` | affected Niagara System object paths still active when the bounded wait expired |
| `outstandingCompilationRequests` | CPU-script work from `HasOutstandingCompilationRequests(false)`, measured after the wait |
| `status` | `completed` \| `timedOut` \| `outstanding` \| `notRequested` — the single value to branch on |
| `rapidIterationMerge` | `merged` \| `skipped_no_wait` \| `skipped_no_compile` \| `skipped_emitter_asset` — whether the rapid-iteration merge ran |
| `rapidIterationPreserved` | how many rapid-iteration values the rebuild changed and this call put back; **`null`** whenever `rapidIterationMerge` is not `merged` |

**A compile rebuilds the system scripts' rapid-iteration stores, and this verb puts the authored
values back.** Each system script's store is regenerated from that script's own graph traversal and
then overwritten from the emitter-stage stores it depends on, so a value written straight into
`systemSpawnRapidIteration` / `systemUpdateRapidIteration` — the only stores `niagara.set_parameter`
can address for an emitter-scoped module constant — used to be discarded on every compile, silently.
The handler now snapshots those values before the request and merges them back onto the parameters
that survived the rebuild; `rapidIterationPreserved` counts what it restored, and `0` is the ordinary
result. Parameters the rebuild dropped are **not** re-added: re-introducing a parameter the traversal
no longer reaches makes the next compile rebuild again.

**The merge needs `wait: true`, and the response says so rather than reporting a hollow zero.** With
rapid-iteration parameters baked out (`bBakeOutRapidIteration`), the engine defers the store rebuild
into a compile task that runs *after* the DDC search, so a merge on a `wait: false` call would
precede the rebuild it is meant to undo, restore nothing, and publish a `0` this page defines as
"reproduced every value". The merge therefore runs only when a wait actually ran; otherwise
`rapidIterationPreserved` is `null` and `rapidIterationMerge` names the reason
(`skipped_no_wait` for a fire-and-forget compile, `skipped_no_compile` when none was issued,
`skipped_emitter_asset` for an emitter target, whose fan-out to every loaded system is not
snapshotted). Branch on `rapidIterationMerge`, never on the count alone. If you need the guarantee
after a `wait: false` call, re-run with `wait: true`.

`force` defaults true and is honoured. With `force: false` on a system the engine already considers
current, no compile is issued and the response says so (`status: "notRequested", compiled: false`)
rather than claiming one ran. An emitter request is true only when at least one loaded, source-valid
system using that emitter returned true from `RequestCompile`; a source-less system is skipped, and
an emitter with no requestable system reports `notRequested`, never a false `completed`. GPU shader
compilation is deliberately outside the wait predicate — the corruption being prevented is in the
CPU script's compiled data-interface list, and
blocking on shaders would add minutes to every paired edit; read `niagara.validate` if you need that
state.

`wait` defaults true and `timeoutSeconds` defaults to 60 (valid range 0.01-60). With `wait: true`, the
call repeatedly advances the public asset-compilation manager and polls Niagara without entering
the engine's unbounded `WaitForCompilationComplete` API. With `wait: false`, it returns after
requesting; poll `niagara.compile_status` until `completed: true`, then inspect `successful` before
saving or running the effect. A timed-out compile keeps running after the response and is not a
completed result; use `stillCompiling` to identify the systems and `niagara.compile_status` for the
follow-up observation.

### niagara.compile_status

A compact, non-blocking probe for the compile state of a Niagara System or Niagara Emitter. It starts
no compile and returns these fields:

| field | meaning |
|---|---|
| `status` | `compiling` \| `completed` \| `failed` \| `unverified` |
| `completed` | the queue was observable, no VM/GPU compile is outstanding, and every script reported a terminal compile status |
| `successful` | `completed` is true and every reported script status passed |
| `outstandingCompilationRequests` | VM or GPU work is still in flight in `compileQueueScope` |
| `outstandingIncludesGpuShaders` | always true; states the scope of `outstandingCompilationRequests` explicitly. It is not a measurement — it never means GPU work is pending, or that this asset has any |
| `hasGpuSimulation` | measured: at least one emitter in scope is `GPUComputeSim` with a GPU compute script. False here means the inclusive scope above is empty for this asset, so no GPU shader work can be what `outstandingCompilationRequests` is reporting |
| `cpuScriptCompilationPending` | the same queue check with GPU shaders excluded |
| `compileQueueScope` | `system` or `loadedSystemsUsingEmitter` |
| `compileQueueObserved` | whether a queue exists in that scope; false for an emitter used by no loaded system |
| `affectedSystemCount` | one for a system target; for an emitter, loaded systems whose handles use it |
| `scriptCompileCheck` | `passed` \| `failed` \| `unverified`; mixed known/unknown or dirty scripts are unverified |
| `failedScriptCount` | scripts whose last reported status is `NCS_Error` |

Use `status: "compiling"` as the keep-polling case. `failed` is a completed compile with at least one
script error; `unverified` means the probe cannot prove every script has compiled and is not a pass.
For a standalone emitter with `affectedSystemCount: 0`, it remains unverified even when cached emitter
statuses look terminal because Niagara exposes compile queues only on systems.

### niagara.set_parameter

Six scopes. Three are system-wide and take no `emitter`: `user` (the system's exposed parameters),
`systemSpawnRapidIteration`, `systemUpdateRapidIteration`. Three are emitter-scoped and need one:
`rendererBindings`, `spawnRapidIteration`, `updateRapidIteration`.

`emitter` is the emitter **handle name** as it appears in the system (matched case-insensitively) —
not an index, not a version GUID. Read it from `niagara.inspect`. It is required by the three emitter
scopes when `assetPath` is a Niagara System, and omitted when `assetPath` is a Niagara Emitter asset,
whose own data is used. The three system-wide scopes reject it with `INVALID_ARGUMENT` rather
than accepting a value they cannot honour. Omitting it on an emitter scope returns
`EMITTER_REQUIRED` naming the scope.

The parameter must already exist — this verb writes, it does not create. `PARAMETER_NOT_FOUND` means
create it with `niagara.add_parameter` first. `PARAMETER_TYPE_MISMATCH` reports both types as
`name (path)`, so the existing and the requested one are always distinguishable — two Niagara types
can share a display name (an enum and the `NiagaraInt32` struct it is stored as). Per-emitter
authored scalars (`Constants.<Emitter>.<Module>.<Input>`) live in `spawnRapidIteration` /
`updateRapidIteration`, so that pair is the route for them.

**`type` spellings, and the value shape each one takes.** `niagara.set_parameter` and
`niagara.add_parameter` resolve `type` through one table, so what one accepts the other accepts.
Both the canonical name `niagara.inspect` prints as `type.name` and the short alias are accepted,
case-insensitively; so is any other registered Niagara type by its own name or by the struct path
`inspect` prints as `type.struct` (`/Script/Niagara.NiagaraInt32`), which takes a JSON object of the
struct's fields.

| canonical (`inspect` prints) | aliases | `value` |
|---|---|---|
| `NiagaraFloat` | `float`, `double` | number |
| `NiagaraInt32` | `int`, `int32` | number |
| `NiagaraBool` | `bool`, `boolean` | boolean |
| `NiagaraHalf` | `half` | number |
| `Vector2f` | `Vector2`, `Vec2` | `{x,y}` or `[x,y]` |
| `Vector3f` | `Vector`, `Vector3`, `Vec3`, `/Script/CoreUObject.Vector` | `{x,y,z}` or `[x,y,z]` |
| `NiagaraPosition` | `Position` | `{x,y,z}` or `[x,y,z]` |
| `LinearColor` | `Color` | `{r,g,b[,a]}` or `[r,g,b,a]` |
| `NiagaraID` | `ID` | `{index,acquireTag}` or `[index,acquireTag]` |
| `NiagaraSpawnInfo` | `SpawnInfo` | object of the struct's fields |
| `NiagaraHalfVector2` / `3` / `4` | `half2`/`half3`/`half4`, `HalfVec2`/`3`/`4` | `{x,y[,z[,w]]}` or the matching array |

Both `Vector` spellings resolve to Niagara's single-precision `Vector3f`, which is what parameter
stores hold; the double-precision core `Vector` is not addressable through them. The other LWC core
types are not shadowed — `Vector2D`, `Vector4` and `Quat` resolve through the registry to
themselves and take a JSON object of the struct's fields.

`type` must be a **string**. `inspect` reports a parameter's type as an object; send its `name`, not
the object — the object is refused (`PARAM_TYPE_MISMATCH` at the wire gate, `INVALID_ARGUMENT` at the
parser), never accepted and dropped. A type outside the table and outside the registry is refused
`INVALID_PARAMETER_TYPE` listing the aliases; data-interface and UObject-typed parameters are not
writable through this verb at all (use `niagara.add_data_interface` for those).

### niagara.add_parameter

Creates a parameter that does not exist yet; `PARAMETER_EXISTS` means the name is taken and
`niagara.set_parameter` is the verb that writes it. Scopes and `emitter` behave exactly as they do
there, and `type` is resolved through the same table — read the `type` spellings and value shapes
under `niagara.set_parameter`. The initial value goes in `defaultValue`.

### niagara.set_static_switch

`value` for an **Integer** switch takes a declared branch index. Boolean values are shorthand:
`false` selects branch `0` and `true` selects branch `1`. The index must be inside the switch node's
declared range (`0..N-1`); an out-of-range value is refused with `INVALID_VALUE` naming that range
instead of being stored for Niagara to resolve unpredictably. This also covers Integer switches
presented as two-state toggles in stock modules such as `Light_Attributes`.

`value` for an **enum** switch takes the branch index, the authored entry name, or the display name
the Niagara editor shows — matched case-insensitively and ignoring spaces and underscores, so `2`,
`NewEnumerator1` and `Random Uniform` all select the same branch.

Reach for the label. On the user-defined enums Niagara's stock modules use for their switches
(`EmitterState` loop behaviour, `InitializeParticle`'s mode switches, `ShapeLocation`,
`KillParticlesInVolume`, ...) the authored names are `NewEnumerator0..N` and their **order is a
permutation of the label order** — `NewEnumerator1` is not necessarily branch 1. Neither the index nor
the label is derivable from the other.

The branch table is published rather than guessed. Every response for an enum switch carries
`enumPath` and `enumOptions[]` of `{index, name, displayName}` — the success response, the
`INVALID_VALUE` rejection payload, and the `staticSwitchInputs` entries of `niagara.inspect` /
`asset.dump` (`niagara_stack.json`, `niagara_model.json`). Read one of those instead of probing an
integer at a time. Hidden and `Spacer` entries and the trailing `_MAX` sentinel are not branches and
are absent from the table.

The success response also echoes `index` and `displayName` beside `value`, which is the authored name
now on the pin — that is what Niagara itself reads back, so a display name is never what gets stored.
An index outside the table is refused, not written: Niagara clamps an unresolvable selector to branch
0 at compile time, so a wrong index used to produce a green compile, a green
`niagara.validate {level:"strict"}` and the wrong behaviour.

Reading back is symmetric: a `staticSwitchInputs` entry with `source: "override"` whose stored pin
value does not resolve to a branch carries **no `value` field at all**, and reports `rawValue` (the
string on the pin) plus `valueError` instead. It used to report the declared default under
`source: "override"`, which no reader could tell apart from a real override to that value.

### niagara.create_system

Creates an empty `UNiagaraSystem` and saves it. Populate it afterwards with `niagara.add_emitter`.

**`name` is a BARE asset name, never a path, and `savePath` is the only argument that chooses a folder.** The name is checked against the engine's object-naming rules (`FName::IsValidXName` / `INVALID_OBJECTNAME_CHARACTERS`) and the composed `<savePath>/<name>` against `FPackageName::IsValidLongPackageName`, so a `/`, `\`, `.`, `..`, a leading or trailing slash, a space or an unmounted root is refused `INVALID_ARGUMENT` with the engine's own reason text quoted. This is not naming pedantry: a `name` beginning with `/` used to compose `<savePath>//Game/...` and `CreatePackage` logs a double slash at **Fatal**, which is not compiled out in any configuration — the call did not fail, the editor **process** died with every unsaved package in it. A trailing slash on `savePath` is still accepted and trimmed.

Two spellings that used to be quietly repaired are now refused, because both wrote somewhere the caller did not name: a `name` containing `.` or `:` was truncated at that character, and a `name` containing an interior `/` composed a nested package.

### niagara.create_emitter

Creates an empty standalone `UNiagaraEmitter`. Several systems can inherit from it through `niagara.add_emitter` — each takes its own child copy that keeps a parent link back to this asset, so an edit here reaches all of them once `niagara.refresh_emitter` merges it in. The asset is created **inheritable**, so `add_emitter`'s default never has to refuse on it (unlike an emitter `asset.duplicate`d from a stock template — see `niagara.add_emitter`). `name` and `savePath` carry exactly the validation and the same `INVALID_ARGUMENT` refusal described under `niagara.create_system` above.

### niagara.set_curve_keys

**Two addressing forms. Pick by which keys you send; sending both is refused.**

| form | keys | reaches |
|---|---|---|
| parameter store | `scope` + `parameterName` (+ `emitter`) | a DI something already resolved into a store: `User.` DIs, renderer bindings, `add_data_interface` results |
| module input | `entryId` + `inputName` (+ `emitter`, `scriptUsage`) | the DI on a stack module's input pin — where every stock curve actually lives |

**The module-input form is the one you want for over-life ramps.** `ScaleSpriteSize`'s
`Uniform Curve Sprite Scale`, `ScaleMeshSize`'s `Uniform Curve Mesh Scale` and `ScaleColor`'s alpha
curve are **not in any parameter store**. Both rapid-iteration stores were read in full on an
emitter owning three curve DIs and held not one data-interface entry, so every `scope` +
`parameterName` spelling for them returns `DATA_INTERFACE_NOT_FOUND` — correctly, because no store
holds them. `entryId` + `inputName` is the same addressing `niagara.set_module_input` takes,
including the owner-qualified `entryKey` form.

**An input still at its script default gets its own override first.** A module input showing
`valueMode: "default"` in `niagara.inspect {includeStack:true}` has no DI of its own — the object
the stack displays belongs to the **module asset**, shared by every placement of that module in the
project, engine content included. Writing there would edit `/Niagara/Modules/...` itself. So the
write path creates an override pin and its own data interface first, **seeded from that default**
so an edit to one channel keeps the authored shape of the rest. The response reports
`createdOverride: true` the first time and `false` on every later write to the same input.

An input driven by something that is not a data interface — a dynamic-input chain, a linked
parameter, an inline expression — is refused `MODULE_INPUT_OVERRIDE_LINKED` naming the driver:
there is no curve object there to edit. Clear it with `niagara.reset_module_input` first, or edit
the driving source. A wrong `inputName` is refused `MODULE_INPUT_NOT_FOUND` **and the message lists
the module's data-interface inputs**, so the spelling is discoverable rather than guessable.

Responses carry `addressing` (`"moduleInput"` / `"parameterStore"`) and `valueMode`, and echo only
the fields of the form that was used.

### niagara.get_curve_keys

The read half of `niagara.set_curve_keys`, taking the same two addressing forms and the same
`channel` selector — omit `channel` and every channel of the data interface comes back.

**This is the only surface that emits curve samples at all.** `niagara.inspect {includeGraphs:true}`
reports a module's curve input pin as `defaultObject: ""` with `linkCount: 0`, `asset.dump`'s
`nir.txt` and `niagara.decompile_nir` resolve and link the input node but never serialize its keys,
and the `parameters` aspect lists the scalar *multiplier* beside the curve while omitting the curve
— a value that looks like the answer and is not. Read the ramp here before changing it: without a
read path, an overwrite on a shared asset is a blind clobber of whatever was authored.

`valueMode` says whose curve you got. `"data"` is this asset's own override object. `"default"` is
the module **script's** shared object, describing every placement of that module rather than this
emitter's authored ramp; `writable: false` marks it, and `niagara.set_curve_keys` will create an
override rather than write it.

### niagara.reset_module_input

Removing the override pin is only half a reset. An input whose rapid-iteration constant
`Constants.<Emitter>.<Module>.<Input>` exists has its value in two places, and
`niagara.set_module_input` writes both — so a reset that touched only the graph would revert the pin
while the compiled simulation kept the last written value, the write defect with the sign flipped.

The override-pin path therefore also **removes** that constant from every store holding it, and
reports it as `rapidIteration: { parameter, shadowed, action: "removed", previousValue,
updatedScopes }` — the same object `niagara.set_module_input` returns, with `action: "updated"`
there. Removal rather than a value rewrite because the engine's own compile pass supplies the
replacement: it seeds a *missing* constant from the module script's default pin, and only for an
input with no override pin, which is exactly the state a reset leaves. The next compile therefore
regenerates the constant at the module template default — the value the Niagara editor's own
`Reset()` writes back. `shadowed: false` means no constant existed and only the pin needed clearing.

The static-switch path is unaffected: switch overrides live on the caller pin's `DefaultValue` and
have no rapid-iteration constant, so its result carries no `rapidIteration`.

### niagara.clear_module_overrides

Clears every override pin on the module, and removes the rapid-iteration constant behind each one
for the same reason `niagara.reset_module_input` does. The response carries `rapidIteration` as an
**array**, one entry per cleared input that actually had a constant, each in the same shape the
single-input verbs return. It is empty on a module whose inputs are all pin-driven — that is the
true answer, not a missing field. Static-switch caller pins are reset as before and contribute no
entries.
