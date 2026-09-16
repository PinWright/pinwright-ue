---
type: system
summary: "A scoped, restored, measured `previewScene` parameter that gives a capture control over the key light, the sky and the backdrop of an asset-editor preview scene, plus the report block that says what rig a frame was drawn under. Fixes the two engine-driven mutations a capture leaks today (a shared profile write and a committed-config write), adds `viewMode` to the ortho path, closes the one unstamped alpha path, and closes the verb-parameter parity gaps a domain matrix structurally cannot see. Seven file-disjoint chunks for one wave."
date: 2026-08-21
tags: [capture, render, camera, lighting, preview-scene, viewport, scoped-override]
---

# The preview-scene rig: lighting a capture without leaving anything behind

Ordering below is **hard dependency only**, per [format-decisions.md](format-decisions.md). There are no
milestones. Where two chunks are independent, that is stated and they run together.

Every claim in the originating brief was re-read against the tree and against `C:\UE_5.8\Engine` on
2026-08-21. §0 records each verdict. Six claims were corrected, one was refuted outright, and the
correction to it changes the design.

## Decisions

1. **The rig is a per-capture parameter, never a verb.** The user's ruling on `editor.set_view_mode`
   binds here: *"bad api editor.set_view_mode shouldnt be really used by ai agents. it causes side
   effects. we must integrate it into screenshot / orbit and such image capture functions, that do
   not cause these view modes to persist."* There is no `render.set_lighting`. There is one optional
   `previewScene` object on the capture verbs that already own a preview scene, applied by an RAII
   guard, restored on every exit path including errors, with the restore **measured and published**.
   The pattern is `FScopedExposurePin` (`PreviewViewportCaptureUtils.cpp:203-247`) and
   `FScopedViewModeOverride` (`.h:382-394` / `.cpp:1180-1204`), copied down to the five invariants
   in §2.3.

2. **The restore has three levels, and only the shallowest one is obvious.** A capture that changes
   the key light must put back (a) the light **components**, (b) the shared
   `UAssetViewerSettings::Profiles` array, and (c) the bytes of `Config/DefaultEditor.ini`. Level (b)
   is not optional bookkeeping: `FAdvancedPreviewScene::UpdateScene` compares `GetLightDirection()`
   against `Profile.DirectionalLightRotation` and, when they differ, **writes the component's
   rotation into the shared profile** (`AdvancedPreviewScene.cpp:176-188`). So a component-only
   override arms the config write it was supposed to avoid. Level (c) is measured, not assumed, and
   published as `viewport.previewScene.restore.configFileUnchanged`.

3. **Per-capture profile selection is refused, and the refusal is the cheaper answer.** §4.1. Every
   lighting outcome `Grey Ambient` produces is reachable through `key.intensity: 4.0` +
   `sky.intensity: 2.0` on `FPreviewScene`'s own `ENGINE_API` setters — which touch **only** the
   components, need **no** downcast, and emit **no** process-wide broadcast. `SetProfileIndex`
   broadcasts to every live preview scene in the editor (`AdvancedPreviewScene.cpp:267`), re-aims
   the key from the new profile (`:262`) so it fights the `key` block, and silently swaps the tone
   curve — **both** non-default shipped profiles carry `bEnableToneMapping=false`
   (`AssetViewerSettings.h:279, :291`, and verbatim in this project's committed
   `Config/DefaultEditor.ini:28-29`). The audit called this "the cheapest large win". It is the
   most expensive way to reach a result the `key` block already reaches.

4. **Azimuth and elevation are where the light ARRIVES from, and the engine default proves the
   conversion.** `FPreviewScene::GetLightDirection()` returns the component's **+X** axis
   (`PreviewScene.cpp:264-272`) and `ULightComponent::GetDirection()` is negated at every shading
   site (`DirectionalLightComponent.cpp:396`, `:820`) — +X is the direction light *travels*. So
   `Rotation = (pitch = -elevation, yaw = azimuth - 180, roll = 0)`, and the shipped default
   `(-40, -67.5, 0)` is **arrival azimuth 112.5°, elevation 40°**. That number is independently
   corroborated by measurement: `visual-review.model-rig.md:52` re-measured the lit band on
   2026-08-21 and put the key "near azimuth **110°**", with the whole facing surface lit at 85/110/130
   and a black silhouette at −40 and −95. Two methods, 2.5° apart. The report publishes the raw
   `FRotator` **and** the derived arrival pair, so nobody redoes the trig.

