# model

Compile a `.pwmodel` text recipe of geometry operations into exactly one Static Mesh asset. Use this namespace for durable, re-editable source; use `call("geometry")` to mutate a placed `DynamicMeshActor`.

## Authoring a document

**Start at [`model.authoring`](model.authoring.md)** for the minimal file, structure, spaces, materials, collision, and workflow: `model.describe_ops` → write → `model.validate` (inline `text`, creates nothing) → `model.compile` (requires `filePath`) → verify with [`static_mesh.describe`](static_mesh.describe.md) and [`render.capture_asset_preview`](render.capture_asset_preview.md) without spawning.

**Spawning is the exception.** Use it to compare variants or judge context; read [`visual-review`](visual-review.md) first. A level adds lighting, ground, and a default material absent from the asset preview. In one five-artifact investigation, three were that material's world-space grid and two were shadows; all were initially filed as geometry defects.

Diagnostics are structured (`severity`, `line`, `column`, `code`, `message`, owning part, suggestions); match `code`, not prose.

## Availability

`model.*` requires the **GeometryScripting** engine plugin, like `geometry.*`. When disabled, methods are unregistered and calls return `PLUGIN_DISABLED`; enable it and restart the editor.

Maturity is **experimental**: `pwmodel 0` has no compatibility promise and may require regeneration until version `1`.

## Why a source file rather than more geometry verbs

A mesh cannot be decompiled into its operations; triangles do not remember “box minus sphere.” Each `geometry.*` call mutates in place and leaves its recipe only in the transcript, so later edits start from triangles.

A committed `.pwmodel` is the recipe: enlarging the third of eight holes means editing one line and recompiling. It is data, not code; it deliberately has no variables, loops, or functions.

## One file, one asset

A `.pwmodel` is one model. `part` blocks group ops, apply one transform, and tag material slots, but all parts merge into one `UStaticMesh`; no construct emits a second asset.

`outputPath` is one `/Game/…` **asset** path, never a directory. The result has one `assetPath`; per-part counts are diagnostic only.

## Keep source beside the generated asset

Recommended layout: `Content/<Rel>/<Name>.pwmodel` beside `Content/<Rel>/<Name>.uasset`. Unreal's asset registry recognizes `.uasset`/`.umap`, not `.pwmodel`; extension-based Git LFS rules leave the source diffable text.

With that layout, `<ProjectDir>/Content/<Rel>/<Name>.pwmodel` derives `/Game/<Rel>/<Name>` and `outputPath` is optional. Otherwise pass it explicitly. A non-derivable omission fails `SOURCE_OUTPUT_PATH_NOT_DERIVABLE` with the reason; it never guesses. Existing layouts remain supported, but derivation does not grant overwrite permission.

## Provenance and overwrite

Every generated asset is stamped with source path and compiled semantic state. Recompile is free only while the live asset matches that baseline or the new source describes the live change. If editor/typed changes would be discarded, compile stops before touching it with `PWSRC_RECOMPILE_UNMANAGED_STATE`. A differently stamped or unstamped target also refuses unless `overwrite: true`; takeover compares live state with incoming source and names changed/omitted values.

`overwrite` grants **permission**, not a different mechanism. It permits discarding inventoried out-of-band state, still returned as a warning with the same code; without it, refusal includes the state-loss list. An existing mesh rebuilds **in place**, preserving referencers; omitted state is not copied forward. A different asset class is always refused.

Treat the asset as **derived**. Editor, `python.execute`, or `geometry.*` edits are lost on next compile and cannot be lifted into source; move them into `.pwmodel` or stop compiling to that path.

## What the format cannot express

`loft` has no op and returns `PWSRC_UNKNOWN_OP`. `geometry.loft` does not trace profile outlines: it reads profile actor world locations, derives a **circular** section from the *first* profile's bounding-box extent, and sweeps that circle. The honest equivalent is `sweep path=[…]` with per-frame rotation and an authored section. A real loft needs a list of profiles (lists of points), which the value grammar cannot hold.

