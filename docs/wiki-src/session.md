# session

Local-multiplayer PIE testing: LAN listen-server hosting via ServerTravel, split-screen local-player add/remove on the running game instance, and live session status readback. Use this for editor-driven local multiplayer testing; use `call("networking")` for Blueprint replication, RPCs, actor ownership, prediction, and authority checks.

## Prerequisite: adding/removing local players needs a live PIE game instance

`add_local_player` and `remove_local_player` operate on the **active game instance**
(`GEditor->PlayWorld->GetGameInstance()`), which exists only during Play-In-Editor. With PIE
stopped both hard-fail with `[NO_GAME_INSTANCE] No active game instance. Start Play-In-Editor first.`,
so start PIE with `editor.play` before changing the roster. `remove_local_player` also refuses index
0 (`INVALID_ARGUMENT`): the primary local player cannot be removed.

`get_sessions_info` is safe pre-PIE and returns (`localPlayerCount 0`, `inPlaySession false`,
`splitScreenType "None"`). With more than one local player in PIE it reports
`splitScreenEnabled true` and `splitScreenType "Active"`; it never returns `[NO_GAME_INSTANCE]`.

## Hosting a LAN listen server

`host_lan_server` assembles `?listen?bIsLanMatch=1?MaxPlayers=N` plus `travelOptions` for
`mapName`. `executeTravel:false` (default) returns the configuration fields (`serverName`,
`mapName`, `mapPath`, `maxPlayers`, and `travelURL`), `status:"configured"`, `timedOut:false`, and
false `travelAccepted`, `travelQueued`, `travelCompleted`, and `travelExecuted` fields. With
`executeTravel:true`, it issues `ServerTravel` on the PIE world when running, otherwise the editor
world. A world is required only to execute travel, so a loaded map succeeds pre-PIE; with no world
it fails `HOST_FAILED "No world available"`, never `NO_GAME_INSTANCE`.

For `executeTravel:true`, `travelAccepted` is the engine call's boolean result. `travelQueued` means
there was no pending URL or seamless transition before the call and the requested destination was
pending afterward (or a new seamless transition began). `travelCompleted` means a bounded readback
observed that destination as the current map in a replacement world after both pending forms
cleared. The legacy
`travelExecuted` field is true only with `travelCompleted`; a queued request is not reported as
executed. If the engine rejects the call, or accepts it without newly queuing this destination, the
RPC fails with `TRAVEL_REFUSED` and the three fields
identify which stage was reached. Once queued, the handler retains the request for up to 15 seconds,
captures the initiating world-context handle, and re-resolves only that exact context on every poll.
If that context disappears, the request fails `HOST_FAILED` without accepting an unrelated world's
map as completion. A missing destination readback at the deadline also fails `HOST_FAILED` with
`travelAccepted:true`, `travelQueued:true`, `travelCompleted:false`, and `travelExecuted:false`.

The completion poll reuses `PinWrightPieState::WaitForPieLifecycleState`; the handler must not
register a second ticker. Its injected `PlayWorldActive` probe means exact-context destination
completion here, not merely the presence of a PIE world, so abandonment and the 15-second bound
stay on the shared lifecycle-wait path.

The natural local-multiplayer testing order is `host_lan_server` (or `editor.play`), then `add_local_player` once per extra player, then `remove_local_player`, with `get_sessions_info` to confirm state at any point.

## See also

- [`editor`](editor.md) — PIE lifecycle ownership, play/stop waits, and multi-world status readback.
- [`networking`](networking.md) — replication, RPCs, ownership, prediction, and authority checks.
