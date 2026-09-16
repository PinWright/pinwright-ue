# Running PinWright unattended

Unattended operation is any session where no human is watching the editor window: CI, cron, an overnight agent run, or simply an agent driving an editor nobody is looking at. The failure mode that defines it is the modal dialog, because a modal is the one editor state no RPC can clear.

## Why modal dialogs are fatal

Every PinWright RPC executes on the game thread. While a modal is up, that thread sits in a nested Slate loop (`SlateApplication.cpp:2238`) that pumps Slate and nothing else — no `FTSTicker`, `FEngineLoop::Tick`, or `UEditorEngine::Tick` — so the request cannot dismiss it. The 0.1 s readiness ticker keeps publishing its last positive snapshot, the completion-timeout sweep cannot fire the server's 120 s / 300 s timeouts, and the deferred dispatch queue cannot drain.

The client therefore sees: connection accepted, request read, then silence until *its own* timeout. That is the state an agent cannot distinguish from a crash, and polling makes it worse — a well-behaved client retrying a retryable `EDITOR_NOT_READY` will burn its entire watchdog while a human never learns a dialog is waiting.

## Launching for automation

An automated or unattended launch should also be wrapped in the capped launcher (`scripts/Run-Capped.ps1`, or `scripts/Run-SuiteCapped.ps1` for the automation suite) so the editor and its children run under a per-process memory cap, kill-on-close and BelowNormal priority; a visible session a human is working in is launched normally, uncapped and at normal priority.

`-AutoDeclinePackageRecovery` suppresses the boot-time "Restore Packages" auto-save recovery prompt, and nothing else — its only two references in the whole engine are the `FPackageAutoSaver` constructor and the one branch that consumes it. It behaves exactly as clicking "Don't restore": only the restore *manifest* is discarded, and the auto-saved `.uasset` files under `Saved/Autosaves/` stay on disk for manual recovery. Nothing an agent authored and saved is affected — only unsaved-at-crash-time work, which an unattended run cannot triage anyway.

The `editor_start` MCP tool passes this flag on **every** launch, visible and windowless, because it is the one dialog that fires before PinWright's ticker has run even once. Pass it by hand to an editor you start yourself, and to any `UnrealEditor-Cmd.exe` invocation.

`-RunningUnattendedScript` sets `GIsRunningUnattendedScript`, suppresses `FMessageDialog`, and cancels raw Slate modals from process start, including the pre-first-RPC window nothing else can cover. `editor_start` passes it on **every windowless launch** (`visible: false`) and on a **visible** launch only with `unattended_script: true` or a `map` (an agent-driven boot). It auto-answers engine defaults, so do not use it on a visible editor shared with a human; pass `visible: true` when dialogs should open. A windowless editor cannot opt out: `unattended_script: false` would assemble `-unattended` alone, which is worse (see below). This flag also forces a direct spawn because the `open` verb takes no command line.
`editor_prepare_tests` is the only test-related command-returning proxy verb. It is an instant planner: `filter` is mandatory and must be non-empty; the live PinWright guard runs first and reports `EDITOR_ALREADY_RUNNING` for a detected editor or `not_probed` when the probe is unavailable. An unavailable probe never proves the editor is stopped. The planner resolves the project's `EngineAssociation` to the matching `UnrealEditor-Cmd` executable and returns `COMMAND_READY` with the launch executable and `argv`, an explicit absolute `logPath`, and the checker executable and `argv`. It does not spawn or own the test process, wait, kill, apply test-run timeouts or watchdogs, or classify a verdict.
The caller runs the returned launch executable with its returned `argv`, then runs `check_suite_log.py` using the returned checker executable and `argv` with the exact same `logPath`. The command shape is `-ExecCmds="Automation RunTests <filter>,Quit"`, `-TestExit="Automation Test Queue Empty"`, `-Abslog=<same absolute logPath>`, plus `-unattended`, `-RunningUnattendedScript`, `-nopause`, `-nocefaccelpaint`, `-ddc=InstalledNoZenLocalFallback`, and `-log`; use a real RHI and never add `-NullRHI`.

