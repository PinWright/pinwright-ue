# Changelog

## 1.0.0

- Added: graph layout keeps comment boxes. Every PinWright layout (BPIR / MGIR / AGIR compiles,
  `material.authoring.auto_layout`) records each comment's members from the rects before it runs;
  a comment around unmoved nodes is an obstacle the moved nodes stay out of, and a comment with
  moved members is re-fitted around them (title bar included, nested comments inner first), undoable
  with the moves. `material.authoring.auto_layout` reports `commentsRefit[{nodeId, from, to}]` and
  `sizeSource {measured, estimated}`. BPIR's unused comment-box emitter was deleted.
- Added: Blueprint, anim and state-machine layouts measure node sizes and pin rows from the nodes'
  editor widgets (offscreen Slate prepass, no window) whenever Slate runs, falling back to the
  estimator in commandlets; `UBpirLayoutSettings::bMeasureNodeSizes` turns it off.
- Added: `GraphLayoutMetrics` counts backward edges and, for edges carrying pin anchors, scores
  straightness and crossings on pin-to-pin segments with a per-edge row delta and the basis used.

- Added: `editor_start`, `editor_restart`, `editor_run_tests` and `editor_build` refuse before
  spawning when the machine is short of memory (`LAUNCH_MEMORY_LOW`: available physical memory, or
  Windows available commit, below the launch's expected peak plus `$PINWRIGHT_LAUNCH_RESERVE_GB`,
  default 3, negative disables) or already runs `$PINWRIGHT_MAX_EDITORS` PinWright-launched editors
  (`EDITOR_LIMIT_REACHED`, unset = no cap); both name the running editors. A run killed from outside
  (Linux SIGKILL; Windows a non-zero exit with no crash banner and no exit request in its log) gets
  the supervisor verdict `EDITOR_KILLED_EXTERNALLY` / `COMMAND_KILLED_EXTERNALLY`, and
  `editor_test_status` / `editor_build_status` report `killedExternally` and `logStoppedAt`.
- Added: `EDITOR_ALREADY_RUNNING` names the editor holding the checkout's MCP port (`owner`: pid,
  `launchedBy`, reason, mode, start time, log, command line), and `editor_start` /
  `editor_run_tests` take `slot_wait` (seconds) to wait for that editor to exit instead of polling.
- Fixed: Linux capped runs. A drained suite's forced `-TestExit` exit (`_exit(1)` on Unix) is no
  longer `EDITOR_EXIT_NONZERO`; `capSeenByEditor` reads `n/a` (UE does not read cgroup limits)
  instead of a misleading `False`; an OOM kill is still counted when systemd removed the scope
  before the final read; and the supervisor stops transient scopes the systemd user manager left
  `active` with no processes.

