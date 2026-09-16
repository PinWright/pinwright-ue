# system

Process- and engine-level controls: UBT, automation tests, GEngine console commands and search, and
the long-running job queue. Methods live at `system.*`; read-only inspection is
`call("system.inspect")`, and headless operation is `call("unattended")`.

## Cross-cluster overlap

- **`system.console_command` ↔ `editor.console_command`** — both run commands. Use `system.*` for
  project/engine cvars and process commands; use `editor.*` for editor-world/viewport commands.
  Discover unfamiliar names with `system.console.search`.
- **`system.inspect.*` ↔ `actor.*` / `level.*` reads** — inspection is read-only; use it when no
  mutation is intended.
- **Subsystem `UFunction` invocation** — resolve a subsystem with
  `call("system.inspect.list_subsystems")`, inspect its surface with
  `call("system.inspect.inspect_class")`, then invoke it through `call("object.call_function")`.
  The latter builds a real parameter frame and serializes return values and out-params.

## Long-running jobs

Long-running handlers (`system.run_tests`, `level.build_lighting`, `asset.dump_folder`,
`editor.screenshot`, ...) use tickets. A streaming request with `progressToken` and
`Accept: text/event-stream` defaults to block-and-stream: the completed result arrives with SSE
progress. `args: {wait: false}` returns the ticket immediately; plain-JSON clients always get it
immediately. Observe completion in `Saved/PinWright/jobs.jsonl` or via `system.job_status`.

**Immediate response shape**

```json
{
  "result": {
    "status": "running",
    "ticket_id": "j_20260426T123456_a1b2c3d4",
    "monitor_path": "Saved/PinWright/jobs.jsonl",
    "method": "<method-name>",
    "started_at": "2026-04-26T12:34:56Z",
    "docs": "call(\"system.job_status\") for guidance"
  }
}
```

Handlers may add method-specific fields (for example, `asset.dump_folder` adds `rootDir`,
`folderPath`, `assetCount`). `docs` is always emitted and points to `### system.job_status` for
polling guidance.

**JSONL line schema**

One JSON object per line in `Saved/PinWright/jobs.jsonl`. Common fields are `ts`, `ticket_id`,
`method`, and `event`; events are `started`, `progress`, `completed`, `failed`, and `cancelled`.
Each line flushes synchronously, including the terminal event.

**There is no `status` field on a JSONL line.** `status` exists only on the kickoff envelope above and in the `system.job_status` reply; the JSONL says `event`. Match `"event":"completed"` — a waiter matching `"status":"completed"` against this file never fires.

```json
{"ts":"2026-04-26T12:34:56.123Z","ticket_id":"j_...","method":"system.run_tests","event":"started","params":{...}}
{"ts":"2026-04-26T12:35:42.001Z","ticket_id":"j_...","method":"system.run_tests","event":"progress","message":"42/100 tests"}
{"ts":"2026-04-26T12:38:11.901Z","ticket_id":"j_...","method":"system.run_tests","event":"completed","result":{...}}
```

**Settings**

`Project Settings > Plugins > PinWright > Jobs`:

- `Completed ticket TTL (seconds)` — how long completed tickets remain queryable (default 3600).
- `Monitor file max bytes` — JSONL rotation threshold (default 10 MB).
- `Monitor file rotations to keep` — keeps `jobs.jsonl.1` ... `jobs.jsonl.N` (default 3).
- `Min progress event interval (ms)` — progress-event cap per ticket (default 1000). It limits
  `jobs.jsonl` growth only; live SSE bypasses it. The old 60000 default emitted one event per
  minute, indistinguishable from a wedged editor.

`Project Settings > Plugins > PinWright > HTTP`:

- `Http Response Spill Threshold Characters` — responses larger than this serialized character
  count are written to `Saved/PinWright/HttpResponses/<startup-datetime>/` and replaced with a
  small `outputTooLong` file reference (default 10000). Recent spill directories survive restarts;
  directories older than 24 hours are pruned at startup.

**Lifecycle**

The JSONL file and rotation files are wiped at editor startup. The ticket registry is in-memory
only, so cross-session lines would reference dangling IDs; every session starts with a clean log.

**Methods that use the ticket pattern**

