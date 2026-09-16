# camera

Give an agent multi-angle "eyes" on the scene by posing the live Level Editor viewport and reading
back a PNG. Use this namespace for framed actors and orbit sets; use `call("render")` for raw poses
or overlays, and `call("spatial")` to turn pixels back into world coordinates.

## Three verbs, one live viewport

All three verbs move the **real** editor camera, read the active Level Editor viewport, and restore
the camera on scope exit. They need a GPU-backed active viewport; headless / `-unattended` use
returns typed `NO_ACTIVE_LEVEL_VIEWPORT` or `CAPTURE_FAILED`, not a blank image. Each shot flickers
the live viewport as it re-poses.

The three verbs differ as follows:

| | `frame_actor` | `orbit_shots` | `animation_shots` |
| --- | --- | --- | --- |
| Images per call | one | a shot set | instants × angles |
| `subject` | yes | yes | yes |
| `exposure`, read once for the whole call | yes | yes | yes |
| `viewMode`, scoped and restored | yes | yes | yes |
| `distribution` / `seed` | — one shot has no distribution | yes | yes |
| `hideEditorSprites` | yes | yes | yes |
| Time axis | — | — | a Level Sequence, or a subject that carries one |

`camera.animation_shots` now also honours `hideEditorSprites` and reports the measured
`viewport.editorSprites` block. `viewMode` behaves the same way. Both are read **once for the whole
burst**, like `exposure`.

## Orthographic capture

Orthographic shots use a different engine path: `FEditorViewportClient::CalcSceneView` hard-codes
the rotation per viewport type and reads only `ViewOrigin` from the camera. Arbitrary tilted poses
are snapped to one of six axis-aligned views; the response reports `orthoAxisSnapped`, `orthoView`,
and the `requested*` values.

`camera.orbit_shots` reaches this path through `projectionMode:"orthographic"` on `count` /
`angles` shots or `views:"sides"`, which plans all six axes at once.

All six axis-aligned views render scene geometry. `elevation` near `90` gives the distortion-free
plan view with a constant pixel-to-world mapping; `elevation` 0 gives `front` / `left` / `back` /
`right`. Verified 2026-08-13 on UE 5.8.

Expect a **dark** background, not perspective sky: orthographic lit views drop fog and draw the
reference grid, world-axis lines, and scale bar. Edge-on elevations can leave a mostly empty frame
around a thin band: a 3,000 uu-tall level at `orthoWidth` 92,000 occupies about 40 px of 1024.
That is correct framing.

Treat an orthographic capture as broken only when its PNG is uniformly one colour with **no** gizmo,
**no** grid, and **no** scale bar, and is much smaller than a perspective shot (20-30 KB versus
500 KB+). Re-verify with `camera.orbit_shots {point:{x:0,y:0,z:0}, radius:40000, width:1024,
height:1024}`: shots 1, 2, and 3 are orthographic and should all render the scene.

**A near-white frame with gizmo and scale bar is the older transparency fault, not a failed
render.** Before the opaque-alpha fix, the back-buffer alpha was 0 over scene pixels and 255 only
under Slate overlays, making the PNG ~99.97% transparent even though RGB was present. A viewer that
composites alpha showed only overlays; one that removes alpha showed the scene. Check the file's
**alpha channel** before calling it failed, then update the plugin. PNG-producing capture verbs
stamp opaque; the last gaps, `widget.screenshot_designer` with `target: "preview"` and the widget
aspect of `asset.dump`, closed on 2026-08-21. The old "every path" wording was wrong while those
two shipped raw alpha. Non-picture paths deliberately do not stamp: `render.detect_z_fighting`
returns depth/normal measurements, and its z-fighting mask is built opaque rather than read back.

## Captures inherit the viewport's view mode, unless you ask for one

All three verbs capture the live Level Editor viewport in its current view mode. Each response has
`viewport`; `lit:false` plus `viewModeWarning` means the frame cannot support a
material/lighting/colour judgement. See [`render` → Check the view mode before you trust a capture](render.md).

`viewMode` now **applies** for the set and restores both previous view-mode slots after the last shot.
It was formerly an echoed label; unrecognised values now error. `front_back_face` checks an
inside-out shell and complements `health.signedVolume`; useful alternatives are `unlit`,
`random_color`, `zebra`, and `clay`. Vocabulary, refusals, and measured
`viewport.viewModeOverride`: [`render.view-modes`](render.view-modes.md).

## Spread a shot set over the sphere, not around a ring

