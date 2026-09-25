# widget

Asset-side UMG authoring: create Widget Blueprints, edit their tree and properties, bind events, and
drive animations. Use `ui.*` for already-instantiated runtime widgets.

## Styling

Set a style property directly with `widget.set` (a `Button` needs a complete `FButtonStyle`
ExportText literal), or bind it at runtime through BPIR (`blueprint.compile_bpir`). Declare reusable
style **member variables** with `blueprint.add_variable` (`FButtonStyle`, `FSlateBrush`,
`FSlateFontInfo`, `FProgressBarStyle`, etc.). **Verify with `widget.describe` / `widget.export_xml`**;
a declared style variable is not automatically used.

## Cross-cluster overlap

`widget.bind_event` is **verify-only**: the `BndEvt__*` K2Node must already exist. Create a binding
with a `widget_event` entry through `blueprint.compile_bpir` (see `call("bpir")` and
`call("bpir.examples.widget-event")`), then optionally verify it with `widget.bind_event`. Prefer
the single-call `widget.import_xml` / `widget.export_xml` round trip over `widget.add` chains;
`widget.describe` is the richer read for bindings or runtime state.

## Shared asset path aliases

Widget Blueprint asset-path fields use canonical `widgetPath`; dispatcher aliases are `assetPath`,
`path`, `blueprintPath`, `blueprint_path`, and `requestedPath`. `widget.describe` and
`widget.export_xml` accept legacy snake-case aliases in asset mode; live mode rejects all asset-path
fields.

## See also

- [`widget.iteration-loop`](widget.iteration-loop.md) — the inspect → plan → edit → save → verify discipline and the tools-at-a-glance table.
- [`widget.xml-markup`](widget.xml-markup.md) — XML format spec, `IsVariable`, choosing a root panel, widget property introspection.
- [`widget.slot-layout`](widget.slot-layout.md) — CanvasPanelSlot `LayoutData` math, Box slot rules, button / border / partial-struct gotchas.
- [`widget.failure-modes`](widget.failure-modes.md) — orphan-widget trap, save hygiene, editor-restart-after-C++ rule, Python constraints, Designer preview size, symptom→fix table.
- `call("ui")` — runtime instance ops on widgets that have already been added to the viewport.
- `call("blueprint.graph")` — edit the widget blueprint's event graph nodes (tick, construct, custom events).
- `call("blueprint.compile_bpir", …)` — author event handler graphs in BPIR markup, including the `widget_event` entry form needed for `widget.bind_event`.
- [`asset`](asset.md) — cached `tree.xml`, `widget_animations.json`, and `bpir.txt` mirrors for repeatable Widget Blueprint audits.
- [`visual-review`](visual-review.md) — choose between `widget.screenshot_designer`, live UI structure capture, and dump `preview.png`.
- `call("bpir")` — BPIR topic index; see `call("bpir.instructions")` for syntax of `widget_event` handlers and widget-variable references, plus `call("bpir.examples.widget-event")` for a worked example.
- [`asset-audit`](asset-audit.md) — repeatable Widget Blueprint evidence caches for audit sweeps.

## Text and localization

Persisted UE text needs a localization identity. Pass `NSLOCTEXT("namespace","key","source")`
with non-empty namespace/key. Plain strings are rejected for new persisted `FText`; on existing
localized text they preserve the identity and change only the source string.

`widget.set` and `property.set` parse string values with
`FTextStringHelper::CreateFromBuffer` (declared in
`Runtime/Core/Public/Internationalization/Text.h`), recognizing:

- `NSLOCTEXT("namespace","key","source")` — localizable text with namespace/key.
- `LOCTEXT("key","source")` — localizable text in the current namespace.
- `LOCTABLE("TableId","Key")` — `UStringTable`-backed FText, resolved at runtime by
  `FText::FromStringTable(TableId, Key)`; round-trips through `widget.export_xml` and BPIR decompile
  when `IsFromStringTable()` fires (see `call("bpir.instructions")`).
- `INVTEXT("text")` — parsed by Unreal, but rejected for persisted authored text because it has no namespace/key.
- Anything else — treated as a source-string replacement only when the existing text already has namespace/key.

Raw `NSLOCTEXT(...)` returns FText with `Namespace`, `Key`, and `SourceString`; `widget.export_xml`
round-trips the macro.

**`namespace` and `key` must be readable, stable text** (for example, `WBP_GameMenu` +
`Menu.Camera.WASD`). Do not emit hex GUIDs or regenerate identities; preserve an existing readable
identity or mint one.

**Lesson:** marshal JSON strings into FText through `FTextStringHelper::CreateFromBuffer`, never a
custom quote/paren parser.

## Reparenting vs. remove + add

`widget.reparent_widget` preserves the instance, name, properties, subtree, and BP event-graph
references. Use it when only the parent changes.

`widget.remove_widget` + `widget.add` destroys and recreates. BP nodes referring to the old name then
hold stale references (`"Event Dispatcher pin is not connected"` at compile time). Use this only for
a genuinely different class; rewire the BP graph through BPIR (`call("bpir.instructions")`).

## Panel-editing primitives

Three UE/UMG primitives underpin tree-mutation handlers; use them instead of rebuilding slot state.

**`UPanelWidget::ReplaceChild(Current, New)`**

Rehomes `Current`'s `UPanelSlot` onto `New`, preserving parent-slot properties (anchors, offsets,
alignment, z-order, CanvasPanelSlot position/size, Overlay/Box alignment). `Current` must be a
panel child and `New` non-null or the call returns `false`; with `OldParent = OldTarget->GetParent()`,
the bidirectional invariant guarantees success.

`widget.wrap` and `widget.replace_class` use `ReplaceChild`, not `RemoveChild` + `AddChild`; the
latter creates a default slot (100×30 top-left, `Auto` sizing), causing the classic snap-to-corner
bug. Never use the latter for a child swap.

See `UMG/Private/Components/PanelWidget.cpp:~230` in the engine for the implementation.

