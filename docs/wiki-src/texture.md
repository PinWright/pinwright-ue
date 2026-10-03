# texture

Inspect, transform, and configure texture assets, plus create render targets, procedural patterns, font-rendered text and caller-supplied pixel images — compression, LOD bias, streaming, filtering, wrapping, and channel operations. Use this for texture asset data and settings; use `call("material")` when wiring textures into shader graphs or material instances.

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

The procedural creators share one helper, so the refusal on them arrives as
`TEXTURE_ERROR` with the engine's own reason quoted on the wire (and in the editor
log at Warning). `channel_extract` and `create_render_target` refuse with
`INVALID_ARGUMENT` and quote the reason on the wire.

## An existing asset at the output is refused

The same shared helper serves `create_noise_texture`, `create_gradient_texture`,
`create_pattern_texture`, `create_normal_from_height`, `resize_texture`,
`channel_pack`, `combine_textures`, and `invert` / `desaturate` / `adjust_curves`
when they write a new asset (`inPlace: false`). It refuses an output `<path>/<name>`
that already holds an asset — loaded or only on disk, of any class, an input of
the same call included — with `ASSET_ALREADY_EXISTS`, before anything is created.
An existing asset is never replaced, so pick a fresh name or delete the old asset
first. Each of these verbs checks its inputs before it creates the output, so a call
refused on an input leaves no output asset behind to block a retry under the same
name.

## See also

- [`asset`](asset.md) for the shared dump sidecar schema and dump-parity live read policy.

### texture.combine_textures

A **full-frame blend, not a compositor**. Every output pixel `i` is `blend(base[i], overlay[i])`
over one flat pixel index, so the parameter list is the whole interface: there is **no placement**
— no offset, origin, rect, anchor or scale. Both images are consumed from pixel 0 (top-left). A
stencil, logo or label cannot be stamped anywhere but the top-left corner at full size.

**Use two textures of the same dimensions.** The output is created at the **base's** source size,
and the loop is bounded by the smallest pixel count of base, overlay and output — nothing
reconciles the two row strides. On a mismatch the call still returns `success: true`:

- an overlay of a different **width** reads one scanline out of phase per output row, so the
  overlay lands as a diagonal smear, not a corner placement;
- an overlay with **fewer pixels** than the base stops the blend early, and the rest of the output
  is left **zero-filled (transparent black)** on UE 5.8, where the engine zero-fills a source
  created without data (unverified on 5.3-5.7) — it is not the base's pixels. Combining a small
  overlay into a large base discards the tail of the base.

**An existing asset at the output is refused with `ASSET_ALREADY_EXISTS`**, whatever its class,
loaded or only on disk, and including `baseTexture` or `overlayTexture` themselves: the inputs are
never touched. Both inputs' format checks run before the output is created, so a refused call
leaves no output asset behind.

**Overlay alpha is ignored.** Only R, G and B are blended; the output's alpha is
copied from the base. A transparent overlay region composites as if fully opaque.
`opacity` (clamped to 0-1) is a uniform scalar over the whole image and is the only transparency control:
`out.rgb = lerp(base.rgb, blend(base.rgb, overlay.rgb), opacity)`.

Other limits: both inputs must have an 8-bit BGRA (`TSF_BGRA8`) editable source — any other source
format is refused; the blend runs on the stored 8-bit bytes as-is, with no colour-space conversion
(the output is always flagged sRGB, whatever the inputs' flags); an
unrecognised `blendMode` is not refused, it falls through to `Normal` (the response `message` echoes
the name you sent). Check the result with [`texture.get_pixel_stats`](texture.get_pixel_stats.md)
(`region` / `tileGrid`) rather than trusting the success flag.

### texture.create_from_pixels

Creates a `BGRA8` texture from **your own pixels** — the general route for any shape, stencil, mask
or marking you rasterise yourself. `data` is base64 of raw bytes, row-major from the top-left, no
PNG or other container: `format: "RGBA8"` (default, 4 bytes R,G,B,A per pixel) or `"Gray8"` (1 byte
per pixel, written as R=G=B with alpha 255). The decoded length must be **exactly**
`width * height * bytesPerPixel`; anything else is refused with `INVALID_ARGUMENT` naming both counts
rather than padded or truncated. `width`/`height` are 1..4096, but the practical bound is the request
body limit (`HttpMaxRequestBodyBytes`, 1 MiB by default): about 440x440 RGBA8 or 880x880 Gray8 per
call. For a file already on disk use `asset.import` instead.

Bytes are stored as given — no colour-space conversion; the texture is sRGB like the other creators.
An existing asset at `<path>/<name>` is refused with `ASSET_ALREADY_EXISTS`, never replaced in place.
The response carries `pixelStats` (the [`texture.get_pixel_stats`](texture.get_pixel_stats.md) block
read back from the written source mip), so `hash` / `mean` confirm what landed.

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

### texture.create_text_texture

Renders `text` with a **real font** — any runtime `UFont` (composite font) or `UFontFace`, default
`/Engine/EngineFonts/Roboto.Roboto` — rasterised on the CPU by FreeType at `size` pixels (the em
size, 1..1024). Use it for labels, markings, serial numbers and stencil sources; it is not the 3x5
annotation font `image.annotate` draws.

- **Background:** `backgroundColor` defaults to fully transparent `{0,0,0,0}`. The text colour is then
  carried on every pixel and the glyph coverage is the alpha, so the texture is directly usable as a
  masked / translucent decal or as a stencil (sample A). An opaque `backgroundColor` gives a flat
  plate. Colours are `{r,g,b,a}` in 0..1, stored without a colour-space conversion, like the other
  creators.
- **Typeface:** `typeface` picks an entry of a composite font's default typeface (Roboto: `Regular`,
  `Bold`, `Italic`, `Bold Italic`, `Light`); default is the font's first entry, echoed back as
  `typeface`. An unknown name is refused with the available list. Offline (bitmap-cache) `UFont`s
  are refused — they carry no outline data.
- **Layout:** `'\n'` starts a new line; each line is aligned by `align` (`Left` / `Center` / `Right`),
  the block of lines is centred vertically. Kerning comes from the font's legacy `kern` table only
  (fonts that kern solely through OpenType GPOS draw unkerned); there is **no complex-script shaping**
  (Latin, Cyrillic, Greek and similar render correctly; Arabic or Indic do not). `text` is capped at
  4096 characters; longer is refused with `INVALID_ARGUMENT`.
- **Size:** omit `width`/`height` to fit the canvas to the widest line and the line block. A line's
  width is its advance plus any ink outside it (an italic overhang, a leading `j`'s negative bearing),
  and alignment places that whole box, so an auto-sized canvas loses no coverage horizontally. Pass
  `width`/`height` for a fixed canvas (1..4096). Glyph coverage that falls outside the canvas is counted, not
  silently dropped: `clipped` / `clippedPixels`.

Measured, not echoed: `inkPixels` and `inkBounds` (`{x, y, width, height}`, absent when nothing was
inked, e.g. all-space text) are read off the rendered coverage, and `missingCharacters` lists the code
points (`U+XXXX`) the font has no glyph for — those draw the font's `.notdef` box. As with
`create_from_pixels`, an existing asset is refused with `ASSET_ALREADY_EXISTS` and the response carries
`pixelStats`.

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
