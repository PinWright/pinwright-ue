# animation

Skeletal animation convenience helpers for Animation Blueprints, AnimSequences, AnimMontages, Blend Spaces, IK, retargeting, and runtime montage playback. Use this namespace when one operation can create, update, or play a skeletal animation asset or runtime montage state; reach for `call("animation.authoring")` for fine-grained graph and asset authoring, or `call("anim")` for the two text surfaces: AGIR graph compile/decompile and `.pwanim` validation/sequence compilation.

## Choose the layer

- **`animation.*`** provides one-shot creation and runtime helpers (`create_animation_bp`, `create_blend_space`, `create_state_machine`, `create_animation_asset`, `setup_retargeting`, `retarget_animations`, `play_montage`, `add_notify`, `cleanup`). Use `animation.describe_sequence` for clip length and frame count.
- **`call("animation.authoring")`** provides fine-grained asset and graph work: tracks, montage sections/notifies/timing, Control Rig, IK and retargeting, blend-space samples, anim-graph values, transition rules, sync markers, root motion, and dump-parity sequence readers.

Workflow gotcha: anim authoring almost always needs the target Skeleton already to exist. Create the skeleton (or import an SK that creates one) before reaching into either layer.

## Which anim-BP creator?

Two methods create a `UAnimBlueprint`; choose by how you supply the Skeleton and how much save-path control you need:

- **`animation.create_animation_bp`** is the recommended **one-shot creator**. It takes **`meshPath`** (auto-resolves the Skeleton from a SkeletalMesh) or `skeletonPath`, plus optional `parentClass`; `FAssetToolsModule::CreateAsset` safely de-dupes a name collision without overwrite.
- **`animation.authoring.create_anim_blueprint`** gives save-path and parent-class control, but requires explicit `skeletonPath` (no `meshPath` auto-resolve) and rejects an in-session collision with `ASSET_EXISTS`.

Thus, "create an ABP and derive the skeleton from a SkeletalMesh" wants `animation.create_animation_bp` with `meshPath`; the authoring peer rejects it for missing `skeletonPath`.

## Read a sequence's length / info

To read a `UAnimSequence` (its length, frame count, frame rate, additive settings, etc.) the go-to reader is **`animation.describe_sequence`** - the top-level dump-parity live read that returns the full `anim_sequence.json` shape (delegates to `AnimSequenceDumpBuilder`). The verb is `describe_sequence`, not `get_animation_info`: there is no `animation.get_animation_info`, so that intuitive guess returns `[UNKNOWN_ACTION]`.

The `get_animation_info` verb lives under the *sub*-namespace as [`animation.authoring.get_animation_info`](animation.authoring.md) - the thinner authoring-layer introspection dual you call before mutating an asset to confirm its shape. For a single slice (curves / notifies / sync markers) use the sub-array readers under `animation.authoring` (`list_curves` / `list_notifies` / `list_sync_markers`).

## Measure authored motion

`animation.measure_motion` reads an AnimSequence's authored data model and returns numeric motion checks without a component, world, playhead, or render pass. The response always includes `moving`, `loopSeam`, and `jitter` checks. Add `footBones` to include the `locomotion` check; omit it when only general motion, seam, and curve measurements are needed.

The measured jitter fields are named `frameFraction` (the fraction of evaluated interior samples classified as jitter) and `maxRatio` (the largest normalized second-difference ratio). `maxJitterFraction` is an optional threshold, not a measured field: without it, a measurable jitter check has status `reported`; with it, the same `frameFraction` produces status `pass` or `fail` and the check echoes `maxJitterFraction`.

The `footBones` response is conditional on both track resolution and stance measurement:

- Without `expectedGroundSpeed`, each measurable foot row includes `impliedGroundSpeed` and `stridePerCycle`, while the `locomotion` check has status `reported`. `slideFraction`, each foot's `pass`, the overall locomotion verdict, and `suggestedPlayRate` are absent.
- With `expectedGroundSpeed`, each measurable foot row includes `slideFraction` and a per-foot `pass`; the `locomotion` check is `pass` or `fail` only when at least one resolved foot track has a usable stance. A failed foot also includes `suggestedPlayRate`, the play rate that would reconcile the measured speed. The check includes `expectedGroundSpeed` and `groundSpeedTolerance`.
- If no resolved foot track produces a usable stance window, the `locomotion` check is instead `unmeasured`, with `measuredFeet: 0` and a `reason`; each resolved-but-unmeasurable foot row has `measured: false` and no `impliedGroundSpeed`, `slideFraction`, `pass`, or `suggestedPlayRate`. Supplying `expectedGroundSpeed` still echoes `expectedGroundSpeed` and `groundSpeedTolerance` on the unmeasured check, but does not manufacture a verdict or foot-level gate fields.

If `footBones` is omitted, no `locomotion` check is emitted even when `expectedGroundSpeed` is supplied. A failed or unmeasured check makes the top-level `pass` false; a `reported` check is a measurement without that optional threshold verdict.

## Will these curves move the morphs?

`animation.check_morph_curves` answers it per curve for one `skeletalMeshPath`. A curve drives a morph only when its name matches a morph on the mesh **and** its curve metadata carries the MorphTarget flag; the mesh's own curve metadata overrides the Skeleton's for any curve it flags, exactly as the engine's bone container merges them. A name match alone is the common false positive: it reads `drives: false`, `flagSource: "none"` and an `unflagged_morph_curve` error. Each asset row also lists `morphsWithoutCurve`. It is an audit (`pass`, `passRule`, `failOn`), so a missing or wrong-type asset is an `unrunnable` row, never a silent skip.

## Read a live pose

`animation.get_live_pose` reads the pose a placed skeletal mesh component currently shows (PIE or editor world) in a required `space` (`world`, `component`, `local`), plus optional AnimInstance state through `include` (`stateMachines`, `montage`, `curves`). It never ticks the component, so check `poseBuffer`: `tickedThisFrame` false or an unchanged `revision` across two calls means the pose has not updated. `skeleton.get_bone_transform` reads the reference pose instead.

## `setup_retargeting` is a skeleton-swap copier

`animation.setup_retargeting` duplicates `UAnimSequence` assets and assigns the requested target `USkeleton` to each duplicate. It does not run IK Retargeter evaluation or remap bone tracks. Every successful response reports `retargeted: false`; when at least one output is produced, `duplicatedWithSkeletonSwap` is true and the paths are under `duplicatedAssets`. The verb never returns `retargetedAssets`.

Use this verb only when the source tracks are already compatible with the target Skeleton. For a real IK export between different skeletons use `animation.retarget_animations` (below); this verb accepts none of its source-mesh, target-mesh or retargeter inputs.

When `savePath` is supplied it must normalize to a writable long package directory or the call fails with `INVALID_PATH` before loading either Skeleton or creating any asset. Omit it to place each duplicate beside its source. Every produced copy is saved before success is reported (`saved: true`, `diskPersistenceGuaranteed: true`). With `overwrite: true`, the replacement is first validated and saved in a unique staging package, then published while the old destination remains available for rollback; the old object is deleted only after the new destination is durable. A staging or final-save failure returns `SAVE_FAILED`, and a transactional rename failure returns `RENAME_FAILED`; both failure shapes report whether the original object and file were restored.

## Retarget clips with an IK Retargeter

`animation.retarget_animations` runs the engine's IK Retargeter batch export (`UIKRetargetBatchOperation::RunRetarget`) over `AnimSequence` assets: `retargeter`, `sourceMesh`, `targetMesh`, `assets[]` and `outputPath` are required, and each output is named `prefix + sourceName + suffix` in `outputPath`. Build the inputs with `animation.authoring.create_ik_rig` (one rig per mesh, each with matching retarget chains via `add_ik_chain`) and `create_ik_retargeter` (`sourceIKRigPath` / `targetIKRigPath`).

