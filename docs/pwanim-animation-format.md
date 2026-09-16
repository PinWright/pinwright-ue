---
type: system
summary: Plan for the .pwanim animation source format: decisions D1-D7, grammar, the shared text core with pwmodel, the bake-and-write path, dependency order, the T1-T15 test strategy, and what cannot be built yet
date: 2026-08-26
tags: [pwanim, animation, format]
---

# `.pwanim` — animation source format

Scope note per the coordinator's directive: no milestones. Ordering below is **hard dependency only**. Where two items are independent, that is stated.

## Verification of the brief

| Claim | Verdict | Evidence |
|---|---|---|
| `animation <name> { … }` parsed, brace-checked, rejected | True | `PwModelParser.cpp:2544-2578` (`ParseReservedBlock`, body discarded via `SkipBalancedBlock`); `PwModelAst.h:121-132`; `PwModelCompiler.cpp:1929-1952`, milestone string at `:1944` |
| No RPC writes bone keyframes | **True** | `SetBoneTrackKeys` has 5 call sites, all tests: `TestPoseSearchHandlers.cpp:102`, `TestMotionMeasure.cpp:131`, `TestAnimationHandlers.cpp:229`, `TestAnimSequenceDumpBuilder.cpp:273,276`. `animation.authoring.add_bone_track` (`AnimationAuthoringHandler_Sequence.cpp:376-420`) calls `AddBoneCurve` and stops — it creates an **empty** track and reports success |
| `save:true` means two different things | True | `AnimationAuthoringHelpers.cpp:183-194` marks dirty + `FAssetRegistryModule::AssetCreated`, writes nothing; `AssetUtils.h:189` `SaveAssetToDiskReportingPresence` writes and gates on a freshness probe |
| `sequencer.add_section` cast display frames to ticks | True, **and already fixed in the working tree, uncommitted** | `git diff --stat` = 7 insertions / 2 deletions on `SequenceHandler.cpp`; the fix and its `4.17 s vs 0.0042 s` comment are at `SequenceHandler.cpp:2447-2452`. The weak test is `TestSequencerHandlers.cpp:920-931` — it asserts only `InvokeHandler(...)` returned true, weaker even than `Sections->Num() == 1`. A real regression test landed at `TestAddTrackIdentifier.cpp:48-58`, and it re-derives the expected tick from the MovieScene's own `GetDisplayRate()`/`GetTickResolution()` rather than copying the handler's arithmetic — that is the pattern this plan's time tests follow |
| Nothing started; `Private/Model/` is pwmodel-only | True | 9 files, all `PwModel*` |

---

## Decisions

### D1 — Separate `.pwanim` file, argued from ADR 0001

Settled by the user; the argument, because the plan has to survive being re-litigated:

A `.pwmodel` compiles to one `UStaticMesh`. An `animation` block inside it must produce a `UAnimSequence` — a second `.uasset` from one file, which ADR 0001 forbids outright (`docs/adr/0001-one-file-one-asset.md:1-9`). The three escape routes all fail on the ADR's own reasoning:

- *Let a document with only `animation` blocks compile to a `UAnimSequence` and no mesh.* Then `model.compile`'s single `outputPath` names an asset whose **class depends on the file's contents**, `PWMODEL_NO_PARTS` becomes conditional, and the provenance stamp's "recompile this source" relation stops being typeable. That is not one-file-one-asset; it is one-file-one-asset-of-unpredictable-kind.
- *Allow N `animation` blocks.* N assets from one file — the rejected option verbatim (`0001:13-17`), including its stated cost: "it forces a naming scheme for the extra outputs — derived from part names… none of which survive the part being renamed." The reserved grammar's `animation <name>` **is** that naming scheme. It exists only to serve the shape the ADR rejects.
- *Put animation in the skeletal-mesh file.* `USkeletalMesh` + `UAnimSequence` is still two.

Consequence the ADR already accepted and this discharges: "`use <kind> from "<path>"` is therefore **mandatory infrastructure**, not a convenience" (`0001:24-26`).

**Deliverable in this plan:** the `animation` keyword stays reserved in `.pwmodel`, and its rejection message stops naming a milestone and starts naming the format. `PwModelCompiler.cpp:1944` currently produces *"'animation' is reserved in pwmodel 0. …milestone 3 adds this."* — replace with a pointer to `.pwanim` and to `docs/pwanim-format.md`. Same for `use animation from` at `:1937`. A reserved keyword whose diagnostic names a schedule instead of a destination is a dead end with extra steps.

### D2 — Time is integer frames, and there is no second unit

The `add_section` defect was possible because **two integer-valued units** (display frames, ticks) coexisted and the conversion was omitted. Any format carrying both frames and seconds reproduces it with factor `rate` instead of factor 1000.

**Rule: frames are the only time unit in a `.pwanim` file, they are integers, and the rate is declared once.**

```
timebase rate=(30, 1) frames=48
```

- `frame=` is a **required** parameter on every `key` statement, typed `Integer` (the existing `EPwModelParamType::Integer` = "a Number token that must have no fractional part", `PwModelParser.h:36`), range-checked `0 … frames` inclusive against the declared count.
- Seconds appear **nowhere** in the grammar. There is no conversion to omit, so the error class is *unexpressible* rather than merely *caught*.
- `frame=12.5` is a hard `PWANIM_BAD_VALUE`, not a rounding — the engine's model is a uniform grid (below), so a fractional frame has no representation and would silently snap.
- `rate` is a tuple, both components required positive integers, mapping 1:1 onto `FFrameRate(Numerator, Denominator)`. New param type `FrameRate` so the diagnostic reads *"expects framerate, e.g. (30, 1) or (30000, 1001)"*. One spelling only — no scalar `rate=30` sugar, because two spellings of one value is precisely the `material=` / `material_id=` conflict the format already made an error (`pwmodel-format.md:376`).

**And impossible to miss, as the second layer.** The compile response pairs requested against read-back, the same idiom `meshTriangleCount` / `assetTriangleCount` already uses (`PwModelCompiler.h:75-104`):

| Requested | Read back off the created `UAnimDataModel` |
|---|---|
| `frameRate` | `assetFrameRate` (`IAnimationDataModel::GetFrameRate`, `:180`) |
| `frames` | `assetNumberOfFrames` (`:168`) |
| `keysPerTrack` | `assetNumberOfKeys` (`:174`) |
| — | `durationSeconds`, derived and printed |

