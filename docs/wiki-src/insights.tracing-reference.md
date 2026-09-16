# `insights.tracing-reference`

Reference for configuring an Unreal Insights capture from an agent: the trace-channel catalog, the `Trace.*` console commands, launch args, and capture recipes by diagnostic goal. Drive channels at runtime with `call("insights.set_channels", {...})` and one-off commands with `call("editor.console_command", {...})`; for the analysis half see [`insights.export_trace`](insights.export_trace.md) and the "Revealing untraced CPU time" playbook on the [`insights`](insights.md) page.

## How channels work (read first)

- **Enable-name = the C++ channel symbol minus the `Channel` suffix, case-insensitive** (the engine strips it in `TraceAuxiliary.cpp`). `CpuChannel`→`cpu`, `MemAllocChannel`→`memalloc`, `StackSamplingChannel`→`stacksampling`. A symbol that doesn't end in `Channel` keeps its full name (`ObjectProperties`).
- **Everything is OFF by default.** At init the engine disables all channels; only the `default` preset (auto-applied in the editor), a `-trace=` list, or a runtime `Trace.Enable` turns any on.
- **Read-only channels are launch-only.** `memalloc`, `memtag`, `callstack`, `module` are flagged read-only; `Trace.Enable`/`insights.set_channels` **cannot** turn them on mid-session (they log `Channel '…' is read only`). They must be on the launch command line (`-trace=memory`).
- **Presets** (`TraceAuxiliary.cpp:176`): `default` = `cpu,gpu,frame,log,bookmark,screenshot,region`; `memory` = `memtag,memalloc,callstack,module`; `memory_light` = `memtag,memalloc`. `call("insights.start_session")`'s `channels` arg accepts preset names as well as bare channels.

## Channel catalog (most useful)

| Channel | Captures | Default-on | Notes |
|---|---|---|---|
| `cpu` | Named CPU timer scopes — the bulk of Timing Insights | yes | Carries `TRACE_CPUPROFILER_EVENT_SCOPE` + named-events. High volume. |
| `gpu` | Named GPU timers (one `gpu` channel in 5.7; no `gpu2`) | yes | Needs `r.GPUStatsEnabled 1` + timestamp-capable HW for per-pass durations. |
| `frame` | Game/Render frame boundaries | yes | Minimal; works in shipping. |
| `bookmark` | Instant named markers | yes | `Trace.Bookmark`. |
| `region` | Named timeline spans | yes | `Trace.RegionBegin/End`. |
| `log` | UE_LOG messages | yes | Low. |
| `stats` | Stats-system counter values over time | no | Counter values, not CPU scopes. |
| `counters` | `TRACE_COUNTER_*` int/float series | no | Low–med. |
| `task` | Task Graph scheduling | no | Med. Useful to see what a `WaitForTasks` is waiting on. |
| `net` | Networking events | no | Med. |
| `loadtime` / `assetloadtime` | Asset/package load timing | no | Med. |
| `file` / `iostore` | Platform file / pak I/O | no | Med. |
| `slate` | Slate UI Insights | no | Med. |
| `object` / `objectproperties` | UObject worlds/instances/events; per-property values | no | Med–high; `objectproperties` is heavy. |
| `animation` | Animation Insights / Rewind Debugger | no | Med. |
| `contextswitch` | OS thread scheduling (which core, when preempted) | no | ETW, **admin required** on Windows. |
| `stacksampling` | Periodic call-stack samples (catches unscoped code) | no | ETW, **admin required**; 8 kHz default. |
| `memalloc` | Every allocation (+ LLM tag scopes) | no | **Read-only / launch-only**; very heavy. |
| `memtag` | Per-LLM-tag memory snapshots | no | **Read-only / launch-only**; auto-enables LLM. |
| `callstack` / `module` | Alloc callstacks + symbol modules | no | **Read-only / launch-only**; needed to attribute leaks. |
| `rhicommands` / `rendercommands` / `rdg` | RHI / render-thread / Render-Dependency-Graph timers | no | High; render-thread deep dives. |

Run `call("editor.console_command", {"command": "Trace.Status"})` to print the full enabled + available channel list for the running build.

## Control commands (`Trace.*`)

All via `call("editor.console_command", {"command": "..."})`.

| Command | Effect | Plugin wrapper |
|---|---|---|
| `Trace.File [path] [channels]` | **Canonical** start-to-`.utrace`. First arg is a path if it contains `/ \ . :`, else a channel set. | — |
| `Trace.Send <host[:port]> [channels]` | Stream to a trace store / UnrealTraceServer. | — |
| `Trace.Stop` | Stop the active trace. | `insights.stop_session` (calls `FTraceAuxiliary::Stop` directly) |
| `Trace.Pause` / `Trace.Resume` | Snapshot-disable / re-enable the current channel set. | — |
| `Trace.Enable [channels]` | Enable channels mid-run (read-only channels rejected). | `insights.set_channels` (`enable`) |
| `Trace.Disable [channels]` | Disable channels — **empty arg disables ALL**; always pass an explicit list. | `insights.set_channels` (`disable`) |
| `Trace.Status` | Print connection, memory, enabled + available channels. | partly `insights.get_trace_path` |
| `Trace.SnapshotFile [path]` | Write the in-memory tail buffer to `.utrace` without stopping. | `insights.snapshot` (`FTraceAuxiliary::WriteSnapshot`) |
| `Trace.SnapshotSend <host> <port>` | Send the tail buffer to a store. | — |
| `Trace.Bookmark [name]` | Emit an instant marker. | — |
| `Trace.RegionBegin [name]` / `Trace.RegionEnd [name]` | Open / close a named region. | — |

