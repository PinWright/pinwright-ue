---
type: reference
summary: "The supported engine range (UE 5.3-5.8, Win64 and Linux), the per-symbol version guards that hold it together with call sites, first-available engine version and older-engine substitute, and the engine crash sites and deprecation traps found on the way."
date: 2026-09-16
tags: [engine-compat, version-guards, build, packaging, honesty, crash-guards]
---

# Engine version support

**UE 5.3 through 5.8, Win64 and Linux.** Every one of the six engine versions is green end to end —
clean compile, full automation suite, stdio-connector unit tests on that version's bundled Python,
and a clean Rocket packaging gate — measured by the `mcp-version-matrix` loop between 2026-09-10 and
2026-09-12 (`git log --grep=version-matrix`: 5.3 `96f9f7ef`, 5.4 `d5dfb11b`, 5.5 `ae04608e`,
5.6 `0b39d4f7`, 5.7 `ac62b0fe`, 5.8 `9b5d37fb`). Linux joined at `aa920932` (2026-09-12), which
carries the clang fixes and the `PlatformAllowList` of `["Win64", "Linux"]` on every module.

**The backport is done; the deferral is cancelled.** This file no longer tracks debt — it tracks the
guards that pay for the range. Every symbol that does not exist across the whole range now sits
behind a `UE_VERSION_NEWER_THAN_OR_EQUAL` branch with a named substitute or an honest
`Ctx.SendUnsupportedEngineVersion` rejection, and the table below records where each guard lives so
an engine bump can retire one without re-deriving the survey.

**A new unguarded 5.8-only symbol is a regression, not an accepted cost.** Guard it at the call site
and add its row here in the same change.

Guard macros, reflection escapes for unexported types, the existing compat shims, and
`Ctx.SendUnsupportedEngineVersion` are in `CLAUDE.md` → Conventions → *UE version compat*. The
design rule that produced this file is `rpc-design.md` §14.

## Known blockers

Every row is a symbol that does not exist across the whole supported range. **All 20 are resolved:
re-verified 2026-09-16 by grepping each call site in the current tree, every one of them guarded by
`#if UE_VERSION_NEWER_THAN_OR_EQUAL(...)` with the substitute or the rejection on the `#else`.** The
final column names the guard. Line numbers are 5.8 headers unless stated; the first-available column
was verified by grepping all six engine trees, not inferred.

