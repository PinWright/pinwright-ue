# visual-review.model-rig

The standard setup for judging a compiled model and triaging a suspected geometry defect. In one review session, six artefacts were reported as geometry defects; four were material and two were shadow.

## What was actually wrong, six times

Each was reported as a mesh defect and closed as something else. The recurring mistake shapes are the useful part.

| Reported as | Actually |
|---|---|
| Inverted sheet model — inverted normals, "large black wedge" | Cast shadow plus a real underside — for the wedge. The `sheet` part **was** inside-out because a whole-mesh `extrude` direction opposed the panels' facing normal. The 0.00007 mean-luminance A/B that "disproved" it is blind by construction: a closed shell renders identically inside-out. See [`visual-review`](visual-review.md) § Diagnosing A Black Or Unlit Surface for the two tests that are not. |
| Twisted band — normal-interpolation faceting, 5.4° crease | `WorldGridMaterial`'s grid texture, squeezed by a cylindrical UV; the nearby black stipple was soft-shadow sampling noise from `SourceRadius: 60`. |
| Rook-like model — boolean garbage, "comb-toothed dark bands" | Shadow acne. `SourceRadius`, `ShadowBias` and `ShadowSlopeBias` failed to clear it; only `CastShadows: false` did. |
| Driftwood-like model — `remesh_uniform`, then `twist`, blamed by four authors | `WorldGridMaterial`'s grid again, tiled by `uv mode=cylindrical`; a six-column op probe was clean on all six columns. |
| Tower-like model — corbel/parapet tangency | Shadow-map acne. It survived disabling the key light, and `render.detect_z_fighting` reported 0 pixels. |
| Metal wheel — "white crescent" in the hub; metal gear — white top face | Both are `Metallic: 1` reflecting the backdrop, not untinted vertices. Adding `color=` to the wheel's socket tool left the capture byte-identical and the vertex count at 11,840. **A metal cannot be colour-judged by brightness at all**: base colour is its reflection tint, so one camera can make the gear near-white and another near-black without changing it. Orbit a metal; for a dielectric, ask whether the suspect area stays bright while everything around it swings. |

Four of the six are the *material*. Two are the *shadow*. None is the mesh.

**This does not mean the mesh is never wrong; brightness is simply the wrong instrument.** Two real geometry defects were found on 2026-08-21 and neither was a shading anomaly: a model's uncolourable `bevel` chamfers stayed bright across a 110° orbit while nearby boxwood went tan to black, and another model's head was a **detached island** 3.6 off the neck tip, exposed by silhouetting the joint against a bright backdrop. `health` cannot see the latter: the mesh was `isClosed: true` with `boundaryEdges: 0` because each island was closed on its own. The useful tests are **orbit and ask what stays put** and **silhouette the joint against something bright**.

## Diagnosis order

Cheapest first. Each step is about one call and rules out one cause; keep the expensive checks last.

1. **Is the mesh closed?** `model.validate` / `model.compile` return `health` with `isClosed`, `boundaryEdges`, `nonManifoldVertices`. `boundaryEdges > 0` means single-sided geometry that renders black from behind by construction, and the fix is thickness, not a flip.
2. **Re-shoot through [`render.capture_asset_preview`](render.capture_asset_preview.md).** It eliminates level lighting, competing lights and post-process in one call. If the artefact is gone it was the scene. **Do not stop at the default camera; see the next step, which uses the same call.**
3. **Move the camera to the opposite side.** Both in the asset preview and in a level. Black that *follows* the camera is a facing problem; black that stays put on the same faces is not. This is the single highest-yield step and it is nearly free.
4. **Turn the key light's shadows off** — `CastShadows: false` on the key alone, re-shoot. Anything that lights back up was never a facing question. Acne, contact-shadow staircase and self-shadowing all die here.
5. **Turn dynamic GI off** — `post_process.set_lumen_gi {enabled: false}`. Lumen's stochastic final gather can look like shadow acne in a still. In the measured rig, speckle across a model's neck and crown vanished while the rest of the frame stayed unchanged.
6. **Swap to the neutral material** — apply it per actor through `actor.spawn`'s `materialPaths` or `OverrideMaterials`. Every default material handed to an unbound slot (`WorldGridMaterial`, `DefaultMaterial`, `M_Grid`) carries a **world-space** grid texture; ribbing, corrugation, herringbone, moiré and stipple can all originate here.
7. **Only now, geometry.** And then it is a *subset* of faces rather than the whole shape — typically from `mirror`, which negates an axis without reversing triangle order, or from a sibling op appended where it should have been unioned.

