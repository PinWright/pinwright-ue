# level-building.instancing-and-scatter

How to place thousands of repeating meshes without thousands of actors: instanced components, PCG generation, Python transform traps, and deliberate scatter.

## Instancing Thousands Of One Mesh

Never spawn thousands of `StaticMeshActor`s for a repeating mesh — a forest of 8000 trees built that way is 8000 actors in the outliner and `.umap`. Choose either instanced route:

- `foliage.add_instances {foliageTypePath, transforms}` — typed, no documented cap, and `foliageTypePath` accepts a static-mesh path (the `UFoliageType` is auto-created). Instances land in the level's foliage actor, so they are not addressable by your own actor prefix.
- **A HISM component you own** — one actor per mesh variant, named with your layer's prefix, deletable and rebuildable as a unit. There is no typed verb; drive it from `python.execute`:

```python
a = unreal.EditorActorSubsystem().spawn_actor_from_class(unreal.Actor, unreal.Vector(0, 0, 0))
a.set_actor_label('FO_Trees_Oak'); a.set_folder_path('MyLevel/Trees')
comp = unreal.new_object(unreal.HierarchicalInstancedStaticMeshComponent, a, 'HISM_Trees_Oak')
a.set_editor_property('root_component', comp)          # without this the component is not the root
comp.set_static_mesh(unreal.load_asset('/Game/MyLevel/Meshes/SM_Tree_Oak'))
comp.add_instances(instance_transforms=transforms, should_return_indices=False,
                   world_space=False, update_navigation=False)
assert comp.get_instance_count() == len(transforms)
```

`add_instances`' Python signature has changed across 5.x — if the keyword form raises, use `add_instance(t, False)` per transform. Instances serialise into the `.umap`; `level.save` persists a HISM layer like any actor, and rebuilding rewrites the map package.

**`mark_render_state_dirty` is a parameter, never a method.** There is no `comp.mark_render_state_dirty()` on UE 5.8 — `UActorComponent::MarkRenderStateDirty()` carries no `UFUNCTION`, so it never reaches Python and the obvious call is an `AttributeError`. The flag rides on the instance-write verbs instead, defaulting to `False` on every one:

- `update_instance_transform(instance_index, new_instance_transform, world_space=False, mark_render_state_dirty=False, teleport=False)`
- `batch_update_instances_transforms(start_instance_index, new_instances_transforms, world_space=False, mark_render_state_dirty=False, teleport=False)`
- `batch_update_instances_transform(start_instance_index, num_instances, new_instances_transform, world_space=False, mark_render_state_dirty=False, teleport=False)`
- `set_custom_data_value(instance_index, custom_data_index, custom_data_value, mark_render_state_dirty=False)`

**Pass `True` on the last write of a pass and nowhere else.** Left `False` throughout, the edits land in the component and never reach the screen: the transforms read back correct while the viewport still draws the old layout, which is indistinguishable from a write that failed. Set on every instance, each call re-sends the whole instance buffer.

```python
last = len(new_transforms) - 1
for i, t in enumerate(new_transforms):
    comp.update_instance_transform(i, t, world_space=False,
                                   mark_render_state_dirty=(i == last))
```

`add_instance` / `add_instances` / `remove_instance` / `clear_instances` carry no such parameter — the flag exists only on the update and custom-data verbs above.

**`register_component()` and `is_registered()` do not exist in Python either — same family, same cause.** `UActorComponent::RegisterComponent()` and `IsRegistered()` carry no `UFUNCTION` on UE 5.8, and `AActor::AddComponentByClass` is marked `ScriptNoExport`, so all three natural spellings after `unreal.new_object` raise `AttributeError`. Do not write them and do not write a registration read-back around them. The recipe above needs neither: `set_editor_property('root_component', comp)` on a spawned actor is what puts the component in the level, and `get_instance_count()` is the read-back that proves the write landed. If you need a component that is registered by C++ at creation time, use `call("actor.add_component")` instead of building it from Python.

## Running A PCG Graph

`pcg.generate` is a **ticketed async job**, not a blocking call. PCG advances only inside the editor tick under a per-frame budget, so generation takes an unbounded number of frames and wall-clock time follows the editor frame rate; an unfocused editor is throttled and can stretch a seconds-long graph into minutes. Kick it off and poll; do not hold a request open.

```js
call("pcg.generate", { actorName: "PCG_Trees", force: true, wait: false })
// -> { status: "running", ticket_id: "j_...", monitor_path: "Saved/PinWright/jobs.jsonl", ... }

call("system.job_status", { ticket_id: "j_..." })
// poll to a terminal status; cancel with system.job_cancel {ticket_id}
```

