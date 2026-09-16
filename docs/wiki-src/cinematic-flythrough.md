# cinematic-flythrough

How to author a camera flythrough that is *good* rather than merely *not wrong*: scout the level for
shots before you key anything, build the path through the shots you found, author tangents so the
camera actually moves, close a loop in velocity as well as position, and judge the result in motion
at the aspect ratio you are delivering. Pairs with [`level-review`](level-review.md), which owns
capture comparability and the look-before-you-measure discipline this page assumes; with
[`sequencer`](sequencer.md) for the track surface; and with [`mrq`](mrq.md) for the render.

## Scout Before You Key, Not After

**A path sketched before the level exists encodes only what the author already imagined.** The
failure is not that such a path is wrong — it evaluates, it composes, it passes every gate — it is
that nobody ever asked where the good shots in this level actually are. Patching it afterwards makes
it worse in a specific way: each pass asks "which frames measure badly?" and repairs those, so the
path converges on *nothing measurably bad* and never on *good*.

Invert the order. Fly the level first, collect a shortlist of poses that genuinely sing, and only
then lay a curve through them. The shortlist is the design; the keyframes are just how you spell it.

Some prompts, in rough order of how often they decide a shot. **Treat them as prompts, not a
specification** — the point of flying the level is to find what nobody thought to look for.

- **Where the hot accent is legible.** Most levels have one saturated thing in a desaturated palette,
  visible only from part of the space. Find that half-space early; it is a hard constraint.
- **Where the atmospherics land.** Volumetric shafts need the view axis near the light's bearing —
  a narrow cone of camera *headings*, not a place. You can be anywhere and see them facing right.
- **Where surface detail is strongest.** Projected light functions, caustics and wet-surface response
  are usually a level's best technical work and are routinely under-used because nothing points at
  the floor — or, just as often, at horizontal surfaces seen from above.
- **Depth versus flat bands.** A frame is dead when its subject crosses the lens as a horizontal
  slab. Watch camera *height* as much as azimuth: at the height of a building's roofline, every
  bearing reads as a slab, including ones that are superb from below or above.
- **Foreground occlusion.** Something passing close to the lens gives parallax that subject framing
  cannot. It works when the object is hard-edged and lit — a column, a bubble curtain — and reads as
  clutter when it is a soft two-sided card.

Two cheap instruments cover most of this. `camera.orbit_shots` with `point` + `radius` + `count`
gives a whole ring of framings in one call and reports the exact pose for each, so a promising
azimuth converts straight into a keyframe. `render.capture_open_level` covers everything else. See
[`camera`](camera.md) and [`render`](render.md).

## Scout Through The Lens You Will Deliver

**Capture at the delivered aspect ratio and at the delivering camera's real field of view.** Both
halves of that get violated by default, and each one alone is enough to invalidate a shot list.

- A square capture of a 16:9 film shows about **1.8x more vertically** than the frame the audience
  gets. Compositions judged square routinely lose their subject off the top or bottom edge in
  delivery, and dead frames read as acceptable because the square crop contains detail the film never
  shows.
- The capture verbs default to `fov 50`. A cine camera does not. Read the lens off the camera the
  sequence actually binds and pass *that*:

```js
// horizontal FOV = 2 * atan(sensorWidth / (2 * focalLength))
// 28 mm on a 36.0 x 20.25 mm filmback  ->  65.5 deg horizontal, 39.8 deg vertical
call("render.capture_open_level", { location, rotation, fov: 65.5, width: 1024, height: 576 })
```

Scouting at 50 deg for a 65.5 deg film is the same error as scouting square, in the other direction:
every framing you approve is a crop of what will actually be on screen, so the film arrives wider,
emptier and looser than the shots you signed off. Compute the number once, write it down, and use it
for every capture in the pass.

Hold `width`/`height` constant across the whole session — that is a
[`level-review`](level-review.md) rule for comparability and a `render` rule for editor stability.

## The Path Is Forced By The Site, So Measure The Site First

**A flight path is mostly not a creative choice — it is the small set of curves the level's obstacles
leave open.** Discovering that late means re-keying work you have already composed and approved.
Before laying keys, build a *legality map* of the volume the camera will occupy: sample a grid of
candidate poses and record, for each, the distance to the nearest obstacle.

