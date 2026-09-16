# showcase-video

How to produce the footage a showcase video is cut from: which shots the offline render pipeline
owns, and which a stepped capture can still do. Pairs with
[`cinematic-flythrough`](cinematic-flythrough.md), which owns the camera path itself; with
[`mrq`](mrq.md) for the render verbs; and with
[`render.preview-scene-rig`](render.preview-scene-rig.md) for the preview lighting this page tells
you to rebuild in a level.

## The Line Between MRQ And Stepped Captures Is Temporal History

**A stepped capture set is N independent frames; a render is one shot.** `camera.orbit_shots` and
`render.capture_asset_preview` step a pose, draw, and hand back a PNG. Nothing carries across the
call boundary — no engine warm-up, no temporal accumulation, and (measured below) not even a stable
lighting solution. MRQ renders a shot: warm-up frames run first, each output frame accumulates its
temporal samples, and the frame before is part of the history of the frame after.

**The rule is what the frames are for, not how many there are.** Frames that will be *looked at* —
a before/after pair, a coverage sweep, evidence that an edit landed — are captures, and the whole
[`visual-review`](visual-review.md) discipline applies. Frames that will be *watched as motion* are
a render, even when the camera move is trivial and the subject is one mesh.

**Any set longer than 24 frames is already multiple calls.** `GMaxOrbitShots = 24` is shared by the
four multi-shot capture verbs and refused hard, with no caller override
(`F-multi-shot-ceiling-not-settable`); a 240-frame set is therefore 10 calls, 10 preview-scene rig
cycles, and an out-of-band mux, and the outputs of those calls **interleave on disk** rather than
concatenating. `camera.orbit_shots` also has no `filename` stem and rounds azimuth to a whole degree
in the name it generates, so a set stepped finer than 1° cannot be mapped back to poses without
parsing every response (`B-orbit-shots-no-filename-stem`). One verb for the whole spin is asked for
in `F-preview-turntable-capture`.

## A Turntable Is A Render Of A Lightbox Level, Never A Capture Set

**Shading of a fixed mesh under a fixed light, while only the camera moves, is view-independent: it
must not change at all.** On an asset-preview viewport it does, and the amount is visible in a cut.

Measured 2026-09-02 on a 240-frame turntable of a fluted column mesh, and filed as
`B-preview-capture-lighting-cycles-per-shot`:

- **12 captures of the *same* pose, in one call, returned a different lighting solution each time.**
  Mean luminance of the plinth's front face: **171.0 / 155.8 / 143.3 / 156.1 / 170.6 / 155.9 /
  143.4 …** — a clean **period-4 cycle, ±14/255**, with the camera stationary.
- **The settle check cannot see it.** Whole-frame mean luminance moves by **0.001** across that same
  set, so `warmup.settled: true, settleRounds: 1` certified every one of them. `warmup` is a
  whole-frame mean-delta test and the cycle is local to a region. **A convergence check on the frame
  mean cannot see a local cycle** — test the subject region, or repeat the pose and diff.
- **It is not aliasing, and no anti-aliasing setting moves it.** Splitting 240 frames across 10
  calls lands the cycle in the film as a **10-frame plateau with a step at every boundary**: 34
  adjacent-frame steps above 3/255, the largest **56/255**. A TSR pass and a no-AA pass show the
  *same* steps at the *same* frames to within 0.2/255.
- **Averaging four captures per pose gets most of the way and does not close.** A whole cycle
  averaged per pose drops the worst step to 7.3/255, but the cycle is not exactly four captures in
  every group, so frames still jump.
- **A bit-stable preview is a preview that has not converged at all.** A freshly opened preview
  window renders the floor near-black (mean 0.591, min 0.004) and is then bit-stable; ten seconds
  later the same call, same pose, **same reported rig** renders the courtyard ambient (mean 0.686,
  min 0.117). The response's `previewScene` block is byte-identical across that change — **a report
  of the rig applied is not a report of the rig in the pixels** (`B-preview-rig-first-capture-stale-sky`).
- **The MRQ lightbox removes it.** Same framing, rebuilt as a level and rendered: **0.24/255** of
  plinth-shadow jitter against 11.9, and **0 steps above 3/255** across the whole 240-frame clip
  against 34.

`r.SkyLight.RealTimeReflectionCapture.TimeSlice 0` does not change the cycle. The preview world runs
Lumen-off show flags with DFAO, and its indirect term is what oscillates.

## Rebuilding The Preview Look In A Level

The asset-preview environment is reproducible, and this is the whole recipe. Every path here is an
engine asset.

- **Floor:** `/Engine/EditorMeshes/AssetViewer/Floor_Mesh` with `/Engine/EngineMaterials/M_Grid`.
  **The cross-tick pattern is the floor material, not a grid overlay** — turning the grid off does
  not remove it, and it is not editor chrome to clean up.
