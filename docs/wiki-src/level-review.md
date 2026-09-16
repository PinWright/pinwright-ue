# level-review

How to prove a level is actually right — playable, correctly proportioned, and good to look at: look before you measure, pin the capture so two passes are comparable, frame a whole map without guessing, and check proportion at the ranges a map-altitude shot cannot resolve. Pairs with [`level-building`](level-building.md), which covers building the thing; [`visual-review`](visual-review.md) covers choosing a capture surface for a single visual state.

## Look First, Measure Only What Looking Cannot Resolve

Looking and measuring answer different questions, and the order matters. **Look first. Reach for a number only when looking cannot settle the question.**

When the two disagree:

- **Looking wins** on *does this look right* — composition, believability, motion, proportion in context.
- **Measuring wins** on *did the edit actually reach the screen*, *is this symmetric*, *is anything intersecting*, *is this file genuinely an image*.

**Metrics are detectors, not acceptance gates.** A number can confirm a change landed and can flag a class of defect worth looking at. It cannot tell you the result looks good, and no threshold on it constitutes sign-off.

**How metrics mislead:**

- **A metric that measures distance from a reference value is minimised at correctness.** Fixing a real defect drags the score down through its own minimum and back up again, so the gate rewards the bug and rejects the fix. Check that your metric is monotonic in quality before gating on it.
- **Comparing incompatible quantities reverses the sign of an error, not merely its magnitude.** A rendered visual extent measured against an engine collision or pathing radius is not the same quantity — "too wide" and "too narrow" swap places, and the correction is applied in the wrong direction.
- **Masks derived from brightness or darkness measure shadow, not material.** Under a strong directional light such a mask latches onto cast shadow and misses lit surfaces of the very thing you meant to measure. Prefer a hue-based or material-based mask for coverage.
- **Absolute pixel thresholds do not survive across sessions.** Editor exposure drifts from session to session even when it is pinned within one. Judge by relative change inside a single session, or by a step-function signal that drift cannot move.

**How looking alone fails:**

- A structure can be substantially out of proportion and look fine for a long time, and eyeballing produces a *confident* wrong diagnosis that makes things worse when acted on. Measure plan dimensions.
- Some defects are invisible without the right view at all. Flat massing does not show in a top-down at any resolution; it needs an elevation.
- A capture can be fully transparent and look perfect in any viewer that composites it over white. Check the alpha histogram, not your eyes.

## Every Number Needs A Source

**Tag every figure you report as measured (naming what produced it), read from a file (`file:line`), derived (showing the conversion), or open.** A figure that cannot be sourced goes on an open list — **never invent one to fill a cell.** Eleven unsourceable figures were found in one document set; one had already driven two days of building in the wrong direction, and all of them looked exactly like the sourced figures beside them.

The same applies to the instruments: a cached baseline is only valid if its date is *newer* than the content it classifies, and a script whose figures you cite belongs in version control with the document that cites them. Both rules, the open-list discipline, and how to build a check that is actually capable of failing are on [`level-review.evidence-and-provenance`](level-review.evidence-and-provenance.md).

Measuring off a picture has its own failure modes — establish the pixel-to-world registration and report its error before deriving anything from a reference. See [`level-building.working-from-references`](level-building.working-from-references.md).

## Make Captures Comparable

