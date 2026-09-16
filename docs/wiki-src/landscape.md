# landscape

Create and edit `ALandscape` actors — UE's heightfield terrain system — including component grid spawn, brush sculpting, bulk heightmap edits, landscape material swaps, grass-type assets, and one-shot noise-driven terrain seeding.

Use this namespace for heightfield terrain authoring; for static-mesh-driven scattering (rocks, props) reach for `foliage` or instanced static mesh actors via `actor`, and note that landscape grass authored here renders through the landscape material's grass node, not through the foliage tool.

## Geometry constraints (`create`)

UE landscapes require `ComponentSizeQuads == SubsectionSizeQuads * NumSubsections`, so `create` does not take an arbitrary whole-component size. Pass the per-subsection size and the subsection grid; the handler derives the rest:

- `quadsPerComponent` is the **per-subsection** quad count (`SubsectionSizeQuads`) and must be one of `7, 15, 31, 63, 127, 255` (i.e. `value + 1` is a power of two). It is **not** the whole-component size.
- `sectionsPerComponent` is the **total** section count: `1` (a 1x1 grid → `NumSubsections = 1`) or `4` (a 2x2 grid → `NumSubsections = 2`).
- The whole-component size is computed as `NumSubsections * SubsectionSizeQuads`.

Any other combination is rejected with `INVALID_ARGUMENT` rather than silently truncated — e.g. `quadsPerComponent=31, sectionsPerComponent=4` does not produce a `31/7/4` actor; either `31` is a valid subsection size on its own (with a 2x2 grid the component spans `62` quads) or you choose values that satisfy the invariant. The response echoes back the derived `subsectionSizeQuads`, `numSubsections`, and `componentSizeQuads`.

## Layer painting requires a material that declares the layer

Landscape layers are not free-form names — a layer exists only because the **landscape material declares it**. `create_procedural_terrain` (the layer-paint verb, misnamed) cannot invent one, and it will not pretend it did.

A landscape material must contain a **`LandscapeLayerBlend`** node (or `LandscapeLayerWeight` / `LandscapeLayerSample`) whose entries carry the layer names you intend to paint. Those names are what the engine calls the landscape's *target layers*, and they are the only names any paint call accepts. A material without one — including the default `/Engine/EngineMaterials/WorldGridMaterial` that `create` assigns when you pass no `materialPath` — has **zero** target layers, so **no** layer name can be painted on that landscape.

How to check before painting:

- Call `create_procedural_terrain` with any layer name and read the error. `LAYER_NOT_FOUND` lists every layer that *is* available (in the message and in error data `availableLayers[]`), and `LANDSCAPE_MATERIAL_NO_LAYERS` means the material declares none at all. Either way the answer arrives in one call.
- Or inspect the landscape actor's `TargetLayers` / the material's layer-blend node in the editor. A `target_layers` list holding only `__LANDSCAPE_VISIBILITY__` is the engine's hole mask, **not** a paintable layer — that landscape has no usable layers.

To fix a landscape with no usable layers: `material.authoring.create_landscape_material`, then `material.authoring.configure_layer_blend` with your layer names, assign it with `set_material`, then re-run the paint. `configure_layer_blend` writes the names into a `LandscapeLayerBlend` node and reports the target layers the material now declares in `targetLayers[]` — read that, not the success envelope. It also wires the node into `BaseColor` (`connectTo`) so the paint is visible, but **never over an input that is already wired**: when it declines, `connectionState` is `skipped_input_already_wired` and a `warnings[]` entry says so. That case is paintable-but-unsampled — wire the node yourself with `material.graph.connect_nodes`.

## Making a landscape material edit reach the screen

**Editing a landscape material — a rewired graph *or* a single constant value — changes nothing on screen until the landscape rebuilds its per-component material instances.** The asset can be correct on disk and compile successfully while the terrain renders its old look, because every `ULandscapeComponent` draws through a cached material instance holding the previous shader map.

**`material.authoring.compile_material` now does the rebuild itself.** One call is enough:

```js
call("material.authoring.compile_material", { assetPath: "/Game/Terrain/M_Ground" })
// -> consumerRefresh: { measured: true, consumersFound: 1, consumersRefreshed: 1,
//                       subObjectsRefreshed: 64, refreshed: ["Terrain"], complete: true }
```

Check `consumerRefresh.complete` before believing the edit reached the terrain, and check that your landscape is named in `consumerRefresh.refreshed`. `subObjectsRefreshed` counts components that received a *new* material instance, measured by object identity, so it cannot be faked by the code that issued the rebuild. When coverage is incomplete the response carries a `warnings[]` entry and you fall back to `landscape.set_material` on the named landscape.

**On older builds, compiling alone was not enough** — the base recompiled, but component instances did not, so every verb reported success while the edit measured as a no-op. (The claim that constant edits reach the cached shader through the uniform buffer is false for landscapes.) There were two caches to fix: the master's `PostEditChange` created no `FMaterialUpdateContext`, leaving dependent static-permutation shader maps stale, while `ALandscapeProxy::MaterialInstanceConstantMap` cached per-layer-allocation combinations that no material update context could see. `compile_material` now opens the update context and forces `UpdateAllComponentMaterialInstances` on every landscape rendering with the material.

`landscape.set_material` reaches the same rebuild through the assignment path and remains the fallback:

```js
call("landscape.set_material", { landscapeName: "Terrain", materialPath: "/Game/Terrain/M_Ground" })
```

Re-assigning the **same** material is the normal iteration step and is expected to work — the rebuild is driven by the property-changed notification, not by the pointer differing. The response echoes `previousMaterialPath` and `changed`, so a refresh (`changed: false`) is distinguishable from a real assignment without re-reading the actor.

**On builds before this was fixed, `set_material` silently did nothing** and the only route was a *round-trip*: assign a different material, then assign the intended one back.

```js
// Workaround for older builds only — no longer required.
call("landscape.set_material", { landscapeName: "Terrain", materialPath: "/Engine/EngineMaterials/WorldGridMaterial" })
call("landscape.set_material", { landscapeName: "Terrain", materialPath: "/Game/Terrain/M_Ground" })
```

