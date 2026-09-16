# level.structure

World-partition / structural-authoring API for levels — enable world partition on a map, define data layers, configure HLOD/streaming, build level instances or packed-level actors, and edit the level blueprint graph. Sits on top of a `ULevel` that already exists; reach for `call("level")` when you just want to load, save, list actors, or build lighting on a plain map.

## Cross-cluster overlap

The supported way to get a partitioned world is to create the level with World Partition already on: `call("level.structure.create_level", { bCreateWorldPartition: true })` — that attaches a real `UWorldPartition`, after which the data-layer / HLOD / streaming verbs work on it. Then place actors via `call("actor.spawn", …)` and assign them to a data layer via `call("level.structure.assign_actor_to_data_layer", …)`. Do **not** start from a plain `call("level.create", …)` and expect `enable_world_partition` to convert it — that call refuses on an already-loaded non-WP world (see the Gotchas).

**Gotchas**

- `enable_world_partition` **cannot convert an already-loaded non-WP world** — it only reports state and rejects the enable request; create the world with `create_level {bCreateWorldPartition:true}` instead. See the `### level.structure.enable_world_partition` section below for the exact rejection message.
- `configure_hlod_layer` creates and **configures** the layer asset (`cellSize`, `loadingDistance`, `bIsSpatiallyLoaded`, `layerType` all land on it), but it does not *build* HLODs — generating the proxy meshes is a separate editor/commandlet build. See `### level.structure.configure_hlod_layer` for the UE 5.7+ caveat on the grid fields.
- Level-blueprint graph editing follows the same pattern as `call("blueprint.graph")` — see that namespace's wiki for node / pin conventions. The dedicated `add_level_blueprint_node` / `connect_level_blueprint_nodes` verbs only create **unbound stub** nodes; see `### level.structure.add_level_blueprint_node` below before using them.

### level.structure.enable_world_partition

Reports the active level's World-Partition state. It **cannot enable** World Partition on an already-loaded non-WP world: calling it with `bEnableWorldPartition:true` on such a world returns `[OPERATION_FAILED]` ("Cannot enable World Partition programmatically. Use 'Edit > Convert Level' in editor or create a new level with World Partition enabled."). There is no programmatic in-place conversion today. To author a partitioned world, create it with the flag set rather than converting an existing one.

Workflow (partitioned-world authoring):

1. Create the level with World Partition on: `call("level.structure.create_level", { levelName: "MyMap", bCreateWorldPartition: true })`. This attaches a real `UWorldPartition` (the response carries `worldPartitionEnabled:true`) **and makes the created world the active editor world** (`activeWorld:true`), so the steps below — which all operate on the active world — target it directly.
2. Tune cell size and streaming distance: `call("level.structure.configure_grid_size", …)` and `call("level.structure.set_streaming_distance", …)`.
3. Define data layers: `call("level.structure.create_data_layer", …)` per gameplay grouping.
4. Assign actors: `call("level.structure.assign_actor_to_data_layer", …)` for each actor that should be data-layer-gated.

`create_level` `{save:false}` builds the world in memory only — it is reachable now (it is the active editor world), but `call("level.load", …)` will refuse the path with `[LEVEL_NOT_PERSISTED]` until it is saved to disk. Use `{save:true}` (the default) to also write the `.umap`.

On a partitioned world, sublevel-streaming calls in `call("level")` no longer apply — composition is handled by the world-partition runtime, not by manual sublevel adds.

### level.structure.configure_hlod_layer

Creates the `UHLODLayer` asset **and writes every requested setting onto it**: `cellSize` and `loadingDistance` (which have no C++ setters — they are written by reflection), `bIsSpatiallyLoaded`, and `layerType` (`MeshMerge` | `Instancing` | `MeshSimplify`/`SimplifiedMesh` | `MeshApproximate`/`ApproximatedMesh`; anything else falls back to `MeshMerge`).

On **UE 5.7+** the layer's `bIsSpatiallyLoaded` / `cellSize` / `loadingDistance` fields are deprecated by the engine — the runtime grid they used to drive is now configured in the world partition's own settings (`call("level.structure.configure_grid_size", …)`). The values are still written to the asset and the response carries a `note` saying so; if you need the streaming grid to actually change on 5.7+, configure the partition, not the HLOD layer.

Declaring the layer does not build anything. Generating HLOD proxy meshes is a separate editor / commandlet build pass.

### level.structure.configure_level_streaming

