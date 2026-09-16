---
type: guide
summary: "Spatial authoring for level prototyping and mesh building: the closed capture -> raycast/place -> measure/verify -> adjust loop and the procedural-mesh authoring path, plus the design rationale for choosing stated relations + deterministic measurement over guessed coordinates."
date: 2026-07-04
tags: [spatial, level-prototyping, mesh-authoring, capture, placement, measurement]
---

# Spatial Authoring

This page describes how an agent builds and arranges scene geometry with PinWright without ever guessing a world coordinate. It ties together four namespaces — `spatial` (perception + placement), `camera` and `render` (multi-angle capture), `actor` (spawn/nudge), and `geometry` (procedural mesh authoring) — into two coherent workflows: **level prototyping** (place and arrange existing meshes) and **mesh authoring** (build a new mesh and confirm it is sound).

## Design intent: relations + measurement beat guessed coordinates

A vision model reading a screenshot is reliable at proposing *relationships* — "the crate belongs on the shelf, pushed flush to the back wall" — and unreliable at proposing *coordinates*, because a pixel does not carry a metric world position. The spatial verbs split those two jobs cleanly:

- **State the relationship.** `spatial.place_relative`, `spatial.place_on_surface`, and `spatial.verify_placement`'s `expect` block take an intent ("on top of", "resting on this surface", "grounded within 2 cm, not overlapping the walls") rather than a hand-computed transform.
- **Let deterministic math decide.** Each solver computes the transform from world-space AABBs and traces, and every mutating verb returns an **inline verification** (resting gap, edge gap, overlap) so the result is self-checking. The measurement verbs (`measure_distance`, `measure_overlap`, `verify_placement`) read the same `GetActorBounds()` AABB that `actor.get_bounding_box` reports, so their numbers agree across the toolset.

The research basis is straightforward: set-of-mark / grounded-vision techniques let a model *point* accurately (which pixel), but converting a point to a metric placement, and then confirming it, is a geometry problem, not a perception one. Vision proposes; measurement disposes. In practice this means: prefer a stated relation over a computed coordinate, and trust the returned verification over a follow-up screenshot.

### Coordinate conventions

Everything in the `spatial` namespace is centimetres, left-handed, Z-up: `+X` forward, `+Y` right, `+Z` up; rotation is an `FRotator` in degrees (`pitch` about Y, `yaw` about Z, `roll` about X). Crucially, an actor's **pivot** (`GetActorLocation()`) is not its **bounds centre** — the placement solvers pivot-correct so an off-centre pivot still rests flush, and `measure_distance` returns both `centerDistance` and `pivotDistance` because they differ.

## The level-prototyping loop

The loop is look → anchor → measure → adjust, and it closes on itself:

1. **Seed placeholders.** `actor.spawn_shape` drops a single BasicShapes primitive; `actor.spawn_batch` drops many from one source (shape / mesh / class / duplicated actor) and can file them all into a World Outliner folder via `folder` or `actor.set_folder`. Existing meshes are placed the same way.
2. **Capture from a useful angle.** `render.capture_open_level` gives a raw camera-pose PNG; `camera.frame_actor` and `camera.orbit_shots` auto-frame one actor (orbit's default is a 3/4 perspective plus front/side/top orthographic); `render.capture_annotated` paints world axes, a Z=0 grid, and per-actor AABB size labels directly onto the frame so the agent can read dimensions off the image.
3. **Anchor to geometry.** `spatial.raycast_screen` inverts a capture — hand it the exact pose + dimensions the capture returned plus a pixel, and it returns the world ray + hit ("put it where I can see it, THERE"). `spatial.place_on_surface` rests an existing actor on whatever a screen pixel, a world point, or a straight-down drop hits, pivot-corrected so it never sinks. `spatial.raycast` is the raw world-space trace beneath both.
4. **Measure / verify.** `spatial.measure_distance` (center / pivot / edge-gap), `spatial.measure_overlap` (intersection + penetration), and `spatial.verify_placement` (grounded / on / noOverlapWith / within) confirm the arrangement with box math.
5. **Adjust and re-check.** `spatial.place_relative` moves actor B by a relation to anchor A (`on_top_of`, `left_of`, `against_wall`, …); `actor.nudge` applies a relative world- or camera-space delta ("push it away from me"). Then re-measure.

Two editor-world caveats gate the loop. First, **spawn and trace on separate calls**: in a non-PIE editor world a body spawned in the same call is not yet in the physics scene-query structure until the world ticks, so a raycast/placement issued in that same call can miss its own new target. Second, the **pixel-producing and deproject verbs need a live GPU-backed Level Editor viewport** (`raycast_screen`, `place_on_surface`'s `screen` mode, and all `camera.*` / `render.capture_*` captures); a headless run returns a typed `NO_ACTIVE_LEVEL_VIEWPORT` rather than a wrong answer. The pure box-math verbs (`measure_*`, `verify_placement`, `place_relative`, `actor.nudge` with `deltaWorld`) do not need a viewport.

## The mesh-authoring path

For building a new mesh rather than arranging existing ones, the loop runs through the `geometry` namespace on a `DynamicMeshActor`:

1. **Create the mesh.** `geometry.create_procedural_mesh` seeds a `DynamicMeshActor`; the `create_*` primitives (box, sphere, cylinder, arch, …) build named starting shapes.
2. **Add geometry.** `geometry.append_buffers` uploads whole vertex + indexed-triangle buffers (optionally with per-vertex normals/UVs/colors) in one call instead of per-element round-trips; the `create_*` primitives, `array_linear`/`array_radial`, and the boolean ops (`boolean_union` / `boolean_subtract` / `boolean_intersection`) compose more geometry onto the same actor.
3. **Measure / check health.** `geometry.measure` reports the bounding box, dimensions (full extents), volume, area, and vertex/triangle/edge counts (local or world space). `geometry.check_health` reports boundary/open edges, degenerate triangles, non-manifold (bowtie) vertices, a connected-component count, and a single `healthy` verdict. Watch for `boundaryEdges > 0` on a mesh that should be solid — that is unwelded connectivity, not necessarily a hole.
4. **Confirm visually from multiple angles.** `camera.orbit_shots` around the authored actor gives a multi-angle confidence check that a single screenshot cannot — the canonical 4-shot set catches a defect that hides at one azimuth.
5. **Bake.** `geometry.convert_to_static_mesh` turns the confirmed `DynamicMeshActor` into a Static Mesh asset once it is right.

### OBJ / STL round-trip

`geometry.export_obj` / `export_stl` write the mesh out as LLM-editable text (OBJ carries `v`/`vt`/`vn`/`f`; STL is ASCII by default or binary); `geometry.import_obj` / `import_stl` parse text or a file back into a **new** `DynamicMeshActor`. This lets an agent read a mesh out, edit the text, and re-import it. Two facts to keep in mind: OBJ import fan-triangulates polygons and applies positions + topology only (`vt`/`vn` are parsed but not applied), and STL import does **no vertex welding** (each triangle emits 3 vertices, so `vertexCount == 3 x triangleCount` and `check_health` reports the mesh as open until it is welded).

## See also

- [`spatial`](wiki-src/spatial.md) — raycast / measure / verify / place verbs, with the coordinate conventions and editor-world caveats on the namespace prelude (`call("spatial")`).
- [`camera`](wiki-src/camera.md) — `frame_actor` / `orbit_shots` multi-angle capture (`call("camera")`).
- [`render`](wiki-src/render.md) — viewport captures including `render.capture_annotated` (`call("render")`).
- [`actor`](wiki-src/actor.md) — `spawn_shape` / `spawn_batch` / `set_folder` / `nudge` (`call("actor")`).
- [`geometry`](wiki-src/geometry.md) — procedural mesh authoring, `append_buffers`, OBJ/STL round-trip, `measure` / `check_health` (`call("geometry")`).
- [visual review](wiki-src/visual-review.md) — choosing the right capture surface for visual proof.
