// mcp-version-matrix — drive the PinWright plugin to a green compile + automation
// suite on every installed UE version (5.3-5.8), one version at a time, then push
// one commit per version straight to origin/master so the other host clones inherit
// the fix on their next sync.
//
// Single box => builds are machine-serial: exactly one version builds/tests at a
// time (a `for` over versions, awaited). The editor never shows a window
// (-RenderOffScreen). The run is finite and git-resumable: relaunching with the same
// args re-derives remaining work from git and converges to the all-green fixpoint.
//
// Sandbox: standard JS only (no fs/shell/Math.random/Date). Every effect is an
// agent(). The script owns sequencing + the fixpoint; agents own all shell + git.

export const meta = {
    name: 'mcp-version-matrix',
    description: 'Drive the PinWright plugin to green stdio-connector unit tests + compile + automation suite on every installed UE version, serially and windowless, pushing one commit per version to origin/master, converging to an all-green cross-version fixpoint.',
    phases: [
        { title: 'Preflight' },
        { title: 'Versions' },
        { title: 'Fab artifacts' },
    ],
};

const opts = (args && typeof args === 'object') ? args
    : (typeof args === 'string' && args.trim()
        ? (() => { try { return JSON.parse(args); } catch (e) { return {}; } })()
        : {});

// Front-load version: greened first so version-agnostic fixes land once on
// origin/master and the rest inherit them on sync (then only chase compat deltas).
const PRIMARY = opts.primary || '5.7';
// Deferred to the very end: the newest engine is the baseline the code is written
// against, so it is the cheapest confirmation and earns the least by running early.
// Going PRIMARY -> oldest first surfaces the deep compat drift while there is still
// cycle budget to fix it.
const DEFER_LAST = opts.deferLast || '5.8';
// Hard cap on compile/test cycles for one version before it is declared BLOCKED.
const MAX_CYCLES = (typeof opts.maxCyclesPerVersion === 'number' && opts.maxCyclesPerVersion > 0) ? opts.maxCyclesPerVersion : 20;
// A version that regresses (needs fixes again at a newer tip) more than this many
// times is cross-version ping-pong — hard-fail rather than spin.
const MAX_REGRESSIONS = (typeof opts.maxRegressions === 'number' && opts.maxRegressions > 0) ? opts.maxRegressions : 3;
// Optional explicit candidate list; default is the full 5.3-5.8 matrix.
// Every agent runs on Opus (override with args.model); never inherit the session model.
const MODEL = opts.model || 'opus';
const CANDIDATES = Array.isArray(opts.versions) && opts.versions.length ? opts.versions : ['5.3', '5.4', '5.5', '5.6', '5.7', '5.8'];
// After the matrix reaches its fixpoint, stage a Fab-ready zip per green version
// (staging only, no compile). Default on; pass emitFabZips:false to skip.
const EMIT_FAB_ZIPS = opts.emitFabZips !== false;

// One host project per engine version, laid out as <hostsRoot>\<hostPrefix><nn> with a matching
// <hostPrefix><nn>.uproject inside (a Content Examples host project copied per version works well,
// since it exercises every integration sub-module). Both are mandatory: there is no canonical
// location, so a baked-in checkout path would only ever be right on one machine.
const HOSTS_ROOT = opts.hostsRoot;
const HOST_PREFIX = opts.hostPrefix;
if (!HOSTS_ROOT || !HOST_PREFIX) {
    throw new Error('mcp-version-matrix: args.hostsRoot and args.hostPrefix are required (per-version host projects live at <hostsRoot>\\<hostPrefix><nn>)');
}
// Engine installs, one per version. Overridable for a non-default layout.
const ENGINE_ROOT_TEMPLATE = opts.engineRootTemplate || 'C:\\UE_{version}';