**`UWidgetTree::RootWidget` — native writable, python blocked**

`RootWidget` is `UPROPERTY(Instanced)` without a C++ `BlueprintProtected` tag, so native code can
assign it. Python and Blueprint bindings block it and fail with
`"Property 'RootWidget' is protected and cannot be read/set"`.

`widget.replace_class`'s root-swap branch, `WidgetAuthoringUtils::ReplaceWidgetClass`,
`AddWidgetToParentOrRoot`, and `AttachToParentOrRoot` assign `WidgetTree->RootWidget` natively;
Python callers must use the typed handler.

Engine ref: `UMG/Public/Blueprint/WidgetTree.h:~129`.

**`UPanelWidget::AddChild(Widget, SlotTemplate)`**

The two-arg `AddChild` reuses template FProperty values when panel and template slot classes match;
otherwise it uses a default slot. `widget.replace_class` snapshots each child's old `UPanelSlot*`,
detaches it, then calls `NewPanel->AddChild(Child, OldSlotTemplate)`. Matching classes (for example,
both `UCanvasPanelSlot`) preserve layout; differing classes get a default slot.

Engine ref: `UMG/Private/Components/PanelWidget.cpp:~132`.

## Tree restructure handlers

**Delete a widget and promote its children to the grandparent (un-wrap)**

Goal: remove an intermediate wrapper while attaching its children directly to its parent.

Workaround:

1. For each child of `X`, call `widget.reparent_widget newParent=<X's parent>`.
2. Call `widget.remove_widget X` once empty.

Composes cleanly from existing primitives.

## UWidgetTree iteration — ForEachWidget vs ForEachWidgetAndDescendants

`UWidgetTree::ForEachWidget` walks **only widgets owned by that tree**; it treats nested
`UUserWidget`s as leaves because each has its own `WidgetTree`.

`UWidgetTree::ForEachWidgetAndDescendants` crosses those boundaries by following nested
`UUserWidget->WidgetTree` recursively, matching `UUserWidget::TakeWidget()`'s Slate build cascade.

For guards tied to `Root->TakeWidget()` (extension-point realization, `RebuildWidget` side effects,
or runtime-dependency walking), MUST use `ForEachWidgetAndDescendants`; `ForEachWidget` silently
misses nested subtrees that `TakeWidget` constructs.

The `UI_EXTENSION_POINT_REQUIRES_RUNTIME` guard in
`FWidgetGeometryResolver::ContainsExtensionPointWidget` had this bug: `ForEachWidget` missed a
nested `UUIExtensionPointWidget`, and the offscreen prepass crashed in
`UCommonLocalPlayer::CallAndRegister_OnPlayerStateSet`.

See the `UMG/Public/Blueprint/WidgetTree.h` engine comment for the explicit warning that `ForEachWidget` is non-recursive across user-widget boundaries.

## Inherited widget trees and animation bindings

Animation bindings to the root widget or to slot widgets are no longer skipped. `WidgetAnimationJsonSerializer.cpp` (at `Source/.../Private/Handlers/UI/`) emits binding entries with `isRootWidget: true` or `slotWidgetName: "<SlotName>"` instead of dropping the v1 binding silently. The schema is `pinwright.widget-animations.v1` (see `widget.export_animations_json` with `includeEventMetadata=true`).

For child WBPs inheriting a parent root, the exporter uses `FindWidgetTreeOwningClass()` in
`WidgetXmlExporter.cpp` and writes `<!-- inherited from <ParentPath> -->` plus
`inherited_from="<ParentPath>"` on the root. Cache the archetype: `GetWidgetTreeArchetype()` is
called repeatedly on this path.

See also [`asset.dump`](asset.dump.md) for the `tree.xml` `inherited_from` marker that mirrors this behaviour in dumps.

## Related shipped behavior

- `widget.set` / `property.set` parse NSLOCTEXT / LOCTEXT / INVTEXT via `FTextStringHelper::CreateFromBuffer`. See "Text and localization" above.
- `widget.wrap` supports tree restructure workflows. See "Tree restructure handlers" above.
- `widget.replace_class` covers both non-root and root class swaps in one call.

### widget.create_widget_blueprint

**`name` is a BARE asset name, never a path.** It is validated against the engine's own object-naming rules (`FName::IsValidXName` / `INVALID_OBJECTNAME_CHARACTERS`) before `folder` is even resolved, and the composed package path against `FPackageName::IsValidLongPackageName`, so a `/`, `\`, `.`, `..`, a leading or trailing slash, a space or an unmounted root is rejected `INVALID_ARGUMENT` with the engine's own reason text quoted. This is not pedantry about naming: `name` is concatenated onto `folder` and handed to `CreatePackage`, which logs a package name containing `//` at **Fatal** — a verbosity that is not compiled out in any configuration — so a name like `a//b` did not fail the call, it ended the editor **process** and every unsaved package in it. A name of `..` reaches a second Fatal on the same function by resolving to an empty package name. The same defect was measured end-to-end on `foliage.add_type`; see that verb on the [`foliage`](foliage.md) page.

**`folder` (optional, default `/Game/UI`) is how you choose the destination.** It is checked separately for traversal and unmounted roots and refused `SECURITY_VIOLATION`. Because the name check runs first, a call that gets both wrong is answered about the name.

**`parentClass` (optional) is resolved before any package is created.** An unresolved or empty
explicit value returns `CLASS_NOT_FOUND`; a resolved class that is not a `UUserWidget` subclass, or
is an abstract/deprecated/superseded subclass, returns `CLASS_NOT_INSTANTIABLE`. The canonical
`UUserWidget` root is allowed even though the engine marks it abstract. The handler never falls
back to `UUserWidget` for an invalid explicit parent, and these refusals create no package or asset.
The success response's `parentClass` is the actual resolved class path.

**`save` (optional, default `true`) controls persistence.** The default writes the `.uasset` to
disk. `save:false` leaves the newly created package dirty in memory. The response reports
`saveRequested` and `saved`; `pendingFlush` is true when a requested save did not become durable,
and `saveState` / `saveDetail` explain the measured `AssetSaveState` when available.