A grid of `(bearing, distance, height)` around the subject, printed as a table, is worth an hour of
guessing. It tells you which arcs need altitude, which need a tighter radius, and which are closed at
every radius — and those constraints, not taste, decide where the path can linger.

Two things routinely surprise:

- **Vegetation is far taller than its bounds suggest**, and it is the most common thing a path
  clips. A canopy that reads as low ground cover can occupy a 4 m band the camera must fly over or
  under.
- **A large hollow prop is a wall in some directions and open in others.** Distance to its origin or
  to its axis-aligned box tells you neither.

Sample the map again after any content pass that scatters or moves anything — a legality map is a
measurement of the level at a moment, not a property of the path.

## Measure Clearance Against The Interpolated Path, Not The Keys

**Keys can all pass a clearance check while the curve between them fails.** This is not a rounding
error: on one path every key cleared its threshold and the interpolated polyline breached it on
**99 of 1201 frames**, worst case 431 of a required 900. A cubic bows away from the chord, so the
curve is somewhere the straight line between keys never goes.

Evaluate the authored curve to a per-frame table, then probe every frame:

```js
// bake the real curve, do not re-implement the interpolation
const range = MovieSceneSequenceExtensions.make_range(seq, 0, lastFrame + 1)
const baked = channel.evaluate_keys(range, FrameRate(60, 1))   // one value per display frame
```

Three properties the probe itself must have, each learned by getting it wrong:

- **Sweep in six directions and take the true distance to the impact point.** A one-way sweep is
  direction-blind on an initial overlap: the engine resolves the overlap against the sweep
  direction, so a segment that starts inside an obstacle's radius reports *no hit* one way and a hit
  the other. A hit/no-hit verdict is not a measurement — read the impact point and compute
  `min |p − impact|`.
- **Test against real collision, and exclude only what the path is legitimately allowed to be near**
  (the ground plane it flies over, a water surface). Everything else counts.
- **Scattered content is usually instanced onto a few holder actors**, so the "actor" a hit names is
  a holder whose bounds span the whole scatter. Its name is useful for diagnosis and its transform is
  useless for one — do not try to route around it by querying its position.

Fix a breach by lifting or tightening the segment, then **re-probe the whole path**: raising one key
changes the auto tangents of its neighbours and can open a breach somewhere you already passed.

## `RCTM_Auto` Is A Lie Until Something Recomputes It

**A key stamped "auto tangent" can hold tangent 0.** The tangent mode is a *declaration of intent*;
the values are only filled in when the engine runs its tangent solver over the curve. Author keys
through a path that never triggers that solver and every key stores zero, which turns a cubic curve
into a chain of independent smoothsteps — the camera decelerates to a near-stop at **every single
key** and accelerates out of it. The curve evaluates, the poses are right, and the film is a series
of lurches.

This is invisible to every still-image check ever devised. It is obvious within one second of
watching the motion, and it is caught numerically the moment you plot speed.

The recipe that produces genuinely smooth interior tangents:

1. Write all your keys (any order; writing at an existing time replaces that key).
2. **Add one throwaway key at a frame that has no key, then remove it.** Removing a key runs the
   tangent solver over the whole channel, which fills in every interior tangent.
3. The solver then **zeroes the first and last key unconditionally**, so those two need explicit
   tangents — see the loop section below.

Two traps in step 2. **Pick a frame that is not already a key**: adding at an occupied time replaces
that key, and removing it then deletes the real one — a key count one short of what you wrote is the
tell. And **read the tangents back**; a mode set without a recompute reads as auto with value 0,
which looks correct in every field except the one that matters.

**Tangent values are per tick, not per second.** Divide a units-per-second figure by the sequence's
tick resolution to get the stored value. Getting this wrong by the tick rate produces a curve that
overshoots wildly or flattens completely, and the error scales with the tick resolution rather than
announcing itself.

**The interior solver clamps.** Where the two intervals around a key are very unequal, or the key is
a local extremum in that channel, the computed tangent is reduced or zeroed rather than set to the
plain secant. That is correct — it is what stops a cubic overshooting — but it means an *inserted*
key can *deepen* a speed dip instead of smoothing it, by making its neighbour's intervals lopsided.
Measure after inserting, never assume.

## Close The Loop In Velocity, Not Only In Position

