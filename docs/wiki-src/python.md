# python

Run code (or a `.py` file path) inside UE's built-in Python interpreter for editor APIs exposed through reflection but not covered by a typed handler. Check the relevant namespace first; for live PIE UObject reads, prefer [`runtime-uobject-inspection`](runtime-uobject-inspection.md).

## Cross-cluster overlap

Use this sparingly. Almost everything has a typed handler somewhere in this wiki. Reach for `python.execute` when:

- The operation needs a tight loop over many objects (one Python script vs. hundreds of RPCs).
- You need an `unreal.*` API surface that has no typed wrapper (e.g. niche editor toolbox functions).
- You're prototyping; once the operation stabilizes, consider promoting it to a typed handler.

## Marking packages dirty from Python

**On UE 5.8, `asset.mark_package_dirty()` does not exist in engine Python; `actor.mark_package_dirty()` and `set_dirty_flag` are also not exposed.** `UObjectBaseUtility::MarkPackageDirty` (`UObjectBaseUtility.h:527`) and `UPackage::SetDirtyFlag` (`Package.h:649`) are C++ APIs without `UFUNCTION`, and the Python exporter wraps a function only when it is flagged `BlueprintCallable`/`BlueprintEvent` (`PyGenUtil.cpp:1615`) — so those engine-style calls do not exist and `unreal.Package` is an empty wrapper with no dirty methods on it at all. Advice of the form "just dirty the package yourself from `python.execute`" is not actionable as written. This matters because **a save no-ops on a clean package**: an in-memory edit that never set the flag is silently lost when the editor closes.

PinWright ships the replacement as a reflected `UBlueprintFunctionLibrary`, exposed to bundled Python as `unreal.PinWrightPackageLibrary`:

| Call | Returns |
|---|---|
| `unreal.PinWrightPackageLibrary.mark_package_dirty(asset)` | `bool` — true only when the package is observably dirty afterwards |
| `unreal.PinWrightPackageLibrary.mark_actor_package_dirty(actor)` | `bool` — same contract, typed for level actors |
| `unreal.PinWrightPackageLibrary.mark_package_dirty_by_path(asset_path)` | `bool` — package or object path; the package must already be loaded |
| `unreal.PinWrightPackageLibrary.is_package_dirty(obj)` | `bool` — read-only probe |
| `unreal.PinWrightPackageLibrary.is_package_dirty_by_path(asset_path)` | `bool` — read-only probe |
| `unreal.PinWrightPackageLibrary.describe_mark_dirty_blocker(obj)` | `str` — `""` when dirtying is allowed, else the reason it is refused |
| `unreal.PinWrightPackageLibrary.get_package_name(obj)` | `str` — the package a save would actually write |

```python
import unreal
lib = unreal.PinWrightPackageLibrary

asset_path = '/Game/Props/SM_Example'
asset = unreal.load_asset(asset_path)
assert asset is not None
if not lib.mark_package_dirty(asset):
    unreal.log_error(lib.describe_mark_dirty_blocker(asset))   # says exactly why

# Assert the in-memory post-condition before saving.
assert lib.is_package_dirty(asset)
assert unreal.EditorAssetLibrary.save_asset(asset_path, only_if_is_dirty=False)

# The force-save call is not durable proof by itself: read the package back clean.
assert not lib.is_package_dirty(asset)
unreal.log(lib.get_package_name(asset))
```

`False` is never silent: the reason is always available from `describe_mark_dirty_blocker`, and the `LogPinWrightPackageDirty` category logs it (`Verbose` for a caller-side refusal, `Warning` when the editor suppressed a dirty that should have worked). Dirtying is refused for null/garbage objects, transient objects, the transient package, native script packages, PIE duplicates, cooked packages, and while the editor is loading, undoing/redoing, transacting, cooking, or async-loading. Nothing here loads, saves, or allocates, so none of it can trigger the GC-during-Python crash described under "Calls that crash the editor" below.

