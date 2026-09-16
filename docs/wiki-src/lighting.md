# lighting

Configure level lighting systems and light actors, including spawned lights, skylight setup, exposure, shadows, ambient occlusion, global illumination, volumetric fog, and lighting builds.

Use this namespace for scene illumination and lighting-build operations; use `call("environment.control")` for the legacy sun or skylight intensity controls grouped under environment.

## See also

- [`rendering`](rendering.md) — for persistent renderer-setting writes (DefaultEngine.ini), pair the live `call("lighting.setup_global_illumination")` with `call("rendering.set_dynamic_gi_method")`.
- [environment](environment.md) for typed atmosphere, volumetric cloud, and reflection capture spawns that pair with light actors.
- [`level-building`](level-building.md) — creating and lighting a map as part of the whole build.
- [`level-review`](level-review.md) — judging the lighting result with comparable repeat captures.

### lighting.setup_volumetric_fog

Sets `bEnableVolumetricFog` on the level's `AExponentialHeightFog`, spawning one if the level has
none, and optionally writes the five tuned fields: `viewDistance` → `VolumetricFogDistance`,
`albedo` → `VolumetricFogAlbedo`, `emissive` → `VolumetricFogEmissive`, `extinctionScale` →
`VolumetricFogExtinctionScale`, `scatteringDistribution` → `VolumetricFogScatteringDistribution`.
Each is a raw component field write followed by one `MarkComponentRenderStateDirty`, so it reaches
the renderer without dropping to `property.set`.

**Inside `viewDistance` the fog colour is `albedo`, NOT `FogInscatteringLuminance`.** The
volumetric integration replaces the height fog's analytic inscattering within
`VolumetricFogDistance`, so `FogInscatteringLuminance` and `DirectionalInscatteringLuminance` apply
only *beyond* that distance. The field whose name makes it the obvious first thing an author
reaches for is the one the renderer does not sample in the region they are looking at — writing it
succeeds and changes nothing near the camera. `albedo` is the particle reflectiveness the
volumetric pass multiplies scattered light by; `emissive` is light the fog emits on its own, as a
density, so more of it accumulates the further you look through the fog. The component's
`bOverrideLightColorsWithFogInscatteringColors` is the opt-in that makes the volumetric pass use
the height-fog inscattering colours instead — off by default, and this verb does not touch it.

`albedo` takes linear `{r, g, b}` in 0-1 and is sRGB-encoded into the engine's 8-bit
`FColor VolumetricFogAlbedo`, the same conversion the details-panel colour picker applies;
`emissive` takes linear `{r, g, b, a}` and is stored unclamped as an `FLinearColor`.
`extinctionScale` scales how much light the particles absorb (1 is neutral, UI range 0.1-10);
`scatteringDistribution` is the phase anisotropy (0 scatters equally in all directions, 0.9
predominantly forward, UI range -0.9 to 0.9 — visible side-lit shafts need a value near 0).
Omitted fields are left at whatever the component already carries; a non-finite number is ignored
with a log warning rather than written.

**`enabled` means "volumetric fog will render", not "the flag is set".** The `r.VolumetricFog`
cvar vetoes the entire volumetric pass, and nothing readable off the actor records the veto —
`property.get bEnableVolumetricFog` returns `true` either way. The response therefore splits the
two apart:

| field | meaning |
|---|---|
| `componentFlag` | the write that happened: `bEnableVolumetricFog` read back off the component |
| `volumetricFogCVar` | `{cvar, found, value}` — the measured `r.VolumetricFog`; `value` is absent when the console registry does not carry it |
| `enabled` | `componentFlag && value != 0` — measured, never echoed |
| `cvarWarning` | present only when the pass cannot run, naming the remedy |

While `r.VolumetricFog` is `0`, `VolumetricFogAlbedo`, `VolumetricFogEmissive`,
`VolumetricFogDistance`, `VolumetricFogExtinctionScale`, `VolumetricFogScatteringDistribution`
and every light's `VolumetricScatteringIntensity` are inert. Fog values tuned against a capture
taken in that state are tuned against a renderer that is not running — and "this engine cannot
do god rays" is what the missing pass looks like from the outside.

**The veto follows shadow quality, not effects quality.** `BaseScalability.ini` sets
`r.VolumetricFog=0` in `[ShadowQuality@1]` and restores it in `[ShadowQuality@3]`;
`[EffectsQuality@*]` never mentions it. An editor at low shadow quality renders no volumetric fog
at all while every fog write still reports success.

**This verb does not change the cvar** — a scalability cvar is global state a verb must not flip
silently. Turn the pass on for the session with
`call("system.console_command", {command: "r.VolumetricFog 1"})` (a later scalability change can
zero it again), or durably with `r.VolumetricFog=1` under `[SystemSettings]` in the project's
`Config/DefaultEngine.ini`. Read it back with
`call("system.console.search", {query: "r.VolumetricFog", fields: ["name", "currentValue"]})`.

`r.VolumetricFog 0` is not a usable diagnostic: it produces the same "no visible change" for
"had no effect" and "was already 0". Only setting it to `1` proves anything.

