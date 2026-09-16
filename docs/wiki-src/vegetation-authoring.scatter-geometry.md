# vegetation-authoring.scatter-geometry

Why a jittered lattice reads as rows, how to measure whether yours does, and what superimposing
lattices costs. The measurement half of [`vegetation-authoring`](vegetation-authoring.md) § *Lay out
a scatter that does not read as a grid* — that page carries the rule; this one carries the numbers
and the method, so a disagreement can be re-derived rather than argued.

## Rows Survive Almost The Whole Legal Jitter Range

`spatial.scatter_layout` offsets each point by up to `jitter * spacing` **per axis**, drawn
continuously. A hex lattice's rows are pitched `spacing * sqrt(3)/2` apart. Two adjacent rows can
therefore never touch while

    2 * jitter * spacing  <  spacing * sqrt(3)/2      i.e.   jitter < sqrt(3)/4 = 0.4330

Below that threshold an empty corridor runs between every pair of rows, whatever the seed. The
threshold is **86.6% of the legal range** (`jitter` is refused above 0.5) and it contains the `0.18`
default.

Measured at the default on spacing 1000, over one region:

- **506 cm** of corridor between rows.
- **41.6%** of the ground in Y can hold a point at all; the rest is structurally empty.
- Nearest-neighbour bearing chi-square **1362** against a 99% critical value of **24.7**, with the
  histogram peaking in the bins containing 0, 60 and 120 degrees — the three hex axes.

`pattern: "square"` is worse, not better: its rows are disjoint at **every** legal jitter, with 36%
of the ground reachable.

At `jitter: 0.5` the lattice genuinely dissolves — bearing chi-square 11.5, below the critical
value — but the spacing floor goes with it: the closest pair measured 37.5 cm on a 1000 cm spacing
and **38.9%** of points landed inside half the nominal spacing. There is no single setting that
gives both an organic look and a spacing floor.

## The Test: Nearest-Neighbour Bearing Chi-Square

For each point take the bearing to its nearest neighbour, fold it into `[0, 180)`, and bin it into
12 bins of 15 degrees. A lattice concentrates those bearings on its axes; an isotropic set spreads
them evenly. Chi-square against a uniform expectation, 11 degrees of freedom, 99% critical value
**24.725**.

**Chi-square scales with N, so renormalise before comparing two sets.** `chi²@1k = chi² * 1000 / N`
is the comparable figure; a set of 188 points scoring 167 and a set of 985 scoring 6.9 are not two
degrees of the same result — they are 888 against 7.0.

Two other numbers belong in the same report, because the bearing test cannot see either:
`nnMin` (the closest pair in the set) and the fraction of points closer than half the effective
spacing. A set can be perfectly isotropic and still have points on top of each other.

## Superposition Fixes The Look And Destroys The Floor

Calling `scatter_layout` several times over one region at mutually incommensurate spacings and
different seeds, then merging the transforms, removes the row signature: the lattices have no common
row pitch to expose. Measured on one zone's real verb output:

| set | N | chi² | chi²@1k | nnMin | verdict |
|---|---|---|---|---|---|
| one lattice, spacing 700, jitter 0.18 | 188 | 167.0 | **888** | 413 cm | rows, overwhelmingly |
| one lattice, spacing 830 | 133 | 109.3 | 822 | 482 cm | rows |
| one lattice, spacing 980 | 96 | 94.0 | 979 | 564 cm | rows |
| two merged (700, 830) | 321 | 28.0 | 87 | 24 cm | borderline |
| three merged (700, 830, 980) | 417 | 21.9 | 53 | 24 cm | passes |
| eight merged | 753 | 20.9 | 28 | 10 cm | passes |
| thirteen merged + density mask + radius cull | 985 | 6.9 | **7.0** | 47 cm | isotropic |

The single-lattice histogram was `[17, 1, 0, 30, 37, 1, 3, 16, 35, 2, 8, 38]` — three bins at 0-1
counts. The as-built histogram was `[71, 75, 85, 92, 89, 87, 85, 88, 86, 74, 73, 80]` against an
expected 82.

**The `nnMin` column is the cost.** Merged sets measured 10-24 cm minima on a 700 cm nominal
spacing, and **50.2%** of merged points sat inside half the merged set's effective spacing — worse
than the 38.9% that `jitter: 0.5` produces on its own. Independent lattices have nothing preventing
two points coinciding. Superposition without a distance cull afterwards is not a complete answer,
and advice that recommends it without saying so is wrong.

## Culling Restores The Floor And Costs Isotropy

A greedy minimum-distance cull, coarsest lattice placed first, restores the spacing floor. It also
*raises* anisotropy, because the pairs it removes are the ones whose bearings were most random.
Measured on one unrotated four-lattice merge:

| minDistance | kept | chi²@1k | nnMin |
|---|---|---|---|
| none | 434/434 (100%) | 54 | 24 cm |
| 150 cm | 391 (90%) | 74 | 151 cm |
| 220 cm | 336 (77%) | 80 | 222 cm |
| 300 cm | 280 (65%) | 77 | 300 cm |
| 380 cm | 220 (51%) | 74 | 381 cm |

A radius-aware cull — reject a candidate overlapping an existing point by more than a fraction of
the sum of their radii, rather than by one absolute distance — keeps more of the set at the same
visual quality: one build kept 985 of 1084 candidates at a 25% overlap allowance.

## Rotation Is The Axis Superposition Cannot Reach

Every `scatter_layout` call rows along the same world axes and the verb has no rotation parameter,
so merging shifts row **phase**, never row **direction**. A residual anisotropy therefore survives
any number of merged lattices.

Rotating each lattice about the region centre before merging is what removes it. Measured:
0 / 23 / -41 / 11 degrees on a four-lattice merge took chi²@1k from **54 to 38.6**, with no other
change. Do it in the caller — sample the lattice over a region large enough to cover the rotated
footprint, rotate the points, then clip to the real region.

## The Density Mask Is What Makes It Composition

An isotropic scatter at uniform density reads as a texture. A smooth field thresholded over the same
points is what makes it read as terrain history. The field used on one build:

- ridged value noise, `1 - |2n - 1|`, two octaves, **sheared** so the ridges run across the slope
  rather than along an axis;
- multiplied by a slope term (0.45 on flat ground rising to 1.66 at 22 degrees);
- cut to a low constant on the valley floor and on smooth shoulders;
- **nothing placed at all below a 0.42 crest threshold** — the bare ground is the point, not a gap
  in coverage.

The result reads as debris tracked downhill rather than sprinkled. Carve clearings with the verb's
own `exclude` regions rather than by deleting instances, so the carve survives a re-scatter, and
carve a different radius per tier so a clearing gets a shrub rim instead of a hard edge.

## What The Verb Would Need

Two parameters would remove every hand-rolled step above: a Poisson-disc **`minDistance`** (or
Lloyd relaxation), which is the only way to get an organic look *and* a spacing floor from one call,
and a **`latticeRotation`**, which is the one axis superposition provably cannot reach. Neither
exists today.

## See also

- [`vegetation-authoring`](vegetation-authoring.md) — the workflow this measurement supports.
- [`spatial`](spatial.md) — `spatial.scatter_layout`'s parameters and guarantees.
- [`level-building.instancing-and-scatter`](level-building.instancing-and-scatter.md) — the
  jittered-lattice doctrine and why the jitter must be continuous.
