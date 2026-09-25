---
type: system
summary: "PinWright architecture: 7-layer in-editor MCP design plus the stdio proxy lifecycle boundary (call, editor_start, editor_prepare_tests), gated integration sub-modules (IntegrationGates, LoadingPhase None, v0.7.0 module split), auto-registration pipeline, FHandlerContext, FAsyncResponseToken, retained safe-point response paths, AtomicFileWriter output publication, Find-in-Blueprints task lifetime, recorder query/session invariants, state management, IR sidecar registry, shared JSON builders, and testing categories."
date: 2026-09-04
tags: [architecture, mcp, rpc, http, handlers, dispatcher, json-rpc, transport, stdio-proxy, editor-lifecycle, jobs, async, ir-sidecar-registry, json-builders, recorder, pagination, recorder-segments, integration-gates, sub-modules]
---

# Architecture: PinWright

## Layer Diagram

```
┌─────────────────────────────────────────────────────────────┐
│  MCP client adapter (direct HTTP or bundled stdio proxy)     │
│  Direct endpoint: `call`; stdio adds local lifecycle tools   │
└───────────────────────────┬─────────────────────────────────┘
                            │ internal HTTP POST /mcp
┌───────────────────────────▼─────────────────────────────────┐
│  Transport (SocketHttpServer + McpRequestCore)               │
│  - Custom socket HTTP/1.1 server (single I/O thread)         │
│  - POST /mcp only. No /health, no /rpc, no GET /mcp.         │
│  - Parses one JSON-RPC 2.0 envelope per request              │
│  - Dispatches the five MCP protocol methods:                 │
│      initialize, notifications/initialized, ping,            │
│      tools/list, tools/call                                  │
│  - tools/list returns one descriptor for the single `call`   │
│    tool; the dotted RPC name is a string value passed in     │
│    the `method` argument, not a tool name on the wire.       │
│  - tools/call routes by argument shape: omit args → wiki     │
│    page (via WikiHandler), present args → FRpcDispatcher     │
│    invoked with the dotted method + args object.             │
│  - Completion-based async: holds HTTP response open until    │
│    handler resolves or timeout fires                         │
│  - Oversized tools/call results spill to                     │
│    Saved/PinWright/HttpResponses and return a file    │
│    reference inside the tools/call result.                   │
│  - Timeout sweeping on tick                                  │
│  Files: Private/Transport/SocketHttpServer.h/.cpp,           │
│         Private/Transport/McpRequestCore.h/.cpp              │
├─────────────────────────────────────────────────────────────┤
│  Request Protocol (JsonRpc — JSON-RPC 2.0 envelope helpers)  │
│  - ParseEnvelope() → id, method, params, isNotification       │
│  - BuildResponse(Id, Result)                                 │
│  - BuildErrorResponse(Id, NumericCode, Message, Data?)       │
│  - Numeric codes: -32700/-32600/-32601/-32602/-32603         │
│  File: Private/JsonRpc.h                                     │
├──────────────┬──────────────────────────────────────────────┤
│  Dispatcher  │  Catalog + Wiki                               │
│  (RpcDispatcher)  │  (FToolCatalog, WikiHandler,             │
│                   │   FWikiOverlay)                          │
│  - TMap handler   │  - `FToolCatalog` caches the dispatcher  │
│    lookup O(1)    │    handle for the Wiki layer; no on-disk │
│  - Reentrancy     │    reference is generated.               │
│    guard +        │  - `WikiHandler::RenderPage(path)` walks │
│    deferred queue │    the namespace tree and renders        │
│  - GC/save        │    branch / leaf / method pages. It is   │
│    deferral       │    the discovery surface, reached        │
│  - Auto-validate  │    through the single `call` MCP tool    │
│    required       │    by passing `method=<path>` with no    │
│    params from    │    `args`.                               │
│    ParamSpec      │  - `FWikiOverlay` loads hand-authored    │
│  - FormatterReg.  │    overlay sources from                  │
│    : per-method   │    `docs/wiki-src/<path>.md` and merges  │
│    text formatter │    prelude + per-method H3 sections      │
│    map keyed by   │    into the auto-content.                │
│    method name;   │  - `WikiDiskGenerator` writes the full   │
│    populated by   │    assembled tree to                     │
│    REGISTER_RPC_  │    Saved/PinWright/wiki/ at editor       │
│    FORMATTER.     │    launch (source = docs/wiki-src/,      │
│                   │    output = Saved/PinWright/wiki/); the  │
│                   │    primary wiki response is an on-disk   │
│                   │    path ref, inline markdown is the      │
│                   │    cold-start / not-found fallback.      │
│                   │  - Direct in-editor `tools/list` returns │
│                   │    one `call` descriptor, built inline   │
│                   │    in the transport. No per-handler MCP  │
│                   │    tool descriptors and no               │
│                   │    dotted→underscore flattening.         │
│                   │  - The bundled stdio proxy adds local    │
│                   │    `editor_start` and                    │
|   editor_prepare_tests descriptors.                           |
│  File: Private/   │  Files: Private/Catalog/                 │
│    Dispatch/      │    ToolCatalog.h/.cpp                    │
│    RpcDispatcher  │    WikiHandler.h/.cpp                    │
│    .h/.cpp        │    WikiOverlay.h/.cpp                    │
│                   │    WikiDiskGenerator.h/.cpp              │
│                   │                                          │
├──────────────┴──────────────────────────────────────────────┤
│  Subsystem (UPinWrightSubsystem)            │
│  - UEditorSubsystem that owns Transport, Dispatcher, Catalog │
│  - Loads gated integration sub-modules (IntegrationGates)    │
│    before draining handler registrations                     │
│  - Wires delegates, starts HTTP, runs 0.1s ticker            │
│  - Pure orchestrator: no domain state                        │
│  Files: Public/PinWrightSubsystem.h         │
│         Private/PinWrightSubsystem.cpp      │
├─────────────────────────────────────────────────────────────┤
│  Handlers (~90 files, ~1,170 methods)                        │
│  - One .cpp per handler (or small group)                     │
│  - REGISTER_RPC_HANDLER macro → zero-wiring registration    │
│  - FHandlerContext for typed params, validation, response    │
│  - Organized by domain: Actor/, Blueprint/, Material/, UI/,  │
│    Animation/, Sequencer/, Niagara/, Render/, Spatial/, etc. │
│  - Engine-plugin-linked clusters live in five gated          │
│    LoadingPhase=None sub-modules (PinWrightGeometry,         │
│    PinWrightPCG, PinWrightChooser, PinWrightPoseSearch,      │
│    PinWrightCommonUI), loaded via IntegrationGates only      │
│    when the owning engine plugin is enabled                  │
│  Files: Private/Handlers/<Domain>/*.cpp,                     │
│         Source/PinWright<Integration>/Private/Handlers/      │
├─────────────────────────────────────────────────────────────┤
│  Jobs (FJobRegistry + FJobMonitorLog)                        │
│  - FJobRegistry: in-memory map of FJobTicket, owned by       │
│    FPluginState. Status: "running"|"completed"|"failed"|     │
│    "cancelled". TTL eviction on 0.1s subsystem ticker.       │
│    Progress events use a configurable interval (60s default);│
│    max 50 events/ticket with FIFO trim.                      │
│  - FJobMonitorLog: thread-safe append-only JSONL writer at   │
│    Saved/PinWright/jobs.jsonl. Size-based rotation        │
│    (default 10 MB, 3 keeps). File wiped at subsystem init.   │
│  - Ctx.StartJob(FJobBindArgs): allocates ticket → sends      │
│    immediate {status:"running", ticket_id, ...} response →   │
│    calls BindNativeDelegate. Response goes before delegate   │
│    so sync failure paths never race the HTTP response.       │
│  - Three RPCs: system.job_status, system.job_list,           │
│    system.job_cancel (JobControlHandler.cpp).                │
│  - Opt-in only. Auto-promotion at transport level was        │
│    rejected: fire-and-forget handlers respond before any     │
│    sweeper could fire, so it catches zero real cases.        │
│  Files: Private/State/JobRegistry.h                          │
│         Private/Utils/JobMonitorLog.h                        │
│         Private/Handlers/System/JobControlHandler.cpp        │
├─────────────────────────────────────────────────────────────┤
│  State (FPluginState singleton)                              │
│  - FBlueprintTracker: inflight ops, busy state, registry     │
│  - FSaveThrottler: prevents rapid repeated saves             │
│  - FJobRegistry: ticket/event registry (see Jobs layer)      │
│  - Sequencer/Niagara registries                              │
│  - Actor snapshot cache, log capture device                  │
│  - All access thread-safe via FCriticalSection               │
│  Files: Private/State/PluginState.h/.cpp                     │
│         Private/State/BlueprintTracker.h/.cpp                │
│         Private/State/SaveThrottler.h/.cpp                   │
├─────────────────────────────────────────────────────────────┤
│  Utilities                                                   │
│  - UE 5.3-5.8 compat: Misc/EngineVersionComparison.h        │
│  - PathUtils: path sanitization, validation                  │
│  - AssetUtils: loading, saving, throttled saves              │
│  - ClassUtils: multi-strategy class resolution (ResolveUClass)│
│  - JsonUtils: field extraction, vector/rotator parsing       │
│  - PropertyUtils: UProperty ↔ JSON reflection                │
│  - ActorUtils: actor find by name/label/path                 │
│  - LogUtils: RAII log capture                                │
│  Files: Private/Utils/*.h/.cpp                               │
└─────────────────────────────────────────────────────────────┘
```

