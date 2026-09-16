# static_mesh

Dump-parity live read plus targeted editing for `UStaticMesh` assets — `static_mesh.describe` returns the same shape `asset.dump` writes to `static_mesh.json` (bounds, materials, per-section material assignment, LOD/UV counts, collision, Nanite state), `static_mesh.set_material` binds a material into a StaticMaterials slot, and `static_mesh.set_collision_complexity` chooses how the mesh uses simple versus render-triangle collision. Use `call("asset.dump")` or `call("asset.dump_folder")` for repeatable mesh-content baselines; this namespace is the one-off live read/edit for the currently-loaded asset.

## Geometry audit z-fighting evidence

The `geometry.audit_static_meshes` `z_fighting` check is a warning-only geometric audit. Its
fine spatial grid uses a fixed resolution across the measured model extent; the much smaller plane
epsilon expands triangle bounds but never sets the cell size. Triangles spanning too many fine
cells use a coarse-grid fallback. The fallback counts every coarse reference inspected for each
large triangle before component, AABB, and duplicate filtering; exceeding that reference cap, a
dense fine cell, or a candidate budget becomes `MESH_AUDIT_Z_FIGHTING_UNRUNNABLE` rather than a
partial clean result. The response reports actual grid references, large-triangle count, inspected
fallback references, candidate pairs, and exact overlap tests.

Exact candidates need finite, non-degenerate geometry, parallel or anti-parallel normals, both
triangle-to-opposite-plane distance tests, and winding-independent projected clipping. The reported
plane epsilon is a fixed fraction of the measured model extent, not a world-unit constant. Fighting
pairs are unioned when they share a triangle. Each ranked region reports the sum of its pair overlap
areas, unique triangle and component ids, and at most 16 strongest pairs ranked by overlap area.

This is geometric evidence about duplicate near-coplanar surfaces, not a universal prediction of
what every camera, projection, material, or depth-buffer configuration will show. A warning should
therefore guide inspection and cleanup; it is not proof that a particular captured frame will
flicker. Conversely, a clean geometric result is not a substitute for a visual capture when the
rendering setup itself is under investigation.

## See also

- [`asset`](asset.md) — shared asset-dump and dump-parity live-read workflow.
- [`asset.dump-sidecars`](asset.dump-sidecars.md) — typed sidecar schemas and parity policy.

### static_mesh.set_collision_complexity

Set the Static Mesh Editor's **Collision Complexity** field, recook physics data, refresh loaded components that use the mesh, and save by default. This changes the asset's collision-selection policy; it does not create simple shapes. Use `geometry.generate_collision` before `geometry.convert_to_static_mesh` when the mesh also needs authored simple collision.

Args:

- `assetPath` (string, **required**) — the StaticMesh asset to edit.
- `complexity` (string, **required**) — `project_default`, `simple_and_complex`, `simple_as_complex`, or `complex_as_simple`.
- `save` (boolean, default `true`) — persist the asset after the physics recook.

```js
call("static_mesh.set_collision_complexity", { assetPath: "/Game/Meshes/SM_Platform", complexity: "project_default" })
```

Gotchas: `project_default` stores `CTF_UseDefault`; its `effectiveCollisionTraceFlag` resolves through the current project physics setting and can therefore differ from `collisionTraceFlag`. `complex_as_simple` is suitable for static query/collision geometry but Unreal cannot simulate that mesh as a movable rigid body. The response reports both flags, whether the stored value changed, and how many loaded component physics states were recreated.

### static_mesh.bake_transform

Bake a rotation / translation / uniform scale permanently into a `UStaticMesh` asset — every source-model LOD, the hi-res (Nanite) source, the simple-collision primitives, and the sockets — then rebuild and (by default) save. Use it to fix an asset authored on the wrong axis or origin once, instead of countering the error with a per-instance actor rotation on every placement.

This is tick-unsafe and runs through the shared safe-point/render guard. Before the in-place Build/PostEditChange, the guard quiesces matching live `UStaticMeshComponent`s and Niagara mesh-renderer consumers, then restores and reregisters them after the rebuild; an unquiescable consumer returns `MESH_REBUILD_CONSUMER_NOT_QUIESCABLE` and nothing is built or saved.

Args:

- `assetPath` (string, **required**) — the StaticMesh asset to rewrite.
- `rotation` (object) — `{pitch,yaw,roll}` degrees baked into the geometry (default zero).
- `translation` (object) — `{x,y,z}` cm baked into the geometry (default zero).
- `scale` (number) — uniform scale factor > 0 (default 1).
- `save` (boolean, default `true`) — persist to disk after the rebuild.