- Added: `drive.os_gesture`, a real-pointer (XTEST) click or drag at a PIE game-viewport pixel
  or a world actor (`{actor, component?}`, projected through the player's view). It takes
  `button`, `hold_ms` and pressed `waypoints`, and runs its steps across engine frames, so per-frame
  mouse polling (gizmo drags, box select) sees the button held. The `os_input` gates apply.
  `drive.drag` now accepts `os_input`.
- Added: `pose_search.create_schema` accepts any concrete `UPoseSearchFeatureChannel` kind
  (`Trajectory`, `Velocity`, `Heading`, `Pose`, `Phase`, ... as well as `Position`); every other
  key of a channel spec sets that channel's editable property of the same name by reflection, and
  the response echoes each created channel as `channels[]` `{kind, className, settings}`. An unknown
  setting is now refused with `INVALID_ARGUMENT` instead of silently ignored, a `kind`-only spec is
  honoured, and channel specs are validated before the schema asset is created.
- Fixed: `pose_search.*` no longer compiles out to `PLUGIN_DISABLED` on UE 5.3-5.5. The handler
  gated on `PoseSearchFeatureChannel_Position.h`, which is a Private header before 5.6; it now gates
  on the schema/database headers and reaches channel classes by reflection (unverified on 5.3-5.5).
- Added: `ui.create_hud`, `ui.set_widget_*`, `ui.remove_widget_from_viewport`, `ui.activatable_*`,
  `ui.list_stack_widgets` and `ui.get_active_widget` take `world` (`server` / `client:N` / `pie:N`,
  as `editor.console_command`) to act in one instance of a multi-client PIE, and echo `pieInstance`.
  An omitted `world` with several PIE worlds running returns `TARGET_AMBIGUOUS` instead of acting in
  whichever instance came first.
- Added: `niagara.create_module_script` creates a standalone module from HLSL, wired
  Input -> MapGet(`Module.<in>`) -> CustomHlsl -> MapSet(`<namespace>.<out>`) -> Output. It sets the
  usage bitmask from the required `usages`, compiles, and reports the measured `compile.status` and
  errors. Refuses an existing asset. UE 5.5+.
- Added: `niagara.graph.create_node` / `connect_pins` / `remove_node` and `niagara.set_pin_default`
  accept standalone Niagara Script assets, refusing `EDITOR_OPEN` while the script editor holds one.
  A CustomHlsl node created with `payload.inputs` / `payload.outputs` gets those typed pins; returned
  pins carry `niagaraType`.
- Added: `niagara.get_compiled_script` reads per-script compile output: stats, compile events, GPU
  permutations, and on request the generated HLSL / VM assembly. Supports `maxChars` truncation and
  reports text the engine did not retain as `<field>Missing`. `forceCompile` restores the package
  dirty flag.
- Added: `material.audit` validates material graphs (`assets` or `folder`): `island`, `null_texture`,
  `null_function`, `unused_param`, `duplicate_param`, `blend_output_mismatch`, `uv_width`,
  `expression_budget`, plus opt-in `shader_compile` (`includeShaderCompile`). Findings carry the
  `nodeId` material.graph verbs accept; read-only, no fix mode; `pass` follows the shared audit rule.
- Added (untested): `drive.click` / `drive.hover` `os_input` on Windows through `SendInput`
  (`input_path: "os_win32"`): virtual-desktop absolute moves with the X11 path's pacing, a
  session-wide named-mutex lock, `WindowFromPoint` ownership gate, a foreground step before the
  press (`FOREGROUND_LOCKED` when Windows refuses), and a typed refusal in offscreen / nullrhi /
  commandlet editors. Written on Linux; not yet compiled or run on Windows.
- Added: `blueprint.compile_batch` compiles an `assets` list or a `folder` in one job and returns
  one row per Blueprint (`statusBefore`, `outcome` compiled | failed | refused | unloadable, errors,
  warnings, `reinstanced`) with totals that sum. A live-instance refusal is a row, not a call
  error; `onlyStatus: "error"` recompiles the loaded Blueprints in the Error state. Progress per
  Blueprint, nothing saved, one deferred GC. `blueprint.compile` errors gain `nodeGuid` / `graph`
  for node-level messages, and both verbs take `warningsAsErrors`.

- Fixed: `widget.screenshot_designer` `target:"preview"` (and `asset.dump`'s `preview.png`) waits,
  up to 20 s, for the textures and materials the preview references before drawing, so a cold
  widget no longer comes back missing images or font-material text; the response adds
  `previewComplete` and `readiness` (`notReady` names anything still compiling). The Designer's
  dashed outlines are no longer drawn into the preview (`designerOutlinesHidden`).
- Added: `blueprint.preview_construction` runs an Actor Blueprint's construction scripts on a
  throwaway transient instance (`world: preview` by default, or `editor`), with optional
  `variables` applied first, and reports every component built (`creationMethod`, attach parent,
  relative and world transform, sparse property diff), child actors and bounds. Nothing is placed
  or dirtied. A Blueprint in compile error is refused with `BLUEPRINT_COMPILE_FAILED`.
- Changed: game-surface `drive.*` with no `instance_name` / `root_index` walks every live UMG root
  on the viewport (z-order, bottom-most first) instead of failing `AMBIGUOUS_LIVE_ROOT`; each
  element carries `root`, handles are unique across roots, and `root_name` lists the walked roots.
- Added: `world` on game-surface `drive.observe` / `expect` / `wait_for` / action verbs, in
  `editor.console_command`'s grammar, picks the PIE instance (the listen-server host is now
  reachable). Omitted, it is the only PIE instance with a game viewport, and `TARGET_AMBIGUOUS`
  when several have one, instead of the ambient `GEngine->GameViewport`, which flipped between
  instances. `drive.observe` reports `world` / `world_kind` and screenshots that instance.
- Fixed: `drive.observe` on `editor_chrome` fails with `BLANK_CAPTURE` for a window that read back
  entirely empty, instead of returning its marks over a black frame as a successful observation.
- Fixed: class lookups by bare short name (`Object`, `Actor`, `PointLight`) no longer log
  `Failed to find object 'Class <Name>'` warnings before succeeding; the class resolvers no longer
  attempt a load for a bare name or a `/Script/` class.
- Changed: `python.execute` adds a `Warning` to `log` when the script calls `is_bound(`: the
  engine's Python wrapper for a Blueprint event dispatcher has reported unbound while bound (and
  `dir()` on it crashed the editor). `property.get` is the reliable read; its delegate value
  (`bindingStatus`, `bindings[]` of `{object, function}`) is now documented on the `property` page.
