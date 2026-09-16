# blueprint

Author Blueprint *classes* — create new BPs, add variables / functions / events, manage components, mutate the class default object (CDO), reparent, compile, decompile, and inspect. Use this branch for class-level work; for per-instance edits on placed actors reach for `call("actor")` instead, and for direct node/pin surgery on an existing graph drop into `call("blueprint.graph")`.

## Cross-cluster overlap

For most logic authoring, use `blueprint.compile_bpir`: one text-IR call produces wired graphs and can preserve authored `@(x, y)` positions. Use `blueprint.graph` only for fine-grained surgery on an existing graph (for example, deleting an orphan, moving a comment, or reconnecting one pin). See `call("bpir")` for syntax (`call("bpir.instructions")`), examples (`call("bpir.examples")`), and errors (`call("bpir.errors")`).

For a structural read of any BP, `blueprint.inspect` is the right entry point — broader than the asset-registry summary `blueprint.get` and structured for automation clients. For deep multi-asset audits, call `asset.dump_folder` first, then read the cached `bpir.txt` / `properties.json` / `scs.json` files instead of repeating `blueprint.inspect` per asset.

- **Components — class vs instance** — `blueprint.scs.add_component` adds a *template* component to the BP class; every newly spawned actor gets it. `call("actor.add_component")` adds an *instance* component to one placed actor only. Same name, different worlds — pick deliberately.
- **CDO defaults vs per-instance** — `blueprint.set_default` mutates the CDO so every new spawn starts with the value. `call("actor.set_blueprint_variables")` changes one already-placed instance. CDO edits do not retroactively affect existing instances.
- **Widget Blueprints** — graph logic still uses `blueprint.compile_bpir` / `blueprint.decompile`, but named widget variables and `widget_event` entries depend on the widget tree. Cross-check [`widget`](widget.md) when BPIR references UMG widgets.

## Blueprint path parameter aliases

Every handler in this namespace accepts the Blueprint asset path under any of: `path`, `assetPath`, `blueprintPath`, `blueprint_path`, `requestedPath`. The resolver is centralized in two free functions in `Handlers/Blueprint/BlueprintHandlerUtils.cpp`:

- `ResolveBlueprintPath(FHandlerContext& Ctx)` — used by most handlers that have a live request context. Also accepts `name` as an alias (handy when the only thing the caller knows about a BP is its short name).
- `ResolveExplicitBlueprintPath(const TSharedPtr<FJsonObject>& Payload, bool bNormalize)` — used in no-subsystem callers (tests, payload parsing helpers where `name` would collide with a semantic name field). Does NOT accept `name`.

Both functions share the field lists exposed by `VisitBlueprintPathScalarFieldNames`; to add a new alias project-wide, update that helper and the `BlueprintPathParamReq` / `BlueprintPathParamOpt` metadata path. `assetPath` is the most recently unified alias - it is what `blueprint.inspect`, `blueprint.decompile`, `blueprint.compile_bpir`, `blueprint.create_bpir_expression`, `blueprint.compile`, `blueprint.graph.find_orphaned_nodes`, and `blueprint.graph.delete_orphaned_nodes` all resolve through. `blueprint.graph` also delegates path resolution to `ResolveBlueprintPath` via `BlueprintGraphHandler::ResolveBlueprintAndGraph`.

Wire-level aliases must be present in `FParamSpec`, not only in handler-side resolver code. `RpcDispatcher` performs required-parameter and unknown-parameter validation before the handler runs; an alias-only payload reaches the handler only when that alias is listed on the canonical `FParamSpec`. Discovery also reports these metadata fields: scalar aliases such as `path`, `assetPath`, `blueprintPath`, `blueprint_path`, `requestedPath`, and `name` are emitted as `aliases`, while candidate arrays such as `blueprintCandidates` and `candidates` are emitted as typed array aliases. Keep `name` off explicit-path call sites where it is a semantic object name instead of an asset-path alias.

## Compiling rebuilds live instances — and can refuse

A Blueprint compile flushes UE's reinstancing queue: every live instance of the class in every loaded world is **destroyed and re-created**, and the level that owns it is marked dirty. That includes placed actors in a map someone else has open. `blueprint.compile`, `blueprint.set_default`, `blueprint.reparent`, `blueprint.modify_scs`, and all six compiling `blueprint.scs.*` mutators therefore refuse with `LIVE_INSTANCES_WOULD_BE_REINSTANCED` when loaded worlds hold live instances, naming the count and each owning world in the error payload's `reinstanced` block. Pass `allowReinstancing: true` to accept the rebuild, or stop PIE / close the map first. `blueprint.modify_scs` applies this check only when `compile:true` or an `add_component` operation takes its implicit compile path.

For `blueprint.compile_bpir` and the `insert_bpir_*` verbs, an active PIE session is an additional preflight refusal before the Blueprint is loaded; they return `PIE_ACTIVE`, and `allowReinstancing:true` cannot bypass that refusal.

`blueprint.set_default` is in that set because a raw CDO write does not persist without a compile — the verb runs one for you, so it carries the same side effect as `blueprint.compile`.

Whenever a registered RPC actually performs a full Blueprint compile, its response reports `compiled`, `status`, error and warning arrays, plus the same `reinstanced` block when live objects were rebuilt (`count`, `actorCount`, `pieActive`, and one `worlds[]` entry per owning world). Some established response contracts name the arrays `compileErrors` / `compileWarnings` instead of `errors` / `warnings`; `reinstanced` is absent when no live instances were present. Conditional and idempotent paths that do not compile do not add compile fields.

Every registered RPC that performs a full Blueprint compile is deferred to a safe point before it runs, including the Blueprint, networking, game-framework, interaction, vehicle, AI, GAS, and widget mutators. Reinstancing from inside the engine's frame destroys objects whose tick tasks are already queued, which kills the editor outright. Cost is one subsystem tick (0.1 s worst case).

Full compile paths converge on `BlueprintHandlerUtils::CompileBlueprintWithDiagnostics`, and that choke point passes `EBlueprintCompileOptions::SkipGarbageCollection` to `FKismetEditorUtilities::CompileBlueprint`. UE 5.8's `CompileSynchronouslyImpl` otherwise calls `CollectGarbage` after reinstancing, which broadcasts `PythonScriptPlugin`'s global pre-GC callback while the compile handler is still active. The helper requests an equivalent full purge for the next engine GC opportunity instead; that later collection still invokes the Python pre-GC callback after the handler has returned. This scheduling change neither proves nor repairs the broader unverified theory that a completed `python.execute` permanently poisons the editor session. It also does not make compilation tick-safe: reinstancing still happens, so `blueprint.compile` and `blueprint.set_default` must remain in `GTickUnsafeMethodNames` and continue through safe-point dispatch.

User-defined type creation is also editor-only: `blueprint.create_enum` and `blueprint.create_struct` return `PIE_ACTIVE` before creating the asset when Play In Editor is active. Stop PIE and retry; this is separate from the live-instance compile refusal above.

## See also

