# level-building

Workflow for taking a level from nothing to playable and good-looking — work from reference imagery if you have it, create and light the map, block out massing at scale and refine it, conform geometry to terrain, get terrain and water materials to actually reach the screen, capture what you built, and drive the whole thing from re-runnable build scripts. Proving what you built is [`level-review`](level-review.md); use [`visual-review`](visual-review.md) when you only need to capture one visual state.

## How To Read This Page

This page is the create/light/colour/place/refine through-line; bulky reference material is on the four linked topic pages below and at each relevant step. **Silent** failures can report success while the screen stays unchanged, so read those warnings before iterating.

## If You Are Matching A Reference, Register It First

Map scale is decided at the first step below and is expensive to change afterwards — every layer placed since inherits it — so a reference has to be resolved into numbers *before* the landscape exists, not after the first capture looks wrong.

Two rules carry most of the value; the rest are on [`level-building.working-from-references`](level-building.working-from-references.md):

- **Establish the pixel-to-world registration as its own step and publish its error.** Report the residual at the frame centre and at the edge separately, then work out what that error rules out before running any comparison. A comparison finer than the registration error manufactures differences that are not there and hides ones that are.
- **Never derive scale from the image's bounding box.** References are cropped and clipped without saying so. Anchor on an identifiable edge of the playable space and cross-check against a second, independent anchor; if the two disagree, that is a finding to resolve, not an average to take.

Also settle what the image *cannot* answer. A stylised render can be entirely faithful about art direction, biome and landmarks while carrying no usable layout at all, and a picture does not become able to answer a question just because nothing else can.

## Create The Level

| Need | Use |
|---|---|
| Flat, non-partitioned map | `level.create` |
| World Partition map | `level.structure.create_level` |
| Sculpted ground | `landscape.create`, then `landscape.sculpt` |

- `level.create` takes `levelPath` (full package path; wins when both are present) or `levelName` (short name, resolved under `/Game/Maps/`). It creates the level **and makes it the active editor world** — no `level.load` afterwards. It has no `save` argument, so persist it yourself.
- `level.structure.create_level` takes `levelName` (required), `levelPath` (destination *folder*, default `/Game/Maps`), `bCreateWorldPartition` (default `false`), and `save` (default `true`). Choose it for anything large or streamed: there is no programmatic in-place conversion of an already-created flat world.
- `level.load {levelPath}` is synchronous and makes that map the active world. Use it to reopen a map later, not immediately after `level.create`.
- `landscape.create {name, location, componentsX, componentsY, quadsPerComponent, sectionsPerComponent, materialPath}`. `quadsPerComponent` is the per-**subsection** edge and must be one of `7 / 15 / 31 / 63 / 127 / 255`; `sectionsPerComponent` must be `1` or `4`. Anything else is `INVALID_ARGUMENT`. The default 8x8 grid of 63 is roughly 500 m square.

**Only one client may change level state at a time.** Concurrent `level.load` / `level.save_as` / `editor.open_level` calls from several agents race the engine's level teardown and take the editor down with `!LevelList.Contains(TickTaskLevel)`. Read `editor.status` and work in the level that is already open; prefer operating on the current level over loading one.

## Light It Or See Nothing

A newly created level contains no lights, so every capture comes back black. Spawn illumination before judging anything.

| Call | Purpose |
|---|---|
| `lighting.spawn_light {lightType: "directional"}` | Key light. `lightType` accepts `point`, `directional`, `spot`, `rect`, `sky`. |
| `lighting.spawn_sky_light {recapture: true}` | Ambient fill; `recapture` re-captures the current scene into the cubemap. |
| `environment.spawn_sky_atmosphere {}` | Sky and horizon, so the background is not void black. |

`lighting.create_lighting_enabled_level {path}` is the one-shot alternative — it creates a new level that already carries directional + sky lighting. It takes only `path` and offers no World Partition option, so it is a shortcut for flat maps only.

`lighting.spawn_light` applies `properties.intensity` to the light **component** itself, so the common case needs no follow-up call. Anything the spawn verbs do not cover still lives on the component, not the actor — `property.set` against the actor path returns `PROPERTY_NOT_FOUND`. Write those through `actor.set_component_properties` against `LightComponent0`:

```js
call("actor.set_component_properties", { actorName: "Sun", componentName: "LightComponent0", properties: { Intensity: 5.0 } })
```

## Colour Without Authoring Materials

`/Engine/BasicShapes/BasicShapeMaterial` exposes a `Color` vector parameter and a `Roughness` scalar — enough for a readable palette while the level is still rough massing. It has **no emissive input**, so nothing built this way can glow; for a genuinely bright element use `/Engine/EngineMaterials/EmissiveMeshMaterial` (`Color` parameter) and expect it to be unlit, additive and two-sided, which washes out whatever is behind it.

