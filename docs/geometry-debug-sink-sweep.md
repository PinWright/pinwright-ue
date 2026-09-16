---
type: reference
summary: "Verdict per GeometryScript call site in PinWrightGeometry on whether its UGeometryScriptDebug* argument can report anything: wired, null-only, guarded by PinWright, reachable-but-not-yet-wired, or no channel in the engine signature. Measured against UE 5.8 engine source."
date: 2026-08-20
tags: [geometry, error-codes, engine-compat, testing, design]
---

# GeometryScript `UGeometryScriptDebug` sweep

Every GeometryScript call site in `Source/PinWrightGeometry/`, with a verdict on whether its
`UGeometryScriptDebug*` argument can carry anything the caller needs. Measured against
**UE 5.8** engine source at `C:\UE_5.8\Engine\Plugins\Runtime\GeometryScripting\` (plus
`Engine\Source\Runtime\GeometryCore\` for `FDynamicMesh3`). Re-verify against the engine before
trusting a row on a different version — several verdicts turn on one `return` statement.

Why this file exists: the argument is the ONLY channel a GeometryScript failure travels on. Every
`UGeometryScriptLibrary_*` entry point returns its `TargetMesh` on every path it has, error paths
included, so `if (!ResultMesh)` behind a `nullptr` Debug is dead code. Mechanism, ownership and
cost: `Handlers/Geometry/GeometryScriptDebugSink.h`. Design rule: `rpc-design.md` §12.

## Counts

| | Call sites | Distinct engine functions |
|---|---:|---:|
| Total in `PinWrightGeometry` (production, excluding test fixtures) | 161 | — |
| Wired to a live sink | 17 | 12 |
| `nullptr`, verdict LEAVE (null-only) | 76 | 30 |
| `nullptr`, verdict LEAVE (guarded by PinWright) | 18 | 9 |
| `nullptr`, verdict WIRE — reachable, **not yet done** | 17 | 13 |
| No `Debug` parameter in the engine signature | 33 | 14 |

Reproduce the wired count with
`grep -rc "Debug\.Get()\|RecomputeNormalsSink\.Get()\|NormalsDebug\.Get()" --include=*.cpp Source/PinWrightGeometry/`.

`PinWrightGeometry` is the only module with GeometryScript calls; the main module and the other
four sub-modules have none.

## Verdict vocabulary

- **WIRED** — a sink is passed and the result is acted on.
- **LEAVE (null-only)** — the engine's only `AppendError` on this function is a
  `TargetMesh is Null` guard, which PinWright rejects before the call (`BeginOp`,
  `GeometryTarget::ResolveOrSendError`, or an explicit handle check). A sink here can never fire.
- **LEAVE (guarded)** — the engine has a reachable error, but PinWright's own validation makes it
  unreachable from any caller. The row says which guard.
- **LEAVE (no channel)** — the function takes no `UGeometryScriptDebug*`.
- **WIRE** — the engine has an error a real caller can provoke and PinWright does not already
  prevent. Not yet done; each needs its own side-effect audit before it is turned on.

## Wired

| Site | Function | Reports |
|---|---|---|
| `GeometryOps_Boolean.cpp:179` (`Boolean`) | `ApplyMeshBoolean` | empty-result refusal → `BOOLEAN_FAILED` |
| `GeometryOps_Boolean.cpp:286` (`Trim`) | `ApplyMeshBoolean` | empty-result refusal → `BOOLEAN_FAILED` |
| `GeometryOps_Boolean.cpp:371/391/429` (`Mirror`) | `ScaleMesh`, `AppendMesh`, `WeldMeshEdges` | null-only + the weld error flag; kept as a future-engine guard |
| `MeshIOHandler.cpp` (`FinishMeshIOImport`) | `AppendBuffersToMesh` | refused triangles → `IMPORT_FAILED`, or `allowPartial` + `droppedTriangles` |
| `GeometryOps_Advanced.cpp` (`AppendBuffers`) | `AppendBuffersToMesh` | refused triangles → `MESH_APPEND_FAILED`, mesh rolled back |
| `GeometryOps_Advanced.cpp` (`Loft`) | `RecomputeNormals` | warning only (per-vertex fallback) |
| `GeometryOps_Modeling.cpp` (`RecalculateNormals`, `Spherify`, `Cylindrify`) | `RecomputeNormals` | warning only (per-vertex fallback) |
| `GeometryOps_Modeling.cpp` (`RecomputeTangents`) | `ComputeTangents` | missing UV set / normals, MikkT failure → `TANGENTS_FAILED`; platform fallback as a warning |
| `GeometryOps_Modeling.cpp` (`Subdivide`, `Poke`) | `ApplyPNTessellation` | `TESSELLATION_FAILED` — **no reachable failure on 5.8**, see below |
| `GeometryOps_Modeling.cpp` (`UnwrapUVXAtlas`) | `AutoGenerateXAtlasMeshUVs` | non-compact mesh, generation failure → `UV_GENERATION_FAILED` |
| `GeometryOps_Modeling.cpp` (`AutoUVPatchBuilder`) | `AutoGeneratePatchBuilderMeshUVs` | missing polygroup layer, generation failure → `UV_GENERATION_FAILED` — **no failure test**, see below |
| `PwModelCompiler.cpp` (`ApplyUVOp`, xatlas branch) | `AutoGenerateXAtlasMeshUVs` | generation failure, as a `.pwmodel` stage failure. **Not** the non-compact refusal: this site calls `CompactMesh` first when the mesh is non-compact, because a `.pwmodel` part reaches the uv stage non-compact whenever a boolean ran and the format has no `compact` op for the author to reach for. The op layer keeps reporting it — see below |

### The two XAtlas sites report differently, on purpose

`GeometryOps::UnwrapUVXAtlas` **reports** the non-compact refusal; `PwModelCompiler`'s xatlas
branch **prevents** it by compacting first. That is not an inconsistency to tidy up.

`geometry.unwrap_uv` / `auto_uv` / `pack_uv_islands` run one op against a caller-owned named mesh.
The caller chose the op order, still holds the vertex and triangle ids, and can call a compacting
verb itself; silently renumbering under it would be the surprise, and
`Geometry.Ops.Modeling.XAtlasRefusesANonCompactMeshInsteadOfReportingSuccess` pins the reporting.

The `.pwmodel` compiler owns the mesh end to end inside a part, and the format has no `compact`
op — so a document with `subtract` before `uv mode=xatlas` (the ordinary shape) had no expression
that could make the unwrap work. Reporting there names a remedy the author cannot apply.

Worth knowing because it is what this whole sweep is for: that refusal was firing on every such
document long before a sink existed. `Saved/Logs/pw_full.log` in the host project carries the
engine's own `AutoGenerateXAtlasMeshUVs: TargetMesh is non-Compact` twice, in the same second that
`Model.Compiler.RecompilingAnXAtlasUnwrapReproducesTheSameMesh` reported `Result={Success}` — the
test named for XAtlas determinism had never run XAtlas, and reproduced an earlier stage's box
projection instead. Fact 1 below (`Debug == nullptr` does not suppress the log) is what makes that
recoverable from an old log rather than merely suspected.

### Wired but untested: `AutoGeneratePatchBuilderMeshUVs`

Its one caller-reachable refusal is "Requested Polygroup Layer does not exist", raised when
`Options.bRespectInputGroups` is set and `FPolygroupLayer::CheckExists` fails. `CheckExists` on
the default layer is `Mesh->HasTriangleGroups()` (`PolygroupSet.cpp`), so a fixture that calls
`FDynamicMesh3::DiscardTriangleGroups()` on a box and then asks the op to respect groups should
provoke it. It does not: the engine raises nothing, and raises nothing into `LogGeometry` either,
which is decisive because `MakeScriptError` logs whether or not a `Debug` object is passed. The
mesh still had triangle groups by the time the engine looked, and nothing on the path between
(`BeginOp`, `EnsureMeshHasUVChannel`, `ApplyMeshUVEditorOperation`, the `FDynamicMeshUVEditor`
constructor) re-enables them. Unexplained; recorded rather than papered over. Its other two
refusals are pre-empted by `EnsureMeshHasUVChannel` or need a mesh the parameterizer cannot
partition. The site keeps a success-direction control test only.

### Wired but unprovokeable: `ApplyPNTessellation`

Stated rather than left as a silent gap. Its two error paths are `FPNTriangles::Validate()`,
which fails only on `TessellationLevel < 0 || Mesh == nullptr` — PinWright passes a literal `1`
and a mesh `BeginOp` already guarded — and `FPNTriangles::Compute()`, whose `return false`
statements are the same null/negative check, a `FProgressCancel` PinWright does not pass, and a
missing-normals check that cannot fire because `Compute` computes per-vertex normals itself when
the mesh has none (`PNTriangles.cpp`, the `bHasNormals` branch). There is therefore **no test**
for these two sites, deliberately: a test would be one that cannot fail.

## LEAVE (null-only) — the engine raises nothing else

Every one of these has exactly one `AppendError`, on a null `TargetMesh`/`Component`/`Skeleton`,
which PinWright rejects first.

| Function | Sites |
|---|---|
| `AppendBox`, `AppendSphereBox`, `AppendSphereLatLong`, `AppendCylinder`, `AppendCone`, `AppendCapsule`, `AppendTorus`, `AppendRectangleXY`, `AppendDisc`, `AppendLinearStairs`, `AppendCurvedStairs` | `GeometryOps_Primitives.cpp` ×13, plus every test fixture that builds a probe mesh |
| `AppendMesh`, `AppendMeshRepeated` | `GeometryOps_Boolean.cpp:507`, `GeometryOps_Primitives.cpp:536`, `PwModelCompiler.cpp:1018/2140` |
| `ApplyMeshSelfUnion` | `GeometryOps_Boolean.cpp:338` — the engine's own failure report is **commented out** (`MeshBooleanFunctions.cpp`, "currently ignoring bSuccess"), so a failed self-union is indistinguishable from a successful one through any channel |
| `FillAllMeshHoles` | `GeometryOps_Advanced.cpp:153/239`, `GeometryOps_Modeling.cpp` (`FillHoles`) — failures arrive on the `NumFailedHoleFills` out-param instead, which PinWright already reads |
| `RepairMeshDegenerateGeometry`, `CompactMesh` | `GeometryOps_Modeling.cpp` (`RemoveDegenerates`, `MergeVertices`) |
| `FlipNormals` | `GeometryOps_Modeling.cpp` (`FlipNormals`) |
| `ApplySimplifyToTriangleCount` | `GeometryOps_Modeling.cpp` (`SimplifyMesh`), `LODCollisionHandler.cpp:130`, `PwModelCollision.cpp:410` |
| `ApplyMeshLinearExtrudeFaces`, `ApplyMeshInsetOutsetFaces`, `ApplyMeshOffsetFaces`, `ApplyMeshShell` | `GeometryOps_Modeling.cpp` (`Extrude`, `Inset`, `Outset`, `OffsetFaces`, `Poke`, `Shell`) |
| `ApplyBendWarpToMesh`, `ApplyTwistWarpToMesh`, `ApplyFlareWarpToMesh`, `ApplyIterativeSmoothingToMesh` | `GeometryOps_Modeling.cpp` (`Bend`, `Twist`, `Taper`, `Smooth`, `Relax`) |
| `TranslateMesh`, `ScaleMesh`, `TransformMesh` | `GeometryOps_Elements.cpp:287`, `GeometryOps_Modeling.cpp` (`Stretch`), `PwModelCompiler.cpp:1721/1980` |
| `ClearMaterialIDs` | `PwModelCompiler.cpp:985` |
| `ComputeSmoothBoneWeights` | `SkeletalMeshAssetIOHandler.cpp:790` |
| `SetDynamicMeshCollisionFromMesh`, `GenerateCollisionFromMesh`, `SetSimpleCollisionOfDynamicMeshComponent` | `CollisionHelpers.h:33/41/48/50`, `PwModelCollision.cpp:496` |

Three of those deserve a caveat, because "raises nothing" is not the same as "cannot fail":

- `ApplyMeshShell` discards the return of `Editor.WeldVertexLoops` and `Join.Apply()`. A shell
  that fails to stitch is silent through every channel.
- `GenerateCollisionFromMesh` does its work in `UELocal::ComputeCollisionFromMesh`, which takes
  no `Debug` at all. A convex decomposition that yields zero hulls returns successfully.
- `ComputeSmoothBoneWeights` ignores `SkinBindingOp.CalculateResult(nullptr)`. A failed bind
  still overwrites the mesh.

Those are engine gaps, not call-site defects; a sink cannot close them.

## LEAVE (guarded) — reachable in the engine, unreachable through PinWright

| Site | Function | Engine error | Why it cannot fire here |
|---|---|---|---|
| `GeometryOps_Modeling.cpp` (`ProjectUV` ×3, `TransformUVs` ×3) | `SetMeshUVsFrom*Projection`, `Translate/Scale/RotateMeshUVs` | "UVSetIndex does not exist on TargetMesh" | `GeometryUtils::EnsureMeshHasUVChannel` creates the layer one statement earlier |
| `GeometryUtils.cpp:359` | `SetMeshUVsFromBoxProjection` | same | same guard, inside `EnsureMeshHasUVs` |
| `PwModelCompiler.cpp:400` | `SetMeshUVsFromBoxProjection` | same | same guard, inside `BoxProjectChannel` |
| `GeometryUtils.cpp:380` | `SetNumUVSets` | "Maximum of 8 UV Sets are supported" | `EnsureMeshHasUVChannel` range-checks the channel first |
| `GeometryOps_Advanced.cpp:70/79` | `AppendSweepPolygon` | "PolygonVertices requires at least 3", "SweepPath requires at least 2" | `GeometryOps::Sweep`/`ExtrudeAlongSpline` clamp both counts through `ClampCountWarn` |
| `GeometryOps_Primitives.cpp:495/555/596` | `AppendRevolvePolygon`, `AppendSimpleExtrudePolygon`, `AppendRevolvePath` | "requires at least N positions" | the profile builders synthesise the vertices; no caller array reaches them |
| `GeometryOps_Boolean.cpp:554/590` | `AppendMeshTransformed` | "AppendTransforms array is empty" | both array ops reject a zero count before building the transform list |
| `GeometryOps_Modeling.cpp` (`WeldVertices`), `GeometryOps_Advanced.cpp:314` (`EdgeSplit`) | `WeldMeshEdges` | "Weld Operation returned error flag" | `FMergeCoincidentMeshEdges::Apply()` has one `return` statement and it is `return true` |

The `WeldMeshEdges` row is the same conclusion `GeometryOps::Mirror` reached; Mirror passes a sink
anyway, as a guard against an engine that starts raising the flag. The two rows above do not, and
that inconsistency is deliberate only in the sense that Mirror's sink was already there — either
choice is defensible, and the cheap one is to leave them.

## WIRE — reachable, caller-provokeable, not yet done

Ordered by consequence: the first seven are asset or bone paths where a discarded failure leaves
something wrong **on disk**, the rest return a wrong answer over an unchanged mesh. Thirteen
functions across seventeen call sites (`CopyMeshToStaticMesh` accounts for four,
`ApplyPerlinNoiseToMesh` for two).

| Site | Function | Reachable error | Mutated first? |
|---|---|---|---|
| `SkeletalMeshAssetIOHandler.cpp:1009` | `CopyMeshToSkeletalMesh` | "Source geometry contains bone '{0}' which does not exist on the skeletal mesh asset" (`RemapGeometryToReferenceSkeleton`); "Failed to generate the mesh data for the Target LOD Index"; "Cannot modify built-in engine asset"; "Writing HiResSource LOD is not yet supported" | **YES** — LODInfos appended, `Modify()`/`PreEditChange` already called, and the engine returns without `EndTransaction()`/`PostEditChange()` |
| `MeshOpsHandler.cpp:645/649/661/665` | `CopyMeshToStaticMesh` | "Can only Replace Materials when updating LODs"; "Cannot modify built-in Engine asset"; "MeshDescription for HiRes/LOD is null?" | **YES on the last two** — build settings overwritten, source model count raised, transaction left open |
| `SkeletalMeshAssetIOHandler.cpp:1063` | `CreateNewSkeletalMeshAssetFromMesh` | "LOD 0 has no skin weight attributes"; "LOD 0 has no triangles"; "invalid skinning data or the bone data is not compatible with the specified skeleton"; "Failed to create new Asset" | first two: no; last two: transaction left open |
| `SkeletalMeshAssetIOHandler.cpp:769` | `CopyBonesFromSkeleton` | "Found multiple root bones on source mesh"; "Target bone '{0}' not found on source mesh"; "Invalid bone index found on mesh"; "Failed to reindex bone weights for {0} weights profile" | partially — attributes enabled, and a multi-profile reindex can fail after rewriting earlier profiles |
| `SkeletalMeshAssetIOHandler.cpp:575` | `CopyMeshFromSkeletalMesh` | "Renderdata for specified LOD is not available"; "Requested LOD source mesh is not available"; plus a warning for unreferenced vertices | no |
| `MeshAssetIOHandler.cpp:335` | `CopyMeshFromStaticMesh` | the engine forwards `UE::Conversion::StaticMeshToDynamicMesh`'s own `FText` — missing LOD, no render data, source models unavailable | no |
| `SkeletalMeshAssetIOHandler.cpp:375` | `GetAllBonesInfo` | "TargetMesh has no bone attributes" — and the out-array is left **untouched**, not emptied, so a stale caller array reads as a result | no (read-only). Also the one site where the Debug argument is dropped by *defaulting* rather than by writing `nullptr` |
| `CollisionHelpers.h:44` | `GetSimpleCollisionFromComponent` | "Component's BodySetup is Null" — reachable with a perfectly valid component | no (read-only) |
| `GeometryOps_Modeling.cpp` (`SplitNormals`) | `ComputeSplitNormals` | "Polygroup Layer does not exist" when `bSplitByFaceGroup` | no |
| `GeometryOps_Modeling.cpp` (`RemeshUniform`) | `ApplyUniformRemesh` | "Error computing result, returning input mesh" | no — the engine restores from its own copy first |
| `GeometryOps_Modeling.cpp` (`Bevel`) | `ApplyMeshPolygroupBevel` | "Filter box does not contain any Polygroup Edges, bevel will not be applied" when `bApplyFilterBox` | no |
| `GeometryOps_Modeling.cpp` (`NoiseDeform`) | `ApplyPerlinNoiseToMesh2` | "TargetMesh did not have overlay normals computed" when `bApplyAlongNormal` + `AverageFromOverlay` | no |
| `GeometryOps_Modeling.cpp` (`LayoutUV`) | `LayoutMeshUVs` | "Error computing result, returning input mesh" | no — engine restores first |

## LEAVE (no channel) — 33 sites, no `Debug` parameter exists

`MeshQueryFunctions` (`GetMeshBoundingBox` ×9, `GetVertexCount` ×3, `GetNumUVSets` ×4,
`GetNumOpenBorderEdges` ×4, `GetNumConnectedComponents`, `GetMeshVolumeArea`, `GetVertexPosition`,
`GetHasTriangleNormals`, `GetHasVertexColors`, `GetHasMaterialIDs`),
`MeshSelectionFunctions::SelectMeshElementsByNormalAngle`,
`CollisionFunctions::GetSimpleCollisionShapeCount` ×2,
`MeshMaterialFunctions::GetMaxMaterialID`,
`MeshBoneWeightFunctions::MeshHasBoneWeights` / `GetVertexBoneWeights` / `MeshCreateBoneWeights`
(their last parameter is a weight `Profile`, not a Debug pointer — easy to misread).

Nothing can be done at these sites; where they can fail quietly they do so through a `bool&`
out-param the caller must read.

## Two engine facts worth carrying forward

1. **`Debug == nullptr` does not suppress the log.** `MakeScriptError`/`MakeScriptWarning`
   `UE_LOG` to `LogGeometry` unconditionally, before the null check. A discarded failure is
   invisible to the RPC caller but visible in the editor log — which is how these were confirmed
   without a debugger, and why grepping `LogGeometry: Error` after a suspicious "success" is a
   fast first check.
2. **`EGeometryScriptErrorType` is a category, not a severity.** Severity is
   `EGeometryScriptDebugMessageType` (Error/Warning). Several *warnings* carry
   `ErrorType::InvalidInputs`, so filtering on `ErrorType` mis-classifies them. `FGeometryScriptDebugSink`
   filters on `MessageType`, which is the correct field.
