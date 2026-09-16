# render.capture-time

Time-series capture: which kinds have a time axis, how unsupported kinds refuse, the `time` / `times` parameters, the measured `subjectTime` block, and what particle captures can promise between runs. The subject descriptor is [`render.capture-subjects`](render.capture-subjects.md).

## A capability a kind cannot support is a typed refusal, not silence

Asking for a static-mesh time series returns `UNSUPPORTED_ASSET_EDITOR` naming the missing time axis, not a still or argument-gate `UNKNOWN_PARAMS`. A skeletal mesh without `animation` gets the same typed refusal: it "can only be captured in its bind pose" and names `subject.animation` plus the skeleton it must bind to, so the next call is specified by the error.

The refusal is installed on the skeletal-mesh kind **unconditionally** and only replaced by a real time driver once an animation resolves, so a bind-pose subject can never answer a time request with a still.

Every refusal in this area uses a code that already exists — `UNSUPPORTED_ASSET_EDITOR`, `PREVIEW_VIEWPORT_NOT_FOUND`, `NO_ACTIVE_LEVEL_VIEWPORT`, `BOUNDS_EMPTY`, `ACTOR_NO_SKELETAL_MESH_COMPONENT` — so a caller matching on codes needs no new cases.

**`render.capture_asset_preview` reaches a subject's time axis.** Pass `time` (one instant in seconds) or `times` (an array). The resolver's setter drives Niagara by resetting, calling `AdvanceSimulation(N, fixedDt)`, then holding at `DesiredAge`; for a `skeletalMesh` with `animation`, it scrubs the animation. `timeSupported:false` produces that kind's typed refusal rather than a still of an unknown moment.

Two constraints on the shape of a series, both consequences of how the axis works rather than policy:

- **Every instant is captured from every camera**, so `times` combined with `count` or `views` is an instants × cameras set, bounded by the same 24-shot ceiling. Over it is `TOO_MANY_SHOTS` naming the product and both factors, never a truncation — a shortened set reads as a complete set of the wrong thing.
- **The subject is driven once per instant, not once per shot.** Every camera of one instant photographs one simulation run: `subjectTime.subjectDrives` counts the runs, and the shots sharing an `instantIndex` are the ones that share a run. This is load-bearing for particles specifically — advancing resets first, and with system determinism off the instance seed is `FMath::Rand()` on every reset, so per-shot driving would make the six sides of one instant six different effects that merely share a number.

`times` also works with explicit `location` / `rotation`: a fixed camera watching a system evolve is a valid series. Only `count` / `views` conflict with naming one camera.

The response adds `subjectTime` when — and only when — an instant was asked for: `instantCount`, `camerasPerInstant`, the measured `subjectDrives`, and one `instants[]` entry per requested moment carrying `requestedSeconds`, `driven`, and, where the provider publishes them, `tickCount` / `tickDeltaSeconds` / `achievedAgeSeconds`. **`achievedAgeSeconds` is read off the running simulation after the ticks, and it is not `requestedSeconds`:** the driver runs whole fixed steps and floors, so 0.5 s at the default 1/30 delta is 15 ticks and lands at 0.5 s, while 0.51 s is still 15 ticks. Read the achieved age, not the request, when comparing two frames.

Use [`render.capture_animation_preview`](render.capture_animation_preview.md) for an animation frame burst: it samples the animation's own frame rate and reports per-instant pose-change proof, neither of which Niagara has. [`camera.animation_shots`](camera.animation_shots.md) refuses every asset kind because its instants scrub a Level Sequence driving a *placed* actor.

## Particle captures carry no reproducibility promise

**There is no `reproducible` field and none is planned.** Niagara determinism has three scopes, all class-defaulting off (though a loaded asset may differ):

