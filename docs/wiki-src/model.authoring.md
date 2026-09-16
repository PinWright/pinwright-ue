# model.authoring

<!--
Authoring note: this guide is larger than WikiDiskGenerator's kSectionSplitBudget,
so the generated page is emitted as a short `## Sections` index plus one file per
`## ` section (Saved/PinWright/wiki/model.authoring.<section-slug>.md, the slug
being the heading lowercased with non-alphanumerics collapsed to '-'). Each `## `
heading at column 0 is therefore a page boundary and a filename: keep them unique
and keep new prose under the heading it belongs to. Do not add a `### ` heading to
this file - overlay parsing stops at the first one and silently drops every `##`
section below it. This comment is stripped before rendering.
-->

Guide to writing and compiling a `.pwmodel`: the minimal file, document structure, transform spaces, material slots, collision, and iteration. Read [`model`](model.md) first for context; the normative grammar and full `PWMODEL_*` diagnostic catalog are in `docs/pwmodel-format.md` in the plugin folder.

## Authoring contract

`model.describe_ops` is the parser's source of truth. Without `op` it returns a compact index (`name`, `context`, `parameterCount`) plus `paramSets`; query an op name, `uv_layout`, `lightmap`, or `part` for complete types, defaults, allowed values, ranges, and required/authoring flags. `uv_layout`, `lightmap`, and `part` are not ops (`context` is `model` or `part_header`), and an op legal in both contexts returns one entry per context. Do not duplicate those generated tables here.

## The minimal file

```
pwmodel 0

part body {
    box size=(80, 50, 40)
}
```

Three rules make this the smallest valid document: the first non-comment, non-blank line must be `pwmodel 0`; there must be at least one `part`; and a part's first op must be a generator, because there is nothing to modify otherwise.

```js
call("model.compile", {
    filePath: "Source/Models/SM_Crate.pwmodel",
    outputPath: "/Game/Models/SM_Crate"
})
```