// Per-version parameters, derived from the version string.
// 5.3 needs the older MSVC toolchain pinned (engine-header C4668 under the machine
// default); 5.4+ use the default.
function paramsFor(v) {
    const nn = v.replace('.', '');
    const engine = ENGINE_ROOT_TEMPLATE.replace('{version}', v);
    const host = `${HOSTS_ROOT}\\${HOST_PREFIX}${nn}`;
    const clone = `${host}\\Plugins\\PinWright`;
    return {
        version: v,
        engine,
        editorExe: `${engine}\\Engine\\Binaries\\Win64\\UnrealEditor.exe`,
        buildBat: `${engine}\\Engine\\Build\\BatchFiles\\Build.bat`,
        host,
        uproject: `${host}\\${HOST_PREFIX}${nn}.uproject`,
        clone,
        testLog: `${host}\\Saved\\Logs\\${HOST_PREFIX}${nn}.log`,
        buildLog: `${host}\\Saved\\Logs\\mcp-version-matrix-build.log`,
        packageLog: `${host}\\Saved\\Logs\\mcp-version-matrix-package.log`,
        // Per-version BUNDLED Python (5.3 -> 3.9, 5.4+ -> 3.11): the stdio MCP proxy runs on it, so
        // its unit tests must pass here to prove the connector works stdlib-only on this UE version.
        bundledPython: `${engine}\\Engine\\Binaries\\ThirdParty\\Python3\\Win64\\python.exe`,
        pyTestsDir: `${clone}\\Content\\Python\\tests`,
        pyTestLog: `${host}\\Saved\\Logs\\mcp-version-matrix-pytest.log`,
        strictIncludes: v !== '5.3',
        packageScript: `${clone}\\scripts\\package-prebuilt.ps1`,
        // Memory-capped suite launcher: same argv, inside a Job Object with a per-process cap.
        suiteScript: `${clone}\\scripts\\Run-SuiteCapped.ps1`,
        // Its general-purpose sibling: runs ANY command in the same Job Object (memory cap +
        // priority class + kill-on-close). Used for the build. Same clone, same scripts dir,
        // so it arrives with the S1 pull exactly like suiteScript does.
        cappedScript: `${clone}\\scripts\\Run-Capped.ps1`,
        suiteResult: `${host}\\Saved\\Logs\\${HOST_PREFIX}${nn}.log.result.txt`,
        pkgOut: `${host}\\Saved\\mcp-matrix-pkg`,
        scratchRoot: 'C:\\ea_pkg_tmp_mtx',
        msvcPin: v === '5.3' ? '14.38.33130' : null,
    };
}

// Directory holding this skill's fix protocols, handed to agents by path rather than pasted.
// Defaults to the compiled copy beside the primary version's host project; override with
// args.protoDir when the skill is invoked from a different location.
const PROTO_DIR = opts.protoDir || `${HOSTS_ROOT}\\${HOST_PREFIX}${PRIMARY.replace('.', '')}\\Plugins\\PinWright\\.polyskill\\skills\\mcp-version-matrix`;

// Retry an agent() that died on a TRANSIENT failure — the runtime returns null
// after its own API-level retries (e.g. "Connection closed mid-response"). A fresh
// agent re-runs the same step; a returned object passes straight through.
async function agentOrRetry(make, tries = 3) {
    let r = null;
    for (let i = 0; i < tries; i++) {
        r = await make();
        if (r) return r;
        if (i + 1 < tries) log(`agent died (transient); retrying (attempt ${i + 2}/${tries})`);
    }
    return r;
}

const preflightSchema = {
    type: 'object', required: ['status', 'order', 'tip'],
    properties: {
        status: { type: 'string', enum: ['OK', 'FATAL'] },
        order: { type: 'array', items: { type: 'string' } },     // eligible versions, primary first
        tip: { type: 'string' },                                 // origin/master HEAD SHA
        skipped: { type: 'array', items: { type: 'object', properties: { version: { type: 'string' }, reason: { type: 'string' } } } },
        reason: { type: 'string' },
        note: { type: 'string' },
    },
};

// The per-version build-test-fix agent's structured result.
const btSchema = {
    type: 'object', required: ['status'],
    properties: {
        status: { type: 'string', enum: ['OK', 'BLOCKED', 'FATAL'] },
        green: { type: 'boolean' },              // true only after a passing Python-connector run AND a final clean full-suite run AND a clean Rocket packaging gate
        pyTestsPassed: { type: 'boolean' },      // mcp_proxy.py unit tests green under THIS version's bundled Python (3.9 on 5.3, 3.11 on 5.4+), stdlib-only
        tipBuiltAgainst: { type: 'string' },     // origin/master HEAD after the agent's pull --rebase
        touched: { type: 'array', items: { type: 'string' } },  // plugin source paths edited (empty if already green)
        cyclesRun: { type: 'number' },
        gatePassed: { type: 'boolean' },         // true after the Rocket packaging gate scanned clean
        packageLogPath: { type: 'string' },      // the Rocket/BuildPlugin log for the supervisor
        packageWarnings: { type: 'array', items: { type: 'string' } },  // distinct plugin-attributable warning sites the gate fixed
        reason: { type: 'string' },              // for BLOCKED/FATAL
        phase: { type: 'string' },               // sync | pytest | compile | test | package (where a FATAL happened)
        logPath: { type: 'string' },             // cite the build or test log for the supervisor
        excerpt: { type: 'string' },             // salient error lines
        summary: { type: 'string' },
    },
};

const commitSchema = {
    type: 'object', required: ['status'],
    properties: {
        status: { type: 'string', enum: ['OK', 'FATAL'] },
        committed: { type: 'boolean' },
        pushed: { type: 'boolean' },
        newTip: { type: 'string' },              // origin/master HEAD after push (or the unchanged tip)
        reason: { type: 'string' },
        logPath: { type: 'string' },
        excerpt: { type: 'string' },
        note: { type: 'string' },
    },
};

// The per-version Fab-zip agent's structured result (post-fixpoint, staging only).
const fabZipSchema = {
    type: 'object', required: ['status'],
    properties: {
        status: { type: 'string', enum: ['OK', 'FAILED'] },
        zipPath: { type: 'string' },             // the staged Fab zip on success
        reason: { type: 'string' },              // failure text (reported only; never un-greens the version)
        logPath: { type: 'string' },
    },
};