```js
call("static_mesh.bake_transform", { assetPath: "/Game/Meshes/SM_GateRing", rotation: { yaw: 180 } })
```

Gotchas: **in-place and destructive** — duplicate the asset first if unsure. The hi-res/Nanite source is transformed too (skipping it would revert the mesh on the next Nanite rebuild). Bounds extensions are scaled but not rotated (axis-aligned padding; the response flags `boundsExtensionsScaledOnly`). Level-set collision elems cannot be transformed analytically — they are skipped and counted in `collision.skipped`. Mirror (negative) and non-uniform scales are rejected.

### static_mesh.describe

Read `UStaticMesh` metadata live in the same JSON shape `asset.dump` writes to `static_mesh.json` — bounds, materials, section-to-slot assignment, LOD/UV counts, lightmap settings, collision — plus one field that is **not** part of the dump shape: `rebuildRenderConsumers`.

Args:

- `assetPath` (string, **required**) — the StaticMesh asset to read.

```js
call("static_mesh.describe", { assetPath: "/Game/Meshes/SM_Platform" })
```

The mesh readout fields make material and UV review possible without spawning an actor:
`static_mesh.describe` and the `asset.dump` `static_mesh.json` sidecar use the shared
`StaticMeshDumpBuilder`; `static_mesh.txt` projects that same JSON, so these fields have one
source of truth across the live read and both sidecars.

- `sections` — the root array is flat, with one row per render section across every LOD. `lodIndex` plus `index` identifies the section; `materialIndex` and `materialSlotName` identify its slot; `firstIndex`, `numTriangles`, `minVertexIndex`, and `maxVertexIndex` locate its buffer range; `bEnableCollision` and `bCastShadow` report the section flags.
- `slotUsage` — one row per material slot with `lod0TriangleCount` and `lod0TriangleFraction`. The fraction denominator is every LOD0 section triangle, including sections whose material index is invalid, so the fractions may sum below 1 when assignment is broken.
- `uvChannelsByLod` — the usable, authored UV-channel count from `UStaticMesh::GetNumUVChannels()` for each render LOD, in LOD order. PinWright is an editor plugin, so this intentionally follows the asset's source MeshDescription; if that source is unavailable, the engine API reports zero rather than guessing from render-buffer capacity. It deliberately does not use the render vertex buffer's allocated texture-coordinate width, which can be `MAX_STATIC_TEXCOORDS` even when fewer channels are populated. `lightMapCoordinateIndex` is the channel selected by the asset; compare it with the matching count before trusting `lightmapResolution`.

Meshes without render data return empty `sections` and `uvChannelsByLod` arrays. Their material slots still appear in `slotUsage` with zero counts and fractions.

**`rebuildRenderConsumers` — read it before rebuilding this mesh in place.** All five in-place StaticMesh rebuild routes — `asset.generate_lods`, `asset.nanite_rebuild_mesh`, `static_mesh.bake_transform`, `geometry.set_lod_settings`, and `model.compile` — use the shared safe-point/render guard. Before Build/PostEditChange, it enumerates registered, render-state-created `UStaticMeshComponent`s whose `GetStaticMesh()` is this target and registered, render-state-created `UNiagaraComponent`s whose assigned system has an enabled mesh renderer that explicitly lists this target. Valid runtime Niagara mesh bindings are conservative may-reference candidates.

- `scannedClasses` — the class paths the diagnostic walk attempts: `/Script/Engine.StaticMeshComponent` and `/Script/Niagara.NiagaraComponent` when Niagara is loaded. This describes scan types, not matched instances, so it remains nonempty even when `components` is empty.
- `components` — path names of matching live components. The guard uses `FComponentRecreateRenderStateContext` to release their render state, flushes before and after the rebuild callback, and recreates the state on scope exit. If any matched consumer remains render-state-created after quiescing, the operation returns `MESH_REBUILD_CONSUMER_NOT_QUIESCABLE`; nothing is built or saved.

Gotchas: StaticMesh components are matched by the target mesh, while Niagara is matched through enabled emitter mesh-renderer properties. Dynamic bindings cannot be resolved from the asset graph and are intentionally conservative. An empty `components` list means no matching live proxy holder was found, not that no scan was attempted. Delete or deactivate a listed component to clear it.