| Engine symbol | Plugin call site | Verb(s) affected | First engine | Older-engine substitute | Status |
|---|---|---|---|---|---|
| `ALandscapeProxy::RetrieveAllLandscapeMaterials` (`LandscapeProxy.h:1319`) | `Source/PinWright/Private/Handlers/Material/MaterialLandscapeConsumers.h:190` (note `:182-188`) | `material.authoring.compile_material` | **5.8** | None. Absent from the entire Landscape module on 5.3-5.7. A hand-rolled sweep of `LandscapeMaterial` + `LandscapeHoleMaterial` + per-component overrides is the only option, and the comment at `:178` records why the engine call was preferred: a hand sweep drifts from the engine's own definition of "materials this landscape uses". | Resolved: guard at `Handlers/Material/MaterialLandscapeConsumers.h:77`. |
| `UWaterBodyRiverComponent::{Get,Set}River{Width,Depth}AtSplineInputKey` (`WaterBodyRiverComponent.h:45,48,51,54`) | `Handlers/Water/WaterHandler.cpp:585,589,597,598` (note `:526-534`) | `water.set_river_width_at_spline_point`, `water.set_river_depth_at_spline_point` | **5.6** | Write `UWaterSplineMetadata::RiverWidth` / `Depth` directly and re-synchronize: both are public `UPROPERTY` `FInterpCurveFloat` (`WaterSplineMetadata.h:81,89` on 5.5) and `UWaterSplineComponent::SynchronizeWaterProperties()` is public (`WaterSplineComponent.h:69` on 5.5). Untested. | Resolved: guard at `Handlers/Water/WaterHandler.cpp:585`. |
| `EGeometryScriptBoneHierarchyMismatchHandling` + `FGeometryScriptCopyMeshToAssetOptions::BoneHierarchyMismatchHandling` (`MeshAssetFunctions.h:21,117`) | `Source/PinWrightGeometry/Private/Handlers/Geometry/SkeletalMeshAssetIOHandler.cpp:355-369,982-983,1094` | `geometry.convert_to_skeletal_mesh` | **5.6** | 5.5: the single bool `bRemapBoneIndicesToMatchAsset` (`:79`) — covers the remap mode only, no `CreateNewReferenceSkeleton` equivalent. 5.3/5.4: the options struct carries no bone-remap control at all; the engine always behaves as `DoNothing`. | Resolved: guard at `Handlers/Geometry/SkeletalMeshAssetIOHandler.cpp:297`. |
| `UGeometryScriptLibrary_MeshBoneWeightFunctions::CopyBonesFromSkeleton` + `FGeometryScriptCopyBonesFromMeshOptions` (`MeshBoneWeightFunctions.h:476,237`) | `SkeletalMeshAssetIOHandler.cpp:868` | `geometry.bind_skin_weights` | **5.5** | None that is drop-in. 5.3/5.4 have only `CopyBonesFromMesh` (`:336`), which takes a source `UDynamicMesh` rather than a `USkeleton`. `ComputeSmoothBoneWeights` still takes the `USkeleton` there, so the bind itself would work — the mesh would simply carry no bone attributes. | Resolved: guard at `Handlers/Geometry/GeometryUtils.cpp:169`. |
| `FGeometryScriptCopyMeshFromAssetOptions::bUseBuildScale` (`MeshAssetFunctions.h:36`) | `SkeletalMeshAssetIOHandler.cpp:673` | `geometry.create_from_skeletal_mesh` | **5.4** | None. The field does not exist on 5.3, so the `useBuildScale` parameter cannot be honoured there and the verb would have to reject or ignore it explicitly. | Resolved: guard at `Handlers/Geometry/MeshAssetIOHandler.cpp:368`. |
| `USceneCaptureComponent2D::bUpdateOrthoPlanes` and `::bUseCameraHeightAsViewTarget` (`SceneCaptureComponent2D.h:68,72`) | `Source/PinWright/Private/Handlers/Render/OrthoTileCaptureUtils.cpp` (`FOrthoTileCapture` constructor) | `render.capture_ortho_tiles` | **5.4** | Delete both lines. They are set to their shipped defaults (`false`) on purpose - an archetype could change a default under us - and 5.3's scene capture has no ortho-plane correction at all, so omitting them gives the same rendering. Untested on 5.3. | Resolved: guard at `Handlers/Render/OrthoTileCaptureUtils.cpp:466`. |
| `Audio::FLKFSAnalyzer` / `FLKFSAnalyzerSettings` / `FLKFSAnalyzerResults` / `FLKFSResult::GatedLoudness` (`DSP/LKFSAnalyzer.h:119,16,86,79`) | `Source/PinWright/Private/AudioGen/PwAudioFeatures.cpp` (`PwComputeLoudness`) — the only direct user; `PwSynthDsp.cpp`'s `PwSynthDspInternal::MeasureIntegratedLufs` reaches the analyzer only through it | `audio.synth.*` render path — LUFS `normalize.mode` only; peak normalization is unaffected. Plus every analysis verb that reports loudness through `PwComputeLoudness` (`FPwLoudnessResult`'s five LUFS/LU fields; its `PeakDb`/`RmsDb` are computed directly and stay portable). Onset and pitch extraction live in the same file and are **not** affected: `PwDetectOnsets` is hand-written and `PwEstimatePitch` is built on `Audio::FBlockCorrelator` (`DSP/BlockCorrelator.h`), which is core SignalProcessing on 5.3-5.8 — the engine's own `FOnsetStrengthAnalyzer` / `FYINPitchDetector` were deliberately not used because they are AudioSynesthesia-plugin-only on every version | **5.8** | The whole header is new in 5.8's `Runtime/SignalProcessing`; verified absent from 5.3-5.7 (`Public/DSP/LKFSAnalyzer.h` does not exist there). **5.7:** the AudioSynesthesia plugin's own copy — `Engine/Plugins/Runtime/AudioSynesthesia/Source/AudioSynesthesiaCore/Public/LKFSFactory.h` + `LKFSNRTFactory.h`, which appeared in 5.7 — reachable only by enabling that plugin and going through its NRT analyzer surface rather than a direct class. **5.3-5.6:** nothing; AudioSynesthesiaCore carries no LKFS at all, so a hand-written ITU-R BS.1770 (K-weighting biquads + 400 ms gated blocks) is the only option. Rejecting the LUFS mode with `Ctx.SendUnsupportedEngineVersion` is the cheap interim: peak normalization stays portable and LUFS is one enumerator of one optional block. On the analysis side the equivalent interim is cheaper still — `PwComputeLoudness` already returns a structured error rather than a number, so an analysis verb can drop the five LUFS fields and keep reporting `PeakDb`/`RmsDb`, onsets and pitch. | Resolved: guard at `AudioGen/PwAudioFeatures.cpp:17,225`. |
| `UGeometryScriptLibrary_MeshUVFunctions::LayoutMeshUVs` + `FGeometryScriptLayoutUVsOptions` (`MeshUVFunctions.h:558`) | `Source/PinWrightGeometry/Private/Model/PwModelCompiler.cpp:763-766`, the `mode=layout` branch of `FCompiler::ApplyUVOp` (`:716`) | `model.compile`, `model.validate` | **5.5** | `RepackMeshUVs` + `FGeometryScriptRepackUVsOptions` (`MeshUVFunctions.h:283` on 5.3, `:547` on 5.8), present on all six and carrying `TargetImageWidth` for the `texture_resolution` parameter. It repacks the islands the mesh already has rather than laying out from scratch, so it is a substitute for `mode=layout` only on a mesh that has been unwrapped first; otherwise reject `mode=layout` by name on 5.3/5.4. | Resolved: guard at `Handlers/Geometry/GeometryOps_Modeling.cpp:3144`. |
| `MovieSceneHelpers::CreateTransientSharedPlaybackState` (`MovieSceneCommonHelpers.h:295`) | `Source/PinWright/Private/Handlers/Sequencer/SequencerBindingUtils.h` (`ResolveBoundObjects`) | `sequencer.add_actor`, `sequencer.add_actors` (only when `componentName` is passed) | **5.5** | None on 5.3/5.4: `UE::MovieScene::FSharedPlaybackState` first appears at `EntitySystem/MovieSceneSharedPlaybackState.h` on 5.4 and has no factory until 5.5. Substitute the reverse lookup `UMovieSceneSequence::FindBindingFromObject(Object, Context)` (present on all six, deprecated 5.5) - it answers object -> GUID rather than GUID -> object, which is sufficient here because the verb already knows the component it asked to bind. Untested. | Resolved: guard at `Handlers/Sequencer/SequencerBindingUtils.h:298`. |
| `MovieSceneHelpers::GetBoundObjects` (`MovieSceneCommonHelpers.h:255`) | `Source/PinWright/Private/Handlers/Sequencer/SequencerBindingUtils.h` (`ResolveBoundObjects`) | `sequencer.add_actor`, `sequencer.add_actors` (only when `componentName` is passed) | **5.7** | Its own sibling `GetSingleBoundObject` reaches back no further (5.7, and already deprecated). On 5.3-5.6 call `UMovieSceneSequence::LocateBoundObjects(Guid, FResolveParams(Context), OutObjects)` directly, computing `Context` the way `MovieSceneHelpers::GetResolutionContext` does - the parent binding's resolved object when the possessable has a parent and `AreParentContextsSignificant()`, the playback context otherwise - or fall back to the `FindBindingFromObject` substitute above. Untested. | Resolved: guard at `Handlers/Sequencer/SequencerBindingUtils.h:301`. |
| `EGeometryScriptPerVertexNormalSource` + `FGeometryScriptPerlinNoiseOptions::NormalSource` (`MeshDeformFunctions.h:133,154`) | `Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryOps_Modeling.cpp` (`NoiseDeform`, the `NoiseOptions.NormalSource` assignment) | `geometry.noise_deform` (`normalSource`), `model.compile` / `model.validate` (`noise_deform normal_source=`) | **5.8** | Drop the assignment. The enum and the field are both absent from 5.3-5.7 (verified by grepping `GeometryScriptingCore/Public/` in all six installed trees; 5.3 has the plugin under `Plugins/Experimental/` rather than `Plugins/Runtime/`), and the behaviour those engines give unconditionally is `Computed`, which is the field's 5.8 default and the op's default - so omitting it costs only the `average_from_overlay` mode. Reject that one value with `Ctx.SendUnsupportedEngineVersion`. **The sibling fields are NOT blockers:** `FGeometryScriptPerlinNoiseLayerOptions::RandomSeed` and `::FrequencyShift` (`:97,100` on 5.3-5.5, `:99,102` on 5.6-5.8) exist on all six, so `seed` and `frequency_shift` are portable as-is. **Separate hazard on the same op:** `ApplyPerlinNoiseToMesh` is deprecated at 5.7 in favour of `ApplyPerlinNoiseToMesh2` because it *incorrectly squared the frequency parameter* (`MeshDeformFunctions.h:330` on 5.7), and the existing `#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)` branch calls the old one on 5.3-5.6 - so one `frequency=` value does not mean the same thing either side of that line, and a `.pwmodel` tuned on 5.8 will look wrong on 5.6 with no error. Decide deliberately whether to compensate or to document it. | Resolved: guard at `Handlers/Geometry/GeometryOps_Modeling.cpp:1992`. |
| `TDynamicMeshOverlay::IsTriangleStorageValid` (`DynamicMeshOverlay.h:380`) | `Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryUtils.cpp` (`MeshHasUVsOnEveryTriangle`) | `geometry.shell`, `model.compile` / `model.validate` (`shell`) | **5.8** | Drop the call and keep the per-triangle `IsSetTriangle` sweep that follows it. The helper is a pure inline predicate - `ParentMesh != nullptr && ElementTriangles.Num() >= 3 * ParentMesh->MaxTriangleID()` - but `ElementTriangles` is `protected` on 5.3-5.7 with no public accessor, so it cannot be reimplemented from outside the class. Dropping it costs only the narrow case the helper names (an overlay whose per-triangle storage was never grown alongside the parent), which no `.pwmodel` op is known to produce; the guard it protects is itself a guard. Verified absent from 5.3-5.7 by grepping `DynamicMeshOverlay.h` in all six installed trees. On 5.8 the engine itself calls it in exactly one FILE, at three sites - `MeshBakeFunctions.cpp:588`, `:600`, `:610`, guarding the UV, normal and color overlays before a bake reads them - and nowhere else; every other engine caller of `IsSetTriangle`/`GetTri*`, including both `FDynamicMeshToMeshDescription` paths and `DynamicMeshMikkTWrapper`, calls them cold. | Resolved: guard at `Handlers/Geometry/GeometryUtils.cpp:794`. |
| `FGeometryScriptSimplifyMeshOptions::{RegularizeWeight, QuadricVariant, NormalAttributeWeight, TangentAttributeWeight, ColorAttributeWeight, TexCoordAttributeWeight, ScaleCorrection}` + `EGeometryScriptMeshSimplificationQuadricVariant` + the `EGeometryScriptRemoveMeshSimplificationType::AttributeAwareV2` enumerator (`MeshSimplifyFunctions.h:60,121-155,86-90`) | `Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryOps_Modeling.cpp` (`SimplifyMesh`, the `SimplifyOptions.*` assignment block) | `geometry.simplify_mesh` (`regularizeWeight`, `quadricVariant`, the four `*AttributeWeight`, `scaleCorrection`, and `method=attribute_aware_v2`), `model.compile` / `model.validate` (the same names on `simplify_mesh`) | **5.8** | Drop the seven assignments and reject `method=attribute_aware_v2` with `Ctx.SendUnsupportedEngineVersion`. On 5.3-5.7 the struct is exactly seven fields - `Method`, `bAllowSeamCollapse`, `bAllowSeamSmoothing`, `bAllowSeamSplits`, `bPreserveVertexPositions`, `bRetainQuadricMemory`, `bAutoCompact` - all of which this op sets and all of which are portable, so the backport keeps the op's whole pre-5.8 vocabulary and loses only the attribute-aware tuning. **`Method`'s default is NOT a blocker in either direction:** 5.3 already declares `Method = AttributeAware` (`MeshSimplifyFunctions.h:57` on 5.3), so removing this op's undocumented `StandardQEM` override is correct on every version in the range. Verified by reading the struct in all six installed trees. | Resolved: guard at `Handlers/Geometry/GeometryOps_Modeling.cpp:303,1057`. |
| `FGeometryScriptMeshBooleanOptions::bAllowEmptyResult` (`MeshBooleanFunctions.h:53`) | `Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryOps_Boolean.cpp` (`Boolean` and `Trim`, the `BoolOptions.bAllowEmptyResult` assignment) | `geometry.boolean_union` / `boolean_subtract` / `boolean_intersection` / `difference` / `boolean_trim` (`allowEmptyResult`), `model.compile` / `model.validate` (`allow_empty_result=`) | **5.4** | Drop the assignment and reject `allowEmptyResult=true` by name on 5.3. The 5.3 engine behaves as `false` unconditionally - an empty result leaves the target mesh untouched - which is the field's default, so `false` costs nothing. Verified absent from 5.3's `FGeometryScriptMeshBooleanOptions` and present from 5.4 on, by grepping all six installed trees. | Resolved: guard at `Handlers/Geometry/GeometryOps_Boolean.cpp:232,380`. |
| `FGeometryScriptMeshBooleanOptions::OutputTransformSpace` + `EGeometryScriptBooleanOutputSpace` (`MeshBooleanFunctions.h:27-37,57`) | `Source/PinWrightGeometry/Private/Handlers/Geometry/GeometryOps_Boolean.cpp` (`Boolean` and `Trim`, the `BoolOptions.OutputTransformSpace` assignment) and `GeometryOps_Boolean.h` (`FBooleanParams` / `FTrimParams` field type) | `geometry.boolean_union` / `boolean_subtract` / `boolean_intersection` / `difference` / `boolean_trim` (`outputTransformSpace`). **Not** `model.compile` / `model.validate`: the compiler passes identity for both operand transforms, so the field is not published there | **5.6** | Drop the assignment, spell the field as a local enum in `FBooleanParams` rather than the engine one, and reject any `outputTransformSpace` other than `target` with `Ctx.SendUnsupportedEngineVersion`. 5.3-5.5 hard-code the target-space behaviour (`MeshBoolean.bPutResultInInputSpace` plus an unconditional `ApplyTransformInverse` by the target transform), which is exactly what `target` means, so the default is free and only `tool` / `shared` are lost. Note the field type is the reason this row also touches the header: `FBooleanParams` names `EGeometryScriptBooleanOutputSpace` directly, so a 5.5 build fails to compile the STRUCT, not merely the call. | Resolved: guard at `Handlers/Geometry/GeometryOps_Boolean.cpp:244`. |
| `FMaterialInstanceBasePropertyOverrides::{bOverride_CompatibleWithLumenCardSharing, bCompatibleWithLumenCardSharing}` (`MaterialInstanceBasePropertyOverrides.h:73,116`) + `UMaterialInstance::IsCompatibleWithLumenCardSharing` (`MaterialInstance.h:1007`) | `Source/PinWright/Private/Handlers/Material/MaterialAuthoringHandler.cpp`, the `compatibleWithLumenCardSharing` entry of `GetInstanceBoolOverrideSlots` | `material.authoring.set_material_instance_base_property_overrides` (`compatibleWithLumenCardSharing` only) | **5.6** | Drop that one table entry and reject the param by name with `Ctx.SendUnsupportedEngineVersion`. The slot table is data, so removing an entry costs exactly one wire param and nothing else in the verb. Verified absent from 5.3-5.5 by grepping the override struct and `Materials/MaterialInstance.h` in all six installed trees. | Resolved: guard at `Handlers/Material/MaterialAuthoringHandler.cpp:2818`. |
| `FMaterialInstanceBasePropertyOverrides::{bOverride_bEnableDisplacementFade, bEnableDisplacementFade}` (`MaterialInstanceBasePropertyOverrides.h:61,111`) + `UMaterialInstance::IsDisplacementFadeEnabled` (`MaterialInstance.h:982`) | Same file, the `enableDisplacementFade` entry of `GetInstanceBoolOverrideSlots` | `material.authoring.set_material_instance_base_property_overrides` (`enableDisplacementFade` only) | **5.5** | Same shape as the row above: drop the entry, reject the param. `bOverride_DisplacementFadeRange` / `DisplacementFadeRange` arrived in the same 5.5 window but are **not** a blocker here — this verb deliberately does not expose the struct-valued displacement slots. | Resolved: guard at `Handlers/Material/MaterialAuthoringHandler.cpp:2812`. |
| `FMaterialInstanceBasePropertyOverrides::{bOverride_bHasPixelAnimation, bHasPixelAnimation, bOverride_bEnableTessellation, bEnableTessellation}` (`MaterialInstanceBasePropertyOverrides.h:50,54,104,108`) + `UMaterialInstance::{HasPixelAnimation, IsTessellationEnabled}` (`MaterialInstance.h:992,1005`) | Same file, the `hasPixelAnimation` and `enableTessellation` entries of `GetInstanceBoolOverrideSlots` | `material.authoring.set_material_instance_base_property_overrides` (those two params only) | **5.4** | Drop both entries and reject both params. 5.3 has neither the flags nor the accessors — it predates both nanite-displacement tessellation and the TSR pixel-animation hint — so there is no substitute, only omission. The rest of the slot table (`twoSided`, `isThinSurface`, `ditheredLODTransition`, `castDynamicShadowAsMasked`, `outputTranslucentVelocity`, `opacityMaskClipValue`, `maxWorldPositionOffsetDisplacement`, `blendMode`, `shadingModel`) plus `FMaterialInstanceParameterUpdateContext::SetBasePropertyOverrides` itself is present on 5.3, so the verb backports with a reduced vocabulary rather than not at all. | Resolved: guard at `Handlers/Material/MaterialAuthoringHandler.cpp:2804`. |
| `USoundWave::SetSoundWaveCuePoints` (`SoundWave.h:500`) + `USoundWave::SetCuePointOrigin` (`:503`) + `ESoundWaveCuePointOrigin` (`:77`) | `Source/PinWright/Private/AudioGen/PwAudioExport.cpp`, `PwAudioExportInternal::UpdateSoundWaveInPlace` | `audio.synth.export`, `audio.authoring.create_sound_wave_from_pcm`, `audio.music.export_stems` - only the branch that rewrites an EXISTING wave; a create never reaches these calls | **5.7** (`SetSoundWaveCuePoints` is 5.6; `SetCuePointOrigin` is 5.7, so the pair is 5.7) | Assign the fields directly: `CuePoints` is a public `UPROPERTY` on 5.3 (`SoundWave.h:734`) and 5.5 (`:795`) - it only moved behind `protected` at 5.8, which is what forces the setter here. `CuePointOrigin` does not exist before 5.6 (`SoundWave.h:793` on 5.6) and has no setter until 5.7, so on 5.3-5.5 drop that line entirely: those engines have no cue-point origin concept and behave as the wave-file origin unconditionally. Dropping it costs nothing here because the rewrite clears the cue points anyway and the origin is inert on an empty set. Untested. | Resolved: guard at `AudioGen/PwAudioExport.cpp:232,245`. |
| `UMetaSoundSource::GetOperatorSettings` (`MetasoundSource.h:345`) | `Source/PinWright/Private/AudioGen/PwMetaSoundRender.cpp`, `PwRenderMetaSoundSource` (the `EffectiveSampleRate` read) | `audio.synth.render_metasound` | **5.4** | The method exists on 5.3 (`MetasoundSource.h:245`) but is **private** there, so this is an access break rather than a missing symbol. Substitute: drop the call and label the buffer with the REQUESTED rate, which is correct for every asset that does not set a `SampleRateOverride` / non-default quality setting - and report only `requestedSampleRate`, never an `effectiveSampleRate` the code did not measure. Untested. Note the rest of this file's MetaSound render path is portable: `InitResources`, `InitParameters`, `CreateSoundGenerator(InParams, TArray<FAudioParameter>&&)` and `OnEndGenerate(ISoundGeneratorPtr)` are public on 5.3-5.8, the `au.MetaSound.EnableAsyncGeneratorBuilder` cvar exists on all six, and `ISoundGenerator::GetNumChannels()` (5.8-only) is deliberately not used - the channel count is read from `UMetaSoundSource::NumChannels`, public on every version. Verified by grepping all six installed engine trees, not inferred. | Resolved: guard at `AudioGen/PwMetaSoundRender.cpp:330`. |

