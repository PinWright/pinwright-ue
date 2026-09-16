---
type: reference
summary: "Test category taxonomy for PinWright: 14 top-level buckets, placement rules, file layout (main module + per-sub-module test trees), registry-walking contract tests, fixture teardown, the scratch root and the suite-start blank world that keep the host repo clean, suite memory containment (in-process watermark reset + external Job Object cap), naming conventions, host-dependent fixture skip convention, sub-module enablement caveat, design rationale."
date: 2026-09-10
tags: [testing, taxonomy, bpir, infra, coverage, fixtures, memory, sub-modules, geometry, pwmodel]
---

# Test Organization

Reference for the `PinWright.*` automation test taxonomy. Use this to find the right category when adding a new test and to understand where existing tests live.

This page describes placement and naming rules. The live test count changes often; count current registrations (main module + integration sub-modules) with:

```powershell
(rg "IMPLEMENT_.*AUTOMATION_TEST|BEGIN_DEFINE_SPEC" "Source" -g "**/Private/Tests/**/*.{cpp,h}" | Measure-Object).Count
```

**4072 registrations, measured 2026-08-21** by that command. The previous figure was 4006 on
2026-08-20, after the required-param consolidation removed 532 per-verb tests and added 7 (see
Contract Walks below); the +66 since is one change wave -- MGIR material properties, the pwmodel
compiler and geometry op fixes, the RPC ergonomics pass, both suggestion rankers, capture aiming,
the mesh-orientation signals, and five tests that had never executed at all because their ids
were strict prefixes of other ids. It is the
static count; ~87 sit inside feature-availability `#if` guards, so the executed total is lower
— derive the expected suite total the way the Testing section of `CLAUDE.md` requires.

The per-bucket counts below are **directory** counts under `Private/Tests/<Bucket>/`, which is
what the command above measures and what File Placement below prescribes. They are not counts of
test-id prefixes: most ids are still the older lowercase `PinWright.<namespace>.<verb>.<Variant>`
form rather than `PinWright.<Bucket>.…`, and the tree has grown top-level test directories this
table does not list (see Undocumented Directories below).

---

## Top-Level Taxonomy

| Bucket | Sub-categories | Tests | What it covers |
|--------|---------------|-------:|----------------|
| `Assets` | Material, Niagara, VFX, Asset | 390 | RPC handlers that operate on content assets (materials, meshes, textures, Niagara systems, generic asset CRUD) |
| `World` | Actor, Level, Volume, Geometry, Environment, WorldInfo | 329 | RPC handlers that operate on the world: actors, levels, volumes, geometry, environment settings |
| `Gameplay` | AI, Character, Physics, Input, Interaction, Animation | 174 | RPC handlers for gameplay systems: AI, character setup, physics, input mappings, sequencer animation |
| `EditorOps` | Editor, Build, Debug, System, Systems, Render | 219 | RPC handlers for editor operations: build lighting, console commands, viewport control, render settings |
| `Media` | Audio, UI, Sequencer | 437 | RPC handlers for media assets and playback: audio, UMG widget operations, Sequencer |
| `Blueprint` | (top-level) | 164 | RPC handlers for Blueprint graph editing, compilation, decompilation, and inspection |
| `Networking` | (top-level) | 26 | RPC handlers for networking, replication, and multiplayer setup |
| `Utility` | (top-level) | 225 | RPC handlers in the `utility` domain (misc helpers not belonging to a themed group) |
| `Bpir` | Tokenizer, Parser, Expression, Compiler (incl. Compiler.Resolvers), Decompiler, RoundTrip, NodeLayout, GraphOrphan | 579 | BPIR compiler pipeline: lexing, parsing, compilation, decompilation, round-trip fidelity, node layout, orphan detection |
| `Infra` | Transport, Dispatcher, HttpApi, ToolCatalog, State, HandlerContext, AutoRegistration, Contract | 238 | Plugin infrastructure: HTTP transport, JSON-RPC dispatcher, tool catalog, plugin state, handler context, auto-registration, contract consistency |
| `Core` | Path, Json, Class, MakePinType, LevelSaveLoad, CodeFunctionResolver, error_codes, pwmodel_diagnostics, docs_schema | 114 | Internal utility libraries: path sanitization, JSON helpers, class resolution, pin type conversion, level save/load |
| `WidgetXml` | Export, Import, RoundTrip | 50 | Widget XML serialization format: export, import, and round-trip identity |
| `Geometry` | Ops, AssetCreate | 216 | The extracted `GeometryOps` layer that both the `geometry.*` handlers and the `.pwmodel` compiler call: op behaviour (`Geometry.Ops`) and single-build static-mesh creation (`Geometry.AssetCreate`). Lives in `Source/PinWrightGeometry/Private/Tests/Geometry/` |
| `Model` | Tokenizer, Parser, Collision, Compiler, Handlers, WidenedOps, Orientation, Sweep, Skin | 184 | `.pwmodel` source-format pipeline: lexing, parsing, collision and skin blocks, compilation to a `UStaticMesh` or `USkeletalMesh`, and the `model.*` handlers. `Model.WidenedOps` asserts the op table publishes each widened op's full engine vocabulary at the engine's defaults, so a narrowed parameter set fails here rather than shipping. Lives in `Source/PinWrightGeometry/Private/Tests/Model/` |

### Undocumented Directories