function preflightPrompt() {
    const table = CANDIDATES.map((v) => {
        const p = paramsFor(v);
        return `  ${v}: editor "${p.editorExe}", uproject "${p.uproject}", clone "${p.clone}"`;
    }).join('\n');
    return `Preflight for the PinWright cross-version test matrix. Run PowerShell. Read-only except a git fetch.

Candidate versions and their paths:
${table}

1. ELIGIBILITY: for each candidate, it is eligible only if BOTH the editor exe AND the host uproject exist on disk (Test-Path). Record the rest as skipped with the reason (missing engine / missing host).
2. CLONES: for each eligible version, confirm "<clone>\\.git" exists and the clone is on branch master (git -C "<clone>" rev-parse --abbrev-ref HEAD). If an eligible version's clone is missing or not on master, that is a setup error you cannot safely proceed past for the whole run -> return status FATAL with the reason and the offending clone path.
3. TIP: in ONE eligible clone, run git -C "<clone>" fetch origin, then read git -C "<clone>" rev-parse origin/master — that SHA is the current tip. If the fetch fails (auth/network), return status FATAL with the git error.
4. ORDER: return the eligible versions ordered as (a) "${PRIMARY}" FIRST if eligible, then (b) every remaining eligible version EXCEPT "${DEFER_LAST}", DESCENDING by version number (newest to oldest), then (c) "${DEFER_LAST}" LAST if eligible. "${DEFER_LAST}" must never appear anywhere but the very end, and "${PRIMARY}" never anywhere but the very front. With the whole 5.3-5.8 matrix eligible and the defaults, that is exactly: 5.7, 5.6, 5.5, 5.4, 5.3, 5.8. If "${PRIMARY}" is not eligible, drop step (a) and keep the rest. If "${PRIMARY}" and "${DEFER_LAST}" are the same version, it goes first and is not repeated at the end.

Return {status:'OK', order:[...], tip:'<sha>', skipped:[{version,reason}], note} on success, or {status:'FATAL', reason, ...} if no version is eligible or a clone/git problem blocks the whole run. Do NOT build anything.`;
}