- `UNiagaraSystem::bDeterminism` + `RandomSeed`. With system determinism off, the instance seed is literally `FMath::Rand()` on every reset. Off is the **class** default, not what every asset loads with: `UNiagaraSystem::Serialize` forces this flag **on** for any system package saved before `FNiagaraCustomVersion::ChangeSystemDeterministicDefault`, since that was the older default — several stock Niagara template systems are that old. The response reports the flag it measured off the asset; do not predict it from the default.
- `FVersionedNiagaraEmitterData::bDeterminism` + `RandomSeed`. Its own guarantee is scoped to a **fixed** tick delta, and it is void against any change to the emitter's scripts.
- `UNiagaraComponent::RandomSeedOffset`, which latches at activate/reset only and whose own comment warns that setting it non-deterministically can break determinism for the whole system.

None of them covers GPU emitters — no GPU determinism knob exists anywhere in Niagara — or data interfaces that read world state.

So a Niagara capture **reports what it did**: tick delta, tick count, measured system/emitter `bDeterminism`, and a `reproducibilityWarning` naming the failing scopes — system off (the instance seed is `FMath::Rand()` on every reset), named emitters off, and named GPU emitters, which no Niagara determinism setting covers. There is no boolean: the internal verdict is hardcoded false and never serialised, so warning **absence** is the only signal; neither `reproducible` nor `timeReproducible` appears. Same-system captures at the same age may differ; treat a difference as evidence only after the warning is absent.

**`simulated` is a measurement of motion, not "a system instance exists".** It is true only when the age the instance reports actually moved forward across the drive: the provider reads the age before and after and publishes both (`startAgeSeconds`, `achievedAgeSeconds`). `tickCount: 33` beside `simulated: false` means 33 ticks were **asked for** and the simulation did not move, so those frames are not of the moment they claim. Five engine paths swallow the ticks with a perfectly valid system instance in hand: the instance is paused, the instance is complete or disabled, the system carries its own fixed tick delta and so does not consume the delta it is handed, `fx.Niagara.SystemSimulation.SkipTickDeltaSeconds` is set at or above the capture's delta, or the instance tick returns before advancing its age (attach component gone, completed mid-run, or dirty data interfaces forcing a reset that re-zeroes it). When `simulated` is false the provider names which one in `simulationStalledReason` — **read that before concluding the asset does not animate.** The provider still blocks on compilation, forces solo, and retries a bounded number of times with a rendering flush, but publishes the measured outcome. `subject.niagara` carries `simulated`, `ageMeasured`, `startAgeSeconds`, `simulationStalledReason` and `boundsProbesSimulated` / `boundsProbesAttempted`; `subjectTime.instants[]` carries per-instant `simulated`. Note `boundsProbesSimulated` counts probes the ticks *moved*: the first probe sits at t=0 with nothing to advance, so a live-but-frozen system scores 1 of 5, and 0 means no instance came up at all.

**Time is advanced by ticking, not by seeking.** The provider resets and runs `AdvanceSimulation(N, fixedDt)`, which forces solo mode and completes N manual ticks on the game thread before it returns. `SeekToDesiredAge` is throttled — 33 ms of simulation per frame by default — so a long seek spreads over frames a capture never gives it and the frame is photographed mid-seek.

**A GPU emitter whose bounds mode is not `Fixed` reports a 200 cm cube.** GPU sim targets get no dynamic bounds at all; they fall back to a hardcoded `FBox(FVector(-100), FVector(100))` regardless of the real extent. The scan walks the *enabled* emitter handles, keeps those that are `GPUComputeSim` and whose `CalculateBoundsMode` is anything but `Fixed`, and the `boundsWarning` names every one of them — so the message identifies which emitter to fix, not just that something is wrong. Set `CalculateBoundsMode` to `Fixed` on the emitter, or pass an explicit camera. A `radius` will **not** help here: it is ignored on the `niagara` kind.

## See also

- [`render.capture-subjects`](render.capture-subjects.md) — the `subject` descriptor, per-kind bounds, and which verbs take one.
- [`render.capture_asset_preview`](render.capture_asset_preview.md) — the verb that reaches the time axis.
- [`render.capture_animation_preview`](render.capture_animation_preview.md) — a frame burst over an animation, counted in the animation's own frames.
- [`camera.animation_shots`](camera.animation_shots.md) — instants scrubbed from a Level Sequence, so a *placed* actor only.