`Private/Tests/` also carries top-level directories the table above never listed, measured the
same way on 2026-08-20: `Drive` 121, `Render` 101, `Niagara` 65, `Material` 49, `Actor` 48,
`Transport` 44, `Sequencer` 43, `Spatial` 38, `PCG` 25, `Environment` 24, `Widget` 22,
`Recorder` 17, `WidgetAnimationJson` 17, `State` 15, `DataTable` 13, `Image` 13,
`SourceControl` 5, `UI` 5, `Reflection` 4, `Layout` 2, `GameplayTags` 1, plus 28 files directly
under `Tests/`. Several duplicate a documented bucket's sub-category (`Niagara`, `Material`,
`Actor`, `Environment`, `Sequencer` are sub-categories of `Assets`/`World`/`Media` in the table
but exist as siblings on disk). Fold a new file into a documented bucket rather than adding to
this list.

---

## Contract Walks

Some contracts hold for **every** registration, so they are asserted once by a loop over the
registry instead of per verb. A walk covers verbs that do not exist yet, which per-verb copies
structurally cannot: a new verb needs no new test, and the walk is what an author who forgets
one still answers to.

The six metadata walks in `Tests/Infra/TestContractConsistency.cpp` (`AllHandlersDocumented`,
`RequiredParamsDocumented`, `NoDuplicateMethodNames`, `RegistrationCountSanity`,
`MethodNameFormat`, `ParamTypes.ValidTypeNames`) read `FAutoRegisterHandler::GetPendingRegistrations()`
and check shape only.

`PinWright.infra.contract.RequiredParamGate.EveryVerb`, in the same file, is the one behavioural walk: for
every registration and every required `FParamSpec` on it, it drives a real `FRpcDispatcher` with a
payload that omits exactly that slot and asserts `MISSING_REQUIRED_PARAM` naming the slot in
quotes. It replaced 532 hand-written per-verb gate tests across 26 files. Measured 2026-08-20:
**1664 required slots across 958 verbs, 0 failures, 0 slots not independently probeable** — every
one of the 518 (verb, slot) pairs those tests asserted, plus 1146 more they never reached. Six
`RequiredParamGate.Mechanism.*` siblings pin the gate itself — one per declaration shape
(`RPC_PARAM_REQ`, `ParamAliasUtils::MakeAliasParamSpec`, the required-spec factories) plus the
alias and tick-unsafe-deferral interactions — because the walk reads `bRequired` and so cannot
notice a shape that stops setting it.

**Do not add a per-verb required-param test.** The walk already covers the verb. Write a per-verb
test only for a guard that lives in the handler *body*, which the dispatcher gate never reaches —
`PinWright.actor.select.MissingRequiredParam` and `PinWright.landscape.sculpt.MissingRequiredParam`
are the two such cases (both verbs declare every param optional and validate in-body), and both
are misnamed for what they assert.

---

## When to Choose Which Bucket

### RPC domain tests (handler tests)

Pick the theme that matches what the RPC *operates on*, not what module implements it:

- Asset operations (create material, import mesh, list assets) → `Assets.<SubCategory>`
- World/level operations (spawn actor, edit level, place volume) → `World.<SubCategory>`
- Gameplay systems (AI config, animation setup, input mapping) → `Gameplay.<SubCategory>`
- Editor operations (build lighting, console command, PIE) → `EditorOps.<SubCategory>`
- Audio, UMG, or Sequencer operations → `Media.<SubCategory>`
- Blueprint graph editing, compile, inspect → `Blueprint`
- Networking/replication setup → `Networking`
- Miscellaneous utility RPC domain → `Utility`

### BPIR pipeline tests

Tests that exercise the BPIR compiler, parser, decompiler, or round-trip belong under `Bpir.<Layer>.<Feature>`:

| Layer | Bucket |
|-------|--------|
| Lexer / tokenizer | `Bpir.Tokenizer` |
| Syntax parser | `Bpir.Parser` |
| `UK2Node_BpirExpression` node | `Bpir.Expression` |
| Compiler (graph emission) | `Bpir.Compiler` |
| Compiler resolver chain | `Bpir.Compiler.Resolvers` |
| Decompiler (graph → text) | `Bpir.Decompiler` |
| Compile → decompile → recompile | `Bpir.RoundTrip` |
| Post-compile node layout engine | `Bpir.NodeLayout` |
| Orphaned-node detection | `Bpir.GraphOrphan` |

### Infrastructure tests

Tests for the plugin's own machinery (not the RPCs it exposes):

- HTTP transport, JSON-RPC protocol, dispatcher, tool catalog → `Infra.<Component>`
- Plugin state, handler context, auto-registration, contract checks → `Infra.<Component>`
- Structural lint of the `docs/wiki-src/` overlay pages against the merger's rendering rules → `Infra.wiki_src`

### Internal utility library tests

Tests for shared utility modules (`Utils/` directory):

- Path sanitization (`PathUtils`) → `Core.Path`
- JSON field helpers (`JsonUtils`) → `Core.Json`
- Class resolution (`ClassUtils`) → `Core.Class`
- Pin type conversion (`MakePinType`) → `Core.MakePinType`
- Level save/load helpers → `Core.LevelSaveLoad`
- Code function resolver → `Core.CodeFunctionResolver`

**Doc-contract tests also live in `Core`,** despite testing no library: they scan plugin source as
text and fail when a hand-maintained document disagrees with it. Both live in the always-loaded
main module and discover their scan roots by glob over `Source/PinWright*/`, so the optional gated
sub-modules stay covered — hosting one beside the code it scans would stop it running on exactly
the configurations nobody watches.

