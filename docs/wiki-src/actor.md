# actor

Operate on placed actors in the active editor world: spawn, transform, query, tag, snapshot, attach and edit components. Use `actor.*` for instance state; use `call("blueprint")` and `call("blueprint.scs")` for Blueprint *class* templates (CDO defaults, SCS components and variables).

## Cross-cluster overlap

- **Read-only audit** — `actor.list`, `actor.find_by_class` and `actor.find_by_tag` have read-only twins under `call("system.inspect")` (`list_objects`, `find_by_class`, `find_by_tag`). Prefer those in audit/dry-run scripts; reserve `actor.*` for a following mutation.
- **Narrowing intent — pick the right query** — the queries narrow on different axes. **Name/label substring** → `actor.list filter=` or `actor.find_by_name`. **Class, including subclasses** → `actor.find_by_class` (a short class name such as `PostProcessVolume` resolves and matches subclasses; this is the real class filter). **Tag** → `actor.find_by_tag`. `actor.list` has no class param: a class-shaped intent belongs in `actor.find_by_class`, not `actor.list filter=`. `filter="PostProcessVolume"` only *coincidentally* lists those volumes because default `PostProcessVolume_*` labels contain the class string; relabel them and it silently returns the wrong set.
- **Persistent level state** — dump the UWorld with `asset.dump` or maps with `asset.dump_folder`, then inspect the `actors/manifest.json` mirror described in [`asset.dump`](asset.dump.md). `actor.describe` is the live equivalent of an `actors/*.json` row.
- **Components** — `actor.add_component` adds an *instance* component and is lost when its BP reconstructs. `call("blueprint.scs.add_component")` adds a *template* component so every instance gets it; they are not interchangeable.
- **Spawn** — `actor.spawn` handles UClasses, `/Script` paths, BP paths and mesh paths. For BP-only input prefer `actor.spawn_from_blueprint`, which resolves `GeneratedClass` and returns `CLASS_NOT_FOUND` for the wrong asset.

## actorName resolution & colliding labels

Every per-actor `actor.*` verb (`add_tag`, `remove_tag`, `set_transform`, `get`,
`get_transform`, `duplicate`, `delete`, …) takes a singular `actorName`. It accepts
these identifiers in this **precedence order**, checking every actor at each tier:

1. full **object path** (`/Game/Maps/Foo.Foo:PersistentLevel.PointLight_1`) — unique
2. **internal object name** (`GetName()`, e.g. `PointLight_1`) — unique per level
3. exact **display label** (`GetActorLabel()`, what the World Outliner shows) — **not unique**
4. display-label **substring** — not unique

Tiers 1–2 resolve exactly one actor. Tiers 3–4 may match several because display labels
are **not guaranteed unique**: duplicated `PointLight` actors can keep `TestPointLight`,
and `actor.spawn*` stores labels verbatim without uniquifying them.

**When a label matches more than one actor the verb fails with
`AMBIGUOUS_ACTOR_NAME` rather than picking one.** The payload lists each
`candidate` as `{label, name, path, class}`; `name` is the unique internal object
name, so the same call can be retried without a second query. Batch verbs such as
`actor.set_folder` put these in `ambiguous[]`, separate from `missing[]`.

Practical rule: **read by label, mutate by internal name.** `actor.list`, `actor.get`,
`actor.find_by_name` and `actor.describe` return both `label` and `name`; the other
two return the internal name as `objectName` (their `name` is the label). It is also
the **object-name leaf** of a `path` — `/Game/Maps/Foo.Foo:PersistentLevel.PointLight_1`
→ pass `PointLight_1`. `actor.set_label` changes only the label; the internal object
name cannot change after spawn.

For per-actor verbs that use the shared live resolver, a slash-prefixed exact object
path is looked up directly in the selected world before the editor actor subsystem's
filtered enumeration. Therefore the path from
`actor.find_by_class {className:"WorldSettings", world:"editor"}` can be passed to
`actor.get_components` (and the other resolver-backed actor verbs), including for
live WorldSettings, brush, info, hidden, and transient actors. Non-path lookups keep
the editor subsystem's normal filtering; all lookups remain scoped to the selected
world and keep the PIE-first fallback when no world is supplied.

`actor.select` and `actor.delete` also accept plural `actorNames` **arrays**; both
accept singular `actorName`, so `actor.select {actorName:"X"}` targets one actor and
`actor.select {actorNames:[]}` clears selection. `sequencer.*_actors` is array-only.

