# ai

Create and configure AI assets, controller Blueprints, and AI-related components — perception setup, blackboard data, behavior-tree assignments, EQS queries, Mass entity config assets, Smart Objects, StateTree assets, and AI movement helpers. Use this namespace when the workflow spans several AI subsystems or connects AI assets to a controller or Blueprint; reach for `call("behavior_tree")` for all Behavior Tree graph authoring inside a Behavior Tree asset.

## Availability

The Smart Object verbs (`ai.create_smart_object_definition`, `ai.add_smart_object_slot`, `ai.configure_slot_behavior`, `ai.add_smart_object_component`) and Mass entity-config verbs (`ai.create_mass_entity_config`, `ai.configure_mass_entity`) are always registered. They resolve types by reflection, then gate at call time with `PLUGIN_DISABLED` naming **SmartObjects** or **MassGameplay** when that plugin is disabled; enable it, restart the editor, and retry.

## EQS authoring

Use `call("eqs")` for new Environmental Query System authoring. The old `ai.create_eqs_query`, `ai.add_eqs_generator`, `ai.add_eqs_context`, `ai.add_eqs_test`, and `ai.configure_test_scoring` forms are compatibility shims.

Generator and test edits must reach the runtime asset model: generators belong in `UEnvQuery::Options`, tests in each option's `UEnvQueryOption::Tests`; `eqs.add_generator` and `eqs.add_test` mutate those arrays and leave the package dirty unless `save=true`.

UE 5.6 `UEnvQueryTest` exposes scoring fields such as `ScoringEquation`, `ScoringFactor`, clamp types and values, and `ReferenceValue`; it does not expose an `FRuntimeFloatCurve ScoringCurve` field. `eqs.set_test_scoring` must reject `curve` input instead of silently accepting and dropping it.

## StateTree authoring

The `call("state_tree")` authoring surface works with native StateTree editor nodes, not UObject node classes. Evaluators, tasks, and conditions resolve `UScriptStruct` types with `ResolveUScriptStruct` and store node instances in `FInstancedStruct`-backed `FStateTreeEditorNode` entries.

Use `state_tree.add_evaluator`, `state_tree.add_task`, `state_tree.add_condition`, `state_tree.set_transition_trigger`, and `state_tree.bind_property` for native node, transition, and binding edits. They mutate public `UStateTreeEditorData` / `UStateTreeState` arrays inside scoped transactions and default to `save=false`, so several edits can be batched.

## Behavior Tree authoring

Use `call("behavior_tree")` for **all** Behavior Tree graph authoring, including `behavior_tree.create`. The remaining `ai.*` adders — `ai.add_composite_node`, `ai.add_task_node`, `ai.add_decorator`, `ai.add_service` — are a disabled/orphaning dead end and hard-error with `DEPRECATED_HANDLER`. `ai.add_composite_node` and `ai.add_task_node` accept no inputs because none can affect a disabled handler. Do not author through them.

Build every tree on `behavior_tree.*`: `behavior_tree.create` seeds `BTGraph` plus hidden Root and returns `rootNodeId` / `rootNodeName`, then use `add_node` → `connect_nodes` → `attach_decorator` / `attach_service` and read back with `decompile`. Only this surface produces a runnable tree. `ai.assign_behavior_tree` and `ai.assign_blackboard` still wire a finished tree and Blackboard onto a controller.

## Mass spawner authoring

`ai.create_mass_entity_config` and `ai.configure_mass_entity` genuinely author the
Mass Entity Config (MEC) asset, but **no `ai.*` verb wires a working spawner**.
`ai.add_mass_spawner` does **not** wire one: UE 5.7 Mass has no
`UMassSpawnerComponent`, so `blueprint.scs.get(componentClass=MassSpawner)` correctly
rejects it ("must derive from UActorComponent"). The verb persists nothing: it only
marks and saves the Blueprint, then echoes request params, so `spawnCount`/`configPath`
do **not** prove a spawner was wired. A Mass spawner is the **`AMassSpawner` actor**;
wire it on that actor's CDO:

1. `blueprint.reparent` the spawner Blueprint to `AMassSpawner`
   (`/Script/MassSpawner.MassSpawner`).
2. `blueprint.set_default {propertyName:"Count", value:<N>}` — the spawn count.
3. `property.set` on the Blueprint CDO (`.../BP_Spawner.Default__BP_Spawner_C`) with
   `EntityTypes[0].EntityConfig = <MEC asset path>` and `EntityTypes[0].Proportion = 1`.
4. Verify with `property.get` on `Count` and `EntityTypes[0].EntityConfig` — do **not**
   trust `ai.add_mass_spawner`'s echoed values.

## Asset names are bare names, not paths

`ai.create_blackboard_asset`, `ai.create_state_tree`, `ai.create_smart_object_definition` and `ai.create_mass_entity_config` compose `<path>/<name>` into a package path. `name` must be a **bare asset name**: a path-shaped one (`Sub/Leaf`, `/Game/X/Y`, `Trailing/`, `../Escape`) is refused with `INVALID_ARGUMENT`, as is a `path` that is not a valid long package name (unmounted root, `//`, backslash, `..`). Both refusals quote the engine's own reason. Put the folder in `path`; a trailing `/` on it is tolerated.