`editor.create_utility_widget` on the [`editor`](editor.md) page takes the same bare-`name` + `folder` contract, with the same refusals.

### widget.rename_widget

`newName` must pass the engine's `FName::IsValidXName` rules and must not already identify a
different widget in the blueprint tree. Invalid names return `INVALID_ARGUMENT`; occupied
destinations return `DESTINATION_EXISTS`; both checks happen before the rename transaction opens.

After `UObject::Rename`, the handler reads the tree back and requires the new identity to resolve
to the target while the old identity is absent. A failed rename or readback returns
`RENAME_FAILED` and restores the original widget name and package dirty state. GUID bookkeeping
and structural-modified state are updated only after that proof. Successful responses keep the
requested `oldName`, but `newName` is the actual `TargetWidget->GetName()` read from the object.
An FName-equivalent request is a transaction-free successful no-op.

### widget.wrap

**Wrap a widget with a new parent**

Put widget X inside a new Overlay / Border / SizeBox Y while keeping its subtree and original parent
slot.

One call replaces `widget.add` + `widget.reparent_widget` + `widget.set`: it creates the wrapper,
uses `ReplaceChild(X, Wrapper)` to rehome the old slot, then `Wrapper->AddChild(X)`. X's subtree and
original parent slot stay intact.

`wrapperProperties` and `wrapperSlot` are validated against scratch instances before the authored
tree is changed. If validation or a later attachment invariant fails, the handler restores the
original parent, child index, slot object, and subtree, and removes the newly constructed wrapper;
failed calls therefore leave no partial wrap to clean up before retrying.

When `wrapperProperties.Slots` is supplied, null entries and slots whose content is the target or
one of its descendants are accepted. A slot or widget reference whose content lies outside the
wrapped target subtree is refused with `INVALID_PROPERTY` before the wrapper is constructed.

Handler: `Handlers/UI/WidgetWrapHandler.cpp`.

### widget.replace_class

**Change a widget's class preserving children**

Change a `CanvasPanel` to an `Overlay` (or another class) while keeping immediate children attached;
works for root and non-root widgets.

- Non-root: `ReplaceChild(OldTarget, NewTarget)` preserves the parent slot; panel swaps migrate
  children with `NewTarget->AddChild(Child, OldSlotTemplate)`.
- Root swap (for example `CanvasPanel` → `Overlay`) assigns `WidgetTree->RootWidget = NewTarget`
  natively; Python cannot do this (see the `UWidgetTree::RootWidget` note).

Matching overridden property names are carried over. Recompile afterwards; `requiresCompile: true`
means BP-level re-validation is needed.

Handler: `Handlers/UI/WidgetReplaceClassHandler.cpp` (author utils in `WidgetAuthoringUtils::ReplaceWidgetClass:~513`).

### widget.import_xml

Single-call tree builder: pass markup and build or replace the tree atomically. It is faster than
chained `widget.add` calls and easier to review as a diff. See [`widget.xml-markup`](widget.xml-markup.md).

**Params:**

- `widgetPath` (required) — widget blueprint asset path.
- `xml` (required) — XML markup string.
- `mode` (optional) — `"replace"` (default) or `"add"`.
- `targetName` (optional) — widget to replace (subtree) or parent to add under.

**Modes:**

- `replace` (no `targetName`): clears the tree and builds XML, including the root class.
- `replace` + `targetName`: replaces that subtree and preserves its parent slot.
- `add` + `targetName`: appends XML under the named parent panel.

**Caveats:**

1. **Imports create new instances.** GUIDs are fresh. `variablesCreated: N` lists new BP variables;
   FProperty-resolved event nodes usually rebuild after recompile, but GUID-bound references stale.
   Recompile (`requiresCompile: true` signals this).
2. **Only XML properties are written.** Unspecified values reset to defaults, including designer
   fonts/styles. For subtree rewrites, export with `include_defaults=true`, edit, and reimport.
3. **The new subtree owns internal slot data.** `targetName` preserves the replaced widget's parent
   slot, but internal slots must be fully specified.
4. **New XML names replace old names.** Keep names stable when BP graphs reference them.
5. **Unknown types can become empty Overlays.** Verify with `widget.export_xml`.

Prefer `widget.add` + `widget.reparent_widget` + `widget.set` for small changes. Use `widget.import_xml`
for known full-state rewrites; it is faster but destroys widget identity.

### widget.export_xml

Dump a Widget Blueprint tree as XML. It is diffable and round-trips through `widget.import_xml`; see
[`widget.xml-markup`](widget.xml-markup.md).

Full-tree export resolves inherited roots. If a child WBP has no local `WidgetTree->RootWidget`, it
starts at the parent root instead of returning `TREE_EMPTY`; the root gets `inherited_from="..."`
and the XML starts `<!-- inherited from ... -->`, matching `asset.dump` `tree.xml`. Live/offscreen
geometry and `widget_count` remain available. `widget_name` subtree export searches only the child's
local tree, so inherited widgets require full-tree export.

With `omit_slot_chain`, compact XML must omit raw `UWidget::Slot` during the normal property pass,
not only skip `Parent` / `Content` in the dedicated slot block. `ExportPropertyToJsonValue` expands
the instanced `UPanelSlot`; leaving `Slot` leaks recursive `Parent={Slots=...}` and `Content={...}`.

Compact XML emits reflected `bOverride_*` booleans, including `false`. `FSlateBrush` embedded
`ResourceObject` references use full UE export-text syntax, such as
`/Script/Engine.Texture2D'/Game/UI/T_Icon.T_Icon'`.

**Params:**

- `widgetPath` (required) — Widget Blueprint asset path.
- `widget_name` (optional) — subtree root to export.
- `include_defaults` (optional, default `false`) — include all properties, not only overrides.
- `capture_source` (optional, default `"asset"`) — `"asset"` or `"live"`.
- `verbose` / `include_geometry` (optional, default `false`) — live mode only.
- `resolve_geometry` (optional bool/object) — emit resolved Slate geometry.

