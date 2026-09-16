# foliage

Edit foliage assets and placed foliage instances in the current level, including foliage type setup, procedural foliage creation, painting, instance queries, and removal.

Use this namespace for Unreal foliage systems rather than generic actor placement; it works with foliage types and instanced foliage data instead of standalone StaticMeshActor edits.

## See also

- [`level-building.instancing-and-scatter`](level-building.instancing-and-scatter.md) — choosing between this namespace and a HISM component you own. There is **no typed verb** for owned-HISM scatter; that page carries the `python.execute` recipe, including the two writes that silently do nothing without it.
- [`level-building`](level-building.md) — scattering as one pass of the level-building guide.
- [`landscape`](landscape.md) — heightfield terrain and landscape-material grass, a different system from the foliage tool.

## An auto-created foliage type is dirty, not written — and the response now says so

`foliage.paint` and `foliage.add_instances` both accept a **static-mesh** path and stand a
`UFoliageType` up for it at `/Game/Foliage/Auto_<Mesh>.Auto_<Mesh>`. That asset is **marked dirty
and never written**: nothing in either verb writes a `.uasset`, and nothing writes the level package
holding the instances either. A scatter therefore survives only as long as the editor does, unless
you flush both.

Only the auto-create branch reports this — pass a real `UFoliageType` path and none of these fields
appear, because that call created and dirtied nothing:

- `saveRequested` / `markedForSave` — what the verb was asked to do and what it did.
- `saved` — **measured**, not assumed: the type's package is clean *and* has a `.uasset` on disk.
  A freshly created type answers `false` with `pendingFlush:true`. A **reused** auto-type that was
  already saved answers `true`.
- `foliageType.existsOnDisk` / `foliageType.existsAfter` / `foliageType.pendingSave` — the same
  measured triple `asset.*` verbs publish, nested under `foliageType` so it cannot be confused with
  the top-level `existsAfter`.
- a `warnings[]` line naming the remedy whenever the type is not durable.

**`existsAfter` at the top level is about the foliage ACTOR, never about durability.** It used to be
the literal `true`; it is now the measurement that the `foliageActorPath` the response publishes
resolves back to that actor. Read `saved` / `pendingFlush` / `foliageType.existsOnDisk` for
persistence.

To make a scatter durable: `asset.save` on the echoed `foliageTypePath`, then `level.save` (or one
`editor.save_all`). Saving the type alone leaves the instances in memory; saving the level alone
leaves it referencing a package that is not on disk.

`foliage.add_type` publishes the same `saveRequested` / `markedForSave` / `saved` / `pendingFlush`
triple over its own `McpSafeAssetSave`, and its `exists_after` is mirrored from the measured
`existsAfter` rather than being a second, contradicting literal.

## Four instance counts, and only one of them is what draws

A foliage response can carry four numbers for the same instances. They are **not** four names for
one quantity, and reading the wrong one is how a scatter that renders nothing gets certified green.
Outermost first:

| field | reads | means |
| --- | --- | --- |
| `count` / `instances_count` / `instancesPlaced` | `FFoliageInfo::Instances` | the editor-side **ledger**. |
| `renderedInstanceCount` | `PerInstanceSMData.Num()` | a **second array**, on the component. |
| `builtInstanceCount` | `NumBuiltInstances` | the instances in the built **HISM cluster tree**. |
| `expectedDrawnInstances` | ledger × `foliage.DensityScale` | of those, how many survive the density **cull**. |

**`ledgerMatchesRendered` compares the first two, and a single `AddInstance` writes both of them
together** — so it is `true` by construction on any write, and it cannot be the check that tells you
a scatter is on screen. That is what `builtInstanceCount` is for: a HISM draws from its cluster
tree, so an instance not counted there is invisible from every camera, at every distance, with
nothing else in the response disagreeing.

