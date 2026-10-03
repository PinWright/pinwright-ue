# niagara.compile-state

Niagara editor-state gotchas: uninitialised compile reporting, the real storage for static-switch and stack-input overrides, and post-swap reconciliation for module scripts. Consult this before writing related RPCs.

## Compile-state honesty: `compileStatus: null` and `COMPILE_STATE_UNINITIALIZED`

`fx.Niagara.OnDemandCompileEnabled` defaults to `1` (UE 5.3-5.5) or `2` (UE 5.6+), and either value defers compile in the editor: unlike Blueprints, Niagara assets do not compile on load. A loaded asset keeps the per-script statuses of its last saved compile (`CachedScriptVM` is serialized), so one whose saved bytecode is in sync with its graph reads `scriptCompileCheck: "passed"` with no compile this session. One that is out of sync is parked by PostLoad for an on-demand compile until it is opened in the asset editor or spawned as an FX component; that shows as `pendingCompile: true` / `NIAGARA_COMPILE_PENDING`, not as a pass. A script with no compile result at all (a freshly created `UNiagaraSystem`/`UNiagaraEmitter`, or one never compiled) reports `NCS_Unknown` and `bIsReadyToRun=false` even when the asset is healthy.

The dumper handles this honestly rather than parroting the misleading `NCS_Unknown` text:

- Per-script `compileStatus` is JSON `null` when the value is `NCS_Unknown`, not the literal string.
- System-level `readyToRun` is JSON `null` when any script is uninitialised, not `false`.
- An info-severity issue `COMPILE_STATE_UNINITIALIZED` is added to the issues array on both system and emitter dump paths, with a message pointing at `niagara.compile` (or opening/spawning the asset) to populate compile state. The same compile block is the live `compile` block of `niagara.validate` and `niagara.inspect`, so both verbs surface the issue too (and `niagara.validate` / `niagara.compile_status` report `scriptCompileCheck: "unverified"` for the same reason). It is about compile state, not about whatever edit was just validated; `niagara.compile` clears it.
- Emitter spawn/update scripts are never compiled on their own (`UNiagaraScript::IsCompilable` is false; the engine folds their modules into the system spawn/update scripts and leaves them at `NCS_Unknown`). Their compile-block entries carry `compiledIntoSystemScripts: true` and are excluded from `COMPILE_STATE_UNINITIALIZED`, the null `readyToRun` and `scriptCompileCheck`; otherwise a system with an emitter could never leave `unverified`.
- `compileDeferredOnLoad` (system, emitter and script compile blocks of `niagara.inspect` / `niagara.validate`) always reports the session fact: whether `fx.Niagara.OnDemandCompileEnabled` defers compile on load (the editor default).
- The `COMPILE_DEFERRED_ON_LOAD` info issue (a warning in `niagara.validate`) is raised only when that deferral actually affects this asset: for a system, when it is not ready to run or still has a compile request pending (PostLoad leaves a system whose cached bytecode is out of sync with its graph pending on demand); for an emitter or script asset, when any script is not at a terminal status (`null`, `NCS_Dirty`, `NCS_BeingCreated`). A system compiled in this session carries no such issue. Its presence means "run `niagara.compile` before trusting this verdict".

For callers, `null` means "we don't know yet", not "compile failed". Open the asset to trigger a real compile before treating null fields as authoritative. Do **not** synchronously call `RequestCompile` from the dumper: `UNiagaraScript::CachedScriptVM` is a non-`Transient` UPROPERTY, so compilation silently mutates persistent state and a later save would commit it.

## Static switch override storage

`UNiagaraNodeStaticSwitch` overrides live on the **caller pin's `DefaultValue`**, not `RapidIterationParameters`. The dump's `staticSwitchInputs[]` and `niagara.set_static_switch` use that caller-pin path; routing them through `niagara.set_module_input` or parameter-store helpers silently no-ops or misroutes.

## Stack-module input override storage

