# level-building.terrain-and-water

Query terrain height before placement, and rebuild landscape/water materials before judging the screen. Many bad calls report success while doing nothing.

## Conform To The Terrain

**Never assume `Z = 0` once a landscape exists.** Query every placement:

| Route | Call | Use when |
|---|---|---|
| Per-actor | `spatial.raycast {origin, direction, onlyClasses: ["LandscapeProxy"]}` | You need the height under one footprint. |
| Bulk / collision-free | `landscape.get_heights {landscapeName, region: {minX, minY, maxX, maxY}}` | You need many samples, or you need *terrain* height rather than "top of whatever is there". |

**Ask for the landscape by name or class, not "whatever is under here".** A downward trace returns the first blocker. With **`traceComplex: true`, a static mesh without simple collision resolves against raw render triangles**; foliage with *no collision* can therefore block a probe and return a treetop height. This is common on vegetated levels, not a rare edge case.

Filter the trace instead of guessing:

```js
call("spatial.raycast", {
  origin: { x: -20000, y: 5000, z: 100000 },
  direction: { x: 0, y: 0, z: -1 },
  traceComplex: true,
  onlyClasses: ["LandscapeProxy"]
})
```

- `onlyClasses` matches the actor class **or any ancestor**, case-insensitively; `["LandscapeProxy"]` needs no actor name. `onlyActors` and `actorFilter` (with `matchMode` / `caseSensitive`) filter names.
- Filtering peels rejected hits and traces behind them, so the landscape is still found; `filteredOut` / `filteredOutCount` report what was skipped. `simpleCollisionShapes: 0` plus `renderGeometryHit: true` identifies a render-geometry hit, and `warnings[]` names its actor.
- `multiHit: true` with `maxHits` (default `32`) returns nearest-first `hits[]`, one per actor. Missing `onlyActors` names return `ACTOR_NOT_FOUND`; `maxHits < 1` returns `INVALID_PARAMS`. Unresolved `ignoreActors` still succeed and appear in `unresolvedIgnoreActors`.
- These filters apply only to `spatial.raycast`; `spatial.raycast_screen` and `spatial.place_on_surface` still accept only `ignoreActors`. A clean miss is successful with `hit: false`.
- `landscape.get_heights` uses a **heightmap-pixel** region, not world units, and returns world-centimetre `minZ` / `maxZ` / `meanZ`. Convert with the landscape origin/draw scale: `px = (world_x - origin_x) / scale_x`; cache both from the actor.
- **Sample the footprint**, not only its centre: centre plus four to eight ring samples at the radius reveal slope; seat on the minimum. Cap ring deviation at `|dz| > min(150, 0.3 * radius)` to stop a clipping cliff/ravine dragging the structure underground or breaking mirrored pairs.
- Sink about 20 uu below the seat; on a slope use `0.6 * (centre - min)`, capped at about 120 uu, to keep a pivot-at-base mesh from hovering. Reject ground below the water surface unless underwater placement is intended; this, not spline width, keeps scatter out of a river.

## Terrain Material Traps That Cost Days

**A landscape ignores material edits—rewired graph or a single constant—until its per-component material instances rebuild.** The asset and compile can be correct while components render their cached old look.

`material.authoring.compile_material` now rebuilds them for you and reports what it reached:

```js
call("material.authoring.compile_material", { assetPath: "/Game/Terrain/M_Ground" })
// -> consumerRefresh: { measured: true, consumersFound: 1, consumersRefreshed: 1,
//                       refreshed: ["Terrain"], complete: true }
```

**Check `consumerRefresh.complete` and that the landscape is in `refreshed`.** These are measurements: `consumersRefreshed` counts landscapes whose component material-instance identity changed. If `complete` is false, `warnings[]` is present; use `landscape.set_material` on the named landscape (reassigning the **same** material is supported).

On older builds, compile and `set_material` did not reach the screen; the supported *round-trip* was assign a different material, then the intended one. It is no longer required. Separately, forcing a lane gate to 0 in a terrain master (which must erase the painted road network) moved a fixed frame **1.81%** (noise floor) before the graph-edit fix and **17.19%** after a round-trip. On such a build, “edited but nothing happened” is invalid unless the test included a round-trip. See [`landscape`](landscape.md).

