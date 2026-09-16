# Runtime UObject inspection in PIE

End-to-end workflow for inspecting live UObjects in a running PIE (Play-In-Editor) session: world targeting, subsystem discovery, property reads, and method invocation. This is a topic page rather than a namespace overlay — reach it directly via `call("runtime-uobject-inspection")` (see [wiki](wiki.md#standalone-topic-pages) for how the router resolves topic slugs), or follow cross-references from [`system.inspect`](system.inspect.md), [`property`](property.md), and the `object.call_function` section of [`object`](object.md).

## Workflow at a glance

The recurring scenario is "I want to read or twiddle live runtime state on an object that isn't an actor and isn't in the editor world" — for example, confirming a config enum on a `GameInstance` subsystem mid-PIE. Treat the steps below as a pipeline; each one resolves the input the next one needs.

1. Resolve which world to query (editor vs PIE).
2. Discover the subsystem (or other UObject) you want to inspect.
3. Capture its live object path.
4. Read reflected properties from that path.
5. Call methods on it where reflection allows.

## Step 1: targeting the PIE world

Object discovery RPCs accept an optional `world` param: `editor`, `pie`, or `auto` (default). The handler resolves the world PIE-first when `auto` is in play — `GEditor->PlayWorld` if a PIE session is active, otherwise the editor world. Responses echo the resolved `world` and `worldPath` so callers can confirm which side they hit.

Affected methods:

- `system.inspect.list_objects`
- `system.inspect.find_by_class`
- `system.inspect.find_by_tag`
- `actor.list`
- `actor.find_by_class`
- `actor.find_by_tag`

The `actor.*` variants additionally iterate via `TActorIterator<AActor>(ResolvedWorld)` instead of the editor-bound `UEditorActorSubsystem`, so they enumerate PIE-side actors correctly.

## Step 2: discovering subsystems

`system.inspect.list_subsystems` enumerates live subsystem instances across scopes. Optional `scope`: `Engine`, `Editor`, `GameInstance`, `World`, `LocalPlayer`. Returned entries include `className`, `scope`, and the live `objectPath`. World/GameInstance/LocalPlayer scopes resolve the world via the same `editor|pie|auto` machinery as Step 1.

For singleton actors there are six dedicated read-only handlers:

- `system.inspect.get_game_instance`
- `system.inspect.get_game_mode`
- `system.inspect.get_game_state`
- `system.inspect.get_player_controllers`
- `system.inspect.get_player_states`
- `system.inspect.get_local_players`

Each returns `{objectPath, className}` (or a typed `*_NOT_FOUND` error) for singletons, or an array of those records for the multi-instance variants. Arrays come back empty when no world resolves.

## Step 3: the live subsystem object-path shape

The path shape produced for live subsystem instances is:

```
/Engine/Transient.<EditorEngine>:<GameInstanceName>.<SubsystemClassName>_<index>
```

Callers rarely need to construct this by hand because `list_subsystems` returns it directly. Copy the `objectPath` from the response and feed it into Step 4; the shape is here only to make response and log values less mysterious.

## Step 4: reading properties

`property.list` defaults to returning the full reflected `UPROPERTY` set, including non-editable fields. Pass `editableOnly: true` to restore the editor-style filtered view. Each entry carries a `flags` sub-object `{edit, blueprintVisible, editOnInstance, transient}` alongside the existing top-level fields.

`property.get` works on any reflected `UPROPERTY` by name regardless of editability — give it the live `objectPath` from Step 2 or Step 3 plus the C++ field name (case-sensitive, not the Blueprint display name).

`system.inspect.inspect_class` now also enumerates `properties[]`, `functions[]`, `interfaces[]`, and `inheritanceChain[]` for a resolved `UClass`. Reach for it when the question is "what members does this class expose?" before drilling into a live instance.

## Step 5: calling methods

`object.call_function` dispatches only `UFUNCTION`-decorated methods. Plain C++ methods on a subsystem are unreachable through this RPC — they are not present in UE's reflection database.

When you need to invoke a non-reflected method, fall back to `python.execute`. The Python UE bindings expose more of the C++ surface than the JSON-RPC layer can reach. Treat this as a last resort: write back a small JSON payload from Python so the caller side stays structured.

## Worked example: reading a reflected PIE subsystem property

Goal: confirm a reflected property on a live subsystem during a PIE session.

1. Locate the `GameInstance`-scoped subsystem list against the PIE world:

```
call("system.inspect.list_subsystems", { scope: "GameInstance", world: "pie" })
```

Response shape: `{ subsystems: [ { className, scope, objectPath }, ... ], world: "pie", worldPath }`.

2. Pick the desired `className` and copy its `objectPath`.

3. Read the property by name:

```
call("property.get", {
  objectPath: "<objectPath from response>",
  propertyName: "<case-sensitive C++ field name>"
})
```

Response shape: `{ value: "<value>", type: "<reflected type>" }` with `includeMetadata: true`; otherwise it returns just `value`.

If `property.list` is called on the same `objectPath` with default flags, `<PropertyName>` appears regardless of whether it is `EditAnywhere`. Pass `editableOnly: true` to drop hidden fields when asking what is visible in the details panel.

## Remaining caveats (post-sprint)

- `object.call_function` dispatches only `UFUNCTION`-marked methods. Fall back to `python.execute` for non-reflected member functions.
- The live subsystem object path under `/Engine/Transient.<EditorEngine>:<GameInstanceName>.<SubsystemClassName>_<index>` is undocumented in UE itself. `system.inspect.list_subsystems` is the supported way to obtain it; the shape note here is for log-reading, not hand-construction.
- `python.execute` is still the only route for invoking arbitrary non-reflected C++ methods on a live UObject. Inspection and reading are now reflection-only paths; method-side gaps remain.

## Cross-references

- [`system.inspect`](system.inspect.md) — namespace overview, including the editor-vs-PIE distinction.
- [`system.inspect.inspect_object`](system.inspect.md) — generic read-only UObject inspector; pairs with the `objectPath` from Steps 2–3.
- [`system.inspect.inspect_class`](system.inspect.md) — class-level enumeration of properties, functions, interfaces, and inheritance.
- [`system.inspect.list_subsystems`](system.inspect.md) — primary entry point for Step 2.
- [`property`](property.md) — reflection-based property accessors.
- [`property.list`](property.md) — discovery with `editableOnly` / `includeAll` and per-entry `flags`.
- [`property.get`](property.md) — single-property reader, works on any reflected `UPROPERTY`.
- [`object.call_function`](object.md) — UFUNCTION-only method dispatch.
- [`python.execute`](python.md) — escape hatch for non-reflected methods.
- [`workflows`](workflows.md) — the index of task-oriented guides this one belongs to.
