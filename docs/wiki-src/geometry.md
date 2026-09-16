# geometry

Create and modify procedural or existing mesh geometry with editor modeling helpers, including UVs, collision, LODs, and asset conversion.

Use the `actor`/`level` namespaces only to place or transform existing actors.

## Availability

`geometry.*` requires the **GeometryScripting** engine plugin. The integration auto-loads when that plugin is enabled in the host project; when it is disabled, these methods are unregistered and calling one returns `PLUGIN_DISABLED` (enable GeometryScripting and restart the editor). `spline.*` does NOT require GeometryScripting; it lives in the always-loaded core module.

## Geometry actor target identifiers

Geometry actor-target slots (`actorName`, `targetActor`, `toolActor`, `trimActorName`,
`sourceActor`, profile actor names, and `splineActorName`) resolve in
this order: full object path, exact internal object name, then exact display label. Geometry does
not use display-label substring matching. Path and internal-name matches are deterministic and
outrank labels. An exact path or internal name for an actor of the wrong class is not reinterpreted
as another actor's label.

Display labels are not unique in Unreal. A duplicate label is rejected with
`AMBIGUOUS_ACTOR_NAME`; the error data includes every candidate's `label`, internal `name`, full
`path`, and `class`, plus `matchedBy` and a `candidateCount` equal to the emitted candidate array.
When `matchedBy` is `label`, re-issue the request with a unique candidate `name` or `path`; when
it is `objectName` or `objectPath`, use a full object path. DynamicMesh targets filter label
candidates to DynamicMeshActors, while exact path/name requests identify the actor actually named
(and then fail the DynamicMesh target check if it is not a DynamicMeshActor). Spline and any-actor
lookups use the candidate's actual actor class.

Mutating existing targets preserve the request's legacy `actorName` echo and also return the
canonical resolved identity as `actorPath` and `actorObjectName`. Multi-actor operations qualify
these fields with the role (`targetActorPath`, `toolActorPath`, `trimActorPath`, `sourceActorPath`,
or `splineActorPath`, with matching `...ActorObjectName` fields).

`actorGuid` is emitted as spawn verification data, but there is no geometry input contract for
resolving an actor by GUID; use the path or internal object name instead.

## Primitive orientation

Procedural primitives use a **fixed local-axis layout** set by the underlying Geometry Script call
(built at `FTransform::Identity`, then transformed on the actor); named dimensions do not always map
to the axis their names suggest:

- **`create_box`:** the *vertical* dimension is `depth`, not `height` — see
  `call("geometry.create_box")`.
- **`create_arch`:** lies flat by default; `rotation.roll = -90` stands it apex-up —
  see `call("geometry.create_arch")`.
- **`get_vertex_position`:** returns **mesh-LOCAL** coords, not world-space — see
  `call("geometry.get_vertex_position")`.
- **`create_pipe`:** centre-placed like every other `create_*` primitive (`height` 100 spans
  local z -50..+50). It was **base**-placed (z 0..100) until that was fixed, so a recipe written
  against the old behaviour now sits half a height too low — see `call("geometry.create_pipe")`.

## Warp deformers (twist / taper / bend) — `extent` units & symmetry

`geometry.twist`, `geometry.taper`, and `geometry.bend` each take an `extent`
number that is **in world units** (the same units as the mesh's `height` /
bounding-box span), **not** a 0..1 fraction of the mesh. All three read
`symmetricExtents` and `lowerExtent` from the request, and all three measure the
extent along `axis` about `center`. `symmetricExtents` defaults **true**, so by default
`extent` is a **symmetric half-extent measured about the mesh origin**: the
deform spans `[-extent, +extent]` along the warp axis. For a center-origin
primitive of height H, the deform covers the **whole** mesh only at
`extent ≈ H/2` — passing `extent ≈ H` over-covers by 2× (the natural guess
"extent = full height" over-covers by 2x). The default `50` covers a
100-unit-tall centered mesh. This convention is shared across all three verbs;
see each method page.

**`symmetricExtents: false` is the answer for a mesh that does not straddle the
origin**, where a symmetric extent always covers the wrong half. The span becomes
`[-lowerExtent, +extent]`, so the two ends are set independently. `lowerExtent`
(default 10) is read only in that case. `bSymmetricExtents` was hardcoded true
until these two were published, which left the engine's lower-extent field dead.

**`axis` and `center` are the other half of the extent story, and they are what makes
a warp reachable on geometry that is not Z-aligned and origin-centred.** Every one of
these ops drives an engine space deformer that measures its extent along the **gizmo
frame's Z** and centres it on the **frame's origin**. That frame used to be hardcoded
to identity, so the only geometry the three could warp was geometry already aligned to
local +Z through the local origin: a form elongated along X could not be tapered along
its own length at all, and a part placed away from the origin got a warp centred off in
space. Neither is expressible through `extent` / `lowerExtent`, because both of those
are measured *along* the axis that was hardcoded.

- **`axis`** (`'x'` / `'y'` / `'z'`, default `'z'`) selects the axis the extent spans.
  The two perpendicular axes are taken cyclically — for `axis: 'x'` they are Y then Z —
  which is what gives `geometry.taper`'s `flareX` / `flareY` a meaning on a non-Z axis,
  and matches the handedness `geometry.harmonic_deform` measures its azimuth with.
- **`center`** (`{x, y, z}`, default `{0,0,0}`) is the point the extent is centred on.
  It is the **axis line, not a bounding-box centre**: a box-derived centre would move
  whenever an earlier op changed the mesh's extent, so the same recipe would deform
  differently depending on what ran before it. Move a mesh by `T` and set `center` to
  `T` and you get the un-moved result moved by `T`, exactly.

Both default to the identity frame that was there before, so an existing request is
byte-for-byte unchanged. These are the same two parameter names, with the same meaning,
that `geometry.harmonic_deform` already publishes — the deformer family is deliberately
one vocabulary.

**All three guard the normal overlay first, and the guard can edit your mesh.** The engine's
space-deformer ops take a normal-overlay element's *parent vertex* straight into `GetVertex`
after checking only that the element exists — and an element can exist with no parent whenever
an earlier op left one allocated that no triangle references. The read is out of bounds and
**kills the editor process** with no diagnostic. So:

- An **orphaned** element is freed and the call **succeeds with a `warnings` entry** naming how
  many were freed. Freeing them cannot change the shaded result — no triangle used them. This is
  a repair, not a refusal: the state is legal and engine-produced.
- A mesh with **no normal layer at all** is refused with `INVALID_NORMAL_OVERLAY`. Run
  `geometry.recalculate_normals` first.
- A mesh with no attribute set at all passes through untouched — the engine's normal pass is
  itself gated on having attributes.

`shell` and `bevel` carry the sibling UV guard and **refuse** rather than repair; see
`### geometry.shell`. The `.pwmodel` wording of both guards is in
[model.authoring](model.authoring.md).

## Editing existing meshes — the StaticMesh round trip

The namespace is a two-way door. `geometry.create_from_static_mesh` loads a `UStaticMesh` asset (or
the mesh of a placed actor) into an editable `DynamicMeshActor`, every `geometry.*` op then applies,
and `geometry.convert_to_static_mesh` bakes it back. Use it to re-edit your own bake, or to modify a
pre-existing project mesh that was never dynamic.

```js
call({ path: "geometry.create_from_static_mesh", args: { assetPath: "/Game/Meshes/SM_Tower" } })
call({ path: "geometry.bevel",  args: { actorName: "SM_Tower_Edit", distance: 4 } })
call({ path: "geometry.convert_to_static_mesh",
       args: { actorName: "SM_Tower_Edit", assetPath: "/Game/Meshes/SM_Tower", overwrite: true } })
```

The `overwrite: true` on the last call is load-bearing: the default create path bakes **geometry
only** and resets the asset to the default material, while the in-place path preserves the target's
material slots, section map, other LODs, and collision. Without it a round trip silently costs you
the materials on every iteration.

Both bake verbs refuse `/Engine/` assets with `SECURITY_VIOLATION`; the engine refuses them too.

**Dynamic meshes persist.** A `DynamicMeshActor`'s geometry is serialized into the level package
(`UDynamicMeshComponent::MeshObject` is an instanced `UPROPERTY` and `UDynamicMesh::Serialize`
writes the full mesh), so it survives level save + editor restart — you do **not** have to bake to a
StaticMesh just to keep your work. Bake when you need instancing, Nanite, collision cooking, or an
asset other levels can reference. Every mutating and spawning `geometry.*` verb marks the level dirty
on its own, so no priming step is needed — just finish a batch of mesh work with an explicit
`level.save`. See [`safe-mutation-save`](safe-mutation-save.md).

**Do not `actor.duplicate` a big dynamic mesh.** Level *save* serializes the mesh properly, but
editor *duplicate/copy-paste* takes a different path (text-based property export) that gives up
above `geometry.DynamicMesh.TextBasedDupeTriThreshold` triangles (default 200 000) and silently
substitutes a small placeholder **cube**. To copy a heavy dynamic mesh, bake it with
`geometry.convert_to_static_mesh` and duplicate the asset, or re-run
`geometry.create_from_static_mesh` with a second `name`.

## UV unwrap verbs (auto_uv vs unwrap_uv)

`geometry.auto_uv` and `geometry.unwrap_uv` are the **same XAtlas auto-unwrap op**
(`AutoGenerateXAtlasMeshUVs`). `unwrap_uv` is the canonical verb; `auto_uv` is an
alias that takes the same `uvChannel` param (default 0). Reach for **`unwrap_uv`** by
default. `geometry.pack_uv_islands` is **not** one of them: it repacks the islands a
mesh already has and never regenerates them, so it is the last step of a UV pass, not
another way to start one.

## Skinned characters — the SkeletalMesh round trip

The skeletal twin of the StaticMesh pair above, plus the binding step that has no static
equivalent. `geometry.create_from_skeletal_mesh` loads a `USkeletalMesh` (bones and BASE skin
weights included) into an editable `DynamicMeshActor`, `geometry.bind_skin_weights` binds the mesh
to a skeleton, and `geometry.convert_to_skeletal_mesh` writes it back. These are the only verbs in
the plugin that **create** a `USkeletalMesh` or write its **base** skinning.

```js
call({ path: "geometry.create_from_skeletal_mesh", args: { assetPath: "/Game/Chars/SKM_Hero" } })
call({ path: "geometry.bevel", args: { actorName: "SKM_Hero_Edit", distance: 2 } })
// bind LAST, after every geometry edit
call({ path: "geometry.bind_skin_weights",
       args: { actorName: "SKM_Hero_Edit", skeletonPath: "/Game/Chars/SK_Hero_Skeleton" } })
call({ path: "geometry.convert_to_skeletal_mesh",
       args: { actorName: "SKM_Hero_Edit", assetPath: "/Game/Chars/SKM_Hero", overwrite: true } })
```

**Bind last, and re-bind after any edit.** The skin-weight attribute pads vertices added after the
bind with *empty* influences, so geometry added afterwards is unskinned. The attribute still
exists, which is all the engine checks — a raw Geometry Script pipeline therefore writes a
partly-unweighted asset and reports success. `geometry.convert_to_skeletal_mesh` re-measures every
vertex and refuses with `SKIN_WEIGHTS_INCOMPLETE` instead; re-run `bind_skin_weights` and write
again. Some ops survive a bind and some do not — the attribute interpolates weights onto vertices
born from an edge split or a face poke, but not onto vertices appended outright (`append_vertex`,
`append_triangle`, `append_buffers`), and which category a given verb falls into is an
implementation detail of that verb. Do not try to predict it: re-bind after every geometry edit,
or read `fullyWeighted` in the bake response.

