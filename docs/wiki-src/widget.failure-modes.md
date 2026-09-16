# Widget failure modes

Pitfall catalog for MCP widget authoring: orphan widgets, save/restart hygiene, Python limits,
Designer preview size, and recurring rendering/binding failures.

## Orphan-widget trap

`widget.reparent_widget` walks the tree from the root. A **detached** widget (`parent` is null but
the object still exists) is not found by name: `NOT_FOUND: Widget 'X' not found`, even though its
full object path loads.

This bites in two ways:

1. **`widget.remove_widget` followed by `widget.add` with the same name** — the tree has a
   temporary hole visible to concurrent inspection.
2. **`python.execute` calling `panel.remove_child(widget)` without re-attaching** — leaves an
   orphan in the outer `WidgetTree` with `parent = None`; reattach with
   `new_parent.add_child(orphan)` through `python.execute`.

Complete a Python detach-attach sequence in one script. If it fails midway, only Python can recover
the unreachable widget.

## Save hygiene

- `editor.save_all` persists every dirty asset, including edited Widget Blueprints; without it,
  restart loses the edits.
- Auto-save conflicts and source-control locks log "package was not saved". Confirm
  `savedCount == totalDirty`.

## Editor restart after C++ changes

Any C++ UCLASS / USTRUCT layout change (plugin or gameplay-module rebuild) requires a full UE Editor
restart before MCP sees new metadata. `unreal.find_object` and `FJsonObjectConverter` cache
reflection data on first access. Reconnect or restart the MCP client so schemas re-register.

Hot reload / Live Coding is **not** sufficient.

## Python constraints

`python.execute` is the fallback when no typed handler covers the operation, but UE Python blocks
surfaces marked `BlueprintProtected` or editor-only:

- `UWidgetTree.RootWidget` — blocked for read and write.
- `UWidgetBlueprint.WidgetTree` — read-blocked; load a child by full object path and call `.get_outer()` to reach the tree.
- UMG internals such as `PanelWidget.Slots` and skeleton/generated-class metadata are similarly hidden.

Check for a typed `widget.*`, `blueprint.*`, or `property.*` handler first. Typed handlers run C++
with editor access and bypass these blocks.

## Designer preview size

The UMG Designer preview-size dropdown has five modes: `FillScreen`, `Custom`, `CustomOnScreen`,
`Desired`, and `DesiredOnScreen`. They belong to the `UWidgetBlueprint` asset, not a tree widget,
and cannot be set by `widget.set`; use `property.set` on the asset:

```
property.set objectPath="/Game/UI/MyWidget.MyWidget"
             propertyName="DesignSizeMode" value="Custom"
property.set objectPath="/Game/UI/MyWidget.MyWidget"
             propertyName="DesignTimeSize" value="(X=260,Y=440)"
```

Widgets sized for a runtime container (a 260×440 panel or 48px bottom bar) are nearly unreadable at
the Designer's 1920×1080 preview. `Custom` plus the runtime size restores the proportions.

`CustomOnScreen` also draws the widget inside a scaled viewport frame for screen-relative HUD layout.

Set `DesignSizeMode` on new non-full-screen widgets; default `FillScreen` is wrong for chrome,
panels, list items, and popups.

## Common failure modes

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| Buttons render as empty boxes | Button has no TextBlock child | `widget.add TextBlock parentName=<button>`, set `Text` |
| All widgets crammed in a 100×30 box top-left | Child of CanvasPanel has no LayoutData | Set `slot.LayoutData` explicitly |
| Border renders as a dome / blob | `RoundingType=HalfHeightRadius` default with large radii | Set `RoundingType=FixedRadius` |
| Text overflows button on the right | Button is auto-sized narrow because its HBox sibling has `Size=Fill` and eats the row | Set the button's HBox slot `Size=(SizeRule=Fill,Value=1)` or set a fixed min width |
| HBox content hugs the left even though HBox fills width | HBox does not center children by default | Add Fill spacers at start/end, or change HBox's canvas slot to a point-X anchor |
| Bottom bar is the wrong height across resolutions | Using percent-Y anchors for chrome | Switch to point-Y anchor at bottom (Anchors (X,1)→(X,1)), Top offset = -height |
| Font property sets but nothing visible changes | FSlateFontInfo partial without a typeface | Include `TypefaceFontName="Default"` or a real font object path |
| Style variables were created but no widget changed | Declaring `FButtonStyle` / `FSlateBrush` member variables (via `blueprint.add_variable`) does not apply them | Set the style property directly via `widget.set` (e.g. `Button.WidgetStyle` to an `FButtonStyle` ExportText literal), or wire a property binding via BPIR; verify with `widget.describe` |
| BP "Event Dispatcher pin is not connected" after edit | A widget was removed and re-added, breaking name-based BP references | Rewire via BPIR; or use `widget.reparent_widget` to preserve identity |
| `widget.reparent_widget` says `NOT_FOUND` for a widget you know exists | Widget is orphaned (parent=None) from a prior detach | Reattach via `python.execute` calling `new_parent.add_child(orphan)` |
| Text renders as `NSLOCTEXT("ns","key","src")` literal at runtime | Old plugin build without `FTextStringHelper::CreateFromBuffer` parsing | Rebuild plugin; NSLOCTEXT / LOCTEXT / INVTEXT macros are now parsed by `widget.set` and `property.set` |
| Whole tree rewritten when you meant to edit one subtree | `widget.import_xml` called without `targetName` | Supply `targetName`, or use `widget.set` for property-only edits |
| BP event bindings broken after `widget.import_xml` | Import recreated every widget instance with a new GUID | Recompile BP; re-wire via BPIR if name-based rebinding didn't restore behaviour |
| Designer previews widget at 1920×1080 but runtime uses 260×440 | `DesignSizeMode` not set | `property.set DesignSizeMode=Custom, DesignTimeSize=(X=260,Y=440)` on the asset |
| Geometry numbers wrong for stretched slots | Known resolver limitation | Trust `Slot.LayoutData`, verify at runtime |
| New MCP tools not in schema after plugin rebuild | Editor caches reflection metadata | Restart UE Editor, reconnect MCP |
| Python says `Property 'WidgetTree' is protected` | UE blocks `BlueprintProtected` properties from python | Use `.get_outer()` via a child widget, or reach for a typed handler |

## See also

- [`widget`](widget.md) — the UMG authoring and inspection namespace these failures come from.
- [`widget.iteration-loop`](widget.iteration-loop.md) — the read-edit-verify loop that avoids most of them.
- [`visual-review`](visual-review.md) — proving a widget change landed with a capture.