`filePath` is a filesystem path — a `.pwmodel` may live anywhere, inside the project or not, and it is not run through the `/Game/` sanitizer. **A relative `filePath` resolves against the project directory**, not the process working directory (which for the editor is the engine's `Binaries` folder). `outputPath` is one asset path, never a directory: one source file yields exactly one `UStaticMesh`.

## Validate, compile, inspect

Follow [`model`](model.md)'s loop: validate exactly one of `text` or `filePath` (it creates nothing), save and compile with the `filePath` form above (`model.compile` refuses inline `text`), then read the asset with [`static_mesh.describe`](static_mesh.describe.md) and [`render.capture_asset_preview`](render.capture_asset_preview.md). **Judging two compiled variants against each other needs them in one scene**, because asset-editor previews are per-asset — see [`visual-review`](visual-review.md).

   **Size is the one check that does not need step 5.** `bounds` — `min` / `max` / `size` / `center` in mesh space, on the response and on every `parts` entry — comes back from `model.validate` as well as `model.compile`, because it is measured on the merged dynamic mesh rather than on the asset. Fit a model to a required bounding box there, on the surface that creates nothing, rather than writing an asset per iteration. Expect to iterate: `noise_deform` displaces along the vertex normal by up to its `magnitude` and `harmonic_deform` scales the perpendicular radius by up to `1 + Σ amplitude`, so the extent a primitive reaches is **not** the `radius=` its line names and cannot be computed from the source. The per-part box is what names *which* part owns an extreme.

   **Pass `render.capture_asset_preview` an explicit `location`/`rotation`.** Its default camera sits on the preview scene's unlit side, and a correct closed mesh comes back as a black silhouette from it — measured at a pinned `ev100: -1`, `SM_ChessRook` 0.143 mean luminance from the default camera against 0.285 from the opposed one. That frame is where "inverted normals" reports come from. Before filing any geometry defect, work the ladder on [`visual-review.model-rig`](visual-review.model-rig.md): three of five artefacts investigated in one session were the default material's world-space grid texture, and two were shadows.

Diagnostics come back structured — severity, line, column, `code`, message, owning part, suggestions. **Match on `code`; the message is prose and is expected to change.** A boolean whose operands were disjoint so it changed nothing is an **error** (`PWMODEL_BOOLEAN_NO_EFFECT`) and fails the compile — it was a warning until it shipped twelve green examples of visibly broken models. Warnings do not fail validation, and they are where the remaining silent footguns live: `bevel` on a mesh with no polygroups *and* `bevel` on a mesh at roughly one polygroup per quad, which chamfers every interior quad boundary instead of the silhouette; a material slot nothing bound; a UV channel the compiler had to fill; `collision { auto }` producing more elements than its per-component budget.

A clean validate does not guarantee a clean compile. Validation covers grammar, the op table, parameter names, the model-level structural rules and the material bindings the geometry uses — every one of those is loaded here, and a path that will not load warns on the binding's own line; failures that only appear when the engine runs an op (a degenerate boolean, an empty hull body, a vertex index that was never valid) surface from `model.compile`. A `delete_vertex` naming an index the engine has already removed is not one of them: the op is skipped with `PWMODEL_VERTEX_ALREADY_REMOVED` and the compile continues.

**The diagnostics array is a bounded slice; `diagnosticSummary` is the count that is never bounded.** Repeats sharing a severity, code and part fold into one entry carrying `occurrences` and a bounded `occurrenceSites` list of their line/column pairs, and at most `diagnosticLimit` entries (default 5, errors before warnings) are printed. Read `diagnosticSummary.total` / `errors` / `warnings`, not the array's length; `complete: false` means something was folded or dropped. `diagnosticLimit: 0` prints every entry, which is also what pushes a diagnostic-heavy run back onto the disk-spill path — ask for it when you actually want it. Full parameter reference on [`model.compile`](model.compile.md).

## Document structure

One statement per line — newlines are significant and there is no terminator. `{` opens on its statement's line. A tuple or list may not span lines. `#` comments run to end of line.

`}` on a line of its own is a convention the parser does not enforce: `box size=(1, 1, 1) }` parses. Write it on its own line anyway — a closing brace trailing an op is the shape a later edit misreads.

After the version header, in any order:

| Construct | Cardinality | Level |
|---|---|---|
| `part <name> [at= rotate= scale=] { … }` | one or more | Part |
| `materials { Slot = "/Game/…" }` | at most one | Model |
| `collision { … }` | at most one | Model |
| `uv_layout channel=N [texture_resolution=M]` | at most one per channel | Model |
| `lightmap channel=N [resolution=M]` | at most one | Model |

**Parts are regions of one mesh, not outputs.** Every part merges into a single `UStaticMesh`; nothing opts out. A part exists to group ops under a name, carry a transform applied once to that group, and tag geometry with a material slot. The compile result reports per-part triangle and vertex counts, but they are diagnostics beside the one `assetPath`.

**The model-level statements and blocks act on the one merged asset.** `UStaticMesh` has one `LightMapCoordinateIndex`, one `LightMapResolution` and one `UBodySetup`, so a per-part lightmap channel, lightmap resolution or collision body would have no coherent meaning. `uv_layout` belongs here because it must see every part's islands at once. Material slots are model-wide for a different reason — see below.

**Set `lightmap resolution=` whenever you set `lightmap channel=`.** It is optional, and omitting it leaves the asset at the `UStaticMesh` default of **4** — so whatever the `uv` ops did to author the lightmap channel bakes at 4×4. Range 4–4096 — the engine's own bounds; `docs/pwmodel-format.md` under Lightmap carries the citations and the multiple-of-4 convention. The compiler warns when a `channel` arrives with no `resolution`, and again when the build did not store what was asked for. It is **not** derived from `uv … mode=layout texture_resolution=`: that is a UV packer gutter hint applicable to any channel, possibly several times with different values, while the mesh has exactly one lightmap resolution — wiring them would let the last `uv` op silently decide an asset property.

**Do not pack a shared atlas inside each part.** `uv` is part-scoped, so `xatlas`, `patch_builder auto_pack=true`, and `layout` each pack that part without seeing any sibling. Two parts then occupy the same 0–1 space. For a multi-part lightmap, create islands per part with `uv channel=N mode=patch_builder auto_pack=false`, then write one `uv_layout channel=N texture_resolution=M` outside the parts. It runs after merge with world-space texel density and packs one final atlas; `texture_resolution` defaults to 1024 and accepts 2–16384. A packer refusal fails the statement instead of silently keeping the old layout. The compiler checks every populated channel for positive-area triangle overlap after this step; `PWMODEL_UV_OVERLAP_ACROSS_PARTS` names the channel and parts. Ordinary texture channels warn, while the channel selected by `lightmap` fails compilation. It anchors to the last contributing part-level `uv` statement when one exists; without one it has model scope and uses the `lightmap` declaration when applicable. Edge/point contact and UV AABB overlap alone stay quiet. A declared lightmap channel that is absent, partial, or topologically invalid fails with `PWMODEL_LIGHTMAP_UV_INVALID` at the `lightmap` statement.

`Examples/pwmodel/chess_rook.pwmodel` shows this exact split: its three parts (`body`, `crown`,
and `felt`) each create channel-1 islands with `patch_builder auto_pack=false`; one model-level
`uv_layout channel=1 texture_resolution=64` then packs the final merged atlas.

Booleans take a block whose ops build a tool mesh, discarded after the operation:

```
part body {
    box size=(80, 50, 40)
    subtract {
        sphere radius=18 at=(25, 0, 0)
        bevel distance=1
    }
    uv channel=0 mode=box
}
```

The block is a full op list: it may hold a generator, modifiers, and further nested booleans. `model.describe_ops` reports `acceptsBlock` per op. **The generator-first rule applies inside it too** — a boolean block and a `hull` body each start with a generator, for the same reason a part does.

## Transform spaces

| Where | Space | Applied |
|---|---|---|
| `at=` / `rotate=` / `scale=` on a generator op | part-local | Composed into that primitive as it is created. |
| `from=` / `to=` / `up=` on a generator op | part-local | Resolved into that op's own `at=`, `rotate=` and length before it runs. |
| the `transform` op | part-local | In sequence, on the geometry accumulated so far, pivoting about the part-local **origin**. |
| `at=` / `rotate=` / `scale=` on the `part` header | part-local → mesh | Once, after the part's ops have run. |
| `at=` / `rotate=` on a collision element | mesh | At collision build, after every part transform. |

Only generator ops carry a local transform; modifiers do not, which is what the `transform` op is for. `at=` is a translation in Unreal units, `rotate=` is `(roll, pitch, yaw)` in degrees.

**`transform rotate=` and `transform scale=` pivot about the part-local ORIGIN, not about the geometry.** The op builds one `FTransform` and applies it to every accumulated vertex — scale, then rotation, then translation, all about `(0, 0, 0)` — so geometry authored away from the origin is *displaced* as well as turned, by a lever arm of `distance * sin(angle)`. A boolean tool authored at `z = 1790` and tilted 7 degrees moves `1790 * sin(7°) = 218 uu` in X; over a radius-155 shaft that lands its side wall *inside* the shaft and slices a crescent off one flank instead of taking the top off. **Nothing in the health gate reports it**: that spelling returned `success: true`, `isClosed: true`, positive `signedVolume`, `boundaryEdges: 0`, `degenerateTriangles: 0`, `nonManifoldVertices: 0`, with 168 triangles left hanging 140 uu clear of the model, and `floatingGeometry` was the only field that noticed — a tool displaced onto a different part of the same solid detaches nothing and leaves no signal at all. `scale=` carries the same lever arm, moving off-origin geometry by `(factor - 1) * distance`.

**To turn a shape in place, put `rotate=` on its own generator.** A generator's `rotate=` composes into the primitive *before* its `at=`, so `box size=(700, 700, 700) rotate=(0, 7, 0) at=(0, 0, 1790)` turns the box about itself and lands it where it was asked for, while `box … at=(0, 0, 1790)` followed by `transform rotate=(0, 7, 0)` does not. Reserve `transform rotate=` for what it actually is — swinging the accumulated geometry *about the part origin* — and note that "author the shape at the origin, warp it, then `transform at= rotate=` it into place" only holds while the shape is **still** at the origin when the `transform` runs.

**`scale=` bakes into vertex positions.** It is not a component scale, so non-uniform scale is safe here in a way it is not on an actor — no inherited transform to shear, nothing downstream that can see a scale was applied. The editor intuition transfers wrongly.

## Skinning: the space, the aiming, and what stays rigid

**`bone=` binds weights. It does not move geometry and it does not change the part's space.** A `bone=` part is authored in the same mesh space as every other part — the target skeleton's *component* space, where its ref pose puts the bones — and the binding only overwrites that part's merged vertices with one full-weight influence after the merge (`PwModelCompiler.cpp:2962-2990`). Nothing is reparented into bone-local space and nothing is translated to the bone. A staff held by a hand bone that sits at component `(-54, 10, 99)` is therefore authored with its shaft running up `x = -54`; authoring it around the part-local origin puts it through the pelvis. `Examples/pwmodel/robot_arm_skin.pwmodel` cannot settle this on its own — its bones stack along `+Z` and its `at=` values read plausibly either way — so take it from here.

**Aim a limb at its joints with `from=` / `to=`.** Every origin-capable generator is built along its own local `+Z`, so a limb drawn between two joint positions `A` and `B` used to be spelled as a centre, a height and a hand-derived `FRotator` — around a hundred of them for a 21-bone creature, all of it arithmetic outside the format and all of it somewhere for a mistake to hide. Write the two joints instead:

```
capsule radius=6 from=(-54, 10, 99) to=(-54, 10, 132)
```

The op is centred on the midpoint and aimed from one point at the other, and its length comes from the distance — `height` on `cylinder` / `cone` / `pipe`, `length` **minus its two caps** on `capsule`, the Z of `size=(x, y, 0)` on `box`. `at=` and `rotate=` are refused alongside it, as is a second spelling of the length; `model.describe_ops` publishes each op's `aimExtent` and `aimExtentParam`, and every way of writing a contradiction is refused with its own `PWMODEL_AIM_*` code naming the remedy. Remember the bone positions are **component-space**, per the paragraph above — that is what `from` and `to` are written in, not bone-local offsets.

**Use `up=` the moment an aimed op carries a non-uniform `scale=`.** Aiming fixes two degrees of freedom; the third — the twist about the aimed axis — decides which world direction the flattening lands on, because **the flattening axis is local `Y`**. Left implicit it is still deterministic (`roll = 0`, so local `Y` stays horizontal), but "horizontal" is not what a leaf wants: a fan of cards aimed outwards at varying pitches comes out lying flat in one place and standing on edge in another, with every parameter in the document correct. `up=` sets local `+Y` perpendicular to both it and the aim, so `scale=(1, 0.2, 1)` squashes the card **towards `up`** wherever it points — for a leaf, the normal of the surface it lies in. An `up=` along the aim names no frame and is refused; the *default* is not, so a vertical limb needs nothing.

**Rigid parts are how a stiff attachment stays off the nearest bone.** The smooth solve has no selection input and weights every vertex by proximity, so a frond tip 50 uu out from the neck binds to an upper arm and a staff finial 150 uu above the hand binds to whatever is closest — both smear the moment anything animates. A `bone=` on that part is the only way to say "this mass is rigid to that joint", and because rigid bindings are applied *after* the solve, it wins. Mixed rigid/smooth in one model is the normal case, not an escape hatch.

**The other trigger is a near-tie, and it does not look like one.** `direct_distance` weights by range, so two bones at nearly equal range split a vertex between them and the mass *smears* instead of snapping to the wrong joint — which is much harder to spot than a limb that jumps. Measured on a biped: a snout tip 25 uu forward of its head bone sat 19.9 uu from `head` and 20.0 uu from `neck_01`, and would have deformed on every head turn. Anything whose two nearest bones are within a few percent of each other in range wants `bone=`. So does anything worn or carried, for the same arithmetic read the other way — cloth hanging past a hip is nearer the shin below it than the pelvis it hangs from, and a distance solve makes the skirt swing with the stride.

**Expect [`skeleton.audit_skin_weights`](skeleton.audit_skin_weights.md)'s `influenceReach` check to name every vertex of a rigid part.** It measures bind-pose distance from each influence to its bone and cannot know the distance was intended: a staff bound to a hand bone reports `normalizedDistance` near 7 with ten `closerBones`, for every one of its vertices. That is the binding working. The check returns a verdict only when you pass `maxReachDistance`, so its default `reported` status is the correct reading — do not gate a `bone=`-using model on its offender list. The four checks that *do* gate (`zeroInfluence`, `weightSum`, `influenceCount`, `coincidentSplit`) are the ones to read.

## Faceted shading

**Per-face normals are reachable, but not under that name.** There is no `set_per_face_normals` op. The equivalent is `split_normals split_angle=0` — it splits the normal at every edge with any dihedral angle at all, which leaves each triangle carrying its own face normal.

```
part canopy {
    sphere radius=50 subdivisions=3
    split_normals split_angle=0
}
```

Measured on that document, 2026-08-19: 48 triangles either way, but [`static_mesh.describe`](static_mesh.describe.md) reports **54** render vertices without the op and **144** with it — 48 × 3, fully split, which is what faceted shading is. Treat this as a style decision rather than a detail: a faceted low-poly prop -- a cut gem, a chiselled rock -- depends on it, and the same silhouette with smooth normals reads as a soft blob.

**Zero is the MAXIMUM this op splits, and a negative angle splits less rather than more.** The engine compares dihedral angles through a *cosine* of `split_angle`, so the sign is discarded: measured on the sphere above, `split_angle=0` bakes to **144** render vertices (48 × 3, fully faceted) while **both** `-60` and `+60` bake to **48**. An author reaching for `-1` to "split everything" therefore gets a coarser result than the default, and no count distinguishes it from a value they meant. The op now warns naming the threshold that actually ran; it does not clamp, because the absolute value is a legal threshold and is what the engine was always going to use.

**`recalculate_normals` has no `split_angle` — use `split_normals`.** It used to publish one, inherited from the `geometry.recalculate_normals` verb, and it reached no engine call: measured 2026-08-19 on the sphere above, `split_angle=0` and `split_angle=180` both compiled to 54 render vertices, the unsplit number. It was removed rather than wired up, because the engine call behind this op (`RecomputeNormals`) re-averages *within* existing hard edges and cannot create one — it has no angle to take. `split_normals` already wraps the call that does, with the same parameter name and units. `recalculate_normals` now carries `area_weighted` and `angle_weighted`, the two flags its engine call actually accepts.

`split_normals` is an ordinary modifier and acts on the geometry accumulated in the part so far, so it belongs after the ops that build the shape.

**`split_by_opening_angle=false` is not a no-op — it SMOOTHS.** `split_normals` has two split predicates, `split_by_opening_angle` (default true) and `split_by_face_group` (default false), and the engine ORs them. Turn both off and there is no predicate left, so the op recomputes normals with **no hard edges at all** and the mesh comes back fully smoothed rather than unchanged — the exact opposite of what a reader disabling the angle test expects. It warns, so read the warning rather than the parameter name. To face-group only, set `split_by_face_group=true` in the same statement; to leave the mesh alone, omit the op.

## Seeded variation

**`noise_deform seed=` is how one recipe becomes a family.** Perlin noise is a *spatial field*, not a per-call random draw: the displacement of a vertex is a pure function of its position, the seed and `frequency_shift`. Two consequences follow, and the second is the one that bites.

Within one model, lobes already differ without touching the seed — the compiler bakes each op's `at=` into vertex positions before the noise runs, so branches at different coordinates sample different parts of the field. Between two *models* from the same source, nothing differs unless the seed does. That is the case a variant family needs: ten boulders from one recipe are ten seeds, one fixed constant per variant (101, 202, 303, and so on), so any one of them is reproducible from its number alone. Before `seed` was exposed, every `noise_deform` ran at the engine default of 0 and the ten came out identical.

```
part canopy {
    sphere radius=60 subdivisions=3
    noise_deform magnitude=14 frequency=0.04 seed=101
    noise_deform magnitude=4  frequency=0.18 seed=1101
}
```

**Two octaves is two ops, not a layer list.** The engine's noise struct carries a single layer — `BaseLayer` has no siblings — so a coarse pass plus a finer detail pass is two `noise_deform` lines. Give them different seeds as well as frequencies; at one seed both passes sample the same field and the fine one mostly reinforces the coarse one.

**`frequency_shift=(x, y, z)` slides where a seed jumps.** It offsets the position before the lookup, moving the sampling window through the same field continuously — the knob for sweeping one shape, where `seed` lands somewhere unrelated and is the knob for another variant. Omitting both reproduces the pre-`seed` behaviour exactly: seed 0, zero shift.

## Scalloped and lobed shapes

**`harmonic_deform` is the periodic counterpart to `noise_deform`.** Noise is a spatial field with no period; this is a period with no field. The displacement is a pure function of a vertex's **azimuth about an axis**, so it repeats exactly `order` times per revolution and is the same at every height and every radius along that azimuth. A conifer's scalloped rim is one term at order 2; a lumpy crown is orders 3 and 5 summed.

```
part crown {
    sphere radius=100 subdivisions=24
    harmonic_deform terms=[(3, 0.2, 30), (5, 0.11, 30)]
}
```

`terms=[(order, amplitude, phase), …]` is the whole vocabulary — a list of literals, because the format has no expressions. The sum is `Σ amplitude · sin(order · θ + phase)`, with `phase` in degrees like every other angle here.

**`amplitude` changes units with `target`.** On `target=radial` (the default) it is a **fraction** of the vertex's own perpendicular radius, so the map is `radius ← radius · (1 + Σ)` and a scallop stays proportional as a revolve profile's radius changes along the axis. On `target=axial` it is **Unreal units** added to the axis coordinate, because an axial displacement has no local scale to be a fraction of. A rim that alternates in both radius *and* height is two lines:

```
part tier {
    revolve profile=[(0, 0), (120, -30), (0, -60)] steps=48
    harmonic_deform terms=[(2, 0.18, 0)]
    harmonic_deform terms=[(2, 25, 0)] target=axial
}
```

**Nothing here varies along the axis.** A shape that needs both an azimuthal period and a change with height is this op plus a warp deformer, not a parameter on either.

**Winding survives it, and that is the reason the op exists.** A radial displacement is a strictly positive scale of the perpendicular radius: it moves no vertex's azimuth and no vertex's axis coordinate, so no triangle can reverse its facing normal. The op **refuses** a radial amplitude total of 1 or more, where that scale would reach zero and fold the surface through its own axis, and it refuses a fractional or below-1 `order` — a fractional wave does not close on itself after a full turn and tears the surface at the wrap however small the amplitude. Vertices on the axis are left alone on both targets, because their azimuth is undefined.

**The one thing the bounds cannot rule out is aliasing, so it is measured.** The op counts the distinct azimuths actually present — `revolve steps=48` has 48 — and warns per term whose `order` reaches half of them. Past that limit the surface gets a lower-order wave than the one asked for and neighbouring triangles can cross. Raise the base primitive's segment count; lowering the amplitude does not fix it.

**`center=` is a point on the axis, not a bounding-box centre.** It defaults to the origin, which is where `revolve` puts its own axis. `spherify` and `cylindrify` fit to the bounding box because their target shape is derived from it; a box-derived centre here would move whenever an earlier op changed the mesh's extent, so the same document would deform differently depending on what ran before it.

## Materials

```
materials {
    Shell = "/Game/Materials/M_Metal"
    Trim  = "/Game/Materials/M_Rust"
}
```

**`Default` is the LAST slot, not slot 0.** It is the one slot exempt from first-use order: it is
placed after every slot a `material=` tag names, so an untagged op between two tagged ones cannot
renumber a slot you named. It is ID 0 only when it is the sole slot. `material="Default"` written by
hand is a name you chose and keeps its first-use index like any other. When a `materials` block is
present and does not bind `Default`, the slot is still reported as `PWMODEL_IMPLICIT_DEFAULT_SLOT` —
that geometry ships on the engine default material. Tag the op, or bind `Default`.

**A closed `revolve` section needs its first point repeated as its last.** `capped` caps to the revolve
**axis**, not to the ends of a partial sweep: an open profile is a lathed silhouette whose axis caps
make it solid, but a closed section — ring, tube, rim, flange — written without the repeat gets capped
to the axis anyway, and the two caps form a coincident membrane across the bore. The fans are oppositely wound, so `signedVolume` cancels
exactly and `isClosed`, `boundaryEdges` and `orientationConsistent` all read clean — `health.selfIntersections`
is the one field that sees it, and it is the third term of the gate. With the repeat present the op detects the
closed section, drops it, sweeps the section closed and emits no axis cap.

**Slot identity is the name, and slots are model-wide.** Two parts tagging `material="Shell"` share one slot; two names produce two. Slots are allocated in first-use order across the whole document, and a slot needs no `materials` entry to exist — the block only *binds* an asset to a name. It does not define materials; MGIR owns material graphs (see [`material.mgir`](material.mgir.md)).

**A slot is for a different shading behaviour; `color=` is for tint within one slot** — and vertex colour is invisible until some material samples it, which no engine placeholder does. [`model.vertex-color`](model.vertex-color.md) has the split, the material to create, and why a `color=`-heavy file can still compile to a uniformly grey asset.

Slot **`Default`** is created **only if some geometry is untagged**. A fully tagged model has no `Default` slot at all, so do not write code that assumes slot 0 is it.

- `material=` and `color=` are tagging parameters on the **primitive generators** — the shape ops plus `revolve` and `procedural_mesh`. `append_buffers` and `append_triangle` carry `material=` but **not** `color=`, for different reasons: `append_buffers` already has a per-vertex `colors=` buffer and the scalar writes every vertex, so the two together would silently overwrite it, while `append_triangle` has simply never needed one. **Three modifiers and all four booleans carry `material=` as well.** `sweep` and `extrude_along_spline` append triangles of their own; `bevel` creates chamfer faces; a boolean creates the walls a cut opens and the surface a union keeps. Every other modifier deforms, deletes or copies triangles that already carry a slot, so a tag there would recolour geometry rather than colour new geometry. `model.describe_ops` reports `acceptsMaterial` per op, which is the list to read rather than any enumeration written here.
- **Untagged, an appending modifier inherits the slot of the geometry it appends onto** — not slot 0. That distinction is the whole point: slot 0 is model-wide first-use order, so it is *another part's* material, and a swept rod in part `b` used to ship rendering in part `a`'s. Inheritance costs no slot, so the table is unchanged. When the geometry it extends carries more than one slot there is no single answer, so the op takes the slot most of it is on and warns `PWMODEL_MODIFIER_MATERIAL_AMBIGUOUS`; write `material=` there.
- **`append_buffers` says it two ways and you must pick one.** `material="<Slot>"` resolves through the model-wide slot table like every primitive; `material_id=<n>` writes a **raw** index straight onto the triangles for input that already computed IDs, and an index past the end of the slot list pads it with a `PWMODEL_MATERIAL_ID_OUT_OF_RANGE` warning. Writing both on one op is `PWMODEL_MATERIAL_ID_CONFLICT`, an error — the compiler can honour only one, and it honours the raw ID in silence.
- **A boolean takes `material=` and it names the faces the operation CREATES** — the walls a `subtract` opens, the tool surface a `union` keeps, the caps `fill_holes` closes. Triangles the operation KEEPS are never recoloured. Untagged, the new faces inherit the slot of the geometry the boolean was applied to; where that carries more than one slot the op takes the slot most of it is on and warns `PWMODEL_BOOLEAN_MATERIAL_AMBIGUOUS`. A tag that reaches no triangle of the result is `PWMODEL_BOOLEAN_MATERIAL_UNUSED` — the realistic source is a tool solid the operation discards entirely, usually one that misses the target while its siblings in the block hit it. Not `trim`: it dispatches the same engine boolean as `subtract`, so its tool surface becomes the cut face.
- **`color=` on a boolean is still refused**, with `PWMODEL_MATERIAL_ON_BOOLEAN`. A boolean creates faces but no *vertices*: every vertex in the result comes from an operand and already carries the colour its generator gave it. Write `color=` inside the block, or `set_vertex_color` after the op.
- **A generator inside a boolean block carries its own `material=`** through the same model-wide table a part-level op uses, so `union { box material="Beta" }` really is on `Beta`. Only a `hull` body allocates no slot — its geometry becomes collision and never a render section.
- **`bevel`'s chamfer faces take the material of the surfaces they join.** `infer_material_id` defaults to **true** here (the engine's own default is false), and where the two sides of an edge disagree the fallback is the slot most of the bevelled geometry is on — never a fixed index. `material=` names the faces outright; `material_id=<n>` is the raw-index escape hatch, and both on one op is `PWMODEL_MATERIAL_ID_CONFLICT`.
- Two warnings, neither fatal: geometry tags a slot `materials` does not bind (empty slot on the asset), and `materials` binds a slot no geometry references (binding dropped).

