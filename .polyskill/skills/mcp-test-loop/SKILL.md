---
name: mcp-test-loop
description: Drive the PinWright plugin to a clean compile + clean unit test pass through an iterative compile→fix→test→fix loop. Uses UnrealEditor-Cmd.exe (CLI) for both compile and tests, dispatches one fix subagent per error cluster (compile) or per failing test .cpp+.h file pair (tests). Use when the user says "fix the plugin tests", "run the test loop", "make the plugin tests pass", "mcp test loop", "compile and test the plugin", or asks to drive the pinwright plugin to a green state.
---

# MCP PinWright Test Loop

Drive the `PinWright` plugin to a green state — clean compile and all plugin unit tests passing — by iteratively compiling, parsing failures, dispatching fix subagents in parallel, and re-running until clean (or the stuck-detector trips after 10 cycles of zero progress on the same problem).

The orchestrator owns builds and test runs, groups failures, and dispatches source investigations to fix workers. It may inspect and fix obvious compile errors inline as described below. Fix workers do not launch competing builds or test runs.

## Hard rules

1. **Never pass `-NullRHI`** to the editor. Some tests need a real RHI (`feedback_no_null_rhi`).
2. **Use configured worker defaults.** Follow the environment's delegation policy and worker model/reasoning defaults unless the user requests an override.
3. **Never branch, never worktree.** Stay on the user's current branch (`feedback_no_branches_or_worktrees`).
4. **Never delete or stub a failing test to make it pass.** No `// COMMENT OUT` of the assertion, no `bSkipped = true`, no widening until it can't fail. If a test is fundamentally testing the wrong thing, the fix subagent SKIPs it with a written reason and the orchestrator surfaces it in the final report.
5. **Never silence the build.** No `#pragma warning(disable)`, no flipping `bWarningsAsErrors`, no commenting out problematic include guards. Compile errors get fixed at the source site or escalated.
6. **Skill repo ≠ project repo.** `PinWright` is a private git subrepo nested inside the host project's repo (its own `.git/`, ignored by the outer repo per `editor_automation_subrepo`). When the skill says "git diff against the invocation start" or otherwise inspects VCS state for a fix targeted at plugin source, run those commands inside `Plugins/PinWright/`, not the host project tree. Edits to plugin source show up only in the plugin's status; edits to host-project source (rare in this loop, but possible if a fix has to touch engine-glue code in the host's own `Source/`) show up only in the outer repo. Don't conflate them.
7. **Subagents must never compile or run Unreal or run tests.** Only the orchestrator runs `Build.bat` and `UnrealEditor-Cmd.exe`. A fix subagent's job is to read, analyze, and edit — it does NOT spawn its own build/test cycle to verify. This is non-negotiable: parallel Unreal invocations collide (UBT mutex, editor instance lock, asset DDC contention), a 3-minute editor cold start in every subagent multiplies wall time by N, and the orchestrator already runs a fresh compile+test cycle after each fix wave. The next cycle's results ARE the verification. Subagents that try to compile or run tests waste budget and produce nothing the orchestrator wouldn't see on the next cycle anyway. State this explicitly in every fix-subagent prompt: "Do not compile. Do not run UnrealEditor-Cmd.exe. Do not invoke any test runner. Make your edits and return — the orchestrator will compile and test."

## When to dispatch subagents vs. fix inline

The orchestrator's context is the constraint. Test failures pull in test bodies, production code under test, and per-test failure messages — that's a lot of reading per failure, and pulling it inline poisons the orchestrator's context for the rest of the run. Compile errors are smaller: a single error site is often a typo, a missing include, a renamed symbol — quick to read, quick to fix.

The split:

**Test failures: always dispatch subagents.** No inline fixing of test failures, ever, regardless of count. One subagent per test-cpp unit (a unit may bundle multiple failing tests + crash for the same file — see Phase 3c). Even a single failing test goes to a subagent, because investigating it correctly means reading the test, the production code path, and possibly engine docs — exactly the context the orchestrator needs to stay clear of.

**Compile errors: split by obviousness, group aggressively when trivial.**

- **Obvious errors** — one-line typos, missing semicolons, a renamed symbol the orchestrator can identify from the error message and a quick file read, dropped includes after a header rename, a `const` qualifier that needs to move. Fix inline. Reading the file once and applying the edit is faster than briefing a subagent.
- **Non-obvious errors** — anything where the cause isn't visible from the error line alone: template instantiation walls, linker errors that need to follow a symbol across modules, errors where multiple files touched the same API and the right fix isn't local, anything mentioning UE engine internals you'd need to research. Dispatch a subagent.
- **Many trivial errors at once** (say >5 obvious ones across several files, all the same kind — e.g. a header rename rippled through 12 files): don't fix inline (too much edit churn in the orchestrator's working memory) and don't dispatch one subagent per file (overhead). **Group them into one subagent** — or two if the affected files split cleanly into two themes — and pass the whole grouped error list verbatim. The grouping rule is per-error-pattern (e.g. "all `error C2065: 'OldName': undeclared identifier` errors from the rename") rather than per-file.

