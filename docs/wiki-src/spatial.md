# spatial

Deterministic spatial reasoning over the editor world: cast rays, measure distances and overlaps, verify placements, and rest or relate actors by world-AABB math instead of guessed coordinates. Reach here after a capture (`call("render")` / `call("camera")`) when you need to know where something is or put it somewhere precise; use `call("actor")` / `call("geometry")` for the spawn and transform primitives these verbs operate on.

## The closed spatial loop

Chain these verbs into a look → decide → place → check → adjust loop; the agent need not guess a world coordinate:

1. **Capture** the scene from a useful angle — `render.capture_open_level`, `camera.orbit_shots`, or `render.capture_annotated` (the annotated variant paints world axes, a Z=0 grid, and per-actor AABB size labels straight onto the PNG). When you do not yet know *what* is in frame, pass `render.capture_annotated`'s `actorLabels` argument: it returns a machine-readable actor → pixel map (`name`, `px`, `py`, `screenArea`, `onScreen`) for every actor matching a folder / tag / class / name filter, so step 2 can start from a name instead of a guess.
2. **Anchor to geometry** — `spatial.raycast_screen` turns a pixel in that capture back into a world point ("place it THERE"), and `spatial.place_on_surface` rests an existing actor on whatever a ray/point/drop hits. `spatial.raycast` is the raw world-space trace behind both.
3. **Measure / verify** the result deterministically — `spatial.measure_distance`, `spatial.measure_overlap`, and `spatial.verify_placement` read the same world-space AABBs `actor.get_bounding_box` reports, so the numbers agree.
4. **Adjust** — `spatial.place_relative` (relation math) or `actor.nudge` (relative delta) move things by a stated relationship, then re-measure.

Step 2 assumes you already know *where*. When you do not — "is there room for this asset anywhere in here, and if not what is standing in the way" — start from `spatial.find_clear_placement` instead: it is the only verb here that **searches** for a placement rather than taking one as input, and the only one that sees ISM/HISM instances rather than only actors.

## Design intent: vision proposes, measurement disposes

