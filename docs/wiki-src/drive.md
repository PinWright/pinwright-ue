# drive

The agent-facing surface for observing, acting on, and verifying live running UI: it reads a surface's addressable elements, injects real input at their geometry, and asserts conditions against what is actually on screen. Use it to drive and test UI that is already running, versus `ui.*` for runtime UMG instance ops (create/mutate a live widget) and `widget.*` for authoring the `UWidgetBlueprint` asset on disk.

## Surfaces

Every verb takes a `surface` param naming which live UI it targets:

- `game` — live PIE/game UMG. Selected by `instance_name` (substring-matched against the backing widget name) or `root_index` (the Nth root, 0-based, which wins over `instance_name`). This is the default surface.
- `editor_chrome` — open editor Slate windows (main frame, asset editors, dialogs, transient popups). Selected by `window_title` (substring) or `window_index` (positional, wins); both unset picks the active top-level window. Enumerate targets first with `drive.list_windows`.
- `web` — the embedded CEF HUD (a WebUI browser, e.g. a PIE HUD). Selected by `browser_index` (default 0). Fully asynchronous — every observation/action is a CEF DOM round-trip — and limited in v1 (see Web limitations).

`surface` defaults to `auto`, which resolves to `game`. Any unrecognized token also resolves to `game`, so a handler never sees `auto` downstream. Acting on a surface with no live UI yields a clean coded error (`PIE_NOT_RUNNING`, `WEB_BROWSER_NOT_FOUND`, `SURFACE_NOT_SUPPORTED`), never a hang.

## Observation

`drive.observe` is the entry point. It returns the surface's `elements[]` — each with a stable `handle` (the addressing token every action verb consumes), `type`, `label`, `value` (present only for an editable input: its **live typed text** — what `drive.type` just entered; omitted when empty, since `label` instead carries the accessible/**hint** text), `enabled`/`visible`/`focused`/`interactable` flags, `geometry` (see below), and `surface`. Static text/labels are included for verification with `interactable:false`; only `interactable:true` elements are real action targets.

`enabled` and `visible` are **effective** state, folded down the widget's ancestor chain — a control under a disabled or `Collapsed`/`Hidden` parent reads `false` even though its own Slate flag says otherwise. (`visible` means "is drawn", so a `SelfHitTestInvisible` HUD label still reads `true`.) Such an element is still listed, so `widget_present`, `count` and `text_*` keep working against it; only its state flags go false.

`geometry` is `{ absolute: {x,y,w,h}, space: "desktop", stale?: true }`. `absolute` is in **desktop pixels** — the owning window's screen origin is already included, so it is directly what `drive.click`/`drive.hover`/`drive.drag` inject at and what an external OS-level injector needs; do not add a window origin to it. `space` names that space in-band so it never has to be inferred from this page. `stale:true` appears only when the element was not arranged for the current frame (it or an ancestor is `Collapsed`/`Hidden`), and in that case `absolute` is zeroed rather than reporting the last frame that did draw it — a stale element is never a valid click target, and `drive.click`/`drive.drag` refuse one with `TARGET_CHANGED`. Absence of `stale` means the rect is live, with one caveat: a widget that is visible but has never been painted (or was culled this frame) is not detected as stale and reports whatever it last stored.