`sweep`, `extrude_along_spline`, and `array_along_path` are in the op table. They take literal frames `path=[(x, y, z, roll, pitch, yaw), …]`; no spline/actor exists, so `steps` does not resample. Without `profile=`, `sweep` and `extrude_along_spline` use a circle sized from the accumulated mesh bounding box, and both APPEND rather than replace.

Faceted shading uses `split_normals split_angle=0`; there is no `set_per_face_normals` op. See [`model.authoring`](model.authoring.md).

## See also

- [`model.authoring`](model.authoring.md) — file structure, spaces, materials, collision, and known gaps.
- [`model.examples`](model.examples.md) / [`model.examples.op-coverage`](model.examples.op-coverage.md) — thirteen worked files and measured op coverage.
- [`model.explicit-geometry`](model.explicit-geometry.md) — explicit buffers and index-addressed edits.
- [`model.vertex-color`](model.vertex-color.md) — material slots, `color=`, and the material required for non-grey vertex color.
- `docs/pwmodel-format.md` — normative grammar and diagnostic catalog; `docs/pwmodel-design.md` — design rationale.
- [`geometry.convert_to_static_mesh`](geometry.convert_to_static_mesh.md) — imperative bake; [`static_mesh.describe`](static_mesh.describe.md) — compiled bounds, slots, and collision.

### model.compile

**Tick-unsafe and gated.** The verb creates a package, builds a `UStaticMesh` (render fences plus `FlushRenderingCommands` on its own stack), and synchronously writes `.uasset`; it may run one 0.1s subsystem tick later. Concurrent requests are serialized and do not interleave, but each lands at its received frame; a compile crash takes the editor and other in-flight work with it.

**`filePath` is required; inline `text` is rejected here.** Use `model.validate` for a string. An RPC-only asset has no recoverable source and would stamp an empty path, defeating provenance.

`filePath` is any filesystem path and is not passed through the `/Game/` sanitizer. A **relative** path resolves against the project directory, not the editor process working directory (the engine `Binaries` folder). The resolved path is stamped, so two spellings cannot create two source identities.

`outputPath` is one asset path and is optional only with the beside-source mapping. `overwrite` (default `false`) is required for an unstamped/differently stamped target or accepted `PWSRC_RECOMPILE_UNMANAGED_STATE` loss. It grants permission, not a different mechanism: existing meshes rebuild in place and keep referencers. `save` (default `true`) persists; the response carries `saved` / `pendingFlush` and `saveState` / `saveDetail`. With `save: true` the compile is refused **before anything is built** while a PIE session holds the editor — `PIE_ACTIVE`, `saveState: "blockedByPie"`, `pieWorlds` naming the session — because a rebuilt mesh whose write is refused would leave the loaded asset ahead of its `.uasset`. Retry when the session ends, or pass `save: false` to build in memory only.

**Diagnostics are bounded; `diagnosticSummary` is not.** `crystal_cluster` once returned 17,815 characters against a 10,000-character inline budget, spilled to `Saved/PinWright/HttpResponses/`, and added a file read per authoring iteration; most text was one warning repeated at thirty sites. These parameters shape output only, identically for `model.compile` and [`model.validate`](model.validate.md):

- **`collapseDiagnostics`** (default `true`) folds entries sharing **severity, code, and part**. The entry has `occurrences`, up to 12 line/column pairs in `occurrenceSites`, `occurrenceSitesOmitted` for the rest, and `distinctMessages` when texts differ; `message` is the **first** occurrence's. Grouping is not by message: repeated `PWMODEL_UNUNIONED_OVERLAP` entries name different second ops/lines. Set `false` for every message.
- **`diagnosticLimit`** (default `5`) caps printed **entries**; `0` means all. The default reflects the spill gate's budget, not object size: the MCP result carries the response twice (`structuredContent` and JSON-escaped `content[0].text`), costing about 2.13x its own length. Errors precede warnings at the limit; array order otherwise stays source order.
- **`diagnosticSeverity`** (default `all`) selects `error` or `warning` (the two compiler severities). Unknown values are refused, not widened to `all`.

