# vegetation-authoring

How to plant a level's vegetation so it reads as a place rather than as a grid of meshes. Pick the
placement pathway on purpose, break the lattice, seat trunks on ground they actually touch, and
verify against a renderer that will delete your work without saying so. Pairs with
[`spatial.ground-placement`](spatial.ground-placement.md), which owns the seat solve this page only
summarises; with [`level-building.instancing-and-scatter`](level-building.instancing-and-scatter.md)
for the instancing recipes; and with [`level-review`](level-review.md) for the capture discipline
assumed throughout.

## Pick The Pathway First — Five Exist And None Converts Into Another

**Landscape grass output** — a grass-type asset referenced by a `LandscapeGrassOutput` node in the
landscape material, driven by a paint layer's weight. The only turnkey ground carpet: no
per-instance authoring, follows the weightmap, culls itself, nearly free per instance. It is also
the only pathway with **no addressable instances** — you cannot move, remove or query one blade.
The ground plane, and nothing else.

**`foliage.*` instancing** — `foliage.add_type`, then `foliage.add_instances` with transforms you
computed. The only pathway that takes the rotation and scale *you* chose, so it is the one to reach
for whenever the look matters. `foliage.paint` is not a substitute: it writes `FRotator::ZeroRotator`
and unit scale on every instance **regardless of the foliage type's `AlignToNormal` / `RandomYaw`**,
which leaves it usable for uniform fill only. Nothing here is transactional — plan around
`level.save`, not Ctrl+Z — and every instance lands in the level's one shared `InstancedFoliageActor`
(see *Working in a shared level*).

**Procedural foliage spawner** — a volume plus a spawner asset, simulated by the engine: seeds
compete for space by radius, age and shade. Use it for emergent density rather than authored
positions, and when the instance count makes transcription the binding cost — the simulation keeps
its data server-side, which is how one zone reached 10,296 groundcover instances the authored
pathway could not have afforded. Its output is a *distribution*, not a placement: you tune radii,
priority and age and take what the simulation gives you, and several of the verb's own arguments
write properties the simulation never reads.

**PCG graph scatter** — rule-driven placement derived from the landscape (slope, height, aspect,
noise) that regenerates when the terrain moves. Right when placement must be a *function* of the
terrain and stay one. Generation is cheap: a 44-node graph over 37,000 m² measured 282 ms cold and
18-31 ms warm, so the earlier warning that an unfocused editor stretches it into minutes is
withdrawn — it did not reproduce. What it gets wrong quietly: landscape-sampled points carry the
**terrain normal as their Z axis and a terrain-derived yaw**, so trees lean with the slope at a
rotation the hillside chose; a weighted mesh selector **partitions a fixed point set**, so
`instanceCount` cannot see a species change at all; and a mesh spawner has **no per-entry scale**,
so meshes of different heights in one band silently mis-scale.

**`spatial.scatter_layout`** — the only verb that *produces* placements, and a pure function: no
world, no trace, nothing spawned. Deterministic, tileable, anchored at the world origin so two
overlapping regions agree on their intersection. The point source under any authored scatter. It
cannot trace (its Z is the region plane — always chain a ground verb), cannot tilt (pitch and roll
are always zero, no knob), cannot hold a spacing floor once you superimpose calls, and cannot rotate
a lattice. It has no server-side handoff either: transforms travel through the caller both ways,
roughly 130 KB each way for 2,600 instances, which is the practical ceiling on this pathway.

**How to pick.** Ground carpet over terrain: landscape grass. Anything whose individual placement
you care about: `scatter_layout` into `foliage.add_instances`, seated afterwards. Density that
should emerge from competition rather than from a rule you wrote: the procedural spawner. Placement
that must track terrain across future edits: PCG. Uniform fill where rotation and scale genuinely do
not matter: `foliage.paint`. Mixing pathways across a level is normal; mixing them within one tier
is what makes a level impossible to re-derive.

