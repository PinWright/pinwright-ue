# Copyright (c) 2026 Alexander Penkin. MIT License.
"""INTERNAL: the capped, detached supervisor behind mcp_proxy.py's editor_start, editor_restart,
editor_run_tests and editor_build. Not a public tool and it has no public CLI. Two internal entry
points:
    --supervise <spec.json>   the detached supervisor itself, which spawn_supervised() starts.
    --spawn <request.json>    multi-engine tooling ONLY (the mcp-version-matrix skill): builds and
                              suites on OTHER engines' host projects, which the proxy tools cannot
                              reach because they are bound to the MCP client's own project and its
                              EngineAssociation. See spawn_request() for the request contract.

Stdlib only, Win64 + Linux. Replaces the removed PowerShell scripts (Run-Capped.ps1,
Run-SuiteCapped.ps1, CappedJob.ps1) and the pinwright_launch.py CLI.

WHY EACH EDITOR RUNS (launch identity)
    Every editor PinWright launches carries two switches on its own command line:
        -PinWrightLaunchReason=<percent-encoded reason>   -PinWrightLaunchedBy=<tool id>
    Tool ids: editor_start, editor_restart, editor_run_tests. There are no identity side files:
    the reason, the tool and (via -Abslog) the log are read back from the live process's
    command line, and the process start time pins the pid against reuse (process_start_ms).
    Encoding: only '%' -> %25 and '"' -> %22. UE's FParse::Value ends a quoted value at the first
    '"' with no escape (Core/Private/Misc/Parse.cpp), and UE rebuilds its command line from argv
    on both platforms, re-wrapping a `-Key=value with spaces` element as `-Key="value with spaces"`
    (Launch/Private/Windows/LaunchWindows.cpp ProcessCommandLine via CommandLineToArgvW;
    Unix/UnixCommonStartup/Private/UnixCommonStartup.cpp). So each switch is ONE argv element with
    no quotes of ours; subprocess does the OS quoting. Editors launched by non-PinWright tooling
    (Epic Games Launcher, IDE, a bare shell) carry no switch and therefore no reason.

SUPERVISOR
    spawn_supervised() starts `python pinwright_supervisor.py --supervise <spec.json>` so that it
    outlives the MCP proxy and the MCP client:
        Windows  WMI Win32_Process.Create (through powershell Invoke-CimMethod), with
                 DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP and a hidden window. WmiPrvSE is the
                 parent, so the supervisor is neither a parent-pid descendant of the proxy nor in
                 its job. A direct child is not enough, whatever its flags: DETACHED_PROCESS and
                 CREATE_BREAKAWAY_FROM_JOB leave the parent pid in place, and a client that tears
                 down its tree with `taskkill /T` (Claude Code does) killed a 20-minute suite with
                 its supervisor when the session's process exited (B-supervisor-dies-with-mcp-client).
                 If WMI fails, the supervisor is started as a direct child (breakaway attempted)
                 and the run reports detached=False with the reason.
        Linux    a direct child in a new session (setsid), out of the caller's process group.
    A Windows visible editor_start comes through here too, capped=False at Normal priority: no
    job, so the editor outlives even the supervisor; the supervisor exists only for the WMI launch.
    tests/test_pinwright_supervisor.py TreeKillSurvivalTest reproduces the kill for real
    (taskkill /T on a stand-in caller) and its counterfactual (the direct child dies).
    The result carries launchMechanism (wmi-win32-process-create | createprocess-breakaway |
    createprocess-in-caller-job | setsid), detached and detachNote. The spec is a JSON file in
    <tempdir>/pinwright-supervisor/<runId>/ (it carries the child's environment, always explicit
    because a WMI-started process gets the user's default environment; the supervisor deletes it
    once read); the supervisor answers with ONE JSON document, handoff.json beside it (child pid +
    start time, or {"error": ...}). The spec carries protocolVersion (PROTOCOL_VERSION): the proxy
    keeps this module in memory but launches the file on disk, so a proxy started before an edit
    can launch a newer script. A mismatch, or the protocol-1 argv (`--supervise` alone, spec on
    stdin), exits EXIT_VERSION_MISMATCH with a SUPERVISOR_VERSION_MISMATCH line in the log and in
    the handoff (stdout for protocol 1), which the proxy reports as that error code: restart the
    MCP server. Later diagnostics go to <base>supervisor.log, the verdict to
    <base>result.txt:
        log_path given    -> <log>.result.txt, <log>.supervisor.log
        output_path given -> <output>.result.txt, <output>.supervisor.log
        neither           -> <tempdir>/pinwright-supervisor/<runId>/result.txt, supervisor.log
    result.txt holds one line (invariant-culture decimals, i.e. always '.'):
        PINWRIGHT_SUITE_RESULT verdict=.. cap_gb=.. peak_gb=.. priority=.. exit=.. oom_alloc=..
            oom_backup_pool=.. watermark_markers=.. capSeenByEditor=.. wall_min=.. log=..
        PINWRIGHT_JOB_RESULT verdict=.. exit=.. priority=.. cap_gb=.. peak_gb=.. wall_min=..
            command=.. output=..
    Verdict ladder: TIMEOUT > MEMORY_CAP_HIT > {EDITOR|COMMAND}_EXIT_NONZERO > {..}_EXITED.
    The suite verdict authority stays
    check_suite_log.check_log(); this module never classifies a suite.

PLATFORM MECHANISMS (capped=True; capped=False = Normal priority, no cap, no kill-on-exit)
    memory cap   Windows: Job Object JOB_OBJECT_LIMIT_PROCESS_MEMORY = floor(totalPhys*fraction)
                          PER PROCESS (shader workers get their own ceiling). UE reads it at
                          startup (WindowsPlatformMemory.cpp) and logs it; the child is created
                          CREATE_SUSPENDED, assigned, then resumed, so the cap precedes its first
                          instruction. A hit makes allocations FAIL (UE's OOM log lines).
                 Linux:   `systemd-run --user --scope --quiet -p MemoryMax=<bytes> -- argv`
                          when a probe of it succeeds and the scope's memory.max reads back.
                          The cap is PER SCOPE = the whole process tree, unlike Windows. A hit
                          invokes the cgroup OOM killer (memory.events oom_kill). Never
                          RLIMIT_AS: UE reserves huge virtual ranges. Unavailable -> uncapped,
                          with uncappedReason in the handoff and supervisor.log.
    priority     Windows: JOB_OBJECT_LIMIT_PRIORITY_CLASS (Normal 0x20, BelowNormal 0x4000,
                          Idle 0x40), job-wide, a job process cannot raise itself above it.
                 Linux:   nice (Normal 0, BelowNormal 10, Idle 19) set in preexec, inherited.
                 BelowNormal is the default, not Idle: an Idle run starves whenever anything else
                 wants CPU and its wall time stops meaning anything.
    kill-on-exit Windows: JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; timeout -> TerminateJobObject.
                 Linux:   PR_SET_PDEATHSIG(SIGKILL) + child in its own process group;
                          timeout and exit -> killpg(SIGKILL) for stragglers.
    peak         Windows: JobObjectExtendedLimitInformation.PeakProcessMemoryUsed.
                 Linux:   the scope's memory.peak (polled; 0 when unreadable/uncapped).
"""

import ctypes
import functools
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import uuid