- The kickoff returns in about a second. Errors that can be decided before any work starts (`ACTOR_NOT_FOUND`, `NO_PCG_GRAPH`, and friends) come back inline, before a ticket exists.
- A streaming-capable client that does not pass `wait: false` blocks and streams instead, returning the finished result inline. **Even then the ticket still holds the real outcome**, so a dropped stream or a transport deadline no longer loses a generation that succeeded — poll the ticket.
- `cleanupFirst: true` removes previously generated components before the pass. Without it, a plain re-generate reuses them, which is why an edited graph can appear to have changed nothing.
- `timeoutSeconds` (default 1800) caps how long the **job** waits. It never cancels the generation itself; `system.job_cancel` does that.

**Judge a generation by `instanceCount`, not `pointCount`.** `pointCount` counts only the data that reached the **graph's Output node**, which is a different question from "did this graph build anything". A graph ending in a Static Mesh Spawner whose `Out` pin is not wired to the Output node emits nothing there while spawning thousands of instances — that is correct engine behaviour, not a defect. So `pcg.generate` omits `pointCount` / `dataCount` entirely in that case, sets the always-present `graphOutputAvailable: false`, and explains the omission in `warnings[]`. It reports what actually survived the pass instead: `instanceCount` (ISM/HISM instances) and `spawnedActorCount`, read off the component's managed-resource list, behind an always-present `resourceCountsAvailable`. Wire the graph's Output node if you specifically want a point count. On a partitioned component the resources live on the per-grid local components; UE 5.7+ walks them and the counts cover them (`localComponentsWalked: true`).

PCG is not always the right scatter tool. A jittered lattice with a noise density mask (below) is easier to reason about and reproduce for an organic-looking stand; a general PCG graph is a heavier dependency for the same result.

## Python Transform Traps

**`unreal.Rotator(a, b, c)` is `(roll, pitch, yaw)`, not `(pitch, yaw, roll)`. Always pass by keyword.** Passing an intended yaw positionally feeds it into `pitch`, which lays every instance on its side. It is easy to miss for a long time, because a scatter layer viewed from overhead often looks the same either way — a toppled cone still reads as a blob from directly above. Verify once in your own session — `unreal.Rotator(11, 22, 33)` reports roll 11, pitch 22, yaw 33 — and treat any inherited script that builds Rotators positionally as broken until proven otherwise. Same rule for `unreal.Vector`.

`unreal.Transform(Vector, Rotator, Vector)` positionally **is** translation / rotation / scale — that struct coerces by argument type, so it is safe.

## Scatter That Reads As Deliberate

- **Dart-throwing with a minimum-distance test is the wrong tool.** Rejection sampling averages far above its own minimum spacing, so the stand comes out visibly sparse and clumped. A **jittered hex grid** gives direct control: lay a hex lattice at the target spacing, offset alternate rows by half a step, and jitter each point by roughly ±0.18 of the spacing.
- **One jittered lattice still reads as rows, and ±0.18 is not enough to hide it.** Rows stay geometrically disjoint below `jitter = sqrt(3)/4 ≈ 0.4330` — 86.6% of the legal range, the default included — which is invisible from overhead where canopies bridge the gap and obvious at eye level where trunks do not. The measurement, the superposition workaround and what that workaround costs are on [`vegetation-authoring.scatter-geometry`](vegetation-authoring.scatter-geometry.md).
- **Space by canopy or footprint diameter, not by taste.** Centres 1.0–1.25 canopy-diameters apart reads as a stand; a 950 uu canopy wants roughly 1000–1200 uu spacing.
- **One global grid, not one grid per zone**, or overlapping zones double-seed their intersection.
- **Fix the seed.** A named constant seed makes the layout reproducible and makes a diff between two runs meaningful.
- Vary per-instance uniform scale by ~±15% and yaw fully; keep pitch and roll at zero unless you mean it (see the Rotator trap above).
- Carve corridors by thresholding a smooth field over the same grid rather than by hand-deleting instances, so the carve survives a rebuild.

**Do not hand-roll this in `python.execute` — [`spatial.scatter_layout`](spatial.md) implements every bullet above.** It is a pure function (no world, no actor, no trace) returning `transforms[]` in the shape `foliage.add_instances` / `actor.spawn_batch` / `actor.set_instance_transforms` consume; seat the result with `spatial.ground_instances` or `spatial.ground_actors`, because the Z it emits is the region plane. The reason to reach for the verb rather than the recipe is the jitter: it has to be a **continuous** offset, and the hand-rolled `(-1, 0, 1) * amount` version re-lands two thirds of the points onto one hex sublattice — banding from overhead, rows from the ground, and every count, spacing and bounds check still green.


## See also

- [`level-building`](level-building.md) for the rest of the build workflow.
- [`level-building.terrain-and-water`](level-building.terrain-and-water.md) for the ground queries every scatter pass needs.
- [`foliage`](foliage.md) and [`pcg`](pcg.md) for the per-verb argument reference.
- [`level-review`](level-review.md) for verifying a scatter pass numerically instead of by eye.