- **Backdrop:** an inverse-normals sphere scaled large, on a material instance of `M_SkyBox` whose
  `SkyBox` parameter is `/Engine/EditorMaterials/AssetViewer/EpicQuadPanorama_CC+EV1` — the default
  preview cubemap. No shadow, no collision.
- **Sky light:** Movable, `SLS_SpecifiedCubemap` pointed at that same cubemap,
  `bLowerHemisphereIsBlack` false.
- **Key light:** one Movable directional light. Read the preview's own key off
  `viewport.previewScene` rather than guessing it, and note that the published azimuth/elevation is
  where the light **arrives from**, not where it points
  ([`render.preview-scene-rig`](render.preview-scene-rig.md)).
- **Post process:** an unbound volume with **Lumen GI and Lumen reflections off**, motion blur 0.
  The asset viewer forces both off through `DisableAdvancedFeatures`, so a plain level render does
  not match the preview until they are.
- **Exposure:** pin the volume to the `ev100` the preview reported, so the two are comparable
  ([`render.capture-exposure`](render.capture-exposure.md)). A volume's
  `AutoExposureMin/MaxBrightness` is a **linear brightness, not an EV100**, and the two run in
  opposite directions; pass `ev100Equivalent` back as the pin and confirm the rig by matching the
  `adapted` gain both sides report, never by assuming the units convert.
- **Camera:** solve the field of view to reproduce the framing you already have rather than picking
  one. A capture that was centre-cropped in post has a narrower effective FOV than the one it was
  taken at; carry that arithmetic across or the composition constants measured against the old clip
  stop holding.

## What A Preview Capture Puts In The Frame

**The asset preview draws the world-axis gizmo and the grid, and two of the four decorations have
no lever.** `previewScene {showFloor, showEnvironment}` suppresses the floor and the backdrop; no
parameter on `render.capture_asset_preview` or `camera.orbit_shots` passes the gizmo or the grid
through (`B-capture-preview-decoration-not-suppressible`). Workarounds, both with a cost:
`ShowFlag.Grid 0` through `system.console_command` removes the grid but is **process-global** and
contaminates every other agent's captures until restored
(`B-showflag-cvar-override-contaminates-capture`); the gizmo has to be **cropped**, which means
over-capturing, and that cost scales with the length of the set.

Two smaller parity traps on the same verbs: `camera.orbit_shots` cannot ask for `subjectCoverage`,
the one signal that catches a frame containing nothing (`B-orbit-shots-no-subject-coverage`), and it
accepts `hideEditorSprites` on the asset-preview branch where it is inert
(`B-orbit-shots-sprites-inert-on-asset-subject`).

## The Rasterised Size Is Not The Size You Asked For

**Every capture verb hands its requested width and height to the editor's own screen-percentage
heuristic and reports only the request back.** Editor viewports ignore `r.ScreenPercentage` by
design (`EditorViewportClient.cpp:4961-4963`), and the plugin installs no screen-percentage driver
anywhere, so the engine fallback decides the fraction — and it seeds that heuristic with **the
capture's own requested size**. Four consequences:

- The frame is rasterised **below** W×H and upscaled to it, and nothing in the response says so.
- The fraction **falls as the request grows** (engine arithmetic: 0.808 at 1120×1400, ~0.62 at
  2240×2800), so asking for a bigger image does not buy proportionally more detail — and can lower
  the internal resolution outright.
- Preview and level captures take **two different laws**, because the branch reads the raw
  `bIsRealtime` member and not `IsRealtime()`, which is what the capture's realtime override writes.
- A `viewMode` that does not support a preview resolution fraction (wireframe, complexity, LOD
  coloration) renders at 1.0 while the lit frame beside it does not.

**Until `B-capture-render-resolution-unreported` lands, the only lever is to request larger than you
need and downscale in post** — which is the same operation that crops the preview's axis gizmo off,
so pay for it once.

## The MRQ Traps That Cost A Whole Render

- **A full revolution renders one frame too many, and the extra one is a duplicate.** A sequence
  keyed `0 … N` over a 360° turn has N+1 frames and frame N re-occupies frame 0's pose, so drop it
  **for loop closure**, not because it is under-accumulated. Measured on a 241-frame lightbox turn:
  frame 240 differs from frame 0 by MAD 0.464 against 9.38 for any adjacent pair, its step from
  frame 239 is below the adjacent-pair median, and its high-frequency content is 15% *lower* than
  239's — the frame is fine, it is just already in the clip.
- **`mrq.run_jobs` renders the whole shared queue.** `mrq.create_job` appends to the editor-global,
  session-persistent queue and `run_jobs` takes no job selection at all; no verb lists, removes or
  clears an entry, so a stale job from earlier in the session re-renders and **overwrites its own
  outputs** — in one measured session, two stale jobs would have overwritten 480 frames and 254 MB
  of finished masters. `queueSize` on the `create_job` response is the only signal that anything
  else is queued. **Workaround:** immediately before rendering, `python.execute` over
  `UMoviePipelineQueueSubsystem` — enumerate `GetQueue()->GetJobs()` and `DeleteJob` everything you
  did not just create. `B-mrq-shared-queue-no-management-verbs`.
