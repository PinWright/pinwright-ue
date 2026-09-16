---
type: reference
summary: "Section five of the unified format plan: every verified unfixed PinWright defect, grouped by subsystem, assigned to an owning plan section, ordered by hard dependency then severity. Includes the ten seed claims rejected as already-fixed or non-existent."
date: 2026-08-20
tags: [pwmodel, pwanim, defects, backlog, persistence, error-codes, geometry, sequencer]
---

# Defect backlog

The fifth section of the unified plan. Bug-fixing is plan work, not a parallel stream: every item
below is scheduled, and the only legitimate reason an item is not fixed is technical.

Every entry was re-verified against the working tree on 2026-08-20 before being listed. **Ten
reported defects did not survive that check** and are in [§10 Rejected](#10-rejected-claims) with
the evidence that retired them — that section is the most useful one here, because each was
circulating as fact and three would have produced a fix for a bug that does not exist.

**Reading an entry.** `Owner` names the plan section that fixes it, or `—` for standalone work.
`Status` is `CONFIRMED` (reproduced in source), `REFINED` (real, but materially different from the
report), `FIXED` (closed in-tree — the entry keeps the finding and records what landed and where),
or `UNVERIFIED`. Line numbers are against a dirty tree that other agents are still
editing; re-read before cutting.

Ordering is hard dependency first, then severity. There are no milestones.

## Hard dependencies

Only four orderings are real. Everything else can start immediately.

```
D-30 save-contract decision ──> D-02, D-10..D-15   (a compiler cannot depend on an ambiguous save)
D-82 Pw* rename ─────────────> all grammar work    (renaming each addition twice otherwise)
D-80 nested list value kind ─> D-05 loft fix       (no literal exists for a profile list)
D-40 shared tick helper ─────> D-41, D-42          (the duplicates are what the fix deletes)
```

`D-30` is the one that gates other people's work: `format-decisions.md` already records that the
bone-keyframe write path cannot be built until `save: true` means one thing.

---

## 1. Silent wrong output

Success reported, wrong result produced. Highest severity in the plan: every other class at least
tells the caller something is off.

### D-01 `cylindrify` at the default `factor=1.0` collapses cap geometry, undiagnosed
**Owner** — · **Status** CONFIRMED (the reported repro case is imprecise)
`Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryOps_Modeling.cpp:1935`
- **Wrong** The radial map is `f(d) = (1-t)·d + t·R`, so `f'(d) = (1-t)`, which is **zero** at
  `t=1`: the map is constant in the radial coordinate. Any two vertices sharing a perpendicular
  direction and axis coordinate but differing in radius land on the same point. On a box the two
  cap faces are exactly that set — every interior cap grid vertex shares its `(θ, z)` with the rim
  vertex on its ray, so each cap collapses onto a circle. Cylindrify projects in 2-D only, so
  unlike `spherify` the surface is not star-shaped with respect to the projection.
- **The in-code justification is false at the endpoint** `GeometryOps_Modeling.cpp:1672` asserts the
  map is "strictly increasing in d … so it is a homeomorphism", and that comment block is the stated
  basis for not diagnosing. It holds for `t < 1` and fails at `t = 1`, which is the default —
  `MeshOpsHandler.cpp:1563`, `Ctx.GetNumber(TEXT("factor"), 1.0)`.
- **Why no warning fires** `GeometryOpsModeling_WarnRadialAnisotropy` measures cross-section aspect
  only, and is identically zero for a square cross-section at every factor — the code's own table at
  `:1728` reads "any cube, any factor 0.000". The existing diagnostic measures the wrong quantity for
  this failure mode.
- **Repro correction** The reported "plain 100³ cube" does not reproduce it. `geometry.create_box`
  defaults all three segment counts to 1 (`PrimitiveHandler.cpp:90-93`), giving a 12-triangle box
  whose 8 corners are equidistant from the axis — so `AvgRadius` equals their radius and `factor=1.0`
  is a no-op. The reported 24 degenerate / 28 inverted / 144 self-intersecting figures require a
  subdivided box, `segments=(5,5,4)`, exactly as `docs/wiki-src/geometry.md:468` states. The defect
  is real for **any subdivided box**; the headline repro was wrong.
- **Already documented, not already fixed** `docs/wiki-src/geometry.md:465-472` describes the
  collapse and says "not yet diagnosed at runtime". The doc is honest; the runtime is not.
- **Symptom** A subdivided box cylindrified with the default `factor` returns success and a vertex
  count, and hands back collapsed caps — degenerate, inverted and self-intersecting triangles.
- **Fixed** Warn when `Factor >= 1.0 - eps` on a mesh with more than one distinct perpendicular
  radius per direction. The second pass already computes `MinScale`/`MaxScale` per vertex, so a
  near-zero `MinScale` is available without another pass. Correct the `:1670-1674` comment in the
  same change.
- **Contract** Additive `warnings`. Changing the `factor` default away from `1.0` **would** break a
  published default and change output for every existing caller — that is the wrong fix.

### D-02 `animation.authoring.add_bone_track` creates a zero-key track and reports success
**Owner** `pwanim-animation-format.md` · **Status** CONFIRMED (runtime effect UNVERIFIED)
`Source/PinWright/Private/Handlers/Animation/AnimationAuthoringHandler_Sequence.cpp:407`
- **Wrong** `Controller.AddBoneCurve(BoneFName)` is the only mutation; the verb has no key, frame or
  transform parameter at all (`:379-381`). Success is unconditional at `:413`, and the
  `IsValidBoneTrackName` guard at `:405` means an already-existing track also reports `"added"`.
  **No verb anywhere writes bone-track keys** — `SetBoneTrackKeys` has zero product call sites across
  all 57 `animation.authoring.*` verbs, so the verb's only reachable outcome is a broken track.
- **UNVERIFIED** The claimed runtime consequence ("evaluates to identity, bone snaps to parent
  origin") could not be confirmed: the engine tree was not readable from the verifying session, so
  `UAnimDataModel::GetBoneTrackTransform`'s empty-key path was not read. What *is* confirmed in-repo
  is that the readback cannot detect it — `AnimSequenceDumpBuilder.cpp:239-243` derives `keyCount`
  from `GetBoneTrackTransforms(...).Num()`, which reports a full per-frame count for a track that was
  never keyed.
- **Symptom** The caller gets `success: true, "Bone track 'foot_l' added"` and an unusable track,
  with no API anywhere to put keys on it.
- **Fixed** Ship `set_bone_track_keys` (validating key count against `NumberOfKeys`), and make
  `add_bone_track` report `created` vs `alreadyExisted` plus `keyCount`.
- **Contract** New verb (additive); `add_bone_track` gains response fields. Blocked by D-30.

### D-03 `material=` on a generator nested in a boolean or hull body is discarded
**Owner** `pwmodel-value-system.md` · **Status** REFINED
`Source/PinWrightGeometry/Private/Model/PwModelCompiler.cpp:966`
- **Refinement** The report said `material=` "inside a boolean block" is silently accepted. Half
  right: `subtract material="X" { }` — on the boolean op itself — is already a hard error
  (`PWMODEL_MATERIAL_ON_BOOLEAN`, `PwModelParser.cpp:1896-1901`). The live gap is one level in:
  `subtract { sphere radius=18 material="Interior" }`. `ValidateParams` (`PwModelParser.cpp:2000`)
  passes the *nested op's own* `bBoolean` flag, false for `sphere`, so `material=` is legal there.
- **Wrong** `RunGenerator` applies the slot only when `!bNested` (`:966`), skipping slot allocation
  and `ClearMaterialIDs` (`:986-988`) wholesale. Hull bodies take the same path (`:2316`).
  `ValidateMaterialSlots` cannot see it either — it walks `Doc.Parts → Part.Ops` and never recurses
  into `Op.Children` (`PwModelParser.cpp:2794-2796`), deliberately, per the comment at `:2761-2764`.
- **Symptom** The document compiles clean and the slot reaches nothing. If a matching `materials { }`
  binding exists the author instead gets `PWMODEL_UNUSED_MATERIAL` blaming the *binding* — an error
  pointing at the wrong line.
- **Fixed** — but the OTHER way round, and the refinement above argued for the wrong direction.
  Rejecting a nested `material=` would have removed the only spelling a `union` block's geometry
  and a `subtract`'s cut walls ever had, and both were shipping on slot 0 — measured at 18,559 of
  21,420 triangles on one rifle. The tag is now HONOURED: ops inside a boolean block resolve
  through the same model-wide slot table, `material=` is accepted on the boolean op itself and
  names the faces the operation creates, untagged block geometry inherits the slot of the geometry
  the boolean was applied to, and `ValidateMaterialSlots` recurses into `Op.Children` for tags.
  Hull bodies keep the old behaviour: a boolean there is told apart by arriving with `bNested` set
  and no enclosing boolean block. See `B-pwmodel-boolean-output-takes-slot-zero`.
- **Contract** Grammar WIDENS rather than narrowing. No shipped example is invalidated.

### D-04 `cap` is silently dropped on `extrude_along_spline`
**Owner** `pwmodel-value-system.md` · **Status** CONFIRMED — **found while disproving the reported
`scale_start`/`scale_end` defect (see §10)**
`Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryOps_Advanced.cpp:590`
- **Wrong** The op passes `bLoop = true` unconditionally. The engine generator caps only when the
  path is *not* looped — `SweepGenerator.cpp:608`, `if (bCapped && !bLoop)` — so `Params.bCap` can
  never take effect. `PwModelParser.cpp:1005` documents `cap` as "Cap both ends", and
  `AdvancedMeshOpsHandler.cpp:416` publishes it on the RPC surface. The unconditional `bLoop` also
  wraps an open spline back onto its first frame.
- **Symptom** `cap=true` is accepted, echoed back, and produces an uncapped tube; a non-closed path
  silently closes.
- **Fixed** Drive `bLoop` from the path's own closedness (or a caller-visible parameter) and honour
  `cap`; until then, warn when `cap` is requested under a looped sweep.
- **Contract** Published parameter starts working, which changes emitted geometry for existing
  documents.

### D-05 `geometry.loft` reads only `Profiles[0].Extent` and overstates what it used
**Owner** `pwmodel-value-system.md` (blocked by D-80) · **Status** CONFIRMED
`Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryOps_Advanced.cpp:348`
- **Wrong** `const FVector ProfileExtent = Profiles[0].Extent;` — three differently-sized profile
  actors contribute three *locations* and one *size*. Interior profiles contribute nothing at all:
  only `Profiles[0].Location`, `Profiles.Last().Location`, `bHasMesh` on both endpoints and `Num()`
  are read. The handler-side adapter deliberately fills `Extent` only for the first entry
  (`AdvancedMeshOpsHandler.cpp:184-187`), with the reasoning at `:160` — known and unpublished.
- **Compounding** `profilesUsed` still reports `Profiles.Num()` (`:383`, echoed at
  `AdvancedMeshOpsHandler.cpp:209`), so the response overstates consumption. No warning is emitted.
- **Symptom** A loft through a small, a large and a small profile comes back as a constant-radius
  tube between the first and last locations, reporting `profilesUsed: 3`.
- **Fixed** Interpolate per-frame radius across all profile extents. Requires the nested
  list-of-lists value kind (D-80) to be authorable from `.pwmodel`; `format-decisions.md` already
  commits to that kind for exactly this reason. At minimum, warn and stop counting ignored profiles.
- **Contract** `profilesUsed` semantics, and the shape of any existing loft with more than two
  profiles.

### D-06 `sweep`'s `steps` is silently halved and means two unrelated things
**Owner** — · **Status** CONFIRMED
`Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryOps_Advanced.cpp:457`
- **Wrong** `const int32 NumPolySides = FMath::Clamp(Params.Steps / 2, 4, 32);` — half of `steps`,
  clamped, is the *side count of the fallback circular cross-section*, used when the caller supplied
  no profile of ≥3 vertices. Separately `steps` is the *path step count*
  (`SplinePathStepCount(Params.Steps)`, clamp `[2,256]`). Only the path meaning is warned about
  (`ClampRangeWarn` at `:503`); the `/2` is silent. Same construct in `ExtrudeAlongSpline` at `:557`.
- **Asymmetry worth noting** The `.pwmodel` front end *does* document both meanings
  (`PwModelParser.cpp:986-987`). Only the RPC description is silent —
  "Number of steps along sweep path (default 16)" (`AdvancedMeshOpsHandler.cpp:224`).
- **Symptom** Raising `steps` for a smoother tube also changes the profile: `steps=8` and `steps=9`
  give an identical 4-sided profile, and above 64 the profile stops changing entirely.
- **Fixed** Split into `steps` (path) and `profileSides` (fallback cross-section). The doc-only
  alternative — copy the `.pwmodel` wording and echo the derived side count — is contract-neutral,
  since `profileVertices` is already returned (`AdvancedMeshOpsHandler.cpp:284`).
- **Contract** Splitting adds a parameter and changes `steps`'s meaning. Documenting only is neutral.

### D-07 Five mesh-creating modifiers append at material ID 0 and cannot be tagged
**Owner** `pwmodel-value-system.md` · **Status** CONFIRMED (all five)
| verb | append site | mechanism |
|---|---|---|
| `bridge` | `GeometryOps_Advanced.cpp:214`, `:220` | raw 3-arg `AppendTriangle`, no material |
| `edge_split` | `GeometryOps_Advanced.cpp:302`, `:303` | raw `AppendTriangle` |
| `fill_holes` | `GeometryOps_Modeling.cpp:2008` | `FillAllMeshHoles` — engine API takes no material |
| `sweep` | `GeometryOps_Advanced.cpp:539` (options `:528-530`) | `FGeometryScriptPrimitiveOptions::MaterialID` left at its `0` default |
| `extrude_along_spline` | `GeometryOps_Advanced.cpp:589` (options `:587`) | same |
- **Wrong** None of the five registers a material parameter. The only geometry verbs that do are
  `append_buffers` and `bevel`.
- **Symptom** Geometry from these verbs always lands in slot 0, so bridged/filled/swept faces cannot
  take a different material without a separate selection pass.
- **Fixed** Add an optional `materialId` (default 0) to each. `sweep` and `extrude_along_spline` need
  only set the existing `PrimOptions.MaterialID`; `bridge`/`edge_split`/`fill_holes` need an explicit
  material-ID write over the newly created triangle set.
- **Contract** Additive only — a new optional parameter defaulting to today's behaviour.
- **Partly superseded at the `.pwmodel` layer, and only there.** `sweep` and
  `extrude_along_spline` now take `material="<Slot>"` in the op table and, untagged, retag the
  triangles they appended with the slot of the geometry they were appended onto rather than leaving
  them at 0 (`PwModelCompiler.cpp ApplyModifierMaterialTag`, warning
  `PWMODEL_MODIFIER_MATERIAL_AMBIGUOUS` when there is no single slot to inherit). That is a
  compiler-side retag of the op's output, not a `materialId` on the RPC verb: the five
  `geometry.*` verbs above still append at 0 and still have no parameter, and `bridge`,
  `edge_split` and `fill_holes` are untouched on both front-ends. This row stays CONFIRMED.

---

## 2. Persistence reported without measurement

The house rule is that a write is verified against disk, never against the in-memory object
(`docs/rpc-design.md`, three levels of persistence). The correct helper already exists and is
well-specified — `SaveAssetToDiskReportingPresence` (`Source/PinWright/Private/Utils/AssetUtils.cpp:610`)
captures size, timestamp and dirty state before the save and re-probes after, with an `ensureMsgf`
at `:733` forcing its bool to equal `IsAssetSaveStateDurable`. Every item here is call-site
convergence, not new infrastructure.

### D-10 `eqs.*` reports `saved` by echoing the caller's own request flag
**Owner** — · **Status** CONFIRMED · **Most severe item in this group**
`Source/PinWright/Private/Handlers/AI/EQSHandler.cpp:394, :449, :514, :589, :675, :799`
- **Wrong** Six sites do `if (bSave) { McpSafeAssetSave(Query); }` then
  `Result->SetBoolField(TEXT("saved"), bSave);` — `saved` is the input parameter, reflected. And
  `McpSafeAssetSave` (`Utils/AssetUtils.cpp:251-266`) is two statements, `MarkPackageDirty()` and
  `AssetCreated()`, which never touch disk by design; its own header says "NOTHING it does makes an
  edit durable."
- **Symptom** Every `eqs.*` mutator answers `saved: true` whenever the caller passed `save: true`,
  for a path that provably never writes a `.uasset`.
- **Fixed** Adopt the sibling shape that already does it right —
  `AI/StateTreeAuthoringHandler.cpp:501` calls `McpSafeAssetSave` then returns
  `IsAssetPersistedToDisk(...)` — or switch to `AddMarkDirtySaveReport`.
- **Contract** `saved` changes from an echo to a measurement and will typically flip `true` → `false`
  with `pendingFlush: true`.

### D-11 `blueprint.create` publishes `saved: true` before the save runs
**Owner** — · **Status** CONFIRMED
`Source/PinWright/Private/Handlers/Blueprint/BlueprintCreationHandler.cpp:421`
- **Wrong** `saved: true` is written at `:421`, `AddAssetVerification` probes disk at `:423`, the
  response is sent at `:434`, and the save runs at `:442` with its outcome discarded. The response
  therefore carries `saved: true` alongside a same-payload `existsOnDisk: false` — two fields in one
  object contradicting each other.
- **Symptom** An agent reads `saved: true`, proceeds, and loses the Blueprint on a cold restart.
- **Fixed** Move the save above the payload build and report the measured outcome.
- **Contract** Some responses flip to `saved: false, saveState: "deferred"`.

### D-12 Seventeen Blueprint sites compute `saved` from the API return with no disk probe
**Owner** — · **Status** CONFIRMED (17 of 26 — the report said ~20)
Representative: `BlueprintPropertyHandler.cpp:255`, `BlueprintFunctionHandler.cpp:484`,
`BlueprintEventHandler.cpp:431`, `BlueprintInfoHandler.cpp:258`, `BlueprintReparentHandler.cpp:95`
- **Wrong** These call `WasSavePersisted(SaveLoadedAssetThrottled(BP))`. That predicate is correct
  *as an engine-outcome test* — it scores `SkippedThrottledDirty`, `NotPersistable` and `Failed` as
  false. What it cannot see is the engine reporting success while nothing landed, because it never
  stats the file. Of the 26 `saved`-emitting sites in that directory, 3 probe disk
  (`BlueprintPropertyHandler.cpp:556`, `SCSComponentDuplicateHandler.cpp:355`, `SCSHandler.cpp:396`)
  and 6 are literal constants (`BlueprintCreationHandler.cpp:220, 243, 293, 375, 421`;
  `BlueprintFunctionHandler.cpp:607`, an honest `false`).
- **Mitigating** 17 of the 26 also call `AddAssetVerification`, which emits a real probe — under a
  *different field name* (`existsOnDisk`). The honest evidence is usually present; `saved` is the
  field that lies.
- **Fixed** Route the 17 through `SaveAssetToDiskReportingPresence` / `AddAssetSaveReport`. No change
  to `WasSavePersisted` itself — the fix is that no handler calls it directly.
- **Contract** `saved` narrows from "the engine did not report failure" to "the `.uasset` is on
  disk"; adds `saveState` / `pendingFlush`.

### D-13 Nine sites discard the save outcome entirely
**Owner** — · **Status** CONFIRMED (exactly 9 — 5 Blueprint + 4 Input, not "nine *more*")
`BlueprintComponentHandler.cpp:523`, `BlueprintCreationHandler.cpp:442`,
`BlueprintEventHandler.cpp:700`, `BlueprintGraphOrphanHandler.cpp:132`,
`BlueprintVariableCleanupHandler.cpp:122`, `InputHandler.cpp:61, :119, :162, :205`
- **Wrong** `SaveLoadedAssetThrottled(...)` called for effect, result dropped.
  `BlueprintCreationHandler.cpp:442` is the same site as D-11.
- **Symptom** A `Failed` or `SkippedThrottledDirty` save is swallowed and the response says nothing.
- **Fixed** Capture and report, or — where the save is genuinely fire-and-forget cleanup — log the
  non-durable outcome with the package name.
- **Contract** Additive.

### D-14 `skeleton.create_skeleton` and `skeleton.add_bone` report no persistence field at all
**Owner** `pwmodel-skeleton-skin-use.md` · **Status** CONFIRMED
`Source/PinWright/Private/Handlers/Animation/SkeletonHandler.cpp:549` and `:633`
- **Wrong** Both call `McpSafeAssetSave`, which makes nothing durable. Responses carry
  `skeletonPath`/`rootBoneName`/`boneCount` (`:551-556`) and `boneName`/`parentBone`/`boneCount`
  (`:635-640`) — no `saved`, no `pendingSave`, no `existsOnDisk`. Neither verb even exposes a `save`
  parameter.
- **Symptom** An agent builds a skeleton bone by bone, gets clean successes with a rising
  `boneCount`, and finds nothing on disk after a restart. This sits directly on the path to
  `bind_skin_weights`.
- **Fixed** `AddMarkDirtySaveReport(Result, Skeleton, true)` in both, plus a real `save` parameter
  routed to `SaveAssetToDiskReportingPresence`.
- **Contract** Additive response fields; a new `save` parameter is new semantics.

### D-15 `EditorQuitHandler` asserts `saved` as a literal
**Owner** — · **Status** CONFIRMED
`Source/PinWright/Private/Handlers/Editor/EditorQuitHandler.cpp:256`
- **Wrong** `bSaved = true` assigned, not measured.
- **Fixed** Measure, using the same helper as D-12.
- **Contract** The field may start reporting `false`.

---

## 3. The `save` contract itself

### D-30 `save: true` means "write the file" in `geometry.*` and "mark dirty" in `animation.authoring.*`
**Owner** `pwanim-animation-format.md` · **Status** CONFIRMED · **Gates D-02 and the whole
bone-keyframe write path**
`Source/PinWright/Private/Handlers/Animation/AnimationAuthoringHelpers.cpp:183-194` vs
`Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryAssetCreate.cpp:389-396`
- **Wrong** Geometry forces a disk write via `SaveAssetToDiskReportingPresence(..., bForce=true)`,
  with a comment at `:388` explaining it deliberately avoids the mark-dirty wrapper. Animation's
  `SaveAnimAsset` marks dirty and returns, with a comment saying "do NOT save to disk". The same
  parameter name spans ~20 animation verbs and the geometry cluster with two meanings. The
  animation-side parameter *descriptions* are honest about being dirty-only, so this is a
  same-name-different-meaning divergence rather than a false statement.
- **Symptom** A script that trusts one behaviour silently loses the animation half of its work.
- **Fixed** Pick one meaning — write-to-disk, per `rpc-design.md`'s persistence levels — and route
  `SaveAnimAsset` through the measuring helper. `format-decisions.md` already records that this must
  be settled before the bone-keyframe path can be built.
- **Contract** The largest in the plan: `save` semantics change across ~20 animation verbs, and
  callers relying on "save is cheap and deferred" start paying real I/O.

### D-31 `SaveAnimAsset` returns a constant under a measurement name
**Owner** `pwanim-animation-format.md` · **Status** CONFIRMED
`Source/PinWright/Private/Handlers/Animation/AnimationAuthoringHelpers.cpp:183-194`
- **Wrong** Returns literal `true` on both paths. This is the exact pattern `McpSafeAssetSave` was
  converted to `void` to make unwritable (`Utils/AssetUtils.h:55-62`); `SaveAnimAsset` still has the
  bool, and every caller discards it.
- **Fixed** Return the measured verdict, or `void`.
- **Contract** None — no caller reads it.

### D-32 `SaveAnimAsset` fires `AssetCreated` on every invocation
**Owner** `pwanim-animation-format.md` · **Status** CONFIRMED
`Source/PinWright/Private/Handlers/Animation/AnimationAuthoringHelpers.cpp:192`
- **Wrong** `FAssetRegistryModule::AssetCreated` is a *creation* notification, fired here for assets
  that already exist and are merely being edited.
- **Fixed** Fire it only on genuine creation.
- **Contract** None.

---

## 4. Time and unit model

### D-40 Two `DisplayFrameToTick` copies with different rounding, plus six test re-implementations
**Owner** `pwanim-animation-format.md` · **Status** CONFIRMED (six test mirrors, not one)
`Handlers/Sequencer/SequenceHandler.cpp:277` (`.FloorToFrame()` at `:282`) vs
`Handlers/Sequencer/ControlRigSequencerHandler.cpp:164` (`.RoundToFrame()` at `:172`)
- **Wrong** Both are file-internal statics, so neither is reachable from the other TU or from any
  test. The divergence is larger at the *input* than at the trailing rounding: `frame: 10.7` becomes
  display frame 10 in one and 11 in the other — a whole display frame apart. `SequenceHandler`'s own
  comment claims its choice "matches the transform-track branches' rounding", true only of its file.
- **Test mirrors, all copying the arithmetic because the helper is unreachable**
  `TestKeyframeExpandsTransformSection.cpp:76`, `TestSequenceAddKeyframeInterp.cpp:216`,
  `TestAddTrackIdentifier.cpp:53`, `TestSetPropertiesFramesToTicks.cpp:43`,
  `TestSequencerComponentBinding.cpp:883`, `TestSequencerControlRigTrack.cpp:153`.
- **Symptom** A Control Rig key and a transform key requested at the same fractional display frame
  land on different ticks.
- **Fixed** One exported helper (the `SequencerKeyInterp.h` cluster is the obvious home) with one
  documented rounding *and* one documented input-truncation policy, called by both handlers and the
  tests — which removes the reason the six mirrors exist.
- **Contract** Only for fractional/NTSC inputs. Integer frames at integral tick ratios are unaffected.

### D-41 `sequencer.add_level_visibility_track` mixes units between its two `range` paths
**Owner** — · **Status** CONFIRMED
`Source/PinWright/Private/Handlers/Sequencer/SequencerHandler.cpp:612`
- **Wrong** The default branch takes `MovieScene->GetPlaybackRange()` (`:602`), already in ticks. The
  caller-supplied branch casts the parameter straight to `FFrameNumber` with no `TransformTime`. The
  schema at `:557` reads "{ start, end } frame numbers; default = sequence playback range" — the two
  halves of one sentence are in different units. Magnitude is `TickResolution/DisplayRate`:
  24000/24 = 1000x.
- **Symptom** `range: {start: 0, end: 100}` yields a ~0.0042-second section that flickers sublevels
  within a single frame; omitting `range` yields a correct full-range section.
- **Fixed** Convert display frames to ticks the way `sequencer.add_section` already does, through the
  shared helper from D-40.
- **Contract** Redefines the units of the published `range` parameter. A caller who worked around the
  bug by pre-multiplying ticks breaks.

### D-42 `startFrame` means ticks on sub-section verbs and display frames on `add_section`
**Owner** — · **Status** CONFIRMED (inconsistency across verbs, not a defect within one)
`SequenceHandler.cpp:3219` and `:3306`, schemas at `:3169-3170` and `:3261-3262`
- **Wrong** These take bare `FFrameNumber(StartFrame)` and their schemas honestly say "Section start
  frame (tick resolution)" — correct by contract, but the same parameter name means display frames on
  `add_section`.
- **Symptom** A caller who learned `startFrame` on one verb is off by the tick ratio on the other.
- **Fixed** One unit across the namespace, or a name that carries the unit.
- **Contract** Changing either verb's units is breaking; renaming is the compatible route.

### D-43 `animation.authoring.set_sequence_length` resets a 60 fps sequence to 30
**Owner** `pwanim-animation-format.md` · **Status** CONFIRMED
`Source/PinWright/Private/Handlers/Animation/AnimationAuthoringHandler_Sequence.cpp:357`
- **Wrong** `SetFrameRate(FFrameRate(FrameRate, 1))` runs unconditionally, with `FrameRate` defaulted
  to the literal `30` at `:344` and never read from the asset. A call passing only `numFrames`
  silently rewrites the sample rate. Dead code alongside it:
  `if (Params->HasField(TEXT("frameRate"))) { /* already set above */ }` at `:359-362`, and
  `Duration` computed at `:354` and never used.
- **Symptom** Playback speed halves and keys resample, on a verb the caller used to set length.
- **Fixed** Default both parameters to the asset's current values, or write each field only when
  present. Drop the dead branch and the unused local.
- **Contract** Published defaults change from "30" to "the asset's current value".

---

## 5. Discarded error channels

The reported split was inverted. A precise count over
`Source/PinWrightGeometry/Private/Handlers/Geometry/`: **90 call sites** take a
`UGeometryScriptDebug*`, of which **11 pass a real sink and 79 still pass `nullptr`** — not the
reported "~8 remaining of ~90". Per-file `nullptr` distribution:
`GeometryOps_Modeling.cpp` 33, `GeometryOps_Primitives.cpp` 17, `GeometryOps_Advanced.cpp` 6,
`CollisionHelpers.h` 5, `SkeletalMeshAssetIOHandler.cpp` 5, `GeometryOps_Boolean.cpp` 4,
`MeshOpsHandler.cpp` 4, `GeometryUtils.cpp` 2, and one each in `GeometryOps_Elements.cpp`,
`LODCollisionHandler.cpp`, `MeshAssetIOHandler.cpp`.

### D-50 The 79-site debug-sink sweep
**Owner** — · **Status** CONFIRMED
- **Wrong** The large majority of geometry ops discard the engine's only failure channel, so
  refusals and partial work surface as success.
- **Fixed** Apply the `FGeometryScriptDebugSink` + `DrainWarningsInto` + `HasError` pattern already
  established in `GeometryScriptDebugSink.h`. Highest-value untouched clusters:
  `GeometryOps_Primitives.cpp` (17) and the UV/repair tail of `GeometryOps_Modeling.cpp`.
- **Contract** Per site: a call that succeeds today may start returning an error, and `warnings`
  becomes populatable. Land in tranches with the affected verbs' docs updated in the same change.

### D-51 `LayoutUV` still passes `nullptr`, and admits it in a comment
**Owner** — · **Status** CONFIRMED (its sibling `AutoUVPatchBuilder` is already fixed — see §10)
`Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryOps_Modeling.cpp:2275`
- **Wrong** The comment at `:2253-2255` states the problem outright: "LayoutMeshUVs sends its
  'UVSetIndex does not exist' complaint only to the GeometryScriptDebug argument, which is null here
  as everywhere." The `EnsureMeshHasUVChannel` pre-guard at `:2257` covers only that one failure mode.
- **Symptom** `geometry.layout_uv` reports success when the packer refused the layout for any reason
  the pre-guard does not cover.
- **Fixed** Grow the same sink block `AutoUVPatchBuilder` already has four lines away at `:2323-2332`.
- **Contract** Response-shape change: currently-successful calls can start returning
  `ERR_UV_GENERATION_FAILED`.

### D-52 Four boolean sites still pass `nullptr`
**Owner** — · **Status** CONFIRMED
`GeometryOps_Boolean.cpp:338` (`ApplyMeshSelfUnion`), `:508`, `:555`, `:591`
(the `AppendMeshTransforms` family)
- **Wrong** The boolean conversion landed at `:186, 293, 373, 392, 430` but stopped short of these.
- **Fixed** Same pattern; the file already has five worked examples.
- **Contract** As D-50.

### D-53 The OBJ parser drops out-of-range faces before the engine can report them
**Owner** — · **Status** CONFIRMED (residual — the import handler's own sink is already fixed)
`Source/PinWrightGeometry/Private/Handlers/Geometry/MeshIOHandler.cpp:504-506`
- **Wrong** `MeshIOParseObjText` range-checks OBJ indices itself and silently drops offending faces
  *before* the engine sees them, so the sink that was added at `:520` never learns about them. The
  comment at `:504-506` documents the behaviour.
- **Symptom** A malformed OBJ still loses geometry under a success response — the exact symptom the
  sink fix was meant to close, surviving one layer up.
- **Fixed** Count and report dropped faces; respect the existing `allowPartial` escape hatch
  (`:545`) rather than dropping unconditionally.
- **Contract** Additive response field; `allowPartial=false` starts refusing files it used to accept.

---

## 6. Contract and vocabulary

### D-60 Thirty verbs read parameter spellings the dispatcher refuses, and nothing cross-checks them
**Owner** — · **Status** CONFIRMED (30 verbs, 53 spellings — the report said ~32)
Root cause: `Source/PinWright/Private/Handlers/ParamSpec.h:33-40`
- **Root cause** The `RPC_PARAM_REQ` / `_OPT` / `_DEF` macros aggregate-initialise only the first
  five fields of `FParamSpec`, so **`Aliases` is silently empty for every macro-declared parameter**
  — 971 of 1194 registrations. Aliases must be attached by hand via
  `ParamAliasUtils::MakeAliasParamSpec`. The dispatcher seeds `KnownParams` from `Spec.Name` +
  `Spec.Aliases` + `Spec.TypedAliases` only (`RpcDispatcher.cpp:78`) and rejects everything else at
  `:159`.
- **The named example is worse than reported** `sequencer.set_playhead`
  (`SequenceHandler.cpp:1626`) declares six parameters with no aliases, *documents* two
  (`path`: "(alias: sequencePath)", `time`: "(alias: seconds)"), and its body reads **eleven**
  non-canonical spellings via `Ctx.Get*FirstOf` (`:1652-1737`) — `frameNumber`, `frame_number`,
  `seconds`, `timeSeconds`, `time_seconds`, `update_method`, `sequencePath`, `sequence_path`,
  `openIfNeeded`, `open_if_needed`, `force_update`. All eleven are refused, including both
  documented ones. The body's comment at `:1697-1700` ("Accept either so callers don't have to know
  which family they are talking to") describes dead code.
- **Scope** 27 macro-only verbs plus 3 helper-spec verbs confirmed by hand
  (`camera.animation_shots` at `Render/AnimationShotsHandler.cpp:88`;
  `geometry.create_from_static_mesh` at `Geometry/MeshAssetIOHandler.cpp:179`;
  `geometry.create_from_skeletal_mesh` at `Geometry/SkeletalMeshAssetIOHandler.cpp:458`). Of the 30,
  **18 additionally document the refused spelling** in the parameter description. Five more examples:
  `Actor/ActorPropertyHandler.cpp:181`, `Animation/PhysicsAssetHandler.cpp:1055`,
  `DataTable/DataTableAuthoringHandler.cpp:710`, `Animation/SkeletonHandler.cpp:139`,
  `Editor/PIEHandler.cpp:254`.
- **Nothing catches it** 46 test files mention aliases; every one is a per-verb regression test. The
  12 registry-wide tests in `Tests/Infra/TestContractConsistency.cpp` validate declarations against
  themselves and never read a handler body.
- **Symptom** An agent copies the alias printed in the verb's own wiki page and gets
  `UNKNOWN_PARAMS`, though the handler body was written to accept that exact key.
- **Fixed** One registry-wide contract test: for each registered method, source-scan its body for
  `Ctx.Get*FirstOf({...})` key lists and assert every key is in `CollectParamNames` of a declared
  spec. The parser already exists — `Tests/Core/TestNoParamHandlersReadNoArgs.cpp:348` does exactly
  this brace-matching and `FirstOf` detection, but only for registrations declared `RPC_NO_PARAMS`.
  Generalising it past the zero-param case catches all 30 at once. Then declare the aliases.
- **Contract** Declaring the aliases is a **compatible widening** — payloads rejected today start
  being accepted, nothing that works today breaks. The tempting alternative, deleting the extra keys
  and the alias sentences, is a **breaking narrowing** of a published contract and must not be done
  silently for any verb whose alias is already in the wiki.

### D-61 `UNKNOWN_PARAMS` is emitted but never registered, and the contract test cannot see it
**Owner** — · **Status** CONFIRMED (line number exact)
`Source/PinWright/Private/Dispatch/RpcDispatcher.cpp:159`
- **Wrong** `Ctx.SendError(TEXT("UNKNOWN_PARAMS"), Message)`, with zero `ERR_UNKNOWN_PARAMS` among
  the 771 constants in `Handlers/ErrorCodes.h` and no row in `docs/error-code-catalog.md`. Its
  sibling at `:114` emits `MISSING_REQUIRED_PARAM`, which *is* registered. It escapes
  `AllEmittedCodesAreRegistered` because `ResolveAllHandlerSourceDirs()`
  (`Tests/Core/TestErrorCodeRegistry.cpp:48-74`) globs `Source/PinWright*/Private/Handlers` — six
  roots — and `Private/Dispatch/` is under none of them. The catalog states this boundary explicitly
  at `docs/error-code-catalog.md:22-31`.
- **Symptom** The most frequently hit error code in the product — every wrong-key call — is absent
  from the catalog agents read to recover.
- **Fixed** Declare `ERR_UNKNOWN_PARAMS`, switch the emit to the constant, and extend the test's root
  discovery to `Private/Dispatch/`, keeping the "roots are discovered, not listed" property the
  test's header comment is proud of.
- **Contract** No wire change — the string is already on the wire; registering only makes the catalog
  honest. **Do not rename it while registering**; that would be the breaking change.

### D-62 One production verb registers outside the scanned roots
**Owner** — · **Status** CONFIRMED (one file, not many)
`Source/PinWright/Private/PinWright_BlueprintHandlers_List.cpp:19`
- **Wrong** `blueprint.list` registers outside every scanned root and emits three raw
  `SendError(TEXT("INVALID_ARGUMENT"))` literals at `:35`, `:124`, `:130`. That code happens to be
  registered, so nothing is broken today — but nothing is checking either. The sibling
  `PinWright_SCSHandlers.cpp` registers nothing yet emits through `ErrorCodes::` constants from
  outside the roots, invisible twice over since the test matches only raw literals. The other
  unscanned registrations (10) are all test fixtures.
- **Fixed** Add these loose files to the test's root discovery, or move them into `Handlers/`.
- **Contract** None — test scope only.

### D-63 `ERR_UnsupportedNodeClass` is the only non-`SCREAMING_SNAKE` code
**Owner** — · **Status** CONFIRMED (verified twice — it is the only one of 771)
`Source/PinWright/Private/Handlers/ErrorCodes.h:1114`
- **Wrong** `ERR_UnsupportedNodeClass[] = TEXT("UnsupportedNodeClass")`. Live, not vestigial: emitted
  three times as a raw literal from `Blueprint/BlueprintGraphCrudHandler.cpp:980, 995, 1002`,
  asserted by `Tests/Blueprint/TestBlueprintReplaceNode.cpp:555`, and carried in the catalog at
  `docs/error-code-catalog.md:314`.
- **Symptom** A client matching error codes on `^[A-Z0-9_]+$` silently fails to match this one, and
  it reads as a leaked C++ identifier next to the separate `UNSUPPORTED_NODE`.
- **Fixed** Two options, and the choice is the open question in §11. Renaming the wire value to
  `UNSUPPORTED_NODE_CLASS` is a **breaking change to a published code**. Renaming only the C++
  constant (`ERR_UNSUPPORTED_NODE_CLASS[] = TEXT("UnsupportedNodeClass")`) restores registry-naming
  consistency at **zero contract cost**.
- **Contract** Breaking in one variant, free in the other. Note `ERR_UNSUPPORTED_NODE` already exists
  separately, so this is not a merge.

### D-64 Thirty-two registered error constants are genuinely dead
**Owner** — · **Status** CONFIRMED at 32 (the reported 47 was wrong — see §10)
`Source/PinWright/Private/Handlers/ErrorCodes.h`
- **Wrong** Of 771 declarations, 553 are never referenced *by symbol* — which is normal, since most
  codes are emitted as raw literals. The genuinely dead set, where the symbol is unreferenced **and**
  the string value appears nowhere else in `Source/`, is **32**: `ERR_ALREADY_PLAYING`,
  `ERR_ASC_NOT_FOUND`, `ERR_AUDIO_UNSUPPORTED_FORMAT`, `ERR_BLUEPRINT_CREATE_FAILED`,
  `ERR_CALL_FAILED`, `ERR_CLASS_NOT_A_COMPONENT`, `ERR_COMMAND_BLOCKED`, `ERR_COMMAND_FAILED`,
  `ERR_CREATE_DYNAMIC_LIGHT_FAILED`, `ERR_CVAR_NOT_FOUND`, `ERR_DELETE_PARTIAL`,
  `ERR_EXPRESSION_CREATION_FAILED`, `ERR_FACTORY_CREATION_FAILED`, `ERR_MERGE_TOOL_MISSING`,
  `ERR_MUTE_FAILED`, `ERR_NODE_CREATION_FAILED`, `ERR_NOT_PLAYING`, `ERR_NO_LEVEL`,
  `ERR_NULL_EXPRESSION`, `ERR_PARAM_FAILED`, `ERR_PERCEPTION_COMPONENT_NOT_FOUND`,
  `ERR_PLAY_FAILED`, `ERR_PROBE_CREATE_FAILED`, `ERR_SC_DISABLED`, `ERR_SERIALIZE_FAILED`,
  `ERR_SPLIT_SCREEN_ERROR`, `ERR_STATE_FAILED`, `ERR_STOP_FAILED`, `ERR_UNSUPPORTED_NODE`,
  `ERR_UNSUPPORTED_VERSION`, `ERR_USE_ASSET_IMPORT`, `ERR_VOICE_CHAT_ERROR`.
- **The more interesting inverse** Live codes are widely emitted as raw literals rather than through
  their constants, so a typo in the literal is caught only by the registry test, never by the
  compiler.
- **Fixed** Delete the 32, or wire the verbs that were meant to emit them. Separately, convert
  literal emits to their constants.
- **Contract** Deleting the 32 changes no published contract — none of those strings can reach a
  client. Converting literals to constants is also neutral. The only breaking move would be renaming
  a live code during the conversion.

### D-65 `geometry.simplify_collision` publishes almost nothing, changed silently, and is undocumented
**Owner** — · **Status** CONFIRMED (all three parts)
`Source/PinWrightGeometry/Private/Handlers/Geometry/LODCollisionHandler.cpp:125`
- **Wrong** `FGeometryScriptSimplifyMeshOptions SimplifyOptions;` is fully default-constructed —
  **17 `UPROPERTY` fields, 0 published**, exactly as reported.
  `FGeometryScriptCollisionFromMeshOptions` has another 17; the verb pins two in code and exposes
  exactly one (`targetHullCount` → `MaxConvexHullsPerMesh`, re-clamped to `1..16` where the engine
  field is uncapped). **34 reachable engine options, 1 published.**
- **Behaviour already changed, uncommitted** The working-tree diff removes
  `SimplifyOptions.Method = StandardQEM` and `bAllowSeamCollapse = true`, so the effective simplifier
  moved to the engine default `AttributeAware`. Nothing echoes the method, so callers cannot see it.
- **Undocumented** `grep -rn "simplify_collision" docs/` returns zero matches, including
  `wiki-src/`, while its siblings `generate_collision` and `generate_complex_collision` are
  documented.
- **Symptom** Callers get a mesh simplified by an unnamed, unselectable algorithm that silently
  changed, with 33 of 34 knobs unreachable and no page to read.
- **Fixed** Publish the options the verb needs (at minimum `method`), echo the effective method, and
  add a `geometry.simplify_collision` section to `docs/wiki-src/geometry.md`.
- **Contract** The algorithm swap **already** changed behaviour with no doc and no response field
  naming it — that is the contract break, and it is uncommitted, so it can still be landed honestly.

### D-66 `AppendSweepPolygon` hardcodes two engine knobs with no comment
**Owner** `pwmodel-value-system.md` · **Status** CONFIRMED
`Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryOps_Advanced.cpp:69-86`
- **Wrong** `MiterLimit` pinned to `1.0f` and `RotationAngleDeg` to `0.0f` in both engine-version
  arms, unreachable from either front end, with no comment recording the choice — unlike every other
  deliberate non-publication in this codebase, which is documented at the site.
- **Fixed** Publish both, or document the pin where it is made.
- **Contract** Additive.

### D-67 `array_along_path` promises a bound the compiler does not apply
**Owner** `pwmodel-value-system.md` · **Status** CONFIRMED
`Source/PinWrightGeometry/Private/Model/PwModelParser.cpp:970-972` vs `PwModelCompiler.cpp:1616-1623`
- **Wrong** The op-table description says "1 to 100 of them"; the compiler branch deliberately skips
  `ValidatePathFrames` and relies on `ArrayAlongPath`'s internal `ValidateArrayCount`. Either the
  published number is wrong, or the two are an un-asserted coupling.
- **Fixed** Read the constant through the ops layer the way `SplinePathStepCount` already is
  (`PwModelCompiler.cpp:279-288`), rather than retyping it in prose.
- **Contract** The published bound may change to the real ceiling.

### D-68 Diagnostics tell authors to wait for milestones that do not exist
**Owner** `pwmodel-skeleton-skin-use.md` · **Status** CONFIRMED
`Source/PinWrightGeometry/Private/Model/PwModelCompiler.cpp:1963`, `:1970`, `:1973-1974`
- **Wrong** Three runtime strings name "milestone 2" and "milestone 3", directly contradicting
  `format-decisions.md:13-15` ("There is no M2/M3/M4"). The same framing is published in
  `docs/pwmodel-format.md:181` and `:985`, `docs/pwmodel-design.md:186`, `docs/wiki-src/model.md:61`
  and `docs/wiki-src/model.authoring.md:244`.
- **Symptom** The error tells an author to wait for a numbered milestone the project abolished, with
  no way to learn what actually happens next.
- **Fixed** Name the format that will own the construct (`.pwanim`, skeleton/skin work) or say "not
  implemented" flatly. `TestPwModelCompiler.cpp:388` asserts the milestone word and moves with them.
- **Contract** Message text and five doc sites. No grammar change.

### D-69 `use animation from` is parseable and meaningless
**Owner** `pwmodel-skeleton-skin-use.md` · **Status** REFINED
`Source/PinWrightGeometry/Private/Model/PwModelParser.cpp:2494-2495`
- **Refinement** The report said it is "accepted and silently discarded". It is **not silent**: the
  compiler hard-errors on *every* `use`, kind-independent, at `PwModelCompiler.cpp:1961-1966`
  (`PWMODEL_UNSUPPORTED_IN_VERSION`). The real defect is narrower — `animation` sits in `ValidKinds`
  even though, with `.pwanim` as its own format, an animation references a skeleton and never the
  reverse, so nothing will ever reference an animation *from* a `.pwmodel`.
- **Note** `.pwanim` does not exist in `Source/` yet; it is a plan, not shipped code.
- **Fixed** Drop `animation` from `ValidKinds`, or keep it and have the message name `.pwanim`
  instead of a milestone (folds into D-68).
- **Contract** Grammar narrows; `docs/pwmodel-format.md:185-186` documents the kind list.

### D-6A Model-level keyword vocabulary disagrees with itself three lines apart
**Owner** `pwmodel-skeleton-skin-use.md` · **Status** CONFIRMED
`Source/PinWrightGeometry/Private/Model/PwModelParser.cpp:2700` vs `:2693-2695`
- **Wrong** The `UnexpectedToken` string lists "part, materials, collision, lightmap or use",
  omitting `skeleton`/`skin`/`animation`, while `ModelKeywords` — used by the did-you-mean path at
  `:2733-2743` — includes all three.
- **Symptom** An author who writes a non-identifier is told a shorter vocabulary than one who
  misspells a construct name.
- **Fixed** One vocabulary list, both paths.
- **Contract** Message text only.

### D-6B `convert_to_skeletal_mesh`'s `ASSET_EXISTS` warning under-reports the damage
**Owner** `pwmodel-skeleton-skin-use.md` · **Status** CONFIRMED
`Source/PinWrightGeometry/Private/Handlers/Geometry/SkeletalMeshAssetIOHandler.cpp:1036-1038`
- **Wrong** The warning lists "LODs, materials, reference skeleton and physics asset".
  `CreateSkeletalMeshUtil.cpp:60-63` also calls `UnregisterAllMorphTarget()` on the reuse path, so
  morph targets are destroyed and the warning does not say so. Same omission in the comment at
  `:1028-1031`.
- **Symptom** An author accepts an overwrite believing morph targets survive it.
- **Fixed** Name morph targets in the message and the comment.
- **Contract** Message text only.

### D-6C Eight capture verbs share fit/framing helpers and expose different halves of them
**Owner** `preview-scene-rig.md` §5 · **Status** CONFIRMED
Verified against the tree on 2026-08-21. `preview-scene-rig.md` §5.1 fixed **P1 only** — `camera.orbit_shots`
gained `padding`, `camera.frame_actor` gained `distance` — because that pair is one file and one helper.
The rest are recorded here rather than fixed: each moves a default that has already shipped, and one of
them records a deliberate per-verb decision in a comment. `preview-scene-rig.md` §5.3's R7 writes the
cross-verb `FParamSpec` walk that turns each row below into a failing test the day someone tries to fix
it, and it lands with these rows in its **exception table**, each citing the `file:line` of its rationale.
A gap with no rationale row fails the check.

- **P2** `render.capture_asset_preview` welds **both** inputs of `ComputeFitDistance` — no `radius`, padding
  hardcoded `1.25f` (`RenderHandler.cpp`, `constexpr float AssetPreviewPadding`). The divergence was
  noticed and accepted per verb; the comment above the constant is the rationale row.
- **P3** Three padding defaults for one concept — **1.15** (`camera.frame_actor`, `camera.orbit_shots`,
  `camera.animation_shots`), **1.25** (`render.capture_asset_preview`), **1.4**
  (`render.capture_animation_preview`, and the only one whose reason is documented). Two are unreachable
  from the wire. Post-convergence the *same* skeletal mesh frames at 1.15 through `camera.frame_actor` and
  1.4 through `render.capture_animation_preview`.
- **P4** `orthoWidth` and free-camera `location` / `rotation` split along the namespace line: the three
  `render.*` still verbs expose both, all four bounds-fitting verbs expose neither. Declared as raw
  `FParamSpec` three times with no shared macro.
- **P5** *The widest gap.* `viewDistanceScale` is plumbed through `FPoseListCaptureRequest`, forwarded per
  frame, and **assigned by no handler** — while the wire parameter exists on two verbs that take the
  single-shot path instead. Dead on every path that declares it, undeclarable on every path that carries it.
- **P6** `bWarmupShot` documents a caller affordance in `PoseListCapture.h` ("a caller that knows the
  viewport is warm can turn it off") with no wire spelling.
- **P7** `allowBlank` reaches one of five pose-capture verbs. `rejectBlank`'s absence is reasoned and
  documented in `CameraShotPlanUtils.h`; `allowBlank`'s is not covered by that rationale.
- **P8** `distribution`'s description is hand-copied four times with divergent prose and no shared macro
  (`CameraFrameHandler.cpp`, `AnimationShotsHandler.cpp`, `AnimationPreviewCaptureHandler.cpp`,
  `RenderHandler.cpp`).
- **P9** `render.capture_ortho_tiles`'s `exposure` forks the shared vocabulary — required rather than
  optional, with its own description text instead of `PINWRIGHT_EXPOSURE_PARAM_DESC`. Low severity; the
  requirement itself is correct and documented.

- **Symptom** A caller cannot reach a default that another verb exposes, and a fully green per-verb matrix
  is compatible with a welded constant — the convergence wave could not have caught P1 because both of its
  matrices used the same axis (subject domain) and the word `padding` appears nowhere in its 811 lines.
- **Fixed** R7's `SharedHelperInputsAreExposedUniformly` walk plus the rationale-citing exception table.
- **Contract** P1 only, already landed. P2–P9 change no behaviour until someone converges the defaults,
  which is a separate breaking wave.
- **GAP, explicitly** The internal welded constants of `AnnotatedCaptureHandler.cpp` (annotation sizing),
  `OrthoTileCaptureHandler.cpp` (tile planner) and `ZFightingHandler.cpp` were **not** read beyond their
  `RPC_PARAMS` blocks, and alias-key coverage (`orthoWorldWidth`, `ActorNameKeys()`) outside the framing
  family is unchecked. Gaps, not N/As.
- **Line numbers deliberately omitted above.** Every `file:line` in `preview-scene-rig.md` §5.1 had drifted
  by 10–80 lines within a day of being written, and one comment block in `RenderHandler.cpp` moved 22 lines
  in a single afternoon. Cite by symbol and re-locate.

---

## 7. Stability

### D-70 `asset.bulk_delete` can wedge the game thread behind an invisible modal dialog
**Owner** — · **Status** REFINED
`Source/PinWright/Private/Handlers/Asset/AssetWorkflowHandler.cpp:515-516`
- **Refinement** The incident that took down the automation suite was a **crash, not a hang**, and it
  is already fixed. `Utils/RedirectorFixupPolicy.h:6-24` documents it precisely: `FixupReferencers` →
  unconditional modal report → unchecked `TOptional::GetValue()` → assert, "Reproduced 2026-08-19
  16:05:49 through asset.bulk_delete". `bulk_delete` no longer calls that path (`:543-547`).
- **Wrong (still live)** `ObjectTools::DeleteObjects(ObjectsToDelete, bShowConfirmation)` passes a
  caller-controlled flag into a path that builds a modal window and calls `EditorAddModalWindow` — a
  nested Slate loop that owns the game thread, which no RPC can dismiss. The plugin's unattended
  guard (`FScopedUnattendedRpc`, applied at `RpcDispatcher.cpp:335-336`, default on via
  `bSuppressModalDialogsDuringRpc`) normally cancels the window, so the wedge requires modal
  suppression to be off. A secondary uncancellable stall exists in the engine's "Verifying Delete"
  slow task, bounded by object count rather than infinite.
- **Fixed** Never pass a caller-controlled `true` — hard-code `false`, or reject
  `showConfirmation: true` up front with an unattended-constraint error.
- **Contract** Removes or redefines the published `showConfirmation` parameter.
- ⚠️ `RedirectorFixupPolicy.h/.cpp` and its test are **untracked**, and `AssetWorkflowHandler.cpp` is
  modified-uncommitted: the crash fix exists only in the working tree. See D-91.

### D-71 `showConfirmation: true` is a "delete nothing" switch
**Owner** — · **Status** CONFIRMED
`AssetWorkflowHandler.cpp:465` → `:516` → `:519` → `:572-573`
- **Wrong** Under the unattended guard the modal auto-cancels, `GetDeletedObjectCount()` returns 0,
  the fixup is skipped, and the handler answers `BULK_DELETE_FAILED — "Deleted 0 of N assets"`. It
  still loads the assets and broadcasts `OnAssetsPreDelete` first.
- **Symptom** The parameter always fails the call, with nothing in the error naming it as the cause.
- **Fixed** Delete the parameter — silent delete is the only viable mode over RPC — or reject it with
  a message naming the constraint.
- **Contract** Removes a published boolean.

### D-72 `bulk_delete`'s redirector fixup DELETED host packages the caller never named
**Owner** — · **Status** RESOLVED 2026-08-21
`AssetWorkflowHandler.cpp` `asset.bulk_delete` · board `B-tests-destroy-host-assets`
- **Wrong** The `FARFilter` set `ClassPaths` only — `PackagePaths` and `bRecursivePaths` were never
  set, so the query covered every redirector in the project. The verb exposed no scope parameter and
  `fixupRedirectors` defaults to `true`, so this was the DEFAULT path.
- **Symptom, corrected.** This was filed as a performance defect ("deleting three assets in one
  folder freezes the editor for minutes") and the destructive half was never written down. It is
  **data loss**: the sweep feeds every redirector in the project to
  `RedirectorFixupPolicy::FixupReferencers(..., bDeleteFixedUpRedirectors=true)`, which re-saves the
  referencing packages and then **deletes the redirector packages**. Measured in host
  the development host on 2026-08-19: **4 pre-existing `ObjectRedirector` packages under
  `Content/ExampleContent` irreversibly deleted and 13 referencing host packages rewritten**,
  recovered only because they were tracked in git. The response reported none of it. The perf
  framing is why this sat below its real severity — it would not have earned Critical.
- **Fixed** `fixupScope` (string, default `"paths"`) bounds the sweep to the deleted assets' own
  package folders, non-recursively; `"project"` is the explicit opt-in to the old behaviour and an
  unrecognised value is refused with `INVALID_ARGUMENT` before anything is deleted. The scope
  decision lives in `RedirectorFixupPolicy::BuildSweepFilter` / `ESweepScope`, so the two verbs
  cannot drift, and an empty resolved folder set matches **nothing** rather than everything (an
  empty `FARFilter::PackagePaths` means "every path" — the original bug's narrow back door).
  Collateral is now reported: `FResult::DeletedRedirectorPackages` names the packages actually
  removed (measured post-delete off a stale weak pointer, not predicted from the request), surfaced
  as `redirectorsDeletedPaths[]` on both verbs and as `redirectorsDeletedOutsideScope[]` — always
  present, empty array included — on `bulk_delete`.
- **Deliberately NOT changed:** `asset.fixup_redirectors`' empty-`directoryPath` default stays
  project-wide. See D-75's note below.
- **Tests** `PinWright.asset.bulk_delete.OutsideRedirectorsSurviveTheDefaultScope`,
  `.ProjectScopeStillReachesOutsideRedirectors`, `.EmptyPathSetMatchesNothing`,
  `.UnknownFixupScopeIsRejected`, `.FixupScopeIsDeclaredWithPathsDefault`
  (`Tests/Assets/TestBulkDeleteRedirectorScope.cpp`). No test dispatches the verb at
  `fixupScope: "project"` — that would delete every redirector in the host, which is the damage
  being fixed; the opt-in is asserted on the filter against the live registry as a read.
- **Contract** Narrows a documented default and adds a parameter. `docs/wiki-src/asset.md` had
  documented the whole-project sweep as intentional and now documents it as the defect it was.
- **Still open, separately** The unbounded *cost* half — the fixup still loads and re-saves every
  referencing package it finds, synchronously (D-73). Scoping the default shrinks the usual case
  dramatically but `fixupScope: "project"` remains a game-thread freeze.

### D-73 `bulk_delete` is fully synchronous with no job handle or cancellation
**Owner** — · **Status** CONFIRMED
`AssetWorkflowHandler.cpp:468-576`
- **Wrong** The whole body runs inline inside the dispatcher. No `FAsyncResponseToken`, no ticket, no
  progress, no cancel check — while the sibling verb doing the *same* redirector work,
  `asset.fixup_redirectors`, already runs deferred (`:54`).
- **Symptom** A large delete blocks the MCP endpoint for its duration; the only escape is killing the
  editor.
- **Fixed** Move onto the async-token path the sibling already uses.
- **Contract** Additive but visible — synchronous callers start receiving a job ticket.

### D-74 `asset.import`'s deferred continuation runs without the unattended guard
**Owner** — · **Status** FIXED
`Source/PinWright/Private/Handlers/Asset/AssetManageHandler.cpp` · `Dispatch/SafePoint.h`
- **Wrong** The editor-timer continuation kept only an async response token. It called
  `ImportAssetsAutomated` and `RenameAssets` after the synchronous handler's unattended and active-
  request scopes had closed, allowing a modal dialog or another RPC to interleave with the import.
- **Fixed** `asset.import` now uses `PinWrightSafePoint::DeferRequestToSafePoint`. The helper always
  takes one core-ticker hop, retains the dispatcher's active request until the continuation returns,
  re-enters the unattended scope through `DeferToSafePoint`, and responds through
  `FSafePointResponder`. `PinWright.asset.import.SafePointContinuationRetainsRequestScope` covers
  deferral, queue order, safe-point/unattended state, response routing, and guard release.
- **Contract** None.

### D-75 Five more asset verbs sweep the project by default
**Owner** — · **Status** CONFIRMED
`AssetWorkflowHandler.cpp:67-82` (`fixup_redirectors`, mitigated: scope parameter, async token,
documented default) · `AssetMaterialHandler.cpp:66-71` (`list_material_instances`, no path parameter
at all) · `AssetManageHandler.cpp:1176-1178` (`search`, `GetAllAssets` when unscoped) ·
`AssetQueryHandler.cpp:249-273` (`search_assets`, unset `packagePaths` = project-wide) ·
`AssetManageHandler.cpp:836-860` (`list`, forced `ScanPathsSynchronous` on `/Game` every call)
- **Wrong** In each, a `limit` caps the *output* while the *scan* stays unbounded.
- **Fixed** Scope by default; make project-wide an explicit opt-in.
- **Contract** Narrows documented defaults.
- **Decision 2026-08-21 (`fixup_redirectors` only):** its empty-`directoryPath` default was
  reviewed alongside D-72's fix and **deliberately left project-wide**. The three mitigations D-72
  lacked all hold here — the sweep *is* the verb's job, so a caller who typed its name asked for it;
  it takes an explicit scope parameter; and the default is documented. Narrowing it would break a
  published contract for callers who use it as the post-`bulk_rename` cleanup, to remove a hazard
  they opted into by name. The decision and its re-open condition are recorded at the call site in
  `AssetWorkflowHandler.cpp`. The other four in this row are untouched and still open; note none of
  them *deletes* — they are cost defects, which is what D-72 was mistaken for.

---

## 8. Format grammar, examples and tests

### D-80 Nested objects, general maps and list-of-lists are all rejected, with a generic diagnostic
**Owner** `pwmodel-value-system.md` · **Status** CONFIRMED · **Blocks D-05**
`PwModelParser.cpp:1609-1611` (no `OpenBrace` case in `ParseValue`), `:1584-1588` (a list must hold
tuples), `:1497-1506` (tuple members must be numbers)
- **Wrong** Value kinds are `None`/`Number`/`Tuple`/`TupleList`/`String`/`Identifier`
  (`PwModelAst.h:18-26`), with numeric-only tuple storage. `[[1,2],[3,4]]`, `["a","b"]` and
  `key = { }` all fail. Neither tuples nor lists may span lines (`:1516-1520`, `:1579-1583`).
- **Symptom** Anything needing a profile list, an options object or a named map is unauthorable, and
  fails with `PWSRC_UNEXPECTED_TOKEN` — "expected a value" — which does not say the kind is
  unsupported.
- **Fixed** Add the kind per the value-system plan. Independently and immediately: the `default:` arm
  should name the kinds that exist rather than saying "a value".
- **Contract** Grammar addition. No example uses these today.

### D-81 A false technical reason is published in four places
**Owner** `pwmodel-value-system.md` · **Status** CONFIRMED
`Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryOps_Modeling.h:940-943`
- **Wrong** The comment says `UDIMResolutions` cannot be carried because it is a `TMap<int32,int32>`
  and "`FPwModelValue` has no map member … so a per-tile resolution table has no literal in the
  format". `[(1001, 2048), (1002, 1024)]` is a legal `TupleList` today — the comment confuses the C++
  container with the literal. It has been copied into `pwmodel-format.md:773-777`,
  `docs/wiki-src/model.authoring.md`, and the `enable_udim_layout` parameter description
  (`PwModelParser.cpp:817-819`): four places asserting an unreachability that was never real.
- **Symptom** The next author believes a reachable feature is unreachable and does not build it.
- **Fixed** Correct all four; ship the parameter.
- **Contract** Additive parameter.

### D-82 The `PwModel*` prefix needs extracting before grammar work lands
**Owner** `pwmodel-emitter-and-migration.md` · **Status** CONFIRMED · **Gates all grammar work**
- **Wrong** `FPwModelDocument` hard-codes Parts/Materials/Collision/Lightmap, which
  `format-decisions.md` names as the seam needing the most work now that `.pwanim` must share the
  core. The rename is measured at 3,220 occurrences across 34 source files plus 8 docs.
- **Fixed** One behaviour-preserving `PwModel*` → `Pw*` commit *before* the value-system additions,
  so each addition is not renamed twice.
- **Contract** Internal only.

### D-83 Three op parameters were removed inside version 0 with no deprecation path
**Owner** `pwmodel-emitter-and-migration.md` · **Status** CONFIRMED
`bridge subdivisions`, `recalculate_normals split_angle`, `trim keep_inside`
- **Wrong** Documents written against last week's vocabulary now fail with `PWSRC_UNKNOWN_PARAM`
  and no message distinguishing "removed" from "misspelled". Contrast `max_hulls`, which got a
  warning and continued acceptance. Version 0 permits the removal; the *inconsistency* with the
  `max_hulls` treatment is the defect.
- **Fixed** The `DropParam` rule kind in §7.2 of the emitter plan.
- **Contract** Diagnostic text; no grammar change.

### D-84 The AST records no block end position
**Owner** `pwmodel-emitter-and-migration.md` · **Status** CONFIRMED
`PwModelAst.h:51-52, 68-69, 106-107`
- **Wrong** `FPwModelPart` / `FPwModelOp` / `FPwModelCollision` store only the opening
  `Line`/`Column`, so the AST cannot express "inside this block" as a line range.
- **Symptom** Invisible today; blocks any trivia-preserving emitter and any source-range diagnostic.
  The emitter ships first in dependency order, so this lands early.
- **Fixed** Record end positions when the emitter's AST work lands.
- **Contract** Internal.

### D-85 Latent: the merge drops skin weights for parts appended before the first weighted part
**Owner** `pwmodel-skeleton-skin-use.md` · **Status** CONFIRMED (latent — not yet reachable)
- **Wrong** `FDynamicMeshEditor::AppendMesh` copies weights only when the destination already holds
  the profile; the profile is attached at the first append that carries one, and is sized against a
  mesh that already holds the earlier parts' vertices — which come out with empty `FBoneWeights`.
  Nothing reports it: `ValidateSkinWeightAttribute` accepts all-zero weights, and
  `FSkeletalMeshAttributes::Register` guarantees a profile always exists.
- **Symptom** None today — no skin weights exist anywhere yet. It becomes a silent-wrong-output
  defect the moment skinning ships, which is why it is listed now.
- **Fixed** The `PWMODEL_SKIN_INCOMPLETE` rule and the mandatory coverage scan in the skeleton plan.
- **Contract** New diagnostic.

### D-86 Two shipped examples emit a deprecation warning on every compile
**Owner** `pwmodel-emitter-and-migration.md` · **Status** CONFIRMED
`Examples/pwmodel/crystal_cluster.pwmodel:155`, `Examples/pwmodel/ships_wheel.pwmodel:219`
- **Wrong** Both use `auto ... max_hulls=`, triggering the rename warning at
  `PwModelCollision.cpp:458-463` unconditionally. The example corpus is the reference documentation
  for the format and should not demonstrate the deprecated spelling.
- **Fixed** `max_hulls_per_component=` in both, and rewrite the two comment blocks that explain the
  old name.
- **Contract** Two example files. `max_hulls` stays accepted.

### D-87 `ships_wheel.pwmodel` documents a parameter that no longer exists
**Owner** `pwmodel-emitter-and-migration.md` · **Status** CONFIRMED — **not** fixed by `1ac056d9`
`Examples/pwmodel/ships_wheel.pwmodel:131-132`
- **Wrong** The comment says "`recalculate_normals split_angle=` is accepted and ignored".
  `recalculate_normals` now registers only `area_weighted` and `angle_weighted`
  (`PwModelParser.cpp:485-488`), so that spelling is `PWSRC_UNKNOWN_PARAM` — an error, not an
  ignore. The comment is inverted, and it is the only place in the corpus recording the behaviour.
- **Also in the same file** `:206` says of `max_hulls` "and nothing warns"; it now warns on every
  compile. Fix both in one pass with D-86.
- **Fixed** Say `recalculate_normals` takes no angle and that `split_angle` there is now rejected.
- **Contract** Comment only.

### D-88 `origami_crane.pwmodel` documents a limitation that no longer applies
**Owner** `pwmodel-emitter-and-migration.md` · **Status** FIXED (comment rewritten in place)
`Examples/pwmodel/origami_crane.pwmodel:106-122` — the report's `:49-55` was itself wrong; the block
has always sat below the vertex-buffer notes, not in the opening summary.
- **Wrong** A block headed "NO `materials` BLOCK, DELIBERATELY" explains that `append_buffers`
  geometry cannot be named. `append_buffers` now takes `material=` (`PwModelParser.cpp:1051`,
  documented at `pwmodel-format.md:351`), so the file's headline lesson is false.
- **Symptom** Authors follow it and hand-manage `material_id` indices instead of using slots.
- **Fixed** The paragraph now names both spellings and which one binds: `material="<slot>"` is a
  slot NAME and is what `pwmodel 0` binds by, `material_id=` is a raw index that BYPASSES the slot
  table, and the two are mutually exclusive. Checked against `model.describe_ops` on a live editor
  rather than against the parser source. No `materials { }` block was added and that is deliberate:
  the crane is one material, so a block would carry a single binding and buy nothing; the rewritten
  comment says what to add when a second material arrives, and why an unreferenced block is dropped.
- **Contract** One example file.

### D-89 `crystal_cluster.pwmodel` contradicts the current implementation
**Owner** `pwmodel-emitter-and-migration.md` · **Status** CONFIRMED — **not** fixed by `1ac056d9`
`Examples/pwmodel/crystal_cluster.pwmodel:149-152`
- **Wrong** The comment calls `max_hulls` "a CEILING THAT IS NOT HONOURED". The current code explains
  the identical observation correctly as a **per-connected-component budget**, with engine-source
  citations and measured ratios (`PwModelCollision.cpp:416-437`), and warns about it by name.
- **Symptom** The corpus presents understood, documented, warned-about behaviour as an unexplained
  engine defect, teaching authors to distrust a parameter instead of reading it correctly.
- **Fixed** Rewrite as "per connected component, not per asset"; folds into D-86.
- **Contract** One example file.

### D-8A A test states a false fact about the tokenizer
**Owner** `pwmodel-emitter-and-migration.md` · **Status** CONFIRMED
`Source/PinWrightGeometry/Private/Tests/Model/TestModelHandlers.cpp:137-139`
- **Wrong** The comment justifies `FString::SanitizeFloat` over `%g` because `%g` "would spell a
  large one as `1e+06`, which the tokenizer does not read as a number". The tokenizer reads it fine
  (`PwModelTokenizer.cpp:159-191` accepts `e`/`E`, sign and digits; `pwmodel-format.md:118` documents
  exponents in the Number grammar). The choice is right for a different reason — `%g` at default
  precision truncates a large bound.
- **Symptom** Misleads the next author into believing exponent literals are unsupported.
- **Fixed** State the real reason.
- **Contract** None.

### D-8B A stale `file:line` citation in a comment
**Owner** `pwmodel-skeleton-skin-use.md` · **Status** CONFIRMED
`Source/PinWrightGeometry/Private/Handlers/Model/ModelCompileHandler.cpp:73`
- **Wrong** Cites `GeometryAssetCreate.cpp:169` for the provenance-stamp comparison; `:169` is a
  closing brace. The comparison is at `:147-148`.
- **Fixed** Correct the citation. The host project already runs a parse-only citation checker for
  `docs/scripts/`; the same idea would catch this class here.
- **Contract** None.

### D-8C Vacuous `ValidParamsNoCrash` tests present as coverage
**Owner** `pwanim-animation-format.md` · **Status** CONFIRMED
`Source/PinWright/Private/Tests/Sequencer/TestSequencerHandlers.cpp:920-931`
- **Wrong** `PinWright.sequencer.add_section.ValidParamsNoCrash` asserts only that the handler was
  *found*. It sends no `path`, so the handler returns `INVALID_SEQUENCE` and the test still passes.
  This is the test that let the ~1000x tick bug ship. The real regression test now exists at
  `TestAddTrackIdentifier.cpp:48-58`.
- **Symptom** A green test that cannot fail, occupying the slot where real coverage would go.
- **Fixed** Delete this one, then sweep the pattern — `docs/test-organization.md:157` documents it as
  a convention, and every instance omitting required parameters has the same vacuous shape.
- **Contract** None.

### D-8D Three test fixtures write a key count that disagrees with their own frame count
**Owner** `pwanim-animation-format.md` · **Status** CONFIRMED (test debt — **not** the product bug it
was reported as; see §10)
`Tests/Gameplay/TestPoseSearchHandlers.cpp:102`, `Tests/Assets/TestAnimSequenceDumpBuilder.cpp:273`
and `:276`
- **Wrong** Each calls `SetBoneTrackKeys` with 1 key on a model whose `NumberOfKeys` is 31. The
  fixtures then assert on `keyCount` derived from the *model*, not the array they passed, so they can
  pass for the wrong reason.
- **Fixed** Size the key arrays to `NumberOfFrames + 1`, or set `NumberOfFrames(0)` where one key is
  genuinely intended. The product-side validation lands with D-02's new verb.
- **Contract** None.

### D-8E `TestEditorHandlers.cpp` escaped the required-param sweep and carries a comment with no referents
**Owner** — · **Status** FIXED (all 10 deleted; both comments rewritten)
`Source/PinWright/Private/Tests/EditorOps/TestEditorHandlers.cpp` — line numbers below are
AS REPORTED (`:47-51` for the comment; `:58, :326, :356, :384, :413, :446, :1021, :1156, :1431,
:1731` for the ten tests) and no longer resolve: the fix deleted 137 lines and rewrote two comment
blocks, so the file is 1719 lines where it was 1835.
- **Wrong (the tests)** `c86f8890` deleted 533 per-verb required-param tests — "530 of the removals
  are `MissingRequired*` ids and nothing else" — and replaced them with
  `infra.dispatcher.AutoValidate.RejectsMissingRequiredParam`. It never touched this file. Ten
  `MissingRequiredParam` registrations survive here in the exact vacuous shape the sweep existed to
  delete: `TestTrue(handler found)` + `TestFalse(Capture.bSuccess)`, no error-code assertion. They
  cannot fail for the reason they are named — `InvokeHandlerWithCapture` (`Tests/TestUtils.h:200-215`)
  calls `Reg.Func(Ctx)` straight out of `GetPendingRegistrations()` and never reaches
  `FRpcDispatcher::ValidateHandlerParams` (`RpcDispatcher.cpp:91`), so the required-param gate is not
  in the code path at all. Deleting every `RPC_PARAM_REQ` from these ten verbs leaves all ten green.
- **Count correction** The reported figure of 11 counts the file's own strategy comment at `:10`
  ("REQ-param handlers: one MissingRequiredParam test"), which is prose. There are **10** test
  registrations. Repo-wide, 16 `*.MissingRequiredParam` ids remain: these 10, the registry-walk
  mechanism test (`infra.dispatcher.AutoValidate.RejectsMissingRequiredParam`), and 5 deliberate
  survivors that were sharpened rather than deleted — `actor.select`, `landscape.sculpt`,
  `material.authoring.add_custom_expression`, `audio.synth.recipe`, `Model.Parser`. Each of the five
  asserts a specific error or diagnostic code, or names the in-body guard it pins; two carry a
  comment saying explicitly that the dispatcher gate does **not** fire for that verb
  (`TestActorHandlers.cpp:1433-1439`, `TestEnvironmentHandlers.cpp:1757-1764`). This file is the only
  un-swept one.
- **Wrong (the comment)** The `+6/-0` uncommitted diff opens "These three, and the dispatcher-gate
  namespace below, must sit OUTSIDE the UnrealEditorSubsystem `__has_include` conditional above."
  Neither referent exists. The file declares **no namespace** — `:92`, `:141` and `:786` are
  `using namespace` of `PieWorldSelector` / `PieNetworkEmulation`, both defined in headers — and
  nothing identifies "these three"; the lines immediately above the comment are the `#if`/`#elif`
  `__has_include` block itself (`:39-45`).
- **Symptom** Ten green tests occupying the slots where real coverage would go, plus a comment that
  sends the next reader looking for a namespace that is not there and cannot tell them whether the
  hazard it describes is still present.
- **Fixed** All ten deleted rather than sharpened. None had the survivors' character: every one of
  the ten verbs declares its parameter `RPC_PARAM_REQ`, so the requirement IS dispatcher-gated and
  `RequiredParamGate.EveryVerb` already drives the real dispatcher over it. Sharpening would have
  duplicated the walk per verb, which is the trade `c86f8890` made in the other direction 533 times.
  The nearest call was `editor.focus_actor`, which carries a redundant in-body `ActorName.IsEmpty()`
  guard returning INVALID_ARGUMENT — but that guard fires on an EMPTY STRING, not on a missing key
  (`ValidateHandlerParams` tests presence, not emptiness), so a test named `MissingRequiredParam`
  could not pin it. See the note under **Follow-up** below.
- **Fixed (the comment)** Rewritten to state the constraint the `__has_include` block really carries
  — nothing may be DECLARED in either arm, because only one arm is taken — and to record that the
  two regions guarded by `MCP_TEST_HAS_UNREALEDITOR_SUBSYSTEM` are both statement-level, inside
  `FEditorSetCameraForceRedrawTest`. The file header was rewritten in the same pass: the stale
  "Total: 31 handlers, 62 tests" became measured figures (36 verbs, 52 tests) plus the rule that
  keeps the family from being re-added, and the handler-file list gained `EditorWindowHandlers.cpp`,
  which it had always omitted despite covering `editor.set_window_state`.
- **Follow-up (new, not part of this defect)** `editor.focus_actor` accepts `actorName: ""` through
  the dispatcher gate and is refused only by the in-body check at `ViewportHandler.cpp:145-147`.
  Nothing covers that path: the registry walk supplies placeholders rather than empty strings. An
  `EmptyStringParam` test would be honest coverage; it needs a build window, so it is not in this
  pass.
- **Contract** None.

### D-8F Four shipped examples document a `pipe` that no longer exists
**Owner** `pwmodel-emitter-and-migration.md` · **Status** CONFIRMED — **found while disproving the
`pipe_junction` claim in `commit-grouping.md` §17**
`Examples/pwmodel/oil_lamp.pwmodel:105-113` · `pipe_junction.pwmodel:24-28, :50` ·
`spur_gear.pwmodel:45-46, :55` · `watchtower.pwmodel:102-103, :114-116`
- **Wrong** All four assert that `pipe` is an open shell with no end caps and is placed by its base
  rather than its centre, and three of them tell the author to build an annulus some other way
  because of it ("DO NOT REWRITE THESE LIMBS AS `pipe`", "THE WEB POCKET AND THE BUSHING ARE NOT
  `pipe`, AND THAT IS DELIBERATE", "A solid drum with its middle bored out, NOT `pipe`"). `pipe` was
  rebuilt as a revolved annulus in `cbc40271` — `GeometryOps_Primitives.cpp:408`, profile spanning
  `-height/2 … +height/2`, `bProfileCurveIsClosed` making closure a property of the construction —
  so it is now a closed manifold centred on its origin like every other primitive. The measured
  figures the comments quote (`pipe radial_steps=32` → open, 4 triangles per radial step) no longer
  reproduce.
- **Already corrected elsewhere** `docs/pwmodel-format.md:213`, `:220` and `:225-233` document the
  rebuilt behaviour correctly, including the base→centre move. Only the example corpus is stale, and
  the corpus is the format's reference documentation (same reasoning as D-86..D-89).
- **Symptom** An author reading the examples routes around a working primitive and hand-builds
  annuli out of `cylinder` + `subtract` — which is exactly what all four files do, and what
  `model.md:71` records as having compiled green while producing a pipe with no hole through it.
- **Fixed** Rewrite the four blocks to describe the swept annulus, and decide per file whether the
  hand-built annulus is still the right construction now that `pipe` closes. Folds into the D-86..D-89
  example pass.
- **Contract** Comments only in four example files; no grammar or geometry change.
- ⚠️ Four agents own `Examples/pwmodel/` right now. Coordinate before editing.

---

## 9. Repository and documentation hygiene

### D-90 `.pwmodel` has no `.gitattributes` entry and 9 of 12 examples are untracked
**Owner** `pwmodel-emitter-and-migration.md` · **Status** FIXED (both halves; re-verified)
- **Wrong** Only `gothic_window`, `spiral_stair` and `watchtower` are tracked — added by `1ac056d9`.
  The other nine are `??`. Separately, `.pwmodel` has no line-ending pin, so under `* text=auto` the
  files normalise to LF in the index and check out CRLF on Windows; once `model.format` can write
  files, an emitter assuming either style produces whole-file diffs on the other.
- **Symptom** The repo has no remote and `git log --diff-filter=D` is empty for the whole history,
  yet producers have already been lost this way — untracked work has no floor under it.
- **Fixed** Both halves. All **13** `Examples/pwmodel/*.pwmodel` are tracked now (`git ls-files`;
  the corpus grew by `mobius_band` since this row was written), and `.gitattributes` carries
  `*.pwmodel text` as its last per-path pin. `git check-attr text eol` reports `text: set`,
  `eol: unspecified`, and no working-tree path changed status when the pin landed.
- **Note on `eol`** Deliberately NOT pinned, unlike `mcp-instructions.md` and `ci/*.patch` above it.
  Those are byte-exact contracts; `.pwmodel` is not, and the emitter's contract
  (`pwmodel-emitter-and-migration.md:183-184`) is to REPRODUCE the newline style of the file it
  rewrote. Pinning `eol=lf` would force a renormalising checkout on Windows, which this repo has
  never run. Explicit `text` still buys the thing that mattered: it removes the content heuristic in
  `* text=auto` from the decision, so the index shape is fixed before an emitter writes whole files.
- **Contract** None.

### D-91 Verified fixes exist only in an uncommitted working tree
**Owner** — · **Status** FIXED (re-verified: every named path is tracked and committed)
- **Wrong** The `sequencer.add_section` tick fix, the `RedirectorFixupPolicy` crash fix and its test
  (both `??`), the `simplify_collision` algorithm change (D-65), and the then-current maintainer-doc
  subdirectory itself are all uncommitted, in a tree with hundreds of modified paths.
- **Symptom** Correct, verified work is one bad checkout from gone, with no remote to refetch from.
- **Fixed** All of it landed. `RedirectorFixupPolicy.{h,cpp}` and `TestRedirectorFixupPolicy.cpp`
  are tracked (`Source/PinWright/Private/Utils/` and `.../Tests/Assets/`), every document named by
  this finding is tracked, and none of those source or document paths is `??` any more. The working tree
  still carries `M` paths from work in flight, which is the shape this row wanted — an untracked
  file has no floor, a modified tracked one does.
- **Contract** None.

### D-92 No plan file is indexed, and the coverage test fails on every one
**Owner** — · **Status** FIXED (re-verified: all 7 plan files are in both indexes)
- **Wrong** `PinWright.core.docs_schema.EveryMaintainerDocIsIndexed`
  (`Source/PinWright/Private/Tests/Core/TestDocsIndexCoverage.cpp:105-142`) requires every `.md`
  under `docs/` outside `wiki-src/` and `logs/` to be linked from **both** `docs/index.md` and
  `docs/tags.md`, matching each document's relative link. No matching links existed in either
  index. Five documents produced ten errors that day; **this file made twelve.**
- **Not fixed here** deliberately: editing `index.md` and `tags.md` was outside this document's
  brief. It is the first thing to do after this lands.
- **Fixed** Done. The two catalogues gained a row or tag entry for every document, and the coverage
  test passed. The former category was removed on 2026-08-26: retained documents now live directly
  under `docs/`, and both catalogues link those direct paths.
- **Contract** None.

### D-93 Three non-exempt docs lack YAML frontmatter
**Owner** — · **Status** CONFIRMED, still open (exactly 3; re-verified 2026-08-20 — all three
still open with `# `)
`docs/engine-research-2026-08-plugin-pass.md:1`, `docs/error-code-catalog.md:1`,
`docs/release-checklist.md:1`
- **Wrong** All three start directly with `# `. The rule is `docs/SCHEMA.md:30-56`; the exemptions
  (`SCHEMA.md`, `lessons.md`, `adr/`, `wiki-src/`, `logs/`) do not cover them. Nothing enforces it —
  `TestDocsIndexCoverage.cpp:16-21` explicitly declines to check frontmatter.
- **Fixed** Prepend `type`/`summary`/`date`/`tags`. Optionally extend the coverage test to check
  presence, which turns a convention into a contract.
- **Contract** None.

### D-94 `docs/tags.md` has lost alphabetical order
**Owner** — · **Status** CONFIRMED, with a correction
- **Correction** This is **not** a new regression. The committed file already carried `rigvm`,
  `roundtrip`, `fproperty-lifecycle`, `json-builders`, `mgir`, `pwmodel`, `adr` and `pagination` out
  of place; this session's uncommitted edits added `backport`, `build`, `crash-guards`, `honesty`,
  `engine-research`, `layout-quality` and `test-workflow` as new breaks.
- **Note** No rule anywhere requires alphabetical rows — `TestDocsIndexCoverage.cpp:21` names the
  ordering as deliberately unchecked. This is optional cleanup, listed so it is a decision rather
  than a drift.
- **Fixed** Resort both tables, or write the rule down and enforce it.
- **Contract** None.

### D-95 The error-code catalog's generator is an inline snippet, and nothing detects staleness
**Owner** — · **Status** REFINED (the reported stale rows do not exist — measured)
`docs/error-code-catalog.md:814-849`
- **Refinement** The report was that the catalog "may carry stale row bodies", verified only for
  containment of the seven new codes. It does not. Running the file's own snippet against the tree on
  2026-08-20 reproduces `## Codes by frequency` (`:94-812`) **byte-for-byte**, all 715 rows, and its
  trailing line prints `715 4088 259` — matching the three bullets at `:40-42` exactly. The generated
  half of this document is in sync. The defect is the absence of anything that would have told us so.
- **Wrong** There is a generator, but it is a fenced Python block inside the document it generates
  (`:821-847`), not a committed script. Nothing runs it, nothing diffs against it, and no test reads
  the catalog at all — `grep -rn "error-code-catalog" Source/` returns one hit, a comment in
  `Handlers/ErrorCodes.h:19`. The near-miss is `TestErrorCodeRegistry.cpp`, which the catalog's own
  header (`:11-16`) says it mirrors: that test asserts every emitted code is *registered*, never that
  the catalog *lists* it, so the two can diverge silently in either direction.
- **Compounding** `commit-grouping.md:52` makes commit 20 last specifically because the catalog "is
  regenerated" — an ordering constraint resting entirely on a human remembering to paste a snippet
  out of a markdown file and run it.
- **Also unenforceable** The two hand-maintained sections — `## Transport-emitted codes` (`:60-74`)
  and `## Hand-annotated handler codes` (`:76-92`) — are outside the scan by design (`:22-31`), so
  regeneration can never validate them and a stale body there is invisible to both the snippet and
  the test.
- **Symptom** The catalog agents read to recover from an error can drift arbitrarily far from the
  tree with no signal; today it happens to be correct, which is indistinguishable from the failure
  case without running the scan by hand.
- **Fixed** The precedent is already in the tree and is an exact match:
  `PinWright.core.pwmodel_diagnostics.DocumentedCodesMatchEmittedCodes`
  (`Tests/Core/TestPwModelDiagnosticCatalog.cpp:202`) reconciles the hand-maintained `PWMODEL_*`
  table in `pwmodel-format.md` against source in every direction that can rot, and its header states
  the same problem in the same words — "The only reconciliation was a PowerShell snippet a human had
  to remember to run; this test is what makes it non-optional" (`:6-8`). Do that here: move the
  snippet to a committed script under `Content/Python/`, and assert the regenerated table equals the
  committed one. That converts "regenerate before commit 20" from a note into a gate, and covers the
  two hand-maintained sections by the same directional checks.
- **Contract** None.

### D-96 Two dead `#else` branches diverged, and only one of them says it is dead
**Owner** — · **Status** CONFIRMED
`Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryOps_Boolean.cpp:374-387` (`Mirror`) vs
`Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryOps_Modeling.cpp:1649-1666` (`Stretch`)
- **Wrong** Both guards now read `#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)` (`:370` and `:1646`),
  so on every version this plugin builds — 5.8 only — the engine's `ScaleMesh` runs and both `#else`
  bodies are unreachable. `Stretch`'s fallback says so at `:1650-1651` — "Positions only … this
  branch does not reach 5.8 and is not the shipped path" — and is left with a *conditional*
  `ReverseOrientation` on negative determinant (`:1662-1665`) and no normal/tangent inverse-scale,
  i.e. still carrying half of the defect it was written up for. `Mirror`'s fallback was instead
  *upgraded*: `9c324252` adds an **unconditional**
  `EditMesh.ReverseOrientation(/*bFlipNormals=*/ true)` at `:386`, with no note that the branch it
  repairs cannot execute. So the two dead branches now differ in two ways — annotated vs not, and
  conditional vs unconditional reversal — and nothing in either file explains why.
- **The surrounding comment reads the wrong way** `GeometryOps_Boolean.cpp:362-364` says "Both
  branches below therefore have to reverse the winding … the branch that did not is what made every
  mirrored half a black, back-faced shell." That was true while the bounded `>= 5.4 && < 5.5` guard
  made the `#else` live on 5.8; after the repair it describes a branch no build takes, in the present
  tense.
- **The doc is now stale on the same point** `docs/engine-version-support.md:217-252` documents the
  guard repair well — the `Was`/`Now` table at `:241-243`, the discriminator table at `:230-233` — but
  never records that the two fallbacks survive as dead code, and its sentence at `:249` ("The
  hand-rolled fallbacks did neither") no longer holds for `Mirror`, which now reverses orientation.
  That page is the backport's checklist, so the divergence is load-bearing: the 5.3 backport is
  exactly the event that makes both branches live again, and it will find one that reverses winding
  unconditionally and one that does it conditionally without touching normals.
- **Symptom** Invisible at runtime on 5.8. It reads as a defect on review, and it becomes one the day
  the backport lands.
- **Fixed** One sentence in `Mirror`'s `#else` matching `Stretch`'s annotation, and one row or
  paragraph under `## Dead version guards` recording that both fallbacks are retained-but-dead and how
  they differ. Correct `:249` while there.
- **Contract** None — comments and one doc section.
- ⚠️ Do not edit the C++ while agents are driving an editor built from this source; the doc half can
  land independently.

### D-97 `mcp_proxy.py`'s calibration comments cite host-project logs that will never be committed
**Owner** — · **Status** FIXED (all of it, including the sites beyond the three reported blocks)
**Retired historical context only:** This entry records the former proxy-owned test runner and its measurements; it is not a current execution contract.
`Content/Python/mcp_proxy.py` — line numbers below are AS REPORTED (`:333-338`, `:304`,
`:926-934`, and `tests/test_mcp_proxy_editor_start.py:2496`) and no longer resolve; the rewrite
lengthened every block. Find them by constant name: `EDITOR_TEST_TIMEOUT_MINUTES`,
`EDITOR_TEST_STARTUP_STALL_MINUTES`, and the terminal-marker rule in `assess_run_completion`.
- **Wrong** Three comment blocks justify their constants with measurements attributed to files by
  name. `EDITOR_TEST_STARTUP_STALL_MINUTES` (`:333-338`) cites `Saved/Logs/pw_suite_verify.log`,
  `pw_suite_final3.log`, `pw_suite_final2.log` and `pw_suite_final.log` — note the fourth is
  `pw_suite_final.log`, not the reported `pw_suite_final1.log` — plus two
  `Saved/PinWright/test-runs/<hash>/automation.log` paths. `EDITOR_TEST_TIMEOUT_MINUTES` (`:304`)
  cites `pw_suite_attest.log`. The terminal-marker rule (`:926-934`) cites `pw_suite_ortho`,
  `pw_suite_attest`, `pw_suite_extendguard`, `pw_suite_clb`, `pw_suite_full2` and two more
  `test-runs/<hash>` logs. `Content/Python/tests/test_mcp_proxy_editor_start.py:2496` cites
  `pw_suite_final.log` the same way.
- **Why they are unreachable** Every path is under the **host project's** `Saved/`, which is outside
  this repo entirely and gitignored in the host (`.gitignore:5`, `Saved/`). All of them exist on this
  dev host today and none of them can exist for anyone who clones the plugin. The plugin's own
  `CLAUDE.md` already states the rule for the suite figures — "every citation here is a host-project
  or gitignored path, so a fresh clone has none of these logs" — and these comments do not carry it.
- **Symptom** The 12-minute watchdog and 90-minute ceiling belonged to the retired proxy-owned editor
  test runner. They are historical context, not a current timeout or supervision contract.
- **Fixed** Every number and derivation kept; every filename gone. Ten comment blocks across the two
  files now attribute to "measured on this dev host, <date>" and say in-line that the source logs are
  gitignored host-project state no clone has — which is the fact a reader needs, and the one a
  filename was standing in for. Distinguishing runs are named by their distinguishing property
  instead: the 395.1 s slowest-healthy boot, the 18m24s wedge that then ran 4013 tests, the 46m07s
  wedge, the killed 610-of-3780 and 762-of-3778 suites, the 3761/3761/0 and 149/149/0 historical semicolon-Quit runs.
  Beyond the three reported blocks this also swept `mcp_proxy.py:2482` and four more sites in
  `tests/test_mcp_proxy_editor_start.py`. `grep -n "pw_suite\|Saved/Logs\|test-runs/<hash>"` is
  clean in both files; both still byte-compile and are unchanged in behaviour.
- **Contract** None — comments only, no behaviour change.
- **Current contract** The old proxy-owned runner, 12-minute watchdog, and 90-minute ceiling are
  retired historical context. `editor_prepare_tests` is the only command-returning test verb and
  instant planner. It accepts one mandatory, non-empty `filter`, runs the live PinWright guard,
  resolves the project's `EngineAssociation` to the matching `UnrealEditor-Cmd`, and returns
  a `COMMAND_READY` object with launch executable/`argv`, checker executable/`argv`,
  and explicit absolute `logPath`. A detected editor is `EDITOR_ALREADY_RUNNING`; an
  unavailable probe is `not_probed`, not proof that the editor is stopped. It returns immediately
  and does not launch, wait, own, supervise, or classify the test process.
  The caller runs the returned launch executable/`argv`, then runs `check_suite_log.py`
  with the returned checker executable/`argv` and the exact same log path. The launch uses
  `-ExecCmds="Automation RunTests <filter>,Quit"`, `-TestExit="Automation Test Queue Empty"`,
  explicit `-Abslog=<same absolute logPath>`, `-unattended`,
  `-RunningUnattendedScript`, `-nopause`, `-nocefaccelpaint`, and `-log`,
  plus a real RHI; never add `-NullRHI`.

---

## 10. Rejected claims

Ten reported defects did not survive verification. Recording them matters as much as the defects:
each was circulating as fact, and three would have produced a fix for a bug that does not exist.

| Claim | Verdict | Evidence |
|---|---|---|
| `scale_start`/`scale_end` do nothing on `extrude_along_spline` because the generator ignores `PathScales` when `bLoop=true` | **False** | The engine applies `PathScales` **regardless of `bLoop`** — `SweepGenerator.cpp:689-690`, unconditional. Only the scalar `StartScale`/`EndScale` are `bLoop`-gated (`:616`), and PinWright pins those to `1.0f` anyway, so they are not the channel. The claim traces to a **stale engine header comment** at `SweepGenerator.h:150` ("ignored if bLoop=true") that the 5.8 implementation contradicts. Disproving it surfaced the real defect on the same line — `cap`, now D-04. |
| A `twist=` parameter gap | **Does not exist** | Ships and is fully wired on three surfaces: `AdvancedMeshOpsHandler.cpp:225/236/286` and `:419/430`, consumed at `GeometryOps_Advanced.cpp:575-577` and `:479-481`; `.pwmodel` at `PwModelParser.cpp:988`/`:1002` → `PwModelCompiler.cpp:1657`/`:1690`. Covered by tests. Likely confusion with `angle` on the separate `geometry.twist` deformer. |
| `append_buffers` drops triangles silently under a null debug sink | **Already fixed** | `GeometryOps_Advanced.cpp:856` passes `Debug.Get()`; warnings drained at `:861`, and on error the mesh is **rolled back** (`:868-875`) with `ERR_MESH_APPEND_FAILED` returned. |
| OBJ/STL import drops triangles silently under a null debug sink | **Already fixed** | `MeshIOHandler.cpp:520` passes `Debug.Get()`, refusal checked at `:537`, and the actor spawn is deliberately sequenced *after* the check (`:552`). A separate residual loss one layer up remains and is filed as D-53. |
| `AutoUVPatchBuilder` still passes `nullptr` | **Already fixed** | `GeometryOps_Modeling.cpp:2323-2332` constructs a sink, drains warnings and fails with `ERR_UV_GENERATION_FAILED`. Its sibling `LayoutUV` is genuinely still null — D-51. |
| ~8 `nullptr` debug-sink sites remain of ~90 | **Inverted** | The total is right; the split is backwards. **79 remain, 11 are converted.** The defect is roughly ten times larger than reported — D-50. |
| `sequencer.add_section` casts display frames straight to ticks | **Already fixed** | `SequenceHandler.cpp:2455-2456` goes through `SequenceHelpers::DisplayFrameToTick`, with the rationale comment at `:2450-2454` naming the 0.0042 s vs 4.17 s case and regression coverage at `TestAddTrackIdentifier.cpp:53`. |
| `SkippedAlreadyClean` counts as persisted, so a never-written package reports `saved: true` | **False for the canonical path** | `SaveAssetToDiskReportingPresence` already re-gates it on file presence (`Utils/AssetUtils.cpp:718-726`): if the file does not exist the state becomes `Failed`. `IsAssetPersistedToDisk` makes the same point in its own comment. The hazard is live only on the 17 handlers that call `WasSavePersisted` directly — folded into D-12, not an independent defect. |
| 47 unreferenced `ERR_*` constants, including five `PCG_GENERATION_*` and three `LIVE_CODING_*` that look half-wired | **False on every number** | The registry holds **4** `PCG_GENERATION_*` and **6** `LIVE_CODING_*`, and **all ten are live on the wire** — emitted as raw literals rather than through their constants (e.g. `PCGGenerateHandler.cpp:465`, `LiveCodingHandler.cpp:184`). "Half-wired" is backwards: the emits are wired, the constants are bypassed. The genuinely dead set is **32**, not 47 — D-64. |
| `SetBoneTrackKeys` accepts a mismatched key count — *as a product bug* | **Test-fixture bug only** | **Zero product call sites.** All five calls are fixtures; three mismatch, two are correct. Real, but it is test debt — retained as D-8D, not as shipped behaviour. |
| `cut_material=` is missing | **Not a defect** | Zero hits in `Source/` and `Examples/`. It is a design proposal in `pwmodel-value-system.md` §7, never a shipped parameter. |

Two further corrections that changed an item rather than retiring it: `material=` inside a boolean
block is **already an error** on the boolean op itself (only the nested-generator case is live —
D-03); and `use animation from` **is not silent** — the compiler hard-errors on every `use` (D-69).
A third: commit `1ac056d9` did **not** fix the three stale examples; it only *added* three new files
(637 insertions, all `new file mode`) and touched nothing else under `Examples/`.

Finally, the reported `cylindrify` repro — "a plain 100³ cube" — does not reproduce. The defect is
real but needs a **subdivided** box; an unsubdivided one is a no-op. See D-01.

---

## 11. Open questions

1. **D-63 `ERR_UnsupportedNodeClass`** — rename the wire value (breaking, and it has a catalog row),
   or rename only the C++ constant and leave the wire string alone (free)? The second restores
   registry consistency at zero contract cost and is the recommendation.
2. **D-42 `startFrame` units** — rename per verb (compatible) or unify the unit (breaking)? Unifying
   is the better contract and the worse migration.
3. **D-30 `save`** — confirm that "write to disk" is the surviving meaning. Everything in §2 and the
   bone-keyframe path assumes it.
4. **D-06 `sweep` `steps`** — split into two parameters (breaking) or document both meanings and echo
   the derived side count (neutral)?
5. **D-94 `tags.md` ordering** — write the sorting rule down and enforce it, or drop the expectation?
   It is currently neither.
