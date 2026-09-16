---
type: reference
summary: "Rejected and deferred RPC hard-removal candidates from the PinWright shortcut audit, with per-method behavior notes and missing generic parity."
date: 2026-05-12
tags: [rpc, cleanup, audit, generic-primitives]
---

# PinWright Deferred RPC Cleanup Candidates

Scope: `Plugins/PinWright`

This document lists RPCs considered during the hard-removal audit but rejected from the execution plan because current generic APIs would lose behavior, exceed the 1-3 call replacement rule, or need a new primitive first. This is not a keep-forever list; each row names the argument that blocks removal now.

## Environment Dispatcher

| Candidate | What it does | Why removal was rejected |
|---|---|---|
| `environment.build` | Legacy action dispatcher for foliage, landscape, lighting, snapshot, delete, and environment helpers. | REJECTION VOID; REMOVAL APPROVED, NOT YET EXECUTED. The rejection assumed the action arms run and therefore carry behavior worth classifying. They do not run and never have: the dispatcher declares exactly one parameter, `action` (`EnvironmentHandler.cpp:293-298`), so `ValidateHandlerParams` (`RpcDispatcher.cpp:123-158`) rejects every other key with `UNKNOWN_PARAMS` before dispatch. A second, independent rejection kills even the no-argument arms — 13 of the 19 arms forward `Ctx.GetRawPayload()` verbatim, so the `action` key itself reaches the target verb, which does not declare it. No arm has ever been reachable, so there is no per-action behavior to classify and nothing to preserve. Tracked as `B-environment-build-dispatcher-rejects-forwarded-params`. |

## UI, Screenshot, and Viewport

| Candidate | What it does | Why removal was rejected |
|---|---|---|
| `ui.screenshot` | Captures viewport screenshots with UI-facing result/path behavior. | Canonical `editor.screenshot` is async/file-oriented and does not provide equivalent byte/base64 response behavior. |
| `editor.set_viewport_realtime` | Toggles realtime rendering on the active level-editor viewport. | It calls viewport client state directly; no confirmed generic viewport operation or console command has equivalent behavior. |

Required parity: canonical screenshot API with file and byte/base64 modes; generic active-viewport realtime setter.

## Asset and Material Gaps

| Candidate | What it does | Why removal was rejected |
|---|---|---|
| `asset.bulk_rename` | Applies prefix/suffix/search-replace and optional checkout across many assets. | Generic `asset.rename` cannot reproduce arbitrary batch transformation in 1-3 calls. |
| `asset.create_material_instance` | Creates a MIC from a `UMaterialInterface` parent and can initialize parameters. | REJECTION VOID; METHOD REMOVED. `material.authoring.create_material_instance` now takes the same inline `parameters` payload (with `material.authoring.set_material_instance_parent` for reparenting), closing the parameter-parity gap that blocked removal. |
| `asset.break_material_connections` | Breaks material expression input connections. | REJECTION VOID; METHOD REMOVED. `material.graph.break_connections` is the replacement for breaking material expression input connections. |
| `asset.get_material_node_details` | Returns material node inputs, connectivity, and type-specific details. | REJECTION VOID; METHOD REMOVED. `material.graph.get_node_details` is the replacement for reading material node inputs, connectivity, and type-specific details. |
| broad `material.authoring.add_*` removal | Removes all typed material expression helpers without per-helper classification. | Some helpers are simple expression constructors, but others encode behavior; future cleanup must list audited leaf helpers only. |

Required parity: batch asset rename, material instance creation with initial parameters, complete material graph disconnect/detail APIs, and per-helper material audit.

## Blueprint Rollback Gaps

| Candidate | What it does | Why removal was rejected |
|---|---|---|
| `blueprint.undo_last_compile` | Deletes nodes recorded by hidden last-compile state. | REJECTION VOID; METHOD REMOVED. `blueprint.undo_last_bpir` supersedes it, rolling back the nodes recorded by hidden last-BPIR state. |
| `blueprint.undo_last_bpir` | Deletes nodes recorded by hidden last-BPIR state. | Hidden last-BPIR-state rollback: generic node deletion works only if the caller retained all node GUIDs, so removal needs an operation-id or transaction-token rollback primitive. |

Required parity: operation-id rollback, transaction-token rollback, or explicit returned-GUID contract from compile/insert APIs.

## Navigation

