# level-building.working-from-references

Building to match reference imagery: deciding what the image actually is, registering it to world coordinates before measuring anything, and comparing captures against it without manufacturing differences. Read this before deriving a single dimension from a picture — [`level-building`](level-building.md) covers the build itself, [`level-review`](level-review.md) the passes that prove it.

## Establish What Kind Of Image You Have

Nothing else on this page is safe until you identify which of these three kinds you hold.

| Kind | Single scale? | What it can be used for |
|---|---|---|
| Orthographic render or plan export | Yes — constant across the frame | Reading plan dimensions directly |
| Perspective photo or stitched capture | No — scale varies with distance from the camera axis | Relationships and feature identification, not absolute measurement |
| Stylised art or promotional render | No | Palette, biome, mood, which landmarks exist |

**Diagnose it from the picture, not from what the file was called.** Look at objects that should be vertical, away from the frame centre: in an orthographic image they stay vertical, in a perspective image they lean outward and lean further the further out they are. Repeated features converging toward a point is the same signal.

One reference was treated as an orthographic render for days but was a near-nadir perspective stitch, proven by uprights leaning near the frame edge. That explained a calibration disagreement between two anchors: they sampled different distances from the camera axis, and a perspective image has no single scale for them to agree on.

## Register Before You Measure, And Report The Error

Registration is the mapping between image pixels and world coordinates. Establish it once, as its own step, before any measurement — and publish its error beside it. **A misregistered comparison manufactures differences that are not there and hides ones that are.**

1. Pick at least three features identifiable in both the image and the level, spread across the frame rather than clustered in one region.
2. Fit the pixel-to-world mapping to them.
3. Report the residual at the centre **and** at the edge separately. They are not the same number, and the edge figure is the one that limits you.

Measured in that reference set: **±195 uu at map centre, ±867 uu at the edge.**

**Then work out what that error rules out, before running any comparison.** The same ±867 uu is 5.4% of one tile on a 4×4 comparison grid and 21.7% of one tile on a 16×16 grid. That ruled out the 16×16 grid on registration alone — a fifth of a tile of positional uncertainty cannot support a per-tile verdict, and no amount of care later in the comparison recovers it. Deciding this first costs one division; discovering it afterwards costs the whole comparison pass and every conclusion drawn from it.

## Never Take Scale From The Bounding Box

References are cropped, padded, letterboxed and clipped, and none of that is visible in the file. Scaling by "image width equals level width" bakes the crop into every number you derive from it.

- Anchor on **identifiable edges of the playable space** — a boundary that the image and the level both unambiguously contain.
- **Cross-check with a second, independent anchor** at a different place and a different scale. A single anchor cannot be wrong out loud.
- When two anchors disagree, that is a finding, not noise. Either an anchor is misidentified or the image is not orthographic — resolve which before deriving anything. A scale averaged across a disagreement you did not explain is an invented number wearing a measurement's clothes.

## Prefer Ratios To Absolute Units

A ratio between two things visible in the same image survives a wrong camera model, a wrong crop and a wrong scale. The same quantity in world units survives none of them.

Express targets as ratios wherever the question allows — step height as a fraction of a doorway, path width as a fraction of the structure beside it, canopy spread as a fraction of trunk height. Convert to centimetres once, at the end, through a scale you have stated and sourced. When that scale is later corrected, one conversion changes instead of fifty derived figures.

## Compare In Chunks, Coarse First

1. Produce a capture that matches the reference's framing as closely as the image type allows: same viewpoint, pinned exposure, game view on. See [`level-building.capture-and-review`](level-building.capture-and-review.md).
2. Tile the reference and the capture **identically**, and compare tile by tile. Per-tile verdicts localise a difference; a whole-frame score only reports that one exists.
3. Start with the coarsest grid that could answer the question. Escalate only where a finer grid demonstrably reveals something the coarse one did not.
4. **Report when the finer pass changed nothing.** "The 8×8 grid changed no verdict from the 4×4" is a result worth writing down — it stops the next person paying for the same escalation.

Never choose a grid finer than your registration error supports.

## Sample Colour From Recorded Coordinates

A colour taken off a reference is usable only if it is reproducible and you can say what it covers.

- Record the **pixel coordinate, the patch size, and the image** next to every swatch, so the sample can be retaken and argued with.
- State **what the patch physically covers**, and make that one material. A patch spanning two materials is not a colour of either.
- **A median across a group of mixed things is not a colour.** One swatch was a mask over rock, bare wood and cast shadow. It was used as a canopy colour target for days, so the canopy was tuned toward a number that described no surface in the reference.

## Know What The Reference Cannot Answer

Before using an image, write down which questions it can settle and which it cannot. A stylised render can be completely faithful about art direction, biome and which landmarks exist while containing no usable gameplay layout whatsoever — and the temptation is to take layout from it anyway, because it is the only picture available.

An image does not become able to answer a question because nothing else can. That question goes on the open list; see [`level-review.evidence-and-provenance`](level-review.evidence-and-provenance.md).

## Beware Measurement Bias From The Content Itself

What is *visible* in an image is not what is *there*, and the gap is systematic rather than random — so it does not average out over more samples.

Crown sizes eye-marked inside a closed canopy came out **1.33× too small**, consistently. Only crown *tops* are separable there; overlapping skirts are hidden, so every mark measured a fragment and was short by roughly the same factor.

**Measure where the object is separable** — an isolated instance, the edge of a group, a clearing — then take the same measurement inside the group and compare. A stable ratio between the two gives you the bias factor; a wildly varying one says the inside-the-group measurement is unusable and should be dropped rather than corrected.

## See also

- [`level-review.evidence-and-provenance`](level-review.evidence-and-provenance.md) — tagging every derived figure with its source, and the open list for the ones you cannot source.
- [`level-building.capture-and-review`](level-building.capture-and-review.md) — producing the matched capture the comparison runs against.
- [`level-review.framing-math`](level-review.framing-math.md) — orthographic capture, units-per-pixel, and verifying which screen axis is which before measuring a pixel.
- [`level-building`](level-building.md) — the build workflow this feeds.
- [`level-review`](level-review.md) — the pose set and review passes.