`streamingMethod` is applied by **swapping the level's `ULevelStreaming` class** (`Blueprint`, alias `Dynamic` → `ULevelStreamingDynamic`; `AlwaysLoaded` → `ULevelStreamingAlwaysLoaded`) — the class is the only carrier of the method, so the response reports the resulting class under `streamingClass` and whether it changed under `streamingClassChanged`. An unrecognised value is rejected with `UNKNOWN_STREAMING_METHOD` rather than accepted.

Two constraints:

- The reclass is applied **only when `streamingMethod` is actually sent**. Omitting it leaves the current class untouched (the schema default `"Blueprint"` is not force-applied), so a call that just flips `bShouldBeVisible` can never silently convert an AlwaysLoaded level to Dynamic.
- Changing the class **hides, removes and re-adds the streaming level**, which requires it to be loaded. On an unloaded level the call fails with `LEVEL_NOT_LOADED`; load it (`call("level.add_sublevel", …)`) first. The visibility / block-on-load / distance-streaming flags in the same call are applied to the level *after* the swap.

### level.structure.create_level_instance

Spawns the `ALevelInstance` **and binds it to `levelAssetPath`** (`SetWorldAsset` + `UpdateLevelInstanceFromWorldAsset`), so the referenced level is actually embedded and loaded; the resolved object path comes back under `worldAsset`. The engine refuses a source level that would create a loop (instancing the world the actor lives in, or an ancestor of it) and refuses a level that was never saved to disk — both come back as `OPERATION_FAILED` with the actor discarded.

### level.structure.create_packed_level_actor

Spawns the `APackedLevelActor`, binds it to `levelAssetPath`, and runs `FPackedLevelActorBuilder` to bake that level's contents into the actor. The response reports `packed` and `packedComponents` (the number of baked components) — a bare spawn with no `levelAssetPath` still succeeds but comes back with `packed:false` and nothing baked, which is only useful as a placeholder.

`bPackBlueprints` / `bPackStaticMeshes` **cannot be disabled**: the engine's default packed-level builder always installs both the recursive (nested level instance) and ISM (static mesh) builders, and the builder classes are engine-private, so no subset can be selected. Passing `false` for either is rejected with `UNSUPPORTED_OPTION` instead of being silently ignored. To keep a level's contents live rather than baked, use `create_level_instance`.

Packing block-loads the source level through the level-instance subsystem, so it cannot run while another level instance is open for edit (`OPERATION_FAILED`).

### level.structure.add_level_blueprint_node

Creates an **unbound stub** node of `nodeClass` in the level Blueprint's event graph — it does **not** bind an event or function reference, so a `K2Node_Event` comes out as "Event None" (no `EventReference`) and a call node has no target `UFunction`. Use this only to drop a placeholder; for a real bound node (e.g. a `BeginPlay` event or a `Print String` call) author it with `call("blueprint.graph.create_node", …)` (passing `eventName` for events, `target`/`memberName` for calls) on the level-script Blueprint object instead.

Parameter notes:

- `nodeName` is applied as the node's comment label and **echoed back under `nodeName`**; the auto-generated node title is returned separately under `nodeTitle` (the two no longer share one response key). Note it is a **display-only comment label, not a node handle**: `connect_level_blueprint_nodes` looks nodes up by title / object name, so it **cannot** reference a node by the `nodeName` you assign here — wire stub nodes by the `nodeTitle` value (or use `blueprint.graph.connect_pins` on the level Blueprint) instead.
- `nodePosition` accepts the object shape `{x,y}` **and** the flat `x`/`y` spelling used by `blueprint.graph.create_node`; either lands the node at the requested coordinates.

### level.structure.open_level_blueprint

Opens the active level's level-script Blueprint in the editor and returns its handle. The
`assetPath` it hands back is the level-script object path
(`/Game/Maps/<Map>.<Map>:PersistentLevel.<Map>`) — the form that **round-trips** straight
into the `blueprint.graph.*` family (`list_graphs`, `create_node`, …) this verb is the
prerequisite for. The same value is echoed explicitly under `blueprintObjectPath`; the bare
`.umap` package path (`/Game/Maps/<Map>`) is returned separately under `mapPath`.

Do **not** feed the `mapPath` value to `blueprint.graph.*`: a level-script Blueprint is a
sub-object of the `.umap`, so the bare package path is not loadable as a Blueprint and is
rejected with `[ASSET_NOT_FOUND]`. Reuse `assetPath` (or `blueprintObjectPath`) for all
graph-authoring calls. (Resolved.)
