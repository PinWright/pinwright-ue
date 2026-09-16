# niagara.forces

Closed-form tuning for the stock force stack: `VortexForce` + `PointAttractionForce` + `Drag` + `SolveForcesAndVelocity`. Read this before tuning any orbit, flock, swarm, or debris-cloud emitter built from those modules; reach for `call("niagara")` for the edit RPCs that write the inputs named here.

## The two equations

`VortexForce` supplies a tangential force `F` about its axis, `PointAttractionForce` supplies the radial pull, `Drag` is the only dissipation, and `SolveForcesAndVelocity` integrates:

```text
terminal speed      v = F / (Drag * mass)     when Drag.Ignore Mass = true
                    v = F / Drag              when Drag.Ignore Mass = false  (mass cancels)
equilibrium radius  r = v / sqrt(k)           k = PointAttractionForce.AttractionStrength
```

`Speed Limit` on `SolveForcesAndVelocity` clamps `v` *before* it reaches the radius equation. Once `F / (Drag * mass)` exceeds the limit, `v` is pinned at the limit and `r = SpeedLimit / sqrt(k)` for every particle regardless of `F`, `Drag`, or mass — the two force inputs stop having any effect on the shape, which is the usual reason a stack "will not respond to tuning".

Provenance: fitted from one measured emitter (`v` pinned at 330, `k` = 8, predicted `r` = 117 against a measured 120) and confirmed against the retuned asset's sim cache. Measure your own rather than reusing those numbers: spawn with `call("niagara.spawn_actor")`, capture a sim cache, and read the position attribute.

Write the inputs with `call("niagara.set_module_input")`.

## AttractionStrength is a spring constant, not an acceleration

`PointAttractionForce` grows linearly with distance, so `AttractionStrength` carries units of 1/s^2, not cm/s^2. Every neighbouring force module takes an acceleration and the parameter name reads like one; substituting it as an acceleration puts the predicted radius out by orders of magnitude. A value that looks negligible beside a `VortexForce` in the hundreds is in fact a stiff well — `k` = 8 collapses a 330 uu/s orbit onto a 117 uu ring.

## Drag.Ignore Mass is the only route from mass variance to speed variance

With `Ignore Mass` false, drag and the applied force both divide by mass, mass cancels out of `v`, and `InitializeParticle`'s `Mass Mode: Random` becomes a no-op on motion: the particles carry different masses and move identically. Randomising mass to break up a uniform group and leaving this switch alone produces no visible variation and emits no diagnostic. With `Ignore Mass` true, `v` is proportional to `1/mass` and `r` follows, so a mass spread becomes a speed and radius spread.

## A strong well erases the spawn distribution rather than ignoring it

`ShapeLocation` — and any other `<X>Location` module — does set the spawn positions, but an attractor pulls every particle onto `r = v/sqrt(k)` within a few seconds. Over a lifetime of tens of seconds the initial distribution is invisible in the steady state, and sweeping the spawn radius across its whole range then changes nothing measurable. That reads as "this input does nothing" and is a misdiagnosis of a strong attractor: weaken `k` and the spawn radius matters again. Compute the equilibrium radius before concluding any location input is dead.

## The equilibrium is isochronous

`omega = v / r = sqrt(k)`, independent of speed and mass. A harmonic well alone therefore gives differential radii but a rigid angular rate, which reads as one rotating solid rather than a group of independent agents. Curl noise at a frequency comparable to the group's size is what breaks that; noise much finer than the group jitters individual particles without touching the rigid-body read.

## See also

- [`niagara`](niagara.md) for the full RPC surface and the read-edit-validate workflow.
- [`niagara.authoring`](niagara.authoring.md) for emitter ownership and the un-exported NiagaraEditor helper inventory.
