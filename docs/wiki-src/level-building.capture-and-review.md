# level-building.capture-and-review

Capture a level so passes are comparable and review sets cover the needed views. For the look-first rule, pose table, framing formulas, and motion sampling, see [`level-review`](level-review.md).

## See What You Built

Pin exposure before comparing passes: pass `exposure: {mode: "fixed", ev100: N}` on the capture verb (scoped to the call and restored afterwards); also enable `editor.set_game_view {enabled: true}` and set `editor.set_view_mode` for a lighting-independent read.

For a whole-level perspective top-down, derive camera height from the desired half-extent, read directly from `level.get_bounds {}`:

```
z_cam = desired_half_extent / tan(fov / 2)
```

```js
call("render.capture_open_level", {
  filename: "overview.png", width: 1024, height: 1024,
  location: { x: 0, y: 0, z: 24000 }, rotation: { pitch: -90, yaw: -90, roll: 0 },
  projectionMode: "perspective", fov: 50
})
```

**Verify the screen orientation empirically before trusting any pixel you measure.** At `pitch: -90, yaw: 0` the mapping is +X up / +Y right; at `pitch: -90, yaw: -90` it is +X right / +Y down. Spawn markers at known coordinates once and read where they land — guessing wrong mirrors every correction you derive afterwards, and a mirrored level looks perfectly plausible.

**Orthographic capture works** — top-down and the four horizontal elevations alike, verified on UE 5.8. Use it when you want a constant pixel-to-world scale across the frame, which is what makes a shot *measurable* rather than merely viewable:

```js
call("render.capture_open_level", {
  filename: "plan.png", width: 2048, height: 2048,
  location: { x: 0, y: 0, z: 20000 }, rotation: { pitch: -90, yaw: 0, roll: 0 },
  projectionMode: "orthographic", orthoWidth: 40000
})
```

`orthoWidth` is the world span in centimetres the image covers left to right, so you choose the scale directly — and `orthoWidth / width` is your units-per-pixel. Write that number down next to the image: it is what lets you measure a plan dimension off the frame instead of estimating it. The raw `render.capture_open_level` surface requires one of the six axis-aligned poses; a tilted rotation returns `UNSUPPORTED_ORTHOGRAPHIC_ROTATION` rather than silently snapping. `camera.frame_actor` and `camera.orbit_shots` do snap requested orthographic angles to the nearest cardinal axis and report the requested and applied poses. Expect a dark grid background rather than sky, and expect an edge-on elevation to put a thin band of geometry in a mostly empty frame.

**Judge a suspicious frame by its alpha channel, not by eye.** A blank or near-white capture that still shows the gizmo and scale bar is most likely *fully transparent with the scene intact in the RGB channels*: captures once encoded the back buffer's alpha, 0 over scene pixels and 255 only under editor-composited overlays. One viewer composites alpha and shows nothing; another re-encodes without it and renders the same file correctly, which masqueraded as a rendering bug for days. Every PNG-writing capture verb now stamps alpha opaque; the last gap closed on 2026-08-21 — `widget.screenshot_designer` with `target: "preview"` and `asset.dump`'s widget aspect shipped raw alpha until then, while this page and [`camera`](camera.md) already promised otherwise. Keep the habit: **run an alpha histogram before concluding a frame rendered nothing.** 100% alpha-0 alongside many distinct RGB values means the image is fine and the viewer is not.

## Capture Discipline For A Burst

A burst is one review set; each rule below exists because it has produced plausible evidence of the wrong thing.

