# image

Working from a reference image: cut one into a georeferenced tile grid, draw world-space marks onto it, and place two of them side by side with a difference composite. Every coordinate entering and leaving this namespace is world centimetres, so nothing here asks you to convert to pixels by hand.

## Coordinates are world space. That is the whole feature

Hand-rolled pixel arithmetic against a reference image is where the error comes from. In one measured run, an eyeballed coordinate on a whole-map reference was off by **~1,188 world units** because the image was being read downsampled; cutting it into native-resolution tiles improved that by roughly **10x**.

The loop these verbs exist to close is: estimate a world position, draw it back onto the reference, **look at it**, correct. Nothing in this namespace judges the picture for you.

## The plugin renders, the reader judges

There is no thresholding, no feature detection and no heuristic anywhere in this namespace. Every verb is deterministic — crop, draw, blend — and every step writes an artifact you can open. That is a deliberate ceiling, not a missing feature: a verb that quietly decided "these two images match" or "the road is here" would be wrong invisibly, and a wrong answer you cannot see is worse than no answer.

- An overlay mark reports `drawn` only when the pixels under it actually changed, measured by hashing that mark's own rectangle before and after painting. An off-frame mark, and a mark whose colour equals what was already there, both report `drawn: false`.
- A comparison reports statistics — differing pixel count, per-channel mean and max absolute difference, best-fit gain — and never a verdict.
- Anything that cannot be represented exactly is refused rather than rounded. An image whose width does not divide by `cols` is `IMAGE_GRID_MISMATCH`, not a grid with a short edge tile.

## The georeference object, and why it carries no pixel size

All three verbs take the same `georeference`:

```json
{
  "axes": "top_down_x_right_y_down",
  "worldMin": {"x": -10000, "y": -5000, "z": 0},
  "worldMax": {"x":  10000, "y":  5000, "z": 0},
  "cols": 4,
  "rows": 2,
  "depthCm": 0
}
```

| Field | Meaning |
| --- | --- |
| `axes` | Which world axis runs across the image and which runs down it. A preset name — `top_down_x_up_y_right`, `top_down_x_right_y_down`, `front_y_right_z_up`, `side_x_right_z_up` — or the explicit form `{screenXAxis, screenXPositive, screenYAxis, screenYPositive}`. An unrecognized name is an **error**; there is no default, because a silently substituted mapping mirrors every coordinate you read and a mirrored map looks entirely plausible. |
| `worldMin` / `worldMax` | The world-space box the whole image covers, in centimetres. `{x,y,z}` or `[x,y,z]`. Given in either order per component. |
| `cols` / `rows` | The subdivision. **Required** — pass `1`/`1` for an untiled image. There is no default subdivision, because a defaulted one is a decision the response would have to report and you would have to notice. |
| `depthCm` | Optional. The coordinate on the axis perpendicular to the image, used for every pixel → world answer. Defaults to the midpoint of the box's depth span, and is reported on every response so it is never implicit. |

**There is no tile pixel size in there, deliberately.** Pixel size is always derived from the image actually being processed. That is what makes one georeference describe a downloaded reference and an in-engine capture of *different resolutions* — the normal case — without either being edited, and an edited georeference is an unverifiable one. It is also what lets [`image.compare`](image.compare.md) tell "different ground" (`GEOREFERENCE_MISMATCH`) apart from "different resolution" (`IMAGE_SIZE_MISMATCH`): those have different recoveries.

Getting `axes` right matters more than it looks. For an orthographic capture the engine **ignores the camera rotation** and derives the view from the viewport type, so which world axis lands on screen X is decided by the *effective* rotation the capture reported, never the requested one. Pitch −90 / yaw 0 gives +X up and +Y right; pitch −90 / yaw −90 gives +X right and +Y down. See [`level-review.framing-math`](level-review.framing-math.md).

## Pairing a reference with a capture, tile by tile

The manifest [`image.tile`](image.tile.md) writes carries, for every tile, a **standalone georeference** for that tile alone — a complete `{axes, worldMin, worldMax, cols: 1, rows: 1, depthCm}` object over just that tile's ground. That is the handle for the whole workflow:

1. Tile the reference on a georeference covering the map.
2. Tile the capture on the **same** georeference — same `axes`, same `worldMin`/`worldMax`, same `cols`/`rows`. Resolutions may differ.
3. Reference tile (r,c) and capture tile (r,c) now cover the same ground by construction, not by eye.
4. Feed one tile's `georeference` straight into [`image.annotate`](image.annotate.md) to mark a position on it, or into [`image.compare`](image.compare.md) as `georeferenceA` / `georeferenceB` to have the pairing checked rather than assumed.

`georeferenceFrom` takes a manifest path so step 2 needs no copy-paste, and `georeferenceFromTile: {col, row}` pulls one tile's georeference out of that manifest.

**Step 2 has a shortcut that skips the capture-then-tile round trip.** `render.capture_ortho_tiles` renders the level orthographically straight into that grid — one PNG per tile, plus a manifest of exactly this shape, so `georeferenceFrom` reads it verbatim in either direction. It also removes the two things that make a hand-driven whole-map capture unreliable: a wide orthographic viewport frame silently loses distance-culled geometry, and the viewport can only render six cardinal poses. See [`render`](render.md) for both, and for the rule that a scene-capture frame must never be diffed against a viewport frame.

## Overlays composite at low alpha, and labels stay on the edges

A prototype grid drawn at full opacity **buried the detail it was drawn to measure** — the reader could no longer see the thing the grid existed to help them locate. So the grid overlay defaults to `opacity: 0.25` and puts its coordinate labels on the frame edges rather than in the field.

Two crossing translucent lines are darker where they cross. That is the honest composite of two source-over blends and is deliberately not corrected; a "fix" would mean the overlay lies about how many marks are on a pixel.

**`opacity` on a crosshair or a box governs the geometry only.** Its label is drawn opaque, in the mark's colour: the stroke is deliberately translucent so it does not bury what it points at, and text at that alpha over arbitrary imagery is unreadable — an unreadable label wastes the coordinate it exists to carry. Grid labels are opaque for the same reason, and additionally ignore the grid's colour: they use the default white ink at the frame edges. A `labels[]` entry is the exception and applies `opacity` to its glyph ink, because there the text *is* the mark — but every label, that one included, sits on an opaque dark backing plate, so no label ever fades into the image behind it. The one thing that does fade a crosshair or box label is alpha carried by the colour itself: `#RRGGBBAA` multiplies into every paint the mark makes, ink included, so `#FF000080` gets a translucent label that the ignored `opacity` field could not have produced.

## Output paths

All defaults sit under `<ProjectSaved>/PinWright/image/`: `image.tile` writes to **`tiles/<namePrefix>/`** — a subdirectory per prefix, so two bursts cannot interleave their PNGs or overwrite each other's manifest — `image.annotate` to `annotated/`, `image.compare` to `compare/`. `namePrefix` defaults to the source image's base name. An explicit `outputDir` wins and is used verbatim: no `<namePrefix>` subdirectory is added under it. None of them writes into `Saved/Screenshots/`: that directory is where the capture verbs drop single frames for a human to scan, and a 64-file tile burst into it destroys that use.

`overwrite` defaults to `false` on every verb, and the refusal happens **before** anything is written, so a half-overwritten tile set is unreachable. What that rule cannot cover is a write that fails midway, and both cases are reported rather than tidied away:

- `image.tile` names, in the error, how many tiles it had already written when a later tile or the manifest failed.
- `image.compare` writes the side-by-side **before** the difference composite, so a failed difference write leaves `<prefix>_side_by_side.png` on disk. The error names it in the message and in `partialOutputs[]`, and reports no statistics for the incomplete pair. It is deliberately not deleted: deleting is an unannounced mutation that can itself fail, and under `overwrite: true` it would take the previous run's file with it. Delete it yourself or re-run with `overwrite: true`.

`image.tile` additionally reports `staleFiles` — files matching its own naming pattern in the output directory that this run did not write, i.e. leftovers from an earlier run at a different subdivision. It names them and never deletes them.

## Response fields

