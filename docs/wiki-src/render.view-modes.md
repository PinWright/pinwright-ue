# render.view-modes

`viewMode` on a capture verb renders one capture in another mode and restores the viewport's previous mode. This page covers the vocabulary, four refusal codes, and measured `viewport.viewModeOverride`; capture verbs are documented in [`render`](render.md).

## Take a diagnostic view without moving the viewport

`viewMode` renders **one capture** in another mode and restores the previous mode on every exit path, including errors, in **both** viewport-client slots. `editor.set_view_mode` persists the mode and never restores it, so a diagnostic look there leaks into later captures and the editor. Use the parameter for a look; use the verb when you want the editor left in that mode.

Accepted on `render.capture_mesh`, `render.capture_asset_preview`, `render.capture_open_level`, `render.capture_annotated`, `render.capture_animation_preview` and all three `camera.*` verbs; multi-shot verbs read it **once for a whole shot set**, so every shot in one call uses the same mode. `render.capture_mesh` owns a transient scene-capture component instead of a viewport, so it has no persistent mode to restore and applies the scene-capture subset described below directly to that component's show flags.

**`front_back_face` is the visual counterpart to `health.signedVolume`, and the two are blind in opposite directions.** Both test an inside-out mesh, and a closed shell built inside-out renders **identically** to a correct one under normal lighting, which is how one shipped in this corpus and survived two investigations. `signedVolume` is the number — negative means reversed — but it is a **sum over the whole mesh**, so two equal components wound opposite ways cancel: measured on a two-box probe holding one correct and one inverted box, `geometry.audit_static_meshes` reported `signedVolume: 0.0` and flagged nothing, while the same asset under `front_back_face` came back one grey box and one green box. The picture is per pixel, so it localises what the number averages away. It has its own blind spot — Nanite, below — so take both.

**The colours, measured with a known-bad in every row: front faces are neutral grey, back faces are GREEN.** Not red. Measured through `render.capture_asset_preview` with `previewScene: {showFloor:false, showEnvironment:false}`, sampling the middle half of each frame; `greenExcess` is mean G minus the mean of R and B:

| Subject | provenance | tris | `signedVolume` | mean RGB | `greenExcess` | reads as |
| --- | --- | --- | --- | --- | --- | --- |
| Box, correct | `.pwmodel` compile | 12 | +1.0e6 | (121.9, 121.0, 121.4) | −0.7 | grey |
| Box, inverted | `.pwmodel` compile | 12 | −1.0e6 | (1.1, 119.0, 1.1) | **+117.8** | green |
| Cube, correct | stock `/Engine/BasicShapes` | 48 | +1.0e6 | (123.5, 120.4, 123.0) | −2.9 | grey |
| Cube, inverted | stock `/Engine/BasicShapes` | 48 | −1.0e6 | (3.3, 118.4, 3.3) | **+115.1** | green |
| Tree, correct | imported content | 2536 | +1.5e8 | (128.5, 120.0, 128.4) | −8.4 | grey |
| Tree, inverted | imported content | 2536 | −1.5e8 | (8.7, 128.0, 8.7) | **+119.3** | green |
| Tree, inverted, **Nanite on** | imported content | 2536 | −1.5e8 | (105.4, 89.4, 105.4) | **−16.0** | grey — **false pass** |

Each pair is one mesh through a single normal flip, so nothing but winding differs, and the winding is established by `signedVolume` rather than by the picture it is being used to explain. One asset and two cameras gives the same answer: a correct closed cube shot from outside is grey, and the identical asset shot with the camera **inside** it is green at `greenExcess` +116.3 — so the split is front-facing versus back-facing, not per-asset. **There is no triangle-count threshold and no dependence on how the mesh was authored**: 12, 48 and 2536 triangles across three provenances all separate by more than 100 points of `greenExcess`, and the only row that breaks the pattern is the Nanite one. Front faces also carry the mode's wireframe overlay, which is how a tinted-but-grey frame is told from an untinted one at close range.