Each pathway's own trap catalogue — the grass cull ring and its twin `*Quality` slots, the spawner
arguments that write properties the simulation never reads, the PCG settings sub-object, the
`scatter_layout` limits above in detail — is on
[`vegetation-authoring.pathways`](vegetation-authoring.pathways.md). Read your pathway's section
before committing a level to it.

## UE 5.8's Procedural Vegetation Editor Is Authorable, Not Usable

A PV graph can be built over the wire — the classes register, nodes can be added, pins connected —
and it will still never produce an asset. Three structural reasons, all engine-side:

- **The Export node does no I/O.** Its element validates its settings and copies input to output;
  that is the whole implementation, and its output pin is invisible. Running the graph through
  `pcg.generate` writes nothing and the default output folder is never created.
- **The writer lives in the editor toolkit and reads a cache no level actor fills.** Export runs
  from the PV editor window against that window's own inspection cache, which is fed by a default
  execution source rather than by a level component, and then blocks on a modal dialog.
- **The graph you can build is not safely editable.** Every PV node's `In` pin is
  single-connection, and `pcg.connect_pins` returns a hardcoded `connected: true` while silently
  replacing whatever was on that pin. The edge *count* does not change, so diff the edge **set**
  after every connect if you author one anyway.

Separately, `pcg.add_node` gates only on the settings base class with no abstract-class check, so
abstract classes are reachable over the wire; in an editor build that path logs an ensure, allocates
the object anyway and nulls it out on save. An earlier note calling this editor-fatal was wrong —
that assert is in the non-editor branch — but it is a defect report, not a supported call.

## Nothing You Placed Is Visible Until Scalability Says So

**Check the scalability group before judging any density, and again after any editor restart.** It
comes first because it invalidates every judgement downstream: the placement verbs report success
with a full instance count, the foliage actor holds every instance, `get_instances` agrees — and the
renderer draws a fraction of them, or none.

The engine's `BaseScalability.ini` defines `FoliageQuality@0` as `foliage.DensityScale = 0` and
`grass.DensityScale = 0`, and `@1` as `0.4` for both. A project shipping no
`Config/DefaultScalability.ini` inherits that unopposed, and the machine-global editor settings file
deciding the starting group is shared by every UE project on the box — one editor came up at
`FoliageQuality 1` every launch.

**The two halves do not scale alike, and an earlier reading of this was wrong.**
`grass.DensityScale` multiplies landscape grass unconditionally. `foliage.DensityScale` applies only
to a `UFoliageType` that opted in through `bEnableDensityScaling`, which defaults **false** and
which auto-created types never set. At `@0` a grass carpet therefore vanishes entirely while an
ordinary foliage scatter is untouched; the claim that both halves scale — "60% of everything you
planted is missing at `@1`" — is false on the foliage half. It cuts both ways: a bare-looking carpet
may only be scaled away, and a foliage layer that looks right at `@1` is not evidence the setting is
harmless.

**Never set `sg.*` through a console command.** A console write lands at `SetByConsole`; the
Scalability panel writes at `SetByScalability`, which is **lower priority**, so every subsequent
click on that group is silently discarded until restart. One session froze seven groups this way
while the five untouched ones still responded — which reads exactly like an editor resetting itself.
Use `performance.set_scalability`, which drives the groups through the engine's own path and reads
them back, so a success cannot mean "nothing changed". For a durable setting, write the config file
with the editor closed.

**A cvar read-back does exist**, contrary to an earlier note: `system.console.search` with
`kind: "variable"` returns `currentValue` per row. `system.console_command` is write-only and its
`success: true` means only that the line was consumed, so set with one verb and confirm with the
other.

## Lay Out A Scatter That Does Not Read As A Grid

**The default jitter does not hide the lattice, and that is geometry rather than taste.** Jitter is
capped per axis at `jitter * spacing`, while hex row pitch is `spacing * sqrt(3)/2`, so rows stay
separated by a corridor no point can enter whenever

    jitter < sqrt(3)/4 = 0.4330