**What the 7%-camera-move control cannot separate.** A camera nudge distinguishes screen-space from world-space effects, and nothing else. Shadow acne, shadow-map staircases, grid patterns, UV moiré and baked vertex colour are all camera-stable, so stability is not evidence of geometry. The control only ruled out TAA/SSR/SSAO ghosting.

**What the numbers cannot see.** In the measured A/B below, the whole surface changed while `meanLuminance` moved 0.9%. Triangle counts, `health` beyond `boundaryEdges` and orthographic silhouettes are identical across causes 2–6. Only a shaded perspective render sees them.

## Pick the capture surface first

**Default to [`render.capture_asset_preview`](render.capture_asset_preview.md) for a single asset.** It uses the Static Mesh editor's preview viewport, spawns nothing, does not dirty the level, closes afterwards, and accepts `location`, `rotation`, `fov`, `projectionMode` and the shared `exposure` pin. A single-asset review does not need a level actor.

Use a review level only when asset preview genuinely cannot do the job:

- **Two or more subjects judged against each other.** Each editor has its own preview scene: the same pinned EV produced `meanLuminance` 0.47 on one mesh and 0.18 on another, which says nothing about geometry. See [`visual-review`](visual-review.md).
- **A material swap.** Preview uses the asset's bindings and takes no override, so neutral and UV-checker steps need a level actor (`materialPaths` / `OverrideMaterials`, one non-destructive call) or a duplicated mesh. The actor is cheaper and leaves the committed asset untouched.
- **Anything judged in context** — scale against a neighbour, a prop on real terrain, a silhouette against the sky.

## The asset preview's own lighting, measured

Measured 2026-08-20 at `exposure: {mode:"fixed", ev100:-1}`, 800×800, against the example set. **The default preview camera looks straight at the unlit side**, which is what the table below records.

**The key is NOT on +X — re-measured 2026-08-21, and the earlier "+X" was wrong.** Orbit the camera round one subject at a fixed elevation and pinned `ev100: -1` and the lit band is in the **+Y** half: at camera azimuth **85°, 110° and 130°** (azimuth measured from +X toward +Y, camera looking back at the subject) the whole facing surface is lit, and at **−40°** and **−95°** it is a black silhouette at `ev100: -1` and still a black silhouette at `ev100: -4`. So the key sits near azimuth **110°**, and a camera on +X — the pose the old sentence recommended — is 110° off it and gets a half-lit subject with the far half at zero. Consequences for review:

- **Shoot the acceptance three-quarter from azimuth 85–95°.** That puts the key ~25° off the camera: enough to model form, close enough that nothing important falls into the black.
- **A camera anywhere in −Y cannot colour-judge anything.** Measured on the tower-like model: an `ev100: -4` shot of its stair side came back a pure black silhouette, `meanLuminance` 0.557 carried entirely by the backdrop.
- **To light the other side, move the key: pass `previewScene`.** `previewScene: {key: {azimuth: 290, elevation: 40}}` on the capture puts the key on the opposite half, scoped to that one call and restored afterwards, and the response reports the rig it was drawn under. Azimuth and elevation are where the light **arrives** from, which is how the engine's own default `(-40, -67.5, 0)` reads as 112.5° / 40°: [`render.preview-scene-rig`](render.preview-scene-rig.md). Prefer this to everything below.
- **Or spin the model, not the camera** — still the route when the rig is a level rather than asset preview. Copy the `.pwmodel` to scratch, add `rotate=(0, 0, 180)` to each `part` header, and compile to `/Game/ReviewScratch/`. A header with its own `rotate=` must have 180 **added** to its yaw, not replace it; a part offset by `at=` needs that `at=` mirrored because the header composes rotation before translation. This is how an external stair and door were made readable from an otherwise unlit orientation.

