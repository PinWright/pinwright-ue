# `recorder.integration`

How a project emits its debug journal into the editor-side recorder so the `recorder.*` query verbs have something to read. This is the producer (write) half of the recorder; the query (read) half is the `recorder` namespace.

## What you write vs. what the plugin already does

Everything here happens in C++ — there is no RPC that writes journal entries.

The plugin's `RecorderLifecycle.cpp` (an editor-only chunk of the gateway module) already owns the whole session lifecycle, so an integrator's job is small. The plugin does:

- Open a session on **BeginPIE** and close it on **EndPIE** (`BeginSession`/`EndSession`), gated by the `bJournalEnabled` project setting.
- Drain the lock-free producer queue once per game-thread tick (`DrainAndFlush`), handing the tick's NDJSON lines to the OS in a single buffered write. The writer never issues a device flush (`FlushFileBuffers`) — the session file survives an editor crash (the OS holds the bytes), but the last seconds before a full OS crash / power loss may be lost. A per-tick device flush is what used to stall PIE by 270–400 ms.
- Stamp the **render** time domain once per world tick (`StampDomain(EJournalDomain::Render, World->GetTimeSeconds(), GFrameCounter)`), so every game-thread value/event inherits the current world time and render frame with no per-site stamping.

So an integrator only ever calls the **emit** verbs below. Do **not** call `BeginSession`/`EndSession`/`DrainAndFlush` from integration code — the plugin owns them, and a second `BeginSession` no-ops anyway. Call `StampDomain` yourself only on a thread that carries an independent time base (an async physics/worker thread); the game thread is already stamped for you.

## Producer API (`FJournalRecorder`, `Source/PinWrightRecorder/Public/JournalRecorder.h`)

All entry points are static; the facade has no instance or project-object dependency.

| Call | Purpose |
|---|---|
| `LogVariable(KeyOrObj, Tag, Value)` | Record one sample of a variable. `KeyOrObj` is a `UObject*` (auto-registered + auto-keyed on first sight) or an `FName` key; `Tag` is an `FName` channel/field name. |
| `LogEvent(Name, Props[, Sev])` | Record a discrete global event. `Name` is an `FName`, `Props` is `TArray<TPair<FName, FRecordedValue>>`, `Sev` is an optional `EJournalSeverity`. An object-keyed overload `LogEvent(Key, Name, Props[, Sev])` also exists. |
| `RegisterObject(KeyOrObj, Label)` | Attach a readable label to a catalog entry. Only needed for `FName`-keyed series or to override a UObject's default name-derived label. |
| `KeyFor(Object)` | The stable `FName` key a UObject-keyed log resolves to, if you want to mix object- and key-addressed logging for the same entity. |
| `IsRecording()` | Relaxed-atomic gate; cheap from any thread. |

**`LogVariable` value overloads:** `float`, `double`, `int32`, `int64`, `bool`, `FVector2D`, `FVector`, `FVector4`, `FQuat`, `FRotator`, `FString`, and any reflected `UENUM` (the enum overload records the underlying integer plus the member name resolved via `StaticEnum<TEnum>()`). The change-point compressor only writes a new row when a value actually changes, so logging an unchanged value every frame is cheap on disk.

**The `PW_JOURNAL_LOG` guard macro** wraps a `LogVariable` call in an `IsRecording()` check so the **value expression is not evaluated** when no session is recording:

```cpp
#define PW_JOURNAL_LOG(KeyOrObj, Tag, Value) \
    do { if (FJournalRecorder::IsRecording()) FJournalRecorder::LogVariable((KeyOrObj), (Tag), (Value)); } while (0)
```

Every bare `LogVariable`/`LogEvent` call is *already* a no-op while no session is open (it checks `IsRecording` internally and returns). The macro's only added value is skipping an **expensive** `Value` computation on a hot path — use it when the argument itself costs something; a plain `LogVariable` is fine otherwise.

## Editor-only integration (the part that bites)

`PinWrightRecorder` is `"Type": "Editor"` in `PinWright.uplugin`, so the module does **not** exist in packaged/shipping builds. How you link it depends on the integrating module's type:

**Case A — integrating module is already editor-type.** Link it directly; no guards are needed, because it isn't built for non-editor targets either:

```csharp
PrivateDependencyModuleNames.Add("PinWrightRecorder");
```

**Case B — integrating module is a runtime module** (the common case — gameplay code emitting per-frame journal entries). The dependency and every call site must be excluded from non-editor builds, or the package link fails:

```csharp
// Integrating runtime module Build.cs
if (Target.bBuildEditor)
{
    PrivateDependencyModuleNames.Add("PinWrightRecorder");
}
```

```cpp
// Integrating runtime .cpp — every producer call site
#include "JournalRecorder.h"   // also inside #if WITH_EDITOR if the header isn't in the runtime include path
// ...
#if WITH_EDITOR
PW_JOURNAL_LOG(this, TEXT("health"), Health);
FJournalRecorder::LogEvent(TEXT("pawn:died"), { { TEXT("cause"), FRecordedValue::From(CauseName) } });
#endif
```

`#if WITH_EDITOR` is the right gate (not `WITH_EDITORONLY_DATA`): it is true in editor and editor-commandlet builds — exactly where the recorder module exists — and false in `Development`/`Shipping`/`Test` game targets.

## Optional: a runtime proxy to avoid `#if WITH_EDITOR` churn

If a runtime module has many call sites and you want to keep `#if WITH_EDITOR` out of gameplay code, the **standard UE editor-only-from-runtime pattern** applies — it is not specific to this plugin: declare a tiny runtime interface with no-op default methods, implement it in an editor-only module that forwards to `FJournalRecorder`, and have the runtime code call through the interface. The runtime module then never links `PinWrightRecorder` and needs no per-call guard; the single `if (Target.bBuildEditor)` gate moves to the editor-only impl module. This is a convenience only — the Case B `#if WITH_EDITOR` recipe above is complete and sufficient on its own.

## Reading back what you emitted

Once an integrator emits values and runs a PIE session, the data is queryable through the read verbs: `recorder.list_sessions` → `recorder.describe_session` (catalog + activity) → `recorder.get_series` / `recorder.find_events` / `recorder.list_segments`. The query/pagination/segment contract is documented in `docs/arch.md` in the plugin folder (Recorder Query Pagination, Recorder Session Segments) — a maintainer document, not a wiki page.

## See also

- [`recorder`](recorder.md) — the read-side `recorder.*` verbs this producer surface feeds.
- [`session`](session.md) — starting and stopping the PIE sessions a recording brackets.
