# niagara.nir

NIR (Niagara text IR) is a decompile-only textual representation of Niagara assets, backed by the `nir.txt` asset-dump sidecar and the `niagara.decompile_nir` RPC. Consult this page when you need the NIR grammar, script-graph body classification, or the editor-internal traps discovered while building the NIR emitter.

## NIR (Niagara text IR, decompile-only)

Both surfaces call `NIRDecompiler::BuildNiagaraIrText(UObject*)`, so the `nir.txt` file and RPC body are byte-identical.

Coverage:
- `system "..."` block with user parameters, warmup/fixed-tick settings, determinism/random-seed state, fixed bounds, scalability, `SystemSpawn` / `SystemUpdate` stacks, and nested `emitter` blocks per handle.
- `emitter "..."` standalone shell with `simTarget`, local-space, determinism/random-seed state, bounds mode/fixed bounds, persistent-ID state, GPU spawn/preallocation hints, renderers (reflected props, filtered by the zero-default rule below), `EmitterSpawn` / `EmitterUpdate` / `ParticleSpawn` / `ParticleUpdate` stacks, `simStage` blocks, and `eventHandler` blocks.
- Module rows: `module ShortName[@vMajor.Minor] @<order> enabled|disabled` + `static X = ...` lines for static-switch inputs.
- GPU emitters keep authored CPU-style stack rows visible. NIR does NOT statically flag "GPU-incompatible" modules — see "GPU vs CPU is per-script-usage, not per-emitter" below for why that classification can't be done in the decompiler. Genuine GPU-incompatibility is a compile-time determination and surfaces through `niagara_compile.json` when the compiler reports it.
- Event handlers and simulation stages include metadata plus a graph/script body using the standalone script-graph grammar; the simulation-stage block keyword remains `simStage`.
- GPU compute scripts are emitted as `graph ParticleGPUCompute { ... }` content when the emitter has a GPU compute script.
- Authored parameter data: `param`, `rapid`, and `binding` lines include type, value, and scope annotations when the backing Niagara parameter store exposes them.
- Module input override-chain expansion (v1b): each override-node-driven `input X = ...` resolves to one of:
  - **Literal**: `input Strength = 3.14` (or per-type serialised form for vectors / quats / structs).
  - **Linked parameter**: `input Strength = $User.Speed` (the upstream `UNiagaraNodeInput` exposes its scoped parameter handle).
  - **Dynamic input** (recursive): `input Strength = dynamic NoiseClamp { input Range = 0.5 }` — the inner module's own inputs are emitted via the same expression emitter; `dynamic Name@vMajor.Minor` for version-pinned references. Depth is bounded at 32 — chains beyond that emit `# recursion-limit-reached` and a matching `FNIRResult.Warnings` entry.
  - **Static switch** override: emitted as `static X = ... @source override|default @default <value>` alongside the module row when source/default metadata is available (decoded via `NiagaraDumpBuilder::BuildStaticSwitchInputs`).
- Standalone `UNiagaraScript` emits `script "..." { graph <Usage> { ... } }` with a node-by-node body (v1c — see "Script-graph body grammar" below).
- Authored-state stability: `nir.txt` omits live compile-readiness annotations and emitter-handle `valid` / `needsRecompile` flags. Query `niagara.inspect` or `niagara.validate` when runtime compiler state matters.

**Script-graph body grammar (v1c)**

Inside `graph <Usage> { ... }` each `UNiagaraNode` emits a `node Name : Class @(x, y)` identity line plus one semantic line (or one nested block) classified into three families. Duplicate display names are suffixed as `_2`, `_3`, etc. Pin wiring uses explicit `link FromNode.FromPin -> ToNode.ToPin` lines plus the older SSA-style `%upstream.OutputPinName` value form where a semantic line needs an RHS. Positions are recorded as `@(x, y)` suffixes for traceability — logical equivalence is preserved regardless of layout (see `feedback_ir_logical_not_visual`).

