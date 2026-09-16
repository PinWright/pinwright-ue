# ui

Runtime UMG operations on live widget instances — instantiate a widget blueprint into the player viewport, mutate its live state (text, image, visibility), and remove it. Distinct from `widget.*`, which authors the underlying `UWidgetBlueprint` asset on disk: `widget.*` survives editor restart, `ui.*` is gone after PIE stops; pick the namespace by which side you want to persist.

## Availability

Only three method families here are plugin-gated: `ui.activatable_*` (push/pop), `ui.list_stack_widgets`, and `ui.get_active_widget` require the **CommonUI** engine plugin. That integration auto-loads when CommonUI is enabled in the host project; when it is disabled, those methods are unregistered and calling one returns `PLUGIN_DISABLED` (enable CommonUI and restart the editor). The rest of `ui.*` is always available.

## Cross-cluster overlap

`ui.screenshot` is a near-duplicate of `editor.screenshot`, kept for symmetry within this namespace. Prefer the canonical `editor.*` form unless you need this method's `ui.*`-runtime behavior (see its method page).

## See also

- `call("widget")` — author and edit the widget blueprint asset itself.
- `call("editor.play", …)` — start PIE so a `ui.create_hud` call has a viewport to add to.
- `call("property.set", …)` — for asset-side widget property changes (mutates the widget blueprint CDO, not the live instance).

### ui.screenshot

Near-duplicate of `editor.screenshot`, kept for symmetry within `ui.*`. Prefer `editor.screenshot`; it can fall back to the level-editor viewport, while this method requires a game/PIE viewport.

`filename` is optional and its `.png` extension is **auto-appended only when absent** (case-insensitive): pass `shot`, `shot.png`, or `shot.PNG` and the file lands as a single-extension `.png` — it is never doubled to `shot.png.png`. Omit `filename` for an auto-generated `Screenshot_<timestamp>.png`. Any directory component in `filename` is stripped and path traversal is rejected — put the target directory in `path` (which, unlike `editor.screenshot`, this method honors; it defaults to `Saved/Screenshots/WindowsEditor`). The success response echoes the resolved on-disk `screenshotPath` and its `filename`, so read those back rather than reconstructing the name you passed.

A viewport that has never presented a frame reads as empty and returns `BLANK_CAPTURE` without writing a file. Previously that read was stamped opaque and saved as a valid, correctly sized all-black PNG with `success: true` (also echoed in `imageBase64`). Treat `BLANK_CAPTURE` as "nothing has rendered yet, not a broken capture": start or step PIE with `call("editor.play")`, then retry. Success guarantees real pixels, so no separate blank-PNG check is needed.

It runs the same readback preamble as `editor.screenshot` and publishes the same `viewport.shadersCompiling`, `viewport.readbackFlushed` and `viewport.onScreenMessages` blocks: shader work still in flight after a bounded 20 s pumping drain returns `CAPTURE_NOT_READY` rather than photographing default materials, the asset-compile queue is disclosed but never waited on, and engine on-screen text is reported because no show flag removes it. Field list and limits are on [`render`](render.md).

### ui.create_hud

Requires an active PIE session; start it with `call("editor.play")`. The widget blueprint must already exist (create it with `call("widget.create_widget_blueprint", …)`).

`widgetPath` accepts the bare Blueprint asset path (`/Game/.../WBP_PlayerHUD.WBP_PlayerHUD`) — the generated-class `_C` suffix is resolved for you and is optional, matching every other class-path slot in the API. A short class name (`WBP_PlayerHUD`) also resolves.

Use the returned widget reference by name in later `ui.set_widget_*` and `ui.activatable_*` calls. Names are widget-tree slot names, not C++ class names. The top-level `widgetName` is the auto-suffixed `WBP_…_C_<n>` runtime-instance name required by `ui.activatable_*` as `host` (see `ui.activatable_push`).

### ui.activatable_push

Requires an active PIE session. `host` and `stack` name the live `UUserWidget` and its `UCommonActivatableWidgetContainerBase` child; start PIE and instantiate the host first with `call("ui.create_hud", …)`.

