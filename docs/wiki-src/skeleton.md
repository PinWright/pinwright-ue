# skeleton

Author `USkeleton`, `SkeletalMesh`, `PhysicsAsset`, sockets, virtual bones, morph targets, cloth, skin weights, ragdoll constraints, and retarget setup. Broader clips, montages, IK retargeters, and animation Blueprints live under `call("animation")` and `call("anim")`.

## Keep `.pwskel` beside the generated asset

Recommended layout: `Content/<Rel>/<Name>.pwskel` beside `Content/<Rel>/<Name>.uasset`. Unreal's asset registry recognizes `.uasset`/`.umap`, not `.pwskel`; extension-based Git LFS rules leave the source diffable text.

With that layout, `<ProjectDir>/Content/<Rel>/<Name>.pwskel` derives `/Game/<Rel>/<Name>` and makes `outputPath` optional. Otherwise pass it explicitly. A non-derivable omission fails `SOURCE_OUTPUT_PATH_NOT_DERIVABLE` with the reason and never guesses. Existing layouts remain supported and derived targets still obey provenance/`overwrite` refusal.

## Sockets live on one of two assets, and which one decides who else is affected

**Read this before any `skeleton.*_socket` verb.** A `USkeletalMeshSocket` belongs to either a `USkeleton` or one `USkeletalMesh`; they are not interchangeable:

- **Skeleton socket** — stored on `USkeleton`; **every** bound `SkeletalMesh` sees it.
- **Mesh-only socket** — stored on one `SkeletalMesh`; other meshes are untouched.

**The target path chooses the owner.** `skeletonPath` writes the shared skeleton; `skeletalMeshPath` writes one mesh. Both together return `AMBIGUOUS_TARGET`; the verb never silently chooses.

**`skeleton.list_sockets` reports `owner` (`"mesh"` / `"skeleton"`) and `ownedBy` per socket, plus `meshOnlyCount` / `skeletonCount`.** `skeletalMeshPath` lists both; `skeletonPath` alone cannot list mesh-only sockets. Mesh sockets come first, matching `USkeletalMesh::FindSocket`: **a same-named mesh-only socket shadows the skeleton socket**, silently changing attachment transform. `create_socket` rejects names present on either list.

**`asset.dump`'s `skeletal_mesh.json` merges both sets** into `sockets` via `USkeletalMesh::GetActiveSocketList()` and marks `ownedBy`; without it, editing a shared skeleton socket could affect every bound mesh.

With `skeletalMeshPath`, `configure_socket` and `delete_socket` search mesh sockets first and report the asset written. `list_sockets`, `configure_socket`, `delete_socket`, and `create_socket` require exactly one target and return `AMBIGUOUS_TARGET` otherwise. Sockets are not authorable in `.pwskel` or `.pwmodel`; on generated assets they are protected out-of-band state. A source compile stops before erasing them with `PWSRC_RECOMPILE_UNMANAGED_STATE`; use an authorable field where available or `overwrite: true` to discard them. The permitted rebuild returns the same code as a warning and does not copy omitted state.

Takeovers use the same protection: the guard compares live asset state directly with incoming source. Without `overwrite: true`, refusal names each socket or other lost value; with it, takeover succeeds and the list remains a warning.

## Asset paths are type-checked, and updates must do work

An existing wrong-class asset is not “missing.” Handlers return `INVALID_ASSET_TYPE`, naming actual/expected classes and the next path; a `USkeleton` in `skeletalMeshPath` is not `MESH_NOT_FOUND`. Physics/morph handlers apply the same rule to `UPhysicsAsset`/`USkeletalMesh`. Exception: `skeleton.create_physics_asset` accepts either `USkeletalMesh` or bare `USkeleton`.

`skeleton.set_bone_transform` always edits the resolved `USkeleton`: `skeletonPath` names it; `skeletalMeshPath` resolves the mesh's bound skeleton. It never edits a mesh reference skeleton. `skeleton.create_socket` requires an existing bone, preventing a socket orphaned by the next rebuild.