The cause was not the pointer being unchanged: the handler wrote `LandscapeMaterial` and called a bare `PostEditChange()`, which builds an **empty** `FPropertyChangedEvent`. With `MemberPropertyName` unset, the engine's `LandscapeMaterial` branch — which empties `MaterialInstanceConstantMap` and calls `UpdateAllComponentMaterialInstances()` — never matched any assignment. The round-trip worked only because the first assignment tore the components down as a side effect. The handler now names the property in the change event, so one call is sufficient.

Measured on one fixed camera against one metric: values written plus compile → no change; plus `set_material` with the same material → still no change; plus the round-trip → the expected result. That third row is what the fix removes the need for.

The **graph-edit** path needed its own fix and did not inherit that one. Naming `LandscapeMaterial` in a change event only helps when there *is* an assignment; a rewired master performs none, so nothing on the landscape ever heard about it. Measured with auto-exposure pinned off (`r.EyeAdaptationQuality 0`) on a per-channel hue control, so no scalar exposure gain can produce or mask the result: wiring pure magenta into the terrain master's `BaseColor` and stopping there moved the frame **0.04%**, the noise floor; the identical edit followed by `compile_material` moved it **94.12%**. That is the gap `compile_material`'s `consumerRefresh` now closes and reports.

**Never call `MaterialEditingLibrary.recompile_material()` from `python.execute`** for a landscape material. It forces garbage collection while the Python frame is on the stack; the Python pre-GC hook can fault, taking down the editor and losing unsaved work. It is non-deterministic — a first call can return normally and a second be fatal, so "it worked when I tried it" is not evidence. `material.authoring.compile_material` does the work natively without garbage collection and returns a verdict. (`UMaterial.post_edit_change()` is not an escape: it does not exist on UE 5.8.)

## Making a grass-type edit reach the screen

**A `ULandscapeGrassType` edit is the same defect class as the material one above, on a different cache.** Grass is not drawn from the asset: every landscape builds HISM clusters from it and caches them per-proxy, keyed on the grass type *pointer* and the variety *count* — not on anything inside an `FGrassVariety`. Changing `GrassDensity`, `ScaleX` or `GrassMesh` therefore leaves every cached key equal to itself, and the terrain keeps drawing the old grass while the asset on disk is correct and every verb reports success. Measured at one fixed pose: the edit moved `meanLuminance` 0.4444 → 0.4439 (the noise floor); flushing the cache moved it → 0.4287. (UE 5.3 flushed the consumers inside `ULandscapeGrassType::PostEditChangeProperty`; from 5.4 that body only invalidates a summary and recomputes `StateHash`, neither of which reaches instances already built.)

**The reflected mutators do the flush themselves**, and `property.set` reports what it reached:

```js
call("property.set", { objectPath: "/Game/Landscape/GT_Grass.GT_Grass",
                       propertyName: "GrassVarieties[0].PlacementJitter", value: 0.5 })
// -> consumerRefresh: { measured: true, consumersFound: 1, consumersRefreshed: 1,
//                       subObjectsRefreshed: 96, refreshed: ["Terrain"], complete: true }
//    grassMaps:       { measured: true, componentsHoldingMapsBefore: 136,
//                       componentsHoldingMapsAfter: 136 }
```

Read `consumerRefresh.complete` and check your landscape is named in `refreshed[]` — the counts are read from the proxies' cache before and after, not from the fact the call returned. `grassMaps` is the second block, and it answers a different question: `componentsHoldingMapsAfter` equal to `componentsHoldingMapsBefore` means the refresh dropped the built grass **instances** and left the per-component density **maps** alone, which is the only correct outcome. A `discarded` field appears there only when maps were lost; it is never emitted on a healthy run, so its presence is the alarm. For an edit made **outside** this plugin (the asset editor, `python.execute`, an undo) use `landscape.flush_grass`, whose page explains both blocks field by field.

> **Do not reach for `grass.FlushCache` as a shortcut.** It is the console command the plugin's flush was originally modelled on and it is *not* equivalent: it takes `ALandscapeProxy::FlushGrassComponents`' defaulted `bFlushGrassMaps=true`, which additionally calls `ULandscapeComponent::RemoveGrassMap()` on every landscape component in the process and replaces each component's density data with an empty one. Measured live: **922,280 → 56,080** grass instances in one level, and on a second run **136 → 0** grass components with no recovery from camera moves, from `grass.Enable 0/1`, or after several minutes — only an editor restart. Nothing in the editor rebuilds those maps synchronously; the amortised, camera-driven grass-map builder is the only path back, and **saving the level while they are empty writes the empty data into the landscape package**. `grass.FlushCachePIE` is the non-deleting variant despite its name (`FlushGrassComponents(nullptr, false)`), and it is what `landscape.flush_grass` and the reflected mutators do. The engine agrees this is the right level: when its own grass-map builder detects a changed grass type it notes *"this invalidates foliage instances but not the grass maps"* and routes the component through `ULandscapeSubsystem::RemoveGrassInstances`, which passes `bFlushGrassMaps = false`.

**Two flush routes that do not exist, so you do not spend a source dive finding out.** `grass.FlushCacheAll` is not a registered console command on any engine — only `grass.FlushCache` and `grass.FlushCachePIE` are — and `unreal.Landscape.flush_grass_components` is not a Python attribute, because `ALandscapeProxy::FlushGrassComponents` is exported C++ but not a `UFUNCTION`; the same is true of `ULandscapeSubsystem::RegenerateGrass`.

## Reading heights back

`get_heights {landscapeName, region: {minX, minY, maxX, maxY}}` is the collision-free readback, and it is what you want when a trace would answer "top of whatever is standing there" rather than "the terrain". Its `region` is in **heightmap pixels, not world units** — convert with the landscape actor's own origin and draw scale, `px = (world_x - origin_x) / scale_x` — while the returned `minZ` / `maxZ` / `meanZ` are already world centimetres.

