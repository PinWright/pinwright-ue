---
type: system
summary: "PinWright Model (.pwmodel) design rationale: why a generative mesh format is not an IR, the architecture decisions behind the geometry-op extraction and the compiler, the data-not-code stance, CAD-feature-tree prior art, derived-asset ownership, scope boundaries, and technical work areas."
date: 2026-09-03
tags: [pwmodel, model, geometry, static-mesh, file-format, ir, ownership, adr]
---

# PinWright Model — Design

Normative grammar, semantics and diagnostic codes: [pwmodel-format.md](pwmodel-format.md).
Per-op parameters: `model.describe_ops`, generated from the parser's own tables — the `ops` array, the `paramSets` array for `uv_layout`, `lightmap`, and the `part` header, and per-parameter `min` / `max` where the spec carries a range.
This file records *why*, for maintainers who would otherwise undo a decision.

## Why this is not an IR

PinWright's nine IRs are **isomorphic** to their assets. A `UEdGraph` is nodes and pins; BPIR
serialises that same structure; decompile is total, so the text can always be regenerated and is
therefore disposable. That is the whole basis of the Ephemeral IR Invariant
([ir-authoring.md](ir-authoring.md)).

A mesh document is **generative**. `FDynamicMesh3` is vertices and triangles. "Box minus sphere"
is not recoverable from the result — not for want of a better decompiler, but in principle: the
triangles do not remember they were once two solids. The recipe exists only if it was written down.

Three content shapes, three storage answers:

| Shape | Example | The text is | Storage |
|---|---|---|---|
| Generative | `.pwmodel`, CAD feature tree | the only source | durable, git-tracked, hand-editable |
| Isomorphic, structured | BPIR, MGIR, AGIR, CRIR | a view, re-derivable | ephemeral sidecar |
| Isomorphic, dense | heightmaps, mocap, baked skin weights | not something anyone edits | external blob |

Rules that follow, and that a future maintainer must not "harmonise" with the IRs:

- The Ephemeral IR Invariant **does not apply**. `.pwmodel` files are durable project data.
- There is **no decompiler** and never will be. Nothing may be designed on the assumption that one
  arrives later.
- Nothing registers with `IrSidecarRegistry` (`Source/PinWright/Private/Utils/IrSidecarRegistry.h`).
  There is no `pwmodel.txt` asset-dump sidecar, because there is nothing to dump.
- Files live under `Private/Model/`, never `Private/<IR>/`.
- The format is **versioned**, and old parser paths are kept when the version bumps — the exact
  opposite of the IR rule. `pwmodel 0` is the single exception: it carries no compatibility promise
  and ships no migration machinery, because no real model has exercised the grammar yet.

`ir-authoring.md` carries one signpost cross-reference and no rule change. Its wording is load-bearing
for nine real IRs.

## Architecture decisions

- **One source file produces exactly one `.uasset`.** See
  [adr/0001-one-file-one-asset.md](adr/0001-one-file-one-asset.md).
- **Extracted ops return `FOpResult`, never `bool`.** ~146 automation tests observe the geometry
  verbs only through dispatcher JSON, and several exist because a past change silently altered an
  echo field or an error code. A `bool` discards *why* a call failed and forces the wrapper to guess
  a code; `FOpResult` carries the same `ERR_*` value the handler emits today, which is the single
  property that keeps those tests green across the refactor.
- **One `F<Verb>Params` struct per op, shared by both front-ends.** The RPC handler translates its
  published legacy names onto the struct (`width`/`height`/`depth` → `FVector Size`); `.pwmodel`
  maps onto the fields directly. Neither front-end can drift from the vocabulary, because both
  compile against it. `actorName` is plumbing, has no field, and therefore does not exist in the
  format.
- **The compiler builds on transient `UDynamicMesh`, never `ADynamicMeshActor`.** Every geometry
  verb today mutates a level-resident actor found by editor label — labels are not unique, lookup is
  first-hit-wins, and no transaction is opened. A compiler that spawned actors would inherit all of
  that plus a dependency on an open level. Actor-free is also what makes compilation deterministic
  and world-free.