`niagara.set_module_input` writes to a stack-resident `UNiagaraNodeParameterMapSet` "override node" between the module's parameter-map input pin and upstream stack. That is the pin the graph carries, but it is not the whole store picture: a compile also generates `Constants.<Emitter>.<Module>.<Input>` in `UNiagaraScript::RapidIterationParameters`, and the literal write path pushes the same value into every store that already holds that constant so the two cannot disagree (`PinWrightNiagara::WriteThroughModuleInputConstant`). It never creates a constant that did not exist.

To locate the override pin for input "Foo" on module `M`: walk `M`'s input-map pin → `LinkedTo[0]->GetOwningNode()` (the `UNiagaraNodeParameterMapSet`) → match pin by aliased handle string `FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(CreateModuleParameterHandle("Foo"), &M).GetParameterHandleString()` (e.g. `"MyModule.Foo"`).

Resetting removes that pin and chained upstream nodes (dynamic-input override chains and linked-parameter `UNiagaraNodeInput` nodes). The plugin uses `NiagaraResetModuleInput::FindStackFunctionOverrideNode` + `RemoveOverridePinAndChainedNodes` in `NiagaraEditHandler.cpp`. Static-switch overrides instead use the **caller pin's `DefaultValue`** and `UEdGraphSchema_Niagara::ResetPinToAutogeneratedDefaultValue`; do not use the override-node helpers.

`FNiagaraStackGraphUtilities::GetStackFunctionOverrideNode` (NiagaraStackGraphUtilities.h:208, UE 5.6) and `FNiagaraStackGraphUtilities::RemoveNodesForStackFunctionInputOverridePin` are both un-exported (`NIAGARAEDITOR_API` absent). Plugin code must walk the override graph inline. To reach the `UNiagaraNodeParameterMapSet` (whose header lives in `NiagaraEditor/Private`), lazy-resolve the class: `static UClass* MapSetClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));` then `IsA(MapSetClass)` + `static_cast`. See the "Engine-helper non-export gotcha" section in [`niagara.authoring`](niagara.authoring.md) for the full un-exported-helper inventory.

## Swapping the script behind a module node (niagara.set_module_script)

To swap the `UNiagaraScript` behind an existing `UNiagaraNodeFunctionCall`, set the new reference then call `RefreshFromExternalChanges()` — not nonexistent-in-UE-5.6 `FNiagaraStackGraphUtilities::SetMessageScript`. The sequence is:

```
ModuleNode->Modify();
ModuleNode->FunctionScript = NewScript;
ModuleNode->SelectedScriptVersion = NewScript->GetExposedVersion().VersionGuid;
ModuleNode->InvalidScriptVersionReference = FGuid();
ModuleNode->MarkNodeRequiresSynchronization(__FUNCTION__, true);
ModuleNode->RefreshFromExternalChanges();   // NiagaraNodeFunctionCall.h:112
```

`RefreshFromExternalChanges` (public, `NIAGARAEDITOR_API`) compares the node's `CachedChangeId` to the new graph's `ChangeID` and runs `ReallocatePins` + `SynchronizeReferencingMapPinsWithFunctionCall` + `FixDynamicInputNodeOutputPinsFromExternalChanges` — the full post-swap reconciliation. Snapshot old overrides (walking override-node pins) **before** the swap and re-apply by `FNiagaraVariable` name+type match **after** `RefreshFromExternalChanges`, because pin slots change.

Compatibility check before swapping: `UNiagaraScript::IsSupportedUsageContextForBitmask(NewScript->GetLatestScriptData()->ModuleUsageBitmask, CurrentStackUsage)` is the canonical "can this script live in this stack group" predicate.

Stack targeting accepts shared stage aliases such as `SystemSpawn`, `SystemUpdate`, `EmitterSpawn`, `EmitterUpdate`, `ParticleSpawn`, and `ParticleUpdate`. If `niagara.set_module_script` resolves the target module by name, id, or index without an explicit `scriptUsage`, it finds the owning stack output by walking forward from the module node to the downstream `UNiagaraNodeOutput`. It does not scan all output stacks and infer ownership by a global search.

## See also

- [`niagara`](niagara.md) — the read-first Niagara inspection and mutation workflow.
- [`niagara.nir`](niagara.nir.md) — the NIR text IR for reading an emitter stack as text.
- [`niagara.graph`](niagara.graph.md) — direct script-graph node and pin operations.
