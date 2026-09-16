# level

Day-to-day "open and edit a map" surface for `ULevel` / `UWorld` assets — create, save, load, rename, duplicate, import/export, stream legacy sublevels in and out, build lighting/nav, query actor lists and bounds. Treat `level` as the runtime / editor-time CRUD layer and `call("level.structure")` as the world-partition authoring layer that sits on top (data layers, HLOD, level instances, packed-level actors, the level blueprint graph).

## Cross-cluster overlap

For persistent level inspection, [`asset.dump`](asset.dump.md) on one UWorld or
[`asset.dump_folder`](asset.dump_folder.md) on a maps folder writes `world_settings.json`,
`level_bp.txt`, `sublevels.json`, and `actors/`. Use dumps for repeatable audits and `level.get_*`
for active-editor summaries; [`asset-audit`](asset-audit.md), [`safe-mutation-save`](safe-mutation-save.md),
and [`visual-review`](visual-review.md) cover audit/edit loops. Build from scratch with
[`level-building`](level-building.md), then use [`level-review`](level-review.md).

## Guarding a mutating sequence against world swaps

Every mutating `level.*` call accepts optional `expectWorld`. Pass the `world` returned by the
previous mutating call to refuse the next operation if another level load or PIE transition changed
the active world in between. The full world object path is canonical, while its package path (for
example `/Game/Maps/Arena`) is also accepted. A mismatch returns `WORLD_MISMATCH` with
`expectedWorld` and actual `world`, before the level handler runs; the same check is repeated at a
deferred safe-point continuation immediately before a save/load mutation. Mutating success and
error responses carry the resolved editor-world object path in `world`, so the value can be chained
without a separate `level.get_info` probe. Omitting the field keeps the legacy unchecked behavior.

**Gotchas**