- Added: `editor.dismiss_notifications` closes the editor's notification toasts (which
  `editor.resize_window` cannot clear) and reports each one's window closed as measured.
  `drive.click {os_input:true}` (and `drive.hover`) now refuses with `TARGET_OCCLUDED` when
  another window of this editor is on top at the target by Slate's window order, before any X
  event is sent. `TARGET_OCCLUDED` adds `occluding_window_type` and `recovery`, and
  `editor.resize_window` warns when the measured client size differs from the request.
- Fixed: `drive.observe` / `drive.expect` / action targeting report `visible:false` (geometry
  `stale`) for an element lying wholly outside a clipping (`ClipToBounds`) ancestor, such as a
  closed dropdown translated out of its panel; `drive.click` / `drive.hover` refuse it with
  `TARGET_CHANGED` instead of settling with `no_change_within_budget`.
- Fixed: `widget.add`, `widget.duplicate`, `widget.replace_class` and `widget.import_xml` no longer
  save a placed Blueprint UserWidget's own widget-variable references (its inner widgets) into the
  parent asset; the placed template carries them null, as a loaded one does.
- Changed: the six `system.inspect.get_*` game-framework singleton readers (`get_game_state`,
  `get_player_states`, ...) take `editor.console_command`'s `world` selector and, omitted, answer
  from the PIE authority (listen/dedicated server) instead of the newest PIE world, which was a
  client in a listen-server session. Responses add `world`, `worldDefaulted`, `pieInstance`,
  `worldPath`, `netMode` and `kind`.
- Fixed: `system.inspect.inspect_object` reports Transient UPROPERTYs (flagged `Transient`)
  instead of dropping them, and names every property it still leaves out in `omittedProperties`.
- Changed: `material.authoring.auto_layout` reports what it actually moved (`movedCount`,
  `unchangedCount`, `moved[{nodeId, from, to}]`, read back after the write) instead of
  `expressionsLaidOut`, the total expression count; the moves are one undoable transaction, and
  none is recorded when nothing moved.
- Changed: graph auto-layout is PinWright's own layered formatter (`PwGraphLayout`) for BPIR
  compile/insert, `material.compile_mgir` / `material.authoring.auto_layout`, AGIR and CRIR
  compiles. It sizes every node (estimated from its title and pins), so laid-out nodes never
  overlap; every created entry in a graph is laid out, not only the first; data nodes sit in
  columns left of their consumer with pin-aligned wires; material and anim graphs grow leftwards
  from their output node. The BPIR Layout settings `NodePadX` / `NodePadY` / `PinPadX` /
  `InternalGridPx` are now `ColumnGapPx` / `RowGapPx` / `DataColumnGapPx` / `GridSnapPx`
  (`IntraParameterPadY`, `CollisionIterationCap` and `TraversalIterationCap` are gone); re-apply
  any customised values. Node sizes and pin rows now follow the editor's drawing (subtitle
  headers, value boxes, compact nodes, material previews, RigVM value editors); the K2 defaults
  are `HeaderHeightPx` 24 and `PinRowHeightPx` 32.