## Collision

At most one `collision` block, carrying a `complexity` flag plus **either** explicit elements **or** a single `auto` rule — both together is `PWMODEL_COLLISION_CONFLICT`.

```
collision {
    complexity = simple_and_complex
    box size=(78, 48, 38)
    hull { cylinder radius=20 height=60 }
}
```

**`hull { … }` runs its block through the ordinary op pipeline and keeps only the convex hull of the result.** The name states what happens to your input rather than what gets stored (`FKConvexElem`), because every concave feature is discarded — a `hull` whose body subtracts a slot yields a plain cylinder, silently. **Several `hull` blocks are the hand-authored convex-decomposition path**, one block per convex piece. For genuine concavity in one element, `auto method=level_sets` reaches `FKLevelSetElem`.

Use `auto` when you want the engine to derive collision from the finished mesh (`method=`, `max_hulls_per_component=`, `simplify_to=`, and the `detect_*` flags; `model.describe_ops` lists the accepted methods, which are reflected from the engine enum). `convex points=[…]` exists only so machine-generated hulls have somewhere to land — do not hand-write vertex lists.

**`max_hulls_per_component=` (default 8) is a CEILING applied to each connected component of the merged mesh, never a quota and never an asset-wide total.** The engine splits the input into one submesh per component before any decomposition runs, then spends the budget only where a split reduces hull error — a piece that is already convex enough comes back as one hull however much budget it is handed — and drops fully contained hulls afterwards. So the produced count is bounded above by `budget × components` and is otherwise unpredictable: measured on `ships_wheel`'s 11 components, `1` → 11 elements, `2` → 11, `3` → 13, `4` → 44 on one run and 14 on the next five, `8` → 22, `16` → 37. **It is not proportional to the budget, not monotonic in it, and not reproducible run to run** — read `collisionElements` off the compile response, and never gate a test on an exact count above budget `1`, the one case that bypasses the decomposer. The old spelling `max_hulls=` still works and warns naming the rename; supplying both warns and `max_hulls_per_component` wins; exceeding the budget warns naming the budget, the element count and the component count. It was renamed rather than reinterpreted because the old name could not be made true: one hull per component is the engine's floor, so no arithmetic honours a budget below the component count, and the only real whole-asset cap merges hulls across disjoint pieces — swallowing the air between them. Full derivation and citations: `docs/pwmodel-format.md` under Collision.