When in doubt for compile errors, dispatch. Test failures are never in doubt — always dispatch.

The protocol at `test-fix-protocol.md` and `compile-fix-protocol.md` applies whether the work is done by a subagent or by the orchestrator inline — the rules about not stubbing, not silencing, etc. don't change.

## Loop shape

```
Outer loop (no cycle cap — run as long as the loop makes progress):
  Phase 1 — Compile (inner loop, no cap — keep going while progress is made):
    1a: run compile
    1b: grep log, group by source file
    1c: if errors: dispatch one fix subagent per cluster → goto 1a
    1d: stuck check (see below)

  Phase 3 — Test run (inner loop, no cap — keep going while progress is made):
    3a: run tests
    3b: grep log, map failures + any mid-suite crash to source units
    3c: if failures or crash: dispatch one fix subagent per unit → goto Phase 1 (fixes may need recompile)
    3d: stuck check (see below)

If compile clean AND tests green at end of any outer cycle: done.
Otherwise keep iterating until the stuck check trips.
```

**No cycle cap. Drive the loop until it's actually green.** A real test loop will burn through many cycles — that is the expected mode of operation, not an emergency. Don't escalate just because the loop has run N times; escalate only when the loop is clearly not making forward progress.

### Stuck check (the only stopping condition besides green)

The loop is **stuck** when the same problem persists for **10 consecutive cycles** with no progress. Concretely:

- **Phase 1 (compile) stuck:** the *same set of compile error signatures* is present for 10 consecutive compile runs. "Signature" = `(error code, source file, line)` triple. If even one error is fixed or a new one appears, that counts as progress and resets the counter.
- **Phase 3 (test) stuck:** the *same set of failing test paths* (plus the same crash site, if any) is present for 10 consecutive test runs, with no test transitioning from fail→pass and no new tests appearing in the failure list. Strict-decrease in count is **not** required — a fix that swaps one failure for another also counts as progress (the loop is moving, even if not monotonically improving). What kills the loop is **stagnation**, not regression.

Track the per-phase signature set across cycles in working memory. On each new run:
1. Compute the current signature set.
2. If it's identical to the previous run's set → increment `stuck_count` for that phase.
3. Otherwise → reset `stuck_count` to 0.
4. When `stuck_count` reaches 10, stop and escalate with the persistent signature set and `git diff` since invocation start.

**Crash-cleared exemption (carries over from the old design):** when the prior cycle crashed mid-suite and this cycle did not, the test suite is now running a strictly larger set of tests — that's a major topology change, not stagnation. Reset `stuck_count` to 0 for the test phase on that transition. Without this exemption, fixing a crash that exposes 50 previously-unrun failures could look like a regression and false-trip the stuck detector.

**Why 10 and not 3?** Compile/test fixes in a large plugin can take several cycles to converge — a header rename rippled through 12 files might surface 3-4 waves of dependent errors before the dust settles, and a tricky test failure may need the subagent to try several approaches. Three cycles is too short to distinguish "in progress" from "stuck"; ten cycles of *literally the same* signature set is unambiguous stagnation. The loop is cheap (cycles run unattended); the human's time is expensive (escalations interrupt them). Bias toward letting the loop work rather than bailing early.

## Log paths

Use symbolic paths in prompts and notes, then resolve them at runtime. Do not hardcode a user's local checkout path in this skill.