The refusal replaces a repair, deliberately: a `name` carrying `//` used to reach `CreatePackage`, which logs at **Fatal** on it — a verbosity that is not compiled out in any configuration — ending the editor process and losing every unsaved package in it.

## Watching a running AI vs. reading its assets

Every other verb on this page, and all of `call("behavior_tree")`, reads or writes **assets**. `ai.get_runtime_state` is the only one that reads the **controller executing them**, and it is PIE-only. Reach for it when an AI does the wrong thing in play — it is the supported answer to "which BT node is it in", "is path following moving, idle, or did it fail", and "what does the live Blackboard hold".

Do not substitute a Python probe for it. `BehaviorTreeComponent` exposes only `is_active` / `set_active` / `toggle_active` to Python — no active node, no debug string; `PathFollowingComponent.get_status()` and `has_valid_path()` are not exposed at all; and `AIController.get_move_status()` returns `-1`. The usual fallback, `GetCurrentAcceleration()`, reads `(0,0,0)` on a pawn that is plainly moving, because AI path following drives a character through `RequestDirectMove` (which sets `RequestedVelocity`) rather than through acceleration input — so that route does not just withhold the answer, it produces a confidently wrong one.

## See also

- [`behavior_tree`](behavior_tree.md) — the surface for all Behavior Tree graph authoring (`create` → `add_node` → `connect_nodes` → `attach_decorator` / `attach_service`) and BTIR decompile behavior; the `ai.*` BT-authoring verbs are deprecated/orphaning (see Behavior Tree authoring above).
- [`gas`](gas.md) for gameplay tag registry authoring and GAS asset wiring.

### ai.get_ai_info

For a `controllerPath`, returns only `{ "controllerClass": "<Controller>_C" }` — it reads the generated class name and **nothing else** off the controller: no AI Perception component, no `SensesConfig` (sight / hearing / damage tuning), no dominant sense, no perception team, no `DefaultBlackboard` / `DefaultBehaviorTree`. A thin payload here means "this verb does not report that state," **not** "the controller has no perception" — do not retry `get_ai_info` to confirm a write landed, the payload will not change. (The `behaviorTreePath` / `blackboardPath` / `queryPath` branches are richer — BT reports `behaviorTreeName` + `hasRootNode`, BB reports `keyCount` + `keys[]`, EQS reports `queryName` — but those describe the standalone asset, not how a controller wires it.)

To verify what the `ai` write verbs (`set_ai_perception`, `assign_blackboard`, `assign_behavior_tree`) persisted on the controller, read it back through one of these — and note the two CDO routes are **not** interchangeable per field:

- **Blackboard / Behavior Tree (scalar CDO props):** `property.get { objectPath: "/Game/.../BP_Foo.Default__BP_Foo_C", propertyName: "DefaultBlackboard" }` (or `"DefaultBehaviorTree"`) reads them off the controller CDO directly.
- **AI Perception component `SensesConfig`:** the perception template is a CDO **subobject**, and `property.get` on the `Default__BP_Foo_C:AIPerception` subobject path returns `OBJECT_NOT_FOUND` — that route does not resolve component subobjects. Read it via `blueprint.scs.get { assetPath, componentName: "AIPerception"|"AIPerceptionComponent", propertyName: "SensesConfig" }` (the SCS-template route), or spawn an instance with `actor.spawn_from_blueprint` and read `actor.get_component_property { componentName, propertyName: "SensesConfig" }`. `blueprint.inspect { assetPath, includeProperties: true }` also dumps the component template's CDO.

### ai.set_blackboard_value

Key metadata (type, sync flag) is available via `ai.get_ai_info` with a
`blackboardPath` (its `keys[]` payload), but that route does not report the
stored **default value**. To read back the default this verb wrote, go through
the generic reflection route: `property.get { objectPath:
"/Game/.../BB_Foo.BB_Foo", propertyName: "Keys" }` returns the `Keys[]` array;
index the matching entry and read `Keys[i].KeyType.DefaultValue` (Int / Float /
Vector / Rotator / Name / String) or `Keys[i].KeyType.bDefaultValue` (Bool). One
caveat: UE's serializer **omits** a value that equals its type default
(Int/Float `0`, Bool `false`), so an absent `DefaultValue` on that key means
"still the type default," not "unreadable" — only non-default values (e.g. a
Name default `"Sentry"`) appear.

### ai.configure_slot_behavior

Two preconditions on this verb are **not** derivable from the bare `behaviorType`
("Type of behavior") / `activityTags` ("Array of gameplay tag strings") param
descriptions — both are validated up front and reject the whole call, so authoring
order matters.