START_MATCH_TOLERANCE_MS = 2000
REASON_MAX_CHARS = 300
LAUNCH_REASON_SWITCH = "-PinWrightLaunchReason="
LAUNCHED_BY_SWITCH = "-PinWrightLaunchedBy="
KINDS = ("editor", "suite", "command")
# Launch modes, the same three words on every proxy launch tool:
#   visible   - a normal window, real RHI, dialogs shown
#   offscreen - real RHI, no OS window (-RenderOffScreen: FNullPlatformApplicationMisc on Windows
#               and Linux, SDL 'dummy' video driver on Linux), modal-suppressed
#   headless  - no GPU rendering (-NullRHI) plus -RenderOffScreen, which is what removes the OS
#               window and the Linux display requirement; -NullRHI alone keeps a real platform
#               application (WindowsPlatformApplicationMisc.cpp / LinuxPlatformApplicationMisc.cpp
#               CreateApplication), so a Windows window and a Linux X display would still be needed.
MODES = ("visible", "offscreen", "headless")
PRIORITIES = ("Normal", "BelowNormal", "Idle")
PRIORITY_CLASS = {"Normal": 0x20, "BelowNormal": 0x4000, "Idle": 0x40}
NICE = {"Normal": 0, "BelowNormal": 10, "Idle": 19}
_GIB = 1024 ** 3

# Win32 constants (winnt.h / winbase.h values).
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
STILL_ACTIVE = 259
JOB_OBJECT_LIMIT_PRIORITY_CLASS = 0x00000020
JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x00000100
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_CLASS = 9
JOB_OBJECT_LIMIT_VIOLATION_INFORMATION_CLASS = 13
CREATE_SUSPENDED = 0x00000004
DETACHED_PROCESS = 0x00000008
CREATE_NEW_PROCESS_GROUP = 0x00000200
CREATE_BREAKAWAY_FROM_JOB = 0x01000000
CREATE_NO_WINDOW = 0x08000000
# Linux
PR_SET_PDEATHSIG = 1
SIGKILL = 9
SIGTERM = 15


# ------------------------------------------------------------------------------------------------
# Launch identity (pure)
# ------------------------------------------------------------------------------------------------
def normalize_reason(value):
    """(reason, error): whitespace runs collapse to one space; blank or over-long is refused."""
    if not isinstance(value, str):
        return None, "reason must be a string, got %s." % type(value).__name__
    reason = re.sub(r"\s+", " ", value).strip()
    if not reason:
        return None, "reason must not be empty or whitespace."
    if len(reason) > REASON_MAX_CHARS:
        return None, "reason is %d characters; the limit is %d." % (len(reason), REASON_MAX_CHARS)
    return reason, None


def encode_reason(reason):
    return reason.replace("%", "%25").replace('"', "%22")


def decode_reason(text):
    return re.sub(r"%(22|25)", lambda m: '"' if m.group(1) == "22" else "%", text)


def launch_identity_args(reason, launched_by):
    return [LAUNCH_REASON_SWITCH + encode_reason(reason), LAUNCHED_BY_SWITCH + launched_by]


def parse_launch_identity(argv):
    """(reason, launched_by) from an argv list; first occurrence of each switch wins."""
    reason = launched_by = None
    for arg in argv or ():
        lower = arg.lower()
        if reason is None and lower.startswith(LAUNCH_REASON_SWITCH.lower()):
            reason = decode_reason(arg[len(LAUNCH_REASON_SWITCH):])
        elif launched_by is None and lower.startswith(LAUNCHED_BY_SWITCH.lower()):
            launched_by = arg[len(LAUNCHED_BY_SWITCH):]
    return reason, launched_by


def _with_identity(argv, reason, launched_by):
    """Append the identity switches unless argv already carries them (idempotent)."""
    have_reason, have_by = parse_launch_identity(argv)
    extra = launch_identity_args(reason, launched_by)
    return list(argv) + ([extra[0]] if have_reason is None else []) + ([extra[1]] if have_by is None else [])


def split_windows_command_line(text):
    """argv per CommandLineToArgvW: the program name ends at the next quote (quoted) or
    whitespace; after it, 2n backslashes + quote -> n backslashes and a quote toggle, 2n+1 -> n
    backslashes and a literal quote, other backslashes are literal, `""` inside quotes -> `"`."""
    argv, i, n = [], 0, len(text)
    while i < n and text[i] in " \t":
        i += 1
    if i >= n:
        return argv
    if text[i] == '"':
        end = text.find('"', i + 1)
        end = n if end < 0 else end
        argv.append(text[i + 1:end])
        i = end + 1
    else:
        start = i
        while i < n and text[i] not in " \t":
            i += 1
        argv.append(text[start:i])
    buf, have, in_quotes = [], False, False
    while i < n:
        c = text[i]
        if c in " \t" and not in_quotes:
            if have:
                argv.append("".join(buf))
                buf, have = [], False
            i += 1
        elif c == "\\":
            j = i
            while j < n and text[j] == "\\":
                j += 1
            count = j - i
            if j < n and text[j] == '"':
                buf.append("\\" * (count // 2))
                if count % 2:
                    buf.append('"')
                    j += 1
            else:
                buf.append("\\" * count)
            i, have = j, True
        elif c == '"':
            have = True
            if in_quotes and i + 1 < n and text[i + 1] == '"':
                buf.append('"')
                i += 2
            else:
                in_quotes = not in_quotes
                i += 1
        else:
            buf.append(c)
            have = True
            i += 1
    if have:
        argv.append("".join(buf))
    return argv


def infer_mode(argv):
    """commandlet / game / headless / offscreen / visible, from the editor argv (case-insensitive).
    What the process IS wins over how it renders, so commandlet and game are checked first."""
    lower = [a.lower() for a in argv]
    if any(a.startswith("-run=") for a in lower):
        return "commandlet"
    if "-game" in lower:
        return "game"
    if "-nullrhi" in lower:
        return "headless"
    if any(a in ("-renderoffscreen", "-unattended") for a in lower):
        return "offscreen"
    return "visible"


# ------------------------------------------------------------------------------------------------
# Suite argv
# ------------------------------------------------------------------------------------------------
def suite_executable(editor_exe, mode):
    """visible -> the GUI editor. offscreen / headless -> Windows' -Cmd console twin (error when absent);
    on Linux the plain UnrealEditor binary: the twin is a Windows subsystem hack (TargetRules.cs
    bBuildAdditionalConsoleApp doc) and UAT's LinuxHostPlatform.GetUnrealExePath maps -Cmd back
    onto the plain binary."""
    if mode == "visible" or os.name != "nt":
        return editor_exe
    import mcp_proxy  # lazy: mcp_proxy imports this module at its top
    cmd = mcp_proxy._editor_cmd_from_editor(editor_exe)
    if not os.path.isfile(cmd):
        raise FileNotFoundError("an offscreen or headless suite needs the -Cmd console editor, not found: %s" % cmd)
    return cmd


def suite_argv(uproject, test_filter, log_path, mode, report_dir=None, extra_args=()):
    """Suite launch argv (without the executable). visible and offscreen use a real RHI; headless
    adds -NullRHI, so renderer-dependent tests cannot measure anything there."""
    argv = [uproject,
            "-ExecCmds=Automation RunTests %s,Quit" % test_filter,
            "-TestExit=Automation Test Queue Empty",
            "-unattended", "-nopause", "-nosplash", "-nosound"]
    if mode == "headless":
        argv.append("-NullRHI")
    if mode != "visible":
        argv.append("-RenderOffscreen")
    argv += ["-nocefaccelpaint", "-RunningUnattendedScript", "-ddc=InstalledNoZenLocalFallback"]
    if report_dir:
        argv.append("-ReportExportPath=%s" % report_dir)
    argv.append("-Abslog=%s" % log_path)
    return argv + list(extra_args)


# ------------------------------------------------------------------------------------------------
# Win32 interop
# ------------------------------------------------------------------------------------------------
class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64),
                ("PerJobUserTimeLimit", ctypes.c_int64),
                ("LimitFlags", ctypes.c_uint32),
                ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t),
                ("ActiveProcessLimit", ctypes.c_uint32),
                ("Affinity", ctypes.c_size_t),
                ("PriorityClass", ctypes.c_uint32),
                ("SchedulingClass", ctypes.c_uint32)]


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in (
        "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
        "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]


class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION),
                ("IoInfo", IO_COUNTERS),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t)]


