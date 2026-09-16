# gas

Create and wire Gameplay Ability System assets: Gameplay Ability, Gameplay Effect, Gameplay Cue, AttributeSet, execution calculation, ability-set data, gameplay tags, modifiers, costs, cooldowns, targeting, and instancing. Use it for GAS asset authoring; add an AbilitySystemComponent with `call("blueprint.scs.add_component")`, and reach for `call("game_framework")`, `call("character")`, or `call("networking")` for the surrounding game rules, character movement, or replication setup outside GAS.

## Gameplay tag registry authoring

Use `call("gameplay_tags")` for registry-level tag declaration, source management, and `FGameplayTagQuery` construction; then use `gas.*` for applying tags to GAS assets. Registry mutation belongs to `IGameplayTagsEditorModule`: add/remove tag entries through the editor module APIs, and read listing metadata from `UGameplayTagsManager::GetTagEditorData` so rows include source, comment, and explicitness information.

The current gameplay-tags namespace is still IN-REVIEW. A known unresolved review gap remains in source-scoped adds: `gameplay_tags.add(tag, sourceB)` must not globally short-circuit just because the same tag already exists in `sourceA`; it needs to ensure the requested source entry exists.

## Execution-calc captures (`gas.set_execution_capture`)

`gas.set_execution_capture` populates `RelevantAttributesToCapture` on a `UGameplayEffectExecutionCalculation` blueprint. Each `captures[]` entry is `{attribute, source, snapshot}`:

- `attribute` is **not** a free-form name. It must be `<AttributeSetClassPath>_C.<AttrName>` — the attribute-set **generated class** path, then the attribute, naming a real `FProperty` on a compiled `UAttributeSet` subclass (e.g. `/Game/GAS/AS_Combat.AS_Combat_C.AttackPower`). A backing AttributeSet asset with the named attributes must already exist; there is no pure-Blueprint capture that materializes the property under the hood.
- `source` is `"source"` (default) or `"target"`; `snapshot` is a bool.

Authoring order:

```
gas.create_attribute_set → gas.add_attribute (×N) → gas.create_execution_calculation → gas.set_execution_capture
```

`gas.add_attribute` now **compiles** the AttributeSet for you, so the attribute lands on the generated class immediately — no separate `blueprint.compile` is required before `gas.set_execution_capture`. If you author the AttributeSet by other means and skip the compile, `set_execution_capture` returns `ATTRIBUTE_NOT_FOUND` ("…has no compiled property…"); the remedy is `blueprint.compile` on the AttributeSet so the property lands on the generated class. The same compiled-generated-class requirement applies to `gas.set_attribute_base_value`.

## Effect modifiers — binding the modified attribute (`gas.add_effect_modifier` / `gas.set_modifier_attribute`)

A GameplayEffect modifier is inert until bound. `gas.add_effect_modifier` accepts optional `attribute` (alias `attributeName`), while `gas.set_modifier_attribute` rebinds an existing modifier by `modifierIndex`. As with `gas.set_execution_capture`, the value must be `<AttributeSetClassPath>_C.<AttrName>` — a generated-class path naming a real compiled `FProperty` (e.g. `/Game/GAS/AS_Combat.AS_Combat_C.AttackPower`). The compile prerequisite applies; an unresolvable spec returns `ATTRIBUTE_NOT_FOUND` / `ATTRIBUTE_SET_NOT_FOUND` instead of an unbound modifier. Both echo the resolved `attribute`. Omitting `attribute` on `add_effect_modifier` preserves the legacy unbound (op + magnitude only) behavior.

## Granted tags & the cooldown-GE requirement (`gas.set_effect_tags`)

`gas.set_effect_tags` authors the tags a GameplayEffect **grants to its target**. Since UE 5.3 these tags live in a component — a `UTargetTagsGameplayEffectComponent` inside the GE's `GEComponents` — not in the deprecated `InheritableOwnedTagsContainer`. The handler writes the component model (find-or-add the component + `SetAndApplyTargetTagChanges`), so the live CDO's `GetGrantedTags()` reflects the tags immediately and repeated calls accumulate. Every requested tag must already be registered (e.g. via `gameplay_tags.add`); an unregistered tag is rejected with `INVALID_PARAMS` and named under `droppedTags` rather than silently dropped. `gas.get_gas_info` reads `grantedTags` back from `GetGrantedTags()`, so the readback matches what the engine sees.