`count` puts shots at evenly spaced azimuths on **one horizontal circle** at `elevation`:
`distribution: "ring"` remains the default and misses the top and underside. That hid defects such
as a detached head, a grazing-elevation self-intersection, and a door on an unreached face.

`distribution: "sphere"` spreads `count` over the viewing sphere with a deterministic golden-angle
(Fibonacci) spiral: near-optimal coverage for any N, with no two shots on one great circle. It
**ignores** `elevation`; each elevation comes from the index and
`shotDistribution.elevationIgnored` says so.

There is deliberately **no random mode**: random sphere points clump, destroying coverage and
reproducibility. Use `seed` to vary a set; it applies a seeded azimuth offset, preserves the same
poses for the same seed, and returns the seed in `shotDistribution.seed`. Without it, output is
deterministic.

Every shot reports used `angle.azimuth` / `angle.elevation` and requested
`angle.requestedAzimuth` / `angle.requestedElevation`, so a defect can be re-shot with the exact
pose via `angles` rather than inferred from a filename.

## Hold the capture size fixed across a session

**Varying capture resolution call-to-call killed an editor and cost 66 actors and 125 emitters of unsaved level state**: `Assertion failed: ProxyMap.Num() == TestSizeX * TestSizeY` in
`FViewport::GetHitProxy()`, from a cursor query against a stale hit-proxy buffer after resize.
Constant-size resizing is proven safe (48 cycles, zero asserts); changing size is the hazard.

`camera.animation_shots` resolves and holds one size for the burst; hand-varied `orbit_shots` and
`frame_actor` sequences do not. Pick one size per session and re-shoot the set instead of resizing
mid-sequence. Detail: [`render`](render.md).

## Pin exposure across a set, not per shot

Auto-exposure re-balances an orbit between frames, so subject differences can become brightness
differences. Pass `exposure: -1` or `{mode: "fixed", ev100: -1}` to pin it for the set and restore
the viewport afterwards. **The usable band is scene-dependent**: read
`imageStats.meanLuminance`, and check `crushed` / `rangeWarning`. The former example `11` is beyond
the asset-preview fixture's range and returns black at `pinned: true`
(`B-exposure-pin-black-frame`).

Exposure is read **once for the whole set**, above the shot loop. For `camera.animation_shots`, that
keeps a pose difference between instants from reading as brightness difference.

Check measured `viewport.exposure.pinned`: it is `false` with `pinWarning` when a debug view mode
forced auto-exposure. Full contract: [`render.capture-exposure`](render.capture-exposure.md).

## Hide the editor icon sprites on an acceptance set

`hideEditorSprites: true` clears `EngineShowFlags.BillboardSprites` and restores it afterwards, so
light bulbs, audio icons, and the directional-light arrow stay out of frame. It defaults to `false`
for compatibility and, like `exposure`, is read **once for the whole set**. It does not touch
selection outlines, the grid, editor-mode handles, or component visualizers; use
`editor.set_game_view` for those. Response and mechanism: [`render`](render.md).

All three verbs take it.

## Name the subject, not only an actor

Every verb takes an optional `subject` naming a level, placed actor, static mesh, skeletal mesh,
animation asset, or Niagara system. Orbit and six-side plans therefore work against asset previews
as well as levels. Existing `actorName` and `point` + `radius` spellings still normalize into a
subject.

The response returns `subject` with resolved `kind`, camera-solve bounds, and `boundsSource` naming
how those bounds were obtained. **It is omitted entirely when no subject was resolved**, never
emitted empty; assert absence rather than zeroed fields.

Descriptor, per-kind bounds rules, time-axis refusals, and particle reproducibility:
[`render.capture-subjects`](render.capture-subjects.md) and [`render.capture-time`](render.capture-time.md).

An asset subject resolves to an asset-editor preview scene, so `camera.frame_actor` and
`camera.orbit_shots` accept `previewScene` **only for an asset kind**. Its key-light aim, intensity,
colour, sky, floor, and backdrop are scoped to the call and read once per set. It is refused with
`UNSUPPORTED_ASSET_EDITOR` for `world` or `actor` subjects, which use level actors for lighting.
`camera.animation_shots` is level-only and takes none. See
[`render.preview-scene-rig`](render.preview-scene-rig.md).

**`subject.radius` is camera distance for `camera.orbit_shots` but a fit sphere radius for
`camera.frame_actor`**: the same `R` places the camera at `R` versus roughly `2.5·R` at fov 50.
Both expose `padding` for margin and `radius` / `distance` for an explicit distance that skips fit.

## See also