Attribution: rows 1-2 from integration pass 11 (`scratchpad/integration_pass_11.md` §6b, board
`B-derived-state-verbs-are-ue58-only`); rows 3-5 from the per-version survey recorded in the
`SkeletalMeshAssetIOHandler.cpp` header comment (`:35-71`), which is the model for documenting this
at the call site. The `PerVertexNormalSource` row from the noise-seed pass, verified by grepping `GeometryScriptingCore/Public/` in all six installed engine trees. Row 6 from the wave-2 audio-synthesis renderer; its first-available and
substitute columns were verified by listing `Runtime/SignalProcessing/Public/DSP/` and
`Plugins/Runtime/AudioSynesthesia/Source/AudioSynesthesiaCore/Public/` in all six installed trees.

The `LayoutMeshUVs` row is the `model` namespace's only blocker. It was first surveyed before the
code landed, from the `.pwmodel` plan's Wave 3, and now cites the shipped call site. Note it is
reached through a **value in the source document** (`uv mode=layout`), so the guard owes the grammar
a version-gated rejection as well as a substitute call — which is what it does; otherwise a 5.3
build would accept the document and fail at the missing function.

The `.pwmodel` plan also listed `EGeometryScriptCollisionGenerationMethod::LevelSets` as a Wave 3
blocker. **The shipped code is not one** — see *Checked and portable* below — so it has no row here.