- `<PROJECT_ROOT>`: the host UE project's root, the directory holding its `.uproject`.
- `<PLUGIN_ROOT>`: `<PROJECT_ROOT>/Plugins/PinWright`.
- `<SKILL_ROOT>`: `<PLUGIN_ROOT>/.claude/skills/mcp-test-loop`.
- `<UE_ROOT>`: Unreal Engine install root containing `Engine/Build/BatchFiles/Build.bat`.
- `<UBT_LOG>`: `<PROJECT_ROOT>/Saved/Logs/ubt.log` — a per-project log the build writes via `-Log=` (below). NOT UBT's machine-global default (`$env:LOCALAPPDATA/UnrealBuildTool/Log.txt`): that one file is shared by every engine version and project, so a concurrent build elsewhere on the machine collides on it (UBT dies in ~0.5s backing it up, before `-WaitMutex` is even acquired).
- `<BUILD_STDOUT>`: `<PROJECT_ROOT>/Saved/Logs/pw-build-stdout.log` — the compile launcher's captured stdout+stderr (its `-OutputPath`). Distinct from `<UBT_LOG>`: it holds what Build.bat *printed*, including the C# rules-compiler errors that never reach UBT's own log.
- `<HOST_LOG>`: `<PROJECT_ROOT>/Saved/Logs/<HostProject>.log` - the editor log, named after the host project.

Use the tools' default log locations, with ONE exception: redirect UBT's log via `-Log=<UBT_LOG>` to a per-project path (its default is machine-global and collides with any concurrent build — see `<UBT_LOG>` above). The UE editor log (`<HOST_LOG>`) needs no redirect — it already lives under this project's `Saved`. UBT and UE both rotate/overwrite these on each run, so the latest file is always the current cycle.