One exception to `space:"desktop"`, which drive cannot currently detect: content under a retained `SRetainerWidget` (UMG's `URetainerBox`) is painted into an `SVirtualWindow` rooted at (0,0), so those elements' `absolute` is **retainer-local**, not desktop. Neither `drive.*` nor an external injector can aim at such an element by its reported rect.

By default the observation also attaches a Set-of-Mark `screenshot`: a PNG (base64) with numbered marks painted over the **interactables only**, capped at `mark_cap` (default 50). The screenshot honestly reports `marks_drawn` versus `marks_omitted` (offscreen, too small, or past the cap) so the agent knows which elements the image actually labels. On `game` and `web` the image is the game viewport's composited frame (scene, post-process and the UMG/Slate layers — the same pixels `editor.screenshot` reports as `captureMode: nativeBackBuffer`), falling back to the scene-only render target only when no back buffer can be read (headless / `-RenderOffScreen`); on `editor_chrome` it is the selected window. Marks are placed at each element's desktop rect minus the captured surface's desktop position, so they land on the element's pixels wherever the viewport or window sits on screen. Pass `screenshot:false` to skip it.

A rich HUD can carry 170-200+ elements, so `drive.observe` can compact its list on request: `interactables_only:true` drops static text/labels and returns only the actionable widgets (often a ~7x reduction), and `max_elements:N` (0 = unlimited) caps the array and reports how many were dropped via a top-level `omitted_count` (never a silent truncation). Both apply to every surface.

The async action and wait verbs do **not** embed a full observation by default — that was the main source of oversized responses. They carry one back under an `observation` field only when you opt in via the `observe` mode: `none` (the default — settle/diff result only, no element list or screenshot), `list` (elements, no screenshot), or `list+screenshot`. Set `include_journal:true` (with an optional `journal_since` cursor) to also attach a journal delta.

## Timing and settle

There are no blind sleeps. After a game/editor action, the settle driver samples the live UI each engine frame and runs a pure change-detection decision: it waits for the UI to **change then hold stable** for `stable_ticks` (default 2) consecutive ticks within `settle_budget_ms` (default 1500), or — if nothing ever moved — reports a clean quiet once `quiet_budget_ms` (default 500) elapses. `changed` and `outcome` are the settle loop's **structural** signal — they track whether the UI's *shape* (element types, geometry, visibility) changed then held stable, deliberately not its text content. So `changed:false` with outcome `no_change_within_budget` means no *shape* change settled within the budget, not a faked success — but a successful `drive.type` into a fixed-geometry field changes only that field's live `value`, which is by design invisible to this signal and so still reports `changed:false` / `no_change_within_budget`. To confirm typed text landed, assert it with `drive.expect`/`drive.wait_for` `text_equals`/`text_contains` (or read `drive.observe`'s `value`) rather than trusting `changed`; the `diff` below *does* list a value-only edit under its `changed` set. Outcomes are `settled_changed`, `no_change_within_budget`, `wait_for_met`, `timeout`, or `continue`.

After an action, the `diff` — a per-handle before→after delta whose `changed` set counts any element whose type, geometry, visibility, **or editable `value`** changed (so a pure `drive.type` edit surfaces here even when the settle `changed` above is false) — is reported as a compact **summary** by default: `{ appeared_count, disappeared_count, changed_count, appeared_sample, disappeared_sample, changed_sample, omitted }`, where each sample carries at most the first 15 handles and `omitted` flags any category that overflowed. Pass `full_diff:true` to get the complete appeared/disappeared/changed handle lists instead.

The web surface uses a different model: inject (click/type), then re-query the DOM exactly once and report the same `diff` — there is no per-tick fingerprint loop, so on web that one-shot `diff` (appeared/disappeared plus type/geometry/visibility changes) is the whole change signal. Note the value-aware `changed` set above is native-only: the web harvest folds an input's live value into the element's text/label, so `value` is empty on web and a value-only web edit does **not** surface in the `diff` — confirm typed web text with `drive.expect`/`drive.wait_for` `text_equals`/`text_contains` instead. `drive.wait_for` on web polls the DOM on a fixed ~0.25s interval until the condition holds or `timeout_ms` elapses.

Any verb can pass an explicit `wait_for` condition object instead of relying on settle-on-change: the driver then polls until that condition is met (outcome `wait_for_met`) or `timeout_ms` (default 5000) elapses (outcome `timeout`). In wait_for mode the change/stability tracking is bypassed — the condition and its timeout are the only terminal states. See Verification for the condition shape.

## Verification

`drive.expect` evaluates a single condition against a surface **right now**, synchronously, with no settle — use it to assert a post-state you already expect to hold. It shares one condition engine with the `wait_for` mode of the action/wait verbs, so the same condition object asserts immediately (`expect`) or polls over time (`wait_for`).

A `condition` object is `{ "type": <kind>, ... }`. The `type` is required; only the fields relevant to the type matter:

- `widget_present` / `widget_absent` / `widget_enabled` / `widget_visible` — `target` names the widget (matched against element handle/label/type). `widget_enabled` / `widget_visible` read the **effective** flags described under Observation, so a control under a disabled or collapsed ancestor is correctly not met — these are usable as gates ("wait until the Create button becomes clickable") and as hidden-assertions.
- `text_equals` / `text_contains` — `target` + `expected_text`. Compares the element's live typed `value` when it has one (an editable input after `drive.type`), otherwise its `label`; the result's `actual` names the field it read (`value="..."` vs `label="..."`). So after typing into a search/edit box you can assert the text actually entered, not just a static label.
- `count` — `target` + `expected_count`, compared with `count_op` (`eq` default, `ne`, `lt`, `lte`, `gt`, `gte`).
- `geometry_in_bounds` — `target` + `expected_bounds` (`{ "min": {x,y}, "max": {x,y} }`).
- `journal_event` — `target` names the event; reads the live journal tail.
- `journal_severity` — `severity` is the threshold (e.g. `error`); reads the live tail.

