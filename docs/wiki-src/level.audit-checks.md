# level.audit-checks

Reference for [`level.audit`](level.audit.md): its checks, thresholds, ignore model, and structural blind spots.

## What the engine already does, and why the audit does not use it

This is not a duplicate of Map Check; the comparison was verified against UE 5.8.

`AActor::CheckForErrors()` (`Actor.h:2720`, editor-only) catches four things: a deprecated or abstract class (and **returns early** if it found one, so a deprecated actor gets none of the other checks), a non-movable root primitive simulating physics, zero scale, and a fan-out to every *registered* component — which is where the useful null-static-mesh warning actually lives (`StaticMeshComponent.cpp:611`). Its zero-scale test is `IsNearlyZero(Sx*Sy*Sz)` on the **product** (`ActorEditor.cpp:1675`), so it fires on `(0.0001, 1, 1)` and cannot tell you which axis collapsed. Everything it produces goes into `FMessageLog("MapCheck")` with no output parameter and no injectable sink; the only way to read it back is the Slate log listing, which is page-scoped and subject to the listing's own severity filters. An audit whose completeness depends on a UI filter state is not an audit, so `level.audit` does not harvest it.

`UEditorEngine::Map_Check` is `private:` and not callable from a plugin at all; the only entry point is `GEditor->Exec(TEXT("MAP CHECK …"))`, which returns a bool and writes to the same listing. There is no Map Check commandlet.

One engine hook **is** wrapped rather than reimplemented, and it is the modern structured one: `AActor::IsDataValid(FDataValidationContext&)` (`Actor.h:3009`). It lives in CoreUObject, touches no Slate, fans out to every component, and is the hook `UEditorValidatorSubsystem` itself drives — so anything a project or plugin validator says about an actor arrives with it. That is the `data_validation` check, and it is **off by default**, because a third-party override can be arbitrarily expensive and this sweep must stay bounded.

Everything else has no editor-time engine equivalent. Confirmed absent in 5.8: an editor-time KillZ check (`AActor::CheckStillInWorld` is runtime-only, authority-gated, tests the *pivot*, and **destroys** what it finds); NaN/Inf transform reporting (`FTransform::IsValid()` exists as a primitive but nothing validates with it, and `DiagnosticCheckNaN_*` compiles to `{}` in every non-debug editor build); negative or mirrored scale; actors at the world origin; any notion of a play area; missing or default-substituted materials; anything below the terrain; and a general duplicate-at-identical-transform check — the engine has one only for `AStaticMeshActor`, and it needs a non-null mesh and a working collision query.

**World bounds deserve their own warning.** `HALF_WORLD_MAX` is not a usable lint bound on 5.8. Under Large World Coordinates `WORLD_MAX` is `UE_LARGE_WORLD_MAX` (8.796e12 cm), so `HALF_WORLD_MAX` is about 44 million kilometres and an actor a thousand km off the map passes it. `thresholds.worldBoundsCm` therefore defaults to **1,048,576 cm** (~10.5 km), which is simultaneously the old UE4 half-world size and `UE_FLOAT_HUGE_DISTANCE` — the largest distance a `float` still holds to 1/16 cm. The engine's own value is echoed as `engineHalfWorldMaxCm` beside it so the difference is visible rather than surprising.

## The checks

`checks` names them by id; `excludeChecks` subtracts. An unknown id is an **error**, never a skip — a typo that silently ran nothing is indistinguishable from a clean level.

