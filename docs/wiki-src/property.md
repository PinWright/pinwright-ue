# property

Reflection-based UPROPERTY accessors (`set`, `reset`, `get`, `list`) on any UObject — actors, components, asset CDOs, widget instances. The generic escape hatch when no typed setter covers the field; prefer typed routes like `widget.set`, `actor.set_blueprint_variables`, or `actor.describe` first since they bundle slot semantics, BP variable shapes, or whole-actor reads that hand-stitched `property.*` calls don't.

## Cross-cluster overlap

The target object is resolved by path (`/Game/...` for assets, level-relative for actors). For Blueprint asset properties, pass the Blueprint asset path directly; `property.list`, `property.get`, and `property.set` resolve `UBlueprint` paths to the Class Default Object (CDO), so callers see configured defaults instead of `UBlueprint` internals. The explicit `Default__` inner-object path still works when you need to target the CDO yourself.

For whole placed-actor inspection, prefer `call("actor.describe")` over hand-stitching `property.list` + per-component reads; it returns the actor plus all component modified-property state in the same compact shape used by map dump `actors/*.json`. But if you are deliberately staying on the `property.*` route, note that the fields you want often live on a **component**, not the actor — and `property.list` on the actor never lists its component subobjects. See the `property.list` and `property.set` sections below for the `actor.get_components` discovery recipe.

## See also

- `call("widget.set", …)` — typed setter for UMG widget properties (handles slot properties too).
- `call("actor.set_blueprint_variables", …)` — typed setter for Blueprint variables on a spawned actor.
- `call("system.inspect.inspect_object", …)` — inspect a UObject's properties read-only without touching the reflection writer.
- [`environment`](environment.md) — typed sky, cloud, and reflection-capture spawns return `actorPath` for follow-up `property.get` / `property.set`.
- [`asset.dump`](asset.dump.md) / [`asset.dump_folder`](asset.dump_folder.md) — repeatable `properties.json` baselines with inheritance and override markers.
- [`safe-mutation-save`](safe-mutation-save.md) — choose between typed writers, `property.*`, and `container.*`, then save and verify.
- [`runtime-uobject-inspection`](runtime-uobject-inspection.md) — end-to-end PIE workflow for reading reflected properties on live subsystems and other non-actor UObjects.

## Compiler-managed UUserWidget flags suppressed in dumps

The override-marker emitter in `PropertyUtils.cpp` (`ExportPropertyToJsonValueWithInheritance`) keeps a denylist for three `UUserWidget`-base compiler-managed bool flags that the WBP compiler unconditionally rewrites on every WBP CDO regardless of authored state:

- `bHasScriptImplementedTick`
- `bHasScriptImplementedPaint`
- `bAutomaticallyRegisterInputOnConstruction`

A byte-compare against the parent CDO always reports them as overridden, producing pure noise on every WBP `properties.json`. The filter is keyed on `Property->GetOwnerClass() == UUserWidget::StaticClass()` AND `Property->GetFName()` in that set. It applies automatically to every WBP — no per-asset config. If you add similar engine-managed compiler-touched flags to the denylist later, key them the same way.

## Preview-scene settings are UNSUPPORTED through this namespace

`UAssetViewerSettings`' CDO is addressable by path here, so `property.set` can reach the asset-editor preview profiles — the key light rotation and intensity, the sky, the floor, the backdrop cubemap. **Do not.** It is unsupported, not gated, and the distinction is deliberate.