`diagnosticSummary` always reports `total`, `errors`, `warnings`, `emitted`, `represented`, `collapsedOccurrences`, `suppressedBySeverity`, `suppressedByLimit`, `complete`, and the applied `limit` / `collapse` / `severityFilter`. Identities: `total == represented + suppressedBySeverity + suppressedByLimit`; `represented == emitted + collapsedOccurrences`. **Gate on `diagnosticSummary`, never `diagnostics.length`**; shaping is not cleanliness, and `complete: false` is the decisive flag.

`use skeleton from "<asset path>"` and `skin { smooth }` are implemented. `skeleton` and `animation` *blocks* parse, then reject with `PWMODEL_CONSTRUCT_IN_WRONG_FORMAT`, naming the owning format and documentation page. Animation authoring belongs to `.pwanim`; `animation` is not a `.pwmodel` `use` kind.

**Rebuilding an occupied `USkeletalMesh` keeps sockets and its `USkeleton` binding, but drops source-omitted state.** The reuse path keeps the *same* object and clears only rebuilt data—LOD/source models, materials, reference skeleton, skeleton pointer, and physics asset (`CreateSkeletalMeshUtil.cpp`); it never touches the socket array. End-to-end: two mesh-only plus three inherited sockets survived a source naming none, with geometry changing 912 triangles / 789 vertices to 108 / 109; all five bones and relative transforms were unchanged.

What survives: mesh-owned and inherited **sockets** (`skeleton.list_sockets` distinguishes ownership via `owner`), and the **`USkeleton` binding**, which follows `use skeleton from`. What does not: **material bindings unless restated** (otherwise one unbound `Default` slot), and **morph targets**; the physics asset is captured and re-attached.

Rebuild losses appear in **`clearedFeatures`**: morph-target names, physics-asset path, and previously bound materials not rebound by new source. An empty array is a measured “nothing lost” claim; gate on it. Material losses are measured after rebuild, so a recompile that rebinds all slots stays quiet.

**`bounds` is on `model.validate` and `model.compile`.** Response and every `parts` entry carry mesh-space `{x, y, z}` `min`, `max`, `size`, and `center`; per-part boxes identify which part reaches an extreme. They are measured on the merged dynamic mesh at the same stage as `health`, so validate-only fitting needs no asset or [`static_mesh.describe`](static_mesh.describe.md). **The box is reduced over the TRIANGLES, not the vertex buffer**, so a vertex a cut left referenced by nothing cannot widen it — a `revolve` whose on-axis apex a `subtract` orphans reports the cut extent, the same number the same solid written as `cylinder` reports and the same one the baked asset measures. **Fitting is iterative:** `noise_deform` displaces along vertex normals by up to `magnitude`, while `harmonic_deform` scales perpendicular radius by up to `1 + Σ amplitude`; reached extent is not the primitive `radius=` or source-derived. The fields match acceptance-test form rather than `static_mesh.describe`'s `origin` + `extent`. With no geometry, bounds are absent, never zeroed.

**`unboundSlots` names slots that use the DEFAULT material, and is the only field that can.** It has one entry per unbound slot: either no `materials` entry names it, or its source path will not load. The latter is otherwise hidden—`materialSlotList` still reports the path, the section renders grey, and `success` is `true`. Both asset creators measured this, but the model pipeline used to drop it. **It appears only when an asset was created**; validate instead reports `PWMODEL_UNBOUND_MATERIAL` for an unbound slot and `PWMODEL_STAGE_WARNING` for an unloadable binding.

**`assetClass` and `skeletal` are OMITTED when class is undetermined.** `use skeleton from` decides it; a parse failing before that declaration has no answer, just as `assetTriangleCount` is absent. A recovered declaration still answers after later failure (for example, line 2 survives a line-40 error). A **complete** parse with no declaration is static even if a later stage fails; an incomplete parse with neither says nothing. They used to default every failure to `assetClass: "UStaticMesh", skeletal: false`, even when source declared a skeleton.