5. **`applied` and `restored` are read back off the live scene; nothing is echoed.** This is the rule
   that already exists (`PreviewViewportCaptureUtils.cpp:1542-1545`: *"A response that only asserted
   the call succeeded is how `camera.orbit_shots`' `viewMode` shipped inert"*) and the reason the
   report block is unconditional — "no rig was asked for" and "a rig was asked for and did nothing"
   must be distinguishable.

6. **No new error codes.** `ERR_INVALID_ARGUMENT` (`ErrorCodes.h:466`) covers a malformed
   `previewScene` payload; `ERR_UNSUPPORTED_ASSET_EDITOR` (`:1089`) covers a rig request against a
   viewport with no advanced preview scene; `ERR_UNKNOWN_VIEW_MODE` / `ERR_VIEW_MODE_NOT_RENDERABLE`
   / `ERR_VIEW_MODE_NEEDS_COMPANION` / `ERR_VIEW_MODE_UNAVAILABLE` (`:1084, :1133, :1130, :1137`)
   already exist for R3's ortho `viewMode`. This keeps `ErrorCodes.h` and the regenerated
   `docs/error-code-catalog.md` out of every chunk's file list. If a chunk believes it needs a code,
   it stops and the plan is amended — it does not add one. §7.

7. **The two defects are one fix.** Defect 1 (a Niagara viewport writing `bShowFloor=false` into the
   shared profile) and Defect 2 (an asset-editor close flushing that profile to a committed config
   file) are the same mutation observed at two depths. Snapshotting and restoring
   `UAssetViewerSettings::Get()->Profiles` around the capture disarms **both**, plus decision 2's
   light-direction write-back, with one mechanism. §3.

8. **A rig report is emitted even when no rig was requested, and even on verbs that have no preview
   scene** — with `sceneAvailable: false`. `viewport` is built by **one** serializer for every
   viewport-owning verb (`MakeViewportInfoObject`, `PreviewViewportCaptureUtils.cpp:2345-2358`), and
   `render.md:29` states the rule: *"a verb-by-verb difference in this block is a bug, not a design."*
   Suppressing the block on level-viewport verbs would be that difference.

9. **`previewScene` is parsed by the shared parser and must be actively refused where it is not
   declared.** `ParseViewportCaptureRequest` (`PreviewViewportCaptureUtils.cpp:1259`) serves
   `capture_asset_preview`, `capture_open_level` and `capture_annotated` alike, so adding the field
   there makes `capture_open_level` accept it silently — precisely bug 1 of the previous wave
   (*"accepts three undeclared parameters"*). R2 clears the parsed pin on the level-viewport path the
   way `RenderHandler.cpp:386-395` already clears `viewDistanceScale` and `hideEditorSprites`.

10. **Parameter parity gets a test, not a one-off fix.** The `padding`/`radius` inversion is real
    (§5) but it is an instance. The wave that just landed could not have caught it because both its
    matrices used the same axis — subject domain — and a fully green row is compatible with a welded
    constant. R7 writes the cross-verb `FParamSpec` walk that turns "these verbs share a helper" into
    a machine-checkable assertion.

---

## §0 Verification of the brief

Every row was read on 2026-08-21 against the working tree at `bc951ea1` (plugin) / `4a59686` (host),
and against `C:\UE_5.8\Engine`. The qmd `ue` collection was not consulted: it is indexed against 5.7.
Several docs cited below are uncommitted (` M`) and may be under concurrent edit — re-read before
cutting.

| Claim in the brief | Verdict | Evidence |
|---|---|---|
| `FPreviewScene` registers a directional light, a sky light and a line batcher at `PreviewScene.cpp:82-100` | **True, exact** | Real path is `Runtime/Engine/Private/PreviewScene.cpp`, not `Editor/UnrealEd`. The whole block — line batcher included — is gated on `CVS.bDefaultLighting` |
| `ConstructionValues` defaults `LightRotation(-40,-67.5,0)`, `SkyBrightness 1.0` | **True, and incomplete in a way that matters** | `PreviewScene.h:23-37`. The struct is `ConstructionValues`, not `ConstructValues`. The claim omits **`LightBrightness = UE_PI` (≈3.14159), not 1.0** — and `FAdvancedPreviewScene`'s constructor overwrites all four from the profile (`AdvancedPreviewScene.cpp:58-104`), so these defaults are dead for every scene this plan touches |
| The key arrives from azimuth 112.5, elevation 40 | **True, and derived twice** | `PreviewScene.cpp:264-272` returns the +X axis; `DirectionalLightComponent.cpp:396, :820` negate it for shading. Trig in decision 4. Corroborated empirically at `visual-review.model-rig.md:52` ("near azimuth 110°") |
| Niagara re-aims to `(-40, +128, 0)` | **True** | `SNiagaraSystemViewport.cpp:869-877` (not 874-877). Arrival azimuth −52°, elevation 40° — the opposite half of the sphere from a static-mesh preview |
| The profile index comes from `UEditorPerProjectUserSettings::AssetViewerProfileIndex`, per-user | **True** | `AdvancedPreviewScene.cpp:49-51` (not 49-52). Out of range falls back to **0 silently**; the `ensureMsgf` at `:51` can only fire on an empty array. On this machine the value is `0` → "Epic Headquarters" (`Saved/Config/WindowsEditor/EditorPerProjectUserSettings.ini:23`) |
| `ULocalProfiles` (per-user) / `USharedProfiles` (`defaultconfig` → committed `Config/DefaultEditor.ini`) | **True** | `AssetViewerSettings.h:309-317` and `:319-335`. `GetDefaultConfigFilename()` (`Obj.cpp:3986`) resolves `ClassConfigName` `Editor` → `Config/DefaultEditor.ini`. **All three shipped profiles carry `bSharedProfile=True`**, so all three route to the committed file; `Saved/Config/WindowsEditor/Editor.ini` holds no profiles at all (95 bytes, three lines) |
| Two machines produce different pixels for the same call and no response field says so | **True, and this is the plan's motivating fact** | `visual-review.md:32`: two meshes at the same pinned `ev100` came back at mean luminance **0.47** and **0.18**, because "a preview scene belongs to the editor showing it, with its own environment and light rig". `render.md` has **zero** occurrences of `environment`, `background`, `cubemap`, `skylight` or `HDRI` |
| **Defect 1** — `SNiagaraSystemViewport::Construct` calls `SetFloorVisibility(false)` with `bDirect` defaulted false, writing the process-wide profile and broadcasting | **True, exact** | `SNiagaraSystemViewport.cpp:872`; `AdvancedPreviewScene.h:59` (default `false`); `AdvancedPreviewScene.cpp:391-409` (not 391-408) writes `Profiles[CurrentProfileIndex].bShowFloor` then `PostEditChangeProperty`. `SetEnvironmentVisibility` (`:411-427`) is identical in shape; `HandleTogglePostProcessing` (`:533-544`) has the same persistence and **no `bDirect` escape hatch at all** |
| **Defect 2** — capturing then closing an asset editor writes a committed project config file | **True, but latent on its own — Defect 1 is what arms it** | `SAdvancedPreviewDetailsTab.cpp:38-48` (`Save()` at `:46`, not 37-47). `UAssetViewerSettings::Save` (`AssetViewerSettings.cpp:118-146`) has **no dirty check, no early-out, no guard of any kind**. But `FConfigFile::WriteInternal` (`ConfigCacheIni.cpp:2693-2698`) compares the **whole rendered file text** to disk and skips the write when identical. So `Config/DefaultEditor.ini` churns **only when a profile value actually differs**. The untracked `Saved/Config/WindowsEditor/Editor.ini` has no such compare (`ConfigCacheIni.cpp:2627` passes `OriginalContents = nullptr`) and is rewritten on **every** `Save()` |
| The serialized block already exists at `Config/DefaultEditor.ini:26-29` | **True; the range is 26-30** | Section header `:26`, three `+Profiles=` lines `:27-29` (19,477 / 19,476 / 19,439 chars — a 31-line, 59,273-byte file), `Version=ApplyNewGridMaterial` `:30`. **Tracked, and byte-identical to HEAD**: `git hash-object` = `git rev-parse HEAD:Config/DefaultEditor.ini` = `bb1e00ab682b3ca742f66c6f3251fec36c07801b`. Touched by exactly one commit ever, `f3f9be7` (repo init) |
| `render.capture_asset_preview`'s `closeAfterCapture` defaults to TRUE | **True, and it is three-state, not two** | `RenderHandler.cpp:309` declares it; `:552-556` reads it into `bCloseAfterCapture` + `bCloseAfterCaptureProvided`. Absent → close only a window this call opened. Explicit `true` → close one that was already open. Explicit `false` → leave open. `CaptureSubject.cpp:1234-1260` implements it and **reads the close back** rather than assuming it |
| `Grey Wireframe` disables environment, floor, post-processing and tone mapping; `Grey Ambient` uses 4.0 / 2.0 | **True, both** | **Not in `BaseEditor.ini`** — they are C++ defaults in `UDefaultEditorProfiles` (`AssetViewerSettings.h:261-296`; "Grey Wireframe" is internally `EditingProfile`). Verified independently against the committed ini. Two fields the claim omits: `Grey Wireframe` sets `bShowMeshEdges=true`, and **both** grey profiles set `bEnableToneMapping=false` |
| Default-camera asset previews come back near-black: 0.174 / 0.143 / 0.167 / 0.148 vs 0.255–0.351 | **True; the opposed column is four values, not a range** | `visual-review.model-rig.md:58-63`, all at `exposure:{mode:"fixed", ev100:-1}`, 800×800. Opposed: 0.351 / **0.285** / 0.255 / 0.255 — the relay dropped the interior value. The stronger measurement is `render.md:109`: mean luminance **0.587 from +Y and 0.036 from −Y** at `orthoWidth: 420`, a sixteenfold difference with no content change |
| Three of five false geometry-bug reports are attributed to it | **True as written, and the document contradicts itself** | `visual-review.model-rig.md:65` says "three of the five false bugs". But `:5` is `## What was actually wrong, six times`, the table at `:11-16` has six rows, and `:18` says "Four of the six are the *material*. Two are the *shadow*." Five vs six inside one page. **GAP** — R6 reconciles it |
| The documented workaround is to rewrite and recompile the model asset rotated 180° | **Half true, and the half that is wrong is the dangerous half** | `visual-review.model-rig.md:56` says **copy the `.pwmodel` to scratch** and compile to `/Game/PinWrightScratch/` — the shipped asset is never rewritten. Two caveats the relay dropped: a part header with an existing `rotate=` must have 180 **added** to its yaw, not replaced, and an off-axis `at=` must be mirrored |
| The backdrop defeats blankness detection: byte-identical pure-backdrop PNGs, `blank:false`, `litPixelFraction:1` | **True; the location is `render.md:98`, not 190-192** | Lines 190-192 are the tail of a JS example. Two drifts: it was **one** mis-aimed camera over two calls, and the pair was "**nearly** compared", not compared. The remedy already shipped — the `framing` block, `render.md:100-105`, `:210` |
| `ExposurePinGovernsFrame` does not test `Tonemapper`, so a profile may make the pin a no-op while reporting `pinned:true` | **First half true, second half FALSE — and this is the brief's one refuted claim** | The omission is real: `PreviewViewportCaptureUtils.cpp:318-345` tests `EngineShowFlags.PostProcessing`, `EngineShowFlags.Lighting` and `bLitViewMode`, and the file contains **zero** reads of `EngineShowFlags.Tonemapper`. But it does not need one. §4.2 |
| `viewMode` on `capture_ortho_tiles` is "documented as reachable and simply unimplemented" | **Corrected — the claim fuses two adjacent table rows** | `render.capture-subjects.md:105` marks show-flag-only modes **`reachable`**, full stop, "this verb already writes it". *"reachable in principle, not implemented"* is `:106` and belongs to a different family (shader complexity, quad overdraw, LOD coloration, texture density). The operative sentence is `:101`: *"It has no `viewMode` argument and reports none"* — the gap is the **caller-facing argument**, not the mechanism |
| `WidgetDesignerCaptureUtil.cpp:148-158` is the only capture path with no `ForceOpaqueAlpha`, inconsistent with twelve others | **True on the gap; the line range and the count are both wrong** | Observed `:149-159` — `ReadPixels` straight into `PNGCompressImageArray` with nothing between. **Eight** production call sites exist, not thirteen. And **two** verbs reach the unstamped util, not one: `widget.screenshot_designer` with `target:"preview"` **and** `asset.dump`'s widget aspect (`AssetDumpHandler.cpp:722`). The same verb's `target:"window"` branch **does** stamp (`WidgetDesignerScreenshotHandler.cpp:310`), which is how this reads as fixed |
| `property.set` resolves arbitrary objects via bare `FindObject` and calls `PostEditChange()` | **`PostEditChange()` true; "bare `FindObject`" FALSE** | `UtilityPropertyHandler.cpp:1129` calls `PostEditChange()` with no `FPropertyChangedEvent`. But `ResolveObjectForProperty` (`:89-173`) is a **four-stage** resolver — `FindObject`, `LoadAsset`, `StaticLoadObject`, `FindActorByName` — each with an `IsAncestorFallback` rejection that exists specifically to prevent UE's `ResolveName` from climbing to an outer and returning the wrong object with `ok:true`. §4.6 |
| `camera.orbit_shots` hardcodes `Padding = 1.15f` while `camera.frame_actor` exposes it and hardcodes the distance | **True; two of four line numbers off by ~10** | `:541` and `:367` are exact. The params are at `:517` and `:193` (the relay pointed at the `REGISTER_RPC_HANDLER` lines). `ComputeFitDistance` is `CameraShotPlanUtils.h:146-151`, with **seven** call sites across five verbs. §5 |
| The convergence wave's matrix could not have caught it | **True, structurally** | `capture-subject-convergence.md:102` and `:120` — both matrices use the same column axis, the six subject domains. Rows `:122` and `:123` are identical apart from the world column; both go fully green once C8 lands, with the constant still welded. The word `padding` does not appear in the 811-line document |

### Two claims nobody made that the plan depends on

| Fact | Evidence |
|---|---|
| The plugin can reach the preview scene from a viewport client with **no new API** | `FEditorViewportClient::GetPreviewScene()` is a **public inline accessor** returning `FPreviewScene*` (`EditorViewportClient.h:363-366`). The plugin already calls it at three sites (`CaptureSubjectProviders_Mesh.cpp:74`, `_Animation.cpp:100`, `_Niagara.h:92`) — for `GetWorld()` only, never downcast |
| The key light and sky light are **public members**, so the rig is readable without a downcast | `PreviewScene.h:119-120`: `class UDirectionalLightComponent* DirectionalLight;` / `class USkyLightComponent* SkyLight;` sit above the `private:` at `:122`. Intensity, colour and rotation are all readable off them, and `SetLightDirection`/`SetLightBrightness`/`SetLightColor`/`SetSkyBrightness`/`SetSkyCubemap` are all `ENGINE_API` (`:104-110`), in a module PinWright already depends on publicly |

---

## §1 Target matrix

`✅` reachable after this wave · `▲` gap, chunk id given · `⛔` structurally impossible, reason
measured · `GAP` unverified. No cell is marked `⛔` for "unchecked".

### 1a. Rig capability × capture verb — the acceptance criterion

Only verbs that drive an `FEditorViewportClient` over an `FAdvancedPreviewScene` can carry a rig.
Rows 3, 5, 6 reach one **conditionally**, when `subject` names an asset kind
(`CaptureSubject.cpp:46-51`).

| Capability | `capture_asset_preview` | `capture_animation_preview` | `capture_annotated` | `frame_actor` | `orbit_shots` | `capture_open_level` | `animation_shots` | `capture_ortho_tiles` |
|---|---|---|---|---|---|---|---|---|
| key aim (arrival azimuth/elevation) | ▲ R1+R2 | ▲ R1+R2 | ▲ R1+R2 (asset subject) | ▲ R1+R5 (asset subject) | ▲ R1+R5 (asset subject) | ⛔ no preview scene | ⛔ level viewport only | ⛔ no viewport client |
| key intensity / colour | ▲ R1+R2 | ▲ R1+R2 | ▲ R1+R2 | ▲ R1+R5 | ▲ R1+R5 | ⛔ | ⛔ | ⛔ |
| sky intensity | ▲ R1+R2 | ▲ R1+R2 | ▲ R1+R2 | ▲ R1+R5 | ▲ R1+R5 | ⛔ | ⛔ | ⛔ |
| `showFloor` / `showEnvironment` | ▲ R1+R2 | ▲ R1+R2 | ▲ R1+R2 | ▲ R1+R5 | ▲ R1+R5 | ⛔ | ⛔ | ✅ today — `ShowFlags` is written directly (`OrthoTileCaptureUtils.cpp:239`) and reported (`:512-534`) |
| profile **selection** | ⛔ §4.1 | ⛔ §4.1 | ⛔ §4.1 | ⛔ §4.1 | ⛔ §4.1 | ⛔ | ⛔ | ⛔ |
| profile **report** (name, index) | ▲ R1 | ▲ R1 | ▲ R1 | ▲ R1 | ▲ R1 | ✅ R1 emits `sceneAvailable:false` | ✅ same | ⛔ block is on `viewport`, which this verb has none of |
| rig report (key, sky, floor, env, tonemapper, eye-adaptation) | ▲ R1 | ▲ R1 | ▲ R1 | ▲ R1 | ▲ R1 | ✅ R1, `sceneAvailable:false` | ✅ same | ⛔ same |
| restore measured at component level | ▲ R1 | ▲ R1 | ▲ R1 | ▲ R1 | ▲ R1 | n/a | n/a | n/a — component is transient and discarded |
| restore measured at shared-profile level | ▲ R1 | ▲ R1 | ▲ R1 | ▲ R1 | ▲ R1 | ▲ R1 (Defect 1 can fire without a rig request) | ▲ R1 | ⛔ |
| restore measured at config-file level | ▲ R1 | ▲ R1 | ▲ R1 | ▲ R1 | ▲ R1 | ▲ R1 | ▲ R1 | ⛔ |
| `viewMode` scoped override | ✅ today | ✅ today | ✅ today | ✅ today | ✅ today | ✅ today | ✅ today | ▲ R3, show-flag family only (§4.3) |
| exposure pin | ✅ today | ✅ today | ✅ today | ✅ today | ✅ today | ✅ today | ✅ today | ✅ today, different mechanism (`OrthoTileCaptureUtils.cpp:256-280`) |
| `ForceOpaqueAlpha` | ✅ today | ✅ today | ✅ today | ✅ today | ✅ today | ✅ today | ✅ today | ✅ today (`OrthoTileCaptureHandler.cpp:579`) |

### 1b. The `viewport.previewScene` report block

Unconditional on every verb that emits `viewport`, per decision 8. `applied` and `restored` are
**measured**, never echoed.

| Field | Meaning |
|---|---|
| `sceneAvailable` | `GetPreviewScene()` returned non-null. `false` on every level-viewport verb, and the only field present when it is |
| `advancedScene` | The scene is an `FAdvancedPreviewScene` (allow-listed viewport widget, §2.4). `false` disables the profile/floor/environment fields rather than zeroing them |
| `profileName` / `profileIndex` | **Measured** via `GetCurrentProfile()` / `GetCurrentProfileIndex()`. This is the field that makes two machines' captures comparable — or provably not |
| `key.rotation` / `key.azimuth` / `key.elevation` | Raw `FRotator` and the derived arrival pair (decision 4) |
| `key.intensity` / `key.color` / `key.colorLinear` | Read off `DirectionalLight` |
| `sky.intensity` / `sky.visible` / `sky.cubemap` | Read off `SkyLight` and the profile |
| `showFloor` / `showEnvironment` | Read off the profile; `advancedScene` only |
| `rotateLightingRig` | `true` means `Tick` re-aims the key every frame **and writes it back into the shared profile** (`AdvancedPreviewScene.cpp:285-294`) — a capture taken here is not reproducible and the report must say so |
| `showFlags.postProcessing` / `.tonemapper` / `.eyeAdaptation` | The three flags the profile drives that decide whether an exposure pin governs the frame and what tone curve it lands on (§4.2) |
| `requested` / `applied` / `restored` | The canonical triple |
| `captureUpdated` | The sky-light and reflection-capture drain (`FPreviewScene::UpdateCaptureContents`) was **driven** for this preview world. `applied` is a statement about the **writes**; this is necessary for them to reach the pixels and is not on its own sufficient. `false` only when the preview world carries no `FSceneInterface`, since the drain is otherwise unconditional |
| `skyCaptureWarning` | Present only when `captureUpdated` is `false`. `warmup.settled` cannot substitute: the settle loop's pump runs no editor tick, so it cannot advance the work it would have to observe |
| `captureIncomplete` / `captureIncompleteWarning` | Post-drain `USkyLightComponent::HasSkyCapturesToUpdate()`. The engine re-queues a capture it could not finish while assets compile and retries no sooner than 5 s later (`SkyLightComponent.cpp:771-787`, `:837-860`), so a drain can run in full and change nothing. Emitted only when the drain ran and left something queued; the predicate is process-wide, so it can raise a false alarm from another window but never stays silent while this scene's capture is pending — which is why it is not folded into `captureUpdated` |
| `previous` / `afterRestore` | Full rig, both sides, so the restore is checkable by the caller and not only by us |
| `restore.profilesRestored` | The shared `UAssetViewerSettings::Profiles` array is byte-equal to entry |
| `restore.configFileUnchanged` / `restore.configFileDigest` | `Config/DefaultEditor.ini` digest at entry equals digest at exit |
| `rigWarning` / `restoreWarning` | Present only when the failure is real, naming the mechanism and the remedy |

---

## §2 The `previewScene` parameter

### 2.1 Wire shape

```jsonc
{
  "previewScene": {
    "key": { "azimuth": 110, "elevation": 40, "intensity": 4.0, "color": "#FFFFFF" },
    "sky": { "intensity": 2.0 },
    "showFloor": false,
    "showEnvironment": false
  }
}
```

Every field is optional; an omitted field is not written. An empty `previewScene: {}` is
`INVALID_ARGUMENT` — an object that asks for nothing is a caller mistake, not a no-op, and the same
reasoning already governs `exposure: {mode:"auto", ev100:5}`
(`render.capture-exposure.md`). Absent `previewScene` writes **nothing** and the capture is
byte-for-byte what it is today.

`azimuth`/`elevation` are the arrival pair (decision 4). Supplying one without the other is
`INVALID_ARGUMENT` naming both — a half-specified aim would silently keep the other half of a rig the
caller did not measure.

### 2.2 The frozen contract

Every chunk codes against these declarations. They are fixed here so the wave does not serialise on
R1 finishing first. New file `Handlers/Render/PreviewSceneRig.h`.

```cpp
namespace PinWrightPreviewSceneRig
{
    // Default-constructed writes NOTHING -- the contract FExposurePin::Unset and
    // FViewModePin(bRequested=false) already carry.
    struct FPreviewSceneRigPin
    {
        bool bRequested = false;

        bool   bKeyAimProvided       = false;
        double KeyAzimuthDegrees     = 0.0;   // where the key ARRIVES from
        double KeyElevationDegrees   = 0.0;
        bool   bKeyIntensityProvided = false;
        double KeyIntensity          = 0.0;
        bool   bKeyColorProvided     = false;
        FColor KeyColor              = FColor::White;   // SetLightColor takes FColor, not FLinearColor

        bool   bSkyIntensityProvided = false;
        double SkyIntensity          = 0.0;

        bool bShowFloorProvided       = false;
        bool bShowFloor               = true;
        bool bShowEnvironmentProvided = false;
        bool bShowEnvironment         = true;

        bool WantsRig() const { return bRequested; }
    };

    // Every field READ BACK off the live scene. Nothing here is echoed from a request.
    struct FPreviewSceneRigReport
    {
        bool     bSceneAvailable = false;
        bool     bAdvancedScene  = false;
        FString  ProfileName;
        int32    ProfileIndex    = INDEX_NONE;

        FRotator KeyRotation     = FRotator::ZeroRotator;
        double   KeyAzimuthDegrees   = 0.0;
        double   KeyElevationDegrees = 0.0;
        double   KeyIntensity    = 0.0;
        FColor   KeyColor        = FColor::White;

        double   SkyIntensity    = 0.0;
        bool     bSkyVisible     = false;
        FString  SkyCubemapPath;

        bool     bShowFloor         = false;
        bool     bShowEnvironment   = false;
        bool     bRotateLightingRig = false;

        bool     bPostProcessingShowFlag = false;
        bool     bTonemapperShowFlag     = false;
        bool     bEyeAdaptationShowFlag  = false;
    };

    bool ParsePreviewSceneRigPin(const TSharedPtr<FJsonObject>& Payload,
        FPreviewSceneRigPin& OutPin, FString& OutErrCode, FString& OutErrMsg);

    // Never writes. Safe on a client with no preview scene: returns bSceneAvailable=false.
    FPreviewSceneRigReport MeasureRig(const FEditorViewportClient& Client);

    FRotator ArrivalToLightRotation(double AzimuthDegrees, double ElevationDegrees);
    void     LightRotationToArrival(const FRotator& Rotation, double& OutAz, double& OutElev);

    // True when this viewport widget type is known to build an FAdvancedPreviewScene.
    // Exact-name allow-list, never a suffix match -- the rule CaptureSubject.cpp already follows.
    bool IsAdvancedPreviewViewport(const FEditorViewportClient& Client);

    class FScopedPreviewSceneRig
    {
    public:
        FScopedPreviewSceneRig(FEditorViewportClient& InClient, const FPreviewSceneRigPin& Pin);
        ~FScopedPreviewSceneRig();

        FScopedPreviewSceneRig(const FScopedPreviewSceneRig&) = delete;
        FScopedPreviewSceneRig& operator=(const FScopedPreviewSceneRig&) = delete;

        bool WasApplied() const { return bApplied; }
        const FPreviewSceneRigReport& GetPreviousRig() const { return PreviousRig; }
        bool    ProfilesRestored()   const { return bProfilesRestored; }
        FString ConfigDigestAtEntry() const { return ConfigDigestBefore; }
        FString ConfigDigestAtExit()  const { return ConfigDigestAfter; }

    private:
        FEditorViewportClient&           Client;
        FPreviewSceneRigReport           PreviousRig;
        TArray<FPreviewSceneProfile>     ProfilesBefore;   // whole array, index-agnostic
        FString                          ConfigDigestBefore;
        FString                          ConfigDigestAfter;
        bool                             bApplied          = false;
        bool                             bProfilesRestored = true;
    };
}
```

Additions elsewhere, all owned by R1:
`FViewportCaptureRequest::PreviewSceneRig` and `FPoseListCaptureRequest::PreviewSceneRig`;
`FViewportCaptureOutput::{PreviewSceneRigBefore, PreviewSceneRigAfter, bPreviewSceneRigRequested,
bPreviewSceneRigApplied, bPreviewSceneRigRestored, bSharedProfilesRestored, bConfigFileUnchanged,
ConfigFileDigest, PreviewSceneRigWarning, PreviewSceneRigRestoreWarning}`;
`MakePreviewSceneRigInfoObject(const FViewportCaptureOutput&)` attached in `MakeViewportInfoObject`;
and `PINWRIGHT_PREVIEW_SCENE_PARAM_DESC` beside `PINWRIGHT_EXPOSURE_PARAM_DESC`
(`PreviewViewportCaptureUtils.h:71`) and `PINWRIGHT_VIEW_MODE_PARAM_DESC` (`:94`).

### 2.3 The five invariants, copied from `FScopedExposurePin`

Read `PreviewViewportCaptureUtils.cpp:203-247` before writing a line of R1. The guard must obey all
five, and every one of them has a reason recorded in that file:

1. **Capture the previous state *before* the `WantsRig()` early return**, so `previous` is reported
   on the omitted-parameter path too.
2. **The omitted-parameter path writes nothing** and leaves `bApplied = false`.
3. **The destructor is a no-op unless `bApplied`.**
4. **Copy and assign are `= delete`.**
5. **`WasApplied()` + `GetPrevious*()` accessors**, and the restore is measured by an `ON_SCOPE_EXIT`
   declared *before* the guard object so it runs *after* the destructor —
   `PreviewViewportCaptureUtils.cpp:1596-1604` is the exact form.

### 2.4 Apply and restore order — the order is the whole design

**Apply**, in this order and no other:

1. Snapshot: `MeasureRig(Client)` → `PreviousRig`; `UAssetViewerSettings::Get()->Profiles` copied by
   value into `ProfilesBefore`; digest `FPaths::SourceConfigDir() / TEXT("DefaultEditor.ini")`.
   The path is **computed the way the engine computes it** (`Obj.cpp:3986`), never hardcoded.
2. Floor / environment, when asked: `SetFloorVisibility(false, /*bDirect=*/true)` and
   `SetEnvironmentVisibility(false, true)` — the `bDirect` branches touch **only** the component
   (`AdvancedPreviewScene.cpp:404-408`, `:422-426`) and never the profile. Note the engine's quirk:
   `bDirect=true` with `bVisible=true` cannot force-*show*, it ANDs with the profile's value. Hiding
   is unconditional, which is the direction we need.
3. Key and sky last: `SetLightDirection`, `SetLightBrightness`, `SetLightColor`, `SetSkyBrightness`.
   All four write only the component (`PreviewScene.cpp:275-324`) and none touches the profile.

**Restore**, in this order and no other:

1. **Components first** — put `SetLightDirection` back to `PreviousRig.KeyRotation`, then intensity,
   colour, sky, floor, environment.
2. **Shared profile second** — if `Profiles != ProfilesBefore`, assign `ProfilesBefore` back.
3. **Broadcast third, and only if step 2 actually changed something** —
   `OnAssetViewerSettingsChanged().Broadcast(NAME_None)`, which drives a four-way `UpdateScene` on
   every live preview scene (`AdvancedPreviewScene.cpp:609, :630`).
4. **Measure** — re-`MeasureRig`, re-compare `Profiles`, re-digest the config file.

**Why that order and not any other.** `UpdateScene` reads `GetLightDirection()` and, when it differs
from `Profile.DirectionalLightRotation`, writes the *component's* rotation into the *shared profile*
(`AdvancedPreviewScene.cpp:176-188`). Broadcast before restoring the component and the override is
laundered into the shared profile — which is then flushed to the committed config by the next
details-tab teardown. Restore the component first and `bLightDirChanged` is false, so the block does
not run at all. The same ordering is why step 3 is conditional: an unconditional broadcast costs a
full `UpdateScene` on every open asset editor for nothing.

**One mechanism, three defects.** Snapshotting the whole `Profiles` array also captures anything the
*engine* mutates during the capture — including `SNiagaraSystemViewport::Construct`'s
`SetFloorVisibility(false)` (Defect 1) when the capture is what opened that editor. Restoring it
disarms the config write (Defect 2), because `FConfigFile::WriteInternal` only writes when the
rendered text differs (`ConfigCacheIni.cpp:2693-2698`). The array copy is index-agnostic, so it needs
no downcast and no assumption about which profile the scene is on.

---

## §3 The two defects, stated precisely

### 3.1 Defect 1 — the shared-profile write

`FAdvancedPreviewScene::SetFloorVisibility(bVisible, bDirect = false)` writes
`DefaultSettings->Profiles[CurrentProfileIndex].bShowFloor` and calls `PostEditChangeProperty`
(`AdvancedPreviewScene.cpp:391-403`). `DefaultSettings` is the process-wide `UAssetViewerSettings`
CDO. `SNiagaraSystemViewport::Construct` calls it with the default (`SNiagaraSystemViewport.cpp:872`).
Every later static-mesh, skeletal-mesh, material and Persona preview in that session loses its floor.
`SetEnvironmentVisibility` (`:411-427`) and `HandleTogglePostProcessing` (`:533-544`) share the shape;
the last has no `bDirect` escape hatch at all.

**This is an engine defect PinWright cannot fix at the source.** R1 restores it instead, and the
restore is measured.

### 3.2 Defect 2 — the committed-config write

`UAssetViewerSettings::Save()` (`AssetViewerSettings.cpp:118-146`) is called from exactly **two**
places in the entire engine — `SAdvancedPreviewDetailsTab.cpp:46` and its copy-pasted Dataflow twin
`DataflowAdvancedPreviewDetailsTab.cpp:41` — both destructors. It has no guard of any kind: it empties
and rebuilds both profile arrays and issues three writes unconditionally.

`render.capture_asset_preview`'s `closeAfterCapture` defaults to `true`, so the ordinary capture
destroys the toolkit, its "Preview Scene Settings" tab, and therefore that widget.

### 3.3 What actually changes bytes — the correction that sets the priority

`TryUpdateDefaultConfigFile` (`Obj.cpp:4037-4054`) skips only when the file is **read-only**;
otherwise it always calls `UpdateSingleSectionOfConfigFile`. But that path ends in
`FConfigFile::WriteInternal` (`ConfigCacheIni.cpp:2693-2698`):

```cpp
    // don't write anything out if it didn't actually change
    if (OriginalContents != nullptr && Text == *OriginalContents)
    {
        Dirty = false;
        return true;
    }
```

and `UpdateSections` passes the on-disk file in for exactly that purpose (`ConfigCacheIni.cpp:7128`).
So:

- **Defect 2 alone is latent.** A `Save()` over unchanged profiles rewrites nothing; the committed
  file's mtime does not move.
- **Defect 1, or a key-light override, or `bRotateLightingRig`, is what arms it.** Any of them makes
  the rendered text differ, and then the **whole 59 KB file is rewritten** — `Save()` empties and
  rebuilds the array, so all three 19 KB lines are re-serialised and can reorder. The diff is not
  small.
- The comparison is on whole-file text, not per value: a float that round-trips differently, or a
  hand edit elsewhere in `DefaultEditor.ini`, also trips it.
- The untracked `Saved/Config/WindowsEditor/Editor.ini` has **no** content compare
  (`ConfigCacheIni.cpp:2627` passes `OriginalContents = nullptr`) and is rewritten on every `Save()`.
  That is noise, not a defect, and the plan does not chase it.

**Therefore the fix is decision 7's snapshot, not a write-blocker**, and the acceptance criterion is
a digest comparison rather than a "we did not call Save()" assertion — which would be untestable,
since we never call it.

---

## §4 What cannot be done, and why

### 4.1 Per-capture profile selection — refused, with four reasons

`FAdvancedPreviewScene::SetProfileIndex` exists (`AdvancedPreviewScene.h:63`) and persists nothing
(`AdvancedPreviewScene.cpp:253-268` writes no ini and calls no `Save()`). It is still refused:

1. **It fights the `key` block.** `:262` calls `SetLightDirection(Profile.DirectionalLightRotation)`,
   so a request carrying both `profile` and `key` has an order-dependent result and no field says
   which won.
2. **It broadcasts process-wide.** `:267` `Broadcast(NAME_None)` drives a full four-way `UpdateScene`
   on every live `FAdvancedPreviewScene` in the editor (`:609`, `:630`) — a capture reaching into
   windows it does not own.
3. **It silently changes the tone curve.** Both non-default shipped profiles set
   `bEnableToneMapping = false` (`AssetViewerSettings.h:279, :291`; committed at
   `Config/DefaultEditor.ini:28-29`), and `SetShowFlags` writes
   `ShowFlags.SetTonemapper(bEnableToneMapping)` (`:221`), selecting the gamma-only tonemapper
   permutation (`PostProcessTonemap.cpp:145-149`). Frames from two profiles at an identical pinned
   `ev100` are not photometrically comparable, and nothing in the request says so.
4. **It buys nothing the `key` block does not.** `Grey Ambient`'s entire lighting delta is
   `DirectionalLightIntensity 4.0` / `SkyLightIntensity 2.0`. That is
   `previewScene: {key:{intensity:4.0}, sky:{intensity:2.0}}` — component-only, no broadcast, no tone
   curve change, no downcast.

The profile is **reported** (§1b) so a caller can see which rig a frame was drawn under and pin
`key`/`sky` explicitly to make two machines agree. Changing the profile stays the human's job through
the editor's own control, which is the same division `editor.set_view_mode` versus the `viewMode`
parameter already draws.

### 4.2 The audit's "exposure hole" — refuted, and the real hazard named

`ExposurePinGovernsFrame` (`PreviewViewportCaptureUtils.cpp:318-345`) tests `PostProcessing`,
`Lighting` and lit-ness, and the file contains zero reads of `EngineShowFlags.Tonemapper`. **It does
not need one**, on three pieces of source evidence:

- `IsAutoExposureDebugMode` (`PostProcessEyeAdaptation.cpp:493-511`), the engine predicate the
  function deliberately mirrors rather than copies, does **not** list `Tonemapper`. Its two
  reachable-from-a-capture terms are `!EngineShowFlags.Lighting` (`:498`) and
  `!EngineShowFlags.PostProcessing` (`:510`) — exactly the two the plugin checks.
- `ShowFlags.Tonemapper == 0` selects the **gamma-only permutation** of the tonemapper
  (`PostProcessTonemap.cpp:145-149`); it does not skip the pass, which stays enabled at
  `PostProcessing.cpp:782` (`bTonemapEnabled = !bVisualizeMotionBlur`).
- The gamma-only branch still multiplies by the exposure scale —
  `Engine/Shaders/Private/PostProcessTonemap.usf:506`:
  `OutColor.rgb = pow(SceneColor.rgb * (OneOverPreExposure * GlobalExposure), InverseGamma.x);`
  against `:523`'s full path using the same `OneOverPreExposure * GlobalExposure` factor.

So `pinned: true` under `Grey Ambient` is **honest**. What gamma-only removes is the film curve, the
grading LUT, bloom, vignette and local exposure — the *response curve*, not the *exposure*.

**The real hazard is a different flag, and it is one the plugin does not report.**
`bPostProcessingEnabled = false` — set on **both** grey profiles — routes through
`FPreviewSceneProfile::SetShowFlags` (`AssetViewerSettings.h:204-223`) to
`FEngineShowFlags::DisableAdvancedFeatures()`, which sets `SetEyeAdaptation(false)`
(`ShowFlags.h:206`). Note it does **not** clear `ShowFlags.PostProcessing`, so
`ExposurePinGovernsFrame`'s first check passes. An explicit `ExposureSettings.bFixed` still wins over
the `EyeAdaptation` flag (`PostProcessEyeAdaptation.cpp:635-682`, priority 2 above priority 3), so the
pin survives — but a caller relying on the profile's own `AutoExposureMinBrightness`/`MaxBrightness`
clamp instead gets it silently discarded.

**Decision: change nothing in `ExposurePinGovernsFrame`; report the three flags instead.** R1 adds
`showFlags.postProcessing`, `.tonemapper` and `.eyeAdaptation` to the rig block. Widening the
predicate would make it report `pinned:false` on a frame the pin does in fact govern — a false
negative traded for a false positive that does not exist.

**Residual, not settleable from source (§4.5):** a view with no `FSceneViewState` gets a hard-coded
ones eye-adaptation buffer (`SceneTextureParameters.cpp:77-87`), which is non-null, so the CPU
fixed-exposure fallback is skipped and even `bFixed` is ignored. Epic works around exactly this in
`TrackThumbnailUtils.cpp:66-74`. Whether an asset-preview client always has a view state is a
runtime question.

### 4.3 `render.capture_ortho_tiles` — what `viewMode` can and cannot reach

Corrected from `render.capture-subjects.md:99-113`. The show-flag-only family is **reachable**, not
"reachable in principle": the verb already writes `Component->ShowFlags`
(`OrthoTileCaptureUtils.cpp:239`) and already reports fourteen flags (`:512-539`). What is missing is
the caller-facing argument.

| Family | On the scene-capture path | Why |
|---|---|---|
| unlit, wireframe, lighting off, foliage off | ✅ R3 | `FEngineShowFlags ShowFlags` is a public writable member; the verb writes it today |
| shader complexity, quad overdraw, LOD/HLOD coloration, texture density | ▲ R3 if the engine's `ApplyViewMode` covers it; **GAP** until measured | The engine derives this family from show flags alone, but `ApplyViewMode` is written against an **editor** flag set and the component's flags start from the **game** set (`SceneCaptureComponent.cpp:168-169`). R3 measures which flags actually differ and reports the ones that did not take |
| buffer visualization, Substrate visualization | ⛔ | Needs `FSceneViewFamily::ViewMode` **and** a `ViewModeParam` name; the capture renderer writes neither |
| lightmap density, stationary-light overlap, lit wireframe, collision | ⛔ | Applied via `ApplyViewMode` + an **editor-scope** `EngineShowFlagOverride`; neither runs on this path |
| two-slot persp/ortho mode with scoped restore | ⛔ not applicable | A component keeps no persistent view state. There is nothing to restore, so `restored` is omitted rather than faked |

**There is no shared apply helper.** `PinWrightViewModes::Resolve` and `DistinguishingShowFlags` are
already viewport-free (`ViewModeVocabulary.h:122, :148`), but application lives in
`FScopedViewModeOverride`, which takes an `FEditorViewportClient&`. R3 calls the engine's
`ApplyViewMode(Pin.ViewMode, /*bPerspective=*/false, Component->ShowFlags)` directly. **No scope
guard is needed and none must be written**: the component is created per call
(`OrthoTileCaptureUtils.cpp:165`) and discarded.

**Default is no override, so existing pixels do not move** — `render.capture-subjects.md:111` names
that as the reason the resolution layer was not shared before.

One reconciliation R3 owns: `OrthoTileCaptureUtils.cpp:484-495` warns when
`!ShowFlags.Lighting || !ShowFlags.PostProcessing`. A requested `unlit` clears `Lighting` on purpose,
so the warning must name the requested mode instead of reading as a fault.

### 4.4 The widget-designer alpha gap — a decision, not just a missing call

`WidgetDesignerCaptureUtil.cpp:149-159` reads pixels and encodes with no `ForceOpaqueAlpha`. Two docs
promise otherwise in the same words — `camera.md:35` and
`level-building.capture-and-review.md:37`: *"Captures are stamped opaque on every path now."* That is
false for `widget.screenshot_designer` with `target:"preview"` and for `asset.dump`'s widget aspect.
The failure mode is already documented and has already cost days: a ~99.97 % transparent PNG that
reads as blank in any compositing viewer while the RGB is intact.

**But a widget's alpha is real content in a way a scene's is not.** A designer preview of a widget with
a transparent background legitimately has alpha 0 where nothing was drawn, and `FWidgetRenderer`
draws onto a transparent render target. Stamping destroys that information.

**Decision: stamp, and publish what was stamped over.** R4 measures `alphaZeroFraction` **before** the
stamp and reports it beside `opaqueStamped: true`, so a caller who wanted the transparency knows
exactly how much there was. This follows the ordering rule the codebase already enforces in the other
direction — `TestScreenshotBlankCapture.cpp:239-244`: *"`ForceOpaqueAlpha` destroys blankness — it
must run after the check."* Both `SceneCaptureProbeUtils.h:30-34` and `ZFightingAnalysis.h:174`
deliberately do **not** stamp; R4 must not touch either.

**This is the one item in the plan that would benefit from a user ruling** before implementation —
§ Unresolved questions.

### 4.5 What needs a running editor to settle

None of these blocks the wave; each is a cheap measurement that turns a `GAP` into a row. They must be
run **after** the wave lands and **not** by inference.

1. **Does a `Save()` over an unchanged profile move `Config/DefaultEditor.ini`'s mtime?** §3.3 says no
   from source, but `OriginalContents` is only non-null because `UpdateSections` passes the disk file
   in, and the sandbox `FConfigCacheIni(EConfigCacheType::Temporary)` path is indirect. Measure:
   digest the file, run `render.capture_asset_preview` with `closeAfterCapture: true`, digest again.
   R1's own report field answers it.
2. **Does the six-stop exposure signal survive under each of the three profiles?** Capture at
   `ev100 -1` and `ev100 5` under Epic Headquarters, Grey Wireframe and Grey Ambient with explicit
   distinct `filename`s. A signal well above the ~0.88 noise floor proves §4.2 empirically.
3. **Does an asset-preview viewport client always have an `FSceneViewState`?** The §4.2 residual.
4. **Which show-flag families actually take on `Component->ShowFlags` from a game-set base?** The R3
   `GAP` row in §4.3.
5. **Re-establish the suite baseline.** `CLAUDE.md:116` records 4051/4051/0 at `39a3eb2b`; three
   commits have landed since (`bcc334e8`, `c04375fc`, `bc951ea1`). Do not carry 4051 forward. §8.

### 4.6 `property.set` as an escape hatch — documented as unsupported, not gated

`property.set` reaches `UObject` CDOs by path, so `UAssetViewerSettings`' CDO is addressable today,
and `UtilityPropertyHandler.cpp:1129` calls a bare `PostEditChange()` — which
`UObject::PostEditChange` (`Obj.cpp:549-553`) turns into a `FPropertyChangedEvent(nullptr)`, hence
`NAME_None`, hence `bNameNone == true` at `AdvancedPreviewScene.cpp:609` and a **full four-way
`UpdateScene` on every live preview scene**.

The relayed description of the resolver was wrong: it is a four-stage resolver with an
`IsAncestorFallback` rejection at each stage (`UtilityPropertyHandler.cpp:89-173`), written
specifically against the "wrong object, `ok:true`" bug.

**Decision: document it as unsupported for preview-scene settings; do not gate it.** Gating means a
class allow-list on a general-purpose reflection verb, which is a much larger contract change than
this plan, and it would not be enforceable — the same mutation is reachable through any of the four
resolution stages under a different path spelling. R6 adds the note to `property.md` naming the
scoped parameter as the supported route, exactly as `render.view-modes.md` names `viewMode` against
`editor.set_view_mode`. **This is a knowingly incomplete answer and it is recorded as one.**

---

## §5 Parameter parity — the instance and the class

### 5.1 The reported defect, verified

`ComputeFitDistance(Radius, Fov, Padding)` (`CameraShotPlanUtils.h:146-151`) has **seven** call sites
across five verbs. Two of them expose opposite halves of it:

```cpp
// CameraFrameHandler.cpp — camera.frame_actor: exposes the margin, welds the distance
:193  RPC_PARAM_OPT("padding", "number", "Bounds-fit margin multiplier; >1 pulls the camera back…"),
:255  const float Padding = static_cast<float>(Ctx.GetNumber(TEXT("padding"), 1.15));
:367  const float Distance = ComputeFitDistance(Radius, Fov, Padding);   // unconditional, no override

// CameraFrameHandler.cpp — camera.orbit_shots: exposes the distance, welds the margin
:517  RPC_PARAM_OPT("radius", "number", "Orbit radius / camera distance. Defaults to a bounds-fit…"),
:540  // Orbit has no padding arg; use the same default margin as frame_actor.
:541  constexpr float Padding = 1.15f;
```

`:541` is the only `1.15f` literal in the whole `Private/` tree.

| call site | verb | distance override | padding |
|---|---|---|---|
| `CameraFrameHandler.cpp:367` | `camera.frame_actor` | **none** | param, default 1.15 |
| `CameraFrameHandler.cpp:778, :796` | `camera.orbit_shots` | `radius`, then `subject.radius` | **welded 1.15** (`:541`) |
| `AnimationShotsHandler.cpp:719` | `camera.animation_shots` | `radius` + `subject.radius` | param, default 1.15 |
| `AnimationPreviewCaptureHandler.cpp:808` | `render.capture_animation_preview` | **none** | param, default **1.4** |
| `RenderHandler.cpp:734, :754` | `render.capture_asset_preview` | **none** | **welded 1.25** (`:573`) |

`camera.animation_shots` is the only verb exposing both inputs of the helper it calls — which is the
proof this is oversight, not design: the union already ships.

**Three defaults for one concept, two of them unreachable from the wire, and it is now live
behaviour.** Post-convergence, `camera.frame_actor` serves the `skeletalMesh` and `animation` kinds
(`CaptureSubject.cpp:46-51`), so the *same* skeletal mesh frames at padding 1.15 through
`camera.frame_actor` and 1.4 through `render.capture_animation_preview`. `render.md:419` documents the
1.4 and explains it ("a limb reaching outside the rest bounds"); nothing documents that a caller
cannot reach it.

**`subject.radius` means two different things.** Parsed once (`CaptureSubject.cpp:572-576`) into one
`FSubjectRequest::Radius`, then read as a **camera distance** by `orbit_shots`
(`CameraFrameHandler.cpp:777`) and as a **sphere radius** by `frame_actor` (`:171-175`, `:345`). For
`subject:{kind:"world", point, radius:R}` at fov 50, orbit places the camera at `R` and frame_actor at
roughly `2.5·R`. Neither verb says so, and `render.capture-subjects.md:25` gives un-scoped advice to
"pass `padding`" in a sentence that qualifies only the radius half for `orbit_shots` — a reader
following it on the orbit path gets `UNKNOWN_PARAMS`.

### 5.2 The other parity gaps found in the same sweep

| # | Gap | Evidence |
|---|---|---|
| P1 | `padding` vs `radius` inverted between `frame_actor` and `orbit_shots` | `CameraFrameHandler.cpp:541`, `:367` |
| P2 | `capture_asset_preview` welds **both** — no `radius`, padding hardcoded 1.25 | `RenderHandler.cpp:573`, with a comment at `:571-572` showing the divergence was noticed and accepted per-verb |
| P3 | Three padding defaults (1.15 / 1.25 / 1.4), two unreachable | §5.1 |
| P4 | `orthoWidth` and free-camera `location`/`rotation` split along the namespace line: the three `render.*` still verbs expose both, all four bounds-fitting verbs expose neither. Declared as raw `FParamSpec` three times with no shared macro | `RenderHandler.cpp:296-298`, `:925-927`, `AnnotatedCaptureHandler.cpp:195-197` |
| P5 | `viewDistanceScale` is plumbed through `FPoseListCaptureRequest` (`PoseListCapture.h:134-136`), forwarded per frame (`PoseListCapture.cpp:38`), and **assigned by no handler** — while the wire parameter exists on two verbs that take the single-shot path instead. Dead on every path that declares it, undeclarable on every path that carries it. **The widest gap found** | as cited |
| P6 | `bWarmupShot` documents a caller affordance (`PoseListCapture.h:155-157`, "a caller that knows the viewport is warm can turn it off") with no wire spelling | as cited |
| P7 | `allowBlank` reaches one of five pose-capture verbs. `rejectBlank`'s absence is reasoned and documented (`CameraShotPlanUtils.h:308-311`); `allowBlank`'s is not covered by that rationale | `RenderHandler.cpp:300, :307` |
| P8 | `distribution`'s description is hand-copied four times with divergent prose and no shared macro | `CameraFrameHandler.cpp:522`, `AnimationShotsHandler.cpp:141`, `AnimationPreviewCaptureHandler.cpp:182`, `RenderHandler.cpp:313` |
| P9 | `capture_ortho_tiles`'s `exposure` forks the shared vocabulary — required rather than optional, with its own description text instead of `PINWRIGHT_EXPOSURE_PARAM_DESC` | `OrthoTileCaptureHandler.cpp:173`. Low severity; the requirement itself is correct and documented |

**GAP, explicitly:** the internal welded constants of `AnnotatedCaptureHandler.cpp` (annotation
sizing), `OrthoTileCaptureHandler.cpp` (tile planner) and `ZFightingHandler.cpp` were **not** read
beyond their `RPC_PARAMS` blocks. Alias-key coverage (`orthoWorldWidth`, `ActorNameKeys()`) outside the
framing family is likewise unchecked. These are gaps, not N/As.

### 5.3 Scope: fix P1, test the class

R5 fixes **P1 only** — `camera.orbit_shots` gains `padding`, `camera.frame_actor` gains `distance` —
because that pair is one file and one helper. P2–P9 are **not** fixed in this wave: each moves a
default that has shipped, and `RenderHandler.cpp:571-572` records a deliberate decision behind one of
them. They are recorded here and filed to `defect-backlog.md` by R6.

R7 writes the general check instead: a cross-verb `FParamSpec` walk asserting that verbs sharing a
helper expose the same inputs, with an explicit **allow-list of known, reasoned exceptions** carrying
the file:line of the rationale. That is what turns P2–P9 from prose into a failing test the day
someone tries to fix them, and it is one loop wider than the `UndeclaredParamsAreDeclared` check that
already exists for a single verb (`capture-subject-convergence.md:635`).

---

## §6 Work — one wave of seven file-disjoint chunks

No chunk edits a file another chunk edits. Every chunk that needs a test writes a **new** test file
rather than appending to a shared one, for the same reason.

**The tree does not compile until the whole wave lands.** R2 and R5 reference §2.2's declarations
before R1's file exists on their agent's disk; that is why the contract is frozen above. Build and
suite verification is a wave-end step, not a per-chunk step.

**Other agents edit this checkout concurrently. Re-read any file before editing and never revert a
change you did not author.** Four docs this plan cites are currently uncommitted (` M`):
`visual-review.model-rig.md`, `render.md`, `render.capture-subjects.md`,
`capture-subject-convergence.md`.

### R1 — the rig: parse, apply, restore, measure, report

**Create** `Source/PinWright/Private/Handlers/Render/PreviewSceneRig.h`, `PreviewSceneRig.cpp`,
`Source/PinWright/Private/Tests/Render/TestCapturePreviewSceneRig.cpp`.
**Modify** `Handlers/Render/PreviewViewportCaptureUtils.h`, `PreviewViewportCaptureUtils.cpp`,
`Handlers/Render/PoseListCapture.h`, `PoseListCapture.cpp`, `PinWright.Build.cs`.

`PinWright.Build.cs` gains `"AdvancedPreviewScene"` — verified absent today (zero hits for it in any
`.cs` in the plugin) and **not** reachable transitively (`UnrealEd.Build.cs` does not list it). Expect
a full module rebuild. The `key`/`sky` half needs no new dependency at all (`FPreviewScene` is
`Runtime/Engine`, already a public dep); the dependency buys `FPreviewSceneProfile`,
`UAssetViewerSettings` and `FAdvancedPreviewScene` for the profile snapshot, the floor/environment
`bDirect` writes, and the profile **report**.

Steps: implement §2.2 verbatim; `IsAdvancedPreviewViewport` as an **exact-name allow-list** over the
viewport widget type — never a suffix match, the rule `CaptureSubject.cpp` already follows, and note
Persona qualifies because `IPersonaPreviewScene : public FAdvancedPreviewScene`
(`IPersonaPreviewScene.h:71`) with `FAdvancedPreviewScene` on the primary base chain, so the
`static_cast` is a no-op adjustment; the guard with §2.3's five invariants and §2.4's apply/restore
order; digest `FPaths::SourceConfigDir() / TEXT("DefaultEditor.ini")` — **computed, never
hardcoded**; `MakePreviewSceneRigInfoObject` attached in `MakeViewportInfoObject`; the pin threaded
through `ParseViewportCaptureRequest` and `FPoseListCaptureRequest`;
`PINWRIGHT_PREVIEW_SCENE_PARAM_DESC`. Change **nothing** in `ExposurePinGovernsFrame` (§4.2).

Acceptance:
- `PinWright.render.preview_scene_rig.ArrivalRoundTripsTheEngineDefault` — `ArrivalToLightRotation(112.5, 40)`
  equals `FRotator(-40, -67.5, 0)` within 1e-3, and `LightRotationToArrival` inverts it. *Unable to
  fail if* it asserted only that the functions are inverses of each other — two consistently wrong
  functions pass that. It must compare against the engine's own literal, read from
  `FPreviewScene::ConstructionValues()` at assert time rather than typed in.
- `…OmittedParameterWritesNothing` — with no `previewScene` in the payload, the light component's
  rotation, intensity and colour, the sky intensity, and the **entire** `UAssetViewerSettings::Profiles`
  array are bit-identical before and after, and `applied` is `false` while `previous` is still
  populated. *Unable to fail if* it only checked `applied == false`; assert the component values and
  the profile array too.
- `…ProfileArrayIsRestoredAfterAnEngineMutation` — mutate `Profiles[i].bShowFloor` inside the guarded
  scope the way `SNiagaraSystemViewport.cpp:872` does, then assert the array is byte-equal to entry
  on exit **and** that `restore.profilesRestored` is `true`. *Unable to fail if* the fixture never
  mutated anything — assert the mutation took effect **inside** the scope as a precondition, in both
  directions.
- `…ConfigDigestIsUnchangedAcrossAGuardedMutation` — digest `Config/DefaultEditor.ini`, run the
  scope with a profile mutation inside it, digest again, assert equal **and** assert
  `restore.configFileUnchanged` reports it. *Unable to fail if* the digest were computed once and
  compared to itself, or if the file did not exist — assert the file exists and that the digest is
  non-empty first. A **counterfactual** comment must record what removing the restore does, the way
  `TestGenerateThumbnail.cpp:28` does.
- `…RestoreOrderPreventsTheProfileWriteback` — apply a key aim, then *inside* the scope raise
  `OnAssetViewerSettingsChanged().Broadcast(NAME_None)` (which forces `UpdateScene`'s four-way path),
  and assert `Profiles[i].DirectionalLightRotation` is **still** the original. *Unable to fail if*
  the broadcast did not actually reach `UpdateScene`; assert `bLightDirChanged`'s precondition by
  first checking the component rotation differs from the profile's.
- `…LevelViewportReportsSceneUnavailable` — a level-viewport client yields
  `previewScene.sceneAvailable == false` with no `profileName` key at all, not an empty string.
  Assert **absence**, per `capture-subject-convergence.md:393`'s absence-assertion rule.
- `…HalfAnAimIsRefused` — `{key:{azimuth:110}}` returns `INVALID_ARGUMENT` whose message contains
  **both** `azimuth` and `elevation`. *Unable to fail if* it asserted only that parsing failed; a
  typo also fails. Assert both substrings.
- `…EmptyBlockIsRefused` — `previewScene:{}` returns `INVALID_ARGUMENT`. *Unable to fail if* the
  parser treated `{}` as absent; assert the error, not the no-op.

### R2 — the three `render.*` preview verbs declare the parameter

**Create** `Tests/Render/TestPreviewSceneRigRenderVerbs.cpp`.
**Modify** `Handlers/Render/RenderHandler.cpp`, `Handlers/Render/AnimationPreviewCaptureHandler.cpp`,
`Handlers/Render/AnnotatedCaptureHandler.cpp`.

Steps: `RPC_PARAM_OPT("previewScene", "object", PINWRIGHT_PREVIEW_SCENE_PARAM_DESC)` on
`render.capture_asset_preview` (`RenderHandler.cpp:272` block),
`render.capture_animation_preview` (`AnimationPreviewCaptureHandler.cpp:166` block) and
`render.capture_annotated` (`AnnotatedCaptureHandler.cpp:185` block). **Do not declare it on
`render.capture_open_level`**, and clear the pin the shared parser filled on that path, exactly the
way `RenderHandler.cpp:386-395` already clears `viewDistanceScale` and `hideEditorSprites` —
decision 9. Carry `closeAfterCapture`'s three-state semantics untouched.

Acceptance:
- `PinWright.render.capture_asset_preview.PreviewSceneRigIsDeclared` — walks the verb's `FParamSpec`
  list and asserts `previewScene` is present. *Unable to fail if* it grepped the source string;
  it must read the registered spec list, which is what the dispatcher gates on.
- `…OpenLevelRefusesPreviewSceneRig` — `render.capture_open_level` with a `previewScene` payload
  returns `UNKNOWN_PARAMS` naming the field. *Unable to fail if* it asserted merely that the call
  failed; assert the code **and** that the message names `previewScene`.
- `…OpenLevelPinIsClearedNotJustUndeclared` — call the shared parser directly with a `previewScene`
  payload, run the level-viewport path, and assert `Request.PreviewSceneRig.bRequested == false`.
  *Unable to fail if* the verb had already rejected the payload upstream; the test must exercise the
  parser and the clear, not the dispatcher gate. This is the assertion that separates this from bug 1
  of the previous wave.
- Existing `TestCaptureExposurePin.cpp`, `TestCaptureViewModeOverride.cpp`,
  `TestAssetPreviewSubjects.cpp`, `TestAnimationPreviewSubjects.cpp` and `TestAnnotatedSubjects.cpp`
  pass **unmodified**.

### R3 — `viewMode` on `render.capture_ortho_tiles`

**Create** `Tests/Render/TestOrthoTileViewMode.cpp`.
**Modify** `Handlers/Render/OrthoTileCaptureHandler.cpp`, `OrthoTileCaptureUtils.h`,
`OrthoTileCaptureUtils.cpp`.

Steps: `RPC_PARAM_OPT("viewMode", "string", PINWRIGHT_VIEW_MODE_PARAM_DESC)`; parse with the existing
`ParseViewModePin(Payload, Pin, ErrCode, ErrMsg, /*Client=*/nullptr)` — the null-client form
`camera.frame_actor` already uses (`CameraFrameHandler.cpp:311-318`); apply with the engine's
`ApplyViewMode(Pin.ViewMode, /*bPerspective=*/false, Component->ShowFlags)` beside
`OrthoTileCaptureUtils.cpp:239`; **no scope guard** — the component is per-call and discarded, and a
guard that restored nothing would be theatre; extend `DescribeShowFlags()` (`:512-534`) with a
`viewMode` sub-block reporting the requested key, the applied key, and the flags that did **not**
read back; reconcile the `!Lighting || !PostProcessing` warning at `:484-495` so a deliberately unlit
mode names itself rather than reading as a fault. Refuse the unreachable families (§4.3) with the
four existing view-mode codes. Default is no override.

Acceptance:
- `PinWright.render.capture_ortho_tiles.OmittedViewModeLeavesShowFlagsUntouched` — with no `viewMode`,
  every flag `DescribeShowFlags` reports is identical to a build of the same component without the
  parameter. *Unable to fail if* it compared the component to itself; construct two components, one
  through each path, and diff.
- `…UnlitActuallyClearsTheLightingFlag` — `viewMode:"unlit"` yields `showFlags.lighting == false` in
  the response **and** the reported `viewMode.applied` key equals `"unlit"`. *Unable to fail if* it
  asserted only that the call succeeded — that is exactly how `camera.orbit_shots` shipped an inert
  `viewMode` (`PreviewViewportCaptureUtils.cpp:1542-1545`). Assert the measured flag.
- `…UnlitDoesNotReadAsAFault` — the same call's warning text names `unlit`, and no
  lighting-off warning fires that would have fired without the parameter. *Unable to fail if* the
  warning array were empty for an unrelated reason; assert the un-parameterised call **does** warn
  when lighting is off for other reasons, as the contrast case.
- `…UnreachableFamiliesAreRefusedSpecifically` — `viewMode:"VisualizeBuffer"` returns
  `VIEW_MODE_NEEDS_COMPANION`, `"LightmapDensity"` returns a refusal naming the editor-scope override.
  *Unable to fail if* both returned the same generic code; assert the codes differ.
- `…GameFlagBaseIsReportedNotAssumed` — the response names which requested distinguishing flags did
  not read back off `Component->ShowFlags`. This is the §4.3 `GAP` row's answer, and it must be a
  measurement, not a table in a comment.

### R4 — the widget-designer alpha stamp

**Create** `Tests/Widget/TestWidgetDesignerCaptureAlpha.cpp`.
**Modify** `Handlers/UI/WidgetDesignerCaptureUtil.cpp`, `WidgetDesignerCaptureUtil.h`.

Steps: measure `alphaZeroFraction` over `ColorData` **before** stamping; call
`PinWrightScreenshotUtils::ForceOpaqueAlpha(ColorData)` between `ReadPixels` and
`PNGCompressImageArray` (`:155`/`:157` today); return both facts on `FWidgetDesignerCaptureInfo` so
`widget.screenshot_designer` and `asset.dump`'s widget aspect can both publish
`opaqueStamped` / `alphaZeroFraction`. **Do not touch** `SceneCaptureProbeUtils` or
`ZFightingAnalysis` — both deliberately skip the stamp (`SceneCaptureProbeUtils.h:30-34`,
`ZFightingAnalysis.h:174`).

Acceptance:
- `PinWright.widget.screenshot_designer.PreviewPathStampsOpaque` — every pixel of the encoded buffer
  has `A == 255`. *Unable to fail if* the fixture widget happened to be fully opaque already; the
  fixture must be a widget with a transparent region, and the test asserts
  `alphaZeroFraction > 0` as a **precondition** before checking the stamp.
- `…AlphaZeroFractionIsMeasuredBeforeTheStamp` — the reported fraction is non-zero on that fixture.
  *Unable to fail if* the measurement ran after the stamp, in which case it is always 0; assert
  non-zero, which is only possible from a pre-stamp read.
- `…RgbIsUnchangedByTheStamp` — RGB bytes are identical before and after. Mirrors
  `TestCaptureOpaqueAlpha.cpp:53`.
- A **counterfactual** comment naming what deleting the call does, per `TestGenerateThumbnail.cpp:28`.

### R5 — camera parameter parity, and the two camera verbs declare the rig

**Create** `Tests/Render/TestCameraFitDistanceParity.cpp`.
**Modify** `Handlers/Render/CameraFrameHandler.cpp`, `Handlers/Render/CameraShotPlanUtils.h`.

Steps: `camera.orbit_shots` gains `padding` (default 1.15, replacing the `constexpr` at `:541`);
`camera.frame_actor` gains `distance` (an explicit camera distance that skips
`ComputeFitDistance`, mirroring `orbit_shots`' `radius` precedence at `:775-778`); both verbs gain
`RPC_PARAM_OPT("previewScene", …)` and pass the parsed pin into `FPoseListCaptureRequest`; document
in the header comment that `subject.radius` is a **distance** on orbit and a **sphere radius** on
frame_actor (§5.1) — the code keeps both meanings, the comment stops the next reader losing an hour.
`PoseRequest.MaxPoses = GMaxOrbitShots` must survive.

Acceptance:
- `PinWright.camera.orbit_shots.PaddingDefaultMatchesTheWeldedConstant` — with `padding` omitted, the
  solved distance equals `ComputeFitDistance(R, Fov, 1.15f)` exactly. *Unable to fail if* it asserted
  the new default is 1.15 by reading the same literal it set; compute the expected distance from the
  helper at assert time and compare distances, not defaults.
- `…PaddingActuallyMovesTheCamera` — `padding: 2.0` yields a strictly larger distance than
  `padding: 1.15` on the same fixture. *Unable to fail if* the two were compared with a tolerance
  wider than the difference; assert the ratio is within 1e-3 of `2.0/1.15`.
- `PinWright.camera.frame_actor.ExplicitDistanceSkipsTheFit` — `distance: 500` places the camera at
  500 regardless of bounds. *Unable to fail if* the fixture's fit distance happened to be 500;
  assert the un-overridden fit distance differs from 500 first.
- Existing `TestCameraFrameHandlers.cpp` and `TestCameraFrameSubjects.cpp` pass **unmodified** —
  `path`, `width`, `height`, `actorName`, `count`, `shots[]`, `resolutionSource`, per-shot
  `orthoAxisSnapped`/`orthoView`/`orthoWidth`.

### R6 — documentation

**Create** `docs/wiki-src/render.preview-scene-rig.md`,
`Tests/Core/TestPreviewSceneRigDocs.cpp`.
**Modify** `docs/wiki-src/render.md`, `render.capture-exposure.md`, `render.capture-subjects.md`,
`camera.md`, `visual-review.md`, `visual-review.model-rig.md`, `widget.md`, `property.md`,
`level-building.capture-and-review.md`, `docs/index.md`, `docs/tags.md`, `docs/lessons.md`,
`docs/defect-backlog.md`, and the comment block at
`Handlers/Render/CaptureSubjectProviders_Mesh.h:28-30` (**comment text only**).

Sole owner of all of them. Any chunk wanting a doc line files it here.

Steps: the new topic page carries the rig contract, the arrival-azimuth convention with the 112.5°/110°
two-method agreement, the measured working band (`visual-review.model-rig.md:138`: 1.5 lux key with a
1.0 sky lands a lit face at 0.55–0.65 sRGB; 2.6 lux with 1.4 sky clips past 0.85), and §4.1's refusal
of profile selection. `render.md:19-27`'s `viewport` field table gains the `previewScene` row.
`camera.md:35` and `level-building.capture-and-review.md:37` have *"Captures are stamped opaque on
every path now"* corrected to name R4's fix and its date. `property.md` gains §4.6's unsupported note.
`visual-review.model-rig.md`'s five-versus-six contradiction (`:3`/`:65` against `:5`/`:11-16`/`:18`)
is reconciled to one number. **`docs/lessons.md:143` is retracted** — its claim that
`FAdvancedPreviewScene` is unlit and bimodal under `-unattended` was withdrawn by
`render.capture-exposure.md` (the captures had all been written to one file, because the auto-generated
screenshot filename carries a one-second timestamp while a capture takes ~60 ms) and by
`RenderHandler.cpp:445-446`, while `lessons.md` and `CaptureSubjectProviders_Mesh.h:28-30` still
propagate it. §5.2's P2–P9 are filed to `defect-backlog.md`.

Two house rules this chunk lives or dies by:
**rendering stops at the first `###` line** (`CLAUDE.md:215`), so every `##` editorial section must sit
above the first `###` — audited 2026-08-14, this had swallowed ~26.5 KB of shipped content across five
pages; and a **new** topic file needs an editor restart before `FWikiOverlay` picks it up
(`wiki-src/README.md:69`), while edits to existing pages are mtime-cached and need none.
Plans are **not** index-exempt (`docs/SCHEMA.md:92`), so this plan itself needs rows in `docs/index.md`
and `docs/tags.md`.

Acceptance: `PinWright.core.docs_schema.EveryMaintainerDocIsIndexed` passes;
`PinWright.infra.wiki_src.SourcePagesFollowRenderingRules` passes; a new
`TestPreviewSceneRigDocs.cpp` asserts on overlay-**exclusive** phrasing only, never on bare parameter
names the generator already renders — *unable to fail if* it asserted on `"previewScene"`, which the
generator emits from `RPC_PARAMS` whether or not anyone wrote a word of prose; and
`…OpaqueEveryPathClaimIsGone` greps `camera.md` and `level-building.capture-and-review.md` for the
literal string *"stamped opaque on every path"* and asserts **zero** matches — *unable to fail if*
it searched for a paraphrase; assert on the exact shipped sentence.

### R7 — cross-verb parameter parity, as a test

**Create** `Tests/Render/TestCaptureVerbParameterParity.cpp`. **Modify** nothing.

Steps: walk the registered `FParamSpec` lists of the eight capture verbs; group them by shared helper
(`ComputeFitDistance`, `ComputeOrthoWorldWidth`, `ParseExposurePin`, `ParseViewModePin`,
`FPoseListCaptureRequest`); assert every verb in a group exposes the same input set; carry an explicit
**exception table**, each row citing the `file:line` of its written rationale — `hideEditorSprites` on
the preview verbs (`PreviewViewportCaptureUtils.h:26-51`), `rejectBlank` on the camera verbs
(`CameraShotPlanUtils.h:308-311`), `padding` 1.25 on `capture_asset_preview`
(`RenderHandler.cpp:571-572`). A gap with no rationale row fails.

Acceptance:
- `PinWright.render.parameter_parity.SharedHelperInputsAreExposedUniformly` fails today on P2–P9 and
  passes only once each is either fixed or given a rationale row. **The chunk lands with P2–P9 in the
  exception table**, so the test is green at wave end and red the moment a new gap appears.
- `…EveryExceptionCitesARationale` — every exception row's `file:line` resolves to a real line whose
  text contains the parameter name. *Unable to fail if* the citation were only checked for
  non-emptiness; it must open the file and match the name, which is how the repo's own
  `check_hazards` tool validates citations (`docs/tools/README.md`).
- `…TheKnownDefectIsCaught` — with R5's fix reverted in the fixture's model of the spec lists, P1
  fails the parity assertion. *Unable to fail if* the test were written against the post-R5 spec
  lists only; it must include a synthetic pre-fix pair and assert the check rejects it.

### Ordering

Only one ordering is real. Everything else runs together.

```
R1 contract frozen in §2.2  ──>  R2, R5     (compile-time only; the wave lands together)
```

R3, R4, R6 and R7 have no blocker at all. R6 can be written from this document.

---

## §7 Shared-file ownership

| File | Owner | Rule |
|---|---|---|
| `Handlers/ErrorCodes.h` | **nobody** | Decision 6: no new codes. A chunk that believes it needs one stops and the plan is amended |
| `docs/error-code-catalog.md` | **nobody** | Generated, and `commit-grouping.md:52` says regenerate it **last**, once, not per commit |
| `PinWright.Build.cs` | **R1** | The only chunk that needs `AdvancedPreviewScene`. Verified absent and not transitively available |
| `Handlers/Render/PreviewViewportCaptureUtils.h/.cpp` | **R1** | The single `viewport` serializer and the single capture-request parser both live here. No other chunk edits them; R2 and R5 only `#include` the header |
| `Handlers/Render/PoseListCapture.h/.cpp` | **R1** | Additive: one field on the request, forwarded per pose |
| `Handlers/Render/RenderHandler.cpp` | **R2** | Holds two of the eight verbs. R5 does **not** touch it |
| `Handlers/Render/AnimationPreviewCaptureHandler.cpp`, `AnnotatedCaptureHandler.cpp` | **R2** | |
| `Handlers/Render/CameraFrameHandler.cpp`, `CameraShotPlanUtils.h` | **R5** | Holds `camera.frame_actor` and `camera.orbit_shots` **and** `ComputeFitDistance`. R2 does not touch either |
| `Handlers/Render/AnimationShotsHandler.cpp` | **nobody** | `camera.animation_shots` is level/actor-only (§1a) and already exposes both helper inputs — it is the parity reference, not a target |
| `Handlers/Render/OrthoTileCaptureHandler.cpp`, `OrthoTileCaptureUtils.h/.cpp` | **R3** | |
| `Handlers/Render/ViewModeVocabulary.h/.cpp` | **nobody** | R3 reads `Resolve` and `DistinguishingShowFlags`; both are already viewport-free and need no change |
| `Handlers/UI/WidgetDesignerCaptureUtil.h/.cpp` | **R4** | |
| `Handlers/UI/WidgetDesignerScreenshotHandler.cpp` | **nobody** | Its window branch already stamps (`:310`). Only the util is broken |
| `Handlers/Render/SceneCaptureProbeUtils.h`, `ZFightingAnalysis.h` | **nobody** | Both deliberately skip `ForceOpaqueAlpha`. Do not "fix" |
| `Handlers/Utility/UtilityPropertyHandler.cpp` | **nobody** | §4.6: documented, not gated |
| `Handlers/Render/CaptureSubject.h/.cpp` | **nobody** | R1's allow-list is its own; widening `CaptureSubject`'s would make it contested for no gain |
| All `docs/**` + `CaptureSubjectProviders_Mesh.h`'s comment block | **R6** | Append-only for anyone else; re-read before editing. The header edit is **comment text only** |
| `Tests/Render/TestCaptureExposurePin.cpp`, `TestCaptureViewModeOverride.cpp`, `TestCaptureViewModeReporting.cpp`, `TestCaptureOpaqueAlpha.cpp`, `TestCaptureBlankCriterion.cpp`, `TestScreenshotBlankCapture.cpp`, `TestCameraFrameHandlers.cpp`, `TestOrthoTileCapture.cpp` | **nobody** | The regression floor. They must pass **unmodified**. A chunk needing a new assertion writes a new file |

---

## §8 Preconditions

**This wave starts only after a green full suite with reconciling counts.** Not a filtered run.

1. **The recorded baseline is stale and must not be carried forward.** `CLAUDE.md:116` records
   4051/4051/0 at `39a3eb2b`; `bcc334e8`, `c04375fc` and `bc951ea1` have landed since. Re-run and
   record the new figure, then derive this wave's expectation from it —
   `<UE_ROOT>/Engine/Binaries/ThirdParty/Python3/Win64/python.exe -m check_suite_log <log> --expected N` from `Content/Python/` (never `uv`), per `CLAUDE.md` § Testing.
   Whoever changes that figure moves the citation with it (`:122`).
2. **A count can be green and still prove nothing.** `CLAUDE.md:122` and board ticket
   `B-test-skips-assertions-silently`: a conditional-skip path prints
   `PINWRIGHT_ASSERTIONS_SKIPPED` and reports success having asserted nothing. Every acceptance
   criterion above is written against that failure mode; check the log for skips, not only for fails.
3. **Announce the build.** `CLAUDE.md:44`. Pass `-NoHotReloadFromIDE` (`:33`) — the blocker is another
   project's Live Coding console and waiting never clears it; never kill it. R1's `.Build.cs` edit
   forces a full module rebuild, so compile-checking with `-SingleFile` (`:46`) will not cover it.
4. **A suite run is in progress at the time of writing.** Nothing in this plan may be built, run or
   tested until it drains.

---

## §9 Risks

- **The `.Build.cs` edit is the wave's only irreversible-feeling step and it is R1's alone.** Adding
  `AdvancedPreviewScene` rebuilds ~58 unity TUs. If the wave is abandoned the edit must come out with
  it, or the next builder pays for a dependency nothing uses.
- **The profile snapshot is a whole-array copy including three full `FPostProcessSettings` structs.**
  Cheap per call, but it happens **per capture**, and multi-shot verbs capture N times. If a burst
  regresses measurably, hoist the snapshot to the set rather than the shot — the same reason
  `camera.animation_shots` resolves resolution once (`AnimationShotsHandler.cpp:23-29`).
- **`bRotateLightingRig` makes a capture non-reproducible by construction** and writes the key
  rotation into the shared profile on **every tick** (`AdvancedPreviewScene.cpp:285-294`). All three
  shipped profiles have it `false`, so this is latent here — which is exactly why the report field
  exists. A capture that finds it `true` is not comparable with one that finds it `false`, and no
  restore can fix that.
- **A concurrent editor window can undo the restore.** `Save()` fires from any
  `SAdvancedPreviewDetailsTab` teardown anywhere in the process, including one another agent closes
  a millisecond after our restore. The digest field will report it; the plan cannot prevent it. Say
  so in R6's docs rather than implying the guarantee is stronger than it is.
- **Pixel assertions in the suite are viable, but only with explicit distinct filenames.** The
  auto-generated screenshot filename carries a **one-second** timestamp while a capture takes ~60 ms,
  so back-to-back shots overwrite each other and every "comparison" becomes a file against itself.
  That artefact is what produced the retracted `lessons.md:143` claim. Any criterion comparing two
  captures must pass an explicit `filename` **or** assert the two returned `path` values differ.
- **`meanLuminance` is not a verification.** `visual-review.model-rig.md:157`: it moved **0.9 %** while
  the whole surface changed appearance. Per `rpc-design.md:341`, a chunk reporting on a frame must
  describe **what the frame looked like**, not only its statistics.
- **R4 changes what a widget capture's bytes mean.** Anyone compositing those PNGs against a
  background gets a different picture after this wave. That is why §4.4 publishes
  `alphaZeroFraction`, and why it is the one item flagged for a ruling.

---

## Bugs found while writing this plan

1. **`docs/lessons.md:143` is retracted elsewhere and still shipping.** Its claim that
   `FAdvancedPreviewScene` is unlit and bimodal under `-unattended` was withdrawn in
   `render.capture-exposure.md` and in `RenderHandler.cpp:445-446` (which names the filename-collision
   cause), while `lessons.md` and the design comment at `CaptureSubjectProviders_Mesh.h:28-30` still
   assert it. Three places, two answers, and the stale one is load-bearing for whether pixel
   assertions are worth writing.
2. **Two doc pages promise an opaque-alpha guarantee the code does not keep.** `camera.md:35` and
   `level-building.capture-and-review.md:37`, verbatim: *"Captures are stamped opaque on every path
   now."* False for `widget.screenshot_designer target:"preview"` and `asset.dump`'s widget aspect.
3. **`visual-review.model-rig.md` says five and six on the same page.** `:3` and `:65` say five false
   bugs; `:5` (`## What was actually wrong, six times`), the six-row table at `:11-16`, and `:18`
   ("Four of the six … Two are the …") say six. `:3` also says "every one was lighting or material",
   contradicted by `:11` (a `sheet` part that **was** inside-out) and `:20`.
4. **`visual-review.model-rig.md`'s luminance table may be stale by one day.** The 58-63 table is dated
   2026-08-20; `:52` re-measured the key direction on 2026-08-21 and states the earlier "+X" was
   wrong. Nothing says the table was re-taken after the correction.
5. **`render.capture-subjects.md:25` gives un-scoped advice that fails on one of the two verbs it
   addresses.** *"To pull the camera back on an asset kind, pass `padding` or an explicit camera; on
   `camera.orbit_shots` the top-level `radius` argument still applies…"* — `orbit_shots` has no
   `padding`, so a reader following the first clause gets `UNKNOWN_PARAMS`. R5's fix makes the
   sentence true; until it lands the sentence is wrong.
6. **`render.md` documents no lever for the preview backdrop at all.** Zero occurrences of
   `environment`, `background`, `cubemap`, `skylight`, `HDRI`; two of `backdrop`, both describing it
   as a hazard (`:98`, `:109`). The one chrome-suppression parameter, `hideEditorSprites`, is
   explicitly refused on the preview verbs (`:282`) and correctly so — an `FAdvancedPreviewScene`
   holds no sprite components (`:445`).
7. **`render.md` gives `blank` two definitions.** `:274` defines it as a two-part threshold test
   (mean ≤ 0.01 **and** fewer than a floor of lit pixels above 0.02); `:525` calls it "an all-zero
   test". Same page, same field.
8. **`FAdvancedPreviewScene::UpdateScene`'s visibility tail ignores its own flags.** Lines 225-234
   re-assert sky, sky-light and floor visibility from the profile regardless of all four `bUpdate*`
   parameters. A caller passing `bUpdateEnvironment=false` still gets the environment updated. Engine
   defect; recorded because R1's restore ordering depends on knowing it.
9. **`SetFloorVisibility(true, /*bDirect=*/true)` cannot force the floor visible.** `:407` ANDs the
   argument with the profile's `bShowFloor`. Only `false` is unconditional. Engine defect; it is why
   §2.4 restores the profile rather than calling the direct setter with `true`.
10. **`SAdvancedPreviewDetailsTab`'s destructor has a copy-pasted twin.**
    `DataflowAdvancedPreviewDetailsTab.cpp:41` repeats the same three delegate removals and the same
    unguarded `Save()`. Two engine paths, one defect.
11. **`FPreviewSceneProfile`'s constructor pins auto-exposure to `[-1, 1]` and this project's
    committed profiles turn that off.** `AssetViewerSettings.h:65-69` sets
    `bOverride_AutoExposureMin/MaxBrightness = true` with the comment *"This will stop scene from
    becoming bright due to exposure"*; all three profiles in `Config/DefaultEditor.ini` carry
    `bOverride_AutoExposureMinBrightness=False`. How the `False` first got written could not be
    determined from source. Do not assume the clamp is active.
12. **`bShowGrid` is deprecated and inert in UE 5.8** (`AdvancedPreviewScene.h:86-91`,
    `HandleToggleGrid` is a no-op at `:522-531`) but is still serialized into this project's committed
    config at all three profiles. Harmless, but it will appear in any config diff this plan produces.

---

## Unresolved questions

1. **R4 — should a widget-designer preview capture be stamped opaque, or should the alpha be
   preserved and the docs corrected instead?** §4.4 chooses "stamp and publish
   `alphaZeroFraction`", on the grounds that two docs already promise the stamp and the transparent-PNG
   failure has cost days. The opposite choice — keep alpha, fix the two doc sentences — is defensible
   because a widget's transparency is genuine content. **This changes what existing callers' bytes
   mean either way, so it wants a ruling before R4 starts.**
2. **Should `previewScene` also be accepted on `camera.animation_shots`?** It is level/actor-only
   today (`AnimationShotsHandler.cpp`), so §1a marks it `⛔`. If the subject resolver is ever pointed
   at an asset kind there, the cell changes. Not in this wave.
3. **Should P2–P9 be fixed now or only tested?** §5.3 chooses test-only, because each moves a shipped
   default and one of them (`RenderHandler.cpp:571-572`) records a deliberate decision. If the
   preference is to converge the padding defaults to one number, that is a different, breaking wave
   and should be planned as one.
4. **Is a per-capture profile snapshot acceptable at burst scale, or should it hoist to the set?**
   §9. The answer is a measurement, not a design choice, and it needs the wave landed first.
5. **`docs/lessons.md:143` — retract or annotate?** R6 assumes retract-and-replace. If the corpus
   convention is to keep the wrong claim with a dated correction beneath it (as
   `render.capture-exposure.md` and `model.examples.md:99` both do), say so and R6 follows that shape
   instead.
