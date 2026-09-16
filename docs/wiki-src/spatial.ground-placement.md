# Ground placement

How to put many props on terrain so they read as bedded in rather than balanced or floating, and how to prove it happened. Three verbs: `spatial.ground_actors` (seat a batch of actors), `spatial.ground_instances` (seat the instances of an ISM/HISM scatter - `ground_actors` refuses a scatter holder with `HOLDER_NOT_SEATABLE`, so this is the only path for scattered content), and `spatial.verify_grounding` (measure a batch of actors, non-mutating).

## Why "raycast down and set Z" does not work

The obvious algorithm — one downward ray at the actor's pivot, set the actor's Z to the hit — fails four independent ways, and a level can hit all four.

**The ray hits the wrong thing.** Trees and haze cards are render geometry. With `traceComplex: false` a card that carries a simple box collider still blocks and becomes "the ground"; with `traceComplex: true` a collisionless tree blocks on its render triangles. In one measured run, a wave of characters ended up at Z 4299.6 because the ground reference resolved to a translucent fog card instead of the terrain. This is why `surface` is a **required** argument on both verbs — see below.

**A pivot is not the mesh's lowest point.** For an irregular rock the pivot is wherever the artist left it. Setting pivot Z to ground Z leaves the mesh floating or buried by an arbitrary amount.

**The bottom of a rock is a surface, not a plane.** One sample cannot seat a wide object on uneven ground. Model the underside as a flat plane at the bounds minimum and it touches at exactly one point and hangs everywhere else — which is what a "balanced boulder" is.

**Nothing checks the result.** A downward ray from the bounds bottom cannot report a negative gap, so an actor *sunk into* a surface reads as *no ground* — indistinguishable from an actor over a hole. A re-seat pass once reported success while 132 rocks hung in the air.

## What these verbs do instead

For each actor: sample an N×N grid of columns over the footprint (inset from the AABB edge, because an AABB corner over a rounded rock is empty air); at each column find the actor's own lowest geometry by tracing its components directly; at each column find the ground under the stated surface; then solve one Z offset from the whole set of per-column clearances, apply it, **re-measure**, and only report `placed: true` when the second measurement agrees with the solve.

Columns where the actor has no geometry impose no constraint and are excluded from coverage. Columns with geometry but no ground beneath drive `coverage` down — which is how an actor overhanging the terrain edge is caught.

## The footprint is the silhouette, and for a tree the silhouette is the canopy

The grid spans the object's world **bounding box**, and the response now says so: every `contact` block carries `footprintHalfExtentCm: {x, y}` — the XY half-extent that was actually sampled — and `footprintSource` (`bounds` or `contact_radius`). Read it. `coverage`, `contactPoints`, `maxGapCm`, `undersideReliefCm` and `pass` are all computed **over that box**, so when the box is not the object they are all green and all irrelevant.

For a rock, a slab, a log or a boulder the box *is* the contact patch — measured bounds/contact area ratios of 0.98–2.49× across four such meshes — and nothing below applies. For anything with a trunk, a stem or a pedestal it is not: measured from LOD0 vertices, one tree's bounds half-extent is 656.8 × 801.3 uu around a 109.7 × 102.8 uu contact patch (**46.7×** the area), and a dead-tree mesh measures **80.8×**. The grid then probes ground the trunk never touches, a first-contact solve lifts the instance to clear the highest of it, and the verb reports `coverage 1.00`, `pass true`, `undersideReliefCm 0` — truthfully, about the box. Measured on 177 already-correctly-planted instances: **177/177 proposed for a lift**, p50 **+196 cm**, max **+773 cm**.

**`spatial.ground_instances` takes `contactRadius` for this** — the radius of the contact patch in **mesh-local cm**, scaled per instance by that instance's mean XY scale. It replaces the grid's XY span and changes nothing else; the underside stays the bounds plane, which is what an instanced component can honestly offer. Get the number from the mesh: the horizontal extent of its lowest few percent of height, which is the root flare, the stem base or the pedestal footprint. `contactRadius: 100` is what the tree above wants.