**`materialSlotList` is the slot table in section order; `materialSlots` is its length.** Each entry has `index`, `name`, and `material` (the `materials { }` path, empty when unbound) on both validate and compile. **Read it before recompiling an existing asset.** Slots use model-wide *first-use* order, except the implicit `Default`, which is always last; occupied assets rebuild **in place**, but referencers address sections by **index** (`OverrideMaterials`, section material assignments, LOD section settings). Reordering parts changes the meaning of every index on placed instances; skeletal rebuilds also clear the list first, so `clearedFeatures` can miss renumbering. Six slots before/after looks identical in `materialSlots`; validate is the pre-overwrite check. Names are author names (`Bark`, `Trim`), not `Slot0`…`SlotN`; the engine binds by index, so tools using names see changes on first rebuild.

**Four counts:** `meshTriangleCount` / `meshVertexCount` are the MESH's; `assetTriangleCount` / `assetVertexCount` are the ASSET's. `mesh*` is the merged/per-part `UDynamicMesh` before bake. The bake **drops** degenerates (`bRemoveDegenerates`, default true), reducing triangles, and **splits** vertices for normal/tangent/UV/color seams, increasing vertices. Neither gap is source-derived; read both pairs. [`model.examples`](model.examples.md) has measured counts for thirteen shipped documents. The distinction matters because adding `split_normals` can leave an old response byte-identical and look like a no-op, while a triangle budget may read the wrong pair. `asset*` is LOD0 render data, present only when an asset was written; validate omits it rather than returning `0`. Parts merge before bake, so there is no per-part asset count. Gate on `asset*` or [`static_mesh.describe`](static_mesh.describe.md)'s `verticesByLod`.

**Boolean cleanup telemetry:** after every successful boolean, the compiler welds coincident boundary edges and deletes degenerate triangles with the repair layer's `DeleteOnly` mode. `.pwmodel` coordinates are already Unreal centimetres, so the cleanup length is a named relative fraction of the larger target/tool characteristic extent, clamped to a safe `[1e-6, 1e-3]` window; the triangle-area threshold uses the square of that extent and its own relative fraction, clamped to `[1e-12, 1e-3]`. When a boolean reaches that window, `trianglesBefore` and `trianglesAfter` report the target count around the weld/delete passes, and `sliversRemoved` reports the triangles removed by the degenerate pass. These fields describe the most recent boolean and are omitted when no boolean reached cleanup; `meshTriangleCount` remains the final merged-mesh count. The targeted cleanup keeps compound disconnected `subtract` tools from carrying zero-area slivers into the health audit.

**`health.unreferencedVertices` is what reconciles `meshVertexCount` with `bounds`.** The box is reduced over TRIANGLES, so a vertex a cut or a boolean stranded cannot widen it, while `meshVertexCount` is the vertex buffer and still counts it — two fields describing one mesh and disagreeing about what is in it, correctly, and with nothing naming the difference until this number. Non-zero is a signal in its own right: an op removed geometry and left the leftovers behind, which is the class of fault the triangle reduction was written for. **Reported, never judged** — no verdict reads it, because the bake is driven by triangles so an orphan reaches no asset and changes nothing on disk. Model-wide only; there is no per-part equivalent.

**`health.signedVolume` is the inside-out signal; gate on `health.isClosed && health.signedVolume > 0`.** A uniformly reversed closed mesh renders *identically*: culling shows the wall facing the camera, and `isClosed`, `boundaryEdges`, `degenerateTriangles`, `nonManifoldVertices`, `componentCount`, both `mesh*` counts, and an A/B render stay the same (luminance moved 0.000004). Offline distance fields count backface hits, so inverted winding inverts inside/outside and makes Lumen / DFAO light it as though the camera were inside. `signedVolume` is meaningless on an **open** mesh; `orientationConsistent` / `inconsistentEdges` detect partial inversion only, while uniform inversion leaves `orientationConsistent: true`. **All three are per-part** in `parts[]`: a correct 20-cube (`+8000`) plus inverted 10-cube (`-1000`) looks healthy at model-wide `+7000`. `PWMODEL_EXTRUDE_FACING_OPPOSED` catches the authoring mistake at its line; see [`model.authoring`](model.authoring.md).

