# render

Rendering and viewport capture helpers for editor-visible assets and levels. Use this namespace when you need exact-size PNG captures from asset editor preview viewports, the active Level Editor viewport, render targets, or post-process volume rendering setup.

## Scope, and where to start

For choosing the right capture surface, start with `call("visual-review")`. Capture calls operate on editor viewports or render resources; they do not author gameplay objects unless a specific method says it configures a level render component or volume.

Choose the surface by the pixels you need:

| Need | Start with |
| --- | --- |
| A static or skeletal mesh without opening its editor or using the active level | `render.capture_mesh` |
| An asset-editor preview (static mesh, skeletal mesh, animation or Niagara) | `render.capture_asset_preview` |
| The active level viewport | `render.capture_open_level` |
| That viewport or preview with measured overlays | `render.capture_annotated` |
| A skeletal mesh at animation instants, outside a level | `render.capture_animation_preview` |
| A georeferenced orthographic tile set | `render.capture_ortho_tiles` |
| A two-render probe for z-fighting | `render.detect_z_fighting` |

The shared inputs, where a verb exposes them, are camera pose, projection, fixed `width` / `height`, `viewMode`, `exposure`, and an optional `subject`; shot sets add the declared count/distribution controls. The shared evidence blocks are described below (`viewport`, `imageStats`, `blank`, `framing`, `poseSet`, and `shotDistribution`). Method sections state their own required fields, defaults, routing and exceptions, then link back here for behavior they do not repeat. An absent block still means “not measured” or “not applicable”; it is never an empty default.

## Check the view mode before you trust a capture

Every viewport capture inherits whatever view mode the viewport is currently in. No capture verb sets it, and `editor.set_view_mode` deliberately does not restore itself, so a viewport left in Wireframe, Unlit or a collision mode by earlier work keeps producing captures in that mode indefinitely.

A wireframe frame is **not** a blank frame — it is full of bright lines — so `BLANK_CAPTURE` cannot catch it. Every capture response therefore carries a `viewport` block:

| Field | Meaning |
| --- | --- |
| `lit` | `true` only for Lit and Path Tracing. **The one field to check.** `false` means the image shows no usable material/lighting/colour information. |
| `viewMode` | Human-readable mode name (localized — read it, do not compare against it). |
| `viewModeKey` | Stable key, spelled as `editor.set_view_mode` accepts it, so it feeds straight back to restore the mode. |
| `viewModeValue` | Raw `EViewModeIndex`. |
| `viewModeWarning` | Present **only** when `lit` is `false`; names the mode and the remedy. |
| `type` / `typeValue`, `gameView`, `realtime` | Other viewport state. |
| `render` | **Unconditional.** What was rasterised before the PNG readback: `width`, `height`, `primaryResolutionFraction`, `secondaryResolutionFraction`, their combined `resolutionFraction`, `screenPercentage`, `antiAliasingMethod`, `antiAliasingMethodValue`, `resolutionPinned`, `upscaled`, and `temporalAntiAliasingSuppressed`. Viewport stills pin both resolution stages at draw time, so render and file dimensions match. |
| `warmup` | Whether the frame had stopped changing when it was read. Unconditional: `settled`, `settleRounds`, `meanLuminanceDelta`, `pixelChangeMeasured`, `meanAbsDelta`, `maxDelta`, `changedPixelFraction`, `channelThreshold`, `settleMs`, plus a `warmupWarning` when the budget ran out. See below. |
| `aim` | **Unconditional and measured.** Whether the camera went where the request asked: `applied`, `rotationErrorDegrees`, `locationErrorCm`, `orbitCameraAtEntry`, `orbitCameraSuppressed`, plus an `aimWarning` when it did not. See *The camera goes where you asked* below. |
| `exposure` | **Unconditional and measured.** Whether an exposure pin was written and applied, and at what EV100: `pinned`, `restored`, `ev100`, `adapted` / `ev100Equivalent` / `adaptedSource` when a measurement exists, `adaptedMeasured`, `adaptedReadbackPending`, plus a `pinWarning`. `pinned` answers a question about the **viewport**; `pinnedFrameUsable` and `pinRangeWarning` answer one about the **frame** the pin produced, and a capture can answer the two differently. See [`render.capture-exposure`](render.capture-exposure.md). |
| `viewModeOverride` | **Unconditional.** Distinguishes "no override was asked for" from "an override did nothing": `requested`, `applied`, `restored`, `previous` / `afterRestore`, `showFlagsChecked` / `showFlagMismatches`, `companionMode`. See [`render.view-modes`](render.view-modes.md). |
| `editorSprites` | **Unconditional on the verbs that offer `hideEditorSprites`.** `hideRequested`, `visible` (the flag the pixels were drawn with, so `true` means icons could be in this frame whether or not anybody asked), `billboardSpritesBefore`, `billboardSprites`, `restored`, plus `hideWarning` / `restoreWarning`. See [`render.capture_open_level`](render.capture_open_level.md). |
| `previewScene` | **Unconditional and measured.** Which light rig an asset preview was drawn under and whether a requested one was applied and put back: `sceneAvailable`, `advancedScene`, `profileName` / `profileIndex`, `key`, `sky`, `showFlags`, `requested` / `applied` / `restored`, `previous` / `afterRestore`, `restore.*`. `sceneAvailable: false` is the only key on a level viewport. See [`render.preview-scene-rig`](render.preview-scene-rig.md). |
| `grass` | **Unconditional and measured.** Which camera the landscape grass in these pixels was built around, and whether that build finished: `measured`, `landscapes`, `builtForPose`, `settled`, `cameraLocation`, `components` / `componentsBefore`, `instances` (omitted when the total could not be taken — see below), `pendingComponents` / `pendingTasks`, `buildMs`, plus a `grassWarning`. `landscapes: 0` is the whole block on a level with no terrain and on every asset-preview capture. See below. |

| `shadersCompiling` + `readbackFlushed` | **Composed by the verb, not by the shared serializer — present on `render.capture_open_level`, `editor.screenshot` and `ui.screenshot` only.** The readback preamble: what the compile queues held when the frame was taken (`measured`, `shaderCompilerAvailable`, `pendingAtEntry`, `ready`, `timedOut`, `shaderJobsAtEntry` / `shaderJobsRemaining`, `assetCompilationsAtEntry` / `assetCompilationsRemaining`, `pumpRounds`, `drainMs`, `budgetMs`, plus `readinessWarning` / `assetCompilationWarning`), and whether the render and RHI queues were flushed before the copy. See below. |
| `onScreenMessages` | **Composed by the verb, not by the shared serializer — present on `render.capture_open_level`, `editor.screenshot` and `ui.screenshot` only.** Text the engine drew into the frame, as `{severity, source, text, color}` rows, beside `onScreenMessageCount`, `screenMessagesEnabled`, `mapWarningsSuppressed` and an `onScreenMessageWarning` when the list is non-empty. See below. |

The four rows above `previewScene` are sub-objects and were absent from this table for a while although the page described each of them further down. One serializer builds the whole block for every viewport-owning verb, so the field list is the same on `render.capture_open_level`, `render.capture_asset_preview`, `render.capture_annotated`, `render.capture_animation_preview`, `camera.frame_actor`, `camera.orbit_shots` and `camera.animation_shots` — a verb-by-verb difference in that part of the block is a bug, not a design. **The last two rows are the stated exception:** both are readings of live process state at capture time rather than of the capture record, so they are composed onto the block by the two verbs that take them, and their absence elsewhere is deliberate rather than a gap.

**`viewport.warmup` — the frame is pumped until it stops moving, and the response says what that cost.** A viewport that has just been created draws a dark frame first: measured 2026-08-19 on `/Engine/BasicShapes/Cube` pinned at `ev100 0`, the capture that *opened* the asset editor read `meanLuminance` **0.0506** and the identical capture into the same editor one call later read **0.2437** — **2.26 stops apart at a pinned exposure**, which is the one thing pinning is supposed to make impossible. Same effect on a Static Mesh editor close/reopen the same day: **0.0943** then **0.3204** from identical pinned requests, ~1.8 stops. Auto-exposure hides the gap by re-exposing; a pin surfaces it, so it corrupts precisely the comparison workflow pinning exists for.

Every capture now redraws until the frame's mean luminance stops changing, then reports `settled`, `settleRounds`, `meanLuminanceDelta` and `settleMs`. Beside the mean, `meanAbsDelta`, `maxDelta`, and `changedPixelFraction` compare the final two BGRA8 readbacks with a one-level per-channel noise floor; this exposes a local filled-face change that a whole-frame mean can hide. A viewport that is already warm settles on the first extra round — that is one extra draw and one extra pixel readback, and `settleMs` is what it actually cost on your scene rather than an estimate. `settled: false` means the budget (8 rounds or 1500 ms) ran out with the frame still moving, and comes with a `warmupWarning`: those pixels are mid-warm-up and are not comparable with anything. Re-shoot into the same viewport and use the later frame.

**Viewport captures render at their returned size.** A draw-scoped view extension overrides the primary fraction and the independent low-DPI secondary fraction to 1.0 on the capture's view family; it does not write the viewport client's preview-screen-percentage optional, and `r.ScreenPercentage` is not used because editor viewports ignore it. It also clears the `TemporalAA` show flag, so a project configured for TAA or TSR uses the engine's non-temporal FXAA fallback for these stills. Read `viewport.render` rather than inferring either fact from the file dimensions.

Do **not** use `viewport.exposure.adaptedReadbackPending` for this — it is true of fully warmed frames and of every pinned capture (see [`render.capture-exposure`](render.capture-exposure.md)). It was published as a `warmupWarning` on that basis and told callers to discard good frames. `warmup.settled` is the measurement.

**`viewport.grass` — the pose now drives the landscape-grass build, and the response says which camera the grass belongs to.** Landscape grass is built around **camera locations** collected on the **world tick** (`ULandscapeSubsystem::Tick` reads `World->ViewLocationsRenderedLastFrame` and calls `ALandscapeProxy::UpdateGrass`), and a capture ticks Slate and the renderer but never the world — so `location` / `rotation` used to move the render camera and nothing else. The frame came back valid, non-blank and settled, showing grass built around wherever the persistent Level Editor viewport had been sitting. Measured, same map, one variable: the identical pose captured through `render.capture_open_level` read `meanLuminance` **0.6443** at 844 KB with **no grass**, while `editor.set_camera` to that pose (which leaves the camera there long enough for a tick to pick it up) followed by the same capture read **0.4796** at 1558 KB with a **full carpet**. Grass darkens the frame, so the lower mean is the one with more of it.

Every capture now hands its own **measured** eye position to `ULandscapeSubsystem::RegenerateGrass(bFlushGrass: false, bForceSync: true, [cameraLocation])` before the first draw. No viewport camera is moved to do it and none is left moved: the location travels as data, and the grass components are transient — the editor's own amortised update rebuilds around your camera on the next world tick. `grass.cameraLocation` equals the response's top-level `cameraLocation` on every level capture; a mismatch, or `builtForPose: false`, means these pixels show somebody else's grass.

**Read `settled` before concluding "no vegetation here".** `settled: true` with `instances: 0` is a measurement — that ground really is bare. `settled: false` means the build had not finished when the shutter fired and comes with a `grassWarning`; those pixels may be missing grass that exists. The two are the same picture and opposite conclusions, which is why the block is unconditional. `buildMs` is what the synchronous build cost on your terrain, published rather than estimated.

**`instances` is present or it is absent — an absent one is never a zero.** The count is read off the render-side instance buffer (`GetNumRenderInstances`), which is where the landscape grass build actually puts its instances; it used to be read off `PerInstanceSMData`, an array landscape grass never writes, so it was 0 on every capture of every grass type no matter how much was standing there. If a grass component is destroyed between the build and the count the total would be a floor rather than a measurement, so the field is **omitted** and a `grassWarning` says so alongside `unreadableComponents`. Branch on whether `instances` is there, not on its value: present-and-zero beside `settled: true` is the bare-ground reading above, absent means nobody could take the number and `components` is the field to read instead.

A non-Lit capture is a warning, not a failure: the collision-review workflow ([`editor.collision-review`](editor.collision-review.md)) captures in a debug mode on purpose. If you did not intend it, call `editor.set_view_mode {viewMode: "Lit"}` and capture again before making any visual acceptance decision.

`camera.animation_shots` and `render.capture_animation_preview` also copy the warning into their `warnings[]` array. `editor.screenshot` reports the block only on its level-viewport branch — the game/PIE branch has no editor viewport client and therefore no view mode, and says so via `captureSource: "gameViewport"`.

## Take a diagnostic view without moving the viewport

`viewMode` renders **one capture** in a different view mode and restores the viewport's previous mode on every exit path, both view-mode slots. `editor.set_view_mode` is the persistent verb and never restores itself, so a diagnostic look taken that way leaks into every later capture. Use the parameter for a look; use the verb only when you want the editor left in that mode. Every verb here accepts it, and the multi-shot verbs read it once for the whole set.