| Candidate | What it does | Why removal was rejected |
|---|---|---|
| `navigation.configure_nav_mesh_settings` | Sets multiple `ARecastNavMesh` settings, including nested resolution params. | Needs batch/nested property support. |
| `navigation.set_nav_agent_properties` | Sets nav agent radius, height, slope, and step height. | Multi-field navmesh mutation; exact replacement needs batch/nested property support. |
| `navigation.create_nav_modifier_component` | Adds a `NavModifierComponent` to a Blueprint. | Could be generic SCS in pieces, but component class/default setup needs a per-method parity check before removal. |
| `navigation.set_nav_area_class` | Sets nav area class on a nav modifier component. | Needs component-template property mutation parity. |
| `navigation.configure_nav_area_cost` | Configures area class cost settings. | Mutates nav area configuration; requires property/class default parity proof. |
| `navigation.create_nav_link_proxy` | Spawns a `NavLinkProxy` actor. | Spawn is generic, but full link struct/default setup is not proven equivalent. |
| `navigation.configure_nav_link` | Updates simple/smart nav link struct data on an existing proxy. | Generic actor/property calls do not cover link struct mutation cleanly in 1-3 calls. |
| `navigation.set_nav_link_type` | Switches a nav link between simple and smart behavior. | Touches smart-link component state, not just actor properties. |
| `navigation.create_smart_link` | Spawns/configures a smart `NavLinkProxy`. | Needs spawn plus smart-link component/link data setup beyond current generic parity. |
| `navigation.configure_smart_link_behavior` | Configures smart-link behavior settings. | Needs component/link behavior mutation parity. |
| `navigation.get_navigation_info` | Aggregates nav system/build/navmesh/link/bounds info. | Requires a generic navigation inspector or multi-class query with selected fields. |

Required parity: generic nav link struct editing, component-template property mutation, and navigation inspector.

## AI and Interaction Recipes

| Candidate | What it does | Why removal was rejected |
|---|---|---|
| `ai.set_ai_movement` | Sets several movement properties and searches native/SCS component templates. | Exact replacement needs batch component-template property mutation. |
| `interaction.configure_interaction_trace` | Creates/configures interaction trace Blueprint variables/defaults. | Needs batch Blueprint variable/default creation. |
| `interaction.configure_interaction_widget` | Creates/configures interaction widget Blueprint variables/defaults. | Needs batch Blueprint variable/default creation. |
| `interaction.configure_door_properties` | Creates/configures door-specific Blueprint variables/defaults. | Domain recipe; exact replacement exceeds 1-3 generic calls. |
| `interaction.configure_switch_properties` | Creates/configures switch-specific Blueprint variables/defaults. | Domain recipe; exact replacement exceeds 1-3 generic calls. |
| `interaction.configure_chest_properties` | Creates/configures chest-specific Blueprint variables/defaults. | Domain recipe; exact replacement exceeds 1-3 generic calls. |
| `interaction.add_destruction_component` | Adds destruction component behavior and health variables to a Blueprint. | Needs component plus multi-variable/default setup in one generic batch. |

Required parity: batch Blueprint variable/default creation and batch component-template mutation.

## Game Framework

| Candidate | What it does | Why removal was rejected |
|---|---|---|
| `game_framework.set_default_pawn_class` | Sets default pawn class on a GameMode Blueprint/class. | REJECTION VOID; METHOD REMOVED. `blueprint.set_default` sets the class field generically, supplying the per-method parity the rejection was waiting on. |
| `game_framework.set_player_controller_class` | Sets player controller class. | REJECTION VOID; METHOD REMOVED. `blueprint.set_default` sets the class field generically. |
| `game_framework.set_game_state_class` | Sets game state class. | REJECTION VOID; METHOD REMOVED. `blueprint.set_default` sets the class field generically. |
| `game_framework.set_player_state_class` | Sets player state class. | REJECTION VOID; METHOD REMOVED. `blueprint.set_default` sets the class field generically. |
| `game_framework.configure_game_rules` | Configures multiple game-rule variables/defaults. | Multi-variable recipe exceeds 1-3 generic calls. |
| `game_framework.configure_round_system` | Configures round-system variables/defaults. | Multi-variable recipe exceeds 1-3 generic calls. |
| `game_framework.configure_team_system` | Configures team-system variables/defaults. | Multi-variable recipe exceeds 1-3 generic calls. |
| `game_framework.configure_scoring_system` | Configures scoring variables/defaults. | Multi-variable recipe exceeds 1-3 generic calls. |
| `game_framework.configure_spawn_system` | Configures spawn variables/defaults. | Multi-variable recipe exceeds 1-3 generic calls. |
| `game_framework.configure_player_start` | Configures placed player-start behavior/properties. | Actor/property pieces are generic, but full behavior needs per-method parity. |
| `game_framework.set_respawn_rules` | Sets respawn-related rules/defaults. | Multi-variable recipe exceeds 1-3 generic calls. |
| `game_framework.configure_spectating` | Configures spectating rules/defaults. | Multi-variable recipe exceeds 1-3 generic calls. |
| `game_framework.get_game_framework_info` | Aggregates game-framework configuration info. | Needs a generic inspection/query equivalent before removal. |

Required parity: per-method proof for simple class setters; batch Blueprint defaults and generic inspection for recipe/info methods.