Journal conditions read the running session's event tail. To inspect that tail directly, poll `drive.events_since` with a `since` cursor: it drains newly emitted events plus changed variables and returns the next cursor (empty delta, no error, when no PIE/session is active).

## Web limitations

Web is v1 and partial. Supported on `surface=web`: `drive.observe`, `drive.expect`, `drive.click`, `drive.type`, `drive.wait_for`. The remaining action verbs — `drive.scroll`, `drive.drag`, `drive.hover`, `drive.key` — have no web mapping yet and return `SURFACE_NOT_SUPPORTED` rather than silently dropping the request. Web targets a browser by `browser_index` (default 0) instead of `instance_name`/`root_index`; with no live CEF browser the call returns `WEB_BROWSER_NOT_FOUND` synchronously.

## See also

- `call("ui")` — runtime UMG ops on a live widget instance (create a HUD, push/pop activatable stacks, set live text/visibility). `drive.*` reads and drives whatever UI is already on screen; `ui.*` puts specific widgets there.
- `call("widget")` — author and edit the underlying `UWidgetBlueprint` asset on disk (survives editor restart).
- `call("editor")` — drive the editor app itself (PIE play/stop, viewport, screenshots). Start PIE here so `drive.observe` on `surface=game` has a live world.

### drive.observe

Capture one observation of a surface. Params: `surface` (default `auto`→`game`), `instance_name`/`root_index` (game root, with `instanceName`/`rootIndex` aliases), `window_title`/`window_index` (editor chrome), `browser_index` (web), `screenshot` (default true), `mark_cap` (default 50), `interactables_only` (default false — return only actionable widgets), `max_elements` (default 0 = unlimited — cap the list COUNT, reporting drops via `omitted_count`), `max_bytes` (default 0 = unlimited — cap the elements' total serialized SIZE, reporting drops via `omitted_count`), `include_journal` (default false), `journal_since` (default 0). Returns `{ surface, root_name, frame, timestamp, elements[], omitted_count?, screenshot?, journal? }`, each element carrying its `label` and — for an editable input — a distinct `value` holding the live typed text (omitted when empty). Start here to get the element `handle`s every action verb needs; reach for `interactables_only`/`max_elements` on verbose HUDs.

On `game` and `web`, a viewport that has never presented a frame reads back as an entirely empty surface. The observation now fails with `BLANK_CAPTURE` rather than painting its marks over an all-black frame and returning it as a successful observation of a scene that was never rendered. Start PIE (`call("editor.play")`) and let it run a frame before observing.

Each element's `handle` is the full Slate/UMG widget-tree path, which dominates the payload — so on `editor_chrome` (and deeply-nested game/web UIs) `interactables_only` + `max_elements` bound the element COUNT but not the byte size, and a handful of capped elements can still overflow the inline display budget and spill to a file. Set `max_bytes` (e.g. `max_bytes:8000`) to bound the element list inline: it keeps adding elements until the serialized budget is reached, then reports the remainder in `omitted_count` (at least one element is always returned). `max_bytes` caps only the element list, so a scan/discovery observe must ALSO drop the default inline screenshot (`screenshot:false`, or `screenshot_mode:file`) — a single inline base64 screenshot alone exceeds the inline budget regardless of `max_bytes`. For a pure presence check — "is control X on this surface?" — prefer `drive.expect` with a `widget_present` condition instead: it returns a small fixed `{ met, actual, expected, detail, root_name }` shape (matching the control by `label`, so you need no handle) and never spills.

### drive.click

Move to a live element's center and inject a mouse-button click. Params: `handle` (required, from a `drive.observe` element), `button` (`left` default, `right`, `middle`), `os_input` (default false, see below), plus the common settle/observe/journal params (`observe` defaults to `none`, `full_diff` defaults to false). Re-resolves the handle against the live UI (a hidden/disabled re-resolve is `TARGET_CHANGED`, not a silent no-op), clicks, then settles. Returns the compact `{ outcome, changed, settled, condition_met, elapsed_ms, ticks, input_path, diff (summary), journal? }` — no element list/screenshot unless `observe=list`/`list+screenshot`. On web returns `{ ok, code, diff? (summary) }`.

`os_input:true` swaps the injection layer (`input_path` reports which one ran: `"slate"` or `"os_x11"`). The default `"slate"` path enters at `FSlateApplication::ProcessMouseButtonDownEvent`/`UpEvent` after an `ICursor::SetPosition`, and temporarily forces `SetHandleDeviceInputWhenApplicationNotActive(true)` so a click lands while the editor is not the active OS window. That is fast and reliable for editor chrome, but it *skips the OS and SDL layers entirely*: SDL mouse confinement (`EMouseLockMode::LockOnCapture`) and relative mode (`UseHighPrecisionMouseMovement`) never engage, and the forced inactive-input flag perturbs every `FSlateApplication::IsActive()`-dependent path — so capture/confinement bugs simply do not reproduce on it. `os_input:true` instead asks the X server for **real pointer events** (XTEST): an interpolated 24-step motion path from the pointer's current position to the element's center, then a genuine ~80 ms button hold. It never calls `ICursor::SetPosition`, never touches the inactive-input flag, and never calls `FSlateApplication::ProcessMouse*` — the input arrives over the same X → SDL → engine route a human's mouse takes. Element geometry needs no translation: `geometry.absolute` is already X11 root-screen coordinates.

`os_input` is **Linux/X11 only and mouse only**. On any other platform, or when no X display can be opened, it is refused with `INVALID_ARGUMENT` naming the reason rather than silently falling back to the Slate path. Keys have no OS variant — XTEST key events were observed not to reach the editor's SDL window even with X focus on it — so `drive.key` does not accept `os_input` at all and the dispatcher refuses it there. The gesture is paced with real sleeps (motion steps ~8 ms apart, ~80 ms press, ~120 ms after the release), so an `os_input` click blocks the editor for roughly half a second before the settle loop starts; pair it with `drive.input_state` to read who owned the mouse before and after.

### drive.type

Click an input element to focus it, then type a string one character at a time. Params: `handle` (required), `text` (required), plus the common params. Returns the same settle/diff/observation shape as `drive.click`. Supported on web. Use for text fields; use `drive.key` for individual keystrokes like Enter/Escape. A subsequent `drive.observe` surfaces entered text as the element's `value` field, and `drive.expect`/`drive.wait_for` `text_equals`/`text_contains` assert against that `value` — so the `drive.type` -> `drive.expect text_*` round-trip verifies what was typed (`label` still holds the input's hint/placeholder).