Two engine APIs do reach the dirty flag and are fine when they fit — reach for these first if you cannot depend on PinWright:

- `unreal.get_editor_subsystem(unreal.EditorAssetSubsystem).set_dirty_flag(obj, True)` — the clean route **for content assets**, with a real bool result. It refuses any non-asset object and **any package containing a map** (`EditorAssetSubsystem.cpp:1162-1171`), so it can never dirty a level or an actor.
- `unreal.SystemLibrary.transact_object(obj)` — forwards to `UObject::Modify()`, so it dirties anything including maps. It returns **void**, so a refusal is indistinguishable from success, and it writes the object into the open transaction — which corrupts undo when called *after* the edit instead of before.
- `unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages()` / `get_dirty_content_packages()` enumerate the dirty set read-only.

`unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)` is the bundled-Python force-save path, but it rewrites packages that did not need rewriting — prefer dirty-then-save, then verify `is_package_dirty` is false and confirm the saved asset survives a reload.

From the MCP surface, use [`asset.mark_dirty`](asset.md) and [`asset.is_dirty`](asset.md) rather than routing a one-line dirty through `python.execute`.

## Calls that crash the editor

Your script runs inside the editor process with no sandbox and no timeout. **Nothing on this page is enforced** — no call is refused, no loop is bounded. This section and the next are the only protection that exists, so read them before writing a script that touches materials or loops at all.

**Never call `MaterialEditingLibrary.recompile_material()` from `python.execute`.** Use `call("material.authoring.compile_material", { assetPath })` instead: native, no garbage collection, and it returns a real verdict rather than silence — branch on `compileSucceeded`, and read `compileErrors[]` for the reason. (`compiled` is a legacy field pinned to `true`; it means the compile ran, not that it passed.)

The recorded chain, read outside-in — Python calls the engine, the engine calls GC, GC calls back into Python:

```
unreal.MaterialEditingLibrary.recompile_material(m)
  -> UMaterialEditingLibrary::RecompileMaterial
  -> FMaterialEditorUtilities::BuildTextureStreamingData
  -> CollectGarbage()
  -> FPythonScriptPlugin::OnPreGarbageCollect
  -> python311.dll        EXCEPTION_ACCESS_VIOLATION
```

**Deferral cannot fix this, so do not expect the plugin to grow a guard for it.** The collect is legal wherever it runs; what faults is the Python plugin's *own* pre-GC hook walking its wrapper state while your Python frame is still live on the same thread. Moving where the script starts leaves the frame exactly where it was.

It is non-deterministic. The first call in a script can return normally and the second is fatal, so "it worked when I tried it" is not evidence — it is the failure mode.

The same mechanism is reachable from anything that forces a synchronous collect mid-script: `SystemLibrary.collect_garbage`, an `obj gc` console command, a level teardown. Only `recompile_material` has a death recorded against it, so treat the rest as unproven rather than safe and prefer a typed RPC wherever one exists. `UMaterial.post_edit_change()` is not an escape either — it does not exist on UE 5.8.

**Touching a PIE object from a script arms the NEXT map load, long after the script has finished.** The interpreter's wrapper metadata keeps every `UObject` a script has ever wrapped, so once PIE ends its world is dead, still reachable, and the following `editor.open_level` / `level.load` / `level.create` walks into the engine's post-cleanse leak check (`CheckForWorldGCLeaks`), which logs `World Memory Leaks` at **Fatal by default** and kills the process. `scope: "private"` does not protect: it restores `sys.modules`, and the wrapper metadata is per object, not per module. The map-swap verbs now purge those wrappers and re-collect before every swap, so the usual outcome is that the load simply succeeds; when a world still survives that purge they refuse with `DIRTY_WORLD_BLOCKS_MAP_SWAP` and `survivingWorlds[]` instead of killing the process (see [`level`](level.md) → `level.load`). Rebinding the name (`actor = None`) and letting the script end is what avoids arming it in the first place; a world that survives the purge needs the editor restarted. The severity is the engine cvar `Editor.CheckForWorldGCLeaksAreFatal` (default `true`); setting it `false` downgrades the kill to a logged error, which lets a session finish a swap it otherwise cannot make but leaves the world leaked — an operator escape hatch, not a fix.