function buildTestPrompt(p, regressing) {
    // NOTE the quotes: unquoted, PowerShell splits -CompilerVersion=14.38.33130 into
    // "-CompilerVersion=14" and ".38.33130", and UBT then reports "Unable to find valid
    // 14 C++ toolchain". The value must be the FULL version - "14.38" is rejected too.
    // As a -CommandArgs array element the single quotes do that job (one literal token).
    const pin = p.msvcPin ? `,'-CompilerVersion=${p.msvcPin}'` : '';
    const strict = p.strictIncludes ? ' -StrictIncludes' : '';
    const cver = p.msvcPin ? ` -CompilerVersion ${p.msvcPin}` : '';
    return `You are the build-test-fix agent for **UE ${p.version}** of the PinWright plugin. Drive THIS version's host to a passing Python-connector unit-test run (the stdio proxy on the bundled Python), a clean compile, a fully green PinWright automation suite, AND a clean Rocket packaging gate (Fab warning parity), windowless, in this one session. Hold the build errors, the test failures, the packaging warnings, and your edits all in your own context across cycles. Run commands in PowerShell. Hard cap: ${MAX_CYCLES} compile/test/package-fix cycles.

Paths for this version:
- plugin clone (edit + commit source here): ${p.clone}\\Source; warning fixes may also touch ${p.clone}\\PinWright.uplugin; connector fixes touch ${p.clone}\\Content\\Python
- uproject: ${p.uproject}
- editor exe (GUI, run offscreen): ${p.editorExe}
- bundled Python (this version's interpreter): ${p.bundledPython}
- Python connector tests: ${p.pyTestsDir}    pytest log: ${p.pyTestLog}
- build log: ${p.buildLog}    test log: ${p.testLog}    package log: ${p.packageLog}
Fixing guidance (read as needed): ${PROTO_DIR}\\compile-fix-protocol.md, ${PROTO_DIR}\\test-fix-protocol.md and ${PROTO_DIR}\\package-fix-protocol.md.

== SYNC (once, first) ==
S1. git -C "${p.clone}" pull --rebase origin master  — so you build against current source (incl. fixes earlier versions just pushed). Then record the tip: git -C "${p.clone}" rev-parse HEAD -> this is tipBuiltAgainst.
S2. If the pull CONFLICTS and you cannot reconcile it (keeping both sides), STOP and return status FATAL, phase "sync", with the git error in 'reason'/'excerpt' — do NOT git rebase --abort-and-guess; a human reconciles. The tree should normally be clean here.

== PYTEST (after SYNC, before COMPILE — fast, stdlib-only, no editor/DLL) ==
Y1. The stdio MCP connector (${p.clone}\\Content\\Python\\mcp_proxy.py) must run on THIS version's BUNDLED Python (UE 5.3 ships Python 3.9, 5.4+ ship 3.11) using the STANDARD LIBRARY ONLY — no pip / third-party deps. Run its unit tests with ONE foreground BLOCKING PowerShell call (these are fast, no editor, no plugin DLL):
    & "${p.bundledPython}" -m unittest discover -s "${p.pyTestsDir}" -p "test_*.py" 2>&1 | Out-File -FilePath "${p.pyTestLog}" -Encoding utf8; "EXIT=$LASTEXITCODE" | Out-File -FilePath "${p.pyTestLog}" -Append -Encoding utf8
    If the bundled Python exe does not exist (unusual — it ships with the engine), note it and set pyTestsPassed true; do NOT fail the version on a missing interpreter.
Y2. Read the tail (Get-Content "${p.pyTestLog}" -Tail 40). PASS = the run ends with "OK" and EXIT=0. A "FAILED", an ImportError (a non-stdlib import), or a SyntaxError (a 3.9-vs-3.11 incompatibility) means the connector is broken on this UE version -> set pyTestsPassed false and FIX ${p.clone}\\Content\\Python (mcp_proxy.py and/or its tests) so it passes on this bundled Python while staying stdlib-only and syntax-compatible across 3.9-3.11; add every file you edit to 'touched' (it is committed like any source fix), then re-run PYTEST. If the SAME failure persists after ~10 attempts, return status BLOCKED, phase "pytest", citing ${p.pyTestLog}.
Y3. When PYTEST passes, set pyTestsPassed true and proceed to COMPILE.

== COMPILE ==
C1. Stop the host editor for THIS project ONLY so the plugin DLL is unlocked: stop the UnrealEditor.exe whose command line contains "${p.host}" (Get-CimInstance Win32_Process filter, Stop-Process by that PID). NEVER a blanket taskkill /IM UnrealEditor.exe. Wait until it exits.
C2. Build — ONE foreground, BLOCKING tool call with the tool timeout at max (600000 ms). Do NOT background it, do NOT poll. The build goes through Run-Capped.ps1, which runs it inside the same Job Object the suite uses at BelowNormal (so the box stays usable) and redirects ALL of Build.bat's output to the build log so it never floods your context:
    & "${p.cappedScript}" -Command "${p.buildBat}" -CommandArgs @('UnrealEditor','Win64','Development','-Project=${p.uproject}','-WaitMutex','-Log=${p.host}\\Saved\\Logs\\ubt-mtx.txt'${pin}) -OutputPath "${p.buildLog}" -PriorityClass BelowNormal; "EXIT=$LASTEXITCODE" | Out-File -FilePath "${p.buildLog}" -Append -Encoding utf8
    Grade the build on that EXIT= line, never on a pipeline status: Run-Capped passes Build.bat's own exit code straight through, except for its own 2, which means TIMEOUT or MEMORY_CAP_HIT — the PINWRIGHT_JOB_RESULT line printed and written to "${p.buildLog}.result.txt" says which, and neither is a compile error to fix.
    ${p.cappedScript} lives in THIS host's plugin clone and arrives with the S1 pull, exactly like the suite launcher; if it is missing, S1 did not run or did not reach the tip that added it -> return status FATAL, phase "compile".
    -Log is REQUIRED: without it UBT writes %LOCALAPPDATA%\\UnrealBuildTool\\Log.txt, which is shared
    machine-wide, so any other UE build running at the same time (the user's own project, CI) makes UBT
    die with "Unhandled exception: System.IO.IOException" on that file. Redirecting it per host removes
    the collision and keeps the matrix able to build alongside other work.
    If the TOOL reports a timeout (vs the command returning), re-run the exact same command (UBT resumes incrementally). Never proceed on a "still compiling" state.
C3. Read only the tail (Get-Content "${p.buildLog}" -Tail 200). Clean = EXIT=0 AND "Result: Succeeded" with zero " error " lines -> go to TEST. Otherwise fix the compile errors in ${p.clone}\\Source per compile-fix-protocol.md, then repeat COMPILE. If the build fails with NO cl.exe/LNK errors in the tail (e.g. a .Build.cs C# rules-compiler error, which UBT may not surface in this log), that is unrecoverable from here -> return status FATAL, phase "compile", citing the build log. If the SAME compile error signature persists after ~10 attempts, return status BLOCKED, phase "compile".

== TEST ==
T1. Run the FULL suite windowless and DETACHED (a full suite exceeds the 10-min tool cap, so a blocking call would be killed mid-suite; the editor is NOT resumable):
    $launch = '& "${p.suiteScript}" -HostProject "${p.uproject}" -EngineRoot "${p.engine}" -EditorExe "${p.editorExe}" -Filter PinWright -LogPath "${p.testLog}" -MemoryFraction 0.60 -PriorityClass BelowNormal -TimeoutMinutes 120 -ExtraArgs @(''-ddc=InstalledNoZenLocalFallback'',''-PinWrightTestGcEvery=25'',''-PinWrightTestMemoryWatermark=0.55'')'
    $p = Start-Process "powershell.exe" -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-Command',$launch -WindowStyle Hidden -PassThru; $p.Id
    ${p.suiteScript} lives in THIS host's plugin clone and arrives with the S1 pull, so it is always the version under test rather than a copy from somewhere else; if it is missing, S1 did not run or did not reach the tip that added it -> return status FATAL, phase "test". The launcher builds the SAME argv this step used before it existed (-ExecCmds="Automation RunTests PinWright,Quit", -TestExit="Automation Test Queue Empty", -RenderOffScreen, -unattended, -nopause, -nosplash, -nosound, -nocefaccelpaint, -RunningUnattendedScript, -Abslog) and adds a Windows Job Object with JOB_OBJECT_LIMIT_PROCESS_MEMORY at 60% of physical RAM. That cap is not just an outer wall: UE reads it at startup into MemoryConstants.TotalVirtual, so FAssetCompilingManager throttles against it. It is PER-PROCESS, so ShaderCompileWorker children are not charged to the editor's budget. Rationale for the flags is unchanged - -RenderOffScreen (NEVER -NullRHI), -nocefaccelpaint (REQUIRED - without it CEF asserts under -unattended and crashes the suite at a random test), -ddc=InstalledNoZenLocalFallback (REQUIRED on this box - IPv6 loopback is refused machine-wide, so the editor cannot reach ZenServer and the default DDC graph comes up with no writable node, aborting startup with "Unable to use default cache graph 'InstalledDerivedDataBackendGraph'"), and the TestExit drain marker. Do NOT pass -log (it pops a console window). The -PinWright* switches are the in-process half of the guard (reset every 25 tests, forced reset at 0.55 of RAM, under the 0.60 cap) and are passed explicitly so they are visible here rather than buried in settings defaults. Confirm it started: the PID is alive and ${p.testLog} is growing; if not, the args were mis-quoted - fix and relaunch.
T2. BLOCK until it exits — set the tool timeout to the maximum and run: Wait-Process -Id <the PID> -Timeout 570. The PID is the LAUNCHER's; it holds the job handle, and JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE means killing it takes the editor and its shader workers with it. This BLOCKS (does not poll) up to 9.5 min and returns the instant the launcher exits. If it throws a timeout (suite still running), run the EXACT same Wait-Process again; repeat until it returns. NEVER background-and-wait-for-a-notification (a workflow subagent gets NO completion callback), NEVER end your turn to wait — keep actively blocking on Wait-Process.
T2b. MANDATORY, before you read one line of ${p.testLog}: read the launcher's one-line verdict, Get-Content "${p.suiteResult}". It reads PINWRIGHT_SUITE_RESULT verdict=<...> cap_gb=<...> peak_gb=<...> priority=<...> exit=<...> oom_alloc=<...> oom_backup_pool=<...> watermark_markers=<...> capSeenByEditor=<...> wall_min=<...> log=<...>. Branch on verdict, and keep the three failure verdicts DISTINCT - they have different causes and different next actions:
    - MEMORY_CAP_HIT -> this leg STOPS HERE. The editor walked into the per-process cap (peak_gb pinned at cap_gb, and/or oom_alloc/oom_backup_pool non-zero). The test counts in ${p.testLog} are NOT a result and MUST NOT be reported as one, and there is no failing test to fix. Return status BLOCKED, phase "test", reason quoting the full PINWRIGHT_SUITE_RESULT line with peak_gb against cap_gb, logPath ${p.testLog}. Do NOT re-run, do NOT raise -MemoryFraction to get past it, and do NOT report it as a timeout: it is a memory failure on THIS version, and either this version's editor genuinely needs more than 60% of the box or something in the suite grew.
    - TIMEOUT -> the launcher hit -TimeoutMinutes and terminated the job; the suite was still running. Different cause, different report: status BLOCKED, phase "test", reason naming the timeout, NOT the cap.
    - capSeenByEditor=False on any verdict -> the job limit did not reach the editor, so the run was effectively uncapped. Note it in 'summary'; it does not by itself fail the leg.
    - EDITOR_EXITED / EDITOR_EXIT_NONZERO -> the cap did not trip; proceed to T3 and grade the suite normally.
T3. Parse ${p.testLog} (UE rotates the prior run to a -backup-<timestamp>.log). A failing test has Result={Fail}; its region is between "BeginEvents: <FullTestPath>" and "EndEvents:". Also detect a crash (Fatal/assertion/ensure, or the log ending abruptly mid-suite). GREEN requires ALL of: every test Result={Success}, no crash, AND a sane test count actually ran (a PinWright full run is dozens-to-hundreds of tests — if near-zero ran and the editor still exited, the suite did not really execute). If the editor produced ZERO "Test Started" lines AND exited non-zero after you confirmed the ExecCmds quoting, automation never came up (cooked-asset / missing module / license) -> return status FATAL, phase "test", citing the test log.
T4. Otherwise fix the failures per test-fix-protocol.md - map each failing test to the .cpp containing its IMPLEMENT_*_AUTOMATION_TEST and fix; MOST fixes belong in PRODUCTION code, not the test. For a crash, fix the root cause; never catch/swallow it. While iterating you MAY re-run a single test fast (-ExecCmds="Automation RunTests <FullTestPath>,Quit" -TestExit="Automation Test Queue Empty") but you MUST end with one clean FULL-suite run before declaring green. After editing, go back to COMPILE (test fixes can change production code). If the SAME failing-test set persists after ~10 cycles, return status BLOCKED, phase "test".

== PACKAGE (after TEST is green) ==
P1. Run the Rocket packaging gate: ONE foreground, BLOCKING PowerShell tool call with the tool timeout at max (600000 ms). Do NOT background it, do NOT poll. No editor shutdown is needed (it builds a staged copy under the scratch dir, not the host plugin DLL):
    & "${p.packageScript}" -EngineRoot "${p.engine}" -OutputDir "${p.pkgOut}" -ScratchRoot "${p.scratchRoot}" -BuildLogPath "${p.packageLog}"${strict}${cver}
    If the TOOL reports a timeout (vs the command returning), re-run the EXACT same command. BuildPlugin does not resume, it rebuilds from scratch.
P2. On a script throw / non-zero exit: read the tail (~400 lines) of ${p.packageLog}. If it contains cl.exe/LNK error lines (strip the AutomationTool/UBT log prefix to recover the raw file.cpp(NN): error C#### shape), these are strict-includes / unity-hidden compile errors the Rocket build surfaced: fix them in ${p.clone}\\Source per compile-fix-protocol.md, go back to COMPILE, get the suite green again, then re-run PACKAGE. If it failed with NO compiler/linker error (a RunUAT infra failure, a .Build.cs C# rules-compiler error, or a package-fab staging assertion), that is unrecoverable from here: return status FATAL, phase "package", citing ${p.packageLog}. If the SAME error signature persists after ~10 attempts, return status BLOCKED, phase "package".
P3. On exit 0: scan ${p.packageLog} for plugin-attributable warnings, ZERO tolerance. A line GATES if ANY of: (1) a compiler warning line (": warning C####:") whose file path is under the scratch or stage plugin tree; (2) any "warning C4996" line OR any warning line containing "deprecated", even when the path is an engine header (C4996 IS the deprecation code and its message text may omit the word "deprecated", e.g. the MaterialTypes.h case; only plugin TUs compile in a BuildPlugin run, so an engine-header deprecation is triggered by a plugin include); (3) a UBT/descriptor warning naming Plugin 'PinWright'. EXCLUDE: note / message continuation lines; UHT-phase warnings unless they name a plugin header path; engine-path warnings that are neither C4996 nor deprecation-keyword. Dedupe repeats to distinct sites (warning code + symbol + plugin-relative path:line). If ZERO sites gate: set gatePassed true and go to FINISH. Otherwise fix each distinct site per package-fix-protocol.md, add every edited file (including PinWright.uplugin if touched) to touched, record the sites in packageWarnings, then go back to COMPILE (the fixes must re-pass the suite) and re-run PACKAGE. If the SAME site set persists past ~10 attempts, return status BLOCKED, phase "package", citing ${p.packageLog} and the stuck warning lines in excerpt.

== FINISH ==
F1. Leave the host editor DOWN. COMPILE stopped it to unlock the plugin DLL, and nothing after that
    needs it: this run never talks to the editor over MCP. A relaunched editor only holds RAM and CPU
    for every leg that follows - with a few piled up, the packaging gate was measured running at half
    parallelism and taking twice as long. Whoever wants an editor afterwards launches the one host
    they care about.
F2. Determine your outcome:
    - GREEN (a passing PYTEST run + clean compile + a final clean FULL-suite run + a clean packaging gate, gatePassed true): return status "OK", green:true, pyTestsPassed:true, tipBuiltAgainst, touched (the plugin source paths you edited, including any Content/Python connector file and PinWright.uplugin if touched; [] if it was already green), cyclesRun, gatePassed:true, packageLogPath:"${p.packageLog}", packageWarnings (the distinct sites you fixed, [] when the gate was clean first try), summary. Leave your edits UNCOMMITTED (a later phase commits them).
    - BLOCKED (stuck after the cap, in compile / test / package): discard your uncommitted edits so the clone is clean for the next version: git -C "${p.clone}" reset --hard then git -C "${p.clone}" clean -fd (NOT -x, keep Binaries/Intermediate; the reset also reverts any PinWright.uplugin edit). Return status "BLOCKED", green:false, tipBuiltAgainst, reason, phase, logPath, excerpt.
    - FATAL (unrecoverable, per above): do NOT reset — PRESERVE the tree for inspection (optionally git -C "${p.clone}" diff > a patch file under ${p.host}\\Saved). Return status "FATAL", green:false, reason, phase, logPath, excerpt.

${regressing ? 'NOTE: this version was already green at an earlier commit and is being RE-VERIFIED at a newer tip. If it now needs fixes, it regressed — fix it, but keep the edit version-guarded so it cannot regress yet another version.\n' : ''}Do NOT commit, do NOT push. Return the JSON described above.`;
}