**Booleans no longer corrupt skinning, and the repair reports itself.** A boolean over a skinned
mesh used to hand its new vertices the bone weights of vertices it had just DELETED: the engine
frees those vertex IDs and then re-uses them for the appended geometry, and the skin-weight layer
never clears a re-used slot. Measured on stock `SKM_Manny`, one `boolean_subtract` produced
vertices at the feet 0.93-weighted to `head` 162 cm away, with every other bone nearer — and every
verb in the chain reported success. `boolean_union`, `boolean_subtract`, `boolean_intersection`,
`boolean_trim` and `self_union` now snapshot the mesh first and re-derive, from that snapshot's
surface, every vertex whose weights cannot be traced to a pre-operation vertex; untouched vertices
are left byte-identical. The response carries `skinWeights: {verticesTransferred,
verticesUnresolved}` whenever the target was skinned. `verticesUnresolved` above zero is the one
case where the result is still suspect — audit it before baking.

**`fullyWeighted` also means the influences are plausible.** A vertex dominated by the bone
FARTHEST from it of every bone in the skeleton is unweighted for every practical purpose, so
`bind_skin_weights` and `convert_to_skeletal_mesh` count it as one: they report `verticesFarBone`
(and `farBoneWorst: {vertex, bone, distance}` when it is non-zero) and answer `fullyWeighted:
false`. The rule is ordinal, not a distance threshold, so it needs no tuning — but it measures the
mesh against the reference pose its own bone attributes carry, so a mesh deliberately moved away
from its skeleton will flag. It reports; it never blocks a write.

**These write base skinning, `skeleton.*` does not.** Every `skeleton.*` weight verb
(`normalize_weights`, `prune_weights`, `set_vertex_weights`, `copy_weights`) writes a
*named skin weight profile* — the engine's alternate-influence channel — and leaves the
LOD's base section skinning untouched. If you need the weights the renderer uses by
default, they are here.

**`overwrite` is a real gate, not a branch selector.** Creating onto an occupied `assetPath` is
refused with `ASSET_EXISTS`, because the engine's create path does not refuse: it reuses the
existing asset and empties its LODs, materials, reference skeleton and physics asset in place,
without a prompt.

## Clamped inputs and the `warnings` response field

Every `Append*` generator inside Geometry Script floors its own step counts with a bare
`FMath::Max` and returns success with no diagnostic, so a below-floor value is not an
error the engine reports — it is a number quietly swapped for another one. `cylinder
segments=0`, `=1`, `=2`, `=3` and `=-5` all produce the identical 18-triangle prism.

These verbs clamp at their own layer first, and **say so in the response**. When a call
alters any value you passed, the result carries a `warnings` array of plain strings naming
the parameter, the value you sent and the value that ran:

```json
{"name": "Pillar", "warnings": ["segments clamped from 1 to 3 (valid range 3-256)"]}
```

- The field is present **only when something was clamped**. A call that trips nothing has
  no `warnings` key at all, so its absence is the normal case and not an error to handle.
- Two rules decide what a below-range value becomes. `<= 0` reads as *unset* and takes the
  verb's own default; a positive value under the floor reads as a typo and is raised to the
  floor. Both warn. The exception is a parameter whose legal floor is 0 —
  `create_cylinder`/`create_cone`/`create_pipe` `heightSteps` and `create_plane`
  `widthSubdivisions`/`depthSubdivisions` — where 0 means "no interior loop" and is honoured.
- Every count is also capped, and the cap warns too — but **the ceiling is not universal**. It is
  **256** (`GEOM_MAX_SEGMENTS`) everywhere except `create_stairs` / `create_spiral_stairs`
  `numSteps`, which caps at **400** (`GEOM_MAX_STAIR_STEPS`). A step count is not a segment count:
  it had been taking the segment ceiling by inheritance, which capped a 300-step staircase for no
  measured reason. 400 is measured — the solid stair generator costs `2n^2 + 10n` triangles, 324,000
  at 400 steps and inside the dynamic-mesh budget, where 512 steps would breach it on its own.
- **The floors are not uniform.** Guessing 3 everywhere is wrong: capsule `hemisphereSteps`
  floors at 2 (it steps a half-arc, not a closed loop), `create_arch` `majorSteps` and `revolve`
  `steps` at 2 **on a partial sweep** and at 3 once the sweep reaches 360 (they drive the revolve
  generator), `create_torus` `majorSegments` at 3 (its sweep is always a full turn),
  `create_sphere` `subdivisions` at 2, the stair verbs' `numSteps` at 1, plane subdivisions
  at 0. Each verb's parameter page states its own.
- **A revolve's floor follows its sweep angle: 2 below 360, 3 from 360 up.** The engine's own
  floor is 2 everywhere (`Steps = Max(Steps, 2)` in the revolve generator) and it is correct for a
  partial sweep — `create_arch majorSteps: 2` is a sound half-arch and is left alone. On a
  **closed** revolution 2 produces a mesh that cannot close, so it clamps to 3 and warns like any
  other clamp. What 2 used to build, and what the clamp now prevents: `create_torus
  majorSegments: 2` gave **16 triangles with 16 boundary edges** and `isClosed: false`; `revolve
  steps: 2` at `angle: 360` gave **8 triangles, 10 boundary edges and 6 degenerate triangles**.
  Both answered `success: true` with **no clamp warning** — nothing was clamped, 2 was in range —
  so the only diagnostic was the downstream open-mesh report (`geometry.check_health`, or
  `PWMODEL_MESH_NOT_CLOSED` on a `.pwmodel` merge), which names the symptom rather than the
  parameter. **Three is the first value that closes either** (torus 3 → 48 triangles closed;
  `revolve steps: 3` at 360 → 24 triangles closed).
- Where a verb **echoes** a count it also clamps — `create_stairs` / `create_spiral_stairs`
  `numSteps`, `revolve` `steps` and `profilePoints`, `create_box` `width`/`height`/`depth` —
  the echo is the **effective** value, not your request. Read the echo, not what you sent.
- **Not every out-of-range value clamps.** `create_pipe` requires `0 < innerRadius < outerRadius`
  and **fails** `INVALID_PARAMS` when it does not hold, with no `warnings` entry and no mesh:
  `innerRadius >= outerRadius` inverts the swept cross-section into a self-intersecting solid and
  `innerRadius <= 0` collapses its inner edge onto the revolve axis, and there is no non-arbitrary
  value to clamp either to. A clamp is only correct where one substitute is obviously the intended
  shape.

The same strings reach a `.pwmodel` compile as `PWMODEL_STAGE_WARNING` diagnostics, so a
recipe debugged through one surface reads identically through the other.

**A clamped domain is not a refused one, and the two must never be published as the same
thing.** A refused domain rejects a value outside it and names the bound in the error. A
clamped domain *accepts* a value outside it, moves it, and warns. Declaring a radial count's
clamped domain as an enforced range would turn every clamp above into a rejection and make
the `<= 0` means *unset* tier unwritable — a legal, useful value that is outside the clamped
domain by construction. So the domains stay advisory, and every clamped count's floor,
ceiling, default and unset behaviour is declared as structured metadata beside the ops that
apply it rather than only as prose. A test runs each generator at its floor, one below, at
its ceiling, one above and at zero, and fails if any published number disagrees with what
the op did — so these numbers are measured rather than remembered.

## Verbs that spawn an actor answer in one shape

Every `geometry.*` verb that creates an actor emits `class: "DynamicMeshActor"` plus the shared
verification block — `actorPath`, `mapPath`, `actorName`, `actorLabel`, `actorObjectName`,
`actorGuid`, `actorClass`, `existsAfter: true` — so one branch reads the whole family.

**Membership is the spawn, not the `create_` prefix.** The family is the 15 `create_*` verbs plus
`revolve`, `import_obj`, `import_stl`, `create_from_static_mesh` and `create_from_skeletal_mesh`.
The three spelled without the prefix were the last to answer without either half, and they were
missed precisely because a prefix is not a contract.

Read the actor back out of the response rather than reusing the label you sent: a requested label
that is already taken comes back suffixed, `actorObjectName` is the unique name to chain into the
`actorName` slot of later calls, and `existsAfter` is the only field that says the actor is really
there.

## `faceDirection` on extrude / inset / outset / offset_faces

`faceDirection` narrows the four face verbs to the triangles whose normal points within
`faceAngleTolerance` degrees (default 45) of the vector you pass. Three distinct outcomes, and
the middle one used to be indistinguishable from the first:

- **Omitted** — the whole mesh. The response reads `facesSelected: 0`, which is what an empty
  engine selection reports, and Geometry Script expands an empty selection to every triangle.
  Unchanged.
- **Given, and no face matches** — the verb does **NOTHING** and warns. It does **not** fall back
  to the whole mesh. The response reads `facesSelected: 0`, `changed: false` and
  `faceFilterMatchedNothing: true`, and the warning names the direction, the tolerance and the
  mesh's triangle count so a wrong axis is separable from an empty mesh. Previously a mistyped or
  unsatisfiable direction operated on the **entire** mesh with no diagnostic at all — plausible
  enough to ship, and it did.
- **Given, and faces match** — those faces only.

`faceFilterMatchedNothing` is present only when true, so a call that does not trip it is
byte-identical to before. `facesSelected: 0` therefore no longer means "the whole mesh" on its
own: it means that only when `faceFilterMatchedNothing` is absent.

**On an OPEN mesh a matching filter also warns**, and this one is not a bug that can be fixed —
it is what a face filter means. `extrude`/`offset_faces` close a region into a solid only when
the selection covers a *complete connected component*; a filter that leaves any face out leaves
that face flat with no side walls, and a filter that covers only part of a component gets a moved
sheet with a skirt rather than a closed shell. On an open two-sided sheet the unfiltered extrude
measured 24 triangles (closed) against 14 filtered. Drop `faceDirection` to close the whole sheet,
or use `geometry.shell` to thicken it. A **closed** mesh does not trip this — extruding one face
of a box is the intended use and stays closed.

## Auditing meshes you already shipped

`geometry.audit_static_meshes` sweeps a folder of **saved** `UStaticMesh` assets in one call and
reports ones that are inside out, wound inconsistently, open, degenerate, non-manifold, empty, or
mirrored by a negative Build Scale. Nothing is spawned and nothing is written.

```js
call({ path: "geometry.audit_static_meshes", args: { folder: "/Game/Meshes/Trees" } })
```

**Use it instead of looping `geometry.check_health`.** `check_health` answers for a live
`DynamicMeshActor`, so a saved-asset loop would place one actor per asset. That loop is mutating and
unbatched, and once wedged an editor for 168 minutes.

**Winding is the defect nothing else can see.** A closed shell wound inside out matches a correct one
on vertex count, triangle count, boundary edges, bowties and component count, and renders identically
because backface culling shows the camera whichever wall faces it. Only the sign of the enclosed
volume separates them — and recomputing normals *cements* the inversion rather than revealing it, so
a normals check passes on the broken mesh by construction.

**`pass` is stricter than "no errors"**: an unrunnable or truncated audit fails regardless of
`failOn`; an empty match set is also an **error** so a typo cannot look clean. The full pass rule is
in `geometry.audit_static_meshes` below.

## See also

- [`level-building`](level-building.md) — massing a level out of these primitives and driving it from re-runnable build scripts.
- [`level-review`](level-review.md) — checking the massing reads right before detailing it.
- [`skeleton`](skeleton.md) — editing an existing skeletal mesh: bones, sockets, morph targets, cloth, physics assets, and skin-weight **profiles**.

