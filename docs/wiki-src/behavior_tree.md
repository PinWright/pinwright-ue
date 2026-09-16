# behavior_tree

Author and inspect Behavior Tree assets and their editor graph structure: create behavior trees, add task/composite/decorator-style nodes, connect or break graph links, remove nodes, set node instance properties, reorder a composite's children, and decompile Behavior Tree / Blackboard assets to BTIR text. Use it for direct Behavior Tree graph editing; reach for `call("ai")` for broader AI asset setup such as controllers, blackboards, perception, EQS, Mass, Smart Objects, or StateTree wiring.

## Root entry node

A freshly created Behavior Tree already contains a hidden **Root entry node** (seeded by the editor schema). For the tree to run, your top composite (e.g. the first Selector/Sequence) must be wired as a child of that Root — otherwise every node is orphaned and `decompile` reports `"Behavior Tree root has no child node."`.

`behavior_tree.create` returns the Root's addressable identity so you can connect to it immediately:

```json
{
  "assetPath": "/Game/AI/BT_Patrol.BT_Patrol",
  "name": "BT_Patrol",
  "saved": true,
  "rootNodeId": "<root guid>",
  "rootNodeName": "BehaviorTreeGraphNode_Root_0"
}
```

Pass either `rootNodeId` or `rootNodeName` as `parentNodeId` to `behavior_tree.connect_nodes` to link the Root to your top composite:

```json
{
  "assetPath": "/Game/AI/BT_Patrol.BT_Patrol",
  "parentNodeId": "<root guid>",
  "childNodeId": "<top composite guid>"
}
```

## Blueprint node classes

Use the typed-factory variants (`create_task_blueprint`, `create_service_blueprint`, `create_decorator_blueprint`) to mint custom Behavior Tree node Blueprint classes. Each accepts `name`, optional `savePath`, and optional `parentClass`. When `parentClass` is omitted, the handlers default to `UBTTask_BlueprintBase`, `UBTService_BlueprintBase`, and `UBTDecorator_BlueprintBase`.

```json
{
  "name": "BTTask_FindCover",
  "savePath": "/Game/AI/BehaviorTrees/Tasks"
}
```

After creation, add override logic through `call("blueprint")`, then place the class with
`behavior_tree.add_node` (tasks) or `behavior_tree.attach_decorator` /
`behavior_tree.attach_service` (subnodes). The factories only produce classes with the expected BT
base type; graph placement decides where they belong.

## Decorators and services

Use `behavior_tree.attach_decorator` and `behavior_tree.attach_service` to bind auxiliary nodes to an existing task or composite graph node. Pass `parentNodeId` as the target node GUID or name. Short native class names are accepted with or without their native prefix and resolve to the matching AIModule class before generic class-name lookup runs.

The decorator and service name sets are **not** symmetric. `attach_decorator` accepts `Blackboard`
(or `BTDecorator_Blackboard`) for `UBTDecorator_Blackboard`. There is **no** `BTService_Blackboard`:
`UBTService_BlackboardBase` is `UCLASS(Abstract)`, so use `DefaultFocus` (`BTService_DefaultFocus`)
for a blackboard-backed service; `RunEQS` (`BTService_RunEQS`) is the other stock concrete service.
Passing `Blackboard` to `attach_service` fails with `INVALID_CLASS`, whose error data lists concrete
services in `availableClasses`.

**The `properties` payload.**
`attach_decorator`, `attach_service` and `set_node_properties` apply `properties` through the same
reflection writer `property.set` uses, and report the outcome **per key**: `appliedProperties` on
success, `droppedFields` (`name` + `reason`) in an `INVALID_PROPERTY` error when any key does not
land. An attach that drops a key un-attaches the subnode again (`rolledBack: true`) rather than
leaving a decorator configured differently from what you asked for.

A `FBlackboardKeySelector` field — `BlackboardKey` on every blackboard-backed node — takes the key
name as a plain string:

```json
{"BlackboardKey": "bDwellStalled", "BasicOperation": "NotSet", "NotifyObserver": "ValueChange"}
```

The name is resolved against the tree's `BlackboardAsset` the way the BT editor does, so the runtime
key id and type are set, not just the name; a key the blackboard does not carry is a failed key whose
reason lists the available names. Give the **property** name, not the details-panel display name:
`BasicOperation`, not `Key Query`. A tree with no `BlackboardAsset` cannot take a key selector at all.

Decorator and service authoring must attach graph subnodes through `UAIGraphNode::AddSubNode`. In UE 5.6 that path triggers `UBehaviorTreeGraphNode::OnSubNodeAdded` and `UBehaviorTreeGraph::UpdateAsset`, which rebuild both the editor subnode arrays and the runtime Behavior Tree arrays. Do not create floating decorator/service graph nodes and do not bypass `AddSubNode`.

`AddSubNode` opens its own transaction in UE 5.6. The attach handlers should call `Modify()` on the Behavior Tree, graph, parent node, and subnode around the add, then use a separate scoped transaction only for post-add property or position edits. Wrapping `AddSubNode` in an outer `FScopedTransaction` risks nested transaction behavior that is hard to reason about.