## Calls that freeze the editor

Worse than a crash, because there is no error, no stack, and no recovery — the editor is simply gone until someone kills the process, and everything unsaved goes with it.

`python.execute` runs **synchronously on the game thread**. Any long or unbounded operation holds that thread, and the game thread is the only thing that can draw a frame, answer `system.job_status`, or act on `system.job_cancel`. A wedged script cannot be stopped — the call you would reach for needs the thread your script is holding.

It *can* report progress, but only if it is written to. See "Reporting progress from a long script" below: a script that calls back between iterations is observable; one that does not, or that is inside a single long engine call, is exactly as invisible as it always was.

Recorded incident, with numbers, because the shape is worth recognising: a script called `get_components_by_class()` on **every actor, three passes** — about 1660 actors, ~5100 calls at roughly one second each. It held the editor **168+ minutes** before being abandoned. There was no progress output, no way to cancel, and no way to check anything: even `editor.list_dirty_packages`, the one call that would have said whether the work so far was worth saving, needs the very game thread the script was holding.

**The safe-point gate does not prevent this.** That gate only guarantees your script *starts* outside `UWorld::Tick`; the core ticker it defers onto is the same game thread. It changes where a wedge begins, not whether one happens.

**Prefer a typed RPC that sweeps in C++.** This is not a marginal saving. The 168-minute sweep above was eventually answered by three typed calls in about one second:

| the question | the call |
| --- | --- |
| what is this level made of? | `system.inspect.list_actor_classes` — a `{class, count}` census of the whole world in **one call**. Usually the right first move. |
| which actors are of class X? | `actor.find_by_class` — iterates the world in C++, returns label + path |
| what actors are here, filtered? | `actor.list` — `filter` / `limit` / `namesOnly`, and `totalMatches` keeps reporting the untruncated count |
| which actors carry tag X? | `actor.find_by_tag`, or `system.inspect.list_actor_tags` for a `{tag, count}` census |
| what components does *this* actor have? | `actor.get_components` — per actor, not a sweep |
| read-only audit of any UObject | `system.inspect.find_by_class`, `system.inspect.list_objects` |

If you are about to write `for actor in ...: actor.get_components_by_class(...)`, one of the rows above almost certainly answers the same question without touching Python.

**Or answer it without the editor at all.** "Does this level contain PCG / foliage / Niagara?" is answerable by searching the `.umap` package on disk — `grep -a` over the file finds the `/Script/PCG`, `/Script/Foliage`, `/Script/Niagara` references directly. That is how the 168-minute question was actually settled, and a later live check agreed with it exactly. No game thread, so it cannot wedge anything, and it works on levels that are not even open.

**If a Python loop is genuinely unavoidable**, bound it before you run it. Count first with a cheap typed read and decide whether that count is one you are willing to block on. Batch the work across several `python.execute` calls with an assertion between them, so a stall is attributable to one batch instead of the whole job. Then report progress from inside the loop (next section) — logging does not work, because nothing reads the log until the call returns, but reporting does.

## Reporting progress from a long script

A script can push live progress to its caller, and a long one should. Call this between units of work:

```python
import unreal
lib = unreal.PinWrightProgressLibrary
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
for i, a in enumerate(actors):
    lib.report_progress("actor %d of %d" % (i + 1, len(actors)), i + 1, len(actors))
    ...                                     # your per-actor work
```

Each call becomes an MCP `notifications/progress` frame on the caller's open stream, delivered *while the script is still running* — the game thread only queues the frame, and the transport's own I/O thread writes it to the socket, so the script need not return for the caller to see it.

