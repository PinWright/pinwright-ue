---
type: reference
summary: User decisions governing the unified PinWright format plan (pwmodel, pwanim, shared core)
date: 2026-08-20
tags: [pwmodel, pwanim, format, decisions]
---

# Format plan decisions

Settled by the user 2026-08-20. These bind the four plan sections in this directory.
Where a plan section disagrees with a line here, this file wins.

## Structural

**One plan, no milestones.** There is no M2/M3/M4. Everything ships in a single unified plan;
nothing is delayed or postponed. Order work by **hard dependency only** — what genuinely cannot
start until something else finishes — never by milestone, priority or risk appetite.

**A deferral is only legitimate if it is technical.** "Cannot be built, because X" is an answer.
"Later" is not. Items previously recorded as closed for scheduling reasons are back in scope:
general maps, nested `key = { }` objects, and named-resource handles (`simplify_mesh` weight maps,
`uv layout` UDIM resolutions). `cut_material=` is **done** and did not need its own parameter: a
boolean's `material=` names the faces the operation creates. See
[pwmodel-format.md](pwmodel-format.md) "What a boolean does to material slots".

**All formats share one tokenizer / AST / parser core.** `.pwanim` and any future format must not
fork the pwmodel parser. Shared: tokenizer, token types, value system, diagnostic type, `use`
statements, block/brace machinery, the op-table mechanism. Per-format: op-table contents, document
shape, compiler, asset creation. `FPwModelDocument` hard-codes Parts/Materials/Collision/Lightmap
today — that is the seam needing the most work, and the `PwModel` name prefixes need revisiting.

**The formats are export-only.** Source compiles to asset; there is no decompiler in either
direction and never will be. Nothing imports an existing `.uasset` back into source. This is why
no dense-data cap is needed on animation: a 10,000-frame mocap file cannot *become* a `.pwanim`,
because no path creates one.

## Format shape

**Animation is a separate `.pwanim` file**, not an `animation { }` block in `.pwmodel`. A
`UAnimSequence` is its own asset and [ADR 0001](../../docs/adr/0001-one-file-one-asset.md) makes
one-file-one-asset hard. The reserved `animation` keyword in the pwmodel grammar becomes a signpost
whose diagnostic names `.pwanim`.

**One `.pwanim` = exactly one `UAnimSequence`.** A 20-clip character is 20 files, which makes
`use skeleton from "…"` mandatory infrastructure repeated in each — so its cost and its diagnostics
(missing target, wrong kind, cycle) matter more than they otherwise would.

**`pwmodel` stays at version `0`.** A statement about the version *number*, not licence to
postpone grammar work. Existing example files may need regenerating; that is accepted.

## Grammar

**Add a nested list-of-lists value kind**, to unlock a real profile-blending `loft` — the one
genuinely unreachable modelling capability, since `sweep` already subsumes the weak bounding-box
loft that shipped in the RPC layer.

**No named `curve` block.** Inline frame lists stay. This is a design decision, not a
postponement: the format stores no cross-references, and that immunity from the dangling-reference
problem — the one that leaves Maya's `e[4]` and CAD topological naming unsolved after thirty years
— is worth more than deduplicating a frame list. Revisit trigger: real models showing genuine path
duplication. The material Slot table is the sole precedent for a named value, and it works because
it names a *binding*, not a component.

## Animation specifics

**The bone-keyframe write path is in scope.** `SetBoneTrackKeys` appears only in test files today.
Building it also requires settling the `save: true` disagreement it depends on — a forced disk
write in geometry versus dirty-only in `AnimationAuthoringHelpers::SaveAnimAsset` — because a
compiler cannot depend on an ambiguous contract.

**The time model must make unit errors impossible to express or impossible to miss.** Measured
precedent: `sequencer.add_section` cast display frames straight to ticks, so `endFrame: 100`
produced a 0.0042-second section instead of 4.17 — a ~1000x error that shipped because its only
test asserted `Sections->Num() == 1`.

## Ordering

**The AST→text emitter ships first** in dependency order, so it effectively defines the shared
core's shape. It buys a canonical formatter (stable diffs, which is the entire review mechanism for
a git-tracked source format), the version migrator, and a parser idempotence test
`emit(parse(emit(parse(x)))) == emit(parse(x))`.

An emitter is **not** a decompiler: text → AST → text is pretty-printing; asset → text cannot exist.