A ~1000× discrepancy shows as two numbers disagreeing inside one response. Neither number is echoed from the request.

**Why `key frame=12` rather than positional `key 12`.** Positional is terser and matches `part <name>`. It is rejected: `frame=` names the unit at every single key, which is the strongest available answer to "impossible to miss", and it needs zero extension to the shared statement parser (`identifier key=value…`, `PwModelParser.cpp:2105`), which matters directly for the emitter (D6).

### D3 — Sparse in, dense out; the engine's storage forces it

Verified engine facts, all load-bearing:

- `FRawAnimSequenceTrack` is three parallel arrays — `PosKeys`, `RotKeys`, `ScaleKeys` — with **no per-key time and no tangents** (`AnimTypes.h:852-880`). Keys are uniform samples on the frame grid.
- `NumberOfKeys = NumberOfFrames + 1` (`AnimDataController.cpp:142`, `:247`, `:2093`). A 48-frame clip carries **49** keys.
- Interpolation is **per-sequence, not per-key**: `EAnimInterpolationType { Linear, Step }` (`AnimTypes.h:687-695`), stored on `UAnimSequence::Interpolation` (`AnimSequence.h:316`).

So an authored ease curve has no asset representation. The compiler bakes it. That is exactly what makes `.pwanim` **generative** rather than isomorphic (`pwmodel-design.md:25-32`): the dense array is derived from the recipe and the recipe is not recoverable from it. Per the coordinator, there is **no cap, no threshold and no warning** on key count — the format is export-only and nothing can turn an existing `UAnimSequence` into `.pwanim`, so the "dense blob" classification never applies to a file an author wrote.

Two consequences to build:

1. **`UAnimSequence::Interpolation` stays `Linear` and is not exposed.** With fully dense keys, a `step`-eased segment bakes to a run of identical keys, and linear interpolation between two identical values is that value. Exposing the property would give the author a second, sub-frame notion of interpolation that silently fights the one they wrote. State this in the format doc, because someone will propose exposing it.
2. **Rotation bakes with slerp, not per-component Euler lerp.** `FQuat::Slerp` (`Quat.h:658-661`) normalises and `Slerp_NotNormalized` takes the shortest arc. Euler-lerping `(0,0,350)` → `(0,0,10)` spins 340° the long way. The source's `(roll, pitch, yaw)` goes through `PwModelValueRead::MakeRotator` (`PwModelValueRead.h:178-181`) — the same single spelling of the convention `.pwmodel` uses — then to `FQuat`.

### D4 — Unkeyed channels default to the **reference pose**, not identity

`SetBoneTrackKeys` takes all three arrays and has no "unset". A `bone` block that keys only `rotate=` must still supply 49 positions and 49 scales. Filling them with `FVector::ZeroVector` / `FVector::OneVector` teleports every keyed bone to its parent's origin.

The correct fill is the bone's **local reference-pose transform**: `Skeleton->GetReferenceSkeleton().GetRefBonePose()[BoneIndex]` (`ReferenceSkeleton.h:267`), indexed by `FindBoneIndex` (`:90`). `FRawAnimSequenceTrack` is parent-relative local space, the same space as the ref-pose array, so this is a straight substitution.

This is the single most damaging thing to get wrong and it produces no error at any layer. It gets its own test (T4).

### D5 — `use skeleton from` names an **asset**, and repetition stays one line per file

`.pwmodel` documents `use … from "<path>"` as *"relative to the containing file"* (`pwmodel-format.md:185-188`). A `.pwanim` cannot use that meaning: pointing at a skeleton **source** would require the anim compiler to know where that source's asset landed, and the output path is an RPC argument, not a fact in the file.

So in `.pwanim`, `use skeleton from "/Game/Chars/SK_Hero_Skeleton"` is a `/Game/…` asset path. **Do not sniff** — do not infer the path space from a leading `/Game/`. Diagnose it: `PWANIM_SKELETON_NOT_AN_ASSET_PATH` when the path is not a mounted object path, with the message *"a .pwanim references the compiled USkeleton, not its source"*.

Failure modes, each with its own code (the coordinator asked for these explicitly):

| Code | Condition | What the message must carry |
|---|---|---|
| `PWANIM_MISSING_SKELETON` | no `use skeleton` | anchored at the header line; a `UAnimSequence` cannot exist without one (`UAnimSequenceFactory::TargetSkeleton`) |
| `PWANIM_DUPLICATE_SKELETON` | more than one | both lines |
| `PWANIM_SKELETON_NOT_FOUND` | path does not load | the path as written and as normalised |
| `PWANIM_SKELETON_WRONG_KIND` | loads, is not a `USkeleton` | the class found **and the remedy**: on a `USkeletalMesh`, resolve `GetSkeleton()` and print *that* path — a did-you-mean that costs one call and covers the overwhelmingly likely mistake |
| `PWANIM_UNSUPPORTED_USE_KIND` | `use skin` / `use part` / `use animation` | `skin`/`part` are mesh concepts; `animation` would be a second asset |
| `PWANIM_UNKNOWN_BONE` | a `bone "x"` not on the reference skeleton | `SuggestClosest` over the ref-skeleton bone names. Highest-value diagnostic in the format: today `InsertBoneTrack` emits `ReportErrorf` into the log (`AnimDataController.cpp:1259-1263`) and nothing reaches the caller |

**Cycles are structurally unreachable** because the target is an asset, not a source file, and there is exactly one non-recursive `use`. No cycle machinery ships. Record the reason, so that a future `use anim from` knows it must add one.

**On making the repetition cheap: do not.** A 20-clip character is 20 files each repeating one `use skeleton` line. A project-level default, an include directive, or an output-path-adjacent convention would each break the property that a `.pwanim` file is self-describing — which is the only reason it is a file. ADR 0001 already accepted "a rigged character becomes three or more files" (`0001:23`). What *is* worth building, and is cheap: the compile response echoes the resolved skeleton path and its bone count, so twenty files that got it wrong are visible in twenty responses instead of one silent bind.

### D6 — `pwanim` versions independently of `pwmodel`

`pwanim 0`, its own integer.

