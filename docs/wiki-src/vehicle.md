# vehicle

Author Chaos Vehicles assets — create wheel assets, set wheel-asset properties, add/remove wheel setups on a wheeled-vehicle Blueprint, and configure suspension. Use `call("physics")` for general collision and physics-asset authoring; `vehicle.*` is scoped to the Chaos Vehicles plugin's wheel/suspension data shape.

## Finding the vehicle component host

`vehicle.set_wheel_setup`, `vehicle.remove_wheel_setup` and `vehicle.set_suspension` all take a `componentPath` naming the `UChaosWheeledVehicleMovementComponent` on a wheeled-vehicle Blueprint. Locate that Blueprint in one call:

```
call("asset.search", { query: "*", parentClassPath: "/Script/ChaosVehicles.WheeledVehiclePawn" })
```

`parentClassPath` matches native subclasses recursively, so an empty result is the authoritative "no wheeled-vehicle Blueprint exists here" answer. Do not use `asset.list` for this: a short class name (`WheeledVehiclePawn`) matches nothing, and an unfiltered Blueprint listing returns an over-limit payload on a content-heavy project.

Read the component name off the host with [`blueprint.scs.get`](blueprint.scs.get.md) or [`asset.dump`](asset.dump.md) rather than guessing it — `AWheeledVehiclePawn`'s native component is `VehicleMovement`, templates often rename it to `VehicleMovementComp`, and neither is `MovementComponent`.

These three shapes all address the class-default (archetype) component on the Blueprint, including the `.Default__<BP>_C:` CDO-instance shape those two read RPCs emit in their object-ref fields — they resolve to the same object, so an edit through any of them lands on every actor spawned from the Blueprint:

- `/Game/Vehicles/BP_Car:VehicleMovement`
- `/Game/Vehicles/BP_Car.BP_Car_C:VehicleMovement`
- `/Game/Vehicles/BP_Car.Default__BP_Car_C:VehicleMovement`

A placed actor is addressed separately as `MyCarInLevel:VehicleMovement` (actor label or name). That resolves to the world instance, not the archetype: the edit reaches only that one actor and does not touch the Blueprint.

## Suspension on a standalone wheel asset

`vehicle.set_suspension` reaches wheel CDOs by walking `WheelSetups[idx].WheelClass` from a movement component, so pointing its `componentPath` at a loose wheel asset returns `COMPONENT_NOT_FOUND` — there is no host to walk. To edit `SuspensionMaxRaise`, `SuspensionMaxDrop`, `SpringRate` or `SuspensionDampingRatio` on a wheel asset that no vehicle references yet, use [`vehicle.set_wheel_asset_property`](vehicle.set_wheel_asset_property.md), or pass the fields at [`vehicle.create_wheel_asset`](vehicle.create_wheel_asset.md) time.

## vehicle.set_suspension compile results

When one call touches several distinct wheel Blueprint classes, `vehicle.set_suspension` compiles each class once and returns every outcome in `compileResults[]`, keyed by `assetPath`. The top-level `compiled`, `status`, `compileErrors`, `compileWarnings`, and optional `reinstanced` fields aggregate the whole batch. If any wheel Blueprint fails, the RPC returns `COMPILE_FAILED` with `success:false` and keeps those aggregate and per-asset diagnostics in the error payload; a successful compile from another wheel cannot mask it.