`behaviorType` accepts a **concrete `USmartObjectBehaviorDefinition` subclass**,
named by its `/Script/Module.Class` path (e.g.
`/Script/MassSmartObjects.SmartObjectMassBehaviorDefinition`, a sensible default
when MassSmartObjects is enabled) — *not* a free-form label. The base class is
abstract and every concrete subclass lives in an **optional** plugin
(`MassSmartObjects`, `GameplayBehaviorSmartObjects`, `GameplayInteractions`), so the
**loadable set depends on which plugins are enabled in this editor** and is not
fixed (resolution is by reflection, not linkage). An
unknown / unloadable value is rejected with `[INVALID_PARAMS]` whose
`availableBehaviorTypes` lists the classes currently resolvable on this editor — so
the **discovery move** is to pass a throwaway `behaviorType` once and read
`availableBehaviorTypes` from the error, then retry with one of those paths. (An
empty `behaviorType` is accepted and attaches no behavior; pass a value only when
you mean to attach one.)

`activityTags` must already be **registered** (e.g. via `gameplay_tags.add`) — an
unregistered tag does not silently drop: the call is rejected with `[INVALID_PARAMS]`
and a `droppedTags` array naming the unresolved tags. So the authoring order is
`gameplay_tags.add` → `configure_slot_behavior`.

### ai.add_mass_spawner

This verb does **not** wire a working Mass spawner — a success payload here is **not**
proof of a write, so do not retry it to confirm one. To actually wire a spawner, follow
the **Mass spawner authoring** section on the `ai` namespace page: `blueprint.reparent`
the Blueprint to `AMassSpawner`, then set `EntityTypes[0].EntityConfig` via `property.set`
on the CDO.

### ai.get_runtime_state

**PIE-only, read-only, one actor per call.** Outside a play session it refuses with `NOT_IN_PIE` rather than answering about the editor world's unpossessed placement copy of the same actor. Actor resolution is scoped to the PIE world and accepts either a possessed **Pawn** or the **AIController** itself (plus the `objectPath` / `actorPath` aliases).

Three refusals, kept distinct because the remedies differ:

| Code | Meaning | Remedy |
| --- | --- | --- |
| `NOT_IN_PIE` | No play session. | `editor.play`, then re-issue. |
| `NO_AI_CONTROLLER` | The actor exists in PIE but no `AController` drives it (an unpossessed Pawn, or a plain actor). | Name the controller, or possess the pawn. |
| `NO_BRAIN_COMPONENT` | A controller is there but carries neither a `UBrainComponent` nor a `UPathFollowingComponent`. | Nothing AI-shaped is running; assign a brain. |

A controller with **one** of the two is answered, not refused: an `AIController` driving a pawn with a bare `MoveTo` and no Behavior Tree is exactly the "is it stuck?" case this verb serves.

**Absence is stated, never zeroed.** Each of `brain` / `pathFollowing` / `blackboard` is an object carrying `present: true|false`; when false it carries `reason`. Inside a present section an unreachable field is an explicit JSON `null` with a sibling `<field>Reason` — so a null goal location means "there is no path", not "the goal is the origin". The same applies to `behaviorTree` inside `brain`: a non-BT brain (StateTree, a custom `UBrainComponent`) and a BT brain whose tree was never started are two different `present: false` reasons, not one empty payload.

What each section carries when present:

- **`brain`** — `componentClass`, `isRunning`, `isPaused`, `isBehaviorTree`, and for a BT brain `behaviorTree` with `currentTree` / `rootTree` / `activeInstanceIndex`, the engine's own `activeTrees` and `activeTasks` strings, `activeNode` (`name`, `class`, `executionIndex`, `treeDepth`, `staticDescription`, `isTask`, and `taskStatus` = `Active` / `Aborting` / `Inactive` when it is a task), and `activeNodePath` — the root-first chain of nodes down to the active one.
- **`pathFollowing`** — `status` (`Idle` / `Waiting` / `Paused` / `Moving`) plus `statusValue`, `hasValidPath`, `hasPartialPath`, `didMoveReachGoal`, `currentRequestId`, `acceptanceRadius`, `currentPathIndex` / `nextPathIndex`, `goalActorPath` (null for a location goal), and — only while a path is valid — `currentTargetLocation` and `pathEndLocation`.
- **`blackboard`** — `assetPath`, `keyCount`, and `keys[]` as `{name, type, value}`, where `value` is the same text the Gameplay Debugger shows.

`lastMoveResult` is **always** `null`. `UPathFollowingComponent` keeps the last `FPathFollowingResult` in a protected member and exposes no accessor; `didMoveReachGoal` is the engine's whole public readback, and deriving a result code from `status` would be a fabrication. Bind `OnRequestFinished` in game code if the result code itself is needed.

**Reading a suspected abort/restart loop.** One sample separates the two causes the position stream cannot: `pathFollowing.status == "Moving"` with a stable `currentRequestId` across samples is a pawn that is trying and not arriving (blocked capsule); a `currentRequestId` that changes every sample, or an `activeNode` that keeps returning to the same composite, is a decorator aborting and restarting the branch faster than `MoveTo` can finish. Sample one RPC per sample — `time.sleep()` inside `python.execute` blocks the game thread, so successive reads in one call return identical values.