## GAS

| Candidate | What it does | Why removal was rejected |
|---|---|---|
| `gas.add_ability_system_component` | Adds an AbilitySystemComponent to a Blueprint. | REJECTION VOID; METHOD REMOVED. The GAS audit found these handlers were stubs that invented unread Blueprint variables instead of wiring an ASC, so there was no real behavior to preserve. |
| `gas.configure_asc` | Configures ASC properties. | REJECTION VOID; METHOD REMOVED. The "component-template property mutation parity" premise was factually void: ASC `ReplicationMode` is a non-UPROPERTY member with no FProperty, so `SetReplicationMode` on the template only wrote transient in-editor state that is dropped on save and never copied to spawned ASC instances, leaving no reflected property to preserve (replication mode is runtime-only). |
| `gas.create_attribute_set` | Creates an AttributeSet Blueprint. | Asset/class creation recipe; generic parity not proven. |
| `gas.add_attribute` | Adds a gameplay attribute to an AttributeSet Blueprint. | Requires GAS-specific attribute/member setup. |
| `gas.set_attribute_base_value` | Sets a reflected attribute base value. | Needs property-path parity over GAS attribute structs. |
| `gas.set_attribute_clamping` | Configures clamping variables. | REJECTION VOID; METHOD REMOVED. The GAS audit found it was a stub that invented unread Blueprint variables, so no clamping behavior existed to preserve. |
| `gas.create_gameplay_ability` | Creates a GameplayAbility Blueprint. | Asset/class creation recipe; generic parity not proven. |
| `gas.set_ability_tags` | Sets gameplay tags on an ability. | Gameplay tag container mutation needs parity proof. |
| `gas.set_ability_costs` | Sets cost effect references. | GAS-specific references/defaults; generic parity not proven. |
| `gas.set_ability_cooldown` | Sets cooldown effect references. | GAS-specific references/defaults; generic parity not proven. |
| `gas.set_ability_targeting` | Configures targeting variables. | REJECTION VOID; METHOD REMOVED. The GAS audit found it was a stub that invented unread Blueprint variables, so no targeting behavior existed to preserve. |
| `gas.add_ability_task` | Adds ability task configuration. | REJECTION VOID; METHOD REMOVED. The GAS audit found it was a stub that invented unread Blueprint variables, so no ability-task behavior existed to preserve. |
| `gas.set_activation_policy` | Sets ability network execution policy. | Could be property-settable, but needs per-method enum/property parity proof. |
| `gas.set_instancing_policy` | Sets ability instancing policy. | Could be property-settable, but needs per-method enum/property parity proof. |
| `gas.create_gameplay_effect` | Creates a GameplayEffect Blueprint. | Asset/class creation recipe; generic parity not proven. |
| `gas.set_effect_duration` | Sets duration policy and magnitude. | Mutates GameplayEffect duration fields/structs; needs struct property parity. |
| `gas.add_effect_modifier` | Appends a GameplayEffect modifier. | Mutates `FGameplayModifierInfo` arrays; generic array-of-struct editing not proven. |
| `gas.set_modifier_magnitude` | Updates modifier magnitude data. | Needs nested struct/array mutation parity. |
| `gas.add_effect_execution_calculation` | Adds execution calculation class reference. | GAS-specific array/reference mutation; generic parity not proven. |
| `gas.add_effect_cue` | Adds gameplay cue configuration. | Gameplay cue/tag struct mutation needs parity proof. |
| `gas.set_effect_stacking` | Configures stacking behavior. | Multi-field GameplayEffect setup; needs nested property parity. |
| `gas.set_effect_tags` | Sets granted tags on a GameplayEffect. | Gameplay tag container mutation needs parity proof. |
| `gas.create_gameplay_cue_notify` | Creates a cue notify Blueprint. | Asset/class creation recipe; generic parity not proven. |
| `gas.configure_cue_trigger` | Configures cue trigger variables. | REJECTION VOID; METHOD REMOVED. The GAS audit found it was a stub that invented unread Blueprint variables, so no cue-trigger behavior existed to preserve. |
| `gas.set_cue_effects` | Adds effect reference variables to cue notify Blueprint. | REJECTION VOID; METHOD REMOVED. The GAS audit found it was a stub that invented unread Blueprint variables, so no cue-effect behavior existed to preserve. |
| `gas.add_tag_to_asset` | Adds a gameplay tag to a GAS asset. | REJECTION STANDS; METHOD KEPT (branch-fixed). Four of its five target branches perform real tag writes (GameplayAbility `AbilityTags`, GameplayEffect granted tags via the 5.3+ `UTargetTagsGameplayEffectComponent`, and the `GameplayCueNotify_Static` / `GameplayCueNotify_Actor` cue tag), so gameplay-tag container mutation still needs parity proof before removal. Only the fifth branch was broken: Actor-with-ASC dropped the requested tag and fabricated a blank, unread `OwnedGameplayTags` variable while falsely reporting `tagAdded:true`. That branch is deleted — an ASC-owning Actor is now rejected with `UNSUPPORTED_TYPE` (an ASC template has no authorable default-tag property). |
| `gas.get_gas_info` | Aggregates GAS information about an asset. | Needs a generic GAS/object inspector equivalent. |
| `gas.create_ability_set` | Creates an ability-set data asset. | Uses specialized asset/data setup not covered by generic asset create. |
| `gas.grant_ability` | Configures ability granting on an actor with ASC. | REJECTION VOID; METHOD REMOVED. The GAS audit found it was a stub that invented unread Blueprint variables, so no ability-grant behavior existed to preserve. |
| `gas.create_execution_calculation` | Creates a GameplayEffectExecutionCalculation Blueprint. | Asset/class creation recipe; generic parity not proven. |