### geometry.auto_uv

Alias of `geometry.unwrap_uv`: the same XAtlas auto-unwrap with optional `uvChannel` (default 0).
`unwrap_uv` is canonical; `geometry.pack_uv_islands` instead repacks existing islands. See
`call("geometry.unwrap_uv")`.

### geometry.unwrap_uv

The canonical XAtlas auto-unwrap onto `uvChannel` (default 0); `geometry.auto_uv` is an alias.
It **replaces** that channel. To rearrange an existing layout, use
`geometry.pack_uv_islands`; see `call("geometry.pack_uv_islands")`.

### geometry.array_linear

Appends `count-1` offset copies to the source actor's dynamic mesh along a direction. The result is
**one merged dynamic mesh** on the original actor; it does **not** spawn new actors. See
`geometry.array_radial` for the same output contract.

### geometry.array_radial

Appends `count-1` rotated copies to the source actor's dynamic mesh around a center point. The result
is **one merged dynamic mesh** on the original actor; it does **not** spawn new actors.

For N separate, individually placeable copies, use `actor.duplicate` + `actor.set_transform`.

### geometry.create_box

`width`/`height`/`depth` map to local **X/Y/Z** respectively (`AppendBox(... Width,
Height, Depth ...)`, origin-centered). The natural reading that `height` is the
vertical dimension is **wrong**: `height` is the local-Y extent, and
the vertical extent is **`depth`** (local Z). A "400 wide x 500 tall x 80 deep" wall authored as
`{width:400, height:500, depth:80}` comes out a flat slab lying in X-Y (tall along Y,
only 80 thick along Z). To stand it up as an upright wall, pass
`{width:400, height:80, depth:500}` (footprint WxH, vertical = `depth`). The box is
built at identity and then takes your `rotation`, so this axis mapping is what you
rotate *from*.

`widthSegments`/`heightSegments`/`depthSegments` count **vertices along an edge, not quads**, and
the generator raises each to 2 before building (`N.A = FMath::Max(2, N.A)`) — so `0`, `1` and `2`
all produce the same unsubdivided 12-triangle box and `3` is the first value that adds a quad.
`0` is legal and means "no subdivision"; a negative value clamps to `0` and warns. Pass the count
you want plus one when porting a recipe written against a quad count.

### geometry.create_arch

The arch is a partial torus built with `AppendTorus`, which revolves in the local
**X-Y plane about Z** — so by default the arch lies **flat** (the opening faces up,
not forward). To stand it apex-up as a doorway, rotate it: `rotation.roll = -90`
stands it apex-up, and `rotation.roll = +90` points the apex **down** (below the
wall). The opening spans the `angle` sweep (default 180 deg = a half-arch). Nothing
about the build plane is derivable from `majorRadius`/`minorRadius`/`angle` alone, so
set the roll explicitly when you need it upright.

### geometry.create_pipe

**A closed solid, built as one revolve.** The annular cross-section — `innerRadius`..`outerRadius`
by `-height/2`..`+height/2` — is swept 360 degrees about local Z with `AppendRevolvePolygon`. There
is no boolean involved, and closure is a property of the construction rather than something a
boolean has to reconstruct: `FRevolvePlanarPolygonGenerator` sets `bProfileCurveIsClosed` and, at
360 degrees, `bSweepCurveIsClosed` (`RevolveGenerator.cpp:341`, `:370`). It used to be an uncapped
outer cylinder minus an inner one, which formed no annular end caps at all — two open walls,
4 triangles per radial step where a closed annulus needs 8 — and it rendered see-through with no
diagnostic.

**Exact triangle count: `2 * radialSteps * (2 * heightSteps + 4)`.** At the defaults
(`radialSteps` 24, `heightSteps` 1) that is 288; at `heightSteps: 0` it is exactly 8 per radial
step. `heightSteps` keeps `create_cylinder`'s meaning — **additional** wall loops, not the total
(`SweepGenerator.cpp:547`, `FCylinderGenerator::LengthSamples`) — so a pipe and a cylinder at the
same `heightSteps` carry the same number of loops up the wall.

**Centre-placed, and it was not.** `height: 100` spans local z `-50..+50`, exactly as
`create_cylinder height: 100` does. Until this was fixed it was the family's one **base**-placed
primitive (z `0..100`), which belonged to no rule — every other origin-capable generator here is
`Center` and `create_ramp`'s `Base` is on its extrusion axis, a different construction. **A recipe
written against the old placement now sits half a height too low; raise `location.z` by
`height/2`.** The old placement also mis-covered `twist` / `taper` / `bend`, whose `extent` is
symmetric about the mesh **origin**: a base-placed pipe got the deform on its lower half only.

**`0 < innerRadius < outerRadius` is required, not clamped.** Violating it fails `INVALID_PARAMS`
and creates nothing. `innerRadius >= outerRadius` turns the swept rectangle inside out into a
self-intersecting solid; `innerRadius <= 0` collapses its inner edge onto the revolve axis as a ring
of zero-area triangles. There is no non-arbitrary value to clamp to — every substitute is a
different pipe from the one asked for. Both were previously accepted in silence and returned an
empty or single-walled mesh.

**Exactly four polygroups** — outer wall, bore, top rim, bottom rim — assigned by PinWright after
the sweep, classified by face normal, so the count holds at any `radialSteps` / `heightSteps`. This
is deliberate: left to the engine, a revolved primitive arrives with one polygroup per **quad**,
which is the shape that makes `geometry.bevel` notch every interior quad boundary. See
`call("geometry.bevel")`.

### geometry.bevel

**The mesh's polygroups ARE the edge selection.** `ApplyMeshPolygroupBevel` walks polygroup edges
and bevels every one of them — no angle filter, no silhouette filter, no way to narrow it — so the
result is decided by whatever grouping the generator left behind. Both extremes fail, and both warn:

| Grouping the mesh arrives with | What `bevel` does |
|---|---|
| None | Nothing. The call succeeds and returns the mesh unchanged. |
| Coarse (one group per face, wall or cap) | The intended chamfer, on the edges between those faces. |
| One group per **quad** | Chamfers **every interior quad boundary** — a grid of notches that reads as surface damage, at roughly 3x the triangle cost. One example model spent 30,000 triangles producing an artifact. |

The per-quad warning fires at **>= 32 distinct polygroups and `groups * 3 >= triangles`**. The
absolute floor of 32 is what separates a dense revolved surface from a default `create_box` — 6
groups over 12 triangles, the same 1:2 ratio, and a mesh anyone should bevel.

**Per-quad grouping is what the revolved primitives hand you**, not a shape you have to construct.
`create_torus`, `create_arch` and `revolve` all go through `FBaseRevolveGenerator`, and
`AppendTorus` / `AppendRevolvePolygon` / `AppendRevolvePath` never map
`FGeometryScriptPrimitiveOptions::PolygroupMode` onto `FBaseRevolveGenerator::PolygonGroupingMode`,
which defaults to `EProfileSweepPolygonGrouping::PerFace` (`SweepGenerator.h:308`) — and for a sweep
"PerFace" means one group per **quad**: `PolygonId = SweepIndex * NumProfileSegments + ProfileIndex`
(`SweepGenerator.cpp:764`). Worked example: `create_torus majorSegments: 16, minorSegments: 8` is
**128 polygroups over 256 triangles**. Nothing on this side can turn it off — the only post-pass the
engine applies is the `SingleGroup` collapse, which removes *all* polygroups rather than coarsening
them.

The grid, sweep and stair generators are unaffected: `create_box` gets one group per box face,
`create_cylinder` / `create_cone` get side-wall-plus-caps, `create_capsule` and the stair verbs a
comparably coarse set, and `create_plane` / `create_disc` / `create_ring` a single group for the
whole surface — because those generators read the enum only to detect `PerQuad` and PinWright never
passes it. `create_pipe` is kept out of the trap on purpose: PinWright assigns it four polygroups
after the sweep.

**What to do instead of bevelling a revolved primitive:** bevel a box or a boolean result, regroup
the mesh first, or accept the shape as generated. `bevel` also refuses any mesh with no UV layer or
no normal layer at all (`NO_UV_ELEMENTS`) — a crash guard, separate from this, see
`call("geometry.append_buffers")`.

### geometry.get_mesh_info

Reports `vertexCount` / `triangleCount`, the feature flags (`hasNormals`,
`hasUVs`, `hasColors`, `hasPolygroups`), and a `boundingBox` object describing
the dynamic mesh's **mesh-LOCAL** extent: `{min, max, origin, extent}`, each a
`{x,y,z}` in mesh-local space (`extent` is the half-size, so a 100-wide centered
box reads `extent.x ≈ 50`). Use this to verify an op landed at the expected size
— e.g. that a `boolean_intersection` clipped a shape to a box's half-extents, or
that a `bend` / `taper` changed the silhouette — in the same call that confirms
the vertex/triangle counts, with no separate `actor.get_bounding_box`
round-trip. Note the distinction: this `boundingBox` is mesh-LOCAL (the actor's
location / rotation / scale are **not** applied), whereas `actor.get_bounding_box`
is world-space; the two coincide only at an identity actor transform.

### geometry.get_vertex_position

Returns the vertex's **mesh-LOCAL** coordinates — the actor's world location /
rotation / scale are **not** applied (the handler reads `GetVertexPosition` straight
off the dynamic mesh). So moving the actor with `actor.set_transform` does **not**
change what this verb reports for a given `vertexIndex`. To reason about world-space
overlap of two actors (the prerequisite for a `boolean_subtract` / `boolean_union`),
either combine each local position with that actor's `actor.describe` transform, or
read `actor.get_bounding_box`, which is already world-space.

### geometry.twist

`angle` is the total twist in degrees. `extent` is measured in **world units** (the mesh's
`height`/bounding-box units, **not** a 0..1 fraction). `symmetricExtents` defaults **true**:
it is a symmetric half-extent about the deform origin spanning `[-extent, +extent]`, so a
centered height-H mesh needs `extent ≈ H/2` (a 300-tall column needs 150, not 300). With
`symmetricExtents: false`, the span is `[-lowerExtent, +extent]` and `lowerExtent` (default 10)
is read. The twist is `bBidirectional` about `center` and twists about `axis`; both default to
the local origin and Z. See the `## Warp deformers` section on `call("geometry")` for shared rules.

### geometry.taper

`flareX` / `flareY` are per-axis percentages (default 50); `extent` is measured in **world units**,
**not a percentage**. It defaults to a symmetric half-extent with
`symmetricExtents: true`, spanning `[-extent, +extent]` and covering a centered height-H mesh
at `extent ≈ H/2`. With `symmetricExtents: false`, the span is `[-lowerExtent, +extent]` and
`lowerExtent` (default 10) is read. See the `## Warp deformers` section on `call("geometry")`.

`flareX` / `flareY` are per **perpendicular** axis of the warp frame, taken cyclically
from `axis`: at the default `axis: 'z'` they are world X and world Y, at `axis: 'x'`
they are Y and Z. Tapering a form along its own length is therefore `axis` plus a
`center` on that form's own axis line — not a rotate-taper-unrotate round trip.

### geometry.bend

`angle` is the total bend in degrees. `extent` is measured in **world units** (the mesh's
`height`/bounding-box units, **not** a 0..1 fraction). With `symmetricExtents: true` (default),
it is a symmetric half-extent spanning `[-extent, +extent]`; a centered height-H mesh needs
`extent ≈ H/2`. With `symmetricExtents: false`, the span is `[-lowerExtent, +extent]` and
`lowerExtent` (default 10) is read. The bend is `bBidirectional` about `center` and measures
the span along `axis`; see the `## Warp deformers` section on `call("geometry")`.

