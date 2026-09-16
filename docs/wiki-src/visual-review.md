# visual-review

Workflow for proving visual state with the right capture surface. Pick the surface that matches the
review; do not spawn temporary actors or write runtime visibility when a dedicated handler covers it.

## Choose The Capture Surface

| Review target | Use | Notes |
|---|---|---|
| Widget Blueprint rendered preview | `widget.screenshot_designer` with `target: "preview"` | `FWidgetRenderer` capture; supports `showOnly`, `hide`, and `visibilityOverrides` without dirtying the asset. |
| Widget Blueprint editor window | `widget.screenshot_designer` with `target: "window"` | Captures Designer chrome and panels. |
| One compiled Static Mesh, judged alone | `render.capture_asset_preview` | **Default for single-asset review.** Uses the real Static Mesh preview with no actor, level dirtiness, or competing lights. Pass `location`/`rotation`: the default camera is on the key light's **dark** side and makes a closed mesh a black silhouette. See [`visual-review.model-rig`](visual-review.model-rig.md). |
| One material or texture, judged alone | `asset.generate_thumbnail` | The only verb that renders either. `render.capture_asset_preview` refuses both — neither is a capture subject kind. Renders offscreen to `outputPath` with no asset editor, no level and no viewport; a material is drawn onto a chosen `primitive` under caller-set `azimuth`/`elevation`/`zoom`. |
| Open Level Editor viewport | `render.capture_open_level` | Caller controls camera/projection/size; viewport state is restored. |
| Two or more subjects compared | Spawn them into one level, then `render.capture_open_level` | Per-asset preview scenes are not comparable. See [Comparing Two Or More Subjects](#comparing-two-or-more-subjects). |
| One asset under a **different** material | Spawn one actor, set `materialPaths` / `OverrideMaterials` | Asset preview uses committed bindings and no override; a level actor swaps material without changing the asset. |
| One placed actor, one angle | `camera.frame_actor` | Fits bounds from chosen `azimuth`/`elevation`; one PNG. |
| One placed actor, several angles | `camera.orbit_shots` | N evenly spaced shots or explicit poses in one call. See [Multi-Angle Actor Review](#multi-angle-actor-review). |
| PIE/game viewport | `editor.screenshot` | Async screenshot under `Saved/Screenshots/`; poll `system.job_status`. Omit `width`/`height` for the live size, or provide both for an exact-size scene + UMG frame; read `captureMode` and `viewportRestored` on the game/PIE path. |
| Widget Blueprint audit previews | `asset.dump` or `asset.dump_folder` with `includeWidgetScreenshot: true` | Writes `preview.png` beside supported dumps; leave off for text-only sweeps. |
| Runtime UI structure, not PNG | `widget.describe` or `widget.export_xml` with `capture_source: "live"` | Structured JSON/XML of the live PIE UMG tree, optionally with geometry. |

## Recipe

1. Read structure first with `call("asset.dump")`, `call("widget.export_xml")`, `call("widget.describe")`, or `call("property.list")`.
2. **If a material is involved, compile it before you capture anything.**
   `call("material.authoring.compile_material", {assetPath})` and assert `compileSucceeded`, or read
   `shaderCompile.status` off the write that authored it. It costs 3-10 seconds, no world, no lock
   and no PIE, and it strictly dominates a capture: a material whose shader map failed with compile errors renders the
   engine **Default Material** — flat grey with a world-space checker grid — and a capture now rejects
   that known substitution as `MATERIAL_FALLBACK` with `success: false` by default; pass
   `allowFallback: true` only when that fallback image is wanted. See [`material.compile-state`](material.compile-state.md).
3. Pick the matching surface above and set deterministic dimensions through its own `width`/`height`, `max_size`, or Widget Blueprint design size.
4. For async screenshots, wait with `call("system.job_status", {"ticket_id": "..."})`.
5. Verify response path, width/height, and file size before treating the image as evidence.
6. For before/after, keep camera/projection/size fixed **and pin exposure on the capture** with
   `exposure: {mode: "fixed", ev100: N}`. It is scoped to the call; `viewport.exposure.pinned` says
   whether it governed the pixels. See [`render.capture-exposure`](render.capture-exposure.md).
   Choose `N` from one `{mode: "auto"}` shot's `viewport.exposure.ev100Equivalent`, **not**
   `adapted` (a linear gain that underexposes by several stops when passed as `ev100`).

## Comparing Two Or More Subjects

**Put every subject of a comparison in one scene before capturing it.** `render.capture_asset_preview`
uses each asset editor's own environment and light rig, so two such frames are not comparable;
pinning `ev100` removes auto-exposure gain, not different lighting. Measured 2026-08-19: two meshes
at the same pinned `ev100` returned mean luminance **0.47** and **0.18**. That pair says nothing
about geometry.

**`previewScene` narrows that gap without closing it.** It pins key aim/intensity, sky, floor, and
backdrop; `viewport.previewScene.profileName` identifies each rig. It does not equalize other profile
differences. Use it to make a cross-editor comparison *checkable*, not to make scenes one. See
[`render.preview-scene-rig`](render.preview-scene-rig.md).

**A comparison is about its subjects only when they share a scene, light rig, and camera.** Build
that shared scene:

1. Spawn variants with [`actor.spawn`](actor.spawn.md), spaced on one axis so none occludes another and all share lighting.
2. Capture with [`render.capture_open_level`](render.capture_open_level.md), or use
   `camera.frame_actor` / `camera.orbit_shots` once per subject when judging them alone.
3. Fix pinned `exposure`, `width`/`height`, projection, and view mode; read
   `viewport.exposure.pinned` rather than assuming it governed pixels. See
   [`render.capture-exposure`](render.capture-exposure.md).
4. **Destroy spawned actors with [`actor.delete`](actor.delete.md) and leave the level unchanged.**

Judge with `image.compare`: it reports `meanAbsDifference` / `maxAbsDifference` and a best-fit gain,
not a verdict. A difference that vanishes after dividing out the gain was lighting, not content.

A before/after pair of *one* asset through its own editor is valid; A-versus-B through two editors is
not.

## Multi-Angle Actor Review

`camera.orbit_shots` takes the whole set around one placed actor in one call. Do not loop
`camera.frame_actor`: one orbit plans every pose, reuses viewport acquisition, and gives each shot a
distinct filename.

**Do not hand-compute the camera pose.** `camera.frame_actor` and `camera.orbit_shots` fit bounds and
report the used pose; hand-set location/rotation/FOV can clip the subject. Use an explicit pose only
for a deliberate multi-subject frame through [`render.capture_open_level`](render.capture_open_level.md)
or a computed frame following [`level-review.framing-math`](level-review.framing-math.md).

**N shots at a chosen elevation.** `count` shots are spread evenly around the actor (`360 / count` apart, starting at azimuth 0), all at `elevation` degrees above the horizon:

```js
call({
  method: "camera.orbit_shots",
  args: { actorName: "Statue_1", count: 4, elevation: 30, width: 640, height: 640 }
})
```

That answers "give me 4 side shots at a given angle". `count: 6, elevation: 0` is an eye-level
walk-around; `count: 8, elevation: 15` is a fuller turntable. `elevation` defaults to `30` and may
be negative to look up from below.

**Explicit poses.** `angles` overrides `count`; each entry may carry its own `elevation`, otherwise
the top-level value applies:

```js
call({
  method: "camera.orbit_shots",
  args: {
    actorName: "Statue_1",
    angles: [ {azimuth: 0, elevation: 0}, {azimuth: 90, elevation: 0}, {azimuth: 45, elevation: 60} ],
    width: 640, height: 640
  }
})
```

**Six orthographic sides.** `views: "sides"` captures front / back / left / right / top / bottom in
one call for proportion and silhouette checks without perspective foreshortening:

```js
call({ method: "camera.orbit_shots", args: { actorName: "SM_Statue", views: "sides" } })
```

It cannot combine with `count` or `angles` and defaults to **640** on the long edge because it
always produces six images.

**Canonical set.** Omit `views`, `count`, and `angles` for 4 shots: 3/4 perspective plus front /
side / top orthographic—the fastest "what is this thing" call.

**Orbit a place instead of an actor.** Pass `point: {x, y, z}` with mandatory `radius` (a point has
no bounds to fit) to ring the camera around a location.

Parameters worth setting:

- `count` — number of evenly-spaced shots. `angles` wins when both are given.
- `elevation` — degrees above the horizon for `count` / `angles` shots (default `30`).
- `radius` — camera distance. Defaults to a bounds-fit distance for an actor; required for a `point`.
- `fov` (default `50`), `width` / `height` (default `1024` for existing `count` / `angles` /
  canonical shapes; `640` for unsized `views`).
- `inline` — set `true` only when you need the bytes on the wire; take the returned `path` otherwise.

Resolution: unsized `views:"sides"` is **640** on the long edge; unsized `count`, `angles`, and
canonical orbit shapes remain **1024**. Other ordinary capture paths use **768 x 768**. Change size
only when the review needs it.

**Keep one size for the whole session.** On 2026-08-13, changing size in
`render.capture_open_level` left a stale hit-proxy buffer; a cursor query hit
`Assertion failed: ProxyMap.Num() == TestSizeX * TestSizeY` in `FViewport::GetHitProxy()` and cost 66 actors and 125 emitters of unsaved level state. Saved assets survived; level state did not.

Resizing itself is fine: 48 back-to-back cycles at constant 640 produced zero asserts, and
`camera.animation_shots` holds one size for its burst. If a capture is too small, re-shoot the set at
the new size rather than changing it mid-sequence. `editor.screenshot` is scoped differently: omit
`width`/`height` for native resolution, or pass both to render a temporary exact-size scene + UMG
composite; the game/PIE path restores its original viewport size before the job completes.

Gotchas: more than 24 shots is rejected (`TOO_MANY_SHOTS`); `camera.orbit_shots` fails whole on its
first capture failure, unlike `camera.animation_shots`, which returns `partial: true` with captured
shots. Each shot re-poses the real camera and restores it afterwards. `viewMode` applies to the set
and restores the prior mode; unknown values error rather than echo.

## Focused Widget Captures

For one subtree in a multi-mode Widget Blueprint, prefer `widget.screenshot_designer` overrides:

- `showOnly` keeps named widgets and ancestors visible for that capture.
- `hide` hides named widgets for that capture.
- `visibilityOverrides` temporarily changes runtime `Visibility` on the preview instance only.

Overrides revert even on capture failure and do not dirty the asset. Use
`widget.get_designer_visibility` / `widget.set_designer_visibility` only when eye state must persist
across operations.

## Capturing A Short-Lived In-Game HUD Animation

A hit marker, a damage flash, a pickup pop — anything whose whole life is a few hundred
milliseconds — cannot be caught by polling `editor.screenshot`: the RPC round-trip is longer than
the animation. Stop time instead:

1. `editor.play`, then `editor.pause` — this freezes the world **and** the session's UI (`uiFrozen`
   on `editor.status` confirms it; the frozen frame still repaints, so captures work normally).
2. Trigger the animation (`object.call_function` on the pawn, `editor.simulate_input`, …). Nothing
   decays while you do it.
3. `editor.step_frame` — one frame of `deltaSeconds` for the world, the FX and the UMG animations,
   then frozen again. Repeat to walk through the animation. Check `deltaHonoured` on each
   response: `false` means the level's own `AWorldSettings` floor or ceiling resized the tick,
   and `worldDeltaClamp` names which.
4. `editor.screenshot_window` or `editor.screenshot` between steps.

To place a captured frame in time, sum `worldSecondsAdvanced` — never the requested
`deltaSeconds` — or, better, read the state you intend to claim (a montage flag, an alpha) in the
same frozen instant as the capture and quote that instead of a step time.

Read `editor.step_frame` on the [`editor`](editor.md) page for what advances by exactly the step
and what does not (a widget that integrates its own alpha in `Tick` advances by the real frame).
Do **not** capture from a session paused with `freezeUi: false`, or paused from the editor toolbar:
the widget `Tick` runs on against a frozen world, so the frame can show a HUD state the running
game never renders.

## Capture Output Paths

- `widget.screenshot_designer`: `Saved/Screenshots/WidgetDesigner/` by default.
- `render.capture_asset_preview`: `Saved/Screenshots/AssetPreview/`.
- `render.capture_open_level`: `Saved/Screenshots/OpenLevel/`.
- `camera.frame_actor`: `Saved/Screenshots/CameraFrame/`.
- `camera.orbit_shots`: `Saved/Screenshots/CameraOrbit/`, one file per shot, named with shot index and azimuth/elevation.
- `editor.screenshot`: `Saved/Screenshots/`.
- `asset.dump` / `asset.dump_folder`: `<ProjectSavedDir>/PinWright/asset-dumps/.../preview.png` when `includeWidgetScreenshot` is enabled and capture succeeds.

## Keeping Editor Decoration Out Of An Acceptance Shot

Call `editor.set_game_view {enabled: true}` before a `render.capture_open_level` burst. It clears
the axis gizmo, grid, selection outlines, spline handles, and component visualizers; the response
reports the **measured** show-flag state.

It is **per-viewport state the verb does not restore**; use returned `previous.gameViewEnabled` to
put it back. In the measured build it did **not** clear `overlayShowFlags.billboardSprites`, so
light-bulb and arrow icons remained after `gameViewEnabled: true`. Read that field, not the toggle.

A per-capture `hideEditorSprites` boolean is the scoped alternative: it clears
`EngineShowFlags.BillboardSprites` and restores it. Level-viewport verbs accepting it are
`render.capture_open_level`, `render.capture_annotated`, `camera.frame_actor`, `camera.orbit_shots`,
and `camera.animation_shots`. Preview verbs (`render.capture_asset_preview`,
`render.capture_animation_preview`) answer `UNKNOWN_PARAMS`; preview scenes have no icon sprites.

`render.capture_asset_preview` is **not** decoration-free: it draws an axis gizmo, grid floor with
coloured axis lines, and a camera-moving photographic backdrop.

## Diagnosing A Black Or Unlit Surface

A black panel is reported as "inverted normals" more often than it is. There are four causes and
only one is winding. Establish the cause first: `flip_normals` and `recalculate_normals` do nothing
to three causes and can merely move the black side on single-sided geometry.

The full cheapest-first ladder, materials, dynamic-GI checks, and light rig are on
[`visual-review.model-rig`](visual-review.model-rig.md). The short form below covers black surfaces.

Work in order; each step rules out one cause and costs one call.

0. **Does the material compile?** Before any of the geometry ladder below, run
   `call("material.authoring.compile_material")` on the surface's material (or read `shaderCompile`
   off whatever authored it). A failed shader map is drawn as the Default Material, and on a dark
   rig that reads as a near-black frame whose `rangeWarning` blames exposure — pointing at the wrong
   knob entirely. It is the cheapest step here and the only one that is not about geometry.