class JOBOBJECT_LIMIT_VIOLATION_INFORMATION(ctypes.Structure):
    _fields_ = [("LimitFlags", ctypes.c_uint32), ("ViolationLimitFlags", ctypes.c_uint32),
                ("IoReadBytes", ctypes.c_uint64), ("IoReadBytesLimit", ctypes.c_uint64),
                ("IoWriteBytes", ctypes.c_uint64), ("IoWriteBytesLimit", ctypes.c_uint64),
                ("PerJobUserTime", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                ("JobMemory", ctypes.c_uint64), ("JobMemoryLimit", ctypes.c_uint64),
                ("RateControlTolerance", ctypes.c_uint32),
                ("RateControlToleranceLimit", ctypes.c_uint32)]


class MEMORYSTATUSEX(ctypes.Structure):
    _fields_ = [("dwLength", ctypes.c_uint32), ("dwMemoryLoad", ctypes.c_uint32)] + [
        (name, ctypes.c_uint64) for name in (
            "ullTotalPhys", "ullAvailPhys", "ullTotalPageFile", "ullAvailPageFile",
            "ullTotalVirtual", "ullAvailVirtual", "ullAvailExtendedVirtual")]


class FILETIME(ctypes.Structure):
    _fields_ = [("dwLowDateTime", ctypes.c_uint32), ("dwHighDateTime", ctypes.c_uint32)]


@functools.lru_cache(maxsize=None)
def _kernel32():
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    vp, u32 = ctypes.c_void_p, ctypes.c_uint32
    k.OpenProcess.restype = vp
    k.OpenProcess.argtypes = (u32, ctypes.c_int, u32)
    k.CreateJobObjectW.restype = vp
    k.CreateJobObjectW.argtypes = (vp, ctypes.c_wchar_p)
    k.SetInformationJobObject.argtypes = (vp, ctypes.c_int, vp, u32)
    k.QueryInformationJobObject.argtypes = (vp, ctypes.c_int, vp, u32, vp)
    k.AssignProcessToJobObject.argtypes = (vp, vp)
    k.TerminateJobObject.argtypes = (vp, u32)
    k.GetExitCodeProcess.argtypes = (vp, ctypes.POINTER(u32))
    k.GetProcessTimes.argtypes = (vp,) + (ctypes.POINTER(FILETIME),) * 4
    k.CloseHandle.argtypes = (vp,)
    k.GlobalMemoryStatusEx.argtypes = (ctypes.POINTER(MEMORYSTATUSEX),)
    return k


@functools.lru_cache(maxsize=None)
def _ntdll():
    n = ctypes.WinDLL("ntdll")
    n.NtResumeProcess.argtypes = (ctypes.c_void_p,)
    n.NtResumeProcess.restype = ctypes.c_long
    return n


def _last_error():
    return getattr(ctypes, "get_last_error", lambda: 0)()


def job_limits(cap_bytes, priority):
    """The JOBOBJECT_EXTENDED_LIMIT_INFORMATION the supervisor sets: per-process cap,
    kill-on-close, job-wide priority class."""
    info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    info.BasicLimitInformation.LimitFlags = (JOB_OBJECT_LIMIT_PROCESS_MEMORY
                                             | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                                             | JOB_OBJECT_LIMIT_PRIORITY_CLASS)
    info.BasicLimitInformation.PriorityClass = PRIORITY_CLASS[priority]
    info.ProcessMemoryLimit = cap_bytes
    return info


def _win_start_in_job(command_line, cap_bytes, priority, creationflags, stdout, stderr, env=None):
    """Create job -> set limits -> CreateProcess SUSPENDED -> assign -> resume. Returns (proc, job)."""
    k32 = _kernel32()
    job = k32.CreateJobObjectW(None, None)
    if not job:
        raise OSError(_last_error(), "CreateJobObject failed")
    info = job_limits(cap_bytes, priority)
    if not k32.SetInformationJobObject(job, JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_CLASS,
                                       ctypes.byref(info), ctypes.sizeof(info)):
        err = _last_error()
        k32.CloseHandle(job)
        raise OSError(err, "SetInformationJobObject failed")
    try:
        # Subprocess closes the primary-thread handle, so the resume is NtResumeProcess on the
        # process handle rather than ResumeThread.
        proc = subprocess.Popen(command_line, creationflags=creationflags | CREATE_SUSPENDED,
                                stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                close_fds=True, env=env)
    except OSError:
        k32.CloseHandle(job)
        raise
    handle = int(proc._handle)
    if not k32.AssignProcessToJobObject(job, handle):
        err = _last_error()
        proc.kill()
        k32.CloseHandle(job)
        raise OSError(err, "AssignProcessToJobObject failed")
    status = _ntdll().NtResumeProcess(handle)
    if status:
        proc.kill()
        k32.CloseHandle(job)
        raise OSError(status, "NtResumeProcess failed")
    return proc, job


def _win_job_peak_and_violation(job):
    """(peakBytes, violationIsMemory, violationFlags text) -- read after the child exits."""
    k32 = _kernel32()
    info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    peak = info.PeakProcessMemoryUsed if k32.QueryInformationJobObject(
        job, JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_CLASS, ctypes.byref(info),
        ctypes.sizeof(info), None) else 0
    violation = JOBOBJECT_LIMIT_VIOLATION_INFORMATION()
    if k32.QueryInformationJobObject(job, JOB_OBJECT_LIMIT_VIOLATION_INFORMATION_CLASS,
                                     ctypes.byref(violation), ctypes.sizeof(violation), None):
        flags = violation.ViolationLimitFlags
        return peak, bool(flags & JOB_OBJECT_LIMIT_PROCESS_MEMORY), "0x%08X" % flags
    # A hard-limit job may legitimately not populate the violation record.
    return peak, False, "unavailable (hard limits do not populate the violation record)"


def _windows_command_line(argv):
    """A .bat/.cmd is not a PE image: host it in cmd /S /c, which strips ONLY the outer quote
    pair and keeps embedded quotes intact. Arguments are not escaped for cmd metacharacters."""
    if os.path.splitext(argv[0])[1].lower() in (".bat", ".cmd"):
        shell = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32",
                             "cmd" + os.extsep + "exe")
        return '"%s" /S /c "%s"' % (shell, subprocess.list2cmdline(argv))
    return subprocess.list2cmdline(argv)


# ------------------------------------------------------------------------------------------------
# Process identity + host memory
# ------------------------------------------------------------------------------------------------
def process_start_ms(pid):
    """Creation time (Unix epoch ms, UTC) of a LIVE process, else None."""
    if not pid or pid <= 0:
        return None
    if os.name == "nt":
        return _win_process_start_ms(pid)
    return proc_start_ms(pid)


