# niagara.graph

Focused reads for Niagara systems, emitters, and standalone scripts: graph metadata, nodes, pins, links, and function-call script refs. Use `call("niagara")` for the full read/edit/validate surface.

## Usage

Example:

```json
{
  "assetPath": "/Game/FX/NS_Fire",
  "emitter": "Smoke",
  "scriptUsage": "ParticleUpdate"
}
```

Standalone `UNiagaraScript` assets include modules, functions, and dynamic inputs. They return `assetKind: "NiagaraScript"` and expose one script graph in the same `graphs[]` shape as system/emitter reads. `scriptUsage` still filters them: a module accepts `"Module"` or `"ModuleScript"`; a mismatch succeeds with `count: 0`.

Before graph edits, use `niagara.inspect`, `asset.dump`, or `niagara.graph.get` to obtain current node ids, pin names, function-call scripts, renderer indexes, and parameter types. Grouped graph operations and `niagara.apply_patch` are not accepted; each edit RPC accepts one graph or stack mutation.

## See also

- [`niagara`](niagara.md) for the read-first Niagara asset workflow, top-level edit RPCs, imperative node-creation sequence, script-swap mechanics, and override-node internals.
- [`asset`](asset.md) for `asset.dump` and cached Niagara graph dump files.
- [`blueprint.graph`](blueprint.graph.md) and [`material.graph`](material.graph.md) for the analogous node-graph model in other asset domains.

## Cross-cluster overlap

Same node-graph model as `call("blueprint.graph")` and `call("material.graph")`: nodes are addressed by id, pins by name, and connections are validated by the graph schema.

### niagara.graph.search_ops

Searches the Niagara op catalog by name, category, and keywords. The optional `limit` defaults to 50 and follows PinWright's integer coercion (numeric strings and booleans are accepted; fractional numbers truncate toward zero). The resulting integer must be non-negative. A limit of `0` returns no rows while preserving `totalMatches`; values above 500 are clamped to 500. Negative values are rejected with `INVALID_ARGUMENT` before the op catalog is read.

### niagara.graph.create_node

For `nodeClass: "NiagaraNodeOp"`, `payload.opName` accepts either `niagara.graph.search_ops` form: bare `opName` (`"Mul"`) or qualified `signature` (`"Numeric::Mul"`). A bare leaf is canonicalized to the engine's `Category::Leaf` key, so both forms resolve to the real op and its pins (for `Mul`, A/B/Result). An unknown op returns `INVALID_OP`. Previously, storing the bare leaf verbatim created a pinless `Unknown` node but reported success.

A `nodeClass` outside the v1 list is refused with `UNSUPPORTED_NODE_CLASS` before anything is
constructed — no transaction, no `Modify()`, no half-built node — so the graph is untouched by a
rejected request. A rejected `payload` (for example an unknown `opName`) is caught after the node
exists, because pin allocation depends on the applied fields; the node is finalized and then
removed, leaving the same net-zero result. `ResolveNiagaraSubclassByPath` accepts any `UNiagaraNode`
subclass, so the class gate is what separates a resolvable class from a supported one.

### niagara.graph.connect_pins

Wires two script-graph pins through the Niagara schema. The optional `scriptType` target is `Spawn` by default when omitted or empty (including whitespace-only input); a supplied value is matched case-insensitively against only `Spawn` and `Update`. Any other value returns `TARGET_NOT_FOUND`, includes the requested value and valid candidates `Spawn` and `Update`, and is rejected before node or pin lookup, schema work, transactions, notifications, or mutation.

Node lookup is GUID-first: a value that parses as a GUID first matches `NodeGuid`, and a real GUID match wins. If no node has that GUID, exact UObject-name/display-title alias matching is attempted as well; UE accepts 22-character Base64 short GUID strings, so a GUID-looking alias can reach this fallback. A non-GUID value follows the same exact alias matching, but it must match exactly one node. Zero matches return `NODE_NOT_FOUND`; multiple matches return `AMBIGUOUS_NODE` with every candidate GUID, before pin lookup or schema work. On success, `scriptUsage` is the canonical resolved graph usage (for example, `SystemSpawn` or `SystemUpdate`), and `fromNode`/`toNode` are the resolved node GUIDs; `fromPin`, `toPin`, and `connected` retain their existing meanings.

Spawn and Update scripts normally share one Niagara graph. After identity resolution, each endpoint is therefore scoped by output reachability: the node must be upstream of the output matching the selected script usage and usage ID. A node reachable from both outputs is valid for either target, while a disconnected node remains valid so newly-created nodes can be wired. If a node is reachable only from other outputs, the request returns `TARGET_NOT_FOUND` naming the requested canonical usage and all of the node's reachable usages, before pin lookup, schema work, notification, or mutation.

A refused wire returns `CONNECTION_DISALLOWED`, the schema reason (for example, "Types are not compatible"), and both pin names/types rather than an opaque generic failure. A Map Set `+` pin (`DynamicAddPin`) accepts a direct wire only from a **concretely-typed** output; the engine then materializes a named parameter entry and links it as editor drag-onto-`+` does. A generic-numeric source, such as an `Multiply` `Result` whose type is not inferred, is rejected until its type resolves. The singular `niagara.connect_pin` reports the same schema reason.

### niagara.graph.remove_node

Removes one node from the selected script graph by its GUID. It uses the same optional `scriptType` contract as `niagara.graph.connect_pins`: omitted or empty means `Spawn`, `Spawn` and `Update` are accepted case-insensitively, and every other supplied value returns `TARGET_NOT_FOUND` with the requested value plus valid candidates `Spawn` and `Update` before the node is resolved or removed. Pass the node GUID returned by the graph read; names and display titles are not accepted by this verb. A successful response includes canonical `scriptUsage` and the removed node's canonical `nodeId` GUID.

GUID removal uses the same output-reachability scope as `connect_pins`. Shared nodes are removable through either matching target and disconnected nodes remain admissible; a node owned by another reachable usage returns `TARGET_NOT_FOUND` with the requested and actual usage list before removal or graph notification.