- [`render.capture-subjects`](render.capture-subjects.md) — the `subject` descriptor shared by every capture verb, and what each kind can and cannot do.
- [`render.capture-time`](render.capture-time.md) — the time axis a subject is driven over, and what a particle capture promises between two runs.
- [`render.capture-exposure`](render.capture-exposure.md) — the full exposure contract behind `viewport.exposure`.
- [`render.view-modes`](render.view-modes.md) — the `viewMode` vocabulary and its four refusal codes.
- [`render.preview-scene-rig`](render.preview-scene-rig.md) — the `previewScene` parameter on an asset subject, and the rig a preview frame was drawn under.
- [`visual-review`](visual-review.md) — the guide to picking a capture surface and proving visual state after an edit.
- [`level-review`](level-review.md) — comparable repeat passes, whole-map framing, and multi-range proportion checks.
- [`render`](render.md) — raw camera-pose captures, asset-preview shots, and annotated overlays.
- [`spatial`](spatial.md) — turning what a shot shows back into world coordinates.

### camera.frame_actor

Frame one actor at an azimuth/elevation and capture the live viewport as a PNG fitted to its bounds.

Args:

- `actorName` (string; aliases `objectPath` / `actorPath` / `actor_name`) — actor to frame. Supply
  this **or** `subject`; both give `INVALID_ARGUMENT` naming both keys. The handler owns this
  refusal because the dispatcher can validate only one of the two target slots.
- `subject` (object) — `{kind, path}` for a static mesh, skeletal mesh, animation asset, or Niagara
  system; `{point, radius}` for a bare world point. `actorName` normalizes to `{kind:"actor"}`.
  See [`render.capture-subjects`](render.capture-subjects.md). An asset subject opens its preview
  editor; `subject.captureSource` identifies the viewport that drew the pixels.
- `azimuth` (number, default `45`) — horizontal orbit angle in degrees.
- `elevation` (number, default `30`) — vertical angle above the horizon in degrees.
- `padding` (number, default `1.15`) — bounds-fit margin multiplier; `>1` pulls the camera back.
- `fov` (number, default `50`), `width` / `height` (number, default `768`), and `projectionMode`
  (string, default `"perspective"`). Orthographic mode **snaps azimuth/elevation to the nearest
  cardinal world axis**; the response returns the used angles plus `orthoAxisSnapped`,
  `requestedAzimuth`, and `requestedElevation`.
- `inline` (boolean, default `false`) — also embed base64 PNG bytes.

```js
call({
  path: "camera.frame_actor",
  args: { actorName: "Statue_1", azimuth: 45, elevation: 30 }
})
```

Beyond PNG fields, the response carries `resolutionSource` and `poseSet`, plus `framing` and
`subject` only when resolved. `resolutionSource` is `caller` when a size was passed, otherwise
`default`; the omitted-size default is **768 × 768**.

**One call is three frames.** The shared pose-list primitive takes a throwaway warm-up, the real
frame, then a discarded repeat of the same pose. The last pair feeds
`poseSet.poseRepeatability {meanAbsDelta,maxDelta,changedPixelFraction}` so render-state drift is
measured rather than assumed. `poseSet.warmupShotTaken` / `warmupShotDiscarded` report the warm-up;
`poseSet.warmupFileNotDeletedWarning` names a leftover when deletion failed.

Gotchas: it needs a live active level viewport (headless → `NO_ACTIVE_LEVEL_VIEWPORT` /
`CAPTURE_FAILED`), moves and restores the real editor camera, and leaves `inline` base64 off by
default. Prefer the returned `path` unless bytes on the wire are required.

### camera.orbit_shots

Capture a shot set around an actor, asset subject, or bare world point.

Args:

- `actorName` (string; aliases `objectPath` / `actorPath` / `actor_name`) **or** `point` (object
  `{x,y,z}`) **or** `subject` (object) — provide exactly one. Combining `subject` with either
  other target gives `INVALID_ARGUMENT` naming both keys.
- `subject` (object) — `{kind, path}` for a static mesh, skeletal mesh, animation asset, or Niagara
  system. `actorName` and `point` + `radius` normalize into it. An asset subject uses its preview
  editor, allowing `views:"sides"` and `count` orbits against an asset. See
  [`render.capture-subjects`](render.capture-subjects.md).
- `views` (string) — canned plan. `"sides"` captures the **six axis-aligned views** (front / back /
  left / right / top / bottom), orthographic unless `projectionMode` says otherwise. It cannot be
  combined with `count` or `angles` (`INVALID_ARGUMENT`).