`skeleton.configure_socket`, `skeleton.configure_physics_body`, and `skeleton.configure_constraint_limits` reject requests with no update field. Success changes/marks the in-memory asset; `McpSafeAssetSave` does not immediately write `.uasset`. Persistence fields stay `saved:false`, `pendingFlush:true` until project save/flush.

`skeleton.compile` verifies the finished asset before success: reference bones, local transforms, private retargeting table, and source-owned metadata must read back as described or it returns `PWSKEL_ASSET_POSTCONDITION_FAILED`. A successful hierarchy change refreshes loaded animation assets without an editor restart.

## Skin weights: base skinning vs. alternate profiles

**Read this before using any `skeleton.*` weight verb.** A `SkeletalMesh` carries two separate, non-interchangeable influence sets.

- **Base skinning** — `FSkelMeshSection::SoftVertices` in the LOD MeshDescription; this is what the renderer and FBX import use.
- **Named alternate profiles** — `FSkinWeightProfileInfo` + `FImportedSkinWeightProfileData`, stored through the engine's *alternate influences* channel (`FLODUtilities::UpdateAlternateSkinWeights`). They matter only when a `SkinnedMeshComponent` activates one **by name** at runtime.

**All four weight mutators—`normalize_weights`, `prune_weights`, `set_vertex_weights`, `copy_weights`—write a named alternate profile and leave base skinning untouched.** Each says `wrote: "alternateSkinWeightProfile"`, `baseSkinningModified: false`. They do not change rendering, and no error reports that fact.

**To change base skin weights, use the geometry round trip:**

1. `geometry.create_from_skeletal_mesh` — load into a DynamicMeshActor.
2. `geometry.bind_skin_weights` — smooth-bind against the skeleton with `DIRECT_DISTANCE`, **after** every geometry edit; an earlier bind leaves later vertices uninfluenced.
3. `geometry.convert_to_skeletal_mesh` with `overwrite: true` — writes the LOD MeshDescription in place and preserves material slots. Without it, an occupied path returns `ASSET_EXISTS` because creation empties the asset in place.

**To verify base weights, read `baseSkinning` from `skeleton.describe_skin_weights` or gate on `skeleton.audit_skin_weights`.** Both read section soft-vertices; reading a profile verifies only that alternate profile.

**Bone index spaces.** A bone number is meaningless without its space:

- **Reference-skeleton index** — reported by `skeleton.list_bones`, accepted by `set_vertex_weights`, and reported under `baseSkinning`.
- **Section-local slot** — index into `FSkelMeshSection::BoneMap`, used by a soft vertex/profile `SkinWeights`; slot 3 in section 0 differs from slot 3 in section 4.

Every `describe_skin_weights` block carries `boneIndexSpace`, and each sampled influence resolves `boneName`. The spaces coincide on a single-section mesh, hiding mistakes until a multi-section character.

## A skeleton with no preview mesh opens with an empty viewport

A `USkeleton` carries a preview mesh reference. A `.pwskel` can declare `preview_mesh path="/Game/..."`, normally on a second compile after the mesh exists. Other creation paths leave it empty; `skeleton.set_preview_mesh` fills it and `skeleton.get_info` reports `previewMesh` (empty means unset). Put the value back into `.pwskel` before recompiling a generated skeleton.

**The mesh must be compatible, and the verb refuses otherwise.** `USkeleton::SetPreviewMesh` validates nothing and can store an unrelated mesh, but the next slot read runs `IsCompatibleForEditor` and *silently resets the reference* without dirtying. The verb checks that predicate first instead of reporting a success the engine later undoes.

This is separate from `skeleton.compile`: the mesh often does not exist yet (build skeleton, skin mesh, *then* preview), and the slot belongs to `USkeleton` regardless of creation path.

## See also

- [`asset`](asset.md) for the shared dump sidecar schema and dump-parity live read policy.
- [`geometry`](geometry.md) for the skeletal round trip that writes base skin weights.

### skeleton.describe_mesh

Read-only dump-parity metadata for `USkeletalMesh`; response matches the `skeletal_mesh.json` sidecar from `asset.dump` / `asset.dump_folder`.

