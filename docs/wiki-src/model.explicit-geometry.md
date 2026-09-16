# model.explicit-geometry

The explicit-data tier contains `procedural_mesh`, `append_buffers`, `append_vertex`, `append_triangle`, `delete_vertex`, `delete_triangle`, `set_vertex_position`, and collision `convex points=[…]`. It is **machine-generated** and the only `.pwmodel` tier that addresses geometry by index. Read [`model.authoring`](model.authoring.md) first; see `Examples/pwmodel/mobius_band.pwmodel` for a worked example.

## Why the rest of the format carries no indices

Every other op selects by **predicate**—`extrude face_direction=(0,0,1)`, `bevel` on polygroup edges, or a boolean's tool volume—so it survives shape changes. `delete_triangle index=182` does not: inserting one upstream station slides later ids, cuts elsewhere, and **nothing fails**; the document still compiles to a silently wrong mesh.

`pwmodel 0` stores no face/edge/vertex/triangle references elsewhere. Its exception contract is: **the buffer and its indices are one artifact, regenerated together or not at all.** To enlarge an indexed hole, rerun the generator and replace the part; never hand-edit ids.

## When it is the right tool

Use it when the op vocabulary cannot express the shape. A half-twisted band is a clean case: no primitive twists along a closed path; `twist` rotates around an axis, while `sweep` / `revolve` take a fixed profile. The surface must be evaluated per station.

It is the wrong tool when generators plus modifiers can reach the mesh: re-editability, diffs, and one-dimension changes depend on enumerable ops. Put procedural generation in a builder that *emits* `.pwmodel` text.

## Ids are the emission order, and that is measured, not promised

`docs/pwmodel-format.md` lists “vertex and triangle ORDER” under **Not guaranteed, and do not assume**. On UE 5.8, a fresh `procedural_mesh` filled by `append_buffers` assigned ids in supply order: two disjoint quads, deleting triangle ids 2 and 3, returned the second quad. Re-measure before relying on this. Emit the indexing scheme as a comment, e.g. `vertex id = station * 10 + perimeter`.

Appends add at the end and never disturb existing ids. **Deletes leave ids sparse rather than renumbering**, so earlier ids stay valid. `uv mode=xatlas` is the exception: it compacts and renumbers, so never address an index after it.

## The triangle star of a grid vertex is six, not eight

`delete_vertex` removes the vertex **and every triangle using it**, so on a quad grid it cuts. An interior vertex does not own eight triangles: with every quad split on one diagonal, two quads contribute both triangles and two contribute one each—six triangles and a **sheared** six-vertex ring.

The resulting hole is a slanted hexagon. Move the six ring vertices with `set_vertex_position` to design its outline; do not change station count just to obtain convenient ids.

## `delete_vertex` after `delete_triangle` is a no-op, not a failure

Cutting with `delete_triangle` and then cleaning its orphan with `delete_vertex` asks for work already done. Triangle removal drops isolated vertices, so the later op is **skipped and the compile continues**:

```
PWMODEL_VERTEX_ALREADY_REMOVED  'delete_vertex' names vertex 4, which the mesh no longer
carries - a previous 'delete_triangle' removed it along with the triangle that was its last
user. The op is skipped; the mesh already holds the state it asked for. Delete the statement
to silence this. An id the mesh NEVER carried is still an error.
```

An orphan-cleanup statement is therefore safe to emit unconditionally. **The warning covers only an id the mesh once carried**; an id below 0 or at/past `MaxVertexID` was never valid and remains hard `PWMODEL_OP_FAILED [INVALID_VERTEX]`, aborting compile. Sparse deletes keep prior valid ids inside that bound, so a bad index cannot hide behind the benign case.

Deleting triangles cannot leave a stray vertex. Cut with `delete_triangle` **or** `delete_vertex`; they produce identical openings when the triangle list is the vertex's star.

## `append_triangle` takes positions, and that has three consequences