which is **86.6% of the legal range, including the 0.18 default**. Measured there on spacing 1000: a
**506 cm empty corridor**, only 41.6% of the ground in Y able to hold a point, and a
nearest-neighbour bearing chi-square of **1362** against a 99% critical value of 24.7. Raising
jitter to its 0.5 ceiling dissolves the lattice and takes the spacing floor with it — 38.9% of
points then land inside half the nominal spacing. **No single setting gives both.**

**Canopies bridge the corridor; trunks do not.** From overhead a tree scatter at the default looks
fine, and at eye level the trunks are in rows. Anything with a footprint smaller than the corridor —
rocks, bushes, grass clumps, debris — reads as rows from every angle.

**Superimpose several lattices, and pay for it deliberately.** Calling `scatter_layout` repeatedly
over one region at mutually incommensurate spacings and different seeds, then merging, removes the
row signature: measured, one lattice scored chi-square 888 normalised to N = 1000, and a 13-lattice
composition with a density mask and a radius cull scored **7.0**. The cost is the spacing floor — **50.2% of merged points sat
inside half the merged effective spacing, worse than `jitter: 0.5`'s 38.9%**, because independent
lattices have nothing stopping two points coinciding. An earlier version of this advice recommended
superposition without that caveat and was wrong to. Add a **radius-aware minimum-distance cull**
afterwards, and expect it to trade against isotropy.

**Superposition cannot decorrelate orientation.** Every call rows along the same world axes and
there is no rotation parameter, so merging shifts row *phase*, never row *direction*. Rotate each
lattice about the region centre yourself before merging — 0 / 23 / -41 / 11 degrees took one
four-lattice merge from chi²@1k 54 to 38.6.

**A density mask is what turns an even sprinkle into composition.** A ridged noise field multiplied
by slope, with nothing placed below a threshold, concentrates a scatter into gully lines and leaves
genuine bare ground — which is what makes debris read as having moved downhill. Carve clearings with
the verb's own `exclude` regions rather than by deleting instances, so the carve survives a
re-scatter, and carve a **different radius per tier** (largest for canopy, smallest for understory)
so a clearing gets a shrub rim instead of a hard edge.

The method behind every number above, the full superposition and cull tables, and the two parameters
that would remove all of this hand work are on
[`vegetation-authoring.scatter-geometry`](vegetation-authoring.scatter-geometry.md).

## Seat It, And Know Which Objects The Seat Solve Cannot Express

Full treatment on [`spatial.ground-placement`](spatial.ground-placement.md); three rules decide what
a vegetation pass does at the workflow level.

**Measure bounds-versus-contact footprint before seating anything.** The seat solve samples a grid
over the instance's bounds and models its underside as a flat plane at the world-AABB minimum. For a
rock, slab, log or boulder that is fair — measured ratios of bounds area to contact area of 0.98x,
1.14x, 1.29x, 2.49x. For anything with a **trunk** it is not: 46.7x on a 19 m oak, 80.8x on a dead
snag. The grid is then sampling the canopy, and on a slope the solve lifts the whole tree until its
underside plane clears the uphill hillside — on 177 already-correct instances it proposed a lift for
**every one**, median +196 cm, each row reporting `coverage: 1.00` and `pass: true`. **`samples: 1`
is not the fix**, though an earlier field note said it was: the single column lands at the world-AABB
centre, which is neither the pivot nor the trunk, and it makes `coverage` 1.000 by construction.
Compute a trunk seat offline against a ring at the contact radius and write it with
`actor.set_instance_transforms`; one map's trunks went from 27.9% with measurable daylight
underneath (worst +252 cm) to 2.3% (worst +1.9 cm).

