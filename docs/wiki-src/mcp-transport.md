# MCP transport

This page is a maintainer-level transport reference, not an operator recipe. Agents and operators must use the configured MCP client and its exposed `call` tool; do not manually probe or invoke the gateway with raw HTTP, `curl`, `Invoke-RestMethod`, `/health`, or legacy `/rpc` calls.

## Routes and response modes

`POST /mcp` is the only HTTP route the plugin exposes internally. The editor speaks JSON-RPC 2.0 on this endpoint; agents talk to it through their MCP client. There is no `/health` and no legacy `/rpc`; `GET /mcp` returns 405, and any other GET (e.g. Codex's OAuth-discovery probes) returns 404. The server is stateless — no `Mcp-Session-Id` is issued or required, and concurrent clients are independent. Responses are plain `application/json` unless the request qualifies for a Server-Sent Events upgrade for live job progress — the default for streaming-capable clients, opt-out via `wait: false` (see [Streaming responses (SSE)](#streaming-responses-sse)); statelessness, auth, and the job-ticket contract are identical in both modes. Every response carries an explicit `Content-Type` — Codex's rmcp stack hard-fails on a response without one (openai/codex#26955).

## Endpoint and port resolution

The listen port is derived from the project path by default — **Auto-derive Port From Project Path** (`bAutoDerivePort`, default on) in Project Settings maps the project path to a deterministic port `19880 + hash(projectPath) % 10240`, yielding a stable, unique port per project folder in the range `19880`–`30119` (10240 slots) with no manual config, so several projects/editors run on one machine without clashing (those ports are machine-specific, so don't commit baked configs). Disabling it binds a fixed `HttpPort` (default `19880`) — the same on every machine, which keeps committed agent configs (e.g. a tracked `.codex/config.toml`) portable across a team.

The MCP client adapter uses `http://127.0.0.1:<port>/mcp`. Direct-HTTP configs need no runtime port lookup; onboarding (the setup screen) resolves the port and, for direct-HTTP configs, bakes the full endpoint URL into the agent's project-local config (`.mcp.json`, `.codex/config.toml`, `.gemini/settings.json`, `.vscode/mcp.json`, Cursor deeplink); stdio proxy configs carry file paths instead of a URL. Clients reconnect on the same endpoint after an editor restart — no MCP-side reload is needed. On a port collision — with auto-derive on (the default), a rare cross-folder hash collision or an OS-reserved port; with auto-derive off, two projects sharing one fixed `HttpPort` — the editor refuses to serve; keep Auto-derive Port on (to give each project a unique port) or set a different fixed `HttpPort`, then re-run onboarding.

The editor also publishes the actually-bound port to `<Project>/Saved/PinWright/gateway-port` (decimal, rewritten on every successful transport start, left in place on shutdown as last-known-good, and **retracted by an editor that is not serving when nothing is listening on the port it names**). The bundled stdio proxy re-reads that file before each forwarded call and constructs `http://127.0.0.1:<port>/mcp` itself (the loopback address and path are hardcoded, so the file can only select a loopback port), preferring the `--port-file` arg onboarding bakes in, then a script-relative fallback for in-project installs with legacy hand-written configs, then an optional `--url` (still accepted for hand-written configs; onboarding does not emit it — a generated config can only exist after the editor has already published the port file). So an onboarding-generated proxy config keeps working when the project moves, the derived port changes, or the port setting changes, with no config edit and no proxy restart. Direct-HTTP configs never consult it.

Last-known-good is only defensible while something still honours the claim, so a bind failure reconciles the file instead of leaving it: retained when a probe finds a listener on the advertised port (normally another editor of the same project, serving correctly — deleting it would break that editor), removed when nothing is listening (the claim is false whoever wrote it, and an absent file makes a client report "editor not running" rather than dialling a dead endpoint). An inconclusive probe counts as listening, so ambiguity never deletes a working endpoint. A successful bind whose port could not be written to the file is treated as not-yet-serving — logged at Error and retried — because a server no client can discover is indistinguishable from one that never bound.

## Authentication

By default every `POST /mcp` request must carry `Authorization: Bearer <token>`. The check runs **before** the JSON-RPC envelope is parsed, so a missing or wrong token never reaches the dispatcher — and a notification (no `id`) sent with a bad token gets a 401 rather than the usual HTTP 202. On failure the transport returns HTTP 401 with a `WWW-Authenticate: Bearer` header and a JSON-RPC error body; because auth precedes parse, the body carries `id: null` (the stdio proxy patches the real id back in for correlation). The error message names the token file path and directs the operator to re-run Install in the setup screen.

The token is 64 lowercase hex characters (32 random bytes), stored one-per-line at `<Project>/Saved/PinWright/gateway-token`, auto-created on editor start and persistent across sessions. The stdio proxy reads the file **per request** (so rotation needs no proxy restart) using the `--token-file <absolute path>` baked into its launch args, with a script-relative fallback path so existing stdio installs keep working without a re-install. Direct-HTTP clients instead carry the literal token in a baked `Authorization` header, so those config files must stay machine-local.

The `bRequireAuthToken` kill switch (**Require Auth Token (Bearer)** in Project Settings → Plugins → PinWright, default on) gates all of this. When off, the server ignores the `Authorization` header entirely — previously-tokened configs keep working because the header is simply not inspected — and Install/Update write agent configs with no token plumbing at all.

## Envelope shape

Every request body is a single JSON-RPC 2.0 envelope:

```json
{"jsonrpc":"2.0","id":1,"method":"<protocol_method>","params":{...}}
```

Successful response:

```json
{"jsonrpc":"2.0","id":1,"result":{...}}
```

Error response (envelope-level — parse error, malformed request, unknown protocol method, internal exception):

```json
{"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"...","data":{...}}}
```

Notifications (requests with no `id`, e.g. `notifications/initialized`) get an HTTP 202 with an empty body and no JSON-RPC response.

Numeric error codes follow JSON-RPC 2.0:

- `-32700` parse error
- `-32600` invalid request
- `-32601` method not found
- `-32602` invalid params
- `-32603` internal error

## Supported protocol methods

Five methods are handled inside the transport. Anything else returns `-32601`.

- `initialize` — handshake. Returns `{ protocolVersion: "2025-06-18", capabilities: { tools: { listChanged: false } }, serverInfo: { name, version }, instructions }`. The optional `instructions` client hint is the approved `mcp-instructions.md` text with `{{PINWRIGHT_WIKI_DIRECTORY}}` replaced by the absolute `<Project>/Saved/PinWright/wiki` path, normalized to forward slashes with no trailing slash. It directs clients to read/search the flat on-disk wiki, distinguishes documentation lookup from RPC execution, and requires `args: {}` for no-argument RPCs.
- `notifications/initialized` — client-sent notification. Server responds HTTP 202 with empty body.
- `ping` — liveness probe plus an operational-readiness snapshot. Returns `editorReady`, `retryable`, `editorLoadingPackage`, and `editorSelectionSetAvailable`; while not ready it also returns `error: "EDITOR_NOT_READY"` and a diagnostic `message`. When the game thread is blocked by a modal dialog it instead returns `error: "EDITOR_BLOCKED_ON_MODAL"` with `retryable: false`, plus `blockedOnModal: true`, `blockedSeconds` (a double), and `modalTitle` when the active modal window could be read. Those three fields are **absent entirely** when not blocked, so existing parsers are unaffected, and `modalTitle` is omitted rather than emitted as `""`.
- `tools/list` — returns `{ tools: [...] }` carrying the single `call` tool descriptor (see below). No pagination in v1.
- `tools/call` — invokes the `call` tool, routed by argument shape (see below).

The direct C++ transport and the stdio proxy's local `initialize` response must render byte-identical `instructions` for the same project. Keep both embedded templates verbatim with `mcp-instructions.md`, apply the same path normalization in both response paths, and keep the focused canonical-file tests green.

## Operational readiness

A successful socket connection or `ping` proves MCP liveness, not that editor-facing operations are safe. `editorReady` becomes true only after `FEditorDelegates::OnEditorInitialized`, package loading is idle, and the selected-actor typed element selection set exists. Until then, `initialize`, `ping`, and `tools/list` remain available for bootstrap, while every `tools/call` shape is rejected before wiki lookup or handler routing with an in-band, retryable `EDITOR_NOT_READY` result.

The transport consumes a game-thread readiness snapshot so the socket I/O thread can reject unsafe calls immediately. `FRpcDispatcher` checks the state again on the game thread for direct callers and requests that raced a readiness transition.

That snapshot freezes — stale-but-positive — the moment a modal dialog owns the game thread, because every writer of it runs on the blocked thread. The transport therefore also reads `ModalStateProbe`, which is latched from inside the nested Slate modal loop and cleared from the subsystem tick, and gates it on `UPinWrightSettings::ModalBlockedReportSeconds` (default 2.0) so a quickly dismissed dialog changes nothing. While blocked, `tools/call` is rejected by the same gate with the non-retryable `EDITOR_BLOCKED_ON_MODAL` result instead of the retryable `EDITOR_NOT_READY`. See `call("unattended")`.

The snapshot freezes the same way when a *handler* stops returning, and nothing broadcasts anything in that case, so the same probe carries a second signal: a heartbeat stamped from the subsystem tick whose **age** the I/O thread reads. Past `UPinWrightSettings::GameThreadStallReportSeconds` (default 90.0) `ping` answers `editorReady: false` with the retryable `EDITOR_GAME_THREAD_STALLED`, naming the in-flight RPC. Unlike the modal case this **never gates `tools/call`** — a stalled thread may still drain, and the queued request runs when it does. See `call("unattended")`.

## The single `call` tool

The MCP server exposes **exactly one tool**, named `call`. There is no per-handler MCP descriptor, no dotted-to-underscore tool-name flattening, and no reverse map. The dotted RPC method (e.g. `asset.dump_folder`) is passed as a *string value* in the `method` argument; the tool name on the wire is always `call`.

`tools/list` returns one descriptor:

```json
{
    "tools": [{
        "name": "call",
        "description": "Invoke an PinWright RPC. Pass method='<namespace.verb>' and args={...} to execute; omit args to fetch the wiki page for the method; omit both to get the root namespace index. 'path' is accepted as an alias for 'method'; any other argument field is rejected.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "method": { "type": "string" },
                "args":   { "type": "object" }
            }
        }
    }]
}
```

## `tools/call` argument shapes

`tools/call` with `name: "call"` routes by argument shape — three modes:

1. **Root index.** `arguments` is empty (no `method`, no `args`) → returns the wiki root namespace index, rendered as markdown via `WikiHandler::RenderPage("", ...)`.
2. **Wiki page.** `arguments` has `method` but no `args` field → returns the wiki page for that namespace or method, rendered as markdown via `WikiHandler::RenderPage(method, ...)`.
3. **Execute.** `arguments` has both `method` and `args` → dispatches the RPC: `method` is the dotted method name, `args` is the params object handed to `FRpcDispatcher`.

Both object layers are type-checked before any wiki lookup or RPC dispatch. A present
`params.arguments` or inner `arguments.args` value must be a JSON object; a string,
array, or `null` returns JSON-RPC `-32602` immediately. Only an omitted field is
eligible to synthesize an empty object: omitting outer `arguments` behaves like
`arguments: {}`, while omitting inner `args` deliberately selects the wiki-page route.
To execute a parameterless RPC, send `args: {}` explicitly.

`path` is accepted as an alias for `method`; supplying both with conflicting values is rejected with `-32602`. Any other top-level argument field is rejected with `-32602` naming the offending field(s), the valid fields (`method`, `args`), and the wiki root index path. Previously unknown fields were silently ignored and the request fell through to the root-index response.

## World preconditions on mutating calls

Every registered mutating RPC accepts an optional `expectWorld` string inside `args`. Use the
`world` value returned by one mutating call as the next call's `expectWorld`; the full world object
path is canonical, and the corresponding package path is accepted too. The dispatcher checks the
assertion on the game thread after queue routing and parameter validation, and the shared safe-point
continuation hook checks it again immediately before deferred work enters the handler. A mismatch
returns `WORLD_MISMATCH`, with `expectedWorld` and the actual `world` in structured error data, and
the handler does not run.

Omitting `expectWorld` preserves the previous behavior. Mutating responses carry the resolved target
world object path in `world`, including successful and error responses, formatted and asynchronous
handler responses, and fanout completions. The target policy follows the handler family:
editor-world mutators use the editor context, while runtime UI and Niagara-spawn mutators use the
same PIE-aware selector as their handlers. The echo is applied at
`UPinWrightSubsystem::SendAutomationResponse`, the single transport-facing decorator; individual
handlers do not need to add it. This is an optimistic concurrency guard between calls, not a world
lock: callers still need project-level coordination for a sequence of operations.

## `tools/call` result wrapping

The dispatcher's success / error is folded into the MCP tool-call result, **not** into a JSON-RPC error.

Execution success:

```json
{
    "content":[{"type":"text","text":"<JSON-stringified result>"}],
    "structuredContent":{...},
    "isError":false
}
```

Handler error:

```json
{
    "content":[{"type":"text","text":"[ERROR_CODE] message\n<serialized error payload, if any>"}],
    "structuredContent":{...},
    "isError":true
}
```

Handler errors are not text-only: when the handler attached a structured error payload (the 3-arg `FHandlerContext::SendError(Code, Message, Result)`), the transport's `MakeToolCallError` appends the serialized JSON to the text block after the `[CODE] Message` line and sets the same object as `structuredContent`. Errors without a payload carry just the `[CODE] Message` text and omit `structuredContent`. Error payloads are handler-controlled and unbounded, so the oversize-spill path applies to both success and error results.

Every error result whose method is known also carries a doc pointer: `MakeToolCallError` appends a final `Docs: <absolute wiki page path>` line to the text block and sets a top-level `docs: {page, wiki}` field - the exact method page when it exists on disk, else the nearest parent namespace page (found by stripping dotted segments), else `index.md`. The pointer is omitted entirely when no `Saved/PinWright/wiki/` tree exists on disk yet. The structured `docs` field survives the oversize-spill rewrite; success results carry no doc pointer.

Wiki discovery prefers an on-disk reference: `content[0].text` is its JSON string, `content[0].mimeType` is `application/json`, and `structuredContent` carries the same `{root|page, wiki, hint}` object. On cold start, before the disk tree exists, the fallback returns rendered markdown in `content[0].text` and omits `structuredContent`.

Older clients read the text block; newer clients consume `structuredContent` directly. Handler-domain errors (invalid params, asset not found, validation failure) ride inside this envelope — JSON-RPC error codes are reserved for envelope-level failures.

## Long-running operations

For non-streaming requests (plain-JSON clients, or an explicit `wait: false` opt-out), long-running RPCs (`system.run_tests`, `level.build_lighting`, `asset.dump_folder`, `editor.screenshot`, save operations, …) return a job ticket synchronously as a normal successful `tools/call` result:

```json
{
    "status":"running",
    "ticket_id":"j_20260426T123456_a1b2c3d4",
    "monitor_path":"Saved/PinWright/jobs.jsonl",
    "method":"<method-name>",
    "started_at":"2026-04-26T12:34:56Z"
}
```

Poll progress by calling the same `call` tool again with `method: "system.job_status"` and `args: { ticket_id: "..." }` (or tail `Saved/PinWright/jobs.jsonl`) for `started` / `progress` / `completed` / `failed` / `cancelled` events. Big jobs (`system.run_ubt`, `system.run_tests`, level builds) emit incremental progress events (`RecordProgress`) between start and completion, not just endpoints. A streaming request — the default for clients that send a `progressToken` and `Accept: text/event-stream` (see [Streaming responses (SSE)](#streaming-responses-sse)) — receives those same events as server-push `notifications/progress` frames and the terminal result on the same call instead of polling — the ticket contract is identical either way, and `jobs.jsonl` / `system.job_status` are unchanged. See [system](system.md) for the full ticket pattern.

## Streaming responses (SSE)

Streaming is served by the transport itself: a custom socket-based HTTP/1.1 server (`Transport/SocketHttpServer.h/.cpp`, non-blocking single I/O thread) that behaves identically on UE 5.3–5.8, and is the plugin's only transport. It deliberately does not use UE 5.8's `IHttpRouter` incremental-write flags (`MultipleWriteStream` et al.) — those exist only in 5.8, and engine-source patching is off the table, so the raw-socket server is what makes streaming uniform across the supported engine range. The request logic — envelope parse, the five protocol methods, dispatcher routing — lives in `Transport/McpRequestCore.h/.cpp`. SSE is always available and is the **default** for streaming-capable requests, gated **per request** as below; a request that doesn't qualify (or opts out) gets plain JSON.

A request blocks-and-streams (SSE progress frames plus the terminal result on one call) **iff both** hold; any other combination returns plain `application/json` byte-identically to the ticket flow:

1. the JSON-RPC envelope carries `params._meta.progressToken`;
2. the request's `Accept` header includes `text/event-stream`.

The caller opts **out** per call with tool-call `args: {wait: false}`, which returns the job ticket immediately (fire-and-forget; poll `system.job_status`). `wait` absent or `true` blocks-and-streams — matching UE 5.8's block-by-default model (Epic's built-in server is SSE-always with no opt-out at all; ours keeps the opt-out and the ticket fallback). `wait` is an internal transport parameter: it is stripped from `args` before the handler sees them. Known caveat: Codex's `tool_timeout_sec` (300s in the generated config) may not reset on progress notifications, so long jobs from Codex may need `wait: false` + polling — unverified.

A streaming response is `Content-Type: text/event-stream` with frames of the form `event: message\r\ndata: <json>\r\n\r\n`:

- **Progress frames** are JSON-RPC `notifications/progress` notifications carrying `progressToken`, `progress`, an optional `total`, `message`, and **always** `ticket_id`. Job tickets stay canonical — a dropped stream degrades to polling `system.job_status` with that ticket, losing nothing but liveness.
  - `progressToken` is **the client's own token**, echoed verbatim from `params._meta.progressToken` (string or number, whichever was sent). MCP admits progress only for tokens "provided in an active request", so the ticket id travels in `ticket_id` instead of being substituted for the token — a substituted token is one the client cannot correlate with the call it is waiting on, and is entitled to discard.
  - `progress` never decreases across the life of the request. It is the reporter's own numerator when one was given, otherwise a count of accepted events. A reporter whose number goes backwards is **held at the previous value, never advanced past it** — a stalled job repeats its numerator rather than drawing a bar that moves while nothing happens — and the raw value is preserved in the `jobs.jsonl` line as `reportedProgress` + `progressHeldAtPrevious`.
  - `total` is present only when the reporter knows its denominator, and is **omitted rather than zero-filled** — a zero total renders as a finished bar.
- **The final frame** is the normal JSON-RPC response, exactly what the plain-JSON path would have returned; it terminates the stream.
- **Heartbeats** are SSE comment frames (`: ping`) sent every `SseHeartbeatSeconds` (default 15) to keep idle proxies and client timeouts from killing a quiet stream.

Bearer auth is checked once when the POST opens (same 401 semantics as [Authentication](#authentication)), and the `Origin` header is validated as loopback-only. The bundled stdio proxy participates: when a forwarded call streams, it forwards progress notifications to its stdout as they arrive, and it remains the primary client path (it survives editor restarts).

## Oversized responses

`tools/call` results larger than the configured spill threshold (default 10,000 serialized characters; configurable in **Project Settings → Plugins → PinWright → HTTP**) are written to `Saved/PinWright/HttpResponses/<startup-datetime>/<timestamp>_<guid>.json` and replaced inside the `tools/call` envelope by a small `outputTooLong` reference carrying `file.path`, `contentType`, `payload`, `characters`, `fileCharacters`, and `threshold`. Spill directories older than 24 hours are pruned at editor startup.

**The file is the payload, not the envelope around it.** Reading `file.path` is equivalent to having read the response inline: the file's top level is the response's `structuredContent` object (`payload: "structuredContent"`, `contentType: application/json`), or — for a result with no structured half, such as the wiki markdown fallback — the text content verbatim, unescaped (`payload: "text"`, `contentType: text/plain`). No `content` / `structuredContent` / `isError` wrapper keys are written, so no unwrapping step is needed and a field read straight off the file means what it says.

**An error always spills its text** (`payload: "text"`), even when it has a `structuredContent`, because the two halves of an error are not copies of each other: `structuredContent` holds only the handler payload, while the `[<CODE>] <message>` line, the report hint and the `Docs:` pointer exist only in the text block — and the inline text block is what the spill notice replaces. The text repeats the payload, so nothing is lost. The top-level `docs` field survives the rewrite either way.

`characters` is what was measured against the threshold (one condensed copy of the payload); `fileCharacters` is the size on disk, which is one copy but pretty-printed — and pretty-printing costs per line, so a deeply nested payload lands well above the condensed count.

## Shutdown ordering

Subsystem teardown stops new request handoffs before it destroys the dispatcher, while completion routing and the socket I/O thread are still alive. `QuiesceRequestIntake` closes the atomic intake gate and takes the same lock as the final request-delegate handoff before unbinding it. Dispatcher destruction then severs raw dispatcher access and abandons registered asynchronous lifetimes, allowing their owners to cancel tickers and enqueue typed terminal errors. Any remaining completions receive `BRIDGE_SHUTTING_DOWN`. Queued request callbacks pin a weak dispatcher/lifetime before use, so work already marshalled to the game thread cannot call a destroyed owner.

Only after those responses are queued does transport stop begin its bounded drain. Each connection flushes pending response bytes, sends a write-side FIN, and continues reading and discarding inbound bytes during a bounded linger. This avoids an immediate close with unread request data turning into a TCP reset that discards the response at the peer. Connections are destroyed only after the peer closes or the drain/linger bounds expire.

## Transport defaults

- HTTP transport: enabled. The release package keeps it on because the plugin exists to provide editor automation.
- Bind policy: loopback-only.
- Port: per-project derived by default, see [Endpoint and port resolution](#endpoint-and-port-resolution).
- Auth: bearer token required by default, see [Authentication](#authentication).
- Request body limit: 1 MB.
- Default request timeout: 120 seconds.
- Maximum request timeout: 300 seconds.

Review the plugin's settings under **Plugins -> PinWright** before using the gateway on a shared machine, remote desktop, studio build farm, or CI environment. If a client cannot connect, confirm that the Unreal Editor is open, the plugin is enabled, and no other local process is using the configured port.

## Stdio proxy lifecycle tools

The direct in-editor endpoint has exactly one tool, `call`. A client configured through the bundled stdio proxy sees four tools: `call`, `editor_start`, `editor_restart`, and `editor_prepare_tests`.

- `editor_start({})` opens the project's `.uproject` through the registered file association (on Linux, which has none, it spawns the resolved editor directly, borrowing `DISPLAY`/`XAUTHORITY` from your desktop session when the proxy runs over SSH). Unreal owns prompts, compilation, and module loading for this path, and the engine comes from the project's `EngineAssociation` (on Linux, `UE_<version>=<engine root>` or a GUID key under `[Installations]` in `~/.config/Epic/UnrealEngine/Install.ini`), or from `$PINWRIGHT_ENGINE_ROOT` when set. An unresolved association fails with `EDITOR_ENGINE_NOT_FOUND`.
- `editor_start({"map": "/Game/Maps/MyLevel"})` boots into a level. The map token is the first argument after the `.uproject`, so a map forces direct spawn and defaults `unattended_script` on.
- `editor_restart({"map": "/Game/Maps/MyLevel"})` quits through `editor.quit`, waits for the endpoint to go down, then starts a fresh editor. Dirty or modal-blocked editors are reported rather than bypassed.
- For tests, `editor_prepare_tests({"filter": "Project.Tests"})` is the only command-returning proxy verb. The `filter` is mandatory, must be non-empty, and has no default. The live-editor guard runs first and is reported as structured observation: a detected editor is `EDITOR_ALREADY_RUNNING`, an unavailable probe is `not_probed`, and only a safe probe proceeds to `COMMAND_READY`.

All proxy-local lifecycle tools ping PinWright MCP before resolving or launching anything. If an editor answers, even while starting, lifecycle tools fail with `EDITOR_ALREADY_RUNNING`. A forwarded `call` while no editor is reachable reports `EDITOR_NOT_RUNNING`; an unavailable probe never licenses a second editor start.

The dotted `system.run_ubt` and `system.run_tests` RPCs remain separate live-editor operations reached through `call`. They cannot start a stopped editor; use `editor_start` for a live editor or `editor_prepare_tests` to prepare a cold test command for the caller.

## Cold test commands from editor_prepare_tests

`COMMAND_READY` contains the resolved `UnrealEditor-Cmd` executable (off Windows, `UnrealEditor` when no `-Cmd` twin is built), the absolute project path, launch `argv`, explicit absolute `logPath`, checker executable, checker `argv`, and the `EngineAssociation`-resolved engine root. The verb returns immediately. It does not compile, launch, wait, kill, or return a test verdict.

The caller runs the returned launch command and then the returned checker command with the exact same log path. The launch uses comma-separated `-ExecCmds` with `Automation RunTests <filter>,Quit`, `-TestExit="Automation Test Queue Empty"`, `-Abslog=<same absolute logPath>`, `-unattended`, `-nopause`, `-nocefaccelpaint`, `-ddc=InstalledNoZenLocalFallback`, and `-log`. It uses a real RHI and does not add `-NullRHI`.

The command shape is:

```powershell
$log = "$env:HOST_ROOT\Saved\PinWright\test-runs\<run>\automation.log"
& "$env:UE_ROOT\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$env:HOST_ROOT\<HostProject>.uproject" `
  '-ExecCmds=Automation RunTests Project.Tests,Quit' `
  '-TestExit=Automation Test Queue Empty' `
  "-Abslog=$log" `
  -unattended -nopause -nocefaccelpaint -log
```

`check_suite_log.py` is the fail-closed verdict authority. The caller runs it under Unreal's bundled Python interpreter after the launch. It requires the counted `Automation Test Queue Empty <N> tests performed.` drain marker and distinguishes incomplete, empty, failed, skipped, and clean measurements. `PINWRIGHT_ASSERTIONS_SKIPPED` remains a distinct `COMPLETED_WITH_SKIPS` outcome, not a clean result.

The checker states are:

| state | meaning | code |
| --- | --- | --- |
| `CRASHED` | the editor died: a fatal/assert banner, or a non-ensure crash report in the run window | `EDITOR_TESTS_CRASHED` |
| `DID_NOT_COMPLETE` | the queue never drained and no crash evidence exists, so the run was killed | `EDITOR_TESTS_INCOMPLETE` |
| `NO_TESTS` | nothing was enqueued or recorded a success | `EDITOR_NO_TESTS` |
| `COMPLETED_WITH_FAILURES` | the run drained with failures | `EDITOR_TESTS_FAILED` |
| `COMPLETED_WITH_SKIPS` | the run drained but a skip marker was emitted | `EDITOR_TESTS_SKIPPED` |
| `COMPLETED_CLEAN` | the run drained and every assertion ran | - |

## Not supported in v1

- `GET /mcp` SSE channel (server-initiated streams) — `GET /mcp` returns 405; streaming exists only as the per-request upgrade on `POST /mcp` described above
- Server-push outside an opted-in stream (no unsolicited notifications, no log streaming)
- `Mcp-Session-Id` issuance or validation (server is stateless, streaming or not)
- JSON-RPC batch requests (request arrays)
- `resources`, `prompts`, `sampling`, `elicitation` capabilities
- Per-handler MCP tools (the surface is one `call` tool; dotted RPC names are string values, not tool names)
- Namespace filtering at `initialize` time

## See also

- [`system`](system.md) — the job-ticket verbs (`system.job_status`, `system.job_list`, `system.job_cancel`) that this transport carries.
- [`unattended`](unattended.md) — launch flags and modal suppression for sessions with no human present.
- `docs/arch.md` in the plugin folder — subsystem readiness, request flow, stdio proxy lifecycle, and handler dispatch boundaries. A maintainer document shipped beside the plugin, not a wiki page.