When local Zen storage takes over 20 s to start — first launch or maintenance — the engine raises "Wait for ZenServer?" from `ZenServerInterface.cpp:2400`, guarded only by `!(FApp::IsUnattended() || IsRunningCommandlet() || GIsRunningUnattendedScript)`. It is a native `FPlatformMisc::MessageBoxExt`, not a Slate modal, so neither the in-editor suppression scope nor the modal probe sees it; it fires before PinWright loads. On a visible launch `-RunningUnattendedScript` is the only lever because `-unattended` is excluded (a human may be at that window). A windowless launch carries `-unattended`, `-RunningUnattendedScript`, and a commandlet-free boot, so it is covered three times. Nothing can detect or dismiss it; the proxy can only report a readiness timeout and names this hazard when the flag was absent.

`-unattended` is for fully headless runs only, and `editor_start` adds it only when `visible: false`. **On its own it makes saving worse, not better**, which is why it never travels alone. `FEditorFileUtils::PromptForCheckoutAndSave` reads the two switches oppositely and in this order:

```
FileHelpers.cpp:4659   if (GIsRunningUnattendedScript)  -> SavePackages(...)   // saves, no prompt
FileHelpers.cpp:4664   if (FApp::IsUnattended() && !bAlreadyCheckedOut)
                           return PR_Cancelled;                                // saves NOTHING
```

With **both** switches set, the first branch wins and packages save silently. With **bare `-unattended`**, the second branch cancels and saves nothing; `editor.quit {save: true}` then fails with `SAVE_FAILED` and refuses to exit. `_HEADLESS_FLAGS` always supplies the pair, so this combination is unreachable from `editor_start`. Never add `-unattended` to a visible editor or without `-RunningUnattendedScript`. When no PIE session is active, `editor.save_all` saves per package through `UEditorAssetLibrary::SaveAsset` → `UEditorLoadingAndSavingUtils::SavePackages`, bypasses `PromptForCheckoutAndSave`, and forces `GIsRunningUnattendedScript` (`FileHelpers.cpp:5919`); with PIE active it refuses before the first package with `PIE_ACTIVE`. Engine save-on-exit is also unreachable on any `-unattended` editor: `FMainFrameHandler::CanCloseEditor` returns `true` at `MainFrameHandler.h:117` before `SaveDirtyPackages`, so `editor.quit` saves explicitly. A save failure under `GIsRunningUnattendedScript` emits no dialog, `Error:`, or `Warning:` — only Log-verbosity `Message dialog closed, …`; `InternalSavePackages` converts `PR_Cancelled`, not `PR_Failure`, to `false`. A caller can therefore be told every package saved, which is why `editor.save_all` verifies each package instead of trusting one aggregate bool.

## Starting into a specific map

`editor_start` and `editor_restart` take `map: "/Game/Maps/MyLevel"` (a bare short name or `.umap` path also works) and boot straight into it. The map loads during startup instead of tearing down an already-open world.

Place the token as the **first** argument after the `.uproject`, before every switch: `FUnrealEdMisc::OnInit` reads only that position and skips the map load when it starts with `-` (`UnrealEdMisc.cpp:396-399`). A map in `extra_args` comes after the switches and is silently ignored, so `map` has its own parameter. Because file association carries no command line, `map` forces a direct spawn and defaults `unattended_script` to **true** so the ZenServer-class modal cannot wedge boot; visible launches can opt out with `unattended_script: false`, windowless ones cannot. Empty, whitespace-bearing, or `-`-prefixed tokens are rejected with `INVALID_MAP`.