**Same shape elsewhere.** `r.LightShaftQuality` is zeroed by `[PostProcessQuality@1]`, which makes
a directional light's `bEnableLightShaftBloom` / `bEnableLightShaftOcclusion` decorative in exactly
this way. Any component flag whose renderer pass a scalability cvar can switch off has this failure
mode; `enabled` here is measured, and so is `lighting.setup_light_shafts`. So are `spawn_light`'s `shadowsEnabled`, `set_ambient_occlusion`'s `enabled`, `spawn_sky_light`'s `effectiveIntensity` and `configure_shadows`'s `virtualShadowMaps`, which reads `r.Shadow.Virtual.Enable` back after writing it — so a write a higher-priority setter refused reports the value actually in force, and is omitted rather than zeroed when the console registry does not carry the cvar. (`set_exposure` was checked and is **not** in this class: `r.EyeAdaptationQuality` is 2 at every level.)

### lighting.setup_light_shafts

Sets `bEnableLightShaftBloom` (on `ULightComponent`) and/or `bEnableLightShaftOcclusion` (on
`UDirectionalLightComponent`) on one directional light. At least one of `bloom` / `occlusion` is
required — a call naming neither is an error, not a success echoing the current state. With no
`actorName`, the level must contain exactly one directional light; two or more is
`AMBIGUOUS_ACTOR_NAME` rather than a write to whichever the level lists first.

**`bloomEnabled` / `occlusionEnabled` mean "shafts will render", not "the flag is set".**
`r.LightShaftQuality` is a binary master switch despite the name, and its single read sits in
`ShouldRenderLightShafts` — the opening test of both `RenderLightShaftOcclusion` and
`RenderLightShaftBloom`, *above* the loop over the scene's lights. At `0` the component flags are
never read at all, and nothing on the light records that.

| field | meaning |
|---|---|
| `componentFlags` | `{bloom, occlusion}` read back off the component |
| `lightShaftQualityCVar` | `{cvar, found, value}` — the measured `r.LightShaftQuality`; `value` is absent when the console registry does not carry it |
| `bloomEnabled` / `occlusionEnabled` | `componentFlag && value != 0` — measured, never echoed |
| `cvarWarning` | present only when a flag is set and the passes cannot run |

While the cvar is `0`, `BloomScale`, `BloomThreshold`, `BloomMaxBrightness`, `BloomTint`,
`OcclusionMaskDarkness`, `OcclusionDepthRange` and `LightShaftOverrideDirection` are all inert, and
fog samples a white dummy occlusion texture.

**The veto follows post-process quality.** `BaseScalability.ini` sets `r.LightShaftQuality=0` in
`[PostProcessQuality@0]` **and** `[PostProcessQuality@1]`, restoring it at `@2` / `@3` / `@Cine`. So
`sg.PostProcessQuality 2` lifts it, as does
`call("system.console_command", {command: "r.LightShaftQuality 1"})` for the session, or
`r.LightShaftQuality=1` under `[SystemSettings]` in `Config/DefaultEngine.ini` durably. **This verb
does not change the cvar.**

**Nothing in the editor UI shows the veto.** Unlike `r.LightFunctionQuality` and `r.ShadowQuality`,
this cvar has no `EngineShowFlagOverride` entry, so the `LightShafts` show flag stays on while the
passes are off.

**Two other silent no-ops this verb refuses rather than performs.** Light shafts render for
*directional* lights only, so `bEnableLightShaftBloom` on a point, spot or rect light does nothing
despite being declared on `ULightComponent` and described otherwise in the engine tooltip — a
non-directional target is rejected. And both engine setters no-op on a registered `Static`-mobility
component, so a static light is rejected with the mobility named instead of written and read back.

### lighting.spawn_light

**`shadowsEnabled` means "this light's shadows will render", not "CastShadows is set".**
`r.ShadowQuality` at `0` makes `EngineShowFlagOverride` force-clear the `DynamicShadows` show flag
for the whole frame, above any per-light test, so the component's `CastShadows` bit is never
consulted — and it still reads back `true`. `BaseScalability.ini` zeroes the cvar in
`[ShadowQuality@0]` and restores it at `@1` and above, so `sg.ShadowQuality 1` lifts it, as does
`call("system.console_command", {command: "r.ShadowQuality 3"})` for the session, or
`r.ShadowQuality=3` under `[SystemSettings]` in `Config/DefaultEngine.ini` durably. **This verb does
not change the cvar.**

| field | meaning |
| --- | --- |
| `castShadows` | the component flag read back off the spawned light — `true` by default even when `properties.castShadows` was not passed |
| `shadowQualityCVar` | `{cvar, found, value}` — the measured `r.ShadowQuality`; `value` is absent when the console registry does not carry it |
| `shadowsEnabled` | `castShadows && value > 0` — measured, never echoed |
| `cvarWarning` | present only when a shadow-casting light cannot cast, naming the remedy |

While the cvar is `0`, `ShadowBias`, `ShadowSlopeBias`, `ShadowSharpen`, `ContactShadowLength`,
`CastVolumetricShadow` and every cascade setting on a directional light are inert.

