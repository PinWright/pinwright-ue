# environment

Coordinate environment building and live level-world ambience, terrain, sky, fog, and time-of-day state. Use this namespace for environment work; use [`landscape`](landscape.md) for terrain-specific authoring and the focused `lighting` / `property` surfaces for their parts of the rig.

## Call the full verb name, not the legacy `action` dispatcher

`environment.build` is a legacy compound dispatcher kept for backward compatibility, and it **declares only one parameter, `action`**. Anything else you send alongside it — `name`, `location`, `landscapeName`, `layerName` — is rejected with `UNKNOWN_PARAMS` before the sub-action is ever reached.

**No sub-action works, including the ones that take no arguments.** There are two independent rejections, not one. Even a bare `{action: "create_sky_sphere"}` fails: most arms forward the raw payload unchanged, so the `action` key itself travels to the target verb, and no target declares `action` — the forwarded call is rejected with a second `UNKNOWN_PARAMS` at the far end. The entire cross-dispatch surface is therefore unreachable and always has been; treat it as removed.

Call the registered verb directly instead. `call("environment.build.create_sky_sphere", {...})` is a real method with its own parameters and works normally; `call("environment.build", {action: "create_sky_sphere", ...})` does not. The same applies to every other `environment.build.*` sub-action, and to terrain: the layer-paint verb is `landscape.create_procedural_terrain` — there is no `environment.create_procedural_terrain` to call. See [`landscape`](landscape.md). For console commands use `editor.console_command` / `system.console_command`.

## Snapshot round-trip is not available

There is no environment snapshot export/import verb: a whole environment look cannot be
persisted to a file and restored through a single call. This is a known capability gap.

To persist a look today, capture it manually — `actor.list` + per-actor `property.get`
for the sky / fog / lighting / time-of-day actors — and rebuild it on restore with the
typed spawn verbs (`environment.build.create_sky_sphere`, `environment.spawn_*`) +
`property.set`.

## Modern volumetric atmospherics + reflection capture spawn

Three typed spawn verbs cover the modern UE outdoor-lighting cluster; pair them
with `call("lighting.spawn_light")` (DirectionalLight) and `call("lighting.spawn_sky_light")` for
a full sky rig. Each accepts optional `properties`, applied to the primary
component through case-insensitive `FProperty` lookup. Unknown keys appear in
`rejected` without failing the spawn.

- `environment.spawn_sky_atmosphere(name?, location?, rotation?, properties?)`
  — spawns `ASkyAtmosphere`, walks `properties` against the actor's
  `USkyAtmosphereComponent`. Most-tuned UPROPERTYs: `RayleighScattering`,
  `RayleighScatteringScale`, `MieScatteringScale`, `MieAbsorptionScale`,
  `MultiScatteringFactor`.
- `environment.spawn_volumetric_cloud(name?, location?, rotation?, properties?)`
  — spawns `AVolumetricCloud`, applies properties to the actor's
  `UVolumetricCloudComponent`. Most-tuned UPROPERTYs: `LayerBottomAltitude`,
  `LayerHeight`, `TracingStartMaxDistance`, `TracingMaxDistance`, and `Material`
  (soft path to a `UMaterialInterface`).
- `environment.spawn_reflection_capture(shape, name?, location?, rotation?, properties?)`
  — `shape` is `"Sphere"` or `"Box"`. Spawns `ASphereReflectionCapture` or
  `ABoxReflectionCapture`; the apply loop targets the base
  `UReflectionCaptureComponent`, so shape-specific knobs (`InfluenceRadius` on
  sphere, `BoxTransitionDistance` on box) resolve through the derived class via
  reflection without a per-shape branch.

All three return `{ actorPath, actorLabel, className }` plus
`{ componentPath, componentName, componentClass }` for the component receiving
`properties`, along with the standard actor verification fields (`actorName`,
`actorClass`, `actorGuid`, `existsAfter`).

Read or tune other UPROPERTYs with `call("property.get")` /
`call("property.set")` against **`componentPath`, not `actorPath`**. These
component-owned knobs otherwise produce `PROPERTY_NOT_FOUND`; `componentPath`
is already returned by the spawn, so no `actor.get_components` hop is needed.

Spawn path gotcha: these verbs use `SpawnActorInActiveWorld`. In an interactive
editor, `UEditorActorSubsystem::SpawnActorFromClass` can return null for
`ASkyAtmosphere`, `AVolumetricCloud`, and reflection capture actors even though
direct spawning into the active editor world works. The shared helper therefore
falls back to direct `UWorld::SpawnActor` before treating the request as a
spawn failure.

## Static lights ignore intensity changes at runtime

`environment.control.set_sun_intensity` and `environment.control.set_skylight_intensity`
write `Intensity` directly, so the value always lands on the component and the level is
dirtied. But a light whose `Mobility` is **Static** does not re-render from that write —
the engine rejects dynamic data changes on Static lights, so the viewport keeps showing
the previously baked result until lighting is rebuilt.

Both verbs therefore return two additive fields:

- `mobility` — the light component's mobility at the time of the call (`"Static"`,
  `"Stationary"`, or `"Movable"`).
- `requiresLightingRebuild` — `true` when `mobility` is `Static`, meaning the new
  intensity is stored but will not be visible until a lighting rebuild.

Only author-set **Static** lights are affected. Stationary is not gated (and
`USkyLightComponent` defaults to Stationary), and `lighting.spawn_light` forces Movable,
so lights created through PinWright are unaffected. The verbs deliberately do not call
`SetMobility` to work around this — that would re-register the component and invalidate
the level's baked lighting.

## See also

- [lighting](lighting.md) for directional and sky light spawn helpers that pair with the atmosphere/cloud/reflection rig.
- [property](property.md) for chaining `property.get` / `property.set` from the returned `componentPath` (the spawned-component knobs live there, not on `actorPath`).
- [actor](actor.md) for general placed-actor operations after a typed environment spawn.