- [`blueprint.graph`](blueprint.graph.md) for direct graph node and pin surgery after BPIR generation.
- [`blueprint.scs`](blueprint.scs.md) for class-level component-template trees.
- [`blueprint.bpir-gotchas`](blueprint.bpir-gotchas.md) for the BPIR trap checklist (compile-pair-with-validate, enum pins, dispatcher self-context, native event overrides, etc.).
- [`blueprint.cpp-sequencing`](blueprint.cpp-sequencing.md) for the C++-then-BPIR workflow that avoids dangling `UFunction` references.
- [`asset`](asset.md) for cached `bpir.txt`, `scs.json`, and dump-folder audit workflows.
- [`widget`](widget.md) for Widget Blueprint trees, widget variables, and UMG event binding context.
- `call("bpir")` for the BPIR topic index — covers entry points, instructions, types, pure/impure, and errors.
- `call("bpir.examples")` for worked graph-authoring patterns.
- [`asset-audit`](asset-audit.md) — building a repeatable text mirror of Blueprints before analysis or bulk edits.

### blueprint.compile_bpir

Compiles a text-based Blueprint IR into wired graph nodes and runs `blueprint.compile` implicitly on success. Single call replaces a long sequence of `blueprint.graph.create_node` + `blueprint.graph.connect_pins` RPCs and is the preferred way to author non-trivial graph logic.

With the default `allowPreexistingErrors:false` behavior, if BPIR placement succeeds but the following whole-Blueprint compile fails, `compile_bpir` and both `insert_bpir_*` verbs return `BLUEPRINT_COMPILE_FAILED` with the standard `compiled:false`, `status`, error/warning arrays, and `reinstanced` block when live instances were rebuilt, and roll back the transaction. For `compile_bpir`, `allowPreexistingErrors:true` is the exception: when the placement introduces no new compile errors and all remaining errors are exclusively pre-existing, the valid placement is retained and the response reports `success:true`, `compiled:false`, and `preexistingErrorsOnly:true` with full diagnostics; if it introduces a new error, the default rollback/error path applies.

Reach for it whenever you want to add a fresh function body, an event handler, a small graph excerpt, or seed a new BP's logic from scratch. The IR syntax is documented in `call("bpir.instructions")` (with entry-point shapes in `call("bpir.entry-points")` and type rules in `call("bpir.types")`); worked examples live in `call("bpir.examples")`. To learn the IR for an unfamiliar pattern quickly, decompile an existing graph with `blueprint.decompile` and adapt the output.

Node positioning is per entry body. If no primary node-backed instructions in a body have `@(x, y)`, BPIR uses auto-layout and implicit helper/generated nodes are allowed. If every primary node-backed instruction in a body has `@(x, y)`, those absolute coordinates are preserved and implicit visible helpers are rejected. Mixed positioned/unpositioned primary nodes fail with a manual-placement diagnostic; fix by adding positions to every visible node-backed instruction, removing all positions for auto-layout, or making the helper explicit with its own positioned BPIR line. Comment-only lines, transparent reroutes/knots, and zero-primary-node instructions do not carry position suffixes.

For Widget Blueprints, build or inspect the widget tree with [`widget.import_xml`](widget.import_xml.md), [`widget.export_xml`](widget.export_xml.md), or [`widget.describe`](widget.describe.md) before compiling BPIR that references widget variables. For UMG event handlers, see [`widget.bind_event`](widget.bind_event.md) and `call("bpir.examples.widget-event")`.

PIE safety: after validating the required path/code fields, `compile_bpir` and both `insert_bpir_*` verbs check the shared editor play-mode state before loading the Blueprint, pre-compiling, opening a transaction, or mutating the graph. While PIE is active they return `PIE_ACTIVE` with an instruction to stop PIE and retry; `allowReinstancing:true` does not bypass this refusal. A successful non-PIE call changes the Blueprint in memory only, so call [`asset.save`](asset.md) after PIE has stopped when the edit should be persisted.

#### mode: "append" / "replace" (iterative development)

Through the public RPC, `compile_bpir` defaults to `mode: "append"`, and both `"append"` and legacy `"replace"` use the same upsert path. Matching ordinary entries are removed before the new BPIR is emitted, so retrying a timed-out compile or iterating on the same entry does not accumulate duplicate entry nodes. An output-bearing Blueprint Interface implementation is the exception: its graph belongs to `ImplementedInterfaces`, so upsert preserves that graph plus every function entry/result terminator and removes only its previous body nodes before compiling the replacement body.

1. For each BPIR entry (`custom_event`, `event`, `function`, `macro`), find the existing entry node whose name matches.
2. Starting from that node, walk the exec-output graph breadth-first to collect every node reachable through exec pins — the full subgraph that "belongs" to the entry.
3. Delete the entry node and every collected subgraph node, except that an interface-owned function graph keeps its entry and result terminators and deletes only body nodes.
4. Compile the new BPIR. `SetupCustomEvent` also skips its duplicate-rejection check in replace mode, which allows the new custom event to have a different signature from the one just removed.

Use the default upsert path whenever iterating on an entry in place. `mode: "extend"` is the special case: it skips the deletion pass and appends a new body to the terminal exec pin of an existing override when the existing chain is linear.

#### Sequence node — exec pin auto-creation

When `compile_bpir` (or `insert_bpir_at_node`) targets a **Sequence** node and the requested `execPin` does not yet exist, the pin is automatically added up to index 15 (i.e. `then_0` … `then_15`). Both UE internal format (`"then_4"`) and human-friendly format (`"Then 4"`) are accepted. If the Sequence only has `then_0` and `then_1`, pins `then_2` through `then_4` are created automatically before wiring. Requesting index > 15 returns an error.

#### Failed-compile rollback uses FTransaction::Apply + FScopedTransaction::Cancel (never UTransBuffer::Undo)

When `compile_bpir` (and the two `insert_bpir_*` siblings) hits a fatal compile error, it must roll back without invoking the undo broadcast. Going through `UTransBuffer::Undo` (the older `PinWrightTransactionUtils::RollbackLastTransaction` path) fires `FEditorDelegates::PostUndoRedo`, which the Blueprint editor's undo client answers by running `MarkBlueprintAsStructurallyModified` — a synchronous skeleton compile that hits `ensure(SkeletonCompiledBlueprints.Num() == 1)` at `BlueprintCompilationManager.cpp:424`. The ensure dialog blocks the game thread, which is also the HTTP transport tick — so the symptom from the caller side is: a malformed BPIR call appears to time out, then port 19880 stops accepting new requests until the editor is restarted.

The correct rollback uses `FTransaction::Apply` + `FScopedTransaction::Cancel` while the transaction is still scoped — it reverts recorded property diffs without firing the undo broadcast (see the comment at `BpirCompilerHandler.cpp` near line 124: "avoids the IsObjectTransacting ensure and the duplicate ..."). In addition, the handler captures a `BlueprintGraphSnapshot` of every authored graph returned by `UBlueprint::GetAllGraphs` *before* opening the `FScopedTransaction`, including implemented-interface, delegate, child, and extension graphs. The snapshot removes non-transactional node additions from any surviving captured graph, removes newly added ubergraph, function, and macro graphs, and captures the pre-existing pin-link topology for restoration. `PinWright.blueprint.compile_bpir.RollbackNoStructuralCompile` proves failed rollback emits no undo callbacks, and `PinWright.blueprint.compile_bpir.InterfaceGraphRollbackAfterFinalCompileFailure` proves a whole-Blueprint compile failure restores an interface-owned graph exactly.