Params:

- `skeletalMeshPath` (string, required): asset path to a `USkeletalMesh`.

Returns `bounds` (`origin`, `extent`, `sphereRadius`), `materials` (`slot`, `path`, `importedSlot`), `lods`, per-LOD `trianglesByLod` / `verticesByLod` / `sectionsByLod` / `numTexCoordsByLod`, and `physicsAsset`/`skeleton` paths (empty when unset).

This is geometry/material/LOD only; it does **not** report skin-weight profiles. Verify `skeleton.copy_weights` / `normalize_weights` / `set_vertex_weights` with [`skeleton.describe_skin_weights`](skeleton.describe_skin_weights.md).

Errors:

- `MESH_NOT_FOUND`: no asset at `skeletalMeshPath`.
- `INVALID_ASSET_TYPE`: existing path has another class; message names it and the `USkeletalMesh` path to use.

### skeleton.list_bones

Enumerate a `USkeleton` reference skeleton. This is the canonical **bone-name source**, especially for `startBone` / `endBone` in `animation.authoring.add_ik_chain` (`CHAIN_NOT_ADDED` for a bad name) and `boneName` in `skeleton.get_bone_transform`. `animation.authoring.get_animation_info` on a `USkeleton` returns only `{assetType:"Skeleton"}` (no bones), and `asset.get` has no list; do not use their dump sidecar instead.

An ordinary rig (~200 humanoid or ~60 creature bones) exceeds the 10000-character inline budget and spills to `Saved/PinWright/HttpResponses/*.json`. Keep it inline with `nameFilter=` (case-insensitive substring; e.g. `hand` → `hand_r`/`hand_l`), `limit=` (while `totalCount` remains full), and `namesOnly=true` / `fields=[...]` (drop bulky per-bone `location`).

Params:

- `skeletonPath` (string, optional): primary `USkeleton` path.
- `skeletalMeshPath` (string, optional): fallback `USkeletalMesh` path; resolves its bound Skeleton when `skeletonPath` is omitted.
- `nameFilter` (string, optional): case-insensitive substring; aliases `name_filter` / `boneName` / `bone_name`.
- `limit` (number, optional, default `0` = all): max rows after filtering; `totalCount` remains the full match count.
- `fields` (array, optional): allow-list `name`, `index`, `parentIndex`, `parentName`, `location`; a string is a single-key shorthand; omit for all.
- `namesOnly` (bool, optional): shorthand for `fields:["name"]`; ignored when `fields` is supplied.

One of `skeletonPath` / `skeletalMeshPath` is required.

Returns:

- `bones`: reference-skeleton bones in index order with `name`, `index`, `parentIndex` (`-1` root), root-omitted `parentName`, and ref-pose `location` (`{x, y, z}`); projections drop unlisted keys.
- `count`: returned rows after `nameFilter` + `limit`.
- `totalCount`: full match count (unfiltered equals `boneCount` from `skeleton.get_info`); `count < totalCount` means capped.
- `truncated`: `true` when `limit` elided rows; `filter`: resolved filter only when applied.

Errors:

- `SKELETON_NOT_FOUND`: neither path loaded or mesh has no bound Skeleton.
- `INVALID_ASSET_TYPE`: existing path has another class; message names it and the `USkeleton`/`USkeletalMesh` path to use.

### skeleton.list_physics_bodies

Enumerate `UPhysicsAsset` bodies (`USkeletalBodySetup`) for ragdoll audit: each row has `boneName`, `considerForBounds`, `collisionType`, and `sphereCount` / `boxCount` / `capsuleCount` / `convexCount`. This is the **full-enumeration** verb; `skeleton.get_physics_asset_info` otherwise gives counts and opt-in `bodies[]`/`constraints[]`.

The method returns **all** matching bodies. A normal ~30+ body ragdoll can exceed the ~10000-character inline budget; the response returns `outputTooLong` and spills to `Saved/PinWright/HttpResponses/.../<uuid>.json`. Narrow it inline:

- `boneName` (string, optional): case-insensitive substring; aliases `bone_name`, `nameFilter`, `name_filter`.
- `limit` (number, optional, default `0` = all): max filtered bodies; `count`, full `totalCount`, and `truncated` expose elision.
- `namesOnly` (bool, optional) / `fields` (array, optional): projection. `namesOnly=true` returns only `boneName`; `fields` allow-lists `boneName`, `considerForBounds`, `collisionType`, `sphereCount`, `boxCount`, `capsuleCount`, `convexCount` (a string is accepted). Omit both for all.

One of `physicsAssetPath` / `skeletalMeshPath` is required; the latter resolves the mesh's bound physics asset. Returns `physicsBodies`, `count`, `totalCount`, `truncated`, `constraintCount`, and applied `filter`. With no `limit`/`fields`/`namesOnly`, output is byte-identical to the prior all-bodies shape plus `totalCount`/`truncated`.

### skeleton.describe_skin_weights

Read-only readback of **both** halves of `USkeletalMesh` skinning: `baseSkinning` (section soft-vertices used by the renderer) and `profiles` (named alternate influence sets written by `skeleton.*` mutators). See *Skin weights* above.

Previously it reported only profiles, so a mutator and this read agreed while both disagreed with the mesh. A mesh without an authored profile returned `profileCount: 0` success even though nothing was measured.

Params:

- `skeletalMeshPath` (string, required): `USkeletalMesh` path.
- `profileName` (string, optional): limit **profiles** to one; does not affect `baseSkinning`.
- `lodIndex` (number, optional): limit to one LOD; omit for all.
- `sampleCount` (number, optional, default `0`): first-N vertices per LOD in `sample`, for base and profiles.
- `includeBaseSkinning` (bool, optional, default `true`): keep true; only this block describes rendered weights.

Returns:

- `skeletalMeshPath`, `lods`, `profileCount`, `profileKind`, `note`.
- `baseSkinning` (unless `includeBaseSkinning: false`): `source: "sectionSoftVertices"`, reference-skeleton `boneCount`, and per-LOD `lodIndex`, `sectionCount`, `clothSectionCount`, `disabledSectionCount`, `unmappedInfluences`, `boneIndexSpace: "referenceSkeleton"`, `boneCoverage`, shared validity fields, and a warning when `unmappedInfluences > 0`. Such section-local slots are **dropped rather than resolved to bone 0**, so counts are lower bounds.

`boneCoverage` appears on every `baseSkinning` LOD and shows which bones move it and which vertices one bone owns:

- `bonesWithNoInfluence`: reference-skeleton bones **no vertex is weighted to**. **Always emitted, including empty**, because absence could mean no measurement. It finds an animation curve driving a bone that owns no geometry even when all other weights look valid.
- `bonesWithNoInfluenceCount`, `influencedBoneCount`, `boneCount`.
- `bones`: uncapped, most-rigid-first rows `{bone, boneIndex, influencedVertices, rigidVertices}`. `rigidVertices` counts vertices whose **only** influence at/above `weightEpsilon` is that bone; equality with `influencedVertices` identifies deliberately rigid geometry (prop, weapon, eyeball, or attachment cape).
- `rigidVertexCount` (LOD total) and `weightEpsilon`; `skeleton.audit_skin_weights` exposes the same epsilon as `minInfluenceWeight` and uses the same code.

Coverage is reported on `baseSkinning` only: profile `SkinWeights` are alternate and ignored unless activated. `profiles` has one row per matching `FSkinWeightProfileInfo`, with `name` and `lods` (`lodIndex`, `present`, `boneIndexSpace: "sectionLocal"`, and shared validity fields).

Every per-LOD block shares `vertexCount`, `maxInfluencesPerVertex`, `normalizedVertexCount` (sum ~1.0), `zeroWeightVertexCount` (no influences), `degenerateVertexCount` (has influence but sum != 1.0), and, when `sampleCount > 0`, `sample: {vertexIndex, weightSum, influences:[{boneIndex, refSkeletonBoneIndex, boneName, weight}]}`. `boneIndex` uses the block's space; `boneName` resolves through its section `BoneMap`. A missing map entry is `boneName: "<unmapped>"`, never a fabricated root.