A vision pass is good at proposing *relationships* ("the crate should sit on the shelf") but not metric *coordinates*. State the relation (`place_relative`, `place_on_surface`, or `verify_placement`'s `expect`); the solver computes the transform and a measurement verb confirms it with box math. Mutating verbs return inline verification.

## Coordinate conventions

Shared by every RPC in this namespace (each result also echoes `units` and an `axis` note):

- **Units are centimetres.** All positions, distances, gaps, extents, and grid sizes are cm.
- **Left-handed, Z-up:** `+X` forward, `+Y` right, `+Z` up. So `left_of` is `-Y`, `in_front_of` is `+X`, `on_top_of` is `+Z`.
- **Rotation is an `FRotator` in degrees:** `pitch` about Y, `yaw` about Z, `roll` about X.
- **An actor's pivot is not its bounds centre.** `GetActorLocation()` (the pivot) can sit anywhere inside or outside the mesh; the AABB these verbs use is `GetActorBounds()` (centre + extent). `pivotDistance` vs `centerDistance` in `measure_distance` exists precisely because the two differ, and `place_on_surface` pivot-corrects so an off-centre pivot still rests flush.

## Editor-world caveats

- **Spawn and trace on separate calls.** In a non-PIE editor world, a body spawned earlier in the *same* call is not registered in the physics scene-query structure until the world ticks, so a same-call raycast/placement can miss it.
- **Pixel-producing and deproject verbs need a live viewport.** `spatial.raycast_screen` (and `place_on_surface`'s `screen` mode) rebuild the capture's `FSceneView` off the active, GPU-backed Level Editor viewport. Headless / `-unattended` runs with no such viewport get a typed failure (`NO_ACTIVE_LEVEL_VIEWPORT`), not a wrong answer. The pure box-math verbs (`measure_*`, `verify_placement`, `place_relative`) do not need a viewport.

## traceComplex hits render geometry — read this before writing a height probe

**`traceComplex: true` makes a mesh with NO collision set up still block the ray.** This is expected Unreal behaviour, not an engine bug and not a PinWright bug: complex collision for a `StaticMesh` *is* its render triangle soup, so when a mesh has no simple collision primitives the render triangles are the only representation the trace has to hit. Asking for a complex trace is asking to hit render geometry.

The failure is silent and expensive: a downward probe near foliage can return plausible `location`, `normal`, and `distance` from the side of a collisionless tree. One live probe set measured **268 of 930** downward `traceComplex` probes blocked by a collisionless HISM foliage mesh instead of the landscape; every hit looked clean, so a height map built from it was wrong.

How to not get caught:

- **Never take the first blocking hit on faith.** Use `onlyActors`, `actorFilter` (with `actor.list`'s `matchMode` + `caseSensitive` knobs), or `onlyClasses` (`["LandscapeProxy"]` covers `Landscape` and `LandscapeStreamingProxy`). Other hits are peeled out and listed in `filteredOut`.
- **Or look at the layers yourself** with `multiHit: true` and pick the hit you want out of `hits[]` (nearest first, one hit per actor).
- **Or drop `traceComplex` entirely.** It is `false` by default, and on terrain the two traces are interchangeable. On authored meshes they are not — read *Which trace, for which ground* below before you settle on either value.
- **Check the response.** Every hit carries `simpleCollisionShapes` (`0` means a complex trace resolved against render triangles) and, then, `renderGeometryHit: true` plus a top-level `warnings[]` actor entry. An absent field means the engine reported nothing; Landscape has no simple/complex split and is never flagged.

Filtering and `multiHit` live on `spatial.raycast`. `spatial.raycast_screen` and `spatial.place_on_surface` still take only `ignoreActors`; when a screen pick lands on the wrong thing, read the world point off `spatial.raycast_screen` and re-probe it with a filtered `spatial.raycast`.

## Which trace, for which ground

`traceComplex: false` asks the **simple hull**; `true` asks the **render surface**. On authored geometry those are two different shapes, so neither value is a safe default — choose by what you are probing.

- **Landscape and terrain — either works.** A heightfield has no simple/complex split, and measured at three points both traces returned the same height to the last digit. This is where leaving the default alone is unambiguously right.
- **Authored architecture — trace complex, then look at it.** A hull fitted before the mesh was chipped, eroded or boolean-subtracted diverges over exactly those features. One measured set: a podium hull standing **≥210 cm** proud of the stone it stands for, a wall crest at **+115 to +128**, a ruined wall at **+85.3**, and a dome **125.7 cm the other way** — a simple probe seats a prop in mid-air on the first three and buries it in the last. **The sign is not constant within one mesh**: that same wall measured **+86.5** at one XY and **−104.8** at another, so no "take the higher hit" rule rescues it. Non-uniform scale alone is enough to cause it — UE scales an `FKSphereElem` radius by the **minimum absolute scale component**, so a sphere hull on a mesh at `(1.1, 1.1, 1.0)` is sized to the unscaled axis and drops as much as **396 cm** below the render surface — no authoring involved.
- **Foliage, cards, anything possibly collisionless — complex needs a filter.** That is the hazard above: pair `traceComplex: true` with `onlyClasses` / `onlyActors` / `actorFilter`, or peel the layers with `multiHit`, and read `renderGeometryHit` on the hit you keep.

**More samples cannot find a divergent hull; asking which representation answered can.** On the unchipped stone of that same podium both traces returned `600.000` — identical. The hull is not coarse: it agrees everywhere the mesh is unmodified and disagrees only over the authored features. Three response fields answer three different questions, and only the first is about provenance:

- **`faceIndex` — which representation answered.** Present means a triangle mesh or heightfield resolved the query; absent means a simple primitive did. It is requested on every line trace the verb issues, independent of both `channel` and `traceComplex`, so a `traceComplex: false` probe that returns **no** `faceIndex` was answered by the hull — confirm that before you seat anything on it. Absence is presumptive, not proof: the engine also reports none when the backend supplied none. Landscape answers with a face index either way, which is part of why terrain is the easy case.
- **`renderGeometryHit` — did a complex trace fall through to render triangles.** It is `traceComplex && simpleCollisionShapes == 0`, so it fires only for the hazard above and can never flag a hull. A simple trace resting two metres above the visible stone reports no warning at all.
- **`simpleCollisionShapes` — how much hull there is.** The engine's own primitive count. A `1` standing in for an elaborate mesh is the shape of hull that diverges; absent means no body setup at all.

Probe the XY both ways and compare the Z. [`editor.collision-review`](editor.collision-review.md) shows the same divergence as a picture.

## See also

- [`spatial.ground-placement`](spatial.ground-placement.md) — seating props on terrain at scale: why a pivot raycast cannot do it, and how `spatial.ground_actors` / `spatial.verify_grounding` do.
- [`level-building`](level-building.md) — placing and conforming geometry at scale using these measurements.
- [`camera`](camera.md) — looking at what a trace reported, from an angle you choose.
- [`level-review`](level-review.md) — measurement-grade orthographic framing for proportion checks.

### spatial.raycast

Cast a world-space line trace and report the hit. Defaults to the first blocking hit; `multiHit` walks the layers along the ray and the actor filters restrict which hits count.

Args — geometry:

- `origin` (object, **required**; aliases `start` / `from`) — ray start `{x,y,z}` in cm.
- one of `direction` (object; alias `dir`) `{x,y,z}` (normalized internally) **or** `target` (object; aliases `end` / `to`) `{x,y,z}` (segment end point; direction is derived and the trace stops here).
- `maxDistance` (number, default `1e7`; aliases `max_distance` / `distance`) — ray length in cm when using `direction`; ignored when `target` is given.
- `channel` (string, default `"visibility"`) — one of `visibility`, `camera`, `worldstatic`, `worlddynamic`.
- `traceComplex` (boolean, default `false`; alias `trace_complex`) — trace per-triangle collision instead of simple collision. **A mesh with no collision set up still blocks a complex trace** — see *traceComplex hits render geometry* on `call("spatial")`, and the Gotchas below. Neither value is safe everywhere; *Which trace, for which ground* on that page says which to pick per surface.

Args — which hits count:

- `ignoreActors` (array; alias `ignore_actors`) — names / labels / paths excluded from the trace entirely (engine-level ignore list). Names that don't resolve are skipped and echoed back as `unresolvedIgnoreActors`.
- `onlyActors` (array; alias `only_actors`) — accept hits **only** on these actors, resolved to exact actors so a duplicate label can't widen the filter. If none of the names resolve the call fails with `ACTOR_NOT_FOUND` rather than reporting a miss for every ray.
- `actorFilter` (string; aliases `actor_filter` / `filter`) — accept hits only on actors whose internal name **or** display label matches. Uses the shared name-filter policy, so it takes the same two knobs `actor.list`'s `filter` does:
  - `matchMode` (string, default `"contains"`; alias `match_mode`) — `contains` (alias `substring`) | `prefix` (alias `starts_with`) | `exact` | `regex` (unanchored ICU search; a pattern that doesn't compile is `INVALID_PATTERN`, never a silent no-match).
  - `caseSensitive` (boolean, default `false`; alias `case_sensitive`) — the default is case-**insensitive**, so `actorFilter: "SH_"` also matches `Brush_0`. Pair `matchMode: "prefix"` with `caseSensitive: true` when you are selecting by an upper-case naming prefix.
  - Supplying `matchMode` / `caseSensitive` without `actorFilter` is `INVALID_ARGUMENT` (it would silently do nothing). The resolved `actorFilter` / `matchMode` / `caseSensitive` are echoed in the response so you can see which semantics ran.
- `onlyClasses` (array; aliases `only_classes` / `classFilter` / `class_filter`) — accept hits only on actors whose class **or any ancestor class** matches, as a case-insensitive substring of the class name or its `/Script` path. `matchMode` / `caseSensitive` govern `actorFilter` only — these are engine class identifiers, not labels. `["LandscapeProxy"]` is the terrain shortcut: it covers `Landscape` and `LandscapeStreamingProxy` without naming an actor.
- `multiHit` (boolean, default `false`; aliases `multi_hit` / `returnAllHits` / `return_all_hits`) — return every layer along the ray in a `hits[]` array instead of only the nearest one.
- `maxHits` (number, default `32`, ceiling `256`; alias `max_hits`) — caps both the hits returned and the trace passes performed. **Unlike `actor.list`'s `limit`, `0` is not "all"** — each layer costs a line trace — and is rejected with `INVALID_PARAMS`.

Filters are ANDed and do **not** turn a blocked ray into a miss: a rejected hit is peeled off and the trace runs behind it, so `actorFilter: "Terrain"` can reach terrain behind foliage. Peeled actors appear in `filteredOut`.

```js
// Ground height under a point, immune to whatever is standing on it.
call({
  path: "spatial.raycast",
  args: {
    origin: { x: 1200, y: -400, z: 100000 },
    direction: { x: 0, y: 0, z: -1 },
    onlyClasses: ["LandscapeProxy"]
  }
})

// Or look at every layer and choose.
call({
  path: "spatial.raycast",
  args: {
    origin: { x: 1200, y: -400, z: 100000 },
    direction: { x: 0, y: 0, z: -1 },
    multiHit: true, maxHits: 8
  }
})
```

Response: `{hit, location, normal, distance, actor:{name,path,label,internalName,class}, component, channel, traceComplex, units, axis}`. Top-level fields always describe the nearest **accepted** hit, even with `multiHit`. Additional fields when relevant: `faceIndex` (present when a triangle mesh or heightfield answered the query, absent when a simple primitive did — the signal for *which* representation you measured; see *Which trace, for which ground*), `simpleCollisionShapes` (`0` means none; absent means no body setup, e.g. Landscape), `renderGeometryHit: true`, `warnings[]`, `filteredOut[]` + `filteredOutCount`, `unresolvedIgnoreActors[]`, and `unresolvedOnlyActors[]`. Any `multiHit` or filtered call also returns `truncated` when `maxHits` ran out with geometry remaining. `multiHit` adds nearest-first `hits[]` (at most one hit per actor) and `count`.

Gotchas: a clean **miss is a success with `hit:false`**. **`traceComplex: true` hits the render geometry of meshes that have no collision at all** — expected engine behaviour; see *traceComplex hits render geometry*. A zero-length `direction` or `target == origin` is `INVALID_PARAMS`. `visibility` uses standard mesh collision; switch to `worldstatic`/`worlddynamic` as needed. Peeling costs one trace per layer, and because it excludes the whole **actor**, a ray that enters and exits one mesh yields one `hits[]` entry, not two.

### spatial.raycast_screen

Map a pixel from a prior capture back to a world-space ray and first blocking hit — the inverse of a viewport capture ("place it THERE where I can see it").

**Pose-coherence contract:** pass back the EXACT pose and dimensions the capture returned. The handler never reads the live camera; it deprojects only against the args you supply.

Args:

- `location` (object, **required**) — the capture's camera `{x,y,z}` in cm.
- `rotation` (object, **required**) — the capture's `{pitch,yaw,roll}` in degrees.
- `x`, `y` (number, **required**; aliases `pixelX` / `pixelY`) — pixel, top-left origin, `0 <= x < width`, `0 <= y < height`.
- `width`, `height` (number, **required**) — the capture's pixel dimensions.
- `projectionMode` (string, default `"perspective"`), `fov` (number, default `50`) or `orthoWidth` (number, default `2000`; alias `orthoWorldWidth`) — must match the capture. `orthoWidth` is the frame's **world width in centimetres**. An orthographic `rotation` must look along a world axis (the same rule the capture obeys); a tilted one returns `UNSUPPORTED_ORTHOGRAPHIC_ROTATION`. Pass back the capture's `cameraRotation` (the effective pose), not the rotation you originally asked for.
- `maxDistance` (number, default `1e7`), `channel` (string, default `"visibility"`), `traceComplex` (boolean; alias `trace_complex`), `ignoreActors` (array; alias `ignore_actors`) — as in `spatial.raycast`. The hit-selection args are **not** available here: `multiHit`, `onlyActors`, `actorFilter` and `onlyClasses` exist only on `spatial.raycast`. `traceComplex: true` carries the same render-geometry hazard described under *traceComplex hits render geometry* on `call("spatial")`; when a screen pick lands on a collisionless mesh, take the world point from this response and re-probe it with a filtered `spatial.raycast`.

```js
call({
  path: "spatial.raycast_screen",
  args: {
    location: { x: 1000, y: 200, z: 300 },
    rotation: { pitch: -10, yaw: 135, roll: 0 },
    x: 512, y: 384,
    width: 1024, height: 768,
    projectionMode: "perspective", fov: 50
  }
})
```

Gotchas: the response echoes the deprojected `ray` and `view` (pose, dimensions, and pixel) for image-coherence checks. The **centre pixel maps to the camera forward** direction. An out-of-range pixel is `INVALID_PARAMS`; headless runs return `NO_ACTIVE_LEVEL_VIEWPORT`; a miss is success with `hit:false` and still echoes ray + view.

This is a **physics trace**: decals, most lights, `NoCollision` meshes, and editor-only volumes can be visible yet return `hit:false`, and one call answers one pixel. For "what is in this frame, and where?", use `render.capture_annotated`'s `actorLabels` render-bound map, then raycast the selected pixel for its exact surface point.

### spatial.measure_distance

Deterministic distance between two actors by world-AABB math (no screenshots).

Args:

- `a`, `b` (string, **required**) — display label / internal name / path of each actor.
- `mode` (string, default `"center"`) — headline selector: `center`, `pivot`, or `edge_gap`. Does not change which fields are returned.

```js
call({ path: "spatial.measure_distance", args: { a: "Cube_1", b: "Wall_2", mode: "edge_gap" } })
```

Gotchas: all three distances are **always present** — `centerDistance` (bounds-centre to bounds-centre), `pivotDistance` (pivot to pivot), `edgeGap` (nearest surface-to-surface gap) — plus `perAxisGap {x,y,z}`; `mode` only picks which is echoed as the headline `distance`. `edgeGap` is `0` when the AABBs overlap. A negative `perAxisGap` component means the boxes overlap on that axis. AABBs bound a rotated non-box mesh **looser** than its true silhouette, so expect a rotated mesh to read larger than it looks.

### spatial.measure_overlap

Non-mutating AABB overlap test with per-axis penetration.

Args:

- `a`, `b` (string, **required**) — display label / internal name / path of each actor.

```js
call({ path: "spatial.measure_overlap", args: { a: "Crate_1", b: "Crate_2" } })
```

Gotchas: returns `{overlapping, penetrationAxis:"x|y|z", penetrationDepth, perAxisPenetration}`. `penetrationAxis` is the minimum-translation separating axis and `penetrationDepth` its smallest push-out — both are **only meaningful when `overlapping` is true** (a neutral `""` / `0` otherwise). Faces that merely touch count as overlapping. This is a pure read — it does not modify geometry the way `geometry.boolean_intersection` does.

### spatial.verify_placement

Assert an actor's placement with box math instead of eyeballing a screenshot. Only the checks present in `expect` run.

Args:

- `actor` (string, **required**) — the actor to verify.
- `expect` (object, **required**) — any of:
  - `grounded: { maxGap }` — a downward trace from the bounds bottom must hit within `[0, maxGap]`.
  - `on: "<actorName>"` — the surface directly below must be that actor.
  - `noOverlapWith: [names]` — none may intersect this actor's AABB.
  - `within: { min:{x,y,z}, max:{x,y,z} }` **or** `"<actorName>"` — this actor's AABB must be fully contained.

```js
call({
  path: "spatial.verify_placement",
  args: {
    actor: "Barrel_1",
    expect: {
      grounded: { maxGap: 2 },
      noOverlapWith: ["Wall_1", "Wall_2"],
      within: "Room_Bounds"
    }
  }
})
```

Gotchas: returns `{pass, checks:[{name, pass, detail}]}` where `pass` is the AND of all requested checks. `grounded`/`on` trace **pre-existing** geometry, so tick the world between a spawn and the verify. An actor **sunk into** a surface reads as *no ground* (a downward-from-bottom trace can't report a gap above its start), i.e. a miss rather than a negative gap. `on` uses a 2 cm rest tolerance; unresolved `noOverlapWith` names are reported but do not fail the check.

### spatial.place_on_surface

Rest an EXISTING actor ON a surface, pivot-corrected so it never sinks. Backed by `FActorPositioning::GetSurfaceAlignedTransform` + `UsePlacementExtent(bounds)`.

**Superseded for terrain work by [`spatial.ground_actors`](spatial.ground_actors.md).** This verb rests one point of a flat bounds plane on one trace hit, so a boulder on a slope comes to balance on a corner, and it has no notion of a footprint that overhangs the ground. `ground_actors` samples an N×N grid of the actor's own underside, refuses an actor whose footprint is not supported (`PARTIAL_GROUND_COVERAGE`), re-measures after the move, and takes a batch in one call. Reach for `place_on_surface` when you genuinely want a single-point rest — a lamp on a table, a decal plate on a wall.

Args:

- `actorName` (string, **required**; aliases `objectPath` / `actorPath`) — the actor to place.
- exactly one target of:
  - `screen` (object) — `{x, y, location, rotation, fov?, orthoWidth?, projectionMode?, width, height}`; deprojects pixel `(x,y)` through that viewport pose to a ray, then traces. `orthoWidth` is world centimetres (default `2000`) and an orthographic `rotation` must look along a world axis, exactly as for `spatial.raycast_screen`.
  - `worldPoint` (object; alias `world_point`) — `{x,y,z}`; a straight-down trace from just above it finds the surface. Nothing below it is `SURFACE_TRACE_MISSED` and **nothing moves**.
  - `dropDown` (boolean; alias `drop_down`) — drop straight down from the actor's current position.
- `assumePointIsSurface` (boolean, default `false`; alias `assume_point_is_surface`) — `worldPoint` only: on a trace miss, treat the point itself as the surface with an up-normal instead of failing. The result then carries `surface.measured: false`.
- `alignToNormal` (boolean, default `false`; alias `align_to_normal`) — tilt the actor's `+Z` to the surface normal.
- `offset` (number, default `0`) — extra cm along the normal after resting (e.g. hover above).
- `snapGrid` (boolean, default `false`; alias `snap_grid`) + `gridSize` (number, default `10`; alias `grid_size`).

```js
call({
  path: "spatial.place_on_surface",
  args: { actorName: "Lamp_1", dropDown: true, snapGrid: true, gridSize: 10 }
})
```

Gotchas: returns `{placed, transform, surface:{location, normal, measured, actor?}, verification:{restingGap, overlapping, againstMeasuredSurface, placementErrorCm}, units, axis}`. `placed` is a **readback** — the actor's reported location compared with the one computed for it — not a constant, and a mismatch comes back `placed: false` with `reasonCode` / `reason`. `surface.measured: false` means no trace ever answered and `location` / `normal` are your own point with an assumed up-normal, which also makes `restingGap` a statement about your point rather than about the world (`againstMeasuredSurface` says so). The placed actor is **auto-ignored** by its own trace, so it can't hit itself. `screen` mode needs a live viewport. A miss is an error in every mode: `SURFACE_TRACE_MISSED` for `worldPoint` (unless `assumePointIsSurface`), `SURFACE_NOT_FOUND` for `dropDown` / `screen`. `snapGrid` rounds to the requested `gridSize` (a manual grid snap, not the editor's grid setting).

Before 2026-08 the `worldPoint` miss silently invented a surface at the caller's own point and answered `placed: true`. That is the mechanism behind props sitting beyond the map edge, over holes and above water — the exact places a downward trace has nothing to hit. If you were relying on it, pass `assumePointIsSurface: true` and read `surface.measured`.

### spatial.place_relative

Place actor B by relation to anchor A using pure world-AABB arithmetic (no raycast).

Args:

- `actor` (string, **required**; aliases `actorName` / `actorPath` / `objectPath`) — B, the actor to move.
- `anchor` (string, **required**; aliases `anchorName` / `anchorPath`) — A, the actor to place relative to.
- `relation` (string, **required**) — `on_top_of | below | left_of | right_of | in_front_of | behind | centered_on | against_wall`.
- `gap` (number, default `0`) — cm between B's face and A's opposing face along the relation axis; ignored for `centered_on`.
- `align` (object) — per non-relation axis override `{x,y,z}` each `min | center | max` (default `center`).

```js
call({
  path: "spatial.place_relative",
  args: { actor: "Book_1", anchor: "Table_1", relation: "on_top_of", gap: 0 }
})
```

Gotchas: returns `{placed, transform, verification:{overlapping, edgeGap}, units, axis}`. Axes are **left-handed** — `left_of` is `-Y`, `in_front_of` is `+X`. `against_wall` stands B flush to the anchor's `+X` face, base-aligned (rests on the anchor's bottom) and Y-centered. `centered_on` **intentionally overlaps** (B's centre lands on A). B moves rigidly — its rotation is never changed.

### spatial.ground_actors

Seat a BATCH of actors on the ground and report, per actor, whether it actually happened. Footprint-sampling solver, not a pivot raycast — full rationale on [`spatial.ground-placement`](spatial.ground-placement.md).

Args:

- `surface` (object, **required**) — what counts as ground: `{preset, channel?, traceComplex?, excludeEffectGeometry?, onlyClasses?, excludeClasses?, excludeNames?, ignoreActors?, maxLayers?, maxDrop?, probeLift?}`. `preset` is `landscape` (only `LandscapeProxy` — the right answer for terrain), `any_solid` (anything blocking except foliage actors and effect geometry), or `custom`. Explicit lists **add** to the preset; `excludeEffectGeometry` **overrides** it. There is no default.
- exactly one selector: `actors` (array of labels/names/paths; aliases `actorNames` / `actor_names`), `prefix` (string), `filter` (string, with `matchMode` / `caseSensitive`), or `selection: true`. A pattern selector (`prefix` / `filter`) on this **mutating** verb also requires `expectedMatches`.
- `expectedMatches` (number; alias `expected_matches`) — **required with `prefix` / `filter`**: how many actors you believe the pattern names. A different match count is refused `MATCH_COUNT_MISMATCH` *before anything moves*, and the refusal carries `matchedActors[]` so you can see which ones are not yours. A name pattern is not a scope in a shared level — the same prefix is somebody else's prefix too. Find the number with `spatial.verify_grounding`, which takes the same selectors and moves nothing. Optional — but honoured — with `actors` / `selection`. Counts the full match set, so it is the same value on every page of a `limit`/`offset` walk.
- `samples` (number, default `3`; aliases `gridSize` / `grid_size`) — N×N footprint grid, clamped 1–9. `1` is the old single-point behaviour.
- `footprintInset` (number, default `0.1`) — pull samples inward from the AABB edge (0–0.45).
- `undersideModel` (string, default `mesh`) — `mesh` traces the actor's own geometry per column; `bounds_plane` models a flat bottom.
- `seatPercentile` (number, default `0`) — `0` rests on first contact, `1` sinks until nothing floats, `0.5` median.
- `embedFraction` (number, default `0.02`) + `embedDepth` (number, default `0`) — deliberate bedding, as a fraction of bounds height plus absolute cm.
- `alignToSurface` (boolean, default `false`; alias `alignToNormal`) + `maxTilt` (number, default `20`) — tilt +Z toward the **averaged** ground normal, clamped.
- `minCoverage` (`0.5`), `minContactPoints` (`1`), `contactTolerance` (`2`), `maxSeatError` (`1`) — pass criteria.
- `revertOnFailure` (boolean, default `false`), `limit` (`512`, max 5000), `offset` (`0`), `detail` (`summary` | `failures` | `all`, default `failures`).

```js
call({
  path: "spatial.ground_actors",
  args: {
    surface: { preset: "landscape" },
    prefix: "SM_Rock_",
    seatPercentile: 0,
    embedFraction: 0.03,
    detail: "failures"
  }
})
```

Gotchas: returns `{requested, placed, failed, moved, totalMatches, truncated, surface, seat, movedActors[], results[], units, axis}`, and `placed + failed == requested` always. `placed` per actor is **derived** — a transform recorded by the code that moved the actor, ANDed with a post-move re-measurement that passed — so a failed actor is never counted as placed. An actor may be `moved: true, placed: false` (it was seated but the readback disagreed); `previousTransform` is echoed for **every actor that was moved**, successes included — "the move succeeded" is not "the move was intended" — and the same record is repeated in `movedActors[]`, which is **not** governed by `detail` and is present even at `detail: "summary"`. That is the undo; `revertOnFailure` only covers a failed readback. Per-actor failures carry `reasonCode`: `GROUND_NOT_FOUND`, `GROUND_HITS_ALL_REJECTED` (with `rejectedSurfaceActors` naming what the filter refused), `PARTIAL_GROUND_COVERAGE`, `GROUND_SEAT_READBACK_MISMATCH`, `ACTOR_HAS_NO_BOUNDS`, `ACTOR_LOCATION_LOCKED`, `HOLDER_NOT_SEATABLE` (an ISM/HISM scatter holder — its bounds are the union of every instance, so seating it would move the whole scatter with one transform; the message names the component, and `spatial.verify_grounding` refuses the same actor with the same code rather than returning a verdict about nothing. Seat that scatter with **`spatial.ground_instances`**, which runs this same solve per instance against the instance's own footprint; `actor.get_instances` / `actor.set_instance_transforms` read and write the per-instance transforms directly). An empty match set is `NO_ACTORS_MATCHED`, not a 0-of-0 success. Spawn and seat on **separate calls** — a body spawned in the same call is not in the physics scene-query structure until the world ticks.

### spatial.ground_instances

Seat the INSTANCES of one ISM/HISM component and report, per instance, whether it actually happened. `spatial.ground_actors` refuses a scatter holder with `HOLDER_NOT_SEATABLE` - the holder's bounds are the union of every instance, so seating it would relocate the whole scatter with one transform - which makes this the only seat path for scattered content. Same solver, per instance, against that instance's own footprint. Full rationale on [`spatial.ground-placement`](spatial.ground-placement.md).

Args:

- `surface` (object, **required**) - identical shape and defaults to `spatial.ground_actors`' `surface`. `excludeComponentClasses` earns its keep here in particular: this verb seats instances against whatever surrounds them, and a plain ISM/HISM neighbour is nameable on no other axis. The holder actor itself is always excluded, so an instance is never seated onto a sibling.
- `actorName` (string, **required**; aliases `objectPath` / `actorPath` / `actor_name`) - the actor carrying the scatter.
- `component` (string, optional; aliases `componentName` / `component_name`) - object name of the component, case-insensitive. Omit to take the component with the MOST instances. **Resolve it by static mesh plus instance count immediately before writing:** component object names are not stable across a session - a procedural generator can rebuild and rename its components between two calls - and a stale name silently addresses a different scatter.
- `indices` (array, optional) - specific instance indices. Omit to walk the component under `limit` / `offset`. An out-of-range index refuses the call rather than being skipped.
- `apply` (boolean, default `true`) - `false` runs the identical solve, writes nothing, and reports `proposedDeltaZCm` / `proposedTransform` with `status: "dry_run"` per instance. This is the verify half of the verb; there is no separate non-mutating sibling, because instances are addressed by index against one named component and so carry no name-pattern scope hazard.
- `samples` (`3`), `footprintInset` (`0.1`), `seatPercentile` (`0`), `embedFraction` (`0.02`), `embedDepth` (`0`) - the seat solve, same meanings as `spatial.ground_actors`.
- `minCoverage` (`0.5`), `minContactPoints` (`1`), `contactTolerance` (`2`), `maxSeatError` (`1`) - pass criteria.
- `limit` (`512`, max 5000), `offset` (`0`), `detail` (`summary` | `failures` | `all`, default `failures`).

```js
// dry run first, always
call({
  path: "spatial.ground_instances",
  args: {
    actorName: "Scatter_Rocks",
    component: "ISM_Rock_A",
    surface: { preset: "landscape" },
    apply: false,
    samples: 5,
    seatPercentile: 0.9,
    embedFraction: 0,
    embedDepth: 4,
    minContactPoints: 3,
    detail: "all"
  }
})
```

Gotchas: returns `{instanceCount, selected, requested, placed, moved, solved, failed, offset, truncated, surface, seat, movedInstances[], results[], units, axis}`. `movedInstances[]` is the undo - `actor.set_instance_transforms` writes those rows back verbatim - and it is **not** governed by `detail`, so it is present even at `detail: "summary"`; capture it before the response scrolls away. Instance indices are **positional**: anything that re-scatters, adds or removes instances renumbers them.

Three limits are structural rather than tunable, and each has cost the caller real work:

- **`undersideModel` is not a parameter** (passing one is `UNKNOWN_PARAMS`). The underside is always a flat plane at the instance's world-AABB minimum, because an instanced component's `LineTraceComponent` answers from every instance body at once and cannot attribute a hit to one of them. `undersideReliefCm` is therefore `0` by construction, not by measurement, and the AABB minimum of a rotated prop sits **below** its real lowest point - which is why a low `seatPercentile` can LIFT an instance that was already bedded.
- **The footprint is the bounds footprint.** For anything whose contact patch is far narrower than its silhouette the grid samples the silhouette; `footprintInset` clamps at `0.45` and cannot close a 47x area gap. Compute the seat yourself and write it with `actor.set_instance_transforms` - the closed form is on [`spatial.ground-placement`](spatial.ground-placement.md).
- **There is no `alignToSurface` / `maxTilt`.** `spatial.ground_actors` has both; instances are seated in Z only. A scatter that needs to lie along the slope needs its rotations written directly.

### spatial.verify_grounding

Measure ground-contact quality for a BATCH of actors. Non-mutating. Sees the two failures `spatial.verify_placement`'s single-ray `grounded` check structurally cannot: an actor **sunk into** a surface (a bottom-up ray cannot report a negative gap) and an actor **balanced on one point**.

Args:

- `surface` (object, **required**) — identical shape and defaults to `spatial.ground_actors`. Verify with the same surface you seated with.
- exactly one selector: `actors` / `prefix` / `filter` / `selection` (as above).
- `samples` (`3`), `footprintInset` (`0.1`), `undersideModel` (`mesh`) — must match the seat to be comparable.
- `maxGap` (number, default `2`) — largest allowed clearance in cm between the actor's **lowest** underside sample and the lowest ground under its footprint. The floating check. Deliberately **not** a bound on the silhouette: overhanging or curved geometry far from where the actor rests is shape, not float, and is reported as `maxColumnClearanceCm` / `undersideReliefCm` instead of failing the actor.
- `maxPenetration` (number, default `2`) — deepest allowed burial. **Raise past the embed depth** when verifying deliberately bedded actors, or every one fails.
- `minCoverage` (`0.5`), `minContactPoints` (`1`; raise above 1 for wide actors), `contactTolerance` (`2`).
- `limit` (`512`), `offset` (`0`), `detail` (`failures`).

```js
call({
  path: "spatial.verify_grounding",
  args: {
    surface: { preset: "landscape" },
    prefix: "SM_Rock_",
    maxGap: 2,
    maxPenetration: 12,
    minContactPoints: 3
  }
})
```

Gotchas: returns `{pass, checked, passed, failed, totalMatches, truncated, surface, criteria, results[], units, axis}`. Each row's `contact` carries `coverage`, `contactPoints`, `maxGapCm`, `minGapCm`, `maxColumnClearanceCm`, `undersideReliefCm`, `penetrationCm`, `groundSpreadCm`, `averageNormal`, `supportedColumns` / `actorColumns` / `sampledColumns`, plus `failCode` + `failReason` when it did not pass. `maxGapCm` is the gated figure (lowest point vs lowest ground); `maxColumnClearanceCm` is the raw per-column maximum and `undersideReliefCm` how far the actor's own underside is from a plane — read those two together to see why a shaped actor's numbers look the way they do. Gap numbers are **omitted entirely** when nothing was measured or no column found ground — an absent number is not a zero. `boundsPlaneFallback: true` means the actor answered no geometry query and the flat-plane model was used, which changes what the numbers mean. `overLandscape: false` distinguishes "not over the terrain at all" from "something was in the way". Top-level `pass` covers only the actors reached in this call; check `truncated` when paging.

`contact.groundProvenance` says **which surface every number above was measured against**, because a mesh has two and they are not the same one. `primitiveColumns` counted a hit with no `faceIndex` — a simple collision **hull** answered; `triangleColumns` counted one with a face index — a triangle mesh or landscape heightfield answered, i.e. the surface a viewer sees. `minSimpleCollisionShapes` / `maxSimpleCollisionShapes` are the struck components' simple-primitive counts (omitted for Landscape, which has no body setup); a count of `1` under an elaborately modelled mesh is the signature of a hull that is not its own render surface. `surfaceTrust` is the block's verdict and is always present: `untrusted` (a condition fired, and `warning` says which and names the surface), `trusted` (every supported column named a surface and none tripped), or `undetermined` (some column's primitive never resolved — `unclassifiedColumns` counts them, and `warning` is omitted rather than written as an all-clear over columns nobody could examine). `warning` carries every fired condition in one string: a **hull** answered (`primitiveColumns`); an **instanced scatter** answered (`instancedScatterColumns` — ISM/HISM/foliage, which `any_solid` admits deliberately, so this is a call for you to make, not a failure); **render triangles** answered because the struck component has no simple collision (`renderGeometryColumns`); or a **UseComplexAsSimple** body answered a `traceComplex: false` probe with per-triangle geometry (`complexAsSimpleColumns`). The last three all answer *with* a face index, which is why a warning gated on its absence could never reach them. This is provenance, not magnitude: it says which representation replied, never by how much or in which direction the two differ — to get that, re-probe the same columns with `spatial.raycast` at the opposite `traceComplex` and compare. Absence of a `faceIndex` is a strong prior, not proof; the physics backend can also report none.

### spatial.find_clear_placement

Search for a pose a footprint fits in, instead of verifying one you already picked. This is the only occupancy verb in the namespace: every other one takes the placement as **input**.

Non-mutating — nothing is spawned or moved. It sweeps an XY grid × yaw grid over a region, tests each pose with a physics **box overlap**, bisects the free space around it to get a measured clearance, and attributes every rejection to the thing that caused it.

**It sees ISM/HISM instances.** A box overlap resolves against a scatter's per-instance bodies, so a rejection names the actor, the component **and the instance index**. No name-based occupancy input here can express that: `verify_placement`'s `noOverlapWith` takes actor names, and a scatter holder's single AABB spans the whole scatter, so naming the holder says nothing about which instance is in the way.

Args — the footprint (exactly one):

- `footprint` (object) — `{width, depth, height?}` in cm; `height` defaults to `100`.
- `assetPath` (string; aliases `asset_path` / `meshPath`) — a static mesh whose own `GetBounds()` extent becomes the footprint, so you never re-derive it.

Args — where to look:

- `region` (object, **required**) — `{center:{x,y,z}, radius}` or `{min:{x,y,z}, max:{x,y,z}}`. The region also bounds the ground probe in Z: each column is traced from the region's top face down to its bottom face.
- `step` (number) — XY grid spacing in cm. Default: half the footprint's shorter side, floored at 10.
- `yawStep` (number, default `90`; alias `yaw_step`) — only yaws in `[0, 180)` are tried, because a rectangular footprint maps onto itself under a half turn. `0` searches yaw 0 only; anything else below `0.5` is `INVALID_PARAMS` rather than silently coarsened.
- `seatOnGround` (boolean, default `true`; alias `seat_on_ground`) — rest the footprint on the surface under each column. `false` tests it centred at the region's Z instead (the airborne-corridor case).

Args — what counts as in the way:

- `minClearance` (number, default `0`; alias `min_clearance`) — required free space around the footprint, in cm.
- `clearanceProbe` (number, default `200`; alias `clearance_probe`) — ceiling on how far clearance is measured (raised to `minClearance` when that is larger).
- `ignoreActors` (array; alias `ignore_actors`) — excluded from the occupancy query entirely. Unresolved names come back as `unresolvedIgnoreActors`.
- `onlyClasses` (array; alias `only_classes`) — count as obstacles **only** actors whose class or any ancestor class matches, as a case-insensitive substring of the class name or its `/Script` path (same rule as `spatial.raycast`).
- `keepOut` (object; alias `keep_out`) — `{points:[{x,y,z},...], radius}`, a corridor to stay clear of (a camera path is the recurring one).

Args — output shape:

- `rank` (string, default `"clearance"`) — `clearance` (roomiest first) or `distance` (nearest the region centre first). Both numbers are on every returned pose either way.
- `maxResults` (number, default `10`, ceiling `200`; alias `max_results`), `maxPoses` (number, default `5000`, ceiling `100000`; alias `max_poses`).

```js
// Does a 4000 x 2300 mesh fit anywhere in this quadrant, and if not, what is stopping it?
call({
  path: "spatial.find_clear_placement",
  args: {
    assetPath: "/Game/Example/Meshes/SM_Stair_Block",
    region: { center: { x: 12000, y: -4000, z: 500 }, radius: 6000 },
    step: 200, yawStep: 45, minClearance: 50,
    keepOut: { points: [{x:9000,y:-2000,z:900}, {x:14000,y:-6000,z:900}], radius: 900 },
    rank: "distance"
  }
})
```

Response: `{found, evaluated, footprint:{width,depth,height,source,assetPath?}, region:{mode,center,min,max,radius?}, search:{step,yawStep,yawCount,columns,minClearance,clearanceProbe,seatOnGround,rank,keepOutPoints?,keepOutThresholdCm?}, poses[], binders[], binderCount, rejected:{total,occupied,clearance,keepOut,noGround}, units, axis}`.

Each `poses[]` entry: `{location, rotation, clearanceCm, clearanceCapped, distanceFromCenterCm, groundLocation?, groundActor?, clearanceBinder?}`. `location` is the **bounds centre** of the fitted box, not an actor pivot — the two differ, as everywhere else in this namespace. `clearanceBinder` is present even on an accepted pose: "it fits, and the nearest thing to it is that HISM instance" is the actionable form.

Each `binders[]` entry: `{actor, actorPath, actorClass, component, componentClass, instanced, instanceIndex?, instanceIndices?, instanceCount?, blockedPoses}`, keyed on the blocking **component** and ranked by how many poses it killed. `binderCount` is how many distinct blockers were tracked. This is the load-bearing half of a zero-result answer: *0 poses fit* is not actionable, *0 poses fit and the binder is always `HISM_Rubble` instances* is.

Gotchas: **an obstacle must have collision to be seen**, exactly as for `spatial.raycast` — a collisionless decorative mesh is invisible to a physics overlap. `clearanceCm` is a **bisected lower bound** (resolution `clearanceProbe / 64`), so never compare it against your own threshold — pass the threshold as `minClearance` and let the verb probe it directly, which is what it does. `clearanceCapped: true` means the pose was still free at `clearanceProbe`, so the number is a floor rather than the real gap. Each column is seated on the **highest** of five ground probes (centre plus the footprint's circumscribed extremes) so a box on a slope rests on the high side, and the actors those probes hit are excluded from that column's occupancy query — the surface you stand on is not an obstacle to you. `keepOut` is measured from the pose centre against the footprint's **circumscribed radius**, so it is conservative: it never passes a pose that actually intrudes, but it can refuse one that does not. A grid larger than `maxPoses` is **refused**, not truncated — a partial sweep would report "no room" for a region it never looked at. Spawn and search on **separate calls**: a body spawned in the same call is not in the physics scene-query structure until the world ticks.

### spatial.scatter_layout

The only verb here that **produces** placements. Everything else in this namespace answers a question about a placement you already chose (`measure_*`, `verify_*`, `find_clear_placement`) or moves one thing (`place_*`, `ground_*`). This one generates a set from a rule and is a **pure function**: no world, no actor, no asset, no trace, nothing spawned, moved or saved. It is the implementation of the jittered-hex-lattice doctrine in [`level-building.instancing-and-scatter`](level-building.instancing-and-scatter.md) — written once, with tests, instead of re-derived per caller in `python.execute`.

Params:

- `region` (object, **required**; alias `bounds`) — `{min:{x,y,z}, max:{x,y,z}}` or `{center:{x,y,z}, radius}`, the same two shapes `find_clear_placement` takes. Tested in **XY only**; every emitted location takes the region centre's Z.
- `spacing` (number, **required**) — distance between neighbouring centres, cm. Space by **canopy or footprint diameter**: 1.0–1.25 canopy diameters reads as a stand, so a 950 cm canopy wants roughly 1000–1200.
- `pattern` (string, default `"hex"`) — `hex` (triangular: rows pitched `spacing * sqrt(3)/2`, alternate rows offset half a step) or `square`.
- `jitter` (number, default `0.18`) — per-axis offset as a **fraction** of spacing, drawn **continuously** from `[-jitter, +jitter]`. `0` emits the bare lattice; anything above `0.5` is refused. **Hex rows stay geometrically disjoint below `sqrt(3)/4 ≈ 0.4330`** — 86.6% of the legal range, the default included — so one call at the default still reads as rows at eye level whatever the seed; see [`vegetation-authoring.scatter-geometry`](vegetation-authoring.scatter-geometry.md) for the measurement and the superposition workaround.
- `seed` (number, default `1337`) — echoed back.
- `scaleRange` (object, default `{min: 0.85, max: 1.15}`; alias `scale_range`) — uniform-scale multipliers.
- `randomYaw` (boolean, default `true`; alias `random_yaw`) — pitch and roll are **always** zero and have no knob.
- `exclude` (array) — carve-out regions, each in the same shape as `region`. An unparseable entry is refused, never skipped.
- `maxPoints` (number, default `5000`, ceiling `100000`; alias `max_points`).

```js
// A stand of 950 uu-canopy trees over a clearing, with a corridor carved out.
call({
  path: "spatial.scatter_layout",
  args: {
    region: { min: {x: -8000, y: -8000, z: 400}, max: {x: 8000, y: 8000, z: 400} },
    spacing: 1100, seed: 8891,
    exclude: [{ center: {x: 0, y: 0, z: 400}, radius: 2200 }]
  }
})
// -> feed data.transforms straight to foliage.add_instances, then seat with
//    spatial.ground_instances - the Z above is the region plane, not the ground.
```

Response: `{count, latticePoints, outsideRegion, excluded, region:{mode,center,min,max,radius?}, layout:{pattern,spacing,rowStep,jitter,jitterCm,seed,scaleMin,scaleMax,randomYaw,columns,rows,maxPoints,excludeRegions}, transforms[], units, axis}`. `count + outsideRegion + excluded == latticePoints` always — nothing is dropped unattributed. Each `transforms[]` entry is `{location, rotation, scale}`, the shape `foliage.add_instances`, `actor.spawn_batch` and `actor.set_instance_transforms` already consume.

Gotchas: **the layout is floating until you ground it.** Nothing here traces, so every point sits on the region centre's Z — chain `spatial.ground_instances` (for an ISM/HISM scatter) or `spatial.ground_actors` afterwards. **Jitter must be continuous**, and this verb makes it so: a jitter drawn from a small set of values (the classic hand-rolled `(-1, 0, 1) * amount`) re-lands two thirds of the points onto one hex sublattice, renders as banding from overhead and as rows from the ground, and passes every count, spacing and bounds check a caller would think to run. **The lattice is anchored at the world origin**, not at the region, and per-point randomness is keyed on the global lattice index — so two overlapping regions agree exactly on their intersection instead of double-seeding it. All four random draws happen per point even when their knob is off, so toggling `randomYaw` does not shift the scale sequence. A lattice larger than `maxPoints` is **refused**, not truncated: a layout that stops partway across the region reads as a bug in the level rather than in the call.
