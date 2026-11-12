---
type: system
summary: "PinWrightRuntime: transport, dispatch and game-safe verbs in a Runtime module (Debug/Development, never Shipping) so -game processes and packaged Development builds are drivable; several instances of one project side by side."
date: 2026-11-08
tags: [architecture, modules, transport, game-mode, multi-instance]
---

# PinWrightRuntime module (contract for the split)

Decision (owner, 2026-11-08): PinWright works in `-game` processes and packaged Development builds, not only in the editor, and several processes of one project can be driven at once (parallel playtests). Editor-only verbs stay editor-only.

## Modules

| Module | Type | Builds | Holds |
|---|---|---|---|
| `PinWrightRuntime` (new, `Source/PinWrightRuntime`) | `Runtime`, `"TargetConfigurationDenyList": ["Shipping", "Test"]`, `"PlatformAllowList": ["Win64", "Linux"]`, LoadingPhase `Default` | Debug, DebugGame, Development (editor, `-game`, packaged); never Shipping or Test (TraceAnalysis / TraceServices are Developer modules) | HTTP transport, JSON-RPC, dispatch, handler registry, formatters, error codes, catalog (no wiki generation), runtime host subsystem, world/viewport accessor, instance file, game-safe handler folders |
| `PinWright` (existing) | `Editor` | editor targets | editor host duties (wiki generation and rendering, integration gates, editor readiness, background-throttle guard, setup screen), editor-only handler folders; depends publicly on `PinWrightRuntime` |
| integration sub-modules | unchanged (`Editor`) | | depend on `PinWright` and `PinWrightRuntime`, include path into `PinWrightRuntime/Private` |
| `PinWrightRecorder` | `Runtime`, denied Shipping + Test | as `PinWrightRuntime` | journal / live tail, so `drive.events_since` works in game |

Deny `Test` too if any moved code needs it; Shipping is mandatory. Code that must never ship is additionally inside `#if !UE_BUILD_SHIPPING`.

## Include paths do not change

Moved headers keep their path relative to the module root: `Source/PinWright/Private/Dispatch/Foo.h` that handlers include as `"Dispatch/Foo.h"` moves to `Source/PinWrightRuntime/Public/Dispatch/Foo.h` (headers other modules need) or `Source/PinWrightRuntime/Private/Dispatch/Foo.h` (runtime-internal). `PinWrightRuntime.Build.cs` adds its `Public` root to the public include path, so every existing `#include "Dispatch/Foo.h"` keeps compiling. Moved exported symbols use `PINWRIGHTRUNTIME_API`. Process-wide state reachable from a shared header (test latches, refcounts) lives in a runtime `.cpp` behind a `PINWRIGHTRUNTIME_API` accessor, never in a function-local static of an `inline` header function: each DLL gets its own copy, so a test or handler in another module sets state the runtime dispatcher never reads (`Dispatch/SafePoint.h`, `Dispatch/ScopedUnattendedRpc.h`, `Utils/RenderingAvailability.h`). Moved handler folders keep their folder name: `Private/Handlers/Input/` -> `Source/PinWrightRuntime/Private/Handlers/Input/`. Use `git mv` (history).

## Host and activation