### geometry.noise_deform

`magnitude` is read according to `magnitudeMode`, and the two readings are different
quantities rather than two scales of one.

**`magnitudeMode: 'absolute'` (default)** — `magnitude` is a displacement in world
units, the same everywhere on the mesh. This is the engine call unchanged, so every
result produced before the mode existed is reproduced exactly. Its limit is a mesh whose
parts differ in size: one pass sized for a thick form erases a thin one, and one sized
for the thin form leaves the thick one smooth, because a displacement that reads as
surface detail on a 40-unit form is most of the radius of a 2-unit one. The workaround
was to split the mesh and noise each piece separately.

**`magnitudeMode: 'relative'`** — `magnitude` is a
**fraction of each vertex's mean one-ring edge length**,
so one pass follows local feature size across the whole mesh.
`magnitude: 0.5` displaces every vertex by half the average length of the edges meeting
at it, whatever that length is there. It reads the same noise field as absolute (same
`seed`, `frequency`, `frequencyShift`) and the same per-vertex normals, so only the scale
differs and a mesh can be switched between the modes without the pattern moving.

Three things to know before choosing it:

- **It is a proxy for feature size, not a measurement of it.** The proxy holds while the
  tessellation tracks the form — a revolve or a sweep gives fine edges on a thin section
  and coarse ones on a thick section. It does **not** hold on a mesh uniformly remeshed
  to one target edge length, where every vertex reports the same number and `relative`
  degenerates into `absolute` with a rescaled magnitude. Measure the mesh's edge-length
  spread before assuming — a mesh that came out of `geometry.remesh_uniform` is exactly
  the case it does not help.
- **A boundary vertex's one-ring is an open fan.** Its mean is over the edges it actually
  has — a smaller sample of the same quantity, neither extrapolated across the hole nor
  excluded — so an open mesh noises to its edge.
- **A split / duplicated vertex gets one mean per vertex.** Two vertices at the same
  position with disjoint one-rings (a hard seam, or unwelded imported geometry) scale by
  their own edges only, so if the two sides are tessellated differently the seam
  **opens**. `geometry.merge_vertices` first, or stay on `absolute`, which does not have
  this failure. That is inherent to any per-vertex local measure.

`relative` requires `applyAlongNormal: true` and is refused with `INVALID_ARGUMENT`
otherwise: the vector-displacement form rides three decorrelated noise fields whose
offsets have no public engine entry point, and guessing at them would drift silently on
an engine upgrade. A vertex with no incident edges has no one-ring, so it is left where
it is and reported in `warnings`. Neither mode recomputes normals — follow either with a
normals op.

### geometry.spherify

Lerps every vertex toward the sphere of radius `GetExtent().GetMax()` — **the largest
half-extent, not the bounding sphere**. For a 150×118×92 box that is 75, while the bounding
sphere is 105.93, so the target sphere touches the two most distant faces and leaves the
corners *outside* it: corners are pulled **in**, the flat centres of the short faces are pushed
**out**. `factor` is the lerp weight, clamped to 0–1 with a warning.

**The aspect ratio of what you feed it is the thing to get right, not the factor.** The map is
purely radial and radially monotone, so it cannot fold a surface — measured on the exact
`FGridBoxMeshGenerator` tessellation, boxes from 100³ to 400×40×40 (10:1) spherified at
`factor=1.0` come back with **0 inverted triangles, 0 degenerate triangles and 0
self-intersecting triangle pairs**. What an oblong box *does* get is a skewed distribution of
triangle sizes, because the scale applied at the flattest face centre is
`(1-factor) + factor·(Emax/Emin)` while the longest face does not move at all. A 150×118×92 box
at `factor=0.82` goes in with an edge-length ratio of 1.64:1 and comes out at 2.81:1, shortest
edge 21.3 uu.

That is what produces the black cavity authors report: it is the **next** op folding. Normal
displacement (`noise_deform`, `displace`, the warp deformers) self-intersects wherever its
magnitude approaches the local edge length, and a first noise octave of magnitude 12 across a
21 uu edge is up to 24 uu of relative displacement.

The op warns when `factor·(Emax/Emin − 1)` exceeds **1/3**, naming the bounding box, the factor,
the resulting radial scales and the remedy. Measured: 150×118×92 @ 0.82 → 0.517 (warns),
@ 0.72 → 0.454 (warns), 132×118×104 @ 0.70 → 0.189 (silent), any cube at any factor → 0
(silent). Remedies, in order of preference: near-cubic source geometry; a lower `factor`; a
`remesh_uniform` between `spherify` and any displacement op, which re-uniformises exactly the
edge lengths `spherify` skewed.

Note the warning is deliberately **not** driven by the raw radial scale spread: a perfect cube
at `factor=1.0` spreads by √3 = 1.73, higher than the 1.58 of a box that composites cleanly,
because corner-vs-face-centre spread is inherent to turning any box into a sphere.

### geometry.cylindrify

The same radial map as `geometry.spherify` measured perpendicular to `axis`, but fitted to the
**mean** perpendicular radius over the mesh's vertices rather than to a bounding quantity — two
sibling ops, two different conventions. The fitted value comes back as `avgRadius`. Vertices
whose perpendicular radius is 0 (anything sitting on the axis) are skipped and are not counted
in `verticesModified`. `factor` is clamped to 0–1 with a warning.

**`factor=1.0` — the default — is not injective.** At exactly 1.0 every vertex is placed at
perpendicular radius `avgRadius`, so any two vertices sharing an angular direction but not a
radius land on top of each other. On a box the two cap faces perpendicular to `axis` guarantee
it: measured on a 100³ cube at `segments=(5,5,4)`, `factor=0.82` gives 0 degenerate triangles
while `factor=1.0` gives **24 degenerate triangles, 28 inverted triangles, 144 self-intersecting
triangle pairs and a zero-length edge**. Pass anything below 1.0 unless the input is a tube with
no geometry near the axis. This is a separate mechanism from the anisotropy warning below and is
not yet diagnosed at runtime.

The anisotropy warning is measured on the **cross-section**, so the `axis` dimension does not
enter it — a tall column with a square cross-section is isotropic under this map however tall it
is. Same 1/3 threshold and same remedies as `geometry.spherify`; at `factor=1` the fitted radius
cancels out of the metric, which is why one threshold serves both ops.

### geometry.append_buffers

Bulk-append vertices + indexed triangles (with optional per-vertex normals/UVs/colors) to a `DynamicMeshActor` in one call, instead of per-element `append_vertex` / `append_triangle` round-trips.

```js
call({
  path: "geometry.append_buffers",
  args: {
    actorName: "ProcMesh_1",
    vertices: [[0,0,0],[100,0,0],[100,100,0],[0,100,0]],
    triangles: [[0,1,2],[0,2,3]]
  }
})
```

