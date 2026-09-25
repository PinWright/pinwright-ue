# editor

Drive the editor application itself — viewport camera, view mode, PIE play/pause/step, console commands, undo/redo, save-all, open-asset, level open, screenshots, bookmarks, content-browser focus, synthetic input. Operations target the running editor process and its active viewport / world; for the broader process / engine scope (build, automation, job queue, GEngine-scope console) use `call("system")` instead.

## Cross-cluster overlap

- **`editor.console_command` ↔ `system.console_command`** — both run console commands; `editor.*` targets the editor world by default or a selected PIE world, while `system.*` targets the broader process / GEngine scope. Use `system.console.search` before unfamiliar commands.
- **`editor.screenshot` / `editor.play` / `editor.stop` / `editor.simulate_input` / `editor.save_all` ↔ `ui.*` same-named** — the `ui.*` versions exist as legacy wrappers; prefer the `editor.*` form here.
- **PIE control** — `editor.play`, `editor.stop`, `editor.pause`, `editor.resume`, `editor.step_frame` guard their lifecycle state (`alreadyPlaying` / `alreadyStopped` / `NO_ACTIVE_SESSION` rather than destructive retries). `play` waits for `PlayWorld`; `stop` waits until Unreal's broader session-in-progress predicate is false and `PlayWorld` is gone. One editor-thread lifecycle owner prevents overlapping waiters from claiming the same later session. Every terminal play/stop payload carries measured `pieActive` and `timedOut`. `editor.status` and `editor.pie_status` are the read-only probes, and both report `uiFrozen` beside `pieIsPaused` — the world tick and the UI clocks are separate things, see `editor.pause`.

## Save-all PIE gate

`editor.save_all` returns `PIE_ACTIVE` before touching any dirty package while Play In Editor is active. Stop PIE and retry the batch; a PIE response is not a partial-save result.

## Legacy

Note on legacy: the `editor.execute.*` family was dropped (Wave 1, Chunk 1C). Each former member has a modern replacement under a topical namespace — `editor.execute.spawn_actor` → `actor.spawn`, `editor.execute.delete_actor` → `actor.delete`, `editor.execute.get_all_actors` → `actor.list` (or `system.inspect.list_objects` for read-only audit), `editor.execute.create_asset` → the per-domain `*.create_*` methods (`asset.create_*`, `material.authoring.create_*`, `audio.authoring.create_*`, etc.), `editor.execute.resolve_object` → `system.inspect.inspect_object`, `editor.execute.list_actor_components` → `actor.get_components`, `editor.execute.blueprint_add_component` → `blueprint.scs.add_component`. If you find docs or skills still referencing `editor.execute.*` anywhere, that is stale — update them to the modern target.

## See also

- [`safe-mutation-save`](safe-mutation-save.md) — the read → mutate → verify → save loop for any editor change that must persist.
- [`editor.collision-review`](editor.collision-review.md) — proving a level's geometry actually has collision: the two collision view modes, the report that comes with them, and why the screenshot alone cannot answer the question.
- [`visual-review`](visual-review.md) — choosing the capture surface that proves an edit landed.
- [`level-review`](level-review.md) — comparable repeat passes and whole-map framing when the subject is a level.
- [`session`](session.md) — LAN `ServerTravel` outcome stages, exact-context completion, and local-player control.

### editor.create_utility_widget

**`name` is a BARE asset name, never a path.** It is validated against the engine's own object-naming rules (`FName::IsValidXName` / `INVALID_OBJECTNAME_CHARACTERS`) and the composed package path against `FPackageName::IsValidLongPackageName` — both before the parent class is resolved — so a `/`, `\`, `.`, `..`, a leading or trailing slash, a space or an unmounted root is rejected `INVALID_ARGUMENT` with the engine's own reason text quoted. This is not pedantry about naming: `name` is concatenated onto `folder` and handed to `CreatePackage`, which logs a package name containing `//` at **Fatal** — a verbosity that is not compiled out in any configuration — so a name like `a//b` did not fail the call, it ended the editor **process** and every unsaved package in it. A name of `..` reaches a second Fatal on the same function by resolving to an empty package name. The same defect was measured end-to-end on `foliage.add_type`; see that verb on the [`foliage`](foliage.md) page.

**`folder` (required) is how you choose the destination.** It is checked separately for traversal and unmounted roots and refused `SECURITY_VIOLATION`. `parentClass` (optional) is resolved before package creation: an unresolved or empty value returns `CLASS_NOT_FOUND`, while a resolved class that is not a `UEditorUtilityWidget` subclass, or is an abstract/deprecated/superseded subclass, returns `CLASS_NOT_INSTANTIABLE`. The canonical `UEditorUtilityWidget` root is allowed even though the engine marks it abstract. Invalid explicit parents never fall back to `UEditorUtilityWidget` and create no package or asset.

**`save` (optional, default `true`) controls persistence.** The default writes the `.uasset` to disk; `save:false` leaves the new package dirty in memory. The response reports `saveRequested` and `saved`; `pendingFlush` is true when a requested save did not become durable, and `saveState` / `saveDetail` explain the measured `AssetSaveState` when available. `className` reports the actual parent class stored on the created Blueprint.

`widget.create_widget_blueprint` on the [`widget`](widget.md) page is the plain-UMG counterpart and takes the same contract.

### editor.console_command

Run a console command in the editor world (e.g. `Stat FPS`, `showflag.Bloom 0`, `viewmode Lit`) — or, with the optional `world` selector, inside a **specific PIE world**. Distinct from `system.console_command` which targets the broader process / GEngine scope; use this one for editor-, viewport-, and PIE-world-scoped commands.

**World targeting** (`world`, default `"editor"` — fully backward compatible):

- `editor` — the editor world (the historical behavior).
- `server` — the first PIE context whose world has authority: the listen or dedicated server, or the sole instance of a standalone PIE session (which is its own authority).
- `client` — the first PIE client; `client:N` — the N-th PIE client, **1-based** (`client:1` is the first client), counted in `GEngine->GetWorldContexts()` order.
- `pie:N` — the PIE context with raw `PIEInstance == N`, for full manual control.

Classification reads each PIE world's `GetNetMode()`: listen/dedicated server → `server`, `NM_Client` → `client`, `NM_Standalone` → `standalone`. Selectors are case-insensitive. A well-formed selector that matches nothing errors `WORLD_NOT_FOUND` and lists the available PIE contexts (instance, kind, client ordinal, map) so the call can be corrected; a malformed selector errors `INVALID_ARGUMENT`. Call `editor.pie_status` first to see what is running.

