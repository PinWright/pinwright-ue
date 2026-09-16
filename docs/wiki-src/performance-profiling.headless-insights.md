# performance-profiling.headless-insights

Analysing a captured `.utrace` with no GUI, without stalling the editor, and without believing an
export that silently ignored half your arguments. Read after
[`performance-profiling`](performance-profiling.md), which owns the method this toolchain serves.

## Analyse Out Of Process

**In-editor trace analysis runs as a background job.**
[`insights.export_trace`](insights.export_trace.md) returns a ticket, starts
`TraceServices::StartAnalysis()` at a game-thread safe point, and lets a worker
poll `IAnalysisSession::IsAnalysisComplete()` before provider scans and CSV
publication; the worker stops, waits for, and releases the analysis session
before completion is delivered through the PinWright safe-point ticker.
Poll `system.job_status` for the terminal result, and treat `EXPORT_FAILED` as
a real publication or missing-artifact failure rather than an empty export. A
requested `frame_series` with no Game frames is also `EXPORT_FAILED`. The standalone binary
remains useful for very large traces, unsupported analyzer modules, or a separate process that must
not share editor resources.

The in-editor request accepts `timeoutSeconds` (default 300, hard maximum 1800). Expiry returns
`EXPORT_TIMED_OUT`; cancellation, timeout, and export errors release the analysis session off-thread
and remove only artifacts successfully published by that request. A request refuses pre-existing
CSV/temp artifacts in its output directory instead of overwriting them.
The process-local output-directory reservation also rejects concurrent exports targeting the same artifacts.
The deadline is cooperative around plugin-controlled enumeration boundaries; engine aggregation and an individual OS write are not forcibly preempted.

    UnrealInsights.exe -OpenTraceFile="<abs path to .utrace>" ^
      -unattended -autoquit -noui -nullrhi ^
      -ExecOnAnalysisCompleteCmd="TimingInsights.ExportTimingEvents <abs out.csv> -columns=* -threads=GPU*"

This is the shape the engine's own `ExportTimerStatisticsFromUtrace` automation script uses, so the
switches are stable. It touches no editor and can run while one is busy. Useful commands:
`TimingInsights.ExportTimingEvents` (every event span), `TimingInsights.ExportTimerStatistics`
(aggregates), `TimingInsights.ExportTimerCallees` (what a scope spends its time in),
`TimingInsights.ExportTimers` and `TimingInsights.ExportThreads` (the id → name dictionaries),
`TimingInsights.ExportCounters` and `TimingInsights.ExportCounterValues` (counter dictionary and
series).

## Four Ways The Export Command Line Lies

**`;` does not chain commands.** The whole value is handed to a single `Exec` call, so the first
command matches and everything after the semicolon becomes trailing tokens of *that* command — the
second export is silently not run. One export per invocation, and the analysis is re-run each time;
budget for that on a large trace rather than discovering it after four exports are missing. (The
engine's own `Automation RunTests X;Quit` form works because the automation command parses its own
semicolon, not because the switch chains.)

**Any `"` inside the value truncates the token list, silently.** The value is parsed as one quoted
token, so an inner quote ends it and every switch after that point — `-threads=`, `-startTime=`,
`-timers=` — is dropped. **The export still reports success and still writes a file.** Five identical
unfiltered exports were produced under the belief that they were five different filtered ones. Write
the inner arguments unquoted (`-threads=GameThread`, not `-threads="GameThread"`) even though the
engine's own inline examples show them quoted; those examples describe the in-GUI console form, where
the outer quoting does not exist.

**A misspelled switch is a log warning, not an error.** The exporter logs `Unknown Cmd Param: <token>`
and proceeds unfiltered. It also logs every token it *did* parse. **Read that log before trusting the
CSV** — it is the only place the truncation above is visible.

**`ExportTimerStatistics` ignores your thread filter for GPU work.** `-threads=` is wired to the CPU
thread filter only; the GPU queue filter is hardcoded to accept everything and both legacy GPU tracks
are force-included. So `-threads=GameThread` yields a table containing every GPU scope, exclusive
sums that exceed wall-clock time, and a "bottleneck" that is not on the thread you asked about.
`ExportTimingEvents` honours the filter — export raw spans from it and aggregate them yourself.

## What The CSV Cannot Give You

**Counters need a channel the default capture does not enable.** The default channel set is
`cpu,gpu,frame,log,bookmark,screenshot,region` — no `counters`. `ExportCounterValues` therefore
returns nothing from a default capture: no draw calls, no ray-tracing instance counts, no
dynamic-resolution fraction, no memory series. Enable it **at capture time** (`-trace=default,counters`
at launch, or `Trace.Enable counters` before recording); it cannot be added to a trace afterwards. If
draw-call counts matter, the CSV profiler is the cheaper source — see
[`insights.tracing-reference`](insights.tracing-reference.md).

**Scope metadata is unreachable from CSV.** The timing-event export columns are `ThreadId`,
`ThreadName`, `TimerId`, `TimerName`, `StartTime`, `EndTime`, `Duration`, `Depth`. There is no
metadata column. A scope whose displayed name is built from a metadata format string exports as the
**format string itself** — `MeshCardCapture - Pages:%u Draws:%u Instances:%u Tris:%u` — so the most
identifying datum on the scope, the one that names what was captured and how much of it, is exactly
the one the CSV cannot carry. Three ways out, in order of cost: get the fact live from the
subsystem's own logging CVar, open the trace in the Insights GUI, or accept that you know the scope
but not its subject.

## What To Compute From A Timing-Events Export

The whole triage in [`performance-profiling`](performance-profiling.md) is stdlib arithmetic over one
timing-events CSV. In order:

- **Per-thread exclusive time** — subtract each event's children by `Depth` within the thread. A
  thread whose exclusive time is dominated by a wait scope is not the cost; follow it downstream.
- **Frame classification** — mark each frame by whether the scene-render scope appears in it, then
  report the two distributions separately. A merged distribution hides the tail.
- **Per-scope mean by speed class** — bucket frames into slow, mid and fast, and tabulate each
  scope's mean cost in each bucket. **Rank by variance across the buckets, not by mean.** A scope that
  stays flat across a 15x frame-time swing is not the cause however large it is; the scope whose cost
  tracks the frame time is. This single table is what overturned a careful profile that had ranked by
  mean and blamed the wrong pass.
- **Child-scope count per frame** — count repeats of the suspect scope per frame and compare against
  the subsystem's per-frame budget. Counts that peak at *exactly* the cap, with a band of frames just
  below it, mean the budget is saturated and the cost is a queue, not a spike.
- **Cross-queue span matching** — for any compute-queue scope, check whether its start and end match a
  graphics-queue scope's to within a fraction of a millisecond. Matching spans mean a fence wait; do
  not rank it.

## See also

- [`performance-profiling`](performance-profiling.md) — the workflow this analysis serves, and what
  each step's numbers are allowed to conclude.
- [`insights`](insights.md) — starting, stopping and locating a trace, and the in-editor export verb.
- [`insights.tracing-reference`](insights.tracing-reference.md) — channels, launch args and the
  per-goal capture recipes, including the CSV profiler for per-frame budgets.
- [`insights.stat-companions`](insights.stat-companions.md) — the no-trace triage commands.