**Never call `MaterialEditingLibrary.recompile_material()` from `python.execute`.** It triggers GC while the Python frame is on the stack; the Python pre-GC hook can fault and take the editor down with unsaved work. It is non-deterministic—the first call may return and the second may be fatal. Use native `material.authoring.compile_material`, which avoids GC and returns `compileSucceeded` / `compileErrors[]`. `UMaterial.post_edit_change()` is not an alternative; it does not exist on UE 5.8.

**Painting needs a declared layer.** The material must contain `LandscapeLayerBlend` (or `LandscapeLayerWeight`) entries with that name. The default material assigned by `landscape.create` declares none, so a fresh landscape has zero paintable layers. Painting now returns `LANDSCAPE_MATERIAL_NO_LAYERS` (none declared) or `LAYER_NOT_FOUND` with available names in `availableLayers[]`, rather than reporting success. `material.authoring.configure_layer_blend` writes names into `LandscapeLayerBlend`, wires it to `BaseColor` unless already occupied, and returns `targetLayers[]` plus `connectionState`. `connectionState: skipped_input_already_wired` means layers are paintable but not sampled. On paint, check `texelsWithWeight > 0`, not the envelope. See [`landscape`](landscape.md).

During greybox, a material deriving color from world position or slope needs no layers, weightmaps, or paint calls and has no blend seams. It can also be a finished stylized look; use painted layers only for variation the procedural rule cannot express.

## Water

Create the Water plugin's zone first: `water.spawn_water_zone {location, extent}`—**at least one `AWaterZone` must exist or nothing renders**—then `water.spawn_water_body {type: "River" | "Lake" | "Ocean" | "Custom", name, location}`, shape its `WaterSpline` with `spline.set_spline_point_position` (there is no `splinePoints` parameter), and set material with `water.set_water_body_material`.

What the parameters do not tell you:

- **The visible shoreline is the terrain/water intersection, not body width.** `RiverWidth` bounds the footprint, but terrain occludes surface above water Z; surface Z relative to the bed controls apparent width, color, and depth. Fit spline Z to measured bed before trimming width—a few hundred units can flood a valley thousands wider.
- **Set width/depth with `water.set_river_width_at_spline_point` / `water.set_river_depth_at_spline_point`, never `spline.set_spline_point_scale`.** `Scale.X`/`Scale.Y` derive from `UWaterSplineMetadata`'s `RiverWidth`/`Depth` and re-derive on load. A scale write can read back and save, then revert: one session's committed 3490.6 became 4800 after reload. The spline verb now refuses water-spline scale with `DERIVED_PROPERTY` and names the correct verb. Position, rotation, and tangents are not derived and still use `spline.*`.
- **Clear the zone's far-distance mesh** on a river-only map. `AWaterZone`'s `UWaterMeshComponent` supplies `FarDistanceMaterial` and `FarDistanceMeshExtent` of 4,000,000 uu in this build, painting at water height beyond every body and inflating bounds.
- **Shoreline stepping is WaterInfo texel size**: zone extent / render-target resolution. 80000/2048 = 39 uu per texel (visible); 66000/4096 = 16 uu (not visible). Landscape quad size is the lower floor.
- **`bAffectsLandscape` re-sculpts terrain under the body.** Leave it off when another system owns the sculpt and verify after every write.
- WaterInfo populates lazily. A fresh-restart capture before zone rebuild shows pale/cyan water unlike the authored material; force a rebuild before judging color.

**Water material edits are invisible until the body's dynamic material rebuilds.** The component creates a *transient* instance from `water_material` and snapshots its parent; the mesh draws that snapshot. Parameter changes or re-parenting therefore keep the old look. For a diagnostic, set an absurd value such as extreme `Scattering`: **pixel-identical output means a stale instance, not a broken material.**

Re-assigning the property rebuilds it. Set `water_material` to `None`, then set it back:

```python
comp = water_body.get_editor_property('water_body_component')
mi = comp.get_editor_property('water_material')
comp.set_editor_property('water_material', None)
comp.set_editor_property('water_material', mi)     # rebuilds the transient instance
```

`on_water_body_changed()` does **not** rebuild it. Map load does, explaining changes that appear only after restart. Reassign after every water-material edit and capture afterward.


## See also

- [`level-building`](level-building.md) for the rest of the build workflow.
- [`landscape`](landscape.md) for sculpting, height queries, and the landscape-material error table.
- [`water`](water.md) for water zones, bodies, and materials.
- [`level-review`](level-review.md) for re-verifying every layer that sits on ground you just changed.