State the orchestrator needs (current error list, the prior cycle's signature set, and the `stuck_count` per phase for the stuck check) lives in its working memory; persisting it to JSON or markdown files is unused ceremony. The signature sets are small (a few dozen tuples at most) — keep them inline.

## Search command portability

The `Grep` blocks below are Claude tool specs. In Codex, use `rg` or PowerShell equivalents with the same regex, context, and count semantics:

- Content with line numbers: `rg -n "<pattern>" "<path>"`
- Before-context: `rg -n -B 5 "<pattern>" "<path>"`
- Count matches: `(rg "<pattern>" "<path>" | Measure-Object).Count`
- If `rg` is unavailable, use `Select-String -Path "<path>" -Pattern "<pattern>"` and add `-Context <before>,<after>` when needed.

## Phase 1: Compile

### 1a. Run the build

Pre-flight: Claude shell can use `tasklist | grep -E "UnrealBuildTool|cl\.exe|link\.exe"`. Codex/PowerShell should use `Get-Process UnrealBuildTool,cl,link -ErrorAction SilentlyContinue`. If anything is left over from a prior aborted run and isn't part of the current invocation, `taskkill //f //pid <pid>`. Don't blanket-kill.

```powershell
& "<PLUGIN_ROOT>\scripts\Run-Capped.ps1" `
  -Command "<UE_ROOT>\Engine\Build\BatchFiles\Build.bat" `
  -CommandArgs @('<HostProject>Editor','Win64','Development','-Project=<PROJECT_ROOT>\<HostProject>.uproject','-Log=<UBT_LOG>','-WaitMutex','-FromMSBuild') `
  -OutputPath "<BUILD_STDOUT>" `
  -PriorityClass BelowNormal
```

Both the compile and the suite run at `BelowNormal` so the machine stays usable while the loop grinds, and both get it from the Job Object rather than from `SetPriorityClass`/`start /low` on the launcher: `JOB_OBJECT_LIMIT_PRIORITY_CLASS` applies to every process *in* the job — dotnet UnrealBuildTool, cl.exe and link.exe here, ShaderCompileWorker under the suite — and a process inside the job cannot raise itself back above it, which setting the parent's priority alone does not achieve. Each array element must stay single-quoted: unquoted, PowerShell splits an argument like `-Log=<UBT_LOG>` on its own punctuation.

Run with `run_in_background: true`. Wait for the completion notification — do not stream output into context. UBT still writes its own log to `<UBT_LOG>` (the `-Log=` path) and the launcher captures Build.bat's stdout+stderr to `<BUILD_STDOUT>`; the background-task notification gives us the exit code. The launcher passes Build.bat's exit code straight through except for its own `2` (TIMEOUT or MEMORY_CAP_HIT — the `PINWRIGHT_JOB_RESULT` line printed and written to `<BUILD_STDOUT>.result.txt` says which, and neither is a compile error to dispatch against).

**Blind spot — `.Build.cs` rules-compiler errors.** UBT's `<UBT_LOG>` only captures cl.exe / link.exe output. C# rules-compiler errors (e.g. `error CS0136`) emitted before UBT proper takes over never reach `<UBT_LOG>` — they go to stdout, which is now captured in `<BUILD_STDOUT>`. If the build exits non-zero AND the grep below produces zero matches, re-run the same grep against `<BUILD_STDOUT>` (add `error CS\d+` to the pattern). Escalate to the user with the exit code only if that is empty too.

### 1b. Parse the log

```
Grep
  pattern: "(error C\d+|error LNK\d+|: error :|fatal error)"
  path:    "<UBT_LOG>"
  output_mode: "content"
  -n: true
  head_limit: 0
```

Codex equivalent:

```powershell
rg -n "(error C\d+|error LNK\d+|: error :|fatal error)" "<UBT_LOG>"
```

Each hit looks like ` Compile [x64] <file.cpp>: <source-path>(<line>,<col>): error CXXXX: ...` (UBT prefixes every cl.exe stderr line with ` Compile [x64] <file>: `). Strip the prefix, extract the source path before the first `(`, normalize it relative to `<PROJECT_ROOT>` or `<PLUGIN_ROOT>` when possible, and group hits by source file. Two files are a **cluster** iff they're a same-name `.cpp` + `.h` in the same directory; otherwise singletons. Cluster pairs because edits to one usually require edits to the other.

### 1c. Dispatch compile-fix subagents

One subagent per cluster, all in the same tool turn (parallel). Prompt:

> Fix the following compile errors in `<cpp-path>`<and `<header-path>` if paired>:
>
> ```
> <error lines verbatim from the grep result, one per line>
> ```
>
> Compile log (read only the surrounding lines you need): `<UBT_LOG>`
> Protocol: `<SKILL_ROOT>/compile-fix-protocol.md`
> Cycle: `<N>`. Report in three bullets when done.

Foreground (no `run_in_background`); compile fixes are fast and we need all of them before the next compile. Wait for all to return.

### 1d. Loop back

Re-run compile. Clean → Phase 3. Still red → compute the current compile signature set (`{(error_code, file, line)}`), compare to the previous compile run's set:

- **Sets differ in any way** (any error fixed, any new error appeared) → reset `compile_stuck_count = 0`, repeat 1b–1c.
- **Sets identical** → increment `compile_stuck_count`. If it reaches 10, escalate: emit the persistent error list and `git diff` against the invocation start (run inside `Plugins/PinWright/` — the subrepo — for plugin edits; the host project for any glue-code edits), stop the skill. Otherwise repeat 1b–1c.

Don't bail before 10 identical-signature cycles. Many real fixes take 2-4 cycles to converge (cascading header renames, dependent errors that only surface once the upstream is fixed) — early bailouts here are the most common false-escalation mode.

## Phase 3: Test run

### 3a. Run the tests

```powershell
& "<PLUGIN_ROOT>\scripts\Run-SuiteCapped.ps1" `
  -HostProject "<PROJECT_ROOT>\<HostProject>.uproject" `
  -EngineRoot "<UE_ROOT>" `
  -Filter PinWright `
  -LogPath "<HOST_LOG>" `
  -MemoryFraction 0.60 `
  -PriorityClass BelowNormal `
  -ExtraArgs @('-ddc=InstalledNoZenLocalFallback','-PinWrightTestGcEvery=25','-PinWrightTestMemoryWatermark=0.55')
```

The launcher builds the same argv the suite has always used (`-ExecCmds="Automation RunTests PinWright,Quit"`, `-TestExit="Automation Test Queue Empty"`, `-unattended -nopause -nosplash -nosound -RenderOffscreen -nocefaccelpaint -RunningUnattendedScript -Abslog=<HOST_LOG>`) and adds one thing: the editor runs inside a Windows Job Object with `JOB_OBJECT_LIMIT_PROCESS_MEMORY` set to `-MemoryFraction` of physical RAM. **Why it is not a watchdog:** UE reads that limit at startup (`WindowsPlatformMemory.cpp:449-486`, `Detected a per-process memory limit of %.1fGB for this job.`) into `MemoryConstants.TotalVirtual`, so `FAssetCompilingManager` throttles against the capped figure instead of against the whole host. The limit is per-process, not job-wide, so `ShaderCompileWorker` children get their own ceiling rather than being charged to the editor's. A cap hit makes allocations fail rather than killing the process, so the run ends at a known bound with `Ran out of memory allocating` in the log instead of paging the host to a standstill.

The two `-PinWright*` switches are passed explicitly, at the call site, rather than left to their settings defaults, because they are the in-process half of the same guard: a reset every 25 tests, plus a forced reset once the working set reaches 0.55 of RAM — under the 0.60 cap, so the reclaim runs before an allocation can fail.

It prints one machine-readable line and also writes it to `<HOST_LOG>.result.txt`:

```
PINWRIGHT_SUITE_RESULT verdict=EDITOR_EXITED cap_gb=37.92 peak_gb=14.83 priority=BelowNormal exit=0 oom_alloc=0 oom_backup_pool=0 watermark_markers=0 wall_min=21 log=...
```

`verdict=MEMORY_CAP_HIT` (exit 2) means the run walked into the wall — quote `peak_gb` against `cap_gb` and the two OOM counts, and do not read the test numbers as a result. The launcher does **not** classify the suite; step 3b still owns that.

`-ddc=InstalledNoZenLocalFallback` selects a cache graph without the ZenLocal store. Mounting that store is the only thing at editor startup that builds an autolaunching `FZenServiceInstance`, and on a host with no zenserver already running its spawn-and-wait-for-health loop cost **23.5 s** before the first test (`Local ZenServer AutoLaunch initialization completed in 23.468 seconds`). Do **not** reach for `-NoZenAutoLaunch` instead: it keeps the store and repoints it at `[::1]:8558`, so with nothing listening every request fails and the `Failed to connect to localhost port 8558` spam starves the automation tick — the failure mode the completion gate exists to catch. The filesystem store still serves and still takes every put (the two are written together), so the hit rate is unchanged, and `%ENGINEVERSIONAGNOSTICUSERDIR%` resolves to the engine dir on a source build, so the `Installed` spelling is the portable one.

`-nocefaccelpaint` is required (UE 5.7+): a host project that embeds a CEF web browser widget brings one up, and under `-unattended` the editor's RHI doesn't support CEF's GPU-accelerated shared-texture paint. Without the flag, CEF's async `OnAcceleratedPaint` (`CopySharedTextureSync` → `BUseSupportedRHIRenderer()`) asserts from the WebBrowserSingleton ticker and kills the suite mid-run at a random test — it looks like a mid-suite crash but is environmental, not a plugin/test bug. The switch forces CEF software paint. Do **not** substitute `-NullRHI` (rule 1: some tests need a real RHI).

Background, wait for editor exit before parsing. Don't tail or stream the log — let the process write to `<HOST_LOG>`, then analyze. UE rotates the previous run to `<HostProject>-backup-<timestamp>.log` automatically, so `<HostProject>.log` is always the current run.

The `PinWright` prefix is the plugin suite root. Don't try to run tests individually; the editor cold-start cost per invocation makes a focused re-run slower than a full re-run.

### 3b. Classify the run BEFORE reading anything out of it

Run the checker first, every cycle. It is not an optional confirmation step:

```powershell
& "<UE_ROOT>\Engine\Binaries\ThirdParty\Python3\Win64\python.exe" `
  "<PLUGIN_ROOT>\Content\Python\check_suite_log.py" "<HOST_LOG>"
```

It prints one of `COMPLETED_CLEAN`, `COMPLETED_WITH_MEMORY_PRESSURE`, `COMPLETED_WITH_SKIPS`, `COMPLETED_WITH_FAILURES`, `NO_TESTS`, `DID_NOT_COMPLETE`, `CRASHED`, `MEMORY_EXHAUSTED`, exits 0 only for the first, and prints a `provenance:` line naming the log path and the exact terminal-marker line the verdict rests on, plus a `memory:` line carrying the OOM and watermark-marker counts. Quote that line whenever you quote the numbers — a suite figure with no citable marker cannot be re-derived by the next reader.

**`MEMORY_EXHAUSTED` is the worst state, above `CRASHED`.** A real OOM logs the engine's allocation-failure strings, then dies with a `Fatal error:` banner and an `OutOfMemory` crash report, so `CRASHED` and `DID_NOT_COMPLETE` both describe it correctly and uselessly — one sends you to `Saved/Crashes`, the other to a harness timeout, and neither names the memory. Do not read the test counts from such a run; re-run under a higher `-MemoryFraction`, or fix what grew. **`COMPLETED_WITH_MEMORY_PRESSURE`** means the queue drained and every assertion ran, but either a maintenance collect could not get the working set back under the hard fraction (`PINWRIGHT_MEMORY_WATERMARK_EXCEEDED`) or an allocation failed and the backup pool absorbed it: the numbers are real, and the next run on this tree is the one that OOMs.

**`DID_NOT_COMPLETE` means the counts below are not a result.** A run killed mid-queue greps clean: zero `Result={Fail}`, no crash banner, no drain marker. Two such logs (4009 of 4559 and 2225 of 4559) were read as suite results on this host. Do not grep the bare phrase `Automation Test Queue Empty` to check for completion — it is the argument to `-TestExit` and is echoed into every log's command line (2 hits in a killed log, 4 in a drained one). Re-run instead of interpreting, and only after fixing whatever ended the run.

**`CRASHED` is a separate verdict and the checker decides it, not you.** It requires positive evidence: a fatal/assert banner, or a non-ensure crash report in `Saved/Crashes` inside the run's window. A short log is not a crash.

### 3b-2. Parse the log

Only once the verdict is `COMPLETED_*`:

```
# Failing tests
Grep
  pattern: "Result=\\{Fail\\}"
  path:    "<HOST_LOG>"
  output_mode: "content"
  -B: 5
  head_limit: 0

# Sanity counter
Grep
  pattern: "Result=\\{Success\\}"
  path:    "<HOST_LOG>"
  output_mode: "count"
```

Codex equivalents:

```powershell
rg -n -B 5 "Result=\\{Fail\\}" "<HOST_LOG>"
(rg "Result=\\{Success\\}" "<HOST_LOG>" | Measure-Object).Count
```

Each `Result={Fail}` line is preceded a few lines earlier by `BeginEvents: <FullTestPath>`; `-B 5` catches it. Extract the full test path (e.g. `PinWright.bpir.compile.PrivateVarErrorMessage`).

Map each test path to its source `.cpp` via one batched Grep:

```
Grep
  pattern: "IMPLEMENT_SIMPLE_AUTOMATION_TEST\\([^,]+,\\s*\"(<full.test.path.A>|<full.test.path.B>|...)\""
  path:    "<PLUGIN_ROOT>/Source/PinWright/Private/Tests"
  output_mode: "content"
  -n: true
```

Codex equivalent:

```powershell
rg -n "IMPLEMENT_SIMPLE_AUTOMATION_TEST\\([^,]+,\\s*\"(<full.test.path.A>|<full.test.path.B>|...)\"" "<PLUGIN_ROOT>/Source/PinWright/Private/Tests"
```

Each match line gives you the test class name (first macro arg) and the file. A test unit = the failing `.cpp` plus its same-name `.h` in the same directory if one exists.

**Mid-suite crash — only when the checker said `CRASHED`.** The editor sometimes segfaults part-way through (typical pattern: an `ensure` failure followed by an access violation, or a direct unhandled exception). The suite stops at that point — tests after the crash never run. Treat the crash as one more failing unit and continue the loop. Don't escalate just because the editor died; a crashing test is a bug to fix like any other failure, and the next cycle's run will pick up where this one left off.

**An `ensure` on its own is NOT a crash.** It writes a full `Saved/Crashes` report with `IsEnsure=true`, logs `Ensure condition failed:` and lets the run continue — a green 4576-test run on this host carried four of them. Greping a truncated log for `EnsureFailed` is what got two externally-killed runs (4009 and 2225 of 4559, no fatal banner, no non-ensure crash report anywhere in their windows) filed as a host GC crash, and a board ticket built on that misattribution. That is why `check_suite_log.py` owns this call: it requires a fatal/assert banner or a non-ensure crash report inside the run's window, and prints `crashReports: <n> non-ensure, <n> ensure-only` so the distinction is visible.

The crash-site greps below are for locating the offending test once the verdict is `CRASHED` — not for deciding whether one happened:

```
# Crash-site context (run only when the verdict is CRASHED)
Grep
  pattern: "(=== Critical error ===|Unhandled Exception|Fatal error:|Assertion failed:)"
  path:    "<HOST_LOG>"
  output_mode: "content"
  -n: true
  head_limit: 5
```

Codex equivalent:

```powershell
rg -n "(=== Critical error ===|Unhandled Exception|Fatal error:|Assertion failed:)" "<HOST_LOG>" | Select-Object -First 5
```

When a crash is detected, identify the offending test:

1. **Test path:** the last `Test Started. Name=\{<X>\} Path=\{<Y>\}` line in the log with no matching `Test Completed` is the crashing test — extract `<Y>`.
2. **Source site:** in the crash callstack (the lines after `=== Critical error ===` or `Unhandled Exception`), the first frame inside `UnrealEditor-PinWright.dll` (tests are compiled into the main module DLL) gives an exact `<test-cpp-path>:<line>`. That cpp file is the unit to dispatch against.

Add the crashing test to the per-unit dispatch list. If a unit you'd already build for visible failures shares the same cpp file as the crash, fold them into one subagent dispatch (one prompt covers all failing tests in that file plus the crash) — don't dispatch two subagents against the same file.

**Wrong command shape.** If the log shows `Ready to start automation` but zero `Test Started` lines, first verify the launch used the exact quoted `-ExecCmds="Automation RunTests PinWright,Quit"` form and the `-TestExit="Automation Test Queue Empty"` marker. A wrong or over-broad prefix can look like a hung launcher even when the editor process is alive.

**True launcher failure.** The above handles in-suite crashes, where the editor at least ran some tests. If the log shows zero `Test Started` lines AND zero `Result={Success}` AND the editor exit was non-zero after confirming the command shape, the editor itself failed to bring up automation (cooked-asset error, missing module, license check, etc.) — that is unrecoverable from this loop. Stop and escalate with the exit code and last 50 log lines.

### 3c. Dispatch test-fix subagents

One subagent per unit, all in the same tool turn. Standard failure prompt:

> Failing tests in `<test-cpp-path>`<and `<test-h-path>` if it exists>: `<FBlah, FBaz>`.
>
> Test log (search for the test class names to find each `BeginEvents`/`EndEvents` block): `<HOST_LOG>`
> Protocol: `<SKILL_ROOT>/test-fix-protocol.md`
> Cycle: `<N>`. Report in three sentences when done.

If the unit is the **crashing** test (with or without additional visible failures in the same file):

> Failing tests in `<test-cpp-path>`<and `<test-h-path>` if it exists>: `<FBlahCrashing>`<, plus visible failures `<FOther1, FOther2>` if any in the same file>.
>
> The editor crashed during `<FBlahCrashing>`. Crash callstack and surrounding context (read directly from the log around the `=== Critical error ===` block):
> ```
> <paste the 10-30 most-relevant lines verbatim — the ensure message, the top ~15 stack frames, the test's BeginEvents block if present>
> ```
>
> Fix the root cause at the test or the production code it exercises. Do not catch/swallow the exception to make the test "pass", and do not skip the test to dodge the crash. If the crash is caused by leftover fixture assets (e.g. a stale `/Game/.../NS_*` artifact whose PostLoad asserts), the right fix is usually to recreate the fixture cleanly each run rather than tolerate the stale state.
>
> Test log: `<HOST_LOG>`
> Protocol: `<SKILL_ROOT>/test-fix-protocol.md`
> Cycle: `<N>`. Report in three sentences when done.

Foreground. After all return → Phase 1 (recompile + retest). On each subsequent test run, compute the test signature set: `{failing test paths} ∪ {crash site if any}`. Compare to the previous test run's set:

- **Sets differ in any way** (any test went fail→pass, any test went pass→fail, crash appeared/disappeared/moved) → reset `test_stuck_count = 0` and continue.
- **Sets identical** → increment `test_stuck_count`. If it reaches 10, escalate with the persistent failure list and `git diff` since invocation start, stop the skill.
- **Crash-cleared transition** (previous cycle crashed mid-suite, this cycle did not) → reset `test_stuck_count = 0` regardless of count change. The new suite is running a larger set of tests; that's progress, not stagnation.

Strict-decrease in failure count is **not** required. The loop is allowed to swap one failure for another for as long as it takes — what stops it is 10 cycles with literally the same set of failures, indicating the fix subagents are no longer moving the needle.

### Conflict handling

A test-fix subagent will often touch production code under `Source/PinWright/Private/...`. Two subagents touching the same production file in parallel can lose edits. We accept this risk: dispatch optimistically in parallel, and let the next compile cycle catch any collision as a compile error. The cost of an occasional overlap is much lower than serializing every dispatch through pre-flight declaration round-trips.

If the next compile cycle produces "previously-passing-now-broken" cluster errors that look like merge collisions (e.g. duplicated insertions, missing closing braces from competing edits), escalate to the user with the diff — don't try to auto-recover.

## Phase 6: Summary

When the outer loop terminates (clean exit or stuck-detector escalation), print one summary block:

```
Test loop summary
=================
Outer cycles run: <N>
Final state: <CLEAN | DIRTY-COMPILE | DIRTY-TEST | LAUNCHER-FAIL | STUCK-COMPILE | STUCK-TEST>

Compile errors fixed: <count>
Test failures fixed:  <count>
Crashes fixed:        <count>
Test failures remaining: <count>
Cycles spent on the final stuck signature: <0 if CLEAN, else 10>

Remaining issues:
- <test name or compile error> — <last subagent's one-line report>

Suggested next: <re-invoke skill | manual investigation of <X> | mcp-audit for <Y>>
```

`STUCK-COMPILE` / `STUCK-TEST` mean the loop tripped the 10-cycle identical-signature detector. `LAUNCHER-FAIL` is the editor-failed-to-start case from Phase 3b. There is no `CAPPED` state anymore — the only ways out are green, stuck, launcher-fail, or `.Build.cs` rules-compiler failure (Phase 1a blind spot).

In-loop status to the user: at most one short line per cycle ("cycle 2 compile: 7 errors → 5 subagents", "cycle 2 tests: 12 failing + 1 crash → 8 subagents"). Don't narrate further — the summary above is the only longer output.

## Why this shape

- **Orchestrator never reads source.** A 2000-test suite with even 5% failures would blow up the main context if test bodies were pulled in. Subagents own all reads and edits.
- **One subagent per source file**, not per test or per error. Errors/failures in one file usually share a root cause (the production handler the file exercises); bundling them gives the subagent the full picture and avoids races.
- **CLI compile + CLI test** rather than MCP. CLI runs cold every time but always runs — no precondition that the editor be live, and a crash-and-fix cycle is just two CLI invocations. Editor crashes mid-suite are recoverable: identify the crashing test from the callstack, dispatch a fix, re-run.
- **Inline errors in prompts, fixed log paths, no JSON state.** The orchestrator already has the parsed error list in working memory after the grep; serializing it to disk and rereading it later was redundant. Subagents get errors verbatim in the prompt and may read the log directly for surrounding context.
- **Optimistic-parallel test fixes.** Pre-flight declaration round-trips cost more than the occasional collision they'd prevent; the next compile naturally surfaces overlapping edits.
- **Stuck-detector instead of hard caps.** Iterative drive toward green is the goal — and real green states often take many cycles to reach. A hard cap (3 inner / 5 outer in the old design) bailed out on legitimate in-progress work; a 10-cycle identical-signature detector lets the loop run as long as it's making *any* progress (errors changing shape, failures swapping, tests flipping fail↔pass) and only stops when it's clearly spinning. The cost of an extra 20 cycles unattended is tiny; the cost of a false escalation that interrupts the human is large.
