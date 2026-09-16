# level-review.framing-math

The camera mechanics behind a trustworthy capture: what orthographic projection can and cannot do, the two field-of-view formulas that answer different questions, and how to establish which screen axis is which before measuring a single pixel. Reach for this page whenever you are computing a pose rather than judging one.

## Orthographic Capture Is Axis-Aligned Only

**Orthographic** capture renders, but only from the six axis-aligned views the editor
supports. `FEditorViewportClient::CalcSceneView` does not read camera rotation for an
orthographic view — the view matrix is fixed per viewport type — so an arbitrary tilted
ortho pose cannot be honoured. The surfaces handle an off-axis request differently:

- Raw `render.capture_open_level` rejects a pose more than 1 degree off every world axis
  with `UNSUPPORTED_ORTHOGRAPHIC_ROTATION`; it never silently captures a different view.
- `camera.frame_actor` and `camera.orbit_shots` are higher-level framing helpers. They snap
  a requested orthographic angle to the nearest cardinal world axis and report both the
  requested and applied poses, including `orthoAxisSnapped`.

See [`camera`](camera.md) and [`render`](render.md) for the per-verb fields.

Practical consequence: a **top-down** ortho shot is exact and repeatable, which makes it
good for measuring. A *tilted* ortho is unavailable — raw capture rejects it, while camera
helpers select the nearest axis view. Use perspective for a three-quarter view.

The **horizontal** ortho views (`elevation` 0 — `front`, `left`, `back`, `right`) render
scene geometry the same way top-down does; verified 2026-08-13 on UE 5.8. Read them as
elevations: a flat level seen edge-on fills a thin band in the middle of the frame, and the
background is the editor's dark ortho grid rather than sky (the engine drops fog for
orthographic lit views).

Use perspective for a natural-looking layout read; use orthographic for a distortion-free
plan view whose pixel-to-world mapping is constant across the frame.

**An orthographic top-down may cull distant foliage.** Instanced vegetation is subject to cull
distance, and an ortho camera placed far above the level can drop it from the frame, so the very
shot you set up to *measure* vegetation coverage can under-report it. Cross-check against a
perspective capture of the same framing before concluding that a scatter pass came out thin.

**Check alpha before calling a frame blank.** A capture that reads as blank or near-white while
still showing the gizmo and scale bar is most likely fully transparent with the scene intact in
the RGB channels — one viewer composites the alpha and shows nothing, another re-encodes without
it and shows the same file correctly. Run an alpha histogram rather than judging by eye; 100%
alpha-0 alongside many distinct RGB values means the image is fine. See [`camera`](camera.md)
for the full signature.

## Framing Math — Two Formulas, Do Not Confuse Them

Both use the half-angle of the field of view. They answer different questions and swapping them puts the camera badly wrong.

**A. Where to put the camera for a new shot.** Given the half-extent you want to see on the ground plane:

```
z_cam = desired_half_extent / tan(fov / 2)
```

Read the half-extent you want off `level.get_bounds {}` rather than guessing it. Worked example: to see ±32000 uu of ground at `fov 30`, `z_cam = 32000 / tan(15°) = 119426`.

**B. Where a world point lands in pixels, for a shot whose camera height is already fixed.** The half-extent is a function of the **world Z of the plane you are measuring on**, with `z_cam` held at whatever that shot used:

```
H(z_world) = (z_cam - z_world) * tan(fov / 2)
px = (width  / 2 - 0.5) + world_x / H(z_world) * (width  / 2)
py = (height / 2 - 0.5) + world_y / H(z_world) * (height / 2)
```

**B is not a function of camera height.** Feeding a *desired camera height* into B instead of the world Z of the ground plane put one agent's close-up camera 4.3× too high. If you are choosing where to fly the camera, you want A.

## Verify Screen Orientation Empirically

Before trusting any pixel measured off a top-down frame, **spawn markers at known coordinates and read where they land.** The yaw that produces "+X right, +Y down" is not the obvious one: at `pitch: -90, yaw: 0` the mapping is +X **up** and +Y **right**; at `pitch: -90, yaw: -90` it is +X right and +Y down. Guessing wrong mirrors every correction you derive from the image afterwards, and a mirrored map looks plausible.

```js
call("render.capture_open_level", {
  filename: "overview.png", width: 1024, height: 1024,
  location: { x: 0, y: 0, z: 24000 },     // z_cam from formula A, for YOUR bounds
  rotation: { pitch: -90, yaw: -90, roll: 0 },
  projectionMode: "perspective", fov: 50
})
```

`spatial.raycast_screen` converts a pixel back to a world point, but only if you pass the exact pose and dimensions the capture used. `level.get_bounds {}` gives the world AABB to feed formula A each iteration.


## See also

- [`level-review`](level-review.md) for the review workflow these poses feed.
- [`camera`](camera.md) and [`render`](render.md) for the per-verb capture arguments.
- [`spatial`](spatial.md) for deprojection and the coordinate conventions.
