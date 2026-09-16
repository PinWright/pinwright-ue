---
name: mcp-version-matrix
description: Drive the PinWright plugin to a green compile + automation suite on every installed UE version (5.3-5.8), one version at a time, windowless, as a background Workflow — front-loading a primary version, fixing each host to green, and pushing one commit per version straight to origin/master until all versions are green at one commit. Use when the user says "mcp-version-matrix", "fix all UE versions", "test against every engine version", "version matrix", "make all versions green", or asks to compile+test the plugin across the whole UE matrix.
---

# MCP Version Matrix

Take the PinWright plugin to green stdio-connector unit tests + a clean compile + green automation suite + a
clean Rocket packaging gate on **every installed UE version** (5.3-5.8), as a background Workflow. Builds are
machine-serial on one box, so the run processes **one version at a time**: it syncs that version's host clone
to `origin/master`, then runs the Python connector's unit tests on that engine's bundled interpreter, builds,
tests, and packages it to green with the editor running **windowless** (`-RenderOffScreen`), then pushes
**one commit per version** straight to `origin/master` so the other clones inherit the fix on their next sync. It front-loads a primary version (default 5.7) so version-agnostic fixes land once and propagate, then
walks the rest newest-to-oldest and defers 5.8 to the very end (order: 5.7, 5.6, 5.5, 5.4, 5.3, 5.8), then
converges to an all-green cross-version fixpoint. Once every non-blocked version is green at one commit, a
final **Fab artifacts** phase stages a Fab-ready zip per green version (staging only, no compile).

The run is **finite and git-resumable**: the only durable state is git. Relaunching with the same args
re-derives the remaining work from git and converges — so a session/message-limit death is survivable.

## Preflight (main conversation)

- **One matrix run per box.** This run owns the editor + build slot for every host it touches. Do not start
  it while CI is building, or while another UE build/editor for these hosts is live — UBT's machine-global
  mutex and the editor instance lock will collide.
- The MCP need not be connected up front; the run never talks to the editor over MCP, and each leg leaves
  its host editor down when it finishes.

## Launch (background workflow)

Invoke the **Workflow** tool with:
- `scriptPath`: `<host project>\.claude\skills\mcp-version-matrix\mcp-version-matrix.workflow.js`
- `args`:
  - `hostsRoot` (**required**) — directory holding the per-version host projects.
  - `hostPrefix` (**required**) — their common name prefix: version `5.6` uses `<hostsRoot>\<hostPrefix>56`
    with `<hostPrefix>56.uproject` and `Plugins\PinWright` inside. A Content Examples host project copied
    per engine version is a good choice, since it exercises every integration sub-module. There is no
    default: a baked-in checkout path would only ever be right on one machine.
  - `engineRootTemplate` (default `C:\UE_{version}`) — engine install per version; `{version}` is substituted.
  - `protoDir` (optional) — directory holding this skill's fix protocols; defaults to the compiled copy
    inside the `primary` version's host project.
  - `primary` (default `"5.7"`) — the front-load version greened first.
  - `deferLast` (default `"5.8"`) — the version held back to the very end. Everything between `primary`
    and `deferLast` runs newest-to-oldest, so the deepest compat drift (oldest engines) is hit while there
    is still cycle budget, and the newest engine — the baseline the code is written against — is the final
    cheap confirmation.
  - `maxCyclesPerVersion` (default `20`) — compile/test cycle cap before a version is declared BLOCKED.
  - `versions` (optional) — explicit candidate list; default is the full `5.3`-`5.8` matrix (missing
    engines/hosts are skipped in preflight).
  - `maxRegressions` (default `3`) — a version that regresses this many times is treated as non-converging
    ping-pong and hard-fails the run.
  - `emitFabZips` (default `true`): after the fixpoint, stage a Fab-ready zip per green version (staging only,
    no compile). Pass `false` to skip the Fab artifacts phase.