**`health.selfIntersections` is the third term of the gate: `health.isClosed && health.signedVolume > 0 && health.selfIntersections === 0`.** The other fields measure connectivity and winding; neither can say whether the surface is the *boundary* of the region it appears to enclose. A `revolve` whose profile ends sit off the axis at the same height caps **both** ends to the axis, leaving a disc across the bore — and the two fans are oppositely wound, so `signedVolume` **cancels exactly** to the figure the intended ring would have had while every other field stays byte-identical to a correct mesh. A sweep whose walls were pushed through each other moves `signedVolume` **smoothly** and flips sign only long after it stopped being a solid. The count is distinct triangle pairs that cross or coincide **inside one edge-connected shell**; pairs sharing a vertex are excluded, and cross-shell pairs are deliberately not counted, because appended parts and siblings interpenetrate on purpose here and have `PWMODEL_UNUNIONED_OVERLAP{,_PARTS}` instead. `selfIntersectingComponents` says how many shells are at fault; `selfIntersectionsTruncated` says the count is a floor. All three are **absent, never zeroed**, when the measurement is declined (it builds an AABB tree per shell and is skipped above a triangle budget) — an absent field fails `=== 0`, which is the correct way round. Not reported per part: a crossing *between* two parts' shells is not a fault. `PWMODEL_SELF_INTERSECTING_SURFACE` carries a witness coordinate, since an interior surface is invisible in every render.

**Live render consumers.** A compile onto an occupied path uses the shared safe-point/render guard.
It enumerates matching registered, render-state-created `UStaticMeshComponent`s and
`UNiagaraComponent`s whose enabled emitter mesh-renderer properties explicitly list the target
mesh; valid dynamic bindings are conservative may-reference candidates. A scoped
`FComponentRecreateRenderStateContext` releases and restores each matching component around
Build/PostEditChange, with render-command flushes before and after the callback. If any consumer
remains render-state-created, compile is refused with `MESH_REBUILD_CONSUMER_NOT_QUIESCABLE`
naming them, and nothing is built or saved. Delete or deactivate those components and compile
again. A first-time compile onto an unoccupied path skips the scan entirely.

**Live render consumers, materials, and what validate names.** Alongside `PWMODEL_UNBOUND_MATERIAL`
for an unbound slot and `PWMODEL_STAGE_WARNING` for an unloadable binding, validate reports
`PWMODEL_IMPLICIT_DEFAULT_SLOT` when an untagged generator opens `Default` in a document whose
`materials` block does not bind it — naming the generator and the index `Default` took. The slot is
placed last, after every slot a tag names, so nothing the block declares is renumbered by it.

### model.validate

Takes exactly one of `text` **or** `filePath`; both are rejected. It performs full parse/semantic validation and **creates nothing**, making it the pre-file iteration surface.

A clean validate does not guarantee compile: it runs the same grammar, semantic, and engine-operation stages but skips asset creation. Degenerate booleans and empty hulls therefore surface here; UStaticMesh/USkeletalMesh bake, package-write, and asset-read-back failures may appear only in compile.

**Material bindings resolve here too.** A bound path that will not load returns `PWMODEL_STAGE_WARNING` with the same sentence as compile, anchored to the binding line; compile has no position and uses -1. It remains a warning and the asset writes with the default material.

**A boolean that changed nothing is an error, not a warning.** "Changed nothing" means the accumulated geometry came back with the **same triangle count and the same enclosed volume**. Triangle count alone is not the test and never was sufficient — a through-cut turns a box into a smaller box, so a cut that removed two fifths of the material used to abort on an unsubdivided operand. The diagnostic names both operands' bounds and their separation, and says whether the two bounding boxes actually miss each other or overlap while the solids do not. For genuinely disjoint operands the engine returns the target unchanged; `PWMODEL_BOOLEAN_NO_EFFECT` aborts the part, so validate fails and compile writes nothing. It used to warn: twelve broken examples compiled green, including `pipe_junction`, whose two `subtract` bores reported “changed nothing” while compile returned `success: true` for a pipe with no hole. An **engine refusal** is distinct but also fatal: disjoint `intersection` or a `subtract` whose tool swallows the part returns `PWMODEL_OP_FAILED` with `[BOOLEAN_FAILED]` and engine text, not `PWMODEL_BOOLEAN_NO_EFFECT`. Before the ops layer passed `UGeometryScriptDebug`, that refusal was invisible and the op reported success on an unchanged mesh. `allow_empty_result=true` accepts the empty result; the part is then named by `PWMODEL_EMPTY_MESH` one stage later.