def _win_process_start_ms(pid):
    k32 = _kernel32()
    handle = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return None
    try:
        code = ctypes.c_uint32()
        if not k32.GetExitCodeProcess(handle, ctypes.byref(code)) or code.value != STILL_ACTIVE:
            return None
        times = [FILETIME() for _ in range(4)]
        if not k32.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
            return None
        ticks = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        return ticks // 10000 - 11644473600000  # 100 ns since 1601 -> ms since 1970
    finally:
        k32.CloseHandle(handle)


def proc_start_ms(pid, proc_root="/proc", clk_tck=None):
    """Linux: btime + starttime (field 22 of /proc/<pid>/stat) / SC_CLK_TCK, in ms. A zombie
    counts as gone."""
    try:
        with open(os.path.join(proc_root, "stat")) as fh:
            btime = next(int(line.split()[1]) for line in fh if line.startswith("btime "))
        with open(os.path.join(proc_root, str(pid), "stat")) as fh:
            data = fh.read()
        fields = data[data.rindex(")") + 2:].split()  # fields[0] is field 3 (state)
        if fields[0] == "Z":
            return None
        ticks = int(fields[19])
    except (OSError, StopIteration, ValueError, IndexError):
        return None
    tck = clk_tck or os.sysconf("SC_CLK_TCK")
    return btime * 1000 + ticks * 1000 // tck


def _matches(pid, start_ms):
    live = process_start_ms(pid)
    return live is not None and start_ms is not None and abs(live - start_ms) <= START_MATCH_TOLERANCE_MS


def total_physical_bytes():
    if os.name == "nt":
        status = MEMORYSTATUSEX()
        status.dwLength = ctypes.sizeof(status)
        if not _kernel32().GlobalMemoryStatusEx(ctypes.byref(status)):
            raise OSError(_last_error(), "GlobalMemoryStatusEx failed")
        return status.ullTotalPhys
    return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")


def cap_bytes_for(total_bytes, fraction):
    return int(math.floor(total_bytes * fraction))


# ------------------------------------------------------------------------------------------------
# Linux cap / priority / kill-on-exit
# ------------------------------------------------------------------------------------------------
def _libc():
    return ctypes.CDLL(None, use_errno=True)


def linux_preexec(nice_value, pdeathsig, parent_pid):
    """preexec_fn: own process group (timeout killpg), nice, and SIGKILL when the supervisor dies.
    prctl is resolved before the fork, so the child only calls it."""
    prctl = _libc().prctl if pdeathsig else None

    def preexec():
        os.setpgid(0, 0)
        if nice_value:
            os.nice(nice_value)
        if prctl is not None:
            prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0)
            if os.getppid() != parent_pid:  # supervisor died before prctl took effect
                os._exit(1)
    return preexec