After editing the skill sources under `.polyskill\`, re-run `npx --yes polyskill` in
`Plugins\PinWright\.polyskill\` to regenerate the compiled copies the `scriptPath` above points at.

Report the task id, then **begin supervision** — do not fully return control; keep the run alive per
"Supervision" below until it reaches the fixpoint, hard-fails, or the user stops it.

## Connector compat gate (bundled Python)

Before COMPILE, the per-version agent runs the stdio MCP connector's unit tests on **that engine's bundled
Python** (`<engine>\Engine\Binaries\ThirdParty\Python3\Win64\python.exe` — 3.9 on 5.3, 3.11 on 5.4+):
`python -m unittest discover` under `Content\Python\tests`, teed to `Saved\Logs\mcp-version-matrix-pytest.log`.
This proves `mcp_proxy.py` imports and passes stdlib-only on every supported UE version — a non-stdlib import
or a 3.9-vs-3.11 syntax incompatibility fails the run. The agent fixes `Content\Python` (keeping it stdlib-only
and 3.9–3.11 compatible) and re-runs; the fix is committed like any source change. It **gates green**
(`pyTestsPassed`), so a version whose connector tests fail is `blocked`, not green. The step is fast (no editor,
no plugin DLL) and runs before the expensive compile so a broken connector fails early.

## Packaging gate (Fab parity)

After a version's suite is green, the per-version agent runs the **Rocket packaging gate**:
`scripts\package-prebuilt.ps1` drives RunUAT `BuildPlugin -Rocket` (with `-StrictIncludes` on 5.4+, and the
MSVC `14.38.33130` pinned via `BuildConfiguration.xml` on 5.3 - the full version, quoted) and tees the output to
`Saved\Logs\mcp-version-matrix-package.log`. Only the **test-stripped staged tree** is compiled, so the gate
sees exactly the file set Fab would.

The gate **fails** on any plugin-attributable warning in the log: a `warning C####` under the staged plugin
tree, any `deprecated` warning (even from an engine header, since only plugin TUs compile here), or a UBT
descriptor warning naming `Plugin 'PinWright'`. Strict-includes / unity-hidden **compile** errors that only the
Rocket build surfaces are fixed and fed back through COMPILE. A warning-only fix (a version-guarded API swap, a
header switch, or a `.uplugin` descriptor entry) still counts as a real change and still produces that
version's per-version commit.

## Supervision (you, the main conversation, after launch)

There is no separate supervisor process — you are it. The run is long (a full cold matrix is hours), so watch
it two ways: **on the task's completion notification** (authoritative — it carries the return value with
`stop_reason`), and with a **periodic progress check** (every ~15 min via `CronCreate`, or `ScheduleWakeup` /
`/loop`). The workflow already retries transient agent deaths in-script; supervision is the outer layer for
when the whole task dies.

### Reading progress (durable signals only)

The background Workflow is **not** a `TaskGet`/`TaskList` task — calling those returns "task not found". Read
progress from durable state instead:
- **Active leg:** `Get-CimInstance Win32_Process -Filter "Name='cl.exe' or Name='UnrealEditor.exe' or Name='UnrealBuildTool.exe'"` — the `<hostPrefix><NN>` in a command line is the active version (`53`→5.3 … `58`→5.8). A `cl.exe` swarm = compiling; an `UnrealEditor.exe` on that host = running the suite; neither = the agent is fixing between cycles. Ignore `-fuzz` hosts (separate `mcp-fix-workflow` runs).
- **PACKAGE phase:** the Rocket packaging gate shows up as `AutomationTool.exe` / `UnrealBuildTool.exe`
  processes plus a `cl.exe` swarm compiling under `C:\ea_pkg_tmp_mtx` (the short scratch root, **not** the host
  project tree), writing `<host>\Saved\Logs\mcp-version-matrix-package.log`. This is normal progress on a green
  version, not a stall.
- **Greens landed:** `git -C <a host clone> fetch origin` then `git log origin/master --oneline | grep version-matrix` — one `version-matrix: UE <v> green` commit per version that needed a fix. A version that passed with **no** fix (e.g. 5.7, kept healthy by the fuzz loop) produces no commit — that's a clean pass, not a skip.
- **Liveness:** the newest file under the run's transcript dir (`<session>/subagents/workflows/<runId>/agent-*.jsonl`) — its mtime shows the run is moving. Optionally tail the active host's `Saved/Logs/mcp-version-matrix-build.log` (compile state) or `Saved/Logs/<hostPrefix><NN>.log` (suite state).
- **Stall:** newest transcript file unchanged ~25+ min AND no build/editor process for any matrix host → investigate (tail the active log). A long suite run (editor up, agent blocking on `Wait-Process`) is **not** a stall.

### Classifying the completion (`stop_reason`)

- **`fixpoint`** (bounded-done) — every non-blocked version is green at one commit. **Stop, do not relaunch.**
  Report the `green` list, the `finalTip`, and any `blocked` versions. If `blocked` is non-empty, surface each
  blocked version with its reason + cited log — those need a human (a genuinely un-greenable version, e.g. a
  removed engine API, or a `.Build.cs` error). The matrix is otherwise done.
- **`agent_died`**, an abnormal/killed end, or a failure mentioning *session limit* / *connection* —
  **Transient.** Relaunch the **same Workflow with the same args** (it resumes from git: already-green
  versions reconfirm fast via an incremental build, only behind/red versions get worked). **Thrash bound:**
  count only consecutive relaunches that make **no forward progress** (no new green version, no new commit on
  `origin/master`); after **2**, stop and report — a human is needed.