Warnings do not fail validation; read them for remaining footguns: an unbound material slot, `bevel` without polygroups, compiler-filled UV channels, or a segment count clamped to a primitive floor. `bevel` validates each selected polygroup edge by its exact topology ID and ordered mesh-edge span; it skips invalid spans, spans containing a mesh-boundary edge that the engine cannot bevel, and output already touched by an earlier bevel. Internal open spans remain eligible even when an endpoint is a boundary vertex. It also compares self-intersections before and after the operation: if the bevel introduces any, its output is discarded, the input mesh is restored, and a line-anchored `PWMODEL_STAGE_WARNING` names the bevel. `diagnosticLimit` / `diagnosticSeverity` / `collapseDiagnostics` from [`model.compile`](model.compile.md) apply with the same defaults and `diagnosticSummary`.

`model.validate` runs real engine ops on real meshes; only asset creation is skipped, so a compile crash can also crash validate. Two cases now refuse instead: `shell` on an **open** mesh whose UV channel 0 misses a triangle, and `bevel` with no UV layer. Both return `NO_UV_ELEMENTS` inside line-anchored `PWMODEL_OP_FAILED`. `append_buffers` without `uvs=` removes the layer; provide `uvs=` or `uv mode=box` first.

Generator/part UV disagreement is repaired by box-projecting the missing side at append, so `plane` plus UV-less `append_buffers` is covered. This applies to **generators only**: in-place triangle ops `bridge` and `edge_split` can leave a partial layer, and merge-stage fill may be too late for a UV-dependent modifier. Each repair returns once per model as `PWMODEL_UV_CHANNEL_FILLED`; its third clause means authored UVs were replaced. `bend`, `twist`, and `taper` also free unreferenced normal-overlay elements before engine deformation (otherwise an out-of-bounds parent vertex can crash the editor); they warn when freeing any and refuse only no-normal meshes with `INVALID_NORMAL_OVERLAY`, fixed by `recalculate_normals`.

Per-part packing does not create a shared atlas: each part's `uv mode=layout`, `xatlas`, or auto-packed `patch_builder` sees only that part and can occupy the same 0–1 space as every sibling. Use part-level `patch_builder auto_pack=false` to create islands, then `uv_layout channel=N [texture_resolution=M]` at model level to normalize world-space density and pack the merged mesh; the gutter resolution defaults to 1024 and accepts 2–16384, and a packer refusal fails the statement. After this stage, `PWMODEL_UV_OVERLAP_ACROSS_PARTS` reports positive-area overlap across parts on every populated channel: ordinary channels warn, while the channel named by `lightmap` fails validation. Edge and point contacts stay quiet. An absent, partial, or topologically invalid declared lightmap channel fails with `PWMODEL_LIGHTMAP_UV_INVALID`.

### model.describe_ops

Without `op`, returns compact indexes: one row per parser entry with `name`, `context`, and `parameterCount`, plus indexes for the three parameter sets. With `op`, returns description, flags, and each parameter's type, default, allowed values, and inclusive `min` / `max` where bounded (`channel` is 0-7). An op legal in both contexts returns both entries; `index` distinguishes the shapes.

The response has `ops` and `paramSets`. The latter describes the three non-op `key=value` lists: `uv_layout` and `lightmap` (outside parts), and `part` (`at=` / `rotate=` / `scale=` before `{`). Index rows have `name` / `context` / `parameterCount`; details add `description` / `params` / flags. `{ op: "uv_layout" }` and `{ op: "lightmap" }` return empty `ops` and the matching complete parameter set.

Call it before authoring: it reads the parser's static map, so it cannot describe an uncompilable op. `docs/pwmodel-format.md` deliberately has no separate per-op list to drift.

`context` separates part ops from collision entries, so `box` appears twice with different parameters. Boolean ops have `acceptsMaterial: false`: they discard their tool, and `material=` is an error.