#### Live runtime UI snapshot

`widget.export_xml` and `widget.describe` accept `capture_source="live"` to snapshot the active PIE
UMG subtree shown by Widget Reflector with **UMG as root**. Live mode accepts no asset path; the
default `capture_source="asset"` exports a stored WBP tree.

| Parameter | Method(s) | Description |
|---|---|---|
| `capture_source` | `widget.describe`, `widget.export_xml` | `"asset"` (default) uses the WBP tree; `"live"` captures the active PIE UMG-as-root subtree. |
| `verbose` | both | Live only; `false` omits default-valued noise, `true` includes readable defaults. |
| `include_geometry` | both | Live only; `false` omits geometry, `true` includes cached runtime Slate geometry. |

Compact live output keeps class/name/visibility/text/value-style fields that differ from defaults.
`verbose=true` adds readable default-valued runtime state; `include_geometry=true` adds geometry and
is independent of `verbose`.

`widget.describe` returns structured JSON with top-level `capture_source: "live"`; `widget.export_xml`
serializes the same snapshot as XML with captured Slate/UMG class names as tags.

##### Dual JSON shape — `widget.describe` returns two different payloads

`widget.describe` has two JSON shapes selected by `capture_source`. Consumers, including the C++
formatter at `Source/PinWright/Private/Handlers/Widget/WidgetDescribeFormatter.cpp`, MUST branch;
`capture_source` and `root` are the discriminators.

**Asset shape** (default, `capture_source="asset"`):

```
{
  "asset_path": "/Game/UI/W_Foo.W_Foo",
  "root_class": "CanvasPanel",
  "widget_count": 23,
  "tree": { "type": "CanvasPanel", "name": "Root",
            "props": {...}, "slot": {...}, "children": [...] }
}
```

Asset per-node fields: `type`, `name`, `props`, `slot`, `children`.

**Live shape** (`capture_source="live"`):

```
{
  "capture_source": "live",
  "verbose": false,
  "geometry_included": true,
  "viewport_size": { "w": 1920, "h": 1080 },
  "snapshot_summary": { ... },
  "root": { "slate_type": "SObjectWidget", "debug_name": "W_GameLayout_C_0",
            "runtime_state": {...}, "source": {...},
            "slot": { "properties": {...} },
            "bindings": [...], "delegates": [...],
            "geometry": {...}, "children": [...] }
}
```

Live per-node fields: `slate_type`, `debug_name`, `runtime_state`, `source`, `slot` (with
**`properties`**, not `props`), `bindings`, `delegates`, `geometry`, `children`.

Shape-mismatch symptom: a valid live capture rendered as "Widget: Unknown" because the formatter
branched only on asset fields. The capture was fine; the formatter was wrong.

##### Live-root locator — `AddToScreen` wrapper

The live tier finds its UMG-as-root candidate through `IsUmgRootCandidate` in
`Source/PinWright/Private/Handlers/UI/LiveUiSnapshot.cpp`. It matches **`SConstraintCanvas` whose
first child is `SObjectWidget`**, the wrapper produced by `UMG::FGameViewportSubsystem::AddWidget`
(the C++ behind `AddToScreen`).

`LiveUiSnapshotJsonWriter.cpp` and `LiveUiSnapshotXmlWriter.cpp` walk from this root. If
`IsUmgRootCandidate` returns `LIVE_UI_NOT_FOUND` while UMG is active, check whether the application
bypasses `AddToScreen` and uses a non-`SConstraintCanvas` wrapper; that is the legitimate miss.

```json
{
  "method": "widget.export_xml",
  "params": {
    "capture_source": "live",
    "verbose": false,
    "include_geometry": true
  }
}
```

Live mode errors are explicit; the handlers do not silently fall back to asset mode.

| Error code | Meaning |
|---|---|
| `SLATE_NOT_INITIALIZED` | Slate is unavailable, so no live UI tree can be walked. |
| `PIE_NOT_RUNNING` | No Play-in-Editor session is active. |
| `GAME_VIEWPORT_NOT_FOUND` | PIE is running, but the game viewport could not be found. |
| `LIVE_UI_NOT_FOUND` | No live UMG-as-root subtree was found in the active PIE viewport. |
| `AMBIGUOUS_LIVE_ROOT` | More than one possible live root was found and the handler could not choose one unambiguously. |

#### Widget geometry resolution

`widget.describe` and `widget.export_xml` can emit **fully-resolved** window-relative geometry from a
real Slate layout pass, not slot-anchor arithmetic. It is **off by default**; pass
`resolve_geometry: true` or an object to enable it.

**Param shape** (optional bool or object on both RPCs):

- `resolve_geometry: true` — enable with defaults.
- `resolve_geometry: false` or omitted — disable (default).
- `resolve_geometry: { ... }` — enable with overrides:

| Field | Type | Default | Description |
|---|---|---|---|
| `viewport_size` | `{w, h}` | `{1920, 1080}` | Window size for the `offscreen` tier. |
| `force_visible_for_measure` | bool | false | Temporarily flip Collapsed→SelfHitTestInvisible during offscreen measurement so hidden widgets get a rect. |
| `instance_name` | string | — | Disambiguator for `live` tier when multiple instances of the class exist. |

**Tiered resolver:** it runs the waterfall and picks the first successful tier:

1. **Designer** — an open target `UWidgetBlueprint`; reads cached preview geometry.
2. **Live** — a generated-class `UUserWidget` on the player viewport; requires
   `IsInViewport() && GetCachedWidget().IsValid() && LocalSize > 0`. Multiple matches without
   `instance_name` return `AMBIGUOUS_INSTANCE` and candidates.