```js
call("material.authoring.create_material_instance", {
  name: "MI_Block_Red",
  path: "/Game/MyLevel/Materials",
  parentMaterial: "/Engine/BasicShapes/BasicShapeMaterial",
  parameters: { vector: { Color: { r: 0.8, g: 0.12, b: 0.1, a: 1.0 } }, scalar: { Roughness: 0.9 } }
})
```

`name` is the instance leaf (a combined path may be passed as the `assetPath` alias) and `path` is always a **folder**. `parameters` is type-keyed: `{scalar, vector, texture, staticSwitch}`.

Spawn verbs can assign materials in the same call. `materialPath` binds slot 0 and `materialPaths` maps array index to material slot on `actor.spawn`, `actor.spawn_shape`, and `actor.spawn_batch`; batch spawning also accepts a batch-level default that one transform entry can replace. These are serialized per-instance component overrides, so persist them with `level.save`.

Use a duplicated primitive plus `static_mesh.set_material` only when you need the **mesh asset's shared default** to carry the material:

1. `asset.duplicate {sourcePath: "/Engine/BasicShapes/Cube", destinationPath: "/Game/MyLevel/Meshes/SM_Block_Red"}`
2. `static_mesh.set_material {assetPath: "/Game/MyLevel/Meshes/SM_Block_Red", materialPath: "/Game/MyLevel/Materials/MI_Block_Red", materialIndex: 0}`
3. Batch-spawn against the duplicate by `meshPath`.

**Never call `static_mesh.set_material` against `/Engine/BasicShapes/*`.** Those are shared engine assets — recolouring one rewrites every primitive everywhere that engine build is used. Duplicate into `/Game/` first, every time. The returned `slot` field is the slot *name*, not a result code; read `materialPath` back to confirm what was assigned.

## Placing Geometry At Scale

| Count | Use | Notes |
|---|---|---|
| One-off | `actor.spawn_shape` / `actor.spawn` | `spawn_shape` takes `shape` = `CUBE`, `SPHERE`, `CYLINDER`, `CONE`, `PLANE`; `actor.spawn` takes `classPath` or `meshPath`. Neither accepts a folder. |
| Tens–hundreds, structural | `actor.spawn_batch` | Exactly one source (`shape` / `meshPath` / `classPath` / `sourceActor`) plus `transforms`; pass `folder` inline. |
| Thousands, one repeating mesh | `foliage.add_instances`, or a HISM component | Instanced rendering — see [`level-building.instancing-and-scatter`](level-building.instancing-and-scatter.md). |
| Tight loops with no typed verb | `python.execute` | Synchronous on the game thread; check the returned Python `success`, not transport success. |

```js
call("actor.spawn_batch", {
  meshPath: "/Game/MyLevel/Meshes/SM_Block_Red",
  folder: "MyLevel/Walls",
  transforms: [ { location: { x: 0, y: 0, z: 50 }, scale: { x: 4, y: 0.5, z: 1 } }, { location: { x: 0, y: 800, z: 50 } } ]
})
```

Two batch-validation traps:

- `actor.spawn_batch` caps `transforms` at **512 per call** (over that is a hard `INVALID_PARAMS`). A non-object entry is skipped and reported in `skipped[]`, but an object that omits `location` is valid and spawns at the **world origin** — it is not dropped. Validate locations before the call, then check `count + skippedCount == requested`; counts alone cannot detect an accidental origin placement.
- `foliage.add_instances` drops any `transforms` entry without a valid `location`, and ignores `locations` entirely whenever `transforms` yields at least one instance. Neither is silent any more: both land in `skipped[]`/`skippedCount`, and the ignored-`locations` case also returns `ignoredLocationCount` plus a `warnings` entry. Compare `instances_count + skippedCount` against what you sent.

Past a few dozen copies of one mesh, stop spawning actors: instanced components, PCG generation, the `unreal.Rotator` argument-order trap that silently topples a whole layer, and spacing that reads as deliberate are on [`level-building.instancing-and-scatter`](level-building.instancing-and-scatter.md).

## Custom Shapes Beyond A Scaled Cube

When a scaled cube will not do, the `geometry` namespace authors a `DynamicMeshActor` in place: `create_box`, `create_cylinder`, `create_cone`, `create_ramp`, `create_stairs`, `create_spiral_stairs`, `create_arch`; `boolean_union` / `boolean_subtract` / `boolean_intersection` (`targetActor` + `toolActor`); `noise_deform` to rough a surface up; `array_linear` / `array_radial` for repeats; `extrude_along_spline` / `duplicate_along_spline` for paths.

Traps worth memorising:

- **`geometry.create_box`'s axis names mislead.** `width` is local X, `height` is local **Y**, and `depth` is the **vertical** local Z. An upright wall 400 wide, 500 tall, 80 thick is `{width: 400, height: 80, depth: 500}`.
- **`geometry.create_arch` lies flat** by default; `rotation: {roll: -90}` stands it apex-up, `roll: 90` points the apex down.
- **`array_linear` / `array_radial` merge the copies into the source mesh** — one mesh, not N actors. When you want separate actors, use `actor.spawn_batch`.
- The namespace requires the **GeometryScripting** engine plugin. Without it the methods are unregistered and calls return `PLUGIN_DISABLED`.