**The blind spot is Nanite, and it fails silently: an untinted frame means UNKNOWN, never "correct".** The tint is produced entirely by a per-mesh-batch material substitution in `ApplyViewModeOverrides` (`PrimitiveDrawingUtils.cpp:1794-1805`), reached only from `FMeshElementCollector::AddMesh` (`SceneManagement.cpp:624-636`) — only for geometry gathered through the **dynamic** mesh-element path. `VMI_FrontBackFace` maps to `DVSM_None` (`SceneView.cpp:3299`), so it is not a debug-view *shader* and there is no second path to catch it. An ordinary static mesh reaches the substitution only because the mode forces `Lighting` off (`ShowFlags.cpp:592-604`), which makes `IsRichView()` true and flips the proxy to dynamic relevance (`StaticMeshSceneProxy.cpp:2456`). **A Nanite proxy does not honour `IsRichView`** — the term is commented out at `NaniteResources.cpp:1310-1311` under the engine's own note *"Nanite doesn't respect rich view enabling dynamic relevancy"* — so it keeps static relevance, rasterises through `Nanite::DispatchBasePass` with its real materials, and is never tinted. Nanite's own debug-view selector (`BasePassRendering.cpp:1324-1357`) has no case for this mode.

Demonstrated, not inferred: the inverted 2536-triangle mesh above, duplicated and rebuilt with **Nanite enabled and nothing else changed**, went from `greenExcess` +119.3 to **−16.0** with zero green pixels — pixel-indistinguishable from the correct mesh, while its `signedVolume` still read −1.5e8. **A grey frame is evidence of correct winding only when the subject is known not to be Nanite.** Check the asset's Nanite flag first, or turn Nanite off for the diagnostic; `viewModeOverride.applied` reads `true` either way and cannot tell you. `clay`, `zebra` and `random_color` share the one substitution site and inherit the same blindness.

`render.capture_asset_preview` reports the pairing: when one of those four modes is requested for Nanite geometry, the response carries `viewModeNaniteBlindSpot: true` and a `viewModeNaniteWarning` naming the mechanism. It is a warning, not a refusal — `clay` and `zebra` still show the surface, `random_color` is merely uninformative, and only `front_back_face` turns the gap into a wrong answer.

The backdrop does **not** hide the subject, contrary to an earlier claim on this page. With the default preview scene the floor and environment are drawn under the same substitution and carry the overlay, which is clutter; measured on a correct cube at the default preview scene, `greenPixelFrac` was 0.000 and the cube was plainly visible against it. `previewScene: {showFloor:false, showEnvironment:false}` removes the clutter and is worth passing, but a frame that is uniformly one colour is a reason to check `framing.boundsInFrame` and the subject's Nanite flag, not a documented pass.

The other four that pay for themselves here:

| Mode | What it shows | Why it matters |
| --- | --- | --- |
| `unlit` | Materials with no lighting at all. | Kills a whole class of false positives in one call — shadow acne read as boolean garbage, a cast shadow read as inverted normals, Lumen final-gather noise read as surface damage. Five rendering artifacts have been misdiagnosed as geometry defects here. |
| `random_color` | Gives separate primitive components different colours. | A detached island among separate static-mesh components shows instantly: a model shipped with its head floating 3.6 uu off the neck while reporting `isClosed: true` and `boundaryEdges: 0`, because each island is closed on its own. It does NOT distinguish sections or islands inside one skeletal-mesh component -- that is one primitive component and renders uniformly, so a skeletal mesh comes back flat and tells you nothing. |
| `zebra` | Simulated reflection stripes over the surface. | Surface continuity and faceting. A visibly faceted handrail took a multi-probe A/B to isolate; a zebra shot shows it at a glance. |
| `clay` | A neutral built-in material. | Removes the material from the question. Grid-textured defaults have repeatedly been read as geometry defects, to the point that an agent hand-authored a grey material for exactly this. |

