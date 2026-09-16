# post_process

Typed setters for the most-used `FPostProcessSettings` knobs on an unbound `APostProcessVolume`. Each finds (or spawns) an unbound PPV in the current editor world, writes the values, and flips the paired `bOverride_<Field>` to `true` so they take effect at composition time.

Use this namespace for the typed surface (color grading global, bloom, Lumen, AA, motion blur); for other PPV fields, use raw `property.set` and flip the matching `bOverride_<Field>` to `true` yourself.

## The bOverride foot-gun

Every value in `FPostProcessSettings` (~150 fields) is paired with a `bOverride_*` boolean. **Writing the value without also flipping the override silently no-ops** — the volume keeps the cinematic default. These typed setters exist to eliminate that double-call. If you ever reach for raw `call("property.set")` on a PPV field, you must also `call("property.set")` `bOverride_<Field> = true`; otherwise the change does not blend into the scene.

## Anti-aliasing is CVar-only

`r.AntiAliasingMethod` (0=None, 1=FXAA, 2=TAA, 3=MSAA, 4=TSR) and `r.ScreenPercentage` are not exposed as per-volume override fields on `FPostProcessSettings` — `ScreenPercentage` is `ScreenPercentage_DEPRECATED` and the AA method has no PPV override at all. `call("post_process.set_anti_aliasing")` writes both via `IConsoleManager`. They affect the whole editor viewport, not just one volume, and survive until overridden again.

Unlike the other typed setters on this namespace, `set_anti_aliasing` **does not touch any PostProcessVolume actor** — it neither finds nor spawns one. The response carries only the resolved values (`{ success, antiAliasingMethod?, screenPercentage? }`), not `actorName` / actor verification fields. Calling it on a fresh empty level will not leave behind an unbound PPV. Because both fields are optional, an empty call still returns `success:true` and changes no CVar; pass at least one field.

## Escape hatch for advanced fields

The typed surface covers color grading global, bloom, Lumen GI, Lumen reflections, anti-aliasing, and motion blur. For anything else (per-channel color grading splits, depth-of-field, chromatic aberration, vignette, film grain, screen-space-reflections legacy, path tracer, ray-traced shadows), use `call("property.set")` against the PPV — but remember to also set the matching `bOverride_<Field>` to `true`.