For organic or recursive shapes the RPC verbs cannot express, `python.execute` against the Geometry Script Python API (`unreal.GeometryScript_*` — swept polygons, lat-long spheres, `apply_perlin_noise_to_mesh2`) is far more expressive than the typed verbs and runs against the same `DynamicMeshActor`. Two things bite: the parts are overlapping shells, never boolean-unioned, so any lobe that stops overlapping after a noise pass **floats free** — test for it by bounding-box non-overlap; and plant the mesh by the base part's minimum Z, not the whole bounding box, or a low side lobe sets the bbox floor and leaves the body hovering.

Finish with `geometry.generate_collision {actorName, collisionType}` (`box`, `sphere`, `capsule`, `convex`, `convex_decomposition`, `auto`), then `geometry.convert_to_static_mesh {actorName, assetPath}` to bake a reusable asset. On the **create** path, materials are not carried, so follow with `static_mesh.set_material` per slot; source collision is carried only when you generated it first. On `overwrite:true`, the target's material slots, section map, other LODs, and existing collision remain preserved while LOD0 geometry is rewritten — do not reapply those merely because you rebaked.

When collision selection matters, finish the baked asset with `static_mesh.set_collision_complexity {assetPath, complexity}`. Prefer `project_default` or `simple_and_complex` for authored simple shapes; reserve `complex_as_simple` for static meshes that must collide against their render triangles. Do not use `complex_as_simple` on a mesh that must simulate as a movable rigid body.

## Precise Placement

Propose the arrangement visually, then confirm it by measurement. A screenshot never carries a metric coordinate.

| Intent | Call |
|---|---|
| Absolute transform | `actor.set_transform {actorName, location, rotation, scale}` — omitted fields stay unchanged. **One actor per call; there is no batch move.** |
| Relative shove | `actor.nudge {actorName, deltaWorld}`, or `deltaCamera {right, up, forward}` for "push it away from me" |
| Stated relation to an anchor | `spatial.place_relative {actor, anchor, relation, gap, align}` |
| Rest on whatever is below | `spatial.place_on_surface {actorName, dropDown: true}` |

**Known gap: no verb moves many actors in one call.** `spatial.ground_actors` is the only batch re-seat and it writes Z only, against a required `surface` filter. For an arbitrary bulk move, either re-run the pass that placed them (`actor.spawn_batch` takes per-entry transforms, capped at 512) or drive `actor.set_transform` from a script with a bounded, reported count — an unbounded per-actor loop over a whole level is what wedged an editor for 168 minutes with 5,100 uncancellable calls.

`relation` accepts exactly `on_top_of`, `below`, `left_of`, `right_of`, `in_front_of`, `behind`, `centered_on`, `against_wall`. `gap` (default `0`) is the centimetre clearance between the facing surfaces and is ignored for `centered_on`; `align` sets each non-relation axis to `min`, `center`, or `max`.

Confirm every placement pass numerically — see [`level-review`](level-review.md).

**Never assume `Z = 0` once a landscape exists.** Seating geometry on terrain, and getting landscape and water material edits to actually reach the screen, are on [`level-building.terrain-and-water`](level-building.terrain-and-water.md).

## See also

- [`level-building.working-from-references`](level-building.working-from-references.md) — telling an orthographic render from a perspective stitch, registering an image to world coordinates and reporting its error, chunked comparison, and sampling colour you can defend.
- [`level-building.terrain-and-water`](level-building.terrain-and-water.md) — ground height queries, conforming placements to terrain, landscape-material and water-material edits that only reach the screen after a rebuild.
- [`level-building.instancing-and-scatter`](level-building.instancing-and-scatter.md) — HISM and foliage instancing, PCG generation, Python transform traps, scatter spacing.
- [`level-building.capture-and-review`](level-building.capture-and-review.md) — pinned exposure, whole-level framing, orthographic capture, and what a review set must contain.
- [`level-building.build-scripts`](level-building.build-scripts.md) — outliner organisation and tags, one script per layer, modal-dialog deadlocks, saving and dirtying packages.
- [`level-review`](level-review.md) for the capture poses, framing math, and placement verification that close the loop.
- [`level`](level.md) and [`level.structure`](level.structure.md) for map creation, loading, and World Partition authoring.
- [`landscape`](landscape.md) for sculpting, height queries, and landscape materials.
- [`actor`](actor.md) for spawn, batch, folder, tag, and snapshot verbs.
- [`geometry`](geometry.md) for procedural mesh authoring and the primitive-orientation traps.
- [`spatial`](spatial.md) for placement relations, measurement, and coordinate conventions.
- [`water`](water.md) for water zones, bodies, and materials.
- [`safe-mutation-save`](safe-mutation-save.md) for the save-and-verify loop.