No `pwmodel 0` op builds a triangle from ids. `append_triangle` takes positions `v0=` / `v1=` / `v2=` and creates three new vertices each time.

- **A patch does not connect to its rim.** Run `weld_vertices`. It matches coincident *edges* whose coordinates parse to the same doubles, not merely close points. Retuning the rim without regenerating the patch makes the weld quietly find no pairs; `health.boundaryEdges` catches the crack.
- **Slots are explicit.** `append_triangle` takes `material="<Slot>"`, uses the model-wide slot table like `append_buffers`, and takes neither `color=` nor `material_id=`. Untagged allocates implicit `Default`, not slot 0. Measured with `material="Cast"` on the base and twelve untagged triangles: `materialSlots: 2`; `static_mesh.describe` returned `Cast`/`BasicShapeMaterial` and `Default`/`WorldGridMaterial`, with the patch in the grid material.
- **Overlaps may be silent.** `PWMODEL_UNUNIONED_OVERLAP` compares sibling bounding boxes; twelve wall triangles once produced nine warnings, each suggesting undefined `union { }` on open geometry. The check now skips pairs where either mesh has boundary edges, including open patches, `plane`, `disc`, `ring`, uncapped `revolve`, and `procedural_mesh` + `append_buffers` shells. Two overlapping **open** shells can still z-fight without warning; closed interpenetrating solids still warn.

Bulk geometry belongs in `append_buffers` — one line, local indices, no duplication, and it alone carries `uvs=`, `normals=` and `colors=`. `append_triangle` is the patch op: one triangle, three positions, the same `material=`.

## `append_vertex` only means something inside `hull { }`

Inside a part, `append_vertex` adds a vertex no triangle can reference because no op builds triangles from ids. The meaningful use is collision `hull { }`: it runs the body through the normal pipeline, then keeps only **vertex positions**, so `procedural_mesh` plus `append_vertex` with no triangles is a legal point cloud and hulls correctly.

This is the long form of `convex points=[…]`, the compact equivalent a generator should emit. Neither is an authoring path; do not hand-write vertex lists.

## Collision for a closed loop is a decomposition, not a hull

A single hull around a ring fills its hole. Emit one `convex points=` per arc segment: for a 40-station ring, ten 36-degree slabs, each hulling four corner rails sampled at three stations, use twelve points per element and reproduce the ring closely. `hull { }` similarly discards concave features silently.

## Winding, and what a wrong one looks like

Unreal's visible face is the **negation** of the right-hand-rule normal. For `(v0, v1, v2)`, `(v1-v0) x (v2-v0)` must point *away* from the viewer; `VectorUtil::Normal` returns `(V2-V0) x (V1-V0)`. Assert one dot product per emitted triangle against a known outward direction.

**Assert it; a render cannot reliably show it.** A *subset* of reversed triangles appears as black holes, but a **uniformly** reversed closed shell renders like a correct one: culling shows the wall facing the camera, and `health` still says closed/manifold with 0 boundary edges. Render before closing (one-sided geometry is invisible from behind), or put a camera inside a thick shell (outward geometry vanishes; an inverted shell shows a lit interior).

**Wrong winding and an open surface can look identical.** An open surface is black from behind; `flip_normals` only moves the black side. Close it or add thickness with `shell`/`extrude`; `PWMODEL_MESH_NOT_CLOSED` reports boundary-edge count. A closed black mesh may simply face away from every light; a render cannot confirm winding.

**When `extrude` closes a surface, `direction` controls facing.** It displaces original triangles along `direction` and reverses the stationary duplicate, so outward requires `direction` parallel to the *negated* RHR normal, not the RHR normal. The wrong sign made `origami_crane`'s `sheet` inside-out. See [`model.authoring`](model.authoring.md) § Primitive and response readings that are not what the name suggests.

## Verify on `health`, not on counts