def systemd_scope_prefix(cap_bytes, run=subprocess.run, which=shutil.which):
    """(prefix, None) when a probe scope with MemoryMax starts, else (None, reason)."""
    exe = which("systemd-run")
    if not exe:
        return None, "systemd-run not found on PATH"
    prefix = [exe, "--user", "--scope", "--quiet", "-p", "MemoryMax=%d" % cap_bytes, "--"]
    try:
        probe = run(prefix + ["true"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE, timeout=15)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return None, "systemd-run probe failed: %s" % exc
    if probe.returncode != 0:
        detail = (probe.stderr or b"").decode("utf-8", "replace").strip()[:300]
        return None, "systemd-run probe exited %d: %s" % (probe.returncode, detail)
    return prefix, None


def linux_cgroup_dir(pid, proc_root="/proc", cgroup_root="/sys/fs/cgroup"):
    """The cgroup v2 directory of pid, or None (v1-only host, or pid gone)."""
    try:
        with open(os.path.join(proc_root, str(pid), "cgroup")) as fh:
            for line in fh:
                if line.startswith("0::"):
                    return os.path.join(cgroup_root, line[3:].strip().lstrip("/"))
    except OSError:
        pass
    return None


def _read_text(path):
    try:
        with open(path) as fh:
            return fh.read().strip()
    except OSError:
        return None


def linux_wait_for_scope(pid, cap_bytes, timeout=10.0, proc_root="/proc", cgroup_root="/sys/fs/cgroup"):
    """(scopeDir, None) once pid sits in a .scope whose memory.max is the cap; else (None, reason).
    The kernel stores the limit in pages, so the readback may be up to a page short."""
    deadline = time.monotonic() + timeout
    while True:
        directory = linux_cgroup_dir(pid, proc_root, cgroup_root)
        if directory and directory.endswith(".scope"):
            value = _read_text(os.path.join(directory, "memory.max"))
            if value and value.isdigit() and abs(int(value) - cap_bytes) < 1024 * 1024:
                return directory, None
        if time.monotonic() >= deadline:
            return None, ("scope memory.max never read back as %d (cgroup %s); the memory "
                          "controller is likely not delegated to the user manager"
                          % (cap_bytes, directory))
        time.sleep(0.2)


def linux_scope_usage(directory):
    """(memory.peak bytes or 0, oom_kill count) of a scope."""
    peak = _read_text(os.path.join(directory, "memory.peak")) if directory else None
    events = _read_text(os.path.join(directory, "memory.events")) if directory else None
    kills = 0
    for line in (events or "").splitlines():
        if line.startswith("oom_kill "):
            kills = int(line.split()[1])
    return (int(peak) if peak and peak.isdigit() else 0), kills


# ------------------------------------------------------------------------------------------------
# Log scan, verdict, result line
# ------------------------------------------------------------------------------------------------
_PATH_RE = re.compile(r"Path=\{([^}]*)\}")
_CAP_SEEN = ("Detected a per-process memory limit of",
             "running as part of a Windows Job with separate resource limits")


def _scan_log(path):
    """One streaming pass: test progress plus the memory evidence of an out-of-memory run."""
    scan = {"exists": False, "started": 0, "succeeded": 0, "failed": 0, "lastTest": None,
            "oomAlloc": 0, "oomBackupPool": 0, "watermarkMarkers": 0,
            "capSeenByEditor": None, "memoryTotalLine": None}
    last = None
    try:
        fh = open(path, encoding="utf-8", errors="replace")
    except OSError:
        return scan
    with fh:
        scan["exists"] = True
        for line in fh:
            if "Test Started" in line:
                scan["started"] += 1
                last = line
            if "Result={Success}" in line:
                scan["succeeded"] += 1
            if "Result={Fail}" in line:
                scan["failed"] += 1
            if "Ran out of memory allocating" in line:
                scan["oomAlloc"] += 1
            if "from backup pool to handle out of memory" in line:
                scan["oomBackupPool"] += 1
            if "PINWRIGHT_MEMORY_WATERMARK_EXCEEDED" in line:
                scan["watermarkMarkers"] += 1
            if scan["capSeenByEditor"] is None and any(s in line for s in _CAP_SEEN):
                scan["capSeenByEditor"] = line.strip()
            if scan["memoryTotalLine"] is None and "Memory total: Physical=" in line:
                scan["memoryTotalLine"] = line.strip()
    if last is not None:
        match = _PATH_RE.search(last)
        scan["lastTest"] = match.group(1) if match else last.strip()
    return scan


def scan_test_progress(log_path):
    scan = _scan_log(log_path)
    return {key: scan[key] for key in ("exists", "started", "succeeded", "failed", "lastTest")}


def is_cap_hit(cap_bytes, peak_bytes, violation_is_memory, oom_alloc=0, oom_backup_pool=0):
    """Allocation-failure lines, a memory violation, or a peak pinned within 2% of the cap."""
    return (oom_alloc > 0 or oom_backup_pool > 0 or violation_is_memory
            or (cap_bytes > 0 and peak_bytes > 0 and peak_bytes >= int(cap_bytes * 0.98)))


def verdict_for(kind, timed_out, cap_hit, exit_code):
    prefix = "COMMAND" if kind == "command" else "EDITOR"
    if timed_out:
        return "TIMEOUT"
    if cap_hit:
        return "MEMORY_CAP_HIT"
    return prefix + ("_EXIT_NONZERO" if exit_code != 0 else "_EXITED")


def _gb(value_bytes):
    """Invariant decimals like [math]::Round(x,2).ToString(InvariantCulture): '37.91', '38'."""
    text = "%.2f" % round(value_bytes / _GIB, 2)
    return text.rstrip("0").rstrip(".")


def format_suite_result(verdict, cap_bytes, peak_bytes, priority, exit_code, scan, wall_seconds, log_path):
    return ("PINWRIGHT_SUITE_RESULT verdict=%s cap_gb=%s peak_gb=%s priority=%s exit=%d "
            "oom_alloc=%d oom_backup_pool=%d watermark_markers=%d capSeenByEditor=%s "
            "wall_min=%d log=%s" % (
                verdict, _gb(cap_bytes), _gb(peak_bytes), priority, exit_code, scan["oomAlloc"],
                scan["oomBackupPool"], scan["watermarkMarkers"], bool(scan["capSeenByEditor"]),
                int(round(wall_seconds / 60.0)), log_path))


def format_job_result(verdict, exit_code, priority, cap_bytes, peak_bytes, wall_seconds, command, output):
    return ("PINWRIGHT_JOB_RESULT verdict=%s exit=%d priority=%s cap_gb=%s peak_gb=%s "
            "wall_min=%d command=%s output=%s" % (
                verdict, exit_code, priority, _gb(cap_bytes), _gb(peak_bytes),
                int(round(wall_seconds / 60.0)), command, output or "<none>"))


def _write_atomic(path, text):
    tmp = "%s.%s.tmp" % (path, uuid.uuid4().hex)
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(text)
    os.replace(tmp, path)


def read_result(result_path):
    """(line, verdict, exitCode) from a result file, or (None, None, None) before it exists."""
    try:
        with open(result_path, encoding="utf-8-sig") as fh:
            line = fh.readline().strip()
    except OSError:
        return None, None, None
    verdict = re.search(r"\bverdict=(\S+)", line)
    code = re.search(r"\bexit=(-?\d+)", line)
    if not code:
        return None, None, None
    return line, verdict.group(1) if verdict else None, int(code.group(1))


def read_started_pid(supervisor_log_path):
    """The supervised child's pid from the last `started pid N:` line supervise() wrote, or None.

    Read from supervisor.log rather than result.txt because it is written at start, so a run whose
    supervisor died before writing a verdict still names its editor. Last line wins: the log is
    appended across runs that reuse one log path, and -Abslog truncates the log for the newest.
    """
    try:
        with open(supervisor_log_path, encoding="utf-8", errors="replace") as fh:
            pids = re.findall(r"\] started pid (\d+):", fh.read())
    except OSError:
        return None
    return int(pids[-1]) if pids else None


# ------------------------------------------------------------------------------------------------
# Supervisor (runs detached: `python pinwright_supervisor.py --supervise <spec.json>`)
# ------------------------------------------------------------------------------------------------
def _say(message):
    sys.stderr.write("[%s] %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), message))
    sys.stderr.flush()


class _Child:
    """The supervised process plus whatever contains it (Windows job / Linux scope + group)."""

    def __init__(self, proc, job=None, scope=None, capped=False, mechanism=None, uncapped_reason=None):
        self.proc, self.job, self.scope = proc, job, scope
        self.capped, self.mechanism, self.uncapped_reason = capped, mechanism, uncapped_reason

    def kill(self):
        if os.name == "nt":
            if self.job:
                _kernel32().TerminateJobObject(self.job, 1)
            else:
                self.proc.kill()
        else:
            self.kill_group()

    def kill_group(self):
        try:
            os.killpg(self.proc.pid, SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


def _start_child(spec):
    argv = spec["argv"]
    visible = spec["mode"] == "visible"
    out = open(spec["outputPath"], "wb") if spec.get("outputPath") else subprocess.DEVNULL
    if spec.get("outputPath") and spec.get("outputHeader"):
        # Lines the caller wants at the top of the output (e.g. why a build was started), since
        # the command itself may not accept them on its command line.
        out.write((spec["outputHeader"].rstrip("\n") + "\n").encode("utf-8"))
        out.flush()
    err = subprocess.STDOUT if spec.get("outputPath") else subprocess.DEVNULL
    capped = spec["capped"]
    env = spec.get("env")
    if os.name == "nt":
        command_line = _windows_command_line(argv)
        flags = CREATE_NEW_PROCESS_GROUP | (0 if visible else CREATE_NO_WINDOW)
        if capped:
            proc, job = _win_start_in_job(command_line, spec["capBytes"], spec["priority"], flags, out, err, env)
            return _Child(proc, job=job, capped=True, mechanism="windows-job-object"), command_line
        proc = subprocess.Popen(command_line, creationflags=flags, stdin=subprocess.DEVNULL,
                                stdout=out, stderr=err, close_fds=True, env=env)
        return _Child(proc, uncapped_reason="capped=False requested"), command_line
    if shutil.which(argv[0]) is None:
        # Under systemd-run the Popen below starts systemd-run, not argv[0], so a missing
        # executable would otherwise hand off as a started child and surface only as exit 1.
        raise FileNotFoundError(2, "No such executable file", argv[0])
    prefix, reason = (systemd_scope_prefix(spec["capBytes"]) if capped
                      else (None, "capped=False requested"))
    cmd = (prefix or []) + argv
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=out, stderr=err, close_fds=True,
                            env=env, preexec_fn=linux_preexec(NICE[spec["priority"]] if capped else 0,
                                                     capped, os.getpid()))
    scope = None
    if prefix:
        scope, reason = linux_wait_for_scope(proc.pid, spec["capBytes"])
    command_line = subprocess.list2cmdline(cmd)
    if scope:
        return _Child(proc, scope=scope, capped=True, mechanism="systemd-run-scope"), command_line
    return _Child(proc, uncapped_reason=reason), command_line


def _handoff(spec, payload):
    """The one JSON document the caller waits for, written atomically to spec["handoffPath"]: a
    supervisor started through WMI has no pipe back to its caller."""
    _write_atomic(spec["handoffPath"], json.dumps(payload))


def supervise(spec):
    started = time.time()
    try:
        child, command_line = _start_child(spec)
    except Exception as exc:  # reported to the caller, which raises it
        _say("start failed: %r" % (exc,))
        _handoff(spec, {"error": "%s: %s" % (type(exc).__name__, exc)})
        return 1
    pid = child.proc.pid
    _say("started pid %d: %s" % (pid, command_line))
    if not child.capped:
        _say("UNCAPPED: %s" % child.uncapped_reason)
    _handoff(spec, {"pid": pid, "processStartMs": process_start_ms(pid), "capped": child.capped,
                    "capMechanism": child.mechanism, "uncappedReason": child.uncapped_reason,
                    "commandLine": command_line})

    timeout = spec.get("timeoutMinutes")
    deadline = started + timeout * 60 if timeout else None
    timed_out, peak, oom_kills = False, 0, 0
    while True:
        try:
            code = child.proc.wait(timeout=2)
            break
        except subprocess.TimeoutExpired:
            pass
        if child.scope:
            peak, oom_kills = linux_scope_usage(child.scope)
        if deadline and time.time() > deadline:
            _say("TIMEOUT after %s minutes -- terminating the job" % timeout)
            child.kill()
            timed_out = True
            code = child.proc.wait()
            break

    violation_is_memory, violation_text = False, "n/a"
    if os.name == "nt":
        code = code - (1 << 32) if code >= (1 << 31) else code  # NTSTATUS exit codes as signed
        if child.job:
            peak, violation_is_memory, violation_text = _win_job_peak_and_violation(child.job)
            _kernel32().CloseHandle(child.job)  # KILL_ON_JOB_CLOSE reaps stragglers
    else:
        if child.scope:
            last_peak, oom_kills = linux_scope_usage(child.scope)
            peak = max(peak, last_peak)
            violation_is_memory = oom_kills > 0
            violation_text = "oom_kill=%d" % oom_kills
        if spec["capped"]:
            child.kill_group()  # the Linux stand-in for KILL_ON_JOB_CLOSE

    wall = time.time() - started
    cap = spec["capBytes"] if child.capped else 0
    _say("exit code %d, wall %.1f min, peak %s GiB of %s GiB cap, violation %s"
         % (code, wall / 60.0, _gb(peak), _gb(cap), violation_text))
    if spec["kind"] == "suite":
        scan = _scan_log(spec["logPath"])
        hit = is_cap_hit(cap, peak, violation_is_memory, scan["oomAlloc"], scan["oomBackupPool"])
        verdict = verdict_for("suite", timed_out, hit, code)
        _say("cap seen by editor: %s" % (scan["capSeenByEditor"] or
             "NOT LOGGED -- the editor did not report a job memory limit; the run may be uncapped"))
        _say("editor memory total: %s" % (scan["memoryTotalLine"] or "<not logged>"))
        _say("oom_alloc=%d oom_backup_pool=%d watermark=%d last Test Started: %s" % (
            scan["oomAlloc"], scan["oomBackupPool"], scan["watermarkMarkers"], scan["lastTest"]))
        line = format_suite_result(verdict, cap, peak, spec["priority"], code, scan, wall, spec["logPath"])
    else:
        verdict = verdict_for(spec["kind"], timed_out, is_cap_hit(cap, peak, violation_is_memory), code)
        line = format_job_result(verdict, code, spec["priority"], cap, peak, wall,
                                 spec["argv"][0], spec.get("outputPath"))
    _say(line)
    _write_atomic(spec["resultPath"], line + "\n")
    return 0


# ------------------------------------------------------------------------------------------------
# Caller side
# ------------------------------------------------------------------------------------------------
class SupervisedRun:
    """Quacks like subprocess.Popen for pid / returncode / poll() / wait() / terminate() / kill().
    pid is the supervised child (the editor), not the supervisor."""

    LOST_GRACE_SECONDS = 5.0

    def __init__(self, run_id, pid, process_start_ms_value, supervisor_pid, supervisor_start_ms,
                 result_path, supervisor_log_path, log_path, handoff, args=None):
        self.run_id, self.pid, self.process_start_ms = run_id, pid, process_start_ms_value
        self.supervisor_pid, self.supervisor_start_ms = supervisor_pid, supervisor_start_ms
        self.result_path, self.supervisor_log_path, self.log_path = result_path, supervisor_log_path, log_path
        self.handoff, self.args = handoff, args
        self.capped = bool(handoff.get("capped"))  # whether the memory cap actually applied
        # Whether the run outlives the MCP client's process tree, how the supervisor was started,
        # and why it is not detached when it is not.
        self.detached = handoff.get("detached")
        self.launch_mechanism = handoff.get("launchMechanism")
        self.detach_note = handoff.get("detachNote")
        self.returncode = self.verdict = self.result_line = None
        self.supervisor_lost = False
        self._gone_since = None

    def poll(self):
        if self.returncode is not None:
            return self.returncode
        line, verdict, code = read_result(self.result_path)
        if line is not None:
            self.result_line, self.verdict, self.returncode = line, verdict, code
            return code
        if _matches(self.pid, self.process_start_ms):
            return None
        # Child gone: a live supervisor is still scanning the log / writing the result.
        if _matches(self.supervisor_pid, self.supervisor_start_ms):
            return None
        if self._gone_since is None:
            self._gone_since = time.monotonic()
        if time.monotonic() - self._gone_since < self.LOST_GRACE_SECONDS:
            return None
        self.supervisor_lost = True
        self.returncode = -1
        return -1

    def wait(self, timeout=None):
        deadline = None if timeout is None else time.monotonic() + timeout
        while self.poll() is None:
            if deadline is not None and time.monotonic() >= deadline:
                raise subprocess.TimeoutExpired(self.args, timeout)
            time.sleep(0.5)
        return self.returncode

    def _signal(self, sig):
        if not _matches(self.pid, self.process_start_ms):
            return
        if os.name != "nt" and sig == SIGKILL:
            os.killpg(self.pid, SIGKILL)  # the child leads its own process group
        else:
            os.kill(self.pid, sig)  # Windows: TerminateProcess; the supervisor then closes the job

    def terminate(self):
        self._signal(SIGTERM)

    def kill(self):
        self._signal(SIGKILL)


def _supervisor_popen(cmd, stderr):
    """Start the supervisor as a direct child. Returns (proc, launchMechanism).

    On Windows a direct child stays a parent-pid descendant of the caller whatever its flags, so
    a tree kill of the MCP client (`taskkill /T`, which Claude Code runs on the trees it owns)
    reaches it; it is the fallback, not the default. Linux: a new session, out of the caller's
    process group."""
    common = {"stdin": subprocess.DEVNULL, "stdout": subprocess.DEVNULL, "stderr": stderr,
              "close_fds": True}
    if os.name != "nt":
        return subprocess.Popen(cmd, start_new_session=True, **common), "setsid"
    flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW
    try:
        return (subprocess.Popen(cmd, creationflags=flags | CREATE_BREAKAWAY_FROM_JOB, **common),
                "createprocess-breakaway")
    except OSError:
        # The caller's job forbids breakaway: the run then also lives only as long as that job.
        return subprocess.Popen(cmd, creationflags=flags, **common), "createprocess-in-caller-job"


_WMI_CREATE_SCRIPT = (
    "$ErrorActionPreference='Stop';"
    "$s=New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly "
    "-Property @{CreateFlags=[uint32]$env:PINWRIGHT_WMI_FLAGS;ShowWindow=[uint16]0};"
    "$r=Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments "
    "@{CommandLine=$env:PINWRIGHT_WMI_COMMAND;CurrentDirectory=$env:PINWRIGHT_WMI_CWD;"
    "ProcessStartupInformation=$s};"
    "'{0} {1}' -f $r.ReturnValue,$r.ProcessId")


def _wmi_create(command_line, cwd):
    """Start command_line through WMI Win32_Process.Create and return its pid.

    WmiPrvSE creates the process, so it is neither a parent-pid descendant of the caller nor in
    the caller's job: a tree kill or job close of the MCP client cannot reach it. The command
    line travels in the environment, so no PowerShell quoting applies to it. Raises RuntimeError.
    """
    powershell = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32",
                              "WindowsPowerShell", "v1.0", "powershell.exe")
    env = dict(os.environ, PINWRIGHT_WMI_COMMAND=command_line, PINWRIGHT_WMI_CWD=cwd,
               PINWRIGHT_WMI_FLAGS=str(DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP))
    try:
        done = subprocess.run([powershell, "-NoProfile", "-NonInteractive", "-Command",
                               _WMI_CREATE_SCRIPT], env=env, capture_output=True, text=True,
                              timeout=60, creationflags=CREATE_NO_WINDOW)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError("powershell could not run: %s" % exc)
    match = re.search(r"^(\d+) (\d+)\s*$", done.stdout or "", re.MULTILINE)
    if not match:
        raise RuntimeError("no result from Win32_Process.Create (exit %d): %s"
                           % (done.returncode, (done.stderr or done.stdout or "").strip()[:300]))
    if match.group(1) != "0":
        raise RuntimeError("Win32_Process.Create returned %s" % match.group(1))
    return int(match.group(2))


def _start_supervisor(cmd, supervisor_log):
    """Start the supervisor process. Returns (pid, launchMechanism, detached, detachNote).

    Windows: WMI first, so the run outlives the MCP client; a WMI failure falls back to a direct
    child and says so (detached False plus the reason), never silently."""
    if os.name == "nt":
        try:
            return (_wmi_create(subprocess.list2cmdline(cmd), os.path.dirname(cmd[1])),
                    "wmi-win32-process-create", True, None)
        except RuntimeError as exc:
            note = ("WMI launch failed (%s); started as a direct child instead, so the run dies "
                    "with the MCP client's process tree" % exc)
    else:
        note = None
    with open(supervisor_log, "ab") as log_fh:
        proc, mechanism = _supervisor_popen(cmd, log_fh)
    threading.Thread(target=proc.wait, daemon=True).start()  # reap: no zombie supervisor
    if mechanism == "createprocess-in-caller-job":
        note += "; the caller's job also refused breakaway"
    return proc.pid, mechanism, note is None, note


def spawn_supervised(argv_with_exe, *, kind, reason, launched_by, mode, memory_fraction=0.60,
                     priority="BelowNormal", timeout_minutes=120, log_path=None, output_path=None,
                     pid_wait_seconds=30, capped=True, env=None, output_header=None):
    """Launch argv under a detached supervisor; return once the child pid is known. env (dict or
    None = inherit) becomes the supervised child's environment. mode may be None only for kind
    'command' (a build launches no editor; it runs without a window). output_header is written
    at the top of output_path before the child's output."""
    reason, error = normalize_reason(reason)
    if error:
        raise ValueError(error)
    if kind not in KINDS:
        raise ValueError("kind must be one of %s" % (KINDS,))
    if mode not in MODES and not (mode is None and kind == "command"):
        raise ValueError("mode must be one of %s" % (MODES,))
    if priority not in PRIORITIES:
        raise ValueError("priority must be one of %s" % (PRIORITIES,))
    if not 0 < memory_fraction <= 1:
        raise ValueError("memory_fraction must be in (0, 1]")
    argv = list(argv_with_exe)
    if kind in ("editor", "suite"):
        argv = _with_identity(argv, reason, launched_by)
    run_id = uuid.uuid4().hex
    base = log_path or output_path
    if base:
        base = os.path.abspath(base)
        os.makedirs(os.path.dirname(base), exist_ok=True)
        result_path, supervisor_log = base + ".result.txt", base + ".supervisor.log"
    else:
        run_dir = os.path.join(tempfile.gettempdir(), "pinwright-supervisor", run_id)
        os.makedirs(run_dir, exist_ok=True)
        result_path, supervisor_log = os.path.join(run_dir, "result.txt"), os.path.join(run_dir, "supervisor.log")
    if output_path:
        os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    try:
        os.remove(result_path)  # a stale verdict from a previous run on the same log
    except FileNotFoundError:
        pass
    spec = {"kind": kind, "argv": argv, "mode": mode, "capped": capped, "priority": priority,
            "capBytes": cap_bytes_for(total_physical_bytes(), memory_fraction) if capped else 0,
            "timeoutMinutes": timeout_minutes,
            "logPath": os.path.abspath(log_path) if log_path else None,
            "outputPath": os.path.abspath(output_path) if output_path else None,
            # Always explicit: a supervisor started through WMI gets the user's default
            # environment, not the caller's, so an inherited env would silently change.
            "resultPath": result_path, "env": dict(env) if env is not None else dict(os.environ),
            "outputHeader": output_header}
    # Spec and handoff travel as files in the user's temp dir: WMI gives no pipe to the process
    # it creates. The spec carries the environment, so the supervisor deletes it once read.
    exchange_dir = os.path.join(tempfile.gettempdir(), "pinwright-supervisor", run_id)
    os.makedirs(exchange_dir, exist_ok=True)
    spec_path = os.path.join(exchange_dir, "spec.json")
    handoff_path = os.path.join(exchange_dir, "handoff.json")
    spec.update(handoffPath=handoff_path, supervisorLogPath=supervisor_log,
                protocolVersion=PROTOCOL_VERSION)
    _write_atomic(spec_path, json.dumps(spec))
    cmd = [sys.executable, os.path.abspath(__file__), SUPERVISE_FLAG, spec_path]
    try:
        try:
            pid, mechanism, detached, note = _start_supervisor(cmd, supervisor_log)
        except OSError as exc:
            raise RuntimeError("could not start the supervisor: %s" % exc)
        supervisor_start = process_start_ms(pid)
        deadline = time.monotonic() + pid_wait_seconds
        while True:
            handoff = _read_json(handoff_path)
            if handoff is None and not _matches(pid, supervisor_start):
                handoff = _read_json(handoff_path)  # it may have written and exited in between
                if handoff is None:
                    last = _last_line(supervisor_log)
                    if VERSION_MISMATCH in last:
                        raise SupervisorVersionMismatch("%s (see %s)" % (last, supervisor_log))
                    raise RuntimeError("supervisor pid %d exited without a handoff; its log (%s) "
                                       "ends: %s" % (pid, supervisor_log, last or "<empty>"))
            if handoff is not None:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("supervisor pid %d reported no child within %ss; see %s"
                                   % (pid, pid_wait_seconds, supervisor_log))
            time.sleep(0.1)
    finally:
        for path in (spec_path, handoff_path):
            try:
                os.remove(path)
            except OSError:
                pass
        try:
            os.rmdir(exchange_dir)  # still holds result/log for a run with no log_path
        except OSError:
            pass
    if handoff.get("code") == VERSION_MISMATCH:
        raise SupervisorVersionMismatch("%s (see %s)" % (handoff["error"], supervisor_log))
    if "error" in handoff:
        raise RuntimeError("supervised start failed: %s (see %s)" % (handoff["error"], supervisor_log))
    handoff.update(capBytes=spec["capBytes"] if handoff.get("capped") else 0, priority=priority,
                   resultPath=result_path, supervisorLogPath=supervisor_log, argv=argv,
                   detached=detached, launchMechanism=mechanism, detachNote=note)
    return SupervisedRun(run_id, handoff["pid"], handoff.get("processStartMs"), pid,
                         supervisor_start, result_path, supervisor_log, spec["logPath"], handoff, argv)


def _read_json(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def _last_line(path, tail_bytes=4096):
    """The last non-empty line of a log, or '' -- so a supervisor that died before its handoff
    still says why in the caller's error."""
    try:
        with open(path, "rb") as fh:
            fh.seek(max(0, os.path.getsize(path) - tail_bytes))
            lines = fh.read().decode("utf-8", errors="replace").splitlines()
    except OSError:
        return ""
    return next((line.strip() for line in reversed(lines) if line.strip()), "")


# ------------------------------------------------------------------------------------------------
# Internal entry point (no public CLI)
# ------------------------------------------------------------------------------------------------
SUPERVISE_FLAG = "--supervise"
# The proxy <-> supervisor exchange (argv, spec, handoff). The MCP proxy imports this module once
# and keeps it in memory, but launches THIS FILE from disk, so a proxy started before an edit runs
# a newer script. Bump on any change to that exchange. 1: `--supervise`, spec on stdin, handoff on
# stdout. 2: `--supervise <spec.json>`, spec["protocolVersion"], handoff.json.
PROTOCOL_VERSION = 2
VERSION_MISMATCH = "SUPERVISOR_VERSION_MISMATCH"
EXIT_VERSION_MISMATCH = 3


class SupervisorVersionMismatch(RuntimeError):
    """The running MCP proxy and pinwright_supervisor.py on disk speak different protocols."""


def _mismatch_text(caller_version):
    return ("%s: the MCP proxy speaks supervisor protocol %s but pinwright_supervisor.py on disk "
            "speaks protocol %d: the plugin's Python changed after the MCP server started. "
            "Restart the MCP server (Claude Code: /mcp, reconnect) so it loads the current code."
            % (VERSION_MISMATCH, caller_version, PROTOCOL_VERSION))


# ------------------------------------------------------------------------------------------------
# Multi-engine tooling entry point: `python pinwright_supervisor.py --spawn <request.json>`
# ------------------------------------------------------------------------------------------------
SPAWN_FLAG = "--spawn"
_SPAWN_REQUIRED = {
    "suite": ("kind", "reason", "launchedBy", "mode", "uproject", "editorExe", "filter", "logPath"),
    "command": ("kind", "reason", "launchedBy", "argv", "outputPath"),
}
_SPAWN_OPTIONAL = {"suite": ("extraArgs",), "command": ("mode",)}


def spawn_request(request):
    """Start one capped, detached run from a request dict and describe it. For multi-engine tooling
    that drives host projects the proxy tools cannot target; everyone else uses the proxy tools.

    kind "suite":   reason, launchedBy, mode (visible | offscreen | headless, no default), uproject,
                    editorExe (the GUI UnrealEditor.exe; offscreen / headless use its -Cmd twin),
                    filter, logPath, optional extraArgs (list). The argv is suite_argv(), the same
                    contract editor_run_tests launches.
    kind "command": reason, launchedBy, argv (executable first), outputPath (stdout + stderr, the
                    reason as its first line), optional mode.
    Cap, priority and timeout are spawn_supervised()'s: 0.60 of RAM, BelowNormal, 120 minutes, and
    so is the spec it hands --supervise, stamped with PROTOCOL_VERSION: a skew still raises
    SupervisorVersionMismatch. Unknown or missing keys raise ValueError. Returns the pids and paths the caller waits on: the
    supervisor exits right after writing resultPath, so waiting on supervisorPid is waiting for
    the verdict."""
    kind = request.get("kind")
    if kind not in _SPAWN_REQUIRED:
        raise ValueError("kind must be one of %s, got %r" % (tuple(_SPAWN_REQUIRED), kind))
    missing = [key for key in _SPAWN_REQUIRED[kind] if key not in request]
    unknown = sorted(set(request) - set(_SPAWN_REQUIRED[kind]) - set(_SPAWN_OPTIONAL[kind]))
    if missing or unknown:
        raise ValueError("%s request: missing %s, unknown %s" % (kind, missing, unknown))
    reason, error = normalize_reason(request["reason"])
    if error:
        raise ValueError(error)
    if kind == "suite":
        mode = request["mode"]
        if mode not in MODES:
            raise ValueError("mode must be one of %s, got %r" % (MODES, mode))
        argv = [suite_executable(request["editorExe"], mode)] + suite_argv(
            request["uproject"], request["filter"], request["logPath"], mode,
            extra_args=request.get("extraArgs", ()))
        run = spawn_supervised(argv, kind="suite", reason=reason,
                               launched_by=request["launchedBy"], mode=mode,
                               log_path=request["logPath"])
    else:
        run = spawn_supervised(list(request["argv"]), kind="command", reason=reason,
                               launched_by=request["launchedBy"], mode=request.get("mode"),
                               output_path=request["outputPath"],
                               output_header="PinWright spawn request: reason=%s" % reason)
    handoff = run.handoff
    return {"pid": run.pid, "supervisorPid": run.supervisor_pid, "resultPath": run.result_path,
            "supervisorLogPath": run.supervisor_log_path, "logPath": run.log_path,
            "outputPath": request.get("outputPath"), "capped": run.capped,
            "capBytes": handoff.get("capBytes"), "priority": handoff.get("priority"),
            "detached": run.detached, "launchMechanism": run.launch_mechanism,
            "detachNote": run.detach_note, "commandLine": handoff.get("commandLine")}


def _spawn_main(request_path):
    """One JSON line on stdout: spawn_request()'s result, or {"error", "code"?}. Exit 0 started,
    2 bad request, 1 could not start, EXIT_VERSION_MISMATCH when the supervisor refuses the spec."""
    def answer(payload, code):
        sys.stdout.write(json.dumps(payload) + "\n")
        sys.stdout.flush()
        return code

    try:
        with open(request_path, encoding="utf-8-sig") as fh:  # PowerShell 5.1 writes a BOM
            request = json.load(fh)
        if not isinstance(request, dict):
            raise ValueError("the request must be a JSON object")
        return answer(spawn_request(request), 0)
    except ValueError as exc:
        return answer({"error": "%s: %s" % (type(exc).__name__, exc)}, 2)
    except SupervisorVersionMismatch as exc:
        return answer({"error": str(exc), "code": VERSION_MISMATCH}, EXIT_VERSION_MISMATCH)
    except (OSError, RuntimeError) as exc:  # unreadable request, no -Cmd twin, start failure
        return answer({"error": "%s: %s" % (type(exc).__name__, exc)}, 1)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) == 2 and argv[0] == SPAWN_FLAG:
        return _spawn_main(argv[1])
    if argv == [SUPERVISE_FLAG]:
        # A protocol-1 proxy: it reads its handoff as one JSON line on our stdout and routes our
        # stderr to its supervisor.log, so both carry the reason instead of a bare refusal.
        text = _mismatch_text(1)
        sys.stderr.write(text + "\n")
        sys.stderr.flush()
        sys.stdout.write(json.dumps({"error": text, "code": VERSION_MISMATCH}) + "\n")
        sys.stdout.flush()
        return EXIT_VERSION_MISMATCH
    if len(argv) != 2 or argv[0] != SUPERVISE_FLAG:
        sys.stderr.write("pinwright_supervisor.py is internal to the PinWright MCP proxy and has no "
                         "command line; use the editor_start / editor_run_tests / editor_build "
                         "proxy tools (multi-engine tooling: --spawn <request.json>, see "
                         "spawn_request).\n")
        return 2
    with open(argv[1], encoding="utf-8") as fh:
        spec = json.load(fh)
    os.remove(argv[1])  # it carries the child's environment
    # A WMI-started supervisor has no inherited stderr; diagnostics go to its own log.
    if spec.get("supervisorLogPath"):
        sys.stderr = open(spec["supervisorLogPath"], "a", encoding="utf-8")
    # Absent = 2: the spec-file exchange shipped one revision before this field, unchanged, and
    # proxies already running that revision must keep working when this file changes.
    version = spec.get("protocolVersion", 2)
    if version != PROTOCOL_VERSION:
        text = _mismatch_text(version)
        _say(text)
        if spec.get("handoffPath"):
            _handoff(spec, {"error": text, "code": VERSION_MISMATCH})
        return EXIT_VERSION_MISMATCH
    return supervise(spec)


if __name__ == "__main__":
    sys.exit(main())