`editor_restart` exists as one verb rather than a documented `editor.quit` + `editor_start` recipe because the seam between the halves is not callable from outside: `editor_start` rejects a spawn with `EDITOR_ALREADY_RUNNING` for as long as the outgoing editor is still answering, so a hand-sequenced restart races the shutdown. It quits through the editor's own `editor.quit` RPC, waits for the endpoint to stop answering, then runs the normal start path. A dirty editor with neither `save` nor `discard` set refuses, and that refusal is relayed verbatim as `EDITOR_QUIT_REFUSED` rather than force-discarded — a restart must never be a quiet way to lose work. The same relay carries `EDITOR_IN_USE`, which `editor.quit` raises when a **different** client has driven that editor recently. The proxy's own calls never trigger it (they carry this proxy process's client id, and the editor excludes the caller's own traffic), so seeing it means another agent is mid-session on that editor — and `force` is deliberately not passed for it, because restarting out from under another agent is not this verb's call to make. A modal-blocked or unresponsive editor is reported, not bypassed: `editor.quit` runs on the game thread the dialog owns, so it cannot land, and starting a second editor beside a wedged one would be worse than failing. A shutdown that never completes is `EDITOR_STOP_TIMEOUT`; nothing is killed and nothing is respawned.

To swap the active world in a *running* editor instead, use `level.load` — it defers the swap to a safe point rather than tearing the world down mid-frame; see [`level`](level.md).

## Settings

`Editor Preferences > Plugins > PinWright > Unattended`:

- `Suppress modal dialogs during RPCs` (default on) — scopes `GIsRunningUnattendedScript` over every dispatched handler, so engine confirmation dialogs auto-answer instead of owning the game thread. This covers the synchronous handler body. A handler that schedules a deferred continuation (`AddTicker` / `SetTimer` / `AsyncTask(GameThread)`) reaching a dialog-capable API must hold its own scope across that continuation.
- `Decline auto-save recovery prompt at startup` (default on) — calls `IPackageAutoSaver::DisableRestorePromptAndDeclinePackageRecovery()` from `OnPostEngineInit`, the only plugin-reachable callback between the auto-saver's construction and the recovery point. This protects editors launched from the Epic Launcher with no PinWright flags. Turn it off to get the prompt back on an editor you drive by hand.
- `Modal blocked report threshold (seconds)` (default 2.0) — how long the game thread must stay blocked before the transport reports `EDITOR_BLOCKED_ON_MODAL`. Below the threshold every response is byte-identical to the unblocked case, so a human dismissing a dialog quickly never produces a spurious failure.
- `Game thread stall report threshold (seconds)` (default 90.0) — how long the game thread must go without completing a tick before `ping` reports `EDITOR_GAME_THREAD_STALLED`. Raise it on projects that routinely do long synchronous game-thread work; a threshold that fires on a healthy-but-busy editor is worse than no probe. `0` disables it.

## Create verbs and existing assets

Suppressing the flag is not enough for asset creation. `IAssetTools::CreateAsset` funnels through `UAssetToolsImpl::CanCreateAsset`, which on finding a live object at the target path raises three modals in a row — the "Overwrite Existing Object" prompt, `ObjectTools`' reference-check prompt, then the "referenced by other content" notice. On UE 5.5 and 5.6 the first of those is a raw `SMessageDialog::ShowModal()` with **no** `CanShowDialogs()` guard (`AssetTools.cpp:4606`), so `GIsRunningUnattendedScript` does not reach it. The only portable fix is never to let an existing asset survive into `CanCreateAsset`.

So the `create_*` verbs resolve the target path themselves, before any engine call, and never prompt:

- **Nothing there** → created as before. Response carries `existing: false`, `mode: "created"`.
- **Same class already there** → returned and updated in place, keeping its referencers. Response carries `existing: true`, `mode: "updated_in_place"`. This is what makes a re-run idempotent instead of a failure.
- **A different asset class there** → `ASSET_ALREADY_EXISTS`, naming the class actually occupying the path. Never replaced, whatever `overwrite` says.
- **`overwrite: true` and something references it** → `ASSET_IN_USE`. Error data carries two pairs: `referencers` / `referencerCount` for on-disk packages the asset registry indexed (no delete is attempted in that case, so nothing is orphaned), and `referencingActors` / `referencingActorCount` for **live level actors** holding it in memory — the case the registry cannot see, because an unsaved level reference is not on disk. Both lists are capped at 25 with uncapped counts. The fix is to drop `overwrite`: the re-run then updates in place and keeps every reference. **`model.compile` and `geometry.convert_to_static_mesh` never produce this code** — they resolve with `overwrite` forced off and read their own flag as permission at the provenance gate instead, so a StaticMesh occupant is always rebuilt in place and referencers cannot refuse a compile.

`overwrite` (default `false`) is the opt-in for the old wipe-and-recreate behaviour. The lookup is `StaticFindObject` first, then an on-disk probe — a registry-only existence check misses an asset created earlier in the same session, which is exactly the object that reached the prompt.

Converted so far: `material.authoring.create_material`, `blueprint.create`, `behavior_tree.create_task_blueprint` / `create_service_blueprint` / `create_decorator_blueprint`, `animation.create_animation_bp` / `create_blend_space` / `create_animation_asset`, `data_table.create`, `input.create_input_action` / `create_input_mapping_context`, `sequencer.create`. Any new handler calling `CreateAsset` / `DuplicateAsset` / `RenameAssets` must go through the same policy.

## Redirector fix-up, and the modal that kills rather than blocks

Suppression cancels a modal; it does not answer one. `FMessageDialog::Open` copes because it returns a documented `DefaultValue`, but UE 5.8's `SModalEditorDialog<T>::ShowModalDialog` reads its answer with an unchecked `TOptional::GetValue()` (`Dialogs.h:280`). Cancel the window and that read hard-asserts at `Optional.h:372` — the editor **dies**, and the suppression is what turned a hang into a crash.

Engine-wide there is exactly one such dialog, `SFixupRedirectorsReport` (`AssetFixUpRedirectors.cpp:214`), and `FAssetFixUpRedirectors::ExecuteFixUp` shows it unconditionally at `:938-939`. Neither `ERedirectFixupMode` nor `bCheckoutDialogPrompt` avoids it, so `IAssetTools::FixupReferencers` — the only public redirector fix-up API — has **no** automation-safe mode.

