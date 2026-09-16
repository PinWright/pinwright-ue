# character

Configure Character Blueprint components and movement behavior — movement speeds, jump/crouch/sprint settings, capsule and mesh setup, camera component options, navigation movement, rotation rules, custom movement modes, and footstep effects.

Use this namespace for Blueprint asset configuration on Character-derived classes; runtime actor authority, replication, or local-control checks belong under `networking` or `session`.

## Verifying movement settings (`get_character_info` read-back coverage)

`get_character_info` is the `character` reader: it reads back the fields the `set_*`/`configure_*` methods write, so the natural "apply setters, then read back to confirm" loop closes in-namespace without a cross-namespace fallback.

It round-trips the movement-component values: `walkSpeed` (MaxWalkSpeed), `jumpZVelocity`, `airControl`, `orientToMovement`, `gravityScale`, `customMovementSpeed`, `groundFriction`, `brakingDeceleration`, and the `configure_nav_movement` nav-agent fields `navAgentRadius` (NavAgentProps.AgentRadius), `navAgentHeight` (NavAgentProps.AgentHeight), and `avoidanceEnabled` (bUseRVOAvoidance) — plus capsule dimensions and the jump/camera flags.

The `configure_footstep_fx` / `map_surface_to_sound` blueprint variables (`FootstepVolumeMultiplier`, `FootstepParticleScale`, `FootstepSoundMap`) are listed by name in the `movementVariables` array. To read their persisted **default values** (not just confirm the variable exists), inspect the blueprint variable defaults with `blueprint.inspect {blueprintPath, includeProperties:true}`.

`configure_movement_speeds` exposes `walkSpeed` and `runSpeed`, but UE characters have a single ground speed — both are aliases for `MaxWalkSpeed`. Pass only one; passing both is rejected with `INVALID_PARAMS`. A successful call echoes the applied `walkSpeed` in its result. For a genuinely separate run/sprint speed (a distinct field, `MaxCustomMovementSpeed`), use `configure_sprint`.