**`footprintInset` is not this parameter, and raising it will not substitute.** It is a *fraction of the bounds*, so it re-derives from the world AABB per instance — and `FBox::TransformBy` returns the axis-aligned hull of the rotated box, so instance yaw alone moves it. It is clamped at `0.45`, which on the tree above still leaves 361 × 441 uu of sample half-extent around a 100 uu contact radius; driven to that ceiling with `seatPercentile: 0.5` and `embedFraction: 0`, the same 177 instances still measure p50 **+76.9 cm**, with 9 of 177 within 20 cm of a no-op.

`spatial.ground_actors` has no equivalent parameter: an actor has neither one mesh nor one scale for a mesh-local radius to mean the same thing, so it samples its bounds and its report says `footprintSource: "bounds"`. Its partial escape is `undersideModel: "mesh"`, which at least measures the real underside per column — instances have no such option, which is why this is a blocker there and a caveat here.

## The `surface` argument is required

Both verbs refuse to run without it. Three presets:

- `landscape` — only `LandscapeProxy` (covers `Landscape` and `LandscapeStreamingProxy`). **Use this for terrain.** A landscape heightfield is single-valued per column, so "the ground at this XY" is unambiguous and nothing can shadow it.
- `any_solid` — anything blocking except **foliage** and **effect geometry**. Use when props must sit on structures as well as terrain.
- `custom` — your `onlyClasses` / `excludeClasses` / `excludeComponentClasses` / `excludeNames` only.

**What "effect geometry" means, and why it is not a name pattern.** Haze, fog, glow and light-shaft cards are render geometry that often carries a simple box collider, so a downward probe treats one as ground — one measured run put a whole batch of characters ~4300 uu in the air. `any_solid` rejects them from what it is, not from what it is called: an actor is effect geometry when it owns at least one collision-enabled primitive and **every** such primitive is either an effect/marker component class (particle, Niagara, decal, billboard, arrow, text render) or draws only with non-opaque materials (any blend mode other than Opaque or Masked). One opaque collidable component is enough for the actor to count as real ground, so a building with a translucent window is never disqualified.

Set `excludeEffectGeometry: false` if your ground genuinely is translucent (a glass walkway). Set it `true` on a `landscape` or `custom` spec to borrow the protection. If your cards share a naming convention, `excludeNames` still takes wildcards — `{"preset":"any_solid","excludeNames":["FX_Haze_*"]}` — and adds to the preset rather than replacing it.

**What "foliage" means, and why it needs `excludeComponentClasses`.** `excludeClasses` names **actor** classes, and the level's shared `InstancedFoliageActor` is one — but a vegetation layer scattered as ISM/HISM components on an ordinary actor is not, and no actor class distinguishes that holder from anything else in the level. Only the component that **answered the probe** does, which is what `excludeComponentClasses` reads (same case-insensitive class-name/class-path substring rule as `excludeClasses`, matched up the component's ancestry). `any_solid` populates it with `FoliageInstancedStaticMeshComponent` and `GrassInstancedStaticMeshComponent` — the two engine classes that exist for vegetation and nothing else.

Plain `InstancedStaticMeshComponent` / `HierarchicalInstancedStaticMeshComponent` are deliberately **not** excluded: a scatter of paving stones, rocks, debris or modular tiles is legitimate ground, and a preset that rejected every instanced component would move every actor you had already seated on one. When yours is not ground, say so: `{"preset":"any_solid","excludeComponentClasses":["HierarchicalInstancedStaticMeshComponent"]}`. Caller entries add to the preset. A hit whose component cannot be resolved is never rejected on this axis — an unknown is not evidence.

This matters most on `spatial.ground_instances`, which seats instances against whatever surrounds them: a neighbouring scatter is nameable on no other axis.