**Spawn-time label.** `actor.spawn`, `actor.spawn_shape` and
`actor.spawn_from_blueprint` take optional canonical `actorName` plus `label`, `name`
and `actor_name`; other spawners use the same aliases. Nothing uniquifies the label.
`actor.spawn_batch` is different: its per-placement label key is `name` inside
`transforms[]`.

## Dynamic mesh duplication

`actor.duplicate` on an actor carrying a `UDynamicMeshComponent` is guarded because the engine's copy path can fail **silently and successfully**: above `geometry.DynamicMesh.TextBasedDupeTriThreshold` (default 200000 triangles), or after `geometry.DynamicMesh.DupeStashTimeout` (default 300 s), `UDynamicMesh.cpp:568` emits a 12-triangle, 50-unit placeholder cube and reports success. Its only signals are a 5-second toast and `LogGeometry` warning; neither reaches an RPC caller.

The handler compares triangle counts across the copy instead of predicting either threshold:

- Counts match → unchanged success plus `sourceTriangles` / `duplicateTriangles`.
- Not probeable (no dynamic mesh component, or GeometryFramework not loaded) → **byte-identical to before**, no new fields. This is the ordinary-actor case.
- Counts differ → the duplicate is destroyed, restoring the exact pre-call world state, and the call returns `MESH_DUPLICATE_SUBSTITUTED` carrying `sourceTriangles`, `duplicateTriangles`, `actorName` (the **source** actor's label — the duplicate no longer exists) and a `remedy` string.

Two opt-outs, both `false` by default:

- `allowPlaceholderMesh` (boolean) — keep the cube and return success with `meshPlaceholderSubstituted:true` plus a `warnings` entry, for callers who genuinely want it.
- `allowSlowLargeMeshCopy` (boolean) — raise the triangle threshold above this mesh for one call (restored by a scope guard) so the duplicate succeeds. The Base64 text path is "quite slow" and O(mesh) in memory, hence opt-in. The response echoes `slowLargeMeshCopyApplied`; `false` means it was inert.

## See also

- [environment](environment.md) for typed sky, cloud, and reflection-capture actor spawns that return chainable actor paths.
- [property](property.md) for generic `property.get` / `property.set` follow-up on returned actor paths.

### actor.list

The narrowing params are `filter` (a **name/label pattern** — not a class filter), plus `matchMode` and `caseSensitive`. There is no class param; `classFilter`/`class` returns `[UNKNOWN_PARAMS] ... Valid parameters: [...]`.

#### `filter` matching semantics — read this before counting anything

`filter` matches **both** the display label (`GetActorLabel`) and internal object name (`GetName`); either match keeps the actor.

**The default is a case-INSENSITIVE SUBSTRING match.** Both halves of that surprise people, and both silently inflate counts:

```js
// The trap, reproduced from a real prefix-counting failure:
call({ method: "actor.list", args: { filter: "SH_", limit: 1, namesOnly: true } })
// -> totalMatches: 98, first match "Brush_0"
//    "Bru[sh_]0" contains a lowercase "sh_", and the default match is
//    case-insensitive AND unanchored. The true SH_-prefixed count was 4.
```

`totalMatches` is the full untruncated count, so a naming-prefix count can be well-formed but wrong. These two **opt-in** params fix it; omitting both preserves the legacy behaviour:

- **`caseSensitive`** (bool, alias `case_sensitive`, default **`false`**) — `true` stops `SH_` matching lowercase `sh_` in `Brush_0`.
- **`matchMode`** (string, alias `match_mode`, default **`"contains"`**) — `contains` (alias `substring`) matches anywhere; **`prefix`** (alias `starts_with`) anchors at the start; `exact` is full equality; `regex` is an unanchored ICU search, so anchor it with `^` when needed.

```js
// The correct prefix count:
call({ method: "actor.list", args: {
  filter: "SH_", matchMode: "prefix", caseSensitive: true, namesOnly: true
}})
// -> totalMatches: 4. "Brush_0" is excluded on both grounds.

// Equivalent via regex:
call({ method: "actor.list", args: { filter: "^SH_", matchMode: "regex", caseSensitive: true }})
```

With `filter`, the response echoes `filter`, `matchMode` and `caseSensitive`, proving which semantics produced `totalMatches`; an unfiltered call carries none of them.

A pattern that does not compile under `matchMode:"regex"` is rejected with **`INVALID_PATTERN`**, not treated as zero matches. An unknown `matchMode` returns `INVALID_MODE`; supplying `matchMode`/`caseSensitive` **without** `filter` returns `INVALID_ARGUMENT`.

The same three params are on [`system.inspect.list_objects`](system.inspect.md) and `system.inspect.find_objects_by_class`; `blueprint.graph.find_nodes` uses the same `matchMode` / `caseSensitive` vocabulary.

**`filter` is not a class filter.** A class-shaped intent belongs in `actor.find_by_class` (a short class name such as `PostProcessVolume` resolves the UClass and matches subclasses). `actor.list filter="PostProcessVolume"` only *coincidentally* returns those volumes because default labels contain the class string; relabel them and it returns the wrong set. Use `filter` for a label/name substring, such as `Trigger_`.

**A bare `actor.list {}` spills on any populated level.** It returns every actor as a four-field row (`label`/`name`/`path`/`class`); on a normal level the array exceeds the 10000-character inline budget and is written to `Saved/PinWright/HttpResponses/...` for `Read`/`Grep`. To keep a listing inline:

- **`limit`** — cap rows (e.g. `limit:20`); `0` returns all. `count` is returned rows; `totalMatches` remains full count and `truncated` exposes elision.
- **`namesOnly:true`** (alias `names_only`) — return only `label`/`name`/`class`, dropping `path`.
- **`fields`** — allow-list `label`/`name`/`path`/`class` (array or bare string), such as `fields:["label","class"]`, plus **`folder`**, which is returned *only* when named. It does **not** mirror `actor.describe`: that verb's key set is a strict superset (`level`, `guid`, `tags`, `transform`, `properties`, `components` on top of these), and asking `actor.list` for one of those is an error, not a wider row.

  Any `fields` entry outside `label`/`name`/`path`/`class`/`folder` is rejected with **`INVALID_PARAMS`** naming the offending entry and the valid set — the same courtesy `UNKNOWN_PARAMS` extends to top-level parameter names, which is all the dispatcher's gate can see. It is never silently dropped: a projection made only of unrecognised keys used to return rows that were empty JSON objects, which reads as "this actor has no such data" rather than "the verb refused to answer". `fields:["name","folder"]` is the cheap way to answer "which Outliner folder is this actor in" without `actor.describe`'s full property and component tree; `folder` stays off the default row so it does not widen a verb that already spills.

`filter` narrows best when you know a discriminating fragment; combine it with these levers and use `matchMode`/`caseSensitive` for naming-convention prefixes.

### actor.find_by_name

The search fragment goes in **`name`**; `pattern`, `filter`, `query`, `substring` and `search` are declared aliases. Because the summary and `name` help call the *value* a "substring", the most natural wrong guess is the key `substring`; it formerly failed as a missing `name` param. All five now resolve to `name`, so `pattern:"Campfire_Light"` and `name:"Campfire_Light"` are identical. `name` is canonical and the response echoes it as `query`.

`actorName` and `actorPath` are deliberately **not** aliases. They are identity slots for [`actor.get`](actor.get.md) and [`actor.describe`](actor.describe.md), where an ambiguous match returns `AMBIGUOUS_ACTOR_NAME`; this verb is for a fragment that can match many actors. Fragments containing `..`, `/` or `\` are rejected as a path-traversal guard.

Matching is always a case-insensitive substring across label, internal name **and** object path. There is no `matchMode`/`caseSensitive`; use [`actor.list`](actor.list.md) with `filter=` plus `matchMode:"prefix"`/`"regex"` for anchored or case-exact matching.

Failure modes: `INVALID_ARGUMENT` for an empty fragment or `..`, `/` or `\`; `EDITOR_ACTOR_SUBSYSTEM_MISSING` when no `UEditorActorSubsystem` is available. The verb reads through `UEditorActorSubsystem::GetAllLevelActors`; retry after editor initialization.

### actor.find_by_tag

`matchType` takes exactly two values: **`exact`** (the default — `FName` equality against each
entry of `AActor::Tags`, which is itself case-insensitive) and **`contains`** (case-insensitive
substring against each tag string; `substring` is accepted as an alias). Anything else is refused
with `INVALID_MODE` naming both. It is deliberately not the `matchMode` vocabulary of
[`actor.list`](actor.list.md): `prefix` and `regex` are not implemented here, and accepting them
in order to ignore them is the defect this rejection exists to prevent.

The response echoes the resolved `matchType` in its canonical spelling, alongside the `tag` that
was queried. Read it before trusting a count: an unrecognised value used to fall through to
`exact` silently, so a substring query came back as a narrower exact-match set with nothing in
the response to distinguish the two, and an empty result read as "nothing is tagged that way"
rather than "the mode you asked for never ran".

To discover which tags exist before querying one, use
[`system.inspect.list_actor_tags`](system.inspect.list_actor_tags.md); the read-only twin of this
verb is [`system.inspect.find_by_tag`](system.inspect.find_by_tag.md), which offers `exact` only.

Failure modes: `INVALID_ARGUMENT` when `tag` is empty or carries `..`, `/` or `\`;
`INVALID_MODE` for an unrecognised `matchType`.

### actor.describe

Returns the compact JSON description for one live placed actor. Use it for identity, world transform, sparse modified actor properties, components, attachment data, relative transforms and sparse modified component properties in one read-only call.

This is the live-world equivalent of the per-actor `actors/*.json` files from `asset.dump`. It does not inspect Blueprint templates; use `blueprint.scs.get` or `scs.json` for that component tree.

`nameMatch` / `name_match` filters component names by case-insensitive substring; `componentClass` / `component_class` resolves a UClass name/path and includes subclasses. These narrow *which components* are dumped, not the actor fields or component array.

**The full shape spills.** Even a few default components can exceed the 10000-character display threshold and write to `Saved/PinWright/HttpResponses/...` for `Read`/`Grep`. Shape the output:

- **`fields` allow-list** — `fields:["label","transform"]` (or `properties`/`components`) projects **top-level** keys; `schema`/`storage` remain. Valid keys: `name`, `label`, `path`, `class`, `level`, `folder`, `guid`, `tags`, `transform`, `properties`, `components`. Data such as `attachParent` is inside `components`; combine `fields:["components"]` with `nameMatch` / `componentClass`. A single string is accepted and this mirrors `property.list`'s `propertyNames`.
- **`includeComponents:false`** (aliases `componentsMode:"none"` / `include_components`) drops `components` while retaining identity, transform and sparse actor properties; it is ignored when `fields` is supplied.

For a spawn confirmation needing only location and label, the lighter `actor.get` returns name/label/path/class/tags/location/scale without a component tree or spill. It omits full rotation; pair it with `actor.get_transform`. Use `actor.describe fields=[...]` for `attachParent` or sparse properties that `actor.get` omits.

### actor.get_components

Returns the lean component list for one placed actor or a Blueprint CDO: component name, class path, object path, and scene-component relative transform fields. Use this instead of `actor.describe` when transforms are the only component data you need.

The same scoped-read filters are available here: `nameMatch` / `name_match` filters component names by case-insensitive substring, and `componentClass` / `component_class` resolves a UClass and includes that class plus subclasses.

### actor.get_component_property

Reads one UPROPERTY from a component addressed by its friendly runtime name (e.g. `StaticMeshComponent0`). Reflection serializes a struct UPROPERTY (`BodyInstance`, `BodySetup`, `SensesConfig`, any `F...Instance`) as the **full decomposed struct**, which can exceed the response threshold and spill to `Saved/PinWright/HttpResponses/`.

To read one sub-field, pass a **dotted `propertyName`** — `propertyName: "BodyInstance.CollisionEnabled"` returns that scalar leaf under the spill threshold. This is the right read-after-write confirmation for `CollisionEnabled == NoCollision`.

The generic `property.get` offers the same dotted read but addresses by full object path or UPROPERTY walk; prefer this verb when you already have the actor and friendly component name.

### actor.spawn

The general spawner accepts `classPath` (UClass, `/Script` or BP asset path) or `meshPath` (static/skeletal mesh, auto-picking `StaticMeshActor`/`SkeletalMeshActor`). Inline transforms are `location` `{x,y,z}` (cm), `rotation` `{pitch,yaw,roll}` (degrees) and `scale` `{x,y,z}` (identity `(1,1,1)`).

Aliases: `classPath` → `assetPath`, `class_name`, `className`; `meshPath` → `mesh_path`; `actorName` → `label`, `name`, `actor_name`. The dispatcher formerly rejected the undeclared aliases with `UNKNOWN_PARAMS` before the handler ran.

**`assetPath` resolves onto `classPath`, never `meshPath`, and that is deliberate.** `classPath` already loads static/skeletal mesh asset paths as well as UClass names, `/Script` paths and BP asset paths, so it is the strictly more capable slot; aliasing `assetPath` onto both would let one wire key populate two competing spawn sources in the same request. Do not "fix" it by adding it to `meshPath`.

Pass `scale` directly for a non-unit prop, such as `scale:{x:2,y:2,z:0.5}`, instead of following unit spawn with `actor.set_transform`. For Blueprint-only input prefer `actor.spawn_from_blueprint`.

**Volume classes get brush geometry.** An `AVolume` subclass (`BlockingVolume`, `TriggerVolume`, `PCGVolume`, …) carries its shape in a `UModel` brush, not in its transform, so this verb builds one: the engine's default 200 uu box — the same geometry the place-actors panel gives a dragged-in volume — with `scale` multiplying it. Without that the actor spawns with bounds extent `(0,0,0)` and no collision, every containment query against it answers empty, and the spawn still reports success. Resize afterwards with `volume.set_volume_extent`, or prefer the typed `volume.create_*` verb where one exists for the class.

Materials are settable at spawn: `materialPath` binds **slot 0**; `materialPaths` maps index `i` to slot `i` and takes precedence. This replaces the two-step spawn plus `actor.set_component_properties {OverrideMaterials:[…]}` flow without duplicating the mesh asset.

```js
call({ path: "actor.spawn", args: {
  meshPath: "/Game/Meshes/SM_Wall",
  materialPath: "/Game/Materials/M_Blockout_Red"
}})

// multi-slot mesh: slot 1 only, leaving slot 0 on the mesh default
call({ path: "actor.spawn", args: {
  meshPath: "/Game/Meshes/SM_Kiosk",
  materialPaths: ["", "/Game/Materials/M_Trim"]
}})
```

Semantics and failure modes (identical on `actor.spawn_shape` and `actor.spawn_batch`):

- Written to serialized `OverrideMaterials` on the mesh **component**, so it persists through map save/reload and leaves the mesh **asset** untouched. Use `static_mesh.set_material` for the shared asset default.
- The component is marked `Modify()` and package-dirty; persist with `level.save` or `editor.save_all` per [`safe-mutation-save`](safe-mutation-save.md).
- An empty string or `null` entry in `materialPaths` leaves that slot on the mesh default.
- A path outside a project asset root is `SECURITY_VIOLATION`; a path that loads nothing is `MATERIAL_NOT_FOUND`. Both are raised **before** the actor is spawned, so a bad path never leaves an orphan in the level.
- Entries past the slot count, or a `classPath` actor with no mesh component, are **not** errors: the actor spawns with `material_applied:false`/partial and misses in `warnings`. Compare `materialSlotsApplied` with the request.
- Response adds `material_applied` (bool), `materialSlotsApplied` (int), `materialComponent` (the component that received it), and the echoed `materialPath`/`materialPaths`. Calls that pass no material argument get the exact response they always did.
- On a `classPath` Blueprint spawn, the construction script may create the receiving component. `FComponentInstanceDataCache` reapplies the per-instance override after reruns, including a measured Blueprint recompile that **renames** the component. Removing the component or replacing the instance drops the delta. The call reports `materialOnConstructionScriptComponent:true` plus `warnings`; native-component spawns such as `actor.spawn_shape` are unchanged. For a durable default, use [`blueprint.scs.add_component`](blueprint.scs.add_component.md) `{materialPath}`.

### actor.spawn_from_blueprint

Spawns a Blueprint class instance by asset path, resolves `GeneratedClass`, and uses `TeleportPhysics` so initial physics is not interpolated.

Use it for a `/Game/.../BP_Foo` path: it requires a `UBlueprint` and returns `CLASS_NOT_FOUND` for another asset. `actor.spawn` accepts BP paths too, but its asset-dependent behaviour is less explicit.

The label slot is `actorName`, which also accepts `label`, `name` and `actor_name`; omit it and UE generates one.

Seed BP variables afterward with `call("actor.set_blueprint_variables")` on the returned actor; spawn has no property bag.

### actor.add_component

Adds an *instance* component to one placed actor, not its BP class; recompiling or reconstructing from the BP drops it.

Use it for one-off level tweaks. For "every instance of this BP must have this component," call `blueprint.scs.add_component` on the BP asset.

`StaticMeshComponent`s auto-load `meshPath`; lights are forced to `Movable`. A property failure is reported in `warnings` without failing the call, and so is a `properties` map that leaves the new component with `bAutoActivate:false` — see the same note under `actor.set_component_properties`.

### actor.set_component_properties

Sets UPROPERTY values on one named component. `Mobility` is applied first so later edits are accepted by a Static component; `SimulatePhysics` has a dedicated path. Per-property failures go to `warnings` while the call succeeds.

Two properties do NOT use generic reflection: a raw write leaves the engine's private asset cache stale, so rendering, streaming and collision use the OLD asset while the package serializes the new one; the next bounds update can raise `Ensure condition failed: KnownStaticMesh == StaticMesh`:

- `StaticMesh` (including ISM/HISM) routes through `UStaticMeshComponent::SetStaticMesh`; HISM cluster data is rebuilt and instances are preserved.
- `SkinnedAsset` routes through `SetSkinnedAssetAndUpdate` with `bReinitPose=true`.

Both are verified by reading the component back. A setter that declines (for example, a Static-mobility component after play begins) reports `warnings` instead of `applied`. `""`, `"None"`, `"null"` and JSON `null` clear the slot; another unresolved or wrong-class value is refused.

Four more do not use generic reflection either — the `UShapeComponent` sizes. A reflection write plus the change hook updates the render proxy, the bounds and the body *setup*, but not the live physics body, so the picture and every trace/overlap would report different boxes. `BoxExtent`, `SphereRadius`, `CapsuleHalfHeight` and `CapsuleRadius` route through `SetBoxExtent` / `SetSphereRadius` / `SetCapsuleHalfHeight` / `SetCapsuleRadius`, which also rebuild the Chaos shapes. One behaviour note: `CapsuleRadius` follows the setter's rule, keeping the requested radius and growing `CapsuleHalfHeight` to match, rather than the Details panel's rule of clamping the radius down.

The **collision** fields of `BodyInstance` are the last group that skips generic reflection, and the only one where the engine's own setter is measured *insufficient*. `CollisionProfileName`, `CollisionEnabled`, `ObjectType` and any changed channel inside `CollisionResponses` route through `SetCollisionProfileName` / `SetCollisionEnabled` / `SetCollisionObjectType` / `SetCollisionResponseToChannels`; every other `BodyInstance` sub-field (mass, damping, `bNotifyRigidBodyCollision`, …) keeps its reflection write unchanged. A raw store of `CollisionProfileName` is worse than stale — it writes the *name* without running `LoadProfileData`, so the profile's responses and `CollisionEnabled` are never applied and the component reports a profile it does not implement.

An explicit collision write also clears `bUseDefaultCollision` on a `StaticMeshComponent` (so on an ISM/HISM and a `SplineMeshComponent` too). With that flag set, `UStaticMeshComponent::OnRegister` → `UpdateCollisionFromStaticMesh` re-reads the component's whole collision setup off the mesh asset at every registration, which discards the write the next time the component registers. The engine clears the flag from `SetCollisionProfileName` but from none of the other three setters; the Blueprint editor's details panel clears it before any explicit collision edit, and this matches that.

On an **ISM/HISM** the setter alone still does nothing to collision. The component's own `BodyInstance` owns no shapes: it is a template copied into each per-instance `FBodyInstance` once, at creation, and never consulted again — so `ECC_Pawn → Ignore` reads back `ECR_Ignore` with the profile flipped to `Custom` while a pawn-profile sweep still names the component as the blocker. The write is therefore pushed onto the per-instance bodies with the engine's own `FBodyInstance::CopyRuntimeBodyInstancePropertiesFrom`, which ends in `UpdatePhysicsFilterData()`. A bare `UpdatePhysicsFilterData()` loop is **not** the fix: it rebuilds filter data from the instance body's own stale copies.

A collision write on an instanced component adds an `instanceBodies` block: `count` and `refreshed` (bodies found and bodies the propagation ran on), `requested`, and `measured` — read back off the **per-instance bodies**, the objects the physics scene consults, not off the component. `measured` is **omitted** when there are no bodies to inspect (no instances, no physics state) rather than filled in from the template, and that omission is disclosed in `warnings`; a disagreement between `requested` and `measured` is also a warning. Nothing is added for a non-instanced component or a `BodyInstance` write that touched no collision field.

Every other property uses reflection **and then fires the engine's own `PostEditChangeProperty`**. That hook drives effects such as a water body's `WaterMID`, `SkyLightComponent` capture, `ChildActorComponent` respawn and `UPrimitiveComponent`'s `CachedMaxDrawDistance`; without it, values read back correctly but do nothing.

The response carries **two** lists: `applied` means the field has the value; `notified` means the class change hook ran. Setter-handled properties (`Mobility`, `SimulatePhysics`, `StaticMesh`, `SkinnedAsset`, the four shape sizes, and `BodyInstance` when it names a collision field) appear only in `applied` because the setter supersedes notification.

`notified` reports that the change path *ran*, not that a downstream consumer rebuilt. For values re-derived from other state, such as a water spline's point scale, see `derivedWrite` and the `spline` / `water` pages.

**A write that leaves `bAutoActivate` false is disclosed in `warnings`.** The flag is a CDO default of `true`, so turning it off is serialised into the `.umap`: the component never starts again on any later load of that level, and the level renders nothing from it until something restores the flag or activates it explicitly. The disclosure is derived from the flag read back off the component, so a write the importer declined is never reported as one that landed, and restoring `bAutoActivate:true` says nothing. Three shipped Niagara systems went dark for a day this way after a quiesce step and the map save that followed; `niagara.validate` / `niagara.inspect` now report the level-side consequence as `componentActivation`. `property.set` writing the same flag by reflection discloses it identically.

### actor.spawn_shape

One-call spawn of a `/Engine/BasicShapes` primitive as a `StaticMeshActor`; a convenience wrapper over `actor.spawn` that resolves the shape enum and returns the same actor result.

Args:

- `shape` (string, **required**) — `CUBE`, `SPHERE`, `CYLINDER`, `CONE`, or `PLANE` (case-insensitive).
- `actorName` (string; aliases `label` / `name` / `actor_name`) — display label; defaults to the shape mesh name (e.g. `Cube`).
- `location` (object) `{x,y,z}` cm, `rotation` (object) `{pitch,yaw,roll}` deg, `scale` (object) `{x,y,z}` (default `(1,1,1)`).
- `materialPath` (string) — material bound to slot 0 of the spawned `StaticMeshActor`; `materialPaths` (array) for the multi-slot form. Same semantics and failure modes as [`actor.spawn`](actor.spawn.md).

```js
call({ path: "actor.spawn_shape", args: { shape: "CUBE", location: { x: 0, y: 0, z: 50 } } })

// tinted blockout box in one call
call({ path: "actor.spawn_shape", args: {
  shape: "CUBE",
  location: { x: 0, y: 0, z: 50 },
  materialPath: "/Game/Materials/M_Blockout_Red"
}})
```

Gotchas: an unknown shape is `INVALID_PARAMS` and lists valid tokens. `/Engine/BasicShapes` primitives have one material slot, so entries past `materialPaths[0]` become `warnings`, not errors.

### actor.spawn_batch

Spawn or duplicate many actors in one call, one per `transforms[]` entry.

Args:

- exactly one source: `shape` (BasicShapes primitive), `meshPath` (static/skeletal mesh), `classPath` (UClass/BP) or `sourceActor` (existing actor).
- `transforms` (array, **required**) — `[{location, rotation?, scale?, name?, materialPath?, materialPaths?}]`, non-empty and capped at 512. Missing `rotation`/`scale` are identity/`(1,1,1)`; missing `name` auto-labels `<base>_<i>`; missing material keys inherit the batch default.
- `folder` (string) — World Outliner folder assigned to every spawned actor.
- `materialPath` (string) / `materialPaths` (array) — the **batch-level default** material binding, applied to every spawned actor. Same slot semantics as [`actor.spawn`](actor.spawn.md).

```js
call({
  path: "actor.spawn_batch",
  args: {
    shape: "CUBE",
    folder: "Prototype/Walls",
    transforms: [
      { location: { x: 0, y: 0, z: 0 } },
      { location: { x: 200, y: 0, z: 0 }, name: "Wall_B" }
    ]
  }
})
```

Per-placement material override — either material key **replaces** the batch default wholesale; it is not merged slot-by-slot:

```js
call({
  path: "actor.spawn_batch",
  args: {
    shape: "CUBE",
    folder: "Prototype/Walls",
    materialPath: "/Game/Materials/M_Blockout_Grey",
    transforms: [
      { location: { x: 0,   y: 0, z: 0 } },
      { location: { x: 200, y: 0, z: 0 }, name: "Wall_Hot", materialPath: "/Game/Materials/M_Blockout_Red" }
    ]
  }
})
```

Gotchas: zero or multiple sources, or more than 512 transforms, is `INVALID_PARAMS`. An entry without `location` spawns at world origin; it is *not* dropped.

Partial success is explicit: failed entries appear in `skipped` as `{index, reason}` with reasons `transforms[] entry is not a JSON object`, `actor creation returned null for this transform`, `DuplicateActor returned null for the source actor`, or `duplicate failed: EditorActorSubsystem is unavailable`. `skippedCount` is always present; `skipped` is capped at 32 with `skippedTruncated:true`; `count` + `skippedCount` == `requested`.

Materials resolve in one pre-pass—batch default and per-entry overrides—before spawning. One bad path fails the call with `SECURITY_VIOLATION` / `MATERIAL_NOT_FOUND` naming `transforms[i]`; **nothing spawns**. Non-fatal misses collect in batch `warnings`; the response carries `material_applied`, `materialSlotsApplied`, `actorsWithMaterial` and per-entry `materialSlotsApplied` on `spawned[]` rows.

### actor.set_label

Rename a placed actor's **display label**—the World Outliner name. This is the only actor name you can change; the internal object name is fixed at spawn.

Args:

- `actorName` (string, **required**) — the actor to rename, by label, internal object name, or object path. Also accepts the `objectPath` / `actorPath` aliases.
- `label` (string, **required**) — the new display label.
- `unique` (bool, default `false`) — when `true`, route through `SetActorLabelUnique` so a label already in use gets a numeric suffix instead of colliding.

```js
call({ path: "actor.set_label", args: { actorName: "StaticMeshActor_112", label: "Wall_Top_01" } })
```

Returns `label` (what the actor carries **after** the write, i.e. what the outliner now shows), `previousLabel`, `requestedLabel`, `actorObjectName` (unchanged by a rename), `actorPath`, plus `applied`, `changed` and `labelWasUniquified`.

Gotchas: `unique:false` (default) stores the label verbatim and **does not** uniquify it, so label lookups can become ambiguous. Invalid labels are read back and return `INVALID_ACTOR_LABEL` with `applied:false`; a non-edited Level Instance returns `ACTOR_LABEL_NOT_EDITABLE`; a multi-match returns `AMBIGUOUS_ACTOR_NAME`, so retry with a candidate's `name` or `path`.

### actor.set_folder

Assign placed actors to a World Outliner folder via `SetFolderPath`.

Args:

- `actorName` (string) **or** `actorNames` (array) — a single target or a batch.
- `folderPath` (string, **required**) — e.g. `Prototype/Walls`; nested folders use `/`.

```js
call({ path: "actor.set_folder", args: { actorNames: ["Wall_1", "Wall_2"], folderPath: "Prototype/Walls" } })
```

Gotchas: folders are an editor-only tree created on assignment; the op is undoable. `ACTOR_NOT_FOUND` occurs only when **nothing** matched; a partial match returns `updated` and `missing`.

### actor.nudge

Move and/or rotate an actor by a RELATIVE delta, not an absolute transform.

Args:

- `actorName` (string, **required**; aliases `objectPath` / `actorPath`).
- one of `deltaWorld` (object `{x,y,z}`, world axes) or `deltaCamera` (object `{right,up,forward}`, active viewport camera basis).
- `deltaRotation` (object `{pitch,yaw,roll}` degrees, optional).

```js
call({ path: "actor.nudge", args: { actorName: "Crate_1", deltaCamera: { forward: 100 } } })
```

Gotchas: at least one delta is required (`INVALID_PARAMS` otherwise). If both translation deltas are given, **`deltaWorld` wins**. `deltaCamera` needs an active editor viewport or returns `VIEWPORT_NOT_AVAILABLE`; use `deltaWorld` otherwise. The response includes the resulting transform, `units`, `axis`, `pivot` and `bounds`.