**There is no verb that samples terrain height at a single world XY.** Either take a one-pixel region through this verb, or trace with `spatial.raycast {traceComplex: true, onlyClasses: ["LandscapeProxy"]}` — the class filter is not optional there, because a collisionless foliage mesh blocks a complex trace. See [`level-building.terrain-and-water`](level-building.terrain-and-water.md).

**Every number `get_heights` returns describes terrain that exists.** A region reaching past the landscape is clamped, and the response then carries three things it used to omit: `region` (the rectangle actually read), `requestedRegion` (what you asked for), and `omittedSampleCount`, with the clamp restated in `warnings[]`. `sampleCount` and the `minZ`/`maxZ`/`meanZ` aggregates cover the read rectangle only. A region no part of which is inside the extent is refused with `INVALID_ARGUMENT` naming the extent, and a region the engine finds no `ULandscapeComponent` for is refused with `LANDSCAPE_NO_HEIGHT_DATA`. `landscape.audit_shape` shares this read path and refuses on the same two conditions.

**On builds before this, a region past the far edge came back as a lie you could not detect.** All four coordinates clamped onto the edge pixel, the engine filled the buffer with interpolation, and the response was `success: true`, `sampleCount: 1`, `meanZ: 0` — the uint16 midpoint, which reads exactly like flat ground at sea level — with `region` echoing the engine's uninitialised `{2147483647, 2147483647, -2147483648, -2147483648}` out-params. Any heightmap assembled from an older build near a landscape edge is worth re-reading.

**`get_heights` leaves every package's dirty flag exactly as it found it** — clean stays clean, dirty stays dirty. It is safe to sweep a whole heightmap for measurement without the level acquiring unsaved changes, and a dirty package after a read-then-write sequence is still evidence that the *write* landed.

**On builds before this, it was not.** One 5x5 `get_heights` on a freshly booted editor took `editor.list_dirty_packages` from 0 to 1, naming the map — because `FLandscapeEditDataInterface` is an edit interface even when only read from, and its texture accessor calls `Modify()` on the heightmap texture regardless. Measurement passes taken on an older build therefore leave the map dirty; that dirty flag means nothing, and the level can be closed with `discard: true`.

## Gotchas

- `set_material` on a large landscape can stall the editor while proxies recompile. Test on a small landscape first.
- Stamp brushes mutate the heightmap; if you need to undo, snapshot the actor (via `call("actor.create_snapshot", …)`) before sculpting.
- A landscape sculpt does not mark the map package dirty by itself, so a later save can no-op and lose the edit. Mark it with `call("asset.mark_dirty", {assetPath: "<map package>"})` — the engine's `mark_package_dirty` / `set_dirty_flag` are not exposed to Python on UE 5.8. See [`python`](python.md).

## Sculpting a shape versus writing heights

Two write paths, and the choice shows in the **outline**, not in the heights.

- **`sculpt`** sweeps a brush along a **world-space path** in one call. Brush weight comes from each vertex's distance to the nearest point of the polyline, so the edited region is the exact offset curve of the stroke and its boundary curves. Centres are sub-cell accurate.
- **`edit`** writes raw height samples over a heightmap-pixel region. Assigning a value **per cell** from a region mask makes the boundary follow cell edges by construction: the outline comes out a 90-degree zigzag however correct the heights are.

**Neither is wrong in general.** A per-cell write is the right tool where the thing genuinely is cell-aligned — axis-aligned platforms, plinths, paving, coarse massing — and it is the wrong tool for organic boundaries such as cliff lines, river corridors and clearing edges. Reach for `edit` when you have a heightfield to install and `sculpt` when you have a shape to draw.

**A steep face and a blocky face are independent properties.** Quantising Z has nothing to do with the staircase; a one-cell cliff between two stamped values is steep *and* blocky, and the fix for the second is sub-cell accuracy on the boundary, not a different height.

**Resolution decides whether the artifact is visible at all.** The staircase is one cell of horizontal run against the height of the step it sits on. On a fine grid it disappears into the silhouette; on a coarse one each tread is a feature. Before blaming the technique, work out how many cells span the smallest thing you need to resolve — and compare that figure in the units the *world* is measured in, never in raw `uu`, if your project has ever changed its scale convention.

## Drawing an organic boundary with `sculpt`

Hand `sculpt` the curve. `path` is a world-space polyline applied in **one** call, and `brushRadius` is the stroke half-width:

```js
call("landscape.sculpt", {
  landscapeName: "Terrain",
  path: [{x: -12000, y: 3000, z: 0}, {x: -4000, y: 5200, z: 0},
         {x: 3500, y: 4100, z: 0}, {x: 11000, y: -900, z: 0}],
  toolMode: "Flatten",
  targetHeight: 800,
  brushRadius: 1400,
  brushFalloff: 0.35,
  falloffProfile: "smooth"
})
```

Why this reads as a curve rather than a row of scallops:

- **Brush weight is distance to the polyline, not to a stamp.** The edited region is the exact offset curve of the stroke, evaluated per vertex, so there is no sample spacing to tune and no accumulation where consecutive parts of the stroke overlap. A chain of separate stamps beads; a sweep cannot.
- **Centres are sub-cell accurate.** Nothing is rounded to a heightfield vertex unless you pass `snapToVertex: true`, and the response echoes the fractional centre it used in `firstCentreVertexFractional`. Rounding was the previous behaviour and it was what turned a shallow diagonal into 90-degree treads.
- **Z is interpolated along the stroke.** Each path point's `z` is the `Flatten` target at that point, so one call cuts a graded river bed or a sloped road. Pass `targetHeight` to flatten the whole stroke to one level instead.

Four falloff profiles are available through `falloffProfile`, and they are the engine's own Landscape Ed Mode brush curves: `linear` (the default and the historical ramp), `smooth` (smoothstep, alias `smoothstep`), `spherical` (a dome shoulder that rises fast off the rim) and `tip` (hugs the rim, spikes at the plateau). `brushFalloff` sets what fraction of the radius the ramp occupies — `0` is a hard edge, `1` ramps from the centre.