`model.compile` returns `health`: `isClosed`, `boundaryEdges`, `degenerateTriangles`, `nonManifoldVertices`, and `componentCount`. It is the cheap signal that explicit-buffer surgery landed. An eight-sided bore wall stitched to a six-sided opening compiled `success: true` with 802 triangles and `boundaryEdges: 6`; no other response field or silhouette flagged it. Gate cuts/re-caps on `isClosed` / `boundaryEdges`, then inspect a shaded perspective render.

## Normals on a part that `append_buffers` opened

`append_buffers` without `normals=` leaves **no normal layer**, so the following normals op chooses the shading model.

`recalculate_normals` warns — `RecomputeNormals: TargetMesh did not have normals to recompute; falling back to per-vertex normals` — and **the fallback is the answer, not a failure**: averaged per-vertex normals are fully smooth. The warning fires once per buffer-built part and does not mean the op did nothing.

`split_normals split_angle=0` is the faceted counterpart and the closest format equivalent to engine `SetPerFaceNormals`, which **has no op**:

- `SetPerFaceNormals` splits every corner, producing exactly `3 * triangles` render vertices.
- `split_normals split_angle=0` splits where dihedral angle **exceeds** the threshold; exactly coplanar pairs keep shared corners. On a 2366-triangle mesh of 31 interpenetrating shells it produced 6656 render vertices versus 7098 for per-face normals, a difference of 221 planar quads. **Shading is identical**; the count differs only for vertex-for-vertex reproduction.

**`split_angle` reaches the engine through a cosine; a negative value is not "split even harder".** It behaves as its positive twin: the same mesh at `split_angle=-1` produced 5706 render vertices, **fewer** than at 0, welding everything within one degree. Zero is the maximum split.

## A `lightmap` channel has to be authored, or it is clamped away

`lightmap channel=N` sets the asset index; it does not create UVs. Asset creation deliberately leaves `bGenerateLightmapUVs` **off**, because generation would repack and discard authored UVs. If parts provide only channel-0 `uvs=` but the document requests `channel=1`, it clamps to 0 and reports:

```
PWMODEL_STAGE_WARNING  Requested lightmap channel 1, but the mesh does not carry that UV
                       channel; the asset uses channel 0
```

Use `uv channel=1 …`. On this tier, `mode=xatlas` compacts/renumbers the mesh, and a second channel splits render vertices at seams; adding one to a reproduction document changes the count it was meant to preserve.

## Porting a Geometry Script bake onto `append_buffers`

Passing `model.compile` the same buffers as a `copy_mesh_to_static_mesh` script reproduces the **source** mesh exactly—vertex/triangle counts, positions, and bounds—but can produce a different LOD0 render-vertex count because writers convert dynamic mesh to mesh description differently.

On a 412-vertex mesh with exactly one coincident-position pair (two shells meet at a shared apex with the same position/UV), Geometry Script welded one render vertex while `model.compile` kept both: 411 versus 412. **It is not a build setting.** `bRemoveDegenerates` was the only differing asset entry; matching it still left the compiled count at 412.

Gate a port on `assetTriangleCount`, `static_mesh.describe` bounds, `lightmapResolution`, `collisionTraceFlag`, and material slots (all matched to the last decimal). Treat a small `verticesByLod` difference as a writer property. Confirm with captures of original and port at one pinned exposure; two ports produced **byte-identical PNGs** in three-quarter perspective and orthographic edge-on elevation.

## See also

- [`model.authoring`](model.authoring.md) — ordinary authoring: structure, transforms, materials, collision.
- [`model.examples`](model.examples.md) — the thirteen worked documents, including `mobius_band` and `origami_crane`, the two that use this tier.
- [`model`](model.md) — why the format exists, and the compile/provenance contract.
- [`static_mesh.describe`](static_mesh.describe.md) — read the compiled asset's material slots and collision counts back.
- `docs/pwmodel-format.md` in the plugin folder — normative grammar and the `PWMODEL_*` catalog.