| Method | Bound to |
|---|---|
| `system.run_tests` | `IAutomationControllerManager::OnTestsComplete`, or a bounded child process per isolated group |
| `system.run_ubt` | poll proc handle |
| `level.build_lighting` | `FEditorDelegates::OnLightingBuildSucceeded/Failed/Kept` |
| `lighting.build_lighting` | same lighting delegates |
| `level.build_navigation`, `navigation.rebuild_navigation` | poll `UNavigationSystemV1::IsNavigationBuildInProgress` |
| `level.build_all` | lighting + nav fan-in |
| `editor.screenshot` | Synchronous `CaptureGameViewportToPngFile` or `CaptureActiveLevelViewportToScreenshot` inside the tracked job; completes inline |
| `level.save`, `level.save_as` | wraps engine save in `AsyncTask`, completes on return |
| `render.nanite_rebuild_mesh` | retains the mesh after `Build`, then polls `IsCompiling` with a bounded 120-second core ticker; final drain/save/probe runs after settle and persistence is on the terminal result |
| `blueprint.build_api_index` | wraps iterator in `AsyncTask` |
| `performance.optimize_shaders` | poll `GShaderCompilingManager->IsCompiling` |
| `performance.run_benchmark` | duration-driven `FTSTicker` |
| `asset.dump_folder` | existing per-tick `FTSTicker`; reports completion via registry |
| `pcg.generate` | `UPCGComponent::OnPCGGraphGenerated/CancelledDelegate` plus an `FTSTicker` watchdog if no broadcast arrives |

The completion-binding column is implementation detail: these methods return the running-ticket shape
when the request does not block-and-stream, and ticket flows require `system.job_status`.

## Console command discovery

`system.console.search` searches the live `IConsoleManager` registry before
`system.console_command` or `editor.console_command`. It covers names such as `r.*`, `p.*`, `ai.*`,
`net.*`, `t.*`, `Slate.*`, `Stat*`, `showflag.*`, `wp.*`, and plugin prefixes.

**Matches are console-object NAMES only: one token, no embedded spaces.** Search is a substring
match over registered `IConsoleObject` names, not multi-word command lines. For `Stat <group>`,
`Show <flag>`, or another multi-word command, search the **bare leading token** (`Stat`, not
`stat unit`; `Show`, not `show collision`) and scan its rows. A full `stat unit` query returns zero
matches even though `system.console_command "stat unit"` runs. Pure `Exec` commands and stat-group
subcommands may not be registry objects at all. A zero-result bare-token search means not
registry-enumerable; a zero-result whitespace query means only that the exact string is not an
object name, not that the command is invalid.

Example:

```json
call("system.console.search", {
  "query": "ScreenPercentage",
  "kind": "variable",
  "limit": 20
})
```

The response contains `results`, `totalMatches`, and `truncated`. Each result has `name`, `kind`
(`variable` or `command`), `help`, `flags`, and variable `currentValue`. Matching is case-insensitive
and sorts exact, prefix, then other substring matches.

A broad query such as `ScreenPercentage` can spill multi-line `help` text beyond the inline budget.
Keep discovery inline with `namesOnly: true`, an explicit `fields` allow-list, a narrower query, or
a smaller `limit`; `totalMatches` / `truncated` still report untruncated totals.

## Console commands run at a safe point

`system.console_command` and `editor.console_command` accept arbitrary payloads: `open <map>` tears
down the world, `obj gc` collects, and `viewmode` redraws. Both always use the plugin safe point; a
call arriving inside `UWorld::Tick` is re-queued on the editor core ticker.

Cost is at most one 0.1 s pass, only while a world ticks. Response, ticket, and error paths are
unchanged. `open <map>` can no longer trip `Assertion failed: !LevelList.Contains(TickTaskLevel)`,
the same crash gated by `level.load`.

This does not make commands safe and refuses none. A long synchronous operation still holds the game
thread with no progress or cancel—the `call("python")` freeze through another verb—and shutdown
commands still shut down. Prefer `level.load` over `open`, `editor.quit` (refuses unsaved work) over
`quit`, and `editor.set_view_mode` over `viewmode`. A success means the line was *consumed*, not that
its effect succeeded; search unfamiliar names first.

## Modal dialogs

Every RPC runs on the game thread. A modal dialog owns it in a nested Slate loop, so no RPC can
dismiss it; the readiness ticker, completion-timeout sweep, and deferred dispatch queue also stop.
PinWright suppresses engine dialogs during each handler (`Editor Preferences > Plugins > PinWright >
Unattended`) and auto-declines the boot-time "Restore Packages" prompt, which appears before the
ticker first runs.

