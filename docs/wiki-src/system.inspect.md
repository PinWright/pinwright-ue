# system.inspect

Read-only inspection of the live editor process — actors, classes, objects, viewport info, selected actors. The audit-side counterpart to several mutating namespaces; prefer this branch whenever the workflow is purely observational since it documents intent and never dirties packages.

## Cross-cluster overlap

- **`system.inspect.list_objects` / `find_by_class` / `find_by_tag` ↔ `actor.list` / `actor.find_by_class` / `actor.find_by_tag`** — same actors, but the `name` field differs: the `system.inspect.*` rows put the **internal object name** (`GetName`, e.g. `StaticMeshActor_0`) in `name`, while the `actor.find_by_class` / `actor.find_by_tag` twins put the **display label** (`GetActorLabel`, e.g. `Checkpoint_A`) in `name`. The `system.inspect.*` rows additionally carry a `label` field (the display label) alongside `name`, so a read-only audit can read the human label without re-calling the write twin; they also key the array `objects` (vs `actors`). The `actor.*` versions are the historical home (and slightly faster for actor-only queries); `system.inspect.*` is the explicit read-only side. When in doubt, use `system.inspect.*` for audit code and `actor.*` for code that's about to mutate. The internal `name` is the collision-safe key callers feed back to the `actor.*` verbs — `actor.*` accepts either the label or the internal name, so prefer `name` when an audit result must round-trip to a mutation.
- **`system.inspect.inspect_object` ↔ `actor.get` / `blueprint.inspect`** — the general-purpose read-only inspector. Broader than `blueprint.inspect` (works on any UObject, not just BPs) and narrower than `actor.get` (fewer per-actor convenience fields). Reach for it when you have a path to an arbitrary UObject and need its properties + transform + components.
- **Live object reads vs content asset mirrors** — for content asset source-of-truth, prefer `asset.dump` / `asset.dump_folder` and read the cache files; `inspect_object` is for live editor UObject reads where the loaded process state is the target.
- **`system.inspect.inspect_class` ↔ `blueprint.search_api`** — class-introspection. `inspect_class` resolves a UClass by short name / `U`/`A` prefix / `/Script` path / `/Game/` BP path and returns the parent class. `blueprint.search_api` searches the indexed BP API surface for callable functions.

PIE-aware reads: `list_objects`, `find_by_class`, and `find_by_tag` accept an optional `world` param (`editor` / `pie` / `auto`, default `auto`) and resolve PIE-first when available, echoing the resolved `world` and `worldPath` in the response. Subsystems, GameInstance / GameMode / GameState / PlayerControllers, and live UObject paths fall outside the actor-iteration model — see [`runtime-uobject-inspection`](runtime-uobject-inspection.md) for the end-to-end PIE workflow (subsystem discovery, the live transient object-path shape, and property reads).

Note: project settings, editor settings, and performance/memory statistics are not inspectable through this branch. For rendering project settings use `rendering.get_project_settings`; for performance and memory metrics use the `performance.*` namespace. For the active world's name and current level use `editor.status` (plus `level.get_info` for per-level metadata), and for a scene-composition breakdown use `system.inspect.list_actor_classes` (or `list_objects` with its `totalMatches` count).

## Discovery surfaces for class catalogs

Class and command discovery is split by data source:

- **BP-derived classes in `/Game`** — use `asset.search` with `parentClassPath` (`/Script/Engine.Actor`, `/Script/Engine.ActorComponent`, `/Script/UMG.UserWidget`) or `asset.search_assets` with `classNames=["Blueprint"]` + `recursiveClasses=true` + `parentClass`.
- **Exact class name resolution** — `system.inspect.inspect_class` resolves one known name/path to a UClass. Errors `CLASS_NOT_FOUND` on misspellings; not fuzzy.
- **Console commands and CVars** — use `system.console.search` for live `IConsoleManager` substring search before `system.console_command` or `editor.console_command`.
- **Native UClasses without a known name** — use `python.execute` to enumerate `unreal.find_class` / iterate `UClass` children by reflection, or grep the `blueprint.search_api` index when you need callable UFunctions rather than class names.

For placed instances (orthogonal to type discovery) use `actor.find_by_class` / `actor.find_by_name` / `actor.find_by_tag` and their read-only twins `system.inspect.find_by_class`, `system.inspect.find_by_tag`, `system.inspect.list_objects`.

MetaSound nodes are not UClass-based; they use the MetaSound Frontend's own `FNodeClassMetadata` registry (class name + version + interface), so native UClass enumeration does not cover them.

### system.inspect.inspect_object

The widest-scope read-only inspector. Resolves the target by path (`/Game/Maps/MyLevel.MyLevel:PersistentLevel.MyActor_1`) or by display name within the active world, returning properties + transform + components + class. The top-level leaf class name is carried under **`class`** (the same key as the nested `components[]` and the `list_objects`/`find_by_class`/`find_by_tag` rows); the legacy **`className`** key is retained as a back-compat alias with the identical value, and `classPath` holds the full `/Script/...` path.

For Blueprint *assets* prefer `call("blueprint.inspect")` — it returns graph + CDO + SCS detail this method doesn't surface. For an actor where you also want world location and class metadata in convenience fields, `call("actor.get")` is sharper. Reach for `inspect_object` when the target is an arbitrary UObject (not necessarily an actor or BP) or when you do not know which side it falls on yet.