**`baseSkinning` and `profiles` disagreeing is normal**: mutators write only `profiles`. `skeleton.audit_skin_weights` reads the same base skinning but returns a verdict; `skeleton.describe_mesh` reports no skin weights.

Errors:

- `MESH_NOT_FOUND`: no asset at `skeletalMeshPath`.
- `INVALID_ASSET_TYPE`: existing path has another class; message names it and the `USkeletalMesh` path to use.
- `NO_LOD_MODELS`: no imported LODs, so nothing is readable (cooked-only/not-yet-built mesh); refusal, not empty success.
- `INVALID_LOD`: `lodIndex` is out of range.
- `PROFILE_NOT_FOUND`: supplied `profileName` does not exist.

### skeleton.normalize_weights

Renormalizes influences to sum to 1.0 and writes a **named ALTERNATE profile** (default `NormalizedWeights`). **Base skinning is not modified**; see *Skin weights* above.

Seeds from the existing target profile, or a copy of base skinning when absent. Returns `vertexCount`, `verticesNormalized`, `wrote: "alternateSkinWeightProfile"`, `baseSkinningModified: false`, and `verifyWith`.

### skeleton.prune_weights

Drops influences below `threshold`, renormalizes survivors, and writes a **named ALTERNATE profile** (default `PrunedWeights`). **Base skinning is not modified**; `baseSkinningModified: false`. If all influences would be pruned, the vertex stays intact—never zero-filled, since no influence renders at component origin. Returns `influencesRemoved`, `verticesAffected`, and shared disclosure fields; `threshold` must be `[0, 1)`, otherwise `INVALID_THRESHOLD`.

### skeleton.set_vertex_weights

Overwrites named vertices in a **named ALTERNATE profile** (default `CustomWeights`). **Base skinning is not modified**; `baseSkinningModified: false`.

Params of note:

- `weights` (array, required): `[{vertexIndex, influences:[{boneName | boneIndex, weight}]}]`.
  - `vertexIndex` is a **flat LOD render-vertex index**, the same space reported by `describe_skin_weights` and `audit_skin_weights`.
  - `boneName` wins when both are given. `boneIndex` is a **reference-skeleton** index from `skeleton.list_bones`, never a section-local slot; the handler translates it for storage.
  - `weight` must be `(0, 1]`. Zero is rejected because influences are largest-first and **zero-terminated**; a middle zero hides later influences.

Validation is pre-flight: **any** unusable entry rejects the whole call before writes, never half-applying. Unnamed vertices keep seeded influences. Returns `verticesModified`, `verticesRequested`, `vertexCount`, `boneIndexSpace: "referenceSkeleton"`, and shared disclosure fields.

Errors:

- `BONE_NOT_FOUND`: name absent from reference skeleton or `boneIndex` out of range; use `skeleton.list_bones`.
- `BONE_NOT_IN_SECTION`: bone exists but the vertex-owning section's `BoneMap` lacks it, so no slot can be added without re-chunking; section maps are in `skeleton.audit_skin_weights`.
- `INDEX_OUT_OF_RANGE`: `vertexIndex` exceeds LOD section coverage.
- `INVALID_ARGUMENT`: empty influences, >12 influences, repeated bone, or weight outside `(0, 1]`.
- `MISSING_PARAM` / `INVALID_PAYLOAD`: malformed entry.

### skeleton.copy_weights

Closest-vertex transfer from source `SkeletalMesh` to target, written into a **named ALTERNATE profile** (default `CopiedWeights`). **The target's base skinning is not modified**; `baseSkinningModified: false`.

Source and target must share a reference skeleton. Each influence is translated out of the *source* section's `BoneMap` and into the *target* section's; verbatim slot copying misidentifies bones on multi-section meshes. A bone the target section cannot carry is dropped and reported as `influencesDropped` with `transferWarning`, never substituted.

Re-running the same `profileName` overwrites it, not a second profile. Returns `verticesCopied`, `sourceVertices`, `influencesDropped`, `baseSkinningModified: false`, and shared disclosure fields. Errors: `NO_SOURCE_WEIGHTS`, `INVALID_LOD`, `SOURCE_NOT_FOUND`, `TARGET_NOT_FOUND`.