`host` is the **runtime instance name** — the auto-suffixed `WBP_…_C_<n>` that `ui.create_hud` returns as `widgetName`, **not** the authored widget blueprint asset/class (`WBP_PauseHost`). The asset name yields `HOST_NOT_FOUND` ("Host widget '…' not found in any live world"). Use `widgetName` verbatim. Worked chain:

```
ui.create_hud {widgetPath:"/Game/UI/WBP_PauseHost.WBP_PauseHost"} → {widgetName:"WBP_PauseHost_C_0"}
ui.activatable_push {host:"WBP_PauseHost_C_0", stack:"MenuStack", widgetClass:"/Game/UI/WBP_PauseMenu.WBP_PauseMenu"}
```

`widgetClass` accepts the bare Blueprint asset path (`/Game/.../WBP_Menu.WBP_Menu`); `_C` is optional and short class names also resolve, as with `ui.create_hud`. The class must subclass `UCommonActivatableWidget`, or the call returns `CLASS_NOT_FOUND`.

### ui.activatable_pop

Requires an active PIE session. `host` is the live runtime name returned by `ui.create_hud` as `widgetName` (the auto-suffixed `WBP_…_C_<n>`), not the asset/class name; the latter yields `HOST_NOT_FOUND`. `stack` names the `UCommonActivatableWidgetContainerBase` child. Omit `instanceName` to pop the active top entry, or pass an `instanceName` returned by `ui.activatable_push` / `ui.list_stack_widgets` to pop a specific entry.

### ui.list_stack_widgets

Requires an active PIE session. `host` is the live runtime name returned by `ui.create_hud` as `widgetName` (the auto-suffixed `WBP_…_C_<n>`), not the asset/class name; the latter yields `HOST_NOT_FOUND`. `stack` names the `UCommonActivatableWidgetContainerBase` child. Returns entries top → bottom with `instanceName`, `className`, and `isActive`.

### ui.get_active_widget

Requires an active PIE session. `host` is the live runtime name returned by `ui.create_hud` as `widgetName` (the auto-suffixed `WBP_…_C_<n>`), not the asset/class name; the latter yields `HOST_NOT_FOUND`. `stack` names the `UCommonActivatableWidgetContainerBase` child. Returns the active top entry's `instanceName` / `className`, or `NO_ACTIVE_WIDGET` when empty.

### ui.set_widget_text

Targets the **live** runtime instance only. Asset-side text changes belong on the widget blueprint's CDO via `call("property.set", …)`, or by editing the text in `widget.import_xml`'s markup.

### ui.set_widget_visibility

Sets the full `ESlateVisibility` on the **live** runtime widget. Pass `visibility` as one of `Visible`, `Collapsed`, `Hidden`, `HitTestInvisible`, `SelfHitTestInvisible` (case-insensitive) to reach any of the five states — note `Hidden` keeps the widget's layout space reserved while `Collapsed` removes it. The `visible` boolean is a back-compat shorthand only (`true`→`Visible`, `false`→`Collapsed`) and is ignored when `visibility` is supplied. An unrecognized `visibility` string is rejected with `INVALID_VISIBILITY` rather than silently coerced. The response echoes the resolved `visibility` enum string alongside the `visible` bool (true only for the fully-`Visible` state).

### ui.remove_widget_from_viewport

With an empty `key`, removes every top-level widget currently attached to the active game viewport world and returns `removedCount`. This is intentionally destructive. For named removal, the handler resolves the PIE-first runtime world and considers only top-level widgets that are in the viewport and have an owning player; editor previews, foreign worlds, and unowned transients are not candidates.

`key` may be the runtime `widgetName` (the short `UUserWidget` object name returned by `ui.create_hud`) or the exact `objectPath` from a previous response. An object-path match takes precedence. If a short name matches more than one live viewport instance, the call refuses with `AMBIGUOUS_ACTOR_NAME` and returns `requestedName`, `matchedBy`, `candidateCount`, and `candidates` containing each candidate's `objectPath`, `worldPath`, and `playerPath`; retry with one candidate path. A successful named response echoes `widgetName`, `objectPath`, `worldPath`, and `playerPath`, and success is reported only after the selected widget is no longer in the viewport. Missing targets return `WIDGET_NOT_FOUND` and a missing or mismatched game viewport returns `NO_VIEWPORT`.