- The grammars change for unrelated reasons. A shared number forces every `.pwanim` file to be "regenerated" whenever a mesh op lands.
- `pwmodel 0`'s no-compatibility-promise (`pwmodel-format.md:1334-1347`) is a claim about the *mesh* vocabulary having no real models behind it. `pwanim 0` earns the same status on its own evidence, not by borrowing.
- The shared header parser takes `<keyword>` and an accepted-version predicate as arguments (§ Shared core), so two numbers cost nothing.
- **The header keyword is the format discriminator**, and that is the payoff: feeding a `.pwmodel` to the anim compiler fails on **line 1** with `PWANIM_WRONG_FORMAT` naming both keywords, instead of several stages in with a cascade of unknown-op errors.
- One real coupling: a breaking change to the **shared** value grammar or diagnostic vocabulary bumps both. Record it as the single case; it has not happened.

### D7 — `save: true` resolution

The compiler cannot depend on an ambiguous contract, so:

**For everything the `.pwanim` compiler and the new create library write:** `SaveAssetToDiskReportingPresence` (`AssetUtils.h:189`), reporting `EAssetSaveState` (`AssetSaveState.h:24-48`) on the result exactly as `FPwModelCompileResult::bSavedToDisk` / `SaveState` already do (`PwModelCompiler.h:131-137`). This is the only meaning under which a compile response's `saved` field is a measurement.

**For the ~24 existing `animation.authoring.*` verbs:** behaviour unchanged, claim corrected. `SaveAnimAsset` (`AnimationAuthoringHelpers.cpp:183-194`) gains an out-parameter: `EAssetSaveState::Deferred` when it did its mark-dirty, `NotRequested` when `bShouldSave` was false. Every caller routes it through `AddAssetSaveReport` (`AssetUtils.h:203`), which already emits `{saveRequested, saved, pendingFlush, saveState, saveDetail}`. One mechanical sweep; the lie goes away without re-deciding modal-dialog policy for two dozen verbs inside an animation-format change.

Note for whoever does re-decide it later: the stated reason for mark-dirty is *"UE 5.7+ Fix: Do not save immediately to avoid modal dialogs"* (`AnimationAuthoringHelpers.cpp:180-181`). The geometry verbs force-save through `SaveLoadedAssetThrottled` with no dialogs, so the premise looks stale. That is not this plan's argument to have.

---

## Grammar

```
pwanim 0

use skeleton from "/Game/Chars/SK_Hero_Skeleton"

timebase rate=(30, 1) frames=48 loop=true

bone "pelvis" {
    key frame=0  at=(0, 0, 0)   ease=ease_out
    key frame=12 at=(0, 0, 24)  ease=ease_in
    key frame=24 at=(0, 0, 0)   ease=ease_out
    key frame=36 at=(0, 0, 8)   ease=ease_in
    key frame=48 at=(0, 0, 0)
}

bone "spine_01" ease=linear {
    key frame=0  rotate=(0, 0, 0)
    key frame=24 rotate=(0, -6, 0)
    key frame=48 rotate=(0, 0, 0)
}
```

That is the hand-authored 8-key bounce the brief demanded, in eight lines.

**Statement shapes, all already in the shared grammar:**

- `timebase rate=… frames=… [loop=…]` — a keyword-plus-params line, structurally identical to `lightmap channel=N resolution=M` (`pwmodel-format.md:146`). Required, at most one → `PWANIM_MISSING_TIMEBASE` / `PWANIM_DUPLICATE_TIMEBASE`.
- `bone "<name>" [ease=…] {` — a header with one positional argument then optional params then `{`, structurally identical to `part <name> [at= rotate= scale=] {` (`pwmodel-format.md:154-159`). The name is a **String literal, not an identifier**, because imported rigs carry bone names outside `[A-Za-z_][A-Za-z0-9_]*` and the tokenizer's identifier rule is ASCII-exact by design (`PwModelTokenizer.cpp:9-16`).
- `key frame=N …` — the plain `identifier key=value…` op statement.

**Why `bone` and not `track`.** The design rule is "keep the engine's name unless it describes what is *stored* while the author needs to know what happens to their *input*" (`pwmodel-design.md:124-133`). `FBoneAnimationTrack` is the storage; the author is choosing a bone. `bone` also makes `use skeleton` → `bone` → `key` read as one chain and makes `PWANIM_UNKNOWN_BONE` agree with the block that raised it.

**`ease` vocabulary:** `step | linear | ease_in | ease_out | ease_in_out`. One spelling per concept — no `hold` synonym for `step`. `ease=` on a key governs the segment **from this key to the next**; on a `bone` header it is the default for the block. `ease=` on the final key governs nothing → warning `PWANIM_TRAILING_EASE`, not an error, because it is harmless and is what an author gets from copy-pasting a key line.

**Baking rules, each stated normatively because each is silently wrong otherwise:**

| Situation | Result |
|---|---|
| Before the first key | hold the first key's value (not ref pose — the author keyed it) |
| After the last key | hold the last key's value |
| A channel never keyed on this bone | that bone's **local reference-pose** value, constant across all frames (D4) |
| `loop=true` | frame 0 and frame `frames` must agree per bone within tolerance, else `PWANIM_LOOP_SEAM` naming the bone and the delta. `frames=48` at 30 fps is 49 keys, 1.6 s, with the wrap key duplicated at both ends — the "explicit duplicate wrap key" case `animation.measure_motion`'s `maxSeamRatio` documentation calls correct authoring (`MotionMeasureHandler.cpp:121`) |

**Structural errors:**

- `bone "x" { }` with no keys → **error** `PWANIM_EMPTY_BONE`. This deliberately diverges from `PWMODEL_EMPTY_PART`, which is a warning because it is harmless (`PwModelDiagnostic.h:62-63`). An empty bone track is not harmless: it produces a zero-key track, which `ExtractBoneTransform` logs about and evaluates as **identity** (`AnimSequenceHelpers.cpp:135-141`) — the bone snaps to its parent's origin at runtime. Record the divergence with this reason.
- Document with no `bone` → `PWANIM_NO_BONES` (mirrors `PWMODEL_NO_PARTS`).
- Keys not in ascending `frame` order → `PWANIM_KEYS_OUT_OF_ORDER`. Required for the emitter (a silent sort makes `emit(parse(x)) != x` in a way that surprises) and it catches a real authoring mistake.
- Two keys at one frame on one bone → `PWANIM_DUPLICATE_KEY`.
- Same bone in two `bone` blocks → `PWANIM_DUPLICATE_BONE`.