function commitPrompt(p, bt) {
    return `In the plugin clone ${p.clone}: publish UE ${p.version}'s verified-green fix to origin/master. Run PowerShell. The build+test already passed and the fix is UNCOMMITTED in the working tree.

1. Enumerate the exact changed tracked files: git -C "${p.clone}" status --porcelain. Stage ONLY those exact source paths individually (git -C "${p.clone}" add "<path>" for each). The changed set may include the repo-root PinWright.uplugin (a packaging-gate descriptor fix); stage it by its exact path like the rest. NEVER git add -A / -a / . — stage the exact paths only. Binaries/Intermediate are gitignored and must not appear; if any do, something is wrong — stop and return FATAL.
2. If there is nothing to stage (the version was already green with no edits), return {status:'OK', committed:false, pushed:false, newTip:'${bt.tipBuiltAgainst || ''}', note:'already green, no changes'} and stop.
3. Commit: git -C "${p.clone}" commit -m "version-matrix: UE ${p.version} green @ ${(bt.tipBuiltAgainst || '').slice(0, 12)} — ${(bt.summary || 'compile/test/package fixes').replace(/"/g, "'").slice(0, 80)}".
4. Integrate origin: git -C "${p.clone}" pull --rebase origin master. On a single box this is normally a no-op fast-forward. If it CONFLICTS and you can reconcile it (keep both sides, strip markers, git add the resolved exact paths, git rebase --continue until done), do so. If a conflict is genuinely unreconcilable, return status "FATAL", reason describing it, and do NOT git push — never force-push master, never blindly --abort-and-drop work.
5. Push: git -C "${p.clone}" push origin master. If rejected because origin advanced, repeat step 4 then push again.
6. Read the new tip: git -C "${p.clone}" rev-parse origin/master.

Return {status:'OK', committed:true, pushed:true, newTip:'<sha>', note:'<files committed>'} on success, or {status:'FATAL', reason, logPath, excerpt} if git is in a state a human must reconcile.`;
}