| Asset | Default camera | Opposed / lit camera |
|---|---|---|
| Large prop | 0.174 — black silhouette | 0.351 — fully readable |
| Rook-like model | 0.143 — black silhouette with a lit base | 0.285 — fully readable, no acne |
| Inverted sheet model | 0.167 — black silhouette | 0.255 |
| Twisted band | 0.148 — black silhouette | 0.255 — herringbone plainly visible |

Read that table as the mechanism behind three of the six false bugs. A default-camera asset-preview shot of a correct, closed, correctly-wound mesh is a black silhouette—the frame a reviewer gets from the obvious no-argument call.

What the switch to asset preview does and does not fix:

- **Fixed:** soft-shadow sampling noise (there is no `SourceRadius: 60` point light in the preview scene), competing lights and the level's post-process.
- **Not fixed:** material-borne artefacts. The twisted-band herringbone remains in the lit preview because the grid material is bound to the asset.
- **Not fixed:** a downward- or −X-facing surface still reads near-black. The preview scene has one key and very little ambient.
- **Partly fixed:** a mild shadow-map staircase remains on cast-shadow edges — much weaker than the point-light version, still visible at close range.
- **Not sprite-free.** The preview draws an axis gizmo bottom-left, a grid floor with coloured axis lines, and a photographic backdrop that changes with the camera. Do not read the backdrop as part of the subject, and do not present the frame as an acceptance shot without saying what the floor is.

## Suppressing editor decoration

`editor.set_game_view {enabled: true}` before a `render.capture_open_level` burst removes the axis gizmo, the grid, selection outlines, spline handles and component visualizers. Measured here: it cleared `splines`, `selection`, `selectionOutline`, `grid`, `volumes`, `lightRadius` and `audioRadius`, and the captured frames lost the gizmo and two stray overlay lines.

**It did not clear `billboardSprites`.** The verb's own response reports `overlayShowFlags.billboardSprites: true` immediately after a confirmed `gameViewEnabled: true`, so light-bulb and arrow icons still draw. Read that field rather than trusting the toggle. Game view is per-viewport state the verb does not restore itself — it returns `previous.gameViewEnabled` so you can put it back, and you should.

A dedicated `hideEditorSprites` boolean clears `EngineShowFlags.BillboardSprites` for one capture and restores it afterwards. Level-viewport verbs take it: `render.capture_open_level`, `render.capture_annotated`, `camera.frame_actor`, `camera.orbit_shots` and `camera.animation_shots`. Preview verbs answer `UNKNOWN_PARAMS` because their scene has no icon sprites; older editors do too, so fall back to `editor.set_game_view` when refused.

## The review level

`/Game/Review/L_ModelReview` — optional development content, not shipped with the plugin. It covers the three fallback cases above; for one asset, use asset preview.