**Emitter compatibility (D6 of the coordinator's list).** Every construct above is reconstructible from the AST with no loss, because keys are stored as `FPwOp` with a `TMap<FString, FPwValue>` of params exactly as `.pwmodel` ops are (`PwModelAst.h:45-53`) — so "absent" stays distinguishable from "explicitly written default", which the format already depends on for part transforms (`PwModelAst.h:62-64`). The emitter gets `.pwanim` nearly free.

---

## The shared core

This is the largest structural item and the one with the most ways to rot. The seam:

### What is genuinely shared

Moving from `Source/PinWrightGeometry/Private/Model/` to **`Source/PinWright/Private/PwSource/`** — the always-loaded main module.

| Today | Becomes | Note |
|---|---|---|
| `PwModelToken.h` (`FPwModelToken`, `EPwModelTokenType`) | `PwSource/PwToken.h` (`FPwToken`, `EPwTokenType`) | verbatim |
| `PwModelTokenizer.h/.cpp` | `PwSource/PwTokenizer.h/.cpp` | verbatim. It includes `IrCore/IrTextUtils.h`, which is `PINWRIGHT_API` in the main module — the move **removes** a cross-module dependency |
| `FPwModelValue` / `EPwModelValueType` (`PwModelAst.h:18-40`) | `PwSource/PwValue.h` (`FPwValue`, `EPwValueType`) | verbatim |
| `FPwModelOp` (`PwModelAst.h:45-53`) | `PwSource/PwValue.h` (`FPwOp`) | verbatim — name + params + children + position is format-neutral |
| `FPwModelUse` (`:112-119`) | `PwSource/PwDocument.h` (`FPwUse`) | verbatim |
| `PwModelDiagnostic.h` — severity, struct, `ToString`, `PwModelDiagnosticsHaveError`, `JoinPwModelDiagnostics` | `PwSource/PwDiagnostic.h` | see the two changes below |
| `PwModelValueRead.h` | `PwSource/PwValueRead.h` | **verbatim, and this is load-bearing.** `MakeRotator`'s `(roll,pitch,yaw)` → `FRotator(Y,Z,X)` reordering must be the same single spelling in both formats, or a bone's rest offset authored in a `.pwmodel` disagrees with the rotation authored in the `.pwanim` that drives it. The header's own comment already records what a duplicated copy cost (`PwModelValueRead.h:5-11`) |
| `FPwModelParamSpec` (`PwModelParser.h:74-97`), `PwModelParamTypeToString`, `PwModelValueTypeToString`, `SuggestClosest` | `PwSource/PwParamSpec.h` | the parameter machinery, not the op-spec struct |
| Parser cursor + value/param machinery (`PwModelParser.cpp:1381-2153`, `2581-2664`) | `PwSource/PwParseCursor.h/.cpp` | see below |

**`FPwParseCursor` — the extraction.** These members of `FPwModelParserImpl` are format-neutral and move out unchanged: `Peek`/`Check`/`CheckIdentifier`/`AtEnd`/`Advance`/`Consume`/`SkipNewlines`/`SkipToNextLine` (`:1381-1441`); `Emit`/`Error`/`Warn`/`ErrorAtLine`/`WasReportedByTokenizer`/`UnexpectedToken` (`:1443-1495`); `ParseNumberInto`/`ParseTupleBody`/`ParseValue`/`ParseParams` (`:1497-1669`); `ExpectedTupleArity`/`BadValue`/`ValidateRange`/`ValidateValue`/`ValidateParams` (`:1671-1949`); `ForEachBlockEntry`/`ParseOpStatement`/`ParseOpList` (`:2069-2153`); `SkipBalancedBlock`/`SkipUnknownConstruct` (`:2581-2664`). Roughly 800 lines.

`ParseVersionHeader` (`:2606-2647`) moves out **parameterised**: it takes the keyword (`"pwmodel"` / `"pwanim"`) and an accepted-version predicate, and returns an `FPwDocumentHeader { FormatKeyword; Version; VersionLine; }`. `ParseUse` (`:2490-2542`) moves out taking the valid-kinds list.

**What does NOT move: the document parser.** `ParseDocument`, `ParseModelLevelConstruct`, `ValidateDocument`, `ValidateMaterialSlots`, `ParsePart`, `ParseMaterials`, `ParseCollision`, `ParseLightmap`, `ValidateOp` (`:1951-2067`, `:2155-2488`, `:2666-2886`) stay pwmodel's. **The shared core is a cursor plus value machinery, not a document parser.** That is the decision that keeps `FPwDocument` from becoming a union of every format's fields, which is precisely how a shared AST rots.

### The document seam

`FPwModelDocument` (`PwModelAst.h:135-153`) hard-codes `Parts`, `Materials`, `Collision`, `Lightmap`. Do not generalise it. Split:

```
// PwSource/PwDocument.h  — format-neutral
struct FPwDocumentHeader { FString FormatKeyword; int32 Version = -1; int32 VersionLine = 0; };
struct FPwUse           { FString Kind; FString Path; int32 Line = 0; int32 Column = 0; };
```

Each format then composes:

```
struct FPwModelDocument { FPwDocumentHeader Header; TArray<FPwUse> Uses; TArray<FPwModelPart> Parts; /* … */ };
struct FPwAnimDocument  { FPwDocumentHeader Header; TArray<FPwUse> Uses;
                          TOptional<FPwOp> Timebase; TArray<FPwAnimBone> Bones; };
struct FPwAnimBone      { FString BoneName; TMap<FString, FPwValue> Header; TArray<FPwOp> Keys;
                          int32 Line = 0; int32 Column = 0; };
```

`FPwAnimBone` is deliberately the same shape as `FPwModelPart` (`PwModelAst.h:58-70`): a positional name, a header param map, an ordered statement list. The parallel is not decoration — it is what lets one emitter walk both.

### The op-table seam

`FPwModelOpSpec` (`PwModelParser.h:99-131`) carries four mesh-flavoured bools: `bGenerator`, `bAcceptsBlock`, `bAcceptsMaterial`, `bBoolean`. Only `bAcceptsBlock` is neutral.

**Share the parameter machinery, not the op-spec struct.** Each format keeps its own `F<Format>OpSpec` with its own flags and its own table, embedding `TArray<FPwParamSpec> Params`; the core owns `FPwParamSpec`, `ValidateParams`, `ValidateValue`, `ValidateRange`, `BadValue`, `ExpectedTupleArity`, `SuggestClosest`. This is the minimal seam and it leaves `model.describe_ops` **byte-identical** — which matters, because that verb emits `PwModelOpTable::Get()` verbatim and every consumer, including the round-trip test in `TestModelHandlers.cpp`, assumes each entry is legal inside a part or inside collision (`PwModelParser.h:145-153`). Do **not** extend `EPwModelOpContext` with an `Animation` enumerator; do not add animation rows to `PwModelOpTable`.

`.pwanim` gets its own `PwAnimOpTable` with two param sets — `TimebaseParams()` and `BoneHeaderParams()` — plus the one op, `key`. It publishes through a sibling verb, `anim.describe_ops`, built from the same generator so the two cannot drift.

### The diagnostic seam, and a code rename

Two changes to `FPwDiagnostic`:

1. **`PartName` → `ScopeLabel` + `ScopeName`.** `ToString` (`PwModelDiagnostic.h:263-266`) currently hardcodes `(part "x")`. `.pwanim` needs `(bone "pelvis")`. Two fields, printed as `(%s "%s")`.
2. **Fifteen codes move to a family prefix.** `PWSRC_UNTERMINATED_STRING` fired on a `.pwanim` file is simply wrong, and diagnostics are stated to be "the entire authoring UX" (`PwModelDiagnostic.h:7-10`). The shared set: the four lexical codes (`UNEXPECTED_CHARACTER`, `UNTERMINATED_STRING`, `INVALID_STRING`, `INVALID_NUMBER`) plus `MISSING_VERSION`, `UNSUPPORTED_VERSION`, `UNEXPECTED_TOKEN`, `UNCLOSED_BRACE`, `UNKNOWN_OP`, `UNKNOWN_PARAM`, `DUPLICATE_PARAM`, `MISSING_PARAM`, `BAD_TUPLE_ARITY`, `BAD_VALUE`, `BAD_BLOCK` → `PW_*`.

   `pwmodel 0` carries no compatibility promise (`pwmodel-format.md:1334-1347`), so the rename is permitted; and `PinWright.core.pwmodel_diagnostics.DocumentedCodesMatchEmittedCodes` makes it mechanical, since it checks registry ↔ emit sites ↔ doc table in every direction (`TestPwModelDiagnosticCatalog.cpp:10-30`). **This belongs in the emitter agent's core-extraction commit, not here** — that commit already touches every emit site, and doing it twice costs a second sweep. If it does not happen there, `.pwanim` ships with `PWMODEL_*` lexical codes and a documented note that the prefix names the format *family*; that is the fallback, and it is a papercut rather than a defect.

### A breakage the migration must handle

`TestPwModelDiagnosticCatalog.cpp` discovers its scan roots as `Source/PinWright*/Private/Model` (`:62-80`). Moving the tokenizer to `Private/PwSource/` takes four emit sites out of that scan, and the test's "registered but never emitted" direction then **fails on four codes**. Widen the discovery to cover `Private/{Model,PwSource,PwAnim}` in the same commit as the move. Ship a sibling `TestPwAnimDiagnosticCatalog` with the identical three-way comparison against `docs/pwanim-format.md`; without it the new catalog is unenforced from day one, which is exactly the state that let 18 `PWMODEL_*` codes ship undocumented.

---

## The write path

Nothing writes bone keyframes today, so this is built here. It must not be an RPC call from the compiler: `PwModelCompiler.h:5-8` states the compile surface carries no MCP and no JSON, and the pwmodel compiler calls `CreateStaticMesh` rather than a `geometry.*` verb. The animation compiler does the same thing against a library.

**`Source/PinWright/Private/Handlers/Animation/AnimSequenceCreate.h/.cpp`** — the animation twin of `Handlers/Geometry/GeometryAssetCreate.h`, in the main module (which already links `AnimGraph`, `AnimationCore`, `Persona`, `UnrealEd`, `AssetRegistry`, `AssetTools` — `PinWright.Build.cs:21,25,53,70`).

```
struct FPwBoneTrackSpec { FName BoneName;
                          TArray<FVector3f> Pos; TArray<FQuat4f> Rot; TArray<FVector3f> Scale; };

struct FAnimSequenceCreateSpec { FString AssetPath; USkeleton* Skeleton;
                                 FFrameRate FrameRate; int32 NumberOfFrames;
                                 bool bOverwrite; bool bSave;
                                 TArray<FPwBoneTrackSpec> Tracks; };

struct FAnimSequenceCreateResult { bool bSuccess; FString ErrorCode; FString ErrorMessage;
                                   FString AssetPath;
                                   FFrameRate AssetFrameRate; int32 AssetNumberOfFrames = -1;
                                   int32 AssetNumberOfKeys = -1; int32 AssetTrackCount = -1;
                                   TArray<int32> AssetKeyCountPerTrack;
                                   EAssetSaveState SaveState = EAssetSaveState::NotRequested; };
```

`-1` rather than `0` on the asset counts, for the reason `FPwModelCompileResult` already gives: zero keys is a real answer and "not measured" is not (`PwModelCompiler.h:97-104`).

**Ordering the function enforces structurally**, inside one `IAnimationDataController::FScopedBracket` (`IAnimationDataController.h:52-80`), all with `bShouldTransact=false`:

`SetFrameRate` (`:176`) → `SetNumberOfFrames` (`:127`) → per bone `AddBoneCurve` (`:191`) → `SetBoneTrackKeys` (`:238`, the `FVector3f`/`FQuat4f` overload — the raw track's own types, so no double→float narrowing happens invisibly at the boundary).

**The invariant the engine does not check.** `UAnimDataController::SetBoneTrackKeys` validates only that the three arrays are equal-length and non-empty (`AnimDataController.cpp:1429-1440`). It **never compares against `NumberOfKeys`**. An 8-key track on a 48-frame sequence is accepted silently, and then reads two different ways:

- runtime evaluation **clamps** to the last key and holds it for the remaining 41 frames (`AnimSequenceHelpers.cpp:143-145`, `FMath::Min(KeyIndex, PosKeys.Num()-1)`);
- `UAnimDataModel::GetBoneTrackTransform` returns **identity** past the end (`AnimDataModel.cpp:203-217`), which is what `animation.measure_motion` sees (`MotionMeasureHandler.cpp:293`).

So the asset animates one way and measures another, with no error anywhere. `CreateAnimSequence` therefore asserts `Track.Pos.Num() == NumberOfFrames + 1` **before** the call, and re-reads `Model->GetNumberOfKeys()` and each track's key count **after** — the second measurement is what the result reports, not the first.

**Also in the library, both callers sharing it:**

```
FAnimSequenceWriteResult WriteBoneTracks(UAnimSequence*, TArrayView<const FPwBoneTrackSpec>, bool bSave);
```

**RPC verb `animation.authoring.set_bone_track_keys`** — the gap for RPC callers, closed against the same library. Required: `assetPath`, `boneName`, and the key arrays. Response reports `assetNumberOfKeys` and the track's key count read back off the model, plus `AddAssetSaveReport`. It refuses a count mismatch with a typed code rather than writing a track that animates differently from how it measures.

**Provenance.** `UAnimationAsset` implements `IInterface_AssetUserData` with a plain serialised `UPROPERTY TArray<TObjectPtr<UAssetUserData>> AssetUserData` — outside the `WITH_EDITORONLY_DATA` block that closes at `AnimationAsset.h:1044` (declaration at `:1046-1049`, interface at `:1145-1150`). So `UPwModelAssetUserData` (`GeometryAssetCreate.h:49-60`) works unchanged on a `UAnimSequence` and the same-source-overwrites / different-source-refuses rule transfers verbatim. One change needed: `ReadProvenanceStamp` currently `Cast<UStaticMesh>` first (`GeometryAssetCreate.cpp:62-71`); the stamp reader must move to the neutral `IInterface_AssetUserData`. Rename the class to `UPwSourceAssetUserData` while renaming the rest, or leave the name and document that "PwModel" is the family.

---

## What I need from the skeleton/skin section, and when

Not deferrals — hard dependencies within this plan.

1. **A `USkeleton` asset at a `/Game/…` path.** That is all. `.pwanim` does not read the skeleton source, does not care how it was authored, and imposes no ordering on skeleton work beyond "the asset exists before an animation compiles against it". Until skeleton compilation lands, `.pwanim` works against any hand-made or imported `USkeleton`, so this dependency does not block a single line of the format, the parser, the compiler or the tests — the test fixtures build transient skeletons in-process (`TestAnimSequenceDumpBuilder.cpp:66`).
2. **Bone-name stability.** If the skeleton section derives bone names from `.pwmodel` part names, `.pwanim`'s `bone "x"` strings must match those names exactly. Agree the naming rule once and put it in `CONTEXT.md`; a rename convention discovered later invalidates every `.pwanim` file written before it.
3. **`use` path-space consistency.** If skeleton/skin also use `use … from`, the asset-path-vs-source-path rule (D5) must be the same in both, decided once. Two rules would be the `PWMODEL_` / `PWANIM_` prefix problem again in a place with no compiler to catch it.

**From the emitter agent** (which ships ahead in dependency order): the shared-core shape. `.pwanim` needs `FPwParseCursor`, `FPwValue`, `FPwValueRead`, `FPwDiagnostic` and `FPwParamSpec` to exist at their final names. If the emitter's extraction differs from the split above, this section follows it — the split here is a proposal that matches the existing seams, not a constraint on the emitter.

**Independent of everything:** the `save:true` correction (D7), the `SequenceHandler.cpp` / `ControlRigSequencerHandler.cpp` rounding divergence (bug 3 below), and the `TestPwModelDiagnosticCatalog` scan-root widening. None of these depends on any other item here and none blocks any.

---

## Dependency order

1. **Shared core extraction** (emitter agent) — everything below needs `FPwParseCursor` / `FPwValue` / `FPwDiagnostic` at final names. The `PW_*` code rename rides here (§ diagnostic seam). Widen `TestPwModelDiagnosticCatalog`'s scan roots in the same commit or four codes go dark.
2. **`AnimSequenceCreate` library + `animation.authoring.set_bone_track_keys`.** Depends on nothing in the format. Can be built in parallel with 3.
3. **`PwAnimAst.h` / `PwAnimOpTable` / `PwAnimParser`.** Depends only on 1.
4. **`PwAnimCompiler`.** Depends on 2 and 3.
5. **`anim.compile` / `anim.validate` / `anim.describe_ops`**, mirroring `model.compile` / `model.validate` / `model.describe_ops` (`ModelCompileHandler.cpp:412,481,546`) — including the refusal of inline `text` on `anim.compile` for the same provenance reason (`:436-444`). Depends on 4.
6. **`docs/pwanim-format.md`, `docs/pwanim-design.md`, `TestPwAnimDiagnosticCatalog`, `docs/index.md` rows, `CONTEXT.md` glossary entries** (Clip, Bone track, Key, Timebase, Ease). Depends on 5 for the diagnostic set to be final.
7. **`.pwmodel` reserved-keyword message change** (`PwModelCompiler.cpp:1937,1944`) — depends on 6 only so it can name a doc that exists.

---

## Test strategy

Per test: the assertion, and **what would make it unable to fail** — the failure mode the codebase keeps producing (`rpc-design.md:22-40`, and `TestSequencerHandlers.cpp:920-931` as the live specimen).

**T1 — `PinWright.Anim.Compiler.FrameCountReachesTheAsset`.** Compile `timebase rate=(30,1) frames=48`; assert `Model->GetNumberOfFrames() == 48` **and** `Model->GetNumberOfKeys() == 49`, read off the created `UAnimDataModel`, not off the compile result. Also assert `Model->GetPlayLength()` ≈ 1.6 s.
*Unable to fail if:* it asserted the compile result's echoed fields, which are the request. The whole point is the second measurement. Assert 49 explicitly — `frames+1` computed in the test reproduces the bug it is guarding.

**T2 — `…SecondsCannotBeWrittenAsATime`.** A document with `key frame=1.6` must produce exactly one error with the `BAD_VALUE` code at that line/column; a document with `key time=1.6` must produce `UNKNOWN_PARAM` naming `frame`. Assert the code and position, never the message text (`pwmodel-format.md:967-969`).
*Unable to fail if:* it asserted only "compile failed". A `.pwanim` with a typo also fails. Assert the code, the line and the column.

**T3 — `…EightKeysBakeToFortyNine`.** The bounce document above. Assert each bone track's key array length equals 49, and assert the *values*: frame 12 is exactly `(0,0,24)` (an authored key survives verbatim), frame 6 is strictly between 0 and 24 for `ease_out`, and a `step`-eased segment holds its start value at every intermediate frame.
*Unable to fail if:* it only checked the count. 49 identity keys pass a count check. The authored-key-survives-verbatim assertion is the one that catches an off-by-one in the bake index — the highest-probability defect in the whole compiler.

**T4 — `…UnkeyedChannelsHoldTheReferencePose`.** A skeleton whose `spine_01` ref pose has a non-zero, non-identity local translation. A document keying only `rotate=` on it. Assert every one of the 49 position keys equals `GetRefBonePose()[idx].GetTranslation()`, read from the reference skeleton at assert time, not from a literal in the test.
*Unable to fail if:* the fixture skeleton's ref pose were identity — then zero-fill and ref-pose-fill agree and the test passes on the bug. The fixture must be constructed with a deliberately non-identity ref pose, and the test should assert that precondition first.

**T5 — `…KeyCountMismatchIsRefused`.** Call `WriteBoneTracks` directly with an 8-key track on a 48-frame sequence. Assert it returns a typed failure and that `Model` is unchanged.
*Unable to fail if:* it went through the compiler, which cannot produce a mismatch. This must call the library at the seam where the engine is permissive.

**T6 — `…ShortTrackReadsTwoWays`** (characterisation, guards T5's premise). Force an 8-key track onto a 48-frame sequence via the raw controller; assert `GetBoneTrackTransform(bone, FFrameNumber(40))` is identity while `ExtractBoneTransform(RawTrack, …, 40)` returns the 8th key. If a future engine version unifies these, this test fails and T5's justification is re-examined rather than silently stale.
*Unable to fail if:* it asserted only one of the two paths. The finding **is** the disagreement.

**T7 — `…RotationTakesTheShortArc`.** Keys at `rotate=(0,0,350)` and `rotate=(0,0,10)`. Assert the midpoint frame is within 1° of yaw 0, and — the discriminating half — assert it is **not** within 1° of yaw 180.
*Unable to fail if:* it only asserted "the midpoint is between the endpoints". Both slerp and Euler-lerp satisfy that; only the negative assertion separates them.

**T8 — `…UnknownBoneIsDiagnosedWithASuggestion`.** `bone "pelvsi"` on a skeleton carrying `pelvis`. Assert `PWANIM_UNKNOWN_BONE`, the line, and `Suggestions[0] == "pelvis"`.
*Unable to fail if:* it asserted only the code. `SuggestClosest`'s Levenshtein tier is what catches transpositions (`PwModelParser.h:155-159`) and it is the half that regresses.

**T9 — `…WrongFormatFailsOnLineOne`.** Feed a valid `.pwmodel` to `anim.validate`. Assert exactly one error, `PWANIM_WRONG_FORMAT`, at line 1, and assert the diagnostic count is 1.
*Unable to fail if:* it asserted "failed". The value here is that it fails on line 1 with one message instead of cascading — assert the count.

**T10 — `…SkeletalMeshPathNamesItsSkeleton`.** `use skeleton from "/Game/…/SKM_X"` where `SKM_X` is a `USkeletalMesh`. Assert `PWANIM_SKELETON_WRONG_KIND` **and** that the message contains the path returned by `SkeletalMesh->GetSkeleton()`, derived in the test from the fixture rather than hardcoded.
*Unable to fail if:* it asserted only the code. The remedy path is the feature.

**T11 — `…LoopSeamIsMeasuredNotAssumed`.** Two documents, `loop=true`: one whose first and last keys agree (must compile clean) and one whose last key is off by 5 units (must produce `PWANIM_LOOP_SEAM` naming the bone). Both directions in one test.
*Unable to fail if:* only the passing document existed. `rpc-design.md:182` — measure both failure directions.

**T12 — `…CompiledSequencePassesMeasureMotion`.** Compile the bounce, then drive `animation.measure_motion` (`MotionMeasureHandler.cpp:112`) over it through the dispatcher. Assert `pass: true`, a non-zero motion magnitude, and a passing loop-seam verdict.
*Unable to fail if:* it read back through `AnimSequenceDumpBuilder::BuildBoneTracksArrayJson`, which reports the key count the writer just set. `measure_motion` accumulates local tracks into component space through a different subsystem and reports seam, jitter and implied ground speed — the readback the write path cannot fake (`rpc-design.md:91-101`).

**T13 — `…SaveReportsTheStateItMeasured`.** Compile with `save:true`; assert `SaveState == Written` and that `IsAssetSaveStateDurable` agrees with the reported `saved`. Compile with `save:false`; assert `NotRequested` and `saved:false`.
*Unable to fail if:* it asserted `saved == true`. `AssetSaveState.h:12-23` records exactly why a bool cannot distinguish the three non-durable outcomes.

**T14 — `…RecompilingOneSourceReproducesTheSameKeys`.** Compile the same text to two fresh paths; assert every key array is bytewise equal. Mirrors `PinWright.Model.Compiler.RecompilingOneSourceReproducesTheSameMesh` (`pwmodel-format.md:1246`).
*Unable to fail if:* it compared counts. Compare values.

**T15 — `PinWright.core.pwanim_diagnostics.DocumentedCodesMatchEmittedCodes`.** The three-way registry ↔ emit-site ↔ doc-table comparison, in the always-loaded main module for the reason `TestPwModelDiagnosticCatalog.cpp:31-37` gives.

**Fixtures.** Promote `NewTransientSkeletonWithBones` / `NewTransientAnimSequence` out of `TestAnimSequenceDumpBuilder.cpp:66` into a shared `Tests/Assets/AnimAuthoringTestFixtures.h`, named-namespace inline per the Unity-ODR rule. Extend the skeleton builder to take per-bone ref-pose transforms — T4 cannot exist without that.

**Engine-version note to carry forward:** `TestAnimSequenceDumpBuilder.cpp:224-238` records that authoring bone tracks through the controller hard-asserts inside the engine's DDC anim compression on UE 5.3/5.4. Every test here that closes a controller bracket needs the same gate, and the constraint belongs in `docs/engine-version-support.md` as a row against the whole `.pwanim` compile path, not just against one test.

---

## What cannot be built here, and why — technical, not scheduling

- **Per-key interpolation stored in the asset.** `FRawAnimSequenceTrack` has three value arrays and no tangents (`AnimTypes.h:852-866`); `EAnimInterpolationType` is per-sequence with two enumerators (`AnimTypes.h:687-695`). `ease=` is a compiler input with no asset representation, by construction. A future format could expose per-key tangents only by moving off the raw bone track and onto float curves, which is a different asset shape.
- **Any decompile direction.** `pwmodel-design.md:36-37` states there is no decompiler and never will be, and the same reasoning binds harder here: the sparse keys and easing that produced a dense array are not recoverable from it. This is why the export-only property in D3 is a fact about the data and not a policy.
- **`bone "x"` for a bone absent from the reference skeleton.** `InsertBoneTrack` resolves the bone index through `Skeleton->GetReferenceSkeleton().FindBoneIndex` and reports an error when it fails (`AnimDataController.cpp:1257-1263`). There is no path to a track for a bone the skeleton does not have; the format diagnoses it instead of attempting it.

The remaining unmodeled animation surface — notifies, float and transform curves, montages, composites, additive settings, root-motion settings, blend spaces, morph-target curves — is buildable but not in this format's grammar. Sync markers are no longer in that gap: `.pwanim` now authorizes them at clip scope, recompiles replace the authored set idempotently, and the authoring API can replace or remove them. The next likely grammar request is **notifies**, because `AnimationAuthoringHelpers::LinkNotifyAtTime` (`AnimationAuthoringHelpers.h:38`) already encodes the non-obvious rule that `LinkValue`, not `TriggerTimeOffset`, is what every reader reports — so a `.pwanim` `notify` statement would be a thin call rather than new research.

---

## Bugs found

1. **`animation.authoring.add_bone_track` creates a track with zero keys and reports success.** `AnimationAuthoringHandler_Sequence.cpp:376-420` calls `AddBoneCurve` and stops. `AddBoneCurve` seeds no keys (`AnimDataController.cpp:1218-1227`), and a zero-key track evaluates to **identity** with only a `UE_LOGF` (`AnimSequenceHelpers.cpp:135-141`) — the bone snaps to its parent's origin at runtime. There is also no companion verb that could add the keys (finding 2), so the verb's only reachable outcome today is a broken track. It should either seed `NumberOfKeys` ref-pose keys or refuse.

2. **`UAnimDataController::SetBoneTrackKeys` accepts a key count that does not match `NumberOfKeys`, and the resulting asset reads two different ways.** Validation is equal-length-and-non-empty only (`AnimDataController.cpp:1429-1440`). Runtime evaluation clamps to the last key (`AnimSequenceHelpers.cpp:143-145`); `UAnimDataModel::GetBoneTrackTransform` returns identity past the end (`AnimDataModel.cpp:203-217`). So the same asset animates as a hold and measures as a snap-to-identity. Engine-side; the plan guards it at the library boundary and pins it with T6. `TestAnimSequenceDumpBuilder.cpp:273-276` writes 1 key on a 30-frame sequence and is a live instance in this repo's own fixtures.

3. **Two copies of `DisplayFrameToTick` with different rounding.** `SequenceHandler.cpp:277-283` uses `.FloorToFrame()`; `ControlRigSequencerHandler.cpp:164-173` uses `.RoundToFrame()`. The same display frame therefore lands on different ticks depending on which handler the caller reached, and `SequenceHandler`'s own comment claims the choice "matches the transform-track branches' rounding" — which is only true of its own file. `TestAddTrackIdentifier.cpp:53-58` had to re-implement the helper in the test because it is a file-internal static; that is the same duplication in a third place. Consolidate into one shared helper (the `SequencerKeyInterp.h` cluster is the obvious home) and pick one rounding deliberately.

4. **`AnimationAuthoringHelpers::SaveAnimAsset` calls `FAssetRegistryModule::AssetCreated` on every invocation** (`AnimationAuthoringHelpers.cpp:192`), including for assets that already exist and are merely being edited. `AssetCreated` is a creation notification; firing it on an existing asset is at best a redundant registry broadcast. Separate from the `save:true` naming issue (D7) and not fixed by it.

5. **`PinWright.sequencer.add_section.ValidParamsNoCrash` asserts only that the handler was found.** `TestSequencerHandlers.cpp:920-931` — `TestTrue("sequencer.add_section handler found", InvokeHandler(...))`. It sends no `path`, so the handler returns `INVALID_SEQUENCE` and the test still passes. This is weaker than the `Sections->Num() == 1` the brief described, and it is the test that let the ~1000× tick bug ship. The real regression test now exists at `TestAddTrackIdentifier.cpp:48-58`; the vacuous one should be deleted rather than left as apparent coverage. Grep for the `.ValidParamsNoCrash` pattern — `docs/test-organization.md:157` documents it as a convention, and every instance that omits required params is the same vacuous shape.

6. **Dead code in `animation.authoring.set_sequence_length`.** `AnimationAuthoringHandler_Sequence.cpp:361-364`: `if (Params->HasField(TEXT("frameRate"))) { /* Frame rate already set above */ }`. Also, the handler unconditionally overwrites both frame rate and frame count with defaults of 30/30 when the caller omits them, so a call passing only `numFrames` silently resets a 60 fps sequence to 30 fps. `Duration` is computed at `:355` and never used.

7. **The `sequencer.add_section` tick-conversion fix is uncommitted.** `git diff --stat` shows 7 insertions / 2 deletions on `SequenceHandler.cpp` in a working tree with 268 modified paths and 11 commits ahead of `origin/master`. It is correct and it is unlanded; it should not be lost in the churn.

### Critical Files for Implementation
- `Source/PinWrightGeometry/Private/Model/PwModelParser.cpp`
- `Source/PinWrightGeometry/Private/Model/PwModelAst.h`
- `Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryAssetCreate.h`
- `Source/PinWright/Private/Handlers/Animation/AnimationAuthoringHelpers.cpp`
- `Source/PinWright/Private/Handlers/Animation/MotionMeasureHandler.cpp`