For callers: if a `compile_bpir` call returns `COMPILE_FAILED` and the next RPC hangs, the editor is sitting on this ensure dialog — restart UE, reconnect the MCP client, and treat the failing BPIR as the bug rather than the gateway.

#### Generic K2Node round-trip via `call K2Node_<Type>(...) node_props { ... }`

BPIR has typed keywords for the common K2Node subclasses (`branch`, `foreach`, `cast`, `make`, `subsystem`, `call_dispatcher`, ...) but the long tail of K2Nodes (`UK2Node_PlayAnimationTimeRange`, `UK2Node_GenericCreateObject`, `UK2Node_VariableSetRef`, plugin-defined K2Nodes) has no dedicated parser surface. Rather than add a `generic` opcode, the existing `call` opcode does double duty: when the function-name slot starts with `K2Node_` or `UK2Node_`, the compiler routes to `CodeNodeEmitter::CreateGenericK2Node` and the decompiler emits the same shape from `EmitGenericNode`.

For non-pin C++ UPROPERTY state on the K2Node itself (e.g. `UK2Node_FormatText::PinNames` populated from the format string, or any shape-determining property the node consults during `AllocateDefaultPins`), append a single-line `node_props { Key: Value, Key2: Value2 }` block:

```
call K2Node_FormatText(Format: "Score: {0}", 0: $score) node_props { CachedPinNames: "0" }
```

Property-ordering rule: `node_props` keys whose target UPROPERTY is shape-determining (drives pin allocation) are applied before `AllocateDefaultPins()`; everything else is applied after. The compiler uses a per-class metadata table to classify (`BpirShapeMetadata.cpp`); unrecognized properties default to post-allocate. Multi-line `node_props` blocks are not supported — the line-based parser preserves single-line shape only.

Decompiler emits `node_props { ... }` from non-default UPROPERTY values via `BuildSparsePropertyDiffJson` (CDO diff). Nodes with zero editable UPROPERTYs (most `UK2Node_CallFunction`-derived, including `UK2Node_CreateWidget` and `UK2Node_SpawnActorFromClass`) emit no block — their per-pin state travels through normal pin args, not `node_props`.

**The round trip is by node class, and a `UK2Node_CallFunction` subclass does not come back through this lane.** `FGraphWalker::ClassifyNode` casts to `UK2Node_CallFunction` *before* the class registry walk, so every subclass of it decompiles as a plain `call Foo(...)` and recompiles as a plain `UK2Node_CallFunction` — the class the generic lane was asked for is not what the text says. Where that mattered (`UK2Node_Message`) the fix is a first-class keyword, `message Interface::Function(...)`, not the generic lane; see `call("bpir.instructions")`. A correct `call K2Node_Message(...)` built through the generic lane *did* produce a real message node, but decompiled as an ordinary `call`, which is indistinguishable from the node never having been created, so a decompile that shows no message node is not evidence that none was built. Reach for `call K2Node_<Type>(...)` only for node classes outside the `UK2Node_CallFunction` hierarchy, and verify what was built by node class rather than by reading the decompiled text.

#### Multi-input-exec target syntax: `@label.PinName`

BPIR's default exec-target syntax wires into a target node's first input exec pin. For nodes with multiple input exec pins — engine standard macros `Gate` (Enter/Open/Close/Toggle), `MultiGate` (Enter/Reset), `DoOnce` (Start/Reset), `DoN` — appending `.<PinName>` to the target label routes into a specific input pin:

```
branch(...) [True -> @gate.Enter, False -> @gate.Reset]
```

Pin names containing spaces use BPIR's existing backtick quoting convention: `` @gate.`Open Pin` ``. The lookup is `FName`-equality (case-insensitive); whitespace tolerance is **not** supported.

**Hard-fail policy.** Missing or misnamed pins fail the compile with a diagnostic listing the available input exec pins (`available: Enter, Reset`). There is no silent fallback to the first input exec pin — earlier behaviour would misroute and corrupt the wiring.

Limitation: composite nodes with multiple input exec tunnels are out of scope. Composites are inlined at decompile (see `blueprint.decompile`) and have no compile-side BPIR opcode, so the syntax is only meaningful for `macro` and direct K2Node targets.

#### Struct-literal pin defaults follow K2's validator grammar

BPIR's struct literal formatter is a K2-validator-grammar registry, not a generic `ExportText` consumer. The Kismet schema's `IsStringValid<T>` validators each accept only a hardcoded set of textual forms; values outside that set are silently rejected by `TrySetDefaultValue` and the pin reverts to zero with no diagnostic.

Three forms are currently supported:

- `FVector` → `(X=,Y=,Z=)` keyed form
- `FRotator` → **positional CSV `pitch,yaw,roll`** — keyed form `(Pitch=,Yaw=,Roll=)` is silently zeroed by `IsStringValidVector`, which `IsStringValidRotator` delegates to and which only accepts `X=Y=Z=` keys or positional CSV.
- `FLinearColor` → `(R=,G=,B=,A=)` keyed form

The registry shape is intentional: the engine itself hardcodes these grammars, and a generalized `ExportText`-based formatter would emit `(Pitch=,Yaw=,Roll=)` long form for FRotator and silently corrupt round-trips. New struct literal types must be added to the registry with the validator's actual accepted form, not a guess.

### blueprint.insert_bpir_at_node

Inserts BPIR into an existing execution chain after `nodeId`. This is headless insertion mode: omit the `entry ... {}` wrapper and provide only body instructions, labels, and calls. The anchor node supplies the initial exec context.

By default insertion uses the first exec output pin. For multi-output nodes, pass `execPin` (`"True"`, `"False"`, `"then_4"`, `"Then 4"`). Pin matching is case-insensitive. If the target pin has downstream connections, the inserted code is spliced between the anchor and all existing downstream exec targets.

For Sequence nodes, missing exec output pins are auto-created up to `then_15`; requests for `then_16` or higher fail. Anchor node non-exec output pins are also available by bare parameter name inside the inserted BPIR, so code inserted after a Custom Event with `HitActor` can use `HitActor` directly.

Use `context` to expose external existing pins as `$Name` references:

```json
{
  "assetPath": "/Game/BP_MyActor",
  "nodeId": "<target node GUID>",
  "code": "call PrintString(InString: $MyVar)",
  "context": { "MyVar": "abc12345-...:ReturnValue" }
}
```

Object-typed return references support `%ref.Property` access, e.g. `%target = call GetTarget()` then `%target.Health`. See `call("bpir")` for the full syntax and examples.

On an insertion failure, `insert_bpir_at_node` removes inserted nodes and any newly created graphs as applicable, restores the snapshot's full-Blueprint pin-link topology, and verifies that restoration. If verification is incomplete, it returns `INTERNAL_ERROR` and preserves the original failure details in the payload under `originalErrorCode` and `originalErrorMessage`, with `rollbackVerified: false` and the verification diagnostic in `rollbackError`.

### blueprint.insert_bpir_before_node

Inserts headless BPIR before `nodeId` by finding the target node's upstream exec connection and inserting after that upstream pin. It cannot insert before entry/event nodes because they have no upstream exec connection.

