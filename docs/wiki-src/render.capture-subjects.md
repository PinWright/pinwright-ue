# render.capture-subjects

One optional `subject` object names what a capture is *of*. The shared capability—bounds-fit framing, orbit sets, six axis-aligned sides, time series, and overlays—covers levels, actors, static/skeletal meshes, animation assets, and Niagara systems; verb pages document only differences.

Subject kind does not change image geometry: omitted `width` / `height` resolve to 768 × 768, while an explicit non-square pair remains valid and drives the projection aspect.

## The descriptor

```jsonc
"subject": {
  "kind":  "world" | "actor" | "staticMesh" | "skeletalMesh" | "animation" | "niagara",
  "path":  "/Game/…",            // asset kinds: staticMesh, skeletalMesh, animation, niagara
  "name":  "SM_Rock_12",         // actor kind; the same spellings actorName accepts
  "point": {"x":0,"y":0,"z":0},  // world kind
  "radius": 4000,                // world kind ONLY, and only with `point`; ignored everywhere else
  "animation": "/Game/…",        // skeletalMesh kind only: which animation to load onto the mesh
  "closeAfterCapture": true      // asset kinds; the same three states as the top-level argument
}
```

When omitted, `kind` is **inferred** from exactly one key: `path` from loaded `UClass`, `name` as `actor`, or `point` as `world`. Two keys return `INVALID_ARGUMENT` naming **both**, rather than silently capturing the wrong subject.

**Old spellings still work and are not deprecated.** `actorName`, `assetPath`, and `point` + `radius` normalize internally with the same behavior and response keys. `sequencePath` on [`camera.animation_shots`](camera.animation_shots.md) is **not** a subject; it is a separate time source.

`closeAfterCapture` keeps three states on every asset kind: omitted closes only a window this call opened; explicit `true` closes an already-open one; `false` never closes. Default `true` avoids a UE 5.8 shutdown crash from an open asset editor.

**`radius` belongs to a bare `point` and is silently ignored on other kinds.** Assets use their own bounds and actors do not read it. It is *mandatory* for `point`; omission returns `INVALID_ARGUMENT`. For asset padding, use `padding` (now accepted by every bounds-fitting verb, including [`camera.orbit_shots`](camera.orbit_shots.md), previously fixed at 1.15), or an explicit distance: `radius` on orbit, `distance` on [`camera.frame_actor`](camera.frame_actor.md). An explicit distance skips bounds fit, so `padding` cannot move the camera and the response says so. Top-level orbit `radius` wins over `subject.radius`.

**`subject.radius` has two deliberate meanings.** `camera.orbit_shots` reads it as **camera distance**; `camera.frame_actor` reads it as **sphere radius to fit**. At fov 50, `subject: {kind:"world", point, radius: R}` places the camera at `R` on orbit and about `2.5·R` on frame. Do not reuse the number without checking the verb.

## The `subject` response block, and when it is absent

| Field | Meaning |
| --- | --- |
| `kind` | Resolved kind after inference; read it instead of trusting payload spelling. |
| `path` / `name` | Asset object path, or actor's unique internal name. |
| `boundsOrigin` / `boundsRadius` | Sphere used for framing. Radius `0` means bounds were not measured; `framing` then reports nothing rather than a zero-based verdict. |
| `boundsSource` | How bounds were obtained. The only seven values are `assetBounds`, `sampledUnion`, `actorBounds`, `levelBounds`, `pinnedFixedBounds`, `authoredFixedBounds`, and `point`; they are not interchangeable. |
| `captureSource` | Viewport that drew pixels, using the verb's vocabulary (`staticMeshEditorPreview`, `personaPreviewViewport`, `levelEditorViewport`, …). |
| `timeSupported` | Whether the kind has a time axis; `false` for static mesh and skeletal mesh without a named animation. |
| `timeStartSeconds` / `timeEndSeconds` | Sampleable time-series span, when one exists. |
| `assetEditorWasAlreadyOpen` / `assetEditorClosed` | Measured, not echoed; asset kinds only. `assetEditorWasAlreadyOpen` separates cold first frames from warm ones—see [`render.capture-exposure`](render.capture-exposure.md) § *The first capture into a fresh preview window is a stop dark*. |
| `boundsWarning` | Present only for known-unrepresentative bounds: a Niagara GPU emitter without authored fixed bounds, or a system whose simulation never started and used `authoredFixedBounds`. |
| `subjectCoverage` | Fraction of frame actually occupied, measured in **pixels**. Published for every served asset kind—`staticMesh`, `skeletalMesh`, `animation`, `niagara`—and **absent**, never `0`, if the preview component is unidentified. See *Coverage is measured differentially*. |
| `coverageWarning` | Present only below the 0.005 floor; warning, never failure, because a wide establishing shot is valid. |