- `count` (number) — evenly-spaced shots; omit for the 4-shot canonical set.
- `angles` (array) — explicit poses, each `{azimuth, elevation}` in degrees; takes precedence over `count`.
- `elevation` (number, default `30`) — default elevation for `count` / `angles` shots.
- `projectionMode` (string, default `"perspective"`) — projection for `count` / `angles`. In
  `"orthographic"` mode each pose first snaps to the nearest cardinal axis (see
  [Orthographic capture](camera.md#orthographic-capture)); a moved pose reports `orthoAxisSnapped`,
  `requestedAzimuth`, and `requestedElevation`.
- `radius` (number) — orbit radius / camera distance. Precedence: top-level `radius`, then
  `subject.radius`, then a distance solved from subject bounds; **required** for bare `point`.
- `fov` (number, default `50`), `width` / `height` (number, default `1024` for existing `count` / `angles` / canonical shapes; `640` when `views` is used and neither is given - see Resolution below).
- `distribution` (string, default `"ring"`) — how `count` spreads shots: `"ring"` (even azimuths
  at one elevation) or `"sphere"` (golden-angle spiral over the sphere, ignoring `elevation` and
  reporting that fact). Only affects `count`.
- `seed` (number) — jitter the distribution by a seeded azimuth offset. Deterministic with or without it; the seed is echoed back so any set is reproducible.
- `viewMode` (string) — render every shot in this mode, then restore both previous slots. Read
  **once for the whole set**. Unknown or unrenderable modes error rather than echo; measured
  `viewport.viewMode` / `viewport.lit` describe the pixels and `viewport.viewModeOverride` gives the
  applied/restored verdict.
- `inline` (boolean, default `false`) — also embed base64 per shot.

Shot-plan precedence: `views` (exclusive) | `angles` > `count` > canonical set.

```js
// Six orthographic sides of one actor, in one call.
call({
  path: "camera.orbit_shots",
  args: { actorName: "SM_Statue", views: "sides" }
})
```

`views:"sides"` replaces six separate `camera.frame_actor` calls and viewport flickers. Its poses
land exactly on cardinal axes, so none reports `orthoAxisSnapped`; each has a different `orthoView`
(`front` / `back` / `left` / `right` / `top` / `bottom`).

**Resolution.** Unsized `views:"sides"` uses **640** on the long edge; existing `count`, `angles`,
and canonical calls keep **1024**. `resolutionSource` is `"caller"`, `"budget"` (sides), or
`"default"` (legacy orbit); explicit `width` / `height` wins. Other capture verbs use the shared
**768 x 768** default. These orbit sizes are compatibility guarantees.

```js
call({
  path: "camera.orbit_shots",
  args: { actorName: "Prop_Table", width: 1024, height: 1024 }
})
```

`camera.orbit_shots` uses the shared pose-list primitive, so camera application, warm-up discard,
exposure pin, scoped view mode, sprite hiding, and aim verification match the other verbs. `poseSet`
reports `posesRequested` / `posesCaptured` / `posesTruncated`, warm-up status, and one final
same-pose repeatability comparison; each shot has its own `framing` verdict. Many keys are conditional—test for absence:
[`render` → A set reports its own shape, and omits what it cannot say](render.md).

Gotchas: omitting `views`/`count`/`angles` gives **4 shots** (3/4 perspective plus front / side /
top orthographic); use `views:"sides"` for six. The call **fails whole** on the first capture
failure. More than 24 planned shots gives `TOO_MANY_SHOTS`. Bare `point` requires `radius`.
Each shot flickers the live viewport and the camera self-restores. The result is
`{shots:[{angle, projectionMode, path, ...}], count}`; orthographic shots also carry `orthoView`
and the displayed `cameraRotation` (the top shot reports `yaw:180`).

### camera.animation_shots

Capture N Level Sequence instants crossed with M camera angles and prove numerically whether the
**pose** changed.

It scrubs the Sequencer playhead; it never plays. One camera, one capture size, and one exposure
apply to the burst, making frames comparable: a pose difference cannot be a brightness difference
or a re-fitted camera.

`subject` names what is framed and time-driven; `actorName` normalizes to `{kind:"actor"}` and an
asset subject uses that asset's preview editor. `sequencePath` is a separate time source, not part
of `subject`, and remains required for `world` or `actor` because scrubbing it evaluates a bound
skeletal mesh in the editor. A static-mesh subject refuses with `UNSUPPORTED_ASSET_EDITOR` naming
the missing time axis instead of returning identical stills. See
[`render.capture-subjects`](render.capture-subjects.md).

**Read `poseChanged` before the images.** It is the verb's verdict; a `false` has three distinct
causes:

| Response field | Meaning |
| --- | --- |
| `poseChanged` | `true` when any sampled bone set moved against its predecessor **or** the first instant. Both comparisons catch a pose that moves and returns or drifts only slightly. |
| `poseSampled` | `false` means no component-space bone transforms were reported at *any* instant: the animation system never evaluated. "Could not measure" is not "measured clean" and adds a `warnings[]` entry. |
| `actorTranslationCm` | Largest component translation. `poseChanged:false` with a large value means **sliding in bind pose**: a transform track has no skeletal animation track. The warning names `sequencer.list_tracks`. |
| `frames[]` | Per instant: `index`, `frame`, `time`, `poseSampled`, `boneCount`; after the first, `poseDeltaFromPrevious` and `poseDeltaFromFirst`, each `{comparable, maxBoneTranslationCm, maxBoneRotationDegrees, movedBoneCount, componentTranslationCm}`. |
| `frameCount` / `viewCount` / `framePlanSource` | Burst shape and instant-list source. `count` is `frameCount × viewCount`, the image count. |
| `shots[]` | One entry per image: `frameIndex`, `frame`, `time`, `viewIndex`, `angle` `{azimuth, elevation}`, `path`, `filename`, `width`, `height`, `sizeBytes`, `projectionMode`, measured `cameraLocation` / `cameraRotation`, `fov` or `orthoWidth` + `orthoView`, `renderer`, `mimeType`, `imageStats`, `blank`, and measurable `framing`. `orthoAxisSnapped` + requested angles appear only after snapping. |
| `blankShots` | Count of near-uniform black images. A blank set otherwise reads as success; non-zero also raises a warning. |
| `framedCenter` / `framedRadius` / `cameraDistance` | Bounds solved from the **union** of the actor's bounds across sampled instants, so a walking actor stays in frame. |
| `width` / `height` / `resolutionSource` | `caller` for an explicit size; otherwise `budget`, `singleStill`, or `default`, all currently 768 × 768. Resolved **once**; changing size within a session is the hazard noted above. |
| `displayRate` / `tickResolution` | Sequence rates; `frames[]` numbers use display rate. |
| `updateMethod` / `forcedUpdate` / `opened` / `pausedPlayback` / `restoredPlayhead` | Measured Sequencer actions. An unhonoured requested `updateMethod` returns the applied method plus a warning. |
| `viewport` | `lit`, `aim`, `warmup`, `exposure`, `viewModeOverride`, `editorSprites`; view mode is reported once at top level and `viewModeWarning` is mirrored into `warnings[]`. |
| `poseSet` / `shotDistribution` / `subject` | Shared blocks. `subject` appears only when resolved; `poseSet` has conditional keys (see [`render` → A set reports its own shape](render.md)). Each shot also has measurable `framing`. |
| `partial` / `captureError` | Present only after capturing something and then failing. |
| `warnings[]` | Present only when non-empty; read every verdict that the set proves nothing. |

**A half-captured burst succeeds with `partial: true`, not an error.** Only zero captured shots
errors; after a partial failure the response has `partial: true`, `captureError`
`{code, message, failedAtShotIndex}`, and a warning beginning `PARTIAL SET:`. Existing shots are
usable, but a missing angle or instant is a gap, not a subject finding: branch on `partial` first.
(`camera.orbit_shots` did **not** change: it still fails whole on the first failed shot.)

**Sub-frame instants are rounded onto whole display frames.** Display-rate/tick-resolution remainder
can move samples off-grid, so each is rounded to the sequence grid; a warning counts moved samples,
and `frames[i].frame` / `frames[i].time` report the photographed instant.

**A `world` subject has no pose evidence, and the response says so rather than zeroing it.**
`actorName`, `poseSampled`, `poseChanged`, `actorTranslationCm`, and per-frame pose fields are
**omitted entirely** with a warning: the burst framed the level, so no bone transforms were
measured. Images show each instant but do not establish animation. Pass
`subject:{kind:"actor"}` or `actorName` for the verdict.

Gotchas: it captures the **level** viewport; pass `hideEditorSprites: true` to suppress editor icon
sprites. No actor target and no `subject:{kind:"world"}` gives handler-body `INVALID_ARGUMENT`
naming `actorName`, `objectPath`, `actorPath`, `actor_name`, and the world form (formerly the
dispatcher named only `MISSING_REQUIRED_PARAM`). The playhead restores by default;
`restorePlayhead:false` leaves it at the end. `open:false` refuses with `SEQUENCE_NOT_OPEN`.
