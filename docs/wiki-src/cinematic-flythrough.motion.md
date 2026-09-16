# cinematic-flythrough.motion

How to prove a scripted camera move actually moves well: the loop seam proved instantaneously by
convergence rather than by an average, and the higher derivatives measured and then judged against
the film's beats rather than scored for smoothness. Split out of
[`cinematic-flythrough`](cinematic-flythrough.md), which covers the rest of the workflow.

## Prove The Seam Instantaneously, By Convergence

**A multi-frame average cannot distinguish a C1 join from a full stop.** A pure zero-tangent
smoothstep reproduces the 10-frame average velocity of a smooth curve to four decimals — one project
shipped a stopping camera behind exactly that "proof".

Measure the one-sided derivative at the seam at **three step sizes, each ten times smaller**, and
report all three:

| step | speed before | speed after | difference |
|---|---|---|---|
| 1 frame | 3423.25 | 3395.42 | 128.62 |
| 1/10 frame | 3408.93 | 3406.11 | 12.96 |
| 1/100 frame | 3407.08 | 3407.25 | 1.28 |

**The number that proves it is the ratio, not the magnitude.** The difference falling by 10x for each
10x reduction in step is first-order convergence to zero — the residual is the curvature term, which
is exactly what a C1 join leaves behind. A C0-only seam converges to a non-zero constant instead, and
a single-step measurement cannot tell the two apart. Do the same for every angle channel.

## Measure The Higher Derivatives, Then Ask What Beat Each Spike Is

**A speed profile alone passes a camera that steps in acceleration at every key.** Speed is the
first derivative; a cubic Hermite curve is C1 by construction, so its *acceleration* jumps at every
single key whatever the tangents are. Twenty-odd acceleration discontinuities read as chop even when
the speed plot is immaculate — and every first-derivative check ever written will call that path
finished.

So measure further, and measure the right things:

- **speed** — min, max, mean, ratio, and the value at every key. A ratio in the single digits is a
  camera with dynamics; three or four orders of magnitude is a camera that stops.
- **acceleration** — the step across each key, absolute and relative to the local magnitude.
- **jerk** — per segment. Report it **beside the segment durations**, because jerk scales as `1/h^3`:
  a segment half its neighbours' length carries eight times their jerk for the same shape.
- **turn radius**, and the tangential/normal split of the acceleration. This is the one that
  correlates with "feels jagged", and it is the one nobody measures.
- **angular rate and angular acceleration** for the rotation channels, in degrees. They can feel
  worse than positional jerk and are not implied by the linear numbers.

**Compute them analytically, not by finite differences.** Differencing a dense bake gives a clean
first derivative and a useless second one: value storage is float32-precision, so the noise is
divided by `dt^2`. The Hermite basis gives all three in closed form from the keys and tangents you
already have, and the third is constant within a segment. Validate your evaluator against the
engine's own bake before trusting a single number — agreement to storage precision is the bar.

**Smoothness is not the goal — attributability is.**

**A camera with zero jerk is a drone on rails.** Real camera work decelerates into a subject,
accelerates out of a beat, holds. Those are discontinuities *on purpose*, and a metric that penalises
them optimises the film flat. The criterion is not "is this spike small" but:

> **Every discontinuity should be attributable to a beat.**

Walk the outliers and name each one. *Arriving at the subject. Breaking out of the colonnade. The
release into the reveal.* Where you can name it, leave it — and consider whether it is decisive
*enough*, because a timid slow-down reads as drift where a committed one reads as arrival. Where you
cannot name it, that is the defect, and that is what to fix.

The loop seam is the one exception: it is a hidden cut, so continuity there is non-negotiable.

**Three fixes, in the order to try them.**

1. **Even out the key spacing.** Most unexplained jerk is a spacing artefact, not a shaping one.
   Keep adjacent segment durations within about 1.4x. Note the trap: spacing keys at equal *arc
   length* makes the fast legs *short in time*, which is the wrong variable — jerk depends on
   duration. Aim instead for **equal durations with arc lengths proportional to the speed you want**,
   and put the speed changes at the beats.
2. **Remove keys that exist for a constraint rather than a beat.** A key inserted only to route
   around an obstacle usually lands mid-segment and halves two durations at once. Satisfy the
   constraint by moving its neighbours instead.
3. **Widen the corner geometrically.** Only then reach for tangents.

**Never shrink a tangent at a key where the path also turns.**

This is the specific trap that produced the worst corner on one path, and the newly available
explicit-tangent parameters make it a one-liner.

Shrinking a key's tangent is the obvious way to author a deceleration. It works — on a **straight**
stretch. At a vertex where the direction also changes, a short tangent concentrates the entire turn
into a tight corner at that key: a 63-degree direction change plus a tangent scaled from 1309 to 820
uu/s produced a **66-unit radius of curvature**, a hairpin, while every authored number still read
exactly as intended.

**Get a ritardando from timing, not from tangent length.** Make the segment *into* the beat long and
the one *out of* it short; the speed then dips across the approach and the corner stays round. Put
the speed minimum where the path is straight, and let the turn happen after it.
