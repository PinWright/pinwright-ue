# volume

Place, inspect, resize, configure, and remove level volume actors, including blocking, trigger, post process, physics, audio, reverb, navigation, Lightmass, visibility, cull distance, and kill-Z volumes.

Use this namespace for volume-shaped editor-world actors whose bounds control gameplay, rendering, audio, navigation, or lighting behavior.

### volume.set_volume_properties

Every property this verb accepts is **class-specific** and applies only to a matching volume class:

- `bWaterVolume`, `fluidFriction`, `terminalVelocity`, `priority` → **PhysicsVolume** (create with `volume.create_physics_volume`)
- `bPainCausing`, `damagePerSec` → **PainCausingVolume** (`volume.create_pain_causing_volume`)
- `bEnabled`, `reverbVolume`, `fadeTime` → **AudioVolume** (`volume.create_audio_volume`; reverb volumes too)

The target volume must be the class that owns the properties you set. A plain `TriggerVolume` (what `volume.create_trigger_volume` makes), `BlockingVolume`, `PostProcessVolume`, etc. is **none** of those classes and supports none of these properties.

Behavior on a class mismatch:

- **Total mismatch** (the volume's class supports *none* of the requested properties): the call **fails** with `CLASS_MISMATCH` rather than reporting a misleading success on a no-op. The error data carries `actorClass` (the volume's real class) and `skipped` (the property names that were dropped). Pick the matching create verb above and target a volume of the right class.
- **Partial mismatch** (e.g. a PhysicsVolume given both physics and audio properties): the matching properties are applied and the response succeeds, but a `skipped` array lists the properties that were dropped because they belong to a different class — so the omission is explicit, not inferred from a short `propertiesSet`.
- **Full match**: `propertiesSet` lists the applied properties and no `skipped` array is present.