Auto-exposure silently re-balances every screenshot, so two review passes cannot be compared until it is pinned. Two independent traps make a real change read as no change here, and pinning only closes one of them — **hold `width`/`height` constant across every pass as well**, which is also what keeps the editor alive ([`render`](render.md#hold-the-capture-size-fixed-across-a-session)). If a pose needs a bigger frame, re-shoot the whole set.

1. `lighting.set_exposure {minBrightness: 1.0, maxBrightness: 1.0}` locks auto-exposure by configuring a PostProcessVolume. Keep exactly **one** unbound (infinite-extent) PostProcessVolume in the level — a second one competes with the first and the winner depends on priority, not on which you wrote last. If a shot is too dark, raise the key light rather than adding a volume.
2. `editor.set_game_view {enabled: true}` hides editor billboards, light icons, and volume wireframes so the capture shows shipped geometry only. **Set it before every burst and read it back** — it is per-viewport state that other work resets, and overlay icons get reported as content in the level. `gameViewEnabled: true` is **not** a clean-frame guarantee: check `overlayShowFlags` in the same response (a still-set `splines` draws water-body and landscape spline lines that read as foam or paths, and the verb raises `overlayWarning` when it sees that), and `notGovernedByGameView` for the overlays it cannot touch at all. `previous` carries the pre-toggle value, so restore from it rather than guessing.
3. `editor.set_view_mode {viewMode: "Unlit"}` gives a flat, lighting-independent read of silhouette and colour, which is what you want for a diff. **Report the mode with every capture**: a wireframe frame is not blank, so a blankness or file-size check passes it while it shows none of the work under review. Return with `viewMode: "Lit"` to judge the lighting itself. Also accepted: `Wireframe`, `DetailLighting`, `LightingOnly`, `LightComplexity`, `ShaderComplexity`, `LightmapDensity`, `StationaryLightOverlap`, `ReflectionOverride`. A viewport keeps **separate perspective and orthographic view modes**; the verb writes both by default and reports `applied.perspective` / `applied.orthographic`, so an orthographic tile burst and a perspective one agree. If you set `projection` to anything other than `both`, read `applied` before trusting the other projection.

`render.capture_open_level` drives the active viewport from a pose you supply: `location`, `rotation`, `fov` (default `50`), `width` / `height` (default `768`, capped `16384`), `projectionMode`. It is a true fixed-size render, not a crop, and it returns an absolute path under `Saved/Screenshots/OpenLevel/`. `editor.set_camera` has no projection control, and there is no `editor.get_state` — read `editor.status` or `system.inspect.get_viewport_info`.

Every pixel-producing verb needs a live Level Editor viewport; headless runs return `NO_ACTIVE_LEVEL_VIEWPORT`.

**Computing a pose rather than judging one** is on [`level-review.framing-math`](level-review.framing-math.md): what orthographic projection can and cannot do, the two field-of-view formulas (**formula A** and **formula B**, referred to by name below), and how to establish which screen axis is which before you measure a single pixel.

## Review At More Than One Range

**A single top-down view cannot resolve proportion.** At overview altitude a whole building is a couple of dozen pixels tall — bevels, roofs, stairs, trim and relative size are not resolvable at all, so nothing seen only from above has been reviewed. A structure set can be badly out of proportion, with every ledge and roof reading as stacked boxes, and survive an arbitrarily long review history, because top-down captures physically cannot show the defect.

Fix a small set of poses **for your level**, re-shoot all of them identically after every change, and compare pass to pass. Five roles is enough; the angles and distances are yours to pick:

| Pose | Rotation | Framing | What it is for |
|---|---|---|---|
| Overview | `pitch: -90` | formula A over the full level | Layout, distribution, symmetry, anything out of bounds. |
| Play angle | the pitch your game is actually played at | `fov 50` | **The acceptance view.** Does it read the way a player will see it? |
| Close oblique | `pitch: -30`, one group of objects filling the frame | `fov 50` | **Proportion between neighbouring objects.** This is the pose that exposes scale bugs. |
| Eye level | `pitch: -8`, camera at roughly eye height | `fov 60` | Object-vs-terrain scale, silhouette against sky, material detail. |
| From below | `pitch: +20` or steeper, camera under the ground plane or downhill of it | `fov 60` | **Objects that have sunk.** A top-down shows a hovering object as a shadow gap; nothing but a low or upward view shows one buried in the ground. |

Add a sixth when you need numbers rather than opinions: a **high-resolution orthographic top-down for measurement**. Record its units-per-pixel (`orthoWidth / width`) next to the image so plan dimensions can be read off it directly. That is a measuring instrument, not a judgement shot — do not use it to decide whether something looks right.

**Mirror every pose to the far side of anything meant to be symmetric, and re-shoot both.** A defect that exists on only one side is invisible in every shot of the other, so a one-sided review will keep passing a level that is visibly wrong from the opposite approach.

**Define each pose relative to sampled terrain height, not to an absolute Z.** An absolute camera height does not survive a terrain edit: after a re-sculpt the same numbers can put the camera underground or leave it staring into a cliff face, and it will go on producing captures that look perfectly plausible while framing the wrong thing. Sample the ground under the camera's XY and store the pose as *ground + offset*:

```js
call("spatial.raycast", {
  origin: { x: <cam_x>, y: <cam_y>, z: 100000 }, direction: { x: 0, y: 0, z: -1 },
  traceComplex: true, onlyClasses: ["LandscapeProxy"]      // terrain height, not treetop height
})
// camera z = hit.z + your_offset
```

`landscape.get_heights {landscapeName: "MyLandscape", region}` is the bulk alternative when you are re-deriving many poses at once. **Then check that no re-solved pose ended up inside geometry.** A solver that filters foliage out of the sightline but not out of the occupancy test will happily place the camera inside a tree, and the resulting frame is a plausible-looking capture of bark. Probe each solved position for an overlap before shooting the set.

Re-derive the whole set after every terrain change; only then is a pass-to-pass comparison meaningful. The overview pose is the exception — formula A already ties it to `level.get_bounds {}`, so re-reading the bounds each pass keeps it honest.

Resolution guidance: the shared **768 x 768** default covers ordinary stills and multi-image captures. `camera.orbit_shots` preserves **1024** for existing `count` / `angles` / canonical shapes and **640** for `views:"sides"`; go higher only when you are genuinely measuring off the pixels. Bigger frames cost more to produce and to review without telling you more.

## Check These Only At Close Range

- Are edges bevelled, or is everything a hard-edged primitive?
- Do roofs read as roofs, or as stacked flat tiers?
- Are stairs real stairs or slab stacks?
- Is trim recessed, or painted-on stripes?
- Do materials have surface variation, or flat colour?
- Do neighbouring objects have believable relative size?
- Is anything intersecting, floating, or z-fighting?

New silhouette detail also needs checking at `pitch: -90` specifically — thin upward features that read fine obliquely can read as scattered debris from directly overhead.

## Review In Situ, Never On Test Ground

**An asset verified alone on flat test ground has not been verified.** Authoring iteration on an isolated test row is fine, but nothing is done until it has been captured *in the map, beside its neighbours*, at every pose above. Proportion is a relationship; it does not exist in an isolated capture. Report honestly at both ranges — "reads well from above, weak up close" is the useful answer, and the one that gets acted on.

## Review Motion, Not Only Stills

**Proving an animation *evaluates* is not proving it *looks right*.** That frames differ, that transforms change, that scrubbing is deterministic — none of that says the motion reads well, and a defect that appears at only one point in a loop is invisible to a two-frame before/after check.

Sample the loop at several points and review each sample the way you review a still:

```js
for (const frame of [0, 15, 30, 45, 60]) {
  call("sequencer.set_playhead", { path: "/Game/Cinematics/MySequence", frame: frame })
  call("render.capture_open_level", { filename: "motion_" + frame + ".png" /* + your pose */ })
}
```

`sequencer.set_playhead` is the deterministic entry point: it moves to an exact position and re-evaluates before returning. Wall-clock ticking is not deterministic, so two passes sampled by waiting are not comparable — drive the loop in **frames**, not seconds, and do not start playback first. See [`sequencer`](sequencer.md).

**Run your ground-clearance, intersection and placement checks at every sample, not only at rest.** A character can be planted correctly at frame 0 and float or sink mid-stride. Note also that **a clean floater sweep over actor placement says nothing about animated actors** — their per-frame transforms live in animation keys, not in the actor transform that `spatial.verify_placement` reads, so the animated case has to be sampled explicitly.

## Navigate The Level, Do Not Only Capture It

**A scripted capture set never exercises a level the way a person does.** Fixed poses answer only the questions you already thought to ask. Fly the level as well — move through it continuously, at the speed and in the way someone playing or editing it would.

Two categories of defect are structurally invisible to a fixed-pose set and immediately obvious in navigation:

- **Usability against the world's scale.** Camera or movement speed tuned for a differently sized world, traversal that drags, distances that read fine in a still and feel wrong in motion.
- **Motion defects.** Anything that exists only between frames — popping, jitter, foliage or LODs snapping in, a loop that hitches, and z-fighting shimmer (see the next section).

Navigation does not replace the fixed pose set, which is what makes two passes comparable. It is the pass that finds what the fixed set cannot see.

## Assume Z-Fighting Is Present Until You Have Checked

**Treat z-fighting as present by default and check for it deliberately every pass** — do not wait until something looks wrong. Procedurally placed and rapidly iterated levels produce it routinely, and a still can look perfectly clean while the same surface shimmers badly the moment the camera moves. Move the camera slowly across every place where two surfaces coincide; a frozen frame will not show it, and there is no verb to call instead.

Where it comes from, how to tell it apart from temporal-AA ghosting, shadow acne, LOD popping and specular aliasing, why differencing consecutive frames reports clean on a visibly broken scene, and why this one check must not inherit the reduced capture resolution the rest of this page recommends are on [`level-review.z-fighting`](level-review.z-fighting.md).

## Measure Instead Of Eyeballing

A screenshot never carries a metric coordinate. Assert placement with box math:

```js
call("spatial.verify_placement", { actor: "Crate_01", expect: { grounded: { maxGap: 2 }, noOverlapWith: ["Wall_North"], within: "Level_Bounds" } })
```

The first argument is `actor`. `expect` runs only the checks present: `grounded {maxGap}`, `on: "<actorName>"`, `noOverlapWith: [names]`, `within: {min, max}` or an actor name. `spatial.measure_distance {a, b, mode}` (`center` / `pivot` / `edge_gap`) and `spatial.measure_overlap {a, b}` give the raw numbers; they read the same AABB the placers use, so their numbers agree.

Two editor-world rules gate the loop. **Spawn and trace in separate calls** — in a non-PIE world a body spawned in the same call is not yet in the physics scene-query structure, so the trace misses its own new target. And **`expect.grounded` here is a one-sided, single-ray check**: it measures clearance upward from one point, so an actor sunk *into* a surface reads as "no ground" rather than as a negative gap, and a wide actor touching on one corner reads the same as a bedded one. When burial or one-point balancing matters, measure with `spatial.verify_grounding` (`maxGapCm` **and** `penetrationCm`, sampled over the footprint) or `level.audit` instead — see [`spatial.ground-placement`](spatial.ground-placement.md).

For anything that is meant to be symmetric or evenly distributed, verify in bulk from `python.execute` rather than by eye: negate the coordinates of one half and assert the sets match, assert worst-case tilt against the value your scatter designed for (a bad Rotator argument order shows up as roughly 180°), and report min / median / max ground gap across a sample. `python.execute {mode: "evaluate_statement"}` is the cheapest surface for that.

## Track What Each Layer Depends On

**Changing an upstream layer silently invalidates everything placed on or against it.** Nothing errors and nothing warns; the downstream layer simply becomes wrong, and every capture of it keeps looking plausible. Write the dependency list down for your own project and re-verify the downstream side after every upstream edit.

| Upstream change | What it invalidates |
|---|---|
| Terrain sculpt | Anything seated on the ground — props, foliage, structures, encounter setups — plus any review camera defined at an absolute Z. |
| Path, road, or corridor width | Anything positioned relative to that path's edge or centreline. |
| Re-scattering foliage | Anything meant to move or see through the scatter. |
| Material edit | Every colour or coverage measurement taken before it. |
| Re-lighting or exposure change | Every absolute pixel threshold derived from an earlier pass. |

Build scripts make this tractable. If each layer re-derives its ground contact at build time instead of baking absolute Z, re-running the downstream script *is* the re-verification — see [`level-building`](level-building.md).

## Do Not Let The Builder Sign Off Its Own Work

**Whoever built a layer will choose verification that flatters it** — the angle where it reads best, the metric it already passes, the range at which its defect is unresolvable. This is rarely deliberate: the builder knows what the thing is meant to look like and sees that instead of what is on screen.

Have the acceptance pass done by someone — or something — that did not build the thing: a different person, a different agent, or at minimum a pose set and checklist fixed *before* the layer was built and not edited by whoever built it. The builder reports what changed; someone else decides whether it is acceptable.

**Give that pass a different instrument, not just a different reviewer.** A second reviewer running the builder's own check re-confirms the builder's own blind spot. In one review, a reviewer who built the *opposite* kind of measurement found four errors the builder had looked directly at and passed — and a third pass then found an error in the reviewer's own instrument, which is why the instrument is reviewed too. The full three-step form is on [`level-review.evidence-and-provenance`](level-review.evidence-and-provenance.md).

## Do Not Trust A Success Flag Alone

**Several verbs report success for work they did not do — verify the effect, not the envelope.** The general rule is that a check has to measure something the write path cannot fake: not a readback of the value you just wrote, not an existence probe that the *previous* save already satisfies. And persistence has three levels — readback, a save that reported success, and a genuine reload — of which only the third is proof.

The per-verb catalogue (`python.execute`, `pcg.generate`, `material.authoring.compile_material`, `landscape.create_procedural_terrain`, `spline.scatter_meshes_along_spline`, `actor.spawn_batch`, `foliage.add_instances`) is on [`level-review.evidence-and-provenance`](level-review.evidence-and-provenance.md), together with how to measure both failure directions.

## See also

- [`level-review.evidence-and-provenance`](level-review.evidence-and-provenance.md) — sourcing every figure, the open list, checks that can actually fail, the three levels of persistence, and independent acceptance.
- [`level-review.framing-math`](level-review.framing-math.md) — orthographic capture limits, the two field-of-view formulas, and empirical screen-axis verification.
- [`level-review.z-fighting`](level-review.z-fighting.md) — sources, the flicker-differentiation table, the fix, and why an automated detector does not exist yet.
- [`level-building.working-from-references`](level-building.working-from-references.md) — measuring against reference imagery: image type, registration and its error, chunked comparison, defensible colour samples.

- [`level-building`](level-building.md) for building the map this page reviews.
- [`level-building.capture-and-review`](level-building.capture-and-review.md) for the capture step as it appears inside the build workflow.
- [`visual-review`](visual-review.md) for choosing a capture surface.
- [`render`](render.md) for viewport and asset-preview captures.
- [`camera`](camera.md) for multi-angle capture and the orthographic caveat.
- [`spatial`](spatial.md) for measurement, deprojection, and placement verification.
- [`editor`](editor.md) for game view, view modes, and editor status.