Two measurements that read wrong if assumed: `box size=` is **full extents**, not half-extents; `capsule height=` is the **total** height including both caps, converted to `FKSphylElem::Length` as `max(0, height - 2 * radius)`, so a height below `2 * radius` clamps to a sphere and warns.

## Primitive and response readings that are not what the name suggests

**`sphere` is a box-sphere.** It calls `AppendSphereBox`: a rounded cube, six subdivided quad faces projected onto the sphere, no poles and no seam, with a triangle count of `12*(subdivisions-1)^2` (2 → 12, 3 → 48, 4 → 108, 5 → 192, 16 → 2700). A recipe ported from a lat/long sphere therefore yields a different polyhedron *and* a different triangle count at the same nominal value — which has already cost one authoring cycle. `subdivisions` has an effective floor of **2**; asking for 1 draws the same 12-triangle cube and now warns that it was clamped. **At that floor `radius=` is not the half-extent:** the mesh is the 8 cube corners and nothing else, each projected to `sqrt(1/3)` per axis, so the shape measures `radius/sqrt(3)` = `0.5774 * radius` on every axis — 42% short — and geometry placed against the nominal radius lands *outside* it (`sphere radius=100 subdivisions=2` reports a `bounds` size of 115.5, not 200). From `subdivisions=3` up, edge and face-centre vertices reach `radius` and the box matches. At the floor, fit against the reported `bounds`.

**`pipe` is centre-placed, and it was not.** `height=100` spans part-local z `-50..+50`, the same span `cylinder height=100` covers. It was the family's one **base**-placed primitive (z `0..100`) until it was rebuilt as a swept annulus, so **a document written against the old behaviour now sits half a height too low relative to its `at=`** — raise `at=` by `height/2`. The same rebuild closed it: it was an uncapped outer cylinder minus an inner one, forming no annular end caps and rendering see-through with no diagnostic. Two consequences to author around: `0 < inner_radius < outer_radius` is **required**, not clamped — violating it is a line-anchored `PWMODEL_OP_FAILED` carrying `INVALID_PARAMS`, since there is no non-arbitrary value to clamp to — and `pipe` carries exactly four polygroups, so unlike `torus` / `arch` / `revolve` it is safe to `bevel`.

**Whole-mesh `extrude` does NOT pick the facing for you: `direction` must point along the surface's own facing normal, or the slab comes out inside-out.** Omitting `face_direction` selects everything, which on an open sheet is duplicate-and-wall and is the normal way to give a flat `append_buffers` panel a thickness. The engine then displaces **the original triangles** by `direction * distance` (`OffsetMeshRegion.cpp:696-705`), so they keep their authored winding and land on the +`direction` face, and reverses the **stationary duplicate** on the -`direction` face (`:734-744`). Outward therefore requires `direction` ∥ facing normal — and remember the facing normal is the **negation** of the right-hand rule (`VectorUtil::Normal` returns `(V2-V0) x (V1-V0)`), so a table wound for RHR +Z has facing -Z and wants `direction=(0,0,-1)`.

**A closed shell renders identically inside-out, so no A/B of the `direction` sign can find this.** Backface culling shows you whichever wall faces you, the two walls carry opposite normals, and they sit one thickness apart. An earlier revision of this paragraph cited exactly such an A/B — `direction=(0,0,-1)` vs `(0,0,1)` on `Examples/pwmodel/origami_crane.pwmodel`, mean luminance 0.47222 vs 0.47229 — as proof that the sign does not set the facing. It is not proof of anything except that the test is blind, and acting on it shipped that file's whole `sheet` part as an inside-out shell for two releases.