It refuses before writing anything: `RETARGETER_NO_OPS` when the retargeter's op stack is empty on UE 5.6+ (a retargeter from `create_ik_retargeter` starts empty; pass `seedDefaultOps: true` to add the engine's default stack, which also auto-maps same-named chains and mutates the retargeter), `RETARGET_CHAINS_UNMAPPED` listing target chains no op maps to a source chain (`requireCompleteMapping: false` accepts them), `SKELETON_MISMATCH` for clips whose Skeleton is not compatible with `sourceMesh`'s, `INVALID_ASSET_TYPE` for anything that is not an `AnimSequence`, and `ASSET_EXISTS` when an output name is taken (there is no overwrite).

Every output is read back: its Skeleton must be `targetMesh`'s, its frame count must equal the source's, and it must have bone tracks. The response lists them in `created[]` (`path`, `source`, `skeleton`, `frames`, `boneTracks`), everything that appeared in `outputPath` in `allCreatedAssets`, plus `mappingComplete`, `unmappedTargetChains` and `seededDefaultOps`; outputs are saved and reported through `saved` / `pendingFlush`. If any output fails that read-back, the verb deletes every asset it created and returns `RETARGET_INCOMPLETE` with `failures`, `removedOutputs` and `outputsRemaining`. The read-back cannot tell a well-retargeted clip from a badly configured one (wrong retarget pose, poor chain choice): look at the result with `render.capture_animation_preview`.

## PIE safety for asset mutations

`animation.cleanup`, `animation.create_animation_asset`, `animation.setup_retargeting`, and `animation.retarget_animations` refuse with `PIE_ACTIVE` before any asset deletion, folder creation, duplication, or replacement work when Play In Editor is active. Stop PIE and retry; read-only asset resolution remains available through the shared registry/object resolver.

## Cross-cluster overlap

For adjacent visual content use `call("niagara")`; for cinematics that drive animation tracks use `call("sequencer")`.

## See also

- [`animation.authoring`](animation.authoring.md) for fine-grained animation asset writers and dump-parity live sequence readers.
- [`anim`](anim.md) for the two text families: AGIR import/export of `UAnimBlueprint` graphs and `.pwanim` validation/compilation for `UAnimSequence` assets.
- [`sequencer`](sequencer.md) for cinematic tracks that drive animation assets.

### animation.create_state_machine

Each `states[]` element has the closed shape `{name, isEntry}`. The convenience verb creates and names state graph shells and chooses the entry state; it does not bind an animation asset or mark an exit state. `animation` and `isExit` are rejected with `UNKNOWN_NESTED_PARAMS` instead of being accepted and discarded. Populate each state graph afterward with the fine-grained `animation.authoring` graph verbs.

The request is atomic: transition endpoints are validated before graph creation. If the verb reports a state/transition construction or postcondition error after mutation begins, it restores the pre-request graph topology and package dirty state. Entry-node wiring remains best-effort for compatibility and does not turn an otherwise-created machine into an error.

### animation.check_morph_curves

AnimMontage curves include the curves of every segment's sequence, because the slot node evaluates them. PoseAsset curves are its stored curve names. `maxLod` is the Skeleton metadata MaxLOD (255 = every LOD; above it the curve is filtered out), or null when the Skeleton has no metadata for the curve. `flagged_curve_without_morph` is a warning, since a Skeleton shared by several meshes legitimately flags morphs only some meshes carry.

### animation.get_live_pose

Bones come from the component-space read buffer, the same one `GetSocketTransform` resolves bone names through; `local` is relative to the parent bone (the root is component-relative). Omitting `bones` returns every bone up to 1000 (`truncated`, `totalBones`). An actor with several SkeletalMeshComponents needs `component` (`TARGET_AMBIGUOUS` lists them); a leader-pose follower has no pose of its own and is refused with `INVALID_STATE`. `include` sections are null when the component has no AnimInstance; `stateMachines` covers the main instance only, not linked or post-process instances. `world: "pie"` with no session returns `PIE_NOT_ACTIVE`.