Matching the first and last pose closes the loop *visually in a still*. It does nothing for motion: a
camera can arrive at rest and leave at rest, hitting the same pose, and the loop still reads as a
stop rather than as a cut nobody can find.

**Treat the wrap as just another interval and give the seam the tangent the solver would have
computed if the curve were continuous.** For a seam key whose neighbours are the last key before it
and the first key after it:

```
seamTangent = (valueAfter − valueBefore) / (framesAfter + framesBefore) / ticksPerFrame
```

Set that on **both** the first and last key, arrive and leave, and set the tangent mode to **user** —
the solver discards a value on an auto key at its next recompute.

Three geometric consequences worth designing around rather than discovering:

- **The seam must not sit where the path reverses direction.** If the camera retreats one way and
  then advances back along the same line, its velocity passes through zero and no tangent can hide
  that. A closed loop needs its far turn taken *sideways* — the outbound and inbound legs offset so
  the camera is moving laterally where they join.
- **Every channel should be monotone through the seam**, position and heading especially. A channel
  that reverses at the seam gets an averaged tangent pointing the wrong way for one side, and the
  curve wobbles as it leaves.
- **Put the seam at the least conspicuous moment** — mid-move, constant speed, nothing in frame
  aligned to anything. A seam inside a slow lateral drift through empty space is invisible; a seam at
  a beat boundary is a cut.

For an angle channel that turns a full revolution, the last key's value is the first key's plus 360.
That is the same orientation and the correct value: it keeps the channel monotone. Add the wrap when
computing the seam tangent.

## Prove The Motion, Not Just The Poses

Two things need measuring before composition is worth reviewing, and both are covered in full on
[`cinematic-flythrough.motion`](cinematic-flythrough.motion.md):

- **The loop seam, proved instantaneously by convergence.** A multi-frame average cannot tell a C1
  join from a full stop — a zero-tangent smoothstep reproduces one to four decimals, and a project
  shipped a stopping camera behind exactly that proof. Measure the one-sided derivative at three
  step sizes a decade apart; the *ratio* is the evidence, not the magnitude.
- **The higher derivatives, judged against beats rather than scored for smoothness.** A speed
  profile passes a camera that steps in acceleration at every key, and the quantity that actually
  correlates with "feels jagged" is the path's **turn radius**, which nothing reports. Smoothness is
  not the goal: every discontinuity should be attributable to a beat, and the ones you cannot name
  are the defect.

## Judge Every Frame Against The Frame It Follows

Shots reviewed one at a time cannot show a defect that exists *between* frames, and a burst sampled
every 50 frames cannot either — at 60 fps a pop lasting six frames falls between two samples every
time. The fix is not more sampling: it is a cheap detector over **every** delivered frame plus a
close look wherever it fires.

**Render the sequence, then difference consecutive frames across the whole range.** If you have no
image-decoding library to hand, the compressed size of each frame is a serviceable proxy for its
complexity, and the frame-to-frame *change* in that size is a serviceable proxy for how much the
image changed:

```
sizes = [filesize(frame_i) for i in range(0, lastFrame + 1)]
deltas = [abs(sizes[i+1] - sizes[i]) for i in ...]
# flag any delta more than ~8x the median, then open those frames
```

On one 1201-frame render this flagged ten frames in two clusters. Opening them showed one cluster
was a bubble curtain crossing the lens — correct, and one of the best frames in the film — and the
other was a single foreground object passing within a few hundred units of the lens, motion-blurring
into a dark banded smear across a sixth of the frame for six frames. Neither is visible in a still
review; both are obvious the moment the delta series is plotted.

It is worth doing before anything more expensive because it covers every frame, which no sampled
burst does, and because it points at where to look — so the expensive step, a human or a model
opening images, is spent only on the frames that changed anomalously.

The same series proves the loop. Compare the frame-pair difference *across* the seam with a pair the
same distance apart just before it: a stall makes the across-seam difference collapse toward zero, a
jump makes it spike, and a correct loop puts it in the same regime as its neighbours.

## Render Through The Offline Pipeline, Never The Viewport

**Viewport capture is not a fallback for a cinematic.** It renders at whatever the editor's
scalability happens to be, it cannot do temporal sampling, and it does not run the engine warm-up
that atmospheric and temporal effects need to converge. Use the movie pipeline for anything that
will be watched as motion.

