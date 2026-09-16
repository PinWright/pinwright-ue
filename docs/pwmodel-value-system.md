---
type: system
summary: "Value-system and grammar work for pwmodel: nested lists for a real loft, map-as-pair-list, named weight-map handles, cut_material, and the format-neutral parser core."
date: 2026-08-20
tags: [pwmodel, pwanim, format, grammar, value-system, parser]
---

# pwmodel value system and grammar

Section of the unified format plan. Decisions in
[format-decisions.md](format-decisions.md) bind this page.

## Verdicts

| Item | Verdict | Ground |
|---|---|---|
| Nested list-of-lists | **Build.** New value kind `NestedList`, one param constraint `profile_list`, one op `loft` | Only unreachable modelling capability; no engine call exists either, so the op is hand-rolled |
| Maps (UDIM resolutions) | **Build, no new value kind.** A map literal is a list of pairs; new param constraint `int_map` over the existing `TupleList` | `TMap<int32,int32>` needs a *literal*, not a container. `[(1001, 2048), …]` already parses today |
| Named-resource handles (weight maps) | **Build.** New op `weight_map`, name→handle resolution, 6 new params on `simplify_mesh` | `FindOrAddMeshWeightMap` / `GetWeightMapHandle` exist; spatial selectors author values without vertex identity |
| Object-valued `key = { … }` | **Closed on technical ground** | The shape it uniquely serves — an array of *heterogeneous* structs — does not occur anywhere in the reachable engine surface. Falsifier stated below |
| Named `curve` block | **Closed by design decision** (user) | Recorded in format-decisions.md. Cheaper fix for the real problem (line length) specified below |
| Twist on swept paths | **Premise is false.** `twist=` ships on both ops. Three *different* gaps found instead | Verified in table, compiler, ops layer and engine |
| `cut_material=` | **Build.** No face identity required | The exposed faces literally *are* the tool mesh's triangles, and `AppendMesh` copies their MaterialID |

---

## 1. Nested lists, and the `loft` they unlock

### What is true today

`FPwModelValue` (`PwModelAst.h:28-40`) has `double Number` / `TArray<double> Tuple` /
`TArray<TArray<double>> TupleList` / `FString Text`. `ParseValue`
(`PwModelParser.cpp:1540-1613`) reads `[` then requires `(` at every element
(`:1584-1588`, `"'(' - a list holds tuples"`). Two levels, no more.

### The engine has no loft either

Verified: `FGeneralizedCylinderGenerator` (`SweepGenerator.h:141-152`) carries **one**
`FPolygon2d CrossSection`; `FProfileSweepGenerator` (`SweepGenerator.h:254-262`) carries **one**
`TArray<FVector3d> ProfileCurve`. Nothing in `GeometryCore/Public/Generators/` or
`GeometryScriptingCore/Public/GeometryScript/` blends between N authored profiles. A grep for
`loft|blend.*cross.?section|morph.*profile` across both public trees returns nothing.

So the value kind is necessary but not sufficient: **`loft` is hand-rolled mesh construction**,
the same class of work as `bridge` (`GeometryOps_Advanced.cpp:128-230`, which builds a strip over
`FDynamicMesh3` directly). Do not plan this as "add a type, call an engine function."

The cheap honest implementation: for N profiles of **equal vertex count** placed at N frames,
build ring vertices in the op and stitch consecutive rings — then hand the buffers to the existing
`GeometryOps::AppendBuffers` path, so no new low-level mesh code is written at all. Unequal vertex
counts need resampling and are **rejected**, not silently resampled.

### The value kind

`EPwModelValueType::NestedList` on `PwModelAst.h:18-26`.

Storage: **add one member, change nothing existing.**

```
TArray<FPwModelValue> Items;   // Type == NestedList; each Item is a TupleList
```

Recursive rather than a third parallel `TArray<TArray<TArray<double>>>`, for three reasons that
are not style:

- Every existing reader in `PwModelValueRead.h` keeps working untouched. A flat third member would
  work too, but stops at depth three.