`asset.fixup_redirectors` and `asset.bulk_delete` therefore drive the fix-up themselves (registry referencers → load → `RenameReferencingSoftObjectPaths` → save → delete), and both report what actually happened: `redirectorsConsidered`, `redirectorsDeleted`, `referencingPackagesFound` / `Saved`, plus `failedPackages[]` and `codeReferences[]`. A redirector survives whenever one of its referencers did not re-save — see [asset.fixup_redirectors](asset.md#assetfixup_redirectors).

## When the editor is blocked anyway

`EDITOR_BLOCKED_ON_MODAL` means a modal dialog owns the game thread right now. It is reported by the socket I/O thread, which is the only reason it can be reported at all — the probe latches from inside the nested Slate loop and clears from the subsystem tick, which by definition can only run once the modal is gone.

**It is not retryable.** That is the whole point of it being a distinct code rather than a flavour of `EDITOR_NOT_READY`: retrying is exactly the behaviour that burns the watchdog. `ping` answers it, and every `tools/call` fails in milliseconds instead of queueing behind a dispatcher that cannot run:

```jsonc
{
  "editorReady": false,
  "retryable": false,
  "error": "EDITOR_BLOCKED_ON_MODAL",
  "message": "The Unreal editor is blocked on a modal dialog titled \"Restore Packages\" and has been for 42 s. ...",
  "blockedOnModal": true,
  "blockedSeconds": 42.1,
  "modalTitle": "Restore Packages"
}
```

`modalTitle` is **omitted**, never guessed, when the active modal window could not be read. The three `blocked*` fields are absent entirely when the editor is not blocked, so existing parsers are unaffected.

The only remedies are a human dismissing the dialog in the editor window, or killing the process. Requests already in flight when the modal opened stay stuck: the completion-timeout sweep that would have failed them runs on the blocked thread too. What this reporting buys is that a *second* connection gets a truthful answer, so an agent can report rather than guess.

## When the game thread is wedged

A modal is the *rare* way to lose the game thread. The common one is a handler that simply does not return — a `python.execute` script looping over every actor, a synchronous engine call that never comes back. Nothing broadcasts anything while that happens, so the modal probe stays silent and, before this existed, `ping` answered `editorReady: true` for the entire hang. An agent could not tell it from a crashed process.

`EDITOR_GAME_THREAD_STALLED` closes that. The subsystem tick stamps a heartbeat while the thread is demonstrably healthy; the socket I/O thread reports the stamp's *age*. Nothing has to fire during the wedge — the absence is the signal.

```jsonc
{
  "editorReady": false,
  "retryable": true,
  "error": "EDITOR_GAME_THREAD_STALLED",
  "message": "The Unreal editor's game thread has not completed a tick for 412 s. ...",
  "gameThreadStalled": true,
  "stalledSeconds": 412.0,
  "inFlightMethod": "python.execute",
  "inFlightRequestId": "9f2c-...-a1",
  "inFlightSeconds": 411.0
}
```

Three differences from the modal case, all deliberate:

- **Retryable.** A wedged handler may still return, and the queued request runs when it does. Unlike a dialog, waiting can work.
- **`ping` only — it never gates `tools/call`.** A new call is still handed to the dispatcher. Failing it would break every legitimately long game-thread operation, and the stall threshold is a heuristic, not proof.
- **The threshold is high (90 s default).** Multi-second game-thread stalls are routine — map loads, package saves, synchronous asset compiles — and legitimate stalls of ~54 s have been observed on a loaded machine. A probe that cries wolf on a busy editor is worse than none.

`inFlightMethod` / `inFlightRequestId` / `inFlightSeconds` are **omitted**, never emitted empty, when the thread stalled outside any dispatch — that case means the stall is engine-internal work, not a PinWright call, and the message says so. All `gameThreadStalled*` fields are absent entirely below the threshold, so existing parsers are unaffected.

**This is diagnosis, not recovery.** PinWright cannot interrupt a wedged game thread: suspending it from outside deadlocks on any lock it holds, and killing it corrupts the editor. What the report buys is knowing *when* and *what* — the difference between an unexplained 168-minute hang and a 90-second verdict naming the verb responsible.

## When the editor is alive but never bound its port

A distinct failure from a wedge: the process is running and healthy, but it never got the MCP listener. Measured shape — an editor died in the render thread, a replacement booted before the dead process released the socket, and the bind lost. Every probe against that editor is refused, so it reads as "editor down" while `tasklist` shows it running.

The bind is now retried from the subsystem ticker on a bounded backoff: **2 s doubling to a 30 s cap, for a total of 600 s**, so an editor that boots into a port the previous one is still releasing recovers on its own and logs `Transport bound port N on retry K`. Bounded deliberately — a port held by another project's editor, or inside a Windows excluded / Hyper-V reserved range, never clears, and an unbounded retry would hide that behind an infinite "still trying" instead of naming the fix. When the budget is spent the terminal error is **re-logged every 300 s**, because one line at startup is invisible to anyone who attaches to the log later.

**What the port file does while that is going on.** `Saved/PinWright/gateway-port` is how the stdio proxy finds the endpoint — it re-reads the file before *every* forwarded call — so a bind failure raises a second question: what is that file still telling callers? An editor that is not serving reconciles it rather than leaving it alone, and the rule is asymmetric on purpose:

- **Something is listening on the advertised port** → the file is left exactly as it is, logged at Warning. This is the normal two-editor case: the reason *this* editor lost the bind is usually that another editor of the same project owns that port and is serving callers on it perfectly well. Deleting the advertisement there would break the editor that works.
- **Nothing is listening on it** → the file is **retracted**, logged at Error. The claim is false no matter who wrote it, and a proxy that finds no file reports "editor not running" — the truth — instead of dialling a port nobody owns.

Ownership cannot be known after a crash; liveness can be probed, so liveness is the test. Any inconclusive probe counts as *listening*, so an ambiguous answer never deletes a working endpoint. The judgement is re-made on the terminal re-log cadence too, because the process holding the port may exit after this editor gave up watching it.

A **bound** editor whose port could not be *written* to that file is the mirror-image failure and reads the same way to a caller — serving, but undiscoverable. It is logged at Error naming the path, and the write is retried every tick until it lands; it is never a silently discarded return value.

Recognising it:

- `Saved/Logs/*.log` — `LogSocketHttp: Error: Port N could not be bound` once, then `LogPinWrightSubsystem: Warning: Port N still held after K attempts`, then the repeating terminal error.
- In-editor — the setup screen's red **"MCP server is NOT running - port conflict"** banner, which now states the attempt count and whether the retries are spent.
- No RPC can report this. An editor that never bound answers nothing, so `ping` is refused exactly as it would be for a stopped editor. **The log and the setup screen are the only evidence.**
- `LogPortAdvertisement` — `Retracting a false MCP advertisement` (the file named a dead port and was removed) or the Warning that names the port something else is serving.

Do not answer a refused probe by starting another editor when one is already running for the project: the new process loses the bind the same way, and now two editors compete. `_probe_state` reports `not_running` — the only state that licenses a spawn — only for a *refused connection*; anything else that answered on the port is `unresponsive`.

## What still needs a human

- **Dialogs that fire before any plugin code exists.** PinWright's earliest reachable hook is `FCoreDelegates::OnPostEngineInit`. The module-compatibility prompts in `LaunchEngineLoop::PreInit` — "Target Upgrade Required", "the following modules are missing or built with a different engine version", "Engine modules cannot be compiled at runtime", "the `<X>.Target.cs` file does not exist" — use raw `FPlatformMisc::MessageBoxExt`, which consults neither `GIsRunningUnattendedScript` nor any delegate. Only `-unattended` helps, and only for two of the four, and it converts them into a silent nonzero exit rather than a diagnosis. Mitigation is operational: keep the project built and `*.Target.cs` current, and treat "editor process exited during boot" as a distinct, non-retryable outcome.
- **Suppressed is not the same as safely answered.** `FMessageDialog::Open` returns a documented default. A raw Slate modal (`SCustomDialog::ShowModal`, `SWindow::ShowModal`) is *cancelled* — no window, no deadlock — but then returns its uninitialised widget-local value. `ObjectTools.cpp:937`'s overwrite-vs-cancel choice is the sharp example. Delete, rename and duplicate verbs carry this exposure; re-read state after any mutating verb that could have hit one.
- **`editor.run_utility_blueprint` cannot be closed.** It executes user-authored Blueprint graphs. Dialog nodes backed by `FMessageDialog` are suppressed; a graph that reaches a raw Slate modal is beyond anything short of refusing to run the verb.
- **`python.execute` deferred callbacks.** The dispatch scope covers the synchronous script execution. A script that calls `unreal.register_slate_post_tick_callback` and opens a dialog from its own engine-side callback runs outside every scope PinWright owns. It is bounded only by the fact that the caller wrote the script.
- **`FScopedSlowTask` is not in this class.** A slow-task window sets `bSlowTaskWindow`, which both exempts it from the unattended cancel and keeps the game thread running (`SlateApplication.cpp:2217`). It cannot produce the symptom above; do not spend effort there.

## See also

- `call("system")` — the process- and engine-level namespace, including the long-running job queue.
- `call("mcp-transport")` — the full wire contract for `ping` and `tools/call`, including the modal-block fields.