The two Sequencer binding-resolution rows (`CreateTransientSharedPlaybackState`, `GetBoundObjects`) are the cost of *verifying* a component binding rather than of creating one: `ULevelSequence::FindOrAddBinding`, which does the binding itself, is present on all six. They are reached only when `componentName` is supplied, so an actor-only caller on an older engine would be unaffected - and both are now guarded, so 5.3-5.6 fall back to the substitutes named above. Verified by grepping the six trees: `CreateTransientSharedPlaybackState` absent on 5.3/5.4, `GetBoundObjects` absent on 5.3-5.6.

## Checked and portable

Recorded so the next backport does not re-derive them:

- `ALandscapeProxy::UpdateAllComponentMaterialInstances` — present 5.3-5.8 (`LandscapeProxy.h:1213`
  on 5.3 … `:1414` on 5.8). The landscape consumer-refresh header is otherwise portable; one call
  pins it to 5.8.
- `USplineComponent::AllowsSplinePointScaleEditing` — present 5.3-5.8 (`SplineComponent.h:350` on
  5.3 … `:425` on 5.8), so the derived-scale refusal in `SplineHandler.cpp:688` is portable.
- `SplineHandler.cpp` reaches `UWaterSplineComponent` by reflection
  (`FindObject<UClass>("/Script/Water.WaterSplineComponent")`), so it carries no Water link and no
  version dependency.
