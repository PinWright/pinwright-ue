# level-review.z-fighting

Finding and fixing z-fighting: its causes, look-alikes, and automatic or visual review. [`level-review`](level-review.md) covers the review pass.

## Where It Comes From

Check these likely surfaces rather than sweeping the whole level:

- Two surfaces at the same depth — coplanar faces, stacked slabs, a floor plane laid exactly on the ground.
- Geometry re-seated onto terrain without a deliberate offset, especially after a re-sculpt.
- Decals, paint, or trim geometry coplanar with whatever they sit on.
- Anything duplicated in place. A paste that landed on top of its source is invisible in a still and fights immediately in motion.

## How To Spot It By Looking

Look for shimmer or crawl, strongest at grazing angles and distance where depth precision is worst. It is more visible in motion than in a still, so move the camera slowly across each suspect surface.

## Not Everything That Flickers Is Z-Fighting

Identify the flicker before acting; distinguish it by what it follows:

| Symptom | Tied to | How to tell |
|---|---|---|
| Z-fighting | A *pair of surfaces* | Changes as the camera moves; strongest at grazing angles; sits exactly where two surfaces coincide. |
| Temporal-AA ghosting | A *moving object* | Trails behind the motion and stops when the object does. |
| Shadow acne | The *light* | Banding follows the light direction rather than the camera; worst on curved surfaces. |
| LOD or foliage popping | *Distance* | Snaps at fixed distance bands, repeatably, in the same place every pass. |
| Specular aliasing | *Gloss* | Sparkles on high-gloss materials and follows the highlight, not a surface seam. |

## Fix It With An Offset Or A Deletion, Not A Viewport Nudge

Give coplanar surfaces a small deliberate depth offset. Delete an unwanted duplicate instead of nudging it: it still doubles the draw and fights from other angles.

## On Checking This Automatically

The shipped detector is [`render.detect_z_fighting`](render.md). It renders offscreen twice with a perturbed clipping plane, compares `baseColor` / `normal` surface identity, and returns affected-pixel count/fraction, regions/actors, pass/fail, and an optional mask PNG. Omit `location` / `rotation` to use the active Level Editor viewport camera; it does not move, resize, or read viewport pixels. See the method page for all parameters and defaults.

The analysis default is 768×768. On 2026-08-24, a matched coplanar-slab fixture measured 461 affected pixels / 8 regions at 1920 and 38 / 8 at 768; the separated control measured 0 / 0 at both. Below the 768 long-edge default, `resolutionWarning` records the risk; use full resolution when z-fighting is the question. `nearPlaneRatio` defaults to 3 and must not be 1 or a power of two. Use `farPlane` when Nanite geometry produces scattered false positives.

**Differencing consecutive colour frames does not detect it.** A one-shot capture drops anti-aliasing to none, so two frames are identical; forcing jitter brings temporal-AA convergence; and renderer flicker rejection suppresses this signal. Use the detector's perturbed-plane comparison instead.

## See also

- [`level-review`](level-review.md) — the review pass this check belongs to, including navigating the level rather than only capturing it.
- [`level-building.terrain-and-water`](level-building.terrain-and-water.md) — re-seating geometry after a terrain change, the most common source.
- [`render`](render.md) and [`camera`](camera.md) — capture arguments, including full-resolution captures.