`traceComplex` defaults `false`, which is right for terrain and for the `landscape` preset. It is **not** right on authored architecture: a simple hull can stand metres above the visible stone or sink below it, in either direction within one mesh, so a batch seated onto structures should probe complex and then be looked at. Both hazards, and which surface each applies to, are laid out under *Which trace, for which ground* on the [`spatial`](spatial.md) page. Terrain-probe examples elsewhere do pass `traceComplex: true` — they are safe only because they also pass `onlyClasses: ["LandscapeProxy"]`, which peels everything else off the ray. Complex tracing without a class or actor filter is the combination that returns a confident hit on a tree with no collision at all.

**Read `contact.groundProvenance` before you trust a seat on authored geometry.** `traceComplex: false` resolves against **simple** collision, so on a mesh whose hull is not its render surface the verb measures a floor the mesh does not have — and it looks perfect, because every gap number agrees with every other one precisely by reading the same wrong surface. `primitiveColumns` counts columns a **hull** answered (no `faceIndex`), `triangleColumns` counts columns a triangle mesh or heightfield answered, and `surfaceTrust` (`trusted` / `untrusted` / `undetermined`) is the verdict over **all** of it, with `warning` naming every fired condition and the surface it belongs to. A hull is only one of them: an instanced scatter (`instancedScatterColumns`), a collisionless component answering off render triangles (`renderGeometryColumns`) and a `UseComplexAsSimple` body answering a simple probe (`complexAsSimpleColumns`) each raise it too, and all three answer *with* a face index — which is exactly why a warning gated on its absence used to be silent on them. `undetermined` is not an all-clear: it means some column's primitive never resolved (`unclassifiedColumns`), so no statement about the surface was possible and none was made. Terrain is provably the safe case: a landscape heightfield returns a face index under either flag. Divergence needs no authoring mistake — UE scales an `FKSphereElem` radius by the **minimum** absolute scale component, so any non-uniformly scaled actor with a sphere hull has a hull narrower than its own render mesh. Provenance says *which* surface replied, never by how much or in which direction: sign is not constant even within one mesh, so there is no "take the higher hit" rule. For the magnitude, re-probe those columns with `spatial.raycast` at the opposite `traceComplex` and compare.

**`contact.groundProvenance.surfaceComponents` names what the ground actually was.** One row per distinct primitive that answered a supported column — `{actor, component, componentClass, columns}` — ordered by how much of the footprint each answered, with `surfaceComponentCount` carrying the true distinct total when the list is capped. The column counts beside it say which *representation* replied; only this says the representation belonged to a kelp scatter rather than to a cliff, which is the check `excludeComponentClasses` exists to act on: see a `componentClass` of `HierarchicalInstancedStaticMeshComponent` where you expected `LandscapeHeightfieldCollisionComponent`, and the surface spec, not the seat, is what needs fixing. A footprint may straddle several surfaces, so this is a distribution and never one label, and the trust flags travel per row (`instancedScatter`, `renderGeometry`, `complexAsSimple`, each present only when true) rather than collapsing into one verdict for the batch. Columns whose component could not be resolved contribute no row and are counted in `unclassifiedColumns`.

When a probe finds nothing, the response says which of the three reasons applies: `GROUND_HITS_ALL_REJECTED` (geometry was hit and the filter refused it — `rejectedSurfaceActors` names it), `GROUND_NOT_FOUND` with `overLandscape: false` (not over the terrain at all), or `GROUND_NOT_FOUND` plain.

## Choosing seatPercentile and embed

`seatPercentile` picks which sampled column the actor comes to rest against:

- `0` (default) — rest on the **first contact**, i.e. the highest ground under the footprint. Nothing is buried; on uneven ground exactly one column touches. This is physically what resting means, and visually it is the balanced look.
- `1` — sink until **no column floats**, i.e. the lowest ground under the footprint. Nothing hangs in the air, at the cost of burying the high side.
- `0.5` — median.