1. **Is the mesh closed?** `model.validate` and `model.compile` return `health.isClosed`,
   `boundaryEdges`, and `nonManifoldVertices`. A bare `plane`, `ring`/`disc` without `shell`, or
   `append_buffers` panel without `extrude` is single-sided and black from behind;
   `boundaryEdges > 0` is the tell. Add thickness, not a flip.
2. **Re-shoot through `render.capture_asset_preview` from the opposite side.** This removes level
   lighting and the preview's dark side. Measured 2026-08-20 at pinned `ev100: -1`:
   `SM_ChessRook` returned `meanLuminance` 0.143 as a default-camera black silhouette and 0.285
   fully readable from the opposed camera—the same asset, exposure, and closed mesh.
3. **Does the black MOVE?** Render from two opposed cameras. Single-sided geometry moves the black
   with the camera; a closed solid keeps its silhouette. This is the fastest discriminator without
   `health` and works on someone else's mesh. **It separates open from closed and nothing else.**
4. **Is it a cast shadow?** Set `CastShadows: false` on the key light and re-shoot. A lit-back-up
   area was never a facing question; an overhanging limb can account for most dark area.
5. **Is it facing away from every light?** What survives step 4 on a closed, correctly wound mesh
   is a real surface facing away from all lights. With no usable ambient it can be near-black. Add a
   light on that side, move the camera, or change a flat underside's geometry angle.