## Request Flow

```
MCP client adapter POST /mcp
  JSON-RPC 2.0 envelope: {jsonrpc, id, method, params}
  → SocketHttpServer.HandleCompleteRequest() → McpRequestCore::ProcessRequestBody()
    → JsonRpc::ParseEnvelope() → id, method, params
    → Switch on protocol method:
        initialize                      → handshake reply
        notifications/initialized       → HTTP 202, no body
        ping                            → result:{}
        tools/list                      → one descriptor: the `call` tool
        tools/call                      → continue below
        anything else                   → error -32601
    → tools/call (params.name must be "call"):
      → Inspect params.arguments shape:
        - no `method`, no `args`    → WikiHandler::RenderPage("")
                                       → markdown root index in content[]
        - `method` only, no `args`  → WikiHandler::RenderPage(method)
                                       → markdown page in content[]
        - `method` + `args`         → execute path (continue)
        - `path` aliases `method`; both conflicting → error -32602
        - any other arg field       → error -32602 (names field + valid set)
      → OnRequestReceived → RpcDispatcher.ProcessRequest(method, args)
        → Game thread check (marshal if needed)
        → GC/save deferral check (defer if active)
        → Reentrancy guard (defer if already processing)
        → Auto-validate required params from ParamSpec
        → Handler TMap lookup O(1)
        → FHandlerContext constructed
        → Handler function called (domain logic)
        → Ctx.SendSuccess()/SendError()
          → If a REGISTER_RPC_FORMATTER is registered for the method,
            the dispatcher applies it to the JSON result and returns
            plain markdown text instead of structured JSON.
          → Subsystem.SendAutomationResponse()
            → Transport wraps the dispatcher result into the MCP
              tools/call shape: { content, structuredContent, isError }
            → Transport.ResolveCompletion() → JSON-RPC 2.0 response sent
```

## Stdio Proxy Lifecycle Boundary

The in-editor HTTP endpoint and bundled stdio proxy intentionally expose different tool catalogs:

- The direct endpoint's `tools/list` returns exactly one tool, `call`. All dotted RPCs, including `system.run_ubt` and `system.run_tests`, execute inside a running editor through that tool.
- The stdio proxy forwards `call` to the endpoint and advertises three additional proxy-local tools, `editor_start`, `editor_restart`, and `editor_prepare_tests`. These tools remain discoverable when the editor endpoint is absent.

The stdio loop reads stdin on a dedicated reader thread. Request parsing, protocol methods and the proxy-local lifecycle tools run on the calling thread in order; each forwarded `tools/call` runs on its own daemon thread and writes its own response (stdout frames are serialized by one lock), so a streamed call whose job never turns terminal cannot queue later calls behind it. A streamed relay also has an overall ceiling (`STREAM_MAX_SECONDS`, 1800 s): past it the proxy closes the stream and returns an `isError` result naming the job's `ticket_id` for `system.job_status` polling; closing the stream does not cancel the job. EOF, a client disconnect, or a termination signal marks proxy shutdown. Cleanup applies only to tracked direct editor-start children; a prepared test command is returned before execution and is never proxy-owned.

All proxy-local lifecycle tools begin with an authenticated JSON-RPC ping. The ping is the live PinWright precondition and carries editorReady. A responsive-but-starting endpoint returns EDITOR_ALREADY_RUNNING, while forwarded public calls return retryable EDITOR_NOT_READY before dispatch. A forwarded call with a known-absent endpoint returns EDITOR_NOT_RUNNING; an unavailable ping remains EDITOR_UNRESPONSIVE because it does not prove that the editor is stopped.

### `editor_start`

Every mode first resolves the engine from the project's `EngineAssociation`, before the mode branch, so no argument combination can change which engine runs. Resolution mirrors the engine's own lookup order — the `EpicGames\Unreal Engine\<version>` registry entries (plus GUID-keyed source builds under `Epic Games\Unreal Engine\Builds`), the Epic launcher install manifest, on Linux the `[Installations]` section of `~/.config/Epic/UnrealEngine/Install.ini` (read as `FDesktopPlatformLinux` reads it: a `UE_<version>=<root>` key serves a version association, e.g. `UE_5.8=/path/to/UE_5.8` for `"5.8"`, and a GUID key serves that GUID; a bare `5.8=` key is not a registration), the `C:\UE_<version>` convention, and an association written as a project-relative engine path. An explicit `--editor-exe`, then a per-machine `$PINWRIGHT_ENGINE_ROOT`, precede the association; the variable is for an engine no lookup source can see (an unregistered source build) and suits a client config shared between machines. An association that matches no installed engine is a hard `EDITOR_ENGINE_NOT_FOUND` failure; the proxy never substitutes another engine, because launching a mismatched editor makes it rebuild the project's editor modules against the wrong engine.