- **Purpose-built tokenizer, not `FIrTokenizer`.** The shared one is IR-shaped and its number scanner
  stops before `e`, so `1e-5` lexes as three tokens. `FIrTextUtils` is reused as-is; it is already
  `PINWRIGHT_API`.
- **Local diagnostic type with severity, line, column, code, part and suggestions.**
  `ir-authoring.md` forbids per-IR severity shapes — that rule binds IRs, and this is not one.
  `FPwModelDiagnostic::ToString` follows `FCRIRParseError::ToString`, the one IR with a canonical
  formatter.
- **Single-build asset creation.** `CreateNewStaticMeshAssetFromMesh` hides `AssetMaterials` and
  `NumMaterialSlots`, hard-codes `bDeferPostEditChange = false`, then calls `PostEditChange()` a
  second time itself — so PinWright builds twice, then patches materials, collision and lightmap onto
  an already-built asset. With `bDeferPostEditChange = true` the returned asset has **never been
  built**, so the real distinction is pre-first-build versus post-first-build, and everything fits on
  the pre-build side with exactly one build.
- **Provenance stamps derive the overwrite rule.** `UPwModelAssetUserData { SourcePath; SourceHash; }`
  is attached at creation through `IInterface_AssetUserData` and survives save/load. The same
  `SourcePath` overwrites freely — that is iteration, and gating it behind a flag trains the habit of
  always passing the flag, which is exactly how the destructive case becomes invisible. A different
  `SourcePath`, or no stamp at all, refuses unless `overwrite=true`. This turns Generated versus
  Detached from a paragraph in a design doc into a mechanism, and answers "is this asset generated,
  and from what?" — the question `docs/map/OWNERSHIP.md` in the host project had to reconstruct after
  the fact.

  **The flag is permission and nothing else.** The mechanism is fixed: a same-class occupant is always
  rebuilt in place, so its referencers survive whichever way the gate goes, and the stamp is the only
  thing deciding anything. Routing `overwrite` into `AssetCreatePolicy`'s delete-then-recreate branch
  as well made the two into one knob and deadlocked them — with the flag the registry refused
  `ASSET_IN_USE`, without it the stamp refused `ASSET_ALREADY_EXISTS`, and each rejection named the
  other as the remedy. Permission and mechanism have to stay separate axes for the stamp to mean
  anything on an asset that is actually in use, which is every asset worth migrating.

  `SourcePath` is stored project-relative when the source lies under the project directory and
  absolute otherwise. Absolute-always mismatches on every second machine. Content-hash matching fails
  for the opposite reason: editing the source *is* the iteration loop, so a hash comparison would
  refuse every real recompile. `SourceHash` is recorded but unused in version 0, so adding a use for it
  later needs no migration.
- **Parts merge; nothing opts out.** A part is a named sub-region of the single output mesh, not a
  sub-asset. Slot identity is the material *name*, so two parts tagging `material="Shell"` share one
  slot, and slot `Default` exists only if some geometry is untagged.
- **`materials`, `collision` and `lightmap` are model-level; geometry ops including `uv` are
  part-level.** `UBodySetup` and `LightMapCoordinateIndex` are per-asset, so per-part collision has
  no coherent meaning.
- **`model.compile` requires `filePath`; inline `text` is accepted only by `model.validate`.** An
  asset compiled from an RPC payload has no recoverable source — the exact failure this format exists
  to prevent, reproduced inside the tool meant to prevent it. It also defeats the provenance stamp,
  since every inline compile would stamp an empty `SourcePath` and therefore match every other one.
  `FPwModelCompiler::Compile` still takes an `FStringView`, so tests and validation are unaffected.
- **The compile surface carries no MCP and no JSON.** No `FJsonObject` and no `FHandlerContext` below
  the handler. That is what keeps a future `UPwModelLibrary` Python wrapper — following
  `UPinWrightPackageLibrary`'s precedent — a thin shim rather than a refactor.

