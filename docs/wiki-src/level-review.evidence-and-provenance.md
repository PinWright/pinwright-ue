# level-review.evidence-and-provenance

Make level-review numbers trustworthy: record each figure's source, label honest unknowns, and build checks that can fail. Pairs with [`level-review`](level-review.md), which covers what to inspect and where.

## Tag Every Figure With Its Source

Four tags cover everything. Carry the tag with the figure wherever it appears.

| Tag | Means | Must carry |
|---|---|---|
| measured | produced by running something | which script, against which capture, asset or level |
| read | copied out of a file | `file:line` |
| derived | computed from other figures | the inputs and the conversion, written out |
| open | not known | nothing — that is the point of it |

**An untagged figure is an open figure that has not admitted it yet.** Tagging costs a few words while you have the answer in front of you, and is close to impossible to reconstruct a week later.

## A Figure You Cannot Source Goes On The Open List

**Never invent one.** Eleven unsourceable figures were found in one document set on this project. What they turned out to be is the argument for the rule:

- One had driven two days of building in the wrong direction before anyone asked where it came from.
- One was a camera position that had been copied into a document as a path coordinate.
- One was a pixel offset in a cropped image, quoted as world centimetres.

All three were plausible: right order of magnitude, right-looking units, sitting in a table beside properly sourced figures. Plausibility is what makes an invented number expensive — an implausible one gets caught the same day.

**An open list is a deliverable, not a failure.** "The width of this space is unknown" is directly actionable; someone goes and measures it. A number invented to fill the cell is not actionable, reads as settled, and costs more the longer it survives.

## Keep Corrected Figures Visible

When correcting a number, keep the old value and reason beside the new one. Deleting it lets the same bad source recreate the error and hides anything built against the old value.

## Date-Check Every Cached Baseline

A cached baseline — an asset dump, a colour histogram, a coverage sample, a screenshot set — is a claim about content **at a moment**. Compare its timestamp against the modification time of what it classifies before trusting any comparison against it. One baseline here was built a day *before* the content it described, and duly reported a regression that did not exist.

Regenerate rather than reason about it: regeneration is cheap and the argument is not. A baseline of unknown age is not a baseline.

## Scripts That Produce Cited Figures Are Source Code

Commit them, in the same repository as the document that cites them. Thirty-eight analysis scripts were lost on this project before anyone noticed — one of them still cited by name in a committed document, which made its figure permanently unverifiable and un-rerunnable.

A script kept only in scratch or `Saved/` is one cleanup away from turning every figure it produced into an open item.

## Make The Check Measure What The Write Path Cannot Fake

The recurring failure is not a missing check but one that agrees with the tool because both read the same thing — tool and verification agree while both disagree with the level.

Shapes to recognise in your own checks:

- **A readback of the value you just wrote.** It proves the write reached memory and nothing else.
- **A check that opens the same subsystem the fix controls.** A position check that opened an animation asset before measuring could only ever observe the state the fix controlled; a wave of characters sat 4300 uu in the air for days behind exactly that check.
- **An existence probe blind to the failure the write path produces.** The file from the *previous* save satisfies "the file exists", so a skipped save is indistinguishable from a real one. Capture timestamp, size and dirty state *before* the write and compare.

Prefer a different subsystem, a fresh world probe, or a reload. Re-measure **after** the mutation and derive the verdict from that measurement, not the supplied value. If a blind spot remains, report it instead of emitting a zero that reads as a measurement.

## Persistence Has Three Levels, And Only The Third Is Proof

1. **Readback** — the value is in memory. This says nothing about durability.
2. **A save that reported success** — still not proof. A transform set from a script may not dirty the package at all, so the save writes nothing and reports success honestly.
3. **A genuine reload** — the only proof. One value on this project survived readback *and* save and reverted on load, because the engine re-derives it from other data at load time.

Re-opening a map that is already open does not reload it and will not catch level 3. Nor will any check that runs in the same editor session that made the edit, if the value is one the engine re-derives. See [`safe-mutation-save`](safe-mutation-save.md) for the read-edit-verify-save loop and [`level-building.build-scripts`](level-building.build-scripts.md) for the dirty-flag traps that produce silent no-op saves.

