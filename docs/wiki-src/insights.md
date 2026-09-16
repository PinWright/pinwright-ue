# `insights`

Drive Unreal Insights capture from the editor — start/stop a `.utrace` session, mutate channels, snapshot the ring buffer, and export flat CSV tables. Use `call("performance")` for live scalability/CVar tuning and stat snapshots; use this namespace for recorded traces and offline timing breakdowns.

## Capture & analysis guides

Configuring a capture and reading the result:

- [`insights.tracing-reference`](insights.tracing-reference.md) — trace-channel catalog, `Trace.*` console commands, launch args, and capture recipes by goal (memory, GPU, CSV profiler, regions/bookmarks, Chaos Visual Debugger).
- [`insights.stat-companions`](insights.stat-companions.md) — runtime `stat`/`dump*` commands that triage a frame *without* recording a trace (find the bound thread, dump a hitch's hierarchy, the Chaos stat groups).
- **Revealing untraced CPU time** — when a trace shows time as exclusive self-time of a coarse scope (e.g. `Tick_Core`) with no children, the named-events → stack-sampling → context-switch → add-a-scope playbook is in [Revealing untraced CPU time](#revealing-untraced-cpu-time) below.
- [`performance-profiling`](performance-profiling.md) — the profiling workflow a trace serves: reproduce before measuring, classify convergence costs apart from steady-state ones, find the bound thread, and interleave every A/B.
- [`performance-profiling.headless-insights`](performance-profiling.headless-insights.md) — analysing a `.utrace` out of process with the standalone Insights binary, and the export-command-line traps that write a successful file from arguments that were silently dropped.

## Revealing untraced CPU time

**The problem.** A captured trace can show a frame's time hiding as *exclusive self-time* of a coarse scope with no traced children — e.g. ~280 ms/frame attributed to `FEngineLoop::Tick` (`Tick_Core` in `LaunchEngineLoop.cpp`) with nothing under it. That time is real CPU work; it just emits no CPU-timeline event. The usual reason: most engine/physics code is instrumented with `SCOPE_CYCLE_COUNTER(STAT_*)`, which feeds the **stats** system but does **not** emit a `Cpu`-channel event by default. The `cpu` channel only carries `TRACE_CPUPROFILER_EVENT_SCOPE` events plus whatever the named-events toggle promotes. So a `SCOPE_CYCLE_COUNTER`-only function is invisible on the timeline, and its cost piles up as self-time in whatever traced ancestor encloses it.

This is a capture-settings problem, not a code problem. Work the playbook in order; stop as soon as the cost has a name.

**Step 1 — Named events (do this first, no relaunch)**

Promotes **every** `SCOPE_CYCLE_COUNTER` (and `QUICK_SCOPE_CYCLE_COUNTER`, `DECLARE_SCOPE_CYCLE_COUNTER`, plus `UObject` `Class::Function` scopes) into a `Cpu`-channel named event for the duration it is on. This is the single highest-yield setting and turns the opaque `Tick_Core` self-time into a named call tree.

- **Enable (runtime, recommended):** `call("editor.console_command", {"command": "stats.NamedEvents 1"})`. The `stats.NamedEvents <0/1>` console command sets `GCycleStatsShouldEmitNamedEvents` directly — no viewport required, so it works in headless/commandlet contexts. Defined at `CoreMisc.cpp:548-576`; the global lives at `CoreGlobals.cpp:462`.
  - The engine-stat form `stat namedevents` also works **but only with an active game viewport** (`UEngine::ToggleStatNamedEvents`, `UnrealEngine.cpp:18867`); it returns early when no viewport is open. Prefer `stats.NamedEvents 1` from an RPC for that reason.
- **Enable (relaunch):** add `-statnamedevents` to the command line (`LaunchEngineLoop.cpp:1716` → `++GCycleStatsShouldEmitNamedEvents`).
- **Verbose variant** — adds sleep/wait and very-high-frequency stats flagged `EStatFlags::Verbose` (off by default to keep the timeline readable):
  - Runtime: `call("editor.console_command", {"command": "stats.VerboseNamedEvents 1"})` (`CoreMisc.cpp:578-597`; sets `GShouldEmitVerboseNamedEvents` and force-enables named events if they were off).
  - Relaunch: `-verbosenamedevents` (`LaunchEngineLoop.cpp:1721-1725`). Note the arg is `-verbosenamedevents`, **not** `-statnamedeventsverbose`.
  - The gate is checked in `StatsSystemTypes.h:1534-1535`: a verbose stat emits only when `GCycleStatsShouldEmitNamedEvents && (GShouldEmitVerboseNamedEvents || !Verbose)`.
- **Channel:** named events ride the `Cpu` channel, which is **on by default** (default set: Bookmark, Cpu, Frame, Gpu, Log, Region, Screenshot). If you stripped channels down, re-add it: `call("insights.set_channels", {"enable": "cpu"})`.
- **Runtime-toggleable:** yes — flip on, capture a few seconds, flip off. The global is just an int counter.
- **Overhead caveat (important):** instrumenting thousands of scopes inflates per-frame timings — Epic documents named-events overhead "as high as 20%" and warns it "should not be used to gauge overall frame performance." **Read structure, not absolute ms:** use it to find *which* scope owns the self-time, then confirm magnitude with `insights.export_trace` `timer_stats` from a clean (non-named-events) capture.

**Step 2 — Stack sampling (catch-all when named events aren't enough)**

Periodically samples the call stack of running threads via **ETW** on Windows, independent of any `SCOPE_*` instrumentation. This is what catches code that has **no scope at all**: third-party/system code, tight inlined loops, busy-waits, and lock spins that named events can't reach.

- **Enable (runtime):** `call("insights.set_channels", {"enable": "stacksampling"})`. Channel name matching is case-insensitive and strips a trailing `"Channel"` suffix (`TraceLog/Private/Trace/Channel.cpp:48-83`), so `stacksampling`, `StackSampling`, and `StackSamplingChannel` all resolve to the same channel defined at `PlatformEvents.cpp:61` (`UE_TRACE_CHANNEL_CUSTOM_DEFINE(StackSamplingChannel, ...)`).
- **Enable (relaunch):** `-trace=default,stacksampling` (parsed at `TraceAuxiliary.cpp:1714`). Pairing with `contextswitch` is recommended (see Step 3).
- **Sampling interval:** default **125 µs (8 kHz)**; override only at launch with `-samplinginterval=<microseconds>` (`TraceAuxiliary.cpp:2189-2192`). There is **no runtime cvar** for the interval. The ETW driver clamps the effective interval to 1221–10,000,000 (×100 ns units) in `EventTracingForWindows.cpp:519`.
- **Windows requirements — hard gate:** the process must be **elevated (run as administrator)**. `FPlatformEvents::CanEnable` checks the token elevation and refuses otherwise with "Process does not have privileges to start event tracing" (`EventTracingForWindows.cpp:257-307`); `StartTraceW` returns `ERROR_ACCESS_DENIED` → "Administrator rights required for ETW" (`EventTracingForWindows.cpp:558-563`). It uses the single system-wide **NT Kernel Logger** session, so it can collide with other ETW tools (WPR/xperf) — those must be stopped first. Enabling the channel without admin silently produces no samples; check `Saved/Logs/<ProjectName>.log` for the privilege error if the timeline stays empty.
- **Overhead caveat:** sampling adds per-sample call-stack-walk cost and a kernel→user event stream; far cheaper than named events for total-frame fidelity, but the trace grows fast at 8 kHz across all threads. Capture in short windows.

**Step 3 — Context switches (computing vs. blocked)**

Records thread scheduling: when each thread is on/off a core and which core. Use it to answer the question named events and stack sampling can't: is that `Tick_Core` self-time **actually computing**, or is the thread **descheduled / blocked on a lock or I/O**? Flat, busy stack samples with no context-switch gaps = compute-bound; frequent off-core gaps = stalled.

- **Enable (runtime):** `call("insights.set_channels", {"enable": "contextswitch"})` (channel `ContextSwitchChannel`, `PlatformEvents.cpp:60`).
- **Enable (relaunch):** `-trace=default,ContextSwitch` (Epic's documented form).
- **What it adds in Timing Insights:** one extra **CPU Core track per core** showing which thread runs where, plus a per-thread header lane marking when the thread is preempted. "Unknown" segments are other processes/the OS stealing the core.
- **Windows requirements:** same ETW elevation gate as stack sampling (shares `FPlatformEvents` / NT Kernel Logger). Supported on Windows, XB1/XSX, PS4/PS5. **Pair it with `stacksampling`** in one capture — together they tell you both *what* the busy thread was doing and *whether* it was actually scheduled.

**Step 4 — Add a permanent scope (when a specific hot function recurs)**

Once stack sampling has named the hot function, the durable fix is to instrument it so it shows up on the `Cpu` channel by default at near-zero cost (no named-events overhead, no admin/ETW). This is the right answer for a function you'll profile repeatedly.

- **Macro:** `TRACE_CPUPROFILER_EVENT_SCOPE(Name)` for a literal scope name, or `TRACE_CPUPROFILER_EVENT_SCOPE_STR("My Timer")` / `..._STR(FName)` when the name is a string/`FName`. Defined in `CpuProfilerTrace.h` (`SCOPE` at line 449, `SCOPE_STR` at line 404). Both ride the `Cpu` channel, so they appear in every default capture with no extra flags.
- **One-line usage** (drop at the top of the function body):
  ```cpp
  void AMyActor::ExpensiveStep()
  {
      TRACE_CPUPROFILER_EVENT_SCOPE(AMyActor::ExpensiveStep);
      // ... work that was previously hiding as Tick_Core self-time ...
  }
  ```
  Use `TRACE_CPUPROFILER_EVENT_SCOPE_STR("AMyActor::ExpensiveStep")` instead if the name must be a string literal (the non-`STR` form stringizes the token and would add quotes around a literal).
- **Overhead:** designed to be permanent and low-overhead — when the `Cpu` channel is off the scope compiles to a cheap disabled check; when on it emits one begin/end pair. Unlike named events it instruments only the one scope you chose, so it does not distort total frame time.

**Could-not-verify / open questions**

- **Stack sampling on non-Windows:** the catch-all path documented here is the Windows **ETW** implementation (`EventTracingForWindows.cpp`). `StackSampling`/`ContextSwitch` are gated behind `PLATFORM_SUPPORTS_PLATFORM_EVENTS`; console/other-platform behavior and their elevation model were not verified — this reference covers the Windows path only.
- **PERF_LOG_USERS as an admin alternative:** the source comment at `EventTracingForWindows.cpp:289-291` says membership in the `PERF_LOG_USERS` group *should* substitute for elevation, but the check is marked `// todo: Implement` and is **not** wired up. Treat "run as administrator" as the only reliable enable path today.
- **`stat namedevents` via `editor.console_command` headless:** confirmed it needs a viewport in source; not empirically tested through the RPC in a `-game`/PIE editor session. `stats.NamedEvents 1` is the viewport-independent path and is what the playbook above uses.
- **`Stat` channel vs named events:** the `stat` channel ("Stats counters, based on the Stats system") surfaces stat *counter values*, not CPU-timeline scopes. It is a different signal from named events and is not a substitute for Step 1; their exact interaction in the Insights Timing view was not exercised here.

### insights.export_trace

Post-process a captured `.utrace` into flat CSV tables — timer stats, per-frame thread timing series, counters, and raw timer/thread dictionaries — for offline pandas/Excel analysis. `insights.start_session` / `insights.stop_session` produce the trace; `export_trace` digests it.

**Runs as a background job.** The handler returns a running ticket immediately, then starts `TraceServices::StartAnalysis()` at a game-thread safe point. A worker polls `IAnalysisSession::IsAnalysisComplete()` and performs the provider scans, CSV construction, and file publication only after analysis completes; it stops, waits for, and releases the TraceServices session on that worker before the lightweight completion re-enters through the PinWright safe-point ticker. The retained continuation covers safe-point startup, while a dispatcher-lifetime lease suppresses late completion if its owner is abandoned. `timeoutSeconds` defaults to 300 seconds and is capped at 1800; timeout is reported as `EXPORT_TIMED_OUT`, and job cancellation/error cleanup removes only paths successfully published by that request. Requests refuse pre-existing CSV/temp artifacts in `outDir` rather than overwriting them. Poll `call("system.job_status", {"ticket_id": "..."})` until the ticket is terminal. A failed CSV publication, including a requested `frame_series` with no Game frames, is reported as `EXPORT_FAILED` with the attempted path and reason; each requested artifact must be present and non-empty, and atomic publication leaves no partial destination. Large traces can still be analysed out of process with the standalone Insights binary — see [`performance-profiling.headless-insights`](performance-profiling.headless-insights.md). The terminal result carries the list of written CSV files, the resolved `outDir`, the trace summary, and the truncation report.

The process-local canonical output-directory reservation rejects concurrent exports targeting the same artifacts.
Deadline enforcement is cooperative at plugin-controlled enumeration boundaries; an individual engine aggregation or OS write is not forcibly preempted.

**Parameters** (most top-level keys accept both `camelCase` and `snake_case`; the snake_case alias is shown in parentheses. `timeoutSeconds` is camelCase-only. The `windows[]` inner keys are the exception — they accept only `startTime`/`start` and `endTime`/`end`, no snake_case variant):

- `tracePath` (`trace_path`) — optional. Absolute path to the `.utrace` to read. Omit to use the newest `.utrace` found in the UnrealTrace store and the project `ProfilingDir`. Use `insights.get_trace_path` / `insights.stop_session` to discover the path of a session you just recorded.
- `kind[]` (`kinds`) — array selecting which exports to emit. Recognized values: `timer_stats`, `frame_series`, `counters`, `timers`. Omit to default to `frame_series` only. Each kind maps to its own output file(s) below.
- `windows[]` (`windows`) — array of named time ranges for the `timer_stats` kind, each `{name, startTime, endTime}` (or `{name, start, end}`), in **seconds**. One `timer_stats__<name>.csv` is written per window so you can diff, e.g., a fly-forward window against a hover window. Omit to aggregate over the whole trace into a single `timer_stats__all.csv`.
- `threads[]` (`threads`) — array of thread-name filters. Restricts `frame_series` columns and `timer_stats` rows to the named threads (substring match). Omit to include every thread present in the trace.
- `timers[]` (`timers`) / `topN` (`top_n`) — `timers[]` restricts the export to an explicit set of timer (scope) names; `topN` instead keeps only the N timers with the highest total inclusive time. Supply one or the other; if both are given, `timers[]` wins and `topN` is ignored.
- `outDir` (`out_dir`) — output directory for the CSVs. Defaults to `Saved/PinWright/insights/` under the project root. The atomic writer creates it if missing and reports directory or publication failures through `EXPORT_FAILED`.
- `format` — output format. Only `csv` is supported in v1; the field exists so JSON/Parquet can be added without an arg-shape break. Defaults to `csv`.
- `tableEntryLimit` (`table_entry_limit`) — hard cap on rows written to any single table (guards against multi-GB CSVs from pathological traces). When a table hits the cap the remaining rows are dropped and the drop is recorded (see truncation, below) — never silently.
- `counterDownsampleHz` (`counter_downsample_hz`) — resample frequency for the `counters` export. Insights counters can fire thousands of times per second; downsampling to, e.g., 60 Hz keeps `counters.csv` small enough to load. Omit to keep the native sample rate.
- `timeoutSeconds` — camelCase-only worker wall-clock deadline. Defaults to 300 seconds; values above 1800 are rejected. Expiry returns `EXPORT_TIMED_OUT` and does not retain newly published artifacts.

**Output files** (written under `outDir`, default `Saved/PinWright/insights/`):

- `timer_stats` → one `timer_stats__<window>.csv` per entry in `windows[]` (or `timer_stats__all.csv` when no windows are given). Columns are per-timer aggregates over that window: call count, total/exclusive/inclusive ms, ms-per-frame. This is the file you diff between two windows to find the regressed scope.
- `frame_series` → `frame_series.csv`. One row per frame, with a pair of columns **per thread**: `<thread>_busy_ms` (sum of timer time on that thread that frame) and `<thread>_span_ms` (wall-clock extent of that thread's work that frame). Use this to plot GameThread vs RenderThread busy time across the trace and spot the slow window.
- `counters` → `counters.csv`. Time series of Insights counters (memory, draw calls, custom `TRACE_*` counters), optionally downsampled via `counterDownsampleHz`.
- `timers` → `timers.csv` plus `threads.csv`. The raw dictionaries: `timers.csv` maps every timer id to its scope name (the lookup you grep to discover Chaos scope names); `threads.csv` maps every thread id to its thread name.

**Truncation is never silent.** When `tableEntryLimit` (or any internal safety cap) drops rows, the finished response reports a `truncation` field — an array of `{table, kind, written, dropped, cap}` entries. An empty array means every table was written in full. Check it before trusting a `timer_stats` diff; a capped table can hide the scope you are hunting.

**Canonical 3-step investigation workflow** (find what got slower between two gameplay states):

1. **Locate the slow window.** Export `frame_series` and plot `GameThread_busy_ms` and `RenderThread_busy_ms` across the whole trace. Find the frame range where total frame time spikes and note which thread carries the cost — that range (in ms) becomes your investigation window, and the dominant thread tells you whether you are CPU-game-bound or render-bound.
2. **Discover the scope names.** Export `timers` and read `timers.csv` to list the timer (scope) names active in the trace. This is how you find the exact Chaos scope strings to target in step 3. Note: under Chaos resimulated physics the resim work runs on **"Foreground Worker #N"** threads, and N is not stable run-to-run — so isolate Chaos cost by **timer name, not by thread**. Filtering `frame_series` by a `Foreground Worker` thread name will miss or mislabel the resim cost; filtering `timer_stats` by the Chaos scope name will not.
3. **Diff two windows.** Run `timer_stats` with two `windows[]` entries — a moving window and an idle window over the same trace — restricted (via `timers[]` or `topN`) to the scopes of interest. This yields `timer_stats__moving.csv` and `timer_stats__idle.csv`. Load both in pandas, join on timer name, and sort by the delta in **exclusive ms-per-frame**; the top of that diff is the scope whose per-frame cost grew when the pawn started moving.