| id | severity | default | needs | fires when |
|---|---|---|---|---|
| `nan_transform` | error | on | | the transform holds a NaN or infinity, or its rotation is not normalized (`FTransform::IsValid()`) |
| `zero_scale` | error | on | | any scale axis is at or below `zeroScaleEpsilon`, reported **per axis** |
| `negative_scale` | warning | on | | any scale axis is negative — mirrors the mesh, inverts normals and winding |
| `extreme_scale` | warning | on | | `|scale|` exceeds `maxScale` on some axis, **or** world bounds exceed `maxBoundsCm` |
| `no_mesh` | error | on | | a static or skinned mesh component has a null mesh asset |
| `missing_material` | warning | on | | a mesh material slot is null, or holds `WorldGridMaterial` / the engine default |
| `at_world_origin` | warning | on | | a visible actor sits within `originToleranceCm` of (0,0,0) |
| `below_kill_z` | error | on | | the actor's bounds **top** is below `KillZ − killZMarginCm` |
| `outside_world_bounds` | error | on | | the actor's bounds leave ±`worldBoundsCm` on some axis |
| `duplicate_transform` | warning | on | | ≥2 actors of one class share a location and rotation within tolerance |
| `outside_play_area` | warning | off | `playArea` | the actor's bounds centre is outside the stated area + margin |
| `below_surface` | error | off | `surface` | the surface is above the actor's **highest** point in every sampled column |
| `deeply_embedded` | warning | off | `surface` | more than `maxEmbedFraction` of the actor's height is under the surface while some is still above |
| `airborne` | warning | off | `surface` | the **closest** part of the actor is more than `maxGapCm` above the surface, or nothing was found beneath it |
| `balanced` | warning | off | `surface` | a large actor touches in fewer than `balancedMinContactPoints` footprint columns |
| `unsupported_assembly` | warning | off | `surface` | the actor rests on a stack whose own support ends in mid-air |
| `data_validation` | warning | off | | the engine's `AActor::IsDataValid` reported anything for this actor |

`deeply_embedded` stays opt-in even when you supply a `surface`, because on real content most of what it flags is deliberate.

**`airborne` vs `balanced` is a deliberate split.** `airborne` uses the **minimum** column gap: if any part of the actor touches, it is resting, however far the far side is off the ground on a slope. `balanced` then asks how many columns touch. A single-ray probe cannot produce either number, which is why a boulder resting exactly tangent on one corner used to be indistinguishable from a bedded one.

## Thresholds

Every threshold is overridable through `thresholds` and echoed in the response.

| threshold | default | why |
|---|---|---|
| `zeroScaleEpsilon` | 1e-4 | 1e-4 on a 1 m mesh is 0.01 mm — not a small object, an invisible one — and three orders above the float noise (~1e-7) around a scale of 1 |
| `maxScale` | 100 | 100× turns a 1 m prop into a 100 m one |
| `maxBoundsCm` | 50000 | measures the *result* rather than the multiplier, because 5× on a large mesh and 50× on a small one are the same defect. 500 m is far above any hand-placed prop and far below a landscape, which the default exempts. The observed defect here was a rim rock 4472 cm across — wider than the pit floor it bordered |
| `originToleranceCm` | 1 | |
| `duplicateToleranceCm` / `duplicateAngleToleranceDeg` | 1 / 0.5 | a paste that landed twice is bit-identical; the tolerance exists for round-tripped transforms, not for near-misses |
| `killZMarginCm` | 0 | the engine's own line. The check uses the bounds **top**, so an actor is reported only when all of it is under; the engine's runtime check uses the pivot and fires earlier |
| `worldBoundsCm` | 1048576 | see the LWC note above |
| `playAreaMarginCm` | 0 | centimetres added around the resolved `playArea` before `outside_play_area` reports an actor |
| `maxGapCm` | 2 | matches the shared contact tolerance, so "touching" means one thing across this plugin |
| `minCoverDepthCm` | 0 | "nothing of this actor is above the ground" |
| `deepCoverDepthCm` | 1000 | 10 m of cover is deeper than any authored bedding, so what is under it is far more likely to be at the wrong Z than deliberately set into the ground. Sets the `deep` flag for triage, nothing else - see below |
| `maxEmbedFraction` | 0.5 | more than half of it is under. Deliberate bedding is normally a few percent (`spatial.ground_actors` defaults to 0.02); a foundation is legitimately 1.0 |
| `balancedMinFootprintCm` | 100 | one contact column under a 20 cm pebble is a correct rest; under a 4 m boulder it is a glitch |
| `balancedMinContactPoints` | 2 | |
| `minCoverage` | 0.5 | below this the footprint overhangs a hole or the terrain edge and the gap numbers describe a fragment, so the ground checks report **unrunnable** rather than guessing |
| `contactToleranceCm` | 2 | shared with `spatial.verify_grounding` |