Spelling is case- and separator-insensitive over the engine's own `EViewModeIndex` names, so `front_back_face`, `FrontBackFace` and `VMI_FrontBackFace` are one request, and every legacy key (`Lit`, `Unlit`, `Wireframe`, `DetailLighting`, `LightingOnly`, `LightComplexity`, `ShaderComplexity`, `LightmapDensity`, `StationaryLightOverlap`, `ReflectionOverride`, `collisionSimple`, `collisionComplex`) still works. The vocabulary is derived from the engine enum by reflection rather than hand-listed, so it cannot fall behind the engine — and the key a capture reports in `viewport.viewModeKey` is always a key a request accepts.

**Nothing falls back to Lit silently.** Four kinds of request are refused with their own error code instead of rendering a plausible picture of something else:

| Error | When | What to do |
| --- | --- | --- |
| `UNKNOWN_VIEW_MODE` | No enumerator matches, or the name is a sentinel (`VMI_Unknown`, `VMI_Max`) or a hidden/deprecated one (`VMI_Lit_Wireframe`). | Read the recognised list in the message. |
| `VIEW_MODE_NOT_RENDERABLE` | **Two different instance classes now.** (1) The mode sets no show flag that Lit does not, so the frame would *be* a Lit frame with a different label — `GroupLODColoration` is a menu grouping item, not a render mode. (2) *This renderer* cannot draw it: the editor debug families on the scene-capture path, below. | (1) Use `LODColoration` / `HLODColoration`, or `Lit`. (2) Take it through `editor.set_view_mode` + [`render.capture_open_level`](render.capture_open_level.md). |
| `VIEW_MODE_NEEDS_COMPANION` | The mode renders a sub-visualisation chosen separately: `VisualizeBuffer`, `VisualizeNanite`, `VisualizeLumen`, `VisualizeMegaLights`, `VisualizeSubstrate`, `VisualizeGroom`, `VisualizeVirtualShadowMap`, `VisualizeVirtualTexture`, `RayTracingDebug`, `VisualizeGPUSkinCache`. | Pick the target in the editor's viewport menu once, then re-issue — a pre-selected target is accepted and named back in `viewModeOverride.companionMode`. PinWright will not set it: validating the name needs a different engine registry per mode, and an unvalidated one renders the overview default. |
| `VIEW_MODE_UNAVAILABLE` | The engine's own `EngineShowFlagOverride` strips the flag the mode depends on before the frame is drawn — what happens to `PathTracing` when ray tracing is disabled. | Enable the feature, or pick another mode. |

The response's `viewport.viewModeOverride` block is unconditional, so "no override was asked for" and "an override did nothing" are distinguishable:

| Field | Meaning |
| --- | --- |
| `requested` | An override was asked for at all. |
| `applied` | **Measured.** Both slots read back as the requested mode *and* every show flag that distinguishes the mode from Lit read back off the viewport. A successful call is not evidence on its own — that is exactly how `camera.orbit_shots` shipped a `viewMode` argument that changed nothing. |
| `restored` | **Measured.** Both slots came back to what they held before. `false` brings a `restoreWarning`: that capture changed the editor window, not just the next capture. |
| `previous` / `afterRestore` | `{perspective, orthographic}` each. A viewport client keeps two separate view-mode slots, so one number cannot describe the restore. |
| `showFlagsChecked` / `showFlagMismatches` | The show flags the mode writes differently from Lit, and any that did not read back. |
| `companionMode` | The sub-visualisation in force, when the mode has one. |

A capture taken in a non-lit mode **you asked for** still reports `lit: false` with a `viewModeWarning` — the frame genuinely cannot carry a material or colour judgement — but the warning names the scoped parameter and confirms the restore, instead of sending you to `editor.set_view_mode`.


## `viewMode` means two things, and only one of them has a viewport

Everything above describes the **seven viewport verbs**. [`render.capture_ortho_tiles`](render.capture_ortho_tiles.md) accepts the same argument, vocabulary and four refusal codes, but renders through a transient `USceneCaptureComponent2D`, not an `FEditorViewportClient`.

