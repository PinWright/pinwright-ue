# water

Spawn and configure UE Water plugin actors (`AWaterBodyRiver`, `AWaterBodyLake`, `AWaterBodyOcean`, `AWaterBodyCustom`, `AWaterZone`) including materials, underwater post-process, and river-transition materials, all routed through each body's `UWaterBodyComponent`.

Use this namespace for UE Water plugin actors; spline-point authoring on the body's `WaterSpline` belongs to the `spline` namespace and shoreline integration with terrain belongs to `landscape`.

## Plugin gating

These handlers compile-gate on `WaterBodyActor.h` being reachable (`#define MCP_HAS_WATER` via `__has_include`), with the `Water` module added through `TryAddConditionalModule` in `PinWright.Build.cs`. Registration is unconditional, so the `water.*` namespace appears in discovery even when the Water plugin is disabled — calls then return `WATER_PLUGIN_NOT_AVAILABLE` instead of `METHOD_NOT_FOUND`, so agents get an actionable signal.

## Canonical workflow

For river / lake / custom bodies, spawn the actor first, then chain `spline.set_spline_point_position` against the returned actor's `WaterSpline` component to author the shape, then set materials. There is intentionally no `splinePoints` parameter on `water.spawn_water_body` — spline editing belongs to the `spline.*` namespace.

1. `water.spawn_water_zone({ location, extent })` — at least one `AWaterZone` is required in the level for water rendering and underwater post-process to function.
2. `water.spawn_water_body({ type: "River", name, location, rotation })` — returns `{ actorName, actorPath, className }`.
3. For River/Lake/Custom, repeat `spline.set_spline_point_position` against the spawned actor's `WaterSpline` component to lay out the surface. For a river, set its **width** and **depth** here with `water.set_river_width_at_spline_point` / `water.set_river_depth_at_spline_point` — never through `spline.set_spline_point_scale` (see below).
4. `water.set_water_body_material({ actor, waterMaterial?, underwaterMaterial?, riverToLakeTransitionMaterial?, riverToOceanTransitionMaterial? })` — only the river-transition channels require an `AWaterBodyRiver` actor; passing them against a Lake/Ocean/Custom returns `INVALID_WATER_BODY_TYPE`.
5. `water.set_water_body_underwater_post_process({ actor, postProcessMaterial?, settings? })` — `settings` is a partial `FUnderwaterPostProcessSettings` field map, applied via property reflection. Only the four direct fields (`bEnabled`/`Priority`/`BlendRadius`/`BlendWeight`) live on that struct; color/tint/fog knobs are `FPostProcessSettings` members and must be nested under a `PostProcessSettings` object (see the per-method page). Unmatched keys are rejected with `INVALID_PARAMS` + `droppedSettings`, not silently dropped.

## A body added to an existing zone does not render until the level reloads

**A water body spawned into a level whose `AWaterZone` already exists renders nothing, and every call reports success.** The zone builds its quadtree and water-info texture from the bodies present when it is built; a body added afterwards is not in it. The surface is drawn by that quadtree, not by the body's own components, so the body is genuinely invisible while its spline, material and `WaterBodyComponent` all read back exactly as written.

Measured on UE 5.8: two `AWaterBodyLake` actors spawned into a level holding one pre-existing `WaterZone` and one `AWaterBodyRiver`. The river kept rendering; neither lake appeared. Their splines read back correct (32 points, all at the intended world Z, the second the exact negation of the first), `affects_landscape` false, the intended material instances bound. After `level.save` and an editor restart onto the same map, **both lakes rendered correctly with no other change**.

`AWaterBody::OnWaterBodyChanged` is **deprecated on 5.8** and does not rebuild the zone — calling it emits a `DeprecationWarning` and changes nothing on screen. `UWaterBodyComponent::UpdateWaterBody` does not either. There is no zone-rebuild verb in this namespace and no `MarkForRebuild` exposed to Python.

So, when adding a body to a level that already has a zone:

- expect no visible water until the level is reloaded — **a blank result is not evidence the body is wrong**; read the spline points back instead of trusting the render;
- `editor.open_level` on the already-open map does **not** reload it. Save, then restart the editor (`editor_restart`, or `editor_start` with `map`);
- creating the zone *after* the bodies avoids it entirely, which is why the canonical workflow above lists `spawn_water_zone` first.

## Two lake traps that cost a rebuild each

**`AWaterBodyLake` ignores per-point spline Z; the actor's Z is the water surface.** A lake is flat by definition. Setting every spline point to `(x, y, 195)` in world space against an actor at the origin silently produced a surface at **z 0**, with the points reading back at `z 0.0`. Place the actor at the surface height and author the spline around it.

**`get_actor_bounds` on a water body reports stale generated-mesh bounds.** Immediately after authoring, a lake whose spline correctly spanned x −6450..−1661 reported origin `(−3258, 10191, −375)`, extent `(3258, 10191, 375)` — a box stretching back to the world origin. The spline was right and the bounds were lying. Verify geometry from `spline.get_location_at_spline_point`, never from the actor bounds.

## Width and depth are metadata; the spline scale is derived from them

A river's width and depth live in `UWaterSplineMetadata` (`RiverWidth` / `Depth`). The water
spline's **point scale is derived from that metadata, not the other way round**:
`UWaterSplineComponent::SynchronizeWaterProperties` assigns `Scale.X = RiverWidth` and
`Scale.Y = Depth`, and `PostLoad` calls it (so does `PostDuplicate` and every
`PostEditChangeProperty`).