## Measure Both Failure Directions

- **A one-sided measure hides half the failures.** A clearance check that measures upward scores a sunken object identically to a perfectly seated one — 620 buried actors went unnoticed behind one here. Use a signed measure, or a pair (`maxGapCm` *and* `penetrationCm`) taken from the same probe.
- **Per-item checks miss assemblies.** Objects resting on each other each pass individually while the whole stack floats. If the question can be asked about a set, the check has to be able to answer about the set.
- **A single sample cannot seat or verify a large object.** A pivot is not the lowest point, and the lowest point is not a plane. Sample the footprint. See [`spatial.ground-placement`](spatial.ground-placement.md).
- **Assert an independently-derived expected count, not just a failure count.** A sweep that stopped early, matched nothing, or silently skipped a category reports zero failures and is indistinguishable from a clean run. Derive what the count *should* be from something other than the sweep itself, and reconcile any difference rather than accepting it.

[`level.audit-checks`](level.audit-checks.md) documents a check catalogue built this way, including what it structurally cannot see — which is the part worth copying.

## Verbs That Report Success For Work They Did Not Do

Verify the effect, not the envelope:

- `python.execute` returns the RPC success envelope even when the script raised — read the returned Python `success` / `result` / `log`.
- `landscape.create_procedural_terrain` no longer fakes a paint — a layer the material does not declare is `LAYER_NOT_FOUND` or `LANDSCAPE_MATERIAL_NO_LAYERS`. Still read the verification fields on a *success*: `texelsWithWeight > 0` is what proves the weightmap changed.
- `pcg.generate` judges a pass by `instanceCount`, not `pointCount`. `pointCount` counts only what reached the graph's Output node, so a graph ending in a spawner emits none — it is omitted rather than reported as a confident `0`, with `graphOutputAvailable: false` saying so.
- `material.authoring.compile_material` returns `compiled: true` meaning only "it ran" — read `compileSucceeded` for the verdict, and `compileErrors[]` for the reason. On a landscape material also read `consumerRefresh.complete`, which is what proves the compile reached the screen; the old assign-away-and-back round-trip is no longer needed. See [`level-building.terrain-and-water`](level-building.terrain-and-water.md).
- `spline.scatter_meshes_along_spline` is not verifiable through `spline.get_splines_info`, which reports curve geometry only — read the returned `meshesCreated` or `actor.get_components`.
- `actor.spawn_batch` and `foliage.add_instances` drop malformed entries silently — compare counts, and note that an entry missing `location` is not dropped at all: it spawns at the world origin.
- Any writer that only marked a package dirty reports `saved: false` with `pendingFlush`. That is the truth, not a failure — but it means nothing is on disk yet.

## Give The Acceptance Pass A Different Instrument

Independent review is only independent when the *instrument* is independent too. A second reviewer running the builder's own check re-confirms the builder's own blind spot.

On this project a reviewer who built the **opposite** kind of measurement — one that could see what the builder's check structurally could not — found four errors the builder had looked directly at and passed. A third pass then found an error in the reviewer's own instrument.

Make it a documented step, in this order:

1. The builder reports **what changed and how it was measured**, not whether it is acceptable.
2. An acceptance pass by someone who did not build the layer measures it a **different way** — a different quantity, a different subsystem, a different view — and decides.
3. The instrument itself gets reviewed. A check is content, it can be wrong, and its errors are invisible precisely where it is trusted most.

If a second reviewer is not available, the substitute is a pose set and checklist fixed *before* the layer was built and not edited by whoever built it — see [`level-review`](level-review.md).

## See also

- [`level-review`](level-review.md) — the review workflow these rules serve.
- [`level-building.working-from-references`](level-building.working-from-references.md) — registering reference imagery, and reporting the error of a measurement taken off a picture.
- [`safe-mutation-save`](safe-mutation-save.md) — the read-edit-verify-save contract and the persistence fields to read.
- [`spatial.ground-placement`](spatial.ground-placement.md) — footprint sampling, and measuring both floating and buried.
- [`level.audit-checks`](level.audit-checks.md) — a check catalogue with its thresholds, ignore model, and stated blind spots.