function fabZipPrompt(p) {
    return `Post-fixpoint Fab packaging for **UE ${p.version}** of the PinWright plugin. This version is already green at the final tip; produce its Fab-ready zip. Run PowerShell. This is a STAGING-ONLY step: package-fab.ps1 stages + blocklist-validates the curated public file set and zips it. It does NOT compile, so it is cheap and cannot regress the green state, and it needs no editor shutdown.

Run ONE PowerShell call:
    & "${p.clone}\\scripts\\package-fab.ps1" -EngineVersion "${p.version}.0" -PackageName "PinWright-Fab-UE${p.version}" -OutputDir "${p.clone}\\dist"

On success the script writes the zip under ${p.clone}\\dist: return {status:'OK', zipPath:'<the .zip path>'}. On any throw / non-zero exit, return {status:'FAILED', reason:'<the error text>', logPath}. Do NOT attempt a fix and do NOT touch source: a Fab-staging failure here is reported only, it does not un-green the version.`;
}

// Build a terminal payload for a hard failure so the supervisor classifies it as
// Critical (stop, surface, do NOT relaunch) rather than a transient crash.
function fatal(version, where, info) {
    const i = info || {};
    log(`FATAL on UE ${version} (${where}): ${i.reason || i.note || 'unrecoverable'}`);
    return {
        stop_reason: 'fatal',
        fatalVersion: version,
        fatalPhase: i.phase || where,
        reason: i.reason || i.note || 'unrecoverable error',
        logPath: i.logPath || null,
        excerpt: i.excerpt || null,
        results,
        greenAtTip: { ...passedAtTip },
        blocked,
        currentTip,
    };
}