- The emitter (which ships first and defines the core's shape) needs a recursive spine to print
  any depth from one function. A parallel member forces one print arm per depth.
- `.pwanim` will want a list of keyframe tracks, which is the same shape.

`Tuple` and `TupleList` are **not** re-expressed on top of `Items`. That refactor touches all 671
occurrences of the value/spec symbols for no behaviour change, and `GetPointList3` etc. would each
grow an unwrap. Leave them.

`PwModelValueTypeToString` (`PwModelParser.cpp:1138-1150`) gains
`case NestedList: return TEXT("a list of lists");`.

### The parser change

In `ParseValue`, the `OpenBracket` arm (`:1569-1607`) currently commits to `TupleList` before
looking. Change it to peek one token after `[`:

- next token is `(` → today's path, `Type = TupleList`, unchanged byte for byte.
- next token is `[` → `Type = NestedList`, loop parsing bracketed `TupleList` values into `Items`.
- next token is `]` → empty list. **Keep it `TupleList`**, so `x=[]` does not change meaning for
  any existing parameter.

The "may not span lines" rule (`:1579-1583`) applies unchanged at both levels. Do not relax it
here — see §5 for where relaxing it belongs.

The tokenizer needs **zero** changes. `[` and `]` already lex (`PwModelTokenizer.cpp:32-45`).

### The param constraint

`EPwModelParamType::ProfileList` on `PwModelParser.h:33-61`, spelled `profile_list` by
`PwModelParamTypeToString` (`PwModelParser.cpp:1108-1127`).

Named for what an entry **means**, exactly as `frame_list` was and for the same recorded reason
(`PwModelParser.h:48-59`): `PwModelParamTypeToString` is the one spelling an author sees in both
the diagnostic and `model.describe_ops`, and a parameterised `list_of_point_list2` has no name to
print. An entry is a *profile* — a closed cross-section, in the same `(u, v)` form
`sweep profile=` and `revolve profile=` already take.

Validation, added to `ValidateValue` (`PwModelParser.cpp:1728-1873`) as its own arm — it cannot
share the `PointList*`/`FrameList` arm at `:1845-1868`, which indexes one level:

| Wrong shape | Code | Message |
|---|---|---|
| not a nested list | `PWSRC_BAD_VALUE` via `BadValue` | `Parameter 'profiles' on 'loft' expects profile_list, but found a list of tuples.` |
| an inner point is not a 2-tuple | `PWSRC_BAD_TUPLE_ARITY` | `Point 3 of profile 1 of 'profiles' on 'loft' expects 2 components (u, v), but found 3.` |
| fewer than 2 profiles | `PWSRC_BAD_VALUE` | `Parameter 'profiles' on 'loft' needs at least 2 profiles to blend between, but carries 1.` |
| a profile with fewer than 3 points | `PWSRC_BAD_VALUE` | `Profile 1 of 'profiles' on 'loft' has 2 points; a cross-section needs at least 3 to bound a face.` |
| profiles of differing length | `PWSRC_BAD_VALUE` | `Profile 1 of 'profiles' on 'loft' has 6 points but profile 0 has 8. Every profile must carry the same number of points: the loft stitches them point-for-point and does not resample.` |

The two-level index in the arity message is the load-bearing part. `Entry 4 of 'profiles'` with a
flat index is unactionable once there are three profiles of eight points.

**No new diagnostic code.** `PWSRC_BAD_VALUE` already carries range violations, bad enum values
and non-whole integers (`:1723`, `:1754`, `:1768`, `:1785`); length rules fit it. A new code costs
a `PwModelDiagnostic.h` row, a `pwmodel-format.md` table row, and a pass through
`PinWright.core.pwmodel_diagnostics.DocumentedCodesMatchEmittedCodes`
(`Source/PinWright/Private/Tests/Core/TestPwModelDiagnosticCatalog.cpp`), for no reader benefit.

### The `loft` op

```
loft profiles=[[(-20,-20),(20,-20),(20,20),(-20,20)], [(-8,-8),(8,-8),(8,8),(-8,8)]] \
     path=[(0,0,0,0,0,0), (0,0,120,0,0,0)]
```

- `MakeModifier`, **not** a generator, and it **appends** — the same rule `sweep` and
  `extrude_along_spline` carry (`PwModelParser.cpp:976-979`). Consistency matters more than
  convenience here; a third path-driven op that opened a part would make the rule un-teachable.
- `profiles` — required, `profile_list`.
- `path` — required, `frame_list`. Reuses the existing type and its tested reader
  (`PwModelValueRead.h:194-214`), including `MakeRotator`, so a loft frame and a `rotate=` cannot
  disagree.
- `cap` — bool, default true.
- **Cross-parameter count check goes in the compiler**, not the parser, as an `FOpResult` beside
  `ValidatePathFrames` (`PwModelCompiler.cpp:299-313`). Two reasons: it matches the existing
  precedent for cross-value rules (`ReadSweepProfile`, `ValidatePathFrames`), and it keeps the
  `describe_ops` round-trip test out of the business of making two independently-generated
  literals agree in length (see Blast radius).
- No `material=` / `color=`. `sweep` and `extrude_along_spline` tag nothing
  (`pwmodel-format.md:287-290`); a loft that did would be the odd one out.

`profiles` must also reject a repeated point and a degenerate ring — but that is geometry, so it
belongs to the compiler's `FOpResult`, not the parser (`PwModelParser.h:6-9`).

### `describe_ops`

`type: "profile_list"` and nothing else. `ModelHandler_ParamSpecToJson`
(`ModelCompileHandler.cpp:308-330`) serialises `PwModelParamTypeToString` verbatim, so the new
type reaches the wire with **zero handler changes**. Do not add a `valueKinds` block: it would be
a second published description of the same thing, and the format doc's whole delegation model
(`pwmodel-format.md:29`) depends on there being one.

---

## 2. Maps: a map literal is a list of pairs

### The claim to retract

`GeometryOps_Modeling.h:940-943` says *"UDIMResolutions is NOT carried: it is a
`TMap<int32, int32>`, and `FPwModelValue` has no map member, so a per-tile resolution table has no
literal in the format."* `pwmodel-format.md:773-777` and the `enable_udim_layout` description
(`PwModelParser.cpp:817-819`) repeat it.

**It is wrong, and it conflates the C++ container with the literal.** A `TMap<int32,int32>` is a
set of integer pairs. `[(1001, 2048), (1002, 1024)]` is a legal `TupleList` **today** — it parses
now, on the shipped tokenizer and parser, with no grammar change whatsoever.

### The constraint

`EPwModelParamType::IntMap`, spelled `int_map`.

- Value kind: existing `TupleList`. **No AST change.**
- Arity: 2. Add to `ExpectedTupleArity` (`PwModelParser.cpp:1671-1689`).
- Shape hint: extend `PwModelParamTypeTupleShape` (`:1133-1136`) to return `" (key, value)"` for
  `IntMap`. That function exists for exactly this — it was added so a 6-tuple's message could say
  what the six mean, and it leaves every other message byte-identical.
- Joins the `PointList*` / `FrameList` arm at `:1845-1868` for free; add two checks after it:
  both components whole (reusing the `Integer` wording at `:1754`), and keys unique
  (`PWSRC_BAD_VALUE`: `Parameter 'udim_resolutions' on 'uv' names tile 1001 twice.`).

Naming follows the `frame_list` rule under protest and then satisfies it: `int_pair_list` is the
shapeless spelling the `PwModelParser.h:48-59` comment argues against, and `udim_map` is
over-fitted to one call site. `int_map` plus the printed `(key, value)` hint tells an author both
the shape and the ordering, and it is the type `.pwanim` will reuse for any integer-keyed table.

### Where it lands

`uv mode=layout udim_resolutions=[(1001, 2048), (1002, 1024)]`.

`GeometryOps::FLayoutUVParams` (`GeometryOps_Modeling.h:944-970`) gains
`TMap<int32,int32> UDIMResolutions`, forwarded to `FGeometryScriptLayoutUVsOptions::UDIMResolutions`
(`MeshUVFunctions.h:79`) in `LayoutUV` (`GeometryOps_Modeling.cpp:2139-2167`, replacing the
"deliberately left default-empty" line at `:2167`). Also reachable on
`FGeometryScriptRecomputeUVsOptions`-adjacent packers (`MeshUVFunctions.h:229`) if a later op wants
it; do not wire that speculatively.

**Ships with a warning, not silently:** `udim_resolutions` with `enable_udim_layout=false` is a
parameter the engine reads and ignores. Warn naming the flag.

### What this does *not* unlock

A `TMap<FName, F…Struct>` still has no literal. None exists in the reachable surface (see §4).

---

## 3. Named-resource handles: weight maps

### What the engine actually offers

- `FGeometryScriptWeightMapHandle` (`GeometryScriptTypes.h:297-310`) is **one `int
  WeightMapAttributeLayerIndex`**. It is not a name and not a pointer.
- `FindOrAddMeshWeightMap(TargetMesh, FName Name, FGeometryScriptWeightMapHandle& Out)`
  (`MeshWeightMapFunctions.h:23-26`) creates a named layer and hands back the index.
- `GetWeightMapHandle(TargetMesh, FName Name, EGeometryScriptSearchOutcomePins& Found)`
  (`MeshQueryFunctions.h:486`) resolves an existing name.
- Value writers: `SetMeshConstantWeightMapValue` (`MeshWeightMapFunctions.h:62-68`),
  `SetMeshSelectionWeightMapValue` (`:73-81`), `SetMeshWeightMapValues` (`:53-56`).
- `FGeometryScriptSimplifyMeshOptions` carries three `FGeometryScriptWeightMapDensity`
  (`MeshSimplifyFunctions.h:111,115,119`), each a `{ Handle, float RelativeDensity }`
  (`:67-83`). `FGeometryScriptRemeshOptions` carries a fourth (`MeshSimplifyFunctions.h:216`).

### Why this is not the dangling-reference problem

A weight map name is a reference to a **mesh attribute layer**, not to a mesh element.

- It survives topology change *by construction*: `FDynamicMeshEditor` interpolates weight layers
  across split and appended vertices (`DynamicMeshEditor.cpp:1869-1875`). A boolean that
  retriangulates the surface carries the field through; `e[4]` does not survive because index 4
  means a different edge afterwards.
- A stale name resolves to **not found**, at compile time, with a line-anchored diagnostic. It can
  never resolve to the *wrong* thing, which is the failure mode that makes `e[4]` and topological
  naming unsolved.
- The material slot table (`PwModelAst.h:86-93`) is the standing precedent: a model-level
  name→binding table, resolved at compile time, with `PWMODEL_UNBOUND_MATERIAL` /
  `PWMODEL_UNUSED_MATERIAL` for both failure directions.

### The op

```
weight_map name="Density" value=0.2
weight_map name="Density" value=1.0 region=sphere center=(0, 0, 40) radius=25
simplify_mesh target_percentage=30 quadric_error_weight_map="Density" quadric_error_weight_density=1.5
```

- `MakeModifier`, part context. Not a generator — it edits the accumulated mesh.
- `name` — required `string`. `FindOrAddMeshWeightMap`, so a repeat statement re-selects the same
  layer rather than creating a second one.
- `value` — number, default 1. First statement for a name calls `SetMeshConstantWeightMapValue`
  (so the field is total: no vertex is left undefined); later statements with a `region` call
  `SetMeshSelectionWeightMapValue`.
- `region` — enum `whole_mesh | box | sphere | plane`, default `whole_mesh`.
  `box_min` / `box_max` (`vector3`), `center` (`vector3`) + `radius` (`number`),
  `plane_origin` + `plane_normal` (`vector3`), matching
  `SelectMeshElementsInBox` (`MeshSelectionFunctions.h:204-209`),
  `SelectMeshElementsInSphere` (`:221-229`) and `SelectMeshElementsWithPlane` (`:340+`).

**Every selector is spatial**, which is the point. `pwmodel-design.md:151-152` already records the
rule — *"If selection is ever added, prefer semantic or positional selectors over indices"* — and
these are positional, evaluated against positions at compile time, storing no element identity in
the document. `SelectMeshElementsInsideMesh` (`MeshSelectionFunctions.h:352-362`) is the obvious
fourth and needs a tool-mesh block; hold it until `cut_material` (§7) has proven the block-selector
shape, since they are the same mechanism.

### The `simplify_mesh` parameters

Six, replacing the "not here" comment at `PwModelParser.cpp:512-514`:

`edge_length_weight_map` / `edge_length_weight_density`,
`geometric_tolerance_weight_map` / `geometric_tolerance_weight_density`,
`quadric_error_weight_map` / `quadric_error_weight_density`.

`*_weight_map` is a `string` naming a layer; `*_weight_density` a `number`, default `0` — which is
the engine's own default and, per `MeshSimplifyFunctions.h:79-82`, means *no effect*, so a map
named with no density is a knowingly inert pair and must **warn**.

Resolution happens in the compiler through `GetWeightMapHandle`. A name that does not resolve is a
`PWMODEL_OP_FAILED` `FOpResult` naming the name and listing the layers that do exist — not a
parser error, because the layer set depends on which ops have run.

`GeometryOps::FSimplifyMeshParams` grows three `{ FString MapName; double Density; }` pairs, and
the RPC front-end (`geometry.simplify_mesh`) gains the same six published names. One struct, two
front-ends, per `pwmodel-design.md:57-60`.

### Two honest caveats to document

- **Weight layers are appended by *index*, not by name.** `DynamicMeshEditor.cpp:2051-2061` copies
  `FMath::Min(NumWeightLayers, …)` layers positionally. Two meshes whose layers were created in
  different orders cross-contaminate through a boolean or a `mirror`. Document that a `weight_map`
  should be authored **after** the booleans that build the part, and warn when a `weight_map`
  statement precedes a boolean in the same part.
- Layer scope is the part's accumulated mesh at the point the op runs, not the model. There is no
  cross-part weight map, and none is needed: every consumer (`simplify_mesh`, `remesh_uniform`)
  is a part-level op.

---

## 4. Object-valued `key = { … }` — closed on a technical ground

**Nothing in the reachable engine surface has the shape only an object grammar can hold.**

Every nested option struct the format touches is a **single, non-array** struct of scalars:

| Struct | Fields | Reached by | Spelled |
|---|---|---|---|
| `FGeometryScriptMeshEditPolygroupOptions` (`MeshModelingFunctions.h:24-33`) | 2 | `extrude`, `inset`, `outset`, `offset_faces`, `poke` | `group_mode=`, `group_id=` |
| `FGeometryScriptPerlinNoiseLayerOptions` (`MeshDeformFunctions.h:88-103`) | 4 | `noise_deform` | `magnitude=`, `frequency=`, `frequency_shift=`, `seed=` |
| `FGeometryScriptGroupLayer` | 2 | `split_normals` | `use_default_group_layer=`, `group_layer_index=` |
| `FGeometryScriptWeightMapDensity` (`MeshSimplifyFunctions.h:67-83`) | 2 | `simplify_mesh`, `remesh_uniform` | `*_weight_map=` + `*_weight_density=` (§3) |

Flattening these is **total and lossless**, and `FGeometryScriptPerlinNoiseOptions::BaseLayer`
(`MeshDeformFunctions.h:147-148`) is a single member, not an array — the engine has exactly one
noise layer, which is why `noise_deform`'s description already says multi-octave noise is two
statements.

The shape flattening *cannot* hold is an **array** of structs. And that shape is now covered
without an object grammar:

- array of homogeneous N-scalar structs → `[(a, b, c), …]`, which is what `frame_list` already is
- array of 2-scalar structs → `int_map` (§2)
- array of arrays → `profile_list` (§1)

What is left uncovered is an array of **heterogeneous** structs (mixed field types per entry).
A grep for `TArray<F…> …;` across the Geometry Script option headers the two front-ends reach
returns exactly one hit: `TArray<FVector> RestrictToViewDirections`, which is a `point_list3`.

So the cost of `key = { }` is a syntax, a recursive sub-spec list on `FPwModelParamSpec`, a nested
`describe_ops` shape every consumer must be taught, a printable type name for "an object of what?"
that the `PwModelParser.h:48-59` argument says cannot exist, and an emitter indentation rule — for
**zero** call sites.

**Falsifier, stated so this is checkable rather than an opinion:** the argument dies the day a
target engine field is a `TArray<FSomeStruct>` or a `TMap<FName, FSomeStruct>` whose entries carry
mixed types. Re-run the grep above before re-opening.

Grammar note for whoever does re-open it: it is *not* ambiguous today. `ParseParams`
(`PwModelParser.cpp:1619-1667`) stops at `{` only where a **key** would start; a `{` after `=` is
unreachable in the current grammar, so a value-position brace is free. The blocker is the spec and
publication machinery, not the parser.

---

## 5. Named `curve` block — closed, and the real problem fixed cheaply

Closed by user decision, recorded in [format-decisions.md](format-decisions.md#grammar). The
reasoning stands on its own: the format stores no cross-references and is therefore immune to the
dangling-reference problem, and a second name space (curve names, ordering rules, unused/unbound
diagnostics, emitter preservation) is not worth deduplicating a list.

**But the pain a `curve` block was reaching for is real and is not about reuse.** The authored path
ceiling is 257 frames (`PwModelCompiler.cpp:279-288`, read out of
`GeometryOps::SplinePathStepCount` rather than typed), and a tuple or list **may not span lines**
(`PwModelParser.cpp:1579-1583`, `pwmodel-format.md:128`). A 257-frame path is one unreadable line
and an undiffable one — which matters precisely because a stable diff is the review mechanism for
a git-tracked source format.

**Fix: let a bracketed list span lines. Tuples still may not.**

- In `ParseValue`'s bracket loop, `SkipNewlines()` after `[`, after each `,`, and before `]`.
- `ParseTupleBody` (`:1508-1538`) is untouched: `(` … `)` stays on one line, so the
  one-statement-per-line recovery model is intact — a runaway `[` is still bounded by its `]` and
  by `ForEachBlockEntry`'s no-progress guard (`:2077-2093`).
- `ParseParams`'s terminator set (`:1623-1624`) is unaffected: it checks for `Newline` between
  *pairs*, and a newline inside a `[…]` is consumed by the value parser before control returns.
- Cost: ~4 lines. Benefit: a 257-frame path, a `profile_list` and an `append_buffers vertices=`
  all become reviewable.

This is a strict grammar widening — every document legal today stays legal — so it does not
disturb the version-0 decision.

---

## 6. Twist on swept paths — the brief's premise is false

`twist=` **already exists, is published, and is consumed**, on both ops and both front-ends:

- op table: `PwModelParser.cpp:986` (`sweep`), `:1002` (`extrude_along_spline`)
- structs: `GeometryOps_Advanced.h:173`, `:203`
- compiler: `PwModelCompiler.cpp:1631`, `:1664`
- RPC: `AdvancedMeshOpsHandler.cpp:225` (`geometry.sweep`)
- applied: `GeometryOps_Advanced.cpp:478-480`, `:510-511`, `:574-576`

**Parallel-transport framing is never used**, so there is no interaction to design.
`AppendSweepPolygon` populates `SweepGen.PathFrames` from the supplied transforms
(`MeshPrimitiveFunctions.cpp:1132-1141`), and `FGeneralizedCylinderGenerator` documents that
`PathFrames[k]` is used *"instead of the propagated InitialFrame"* (`SweepGenerator.h:148-149`).
The plugin always supplies one frame per sample, so the author's per-frame roll **is** the framing
and `twist` composes onto it. Twist is applied about the frame's tangent in both branches — `+X`
on the path branch and `+Z` on the vertical fallback — which is consistent, because
`FFrame3d(Loc, Rot.AxisY(), Rot.AxisZ(), Rot.AxisX())` makes the frame's sweep axis the
transform's forward.

### Three real gaps found in the same code

All three are *hardcoded engine arguments*, not value-system problems, so they are independent of
everything else on this page and can be done at any time.

1. **`RotationAngleDeg` is hardcoded `0.0f`** in both arms of
   `GeometryOpsAdvanced_AppendSweepPolygonCompat` (`GeometryOps_Advanced.cpp:59-88`). The engine
   rotates the cross-section in its own plane by this (`MeshPrimitiveFunctions.cpp:1126`). It is a
   *constant* section orientation, distinct from the progressive `twist` the plugin computes — the
   difference between "start the keyway at 30°" and "wind the keyway 90° along the run". Publish as
   `profile_rotate=` (degrees).
2. **`MiterLimit` is hardcoded `1.0f`**, i.e. mitering off (`SweepGenerator.h:161-173`,
   `EnableMitering`). A sweep through a sharp corner pinches to a smaller cross-section instead of
   holding its width. Publish as `miter_limit=` (`number`, default 1, min 1). Note `EnableMitering`
   also sets `bAlignFramesToSampledTangents = true`, which overrides the author's per-frame roll —
   so the two must be documented as mutually exclusive and the op should warn when a `path=`
   carries non-zero roll *and* `miter_limit > 1`.
3. **Per-frame scale is unreachable.** `SweepGen.PathScales` is read from each frame's
   `GetScale3D().YZ` (`MeshPrimitiveFunctions.cpp:1139-1140`), but `GetFrameList`
   (`PwModelValueRead.h:194-214`) hardcodes `FVector::OneVector`. Authors get only the linear
   `scale_start` → `scale_end` lerp. Fixing it means an 8-component frame — which would be a
   *second* `frame_list` arity and exactly the parameterised-arity trap `PwModelParser.h:48-59`
   rejects. **Recommendation: leave it, and record the reason here.** `scale_start`/`scale_end`
   covers the taper case; a non-monotonic profile scale is better authored as a `loft` (§1), which
   is the general form of exactly that request and now exists.

---

## 7. `cut_material=` — build it; no face identity is required

### Why the deferral argument does not survive contact with the code

The reserved rationale (`pwmodel-format.md:374-377`, `pwmodel-design.md:150-152`) is that
identifying boolean-created faces is the face-identity problem. **For a subtract it is not a
problem at all, because the exposed faces are not "created" — they are the tool mesh's own
triangles, moved.**

Traced end to end in UE 5.8:

- `FMeshBoolean::Compute` for `EBooleanOp::Difference` keeps mesh 0's *outside* and mesh 1's
  *inside* (`MeshBoolean.cpp:394-398`), then reverses every surviving mesh-1 triangle
  (`:710-716`) and appends mesh 1 into the result (`:731-733`).
- `FDynamicMeshEditor::AppendMesh` copies MaterialID **per triangle** from source to destination
  (`DynamicMeshEditor.cpp:2029-2035`).

So the interior surface of a subtract carries the tool's material IDs already; today it carries
ID 0 only because the tool is never tagged. `intersection` and `trim` behave the same way — note
that `.pwmodel`'s `trim` maps to `EGeometryScriptBooleanOperation::Intersection` / `Subtract`
(`GeometryOps_Boolean.cpp:267-269`), **not** to the engine's `TrimInside`/`TrimOutside`, so it is
a genuine two-mesh boolean and does append tool geometry.

### The change

In `FCompiler::RunBoolean` (`PwModelCompiler.cpp:1734-1826`), after `RunOps` builds the tool
(`:1741`) and before the boolean call:

```
if (HasValue(Op.Params, TEXT("cut_material")))
{
    UGeometryScriptLibrary_MeshMaterialFunctions::ClearMaterialIDs(
        Tool.Get(), ResolveSlot(GetString(Op.Params, TEXT("cut_material"))), nullptr);
}
```

`ClearMaterialIDs` enables the MaterialID attribute when missing —
`SimpleMeshMaterialEdit(TargetMesh, /*bEnableIfMissing=*/true, …)`
(`MeshMaterialFunctions.cpp:106-130`, `:43-73`) — so no separate enable is needed. The target mesh
already has the attribute from `RunGenerator`'s own `ClearMaterialIDs` (`:985`).

Parser: `cut_material` joins the four boolean ops' shared `BooleanOptionParams`
(`PwModelParser.cpp:895-913`) as a `String`, and `ValidateParams`'s boolean guard
(`:1896-1902`) keeps rejecting `material=` / `color=` — its message must be reworded to point at
`cut_material=` rather than only at "model the interior as its own part".

`ValidateMaterialSlots` (`PwModelParser.cpp:2770+`) must count `cut_material` as a slot reference.
Today its loop deliberately does not recurse into `Op.Children` (`:2799-2801`); `cut_material` sits
on the boolean op itself, which **is** part-level, so the fix is one more `Op.Params.Find` beside
the existing `material` lookup — not a recursion change.

`RunBoolean`'s "It is always nested, so nothing inside it allocates a material slot" comment
(`:1737-1739`) stays true: the slot is allocated by the boolean op, not by anything in its block.

### Per-op honesty, which must be in the docs

| Op | What `cut_material=` tags |
|---|---|
| `subtract` | Exactly the cut surface — the tool's surface inside the target |
| `intersection` | The tool's contribution to the intersection boundary. A visible exterior surface, not a "cut" |
| `trim` | Same as whichever of subtract/intersection `keep_inside` selected |
| `union` | The tool's **outside** surface — an ordinary exterior face. Almost certainly not what an author means |

**Recommendation: publish it on `subtract`, `intersection` and `trim`; withhold it from `union`,**
and let `union cut_material=` report `PWSRC_UNKNOWN_PARAM` listing the three ops that take it.
`BooleanOptionParams` is already a per-call builder (`:895`) so this is a parameter rather than a
new list.

Caveat to document: `simplify_output=true` (the default) collapses triangles along the seam
(`MeshBoolean.cpp:796-870`), which can move the material boundary by up to one collapsed edge.
It cannot smear across the whole mesh — the pass only visits `CutBoundaryEdges` and runs before the
two meshes are joined — but a razor-exact boundary needs `simplify_output=false`.

**What this does not buy, and must not be sold as:** it does not tag faces the boolean *created*
from scratch (fill-hole triangles from `fill_holes=true`), and it does not let you tag a subset of
the cut. Both would need real face identity. This tags the tool's surface, which happens to be the
cut — a different and much smaller claim.

---

## Shared, format-neutral core

`.pwanim` must not fork this parser. The structural blocker is not naming.

### The blocker: `Private/Model/` is in a gated module

`Source/PinWrightGeometry/` is `"LoadingPhase": "None"` and loads only when `IPluginManager`
reports GeometryScripting enabled. `.pwanim` needs no GeometryScripting. **A shared core cannot
live there.**

Precedent for the move already exists and is documented:
`Tests/Core/TestPwModelDiagnosticCatalog.cpp` was deliberately placed in the always-loaded main
module *"because PinWrightGeometry is optional and gated: hosting it beside the other
PinWright.Model.\* tests would stop the contract from running on exactly the configurations nobody
watches"* (`pwmodel-format.md:1035-1039`).

The include mechanism is free: `PinWrightGeometry.Build.cs:16` already does
`PrivateIncludePaths.Add(… "PinWright", "Private")`, and `PwModelTokenizer.cpp:5` already includes
`IrCore/IrTextUtils.h` from the main module. The dependency direction works today.

### The split

**Move to `Source/PinWright/Private/TextFormat/`** (main module, always loaded, Core-only —
verified: nothing below touches a GeometryScripting header):

| Today | Neutral | Note |
|---|---|---|
| `PwModelToken.h` | `PwToken.h` | zero format-specific content |
| `PwModelTokenizer.h/.cpp` | `PwTokenizer.h/.cpp` | zero format-specific content |
| `EPwModelValueType`, `FPwModelValue` (`PwModelAst.h:18-40`) | `PwValue.h` | plus the `Items` member from §1 |
| all of `PwModelValueRead.h` | `PwValueRead.h` | Core types only, incl. `MakeRotator` |
| `EPwModelSeverity`, `FPwModelDiagnostic` | `PwDiagnostic.h` | the *type*, not the codes |
| `EPwModelParamType`, `FPwModelParamSpec`, `PwModelParamTypeToString`, `PwModelValueTypeToString`, `PwModelParamTypeTupleShape` | `PwParamSpec.h/.cpp` | the vocabulary machinery |
| `ExpectedTupleArity`, `BadValue`, `ValidateRange`, `ValidateValue`, `ValidateParams` (`PwModelParser.cpp:1671-1947`) | `PwParamValidate.h/.cpp` | needs extracting from `FPwModelParserImpl`'s members into free functions over a diagnostic sink |
| `ParseValue`, `ParseTupleBody`, `ParseParams`, `ForEachBlockEntry`, `SkipToNextLine`, `UnexpectedToken` | `PwValueParser.h/.cpp` | the generic `key=value` + brace machinery |
| `FPwModelOp` (`PwModelAst.h:45-53`) | `PwOp.h` | name + params + children is already format-neutral |

**Stays in `PinWrightGeometry/Private/Model/`:**

`BuildOpTable` and everything it builds; `FPwModelDocument` / `FPwModelPart` /
`FPwModelMaterialBinding` / `FPwModelCollision` (`PwModelAst.h:58-153`) — the document *shape*;
`PwModelDiagnosticCodes`; `PwModelWarningNames`; `PwModelCompiler`; `PwModelCollision`.

Keeping the codes under `Private/Model/` also means the catalog test's directory scan
(`Source/PinWright*/Private/Model`) needs no widening.

`ValidateParams`'s `bBooleanOp` flag (`PwModelParser.cpp:1888`) is the one leak: it hardcodes a
pwmodel concept into an otherwise neutral function. Replace with a caller-supplied
`TFunctionRef<bool(const FString& Key, FString& OutMessage)>` rejection hook, or the geometry
module keeps a thin wrapper. The former; the hook is what `.pwanim` will need for its own
per-op-class rejections.

### Rename cost, measured

`grep -ro "PwModel[A-Za-z_]*" Source/` → **3,220 occurrences, 250 distinct symbols, 34 files**.
Of those, `FPwModelValue` / `EPwModelValueType` / `EPwModelParamType` / `FPwModelParamSpec` /
`PwModelParamTypeToString` / `PwModelValueTypeToString` alone account for **671**. Eight docs also
carry `PwModel` identifiers, including six `file:line` citations in `pwmodel-format.md` that go
stale on the move.

Mechanical, but not free, and it collides head-on with every other edit on this page.

### Sequencing recommendation, for the emitter agent

**Do the extraction and rename as one commit, before any of §1–§3 land**, and do it as part of the
emitter's own core-shaping work rather than as a separate pass. Reasons:

- The emitter is first in dependency order and must be written against `PwValue`, not
  `FPwModelValue`; writing it against the old name means renaming it too.
- §1 adds a member to the value struct and §1–§3 add three enum entries to `EPwModelParamType`.
  Doing those first means every one of them is renamed twice.
- A pure rename is reviewable as a pure rename; interleaved with behaviour it is not.

**Concretely for the emitter agent:** the value struct you print must be `PwValue.h` carrying
`Number` / `Tuple` / `TupleList` / `Text` **plus a recursive `TArray<FPwValue> Items`** (§1). Print
`Items` recursively and both `NestedList` and any future depth come out for free. Take the
line-spanning list rule from §5 as an emitter *output* rule too: emit one profile per line inside a
`profile_list`, which is what makes a loft diffable.

---

## Ordering — hard dependency only

```
A. Shared-core extraction + rename          (no dependencies; blocks nothing technically,
   [emitter agent owns this]                 but doing it later doubles every rename below)
        │
        ├── B. NestedList value kind + Items member  ──► C. profile_list constraint ──► D. loft op
        │                                                                                 ▲
        │                                                            E. line-spanning lists (§5)
        │                                                               (independent; D is
        │                                                                unreadable without it)
        │
        ├── F. int_map constraint ──► G. uv udim_resolutions
        │
        ├── H. weight_map op ──► I. simplify_mesh weight-map params
        │
        ├── J. cut_material=
        │
        └── K. sweep profile_rotate= / miter_limit=
```

Genuinely independent of each other, and of everything above: **F/G, H/I, J, K, E**. None of them
touches `FPwModelValue`, so none blocks or is blocked by B/C/D.

The only real dependencies are:

- **B → C → D.** The op cannot validate a type that does not exist; the type cannot constrain a
  value kind that does not exist.
- **H → I.** `simplify_mesh` cannot name a weight map before anything can create one. Shipping I
  first would publish six parameters that always fail to resolve.
- **A before everything**, and that is a *cost* dependency, not a technical one: B/C/F/H/J all
  compile fine against the current names. Stated as an ordering recommendation, not a blocker.

`pwmodel` stays at **version 0** throughout. Every change here is either additive (new value kind,
new param types, new op, new params) or a strict widening (line-spanning lists), so no document
legal today becomes illegal. Existing example files may want regenerating to use the new spellings;
none *needs* it.

---

## Blast radius

**Op table.** 74 entries / 71 distinct names (`Ops.Add(` × 74 in `BuildOpTable`, 6 collision,
3 names in both contexts). Net after this plan: **+2 ops** (`loft`, `weight_map`) and **+3
distinct names** → 76 entries, 73 names. Ops whose parameter lists change: `simplify_mesh` (+6),
`uv` (+1), `sweep` (+2), `extrude_along_spline` (+2), and the four booleans through
`BooleanOptionParams` (+1 on three of them).

**Both front-ends.** `pwmodel-design.md:57-60` is the governing rule: one `F<Verb>Params` per op,
shared. So `FSimplifyMeshParams` (+3 pairs), `FLayoutUVParams` (+1 `TMap`), `FSweepParams` /
`FExtrudeAlongSplineParams` (+2 each), `FBooleanParams` (+`CutMaterialId`), and a new
`FLoftParams` / `FWeightMapParams`. Each gains matching `RPC_PARAM_OPT` entries in
`AdvancedMeshOpsHandler.cpp` / the simplify and UV handlers. `loft`'s new params must **not** be
grafted onto `geometry.loft`, which is a different operation (bounding-box circle through actor
locations, `AdvancedMeshOpsHandler.cpp:121-195`) — that verb keeps its shape and the doc keeps
saying so.

**`model.describe_ops`.** Zero handler changes. `ModelHandler_ParamSpecToJson`
(`ModelCompileHandler.cpp:308-330`) serialises whatever `PwModelParamTypeToString` returns.

**Round-trip test, and one non-obvious trap.**
`ModelHandlersTest_SampleValue` (`TestModelHandlers.cpp:132-162`) needs one arm per new type:

```
profile_list → [[(0,0),(10,0),(10,10),(0,10)], [(0,0),(6,0),(6,6),(0,6)], [(0,0),(3,0),(3,3),(0,3)]]
int_map      → [(1001, 1024), (1002, 512)]
```

The `profile_list` sample must carry **three** profiles to match the existing three-frame
`frame_list` sample (`:159`), because `loft` requires both and their counts must agree. This is why
§1 puts the count check in the **compiler**: as a parser `PWSRC_BAD_VALUE` it would be counted by
`ModelHandlersTest_IsVocabularyMismatch` (`:478-486`) and the two literals would become silently
coupled — change either and a test in another file fails for a reason its name does not suggest.
Make them agree anyway, and say why in a comment.

**Documentation.** `pwmodel-format.md`: Lexical table (`:112-130`) gains a nested-list row and the
line-spanning amendment; §Path-driven (`:259-310`); §`loft` is not in the op table (`:311-326`) is
**deleted and replaced** with what `loft` now does; §Nested engine option structs (`:636-650`)
rewritten around §4's argument; §UV (`:773-777`) retracts the UDIM claim; §`material=` (`:374-377`)
rewritten for `cut_material=`. `pwmodel-design.md:150-152` loses `cut_material` as an example of
the immunity cost and gains the weight-map name as the counter-example of a durable reference.
`docs/wiki-src/model.authoring.md:220` and `model.md:37` both carry the "a real loft needs a list of
lists" paragraph and must be replaced. `GeometryOps_Modeling.h:938-943` carries the wrong UDIM
claim in a source comment. Eight docs carry `PwModel` identifiers that the rename moves.

**No new diagnostic codes**, so `TestPwModelDiagnosticCatalog.cpp` and the
`pwmodel-format.md:964-1063` table are untouched. Keep it that way; it is the cheapest constraint
on this page.

---

## Test strategy, and what would make each test unable to fail

The recurring failure this codebase documents is a test that proves a call returned something —
`sequencer.add_section` shipped a 1000× unit error behind `Sections->Num() == 1`, and
`PinWright.render.capture_asset_preview.PinnedCapturesAreIdentical` skipped its only substantive
assertions in 3 of 3 runs while reporting success. Each item below therefore names its own
falsification.

### §1 `NestedList` + `profile_list` + `loft`

- **Parser, positive:** `profiles=[[…],[…]]` parses to `Type == NestedList`, `Items.Num() == 2`,
  `Items[0].Type == TupleList`, `Items[0].TupleList.Num() == 4`. Assert the *shape*, not
  `Diagnostics.Num() == 0`.
- **Parser, negative, one test per row of the §1 diagnostic table.** Assert the **code and the
  message substring**, not merely that an error occurred. In particular assert the ragged-profile
  message names *both* lengths — that is the one an author acts on.
- **Regression against the old path:** an existing `sweep profile=[(0,0),(1,0),(1,1)]` still parses
  as `TupleList`, and `x=[]` still parses as `TupleList`. Without these, the `ParseValue` peek can
  silently reclassify every list in the format.
- **Compiler, geometry:** loft two squares of different size at two frames 100 apart. Assert
  triangle count **and** that the mesh bounding box spans both profiles' extents and the full path
  length. *Unable to fail if:* it asserts only `bSuccess` or `TriangleCount > 0` — a loft that
  emitted one profile twice, or that collapsed to a point, passes both.
- **Compiler, the honest-scope rejection:** profiles of unequal length produce
  `PWSRC_BAD_VALUE` and the compile creates **no asset** — assert the asset's absence, not just
  the diagnostic. `FPwModelCompiler`'s header promises "never partially creates".
- **Cross-count:** 3 profiles + 2 frames is `PWMODEL_OP_FAILED` naming both counts.

### §2 `int_map` / UDIM

- **Reaches the engine:** the only assertion that matters. Lay out UVs with
  `udim_resolutions=[(1001, 2048), (1002, 128)]` on a mesh with islands in two tiles, then read the
  resulting UVs back and assert the gutter differs between the two tiles.
  *Unable to fail if:* it asserts `FLayoutUVParams::UDIMResolutions.Num() == 2` — that proves the
  parser filled a struct field, which is the half that was never in doubt.
- **The inert-pair warning:** `udim_resolutions=` with `enable_udim_layout=false` warns.
- **Duplicate key** → `PWSRC_BAD_VALUE` naming the repeated key.

### §3 weight maps

- **Density actually changes the simplification.** Simplify the same mesh twice at the same
  `target_percentage`, once with a sphere-region weight map at density 3 and once without, and
  assert the **triangle density inside the sphere differs** — e.g. count triangles whose centroid
  lies within the region and assert the weighted run retains more.
  *Unable to fail if:* it asserts the two runs have different total triangle counts (they may not —
  `target_percentage` fixes the total), or that `GetWeightMapHandle` returned valid (proves the
  layer exists, not that anything read it). This is the single most important assertion on the page
  and the easiest to fake.
- **Unresolvable name:** `quadric_error_weight_map="Nope"` fails the op, names `Nope`, **lists the
  layers that do exist**, and creates no asset.
- **Map without density warns.**
- **The index-not-name append hazard:** two weight maps created in different orders on the two
  sides of a `subtract`, then assert the diagnostic fires. If the warning is not implemented, this
  test is the record that the hazard is known.

### §5 line-spanning lists

- A `path=` broken across 4 lines parses to the **same frame count and same frame values** as the
  one-line spelling. Compare the parsed values element by element.
- A tuple broken across lines still errors with the existing
  `"',' or ')' - a tuple may not span lines"`. *Unable to fail if:* only the positive case is
  tested — the whole risk is that the newline skip leaks into `ParseTupleBody`.
- The unclosed `[` at end of file still terminates and reports, with the no-progress guard intact.

### §6 `profile_rotate` / `miter_limit`

- `profile_rotate=45` on a square section produces a **rotated** section: assert a vertex position,
  not the triangle count, which is identical either way. *Unable to fail if:* it counts triangles.
- `miter_limit=2` through a 90° corner holds cross-section width: measure the width at the corner
  against the width on the straight run.
- The roll-vs-mitering conflict warns.

### §7 `cut_material`

- **The interior faces carry the slot, and the exterior does not.** Subtract a sphere from a box
  with `cut_material="Interior"`, write the asset, then read material IDs back off the built mesh
  and assert: (a) the slot exists on the asset, (b) triangles whose centroid lies within the
  sphere's former radius carry the interior slot's ID, (c) triangles on the box's outer faces do
  not. *Unable to fail if:* it asserts only `MaterialSlots.Num() == 2` — a slot that is allocated
  and tagged onto nothing, or onto everything, passes that.
- `union cut_material=` is `PWSRC_UNKNOWN_PARAM` and the message names the three ops that take it.
- `cut_material="Unbound"` with no `materials { }` entry warns `PWMODEL_UNBOUND_MATERIAL` — this is
  the assertion that proves `ValidateMaterialSlots` was actually taught the new parameter.

### Shared-core extraction

- **The extraction is behaviour-preserving or it is nothing.** Before moving anything, capture the
  full diagnostic output (code, line, column, message) of the existing parser test corpus
  (`TestPwModelParser.cpp`, 1,974 lines) and assert it **byte-identical** afterwards. A rename that
  silently reorders a diagnostic or drops a column is invisible to every existing assertion.
- **The core must not link GeometryScripting.** Assert structurally: the new `TextFormat/` sources
  compile in the main module, which has no GeometryScripting dependency, so a stray include is a
  build failure rather than a test. That is the guarantee worth having — see
  `rpc-design.md` on structural guarantees over discipline.
- **The core must run where the geometry module does not.** The value/tokenizer/param-spec tests
  move into the main module's `Tests/Core/` alongside `TestPwModelDiagnosticCatalog.cpp`, for the
  reason already recorded at `pwmodel-format.md:1035-1039`. Left in `PinWrightGeometry`, they
  silently do not run on a host with GeometryScripting disabled — a green suite that tested
  nothing, which is the exact failure mode this repo has documented.

### One pre-existing gap worth naming

`ModelHandlersTest_BuildDocumentForOp` synthesises **only required** parameters
(`TestModelHandlers.cpp:363-366`, `if (!GetBoolField("required")) continue;`), unlike the
`paramSets` builder, which writes every one (`:417-420`, and its comment says why). So an
**optional** parameter of a brand-new type is published, validated by the parser, and never
round-tripped. `int_map`, the weight-map strings and `cut_material` are all optional. Either
widen the op builder to all parameters — the `paramSets` comment's argument applies verbatim — or
add a targeted parse test per new optional type. Widening is better and is one `continue` deleted;
it will surface other gaps, which is the point.

---

## Bugs found

1. **`material=` inside a boolean block or a `hull` body is accepted and silently discarded.**
   `RunGenerator` applies the slot only when `!bNested` (`PwModelCompiler.cpp:965`), and no parser
   check rejects it — `ValidateOp` (`PwModelParser.cpp:1951+`) validates a block's children with
   the same op specs, and `sphere` legitimately accepts `material=`. So
   `subtract { sphere radius=18 material="Interior" }` parses clean, compiles clean, and the
   parameter reaches nothing. `ValidateMaterialSlots` also cannot see it (`:2794-2801` does not
   recurse into `Op.Children`), so it is not even reported as an unbound slot. This is precisely
   the class the codebase declares worse than a missing parameter — see the `bridge subdivisions`
   removal rationale (`PwModelParser.cpp:1008-1018`) and
   `GeometryOps_Advanced.h:40-44`. **Fix:** a diagnostic on `material=` at nested depth, whose
   message points at `cut_material=` (§7) for the boolean case, since that is now the thing the
   author was reaching for.

2. **`GeometryOps_Modeling.h:940-943` states a false technical reason.** *"UDIMResolutions is NOT
   carried: it is a `TMap<int32, int32>`, and `FPwModelValue` has no map member … so a per-tile
   resolution table has no literal in the format."* `[(1001, 2048), (1002, 1024)]` is a legal
   `TupleList` today. The comment confuses the C++ container with the literal, and it has been
   copied into `pwmodel-format.md:773-777`, `docs/wiki-src/model.authoring.md` and the
   `enable_udim_layout` parameter description (`PwModelParser.cpp:817-819`) — four places asserting
   an unreachability that was never real.

3. **`AppendSweepPolygon`'s `MiterLimit` and `RotationAngleDeg` are hardcoded to 1.0f and 0.0f**
   in both engine-version arms of `GeometryOpsAdvanced_AppendSweepPolygonCompat`
   (`GeometryOps_Advanced.cpp:69-86`). Two engine knobs, unreachable from either front-end, with no
   comment recording the choice — unlike every other deliberate non-publication in this codebase,
   which is documented at the site (`pwmodel-format.md:651-661`). Out of scope as a bug; specified
   as work in §6.

4. **`FSweepParams::ScaleStart` / `ScaleEnd` are applied twice-over in intent.** The plugin lerps
   scale into each `PathFrame`'s `FVector(Scale)` (`GeometryOps_Advanced.cpp:481-483`) *and* passes
   `1.0f` for the engine's own `StartScale`/`EndScale` (`:75-76`, `:85-86`). The engine reads both
   — `PathScales` from the frame (`MeshPrimitiveFunctions.cpp:1139-1140`) *and* `StartScale`/
   `EndScale` (`:1147-1148`) — and `FGeneralizedCylinderGenerator` documents that `PathScales` "is
   combined with StartScale/EndScale, but ignored if bLoop=true". So on `extrude_along_spline`,
   which sweeps as a closed loop, **the frame scale is discarded and `scale_start`/`scale_end` are
   hardcoded to 1** — the two published parameters provably do nothing on that op. Not verified by
   execution (no build/run in this task); verified by reading both call sites and the generator's
   own contract. Worth a targeted test before anything else on `extrude_along_spline` is trusted.

5. **`array_along_path`'s op-table description promises a bound the compiler does not apply.**
   The description says *"1 to 100 of them"* (`PwModelParser.cpp:970-972`) but
   `DispatchOp`'s `array_along_path` branch (`PwModelCompiler.cpp:1616-1623`) deliberately skips
   `ValidatePathFrames` and relies on `ArrayAlongPath`'s internal `ValidateArrayCount`. If that
   internal check's ceiling is not 100, the published number is wrong; if it is, the two are an
   un-asserted coupling — the same shape as the `SplinePathStepCount` ceiling, which was
   deliberately read through the function rather than retyped (`PwModelCompiler.cpp:279-288`).
   Read the constant through the ops layer the same way.

---

**Coordination note for the emitter agent:** the value struct to print is `PwValue.h` with a recursive `TArray<FPwValue> Items` member added alongside the existing `Number`/`Tuple`/`TupleList`/`Text`. Print `Items` recursively and both `NestedList` and any future depth come out of one function. Please own the `PwModel*` → `Pw*` extraction and rename as a single behaviour-preserving commit before any of the grammar work here lands — measured at 3,220 occurrences across 34 source files plus 8 docs, and doing it after §1–§3 means renaming each addition twice.

### Critical Files for Implementation

- `Source/PinWrightGeometry/Private/Model/PwModelAst.h`
- `Source/PinWrightGeometry/Private/Model/PwModelParser.cpp`
- `Source/PinWrightGeometry/Private/Model/PwModelParser.h`
- `Source/PinWrightGeometry/Private/Model/PwModelCompiler.cpp`
- `Source/PinWrightGeometry/Private/Tests/Model/TestModelHandlers.cpp`