A configuration that renders a 1201-frame 1080p sequence in well under two minutes, and the reason
for each part:

- **One deferred render pass**, temporal sample count 1, spatial sample count 1, anti-aliasing
  override off. Multi-sampling multiplies render time and buys little on a camera that is already
  moving.
- **An engine warm-up of a few hundred frames.** Volumetrics, temporal accumulation and particle
  systems all need to settle; without it the first seconds render visibly different from the rest.
- **Screen percentage pinned to 100 through the pipeline's console-variable setting.** Editors are
  routinely found at 50, and screen-space passes — light shafts especially — are computed off the
  already-reduced buffer and blur away entirely at half resolution. That is a machine-local setting,
  so a render that does not pin it is not reproducible across machines.
- **Output resolution and frame rate set explicitly**, not inherited.
- **The video encoder's rate control set explicitly.** A quality-targeted default (constant rate
  factor) collapses on smooth, low-detail footage: on one 1080p60 render it produced **1.16 Mbps and
  a 3.4 MB file** while frame count, duration and resolution were all correct. Switch to variable
  bit rate and name an average and a maximum; the same sequence then encoded at **21 Mbps / 51 MB**.
  Nothing in the job result says the bitrate collapsed — only the file does.

Two traps that cost a whole render each:

- **The pipeline renders the sequence's playback range, not the range you think of as the film.**
  One sequence had a playback range starting 301 frames before zero, and the render dutifully
  produced 1502 frames. Read the playback range back and fix it on the asset rather than papering
  over it with a custom output range — every other tool reads that range too.
- **A filename token that resolves to nothing produces hidden files, not an error.** A format string
  whose leading token comes out empty writes `.0001.png`, which a plain glob does not list, so the
  render looks like it produced nothing. Put a literal in the format, and list the directory in a
  way that shows dotfiles before concluding a render failed.

Verify the finished video **from the file**, not from the fact that the job reported success: read
duration, frame count **and bitrate** out of the container header and check all three against what
you asked for. Frame count and duration can be perfect while the bitrate is 20x low. Then open
frames from the render itself — those are the pixels being delivered, and they are
not the same pixels a viewport capture gave you.

**A loop closes in camera, not in the world.** Material panners, world-position-offset sway and
particle systems run on world time, so at the last frame they are 20 seconds further along than at
the first. On one loop the camera pose at both ends was bit-identical and the two rendered frames
still differed by 3-4% — a caustic pattern, a swayed frond, a bubble column. No camera work fixes
it: the animated materials need a period that divides the loop length. Note also that the warm-up
frame count *shifts that phase*, so changing it changes which pattern frame 0 lands on rather than
converging anything.

## What To Report

The honest report of a flythrough has a shape, and it is not a checklist of gates passed:

- **The shot list, with why each shot earned its place** — this is the design, and what a reviewer
  needs in order to disagree usefully.
- **The speed profile** — min, max, mean, ratio, and the speed at every key.
- **The seam, proven by convergence** at three step sizes.
- **The clearance measurement** — frames under the threshold out of the total, and the worst case,
  measured on the interpolated path.
- **What continuous review found that the still burst did not.**
- **Which beats work and which are merely acceptable.** Every path has a weakest passage, usually
  forced by the site rather than chosen. Name it, say why it is the best available, and say what
  would have to change in the *level* to improve it. A report where every beat is strong has not
  been reviewed.

## See also

- [`cinematic-flythrough.motion`](cinematic-flythrough.motion.md) - proving the loop seam by convergence, and measuring speed, acceleration, jerk, curvature and angular rate so each spike can be matched to a beat.
- [`level-review`](level-review.md) - the review discipline this page assumes: look before you measure, make captures comparable, sample motion rather than assuming it, and never let the builder sign off its own work.
- [`sequencer`](sequencer.md) - the track and channel surface these keys are written through, and deterministic playhead scrubbing for frame bursts.
- [`mrq`](mrq.md) - queueing and running the offline render.
- [`camera`](camera.md) and [`render`](render.md) - the capture verbs used for scouting.
- [`spatial`](spatial.md) - the trace and measurement surface the clearance probe is built on.
- [`showcase-video`](showcase-video.md) — the rest of a showcase cut: asset turntables, grid tiles, frame-exact windows, and proving which render is in the delivered file.
