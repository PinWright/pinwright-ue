# render.capture-exposure

Auto-exposure cancels the change you are trying to measure. Supported scene/viewport capture verbs take an `exposure` argument that pins it for the duration of one call; this page is why that matters, what the response's measured exposure block means field by field, and the one warm-up effect no exposure setting avoids. Verb-by-verb argument reference: [`render`](render.md), [`camera`](camera.md), and [`editor.screenshot`](editor.md).

## Pin exposure before comparing two captures

Auto-exposure is a live scalar gain over the whole frame, and it re-balances between shots. Measured against an unpinned capture, a real change is partly cancelled by the camera re-exposing — which is how an edit that *did* work gets recorded as a no-op, and on 2026-08-16 nearly became a false bug report against this plugin.

**Pass `exposure` on the capture itself.** The supported scene/viewport capture verbs take it — `render.capture_mesh`, `render.capture_open_level`, `effect.step_and_capture`, `render.capture_asset_preview`, `render.capture_annotated`, `render.capture_animation_preview`, `render.capture_ortho_tiles`, the `camera.*` shot verbs, and `editor.screenshot`. It is optional except on `render.capture_ortho_tiles`, where it is **required**: that scene-capture mosaic has no persistent view state, so an unpinned burst re-exposes at every seam. Elsewhere, omit it for auto exposure or pass `11` / `{mode: "fixed", ev100: 11}`; the pin is scoped to the call. `render.capture_mesh` owns and destroys its capture component, so its `restored:true` means no exposure state escaped that transient component rather than that a live viewport was rewritten and put back. `{mode: "auto"}` explicitly leaves auto exposure running and takes **no** `ev100`; `{mode: "auto", ev100: 5}` is refused with `INVALID_ARGUMENT` naming both halves. It used to drop `ev100` silently and return an auto-exposed frame, creating the uncomparable pair this parameter prevents. Use one `ev100` for every frame; multi-shot verbs read it once per set.

`effect.step_and_capture` uses these same `exposure` controls and the normal `viewport.exposure` readback as `render.capture_open_level`. It activates the placed Niagara component before the shared warm-up, waits for the viewport exposure/frame convergence measurement, then freezes editor-world time for the deterministic advance and final readback; exposure is not expected to converge while that freeze is active.

`editor.screenshot` uses the same input spellings. On its game/PIE branch, a last-priority, draw-scoped scene-view extension sets the view family's native fixed-EV100 override and matching manual fields directly on each view's final post-process settings after the normal camera, volume, stereo, and extension blends. The family override also takes precedence over `r.EyeAdaptation.MethodOverride`. The measured block is top-level `exposure` (`pinRequested`, `pinned`, `restored`, `viewCount`, and `ev100` when pinned), because that path has no render-viewport response wrapper; `restored:true` means the temporary extension was released without mutating persistent camera state. Its Level Editor fallback carries the normal `viewport.exposure` block. `ui.screenshot` remains a legacy wrapper and does not declare this parameter.