Use `blueprint.get_node_connections` first when you need to inspect a node's upstream/downstream exec topology before choosing `insert_bpir_at_node`, `insert_bpir_before_node`, or a specific `execPin`. For broader graph reads, see [`blueprint.graph`](blueprint.graph.md).

### blueprint.decompile

Decompiles Blueprint graph logic back to BPIR-style text for review, reuse, and compile-loop iteration. Use it before writing unfamiliar `blueprint.compile_bpir` input, or inspect the cached `bpir.txt` produced by `asset.dump` / `asset.dump_folder` when you already dumped the asset.

Decompiler output includes current `@(x, y)` suffixes for node-backed BPIR lines, so graph nodes moved in the editor can round-trip into authored BPIR positions. Transparent reroutes/knots stay transparent in v1 and do not gain BPIR position suffixes.

For direct node topology questions after decompile — exact pins, node GUIDs, orphaned nodes, graph connections, or execution flow — switch to [`blueprint.graph`](blueprint.graph.md). For Widget Blueprints, pair decompiled `widget_event` handlers with [`widget.describe`](widget.describe.md) or [`widget.export_xml`](widget.export_xml.md) so widget variable names come from the actual UMG tree.

#### K2Node_Composite is inlined into the parent graph

Collapsed graphs (`K2Node_Composite`) are pure editor-time grouping — `FKismetCompilerContext::ExpandTunnelsAndMacros` dissolves them via `Schema->CollapseGatewayNode` before any node-handler scheduling, so they have no runtime artifact. The decompiler reflects that: when a graph contains composites, the decompile pass clones the graph under the Blueprint's outer, calls `BoundGraph->MoveNodesToAnotherGraph` per composite (the same primitive `BlueprintEditor::ExpandNode` uses), and walks the inlined result. Callers reading `bpir.txt` will never see a `composite` opcode or a `call K2Node_Composite(...)` placeholder.

Two round-trip implications:

- **Cloning is gated on actual composite presence.** `FEdGraphUtilities::CloneGraph` auto-suffixes the clone name on collision (`Set Error` → `Set Error_2`), which would corrupt every emit path that surfaces the graph name. Composite-free graphs walk the original directly with no clone overhead.
- **Visual grouping is lost by design.** BPIR's round-trip principle is logical equivalence, not visual fidelity — a recompile of the inlined BPIR will not re-collapse the nodes back into a composite.

Note: `UK2Node_Composite::DestroyNode()` asserts via `FindBlueprintForNodeChecked()`, so the inline pass tears down composites with manual `BreakAllNodeLinks` + `Graph->Nodes.Remove` + `MarkAsGarbage` instead of `DestroyNode`.

#### Optional-pin omission for object-ref defaults

The decompiler omits unwired optional pins from emitted `call`/`pure` argument lists when their default value matches the autogenerated default. For object-ref pin categories (`PC_Object`, `PC_Class`, `PC_Interface`, `PC_SoftObject`, `PC_SoftClass`), `IsPinOmittableAtCallSite` short-circuits on `DefaultValue ∈ {"", "None"}` rather than calling the engine's `DoesDefaultValueMatchAutogenerated()` — that engine helper returns false for object-ref pins whose `DefaultObject` is null even when the textual default is the canonical "none", which previously emitted noise like `ConcurrencySettings: ?` and `OwningActor: ?` on every `PlaySound2D` call site.

The public predicate lives in `BpirTextEmitterInternal::IsPinOmittableAtCallSite` (private header reachable from the test module). Pin categories outside the object-ref set still defer to the engine helper.

#### Decompiler warnings shape and severity

`FBpirDecompileResult::Warnings` is `TArray<FBpirWarning>` where `FBpirWarning { FString Text; EBpirWarningSeverity Severity = Warn; }`. `Severity::Error` is reserved for the three decompiler-internal "shouldn't happen" sites in `BpirDecompiler.cpp` — composite-inline-depth exceeded, reconvergence could not find line index, and source node visited but produced no value name. All other warning sites (orphan nodes, multi-connected `self` pins, cast-to-Unknown, and the other graph-hygiene diagnostics) are `Warn`.

Surface impact:

- `bpir.txt` files in asset dumps emit one `# BPIR_WARN: <text>` or `# BPIR_ERROR: <text>` comment line per warning instead of the previous single `# BPIR_FAILED: <first>` line. Consumers grepping the dump corpus for true decompiler failures should filter on `BPIR_ERROR:`; graph-hygiene notes from authored source live under `BPIR_WARN:`.
- `blueprint.decompile` JSON serializes `warnings` as an array of `{ "text": "...", "severity": "warn" | "error" }` objects instead of bare strings. Callers that previously iterated warnings as `FString` must read `.text` (and optionally branch on `.severity`).

### blueprint.create_struct

Creates a `UUserDefinedStruct` asset. UE forbids a zero-field user-defined struct, so the engine always seeds a new struct with one default bool member named `MemberVar_0` — an "empty" create still starts with one field.

The handler reclaims that seed automatically so you never have to clean it up:
- Pass the whole layout in the `fields` array at creation — the first entry renames/retypes the seed in place, the rest append, and you get exactly the fields you listed (no stray `MemberVar_0`). This is the recommended path.
- Or create with no `fields` and build the struct via `blueprint.add_struct_field`. The **first** `add_struct_field` on a freshly created struct reclaims the untouched seed the same way (rename/retype in place), and subsequent adds append — so N adds yield exactly N fields, matching the create-with-`fields` path.

The seed is only reclaimed while it is pristine: a single field with the engine `MemberVar_<n>` default name, plain bool, no container, and no user-set default value, tooltip, or metadata. Once you have renamed, retyped, or edited that lone field, it is treated as a real field and `add_struct_field` appends as normal.

### blueprint.list_struct_fields

Lists fields on a `UUserDefinedStruct` asset. The response includes the struct `path`, root `guid`, `count`, asset verification fields, and `fields`.

Each field keeps the compatibility `type` string as the collapsed pin description returned by the Blueprint handler (`int`, `string`, `Array<int>`, `Map<string,int>`, etc.). The raw user-defined-struct category from `FStructVariableDescription::Category` is exposed additively as `rawType` so callers can read dump-parity detail without breaking older callers that use `type`.

Field entries mirror the rich struct dump shape: `name`, `displayName`, `guid`, `type`, `rawType`, `subType`, `subTypeObject`, `containerType`, `defaultValue`, `currentDefaultValue`, `tooltip`, `flags`, and `metaData`. `flags` contains `dontEditOnInstance`, `enableSaveGame`, `multiLineText`, and `enable3dWidget`; `metaData` is the field metadata map.

### blueprint.get

Asset-registry-backed summary of a Blueprint class: parent class, `variables`, `functions`, `events`, plus `metadata` (per-variable) and a `defaults` map.

`events` is a live enumeration of the ubergraph nodes, so it is an independent check on the authoring verbs rather than a replay of them: an event authored with `blueprint.add_event` and later removed — including by a default-mode `compile_bpir`, whose Phase 0 sweep deletes it (see [BPIR gotchas](blueprint.bpir-gotchas.md)) — is absent from `events`, and each entry's `parameters` are the node's real pins.