This matters most for a **cooldown GameplayEffect**: a GE used as an ability cooldown must grant at least one tag, or compiling the owning ability trips UE's cooldown-GE validation — `CooldownGameplayEffectClass '<GE>' grants no tags. A GameplayEffect class must grant tags (Component: Grant Tags to Target Actor) to be used as cooldown.` (the default-on `AbilitySystem.WarnCooldownEffectWithoutTags` guard). Because the validator reads the component model, granting the cooldown tag through `gas.set_effect_tags` is what satisfies it. `gas.add_tag_to_asset` grants a single tag through the same component model for GameplayEffects.

## Inspect-after-mutate (`gas.get_gas_info`)

`gas.get_gas_info` reads a GAS asset back so you can confirm an authoring run without an `asset.dump` pivot. The fields depend on the asset's `gasType`:

- **GameplayEffect** — `durationPolicy` (raw enum int) plus a readable `durationPolicyName` ("Instant"/"Infinite"/"HasDuration"); `durationMagnitude` (the seconds `gas.set_effect_duration` wrote, present only for has-duration effects with a static magnitude); `stackingType`; `modifierCount` plus a `modifiers` array — one entry per modifier with `operation` (the engine op name, e.g. "AddBase"), `magnitude` (the static scalar; omitted for AttributeBased/Custom/SetByCaller magnitudes), `magnitudeType`, and `attribute` (the bound attribute name from `gas.add_effect_modifier`/`gas.set_modifier_attribute`, empty when unbound); `cueCount`; `executionCount`/`executionClasses`; and `grantedTags`.
- **AttributeSet** — an `attributes` array, one entry per declared attribute with `name` and `baseValue` (the default written by `gas.add_attribute`/`gas.set_attribute_base_value`).
- **GameplayAbility** — `instancingPolicy`/`netExecutionPolicy`, the wired `cooldownEffect`/`costEffect` class paths, and `abilityTags`.
- **ASC-owning actor** — `gasType:"AbilitySystemOwner"` and an `abilitySystemComponents` list (name + replicationMode).
- **GameplayEffectExecutionCalculation** — `gasType:"GameplayEffectExecutionCalculation"` and a `captures` array, one entry per `RelevantAttributesToCapture` element with `attribute` (the captured attribute name), `source` ("source"/"target"), and `snapshot` (bool) — the values `gas.set_execution_capture` wrote. Pass the exec-calc **asset** path, not the `_C` generated-class path.

So a `set_effect_duration` / `add_effect_modifier` / `set_modifier_magnitude` / `add_attribute` / `set_execution_capture` run is confirmable from this tool — `asset.dump { assetPath }` -> `properties.json` is only needed for fields outside this summary.

## See also

- [`gameplay_tags`](gameplay_tags.md) for registry mutation and `gameplay_tags.build_query` query authoring.
- [`ai`](ai.md) for StateTree transition event tags and EQS/AI authoring surfaces.
- [`asset`](asset.md) for content-browser-level asset operations before domain-specific edits.

### gas.create_ability_set

`setPath` (or its `assetPath` alias) is a **package path**, not an object path, and it is validated with `FPackageName::IsValidLongPackageName` before anything is loaded or created. A `.`, `\`, `:`, `..`, a `//`, a trailing slash or a root that is not mounted is rejected `INVALID_ARGUMENT` with the engine's own reason text quoted — so `/Game/Sets/AS_Hero` is accepted and `/Game/Sets/AS_Hero.AS_Hero` is not. A value with no leading slash is still resolved under `/Game/` first, so `Sets/AS_Hero` keeps working.

That check is not naming pedantry. `CreatePackage` logs a name containing `//` at **Fatal**, a verbosity that is not compiled out in any configuration: the call did not fail, the editor **process** died with every unsaved package in it. Two shapes reached it here — a `//` typed by the caller, and any rooted-but-unmounted path, because the `/Game/` fallback manufactured the double slash itself (`/NotAMount/X` became `/Game//NotAMount/X`). The check sits above the already-exists lookup as well as above the package creation, because loading a path with no `.` retries it as `<path>.<shortname>` and reaches the same `CreatePackage` from inside the engine's name resolution.

When the path names an asset that is already there the verb answers `status: "already_exists"` and writes nothing.