- **Enable game view before every burst, and verify it took.** `editor.set_game_view {enabled: true}` is per-viewport state that other work resets. Read it back rather than assuming — editor billboards, light icons and volume wireframes are routinely mistaken for content in the level, and once one frame in a set is contaminated the whole comparison is.
- **For icons alone, prefer `hideEditorSprites: true` on the capture verb.** `render.capture_open_level`, `render.capture_annotated` and all three `camera.*` verbs take it — `camera.animation_shots` included, which used to be the one level-viewport verb without it. It clears one show flag for the duration of the shot and puts it back, so it removes light bulbs, audio icons and the directional-light arrow without leaving the user's viewport in a state a later burst inherits — which game view, being persistent per-viewport state, does. Use game view when the burst also needs the grid, selection outlines and volume wireframes gone; use both when it needs everything gone. Check `viewport.editorSprites.restored` in the response.
- **`gameViewEnabled: true` is not "the frame is clean".** Game view governs one set of show flags plus component visualizers; it does not govern active editor modes or debug-drawn geometry. Read `overlayShowFlags` in the same response — a still-set `splines` keeps drawing water-body and landscape spline lines, which has already been mistaken for mid-channel foam — and `notGovernedByGameView` for the rest. The verb raises `overlayWarning` when it measures that contradiction itself.
- **Report the view mode with every capture.** A wireframe or unlit frame is not blank, so a blankness or file-size check passes it happily while it shows none of the work under review. Name the mode next to the filename; if the mode is not recorded, the image is not evidence.
- **Hold one capture size for the whole burst.** Varying `width`/`height` between shots is what took an editor down on 2026-08-13 — `render.capture_open_level` resizes the live viewport and a later hit-proxy query asserts against the stale buffer. If a shot came out too small, re-shoot the *whole* set at the new size. See [`visual-review`](visual-review.md).
- **Save, then shoot, then hand over.** Capture after the level and its assets are persisted, so the frames describe a state that exists on disk. A burst taken before the save documents a state that may never have been written, and the reviewer has no way to tell.
- **Confirm no solved pose sits inside geometry.** A pose solver that filters foliage out of the sightline but not out of the occupancy test puts the camera inside a tree, and the frame is a plausible capture of bark. Probe each position for an overlap before shooting; re-solve after every terrain change, per [`level-review`](level-review.md).
- **Shoot from more than one height, including eye level and from below.** A top-down cannot show that an object is hovering, and nothing but a low or upward-looking view shows one that has sunk into the ground.

## Reviewing What You Built

No single camera position shows scale, proportion, silhouette and layout at once. Fix a pose set for **your** level, re-shoot it identically after every change, and compare passes. The required coverage is general even though the coordinates are yours:

- **At least one shot at the angle your game is actually played at.** This is the acceptance view — the only one that answers "does it read the way a player will see it". A level that looks right from everywhere except the play angle is wrong.
- **One high-altitude top-down for layout**, and no more weight than that. Overview shots answer "is anything missing, out of bounds, or badly distributed" and nothing else.
- **One deliberately high-resolution orthographic top-down whose purpose is measurement, not judgement.** State its units-per-pixel (`orthoWidth / width`) alongside it so plan distances can be read off the image rather than guessed.
- **Close range, separately.** Bevels, roof forms, stair treads, trim depth and material variation are physically unresolvable from altitude — at overview distance a building can be a couple of dozen pixels tall. Proportion defects survive indefinitely behind a top-down-only review, and they look fine right up until someone frames two neighbouring objects together.
- **Both sides of anything symmetric.** A defect present on only one side is invisible in every shot of the other, so capture the mirrored pose too and compare the pair.
- **In place, not on test ground.** An asset verified alone on a flat test row has not been verified — proportion is a relationship with its neighbours and does not exist in an isolated capture.
- **Poses anchored to sampled terrain height, not to an absolute Z.** An absolute camera height does not survive a re-sculpt: the same numbers end up underground or facing a cliff, and keep returning plausible captures of the wrong thing. Store each pose as *sampled ground + offset* and re-derive the set after every terrain change.
- **Several samples through any animation, not one still.** That a sequence *evaluates* is not that it *looks right*. Step `sequencer.set_playhead` through the loop and apply the placement and clearance checks at every sample — a floater sweep over actor transforms does not see animated actors at all.
- **Navigation, not only scripted captures.** Fly the level. Camera speed wrong for the world's scale, popping, and anything else that lives between frames is obvious in navigation and structurally invisible to a fixed pose set.

**Look first; measure only what looking cannot resolve.** Metrics are detectors, not acceptance gates: a number confirms an edit reached the screen, never that the result looks good. The failure shapes on both sides are catalogued on [`level-review`](level-review.md).

Screenshots carry no metric coordinate, so eyeballing produces confident wrong diagnoses; a plausible-looking wrong number is worse than no number. Assert placement with `spatial.verify_placement`, `spatial.measure_distance` and `spatial.measure_overlap`; for bulk symmetry or distribution checks, read transforms through `python.execute` and compare sets numerically.

Resolution guidance: the shared **768 x 768** default covers ordinary stills and multi-image captures. `camera.orbit_shots` preserves **1024** for existing `count` / `angles` / canonical shapes and **640** for `views:"sides"`; go higher only when you are genuinely measuring off the pixels. Larger frames cost more to produce and review without telling you more.

## See also

- [`level-review`](level-review.md) for the complete review workflow this page summarises.
- [`level-building.working-from-references`](level-building.working-from-references.md) for matching a capture to reference imagery and comparing the two without manufacturing differences.
- [`level-building`](level-building.md) for the rest of the build workflow.
- [`visual-review`](visual-review.md) for choosing a capture surface for a single visual state.
- [`render`](render.md) and [`camera`](camera.md) for the per-verb capture arguments.
