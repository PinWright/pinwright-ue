# MCP transport

This page is a maintainer-level transport reference, not an operator recipe. Agents and operators must use the configured MCP client and its exposed `call` tool; do not manually probe or invoke the gateway with raw HTTP, `curl`, `Invoke-RestMethod`, `/health`, or legacy `/rpc` calls.

## Routes and response modes

`POST /mcp` is the only HTTP route the plugin exposes internally. The editor speaks JSON-RPC 2.0 on this endpoint; agents talk to it through their MCP client. There is no `/health` and no legacy `/rpc`; `GET /mcp` returns 405, and any other GET (e.g. Codex's OAuth-discovery probes) returns 404. The server is stateless — no `Mcp-Session-Id` is issued or required, and concurrent clients are independent. Responses are plain `application/json` unless the request qualifies for a Server-Sent Events upgrade for live job progress — the default for streaming-capable clients, opt-out via `wait: false` (see [Streaming responses (SSE)](#streaming-responses-sse)); statelessness, auth, and the job-ticket contract are identical in both modes. Every response carries an explicit `Content-Type` — Codex's rmcp stack hard-fails on a response without one (openai/codex#26955).

## Endpoint and port resolution

The listen port is derived from the project path by default — **Auto-derive Port From Project Path** (`bAutoDerivePort`, default on) in Project Settings maps the project path to a deterministic port `19880 + hash(projectPath) % 10240`, yielding a stable, unique port per project folder in the range `19880`–`30119` (10240 slots) with no manual config, so several projects/editors run on one machine without clashing (those ports are machine-specific, so don't commit baked configs). Disabling it binds a fixed `HttpPort` (default `19880`) — the same on every machine, which keeps committed agent configs (e.g. a tracked `.codex/config.toml`) portable across a team.

The MCP client adapter uses `http://127.0.0.1:<port>/mcp`. Direct-HTTP configs need no runtime port lookup; onboarding (the setup screen) resolves the port and, for direct-HTTP configs, bakes the full endpoint URL into the agent's project-local config (`.mcp.json`, `.codex/config.toml`, `.gemini/settings.json`, `.vscode/mcp.json`, `.cursor/mcp.json`); stdio proxy configs carry file paths instead of a URL. Clients reconnect on the same endpoint after an editor restart — no MCP-side reload is needed. On a port collision — with auto-derive on (the default), a rare cross-folder hash collision or an OS-reserved port; with auto-derive off, two projects sharing one fixed `HttpPort` — the editor refuses to serve; keep Auto-derive Port on (to give each project a unique port) or set a different fixed `HttpPort`, then re-run onboarding.

Which launches bind: an editor whose `-ExecCmds` names an `Automation` command (`UnrealEditor-Cmd <uproject> -ExecCmds="Automation RunTests X;Quit"`) does **not** start the transport, so a project test run never holds the checkout's port, refuses nobody's `editor_start` / `editor_run_tests`, and leaves `gateway-port` and `jobs.jsonl` alone. `-PinWrightTransport` opts such a run back in (`editor_run_tests` passes it: PinWright's own suite exercises the transport). `-PinWrightNoTransport` keeps any launch off the port and wins over `-PinWrightTransport`. Commandlets never start it. An `editor_start` whose `extra_args` carry such an `-ExecCmds` therefore needs `wait: "exit"`, or `-PinWrightTransport` for `wait: "ready"`.

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
array, or `null` returns JSON-RPC `-32602` immediately. One exception, for clients that
serialize nested objects as JSON strings: an inner `args` string that parses to a JSON
object is accepted as that object (the stdio proxy applies the same coercion before its
`wait` check); any other `args` string stays `-32602`, and outer `arguments` is never
coerced. Only an omitted field is
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

For non-streaming requests (plain-JSON clients, or an explicit `wait: false` opt-out), long-running RPCs (`system.run_tests`, `level.build_lighting`, `asset.dump_folder`, save operations, …) return a job ticket synchronously as a normal successful `tools/call` result (a ticketed verb that finishes inline, such as `editor.screenshot`, instead replies after the work with its terminal result plus `ticket_id`):

```json
{
    "status":"running",
    "ticket_id":"j_20260426T123456_a1b2c3d4",
    "monitor_path":"Saved/PinWright/jobs.jsonl",
    "method":"<method-name>",
    "started_at":"2026-04-26T12:34:56Z"
}
```

Poll progress by calling the same `call` tool again with `method: "system.job_status"` and `args: { ticket_id: "..." }` (or tail `Saved/PinWright/jobs.jsonl`) for `started` / `progress` / `completed` / `failed` / `cancelled` events. Big jobs (`system.run_tests`, level builds) emit incremental progress events (`RecordProgress`) between start and completion, not just endpoints. A streaming request — the default for clients that send a `progressToken` and `Accept: text/event-stream` (see [Streaming responses (SSE)](#streaming-responses-sse)) — receives those same events as server-push `notifications/progress` frames and the terminal result on the same call instead of polling — the ticket contract is identical either way, and `jobs.jsonl` / `system.job_status` are unchanged. See [system](system.md) for the full ticket pattern.

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

Bearer auth is checked once when the POST opens (same 401 semantics as [Authentication](#authentication)), and the `Origin` header is validated as loopback-only. The bundled stdio proxy participates: when a forwarded call streams, it forwards progress notifications to its stdout as they arrive, and it remains the primary client path (it survives editor restarts). It serves forwarded calls concurrently, so a long stream never blocks other calls, and it stops relaying a stream after 30 minutes without a final frame, returning an error that names the `ticket_id` to poll with `system.job_status` (the job keeps running).

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

The direct in-editor endpoint has exactly one tool, `call`. A client configured through the bundled stdio proxy (`Content/Python/mcp_proxy.py`) also sees seven proxy-local tools that answer while no editor is running:

| tool | required params | does |
| --- | --- | --- |
| `editor_start` | `mode` (`visible` \| `offscreen` \| `headless`), `reason` (string) | starts this project's editor; optional `map`, `wait` (`ready` default, or `exit`), `extra_args`, `unattended_script`, `display` (`desktop` default, `xvfb`, `xephyr`), `slot_wait`, `timeout`, `allow_build` (visible only) |
| `editor_restart` | `mode`, `reason` | `editor.quit` (optional `save` / `discard`), waits for the endpoint to go down, then runs the `editor_start` path; optional `map`, `extra_args`, `unattended_script`, `timeout`, `allow_build` |
| `editor_run_tests` | `filter`, `reason`, `mode` | launches the automation suite, returns once the first test has started; optional `slot_wait` |
| `editor_test_status` | exactly one of `logPath`, `runId` | non-blocking progress; the verdict once finished |
| `editor_list` | none | read-only census of every Unreal editor process on the machine |
| `editor_build` | `reason` | builds this project's `<Project>Editor` Development target through the capped supervisor, returns at once |
| `editor_build_status` | `logPath` | non-blocking build status (`stale` when sources changed mid-build), UBT result and every error line |

- **`mode` and `reason` have no default.** Missing: `MISSING_REQUIRED_PARAM`. Any `mode` other than the three words below, including a boolean: `INVALID_MODE`, whose message lists all three. A `reason` that is not a string, is blank after whitespace runs collapse to one space, or exceeds 300 characters: `INVALID_REASON` (refused, never truncated). `editor_restart` validates both before stopping anything.
- **Modes** (the same three words in `editor_list`):

| mode | flags | window | RHI | cap / priority | use it for | Windows vs Linux |
| --- | --- | --- | --- | --- | --- | --- |
| `visible` | none added | yes | real | uncapped, normal priority; Windows: started by the uncapped supervisor through WMI so it survives the MCP client, Linux: direct detached spawn | a person watching or using the editor | Linux needs an X display: over SSH the proxy borrows `DISPLAY`/`XAUTHORITY` from the user's desktop session, else `EDITOR_NO_DISPLAY` |
| `offscreen` | `-RenderOffScreen -unattended -RunningUnattendedScript -nopause -nosplash -nocefaccelpaint` | no | real | capped supervisor, BelowNormal | unattended work that renders or captures; full-suite verdicts | Windows: `CREATE_NO_WINDOW` and the null platform application; Linux: no display needed (SDL `dummy` video driver) |
| `headless` | `-NullRHI` plus the `offscreen` flags | no | null | capped supervisor, BelowNormal | unattended work that never renders or reads pixels | same as `offscreen` |

- **`-RenderOffScreen` stays in `headless` on purpose.** `-NullRHI` only replaces the renderer; `-RenderOffScreen` is what selects the null platform application (`WindowsPlatformApplicationMisc.cpp:171-175`, `LinuxPlatformApplicationMisc.cpp:619-624`) and SDL's `dummy` video driver on Linux (`LinuxPlatformApplicationMisc.cpp:366-371`). Without it a NullRHI editor still opens OS windows and still needs an X display. Render, capture and screenshot verbs cannot produce pixels in `headless`.
- **`unattended_script`** governs `visible` only: it opts that window into `-RunningUnattendedScript` and defaults on when `map` is given. `offscreen` and `headless` always carry it and suppress modal dialogs; `unattended_script` cannot turn that off. Capped launches run under the capped supervisor, `Content/Python/pinwright_supervisor.py` (internal to the proxy, no CLI): memory cap at 60% of physical RAM (Windows: per process; Linux: one cap over the whole process tree), BelowNormal priority, kill-on-close for the editor's children.
- **One launch path.** Every launch is a detached spawn of the editor the project's `EngineAssociation` resolves to (on Linux, `UE_<version>=<engine root>` or a GUID key under `[Installations]` in `~/.config/Epic/UnrealEngine/Install.ini`), or of `$PINWRIGHT_ENGINE_ROOT` when set. An unresolved association fails with `EDITOR_ENGINE_NOT_FOUND`, never a fallback to another engine. There is no OS `.uproject` file-association launch. Every editor outlives the proxy and the MCP client: a client disconnect ends only the wait. On Windows every editor, visible included, is started by the supervisor through WMI, outside the client's process tree; the result's `detached` says whether that held.
- **`timeout`** (seconds, above 0 and at most 3600) is the `wait: "ready"` ceiling for this call; without it the proxy's `--start-timeout` (default 180) applies. Raise it on the launch for a boot known to be slow (cold DDC, shader recompile, a loaded machine): after a timeout there is no waiter left to resume. Out of range, not a number, or combined with `wait: "exit"` (which has no cutoff): `INVALID_ARGUMENTS` (`param: "timeout"`), before anything is started; `editor_restart` forwards it to its start half and validates it before stopping anything. `EDITOR_START_TIMEOUT` carries `timeoutSeconds` and `probeState`, and the editor is left running in every case: `not_ready` (the port answers but the editor is still initializing; `editor_start` is refused `EDITOR_ALREADY_RUNNING` until it is ready, so poll it with `call()` or `editor_list`), `unresponsive` (something answers the port but the ping could not complete; the text quotes the probe's diagnostic) or `not_running` (the process is still booting and has not bound the port; do **not** call `editor_start` again, because the guard only probes the port and a second editor would lose the bind). A GPU-crashed editor (`gpuCrashed` on the ping) never recovers, so the wait fails fast with `EDITOR_UNRESPONSIVE` instead of polling to the ceiling.
- **No build at startup.** A project that sets `bForceCompilationAtStartup` (`[/Script/UnrealEd.EditorLoadingSavingSettings]` in `EditorPerProjectUserSettings`) makes the engine run UnrealBuildTool on the editor target at every start that is not `-unattended`, before PinWright loads (`LaunchEngineLoop.cpp:6581-6640`), which in a shared checkout compiles other agents' half-edited C++ into the loaded binaries. A `visible` launch therefore passes `-SKIPCOMPILE`, the only switch that clears that setting; `allow_build: true` (visible only) leaves the choice to the engine. `offscreen` and `headless` never compile at startup (the engine skips the check under `-unattended`), so `allow_build: true` there is `INVALID_ARGUMENTS` (`param: "allow_build"`), refused before anything starts or stops. The result's `startupCompile` is `skipped` or `allowed`. `-SKIPCOMPILE` does not cover missing or wrong-BuildId module binaries: a `visible` editor still raises the engine's native rebuild prompt for those. Build with `editor_build`.
- **`map`** is placed as the first token after the `.uproject`, the only position the engine reads it from; booting into a map is more reliable than `level.load` on a live world. Dirty or modal-blocked editors are reported by `editor_restart` rather than bypassed.
- **Guard.** `editor_start` and `editor_run_tests` ping PinWright MCP before resolving or launching anything: an editor of this project that answers, even while starting, fails them with `EDITOR_ALREADY_RUNNING`, and an unavailable probe never licenses a second editor. A forwarded `call` while no editor is reachable reports `EDITOR_NOT_RUNNING`. The refusal's `owner` names the editor holding the port as an `editor_list` entry cut to `pid`, `launchedBy`, `reason`, `mode`, `startTime`, `logPath`, `commandLine` (the pid its `system.identity` reports when it is ready, else the only editor whose checkout publishes that port; `null` when they cannot be told apart), so a caller can tell its own stale editor from a peer's session or a test run that will exit. `slot_wait` (seconds, 0-3600, default 0; anything else is `INVALID_ARGUMENTS`) waits for that editor to stop answering before the guard decides, instead of polling. A checkout still serves one MCP editor at a time.
- **Machine-wide launch refusals.** After the guard, and before anything is spawned, `editor_start` / `editor_restart` (kind `editor`), `editor_run_tests` (`suite`) and `editor_build` (`build`) check the whole machine. `EDITOR_LIMIT_REACHED` (`maxEditors`, `running`): `$PINWRIGHT_MAX_EDITORS` PinWright-launched editors (`launchedBy` not `unknown`) already run on the machine, any checkout; unset or `0` is no cap, and builds are not counted against it. `LAUNCH_MEMORY_LOW` (`kind`, `availableBytes`, `availableCommitBytes`, `expectedPeakBytes`, `neededBytes`, `reserveGiB`, `running`): available physical memory (Linux `MemAvailable`), or on Windows available commit, is below the launch's expected peak plus `$PINWRIGHT_LAUNCH_RESERVE_GB` (default 3, the floor at which a host OOM watchdog was measured killing every editor; a negative value disables the check). Expected peaks: suite 16 GiB, editor 6 GiB, build 8 GiB, each capped at 60% of physical RAM, the most a capped launch can reach. Both texts list the running editors. It is a point-in-time check: two launches in the same minute both see memory the first has not allocated yet. Set the variables in the MCP server's `env`.
- **Launches are serialized through CEF initialization.** CEF picks its cache dir (`<user settings>/UnrealEngine/<Project>/webcache_<n>`, shared by every checkout of a project name) by probing a lockfile, and the engine has no switch to give an editor its own. Two editors inside `CefInitialize` at the same moment can pick the same dir; on Windows the loser logs `Detected concurrent CEF initialization for cache dir ...! Retrying...`, reloads the CEF DLLs, and can die there (`WebBrowserSingleton.cpp`). So `editor_start`, `editor_restart` and `editor_run_tests` take a machine-wide, per-user OS lock (`<tempdir>/pinwright-supervisor/editor-launch[-<uid>].lock`) before spawning and hold it until the new editor's log shows it past CEF initialization (a line of another category after the last `LogCEFBrowser: CEF GPU acceleration` attempt, or `Engine is initialized` without one), else until the launch's own wait ends (readiness, first test, exit, timeout). The log is known for every `editor_run_tests` run and for an `editor_start` given `-Abslog=`; without one, the lock is held until the editor is ready. `headless` launches and launches carrying `-nocef` never start CEF and skip the lock. A launch that waits 600 s behind another fails with `EDITOR_LAUNCH_QUEUE_TIMEOUT` (`lockPath`) and starts nothing. Editors started outside PinWright do not take the lock and can still race.
- **`display` gives a visible Linux editor its own X server** (opt-in; `desktop`, the session's display, is the default). Several editors on one desktop share one pointer, stacking order and keyboard focus, so drive `os_input` waits on the display lock or refuses with `TARGET_OCCLUDED` while a peer's window is on top. With `display: "xvfb"` (invisible; needs no desktop session, so it also works over SSH) or `"xephyr"` (a window on the desktop you can watch) the proxy starts that X server with `-displayfd` (it picks a free `:N`) and `-terminate 10` (it exits 10 s after its last client disconnects, so it goes with the editor even after the proxy is gone, while a connection the editor opens and closes during startup does not end it; the delay argument needs an X.Org 21.1+ server), and starts the editor with `DISPLAY=:N`, `WAYLAND_DISPLAY` removed and `SDL_VIDEODRIVER=x11`. The result carries `display` (`":N"`, what a raw `xdotool` needs as `DISPLAY`) and `displayServer`, and `editor_list` shows every Linux editor's `display`. Without an `-Abslog` in `extra_args` the editor gets its own `Saved/Logs/<Project>-<server>-<epoch>.log`, because the device check reads it: once the editor is ready, the proxy reads the Vulkan device from its log (`rhiDevice`, `rhiDeviceType`), and when it is a CPU device (`VK_PHYSICAL_DEVICE_TYPE_CPU`, llvmpipe, lavapipe, SwiftShader) it stops the editor and the server and refuses with `PRIVATE_DISPLAY_SOFTWARE_RHI` naming the device and the display. Refusals before anything starts: `INVALID_DISPLAY` (any other value, a mode other than `visible`, or a non-Linux host; Windows has one input desktop per session), `PRIVATE_DISPLAY_UNAVAILABLE` (the binary is not on `PATH`; names the `xvfb` / `xserver-xephyr` package), `PRIVATE_DISPLAY_FAILED` (it did not report a display within 10 s). `editor_restart` and `editor_run_tests` do not take `display`. A modal the engine raises on that display (for example a Vulkan-driver message box when the RHI fails there) is invisible under Xvfb and shows as `EDITOR_START_TIMEOUT`; the result's `display` lets you screenshot it (`DISPLAY=:N`). Rendering on a non-vendor X server is driver-dependent (no NVIDIA GLX or DRI3 there), which is why this is opt-in. Confirmed combinations, filled only from live runs:

| GPU | driver | X server | Vulkan device the RHI picked | editor reached ready | date |
| --- | --- | --- | --- | --- | --- |
| (none confirmed yet) | | | | | |

Measured on the development host (X.Org Xephyr, 2026-10-01, no editor started): `-displayfd` picked a free `:1`, XTEST through `xdotool` worked on it, and the server exited 10.2 s after the last client left and survived a reconnect inside that window. Xvfb was not installed there, and whether an NVIDIA (580.x) Vulkan device presents on either server is unmeasured.

- **A launched editor that dies in the CEF retry is named.** When an editor exits before readiness (`editor_start`, `-Abslog` given) or before its first test (`editor_run_tests`) and its log's last CEF line is the concurrent-initialization warning, the error is `EDITOR_STARTUP_CEF_RACE` (`exitCode`, `cefLine`) instead of `EDITOR_EXITED_BEFORE_READY` / `EDITOR_EXITED_BEFORE_TESTS`: relaunch it.

**Launch identity.** Every PinWright editor launch (`editor_start`, `editor_restart`, `editor_run_tests`) appends `-PinWrightLaunchReason=<reason>` (`%` encoded as `%25`, `"` as `%22`) and `-PinWrightLaunchedBy=<editor_start|editor_restart|editor_run_tests>` to the editor's command line. Nothing is written to files or the registry; `editor_list` and `system.identity` (`launch_reason`, `launched_by`) read both back from the live process.

**`editor_list` entries:** `pid`, `exe`, `engineRoot`, `commandLine`, `startTime` (UTC ISO) and `startMs`, `project`, `projectName`, `checkoutRoot`, `projectSource` (`commandLine` | `unresolved`), `mode` (`visible` | `offscreen` | `headless` | `commandlet` | `game`, inferred from the switches: `headless` when `-NullRHI` is present; `commandlet` (`-run=`) and `game` (`-game`) win over rendering flags), `map`, `logPath` (its `-Abslog`), `display` (Linux: the `DISPLAY` it started with; `null` elsewhere or for another user's process), `isThisProject`, `gatewayPort` (that checkout's `Saved/PinWright/gateway-port`), `reason`, `launchedBy`. Beside `editors`, `activeBuild` is this project's build lease while its `editor_build` runs (see **Building**), else `null`. It covers every checkout and engine, commandlets (`UnrealEditor-Cmd`), `-game` runs and headless workers. An editor started by anything else, including other repos' tooling, reports `reason: null`, `launchedBy: "unknown"`; that is expected. It never signals a process.

The dotted `system.run_tests` RPC remains a separate live-editor operation reached through `call`; it cannot start a stopped editor.

`editor_build` builds the editor target with every editor of this checkout closed (see **Building** below); `system.live_coding_compile` hot-patches a running editor.

## Test runs: editor_run_tests, then editor_test_status

`editor_run_tests({"filter": "Project.Tests", "reason": "<why>", "mode": "offscreen"})` runs the live-editor guard, launches the suite under the capped, detached supervisor, and blocks **only** until the first `Test Started` line lands in its log. It returns `status: "TESTS_STARTED"`, `runId`, `pid`, `logPath`, `reportDir`, `startedTests`, `lastTest`, `project`, `engineRoot`, `mode`, `reason`, `capped`. The run continues detached and outlives the proxy. `filter` must be non-empty and free of control characters and of the `ExecCmds` separators `,` / `;` (`INVALID_FILTER`).

Launch argv: `<uproject> "-ExecCmds=Automation RunTests <filter>,Quit" "-TestExit=Automation Test Queue Empty" -unattended -nopause -nosplash -nosound [-NullRHI] [-RenderOffscreen] -nocefaccelpaint -RunningUnattendedScript -ddc=InstalledNoZenLocalFallback -PinWrightTransport -ReportExportPath=<reportDir> -Abslog=<logPath>`: `-RenderOffscreen` in `offscreen` and `headless`, `-NullRHI` in `headless` only. `visible` and `offscreen` use a real RHI. **The caller picks the mode. Renderer-dependent tests need `offscreen` or `visible`. A full-suite verdict is taken in `offscreen` until renderer-dependent tests skip cleanly under NullRHI.** Binary: `offscreen` and `headless` use `UnrealEditor-Cmd` on Windows and plain `UnrealEditor` on Linux; `visible` uses the GUI binary. Log: `<Project>/Saved/PinWright/test-runs/<runId>/automation.log`.

Startup errors never stop the run; `pid` and `leftRunning` say what is still alive:

| code | means |
| --- | --- |
| `EDITOR_EXITED_BEFORE_TESTS` | the editor exited before its first test (`exitCode`) |
| `EDITOR_STARTUP_CEF_RACE` | the editor exited before its first test inside CEF's concurrent-initialization retry (`exitCode`, `cefLine`); relaunch |
| `EDITOR_LAUNCH_QUEUE_TIMEOUT` | another PinWright launch held the editor-launch lock for 600 s; nothing was started |
| `EDITOR_BLOCKED_ON_MODAL` | a modal owns the game thread before the first test |
| `EDITOR_TESTS_NOT_STARTED` | no test within 600 s: the slowest healthy boot measured 395.1 s, plus half again |
| `EDITOR_RUN_INTERRUPTED` | the MCP client disconnected, or the wait was interrupted |

`editor_test_status({"logPath": "..."})` (or `{"runId": "..."}`, this project's runs only) never blocks and keeps no state: `status` is `running` while an Unreal editor whose `-Abslog` equals that log is alive, else `finished`. It reports `pid`, `mode` (read from the live editor's command line, or from the log's `Command Line:` line once finished, so every verdict says which mode produced it), `started` / `succeeded` / `failed`, `lastTest`, `supervisorResult` (the `PINWRIGHT_SUITE_RESULT` line from `<log>.result.txt`, once written), `killedExternally` (with `logStoppedAt`, the UTC time the log was last written, when true; see **Killed from outside**) and, once finished, `verdict: {state, reason, warnings}` from `check_suite_log.check_log`. Neither a log nor a live writer: `TEST_RUN_NOT_FOUND`.

**Killed from outside.** The supervisor's verdict is `EDITOR_KILLED_EXTERNALLY` (builds: `COMMAND_KILLED_EXTERNALLY`) when the run ended without a timeout or cap hit and: on Linux, by SIGKILL (UE shuts down gracefully on SIGTERM, SIGINT and SIGHUP and logs a crash before re-raising its signal, so SIGKILL is the one exit it cannot log, as from an OOM watchdog, `kill -9` or the kernel); on Windows, with a non-zero exit code and no `Fatal error` / `Assertion failed:` / `Caught signal` line and no `RequestExit(` line in its log. Such a run leaves no crash report and a log that just stops; `check_suite_log` still classifies it `DID_NOT_COMPLETE`. On Linux the supervisor also reads a drained suite's exit code 1 as a clean exit when the log's last exit request is the forced `-TestExit` exit (`FEngineLoop::Tick.GScopedTestExit`; Unix `RequestExit(true)` is `_exit(1)`), and writes `capSeenByEditor=n/a`: the Unix platform layer never reads cgroup limits, so the cap's evidence is the scope's `memory.max` read back at launch and named in `<log>.supervisor.log`.

`check_suite_log.py` is the fail-closed verdict authority; `editor_test_status` calls it, and it runs standalone under Unreal's bundled Python interpreter on any suite log. It requires the counted `Automation Test Queue Empty <N> tests performed.` drain marker and distinguishes incomplete, empty, failed, skipped, and clean measurements. `PINWRIGHT_ASSERTIONS_SKIPPED` remains a distinct `COMPLETED_WITH_SKIPS` outcome, not a clean result.

The checker states are:

| state | meaning | code |
| --- | --- | --- |
| `CRASHED` | the editor died: a fatal/assert banner, or a non-ensure crash report in the run window (log open to last write) from the run's own editor (its `-Abslog` and pid) | `EDITOR_TESTS_CRASHED` |
| `DID_NOT_COMPLETE` | the queue never drained and no crash evidence exists, so the run was killed; a run that started no test and whose last CEF line is the concurrent-initialization retry says so in its `reason` (a startup death, relaunch; the supervisor's verdict is `EDITOR_STARTUP_CEF_RACE`) | `EDITOR_TESTS_INCOMPLETE` |
| `NO_TESTS` | nothing was enqueued or recorded a success | `EDITOR_NO_TESTS` |
| `COMPLETED_WITH_FAILURES` | the run drained with failures | `EDITOR_TESTS_FAILED` |
| `COMPLETED_WITH_SKIPS` | the run drained but a skip marker was emitted | `EDITOR_TESTS_SKIPPED` |
| `COMPLETED_CLEAN` | the run drained and every assertion ran | - |

## Building: editor_build, then editor_build_status

`editor_build({"reason": "<why>"})` builds `<Project>Editor` Development for the host platform (Win64: `Engine/Build/BatchFiles/Build.bat`; Linux: `Engine/Build/BatchFiles/Linux/Build.sh`) with `-Project=<uproject> -WaitMutex -NoHotReloadFromIDE -Log=<build dir>/ubt.log` (UBT's own log, so a concurrent build on the same engine cannot collide on the shared `Engine/Programs/UnrealBuildTool/Log.txt`), on the engine the project's `EngineAssociation` resolves to, through the capped supervisor at BelowNormal, detached. `reason` follows `editor_start`'s rules; there is no `mode` (it launches no editor). It returns at once: `status: "BUILD_STARTED"`, `logPath`, `pid`, `buildId`, `target`, `platform`, `configuration`, `project`, `engineRoot`, `reason`, `commandLine`, `capped`. Log: `<Project>/Saved/PinWright/builds/<buildId>/build.log`. The reason stays off the UBT command line, because a per-build string would become part of the target's inputs; it is the log's first line (`PinWright editor_build: reason=...; project=<uproject>; startedAt=<epoch seconds>`) and in the result, with `startedAt` (UTC ISO).

**Build lease.** While an `editor_build` runs, `<Project>/Saved/PinWright/builds/lease.json` names it (`buildId`, `logPath`, `reason`, `project`, `startedAt`, `pid`, `ownerPid`: the MCP proxy that started it). The lease is live exactly as long as that build's `editor_build_status` would say `running`; there is nothing to release. `editor_list` shows it as `activeBuild`, so other sessions can hold header edits until it ends, and a second `editor_build` of the same project is refused with `BUILD_ALREADY_RUNNING` (`build`: the lease) instead of racing it.

| code | means |
| --- | --- |
| `MISSING_REQUIRED_PARAM`, `INVALID_REASON` | `reason` missing or invalid |
| `INVALID_ARGUMENTS` | an unknown field, including `mode` |
| `BUILD_BLOCKED_BY_EDITOR` | an Unreal editor of this checkout is running (`pids` named): the link replaces its DLLs. Editors of other checkouts do not block |
| `BUILD_ALREADY_RUNNING` | another `editor_build` of this project is running (`build` names it) or is being started at this moment |
| `EDITOR_LIST_FAILED` | the process census failed; refused rather than risk the link |
| `BUILD_SCRIPT_NOT_FOUND`, `BUILD_START_FAILED` | the engine's build script is missing, or the supervisor could not start |
| `EDITOR_PROJECT_NOT_FOUND`, `EDITOR_ENGINE_NOT_FOUND` and the other engine-resolution errors | as for `editor_start` |

`editor_build_status({"logPath": "..."})` never blocks and keeps no state: `status` (`running` \| `succeeded` \| `stale` \| `failed` \| `lost`, the supervisor died without a result), `ubtResult` (UBT's `Result: Succeeded|Failed (...)` line), `exitCode`, `verdict` (supervisor verdict, e.g. `MEMORY_CAP_HIT`, `TIMEOUT`), `supervisorResult` (the `PINWRIGHT_JOB_RESULT` line), `killedExternally` (and `logStoppedAt` when true; see **Killed from outside** above), `errorCount` and `errors` (compiler / linker / UBT error lines from the whole log: MSVC `error Cxxxx` / `LNKxxxx` / `MSBxxxx`, clang `: error:` / `fatal error:`, UBT `ERROR`, `Unable to build while Live Coding is active`, `cannot open file`; the first 200 distinct), `pid`, `sourcesChangedDuringBuild`. Refusals: `MISSING_REQUIRED_PARAM`, `INVALID_LOG_PATH`, `INVALID_ARGUMENTS`, `BUILD_NOT_FOUND`.

**`stale`: a source changed while the build ran.** Once a build has finished, `sourcesChangedDuringBuild` lists every `.h` / `.hpp` / `.inl` / `.c` / `.cc` / `.cpp` / `.cs` under the project's `Source/` and each project plugin's `Source/` whose modification time falls between `startedAt` and the build's end (its result line); `null` while running. When UBT reported `Succeeded` and the list is not empty, `status` is `stale`, never `succeeded`: translation units compiled before the edit saw the old header and the ones after saw the new, and UBT then treats the module as up to date because every object is newer than the header. An edit after the build ended is not listed, because the file is then newer than its objects and the next build recompiles it. The window opens when the build is requested, so an edit made while UBT still waits on `-WaitMutex` is listed too (conservative). **Recovery:** touch every listed file and run `editor_build` again. The same recovery applies to the crash this produces when missed: an editor that dies at startup with `EXCEPTION_ACCESS_VIOLATION` in `FObjectInstancingGraph::InstancePropertyValue` while loading a Blueprint CDO, right after a build during which a header was edited, has a module compiled against two class layouts; touch the recently edited headers and rebuild.

A one-file compile check has no tool parameter: run plain `Build.bat` / `Build.sh ... -SingleFile=<file>` from a shell. That is fine outside the supervisor because it compiles one translation unit and writes no binaries.

## Platform differences

| | Windows | Linux |
| --- | --- | --- |
| spawn | direct, detached (`DETACHED_PROCESS`, new process group) | direct, new session |
| supervisor start (tests, builds, `offscreen` / `headless` editors capped; `visible` editors uncapped on Windows) | WMI `Win32_Process.Create`: parent `WmiPrvSE`, so outside the MCP client's process tree and job and a client tree kill cannot reach the run. WMI failure: direct child, result `detached: false` with `detachNote`, text says `NOT DETACHED` | direct child, new session |
| proxy / supervisor skew | the proxy keeps the supervisor module in memory and launches the file on disk; a protocol skew fails the launch with `SUPERVISOR_VERSION_MISMATCH`: restart the MCP server (`/mcp` reconnect) | same |
| result fields | `detached`, `launchMechanism` (`wmi-win32-process-create` \| `createprocess-breakaway` \| `createprocess-in-caller-job`), `detachNote` | same fields, `launchMechanism: setsid` |
| display | `offscreen` / `headless`: none (`CREATE_NO_WINDOW`, null platform application) | `offscreen` / `headless`: none (SDL `dummy` driver); `visible`: an X display |
| memory cap (`offscreen` / `headless`, tests, builds) | Job Object `JOB_OBJECT_LIMIT_PROCESS_MEMORY`, per process (shader workers get their own ceiling); a hit fails allocations | `systemd-run --user --scope -p MemoryMax`, per scope = whole process tree; a hit invokes the cgroup OOM killer. Unavailable: uncapped, with the reason in `uncappedReason`. Verified on a real host (Ubuntu 22.04, systemd 249, kernel 6.8): the scope holds the spawned pid, `memory.max` / `memory.peak` / `memory.events` read back, nice 10 and the process group reach the child, `killpg` reaches grandchildren; an OOM kill whose emptied scope systemd removed before the final read is recovered from the parent slice's count |
| priority (BelowNormal) | job priority class `0x4000`, job-wide | nice 10, inherited |
| kill-on-exit, timeout | `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, `TerminateJobObject` | `PR_SET_PDEATHSIG(SIGKILL)` + own process group, `killpg` |
| `offscreen` / `headless` suite binary | `UnrealEditor-Cmd.exe` | `UnrealEditor` |
| `editor_list` enumeration | one `Get-CimInstance Win32_Process` query (no working directory) | `/proc/<pid>/cmdline`, `exe`, `cwd`; start time from `stat` + `btime` |

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
