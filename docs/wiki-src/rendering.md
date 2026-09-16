# rendering

Typed read/write surface for `URendererSettings` (the `UDeveloperSettings` subclass behind Project Settings → Engine → Rendering), persisted to `Config/DefaultEngine.ini` under `[/Script/Engine.RendererSettings]`.

Use `rendering.*` when a renderer change must survive restart and update Project Settings. For live-only CVar effect, use `lighting.setup_global_illumination`, `system.console_command`, or `render.lumen_update_scene`.

## Live vs persistent paths

For live-only, in-memory CVar changes:

- `call("lighting.setup_global_illumination")` — live GI CVar mirror, no persistence; shares `call("rendering.set_dynamic_gi_method")`'s `ApplyDynamicGIMethodToCVars` mapping.
- `call("system.console_command", { command: "r.Foo 1" })` — generic CVar escape hatch.
- `call("render.lumen_update_scene")` — recapture only.

Choose `rendering.*` when persistence matters; otherwise stay on the live paths.

## Caveats

- **Concurrent CVar overrides.** A `system.console_command` CVar (or console `r.Foo X`) at `Scalability` priority beats UPROPERTY `SetByProjectSetting` until the CVar-bind path runs again. The namespace updates ini/CDO; add a live path for guaranteed current effect.
- **Hot reload.** Some settings need shader recompile or map reload; this namespace does not force either.
- **Scope.** Only `URendererSettings`; a future `developer_settings.*` surface could cover other `UDeveloperSettings` classes.
- **Capture resolution is separate.** `render.capture_*` and most camera paths default **768 x 768**; `camera.orbit_shots` preserves **1024** for existing `count`/`angles`/canonical shapes and **640** for `views:"sides"`. See [`render`](render.md#default-image-geometry) and [`camera`](camera.md); project settings do not change per-call geometry.

### rendering.get_project_settings

Enumerates every `CPF_Config | CPF_GlobalConfig` UPROPERTY on `URendererSettings` through property→JSON reflection. Optional `filter` is a case-insensitive UPROPERTY-name substring. Response:

```
{ settings: { "<UPROPERTYName>": <jsonValue>, ... }, configFile: "<DefaultEngine.ini>" }
```

Read-only. Non-config editor-state UPROPERTYs are omitted because they cannot round-trip through the ini.

### rendering.set_project_settings

Bulk write of `{ updates: { name: value, ... }, save?: bool=true }`:

- Unknown name → reject with `reason: "unknown_property"`.
- Existing non-`CPF_Config | CPF_GlobalConfig` property → reject with `reason: "not_config_serializable"`.
- Otherwise → apply via `ApplyJsonValueToProperty` and broadcast `FCoreUObjectDelegates::OnObjectPropertyChanged` so open Project Settings refreshes.

When `save` is true (default) **and** at least one property applied, persistence uses `S->TryUpdateDefaultConfigFile()` (the Project Settings UI path) and checks both its result and the target file's presence. A refused or unverifiable write returns `SAVE_FAILED`; the error data still reports the in-memory mutation and measured save state. Use `save:false` for transient experiments that must not touch the ini.

Response:

```
{ applied: ["<name>", ...], rejected: [{name, reason}, ...], saveRequested: true, saved: true, saveState: "written", saveDetail: "...", savedTo: "<DefaultEngine.ini>" }
```

`savedTo` is present only after a successful measured write. With `save:false` (or no applied properties), the response reports `saveRequested:false`, `saved:false`, and `saveState:"notRequested"` without a destination claim.

### rendering.set_lumen_method

Typed convenience verb; builds an `updates` map and uses `set_project_settings`'s path. Knobs:

- `hardwareRT` → `bUseHardwareRayTracingForLumen` UPROPERTY.
- `reflectionMethod` → `Reflections` UPROPERTY enum: `None | Lumen | ScreenSpace`; `RT`/`SSR` alias `Lumen`/`ScreenSpace`.
- `softwareRTMode` → `LumenSoftwareTracingMode`: `Detail | Global`.
- `finalGatherQuality` → **CVar-only**; UE 5.6 `URendererSettings` has no such UPROPERTY. The handler writes the CVar and returns `finalGatherQualityCvarOnly: true`; use `system.console_command` if this is the only knob.

### rendering.set_dynamic_gi_method

Persistent sibling of `call("lighting.setup_global_illumination")`; accepts `method`: `LumenGI | ScreenSpace | None | RayTraced | Lightmass`.

- Always mirrors matching CVars for live effect, sharing `lighting.setup_global_illumination`'s path.
- With `persist:true` (default), writes `DynamicGlobalIllumination` (and `Reflections` for `LumenGI`) UPROPERTYs and saves `DefaultEngine.ini`; `configFile`/`savedTo` appear only when that write succeeds, while a refused write returns `SAVE_FAILED` with the measured save fields.
- `RayTraced`: UE 5.6 removed the token from `EDynamicGlobalIlluminationMethod`; persist as `DynamicGlobalIllumination=Lumen` + `bUseHardwareRayTracingForLumen=true`, while retaining legacy live-CVar integer 3.
