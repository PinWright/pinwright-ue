# eqs

Author Environment Query System (`UEnvQuery`) assets — create a query, add generators and tests, set context classes, and configure test filter / scoring. Use `call("behavior_tree")` for the behavior trees that consume EQS queries via the Run EQS Query task; `eqs.*` is scoped to query-asset authoring only.

## Reading back / verifying a query

To confirm an authored query, `asset.dump` now writes an **`env_query.json`** sidecar (alongside `properties.json`/`meta.json`) that expands the topology the generic property walk could not reach. `UEnvQuery::Options` is a plain `TArray<TObjectPtr<UEnvQueryOption>>` (no Instanced specifier), so `properties.json` serializes it only as bare option object-ref path strings; the sidecar walks each option and surfaces its `generatorClass` plus each test's `testClass`, `purpose`, `filter` (type + float bounds / bool match), `scoring` (equation + factor), and `comment`. Read `env_query.json` from the dump directory to verify the round-trip in one file.

If you need a live (non-dump) read of a specific test field, `property.get` resolves the option's test subobjects directly — path shape `<EqsAsset>.EnvQueryOption_<g>:EnvQueryTest_<...>` (or via the `EnvQueryOption_<g>.Tests` array index) — reading `TestPurpose`, `FilterType`/`FloatValueMin`/`FloatValueMax`, `ScoringEquation`/`ScoringFactor`, and the context property.

### eqs.add_generator

`generatorType` accepts a short built-in name or a full generator class path. Built-in names (case-insensitive): `actorsofclass`, `oncircle`, `simplegrid`, `pathinggrid`, `composite`, `donut`, `blueprintbase`. Natural-language synonyms are NOT accepted — e.g. a "Points: Grid" generator is `simplegrid`, not `points_grid`. Any other value must be a full class path such as `/Script/AIModule.EnvQueryGenerator_SimpleGrid` (or a project Blueprint generator path). Rejections return `INVALID_ARGUMENT` and list the valid names.

### eqs.add_test

`testType` accepts a short built-in name or a full test class path. Built-in names (case-insensitive): `distance`, `trace`, `pathfinding`, `pathfindingbatch`, `dot`, `gameplaytags`, `overlap`, `random`, `project`, `volume`. Natural-language synonyms are NOT accepted — e.g. a line-trace test is `trace`, not `linetrace`. Any other value must be a full class path such as `/Script/AIModule.EnvQueryTest_Distance`. `purpose` is one of `filter`, `score`, `filter_and_score`.

### eqs.set_context_class

`contextClass` accepts a short built-in name or a full `UEnvQueryContext` class path. Built-in names (case-insensitive): `querier`, `item`, `navigationdata`, `blueprintbase`. The querier actor's context is `querier`, not `querier_actor`. A project-defined context (a Blueprint `UEnvQueryContext` subclass) is not a built-in — pass its full asset/class path, which you can locate with `asset.list`. Note `blueprintbase` resolves the *abstract* `EnvQueryContext_BlueprintBase` base, not a usable named context (e.g. a "Player" context). If no project context exists to locate — e.g. you need a "Player" context the engine does not ship — author one with `blueprint.create {parent: /Script/AIModule.EnvQueryContext_BlueprintBase}` and pass its generated `_C` class path here; there is no EQS-context-create verb (`ai.add_eqs_context` is a deprecated alias for this assign method, not a creator).

With `save:true`, the handler synchronously measures the package write and returns the standard `saveRequested`, `saved`, `saveState`, and `saveDetail` fields. `saved:true` means the new context assignment reached disk; a blocked or failed write returns `SAVE_FAILED` with `saved:false` and the measured save-state payload while leaving the in-memory mutation available for an explicit retry. With `save:false`, `saveState` is `notRequested` and the change remains memory-only.

### eqs.set_test_filter

Use exactly one discriminator, `filter.kind` or its compatibility spelling `filter.filterType`; supplying both is `INVALID_ARGUMENT`. Accepted values are case-insensitive: `minimum`/`min` requires numeric `min`; `maximum`/`max` requires numeric `max`; `range`/`float_range` requires both numeric `min` and `max`; `match`/`bool`/`boolean` requires boolean `value`. Each branch accepts only its discriminator and required value fields. Missing values, off-branch fields, and unknown fields are rejected before the query asset is loaded instead of being written ineffectively or discarded. Examples: `{kind:'float_range', min:125, max:650}` and `{kind:'bool', value:true}`.

### eqs.set_test_scoring

`scoring.equation` selects the scoring curve shape. Accepted equations (case-insensitive): `linear`, `inverse_linear`, `square`, `square_root`, `constant`. "inverse-linear" maps to `inverse_linear`, not `inverse`. Other scoring fields: `factor`, `clampMinType`/`clampMaxType` (`none`, `specified_value`, `filter_threshold`), `clampMin`/`clampMax`, `referenceValue`. `curve` is rejected on UE 5.6 (no `ScoringCurve` field).