On Linux the default visible launch spawns the resolved editor directly instead of the OS `open` action: `xdg-open` reaches the editor only if UnrealVersionSelector registered its MIME type, and otherwise hands the `.uproject` to whatever opens JSON. A proxy started over SSH has no `DISPLAY`, so a visible Linux launch borrows `DISPLAY`/`XAUTHORITY` from a process of the same user in a local X session (`:N` displays only, never an `ssh -X` `host:N` one) and fails with `EDITOR_NO_DISPLAY` before spawning when there is none; windowless launches need no display (`-RenderOffScreen` selects SDL's dummy video driver). Direct children never inherit the proxy's stdio on POSIX, because stdout is the MCP frame stream.

The default `editor_start({})` path resolves the `.uproject`, delegates it to the operating system's registered `open` action, and polls the MCP endpoint until `ping.editorReady` is true. This is the same path as a normal `.uproject` double-click. On Windows the association normally routes through UnrealVersionSelector `/editor`; the proxy does not invoke that executable directly and does not reproduce its prompting, compilation, or compatibility logic. Its own engine resolution is the precondition gate for this path (an unresolvable association would otherwise leave the selector waiting on a modal engine picker until the readiness timeout) and reports the chosen root as `engineRoot`. It runs no UBT phase, injects no flags, performs no project/plugin classification, and does not inspect manifests, BuildIds, or DLLs. Unreal owns startup compilation and module loading.

The socket transport publishes the game-thread readiness snapshot to its I/O thread. When the snapshot is unsafe, every `tools/call` — including read-only wiki pages and quit/status/diagnostic handlers — resolves immediately to an in-band retryable `EDITOR_NOT_READY`; `initialize`, `ping`, and `tools/list` remain available as bootstrap protocol operations. `FRpcDispatcher` repeats the readiness check on the game thread for direct in-editor callers and requests that raced a state transition. Widget Designer capture retains its own selection/package guard as a final defense before nested Slate ticking.

That snapshot also carries modal-block state: `Private/Transport/ModalStateProbe.h/.cpp` latches from `FSlateApplication::GetOnModalLoopTickEvent()` — the one callback that fires inside the nested Slate loop while the game thread is owned by a modal — and is cleared from `UPinWrightSubsystem::Tick`, which can only run once the core ticker resumes. Past `UPinWrightSettings::ModalBlockedReportSeconds` the I/O thread answers `ping` and `tools/call` with the non-retryable `EDITOR_BLOCKED_ON_MODAL` instead of the retryable `EDITOR_NOT_READY`, because the plain readiness snapshot is stale-but-positive in that state (every writer of it runs on the blocked thread).

The same file carries a second, complementary probe for the case with no callback to latch: a handler that simply does not return. `NoteGameThreadAlive()` stamps a liveness heartbeat on every subsystem tick, and the I/O thread reports the stamp's **age** — nothing has to fire during the wedge for the wedge to be visible. `FRpcDispatcher::ProcessRequest` brackets its handler call with `NoteRpcDispatchBegin/End`, publishing the in-flight method and request id under a lock the game thread can never be holding while wedged, so the report names the responsible verb rather than only a duration. Past `UPinWrightSettings::GameThreadStallReportSeconds` (default 90.0, chosen above the worst observed *legitimate* game-thread stall) `McpRequestCore::BuildPingResult` answers the retryable `EDITOR_GAME_THREAD_STALLED` with `stalledSeconds` / `inFlightMethod` / `inFlightRequestId` / `inFlightSeconds`. Deliberately asymmetric with the modal path: the stall is reported on `ping` only and **never gates `tools/call`**, because a stalled thread may still drain and the queued request will run when it does. Detection only — there is no safe way to interrupt a game thread wedged inside arbitrary engine or CPython code.

Non-default headless, caller-argument, map, and wait-for-exit modes retain direct process spawning because a file association cannot represent those arguments. They use the association-resolved engine root and the live-editor guard. -AutoDeclinePackageRecovery protects boot-time recovery, and windowless launches keep -unattended paired with -RunningUnattendedScript. Direct editor_start children remain proxy-owned while the verb waits. When it returns, a child still running (ready, timed out, interrupted) is released: it keeps running in its own session through a proxy exit or restart, and a daemon reaper thread (`_reap_on_exit`) waits on it so its exit is collected instead of leaving a `<defunct>` entry under the long-lived proxy. A zombie keeps its PID, so UBT's hot-reload check reads its `Engine/Intermediate/EditorRuns/<pid>` marker as a live editor (and fails on its empty exe path), and the project's next editor writes `<Project>_2.log`. The editor_prepare_tests planner does not spawn a test child, and a caller executing its returned command owns that process.

The optional `map` argument is emitted as the first token after the `.uproject` and before every switch, the only position `FUnrealEdMisc::OnInit` reads it from (`UnrealEdMisc.cpp:396-399` takes the first token of the remaining command line and skips the load when it begins with `-`). A map routed through `extra_args` would land after the switches and be silently ignored, so it is validated and positioned by `build_editor_command` rather than left to the caller; a token the engine would ignore is rejected with `INVALID_MAP` before anything is spawned.

### `editor_restart`

Quits the running editor through its own `editor.quit` RPC, waits for the endpoint to stop answering (`_EDITOR_STOP_TIMEOUT`, separate from the start half's `--start-timeout`), then runs the `editor_start` path — optionally with `map`. It is proxy-local for the same reason `editor_start` is, and it exists as one verb because the seam between the halves is not callable from outside: `editor_start`'s live-endpoint guard returns `EDITOR_ALREADY_RUNNING` for the whole shutdown window, so a caller sequencing the two by hand races it. The quit half never forces anything — a refusal (notably `UNSAVED_CHANGES` with neither `save` nor `discard` set) is relayed as `EDITOR_QUIT_REFUSED` and no second editor is started; a modal-blocked, unresponsive, or still-starting editor is reported rather than bypassed, because `editor.quit` runs on the game thread those states own; a shutdown that never completes is `EDITOR_STOP_TIMEOUT` with no kill and no respawn. The map argument is validated before the quit, so a typo never costs a running editor.

### `editor_prepare_tests`

`editor_prepare_tests` accepts exactly one mandatory, non-empty `filter` string and has no default. It runs the live PinWright editor guard before validation or path preparation. A detected live editor is a hard `EDITOR_ALREADY_RUNNING` response. An unavailable probe is reported as `not_probed` and is never treated as proof that the editor is stopped. Only a safe precondition result proceeds to command preparation.

The planner resolves the project's `EngineAssociation` and then resolves the matching `UnrealEditor-Cmd` executable from the configured engine installation; off Windows, where `-Cmd` is only a console-subsystem twin that may not be built, it falls back to the `UnrealEditor` binary itself. It prepares an absolute project path and log path and returns a structured `COMMAND_READY` object containing the launch executable and `argv`, the explicit `logPath`, the checker executable, and checker `argv`. The engine association is authoritative; the planner does not substitute another engine.

The launch `argv` contains `-ExecCmds="Automation RunTests <filter>,Quit"`, `-TestExit="Automation Test Queue Empty"`, `-Abslog=<same absolute logPath>`, `-unattended`, `-RunningUnattendedScript`, `-nopause`, `-nocefaccelpaint`, `-ddc=InstalledNoZenLocalFallback`, and `-log`. It uses a real RHI and does not add `-NullRHI`. Filters that would corrupt the command boundary are rejected.

The planner returns immediately. It does not compile, launch, wait, kill, sample CPU or IO, apply timeout overrides, or return a test verdict. The caller owns the returned process, runs the returned launch command, and then runs the returned checker command with the exact returned log path.

`check_suite_log.py` remains the fail-closed classification authority. It requires the counted `Automation Test Queue Empty <N> tests performed.` drain marker and preserves the ordered empty, incomplete, failure, skip, and clean states. `PINWRIGHT_ASSERTIONS_SKIPPED` remains distinct from a clean result as `COMPLETED_WITH_SKIPS`. The preparation response carries no counts, process exit, timeout, PID, or verdict fields, so preparation and classification cannot drift.
## Auto-Registration System

Handlers use static auto-registration — no header declarations or manual wiring needed:

```cpp
// Private/Handlers/<Domain>/MyHandler.cpp
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"

REGISTER_RPC_HANDLER("namespace.verb", "namespace", "Summary",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Description"),
        RPC_PARAM_OPT("flag", "bool", "Description")
    ))
{
    FString Name; // auto-validated by framework (required params)
    if (!Ctx.RequireString(TEXT("name"), Name)) return true; // harmless double-check

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("message"), TEXT("done"));
    Ctx.SendSuccess(Result);
    return true;
}
```

**Pipeline:** `REGISTER_RPC_HANDLER` → static `FAutoRegisterHandler` → `GetPendingRegistrations()` array → `DrainAutoRegistrations()` at subsystem init → runtime `Handlers` TMap

**Subsystem init order:** `IntegrationGates::LoadEnabledIntegrations()` (loads enabled sub-modules so their static-init registrations join the pending array) → `DrainAutoRegistrations()` → `DrainFormatterRegistrations()` → `Catalog->Initialize()` → `WikiDiskGenerator::Generate()` → transport bind/start. The gate must precede the drain: a sub-module loaded later would register handlers into an already-drained array, and its methods would be missing from the catalog, the generated wiki, and the dispatch TMap.

**Transport bind and its bounded retry:** `FSocketHttpServer::Start` binds `127.0.0.1:<port>` directly — the bind doubles as the port-conflict probe, and `EStartResult` separates a contested port (retryable) from a missing socket subsystem or a dead I/O thread (not). A lost bind arms `PinWrightBindRetry::FState` (`Public/Transport/BindRetryPolicy.h`), driven from the existing 0.1 s subsystem ticker: 2 s doubling to a 30 s cap, total budget 600 s, then a terminal error re-logged every 300 s. Bounded on purpose — the case it must cover is a previous editor of this project that has not finished releasing the socket; a port owned by another project's editor or sitting in a Windows excluded / Hyper-V range never clears, and retrying forever would hide a configuration problem behind an infinite "still trying". A late bind runs the same activation as the first (`TryStartTransport`), so the `Saved/PinWright/gateway-port` file and the job-event bridge are never skipped.

**The advertisement half:** `Private/Transport/PortAdvertisement.h` keeps `Saved/PinWright/gateway-port` truthful when this editor is *not* serving — the stdio proxy re-resolves the endpoint from it per call, so a stale port file is a worse failure than none. `Judge(bHasClaim, bSomethingListening)` is pure and socket-free; the single impure input is a non-blocking loopback connect (`IsSomethingListening`), whose every inconclusive outcome reads as *listening*. Retain on a live port (normally a sibling editor of the same project that legitimately won the bind), retract on a dead one, no-op when nothing is advertised. Called from every not-serving path in `UPinWrightSubsystem::TryStartTransport` and again on the exhausted re-log cadence. Publication is treated as part of serving: `GatewayPortFile::WritePortFile` returning false sets `bPortPublishPending`, logs an Error, and is retried from the ticker.

`GetServerStatus()` reports `PortInUse` for a bind-failed editor, which is the only way this state is observable: an editor that never bound serves no RPC, so nothing it could answer would report it. Before this, a lost bind reported `Disabled`, which made the setup screen's red port-conflict banner and the module's show-setup-screen-on-problem path unreachable. **A bind-failed editor is a live process that answers nothing** — distinguish it from a stopped editor by the `Saved/Logs` error, `GetServerStatus`, or the setup screen, never by a probe.

Proxy side: `_probe_state`'s `not_running` is the only state that licenses starting an editor, so it requires evidence that nothing is listening (a refused connection). Every other transport failure is `unresponsive`. The old catch-all mapped any unexpected exception to `not_running`, which told a caller to start a second editor beside a live one — and the second editor loses the bind, which is precisely the failure the retry above exists for.

**Launch-time wiki generation:** After `DrainAutoRegistrations()` populates the dispatch TMap at subsystem init, `WikiDiskGenerator` walks the namespace tree and assembles every page (auto-content + `docs/wiki-src/` overlays via `FWikiOverlay`), `registry.json`, and a conservative ownership manifest. It byte-verifies that complete set in one unique sibling staging directory before creating an absent final output root or publishing any final file. Changed pages and `registry.json` are then published one at a time through `AtomicFileWriter`; any failure stops before pruning. Stale files are pruned only in the canonical `Saved/PinWright/wiki/` root and only when the previous valid ownership manifest lists them. The generator records failed deletions, rebuilds the final manifest from the desired filenames plus only those failures, validates its limits again, and atomically publishes that manifest last. Per-file publication is deliberate because open Windows readers can block a whole-directory rename; a mid-publication or final-manifest failure can leave earlier successful publications or prunes and does not imply whole-tree rollback. A `WikiOutputDirectory` override still receives generated output but is never pruned. The primary wiki-discovery response returns an on-disk file path reference to the generated output; `WikiHandler`'s inline markdown rendering is used only as the cold-start / not-found fallback. `docs/wiki-src/` holds the hand-authored overlay sources; `Saved/PinWright/wiki/` is the plugin-owned generated output.

**Auto-validation:** Required params declared via `RPC_PARAM_REQ()` are automatically checked by the dispatcher before the handler runs. Missing required params return `MISSING_REQUIRED_PARAM` error without invoking the handler body.

## Atomic Output Publication

`AtomicFileWriter` is the shared destructive-output boundary for agent MCP configuration (`AgentMcpConfigurator`), generated wiki pages and manifests (`WikiDiskGenerator`), and OBJ/STL exports (`MeshIOHandler`). Callers choose `FailIfExists` or `ReplaceExisting`; the result reports whether a successful publication actually replaced a destination.

The writer stages to a unique GUID-named sibling of the destination, writes every byte, flushes and closes the handle, then reopens the temporary file and verifies its size and bytes. Only then does it publish within the same directory through Windows `MoveFileExW`, with `MOVEFILE_REPLACE_EXISTING` used only for an explicit replacement. On Linux the stage is `open(O_CREAT|O_EXCL)` + `fsync`, a non-replacing publish is `renameat2(RENAME_NOREPLACE)` (falling back to `link`+`unlink` where the filesystem rejects the flag), a replacement is `rename(2)`, and the directory is fsynced after either (the `MOVEFILE_WRITE_THROUGH` counterpart). Both platforms refuse to replace a read-only destination; on Linux a replacement keeps the destination's permission bits. It never pre-deletes the destination, and it removes the temporary file after a failed stage, verification, or publication when possible.

Do not treat Unreal's `IFileManager::Move(..., Replace=true)` as an atomic-replacement precedent. That implementation can delete the destination before attempting the move, so a failure between those operations loses the old file. Destructive file outputs that need preservation on failure should go through `AtomicFileWriter` instead.

## Find-in-Blueprints Task Lifetime

`blueprint.search` can auto-index deferred assets before starting the real search. The first auto-index pass creates a dummy `FStreamSearch` only to trigger Find-in-Blueprints' first-search state, then cancels it through `BlueprintIndexHandler::StopAndJoinStreamSearch`.

The cancellation order is load-bearing: call `Stop()`, poll `IsComplete()` with a short sleep and no TaskGraph pumping, then call `EnsureCompletion()` only after completion. Calling `EnsureCompletion()` immediately can enter `ProcessThreadUntilIdle` from an RPC already running on the game-thread named queue and hit TaskGraph's recursion guard. Waiting for `IsComplete()` first also lets `FStreamSearch::Run` register and unregister the query normally before the native thread is joined. `PinWright.blueprint.search.AutoIndexTaskQueueSafe` protects this queue-safety contract.

## Dual-Surface Principle

Any structured asset-state output the plugin emits should be reachable through **both** surfaces — the asset-dump sidecar pipeline and a live MCP RPC — and both surfaces must call the same builder. Drift between the two is a bug.

The two paths serve different agent workflows: sidecars are cheap, pre-baked snapshots for read-many batch reasoning (`asset.dump_folder` sweeps the project once, agents grep the cache); the live RPC is the read-after-mutate path, where an agent has just compiled or rewired something and needs the current truth. Two builders for the same logical output will silently diverge and corrupt downstream reasoning — the cache says one thing, the live call says another, and the agent has no way to tell which is wrong.

**Mandatory** for IR text outputs: BPIR, MGIR, AGIR, SCIR, BTIR, MSIR, NIR, PCGIR, and any future IR. **Strongly recommended** for structured JSON sidecars on complex assets (`sound_cue.json`, `metasound.json`, `niagara_*.json`, `tree.xml`, `widget_animations.json`, `scs.json`, `data_table.json`, etc.) — current state is mostly dual but informal.

**Out of scope:** mutators (`add_node`, `set_property`, `compile`, `play`) — no inspectable state to mirror; file-shaped artifacts (`preview.png`, binary captures) — the dump file is the artifact; generic catch-alls (`meta.json`, `properties.json`) — emitted for every asset with no targeted live equivalent; editor-control / job-ticket RPCs — orthogonal subsystem.

**Implementation rule:** one shared builder per output, e.g. `BuildXxxText(UAsset*) -> FIrResult` or `BuildXxxJson(UAsset*) -> TSharedPtr<FJsonObject>`. The `AssetDumpHandler` sidecar writer and the matching MCP RPC handler both call it. Never let two builders exist for the same output. See the [IR authoring guide](ir-authoring.md) for the IR-side application of this rule, board ticket `F-ircore-shared-text-helpers` for the shared formatting layer IR builders rest on, and the IR sidecar registry below for the registry that makes sidecar/RPC parity structural.

## IR Sidecar Registry (Dual-Surface Pattern)

`Private/Utils/IrSidecarRegistry.h` exposes the `REGISTER_DECOMPILE_IR(SpecName, FileName, ClassThunk, BuildFn, Priority)` macro, which auto-registers a decompile-IR through a single declaration site. The macro records a `FIrSidecarSpec` (name, sidecar filename, asset-class thunk, shared build function pointer, priority) into a static registry drained at runtime by `AssetDumpHandler::DumpSingleAsset` via `AddRegisteredIrSidecarFiles`.

One `REGISTER_DECOMPILE_IR` line wires both surfaces required by the Dual-Surface Principle:

- **Sidecar:** the registry-drain in `AssetDumpHandler.cpp` writes the IR text to the canonical `<ir>.txt` file (e.g. `msir.txt`, `nir.txt`, `scir.txt`, `btir.txt`) when the asset class matches.
- **Live RPC:** the matching `Handlers/<Domain>/<IR>DecompileHandler.cpp` calls the same `BuildFn`.

Both surfaces invoke a single shared builder function, so drift between dump and RPC is structurally impossible — no dispatch-branch edit in `AssetDumpHandler.cpp` is required for a new IR. Current consumers: SCIR, BTIR, MSIR, NIR. BPIR/MGIR/AGIR predate the registry and keep their explicit dispatch-branch wiring; new IRs should always use the registry.

## CRIR Decompiler Classifier Dispatch Order

The CRIR decompiler (`Private/CRIR/CRIRDecompiler.cpp`) must classify each `URigVMNode` from most-derived to most-general because `URigVMUnitNode` and `URigVMDispatchNode` both inherit `URigVMTemplateNode`. The correct emission-loop order is: unit → dispatch (if/select special case → general dispatch) → bare template → variable/reroute/enum/invoke_entry → collapse → function_ref → function_entry/function_return → catch-all `# TODO`.

`URigVMAggregateNode` rides the collapse arm via `Cast<URigVMCollapseNode>` (URigVMAggregateNode : URigVMCollapseNode), gated by `UE_RIGVM_AGGREGATE_NODES_ENABLED` which defaults to `1` in UE 5.6. Aggregate class identity is documented-loss on round-trip — the inner unit nodes carry all runtime behavior.

The function library is per-asset, not per-graph. Access via `Blueprint->GetRigVMClient()->GetLocalFunctionLibrary()`. The library extends `URigVMGraph`; its top-level children are `URigVMLibraryNode` (the function definitions). The decompiler emits one `rig_function "Name" { ... }` block per library node alphabetically sorted, AFTER `rig_graph` blocks. See [CRIR language reference](crir-language-reference.md) for the opcode grammar.

## Niagara Decompile Helpers

`Private/Handlers/Niagara/NiagaraDecompileHelpers.h` exports two `PINWRIGHT_API` functions shared between the JSON dump path (`NiagaraDumpBuilder.cpp`) and the NIR text path (`NIRDecompiler.cpp`):

- `IsNiagaraOnDemandCompileEnabled()` — reads the `fx.Niagara.OnDemandCompile` cvar without forcing a compile. Used only by live Niagara diagnostic builders; persisted NIR and compile sidecars intentionally omit this editor-session state.
- `CollectAndSortFunctionCalls(const UNiagaraGraph*)` — returns a stable-sorted (`NodePosY` then `NodeGuid.ToString()`) array of module function-call nodes.

Future Niagara IR work (the v1b override-chain resolver, the v1c module-script graph emitter) should consume these helpers rather than re-copying. `NiagaraJsonHelpers::GetGraphFromScript` is already public for getting the graph from a `UNiagaraScript*`; use it instead of rolling local accessors.

## Shared Handler-Cluster Helpers (Per-Domain)

Alongside `MaterialFinders.h`, several inline header-only helpers live next to the handler cluster they primarily serve. Convention: header co-located in the handler subdirectory, no `.cpp`, no `Build.cs` change, ODR-safe under Unity builds. Reach for these before copying logic across sibling handlers.

| Header | Exposes | Consumers |
|---|---|---|
| `Handlers/Environment/PostProcessVolumeUtils.h` | `FindOrSpawnUnboundPPV(FHandlerContext&)` | 8 callers across `LightingHandler` (`set_exposure`, `set_ambient_occlusion`) and `PostProcessHandler` (5 mutating setters) |
| `Handlers/Environment/EnvironmentSpawnHelpers.h` | `ReadLocationRotationFromPayload(...)`, function template `ApplyPropertiesJsonToComponent<TComponent>(...)` | 3 environment spawn handlers (`spawn_sky_atmosphere`, `spawn_volumetric_cloud`, `spawn_reflection_capture`) |
| `Handlers/Environment/GIMethodCVarHelper.h` | `ApplyDynamicGIMethodToCVars(const FString& Method)` | `lighting.setup_global_illumination` (CVar-only live) and `rendering.set_dynamic_gi_method` (CVar + UPROPERTY persistence) |
| `Handlers/Material/MaterialLayerStackHelpers.h` | `ApplyMaterialLayerStack(...)` — maintains the parallel arrays Layers / Blends / LayerNames / LayerStates / LayerGuids / LayerLinkStates / RestrictToLayerRelatives / RestrictToBlendRelatives + `RebuildLayerGraph` | `material.authoring.set_material_layer_stack` and `MGIRExpressionEmitter::EmitLayerStack` |
| `Handlers/Material/MaterialFinders.h` | Material expression / parameter lookup helpers (pre-existing); `IsMaterialEditorOpen(UMaterial*)` open-editor guard backing the `EDITOR_OPEN` reject in `LOAD_MATERIAL_OR_RETURN` and `remove_material_node` (a live `FMaterialEditor` works on a graph copy, so external mutations are clobbered — fail loud instead) | Material authoring + MGIR handlers |

When a new helper would be consumed by ≥2 sibling handlers in the same cluster, prefer this pattern over either anonymous-namespace duplication or promoting it to `Utils/`. Cross-cluster reuse is the trigger for moving into `Utils/`.

## Shared Audit Contract (`Private/Audit/AuditFramework.h`)

The one definition of what an audit verb's `pass` means, shared by `level.audit`, `geometry.audit_static_meshes`, `landscape.audit_shape` and `skeleton.audit_skin_weights`. Exposes `PinWrightAudit::FVerdict` (`DerivePass` — the rule), `ESeverity` / `EFindingStatus` plus their wire spellings, `EFailOn` + `ParseFailOn`, `PassRuleText`, and templates over any check-descriptor table (`ParseCheckId`, `CheckInfo`, `CheckBit` / `HasCheck`, `DefaultCheckMask` / `AllCheckMask` / `MaskWhere`, `ValidCheckIdList`). Each audit keeps its own `FCheckInfo` struct — the extra columns are genuinely per-audit — and aliases the shared enums so `LevelAudit::ESeverity::Error` still reads the same at every call site. The design rules are `docs/rpc-design.md` §18; its own rule net is `PinWright.infra.audit_contract.*`.

**Why `Audit/` and not `Utils/`**, against the rule stated just above: this is a wire *contract* — a response shape and the verdict derived from it — not helper code. It sits beside `Dispatch/`, `Catalog/`, `State/` and `Transport/` as a named concern, and `Utils/` is engine-level plumbing (`PathUtils`, `ClassUtils`, `JsonUtils`). The one genuinely util-shaped piece of this work, `PathUtils::NormalizeToObjectPath`, did go to `Utils/`.

**Header-only, and that is what makes it cross-module.** `PinWrightGeometry` is a separate module; it already adds `Source/PinWright/Private` to its `PrivateIncludePaths` (see Integration Sub-Modules below), so `#include "Audit/AuditFramework.h"` resolves from both modules with no `PINWRIGHT_API` export and no new link edge. Everything in it is `inline`, `constexpr` or a template for that reason.

## Spatial Perception & Camera Capture

Two clusters give an agent metric "eyes" on the editor world: the `spatial` namespace (`Handlers/Spatial/`) for deterministic perception and placement, and the `camera` namespace (`Handlers/Render/CameraFrameHandler.cpp`) for multi-angle live-viewport capture. They rest on two shared primitives plus a reused CPU rasterizer pattern:

- **`Handlers/Render/ViewProjectionUtils.h/.cpp` (`namespace PinWrightViewProjection`)** — `DeprojectScreenToWorld(...)` and `ProjectWorldToScreen(...)`. Both rebuild the capture's `FSceneView` via `FEditorViewportClient::CalcSceneView` from a caller-supplied pose + fixed size (top-left pixel origin) and restore the viewport afterward, so screen↔world stays pose-coherent with the pixels a capture actually wrote. Consumers: `spatial.raycast_screen`, `spatial.place_on_surface` (screen mode), and `render.capture_annotated`'s overlay projection. Because it drives the active Level Editor viewport, a headless editor returns a typed `NO_ACTIVE_LEVEL_VIEWPORT` / `VIEW_BUILD_FAILED`.
- **`Handlers/Spatial/SpatialTraceUtils.h/.cpp` (`namespace SpatialTraceUtils`)** — `TraceLine(...)` and `TraceGroundBelow(...)` over `UWorld::LineTraceSingleByChannel`, returning a shared `FSpatialHit`. Consumers: every world-touching `spatial.*` verb (`raycast`, `raycast_screen`, `place_on_surface`, `verify_placement`). The `measure_*` / `place_relative` verbs are pure `GetActorBounds()` box math and take no trace.
- **`render.capture_annotated` (`AnnotatedCaptureHandler.cpp`)** reuses the **`FDriveSetOfMarkRenderer` CPU pixel-buffer rasterizer pattern**: a SetPixel / FillRect / DrawLine / DrawLabel toolkit with a 3×5 bitmap font that paints measured overlays (world XYZ axes, a Z=0 grid, per-actor AABB wireframes + size labels) onto the decoded PNG buffer. The Drive renderer's primitives are file-local (only `DrawMarks` / `MarkColor` / `LabelColor` are header-exported), so the equivalents are re-implemented in a uniquely-named namespace and extended with a Bresenham line routine and an A–Z glyph font the digit-only Drive font lacks.

New verbs beyond those two namespaces, added alongside this cluster: `render.capture_annotated` (`Render/`); `actor.spawn_shape`, `actor.spawn_batch`, `actor.set_folder`, `actor.nudge` (`Actor/`); and `geometry.append_buffers`, `geometry.export_obj` / `import_obj` / `export_stl` / `import_stl`, `geometry.measure`, `geometry.check_health` (`Geometry/`). The agent-facing workflow that chains all of these is [spatial-authoring.md](spatial-authoring.md); the caller-facing overlays are `wiki-src/spatial.md` and `wiki-src/camera.md`.

## Shared JSON Builders (`Utils/JsonBuilders.h`)

`Private/Utils/JsonBuilders.h` is the canonical home for JSON primitives reused across `DumpBuilders` and RPC handlers. Reach for these before hand-rolling local helpers; the file uses a named namespace (`JsonBuilders`) so it is ODR-safe under Unity merges.

Inline header helpers:

- `MakeObject()` — `MakeShared<FJsonObject>()` shorthand.
- `BuildVectorJson(FVector)` — `{x,y,z}` shape.
- `BuildRotatorJson(FRotator)` — `{pitch,yaw,roll}` shape.
- `BuildLinearColorJson(FLinearColor)` — `{r,g,b,a}` shape.
- `EnumValueToString<TEnum>(TEnum)` — `StaticEnum<TEnum>()->GetNameStringByValue`; returns empty string for non-reflected enums.

Out-of-line helpers (in `JsonBuilders.cpp`):

- `BuildTransformJson(FTransform)`
- `BuildNameArrayJson(TArray<FName>)`
- `GetActorLevelPackageName(AActor*)`
- `GetObjectPathSafe(UObject*)` — null-safe `GetPathName()` wrapper.

Local enum-to-string helpers in builders are now considered duplication and should be replaced with `EnumValueToString<>`. **Exception:** namespaced UENUMs declared as `enum class EFoo::Type { ... }` (e.g. `EBodyCollisionResponse::Type` in `BodySetupEnums.h`) cannot be resolved through `StaticEnum<>` and must call `LexToString` directly.

## Sparse Instanced-Subobject Property Diffs (`Utils/PropertyExport`)

Instanced `UObject` subobjects in asset dumps (`properties.json`, `scs.json`) and in `actor.describe` are serialized as **sparse diffs against the subobject's archetype / CDO**, not full field dumps. Only fields that are `CPF_Edit | CPF_BlueprintVisible`, non-transient, and whose value differs from the baseline are emitted. This keeps the dumps small and makes "what did the user actually change" legible.

The shared core is `BuildSparseFieldDiffJson` (`Private/Utils/PropertyExport.h/.cpp`). It owns the whole skeleton: iterate the leaf class's `CPF_Edit | CPF_BlueprintVisible` fields (skipping `CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated | CPF_SkipSerialization | ExtraSkipFlags`, known nonsemantic derived fields, and any name in `SkipProperties`), sort by property name for deterministic output, diff each against the resolved baseline via `Field->Identical(InstanceValue, BaselineValue, PPF_DeepComparison)`, drop matches, and emit an `IsKnownOversizedProperty` placeholder for known-huge fields. Per-leaf serialization is delegated to a `TFunctionRef` callback so the two callers can emit different leaf shapes from one diff loop.

Two callers delegate to it, differing only by their `EPropertyFlags ExtraSkipFlags` arg and their leaf-emit callback:

- `BuildSparsePropertyDiffJson` (`Utils/PropertyDiff.cpp`) — the `actor.describe` path (`Handlers/Actor/DescribeHandler.cpp`). Passes `CPF_None` extra skip and emits inheritance-annotated `{type, value, ...}` leaves via `ExportPropertyToJsonValueWithInheritance`.
- `SparseInstancedSubobjectToJsonObject` (`Utils/PropertyExport.cpp`) — the asset-dump / SCS path. Adds `CPF_DuplicateTransient | CPF_TextExportTransient` to the skip set (subobject templates carry duplicate/text-export transients that aren't real authored state) and emits plain values via `ExportPropertyToJsonValue`.

Because this defines the serialized bytes of `properties.json` and `scs.json`, any change to the diff shape **must bump the `properties.json` / `scs.json` aspect versions** in the `Versions` map in `Handlers/Asset/AssetDumpCache.cpp` — otherwise stale caches keep serving the old format.

## Recorder Query Pagination

Recorder query methods share one cursor convention, implemented in `Private/Handlers/Recorder/RecorderQueryEngine.cpp`: a cursor is a decimal-integer offset **string** (parsed by the file-local `ParseCursor` helper; empty/negative/garbage input parses to 0) into a deterministically sorted, fully materialized match/change list. Page math:

- `Consumed = StartOffset + Shown`
- `Elided = max(0, Total - Consumed)`
- `bTruncated = Elided > 0` — per-page remaining, so the **last** page reports `truncated: false`
- `NextCursor = FString::FromInt(Consumed)`, populated only while `Consumed < Total`

Cursor stability holds because sessions are immutable on-disk ndjson files reloaded per request through a deterministic loader + sort. Consumers of the pattern: `recorder.get_series` with `reduction='diff'` (via `RecorderReductions::Diff`) and `recorder.find_events` (since 2026-06).

Gotcha for new recorder methods: `RecorderEnvelope::Build` (`Private/Handlers/Recorder/RecorderEnvelope.cpp`) unconditionally renders a `nextCursor` field — an explicit `FJsonValueNull` when `bHasNextCursor` is false. The header comment claims unset optional fields are omitted, but `nextCursor` is the one deliberate exception. Any recorder method that truncates without populating the cursor therefore shows agents a misleading `truncated: true, nextCursor: null` combination — this is exactly what motivated the `find_events` pagination fix. If a method can truncate, it must implement the cursor math above.

## Recorder Session Segments (`recorder.list_segments`)

The recorder query surface has first-class session segments: `recorder.list_segments` (handler in `Private/Handlers/Recorder/RecorderHandler.cpp`) pairs host-project gameplay boundary events into typed `[tMin, tMax]` windows. The pairing logic lives in the pure-function module `Private/Handlers/Recorder/RecorderSegments.h/.cpp`, which follows the `RecorderReductions` pattern — no editor dependencies, operates on `RecorderModel::FSessionModel`, unit-testable in-memory (`Tests/Recorder/RecorderSegmentsTests.cpp` builds fixtures directly). A declarative static rule table (`RuleTable()` in `RecorderSegments.cpp`) maps the host project's boundary-event vocabulary to segment types; adding a segment type only needs a new table row. A representative rule set:

| Type | Starts | Ends | Notes |
|---|---|---|---|
| `round` | `game:countdown_start` | `game:run_finish`, `game:run_abandon` | `game:run_start` and `game:reset` are **interior anchors, not boundaries** — `game:reset` fires on mid-run soft resets and must never close a round |
| `lobby` | `mp:session_join` | `mp:match_start` | `bIgnoreStartWhileOpen`: every joining player emits `mp:session_join`, so repeats attach as anchors instead of restarting |
| `mp_match` | `mp:match_start` | `mp:match_end` | |
| `arena_round` | `arena:round_start` | `arena:round_end` | start props carry `round` (int) |
| `editor_session` | `editor:session` `action=open` | `editor:session` `action=exit` | single event name discriminated by the `action` prop; concurrent instances correlated via `CorrelationProp = editor_session_id` (GUID) |
| `mission` | `mission:start` | `mission:complete` / `fail` / `abandon` | one vocabulary shared by all mission modes |
| `practice` | `practice:start` | `practice:end` | |

Pairing semantics (`PairSegments`): one timestamp-StableSorted pointer scan per rule. A restart-while-open closes the prior segment half-open (`endOpen`) at the new start; an end-without-start degrades to a `startOpen` segment anchored at the session's `MinTs`; EOF closes still-open segments at `MaxTs` with `endOpen`. Indices are 1-based per type; output is sorted by `tMin`. Output schema per segment: `{type, index, tMin, tMax, startOpen?, endOpen?, props, anchorEvents}` — `props` echoes the start event's props verbatim (`run_id`, `round`, `editor_session_id`, ...), and `tMin`/`tMax` feed directly into the `from`/`to` params of the existing window verbs (`recorder.describe_session`, `recorder.summarize_change`, `recorder.get_series`, `recorder.find_events`). Segment- or round-scoped analysis therefore no longer needs the old `find_events` → manual-window two-step.

Related: `recorder.describe_session` accepts optional `from`/`to`. When windowed, the object-catalog activity ranking is recomputed inside the window via `RecorderAsOf::SliceRange` over the sorted `Series`, `eventCount` counts only in-window events, and `meta` echoes `timeRangeCovered`; the unwindowed path is behaviorally identical to before.

## Recorder Session Model Ordering Invariants

`RecorderModel::FSessionModel.Events` is **not** guaranteed timestamp-sorted: `RecorderSessionLoader` only `StableSort`s the per-tag `Series` change-point arrays (~line 286 of `RecorderSessionLoader.cpp`); `Events` are appended in NDJSON file order with no sort. Consequences for any consumer:

- The `RecorderAsOf` binary searches (`IndexAt` / `FirstAtLeast` / `SliceRange`) must never be applied to `Events` — the linear scans in event-consuming code are correct there, not waste.
- A consumer that needs time order must sort a pointer view itself, and it must use **`StableSort` keyed on `Ts`**: same-frame events share an identical `double` `Ts`, and emission (file) order is semantically meaningful for boundary pairs. An unstable sort can swap an equal-`Ts` end/start pair and corrupt segment pairing into bogus half-open + degenerate pairs — this exact bug was caught in review of `RecorderSegments.cpp` (see the StableSort comment in `PairSegments`).

## Conditional-Compile Pattern for Optional-Engine-Plugin Handlers

Handlers that depend on an optional engine plugin (e.g. Water, MetaSound) follow a four-step pattern so the plugin still builds and namespaces still appear in the wiki when the optional module is absent:

1. **`Build.cs`** — soft-link the optional module via `TryAddConditionalModule(Target, EngineDir, "Foo", "Foo")`. Hard `PublicDependencyModuleNames.Add(...)` would break the build on installations without the plugin.
2. **Handler `.cpp`** — at file top, probe the headers and define a feature macro:
   ```cpp
   #if __has_include("FooHeader.h")
       #include "FooHeader.h"
       #define MCP_HAS_FOO 1
   #else
       #define MCP_HAS_FOO 0
   #endif
   ```
3. **`REGISTER_RPC_HANDLER` blocks are ALWAYS unconditional.** Registration must succeed so the namespace shows up in the wiki (`call("foo")`) — agents need to know the method exists, even if it will return an error on this install.
4. **Only the handler body branches:**
   ```cpp
   #if MCP_HAS_FOO
       // real impl
   #else
       Ctx.SendError(TEXT("FOO_PLUGIN_NOT_AVAILABLE"), TEXT("..."));
   #endif
   ```

Do **not** use a separate stub-registration TU — the unconditional `REGISTER_RPC_HANDLER` body with a guarded impl is the convention. Canonical references: `Handlers/Audio/MetaSound/MetaSoundDestructiveHandler.cpp` (original) and `Handlers/Water/WaterHandler.cpp`.

This pattern covers optional engine **modules** soft-linked into the main module. Integrations that depend on a whole optional engine **plugin** are instead quarantined into gated sub-modules (see the next section).

## Integration Sub-Modules (IntegrationGates, v0.7.0)

Five sub-modules (`PinWrightGeometry`, `PinWrightPCG`, `PinWrightChooser`, `PinWrightPoseSearch`, `PinWrightCommonUI`), each declared `"Type": "Editor"`, `"LoadingPhase": "None"`, Win64 + Linux in the `.uplugin`. Each hard-links its owning engine plugin's modules (GeometryScriptingCore/Editor + GeometryCore + GeometryFramework + DynamicMesh + MeshDescription/StaticMeshDescription; PCG + PCGEditor; Chooser; PoseSearch; CommonUI) and adds `PrivateIncludePaths` into `Source/PinWright/Private` so moved files keep their original `#include "Handlers/..."` shape. Motivation: Fab consumers on launcher engines hit LoadLibrary error 126 because `UnrealEditor-PinWright.dll` hard-imported DLLs of engine plugins the consumer had disabled; with `LoadingPhase: None`, a sub-module's DLL is never touched unless the main module explicitly loads it. The shape follows the UE 5.8 IKRig → IKRigUAF precedent.

- **Gate table:** `Private/IntegrationGates.h/.cpp`, a static plugin → module → method-prefix table (`GIntegrations`). `LoadEnabledIntegrations()` checks `IPluginManager::FindPlugin(...)->IsEnabled()` per entry and calls `FModuleManager::LoadModulePtr` for enabled ones; disabled/absent ones are recorded as skipped indices. One greppable startup line: `PinWright integrations: loaded=[...] skipped=[...]` (`LogPinWrightIntegrations`).
- **Skipped-method behavior:** the dispatcher's unknown-action path consults `IntegrationGates::FindSkippedByMethod` and returns `PLUGIN_DISABLED` naming the disabled engine plugin (instead of fuzzy suggestions) for methods matching a skipped prefix (`geometry.`, `pcg.`, `chooser.`, `pose_search.`, `ui.activatable_`, `ui.list_stack_widgets`, `ui.get_active_widget`). `WikiHandler` consults `FindSkippedByNamespace` and prepends an unavailability banner on skipped-namespace pages (CommonUI shares the `ui` namespace and has no exclusive slug, so it never banners).
- **Descriptor rules:** every optional engine-plugin ref in the `.uplugin` is `"Enabled": true, "Optional": true`. `Optional` makes launcher hosts whose receipt lacks the plugin skip it silently (UE 5.4+ receipt gate; BP-only projects use the stock `UnrealEditor.target`) and satisfies UBT's undeclared-dependency validation (checks `bOptional` only). `Enabled` must be true: every listed name is seen-marked in the engine's reference BFS before any flag check, so an `Enabled:false` ref silently blocks a later enabled ref to the same plugin from elsewhere in the closure (this broke default 5.8 boots via SkeletalMeshModelingTools -> GeometryScripting; see `lessons.md`). Where the receipt allows the plugin the ref enables it and the sub-module loads; where it does not, the integration is skipped gracefully.
- **Boundary notes:** `SplineHandler.cpp`/`SplineHelpers.h` stayed in the main module (`spline.*` has no GeometryScripting dependency). The `pcgir.txt` sidecar registers through the main module's `IrSidecarRegistry` from `PinWrightPCG`'s `PCGDecompileHandler.cpp`. The former `ai.*` SmartObject/Mass handlers did NOT become a sub-module: they were rewritten reflection-only in `Handlers/AI/AIHandler.cpp` (zero linkage; the SmartObjects/MassGameplay plugin refs were removed from the `.uplugin`).

## FHandlerContext API

Per-request context object providing typed access to params and response helpers:

**Getters** (return default if missing):
`GetString`, `GetNumber`, `GetBool`, `GetInt`, `GetVector`, `GetRotator`, `GetObject`, `GetArray`, `GetStringFirstOf` (alias lookup)

**Validators** (send error and return false if missing/wrong type):
`RequireString`, `RequireAssetPath`, `RequireInt`, `RequireNumber`, `RequireBool`, `RequireObject`, `RequireArray`

**Response:** `SendSuccess(Result)`, `SendSuccess(Message)`, `SendSuccess(Message, Result)`, `SendError(Code, Message)`, `SendError(Code, Message, Result)` (the 3-arg form attaches a structured error payload that the transport delivers to MCP clients as `structuredContent` + appended text — see Wire Protocol), `SendUnsupportedEngineVersion(RequiredVersion, Feature)` (standardized `UNSUPPORTED_ENGINE_VERSION` error for features that require a newer UE than the running editor — use instead of a fake-success stub)

**Test seams:** `MakeTestContext()`, `MakeTestContextWithCapture()` (with `FTestResponseCapture`)

### Async Response Token (`FAsyncResponseToken`)

The stack-local `FHandlerContext` is a value type that cannot safely outlive its handler invocation. `Ctx.MakeAsyncToken()` produces a shareable late-response handle carrying `RequestId`, `Method`, a weak subsystem pointer, and a weak handle to any shared-owned test capture. The raw stack-owned `FResponseCapture*` is deliberately not forwarded because it would dangle after the handler returns. A token can route `SendSuccess` / `SendError` after that return, but it does **not** retain the dispatcher's active-request serialization or reopen the unattended scope.

Request-owned work that must survive a safe-point hop needs the retained continuation as well as the response token. `asset.import` always uses `PinWrightSafePoint::DeferRequestToSafePoint`: it takes one editor core-ticker hop, keeps later RPCs queued until the import continuation returns, and runs import, primary rename, verification, and response inside the reopened unattended scope. Its previous `GEditor->GetTimerManager()->SetTimerForNextTick` path used `UEditorEngine`'s timer, not `UWorld::TimerManager`, and was already outside `UWorld::Tick`; the defect was releasing dispatcher request ownership before the callback, not an unsafe world-tick phase.

Calling the subsystem's `SendAutomationResponse` / `SendAutomationError` directly from a detached lambda remains the historical anti-pattern because it bypasses the token's request metadata and async-capable test capture. Genuine multi-subscriber `SubRequestId` fan-outs are the exception: those responses target request ids the originating context does not own, so they legitimately stay on the raw subsystem API. Capture-test coverage lives in `Tests/Core/AsyncHandlerResponseCaptureTest.cpp`.

## State Management

All shared mutable state lives in `FPluginState` (Meyer's singleton):

| Component | Responsibility |
|---|---|
| `FBlueprintTracker` | Inflight operation tracking, busy state, blueprint registry |
| `FSaveThrottler` | Prevents rapid repeated saves of the same asset |
| `FJobRegistry` | Ticket/progress/event registry for long-running RPCs (see [wiki-src/system.md](wiki-src/system.md)) |
| `SequenceRegistry` | Sequencer track/section tracking |
| `NiagaraRegistry` | Niagara system instance tracking |
| `CachedActorSnapshots` | Actor transform snapshots for automation |

Access: `FPluginState::Get().Blueprints()`, `FPluginState::Get().Saves()`, etc.
Thread safety: all accessors use `FCriticalSection` + `FScopeLock`.

## Wire Protocol

This section is for transport maintainers and test authors. Agents should use the MCP client-exposed `call` tool, e.g. `mcp__pinwright__call` in this project, rather than raw HTTP probes or shell JSON-RPC.

Single endpoint: `POST /mcp`. JSON-RPC 2.0 envelope. Stateless — no `Mcp-Session-Id` header is issued or required. No batch (JSON arrays) in v1. A `tools/call` carrying an MCP progress token can keep its `POST` response open as an SSE stream for progress and terminal frames unless `args.wait` is explicitly `false`; `GET /mcp` and unsolicited server-push notifications are not supported.

```
Request:           {"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"call","arguments":{"method":"actor.list","args":{}}}}
Success response:  {"jsonrpc":"2.0","id":1,"result":{...}}
Error response:    {"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"...","data":{...}}}
Notification:      no `id` field — server responds HTTP 202 with empty body
```

Supported protocol methods: `initialize`, `notifications/initialized`, `ping`, `tools/list`, `tools/call`. Anything else returns JSON-RPC `-32601`. Numeric error codes follow JSON-RPC 2.0: `-32700` parse, `-32600` invalid request, `-32601` method not found, `-32602` invalid params, `-32603` internal.

The MCP server exposes exactly **one** tool named `call`. `tools/list` returns one descriptor; the dotted RPC method (e.g. `asset.dump_folder`) is passed as a string value in the `method` argument, never as a tool name. There is no dotted-to-underscore flattening and no reverse map.

`tools/call` routes by argument shape:

- `arguments = {}` → wiki root namespace index (markdown in `content[]`).
- `arguments = { method: "<path>" }` (no `args`) → wiki page for that namespace or method (markdown in `content[]`).
- `arguments = { method: "<dotted.name>", args: {...} }` → dispatcher executes the RPC.
- `path` is accepted as an alias for `method`; both present with conflicting values → `-32602`.
- Any other top-level argument field → `-32602` naming the bad field(s), the valid set (`method`, `args`), and the wiki root index path.

`tools/call` result shape — handler success and handler error both surface inside the MCP result envelope, **not** as JSON-RPC errors:

```
Success:  {"content":[{"type":"text","text":"<stringified result>"}],"structuredContent":{...},"isError":false}
Error:    {"content":[{"type":"text","text":"[CODE] Message\n<serialized error payload, if any>"}],"structuredContent":{...},"isError":true}
```

Handler error responses are **not** text-only: a handler-attached error payload (the 3-arg `FHandlerContext::SendError(Code, Message, Result)`) is folded into the tool-call error by `MakeToolCallError` — the serialized JSON is appended to the text block after the `[CODE] Message` line and the same object is set as `structuredContent` alongside `isError: true`. Historically this payload was dropped at two separate sites on the error path — the transport completion lambda built `MakeToolCallError` from code+message strings only, and `RpcDispatcher`'s text-formatter capture path forwarded `nullptr` instead of `Capture.Result` — even though the `TSharedPtr<FJsonObject>` traveled intact through `SendAutomationResponse` → the transport's `ResolveCompletion`. Both were fixed (2026-06). Because error payloads are handler-controlled and unbounded, the oversize-spill marking (`HttpResponseSpill::MarkOversizedToolResult`) covers both the success and error branches.

Internal/unexpected handler errors (empty code, `INTERNAL`, or `ERR_AUTOMATION_ERROR`) additionally carry a `report` hint — the string `call("support")` — which `MakeToolCallError` folds into both the text block and a `report` field. It nudges the agent to file a bug report for genuine plugin faults only, never for caller-side param/validation errors. See [wiki-src/support.md](wiki-src/support.md).

Wiki-discovery responses carry the rendered markdown in `content[0].text` and omit `structuredContent`. JSON-RPC error codes are reserved for envelope-level failures (parse error, unknown protocol method, malformed envelope, internal exception). Handler-domain errors (invalid params, asset not found, validation failure) ride inside the `tools/call` result with `isError: true`.

Long-running operations return a job ticket synchronously as a normal successful `tools/call` result; agents poll progress by calling the `call` tool again with `method: "system.job_status"`. See [wiki-src/system.md](wiki-src/system.md).

Clients may receive an `outputTooLong` result inside the `tools/call` envelope when a serialized response exceeds the configured HTTP response spill threshold. The full payload is written under `Saved/PinWright/HttpResponses/<startup-datetime>/<timestamp>_<guid>.json`, and the reduced response includes `file.path`, `contentType`, `payload`, `characters`, `fileCharacters`, and `threshold`. The default threshold is 10,000 characters and is configurable in `Project Settings > Plugins > PinWright > HTTP`. The file holds the payload itself — the response's `structuredContent` object at the file's top level, or the text content verbatim when there is no structured half — never the MCP envelope, so following `file.path` needs no unwrapping; `file.payload` names which of the two shapes was written. An error spills its text even when it has a `structuredContent`, because the `[<CODE>] <message>` line exists only in the text block and the inline one is replaced by the spill notice. `fileCharacters` is one copy of the payload but pretty-printed, and pretty-printing costs per line, so a nested payload's file runs well above the condensed `characters` count.

See [wiki-src/mcp-transport.md](wiki-src/mcp-transport.md) for the full transport reference.

Spill files are temporary generated editor output. On subsystem startup, datetime directories older than 24 hours are pruned.

## Testing Architecture

Tests in `Source/PinWright/Private/Tests/` — compiled into the main module DLL (no separate tests module; `WITH_DEV_AUTOMATION_TESTS` self-strips registration in shipping). Each integration sub-module additionally carries its own `Private/Tests/` tree (typed tests moved with their handlers, compiled into the sub-module DLL); those tests only run on hosts where the owning engine plugin is enabled, so check the startup `PinWright integrations:` line when test counts look low.

| Category | What | Files |
|---|---|---|
| **Core unit tests** | Transport, Dispatcher, Catalog, HandlerContext, PluginState, JsonRpc | `TestMcpTransport`, `TestDispatcher`, `TestToolCatalog`, `TestHandlerContext`, `TestPluginState` |
| **Utility tests** | PathUtils, ClassUtils, JsonUtils, LevelSaveLoadUtils | `TestPathUtils`, `TestClassUtils`, `TestJsonUtils`, `TestLevelSaveLoadUtils` |
| **Contract tests** | All registrations valid, no duplicates, method name format | `TestContractConsistency` |
| **Handler domain tests** | Per-domain registration + no-crash tests | `TestActorHandlers`, `TestBlueprintHandlers`, etc. |
| **Smoke tests** | All handlers with empty/null payload, response validation | `TestHandlerSmoke` |
| **MCP integration** | Transport-level JSON-RPC 2.0 round-trips against the running plugin, used by transport tests rather than agent workflows | `TestMcpTransport` |
| **Compiler tests** | Blueprint code compiler/decompiler | `TestCompilerIntegration`, `TestCompilerResolvers`, `TestDecompiler` |

**Shared test utilities:** `TestUtils.h` provides `InvokeHandler()`, `InvokeHandlerWithCapture()`, `IsHandlerRegistered()`.

**Test context factories:**
- `FHandlerContext::MakeTestContext()` — null subsystem, responses no-op
- `FHandlerContext::MakeTestContextWithCapture()` — captures responses into `FTestResponseCapture`

**Capture tests cannot see wire shaping.** `FResponseCapture` preserves everything the handler sends — including the 3-arg `SendError` payload — so in-process capture tests can never detect a transport-layer regression that drops or reshapes it when building the `tools/call` result. Regressions about what the *client* sees (content blocks, `structuredContent`, `isError`, spill markers) are only observable through the live loopback transport seam in `Tests/Infra/TestMcpTransport.cpp` (real `FMcpTransport` on port 19981 + stub dispatcher + real HTTP POSTs).

## Key Files Quick Reference

| File | Purpose |
|---|---|
| `Public/PinWrightSubsystem.h` | Subsystem (orchestrator) |
| `Private/IntegrationGates.h/.cpp` | Plugin → sub-module → method-prefix gate table; loads enabled integration sub-modules before the registration drain |
| `Source/PinWright<Integration>/*.Build.cs` | Per-sub-module build rules (hard-link engine plugin + PrivateIncludePaths into main Private/) |
| `Private/Transport/SocketHttpServer.h/.cpp` | Socket HTTP/1.1 server + async completions + SSE streaming |
| `Private/Transport/McpRequestCore.h/.cpp` | Transport-agnostic MCP request pipeline (auth, envelope, protocol methods) |
| `Private/JsonRpc.h` | JSON-RPC 2.0 envelope parsing + response building |
| `Private/Dispatch/RpcDispatcher.h/.cpp` | Handler dispatch + auto-validation |
| `Private/Catalog/ToolCatalog.h/.cpp` | Caches the dispatcher handle for the Wiki layer; generates nothing |
| `Private/Catalog/WikiHandler.h/.cpp` | Namespace-tree wiki renderer (discovery surface); inline markdown is the cold-start / not-found fallback |
| `Private/Catalog/WikiDiskGenerator.h/.cpp` | Writes the assembled wiki tree (auto-content + `docs/wiki-src/` overlays) to `Saved/PinWright/wiki/` at editor launch |
| `Private/Handlers/HandlerRegistration.h` | REGISTER_RPC_HANDLER macro |
| `Private/Handlers/HandlerContext.h/.cpp` | Per-request context + test seams |
| `Private/Handlers/ParamSpec.h` | Parameter schema macros |
| `Private/Audit/AuditFramework.h` | Shared audit contract — the one `pass` rule, finding statuses + wire spellings, check-table templates. Header-only so `PinWrightGeometry` includes it cross-module |
| `Private/State/PluginState.h/.cpp` | State singleton |
| `Private/State/BlueprintTracker.h/.cpp` | Blueprint operation tracking |
| `Private/State/SaveThrottler.h/.cpp` | Save throttling |
| `Private/State/JobRegistry.h` | Job ticket registry (owned by FPluginState) |
| `Private/Utils/JobMonitorLog.h` | JSONL event writer for job monitoring |
| `Private/Handlers/System/JobControlHandler.cpp` | system.job_status/list/cancel RPCs |
| `Misc/EngineVersionComparison.h` (engine) | `UE_VERSION_NEWER_THAN_OR_EQUAL` / `UE_VERSION_OLDER_THAN` guards — the UE 5.3-5.8 compat primitive |
| `Private/Material/MaterialInputIterCompat.h` | `ForEachExpressionInput` — version-safe material-input iteration |
| `Source/PinWrightGeometry/Private/Handlers/Geometry/CollisionHelpers.h` | version-safe collision-from-mesh wrapper (moved with the geometry cluster) |
| `Private/Handlers/Audio/MetaSound/MetaSoundPathUtils.h` | MetaSound paged-graph accessors (5.4 vs 5.6+) |
| `Private/Utils/AssetUtils.h/.cpp` | Asset operations |
| `Private/PinWrightSettings.h` | Project Settings UI |
| `PinWright.Build.cs` | Build rules + conditional modules |

## Utils Spotlight: ClassUtils / ResolveUClass

`Utils/ClassUtils.h` exposes `UClass* ResolveUClass(const FString& Input)` — a robust UClass lookup used by handlers that accept either a full class path (`/Script/UMGEditor.WidgetBlueprint`) or a short class name (`WidgetBlueprint`). The resolution chain is:

1. `FindObject<UClass>`
2. `LoadObject<UClass>`
3. Script-package prefix scan (`/Script/Engine`, `/Script/UMG`, `/Script/UMGEditor`, …)
4. `TObjectIterator<UClass>` fallback
5. Retry with `U`/`A` prefix stripped / added

Returns `nullptr` on failure. **Prefer it over constructing `FTopLevelAssetPath` directly** — the latter fires a UE ensure at `Engine/Source/Runtime/CoreUObject/Private/UObject/TopLevelAssetPath.cpp:141` when given a short name like `"WidgetBlueprint"`, which spams Message Log and breaks batch RPCs.

Current callers: `AssetManageHandler::asset.list` and `AssetQueryHandler` (the latter still inlines its own short-name loop — candidate for dedup). See `Private/Utils/ClassUtils.cpp:99-172`.

## See Also

- [reflection-invocation.md](reflection-invocation.md) — Rules for handlers that invoke a UFunction via `ProcessEvent` with a caller-built parameter buffer (FProperty init/destroy lifecycle, `CPF_OutParm`/`CPF_ConstParm` classification, CDO refusal, any-UObject path resolution). Canonical implementation: `object.call_function`.
- [wiki-src/asset.md](wiki-src/asset.md) — Caller-facing `asset.import` overwrite policy, complete multi-output reporting, and retained safe-point behavior.
- [wiki-src/system.md](wiki-src/system.md) — Ticket pattern + JSONL monitor for long-running RPCs: response shape, event schema, settings, method list (absorbed from former `jobs.md`)
- [wiki-src/mcp-transport.md](wiki-src/mcp-transport.md) — Maintainer-level transport reference: JSON-RPC 2.0 envelope, supported protocol methods, the single `call` tool and its three argument shapes, `tools/call` result wrapping, what is and is not supported in v1
- [pwmodel-design.md](pwmodel-design.md) — PinWright Model (`.pwmodel`) design rationale: why a generative mesh format is not an IR, the geometry-op extraction and single-build asset-creation decisions behind the `model` namespace, derived-asset ownership, and the M1-M3 roadmap
- Client-facing RPC reference is the in-process wiki served by the `WikiHandler` RPC; namespace overlays under [wiki-src/](wiki-src/) carry workflow guidance, gotchas, and examples