**When no subject resolves, the block is omitted, not empty.** A call with no `subject` or subject-implying legacy spelling has no `subject` key. Blocks appear only when they have a value, so test key absence rather than fields equal to `0`.

## Material readiness and fallback policy

`render.capture_asset_preview` probes after the final capture and always publishes `materialReadiness` beside the frame and subject evidence. It gives already-submitted material work one bounded, game-thread-pumped wait before the final probe. For Static Mesh and Skeletal Mesh assets it reports aggregate `compiled`, `compiling`, `failed`, `usingDefaultMaterial`, `fallbackOccurred`, and `fallbackPossible` fields, and probes each material interface's actual render resource so instance static permutations keep their own verdict. The response says `meshUsagePolicy:"capturedComponentSections"`: Skeletal Mesh uses the forced LOD when set and otherwise the component's post-draw predicted LOD; Static Mesh uses its forced LOD when set, or an explicitly disclosed LOD0 fallback because the component has no game-thread predicted-LOD API. `renderedLodIndex`, `scope`, and each row's `includedByFallbackPolicy` expose that choice. Editor section/material preview filters and skeletal hidden-material state exclude sections not drawn by the component. Null included slots are explicit unassigned Default Material subjects. Asset kinds without directly inspectable material slots publish `measured:false` with an empty subjects list rather than inventing a clean verdict.

A subject with a failed shader map carrying compile errors, or an unassigned material on a rendered section, is a known Default Material substitution and returns `MATERIAL_FALLBACK` with `success:false` by default. The image evidence remains in the error details. `allowFallback:true` explicitly accepts that known-fallback image, preserving the normal success/path/image statistics while retaining `fallbackOccurred:true` and adding a reason-aware top-level `warnings[]` entry. Incomplete `notCompiled`, `outstanding`, and `timedOut` states are uncertainty rather than failure: the capture succeeds without opt-in, reports `fallbackPossible:true` plus `possibleReason:"shaderMapIncomplete"`, retains the exact per-subject status, and warns that the frame may use fallback. Intentionally selecting the engine Default Material reports `usingDefaultMaterial:true` without either fallback flag. Debug view modes that replace subject materials set `subjectMaterialsRendered:false` and do not attribute those debug pixels to a broken slot; the existing Nanite warning remains the exception because Nanite can bypass that substitution and render the subject material.

## Bounds never come from the posed or simulated subject

A camera solved from bounds that move is a camera that moves, and a set shot that way is not comparable frame to frame. Each provider therefore names a static source, and the response says which one it used:

| `boundsSource` | Where it comes from | Why that one |
| --- | --- | --- |
| `assetBounds` | Mesh asset `GetBounds()`. | Bit-identical across animation instants; posed component bounds change and would re-fit each shot. |
| `sampledUnion` | Union of actor bounds over every sampled instant in a no-capture pre-pass. | Keeps a translating *placed* actor in frame; costs one extra time-plan pass. |
| `actorBounds` | `GetActorBounds(false, …)` on the placed actor. | Single-instant render bounds, including collision-less actors. |
| `levelBounds` | Level bounds. | Whole-map framing when no `point` is given. |
| `point` | Caller `point` and `radius`, verbatim. | A point has no bounds to fit, so `radius` is mandatory. |
| `pinnedFixedBounds` | Union of Niagara local bounds over five probe instants across the time window, written with `SetSystemFixedBounds` for the call. Release restores prior fixed bounds or clears the pin. | Dynamic bounds refresh on escape or every 5 s (up to 5 s stale on shrink). Pinning removes that staleness and prevents t=0 framing from cropping the widest effect. |
| `authoredFixedBounds` | `UNiagaraSystem::GetFixedBounds()`, the author's asset box, reached only when **not one** probe instant ran a simulation; always has `boundsWarning`. | It is not a live measurement. It remains distinct from `pinnedFixedBounds` because they once shared a name: engine `SimpleExplosion` repeatedly reported `boundsRadius` 173.2050807568877, exactly 100·√3 for authored `FBox(-100…100)`, under a “measured” source string. Cross-check `subject.niagara.boundsProbesSimulated` and `simulated`. |

## Coverage is measured differentially, because no geometric measure can work

`boundsInFrame` only asks whether the subject's bounding sphere could project into the frame. It stays `true` for a central dot, floor-occluded subject, or Niagara system with **no particles alive** whose frame is pure backdrop. One empty preview reported `boundsInFrame: true`, `blank: false`, and `litPixelFraction: 1.0`: every signal looked healthy while the frame was empty.