Three things go wrong, and since the notification fix the third is the one that matters most. The settings object is **process-wide** — it is a CDO, and `UAssetViewerSettings::PostEditChangeProperty` broadcasts to every listener (`AssetViewerSettings.cpp:166`), one of which every open asset editor registers (`AdvancedPreviewScene.cpp:47`) — so a write leaks into every live preview scene in the process rather than the one you are capturing, and the profile it lands on is picked by the **global** `UEditorPerProjectUserSettings::AssetViewerProfileIndex` (`AssetViewerSettings.cpp:154`), not by you. The per-scene cost is now smaller than this note used to claim: these verbs formerly finished with a bare `PostEditChange()`, whose nameless event `FAdvancedPreviewScene::OnAssetViewerSettingsRefresh` ORs into all four `UpdateScene` flags (`AdvancedPreviewScene.cpp:609`, `:630` — `bNameNone`), forcing a full four-way rebuild; since `B-property-set-container-empty-change-event` all thirteen of them emit a **named** event (`UtilityPropertyHandler.cpp:292`) and `FPropertyChangedEvent::GetPropertyName()` returns the leaf (`UnrealType.h:7065-7068`), so only the flag matching that property fires. The narrowing is real but partial — a whole-array write (`propertyName: "Profiles"`) names `Profiles`, which is in `EnvironmentPropertiesName` *and* forces `bUpdateSkyLight`, so the broad rebuild is one call away, and every scene is visited either way. And `UAssetViewerSettings` flushes to the **committed** `Config/DefaultEditor.ini` with no dirty check of its own: `SAdvancedPreviewDetailsTab`'s destructor calls `Save()` unconditionally (`SAdvancedPreviewDetailsTab.cpp:38-48`), which ends in `TryUpdateDefaultConfigFile` (`AssetViewerSettings.cpp:145`) and rewrites the whole 59 KB file around whatever your call left in the CDO. **The notification fix did not touch that mechanism**, which is why this stays unsupported.

The supported route is the scoped `previewScene` capture parameter, which touches only the light components, snapshots and restores the shared profile array, and publishes a digest of the config file on both sides: [`render.preview-scene-rig`](render.preview-scene-rig.md). This is the same division [`render.view-modes`](render.view-modes.md) draws between the `viewMode` parameter and `editor.set_view_mode`.

**Why documented rather than refused.** Gating would mean a class allow-list on a general-purpose reflection verb — a much larger contract change — and it would not hold: this object is reachable through any of the four resolution stages under a different path spelling. That is a knowingly incomplete answer and it is recorded as one.

### property.set

Discover the property name first with `property.list` — names are case-sensitive and match the C++ `UPROPERTY` declaration, not the Blueprint display name (e.g. `bHidden`, not "Hidden"). For struct fields, pass nested JSON matching the struct layout. For array properties, pass a JSON array; the call replaces the entire array.

**Nested `propertyName` paths — struct/object hops AND array-element subscripts.** The dotted `propertyName` resolver (shared by `property.get`/`property.set`/`property.reset`, `actor.get_component_property`, and the typed SCS/widget/blueprint/niagara/animation/gameplay-tag writers) walks three segment kinds: a struct member (`BodyInstance.CollisionEnabled`), a subobject hop into an `FObjectProperty` (`KeyType.BaseClass`), and an **array element index**. Index an array element with either the bracket form `Keys[2].KeyType.BaseClass` or the dotted-numeric form `SensesConfig.0.PeripheralVisionAngleDegrees` — both resolve element N (0-based) and then continue resolving the remaining `.field` segments against that element. An out-of-range index returns a `… out of range … (length N)` error; subscripting a non-array property returns `… is not an array …`; and an array hop with no index (`Keys.KeyType`) returns a `… without an element index (use 'Keys[N]' or 'Keys.N.<field>')` error. Note that whole-array reads of an object array (`TArray<U…*>`, e.g. AIPerception `SensesConfig`) still serialize as opaque `[{}]` elements with no per-element drill-in — reach the per-element fields via the `[N]`/`.N.` subscript instead.

**Properties that live on a component (not the actor):** a directional light's `Intensity`/`LightColor`/`Temperature`, fog `FogDensity`, sky-light `Intensity`, a mesh's material slots — these live on a **component** and are NOT reachable from the bare actor path. `property.list` on the actor lists only the actor's own fields and never enumerates its component subobjects, so you cannot discover the component name from it, and one wrong guess at the level-style path can silently resolve to the World. First call `call("actor.get_components", { actorName: "<actorName>" })` to read the component's real **name** — UE default subobjects are named `…Component0` (e.g. `LightComponent0`, `ExponentialHeightFogComponent0`, `SkyLightComponent0`), NOT `…Component` / `DirectionalLightComponent` / `LightComponent`. Then either target the level-style subobject path `<actorPath>:PersistentLevel.<Actor>.<Component0>`, or skip the path construction entirely and use the typed `call("actor.set_component_properties", …)` / `call("actor.get_component_property", …)` verbs, which take the actor plus the named component directly (no level-style path, no World-fallback trap).