### skeleton.bind_cloth_to_skeletal_mesh

Binds or lists an **already-existing** `UClothingAsset`; it does **not** create one. There is **no MCP verb anywhere that creates a `UClothingAsset`**, and `skeleton.describe_mesh` exposes no cloth field. A freshly imported mesh with zero clothing assets therefore cannot receive Chaos Cloth end-to-end through this API (there is no create-cloth verb today).

Params:

- `skeletalMeshPath` (string, required): `USkeletalMesh` path.
- `clothAssetName` (string, optional): **already-existing** mesh asset to bind; missing returns `CLOTH_NOT_FOUND` (it does **not** create one). **Omitted** lists existing clothing assets.
- `meshLodIndex` / `sectionIndex` / `assetLodIndex` (number, optional, default `0`): bind target, used only with `clothAssetName`.

Errors:

- `MESH_NOT_FOUND`: no asset at `skeletalMeshPath`.
- `INVALID_ASSET_TYPE`: existing path has another class; message names it and the `USkeletalMesh` path to use.
- `CLOTH_NOT_FOUND`: named asset absent; this verb cannot create it.
- `BIND_FAILED`: the engine `BindToSkeletalMesh` call returned false.

### skeleton.assign_cloth_asset_to_mesh

Attaches an **already-existing** `UClothingAsset` named by `clothAssetName` to one `USkeletalMesh` section, or lists assets when omitted. Call once per `sectionIndex` to share one asset across sections. The asset must already be present on the mesh—this verb does **not** create a `UClothingAsset` (no MCP verb does).

Params:

- `skeletalMeshPath` (string, required): `USkeletalMesh` path.
- `clothAssetName` (string, optional): existing mesh asset to bind; missing returns `CLOTH_NOT_FOUND` (it does **not** create one). **Omitted** lists clothing assets.
- `sectionIndex` / `meshLodIndex` / `assetLodIndex` (number, optional, default `0`): bind target, used only with `clothAssetName`.

Returns (attach mode): the bind result for the named asset. Returns (list mode):

- `clothingAssets`: `{name}` rows (list mode); `count`: number of rows (list mode).

Errors:

- `MESH_NOT_FOUND`: no asset at `skeletalMeshPath`.
- `INVALID_ASSET_TYPE`: existing path has another class; message names it and the `USkeletalMesh` path to use.
- `CLOTH_NOT_FOUND`: named asset absent; this verb cannot create it.

### skeleton.set_preview_mesh

Sets the `USkeletalMesh` a `USkeleton` opens with. A mismatched mesh is refused rather than stored; see the namespace section above.

Params:

- `skeletonPath` (required): target `USkeleton`.
- `skeletalMeshPath` (required): preview mesh whose bound `Skeleton` is this skeleton or one it accepts as compatible.

Response:

- `previewMesh`: slot read **through the engine's fixup path**, not the assigned pointer; rejected references read empty and fail `VERIFICATION_FAILED`.
- `previewMeshApplied`: measured from readback; `previousPreviewMesh`: prior value (empty when unset).
- `changed`: false when already set; write is skipped like `FEditableSkeleton::SetPreviewMesh`, and `saveRequested` is false to avoid a needless dirty package.
- `saveRequested` / `saved` / `pendingFlush` / `markedForSave`: standard mark-dirty fields. This verb dirties but does **not** write `.uasset`; normally `saved` is false and `pendingFlush` true until flush.

Errors:

- `SKELETON_NOT_FOUND` / `SKELETAL_MESH_NOT_FOUND`: no asset at that path.
- `INVALID_ASSET_TYPE`: existing path has another class; message names actual/expected paths.
- `SKELETON_MISMATCH`: mesh has no bound skeleton or an incompatible one; names both skeletons.
- `VERIFICATION_FAILED`: the engine dropped the reference after it was assigned.

### skeleton.audit_skin_weights