For repeatable content asset inspection, dump the package with [`asset.dump`](asset.dump.md) or sweep a folder with [`asset.dump_folder`](asset.dump_folder.md), then read the mirror files from disk. That cache is usually the better source of truth for Blueprint, Widget Blueprint, SCS, level, and property audits; use `inspect_object` when you specifically need the currently loaded UObject state.

### system.inspect.get_viewport_info

Return the active viewport's pixel `width`/`height` plus the level-editor camera transform — `cameraLocation` `{x,y,z}`, `cameraRotation` `{pitch,yaw,roll}`, `fov` — read from the level-editor viewport client. (Returns `success:true` with no dimensions/camera when no viewport is active.)

**The two halves of the response can describe two different viewports while PIE runs**, and `pie` / `activeViewport` say when. Starting PIE inside the level viewport swaps the *active* viewport for one owned by the game client, so `width`/`height` then measure the play window (`pie:true`, `activeViewport:"pieGameViewport"`); outside PIE they measure the level viewport (`pie:false`, `activeViewport:"levelEditorViewport"`). The camera fields are always the **editor** camera — the verb never reads the play camera, and never reaches through the game viewport's client — so a bookmark or `editor.set_camera` pose is still readable back mid-session. They are omitted when no level-editor viewport is open at all. There is no play-camera readback here: this verb reports the editor camera or nothing.

This is the **camera read-back partner** for the `editor.*` camera-moving verbs. `editor.focus_actor`, `editor.set_camera`, and `editor.jump_to_bookmark` each return only `{success:true}` and do not echo the resulting transform, and `editor.status` carries only PIE/world state — so to confirm where the camera ended up after framing/teleporting/restoring a bookmark (or to record the pose for a "note the resulting camera location/rotation" task), call this method.

### system.inspect.list_objects

Enumerate the actors in the resolved world (PIE-first in `auto`) as `{name, label, path, class}` rows — the natural "what's in this level?" survey, the read-only counterpart to `actor.list`. `name` is the **internal object name** (`GetName`), `label` is the **editor display label** (`GetActorLabel`); the `actor.*` find twins instead put the label in `name`, so read `label` here when you want the human-facing name. The method returns *all* matching actors, so on any populated level the full array exceeds the inline display budget and spills to a file. Narrow it inline instead of switching methods:

- **`filter`** — pattern matched against the actor's internal `name` **and** its class name (kept if either matches); omit for everything. Note it does **not** match the display `label` — use `actor.list` for that. **The default is a case-INSENSITIVE SUBSTRING match**, so `filter:"SH_"` also matches `Brush_0` (the lowercase `sh_` inside `Bru[sh_]0`) and inflates `totalMatches`. Two opt-in modifiers fix that, identical in name and behaviour to `actor.list`'s: **`caseSensitive`** (alias `case_sensitive`, default `false`) and **`matchMode`** (alias `match_mode`, default `"contains"`; also `prefix`/`starts_with`, `exact`, `regex`). Omitting both reproduces the legacy behaviour exactly. See [`actor.list`](actor.list.md) for the worked `SH_`/`Brush_0` example and the failure it caused. When `filter` is supplied the response echoes `filter`/`matchMode`/`caseSensitive`; a malformed `regex` returns `INVALID_PATTERN` instead of silently matching nothing.
- **`limit`** — max rows after filtering; `0` (default) returns all, so default output is unchanged. `count` reports the returned rows, `totalMatches` always reports the full untruncated match count, and `truncated` flips `true` when rows were elided — the same detectable-elision contract as the sibling readers.
- **`namesOnly` / `fields`** — per-row projection. `namesOnly=true` drops the verbose per-row `path` (keeps `label`+`name`+`class`); `fields=["name","class"]` is the explicit allow-list. Valid keys are `label`, `name`, `path`, `class` — this verb's own four columns and nothing else. **Any other key is rejected by name with `INVALID_PARAMS`**, never silently dropped: an unrecognised entry still switches projection on while matching no column, so `fields:["folder"]` would otherwise answer every matched actor with an empty `{}` row — success-shaped and indistinguishable from "this actor has no such data". For the Outliner folder use [`actor.list`](actor.md) with `fields=["folder"]`; for properties, transform or components use `system.inspect.inspect_object`. Use the projection for the common "just give me names so I can pick" survey.

These levers supersede the old "for large worlds, prefer `find_by_class`/`find_by_tag`" steer — that was a workaround for the missing narrowing, not a substitute for the "show me everything, then narrow" survey. Reach for `find_by_class`/`find_by_tag` when you already know the class/tag to scope to.

### system.inspect.find_objects_by_class

`filter` here is matched against three columns — the instance `name`, its class name, and its **full object path** — and an instance is kept if any of the three matches. Because the path is in the set, a short `filter` matches far more broadly than it does on `list_objects`.

It takes the same `matchMode` / `caseSensitive` modifiers as `actor.list` and `system.inspect.list_objects`, with the same names, aliases, defaults, and error codes: default `matchMode:"contains"` + `caseSensitive:false` (a case-INSENSITIVE substring match — see the [`actor.list`](actor.list.md) `SH_`/`Brush_0` worked example), `matchMode:"prefix"` to anchor, `matchMode:"regex"` for a pattern (malformed → `INVALID_PATTERN`). Note that under `prefix` the path column starts with `/Game/…` or `/Engine/…`, so an anchored name prefix will only ever match via the `name`/class columns.