// --- State ---
const passedAtTip = {};   // version -> the tip SHA it is currently green at
const blocked = {};       // version -> reason (excluded from the must-match set)
const regressions = {};   // version -> times it needed fixes again at a newer tip
const results = [];
let currentTip = null;
let tipAdvances = 0;
let stopReason = 'fixpoint';

// --- Preflight ---
phase('Preflight');
const pre = await agentOrRetry(() => agent(preflightPrompt(), { schema: preflightSchema, label: 'preflight', model: MODEL }));
if (!pre) { return { stop_reason: 'agent_died', note: 'preflight agent died after retries — relaunch to resume', results }; }
if (pre.status === 'FATAL' || !pre.order || pre.order.length === 0) {
    return fatal('(preflight)', 'preflight', { reason: pre.reason || 'no eligible version', note: pre.note });
}
currentTip = pre.tip;
const order = pre.order;
const maxTipAdvances = order.length * 2;
log(`eligible: ${order.join(', ')} (primary ${PRIMARY}); tip ${String(currentTip).slice(0, 12)}` + (pre.skipped && pre.skipped.length ? `; skipped ${pre.skipped.map((s) => s.version).join(', ')}` : ''));

// --- Forward walk + fixpoint ---
// A version needs (re)work whenever it is not blocked and not green at the current
// tip. Re-queueing happens automatically when a commit advances the tip past a
// version that already passed (cross-version regression). Loop until every
// non-blocked version is green at one common tip.
while (true) {
    const todo = order.filter((v) => !blocked[v] && passedAtTip[v] !== currentTip);
    if (todo.length === 0) break;   // fixpoint reached

    for (const v of todo) {
        const p = paramsFor(v);
        const regressing = passedAtTip[v] !== undefined;   // was green before, now re-verifying at a newer tip
        phase(`UE ${v}`);

        const bt = await agentOrRetry(() => agent(buildTestPrompt(p, regressing), { schema: btSchema, label: `build-test:${v}`, model: MODEL }));
        if (!bt) { stopReason = 'agent_died'; log(`build-test agent for UE ${v} died after retries — stopping (relaunch resumes from git)`); break; }
        if (bt.status === 'FATAL') return fatal(v, 'build-test', bt);

        if (bt.status === 'BLOCKED' || !bt.green || bt.pyTestsPassed === false) {
            blocked[v] = bt.reason || 'stuck after the cycle cap';
            results.push({ version: v, state: 'BLOCKED', reason: blocked[v], phase: bt.phase, logPath: bt.logPath });
            log(`UE ${v}: BLOCKED — ${blocked[v]} (continuing; other versions are independent)`);
            continue;
        }

        // GREEN. Commit only if the agent actually changed source.
        const changed = Array.isArray(bt.touched) && bt.touched.length > 0;
        if (regressing && changed) {
            regressions[v] = (regressions[v] || 0) + 1;
            if (regressions[v] > MAX_REGRESSIONS) {
                return fatal(v, 'fixpoint', { reason: `UE ${v} regressed ${regressions[v]} times — cross-version ping-pong is not converging`, phase: 'fixpoint' });
            }
        }

        if (changed) {
            const cm = await agentOrRetry(() => agent(commitPrompt(p, bt), { schema: commitSchema, label: `commit:${v}`, model: MODEL }));
            if (!cm) { stopReason = 'agent_died'; log(`commit agent for UE ${v} died after retries — stopping (relaunch resumes from git)`); break; }
            if (cm.status === 'FATAL') return fatal(v, 'commit', cm);
            if (cm.pushed && cm.newTip && cm.newTip !== currentTip) {
                currentTip = cm.newTip;
                if (++tipAdvances > maxTipAdvances) {
                    return fatal(v, 'fixpoint', { reason: `tip advanced ${tipAdvances} times (> ${maxTipAdvances}) — fixpoint not converging`, phase: 'fixpoint' });
                }
            }
            results.push({ version: v, state: 'GREEN', committed: !!cm.committed, pushed: !!cm.pushed, tip: currentTip });
            log(`UE ${v}: green + pushed @ ${String(currentTip).slice(0, 12)}`);
        } else {
            results.push({ version: v, state: 'GREEN', committed: false, pushed: false, tip: currentTip });
            log(`UE ${v}: already green @ ${String(currentTip).slice(0, 12)} (no changes)`);
        }
        passedAtTip[v] = currentTip;
    }

    if (stopReason === 'agent_died') break;
}