3. **Offscreen** — always available; instantiates a transient `UUserWidget` in an `SVirtualWindow`
   of `viewport_size`, runs `SlatePrepass` and recursive `ArrangeChildren` (as in
   `SDesignerView::PopulateWidgetGeometryCache_Loop`), reads `FGeometry`, then tears down.
   The instance is **design-time**, like the Designer preview: a C++ parent's
   `NativeOnInitialized` / `NativePreConstruct` / `NativeConstruct` never run, so a widget whose
   runtime construct needs a game (a subsystem, a local player) is measured safely. The layout
   is therefore the Designer's, not whatever runtime code would have set in `NativeConstruct`.

**Coordinates** are **post-DPI Slate absolute units**, matching Widget Reflector and UMG Designer
rulers. Root `dpi_scale` lets callers recover logical pixels.

**JSON output** (`widget.describe`): per-node `geometry` object:

```
geometry: {
  status: "ok" | "skipped" | "error",
  reason: "NOT_REACHED" | "NOT_PAINTED" | "AMBIGUOUS_INSTANCE" | ... ,  // only when status ≠ ok
  absolute: { x, y, w, h },     // window-space
  local:    { w, h },
  desired_size: { w, h },
  visibility: "Visible" | "Collapsed" | "Hidden" | "HitTestInvisible" | "SelfHitTestInvisible",
  render_transform: { translation: {x,y}, scale: {x,y}, shear: {x,y}, angle_deg }  // only when non-identity
}
```

Top-level additions: `geometry_source`, `viewport_size`, `dpi_scale`,
`geometry_summary: {ok, skipped, error, total}`, and applicable `top_level_error` /
`ambiguous: true` + `candidates`.

**XML output** (`widget.export_xml`): per-node `Geom.*` attributes on each widget element:

```
Geom.status, Geom.reason, Geom.abs_x, Geom.abs_y, Geom.abs_w, Geom.abs_h,
Geom.local_w, Geom.local_h, Geom.desired_w, Geom.desired_h, Geom.visibility
```

For a non-identity transform, a `<RenderTransform translation_x=".." translation_y=".." scale_x=".." scale_y=".." shear_x=".." shear_y=".." angle_deg=".." />` child is inserted before child widgets.

The root receives `Geom.source`, `Geom.viewport_w`, `Geom.viewport_h`, `Geom.dpi_scale`,
`Geom.summary_ok`, `Geom.summary_skipped`, `Geom.summary_error`, `Geom.summary_total`, plus
relevant `Geom.top_level_error` / `Geom.ambiguous` and a `<Candidates>` sibling for live candidates.

**Reason codes:** `NOT_REACHED` (asset widget absent from resolved live tree), `NOT_PAINTED` (no
cached live Slate widget), `AMBIGUOUS_INSTANCE` (multiple live matches),
`UI_EXTENSION_POINT_REQUIRES_RUNTIME` (a `UUIExtensionPointWidget` needs live
`UCommonLocalPlayer` during `RebuildWidget`), `NULL_BLUEPRINT`, `BLUEPRINT_NOT_COMPILED`, `NO_WORLD`.

**Nested-extension-point detection.** `FWidgetGeometryResolver::ContainsExtensionPointWidget` uses
`UWidgetTree::ForEachWidgetAndDescendants`, not `ForEachWidget`, so nested
`UUIExtensionPointWidget`s are found. A non-recursive walk lets the offscreen prepass crash in
`UCommonLocalPlayer::CallAndRegister_OnPlayerStateSet` when `RebuildWidget` lacks a live local
player. Use the same recursive guard for other runtime-only widget classes; see the
"UWidgetTree iteration" section.

**Collapsed widgets** report `status: ok`, `visibility: Collapsed`, and a zero-sized `absolute` rect.
`force_visible_for_measure: true` flips them during offscreen measurement and labels them
`visibility: CollapsedForced`.

**Stretched slots:** stored `Slot.LayoutData` is correct; static resolver geometry can disagree.
Trust stored data and verify at runtime when unsure.

**Breaking change:** `resolve_geometry.source` (`auto|designer|live|offscreen|off`) was removed.
Pass a bool/object; default is **off**, so callers must pass `resolve_geometry: true` to opt in.

### widget.describe

Recommended first read before mutating an unfamiliar WBP. Returns the full tree, overridden
properties, slot info, and bindings in one call; this is cheaper than per-child
`widget.get_class_properties`. Text uses the C++ formatter registry; JSON is available without
`_format: text`.

Supports `capture_source="live"` and `resolve_geometry`; see `widget.export_xml` for shared
semantics, parameters, and errors.

### widget.set

Sets `properties` and/or `slotProperties` on a named widget. Struct values accept three JSON shapes:

1. **Nested object** (preferred for readability; produces per-field error aggregation):
   ```json
   { "properties": { "LayoutData": { "Offsets": { "Left": 10, "Top": 20, "Right": 10, "Bottom": 20 } } } }
   ```
2. **ExportText string literal** (single UE-format string, falls through to `ImportTextToProperty`):
   ```json
   { "properties": { "LayoutData": "(Offsets=(Left=10,Top=20,Right=10,Bottom=20))" } }
   ```
3. **JSON-string-of-JSON** (legacy form, still supported):
   ```json
   { "properties": { "LayoutData": "{\"Offsets\":{\"Left\":10}}" } }
   ```

Prefer nested objects: errors identify exact paths such as `LayoutData.Offsets.Left`; string forms
report one parse error for the blob. This applies to both fields.

FText values route through `FTextStringHelper::CreateFromBuffer`; see "Text and localization".

> Do not use `Visibility` here for screenshot prep — that's the runtime UPROPERTY and persists on save. Use `widget.set_designer_visibility` for editor-only eye control. See `### widget.screenshot_designer` above for the full capture workflow.

### widget.bind

Writes one entry into the widget blueprint's `Bindings` array and creates the handler function graph
for it. This is the UMG *property/event binding* mechanism (the details-panel link icon), not the
event-graph `BndEvt__` mechanism — for multicast events such as `OnClicked` use
`blueprint.compile_bpir` with `entry widget_event ...` and verify with `widget.bind_event`.

