# PinWright

PinWright is an Unreal Editor plugin that lets an AI coding assistant (Claude Code, Cursor, Codex, and others) work directly in the editor. It runs a local MCP (Model Context Protocol) server: you describe what you want in plain language and the assistant does it, while you review the results.

Website: https://pinwright.com

**What it is best at today:** inspecting and dumping your project, authoring Blueprints, building UMG widgets, creating custom assets (Data Tables and other data assets), and recording Play-In-Editor sessions for debugging. Material, Niagara, animation, rigging, and level-layout authoring are included but still experimental.

## Features

| Area | What it does | Maturity |
| --- | --- | --- |
| Project inspection & dumps | Browse and inspect assets; dump any asset or folder to readable text for review and accurate agent context | Core |
| Blueprints | Create and edit Blueprint classes, variables, functions, events, and graphs; compile and decompile | Core |
| Widgets (UMG) | Author the widget tree, styles, bindings, and animations; export and import as XML | Core |
| Custom assets | Create and edit Data Tables and other data assets | Core |
| Actors & levels | Spawn actors, edit transforms, manage levels, inspect world state | Core |
| Session recorder | Record a Play-In-Editor session and query it to debug what happened (requires host instrumentation to capture gameplay data) | Core |
| System operations | Read logs, run editor commands, trigger build and test helpers | Core |
| Graph text IRs | Round-trip (compile and decompile) for Blueprint (BPIR), Material (MGIR), Animation Blueprint (AGIR), and Control Rig (CRIR); decompile-to-text for Behavior Tree (BTIR), Sound Cue (SCIR), MetaSound (MSIR), Niagara (NIR), and PCG (PCGIR) | Mixed |
| Materials, Niagara, animation, rigging, levels | Graph and asset authoring for these areas | Experimental |

## Requirements

- Unreal Engine 5.3-5.8 editor build with C++ plugin support, on Windows or Linux. Per-version notes and known engine-specific caveats are in [docs/engine-version-support.md](docs/engine-version-support.md).

The plugin module is editor-only and is not intended for packaged game runtime use.

## Installation

**From Fab / the Epic Launcher.** Install the plugin to the target Unreal Engine version and enable it from the Unreal Editor's plugin browser.

**From source.** Clone it into your project's `Plugins` directory, then build your project's editor target:

```
git clone https://github.com/PinWright/pinwright-ue.git <Project>/Plugins/PinWright
```

Then, for either source path:

1. If you did not clone straight into it, copy `PinWright` into the engine `Plugins` directory or your project's `Plugins` directory.
2. Regenerate project files if your project uses a source checkout or local C++ project files.
3. Build your project's editor target if Unreal asks for a rebuild.
4. Open the Unreal Editor.
5. Enable the plugin and confirm its settings under **Editor Preferences -> Plugins -> PinWright**.

## Connecting Your AI Agent

**One-click setup (recommended).** On startup the editor opens a **PinWright Setup** screen (reopen it any time from **Tools -> PinWright Setup**). It shows the resolved local endpoint and a green "Server listening" status, with a one-click **Install** button for each agent below (Claude Code, Codex CLI, Cursor, Gemini CLI, VS Code Copilot). Clicking Install writes that agent's project-local config with the endpoint already baked in, so you never edit a file by hand or discover the port at runtime. For an agent without a button (Windsurf, Cline, or any other), copy the **Agent setup prompt** from the screen and paste it to your agent.

<details>
<summary><strong>Manual setup (fallback)</strong>: per-agent config files</summary>

The snippets below are the manual equivalent of the one-click buttons. Substitute the project's resolved port for `19880`. "Project root" means the UE project the plugin is installed into.

### Claude Code

Create or merge `<project>/.mcp.json`:

```json
{
  "mcpServers": {
    "pinwright": { "type": "http", "url": "http://127.0.0.1:19880/mcp" }
  }
}
```

Or via CLI: `claude mcp add --transport http --scope project pinwright http://127.0.0.1:19880/mcp`. Claude Code shows a one-time approval prompt for project-scoped servers.

### Codex CLI

Create or merge `<project>/.codex/config.toml` (loaded only after you trust the project in Codex; the first `codex` run in the folder prompts):