- **`fatal`** — **Critical / unrecoverable.** **Do not relaunch.** The payload carries `fatalVersion`,
  `fatalPhase`, `reason`, and `logPath`/`excerpt`. Read the cited log, summarize the root cause for the user,
  and leave the run stopped. These are environment/state errors that recur for every relaunch: editor won't
  launch at all, an engine/host path vanished, disk/DDC failure, a git auth/push failure, an unreconcilable
  rebase, or non-convergence (ping-pong / tip-advance ceiling). The failing version's working tree is
  preserved for inspection.

## Stopping the run (main conversation)

When the user asks to stop:
1. **Disarm supervision first** — cancel the hourly re-check (exit `/loop` / drop the scheduled wakeup) so the
   deliberate stop is not classified as a transient crash and relaunched.
2. `TaskStop` the background task.
3. **Kill only the editors this run started** (at most the active leg's suite editor), never
   an editor the user has open for another project or the `-fuzz` hosts. OS-kill scoped by command line is the practical mechanism
   (the MCP is wired to only one editor): `Stop-Process` the `UnrealEditor.exe` whose command line matches
   `<hostPrefix>5[3-8]\` **and not** `fuzz`, never a blanket `taskkill /IM UnrealEditor.exe`. After
   `Stop-Process`, re-query to confirm — `Get-CimInstance` can read stale for a beat (a "still running" list
   right after the kill is usually already-dead processes).

## Notes

- **Windowless:** the per-version agent launches the GUI `UnrealEditor.exe` with
  `-RenderOffScreen -unattended -nopause -nosplash -nocefaccelpaint -ddc=InstalledNoZenLocalFallback
  -WindowStyle Hidden` and never `-log`, so no editor or console window appears. The build redirects to a log
  file. (See the workflow script for the exact launch.) `-ddc=InstalledNoZenLocalFallback` is required on this
  box: IPv6 loopback is refused machine-wide, so the editor cannot reach ZenServer and the default DDC graph
  comes up with no writable node, aborting startup with "Unable to use default cache graph
  'InstalledDerivedDataBackendGraph'".
- **Git:** all commits go straight to `origin/master`, one per version's green-achievement, staging exact
  paths (never `add -A`). A later regression re-fix is a new commit, never an amend; the run never
  force-pushes master.
- **Independent severities:** a single version that can't go green is recorded `blocked` and the run
  continues the others; only environment/state errors hard-fail the whole run (see `fatal` above).

## Known characteristics (observed in practice)

- **The fixpoint sweep is a long tail.** Every per-version commit advances the tip, so when the forward walk
  finishes, the loop re-verifies *all* earlier versions at the final tip — a full incremental-build + suite
  per version (~15-20 min each, ~1+ hour total) even though the substantive fixes are already pushed. A
  concurrent writer (e.g. the `mcp-fix-workflow` fuzz hosts) pushing to `origin/master` keeps the tip moving
  and lengthens the sweep. For a fast, clean run, **pause other UE work on the box first**.
- **No idle-editor accumulation.** A finished leg leaves its host editor **down** - COMPILE stops it to
  unlock the plugin DLL and FINISH no longer relaunches it - so at most one matrix editor is up: the leg
  currently running its suite. (The old FINISH relaunch existed "so the MCP reconnects", but the matrix
  never uses the MCP; with several piled up the packaging gate ran at half compile parallelism and took
  twice as long.) If you want the MCP against a host afterwards, launch that host's editor by hand.
- **Concurrent builds serialize, they don't fail.** `-WaitMutex` queues a matrix build behind any other UE
  build on the box, so the run still completes — just slower.
- **Partial-stop result is durable.** Each version's fix is committed+pushed the moment it greens, so stopping
  mid-sweep keeps every landed fix; only the cross-version regression *confirmation* is lost.
- **Packaging roughly doubles per-version wall time.** The gate is a full from-scratch Rocket `BuildPlugin`
  (no incremental reuse), so each newly-green version pays a second full build on top of its editor build. 5.3
  packages **non-strict** (no `-StrictIncludes`) and pins MSVC `14.38.33130` through `BuildConfiguration.xml` (the FULL version, quoted: `14.38` is rejected, and PowerShell splits the value if unquoted); a crash
  mid-gate can leave a stale pin in that file, which the script self-heals on its next run (backup / restore
  around the build). Per-version commits may now include `PinWright.uplugin` when the gate needed a descriptor
  fix.