Weight runs `t = 0` at the outer rim to `t = 1` at the inner plateau edge, and the four are `t`, `t²(3−2t)`, `√(1−(1−t)²)` and `1−√(1−t²)`. Which side of `linear` a profile sits on is not intuitive and is worth pinning before you pick one: `smooth` **lags** linear over the outer half of the ramp and **leads** it over the inner half — `0.15625` against linear's `0.25` a quarter of the way in from the rim, `0.84375` against `0.75` three quarters in — so it feathers the rim harder and pushes the transition inward. `spherical` is above linear everywhere on the ramp (`0.866` at the midpoint), `tip` below it everywhere (`0.134`).

Before trusting a metric, measure the boundary crossing as a fraction of a cell (bisect the distance field), not which cell it lands in: at one-cell stride every raster boundary is cell-aligned and reads `1.000`, while an advance fraction measured a swept sub-cell diagonal at `0.893` and lattice-snapped stamps at `0.920` (the broken form scored better). Then capture the terrain against its reference at oblique and top-down angles; top-down ortho reduces edge-on faces to a line.

## What `sculpt` still cannot do

- **No spline or curve asset input.** `path` is a polyline of explicit points; there is no Bezier, no landscape spline, and landscape splines are not exposed by any verb.
- **Round profile only.** The stroke is a swept disc — there is no square, ellipse or alpha/pattern brush, and no per-point radius.
- **No engine sculpt tools.** Erosion, Hydro, Noise, Ramp and Retopologize are not reachable through any verb. `toolMode: "Smooth"` is a local box average over the heights the call reads back, **not** the engine's Smooth tool (`FLandscapeToolStrokeSmooth` lives inside the LandscapeEditor module behind an `FEdModeLandscape` and cannot be driven headlessly). It is the right companion to a sweep — run the same `path` again with `toolMode: "Smooth"` to relax the shoulders — but do not expect the interactive tool's kernel.
- **One landscape per call.** A stroke crossing two landscape actors edits only the one you name.
- **Bounded on footprint, not on path length.** A stroke whose clamped bounding box exceeds 4,194,304 heightfield vertices is refused with `OUT_OF_BOUNDS` naming the arithmetic; the handler cannot yield mid-write, so a larger sweep would outlive its own response timeout. Split the path.


## Measuring whether the outline actually curves

`audit_shape` is the read verb that checks the section above on real content instead of trusting it. It is the only landscape verb that measures **shape** rather than height, and it exists because two complete terrains were rejected on sight while every number in their reports was correct.

It reports two independent scalars over a heightmap region, plus per-check findings:

- **`axisFraction`** — of the band-boundary chords taken at a stride of `stride` cells, the fraction running within `epsDeg` of a cell axis. Axis-aligned rectangular plateaus score ~0.89; a sculpted dome under a diagonal tilt scores ~0.09.
- **`stepFraction`** — the share of the region's total height variation carried by **isolated risers**: a nonzero sample-to-sample delta with a flat sample on each side. Terrain stamped one tier value per cell scores 1.000; a sculpted slope scores 0.000.

**The stride is the whole trick, and skipping it produces a metric that cannot fail.** Every boundary between two cells of a raster is a unit segment on the cell lattice, so measured *per segment* a traced curve and a stamped mask both read exactly 1.000 — asserted on all three fixtures by `PinWright.landscape.audit_shape.PerSegmentReadingCannotFail`. Likewise, do not measure conformance to the step size you were handed: quantised noise scores 100% on that and reads green while the terrain is 90-degree blocks.

**Three stated limits.** They are printed in the response's `limits[]` on every call, so a green here means exactly what it says and no more:

1. `axisFraction` **cannot see a staircase whose risers are shorter than the stride** — it averages out to the diagonal it approximates. A mask upsampled 8x measured ~0.41 on the retired gate this ports the idea of, and a *per-cell* staircase from a radial mask measures 0.055 here. That is the reason `stepped_profile` exists and the reason both checks are on by default; run them together or you have covered half the failure.
2. **A genuinely axis-aligned shape scores high and that reading is correct.** Platforms, plinths and paving are a legitimate per-cell write. This verb measures shape, never intent — drop `axis_locked` from `checks` for a region you know is meant to be rectangular rather than raising the bar.
3. It reads the **stored heightfield only**. Runtime displacement, tessellation and material-driven offset are invisible to it, as is anything an edit layer has not flushed.

**Unmeasurable is never a pass.** A flat region, a boundary too short to measure at this stride, and a region smaller than one chord each return an `unrunnable` finding naming which of the three it was — and any unrunnable check makes `pass` false whatever `failOn` says. A fraction over an empty denominator is not an answer.

**Where 0.50 and 0.60 come from, and how to pick your own.** The two default thresholds are a **separation boundary**, not a tuned constant. Measured on one project's terrain: stamped masks - the failure the verb exists to catch - scored `axisFraction` ~0.937 and `stepFraction` ~0.871, while organic sculpted terrain scored ~0.060 and ~0.025. The two populations sit about 0.80 apart on both scalars, so anything in the middle of that gap separates them and the exact value carries no information; `maxAxisFraction` 0.50 and `maxStepFraction` 0.60 are simply in the middle. **Those numbers come from one project's terrain and nothing wider** - a different cell size, height range or authoring convention can land anywhere. On such content, measure a region you know is good and a region you know is bad, check that they separate at all, and set `maxAxisFraction` / `maxStepFraction` between them; both are echoed in every response, so the value a run used is always recoverable from its report. If the two do not separate, the defaults would not have helped either - that is a signal to widen `stride` or the region, not to move the bar.

**Not a replacement for looking at the terrain.** It is a cheap finding, not a verdict: it makes a bad outline cheap to catch before a capture burst is worth running. Terrain can pass every check here and still be wrong.

## See also

- [`level-building`](level-building.md) — the guide for taking a level from nothing to playable: massing geometry, conforming it to terrain, and driving it from re-runnable build scripts.
- [`material.authoring`](material.authoring.md) — building the landscape material whose `LandscapeLayerBlend` declares the paintable target layers.
- [`foliage`](foliage.md) — static-mesh scattering, a different system from the landscape-material grass authored here.
- [`level-review`](level-review.md) — checking the terrain reads right at the ranges and angles a top-down shot cannot resolve.