- **`warnings[]`** — `image.tile` and `image.annotate`. An array of plain strings, present only when non-empty. One condition raises it today: the georeference's world box does not match the image's aspect, so a distance measured across the image and one measured down it are on different scales. Reported and never refused, because a deliberately anamorphic reference is rare but real. In `image.annotate` tile mode it is also the one available tell that the file passed was the whole mosaic rather than the tile it was declared to be.
- **`stats.pixelCount`** — `image.compare`. Pixels in **one** image (`width * height`), not the sum of the pair; the two are dimension-matched before it is computed. It is the denominator behind `differingFraction` and the per-channel `meanAbsDifference`; `meanAbsDifferenceOverall` averages the three channels too, so it divides by `3 * pixelCount`.
- **`staleFileCount`** — `image.tile`. Always present, `0` when the directory is clean. The `staleFiles[]` array of absolute paths appears only when it is non-zero.

## See also

- [`render`](render.md) — capturing frames out of the editor in the first place, and the capture hazards (fixed capture size, pinned exposure) that decide whether two captures are comparable at all.
- [`level-review`](level-review.md) — reviewing a whole level, and what counts as visual evidence.
- [`spatial`](spatial.md) — measuring and placing actors once a reference has told you where something belongs.

### image.tile

Cut an image file into an N x M grid on a given georeference, writing every tile as PNG plus a manifest.

The slice is half-open per tile and its origin comes from the georeference's own tile-origin function, so the mosaic and the georeference cannot disagree about where a seam is: a pixel exactly on a seam belongs to the tile on its right / below, and the far image edge belongs to the last tile. Cut and reassemble is byte-lossless.

**Manifest** (`<namePrefix>_manifest.json`, `schema: "pinwright.image.tile/1"`):

| Field | Meaning |
| --- | --- |
| `georeference` | The whole-mosaic georeference, in exactly the shape every `image.*` verb accepts. |
| `grid` | Everything derived: `tilePixelWidth`/`tilePixelHeight`, `imageWidth`/`imageHeight`, `worldUnitsPerPixelAcross`/`Down`, `squarePixels`, the explicit axis mapping, `acrossAxis`/`downAxis`/`depthAxis`, `depthCm`. |
| `tiles[]` | Per tile: `col`, `row`, `file`, `path`, `pixelOriginX`/`pixelOriginY`, `pixelWidth`/`pixelHeight`, `worldMin`/`worldMax`/`worldCentre`, and a standalone `georeference` for that tile alone. |

Tiles are named `<namePrefix>_r<row>c<col>.png`, zero-padded to the grid's digit count so a directory listing sorts correctly.

**Refusals.** `IMAGE_GRID_MISMATCH` when the image does not divide into whole tiles — a partial edge tile would have a different world-units-per-pixel from its neighbours at exactly the seam a reader measures across, so it is refused rather than rounded away. `TILE_BUDGET_EXCEEDED` past 1024 tiles in one call: every tile is a separate encode and file write on the game thread with no job handle to cancel it, so an unbounded grid is an unobservable wedge. `ALREADY_EXISTS` when any target file exists and `overwrite` is false — checked for the whole set before the first write.

The response carries the per-tile array inline only up to 64 tiles, and says which happened via `tilesInResponse`; the manifest always has all of them.

### image.annotate

Draw a world-aligned grid, crosshairs, boxes and text onto an image, with **every position given in world coordinates**, and get back the pixel each one landed on.

**Whole image or one tile.** Omit `tile` and the supplied image is the whole mosaic. Pass `tile: {col, row}` and the supplied image is that tile of the georeference's grid — the same world coordinate then lands on the same ground it would on the full image, and a coordinate outside the tile is reported `inFrame: false` rather than clamped onto the edge. Every positioned mark reports its pixel in both spaces, so a tile reading feeds straight back into a full-image one: crosshairs and `labels[]` carry `pixel` and `mosaicPixel`, boxes carry `pixelMin`/`pixelMax` and `mosaicPixelMin`/`mosaicPixelMax`. Without `tile` the two spaces coincide and the pairs are equal. The offset between them is reported once, as `canvasOriginInMosaic`. The grid overlay reports counts (`linesAcross`, `linesDown`, `labelsDrawn`), not pixels.

