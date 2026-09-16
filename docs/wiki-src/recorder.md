# `recorder`

Query the editor-side debug journal recorder — list sessions and read back the per-frame variable series, change-points, events, and gameplay segments a PIE session captured to disk. The `recorder.*` verbs here are the READ side; the WRITE/producer side (how a host project emits journal entries) lives off-wire in C++ and is documented on [`recorder.integration`](recorder.integration.md).

## Producer side (emitting journal entries from a host project)

The `recorder.*` RPCs only *read* sessions. A session's catalog is empty
(`describe_session` returns `objects:[], variables:[]`) until code pushes values through the C++
producer API; there is no RPC to write journal entries. The producer surface is the static
`FJournalRecorder` facade in `Source/PinWrightRecorder/Public/JournalRecorder.h`:

- `FJournalRecorder::LogVariable(KeyOrObject, Tag, Value)` — record one sample of a variable (a UObject* or an `FName` key, plus an `FName` tag). Overloads cover float/double/int32/int64/bool, the math types (FVector/FVector2D/FVector4/FQuat/FRotator), FString, and any reflected `UENUM`.
- `FJournalRecorder::LogEvent(Name, Props[, Severity])` — record a discrete event with property pairs (and optional `EJournalSeverity`); an object-keyed overload also exists.
- `FJournalRecorder::RegisterObject(KeyOrObject, Label)` — give a catalog entry a readable label (UObject-keyed `LogVariable` auto-registers on first sight, so this is only needed for `FName`-keyed series or a nicer label).
- `PW_JOURNAL_LOG(KeyOrObj, Tag, Value)` — guard macro: evaluates the value expression and calls `LogVariable` **only** while a session is recording. Prefer it on hot paths so the value computation is skipped when not recording.
- `FJournalRecorder::IsRecording()` — relaxed-atomic no-op gate, cheap from any thread. Every producer call is already a no-op when no session is open, so explicit gating is only needed to skip an expensive value computation.

**The plugin owns lifecycle and the render-domain stamp.** `RecorderLifecycle.cpp` wires
`BeginSession`/`EndSession` to PIE begin/end, drains each tick, and stamps the render time domain
once per world tick. Integrating code only emits `LogVariable`/`LogEvent`; it does **not** call
`BeginSession`/`EndSession`/`DrainAndFlush`/`StampDomain`. Values inherit the current render-frame
stamp. Call `StampDomain` only on a thread with an independent time base (e.g. async physics).

## Editor-only packaging constraint

`PinWrightRecorder` is an **editor-type module** (`"Type": "Editor"` in `PinWright.uplugin`), so it is not present in packaged/shipping builds. A host **runtime** module that links it must keep both the dependency and the calls out of non-editor builds:

```csharp
// In the host module's Build.cs
if (Target.bBuildEditor)
{
    PrivateDependencyModuleNames.Add("PinWrightRecorder");
}
```

```cpp
// At every producer call site in the host runtime module
#if WITH_EDITOR
PW_JOURNAL_LOG(Pawn, TEXT("speed"), Pawn->GetVelocity().Size());
#endif
```

An **editor-type** module links the recorder directly and needs neither guard. The full recipe,
including the optional runtime-proxy approach, is on [`recorder.integration`](recorder.integration.md).

## `recorder.query` execution and session selection

`recorder.query` writes the caller's function body to a temporary script and runs it in the
engine-provided Python interpreter as an isolated child process (`-I -u`), rather than through
the editor's embedded Python interpreter. The handler returns a `system.job_status` ticket from
`FHandlerContext::StartJob`; the child stdout is drained by the core ticker and the temporary
script is closed and removed only after the termination worker and child cleanup are safe to
finalize. Cancellation and timeout request process-tree termination on a background worker while
the job remains alive for bounded, nonblocking exit confirmation. If the hard cutoff is reached
first, the terminal result reports `cleanupIncomplete: true` when the worker or child exit was not
confirmed. If the worker is still active, its owned handle remains alive; its completion callback
then makes a best-effort cleanup attempt. The query surface remains
the read-only stdlib-backed `recorder_query` module (`json`, `bisect`, and `math` only).

`timeoutSeconds` is optional (default `30`) and is clamped to `0.1-60.0`. If the child is still
running at the deadline, the handler terminates the process tree and the terminal result carries
`timedOut: true` (also in `meta`), `timeoutSeconds`, and `elapsedSeconds`. Timeout results also
carry `terminationConfirmed: true` only when both child exit and worker completion are observed
before the hard cutoff, or `false` otherwise. `cleanupIncomplete: true` means the bounded cutoff
was reached before both observations were available; no successful completion is claimed.
`system.job_cancel` uses the same killable process boundary. `maxRows` still limits the serialized
result, but it is not a runtime budget.

Session resolution first accepts an exact path, filename, or session id through the shared
`RecorderResolver`. The legacy substring form is accepted only when exactly one `.ndjson` file
matches; multiple matches return `AMBIGUOUS_SESSION` with sorted `candidates` so callers can
reissue the request with an exact identifier.

## See also

- [`recorder.integration`](recorder.integration.md) — the full producer recipe, including the editor-only dependency/`WITH_EDITOR` gate and optional runtime-proxy pattern.
- The read verbs support query, pagination, and per-session segments. Session resolution is explicit: there is no `latest` alias today, so name the session you want to read. For `recorder.query`, use an exact session id when substring matching could collide.
