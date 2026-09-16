# texture

Inspect, transform, and configure texture assets, plus create render targets and procedural patterns — compression, LOD bias, streaming, filtering, wrapping, and channel operations. Use this for texture asset data and settings; use `call("material")` when wiring textures into shader graphs or material instances.

## Generic texture read

`texture.describe` is the canonical dump-parity live read for texture assets. It accepts required `assetPath` and loads the asset as `UTexture`, so it covers `UTexture2D`, `UTextureCube`, texture arrays, volume textures, and render targets. The result matches the `asset.dump` `texture.json` sidecar shape, including `kind`, `textureClass`, `size`, `arraySize`, `pixelFormat`, compression, LOD, sRGB, mip generation, streaming, and editor source fields where available.

`texture.get_texture_info` is legacy and intentionally remains Texture2D-shaped under `textureInfo`. Keep using it only for older callers that depend on its `width` / `height` / `mipCount` response shape.

## Creating cube / volume / array textures

The `create_*` verbs here (`create_render_target`, `create_noise_texture`,
`create_gradient_texture`, `create_pattern_texture`, `create_normal_from_height`)
build real 2D / render-target assets. There is no verb for
authoring a cube map, volume texture, or texture array from scratch — those
classes are produced outside this namespace:

- **Cube maps** — import an HDR / `.hdr` longlat or cross-layout source through the
  editor's content import (or an `AssetTools` file import); that produces a
  `UTextureCube` directly.
- **Volume textures** — assemble from a VDB / EXR slice sequence, or from 2D source
  slices, via the engine's volume-texture build path.
- **Texture arrays** — assemble a `UTexture2DArray` from multiple existing 2D
  source textures in the editor.

`texture.describe` still *reads* `UTextureCube`, texture arrays, and volume
textures (see above); describe coverage is genuine. Only the from-scratch create
verbs for those classes were never real, and they have been removed rather than
left as failing stubs.

## `name` is a bare asset name, `path` is the folder

Every verb here that writes a new texture — the procedural creators, the processing
verbs that write to a new asset, `channel_extract` (`name` + `outputPath`) and
`create_render_target` (`name` + `path`, or one combined `renderTargetPath`) —
validates the leaf against the engine's own object-naming rules
(`FName::IsValidXName` / `INVALID_OBJECTNAME_CHARACTERS`) and the composed
`<path>/<name>` against `FPackageName::IsValidLongPackageName`. A path-shaped
`name`, a `\`, a `..`, a trailing slash on the name, or an unmounted root is
refused rather than written; a folder that already ends in `/` is still accepted.

The reason is not naming hygiene. A `name` containing `//` used to reach
`CreatePackage`, which logs that at **Fatal** — a verbosity that is not compiled
out in any configuration — so the call did not fail: the editor **process** died,
taking every unsaved package in it with it. `SanitizeAssetName` did not catch it:
`/` is not in its invalid-character list. The same defect was measured end-to-end
on `foliage.add_type`; see that verb on the [`foliage`](foliage.md) page.

The procedural creators share one helper, so the refusal on them arrives as the
generic `Failed to create texture`, with the engine's own reason for the refusal
in the editor log at Warning. `channel_extract` and `create_render_target` refuse
with `INVALID_ARGUMENT` and quote the reason on the wire.

## See also

- [`asset`](asset.md) for the shared dump sidecar schema and dump-parity live read policy.

### texture.create_noise_texture

Two algorithms, selected by `noiseType`: `Perlin` (smoothed value-noise FBM) and
`Worley` — spelled `Voronoi` if you prefer — a cellular F1 distance field, dark at
the feature points and bright along the cell boundaries. Any other name is refused
rather than substituted; the resolved name comes back on the response as `noiseType`.

`seamless: true` makes the output tile exactly by wrapping the noise lattice at the
texture edge. Because a lattice only repeats on whole cells, `scale` snaps to a whole
cell count (and each octave to its own integer period); the value actually used is
reported as `effectiveScale`. A seamless tile needs `scale` of at least 2 for the base
octave to vary at all — one cell across the tile is, correctly, flat.

`octaves` must be between 1 and 16. `0` is refused rather than read as "flat": an FBM with no octaves
has no accumulated amplitude to normalise by, and the 0/0 that produced was clamped into a plain white
image and reported as a success. The upper bound is where an octave can no longer change a pixel.

`persistence` must be between 0 and 1. A negative value is refused because it alternates the sign of
the octave amplitudes, and on an even octave count they cancel to exactly zero — the same 0/0, clamped
into the same plain white image reported as a success. Above 1 the amplitudes grow instead of falling
off, and enough of that overflows the accumulation to infinity, which normalises to the same NaN.

`hdr: true` produces a linear `RGBA16F` source written as half-floats, with `SRGB` off and `TC_HDR`
compression; `hdr: false` (the default) produces the sRGB `BGRA8` byte texture. The format the pixels
were actually written in comes back on the response as `hdr` — read off the source, not echoed from
the request.

### texture.get_pixel_stats

Reads the **editable source mip** (`FTextureSource`), not the streamed platform data, so the
answer is the authored pixels and needs no GPU readback. Only `TSF_BGRA8` and `TSF_G8` sources
are decoded; any other source format is refused with `PIXEL_STATS_UNAVAILABLE` rather than
misread through the wrong byte layout. `hash` is a CRC32 of the whole mip buffer — compare two
reads to answer "did the pixels change at all".

`region` and `tileGrid` make the answer addressable below the whole image. A whole-image mean is
close to meaningless on a tiled authoring texture — a SubUV flipbook atlas, a sprite sheet, a
row-banded AO or mask sheet — where every real question is about one cell, and answering those
outside the plugin costs a hand-written PNG decode over a stale editor thumbnail.

- `region: {x, y, width, height}` — in the read mip's own pixels. `x`/`y` default to 0; `width`
  and `height` are required. The rect is **clamped** to the mip and the clamped rect comes back
  as `region`, so a caller sees what was actually measured. An origin *outside* the mip is
  refused rather than clamped to nothing: "0 pixels, mean 0" is how a wrong `x`/`y` passes for a
  measured answer.
- `tileGrid: {columns, rows}` — splits the measured area into `columns` x `rows` tiles and
  returns one stats block per tile in `tiles`, **row-major** (left to right, then top to bottom),
  each carrying `column` / `row` / `x` / `y` / `width` / `height` / `pixelCount` plus the same
  `mean` / `min` / `max` / `maxChannelSpread` / `grayscale` fields as the top-level block. Tile
  boundaries are computed from the running fraction, so an area that does not divide evenly still
  covers every pixel exactly once. Capped at 4096 tiles; a grid finer than the pixels it would
  cover (more columns than pixels wide) is refused rather than emitting empty tiles.

The two compose: `tileGrid` with `region` tiles the sub-rectangle. The whole-image block is always
present beside `tiles`, so one call answers "what is the sheet" and "what is in cell 9" together.
`width` / `height` stay the **mip's** size whatever `region` asked for — a caller needs to know
what the rect was clamped against — while `pixelCount` is always the pixels the stats cover.
`hash` likewise always covers the whole mip, so its meaning never depends on the request shape.

With neither argument the response is unchanged from the whole-mip-only verb: `region`, `tileGrid`
and `tiles` are absent, not empty.

A row-banded sheet is `{columns: 1, rows: N}`; a square flipbook atlas is `{columns: N, rows: N}`.
`niagara.validate`'s SubUV atlas check measures through the same code path — see
[`niagara`](niagara.md).