### landscape.audit_shape

Measures whether a landscape's height-change boundaries **curve** or follow the heightfield's own cell lattice. Read-only, and the level's dirty flag is preserved exactly — clean stays clean, dirty stays dirty. See *Measuring whether the outline actually curves* on the [`landscape`](landscape.md) page for what the two scalars mean, for the three limits every green carries, and for where `maxAxisFraction` 0.50 / `maxStepFraction` 0.60 came from - they are a separation boundary measured on one project's terrain, so content unlike it should measure its own.

**A shape defect is content, not an error.** This verb returns success with `pass: false` and structured findings. It sends an RPC error only for a malformed request — an unknown check id, an unknown `failOn`, an empty `checks`, a region above the 4,194,304-sample ceiling.

**Checks** (both on by default; naming an unknown id is rejected rather than skipped, because a typo that ran nothing looks exactly like terrain that passed):

| id | code | flags when |
| --- | --- | --- |
| `axis_locked` | `LANDSCAPE_SHAPE_AXIS_LOCKED` | `axisFraction` > `maxAxisFraction` (0.50) |
| `stepped_profile` | `LANDSCAPE_SHAPE_STEPPED` | `stepFraction` > `maxStepFraction` (0.60) |

**Unrunnable reasons**, each of which makes `pass` false regardless of `failOn`: `LANDSCAPE_SHAPE_FLAT_REGION` (every sample at one height — nothing to measure), `LANDSCAPE_SHAPE_NO_BOUNDARY` (fewer than `minChords` chords at this stride), `LANDSCAPE_SHAPE_REGION_TOO_SMALL` (the region is smaller than one chord).