### drive.key

Inject a key event to the focused widget; with a `handle` it clicks that element to focus first. Params: `key` (required, e.g. `Enter`, `Escape`, `A`, `SpaceBar` — validated against `EKeys`, unknown names give `INVALID_KEY`), `handle` (optional focus target), `modifiers` (e.g. `ctrl+shift`, from shift/ctrl/alt/cmd), `action` (`press` default, `down`, `up`), plus the common params. Not supported on web (`SURFACE_NOT_SUPPORTED`). Use for keyboard shortcuts and submit/cancel keys.

This is the **UI** keystroke: it goes to whatever widget holds keyboard focus and never resolves a game destination, so it cannot drive a pawn. To send a key into a running PIE session — focus moved onto the game viewport, delivery verified against the player controller's input stack — use `editor.simulate_input` with `type: "key_down"` / `"key_up"` (see the [`editor`](editor.md) page).

### drive.scroll

Inject a mouse-wheel event at an element's center. Params: `handle` (required), `delta` (default 1; positive scrolls up), plus the common params. Returns the settle/diff/observation shape. Not supported on web. Use to scroll a list or scrollbox into range before observing.

### drive.drag

Press at one element's center, drag to a second point, and release. Params: `handle` (required, the press point), and a release point as either `to_handle` (its live center) or both `to_x` and `to_y` (absolute **desktop** pixels, the same space as an element's `geometry.absolute`) — missing both is `INVALID_ARGUMENT`; `duration_ms` (default 200) spreads the interpolated moves; plus the common params. Not supported on web. Use for sliders, reorderable lists, and drag-drop.

### drive.hover

Dispatch a real mouse-move to an element's center so hover state fires. Params: `handle` (required), `os_input` (default false — the same OS/X11 injection opt-in documented under `drive.click`, moving the pointer along an interpolated XTEST path with no button press), plus the common params. Returns the settle/diff/observation shape plus `input_path` (`"slate"` or `"os_x11"`). Not supported on web. Use to surface tooltips or hover-only UI before observing — and with `os_input:true` to move the real pointer over a viewport that confines or hides the cursor, which the Slate path cannot exercise.

### drive.wait_for