**Call it every iteration — you do not need to report only every Nth item.** Reports are rate-limited to one per `Min progress event interval (ms)` (default 1000), so the loop above is correct as written; without that limit, a fast sweep could flood the client with notifications.

Delivery needs a caller that opened a stream (`params._meta.progressToken` + `Accept: text/event-stream`, without `args: {wait: false}`). Otherwise `report_progress` returns `False` and does nothing. `False` is not an error and is the *normal* return inside a fast loop — it also means "rate-limited" — so never branch on it or stop reporting because of it. It never raises, so a script that calls it unconditionally behaves identically watched or unwatched. Pass a total when you know one: `report_progress(msg, i, n)` lets a client draw a bar, while `report_progress(msg, i, 0)` omits the denominator rather than claiming zero. `is_progress_observed()` can skip building an expensive message, never the work.

Reporting also keeps the stream alive: a stream silent for 120 s is abandoned by the stdio proxy and the caller drops back to ticket polling, while each frame refreshes the deadline.

**The limit, stated exactly: this reports progress *between* units of work, never inside one.** Control has to come back to your loop for the call to happen. A single long engine call — `EditorAssetLibrary.rename_directory` over a 237-asset folder, one mesh build, one package save — hands nothing back until it finishes, so nothing is emitted for its whole duration. That is why the Content Browser can show a progress bar where an RPC cannot: it is *inside* the operation, driving it item by item. Split such work into per-item calls yourself if you need to watch it move.

**It does not make anything cancellable.** Progress is one-way; nothing here can interrupt a running script.

**What has improved: a hang is now detectable.** The socket I/O thread — the only thread still running — reports the age of a game-thread liveness heartbeat. Past 90 s, `ping` returns `error: "EDITOR_GAME_THREAD_STALLED"` with `retryable: true`, `stalledSeconds`, and `inFlightMethod` / `inFlightRequestId` naming the exact RPC the thread is stuck inside. It never gates `tools/call`, because the work may still finish. This is the single most useful thing on this page: it is the difference between losing three hours and losing ninety seconds. **It is diagnosis, not recovery** — nothing can interrupt a wedged game thread.

The sibling failure is a modal dialog, which deadlocks every RPC client the same way and reports `EDITOR_BLOCKED_ON_MODAL` with `retryable: false` — polling can never clear that one. Both probes and the full contract are under "Modal dialogs" on `call("system")`.

**`python.execute` runs at a safe point, not inside the world tick.** Since the safe-point gate covers it, a call that arrives while a world is mid-`UWorld::Tick` is re-queued and runs from the editor's core ticker instead — at most one 0.1 s pass later, with the response, job ticket and error path unchanged. You do not need to do anything about this; it exists so a script that swaps maps or pumps Slate cannot trip `Assertion failed: !LevelList.Contains(TickTaskLevel)`. It does **not** help with either hazard above: both are properties of what is on the stack and how long it stays there, not of where the stack started.

Neither section is an enforcement gate: these calls are not refused.

## Callbacks a script leaves behind

**A tick or shutdown callback registered inside `python.execute` outlives the call.** `unreal.register_slate_post_tick_callback(fn)` returns a handle, and `unreal.unregister_slate_post_tick_callback(handle)` is the only way to remove it. The handle lives in the script's namespace, which is discarded when the call returns — so the handle is gone and the callback is not. It keeps running after the script ends, after **PIE ends**, and through every later request in an editor other agents are working in.

Recorded incident: a callback named `watch`, registered from one inline script, captured a PIE actor and threw `Actor: Internal Error - ObjectInstance is null!` 0.2 s after `UWorld::CleanupWorld` — then **94,358 times over 93 minutes**, across four other streams' slots, with no supported way for any of them to find or stop it.

**Three things now make that visible and recoverable:**