If one gets through, the socket I/O thread reports it. `ping` returns `error: "EDITOR_BLOCKED_ON_MODAL"`
with `retryable: false`, `blockedOnModal: true`, `blockedSeconds`, and readable `modalTitle`; every
`tools/call` fails immediately with the same code. **`retryable: false` is the contract**: unlike
`EDITOR_NOT_READY`, polling cannot clear it. A human must dismiss the dialog or kill the process;
requests already in flight remain stuck.

A handler that never returns is more common than a modal. Past 90 s without a liveness heartbeat,
`ping` returns `error: "EDITOR_GAME_THREAD_STALLED"` with `retryable: true`, `stalledSeconds`, and
`inFlightMethod` / `inFlightRequestId`. It does not gate `tools/call`; the work may finish. PinWright
cannot interrupt a wedged game thread, so this is diagnosis, not recovery.

The common trigger is a `python.execute` loop; one incident held an editor for 168+ minutes.
`call("python")` has the detail and the typed C++ RPCs that avoid doing those sweeps on the game thread.

Suppression is not correctness: `FMessageDialog` prompts use the engine's documented default, but a
raw Slate modal is *cancelled* and `ShowModal()` returns an uninitialised widget-local value. Re-read
state after a mutating verb (delete, rename, duplicate) that could have hit one.

Full contract, launch flags, and the residual cases that still need a human: `call("unattended")`.

## Which editor am I talking to

The MCP port is derived from the **project path**, not from the editor instance
(`19880 + hash(projectPath) % 10240`). Two editors opened on the same project therefore compute the
same port, exactly one of them wins the bind, and every RPC on that port reaches that one — so a
client that launched its own editor can be driving a different, already-running editor.

`system.identity` is the handshake. It reports what was **measured** from the answering process:
`pid`, a per-boot `instance_id`, `project_file`, `project_name`, `engine_version`, a `plugin_build`
compile stamp, `executable_path`, `command_line`, and the port it is actually serving on
(`bound_http_port`, omitted when nothing is bound) beside the port it was configured for
(`configured_http_port`).

Pin it by putting the reserved `_expect_editor` key **inside `args`**, the same place `_format` and
`wait` live. The dispatcher consumes and strips it, so it is never a handler parameter and works on
every verb:

```json
{"method": "blueprint.set_default",
 "args": {"...": "...", "_expect_editor": {"pid": 12345, "instance_id": "…"}}}
```

A request whose assertion does not match is refused with `EDITOR_IDENTITY_MISMATCH` **before the
handler runs** — no mutation lands — and the payload carries `expected` and `actual` for exactly the
fields asserted. Assertable fields: `pid`, `instance_id`, `project_file`, `project_name`,
`engine_version`, `plugin_build`. An assertion that is empty, or that names a field outside that
list, is refused rather than partially applied: an ignored assertion reads to the caller as a passed
one. Asserting is opt-in — a call with no `_expect_editor` behaves exactly as it always has.

`pid` is the field to pin first, because it is externally observable without a prior handshake;
`instance_id` is what separates two editors after a pid is recycled.

## See also

- [`mcp-transport`](mcp-transport.md) — the wire protocol, job-ticket shape, and SSE streaming that the long-running verbs above ride on.
- `call("unattended")` — modal suppression, launch flags, and `EDITOR_BLOCKED_ON_MODAL` for sessions with no human present.
- `docs/arch.md` in the plugin folder — subsystem readiness, request flow, and handler dispatch boundaries. A maintainer document shipped beside the plugin, not a wiki page.

### system.run_tests

Run UE automation tests as a long-running job. Use one selection mode:

- `filter: "PinWright"` — legacy broad substring/filter selection through UE's console automation command.
- `test: "PinWright.system.run_tests.ValidParamsNoCrash"` — one exact full test name.
- `tests: ["Test.Full.Name.A", "Test.Full.Name.B"]` — multiple exact full test names.

`filter` is mutually exclusive with `test` and `tests`. Exact-name runs return `requestedTests`,
`resolvedTests`, and `missingTests`; if none resolve, the job fails deterministically instead of
waiting for UE's completion delegate.

Only one `system.run_tests` job may own the process-wide UE automation controller at a time. A
second request is refused with `AUTOMATION_RUN_IN_PROGRESS` until the current ticket is terminal;
it never changes the active filter or shares that run's completion result.