- `render.capture_ortho_tiles`' remaining scene-capture surface is portable 5.3-5.8:
  `bEnableOrthographicTiling` (`SceneCaptureComponent2D.h`, all six), `OrthoWidth`, `ProjectionType`,
  `CaptureSource`/`SCS_FinalColorLDR`, `PostProcessSettings`/`PostProcessBlendWeight`, the
  `AEM_Manual` physical-camera exposure fields, `UTextureRenderTarget2D::RenderTargetFormat` /
  `RTF_RGBA8_SRGB`, and `FRenderTarget::ReadPixels`. Only the two ortho-plane booleans pin it to 5.4.
- `SkeletalMeshAssetIOHandler.cpp`'s remaining Geometry Script surface — `CopyMeshFromSkeletalMesh`
  / `CopyMeshToSkeletalMesh`, `CreateNewSkeletalMeshAssetFromMesh`, the `MeshBoneWeight` calls,
  `bUseMeshBoneProportions`, `EGeometryScriptSmoothBoneWeightsType`, `GetAllBonesInfo` — verified
  unchanged across all six.
- SignalProcessing's pseudo constant-Q surface — `Audio::NewPseudoConstantQKernelTransform`,
  `Audio::FPseudoConstantQ::{GetConstantQCenterFrequency,GetConstantQBandWidth}`,
  `FPseudoConstantQKernelSettings` (`NumBands`, `NumBandsPerOctave`, `KernelLowestCenterFreq`,
  `BandWidthStretch`, `Normalization`), `EPseudoConstantQNormalization::EqualAmplitude` and
  `FContiguousSparse2DKernelTransform::TransformArray(const float*, float*)` — present 5.3-5.8
  (`DSP/ConstantQ.h`, `DSP/FloatArrayMath.h:310` on 5.3 … `:379` on 5.8). Used by
  `AudioGen/PwAudioPlot.cpp`'s constant-Q view, which therefore owes no blocker row.
- The MetaSound data-type registry surface — `Metasound::Frontend::IDataTypeRegistry::Get()` plus
  `IsRegistered`, `GetDesiredLiteralType`, `GetUClassForDataType`, `IsValidUObjectForDataType`,
  `GetRegisteredDataTypeNames`, and `Metasound::Frontend::GetMetasoundFrontendLiteralType` /
  `FMetasoundFrontendLiteral::SetType` — present 5.3-5.8
  (`MetasoundFrontendDataTypeRegistry.h`, `MetasoundFrontendLiteral.h`; verified by grepping all six
  trees, not inferred). Used by `Handlers/Audio/MetaSound/MetaSoundLiteralFromTypeName.cpp` to answer
  "is this a real data type / does this UObject fit it" from the registry instead of a hardcoded
  table, so `audio.authoring.add_metasound_input` / `set_metasound_default` /
  `set_metasound_node_input_default` owe no blocker row. The header is probed with
  `__has_include` (`PW_METASOUND_HAS_DATATYPE_REGISTRY`) and the primitive table stands in when it
  is absent, so a host without it degrades rather than failing to compile.
- `FMetaSoundFrontendDocumentBuilder::{FindNode,FindNodeInput,FindNodeInputs,SetNodeInputDefault}` —
  present 5.3-5.8 (`MetasoundFrontendDocumentBuilder.h:184,186-188,250` on 5.3 … `:361,377-378,389,849`
  on 5.8; 5.5+ appends a defaulted `const FGuid* InPageID`, which the call sites omit). Used by
  `audio.authoring.set_metasound_node_input_default`. Deliberately NOT used there:
  `FindNodeInputDefault` is 5.5+, so the node-pin readback walks
  `FMetasoundFrontendNode::InputLiterals` on the document instead — which is also the independent
  read path rpc-design §4 wants. `FindGraphVariable` is 5.5+ too and *is* called, but only from
  `set_metasound_variable_default`'s `#if !UE_VERSION_OLDER_THAN(5, 6, 0)` branch (the whole
  graph-variable family is 5.6+ and rejects older engines with `UNSUPPORTED_ENGINE_VERSION`).

- SignalProcessing's music-theory surface — `Audio::EMusicalScale::Scale` (all 30 enumerators, same
  order), `Audio::FMidiNoteQuantizer::{QuantizeMidiNote, ScaleDegreeSetMap}`,
  `Audio::ScaleDegreeSet::GetScaleDegreeSet` (`DSP/MidiNoteQuantizer.h`) and
  `Audio::GetFrequencyFromMidi` (`DSP/Dsp.h`) — present 5.3-5.8, verified by listing the header in
  all six installed trees and diffing the enum body between 5.3 and 5.8 (identical). Used by
  `AudioGen/PwMusicScore.cpp` for scale-degree resolution and tuning, so the music-score schema owes
  no blocker row. Also worth knowing for the backport: `FQuartzTimeSignature` and
  `EQuartzTimeSignatureQuantization` (`Sound/QuartzQuantizationUtilities.h`), which the score's
  beat convention is stated against, are likewise present 5.3-5.8.
- **`ModelingComponentsEditorOnly` — the new module dependency the `.pwmodel` asset bake adds — is
  portable 5.3-5.8.** `MeshModelingToolset` is a Runtime engine plugin on all six
  (`Engine/Plugins/Runtime/MeshModelingToolset/…`), and the whole surface the single-build
  `CreateStaticMeshAsset` path uses is present and identically named in every tree:
  `UE::AssetUtils::CreateStaticMeshAsset` (`AssetUtils/CreateStaticMeshUtil.h:119` on 5.3-5.4,
  `:128` on 5.5-5.8), `FStaticMeshAssetOptions::NumMaterialSlots` (`:59`, all six),
  `::AssetMaterials` (`:89` on 5.3-5.4, `:92` on 5.5-5.8), `::CollisionType` (`:85` / `:88`), and
  `::bDeferPostEditChange` (`:95` / `:98`). Only the line numbers moved. The sibling
  `ModelingComponents` symbols are portable too: `UE::Geometry::UpdateSimpleCollision`
  (`Physics/ComponentCollisionUtil.h:103`, all six) and `GenerateNewMaterialSlotName`
  (`AssetUtils/StaticMeshMaterialUtil.h:73` on 5.3, `:75` on 5.4-5.8). Adding these two modules
  therefore costs the backport nothing; the `model.*` rows above are about two *values*, not the
  bake.
- The UV projection surface `.pwmodel`'s `uv` op maps onto is portable 5.3-5.8 apart from the
  `LayoutMeshUVs` row above: `SetMeshUVsFromPlanarProjection`, `SetMeshUVsFromBoxProjection`,
  `SetMeshUVsFromCylinderProjection`, `AutoGeneratePatchBuilderMeshUVs` and
  `AutoGenerateXAtlasMeshUVs` all exist in every tree (`MeshUVFunctions.h:230,243,256,294,305`
  on 5.3 … `:445,458,471,570,581` on 5.8 — PatchBuilder is declared before XAtlas in both trees).
  Note GeometryScripting itself is under `Engine/Plugins/Experimental/` on 5.3 and
  `Engine/Plugins/Runtime/` from 5.4 — a path difference, not a symbol one. **No spherical
  projection exists on any of the six**, so `uv mode=spherical` is rejected by name rather than
  version-gated.