`embedFraction` (default `0.02`, of the actor's bounds height) then sinks the actor further so it beds in. This is deliberate and defaults non-zero: an object tangent to terrain reads as a physics glitch. `embedDepth` adds absolute centimetres on top.

For scattered rocks on terrain, `seatPercentile: 0` with the default embed is usually right. For large flat-bottomed props on rolling ground, raise the percentile so the far side does not hang.

## When the bounds footprint is not the contact footprint

Everything above assumes an object's footprint is roughly where it touches the ground. For a rock, a slab, a log or a boulder that holds. For anything with a **trunk** — a tree, a mast, a signpost, a lamp column, a bollard — it does not, and the seat solve then fails in a way that reports `pass: true`.

Measure the ratio before you seat: the XY extent of geometry in the lowest few percent of the mesh's height, against the bounds XY extent. Measured on one foliage set (half-extents in cm):

| mesh | bounds XY | contact XY | bounds/contact **area** |
|---|---|---|---|
| oak, 19 m tall | 656.8 x 801.3 | 109.7 x 102.8 | **46.7x** |
| dead snag | 42.5 x 42.0 | 4.7 x 4.7 | **80.8x** |
| flat rock slab | 278.3 x 111.5 | 278.4 x 114.2 | 0.98x |
| fallen log | 237.0 x 49.5 | 231.4 x 44.3 | 1.14x |
| boulder | 4165.0 x 3586.2 | 4232.1 x 2731.7 | 1.29x |
| small rock | 21.3 x 21.3 | 11.9 x 15.3 | 2.49x |

Anything near 1 is fine. Anything in the tens is a trunk, and the grid is sampling the canopy.

**What goes wrong.** That oak's footprint is 13.1 x 16.0 m — the crown. `seatPercentile: 0` rests the object on the **highest** ground under the footprint, so on a slope the solve lifts the whole tree until its underside plane clears the uphill hillside, and the trunk hangs in the air. On 177 already-correctly-placed instances the seat at its defaults proposed a lift for **every one of them**: median **+196 cm**, p90 **+565 cm**, worst **+773 cm** — each row reporting `coverage: 1.00`, `contactPoints >= 4`, `pass: true`.

**`samples: 1` is not the fix**, though it looks like one. The single column lands at the centre of the instance's **world-axis-aligned** bounding box, which is neither the pivot nor the trunk when the mesh's bounds origin is off-centre or the instance is yawed. Measured on the same 177: pivot-to-column distance median **152.7 cm**, max **324.6 cm**, against a 100 cm root flare; the ground height at that column differs from the ground at the pivot by rms **39.2 cm**, max **172.7 cm**. It also collapses the column count to 1, which makes `coverage` **1.000 by construction** and `groundSpreadCm` **0** for every instance — every coverage and spread check becomes vacuous, the same degenerate row a one-point rest produces. Do not reach for it to aim the probe at the trunk; it does not aim anywhere in particular.

**No parameter combination recovers it.** `footprintInset` is clamped at 0.45, so the tightest grid the verb can build over that oak still spans 361 x 441 uu around a 100 uu contact radius. The most favourable settings measured — `samples: 3, footprintInset: 0.45, seatPercentile: 0.5, embedFraction: 0` — still proposed a median **+77 cm** lift, and only 9 of 177 landed within 20 cm of a no-op.

**Seat a trunk on a trunk-sized ring instead.** Probe the ground yourself on a ring at the contact radius and write the transforms with `actor.set_instance_transforms` (or `actor.set_transform` for a non-instanced prop):

    r     = contactRadius * meanXYScale        # from the mesh's low geometry, not from the bounds
    ring  = ground Z at 8-16 azimuths on radius r, plus the pivot
    newZ  = min(ring) - meshBottomOffset * scaleZ - embed

`min(ring)` is `seatPercentile: 1` evaluated over the **contact** footprint. On a planar slope of angle t it equals `groundZ(pivot) - r*tan(t)`, so this is the closed form of "sink a trunk by `r*tan(t)`". The uphill side of the flare then buries by `2*r*tan(t)`; on 22-39 degree ground that measured 50-198 cm, which reads as a trunk entering the hillside. Daylight under a trunk does not. Applied to 242 trunked instances on one map, the share of trunks with measurable daylight underneath went from 27.9% (worst +252 cm) to 2.3% (worst +1.9 cm).

**Make the embed absolute.** `embedFraction` is a fraction of the instance's world-AABB **height**, and that box is rotation-inflated: a scale-1.48 tree pitched 1.9 degrees measures 2898.7 uu tall against 2805.1 for `meshHeight * scale`. The default `0.02` is therefore **58 cm** on a 19 m tree and **1 cm** on a 50 cm plant, which is why one setting reads as "beds in nicely" on groundcover and as "intermittently masks a float" on canopy. Scale the embed to the contact radius (about `0.1*r`) or pass `embedDepth` and set `embedFraction: 0`.

## The underside plane is the rotated AABB minimum, not the mesh

`spatial.ground_instances` models an instance's underside as a **flat plane at its world-AABB minimum**, and takes no `undersideModel` (passing one is `UNKNOWN_PARAMS`). For a rotated compact prop that plane sits **below** the mesh's real lowest point, by an amount that grows with tilt — so the solve believes the object is lower than it is and, at a low `seatPercentile`, **lifts an object that was already bedded**.

This is not a corner case. On 466 tumbled rocks (pitch and roll up to +-40 degrees) a modest `seatPercentile: 0.4` proposed a **lift** on 245 of 256 measured instances, median **+3.9 cm**, while the same population measured as floating. At `0.9` the same call proposed a sink on 247 of 256, median **-11.4 cm**, and the measured float fell from 76% of the population to 55%. **On instances, treat `seatPercentile` 0.85-1.0 as the working range and anything below about 0.75 as a lift risk**, and dry-run (`apply: false`) every component before applying: the right percentile is a property of the mesh's rotation spread, not of the class of object.

Two acceptance rules make this safe to run in bulk. Both are cheap, and both caught real damage:

- **Never lift.** Take the dry run, keep only the instances whose `proposedDeltaZCm` is negative, and pass those in `indices`. A component whose whole population is already bedded should be skipped entirely, not re-seated.
- **Never over-bury.** An instance that was not floating and ends up far deeper than the requested embed has been damaged, not fixed. Measured: `seatPercentile: 1` on a set of fallen logs pushed one from 1.3 m below the surface to 4.0 m below it, and nothing in the response said so — every row read `placed: true`.

**Judge that second rule on the right number.** `min(clearance)` over the underside samples answers “does the whole object float” and is dominated by the object's *deepest* point, so a slab with one end in a hillside reads as buried while its other end still hangs in the air. Applying the over-bury rule to that number reverted nine correctly-sunk slabs, and a ground-level capture showed the reverted pose was the worse one — the undercut it was supposed to fix. Report both: `min` for whole-object float and `max(pointZ − groundZ)` for the tallest visible void. The `max` figure sums the mesh's own underside relief and the float, exactly as `maxColumnClearanceCm` does, so its absolute value is meaningless on a knobbly mesh; a Z-only seat leaves rotation unchanged, so the shape term is constant and the **change** between before and after is pure float. Read the delta, never the value.

## Verifying

`spatial.verify_grounding` measures the same way and reports, per actor: `coverage`, `contactPoints`, `maxGapCm`, `minGapCm`, `penetrationCm` (how deep the deepest part is buried), `groundSpreadCm`, and a derived `pass`. It sees the two things a single-ray check cannot — a sunk actor and a one-point rest.

**`maxGapCm` is the float, not the silhouette.** A column's clearance is the sum of two independent terms: the actor's own underside **relief** (how far that column's underside sits above the actor's lowest point — pure mesh shape) and the **float** (how far the actor's lowest point sits above the ground). Only the second says anything about placement. `maxGapCm` is the float term at its worst — the lowest underside sample minus the lowest accepted ground under the footprint — and it is the only one of the three that can fail an actor. The old silhouette figure is still reported as `maxColumnClearanceCm`, with `undersideReliefCm` beside it saying how much of it is shape. A capital wider than its base, a cylinder on its flank, an arch, a rounded figure: all have a large `maxColumnClearanceCm` however well they are bedded, and none of them fails for it. `minGapCm <= maxGapCm <= maxColumnClearanceCm` always, so nothing that passed before can start failing.

Verify with the **same** `surface` and the same `samples` / `undersideModel` you seated with, or the two are answering different questions. When verifying deliberately embedded actors, raise `maxPenetration` to at least the embed depth or every one of them fails.

**That instruction cannot be followed for instances.** `spatial.ground_instances` has no `undersideModel` parameter — passing one is `UNKNOWN_PARAMS` — and always uses `bounds_plane`, because an instanced component's `LineTraceComponent` answers from every instance body at once and cannot attribute a hit to one of them. `spatial.verify_grounding` defaults to `mesh`, so a default-for-default verify of an instance seat compares two different underside models. Verify an instance seat with the verb's own `apply: false` dry run, and read *The underside plane is the rotated AABB minimum, not the mesh* above before trusting its `undersideReliefCm` of `0`.

## Verifying an instance seat

The flat-plane model is also why an instance seat cannot be verified the way an actor seat is. `spatial.verify_grounding` takes actors; `ground_instances`' own `apply: false` dry run is its verify half, and it answers with the same `bounds_plane` underside it seats with, so `undersideReliefCm` comes back `0` by construction rather than by measurement.

For anything long, flat or heavily rotated, measure the result **against the mesh**, outside the verb: bucket the mesh's LOD0 vertices into a small local-XY grid, keep the lowest vertex per cell, transform those points by each instance's full transform, and take `min(pointZ - groundZ(pointXY))`. That is the real daylight figure. It is worth the effort, because the obvious cheap proxy — a ring of ground probes at some radius around the pivot — **over-reports float for anything elongated**: the ring samples ground metres to the side of a 4.7 m log or a 5.5 m slab, and on a slope that ground is lower. Measured on one set of logs, the ring metric called 81% of them floating and the mesh-underside metric called 26% — and the instances the ring flagged were the ones a "fix" then buried.

## Batch, paging and cost

Both verbs are batch-only by construction: a per-actor Python loop over a level is what wedged an editor for 168 minutes with 5100 uncancellable calls. Select with exactly one of `actors` / `prefix` / `filter` / `selection`; an empty match set is an **error** (`NO_ACTORS_MATCHED`), not a zero-item success, because a typo in a prefix otherwise looks identical to a clean run.

`limit` (default 512, max 5000) and `offset` page a larger set; `totalMatches` and `truncated` always report the truth. Cost per actor is roughly `samples²` columns × (one underside probe + one layered ground trace), paid up to three times by a seat (pre-measure, solve, readback). Lower `samples` to 1 only if you want the old single-point behaviour back.

`detail` controls response size: `summary` (counts only), `failures` (default — a full row per failed actor), `all`. Row arrays are capped at 256; the counts never are.

## Suggested workflow

1. `spatial.verify_grounding` over the selector with the surface you intend, `detail: "failures"`. Two things to read, not one: the failure reasons, **and `totalMatches` — confirmation that the selector names only your actors**. In a shared level that second reading is the load-bearing one, and `spatial.ground_actors` will refuse a `prefix`/`filter` until you pass that count as `expectedMatches`.
2. `spatial.ground_actors` with the same surface. Read `placed` / `failed` / `moved` — `placed + failed == requested` always.
3. Re-run `spatial.verify_grounding` with `maxPenetration` raised past the embed depth. Anything still failing has a real reason attached to it.

Nothing here reports a placement it did not make: `placed` per actor is a transform that the code which moved the actor recorded, ANDed with a post-move re-measurement that passed.

## See also

- [`spatial`](spatial.md) — the raycast/measure verbs beneath these, and the `traceComplex` warning.
- [`level-building.terrain-and-water`](level-building.terrain-and-water.md) — shaping the terrain these verbs seat onto.