- Changed: `drive.click`, `drive.hover`, `drive.scroll`, `drive.drag` and `drive.key` on
  `surface=web` deliver real input through Slate into CEF (trusted events, CSS `:hover`, key
  default actions) instead of dispatching synthetic DOM events, refuse a covered target with
  `TARGET_OCCLUDED` (`occluding_element`), and settle and respond like the game surface
  (`{ outcome, changed, settled, condition_met, elapsed_ms, ticks, input_path, diff }`, no more
  `ok`/`code`). Web `drive.scroll` `delta` now means wheel notches with positive scrolling up, web
  `drive.key` refuses unknown key names with `INVALID_KEY`, and `os_input` is refused on web.
- Removed `system.run_ubt`. Build the editor target from a shell with the editor closed
  (`Build.bat`/`Build.sh ... -TargetType=Editor`), or use `system.live_coding_compile` to
  hot-patch a running editor.
- Added: Blueprint timeline tracks round-trip through BPIR decompile/compile.
- Added: `editor.undo_history`, and `steps` on `editor.undo` / `editor.redo`.
- Added: `blueprint.graph.connect_pins_batch`.
- Added: `niagara.list_stack_issues` and `niagara.apply_issue_fix` — list the Niagara editor stack's
  issues with the engine's one-click fixes, and apply one by id with a bounded recompile and a
  before/after issue set.
- Added: `actors[]` batches on `actor.set_transform` / `actor.nudge`, and `lookAt` / `roll` on
  `actor.set_transform`.
- Added: `gameplay_tags.find_referencers`.
- Added: `animation.retarget_animations`, an IK Retargeter batch export of AnimSequences that
  verifies every output and deletes them all on a partial result.
- Added: `animation.authoring.get_curve_keys`, `set_curve_keys`, `remove_curve_key`,
  `remove_curve` and `rename_curve`.
- Added: `log_file` in `system.identity`.
- Added: the `call` tool accepts `args` as a JSON string.
- Added: editor background CPU throttling is disabled while an agent is active (project setting
  `bDisableBackgroundThrottleWhileAgentActive`, default on); `performance.run_benchmark` reports
  the throttle state it measured under (`backgroundThrottle`).
- Added: console verbs refuse lines that bypass a typed verb; `force: true` overrides.
- Changed: `niagara.add_module` refuses a module whose usage bitmask does not allow the target
  stack with `INCOMPATIBLE_STACK_GROUP` (it used to add it anywhere); with `scriptUsage` omitted it
  picks the only allowed stack when there is exactly one.
- Added: `niagara.simulate` runs a Niagara system in a private preview world for a bounded number
  of fixed steps and reports each emitter's live particle count, so `emitted` is measured rather
  than inferred from the graph (CPU and GPU emitters; an unreadable GPU count is explained, never
  reported as 0).
- Changed: `asset.get_material_stats` compiles the material (bounded) and returns the measured
  `stats.vertexInstructions` / `pixelInstructions` / `samplers` and the other engine shader
  statistics, plus `statsPlatform`, `measuredSubject` and `textureSampleNodeCount`; `stats` is
  `null` with `statsUnavailableReason` under PIE, without a renderer, or when the compile did not
  complete. The never-populated `stats.instructionCount` is removed.
- Changed: `editor.undo` / `editor.redo` with nothing to do return `NOTHING_TO_UNDO` /
  `NOTHING_TO_REDO` errors instead of success with `success: false`.
- Changed: `gameplay_tags.remove` returns `TAG_IN_USE` for a referenced tag and `REMOVE_FAILED`
  when the engine refuses, instead of success with `removed: false`.
- Changed: `actor.set_transform` / `actor.nudge` with no actor return `INVALID_ARGUMENT` instead of
  `MISSING_REQUIRED_PARAM`.
- Changed: `blueprint.graph.connect_pins` `PIN_NOT_FOUND` carries `sourcePinLookup` /
  `targetPinLookup` in its error data.
- Changed: the `bpir.txt` asset-dump format version is 10 (was 9), so cached BPIR dumps regenerate.
- Added: proxy tool `editor_list`: every Unreal editor process on the machine (any checkout,
  engine, commandlet or `-game` run) with its project, mode, log, gateway port and launch reason.
- Added: proxy tools `editor_run_tests` (launches the suite detached and capped, returns once the
  first test starts) and `editor_test_status` (non-blocking progress and `check_suite_log` verdict
  by log path).
- Added: proxy tools `editor_build` (builds this project's `<Project>Editor` Development target
  detached and capped; refuses while an editor of this checkout runs) and `editor_build_status`
  (non-blocking state, UBT result and compiler/linker error lines from the log).