For render/camera captures, read `viewport.exposure` in the response rather than assuming: `pinned` is measured, not echoed. A pin is written but **not applied** to a frame the renderer treats as a debug view — post-processing or lighting show flags off, a collision or visualization mode, anything but `Lit` — and in that case the response comes back `pinned: false` with a `pinWarning` naming the reason. Those pixels used auto-exposure and cannot be compared with anything. The block also carries `restored` and `ev100` (the fixed EV100 in force while the pixels were drawn — true for a viewport a human pinned through the editor's own EV100 control too). The game/PIE `editor.screenshot` branch reports the corresponding measured fields in its top-level `exposure` block described above.

**`adapted` is a gain, `ev100Equivalent` is the number you pass back.**

`adapted` is the **linear scalar the tonemapper multiplies scene colour by** — not an EV100, and not a value `ev100` accepts. The two run in opposite directions: a *larger* gain is a *brighter* frame, a *larger* EV100 a darker one. `ev100Equivalent`, published beside it, is the same exposure as a stop value and is the one to feed back.

| field | unit | reuse |
|---|---|---|
| `adapted` | linear gain (`× scene colour`) | compare two *unpinned* captures: different values prove they are incomparable |
| `ev100Equivalent` | EV100 (stops) | pass straight back as `exposure: {mode: "fixed", ev100: …}` |
| `adaptedSource` | `"readback"` \| `"fixedPin"` | `readback` lags the drawn frame by a few frames; `fixedPin` is exact |

**Auto once, then pin at what it reported.** This is the supported way to expose N shots at whatever the scene resolves to:

```js
const probe = call({ path: "render.capture_asset_preview",
  args: { assetPath: "/Game/…", exposure: { mode: "auto" } } });
const ev100 = probe.viewport.exposure.ev100Equivalent;   // NOT .adapted
// every subsequent shot:
call({ path: "render.capture_asset_preview", args: { /* … */, exposure: { mode: "fixed", ev100 } } });
```

Measured 2026-08-19 on `/Engine/BasicShapes/Cube` at 128×128 — an auto capture resolved at gain **6.1011**, i.e. **EV100 −2.6091**:

| pinned at | decoded meanAbsDiff vs the auto frame |
|---|---:|
| nothing (a second auto shot — the noise floor) | 1.456 |
| `ev100Equivalent` (−2.6091) | **1.319** |
| `adapted` (6.1011) fed back as an `ev100` | 152.882 |

The round trip lands *inside* the auto-to-auto noise. The last row is what passing the gain costs: a frame at mean luminance 0.0117 against the auto shot's 0.6076 — and it fails silently, because pinning at EV100 6.1 is a perfectly valid request that succeeds and reports `pinned: true`.

`adaptedMeasured: false` means the renderer has no exposure to report for this viewport yet (nothing has drawn into it); `adapted`, `ev100Equivalent` and `adaptedSource` are then omitted rather than zeroed, because EV100 0 is an ordinary exposure and would read as a measurement.

**`adaptedReadbackPending` — a fact about the readback, not a verdict on the pixels.**

Present only when the renderer has completed no eye-adaptation readback, so no measured `adapted` gain is available. It is true for a young view, **every pinned capture** (pinning clears the `EyeAdaptation` show flag), and fully warmed frames (2 of 2 trials, 2026-08-19). It is not a warm-up detector; `viewport.warmup.settled` is the measurement.

The auto-probe above is where warm-up bites: a probe taken as the *first* shot into a freshly opened preview measures a mid-warm-up frame and pins every later shot at the wrong exposure. Take it into a viewport that reports `warmup.settled: true`.

Prefer the parameter to the older recipes. `lighting.set_exposure {minBrightness: 1.0, maxBrightness: 1.0}` still works but writes a PostProcessVolume into the level — it persists, it dirties the map, and it cannot reach an asset-preview viewport at all, which has no level. `r.EyeAdaptationQuality 0` through `system.console_command` is global and unscoped: it survives the call and silently flattens every later capture in the session unless something restores it.

Any luminance comparison, and any comparison across two scenes, is invalid without it. A control that changes **hue** and is measured **per channel** is exposure-proof by construction — no scalar gain can produce or mask it — while a brightness-shaped control is not. When you cannot pin exposure, report the least-squares best-fit gain beside the delta: a pair whose best-fit gain is ~1.000 proves the exposure never moved, and a difference that vanishes once the gain is divided out was never a content change.

**A pinned pair reproduces; it is not byte-identical. Compare captures within a tolerance, never for equality.** Pinning removes exposure drift, not renderer jitter. Measured 2026-08-18 on the quietest fixture — two back-to-back `render.capture_asset_preview` shots of `/Engine/BasicShapes/Cube` at 128×128, same `ev100`, nothing animating — **11676 of 16384 pixels differ, mean absolute difference 0.88 / 255, max 34, best-fit gain 1.0000**. The gain proves exposure did not move. Equality therefore fails on a correct pair, and ~71% of pixels differ from jitter alone, so use a magnitude (`image.compare`'s `meanAbsDifference` / `maxAbsDifference`), not a differing-pixel count. On this fixture a six-stop change (`ev100` −1 vs 5) scores 89.6, about 100× the noise floor. A headless `-unattended -RenderOffscreen` editor matches interactive output: measured 2026-08-21 in a warmed preview window, `meanLuminance` at `ev100` −1 was 0.3644 in both, the six-stop signal was 89.6 in both, and same-`ev100` noise was 0.84 vs 0.88. An earlier claim that headless output was dimmer, scored 23.1, and sometimes went black was wrong: one-second auto filenames overwrote back-to-back shots (~60 ms each), so every comparison was a file against itself. Give each capture an explicit `filename`, or ensure returned `path` values differ.

## `pinned` is about the viewport; `pinnedFrameUsable` is about the frame

These are two different questions and a capture can answer them differently. **`pinned` is a predicate over viewport state:** the fixed-exposure override was written, and the renderer is in a state where it applies it. **`pinnedFrameUsable` is a predicate over the returned pixels:** the frame that pin produced resolves enough luminance levels to be read. A pin driven past the end of the scene's range satisfies the first exactly — it was requested, it was applied, the renderer honoured it — and hands back a frame crushed to black. Every field in the block is true and the aggregate is misleading, which is why the frame question got its own field instead of being folded into `pinned`: callers branch on `pinned` today, and quietly narrowing what it means would have broken them silently.

| field | present when | true means |
|---|---|---|
| `pinned` | always | the override was applied and the renderer would honour it for a frame in this state |
| `pinnedFrameUsable` | a pin was **requested**, it was **applied**, and the pixels were **measured** | the resulting image can be read: it is neither `crushed` nor `blownOut` |
| `pinRangeWarning` | those three hold **and** `pinnedFrameUsable` is false | — (a string; see below) |

`pinnedFrameUsable` is emitted only when there is a pinned frame to judge. With no pin requested there is nothing to say; with a pin requested but not applied, `pinWarning` already reports that these pixels are auto-exposed and uncomparable, and a usability verdict on them would be answering the wrong question.

**`pinRangeWarning` is the one case nothing else covers.** `pinWarning` covers the inverse — a pin requested and *ignored* by the renderer — so without this string a caller holding an unreadable pinned frame gets `pinned: true` and no signal at all. It carries the same measurement text as the top-level `rangeWarning`: the level count, the threshold, the mean, and **which way to move `ev100`**. Note the direction, because the parameter runs backwards from brightness — a *larger* `ev100` is a *darker* frame, and shipping only the gain once sent a caller several stops the wrong way. Re-shoot at a different `ev100`; a comparison set built on this frame compares black against black.

Reading them in order: `pinned: false` → these pixels are auto-exposed, fix the view mode or show flags (`pinWarning` says which). `pinned: true, pinnedFrameUsable: false` → the pin worked and the exposure value is wrong, move `ev100` as `pinRangeWarning` says. `pinned: true, pinnedFrameUsable: true` → comparable.

`pinnedFrameUsable` is structurally safe against the trap the top-level `crushed` / `blownOut` fields need an explicit gate for (see [`render`](render.md), *A set reports its own shape*): it requires `pinned`, and the renderer never applies a pin to a non-lit frame, so it can never be reached on a wireframe or unlit capture whose two tone levels are exactly what the mode was asked to draw.

## The first capture into a fresh preview window is a stop dark, and `warmup.settled` does not say so

Take a throwaway capture with `closeAfterCapture: false`, then judge only the second and later shots in that same window. This is not the auto-probe case above — it applies to every pinned capture, and no exposure setting avoids it.

Measured 2026-08-21, identical camera and identical `exposure: {mode: "fixed", ev100: -1}` on both shots of each pair, same mesh:

| asset | first shot (this call opened the window) | second shot, same window |
|---|---|---|
| large prop | mean 0.1987, **min 0** | mean 0.3686, min 0.0347 |
| vessel-like model | mean 0.2491, **min 0** | mean 0.3655, min 0.0440 |

About nine tenths of a stop, and `viewport.warmup.settled` came back `true` on all four shots with `meanLuminanceDelta` under 4e-4. That flag watches the mean-luminance delta between redraws *within one capture*, which converges immediately while the scene's ambient contribution is still absent — so it cannot see this, and "take the shot into a viewport that reports `warmup.settled: true`" does not avoid it.

**Only the ambient is missing; the key light is there from the first frame.** On the amphora pair `maxLuminance` is identical to the last digit (0.9770211764705882) while `minLuminance` goes 0 → 0.0440 and `litPixelFraction` goes 0.9966 → 1.0000. That is why several notes in this corpus describe the asset-preview scene as having "effectively zero fill", with shadow-side faces pure black at any `ev100`: those were all first captures. The scene has fill.

Two consequences. An A/B that crosses a window open is invalid however carefully exposure is pinned — `pinned: true` is honest, the pin is applied, and the pixels are still wrong. And `assetEditorWasAlreadyOpen` in the response is the field that separates a cold shot from a warm one; read it on every capture in a comparison set.

## The preview profile changes what the pin lands on, and one suspected hole is not one

Asset previews are drawn under an `FAdvancedPreviewScene` profile, and which profile is a **per-user** setting. Two of the three shipped profiles turn tone mapping and post-processing off, which changes what a pinned frame looks like without changing whether the pin governed it. `viewport.previewScene` reports the profile by name and index, plus the three show flags below — read it on any comparison that crosses two editors, and pin `previewScene.key` / `.sky` explicitly if the profiles differ.

**`ShowFlags.Tonemapper == 0` is not a hole in the pin.** It selects the *gamma-only permutation* of the tonemapper rather than skipping the pass, and that branch still multiplies by the same exposure scale as the full path; the engine's own auto-exposure-debug predicate — which `pinned` deliberately mirrors — does not consult it either. So `pinned: true` under `Grey Ambient` is honest. What gamma-only removes is the film curve, the grading LUT, bloom, vignette and local exposure: the response curve, not the exposure. An earlier revision of this corpus suspected the opposite and widened the predicate accordingly; that would have reported `pinned: false` on frames the pin does in fact govern.

**The flag that does bite is `EyeAdaptation`.** Both grey profiles set `bPostProcessingEnabled = false`, which routes through `DisableAdvancedFeatures()` and clears `EyeAdaptation` while leaving `PostProcessing` set. A fixed-exposure setting still wins, but the profile's own auto-exposure min/max clamp is discarded silently; the three committed profiles in the example set carry `bOverride_AutoExposureMinBrightness=False` anyway. Read `viewport.previewScene.showFlags.eyeAdaptation`.

## See also

- [`render`](render.md) — the capture verbs themselves, the view-mode contract, and the two-renderer rule.
- [`render.preview-scene-rig`](render.preview-scene-rig.md) — the `previewScene` parameter, the profile report, and why per-capture profile selection is refused.
- [`camera`](camera.md) — pinning across a shot set rather than per shot.
- [`render.capture-subjects`](render.capture-subjects.md) — the `subject` descriptor, and `assetEditorWasAlreadyOpen`.
- [`image`](image.md) — `meanAbsDifference` / `maxAbsDifference` and the best-fit-gain check, the measures a pinned pair is compared on.