## Data, not code

The format has no variables, no loops, no functions. That reads as a limitation until the tension is
named: **procedural generation and re-editability pull in opposite directions.** A loop is compact to
write and hard to patch; eight explicit blocks are the reverse. An agent asked to enlarge the third
hole edits one line of eight explicit blocks, but has to reason about iteration order, index
arithmetic and every other hole to change `for i in range(8)`.

Re-editability is the entire reason this format exists rather than the 89 imperative `geometry.*`
verbs, so it wins. Enumerable geometry is re-editable geometry.

A Python builder that emits `.pwmodel` text gets both properties. It needs no plugin API, because
the committed artifact is still the text.

## Naming: when the engine's name is kept, and when it is not

Engine names are kept wherever they describe the same thing. `complexity`'s values are snake_case of
`ECollisionTraceFlag` and are not renamed, even though `simple_and_complex` reads like a
contradiction and is not one.

The divergences share one ground: **the engine name describes what is stored, and the author needs
to know what happens to their input.** The list itself is normative and lives in
[pwmodel-format.md § Divergences from engine naming](pwmodel-format.md#divergences-from-engine-naming)
— it is not repeated here. An earlier enumeration on this page was one entry short of the format
doc's from the day both landed, which is the reason there is now exactly one copy.

## Prior art: a CAD feature tree, not a Maya DG

`.pwmodel` is an ordered list of parameterised operations replayed from scratch, where the source is
the tree and the solid is derived. That is a CAD feature tree. The nearest true analogue among
text-first tools is OpenSCAD.

Maya's dependency graph differs on two counts: history lives *in the scene* as live nodes, and it is
a graph with named intermediates you can branch and reuse rather than a linear replay.

The consequence worth writing down is about component references. **Maya names components
positionally (`e[4]`), CAD names them topologically, and both dangle when an upstream feature
changes.** That is why "delete history" is a Maya reflex, and why topological naming is still
unsolved after thirty years of commercial CAD.

This format stores **no component references at all**, so it is immune. The immunity cost
expressiveness: you cannot bevel only the top rear edges. If selection is ever added, prefer
semantic or positional selectors over indices, and expect the dangling problem to arrive with them.

**Tagging the faces a boolean exposes did NOT need selection**, which is worth recording because it
was reserved as `cut_material=` for years on the assumption that it did. A boolean renumbers
triangles wholesale, so *identifying* the new faces afterwards really is the naming problem above —
but the faces never had to be identified. The tool mesh is tagged **before** the operation, and
GeometryCore carries a surviving triangle's material id across; a face the operation invents from
neither operand is found by reserving id 0 across both inputs, so "still 0 afterwards" means
"created here". Both are properties of the operands, not of the result, and neither needs a name
for a triangle. The lesson generalises: before reserving a feature as blocked on component
identity, check whether the same answer can be *carried in* rather than *read out*.

## Ownership of generated assets

A compiled `UStaticMesh` is **derived**. Hand-edits to it — in the editor, or through
`python.execute` and the `geometry.*` verbs — are lost on the next compile and **cannot be lifted
back into the source**, because there is no decompiler. There are exactly two supported responses:

1. Move the edit into the `.pwmodel` source and recompile.
2. Accept the asset as detached and stop compiling it.

This is a documentation decision, not machinery. The provenance stamp detects *which source* an asset
came from; it does not detect that someone edited the result.

The concrete failure it prevents is in the host project. `docs/map/OWNERSHIP.md` records 231 actors
in one generated-effects folder whose original producers — a series of project scripts — were never committed and no
longer exist. A replacement producer had to be reconstructed by reading the live level back, which
only worked because actors are enumerable and carry labels. A baked triangle mesh is neither.

What Python **is** for: generating `.pwmodel` text from loops, maths, or imported data. That is the
intended power-user path, it needs no plugin API, and it keeps the committed artifact a durable
source.

## Scope boundaries

- **Terrain is out.** A heightfield *is* its data — there is no shorter recipe than the samples — so
  it wants raster-delta edits rather than a replay. Regenerating a full field from parameters is a
  known way to silently revert stacked hand edits.
- **World placement is out.** The compiler stays deterministic and world-free; placement is what the
  existing actor and spatial verbs already do.
- **Material graphs are out.** MGIR owns them, and a `UMaterialInterface` is isomorphic to its graph.
  The format *assigns* materials by path; it does not define them.
- **Skeleton, skin and animation are reserved in `.pwmodel 0` but are not implemented here.** A
  document naming them gets `PWMODEL_UNSUPPORTED_IN_VERSION` rather than a parse error. That
  diagnostic is the entire reason to reserve them; it does not promise a schedule.
- **Imported-blob ingest, `UFactory` and reimport paths are out.** A `.pwmodel` file may live at any
  filesystem path, inside the project or not, and is compiled by an RPC verb rather than by the
  editor's import pipeline.

## Current scope and technical work areas

The shipped geometry path covers the geometry-layer extraction (duplicated resolvers and spawn
helpers consolidated, document-meaningful verbs callable without an actor, single-build asset
creation) plus the `model` namespace: `model.compile`, `model.validate`, `model.describe_ops`.

The following are technical work areas. Their ordering is governed only by hard dependencies in the
implementation plan.

### Text emission and migration

**The AST → text emitter is the highest-value addition when this work is undertaken.** One
implementation buys three things:

- a canonical formatter (`model.format`), so diffs stay stable — and a stable diff is the entire
  review mechanism for a git-tracked source;
- the `pwmodel 0 → 1` migrator, which is parse-with-old, emit-with-new;
- a parser idempotence test, `emit(parse(emit(parse(x)))) == emit(parse(x))`.

**An emitter is not a decompiler.** Text → AST → text is pretty-printing. Mesh → text is the thing
that cannot exist. Do not let the emitter's arrival be read as evidence that a decompiler is now
close.

### Skeleton and skin

Skeleton and skin remain reserved in `.pwmodel` until their implementation is complete. Two
constraints are already known from the existing verbs:

- `geometry.bind_skin_weights` is a *solver* with no bone list. It must run last, after every
  geometry op, and it writes the BASE profile only — the `skeleton.*` weight verbs author named
  alternate profiles and are not an alternative to it (`SkeletalMeshHandler.cpp:1053`).
- `geometry.convert_to_skeletal_mesh` is the sole SkeletalMesh creator, and it reuses-and-empties an
  existing asset rather than creating one beside it.

### Animation

The separate `.pwanim` format is blocked on a gap in the current surface, not on format design:
**no RPC writes bone keyframes today.** `SetBoneTrackKeys` has 6 occurrences in the tree, all in test files
(`TestAnimSequenceDumpBuilder.cpp`, `TestAnimationHandlers.cpp`, `TestMotionMeasure.cpp`,
`TestPoseSearchHandlers.cpp`); no handler calls it.

The two clusters also disagree on what `save: true` means. Geometry content-create verbs force a disk
write through `SaveAssetToDiskReportingPresence` (`Private/Utils/AssetUtils.h:178`), while
`AnimationAuthoringHelpers::SaveAnimAsset` deliberately only marks the package dirty and notifies the
asset registry (`AnimationAuthoringHelpers.cpp:183-194`). Resolve that disagreement before
animation authoring claims to have saved anything.

## Related

- [adr/0001-one-file-one-asset.md](adr/0001-one-file-one-asset.md) — the one-file-one-asset decision.
- [pwmodel-format.md](pwmodel-format.md) — normative grammar, semantics, diagnostics.
- [../CONTEXT.md](../CONTEXT.md) — glossary: Model, Part, Op, Hull, Slot, transform spaces.
- [ir-authoring.md](ir-authoring.md) — the IR contract this format is deliberately outside of.
- [arch.md](arch.md) — handler, dispatcher and module architecture the `model` namespace plugs into.