Typical multiplayer-in-PIE loop: run the travel command in the server world (`{command: "servertravel MyMap", world: "server"}`), then connect or drive a client (`{command: "open 127.0.0.1:7777", world: "client"}`, or `world: "client:2"` for the second client). This replaces the old python.execute workaround of hand-iterating world contexts around `unreal.SystemLibrary.execute_console_command`.

The success response echoes `world` and `worldPath`, plus `pieInstance` and `kind` when a PIE world was targeted.

For multiple cvars, prefer `editor.set_preferences` so applied / failed are reported separately — but note it carries **no** scalability guard and writes at the default `ECVF_SetByCode`, so routing a scalability CVar through it pins that CVar above the user's Scalability panel for the session. Use `performance.set_scalability` for anything scalability-flagged.

Before issuing an unfamiliar command, search the live console registry with `call("system.console.search", {"query": "ScreenPercentage"})`. It returns matching command/CVar names, kind, help text, flags, and current values for variables.

Output from the command (logs printed by the command itself) is not captured by this RPC — it goes to the project's `Saved/Logs/<ProjectName>.log` file and the editor's Output Log. If you need the output, read the project log file.

Because a console line can reach any operation, calls arriving during `UWorld::Tick` are re-queued to the editor core ticker (at most one 0.1 s pass later), with the same response contract. This prevents `open <map>` from tripping `Assertion failed: !LevelList.Contains(TickTaskLevel)` — the crash `level.load` was gated against, but the console could still reach.

It does **not** make console commands safe, and only one class of line is refused (scalability-CVar sets, below). A command that starts a long synchronous operation holds the game thread until it finishes, with no progress and no cancel — after 90 s `ping` reports `EDITOR_GAME_THREAD_STALLED` naming this verb, which is detection, not recovery. A command that shuts the editor down still does. Prefer a typed verb where one exists — `level.load` over `open`, `editor.quit` over `quit`, `editor.set_view_mode` over `viewmode`. See "Console commands run at a safe point" and "Modal dialogs" on `call("system")`, and the freeze section of `call("python")` for the same hazard reached from script.

**A line that SETS a scalability CVar is refused** with `SCALABILITY_CVAR_USE_TYPED_VERB` — either an `sg.*` group or **any** CVar declared with `ECVF_Scalability` / `ECVF_ScalabilityGroup` (`r.ViewDistanceScale`, `r.Streaming.PoolSize`, `r.ScreenPercentage`, `r.MaxAnisotropy`, ...). A scalability group is not a special kind of variable: it is a name for a list of ordinary CVars in a `[<Group>@N]` ini section, and the console pins a member exactly as it pins the group. The set lands at `ECVF_SetByConsole`, the highest CVar priority, which outranks the `ECVF_SetByScalability` priority the editor's own *Settings → Engine Scalability Settings* panel writes at — for the rest of the session, so every later change the user makes to the owning group is silently discarded until the editor restarts. Use `performance.set_scalability`, which drives the groups through `Scalability::SetQualityLevels` at the panel's own priority — re-applying every member CVar of the groups it touches — and cannot create the pin. `force: true` runs the line anyway and accepts the pin. **Not refused**, because the engine performs no `Set` on them: **reading** such a CVar (its name with no value, or a bare `?`); the aggregate `scalability N`, which routes through `SetQualityLevels` too; and a first token that resolves to no console object at all (a typo, or an `Exec` command owned by a module). The rule tests the FLAG on the resolved console object rather than the spelling of the name, which is why `r.ScreenPercentage` and `r.VSync` are covered despite having no `BaseScalability.ini` row; `system.console.search` reports `Scalability` / `ScalabilityGroup` in each row's `flags`, so you can tell in advance which lines will be refused.

### editor.focus_actor