**`builtInstanceCount` and `expectedDrawnInstances` are different layers and must not be compared to
each other.** `builtInstanceCount` is *membership* — is the instance in the tree at all — and a
shortfall means a **stale tree**, repaired by rebuilding (any add through these verbs does it, and
so does saving the level: `UHierarchicalInstancedStaticMeshComponent::Serialize` rebuilds on its way
to disk, so the saved level is always correct and nothing is ever lost to this). `expectedDrawnInstances`
is the *cull* the renderer applies to instances that are already in the tree, and a shortfall there
is a **scalability setting**, repaired by raising `foliage.DensityScale`. Neither is a camera or
frustum figure; no field in this namespace is.

`builtInstanceCount` and `clusterTreeUpToDate` (the component's `IsTreeFullyBuilt()`) are **omitted,
never zeroed**, when the scope holds foliage that does not draw from a HISM cluster tree — actor and
ISM-actor foliage types, whose instances live on spawned actors or behind a partition actor's
`FISMClientHandle`. A `clusterTreeWarning` names which case you are in.

**The write verbs rebuild; the read verb does not.** `foliage.add_instances` and `foliage.paint`
rebuild the tree after their batch, because the engine's own add path suppresses that rebuild for
the duration of the add and does not restore it (`FFoliageInfo::AddInstancesImpl` is the only
bracketed mutator in the engine's foliage file that does not close with a `Refresh`).
`foliage.get_instances` reports a stale tree and leaves it stale — a read verb that repaired the
level would hide the writer that broke it.

### foliage.get_instances

Reads back placed foliage instances. Pass `foliageTypePath` to scope the read to one foliage type; omit it to list instances of every type.

**A `foliageTypePath` that does not resolve to a `UFoliageType` is rejected `ASSET_NOT_FOUND`** — both a path naming no asset and one naming some other asset type. It used to answer `success:true` with an empty `instances[]`, which is indistinguishable from a type that genuinely has none: a caller branching on emptiness concluded its own scatter had not happened. The filter is resolved before the world is touched, so the verdict does not depend on whether the map owns a foliage actor, and the bare package handle (`/Game/Foliage/X`) is accepted alongside the `Package.AssetName` object path. A resolvable type with nothing painted still returns an honest `count:0` with the full field set.

Each entry returns the full transform — location (`x`, `y`, `z`), rotation (`pitch`, `yaw`, `roll`), and scale (`scaleX`, `scaleY`, `scaleZ` from `DrawScale3D`) — so `foliage.add_instances` size/orientation can be verified. Without `foliageTypePath`, each entry also includes `foliageType` (the type's object path); the top-level response carries `instances`, `count`, `orphanedInstanceCount`, `renderedInstanceCount`, `ledgerMatchesRendered`, and `foliageActorPath`.

**Two representations, both reported.** `instances`/`count`/`orphanedInstanceCount` come off `FFoliageInfo::Instances`, the editor-side bookkeeping record. `renderedInstanceCount` is what the level actually **draws** over the same scope, read off the foliage components. The engine holds these equal as an invariant but only asserts it under `DO_FOLIAGE_CHECK`, which ships at `0`, so a writer that touched one side and not the other leaves them divergent with nothing complaining. `ledgerMatchesRendered` is the verdict — `renderedInstanceCount == count + orphanedInstanceCount` (`count` excludes orphans, `renderedInstanceCount` includes them). **When it is `false`, neither number alone describes the level** and the divergence is live: `FFoliageStaticMesh::Reapply` reconciles in the record's favour on the next `PostEditUndo` or foliage-type edit, deleting whatever the components still hold in excess.

**The third representation, and the one the two above cannot see.** `builtInstanceCount` is the component's `NumBuiltInstances` — how many of those instances are in the built HISM cluster tree — and `clusterTreeUpToDate` is its `IsTreeFullyBuilt()`. `renderedInstanceCount` and `count` are two arrays a single `AddInstance` writes together, so **`ledgerMatchesRendered` cannot go false on a write that left the tree unbuilt**; `builtInstanceCount` can, and `clusterTreeWarning` says so when it does. Both fields are omitted rather than zeroed when the scope holds non-HISM (actor / ISM-actor) foliage. See *Four instance counts* above for how this differs from `expectedDrawnInstances`. **This verb never rebuilds a stale tree it finds** — reading is not repairing, and a read that silently fixed the level would hide the writer that broke it. To rebuild, add or paint one instance of the type, or save the level.

This round-trips everything `foliage.add_instances` accepts: `scale` written as an object `{x,y,z}`, an array `[x,y,z]`, or a `uniformScale` scalar all read back through the `scaleX`/`scaleY`/`scaleZ` keys.

### foliage.remove

Removes placed foliage instances. Supply **exactly one** scope: `foliageTypePath` (a single foliage type — removes only that type's instances) OR `removeAll:true` (every instance of every type).

Precedence and edge inputs are explicit:

- **`removeAll` wins over a co-supplied `foliageTypePath`.** If you pass both, `removeAll:true` clears **all** types and the named path is ignored for scope purposes — it is not resolved or required (a missing/wrong-type path does not block the wipe). The one exception is safety: a syntactically unsafe path (e.g. path traversal) is still rejected `SECURITY_VIOLATION` before any wipe. The response echoes the interpretation as `mode` (`"all"` when a wholesale wipe was applied, `"type"` for a scoped removal) so the over-broad clear is never silent — check `mode` if you co-supply both.
- **A `foliageTypePath` that does not resolve to a foliage type is rejected `ASSET_NOT_FOUND`**, not confirmed as a success with `instancesRemoved:0`. This covers both a missing asset and an existing asset that is not a `UFoliageType` (e.g. a StaticMesh or Material) — a fat-fingered / typo'd / wrong-type path fails loudly instead of falsely reporting a removal. (A path that *does* resolve to a foliage type but currently has no painted instances honestly returns `instancesRemoved:0` with `mode:"type"`.)
- **Omitting both `foliageTypePath` and `removeAll` is rejected `INVALID_ARGUMENT`** ("specify foliageTypePath or set removeAll:true") rather than a silent no-op success.

On success the response carries `success:true`, `instancesRemoved` (count actually cleared), `mode` (`"all"` | `"type"`), and `foliageActorPath`.

Removal goes through `FFoliageInfo::RemoveInstances`, so it withdraws each instance from the component that draws it as well as from the bookkeeping record. To verify a removal independently, read `renderedInstanceCount` (not just `count`) from `foliage.get_instances` — see that verb above for why the two can differ.

### foliage.add_instances

Places one or more instances of a foliage type. The registry lists `transforms` and `locations`, but not their nested shapes:

`foliageTypePath` (required) is the asset path to a `UFoliageType`, OR a static-mesh path — in which case a `UFoliageType` is auto-created from the mesh at `/Game/Foliage/Auto_<MeshName>.Auto_<MeshName>`, and `foliage.paint` reuses that same asset for the same mesh. A bare name (no `/`) resolves under `/Game/Foliage/<name>`. The response echoes the resolved `foliageTypePath`; **use that string** for every later `foliage.*` / `asset.*` call — it is an object path that resolves, and it is the same on a repeat call. The auto-created type is keyed on the mesh basename alone, so it is level-global: two callers scattering the same mesh share one type. The asset is created and marked dirty, not written to disk — the response's measured `saved` / `pendingFlush` / `foliageType.existsOnDisk` fields say so, and the remedy is in **An auto-created foliage type is dirty, not written** on the namespace page.

`transforms` is an array; each entry is `{ location, rotation, scale }`:

- `location` — **required**. Object `{"x":0,"y":0,"z":0}` or array `[x,y,z]`. An entry with **no valid location places nothing**, so always include it. The drop is no longer silent: it is listed in the response's `skipped[]` array as `{index, reason}` and counted in `skippedCount`.
- `rotation` — optional. Object `{"pitch":0,"yaw":45,"roll":0}` or array `[pitch,yaw,roll]`. Defaults to zero.
- `scale` — optional. Object `{"x":1,"y":1,"z":2}`, array `[x,y,z]`, OR a scalar via the sibling key `"uniformScale": 2` (applies the value to all three axes). Defaults to `{1,1,1}`. Note `uniformScale` is a key on the transform entry, not inside `scale`.

```jsonc
{ "foliageTypePath": "/Game/Foliage/Rocks",
  "transforms": [
    { "location": {"x":0,"y":0,"z":0},
      "rotation": {"pitch":0,"yaw":45,"roll":0},   // optional
      "scale":    {"x":1,"y":1,"z":2} },             // or [x,y,z], or omit scale and pass "uniformScale": 2
    { "location": [200,0,0], "uniformScale": 1.5 } ] }
```

`locations` is the legacy shortcut: an array of `{x,y,z}` positions with default rotation/scale. It is used **only when `transforms` is empty/absent**; if both are supplied, `transforms` wins and `locations` is ignored. Each ignored position is reported in `skipped[]` and counted in `skippedCount` (subject to the 32-entry cap); the uncapped total is `ignoredLocationCount`, and one `warnings[]` string names the precedence rule. Those two fields are omitted unless both arrays were supplied and `transforms` won.

**Partial-success readback.** `skippedCount` is always present (`0` on a clean batch). When non-zero, `skipped` carries `{index, reason}` rows capped at 32 (`skippedTruncated:true` flags the cap). Reasons name their source array (`transforms[] entry is not an object`, `transforms[] entry has no valid location; supply an object {x,y,z} or an array [x,y,z]`, `locations[] entry is not an object`, `locations[] entry ignored because transforms was also supplied - transforms takes precedence`) because both arrays share an index space. `instances_count` + `skippedCount` accounts for every entry sent.

**The batch ends with a cluster-tree rebuild, and the response measures it.** The engine's add path holds `bAutoRebuildTreeOnInstanceChanges` false for the whole batch and restores it without rebuilding, so before this fix a scatter was stored, saved and reloaded correctly and **drew nothing in the session that placed it** — with `instances_count`, `renderedInstanceCount`, `ledgerMatchesRendered` and `expectedDrawnInstances` all agreeing on a number the viewport did not contain. The verb now closes with `FFoliageInfo::Refresh`, the same call the engine's own removal path makes, and reports `builtInstanceCount` (the component's `NumBuiltInstances`) and `clusterTreeUpToDate` measured afterwards. Both are omitted, never zeroed, when the type has no cluster tree to read; `clusterTreeWarning` fires if the tree ends up short. See *Four instance counts* above.

### foliage.add_type

Creates one `UFoliageType_InstancedStaticMesh` from a static mesh and saves it. Two arguments decide where it lands, and they are not interchangeable.

**`name` is a BARE asset name, never a path.** It is validated against the engine's own object-naming rules (`FName::IsValidXName` / `INVALID_OBJECTNAME_CHARACTERS`) and the composed package path against `FPackageName::IsValidLongPackageName`, so a `/`, `\`, `.`, `..`, a leading or trailing slash, a space or an unmounted root is rejected `INVALID_ARGUMENT` with the engine's own reason text quoted. This is not pedantry about naming: a name beginning with `/` used to compose `/Game/Foliage//Game/...`, and `CreatePackage` logs a double slash at **Fatal**, which is not compiled out in any configuration — the call did not fail, the editor **process** died, taking every unsaved package in that editor with it.

**`savePath` (optional, default `/Game/Foliage`) is how you choose the folder.** Same spelling, same validation and the same `SECURITY_VIOLATION` on an unsafe value as `foliage.create_procedural`; a path containing `..` or sitting outside a mounted root is refused before anything is created. The effective folder is always echoed as `save_path` whether you supplied it or not, so the destination is never a literal you have to know.

The response also carries `asset_path` (the object path, which round-trips through `asset.exists` / `foliage.add_instances` / `foliage.remove`), `used_mesh`, and `densityScalingEnabled` read back off the asset rather than echoed from the request.

### foliage.create_procedural

Creates a `UProceduralFoliageSpawner` under `savePath` (default `/Game/ProceduralFoliage`) plus one auto-created `UFoliageType` per entry, then runs the tile simulation. The registry names `bounds` and `foliageTypes`, but not their nested shapes:

`name` is a bare asset name here too — it becomes `<name>_Spawner` and each type's `<name>_Spawner_FT_<n>`, and a path-shaped value is rejected `INVALID_ARGUMENT` for the reason spelled out under `foliage.add_type` above. `savePath` is the only argument that chooses a folder.

`bounds` (required) is `{ "location": {x,y,z}, "size": {x,y,z} }`:

- `location` — object `{"x":0,"y":0,"z":0}` (defaults to origin if omitted).
- `size` — object `{"x":2000,"y":2000,"z":500}` **or** array `[x,y,z]` (defaults to `{1000,1000,1000}` if omitted).

`size` is the volume's **full** extent on each axis, centred on `location`, and it becomes the brush box the simulation samples — the spawned `AProceduralFoliageVolume` is left at identity scale. A `{2000,2000,500}` request therefore covers `location ± (1000,1000,250)`.

`foliageTypes` (required) is an array; each entry accepts `meshPath` (a static-mesh path), `density` (number, default 10), and the same scale/align keys `foliage.add_type` takes: `minScale` / `maxScale` (numbers, default 1, applied as one uniform interval across X/Y/Z) and `alignToNormal` (bool, default true). A range that is non-positive or inverted (`minScale > maxScale`) does not fail the whole call — that one entry is skipped with the reason in `skipped[]`. `randomYaw` is **not** read here (unlike `foliage.add_type`); every unread key you passed is echoed back in the response's `ignoredFields` array.

Entries that contribute no foliage type (not an object, no `meshPath`, or a `meshPath` that does not load as a `UStaticMesh`) are reported like `foliage.add_instances` drops: `skippedCount` (always present) and `skipped` rows capped at 32 (`skippedTruncated:true` flags the cap). `foliage_types_requested` echoes the input length, so `foliage_types_count` + `skippedCount` == `foliage_types_requested`.

**The simulation half of each entry.** `UFoliageType` carries two disjoint property sets — a **painting** half the Foliage-editor brush reads, and a **procedural** half `FProceduralFoliageTile::Simulate` reads. This verb drives the procedural one, so `density` / `minScale` / `maxScale` above are the *paint* knobs and the keys below are what actually decide the scatter. All are optional; an absent key leaves the engine default untouched, and a value the engine would clamp or reject skips that one entry with the reason in `skipped[]`.

| key | writes | note |
| --- | --- | --- |
| `initialSeedDensity` | `InitialSeedDensity` | seeds along 10 m, **implicitly squared** over 10 m × 10 m. This — not `density` — is what the seeding loop multiplies by the tile area. Omit it and the verb derives `sqrt(density)` so the two units line up. |
| `collisionRadius` / `shadeRadius` | same | overlap radii in cm; `max(collisionRadius, shadeRadius)` is the **effective radius** the priority rule is ordered on. |
| `numSteps` | `NumSteps` | how many times the species ages and spreads. |
| `seedsPerStep` | `SeedsPerStep` | seeds cast per step. |
| `averageSpreadDistance` | `AverageSpreadDistance` | cm between a parent and its seeds. |
| `maxAge` / `maxInitialAge` | same | see the age/scale interaction below. |
| `overlapPriority` | `OverlapPriority` | see the ordering rule below. |
| `canGrowInShade` / `spawnsInShade` | `bCanGrowInShade` / `bSpawnsInShade` | read as `canGrowInShade && spawnsInShade`. |
| `randomPitchAngle` | `RandomPitchAngle` | degrees, 0–359. |
| `proceduralScale` | `ProceduralScale` | interval `{min,max}` or `[min,max]`; `min` ≥ 0.001. |
| `height` / `groundSlopeAngle` | same | interval; placement **filters**, see below. |

**Four interactions, each of which makes a correct-looking input do nothing.**

1. **`proceduralScale`, not `minScale`/`maxScale`, is what a procedurally placed instance is sized by.** `FPotentialInstance::PlaceInstance` reaches `GetRandomScale()` (which reads `ScaleX/Y/Z`) only on the non-procedural branch; the procedural branch takes `GetScaleForAge()`, which interpolates `ProceduralScale`. Both are written, because `foliage.add_type` shares the scale helper and its painting path needs `ScaleX/Y/Z`.
2. **`maxInitialAge` is what makes `proceduralScale` observable.** `GetInitAge` returns `MaxInitialAge × random`, so at the engine default of `0` every seed starts at age exactly 0; ages then advance by integer 1 per step, and `GetScaleForAge` evaluates the curve at `Age / MaxAge`. A `proceduralScale` range therefore collapses to `numSteps + 1` discrete sizes. **When you supply `proceduralScale` and no `maxInitialAge`, this verb raises `maxInitialAge` to the effective `maxAge`** and reports `max_initial_age_source: "raised_for_procedural_scale"` plus a `warnings[]` line. Pass `maxInitialAge` explicitly — `0` included — to override that.
3. **`overlapPriority` must be ordered the same way as effective radius.** The engine removes the instance with the *lower* priority, and the contest is entered once per overlapping pair, so a small-radius species at equal or higher priority than a large one wins far more contests than its share of the area and thins or wipes the large one out. The verb holds every entry's radii and priority in one call, compares the two orders, and adds a `warnings[]` line naming both entries when they disagree. It warns rather than refuses — a deliberate inversion is legal, if rare.
4. **`height` and `groundSlopeAngle` are filters, not niches.** Both are `Category=Placement` rejection ranges applied *after* the terrain-free tile simulation, whose results are projected onto the world afterwards. Setting `height` per species produces a *sparser* band, not a differently populated one; it cannot give species elevation niches. Supplying either adds a `warnings[]` line saying so.

`seed` (optional) sets the spawner's random seed (default 12345).

`tileSize` (optional, cm, default 1000), `numUniqueTiles` (optional, default 10) and `tileOverlap` (optional, cm, default 0) drive the simulation grid: the spawner simulates `numUniqueTiles` unique `tileSize`-square tiles, overlapping neighbours by `tileOverlap`, and combines them across `bounds`. The defaults mean ten 10 m tiles, which repeats visibly on a volume of a few hundred metres and gives no variation at all on one smaller than 10 m — raise `tileSize` toward the volume's own extent for a large scatter. **Note the divergence: the engine's own `UProceduralFoliageSpawner` default is `TileSize = 10000` (100 m); this verb keeps 1000 so existing callers' scatters do not change silently.** `tileSize` and `numUniqueTiles` are rejected `INVALID_ARGUMENT` if non-positive (`numUniqueTiles` below 1), `tileOverlap` if negative. `tile_size` and `num_unique_tiles` are always echoed, supplied or defaulted, so the grid is never invisible; `tile_overlap` is measured off the spawned volume's procedural component and is therefore **omitted** in the (pathological) case where the volume has no such component and nothing simulated at all — a `0` there would contradict a caller who asked for 100.

`savePath` (optional, default `/Game/ProceduralFoliage`) is the content folder the spawner and its `_FT_<n>` types are written to. It must resolve under a mounted root — anything else is `SECURITY_VIOLATION`. The effective folder is echoed as `save_path`.

**Reading the result.** `foliage_types[]` carries one row per type actually built, with the simulation values **as written to the asset** (not as requested): `initial_seed_density` + `initial_seed_density_source` (`"supplied"` or `"derived_from_density"`), `procedural_scale`, `max_age`, `max_initial_age` + `max_initial_age_source` (`"supplied"`, `"default"` or `"raised_for_procedural_scale"`), `num_steps`, `seeds_per_step`, `average_spread_distance`, `collision_radius`, `shade_radius`, `effective_radius`, `overlap_priority`, `can_grow_in_shade`, `spawns_in_shade`, `spawns_in_shade_effective`, `random_pitch_angle`, `height`, `ground_slope_angle`, plus the paint-side `density` and the `asset_path`. Read `initial_seed_density` rather than `density` when you want to know what the scatter was seeded at.

**Did the scatter actually place anything?** `instances_spawned` is the number of instances in the world carrying this volume's `ProceduralGuid` — measured off the foliage actors *after* the simulation, so it is what this call produced and not a delta that would also absorb a concurrent foliage edit. `hand_placed_instances_in_world` is published beside it: the world's hand-painted (non-procedural) total, which this verb produces none of. They are two different sets, and neither substitutes for the other. **Both are omitted, never reported as `0`, when there was nothing to measure** (no procedural component on the spawned volume) — a `warnings[]` line says so, so a missing field means "not counted" and a `0` means "nothing landed". `resimulated` is a weaker signal than either: it says only that the FoliageEdit resimulation path was reachable and dispatched, not that foliage landed.

**A `0` here almost always means no surface.** The tile simulation is terrain-free; its results are projected onto world geometry afterwards by a downward `WorldStatic` sweep. A volume over empty space scatters nothing, and the verb adds a `warnings[]` line naming that cause — the same condition the Unreal editor raises its "Unable to spawn instances" toast on, which an MCP caller never sees. Put a landscape or a static-mesh surface inside the volume's Z range.

**Historical note, because it was read as evidence three times:** `instances_spawned` used to be a before/after delta of `FFoliageInfo::GetPlacedInstanceCount()`. That engine function counts instances whose `ProceduralGuid` is **not** valid — the hand-placed ones — so it was blind to every instance a procedural scatter makes and the field had exactly one reachable value, `0`. A `0` from a build predating this fix carries no information at all.

**Still unreachable through this verb.** Per-type, and named in `ignoredFields` if you pass them: `spreadVariance`, `distributionSeed`, `maxInitialSeedOffset`, `scaleCurve` — plus `randomYaw`, which `foliage.add_type` does read. Spawner-level: `minimumQuadTreeSize`, which is not a `foliageTypes[]` key at all and is refused `UNKNOWN_PARAMS` as a top-level one.

```jsonc
{ "name": "MeadowSpawner",
  "bounds": { "location": {"x":0,"y":0,"z":0}, "size": {"x":2000,"y":2000,"z":500} },
  "foliageTypes": [
    { "meshPath": "/Engine/BasicShapes/Cube", "density": 300,
      "minScale": 0.6, "maxScale": 1.8, "alignToNormal": false,
      // The simulation half. Big species first, and its priority is the highest
      // because its effective radius is the largest.
      "collisionRadius": 300, "shadeRadius": 400, "overlapPriority": 10,
      "proceduralScale": { "min": 0.8, "max": 2.0 }, "maxInitialAge": 10,
      "initialSeedDensity": 2, "numSteps": 3, "seedsPerStep": 3 },
    { "meshPath": "/Engine/BasicShapes/Sphere", "density": 900,
      "collisionRadius": 60, "shadeRadius": 60, "overlapPriority": 1,
      "canGrowInShade": true, "spawnsInShade": true } ],
  "seed": 12345, "tileSize": 2000, "numUniqueTiles": 4, "tileOverlap": 100 }
```

### foliage.paint

**Painting projects only when you name a surface.** Supply `surface` and every requested XY is
seated onto that ground through `GroundPlacement::SeatInstance` — the same measure/seat solve
`spatial.ground_instances` uses, so the two verbs cannot disagree about where the ground is.
Omit it and the verb writes the literal `z`: no trace, no projection, no normal alignment, no
collision check. Which of the two happened is always stated — `projected` is on every response,
and the unprojected branch adds a `warnings[]` line naming `surface` as the remedy. Before this,
an unprojected batch and a seated one produced identical responses.

`projectToGround` defaults to `true` when `surface` is present and `false` when it is not, so
naming a surface is the whole opt-in. `projectToGround:true` with no `surface` is
`INVALID_SURFACE_SPEC`, never a guess at a preset.

`placed[]` carries one row per instance on **both** branches — `{index, x, y, z, pitch, yaw, roll,
alignedToNormal}`, capped at 32 with `placedTruncated` — because an unprojected batch still has a
rotation. While projecting the row also carries `deltaZCm` and `groundProvenance`, and the
response adds `projectedCount` and `surfacePreset`. `deltaZCm` is the signed drop from the
requested `z`, which is what distinguishes a request that already sat on the surface from one that
was never moved.

`groundProvenance` is the same block `spatial.ground_actors` / `verify_grounding` /
`ground_instances` publish, from the same writer, so an equivalent probe reports identical facts
whichever verb issued it: `traceComplex`, the hull-vs-triangle column counts, `surfaceComponents[]`
naming the primitive that answered (`{actor, component, componentClass, columns}`), and the
`surfaceTrust` verdict with the `warning` that names any untrustworthy surface. Read it before
trusting a scatter — an instance seated on a neighbouring HISM instead of on terrain is invisible
in `z` and `deltaZCm` alone; `surfaceTrust: "untrusted"` plus the scatter warning now says so
outright, and `componentClass` is where it shows.
The block is omitted for an instance whose probe found no supported column, because it would then
describe nothing. See [`spatial.ground-placement`](spatial.ground-placement.md).

Seating is **bounds-based and tangent**: the instance's underside comes to rest on the highest
accepted ground under its footprint, with no embedding. An instance whose column finds no accepted
ground is **withdrawn** rather than left floating, and reported in `skipped[]` with the solve's own
reason. Only instanced-static-mesh foliage types can project — an actor foliage type has no
footprint to seat and is refused up front.

**Rotation comes from the foliage type, not from a zero rotator.** `RandomYaw` and
`RandomPitchAngle` are drawn per instance on both branches — they need no surface — and
`AlignToNormal` tilts the instance onto the ground normal the seat solve already measured, so it
is applied only while projecting. The order and the `FOLIAGE_NoRandomYaw` flag on the
`RandomYaw:false` branch are the engine's own `FPotentialInstance::PlaceInstance`. The `rotation`
block echoes what the type asked for (`randomYaw`, `randomPitchAngleDeg`, `alignToNormal`,
`alignMaxAngleDeg`) next to the **measured** `alignedCount`, and an `AlignToNormal` the verb could
not honour — no surface, so no normal — is stated in `warnings[]` rather than dropped. Instance
**scale and ZOffset are still hardcoded** to 1 and 0: the type's `ScaleX/Y/Z` and `ZOffset` ranges
are not read here. Use `foliage.add_instances` when you need per-instance scale.

**Painting rebuilds the HISM cluster tree on both branches, and reports the result.** The engine's
add path suppresses that rebuild and does not restore it. What used to save the **projecting**
branch was a side effect of the seat solve's `PostMoveInstances`, which reaches
`HISM::UpdateInstanceTransform`'s own `BuildTreeIfOutdated` only because the editor never takes that
function's in-place branch — an accident of an unrelated engine gate. The **unprojected** branch had
no such accident, so a paint without `surface` stored instances the level never drew. The rebuild is
now explicit on both. `builtInstanceCount` (the component's `NumBuiltInstances`) and
`clusterTreeUpToDate` are measured after it and are omitted, never zeroed, when there is no cluster
tree to read. See *Four instance counts* above for why this is a different layer from
`expectedDrawnInstances`.

`locations[]` entries require **all three** of `x`, `y` and `z`. An entry missing any of them is
reported in `skipped[]` (`{index, reason}`, capped at 32, true total in `skippedCount`) instead of
being placed at the world origin, and so is any entry that is not an object. While projecting, the
supplied `z` only says where the ground probe starts.

`foliage.add_instances` is the richer literal-placement verb (per-instance rotation and scale) and
does not project at all.

Passing a **static-mesh** path here auto-creates a `UFoliageType` that is marked dirty and **not
written to disk**. The response then carries the measured `saved` / `pendingFlush` /
`foliageType.existsOnDisk` fields and a `warnings[]` remedy — see **An auto-created foliage type is
dirty, not written** on the namespace page. Top-level `existsAfter` is about the foliage actor, not
durability.

```jsonc
{ "foliageTypePath": "/Game/Foliage/FT_Grass",
  "locations": [ {"x": 0, "y": 0, "z": 5000}, {"x": 250, "y": 0, "z": 5000} ],
  "surface": { "preset": "landscape" } }
```
