# blueprint.graph

Low-level editor for surgical changes to an existing Blueprint graph: create nodes, connect pins, set defaults, find orphaned wires, and list node types. For whole-graph authoring, use the text-IR compiler first and return here for fixups.

## Cross-cluster overlap

Graph node creation requires a K2Node class and live pin names. `blueprint.graph.list_node_types` enumerates the class catalog; after creation, `blueprint.graph.get_node_details` (or `_batch`) returns the pin shape for the next `connect_pins` call.

`list_node_types` lists the K2Node *container* class, not which UFunction a `CallFunction` targets — see [Why two-tier](#why-two-tier-nodetype--target-for-bp-nodes). For the common arithmetic/comparison operators the `target` is a `KismetMathLibrary` function named `<Op>_DoubleDouble` over `A`/`B` -> `ReturnValue` pins: `-` -> `Subtract_DoubleDouble`, `<=` -> `LessEqual_DoubleDouble`, and likewise `+`/`*`/`/`/`>=`/`==`/`!=`. The one exception is `print` -> `KismetSystemLibrary::PrintString` (pin `InString`). The [operator cheat-sheet](#operator-cheat-sheet-callfunction-targets) below tabulates all of them so you can skip discovery entirely.

For any operator or function **not** in that cheat-sheet, don't read engine headers and don't guess the K2Node name: run `blueprint.build_api_index(classFilter=["KismetMathLibrary","KismetSystemLibrary"])` then `blueprint.search_api("<verb>")` — search the **plain verb** (`add`, `less`, `print`), not the operator symbol, and target the returned UFunction from a `CallFunction` node.

BPIR insertion helpers live on `call("blueprint")`, not this low-level `blueprint.graph` branch:

- `blueprint.insert_bpir_at_node` inserts headless BPIR after an existing node. Use body code only, without an `entry ... {}` wrapper.
- Headless inserted BPIR follows the same position rules as full `compile_bpir`: no primary positions means auto-layout, all primary positions means authored coordinates are preserved, and mixed primary positions fail.
- `execPin` targets a specific exec output on multi-output nodes; existing downstream exec links are preserved and reattached after the inserted code.
- Sequence nodes auto-create missing `then_#` exec pins up to `then_15`; both `then_4` and `Then 4` forms are accepted.
- `blueprint.insert_bpir_before_node` inserts before a target node by routing through its upstream exec connection. It cannot target entry/event nodes with no upstream exec pin.
- `blueprint.get_node_connections` returns exec-only upstream/downstream topology and is the quickest way to choose a safe insertion anchor.
- `context` maps `$Name` references in inserted BPIR to existing pins by `"nodeGuid:pinName"`, and anchor node output parameters are available by bare name.
- Object return references support `%ref.Property` access such as `%target.Health`.

For full BPIR syntax and examples, use `call("bpir")` for the topic index, `call("bpir.instructions")` for syntax, and `call("bpir.examples")` for worked patterns.

## See also

- **Bulk authoring / BPIR syntax** — use `call("blueprint.compile_bpir")` for whole graphs, then return here for fixups. The IR + compile loop can auto-layout unpositioned bodies and supports authored `@(x, y)` suffixes when every visible primary node in an entry body needs fixed coordinates; see `call("bpir")`, `call("bpir.instructions")`, and `call("bpir.examples")` before replacing imperative calls.
- **Read before editing** — use [`blueprint.inspect`](blueprint.inspect.md), [`blueprint.decompile`](blueprint.decompile.md), or cached `bpir.txt` from [`asset.dump`](asset.dump.md) to understand existing graph logic before direct node edits. Use this page when you need exact nodes, pins, GUIDs, graph connections, execution flow, or orphan analysis.
- **Widget graph logic** — UMG `widget_event` handlers and widget-variable references are still Blueprint graphs, but their names come from the widget tree. Inspect that tree through [`widget.describe`](widget.describe.md), [`widget.export_xml`](widget.export_xml.md), or cached `tree.xml` from [`asset.dump`](asset.dump.md) before creating direct nodes that target widgets.
- **Validation** — after edits, run `call("blueprint.compile")` (or rely on `compile_bpir`'s implicit compile) before treating the BP as ready.
- **Visual graph models elsewhere** — `call("material.graph")` and `call("niagara.graph")` follow the same low-level imperative shape for their respective domains; the patterns transfer.

## Standard exec pin names (skip discovery for vanilla exec wiring)

This is the published **standard exec-pin vocabulary**: for the most common wiring — an event/impure node's exec output into the next impure node's exec input — the exec-pin names are **fixed and universal** K2 schema constants, not asset-specific. You can pass them straight to `blueprint.graph.connect_pins` without a `get_node_details`/`_batch` round-trip first. (Casing doesn't matter: `connect_pins` resolves pin names case-insensitively — exact match first, then an `ESearchCase::IgnoreCase` fallback — so `then`, `Then`, and `THEN` all resolve to the same pin.)

| Pin role | Pin name | Where it appears |
|---|---|---|
| Standard exec **output** | `then` | `UEdGraphSchema_K2::PN_Then` — `BeginPlay`, `CustomEvent`, ordinary `CallFunction` / impure nodes |
| Standard exec **input** | `execute` | `UEdGraphSchema_K2::PN_Execute` — impure node exec entry (e.g. `PrintString`, `Delay`) |
| Sequence per-branch outputs | `then_0` .. `then_N` | `K2Node_ExecutionSequence` (`Then 0`/`then_0` both accepted; see auto-create note above) |
| Branch exec outputs | `then` (true) and `else` (false) | `K2Node_IfThenElse` — these are the **live** pin names |

So `BeginPlay.then -> PrintString.execute` and `CustomEvent.then -> Delay.execute` connect with no discovery call. A miss is also self-describing: a wrong pin name returns `PIN_NOT_FOUND` whose payload lists `availablePins`/`inputPins`/`outputPins`/`closestMatches`, so a single failed `connect_pins` hands back the live vocabulary — no separate discovery call is needed just to learn it.

**Branch caveat:** wire to `then`/`else`, **not** `True`/`False`. `True`/`False` are BPIR keyword aliases the compiler normalizes to `then`/`else` internally; `connect_pins` matches the literal live pin name and does not understand the aliases, so `connect_pins(fromPinName:"True")` on a Branch finds nothing.

**Exception — casts still need discovery.** Their success pin may vary by subclass/engine version, so call `blueprint.graph.get_node_details` first; struct members, custom-function parameters, and other data pins are variable too. See [Cast Node Pin Name Round-Trip Gotchas](#cast-node-pin-name-round-trip-gotchas).

## BPIR variable-set RHS and delegate handler shape

BPIR decompiler emits explicit pin-default literals — never the placeholder `?` — when a variable-set RHS pin has no incoming wire. `FormatPinDefaultLiteral` (in `BpirDecompiler.cpp` / `BpirTextEmitter.cpp`) returns the right literal per pin category: `nullptr` for object, `""` for string, `false` for bool, `0`/`0.0` for numeric, `None` for name, a struct-literal for struct, or `<unresolved>` as defense-in-depth. `EmitVariableSet` falls back to `<unresolved>` only if `FormatPinDefaultLiteral` returns empty.

Direct `CustomEvent` delegate links decompile to the `event:@<Handler>` shape — there is NO synthetic `$OutputDelegate` pin emitted from the producer side; the producer skips it before reaching the consumer, which avoids the brittle string parse-rejoin band-aid the early sketch used.

`cast<Unknown>` (target class unresolved) decompiles to a structured warning preserving the original AST shape rather than dropping the node silently. Orphan-warning text now carries `nodeId`, owning graph, and coords so the warning is distinguishable across files; use `FullTitle` (not `GetName`) for friendly node names in those diagnostics.

## UHT-stripped F prefix on delegate signatures

When a Blueprint references a `DECLARE_DYNAMIC_DELEGATE_RetVal` (e.g. `DECLARE_DYNAMIC_DELEGATE_RetVal(bool, FGetBool)`), UHT strips the leading `F` prefix on the generated `UFunction`. The resolved signature function is named `GetBool__DelegateSignature`, NOT `FGetBool__DelegateSignature`.

This matters for BPIR compile/decompile of delegate-property nodes that reference the signature function by name: emit and resolve the unprefixed form. The test `bpir.compiler.DelegateSignatureFunctionParamCompiles` covers this counterfactual.

## Pure input-key nodes need a Branch downstream for reachability

`WasInputKeyJustPressed` and similar input-key getters are **pure** nodes (no exec pin). When testing BPIR decompile/compile round-trip of wired-key output, a synthetic exec wiring from the input-key node to a downstream consumer is a no-op — the compiler prunes pure nodes whose return value is not consumed by a reachable impure node.

To make such tests counterfactual, feed the return value into a `Branch` condition pin (or any impure consumer's data pin). The reachability comes from the `Branch`, not from the input-key node itself. Tests `bpir.decompiler.input_key.{Direct,}WiredKeyOutputResolvesToLiteral` follow this pattern.

## What counts as an entry point (orphan sweep + execution flow)

Whenever these consumers analyze the same authored graph family, `find_orphaned_nodes`, `delete_orphaned_nodes`, and BPIR orphan warnings use one orphan model rooted at that family. An all-graph finder/delete scan combines the Blueprint's families; a named read-only finder and BPIR decompile select the named root plus its transitively nested authored child graphs, while named deletion mutates only the selected graph unless `includeNestedGraphs:true` opts into those child graphs (the response reports `nestedGraphsSkipped`). BPIR builds that same family before cloning/flattening the selected root graph. Each reachability key is `(authored graph, NodeGuid)`, so equal GUIDs in nested graphs remain distinct. Flattening changes emitted text only, while warning identities and graph names come from the authored source family. Its seeds are the engine's own compile root set — `UE::KismetCompiler::Private::GatherRootSet` with `bIncludeNodesThatCouldBeExpandedToRootSet=true`, the pre-expansion pass the Kismet compiler runs over the authored graph — plus macro entry tunnels:

- `K2Node_FunctionEntry`, `K2Node_Event` and every subclass (custom events, component/actor-bound events, legacy input events).
- Anything whose class overrides `UK2Node::IsNodeRootSet()`.
- **Any impure `UK2Node` with no input pins at all.** This is a shape test, not a class list, and it is what covers `K2Node_EnhancedInputAction` — the only way a Blueprint receives Enhanced Input, and a plain `UK2Node` rather than a `K2Node_Event`. Node classes from plugins PinWright does not link are covered by the same rule.

An impure node that *has* input pins (a stranded `PrintString`, an unwired `Branch`, an orphaned `FunctionResult`) is still swept. Comment, entry, and exit-tunnel nodes are structural boundaries, not orphan candidates in either consumer. Knots are connectors rather than boundaries: live exec/data knots are retained by the shared reachability sets, while `includeDataOnly:true` reports and deletes knots in unreachable data chains, including the unreachable knot portion of a mixed live/dead chain and fully disconnected knots. `includeDataOnly: true` extends the same exec result backward over linked non-exec input pins and treats macro exit tunnels as live data sinks, so disconnected pure/data-only nodes are reported by the finder. The backward data closure is demand-driven: `includeDataOnly: false` returns the cached node shape and exec reachability without constructing that closure, and a data-only request skips it when the family has no pure node with a linked data output; BPIR requests it only when its warning set includes such non-standalone pure candidates. Auto-play `K2Node_Timeline` roots are included by the orphan model; `get_execution_flow` resolves its latent roots separately — see its section below.

BPIR keeps one deliberate output-policy exception: a genuinely standalone pure node with no linked data output is emitted as a standalone statement for round-trip preservation and is explicitly excluded from both the finder and BPIR orphan-warning sets by the shared model. Knots are not expressions and cannot use that exception. Finder, deletion, and BPIR all decide membership through `FBlueprintOrphanReachability::IsOrphan`; BPIR warning identities remain graph-qualified, and consumers/tests normalize the identity when the warning renders the GUID with hyphens. Pure nodes that feed an unreachable downstream subgraph are omitted from the entry body and warned by both consumers. In an entryless pure-only graph there is no host entry to receive the preservation statement, so BPIR emits no entry block and the shared model returns no standalone-pure orphan; the finder likewise reports no orphan for that policy node when `includeDataOnly` is enabled. Other pure candidates, including unlinked getter/self nodes, remain subject to the data-only policy.

This is a source-level structural contract for the authored graph family. It does not claim runtime, saved-asset, or compiled-Blueprint verification.

## Cast Node Pin Name Round-Trip Gotchas

BPIR's documented cast syntax uses named exec exits — `cast<T>(%obj) [success -> @ok, fail -> @bad]`. On compile, the resulting `UK2Node_DynamicCast` node exposes its success exec pin under a different name depending on the cast subclass and engine version: pure-cast variants and some impure subclasses emit the pin as `then` (the K2-standard exec-output name) rather than `success`. The BPIR decompiler→compiler round trip is asymmetric here:

- **Decompiler** prints `[success -> @ok, fail -> @bad]` regardless of the underlying pin name (it normalizes to the BPIR keyword).
- **Compiler** wires `success -> @ok` to whichever exec output it considers the success pin — fine in fresh emits.
- **Surgical edits via `blueprint.graph.connect_pins`** that target a literal pin name `success` after a decompile/recompile will fail silently or attach to the wrong pin when the live pin is actually named `then`.

When hand-wiring around a cast result with `blueprint.graph.connect_pins`, call `blueprint.graph.get_node_details` on the cast node first and use the actual pin name returned. If `then` and `Cast Failed` appear instead of `success`/`fail`, wire to those. Re-emitting the cast through `blueprint.compile_bpir` (rather than imperative pin connects) avoids the issue because the compiler resolves the keyword internally.

**Symptom**

After `compile_bpir` followed by `connect_pins(... pinName: "success")` to attach downstream nodes, `find_orphaned_nodes` shows the downstream chain disconnected. The cast node has a wired `then` pin and an unwired (non-existent) `success` pin. Fix: re-emit the whole cast block via `compile_bpir`, or use the literal pin name from `get_node_details`.

## Why two-tier (nodeType + target) for BP nodes

BP node creation is the only graph domain here with a two-tier shape: `nodeType` selects a K2Node *container* and `target` supplies the UFunction, variable, or cast class. Materials, Niagara, anim graphs, and behavior trees are flat — the expression/node class is the operation and `add_node(type=ClassName)` plus reflected `properties` is the API. The split exists because K2Nodes are graph-editor wrappers around the underlying semantics:

- Most K2Nodes are not function-backed at all (`Branch`, `Sequence`, `ForEachLoop`, `MakeStruct`, `BreakStruct`, knots).
- The same UFunction can be wrapped by several K2Node classes with different pin layouts: `K2Node_CallFunction`, `K2Node_CallFunctionOnMember`, and a UHT-generated `K2Node_AsyncAction_<Name>` subclass per `Latent` UFunction. Function name alone is not enough to pick the node.

Discovery needs both halves, which answer different questions:

- `blueprint.graph.list_node_types` — node-class taxonomy (K2Node container catalog). Live, unranked, scoped to one BP asset. Use it for control-flow / structural nodes (Branch, Sequence, Cast, MakeStruct, ForEachLoop).
- `blueprint.search_api` — UFunction signatures with typed parameters, ranked by keyword score. Requires a prior `blueprint.build_api_index(classFilter=[...])`; stale or narrowly-scoped index produces false negatives. Use it for *which function to target* inside a `CallFunction`.

Canonical BPIR code-gen chain: `build_api_index` -> `search_api` -> `compile_bpir`; use `list_node_types` only when BPIR cannot express the node and you fall back to `blueprint.graph.create_node`.

The `nodeType` string is a UClass name: `"CallFunction"` maps to `UK2Node_CallFunction`, and `"Branch"` to `UK2Node_IfThenElse`. The handler also accepts `/Script/BlueprintGraph.K2Node_CallFunction`; it crosses the wire as a string because JSON-RPC has no first-class UClass reference. BPIR collapses both tiers for the common case — `Pawn->GetSpeed()` implies `CallFunction` + `GetSpeed` — which is why it is preferred.

## Operator cheat-sheet (CallFunction targets)

When you hand-build a math/compare graph with `create_node nodeType=CallFunction`, the human operator symbol is **not** the `target` — `target` is the underlying UFunction name. This table is the zero-discovery shortcut for the common cases. (Authoring a whole graph? Prefer `blueprint.compile_bpir`, which accepts the same names — see `bpir.examples`.)

The eight arithmetic/comparison rows below all live on `KismetMathLibrary` with the same `A`, `B` -> `ReturnValue` pin shape, so only the operator -> UFunction mapping varies:

| Operator | `target` UFunction |
|---|---|
| `+` | `Add_DoubleDouble` |
| `-` | `Subtract_DoubleDouble` |
| `*` | `Multiply_DoubleDouble` |
| `/` | `Divide_DoubleDouble` |
| `<=` | `LessEqual_DoubleDouble` |
| `>=` | `GreaterEqual_DoubleDouble` |
| `==` | `EqualEqual_DoubleDouble` |
| `!=` | `NotEqual_DoubleDouble` |

The one exception is `print` -> `KismetSystemLibrary::PrintString` (pin `InString`).

The `_DoubleDouble` suffix is the float (real) pair on UE 5; `float`-typed pins use the `<Op>_FloatFloat` variant (e.g. `Subtract_FloatFloat`) — same `A`/`B`/`ReturnValue` shape. For any operator or function not in this table, run `blueprint.build_api_index(classFilter=["KismetMathLibrary","KismetSystemLibrary"])` then `blueprint.search_api("<verb>")` rather than reading engine headers.

### blueprint.graph.create_node

Creates one node in an existing Blueprint graph and returns its node id. Use this for surgical edits; prefer `blueprint.compile_bpir` for full graph authoring.

**Params**

| Param | Type | Required | Notes |
|---|---|---|---|
| `assetPath` | string | yes | Blueprint asset path |
| `nodeType` | string | yes | Node type or common function shortcut |
| `graphName` | string | no | Graph name, defaults to EventGraph |
| `x` | number | yes | X position in the graph |
| `y` | number | yes | Y position in the graph |
| `target` | string | type-dependent | Type-aware target. Bare `"Foo"`, qualified `"Class::Foo"` / `"Class.Foo"`, class name for casts, or timeline variable name for `Timeline`. |
| `timelineName` | string | Timeline only | Legacy Timeline name alias; `target` takes precedence. If neither is supplied, the handler picks a unique Timeline name. |
| `inputAxisName` | string | InputAxisEvent only | Axis name for `InputAxisEvent` |
| `inputAction` | string | EnhancedInputAction only | Full `UInputAction` object path, for example `/Game/Input/IA_Move.IA_Move`. |

**Supported target-aware node types**

- `VariableGet` / `K2Node_VariableGet`
- `VariableSet` / `K2Node_VariableSet`
- `CallFunction` / `K2Node_CallFunction` / `FunctionCall`
- `Event` / `K2Node_Event`
- `CustomEvent` / `K2Node_CustomEvent`
- `Cast` / `CastTo<ClassName>` shorthand
- `Timeline` / `K2Node_Timeline` / `UK2Node_Timeline`
- `EnhancedInputAction` / `K2Node_EnhancedInputAction` / `UK2Node_EnhancedInputAction`

**`target` interpretation by type**

- **CallFunction / VariableGet / VariableSet / Event** — function, variable, or event name. Bare `"Foo"` infers from the Blueprint or the standard libraries where applicable. Qualified `"Class::Foo"` / `"Class.Foo"` pins the lookup.
- **CustomEvent** — bare custom function name. A class qualifier is ignored.
- **Cast / `CastTo<X>` shorthand** — class name only, qualified (`/Script/Engine.Actor`) or bare (`Actor`).
- **Timeline** — timeline variable name. The handler registers a backing `UTimelineTemplate`, assigns the node `TimelineName`, and returns `timelineName` plus `timelineTemplatePath`.
- **EnhancedInputAction** — `inputAction` is a full `UInputAction` object path. The handler loads and type-checks it, then binds it before pin allocation. Missing, unloadable, or wrong-class values return `INVALID_ARGUMENT`; an unavailable Enhanced Input node class or `InputAction` property returns `NODE_TYPE_NOT_FOUND`. These checks run before the transaction or any `Modify()` call, so a rejected request neither dirties the Blueprint or graph nor leaves a node behind.

Legacy aliases remain accepted when `target` is absent: `variableName` for variable get/set, `memberName` plus optional `memberClass` for function calls and events, `eventName` for events/custom events, `targetClass` for casts, and `timelineName` for timelines. Alias requests should continue to behave like the equivalent `target` request.

Timeline creation is an explicit `K2Node_Timeline` branch before the generic node-class fallback. The handler first verifies that the Blueprint supports timelines, then uses `target` or `timelineName` as the requested Timeline variable name. If neither is supplied, it asks `FBlueprintEditorUtils::FindUniqueTimelineName` for a fresh name; if an explicit name already has a backing template, the request fails with `DUPLICATE_TIMELINE` instead of silently reusing or shadowing it.

The branch registers the backing `UTimelineTemplate` with `FBlueprintEditorUtils::AddNewTimeline`, assigns `UK2Node_Timeline::TimelineName` before finalizing the node, sets the requested position, and returns both `timelineName` and `timelineTemplatePath`. It intentionally stops at empty Timeline node/template creation: track authoring, curve-key editing, length/loop/autoplay flags, and other Timeline template mutation remain out of scope for `blueprint.graph.create_node`. Use BPIR timeline support for scripted Timeline bodies that need track seeding.

**Examples**

Create a PrintString call with an explicit owner class:

```json
{ "assetPath": "/Game/BP_Foo.BP_Foo", "nodeType": "CallFunction", "target": "KismetSystemLibrary::PrintString", "x": 100, "y": 200 }
```

Create a variable getter:

```json
{ "assetPath": "/Game/BP_Foo.BP_Foo", "nodeType": "VariableGet", "target": "CurrentLap", "x": 300, "y": 200 }
```

Create a cast node:

```json
{ "assetPath": "/Game/BP_Foo.BP_Foo", "nodeType": "Cast", "target": "Actor", "x": 500, "y": 200 }
```

Create a Timeline node and backing template:

```json
{ "assetPath": "/Game/BP_Foo.BP_Foo", "nodeType": "Timeline", "target": "IntroFade", "x": 700, "y": 200 }
```

Create an Enhanced Input action event with asset-typed pins:

```json
{ "assetPath": "/Game/BP_Foo.BP_Foo", "nodeType": "K2Node_EnhancedInputAction", "inputAction": "/Game/Input/IA_Move.IA_Move", "x": 100, "y": 500 }
```

Timeline responses include the normal `nodeId`, `nodeName`, and `nodeClass` fields plus `timelineName` and `timelineTemplatePath`.

### blueprint.graph.set_node_property

Sets one presentation property on an existing graph node. This is a fixed whitelist, not a generic reflected property writer. The exact accepted `propertyName` values are `Comment`, `NodeComment`, `X`, `NodePosX`, `Y`, `NodePosY`, `bCommentBubbleVisible`, and `bCommentBubblePinned`. Pass the new value in `value`; `nodeId` is required and `graphName` is optional.

Shape-changing node properties are deliberately outside this verb. If another reflected property writer changes a K2 node property that determines its pins, call `blueprint.graph.reconstruct_node` afterward instead of reloading the asset.

### blueprint.graph.reconstruct_node

Safely refreshes one existing `UK2Node` in memory. It preserves the node identity, calls the engine's public `ReconstructNode()`, notifies the graph, and marks the Blueprint modified. It does not reload the asset. A missing node returns `NODE_NOT_FOUND`; a non-K2 graph node returns `INVALID_NODE_TYPE`. Both checks happen before the transaction or any `Modify()` call, so either rejection leaves the Blueprint and graph unchanged and clean.

```json
{ "assetPath": "/Game/BP_Foo.BP_Foo", "nodeId": "<guid>", "graphName": "EventGraph" }
```

The result includes `nodeId`, `nodeName`, `nodeClass`, `guidPreserved`, `pinCount`, and the normal asset verification object.

### blueprint.graph.replace_node

In-place node substitution that swaps one K2 node for another, transferring matched pin connections and unconnected default values inside a single `FScopedTransaction`. Mirrors UE's internal Convert-Event-To-Function pattern: spawn the replacement using the unified `target` string, refresh both pin sets, per-pin `MovePinLinks` on (name, direction)-matched pins, copy defaults on unconnected pins, then `RemoveNode` with `bDontRecompile=true` plus delegate-cascade and orphan-delta cleanup. Saves five+ round-trips versus the legacy delete + create + N connect + N default-set dance, and never skips the orphan/delegate bookkeeping that hand-rolled call sequences forget.

**Params**

| Param | Type | Required | Notes |
|---|---|---|---|
| `assetPath` | string | yes | Blueprint asset path |
| `nodeId` | string | yes | GUID or unique node name |
| `newNodeType` | string | yes | Replacement vocabulary — same as `create_node` |
| `target` | string | type-dependent | Type-aware target. Bare `"Foo"`, qualified `"Class::Foo"` / `"Class.Foo"`, or class name for casts. Omit for Branch/Sequence/Select/MakeArray. |
| `graphName` | string | no | Disambiguator. **Omit to scan all blueprint graphs** for a GUID match. |
| `pinRemap` | object | no | Map old pin names to replacement pin names before automatic matching, for example `{ "Condition": "Index" }`. Remapped pins must match direction and schema compatibility. |
| `allowOrphanPlaceholders` | bool (false) | no | Escape hatch for unmatched wired pins. By default unmatched wired pins fail the replace with `PIN_REMAP_INVALID`; set true to carry them to red placeholder pins. |

**Supported `newNodeType` vocabulary**

- `VariableGet` / `K2Node_VariableGet`
- `VariableSet` / `K2Node_VariableSet`
- `CallFunction` / `K2Node_CallFunction` / `FunctionCall`
- `Event` / `K2Node_Event`
- `CustomEvent` / `K2Node_CustomEvent`
- `DynamicCast` / `Cast` / `K2Node_DynamicCast` / `CastTo<ClassName>` shorthand
- `ClassDynamicCast` / `K2Node_ClassDynamicCast`
- `CreateDelegate` / `K2Node_CreateDelegate`
- `Branch` / `IfThenElse` / `K2Node_IfThenElse`
- `Sequence` / `ExecutionSequence` / `K2Node_ExecutionSequence`
- `Select` / `K2Node_Select`
- `MakeArray` / `K2Node_MakeArray`

**Generic fallback**

Explicit branches above keep their target-specific setup and should be used for configured nodes such as variables, function calls, events, casts, delegates, select, and make-array. Generic fallback is only for confirmed no-config-needed `UK2Node` classes:

- `UK2Node_IfThenElse`
- `UK2Node_Knot`
- `UK2Node_Self`
- `UK2Node_Copy`
- `UK2Node_GetArrayItem`
- `UK2Node_AssignmentStatement`
- `UK2Node_PureAssignmentStatement`
- `UK2Node_FormatText`
- `UK2Node_TemporaryVariable`
- `UK2Node_EaseFunction`
- `UK2Node_EnumEquality`
- `UK2Node_EnumInequality`
- `UK2Node_MakeContainer`
- `UK2Node_MakeSet`
- `UK2Node_MakeMap`

Generic replacements return `factoryPath: "generic"` and ignore non-empty `target` values, returning `targetIgnored: true` so callers can detect stale request shape. Config-required, abstract, deprecated, dead, non-`UK2Node`, and non-user-deletable classes are refused.

**`target` interpretation by type**

- **CallFunction / VariableGet / VariableSet / Event / CreateDelegate** — function or variable name. Bare `"Foo"` infers the owning class first from the old node (if it carries one), then from self (the BP's `GeneratedClass`), then from the standard libraries (`KismetSystemLibrary`, `GameplayStatics`, `KismetMathLibrary`, `KismetStringLibrary`, `KismetTextLibrary`). Qualified `"Class::Foo"` / `"Class.Foo"` pins the lookup.
- **VariableGet / VariableSet context** — a bare target preserves the old variable accessor's self/external context. A qualified target is self-context when its owner is the Blueprint class or one of its ancestors; an unrelated owner remains external. Self-context replacements hide the `self` pin and require no explicit self wire.
- **CustomEvent** — bare custom function name. Class qualifier is ignored (CustomEvent always lives on the BP). Existing `UserDefinedPins` are deep-copied so the new node owns its parameter list.
- **DynamicCast / ClassDynamicCast / `CastTo<X>` shorthand** — class name only, qualified (`/Script/Engine.Actor`) or bare (`Actor`). No `::` split.
- **Branch / Sequence / Select / MakeArray** — `target` is ignored.

**Pin migration**

Pin migration runs in three passes:

1. `pinRemap[OldPinName]` with direction and schema compatibility checks.
2. Exact old-name/new-name plus direction match.
3. First unclaimed same-direction new pin that the schema allows.

If a requested remap points at a missing replacement pin or an incompatible pin, the response includes `pinRemapUnmatched[]`. If any old wired pin is still unmatched after all passes, the default behavior is to cancel the replace with `PIN_REMAP_INVALID` and leave the old node in place. Setting `allowOrphanPlaceholders=true` creates a red, non-connectable placeholder pin on the new node and re-points the wire onto it; callers can then audit and either reconnect or `break_pin_links` to clean up. Pre-existing orphans on the old node follow the same flag.

**Refusal list**

Refused with `REPLACE_REFUSED`:

- Graph terminators: `UK2Node_FunctionEntry`, `UK2Node_FunctionResult`, `UK2Node_Tunnel`
- Composites & macros: `UK2Node_Composite`, `UK2Node_MacroInstance`, `UK2Node_MathExpression`
- Async / latent: `UK2Node_BaseAsyncTask`, `UK2Node_AsyncAction`
- Bound events: `UK2Node_ComponentBoundEvent`, `UK2Node_ActorBoundEvent`, `UK2Node_GeneratedBoundEvent`
- Input events: `UK2Node_InputActionEvent`, `UK2Node_InputKeyEvent`, `UK2Node_InputAxisEvent`, `UK2Node_InputAxisKeyEvent`, `UK2Node_InputVectorAxisEvent`, `UK2Node_InputTouchEvent`
- Navigation / inheritance: `UK2Node_CallParentFunction`
- Deprecated / dead: `UK2Node_DelegateSet`, `UK2Node_DeadClass`
- Any old node or generic replacement node that fails `CanUserDeleteNode()`

**Examples**

Swap a VariableGet to a different blueprint variable:

```json
{ "assetPath": "/Game/BP_Foo.BP_Foo", "nodeId": "K2Node_VariableGet_0", "newNodeType": "VariableGet", "target": "NewVariable" }
```

Swap a CallFunction with an explicit owning class:

```json
{ "assetPath": "/Game/BP_Foo.BP_Foo", "nodeId": "<guid>", "newNodeType": "CallFunction", "target": "KismetSystemLibrary::PrintString" }
```

Retarget an Event override to a different parent function:

```json
{ "assetPath": "/Game/BP_Foo.BP_Foo", "nodeId": "<guid>", "newNodeType": "Event", "target": "ReceiveBeginPlay" }
```

Replace any node with a plain Branch (no `target` needed):

```json
{ "assetPath": "/Game/BP_Foo.BP_Foo", "nodeId": "<guid>", "newNodeType": "Branch" }
```

Branch-to-Select with a wired condition pin fails by default if the old `Condition` link has no compatible automatic match:

```json
{ "assetPath": "/Game/BP_Foo.BP_Foo", "nodeId": "<branch-guid>", "newNodeType": "Select" }
```

That returns `PIN_REMAP_INVALID` and leaves the Branch in place. Use `pinRemap` when the destination pin is known:

```json
{ "assetPath": "/Game/BP_Foo.BP_Foo", "nodeId": "<branch-guid>", "newNodeType": "Select", "pinRemap": { "Condition": "Index" } }
```

Use `allowOrphanPlaceholders=true` only when preserving the visible broken wire is intentional:

```json
{ "assetPath": "/Game/BP_Foo.BP_Foo", "nodeId": "<branch-guid>", "newNodeType": "Select", "allowOrphanPlaceholders": true }
```

**Result shape**

```json
{
  "oldNodeId": "...", "oldNodeName": "...",
  "newNodeId": "...", "newNodeName": "...", "newNodeType": "K2Node_FormatText_C",
  "factoryPath": "generic", "resolvedClass": "UK2Node_FormatText",
  "connectionsRewired": 3, "connectionsDropped": [...], "defaultsTransferred": 2, "subPinsSplit": 0,
  "pinRemapApplied": 1, "pinRemapUnmatched": [],
  "targetIgnored": true,
  "orphanPlaceholdersCreated": [...], "cascadedCreateDelegatesRemoved": 0,
  "verification": {...}
}
```

`factoryPath` is `"explicit"` for the target-aware branches and `"generic"` for allow-listed no-config `UK2Node` classes. `resolvedClass` records the resolved replacement class. `pinRemapApplied` is returned when a request supplied a non-empty `pinRemap`; `pinRemapUnmatched[]` reports failed requested remaps with reasons such as `MISSING_NEW_PIN` or `TYPE_INCOMPATIBLE`. `targetIgnored` appears only when a generic replacement received a non-empty `target`.

On same-class + matching target, returns `{ "noop": true, "nodeId": "..." }` without opening a transaction.
For `VariableGet` / `VariableSet`, bare same-name targets preserve the existing context;
qualified same-name targets are a no-op only when the requested owner/context already matches,
so a context-changing retarget still runs.

**Cross-refs**

- For surgical creation, see [`blueprint.graph.create_node`](blueprint.graph.create_node.md) (same node vocabulary).
- For pure removal with orphan cleanup, see [`blueprint.graph.delete_node`](blueprint.graph.delete_node.md).

### blueprint.graph.get_graph_details

Returns the graph's `nodeCount` plus a per-node array. **The default (omit `includeNodeDetails`) is a light inline inventory** — each node is `{nodeId, nodeName, nodeTitle}` — so use it for counts and entry nodes. `includeNodeDetails:true` adds full pins and adjacency, reliably exceeding the inline budget and spilling to `HttpResponses/<uuid>.json` even for a small (~12-node) graph; use it only when pin detail is needed.

For a leaner read, pass **`namesOnly:true`** (`nodeId/nodeName/nodeType/nodeTitle/x/y`, no pins) or an explicit **`fields`** allow-list (for example, `fields:["nodeId","x","y"]`). These project each node to the selected keys and **take precedence over `includeNodeDetails`**; add `namesOnly:true` after an overflow to get an inline shape instead of rereading a spill file. The sibling `blueprint.graph.get_nodes` and `blueprint.graph.get_node_details_batch` accept the same `namesOnly`/`fields` levers and snake_case `names_only`.

| Param | Type | Default | Notes |
|---|---|---|---|
| `includeNodeDetails` | boolean | false | false = light `{nodeId,nodeName,nodeTitle}` per node; true = full pins + adjacency (heavy, can spill). |
| `namesOnly` | boolean | false | `nodeId/nodeName/nodeType/nodeTitle/x/y` per node, pins dropped. `names_only` accepted. Wins over `includeNodeDetails`; ignored when `fields` is supplied. |
| `fields` | array | — | Per-node key allow-list (`nodeId,nodeName,nodeType,nodeTitle,nodeComment,x,y,pins,nodeState`). A bare string is accepted. Wins over `namesOnly` and `includeNodeDetails`. |
| `includeConnections` | boolean | false | Also emit the `connections` / `connectionCount` edge payload. |

### blueprint.graph.get_execution_flow

`startNodeId` defaults to the first event / function-entry node. When a graph has **no** event or function-entry node but is driven by a latent exec-output-only root — the common case is an auto-play `K2Node_Timeline` whose `Update`/`Finished`/`Impact` exec outputs root the chain while its `Play`/`Stop` exec inputs are unwired — that root is auto-discovered: default-start, `entryPointsOnly`, and `includeAllEntryPoints` all resolve it instead of failing. Such inferred roots appear in `entryPoints` with the result flag `entryPointsAreLatentRoots: true` (declared event entries leave it `false`). A timeline that is *called* from an upstream event (its `Play` exec input wired) is **not** treated as a separate root — it is already reachable from its real entry, so it is not double-listed. If a graph has neither a real entry nor a latent root, the call returns `NODE_NOT_FOUND` with a message pointing you to pass an explicit `startNodeId` (locate one via `find_nodes` / `get_graph_details`).