## Node ids

Every `nodeId` / `parentNodeId` / `childNodeId` parameter takes the **graph node's GUID** — the
string `add_node` returns and the string `decompile` prints as the leading `nodeId:` field of every
node line. On a tree you did not build in this session, read the ids out of `decompile`; nothing
else has to be tracked between calls.

The same parameters also accept, in this precedence order, the graph node's object name
(`BehaviorTreeGraphNode_Task_3`), its full object path, and — only when none of those match — the
**node instance** object name (`BTT_Reload_C_0`), which is what asset dumps and BT runtime logs
print. A GUID always wins, so an id from `decompile` can never resolve to a different node.

## Child execution order

A composite runs its children **left to right by graph X position**, not by the order the links were
made. `behavior_tree.set_child_order` is the verb that changes that priority on a tree that already
exists; `connect_nodes` cannot, because a reconnected child keeps its old X.

```json
{
  "assetPath": "/Game/AI/BT_Patrol.BT_Patrol",
  "parentNodeId": "<composite guid>",
  "childNodeIds": ["<reload guid>", "<run eqs guid>", "<move to guid>", "<take cover guid>"]
}
```

A **Simple Parallel** parent is refused with `INVALID_ARGUMENT` and an `outputPinCount` of 2. It is the
one BT node with two output pins (`Task` and `Out`), the engine orders each pin's children
independently, and one flat list spanning both cannot express that — so the verb rejects rather than
silently reporting an order it did not change. Put the branch you want ordered under a child composite
and reorder that composite's children instead.

`childNodeIds` is the wanted order, highest priority first, and must list **every** current child of
the parent exactly once — a short, long, or duplicated list is rejected with the parent's
`currentChildOrder` so you can retry against the real membership. The handler rewrites the children's
X positions into their own existing X slots (the row's layout footprint is preserved) and then runs
`UBehaviorTreeGraph::RebuildChildOrder`, the same routine the editor runs when a node is dragged, so
the runtime child array is rebuilt and the new priority is live without a re-save or a re-open.

The response echoes the resulting order read back off the runtime composite, not off the request:

```json
{
  "parentNodeId": "<composite guid>",
  "childOrder": [
    {"nodeId": "<reload guid>", "name": "Reload", "x": -900, "y": 520},
    {"nodeId": "<run eqs guid>", "name": "Run EQS Query", "x": -700, "y": 520}
  ]
}
```

`name` is the same token `decompile` prints, and `x`/`y` match its `@(x, y)` suffix, so the echo lines
up directly against the BTIR you read the ids from.

## `behavior_tree.decompile`

Decompiles a `UBehaviorTree` or standalone `UBlackboardData` asset to BTIR:

```json
{
  "assetPath": "/Game/AI/BT_Patrol.BT_Patrol"
}
```

The response shape is:

```json
{
  "assetPath": "/Game/AI/BT_Patrol.BT_Patrol",
  "ir": "behavior_tree ...",
  "text": "behavior_tree ...",
  "warnings": []
}
```

Behavior Tree dumps write the same text to `btir.txt` beside `properties.json`; standalone
Blackboard dumps write `btir.txt` with only the `blackboard` block. BTIR is registered as the
decompile handler's text-IR sidecar, keeping dump baselines and live calls on one path. It is the
Behavior Tree sibling of BPIR (`call("bpir")`): same text-IR philosophy, distinct opcodes.

BTIR walks the editor `BTGraph`, exposing node positions/comments, root-level aux nodes,
disconnected nodes, decorators, services, and explicit `FBTDecoratorLogic`. Simple implicit-AND
decorators remain ordinary blocks; non-implicit logic emits `decorator_logic [Op:Number, ...]` next
to them.

Every line backed by a graph node — composites, tasks, attached decorators and services, the
composite-decorator wrapper, and the `root_aux` block — leads its parenthesised field list with
`nodeId: <guid>`, the graph node's GUID:

```
root composite Selector Selector @(0, 200) (nodeId: 3F2A...C1) {
  child task Reload BTT_Reload @(-900, 520) (nodeId: 8B41...07)
  child task MoveTo MoveTo @(-700, 520) (nodeId: D902...5E, BlackboardKey: CoverLocation)
}
```

That id is what every `nodeId`-keyed verb takes (`set_node_properties`, `set_child_order`,
`attach_decorator`, `attach_service`, `connect_nodes`, `break_connections`, `remove_node`), so a
`decompile` is the entry point for editing any tree, including one authored in another session or by
hand in the editor. Inner conditions of a composite decorator carry no `nodeId`: they live in a bound
sub-graph and are not addressable by those verbs.

BTIR is decompile-only. It is intended for compact review and diffing, not parse-back authoring.

## See also

- [`asset`](asset.md) for asset dump sidecar registration and diff-baseline behavior.
- [`ai`](ai.md) for broader EQS and StateTree authoring surfaces.