Required parity: GAS-specific struct/tag/container editing, batch CDO/default mutation, generic asset factory coverage, and GAS inspection.

## Physics

| Candidate | What it does | Why removal was rejected |
|---|---|---|
| `physics.configure_vehicle` | Fed nonexistent editor exec commands (`CreateVehicle`, `AddVehicleWheel`, `SetEngineMaxRPM`, `SetGearRatio`, ...) to `GEditor->Exec` and only echoed its inputs back; it mutated nothing. | REJECTION VOID; METHOD REMOVED. The earlier rejection assumed those exec commands worked, but none of them exist in the engine, this plugin, or ChaosVehiclesPlugin, so the method could never configure a vehicle. Real vehicle authoring lives in the `vehicle.*` namespace (`ChaosVehicleHandler.cpp`), so no capability was lost. |

## Volume

| Candidate | What it does | Why removal was rejected |
|---|---|---|
| `volume.add_trigger_volume` | Adds a trigger volume at an existing actor location. | Needs get-transform plus spawn plus extent/attach setup; exceeds generic parity. |
| `volume.add_blocking_volume` | Adds a blocking volume at an existing actor location. | Same add-at-actor recipe gap. |
| `volume.add_kill_z_volume` | Adds a kill-Z volume at an existing actor location. | Same add-at-actor recipe gap plus typed properties. |
| `volume.add_physics_volume` | Adds a physics volume at an existing actor location. | Same add-at-actor recipe gap plus typed properties. |
| `volume.add_cull_distance_volume` | Adds a cull-distance volume at an existing actor location. | Same add-at-actor recipe gap plus typed properties. |
| `volume.add_post_process_volume` | Adds a post-process volume at an existing actor location. | Same add-at-actor recipe gap plus nested post-process settings. |
| `volume.set_volume_properties` | Applies several volume properties in one call. | Needs batch property mutation. |
| `volume.get_volumes_info` | Lists volumes/triggers with bounds and summary data. | Needs generic multi-class actor query with bounds fields. |

Required parity: spawn-with-attach/initial properties, brush/volume bounds support, batch property mutation, and generic actor query with bounds.

## Procedural Geometry, Sequencer, Animation, Render

| Candidate | What it does | Why removal was rejected |
|---|---|---|
| `geometry.create_stairs` | Generates linear staircase mesh topology. | `geometry.create_procedural_mesh` creates an empty mesh and does not recreate the shape. |
| `geometry.create_spiral_stairs` | Generates curved/spiral staircase mesh topology. | Same shape-generation loss. |
| `geometry.create_arch` | Generates arch mesh topology. | Same shape-generation loss. |
| `geometry.create_pipe` | Generates hollow pipe mesh topology. | Same shape-generation loss. |
| `geometry.create_ramp` | Generates ramp/wedge mesh topology. | Same shape-generation loss. |
| `sequencer.add_keyframe` | Adds a float key at seconds-based time. | Generic `sequence.add_keyframe` is frame-numbered; removal needs seconds support or display-rate conversion in 1-3 calls. |
| `sequencer.manage_track` | Adds/removes float-property tracks by binding GUID and property path. | Generic track APIs do not match the full contract. |
| `animation.create_blend_space` | Creates/configures a blend-space asset. | Generic authoring covers pieces but not exact grid/axis behavior in one call. |
| `animation.create_state_machine` | Creates a state machine with states/transitions/rules. | Non-trivial payloads exceed 1-3 generic authoring calls. |
| `render.nanite_rebuild_mesh` | Enables Nanite and rebuilds a static mesh with job/completion behavior. | `asset.nanite_rebuild_mesh` changes settings but does not provide equivalent rebuild/completion semantics. |

Required parity: shape-specific procedural parameters, seconds-based keyframes or display-rate helper, batch animation graph authoring, and canonical Nanite rebuild with completion.