### lighting.spawn_sky_light

**`intensity` is not what lights the scene.** This is the *rescaling* member of the
scalability-cvar family rather than a veto: `FSkyLightSceneProxy::GetEffectiveLightColor` multiplies
the component's light colour by `r.SkylightIntensityMultiplier` before the renderer sees it, and the
component keeps reading back the unscaled value. `BaseScalability.ini` sets the cvar to `0.8` in
`[GlobalIlluminationQuality@0]` and `1.0` at `@1` and above, so a low-GI-quality editor lights the
scene at 80% of every intensity written here. `sg.GlobalIlluminationQuality 1` restores it, as does
`call("system.console_command", {command: "r.SkylightIntensityMultiplier 1"})` for the session, or
`r.SkylightIntensityMultiplier=1` under `[SystemSettings]` in `Config/DefaultEngine.ini` durably.
**This verb does not change the cvar.**

| field | meaning |
| --- | --- |
| `intensity` | the component read-back, i.e. the write that happened |
| `skylightIntensityMultiplierCVar` | `{cvar, found, value}` — the measured `r.SkylightIntensityMultiplier`; `value` is absent when the console registry does not carry it |
| `effectiveIntensity` | `intensity * value` — measured; omitted entirely when the cvar was not measured, rather than echoed unscaled |
| `cvarWarning` | present only when the multiplier is not `1`, naming the remedy |

The spawned `ASkyLight`'s component is `Stationary`, so the scene proxy — and therefore the scale —
is in play. A `Static` sky light is baked by Lightmass instead and gets no proxy in the common case.

### lighting.set_ambient_occlusion

**`enabled` means "AO will render", not "an intensity above 0 was written".**
`ShouldRenderScreenSpaceAmbientOcclusion` requires `FSSAOHelper::GetNumAmbientOcclusionLevels() != 0`
— a straight read of `r.AmbientOcclusionLevels` — so at `0` the pass never runs and the volume's AO
fields are never sampled, while reading back exactly as written. `BaseScalability.ini` zeroes it in
`[PostProcessQuality@0]` and restores it to `-1` ("decide from the post-process settings") at `@1`
and above, so `sg.PostProcessQuality 1` lifts it, as does
`call("system.console_command", {command: "r.AmbientOcclusionLevels -1"})` for the session, or
`r.AmbientOcclusionLevels=-1` under `[SystemSettings]` in `Config/DefaultEngine.ini` durably. **This
verb does not change the cvar.**

| field | meaning |
| --- | --- |
| `intensity` / `radius` | the values applied to the volume; each present only when the call named it |
| `ambientOcclusionLevelsCVar` | `{cvar, found, value}` — the measured `r.AmbientOcclusionLevels`; `value` is absent when the console registry does not carry it, because `0` is itself the veto value |
| `enabled` | `applied intensity > 0 && value != 0` — measured, never echoed; present only when the call wrote an intensity |
| `cvarWarning` | present only when the volume has AO on and the pass cannot run, naming the remedy |

While the cvar is `0`, `AmbientOcclusionIntensity`, `AmbientOcclusionRadius`,
`AmbientOcclusionPower`, `AmbientOcclusionBias`, `AmbientOcclusionQuality`,
`AmbientOcclusionFadeDistance` and `AmbientOcclusionStaticFraction` are all inert.

### lighting.create_lightmass_volume

**Two verbs create an `ALightmassImportanceVolume`, and their parameters do not match.** This one
takes `size` (FULL size per axis, default `1000` cubed) and `name`, and no rotation;
`volume.create_lightmass_importance_volume` takes `extent` (a HALF-extent, default
`5000, 5000, 2000`), `volumeName` and `rotation`. Both now build real box brush geometry, so pick
whichever parameter shape suits the caller — but do not translate one call into the other
mechanically.

**Why this verb refuses instead of spawning a volume it cannot give an extent.** An
`ALightmassImportanceVolume` is an `ABrush`: its shape lives in a `UModel`, not in its transform,
and a volume spawned without one has bounds extent `(0,0,0)` that no actor scale can multiply.
`FStaticLightingSystem::GatherScene` synthesizes an importance region from the scene bounds only
`if (LightmassExporter->GetImportanceVolumes().Num() == 0)` — a COUNT, with no extent test on the
path — so an extent-less volume still counts as one, suppresses that fallback *and* the
`No importance volume found` warning inside it, and the whole level then bakes against a point.
A phantom is worse than no volume at all, so a size that cannot produce a region, or a brush build
that fails, is a hard error and nothing is left in the level.

| field | meaning |
| --- | --- |
| `measuredSize` / `measuredExtent` / `measuredCenter` | read back off the spawned actor's brush bounds (`AVolume::GetBounds`), never echoed — this is the region the next lighting build will use |
| `requestedSize` / `requestedLocation` | what the call asked for, named separately so it cannot be mistaken for a reading |
| `sizeWarning` | present only when measured and requested disagree by more than 1 uu |

`size` must be positive and finite on every axis; a flat box encloses zero volume and is refused
with `INVALID_ARGUMENT` before anything is spawned.