**Marks.**

- `grid: {spacingCm, color, opacity, thicknessPx, labels}` — world-aligned lines across the frame. `spacingCm` is required and must be positive. `opacity` defaults to `0.25`; labels ride the top and left edges and are skipped where they would overlap the previous one. `GRID_TOO_DENSE` past 512 lines on an axis — the count is computed and compared in double, so a `spacingCm` small enough to overflow an `int32` line count still refuses instead of drawing zero lines and calling it a success.
- `crosshairs: [{x, y, z?, label?, color?, opacity?, sizePx?, thicknessPx?}]`
- `boxes: [{min, max, label?, color?, opacity?, thicknessPx?}]` — `min`/`max` are world points; the box is axis-aligned in the capture plane.
- `labels: [{x, y, z?, text, color?, opacity?, scale?}]`

Colours are `#RRGGBB`, `#RRGGBBAA`, or one of `white black red green blue yellow cyan magenta orange grey`. An unknown colour is an error, not a fallback. An empty mark set is `NOTHING_TO_ANNOTATE` rather than a byte-identical copy reported as an annotation — otherwise a typo in an overlay key produces a plausible-looking no-op.

**`snapCm`** snaps every mark's world point to a world grid before projecting (nearest multiple, ties toward +infinity), and the response reports the requested and the snapped point separately. It must be positive when supplied: a non-positive grid size passes straight through the snap untouched, which would report snapped coordinates that were never snapped.

**What `drawn` means.** Each mark's rectangle is hashed before and after painting, so `drawn` is an observation that pixels changed rather than an echo of the request. `pixelsChanged` counts, across the whole image, how many pixels differ from the source. A response with `marksDrawn: 0` and a success status is telling you the marks are off-frame or invisible, not that the call failed.

### image.compare

Write a side-by-side and a per-channel difference composite for two images, with measured statistics.

**It reports; it does not judge.** The difference image is `abs(a - b)` per channel, multiplied by `amplify` and clamped — no threshold, no verdict. `stats` carries `differingPixels`, `differingFraction`, per-channel `meanAbsDifference` and `maxAbsDifference`, `meanAbsDifferenceOverall`, `identical` (measured as the differing count reaching zero), and `bestFitGain`.

`bestFitGain` is the least-squares scalar that best maps B onto A, per channel — `sum(A*B) / sum(B*B)` over every pixel. **A channel is omitted from the object when that denominator is zero**, i.e. imageB is 0 in that channel at every pixel: no scalar maps a black channel onto anything, so the field is left out rather than reported as 0 or 1, which would be a number nobody measured. Read a missing channel as "not measurable", never as "no change" — and note the numerator is not consulted, so a channel that is black in B is dropped whatever A holds.

It is there because a luminance-shaped difference can be entirely explained by exposure: auto-exposure is a scalar gain over the whole frame, so a real content change survives dividing the gain out while an exposure difference collapses, and a pair whose gain is ~1.000 proves the exposure never moved. Pin exposure before capturing anything you intend to compare — see [`render`](render.md).

**Refusals, and why they are two codes.** `IMAGE_SIZE_MISMATCH` when the pixel dimensions differ: no pixel of one corresponds to a pixel of the other, so the difference image would be a picture of the misalignment. `GEOREFERENCE_MISMATCH` when both georeferences are supplied and they describe different ground; the error names the first field that differs and carries both objects. Two images can legitimately be the same ground at different resolutions, so these must not be collapsed — the recoveries are "re-capture at a matching size" and "re-derive the georeference". Supplying only one of `georeferenceA` / `georeferenceB` is `INVALID_GEOREFERENCE`: a one-sided georeference proves nothing about whether the pair is comparable.

Georeferences are optional. Without them the verb still compares pixels and still refuses mismatched dimensions; it simply cannot tell you the two frames are the same ground.

**A failed write is not clean.** The side-by-side is written first, so a failed difference write leaves `<prefix>_side_by_side.png` on disk with no difference image beside it and no statistics reported. The error names that file in the message and in `partialOutputs[]`; it is never deleted for you.