Five modes pay for themselves: `front_back_face` (the inside-out-shell check, and the visual counterpart to `health.signedVolume`), `unlit`, `random_color`, `zebra` and `clay`. `random_color` is useful for separate static-mesh primitive components, but it does not distinguish sections inside one skeletal-mesh component: every section receives the same colour, so it cannot reveal skeletal-mesh islands. Nothing falls back to Lit silently — four kinds of unserviceable request are refused with their own error code (`UNKNOWN_VIEW_MODE`, `VIEW_MODE_NOT_RENDERABLE`, `VIEW_MODE_NEEDS_COMPANION`, `VIEW_MODE_UNAVAILABLE`) rather than rendering a plausible picture of something else.

**`render.capture_ortho_tiles` shares only the vocabulary.** It writes a transient scene-capture component's show flags, not a viewport's two mode slots — so nothing outlives the call, it reports **no `restored`** rather than faking one, and its verdict lands under `showFlags.viewMode`, since that verb emits no `viewport` block at all. The shared parameter description is written for the viewport case and does not hold there.

## Hold the capture size fixed across a session

**Varying capture resolution call-to-call killed an editor on 2026-08-13 and cost 66 actors and 125 emitters of unsaved level state.** `render.capture_open_level` resizes the live scene viewport; a mouse-cursor query against the now-stale hit-proxy buffer then trips an engine `check()`:

```
Assertion failed: ProxyMap.Num() == TestSizeX * TestSizeY
  FViewport::GetHitProxy()
  <- FLevelEditorViewportClient::GetCursor()
  <- FSlateUser::UpdateCursor
```

Saved assets survived. Nothing in the level did. No Python was involved — this is a capture hazard, not a scripting one, and no error code exists for it because it is an engine assert rather than a refusal.

**Resizing is not the problem; *changing* the size is.** 48 back-to-back capture cycles at a constant 640 produced zero asserts. That is why `camera.animation_shots` resolves resolution once and holds it for the whole burst — a hand-driven sequence across several `render.capture_*` / `camera.orbit_shots` / `camera.frame_actor` calls has no such protection, and nothing will stop you.

So: pick one size at the start of a session and stay on it. If a capture came out too small to read, **re-shoot the whole set at the new size** rather than raising it mid-sequence. `editor.screenshot` is scoped differently: omit `width`/`height` for native resolution, or pass both for a temporary exact-size scene + UMG composite whose original viewport size is restored before completion. It remains the low-risk surface when you only need one frame.

**Preview pose sets pin before sizing and resize only on change.** UE 5.8
`FSceneViewport::ResizeViewport` calls `Draw()` synchronously after rebuilding the viewport
(`SceneViewport.cpp:1988-1995`). The capture context therefore establishes fixed exposure,
TemporalAA suppression, and primary/secondary screen-percentage pinning before the first positive
`SetFixedViewportSize`. One context survives the warm-up, all requested poses, and, for
non-time-driven sets, the final repeatability control; unchanged shots do not resize. It restores
the original viewport size and fixed-size state while
the three pins still cover any synchronous hand-back draw. Direct single captures use the same
ordering through a local one-call context.

The earlier per-shot order resized before those pins existed, so every shot began with an unpinned
draw that could feed persistent renderer history. The endpoint `poseRepeatability` warning below
still applies: a clean endpoint comparison does not prove every intermediate preview frame was stable.

## Pin exposure before comparing two captures

Auto-exposure is a live scalar gain over the whole frame and it re-balances between shots, so an unpinned pair silently cancels part of whatever you are measuring — on 2026-08-16 that nearly became a false bug report against this plugin. **Pass `exposure` on the capture itself**: `exposure: -1`, or the long form `{mode: "fixed", ev100: -1}`. **The usable `ev100` band is scene-dependent — pick one by reading `imageStats.meanLuminance` back, never by reusing a number from elsewhere.** This page used to give `11` here, which on the asset-preview fixture is past the end of its range: mean luminance 0.0068, pinned at the floor and unresponsive from `10` up, returned as an ordinary success (`B-exposure-pin-black-frame`). A frame in that state now says so — `crushed`, `rangeWarning` and `viewport.exposure.pinnedFrameUsable` — but the example should not have been steering callers into it. The supported scene/viewport capture verbs (`render.capture_*`, `camera.*`, and `editor.screenshot`) take it; it is optional everywhere except `render.capture_ortho_tiles`, where it is required, and the multi-shot verbs read it once for the whole set so a burst is internally consistent by construction. Read `viewport.exposure.pinned` back for render/camera captures, or top-level `exposure.pinned` on the game/PIE branch of `editor.screenshot`, rather than assuming — it is measured, and a frame the renderer treats as a debug view comes back `pinned: false` with a `pinWarning` naming why.

Two traps a pin does not close, both covered in full on the page below: `adapted` is a **linear gain**, not an EV100 — feed `ev100Equivalent` back instead — and **the first capture into a freshly opened preview window is about a stop dark** whatever you pin.

The measured field-by-field contract, the auto-once-then-pin recipe, the comparison tolerances (a correct pinned pair is not byte-identical) and the headless/interactive parity measurement: [`render.capture-exposure`](render.capture-exposure.md).

## Two renderers, and captures from them are never comparable

Every `render.capture_*` verb except one drives the **live Level Editor viewport** and reads its back buffer. `render.capture_ortho_tiles` drives an offscreen **`USceneCaptureComponent2D`** instead. Both produce a PNG of the level and they do not agree: measured at identical framing, viewport vs. scene capture is **5.76% mean absolute error against a 1.30% viewport self-noise floor**, and fitting an ideal tone LUT between them only closes it to 3.28%. Scene capture is the more repeatable of the two (0.12–0.54% between its own frames).

So: **never diff a viewport frame against a scene-capture frame.** The ortho manifest records `renderer: "sceneCapture2D"` **and** `projectionMode: "orthographic"`, which are there so a comparison can refuse a mismatched pair up front rather than report the renderer or the projection difference as a content change.

Three things the scene-capture path gets that the viewport cannot give, all measured, all in play whenever you shoot a wide orthographic frame:

- **Distance culling stops eating the frame.** A lit orthographic *viewport* measures cull distance from a culling origin pushed ~2,100,000 cm behind the camera, so a wide top-down loses everything with a finite cull distance and needs the whole `viewDistanceScale` apparatus to get it back. A scene capture leaves `bUpdateOrthoPlanes` off and never applies that pushback: at identical framing, static meshes went 0/16 → 16/16 and HISM instances 0/64 → 64/64 with no cvar touched at all.
- **Any pose, not six.** The viewport derives an orthographic view matrix from its `ELevelViewportType`, so it can only render the six cardinal poses and quantises anything else — which is why viewport captures report an `effectiveRotation` that may not be the one you asked for. A scene capture uses the transform verbatim.
- **It works with no viewport open, and it cannot resize one.** The capture-resolution hazard above (a varying capture size trips an engine `check()` in `FViewport::GetHitProxy`) is unreachable from this path: nothing here touches a Slate viewport.

The price is the **near plane**. The scene-capture orthographic projection hardcodes its near plane to **0**, so everything behind the camera *plane* is clipped outright — a camera at z=3000 over a map whose cliffs reach z=8000 renders them sliced off. This is the same switch as the culling fix; you cannot have both. `render.capture_ortho_tiles` therefore derives its camera depth from the world's own measured bounds unless you name one, and reports the depth it used plus how much content (if any) sits behind it.

Show flags on this path start from the **game** set, not the editor set: no grid, no gizmos, no billboards, no spline overlays, no selection outlines. That is deliberate — a frame you are going to diff against a downloaded reference image should not contain editor chrome — and the response publishes the measured flags rather than asserting it.

## The camera goes where you asked, and the response says where it went

**Read `cameraLocation` / `cameraRotation` out of the response, never the values you passed in.** They are measured off the viewport after the pose is applied, and `viewport.aim.applied` is the verdict that they match the request. When it is `false` the response also echoes `requestedLocation` / `requestedRotation` and attaches `viewport.aim.aimWarning`; those pixels show a different part of the world than your arguments describe and must not be paired with another frame.

The mechanism behind that field, because it produced a whole class of wrong conclusions before it was fixed. An editor viewport can be in **orbit mode** — asset-editor preview viewports are, by default, and a level viewport is after the user alt-drags. In orbit mode `SetViewLocation` / `SetViewRotation` still succeed but aim nothing: the engine builds the view from an orbit matrix around a pivot the caller never named. The requested **location's direction was discarded** (only its distance from the pivot survived) and the rendered **yaw came out as `90 − requested yaw`** — a mirror, not an offset, so `yaw:0` rendered as `+90` and `yaw:-90` as `180`. Every perspective capture through a preview viewport was aimed somewhere else while returning a valid PNG and echoing the requested pose back. Captures now take the viewport out of orbit mode for the shot and put it back; orthographic captures were never affected and are left alone.

## A frame can be contaminated by state this capture does not own

Two channels put things in a capture that are not in the level, and both are now measured and named in the response rather than left for the caller to notice.

**`viewport.gameView: false` now carries `viewport.gameViewWarning`.** Game view is per-**viewport** state; the capture verbs neither set it nor own it, so in an editor shared with other agents it can be turned off *after* a confirmed `editor.set_game_view {enabled: true}` and before the shot. The frame then carries the world-axis gizmo, component visualizers (spline handles, tangent arrows, light radii) and the grid/selection passes — none of which `editorSprites` covers, because that governs billboard icons only and reports clean throughout. Re-assert game view immediately before **every** capture and read `gameView` off **that** response, not off the earlier confirmation. The warning is emitted for level-viewport captures only. An asset-preview viewport draws its own decoration instead: the **world-axis gizmo and the grid are on by default** in that viewport class, and no parameter suppresses either — `previewScene {showFloor, showEnvironment}` covers the floor and the backdrop (`render.capture_asset_preview` `RenderHandler.cpp:341`, `camera.orbit_shots` `CameraFrameHandler.cpp:706`) and nothing passes the gizmo or the grid through, with a plugin-wide grep for `bDrawAxes` / `SetShowGrid` / `ShowFlags.Grid` returning one hit that only *reports* a scene-capture component's flag (`OrthoTileCaptureUtils.cpp:845`). Two workarounds, both with a cost: `ShowFlag.Grid 0` through `system.console_command` removes the grid but is process-global (see `viewport.showFlagOverrides` below) and must be set back to `2` afterwards, and the gizmo has to be cropped out of the frame. Tracked as `B-capture-preview-decoration-not-suppressible`.

**`viewport.showFlagOverrides` reports forced `ShowFlag.*` console variables.** Every show flag has a `ShowFlag.<Name>` cvar where `0` forces it off, `1` forces it on and `2` (the default) leaves it alone. A non-default value is ORed over the view's flags *after* the viewport's own are copied, so it is **process-global**, game view does not clear it, and nothing on the viewport client reports it — a `Visualize*` flag another agent left at `1` paints a full-screen debug pass over your frame while `gameView`, `editorSprites`, `blank`, `warmup.settled` and the tone statistics all read clean. The block is unconditional: `measured` says the survey ran, `forced[]` lists `{name, cvar, value, direction, setBy}` for each override, and `overrideWarning` is present iff the list is non-empty. `setBy` tells you whether the project ships it or somebody left it behind. The remedy is one call per flag: `system.console_command {command: "<cvar> 2"}`.

Neither condition fails the capture. Both are shared-editor states a caller may knowingly accept, and the PNG is still returned — the defect was that the response did not say what it had already measured.

**`viewport.onScreenMessages` reports text the ENGINE drew into the frame.** Measured twice on `render.capture_open_level`: every frame of a 15-minute acceptance slot came back with `Video memory has been exhausted (1197.488 MB over budget). Expect extremely poor performance.` burned across the upper third in red, while `showFlagOverrides.forced` was `[]`, `editorSprites.visible` `false`, `gameView` `true`, `blank`/`crushed`/`blownOut` clean and `litPixelFraction` 0.9998. Nothing on the caller's side removes it: it is not a show flag, `hideEditorSprites` governs billboards only, and `editor.set_game_view` does not touch it. The banner is drawn into scene colour **before** the readback, so it also skews `imageStats` — which is why the strings are read directly and no luminance heuristic is used. The overrun that triggers it is not a fixed threshold either: 1197 MB in one report, 377 MB in the next, so it cannot be inferred from uptime.

Each row is `{severity, source, text, color}`; `source` is `coreDelegate` or `renderer`, and `color` is the engine's own draw colour (Info white, Warning yellow, Error red). `screenMessagesEnabled` and `mapWarningsSuppressed` are the engine's two gates: with either against them nothing is drawn, so an empty array under those flags is a positive statement rather than a missing reading. An absent `onScreenMessages` key means the verb took no survey at all.

**Two channels, and the reason the obvious third is missing.** `UEngine::GetOnScreenDebugMessages()` is **not** an API — `ScreenMessages` and `PriorityScreenMessages` are private `UEngine` members with no accessor and no `UPROPERTY` (UE 5.8 `Engine.h`), so messages added through `GEngine->AddOnScreenDebugMessage` cannot be read back by call or by reflection and are **not** covered here. What is covered: `FCoreDelegates::OnGetOnScreenMessages`, which is public and is what the engine's own draw path broadcasts, and the renderer's demoted-local-memory banner, reproduced from `GDemotedLocalMemorySize` under `r.DemotedLocalMemoryWarning` exactly as `SceneRendering.cpp` prints it. The renderer's private `FSceneRenderer::OnGetOnScreenMessages` list — which carries the multiple-directional-light line — is render-thread only and unreachable from the game thread, so a second warning line in a frame may have no row here.

