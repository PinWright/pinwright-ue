---
type: system
summary: Implementation plan for the pwmodel skeleton and skin blocks and the use-from reference form, with brief verification, dependency ordering, and seven bugs found
date: 2026-08-26
tags: [pwmodel, pwskel, skeleton, skin, format]
---

# `skeleton`, `skin`, `use … from` — implementation plan

## 0. Verification of the brief

| Brief claim | Verdict | Source |
|---|---|---|
| `skeleton`/`skin`/`animation` parsed into `FPwModelReservedBlock`, body discarded | **True** | `PwModelAst.h:125-132`, `PwModelParser.cpp:2544-2578` |
| Rejected at compile with `PWMODEL_UNSUPPORTED_IN_VERSION` naming the milestone | **True** | `PwModelCompiler.cpp:1929-1951` (`use` at :1934, blocks at :1943) |
| `use` parses into `FPwModelUse`, kinds `{skeleton,skin,animation,part}` | **True** | `PwModelAst.h:112-119`, `PwModelParser.cpp:2490-2540` |
| One file → one asset; one `AssetPath`, one `CreateStaticMesh(` call | **True** | `PwModelCompiler.h:63-71`, `PwModelCompiler.cpp:2367` (sole call site) |
| `bind_skin_weights` is a solver with no bone list, must run last, writes BASE only | **True** | `SkeletalMeshAssetIOHandler.cpp:683-693` (no bone param), `:74-116` (ordering trap), `:118-129` (base profile) |
| `convert_to_skeletal_mesh` is the sole SkeletalMesh creator | **True** | only `NewObject<USkeletalMesh>` outside tests is via `CreateNewSkeletalMeshAssetFromMesh` at `SkeletalMeshAssetIOHandler.cpp:1063` |
| …and "reuses‑and‑empties an existing asset" | **Half true — reattribute it.** The verb *refuses* an occupied path (`ASSET_EXISTS`, `:1034-1040`). The reuse‑and‑empty is the **engine's**: `CreateSkeletalMeshUtil.cpp:47-82` finds the occupant, empties `LODModels`, `SetNumSourceModels(0)`, `GetMaterials().Empty()`, `GetRefSkeleton().Empty()`, `SetSkeleton(nullptr)`, `SetPhysicsAsset(nullptr)`, and **`UnregisterAllMorphTarget()`** | as cited |
| A part is the rigid‑binding unit; its transform doubles as the bone pivot | **True as intent**, `docs/pwmodel-format.md:170-173`. Not present in code — `FPwModelPart` (`PwModelAst.h:58-70`) has no bone field |
| M1 asset path is single‑build with `bDeferPostEditChange=true`, materials/collision/lightmap pre‑build | **True** | `GeometryAssetCreate.cpp:251`, `:264-327`, one `PostEditChange()` at `:330` |
| "Establish whether a SkeletalMesh equivalent exists — do not assume symmetry" | **It exists, and it is *not* symmetric — in our favour.** See §6 |

## 1. Decisions

**D1 — `skeleton` becomes its own format, `.pwskel`; one file → one `USkeleton`.**
`UE::AssetUtils::CreateSkeletalMeshAsset` `ensure`s `Options.Skeleton` non‑null and returns `InvalidSkeleton` without it (`CreateSkeletalMeshUtil.cpp:24-27`), and it writes `Options.Skeleton->MergeAllBonesToBoneTree(NewSkeletalMesh)` (`:172`). A `USkeleton` asset must therefore exist before any skinned mesh can be built, and it is a first‑class `.uasset` that many files reference. Given the coordinator's `.pwanim` split on identical reasoning (one file = one `UAnimSequence`), a skeleton document is one file = one `USkeleton`. Its top‑level construct is still a `skeleton { … }` block, so "blocks on a shared core" holds.
Alternative rejected: `skeleton { … }` inside `.pwmodel`. It forces a carve‑out in `ValidateDocument`'s `PWMODEL_NO_PARTS` (`PwModelParser.cpp:2744-2756`), makes one extension produce three different asset classes, and gives a document with a skeleton block plus parts two outputs — which the ADR forbids outright.

**D2 — `skin` emits nothing; it changes *what class* the mesh file's single asset is.**
Skin weights live in the LOD's `FMeshDescription` skin‑weight attribute (`SkeletalMeshAssetIOHandler.cpp:123-129`); there is no separate weights asset in UE. `skin` is therefore model‑level in exactly the sense `collision` and `lightmap` already are (`docs/pwmodel-design.md:97-99`). A `.pwmodel` with `use skeleton` + a binding compiles to one `USkeletalMesh`; without them, to one `UStaticMesh`. One asset either way. **This is the entire answer to "does `skin` emit anything": no, and the ADR is untouched.**

**D3 — the `.pwskel` document declares its own asset path; `.pwmodel` does not.**
`skel.compile` takes `filePath` only. `model.compile` keeps its required `outputPath` unchanged. The asymmetry is forced and defensible: **a skeleton is a shared identity that N files must agree on, so it is named once; a mesh is compiled wherever the author wants it.** Only skeletons are the target of a `use`, so only skeletons must self‑name. Header form: `pwskel 0` then `skeleton "/Game/Chars/SK_Hero" { … }`.

**D4 — `use <kind> from "<relative file path>"` reads the target document, never compiles it.**
Resolution is: read → parse header + the one declaration that names the asset → `LoadObject` that asset. Compiling the target would create a second `.uasset` from one `model.compile` call and make `assetPath` ambiguous. See §5 for the full rule set.

**D5 — the rigid binding rides on the `part` header (`bone="…"`), not on a `skin` entry naming a part.**
Coordinator rule 6: a named value is acceptable when it names a *binding*, not a *component*. `bone=` on the part it binds is the same shape as `material=` — it binds this part to a name resolved in another asset. A `skin { rigid <part> bone=<x> }` table would instead name a **component** (a part in this document), and would dangle on every part rename. Rejected on that ground.

**D6 — bone hierarchy is nested, not flat‑with‑`parent=`.**
`parent="spine_01"` names a component in the same document — the exact shape rule 6 forbids, and it admits cycles, forward references and dangling renames. Nesting makes all three unrepresentable and gives `FReferenceSkeletonModifier::Add` (`ReferenceSkeleton.h:78`) its required parent‑before‑child order for free from a depth‑first walk.

**D7 — the compiler stays filesystem‑free; `use` resolution goes behind an injectable reader.**
`PwModelCompiler.h:15-18` states the `FStringView` signature exists so the pipeline is testable without the filesystem. `FPwUseResolver` takes an `IPwSourceReader { bool ReadFile(const FString& AbsPath, FString& OutText) }`. Production passes a filesystem reader; tests pass an in‑memory map. This is what makes the cycle, missing‑target and wrong‑kind tests *able to fail* without a temp directory.