Input entry nodes are included in the same array: `K2Node_InputKey`, legacy input-action, input-axis and input-touch nodes, and `K2Node_EnhancedInputAction`. They retain the common `name`, `eventType`, and `enabled` fields: `name` is the key/action identity (`Touch` for the generic touch node, `<UNBOUND>` for an Enhanced Input node with no action), `eventType` remains the authored node class, and `execOutputs` lists each connected outgoing execution edge as `{pin,targetNodeId,targetNodeTitle}`. This keeps `Pressed` and `Released` bindings on the same key distinguishable without requiring a second `blueprint.graph.get_execution_flow` call. `blueprint.inspect` uses the same event collector and returns the same entries.

Legacy axis bindings are covered separately: `K2Node_InputAxisEvent` uses its authored axis name, while `K2Node_InputAxisKeyEvent` uses the `AxisKey` identity. In UE 5.8, `K2Node_InputVectorAxisEvent` derives from `K2Node_InputAxisKeyEvent`, so it is collected through that same branch while `eventType` retains the concrete vector-axis class. These optional node families are available when their engine headers are present in the host build.

The `defaults` object maps each member variable name to its class-default-object (CDO) value, exported with the same shape `property.get {includeDefault:true}` returns (`defaultSource:"class_cdo"`). It is the readback surface for confirming variable defaults set via `blueprint.add_variable {defaultValue}` / `blueprint.set_default` after a compile. A variable that has been added but not yet compiled onto the generated class is omitted from `defaults` (no CDO property exists yet) rather than reported with a stale value; per-variable `property.get {includeDefault:true}` remains the authority for a single value or for an uncompiled variable.

`blueprint.get` does **not** return a `components` field. Class-level component templates live behind [`blueprint.scs.get`](blueprint.scs.md) (hierarchy + per-node class/transform/properties), and `blueprint.inspect` folds in that same SCS surface. So to *confirm a component you just added to a Blueprint class* — e.g. after `blueprint.scs.add_component` or `ai.add_smart_object_component` — read it back with `blueprint.scs.get`, not `blueprint.get`. For component **hierarchy** (parent/child links between SCS nodes) `blueprint.scs.get` is also the authority; `blueprint.inspect`'s component list reports names without the SCS tree.

### blueprint.inspect

Single-call structural dump of a Blueprint: parent class, variables, functions, events, components, graphs (as BPIR pseudocode + execution-flow summary), and references. Replaces what would otherwise be a fan-out of `blueprint.get` (variables/functions/events) + `blueprint.scs.get` (component templates) + per-graph `blueprint.graph.get_*` calls — the component surface in this fan-out comes from `blueprint.scs.get`, since `blueprint.get` omits components (see the `blueprint.get` section).

Use it as the default first call when you need to understand an unfamiliar Blueprint. It is the C++-side equivalent of the `asset.dump` cache for BPs and produces the same shape — when iterating across many BPs, prefer one `asset.dump_folder` sweep and read the cached `bpir.txt` files.

For component-template detail, use [`blueprint.scs.get`](blueprint.scs.md); for bulk audits, read `scs.json` from the dump cache. `blueprint.get` is the lighter summary with no graphs, references, or components.

Lightweight vs deep mode: pass `includeDecompile: false` for metadata only (no graphs, much faster) when you only need the variable / function / component shape. Use `includeReferences: true` to fold in the same data `blueprint.references` returns, and `includeScriptRefs: true` to surface script-level dependents. Use `includeProperties: true` when you need CDO property values with the same sparse inheritance-tagged shape as `asset.dump` `properties.json`.

`includeProperties` adds a `properties` object to the response. Each key is a reflected property whose Blueprint CDO value differs from the immediate generated superclass CDO, so unchanged inherited defaults are omitted. Each emitted property entry mirrors `asset.dump` `properties.json`: `type`, `value`, `flags` when available, `inherited_from` for properties declared on an ancestor class, and `is_overridden_locally: true` when the child CDO value differs from that immediate parent baseline. This is the live readback path to pair with `blueprint.set_default` when you need to verify local CDO overrides without running a full dump.

Path forms: `blueprint.inspect` accepts short paths like `/Game/MyGame/Blueprints/B_MyGameMode`. Other tools (`asset.search` class filters, `property.set` raw class refs) may require the double-name format `/Game/Folder/AssetName.AssetName`. When in doubt, supply the long form — `blueprint.inspect` accepts both, while the others only accept one.

When investigating a BP -> C++ port that already happened, pair `blueprint.inspect` with `blueprint.decompile` to confirm BP variable names match the C++ `UPROPERTY` names exactly: UE auto-merges only on exact `FName` match (`bNavigatingBackToMainScreen` does NOT merge with `NavigatingBackToMainScreen`).

### blueprint.add_event

The response's `parameters[]` is read back off the created node's user-defined pins — it is not the request array echoed. Trust it as the list of pins that exist, and connect only to names it contains.

`parameters` is checked before anything is created, so a rejection leaves the Blueprint untouched:

| Condition | Error |
|---|---|
| a type that resolves to no concrete pin (it would have become an untyped wildcard pin) | `TYPE_NOT_FOUND` |
| an entry with an empty `name`, or an entry that is not a `{name, type}` object | `INVALID_ARGUMENT` |
| `parameters` supplied with a built-in `eventType` — a built-in node takes its signature from the overridden function and cannot hold user-defined pins | `UNSUPPORTED_ARGUMENT` |

If a pin still fails to appear after the node is authored, the call answers `PIN_CREATION_FAILED` with a `failedParameters[]` list; the node exists and `parameters[]` names the pins that made it.

### blueprint.remove_event

By default, removing an event by name removes every matching event node. When several widgets or components share the same delegate signature (e.g., several `CommonButtonBase` buttons all bound via `CommonButtonBaseClicked__DelegateSignature`), two optional parameters narrow which node is targeted:

| Parameter | Type | Description |
|---|---|---|
| `componentName` | string | Match `K2Node_ComponentBoundEvent::ComponentPropertyName` — the widget variable name the event is bound to. |
| `nodeId` | string | Match the node's `NodeGuid` as a string. Takes precedence over `componentName` when both are supplied. |

Behavior rules:
- **Neither set** (default): all nodes whose event name matches are removed.
- **`componentName` only**: removes the bound-event node whose `ComponentPropertyName` equals the value. Silently ignored for node types that have no component property (`K2Node_CustomEvent`, `K2Node_ActorBoundEvent`, `K2Node_Event`).
- **`nodeId` only** (or **both**): `nodeId` is the authority — removes the single node whose `NodeGuid` matches. `componentName` is ignored when `nodeId` is also set.

Use `componentName` when you know which widget variable the binding belongs to. Use `nodeId` when you have the exact GUID from `blueprint.decompile` or `blueprint.graph.find_nodes`. The default (no disambiguators) remains safe for blueprints where event names are unique.

### blueprint.add_variable

Adds a member variable to a Blueprint class. Recompile (`blueprint.compile`) for the variable to be usable in graphs. For per-variable replication/exposure flags use `blueprint.set_variable_settings`.