**`viewport.shadersCompiling` is a frame-quality precondition: a capture taken mid-compile photographs the default material.** A material whose shader map has not landed renders as grey or `WorldGridMaterial`, and that frame reads **settled, non-blank, correctly exposed and clean on every other honesty field** — `warmup` cannot catch it, because a stand-in material is perfectly stable. So `render.capture_open_level`, `editor.screenshot` and `ui.screenshot` drain the shader queue first — bounded at **20 s**, pumping `FAssetCompilingManager::ProcessAsyncTasks` each round so the wait does not starve the work it waits on — and publish what was pending, how many pump rounds it cost and whether it timed out. Shader work still in flight after the budget **refuses** the capture with `CAPTURE_NOT_READY` and the block as error data, rather than shipping stand-ins with a warning nobody would read.

**The gate waits on shader work only.** A non-zero `FAssetCompilingManager` queue (textures, static meshes, sound waves) is the normal steady state after any map load, so waiting on it spent the full budget on the game thread on *every* screenshot and then took the frame regardless. Those counts are published and carry `assetCompilationWarning` — meshes and textures can be drawn at a placeholder or lower-detail state — but they never block and never produce `CAPTURE_NOT_READY`.

**Placement matters and is asserted: gate → flush → readback, after the pose apply and the settle loop.** The compiles that decide whether *this* frame shows real materials are the ones the new camera pose queued, so a gate at handler entry would measure the wrong queue. On the level path both halves ride the capture's `BeforeFinalFrame` hook, which runs after warm-up settles and immediately before the final draw and readback.

**`viewport.readbackFlushed` is a separate, weaker claim — read it as a mitigation, not a fix.** Immediately before the pixels are requested, the shared preamble calls `FlushRenderingCommands()`, which drains the render command queue and issues `ImmediateFlush(FlushRHIThreadFlushResources)` — flushing the RHI thread and its pending resource deletions — so resources the preceding draws created, resized or released are settled before the copy is set up. This is aimed at a GPU page fault observed once at a readback (Aftermath: `AddressTranslationError` / `Read`, fragment shader), which has the shape of a resource-lifetime or descriptor-residency fault. **It is plausible, not proven**, and nothing here reproduces the fault. Note that the D3D12 `Starting late shader associations…` lines that appeared alongside that crash are the Aftermath crash-dump *decoder's* output **after** the fault — `RHICoreNvidiaAftermath.cpp` calls `CreateShaderAssociations` from inside the crash-dump handler — not a pre-fault state, and not something the compile gate above addresses.

**View-driven geometry is built around the *persistent* viewport camera, on a tick budget, and `warmup.settled` does not wait for it.** Landscape grass is the common case: the engine builds it around the camera locations the streaming manager saw on tick, and `grass.MaxCreatePerFrame` defaults to **1**, so a large cull radius costs hundreds of frames to repopulate after the camera teleports. `warmup` only settles the *frame*, and it settles immediately - it reported `settled: true, settleRounds: 1` on a frame with no grass in it at all, and again on the same pose 12 s later, still empty. Two shots at the same pose agreeing is therefore not evidence the scene is built; it is evidence the build has not progressed. Measured at one pose: `meanLuminance` 0.305 with no grass, 0.278 after 45 s, 0.275 after 105 s, against 0.251 for the fully built carpet the same pose had shown earlier.

The consequence for a before/after pair is severe, because the missing grass changes the frame far more than the edit under test does. Move the camera with `editor.set_camera`, **wait**, then capture repeatedly at the fixed pose until two consecutive frames agree in `imageStats.meanLuminance` and `sizeBytes` - and compare a pair only when both members are built. `sizeBytes` is the more sensitive of the two here, since a sparse carpet compresses well.

## An empty frame is not a blank frame

`blank` catches **black** frames. It does not catch a frame that renders nothing but a brightly lit backdrop: two `render.capture_asset_preview` calls with a mis-aimed camera returned byte-identical PNGs of pure backdrop with `blank: false` and `litPixelFraction: 1`, and were nearly compared as if they showed the mesh.

`render.capture_asset_preview` therefore also returns a **`framing`** block, testing the asset's bounding sphere against the frame the measured camera pose actually covers:

- `boundsInFrame: false` means the subject **provably cannot** project into this image. The test is conservative — it circumscribes the frustum and inscribes nothing — so `false` is a fact and `true` only means "not provably absent".
- `behindCamera: true` narrows it further: the subject is behind the camera plane entirely.
- `offAxis` / `frameLimit` / `units` are the two numbers the verdict is made of (degrees for a perspective frame, world centimetres for an orthographic one), published so you can judge a near-miss without inheriting this verb's approximation.
- `framingWarning` is present if and only if the verdict is `false`.

**`framing` is geometry, and geometry has a blind spot `subjectCoverage` closes.** A bounding sphere is still in frame when the subject is a dot, when the preview floor occludes it, and when a Niagara system has no particles alive at the captured instant — the frame is then pure backdrop with `boundsInFrame: true`, `blank: false` and `litPixelFraction: 1.0`. Every shot is therefore drawn twice, once with the subject hidden, and `subjectCoverage` reports the fraction of pixels that changed; an empty frame scores `0.000` and carries a `coverageWarning`. This is published for **every** asset kind `render.capture_asset_preview` serves — `staticMesh`, `skeletalMesh`, `animation` and `niagara` — and is **absent, never zero**, when the preview component could not be identified. It used to be bound for Niagara alone, which is how a Static Mesh capture of pure backdrop passed every published health signal; the reference frame hides only the preview component carrying the named asset, leaving the preview scene's floor, sky and lights in place. It costs one extra draw per shot, so `render.capture_asset_preview` takes `measureCoverage: false` to turn it off; it defaults on, and no other pose-set verb offers the flag because none of them publishes a coverage number at all. Full contract, including why a geometric coverage number cannot work: [`render.capture-subjects`](render.capture-subjects.md) § *Coverage is measured differentially*.

## Do not compare an opposed pair in orthographic

Orthographic aiming is exact, but the **preview key arrives from one direction only**, so the halves of an opposed pair are not photometrically comparable: at `orthoWidth: 420`, mean luminance **0.587 from +Y** against **0.036 from −Y**, no content change. Shoot such pairs in `perspective`, or pin the rig on both halves — [`render.preview-scene-rig`](render.preview-scene-rig.md).

## Name the subject, and the capability follows it

`render.capture_asset_preview`, `render.capture_annotated`, `render.capture_animation_preview`, `render.capture_open_level` and the three `camera.*` verbs take one optional **`subject`** object naming what the capture is of — a level, a placed actor, a static mesh, a skeletal mesh, an animation asset or a Niagara system. The existing spellings (`assetPath`, `actorName`, `point` + `radius`) still work and normalise into a subject internally, so no existing call changes.

Not every verb serves every kind, and the ones it cannot serve are **typed refusals naming the verb to use instead**, never a plausible picture of something else. `render.capture_open_level` takes a `world` or `actor` subject only, and it uses it to *answer a question about the frame you asked for* — `framing` and `subject` come back, but **the camera does not move**; pass `location` / `rotation` for that. `render.capture_animation_preview` takes `skeletalMesh` or `animation` only. `camera.animation_shots` takes `world` or `actor` only, because its instants come from scrubbing a Level Sequence.

One verb takes none. `render.capture_ortho_tiles` is **domain-named**: its name states its subject, and giving it one would make the name lie. Its capabilities reach other domains through `camera.orbit_shots` and `render.capture_asset_preview`.

The response carries a `subject` block back — resolved `kind`, the bounds the camera was solved from, `boundsSource`, `captureSource`, whether the kind has a time axis. **It is omitted entirely when no subject was resolved**, never emitted empty; assert its absence rather than zeroed fields, the same as every other block on these verbs.

Descriptor, per-kind bounds rules, and exactly which view-mode capabilities the ortho tile renderer can and cannot reach: [`render.capture-subjects`](render.capture-subjects.md). The typed refusal for a kind with no time axis, the `subjectTime` block and the reproducibility position on particle captures: [`render.capture-time`](render.capture-time.md).

## A set reports its own shape, and omits what it cannot say

Every multi-shot verb on the shared pose-list primitive — `camera.orbit_shots`, `camera.animation_shots`, `camera.frame_actor` (a set of one), `render.capture_asset_preview` on a set and `render.capture_animation_preview` — returns a **`poseSet`** block. Six of its keys are unconditional and six are not, and the conditional half is the trap: **they are omitted when the set has nothing to say, so a caller must assert absence rather than expect a zeroed field.** A `posesFramingEvaluated: 0` would mean "bounds were given and nothing could be measured", which is a different fact from "no bounds were given"; collapsing the two is exactly what emitting the key unconditionally would do.

| `poseSet` key | Present |
| --- | --- |
| `posesRequested`, `posesCaptured`, `posesTruncated`, `maxPosesPerCall`, `warmupShotTaken`, `warmupShotDiscarded`, `poseRepeatability` | always |
| `truncationWarning` | only when `posesTruncated > 0` |
| `warmupFileNotDeletedWarning` | only when the warm-up frame was taken and its file could **not** be deleted; the message names the leftover PNG so you can find it. (Formerly `warmupWarning` — renamed because `viewport.warmup.warmupWarning` still exists inside every shot and means something unrelated: "the frame never settled".) |
| `subjectTimesApplied` | only when the subject had a time axis and a time setter was bound |
| `posesFramingEvaluated`, `posesOutOfFrame` | only when bounds were supplied |
| `outOfFrameWarning` | only when `posesOutOfFrame > 0`; names how many shots provably do not contain the subject |

`poseRepeatability` is an **endpoint check, not a consecutive-frame test**: it compares the retained
first *real* pose-0 frame with one final discarded capture at the same camera pose, after all
requested poses have run. It reports `meanAbsDelta`, `maxDelta`, and `changedPixelFraction` when
measured. A renderer state cycle can put both endpoints in the same phase (for example, a period-4
cycle on a set whose endpoint spacing is a multiple of four), so a small delta can alias a real
variation in the intermediate shots. Read `measured:true` as "these two buffers matched"; use
consecutive captures or vary the set length/phase when investigating periodic drift. Time-driven
sets instead report `measured:false` and `notMeasuredReason`: replaying an animation or simulation
setter is not guaranteed to restore identical subject state, so such a control would mix subject
motion into the render-stability signal.

The per-shot `framing` block follows the same rule: written only when a framing verdict was actually measured, absent otherwise.

**The same trap is inside `imageStats`.** `meanLuminance`, `luminanceVariance`, `minLuminance`, `maxLuminance`, `litPixelCount`, `litPixelFraction` and `litLuminanceThreshold` are unconditional. `toneLevelsUsed` and `toneLevelMinPixels` appear **only when the tone range was measured** — so a missing pair means "not measured", not "zero levels used".

`blank` sits beside `imageStats` and is unconditional. `crushed`, `blownOut` and `rangeWarning` sit beside it too but only on a **single** frame — `render.capture_open_level`, `render.capture_asset_preview` and `render.capture_annotated` — gated on the tone range having been measured, and `rangeWarning` additionally on the range having actually collapsed. **Entries in a `shots[]` array carry none of the three**, at all, by construction: a burst gives you `imageStats` and `blank` per shot and nothing more, so do not write a loop that reads `shots[i].crushed`.

**The tone verdict is only reached for a lit frame, and a non-lit one says so instead of going quiet.** `crushed` / `blownOut` count how many of 256 luminance levels the pixels populate and call fewer than eight a collapse. A wireframe frame is edges on a field, an unlit frame is flat albedo, a complexity view is a small fixed palette — each resolves two or three levels *because that is what the mode was asked to draw*, so judging them by that threshold labels a correct frame unreadable. Whenever `viewport.lit` is false the three fields are replaced by **`toneRangeApplicable: false`** plus **`toneRangeNotApplicable`**, a sentence naming the mode and the remedy. Read `toneRangeApplicable` before `crushed`: the fields are absent rather than false, and absence on its own would read as "measured and fine". The measurement is *not* withheld — `imageStats.toneLevelsUsed` is published on a wireframe frame exactly as on a lit one, so you can still judge it yourself. This is the same lit-ness term that makes `viewport.exposure.pinnedFrameUsable` safe (see below), so the two blocks cannot disagree about when the classifier may speak.

## `subjectRegion` is the only block that describes the subject rather than the frame

Every luminance field above is a statistic **of the frame**, and the frame is mostly backdrop. On 2026-08-29 four foliage assets came back from `render.capture_asset_preview` as a **pure black subject inside a correctly lit preview environment**, and all of them read healthy: `blank: false` was correct, the mean was normal, the tone range was fine. An automated caller gating on the response rather than on the pixels passed a useless image through (`B-capture-asset-preview-renders-foliage-black`).

`subjectRegion` projects the subject's own bounding sphere into the frame and compares the **unlit share inside that ellipse** against the unlit share of everything outside it. It is published on the single-frame block and on every entry of `shots[]`.

