# Game processes and several instances

PinWright also serves `-game` processes and packaged Development builds of a project, and several processes of one project at once, each on its own loopback port. Use it for parallel playtests and for driving a standalone game; editor-only work stays in the editor.

## Starting a game or a second instance

A game process serves only when asked; an editor serves as before.

| Launch | Serves on | Writes `gateway-port` |
|---|---|---|
| editor, no switch | the project's settings port (derived or fixed `HttpPort`) | yes: it is the primary editor the MCP proxy talks to |
| editor `-PinWrightPort=<N>` / `-PinWrightPort=0` | port N / an ephemeral port | no |
| `-game` or packaged Development, no switch | nothing (the module loads and stays idle) | no |
| `-game` or packaged Development `-PinWright` | an ephemeral port | no |
| `-game` or packaged Development `-PinWrightPort=<N>` / `=0` | port N / an ephemeral port | no |

Start instances with the CLI (`call("cli")`): `pinwright_cli.py start --name play1 --game --mode offscreen --reason "<why>"` launches `-game -PinWrightPort=0` and waits until it answers. By hand: `UnrealEditor <Project>.uproject -game -PinWright`, or `<Game>.exe -PinWright` for a packaged Development build. Shipping builds contain no PinWright code.

## Finding and authorizing an instance

Every serving process writes `<Project>/Saved/PinWright/instances/<pid>.json` after it binds and deletes it on a clean shutdown (a packaged build writes under its own `Saved/`):

```json
{"pid": 1234, "port": 51234, "token": "<64 hex>", "kind": "editor|game",
 "project": "<abs .uproject>", "startTime": "<ISO 8601 UTC>", "reason": "<-PinWrightLaunchReason or empty>",
 "map": "<current map>", "engine": "5.8"}
```

- `token` authorizes requests to that process only (`Authorization: Bearer <token>`); every process has its own. The primary editor also accepts `Saved/PinWright/gateway-token`, which the MCP proxy uses.
- A crash leaves its file behind; treat a file as stale when its pid is gone or was started after the file was written. The CLI does this for you.
- The MCP `call` tool reaches the primary editor only (through `gateway-port`). Reach games and second instances with `pinwright_cli.py`.

## Which methods answer in a game

- A method page reads `Runtime: true` when the method is registered in game processes, `Runtime: false` when it exists only in the editor; `registry.json` carries the same `runtime` flag per method. In a game an editor-only method returns `UNKNOWN_ACTION`.
- A game process generates no wiki. Read the editor's generated wiki (`Saved/PinWright/wiki/`, written at the last editor launch of the project).
- Runtime methods act on the game viewport's world. In the editor they keep their editor targets (PIE while PIE runs where the method targets PIE today). A runtime method that needs the editor world returns `EDITOR_ONLY` or `NO_EDITOR_WORLD` in a game.

## Security

- Loopback only (`127.0.0.1`); no LAN, VPN or container exposure.
- Per-process token in a user-only instance file (POSIX mode `0600`; Windows: no access for shared groups).
- Opt-in for games: a Development build handed to testers serves nothing unless launched with `-PinWright` or `-PinWrightPort`. Shipping builds compile it out.

## Manual check: a game process targets its own world

The automated suite covers the editor half of the world accessor (`PinWright.infra.target_world.*`). Check the game half after changing `Dispatch/PinWrightWorld.h` or a moved world verb:

1. `pinwright_cli.py start --name wcheck --game --mode offscreen --reason "world accessor check" --map <map>`.
2. `pinwright_cli.py list`: the `wcheck` entry has `kind: "game"` and `map` = that map.
3. `pinwright_cli.py call --name wcheck actor.list "{}"`: the actors are the game world's (the map's level actors plus the spawned player pawn and controller).
4. `pinwright_cli.py stop --name wcheck`, then confirm `Saved/PinWright/instances/<pid>.json` is gone.