**Thresholds**, all optional and all echoed in the response: `stride` (8), `epsDeg` (5.0), `marginCells` (0), `bandCount` (16), `bandHeight` (0 = derive from the region's range), `minChords` (24), `minStepUnits` (8), `maxAxisFraction` (0.50), `maxStepFraction` (0.60).

`bandHeight` is how the boundary is **found**, never what is scored — the verdict comes from chord direction, so a band width that happens to match the content's own quantisation cannot flatter it. `minStepUnits` keeps `stepFraction` honest on gentle terrain, where a slope shallower than one height unit per sample quantises into flat runs with single-unit risers; those risers still count in the denominator and are reported as `subThresholdSteps`.

```
call("landscape.audit_shape", {landscapeName: "Landscape",
                               region: {minX: 0, minY: 0, maxX: 255, maxY: 255}})
-> {pass: false, summary: {errors: 2, unrunnable: 0},
    metrics: {axisFraction: 0.886, stepFraction: 1.000, chords: 140, bandHeight: 125, ...},
    findings: [{check: "axis_locked", status: "flagged", code: "LANDSCAPE_SHAPE_AXIS_LOCKED", ...}]}
```

`region` is in **heightmap pixels**, defaulting to the full extent, and resolves identically to [`landscape.get_heights`](landscape.get_heights.md). A region no landscape component covers — wholly or partly — is refused with `LANDSCAPE_NO_HEIGHT_DATA` naming the sub-rectangle to ask for instead, for the same reason the sample ceiling refuses an oversized one: a verdict derived partly from the engine's interpolated fill would be a different measurement wearing this one's name. Pass `includeHistogram: true` for the 18-bucket chord-angle histogram (5 degrees per bucket over 0..90) when you want to see *how* the directions are distributed rather than only how many crossed the bar.

### landscape.create_grass_type

Creates one `ULandscapeGrassType` from a static mesh and saves it. Two arguments decide where it lands, and they are not interchangeable.

**`name` is a BARE asset name, never a path.** It is validated against the engine's own object-naming rules (`FName::IsValidXName` / `INVALID_OBJECTNAME_CHARACTERS`) and the composed package path against `FPackageName::IsValidLongPackageName`, so a `/`, `\`, `.`, `..`, a leading or trailing slash, a space or an unmounted root is rejected `INVALID_ARGUMENT` with the engine's own reason text quoted. This is not pedantry about naming: a name beginning with `/` used to compose `/Game/Landscape//Game/...`, and `CreatePackage` logs a double slash at **Fatal**, which is not compiled out in any configuration — the call did not fail, the editor **process** died, taking every unsaved package in that editor with it. The same defect was measured end-to-end on `foliage.add_type`; see that verb on the [`foliage`](foliage.md) page.

**`savePath` (optional, default `/Game/Landscape`) is how you choose the folder.** Same spelling, same validation and the same `SECURITY_VIOLATION` on an unsafe value as `foliage.add_type` and `foliage.create_procedural`; a path containing `..` or sitting outside a mounted root is refused before anything is created. The effective folder is always echoed as `save_path` — on the "already exists" branch too — so the destination is never a literal you have to know.

An existing asset at the composed path is returned as-is (`message: "Asset already exists"`) rather than overwritten. For what `density` versus `effectiveDensity` mean and why they differ, see *Making a grass-type edit reach the screen* above.

### landscape.create_procedural_terrain

Paints a weight-blended layer over a region of a landscape. The name is a misnomer: it creates no terrain and sculpts no heightmap — it writes one layer's weightmap. Use `create` to spawn a landscape and `sculpt` / `edit` to shape it.

**Prerequisite:** `layerName` must already be a target layer on the landscape material. See *Layer painting requires a material that declares the layer* above; there is no auto-create for a layer the material does not declare.

The verb previously answered every call with `{"success": true, "message": "Layer painted successfully"}`, including calls that painted nothing. Every way it could do that is now a typed error:

| Error | Meaning | Recovery |
|---|---|---|
| `LAYER_NOT_FOUND` | `layerName` is not a target layer on this landscape's material. The message lists the layers that are; error data carries `availableLayers[]` and `availableLayerCount`. | Paint one of the listed names, or add yours to the material. |
| `LANDSCAPE_MATERIAL_NO_LAYERS` | The material declares no paintable target layers at all (no `LandscapeLayerBlend`), so no layer name would work. Error data carries `materialPath`. | Build and assign a landscape material with named layers. |
| `LANDSCAPE_NO_MATERIAL` | No landscape material assigned. | `set_material`. |
| `LANDSCAPE_NO_COMPONENTS` | Hollow landscape — the actor has no registered `ULandscapeComponent`s, so it has no paintable extent. | Recreate it with `create`. |
| `INVALID_ARGUMENT` | `layerName` missing, or the `region` is empty after clamping to the landscape extent (e.g. `minX > maxX`, or a region entirely outside the landscape). | Fix the region; the message reports the actual extent. |
| `LANDSCAPE_NOT_FOUND` / `INVALID_LANDSCAPE` | The actor could not be resolved, or has no `ULandscapeInfo`. | Check `landscapeName` / `landscapePath`. |
| `LANDSCAPE_ORPHANED_LAYER_WEIGHT` | The landscape carries weightmap weight for a layer that is **not** a registered target layer, and painting would erase it landscape-wide with no undo. Error data carries `orphanedLayers[]` (`layerName`, `layerInfoPath`, `componentCount`, `live`), `orphanScanComponents`, `orphanScanProxies`, `orphanScanLoadedProxiesOnly`. | Re-register the layer in Landscape Ed Mode's Target Layers panel, or pass `allowOrphanedLayerLoss: true` to accept the erasure. See *Orphaned layer weight* below. |

`region` coordinates are **landscape heightmap pixels**, not world units. Each of `minX`/`minY`/`maxX`/`maxY` independently defaults to the corresponding full-landscape extent and is clamped into it; a clamp that changed what you asked for is reported in `warnings[]`, and the region actually painted is echoed back in `region`.

On success the response carries `layerName`, `materialPath`, `strength` (clamped to 0..1), `paintValue` (the 8-bit weight written), `region`, `regionSizeX`/`regionSizeY`, `paintedTexels`, `layerInfoAutoCreated`, `layerBlendMethod` (the **measured** blend method of the layer painted), `componentSizeQuads`/`componentSizeUU` and `requestedFootprintUU`. Unless you pass `verify: false` (or `skipFlush: true`) it also settles the deferred edit-layer regeneration and reads the weightmap back, reporting `sampledTexels`, `texelsWithWeight`, and `texelsAtRequestedWeight` — check `texelsWithWeight > 0` to confirm the paint really landed. `texelsAtRequestedWeight` can legitimately be lower than `paintedTexels`: the composited weightmap renormalizes this layer's weight against the other layers sharing the blend. (`SetAlphaData` itself does not renormalize — it writes only the named layer's channel. The renormalization is the edit-layer merge's final weight-blending pass.)

**Painting one layer costs the other layers in the same blend group — inside the painted region.** Weight-blended layers normalize to a sum of 1, so `strength: 1.0` on layer A necessarily drives every sibling toward 0 wherever A was written. That is the engine's behaviour, and it is the same thing Landscape Ed Mode's paint brush does: the engine's own stroke enumerates the target layer's blend group and writes *every* member of it, the painted one up and the siblings down, strictly inside the brush bounds. The consequence for callers: **build multi-layer terrain by painting each layer over the region it should own** — never by painting one layer over the full extent and then a second one on top, which erases the first everywhere they overlap.

**That whole paragraph is conditional on the layer's blend method, and the response says which one it has.** A `ULandscapeLayerInfoObject` on `ELandscapeTargetLayerBlendMethod::None` — display name *"No Weight Blending"* — is not admitted to the merge's final weight-blending pass at all (`LandscapeEditLayers.cpp:3594`), so painting it at `strength: 1.0` reduces no sibling and two layers can both hold 255 on the same texel, which a normalised weightmap cannot. `layerBlendMethod` reports the **measured** method of the layer this call painted, `layersAffected[].blendMethod` reports it per sibling, and a layer that is not weight-blended raises a `warnings[]` entry saying so at the call site.

**A LayerInfo this verb auto-creates is `FinalWeightBlending`, not the engine's default.** `ULandscapeLayerInfoObject`'s constructor takes its method from `ULandscapeSettings::TargetLayerDefaultBlendMethod`, whose compiled default is `None` — so a freshly constructed layer would silently be excluded from the normalisation the paragraph above describes. Two things override that, in order:

1. **The project template.** If `ULandscapeSettings::DefaultLayerInfoObject` is set, the new layer is **duplicated** from it and keeps that template's blend method, blend group and physical material. `layerInfoTemplatePath` names the template and is **omitted** when none is configured. (This is the `DefaultLayerInfoObject` hook `UE::Landscape::CreateTargetLayerInfo` implements; the engine factory itself is not called, because its first act is `CreatePackage()` — it always mints a `/Game` asset, whereas this verb creates a private per-landscape LayerInfo outered to the landscape actor.)
2. **`weightBlended`.** Supplied explicitly, it wins over the template: `true` → `FinalWeightBlending`, `false` → `None`. Omitted with no template configured → `FinalWeightBlending`.

`weightBlended` applies **only** on the auto-create path. An existing `ULandscapeLayerInfoObject` is potentially a shared asset, and a paint call never rewrites its blend method — supplying the parameter for a layer that already has one is reported in `warnings[]` rather than ignored. Change an existing layer's method with `material.authoring.add_landscape_layer` or in Landscape Ed Mode's Target Layers panel.

The verify pass makes that cost a reported number rather than a discovery in the editor. It censuses every **registered** layer over the **full** landscape extent before and after the write and returns:

| Field | Meaning |
|---|---|
| `layersAffected[]` | One entry per layer: `layerName`, `painted`, `blendMethod`, `texelsWithWeightBefore`/`After`, `texelsWithWeightOutsideRegionBefore`/`After`. `blendMethod` is what tells a flat before/after pair apart from a layer that was never in the blend group. |
| `otherLayerTexelsLostInRegion` | Weight the other layers lost **inside** the region. Expected and normal at high strength — this is the normalization above. |
| `otherLayerTexelsLost` | Weight the other layers lost **outside** the region. Should always be `0`; a region-bounded paint must not touch any layer outside its region. Non-zero also raises a `warnings[]` entry. |
| `censusTexels` | Texels sampled per layer per pass. |

The census costs one full-extent read per layer, twice, so it is **skipped with a `warnings[]` entry** on a landscape larger than 4,194,304 texels, and the fields above are then absent rather than zero. `verify: false` / `skipFlush: true` skip it too.

**The census reads the registration side, so it cannot see an orphaned layer.** Its loop is `ULandscapeInfo::Layers` with null layer infos skipped, which is exactly the set the weightmap merge keeps — so weight allocated for a layer that is *not* registered is absent from `layersAffected[]` and contributes nothing to `otherLayerTexelsLost`, right up until the merge destroys it. `otherLayerTexelsLost: 0` therefore means "no *registered* layer lost weight outside the region", never "nothing was lost". That gap is covered by the separate orphaned-allocation scan below, not by `verify`.

**And the census counts TEXELS, so it cannot express damage in which no texel changed.** `otherLayerTexelsLost` is a difference of two counts of non-zero weightmap bytes. A clean census means "no registered layer's byte count fell outside the region" — it is **not** a statement that the frame is unchanged. The orphaned-allocation scan does not close that gap either: it reports allocations for layers that are *not* registered, and every layer involved below *is* registered. The two together still leave the per-component effect in the next section, which is measured and reported on its own fields.

**The blast radius of a paint is the COMPONENT, not the region.** A `ULandscapeComponent`'s material permutation is built from *that component's own* weightmap allocation list and nothing else (`GetLayerAllocationKey`, `LandscapeEdit.cpp:549`; one `FStaticTerrainLayerWeightParameter` per allocation, `:638-648`). A layer absent from that per-component set gets `INDEX_NONE` from the translator and compiles to `Constant(0.f)` under the engine's own comment *"layer is not used in this component, sample value is 0"*. So **the first allocation a virgin component gains makes every other material-declared layer it holds no allocation for sample as zero across the whole component** — including a `LandscapeGrassOutput` fed by a `LandscapeLayerSample`, which is how a 51×51-texel paint request removed the groundcover from 6300-uu components on the landscape this was measured on: 6.1× the requested area.

Nothing is erased when that happens. No weightmap byte changes, the map package is unchanged, and repainting the affected layers over those components restores the frame exactly — which is precisely why `otherLayerTexelsLost` reports `0` and why that `0` is correct rather than a lie. The reach is reported on its own fields instead:

| Field | Meaning |
|---|---|
| `componentSizeQuads`, `componentSizeUU` | The unit the reach is measured in: `ComponentSizeQuads` and its world span (`× drawScale`). Always present — nothing else in the response says how large a component is. |
| `requestedFootprintUU` | The **request**, named separately: the region's texel count × draw scale. Always present. |
| `componentsGainingAllocation` | Components whose base allocation array grew. |
| `componentsGainingFirstAllocation` | The subset that went from **empty** to non-empty — the expensive transition. |
| `layersNowSampledZeroOnTouchedComponents[]` | Per material-declared layer (`layerName`, `componentCount`): how many of the components this paint allocated on hold **no** allocation for it, i.e. compile it to `Constant(0.f)` across their whole span. Omitted when empty. |
| `allocationFootprintUU` | The **measured** world-space XY box of the affected components (`minX`/`minY`/`maxX`/`maxY`/`sizeX`/`sizeY`). Omitted, never zeroed, when no component gained an allocation. |

Those four measured fields ride on `verify`: without a settle the edit-layer merge has not rewritten the base allocation arrays, so the "after" read would measure the wrong state and the fields are **omitted rather than reported as zeros**. They are *not* subject to the census texel cap — the walk is a pointer comparison, not a weightmap read, so a landscape too large for `layersAffected[]` still gets its blast radius. Whenever `layersNowSampledZeroOnTouchedComponents[]` is non-empty the call also raises a `warnings[]` entry naming those layers, the measured footprint, the requested footprint, and their ratio.

**The order of work follows from this, and it is a property of the engine's per-component permutation rather than of any one map: paint the base layer over the FULL extent first, then the accents over their regions** (plus the base to `0` there if you want a hard edge). The first paint on a virgin component is the expensive one; once every component carries an allocation for the base layer, a later accent paint changes only the texels asked for and the edge is the rectangle's, not the component's.

**Orphaned layer weight.** A `ULandscapeComponent` can carry a weightmap allocation for a layer the landscape no longer registers. The Target Layers panel's **rename** produces one deterministically (it removes the old target layer without visiting a single component), and so do deleting a target layer while World Partition proxies are unloaded, clearing a layer's asset slot, undo/redo, and any tool that paints without registering its `ULandscapeLayerInfoObject` — the engine's `SetAlphaData` never checks. The next paint's edit-layer merge requests only the registered layers and deletes every other allocation from **every component of every loaded proxy**: landscape-wide, unrelated to `region` and `strength`, and **not** reversible with `editor.undo`, because the merge runs in the settle after the paint's transaction has closed.

Every call therefore scans what the weightmaps actually contain, before the write, regardless of `verify` and regardless of the census texel cap. The response always carries `orphanScanComponents`, `orphanScanProxies` and `orphanScanLoadedProxiesOnly`, and carries `orphanedLayers[]` when anything was found. A live orphan (non-null `LayerInfo`) is **refused** with `LANDSCAPE_ORPHANED_LAYER_WEIGHT`; pass `allowOrphanedLayerLoss: true` to accept the erasure, which downgrades the refusal to a `warnings[]` line and still reports `orphanedLayers[]`. A dead orphan (null `LayerInfo`) is only reported — nothing can sample it and no readback can reach it, so refusing would be obstruction.

`orphanScanLoadedProxiesOnly` is always `true` and is not decoration: the walk visits the parent `ALandscape` and the **resident** streaming proxies, so on a World Partition landscape an unloaded proxy's allocations are never examined and `orphanedLayers[]` is a lower bound. That case adds a `warnings[]` entry naming the proxy and component counts actually scanned.

The `SetAlphaData` write runs inside a single transaction, so `editor.undo` reverses **the write**. It does *not* reverse the edit-layer weightmap merge, which runs in the settle after that transaction closes — so neither an orphaned layer's erasure nor a non-zero `otherLayerTexelsLost` is recoverable by undo. Re-paint the affected layers instead.

`strength: 0` writes weight 0, which **erases** the layer over the region rather than painting it; any strength below ~0.002 quantizes to 0 for the same reason. Both cases are called out in `warnings[]` rather than left to look like a paint.

### landscape.sculpt

Sweeps a brush along a world-space path, or stamps it at one world location, in a single call. For installing a heightfield you already have, use `edit`; for drawing a shape, this is the verb — see *Drawing an organic boundary with `sculpt`* above for the worked example and *What `sculpt` still cannot do* for the remaining limits.

**The claim that the boundary curves is verifiable, not self-certified.** This verb's own summary asserts that its edited region is the offset curve of the stroke "rather than stepping along cell edges" — a statement about its own output. [`audit_shape`](landscape.audit_shape.md) measures it, on this verb's output or on terrain from anywhere else.

Pass **either** `location` (one stamp) **or** `path` (a polyline). Passing both is an `INVALID_ARGUMENT`; the two are different shapes for the same edit and picking one would discard your other intent. Internally a `location` is a one-point path, so the two forms share every line of geometry.

**`modifiedVertices` is a measured count.** It is the number of heightfield vertices whose stored height differs, taken by reading the heightmap back through a fresh edit interface after the write has flushed and settled — not by counting what the handler intended to write. The distinction matters on this engine: an edit-layer write that lands in no persistent layer is composited away by the regeneration, and the response says so in `warnings[]` when the planned and measured counts disagree. Read `verified` first: with `deferSettle` there is no readback, and the response then **omits** `modifiedVertices`, `maxHeightDeltaCm` and `meanAbsHeightDeltaCm` rather than reporting them as zero. `plannedVertices` is always present and is honestly named.

The rest of the response: `verticesInBrush` and `verticesConsidered` (the brush footprint and the clamped rectangle around it — `modifiedVertices` used to be the second of these, which is why a stamp that moved nothing reported the same number as one that worked), `changed`, `flushed`, `settled`, `region`, `pathPointCount`, `segmentCount`, `snappedToVertex`, `firstCentreVertexFractional`, `toolMode`, `falloffProfile`, and `warnings[]`.

**A sculpt that changes nothing is refused**, with `LANDSCAPE_SCULPT_NO_CHANGE` and a `reason` naming which case it was — a radius below one cell, a strength of 0, a `Flatten` already at its target, or a stroke that missed the landscape. Those four are indistinguishable from a clean run otherwise. Pass `allowNoChange: true` when a no-op is a legitimate outcome you want to observe rather than an accident.

**An unrecognised `toolMode` is an error.** `LANDSCAPE_INVALID_TOOL_MODE` carries `validToolModes[]`. It used to match nothing, leave the delta at zero and return `success: true` with `modifiedVertices: 0`, so a typo looked like a stamp on already-correct terrain. An unrecognised `falloffProfile` is refused the same way with `INVALID_ARGUMENT`.

**`brushRadius` is in world centimetres and honours both draw scales.** Distance to the stroke is measured through `ScaleX` and `ScaleY`, so the brush is a true circle in the world on a landscape with non-uniform XY scale, and the response warns when it detects one. It used to be converted through `ScaleX` alone, which made it an ellipse while reporting a circle's radius.

**Prefer `deferSettle` to `skipFlush`.** `skipFlush` on this verb never skipped a flush — it deferred only the edit-layer settle, unlike the identically-named parameter on `edit`, which also skips the flush itself. Both spellings still work; `skipFlush` now emits a `warnings[]` entry saying it means the narrower thing. Deferring the settle also turns off the readback, so issue the last call of a burst without it.

**Sculpting does not dirty the map package.** See *Gotchas* above — mark it, or a later save no-ops and the edit is lost.

### landscape.flush_grass

Invalidates and rebuilds the grass instances every landscape in the editor world built from one `ULandscapeGrassType`. Use it after a grass-type edit made **outside** this plugin — the asset editor, `python.execute`, an undo. Edits made through the plugin's own reflected mutators (`property.set`, `container.array.*`) already flush; see *Making a grass-type edit reach the screen* on the [`landscape`](landscape.md) page for the full defect and its measurement.

**The response measures the rebuild instead of asserting it.** `consumerRefresh` is read from each proxy's grass cache before and after: `consumersFound` counts landscapes that either hold cached grass from this type or whose material declares it, `consumersRefreshed` counts the ones whose cache demonstrably changed, `subObjectsRefreshed` counts the clusters, and `refreshed[]` names the landscapes so a caller can check the one it cares about rather than trusting a count. `measured: false` means there was no editor world to look at — never read the zeros beside it as a measurement.

**It drops instances, never density maps, and `grassMaps` proves it.** The verb flushes the built HISM clusters (`FlushGrassComponents(nullptr, bFlushGrassMaps=false)`) and leaves `ULandscapeComponent::GrassData` — the rasterised per-component density map the instances are built *from* — untouched, so the carpet comes back as the camera moves. `grassMaps` reports `componentsHoldingMapsBefore` and `componentsHoldingMapsAfter`, counted across the same consumers on both sides of the flush inside the call; equal is the contract. A `discarded` count plus a `warning` string appear **only** if maps were lost, which would be a defect in this verb rather than a cost of the edit — treat their presence, not their value, as the signal.

**`consumersFound: 0` is not a failure.** A grass type no loaded landscape material references has nothing to refresh, and the message says so rather than claiming a refresh.

**Scope it does not reach, named in `notRefreshed[]`:** play-in-editor worlds keep their own proxies and their own grass cache, and landscape proxies in unloaded World Partition cells rebuild their grass on load.

**`system.console_command {command: "grass.FlushCache"}` is not a stronger version of this verb.** It flushes every proxy in the process *and* deletes their grass density maps (it takes `bFlushGrassMaps`' default `true`), which in the editor is recovered only by the amortised camera-driven grass-map builder or a restart — measured at 922,280 → 56,080 instances on one level and 136 → 0 grass components on another. Use it only when you want that. `grass.FlushCacheAll` does not exist.
