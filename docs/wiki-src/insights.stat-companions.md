# `insights.stat-companions`

Console `stat` / `dump*` commands that triage a frame **without** recording a `.utrace` — run them first to learn which thread is bound and roughly where the time went, then start a trace already knowing what to look for (or skip the trace for an obvious bottleneck).

## How to run these

Run all of these through `call("editor.console_command", {...})`; `call("performance")` wraps some overlays. The `dump*` commands write `Saved/Logs/<ProjectName>.log`; read that file after the run.

## Frame-cost triage (which thread is bound)

- **`stat unit`** — per-frame ms for **Frame** (total), **Game**, **Draw**, **GPU**, and **RHIT**. The largest identifies the bound thread: Game = gameplay/physics/tick, Draw = render submission, GPU = shading/overdraw/resolution, RHIT = RHI translation. Values are EMA-smoothed. (`performance.show_stats` with `unit` is the toggle wrapper.)
- **`stat unitgraph`** — graphs those values over time, showing hitch cadence and the spiking thread; distinguish periodic GC/streaming from a one-off.
- **`stat unitmax`** / **`stat raw`** — rolling max for spikes hidden by EMA / unsmoothed per-frame values.
- **`stat fps`** — average FPS only; prefer `stat unit`. (`performance.show_fps`.)

## Hierarchical frame breakdown (where a hitch went, no trace)

These dump per-stat call hierarchies to the log, giving a no-trace view inside a hitch.

- **`stat dumpframe -ms=<threshold>`** — captures the next frame and logs inclusive/exclusive hierarchy, omitting nodes under the threshold (`-ms=0` = all). Add `-root=<stat>` and `-depth=<n>` to restrict it; e.g. `stat dumpframe -ms=1 -root=physics`.
- **`stat dumpave|dumpmax|dumpsum [-num=N] [-ms=<t>]`** — average / per-node max / sum over N frames (default 30); `dumpmax` finds the worst-frame contributor.
- **`stat dumphitches`** — toggle auto-dumps for frames over `t.HitchThresholdMS`; turning it off logs hitch count/time. `t.DumpHitches.AllThreads 1` includes all threads (default Game + Render).
- **`stat dumpevents [-ms=<t>] [-all]`** — wait/trigger callstacks for slow task-graph events, useful when a hitch is a cross-thread wait.

Once a `dump*` names the offending scope, start an Insights trace (`insights.start_session`) for its timeline/threading/per-call detail, then digest with `insights.export_trace`.

## Stat groups for a physics-heavy game

On-screen group overlays (toggle a second time to clear). Group name = `STATGROUP_` symbol minus the prefix, case-insensitive.

- **`stat game`** — gameplay-tick cost (actor ticks, world tick phases). First stop when `stat unit` is Game-bound.
- **`stat engine`** — high-level engine frame breakdown (game vs render vs RHI).
- **`stat chaos`** — core Chaos solver scopes (Physics Tick, Physics Advance, Solver Advance, Integrate Solver, Sync Physics Proxies); the primary Chaos group (`stat physics` is largely empty under Chaos).
- **`stat chaoscounters`** — cheap Chaos counts (dirty AABB-tree elements, bodies, clusters) that expose structural blowups behind rising `stat chaos` cost.
- **`stat chaoscollision` / `chaosconstraintsolver` / `chaosjoint` / `chaosislands`** — drill into a Chaos subsystem once `stat chaos` points at it.
- **`stat scenerendering`** — render-thread scene cost (draw calls, primitives, visibility/cull). First stop when `stat unit` is Draw-bound.
- **`stat particles`** — particle/Niagara emitter cost.
- **`stat tickgroups`** — time per tick group (`TG_PrePhysics`, `TG_DuringPhysics`, `TG_PostPhysics`…) — tells you whether cost is pre- or post-physics, which matters when gameplay control logic interleaves with the Chaos step.

Use `call("performance.show_stats")` for categories it wraps; use `editor.console_command` for other groups or chained `dump*` commands.

## Tick analysis and hitch reproduction

- **`dumpticks`** (plain exec, **not** `stat dumpticks`) — logs every registered tick function, group, and enabled state. `dumpticks grouped` gives per-context counts; `enabled` / `disabled` filter them. Pair with `stat tickgroups` for cost.
- **`t.MaxFPS <n>`** — runtime cap (0 = uncapped). Set to the target (`t.MaxFPS 60`) to reproduce a hitch under the player's frame budget, or `0` to confirm a stall isn't a cap artifact. (Wrapped by `performance.set_frame_rate_limit`.)
- **Fixed-timestep / deterministic repro** — launch flags `-benchmark` (fixed step) + `-fps=<n>` make a repro frame-for-frame repeatable. They are **launch-only**; a live PIE session must be relaunched.

## See also

- [`insights`](insights.md) — namespace overview + the "Revealing untraced CPU time" playbook.
- [`insights.tracing-reference`](insights.tracing-reference.md) — trace channels, `Trace.*` commands, launch args, and capture recipes (memory/GPU/CSV/regions/ChaosVD).
- [`performance`](performance.md) — typed wrappers for stat overlays (`show_stats`, `show_fps`), scalability, and memory reports.