That makes a `Scale` write the most expensive kind of wrong: it **reads back** at the number
you asked for, it **saves** into the map package, and it is recomputed away on the next level
load. Only a genuine reload can see it — and `editor.open_level` on the already-open map does
not reload. One session set a river to 3490.6, verified the read-back, saved, committed, and
found 4800 on the next load.

`spline.set_spline_point_scale` now refuses on a water spline with `DERIVED_PROPERTY` and
names these verbs. Use them:

- `water.set_river_width_at_spline_point({ actor, width, pointIndex | allPoints })`
- `water.set_river_depth_at_spline_point({ actor, depth, pointIndex | allPoints })`

Both write the metadata, then run `K2_SynchronizeAndBroadcastDataChange` so the engine
re-derives the scale and rebuilds the body, and both report per point the `stored` metadata
value **and** the `derivedScale` the engine computed from it. `derivedScale` is written by the
engine, not by the handler, so a write that did not take cannot report success — a point that
disagrees returns `APPLY_FAILED` with the evidence.

River bodies only: the accessors live on `UWaterBodyRiverComponent`, and `RiverWidth` is
editable on rivers alone (`CanEditRiverWidth`). A lake or ocean returns
`INVALID_WATER_BODY_TYPE` rather than storing a value that controls nothing.

## Foam is behind a static switch a river never satisfies

Before tuning any water-material feature, check that its **gate** is satisfied — a feature behind a static switch, or one that needs data a given body type never produces, cannot be reached by moving its exposed parameters, however far you move them. Foam is the case that costs the most time: it sits behind `Enable Ocean Foam`, which is false on a river chain. Driven to extremes on one — Boost 20, Distance 2000, white emissive — shoreline luminance moved **±0.02 out of ~100**, i.e. nothing. A parameter that visibly does nothing is evidence about the gate, not about the value.

## Out of scope

Water mesh tessellation tuning, custom water body weightmaps, landscape water-brush integration, and Gerstner wave parameter authoring on `AWaterBodyOcean` are deliberately not exposed. Report the issue if you need them.

## See also

- [`level-building`](level-building.md) — terrain and water as one pass of the level-building guide.
- [`landscape`](landscape.md) — the heightfield terrain the water bodies cut into.

### water.set_water_body_underwater_post_process

Wires up the underwater post-process material and settings on the body's `UWaterBodyComponent`. `settings` is applied field-by-field via property reflection onto the component's `UnderwaterPostProcessSettings` (`FUnderwaterPostProcessSettings`).

That struct has only four **direct** fields:

- `bEnabled` (bool)
- `Priority` (float)
- `BlendRadius` (float)
- `BlendWeight` (float)

The underwater color / tint / fog / exposure / bloom knobs are **not** direct fields — they are members of `FPostProcessSettings`, which lives on the **nested** `PostProcessSettings` object. Pass them under a `PostProcessSettings` key:

```json
{
  "actor": "MyRiver",
  "settings": {
    "bEnabled": true,
    "BlendWeight": 0.9,
    "PostProcessSettings": {
      "bOverride_SceneColorTint": true,
      "SceneColorTint": { "r": 0.1, "g": 0.35, "b": 0.55, "a": 1 }
    }
  }
}
```

Passing the `FPostProcessSettings` names **flat** (e.g. `SceneColorTint`, `bOverride_SceneColorTint`, `FogDensity` at the top of `settings`) does **not** work — those keys do not resolve to a direct field. Any settings key that does not resolve is rejected before mutating anything: the whole call returns `INVALID_PARAMS` with a `droppedSettings` array listing each unresolved `{key, error}` (an apply failure surfaces the same way). The all-resolved path returns `{ applied: ["settings.<field>", ...] }`.

### water.set_river_width_at_spline_point

Sets the authoritative `UWaterSplineMetadata::RiverWidth` at one spline point (`pointIndex`)
or at every point (`allPoints: true`). Exactly one of the two selectors is required — neither
has a safe default, since defaulting to point 0 would silently edit one point of a river you
meant to widen end to end, and defaulting to every point would flatten a taper you meant to
keep.

`width` must be greater than 0: the engine floors the stored value at `KINDA_SMALL_NUMBER`,
so a non-positive request would produce a degenerate river rather than the value asked for.

Response:

```json
{
  "curve": "RiverWidth",
  "authoritativeState": "UWaterSplineMetadata::RiverWidth",
  "derivedProperty": "spline point Scale.X",
  "pointsTargeted": 1,
  "pointsApplied": 1,
  "applied": true,
  "points": [
    { "pointIndex": 0, "inputKey": 0.0, "requested": 3490.6,
      "stored": 3490.6, "derivedScale": 3490.6, "applied": true }
  ]
}
```

`stored` is the metadata read back; `derivedScale` is the spline point's `Scale.X` **after**
the engine re-derived it. They are two independent measurements of the same intent, and
`applied` is their conjunction with the request. A point where they disagree returns
`APPLY_FAILED` carrying the same `points[]` array, never a success.

`authoritativeState` / `derivedProperty` name what was written and what the engine computes
from it. There is deliberately no `survivesReload: true` field — the handler did not reload
anything, and a constant under a measurement name is the defect class this verb exists to
close.

### water.set_river_depth_at_spline_point

The `Depth` curve, same shape and same guarantees; `derivedScale` reads the spline point's
`Scale.Y`. River bodies only — `UWaterBodyRiverComponent` owns the depth accessors, so a lake
or ocean returns `INVALID_WATER_BODY_TYPE`.