Only after 1-4 is winding a candidate, usually a *subset* of faces: `mirror` negates an axis without
reversing triangle order, or a sibling op was appended instead of unioned.

**The winding fault the ladder cannot see is a whole closed part inverted.** A closed shell renders
**identically** inside-out: backface culling shows whichever wall faces you, opposite normals sit
one thickness apart, and the silhouette, shading, and `health` (`closed`, manifold, 0 boundary
edges) all pass. Every ladder instrument is blind to it; a recorded `sheet` part therefore survived
two "not a normals bug" investigations. Two checks do see it:

- **Delete the thickening op and render the bare surface from both sides.** A one-sided surface is
  invisible from behind, so its visible side is the facing normal—measured, not derived.
- **Put the camera inside.** Rebuild thick enough to hold a camera and shoot within: outward-facing
  geometry vanishes (every wall back-facing), while an inverted shell renders a lit interior.

It matters even though the fault is invisible in a normal render: offline consumers read winding, not
shading. The mesh distance field counts backface hits (`MeshDistanceFieldUtilities.cpp:261-281`),
so an inverted part carries an inverted field and Lumen/DFAO light it as if the camera were inside.
That is visible in a lit level; "it looks fine in the asset preview" does not close the report.

**None of this shows up in the numbers.** Triangle counts, `health` beyond `boundaryEdges`, and
orthographic silhouettes are identical for causes 3, 4, and 5. A shaded perspective render is the
only instrument that sees them; a silhouette or triangle count is not verification.