**D8 — `collision` and `lightmap` are refused when the output is skeletal, not silently dropped.**
`FSkeletalMeshAssetOptions` (`CreateSkeletalMeshUtil.h:47-85`) has no collision input; `LightMapCoordinateIndex` is a `UStaticMesh` property. Silently dropping either is the failure class this codebase repeatedly removes. New codes, message names `skeleton.create_physics_asset` as the path for collision.

**D9 — the compiler calls `UE::AssetUtils::CreateSkeletalMeshAsset` directly, not `CreateNewSkeletalMeshAssetFromMesh`.** See §6.

## 2. Shared core seam

**Module placement is load‑bearing.** `PinWrightGeometry` is `LoadingPhase: None` and only loads when `IPluginManager` reports **GeometryScripting** enabled (`CLAUDE.md` Module Split; `PinWrightGeometry.Build.cs:28-43` hard‑fails without it). A `.pwanim` compiler has no GeometryScripting dependency. **The shared core must live in the main `PinWright` module** at `Source/PinWright/Private/Format/`, exported `PINWRIGHT_API`, or `.pwanim` is unavailable on any host where GeometryScripting is off.

Every candidate file is already engine‑type‑free (`CoreMinimal` + `Misc/Optional` only), so the move is mechanical:

| Moves to `PinWright/Private/Format/` | From | Notes |
|---|---|---|
| `PwToken.h` | `PwModelToken.h` | verbatim |
| `PwTokenizer.{h,cpp}` | `PwModelTokenizer.*` | verbatim |
| `PwValue.h` | `PwModelAst.h:18-40` + all of `PwModelValueRead.h` | `MakeRotator` is the one rotation convention (`PwModelValueRead.h:174-186`) and must be shared, or `.pwskel`'s `rotate=` silently disagrees with `.pwmodel`'s |
| `PwDiagnostic.h` | `PwModelDiagnostic.h` | type + `ToString` + `PwDiagnosticsHaveError` + `JoinPwDiagnostics` |
| `PwOpTable.h` | the *mechanism* half of `PwModelParser.cpp` | `FPwParamSpec`, `EPwParamType`, `FPwOpSpec`, `SuggestClosest`, `ValidateParams`, `ExpectedTupleArity` |
| `PwScanner.{h,cpp}` | `PwModelParser.cpp` scanning machinery | `Peek/Advance/Check/Consume/SkipNewlines/SkipToNextLine`, `ForEachBlockEntry` (`:2069-2093` — the no‑progress guard is the one thing standing between a malformed document and an infinite loop; it must be shared), `ParseValue` (`:1540-1610`), `ParseParams` (`:1619-1665`), `ParseTupleBody`, `UnexpectedToken`, `BadValue`, `ParseVersionHeader(FormatKeyword, AcceptedVersions)` |
| `PwUse.{h,cpp}` | `FPwModelUse` + new resolver | every `.pwanim` repeats `use skeleton from` |

**Per‑format, deliberately not shared:** the op‑table *contents*, the document struct, the compiler, asset creation.

**The document shape is the seam, and the answer is that it barely generalises — which is the finding.** Only two things are common to every format:

```
struct FPwDocumentHeader { int32 Version = -1; int32 VersionLine = 0; TArray<FPwUse> Uses; };
```

`FPwModelDocument` embeds one and keeps `Parts`/`Materials`/`Collision`/`Lightmap`/`Skin`. `FPwSkelDocument` embeds one and holds `FString AssetPath` + `TArray<FPwBone> Roots`. `FPwAnimDocument` embeds one and holds whatever the sibling needs. **`skeleton` and `skin` are the first real test, and the test's result is: do not hoist Parts/Materials into a base document.** `skin` is a `.pwmodel` block because it configures a `.pwmodel` output; `skeleton` is a `.pwskel` document because it *is* an output. Neither wants a generic document.

**Diagnostic code prefix.** Once `.pwanim` emits `PWSRC_UNCLOSED_BRACE`, the code lies. Rename the shared codes to `PW_*` (`PWSRC_UNCLOSED_BRACE`, `PWSRC_UNEXPECTED_TOKEN`, `PWSRC_UNKNOWN_OP`, `PWSRC_UNKNOWN_PARAM`, `PWSRC_DUPLICATE_PARAM`, `PWSRC_MISSING_PARAM`, `PWSRC_BAD_TUPLE_ARITY`, `PWSRC_BAD_VALUE`, `PWSRC_BAD_BLOCK`, `PWSRC_MISSING_VERSION`, `PWSRC_UNSUPPORTED_VERSION`, `PWSRC_UNEXPECTED_CHARACTER`, `PWSRC_UNTERMINATED_STRING`, `PWSRC_INVALID_STRING`, `PWSRC_INVALID_NUMBER`), leaving the genuinely pwmodel‑specific ones (`PWMODEL_NO_PARTS`, `PWMODEL_DUPLICATE_PART`, `PWMODEL_COLLISION_*`, `PWMODEL_UV_*`, `PWMODEL_MATERIAL_*`, `PWMODEL_BOOLEAN_NO_EFFECT`, …) alone. `pwmodel 0` carries no compatibility promise (`docs/pwmodel-design.md:41-43`), so this needs no migration. It touches ~30 assertions in `TestPwModelParser.cpp` / `TestPwModelCompiler.cpp`. **This rename belongs in the emitter agent's core extraction, not here** — it ships first and pays the cost once.

**Emitter round‑trip contract for the nested forms** (hand to the emitter agent):
- one `bone` per line, header params emitted in fixed order `at`, `rotate`, `scale`, omitted when identity;
- one indent level per nesting depth, four spaces, matching the shipped examples;
- children in declaration order, never sorted — sorting would make `emit(parse(x)) != x` for every hand‑authored file and destroy the diff stability the emitter exists for;
- no blank line inside a `skeleton { }` block;
- `skin { }` entries in declaration order.
Idempotence `emit(parse(emit(parse(x)))) == emit(parse(x))` is the acceptance test.

## 3. Grammar — `.pwskel`

```
pwskel 0

skeleton "/Game/Chars/SK_Hero" {
    bone root {
        bone pelvis at=(0, 0, 96) {
            bone spine_01 at=(0, 0, 12) rotate=(0, -5, 0) {
                bone spine_02 at=(0, 0, 14) { }
            }
            bone thigh_l at=(0, 9, -2) rotate=(0, 180, 0) { }
            bone thigh_r at=(0, -9, -2) rotate=(0, 180, 0) { }
        }
    }
}
```