Note: `insights.start_session` issues the **deprecated** `Trace.Start` (forwards to `Trace.File`, logs a warning each call) — functionally fine. There is no bare `Trace.Snapshot` and no `Trace.SendTo` in 5.7; use the names above.

## Launch args (relaunch-only)

- `-trace=<channels>` — channels/presets to enable at startup (bare `-trace` = `default`).
- `-tracefile[=path]` — trace to a `.utrace` file; `-tracehost[=ip[:port]]` — trace to a store.
- `-trace=memory -llm` — the only way to get allocation/LLM memory tracing (the channels are read-only).
- `-statnamedevents` / `-verbosenamedevents` — promote `SCOPE_CYCLE_COUNTER` scopes onto the `cpu` channel (see the untraced-CPU playbook on the `insights` page; prefer the runtime `stats.NamedEvents 1`).
- `-trace=default,stacksampling,contextswitch` + `-samplinginterval=<µs>` (default 125 µs) — sampling profiler (Windows ETW, **admin required**).
- `-tracetailmb=<MB>` — size of the snapshot tail buffer (editor default 32 MB; `insights.snapshot` is bounded by this).
- `-traceautostart=0` — stage channels but wait for a console `Trace.*` command to begin.

## Capture recipes by goal

**Memory (allocations / leaks / LLM) — launch-only**
`UnrealEditor.exe <Project> -trace=memory -llm` (or `-trace=memory_light` to skip callstacks — much cheaper). The `memory` channels can't be enabled at runtime. Reveals live/total allocations over time, per-LLM-tag breakdown, and callstack-attributed leak sites. Heavy. Open in Insights → Memory Insights.

**GPU timing — runtime**
The `gpu` channel is in `default`. Ensure timestamps: `call("editor.console_command", {"command": "r.GPUStatsEnabled 1"})` (also needs timestamp-capable HW). `r.RHISetGPUCaptureOptions 1` makes pass/draw-event names readable. `stat gpu` shows the same per-pass timing on screen. (`r.GPUCsvProfilerEnable` does **not** exist in 5.7; for CSV GPU columns use the `-csvGpuStats` launch param.)

**CSV Profiler (per-frame budgets → .csv) — runtime, orthogonal to Insights**
`call("editor.console_command", {"command": "CsvProfile START"})` … `CsvProfile STOP` (args are uppercase; `CsvProfile FRAMES=600` for a bounded capture). Writes one row per frame to `<Project>/Saved/Profiling/CSV/`. Ideal for **A/B regression diffs** — diff two runs frame-by-frame. Cheap enough to run continuously. Launch forms: `-csvCaptureFrames=N`, `-csvGpuStats`, `-csvNamedEvents`.

**Regions & bookmarks (annotate the trace) — runtime, recommended for repros**
Bracket the action you want to isolate so the window is trivial to find later:
`call("editor.console_command", {"command": "Trace.RegionBegin fly_forward"})` → reproduce → `Trace.RegionEnd fly_forward`. `Trace.Bookmark <name>` drops an instant marker. Both channels are in `default`. Negligible cost. **Use this to bracket a resim repro** instead of hunting timestamps afterward.

**Chaos Visual Debugger (physics solver) — runtime, key for physics-resim bugs**
Records per-frame solver state to a `.utrace`, including a per-frame **`IsReSimulated`** flag — this is how you *see* resimulation (which frames re-run, how many bodies/contacts/substeps).
- Record: `call("editor.console_command", {"command": "p.Chaos.StartVDRecording"})` … `p.Chaos.StopVDRecording` (file mode by default; `p.Chaos.StartVDRecording Server 127.0.0.1` for live). Editor: Tools ▸ Debug ▸ **Chaos Visual Debugger** has a Record button.
- Master toggle: `p.Chaos.VisualDebuggerEnable` (default 1). Tuning cvars use the `p.Chaos.VD.*` prefix (e.g. `p.Chaos.VD.TimeBetweenFullCaptures`) — **not** `p.Chaos.VisualDebugger.*`. The `ChaosVDChannel` is managed by these commands, not by `insights.set_channels`. Needs a Development/Test build. Heavy.
- **Resim recipe:** launch `-trace=default`, `p.Chaos.StartVDRecording`, bracket with `Trace.RegionBegin/End`, reproduce, stop. Cross-read the `cpu` track (where resim cost shows as game-thread time) against ChaosVD frames flagged `IsReSimulated`.

## See also

- [`insights`](insights.md) — the namespace overview and the "Revealing untraced CPU time" playbook (named events → stack sampling → context switches).
- [`insights.stat-companions`](insights.stat-companions.md) — runtime `stat` commands that triage a frame without recording a trace.
- [`performance`](performance.md) — typed wrappers for `stat` overlays, scalability, and CVar tuning.