**Flat slabs and tumbled rocks need a real underside, not a plane at the AABB minimum.** An
open-bottomed slab cannot be hit by an upward probe at all, so the solve falls back to the plane —
place those only on ground flat enough for a plane to be fair. A rotated compact prop's AABB minimum
sits *below* its real lowest point, so a low `seatPercentile` **lifts** something already bedded;
dry-run every component and keep only the instances whose proposed delta is negative.

**The seat verb cannot tilt an instance.** `spatial.ground_instances` has no `alignToSurface` /
`maxTilt` and seats in Z only, and `scatter_layout` emits pitch and roll of zero. Any lean has to be
baked into the rotations handed to `foliage.add_instances`, with no recovery downstream.

## Make It Look Like A Place

**Lean, and lean selectively.** Bake a small random lean into every tier — 1-5 degrees on mature
trunks, 3-10 on snags, up to 22 on bedded slabs and boulders. **Do not align trunks to the ground
normal:** a tree leaning with a 34-degree slope reads as damaged, not as planted. Ground-normal
alignment is correct for low spreading forms (mats, ferns, groundcover) and for rocks and slabs,
where following the ground is what bedding *means*. For those, derive pitch and roll from the slope
angle and downhill azimuth and add a species-sized random jumble on top — +-30 degrees on rocks,
+-6 on groundcover, on one measured set.

**Scale variance is per tier, not per level.** 0.7-1.4 is a floor for anything organic, and one
species in two well-separated bands reads as two tiers — 0.42-0.72 as mid-story against 0.85-1.35 as
mature canopy, off the same mesh. That is the cheapest variety available when a project owns one
good tree. Yaw uniform 0-360 except where the mesh has a front.

**When you swap a species, preserve world height:** `new = old * (oldMeshHeight / newMeshHeight)`.
Skipping it is how a band authored for 1300 cm trees shipped 157 cm snags as 1 m twigs. And a
component re-point does **not** re-seat: each mesh hangs a different distance below its own origin,
so a swap that preserves position floats or buries the replacement by the difference — up to 83 cm
on one set. Every re-point needs a seat pass after it.

**Cluster; do not distribute.** Density falling off from centres with genuine gaps between them
reads as a place; an even sprinkle at the same instance count reads as a texture. Vary the species
*ratio* across a region rather than sprinkling every species uniformly, and put understory at trunk
bases — in a procedural simulation that clustering is free if the species may both grow in shade and
spawn in it.

**Silhouette contrast beats species count.** Two silhouettes in the canopy tier — the live form plus
a bare dead one at 10-20% of the tree count — buy more apparent variety than a second leafy species.
Below about five distinct meshes an understory layer reads as cloned, and scale bands do not rescue
it, because the silhouette repeats. Canopy monoculture is invisible at eye level, where terrain,
light and density carry the variety, and unmissable in a wide shot: one map at ~51,000 instances
still read as one tree repeated in every establishing frame. When a project owns one tree that is an
asset gap, and saying so is more useful than another placement pass.

**Judge a species by placing it and photographing it — never from an asset preview or a distant
shot.** Three species in one build were chosen off a 150 m frame and cut after one close capture,
each reading as a missing material at 4-12 m. A stylised low-poly palette can be entirely correct at
distance and at low density and still be a blockout at eye level; that makes it a distance palette
rather than a bad asset, and mixing the two registers shows as a seam down the middle of every wide
frame. `render.capture_asset_preview` is specifically not a judge here: measured foliage subjects
came back correctly lit with the mesh pure black, and nothing in the response distinguishes that
from a genuinely dark asset.

**Backlit understory going black is a shading-model problem, and it is not a one-property fix.** A
leaf material on a plain lit shading model has no transmission, so a backlit leaf renders black
beside a two-sided-foliage neighbour that glows. Switching the model looks like the whole answer and
is not: on one master the change produced flat untextured white plants, and wiring the missing
subsurface input moved the frame by 0.0003 — the engine had substituted its default material, most
consistent with a Nanite material audit failing silently. That attempt was reverted. Where a project
already owns a Nanite tree that does not go black, copy its approach — faking transmission inside a
material-attributes graph on the default lit model — rather than changing a shared master's shading
model.

