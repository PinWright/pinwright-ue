# Widget iteration loop

Use this inspect → plan → edit → save → verify loop before editing an unfamiliar widget blueprint.

## The iteration loop

Do not guess: a wasted edit can corrupt a `.uasset`, while `widget.export_xml` is cheap. Every session follows this loop:

1. **Inspect** — `widget.export_xml` (or `widget.describe` for bindings / runtime state) to get the tree, widget names, overridden properties, and slot layout.
2. **Plan** — decide which widgets to add / remove / reparent and which properties to set. Prefer the smallest change that expresses your intent.
3. **Edit** — `widget.add`, `widget.set`, `widget.reparent_widget`, `widget.remove_widget`, or `widget.import_xml`.
4. **Save** — `editor.save_all`. Asset edits are in-memory until you save.
5. **Verify** — re-inspect the affected subtree to confirm. For visual proof, use [`visual-review`](visual-review.md). For runtime layout, the user runs PIE and you check via `widget.describe` with `capture_source=live`.

## Tools at a glance

| Tool | Use when |
|------|----------|
| `widget.export_xml` | Dump a widget tree as XML-with-attributes; default shows overridden properties only (`include_defaults=true` for all). |
| `widget.describe` | Return per-widget bindings, event delegates, and runtime state; use for BP event-wiring investigation. |
| `widget.add` | Create under a named parent; returns the new widget's name. |
| `widget.set` | Set widget (`properties`) and/or slot (`slot`) properties by name. |
| `widget.reparent_widget` | Move to a new panel; preserve the widget/subtree and change only parent + slot. The root-to-target walk cannot reach orphaned widgets. |
| `widget.wrap` | Insert a non-root widget's new panel parent in one call; preserve the original parent slot via `ReplaceChild`. |
| `widget.replace_class` | Change class while preserving immediate children; handles non-root and root (root-swap uses native `WidgetTree->RootWidget` assignment because python cannot). |
| `widget.remove_widget` | Delete a widget and its subtree; cascades bound-event cleanup automatically. |
| `widget.import_xml` | Bulk restructure: `targetName` replaces the named subtree; without it, replace the whole tree including the root class. Read the H3 caveats before using. |
| `editor.save_all` | Persist dirty assets to disk. |
| `property.set` | Set properties on a UObject asset by path. Use for WidgetBlueprint-level settings like `DesignSizeMode` that aren't on any widget in the tree. |
| `python.execute` | Escape hatch when no typed handler covers the task. Several UE scripting surfaces are blocked — see [widget.failure-modes](widget.failure-modes.md) "Python constraints". |

## See also

- [`widget`](widget.md) — the full UMG verb reference.
- [`widget.failure-modes`](widget.failure-modes.md) — the known failure table and its workarounds.
- [`visual-review`](visual-review.md) — choosing the capture surface that proves the result.