**Two things now catch it, and neither is a render.** The compiler warns `PWMODEL_EXTRUDE_FACING_OPPOSED` at the `extrude` line when `direction` opposes the accumulated surface's facing normal — the authoring mistake itself, with a line number, at the moment you make it. And `health.signedVolume` on the compile / validate response is **negative** on an inverted closed shell and positive on a correct one, per part as well as model-wide. The rest of `health` is still blind and always will be: an inverted shell is still closed, still manifold, still 0 boundary edges, and identical in every count — so gate on `health.isClosed && health.signedVolume > 0`, and read the per-part `signedVolume` on a multi-part model, because the model-wide sum averages one inverted part away.

**Measure the facing instead: delete the `extrude` and render the bare panels from both sides.** A one-sided panel is invisible from behind, so the side it draws on *is* its facing normal — measured, not derived. Restore the `extrude` afterwards. To see the difference the sign makes, extrude the same panel two ways at a thickness large enough to put a camera inside (`distance=2000`, `translate_mesh` to recentre): with `direction` along the facing the mesh vanishes from within (every wall back-facing), against it you are standing in a lit corridor.

**It matters even where nothing looks wrong**, because winding — not shading — is what the offline consumers read. The mesh distance field decides inside/outside purely by counting backface hits (`MeshDistanceFieldUtilities.cpp:261-281`), so an inverted shell inverts its field and Lumen/DFAO then light the part as though the camera were inside it. On a project with `bGenerateMeshDistanceFields` and Lumen on, that is a visible defect in a lit scene and invisible in every asset-preview capture.

**`revolve` has the same trap in its `profile`, and it is not the same-looking figure.** `x` is the radius and `y` is the height, and the sweep keeps the winding the point order gives it — so the section must be traversed **counter-clockwise** in that `(x, y)` plane: **up the OUTER face, over the top, back down the INNER face**, which is the enclosed material staying on your left. Walked the other way the solid comes out uniformly inside out, with the same consequence the paragraph above describes and the same total blindness in every other signal. Measured on a 16-point closed off-axis dome section at `steps=36`, revolved both ways: `isClosed`, `boundaryEdges`, `degenerateTriangles`, `nonManifoldVertices`, `orientationConsistent`, `floatingCount`, `bounds` and `meshTriangleCount` were **identical**, and `signedVolume` alone flipped from `+1.73e9` to `-1.73e9`. `PWMODEL_REVOLVE_PROFILE_REVERSED` now names it at the `revolve` line whenever the op's own sweep closes and encloses negative volume — it stays quiet on an uncapped partial sweep, where signed volume means nothing, and on a negative-determinant `scale=`, which reverses winding for a reason of its own. **Do not read the direction off the worked examples**: every `revolve` in `Examples/pwmodel/driftwood.pwmodel` and `Examples/pwmodel/crystal_cluster.pwmodel` starts its profile ON the axis at `(0, …)` and runs outward, and that lathed silhouette is a different-looking figure from the closed off-axis loop a dome, drum, vase, bell or rim needs — the rule is the same, copying the picture does not transfer it.

**`shell` and `offset_faces` thicken a flat profile identically but in OPPOSITE directions.** On `ring outer_radius=40 inner_radius=26 segments=48` authored at z=0, `shell thickness=4` returns bounds spanning z `-4..0` and `offset_faces distance=4` returns z `0..4` — same 384 triangles, same 40 outer radius, neither distorting the rim despite `shell` being an iterative solve. So the choice between them is which side of the profile plane you want the plate on, plus `shell`'s ability to serve a whole profile *set* in one op (a `ring` and a `disc` in the same part get one thickness by construction). `offset_faces` splits more render vertices for the same mesh — 576 against 388 on that ring.

**`mirror` APPENDS a reflected copy — it does not replace the mesh with its reflection.** The op adds an axis-negated copy of the accumulated geometry alongside the original, with the copy's triangle winding reversed so it is a genuine solid rather than an anti-solid. That is the same append trap `sweep` and `extrude_along_spline` carry (below): siblings in a part are appended, never unioned, so **two halves that overlap keep every face buried between them** and hand the next boolean a self-intersecting mesh. Wrap the op in a `union { }` block when the sides meet rather than merely touch; `array_linear`, `array_radial` and `array_along_path` append their copies the same way.

**Nothing reports it if you do not, and the blind spot is wider than those ops.** `PWMODEL_UNUNIONED_OVERLAP` is raised only from the generator path (`PwModelCompiler.cpp:1129`), and **every** modifier *clears* the footprint list the check compares against (`PwModelCompiler.cpp:2285`) — correctly, because a modifier may move or multiply geometry, so the boxes recorded before it stop saying where anything is. Two consequences follow, and the second is the one that misleads:

- **Six ops append a closed solid and none of them can raise the warning**: `mirror`, `array_linear`, `array_radial`, `array_along_path`, **`sweep` and `extrude_along_spline`**. The last two are modifiers like the rest, so a part built out of swept tubes is never checked however deeply they interpenetrate — and `sweep` is exactly what this page recommends for tube-shaped geometry.
- **A modifier also blinds the check for every generator after it in the same part.** The list is cleared, not merely skipped, so in `box … sweep … box` the second `box` is compared against nothing. A part that mixes the two gets warnings about whichever run of generators happens to come last and silence about everything before it, which reads as *"there is one overlap, and it is here"* when the real overlaps may be elsewhere and larger.

- **Two parts are compared as WHOLES, by a second code.** The footprint list is scoped per `RunOps` (`PwModelCompiler.cpp:2299`) — one part, one boolean tool or one hull body — so it never sees across a part boundary, and the format can force the shape that hides behind that: `noise_deform` has no material scope and `harmonic_deform` carries one `center=` and one phase, so **a model that needs per-piece deformation has no choice but one part per piece**. Measured on a foliage model built that way, before the fix: 49 interpenetrating components, 20 one-primitive parts, **zero** warnings. `PWMODEL_UNUNIONED_OVERLAP_PARTS` now covers it, comparing each part's final box against every other part's. It is **one diagnostic per model**, naming the pair count and the first several pairs — a per-pair warning would run to dozens on a correct organic model, and a code that fires dozens of times is one you learn to skip. It is a separate code because `union { }` is an op *inside* a part and is therefore not available across parts at all.

So read the op-level warning as a positive signal only: it names a real pair when it fires, and its **absence proves nothing within a part**. What is now covered is the part-level question — a part built entirely out of `sweep` calls is compared against every other part even though none of its own ops can raise the op-level code. `health.boundaryEdges` and a look at the mesh remain the only signals for two swept tubes interpenetrating *inside one part*.