- `UPinWrightRuntimeSubsystem : UEngineSubsystem` (`Public/PinWrightRuntimeSubsystem.h`) owns: draining auto-registrations, catalog init, transport bind/retry, the 0.1 s ticker, the job/event bridge. It exists in editor and game.
- Editor process (`GIsEditor`): serves exactly as today (derived or fixed port, `Saved/PinWright/gateway-port` + `gateway-token`), unless `-PinWrightPort=<N>` is given (see below). The editor module plugs its duties in through `IPinWrightEditorHooks` (`Public/PinWrightEditorHooks.h`, set by `FPinWrightModule::StartupModule` before engine subsystems initialize; null in game): integration loading before the drain, wiki generation after catalog init, editor readiness, skipped-integration lookup, wiki paths/rendering for `tools/call` docs. The modal-state probe and the transport launch policy are game-safe and live in the runtime module (`Transport/ModalStateProbe.h`, `Transport/TransportLaunchPolicy.h`); a game process registers the probe at post-engine-init. `UPinWrightSubsystem` keeps the settings-migration warning, the background-throttle guard, the never-saved-asset dirty hook and status getters forwarding to the runtime host.
- Game process (`-game`, packaged Development): serves **only** with `-PinWright` or `-PinWrightPort=<N|0>`; otherwise loads and does nothing. Loopback only.
- `-PinWrightPort=<N>` binds N, `-PinWrightPort=0` an ephemeral port. An instance started this way never writes or retracts `gateway-port` (that file stays the primary editor's).

## Instance file (every serving process)

`<ProjectSaved>/PinWright/instances/<pid>.json`, user-only permissions, written after a successful bind, deleted on clean shutdown; stale pids are skipped by readers.

```json
{"pid": 1234, "port": 51234, "token": "<per-process random 32 bytes hex>", "kind": "editor|game",
 "project": "<abs .uproject>", "startTime": "<ISO 8601 UTC>", "reason": "<-PinWrightLaunchReason or empty>",
 "map": "<current map>", "engine": "5.8"}
```

The primary editor additionally keeps `gateway-port` / `gateway-token` (MCP proxy unchanged). A game or `-PinWrightPort` instance authorizes with its own token from its instance file.

### Activation details (implemented)

- Primary editor = editor process without `-PinWrightPort`: settings port (`bEnableHttpTransport`, derived/fixed), bind retry, `gateway-port`, `PortAdvertisement` reconcile, the `jobs.jsonl` wipe and the HTTP-spill prune; its instance file carries the gateway-token (one token works either way); auth is enforced only with `bRequireAuthToken`, as today.
- Every other serving process (game with `-PinWright` / `-PinWrightPort`, editor with `-PinWrightPort`): one bind, no retry, never touches `gateway-port`, `jobs.jsonl` or the spill prune; a per-process random token (64 hex) is always required (`Authorization: Bearer <token>`), whatever `bRequireAuthToken` says. Bare `-PinWright` in a game binds an ephemeral port. An explicit switch overrides the `-ExecCmds` automation suppression and `bEnableHttpTransport`; `-PinWrightNoTransport` and `-PinWrightIsolatedTestChild=` still win.
- Readiness: editor via hooks (unchanged rule); game = engine initialized and a game world exists (`ping` reports `editorReady: true` then).
- Instance file `map` is the target world's package at bind time (not updated on travel); `startTime` is the module load time; a lost bind writes none; the path is logged at Display on bind. Packaged builds write under that build's `Saved/PinWright/instances`; with `-SaveToUserDir` the file follows `FPaths::ProjectSavedDir()` to `<UserSettingsDir>/<ProjectName>/Saved/PinWright/instances`, which readers match by the file's `project`.
- `UPinWrightSettings` moved to the runtime module; `OverrideConfigSection` keeps the ini section `[/Script/PinWright.PinWrightSettings]`.
- Registry: `FHandlerRegistration::bRuntime` (set by the registration macros from `PINWRIGHT_REGISTERING_RUNTIME_MODULE`, defined 1 only in `PinWrightRuntime.Build.cs`); wiki method pages print `Runtime: true|false`.

## World and viewport accessor

`Public/Dispatch/PinWrightWorld.h` (runtime):

```cpp
namespace PinWright
{
    // Editor: the PIE world while PIE runs (GEditor->PlayWorld), else the editor world. Game: the game viewport's world.
    PINWRIGHTRUNTIME_API UWorld* GetTargetWorld();
    // Editor: the editor world (never PIE). Game: nullptr (callers return EDITOR_ONLY / NO_EDITOR_WORLD).
    PINWRIGHTRUNTIME_API UWorld* GetEditorWorldOrNull();
    // Game viewport client of the target world (PIE or game), nullptr in editor-only state.
    PINWRIGHTRUNTIME_API UGameViewportClient* GetTargetGameViewport();
    // True in a -game / packaged process.
    PINWRIGHTRUNTIME_API bool IsGameProcess();
}
```

Moved handlers replace `GEditor->PlayWorld`, `GEditor->GetEditorWorldContext().World()` and similar with these, keeping the exact editor semantics each verb has today (a verb that targets PIE only keeps targeting PIE only). Mode-selecting helpers such as `ActorUtils::ResolveQueryWorld` move with their callers and gain the game branch.

## Which verbs move

A handler file moves when every verb in it works without `UnrealEd` in a game process (input, drive, capture/screenshot, world/actor/physics queries and edits at runtime, console and logs, widgets/UI inspection, networking, system/perf, reflection on live objects). A file with mixed verbs is split: game-safe verbs move, editor-only verbs stay in a same-named file in the editor module. Verbs that only make sense in the editor (assets, Blueprints, level editing, compilers, sequencer authoring, source control, builds, materials authoring) stay. Registration macros are the same in both modules; the registry lives in the runtime module.

In a game process, an editor-only verb is not registered at all (it is in the editor module); the wiki is generated by the editor only and marks each method `runtime: true|false` (from which module registered it).

## Tests, docs

Automation tests stay in their current module and test layout; tests of moved code compile against the new include paths unchanged. New tests: activation gate (no `-PinWright` -> no bind), instance file write/delete, `-PinWrightPort=0`, world accessor in game (smoke via `-game` automation if the harness supports it, else a documented manual check). Docs: `AGENTS.md` (module split, "Editor-only modules" line, the client rule now includes the CLI), `docs/arch.md` (layer diagram, init order), `docs/engine-version-support.md` (any new guarded API), wiki source for the CLI and game mode, `README.md`, `CHANGELOG.md`.

## Engine support

UE 5.3-5.8, Win64 + Linux, as for every module: guard version-specific APIs at the call site and record them in `docs/engine-version-support.md`.