For a combined filter, pass `isolateGroups: true` to split its `+`-separated groups and run them
sequentially in separate `UnrealEditor-Cmd` processes. This mode is filter-only. Its terminal result lists
each group, child exit code, retained log path, counts, and log verdict. A child is successful only
when its log ends with UE's real `Automation Test Queue Empty <N> tests performed` marker after the
last test result, the found/started/finished/performed counts reconcile, and no
`PINWRIGHT_ASSERTIONS_SKIPPED` marker occurred; a command-line echo of
`-TestExit="Automation Test Queue Empty"` is not a marker. Missing or stale markers fail the job
with `TEST_RUN_INCOMPLETE`, even when the log contains zero failed tests. Isolated children do not
publish PinWright HTTP transport, regenerate the shared wiki, or touch the host editor's job monitor;
each child receives a private monitor file beside its automation log and runs with UE's
multi-process config-write suppression.

Each isolated child has a wall-clock budget set by `childTimeoutSeconds` (default 3600 seconds,
clamped to 1-14400). On expiry, PinWright terminates that child's owned process tree, releases the
parent job's automation lease, and fails with `TEST_RUN_TIMEOUT`; the result carries `timedOut: true`,
`timedOutGroup`, and the one-based `timedOutGroupIndex`. `system.job_cancel` is supported for an
isolated-groups ticket and terminates the active child before releasing the lease. The default remains
in-process because the recorded host crash has no stable reproduction; making isolation implicit would
add an editor startup and a distinct process environment to every group without evidence that every run
needs that cost. Exact-name, RunAll, and ordinary filter jobs remain non-cancellable.

### system.job_status

Returns a ticket's `status` (`running` / `completed` / `failed` / `cancelled`), original
`started_at`, latest `progress[]`, and terminal `result` or `error`.

Every async-returning RPC's initial ticket includes `docs: call("system.job_status") for guidance`.

**Polling cadence.** Poll sub-minute jobs once or twice in-session; avoid tight loops for multi-minute
jobs (full asset dumps, full test suite, lighting builds, full-project nav rebuilds). Resume with an
MCP client, scheduler, or polling automation when the ticket is terminal.

**Before arming any waiter, check the status once.** Many verbs finish in seconds (a 1890-asset `asset.dump_folder` sweep takes ~39 s), and the cheapest correct wait is not to wait: on a streaming client, leaving `wait` absent blocks and returns the finished result. Reach for a waiter only after `wait:false` on a job that is genuinely still `running`.

**Waiting on the JSONL — use `event`, not `status`.** This RPC's reply carries `status`; the `jobs.jsonl` stream does not. Terminal lines are `"event":"completed"` / `"failed"` / `"cancelled"`, and each carries `ticket_id` on the same line, so a single per-line match is correct:

```sh
LOG="<Project>/Saved/PinWright/jobs.jsonl"; T="j_..."
until grep "$T" "$LOG" 2>/dev/null | grep -Eq '"event":"(completed|failed|cancelled)"'; do sleep 5; done
grep "$T" "$LOG" | grep -E '"event":"(completed|failed|cancelled)"' | tail -1
```

A loop matching `"status":"completed"` against the JSONL never fires — the field does not exist there — and silently waits forever on a job that already finished. Terminal events are emitted unconditionally and flushed synchronously; only `progress` events are rate-limited (`Min progress event interval (ms)`, default 60000), so a sub-minute job logs `started`, one `progress`, and the terminal event, and progress lines may never advance. The log is also wiped at editor startup, so a waiter that outlives an editor restart will never see its ticket again.

**Blocking-call caveat.** A blocking stream refreshes its request deadline only on accepted progress frames; heartbeats deliberately do not. Raising `Min progress event interval (ms)` (default 60000) above the HTTP request timeout (`Http Default Timeout Ms`, default 120000) therefore leaves nothing to refresh the deadline, and the blocking call is reaped with `TIMEOUT` while the job keeps running. The ticket stays authoritative — poll it.

The complementary RPCs are `system.job_list` (active + recently-completed tickets) and `system.job_cancel` (only handlers with a registered cancellation callback are cancellable; the rest return `JOB_CANCEL_UNSUPPORTED` rather than claim a cancel that did not happen — see below).

### system.job_list

Returns active and recently completed tickets within `Completed ticket TTL` (default 3600 seconds).
Entries carry `ticket_id`, `method`, `status`, `started_at`, latest `progress[]`, and terminal
`result` / `error`. Use it to recover a ticket missed in the original response or confirm eviction.

### system.job_cancel

Cancels a `running` ticket by `ticket_id`. **Most ticketed verbs cannot be cancelled, and this RPC
says so instead of reporting a cancellation that did not happen.**