- `level.build_lighting` is an **async job, not a blocking call**: it does NOT block the editor or
  return a bake result. It returns `{status:"running", ticket_id, ...}` synchronously while the
  lightmap bake (tens of minutes on a large map) runs in the background. Before `level.save` or a
  lighting-state read, poll `call("system.job_status", { ticket_id })` to `completed`/`failed`;
  the ticket means "accepted", never "the bake is done". The full ticket→poll contract and
  ticket-pattern table are on [`system`](system.md#long-running-jobs) →
  [`system.job_status`](system.job_status.md).
- Sublevel ops (`add_sublevel`, `stream`) are the *legacy* level-streaming model. New maps almost always use world partition — create them partitioned with `call("level.structure.create_level", { bCreateWorldPartition: true })` (there is no programmatic in-place conversion of an already-loaded non-WP world); `level.add_sublevel` and friends are still useful for lighting scenarios and for projects that have not migrated.
- `level.stream` is **runtime/PIE-only**: it wraps `StreamLevel`, which has no editor-time consumer.
  Outside play it fails with `RUNTIME_ONLY_COMMAND` (formerly `[EXEC_FAILED] Command not executed`).
  For editor authoring, toggle **visible** with `call("level.set_visibility", …)` and **loaded** with
  `call("level.add_sublevel", …)` / `call("level.remove_from_world", …)` or
  `call("level.structure.configure_level_streaming", …)`. Use `level.stream` only after
  `pie.start` / inside a running game.
- `level.get_actors` returns every actor in the named level — for filtered queries use `call("actor.find_by_class", …)` or `call("actor.find_by_tag", …)` against the active world instead.
- `level.get_info` / `level.get_actors` / `level.get_bounds` inspect **only levels loaded into the
  active world** (persistent level plus loaded sublevels). An on-disk but unloaded `levelPath`
  errors `LEVEL_NOT_LOADED` ("exists on disk but is not loaded… `level.load` it first"), distinct
  from `LEVEL_NOT_FOUND` (no `.umap` on disk). Load it first, or omit `levelPath` for the active
  level. `level.list` still enumerates every on-disk map, so it can list a map these getters reject
  until it is loaded.

## Auditing a level

`call("level.audit", { args })` sweeps every actor **once, in C++** and reports demonstrable
problems: non-finite transforms; degenerate, negative, or extreme scale; null meshes; placeholder
materials; origin, KillZ, or world-bound violations; duplicate transforms; and, with `surface`,
underground, floating, single-contact, or unsupported-stack actors. Use it instead of a per-actor
Python loop: one over ~5000 actors wedged an editor for over two hours with no way to cancel.

Before trusting a report: checks are **selectable and individually accounted**. Each reports
flagged / clean / unrunnable / not-applicable / ignored per actor, and those buckets sum to the
examined count; unrunnable and unselected are never mistaken for clean. Ground-relative checks
require `surface` (same shape as [`spatial.ground_actors`](spatial.ground_actors.md)); the play-area
check requires `playArea`. Both error rather than guess.

## See also

- [`level.audit-checks`](level.audit-checks.md) — the check catalogue, every threshold and its justification, the ignore-list model, and what the audit structurally cannot see.

## PIE safety for package mutations

`level.delete`, `level.rename`, and `level.duplicate` return `PIE_ACTIVE` before changing a level package while Play In Editor is active. Stop PIE and retry; registry/object reads remain available for diagnosis.

### level.audit

Read-only: nothing is moved, modified, or marked dirty. The fixer is
[`spatial.ground_actors`](spatial.ground_actors.md); both use the same footprint solver, so they
cannot disagree about what "seated" means.

**A first call.** With no arguments it runs the cheap default set (transform, scale, mesh, material,
origin, KillZ, world bounds, duplicates) over the whole level and returns in milliseconds. Add
`surface` to enable ground checks:

```js
call({ method: "level.audit", args: {
  surface: { preset: "landscape" },     // buys below_surface / airborne / balanced / unsupported_assembly
  playArea: { source: "landscape" },    // buys outside_play_area
  detail: "findings"
}})
```

Supplying `surface` / `playArea` without `checks` opts into the checks they support; naming
`checks` opts out and selects exactly that list. The response echoes every check with `selected`,
and unselected checks carry `notSelectedReason`, separating "found nothing" from "nobody looked".

**Reading the report.** `pass` is false if any finding is at or above `failOn` (default `error`),
any check is unrunnable, or the sweep is truncated. `failOn: "none"` cannot make an unrunnable run
green. The response separates `errorCount` / `warningCount` / `unrunnableCount`; per-check counts
are exact, while `findings[]` is capped by `maxFindings` and reports `findingsDropped`.

**The burial question.** `below_surface` does **not** ask "is the underside below the ground"—that
is true of correctly bedded rocks too. It asks whether the surface is above the actor's *highest*
point in every sampled column (whether any part is visible), and reports `coverDepthCm` plus
`deep` past `deepCoverDepthCm` (default 1000 cm; a triage sort key that changes no finding or
`pass`). See [`level.audit-checks`](level.audit-checks.md) for the rationale and how to choose a
local value. The "how much is under" number belongs to opt-in `deeply_embedded`. `below_surface`
is meaningful only with `surface: {preset: "landscape"}`; with a non-terrain spec, a roof above an
actor is accepted as its surface and noted in `caveats`.

**Assemblies.** A per-actor contact check misses a stack whose pieces rest on one another while the
whole stack floats. `unsupported_assembly` follows each measured support edge to terrain and flags a
chain ending in mid-air. It needs a surface that accepts non-terrain hits; with `preset: "landscape"`
no actor-on-actor edge is observable, so the check is not-applicable everywhere and says so in a
caveat. A chain leaving the audited set, cycling, or exceeding 16 links is **unrunnable**, never
resolved by assumption.

**What it cannot see.** Instanced-mesh instances (foliage) are not actors; skipped-instance counts
are in `caveats`. On World Partition maps, unloaded actors are absent from `ULevel::Actors` and
invisible to the sweep; `worldPartition.unloadedActors` and a caveat prevent a clean report over a
mostly-unloaded map from being mistaken for a clean map. Thresholds, ignore-list rules, and the
full catalogue are on [`level.audit-checks`](level.audit-checks.md).

### level.build_lighting

`quality` (`preview`/`0`, `medium`/`1`, `high`/`2`, `production`/`3`) is applied through
`FLightingBuildOptions::QualityLevel` and `UEditorEngine::BuildLighting`. Do not use the
`BuildLighting` console command or `FEditorBuildUtils::EditorBuild`: the exec handler drops the
quality and `EditorBuild` rebuilds options from the ini. Omitting `quality` uses
`[LightingBuildOptions] QualityLevel` from `GEditorPerProjectIni`, matching Build ▸ Lighting; the
other options (selection, current level, visibility, error coloring) come from that same block.

The bake is still an async job — see the ticket/poll gotcha above.

### level.duplicate

The copy is created **in memory only**: its package is dirty but no `.umap` is written. The result
returns `saved:false` plus `persistenceNote`; `duplicated:true` means the in-memory copy exists,
**not** that a load-ready file is on disk. Save it (`level.save_as` or `editor.save_all`) before
`level.load`; otherwise the destination fails `LEVEL_NOT_PERSISTED` ("registered/loaded in memory
but never saved to disk").

### level.load

Loading swaps the active editor world and destroys the outgoing `ULevel` during garbage collection.
That is illegal inside `UWorld::Tick` (`!LevelList.Contains(TickTaskLevel)`), so the handler swaps
inline only when already outside a tick; otherwise it defers to the next core-ticker pass. RPCs are
game-thread `AsyncTask`s, so the caller cannot choose the frame. Exactly one response arrives after
the map is fully loaded, with `deferredToSafePoint` identifying the branch. `editor.open_level` and
`editor.open_asset` on a World delegate here.

Reopening the already-active map is a no-op (`alreadyLoaded:true`), never a destroy-and-reload. When the target has no `.umap` on disk the verb splits the verdict: `LEVEL_NOT_PERSISTED` for a world that is live in memory or listed in the asset registry but was never saved (save it or discard the orphan — do not retry path forms), `FILE_NOT_FOUND` for a genuinely absent path.

A map that is **already loaded with unsaved changes, and still held alive by the editor**, refuses with `DIRTY_WORLD_BLOCKS_MAP_SWAP` and changes nothing. (A previously-open map that some verb already tore down with `UWorld::DestroyWorld` is not held alive — the load's own garbage collection reclaims it — so it never refuses.) This is not a policy preference: to re-read the map the engine has to unload the resident copy, it will not unload a dirty package, and `UEditorEngine::Map_Load` then reaches an unconditional `Fatal` ("World Memory Leaks") that kills the editor process and every session attached to it. The arming sequence is ordinary — save a map, keep editing it, get swapped to another world, come back — so a single caller reaches it with no second client. The error payload names `blockingPackage`; clear it by saving that one package (`saveDirtyTargetWorld:true` saves it and then loads, keeping the edits) or by saving everything (`editor.save_all`), or discard its in-memory edits. `editor.list_dirty_packages` shows the condition before you call. Deferring the swap to a safe point does not help here — that fixes a different crash on the same verb.

The same error code covers a second, unrelated blocker: a **dead world that is still resident** — one that belongs to no world context and is not one of the types the editor keeps across a swap. The engine's own post-cleanse check (`CheckForWorldGCLeaks`) walks every resident world after the swap's garbage collection and logs `World Memory Leaks` at **Fatal by default** on anything it finds, so `level.load`, `editor.open_level`, `editor.open_asset` on a World, `level.create` and `lighting.create_lighting_enabled_level` all run the same precondition first: they ask every holder to release those worlds, run that collect themselves, and refuse only on what is still standing. This has nothing to do with unsaved changes — an ordinary dirty map, incoming or outgoing, is never refused by it. The payload adds `survivingWorlds[]` (`worldPath`, `packageName`, `worldType`, `garbage`, `referencedBy` — the shortest reference chain to a root), `survivingWorldCount`, `purgedWorldCount` and `ranCollect`. The known holder is the Python plugin's wrapper registry after `python.execute` touched objects of a PIE session; the probe already purged it and re-collected before refusing, so a world still listed needs the editor restarted. See [`python`](python.md) → "Calls that crash the editor". The severity is the engine cvar `Editor.CheckForWorldGCLeaksAreFatal` (default `true`); `false` downgrades it to a logged error and leaks the world.

**Two things that check does not promise.** It is not side-effect free: reaching the verdict broadcasts the engine's cleanse notification for each dead world (holders drop their references, and asset editors open on them close), flushes async loading and asset compilation, and runs one full-purge garbage collection — the map swap itself does not happen, but the editor is not untouched. And it **cannot see the outgoing world**: at the moment of the check that world still owns its world context, so neither the engine's predicate nor this one counts it, and whether it survives its own teardown is only decidable by performing that teardown. A green pre-flight therefore means "no *other* dead world is resident", not "this swap is safe" — the outgoing direction is a residual risk no refusal here covers. If the probe cannot run at all (a collect already in flight, or a synchronous load on the call stack) the verb refuses with the retryable `EDITOR_NOT_READY` and `probeUnavailableReason` rather than swapping blind.

To open a map in a **fresh** editor rather than swapping the live world, use the stdio proxy's `editor_start` / `editor_restart` tools with `map: "/Game/Maps/MyLevel"` — see [`unattended`](unattended.md).

### level.save

`level.save` is a **ticketed job** with two response modes. Plain JSON or `wait:false` returns
`{status:"running", ticket_id, method:"level.save", ...}` immediately; poll
`call("system.job_status", { ticket_id })` until `completed`/`failed`. Streaming requires all
three inputs: `progressToken`, `Accept: text/event-stream`, and `wait:true`.
That mode suppresses the immediate ticket response and delivers the terminal job response
(a successful result or a failure) on the original stream.

In both modes, exactly one retained core-ticker safe-point continuation is scheduled outside
`UWorld::Tick` and named-thread task pumps while the dispatcher keeps the original request scope
active. The continuation invokes the existing `McpSafeLevelSave` retry policy, with
up to five attempts. `saved:true` is verified by a mount-aware file-exists probe; if the engine reports success
without writing an in-memory-only world, `persistenceNote` is returned and `level.save_as` is the fix. See
[`system`](system.md#long-running-jobs) → [`system.job_status`](system.job_status.md).

### level.save_as

`level.save_as` is a **ticketed job** with the same two response modes. Plain JSON or `wait:false`
returns `{status:"running", ticket_id, method:"level.save_as", ...}` immediately; poll
`call("system.job_status", { ticket_id })` to a terminal status before loading or relying on the
new path. Streaming requires `progressToken`, `Accept: text/event-stream`, and `wait:true`; it
suppresses the immediate ticket response and delivers the terminal job response
(a successful result or a failure) on the original stream.

Exactly one retained core-ticker safe-point continuation is scheduled outside `UWorld::Tick` and
named-thread task pumps while the dispatcher keeps the original request scope active. The
continuation invokes the existing `McpSafeLevelSave` retry policy, with up to five attempts. See
[`system`](system.md#long-running-jobs) → [`system.job_status`](system.job_status.md).