- **`collision { auto method=level_sets }` needs no guard and no row.** `EGeometryScriptCollisionGenerationMethod::LevelSets`
  is 5.4+ (`CollisionFunctions.h:26`), but nothing in the tree names the enumerator:
  `PwModelCollision_MethodTable` (`Source/PinWrightGeometry/Private/Model/PwModelCollision.cpp:135-157`)
  builds the whole `auto method=` vocabulary by reflecting over `StaticEnum<EGeometryScriptCollisionGenerationMethod>()`
  and snake-casing each name. On 5.3 the enum stops at `MinVolumeShapes`, so `level_sets` is simply
  absent from the table and a document using it is rejected by name with `PWSRC_BAD_VALUE`, listing
  the methods that engine does have. A reflected vocabulary is the general pattern worth copying:
  it degrades to an honest rejection instead of a compile break. `FKLevelSetElem` remains unreachable
  through Geometry Script on 5.3, and `convex_hulls` — the nearest generator — cannot represent
  concavity, which is the only reason to ask for level sets.

Two traps this list exists to name:

- `#if MCP_HAS_WATER` gates on **the Water plugin being present**, not on engine version. It does
  not protect the river accessors.
- `WITH_EDITORONLY_DATA` is likewise not a version gate; it is always 1 for this plugin's
  `"Type": "Editor"` modules.

## Latent engine crash sites

Not a version axis - recorded here for the same reason as *Checked and portable*: an engine fact that
costs nothing today and would cost a crash to re-derive. Every site below dereferences
`Mesh.Attributes()->PrimaryNormals()` (or an overlay obtained the same way) after testing only
`HasAttributes()`. `HasAttributes()` says an attribute SET exists; it says nothing about that set
containing a normal layer, and `FDynamicMeshAttributeSet::PrimaryNormals()` returns `nullptr` when
`NumNormalLayers() == 0`. The deref then runs on a null overlay.