**A geometric coverage number does not fix it.** For an empty frame with bounds radius 173.2, fit distance 464, and 50° fov, `asin(173.2/464) = 21.9°` against a 25° half-fov, or about **58% of frame area**. Geometry would report 58% for no content.

`subjectCoverage` is therefore differential pixel coverage: draw the pose **twice**, hidden then shown, and count pixels differing by more than 8 on any R/G/B channel. The same empty frame returns **0.000**. This remains true for a dot, an occluded subject, or no subject.

What it costs and where it applies:

- **Niagara only.** Other kinds are the preview scene's content, so hiding them hides the capture subject; their captures are unchanged and publish no coverage field.
- **One extra draw plus readback per shot**, roughly doubling set capture time; reference frames are deleted like the warm-up shot.
- **Absent, never zero,** when no differential ran. `poseSet.coverageReferenceShots` below shot count identifies shots without a figure; `poseSet.coverageWarning` names the shortfall.
- The 0.005 floor is 0.5% of frame—about a 70×70 square at 1024×1024, below which detail cannot be judged. It is **not** caller-settable; measured `subjectCoverage` and warning text expose the floor for caller-side policy.
- The 8/255 per-channel threshold is fixed so figures remain comparable. It was calibrated against encoder/temporal noise between two draws, well below the 100-plus change from a faint additive particle.

**`measureCoverage` is the switch, and exactly one verb offers it.**

The differential is **on by default**. [`render.capture_asset_preview`](render.capture_asset_preview.md) accepts `measureCoverage: false` to avoid the extra draw; opt-*in* was rejected because the signal is most needed before a caller knows to request it.

**The PROVIDER decides which kinds can be hidden.** A provider binds `FResolvedSubject::VisibilitySetter` when it can hide its subject without hiding the backdrop; the verb passes that setter to the pose-set request. Previously the closure lived inside `render.capture_asset_preview` behind a Niagara-shaped test, so unlisted kinds returned `subjectCoverage: null`, indistinguishable from disabled measurement. A **Static Mesh** capture exposed the gap: uniform color with no subject, `blank: false`, `crushed: null`, `litPixelFraction: 1.0`, and `boundsInFrame: true`.

The old rationale—that a mesh “cannot be hidden without hiding the thing captured”—was wrong: a reference frame hides the subject while retaining floor, sky, and lights. The hidden component must be matched **by the named asset**, never “the first static mesh component”; `FAdvancedPreviewScene`'s floor and sky sphere are static meshes too.

**No other pose-set verb declares `measureCoverage`, and that is an open gap rather than a decision.** `camera.frame_actor`, `camera.orbit_shots`, `camera.animation_shots`, and `render.capture_animation_preview` build the same request, but none reads the visibility setter or emits coverage; the flag would change no byte. A placed actor can be hidden/redrawn, but the seam does not reach those verbs. Extend the behavior before declaring another knob.

**The preview floor is hidden by default for a Niagara subject**, since origin-authored systems can be cut in half. Pass `previewScene.showFloor: true` to keep it; explicit choice wins. The flag affects only the per-capture rig pin and is restored after the shot, never the shared preview profile/config.

## Which verbs take a subject

| Verb | Subject | Domains it reaches |
| --- | --- | --- |
| [`camera.frame_actor`](camera.frame_actor.md) | yes | world, actor, staticMesh, skeletalMesh, animation, niagara |
| [`camera.orbit_shots`](camera.orbit_shots.md) | yes | all six |
| [`camera.animation_shots`](camera.animation_shots.md) | yes | **world and actor only**, both requiring a Level Sequence; asset kinds are refused because sequence scrubbing drives a *placed* actor |
| [`render.capture_asset_preview`](render.capture_asset_preview.md) | yes | four asset kinds; world/actor refused |
| [`render.capture_animation_preview`](render.capture_animation_preview.md) | yes | **skeletalMesh and animation only**; world, actor, staticMesh, niagara refused by name |
| [`render.capture_annotated`](render.capture_annotated.md) | yes | all six, over asset previews and levels |
| [`render.capture_open_level`](render.capture_open_level.md) | yes | world/actor for `framing` + `subject` only; it does **not** move the camera. Asset kinds refused |
| [`render.capture_ortho_tiles`](render.capture_ortho_tiles.md) | **no** | the level extent it is given; see below |

`render.capture_ortho_tiles` is **domain-named**: its level extent is its subject, so a `subject` would make the name lie. Other domains use `camera.orbit_shots` and `render.capture_asset_preview`.

Every refusal above is `UNSUPPORTED_ASSET_EDITOR` and names the replacement verb.

**A material or a texture is not a subject kind at all**, so no capture verb serves one. [`asset.generate_thumbnail`](asset.generate_thumbnail.md) is what renders them — offscreen to `outputPath`, needing no asset editor, no level and no viewport, with a material drawn onto a chosen `primitive` under caller-set `azimuth` / `elevation` / `zoom`. Binding the material to a static mesh and capturing *that* is a served kind and will be accepted — it just opens an asset editor for a picture that never needed one.