Cancellation works only where the handler registered a callback: `asset.dump`, `asset.dump_folder`,
`localization.gather`, `localization.compile`, `pcg.generate` (UE 5.4+), and
`render.nanite_rebuild_mesh`; `system.run_tests` also registers one when `isolateGroups:true`.
Other ticketed verbs—level/lighting builds, `system.run_ubt`, in-process `system.run_tests`,
`mrq.run_jobs`, `performance.*`, `blueprint.build_api_index`, `editor.screenshot`,
`level.save` / `save_as`, and `navigation.rebuild_navigation`—cannot stop their work.

Four outcomes, and only the first is a success:

| Outcome | Response |
| --- | --- |
| Cancel hook invoked | `{cancelled: true, cancellation: "requested", ticket_id}`; ticket → `cancelled` |
| Verb has no cancel hook | error `JOB_CANCEL_UNSUPPORTED`; **ticket stays `running`** |
| Ticket already terminal | error `JOB_NOT_RUNNING` |
| No such ticket (or evicted) | error `TICKET_NOT_FOUND` |

`JOB_CANCEL_UNSUPPORTED` means the job remains `running` and will finish, still writing output. Poll
`system.job_status`; the ticket stays `running` so its eventual real result is not discarded.

`cancellation: "requested"` means the hook ran, not that work stopped. Poll `system.job_status` for
terminal `cancelled` (or `completed` / `failed` if cancellation arrived too late).

### system.console_command

Runs a console line at process / `GEngine` scope. Use it for project/engine cvars; use
`editor.console_command` for editor-world, viewport, and PIE-world commands. It tries a resolved
editor world, then falls back across every live world context.

**A success means the line was consumed, not that its effect succeeded.** `GEditor->Exec` returning
true means an exec handler or cvar claimed it; read the effect back with a typed verb. An
unrecognized line errors `EXEC_FAILED`, usually a typo or unloaded-module command. Search first.

This verb uses the plugin safe point, so `open <map>` and other world-teardown commands cannot begin
inside `UWorld::Tick`. See "Console commands run at a safe point" in `call("system")` for limits.
Prefer typed `level.load` for map swaps and `editor.quit` for shutdown; both report a real verdict,
and `editor.quit` refuses to discard unsaved work.

**A line that SETS a scalability CVar is refused** with `SCALABILITY_CVAR_USE_TYPED_VERB`. That is
either an `sg.*` group or **any** CVar declared with `ECVF_Scalability` / `ECVF_ScalabilityGroup` —
`r.ViewDistanceScale`, `r.Streaming.PoolSize`, `r.ScreenPercentage`, `r.MaxAnisotropy`,
`r.Shadow.Virtual.MaxPhysicalPages` and the rest of a performance pass's vocabulary. A scalability
group is not a special kind of variable: it is a name for a list of ordinary CVars in a
`[<Group>@N]` ini section, and the console pins a member exactly as it pins the group. The set
lands at `ECVF_SetByConsole`, the highest CVar priority, which outranks the
`ECVF_SetByScalability` priority the editor's own *Settings → Engine Scalability Settings* panel
writes at — for the rest of the session, so every later change the user makes to the owning group
is silently discarded until the editor restarts.

Use `performance.set_scalability`, which drives the groups through
`Scalability::SetQualityLevels` at the panel's own priority — re-applying every member CVar of the
groups it touches — and cannot create the pin. `force: true` runs the line anyway and accepts the
pin.

**Three shapes are deliberately NOT refused**, because the engine performs no `Set` on any of
them: **reading** a scalability CVar (the name with no value, or with a bare `?`); the aggregate
`scalability N`, which routes through `SetQualityLevels` at the panel's own priority; and a first
token that resolves to no console object at all — a typo, or an `Exec` command owned by a module.
The rule tests the FLAG on the resolved console object, not the spelling of the name, which is why
`r.ScreenPercentage` and `r.VSync` are covered despite having no `BaseScalability.ini` row.
`system.console.search` reports `Scalability` / `ScalabilityGroup` in each row's `flags`, so you
can tell in advance which lines will be refused.

**The flag is the engine's own classification, and it is broader than "a quality slider".** Some
one-shot levers carry it too — `r.LumenScene.SurfaceCache.Reset`, for one — and are refused
alongside the quality CVars, because a console set pins them just the same. `force: true` is the
intended answer for those: you are accepting a pin on a CVar nobody drives from the panel.
