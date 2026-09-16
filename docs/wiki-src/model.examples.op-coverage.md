# model.examples.op-coverage

Which `.pwmodel` ops the thirteen worked examples exercise, omit, and exclude by measurement. See [`model.examples`](model.examples.md) for the examples.

## The count

**57 of the 75 entries in the parser's op table, 76%.** The split is **69 part-context ops**, of which the corpus exercises **52**, and **6 collision elements**, of which it exercises **5**. Denominators are from 2026-08-20 plus `harmonic_deform` (added 2026-08-22); the numerator is unchanged because that op has no corpus use. The count is measured against `model.describe_ops` on the running editor, which reads the static `BuildOpTable()` in `Source/PinWrightGeometry/Private/Model/PwModelParser.cpp`; re-derive it after table changes.

Collision elements used: `box`, `capsule`, `hull`, `convex`, `auto`. Only `sphere` is unused there — a sphere element is the degenerate capsule, and every rounded lump in the corpus was already covered by a `capsule` or a `hull`.

## Exercised, and where to read each one

An op reachable in more than three files is listed by count; the rest name their files, because for those the file *is* the documentation.

| Op | Where |
|---|---|
| `box` | 8 of 13 |
| `sphere` | 6 of 13 |
| `cylinder` | 8 of 13 |
| `cone` | `amphora`, `ships_wheel`, `watchtower` |
| `capsule` | `oil_lamp` |
| `torus` | `oil_lamp`, `ships_wheel` |
| `disc` | `ships_wheel` |
| `stairs` | `watchtower` |
| `spiral_stairs` | `spiral_stair` |
| `ring` | `ships_wheel` |
| `arch` | `gothic_window` |
| `pipe` | 4 of 13 |
| `revolve` | 5 of 13 |
| `procedural_mesh` | `mobius_band` |
| `recalculate_normals` | `amphora`, `chess_rook`, `oil_lamp` |
| `simplify_mesh` | `driftwood` |
| `subdivide` | `driftwood` |
| `extrude` | `origami_crane` |
| `bevel` | 7 of 13 |
| `shell` | `oil_lamp`, `ships_wheel` |
| `bend` | `driftwood` |
| `twist` | `driftwood` |
| `taper` | `driftwood` |
| `noise_deform` | `crystal_cluster`, `driftwood` |
| `smooth` | `driftwood` |
| `relax` | `driftwood` |
| `stretch` | `crystal_cluster` |
| `spherify` | `crystal_cluster` |
| `weld_vertices` | `mobius_band`, `spur_gear` |
| `remesh_uniform` | `crystal_cluster` |
| `recompute_tangents` | `crystal_cluster` |
| `split_normals` | 7 of 13 |
| `transform` | `spur_gear` (and `amphora`'s collision hulls) |
| `translate_mesh` | `crystal_cluster`, `origami_crane` |
| `uv` | 13 of 13 |
| `set_vertex_position` | `mobius_band` |
| `append_vertex` | `mobius_band`, inside a `hull { }` body only |
| `delete_vertex` | `mobius_band` |
| `append_triangle` | `mobius_band` |
| `delete_triangle` | `mobius_band` |
| `set_vertex_color` | `spiral_stair` |
| `union` | 6 of 13 |
| `subtract` | 10 of 13 |
| `intersection` | `gothic_window`, `pipe_junction`, `spur_gear` |
| `self_union` | `amphora`, `ships_wheel` |
| `mirror` | `amphora` |
| `array_linear` | `spiral_stair` |
| `array_radial` | 6 of 13 |
| `array_along_path` | `crystal_cluster`, `gothic_window` |
| `sweep` | `amphora` |
| `extrude_along_spline` | `spiral_stair` |
| `append_buffers` | `mobius_band`, `origami_crane` |

`append_vertex` is reached only from a collision `hull { }` body. That is intentional: a bare part vertex cannot be referenced because no `pwmodel 0` op builds triangles from ids, while a `hull` body containing only `append_vertex` is a legal point cloud. See [`model.explicit-geometry`](model.explicit-geometry.md).

## Deliberately unexercised, with the reason

Seventeen part-context ops have no corpus use; the reasons below are measured exclusions, not missing examples.

| Op | Why not |
|---|---|
| `bridge` | Needs two boundary loops, i.e. an open mesh. Every example is a closed solid by design: all thirteen compile `isClosed: true`, `boundaryEdges: 0`. |
| `edge_split` | Takes raw edge ids. Shipping an index-addressed op in a reference example would teach a pattern that breaks on any upstream edit — the same face-identity argument that keeps indices out of the rest of the format. |
| `trim` | Every cut in the corpus is a bounded closed-solid subtraction, which is exactly `subtract`. `trim`'s distinguishing power is cutting against an open or unbounded surface, and it cuts the whole accumulated mesh. |
| `outset` | No feature in any example grows a face in its own plane. |
| `offset_faces` | Measured against `shell` on `ships_wheel`'s trim plates: on `ring outer_radius=40 inner_radius=26 segments=48` at z=0, `shell thickness=4` spans z −4..0 and `offset_faces distance=4` spans z 0..4 — same 384 triangles, same 40 outer radius, differing only in which side of the authored plane the plate lands on. `offset_faces` splits 576 render vertices against `shell`'s 388 for that same mesh, and `shell` can serve a whole profile *set* in one op, which is what the `ring` + `disc` pair in that part needs. |
| `inset` | **Measured defect, not taste.** On `watchtower`'s parapet `inset` left 272 boundary edges on its own — with its `offset_faces` partner removed, so it is `inset` and not the pair — turning a closed solid into a shell by inserting the smaller face without stitching it to the border it was cut from. The compile reports success; only `geometry.check_health` sees the hole. Separately, the models that would want it are bodies of revolution, where a profile point does the same job better, and `inset` does nothing visible without a companion `extrude` / `offset_faces`. |
| `fill_holes` | There is no hole to fill: every example compiles `boundaryEdges: 0`. |
| `merge_vertices` | `mirror` welds its own seam plane and `revolve` closes its own loop, so it would be a measurable no-op. The corpus does use its sibling `weld_vertices`, twice, where duplicates genuinely exist: `mobius_band` stitches the 36 vertices twelve `append_triangle` ops duplicated onto the bore rims, and `spur_gear` welds boolean seams so `bevel` runs one continuous chamfer instead of 24 fragments. |
| `plane` | One triangle layer with one facing: invisible from behind, and black wherever a light lets you see its back. `gothic_window`'s glazing is a `box` 1.5 thick instead — 12 triangles, and never wrong. |
| `poke` | **Built and rejected on a render.** `poke offset=1.6 offset_type=face_normal area_mode=per_triangle` on `crystal_cluster`'s matrix produced a shell of separated triangular plates with black cracks between them — shattered rather than druzy — and took the part from 3,134 to 87,752 triangles, 28x, with the whole model at 88,436. |
| `remove_degenerates` | **Measured no-op where it looked right.** `watchtower` compiled with the op in front of `bevel`, behind it, and absent gave 4,312 triangles / 64 degenerates in all three. It does delete them on a lone `cone base_radius=148 top_radius=0`, 192 → 128, and that is the wrong fix: the 32 coincident apex vertices stop being shared and the part returns 64 boundary edges and `PWMODEL_MESH_NOT_CLOSED`. Fixed at source instead — `top_radius=4` and a finial — and `watchtower` now compiles at 0 degenerates. Its triangle count is not restated here: the one dated corpus reading is the `Tris` column of [`model.examples`](model.examples.md), and a copy in this row went stale the last time the example was reworked. |
| `transform_uvs` | **Written, then withdrawn as unverifiable.** `spiral_stair` wanted a 45-degree rotation to make chequer plate read as diamonds; it was written, compiled and rendered, and nothing changed. `M_Grid` and `WorldGridMaterial` — the two engine materials the corpus binds as placeholders — are world-space and never sample a UV channel, so a rotation and a scale change render identically. Measured with a three-panel probe: one plane at `uv mode=box scale=(0.02, 0.02)`, one at `(0.10, 0.02)`, one with `transform_uvs rotate=45 scale=(5, 5)` on top of the first, all three identical and their heavy grid lines falling by world position rather than by UV. |
| `set_uvs` | Same reason, same evidence. |
| `cylindrify` | No honest home: the cylindrical things in the corpus are already exact cylinders. |
| `harmonic_deform` | **Revisit candidate.** It displaces each vertex by azimuth, repeating exactly `order` times per revolution; useful shapes include scalloped rims, fluted columns, lobed vases and gear-like crowns. See [`model.authoring`](model.authoring.md#scalloped-and-lobed-shapes). None of these thirteen has an azimuthal period: the bodies of revolution are intentionally round, and a contrived lobe would demonstrate the parameter rather than the shape. Write it back when a corpus object genuinely wants one. |
| `flip_normals` | Every example is closed with correct winding. It stays a symptom rather than a feature: when `origami_crane`'s `sheet` part *was* inverted, the honest repair was the `extrude direction` sign at the op that got the facing wrong, not a flip bolted on afterwards. That file's header separates the three things reported as its "inverted normals" and says why `flip_normals` and `recalculate_normals` were the wrong tool for all three. |
| `ramp` | Removed from `watchtower`. It permutes its `size` axes and is corner-placed — `size=(90, 240, 120)` spans x 0..240, y 0..120, z 0..90 — which put that stair's wedge on edge between the treads. One `stairs` op with its understructure kept cannot disagree with itself. |

`transform_uvs` and `set_uvs` are **revisit candidates, not permanent exclusions**: once this corpus has a UV-sampling checker material bound to UV0, both become verifiable and should be written back in. The rest are ops these thirteen objects have no honest use for.

## See also

- [`model.examples`](model.examples.md) — the thirteen documents, what each teaches, and their honest verdicts.
- [`model.describe_ops`](model.describe_ops.md) — the generated op and parameter vocabulary this page is counted against.
- [`model.authoring`](model.authoring.md) — the primitive and response readings that are not what the parameter names suggest, including the `shell` / `offset_faces` and `cone top_radius=0` measurements quoted above.
- [`model.explicit-geometry`](model.explicit-geometry.md) — why the format carries no indices outside the machine-generated tier.