## Verify It — Nearly Every Failure Here Is A Verification Failure

**Judge foliage at eye height and at ankle height.** A top-down view hides floating trunks entirely,
and a landscape material's own fractal noise reads as a mottled green carpet from above — one
material's noise blobs measured ~1660 cm across and were repeatedly mistaken for grass. The ankle
shot is the seating check: every stem meeting the ground, contact shadows on the terrain. No
overhead frame can make it.

**Landscape grass lies in three independent ways, and any one will make a working setup look
broken.**

- **A capture that only passes a pose photographs grass built around the *persistent* viewport
  camera.** Grass builds asynchronously around the camera the streaming manager saw on tick, so the
  frame shows whatever was built for wherever the viewport was left. Measured at one pose: 0.6443
  mean luminance and 844 KB with no grass, twice running; after moving the viewport to that same
  pose first, 0.4796 and 1558 KB with a full carpet. Always `editor.set_camera` first.
- **Orthographic capture renders no landscape grass at all.** At matched world coverage,
  perspective 0.4797 against ortho 0.5161 — bare material plus specks. Not the lit-ortho near-plane
  pushback: an unlit repeat with zero pushback still shows none. Ortho tiling therefore **cannot
  verify grass**, and a top-down ortho shows only the material's own noise.
- **Game view suppresses part of it** — 0.4013 on against 0.3769 off at one pose, i.e. more grass in
  the off frame. Game view is still the right way to drop the chrome `hideEditorSprites` does not
  cover (volume wireframes, grid, world-axis gizmo), so take the chrome-free shot for the reviewer
  and an off-game-view shot for the density judgement, and never compare one against the other.

**Grass also takes 60-150 s to rebuild after a camera move while every health signal reads clean.**
`grass.MaxCreatePerFrame` is **1**, so repopulating a large cull radius costs hundreds of frames. A
capture taken before it finishes returns `blank: false` and `warmup.settled: true` with
`settleRounds: 1` and no grass in it — and a repeat at the same pose 12 s later is *identical*,
which reads as settled and is not. Measured: 0.3074 with no grass, 0.2778 after 45 s, 0.2746 after
105 s, against 0.2510 for the built carpet. A before/after pair split across that rebuild shows a
change nobody made, larger than the edit under test. Capture repeatedly at the fixed pose until
`meanLuminance` **and** `sizeBytes` agree across two consecutive frames — `sizeBytes` is the more
sensitive — and compare a pair only when both members are built. Full form on [`render`](render.md)
§ *A frame can be contaminated by state this capture does not own*.

**Pin exposure before comparing any two frames, and hold the capture size fixed across the session.**
Both are [`level-review`](level-review.md) rules and both bite hardest here, because a vegetation
edit moves mean luminance by less than auto-exposure does. In a shared editor, read the pose back
**after** the capture as well as setting it before: another caller's capture moves the same
viewport, and three profiling samples were silently taken at someone else's pose before that was
caught.

**Verify a grass-type edit by rendering it, never by reading it back.** Writing `GrassVarieties` and
saving does not reach the renderer — proxies keep their cached grass components, and one edit moved
a fixed pose by 0.0005, the noise floor. The read-back cannot catch it either: the varieties array
comes back from Python as **copies**, so per-element writes report success, the save returns true,
the `.uasset` mtime moves, and the stored value is unchanged.

**Read the seat provenance, and page it.** A clean seat's `groundProvenance` is absent from
`detail: "summary"` and from `detail: "failures"`, so proving you seated on terrain rather than on a
neighbouring scatter needs a second pass at `detail: "all"` — where `results[]` is capped at 256
rows. On a 324-instance component the response looks complete while covering 256 of them, so page it
with `offset` or two thirds of a large scatter goes unchecked.