- Added: offscreen and headless editors, test runs and builds run under an internal capped,
  detached supervisor (`Content/Python/pinwright_supervisor.py`, no command line of its own);
  Linux caps through `systemd-run --user --scope` when available.
- Added: `launch_reason` and `launched_by` in `system.identity`.
- Changed: `editor_start` / `editor_restart` / `editor_run_tests` require `mode` (`visible`,
  `offscreen` or `headless`; no default) and `reason`; the optional `visible` boolean is removed.
  `headless` is new: `-NullRHI`, no window, no display needed. Every PinWright launch passes
  `-PinWrightLaunchReason` / `-PinWrightLaunchedBy` to the editor.
- Changed: `editor_start` always spawns the resolved editor directly and detached (the OS
  `.uproject` association launch is gone); `offscreen` and `headless` editors run under the capped
  supervisor.
- Removed: proxy tool `editor_prepare_tests` (use `editor_run_tests`), and
  `scripts/Run-Capped.ps1`, `scripts/Run-SuiteCapped.ps1`, `scripts/CappedJob.ps1` (use
  `editor_build`, `editor_run_tests` and `editor_start`).
- Fixed: `blueprint.graph.get_node_details` / `get_node_details_batch` (and `get_graph_details`
  with `includeNodeDetails`) carry `linkedTo` on connected pins, so a wired input no longer
  reads as its unused default.
- Fixed: `blueprint.graph.replace_node` moves a wired `self` pin (an external-owner accessor's
  target) to the replacement, and lists it in `connectionsDropped` when it cannot move.
- Fixed: `geometry.sweep` and `geometry.extrude_along_spline` convert the spline frames into the
  target's local space, so the tube follows the spline when the target actor is not at the
  identity transform instead of landing offset by the target's own transform.
- Fixed: BPIR `call_dispatcher` / `bind_dispatcher` / `unbind_dispatcher` / `clear_dispatcher`
  accept a `K2Node_ConvertAsset` (Resolve Soft Reference) output as `Target:`, the shape the
  decompiler prints, instead of failing with "Failed to create delegate node for dispatcher".
- Fixed: `blueprint.decompile` no longer prints a self-bound dispatcher delegate twice (as
  `event: @Fn` and as an extra `%n = call Create_Event(...)` line the compiler rejects), so a
  decompiled `bind_dispatcher` / `unbind_dispatcher` body recompiles.
- Fixed: `skeleton.remove_bone` and `skeleton.set_bone_parent` keep each bone's translation
  retargeting mode on that bone (they used to leave the modes on the old indices) and refresh the
  engine's dependent caches; both report `boundMeshes` with whether each bound mesh still matches.
- Fixed: `check_suite_log` (and `editor_test_status`) no longer classifies a run `CRASHED` on
  another editor's crash report: a report counts only between the log's open time and its last
  write, only when the `-Abslog` its command line records (if any) is this log, and only when its
  process id matches the run's editor (`--pid`, or the capped supervisor's
  `<log>.supervisor.log`), when both are known.
- Fixed: on Windows the capped supervisor starts through WMI `Win32_Process.Create`, outside the
  MCP client's process tree and job, so a test run, build or editor (visible ones included,
  uncapped) survives the MCP client exiting. Results carry `detached`, `launchMechanism` and `detachNote`; a launch that
  could not detach says `NOT DETACHED` in its text.
- Added: an MCP server started before the plugin's Python changed now fails a supervised launch
  with `SUPERVISOR_VERSION_MISMATCH` (restart the MCP server) instead of an unexplained
  `exited without a handoff`.
- Fixed: `object.call_function` on a PIE object no longer runs every RPC locally: a client's
  `Server` RPC is sent to the server instead of running its `_Implementation` on the client (which
  recursed into a stack-overflow crash for RPCs that re-send themselves). `python.execute` under PIE
  warns that the Python plugin still runs RPCs locally.

## 0.8.0

- First open-source release, under the MIT License.
- Linux editor support alongside Windows (UE 5.3-5.8).
- Demo edition removed, together with its daily request cap.
- GitHub Actions removed in favour of local verification (`scripts/Run-SuiteCapped.ps1` plus
  `Content/Python/check_suite_log.py`).
