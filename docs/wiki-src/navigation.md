# navigation

Configure editor navigation data, nav agent settings, NavAreas, NavModifier components, and NavLinkProxy actors — Recast nav mesh settings, area costs, link endpoints, simple or smart link behavior, and navigation rebuilds.

Use this namespace for level navigation state or Blueprint components that affect pathing; use `ai` for higher-level AI assets that consume navigation (controllers, behavior trees, Mass config, Smart Objects).

## Prerequisite: a RecastNavMesh must exist before you configure it

`configure_nav_mesh_settings` and `set_nav_agent_properties` (and any method that mutates RecastNavMesh generation/agent settings) operate on the level's **RecastNavMesh** nav-data actor. On a fresh/empty level no RecastNavMesh exists yet, so both hard-fail with `[NO_NAVMESH] No RecastNavMesh found in level`.

The RecastNavMesh is not materialized by these methods or by any `create_*` verb in this namespace — the navigation system **auto-registers it as a side effect of placing a `NavMeshBoundsVolume`**. Bring one into existence with `volume.create_nav_mesh_bounds_volume` (which auto-registers the nav data). So the level-nav-setup order is:

`volume.create_nav_mesh_bounds_volume` → `configure_nav_mesh_settings` → `set_nav_agent_properties` → (`create_nav_link_proxy` / `configure_nav_link` / `create_nav_modifier_component` …) → `rebuild_navigation` (async — then poll `system.job_status`; see below).

`get_navigation_info` reports the current `boundsVolumes` count and whether a nav mesh is present, so call it first to check whether the bounds-volume prerequisite is already satisfied.

## Rebuilding navigation is async

`rebuild_navigation` is an **async job, not a blocking call** — it does NOT block the editor and does NOT return a rebuild result. It returns a job ticket synchronously (`{status:"running", ticket_id, ...}`) and the nav-mesh regeneration runs in the background until `UNavigationSystemV1::IsNavigationBuildInProgress` clears, at which point the job completes with the `{nav_built:true, …}` payload. To know when the rebuild actually finished — before you read nav state with `get_navigation_info`, describe nav actors, or otherwise depend on the new nav data being in place — poll `call("system.job_status", { ticket_id })` until the status is terminal (`completed`/`failed`); treat the synchronous ticket as "accepted", never as "the rebuild is done". The full ticket→poll contract (immediate response shape, JSONL event stream, polling cadence) lives on [`system`](system.md#long-running-jobs) → [`system.job_status`](system.job_status.md), where `navigation.rebuild_navigation` is listed in the ticket-pattern table.

## Confirming a NavModifierComponent's AreaClass after authoring

`create_nav_modifier_component` now echoes the values it applied directly in its success result — `resolvedAreaClass` (the component template's `AreaClass` path after resolution) and `failsafeExtent` (`{x,y,z}`). Read those for inline confirmation of which nav area landed; do **not** rely on a follow-up `blueprint.scs.get` / `asset.dump` (`scs.json` / `properties.json`) to surface `AreaClass`.

The readback dumps are intentionally **sparse** — they emit only properties that differ from the component's class CDO. `NavArea_Null` is the value the `UNavModifierComponent` constructor installs as the default `AreaClass`, so when you set exactly `NavArea_Null` (the common no-go-zone case, also what you get by omitting `areaClass`), the value equals the CDO and is **correctly omitted** from every sparse readback. Its absence there is **not** evidence the set failed — trust the create call's `resolvedAreaClass`, or fall back to `property.get … includeDefault:true` (which carries `value` + `defaultValue` + `defaultSource` and disambiguates set-to-default from never-set).

To author the area class on a Blueprint **asset**, pass `areaClass` to `create_nav_modifier_component` or use `blueprint.scs.set_property`. `set_nav_area_class` targets **placed actors only** (it resolves an actor in the editor world via `TActorIterator`, erroring `NOT_FOUND` if none matches), so it cannot author the area class on a Blueprint asset.
