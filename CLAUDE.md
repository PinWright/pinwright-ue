Maintainer agent instructions; humans start at CONTRIBUTING.md.

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Unreal Engine C++ editor plugin ("PinWright") that enables AI assistants to remotely control Unreal Editor via an in-process MCP (Model Context Protocol) server. Includes portions derived from [ChiR24/Unreal_mcp](https://github.com/ChiR24/Unreal_mcp); see `THIRD_PARTY_NOTICES.md`.

- **Engine support: UE 5.3 through 5.8, all six green end to end in the version matrix (2026-09-10..12).** A new verb may **not** land on a 5.8-only API unguarded: guard it at the call site and add its row (symbol, call site, first engine version, substitute, guard location) to [docs/engine-version-support.md](docs/engine-version-support.md) in the same change. Win64 and Linux (every module carries `"PlatformAllowList": ["Win64", "Linux"]`; the Linux port landed with the clang fixes at `aa920932`), Editor-only modules (main module + gated integration sub-modules, see Module Split below)
- Single endpoint: stateless `POST /mcp` HTTP endpoint speaking JSON-RPC 2.0 (Auto-derive toggle / `bAutoDerivePort` is on by default, so the port derives per project as `19880 + hash(projectPath) % 10240` → `19880`–`30119`; disable it to bind the fixed `HttpPort`, which defaults to 19880; the bound port is published to `Saved/PinWright/gateway-port`, which the bundled stdio proxy re-reads per call so proxy configs follow a moved project or changed port); MCP `2025-06-18` revision, no session id, no batch in v1
- ~240 handler files with **1,191 registered RPC methods across 67 public namespaces**, organized into a dotted namespace tree; all methods are exposed through one MCP tool named `call` — agents pass `method=<dotted.name>` and `args={...}` as tool arguments (omit `args` to fetch the wiki page for the method)
  - **Provenance for those counts:** `operations` and `namespaceCount` in the generated `Saved/PinWright/wiki/registry.json` (host project, written 2026-08-18 14:24 from a tree already carrying the `image` namespace), on a boot whose startup line reads `skipped=[]`. A host with an integration's engine plugin disabled registers fewer, so re-read the registry rather than trusting the number. The file figure is `grep -rl REGISTER_RPC_HANDLER Source --include=*.cpp` = 242. Do **not** substitute a static macro count for the method count: `grep -c REGISTER_RPC_HANDLER(` over `Source` reads 1,180 sites and does not reconcile with what the editor registers.
- Beta/Experimental (v0.7.0)

## This plugin is the deliverable, and it is general-purpose

**PinWright is the product. The host project it is developed in is a forcing function, not the
goal.** User, 2026-08-22: *"our main goal is not creating some map or models, but providing best
pinwright plugin for our end users to use. so we must not focus on any [host]-specific nits in the
plugin itself. it is general-purpose."*

**The test for anything landing here: would this help someone building an unrelated level?** If the
answer is no, it belongs in the host project's `docs/`, not in the plugin.

Features are routinely *discovered* through host-project work — a mesh-orientation audit, a terrain
shape metric and an azimuthal deformer all came out of debugging one map's content. That is the
intended workflow. What must not follow them across the boundary:

- **Host-derived constants shipped as general defaults.** A threshold validated on one project's
  content is fine *if* the doc states its provenance and how a caller picks their own; a bare
  number tuned to one map's conventions is not.
- **Host vocabulary in identifiers, comments, error strings, test fixtures or worked examples.**
  Prefer synthetic fixtures (a generated staircase, a cube, a sphere) over content-specific ones,
  even when the defect was found on real content.
- **Assumptions about scale, axis convention or content layout** true only of the host.
- **Host paths** (`/Game/<HostContent>/…`, absolute checkout paths) anywhere in shipped code or
  user-facing docs.

Documentation whose only worked example is a host asset fails this test even when the code is
clean: a user cannot tell what the feature is *for*. Referencing the host as the environment the
plugin is *developed and tested in* — as this file does throughout — is legitimate and stays.

Operational rule: agents must use their MCP client surface, e.g. `mcp__pinwright__call` in this project. Do not probe or invoke the gateway with raw HTTP, `curl`, `Invoke-RestMethod`, `/health`, or legacy `/rpc` calls.

**See `docs/arch.md` for the full architecture reference** — layer diagram, request flow, auto-registration pipeline, FHandlerContext API, state management, testing architecture, and key files index.

**See `docs/rpc-design.md` before designing a new RPC verb** — the earned design rules (report only what happened, structural guarantees over discipline, required parameters with no safe default, verification the write path cannot fake, the three levels of persistence, batching / jobs / honest cancellation, tick safety, error-code registration, failure-direction tests, wiki rendering traps) plus a "before you ship a verb" checklist. **It is a living doc:** when a verb ships a hard-won design lesson, record it there in the section it belongs to. One-off engine quirks still go to `docs/lessons.md`; `rpc-design.md` is for lessons that should shape the *next* verb's interface.

**Issue board:** the public repo `PinWright/pinwright-board`, kept OUTSIDE this repo (it used to live in-repo under the plugin's `docs/` tree but was moved out so it is never reset/committed with plugin code). Clone it to `../../../.pinwright-board` — relative to the plugin directory, so it is a sibling of the host-project checkout, which is where every tool and skill here looks for it. One markdown file per issue, filename = `{id}.md`. Workflow rules, frontmatter schema, and status transitions live in the board's `README.md`. Status transitions: OPEN → IN-REVIEW → DONE. Edit the matching file **in place** when you find a new issue or verify a fix; never create a monolithic board document.

This board is for development tracking. The **customer-facing** channel is the public issue tracker at `https://github.com/PinWright/pinwright-ue/issues`. The agent-guided report/draft flow lives on the `support` wiki page (`docs/wiki-src/support.md`) and is surfaced from the wiki root index; end users and their agents file there, not on this internal board.

## Building

This is an Unreal Engine plugin — there is no standalone build system. To build:

1. Place this folder into a UE project's `Plugins/` directory
2. Regenerate project files (right-click `.uproject` → Generate Project Files)
3. Build via Visual Studio / Rider / UBT

**`Unable to build while Live Coding is active` — pass `-NoHotReloadFromIDE`. You do not need an exclusive build window.**

```powershell
& "$env:UE_ROOT\Engine\Build\BatchFiles\Build.bat" <HostProject>Editor Win64 Development `
  -Project="<...>\<HostProject>.uproject" -WaitMutex -NoHotReloadFromIDE
```

The diagnosis matters more than the flag, because the error names the wrong culprit. It is **not** caused by an editor for *this* project being open: it reproduces with **zero** processes of this project running. The blocker is **another project's** `LiveCodingConsole.exe`: a console left open by any unrelated project on the machine blocks this host's builds globally, and that is another project's tooling, which must never be killed. `HotReload.cs:274-278` gates on `bAllowHotReloadFromIDE && … && IsLiveCodingSessionActive(…)`, and `-NoHotReloadFromIDE` clears the first conjunct.

Without that second sentence the next agent reads the error, looks for an open editor of this project, finds none, and concludes something else is broken — which cost one agent ~40 minutes of waiting for a window that would never have been sufficient. Note the *link* step still needs the target DLLs unloaded, so a running editor **of this project** must exit before linking; the flag only removes the false Live Coding gate.

**Announce a build before starting one.** A plugin relink leaves a window (~3 min here) where `UnrealEditor-PinWright*.dll` files are mid-write: the editor then fails with `GetLastError=126` and `Plugin 'PinWright' failed to load because module 'PinWrightRecorder' could not be found`, which reads exactly like a broken tree. It is not — it is a link in flight. Check the timestamps in `Plugins/PinWright/Binaries/Win64/` against `UnrealEditor.modules` before concluding anything is wrong.

**While you are waiting for a link window, compile-check with `-SingleFile` — it needs no window at all.**

```powershell
& "$env:UE_ROOT\Engine\Build\BatchFiles\Build.bat" <HostProject>Editor Win64 Development `
  -Project="<...>\<HostProject>.uproject" -WaitMutex -NoHotReloadFromIDE `
  -SingleFile="<...>\Plugins\PinWright\Source\PinWright\Private\<file>.cpp"
```

It compiles that one translation unit and skips the link, running UHT first so a brand-new `UCLASS` header still gets its `.generated.h`. **The property that makes it safe is not obvious from the flag name: with a Live Coding session active UBT prints `Actions will be limited to compilation of specified files. Output will be sent to a temporary location.` — so it writes nothing into `Binaries/` and cannot disturb a running editor or an in-flight suite.** ~35 s per file.

Earned: a change was held for hours behind a suite run holding the DLLs. Compile-checking the ten touched files during the wait caught `error C4930: prototyped function not called (was a variable definition intended?)` — a most-vexing-parse, `FScopedSink Sink(FString())` declaring a function instead of a variable — which would otherwise have failed the build *inside* the link window it had waited all that time for. Dead waiting converts into validation; use it.

**Compile, run the suite, and launch the editor for any automated scenario through the capped launcher.** `scripts/Run-Capped.ps1` (arbitrary command) and `scripts/Run-SuiteCapped.ps1` (the suite) put the process in a Windows Job Object with a per-process memory cap, kill-on-close, and `-PriorityClass BelowNormal` by default, so a long build or a 20-minute suite leaves the machine usable. `JOB_OBJECT_LIMIT_PRIORITY_CLASS` applies to every process in the job and a job process cannot raise itself above it, so UBT's `dotnet`, `cl.exe` and `link.exe` and the editor's `ShaderCompileWorker` children are all covered, and none of them breaks away. BelowNormal and not Idle: an Idle job is starved whenever anything else wants CPU and its wall time balloons. **It is not for an interactive editor** — a visible `mcp__pinwright__editor_start` session the user is working in stays uncapped at normal priority; anything unattended (headless test runs, commandlets, scripted PIE probes, scripted sweeps) goes through the launcher. The `-NoHotReloadFromIDE` guidance above is unchanged — pass it inside `-CommandArgs`.

```powershell
# compile
& "<PLUGIN_ROOT>\scripts\Run-Capped.ps1" -Command "<UE_ROOT>\Engine\Build\BatchFiles\Build.bat" -CommandArgs @('<HostProject>Editor','Win64','Development','-Project=<HOST>\<HostProject>.uproject','-WaitMutex','-NoHotReloadFromIDE') -OutputPath "<HOST>\Saved\Logs\pw_build.log" -PriorityClass BelowNormal

# suite
& "<PLUGIN_ROOT>\scripts\Run-SuiteCapped.ps1" -HostProject "<HOST>\<HostProject>.uproject" -EngineRoot "<UE_ROOT>" -Filter PinWright -LogPath "<HOST>\Saved\Logs\pw_suite.log" -MemoryFraction 0.60 -PriorityClass BelowNormal

# automated / unattended editor (commandlet, scripted PIE probe, sweep)
& "<PLUGIN_ROOT>\scripts\Run-Capped.ps1" -Command "<UE_ROOT>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" -CommandArgs @('<HOST>\<HostProject>.uproject','-run=<Commandlet>','-unattended','-RunningUnattendedScript','-nopause','-nocefaccelpaint','-AutoDeclinePackageRecovery') -OutputPath "<HOST>\Saved\Logs\pw_job.log" -PriorityClass BelowNormal
```

`Run-Capped.ps1` prints and writes one `PINWRIGHT_JOB_RESULT verdict=… exit=… priority=… cap_gb=… peak_gb=…` line and exits 2 on `TIMEOUT` / `MEMORY_CAP_HIT`; `Run-SuiteCapped.ps1` prints the `PINWRIGHT_SUITE_RESULT` line documented under **Testing**. Both share `scripts/CappedJob.ps1`, the dot-sourced Job Object interop.

Build rules are in `Source/PinWright/PinWright.Build.cs` (C#) plus one `Source/PinWright<Integration>/PinWright<Integration>.Build.cs` per gated sub-module (see Module Split below). Key build notes:
- PCH + Unity enabled (`PCHUsage = UseExplicitOrSharedPCHs`, `bUseUnity = true`). Shared anonymous-namespace helpers are consolidated into per-cluster named-namespace headers (AGIRCompilerHelpers, NiagaraJsonHelpers, JsonBuilders, MGIRHelpers, BlueprintEnumHelpers, WidgetInspectHelpers, MeshBoundsHelpers, MaterialFinders, and the test-side AssetDumpTestHelpers / NiagaraJsonAssertionHelpers / LiveUiSnapshotTestHelpers / AssetDumpFixtureHelpers / WidgetXmlTestHelpers / BpirGraphTestHelpers / TestAssetTeardown) to dodge ODR collisions when Unity merges TUs.
- Many engine modules are conditionally added via `TryAddConditionalModule()` (MetaSound, StateTree, MovieRenderPipeline, Water, GameFeatures, ChaosVehicles, IKRig, PropertyBindingUtils, ClothingSystemEditorInterface). It no longer covers SmartObjects*/Mass*/PCG*/PoseSearch: the `ai.*` SmartObject/Mass RPCs in `Handlers/AI/AIHandler.cpp` are reflection-only (`FindObject` on `/Script/...` paths + `FProperty` access, zero linkage; they return `PLUGIN_DISABLED` when the plugin is disabled; note the `USmartObjectComponent` storage drift: `DefinitionAsset` on 5.3, `DefinitionRef` on 5.4+), and PCG/PoseSearch moved to gated sub-modules
- The main module no longer links `CommonUI`, `Chooser`, `GeometryScripting*`, `GeometryCore`, `GeometryFramework`, or `DynamicMesh`: those live in the gated sub-modules (the activatable-widget handlers moved to `PinWrightCommonUI`). `StructUtils` is linked as a separate module only on UE ≤ 5.4 (gated on `Target.Version` in the main and affected sub-module `Build.cs` files); from 5.5 it lives in CoreUObject. `UIExtension`/`CommonGame` (Lyra-only modules) are not required — UIExtension is referenced by path string, not linked

## Module Split & Integration Gating (v0.7.0)

The `.uplugin` ships the main `PinWright` module + `PinWrightRecorder` (both `LoadingPhase: Default`) and five integration sub-modules (`PinWrightGeometry`, `PinWrightPCG`, `PinWrightChooser`, `PinWrightPoseSearch`, `PinWrightCommonUI`), each `"Type": "Editor"`, `"LoadingPhase": "None"`, Win64 + Linux. Why: Fab consumers on launcher engines hit LoadLibrary error 126 because `UnrealEditor-PinWright.dll` hard-imported DLLs of engine plugins the consumer had disabled. Each sub-module hard-links its engine plugin and adds `PrivateIncludePaths` into `Source/PinWright/Private` so moved files keep their original `#include "Handlers/..."` shape (fix follows the UE 5.8 IKRig → IKRigUAF precedent).

- **Gate:** `Source/PinWright/Private/IntegrationGates.h/.cpp` holds the plugin → module → method-prefix table. `UPinWrightSubsystem::Initialize` calls `IntegrationGates::LoadEnabledIntegrations()` immediately before `DrainAutoRegistrations()`: for each entry, if `IPluginManager` reports the engine plugin enabled, `FModuleManager::LoadModulePtr` loads the sub-module (its handlers self-register through the exported collector and drain with the main-module ones); otherwise the integration is recorded skipped.
- **Startup line (greppable):** `PinWright integrations: loaded=[...] skipped=[...]` (`LogPinWrightIntegrations`).
- **Skipped-method convention:** the dispatcher's unknown-action path returns `PLUGIN_DISABLED` naming the disabled engine plugin for methods matching a skipped prefix; `WikiHandler` prepends an unavailability banner on skipped-namespace pages.
- **Descriptor:** every optional engine-plugin ref (`GeometryScripting`, `PCG`, `Chooser`, `PoseSearch`, `CommonUI`, `Interchange`, `InterchangeOpenUSD`, `ChaosCloth`) is `"Enabled": true, "Optional": true`. This exact shape is load-bearing on every edge; do not change it without re-reading `docs/lessons.md`:
  - `"Optional": true` is required so a launcher host whose receipt lacks the plugin silently skips it (BP-only consumers), and it is what satisfies UBT's undeclared-dependency validation (which checks only `bOptional`, never `bEnabled`) - removing a ref makes the Rocket packaging gate fail on the resulting warning.
  - `"Enabled": true` (NOT false) is required because EVERY name listed in a `Plugins` array is marked seen in the engine's reference BFS before any flag is checked; an `Enabled:false` ref leaves the name dangling and silently blocks a later enabled ref to the same plugin from elsewhere in the closure (on UE 5.8 this broke default boots: SkeletalMeshModelingTools' enabled `GeometryScripting` ref was dropped because our disabled ref claimed the name first, cascading into a fatal Interchange module-load failure). IKRig's `Enabled:false` UAF pattern is safe only because nothing in any default-boot closure references UAF with an enabled ref.
  - Net behavior: on hosts where the receipt allows the plugin, the ref enables it (and the integration sub-module loads); on launcher BP-only hosts the ref is receipt-dropped and the integration is skipped gracefully.
- **Ownership:** `PinWrightGeometry` = `Handlers/Geometry` + `geometry.*` (minus `SplineHandler.cpp`/`SplineHelpers.h`, which stayed in main; `spline.*` has no GeometryScripting dependency); `PinWrightPCG` = `Handlers/PCG` + `PCGIR` + `pcg.*` (the `pcgir.txt` sidecar still registers through the main module's `IrSidecarRegistry`); `PinWrightChooser` = `chooser.*` (carries the Chooser `Internal` include-path probe for both engine layouts: `Plugins/Chooser` on 5.4+, `Plugins/Experimental/Chooser` on 5.3); `PinWrightPoseSearch` = `pose_search.*`; `PinWrightCommonUI` = `ui.activatable_*`, `ui.list_stack_widgets`, `ui.get_active_widget` (`ActivatableLayerResolver` + `UiActivatableStackHandler`).

## Testing

Tests live in `Source/PinWright/Private/Tests/` plus a `Private/Tests/` tree in each integration sub-module (typed tests moved with their handlers) using UE's `IMPLEMENT_SIMPLE_AUTOMATION_TEST` framework: main-module tests compile into the main module DLL, sub-module tests into their sub-module DLL; there is no longer a separate `PinWrightTests` module. Sub-module tests only RUN on hosts where the owning engine plugin is enabled (a host with a plugin disabled silently skips that integration's tests; when test counts look low, grep the startup `PinWright integrations: loaded=[...] skipped=[...]` line). `WITH_DEV_AUTOMATION_TESTS` self-strips test registration in shipping builds, so no per-file guards are needed. Test files cover: auto-registration macro, class utils, contract consistency, dispatcher, handler context, HTTP API, JSON utils, path utils, plugin state, BPIR parser/tokenizer, compiler integration, decompiler, and round-trip tests, plus the `Tests/Transport/` suite covering the socket HTTP transport and SSE streaming (stream-vs-ticket gate selection, frame format, heartbeats, and the bounded bind-retry backoff schedule).

**Test fixture teardown never force-deletes.** `CleanupTestAsset` detaches loaded assets into the transient package, removes saved fixture files directly, and leaves reclamation to the periodic suite reset controlled by `pinwright.TestGcEvery` (default 25; command line `-PinWrightTestGcEvery=N`) **or** by the memory watermark `pinwright.TestMemoryWatermark` (default 0.55 of physical RAM; command line `-PinWrightTestMemoryWatermark=F`). Tests must not depend on GC or referencer-nulling at cleanup; use `PwTestAssetTeardown::DiscardCreatedAssetByObjectPath` only for a freshly-created, never-saved in-memory object path whose caller explicitly needs its trailing immediate GC; it does not remove a saved `.uasset`.

**Fixture packages go under `/Game/PinWrightTests`, and nothing else may be written.** The root is spelled once as `PinWrightSuiteMaintenance::ScratchRootPackagePath()`; `SweepScratchRoot()` removes its files and its directories, and `PinWright.zz_suite_end.ScratchRootIsEmptyOnDisk` runs that sweep last in the suite (the `zz_` prefix is what orders it last) and fails on a survivor. **A test may never save a shipped host package**: `editor.save_all` has no dry-run and no package filter, so any test invoking a save-everything verb wraps the call in `PinWrightSuiteMaintenance::FScopedForeignDirtyPackageSuspension`, which scopes the dirty set to the scratch root and restores the flags afterwards. Both rules and their limitations are in `docs/test-organization.md` → **Fixture Teardown**.

**The suite does not run on the host's startup map.** `PinWright.aa_suite_start.OpenBlankTransientWorld` sorts first (`aa_`, the mirror of the `zz_` gate) and swaps the editor world for a blank untitled one via `GEditor->NewMap(false)` behind the shared pre-swap survivor probe, so a test that dirties the world without a guard has no host asset to reach; details in `docs/test-organization.md` → **The suite-start blank world**.

**Because reclamation is deferred, a fixture must not be left SELECTED.** `USelection` is element-backed, so a selected fixture holds a typed-element handle onto a raw `UObject*` GC never nulls, and the end-of-frame element drain dereferences it after some later, unrelated test's GC frees the object — an `EXCEPTION_ACCESS_VIOLATION` in `FEngineLoop::Tick` blamed on that other test (`docs/lessons.md`; reference case `Saved/Logs/pw_wave12_c1.log:12290-12340`). `DiscardLoadedAssetNoGc` now deselects before detaching, which is the step `ObjectTools::DeleteSingleObject` performed and force-delete removal dropped; `PinWright.infra.contract.SuiteMaintenance.DiscardDeselectsBeforeDetachingFixture` ratchets the ordering. A test that selects anything itself — including one that trips a refused delete, which re-selects the object it declined to delete — must deselect it before returning.

**Scope the run to the changed areas, and run the whole selection in ONE editor instance.** Pick the group(s) covering what actually changed (`PinWright.Model.*`, `PinWright.Geometry.*`, `PinWright.Skeleton.*`, `PinWright.render.*`, `PinWright.Format.*`, …) rather than reaching for the full suite by default — it is 4000+ tests and usually far more than the change deserves. Escalate to the full suite only when the change *could* reach unrelated tests: a shared helper, a response-shape or serialization change, an error-code or registry table, a `Build.cs` / module-boundary change, or anything touching dispatch. Those genuinely ripple; a per-op geometry fix does not.

**Never launch one editor per group.** Multiple groups or individual tests go into a single run, with the filters concatenated into one invocation. Editor startup is ~40–55 s and each launch re-pays cook/DDC warmup, so N launches cost N times the same coverage and multiply the chance of hitting a modal or a link-window collision mid-sequence. State which groups you chose and why the rest was irrelevant — a scoped green only means something alongside that reason.

**Do not build actor probes with `RF_Transient` when the test drives a verb.** `TestWorldUtils::SpawnTransientCubeActor` sets `SpawnParams.ObjectFlags = RF_Transient`, and `UEditorActorSubsystem::GetAllLevelActors` — which every `actor.*` verb walks when no explicit world is passed — filters those out (`EditorActorSubsystem.cpp:386`, `!Actor->HasAnyFlags(RF_Transient)`). The probe is therefore invisible to the verb under test, and the failure looks like a verb defect rather than a probe defect. Transient probes are fine for tests that call a util directly with an explicit `UWorld*`; handler-level tests need a normal level actor (see `SpawnLabelProbeActor` in `Tests/Actor/TestActorLabelResolution.cpp`) wrapped in `FScopedEditorWorldActorGuard`, which destroys it and restores the level's dirty flag.

**BPIR documentation:** See `Saved/PinWright/wiki/bpir.md` for syntax (authored in `docs/wiki-src/bpir.md`; regenerated at editor startup), `docs/bpir-test-matrix.md` for test coverage and known gaps.

**Prepare and run plugin tests through the proxy (no NullRHI):**

`editor_prepare_tests({"filter": "PinWright"})` is the only sanctioned proxy test-preparation verb and the only test verb that returns a command. The `filter` is mandatory, non-empty, and has no default. The live PinWright guard runs first: a detected editor is a hard `EDITOR_ALREADY_RUNNING` response, while an unavailable probe is reported as `not_probed`, never treated as proof that the editor is stopped. The planner resolves the project's `EngineAssociation` to the matching `UnrealEditor-Cmd` executable and does not substitute another engine. A successful `COMMAND_READY` response contains the absolute project path, launch executable and `argv`, explicit absolute `logPath`, and checker executable and `argv` for `check_suite_log.py`.

The verb returns immediately. It does not compile, launch, wait, kill, own the test process, apply test-run timeouts or watchdog supervision, or return a test verdict. The caller owns execution and classification: run the returned launch executable with the returned launch `argv`, then run the returned checker executable with the returned checker `argv` and the exact same `logPath`.

The launch contract is `-ExecCmds="Automation RunTests <filter>,Quit"`, `-TestExit="Automation Test Queue Empty"`, and `-Abslog=<same absolute logPath>`, plus `-unattended`, `-RunningUnattendedScript`, `-nopause`, `-nocefaccelpaint`, `-ddc=InstalledNoZenLocalFallback`, and `-log`. It uses a real RHI and never adds `-NullRHI`.

The canonical command shape is:

```powershell
$env:UE_ROOT = "<UE_ROOT>"
$env:HOST_ROOT = "<PROJECT_ROOT>"   # root of the host UE project this plugin is installed into
$log = "$env:HOST_ROOT\Saved\PinWright\test-runs\<run>\automation.log"
& "$env:UE_ROOT\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$env:HOST_ROOT\<HostProject>.uproject" `
  '-ExecCmds=Automation RunTests PinWright,Quit' `
  '-TestExit="Automation Test Queue Empty"' `
  "-Abslog=$log" `
  -unattended -RunningUnattendedScript -nopause -nocefaccelpaint -ddc=InstalledNoZenLocalFallback -log

$checker = "$env:HOST_ROOT\Plugins\PinWright\Content\Python\check_suite_log.py"
& "$env:UE_ROOT\Engine\Binaries\ThirdParty\Python3\Win64\python.exe" $checker $log
```

**Do not launch that argv bare — hand it to `scripts/Run-SuiteCapped.ps1`**, which adds the memory cap, kill-on-close and BelowNormal priority described under **Building** above; the same rule covers any other unattended editor launch. Details in `docs/test-organization.md` → **Suite Memory Containment**.

**Retired historical note:** Earlier guidance described an in-band semicolon Quit path and proxy-owned timeout and watchdog supervision. That path is retired and is not a current contract. Use the returned command and the caller-owned checker above.

`-ddc=InstalledNoZenLocalFallback` selects a DDC cache graph with no ZenLocal store, for two independent reasons. **Speed:** mounting that store is the only thing at editor startup that builds an autolaunching `FZenServiceInstance` (`ZenServerInterface.cpp:2852-2921`), and on a host with no zenserver already running its spawn-and-wait-for-health loop costs the run **23.5 s** before the first test — the largest single item in the pre-test window. **Reachability:** where IPv6 loopback is refused machine-wide the editor cannot reach ZenServer at all, the default graph then has no writable node, and startup *aborts* with `Unable to use default cache graph 'InstalledDerivedDataBackendGraph'`. That second failure was diagnosed first by the version-matrix workflow, which passes the same flag in its T1 step (`.polyskill/skills/mcp-version-matrix/mcp-version-matrix.workflow.js:213-214`); keep both rationales together rather than letting one copy drift. **Engine range:** the graph exists from **UE 5.4**. On 5.3 the name resolves to nothing, the engine warns `Unable to create cache graph ... Reverting to the default graph` (`DerivedDataBackends.cpp:168`) and continues — non-fatal, and 5.3's default graph has no Zen store to wait for — so the flag is safe to pass unconditionally across 5.3–5.8, which is what the version-matrix skill does on one shared launch line. Do **not** substitute `-NoZenAutoLaunch`: it keeps the store and repoints it at `[::1]:8558` (`ZenServerInterface.cpp:1750-1760`), so with nothing listening every cache request fails and the resulting `Failed to connect to localhost port 8558` spam starves the automation tick — exactly the untrustworthy-run shape documented under **Check results** below. Hit rate is unaffected: the filesystem store takes every put alongside ZenLocal, and the 1.7 GiB engine pak stays mounted. The `Installed` spelling is the portable one — `%ENGINEVERSIONAGNOSTICUSERDIR%` resolves to the engine dir on a source build (`Paths.cpp:216-226`), so it is identical to `NoZenLocalFallback` there and correct on an installed engine, where a graph that lost its only writable store would silently fall back to the default graph and re-mount ZenLocal (`DerivedDataBackends.cpp:720-750`).

`-nocefaccelpaint` is required for host projects that bring up CEF web browser widgets (in-game web UI, `WebBrowser` widgets): under `-unattended` the editor's RHI doesn't support CEF's GPU-accelerated shared-texture paint — without the flag, CEF's async `OnAcceleratedPaint` (`CopySharedTextureSync` → `BUseSupportedRHIRenderer()`) asserts from the WebBrowserSingleton ticker and kills the suite mid-run at a random test. The engine switch forces CEF software paint instead. Do **not** substitute `-NullRHI` (some tests need a real RHI).

**Check results — assert the count, not just the absence of failures:**
```powershell
$log = "$env:HOST_ROOT\Saved\Logs\<HostProject>.log"
rg -n "Automation Test Queue Empty" $log                # the engine's own "<N> tests performed"
(rg "Test Started"      $log | Measure-Object).Count    # tests that STARTED
(rg "Result=\{Success\}" $log | Measure-Object).Count
rg -n "Result=\{Fail\}" $log
rg -c "Failed to connect to localhost port 8558" $log   # ZenServer outage -> run is untrustworthy
                                                        # (0 by construction under -ddc=InstalledNoZenLocalFallback:
                                                        #  no ZenLocal store, so nothing probes 8558. A nonzero count
                                                        #  means the launch dropped the flag.)
```

**Do not do the above by hand - it is mechanized.** check_suite_log.py (from Content/Python/) classifies a log, exits nonzero unless the run is clean, and needs no editor - which matters because the editor may already be dead. It is the caller-owned verdict authority; editor_prepare_tests only returns launch and checker metadata and never computes a verdict.

**Run it — and every Python in this repo — under Unreal's bundled interpreter, never `uv`, and never install a package into it.** That interpreter ships only pip and setuptools (no numpy, PIL, scipy), and `mcp_proxy.py` / `check_suite_log.py` are stdlib-only by design so it can run them as shipped.

**UE 5.8 package dirtying:** the engine-style `asset.mark_package_dirty()`, `Actor.mark_package_dirty()`, and `Package.set_dirty_flag()` calls are not Python APIs. Do not prescribe them from `python.execute`. The reflected PinWright replacement is `unreal.PinWrightPackageLibrary.mark_package_dirty(asset)` (or its actor/path form); read back with `is_package_dirty` / `is_package_dirty_by_path`, force the bundled-Python save with `unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)`, and verify the package is clean afterwards. For MCP callers, use `asset.mark_dirty` / `asset.is_dirty` and the matching save operation.

```powershell
cd Plugins/PinWright/Content/Python
& "$env:UE_ROOT\Engine\Binaries\ThirdParty\Python3\Win64\python.exe" -m check_suite_log <log> [--expected N]
& "$env:UE_ROOT\Engine\Binaries\ThirdParty\Python3\Win64\python.exe" -m unittest discover tests   # 224 tests, measured 2026-08-30
```

Seven states, worst to best. Exit code is 0 **only** for `COMPLETED_CLEAN`:

| state | means | check_suite_log.py outcome |
| --- | --- | --- |
| `MEMORY_EXHAUSTED` | the engine logged an allocation failure (`Ran out of memory allocating`, `from backup pool to handle out of memory`) **and** the queue did not drain — the host's wall, or the per-process Job Object cap `scripts/Run-SuiteCapped.ps1` applies | `EDITOR_TESTS_OUT_OF_MEMORY` |
| `CRASHED` | the editor died, on positive evidence: a `Fatal error:` / `Assertion failed:` banner, **or** a non-ensure report in `Saved/Crashes` inside the run window | `EDITOR_TESTS_CRASHED` |
| `DID_NOT_COMPLETE` | the queue never drained and nothing says it crashed — killed or wedged; a failure count from it measures nothing | `EDITOR_TESTS_INCOMPLETE` |
| `NO_TESTS` | nothing was enqueued, or nothing recorded a success | `EDITOR_NO_TESTS` |
| `COMPLETED_WITH_FAILURES` | drained with reds | `EDITOR_TESTS_FAILED` |
| `COMPLETED_WITH_SKIPS` | drained, nothing red, but ≥1 `PINWRIGHT_ASSERTIONS_SKIPPED` marker | `EDITOR_TESTS_SKIPPED` |
| `COMPLETED_WITH_MEMORY_PRESSURE` | drained and fully measured, but ≥1 `PINWRIGHT_MEMORY_WATERMARK_EXCEEDED` marker: a suite-maintenance collect could not get the working set back under the hard fraction | `EDITOR_TESTS_MEMORY_PRESSURE` |
| `COMPLETED_CLEAN` | drained, nothing red, every assertion ran | — |

`MEMORY_EXHAUSTED` outranks **both** `CRASHED` and `DID_NOT_COMPLETE`, and that ordering was earned by measurement rather than assumed. A real OOM does not stop quietly: UE logs its two allocation-failure strings, dies with a `Fatal error:` banner and writes a crash report whose type is literally `OutOfMemory` — so ranked below `CRASHED` the state would essentially never fire, and a deliberately capped run (Job Object at 0.05 of RAM) classified `CRASHED` before the reorder. Both of the older verdicts describe an OOM correctly and uselessly: "killed or wedged" sends the reader hunting a harness timeout, "the editor crashed" sends them to `Saved/Crashes`, and neither names the memory. The crash evidence is folded into the reason, not discarded. A run that *drained* despite an allocation failure — the backup pool exists to absorb one — reports `COMPLETED_WITH_MEMORY_PRESSURE` instead, because it measured everything. **Run the suite through `scripts/Run-SuiteCapped.ps1`** — same argv, plus a Windows Job Object with `JOB_OBJECT_LIMIT_PROCESS_MEMORY` at 60% of physical RAM (per-process, so shader workers are not charged to the editor), `JOB_OBJECT_LIMIT_PRIORITY_CLASS` at `-PriorityClass BelowNormal`, and one machine-readable `PINWRIGHT_SUITE_RESULT verdict=... cap_gb=... peak_gb=... priority=...` line. The in-process half is the maintenance reset's memory watermark. Both halves, their knobs and the escalation-marker rules are in `docs/test-organization.md` → **Suite Memory Containment**.

`COMPLETED_WITH_SKIPS` is **not** a red test and must not be "fixed" by making the skip an `AddError` — that re-creates `B-tests-host-dependent-fixtures-hard-fail`. The skip mechanism is correct; what it means is that this run did not measure what a clean run measures, so it may not be quoted as green. The detail line now prints `skipped=` beside the counts and names the skipping tests, because the marker sits *inside* the success total (`succeeded=4241 skipped=1`).

**Every commit hash cited in the baselines below predates the 0.8.0 open-source squash and is no longer reachable in this repo.** They survive as dated provenance — which measurement went with which change — not as revisions to check out. Do not try to resolve one; re-measure instead.

Verdicts on the reference logs: `pw_suite_orthotiles2` COMPLETED_CLEAN, `pw_suite_ortho` COMPLETED_WITH_FAILURES, `pw_suite_full2` and `pw_suite_clb` DID_NOT_COMPLETE, `pw_suite_extendguard` COMPLETED_CLEAN.

A clean `Result={Fail}` grep is **not** a pass on its own: a run that ends early greps clean too. `pw_suite_full2` (762 of 3778) and `pw_suite_clb` (610 of 3780, **zero** failures) were both quoted as evidence of green suites; both were killed commandlets. Do not grep the bare phrase `Automation Test Queue Empty` to test for completion — `-TestExit="Automation Test Queue Empty"` puts it in every log's echoed command line (2 hits in a killed log, 4 in a drained one). Require the `<N> tests performed` tail. Three things must hold together — `started == success + fail` (the queue drained), the started count matches the expected suite total (a shortened run is a red flag even at 0 failures), and the ZenServer probe returns 0 (its absence shows as repeated `Failed to connect to localhost port 8558`, which starves the automation tick and can release the editor mid-queue). Reference totals: **3844 tests, 3844 pass, 0 fail**, measured 2026-08-18 at local `3576104c`. **Provenance:** `Saved/Logs/pw_suite_orthotiles2.log` in the host project, which carries `Automation Test Queue Empty 3844 tests performed`, `Test Started` = 3844, `Result={Success}` = 3844, `Result={Fail}` = 0, and 0 ZenServer probe hits — so `started == success + fail` and the queue drained rather than truncating. `check_suite_log --expected 3844` returns COMPLETED_CLEAN. This is a working tree **11 commits ahead of `origin/master`**, so it is not the pushed-tree figure. The delta from the previous 3778 reconciles exactly against what landed since: +13 `render.capture_ortho_tiles.*`, +12 `image.*`, +13 `TileGridUtils`/`BitmapPaint`, +1 grid-overflow regression, and the 6 actor-label failures of `823e68ae` fixed rather than merely absent. Earlier figures, kept only so a disputed number can be traced: **3778 / 6 fail** at `88d9764b` (`Saved/Logs/pw_suite_ortho.log`, 2026-08-16 12:06), and **3758 / 2 fail** at `031adfb7` (`scratchpad/automation-ip10.log`, gitignored via `.gitignore:12`). Every citation here is a host-project or gitignored path, so a fresh clone has none of these logs: re-measure and re-cite rather than trusting the number alone.

**Working-tree measurements, 2026-08-20 — NOT a clean baseline, quoted only so the next run has something to reconcile against.** `Saved/Logs/pw_full.log`: `Automation Test Queue Empty 4509 tests performed`, 4485 pass / 24 fail, 0 ZenServer hits. `Saved/Logs/pw_full_gate.log` (same working tree plus the required-param consolidation): **3997 tests, 3971 pass, 26 fail**, `started == success + fail`, 0 ZenServer hits. The −512 delta reconciles test-for-test: **−532** per-verb required-param gate tests (all 532 removals are that family, none other), **+7** for the registry walk and its six mechanism tests, **+13** geometry/model tests another agent landed between the two runs. The 26 failures are all in `Geometry.*` / `Model.*` / `CRIR.*` / `core.docs_schema` / `render.exposure_pin` — another agent's in-flight work, 24 of them already failing in the 4509 run; none is in the consolidated family. Do not treat 3997 as a green reference: re-measure once that work lands. Later the same day, `Saved/Logs/pw_full_verify.log`: **3996 tests, 3993 pass, 3 fail**, `started == success + fail`, drain marker present, 0 ZenServer hits; `check_suite_log --expected 3996` returns COMPLETED_WITH_FAILURES. The 26 → 3 drop is the `CRIR.*` (2), `render.exposure_pin` (1), `core.docs_schema` (1) and most `Geometry.*` / `Model.*` reds landing; the survivors are `Geometry.Ops.ModelingOptions.SubdivideRecomputeNormalsReachesTheEngine`, `Geometry.Ops.WidenedOptions.BooleanSimplifyOutputReachesTheEngine`, `Model.Compiler.RecompilingAnXAtlasUnwrapReproducesTheSameMesh`. The −1 against 3997 is a test removed in that same window, not a registration failure.

**Green baseline, 2026-08-21 at local `39a3eb2b`: 4051 tests, 4051 pass, 0 fail.** `check_suite_log` returns **COMPLETED_CLEAN**. **Provenance:** `Saved/Logs/pw_wave2_suite.log` in the host project, which carries `Automation Test Queue Empty 4051 tests performed`, `Test Started` = 4051, `Result={Success}` = 4051, `Result={Fail}` = 0, and **0** ZenServer probe hits — so `started == success + fail` and the queue drained. Use this, not 3996, as the figure to reconcile against; it supersedes the three working-tree measurements above, which were explicitly not baselines. The run was launched by another workstream against binaries linked minutes earlier, and this entry is a reading of its log rather than a separate run — the log is the evidence either way. Reconciliation against the previous run in the same host (`Saved/Logs/pw_wave_suite.log`, 4039 / 4034 / **5 fail**, taken before that link): **+12** tests and **−5** failures. The +12 is 2 new `Geometry.AssetCreate` overwrite tests plus 10 landed with `1fe43fa4` / `6a80d2a7` / `39a3eb2b`; the −5 is those same commits fixing `infra.dispatcher.UnknownActionNoCloseMatch`, the three `infra.wiki_handler.MethodPage.Geometry{Bend,Taper,Twist}ExtentSemantics`, and `Model.Parser.AppendBuffersTakesTheSlotTagButNotTheScalarColor`. **Note `testComplete` reads `false` in the verdict line while `drainMarker` and `testExit` both read `true`** — the drain marker plus `started == success + fail` is what carries completion here, and `check_suite_log` still classifies COMPLETED_CLEAN on that basis.

**Reconciling the next run: +5 ids that never used to execute, +1 guard.** Five ids were strict dot-prefixes of other registered ids, so `FAutomationReport::EnsureReportExists` adopted each as a branch node (`AutomationReport.cpp:611` matches by full path, recursion at `:672`) and every leaf check in the controller is written `ChildReports.Num() == 0`. They never entered the queue, so they are ABSENT from every figure above rather than inflating one. Renamed to leaves: `infra.handler_context.RequireAssetPath` -> `.GamePathAndTraversal`, `niagara.decompile_nir.GraphParameterMapGet` -> `.GetLineFormat`, `...GraphParameterMapSet` -> `.SetLineFormat`, `niagara.reset_module_input` -> `.HandlersRegistered`, `niagara.search_modules` -> `.ClassifyAndScore`. **The 4051 figure above is unchanged: no run has been measured since.** Derive the next expectation as 4051 + 5 + 1 + whatever else the diff adds. The invariant is now enforced at runtime by `PinWright.infra.automation_registry.NoPrefixCollisions` (`Tests/Infra/TestAutomationTestIdPrefixCollisions.cpp`), which walks `FAutomationTestFramework::GetValidTestNames` and fails on any `PinWright.*` id that is a dot-prefix of another; non-PinWright pairs warn instead. **The direction that keeps reproducing is the reverse one — a NEW suffixed id added under an id that is itself a complete leaf silently kills that leaf** (it happened again 2026-08-28 to `niagara.graph.create_node`), so checking only that your own id is not a prefix of something existing is half a check. Because that runtime walk sees only the ids this host would run, the host-independent half is `Content/Python/check_test_ids.py` — a stdlib-only source scan that also covers `#if`-guarded ids and disabled sub-modules, runs standalone on any host, and is self-tested by `Content/Python/tests/test_test_id_prefix_scan.py`. Board ticket `B-test-ids-swallowed-by-dot-prefix`.

**Green baseline, 2026-08-22 at local `c67bf8c7`: 4264 tests, 4264 pass, 0 fail, 0 skips.** `Saved/Logs/pw_push_suite.log` carries `Automation Test Queue Empty 4264 tests performed`, `Test Started` = 4264, `Result={Success}` = 4264, `Result={Fail}` = 0, and **zero** `PINWRIGHT_ASSERTIONS_SKIPPED` markers -- so `started == success + fail`, the queue drained, and every assertion actually executed rather than being stepped over. Supersedes the 4051 figure above. **Reconciliation is by provenance, not test-for-test, and that is a stated limitation of this entry.** The +213 against 4051 spans two full waves -- capture-subject convergence (16 chunks) and the preview-scene rig (10) -- plus the +5 renamed dot-prefix ids and +1 guard recorded above, plus the follow-up fixes. Counting the delta from the chunk reports was deliberately not attempted: more than a dozen relayed figures were wrong across those waves, three separate measurements came in low, and a sum of summaries is exactly the kind of number this file exists to distrust. Re-derive from the log if you need the breakdown.

**Green baseline, 2026-08-22 at local `efe24958`: 4288 tests, 4288 pass, 0 fail, 0 skips.** Supersedes the 4264 figure above. **Provenance:** `Saved/Logs/pw_wave3_suite.log` in the host project, which carries `Automation Test Queue Empty 4288 tests performed`, `Test Started` = 4288, `Result={Success}` = 4288, `Result={Fail}` = 0, **zero** `PINWRIGHT_ASSERTIONS_SKIPPED` markers, zero `Fatal error:` / `Assertion failed:` banners and **0** ZenServer probe hits — so `started == success + fail` and the queue drained. `check_suite_log --expected 4288` returns **COMPLETED_CLEAN**, exit 0, `found=4288 started=4288 succeeded=4288 failed=0 skipped=0 performed=4288`. As with the 4051 entry, `testComplete` reads `false` while `drainMarker` and `testExit` read `true`; the drain marker plus `started == success + fail` is what carries completion. Startup line read `loaded=[geometry,model,pcg,chooser,pose_search,ui] skipped=[]`, so no integration's tests were silently absent.

**This delta *is* reconciled test-for-test, unlike the entry above.** +24 against 4264, and every one of the 24 was confirmed to have entered the queue by its `Path={PinWright.…}` lines in the log: **+6** `landscape.audit_shape.*` (`CleanLevelStaysClean`, `DirtyLevelStaysDirty`, `MetricSeparatesStampedFromOrganic`, `PerSegmentReadingCannotFail`, `UnknownCheckIdIsRejected`, `UnmeasurableIsNotClean`), **+8** `Geometry.MeshAudit.*`, **+10** `Model.HarmonicDeform.*`. The fourth workstream (skip-marker conversions, comment corrections, one assertion fix) added none, as expected. `PinWright.infra.automation_registry.NoPrefixCollisions` ran and passed, and a static sweep of every `"PinWright.*"` literal in `Source` found no id that is a strict dot-prefix of another — so nothing was adopted as a branch node and dropped from the queue.

**Two limitations of this entry, both real.** (1) The measured tree is `efe24958` with a clean working tree at link time (`UnrealEditor-PinWright.dll` / `-PinWrightGeometry.dll` linked 10:35 local, run launched 10:39); a parallel session committed `a5169b02` and `84e21777` at 10:42–10:43 and has uncommitted `Tests/Render/` edits, none of which were built or measured here. This is **not** a reading of HEAD. (2) The 20 `AddInfo` → `AddWarning` skip-marker conversions are **unproven by this run**, not confirmed by it: zero skip sites fired on this host, so the newly-visible path emitted nothing. A run that fires one is still needed to demonstrate the marker now reaches the log. The run used `-RenderOffscreen` alongside the documented flags, which is itself a reason the GPU-contention skips did not trigger.

**Measurement, 2026-08-28 at local `12ac2b44`: 4576 tests, 4576 pass, 0 fail, 4 skips — `COMPLETED_WITH_SKIPS`, NOT a clean baseline.** Quoted so the next run has a figure to reconcile against; it may not be cited as green. **Provenance:** `Saved/PinWright/test-runs/batch6/automation.log` in the host project, which carries `Automation Test Queue Empty 4576 tests performed`, `Test Started` = 4576, `Result={Success}` = 4576, `Result={Fail}` = 0, and **0** ZenServer probe hits — so `started == success + fail` and the queue drained. `check_suite_log.py` returns `COMPLETED_WITH_SKIPS`, exit 1, `found=4576 started=4576 succeeded=4576 failed=0 skipped=4 performed=4576 drainMarker=True testExit=True testComplete=False`.

**The four skips are host limitations, not flakes, and each names a fix this batch shipped whose counterfactual therefore went unproven here:** `core.safe_point.NestedNamedThreadPumpIsUnsafe` and `core.safe_point.DispatcherDefersFromNestedNamedThreadPump` (`reason=nested-named-thread-pump-not-entered` — this host's task graph did not run the probe inside a nested named-thread pump, so the widened `IsSafeNow()` gate could not be observed on that stack); `niagara.data_interface_consistency.WritePathReportsVerdict` (`reason=niagara-resolved-di-unavailable`); `niagara.reset_module_input.DynamicInputResetKeepsStackChain` (`reason=niagara_fixture_assets_absent`). Re-running does not move them. They want a live-editor pass.

**The +284 against 4292 spans three waves and is reconciled only at the wave level, which is a stated limitation of this entry.** The last leg is exact: 4559 → 4576 is **+17**, and every one is wave 3's — 1 `system.inspect.list_objects`, 1 `niagara.add_emitter` quiesce, 1 `bpir.round_trip` make_array, 2 `niagara.set_module_input` replacedOverride, 1 `widget.bind` non-variable, 2 `texture.create_noise_texture` format/octaves, 2 `lighting.setup_light_shafts`, 1 `infra.declared_params`, 4 `Model.SelfIntersection`, 2 `niagara.editor_open_guard`. The earlier legs (4292 → 4559) include this batch's waves 1 and 2 plus whatever other sessions landed in between, and were not counted test-for-test. Re-derive from the log rather than trusting the total.

**Two runs before this one were killed mid-queue and are recorded here because both produced the shape this file exists to distrust:** 4009 of 4559 with 1 failure, and 2225 of 4559 with **0** failures — no drain marker, no crash banner, log stops mid-test. Neither is a result. The cause was the harness terminating a long-running background command, not the suite; launching the editor detached (`Start-Process -PassThru`) is what let this run reach the drain marker. Python side: `unittest discover tests` = **160**, not the 225 recorded above — that figure is stale and the version-matrix commits agree at 160.

**Green baseline, 2026-08-22 at local `367dd9f3` (HEAD): 4292 tests, 4292 pass, 0 fail, 0 skips.** Supersedes the 4288 figure above. **Provenance:** `Saved/Logs/pw_wave4_suite.log` in the host project — a copy taken immediately after the run of the host project's default editor log, because the `-log=` override did not take and the default is overwritten by the next editor launch. It carries `Automation Test Queue Empty 4292 tests performed`, `Test Started` = 4292, `Result={Success}` = 4292, `Result={Fail}` = 0, **zero** `PINWRIGHT_ASSERTIONS_SKIPPED` markers, zero `Fatal error:` / `Assertion failed:` banners and **0** ZenServer probe hits — so `started == success + fail` and the queue drained. `check_suite_log --expected 4292` returns **COMPLETED_CLEAN**, exit 0, `found=4292 started=4292 succeeded=4292 failed=0 skipped=0 performed=4292`. As with the 4051 and 4288 entries, `testComplete` reads `false` while `drainMarker` and `testExit` read `true`. Startup line read `loaded=[geometry,model,pcg,chooser,pose_search,ui] skipped=[]`. Python side: `unittest discover tests` = **225**, matching the figure updated earlier in this wave (218 → 225, the 7 tests of `Content/Python/tests/test_skip_marker_literal.py`). Full build first, no `-SingleFile`: UBT `Result: Succeeded` with a real link of `UnrealEditor-PinWright.dll` at 12:32 local, run launched 12:33, drained 12:53.

**This is the first entry measured on HEAD rather than on a tree a parallel session had already moved past** — the limitation stated in the 4288 entry. The tree was `367dd9f3` with a clean working tree at both link and launch. **The +4 delta is reconciled test-for-test.** The diff `efe24958..HEAD` adds exactly four `IMPLEMENT_SIMPLE_AUTOMATION_TEST` macros and removes none: **+2** `render.tone_range.*` (`VerdictIsWithheldOutsideLitViewModes`, `LitFrameStillGetsTheVerdict`, from `a5169b02`) and **+2** `infra.skip_marker.*` (`EmitterWritesTheWireTextAsAWarning`, `BothIdShapesStayCountable`, from `28896975`). All four were confirmed to have entered the queue by their `PinWright.…` lines in the log. `896afb47` / `a0330754` / `367dd9f3` / `84e21777` / `dd37999a` added none, as expected. Three further `PinWright.*` ids appear in that diff (`render.capture.SomeTest`, `render.capture_subject_animation.PosedBoundsAreNotUsed`, `render.capture_asset_preview.PinnedCapturesReproduceWithinTolerance`) as **string data** inside the skip-marker tests, not registrations — do not count them. `infra.automation_registry.NoPrefixCollisions` ran and passed, and a static sweep of all 4296 unique `"PinWright.*"` literals in `Source` found no id that is a strict dot-prefix of another.

**The skip-marker conversion is still unproven end-to-end, and this run does not change that.** `28896975` routed 32 former call sites across ~15 test files through one shared emitter (`Tests/TestSkipReporting.h`). This run emitted **zero** `PINWRIGHT_ASSERTIONS_SKIPPED` markers — the same result as the 4288 run — so no skip site fired and the wire text has never reached a suite log. What the run *does* establish is that the conversion broke nothing: all ~15 touched files' tests still register and pass, and the emitter's own wire-text and countability assertions pass. It does **not** establish that a real skip now surfaces. `-RenderOffscreen` was used again, which is itself a reason the GPU-contention skips did not trigger; a run on a host holding the GPU is still what would demonstrate it. Note that when one does fire, `check_suite_log` returns `COMPLETED_WITH_SKIPS` and exits 1 — that is the gate working, not a regression.

**The two runs before the 4264 baseline are worth knowing about, because both were honest failures rather than flakes.** At 4262/4261/1 the single red was `render.parameter_parity.PoseListSetFieldsAreReachableFromTheWire` catching the commit immediately before it: four fields added to `FPoseListCaptureRequest` with no classification, which is the P5 defect (`viewDistanceScale` carried, forwarded and unreachable) trying to recur one commit later. At 4264/4262/2 both reds were one malformed H3 in `render.capture-subjects.md` -- rendering stops at the first `### `, so five `##` sections below it never reached the page. Note that `infra.wiki_handler.TopicPage.OrthoViewModeSplit` reported that as missing table content: a `Contains()` doc test cannot tell **content deleted** from **content orphaned**, so it names the wrong defect.

**A concurrent build closes your suite's editor, and the log makes it look like a test did it.** A linking build needs this project's editors gone, so another agent's build closed the editor mid-run: `LogSlate: Window '<Project> - Unreal Editor' being destroyed` → `Cmd: QUIT_EDITOR` → `Engine exit requested (reason: UUnrealEdEngine::CloseEditor())`, at 1514 of ~4000 with 0 failures and no drain marker. It reads like an `editor.quit` test escaping its fixture; it is not — check whether any `editor.quit` test had even started, and compare `Plugins/PinWright/Binaries/Win64/*.dll` mtimes against the kill time. Announce a suite run the same way a build is announced, and re-check for a running `UnrealEditor-Cmd.exe` before starting either.

**A count can be green and still prove nothing.** A test that takes a conditional-skip path reports success without running its assertions, and the started/succeeded totals cannot distinguish that from a real pass — see board ticket `B-test-skips-assertions-silently`, where `PinWright.render.capture_asset_preview.PinnedCapturesAreIdentical` skipped its only substantive assertions in 3 of 3 runs because another project's editor held the GPU. Asserting the total catches missing *tests*. Missing *assertions* are now caught too, but only for skips that emit `PINWRIGHT_ASSERTIONS_SKIPPED`: `check_suite_log` greps that marker (prefix match — the engine appends ` [file(line)]`), counts it and refuses `COMPLETED_CLEAN`, which is what turned a real 4242/4242/0 drained log from `COMPLETED_CLEAN`/exit 0 into `COMPLETED_WITH_SKIPS`/exit 1. **A conditional skip that does not emit the marker is still invisible** — emit it from every early return that steps over an assertion, **through `AddWarning`, never `AddInfo` and never `UE_LOG`.** **Correction, 2026-08-30: this file used to claim "`AddInfo` events are never written to the automation log at all". That is FALSE and it sent readers looking for a log-plumbing bug that does not exist.** The engine logs them: `FAutomationControllerManager::ReportAutomationResult` walks `Results.GetEntries()` and emits `EAutomationEventType::Info` through `UE_LOGF(LogAutomationController, Log, ...)` (`AutomationControllerManager.cpp:1685-1693`), and an archived run log (`Saved/Logs/Automation_PinWright_verify2.log`) carries 36 real `LogAutomationController: FIXTURE-SKIP:` lines — so the `FIXTURE-SKIP:` audit token works exactly as `docs/test-organization.md` documents. The real defect is narrower and still disqualifying: an Info entry carries **no severity and no marker**, so `check_suite_log` counts nothing, the test never lands in the report's `succeededWithWarnings` bucket, and the run classifies `COMPLETED_CLEAN` having measured nothing. That is why the emitter uses `AddWarning`. The `AddInfo` population was swept on 2026-08-30 (223 sites converted, 4 waived at the site as genuine post-measurement notes, plus the two `PINWRIGHT_SKIP_IF_*_FIXTURE*` macro bodies covering 47 call sites), and `Content/Python/check_test_skips.py` now gates both emitters. `UE_LOG` fails the other way: `bElevateLogWarningsToErrors` promotes a log warning to an error, re-creating `B-tests-host-dependent-fixtures-hard-fail`. **Whoever changes this figure must move the citation with it** — a bare number here is what made two agents argue over whether 3758 was a measurement or a recollection. The total rises with every added test, so reconcile a difference against what the change added rather than accepting it. Derive the expected total as *last measured + tests the diff adds* and assert it; a stale figure asserted as the expectation looks green while hiding that the new tests never registered. Do not derive it from `grep -c IMPLEMENT_SIMPLE_AUTOMATION_TEST`: 87 of those sit inside feature-availability `#if` guards and 11 evaluate false on a typical host, so the static count runs ahead of the executed one.

Use `Saved/Logs/<HostProject>.log` (the log filename follows the host project name) as the execution proof, and let `check_suite_log.py` read it — "should end with the marker" was the wording that left this unverifiable, and a human check under time pressure is what it left it to. The checker keys on the marker's full shape (`<N> tests performed`), refuses a match taken off a line that quotes the launch arguments, requires it to sit after the last test result, and prints a `provenance:` line naming the file and line number it read. No verified marker classifies `DID_NOT_COMPLETE`, loudly, with exit 1. Do not trust the editor process state or exit code alone. Sub-module tests only run where the owning engine plugin is enabled — if the count looks low, grep the startup `PinWright integrations: loaded=[...] skipped=[...]` line before assuming a regression.

## Architecture

### Decomposed Layers (under `Source/PinWright/Private/`)

1. **Transport** (`Transport/SocketHttpServer.h/.cpp`, `Transport/McpRequestCore.h/.cpp`, `Transport/McpTransportTypes.h`)
   A single custom socket HTTP/1.1 server behind the `POST /mcp` endpoint (`SocketHttpServer`, non-blocking single I/O thread, uniform across UE 5.3–5.8) with a completion-based async model, timeout sweeping, and the SSE streaming path — SSE is always available; whether a request streams or gets a job ticket is gated per request (see Wire Protocol). Request logic — envelope parse, the five supported protocol methods (`initialize`, `notifications/initialized`, `ping`, `tools/list`, `tools/call`), and routing `tools/call` payloads into the dispatcher — lives in `McpRequestCore`. Shared transport types (`FOnRpcRequest`, `FTransportCompletionCallback`) live in `McpTransportTypes.h`.

2. **Request Protocol** (`JsonRpc.h`)
   JSON-RPC 2.0 envelope helpers: `ParseEnvelope(RootObj, OutId, OutMethod, OutParams, OutIsNotification, OutErr)`, `BuildResponse(Id, Result)`, `BuildErrorResponse(Id, Code, Message, Data?)`. Numeric error codes per JSON-RPC 2.0: `-32700` parse, `-32600` invalid request, `-32601` method not found, `-32602` invalid params, `-32603` internal.

3. **Dispatcher** (`Dispatch/RpcDispatcher.h/.cpp`)
   `TMap<FString, FAutomationHandler>` for O(1) handler lookup. Reentrancy guard with deferred queue. Game thread enforcement. Defers during GC/serialization. `DrainAutoRegistrations()` bridges auto-registered handlers into the TMap at startup. The dispatcher is protocol-agnostic — the MCP transport reads the dotted method name from the `call` tool's `method` argument and invokes the dispatcher with `args` as the params object.

4. **Tool Catalog + Wiki** (`Catalog/ToolCatalog.h/.cpp`, `Catalog/WikiHandler.h/.cpp`, `Catalog/WikiOverlay.h/.cpp`, `Catalog/WikiDiskGenerator.h/.cpp`)
   `FToolCatalog` is a stub that only stores a weak dispatcher pointer — it generates no reference doc. The registered handler set is enumerated by `WikiDiskGenerator` at editor launch and written to `Saved/PinWright/wiki/` — pages as markdown, plus a machine-readable `registry.json` (operation count, namespace count, per-namespace method counts + maturity tiers) that is the generated source of truth for every published count. `WikiHandler::RenderPage(path)` walks the namespace tree and renders branch / leaf / method pages — it is the discovery surface, reached through the single `call` MCP tool by passing `method=<path>` with no `args`. `FWikiOverlay` loads hand-authored overlay sources from `docs/wiki-src/<path>.md` and merges prelude + per-method H3 sections into the auto-content. `WikiDiskGenerator` writes the full assembled tree (auto-content + overlays) to `Saved/PinWright/wiki/`, and the primary wiki-discovery response returns an on-disk file path reference to that output rather than inline markdown — inline rendering remains only as the cold-start / not-found fallback. `tools/list` returns a single descriptor for the `call` tool; the dotted RPC method travels as a string value in the `method` argument, never as a tool name.

5. **Subsystem** (`Public/PinWrightSubsystem.h`, `PinWrightSubsystem.cpp`)
   `UEditorSubsystem` that orchestrates Transport, Dispatcher, and Catalog. Wires delegates, starts HTTP transport, runs a 0.1s ticker for timeout sweeps and deferred processing. Skips init during commandlet execution.

6. **Handlers** (`Handlers/`) — ~90 files organized in domain subdirectories (Actor/, Blueprint/, Material/, UI/, etc.)

### Auto-Registration System

Handlers use a static auto-registration macro instead of manual registration:

**`Handlers/HandlerRegistration.h`** — `REGISTER_RPC_HANDLER(Method, Category, Summary, Params)` macro. Uses `__COUNTER__` for Unity build safety. At static init, pushes `FHandlerRegistration` records into a function-local static array. At subsystem init, `DrainAutoRegistrations()` moves them into the dispatch TMap.

**`Handlers/ParamSpec.h`** — Parameter schema macros:
- `RPC_PARAM_REQ(Name, Type, Desc)` — required parameter
- `RPC_PARAM_OPT(Name, Type, Desc)` — optional parameter
- `RPC_PARAM_DEF(Name, Type, Desc, Default)` — optional with default
- `RPC_PARAMS(...)` / `RPC_NO_PARAMS` — parameter array wrappers

**`Handlers/HandlerContext.h`** — Per-request context object (`FHandlerContext`) providing:
- Typed param getters: `GetString()`, `GetNumber()`, `GetBool()`, `GetInt()`, `GetVector()`, `GetRotator()`, `GetObject()`, `GetArray()`
- Validation: `RequireString()`, `RequireAssetPath()`, `RequireInt()` (auto-sends error on missing)
- Response: `SendSuccess(Result)`, `SendError(Code, Message)`, `SendUnsupportedEngineVersion(RequiredVersion, Feature)` (standardized `UNSUPPORTED_ENGINE_VERSION` rejection for features that need a newer UE than the running editor)
- Access: `GetSubsystem()`, `GetRequestId()`, `GetMethod()`, `GetRawPayload()`

### Handler File Pattern

Every handler follows this structure:

```cpp
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"

REGISTER_RPC_HANDLER("namespace.verb", "namespace", "Summary",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Description"),
        RPC_PARAM_OPT("flag", "bool", "Description")
    ))
{
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;
    // ... logic ...
    Ctx.SendSuccess(ResultJsonObject);
    return true;
}
```

When adding a new handler:
1. Create a `.cpp` file in the appropriate `Private/Handlers/<Domain>/` subdirectory
2. Use `REGISTER_RPC_HANDLER` — no header declarations or manual registration needed
3. The macro auto-registers at static init time; the dispatcher picks it up at subsystem init
4. Take the interface decisions from `docs/rpc-design.md` (response honesty, required params, verification, batching, job handles, tick-safety gating, error codes) and run its "before you ship a verb" checklist before calling the verb done

### Supporting Modules

- **State** (`State/`) — `FPluginState` singleton owning `FBlueprintTracker`, `FSaveThrottler`, Sequencer/Niagara registries. Legacy `GBlueprintExistsInflight` etc. macros in `PinWrightGlobals.h` resolve to `FPluginState::Get()` accessors.
- **Utils** (`Utils/`) — Extracted utilities: `PathUtils`, `AssetUtils`, `ClassUtils`, `JsonUtils`, `LogUtils`, `PropertyUtils`, `ActorUtils` (version branching is done inline with `Misc/EngineVersionComparison.h` macros, see UE version compat below)
- **`PinWrightHelpers.h`** — Umbrella include that delegates to Utils/ modules
- **`PinWrightSettings.h/.cpp`** — per-user `UDeveloperSettings` subclass (Editor Preferences); team-shared output roots live in `PinWrightProjectSettings.h/.cpp` (Project Settings, defaultconfig)
- **`PinWrightModule.cpp`** — Module startup/shutdown

## Wiki Authoring Constraints

Wiki-discovery pages are now generated to disk at editor launch and returned to agents as on-disk file path references (`WikiDiskGenerator` writes the assembled tree to `Saved/PinWright/wiki/`; `call("<path>")` returns `{page, wiki, hint}` pointing at that file). Every reference carries a `hint` steering agents to read/grep that on-disk wiki folder with filesystem tools rather than repeating `call()` doc requests. Because the page body no longer travels inline through the `tools/call` channel, the former 20,000-char ceiling is no longer discovery-blocking for the on-disk pages.

**Soft guideline: keep pages under ~20,000 characters.** The budget now applies only to (a) the cold-start / not-found inline fallback, which still renders markdown directly into the `tools/call` response before the disk tree exists, and (b) keeping the generated files readable and greppable. An over-budget page is a quality smell to clean up opportunistically, not a serious error.

What renders into a namespace page (`WikiHandler::RenderPage`):
- The H1 + the **prelude** of `docs/wiki-src/<ns>.md` (everything from start of file up to but not including the first `## ` or `### ` line).
- The auto-generated `## Subgroups` index (registered child namespaces).
- The auto-generated `## Methods` index (one line per registered method in this namespace, sourced from the registry — *not* from hand-written overlay content).
- `##` sections in the overlay **that appear before the first `###`** — these are visible on the namespace page.
- `### method.name` H3 sections do NOT render on the namespace page — they surface only when an agent calls `call("namespace.method_name")` directly. They cost no tokens on the namespace page itself, only file size on disk.

**RENDERING STOPS AT THE FIRST `### ` LINE — on every page, not just method sections.** The consequence is broader than "H3 method sections don't render": **any `##` section written *after* the first `###` is invisible in the rendered page**, silently. Audited 2026-08-14: this had swallowed ~26.5 KB of shipped content across 5 topic pages truncated by their own `###` — including the `wiki.md` "Reporting bugs" section, which was the *only* inbound pointer to `support.md`, the customer-facing report channel — plus 5 namespace pages (`system`, `editor`, `niagara`, `niagara.graph`, `skeleton`) whose `## See also` sat below their method H3s and therefore never rendered. A dead link inside a dead region is doubly invisible. **Put every `##` editorial section above the first `###`, and prefer bold labels to `###` on topic pages.**

What renders into the root index (`call()`):
- The *prelude* of each top-level namespace overlay (above the first `##`/`###`). Content below the first `##` is hidden from the root.
- **A `## Task guides` index of standalone guide pages**, derived at generation time from `FWikiCache::TopicNodes` entries whose slug contains **no dot** (`workflows` first, then alphabetical). Dotted topic pages such as `level-building.terrain-and-water` are deliberately excluded — their parent hub already links them — and registered namespaces/methods are never enrolled as topics, so they cannot leak in. **Authoring rule: a standalone guide page is auto-listed on the root, so its opening sentence must be short and self-contained** — the index derives each summary from the first sentence of the page's prelude, clipping at a clause boundary past 140 chars. Drop a page into `docs/wiki-src/` and it is listed at the next launch; remove it and it disappears. Nothing is hardcoded.

**Every top-level namespace must be classified in `docs/wiki-src/maturity.json` (`core` | `experimental` | `internal`) — the map fails closed.** The tier drives the root-index marker and the `Stability:` line on the namespace page. A namespace with **no** entry (or an entry whose value is not one of the three) renders `(unclassified)` and `Stability: unclassified`, the least-trusted tier, and `registry.json` publishes `"tier": "unclassified"` for it. It used to render *bare* — which is exactly what `core` renders, and the root legend says "unmarked namespaces are core" — so an omission silently advertised the namespace as solid primary surface (`B-maturity-unmapped-namespace-fails-open`). Adding a namespace without an entry fails `PinWright.infra.wiki_handler.Maturity.EveryRegisteredNamespaceIsClassified`, which names the offender; a map key matching no registered namespace is only a `LogWikiHandler` warning at cache build. `unclassified` is a render-time fallback, never a legal value to write into the file.

### Structural quality rules

1. **Prelude = 2 sentences max.** Purpose + when-to-use vs. nearest sibling. No bullets. No method names. No bolded paragraph sub-labels (e.g. `**Cross-cluster overlap to know about:**`) — they smuggle multi-paragraph content into the root index, defeating the cutoff.
2. **Never duplicate the auto-generated `## Methods` index.** Hand-written `**Key methods:**`, `**Methods:**`, `Key methods at this branch:` bullet lists that enumerate `namespace.method` names with one-line summaries are pure dead weight — delete them when found.
3. **Move editorial content below `##` headings.** Cross-cluster overlap warnings, pitfalls, "see also" lists all belong under `##` on the same overlay file. They render on the namespace page (load-bearing) but are hidden from the root (where they would blow the budget).
4. **Extract large editorial blocks (>2 KB) to topic pages.** Self-contained reference material — text-IR specs, dump-file format contracts, gotcha catalogs, layout-math primers — goes into a new `docs/wiki-src/<ns>.<topic-slug>.md` file. The discovery code in `WikiHandler.cpp` (around lines 169-188) auto-enrolls any `.md` whose slug doesn't collide with a registered category into `TopicNodes`, making it instantly navigable as `call("<ns>.<topic-slug>")`. Reference from the parent overlay under `## See also`.

### When you find an oversized page

Clean it up as a quality pass when convenient:
- Render the page (call the MCP tool) and confirm the byte count.
- Find the offender: oversized prelude, duplicated method index, or oversized inline editorial block.
- Apply the appropriate rule above. Prefer extraction over deletion — preserving editorial content matters.
- Re-render to confirm it now fits within ~20,000 chars.
- File a board entry if the size was caused by something the renderer or auto-content layer should have caught (e.g. a method-index list growing past safe limits without a structural fix).

## Wire Protocol

This section is for transport maintainers. Agents should not manually send these envelopes; use the MCP client-exposed `call` tool instead.

Single endpoint: `POST /mcp`. JSON-RPC 2.0 envelope on both directions. `Content-Type: application/json` by default. Every request must carry `Authorization: Bearer <token>` unless auth is disabled via the `bRequireAuthToken` setting. Stateless — the server issues no `Mcp-Session-Id` and requires none on subsequent calls; concurrent clients are independent. `GET /mcp` returns 405; other GETs (Codex OAuth probes) return 404; every response carries an explicit `Content-Type` (Codex's rmcp hard-fails otherwise). No batch (JSON arrays) in v1.

A request upgrades to an SSE streaming response (block-and-stream: progress frames plus the terminal result on one call, matching UE 5.8's block-by-default model) whenever **both** hold: `params._meta.progressToken` present and `Accept: text/event-stream` sent. The caller opts out with tool-call `args: {wait: false}`, which returns the job ticket immediately (fire-and-forget + `system.job_status` polling, unchanged); `wait` absent or `true` blocks-and-streams, and `wait` is stripped from `args` before the handler runs. Plain-JSON clients (no token or no Accept) always get tickets byte-identically to before. The stream is `text/event-stream` with `event: message` / `data: <json>` frames: `notifications/progress` frames carry `progressToken`, `progress`, `message`, and always `ticket_id` (job tickets stay canonical; a dropped stream degrades to polling `system.job_status`), the final frame is the normal JSON-RPC response, and `: ping` heartbeat comments go out every `SseHeartbeatSeconds` (default 15). The socket transport is the only transport; SSE is always available on it, gated per request as above. Full contract: `docs/wiki-src/mcp-transport.md`.

Supported protocol methods (handled inside the transport, not the dispatcher):

- `initialize` — handshake. Returns `{ protocolVersion: "2025-06-18", capabilities: { tools: { listChanged: false } }, serverInfo: { name, version }, instructions }`. The optional `instructions` client hint is the approved `mcp-instructions.md` text with `{{PINWRIGHT_WIKI_DIRECTORY}}` replaced by the absolute `<Project>/Saved/PinWright/wiki` path, normalized to forward slashes with no trailing slash. It directs clients to read/search the flat on-disk wiki, distinguishes documentation lookup from RPC execution, and requires `args: {}` for no-argument RPCs. Keep the embedded direct response in `McpRequestCore.cpp` and proxy-local response in `Content/Python/mcp_proxy.py` byte-for-byte synchronized with the canonical file; focused tests guard both paths against drift.
- `notifications/initialized` — client-sent notification (no `id`). Server responds `HTTP 202` with empty body.
- `ping` — liveness probe. Returns `{}`.
- `tools/list` — returns `{ tools: [...] }` carrying a single descriptor for the `call` tool. No pagination.
- `tools/call` — invokes the `call` tool, routed by argument shape (see below).

### The `call` tool

`tools/list` returns one descriptor:

```json
{
    "name": "call",
    "description": "Invoke an PinWright RPC. Pass method='<namespace.verb>' and args={...} to execute; omit args to fetch the wiki page for the method; omit both to get the root namespace index. 'path' is accepted as an alias for 'method'; any other argument field is rejected.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "method": { "type": "string" },
            "args":   { "type": "object" }
        }
    }
}
```

`tools/call` with `name: "call"` routes by argument shape:

- `arguments = {}` (no `method`, no `args`) → wiki root namespace index. The primary response returns a JSON path reference (`{wiki, root, hint}`) to the generated `Saved/PinWright/wiki/` tree.
- `arguments = { method: "<path>" }` (no `args` field) → wiki page for that namespace or method. The primary response returns a JSON path reference (`{page, wiki, hint}`) to the generated file on disk.
- `arguments = { method: "<dotted.method>", args: {...} }` → execute the RPC: `method` is the dotted name handed to `FRpcDispatcher`, `args` is the params object.
- `path` is accepted as an alias for `method`; supplying both with conflicting values is rejected with `-32602`.
- Any other top-level argument field is rejected with `-32602` naming the offending field(s), the valid fields (`method`, `args`), and the wiki root index path. Previously unknown fields were silently ignored and the request fell through to the root-index response.

There is no dotted-to-underscore tool-name flattening. The dotted RPC method is a string *value* in the `method` argument, not a tool name on the wire.

Request envelope (execution path):

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"call","arguments":{"method":"actor.list","args":{}}}}
```

Success response:

```json
{"jsonrpc":"2.0","id":1,"result":{...}}
```

Error response (envelope-level only — parse / invalid request / unknown protocol method / internal):

```json
{"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"Method not found"}}
```

Numeric error codes follow JSON-RPC 2.0: `-32700` parse, `-32600` invalid request, `-32601` method not found, `-32602` invalid params, `-32603` internal.

`tools/call` result wrapping — the dispatcher's success/error is folded into MCP's tool-call result shape, **not** a JSON-RPC error:

```json
{"content":[{"type":"text","text":"<stringified result>"}],"structuredContent":{...},"isError":false}
```

Handler errors return `isError: true` with the message in `content`. The numeric error code travels in the text body, not as a JSON-RPC envelope error. The primary wiki-discovery response carries the JSON path reference (`{wiki, root, hint}` for the root index, `{page, wiki, hint}` for a page) in `content[0].text` **with** matching `structuredContent`; the `hint` steers agents to read/grep the on-disk `Saved/PinWright/wiki/` folder with filesystem tools instead of repeating `call()` doc requests; `WikiHandler` renders overlays from `docs/wiki-src/` and `WikiDiskGenerator` writes the assembled tree to `Saved/PinWright/wiki/`. Only the cold-start / not-found fallback returns raw markdown in `content[0].text` and omits `structuredContent`. Long-running ops from streaming-capable requests block-and-stream by default (see Wire Protocol); non-streaming requests (or `wait: false`) get a job ticket synchronously as a regular successful `tools/call` result — poll progress by calling the `call` tool again with `method: "system.job_status"`.

Every ERROR result whose method is known also carries a doc pointer: a final `Docs: <absolute wiki page path>` text line plus a top-level `docs: {page, wiki}` field (the exact method page when it exists on disk, else the nearest parent namespace page found by stripping dotted segments, else `index.md`; omitted entirely before the first wiki generation writes the on-disk tree). The structured `docs` field survives the oversize-spill rewrite; successes are unchanged.

## Aspect Version Bumping

The asset-dump cache (the `.dumpcache.json` marker beside each asset's dump dir under the configured dump root, default `<ProjectSavedDir>/PinWright/asset-dumps`) decides freshness per aspect file using `GetAspectVersion(RelativeFile)` in `Source/PinWright/Private/Handlers/Asset/AssetDumpCache.cpp`. When you change a dumper's serialized output, bump that aspect's number in the `Versions` table there — otherwise existing caches stay "fresh" and keep serving the old format until a caller passes `force=true` **to `asset.dump_folder`** (the only verb that accepts it, see below) or wipes the cache by hand. The Slot.Parent recursion fix shipped without a bump and left 10 MB stale `tree.xml` files everywhere; don't repeat that.

**`force=true` exists on `asset.dump_folder` only. `asset.dump` does not declare it and rejects it with `UNKNOWN_PARAMS`.** `IsDumpFresh` is called from the two folder paths and nowhere else, so `asset.dump` never consults the cache: it re-dumps on every call (it still *writes* the marker afterwards). The rejection is the dispatcher's unknown-parameter gate working correctly — it is not the flag being broken or missing, and retrying with a different spelling will not find it. If you need to defeat a stale cache for one asset, just call `asset.dump`; it never served you a cached aspect in the first place.

**`asset.dump` writes under `<ProjectSavedDir>/PinWright/asset-dumps` unless you pass `outRoot`, and nothing in the response says the mirror was not the target.** A project that keeps a *committed* dump mirror must pass `outRoot` on **every** call; omit it once and that dump lands in `Saved/` while the mirror silently goes stale, and the next reader compares against text that no longer describes the asset. Both `asset.dump` and `asset.dump_folder` take `outRoot` (relative paths resolve against the project dir). The durable fix for a project that always wants the mirror is the `AssetDumpRootDirectory` project setting (Project Settings → Plugins → PinWright (Project)), which moves the default so no per-call argument is needed. Multiple agents got this wrong on 2026-08-23.

Rule: any commit that changes serialized bytes for an aspect (new/removed fields, format reshape, different escaping, schema rev) bumps the entry in the same commit. Pure refactors, log-message edits, and renames that don't alter output don't need a bump. If an aspect is brand-new (no prior dumps exist), no bump is needed because the cache freshness check treats a missing entry as "must regenerate."

## Conventions

- **Method naming**: dot-separated `namespace.verb` (e.g. `actor.spawn`, `blueprint.compile`, `material.set_parameter`)
- **Error codes**: domain-specific uppercase (`CLASS_NOT_FOUND`, `SPAWN_FAILED`, `ASSET_NOT_FOUND`) plus the standard codes listed under Request Protocol above. `UNSUPPORTED_ENGINE_VERSION` is reserved for the `SendUnsupportedEngineVersion` helper (a feature that requires a newer UE than the running editor) — never fake-success a method that can't run on the current engine. Every emitted code must be declared as an `ERR_*` constant in `Handlers/ErrorCodes.h` first; a raw `TEXT("...")` code at a call site fails `PinWright.core.error_codes.AllEmittedCodesAreRegistered` (see `docs/rpc-design.md` §7)
- **Parameter aliases**: handlers accept both `camelCase` and `snake_case` variants for backward compat (e.g. `classPath` and `class_name`)
- **Thread safety**: all requests marshaled to game thread; deferred during GC/serialization; state through `FPluginState` with `FCriticalSection` mutexes
- **UE version compat** (supported range UE 5.3–5.8, all six green in the version matrix — every symbol that is not present across the whole range is guarded, and `docs/engine-version-support.md` records each guard and its substitute; a new version-specific symbol must be added there in the same change): guard version-specific APIs with `#include "Misc/EngineVersionComparison.h"` + the comparison macros: `#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, X, 0)` for "5.X or newer", `#if UE_VERSION_OLDER_THAN(5, X, 0)` for "older than 5.X", and `#if (UE_VERSION_NEWER_THAN_OR_EQUAL(5, X, 0) && UE_VERSION_OLDER_THAN(5, X+1, 0))` for "exactly 5.X". Use `__has_include()` for headers that moved between versions. Do **not** write bare `ENGINE_MAJOR_VERSION`/`ENGINE_MINOR_VERSION` in `#if` conditions (reserve those macros for runtime values), and do **not** use `UE_VERSION_NEWER_THAN(5, X, 0)` to mean "5.X+" — it is strict and drops the 5.X.0 release; use `UE_VERSION_NEWER_THAN_OR_EQUAL` instead. Engine `UCLASS()` types that lack a `*_API` export macro can't be referenced cross-module via `StaticClass()`/`NewObject<T>`/`Cast<T>`/`FGraphNodeCreator<T>` (link error) — resolve them by reflection: `FindObject<UClass>(nullptr, TEXT("/Script/Module.Class"))` + `NewObject<ExportedBase>(Outer, Cls)` / `Obj->IsA(Cls)` + `static_cast`. Reject features unavailable on the running engine with `Ctx.SendUnsupportedEngineVersion(...)`. Reusable compat shims: `Material/MaterialInputIterCompat.h` (`ForEachExpressionInput`), `Handlers/Geometry/CollisionHelpers.h` (now in `Source/PinWrightGeometry/Private/`), `Handlers/Audio/MetaSound/MetaSoundPathUtils.h` (paged-graph accessors). `FInstancedStruct`/StructUtils is linked as a separate module only on ≤5.4 (see `Build.cs`; it merged into CoreUObject in 5.5).

## Key Configuration

Two settings classes:
- `UPinWrightSettings` (per-user, `Config=EditorPerProjectUserSettings`) in Edit → Editor Preferences → Plugins → PinWright:
  - HTTP: per-project derived port `19880`–`30119` by default (`bAutoDerivePort` defaults on; disable it to bind the fixed `HttpPort`, which defaults to 19880), bound port published to `Saved/PinWright/gateway-port` (stdio proxy follows it per call), timeouts 120s/300s, 1MB max request body
  - Security: loopback-only; bearer-token auth required by default (token at `Saved/PinWright/gateway-token`; `bRequireAuthToken` kill switch)
- `UPinWrightProjectSettings` (team-shared, `Config=Editor, defaultconfig` → persists to the host project's committed `Config/DefaultEditor.ini`) in Edit → Project Settings → Plugins → PinWright (Project): `AssetDumpRootDirectory`, `WikiOutputDirectory`