**A stable artefact is not evidence of geometry.** Nudging the camera separates screen-space from
world-space effects and nothing else. Shadow acne, shadow-map staircase, grid-material patterns, UV
moiré, and baked vertex colour are camera-stable; only TAA/SSR/SSAO ghosting was ruled out by that
control. The model-rig page covers two further causes: a default material's world-space grid and
Lumen final-gather speckle, a moving grain indistinguishable from acne in a still.

## Related Pages

- [`visual-review.model-rig`](visual-review.model-rig.md) for judging a compiled `.pwmodel`: the full diagnosis order, the neutral and UV-checker materials, the light rig, and what the asset preview's own lighting does to each artefact class.
- [`widget`](widget.md) for Designer screenshot parameters and focused-capture overrides.
- [`camera`](camera.md) for the full `camera.frame_actor` / `camera.orbit_shots` argument reference and the orthographic-capture rules.
- [`render`](render.md) for asset-preview and open-level viewport captures.
- [`asset.generate_thumbnail`](asset.generate_thumbnail.md) for the one asset class no capture verb serves: a material or a texture, rendered offscreen with no asset editor.
- [`material.compile-state`](material.compile-state.md) for the cheap check that dominates a capture: whether the subject's material compiles at all, and the Default-Material frame it renders when it does not.
- [`render.preview-scene-rig`](render.preview-scene-rig.md) for the `previewScene` parameter: lighting an asset preview for one capture, and reading back which rig a frame was drawn under.
- [`editor`](editor.md) for game-viewport screenshots.
- [`image`](image.md) for measuring one capture against another: side-by-side, difference composite, best-fit gain.
- [`asset-audit`](asset-audit.md) for dump-driven visual audit setup.
- [`level-review`](level-review.md) for comparable repeat captures, top-down framing math, and multi-range proportion review of a level.
- [`level-building`](level-building.md) for building the level those captures review.
- [`workflows`](workflows.md) for the index of task-oriented guides this one belongs to.