Parameter data lines:
- `param User.SpawnRate : Float = 12.0 @scope user` — exposed/user parameter values.
- `rapid Constants.InitializeParticle.Lifetime : Float = 2.0 @scope particleSpawnRapidIteration` — rapid-iteration constants; system and emitter spawn/update scopes are emitted separately.
- `binding User.Color : LinearColor = {...} @scope rendererBindings` — renderer binding store entries.

Data-flow family (`NIRGraphEmit_Dataflow`):
- `%result = op Add(input0: %A, input1: %B) @(x, y)` — `UNiagaraNodeOp`; `OpName` comes from `UNiagaraNodeOp::OpName`.
- `%out = get $Engine.DeltaTime : Float @(x, y)` — `UNiagaraNodeParameterMapGet`, one line per output pin.
- `set $Particles.Color = %value @(x, y)` — `UNiagaraNodeParameterMapSet`, one line per real input pin (implicit map pin skipped).
- `call @SpawnRate@v1.0 (input SpawnRate = 50.0) @(x, y)` — `UNiagaraNodeFunctionCall`; **reference only** (the called module's own `nir.txt` carries its graph, no inline subgraph expansion). Version suffix `@vMajor.Minor` when `SelectedScriptVersion != FGuid()`.
- `%val = input $Particles.Position : Vector @(x, y)` — `UNiagaraNodeInput`.
- `output ParticleSpawn (slot0: %A, slot1: %B) @(x, y)` — `UNiagaraNodeOutput`.

Control-flow family (`NIRGraphEmit_Control`):
- `staticSwitch $UseGravity : Bool { case true: %out = %thenPin case false: %out = %elsePin } @(x, y)` — `UNiagaraNodeStaticSwitch`; cases decoded via the shared `BuildStaticSwitchInputs` helper.
- `if (%cond) then %thenA, %thenB else %elseA, %elseB @(x, y)` — `UNiagaraNodeIf`.
- `select $Selector { case <V>: %out = %src ... } @(x, y)` — `UNiagaraNodeSelect`.
- `selectUsage { case ParticleSpawn: %out = %src case ParticleUpdate: %out = %src } @(x, y)` — `UNiagaraNodeUsageSelector`.
- `selectSimTarget { case CPUSim: %out = %src case GPUComputeSim: %out = %src } @(x, y)` — `UNiagaraNodeSimTargetSelector`.

Utility family (`NIRGraphEmit_Util`):
- `customHlsl @(x, y) { input X : Float = %src\noutput Y : Float\nreturn 1.0f; }` — `UNiagaraNodeCustomHlsl`; verbatim HLSL body on inner lines, declared `input` / `output` lines above for correlation. Source via `UNiagaraNodeCustomHlsl::GetCustomHlsl()`.
- `convert (%inA -> outA, %inB -> outB) @(x, y)` — `UNiagaraNodeConvert`; surface-level pin pairs only (sub-path / sub-field convert maps are out of scope for v1c, would land if v2 round-trip needs them).
- `reroute %in -> %out @(x, y)` — `UNiagaraNodeReroute`. **Caveat**: reroute is logically a pass-through; downstream consumers may elide it. Emitted purely for traceability (`feedback_ir_logical_not_visual`).

Any `UNiagaraNode` subclass outside this 14-class set falls through to `# unknown-node <ClassName> @(x, y)` and a matching `FNIRResult.Warnings` entry so coverage gaps are surfaced rather than silent.

**Editor-internal gotchas (v1b/v1c discoveries)**

Non-obvious Niagara editor-internal traps that surfaced while building the NIR override-chain emitter and script-graph emitter. Anyone building further decompile / inspection RPCs will hit these:

1. **`NiagaraDumpBuilder::BuildStaticSwitchInputs` is a function-call-input view, not a static-switch-node view.** It takes a `UNiagaraNodeFunctionCall*` and walks the static-switch nodes inside that function call's *called subgraph*, decoding the caller's override pins. It does NOT enumerate a `UNiagaraNodeStaticSwitch` node's own branches. Plans/reviews calling for "reuse `BuildStaticSwitchInputs` to enumerate cases" of a directly-graphed static-switch node are factually mismatched — there is no public analogous helper for that direction; walk the option-major pin layout manually.

2. **`UNiagaraNodeUsageSelector::GetInputCaseName` is `protected`** and unreachable from free functions. For symbolic case labels in option-major selectors, use enum reflection instead: `StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(OptionValue)` (UsageSelector) or `StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(OptionValue)` (SimTargetSelector). Fall back to raw integer + `Out.Warn` on unmapped values.

3. **`UNiagaraNodeFunctionCall` has subclasses.** Both `UNiagaraNodeCustomHlsl` and `UNiagaraNodeAssignment` derive from `UNiagaraNodeFunctionCall`. A `Cast<UNiagaraNodeFunctionCall>` will match them. Family dispatchers (e.g. NIR's `NIRGraphEmit_Dataflow` vs `NIRGraphEmit_Util`) must explicitly `IsA<UNiagaraNodeCustomHlsl>()`-reject in the FunctionCall branch so CustomHlsl falls through to the Util family.

4. **`BuildNiagaraIrText` dispatch is path-asymmetric.** Calling `BuildNiagaraIrText(System)` walks the System's stacks via `AppendStack`, which only enumerates `UNiagaraNodeFunctionCall` modules — it does NOT dispatch arbitrary graph nodes to the family emitters. The family-emit path (Dataflow/Control/Util) is reached only via `BuildNiagaraIrText(Script)` → `EmitScriptPlaceholder` → `EmitScriptGraphScope` → `EmitGraphBody`. Tests targeting per-node-class emission must call `BuildNiagaraIrText` on the *script* (e.g. the emitter's spawn script), not the system.

5. **Niagara pin defaults are canonical-text at storage.** `UEdGraphSchema_Niagara::TrySetDefaultValue` serializes pin values through `FNiagaraTypeDefinition::ToString` before storing them on `Pin->DefaultValue`. So `Pin->DefaultValue` is already the type-canonical text form — re-implementing a typed dispatcher to format scalar/vector/struct values from raw bytes is redundant for the common cases. The exception is `UNiagaraDataInterface*` pins: their literal emission needs the DI class name (`dataInterface <ClassName>`), not the canonical pin string.

## Reflected blocks: the zero-default rule

Reflected-property blocks — `renderer <Class> @N enabled { ... }` and `simStage <Name> @N { ... }` — are **filtered**. Read a missing line by this rule and nothing else:

| line form | meaning |
|---|---|
| `Prop: <value>` | authored override; the value differs from the class default |
| `Prop: <value> @default` | the property sits at its class default, and that default is **not** the type's zero value |
| line absent | the property holds its type's zero value — `false`, `0`, empty string / name / array, null object, identity struct |

So absence carries exactly one meaning, and it is the one a reader guesses. `bSubImageBlend: true @default` is printed because `UNiagaraSpriteRendererProperties::bSubImageBlend` defaults to **true**; before this rule the line was simply absent and the naive parse (`'bSubImageBlend: true' in body`) returned the opposite of the truth on every unmodified sprite renderer. `SubImageSize` (default `(1,1)`), `bCastShadows` (default `1`), `bSortOnlyWhenTranslucent` (default `1`) and the ~28 `FNiagaraVariableAttributeBinding` renderer bindings are all printed for the same reason.

Consequences worth knowing:

- **A `@default` line is not an authored value.** The marker is what preserves the authored-vs-inherited distinction that plain omission used to encode. Do not treat a `@default` line as evidence the asset was edited.
- **`@default` values print in full, not as a diff.** A struct emitted because it sits at a non-zero default is exported against a nullptr default, so `SubImageSize: "(X=1.000000,Y=1.000000)" @default` shows both components. Overridden struct lines keep the archetype diff, so their sub-fields still suppress against per-field defaults.
- **Renderer blocks are ~30-40 lines, not ~6.** Most of the growth is renderer bindings, which are load-bearing (they answer "which attribute drives sprite colour") and were previously invisible.
- **Strip `@`-suffixes before parsing a value.** This is the same convention as `@scope`, `@source override @default <value>`, and `@(x, y)` elsewhere in NIR.

The mechanism is `FReflectedFieldEmitOptions::bEmitNonZeroDefaults` in IrCore, off for every IR except NIR. `Plugins/PinWright/docs/ir-authoring.md` carries the family-wide position and why the other IRs have not adopted it yet.

## Structural emission rules

Two NIR emission rules cover system/emitter coverage parity and the dynamic-pin sentinel hazard. Both are routinely violated by ad-hoc emitter changes and surface as missing `link` lines or stray `: Unknown` type tags.

**Graph-scope must cover every main script**

NIR emits two parallel views per script: a `stack <Name> { ... }` (module sequence via `AppendStack`) and a `graph <Usage> { node ...; link ... }` (full node-link wiring via `EmitScriptGraphScope`). For link-coverage parity with the `niagara_graphs.json` sidecar, the graph scope must be emitted for **all six main scripts** — `SystemSpawn`, `SystemUpdate`, per-emitter `EmitterSpawn` / `EmitterUpdate`, and `ParticleSpawn` / `ParticleUpdate` — not just the special scripts (sim stages, event handlers, GPU compute) that originally drove the graph emitter.

Restricting graph emission to special scripts caps link coverage at roughly 24-27% of the JSON sidecar's `links[]` entries. The link walk (iterate output pins, follow `LinkedTo`) was correct; the gap was script coverage. `EmitScriptGraphScope(const UNiagaraScript* Script, FNIRTextEmitter& Out)` null-guards `Script`, so paired calls beside each `AppendStack` are safe even when `GetSystemSpawnScript()` is null on shells that defer compile. This complements gotcha #4: the `System` entry path must emit graph scopes per main script instead of relying on the `Script` path.

Pinned by a regression test.

**Suppress the dynamic "Add" sentinel pin**

`UNiagaraNodeParameterMapGet` and `UNiagaraNodeParameterMapSet` (and any node deriving from `UNiagaraNodeWithDynamicPins`) expose a sentinel "Add" pin that exists purely as a UI affordance for adding new typed parameters. It has:

- `PinSubCategory == "DynamicAddPin"` (`UNiagaraNodeWithDynamicPins::AddPinSubCategory`)
- `PinSubCategoryObject == nullptr`
- no associated `FNiagaraTypeDefinition`

The engine itself excludes it from compile via `UNiagaraNodeWithDynamicPins::IsValidPinToCompile`. NIR must mirror that exclusion: skip via the engine helper `UNiagaraNodeWithDynamicPins::IsAddPin(Pin)` (public static at `NiagaraEditor/Public/NiagaraNodeWithDynamicPins.h`) before falling into `FormatPinType`, otherwise the sentinel materializes as `: Unknown` in the emitted text. Use the engine helper, not an inlined `PinSubCategory == "DynamicAddPin"` check — the helper is the canonical entry point and is stable across dynamic-pin subclasses.

Pinned by a regression test.

## GPU vs CPU is per-script-usage, not per-emitter

The NIR decompiler must NOT try to statically flag "GPU-incompatible" modules on GPU emitters. That was proposed (annotate GPU-emitter stack rows whose modules "won't run on GPU") and deliberately not done after investigation proved the premise invalid: there is no module-level GPU-incompatibility a decompiler can faithfully detect. Four engine facts underpin this — verify against the engine source under `Engine/Plugins/FX/Niagara/Source` before reopening.

**1. Niagara's CPU/GPU split is per-script-usage, not per-emitter.** Only `ParticleSpawnScript..ParticleGPUComputeScript` are GPU scripts — `UNiagaraScript::IsParticleScript(Usage)` is `Usage >= ParticleSpawnScript && Usage <= ParticleGPUComputeScript` (`NiagaraScript.h:1032`). System and Emitter spawn/update scripts ALWAYS run on CPU even when the emitter's `SimTarget == GPUComputeSim`. The engine's own validation code says so: `UNiagaraValidationRule_ModuleSimTargetRestriction` ("system and emitter scripts are always cpu scripts", `NiagaraValidationRules.cpp:1655`), and `UNiagaraValidationRule_NoMapForOnCpu` explicitly skips particle modules on GPU emitters ("modules used in gpu scripts are ignored", `NiagaraValidationRules.cpp:1607`). So modules appearing under `stack EmitterSpawn` / `EmitterUpdate` / `SystemSpawn` / `SystemUpdate` and running on CPU is correct, not a leak.

**2. `ModuleUsageBitmask` encodes which stack groups a module may be placed in, NOT CPU-vs-GPU.** `UNiagaraScript::IsSupportedUsageContextForBitmask(bitmask, ParticleGPUComputeScript)` is always false by default: `ParticleGPUComputeScript` is `UMETA(Hidden)` (`NiagaraCommon.h:1132`) and `GetSupportedUsageContextsForBitmask` excludes hidden usages (`bIncludeHiddenUsages = false`, `NiagaraScript.h:1052-1053`), while `IsEquivalentUsage` only collapses `ParticleSpawnScript ↔ ...Interpolated` (`NiagaraScript.h:996`). So that predicate can't distinguish GPU-incompatible modules — it would flag everything.

**3. Top-level module nodes carry no signature.** A `UNiagaraNodeFunctionCall` is backed EITHER by `FunctionScript` (a module/script asset, whose inline `Signature` is empty — `Signature.Name.IsNone()` is true) OR by an inline `Signature` (data interface / builtin). The code branches `FunctionScript ? … : Signature…` throughout (`NiagaraNodeFunctionCall.cpp:544,1847`). So a `Signature.bSupportsGPU` check on a top-level module node finds nothing to read — module nodes are `FunctionScript`-backed.

**4. Real per-DI GPU capability is `UNiagaraDataInterface::CanExecuteOnTarget(GPUComputeSim)`** (base default `false`, but e.g. `UNiagaraDataInterfaceSkeletalMesh` overrides to `return true` — `NiagaraDataInterfaceSkeletalMesh.h:801`; skeletal-mesh sampling is GPU-capable). This is precisely why an asset using `SampleSkeletalMesh` legitimately ships as `GPUComputeSim`. The genuine GPU-incompatibility signal is resolved at compile time (HlslTranslator `bGPUSim && !InMatchingSignature.bSupportsGPU`, `NiagaraHlslTranslator.cpp:8985`) and surfaces through `niagara_compile.json`. The faithful approach for the decompiler is to relay the compiler's own diagnostics, never to invent a static classifier — any static `GPU_INCOMPATIBLE_MODULE` annotation produces false positives on CPU-by-design emitter/system modules and on GPU-capable DIs.

The separate, legitimate ergonomic gap (NIR not labelling which stacks run on CPU vs GPU) was discussed and deliberately left as-is.

This behavior is intentional and will not change.

## See also

- [`niagara`](niagara.md) — the read-first Niagara inspection and mutation workflow.
- [`niagara.dump-files`](niagara.dump-files.md) — the dump artifacts NIR is emitted alongside.
- [`niagara.compile-state`](niagara.compile-state.md) — compile status, stack usage bitmasks, and script swapping.
- [`material.mgir`](material.mgir.md) — the sibling graph text IR with the same round-trip conventions.