| | Seven viewport verbs | `render.capture_ortho_tiles` |
| --- | --- | --- |
| What is written | Both view-mode slots on the viewport client, perspective and orthographic | The component's own `FEngineShowFlags`, which become the view family's flags verbatim |
| Restore | Scoped guard, both slots, measured | **None, and none is possible.** The component is created per call and discarded, so `restored` is **omitted rather than faked** |
| Where the result is reported | `viewport.viewModeOverride` | `showFlags.viewMode` — this verb emits no `viewport` block at all |
| Verdict field | `applied` (a key) plus `showFlagsChecked` / `showFlagMismatches` | `applied` (a key, present only when every distinguishing flag read back), `flagsTook`, `distinguishingShowFlags`, `showFlagMismatches` — emitted **even when empty**, because an absent array reads as "not checked" — and `derived`, the engine's own `FindViewMode` read of the resulting flags |

**`derived` reads the flags, not the picture, and UE 5.8 leaves one flag on that makes the difference visible.** `FEngineShowFlags::Init` clears every hidden `Visualize*` flag except `VisualizeMegaLights` (`ShowFlags.h:409-415`), and `FindViewMode` tests that one above every lit mode (`ShowFlags.cpp:814`) — so an untouched game flag set derives back as `VisualizeMegaLights` even though nothing MegaLights-shaped is in the frame (the visualization also needs a companion mode name only an editor viewport writes). `render.capture_ortho_tiles` therefore clears it on the component it creates, and publishes `showFlags.visualizeMegaLights` so the clear is checkable from the response; with it clear, a burst that requested no `viewMode` reports `derived: "lit"`. If you build show flags yourself and read a mode back out of them, clear the hidden visualization flags first.

**The shared parameter description is written for the viewport case**, so on the ortho verb its two sentences about slots and restoring do not apply; that verb's own parameter text says so at the call site. Read the per-verb text, not only the shared prose.

**What the scene-capture path can and cannot reach, with the real obstacle named.** The show-flag families — unlit, wireframe, lighting off, foliage off, and the whole debug-view-shader family (shader complexity, quad overdraw, LOD/HLOD coloration, texture density) — are **reachable and reached**: the engine picks that family off `EngineShowFlags` and never consults the view family's `ViewMode`.

Two families are refused, for **different** reasons, and the difference matters:

- **`VisualizeBuffer`, `VisualizeSubstrate` and the other sub-visualisations are structurally unreachable.** `FSceneViewFamily`'s constructor leaves `ViewMode` at `VMI_Lit`, the scene-capture renderer assigns it nowhere, and the engine's only two readers test it against exactly those enumerators. There is no sequence of arguments that reaches them here.
- **`LightmapDensity`, `LitLightmapDensity`, `StationaryLightOverlap`, `collisionSimple` and `collisionComplex` are *not done*, not impossible.** The obstacle is `ApplyViewModeOverrides`, which early-outs unless `AllowDebugViewmodes()` and then substitutes `GEngine`'s editor debug materials per mesh batch — machinery the capture path does not run. It is a scope decision with a named cost, and it is recorded that way deliberately: an earlier draft of this reasoning blamed an "editor-scope show-flag override", which is wrong — that scope parameter gates exactly one line in the whole engine function (`SetAudioRadius(false)`) and nothing else reads it. Writing "cannot be done" where the truth is "not done, and here is what it would take" is a mistake this project keeps paying for.

Either refused family is reachable today through `editor.set_view_mode` plus [`render.capture_open_level`](render.capture_open_level.md).

**Any mode that clears `Lighting` or `PostProcessing`** — unlit, wireframe, the complexity modes — also stops an `exposure` pin from governing the pixels, on both paths. The response says so and names the requested mode as the cause rather than reporting it as a fault.

## See also

- [`render`](render.md) — the capture verbs, the `viewport` block, and why a wireframe frame is not a blank frame.
- [`camera`](camera.md) — `viewMode` read once for a whole shot set.
- [`editor.collision-review`](editor.collision-review.md) — the workflow that captures in a debug mode on purpose.
- [`render.capture-subjects`](render.capture-subjects.md) — which view-mode capabilities the scene-capture renderer can and cannot reach.