```toml
[mcp_servers.pinwright]
url = "http://127.0.0.1:19880/mcp"
startup_timeout_sec = 5.0
tool_timeout_sec = 300.0
```

Alternatively, `codex mcp add pinwright --url http://127.0.0.1:19880/mcp` registers it user-globally.

### Cursor

Settings → Tools & MCP → Add, or merge into `<project>/.cursor/mcp.json`:

```json
{
  "mcpServers": {
    "pinwright": { "url": "http://127.0.0.1:19880/mcp" }
  }
}
```

No `type` key; Cursor auto-detects the transport. Recent Cursor versions add project-file servers disabled — enable the server in Settings → Tools & MCP.

### Gemini CLI

Create or merge `<project>/.gemini/settings.json` (note the key is `httpUrl`, not `url` — `url` means SSE in Gemini):

```json
{
  "mcpServers": {
    "pinwright": { "httpUrl": "http://127.0.0.1:19880/mcp" }
  }
}
```

Project config loads only for trusted folders (`gemini trust` / first-launch prompt).

### VS Code (GitHub Copilot)

Create or merge `<project>/.vscode/mcp.json` (top-level key is `servers`, not `mcpServers`):

```json
{
  "servers": {
    "pinwright": { "type": "http", "url": "http://127.0.0.1:19880/mcp" }
  }
}
```

VS Code prompts once to trust and start the server. Or, user-level: `code --add-mcp "{\"name\":\"pinwright\",\"type\":\"http\",\"url\":\"http://127.0.0.1:19880/mcp\"}"`.

### Other Clients

