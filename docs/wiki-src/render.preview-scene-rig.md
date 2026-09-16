# render.preview-scene-rig

The `previewScene` capture parameter: the key light's aim, intensity and colour, the sky brightness, the floor and the backdrop of an asset-editor preview scene. A single-shot call applies and restores it once; a pose set keeps one scope alive through warm-up, every shot, and its final repeatability control, then restores on exit including errors. Read it before comparing two asset-preview captures taken in two editors, because the rig they were drawn under is a per-user setting and nothing else in the response tells you which one it was.

`render.capture_mesh` uses the same wire shape and measured drawn-rig report on a different ownership boundary: one RPC builds a bare transient `FPreviewScene` with local floor and environment components and reuses it for its shot set. It does not construct `FAdvancedPreviewScene`, read or write `UAssetViewerSettings`, or touch an editor viewport, so there is no shared profile or committed config state for concurrent callers to contend over. Its rig block says `disposed:true` and omits the viewport restore ledger because the entire private scene is destroyed; claiming an `afterRestore` measurement there would be false.

## Why the parameter exists

A preview scene belongs to the editor showing it. `FAdvancedPreviewScene` builds its key light, sky light, floor and sky sphere from `UAssetViewerSettings::Profiles[i]`, where `i` is `UEditorPerProjectUserSettings::AssetViewerProfileIndex` — **per user, not per project**; an out-of-range value silently falls back to profile 0. Two machines can therefore light one asset differently. Measured 2026-08-19: two meshes at the same pinned `ev100` returned mean luminance **0.47** and **0.18** with no content difference ([`visual-review`](visual-review.md) § Comparing Two Or More Subjects).

**This is a parameter, never a verb.** There is no `render.set_lighting`. Like `viewMode` versus `editor.set_view_mode` ([`render.view-modes`](render.view-modes.md)), the scoped form cannot leak state into later captures or other windows.

## Wire shape

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

- Every field is optional and **an omitted field is not written**; unnamed values keep the profile's value.
- `previewScene: {}` is `INVALID_ARGUMENT`. An object that asks for nothing is a caller mistake, not a no-op — the same reasoning that refuses `exposure: {mode:"auto", ev100:5}` ([`render.capture-exposure`](render.capture-exposure.md)).
- `azimuth` without `elevation`, or the reverse, is `INVALID_ARGUMENT` naming **both**. Half an aim silently keeps half a rig the caller never measured. `elevation` must be in `[-90, 90]`; `azimuth` takes any angle and wraps.
- Absent `previewScene` writes nothing and the capture is byte-for-byte what it was before the parameter existed.
- `key.color` is a **hex string** — `"#RRGGBB"`, `"#RRGGBBAA"`, `"#RGB"` or `"#RGBA"`, `#` optional. Anything else is refused rather than defaulted, because the engine's `FColor::FromHex` returns opaque black for input it cannot read, which would light the subject in a colour nobody chose and report success. The response reports the colour back as both a hex string and a linear triple, since the light stores sRGB and the shading path uses the linear form.
- A rig request against a viewport with no preview scene is `UNSUPPORTED_ASSET_EDITOR`, not a silent ignore. [`render.capture_open_level`](render.capture_open_level.md) does not declare the parameter at all and refuses it with `UNKNOWN_PARAMS`; on [`render.capture_annotated`](render.capture_annotated.md), [`camera.frame_actor`](camera.frame_actor.md) and [`camera.orbit_shots`](camera.orbit_shots.md) it is served only when `subject` names an asset kind, because those verbs capture the Level Editor viewport otherwise and a level is lit by its own actors.
- The multi-shot verbs read it **once for the whole set** and restore once after the last shot. A rig that moved between instants would make a lighting change and a pose change read identically.

