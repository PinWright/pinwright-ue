# performance

Scalability, CVar, and performance-capture helpers for the active editor or PIE session — apply baseline rendering settings, tune perf-related console variables, collect timing/stat data, and capture performance snapshots without editing asset content. Use `call("insights")` for trace capture and `call("system.console.search")` to discover CVar names before tuning them.

## CVar writes here cannot outlive the call

**Every `performance.*` verb that writes a console variable writes it at the priority that variable already carries, never at the default `ECVF_SetByCode`.** This matters because `IConsoleVariable::Set(Value)` defaults to `ECVF_SetByCode` = `0x0E000000`, six levels above the `ECVF_SetByScalability` = `0x02000000` that `Scalability::SetQualityLevels` — and therefore the editor's own *Settings → Engine Scalability Settings* panel — writes at. `FConsoleVariableBase::CanChange` is `NewPri >= OldPri`, so a single Code-priority write to a scalability CVar takes that quality group away from the human using the editor **for the rest of the session**, with no way to undo it short of a restart. Seven such CVars were reachable this way through `performance.apply_baseline_settings` and `performance.configure_texture_streaming` alone.

Writing at the CVar's existing priority is accepted unconditionally (the guard is `>=`, not `>`), never raises the priority, and never lowers it — so the write always lands and the user's panel keeps working. It is **not** the same as writing at `ECVF_SetByScalability`: that would be discarded outright for `r.VSync`, which `UGameUserSettings` writes at `ECVF_SetByGameSetting`, and the engine reserves Scalability priority for scalability-flagged variables.

The consequence for a caller: **a `performance.*` write is not sticky.** Anything at an equal or higher priority — a device profile, a project setting, the user's Scalability panel, a console line — can still change it afterwards. If you need a value to survive a scalability change, set the scalability level first with `performance.set_scalability` and tune after.

## See also

- [`performance-profiling`](performance-profiling.md) — the workflow these verbs serve: what to measure, in what order, and which of these numbers is allowed to conclude what. Read it before running an A/B; a non-interleaved before/after in a live editor measures machine drift, and a low percentile does not protect you.
- [`performance-profiling.headless-insights`](performance-profiling.headless-insights.md) — offline `.utrace` analysis without stalling the editor.
- [`insights`](insights.md) — recorded traces and offline timing breakdowns.

### performance.run_benchmark

**It measures frame time, and it is the only `performance.*` verb that returns a measured quantity.** It samples every frame for `duration` seconds (default 5) and completes with the measurement:

```json
{
  "requestedDurationSeconds": 5,
  "measuredDurationSeconds": 5.014,
  "frameCount": 312,
  "frameTimeMs": { "min": 14.9, "p50": 16.1, "p95": 22.4, "max": 41.2, "mean": 16.07 },
  "avgFps": 62.2,
  "statFileCaptured": false,
  "warnings": ["No .uestats file: ..."]
}
```

**Async.** The call returns a ticket; poll `system.job_status` with it, or send `_meta.progressToken` plus `Accept: text/event-stream` to block and stream. The measurement is on the ticket's result, not on the immediate response.

**Requested and measured are separate numbers.** Whole frames are sampled, so the window closes on the first frame that carries `measuredDurationSeconds` past `requestedDurationSeconds` and overruns it by up to one frame. `frameCount`, `avgFps` and `frameTimeMs` all describe the **measured** window. A `warnings` entry appears when the two differ by more than 10%.

**Percentiles are nearest-rank, no interpolation** — every number in `frameTimeMs` is the duration of a frame that was actually observed. Read `p50`/`p95` rather than `mean`: a stalling frame can only add time, so the mean hides exactly the spikes worth benchmarking for.

**It never succeeds without numbers.** If no frame time could be observed the job **fails** with `FRAME_TIME_NOT_MEASURED`. It used to complete a successful job whose whole payload was `{captured:false}`; that shape is gone, and the regression test `PinWright.performance.run_benchmark.ReportsMeasurementOrFails` keeps it gone. A non-positive `duration` is refused up front with `INVALID_ARGUMENT`.

**The `.uestats` capture is a side artefact, not the measurement.** Where the engine still compiles the legacy stat-file capture in, one runs alongside and its path comes back as `statFilePath` with `statFileCaptured: true`. **UE 5.8 compiles it out** — `UE_ENABLE_STATS_FILE_DEPRECATED_IN_5_8` defaults to 0, so `stat startfile` no longer parses — and there `statFileCaptured` is `false` with a `warnings` entry saying so. The frame measurement is unaffected either way; for a full trace use `insights.start_session` / `insights.export_trace`.

**There is no `type` parameter.** It was declared, defaulted to `"all"`, read by nothing and validated by nothing — it advertised benchmark modes this verb has never had. Sending it now returns `UNKNOWN_PARAMS`.

**Its two siblings are not fixable the same way.** `performance.start_profiling` / `performance.stop_profiling` exist only to drive that legacy capture, so on UE 5.8 they correctly refuse with `NOT_SUPPORTED` rather than reporting a capture that cannot happen.

### performance.apply_baseline_settings

Sets a curated bundle of seven `r.*` render CVars for a `profile` of `"performance"`, `"quality"`, or `"balanced"` (default). It touches **no** `sg.*` group — `performance.set_scalability` is the verb for those — and the response says so with `scalabilityGroupsChanged: false`.

**Six of the seven CVars are scalability members even though no `sg.*` value moves.** `r.VSync`, `r.MotionBlurQuality`, `r.DepthOfFieldQuality`, `r.BloomQuality`, `r.ShadowQuality` and `r.MaxAnisotropy` all carry `ECVF_Scalability` and belong to the PostProcessQuality, ShadowQuality and TextureQuality groups; only `r.AllowHDR` does not. Reading `scalabilityGroupsChanged: false` as "this call left the user's scalability settings alone" was therefore a mistake the response invited: the field is literally true and materially incomplete. **`scalabilityCVarsPinned: false` is the field that answers it** — every write goes in at the CVar's existing SetBy priority, so the call cannot raise one above the editor's Scalability panel. See "CVar writes here cannot outlive the call" on `call("performance")`.

**`appliedCVars` reports what the CVar reads AFTER the write, not what was requested.** The two differ when the engine clamps a value, and when `CanChange` discarded the write because something already sits at a higher priority. Each entry is `{cvar, value}`.

**`r.VSync` is the one worth knowing about.** `UGameUserSettings::ApplyNonResolutionSettings` writes it at `ECVF_SetByGameSetting`, above this verb on a host that has applied game user settings — so on such a host the write is discarded and `appliedCVars` will report the value that survived, not the profile's.

### performance.configure_texture_streaming

Writes `r.TextureStreaming` from `enabled` (default true), `r.Streaming.PoolSize` from `poolSize` when that parameter is present, and optionally pushes the editor camera location into the streaming manager with `boostPlayerLocation`.

**`poolSize` is in MB and is only written when you pass it.** Omitting it leaves the pool alone rather than resetting it; `-1` is the engine's "default pool size" sentinel.

**`r.Streaming.PoolSize` is a TextureQuality scalability member**, so it is written at its existing SetBy priority and this call cannot pin it above the editor's Scalability panel. `r.TextureStreaming` is not scalability-flagged but takes the same write rule. See "CVar writes here cannot outlive the call" on `call("performance")`.

**The response is an acknowledgement, not a measurement.** It does not read the pool size back, and the streaming pool the engine actually uses is subject to `r.Streaming.UseFixedPoolSize` and platform limits. Read the effective value with `system.console.search` on `r.Streaming.PoolSize` if it matters.