- **Windsurf** — `~/.codeium/windsurf/mcp_config.json`, `"mcpServers"` entry `{ "serverUrl": "http://127.0.0.1:19880/mcp" }` (user-level only).
- **Cline** — `cline_mcp_settings.json` (VS Code globalStorage), entry `{ "type": "streamableHttp", "url": "http://127.0.0.1:19880/mcp" }`.
- **Stdio-only MCP clients** — bridge with the bundled proxy: run `Plugins/PinWright/Content/Python/mcp_proxy.py` with any Python 3, passing `--token-file` and `--port-file` pointing at the `gateway-token` / `gateway-port` files in `Saved/PinWright` (and optionally `--uproject` to select the project explicitly). It forwards stdio to the HTTP endpoint, sends the bearer token, follows the live port, and supplies the three local lifecycle tools (this is exactly what onboarding-generated configs do, using the engine's bundled Python).

</details>

### Stdio Proxy Lifecycle Tools

The direct in-editor endpoint still has exactly one tool, `call`. A client configured through the bundled stdio proxy sees four tools: `call`, `editor_start`, `editor_restart`, and `editor_prepare_tests`.

- `editor_start({})` opens the project's `.uproject` through the registered file association (on Linux, which has none, it spawns the resolved editor directly, borrowing `DISPLAY`/`XAUTHORITY` from your desktop session when the proxy runs over SSH). Unreal owns prompts, compilation, and module loading for this path, and the engine comes from the project's `EngineAssociation` (on Linux, `UE_<version>=<engine root>` or a GUID key under `[Installations]` in `~/.config/Epic/UnrealEngine/Install.ini`), or from `$PINWRIGHT_ENGINE_ROOT` when set. An unresolved association fails with `EDITOR_ENGINE_NOT_FOUND`.
- `editor_start({"map": "/Game/Maps/MyLevel"})` boots into a level. The map token is the first argument after the `.uproject`, so a map forces direct spawn and defaults `unattended_script` on.
- `editor_restart({"map": "/Game/Maps/MyLevel"})` quits through `editor.quit`, waits for the endpoint to go down, then starts a fresh editor. Dirty or modal-blocked editors are reported rather than bypassed.
- For tests, `editor_prepare_tests({"filter": "Project.Tests"})` is the only command-returning proxy verb. The `filter` is mandatory, must be non-empty, and has no default. The live-editor guard runs first and is reported as structured observation: a detected editor is `EDITOR_ALREADY_RUNNING`, an unavailable probe is `not_probed`, and only a safe probe proceeds to `COMMAND_READY`.

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
| `DID_NOT_COMPLETE` | the queue never drained and no crash evidence exists — the run was killed | `EDITOR_TESTS_INCOMPLETE` |
| `NO_TESTS` | nothing was enqueued or recorded a success | `EDITOR_NO_TESTS` |
| `COMPLETED_WITH_FAILURES` | the run drained with failures | `EDITOR_TESTS_FAILED` |
| `COMPLETED_WITH_SKIPS` | the run drained but a skip marker was emitted | `EDITOR_TESTS_SKIPPED` |
| `COMPLETED_CLEAN` | the run drained and every assertion ran | - |

All proxy-local lifecycle tools ping PinWright MCP before resolving or launching anything. If an editor answers, even while starting, lifecycle tools fail with `EDITOR_ALREADY_RUNNING`. A forwarded `call` while no editor is reachable reports `EDITOR_NOT_RUNNING`; an unavailable probe never licenses a second editor start.

The dotted `system.run_ubt` and `system.run_tests` RPCs remain separate live-editor operations reached through `call`. They cannot start a stopped editor; use `editor_start` for a live editor or `editor_prepare_tests` to prepare a cold test command for the caller.

The endpoint URL is resolved at onboarding and baked into each agent's project-local config. Once connected, agents send in-editor dotted RPCs through their MCP client's `call` tool, while stdio-proxy clients invoke the local lifecycle tools directly.
**Two projects, same port:** by default **Auto-derive Port From Project Path** is on, so each project folder gets a distinct port in `19880`–`30119` and checkouts at different paths don't collide. If you disable it, both projects bind the same fixed `HttpPort` (default `19880`) and two editors running at once collide — the editor that loses the bind refuses to serve. On the rare cross-folder hash collision (or an OS-reserved port) even with auto-derive on, set a fixed `HttpPort` of your choice, re-run onboarding, then restart.

### Defaults

- HTTP transport: enabled.
- Port: per-project derived by default via **Auto-derive Port From Project Path** — `19880 + hash(projectPath) % 10240`, a stable port in `19880`–`30119` (10240 slots). Disable it to bind a fixed `HttpPort` (default `19880`, same on every machine, keeping committed agent configs portable).
- Port publication: the bound port is written to `Saved/PinWright/gateway-port` on each successful start (left in place on shutdown as last-known-good); the bundled stdio proxy re-reads it before every forwarded call, so proxy-based agent configs auto-follow a moved project or a changed port with no config edit or restart. Direct-HTTP configs are unaffected.
- Bind policy: loopback-only.
- Auth: bearer token required by default; auto-generated at `Saved/PinWright/gateway-token`.
- Request body limit: 1 MB.
- Default request timeout: 120 seconds.
- Maximum request timeout: 300 seconds.

If a client cannot connect, confirm that the Unreal Editor is open, the plugin is enabled, and no other local process is using the configured port.

## Security

This plugin exposes powerful editor automation. Even though the module is editor-only, requests can inspect and mutate assets and editor state. Treat it as a local developer tool with asset-mutation privileges, and use it only with trusted local tools.

### Risk Model

The MCP endpoint at `POST /mcp` can expose operations that inspect and change editor state. Depending on the enabled RPC surface and caller permissions, automation can create, edit, save, delete, compile, or otherwise mutate project assets. Editor-only does not remove this risk: the plugin runs inside the Unreal Editor process, and the editor has access to the project.

### Required Operating Rules

- Keep the MCP endpoint loopback-only.
- Do not expose the configured port to a LAN, VPN, container bridge, or the internet.
- Use only trusted local MCP clients.
- Do not probe or invoke the gateway with raw HTTP, `curl`, `Invoke-RestMethod`, `/health`, or legacy `/rpc` calls.
- Do not run untrusted prompts or tool sequences against production project files.
- Use source control and review generated changes before committing them.

### Authentication

By default the gateway requires an `Authorization: Bearer <token>` header on every `POST /mcp` request. This is a boundary against browser-based drive-by requests: a malicious web page cannot read a local file, so CSRF and DNS-rebinding attempts that reach the loopback port are rejected, as are other OS users on the machine. Processes running as the same OS user are inside the trust boundary by design — the token is not a defense against local code you already run.

The token is 64 lowercase hex characters (32 random bytes), stored one-per-line at `Saved/PinWright/gateway-token`. It is created automatically on editor start and persists across sessions. Onboarding bakes it into each agent config for you: the stdio-proxy config receives only the token file *path* (`--token-file <absolute path>`, machine-specific like the Python path already is), while direct-HTTP configs (Gemini `httpUrl`, VS Code, Cursor, Claude Code's http fallback) embed the literal token as `headers: { "Authorization": "Bearer <token>" }`.

**Keep the direct-HTTP config files machine-local and never commit them** — they carry the literal secret. The stdio-proxy config carries only the path and is safe to commit on that count.

To rotate the token, delete `Saved/PinWright/gateway-token` and restart the editor (a fresh token is generated), then re-run **Install** in the setup screen so direct-HTTP clients pick up the new value. Stdio-proxy clients read the file per request and pick up the new token automatically.

The **Require Auth Token (Bearer)** setting (`bRequireAuthToken`) in **Edit -> Project Settings -> Plugins -> PinWright** is the kill switch, ON by default. When OFF, Install/Update write agent configs without any token plumbing and the server ignores the `Authorization` header entirely — previously-tokened configs keep working because the header is simply ignored. When ON, a request with a missing or wrong token gets an HTTP 401 with a `WWW-Authenticate: Bearer` header and a JSON-RPC error body that names the token file path and tells you to re-run Install in the setup screen.

### Default Endpoint

The listen port is derived per project by default via **Auto-derive Port From Project Path** in Editor Preferences (`19880 + hash(projectPath) % 10240`, a stable port in `19880`–`30119`); disable it to bind a fixed `HttpPort` (default `19880`). The release package keeps the MCP transport enabled because the plugin exists to provide editor automation, but the endpoint should remain local-only. Review settings under **Editor Preferences -> Plugins -> PinWright** before using the gateway in a shared machine, remote desktop, studio build farm, or CI environment.

## Usage

Configure an MCP-capable client for the gateway, then interact through the tools exposed by that client. Agents and operators should not probe or invoke the gateway with raw HTTP, `curl`, `Invoke-RestMethod`, `/health`, or legacy `/rpc` calls. Those paths bypass the MCP client contract and are not the supported workflow.

The listen port is derived per project by default via **Auto-derive Port From Project Path** (a stable port in `19880`–`30119`); disable it to bind a fixed `HttpPort` (default `19880`); onboarding bakes the resolved endpoint URL into each agent's project-local config. See [docs/wiki-src/mcp-transport.md](docs/wiki-src/mcp-transport.md) for maintainer-level transport details; the full assembled method reference is generated into the project's `Saved/PinWright/wiki/` at every editor launch (relocate it with **Wiki Output Directory** under **Project Settings -> Plugins -> PinWright (Project)**).

## Support

All support — bug reports, critical gaps in existing features, and feature requests — goes through the issue tracker:

- **Bugs / critical gaps in existing features** → [open an Issue](https://github.com/PinWright/pinwright-ue/issues/new?template=bug_report.yml)
- **The plugin does not load or build at all on your engine** → [compatibility report](https://github.com/PinWright/pinwright-ue/issues/new?template=compatibility_report.yml)
- **New or extra feature ideas** → [open an Issue](https://github.com/PinWright/pinwright-ue/issues/new)

Issue tracker: https://github.com/PinWright/pinwright-ue/issues

## Documentation

- [Installation](#installation)
- [Security](#security)
- [Wiki reference](docs/wiki-src/README.md) (the full assembled reference is generated into the project's `Saved/PinWright/wiki/` at every editor launch)

## License

MIT. See [LICENSE](LICENSE). Third-party components and their required notices are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Contributing

Build, test, and documentation conventions are in [CONTRIBUTING.md](CONTRIBUTING.md); release history is in [CHANGELOG.md](CHANGELOG.md). Contributions are accepted under MIT, with no CLA.

## Third-Party Attribution

Portions derived from ChiR24/Unreal_mcp (MIT, (c) 2025 Unreal Engine MCP Server Contributors) - declared in the Fab Third-Party Software disclosure.
