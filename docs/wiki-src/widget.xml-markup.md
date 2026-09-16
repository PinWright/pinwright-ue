# Widget XML markup

Reference for `widget.export_xml`/`widget.import_xml` XML syntax, `IsVariable` handling, root choice, and property introspection; use it before non-trivial imports.

## Widget XML markup

`widget.export_xml` and `widget.import_xml` round-trip the entire widget tree as XML-with-attributes. Tag = widget class with the `U` prefix stripped; attributes carry properties, slot data, and bindings.

**Format example:**

```xml
<CanvasPanel name="Root">
  <VerticalBox name="Layout"
    Slot.Padding="(Left=10,Top=5,Right=10,Bottom=5)">
    <TextBlock name="Title"
      Text="INVTEXT(&quot;Hello&quot;)"
      Font.Size="24"
      Bind.Visibility="GetTitleVisibility" />
  </VerticalBox>
</CanvasPanel>
```

**Attribute rules:**

- Tag = widget class without `U` prefix: `TextBlock`, `Button`, `WBP_MyCustomWidget`.
- `name` = widget instance name.
- Regular attributes = widget properties (same value syntax as `widget.set`).
- `Slot.` prefix = slot properties (e.g. `Slot.Padding`, `Slot.LayoutData`).
- `Bind.` prefix = bindings to blueprint functions.
- `IsVariable="true|false"` = controls whether the widget is exposed as a Blueprint variable.

**IsVariable attribute**

Both `widget.export_xml` and `widget.import_xml` honour an `IsVariable` attribute per widget. This controls the "Is Variable" checkbox in UMG Designer (whether the widget is accessible from Blueprint code).

- **Export:** Each widget emits `IsVariable="true"` or `IsVariable="false"`.
- **Import:** `IsVariable="false"` prevents GUID registration; absent attribute defaults to `true` (backward compat).
- **`widget.set`:** Pass `{"properties": {"isVariable": false}}` to toggle on existing widgets.
- **`widget.describe`:** Returns `isVariable` boolean per widget node.

The underlying mechanism requires **both** `Widget->bIsVariable = true` on the `UWidget` instance **and** registration in `WidgetBlueprint->WidgetVariableNameToGuidMap`. The `widget.import_xml` handler sets both: `bIsVariable` on the widget and the GUID map entry via `OnVariableAdded`. Without `bIsVariable`, the Kismet compiler's `PopulateBlueprintGeneratedVariables` skips the widget even if it has a GUID entry.

**Always set `name="..."` on every widget element.** When `name` is omitted, the handler auto-generates a unique name from the class (e.g. `Button`, `CanvasPanel_1`) and returns a `warnings` array entry per affected widget. Auto-generated names are unreliable across re-imports: they collide on `add`-mode re-imports (causing `Duplicate widget name` errors), break BPIR `$WidgetName` references and `IsVariable` promotion, and invalidate event bindings (`BndEvt__*`) when the auto-name shifts.

## Choosing a root panel

The widget at the top of the tree carries a measurable runtime cost depending on its class:

- **`CanvasPanel`** — per-child absolute positioning, evaluated every frame. Cheap for a handful of children; non-trivial for deep or busy trees. Use when you need multi-child absolute positioning driven by anchors: HUDs with floating panels, screens that pin chrome to specific corners, layouts that overlap.
- **`Overlay`** — children stack without explicit positioning, each aligns independently via its slot. Lighter than CanvasPanel. Good default for single-child roots (a root that wraps one content subtree — a VerticalBox, a Border, a WidgetSwitcher) and for z-ordered content.
- **`HorizontalBox` / `VerticalBox`** — single-axis linear layout. Lightest when your root is genuinely a row or column. No need for a wrapper when the content *is* the box.
- **`Border`** — single-child with background brush and padding. Appropriate root when the widget is essentially "a styled container".
- **`SizeBox`** — single-child with size constraints. Appropriate root when the widget needs to enforce its own dimensions regardless of how it's placed.
- **`WidgetSwitcher`** — single-active-child tab strip. Appropriate root when the widget IS the switcher.

Project conventions in many UE codebases default to `Overlay` as root for single-child widgets because UMG authoring starts with CanvasPanel by default and that rarely matches what a finished widget actually needs. Sampling roots across a codebase is a quick way to learn the local convention.

**Root-swap via MCP:** use `widget.replace_class` — it handles both root and non-root in one call.

## Widget property introspection

Use `widget.get_class_properties` to enumerate editable properties for UMG widget classes with struct recursion in dot-notation (e.g. `Font.Size`). Pass `maxDepth=0` to skip struct expansion, `maxDepth=1` for one level (recommended), higher values for deeper structs.

```
widget.get_class_properties(classes=["Button", "TextBlock", "CanvasPanelSlot"], maxDepth=1)
  → all editable props with one level of struct expansion (e.g. Font.Size, ColorAndOpacity.R)
```

## See also

- [`widget`](widget.md) — the `widget.export_xml` / `widget.import_xml` verbs this markup feeds.
- [`widget.slot-layout`](widget.slot-layout.md) — slot and panel layout recipes in this dialect.
- [`widget.failure-modes`](widget.failure-modes.md) — classes and properties the XML round-trip cannot express.