**`propertyName` must be a name UMG itself resolves.** `UWidgetBlueprintGeneratedClass::InitializeBindingsStatic`
and `FDelegateEditorBinding::IsBindingValid` both perform exactly two lookups, in this order:

| what you pass | what UMG looks up | `bindingType` | handler requirement |
|---|---|---|---|
| a property stem — `Text`, `Visibility`, `ToolTipText` | `<stem>Delegate` (`TextDelegate`) | `property` | pure, returns the property's type |
| a bindable event delegate verbatim — `OnMouseButtonDownEvent` | that name | `event` | matches the delegate's signature |

Neither lookup appends `Event`, so the friendlier-looking `OnMouseButtonDown` resolves to nothing at
all. Such a name is refused with `WIDGET_BINDING_NAME_UNRESOLVED`: the payload carries
`bindableProperties` and `bindableEvents` for the widget's class, and the message names the near-miss
spelling. `OnClicked` and other multicast delegates are refused with
`WIDGET_BINDING_IS_MULTICAST_EVENT`. Read those two arrays rather than guessing — nothing else
publishes the bindable set, and `widget.get_class_properties` cannot show it (the `<stem>Delegate`
properties are not `EditAnywhere`).

The handler function graph is generated from the delegate's own signature — its parameters and return
value — and flagged pure for a property binding. A pre-existing graph of that name is used as-is.

Before writing, the verb runs `FDelegateEditorBinding::IsBindingValid` on the candidate record. A
handler whose signature or purity does not satisfy the delegate is reported as `BINDING_FAILED` with
the compiler's own messages in `compilerMessages`, and **no binding is written**. The success payload
reports `resolves` from that check plus `bindingStored`, read back off the array.

> A dead binding is invisible downstream: the widget compiler's `SanitizeBindings` prunes only on a
> missing *widget*, never a missing property, and `widget.export_xml` renders a record that resolves
> to nothing identically to one that works. The rejection above is the only place the mistake is
> still visible.

### widget.bind_event

`widget.bind_event` is **verify-only**; it does not create bindings. It inspects an existing
`BndEvt__*` K2Node and reports:

- `isDelegateValid` — whether the delegate property still exists on the target widget class.
- `customFunctionName` — the generated handler function name.
- other diagnostic fields for the existing binding.

To **create** a binding, emit a `widget_event` entry via `blueprint.compile_bpir`:

```
entry widget_event StartButton.OnClicked() {
    call PrintString(InString: "clicked")
}
```

Optionally call `widget.bind_event` to verify it. Its Summary and `BPIR_REQUIRED` error say this;
the historical name is misleading, so treat it as `widget.verify_event_binding`.

### widget.remove_widget

Cascade-deletes `UK2Node_ComponentBoundEvent` entry nodes and exec-reachable subgraphs referencing
the removed widget **or any descendant**. The scan follows `RemoveWidget`'s panel-child cascade and
covers both `UbergraphPages` and `FunctionGraphs`.

Two fields are added to the response payload:

| Field | Type | Meaning |
|---|---|---|
| `cascadedBoundEventsRemoved` | int | Deleted `UK2Node_ComponentBoundEvent` entries plus exec subgraphs |
| `removedWidgets` | array<string> | Variable names of the target widget + all descendants that were removed |

No `widget.remove_widget` → `blueprint.graph.find_nodes` → `blueprint.graph.delete_node` cleanup is
needed; one call leaves no orphan `BndEvt__*` nodes and a compile-clean blueprint.

### widget.screenshot_designer

Captures the UMG Designer to PNG. `target="preview"` captures only the rendered preview canvas;
`target="window"` captures the whole Widget Blueprint editor window. Output defaults to
`Saved/Screenshots/WidgetDesigner/`; `outputPath` overrides it.

For `target="preview"`, the handler renders the preview `UUserWidget` through
`FWidgetRenderer::DrawWidget` into `UTextureRenderTarget2D`, then reads pixels. `max_size` controls
resolution instead of the Slate back buffer, making output stable across DPI/layout. Preview bounds
still determine natural aspect ratio; failure returns `PREVIEW_BOUNDS_NOT_FOUND`. `target="window"`
uses `FSlateApplication::TakeScreenshot` and needs no crop bounds.

**The `target="preview"` PNG is stamped opaque as of 2026-08-21.** `FWidgetRenderer` clears its
render target to transparent, so unfilled preview regions are genuinely alpha 0; raw encoding looked
blank in alpha-compositing viewers while RGB remained intact. The window branch already stamped.
Compositing these PNGs therefore changes after that date. Measure the pre-stamp alpha-zero fraction:
the stamp removes real information, not a back-buffer artefact. Do not stamp the two non-picture
paths: `render.detect_z_fighting` returns depth/normal data, and its z-fighting mask is built opaque.

**Colour space: the PNG is sRGB-encoded exactly once.** A pixel sampled from either target is
`sRGB_encode(linear)` — the same byte `FLinearColor::ToFColor(bSRGB=true)` produces for the
authored value, so a sampled pixel compares directly against a design colour with no conversion.
Until 2026-08-27 the `target="preview"` path encoded **twice** and every colour read wrong by one
transfer function: a tint whose correct value is `(37,77,103)` came back `(106,149,170)`. The image
decoded, kept its shapes and merely looked washed out, so nothing about the file announced the
error, and every widget `preview.png` written into an asset-dump mirror before that date carries it.
Re-dump rather than trusting an old preview. Preview captures put the encode in the render target's
hardware ROP and keep the Slate shader in linear space; `target="window"` inherits Slate's own
single encode from the back buffer.

#### max_size — output resolution control (preview only)

`max_size` (integer, optional, default `1024`, hard cap `16384`) caps the **longer** output axis;
the shorter follows the preview canvas aspect ratio. It applies only to `target="preview"` and is
ignored for `target="window"`.

- 800×400 + `max_size=1024` → `1024×512`; 400×800 → `512×1024`; 400×400 → `1024×1024`.