- **The MP4 writer emits an AAC track for a scene with no audio.** 48 kHz stereo, 192 kbit/s, and
  digitally silent (`max_volume: -91.0 dB`). `mrq.run_jobs` measures the file it produced and never
  looks inside it, so a silent deliverable must be gated on **stream count plus measured loudness**,
  never on the job result — and `-an` is how you drop the track deliberately.
  `B-mrq-artifact-report-omits-stream-count`.
- **The encoder's default rate control has no lower bound.** A quality-targeted default collapses on
  smooth, low-detail footage while frame count, duration and resolution all stay correct. The
  threshold, the warning and the remedy (variable bit rate with an explicit average) are on
  [`mrq`](mrq.md); the response now publishes the bitrate that used to be invisible
  (`B-mrq-render-result-omits-bitrate-and-size`).
- **You cannot read back what a preset carries.** No verb publishes the anti-aliasing sample counts
  or warm-up frame counts that decide whether the frames converged, and `outputs[]` lists only file
  writers (`B-mrq-config-readback-omits-sampling`). Record the settings you rendered with yourself,
  beside the masters.
- **No verb authors a preset.** `mrq.list_presets` enumerates and `mrq.create_job` consumes; a
  project with no preset asset renders at engine defaults or drops to `python.execute`
  (`F-mrq-preset-authoring`). Say which route made a preset when you document one.

## PNG Masters, Then One Encode

**Render an image sequence and encode it yourself.** It costs disk and buys: per-shot re-rendering
without redoing the whole pass, an encoder whose rate control you control, no surprise audio track,
and a master that survives a re-cut. [`cinematic-flythrough`](cinematic-flythrough.md) argues the
same call from render cost; this is the second reason for it.

**Render each tile of a grid at the size it will occupy on screen, then stack the tiles.** A 2×2
grid of 960×540 tiles is four renders at 960×540 composited, not four 1080p renders downscaled: each
tile is a different camera on a different subject, so there is no single large frame to downscale
from, and rendering at delivery size spends the temporal samples on the pixels that ship.

**Count in frames, never in seconds.** Every window is `start … start+N-1`, every segment's length
is a frame count, and the runtime is whatever the segments sum to rather than a target the edit is
pushed toward. When a segment is cut out of an already-encoded file, select frames by decoding and
trimming from the head of the file — `trim=start_frame:end_frame` in ffmpeg — never by seeking:
`-ss` lands on the preceding keyframe on any real GOP, silently. Rebase timestamps after a trim, or
every time-based overlay expression reads the source file's clock instead of the segment's.

**A loop closes when the last frame is one step short of the first**, not when it repeats it. Judge
that seam by comparing it against the *adjacent* frame pairs of the same clip, never against an
absolute PSNR figure: the same turn shot against a moving background instead of a flat one lost
~24 dB of absolute PSNR at every probe while the seam stayed exactly as good as its neighbours.

## Prove Which Render Is In The Cut

**A provenance check scores frames of the finished file against the same-numbered frames of the
source they should have come from, with controls.** Three controls make it a question worth asking:
the adjacent source frame, one about half a second away, and **the same-numbered frame of a
different render of the same camera path**. That third one carries the verdict — same path, so it
scores far above unrelated footage and clearly below the true source, and the gap is what lets the
check say *which* render is in the cut rather than merely that some footage of that scene is.

Three rules learned the expensive way:

- **Judge per probe, never min-over-all against max-over-all.** The global form compares one frame's
  aligned score against a *different* frame's control: a slow frame's own +N control legitimately
  reaches 38 dB while a fast frame's aligned score is 43 dB, and a correct cut fails.
- **Crop deliberate overlays out of the comparison, and probe where none are drawn.** A watermark
  band or a title is not evidence against provenance; a frame with text burnt into it scores the
  text.
- **Expect a residual, and know what it costs.** Each re-encode generation is worth roughly a dB
  against the source, so an exact match is not the passing condition — the margin over the controls
  is.

## See also

- [`cinematic-flythrough`](cinematic-flythrough.md) — authoring the camera move itself: scouting,
  tangents, loop closure in velocity, and the offline-render configuration.
- [`mrq`](mrq.md) — the render verbs, the artifact report, and the encoder warnings.
- [`render.preview-scene-rig`](render.preview-scene-rig.md) — the preview lighting rig this page
  rebuilds in a level, and what the response block can and cannot promise.
- [`render.capture-exposure`](render.capture-exposure.md) — pinning exposure so two frames are
  comparable, and why a pinned pair reproduces without being identical.
- [`visual-review`](visual-review.md) and [`level-review`](level-review.md) — the capture discipline
  the evidence half of this page assumes.
- [`camera`](camera.md) and [`render`](render.md) — the per-verb argument reference for the shot and
  capture verbs.