- `skeleton "<asset path>" { … }` — exactly one per file, mandatory, asset path mandatory. Header shape reuses `ParsePart`'s "brace closes the header" rule (`PwModelParser.cpp:2176-2210`), including its rewind‑and‑resynchronise recovery (`:2192-2207`) — without which one malformed header parameter produced N+2 diagnostics.
- `bone <name> [at=(x,y,z)] [rotate=(roll,pitch,yaw)] [scale=(x,y,z)] { … }` — the block may be empty (a leaf). Transform is **bone‑local**, the parent's frame, matching `skeleton.add_bone` (`SkeletonHandler.cpp:611-618`) and `FReferenceSkeletonModifier::Add(BoneInfo, BonePose)`.
- `rotate=` is `(roll, pitch, yaw)` through `PwValueRead::MakeRotator` — the same function `part` and every collision element use. Not `FRotator`'s `(Pitch, Yaw, Roll)`.
- Exactly one top‑level `bone`. Two is an error: `FReferenceSkeletonModifier::Add`'s `bAllowMultipleRoots` defaults false and `skeleton.add_bone` only sets it for the very first bone (`SkeletonHandler.cpp:626`).
- Op table: `EPwOpContext::Skeleton` with one entry, `bone`, `bAcceptsBlock=true`, `bRequiresBlock=true`, params `at`/`rotate`/`scale` — reuses `PartHeaderParams()`'s spec shapes verbatim.

Diagnostics an author sees:

| Malformed shape | Code | Message |
|---|---|---|
| `skeleton { … }` with no path | `PWSKEL_MISSING_ASSET_PATH` | `A skeleton declares the asset it produces: skeleton "/Game/…/SK_Name" { … }. It is named here rather than passed to skel.compile because every file that 'use's this skeleton reads the name from this line.` |
| second `skeleton` block | `PWSKEL_DUPLICATE_SKELETON` | `A .pwskel file declares exactly one skeleton; USkeleton is one asset.` |
| no `skeleton` block | `PWSKEL_NO_SKELETON` | anchored to the version header line, mirroring `PWMODEL_NO_PARTS` (`PwModelParser.cpp:2749-2755`) |
| two top‑level `bone`s | `PWSKEL_MULTIPLE_ROOTS` | names both, `A skeleton has one root bone. Nest 'X' under 'Y', or split them into two .pwskel files.` |
| repeated bone name | `PWSKEL_DUPLICATE_BONE` | names the earlier line, as `PWMODEL_DUPLICATE_PART` does |
| `bone` with no name | `PWSRC_UNEXPECTED_TOKEN` expecting `a bone name` | shared |
| `bone` with no `{ }` | `PWSRC_BAD_BLOCK` | `'bone' requires a '{ … }' block; write '{ }' for a leaf.` The empty‑block spelling is what makes the hierarchy unambiguous and the emitter's output stable |
| `at="0,0,90"` | `PWSRC_BAD_VALUE` | already names both halves (`PwModelParser.cpp:1690-1700`) |
| non‑identifier bone name chars | tokenizer's `PWSRC_UNEXPECTED_CHARACTER` | unchanged |
| `bone` at model level in `.pwmodel` | `PWSRC_UNEXPECTED_TOKEN` | `'bone' belongs in a .pwskel document. Reference a skeleton from here with: use skeleton from "…/name.pwskel".` |

## 4. Grammar — `skin` and `bone=` in `.pwmodel`

```
pwmodel 0

use skeleton from "../rig/hero.pwskel"

materials { Skin = "/Game/Chars/M_Skin" }

part torso bone="spine_02" at=(0, 0, 110) {         # rigid: whole part rides one bone
    box size=(30, 20, 40) material="Skin"
}

skin {                                               # OR: one model-wide solve
    smooth max_influences=4 stiffness=0.2 method=direct_distance
}
```