const greenVersions = order.filter((v) => passedAtTip[v] === currentTip);

// --- Fab artifacts (post-fixpoint) ---
// For every version green at the final tip, stage a Fab-ready zip (staging only, no
// compile). Best-effort deliverables: a staging failure is reported in the return
// value but never flips a green version to blocked.
const fabZips = [];
if (EMIT_FAB_ZIPS && greenVersions.length > 0) {
    phase('Fab artifacts');
    for (const v of greenVersions) {
        const p = paramsFor(v);
        const fz = await agentOrRetry(() => agent(fabZipPrompt(p), { schema: fabZipSchema, label: `fabzip:${v}`, model: MODEL }));
        if (!fz) { fabZips.push({ version: v, status: 'FAILED', reason: 'fab-zip agent died after retries' }); continue; }
        if (fz.status === 'OK') {
            fabZips.push({ version: v, status: 'OK', zipPath: fz.zipPath });
            log(`UE ${v}: Fab zip staged -> ${fz.zipPath || '(path not reported)'}`);
        } else {
            fabZips.push({ version: v, status: 'FAILED', reason: fz.reason, logPath: fz.logPath });
            log(`UE ${v}: Fab zip FAILED (reported only, version stays green): ${fz.reason || 'unknown'}`);
        }
    }
}

return {
    stop_reason: stopReason,           // 'fixpoint' (all non-blocked green at one tip) | 'agent_died' (relaunch to resume)
    finalTip: currentTip,
    green: greenVersions,
    blocked,                            // versions that could not be greened (surfaced, not fatal)
    tipAdvances,
    results,
    fabZips,                            // per green version: staged Fab zip path or a reported failure
};