A numeric verdict over `USkeletalMesh` **base skinning**—section soft-vertices used when no profile is active. `skeleton.describe_skin_weights` describes the same data; an absent profile does not prevent measurement.

No pose, world, or rendering is involved: it reads asset data, works on an unplaced mesh, and cannot dirty it.

Params:

- `skeletalMeshPath` (string, required): `USkeletalMesh` path; a `USkeleton` is rejected, not audited through its preview mesh.
- `lodIndex` (number, default `0`): audit one LOD because generated LODs can have different influence counts.
- `maxInfluences` (number, optional): override limit; omission resolves `BuildSettings.BoneInfluenceLimit`, then project `DefaultBoneInfluenceLimit`, then engine maximum 12. `limitSource` says which rule fired.
- `coincidentTolerance` (number, default `0.01`): bind-pose distance for same-point seam check.
- `maxSplitCoefficient` (number, default `0.001`): allowed weight disagreement for coincident vertices; multiply by bone travel for world-unit split.
- `checkReach` (bool, default `true`): run the only non-linear check.
- `maxReachDistance` (number, optional): gate when any **gated** influence exceeds this many median bone lengths; omit for report-only.
- `requireAllBonesInfluenced` (bool, default `false`): gate `boneCoverage`.
- `minInfluenceWeight` (number, default `0.01`): meaningful-weight cutoff; **one epsilon** shared by reach and `boneCoverage` counts.
- `reachVertexBudget` (number, default `50000`), `maxReported` (number, default `8`).

Returns `pass`, `failedChecks`, `unmeasuredChecks`, `checks[]`, `vertexCount`, `boneCount`, `sections[]`, `clothSectionCount`, `disabledSectionCount`, and `warnings[]`.

Each check has `status`: `pass`, `fail`, `reported` (measured, deliberately not voting), or `unmeasured`. **An unmeasured check counts against `pass` exactly as a failed one does**; “could not look” must not equal `pass: true`.

The checks:

- `zeroInfluence` — no influence means no bone matrix and component-origin rendering; correct count is 0.
- `weightSum` — sums other than 1.0, within the uint16 quantization band.
- `influenceCount` — counts over effective limit, full histogram, and `limitSource`.
- `coincidentSplit` — shared bind positions with disagreeing influences (UV/material seams); runs across sections. No duplicated vertices is `unmeasured`, not `pass`.
- `boneCoverage` — always-emitted `bonesWithNoInfluence` (including empty), per-bone `{bone, boneIndex, influencedVertices, rigidVertices}`, and LOD `rigidVertexCount`; same block/code as `skeleton.describe_skin_weights`.
- `influenceReach` — bind-pose distance to each bone segment normalized by median bone length, as percentiles and named offenders.

**`boneCoverage` reports rather than gates by default, deliberately.** IK targets, attachment bones, twist drivers, and motion-only roots may have no weighted vertices. The check still finds an animation driving a bone that owns no geometry. Pass `requireAllBonesInfluenced: true` only when the rig should have none.

**`influenceReach` reports rather than gates by default** because thresholds vary by skeleton. Calibrate on a known-good mesh with the same skeleton: use `normalizedDistance.p999`, then pass roughly twice it as `maxReachDistance`.

**A deliberately rigid part is not a reach defect; it is reported separately.** A prop, weapon, eyeball, or attachment cape may be far from its one bone; a leaf-bone segment is a *point* and can be farther still. Such an influence enters `rigidBind` only when **both** hold: exactly one influence is at/above `minInfluenceWeight`, **and** no bone is strictly closer than the named bone. Both prevent excusing a rigid bind to the wrong bone or a blended influence.

`influenceReach` therefore reports:

- `influencesConsidered`, `gatedInfluences`, and `rigidBind.influences`, which **partition** all influences.
- Gated-only `normalizedDistance` (`p50`/`p90`/`p99`/`p999`/`max`) and `offenders[]`, whose rows carry `vertexIndex`, `bone`, `weight`, `distance`, `normalizedDistance`, `closerBones`. Large distance with `closerBones: 0` can be legitimate; `closerBones: 12` signals a bind error.
- `rigidBind`: `influences`, `vertices`, its percentiles, `offenders[]`, and `reason`; measured/published, never silently excluded.
- `medianBoneLength`, `sampledVertices` / `totalVertices` / `exhaustive`; above `reachVertexBudget`, vertices are uniformly strided and counts reveal sampling.