For runtime instances the change is not persisted to the asset.

Use `blueprint.set_default` instead when the intent is specifically "set this Blueprint CDO default, compile, save, and read back the post-compile value." Use `property.set` for the generic UObject writer or one-off live edits.

`property.set` shares FText parsing with `widget.set`: string values for FText fields may be raw text or UE text macros. Supported macro forms: `NSLOCTEXT`, `LOCTEXT`, `LOCTABLE("TableId","Key")` (string-table backed FText), and `INVTEXT`. Use the macro forms when setting localizable text. Both `namespace` and `key` in NSLOCTEXT must be readable, stable strings — do not write hex GUIDs, even when carrying over from an existing entry that uses one.

**Reflected scalar conversion is strict and atomic.** Boolean, numeric, integer, byte,
and enum values are parsed into scratch storage before `property.set` calls `Modify()`
or writes the target. Numeric values must be finite; integer values must also be
integral, valid for the property's signedness, and inside that property's range.
Malformed strings and fractional or overflowing integers are refused with
`PROPERTY_CONVERSION_FAILED` instead of being coerced to zero, truncated, or wrapped.
On this failure path the property is unchanged and the object is neither dirtied nor
notified.

Property types that use Unreal's `ImportText` fallback have the same commit-after-
validation rule: the parser must consume the complete string, with no trailing input,
before the staged value is copied back. The copy commits one reflected value, so a
fixed-size native array write preserves its sibling elements. Incremental container
writes follow the related contract on [`container`](container.md).

For Widget Blueprint designer preview size, target the Widget Blueprint asset path, not a tree widget. Set `DesignSizeMode=Custom` and `DesignTimeSize=(X=Width,Y=Height)` for panels, chrome, list items, and popups that should not preview as full-screen.

Response fields:

- `applied` — `true` when the in-memory write succeeded.
- `markedDirty` — whether the target package is dirty as observed after the write, not what the `markDirty` param asked for. It reads `false` even with `markDirty: true` when the target's outer is the transient package, which can never be dirtied.
- `warnings` — present only when there is something to say. One case today: a write that leaves a component's `bAutoActivate` false. That flag is a CDO default of `true`, so the override is serialised into the `.umap` and the component never starts again on any later load — the level renders nothing from it until the flag is restored or something activates it explicitly. `value: false` states what the field holds; this states what it means. Derived from the flag read back off the component, so restoring `bAutoActivate: true` says nothing, and `actor.set_component_properties` / `actor.add_component` disclose the same write the same way.

`property.set` is a mark-dirty-only mutator — it does **not** write to disk, so neither field implies persistence (there is no `saved` field). The change lives only in the in-memory editor state until you save: call `editor.save_all` / `asset.save` (or `blueprint.set_default` for a Blueprint CDO), then verify. This is intentional so a batch of edits can be saved once at the end.

`markDirty: false` suppresses the dirty flag, not the write: the probe value stays live in memory behind a clean flag, so any later legitimate edit to that package persists the probe value along with it — `asset.reload` is the only correct way to discard it.

**The write runs the engine's own change notification.** `property.set` emits a
`PostEditChangeProperty` naming the property it wrote — the leaf plus, for a dotted
path, the top-level member that contains it — so class overrides that recompute
derived state from that property (material instance rebuilds, water-body MIDs,
primitive bounds, clamped ranges) actually run. When the resolved target is a
`UActorComponent` the component's render state is marked dirty as well, because a
named event alone does not refresh a renderer-backed component (its refresh normally
comes from the editor's `PreEditChange`/`PostEditChange` reregister pair, which this
verb deliberately skips — that path flushes rendering commands and reruns
construction scripts). The response still reports only `applied` / `markedDirty`:
neither field claims a downstream effect, and there is no measured "the renderer saw
it" signal.