- `Review_Ground` — `/Engine/BasicShapes/Plane` at scale 400, bound to a neutral ground instance (`BaseColor 0.14`, `Roughness 0.85`), with its far edge outside a small prop's frame.
- `Review_KeyLight` — movable DirectionalLight, pitch −42 / yaw −55, **Intensity 1.5**, `bAtmosphereSunLight: true`, `LightSourceAngle: 0.35`, `DynamicShadowDistanceMovableLight: 2000`, `DynamicShadowCascades: 2`. The short cascade distance keeps texels small enough to avoid acne on a 50 uu prop; the default 20000 spreads the map over a kilometre.
- `Review_SkyLight` — movable, `bRealTimeCapture: true`, **Intensity 1.0**, `bLowerHemisphereIsBlack: false`, providing neutral ambient fill.
- `Review_SkyAtmosphere` — neutralised: `RayleighScattering` white at scale 0.004, `MieScattering` white at scale 0.02, `MieAbsorption` zero, `MieAnisotropy` 0. Default blue ambient is a colour bias that can be mistaken for vertex colour or a material problem.
- `Review_RimLight` — RectLight, `CastShadows: false`, 300 cd, 600×600 source; shadowless by construction.
- `Review_ReflectionCapture` — SphereReflectionCapture, InfluenceRadius 2500. Without it, a `Metallic: 1` surface renders near-black, which is correct PBR rather than a broken binding.
- `PostProcessVolume_0` — unbound, `DynamicGlobalIlluminationMethod: None` via `post_process.set_lumen_gi {enabled:false}` for deterministic frames.
- `Review_MetalProbe` — a polished-metal sphere used for calibration: a non-mirror result means the capture is not resolving; clipping to white means exposure is wrong.
- `Review_CalibrationProp` — a neutral rook-like model. Frame it at `azimuth: 35, elevation: 18, ev100: -1` and expect `meanLuminance ≈ 0.375`; another number means the rig changed.

## Rebuilding the rig anywhere

The level is a convenience; this sequence is the deliverable. It works in any project, and it is what to run when `L_ModelReview` is absent.

```js
call({method:"level.create", args:{levelPath:"/Game/Review/L_ModelReview"}})
// creates, saves AND activates. If the package already exists it opens it instead.

call({method:"actor.spawn", args:{classPath:"DirectionalLight", actorName:"Key",
      rotation:{pitch:-42, yaw:-55, roll:0}}})
call({method:"actor.set_component_properties", args:{actorName:"Key",
      componentName:"LightComponent0", properties:{
        Mobility:"Movable", Intensity:1.5, CastShadows:true, bAtmosphereSunLight:true,
        LightSourceAngle:0.35, DynamicShadowDistanceMovableLight:2000,
        DynamicShadowCascades:2, CascadeDistributionExponent:2}}})

call({method:"actor.spawn", args:{classPath:"SkyAtmosphere", actorName:"Atmo"}})
call({method:"actor.spawn", args:{classPath:"SkyLight", actorName:"Sky",
      location:{x:0,y:0,z:300}}})
call({method:"actor.set_component_properties", args:{actorName:"Sky",
      componentName:"SkyLightComponent0", properties:{
        Mobility:"Movable", bRealTimeCapture:true, SourceType:"SLS_CapturedScene",
        Intensity:1.0, bLowerHemisphereIsBlack:false}}})

call({method:"environment.spawn_reflection_capture", args:{shape:"Sphere",
      name:"Refl", location:{x:0,y:0,z:150}, properties:{InfluenceRadius:2500}}})
call({method:"post_process.set_lumen_gi", args:{enabled:false}})
call({method:"editor.set_game_view", args:{enabled:true}})
```

Component names that are easy to get wrong: the light component is `LightComponent0` on every light actor, the sky light's is `SkyLightComponent0`, and the atmosphere's is `SkyAtmosphereComponent` — **no trailing zero**, and the wrong spelling returns `COMPONENT_NOT_FOUND` rather than a warning. `actor.set_component_properties` reports unknown property names in `warnings` and still succeeds, so read `applied` rather than assuming: `ShadowFilterSharpen` and `bCastShadows` were both silently rejected that way here.

## Light intensity: use a directional, and stop chasing a factor

A point or rect light's contribution falls as 1/d², so intensity has no meaning without distance and no constant multiplier converts it to directional intensity. Measured with the same 5000 lm point light at pinned `ev100: -1`:

- at ~490 uu from a 437 uu large prop — invisible, frame `meanLuminance` 0.00087.
- at ~85 uu from a 48 uu rook-like model — clipped to white with no surface form, `meanLuminance` 0.243.