When all measured influences are rigid nearest-bone binds, `gatedInfluences` is 0: a supplied threshold has nothing to judge, but this is a **measured** pass marked vacuous, not `unmeasured`. `unmeasured` means nothing could be measured—no bone has length (single-bone/all joints coincident) or no influence meets `minInfluenceWeight`.

What this verb **cannot** see: visual smoothness, volume preservation, or joint falloff; named profiles (use `skeleton.describe_skin_weights`, and note they can disagree); or posed-only candy-wrapper collapse/self-intersection. Cloth sections are counted/warned separately because their vertices are simulated, not skinned.

Errors:

- `MESH_NOT_FOUND`: `skeletalMeshPath` did not load.
- `INVALID_ASSET_TYPE`: path is not `USkeletalMesh`. For `USkeleton`, the message names `USkeleton`, its preview path or `(none)`, and recovery: use that mesh or configure one with `skeleton.set_preview_mesh`.
- `NO_LOD_MODELS`: no imported LODs, so base skinning is unreadable; refusal, not a clean zero-finding audit.
- `INVALID_LOD`: `lodIndex` out of range.
- `INVALID_ARGUMENT`: tolerance, weight, budget, or threshold is illegal, including `maxReachDistance: 0` (rejected, not report-only; omit it for report-only).
- `MISSING_PARAM`: no `skeletalMeshPath`.

### skeleton.create_skeleton

`path` (alias `skeletonPath`) is one whole asset path and must be a valid long package name under a mounted root: `..`, `//`, `\`, a trailing slash, a missing leading slash and an unmounted root are all refused `INVALID_PATH` before anything is created. The package name handed to `CreatePackage` is then re-checked against `FPackageName::IsValidLongPackageName` on the line that uses it, because `CreatePackage` logs a `//` or an empty name at **Fatal** — not compiled out in any configuration, so it ends the editor process rather than failing the call. `rootBoneName` defaults to `Root`.

The asset is marked dirty, not written: the response's save report says so. Add bones with `skeleton.add_bone`, sockets with `skeleton.create_socket`.

### skeleton.create_physics_asset

`skeletalMeshPath` (alias `skeletonPath`) accepts either a `USkeletalMesh` or a bare authored `USkeleton`; the mesh generates capsule bodies from the mesh, the skeleton one body per reference-pose bone span longer than `minBoneLength` (default 5 cm).

**`outputPath` is one whole asset path and is validated before the source asset is resolved.** A path that would compose a package name `CreatePackage` cannot survive — one containing `//`, one resolving to empty (`/`), one with no leading slash, one under an unmounted root — is refused `INVALID_ARGUMENT` quoting the engine's reason and the composed path. The order matters to callers: a bad `outputPath` is reported as an argument error even when `skeletalMeshPath` also names nothing. `CreatePackage` logs `//` and the empty name at **Fatal**, which is not compiled out in any configuration and ends the editor process, so this cannot be checked after the fact — an `if (!Package)` branch is never reached. Omitted, `outputPath` defaults to `<SourcePath>_PhysicsAsset` beside the source, which is validated the same way.

Creation is factory-free by design (board `B-physics-asset-factory-modal-hang`): `UPhysicsAssetFactory` opens an interactive body-generation modal that wedges the game thread in a non-unattended editor.

`save` defaults to `true`. A requested save writes the new PhysicsAsset through the measured disk-save path; `save:false` registers and marks it dirty for a later flush. The response reports `saveRequested`, `saved`, `savedToDisk`, `pendingFlush`, `saveState`, `saveDetail`, and `sizeBytes`, so a resident registry entry is not mistaken for a durable `.uasset`. On the mesh-backed path, assigning the generated asset also changes the SkeletalMesh; `skeletalMeshSave` carries the same save report for that package.