The response echoes applied `max_size` and `renderer` (`"widgetRenderer"` for preview,
`"slateScreenshot"` for window); `width` / `height` are actual output dimensions.

Preview captures use `FWidgetRenderer::DrawWidget`, not the live back buffer, so they omit Designer
chrome (rulers, anchor handles, selection outlines) and resemble runtime widget content. This is
intentional for visual diffs, design review, and documentation.

Cold Designer state opens the asset, switches to Designer, invokes `SlatePreview`, refreshes only if
the preview Slate widget is missing, and pumps Slate with bounded retries before resolving bounds.
If preview Slate or bounds remain unavailable, it returns `PREVIEW_SLATE_NOT_FOUND` or
`PREVIEW_BOUNDS_NOT_FOUND`; it never falls back to the window or whole `SDesignerView`.

#### closeAfterCapture — the Designer is put back the way it was found

**The verb closes the Widget Blueprint editor it opened, on every exit path including its error
returns.** Until 2026-09-03 it left one open per call, and a Designer left open across a later
`blueprint.compile` / `blueprint.compile_bpir` of that same widget killed the whole editor process
(`UUserWidget::RebuildWidget` dereferencing a `WidgetTree` the compile had nulled, reached from
`SDesignerView::Tick` on an ordinary paint). The compile side is guarded independently now — every
PinWright compile tears the Designer preview down first, the way the engine's own Compile button
does — so this is no longer a crash precondition; it is simply state a read-only capture verb owes
back.

`closeAfterCapture` has the same three states as `render.capture_asset_preview`: absent closes only
a Designer **this call** opened, an explicit `true` closes one that was already open, `false` leaves
it (use it for an iteration loop). The close is **queued onto the next editor tick**, never run
inside the call — destroying a toolkit on the capture stack is its own crash — so the response
reports:

- `assetEditorWasAlreadyOpen` — the caller had this widget's Designer open before the capture.
- `assetEditorClosed` — measured, not intended: `true` only when the window is already gone.
- `assetEditorCloseDeferred` — `true` when the close is queued and the window goes a tick later.

`asset.dump`'s widget preview aspect uses the same bracket, so a folder dump no longer leaves one
Designer open per widget it dumped.

#### Hiding siblings for focused captures

To capture one subtree (a mode overlay, `WidgetSwitcher` page, or list row), hide other siblings with
`widget.set_designer_visibility`. It flips editor-only `bHiddenInDesigner`; snapshot each eye state
with `widget.get_designer_visibility` and restore the returned `visible` value after capture.

**Do not use `widget.set` with `Visibility=Collapsed` for this.** `Visibility` is a runtime UPROPERTY,
persists on save, and changes gameplay. Use the eye toggle.

```json
{ "method": "widget.get_designer_visibility",
  "params": { "widgetPath": "/Game/UI/WBP_HUD.WBP_HUD", "widgetName": "RaceOverlay" } }
{ "method": "widget.set_designer_visibility",
  "params": { "widgetPath": "/Game/UI/WBP_HUD.WBP_HUD", "widgetName": "RaceOverlay", "visible": false } }
{ "method": "widget.screenshot_designer",
  "params": { "widgetPath": "/Game/UI/WBP_HUD.WBP_HUD", "target": "preview" } }
{ "method": "widget.set_designer_visibility",
  "params": { "widgetPath": "/Game/UI/WBP_HUD.WBP_HUD", "widgetName": "RaceOverlay", "visible": true } }
```

#### Runtime Visibility=Collapsed leaks into the preview render

The Designer honors runtime `Visibility=Collapsed`: a Collapsed parent contributes zero size and
children do not render even when their eye flag is shown. Hiding siblings alone cannot reveal it.

For an off-by-default subtree, temporarily set runtime `Visibility` to `Visible` or
`SelfHitTestInvisible` with `widget.set`, then revert before saving. Prefer screenshot
`showOnly` / `hide` / `visibilityOverrides`, which apply transient flips without dirtying the asset;
manual `widget.set` is the fallback.

#### Override modes

`widget.screenshot_designer` accepts three transient one-capture overrides. The asset is never dirty,
no `editor.save_all` cleanup is needed, and overrides revert even on failure.

- `showOnly` (string array) — show named widgets and ancestors; hide every other preview widget.
- `hide` (string array) — hide named widgets in addition to saved state.
- `visibilityOverrides` (object) — temporarily set runtime `Visibility` to
  `Visible | Collapsed | Hidden | HitTestInvisible | SelfHitTestInvisible`.

`ON_SCOPE_EXIT` restores original eye flags and runtime visibility on success or error. Mutations are
made on the **live preview UUserWidget instance**, not the asset template, so the package stays clean.

```json
{ "method": "widget.screenshot_designer",
  "params": {
    "widgetPath": "/Game/MyGame/UI/HUD/WBP_GameMenu",
    "showOnly": ["HorizontalBox-Help"],
    "visibilityOverrides": { "HorizontalBox-Help": "Visible" }
  } }
```

Prefer these overrides for one screenshot. Use the manual eye-state dance for multiple operations or
when troubleshooting the eye-flag flow.

### widget.set_designer_visibility

Toggles editor-only `bHiddenInDesigner` in the UMG Designer hierarchy. **Does not** touch runtime
`Visibility`: it has no effect in packaged builds, PIE, or runtime UI. Read state with
`widget.get_designer_visibility`.

#### Use during screenshot capture

Capture-and-revert: snapshot sibling eye flags, hide them, call `widget.screenshot_designer`, then
restore each saved `visible` value. See `widget.screenshot_designer` for the rationale and
`widget.set Visibility=...` warning.

Prefer `widget.screenshot_designer`'s `showOnly` / `hide` overrides for one capture; they do not
dirty the asset. Use the manual dance for persistent eye state across operations.

### widget.export_animations_json

Use `widget.export_animations_json` / `widget.import_animations_json` for persisted `UWidgetAnimation`
data. This is separate from XML: XML owns the tree; animation JSON owns attached MovieScene data.

