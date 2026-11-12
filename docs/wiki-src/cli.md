# cli

Drive PinWright from a shell with `Content/Python/pinwright_cli.py`, without an MCP client, including several editors or `-game` processes of one project at once.

Run it on Unreal's bundled Python (standard library only, like `mcp_proxy.py`): `<Engine>/Engine/Binaries/ThirdParty/Python3/Win64/python.exe` on Windows, `.../Python3/Linux/bin/python3` on Linux. It reuses the stdio proxy's transport, probe, error codes and launch path (`mcp_proxy.Proxy`, `pinwright_supervisor`), so results and refusals are the same as the MCP tools'.

## Commands

| command | does |
| --- | --- |
| `list` | every live PinWright process of the project, each probed (`state`: `alive`, `not_ready`, `not_running`, `unresponsive`, `blocked_on_modal`, `protocol_stale`) |
| `call [--pid P \| --name N \| --port N] <method> [<args>]` | one RPC. `<args>` is a JSON object, `@file` or `-` (stdin); pass `{}` for a method without parameters. Without `<args>` it returns the method's wiki page reference, as `call` does over MCP. No selector: the primary editor |
| `start --name N --mode visible\|offscreen\|headless --reason "<why>"` | launches an editor of this project on its own port and waits for readiness; optional `--game` (adds `-game`), `--port N` (default 0 = ephemeral), `--map`, `--timeout` (seconds, at most 3600), `--extra-arg=-Switch` (repeatable) |
| `stop (--pid P \| --name N \| --port N)` | `editor.quit` (optional `--save`, `--discard`, `--force`, `--reason`), then waits up to 120 s for the endpoint to stop answering; never kills a process |

Global option: `--project <.uproject or folder>` (default: the project the plugin is installed in).

Output is JSON on stdout: the tool result's `structuredContent` (`--raw` prints the whole result), or its text when it has none; an error's text also goes to stderr. Exit codes: `0` success, `1` refused (a JSON-RPC error, a result with `isError`, a refused or timed-out start, a stop that did not complete), `2` bad usage or no matching instance.

Long-running verbs answer with a job ticket (the CLI never streams); poll with `call system.job_status '{"ticket_id": "..."}'`.

## Instances

The **primary editor** is the one `Saved/PinWright/gateway-port` names: the editor the MCP proxy talks to, on the derived or fixed port, authorized with `Saved/PinWright/gateway-token`. It is found even when it writes no instance file.

Every other process serves on the port `-PinWrightPort=<N|0>` gives it and is found through its instance file `Saved/PinWright/instances/<pid>.json` (`pid`, `port`, `token`, `kind` `editor` | `game`, `project`, `startTime`, `reason`, `map`, `engine`; contract in `docs/runtime-module.md`). A process launched with `-SaveToUserDir` writes it under UE's user-dir `Saved/` instead (`<UserSettingsDir>/<ProjectName>/Saved/`: `%LOCALAPPDATA%` on Windows, `~/.config/Epic` on Linux); the CLI reads both, and since every checkout with the same project name shares the user-dir one, a file there counts only when its `project` is this project. A file whose pid is gone, or whose pid belongs to a process started after the file was written, is stale and skipped. The CLI sends a file's `token` only to the loopback port that file names.

`start --name` passes `-PinWrightPort=<port>`, `-PinWrightLaunchReason`, `-PinWrightLaunchedBy=pinwright_cli` and `-Abslog=Saved/Logs/<Project>-<name>.log` (unless `--extra-arg` carries an `-Abslog`), and records the name in `Saved/PinWright/cli-names/<name>.json` as soon as the launched pid is known; `--name` resolves through that pid. A name held by a live pid, or a `--port` a live instance serves, is refused `INSTANCE_IN_USE` before anything starts. All `editor_start` refusals apply unchanged (`MISSING_REQUIRED_PARAM`, `INVALID_MODE`, `INVALID_REASON`, `EDITOR_LIMIT_REACHED`, `LAUNCH_MEMORY_LOW`, `EDITOR_LAUNCH_QUEUE_TIMEOUT`, `EDITOR_ENGINE_NOT_FOUND`, ...), and the result carries `name`, `kind`, `port`, `pid` and `instanceFile`. With an ephemeral port the CLI learns the port only from the new process's instance file, so a server without instance-file support ends in `EDITOR_START_TIMEOUT` with a `hint`; pass `--port` for one.

Requests carry `X-PinWright-Client: $PINWRIGHT_CLIENT_ID`, default `pinwright-cli`. `editor.quit` refuses `EDITOR_IN_USE` while a different client id drove that editor in the last 5 minutes, so one agent keeps one id across invocations, and agents sharing an editor set different ids.

## Several editors of one project

- **Memory.** Each editor is a full process (expect several GiB each, more while shaders compile). The machine-wide `LAUNCH_MEMORY_LOW` and `$PINWRIGHT_MAX_EDITORS` checks of `editor_start` apply to every `start`.
- **Shared `Saved/`.** All instances share the project's DDC, `Saved/PinWright` (job log, wiki output, spills) and config; a setting saved in one is seen by the next launch of every other. Give each instance its own `-Abslog` (the default does) so logs do not interleave.
- **Startup is serialized** through CEF initialization by the machine-wide launch lock, as for `editor_start`; parallel `start`s queue.
- **Builds** of the project's editor target need every editor of the checkout closed (`editor_build` refuses with `BUILD_BLOCKED_BY_EDITOR`), so stop the extra instances first.

## Examples

```sh
PY="<Engine>/Engine/Binaries/ThirdParty/Python3/Win64/python.exe"
CLI="<Project>/Plugins/PinWright/Content/Python/pinwright_cli.py"
"$PY" "$CLI" start --name a --mode offscreen --reason "parallel playtest A" --map /Game/Maps/Test
"$PY" "$CLI" start --name b --mode headless --reason "parallel playtest B" --game
"$PY" "$CLI" list
"$PY" "$CLI" call --name a system.identity '{}'
"$PY" "$CLI" call system.identity '{}'          # the primary editor
"$PY" "$CLI" call --name b actor.list @query.json
"$PY" "$CLI" stop --name a --discard
```

## See also

- [`mcp-transport`](mcp-transport.md) — endpoint, authentication, the proxy's lifecycle tools
- [`game-mode`](game-mode.md) — which launches serve (a hand-started game with bare `-PinWright` serves on an ephemeral port and is found the same way), which methods answer in a game, security