Raising it did not help either: 250000 lm lit thin edge slivers against pure black, and 1500000 lm produced a flat white silhouette. There is no usable band because the band is a function of distance, not of the number. **A reported "roughly 50×" factor between a point light and a directional is an artefact of one particular distance and does not generalise.**

A **DirectionalLight's `Intensity` is in lux and is distance-independent**, so one value works for every prop at every size and every camera range. That is the whole reason the key here is directional. The working band in this rig, against a 0.18-albedo neutral material at `ev100: -1`: **1.5 lux key with a 1.0 sky light** lands a lit face around 0.55–0.65 sRGB with a readable shadow side. 2.6 lux with a 1.4 sky light already pushed lit faces past 0.85 and started clipping.

## The materials

Both live beside the example set. Install either with `material.compile_mgir` on the file text, then `material.authoring.compile_material` on the entry path; the first shader compile of a fresh master can block the editor game thread for minutes.

**`M_Review_Neutral`** (instances: 0.18-grey prop, 0.14-grey ground) — a flat parameterised grey with no texture, so nothing it draws can be mistaken for mesh behaviour. `BaseColor`, `Roughness`, `Metallic` and `Specular` are parameters; use an instance rather than another master. Roughness defaults to 0.55 so the specular gradient reveals curvature errors.

**`M_Review_UVChecker`** — the way a `uv` op becomes verifiable. `WorldGridMaterial`, `DefaultMaterial` and `M_Grid` are **world-space** and never sample a UV channel, so `mode=`, `scale=`, `channel=` and `split_angle=` are visual no-ops under them. This graph reads `TextureCoordinate` and derives the checker arithmetically — `frac(floor(uv * Tiles) · (1,1) / 2) * 2` lerping two greys — with no texture, sRGB, compression or mip chain to misdiagnose. Square shapes mean no stretch; equal squares mean even texel density; squares staying square across a seam mean no shearing. `Tiles` defaults to 8 and should be lowered when `uv scale=` is already large: a model with `scale=(0.02,0.02)` produced dense moiré, exactly what "fine corrugation" looked like.

`CoordinateIndex` is fixed at 0 and is not a parameter, so the checker always shows UV0 and never follows a `channel=1` lightmap unwrap. State that to yourself before reading the picture.

## The measured proof

One twisted-band actor, one camera, one light rig, one pinned `ev100: -1`, and 800×800 throughout. The only change between frames was `OverrideMaterials`.

- Under `WorldGridMaterial` (the asset's binding): a dense regular herringbone cross-hatch over the visible band. `meanLuminance` 0.2080.
- Under the neutral prop instance: the band is smooth; only the real 48-segment silhouette faceting and shading gradient remain. `meanLuminance` 0.2172.

The ribbing was the material, completely. And **`meanLuminance` moved 0.9%** while the whole surface changed appearance, which is the concrete reason a luminance metric is not a verification.

For contrast, the same subject family under an unlit level rig returned `meanLuminance` 0.0007 at pinned `ev100: -1` because its directional light pointed upward and there was no usable ambient. A rook-like model under a 5000 lm point light with `SourceRadius: 60` clipped to white with black stipple across the neck, collar and crown—the frame nearly filed as boolean garbage.

## Related pages

- [`visual-review`](visual-review.md) — choosing a capture surface, the one-scene rule for comparisons, and the exposure pin.
- [`model.authoring`](model.authoring.md) — writing the `.pwmodel` this rig reviews.
- [`model.vertex-color`](model.vertex-color.md) — the example material set these two sit beside, and why a stock material shows no vertex colour.
- [`render`](render.md) — capture verb reference, the exposure protocol, and the constant-capture-size hazard.
- [`render.preview-scene-rig`](render.preview-scene-rig.md) — aiming the asset preview's own key light for one capture instead of rotating the model, and the arrival-angle convention behind the 110° measured here.
- [`lighting`](lighting.md) — light spawn helpers and their limited `properties` set.
- [`material.mgir`](material.mgir.md) — the text IR both review materials are authored in.