Gotchas: returns `{appendedVertices, appendedTriangles, totalVertices, totalTriangles}` — the appended counts are an honest delta, so it works on a seed actor that already carries geometry. Any triangle index outside `[0, vertexCount)` **rejects the whole call**. Winding is CCW-faces-outward; positions are LOCAL to the actor (the actor's own transform is applied on top). Omitting `uvs` on a mesh that already carries UVs does **not** wipe them: the engine's append sets the target's UV layer count to exactly what the incoming buffers carry, so PinWright pads the buffers with placeholder `(0,0)` UVs to keep the existing layers. The appended geometry then has UVs that are present but meaningless — pass `uvs` when it needs real ones. On a mesh whose UV layers are empty there is nothing to preserve and no padding happens, which is why `append_buffers` with no `uvs=` still leaves a hand-built mesh with no UV layer at all (the state `shell` and `bevel` refuse).

### geometry.export_obj

Export a `DynamicMeshActor`'s mesh as Wavefront OBJ text (`v`/`vt`/`vn`/`f`).

```js
call({ path: "geometry.export_obj", args: { actorName: "ProcMesh_1", returnText: true } })
```

Gotchas: returns `{path, vertexCount, triangleCount, replaced, text?}`. `overwrite` defaults to false. An existing output is refused with `ALREADY_EXISTS` before OBJ serialization begins, and the publisher repeats that fail-if-exists check to close the check/write race. Opted-in replacement is staged and published atomically: a write or publish failure returns `WRITE_FAILED` without changing the previous file. `replaced` is true only when that successful call replaced an existing file. Vertices are emitted in mesh-LOCAL space, and sparse `FDynamicMesh3` vertex IDs are **compacted** to dense 1-based OBJ indices.

### geometry.import_obj

Parse Wavefront OBJ (from `text` or `filePath`) into a NEW `DynamicMeshActor`.

```js
call({ path: "geometry.import_obj", args: { actorName: "Imported_1", filePath: "/Saved/PinWright/Meshes/part.obj" } })
```

Gotchas: returns `{actorName, class, path, format, vertexCount, triangleCount, requestedTriangles, droppedTriangles}` plus the shared actor verification block. Polygons with more than 3 vertices are **fan-triangulated**; `vt`/`vn` are parsed but **not applied** (positions + topology only). Malformed input (no vertices/triangles) is `PARSE_FAILED`. The requested transform lives on the actor; imported coordinates are treated as mesh-local.

**A file the engine partly refuses now fails the import.** `AppendBuffersToMesh` drops a triangle it will not take — one that would create a **non-manifold edge**, or one that **duplicates** a triangle already added — and carries on with the rest. That is `IMPORT_FAILED`, quoting the engine's own wording and the count refused, and **no actor is spawned**. Before this it was a success response over an actor silently missing faces, and the echoed counts could not reveal it: the vertices of a refused triangle are appended before any triangle is validated and are never removed, so `vertexCount` still matched the file. Compare `droppedTriangles` against 0, or `triangleCount` against `requestedTriangles`, to assert nothing was lost.

`allowPartial: true` imports what the engine accepted and returns success — with `droppedTriangles` set and a `warnings` entry carrying the engine's message. Use it for real-world scan/CAD exports where a few bad faces are expected; do not use it to silence a file you have not looked at.

### geometry.export_stl

Export a `DynamicMeshActor`'s mesh as STL (ASCII by default, or binary).

```js
call({ path: "geometry.export_stl", args: { actorName: "ProcMesh_1", binary: true } })
```

Gotchas: returns `{path, format, vertexCount, triangleCount, replaced, text?}`. `overwrite` defaults to false. An existing output is refused with `ALREADY_EXISTS` before ASCII or binary STL serialization begins, and the publisher repeats that fail-if-exists check to close the check/write race. OBJ, ASCII STL and binary STL use the same staged atomic publication, so `WRITE_FAILED` leaves the previous file unchanged; `replaced` is true only when that successful call replaced one. Binary STL is **never inlined** (it would need base64); only ASCII rides back in `text`. `vertexCount` is the shared-mesh vertex count, not `3 x triangleCount`.

### geometry.import_stl

Parse STL into a NEW `DynamicMeshActor`; binary/ASCII is auto-detected.

```js
call({ path: "geometry.import_stl", args: { actorName: "Imported_2", filePath: "/Saved/PinWright/Meshes/part.stl" } })
```

Gotchas: returns the same shape as `import_obj` — `{actorName, class, path, format, vertexCount, triangleCount, requestedTriangles, droppedTriangles}` plus the shared actor verification block, and the same `IMPORT_FAILED` / `allowPartial` behaviour on triangles the engine refuses. Binary STL must come from `filePath` (inline `text` can only be ASCII). STL has no shared-vertex indices, so there is **no welding** — each triangle emits 3 fresh vertices and `vertexCount == 3 x triangleCount`. Run `geometry.check_health` after import if watertightness matters (unwelded meshes read as open).

### geometry.measure

Read-only measurement of a dynamic mesh: bounding box, dimensions, volume, area, and topology counts.

```js
call({ path: "geometry.measure", args: { actorName: "ProcMesh_1", space: "world" } })
```

Gotchas: returns `{bbox:{min,max,center,size}, volume, area, vertexCount, triangleCount, edgeCount, space, units}`. `bbox.size` is the **full extents** (`max - min` = 2 × half-extent). `volume`/`area` are always in mesh-local units regardless of `space`. An empty mesh returns a zeroed bbox rather than a non-finite one. **`volume` is SIGNED and the name does not say so** — it is the divergence-theorem integral (`TMeshQueries::GetVolumeArea`, no `abs()` in the accumulation), so a closed mesh wound inside out reports a *negative* volume, and an open mesh reports the integral of an unclosed surface, which is not a volume at all. It is also a `float`, so it loses precision on a large mesh. Use `geometry.check_health`'s `signedVolume` / `inverted` when the question is winding: those come with the `isClosed` that qualifies them.

### geometry.check_health

Read-only defect report for a dynamic mesh.

```js
call({ path: "geometry.check_health", args: { actorName: "ProcMesh_1" } })
call({ path: "geometry.check_health", args: { actorName: "ProcMesh_1", checkSelfIntersection: true } })
```

Gotchas: returns `{isClosed, boundaryEdges, degenerateTriangles, nonManifoldVertices, componentCount, vertexCount, triangleCount, unreferencedVertices, signedVolume, orientationConsistent, inconsistentEdges, inverted, selfIntersectionsMeasured, healthy}`, plus `{selfIntersections, selfIntersectingComponents, selfIntersectionsTruncated}` when `selfIntersectionsMeasured` is true. `healthy` = closed + no degenerate triangles + no bowtie vertices + orientation consistent + not inverted. Non-manifold **edges** are impossible in `FDynamicMesh3` (an edge holds at most 2 triangles), so only bowtie **vertices** are reported. `boundaryEdges > 0` on a mesh you expected to be solid usually means **unwelded** connectivity (e.g. a fresh `import_stl`), not a modeling hole.

**Every field the `health` block of `model.compile` / `model.validate` publishes is here too**, meaning the same measurement — both surfaces serialize one `FMeshHealth` from one walk, so they cannot answer differently about the same mesh. This verb adds `vertexCount`, `triangleCount`, `inverted`, `selfIntersectionsMeasured` and the `healthy` verdict on top.

**`unreferencedVertices` counts allocated vertices no triangle names.** `FDynamicMesh3` keeps a vertex live after the last triangle using it is removed, so a cut or a boolean can strand one: it is inside `vertexCount` and is part of no surface. That makes it the only field reconciling `vertexCount` with any triangle-reduced measurement of the same mesh (`geometry.measure`'s bbox, the `.pwmodel` `bounds`), which otherwise disagree in silence. Non-zero is also a signal on its own — an op removed geometry and left the leftovers behind. **It is in no verdict:** the static-mesh build is driven by triangles, so an orphan reaches no asset, and failing a mesh on it would refuse correct models.

**`checkSelfIntersection` is the third gate term, and it is opt-in.** Nothing above can see a surface that passes through **itself**, and both shapes of that fault walk straight through `isClosed && signedVolume > 0`: a membrane spanning a bore is two oppositely wound fans whose volume contributions cancel *exactly*, and a sweep whose walls were pushed through each other degrades `signedVolume` smoothly with no threshold on it. Pass `checkSelfIntersection: true` and gate on `isClosed && signedVolume > 0 && selfIntersections === 0`. It is off by default because it builds an AABB tree per connected component and descends it against itself — a different cost class from the allocation-free `O(E + T + V)` walk every other field comes from. **Read `selfIntersectionsMeasured` before the count:** it is `false` when you did not ask *and* when the measurement declined (empty mesh, or above its 200k-triangle budget), and the three count fields are **absent rather than zero** in both cases, so a missing measurement can never be mistaken for a clean one. `selfIntersectionsTruncated` says the count is a floor rather than a total; the verdict is unaffected, since a floor above zero is still above zero. Crossings are counted **inside one edge-connected component only** — separate shells that interpenetrate are the ordinary way a model is assembled here and stay at zero.

**`signedVolume` is the only field that can see an inside-out mesh, and it means something only when `isClosed`.** A closed mesh whose winding is uniformly reversed renders *identically* to a correct one — backface culling shows you whichever wall faces the camera, and the two walls carry opposite normals — so every other field here matches a correct mesh exactly: closed, `boundaryEdges: 0`, no bowties, same counts. `signedVolume` is the divergence-theorem volume in the engine's own facing-normal convention, so it is **positive** when the facing normals point outward and **negative** on the same shell wound inside out. Gate on `isClosed && signedVolume > 0`; on an open mesh the number is the integral of an unclosed surface and means nothing. `inverted` is that gate pre-evaluated (`isClosed && signedVolume < 0`), which is also why it stays `false` on an open mesh — open is not inside out, it is open. **This is not cosmetic:** the mesh distance field decides inside from outside by counting backface hits (`MeshDistanceFieldUtilities.cpp:261-281`), so an inverted shell inverts its field and Lumen / DFAO light the part as though the camera were inside it — invisible in any preview capture, wrong in a lit level.

**`orientationConsistent` is a second, independent signal, not a summary of the first.** It is false when some interior edge's two triangles traverse it in the *same* direction rather than opposite ones — a *partly* inverted surface, which is the nastier fault because no single sign describes it. A **uniform** inversion leaves every neighbour pair agreeing, so `orientationConsistent` is `true` on exactly the shell `signedVolume` catches. `inconsistentEdges` is the count behind it. There is no read-only engine helper for this: the one orientation utility, `FMeshRepairOrientation`, only *repairs*, which would destroy the evidence and change the mesh.

### geometry.generate_complex_collision

Generate convex-decomposition simple collision for a `DynamicMeshActor` without changing its
render mesh.

```js
call({ path: "geometry.generate_complex_collision",
       args: { actorName: "ProcMesh_1", maxHullCount: 8 } })
```

Gotchas: returns `{actorName, actorPath, actorObjectName, hullCount, shapeCount, collisionType,
requestedMaxHullCount, effectiveMaxHullCount, clamped}` plus `limit` and a `warnings` array when
`maxHullCount` is outside its inclusive `1..64` range. `hullCount` is measured from the built
component collision (`BodySetup.AggGeom.ConvexElems`), not echoed from the request; decomposition
can produce fewer convex hulls than the budget. `shapeCount` counts every generated simple shape,
including boxes, spheres and capsules, so it can differ from `hullCount`. `effectiveMaxHullCount`
is the clamped budget passed to Geometry Script, while `requestedMaxHullCount` preserves the input.
`clamped` is always present; `limit` and the warning are emitted only when the request changed.

### geometry.create_from_static_mesh

Load an existing `UStaticMesh` **asset** (or a placed actor's mesh) into a new editable
`DynamicMeshActor`.

Gotchas: returns `{actorName, actorPath, actorObjectName, class, assetPath, sourceActor?, sourceActorPath?, sourceActorObjectName?, reused, lodType, lodIndex, vertexCount, triangleCount, hasNormals, hasUVs, hasColors, hasPolygroups, materialSlots, materials:[{index,slot,path}]}` — the mesh-feature keys match `geometry.get_mesh_info`, so a load verifies itself in one call.

- **`reuseExisting` defaults to `true`, unlike every `geometry.create_*` verb.** Those always spawn; its requested create `name` is matched by exact display label only, so an unrelated actor's object path or internal name cannot capture the reuse lookup. Duplicate actor labels make it **refuse** with `AMBIGUOUS_ACTOR_NAME`, listing each candidate's unique object name. A re-run reloads the same actor and reports `reused: true`; pass `reuseExisting: false` or distinct `name`s for independent editable copies.
- **The mesh is LOCAL-space.** The source actor's transform is copied onto the new actor, not baked into the vertices — the same convention every primitive verb follows.
- **ISM/HISM per-instance transforms are not baked.** `sourceActor` reads the component's mesh asset once, at the component's own transform.
- `lodIndex` is **silently clamped** by the engine to the asset's available LOD count, so the echoed `lodIndex` is what you asked for, not necessarily what was read. Compare `triangleCount` against `static_mesh.describe` if it matters.
- `lodType` is a **string enum**: `MaxAvailable` (default) | `HiResSourceModel` | `SourceModel` | `RenderData`. `SourceModel` / `HiResSourceModel` / `MaxAvailable` read the editable MeshDescription and are unaffected by Nanite. `RenderData` reads the built render mesh, which is **split at UV seams and hard-normal creases** — expect a higher vertex count and unwelded-looking `check_health` output.
- Material IDs on the loaded mesh are LOD **section** indices, which coincide with the `materials[].index` **slot** indices only for the common one-section-per-slot asset. `static_mesh.describe` on the source is the authoritative slot map.
- A mesh over 500 000 triangles is `POLYGON_LIMIT_EXCEEDED` (the dynamic-mesh budget) — load a coarser LOD. Missing asset is `MESH_NOT_FOUND`; an actor with no static mesh is also `MESH_NOT_FOUND` (a missing actor is `ACTOR_NOT_FOUND`); zero or both sources is `INVALID_ARGUMENT`; an unknown `lodType` is `INVALID_PARAMS` listing the valid tokens.

### geometry.convert_to_static_mesh

Bake a `DynamicMeshActor`'s mesh into a `UStaticMesh` asset and force it to disk.

Gotchas: returns `{actorName, actorPath, actorObjectName, assetPath, exists, created, updated, overwrite, hiResWritten, materialsPreserved, materialSlots, package, sizeBytes, class, triangleCount, vertexCount, collisionElements, nanite, recomputeNormals, recomputeTangents}` + `{saveRequested, saved, pendingFlush, saveState, saveDetail}` (states table: [`safe-mutation-save`](safe-mutation-save.md)). `created` and `updated` are mutually exclusive — read them rather than inferring the branch from the request.

- **The create path bakes geometry only — materials are NOT carried** (follow with `static_mesh.set_material`). The `overwrite` path is the fix: it rewrites LOD0's geometry while **preserving** the target's material slots, section map, other LODs, and existing collision, so an iterate-and-re-bake loop keeps its materials. `materialsPreserved` reports which happened.
- **`overwrite` is in-place and destructive** — `asset.duplicate` first if unsure. Same discipline as `static_mesh.bake_transform`.
- `overwrite: true` on an asset that does not exist is `ASSET_NOT_FOUND` (omit the flag to create it).
- When the target has a valid HiRes (Nanite) source model, **both** the HiRes source and LOD0 are written and `hiResWritten` reports it. Writing LOD0 alone would leave the HiRes source stale and the mesh would revert on the next Nanite rebuild.
- A mesh authored through `append_buffers` with no UVs gets a deterministic box projection first — without it the MikkT tangent pass hard-crashes the editor on the async build worker.
- A `warnings` string array is added **only when the bake had something non-fatal to report** — no key at all otherwise. It carries what the response's other fields cannot: normal/tangent recompute disabled because the mesh carries no usable UV channel 0 (both branches report it), a lightmap index the build clamped to a channel the mesh does not have, a lightmap resolution the build did not store as requested, a lightmap channel requested with no resolution (the asset then keeps the `UStaticMesh` default of 4 and bakes the authored lightmap UVs at 4×4), and a material slot whose bound asset would not load. Read it: without it a bake with both recomputes silently off answers with the same clean success as one with them on.
- Collision generated on the source actor with `geometry.generate_collision` is carried over and counted in `collisionElements`; a source with no simple collision carries nothing and never wipes what the target already had.

### geometry.create_from_skeletal_mesh

Load an existing `USkeletalMesh` **asset** into a new editable `DynamicMeshActor`, carrying its
bones and **base** skin weights.

Gotchas: returns `{actorName, actorPath, actorObjectName, class, assetPath, reused, lodType, effectiveLodType, lodIndex, vertexCount, triangleCount, hasNormals, hasUVs, hasColors, hasBoneWeights, verticesWeighted, verticesUnweighted, fullyWeighted, boneCount, skeletonPath, materialSlots, materials:[{index,slot,path}]}`.

- **Read `effectiveLodType`, not `lodType`.** A SkeletalMesh has no HiRes source model, so the engine silently collapses `MaxAvailable` and `HiResSourceModel` to `SourceModel`. `lodType` echoes what you asked for; `effectiveLodType` is what was read.
- **`fullyWeighted: false` on load means the source asset itself has unweighted vertices** — not a bug in the copy. `geometry.convert_to_skeletal_mesh` will refuse to write it until you `geometry.bind_skin_weights`, so it is worth noticing here rather than several edits later.
- `RenderData` reads the built render mesh, which is **split at UV seams and hard-normal creases** — expect a higher vertex count and unwelded-looking `check_health` output. `SourceModel` reads the editable MeshDescription.
- `lodIndex` is **silently clamped** by the engine to the asset's source-model count, so the echoed `lodIndex` is what you asked for, not necessarily what was read.
- A mesh over 500 000 triangles is `POLYGON_LIMIT_EXCEEDED` (the dynamic-mesh budget) — load a coarser `lodIndex`. Missing asset is `MESH_NOT_FOUND`; an unknown `lodType` is `INVALID_PARAMS` listing the valid tokens.

### geometry.bind_skin_weights

Bind a `DynamicMeshActor`'s mesh to a skeleton, writing **base** skin weights for every vertex. One call performs copy-bones → create-weight-attribute → smooth-bind, so the ordering cannot be got wrong inside it.

Gotchas: returns `{actorName, actorPath, actorObjectName, skeletonPath, skeletonResolvedFrom, skeletonBoneCount, meshBoneCount, method, maxInfluences, stiffness, voxelResolution?, rebound, profile, vertexCount, triangleCount, hasBoneWeights, verticesWeighted, verticesUnweighted, fullyWeighted, warning?}`.

- **Run this LAST.** Vertices added after the bind get an entry in the weight attribute but with *empty* influences, so they are unskinned while still looking present. Re-run this verb after any further geometry edit; `rebound: true` confirms it replaced an existing binding.
- **`verticesUnweighted` is measured, not assumed.** The response reports a real per-vertex scan of the result, so `fullyWeighted: false` plus a `warning` is a successful call that produced an incomplete binding — the mesh really was mutated, and `geometry.convert_to_skeletal_mesh` is where it gets refused.
- **`DirectDistance` is the default and the exercised path.** `GeodesicVoxel` measures distance along the surface, which in principle avoids weighting a vertex from a bone that is near in space but far along the body; it is slower, allocates a voxel grid, and its quality depends on `voxelResolution`. Prefer `DirectDistance` unless you have measured otherwise.
- **This writes the BASE profile, always.** There is deliberately no `profile` argument: a named profile is the alternate-influence channel that `skeleton.normalize_weights` and friends write, and offering both here would re-import exactly that ambiguity. `profile` in the response echoes the base profile's name (`Default`) so it is visible which was written.
- Missing/unnamed skeleton is `SKELETON_NOT_FOUND`; a skeleton with no bones is `SKELETON_HAS_NO_BONES`; an empty mesh is `MESH_EMPTY` (build the geometry first); an unknown `method` is `INVALID_PARAMS`.

### geometry.convert_to_skeletal_mesh

Bake a `DynamicMeshActor` into a `USkeletalMesh` asset and force it to disk. This is the plugin's
only SkeletalMesh-creation verb.

Gotchas: returns `{actorName, actorPath, actorObjectName, assetPath, exists, created, updated, overwrite, package, sizeBytes, class, skeletonPath, skeletonResolvedFrom, lodIndex, vertexCount, triangleCount, hasBoneWeights, verticesWeighted, verticesUnweighted, fullyWeighted, verticesFarBone?, farBoneWorst?, allowPartialSkinning, recomputeNormals, recomputeTangents, materialsPreserved, boneMismatchHandling?, materialSlots}` + `{saveRequested, saved, pendingFlush}`. `created` and `updated` are mutually exclusive.

- **A partly-unskinned mesh is refused, and that is the point.** `NO_SKIN_WEIGHTS` when the mesh was never bound, `SKIN_WEIGHTS_INCOMPLETE` when the binding predates the current geometry — run `geometry.bind_skin_weights` and try again. Neither engine entry point catches this: the create path reports it only into a debug object this plugin discards, and the overwrite path does not check at all, committing zero-filled weights and reporting success. `allowPartialSkinning: true` opts out; `verticesUnweighted` is reported either way.
- **Creating onto an occupied `assetPath` is `ASSET_EXISTS`.** Not caution: the engine's create path reuses the existing asset and empties its LOD models, materials, reference skeleton and physics asset **in place**, with no prompt. Pass `overwrite: true` to rewrite geometry while preserving those, or pick another path.
- **Every imported or built render section is backed by a real asset-wide material slot.** On the
  create path, the handler first scans the input dynamic mesh for its highest triangle material ID and
  preallocates slots through `MaterialN` before engine conversion. This ordering is load-bearing:
  with too few slots, the engine can clamp a sparse section ID during conversion, and no post-build
  repair can reconstruct the lost ID. After either branch writes the LODs, a second safety pass finds
  the highest `MaterialIndex` referenced across every imported LOD and every available render LOD,
  then fills null slots and pads through that index without compacting sparse IDs. Extra slots use
  the first existing material, or the engine surface default when the source has none. Persistence
  is refused with `ASSET_DATA_INVALID` if material coverage cannot be read or any referenced slot
  remains absent or null. `materialSlots` reports the repaired array. The `overwrite` path still
  preserves the target's existing slots where possible; `materialsPreserved` reports which branch ran.
- `overwrite: true` on an asset that does not exist is `ASSET_NOT_FOUND`.
- `boneMismatchHandling` only matters when the mesh's bone hierarchy differs from the target asset's reference skeleton. `DoNothing` is correct (and fastest) when the mesh was bound to that very skeleton. `RemapGeometryToReferenceSkeleton` re-binds the weights onto the asset's skeleton and falls back to binding everything to the root when there is no bone data at all. `CreateNewReferenceSkeleton` replaces the asset's reference skeleton with the mesh's and **drops its virtual bones**.
- A mesh with no UVs gets a deterministic box projection first — without it the MikkT tangent pass hard-crashes the editor on the async build worker, exactly as in `convert_to_static_mesh`.
- Neither engine call writes the `.uasset`, so this verb force-saves and reports `saved` from an on-disk file probe. `save: false` marks the package dirty only, for batching before one `editor.save_all`.

### geometry.boolean_subtract

Reads as the whole boolean family - `boolean_union`, `boolean_subtract`, `boolean_intersection`, `boolean_trim`.

- **A boolean the engine REFUSES is now an error, and it used to be a success.** `ApplyMeshBoolean` declines to build an empty result
  unless `allowEmptyResult` is set: an `boolean_intersection` of two actors that do not overlap, or a `boolean_subtract` whose tool
  encloses the target, produces nothing and comes back as `BOOLEAN_FAILED` carrying the engine's own text. Before this the verb
  answered `success: true` with `changed: false` and left the target untouched, so callers went on to bevel, collide and bake
  geometry that had never been cut. Pass `allowEmptyResult: true` if an empty mesh is the answer you want.
- **`changed: false` on a SUCCESS still means "the engine did nothing", and that is a different case.** Disjoint operands where the
  result is non-empty - a `boolean_subtract` with a tool nowhere near the target - is a genuine success over an unmodified mesh. The
  two are now distinguishable: refused is an error, no-effect is `changed: false`. Test `changed`, not just the absence of an error.
- `simplifyOutput` defaults **true** on all four verbs — the engine's own default. It collapses only the small coplanar
  triangles the **cut** created, at 0.1 degrees from coplanar, and cannot distort polygroups, UVs or normals. These verbs used
  to pin it false with no way to reach it, so this is a behaviour change: triangle counts after a boolean move. Pass
  `simplifyOutput: false` to reproduce the old triangulation exactly. `simplifyPlanarTolerance` is not published on these four
  because UE 5.8 ignores it for booleans — `ApplyMeshBoolean` reads `bSimplifyOutput` and never assigns the tolerance
  (`geometry.self_union` does publish it, because its engine call reads it).
- **Skinned targets get their bone weights repaired, and the response says how much.** See "Skinned characters" above: the engine hands vertices it invents the weights of vertices it deleted, so these verbs re-derive them from a pre-operation snapshot and report `skinWeights: {verticesTransferred, verticesUnresolved}`. The field appears only when the target carried skin weights.
- `keepTool` defaults **true**, and the destruction is gated on SUCCESS: a boolean that failed has committed nothing, so the tool
  actor survives and the call can be retried. This mattered only once `BOOLEAN_FAILED` became reachable.
- **`boolean_trim` and `geometry.self_union` can now answer `MEMORY_PRESSURE`.** Both dispatch the same class of allocation as the
  three symmetric booleans - `boolean_trim` IS `ApplyMeshBoolean`, and `self_union` resolves a mesh against itself - and both
  shipped without the pre-flight the other three have. The guard runs before either mesh is touched, so a caller that trips it
  keeps exactly the geometry it had.

### geometry.sweep

- **The cross-section lands in each spline frame's local Y-Z plane and the sweep advances along its local +X.** A frame whose
  local +X is PERPENDICULAR to the direction of travel therefore slides the section along inside its own plane and sweeps no
  volume: the span comes back as a flat slab. Nothing in the engine reports it, and the result is indistinguishable from a
  correct one by triangle count, `isClosed` or boundary edges - measured, a broken and a corrected form both answered
  `success: true`, `isClosed: true`, `boundaryEdges: 0` at the same 26 triangles. The verb now warns instead, twice and both
  opening with `path`: `path frame N sweeps inside its own cross-section plane ...`, and, when the sweep is the only geometry
  on the mesh, `path swept a flat result - the mesh has zero extent on X ...`. Neither refuses the call; there is no substitute
  path to guess.
- **Without a resolvable spline actor the verb sweeps a vertical line through the mesh's own bounding box**, and `steps` sizes
  that fallback path. `sweepStatus` says which branch ran. The cross-section is a CIRCLE sized from the bounding box unless the
  caller supplies a profile, which only the `.pwmodel` front-end can do.
- **That fallback used to be flat.** Its frames were rotations about world Z, which left each frame's local +X across the
  direction of travel - the same defect as above, on the one path the verb builds itself. It swept a zero-thickness ribbon at
  exactly the triangle count a solid tube has, so `trianglesAfter` proved nothing. The frames now point along the path; the
  triangle count is unchanged and the geometry encloses volume.

### geometry.extrude_along_spline

- **`cap` works, and the sweep is a closed loop only when the SPLINE is.** The op used to sweep every path as a loop, and
  `FGeneralizedCylinderGenerator` builds caps only when it is not one - so `cap` was unreachable at every path length (`cap`
  omitted and `cap: true` produced byte-identical meshes), `scaleStart` / `scaleEnd` were silently dropped with it, and an open
  spline came back as a tube running from its last sample back to its first. It now sweeps open unless the last sample returns
  within 0.01 uu of the first, which is exactly what sampling a closed-loop `USplineComponent` produces.
- **On a genuinely closed spline `cap`, `scaleStart` and `scaleEnd` still do nothing**, because a loop has no ends - and the verb
  says so in a `warnings` entry rather than leaving it to be discovered. The result is watertight without caps.
- It shares `geometry.sweep`'s two `path` warnings: the same section-plane rule applies, and a spline whose frames do not turn
  with it sweeps flat.

### geometry.audit_static_meshes

Sweeps **saved** `UStaticMesh` assets — a `folder` (recursive by default, optionally filtered by
`namePattern`) or an explicit `assets` list. Exactly one of the two; neither leaves nothing to audit,
and both would mean guessing. Read-only: nothing is spawned, loaded for writing, modified or saved.
Returns a **job ticket**, because every asset costs a package load — orientation is not in the asset
registry's tags.

**`assets` takes either path form**, the package path `/Game/Folder/SM_Name` or the object path
`/Game/Folder/SM_Name.SM_Name`; the shorter one is normalised to the longer before it is resolved.
The three outcomes are three different answers, deliberately:

| what you passed | what you get |
| --- | --- |
| not a content path at all (a bare name, a folder, a `.` in a folder component) | `INVALID_ARGUMENT` **before the sweep runs** — a bad argument is not a finding about a mesh |
| a well-formed path the registry has nothing at | an `unrunnable` row coded `ASSET_NOT_FOUND`, counted in `assetsUnmeasured` |
| a well-formed path that resolves | audited |

The middle row is still `unrunnable` and still makes `pass` false — the asset genuinely was not
measured, and silently dropping it is how a sweep reports clean over assets it never saw. What it is
**not** is `MESH_AUDIT_UNLOADABLE`: "there is nothing here" and "this mesh would not load" are
different facts and only one of them is about geometry.

**Checks.** `inverted` (error), `inconsistent_winding` (error), `empty` (error), `not_closed`,
`degenerate_triangles`, `non_manifold`, `mirrored_build_scale`, `z_fighting`, and
`floating_components` (warnings) are on by default; `thin_shell` is off and must be asked for.
`inverted` walks edge-connected triangle components,
not just the asset-level sum. Its finding measurements include `components[]` with each component's
`index`, `status`, `signedVolume`, `surfaceArea`, `volumeRatio`, `boundaryEdges`,
`degenerateTriangles` and triangle range, plus `answeredComponents`, `unknownComponents`,
`invertedComponents` and `cleanComponents`.
The whole-mesh `signedVolume` remains in the common measurements as context, but it is not the
verdict: equal correctly wound and inverted shells can cancel it to zero.

`floating_components` is a warning-only spatial-isolation check, not a disconnected-component
count. It builds a proximity graph over edge-connected components: component AABBs prune pairs
that cannot link, then exact triangle-to-triangle distance decides every remaining link. The
tolerance is `floatingToleranceFraction * boundingSphereRadius`, with the default exactly
`0.005 *` the model bounding-sphere radius; it is deliberately not a hard-coded project-unit
distance. The main island is anchored by the largest individual edge-connected component; an
island may contain several linked components, and `largestIslandTriangleCount` is the total for
that proximity island. The static response keeps the compatibility fields and also reports
`totalProximityIslands`, `nonMainProximityIslands`, and `floatingComponentRows`, so island totals
cannot be confused with component-row totals. Each floating-component row reports
`componentIndex`, stable `proximityIslandId`, `triangleCount`, `signedVolume`, `center`,
`nearestComponentIndex`, exact `nearestDistance`, `partIndices` when the caller is the model
compiler, and `suppressed`.

`z_fighting` looks for projected overlap between near-coplanar triangles from different connected
components. Its plane epsilon is `modelExtent * 0.00001`, while its grid cell size is derived
separately from a fixed 32-cell model resolution. AABB expansion catches pairs straddling a cell
boundary; full-span triangles use a coarse-grid fallback that caps every inspected coarse
reference before filtering or deduplication. Any dense-cell, inspected-reference, or candidate
limit hit is `unrunnable`, never clean. Exact candidates accept parallel and anti-parallel normals,
test both triangles against the opposite plane, and clip projected triangles independently of
winding. Measurements include actual broad-phase work, fallback reference inspections, total
overlap area, and ranked regions whose `overlapArea` is the unique projected union of their
accepted pair overlaps, so several shells covering the same patch are not pair-summed. Regions
retain unique triangle/component ids and bounded strongest-pair evidence. This remains a
geometric depth-buffer proxy, not a prediction for every camera, projection, material, or depth
format. Candidate generation is expected `O(n + candidates)`; the exact union is a bounded local
`O(p^2)` post-process for `p <= 512` accepted pair polygons in one region, and a denser region is
reported `unrunnable` instead of scanned without a time bound.

The component answerability threshold is `abs(signedVolume) / surfaceArea^1.5 <= minVolumeRatio`,
with the scale-free default `0.001` (the same existing threshold used by `thin_shell`; a cube is
about `0.068`). Open, degenerate, zero-area, non-finite and near-zero-volume components are
`unknown`, never clean. If any component is unknown, the selected `inverted` check is `unrunnable`, so the shared
audit pass rule fails while the response still tells the caller how many other components were
answered.

Two pairs are deliberately complementary:

- `inverted` and `inconsistent_winding` answer different questions. A *uniformly* inverted shell
  is perfectly self-consistent, so only the component volume sign sees it; a *partially* inverted
  shell can retain a positive component volume, so edge adjacency is the signal for that local
  disagreement.
- `mirrored_build_scale` names the **cause** — an odd number of negative Build Scale axes — that
  `inverted` can only report the effect of. Two negative axes are a rotation, not a mirror.

The numeric and visual checks are also not substitutes. The whole-mesh `signedVolume` figure is
blind to cancellation between multiple components, while `front_back_face` is blind in the
opposite direction: Nanite proxies do not honor `IsRichView`, so an inverted Nanite mesh renders
grey and can pass the pixel check. The settled convention is back faces GREEN and front faces
neutral GREY. Use the component `inverted` result and `front_back_face` together; neither alone is
sufficient for every mesh representation.

**An unknown check id is an error, not a skip.** A `checks` array that silently ran nothing looks
exactly like a folder of correct meshes. The response echoes every check with `selected`, plus its
`applicable` / `notApplicable` / `flagged` / `unrunnable` / `clean` buckets. Two identities hold for
each selected check, and they are what makes a silent stop detectable:

```
applicable + notApplicable          == summary.assetsExamined
flagged + unrunnable + clean        == applicable
```

**`status` on a finding row is load-bearing.** `unrunnable` means that check did **not** evaluate that
asset — an asset that would not load, or whose LOD would not copy — and it is never folded into
`clean`. An open component likewise makes `inverted` *unrunnable*, never clean: the selected check
has a component to inspect, but an open surface cannot answer solid winding. The response keeps
that component in `unknownComponents` and identifies the reason as `open`.

**The pass rule**, echoed in the response as `passRule`:

```
pass = no finding at or above failOn
       AND zero unrunnable checks
       AND the sweep was not truncated
```

The last two terms ignore `failOn` entirely. `truncated` is true when the page stopped short of the
match set (`offset + examined < total`) **or** when `findings[]` was clipped by `maxFindings`; the
per-check tallies stay exact either way, so a clipped page still reports honest counts.

**`lodType` matters more than it looks.** The default `MaxAvailable` reads the highest-detail *source*
mesh. `RenderData` reads the **built** mesh, which is split at every UV seam and hard-normal crease —
a perfectly closed authored mesh reads there as thousands of boundary edges, `not_closed` fires on
almost everything, and every closed-only check except `inverted` goes not-applicable. `inverted`
reports those open components as UNKNOWN/unrunnable. The response adds a caveat saying so.
`useBuildScale` defaults **true** so the sweep measures the mesh as it ships; reading with it off
measures the authored triangles instead, and says so in a second caveat.

**A defect is never an RPC error.** However bad the content is, the call succeeds with `pass: false`
and structured findings, so a caller can tell "your meshes are inverted" from "the call did not work".
The only errors are argument and scope refusals: an unknown check id, every check excluded, a bad
`folder`, an `offset` past the end, and an **empty match set** — zero matches is an error precisely so
a typo'd path cannot be read as a clean sweep.

### geometry.audit_skeletal_animation_floating

Evaluates a `USkeletalMesh` against the actual poses in one `UAnimSequence` and reports geometry
that separates while the sequence plays. This is read-only and schedules a job because it skins
every source-LOD vertex at each sampled frame; it does not assume that a component is rigid
or has one bone influence. A call without an `args` object is documentation mode. Executable
buffered JSON, especially `args.wait: false`, returns a small running ticket with `ticket_id`
for `system.job_status` polling. With `progressToken` and `Accept: text/event-stream`, an absent
or true `wait` intentionally blocks until the terminal full result. That SSE result is not the
ticket response; explicit `wait: false` opts out of streaming and keeps the ticket path. The
required inputs are `skeletalMeshPath` and `animationPath`, and the two assets must use the same
skeleton.

The default `sampleStride: 1` visits every timeline frame through the animation evaluator,
including interpolation between stored keys. A caller may request a larger stride; frame zero
and the final frame are always included. `frameCount` is the timeline span, so a complete scan
has `frameCount + 1` sampled frame positions. The `sampling` result reports `frameCount`,
`sampleStride`, `sampledFrameCount`, the exact `sampledFrameIds`, and `complete`.
A sparse scan has `complete: false`: it is evidence only for those listed frames, can miss a
separation between them, and cannot claim the first or worst frame outside the sampled set.

The evaluator follows the sequence's real pose path, including an additive sequence's base pose,
before skinning every source-LOD vertex. It does not treat stored key indices as poses. A missing
mesh or animation is returned as a structured `status: "unrunnable"` result with `pass: false`
rather than as an RPC argument failure. `allowFloatingComponents` accepts bind-pose component
indices for intentional exceptions; the rows and their measured distances remain visible and
carry `suppressed: true`.

The bind pose is measured first with the same AABB-pruned, exact triangle-distance graph and the
same `0.005 *` model bounding-sphere-radius default as `floating_components`; that bind radius
stays fixed while sampled poses move. The result keeps `bindIslandCount` and `bindFloatingCount`
for compatibility and also reports `totalBindProximityIslands`, `nonMainBindProximityIslands`,
`floatingComponentRows`, and `uniqueAnimationSeparatedProximityIslands`, alongside
`modelFloatingCount`, `animationFloatingCount`, `suppressedCount`, and `unsuppressedCount`.
`totalBindProximityIslands` is the total number of proximity islands, including the main island;
`nonMainBindProximityIslands` counts those outside it.
`bindFloatingCount` is the number of edge-connected component rows outside that main island, so
it can be greater than `bindIslandCount` when several rows form one floating island. Each row's
`firstSeparation` and `worstSeparation` are the exact triangle distance to the largest proximity
island, and `nearestComponentIndex` identifies a component in that island. Rows from one
proximity island share the same `proximityIslandId`; `uniqueAnimationSeparatedProximityIslands`
is calculated from those IDs rather than row count. A component already separated at bind pose is
model-owned, marked `alreadySeparatedAtBindPose`, and excluded from `animationFloatingCount`.
A rigidly bound component separating from a smooth-skinned body is EXPECTED and is the single
most common benign cause of a large `worstSeparation` when the body deforms away from it. The
distance is a 3D triangle distance to the largest proximity island, not a visible screen-space
gap; intervening geometry commonly fills it, so it cannot be converted to screen pixels. Findings
are warning-only for exactly this reason: a row prompts you to inspect the frame, never decides a
defect on its own. Use `firstSeparatedFrame` and `worstFrame` to choose which frame to capture.
Every component row reports `componentIndex`, `proximityIslandId`, `triangleCount`,
`signedVolume`, `center`, `nearestComponentIndex`, `firstSeparation`,
`firstSeparatedFrame`, `worstFrame`, `worstSeparation`,
`alreadySeparatedAtBindPose`, and `suppressed`. Findings are warning-only:
`failOn: "error"` shows them without making `pass` false, while `failOn: "any"` makes warnings fail
through the shared audit verdict.

### geometry.fill_holes

An attribute-enabled mesh does not need a pre-existing UV or normal layer. When it has a boundary to
fill, PinWright grows missing UV0 and primary-normal overlays before the engine call so the engine can
assign both to the new triangles safely. Existing higher UV layers are preserved. The preparation does not
invent assignments for existing triangles; run `geometry.project_uv` or `geometry.recalculate_normals`
afterwards when the whole mesh needs those attributes.

An attribute-free mesh stays attribute-free because the engine deliberately skips both writes in that
representation. If a required overlay cannot be created, the call stops before hole filling with
`NO_UV_ELEMENTS` or `INVALID_NORMAL_OVERLAY` instead of entering the unsafe engine path.

### geometry.bridge

Emits **one** triangle strip between the two boundary loops — the loops are stitched directly to each
other with no intermediate rings, at any loop size.

With fewer than two loops, bridge keeps its legacy hole-fill fallback. That branch uses the same
missing-overlay preparation as `geometry.fill_holes`, including on a one-loop UV-less mesh.

**There is no `subdivisions` parameter, deliberately.** One was accepted and echoed for a while
without ever reaching the geometry; it is now off the declared surface, so passing it is rejected with
`UNKNOWN_PARAMS` rather than confirmed back at you. Bridge is hand-rolled over per-triangle appends
rather than wrapping an engine sweep, so there is no option to route a subdivision count to; adding
real intermediate rings is a new feature (and ill-defined when the two loops carry different vertex
counts), not a repair. The response still reports `subdivisions: 1`, which is now a statement of what
the operation did, not an echo of what you asked for.

Need edge rings across the bridged span? Run `geometry.subdivide` afterwards — it is global, so budget
for the triangle count on the rest of the mesh too.

### geometry.pack_uv_islands

Repacks the islands already on `uvChannel` into the unit square. It **moves** islands; it does not
re-solve the mesh into new ones, so seams, projections and any `geometry.transform_uvs` you applied
all survive. This is the last step of a UV pass — `unwrap_uv` or `project_uv` first, then pack.

`textureResolution` (default 1024) is the positive resolution the packing gutter is sized
for, in pixels. It sizes the spacing left between islands, not anything that gets written: a low
value leaves a wide gutter and packs the islands smaller, a high value packs them tighter. Pass
the resolution of the texture you intend to bake to. The engine packer supports 2–16384; positive
values outside that range remain accepted for compatibility, are clamped, and return a warning plus
the effective `textureResolution`. Zero and negative values are refused.

**A channel with no islands is refused, not packed.** There is nothing to rearrange on an empty
layer, and creating one to report a successful pack of zero islands would be a lie the caller
cannot see. Seat a layout first.

Until recently this verb ran the same XAtlas auto-unwrap as `unwrap_uv` and echoed a
`textureResolution` no engine call received — so it destroyed the layout it was asked to tidy. If
you have a pipeline that worked around that by packing *before* projecting, undo the workaround.

### geometry.create_ramp

A wedge: a right-triangle cross-section of `length` x `height`, extruded `width` deep. All three
must be **positive** and are refused otherwise, including zero.

That refusal is not pedantry about defaults. The extrude is capped by Geometry Script's flat
triangulation, which validates only that it was handed three or more points and then caps by ear
clipping that, when it can find no ear, treats the current vertex as one anyway. A zero `height` or
`length` collapses the wedge onto a line: the call used to return a plausible triangle count, a
closed mesh, and a cap made of overlapping self-cancelling triangles, with nothing written to any
error channel. A negative value mirrors the wedge and reverses its winding, so the cap comes out
inside-out. Both now fail with the offending numbers named.

### geometry.set_vertex_color

`setAll: true` paints **every** triangle, including geometry appended after an earlier colour
write. That is worth stating because it was not always true: the colour overlay used to be seeded
once and never widened, so anything a later `extrude_along_spline`, `sweep`, `bevel`, `shell` or
boolean added carried no colour element and stayed at the overlay default — white — however many
`setAll` calls followed.

The response tells you when that happened. `colorElementsCreated` is the number of colour elements
that had to be created to cover triangles carrying none; it is non-zero exactly when the mesh grew
since the last colour write. `verticesModified` is the count of distinct vertices actually painted,
not the mesh's vertex count — a vertex no triangle references cannot hold a colour element and is
not claimed.

**Existing seams are preserved.** Only triangles with no colour assignment are given elements, and
a corner whose vertex already carries exactly one element reuses it rather than splitting the
vertex; an already-split vertex gets a fresh corner. Nothing rebuilds assignments that exist.

**`channels` decides which of R/G/B/A the write touches**, defaulting to `"rgba"` — so a caller who
does not pass it writes all four exactly as this verb always did. Any non-empty subset works, in
any order and any case: `"a"`, `"rgb"`, `"ar"`. The components the mask does not name keep the
value they already carried, which is what lets one mesh hold a per-part RGB tint *and* an alpha
mask; before the mask existed, whoever wrote the colour first owned all four, because setting alpha
meant re-sending an RGB the caller usually does not know per vertex.

Two consequences worth knowing. The `r`/`g`/`b`/`a` numbers in the response echo what you *passed*,
not what was *written* — read the response's own `channels` field (always re-ordered canonically to
r,g,b,a) for that. And a colour element that has to be *created* to cover a triangle carrying none
is born white, so under a narrow mask its unnamed components read 1.0: that is the overlay default,
not a measurement. A spelling the parser cannot read is refused with `INVALID_ARGUMENT` rather than
widened to all four — widening is exactly what would destroy the channels you meant to keep.

The same `channels` parameter is on the `.pwmodel` `set_vertex_color` op, with the same spelling
and the same "omitted means all four" default.

### geometry.bake_ambient_occlusion

Ray-casts the mesh **against itself** and writes the resulting occlusion term into vertex-colour
channels you choose. This is the only verb that computes a signal *from* the geometry rather than
moving geometry around, and it exists for one thing a tiling material cannot do: a mesh assembled
from interpenetrating parts has no contact shading at any junction, because a detail or height map
is a function of UV and therefore structurally cannot know where two solids meet.

**`occlusionRadius` is required and is the load-bearing parameter.** It is the maximum ray length,
in the mesh's **own local units** — the space its vertices are in, so the actor's scale does not
enter into it. It is what makes this a *contact* term: a junction occludes, and the far side of the
same object does not. There is no default because a length cannot have one across meshes, and an
unbounded radius produces a global bent-normal darkening that says nothing about local contact.

**`channels` is required too**, for the opposite reason: RGB and A each already carry a signal on a
real mesh, so there is no channel the verb can pick for you. `"a"` is the cheap one — it costs no
extra interpolator, no second UV set and no texture memory, and it leaves an RGB tint intact.

`samples` (default 64) is rays per colour element and trades noise for time linearly.
`biasAngleDegrees` (default 15) is an **angle, not a distance offset**: rays arriving within that
angle of the surface's own tangent plane have their weight rolled off, which is what stops a
faceted surface reading its neighbouring facet as an occluder and drawing acne along every hard
edge. `blend` is `replace` or `multiply` (an unrecognized value is refused, not silently treated as
`replace`), and `strength` lerps the result toward fully exposed — 0 writes white and darkens
nothing.

**Read the statistics before believing the bake.** The response carries `occlusion`
(`{min, mean, max, count}`) — the raw term over every corner measured, before `strength` and before
a `multiply` blend, where 1 is fully exposed and 0 fully enclosed — plus `written`, the same four
numbers per channel the mask named, over the values actually stored. This is not decoration: the
characteristic failure of a hand-written occlusion producer is not an error, it is a **success that
ships a flat map**, and one that passes its author's hand-picked per-junction spot checks while the
rest of the mesh is white. `occlusion.min == occlusion.mean == occlusion.max == 1` is that failure,
visible in the response without opening the mesh.

Three limits to plan around. **It is a vertex bake, so its resolution is the tessellation**: a
junction crossing the middle of a large triangle is averaged out of existence before it is written,
and the fix is to subdivide near the junction, not to raise `samples`. **It is synchronous**, and
the cost is `samples` x colour-element count ray casts — fine for a part, minutes for a dense
production mesh at a high sample count. **It is not bit-reproducible**: the engine evaluator rotates
each corner's sample frame randomly, so two bakes of the same mesh differ slightly. Raise `samples`
rather than expecting equality.

**Two repairs happen before the measurement, and both are reported.** `trianglesGivenNormals` counts
triangles the mesh's primary normal overlay did not cover and which were given per-vertex normals
first — not tidiness: the engine's occlusion evaluator reads that overlay through an out-parameter
it leaves *untouched* on a miss and never checks the miss, so an uncovered triangle would have been
shaded from uninitialized memory rather than failing. `orphanColorElementsFreed` counts colour
elements no triangle referenced, freed first because the engine's vertex baker indexes an element's
triangle list without an empty check — leaving one would terminate the editor. Both are reachable in
ordinary use (a stripped or partial normal overlay; an isolated vertex after `delete_triangle` or a
boolean), both are mesh edits you did not ask for, and both raise a warning as well as a count.

The verb takes a `DynamicMeshActor`, like the rest of `geometry.*`. A `StaticMesh` asset reaches it
through `geometry.create_from_static_mesh` -> bake -> `geometry.convert_to_static_mesh`. The same
measurement is available inside a model source as the `.pwmodel` `bake_ao` modifier, which is where
it belongs for a mesh that is recompiled from source — this verb is the one that also returns the
statistics. A UV-space variant writing a `UTexture2D`, which would pair naturally with
`geometry.pack_uv_islands`, does not exist.

### geometry.revolve

`capped` caps to the **revolve axis**, not to the ends of a partial sweep. An open profile is a lathed
silhouette and its axis caps are what make it solid. A **closed section** — ring, tube, rim, flange —
is written by repeating the first point as the last; the verb detects that, drops the repeat, sweeps
the section closed, emits no axis cap and warns that it read the profile as a closed section.

Written *without* the repeat, a closed section is capped to the axis anyway and the two caps form a
coincident membrane across the bore — and `isClosed`, `boundaryEdges`,
`orientationConsistent` and `signedVolume` all read clean, because the two fans are oppositely wound
and their volumes cancel exactly — `health.selfIntersections` is the field that catches it, counted
per edge-connected shell. That spelling is genuinely ambiguous against a legitimate lathe, so
the verb warns when it sees it rather than guessing; the remedy is to repeat the first point as the
last.