**The notification follows the write across an object hop.** A dotted path whose segment
is an object reference (`SettingsInterface.LowerBound`) stores into that *inner* object,
so the event — and the render-state push — go to the inner object, and the "top-level
member" above is resolved against it (`LowerBound`, not `SettingsInterface`). A path that
crosses only structs (`BodyInstance.CollisionEnabled`) stays on the object you named. This
matters because the inner object's override is usually the only route to the derived
state: a PCG node's settings sub-object, not the node, is what broadcasts the change that
regenerates the graph. The same applies to every `container.*` mutator, which share this
notification path. Note that the response's `objectPath` still names the object you
addressed, and no response field reports which object was notified.

### property.reset

Use `property.reset` when the goal is "reset to class default", not "set the same value as the class default". The handler copies the CDO value with reflection and clears UE explicit override metadata so later exports and saves do not preserve a no-op override.

Required params:

- `objectPath` — UObject path or actor name. Blueprint asset paths resolve to the generated class CDO, matching `property.set` and `property.get`.
- `propertyName` — reflected property name, including dotted nested paths (struct/object hops and array-element subscripts `Foo[N]` / `Foo.N.field`; see `property.set`).

Optional params:

- `markDirty` — defaults to `true`; pass `false` for transient checks.

Response fields:

- `oldValue` — value before reset.
- `defaultValue` — class default value copied into the target.
- `wasOverridden` — whether the value or explicit override state differed before reset.
- `isOverridden` — always `false` on success.
- `applied` — always `true` on success; the in-memory copy landed.
- `markedDirty` — whether the target package is dirty as observed after the reset, not what the `markDirty` param asked for. Same field, same meaning and same implementation as on `property.set`, so `markDirty: false` answering `markedDirty: true` means the package was already dirty before the call and the reset did not clear that pre-existing dirt.

`property.reset` is a mark-dirty-only mutator, exactly like `property.set`: it does not write to disk, and neither field implies persistence.

Example:

```json
call("property.reset", {
  "objectPath": "/Game/MyGame/UI/WBP_HUD.WBP_HUD_C:WidgetTree.HintText",
  "propertyName": "Visibility"
})
```

The reset notifies the object exactly once, with `EPropertyChangeType::ResetToDefault`
on UE 5.6+ (`ValueSet` below), whether or not the explicit-override metadata path ran.

### property.list

The default output is the full property set including engine inheritance — narrow with `nameMatch` or `propertyNames` (below). There is no `filter` param on this verb; passing one is rejected with `[UNKNOWN_PARAMS]`. Useful as a discovery step before `property.set` when you don't know the exact field name.

**Component properties are not listed here — it lists only the target object's own fields.** `property.list` on an actor (or a Blueprint CDO) reflects only that object's class properties; it does NOT enumerate the actor's component subobjects, so a field that lives on a component (light `Intensity`, fog `FogDensity`, sky-light `Intensity`) will not appear here and the component subobject name (a `…Component0` default subobject) is not discoverable from this call. To reach component properties, run the `actor.get_components` discovery recipe in `property.set` above.

For repeatable property baselines or diff review, `asset.dump` / `asset.dump_folder` writes `properties.json` with inheritance and override markers. `property.list` is the one-off live read.

When given a Blueprint asset path, `property.list` reports the CDO's configured defaults. Use `call("blueprint.inspect")` instead for Blueprint structure: variables, functions, graphs, components, references, and decompiled logic.

`omitOversized` is an opt-in read shrinker for `property.get` and `property.list`. Default is `false`, preserving full serialization. When `true`, known large fields such as `InstancedStaticMeshComponent.PerInstanceSMData`, `AudioImpulseResponse.ImpulseResponse`, and `BodySetup.AggGeom` return the same placeholder object used by asset dumps, with `"$reason": "exceeds-llm-budget"` and a `type` string, instead of serializing the full payload.