- `ERR_*` emit sites vs `Handlers/ErrorCodes.h` → `Core.error_codes`
- `PWMODEL_*` emit sites vs the diagnostics table in `docs/pwmodel-format.md` → `Core.pwmodel_diagnostics`
- Every maintainer doc reachable from `docs/index.md` and `docs/tags.md` → `Core.docs_schema`

### Widget XML serialization tests

Tests for the widget XML export/import serialization format (not UI widget RPC operations):

- Export → `WidgetXml.Export`
- Import → `WidgetXml.Import`
- Round-trip → `WidgetXml.RoundTrip`

---

## File Placement

Test files mirror the category path under:

```
Source/PinWright/Private/Tests/<TopLevelName>/
```

Tests for handlers owned by an integration sub-module live in that sub-module's own tree instead, with the same `<TopLevelName>/` layout (e.g. `Source/PinWrightGeometry/Private/Tests/Geometry/`, `Source/PinWrightPCG/Private/Tests/PCG/`, `Source/PinWrightChooser/Private/Tests/Assets/`, `Source/PinWrightPoseSearch/Private/Tests/Gameplay/`, `Source/PinWrightCommonUI/Private/Tests/UI/`): typed tests moved with their handlers in the v0.7.0 module split. The category strings are unchanged; only the file location follows the owning module.

Main-module tests compile into the main `PinWright` module DLL; sub-module tests compile into their sub-module DLL; there is no separate tests module. `WITH_DEV_AUTOMATION_TESTS` self-strips test registration in shipping builds, so no per-file guards are needed. Test-only helpers shared between cpps in the same subfolder follow the named-namespace inline header pattern (see `Tests/Assets/AssetDumpTestHelpers.h`, `Tests/Media/LiveUiSnapshotTestHelpers.h`, `Tests/Bpir/BpirGraphTestHelpers.h`, etc.). Sub-module test cpps still include the shared `Tests/TestUtils.h` through the `PrivateIncludePaths` entry pointing at the main module's `Private/` dir.

Examples:

| Category | File location |
|----------|---------------|
| `Assets.Material.*` | `Private/Tests/Assets/TestMaterialHandlers.cpp` |
| `Assets.Niagara.*` | `Private/Tests/Assets/TestNiagaraHandlers.cpp`, `Private/Tests/Assets/TestNiagaraDumpBuilder.cpp` |
| `World.Actor.*` | `Private/Tests/World/TestActorHandlers.cpp` |
| `Bpir.Compiler.*` | `Private/Tests/Bpir/TestCompilerIntegration.cpp` |
| `Bpir.Compiler.Resolvers.*` | `Private/Tests/Bpir/TestCompilerResolvers.cpp` |
| `Infra.RequestCore.*` | `Private/Tests/Infra/TestMcpRequestCore.cpp` |
| `Core.Path.*` | `Private/Tests/Core/TestPathUtils.cpp` |
| `WidgetXml.RoundTrip.*` | `Private/Tests/WidgetXml/TestWidgetXmlRoundTrip.cpp` |
| `Geometry.Ops.*` | `Source/PinWrightGeometry/Private/Tests/Geometry/TestGeometryOpsPrimitives.cpp` |
| `Model.Parser.*` | `Source/PinWrightGeometry/Private/Tests/Model/TestPwModelParser.cpp` |

The macro second argument must match the full category path:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMyTest, "PinWright.Bpir.Compiler.MyFeature", ...)
```

---

## Naming Conventions

The trailing variant (the final `.Segment` after the category path) uses ad-hoc conventions that have evolved across the test suite. Common patterns:

| Pattern | Example | Usage |
|---------|---------|-------|
| `.MissingRequiredParam(s)` | `Infra.HandlerContext.MissingRequiredParams` | Validates that missing required params return the correct error. **Retired for RPC verbs** — the dispatcher gate is covered for every verb by `Infra.contract.RequiredParamGate.EveryVerb` (see Contract Walks). Use this suffix only for a guard in the handler body |
| `.ValidParamsNoCrash` | `World.Actor.SpawnActor.ValidParamsNoCrash` | Smoke-level: valid params don't crash |
| `.Missing<SpecificParam>` | `Assets.Material.CreateMaterial.MissingAssetPath` | Missing a named required param |
| `.With<Qualifier>` | `Bpir.Compiler.CastNode.WithInterface` | Feature variant with a qualifier |
| `<FeatureName>` | `Bpir.Parser.ParseBranch` | Simple feature name for parser/compiler unit tests |

A follow-up effort will standardize these into a consistent scheme across all buckets.

**Adding a `.Something` suffix under an id that is already a complete test id kills that test.**
That is the direction people miss, and it is how this defect keeps reproducing. Checking that
*your* new id is not a prefix of an existing one is only half the check — the other half is that
your new id must not dot-extend an existing one either. Both halves are the same rule:

**Every test id must be a leaf.** An id that is a dot-prefix of another test's id is silently
dropped from the automation queue: the controller turns the shared prefix into a branch node, and
the test that wanted to be that node produces no result of any kind — no pass, no fail, no skip,
no warning. It is *absent* from the `<N> tests performed` count rather than failing it, so the
suite total simply comes back one lower and nothing points at the cause. Only a `.` collides;
`create_node` vs `create_node_guard` is a plain string prefix that never reaches the engine's
split, and both members run. Measured 2026-08-20 while adding the required-param walk: registering
it as `PinWright.infra.contract.RequiredParamGate` alongside six
`PinWright.infra.contract.RequiredParamGate.Mechanism.*` leaves ran 12 tests instead of 13, with
the walk missing. Renaming it `…RequiredParamGate.EveryVerb` fixed it. Recurred 2026-08-28 in the
reverse direction: a new `…niagara.graph.create_node.RefusalLeavesPackageClean` landed under the
existing leaf `…niagara.graph.create_node` and killed it.

Two checks enforce this, and neither subsumes the other:

- **Runtime**, `PinWright.infra.automation_registry.NoPrefixCollisions`
  (`Source/PinWright/Private/Tests/Infra/TestAutomationTestIdPrefixCollisions.cpp`) walks
  `FAutomationTestFramework::GetValidTestNames`, so it sees exactly what this host would run —
  and therefore never sees an id inside a `#if` this host compiles out, nor any id of an
  integration sub-module whose engine plugin is disabled here.