**Where `deepCoverDepthCm` 1000 comes from, and how to pick your own.** It is a **triage sort key, not a verdict**: it decides only whether a `below_surface` finding carries `deep: true`. It changes no finding, no severity and no `pass`, so a badly chosen value costs you ordering, never a missed defect or a false one. The general argument for 10 m is one of scale rather than of content: bedding a prop into terrain moves it by a fraction of its own height, and even a foundation sunk to its full depth is a building-storey affair, so a metre or two of cover is ordinary authoring while ten is not reachable by any of it - past that, the likeliest explanation is an actor pasted or imported at the wrong Z. The value was then checked against one project's hand-placed level, where it split the underground set roughly one-to-three, which is a useful triage split rather than evidence about levels in general. **If your content works at a different scale - a scaled-down level, a deep cave or mine network, a terrain whose datum sits far above the geometry - the number does not transfer.** Pick your own from your own data: run the audit once with the check on, read the `coverDepthCm` values out of `findings[]`, and put the threshold in the gap between the shallow cluster (bedding) and the tail (mistakes). Every threshold is echoed in the response, so the value a given run used is always recoverable from its own report.

**`surface.probeLift` defaults to 100000 cm here**, not the 500 cm the placement verbs use. A probe that starts below whatever buried an actor can never see it, which is exactly how underground actors measured as fine. A longer ray is not a more expensive query, so the lift is cheap; `surface.maxLayers` defaults to 16 for the same reason, since a landscape-only filter may have to peel foliage, roofs and fog cards on the way down.

## The ignore list

`ignore` is an **array of rules**, not one flat list, because "exempt the landscape from the size check" and "exempt this one rock from the burial check" are different statements and a flat list can only say "never look at this actor again" — which is how an ignore list quietly becomes a blindfold.

```js
ignore: [
  { classes: ["MyFoundationActor"], checks: ["below_surface"],
    reason: "footings are meant to be entirely underground" },
  { names: ["FX_Haze_*"], reason: "haze cards are not placed geometry" }
]
```

Each rule matches an actor when **every populated field** matches (AND). Fields: `actors` (exact label / internal name / object path), `names` (wildcard `*` `?` against label and internal name), `classes` (case-insensitive substring against the class **and every ancestor**, so one `LandscapeProxy` entry covers `ALandscape` and `ALandscapeStreamingProxy`), `tags` (`AActor::Tags`). `checks` limits the rule to those checks; omit it for all of them. An empty rule is rejected rather than silently matching nothing.

Every rule — built-in or yours — is echoed with its `reason` and the number of actors it **matched**, so an over-broad rule is visible in the report instead of just making findings disappear. It is deliberately *not* accompanied by a count of findings suppressed: a silenced check never runs, so the audit does not know what it would have found, and printing a guess there would be the same invented number the whole verb exists to stop.

**`ignoreTag` (default `PinWright.AuditIgnore`) is the durable per-actor exemption.** "This rock is buried on purpose" is knowledge that belongs with the actor: it survives renames, survives moving between levels, and cannot be expressed by a name pattern or a class. Per-call rules are for policy (a CI config, a one-off sweep) and are visible in the call itself; the tag is for individual authored intent. The audit only **reads** the tag — no verb here ever writes one, and the tag is a parameter so a project can use its own convention. An exemption stored in the level as a settings actor was rejected: it would make a read-only sweep mutate the map to be configurable, it does not travel into a CI config, and it hides the policy from anyone reading the call.

The built-in rules (disable with `useDefaultIgnores: false`) exist because the condition they silence is *correct* for that class, not because it was inconvenient: engine bookkeeping actors at the origin with no mesh; landscape size and origin; environment/lighting actors' ground relationship; and `InstancedFoliageActor`, whose own transform says nothing about where its instances are.

## What the audit cannot see

Stated here because a report that omits these silently is worse than no report.

- **Instanced-mesh instances.** Foliage instances are not actors. No check looks at one. The number skipped is in `caveats`.
- **Unloaded World Partition actors.** A `TActorIterator` sweep builds its set from `ULevel::Actors`, and an unloaded actor is in no such array. The response carries `worldPartition.unloadedActors` and a caveat; load the region before treating a clean report as complete.
- **Component-level transforms.** Every check reads the actor transform and the actor's summed bounds. A single mis-scaled component inside an otherwise correct actor is invisible unless it moves those bounds.
- **Anything past `limit` or `maxGroundActors`.** Actors past `limit` are not examined and `truncated` says so; actors past the ground budget are reported per actor as `AUDIT_TRACE_BUDGET_EXHAUSTED` **unrunnable**, never skipped.
- **Support chains that leave the audited set.** Scoping the audit to a subset makes `unsupported_assembly` unrunnable for anything resting on an actor outside it, by design.

Hidden and inactive sublevels **are** swept: the iterator deliberately drops `EActorIteratorFlags::OnlyActiveLevels`, which would otherwise skip them and report the level clean without having looked.