Poll a surface across frames until a condition holds or the timeout elapses — no input injection. Params: `surface`, `instance_name`/`root_index` (or `browser_index` for web), `condition` (required, see Verification), `timeout_ms` (default 5000), `stable_ticks`/`quiet_budget_ms`/`settle_budget_ms` (carried for parity), `observe` (default `none`), `include_journal`, `mark_cap`, `journal_since`. Pre-checks for live UI so a no-PIE/no-browser case errors immediately instead of polling the full timeout. Returns the compact `{ met, outcome, elapsed_ms, ticks, matched?, journal? }` — `matched` is the single matched element when the condition is met (no full element list unless `observe=list`/`list+screenshot`). Web returns `{ met, elapsed_ms, matched? }`. Use to wait out an async transition (a screen appearing, a count reaching N) before asserting.

### drive.expect

Evaluate a condition against a surface immediately, synchronous, no settle. Params: `surface`, `instance_name`/`root_index` (or `browser_index` for web), `window_title`/`window_index` (editor chrome), `condition` (required, see Verification). Returns `{ met, actual, expected, detail, root_name }` (web omits `root_name`). Use to assert a state you expect to hold now; reach for `drive.wait_for` when the state may still be settling.

### drive.events_since

Drain the live journal tail from a cursor. Params: `since` (default 0; pass back the previous response's `cursor` to poll incrementally). Returns `{ events[], changed_variables[], cursor }` — an empty delta with cursor 0 (not an error) when no PIE/session is active. Use to inspect journal events/variables, or to feed `journal_event`/`journal_severity` verification with the right cursor.

### drive.list_windows

Enumerate open top-level editor windows so an agent can target editor chrome before observing/acting on it. No params. Returns `{ windows[{ title, type, index, maximized, geometry.absolute{x,y,w,h} }], count }`. A window's `index` selects it via the `window_index` selector and its `title` via `window_title` (substring) on the editor-chrome drive verbs. `maximized` is the live `SWindow::IsWindowMaximized()` state — the same signal `editor.resize_window`'s `WINDOW_MAXIMIZED` gate and `editor.set_window_state`'s readback use — so a `maximized:true` window must be restored first (`editor.set_window_state {state:'restored'}`) before `editor.resize_window` will act. (No `minimized` field: this enumeration excludes minimized windows, so one never appears here — an empty `windows` array on a live editor means the whole editor is minimized, not that Slate is down. `editor.set_window_state` is the only verb that can resolve a minimized window; call `editor.set_window_state {"state": "restored"}` with no selector to recover from that state, then re-list. It is a control verb, not an observation one — it changes the state it reports.) Use first whenever `surface=editor_chrome`.

### drive.input_state

Read-only snapshot of **who owns the mouse right now**. No params of its own: it accepts `surface` and `instance_name`/`root_index` for symmetry with `drive.observe` and ignores them, because everything it reports is per-application or per-game-viewport rather than per-UMG-root. Returns:

- `slate_active` — `FSlateApplication::IsActive()`. The Slate injection path (`drive.click` without `os_input`) temporarily forces `SetHandleDeviceInputWhenApplicationNotActive(true)` around its injection; `os_input` never touches it, so this is the field that tells you whether the app was genuinely foreground when something happened.
- `cursor_captor` — `null`, or `{ type, debug_path }` for the widget holding the cursor user's mouse capture.
- `focused_widget` — `null`, or `{ type, debug_path }` for the cursor user's focused widget.
- `viewport_has_mouse_capture` / `viewport_is_captor` / `viewport_has_focus` — the game viewport widget's own state. `has_mouse_capture` is true for the viewport **or a descendant**; `is_captor` is the exact identity test, so the two disagree precisely when a child widget stole the capture. All three are `null` when no game viewport exists (no PIE session).
- `mouse_capture_mode` / `mouse_lock_mode` / `hide_cursor_during_capture` — the `UGameViewportClient` policy, as enum member names (`CapturePermanently`, `LockOnCapture`, …). `null` with no game viewport.
- `show_mouse_cursor` — the first local player controller's `bShowMouseCursor`; `null` when there is no controller.
- `os_cursor` — `{ x, y }` from `FSlateApplication::GetCursorPos`, in the same absolute desktop pixels as an element's `geometry.absolute`, so you can verify where the pointer actually ended up after an `os_input` action.

It reads state and changes nothing, and it does not require PIE — outside a session the viewport-scoped fields are `null` and the Slate-scoped ones still answer. Fails only with `SLATE_NOT_INITIALIZED`. Use it on both sides of a `drive.click`/`drive.hover` to see what the injection did to capture and focus, and to tell a Slate-path artifact from a real capture bug: pair it with `os_input:true`, which is the only path that exercises SDL confinement and relative mode.