- **Static**, `Content/Python/check_test_ids.py` parses the id literals straight out of source. It
  needs no editor, no engine and no build, runs as its own CI job, and *does* see conditionally
  compiled and sub-module ids because it deliberately does not evaluate the preprocessor. The
  price is the opposite error: it can flag a pair that no single build configuration would
  register together. Run it directly with
  `"%UE_ROOT%\Engine\Binaries\ThirdParty\Python3\Win64\python.exe" -m check_test_ids` from
  `Content/Python`; its own fixture pair is `tests/test_test_id_prefix_scan.py`.

---

## Host-Dependent Fixtures

Some tests load fixture content by absolute `/Game/` path (the Lyra mannequin AnimBP `ABP_Manny`, its Skeleton, the `CR_Mannequin_Body` Control Rig) that only certain host projects ship. The plugin runs inside arbitrary hosts, so **host absence must skip, not fail**:

- **Gate before loading** with `PINWRIGHT_SKIP_IF_FIXTURE_MISSING(<path constant>)` from `Tests/TestUtils.h`. It runs `FPackageName::DoesPackageExist` on the fixture's package (object-path suffixes are stripped) and, when the package is absent, routes `FIXTURE-SKIP: ...` through `PinWrightTestSkip::SkipAssertions` and passes the test early. The macro used to call `AddInfo` directly: the note reached the log (Info entries are logged at `AutomationControllerManager.cpp:1685-1693`) but carried no `PINWRIGHT_ASSERTIONS_SKIPPED` marker, so `check_suite_log.py` counted nothing and a host missing every mannequin fixture classified `COMPLETED_CLEAN` having measured nothing. Fixing the two macro bodies made all 47 call sites countable at once.
- **Candidate lists**: a test that probes a LIST of alternative fixtures (e.g. any of several mannequin montages) gates with `PINWRIGHT_SKIP_IF_ALL_FIXTURES_MISSING(<TArray<FString> of the same candidate paths it loads>)` — it skips only when NONE of the candidates' packages exist; if at least one exists, the test proceeds and a load failure stays fatal.
- **Load failure stays fatal.** If the package exists but `LoadObject` (or a dump/handler equivalent) fails, keep the hard failure — that is a real regression signal, not a host difference. (`asset.dump`-driven tests branch on `ASSET_FILE_MISSING` — the handler's own pre-load existence code — for the skip; all other error codes stay fatal.)
- **`FIXTURE-SKIP:` is the audit token, and it does reach the log.** Every skip message starts with it so runs can be audited with a single grep; the same token covers inverted gates (e.g. a test of the no-CommonGame path skipping on Lyra-fork hosts where CommonGame is present). Measured: an archived run log carries 36 real `LogAutomationController: FIXTURE-SKIP:` lines. The token is not a substitute for the wire marker — it is greppable by a human, while `PINWRIGHT_ASSERTIONS_SKIPPED` is what the verdict counts — so keep both, which is what routing the macro through the emitter does.
- **Never gate tests that create their own transient fixtures** — only tests loading host-project content by absolute `/Game/` path.
- Reuse the path constant the test already loads with; don't hardcode a second copy of the fixture path for the gate.

## Fixture Teardown

Use `CleanupTestAsset` for a fixture package path, including a fixture that was saved to disk. For a loaded fixture it detaches the asset into the transient package without GC and moves an assetless source package there too; when a `.uasset` exists, it removes the file and updates registry state without loading an absent package. It can return before package cleanup for the editor-world asset/package or when no package file is present, so callers must not treat it as an unconditional registry/package purge.

For a confirmed on-disk package, `CleanupTestAsset` calls `ResetLoaders` after the file probe and before deletion; `MCP_REN_NO_RESET_LOADERS` otherwise leaves the saved linker open and a same-path save can fail with `ERROR_SHARING_VIOLATION`.

Tests must not depend on cleanup collecting the object or nulling references to it. Periodic suite GC reclaims detached transient objects. Use `PwTestAssetTeardown::DiscardCreatedAssetByObjectPath` only for a freshly-created, never-saved in-memory object path when the caller explicitly needs immediate GC; it does not delete a saved `.uasset`.

### The scratch root, and the two rules that keep the host repo clean

`/Game/PinWrightTests` is the suite-wide fixture scratch root, spelled once as `PinWrightSuiteMaintenance::ScratchRootPackagePath()` (`Tests/AutomationSuiteMaintenance.h`). Per-asset teardown removes files one at a time and never the directories they sat in, and gives up silently when a linker is still attached, so the root accumulates residue that shows up in the *host project's* `git status`.

Two structural rules, both enforced by tests rather than by discipline:

1. **A test may never save a shipped host package.** `editor.save_all` takes no arguments and has no dry-run or package filter — it flushes every dirty package in the editor, including the host's open startup map. Any test that invokes a save-everything verb wraps the call in `PinWrightSuiteMaintenance::FScopedForeignDirtyPackageSuspension`, which clears the dirty flag of every dirty package outside the scratch root for the scope and restores it afterwards, so the verb sees a fixture-only dirty set and a real unsaved host edit is neither written nor discarded. Ratcheted by `PinWright.infra.contract.SuiteMaintenance.ForeignDirtyPackagesSuspendedAndRestored`. A test that dirties the editor world still wraps it in `FScopedEditorWorldActorGuard` for the same reason it always did.
2. **The scratch root does not survive the run.** `PinWrightSuiteMaintenance::SweepScratchRoot()` routes every `.uasset` under the root through `CleanupTestAsset`, deletes whatever is left directly, then removes the emptied directories deepest-first. A `.umap` gets `ResetLoaders` on its package **first**: `CleanupTestAsset` resolves filenames with `GetAssetPackageExtension()` only, so for a map it probes a `<pkg>.uasset` that does not exist and returns before both `ResetLoaders` and the delete — while still having renamed the world into `/Transient`, which is why the linker must be detached before that rename, not after. Without it the direct delete runs against an attached map linker and fails with `ERROR_SHARING_VIOLATION`.

   `PinWright.zz_suite_end.ScratchRootIsEmptyOnDisk` runs the sweep at the end of the suite and fails on a survivor; its `zz_` first segment is what places it after every other `PinWright.*` test, since the controller executes leaves in sorted display-name order (`FString::operator<` is `Stricmp`, so the ordering is case-insensitive and `zz_` beats every uppercase namespace too). **Only that gate may sweep the whole root.** A sweep run mid-suite detaches and deletes fixtures whose owning tests are not finished with them — by the time `PinWright.infra.*` runs, `PinWright.editor.*` and everything before it have already put fixtures on disk there — so any other caller passes `SweepScratchRoot`'s `SubDirectory` argument and sweeps only its own subtree. A scoped run that filters the gate's id out does not sweep at all.

### The suite-start blank world

Both rules above are opt-in per call site: a test that dirties the editor world without reaching for `FScopedEditorWorldActorGuard` still leaves the host's startup map modified. `PinWright.aa_suite_start.OpenBlankTransientWorld` (`Private/Tests/Infra/TestSuiteStartBlankWorld.cpp`) removes the target instead of guarding each path to it. It records the world the editor started on, runs the shared pre-swap survivor probe, calls `GEditor->NewMap(false)`, and asserts the resulting editor world's package is untitled or transient and carries `PKG_NewlyCreated`. Everything after it therefore runs against a world whose package is `/Temp/Untitled_<N>` — a **read-only** mount rooted at `<Project>/Saved` (`PackageName.cpp:904`), with no file on disk — so an unguarded world edit has nothing host-owned to reach.

Three things about it are load-bearing:

- **`aa_` is the ordering, mirroring `zz_`.** The controller sorts the whole batch by display name (`AutomationControllerManager.cpp:1042-1051`) with a case-insensitive compare, and the earliest other second segment in the tree is `actor`, so `aa_` runs first. Move the id into a later namespace and the swap still happens — after the tests it was meant to protect.
- **`GEditor->NewMap`, never `CreateNewMapForEditing`.** `NewMap` (`EditorServer.cpp:2187`) creates the world and nothing else; `CreateNewMapForEditing` (`:2145`) wraps it in `FEditorFileUtils::SaveDirtyPackages(bPromptUserToSave, …)` at `:2158`, and under `-unattended` that modal is auto-answered rather than declined — the prompting form could write the host's startup map on its way out. `NewMap` is also what `level.create` and `lighting.create_lighting_enabled_level` already call.
- **The pre-swap probe is not optional.** `NewMap` reaches `EditorDestroyWorld` → `Cleanse` → `CheckForWorldGCLeaks`, Fatal by default on any dead world still resident, so the step runs `PinWrightMapSwapGuard::ProbeResidentWorldSurvivors` first and **declines** the swap through `PINWRIGHT_ASSERTIONS_SKIPPED` rather than handing the engine a swap the guard would refuse. Its other skip is the absence of an editor world (a commandlet host).

**A test that needs a real on-disk map must discover one, never read it off the ambient world and never hardcode it.** `FindAlternateOnDiskMap(ExcludePackage)` (`Private/Tests/TestWorldUtils.h`) walks the asset registry for the first `World` asset with a `.umap` on disk; it is the single source for every such fixture. Reading the ambient world's package name now yields `/Temp/Untitled_<N>`, which `FPackageName::DoesPackageExist` rejects — a test built that way either skips (and a skip marker downgrades the whole run to `COMPLETED_WITH_SKIPS`) or errors. A hardcoded path is worse: it is a fixture that exists on the host it was written against and nowhere else.

Same limitation as the suite-end gate: a **scoped** run whose filter does not select `PinWright.aa_suite_start` never performs the swap, so it runs on the host's startup map and the per-call-site guards are all that stand between it and host content. Both `FScopedEditorWorldActorGuard` and `FScopedForeignDirtyPackageSuspension` therefore stay mandatory; the blank world is a second layer, not a replacement.

Consequence for `FScopedEditorWorldMapGuard`: the map it snapshots is now untitled, so `FPackageName::DoesPackageExist` on it is false and the `level.load` restore cannot fire. Doing nothing there is not "leave it as found" — the maps those tests swap in are throwaway probes their own teardown deletes moments later, which would leave the editor world context on a destroyed world. The guard therefore falls back to opening an equivalent blank world (same `NewMap`, same probe) when the original has no package on disk.

---

## Suite Memory Containment

Deferred reclamation is what makes the suite fast and what makes it able to run out of memory. Two
independent guards, one inside the process and one outside it.

**Inside — `PinWrightSuiteMaintenance` (`Private/Tests/AutomationSuiteMaintenance.{h,cpp}`).** The
`OnTestEndEvent` hook counts completed `PinWright.*` tests and runs a reset (drain the task graph,
finish sound/shader compilation, `ResetTransaction`, `CollectGarbage`) when either trigger fires:

| Trigger | Knob | Resolution order | Default |
|---------|------|------------------|---------|
| count | tests since the last reset | scoped override → `pinwright.TestGcEvery` → `-PinWrightTestGcEvery=N` → `TestSuiteResetIntervalTests` | 25 |
| watermark | working set ÷ physical RAM | scoped override → `pinwright.TestMemoryWatermark` → `-PinWrightTestMemoryWatermark=F` → `TestSuiteResetMemoryWatermark` | 0.55 |

Both go through `AssetDumpHandler::ShouldRunDumpReleaseStep` — the same predicate the
`asset.dump_folder` sweep schedules its release step with, so "this long-running loop has
accumulated enough to be worth reclaiming" has one rule. Consequence to know: that predicate's
`DumpReleaseMinAssetsBetweenSteps` floor is 25, so at the default interval of 25 the watermark can
never fire *earlier* than the count. It earns its keep when the interval is raised or disabled.

Every reset logs at **Display** with its trigger and before/after GiB, so a run is auditable:

```
LogPinWrightSuiteMaintenance: Display: PinWright suite maintenance reset 12 (trigger=count) after
test 300: usedPhysical 21.40 -> 18.02 GiB of 63.20 GiB (freed 3.38 GiB).
```

When the working set is still at or above `TestSuiteMemoryHardFraction` (default 0.75) *after* that
collect, the reset did not work, and the run emits the greppable token
`PINWRIGHT_MEMORY_WATERMARK_EXCEEDED` at Error naming the last completed test id. **It is a log
marker, never an `AddError`.** The hook runs inside `FAutomationTestFramework::InternalStopTest`
*after* the just-finished test's success state is frozen (`AutomationTest.cpp:1384`) but *before*
the automation output device is detached (`:1407`), so the line lands as an Error event on that
test's report yet cannot turn its `Result={Success}` into a failure. A test that deliberately
triggers the escalation must `AddExpectedError` it, because inside a *running* test the same
capture would fail it.

**Outside — `scripts/Run-SuiteCapped.ps1`.** Builds the identical suite argv and runs the editor in
a Windows Job Object with `JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
JOB_OBJECT_LIMIT_PRIORITY_CLASS` at `-MemoryFraction` (default 0.60) of physical RAM and
`-PriorityClass` (default `BelowNormal`). Two reasons the memory half is not a watchdog: UE reads the
job limit at startup into `MemoryConstants.TotalVirtual`
(`WindowsPlatformMemory.cpp:449-486`, `Detected a per-process memory limit of %.1fGB for this
job.`), so `FAssetCompilingManager` throttles against the cap; and a hard per-process limit makes
allocations *fail* rather than killing the process, so the failure is a diagnosable OOM at a known
bound rather than a host that pages itself to a standstill. Per-process, not job-wide, so
`ShaderCompileWorker` children are not charged to the editor's budget.

The priority half covers those same children: the job's priority class applies to every process in
the job and a job process cannot raise itself above it, and nothing in this path passes
`CREATE_BREAKAWAY_FROM_JOB`, so `ShaderCompileWorker` here — and UBT's `dotnet`, `cl.exe` and
`link.exe` under `Run-Capped.ps1` — inherit it. `BelowNormal` and not `Idle`: an Idle job is starved
whenever anything else on the box wants CPU, which inflates wall time, while BelowNormal yields to
interactive work without starving. **This launcher is how compiles, suite runs and any
automated/unattended editor launch are started; an interactive editor session the user is working in
stays uncapped and at normal priority.**

```powershell
& "<PLUGIN_ROOT>\scripts\Run-SuiteCapped.ps1" `
  -HostProject "<HOST>\<HostProject>.uproject" -EngineRoot "%UE_ROOT%" -Filter PinWright `
  -LogPath "<HOST>\Saved\Logs\pw_suite.log" -MemoryFraction 0.60 -PriorityClass BelowNormal `
  -ExtraArgs @('-ddc=InstalledNoZenLocalFallback','-PinWrightTestGcEvery=25','-PinWrightTestMemoryWatermark=0.55')
```

It prints (and writes to `<LogPath>.result.txt`) one machine-readable line:

```
PINWRIGHT_SUITE_RESULT verdict=EDITOR_EXITED cap_gb=37.92 peak_gb=14.83 priority=BelowNormal exit=0 oom_alloc=0 oom_backup_pool=0 watermark_markers=0 capSeenByEditor=True wall_min=21 log=...
```

**`scripts/Run-Capped.ps1`** is the sibling for everything that is not the suite: it wraps an
arbitrary `-Command` (`Build.bat`, a commandlet, any `UnrealEditor-Cmd.exe` invocation) with
`-CommandArgs` in the same Job Object, redirects stdout+stderr to `-OutputPath`, takes the same
`-MemoryFraction` / `-PriorityClass` plus `-TimeoutMinutes` (default 120), and prints and writes
(to `-ResultPath`, default `<OutputPath>.result.txt`) one line
`PINWRIGHT_JOB_RESULT verdict=<COMMAND_EXITED|COMMAND_EXIT_NONZERO|MEMORY_CAP_HIT|TIMEOUT> exit=<n> priority=<...> cap_gb=<...> peak_gb=<...> wall_min=<n> command=<...> output=<...>`,
exiting 2 on `TIMEOUT` / `MEMORY_CAP_HIT` and otherwise with the command's own exit code. Both
drivers dot-source `scripts/CappedJob.ps1`, the shared Job Object interop.

`verdict=MEMORY_CAP_HIT` exits 2. The launcher does **not** classify the suite —
`check_suite_log.py` remains the verdict authority, and it gained two states for this:

- **`MEMORY_EXHAUSTED`**, keyed on `Ran out of memory allocating` /
  `from backup pool to handle out of memory` **and** a queue that did not drain. It outranks
  **both** `CRASHED` and `DID_NOT_COMPLETE`, which is the one non-obvious part and was earned by
  measurement: a real OOM logs those strings, then dies with a `Fatal error:` banner and a crash
  report whose type is literally `OutOfMemory`, so ranked below `CRASHED` the state would never
  fire. A deliberately capped run (`-MemoryFraction 0.05`, cap 3.16 GiB) produced exactly that
  and classified `CRASHED` before the reorder. Both older verdicts describe an OOM correctly and
  uselessly — "killed or wedged" sends the reader hunting a harness timeout, "the editor crashed"
  sends them to `Saved/Crashes` — and neither names the memory. The crash evidence is folded into
  the reason rather than discarded.
- **`COMPLETED_WITH_MEMORY_PRESSURE`**, ranked just above `COMPLETED_CLEAN`, on the watermark
  token *or* on an allocation failure the run survived (the backup pool exists to absorb one, and
  a run that drained measured everything — it was simply one allocation from the other state).

Both exit non-zero, and every verdict prints `memory: oomLines=N watermarkMarkers=N` on every run,
zeros included, for the same reason the crash line does: an unchecked axis reported as silence is
what let an OOM read as a truncation.

The `-PinWright*` switches are passed **explicitly at every call site**
(`.polyskill/skills/mcp-test-loop/SKILL.md`, `.polyskill/skills/mcp-version-matrix/mcp-version-matrix.workflow.js`)
rather than left to their settings defaults, so a reader of the launch line can see both halves of
the guard without opening `PinWrightSettings.cpp`.

---

## Conditional Skips Must Be Countable

A test that steps over its substantive assertions and returns `true` reports `Result={Success}`. In
the suite log that is byte-identical to a real pass — `started == succeeded`, drain marker present,
`skipped=0`, exit 0 — so a host that measured nothing classifies `COMPLETED_CLEAN`. **Every such
early return emits the marker**, through the one supported emitter:

```cpp
#include "Tests/TestSkipReporting.h"   // sub-modules resolve this via PrivateIncludePaths

if (!World)
{
    PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
        TEXT("No editor world available to place the probe in."));
    return true;
}
```

`check_suite_log.py` greps `PINWRIGHT_ASSERTIONS_SKIPPED:`, counts it, and classifies the run
`COMPLETED_WITH_SKIPS`. Never `AddInfo` (invisible to a log-based checker) and never `UE_LOG`
(`bElevateLogWarningsToErrors` turns it into a failure — `B-tests-host-dependent-fixtures-hard-fail`).
Reuse an existing reason slug rather than minting a near-duplicate; `no-editor-world`,
`fixture-unavailable`, `cvar-pinned` and `capture-unavailable` cover most cases. The detail string
carries the measurement that produced the decision.

Two checks enforce this, and neither subsumes the other:

- **Runtime**, `PinWright.infra.skip_marker.*`
  (`Source/PinWright/Private/Tests/Infra/TestSkipMarkerEmission.cpp`) pins the emitter's wire text
  and its countability — the emitter's contract, not the call sites' adoption.
- **Static**, `Content/Python/check_test_skips.py` flags any `AddWarning(...)` whose very next
  statement is `return true;`. This is the half that can see a guard which never fires: a call site
  that emits no marker emits nothing for a runtime check to observe, and most of these guards are
  near-dead on any host that can run the suite. It needs no editor, no engine and no build, runs as
  its own CI job (`test-skip-scan`), and is self-tested by
  `Content/Python/tests/test_warn_and_pass_scan.py`. Run it with
  `"%UE_ROOT%\Engine\Binaries\ThirdParty\Python3\Win64\python.exe" -m check_test_skips` from
  `Content/Python`.

**A warning that is not a skip** — the test ran all its assertions, then noted something it
measured — is waived at the site, with a reason, never in an allowlist file:

```cpp
    // PINWRIGHT_WARNING_IS_NOT_A_SKIP: the TestEqual above already checked the count; this
    // records the measured spread for the next reader.