| field | meaning |
| --- | --- |
| `measured` | unconditional. `false` plus a `notMeasured` sentence when the capture carried no subject bounds (every level capture), when the camera sits inside the subject's bounds, or when the readback did not match the frame |
| `ellipse` | `centerX` / `centerY` / `radiusX` / `radiusY`, pixels, top-left origin — the same figure the verdict used, so you can crop or overlay it |
| `subject` | `pixelCount`, `meanLuminance`, `maxLuminance`, `unlitFraction` inside the ellipse |
| `backdrop` | the same three, outside it — the control the verdict is made against |
| `litLuminanceThreshold` | the threshold both fractions were counted against; the same one `imageStats.litPixelFraction` uses |
| `silhouette` | the verdict, present whenever `measured` is true |
| `subjectRegionWarning` | sibling of the block, only when `silhouette` fired |

**The verdict is a comparison, never a darkness threshold, and that is deliberate.** A preview profile with its environment switched off renders a black backdrop; an absolute threshold would call every asset in it a silhouette. `silhouette` fires only when the subject's disc is at least 5 percentage points more unlit than its backdrop **and** the backdrop is at least half lit. A subject that shaded *darkly* is therefore not flagged — `maxLuminance` is the field that separates the two, because a darkly shaded subject still carries a highlight and one that never shaded carries none.

**It does not claim a cause, and it cannot.** A genuinely black asset and an asset that failed to shade produce identical pixels. The warning names the lead candidate and then the fields to read next.

**The lead candidate is your exposure pin, and on the reported case that is exactly what it was.** All four of the 2026-08-29 captures passed `exposure: {mode: "fixed", ev100: -0.5}`. A Static Mesh Editor preview in the same project measured `adapted` **4.338** under `{mode: "auto"}` — EV100 **-2.12**, since gain is `1 / (LuminanceMax · 2^EV100)` and the pinned captures' own `adapted` 1.4142 at `ev100: -0.5` fixes `LuminanceMax` at 1 here. So the pin sat **1.62 stops under** what the scene resolves to, and a second probe pinned at `ev100: 0` sat 2.12 stops under. Under the toe of the tone curve that is a black subject.

The environment survives it because an `FAdvancedPreviewScene` backdrop is the **emissive sky sphere**: an inverse-normals mesh carrying `M_SkyBox` with the profile's HDR panorama, which renders identically with the key light, the sky light and every other light switched off, several stops brighter than anything a preview key of intensity 1 puts on a surface. **A correctly lit-looking environment beside a black subject is therefore not evidence that the scene's lights reached the asset** — it is the expected appearance of an underexposed preview.

The A/B is on record: the same asset in the same preview, minutes apart, renders normally under `{mode: "auto"}` and as a black silhouette with the backdrop still correct at `ev100: 0`, with camera azimuth, framing and profile unchanged. That asset is a flat-shaded low-poly tree with no foliage shading model, no world-position offset and no masked leaves — which is what rules the "foliage" reading out. Nothing about this is foliage-specific.

**Compounding, and not the same thing:** the preview key arrives near azimuth 110°, so a default camera looks at the subject's *dark* side, where the fill is genuinely near zero ([`visual-review.model-rig`](visual-review.model-rig.md)). The pin decides whether the little light that is there survives the tone curve. Fix the exposure first, because it is the half you can derive; then pass `location`/`rotation` or a `previewScene` rig for the other half.

Whenever the pixels were drawn at a fixed exposure the warning says so and quotes the EV100. The fix is to **derive** the number rather than pick one: re-shoot with `exposure: {mode: "auto"}`, read `viewport.exposure.ev100Equivalent` off that response, and pass *that* as `ev100`. Only once the exposure is known good are `viewport.previewScene.showEnvironment`, `sky.visible`, `sky.intensity` and `key.intensity` worth reading, along with a comparison against the asset placed in a level.

**It is not `subjectCoverage`.** Coverage answers whether the subject is in the picture at all; a black subject has perfectly ordinary coverage, which is why coverage did not catch this. The two are complementary and both are published.

**The reading is diluted on purpose.** A bounding sphere circumscribes its asset, so the ellipse always contains backdrop too — a thin tree fills perhaps a quarter of its own disc. That weakens the difference rather than inventing one: `subjectRegion` can miss a black subject, it cannot manufacture one.

## An orthographic set cannot hold an `elevation`, and the response says so

`elevation` places a `count` set on a horizontal circle above the horizon and **defaults to 30**. Under `projectionMode: "orthographic"` it cannot be honoured at all: `FEditorViewportClient::CalcSceneView` builds an orthographic view rotation matrix from the viewport **type** and never reads the camera rotation (UE 5.8 `EditorViewportClient.cpp:1341-1401`), and `LVT_OrthoFreelook` is a seventh *fixed* matrix rather than a free one — so only the six cardinal directions are renderable. Every shot is therefore snapped to the nearest axis **before** the camera is placed (an orthographic frame is centred on the camera position, not on a look-at point, so snapping only the direction would push the subject out of frame): `|elevation| < 45` flattens to `0`, `>= 45` goes to `±90`.

The measured A/B, on one asset: `perspective` returned `pitch: -12` and `_el12` filenames; `orthographic` on the same call returned `pitch: 0` and `_el0`, with the top-level `elevation` echoing `null` on both and nothing anywhere saying why. **The default was subject to the same snap** — a caller who never passed `elevation` still had the documented 30 rewritten to 0, which is the harder half to notice because there is no argument of theirs to suspect.

`shotDistribution` now carries it:

| Field | Present |
| --- | --- |
| `elevationRequestedDegrees` | on every **ring** plan, supplied or not — without it, "30 held" and "30 became 0" are the same response |
| `elevationIgnored` | only when an elevation could not reach the cameras |
| `elevationIgnoredReason` | `orthographicProjection`, `sphereDistribution` or `namedViewPlan` — the remedy differs for each, so one bare boolean would send a caller to change the wrong argument |
| `elevationSnappedShots` | how many of the plan's shots the snap **measured** as moved, beside `plannedShots` |
| `elevationWarning` | the sentence, naming the cause and the way out |

`orthographicProjection` outranks the other two reasons when both apply: it is the measured outcome, and the other two are deductions from the request shape. The count is measured by comparing each shot's requested angles against the angles actually placed, never deduced from `projectionMode` — the `views: "sides"` table is cardinal by construction, so a deduction would fire the warning on a set the snap never touched.

**To hold an elevation, use `projectionMode: "perspective"`.** To keep the orthographic projection, ask for the elevation the snap would have produced (`0` or `±90`). For a genuinely arbitrary orthographic pose, `render.capture_ortho_tiles` uses a `SceneCapture2D` and takes the transform verbatim.

## A capture response is sized against a display budget

A tool result larger than the display threshold (10,000 characters by default) is written to `Saved/PinWright/HttpResponses/` and replaced by a short reference, costing the caller an extra file read. Since 2026-08-23 the gate measures **one condensed copy of the payload a reader sees** rather than the pretty-printed wrapper carrying it twice, which is what made every single-still capture spill. Which call shapes still spill, the measured numbers behind the change, and what a spill reference now publishes: [`render.response-budget`](render.response-budget.md).

## Default image geometry

Capture verbs in this namespace and most camera capture verbs default to **768 x 768**. `camera.orbit_shots` preserves **1024** for existing `count`, `angles` and canonical shapes, and **640** for `views:"sides"`; see [camera](camera.md). Square is only the omitted-size default on the shared paths: an explicit non-square `width` / `height` remains valid.

## See also

- [`render.capture-subjects`](render.capture-subjects.md)
- [`render.capture-time`](render.capture-time.md)
- [`render.capture-exposure`](render.capture-exposure.md)
- [`render.view-modes`](render.view-modes.md)
- [`render.preview-scene-rig`](render.preview-scene-rig.md)
- [`render.response-budget`](render.response-budget.md)
- [`visual-review`](visual-review.md)
- [`python`](python.md)
- [`camera`](camera.md)
- [`level-review`](level-review.md)
- [`image`](image.md)

### render.capture_mesh

Capture one `UStaticMesh` or `USkeletalMesh` in a new private `FPreviewScene`, through an ownerless `USceneCaptureComponent2D` and transient render target. It neither opens or closes an asset editor nor reads or changes the active level world, an editor viewport, or the process-wide `UAssetViewerSettings` profiles. One RPC owns the transient scene state and reuses it across every requested shot, so one caller cannot leave lighting, camera, or world state for a later caller.

```js
call({
  path: "render.capture_mesh",
  args: {
    assetPath: "/Engine/BasicShapes/Cube.Cube",
    width: 768,
    height: 768,
    exposure: { mode: "fixed", ev100: 0 },
    viewMode: "front_back_face",
    previewScene: {
      key: { azimuth: 112.5, elevation: 40, intensity: 3.14 },
      sky: { intensity: 1 },
      showFloor: true,
      showEnvironment: true
    }
  }
})
```

`assetPath` is required. The camera fits the mesh bounds when `location` and `rotation` are omitted; `padding` defaults to `1.25`. `count` captures 1-24 evenly spaced azimuths at `elevation` (default 20), while `views:"sides"` captures the six cardinal views; either set form conflicts with an explicit pose. `measureCoverage` defaults to true and redraws each pose with only the mesh hidden, then reports `subjectCoverage`; pass false to avoid the extra draw. A frame is capped at 16 Mi pixels and the whole request, including coverage redraws, at 64 Mi rendered pixels. `width` / `height` default to 768, projection defaults to perspective, and PNGs are written under `Saved/Screenshots/MeshCapture`.

`previewScene` has the same key/sky/floor/environment request shape as `render.capture_asset_preview`, but the floor and environment are transient components rather than shared profile state. `viewMode` uses the scene-capture subset documented in [`render.view-modes`](render.view-modes.md), including `front_back_face`; renderer-only modes are refused rather than returned as Lit under another label.

Success returns the shared `path`, size, camera, projection, `viewport.previewScene`, `imageStats`, `blank`, and renderer fields. `imageStats` additionally carries the shared flat-region measurement. The transient rig reports `disposed:true` rather than fabricating a restore ledger for state destroyed with the RPC. `assetEditorOpened:false` and `activeWorldUsed:false` state the two isolation guarantees directly; a set adds `shots[]`, `count`, and each shot's azimuth/elevation. This route does not animate a skeletal mesh; use `render.capture_animation_preview` when the pose needs an animation time.

### render.capture_asset_preview

Capture an asset-editor preview through its real preview viewport, as a still or shot set. It opens the subject's editor/designer, fixes `width` × `height`, reads pixels, writes under `Saved/Screenshots/AssetPreview`, and restores the camera. **It closes the editor by default** — see `closeAfterCapture`. It does not use `actor.spawn`, place the mesh in the active level, or dirty the level. It serves **Static Mesh, Skeletal Mesh, animation assets and Niagara systems**; an unsupported editor returns `UNSUPPORTED_ASSET_EDITOR` naming the toolkit. See [the shared surface and capture contract](render.md#scope-and-where-to-start) for routing and common behavior.

Orthographic example:

```js
call({
  path: "render.capture_asset_preview",
  args: {
    assetPath: "/Engine/BasicShapes/Cube.Cube",
    width: 1024,
    height: 1024,
    location: { x: 0, y: 0, z: 0 },
    rotation: { pitch: 0, yaw: 0, roll: 0 },
    projectionMode: "orthographic",
    orthoWidth: 2000
  }
})
```

Important fields:

- `assetPath` is an object path such as `/Engine/BasicShapes/Cube.Cube`; supply it **or** `subject`. It is **optional**, so an empty payload is `INVALID_ARGUMENT` from the handler body (formerly dispatcher's `MISSING_REQUIRED_PARAM` for the one required slot).
- `subject` uses `{kind, path}` for a static mesh, skeletal mesh, animation asset or Niagara system, or `{kind:"skeletalMesh", path, animation}` to pose a mesh. `assetPath` normalises into it and infers kind from the loaded `UClass`; `subject.radius` is ignored. A world/actor subject is `UNSUPPORTED_ASSET_EDITOR` and points to [`render.capture_open_level`](render.capture_open_level.md). Full descriptor: [`render.capture-subjects`](render.capture-subjects.md).
- `count` / `views` / `angles`-free shot sets. `views: "sides"` is the only accepted `views` value and plans the six axis-aligned views; anything else is `INVALID_ARGUMENT` naming `'sides'`. `count` gives evenly-spaced shots, spread by `distribution` (`"ring"`, the default, or `"sphere"`) and jittered reproducibly by `seed`; `elevation` sets the ring's height and defaults to 30. On a set the response gains `shots[]`, `count` and `shotDistribution`; a single still has none of the three. **An orthographic set cannot hold an `elevation` at all, including the default** — read `shotDistribution.elevationIgnored` / `.elevationIgnoredReason` / `.elevationRequestedDegrees` and see § *An orthographic set cannot hold an `elevation`* above.
- `allowBlank` (boolean, default `false`) accepts a near-uniform black readback instead of `BLANK_CAPTURE`; `target` defaults to `preview` and v1 rejects other targets. `width` / `height` default to `768` and cap at `16384`.
- `projectionMode` is `perspective` or `orthographic`. Orthographic needs `pitch:-90`, `pitch:90`, or `pitch:0` with yaw `0` / `90` / `180` / `-90`; more than 1° off an axis is `UNSUPPORTED_ORTHOGRAPHIC_ROTATION`. `orthoWidth` is world width in cm (default `2000`), with alias `orthoWorldWidth`; older builds treated raw zoom `512` as roughly 70 cm.
- `viewMode` is scoped to this capture and restores both previous mode slots; see [`render.view-modes`](render.view-modes.md) for vocabulary and refusal codes.
- `closeAfterCapture` **defaults to `true`** and has three states, not two. Omitted, it closes only a window *this call* opened and leaves one you already had open alone. Passed `true` explicitly, it closes the window either way — including one an earlier capture of the same asset left behind, which the old guard silently refused to do. Passed `false`, it never closes. The response reports the measured `assetEditorClosed` and `assetEditorWasAlreadyOpen` rather than echoing the request. The default changed from `false` because a window leaked per call is nobody's intent and an asset editor still open at exit crashes UE 5.8 during shutdown.
- **`viewDistanceScale` and `hideEditorSprites` are not declared here, so passing either is `UNKNOWN_PARAMS` naming the field.** Neither has anything to act on: a preview scene has no distance-culled level content and no icon sprites. The refusal is the dispatcher's, so it costs nothing and leaves no PNG.
- Success returns `path`, `filename`, `width`, `height`, `sizeBytes`, `assetPath`, `captureSource:"staticMeshEditorPreview"` (or the preview source for the kind that was served), `renderer:"sceneViewportReadPixels"`, `mimeType:"image/png"`, `assetEditorClosed`, `assetEditorWasAlreadyOpen`, `resolutionSource`, `poseSet`, and — when a subject resolved — `subject`.
- `cameraLocation` / `cameraRotation` are measured and `viewport.aim` gives the verdict; see [camera placement](render.md#the-camera-goes-where-you-asked-and-the-response-says-where-it-went). `framing` answers whether the mesh is in view; `blank` cannot — see [the empty-frame rule](render.md#an-empty-frame-is-not-a-blank-frame).

**Closing the asset editor is DEFERRED, and the response says so.** Destroying an asset editor
toolkit on the capture's own stack is an `EXCEPTION_ACCESS_VIOLATION` that takes the whole editor
down — observed through both the mesh and the Niagara provider, with an identical callstack from
`CaptureSubject::CloseAssetEditor` outward. So the close is queued onto the next editor tick
instead. Three fields, and you need all three to read the outcome:

| field | meaning |
|---|---|
| `assetEditorWasAlreadyOpen` | you already had this window open before the call. Also the field that separates a cold first frame from a warm one |
| `assetEditorClosed` | MEASURED: the window was gone when this call answered. **Normally `false` now**, because the close runs a tick later |
| `assetEditorCloseDeferred` | a close is queued and the window is gone on the next tick |

`assetEditorClosed: false` **with** `assetEditorCloseDeferred: true` is the healthy default outcome.
`false` with `false` means the window was deliberately left open. Verify a close the way the engine
reports one — `LogSlate: Window '<AssetName>' being destroyed` — not by grepping for the word
"close", which the engine never logs.

**`closeAfterCapture: false` keeps the window only until the next capture opens one.** At most one
capture-opened asset editor is left open at a time; the previous one is queued for close when the
next capture starts. A window you opened yourself is never evicted. This is deliberate: an asset
editor still open when the editor exits faults during shutdown, and an uncapped session measured
29 of 37 opened editors still alive at the end of it.

### render.capture_open_level

Capture the active Level Editor viewport, apply the caller's camera/projection/size, save under `Saved/Screenshots/OpenLevel`, and restore viewport state. It does not require PIE or use `GEngine->GameViewport` (that older path belongs to `call("editor.screenshot")`). V1 uses no `SceneCapture2D`, so it matches editor-visible level state but requires an active Slate Level Editor viewport. See [the shared surface and capture contract](render.md#scope-and-where-to-start) for common routing and fields.

Top-down orthographic example:

```js
call({
  path: "render.capture_open_level",
  args: {
    filename: "level_topdown.png",
    width: 2048,
    height: 2048,
    location: { x: 0, y: 0, z: 5000 },
    rotation: { pitch: -90, yaw: 0, roll: 0 },
    projectionMode: "orthographic",
    orthoWidth: 20000
  }
})
```

Orthographic views are **axis-aligned only**, and that is an engine constraint rather than a plugin policy: `FEditorViewportClient::CalcSceneView` builds the orthographic view matrix from the viewport *type* (`LVT_OrthoXY`, `LVT_OrthoXZ`, …) and never reads the camera rotation, so exactly six directions can be rendered. The call maps the requested rotation onto the matching type; a pose more than 1° off every world axis returns `UNSUPPORTED_ORTHOGRAPHIC_ROTATION` instead of silently capturing a different camera. A separate `SceneCapture2D` renderer remains the right path for genuinely arbitrary offscreen orthographic rotations.

Because the engine also fixes the in-plane orientation of each axis view, the rendered pose can differ from the requested one by a rotation about the view direction. `pitch:-90, yaw:0` renders as the engine's top view, i.e. `pitch:-90, yaw:180` (screen up is world **−X**, screen right is world **−Y**). The response reports what the pixels actually show:

- `cameraRotation` — the effective pose. Feed **this** back into `spatial.raycast_screen`.
- `requestedRotation` — present only when the two differ.
- `orthoView` — `top` / `bottom` / `front` / `back` / `left` / `right`.

`orthoWidth` is the frame's **world width in centimetres** (default `2000`; alias `orthoWorldWidth`) — the world span the image covers left to right. Vertical coverage follows from the aspect ratio. In earlier builds the value was passed to the editor's raw ortho zoom, where `512` framed roughly 70 cm at 1024 px and the framing depended on the pixel width.

Important fields:

- No asset path is required; the target is the active opened level viewport. `subject` accepts only `world` or `actor` and **does not move the camera**: `location` / `rotation` still set the pose, while `subject` adds `framing` and a `subject` block. An asset kind is `UNSUPPORTED_ASSET_EDITOR` naming [`render.capture_asset_preview`](render.capture_asset_preview.md).
- `width` / `height` default to `768` and cap at `16384`; `projectionMode` is `perspective` or `orthographic` (the latter is axis-aligned as above).
- `allowBlank` defaults to `false`: a near-uniform black readback is redrawn once, then fails `BLANK_CAPTURE`; use `allowBlank:true` only for an intentional black frame.
- `allowPieWorld` defaults to `false`. Normally a Level Editor viewport whose world differs from the active editor world is refused with `VIEWPORT_WORLD_MISMATCH`. Pass `allowPieWorld:true` only when intentionally capturing a Level Editor viewport that is still bound to a PIE world after eject; the exception requires an actual `PIE` world whose package matches the active editor map after removing its `UEDPIE_N_` prefix. Another-map PIE, stale non-PIE, and unrelated worlds remain refused.
- `hideEditorSprites` defaults to `false`; pass `true` for an acceptance shot. Its full flag, restoration and editor-chrome contract is below.
- `viewMode` is scoped to this capture and restores both mode slots on every exit; see [`render.view-modes`](render.view-modes.md).
- Success returns `path`, `filename`, `width`, `height`, `sizeBytes`, `levelPath`, `target:"openLevel"`, `captureSource:"levelEditorViewport"`, `renderer:"sceneViewportReadPixels"`, `allowPieWorld`, `capturedPieWorld`, `pieWorldSameMap`, and `mimeType:"image/png"`, plus `orthoWidth` + `orthoView` (and `requestedRotation` when the pose was snapped) for orthographic captures. `levelPath` names the world the viewport actually rendered, including the PIE package when the opt-in was used.
- Every success also reports `activeWorld` / `activeWorldPackage`, `viewportWorld` / `viewportWorldPackage`, `worldMatches`, applied `cameraLocation` / `cameraRotation`, `viewport`, `imageStats`, `blank`, `redrawRetries`, and, when the tone range was measured, `crushed`, `blownOut` and `rangeWarning`; see [the shared response-shape rules](render.md#a-set-reports-its-own-shape-and-omits-what-it-cannot-say).

**`blank` does not depend on the capture size, and the two numbers it is made of are published.** A frame counts as blank when its mean luminance is at or below `0.01` **and** fewer than the floor of lit pixels exceed `litLuminanceThreshold` (0.02) — where the floor is 64 pixels, or 0.05% of the frame on a frame smaller than ~128k pixels. Both halves are needed because the two kinds of content in a viewport frame hold still in different ways: real scene content keeps its lit *fraction* as the frame grows, while a fixed-pixel editor overlay (world-axis gizmo, stats text) keeps its lit *count* and loses its fraction. The previous criterion used luminance variance, which measures share alone, and therefore gave the same camera in empty space `blank:false` at 256 and 512 and `blank:true` at 1024 and 2048 — so raising `width` to get more detail turned a working capture into a hard `BLANK_CAPTURE`. If you need a stricter "is there anything here" test than `blank` gives, read `litPixelFraction` and decide for yourself; comparing it across sizes of the *same* scene is meaningful, comparing `litPixelCount` across sizes is not.

**`hideEditorSprites` — pass `true` for an acceptance shot.** A level viewport draws editor-only icon sprites over the geometry: light bulbs, audio icons, player start, note and camera icons, and the directional-light arrow. They are not in the level and they are not in a packaged build, but they are in the pixels, and a reviewer reading a screenshot cannot always tell an icon from something standing in the scene. `hideEditorSprites:true` clears `EngineShowFlags.BillboardSprites` for the duration of the capture and puts it back afterwards — that one flag is what `UBillboardComponent`, `UMaterialBillboardComponent` and `UArrowComponent` gate their draw relevance on (`Runtime/Engine/Private/Components/BillboardComponent.cpp:214`), and it is the only flag the icon primitives read.

The default is `false` because flipping it would silently change the pixels every existing caller already gets, which a diff-based review would report as a change in the level. Opt in per shot.

What it does **not** cover, because these are separate mechanisms with separate levers: selection outlines, the grid, editor-mode handles (a landscape or spline tool keeps drawing while its mode is active), and component visualizers such as the spline handles on a selected water body. For those use `call("editor.set_game_view")`, which this parameter deliberately does not call — game view *swaps* the viewport's two show-flag sets rather than setting a bit (`EditorViewportClient.cpp:7221-7252`) and cannot be round-tripped safely inside a capture, and it would not remove the sprites on its own anyway: `BillboardSprites` is absent from `FEngineShowFlags::Init`, so it is on in the game flag set too, and game view hides icons through `FPrimitiveSceneProxy::IsShown` instead.

Every success reports a `viewport.editorSprites` block — `hideRequested`, `visible` (the flag the pixels were actually drawn with, so `true` means icons could be in this frame whether or not anybody asked), `billboardSpritesBefore`, `billboardSprites`, `restored`, plus a `hideWarning` if suppression was requested and the flag was still set, and a `restoreWarning` if the capture left the flag moved. `render.capture_annotated` and all three `camera.*` verbs take the same parameter and report the same block — `camera.animation_shots` included, which used to be the one level-viewport verb without it. `render.capture_asset_preview` and `render.capture_animation_preview` do not **declare** it, so passing it there is `UNKNOWN_PARAMS` rather than a no-op; the reason is in [`render.capture_animation_preview`](render.capture_animation_preview.md)'s gotchas.

The handler rejects a stale or unrelated active viewport with `VIEWPORT_WORLD_MISMATCH` before changing it unless the narrow `allowPieWorld:true` exception above applies. The error details report `allowPieWorld`, `viewportIsPieWorld`, normalized `editorMapName` / `viewportMapName`, `pieWorldSameMap`, and a machine-readable `reason` (`allowPieWorldRequired`, `viewportWorldIsNotPie`, or `pieMapMismatch`). A `BLANK_CAPTURE` response includes the same world, viewport, applied camera, and image-stat fields as success, so the caller can distinguish a wrong world, wrong pose, non-lit view mode, and a redraw that still produced black pixels. The capture restores the original camera, projection, FOV/ortho zoom, fixed viewport size, and temporary realtime override on every exit, including the retry and both typed errors.

**A wide orthographic frame culls its own content, and the capture corrects for it.** Two effects stack. First, an orthographic frame's world coverage comes from `orthoWidth` and does not change with camera distance, so a 64000 cm top-down has frame corners tens of thousands of centimetres from the camera while its centre is a few thousand. Second — and this is the larger term by two orders of magnitude — **the point the renderer culls from is not the camera.** A *lit* orthographic editor view pushes its view origin backwards along the view direction by `UE_OLD_WORLD_MAX` (2097152 cm): `EditorViewportClient.cpp:1420` sets `OrthoNearClipPlane = -UE_OLD_WORLD_MAX` and `SceneView.cpp:609` applies it, and `SceneVisibility.cpp:867` then culls against that origin. So every primitive in the frame sits ~2.1e6 cm from the measuring point however close the camera is. The pushback is conditional — it needs `ViewMode > VMI_Unlit` and `r.Ortho.AllowNearPlaneCorrection != 0`, so a wireframe or unlit ortho has none — which is why the capture *measures* the origin off the `FSceneView` it is about to draw instead of assuming a constant.

Foliage is the worst-hit case because forest cover *is* instanced geometry: `HierarchicalInstancedStaticMesh.cpp:1675` folds `EndCullDistance * r.ViewDistanceScale` into its `FinalCull` and drops whole cluster nodes past it, and `SceneVisibility.cpp:998` scales ordinary primitives' `MaxCullDistance` by the same value. Note what is *not* the cause — the screen-size cull is already disabled for orthographic views (`HierarchicalInstancedStaticMesh.cpp:1656`), so foliage with no cull distance set was never affected.

- `viewDistanceScale` (number, optional, must be `> 0`) forces `r.ViewDistanceScale` for the capture and restores it afterwards.
- **Omitted on an orthographic capture**, the value is derived: the world's primitive components are surveyed once from the measured culling origin, and the scale is the largest `reach / cullDistance` ratio **over the primitives that can actually be culled**, each against its own cull distance, plus a 5% margin. It is deliberately not `maxPrimitiveDistance / minCullDistance` — that form pairs the reach of one primitive with the cull distance of an unrelated one, so a single actor with a huge bounding sphere pins the result to the cap regardless of what is at risk.
- **Omitted on a perspective capture**, nothing is touched. A perspective frame's coverage and its cull distances scale together, so it does not have this failure by construction — and auto-enabling it there would move every measurement already taken with this verb. Pass the parameter explicitly to override a perspective shot.
- Passing `0` is an `INVALID_ARGUMENT`, not an opt-out: `0` is also a legal `r.ViewDistanceScale` meaning "cull everything", and the two readings are opposite. Omit the field to opt out.

**The caps, and why they are not arbitrary.** The derived scale is bounded by three things, and the response names which one bound: `int32` — the engine truncates `EndCullDistance * scale` into an `int32` at `HierarchicalInstancedStaticMesh.cpp:1675`, and overflowing that turns "cull nothing" into "cull everything", so the *product* is held at half of `INT32_MAX`; `nearCull` — the same cvar multiplies `MinDrawDistance` (`SceneVisibility.cpp:999`), so on a map that uses near-culling the scale is bounded to keep the closest primitive visible rather than trading one loss for another; `absolute` — a ceiling of `1048576` for degenerate scenes. On a normal map none of them binds: covering the 2.1e6 cm ortho reach for foliage culling at 500 cm needs ~4500, far below every cap. (The earlier `4096` cap was *below* that working range and only appeared to work because the giant-bounding-sphere ratio pinned every derivation to it.)

Every success reports a `viewDistance` block — `scaleBefore`, `scaleApplied`, `scaleEffective`, `overridden`, `restored`, `coverageSufficient`, `clamped`, `source` (`caller` / `auto` / `none`), `clampReason` when `clamped`, and, for a derived value, `survey` with `minCullDistance`, `maxPrimitiveDistance`, `minPrimitiveDistance`, `requiredScale`, `maxMinDrawDistance`, `primitives`, `distanceCulledPrimitives`, `instancedComponents`, `foliageMaxEndCullDistance`, `foliageCeilingLimited`, `cullingOrigin`, `cullingOriginMeasured` and `cullingOriginPushback`.

`scaleEffective` is read out of the renderer's own scalability cache after the write, **not** copied from `scaleApplied`: `r.ViewDistanceScale.ApplySecondaryScale` folds `SecondaryScale` into the value the renderer sees (`UnrealEngine.cpp:905-906`), and the cache refreshes only when a console-variable sink runs. `coverageSufficient` compares that effective value against the survey's `requiredScale` — it is the one field a capture cannot fake by succeeding. `restored` is likewise a read-back of the cvar after the capture. `clamped` is the machine-readable half of the cap disclosure: it is present on every capture, so a caller never has to pattern-match a warning string to learn that the derived scale was clipped, and `clampReason` names which cap bound (`nearCull` / `int32` / `absolute`) because the three have different remedies. Four warnings can appear: `viewDistanceWarning` when a cap bound (naming which, and what is still culled), `coverageWarning` when the renderer read a smaller scale than the scene needs, `cullingOriginWarning` when no `FSceneView` could be built and the survey fell back to the camera location — in that case the derived scale is a lower bound and distant geometry may still be missing — and `foliageCeilingWarning` when `foliage.MaxEndCullDistance` is set and instanced components reach past it.

**`foliage.MaxEndCullDistance` is the one cull term no `viewDistanceScale` can lift.** The engine clamps the instanced end-cull distance *after* multiplying it by `r.ViewDistanceScale` (`HierarchicalInstancedStaticMesh.cpp:1675-1686`), so a non-zero ceiling bounds what any scale reaches — and it also gives instanced components that set **no** end cull distance of their own a finite one, which is how a forest that looks un-cullable in a survey can still render empty. Note that a scale cannot help *those* either: the multiply is `EndCullDistance * MaxDrawDistanceScale`, and zero times any scale is zero, so the ceiling becomes their cull distance outright. The survey therefore reads the cvar, excludes components past the ceiling from `requiredScale` (asking for a scale the engine will clamp away is how a cap becomes load-bearing by accident), counts them in `foliageCeilingLimited`, and says so in `foliageCeilingWarning` — it does not turn the ceiling into a requirement. At the cvar's `0` default the ceiling is disabled outright and none of this applies. The remedy is raising or clearing that cvar, or narrowing `orthoWidth` so the frame stays inside the ceiling — not a larger `viewDistanceScale`.

### render.capture_annotated

Capture the active Level Editor viewport, then paint measured overlays onto the PNG so an agent gets a metrically-annotated view instead of a bare screenshot.

Takes the same base capture args as `render.capture_open_level` (`location`, `rotation`, `projectionMode`, `fov` / `orthoWidth` in world cm, `width`, `height`, `filename`, `hideEditorSprites`, `viewMode`) — including the axis-aligned-only orthographic rule — plus the overlay toggles:

It also takes `viewDistanceScale` and reports the same `viewDistance` block — see `render.capture_open_level` above for the derivation, the caps and `coverageSufficient`. Annotations are drawn over whatever the capture rendered, so an annotated ortho whose content was distance-culled produces measured-looking overlays on missing geometry. The **automatic** wide-orthographic derivation runs on a level frame only; an explicit `viewDistanceScale` you pass is applied on either kind of frame.

- `subject` (object) — what the overlays are drawn over. Omit it and the verb annotates the active Level Editor viewport exactly as before. Full shape: [`render.capture-subjects`](render.capture-subjects.md), and the four paragraphs below for what changes when the subject is an asset.
- `axes` (boolean, default `true`) — draw the world X/Y/Z axes at the origin (red/green/blue).
- `grid` (object) — a Z=0 ground grid: `{ spacing: cm between lines, extent: cm half-size from origin }`. Omit to skip it. **Those two keys are the whole schema** — that brace is the contract, not an example, and any other key inside `grid` is refused with `UNKNOWN_NESTED_PARAMS` naming it. It used to be accepted and discarded: `grid.between`, `grid.lines` and `grid.origin` are all spellings callers invented from the prose, and the response echoed `overlays.grid.spacing` back as if it had been the request.
- `bounds` (array) — actor names (display label / internal object name / object path) to outline with a projected world AABB wireframe.
- `labels` (boolean, default `true`) — axis letters, grid spacing, and each actor's name + `WxDxH` world size in cm.
- `actorLabels` (object) — **opt-in actor discovery**; see below. Omit for the previous behavior.
- `inline` (boolean, default `false`) — also embed base64 PNG bytes.

**`subject` accepts every kind, and an asset kind moves the whole call into a preview scene.** A `world` or `actor` subject annotates the Level Editor viewport. A `staticMesh`, `skeletalMesh`, `animation` or `niagara` subject opens — or reuses — that asset's editor and takes **both the frame and the overlay projection** from its preview viewport. One viewport client serves both halves, which is the point: the axes and the Z=0 grid then measure the preview scene, where the asset sits at the origin, not the open level. Do not read a preview frame's grid as a statement about where anything is in your map.

**`bounds` and `actorLabels` are level-only.** Combining either with an asset subject is `INVALID_ARGUMENT` naming the argument and the kind. Both resolve names against the editor world, and a preview scene contains none of its actors — so every name would come back `found:false` and the actor map would be empty, which reads as an *empty level* rather than as the wrong world. That silent misreading is what the refusal exists to prevent.

**The response says which viewport drew it.** `captureSource` is `levelEditorViewportAnnotated` for a level frame, and for a preview frame it is the resolver's own source name with `Annotated` appended: `staticMeshEditorPreviewAnnotated`, `personaPreviewViewportAnnotated`, `niagaraSystemEditorPreviewAnnotated`. `levelPath` is emitted **only** for a level frame. `subject.assetEditorClosed` is measured after the subject is released — the editor is asked whether it still has an editor open for that asset — not predicted from what was requested.

**`viewDistanceScale` and the automatic wide-orthographic scale are level concerns.** The auto-derivation is skipped outright on a preview frame.

```js
call({
  path: "render.capture_annotated",
  args: {
    filename: "scene_annotated.png",
    width: 1280, height: 720,
    location: { x: 900, y: 0, z: 400 },
    rotation: { pitch: -20, yaw: 180, roll: 0 },
    grid: { spacing: 100, extent: 1000 },
    bounds: ["Wall_1", "Crate_2"]
  }
})
```

Draws world XYZ axes (RGB), a Z=0 grid (capped at ~20 lines per axis half), and per-actor 12-edge AABB wireframes labeled with the actor NAME and its `WxDxH` size. Returns the annotated PNG `path` plus an `overlays` echo (and `base64` when `inline`).

**The image statistics describe the base frame, not the file you get back.** This verb publishes `imageStats`, `blank`, `redrawRetries`, and — gated on the tone range having been measured — `crushed`, `blownOut` and `rangeWarning`, exactly as `render.capture_open_level` does. All of them are measured **before a single overlay pixel is painted**, and that is deliberate: axes, grid and labels are synthetic pixels at fixed saturated colours, so folding them into the measurement would lift the mean, invent tone levels that no scene content produced, and turn a render of nothing at all into a non-blank frame. The numbers answer "was there anything in the capture", which is the only question they are useful for; the annotated PNG on disk is brighter than they say.

**Discovering actors you did not know about (`actorLabels`)**

`bounds` needs the names up front, so it can only confirm what you already know. `actorLabels` inverts that: it finds every actor matching a filter, projects it to screen space, paints its name in cyan, and — more usefully — returns a machine-readable **actor → pixel map** so you never have to read names out of the image.

```js
call({
  path: "render.capture_annotated",
  args: {
    filename: "discover.png",
    width: 1600, height: 900,
    location: { x: 0, y: 0, z: 5000 },
    rotation: { pitch: -90, yaw: 0, roll: 0 },
    projectionMode: "orthographic", orthoWidth: 20000,
    actorLabels: { folder: "Blockout/Towers", minScreenArea: 400, maxLabels: 40 }
  }
})
```

`actorLabels: true` is shorthand for `actorLabels: {}` (all defaults). Sub-fields, all optional:

- `folder` (string) — World Outliner folder **prefix**, case-insensitive: `Blockout/Towers` also matches `Blockout/Towers/North`.
- `tag` (string) — exact tag match, same rule as `actor.find_by_tag`'s default.
- `className` (string; alias `class`) — short name or full path, subclasses included, same rule as `actor.find_by_class`. An unresolvable class is `CLASS_NOT_FOUND` **before** anything is captured, so no PNG is left behind.
- `filter` (string) — matched case-insensitively against the internal name **and** the display label. A value containing `*` or `?` is treated as a wildcard pattern (`SM_*_Tower`); otherwise it is a substring. Same promote-on-metacharacter rule as `asset.search`.
- `minScreenArea` (number, default `256`) — px² floor on the actor's visible projected footprint, so distant clutter drops out automatically. `0` keeps even zero-footprint actors (lights, empty actors).
- `maxLabels` (number, default `50`) — hard cap; `0` means "no cap of mine" and still stops at 500.
- `minSpacing` (number, default `24`) — px between two **painted** anchors, see the overlap note below.
- `draw` (boolean, default `true`) — `false` returns the map without painting anything. `labels:false` has the same effect on the painting and also still returns the map.

Response adds a top-level `actorLabels` block (and `overlays.actorLabels:true`):

```json
{
  "actorLabels": {
    "actors": [
      { "name": "SM_Tower_3", "class": "StaticMeshActor", "folder": "Blockout/Towers/North",
        "px": 812, "py": 344, "screenArea": 1840, "distance": 4211,
        "onScreen": true, "drawn": true }
    ],
    "count": 40, "totalMatches": 214, "truncated": true, "scanned": 3120,
    "drawn": 34, "overlapSkipped": 6,
    "dropped": { "behindCamera": 118, "offScreen": 2680, "belowMinScreenArea": 108, "cap": 174 },
    "maxLabels": 40, "minScreenArea": 400, "minSpacing": 24, "folder": "Blockout/Towers"
  }
}
```

- `name` is the **unique internal object name** — the collision-safe key to hand straight to `actor.select`, `actor.describe` or `spatial.measure_distance`. `label` appears only when the display label differs from it.
- `px` / `py` are the actor's **bounds-centre** pixel (top-left origin), not its pivot — the pivot of a floor slab can sit metres from the thing you can see. Feed them to `spatial.raycast_screen` with the capture's echoed `cameraLocation` / `cameraRotation` / `width` / `height`.
- `screenArea` is the actor's **visible** (frame-clipped) projected area in px², and it is the ranking key: the cap keeps the most visually prominent actors, not whichever ones the world iterator reached first. Ties break by `distance`, then by `name`, so two runs over an unchanged scene return identical output.
- `onScreen:false` means the centroid projected outside the frame while the actor's **bounds** still cross it — those are included on purpose (they are the half-visible things at the edges you most want to know about). Such a row carries an extra `rect: {x,y,w,h}`, the clipped in-frame rectangle; use its centre as the raycast pixel, because `px`/`py` is off-image.
- `count` / `totalMatches` / `truncated` follow `actor.list`. `scanned` is how many actors passed the filters and were projected, and `dropped` explains the rest: `scanned == behindCamera + offScreen + belowMinScreenArea + totalMatches`, and `totalMatches == count + dropped.cap`.

**Why this beats a raycast for discovery.** `spatial.raycast_screen` is a physics trace, so a collision-less actor — decals, most lights, `NoCollision` blockout meshes, editor-only volumes — is invisible to it, and it answers one pixel per call. `actorLabels` reads *render* bounds (`GetActorBounds(false, …)`), so it sees those actors, and it answers the whole frame in one call. Use the raycast afterwards when you need the exact world point under a pixel.

Gotchas: overlay geometry uses no near-plane clip, so any element **behind the camera is skipped** rather than wrapped. A grid whose `extent`/`spacing` would exceed the line cap reports it via `overlays.grid.clamped`. Like `render.capture_open_level`, an orthographic pose must look along a world axis; a tilted one is rejected with `UNSUPPORTED_ORTHOGRAPHIC_ROTATION`. Overlays are projected through the same helper the capture used, so a top-down orthographic frame's axes, grid and AABBs land on the pixels they belong to — including the engine's fixed in-plane orientation (world −X up on the top view).

`actorLabels` gotchas:

- **Budget.** ~50 rows is roughly where the `actorLabels` block alone reaches the 10,000-char inline response budget and spills to `Saved/PinWright/HttpResponses/`. Narrow with `folder`/`tag`/`className`/`filter` and raise `minScreenArea` before raising `maxLabels`.
- **Behind the camera is dropped, not labelled.** A point behind the lens still *projects* — mirrored, through `|W|` — so publishing it would hand you a plausible pixel pointing at the wrong thing. Those actors land in `dropped.behindCamera`. In **orthographic** mode this is stricter than the render: the editor derives an ortho view's near plane from the world bounds rather than from the camera, so geometry behind an ortho camera is genuinely drawn. Place the ortho camera outside the scene (as the top-down example does) and it cannot bite.
- **Label de-overlap is cheap and honest.** A label is not *painted* when its anchor lands within `minSpacing` px of one already painted (`overlapSkipped` counts them), and long names can still overrun a neighbour horizontally — it is anchor-proximity rejection, not true text-extent layout, and it does not see the axis/grid/`bounds` labels the same call may paint. The **row is always returned** with `drawn:false`, so the map never loses an actor to a drawing decision. When the picture is too crowded to read, pass `draw:false` and work from the map.
- **Editor world only.** There is no `world` argument: the verb annotates the Level Editor viewport it just captured, so the editor world is the only pose-coherent choice. For PIE actors use `actor.list {world:"pie"}`.

### render.capture_animation_preview

Capture a skinned mesh **in isolation** at one or more instants of an animation: no level, no placed actor, no Level Sequence. Opens the asset's Persona-family editor, freezes the preview at each requested frame, and shoots that preview viewport from one or more angles.

Use this when the question is about the mesh and its animation. Use [`camera.animation_shots`](camera.animation_shots.md) when the question is about a *placed* actor being driven by a Level Sequence in the level — that one shoots the level viewport and needs a sequence.

`subject` (object) names what to capture. `assetPath` normalises into it; `{kind:"skeletalMesh", path, animation}` is the explicit form and is what supplies both mesh and motion. An animation asset supplies its own mesh through the asset's preview mesh. With no animation the burst collapses to one image — every instant would be identical — and says so in `warnings[]`. See [`render.capture-subjects`](render.capture-subjects.md).

**It serves two kinds and refuses the other four by name.** An explicit `subject.kind` of `skeletalMesh` or `animation` is served; `world`, `actor`, `staticMesh` and `niagara` each get an `UNSUPPORTED_ASSET_EDITOR` naming this verb's Persona-family gate and pointing at the verb to use instead — [`render.capture_asset_preview`](render.capture_asset_preview.md) for a static mesh or a Niagara system, [`camera.orbit_shots`](camera.orbit_shots.md) / [`camera.frame_actor`](camera.frame_actor.md) for a world point or a placed actor. `assetPath` is **optional** in the declaration now, and a payload with neither it nor `subject.path` is refused in the handler body with `MISSING_REQUIRED_PARAM` naming both spellings.

It also takes `viewMode` (scoped and restored, read once for the whole burst) and `distribution` / `seed`, which spread and reproducibly jitter the view plan the same way [`camera.orbit_shots`](camera.orbit_shots.md) does.

**Bounds come from the mesh asset, never from the posed component.** `GetBounds()` on the skeletal mesh is bit-identical at every instant, so the one camera solved from it does not re-fit between frames; two shots of a gait differ only by the pose. A limb reaching outside the rest bounds is why `padding` defaults to `1.4` here rather than the static-mesh `1.15`.

**A burst publishes only a pose whose render state was synchronized.** After each frame is
evaluated, the preview world's deferred skeletal render update is drained before the viewport is
drawn. If the preview component has no live render state or no world to perform that drain, the
capture fails instead of writing a stale PNG. `poseChanged` remains component-pose evidence, not a
replacement for checking the written images.

Response fields:

| Field | Meaning |
| --- | --- |
| `poseChanged` | The verdict. `false` with an animation loaded and more than one instant sampled means the animation carries no bone tracks, or every sampled frame landed on the same pose — and a warning says which. |
| `poseSampled` | `false` means the preview component reported no component-space bone transforms at any instant: its animation system never evaluated. Not measured is not measured clean. |
| `frames[]` | Per instant: `index`, `frame`, `time`, `poseSampled`, `boneCount`, `achievedTime` (the position the single-node instance actually reached, read back rather than echoed), and from the second instant on `poseDeltaFromPrevious` / `poseDeltaFromFirst`, each `{comparable, maxBoneTranslationCm, maxBoneRotationDegrees, movedBoneCount, componentTranslationCm}`. |
| `shots[]` | Per image: `frameIndex`, `frame`, `time`, `viewIndex`, `angle` `{azimuth, elevation}`, plus the shared shot fields — `path`, `filename`, `width`, `height`, `sizeBytes`, `projectionMode`, measured `cameraLocation` / `cameraRotation`, `fov` or `orthoWidth` + `orthoView`, `renderer`, `mimeType`, `imageStats`, `blank`, `subjectRegion`, its own **`viewport`** block, and a `framing` verdict when one could be measured. The per-shot `viewport` is what makes `aim.orbitCameraSuppressed` readable: it is now measured per capture, where an outer toggle used to switch orbit off once for the whole burst and leave every shot reporting `false` for a viewport that genuinely was orbiting. |
| `count` / `frameCount` / `viewCount` / `framePlanSource` | `count` is the number of images, `frameCount × viewCount`. `framePlanSource` names where the instant list came from, including `bindPose` for the collapsed no-animation case. There is **no** `angles` key and **no** `views` key in the response — angles are per shot, and the view-plan size is `viewCount`. |
| `assetPath` / `skeletalMeshPath` / `animationPath` | Resolved paths, so a call that named an animation asset can see which mesh it got. |
| `animationLengthSeconds` / `frameRate` / `frameRateAssumed` | `frameRateAssumed: true` means the rate is not the asset's own and the frame numbers are approximate. |
| `captureSource` / `editorName` | `personaPreviewViewport`, and the toolkit that served it. |
| `previewComponentCount` | More than one preview mesh in the scene raises a warning: the others are in frame and are **not** driven by this call. |
| `orbitCameraWasOn` | A preview viewport is in orbit mode by default, which used to swallow the requested pose entirely. See *The camera goes where you asked* above; `viewport.aim` is the verdict. |
| `width` / `height` / `resolutionSource` | `caller`, `budget` or `singleStill`; every omitted-size policy currently resolves to 768. The policy name remains distinct from the pixel value and is resolved once for the whole burst. |
| `poseSet` / `shotDistribution` / `subject` | The shared blocks. `poseSet` is half conditional keys and `subject` is absent when none resolved — see *A set reports its own shape* above. |
| `cameraDistance` / `framedCenter` / `framedRadius` | The one solved camera and the asset bounds it came from. |
| `blankShots` | Near-uniform black images in the set. Non-zero raises a warning; a blank set otherwise reads as a clean success. |
| `viewport` | The standard block — `lit`, `aim`, `warmup`, `exposure`, `viewModeOverride`. Reported once at top level, from the last capture. Its `viewModeWarning` is mirrored into `warnings[]`. |
| `assetEditorWasAlreadyOpen` / `assetEditorClosed` | Measured after `closeAfterCapture` has run, not echoed from the request. `assetEditorWasAlreadyOpen` is the field that separates a cold first frame from a warm one — see [`render.capture-exposure`](render.capture-exposure.md) § *The first capture into a fresh preview window is a stop dark*. |
| `warnings[]` | Present only when non-empty. |

Gotchas: an asset whose editor the resolver does not serve returns `UNSUPPORTED_ASSET_EDITOR` naming the toolkit, rather than a picture of something else. There is no world or actor subject here — this verb has no level; that is [`render.capture_open_level`](render.capture_open_level.md) and [`camera.animation_shots`](camera.animation_shots.md).

`hideEditorSprites` is not offered. `EngineShowFlags.BillboardSprites` gates only `UArrowComponent`, `UBillboardComponent` and `UMaterialBillboardComponent`, and the `FAdvancedPreviewScene` behind the Static Mesh and Niagara previews holds none of them and spawns no actors at all. The Persona preview world does hold actors, but the only one carrying a sprite is the `AWindDirectionalSource` the artist toggles on by hand — never the light bulbs, audio icons or player start the parameter documents. Passing it is `UNKNOWN_PARAMS`, since the verb does not declare it.

Whatever the frame's view mode, treat an **unlit** preview capture as *not measured* rather than as a pass: no material, lighting or colour judgement survives it. That is a view-mode rule, not a headless one — a `-unattended -RenderOffscreen` editor renders the reference fixture identically to an interactive one (see [`render.capture-exposure`](render.capture-exposure.md)).

### render.detect_z_fighting

Detect z-fighting from one camera and return an affected-pixel count, a pass/fail verdict, the responsible screen regions and actors, and a mask PNG.

It renders the scene **twice** through an offscreen scene capture with one clipping plane perturbed between the two, and flags every pixel whose surface identity changed. It never moves, resizes or reads the live viewport — an omitted `location` / `rotation` only *reads* the viewport camera as a default.

Response fields:

| Field | Meaning |
| --- | --- |
| `pass` | `false` when `affectedFraction` exceeds `minFraction`, **and also** when nothing could be measured. The two are distinguished by `warning`, below — never by `pass` alone. |
| `warning` | Present only when every pixel was excluded. This result is then not evidence of a clean frame: the camera is probably inside or against geometry, or the frame is empty. A `pass:true` with `affectedPixels:0` would have read as "checked and found nothing"; this reads as "could not check". |
| `affectedPixels` / `affectedFraction` | The count and its share of `analyzedPixels`. |
| `analyzedPixels` / `totalPixels` / `clipExcludedPixels` | What was measurable, the frame, and what the perturbed clipping plane removed. `analyzedPixels` is the denominator of `affectedFraction`. |
| `minFraction` | The gate the verdict was made against, echoed so a run can be re-judged without re-running. |
| `analysisWidth` / `analysisHeight` | The resolution the analysis ran at; the default is 768 × 768. A matched coplanar-slab measurement found 461 affected pixels / 8 regions at 1920 and 38 / 8 at 768; the separated control was 0 / 0 at both. Below 768 the response still adds `resolutionWarning`, because that range was not validated by this measurement. |
| `perturbation` / `nearPlane` | `nearPlane` or `farPlane`, and the base plane. With `nearPlane` perturbation the response also carries `nearPlaneRatio` and the resulting `perturbedNearPlane`; with `farPlane`, the `farPlane` used. |
| `channels[]` / `channelThreshold` | Per surface-identity channel (`baseColor`, `normal`): `{channel, flaggedPixels}`, so a result driven entirely by one channel is visible rather than inferred. |
| `forwardShading` / `shadingNote` | Present only under forward shading, where the renderer substitutes lit scene colour for the `baseColor` and `normal` G-buffer channels. The comparison still detects surface swaps but reacts to lighting too — raise `channelThreshold` if results look noisy. |
| `regions[]` | Largest first, capped by `maxRegions`. Each: `index`, `x` / `y` / `width` / `height` (the pixel bounding box), `pixels`, `centroidX` / `centroidY`, and — when a representative pixel could be sampled — `depth`, `worldLocation` and `actors[]` naming what sits at that point. `actors[]` is a bounds test, so it can name more than one and is a lead rather than a verdict. |
| `regionsReported` / `regionsFound` | The cap and the truth. `regionsFound > regionsReported` means the list is truncated. |
| `maskPath` / `maskFilename` / `maskWidth` / `maskHeight` / `mimeType` | Present only when `mask` is on. The mask is written at its own long edge and is not necessarily the analysis size. |
| `cameraLocation` / `cameraRotation` / `fov` / `cameraSource` | The pose the analysis used. `cameraSource` is `caller` or the viewport it was adopted from. |
| `projectionMode` | Always `perspective`. The verb has no projection argument; the probe renders perspective only. |
| `renderer` | `sceneCapture2D`. Same renderer as [`render.capture_ortho_tiles`](render.capture_ortho_tiles.md), and for the same reason never comparable pixel-for-pixel with a viewport capture — see *Two renderers* above. |
| `sceneRenders` | How many scene renders the probe actually issued. Two is the expected cost of one analysis. |
| `analysis` / `levelPath` | `zFighting`, and the level the pixels came from. |

Gotchas: 768 is the measured analysis default, not a promise that every lower resolution is safe. **`nearPlaneRatio` must not be a power of two, and `1` is one**: under the infinite-far reversed-Z projection a scene capture builds, scaling the near plane by a power of two scales every stored depth by that same factor with no change of rounding anywhere, so no depth comparison in the frame can change its outcome and the analysis reports zero affected pixels **on any scene**. That is refused with `INVALID_ARGUMENT` rather than run — a silent power-of-two ratio would ship the exact false negative the verb exists to prevent. The default `3` is fine. Use `perturbation:"farPlane"` when Nanite geometry produces scattered false positives.

### render.capture_ortho_tiles

Render the open level orthographically over a world extent you specify, as an N×M grid of georeferenced PNG tiles at a fixed per-tile resolution, plus a manifest `image.tile` / `image.annotate` / `image.compare` read back verbatim.

This is the capture half of the reference-image workflow: cut a reference with `image.tile` on the same georeference, and reference/capture tile `(r,c)` cover provably the same ground at any pixel counts because a georeference carries no resolution. It uses an offscreen scene capture, not the viewport; read *Two renderers* above before comparing its output.

**It does not dirty the level.** The component is ownerless, transient and outered to the transient package: no actor is spawned and no level package is touched. The response's `level` block measures the dirty-package count before and after.

```js
call({
  method: "render.capture_ortho_tiles",
  args: {
    worldMin: { x: -40000, y: -40000, z: 0 },
    worldMax: { x:  40000, y:  40000, z: 0 },
    axes: "top_down_x_right_y_down",
    cmPerPixel: 20,
    tilePixels: 768,
    exposure: 11,
    namePrefix: "map_ortho"
  }
})
```

**Three arguments are required and have no default**, because there is no answer that is right for every caller:

- `worldMin` / `worldMax` — the world box to cover, `{x,y,z}` in centimetres, either order per component. The two in-plane components (per `axes`) define the extent; the depth components define the **measurement plane** a reader's pixel→world answers land on, not where the camera goes. `depthCm` overrides that plane; it defaults to the midpoint, exactly as `image.tile` defines it.
- `axes` — a preset (`top_down_x_up_y_right`, `top_down_x_right_y_down`, `front_y_right_z_up`, `side_x_right_z_up`) or `{screenXAxis, screenXPositive, screenYAxis, screenYPositive}`. The two top-down presets are **both correct and produce transposed images**, so a default would silently mirror every coordinate read off the tiles. The camera rotation is derived from this, verified by classifying it back, and reported as `camera.rotation`; a mapping that does not survive that round trip is `AXIS_MAPPING_UNRENDERABLE` rather than a plausible-looking mirrored mosaic.
- `exposure` — required here although it is optional on every other capture verb. A scene capture keeps no persistent view state, so under auto-exposure **every tile exposes to its own content** and the mosaic steps at every seam. Pass a number (EV100), `{mode:"fixed", ev100:N}`, or `{mode:"auto"}` to accept per-tile exposure deliberately (you get a warning). One pin is written once for the whole burst — there is one component and one settings block — so "all tiles share an exposure" is structural, not a loop invariant.

Size the grid with **exactly one** of:

- `cmPerPixel` — the grid is sized to cover the extent at no coarser than this, then the extent is **expanded** symmetrically about its centre so the covered area is exactly whole tiles. Expansion, not cropping: cropping would silently drop world you asked to see. It is reported as `extentExpanded` plus a warning naming both spans, and the published georeference describes the **covered** extent.
- `cols` + `rows` — subdivide the requested extent exactly as given. The resulting centimetres-per-pixel must match on both axes; an orthographic frame has only an `orthoWidth` (its vertical span is forced to `orthoWidth × height / width`), so a non-square scale is unrenderable and is refused rather than stretched.

`tilePixels` (default 768) is the per-tile square pixel size, resolved once and held for the whole burst.

**Camera depth, and the near plane that makes it matter.** Omit `cameraDepthCm` and the camera is placed `cameraClearanceCm` (default 10000) beyond the world's own measured bounds along the view direction; the response reports `camera.depthCm`, `camera.depthSource`, the measured `camera.sceneBounds`, and `camera.contentBehindCameraCm`. The bounds survey **excludes primitives whose bounds reach world scale** — sky atmosphere, height fog, unbound post-process volumes and infinite-extent lights all report the full representable world — and says how many it dropped in `camera.sceneBounds.unboundedPrimitivesExcluded`. Without that filter the measured welcome-map scene put the camera at 4.4e12 cm, because 4 of its 346 visible primitives claim the whole world; with it, the same survey returns the real z span of [-1035, 1194] and the camera lands at 11194. That last number is the one to read: it is how much surveyed scene the near-plane-0 clip removed from every tile, and anything above 0 gets a warning. With no visible primitive to measure, an omitted `cameraDepthCm` is `SCENE_BOUNDS_NOT_MEASURED` — the verb refuses to guess a camera plane, because a guessed one silently slices the map.

**The burst is bounded, and the bound is on pixels as well as tiles.** Ceilings are **64 tiles** and **268,435,456 pixels** (256 MP). Cost tracks *pixels*, not tiles: 91–96% of per-tile time is PNG encode plus readback and the renderer barely moves it. Two runs of this verb on the same extent, measured 2026-08-18, make the point — 16 tiles at 1024 px and 4 tiles at 2048 px are both 16.8 MP, and took **0.59 s** and **0.48 s**; encode plus readback was 83% and 88% of a steady-state tile. A tile-count-only ceiling would therefore have let 64 tiles at 4096 px through as a two-minute burst. The pixel budget is sized off the slowest figure on record (~0.112 s/MP, from 64 tiles at 4096 px taking ~2 min), which predicts ~29 s for 256 MP — about a quarter of the transport's 120 s response timeout, and roughly 3.5× more pessimistic than the runs above. Past either ceiling the call is `TILE_BUDGET_EXCEEDED` **before anything is rendered**, with the predicted seconds in the payload. The verb is synchronous and there is no honest way to make it otherwise — `StartJob` invokes its bind delegate on the caller's own stack, and `CaptureScene()` plus `FlushRenderingCommands` cannot yield mid-tile — so the thing being refused is not a long call, it is a call that outlives its own caller with nobody able to observe or cancel it. Split the extent across several calls instead.

**Reading the response.** `renderer` (`"sceneCapture2D"`) and `projectionMode` (`"orthographic"`) appear on both the response and the manifest — the same two keys every `render.capture_*` and `camera.*` verb publishes, so an ortho burst pairs with any other capture without a spelling translation. (They are not universal across every PNG-producing verb: `editor.screenshot` and `ui.screenshot` publish neither, and `widget.screenshot_designer` publishes `renderer` only, since a UMG designer shot has no projection.) `georeference` and `grid` are the same objects `image.tile` emits and parses. `tiles[]` (inline up to 64, always in the manifest) carries per tile: `col`/`row`, `file`/`path`, `pixelOriginX`/`pixelOriginY`, `worldMin`/`worldMax`/`worldCentre`, `orthoWidthCm`, `blank`, the four luminance figures below, per-tile `renderMs`/`readbackMs`/`encodeMs`/`totalMs`, and a standalone `georeference` — hand that one to `image.annotate` and it marks world coordinates on that tile alone. `timing` aggregates the burst; `showFlags` is read off the component. Tiles left over from an earlier run at a different subdivision are reported as `staleFiles` and never deleted.

Three fields are measurements taken *after* the fact rather than echoes of the request, and they are the ones worth reading:

- **`tiles[].cameraLocation`** is the pose the pixels were drawn from, read back off the capture component; `cameraLocationPlanned` is what was asked for. The renderer takes the camera from the component transform, so a move that did not land would render every tile of the burst from one place while every tile still reported its own world extent — indistinguishable downstream from a real mosaic. A mismatch is `CAPTURE_CAMERA_NOT_APPLIED` and no file is written.
- **`exposure.pinned`** is the conjunction of "a pin was asked for", "the component's physical-camera settings re-evaluate to that EV100 through the engine's own formula", and "this frame is one the renderer applies exposure to at all". `exposure.ev100Applied` is that recomputed value, not the requested one.
- **`imageStats`** and the per-tile `meanLuminance` / `maxLuminance` / `minLuminance` / `luminanceVariance` say what the pixels actually contain. They exist because `blank` is not enough: `blank` is an all-zero test, and a frame the tonemapper filled with dither over nothing is not all-zero. During this verb's own bring-up a 16-tile burst of visually black tiles reported `blank: false` for every one of them. The verb states the numbers and warns when the brightest pixel in the whole burst is below 0.02; it does not decide whether your picture is usable.

Seams follow `image.tile`'s rule exactly, because both call the same code: a pixel exactly on a seam belongs to the tile on its **right / below** at pixel-in-tile 0. Filenames match too (`<prefix>_r<row>c<col>.png`), so a capture set and a reference set pair file-for-file.

Gotchas:

- **A pinned scene capture at EV100 *N* is not as bright as a pinned viewport capture at EV100 *N*.** The renderer's fixed-exposure branch multiplies by middle grey (0.18) and the manual-metering branch this path uses multiplies by 1.0 — ~2.47 stops apart. It does not matter as long as you never compare across renderers, which you must not do anyway.
- **Blank tiles are written, counted and flagged.** A tile that reads back all-zero in every channel was never drawn (headless with no RHI surface, a camera plane that clips the scene, an empty extent). If *every* tile is blank the call returns `BLANK_CAPTURE` with the files still on disk and the whole measured response attached; `allowBlank: true` accepts it.
- **`overwrite` is checked before rendering,** so a refused call has cost nothing and left nothing half-written.

### render.nanite_rebuild_mesh

Starts a ticketed async job that enables Nanite on one `UStaticMesh` and calls `Build`. The job body is parked at a retained dispatcher safe point, quiesces live StaticMesh and Niagara render consumers, and keeps that guard active across `Build`, a compile wait bounded by `timeoutSeconds` (default and maximum 120 seconds) and driven by the shared asset-compilation pump, and the optional forced save/probe. `assetPath` is required; `save` is optional and defaults to `true`. The default force-saves the rebuilt asset and verifies that its `.uasset` exists before reporting `saved:true`.

Cooperative cancellation before the safe-point body starts does not build or save. A timeout requests compiler-manager cancellation and fails the ticket without saving; if the engine cannot cancel immediately, a passive core-ticker watcher retains the mesh and render guard until the engine's normal compilation tick reports terminal state. That watcher does not pump compilation outside the retained request scope.

The kickoff response contains the job ticket, not persistence evidence. Poll `system.job_status` and read the terminal result: it carries `rebuilt`, `naniteEnabled`, the resolved package name in `package`, `sizeBytes` (and `sizeBytesIsStale` when applicable), `saveRequested`, `saved`, `saveState`, `saveDetail`, and `pendingFlush` only when a requested save is not durable. With `save:false`, no disk write is attempted and the package remains dirty; an existing `.uasset` is probed, but its size is the prior stale revision (`sizeBytesIsStale:true`), not evidence of a write. If no prior file exists, `sizeBytes` is `0` and the stale marker is absent. `saveState` is `notRequested`, and `pendingFlush` is absent.

### render.create_render_target

Creates a `UTextureRenderTarget2D` asset. Both path arguments are optional — `name` defaults to `NewRenderTarget`, `packagePath` to `/Game/RenderTargets` — and the asset is left dirty in memory rather than saved, so a later save-all is what puts it on disk.

**`name` is a BARE asset name, never a path.** It is validated against `FName::IsValidXName` / `INVALID_OBJECTNAME_CHARACTERS` and the composed `<packagePath>/<name>` against `FPackageName::IsValidLongPackageName`, so a `/`, `\`, `.`, `..`, a leading or trailing slash, a space or an unmounted root is refused `INVALID_ARGUMENT` with the engine's own reason quoted, before anything is created. `FString::operator/` was not a defence here: it only avoids doubling a slash it would add itself, so a `name` of `a//b` reached `CreatePackage`'s **Fatal** — which is not compiled out in any configuration and ends the editor process rather than failing the call. A trailing slash on `packagePath` is still accepted and trimmed.