## Success Is A Claim About The Call, Not About The Level

A family of failures where the verb reports success, the read-back agrees, and the level does not
change. Three of the cases this workflow was written from have since been fixed; they stay here
because the *check* survives the fix and the shape recurs.

- **The ledger and the component are two separate records.** Foliage instances live both in the
  foliage actor's bookkeeping array and in the component that draws them, and the engine asserts
  they agree only under a define that ships off. A removal that empties one leaves the level drawing
  exactly the same picture — and the obvious verification verb reads the *same* record, so it
  corroborates the lie. That was `foliage.remove`'s behaviour, measured three times on one build:
  six scoped calls, exact counts returned, pixel-identical frames afterwards. Fixed;
  `foliage.get_instances` now publishes `count`, `renderedInstanceCount` and a
  `ledgerMatchesRendered` verdict. **Read the rendered count, not the ledger count.**
- **A property write through an object hop.** When a dotted path crosses an object property the
  write lands in the inner object while the change notification goes to the outer one, so the
  override that recomputes derived state never runs: applied true, dirty true, `property.get` reads
  the new value, behaviour unchanged. Measured as a graph edit that moved an output count from
  26,664 to 2,093 only when addressed to the innermost object. Fixed — the notification now follows
  the write — and the rule stands regardless: **verify a property write against behaviour, never
  against `property.get`.**
- **A volume with no brush.** An `AVolume`'s shape lives in a `UModel` brush, so spawning the actor
  and writing the requested size as an actor *scale* yields a zero-extent volume that samples
  nothing while reporting success; four such calls produced four empty simulations. Fixed in both
  spawn paths. Check `actor.get_bounding_box` for a non-zero extent after creating any volume.
- **`grass.FlushCache` — recommended earlier in this session's own notes, and retracted.** It does
  push a grass-type edit to the renderer, and it is destructive: one level went from 922,280 grass
  instances to 56,080 and never recovered — not through camera moves, not through toggles, not after
  four minutes idle. Only an editor restart brought it back. Use `grass.Enable 0` then
  `grass.Enable 1`, which drops and rebuilds the components with the edited settings live in ~45 s.

Still live, each a silent wrong answer rather than an error: `foliage.paint`'s zero rotator (above);
`foliage.create_procedural`'s `minScale`/`maxScale`, which write the paint-time scale range that
procedural placement never reads, so with `MaxInitialAge` at its default the output lands on a
four-value ladder; and that verb's `density`, which writes the paint-brush density rather than the
simulation's seed density.

## Working In A Shared Level Without Destroying Someone Else's Scatter

**The `InstancedFoliageActor` is a singleton carrying every caller's foliage**, and three
consequences compound. `foliage.add_instances` given a bare **mesh** path reuses one global
`Auto_<Mesh>` foliage type, so two callers planting the same mesh land in the same component,
separable only by coordinate. The response names the actor but returns **no component name and no
index range**, so nothing supported maps "the instances I just added" to "the component to seat".
And `spatial.ground_instances` without an explicit `component` defaults to the component with the
most instances — which in a shared level is never yours.

That produced three incidents in one session, two unrecoverable: one call re-seated 512 instances
belonging to a region 16 km away; a caller that inferred component indices from creation order
re-seated 1,536 instances it did not author; and a clear-then-refill helper destroyed 50 instances
when the refill raised a `TypeError` *after* the clear had run. Component numbering churns within a
session — one editor went from 5 to 57 components, and one index held two different meshes twenty
minutes apart — so an index learned earlier in the same session is not a name.

The defensive recipe, in order:

1. **Create your own foliage types with a unique prefix** (`foliage.add_type`) and plant into that
   path, never a bare mesh path. Your instances then land in a component only you own.