```

A bare marker with no reason is rejected. **Known gap:** the scan covers `AddWarning` only, so the
`AddInfo`-and-return-true family (including `PINWRIGHT_SKIP_IF_FIXTURE_MISSING` above) is still
uncounted — see the board ticket `B-tests-warn-and-pass-without-skip-marker` for the census.

### `-unattended` never ships without `-RunningUnattendedScript`

Every editor launch in this repo — the suite run, the version-matrix launcher — pairs the two
switches, and the pairing is a gate rather than a convention.

`FSlateApplication::AddModalWindow` consults `GIsRunningUnattendedScript` and nothing else before
returning without showing the dialog (`SlateApplication.cpp:2134`; `IsUnattended` does not appear
anywhere in that file). Only `-RunningUnattendedScript` sets that global
(`LaunchEngineLoop.cpp:6857-6860`). `-unattended` sets `FApp::IsUnattended()`, which that path never
reads. So `-unattended` alone does not suppress Slate modals — it only removes the *native*
`MessageBoxExt` prompts — and one Slate modal raised during startup, before any test registers,
parks the game thread for the life of the process.

**The reason this needs a gate rather than a note is that the failure is unreadable.** The wedged
process sits at ~0% CPU with zero I/O, the log stops mid-startup with no error line, and under
`-RenderOffscreen` the modal never presents a window, so enumerating the process's top-level windows
finds nothing to dismiss. A cold CI editor wedged this way for 13 minutes; the investigation
correctly ruled out ZenServer, session 0 and shader compilation, and equally correctly concluded
"not a modal — nothing to dismiss", which was the one wrong turn available (board ticket
`B-cold-editor-run-tests-hangs-before-automation`).

Two checks, neither subsuming the other:

- **Unit**, `Content/Python/tests/test_mcp_proxy_editor_start.py` asserts the invariant over every
  argv `build_editor_command` can produce, and the flag list of `_editor_prepare_tests`' launch
  command. That covers the proxy's own tables and nothing else.
- **Static**, `Content/Python/check_unattended_flags.py` scans the `.ps1` / `.yml` / `.yaml` / `.js`
  command files for an argv-shaped `-unattended` without the pair switch. This is the half that
  reaches launches written *outside* the proxy — workflow steps, smoke drivers, skill launchers —
  which no unit test can see. Runs as its own CI job (`unattended-flag-scan`), self-tested by
  `Content/Python/tests/test_unattended_flag_scan.py`. Run it with
  `"%UE_ROOT%\Engine\Binaries\ThirdParty\Python3\Win64\python.exe" check_unattended_flags.py` from
  `Content/Python`.

Markdown and Python are outside the scan by design: both are full of prose naming `-unattended`
while explaining this defect, and telling an explanation from an invocation would need an opt-out at
every paragraph. **Consequence to know:** a launch recipe written only in a `.md` is unguarded —
`.polyskill/skills/mcp-version-matrix/SKILL.md` currently summarises its launcher without the pair
switch, while the `mcp-version-matrix.workflow.js` it defers to has it.

---

## Sub-Module Test Enablement

Sub-module test trees compile whenever the sub-module compiles, but they only **run** on hosts where the owning engine plugin is enabled: `IntegrationGates` loads a `LoadingPhase: None` sub-module at subsystem init only when `IPluginManager` reports its engine plugin enabled, and an unloaded module's automation tests are simply never registered. A host with a plugin disabled silently skips that integration's tests: no failure, no `FIXTURE-SKIP`, just missing registrations.

- A host project that enables every gated engine plugin runs the full suite; one with a plugin disabled runs a strict subset.
- When a run's test count looks low, grep the host log for the startup line `PinWright integrations: loaded=[...] skipped=[...]` (`LogPinWrightIntegrations`) before suspecting a regression.

---

## Design Notes

- **`Core` not `Utils`**: The internal utility libraries were originally prefixed `Utils.*`. That name was changed to `Core.*` to avoid collision with the RPC `Utility` domain, which covers the plugin's `utility.*` RPC methods.

- **`EditorOps` not `Editor`**: A top-level `Editor.*` bucket would produce the category string `PinWright.Editor.Editor.*` when sub-categorizing editor-domain handlers — an ugly stutter. `EditorOps` avoids it.

- **`WorldInfo` inside `World`**: Sub-categories inside `World.*` could not use `World` itself (`World.World.*` stutters). `WorldInfo` is used for world-settings-level tests.

- **`Bpir.Compiler.Resolvers` nesting**: Compiler resolver tests (`TestCompilerResolvers.cpp`) test components that are internal to the compiler's pin/function resolution chain. They sit under `Bpir.Compiler.Resolvers.*` rather than a standalone `Resolvers.*` top-level because they are tightly coupled to compiler internals, not a separate subsystem.

- **`WidgetXml` as a top-level**: Widget XML serialization is a distinct serialization format, not a UI widget RPC operation. Keeping it separate from `Media.UI.*` (which covers UMG widget manipulation RPCs) prevents confusion between the two concerns.

- **`Geometry` top-level next to `World.Geometry`**: `World.Geometry` is the RPC-domain bucket — geometry *handler* tests, which spawn or mutate a level actor. `Geometry.*` tests the extracted `GeometryOps` library, which has no world and no actor and is called by the `.pwmodel` compiler as well as by those handlers. Filing a library under the world bucket is the same mistake `Core` versus `Utility` avoids. Caveat: the handler tests themselves register **lowercase** ids of the form `PinWright.geometry.<verb>.*`, predating this taxonomy; they are the `World.Geometry` bucket in intent, not in id.

---

## See Also

- [bpir-test-matrix](bpir-test-matrix.md) — Feature × layer coverage matrix for all BPIR tests, known gaps
- [arch](arch.md) — Plugin architecture reference, testing architecture section
- [bpir-compiler-internals](bpir-compiler-internals.md) — Compiler pipeline internals