| | |
|---|---|
| `python.execute` response | `leakedCallbacks` — how many callbacks this call left registered, plus a `Warning` in `log` when it is nonzero |
| `python.callbacks {action: "list"}` | every tracked callback: id, kind, registering slot, source line, registration time, invocation count, last traceback |
| `python.callbacks {action: "clear", ids: [...]}` | unregisters them for real, verified by re-reading the registry |

An **EndPIE warning** in the `LogPinWrightPythonCallbacks` category names every callback that survived PIE teardown, at the moment it starts operating on a dead world.

**A private call that leaks keeps its script.** `scope: "private"` runs an inline script from a temp file under `Intermediate/PinWright/Python/`, and that path is what every leaked callback reports as its `source`. The file is normally deleted when the call returns — which is why the recorded incident's 94,358 log lines named an `InlinePython_<guid>.py` that no longer existed. It is now kept whenever `leakedCallbacks` is nonzero, and the response names it in `retainedScript`. Nothing deletes it for you: remove it once the callbacks are cleared.

**What the tracker cannot see, stated exactly.** The engine keeps its Python handle lists as file-scope statics in a Private module (`PySlateUtil::PythonPostTickCallbackHandles`), with no accessor, so nothing can enumerate them. PinWright instead wraps `unreal.register_slate_*_tick_callback` / `register_python_shutdown_callback` at the moment they are called, which means:

- Callbacks registered **before** the first `python.execute` or `python.callbacks` of the session (an editor startup script, another plugin's tool) are invisible and unclearable. `list` reports `trackingInstalledAt` so you can tell "nothing is registered" from "nothing since tracking started".
- **World timers** (`unreal.SystemLibrary.set_timer_delegate` and friends) are not tracked. They die with their world, which is what makes them a different problem.
- A callback registered from C++ or from a non-Python source is not in scope and is not affected by `clear`.

**Write scripts that do not need any of this.** Keep the handle in a module you can `import` again so a later call can reach its `stop()`, unregister in a `finally`, have the callback unregister itself on its first exception, and never register from a script that ends before PIE does.

### python.execute

Pass `code` as either inline Python source or an absolute path to a `.py` file. Inline code is convenient for one-shots; file execution is better when the script is large or you want to version it. `mode` defaults to `execute_file`; use `execute_statement` for one statement and `evaluate_statement` when you need the expression result. `scope` defaults to `private`; `public` shares the UE Python console globals across calls.

Private scope is enforced for `execute_file`, including inline scripts. The handler routes private inline scripts through a temporary `.py` file because UE only honors `EPythonFileExecutionScope::Private` in its file-execution path. Avoid binding live PIE objects (`World`, `GameInstance`, actors, widgets) in `execute_statement` or `evaluate_statement` calls unless `scope: "public"` persistence is intentional.

**A private call also restores `sys.modules`, so a helper module you edited on disk is re-imported.** UE's own `Private` scope covers only the globals dict handed to the entry script; `sys.modules` belongs to the embedded interpreter and is process-global, so before this the second call ran the **new** entry script against the **old** helper — reporting `success: true`, with a traceback that quoted the current file's source text under the cached code object's line numbers and therefore named the wrong function. The handler now snapshots the module table before the script and restores it afterwards: modules the script imported are dropped, modules it replaced or deleted are put back. Two consequences to plan for:

- **A private call re-imports what it imports, every time.** For a script that pulls in something heavy, or one whose helper registers an editor delegate at import time, pass `scope: "public"` — that path deliberately keeps the interpreter's module cache.
- **`sys.path` is not restored**, only `sys.modules`. A script that inserts a directory should remove it in a `finally`.

If the scrub itself fails, the response's `log` array carries a `Warning` entry saying so and naming `importlib.reload` as the manual remedy — the one case where the old cached-import behaviour still applies. A script that must not depend on the handler at all can always force its own reload:

```python
import gen_common as G
import importlib
importlib.reload(G)
```

The interpreter runs synchronously on the game thread. Long-running scripts will stall the editor — don't block on network I/O or long sleeps. If you need async, offload via `unreal.LogPython.log` and return early, then poll a state file. A wedged script cannot be cancelled by anything; after 90 s `ping` reports `EDITOR_GAME_THREAD_STALLED` naming this verb. Bound the loop rather than relying on that.

A long script should report progress with `unreal.PinWrightProgressLibrary.report_progress(message, done, total)` — the frames reach the caller live, while the script runs. It is opt-in on both ends: nothing is emitted unless the caller opened a stream, and nothing is emitted unless the script calls it. Full contract, including the requirements and the exact limit, under "Reporting progress from a long script" on `call("python")`.

**Streaming changes the response envelope, not the response.** For a streaming caller this verb allocates a job ticket so progress has something to be addressed to, and the terminal SSE frame carries the identical `{success, result, log, pieActive?}` body a plain-JSON caller receives. Progress reporting is the only difference; `args: {wait: false}` opts out and takes the plain path.

**When PIE is active, the response carries `pieActive: true` and a `Warning` entry in `log`.** UE editor-scripting methods can refuse work by returning ordinary-looking sentinel values without making the Python command fail. In UE 5.8, `StaticMeshEditorSubsystem.get_lod_count` returns `-1` and `get_num_uv_channels` returns `0` behind this guard. Treat those values as unverified, use a typed PinWright read when one exists, or retry after PIE ends. This annotation applies to every `python.execute` mode, including `execute_file`; there is no separate `python.execute_file` verb.

**Nothing here is enforced.** No call is refused, no loop is bounded, no timeout fires. Read the "Calls that crash the editor" and "Calls that freeze the editor" sections of `call("python")` before writing a script that touches materials, sweeps actors, or loops at all — that page is the only protection there is.

The call is routed through the plugin's safe point, so it never begins executing inside `UWorld::Tick`. Cost is at most one 0.1 s core-ticker pass, and only when a world happens to be ticking; the response contract is unchanged. This prevents a script that swaps maps from tripping a tick-position assert. It does **not** prevent either hazard above.

The response uses the RPC success envelope even when Python code fails. Check the returned Python `success`, `result`, and `log` fields instead of treating transport success as script success.

**The response also carries `leakedCallbacks`: how many editor callbacks this call left registered.** A nonzero value means a tick or shutdown callback is still running with nobody holding its handle — clear it with `call("python.callbacks")` before releasing the editor. The field is present whenever tracking is installed; when it could not be installed the `log` array says so instead, so an absent field never reads as zero. A leaking `private` call additionally reports `retainedScript`, the temp file it kept so the leaked callbacks' `source` line still resolves; delete it yourself once they are cleared. Full contract under "Callbacks a script leaves behind" on `call("python")`.

For UMG tree edits, prefer typed `call("widget")` handlers before Python. UE Python blocks some protected/editor-only widget surfaces: `UWidgetTree.RootWidget` cannot be assigned from Python, and `UWidgetBlueprint.WidgetTree` may not be readable directly. Use `call("widget.replace_class")` for root swaps.

Avoid detaching UMG children unless the same script reattaches them before returning. A Python `remove_child` can leave a widget orphaned with no parent; later `call("widget.reparent_widget")` searches from the root and will report `NOT_FOUND`. If you must use Python for tree surgery, perform the full detach-attach sequence in one script so failures do not leave unreachable widgets.

**Modes:**

- `execute_file` (default) — treats `code` as a file path or multi-line script. Best for `.py` files or multi-statement scripts.
- `execute_statement` — runs `code` as a single Python statement (e.g. `import unreal; unreal.log("hi")`).
- `evaluate_statement` — evaluates `code` as a Python expression and returns its value in `result` (e.g. `2 + 2` returns `"4"`).

**Scope:**

- `private` (default) — `execute_file` calls get a fresh namespace, and `sys.modules` is restored after the call, so an edited helper module is re-imported on the next one. Inline private scripts do not persist top-level variables in UE console globals.
- `public` — shares the UE Python console's global namespace, so variables persist across calls, and the interpreter's module cache is left alone (imports stay cached).

**Auto-enable:** if `PythonScriptPlugin` is loaded but not yet initialized, the handler calls `ForceEnablePythonAtRuntime()` on first use. No manual enable step.

**Build dependency:** header-only include of `IPythonScriptPlugin.h` via `PrivateIncludePathModuleNames` — no linker dependency on `PythonScriptPlugin`.

**Examples:**

```
# Run a Python file:
call("python.execute", { code: "/Game/Scripts/my_tool.py" })

# Evaluate an expression:
call("python.execute", {
  code: "len(unreal.EditorAssetLibrary.list_assets('/Game/MyGame/'))",
  mode: "evaluate_statement"
})

# Multi-statement script with shared state across calls:
call("python.execute", { code: "import unreal; MY_VAR = 42",
                          mode: "execute_statement", scope: "public" })
call("python.execute", { code: "MY_VAR * 2",
                          mode: "evaluate_statement", scope: "public" })
# -> result: "84"
```

### python.callbacks

Lists and clears the editor callbacks Python scripts registered — the ones that outlive the `python.execute` call that created them because the handle died with the script's namespace. Read "Callbacks a script leaves behind" on `call("python")` for why the leak happens and what the tracker cannot see.

`action` defaults to `list`, which is read-only and takes no other argument.

```
call("python.callbacks", { action: "list" })
```

```json
{
  "action": "list",
  "trackingInstalledAt": "2026-09-09T11:02:14Z",
  "count": 1,
  "callbacks": [
    { "id": "pw-cb-3",
      "kind": "slate_post_tick",
      "slot": "req-8f21",
      "source": "…/InlinePython_A46C7846.py:12 in <module>",
      "registeredAt": "2026-09-09T11:04:51Z",
      "invocations": 94358,
      "lastError": "Traceback (most recent call last):\n…ObjectInstance is null!" }
  ]
}
```

`kind` is `slate_post_tick`, `slate_pre_tick` or `python_shutdown`. `slot` is the RPC request that registered it, empty when it was registered outside a tracked call. `invocations` and `lastError` are what separate a healthy background callback from one throwing 30 times a second — a large count beside a non-empty `lastError` is the shape that costs an editor.

**Clearing requires you to say what to clear.** `ids` names them; `all: true` takes everything tracked. Neither is implied by omitting the other, because in a shared editor "everything" includes other people's callbacks.

```
call("python.callbacks", { action: "clear", ids: ["pw-cb-3"] })
call("python.callbacks", { action: "clear", all: true })
```

```json
{ "action": "clear", "trackingInstalledAt": "…",
  "cleared": ["pw-cb-3"], "failed": [], "notFound": [], "remaining": 0 }
```

The three id lists are **measured, not claimed**: the verb snapshots the registry, asks the Python shim to unregister, then snapshots again. `cleared` is what actually disappeared, `failed` is what the unregister call did not remove, `notFound` is ids that were never tracked. `remaining` counts everything still registered afterwards, including callbacks you did not name.

Errors: `INVALID_PARAMS` for an unknown `action`, for `ids`/`all` on a `list` call, for both together, or for a `clear` with neither. `PYTHON_NOT_AVAILABLE` / `PYTHON_INIT_FAILED` when the interpreter is not usable. `PYTHON_CALLBACK_TRACKING_UNAVAILABLE` when the interpreter is up but the tracking shim would not install — nothing can be listed or cleared honestly in that state, and the verb says so rather than returning an empty list.

**A failed install is remembered, and this verb is what retries it.** The install runs a Python import, so retrying it from every `python.execute` would write a traceback per script on a host where it cannot work; `python.execute` therefore reuses the remembered failure and only says so in its `log`. Calling `python.callbacks` forces one fresh attempt, so fixing the cause does not cost an editor restart — which is the outcome this whole feature exists to remove.