Select the named actor and frame it in the active level viewport (the editor's `F` shortcut). `actorName` is resolved through the same shared resolver every `actor.*` verb uses: it matches the **display label**, the **internal object name** (the `name` field `actor.list` / `actor.find_by_class` report, e.g. `BP_Gears_146`), or the full **object path**, all case-insensitively. So the identifier you already have from `actor.list` works directly — you do not need to look up the label first.

On no match it returns `[ACTOR_NOT_FOUND]` naming the identifier kinds tried. If you genuinely have an ambiguous label, pass the unique internal name or object path from `actor.list` to disambiguate.

The success payload is only `{success:true}` — it does **not** echo the camera transform the framing moved to. To read the resulting viewport camera back, call `system.inspect.get_viewport_info` (returns `cameraLocation` `{x,y,z}` / `cameraRotation` `{pitch,yaw,roll}` / `fov`). That read-back lives in the `system.inspect` namespace, not `editor.*`; `editor.status` does not carry it.

### editor.set_camera

`set_camera` returns only `{success:true}` and has no read partner of its own. To confirm where the camera ended up (or to read the current pose before moving it), call `system.inspect.get_viewport_info`, which returns `cameraLocation` / `cameraRotation` / `fov` from the active viewport client.

### editor.set_view_mode

Switch the active level viewport's rendering mode. Names match case-insensitively: `Lit`, `Unlit`, `Wireframe`, `DetailLighting`, `LightingOnly`, `LightComplexity`, `ShaderComplexity`, `LightmapDensity`, `StationaryLightOverlap`, `ReflectionOverride`, plus the two collision views:

- `collisionSimple` — **world collision**: the simple shapes gameplay traces, characters and physics actually hit. Aliases `worldCollision` / `playerCollision` / `collisionPawn`.
- `collisionComplex` — **precise collision**: the per-triangle geometry. Aliases `preciseCollision` / `visibilityCollision` / `collisionVis`.

**A mesh with no collision draws nothing in a collision view, so an empty view is pixel-identical to a fully collided scene.** A picture alone cannot tell you whether everything has collision. The collision modes therefore also return a `collision` report — counts plus the actors that contribute no pixels and why — and that report, not the screenshot, is the answer. Pass `collisionReport: false` to skip the level scan when you only want the mode switched.

**A viewport client keeps two view modes, one for perspective and one for orthographic**, and only the slot matching the current projection used to be written. A mode set while the viewport was perspective therefore applied to perspective captures and to nothing else, while the response reported plain success — which is how orthographic reference tiles kept coming back Unlit after a confirmed switch to `Lit`.

`projection` (default `"both"`) picks which slots to write: `both`, `perspective`, `orthographic` (aliases `persp` / `ortho`), or `active` for the old single-slot behaviour. An unrecognised value is rejected and nothing is written.

The response is **read back off the viewport client**, not echoed from the request:

- `viewMode` — the measured mode of the slot the viewport is rendering with right now.
- `requestedViewMode` — what you asked for. Compare the two.
- `applied: {perspective, orthographic, activeProjection}` — the measured mode of **each** slot.
- `previous: {perspective, orthographic}` — what each slot held before, so you can restore them.
- `verified` — `true` only when both slots were read back. It is `false` on the one path where no viewport client is resolvable at all: the mode is then issued as the `viewmode` console command, `viewMode` and `applied` are **omitted rather than guessed**, and `unverifiedReason` says so.

The mode **persists**: this verb does not restore the previous view, so switch back to `Lit` when you are done (`previous` tells you what to switch back to) or every later capture comes back as a collision view.

Full recipe, report fields and the simple-vs-complex inversion: [`editor.collision-review`](editor.collision-review.md).

### editor.set_game_view

Toggle Game View on the active level viewport — the `G` key. It swaps the viewport's `EngineShowFlags` between the engine's editor set and its game set, which is what hides editor-only billboards, light and audio radii, volume wireframes and mode gizmos.

**Game view does not clean the frame on its own, and this verb no longer implies that it does.** The response carries:

- `gameViewEnabled` — measured with `IsInGameView()` after the toggle, never the request value.
- `previous: {gameViewEnabled, overlayShowFlags}` — captured **before** the toggle. This verb deliberately does not self-restore (a capture burst wants game view to persist), so `previous` is how you put the viewport back the way you found it.
- `overlayShowFlags` — measured after the toggle: `splines`, `billboardSprites`, `selection`, `selectionOutline`, `grid`, `volumes`, `lightRadius`, `audioRadius`, `modeWidgets`, `navigation`, `game`.
- `componentVisualizersSuppressed` — spline handles, tangent arrows and similar per-component editor drawing. Governed by game view.
- `notGovernedByGameView` — the families it cannot touch: **active editor modes** (a landscape, spline or geometry tool keeps drawing its handles and dashed helper lines through the mode manager, which has no game-view guard — deactivate the mode instead) and anything drawn through `DrawDebug*` or a line batcher. A third family joined them: any `ShowFlag.<Name>` console variable forced to `0` or `1` is ORed over the view's flags after the viewport's own are copied, so it is process-global and a game-view toggle does not clear it. The capture verbs measure and report those as `viewport.showFlagOverrides`; reset one with `system.console_command {command: "<cvar> 2"}`.
- `overlayWarning` — present only when game view is on **and** `overlayShowFlags.splines` is still set. Spline components (water bodies, landscape splines, any `USplineComponent` with `bDrawDebug`) then keep drawing into the viewport. A line running along a river in a capture is that overlay, not foam. Toggling game view off and on again forces a fresh game flag set.

### editor.jump_to_bookmark

Move the viewport camera to a previously stored `editor.create_bookmark` slot. Like `set_camera` / `focus_actor` it returns only `{success:true}` and does not echo the restored transform; read it back numerically with `system.inspect.get_viewport_info` (`cameraLocation` / `cameraRotation` / `fov`).

### editor.play

Start a PIE session. Idempotent (`alreadyPlaying: true` when one is active — note that early-return answers **before** any param validation). Optional `numClients` / `netMode` (`standalone` | `listen` | `client`) start a multi-instance networked session: `netMode: "listen"` + `numClients: N` spawns a listen server plus auto-connected clients under one process.

The engine queues PIE startup for a later editor tick. The call stays open until `PlayWorld` exists and returns `pieActive: true, timedOut: false`; this deliberately does not wait for every multiplayer client or world context. A second `editor.play` while Unreal or PinWright already owns a lifecycle transition is rejected with `PIE_START_IN_PROGRESS` instead of replacing the request or creating another waiter. `editor.stop` may supersede a pending play request: the old play call then returns `PIE_START_CANCELLED` because its request generation was invalidated, even if a later `PlayWorld` is already visible.

If no world appears within 15 seconds, `PIE_START_FAILED` reports the measured lifecycle state. A request still queued in Unreal is cancelled. Once Unreal has consumed the request, `IsPlaySessionInProgress()` can remain true from session info before `PlayWorld` exists; cancelling then would invalidate engine state still used by deferred callbacks. PinWright instead retains that generation as a second bounded cleanup watcher: it requests end play if `PlayWorld` appears, releases ownership when the session becomes inactive or the cleanup bound expires, and rejects new play/stop lifecycle operations while it owns cleanup. `transitionCleanupPending: true` distinguishes this wait from `startRequestCancelled: true, startRequestWasQueued: true`.

**Blueprints in `BS_Error` do not block the start.** The tick that consumes the request runs outside the dispatcher's handler scope, and for any loaded Blueprint in `BS_Error` it raises the "Blueprint Asset Compilation Error" modal (`PlayLevel.cpp`, `ShowCompilationErrorsDialog`), which owns the game thread with no RPC able to dismiss it. `editor.play` therefore holds the automation-mode interval (see [`unattended`](unattended.md)) from the request until the wait ends, so that modal, and the pre-play recompile prompt, auto-answer to play. A started session returns `blueprintsWithErrors`: every loaded Blueprint in `BS_Error`, whose generated class may be stale. `blueprint.compile` errors include the engine's compile-time data validation (for example UMG's `Leak Detected` check on a widget that keeps its Slate widget alive at design time), so a Blueprint can land here without a graph error.

**`networkEmulation`** — explicit control over PIE network emulation (the Editor Preferences "Enable Network Emulation" feature that injects latency / packet loss):

- **Omitted or `{enabled: false}` → emulation is FORCE-DISABLED for the session.** The user's saved play settings are deliberately *not* inherited — per-user saved emulation (a leftover "Server Only / Average" checkbox) silently shaping every MP test session is the failure mode this param eliminates. Sessions started through `editor.play` always get a known wire.
- `{enabled: true, target?, profile?}` — applies emulation for this session. `target`: `serverOnly` (default) | `clientsOnly` | `everyone` (case-insensitive). `profile`: a Network Emulation Profile name from the project config, default `Average`.
- Profile names come from `UNetworkSettings::NetworkEmulationProfiles` (`[/Script/Engine.NetworkSettings]` `+NetworkEmulationProfiles=` entries; BaseEngine.ini ships `Average`, `Bad`, `BufferBloat` — a project can add its own). The actual lag/loss numbers live in `[PacketSimulationProfile.<Name>]` engine-ini sections, applied at net-driver setup via the session URL's `?PktEmulationProfile=` option. An unknown `target` or `profile` fails with `INVALID_ARGUMENT` listing the valid values; `target`/`profile` are validated even when `enabled` is false, so typos never pass silently. The details panel's "Custom" pseudo-profile is rejected like any unknown name — it would replay whatever hand-edited packet values sit in the user's saved settings, i.e. exactly the nondeterminism being eliminated.

**Persistence guarantee:** the user's saved `ULevelEditorPlaySettings` (EditorPerProjectUserSettings.ini) is never mutated. All overrides — numClients, netMode, emulation — are applied to a transient duplicate handed to the engine via `FRequestPlaySessionParams::EditorPlaySettings`; the engine re-duplicates that object, keeps it alive for the whole session, and hands it to every PIE instance login **including clients created later in the session**, so there is nothing to restore afterward and an editor crash mid-session cannot leak emulation state into a shutdown save.

On success the response echoes `numClients`, `netMode`, and `networkEmulation: {enabled, target?, profile?}` as applied (`target`/`profile` only when enabled). A timeout or cancellation omits those applied-settings fields because no started session was proven. Read an already-running session back with `editor.pie_status`. Errors: `INVALID_ARGUMENT` (bad `netMode` / `networkEmulation.target` / `networkEmulation.profile`), `UNSUPPORTED_ENGINE_VERSION` (overrides on an engine without `ULevelEditorPlaySettings`), `PIE_START_IN_PROGRESS` (a lifecycle transition is already owned), `PIE_START_CANCELLED` (`editor.stop` superseded this pending request), `PIE_START_FAILED` (the requested world did not appear within 15 seconds).

### editor.stop

Stop the active or pending PIE session. The handler snapshots lifecycle state before acting. If `PlayWorld` exists it calls `RequestEndPlayMap`; `endPlayRequested` is a direct read of Unreal's current `ShouldEndPlayMap()` flag, not a latched record of an earlier request. If no world exists and the start request is still queued, it calls `CancelRequestPlaySession` and reports `startRequestCancelled: true, startRequestWasQueued: true`. If the request has already been consumed but session info remains, it does not cancel: `transitionCleanupPending` is true while the bounded stop waiter watches for `PlayWorld`, requests end as soon as it appears, and waits for authoritative session inactivity. A stop timeout reports `PIE_STOP_FAILED` without invalidating that transition state. The call completes only when `IsPlaySessionInProgress()` is false and `PlayWorld` is gone. Success returns `pieActive: false, sessionInProgress: false, timedOut: false`; a 15-second failure includes the timeout snapshot and measured-current state. Calling it when Unreal has no queued or active session remains an idempotent success with `alreadyStopped: true`. A repeated call while another stop waiter or timeout-cleanup watcher owns teardown returns `PIE_STOP_IN_PROGRESS`.

The `editor.play` and `editor.stop` ticker waits keep a dispatcher-owned lifetime lease, but unlike a retained safe-point continuation they do not keep the single active-request guard held. That is what lets `editor.stop` enter while `editor.play` is waiting and cancel that play generation. If the dispatcher is torn down first, abandonment cancels the ticker before releasing the lifecycle generation and completes the open call with typed `PIE_START_FAILED` or `PIE_STOP_FAILED` data (`dispatcherEnded: true`, `cancelled: true`).

### editor.start_recording

Start a network replay recording in the active PIE game world. Edit mode is refused with `NO_ACTIVE_GAME_WORLD`; the verb never falls back to the editor world, which has no `GameInstance` and turns `DemoRec` into a consumed no-op. Startup uses `UGameInstance::StartRecordingReplay` directly and returns success only after `UReplaySubsystem::IsRecording()` confirms an active recorder; otherwise it returns `REPLAY_RECORDING_FAILED` with the requested name in `requestedRecordingName`. Success includes `recording: true`, `requestedRecordingName`, the replay subsystem's actual `recordingName`, `worldPath`, and `recordingBasePath` when the active demo driver exposes one. This is state-level confirmation of the active GameInstance/replay driver, not proof that a durable `.demo` artifact has been written.

### editor.pause

**Pausing stops two different clocks, and until 2026-09 it stopped only one of them.** `UWorld::bDebugPauseExecution` — what every "pause PIE" path writes — is read by `UWorld::IsPaused`, which gates actor ticking and the FX system. Slate is not in that path, so widgets kept ticking on real time: a HUD animation ran on through the pause and through the seconds of RPC round-trip before the capture, and a widget `Tick` that ran again against a frozen world could leave the HUD in a state the running game never renders (the reported case collapsed a crosshair, so the paused frame was misleading rather than merely empty). `editor.pause` now freezes both.

**What is frozen, and by which lever.**

- **UMG animations** (`PlayAnimation` / `UWidgetAnimation`) advance from the delta Slate broadcasts on its pre-tick, which the `Slate.UseFixedDeltaTime` cvar replaces with a fixed number. Pause drives that number to **0**; `editor.step_frame` drives it to the step size. This is the same pair the engine's own `AFunctionalUIScreenshotTest` uses for a deterministic UI screenshot.
- **Widget `Tick`** (the Blueprint Tick event, widget extensions, that widget's latent actions) does **not** use that delta — it gets Slate's real paint delta, which no public API can re-point. So it is switched **off** instead of slowed: every `UUserWidget` owned by a PIE world has its Slate tick flag cleared, and `editor.resume` recomputes the flag from the widget's own state rather than replaying a saved one. Editor-owned widgets (utility widgets, asset editors) are never touched.
- **The world** is unchanged: `bDebugPauseExecution`, as before.
- **Motion blur** is held at `r.MotionBlurQuality 0` for the life of the freeze (`motionBlurSuppressed` on the response), because a world that is not ticking still renders velocity vectors describing motion from before the pause. See `editor.step_frame`.

`freezeUi: false` restores the old world-only behaviour, and also releases a freeze an earlier `pause` / `step_frame` left in place — the parameter always describes the state you end up in. The freeze is released by `editor.resume`, `editor.stop`, and by the editor's **own** Resume / Stop buttons (the handler listens to `FEditorDelegates::ResumePIE` / `EndPIE`), so driving the toolbar by hand cannot strand dead HUD widgets. Read the current state from `uiFrozen` on `editor.status` / `editor.pie_status`.

### editor.step_frame

Advance the session by exactly one frame of `deltaSeconds` (default 1/60) and freeze again — **world and UI together**. The world tick, the Niagara/FX tick it drives and the UMG animation tick all run at the requested delta; widget `Tick` is switched back on for the stepped frame and off again after.

**Sizing a PIE world tick takes two levers, and until 2026-09 only one of them was pulled.** `FApp::SetUseFixedTimeStep` sizes the *engine* frame, but `UEditorEngine::Tick` sizes each PIE world's tick from that world context's own `PIEFixedTickSeconds` whenever it is above zero, and falls back to the editor's real frame delta when it is not. With the per-context field left at its default the stepped world simply took whatever the editor frame took — so a step reported ~1/3 s on a loaded editor and ~1/60 s on an idle one, *identically for a 0.017 s and a 0.9 s request*, while `uiSecondsAdvanced` tracked the request exactly. Both levers are now engaged for the stepped frame and restored after, including a session already running on a Client/Server fixed FPS.

**It answers after the frame has run**, and reports what the step actually bought rather than what was asked for:

| field | meaning |
|---|---|
| `worldSecondsAdvanced` | measured from `UWorld::TimeSeconds` across the frame |
| `uiSecondsAdvanced` | the delta the UMG animation tick consumed |
| `deltaHonoured` | the world advanced the request. **This is the field to branch on** |
| `worldSecondsExpected` | what one tick of this length can be in *this* level, read from its `AWorldSettings` before the frame ran |
| `worldDeltaClamp` | `none`, `timeDilation`, `worldSettings.MinUndilatedFrameTime` or `worldSettings.MaxUndilatedFrameTime` |
| `timeDilation` | the level's effective dilation, which scales the step before the clamp |

A single world tick is bounded in **both** directions: `UWorld::Tick` runs the delta through the level's own (virtual, so project-replaceable) `AWorldSettings::FixupDeltaSeconds`, which clamps into `[MinUndilatedFrameTime, MaxUndilatedFrameTime]`. A step below the floor therefore buys *more* world time than requested and a step above the ceiling buys less; `deltaHonoured: false` plus `worldDeltaClamp` says which happened, so a floored step is no longer indistinguishable from an honoured one.

**Never derive a timing by summing requested `deltaSeconds`.** Sum `worldSecondsAdvanced`, or — better — assert the state you are claiming (`IsAnyMontagePlaying`, an alpha, a flag) in the same frozen instant as the capture and quote that instead.

**This is the way to capture a sub-second HUD animation:** `editor.pause` → trigger it (`object.call_function`, `editor.simulate_input`, …) → `editor.step_frame` as many times as the animation is long → `editor.screenshot_window`. Nothing decays between the RPCs, because nothing is ticking between them.

**Motion blur is held off while the session is frozen** (`motionBlurSuppressed` on the response). A frozen world is frozen in order to be photographed, and velocity vectors are the one thing pausing does not freeze: nothing in the world is moving, so any motion vector still in the frame describes motion from *before* the pause. Frames captured off a paused session were coming back with the static environment — walls, ground, cover blocks — smeared into horizontal streaks, which is not a faithful image of any instant the running game renders. `r.MotionBlurQuality` is therefore driven to 0 for the life of the freeze and restored with the clocks by `editor.resume` / `editor.stop` / the editor's own buttons. Nothing else about the frame changes.

**The one thing still not exact:** across the stepped frame a widget's own `Tick` receives Slate's real frame delta (~1/60 s), not `deltaSeconds` — the tick flag is binary and the clock behind it is not settable. A HUD that integrates its alpha in `Tick` is therefore steppable and freezable, but its per-step advance is the real frame, while the world and any `UWidgetAnimation` advance by exactly `deltaSeconds`. That is why two numbers are reported instead of one.

Errors: `NO_ACTIVE_SESSION` (PIE not running), `STEP_IN_PROGRESS` (a previous step has not answered), `INVALID_ARGUMENT` (`deltaSeconds` non-finite, negative, or above the 1.0 s ceiling — the ceiling catches milliseconds passed as seconds).

### editor.status

`editor.status` carries only PIE / world state, **not** the viewport camera — read the camera location/rotation/fov via `system.inspect.get_viewport_info`. It surfaces only the *first* PIE world; for multi-instance sessions (listen server + clients) use `editor.pie_status`, which enumerates every PIE world context.

`pieIsPaused` and `uiFrozen` are separate readings and can disagree: `pieIsPaused` is the world tick (it is also true while simulating in editor), `uiFrozen` is whether `editor.pause` / `editor.step_frame` currently holds the session's UMG animation clock and widget ticks. A session paused with `freezeUi: false`, or one paused from the editor toolbar, reads `pieIsPaused: true, uiFrozen: false` — and a HUD animation captured from it is still moving.

### editor.pie_status

Read-only enumeration of every live PIE world context — the multi-instance companion to `editor.status`. Returns `inPie`, `count`, and one `contexts[]` entry per PIE instance:

- `pieInstance` — the raw PIE instance id (usable as `world: "pie:N"` in `editor.console_command`).
- `kind` — `server` / `client` / `standalone`, classified from the world's `GetNetMode()` (listen **or** dedicated server → `server`; a standalone instance → `standalone`).
- `netMode` — the exact net-mode name: `Standalone`, `DedicatedServer`, `ListenServer`, or `Client`.
- `mapName` — the world's map name with the `UEDPIE_N_` prefix stripped.
- `worldPath` — full object path of the PIE world (disambiguates instances of the same map).
- `gameStateClass` — class name of the world's current GameState; empty until one exists (on clients it can lag the server while connecting).
- `numPlayerControllers` — player controllers currently in that world.

While a session is active the response also carries a top-level `networkEmulation: {enabled, target?, profile?}` — the PIE network-emulation state **actually in effect** for the running session, read from the engine's session-scoped settings copy (the object every PIE instance login consumed), so it is truthful even for sessions started from the editor toolbar rather than `editor.play`. Record it alongside any measured MP session so results carry their wire state. `target`/`profile` appear only when enabled; a toolbar-started session with hand-edited packet values reports the pseudo-profile name `Custom`. The field is absent when PIE is not running. See `editor.play` for setting this state per session (default: force-disabled).

`contexts` is empty (and `inPie` false) when PIE is not running; that is a valid answer. The read is cheap and side-effect-free (only `GEngine->GetWorldContexts()` and each world's direct members), so poll it while a session starts to watch clients and GameStates appear. Use it before `editor.console_command` with a `world` selector.

### editor.simulate_input

**Key events go to the running GAME by default, not to the editor.** With PIE live, `key_down` /
`key_up` resolve one concrete world, viewport, local player, controller, and input device. The game
route calls that world's `UGameViewportClient::InputKey` after the current world tick and answers
only after the selected `UPlayerInput` has had its following input tick.

- `target` — `auto` (default: the game when PIE is running, else the editor), `game`, or `editor`.
  `game`, and any call that names a `world`, refuses with `PIE_NOT_ACTIVE` rather than quietly
  degrading into an editor keystroke.
- `world` — PIE selector for multi-instance sessions, same grammar as `editor.console_command`
  (`server`, `client`, `client:N`, `pie:N`). The default picks the active game viewport, preferring
  one that has a player controller (a dedicated-server window has no player to drive).

**The response reports what happened; it is never a hardcoded success:**

| field | meaning |
|---|---|
| `route` | `viewport_client` for a game-targeted key; `slate` only for `target:"editor"`. |
| `handled` | the immediate `UGameViewportClient::InputKey` return. It is diagnostic and never proves game delivery. |
| `playerInputEventQueued` / `playerInputEventId` | the exact edge id initially queued by the selected `UPlayerInput`; this is intermediate evidence, not success. |
| `deliveredToGame` | true only when that exact event id reaches the same `UPlayerInput`'s processed `EventCounts` on its next tick, with the requested pressed/released state. |
| `consumingRoute` | `player_input` on delivery; otherwise `viewport_client_before_player_input`, `player_input_not_processed`, `pie_ended_before_observation`, `dispatcher_ended`, or `none`. |
| `injectionFrame`, `pieInstance`, `kind`, `netMode`, `map`, `worldPath`, `playerController`, `pawn` | when and where the measured delivery was attempted. |

Game-targeted `success` equals `deliveredToGame`. Slate/global-preprocessor consumption and a true
viewport return cannot produce success by themselves; consumed-but-not-processed returns
`INPUT_FAILED` with the consuming route. Send `key_down` and `key_up` as a pair: down proves the
pressed edge and held state, while up proves the released edge and cleared held state. This proves
PlayerInput delivery, not that an Enhanced Input action or pawn callback executed.

An unknown FKey name is `INVALID_KEY`; a missing `key` or an unknown `target` is `INVALID_ARGUMENT`.

With `target: "editor"` (or `target:"auto"` and no PIE) the key goes to the focused editor widget,
and the response still carries `handled` and `focusedWidget`.

`mouse_click` / `mouse_move` are unchanged — they route through Slate at screen-space coordinates
and have no game destination; a click aimed at the PIE viewport's screen rectangle already reaches
the game as a real pointer event. `drive.key` stays the UI verb: it targets the focused widget
(optionally clicking a handle first) and never resolves a game destination.

### editor.screenshot

Capture a PNG into `Saved/Screenshots/`. Uses the game/PIE viewport when one exists; outside PIE it falls back to the active Level Editor viewport. Omit `width` and `height` to capture the surface at its live size, or supply both to render an exact size (each dimension is at most 16384 and the pair at most 64 million pixels). The capture completes synchronously inside a tracked job, so the returned ticket is already terminal and can be read through `system.job_status`.

The exact-size game/PIE path temporarily fixes the game viewport's render target, draws the scene at the requested size, renders its game-layer Slate/UMG tree into a transparent off-screen target of the same size, then composites both. The native window is not resized. `SGameLayerManager` re-reads the project UI scale curve from the temporary viewport size, so a 1920×1080 request reviews the HUD at its 1080p layout rather than scaling pixels from the on-screen window. The original render-target size and fixed-size state are restored before the job completes. Success reports `fixedSize`, `captureMode`, `dpiScale`, and `viewportRestored`; `captureMode:"fixedSizeScenePlusUmg"` is the positive evidence that the exact-size path ran, `"nativeBackBuffer"` means the live Slate-composited back buffer supplied native-size pixels, and `"sceneOnlyFallback"` means native Slate capture was unavailable so the native-size result contains only the scene readback (no UMG overlay).

`exposure` uses the shared capture spelling: a number is fixed EV100 shorthand, `{mode:"fixed", ev100:N}` is explicit, and `{mode:"auto"}` leaves auto-exposure running. On the game/PIE path a draw-scoped view extension sets the view family's native fixed-EV100 override, which takes precedence over the eye-adaptation method cvar, and writes matching manual fields into each view's final post-process settings after camera, volume, stereo, and other extension blends. It is released before viewport restoration and does not mutate persistent camera state. The response's top-level `exposure` block reports `pinRequested`, measured `pinned`, `restored`, `viewCount`, and `ev100`. On the Level Editor fallback, use the normal [`render.capture-exposure`](render.capture-exposure.md) `viewport.exposure` contract. This pin removes the camera component's moving auto-exposure from the shot; it does not invent a rendered frame when the viewport has never drawn one.

On the game/PIE branch, a viewport that has never presented a frame reads back as an entirely empty surface. The job now fails with `BLANK_CAPTURE` and **writes no file** instead of saving the valid, correctly sized, all-black PNG it used to report as a success. It means "nothing has rendered yet", not "the capture is broken" — let PIE run a frame (or wait for the level-editor branch outside PIE) and retry.

**Both branches run the readback preamble, and both publish `viewport.shadersCompiling`, `viewport.readbackFlushed` and `viewport.onScreenMessages`.** This verb draws a frame nobody has drawn before, which is exactly when the materials in view are still compiling — and a material whose shader map has not landed renders as the **default material** while the frame reads settled, non-blank and clean on every other field. Shader work still in flight after a bounded 20 s pumping drain now returns `CAPTURE_NOT_READY` with the drain evidence attached, instead of shipping stand-in materials. The gate does **not** wait on the asset-compile queue (normal after a map load); those counts are disclosed as `assetCompilationWarning`. The drain runs immediately before the readback — on the level branch from the capture's `BeforeFinalFrame` hook, after the pose apply and the warm-up settle — followed by the shared `FlushRenderingCommands` preamble reported as `readbackFlushed`. `onScreenMessages` is the engine text burned into the pixels, which no show flag or game-view setting removes. All three blocks are the same shape as on `render.capture_open_level` and `ui.screenshot` — read [`render`](render.md) for the field list, the limits of the flush, and the two channels the message survey can and cannot see.

On the level-editor branch the reported `cameraRotation` is now the pose the pixels show rather than the value stored on the viewport client. Those differ whenever the user has been orbiting the camera (alt-drag, or a camera lock): orbit mode derives the rendered view from a pivot, so the stored rotation is the orbit gizmo's and not the camera's. The screenshot itself is unchanged — it still reproduces exactly what is on screen — but the reported pose is now one that round-trips into `spatial.raycast_screen`.

### editor.frame_graph

Frame/zoom the on-screen node graph inside a target editor window. Unlike a Blueprint-specific verb, this works for **any** node-editor kind — Blueprint, material, Niagara, animation, state-tree, and so on — because it walks the window's widget tree for the first `SGraphEditor` and moves that widget's *live* view (via `GetGraphPanel()` / `GetCurrentGraph()`), with no asset-type knowledge. It moves the actual on-screen view; it does not render anything to a file (pair with `editor.screenshot_window` for an image).

The shared window selector uses `window_index` / `index` first, then a `window_title` / `title` substring, then the active top-level window (the `drive.*` rule; see `drive.list_windows`). A resolved window without a graph editor errors `GRAPH_EDITOR_NOT_FOUND`. The handler pumps a few Slate frames so lazy node widgets realize before bounds are queried.

**Framing modes** (`mode`, default `fit_all`):

| Mode | Frames |
|---|---|
| `fit_all` | Every node in the current graph. |
| `nodes` | The union of the nodes named in `nodeIds`. |
| `bounds` | An explicit graph-space rect from the `bounds` param. |
| `view` | Sets the view location + zoom directly (`viewLocation` + `zoom`). |

**Parameters:**

- window selector — as above.
- `mode` — `fit_all` (default) / `nodes` / `bounds` / `view`.
- `nodeIds` — for `mode=nodes`, an array of node GUID strings; their graph-space rects are unioned. Missing GUIDs are skipped and echoed back in `nodesNotFound`; the call errors `NODES_NOT_FOUND` only when none match.
- `bounds` — for `mode=bounds`, an explicit graph-space rect `{ topLeft: {x, y}, bottomRight: {x, y} }`.
- `viewLocation` / `zoom` — for `mode=view`, the top-left graph-space view location `{ x, y }` and a strictly-positive `zoom`, applied directly via `SetViewLocation`.
- `padding` — graph-space padding around the framed rect (default `48`; ignored for `mode=view`).
- `aspect` — optional target aspect (a `"W:H"` string like `"16:9"` or a bare float `W/H`). When set, the fit is constrained to the largest box of that ratio inside the live panel (letterbox); centering still uses the full panel. Ignored for `mode=view`.

The response returns `mode`, `windowTitle`, `appliedZoom`, and `appliedView` `{ x, y }`; for the bound-based modes it also returns the padded `framed` rect `{ topLeft, bottomRight }`, and for `mode=nodes` the `nodesFramed` count plus any `nodesNotFound`. Errors: `GRAPH_EDITOR_NOT_FOUND`, `NODES_NOT_FOUND`, `BOUNDS_EMPTY`, `PANEL_NOT_REALIZED`, the window-selector codes (`SLATE_NOT_INITIALIZED` / `NO_WINDOWS` / `WINDOW_NOT_FOUND`), and `INVALID_ARGUMENT` for a bad `mode` / `aspect` / `bounds` / `nodeIds` / `viewLocation` / `zoom`.

Note: framing acts on the live panel geometry (`SGraphPanel::GetTickSpaceGeometry().GetLocalSize()`), so the result depends on the current tab size — resize the window first (or use `editor.resize_window`) for a deterministic frame. Zoom is quantized to the editor's discrete zoom levels, so the applied zoom can differ slightly from an exact fit; the content stays centered.

### editor.resize_window

Resize a top-level editor window's **client** area (the content region; window chrome/border/title bar is excluded). The window is chosen with the shared window selector. `Resize` sets the client size in **physical** pixels; pass `logical=true` to give DPI-independent sizes that are multiplied by the window DPI scale before the resize.

Provide `width` and/or `height` (px), and/or an `aspect` ratio. With `aspect`, the missing dimension is derived from the provided one (or from the current width when neither is given). A **maximized** window will not visibly resize, so the call errors `WINDOW_MAXIMIZED` — restore the window first with `editor.set_window_state {state:'restored'}`, then retry (the editor commonly launches maximized). Note a docked asset editor shares the main-frame window, and the OS enforces a native minimum size, so a very small request can clamp.

**Parameters:** window selector, `width`, `height`, `aspect`, `logical` (default `false`). At least one of `width` / `height` / `aspect` is required.

The response returns `windowTitle`, `requestedClientSize` (physical px actually requested), and the post-resize read-back: `clientSize`, `windowSize`, `position` (each `{ x, y }`), `dpiScale`, and `wasMaximized`. Errors: `WINDOW_MAXIMIZED`, the window-selector codes, and `INVALID_ARGUMENT` when no dimension is given, `aspect` is malformed, or the resolved size is below 1×1.

### editor.set_window_state

Set a top-level editor window's window-manager state — the callable recovery for `editor.resize_window`'s `WINDOW_MAXIMIZED` dead-end (the editor commonly launches **maximized**, and a maximized window will not visibly resize). The window is chosen with the shared window selector; `state` picks the target mode via `SWindow::Restore()` / `Maximize()` / `Minimize()`.

- `state: 'restored'` (alias `'normal'`) — un-maximize **and** un-minimize back to a normal framed window. Call this first, then `editor.resize_window`.
- `state: 'maximized'` — maximize the window (e.g. to fill the frame before a whole-window `editor.screenshot_window`).
- `state: 'minimized'` — minimize the window. Refused with `WINDOW_MINIMIZE_REFUSED` while an automation session is running: a minimized window stops rendering, so every profiling and render number taken afterwards is a frozen last value (a hard ~333 ms frame cap, `DrawCalls` 0, a bit-identical `GameThreadTime`) that reads as a scene regression with nothing out of range to flag it.

**This is the one window verb that can target a MINIMIZED window, and `state:'restored'` against one is the in-band recovery.** Every other verb sharing the window selector (`drive.*` editor-chrome observe/act/capture, `editor.frame_graph`, `editor.resize_window`, `editor.screenshot_window`) still resolves over visible windows only — they read or act on a window's contents, which a minimized window does not have. Here the minimized top-level windows are **appended after** the visible ones, so no `window_index` changes meaning. With the whole editor minimized nothing is visible to name, so call it **with no selector**: `editor.set_window_state {"state": "restored"}`.

**Parameters:** window selector, `state` (required). 

The response returns `windowTitle`, `requestedState` (echoes what you asked for, alias and all), `state` — the state **measured** by reading the window back after the call, one of `restored` / `maximized` / `minimized` — and `stateMatchesRequest`. When those two disagree the response also carries a `warning` naming both: the window manager is free to decline a change (an offscreen window under `-RenderOffscreen` routinely does), and a response that echoed only the request would report that decline as a success. Also returned: the pre-change `wasMaximized` / `wasMinimized`, the post-change `isMaximized` / `isMinimized`, the resulting `clientSize` `{ x, y }`, and — whenever the window ends up minimized — a `recovery` field carrying the exact call that undoes it.

Errors: the window-selector codes, `INVALID_ARGUMENT` for a missing or unrecognized `state`, `WINDOW_MINIMIZE_REFUSED` (above), and `WINDOW_STATE_NOT_CHANGEABLE` — the selector **did** match a window, but it carries no native platform window, so `Restore`/`Maximize`/`Minimize` are no-ops on it. That code is what separates "nothing matched" (`NO_WINDOWS` / `WINDOW_NOT_FOUND`) from "matched, but its state could not be changed".

### editor.screenshot_window

Capture a full top-level editor window — **including its chrome** (menu bar, toolbars, panels, title bar) — to a PNG under `Saved/Screenshots/EditorWindow`. An explicit `window_index` / `index` selects first and takes precedence over a non-empty `window_title` / `title`; explicit selectors use the shared window resolver. When every selector is omitted (or the title values are empty and no index is supplied), this verb instead captures the exact main editor frame returned by `IMainFrameModule::GetParentWindow()`, never whichever asset window happens to be active. This main-frame default is specific to `editor.screenshot_window`; the shared selector behavior of every other `drive.*` and `editor.*` verb is unchanged.

This is the whole-window counterpart to the chrome-free graph capture: use it together with `editor.frame_graph` to produce a documentation shot of a framed graph editor, or pass an explicit selector to snapshot any editor dialog/panel. An unavailable, hidden, or minimized default main frame returns `WINDOW_NOT_FOUND`; the handler does not fall back to another active window. Under `-unattended` headless RHI the capture can come back empty (`CAPTURE_FAILED`).

**Parameters:** window selector, `filename` (optional; inside `Saved/Screenshots/EditorWindow`, `.png` appended if missing).

The response returns `path`, `filename`, `width`, `height`, `sizeBytes`, `windowTitle`, `windowType`, and `mimeType: "image/png"`. `windowTitle` and `windowType` identify the actual captured `SWindow`; `windowType` uses the same `Normal` / `Menu` / `ToolTip` / `Notification` / `CursorDecorator` / `Unknown` vocabulary as `drive.list_windows`. On UE versions before 5.8, that vocabulary also includes `GameWindow`; from UE 5.8 onward, game windows report `Normal`. Errors: the window-selector codes, `CAPTURE_FAILED`, `ENCODE_FAILED`, and `SAVE_FAILED`.

### editor.quit

Ends the editor process. It is the one verb whose blast radius is the whole session — including every other agent connected to the same editor — so it refuses in three situations rather than doing what it was told.

**This editor is shared.** One editor answers on one loopback port with one project-wide bearer token, and several agents reach it at once. A quit is therefore refused with `EDITOR_IN_USE` when a **different** client has driven this editor within the last 5 minutes; the error names that client's last method and how many seconds ago it ran. Your own traffic never counts — clients are told apart by the `X-PinWright-Client` header the bundled proxy sends per process — so closing the editor you have been using needs no flag. `force: true` clears this refusal and nothing else.

An editor nobody is driving produces no traffic, so the five-minute window eventually elapses and abandoned editors can still be reaped. Wait on `EDITOR_IN_USE`; forcing past it ends another session mid-work. This guard exists because an editor was shut down 49 s after serving `render.capture_asset_preview` for another agent, by a caller that concluded it was orphaned.

**Unsaved work.** Dirty content/map packages refuse with `UNSAVED_CHANGES` listing them, unless `save: true` (save first, unattended) or `discard: true` (clear the dirty flags and exit). `force` does **not** override this — the two refusals answer different questions, and neither is a way to lose work quietly.

**Shutdown ordering.** Two engine crashes live on the exit path, and both are avoided by what happens *before* exit is requested, not by anything at the fault:

- A live PIE session is ended first and waited out (up to 15 s). The response reports `pieWasActive` / `pieStopped`, or fails `PIE_STOP_FAILED` **without exiting** — exiting with PIE live faults in `UEditorEngine::EndPlayMap()` driven from a Slate destructor during `FSlateApplication::Shutdown()`.
- Every open asset editor is then closed, reported as `assetEditorsClosed` and `assetEditorsRemaining`. An asset editor left open is destroyed inside `FEngineLoop::Exit()`, after the editor subsystems are gone, and the engine's toolkit destructors do not survive that: `~FStaticMeshEditor` calls `GEditor->GetEditorSubsystem<UImportSubsystem>()->OnAssetReimport.RemoveAll(this)` unguarded (`StaticMeshEditor.cpp:271`) and faults on a null subsystem. A non-zero `assetEditorsRemaining` means a toolkit refused to close and that fault is still possible; unlike PIE it is not treated as fatal — the crash there is certain, here it is likely, and an editor that could never be shut down would be the worse failure. **Verified on UE 5.8:** a Static Mesh editor open at quit reports `assetEditorsClosed: 1, assetEditorsRemaining: 0` and the process reaches `LogExit: Exiting.` with no crash report; the original fault was captured on 5.7 before this step existed.
- Every job still `running` is ended last and listed in `jobsTerminated`: cancelled when its verb has a cancel hook (an isolated `system.run_tests` child tree is killed), otherwise failed with `EDITOR_EXITING`. Each is logged as a `LogPinWrightSubsystem` warning, and a client streaming one gets its terminal event before the process goes.

`render.capture_asset_preview` and `render.capture_animation_preview` used to leave a real asset editor window open on every call, so any session that captured a preview reached quit with windows open as a matter of course. Their `closeAfterCapture` now defaults to `true` and closes what the call opened, which removes that source — but a window a *human* opened, or one a capture left behind before this change, still has to be closed here, which is why this step exists rather than being delegated to the capture verbs.

**Parameters:** `reason` (recorded verbatim in the engine's exit log — write what actually decided the shutdown, since it is the only record of it), `save`, `discard`, `force`.

The response acks first and the process exits ~1 s later, so the reply flushes before teardown begins: a caller sees success, then the endpoint stops answering.

Only a dispatched RPC counts as driving the editor. `ping`, `initialize`, `tools/list` and wiki-page lookups are answered by the transport and never reach a handler, so an idle session that is merely connected — or one reading these pages — cannot hold an editor "in use", and cannot be mistaken for one that is.