**And where it fires correctly, the remedy is often unavailable.** `union { }` cannot be applied to an overlap whose two solids carry *different* material slots, because ops inside a boolean block allocate no slot (see [Materials](#materials)) — so unioning a trim ring onto the limb it stands proud of drops the ring's slot, and the model loses a material. That is the majority case on any multi-material figure: a band on a limb, a claw on a foot, an emissive eye inside a skull. Same-slot pairs *can* be unioned, and there the cost is that `PWMODEL_BOOLEAN_NO_EFFECT` is an **error that aborts the part**, so a later radius tweak that separates two solids turns a clean build into a failed one. Measured on a 14-part, six-slot, 102-primitive skinned biped: 76 warnings, every one a deliberate overlap and none of them unionable. What scales instead is `collapseDiagnostics` — on by default, and it folded those 76 into 7 entries, one per part — plus a render. Interior faces buried inside an opaque closed body cost triangles and nothing else.

**`bevel` chamfers every polygroup edge, and the polygroups are not yours to choose.** A mesh with no polygroups is returned unchanged; a mesh with one polygroup per quad — which is what `torus`, `arch` and `revolve` produce, and nothing on this side can turn it off — gets every interior quad boundary notched at roughly 3x the triangle cost. Both warn. Bevel a `box` or a boolean result instead; the mechanism and the engine citations are on [`geometry.bevel`](geometry.bevel.md).

**Round primitives have segment floors, and they are not all 3.** Every count is clamped where the compiler can report it, so a below-floor value now compiles with a warning naming what you asked for and what ran — `cylinder segments=1` says so instead of quietly building the same 18-triangle prism as `=3`. The floors come from the engine generators, and four families of them are not 3: `capsule hemisphere_steps` floors at **2**, `torus major_segments` and `arch major_steps` at **2** (their `minor_*` partners at 3), **`revolve steps` at 2** — it drives the same revolve generator — and `height_steps` on `cylinder`, `cone` and `pipe` at **0**, where 0 is a legal value meaning "no intermediate loop". Everything else radial floors at 3. A value at or below **0** falls back to the verb's own default rather than to the floor, so `segments=0` builds the 16-segment cylinder, not a prism.

**A revolve's step floor follows its sweep angle: 2 below 360, 3 from 360 up.** The engine's own floor is 2 everywhere and that is right for a partial sweep — `arch major_steps=2 angle=180` is a sound half-arch and is left exactly as written — but a closed revolution at 2 cannot close. `torus major_segments` (always a full turn), `arch major_steps` at `angle=360` and `revolve steps` at `angle=360`, its default, therefore clamp to 3 and warn like any other clamp. What 2 used to build, measured with `model.validate`: `torus major_segments=2` gave **16 triangles, 16 boundary edges, not closed**; `revolve steps=2 angle=360` gave **8 triangles, 10 boundary edges and 6 degenerate triangles**. Both came back `success: true` with **no clamp warning** — nothing was clamped, 2 was in range — so the only signal was the merge-stage `PWMODEL_MESH_NOT_CLOSED` one stage later, which names the symptom rather than the parameter. **Three closes both** (torus 3 → 48 triangles closed, `revolve steps=3 angle=360` → 24 closed).

**`spherify` targets the largest half-extent, not the bounding sphere — so the SHAPE you feed it matters more than the `factor`.** For a `box size=(150,118,92)` the target radius is 75, not the 105.93 the name suggests: the corners end up *outside* the target sphere and are pulled in, while the flat centres of the short faces are pushed out by `(1-factor) + factor·(Emax/Emin)`. The op is purely radial and cannot fold anything — measured on the engine's own tessellation, boxes up to 10:1 spherified at `factor=1.0` produce 0 inverted, 0 degenerate and 0 self-intersecting triangles. What it *does* leave is uneven triangle sizes (that box goes in at an edge-length ratio of 1.64:1 and comes out at 2.81:1, shortest edge 21.3 uu), and the **next** op is what folds: `noise_deform magnitude=12` is up to 24 uu of relative displacement across a 21 uu edge, which is exactly the black cavity in the flank that `Examples/pwmodel/crystal_cluster.pwmodel` records. `spherify` now warns when `factor·(Emax/Emin − 1)` exceeds 1/3 (150×118×92 @ 0.82 → 0.517 warns; the shipped 132×118×104 @ 0.70 → 0.189 does not; any cube → 0). Three remedies, in order: keep the source near-cubic, lower the `factor`, or put a `remesh_uniform` between `spherify` and any displacement op — that last one re-uniformises exactly the edge lengths `spherify` skewed, which is why the shipped recipe has one.

**`cylindrify factor=1` collapses cap geometry, and 1 is the default.** It fits the **mean** perpendicular radius (not a bounding quantity, unlike `spherify`), and at exactly 1.0 every vertex lands on that one radius — so two vertices sharing an angular direction but not a radius become the same point. A box's two cap faces guarantee it: on a 100³ cube at `segments=(5,5,4)`, `factor=0.82` gives 0 degenerate triangles and `factor=1.0` gives 24 degenerate, 28 inverted and a zero-length edge. Pass a factor below 1 unless the part is a tube with nothing near the axis. Its anisotropy warning is measured on the **cross-section**, so the axis dimension never enters it.

**`cone top_radius=0` does not build an apex — it builds 64 zero-area triangles.** It is a truncated cone whose top ring has radius zero, so all `segments` top-ring vertices sit on one point: the top cap becomes a fan of zero-area triangles and every side quad splits into one real triangle and one more. Measured on a lone `cone base_radius=148 top_radius=0 height=220 segments=32`: **192 triangles, 64 of them degenerate** — which was the entire `PWMODEL_DEGENERATE_GEOMETRY` count of `Examples/pwmodel/watchtower.pwmodel` before its roof was truncated. `remove_degenerates` does delete them (192 → 128, at the defaults and at aggressive thresholds alike) and that is the **wrong fix**: the coincident apex vertices stop being shared and the part comes back with 64 boundary edges and `PWMODEL_MESH_NOT_CLOSED`. Give the cone a small non-zero `top_radius` instead and cap it with something — a real spire stops at a seating and carries a finial.

**`ramp` and `stairs` do not place themselves the way their parameter names read.** Both were measured from single-op probes, with no `at=` and no `rotate=`, by reading `static_mesh.describe` bounds:

- `ramp size=(90, 240, 120)` spans x `0..240`, y `0..120`, z `0..90`. It is **corner**-placed, not centred like `box` / `cylinder` / `cone`, and the three `size` components do not land on X, Y, Z in that order.
- `stairs step_size=(90, 15, 30) num_steps=8` spans x `0..240`, y `-45..45`, z `0..120`. The run is along **+X** starting at the origin, the **width** is on Y and centred, and the rise starts at z 0 — i.e. depth on X and width on Y, the transpose of "width, rise and depth along local X, Z and Y".

Written as if both ran along Y, a `ramp` + `stairs floating=true` pair lands the wedge *beside* the treads rather than under them, crossing them in plan, and sends the whole flight off along an axis the rest of the document does not use. Nothing in the compile reports it; only a render does. Prefer one `stairs` with `floating` left off — its own understructure is the raking wall — and rotate the single op rather than keeping two in step. `rotate=` is applied before `at=`, so `rotate=(0, 0, 90) at=(0, -380, 0)` gives x `-45..45`, y `-380..-140`.

**`transform_uvs` is the only way to ROTATE a UV channel — and no engine placeholder material can show you that it worked.** `uv` projects box, planar and cylindrical axis-aligned and carries no rotation parameter, so a pattern meant to run at 45 degrees (chequer plate is the everyday case) can only be said afterwards. It is also the manual way to change one part's density on a channel produced by `mode=xatlas` / `patch_builder` / `layout`; model-level `uv_layout` instead normalizes world-space density across the merged mesh. Its own `scale` and `translate` merely duplicate `uv mode=… scale=` and earn nothing on their own.

**The catch is the verification, not the op.** `/Engine/EngineMaterials/M_Grid` and `/Engine/EngineMaterials/WorldGridMaterial` — the two grid materials every example in `Examples/pwmodel/` binds as placeholders — are **world-space**: they never sample a UV channel. Measured with a three-panel probe (one plane at `uv mode=box scale=(0.02, 0.02)`, one at `(0.10, 0.02)`, one with `transform_uvs channel=0 rotate=45 scale=(5, 5)` on top of the first, all bound to `M_Grid`, rendered side by side): **all three panels came back identical**, and the heavy grid lines fell at different places on each panel according to its position in the WORLD. So `uv … scale=` and every `transform_uvs` under these bindings is a visual no-op, and a UV change cannot be told apart from an op that wrote nothing. Bind a material that samples UV0 before you judge any UV work, and do not ship a UV op into an example you cannot see. This is the same trap [`model.vertex-color`](model.vertex-color.md) documents for `color=`, one channel over — with the difference that vertex colour has a debug material that shows it (`/Engine/EngineDebugMaterials/VertexColorViewMode_ColorOnly`, bindable per instance through `actor.spawn`'s `materialPaths`) and UVs have none in engine content.

**`set_vertex_color` and `set_uvs` address elements by ID, and IDs are not stable across ops.** `set_uvs index=` and `set_vertex_color index=` write one vertex, so they are only usable where you know the vertex numbering — which in practice means immediately after a single generator, and not after any boolean, `array_*`, `mirror`, `weld_vertices` or `remove_degenerates`, all of which re-index. `set_vertex_color set_all=true` has no such problem, and is **redundant with the `color=` parameter every generator already carries** — with one exception worth knowing: `sweep` and `extrude_along_spline` are not generators and take no `color=`, so `set_all` is the only route to a vertex colour on swept geometry. To check that any of this landed, bind the slot to `/Engine/EngineDebugMaterials/VertexColorViewMode_ColorOnly` — `actor.spawn` takes `materialPaths` — and render; the placeholder engine materials ignore vertex colour, so a write that never happened looks identical to one that did.

**`set_vertex_color channels=` decides which of R/G/B/A the write touches**, defaulting to all four so a document written before it existed compiles unchanged. Any non-empty subset works, in any order and any case (`"a"`, `"rgb"`, `"ar"`), and the components it does not name keep the value they already carried — which is what lets one model hold a per-part RGB tint from generator `color=` *and* an alpha mask written over the top. Without it the two cannot coexist: a `set_all` that sets alpha also sets RGB, and the ladder is per-part while the alpha write is whole-mesh, so it cannot be reconstructed by passing the same `color=` back in. A spelling the parser cannot read fails the op rather than widening to all four.

**`bake_ao` is the one op that MEASURES the accumulated part instead of moving it.** It ray-casts the part against itself and writes the occlusion term into the vertex-colour channels `channels=` names, so a part built from interpenetrating primitives gets contact shading at its junctions - the one signal a tiling detail map structurally cannot produce, because it is a function of UV and cannot know where two solids meet. `radius=` (required, model units) is what makes it a CONTACT term rather than a global darkening; `channels=` is required too, because RGB and A each already carry a signal on a real model. `samples=` (64), `bias_angle=` (15 degrees - a weight rolloff for rays arriving near the surface's own tangent plane, an ANGLE and not a distance offset), `blend=` (`replace`|`multiply`) and `strength=` (0-1) are optional. Its resolution is the tessellation, so a junction crossing the middle of a large triangle is averaged away and the fix is to subdivide there rather than to raise `samples`. The same measurement is on the RPC surface as `geometry.bake_ambient_occlusion`, which additionally returns the per-channel min/mean/max a `.pwmodel` compile has no channel to report - use it once to confirm a radius before committing it to the document.

**`meshTriangleCount` / `meshVertexCount` are the mesh's, not the asset's** — see [`model.compile`](model.compile.md). The bake splits vertices at every normal/UV seam, so the asset's vertex count is larger (exactly the 54 → 144 jump the faceted-shading section above measures), and it drops degenerate triangles, so the asset's triangle count is *smaller*. Read `assetTriangleCount` / `assetVertexCount` from the compile response for the asset's own numbers; they are absent on `model.validate`, which writes no asset to measure.

**`shell` and `bevel` refuse a mesh with no UVs.** `shell` on an *open* mesh fails with `NO_UV_ELEMENTS` unless UV channel 0 covers **every** triangle; `bevel` fails on any mesh with no UV layer or no normal layer at all. That is a crash guard: both engine routines dereference the primary UV overlay without checking it, and the one behind `shell` also reads a specific triangle's UV elements without checking that the triangle is set — so the unguarded call terminates the editor. `append_buffers` with no `uvs=` is what removes the layer — supply `uvs=`, or run a `uv mode=box` op before the modifier. The compiler's own UV fill runs at the merge stage, after every part's ops, so it cannot rescue an op that needs UVs while it is running. An **empty** mesh is exempt: `shell` on a mesh with no triangles succeeds with a warning and changes nothing.

**`bend`, `twist` and `taper` repair the normal overlay before they run.** All three drive an engine space-deformer op, and all three of those ops take a normal-overlay element's *parent vertex* straight into `GetVertex` after checking only that the element exists - which it can, with no parent, whenever an earlier op left an element allocated that no triangle references. The index is unsigned and the bounds test is compiled out, so it reads out of bounds and **kills the editor process** with no diagnostic. The three ops now free those orphans themselves and warn that they did, rather than refusing: the state is legal, engine-produced, carries no geometry, and no op in this format could clear it. The one shape they refuse is a mesh with **no normal layer at all** (`INVALID_NORMAL_OVERLAY`) - add a `recalculate_normals` op first. The deformers' `extent` has nothing to do with it: the span reaches the deforming math only through a value clamped to `[0, 1]`, so an extent far larger than the mesh is ordinary input.

## Re-compiling and provenance

`model.compile` uses the source stamp to decide overwrite. In brief: no target creates and stamps; the **same** source overwrites without a flag; a different or unstamped target, or live `PWSRC_RECOMPILE_UNMANAGED_STATE`, needs `overwrite: true`; a different asset class always refuses. `overwrite` defaults to `false` and is permission, never a mechanism: a same-class asset is rebuilt in place at the same object/address, preserving referencers, so a referenced target is not a refusal reason. Migrating an already-placed unstamped asset takes one `overwrite: true` compile.

The stamp is project-relative when the source is under the project directory and absolute otherwise, so a second checkout at a different root recognises its own output.

`save` defaults to `true`; success carries `saveRequested`, `saved`, `pendingFlush: true` only when a requested save put no `.uasset` on disk, `saveState` (`written` / `alreadyCurrent` / `deferred` / `failed` / `notPersistable` / `notRequested`), and `saveDetail`. Read `saveState`, not `pendingFlush`, before retrying: every non-durable outcome sets the latter, but only `deferred` is fixed by a flush. Full table: [`safe-mutation-save`](safe-mutation-save.md). A failed compile and `model.validate` emit no save report because neither has a created asset to flush.

**Recompiling one source gives the same mesh — measured, on one engine version and one machine.** Verified 2026-08-19 on UE 5.8 by compiling documents twice to two fresh asset paths: identical triangle, vertex and material-slot counts, identical bounds to the last float digit, and identical post-unwrap render vertex counts through an XAtlas unwrap. The compiler contributes no randomness, no clock, no GUID and no unordered iteration that reaches geometry. Three tests pin it.

**One real defect turned up, and it is fixed.** `collision { auto }` generated its hulls in worker-completion order — the engine runs one parallel task per connected component and appends under a lock — so an eight-part document compiled four times produced four different hull orderings with the same eight hulls. Counts and physics could not see it; the `.uasset` bytes could. PinWright now re-sorts generated collision into a canonical geometric order, and the standing test asserts hull order rather than just hull count. Note this was invisible on a single-part document, because one connected component means one task: **measure reproducibility with a multi-part model or you are measuring nothing.** `method=level_sets` is still unsorted.

**Exactly two `uv` fields still inherit an engine default this format does not pin** — `mode=patch_builder`'s `GroupLayer` and `mode=layout`'s `UDIMResolutions`, the two the value grammar cannot express — so an engine upgrade can move those two with your source untouched. Every other field of `patch_builder`, `layout` and `xatlas` is now pinned: the params structs behind them carry a copy of each engine default, which stops an engine change from moving your output silently and makes PinWright's copy the thing to re-check on upgrade. That sentence used to name all three modes as unpinned, which was true while they were called with default-constructed option structs and stopped being true when they were widened. **Mesh element order is not guaranteed** — counts and bounds are what is measured, so do not build a diffing workflow on vertex order. And **`savedToDisk` is not a reproducibility signal**: it reports whether the file on disk moved, so a byte-identical rewrite can report `false`. The full list, including the memory-pressure gate that can fail an otherwise fixed source, is in `docs/pwmodel-format.md` under Reproducibility.

## What `pwmodel 0` does not have

**No variables, loops, functions or expressions, deliberately.** Enumerable geometry is re-editable geometry: enlarging the third of eight holes is a one-line edit against eight explicit blocks, while the same change against `for i in range(8)` means restructuring the loop and re-deriving every other hole. If you need loops, generate the `.pwmodel` text from Python and commit the enumerated result — the artifact stays data.

**`loft` is not in the op table** and reports `PWSRC_UNKNOWN_OP` — left out on purpose. `geometry.loft` does not blend between profiles: it takes each profile actor's world location, derives a *circular* cross-section from the **first** profile's bounding-box extent, and sweeps that one circle through the locations. Shipped under the name `loft` that would promise cross-section blending the code does not have, and the honest version of it is `sweep`, below. A real loft needs a list of profiles — a list of lists of points — and the value grammar still tops out at a list of tuples, so that one is deferred past `pwmodel 0`.

**`sweep`, `extrude_along_spline` and `array_along_path` ARE in the op table**, and they take the path as data rather than as an actor:

```
part rail {
    box size=(6, 6, 6)
    sweep profile=[(0, 0), (10, 0), (10, 40)] path=[(0, 0, 0, 0, 90, 0), (0, 0, 50, 0, 90, 0), (0, 0, 100, 0, 90, 0)]
}
```

**Note the `pitch = 90` on every frame, and do not drop it.** The sweep advances along each frame's local **+X** (see below), and a rotation of pitch `P` sends local +X to `(cos P, 0, sin P)` — so a rail that climbs in world Z needs `pitch = 90` to point the sweep upward. Leaving the frames at `(0, 0, 0)` on a path that runs along Z puts the *path direction inside the section plane*, and the op returns a flat slab instead of a bar. **The triangle count does not move**, so nothing you would normally check catches it: measured, both forms compile `success: true`, `isClosed: true`, `boundaryEdges: 0`, at 26 triangles. It used to pass in silence as well; both ops now warn, opening with `path` — `path frame N sweeps inside its own cross-section plane ...`, plus `path swept a flat result ...` when the sweep is the only geometry in the part. The only two tells are the degenerate count — 2 for the `(0,0,0)` version, 0 for this one — and the bounds, where the broken version has **zero extent on one axis**. Read the corrected sweep's bounds back with [`static_mesh.describe`](static_mesh.describe.md) and it spans x `-40..0`, y `0..10`, z `0..100`; the broken one is flat in x.

`path=` is a **frame list**: `(x, y, z, roll, pitch, yaw)` per frame, part-local, degrees, the same `(roll, pitch, yaw)` ordering as every other rotation in the format. There is no spline and no actor anywhere in the op, which is why `steps` / `segments` do **not** resample the path — write the frames you want. To follow a level spline, sample it with `spline.*` and paste the frames in.

- **The profile lands in each frame's local YZ plane, and the sweep advances along the frame's local +X.** Measured: `extrude_along_spline profile=[(10,0),(10,4),(-10,4),(-10,0)] path=[(0,0,0,0,0,0),(200,0,0,0,0,0)]` produces a bar spanning x `0..200`, y `-10..10`, z `0..4` — so profile `u` is the frame's Y and profile `v` is its Z. The consequence is that **frames left at rotation `(0, 0, 0)` on a curved path are wrong**: the section stays in one world plane while the path turns away from it, and the tube comes out sheared. Give every frame the tangent's own `(pitch, yaw)`. A helix of radius `r` rising `h` per `a` degrees of sweep wants `yaw = theta + 90` and `pitch = atan(h / (r * a_radians))` — `Examples/pwmodel/spiral_stair.pwmodel` writes exactly that for its handrail.
- **`scale_start` / `scale_end` interpolate LINEARLY; `scales=` is how a section follows a law.** Two scalars are a straight ramp between the ends, so a swept tube could taper only straight - a power-law taper, an exponential flare, a waist, none of them reachable along a path. The documented way round it was to split the shape into one sweep per span and match the radii at each join by hand, which is more geometry, more caps buried in each other, and one arithmetic slip from a visible ledge at every joint. `scales=[(alpha, scale), ...]` says it directly: `alpha` is the **normalised position** along the path, 0 at the first frame and 1 at the last (not a distance in uu), and the law is piecewise-linear between knots. The engine always accepted this - both ops build one `FTransform` per frame and its scale slot has always been per-frame; only the caller's two scalars were not.

    ```
    sweep profile=[...] path=[...] scales=[(0, 1), (0.25, 0.72), (0.5, 0.48), (0.75, 0.28), (1, 0.3)]
    ```

    At least **2 knots**, `alpha` **strictly ascending** within `[0, 1]`, every `scale` **positive** - each refused rather than repaired, because each repair would be a guess about which half you meant. The positivity rule is the one worth knowing: `0` collapses the section onto the path and a negative one **reflects** it, which reverses the tube's facing normals while `isClosed`, `boundaryEdges` and the triangle count all stay identical. Outside the outermost knots the law is **held, not extrapolated**, for the same reason - extrapolating a curve that stops at alpha 0.9 is exactly how it reaches a negative scale at 1.0. `scales` beside `scale_start` or `scale_end` is an **error**: only one can be honoured, and accepting the pair would leave the scalars looking set while the curve silently won. The two-knot curve `[(0, a), (1, b)]` IS `scale_start=a scale_end=b`, so this is a superset and not a second dialect. It does not resample `path=`, and it is dropped with `cap` on a returning `extrude_along_spline` path, where the op warns.
- **`cap` reaches the engine on both ops, and `extrude_along_spline` closes into a loop only when the path returns to its start.** It did not always: `extrude_along_spline` swept every path as a closed loop, so `cap` had nothing to act on at any path length and an open path came back as a tube running from its last frame to its first. Measured then, on the four-point profile above with the same path `[(0,0,0,0,0,0),(200,0,0,0,0,0)]` and each run against a lone `box size=(1,1,1) at=(0,0,-500)` so the box's own 12 closed triangles were the only other contribution: `extrude_along_spline` gave **20 triangles, 8 boundary edges, not closed**, where `sweep` gave **24 triangles, 0 boundary edges, closed** (8 sides plus two 2-triangle caps), and `cap` omitted was byte-identical to `cap=true`. Both ops now cap an open path. **To sweep a ring, write the first frame's coordinates again as the last** — within 0.01 uu is enough. On a closed path `cap`, `scale_start` and `scale_end` do nothing, because a loop has no ends, and the op says so in a warning rather than leaving it to be found.
- **`sweep` and `extrude_along_spline` APPEND.** The swept surface is added to the geometry already in the part. Neither may open a part, and the primitive in front of it is still in the mesh afterwards — which is usually what you want for a rail on a post, and a surprise if you expected a replacement. Put the sweep in its own part when you want it alone.
- **Without `profile=` the cross-section is a circle sized from the accumulated mesh's bounding box** — never its outline. `profile=` takes the same `[(u, v), …]` list `revolve` does, minimum 3 points, and is the way to get the section you drew.
- **They take `material=`, and untagged they inherit the slot of the geometry they append onto.** These two are the only modifiers that carry the tag, because they are the only ones that produce triangles of their own. `material="<Slot>"` resolves through the model-wide table exactly as on a shape primitive; left off, the swept surface joins whatever the part already had, which costs no slot and leaves `materialSlots` unchanged. **When the geometry it appends onto carries more than one slot there is no single answer**, so the op takes the slot most of that geometry is on and raises `PWMODEL_MODIFIER_MATERIAL_AMBIGUOUS` naming the candidates; the same code covers a sweep with nothing in front of it, whose triangles keep ID 0. **This used to have no answer at all** — the appended triangles kept material ID 0, which is the *model-wide first slot* and therefore another part's: part `a` tags `Alpha`, part `b` tags only `Beta` and sweeps, and the swept rod came back on `Alpha`, on a green compile with the right slot count and no diagnostic. The workaround was to order the parts so the wanted slot fell at index 0, which breaks as soon as a part is added above it. Separately, `set_vertex_color set_all=true` is still the only route to a vertex colour on swept geometry, since the op takes no `color=`.
- **`array_along_path`'s frame list is the count.** N frames, N copies — a leading `(0, 0, 0, 0, 0, 0)` keeps one where the original stood rather than adding a fourth body at the origin. Same rule as `array_linear`, whose original occupies the first of its `count` positions.
- **A frame places the accumulated mesh's ORIGIN**, not its centre and not its bounds. A shape authored around its own origin lands where the frame says; one authored with its origin partway up it — a crystal whose foot is 16 below the origin, a post whose base is at z = 0 — lands that far off, and on `array_along_path` that is the difference between copies sitting on a surface and copies buried inside it. Offset the frames by whatever the generator's own `at=` already applied.
- **A frame list has to fit on one line.** `A tuple or list may not span lines` is a grammar rule for the whole format, but it bites hardest here: a frame is six numbers, `array_along_path` takes up to 100 of them and `extrude_along_spline` up to 257, so a real path is a very long single line. Breaking it across lines fails with `PWSRC_UNEXPECTED_TOKEN` naming a missing `,` or `]`. Generate the line rather than typing it once it is more than a handful of frames.

`sweep` with no `path=` falls back to a vertical sweep through the mesh's own bounding box. That fallback used to be flat for the reason above — its own frames pointed across the path rather than along it — and now encloses volume at the same triangle count. A `path=` that is present but shorter than 2 frames is an error rather than a silent fall back to that shape, and `extrude_along_spline` requires `path=` outright — it has no fallback and produces nothing without one.

**A swept tapered trunk avoids the seam workaround.** Stacking truncated `cone` ops can approximate it, but seams show; use `revolve` when the shape is a surface of revolution and one profile describes it.

`use skeleton from "<asset path>"` and `skin { smooth }` are implemented: they bind parts to a skeleton and solve smooth weights. The `skeleton` and `animation` *blocks* remain signposts - they parse cleanly and are then rejected with `PWMODEL_CONSTRUCT_IN_WRONG_FORMAT`, naming the format that owns them and its documentation page (`docs/pwskel-format.md`, `docs/pwanim-format.md`). Animation authoring belongs to the separate `.pwanim` format, so `animation` is not a `.pwmodel` `use` kind.

`pwmodel 0` carries no compatibility promise. The grammar may change and existing files may need regenerating until it bumps to `1`.

## See also

- [`model`](model.md) — the namespace page: availability, the one-file-one-asset rule, why a source file rather than more geometry verbs.
- [`model.examples`](model.examples.md) — thirteen worked documents in `Examples/pwmodel/`, indexed by the technique each teaches, plus [`model.examples.op-coverage`](model.examples.op-coverage.md) for what they exercise and the measured reason for what they do not.
- [`model.explicit-geometry`](model.explicit-geometry.md) — the machine-generated tier: `procedural_mesh`, the append / delete / `set_vertex_position` ops and `convex points=`, why they are the format's only indexed constructs, and the traps that come with them.
- [`model.vertex-color`](model.vertex-color.md) — slot versus tint, the vertex-colour material the examples expect, the 8-bit quantization limit, and why an unloadable binding is a warning on both `model.validate` and `model.compile` rather than a failure.
- [`model.describe_ops`](model.describe_ops.md) — the generated op and parameter vocabulary.
- [`model.validate`](model.validate.md) and [`model.compile`](model.compile.md) — the two verbs this loop runs on.
- [`static_mesh.describe`](static_mesh.describe.md) and [`render.capture_asset_preview`](render.capture_asset_preview.md) — reading the compiled asset back.
- [`geometry`](geometry.md) — the imperative path, for shapes the format cannot express.
- [`visual-review`](visual-review.md) — capturing a compiled asset, and the one-scene rule for comparing two of them.
- [`visual-review.model-rig`](visual-review.model-rig.md) — the diagnosis order for a suspected geometry defect, the neutral and UV-checker review materials, the light rig, and what the asset preview's own lighting does to each artefact class.
- `docs/pwmodel-format.md` in the plugin folder — the normative grammar, semantics and full `PWMODEL_*` diagnostic catalog. A maintainer document shipped beside the plugin, not a wiki page.