**`category` (optional).** The heading the variable groups under in the Blueprint editor's Variables / My Blueprint panel — an editor-only organizational label with no effect on runtime or replication behavior. Pass a **plain string** (e.g. `"Stats"`, `"Minimap"`); omitting it (or passing `""`) leaves the variable in the default uncategorized group. It is *not* a localized FText: no `NSLOCTEXT`/namespace+key is needed or accepted. The same label can be set later with `blueprint.set_variable_settings` (`category`) or, equivalently, with `blueprint.set_variable_metadata` (key `Category`).

**`variableType` containers.** Beyond primitives and `class:`/`struct:` refs, the token grammar accepts three container wrappers — `array<T>` for a `TArray`, `set<T>` for a `TSet`, and `map<K,V>` for a `TMap` — where the inner tokens are themselves any accepted type. So `set<name>` declares a `TSet<FName>` and `map<string,int>` declares a `TMap<FString,int32>`. (The same grammar backs `blueprint.add_function`'s `inputs[].type`; see `call("bpir.types")` for the full type-string reference.)

```json
{ "method": "blueprint.add_variable",
  "params": { "path": "/Game/AI/B_Enemy", "variableName": "MaxHealth", "variableType": "float", "category": "Stats" } }
```

```json
{ "method": "blueprint.add_variable",
  "params": { "path": "/Game/AI/B_Enemy", "variableName": "UnlockedRegions", "variableType": "set<name>" } }
```

### blueprint.set_variable_settings

Sets per-variable settings on an existing member variable (visibility, instance-editable / read-only, expose-on-spawn, replication, transient, save-game, advanced-display, and category). Supply only the fields you want to change; at least one setting is required.

**`category` (optional).** Same plain-string editor-only Variables-panel grouping label as `blueprint.add_variable` — see there for the full contract; setting it here is equivalent. Use this to (re)categorize a variable that was added without one.

```json
{ "method": "blueprint.set_variable_settings",
  "params": { "path": "/Game/AI/B_Enemy", "variableName": "MaxHealth", "category": "Stats", "isInstanceEditable": true } }
```

### blueprint.set_variable_metadata

Applies arbitrary key-value metadata to a Blueprint variable via the `metadata` object. The metadata key **`Category`** is the same plain-string editor-only grouping label as the `category` param on `blueprint.add_variable` (see there for the full contract); setting it here is equivalent, and is the path used when you also want to set other metadata in the same call.

```json
{ "method": "blueprint.set_variable_metadata",
  "params": { "path": "/Game/AI/B_Enemy", "variableName": "MaxHealth", "metadata": { "Category": "Stats", "ToolTip": "Starting hit points" } } }
```

### blueprint.set_default

Writes a property value directly onto a Blueprint's Class Default Object (CDO), then compiles and saves the Blueprint. Distinct from `property.set` (which operates on any live UObject) — `blueprint.set_default` specifically targets the CDO and always triggers a recompile. For non-CDO live objects, or for `DataAsset` fields, prefer `call("property.set")`. To read CDO defaults back, `call("property.list")` and `call("property.get")` auto-resolve `/Game/...` Blueprint paths to the CDO.

#### TSoftClassPtr path auto-resolution

When the target property is a `TSoftClassPtr<>`, `blueprint.set_default` automatically converts `/Game/` Blueprint asset paths to generated class paths by appending `_C`:

```
/Game/UI/W_Foo          ->  /Game/UI/W_Foo.W_Foo_C
/Game/UI/W_Foo.W_Foo    ->  /Game/UI/W_Foo.W_Foo_C   (if not already _C)
/Script/Engine.Actor    ->  /Script/Engine.Actor       (native paths pass through unchanged)
```

Pass the short Blueprint asset path (no `.AssetName` suffix, no `_C`) and the handler resolves it. If the path already contains a dot and ends with `_C`, it is used as-is. For `property.set` or direct CDO manipulation in C++, the generated class path (`/Game/UI/W_Foo.W_Foo_C`) is required — only `blueprint.set_default` does the auto-append.

#### Post-compile CDO readback

`CompileBlueprint` creates a **new CDO**, invalidating the pre-compile pointer. `blueprint.set_default` re-fetches the CDO after compilation and reads the property value back to confirm it survived the compile cycle. Response fields:

| Field | Description |
|---|---|
| `value` | The property value as read from the **post-compile** CDO. |
| `saved` | Boolean — whether the Blueprint was successfully marked for save after the compile. `false` means the change is compiled in memory but was not queued for persistence. |
| `warning` | Present if the value could not be read back — indicates the value may not have survived compilation (e.g. the property was removed or the BP is in error state). |

Always inspect `warning` in the response. Its presence means the write may have been silently discarded.

#### Clearing an object reference to null

To null out an object/class/soft-object reference default (e.g. remove a `DefaultBehaviorTree` or a `DefaultWidgetClass`), pass any of the canonical null sentinels as `value`: the empty string `""`, the string `"None"` (UE's printed form of a null `UObject*`), the string `"null"` (case-insensitive), or a JSON `null`. All clear the reference without attempting an asset load. This applies uniformly to `FObjectProperty`, `FClassProperty`, `FSoftObjectProperty`, and `FSoftClassProperty` defaults.

```json
{ "method": "blueprint.set_default",
  "params": { "path": "/Game/AI/B_Enemy", "propertyName": "DefaultBehaviorTree", "value": "None" } }
```

A non-sentinel string is always treated as an asset path to load; an unloadable path returns `[CONVERSION_FAILED] Failed to load object at path: <path>`.

#### Example

```json
{
  "method": "blueprint.set_default",
  "params": {
    "path": "/Game/MyGame/Modes/B_MyGameMode",
    "propertyName": "DefaultWidgetClass",
    "value": "/Game/MyGame/UI/W_MyWidget"
  }
}
```

The handler resolves `W_MyWidget` -> `/Game/MyGame/UI/W_MyWidget.W_MyWidget_C`, sets it on the CDO, compiles, and returns the post-compile value.

### blueprint.create

Creates a Blueprint asset. In addition to class Blueprint creation, `blueprintType: "interface"` creates a Blueprint Interface through `UBlueprintInterfaceFactory` with `BlueprintType = BPTYPE_Interface`.

When creating an interface, `parentClass` is optional and defaults to `UInterface`. If `parentClass` is supplied with `blueprintType: "interface"`, it must resolve to `UInterface` or a `UInterface` subclass; actor, pawn, character, and other concrete class parents are rejected before asset creation.

Creation is durable before the response is sent. The response carries the standard `saveRequested`, `saved`, `saveState`, and `saveDetail` fields plus asset verification; `saved:true` means the Blueprint is present on disk and the resident package is current. A refused or failed write returns `SAVE_FAILED` with the same result payload, so a Blueprint that exists only in memory is never reported as saved. The former `waitForCompletion` parameter is no longer accepted; persistence ordering is unconditional.

### blueprint.add_interface

**Also covers [`blueprint.remove_interface`](blueprint.remove_interface.md).**

Adds or removes a Blueprint Interface implementation on a Blueprint class. `interfaceClass` accepts a short class name or full class path, but it must resolve to a Blueprint-implementable interface; non-interface classes and Blueprint types that cannot implement interfaces are rejected.

Both calls are idempotent. Adding an interface that is already implemented, or removing one that is absent, returns `changed: false` and does not open a mutation transaction. Real changes go through `FBlueprintEditorUtils::ImplementNewInterface` / `RemoveInterface`, can auto-compile, and can save only when requested.

### blueprint.add_dispatcher

Creates only the Event Dispatcher authoring half on a Blueprint: a member variable with pin category `PC_MCDelegate` and a delegate signature graph named exactly the dispatcher `name`. The response reports the signature function as `<Name>__DelegateSignature`.

`params` is an array of `{ "name": "...", "type": "..." }` signature parameters. Dispatcher params use strict named-pin parsing, so malformed types fail the request instead of becoming wildcard pins.

This RPC does not remove dispatchers, mutate an existing dispatcher signature, bind the dispatcher to a handler, or create call/broadcast nodes in a graph. Use BPIR or [`blueprint.graph`](blueprint.graph.md) for the call-site graph work after the dispatcher exists.

Implementation note for new handlers: when a payload has both an asset path and a semantic object name, resolve the asset path with `ResolveExplicitBlueprintPath` so a `name` field is never interpreted as the Blueprint path. Route `{name,type}` pin arrays through `ParseNamedTypePinParams`; choose strict mode for dispatcher-like signatures and wildcard fallback only where an invalid type should still create an editable pin.

### blueprint.add_function

Creates an empty function graph with a caller-defined signature. UE's physical terminator directions are the inverse of the signature as seen by a caller: each logical `inputs` entry is an `EGPD_Output` user pin on `UK2Node_FunctionEntry`, while each logical `outputs` entry is an `EGPD_Input` user pin on `UK2Node_FunctionResult`. Supplying at least one output creates the result terminator when the fresh graph does not already have one.

Every `inputs[]` and `outputs[]` element must be an object containing exactly the string fields `{name, type}`. Bare strings, missing/empty fields, wrong field types, and extra keys return `INVALID_ARGUMENT` before the Blueprint is loaded or marked busy; they are never skipped silently.

The response's `inputs` and `outputs` arrays are read back from the reconstructed terminators' surviving physical non-exec pins, and the same measured signature is stored for `blueprint.get`. Pin names therefore reflect UE's normalized graph names rather than the request spelling. Every requested name and type must match one physical pin after reconstruction; duplicate names and collisions with built-in pins such as the entry's `then` exec pin fail with `PIN_CREATION_FAILED` and roll back the new graph.

After authoring, the handler performs a full Blueprint compile and reports its measured `compiled`, `status`, `errors`, and `warnings` fields. The response's `success` matches `compiled`, and saving is attempted only after a successful compile.

With `override:true`, the parent signature decides the representation. A void override that UE permits as an event remains a `K2Node_Event`; an output-bearing Blueprint Interface function reuses the function graph already owned by `ImplementedInterfaces` instead of creating an event or a duplicate ordinary function graph. If `inputs` or `outputs` are supplied explicitly, their names and types must form an exact case-insensitive match within their own direction; property order does not matter, and an input can never satisfy an output.

### blueprint.add_macro

Creates an empty Blueprint macro graph, registers it in `MacroGraphs`, and returns the graph plus tunnel node IDs for follow-up `blueprint.graph.create_node` calls.

Signature:

```json
{
  "method": "blueprint.add_macro",
  "params": {
    "path": "/Game/Blueprints/BP_MyActor",
    "macroName": "ValidateInput",
    "inputs": [{ "name": "Value", "type": "float" }],
    "outputs": [{ "name": "Result", "type": "bool" }],
    "execExits": ["Completed", "Failed"]
  }
}
```

Aliases: `blueprintPath` for `path`, and `name` for `macroName`. `inputs` are added to the entry tunnel as output pins; `outputs` are added to the exit tunnel as input pins. When `execExits` is present, the entry tunnel gets an exec output named `execute`, and the exit tunnel gets exec inputs named exactly from `execExits`. Omit `execExits` for a pure macro.

The returned `inputs`, `outputs`, and `execExits` are read back from the reconstructed tunnel pair's physical pins. The data arrays exclude the `execute` entry and all exit exec pins; `execExits` is the separate list of physical exec pins on the exit tunnel. Duplicate data names or a data-pin collision with `execute` or an exit name fail with `PIN_CREATION_FAILED` and roll back the new macro graph.

Returns `graphName`, `entryNodeId`, `exitNodeId`, `saved`, the measured Blueprint compile fields (`compiled`, `status`, `errors`, `warnings`), and the standard asset verification fields. The response's `success` matches `compiled`, and saving is attempted only after a successful compile. Use `graphName` as the `blueprint.graph.create_node` scope when adding body nodes.

Macro `inputs` and `outputs` use the shared `{name,type}` parser with wildcard fallback: invalid type strings are logged and produce wildcard tunnel pins instead of failing the request. Event dispatcher params deliberately use the same parser in strict mode, so do not copy macro fallback behavior into signature APIs.

### blueprint.compile

Reports the BP compiler's **actual** status, not an unconditional success flag. This matters because BPIR compilation and UE's BP compilation are distinct steps: BPIR only creates graph nodes, while `FKismetEditorUtilities::CompileBlueprint` validates them against the class. Always call this after `blueprint.compile_bpir` or any graph-mutating sequence — see [`blueprint.bpir-gotchas`](blueprint.bpir-gotchas.md).

Response fields:

| Field | Description |
|---|---|
| `compiled` | `true` only when `UBlueprint::Status` is `BS_UpToDate` or `BS_UpToDateWithWarnings`. Any other status (including `BS_Error` and `BS_Unknown`) yields `false`. |
| `status` | One of `"UpToDate"`, `"UpToDateWithWarnings"`, `"Error"`, `"Unknown"`. |
| `errors` | Array of `{message}` objects from `FCompilerResultsLog` errors. |
| `warnings` | Array of `{message}` objects from `FCompilerResultsLog` warnings. |

**Compiling a Widget Blueprint whose UMG Designer is open is safe as of 2026-09-03, and it was not
before.** Every PinWright compile route reaches `FKismetEditorUtilities::CompileBlueprint` directly,
one layer below the toolkit — so nothing destroyed the live Designer preview first, which is the
first thing `FWidgetBlueprintEditor::Compile` does. The preview then survived into the class rebuild
and the editor died on the next ordinary Slate paint, in `UUserWidget::RebuildWidget` dereferencing
a `WidgetTree` the compile had nulled. A pre-compile hook on the engine's own
`OnBlueprintPreCompile` broadcast now tears that preview down for **every** compile route (this
verb, `blueprint.compile_bpir`, `blueprint.add_variable`, the BPIR emitters, engine paths such as
Compile All), and the toolkit rebuilds it on its next tick. No caller action is required and there
is no `EDITOR_OPEN` refusal: the visible effect is the Designer preview rebuilding, exactly as it
does when a human presses Compile.

`blueprint.compile` is compile-only — it validates the graph in memory and never writes to disk. To persist a successfully compiled Blueprint, call [`asset.save`](asset.md) afterward; it runs the same Blueprint integrity gate at the save choke point and refuses to write a corrupt graph. (The former `saveAfterCompile` flag was removed; persistence and its integrity gate now live on the universal save path.) Callers must check `status` (or the `errors` array), not just `compiled`, to detect genuine failures — earlier behavior reported `compiled: true` unconditionally and masked BP compilation errors.

### blueprint.search

Searches Blueprint nodes, variables, functions, events, pins, and comments using UE's built-in Find-in-Blueprints index. No scan step is required — the index is maintained by the editor. Parameters:

- `query` — the keyword (free text).
- `path` — a **result filter, not a cost control** (default `/Game`). Find-in-Blueprints always searches its whole index and matches outside `path` are dropped afterwards, so narrowing it returns fewer results but does not shorten the corpus walk or the `autoIndex` wait. The one exception: a path containing no Blueprints at all is answered immediately, without touching the index.
- `filter` — narrows to a category: `All`, `Nodes`, `Pins`, `Properties`, `Variables`, `Components`, `Functions`, `Macros`, `Graphs`.
- `limit` — caps results (default 50).
- `autoIndex` — wait for the index to finish building before searching (default true). This is the expensive half of the call and the only parameter that changes what it costs: the wait scales with the size of the content tree and dominates a first-of-session call on a cold project. It stays the default because it is passive — the editor keeps ticking and other calls are still serviced — and bounded by `timeoutSeconds`. Pass `false` for the cheap path: it answers from whatever Find-in-Blueprints already holds and never waits, at the price of an answer over a possibly incomplete corpus. When it is off and assets remain unindexed, `message` says so and names the flag.
- `timeoutSeconds` — total budget for the autoIndex wait **and** the search worker together (default 100, clamped 1-600). The default leaves headroom under the transport's 120 s response deadline.

**Read the disclosure fields before trusting the count.** Every response from a searched path carries `timedOut`, `searchRan`, `indexInProgress` and `searchElapsedSeconds` unconditionally, so "few results" is never indistinguishable from "gave up early":

- `timedOut: true` — the answer is incomplete. `searchRan: true` means the worker was cut short (`searchPercentComplete` says how far it got); `searchRan: false` means the index wait used the whole budget and no worker ever started (`indexTimedOut: true`).
- `indexInProgress: true` with `timedOut: false` — the search finished, but assets indexed after the call could not have matched. Re-run for a complete answer; `indexProgress` is the fraction done.
- `unindexedCount` / `unindexedAssets` — how much of the corpus was not searchable. `unindexedCount` is what the worker saw out of date; `unindexedAssets` is the manager's outstanding total. Either being non-zero means the answer covers a partial corpus even when `timedOut: false`.

**Field presence is not uniform — probe rather than assume.** `indexProgress` appears only while `indexInProgress: true`. `indexWaitSeconds` and `indexTimedOut` appear only when an `autoIndex` wait actually happened. `searchTimeoutSeconds` appears only on `timedOut: true`, and `searchPercentComplete` only when a worker also ran (it is omitted rather than zeroed when the index wait consumed the budget). The no-Blueprints-in-scope early-out returns a reduced shape: `timedOut` and `indexInProgress` are present, `searchRan` and the index-wait fields are not.

The call does not hold the editor. The first search of a session on a cold project starts the Find-in-Blueprints index and then waits for it *passively* — the index keeps building on the editor's own tick whether or not you wait, so a `timedOut` first call followed by a second call is a normal and cheap way to get a complete answer.

For non-BP assets reach for `call("asset.search")` instead. For deep multi-asset audits, prefer `asset.dump_folder` and grep the cached `bpir.txt` files.

Tokenization quirk: a query for a full CamelCase identifier (e.g. `GetEffectiveOnlineMode`) can return 0 matches even when a node with that exact `nodeTitle` exists. The matcher works on terms; full identifiers may not register as a single term. Verify by querying partial terms (`Effective`, `Online Mode`) before concluding the node is absent.

### blueprint.build_api_index

Reflection-based scan of Blueprint-callable UE functions. Pass `classFilter` to scope the scan to specific classes (much faster than a full scan); omit it to index all classes (large output).

`classFilter` uses UE class names **without prefix** — `"Actor"` not `"AActor"`, `"ActorComponent"` not `"UActorComponent"`. UE's `GetName()` strips the standard prefix. The plugin filter does an exact post-strip name match.

### blueprint.search_api

Keyword search over the API index built by `blueprint.build_api_index`. **Only covers classes from the last `build_api_index` call.** If the last `build_api_index` filtered to `["Actor"]`, then `search_api` only finds `AActor` methods — not `UCharacterMovementComponent`, even if you searched there in a previous session. Re-run `build_api_index` with the right `classFilter` before searching for new classes.

Typical chain for code-gen:

```
1. blueprint.build_api_index(classFilter=["Actor", "CharacterMovementComponent"])
2. blueprint.search_api("get actor location")  -> find correct function signatures
3. blueprint.compile_bpir(assetPath, code)     -> compile to nodes
```

### blueprint.references

Asset-graph reference query. Combined with UE 5.6's `PropertyRedirects` and `FunctionRedirects` in `Config/DefaultEngine.ini`, this is the right pre-rename safety net for BP -> C++ migrations:

1. Before renaming, inspect the BP function's return pin name and all output parameter names via `blueprint.decompile` or `blueprint.graph.get_pin_details`.
2. Run `blueprint.references` to enumerate every asset that depends on the symbol you're about to rename.
3. After the C++ rename lands, add `+PropertyRedirects` / `+FunctionRedirects` lines under the relevant `CoreRedirects` section.
4. Open and resave each dependent Blueprint to confirm no broken pins remain.

Caveat: `PropertyRedirects` reliably rewires **return value pins**, but is unreliable for **UFUNCTION input-parameter pin names on call-site nodes** — especially when the pin value is a non-default literal. For risky migrations, keep a temporary wrapper with the old param name that forwards to the new implementation, migrate callers in a follow-up, then remove the wrapper. Use `blueprint.references` to enumerate callers up front; do not rely on redirects alone for input-param renames where a specific pin default is the only thing preventing a silent behavior change.

`CoreRedirects` `PropertyRedirects` match BP variable references by exact `FName` from the `.uasset` — case-insensitive but **NOT space-normalized**. A BP variable's display name shows with spaces ("Online Mode") but the stored `FName` can be with-spaces or without depending on how it was originally declared. When migrating a BP-declared variable to a C++ `UPROPERTY`, add BOTH spaced and unspaced `OldName` variants to catch all references.

**Filter case, and what the response tells you about it.** `caseSensitive` governs `targetPath`
**and** `nodeType`, on the substring form as well as under `exactTarget`. It defaults to `false`,
so `targetPath:"sm_"` also matches `.../SM_Wall` and `.../Prism_Wall` alike; pass `true` when you
are matching a case-bearing naming convention. It was a no-op on the substring path until it was
fixed, so a response that carries no `caseSensitive` field came from a build that ignored the
flag — the field is echoed whenever either filter is active, and reading it is how you prove
which semantics produced the reference set you are about to act on.

`caseSensitive` with neither filter, and `exactTarget` with no `targetPath`, are refused with
`INVALID_ARGUMENT`. A modifier with nothing to modify returns every reference while looking like
a filtered answer, which is the failure the echo exists to make visible.

These are not [`actor.list`](actor.list.md)'s `matchMode` knobs: this verb carries two independent
patterns under one shared case modifier plus its own `exactTarget` switch, so it keeps its own
wire shape while sharing the matcher underneath.