**Reachability precondition, and why every row is LATENT today:** a `NumNormalLayers() == 0` attribute
set has to be produced first, and nothing reachable from a `.pwmodel` document or an RPC verb produces
one. `FDynamicMeshAttributeSet::SetNumNormalLayers` has **zero callers** anywhere in GeometryScripting,
and `AppendBuffersToMesh` - the one engine call that does resize a layer set behind the caller's back -
zeroes **UV** layers only (`MeshBasicEditFunctions.cpp:973-983`; that is the live defect the `shell` and
`bevel` guards, and `GeometryOps::AppendBuffers`' UV padding, exist for). The day any code path can reach
`SetNumNormalLayers(0)` - an engine change, a new verb, a new `.pwmodel` op - every row here becomes live
at once. That is the trigger to re-read this section.

Do NOT patch these; they are engine source. The mitigation, when one becomes live, is a PinWright-side
guard on the op that reaches it, in the shape of the `shell` guard in `GeometryOps_Modeling.cpp`.

| Engine site | Reached by | Shape |
|---|---|---|
| `PNTriangles.cpp:113-116`, `:452-459` | `geometry.subdivide`, `.pwmodel` `subdivide` | `PrimaryNormals()` dereferenced under a bare `HasAttributes()`. Note `:143` **in the same file** performs the null check the other two omit, so the fix is already written thirty lines away. |
| `MeshNormalsFunctions.cpp:207-218` | `geometry.recalculate_normals`, `.pwmodel` `recalculate_normals` | `if (!HasAttributes()) EnableAttributes(); ... PrimaryNormals()->...`. The `EnableAttributes()` is a no-op RE-ENTRY when an attribute set already exists, so it does not create the missing normal layer - it only makes the null deref look guarded. |
| `MeshNormalsFunctions.cpp:359-399` | `geometry.split_normals`, `.pwmodel` `split_normals` | Same enable-then-deref shape. |
| `MeshBasicEditFunctions.cpp:955-957` | `geometry.append_buffers` / `.pwmodel` `append_buffers`, **only** with `normals=` | `PrimaryNormals()->AppendElement(...)` inside `if (Buffers.Normals.Num() == NumVertices)`, with no layer check. |
| `MeshPrimitiveFunctions.cpp:64-67` | every primitive verb, **only** with `bFlipOrientation` | Same shape, on the orientation flip's normal rebuild. |
| `DynamicMeshEditor.cpp:1199`, `:1234`, `:1268`, `:1309` | anything reaching `FDynamicMeshEditor`'s normal helpers | Guarded by `check(Mesh->HasAttributes())`, which is the wrong assertion: it holds on exactly the mesh that crashes, because an attribute set with zero normal layers passes it. |

Found by an adversarial audit of the `shell` crash's siblings and verified against the 5.8 engine
source; line numbers are 5.8. The UV-side sibling of this family is **not** latent and is guarded in
code - see `GeometryOps::Shell` and `GeometryOps::Bevel`.

**A second normal-side site is also not latent, and it is a different defect from the null-overlay
family above.** The three space deformers read a normal element's *parent vertex* after checking only
that the element exists, so an element that is allocated but attached to no triangle indexes
`GetVertex` out of bounds - unsigned index, bounds test compiled out, editor process gone with no
diagnostic. Unlike the rows above this needs no `NumNormalLayers() == 0` mesh: an orphaned element is
ordinary engine output (`FDynamicMesh3::Copy(FMeshShapeGenerator*)` orphans one per generator normal
no triangle names, `DynamicMesh3.cpp:174-186`).

| Engine site | Reached by | Shape |
|---|---|---|
| `BendMeshOp.cpp:103-113`, `TwistMeshOp.cpp:48-58`, `FlareMeshOp.cpp:68-78` | `geometry.bend` / `.twist` / `.taper`, `.pwmodel` `bend` / `twist` / `taper` | `Normals->GetParentVertex(ElID)` fed straight into `ResultMesh->GetVertex(VertexID)` under a bare `IsElement(ElID)` test. Verbatim in all three files. |

Guarded in code by `GeometryOpsModeling_GuardWarpDeformerNormals`
(`GeometryOps_Modeling.cpp`), which **repairs and warns** rather than refusing: orphans are freed
(`GeometryUtils::EnsureMeshNormalParentsAreValid`) and the count is reported in the response's
`warnings` array, because the state is legal, engine-produced and carries no geometry. Only a mesh
with no normal layer at all, or one still unparented after the free, is refused
(`INVALID_NORMAL_OVERLAY`). That is the second mitigation shape available when one of the latent rows
above goes live: repair where the state is valid input, refuse only where it is not.

## Dead version guards

Swept 2026-08-19: every `#if`/`#elif` in `Source/` carrying `UE_VERSION_*` or `ENGINE_*_VERSION`
was evaluated against 5.8. **324 guards; 215 take the guarded branch on 5.8, 105 take the `#else`,
4 also depend on `__has_include`/a module macro.** The 105 are almost all
`#if UE_VERSION_OLDER_THAN(5,4|5,5|5,6)` legacy branches whose `#else` is the modern path - correct
by construction and not worth re-auditing.

**The defect this sweep was for:** a *bounded* guard, `>= X && < X+1`, wrapping a documented-primary
**engine API**, with a hand-rolled fallback on every later version. On 5.8 the engine call is dead
and the fallback is the only live path. Written by someone who meant `>= X`.

Only **6** bounded guards ever existed, and the shape alone does not decide it. The discriminator:

| What the bounded range enables | Verdict |
|---|---|
| The **engine API**, fallback on newer engines | **Defect.** The engine path is dead on 5.8. |
| A **workaround** for a bug in that one version | **Correct.** 5.8 takes the normal path, as intended. |

Both defect instances were the same call, `ScaleMesh(..., bFixOrientationForNegativeScale=true)`
pinned to 5.4 alone, and both shipped inside-out geometry because the fallback moved vertex
positions without reversing the winding:

| Site | Verb | Was | Now |
|---|---|---|---|
| `GeometryOps_Boolean.cpp` `Mirror` | `geometry.mirror`, `.pwmodel` `mirror` | `>= 5.4 && < 5.5` | `>= 5.4` |
| `GeometryOps_Modeling.cpp` `Stretch` | `geometry.stretch`, `.pwmodel` `stretch` | `>= 5.4 && < 5.5` | `>= 5.4` |

`ScaleMesh` still takes `bFixOrientationForNegativeScale` as parameter 4, defaulted `true`, in 5.8
(`MeshTransformFunctions.h:70-75`), so the upper bound was simply wrong. It forwards to
`MeshTransforms::Scale`, which reverses orientation when `Scale.X * Scale.Y * Scale.Z < 0`
(`MeshTransforms.cpp:179`) **and** applies the inverse scale to normals and tangents when the scale
is non-uniform (`:150-170`). Neither hand-rolled fallback does the second, so `stretch` left stale
normals behind on *positive* factors too - the fix is not only about the negative case. On the
first they now agree with the engine and with each other: both reverse orientation only when the
scale's determinant is negative. `Mirror`'s used to reverse **unconditionally**, which is wrong for
`EMeshAxis::None` - the unrecognized-`axis` fallback, a unit scale that mirrors nothing - and that
divergence is fixed.

Regressions: `TestGeometryMirrorOrientation.cpp`, `TestGeometryStretchOrientation.cpp`. Both measure
**signed volume**, because a normal-vs-winding consistency check passes on an inverted mesh - and a
`recalculate_normals` afterwards makes it pass by construction.

**Both fallbacks survive as dead code, deliberately.** Repairing the guard to `>= 5.4` did not
delete the `#else`; it made it unreachable on every version this plugin builds against. They are
kept so a 5.3-and-earlier backport has something to restore, and both now carry a `DEAD BRANCH`
banner naming this page. A backport must not simply re-enable them: they move vertex positions
only, so whatever they are restored into needs the normal/tangent handling written as well.

| Dead `#else` | Lives in | What it omits vs `ScaleMesh` |
|---|---|---|
| `Mirror` | `GeometryOps_Boolean.cpp` | Inverse-scale normals and tangents |
| `Stretch` | `GeometryOps_Modeling.cpp` | Inverse-scale normals and tangents |

The four correct bounded guards, so nobody re-audits them:

| Site | Range | Enables |
|---|---|---|
| `MaterialDiscoveryHandler.cpp` | `>= 5.5 && < 5.6` | Skipping `UMaterialExpressionObjectPositionWS::GetCaption()`, which derefs a null `Material` on a CDO on 5.5 only. |
| `MetaSoundPatchPresetHandler.cpp` | `>= 5.5 && < 5.6` | `SendUnsupportedEngineVersion` instead of a hard fatal from 5.5's `ConvertToPreset` default arg. Verified: 5.8's default arg is the safe `TSharedPtr` form (`MetasoundFrontendDocumentBuilder.h:300,303`). |
| `TestMetaSoundPatchPreset.cpp` | `>= 5.5 && < 5.6` | The test-side mirror of the row above. The two agree on 5.8: both attempt creation. |
| `UtilityPropertyHandler.cpp` | `>= 5.5 && < 5.6` | A re-clear after `PostEditChangeChainProperty` re-marks the property overridden on 5.5 only. |

**Auditing rule.** Guard shape is not evidence. Read what the guarded branch *is*: an engine call
fenced with an upper bound is a defect until the API is shown to be absent in 5.8; a workaround
fenced to one version is correct. Verify against `C:\UE_5.8\Engine` - never infer removal from the
guard's own comment, which is what let both of these sit.

## Deprecated-in-5.8 writes that silently no-op

A version guard is not the only way an engine change goes unnoticed. A `UPROPERTY` marked
`meta = (Deprecated = 5.8)` still **compiles and still assigns** - UHT emits no C++ deprecation
warning for the meta form - so a handler can keep writing a field that no engine code reads any
more, and report success.

**FIXED, and verified in a live editor** — not by reading source. `audio.authoring.create_metasound_preset`
now sets `Factory->Template`, verifies presetness off the created document before answering, and
returns `PRESET_NOT_APPLIED` instead of a fake success when the link did not land. Two sibling
*read* sites were fixed with it (below). The finding is kept in full because the mechanism
generalises and the audit below is the reusable part.

Runtime evidence against the built plugin, since the whole point of this entry is that a clean
compile proves nothing here:

| Probe | Preset | Plain patch (control) |
|---|---|---|
| `describe_metasound` → `rootGraph.isPreset` | `true` | `false` |
| `describe_metasound` → `nodes` | one node named after the parent (`FRebuildPresetRootGraph`'s output) | `[]` |
| same, after `asset.reload` pulls it back off disk | still `true`, node still present | — |
| `.uasset` bytes contain `MetaSoundFrontendPresetTemplate` | yes (×1) | **no (×0)** |
| `.uasset` bytes contain the parent's asset name | yes (×2) | — |

The control column is the part that matters: it is what makes the `.uasset` byte match evidence
rather than a string that might appear in any MetaSound package.

**`audio.authoring.create_metasound_preset` produced a blank NON-preset on 5.8.** Found while
auditing the `>= 5.5 && < 5.6` guard above, which is itself correct.

- `MetaSoundPatchPresetHandler.cpp:217` (source) and `:227` (patch) set
  `Factory->ReferencedMetaSoundObject`.
- That field is `UPROPERTY(Transient, meta = (Deprecated = 5.8, DeprecationMessage = "Use document
  template instead"))` - `MetasoundFactory.h:23-24`.
- Neither `UMetaSoundFactory::FactoryCreateNew` (`MetasoundFactory.cpp:34-49`) nor
  `UMetaSoundSourceFactory::FactoryCreateNew` (`:57-71`) reads it; they pass only `.Template` and
  `.SelectedObjects` into `InitAsset`. A grep of the whole MetaSound plugin finds **one** occurrence
  of the name - the declaration. Zero readers.

The handler returned `success` with `"MetaSound preset '%s' created"` and a `referencedAssetPath`
field describing a link that did not exist. The test asserted only `TestNotNull`, which a blank patch
satisfies perfectly, so the suite stayed green — the defect was invisible from both ends.
`PinWright.Assets.CreateMetaSoundPresetIsGenuinePreset` now drives the verb end to end and asserts
the parent link in the response, on the created document, and in the saved `.uasset` bytes.

The 5.8 mechanism is the preset **template**, as Epic's own path does it
(`MetasoundAssetDefinitions.cpp:329-356`): build a
`TInstancedStruct<FMetaSoundFrontendPresetTemplate>` (`MetasoundFrontendPresetTemplate.h:19`), set
`.Parent`, assign it to `Factory->Template`, and add the parent to `SelectedObjects`. The legacy
`InitAsset(UObject&, UObject* InParentMetaSound, bool)` overload still builds that template
internally (`MetasoundEditorSubsystem.cpp:318-341`) but is `UE_DEPRECATED(5.8, ...)`
(`MetasoundEditorSubsystem.h:98-99`).

**The general check this implies:** `meta = (Deprecated = <version>)` on a `UPROPERTY` is invisible
to the compiler. When an engine bump lands, grep the plugin's factory/settings assignments against
the new engine's readers rather than trusting a clean build.

### The sweep this triggered

Every version-deprecated `UPROPERTY` in UE 5.8 was enumerated and intersected against every member
name PinWright assigns, plus every 5.6–5.8 `UE_DEPRECATED` function PinWright calls. **The class is
small and re-auditable: 18 `UPROPERTY` declarations carry `meta=(Deprecated = 5.x)` across all of
`Engine/Source` + `Engine/Plugins`** (the other ~338 `Deprecated = "5.x"` hits sit on `USTRUCT` /
`UENUM` / `UCLASS`), and none of them is a multi-line macro. Re-run the same intersection after the
next engine bump.

Two further same-defect sites, both the **read** mirror of the same field family, both fixed here:

| Site | Field | Why it was always wrong | Fix |
|---|---|---|---|
| `MSIR/MSIRDecompiler.cpp` (`IrDoc.bIsPreset`) | `FMetasoundFrontendGraphClassPresetOptions::bIsPreset`, `MetasoundFrontendDocument.h:1901`, `meta=(DeprecatedProperty, "5.8 - Preset options are now serialized in the Preset MetaSound Template")` | 5.8's document versioning reads the flag once to migrate presetness into `Document.Template`, then **clears** it; nothing sets it again. It drove both `IrDoc.Kind` and the preset-emit branch, so MSIR for any 5.8 preset came out shaped as a full patch/source | `PinWright::MetaSound::IsMetaSoundDocumentPreset(Doc)` |
| `Handlers/Asset/MetaSoundDumpBuilder.cpp` (`rootGraph.isPreset`) | same | `metasound.json` reported `isPreset:false` for every preset while the sibling `assetKind` (an `IsA<>` test) stayed right — the disagreement between the two fields was the only visible symptom | same; `metasound.json` aspect bumped 2 → 3, `msir.txt` 4 → 5 |

Everything else came back clean. 11 sites touch deprecated engine APIs deliberately and correctly —
each is either `PRAGMA_DISABLE_DEPRECATION_WARNINGS`-wrapped or `UE_VERSION_*`-gated, and three of
them (`UMaterialInterface::GetMaterialResource(FeatureLevel)`, `ISearchEngine::FindAllInterfaces`,
`FMetasoundFrontendClassMetadata::GetIsDeprecated`) sit on 5.8 bodies that are literally
`return NULL;` / `return { };` / a dead field, all correctly routed around. Every engine `UFactory`
property PinWright assigns (`TargetSkeleton`, `ParentClass`, `BlueprintType`, `Struct`,
`InitialParent`, and the MetaSound `Template` / `SelectedObjects`) carries no deprecation marker;
`ReferencedMetaSoundObject` was the only offender.

### Backport note for the preset fix

`FMetaSoundFrontendPresetTemplate` and `UMetaSoundBaseFactory::Template` are **5.8-only**. The fix is
gated on `PW_METASOUND_HAS_PRESET_TEMPLATE` (`Handlers/Audio/MetaSound/MetaSoundPathUtils.h`, an
`__has_include` on `DocumentTemplates/MetasoundFrontendPresetTemplate.h`), so it does not break the
compile on an older engine — but with the macro off, `create_metasound_preset` **refuses** with
`METASOUND_NOT_AVAILABLE` rather than falling back. On 5.6/5.7 the old
`Factory->ReferencedMetaSoundObject` path did work (the factory read it and called the 2-arg
`InitAsset`), so a backport must restore that branch under `#else` rather than leave the verb
disabled. `IsMetaSoundDocumentPreset` already carries exactly that shape — 5.8 reads
`Document.Template`, older engines read `RootGraph.PresetOptions.bIsPreset` — and is the model.

## This table is not an audit

It is what has been found while doing other work. The only complete answer is a compile per
version, run locally:

- `.polyskill/skills/mcp-version-matrix/` — the per-version compile + suite loop over `5.3`…`5.8`.
- `scripts/package-prebuilt.ps1 -EngineRoot C:\UE_5.x` — `RunUAT BuildPlugin -Rocket` per engine
  (add `-StrictIncludes` for the unity-disabled, slow gate).

When a change touches a version-sensitive API, run the version matrix against the oldest engine
first, harvest the compiler errors, and extend the table above — the errors *are* the missing rows.

## When you add a row

Two places, both required:

1. A row here, so the backport cost is knowable without a build.
2. A per-version note at the call site, in the `SkeletalMeshAssetIOHandler.cpp:35-71` shape: name
   each absent symbol, the version that introduced it, the header `file:line`, and what the older
   engines have instead. That comment is why rows 3-5 above cost a copy-paste rather than a survey.

## Surfaces that state a range

Every surface that names an engine version. They must move together whenever the range changes in
either direction:

| Surface | Says | Note |
|---|---|---|
| `CLAUDE.md` (overview, *UE version compat*) | 5.3-5.8 | Links here. |
| `README.md` (Requirements) | 5.3-5.8, Windows or Linux | Links here. The end-user-facing statement. |
| `.polyskill/skills/mcp-version-matrix/` | candidates `5.3`…`5.8` | The local verification loop; the version to test is an argument, so a narrowed claim means narrowing the candidate list. |
| `product-facts.json` (`ueRange`, `ueVersions`) | `5.3-5.8` | **Generated** by `scripts/gen-product-facts.ps1` from the `$ueVersions` list in that script — editing the JSON alone is discarded. Changing the advertised range therefore means changing that list. |
| `PinWright.uplugin` | — | Carries **no** `EngineVersion` key, so it advertises nothing and needs no edit. |
| `docs/arch.md:146,586` | "UE 5.3-5.8 compat" | Describes what `Misc/EngineVersionComparison.h` is for, not a support claim. |
| `CLAUDE.md` (Architecture → transport) | `SocketHttpServer` is "uniform across UE 5.3-5.8" | True of that file — it needs no guards on any of the six. A claim about one implementation, not about the plugin; left alone. |