`property.get` optional params:

- `includeDefault` — include class default value in the response, default `false`.
- `includeOverrideState` — include `hasDefaultValue` / `isOverridden`, default `false`.
- `includeMetadata` — include reflected type/editability metadata, default `false`.
- `omitOversized` — when `true`, known oversized current and default values are returned as omission placeholders, default `false`.

**Migrated `*_DEPRECATED` fields (UE 5.5+ material input pins):** when a field has been migrated out of its owner into a subobject, the owner keeps an inert `*_DEPRECATED` shadow. The canonical case is `UMaterial`'s input pins — `BaseColor`, `EmissiveColor`, `Opacity`, `Roughness`, `Metallic`, `Normal`, … — which moved to `UMaterialEditorOnlyData` in UE 5.5+. A top-level `property.get` on the intuitive name (`BaseColor`) resolves the deprecated shadow and serializes it as a benign-empty value (`Expression: null`), which looks unwired even when the pin is correctly populated. The live data lives one path segment away — read it via the nested `EditorOnlyData.<Pin>` path (`property.get` with `propertyName="EditorOnlyData.BaseColor"`), **not** the top-level name and **not** a colon-suffixed `:MaterialEditorOnlyData` inner-object path (that returns `[OBJECT_NOT_FOUND]` — the subobject is reached through the `EditorOnlyData` *property*, not as an inner object). `property.get` flags any deprecated-shadow resolution with `deprecated: true` in the response (and, when derivable, a `movedTo` hint such as `"EditorOnlyData.BaseColor"`), so a `deprecated: true` marker on a "null" read means you are looking at the dead shadow. For authoritative material output-wiring readback, prefer `material.decompile_mgir`.

`property.list` optional params:

- `includeAll` — include all reflected properties, default `false`.
- `includeReadOnly` — include non-instance-editable properties, default `false`.
- `includeTransient` — include transient properties, default `false`.
- `includeValues` — include current values, default `true`.
- `includeDefault` — include class default values, default `true`.
- `includeOverrideState` — include `hasDefaultValue` / `isOverridden`, default `true`.
- `includeMetadata` — include reflected type/editability metadata, default `true`.
- `omitOversized` — when `true`, known oversized current and default values are returned as omission placeholders, default `false`.

**The two name filters — both case-INsensitive.** Neither was documented here before; the only description was the param help, which claimed `propertyNames` was case-sensitive. It never was.

- `nameMatch` (string) — **substring** match on the UPROPERTY name, case-insensitive (`Contains(..., ESearchCase::IgnoreCase)`). Empty/missing = no filter.
- `propertyNames` (array) — whole-name **allow-list**, case-insensitive. It is a `TSet<FString>` membership test, and `FString`'s `operator==`/`GetTypeHash` are case-insensitive in UE, so `propertyNames:["bhidden"]` selects `bHidden`. This matches FName lookup semantics elsewhere in the plugin; because UPROPERTY names are unique per class regardless of case, the insensitivity cannot over-match the way an unanchored substring filter can. Empty/missing = no filter.

Both may be combined; a property must satisfy both to be returned. Unlike [`actor.list`](actor.md), neither takes `matchMode` / `caseSensitive` — `propertyNames` is a set-membership test rather than a single pattern, so the shared `NameMatch::FFilter` vocabulary does not apply.

**Inspecting Blueprint CDO defaults — worked example:**

```
# Read configured defaults on a Blueprint (auto-resolves to CDO):
call("property.list", { objectPath: "/Game/MyGame/Modes/B_MyGameMode" })
  -> returns CDO properties like DefaultPawnData, Actions, etc.

# Read a single CDO property:
call("property.get", { objectPath: "/Game/MyGame/Modes/B_MyGameMode",
                        propertyName: "DefaultPawnData" })

# For Blueprint structure (variables, functions, graphs) reach for blueprint.inspect:
call("blueprint.inspect", { assetPath: "/Game/MyGame/Modes/B_MyGameMode" })
```