## A capability a kind cannot support is a typed refusal, not silence

Requesting a time series on a static mesh returns `UNSUPPORTED_ASSET_EDITOR` naming the missing time axis—not a still or opaque `UNKNOWN_PARAMS`. Refusals use existing codes: `UNSUPPORTED_ASSET_EDITOR`, `PREVIEW_VIEWPORT_NOT_FOUND`, `NO_ACTIVE_LEVEL_VIEWPORT`, `BOUNDS_EMPTY`, and `ACTOR_NO_SKELETAL_MESH_COMPONENT`.

See [`render.capture-time`](render.capture-time.md) for supported time axes, unconditional bind-pose refusal, `time` / `times`, `subjectTime`, and particle reproducibility limits.

## Why the ortho tile mosaic is not on this path

[`render.capture_ortho_tiles`](render.capture_ortho_tiles.md) uses `USceneCaptureComponent2D` plus a render target, not `FEditorViewportClient`. “A scene capture cannot do view modes” is false. Its `viewMode` writes the component's show flags; there is **no scope guard and none is needed** because the component is per-call and discarded, so `restored` is omitted.

| View-mode capability | On the scene-capture path | Why |
| --- | --- | --- |
| Show-flag-only modes: unlit, wireframe, lighting off, foliage off | **reachable, and reached** | The component's public writable `FEngineShowFlags ShowFlags` is written by this verb. |
| Shader complexity, quad overdraw, LOD/HLOD coloration, texture density | **reachable, and reached** | The engine selects this family from `EngineShowFlags`, not `ViewMode`. `viewMode.showFlagMismatches` lists every requested flag that did **not** read back, including an empty array, so each call is measured. |
| Buffer visualization, Substrate visualization and the other sub-visualisations | **structurally unreachable** | `FSceneViewFamily` leaves `ViewMode` at `VMI_Lit`; the scene-capture renderer never assigns it, and the only readers test those enumerators. No arguments reach them here. |
| Lightmap density, lit lightmap density, stationary-light overlap, collision (simple and complex) | **not implemented — not impossible** | `ApplyViewModeOverrides` early-outs unless `AllowDebugViewmodes()` and then swaps `GEngine` editor debug materials per batch; this path does neither. Earlier text blamed an “editor-scope show-flag override”; wrong: scope gates only `SetAudioRadius(false)`. Reach these with `editor.set_view_mode` + [`render.capture_open_level`](render.capture_open_level.md). |
| Two-slot perspective/orthographic mode with scoped restore | **not applicable** | A component keeps no persistent view state, so there is nothing to restore. |

**The result is reported elsewhere.** This verb emits no `viewport` block or `viewport.viewModeOverride`; its measured verdict is `showFlags.viewMode`. See [`render.view-modes`](render.view-modes.md) § `viewMode` means two things.

What does not cross is `viewMode` *application* scope: scoped override and two-slot restore require a viewport client. Name → show-flag *resolution* is shared, so the four refusal codes (`UNKNOWN_VIEW_MODE`, `VIEW_MODE_NOT_RENDERABLE`, `VIEW_MODE_NEEDS_COMPANION`, `VIEW_MODE_UNAVAILABLE`) mean the same thing here.

**The default is no override**, so existing pixels do not move. The verb warns when lighting/post-processing is off; requested `unlit` clears lighting intentionally, and the warning names that mode.

Both renderers share the blank classifier, opaque-alpha stamp, and luminance classifier. Exposure cannot be shared: a viewport writes `FEditorViewportClient::ExposureSettings`, while scene capture writes `FPostProcessSettings` with manual metering; at the same EV100 they differ by about 2.47 stops because one multiplies by middle grey and the other by 1.0.

## See also

- [`render`](render.md) — the capture verbs themselves, the view-mode contract and the two-renderer rule.
- [`render.capture-time`](render.capture-time.md) — the time axis a subject is captured over, and what a particle capture promises.
- [`render.capture-exposure`](render.capture-exposure.md) — exposure pinning and the cold-preview stop.
- [`render.preview-scene-rig`](render.preview-scene-rig.md) — an asset kind resolves to a preview scene, and `previewScene` is what lights it. Not reachable from a `world` or `actor` subject.
- [`asset.generate_thumbnail`](asset.generate_thumbnail.md) — the verb for a material or a texture, which no subject kind serves.
- [`camera`](camera.md) — framed, orbited and animation-burst captures of a level subject.
- [`visual-review`](visual-review.md) — choosing a capture surface and what counts as visual evidence.