v1 supports float tracks such as `RenderOpacity` and UMG families `widgetMaterial`, `2dTransform`,
and `text`. JSON binds by `widgetName`; binding GUIDs are diagnostic/update hints and may regenerate
on import so `UMovieScene` and `FWidgetAnimationBinding::AnimationGuid` stay synchronized. Import
requires `mode: replace` or `merge`.

Per-track-type schema under `bindings[].tracks[]`:

| `type` | identifier field(s) | key location |
| --- | --- | --- |
| `float` | `propertyName` + `propertyPath` | `sections[].keys[]` |
| `2dTransform` | `propertyName` + `propertyPath` | `sections[].channels.{translationX,translationY,rotation,scaleX,scaleY,shearX,shearY}` |
| `widgetMaterial` | `brushPropertyNamePath` (array of brush-property hops) | `sections[].scalarParameters[].keys[]` and `sections[].colorParameters[].channels.{red,green,blue,alpha}` |
| `text` | `propertyName` + `propertyPath` | `sections[].keys[]` |

`widgetMaterial` is the exception: it has no `propertyName`/`propertyPath`, uses
`brushPropertyNamePath`, and nests keys inside per-parameter entries. Asset-dump
`widget_animations.json` uses the same shape through `WidgetAnimationJsonSerializer`.

Asset dumps write `widget_animations.json` for every WBP from `UWidgetBlueprint::Animations`. No
authored animations intentionally emit `animations: []`; non-empty entries emit supported tracks and
optional event metadata through the same serializer.

For scrubbing analysis, `widget.get_animation_info` accepts `includeEvents:true` with
`animationName`, and `widget.export_animations_json` accepts `includeEventMetadata:true`. They add
animation-level `eventTracks`, `delegateBindings`, and deduped `triggeredFunctions`. Metadata is
inspection-only in v1 import; do not treat it as visual track data or put it under `bindings[].tracks`.

Loop count and playback speed are not persisted; set them at runtime through `PlayAnimation` in
Blueprint or C++.

### widget.create_widget_animation

A `UWidgetAnimation` plays only when its `UMovieScene` carries the animation's own `FName`. The UMG
compiler generates the animation's blueprint property from the **animation's** name
(`WidgetBlueprintCompiler.cpp`: `AnimVariableDesc.VarName = Animation->GetFName()`), while
`UWidgetBlueprintGeneratedClass::BindAnimationsStatic` assigns that property by looking it up under
the **MovieScene's** name (`InPropertyMap.Find(Animation->GetMovieScene()->GetFName())`). Under any
other MovieScene name the blueprint compiles clean, the generated property exists and is readable,
and it is permanently null — every `PlayAnimation` call is a silent no-op.

`widget.create_widget_animation` and `widget.import_animations_json` both name the MovieScene after
the animation. Successful `add_animation_track` and `add_animation_keyframe` calls rename a
mis-named MovieScene in place, so an asset authored before this repairs itself when the requested
edit commits. Rejected calls, including every `set_animation_speed` call, leave the MovieScene
pointer, name, playback range, bindings, tracks, and keys unchanged.

Response fields that expose the invariant: `create_widget_animation` returns `movieSceneName` and
`movieSceneCreated`, and `animationName` is read back off the object (object construction uniquifies
a taken name, so it may differ from the request). `widget.get_animation_info` returns
`movieSceneName` and `movieSceneNameMatchesAnimation` for each animation — the latter is the
diagnosis field for "the animation exists and nothing plays".

### widget.add_animation_track

`propertyName` defaults to `RenderOpacity` and must resolve on the selected widget to a reflected
`float` property. Dotted property paths are resolved before authoring. An unknown path returns
`[PROPERTY_NOT_FOUND]`; a real non-float property returns `[UNSUPPORTED_PROPERTY]`. Both errors
include `widgetName`, `widgetClass`, the requested `propertyName`, and sorted compatible float
`candidates`. Validation and off-asset track/section staging happen before the MovieScene is
created, renamed, bound, or saved, so an error does not leave partial animation state.

### widget.add_animation_keyframe

Property validation follows `add_animation_track`. Supplied `time` and `value` fields must also be
finite numbers, and `value` must fit the float channel; invalid input returns `[INVALID_PARAMETER]`
before mutation. When `widgetName` is omitted, the verb resolves the first animation binding back
to its widget, or uses the root widget when no binding exists. A binding whose widget no longer
exists, or a missing root, returns `[BINDING_NOT_FOUND]` without expanding the playback range or
creating a binding/track. A stale MovieScene GUID for an existing widget is replaced on commit.

### widget.set_animation_loop

Always returns `[NOT_SUPPORTED]`; no `loop`/`loopCount` succeeds. Loop count is a runtime-only
`UUserWidget::PlayAnimation()` argument, not serialized `UWidgetAnimation` data (which has only
`MovieScene`/`AnimationBindings`/`bLegacyFinishOnStop`/`DisplayLabel`); `widget.get_animation_info`
has no loop field. The params are accepted but ignored. For looping, wire
`PlayAnimation(Animation, 0.0, NumLoopsToPlay)` (`NumLoopsToPlay = 0` means infinite) in Blueprint
via BPIR/graph authoring or in C++. This deliberate guard ensures the impossible intent **fails loudly**
instead of silently no-op'ing.

### widget.set_animation_speed

Always returns `[NOT_SUPPORTED]`; no `speed` succeeds. Playback speed is a runtime-only
`UUserWidget::PlayAnimation()` argument, not serialized `UWidgetAnimation` data;
`widget.get_animation_info` has no speed field. The param is accepted but ignored. For a non-default
speed, wire `PlayAnimation(Animation, 0.0, 1, EUMGSequencePlayMode::Forward, PlaybackSpeed)` in
Blueprint via BPIR/graph authoring or in C++. This deliberate guard ensures the impossible intent
**fails loudly** instead of silently no-op'ing.