2. **Resolve the component by static mesh plus instance count immediately before every write.** A
   procedural generator can rebuild and rename its components between two of your calls.
3. **Always pass `component` explicitly**, and check the returned `instanceCount` equals your own
   count before trusting the result.
4. **Capture `movedInstances[]`** — it is the undo receipt, `actor.set_instance_transforms` writes
   those rows back verbatim, and it is not governed by `detail`.
5. **Read a component back in full before clearing it**, and hold the transforms outside the call
   that refills it.
6. **Never remove every foliage type in a shared level.** A wholesale clear is every other caller's
   work.

To re-skin rather than re-place, `override_materials` on the component changes only the instances
that component draws, leaves the mesh asset and its material instances alone, and therefore cannot
collide with another caller editing those materials.

## What A Dense Scatter Actually Costs

Measure before optimising: the intuitive levers were negative on one level of ~51,000 authored
instances plus ~1M landscape grass instances.

**Instance count is not the cost.** At one pose 723,880 instances drew 95,648 triangles — 0.13 per
instance, because distant instances are GPU-culled for free. Cutting grass cull distances removed
41% of the instances, drew **31% more triangles**, made the frame *slower*, and stripped the
mid-distance ground back to bare material. Per-species cull distances and world-position-offset
disable distances were negative at two poses of three and were reverted. The cost is near-field
pixels, so the dial that pays is density or the material, not distance.

**What did pay: grass varieties not casting shadows** — 15% of the GPU frame at one pose, 7% at
another, at a real visual cost (the carpet loses its contact darkening). Shadow depths were 27.6% of
the frame, dominated by virtual-shadow-map rasterisation of foliage.

**None of the above is the cost a user will report first.** Every figure here was measured from
settled poses, and a settled pose cannot see the cost that dominates a *flight* through dense
vegetation: foliage instances are participants in the global-illumination surface cache, and flying
into unvisited canopy re-requests cards faster than the per-frame capture budget can retire them.
On one level that reached 272 ms of a 300 ms frame — 92% — while every raster pass measured here
stayed flat. Profile a scatter by flying continuously through ground the session has not visited, and
report that scope separately; the full method and its retraction are in
[`performance-profiling`](performance-profiling.md).

**Use p10, not the median** — a machine that intermittently triples frame time makes the median
wander by 4 ms while p10 reproduces to 0.07 ms. Pin the viewport size before baselining (one editor
came back at three different sizes across two restarts, and the smallest read 50% faster), and use
the primitives-drawn count as the guard that both halves of a comparison saw the same scene.

## See also

- [`vegetation-authoring.pathways`](vegetation-authoring.pathways.md) — the per-pathway trap
  catalogue: read your pathway's section before committing a level to it.
- [`vegetation-authoring.scatter-geometry`](vegetation-authoring.scatter-geometry.md) — why a
  jittered lattice reads as rows, how to measure it, and what superposition costs.
- [`spatial.ground-placement`](spatial.ground-placement.md) — the seat solve, the surface spec, the
  bounds-versus-contact footprint failure and the two acceptance rules in full.
- [`spatial`](spatial.md) — `scatter_layout`'s parameters and `ground_instances`' structural limits.
- [`level-building.instancing-and-scatter`](level-building.instancing-and-scatter.md) — owned-HISM
  scatter recipes, the Python transform traps, and the jittered-lattice doctrine.
- [`foliage`](foliage.md), [`landscape`](landscape.md), [`pcg`](pcg.md) — the per-verb argument
  reference for four of the five pathways.
- [`performance-profiling`](performance-profiling.md) — how to measure a scatter's cost without
  reaching a confident wrong answer, and every lever this page rejected, with why.
- [`render`](render.md) — view-driven geometry, contaminated frames and capture comparability.
- [`level-review`](level-review.md) — reviewing at more than one range, and not letting the builder
  sign off its own work.
- [`performance`](performance.md) — `performance.set_scalability` and the profiling verbs.