Verbs that carry a rig: [`render.capture_mesh`](render.md#rendercapture_mesh), [`render.capture_asset_preview`](render.capture_asset_preview.md), [`render.capture_animation_preview`](render.capture_animation_preview.md), and — on an asset subject only — [`render.capture_annotated`](render.capture_annotated.md), [`camera.frame_actor`](camera.frame_actor.md), [`camera.orbit_shots`](camera.orbit_shots.md). [`camera.animation_shots`](camera.animation_shots.md) and [`render.capture_open_level`](render.capture_open_level.md) drive a level viewport and have no preview scene; [`render.capture_ortho_tiles`](render.capture_ortho_tiles.md) has no viewport client at all and writes its show flags directly instead.

## Azimuth and elevation are where the light ARRIVES from, not where it points

The engine stores the opposite. `FPreviewScene::GetLightDirection()` returns the light component's **+X axis**, the direction light *travels*; shading sites negate it. An arrival pair converts as:

```
Rotation = (pitch = -elevation, yaw = azimuth - 180, roll = 0)
```

The engine's own shipped default proves the conversion. `FPreviewScene::ConstructionValues` defaults `LightRotation` to `(-40, -67.5, 0)`, which is **arrival azimuth 112.5°, elevation 40°** — light arriving from above and behind-left of the default preview camera.

**Two independent methods, 2.5° apart.** Trig gives 112.5°. An empirical orbit at fixed elevation and pinned `ev100: -1` put the key "near azimuth **110°**": the facing surface lit at camera azimuth 85°, 110° and 130° but was a black silhouette at −40° and −95° ([`visual-review.model-rig`](visual-review.model-rig.md) § The asset preview's own lighting, measured). The response publishes both the raw `FRotator` and derived arrival pair.

A third measurement agrees. **An opposed pair in orthographic is not photometrically comparable** because the key arrives from one side: at `orthoWidth: 420`, one scene measured mean luminance **0.587 from +Y** and **0.036 from −Y**, a sixteenfold difference with no content change. That split is what a single key near 110–112.5° produces. Orthographic aiming is exact, but lighting does not survive the flip: shoot opposed pairs in `perspective`, or pin `previewScene.key` on both halves. If orthographic and unpinned, compare **hue per channel**, not brightness ([`render.capture-exposure`](render.capture-exposure.md)).

**A working band to start from**, against a 0.18-albedo neutral material at `ev100: -1`: a **1.5 lux key with a 1.0 sky** lands a lit face around 0.55–0.65 sRGB with a readable shadow side; **2.6 lux with a 1.4 sky** already pushes lit faces past 0.85 and clips. That band was measured on a review *level*; the units are the same, but a preview scene adds a backdrop cubemap a bare review level does not have. Treat it as a starting point and read `imageStats` back — it is not a calibration.

For reference, the three profiles the engine ships: `Epic Headquarters` is key 1.0 / sky 1.0 with post-processing and tone mapping on; `Grey Wireframe` is 1.0 / 1.0 with the environment and floor off, mesh edges on, post-processing and tone mapping off; `Grey Ambient` is **4.0 / 2.0**, post-processing and tone mapping off.

## The `viewport.previewScene` report block

The block is **unconditional** on every verb that emits `viewport`; level-viewport verbs carry `sceneAvailable: false`. One serializer builds it for every viewport-owning verb, so a verb-specific shape is a bug ([`render`](render.md) § Check the view mode before you trust a capture). It stays present to distinguish "no rig was asked for" from "a rig was asked for and did nothing".

**The restore ledger is conditional.** `previous`, `afterRestore`, and `restore` describe a change this call made and put back. With no requested rig, emitting them would repeat the drawn rig and a nonexistent outcome — **1,114 characters of every response** against a 10,000-character display budget. They appear when there is something to report, including a **failed** restore of a verb-internal default. `requested` / `applied` / `restored` and the measured drawn rig remain on every capture; assert absence, not zero.

**Fields inside it are omitted, never zeroed, when there is nothing to say — assert absence.** `sceneAvailable: false` is the *only* key on a level viewport; there is no empty `profileName`. A rig requested against a scene-less viewport is refused upstream with `UNSUPPORTED_ASSET_EDITOR`; if that guard fails, `requested`, `applied: false`, and `rigWarning` appear instead of dropping the request. The same absence rule applies to `subject` and `poseSet`.

| Field | Meaning |
| --- | --- |
| `sceneAvailable` | The viewport client returned a preview scene. `false` on every level-viewport verb, and then the only field present. |
| `advancedScene` | The scene is an `FAdvancedPreviewScene`. `false` **omits** the profile, floor and environment fields rather than zeroing them. |
| `profileName` / `profileIndex` | **Measured** off the live scene. This is the field that makes two machines' captures comparable — or provably not. |
| `key.rotation` / `key.azimuth` / `key.elevation` | The raw `FRotator` and the derived arrival pair. |
| `key.intensity` / `key.color` / `key.colorLinear` | Read off the directional light component. |
| `sky.intensity` / `sky.visible` / `sky.cubemap` | Read off the sky light and the profile. `cubemap` is **omitted when there is none**, not emitted empty; it is the single biggest reason two machines' captures of the same asset differ. |
| `showFloor` / `showEnvironment` | Read off the profile; `advancedScene` only. |
| `rotateLightingRig` | `true` means the scene re-aims the key **every tick** and writes it back into the shared profile. A capture taken there is not reproducible and no restore can make it so. All three shipped profiles have it `false`, which is exactly why the field exists. |
| `showFlags.postProcessing` / `.tonemapper` / `.eyeAdaptation` | The three flags the profile drives that decide what an exposure pin lands on. See below. |
| `requested` / `applied` / `restored` | The canonical viewport-backed triple. Transient mesh capture emits `disposed:true` instead of `restored`. |
| `captureUpdated` | The sky-light and reflection-capture drain was **driven** for the world these pixels were drawn in. `applied` describes the writes only; this is necessary for those writes to reach the pixels, and on its own not sufficient. See below. |
| `skyCaptureWarning` | Present only when `captureUpdated` is `false`. Do not compare that frame's lighting against another capture. |
| `captureIncomplete` / `captureIncompleteWarning` | Present, and `true`, only when the drain ran and a sky capture was **still queued** when it returned — the engine defers a capture it cannot finish while assets compile and will not retry for 5 s. Omitted, never `false`, on the clean path and on the `captureUpdated: false` path where there is nothing to have measured. |
| `previous` / `afterRestore` | The full rig on both sides, so the restore is checkable by the caller and not only by us. On a pose set, `previous` is the one set-entry snapshot repeated in each shot receipt and `afterRestore` is measured once after the set exits. **Present only when a rig was requested or a restore failed** — with nothing changed they would restate the drawn rig above. |
| `restore.profilesRestored` | The shared profile array is equal to what it was at entry. Same presence rule as `previous`. |
| `restore.configFileUnchanged` / `restore.configFileDigest` | Digest of the committed `Config/DefaultEditor.ini` at entry equals the digest at exit. Same presence rule as `previous`. |
| `rotateLightingRigWarning` | Present only when `rotateLightingRig` is `true`. No restore can fix that state, so it warns rather than correcting. |
| `rigWarning` | Present only when a rig was requested and the scene did **not** read back carrying it. Read the `key` / `sky` fields — they are the measured rig — not the request. |
| `restoreWarning` | Present only when one of the three restore levels failed, naming which: light components, the process-wide profile array (re-lights every open asset editor), or the source-controlled config file (a working-tree diff). |

**`applied` and `restored` are read back off the live scene; nothing is echoed from the request.** A response that only asserted the call succeeded is how `camera.orbit_shots` shipped an inert `viewMode`, and this block is shaped against that failure.

## `applied` is about the writes; `captureUpdated` and `captureIncomplete` are about the drain

Sky and reflection captures are not written by the rig's setters. `SetCaptureIsDirty()` only appends the component to a static queue, and the only thing that drains it is `FPreviewScene::UpdateCaptureContents()` — which the engine calls from three `Tick`s and nowhere else. A capture pumps Slate, invalidates and draws; it runs no editor frame, and `Invalidate()` schedules a redraw rather than a tick. Without an explicit drain, **every preview-scene shot is lit by whatever the last real editor frame left behind** while `applied: true` says the rig landed.

`warmup.settled` cannot cover this: the settle loop's pump is the capture's own, so it cannot advance the work it would have to observe. A frame lit by a stale capture is not a frame in transition — it is a finished frame of the wrong thing, and it converges immediately and legitimately. One measured set reported `settled: true` on round 1 with a mean-luminance delta 166× inside tolerance over pixels that were still cycling.

The capture therefore drives the drain itself, **once per rig lifetime** — so a pose set pays it once and every shot in that set is lit identically — and on the omitted-parameter path too, because a capture that requested no rig reads the same queue. `captureUpdated: false` therefore means the preview world has no renderer scene, and the `skyCaptureWarning` beside it says the frame's lighting is not comparable.

**Driven is not completed, which is why there is a second field.** `UpdateSkyCaptureContentsArray` re-queues a component it could not finish while shaders, textures or meshes are async-compiling, and will not retry it sooner than 5 seconds later. So a drain can run in full and leave the sky light exactly as stale as it found it — the first capture after any asset load is the ordinary way to reach that. `captureIncomplete: true` reports it; nothing the call can do shortens that wait, so re-shoot once compilation settles. It is kept out of `captureUpdated` deliberately: the engine predicate behind it is process-wide, so it can be raised by a sky light in another editor window (a false alarm) but cannot stay silent while this scene's own capture is pending — folding it in would let another window decide this capture's verdict.

## The restore has three levels, and only the shallowest is obvious

A capture that moves the key light puts back all three, in this order:

1. **The light components** — rotation, intensity, colour, sky brightness, floor and environment visibility.
2. **The shared `UAssetViewerSettings::Profiles` array**, snapshotted whole at entry and index-agnostic.
3. **The bytes of `Config/DefaultEditor.ini`**, which is a *committed project file*, verified by digest rather than assumed.

Level 2 is not bookkeeping. `UpdateScene` compares component and profile light direction and, when they differ, **writes the component rotation into the shared profile**. Restore components *first* so that write-back never runs; broadcast settings-changed only if the profile array needed restoring, because an unconditional broadcast updates every open asset editor.

Two engine defects follow. A Niagara viewport hides its floor through an *indirect* constructor setter, writing `bShowFloor` into the process-wide profile and removing the floor from later previews. Also, `UAssetViewerSettings::Save()` runs unguarded from the "Preview Scene Settings" tab destructor reached by `closeAfterCapture`. It skips identical rendered text, but a changed profile rewrites the whole 59 KB file with all three profile lines re-serialised. Restoring the array disarms both, so acceptance uses a digest comparison rather than asserting that this code never called `Save()`.

**The snapshot starts before the window opens, not when the capture starts.** Both writes happen during asset-editor construction, before capture parameters are read: the Niagara floor setter runs in the viewport widget constructor, and constructing `FAdvancedPreviewScene` can write a near-but-not-equal rotation because one side is vector-derived. A capture-start snapshot would preserve that damage. The subject resolver therefore snapshots around the open; the capture guard nests inside the clean profile, and both restores finish **before** `closeAfterCapture` can flush config.

**What that changes in a Niagara preview frame.** Restoring `bShowFloor` re-applies the profile value to every live preview. Pass `previewScene: {showFloor: false}` to choose it per capture; it is restored with the rest.

**One guarantee this cannot give.** `Save()` can fire from any preview-settings tab teardown, including another editor closing a millisecond after restore. `restore.configFileUnchanged` reports that; nothing here can prevent it.

## Selecting a profile per capture is refused, and the refusal is the cheaper answer

The engine has `SetProfileIndex` and it persists nothing, so exposing it would be misleading:

1. **It fights the `key` block.** A profile re-aims the key, so carrying both profile and `key` is order-dependent.
2. **It broadcasts process-wide.** The settings-changed delegate has no property name and updates **every live preview scene in the editor**.
3. **It changes the tone curve.** Both non-default profiles set `bEnableToneMapping = false`; identical pinned `ev100` frames from two profiles are not photometrically comparable.
4. **It adds nothing the `key` block lacks.** `Grey Ambient`'s delta is key 4.0 / sky 2.0, expressible as `previewScene: {key:{intensity:4.0}, sky:{intensity:2.0}}` — component-only, no broadcast, no tone-curve change.

The profile is **reported** so you can see the drawn rig; pin `key` and `sky` explicitly to make two machines agree. Profile changes stay with the editor's own control, as with `editor.set_view_mode` versus `viewMode`.

## What the profile does to an exposure pin

**The tonemapper show flag is not a hazard for the pin, and the claim that it was has been withdrawn.** `ShowFlags.Tonemapper == 0` selects the *gamma-only permutation* of the tonemapper; it does not skip the pass, and the gamma-only branch still multiplies by the same exposure scale as the full path. The engine's own auto-exposure-debug predicate — the one the plugin's `pinned` verdict deliberately mirrors — does not list `Tonemapper` either. So `pinned: true` under `Grey Ambient` is honest. What gamma-only removes is the film curve, the grading LUT, bloom, vignette and local exposure: the *response curve*, not the *exposure*.

**The real hazard is a different flag.** `bPostProcessingEnabled = false` — set on **both** grey profiles — routes through the profile's show-flag application into the engine's `DisableAdvancedFeatures()`, which clears `EyeAdaptation`. It does **not** clear `PostProcessing`, so the plugin's first check still passes, and that is correct: an explicit fixed-exposure setting outranks the `EyeAdaptation` flag, so the pin itself survives. What does not survive is a caller relying on the *profile's own* auto-exposure min/max clamp — that is discarded silently. `viewport.previewScene.showFlags.eyeAdaptation` is the field that shows it.

`FPreviewSceneProfile`'s constructor pins auto-exposure to `[-1, 1]` with the comment "This will stop scene from becoming bright due to exposure", yet all three committed profiles carry `bOverride_AutoExposureMinBrightness=False`. **Do not assume the clamp is active** — read the flags back.

## Costs and limits

- **The snapshot is per capture, not per set on the single-shot verbs.** It copies the whole profile array, three full post-process settings structs included. Cheap once; a burst pays it once for the whole set because the multi-shot verbs read the rig once.
- **`rotateLightingRig: true` defeats reproducibility by construction** and writes the key rotation into the shared profile on every tick. A capture that finds it `true` is not comparable with one that finds it `false`, and no restore fixes that.
- **`bShowGrid` is deprecated and inert in UE 5.8** but is still serialised into committed profiles. Harmless, and expected in config diffs.

## See also

- [`render`](render.md) — the capture verbs, the `viewport` block and the two-renderer rule.
- [`render.capture-exposure`](render.capture-exposure.md) — pinning exposure, what `viewport.exposure` measures, and the cold-preview stop.
- [`render.view-modes`](render.view-modes.md) — the scoped `viewMode` parameter this one is modelled on.
- [`render.capture-subjects`](render.capture-subjects.md) — which subject kinds reach a preview scene at all.
- [`visual-review.model-rig`](visual-review.model-rig.md) — the measured light rig for judging a compiled model, and what the preview's own lighting does to each artefact class.
- [`property`](property.md) — why reaching the same settings object through `property.set` is unsupported.
