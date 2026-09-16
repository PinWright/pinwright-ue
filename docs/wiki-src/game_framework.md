# game_framework

Configure Unreal gameplay framework classes and match rules — GameMode setup, default Pawn, GameState, PlayerController, PlayerState, player starts, respawn behavior, scoring, rounds, teams, spawning, and spectating. Use this when the rule or class selection belongs to GameMode/GameState-style framework assets or current level spawn setup; use `call("session")` for active local players, LAN hosting/join settings, split screen, and voice options.

## Reading back a configured GameMode asset

`get_game_framework_info` is NOT the read-back partner for the `set_*`/`configure_*` verbs. They operate on different objects:

- The write verbs (`configure_game_rules`, `configure_team_system`, `configure_scoring_system`, `configure_round_system`, `set_respawn_rules`) each take a **required `gameModeBlueprint` asset path** and persist Blueprint variable defaults onto that **standalone GameMode asset's CDO**. (To set individual framework class properties such as `DefaultPawnClass` / `GameStateClass` / `PlayerControllerClass` / `PlayerStateClass`, use `blueprint.set_default {path, propertyName, value}`.)
- `get_game_framework_info` takes **no params** and reports the **active editor level's** spawn state only: `WorldSettings->DefaultGameMode` (returned as `gameMode`) plus the level's `PlayerStart` actors (`playerStarts[]` / `playerStartCount`). It echoes nothing about the `gameModeBlueprint` asset or the rules / teams / scoring / rounds / respawn / pawn / state / controller values the write verbs just persisted.

So a configure-then-`get_game_framework_info` confirm loop does NOT close in-namespace. When the configured asset is not the level's GameMode override, `get_game_framework_info` reporting `gameMode: "(default)"` — or naming an unrelated GameMode that IS the level override — after a fully successful configure run is **expected, not a failure**.

To read back the values written onto the GameMode asset CDO, inspect the asset directly with `blueprint.inspect {assetPath: "<the gameModeBlueprint>", includeProperties: true}` (the sparse CDO property diff). That is the supported confirm path for class assignments (e.g. `DefaultPawnClass`, `PlayerStateClass`) and for the Blueprint variable defaults the `configure_*` verbs add (e.g. `NumTeams`, `ScoreToWin`, `MaxRounds`).