- `bone="<BoneName>"` on the **part header**. New entry in `PartHeaderParams()` alongside `at`/`rotate`/`scale`, `EPwParamType::String`. Stored on `FPwModelPart` as `FString BoneBinding`.
- `skin { … }` — at most one, model level, `EPwOpContext::Skin` op table with one entry:
  `smooth [max_influences=1..12] [stiffness=0..1] [method=direct_distance|geodesic_voxel] [voxel_resolution=8..1024]`.
  Ranges come from `SkeletalMeshAssetIOHandler.cpp:743-745` (`SkeletalIOMaxInfluencesCeiling = 12`, from `FBoneWeights`' inline limit — above it the engine silently discards). Defaults mirror the verb's: 4 / 0.2 / `direct_distance` / 128, which are the values that shipped four skinned meshes in the host project.
  At most one `smooth`. Reuses `ForEachBlockEntry` and the collision block's duplicate‑detection shape (`PwModelParser.cpp:2422-2458`).

Semantics, exhaustively:

| Document | Output | Rule |
|---|---|---|
| no `use skeleton`, no `bone=`, no `skin` | `UStaticMesh` | M1 path, byte‑identical |
| `use skeleton` + every part `bone=` | `USkeletalMesh`, all rigid | |
| `use skeleton` + `skin { smooth }`, no `bone=` | `USkeletalMesh`, all smooth | |
| `use skeleton` + `skin { smooth }` + some parts `bone=` | `USkeletalMesh`, **smooth first, then the `bone=` parts overwritten rigid** | see §7 |
| `use skeleton` + neither | error `PWMODEL_SKIN_UNBOUND` | `'use skeleton' names a skeleton nothing binds to. Add bone="…" to each part, or a skin { smooth } rule.` |
| `bone=` or `skin` without `use skeleton` | error `PWMODEL_SKIN_NO_SKELETON` | names the `use` line to add |
| some parts `bone=`, others not, **and no `skin { smooth }`** | error `PWMODEL_SKIN_INCOMPLETE` naming every unbound part | An unbound part's vertices reach the asset with zero influences, and the engine's own gate cannot see it (§7) |
| `collision { }` present with skeletal output | error `PWMODEL_COLLISION_ON_SKELETAL` | `A SkeletalMesh carries collision as a UPhysicsAsset, not a UBodySetup. Compile the mesh, then skeleton.create_physics_asset.` |
| `lightmap` present with skeletal output | error `PWMODEL_LIGHTMAP_ON_SKELETAL` | `LightMapCoordinateIndex is a UStaticMesh property.` |
| `bone="Nonexistent"` | error `PWMODEL_BONE_NOT_FOUND` naming the part, the bone, the skeleton asset, and up to 5 nearest bone names via `SuggestClosest` | resolved against the loaded `USkeleton`'s `FReferenceSkeleton` |

## 5. `use … from` resolution

**Statement:** `use <kind> from "<path>"`, `<kind> ∈ {skeleton, part}` in `.pwmodel`, `{skeleton}` in `.pwanim`. **`animation` is removed from the kind list** — nothing references an animation now that `.pwanim` is its own format (`PwModelParser.cpp:2494-2495` currently accepts it).

**Path is relative to the containing file.** `FPwModelCompileOptions` gains `FString SourceDirectory` (absolute, normalised), set by `ModelCompileHandler` from `ModelHandler_ResolveSourcePath`'s output (`ModelCompileHandler.cpp:55-65`). It is deliberately **not** `SourcePath`, which is project‑relativised for the provenance stamp (`:81-99`) and would resolve wrongly for a source outside the project.

Resolution, in order:

1. **No `SourceDirectory`** (inline `model.validate` text) → `PW_USE_UNRESOLVABLE`: *"'use skeleton from "…"' resolves against the containing file's directory, and inline text has none. Save the document and pass filePath to model.compile."*
2. **Normalise + reject traversal.** `FPaths::Combine(SourceDirectory, Path)` → `ConvertRelativePathToFull` → `NormalizeFilename`. A resolved path that escapes no boundary is fine (a `.pwmodel` may live anywhere, `docs/pwmodel-design.md:189-190`) but a path containing `..` after normalisation, or an absolute path, is `PW_USE_BAD_PATH`.
3. **Read via `IPwSourceReader`.** Miss → `PW_USE_TARGET_NOT_FOUND` carrying **both** the authored spelling and the resolved absolute path, following `ModelHandler_LoadSourceFile`'s precedent (`ModelCompileHandler.cpp:111-114`).
4. **Cycle check.** The resolver carries a visited set keyed on the normalised absolute path plus a depth cap of 16. A repeat → `PW_USE_CYCLE` listing **the whole chain in order**, `a.pwmodel → b.pwskel → c.pwskel → a.pwmodel`. With today's kind set the graph is depth 1 and a cycle is unreachable through `use skeleton`; the guard exists because `use part` is next and because the resolver is shared with `.pwanim`. It is tested at the resolver's own level against the in‑memory reader, not through a `.pwmodel` fixture — a fixture that cannot construct a cycle would make the test unable to fail.
5. **Format check from the target's version header only** (no full parse). `use skeleton` requires `pwskel`; anything else → `PW_USE_KIND_MISMATCH`: *"'use skeleton' needs a .pwskel document; '…/x.pwmodel' declares 'pwmodel 0'."*
6. **Parse the target with the shared parser and take its declared asset path.** Parse errors in the target → `PW_USE_TARGET_INVALID`, carrying the target's own first error verbatim with its own file/line so the author is pointed at the right file. No declared path → `PW_USE_TARGET_HAS_NO_ASSET`.
7. **`LoadObject<USkeleton>` that path.** Null → `PW_USE_ASSET_NOT_FOUND`: *"…/hero.pwskel names /Game/Chars/SK_Hero, which does not exist. Compile it first: skel.compile filePath=…/hero.pwskel"*. **This is the diagnostic the author hits most; the message must carry the literal command.**
8. **Class check.** Occupied by a non‑`USkeleton` → `PW_USE_ASSET_CLASS_MISMATCH` naming both classes.
9. **Zero‑bone skeleton** → `PW_USE_SKELETON_HAS_NO_BONES`, pre‑empting the same refusal `bind_skin_weights` makes (`SkeletalMeshAssetIOHandler.cpp:723-729`), because `FSkinBindingOp::SetTransformHierarchyFromReferenceSkeleton` has nothing to solve against.
10. **Duplicate `use skeleton`** → `PW_DUPLICATE_USE` naming the earlier line.

**Is a `use`d file compiled? No — read and parsed, never compiled.** Two reasons, both structural: compiling it would emit a second `.uasset` from one `model.compile` and leave `FPwModelCompileResult::AssetPath` ambiguous (`PwModelCompiler.h:63-71`); and there is no build graph — `docs/pwmodel-design.md:35-39` forbids build artifacts, so there is nothing to record that the target is stale. **The ordering dependency (compile the skeleton, then the meshes, then the animations) lives in the author's workflow, exactly where a `materials { Slot = "/Game/M_Foo" }` binding's dependency already lives.** Step 7's message is what makes that workflow discoverable.

**Repetition cost for `.pwanim`:** one line per file, and the *file* path is what is repeated — not the asset path. Renaming `SK_Hero` to `SK_Hero_v2` is a one‑line edit in `hero.pwskel`; 20 `.pwanim` files are untouched. That is the reason D3 puts the asset path in the skeleton document rather than in the `use` statement.

## 6. Asset creation — the SkeletalMesh path is *not* symmetric, and that is a win

`UE::AssetUtils::CreateSkeletalMeshAsset` exists (`CreateSkeletalMeshUtil.h:104-106`, module `ModelingComponentsEditorOnly`, already a `PinWrightGeometry` dependency at `PinWrightGeometry.Build.cs:95`). Differences from the static path that change the design:

- **There is no `bDeferPostEditChange`.** The build happens *inside* the converter via `FScopedSkeletalMeshPostEditChange` (`StaticToSkeletalMeshConverter.cpp:566-568`). There is nothing to defer and no post‑build patch window.
- **Materials are a true input, with names.** `FSkeletalMeshAssetOptions::SkeletalMaterials` is a `TArray<FSkeletalMaterial>` (`CreateSkeletalMeshUtil.h:78`) written by `SetMaterials` *before* the build (`StaticToSkeletalMeshConverter.cpp:544`). `FSkeletalMaterial` carries `MaterialSlotName`. **So the skeletal path needs none of `GeometryAssetCreate.cpp:269-287`'s post‑hoc slot renaming — the document's slot names go in as input.** This is also why `CreateNewSkeletalMeshAssetFromMesh` must be bypassed: it leaves materials empty and the engine allocates one unnamed default slot (`CreateNewAssetUtilityFunctions.cpp:407-410`), which is why `geometry.convert_to_skeletal_mesh`'s create path loses materials.
- **Polygon‑group → slot mapping.** The converter falls back to indexing the material list by polygon group index when a slot name is `NAME_None` (`StaticToSkeletalMeshConverter.cpp:590-600`). `FDynamicMeshToMeshDescription` writes polygon groups from material IDs, and the compiler's `SlotNames` is already in material‑ID order (`PwModelCompiler.cpp:2333`). The orders agree; nothing extra is needed.
- **The reuse path is destructive in ways the static path is not.** `CreateSkeletalMeshUtil.cpp:53-82` on an existing asset: `UnregisterAllMorphTarget()`, `LODModels.Empty()`, `SetNumSourceModels(0)`, `GetMaterials().Empty()`, `GetRefSkeleton().Empty()`, `SetSkeleton(nullptr)`, `SetPhysicsAsset(nullptr)`. `AssetCreatePolicy::EAction::UpdateInPlace` therefore does **not** mean "rebuilt, referencers intact" the way it does for `UStaticMesh` — the object survives but its PhysicsAsset and morph targets do not.
- **Creating a skeletal mesh mutates the `USkeleton`.** `Options.Skeleton->MergeAllBonesToBoneTree(NewSkeletalMesh)` and, when the skeleton has no preview mesh, `SetPreviewMesh(NewSkeletalMesh)` (`CreateSkeletalMeshUtil.cpp:171-176`). **A second package is dirtied.** This does not violate the ADR — no second `.uasset` is *emitted* — but it must be reported and, under `bSave`, saved. Otherwise the author's skeleton silently goes dirty and is lost on the next launch, the same class as `B-geometry-convert-static-mesh-no-disk-write`.
- **The engine's weight gate cannot see the failure that matters.** `ValidateSkinWeightAttribute` (`StaticToSkeletalMeshConverter.cpp:466-498`) only checks that *a* skin‑weight profile exists and that no bone index exceeds `GetRawBoneNum()`. All‑zero weights pass. And `FSkeletalMeshAttributes::Register()` installs a zero‑filled attribute unconditionally at `CreateSkeletalMeshUtil.cpp:120-121`, so the "no profile" branch is effectively unreachable from this path. **The compiler's own exhaustive coverage scan is the only real gate.**

New file pair, mirroring `GeometryAssetCreate.{h,cpp}` exactly:

```
Source/PinWrightGeometry/Private/Handlers/Geometry/GeometrySkeletalAssetCreate.{h,cpp}
FSkeletalMeshCreateResult CreateSkeletalMesh(UDynamicMesh* Mesh, const FSkeletalMeshCreateSpec& Spec);
```

`FSkeletalMeshCreateSpec`: `AssetPath`, `bOverwrite`, `USkeleton* Skeleton`, `MaterialSlots` (`TArray<FString>`), `MaterialBindings`, `bRecomputeNormals/Tangents`, `SourcePath`, `SourceHash`, `bSave`.
`FSkeletalMeshCreateResult` adds to the static shape: `TArray<FString> ClearedFeatures` (morph target names + physics asset path found on a reused occupant), `FString SkeletonPackageName`, `EAssetSaveState SkeletonSaveState`.

Body:
1. Path validation and `/Engine/` refusal — verbatim from `GeometryAssetCreate.cpp:85-98`.
2. `AssetCreatePolicy::Resolve(PackageName, AssetName, USkeletalMesh::StaticClass(), Spec.bOverwrite)` — `PINWRIGHT_API`, `AssetCreatePolicy.h:83`. Never `IAssetTools::CreateAsset`; the modal chain it opens wedges the game thread (`AssetCreatePolicy.h:6-19`).
3. Provenance rule — `UPwModelAssetUserData` verbatim from `GeometryAssetCreate.cpp:141-168`. `USkeletalMesh` implements `IInterface_AssetUserData` with a serialised `AssetUserData` UPROPERTY (`SkeletalMesh.h:439`, `:2832-2837`), so the stamp survives save/load exactly as on `UStaticMesh`.
4. **On `UpdateInPlace`, capture what the engine is about to destroy** — `GetPhysicsAsset()` and `GetMorphTargets()` names — before calling in. Re‑attach the PhysicsAsset after; report the morph target names in `ClearedFeatures` (they cannot be re‑attached, they are source data the compiler never had).
5. UV guard: `GeometryUtils::EnsureMeshHasUVs` before the create, both recomputes off when it fails — the MikkT `[0]`‑index crash is identical on both paths (`SkeletalMeshAssetIOHandler.cpp:900-913`).
6. `MeshCopy` via `ProcessMesh`, `Options.SourceMeshes.DynamicMeshes.Add(&MeshCopy)`, `Options.SkeletalMaterials` built from `MaterialSlots` × `MaterialBindings` with unbound slots reported (not substituted), `Options.Skeleton`, `Options.NumSourceModels = 1`, `Options.UsePackage` on the update path.
7. `FAssetRegistryModule::AssetCreated` — the util does not publish (same gap as the static one, `GeometryAssetCreate.cpp:380-383`).
8. `SaveAssetToDiskReportingPresence(NewMesh, /*bForce=*/true, …, &SaveState)` — the freshness‑gated writer, never `McpSafeAssetSave` (which persists nothing, `AssetUtils.h:51-62`).
9. **Same save for the skeleton's package**, reported separately in `SkeletonSaveState`.
10. Read the asset's own counts back — `GetImportedModel()->LODModels[0]` section vertex/triangle totals — the way `GeometryAssetCreate.cpp:427-428` reads `GetNumTriangles(0)`.

`.pwskel`'s own creator is much smaller: `CreateSkeleton(const FPwSkelDocument&, Spec)` → `AssetCreatePolicy::Resolve(..., USkeleton::StaticClass(), ...)` → `NewObject<USkeleton>` (or reuse) → one `FReferenceSkeletonModifier` scope, depth‑first `Add(FMeshBoneInfo{Name, ParentIndex, ExportName}, BoneLocalTransform, bAllowMultipleRoots = (i == 0))` — the shape `SkeletonHandler.cpp:526-533` and `:620-629` already use → provenance stamp (`USkeleton` also implements `IInterface_AssetUserData` with a serialised array, `Skeleton.h:294`, `:1011-1020`) → `AssetCreated` → `SaveAssetToDiskReportingPresence`.

## 7. Ordering constraints the compiler must enforce

`bind_skin_weights` "must run last" is a **caller‑discipline** rule in the RPC (`SkeletalMeshAssetIOHandler.cpp:684`, "RUN THIS LAST"). In the compiler it becomes a **structural** one: the solve lives in a stage that no op can follow, because ops only ever run inside `BuildParts` (`PwModelCompiler.cpp:1955-1995`).

Revised `FCompiler::Run` (current stages at `PwModelCompiler.cpp:2401-2469`):

```
1  parse
2  reject reserved  (only `animation` remains reserved in .pwmodel; message now names .pwanim)
2a resolve `use skeleton` -> USkeleton*                  [must precede any binding]
2b validate the skin/collision/lightmap combination      [before any mesh exists — nothing to unwind]
3  BuildParts                                            [unchanged]
3a per-part rigid pre-bind, for each part carrying bone=:
       CopyBonesFromSkeleton(Skeleton, PartMesh)
       MeshCreateBoneWeights(PartMesh, bReplace=true)
       SetAllVertexBoneWeights(PartMesh, {{BoneIndex, 1.0f}})
4  FillMissingUVChannels                                 [unchanged]
5  MergeParts                                            [see the trap below]
5a *** record per-part vertex ranges during the merge ***
6  ValidateMergedMesh                                    [unchanged]
6a if skin { smooth }:
       CopyBonesFromSkeleton(Skeleton, Merged)
       MeshCreateBoneWeights(Merged, bReplace=true)
       ComputeSmoothBoneWeights(Merged, Skeleton, Options)
6b for each bone= part: SetVertexBoneWeights(Merged, VID, {{BoneIndex,1.0f}}) over its recorded range
6c *** exhaustive coverage scan on Merged ***  -> PWMODEL_SKIN_INCOMPLETE on any gap
7  collision — skipped entirely on the skeletal path (rejected at 2b)
8  CreateSkeletalMesh  |  CreateStaticMesh                [exactly one, still]
```

Three ordering facts that are not obvious and that each hide a silent failure:

**(a) The merge silently drops weights unless the destination already carries the profile.** `FDynamicMeshEditor::AppendMesh` copies skin weights only when `Mesh->Attributes()->GetSkinWeightsAttribute(Key)` is non‑null (`DynamicMeshEditor.cpp:2073-2107`). Today this is saved by luck: `FGeometryScriptAppendMeshOptions::CombineMode` defaults to `EnableAllMatching` (`MeshBasicEditFunctions.h:85-86`) → `Target.EnableMatchingAttributes(Source, false, false)` → `DynamicMeshAttributeSet.cpp:615-623` attaches any profile the target lacks, and `EnableMatchingBoneAttributes` at `:637` propagates bones. **But `MakeNew` creates the attribute against a target that already holds earlier parts' vertices, so those vertices come out empty.** That is exactly why "some parts `bone=`, others not" is an error rather than a silent partial rig — and why 6c is not optional.

**(b) A smooth solve covers every vertex and has no selection parameter.** `ComputeSmoothBoneWeights(TargetMesh, Skeleton, Options, Profile, Debug)` (`MeshBoneWeightFunctions.h:392-399`) — no `FGeometryScriptMeshSelection`, unlike `TransferBoneWeightsFromMesh` at `:412-419`. Mixed rigid+smooth is therefore *solve‑then‑overwrite*, never *solve‑the‑complement*. Hence step 6b, and hence 5a.

**(c) The per‑part vertex ranges must be captured, not inferred.** Two options; take the first:
 - **Capture** — replace `UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh` in `MergeParts` (`PwModelCompiler.cpp:2139-2141`) with a direct `FDynamicMeshEditor::AppendMesh(..., FMeshIndexMappings&, ...)` (`DynamicMeshEditor.cpp:1899`), which fills `VertexMap` exactly. `PinWrightGeometry` already links `GeometryCore` and `DynamicMesh` (`Build.cs:75-76`). The one thing that must be replicated is `AppendOptions.UpdateAttributesForCombineMode(AppendToMesh, OtherMesh)` (`MeshBasicEditFunctions.cpp:605`, `:526-537`) — call `Target.EnableMatchingAttributes(Source, false, false)` explicitly, or M1's attribute behaviour drifts.
 - Infer from `[MaxVertexID before, MaxVertexID after)`. Correct only while the merged mesh stays compact; needs a runtime `VertexCount() == MaxVertexID()` assertion and a refusal path. Cheaper, more fragile — do not.

**(d) "Nothing may run after the solve" must be checkable.** Record `MeshVertexCount` at the end of step 6 and re‑read it immediately before step 8; a mismatch is `PWMODEL_SKIN_STALE` naming the stage that moved. This costs nothing and is what makes a future stage inserted at 7 fail loudly instead of shipping an asset whose tail vertices carry no influence.

**(e) The coverage scan must be exhaustive, and must read `GetVertexBoneWeights`' boolean only.** The trap's signature is that the *last* vertices are the unweighted ones, so a head sample misses it every time (`SkeletalMeshAssetIOHandler.cpp:304-308`). The array that call fills is unusable — it `SetNum(N)` then `Add`s N more, so it comes back with 2N entries whose first N are default‑constructed (`MeshBoneWeightFunctions.cpp:293-302`). Reuse `SkeletalIOScanWeightCoverage`'s logic; **promote it out of the handler's anonymous namespace into `GeometryUtils`** so the compiler and the verb cannot drift — the RPC verb's gate and the compiler's gate must be the same predicate.

**Hard‑dependency order** (nothing else is ordered; independent items are marked):

```
[A] Shared-core extraction (PwToken/PwTokenizer/PwValue/PwDiagnostic/PwOpTable/PwScanner
    into PinWright/Private/Format/, PW_* code rename)
        └── owned by the emitter agent; everything below depends on it

[B] FPwUse + FPwUseResolver + IPwSourceReader        depends on: A
[C] .pwskel parser + FPwSkelDocument                 depends on: A
[D] CreateSkeleton()                                 depends on: C
[E] skel.compile RPC                                 depends on: D
[F] GeometryUtils::ScanSkinWeightCoverage (hoist)    depends on: nothing   [independent of A–E]
[G] MergeParts captures FMeshIndexMappings           depends on: nothing   [independent of A–E]
[H] bone= on part header + skin { } block parsing    depends on: A
[I] Skin stages 2a/2b/3a/5a/6a/6b/6c/6d in FCompiler depends on: B, C, F, G, H
[J] CreateSkeletalMesh()                             depends on: F         [independent of A–E, H]
[K] model.compile plumbs SourceDirectory + reports
    skeleton package / cleared features              depends on: I, J
[L] Emitter support for skeleton/skin/bone= forms    depends on: A, C, H
[M] Docs: pwmodel-format.md, pwmodel-design.md,
    a new adr/0002 for D1+D3, engine-version-support  depends on: everything it documents
```

F, G and J are the only three that can start immediately. C and B are independent of each other. E is independent of everything after D.

## 8. What cannot be done, and the value‑system requirement to hand over

**Not blocked — solved above:** mixed rigid+smooth (solve‑then‑overwrite, §7b), cycles (§5.4), material slot names on skeletal output (§6, they are an input).

**Genuinely out, on principle:**
- **Per‑vertex weights authored in the document.** A vertex index is a component reference, and the format stores none by design (`docs/pwmodel-design.md:143-153`) precisely because they dangle on every upstream edit. This is a refusal, not a gap.
- **`cut_material=`‑style bone tagging of boolean‑exposed faces.** Same face‑identity problem, already reserved‑and‑refused.

**Out because the engine surface does not exist through this path:**
- **Named alternate skin‑weight profiles.** `CreateSkeletalMeshAsset` writes one profile through the MeshDescription pipeline; `skeleton.normalize_weights` / `prune_weights` / `set_vertex_weights` / `copy_weights` write `FSkinWeightProfileInfo` and never touch base skinning (`SkeletalMeshAssetIOHandler.cpp:118-129`). Offering a `profile=` on `skin` would re‑import exactly the ambiguity that makes the `skeleton.*` family misleading.
- **Sockets, virtual bones, retarget modes, curve metadata in `.pwskel`.** `FReferenceSkeletonModifier` covers bones only; each of these has its own RPC (`skeleton.create_socket`, `create_virtual_bone`) and its own key/value shape. Reserved inside the `skeleton` block, rejected by name with a pointer to the verb.
- **Morph targets.** No input on `FSkeletalMeshAssetOptions`, and the reuse path destroys existing ones (`CreateSkeletalMeshUtil.cpp:60-63`). Reported in `ClearedFeatures`, never silently dropped.
- **LOD > 0.** `NumSourceModels` accepts N but the compiler has one merged mesh. One LOD, stated.
- **UE < 5.5.** `CopyBonesFromSkeleton` is 5.5+ (`MeshBoneWeightFunctions.h:476` on 5.8; `SkeletalMeshAssetIOHandler.cpp:48-51` records the survey). The whole skin path inherits that floor. Add rows to `docs/engine-version-support.md`.

**The one value‑system requirement — hand this to the value‑system agent:**

The grammar above needs **nothing new**: `at`/`rotate`/`scale` are `Tuple(3)`, `bone=` is `String`, `smooth`'s params are `Number`/`Identifier`. The sibling's nested list‑of‑lists is not needed and neither are named‑resource handles.

**One future shape does need something, and it should be requested now rather than discovered later:** a part blended across two bones (a forearm twist, a shoulder deltoid) wants

```
part forearm_l bone_weights=[("lowerarm_l", 0.7), ("lowerarm_twist_l", 0.3)] { … }
```

That is a **tuple with heterogeneous components** — a string beside a number. `FPwModelValue::Tuple` is `TArray<double>` and `TupleList` is `TArray<TArray<double>>` (`PwModelAst.h:33-34`), so it cannot hold it, and a numeric‑only nested list cannot either. **The requirement is precisely: change `Tuple` from `TArray<double>` to `TArray<FPwValue>`**, with a new `EPwParamType::WeightList` validating "each entry is a 2‑tuple of (String, Number in [0,1])". Every existing accessor (`GetVector3`, `GetPointList3`, `GetFrameList`, …) becomes a filtered read over that array, and the change is source‑compatible for callers because they all go through `PwValueRead`. If the value‑system agent lands `TArray<FPwValue>` rather than `TArray<double>` for the nested kind, `bone_weights=` costs one op‑table entry here and nothing else. If they land numeric‑only, this shape is permanently unreachable without a second migration. **That is the single coordination point.**

## 9. Test strategy

Every entry names what would make it unable to fail. Location: `Source/PinWright/Private/Tests/Format/` for A/B/E (shared core, main module), `Source/PinWrightGeometry/Private/Tests/Model/` for C/D.

**A — `.pwskel`**
1. `Pw.Skel.BoneTreeMatchesDeclaredNesting` — compile, `LoadObject<USkeleton>`, walk `GetReferenceSkeleton().GetParentIndex(i)` and assert the full parent chain. *Unable to fail if* it asserts `boneCount > 0`, or reads back the AST instead of the created asset.
2. `Pw.Skel.BonePoseIsBoneLocalNotModelSpace` — `pelvis at=(0,0,90) { spine at=(0,0,12) }`; assert `GetRefBonePose()[spine].GetTranslation().Z == 12` **and** the composed model‑space Z == 102. *Unable to fail if* the child sits at `(0,0,0)` or there is only one bone — local and model space coincide and the bug is invisible.
3. `Pw.Skel.RotateIsRollPitchYaw` — assert against `PwValueRead::MakeRotator`'s reading, not `FRotator(a,b,c)`. *Unable to fail if* the rotation is about a single axis; the fixture must rotate about two.
4. `Pw.Skel.DuplicateBoneNameIsRejectedAndWritesNothing` — assert the code **and** `!UEditorAssetLibrary::DoesAssetExist(path)`. *Unable to fail if* only `bSuccess == false` is checked.
5. `Pw.Skel.TwoRootBonesRejected` — assert `PWSKEL_MULTIPLE_ROOTS` specifically. *Unable to fail if* any failure satisfies it; a plain parse error would.
6. `Pw.Skel.SkeletonAssetPathIsRequired` — assert `PWSKEL_MISSING_ASSET_PATH` plus a **control**: the identical document *with* a path compiles green. Without the control the test cannot distinguish "the fix works" from "the fixture is broken" (`rpc-design.md` §12).

**B — `use` (in‑memory reader, no filesystem)**
7. `Pw.Use.MissingTargetNamesBothSpellings` — assert the code **and** that the message contains the authored spelling *and* the resolved absolute path. *Unable to fail if* only the code is asserted; the message is the whole authoring UX.
8. `Pw.Use.CycleReportsTheWholeChain` — three in‑memory documents A→B→C→A; assert `PW_USE_CYCLE` and all three names in order. *Unable to fail if* it asserts only that resolution terminated — the depth cap alone terminates and produces the wrong code.
9. `Pw.Use.WrongFormatIsNotAParseError` — target is a **well‑formed** `.pwmodel`; assert `PW_USE_KIND_MISMATCH`. *Unable to fail if* the fixture target is unparsable, because any error would look right.
10. `Pw.Use.AssetClassMismatchNamesBothClasses` — actually create a `UStaticMesh` at the path the `.pwskel` names. *Unable to fail if* the path is empty — then it is `PW_USE_ASSET_NOT_FOUND` and the class check never runs.
11. `Pw.Use.TargetIsReadNeverCompiled` — the `.pwskel` names `/Game/…/SK_Missing`; assert `PW_USE_ASSET_NOT_FOUND` **and** `!DoesAssetExist(/Game/…/SK_Missing)` afterwards, **and** that the reader recorded exactly one read of the target. *Unable to fail if* only the failure is asserted — the property under test is the *absence* of a side effect.
12. `Pw.Use.MessageCarriesTheCompileCommand` — assert the message contains `skel.compile filePath=`. *Unable to fail if* the assertion is on the code; this is the most‑hit diagnostic in the whole feature.
13. `Pw.Use.InlineTextRejectsRelativePathByName` — assert `PW_USE_UNRESOLVABLE` mentions `model.compile filePath`.

**C — skin and the skeletal output**
14. `Model.Compiler.SkinnedModelYieldsExactlyOneSkeletalMesh` — three parts; probe every per‑part path a per‑part emitter would have written, mirroring the existing `ThreePartDocumentYieldsOneAsset` (`TestPwModelCompiler.cpp:218`). *Unable to fail if* only `Result.AssetPath` is asserted — the response reports one path either way.
15. `Model.Compiler.EveryVertexOfTheBuiltAssetCarriesAnInfluence` — read `GetImportedModel()->LODModels[0]` sections' `InfluenceWeights` off the **built asset** and assert a non‑zero total per vertex. *Unable to fail if* it reads the coverage number the compiler itself reported, or reads the `UDynamicMesh`. The engine's own gate passes all‑zero weights (`StaticToSkeletalMeshConverter.cpp:466-498`), so the readback must be the asset (`rpc-design.md` §4: not a readback of the value the writer just set).
16. `Model.Compiler.RigidPartBindsToItsOwnBone` — two parts on two bones, disjoint in X; assert **every** vertex with X<0 is influenced only by bone A and every X>0 only by bone B. *Unable to fail if* both parts bind the same bone, or the assertion is "some vertex references bone B". The partition is what discriminates.
17. `Model.Compiler.SmoothSolveRunsAfterEveryGeometryOp` — the part's **last** op appends geometry (`append_buffers`); assert full coverage. *Unable to fail if* the appended geometry is empty, or coverage is sampled at the head of the ID range — the trap's signature is unweighted vertices at the tail.
18. `Model.Compiler.MixedRigidAndSmoothKeepsRigidPartsRigid` — assert the rigid part's vertices carry exactly one influence at weight 1 and the smooth part's carry more than one. *Unable to fail if* the smooth rule uses `max_influences=1` — both then look rigid.
19. `Model.Compiler.PartialBindingIsRejectedAndWritesNothing` — one part with `bone=`, one without, no `skin`; assert `PWMODEL_SKIN_INCOMPLETE` names the offending part, and `!DoesAssetExist`. *Unable to fail if* the code is not asserted.
20. `Model.Compiler.SkeletalOutputPreservesNamedMaterialSlots` — two distinctly named slots; assert `GetMaterials()[i].MaterialSlotName` values. *Unable to fail if* only `materialSlots == 2` is asserted; the names, not the count, are the surface `CreateNewSkeletalMeshAssetFromMesh` loses.
21. `Model.Compiler.CollisionWithSkinIsRejected` / `…LightmapWithSkinIsRejected` — each paired with a **control** compiling the same document without `skin`.
22. `Model.Compiler.RecompileReportsWhatTheEngineCleared` — create, attach a PhysicsAsset and a morph target, recompile; assert `ClearedFeatures` names the morph target and that the PhysicsAsset is re‑attached afterwards. *Unable to fail if* the target had no PhysicsAsset to begin with.
23. `Model.Compiler.SkeletonPackageIsSavedNotJustDirtied` — capture the skeleton `.uasset`'s timestamp and size **before** the compile and assert both moved. *Unable to fail if* it is an existence probe: a previous save satisfies bare existence (`AssetUtils.h:154-168`).
24. `Model.Compiler.StaticPathIsByteIdenticalWithoutSkin` — every existing M1 compiler test must still pass, plus one explicit assertion that a document with no `use skeleton` produces `UStaticMesh` with the same triangle/vertex/slot counts as before. *Unable to fail if* the suite total is not asserted (`CLAUDE.md` Testing).

**D — emitter round‑trip**
25. `Pw.Emitter.SkeletonIdempotence` — `emit(parse(emit(parse(x)))) == emit(parse(x))` over a 4‑deep, 3‑wide skeleton with rotations and scales. *Unable to fail if* the fixture is one bone deep.
26. `Pw.Emitter.NestedBoneIndentIsStable` — compare against a **literal** expected string, not against `emit`'s own output.

**E — shared‑core seam**
27. `Pw.Core.SharedDiagnosticCodesAreFormatNeutral` — enumerate the codes actually emitted by `PwScanner` through a registry and assert none carries a `PWMODEL_` prefix. *Unable to fail if* it greps source text; enumerate the constants the way `PinWright.core.error_codes.AllEmittedCodesAreRegistered` does.
28. `Pw.Core.PwskelNeedsNoPartsAndPwmodelStillDoes` — both halves in one test: a `.pwskel` with zero parts compiles green **and** a `.pwmodel` with zero parts still errors `PWMODEL_NO_PARTS`. *Unable to fail if* only one half is asserted — the carve‑out is exactly the thing that could over‑apply.
29. `Pw.Core.UseResolverIsSharedByBothFormats` — drive the same `FPwUseResolver` from a `.pwmodel` fixture and a `.pwanim`‑shaped fixture and assert identical codes and messages for the same failure. *Unable to fail if* only one format is exercised; the point is that `.pwanim` cannot get a different diagnostic for the same mistake.

Expected‑total discipline: derive the new suite total as *last measured (3844 at local `3576104c`) + tests this change adds* and assert it; do not accept a green run at a stale number.

---

## Bugs found

1. **`PwModelParser.cpp:2700` — two different answers to the same question, three lines apart.** The `UnexpectedToken` string lists *"part, materials, collision, lightmap or use"*, omitting `skeleton`/`skin`/`animation`, while `ModelKeywords` at `:2693-2695` (used by the did‑you‑mean path at `:2733-2743`) includes all three. An author who writes a non‑identifier where a construct belongs is told a different, shorter vocabulary than one who misspells a construct name.

2. **Stale line citation in `ModelCompileHandler.cpp:73`.** It says the provenance stamp "is compared by plain string equality on the next compile (GeometryAssetCreate.cpp:169)". `:169` is a closing brace; the comparison is `Stamp->SourcePath == Spec.SourcePath` at `GeometryAssetCreate.cpp:147-148`.

3. **`geometry.convert_to_skeletal_mesh`'s `ASSET_EXISTS` message under‑reports the damage.** `SkeletalMeshAssetIOHandler.cpp:1036-1038` lists "LODs, materials, reference skeleton and physics asset". `CreateSkeletalMeshUtil.cpp:60-63` also calls `UnregisterAllMorphTarget()` on the reuse path, so **morph targets are destroyed too** and the warning does not say so. The same omission is in the comment at `:1028-1031`.

4. **`skeleton.create_skeleton` and `skeleton.add_bone` report no persistence at all.** Both call `McpSafeAssetSave` (`SkeletonHandler.cpp:548`, `:632`), which explicitly "does NOT write the .uasset" and "NOTHING it does makes an edit durable" (`AssetUtils.h:51-53`). Neither response carries a `saved`/`saveState` field, so a caller cannot tell the skeleton is dirty‑in‑memory only and will be lost on the next launch. Same class as `B-geometry-convert-static-mesh-no-disk-write`, and it is on the path an agent takes right before `bind_skin_weights`.

5. **Latent, not yet shipped: the merge silently drops skin weights for parts appended before the first weighted part.** `FDynamicMeshEditor::AppendMesh` copies weights only when the destination already holds the profile (`DynamicMeshEditor.cpp:2073-2107`); the profile is attached by `EnableMatchingAttributes` at the first append that carries one (`DynamicMeshAttributeSet.cpp:615-623`), and `MakeNew` sizes it against a mesh that already holds the earlier parts' vertices — which come out with empty `FBoneWeights`. Nothing in the engine reports it: `ValidateSkinWeightAttribute` (`StaticToSkeletalMeshConverter.cpp:466-498`) accepts all‑zero weights, and `FSkeletalMeshAttributes::Register` guarantees a profile always exists. Harmless in M1 (no skin weights anywhere) and the direct cause of the `PWMODEL_SKIN_INCOMPLETE` rule and the mandatory coverage scan above.

6. **Milestone language now false in shipped diagnostics.** `PwModelCompiler.cpp:1936-1937` tells authors *"Cross-file references arrive with skeletons in milestone 2"* and `:1944` tells them *"milestone 3 adds this"* for `animation`. Under the no‑milestones directive and the `.pwanim` split, both messages are wrong; the `animation` one now needs to name a different **file format**, not a later milestone. `docs/pwmodel-format.md:180-189` and `docs/pwmodel-design.md:192-234` carry the same framing.

7. **`use animation from` is accepted by the parser and has no meaning.** `PwModelParser.cpp:2494-2495` lists `animation` among the valid kinds. With `.pwanim` as its own format, nothing in a `.pwmodel` (or anywhere) references an animation asset — an animation references a skeleton, not the reverse. The kind should be dropped from the list.

### Critical files for implementation
- `Source/PinWrightGeometry/Private/Model/PwModelCompiler.cpp`
- `Source/PinWrightGeometry/Private/Model/PwModelParser.cpp`
- `Source/PinWrightGeometry/Private/Model/PwModelAst.h`
- `Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryAssetCreate.cpp`
- `Source/PinWrightGeometry/Private/Handlers/Geometry/SkeletalMeshAssetIOHandler.cpp`
