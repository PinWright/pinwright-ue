# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Unit tests for pinwright_supervisor (internal to the proxy): launch identity on the command line,
suite argv, the Windows Job Object and Linux systemd-run/nice/pdeathsig containment, the supervisor
handoff, result lines, log scanning, and the internal entry point (there is no public CLI).

Pure stdlib. Process creation and the Win32/libc calls are mocked; Linux branches run against
canned /proc and cgroup trees. The one end-to-end test launches a harmless `python -c` child under
a real supervisor and waits for it to exit on its own. No Unreal process is started.
"""
import contextlib
import ctypes
import io
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

# pinwright_supervisor.py lives one directory up (Content/Python/).
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import pinwright_supervisor as pl  # noqa: E402


class TempDirTest(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.tmp = self._temp.name

    def write(self, rel, text):
        path = os.path.join(self.tmp, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        return path


class ReasonTest(unittest.TestCase):
    REASONS = [
        'say "hi" to the editor',
        "C:\\path\\with\\backslashes",
        "ends with a backslash\\",
        "ends with two\\\\",
        "\u041f\u0440\u0438\u0432\u0435\u0442 \u043c\u0438\u0440 \U0001F680",
        "commas, (parens), and; semicolons",
        "literal %22 and %25 and 100%",
        "plain words with spaces",
        '\\"escaped-looking\\" quote',
    ]

    def test_normalize_refuses(self):
        for bad in (None, 5, "", "   ", "\n\t", "x" * (pl.REASON_MAX_CHARS + 1)):
            reason, error = pl.normalize_reason(bad)
            self.assertIsNone(reason, bad)
            self.assertTrue(error)

    def test_normalize_collapses_whitespace_and_accepts_limit(self):
        self.assertEqual(pl.normalize_reason("  a\n\tb   c \r\n")[0], "a b c")
        self.assertEqual(pl.normalize_reason("y" * pl.REASON_MAX_CHARS),
                         ("y" * pl.REASON_MAX_CHARS, None))

    def test_encode_decode(self):
        self.assertEqual(pl.encode_reason('a"b%c'), "a%22b%25c")
        self.assertEqual(pl.decode_reason("%2522 %22 %41"), '%22 " %41')
        for reason in self.REASONS:
            self.assertNotIn('"', pl.encode_reason(reason))
            self.assertEqual(pl.decode_reason(pl.encode_reason(reason)), reason)

    def test_windows_round_trip_through_command_line(self):
        for reason in self.REASONS:
            argv = ["C:\\Program Files\\UE\\UnrealEditor", "P.uproject",
                    "-ExecCmds=Automation RunTests X,Quit"] + pl.launch_identity_args(reason, "editor_start")
            line = subprocess.list2cmdline(argv)
            self.assertEqual(pl.split_windows_command_line(line), argv, reason)
            self.assertEqual(pl.parse_launch_identity(pl.split_windows_command_line(line)[1:]),
                             (reason, "editor_start"))

    def test_linux_argv_carries_identity_verbatim(self):
        for reason in self.REASONS:
            argv = ["/opt/UE/Engine/Binaries/Linux/UnrealEditor", "P.uproject"] + \
                pl.launch_identity_args(reason, "editor_start")
            self.assertEqual(pl.parse_launch_identity(argv), (reason, "editor_start"))

    def test_parse_is_case_insensitive_first_wins(self):
        argv = ["-pinwrightlaunchreason=first", "-PinWrightLaunchReason=second",
                "-PINWRIGHTLAUNCHEDBY=editor_start", "-PinWrightLaunchedBy=other"]
        self.assertEqual(pl.parse_launch_identity(argv), ("first", "editor_start"))
        self.assertEqual(pl.parse_launch_identity(["-foo"]), (None, None))

    def test_with_identity_is_idempotent(self):
        once = pl._with_identity(["exe", "p"], "why", "editor_start")
        self.assertEqual(once, ["exe", "p", "-PinWrightLaunchReason=why", "-PinWrightLaunchedBy=editor_start"])
        self.assertEqual(pl._with_identity(once, "other", "editor_start"), once)


class SplitCommandLineTest(unittest.TestCase):
    def test_rules(self):
        split = pl.split_windows_command_line
        self.assertEqual(split('"C:\\a b\\prog" x'), ["C:\\a b\\prog", "x"])
        self.assertEqual(split('C:\\a\\prog  x\ty'), ["C:\\a\\prog", "x", "y"])
        self.assertEqual(split('p a\\\\"b c"'), ["p", "a\\b c"])          # 2n -> n + toggle
        self.assertEqual(split('p a\\\\\\"b'), ["p", 'a\\"b'])          # 2n+1 -> n + literal
        self.assertEqual(split('p a\\b\\c'), ["p", "a\\b\\c"])            # literal backslashes
        self.assertEqual(split('p "a""b" c'), ["p", 'a"b', "c"])          # "" inside quotes
        self.assertEqual(split('p "" x'), ["p", "", "x"])
        self.assertEqual(split(""), [])


class ArgvTest(unittest.TestCase):
    def test_infer_mode(self):
        self.assertEqual(pl.infer_mode(["p", "-RUN=Cook", "-game"]), "commandlet")
        self.assertEqual(pl.infer_mode(["p", "-Game", "-NullRHI"]), "game")
        self.assertEqual(pl.infer_mode(["p", "-run=Cook", "-NullRHI"]), "commandlet")
        for flag in ("-RenderOffScreen", "-Unattended"):
            self.assertEqual(pl.infer_mode(["p", flag]), "offscreen")
        self.assertEqual(pl.infer_mode(["p", "-nullrhi"]), "headless")
        self.assertEqual(pl.infer_mode(["p", "-RenderOffScreen", "-NullRHI"]), "headless")
        self.assertEqual(pl.infer_mode(["p", "-log"]), "visible")

    def test_suite_argv_per_mode(self):
        windowless = pl.suite_argv("/p/H.uproject", "PinWright.Model", "/l/a.log", "offscreen",
                                   report_dir="/l/report", extra_args=["-x=1"])
        self.assertEqual(windowless, [
            "/p/H.uproject", "-ExecCmds=Automation RunTests PinWright.Model,Quit",
            "-TestExit=Automation Test Queue Empty", "-unattended", "-nopause", "-nosplash",
            "-nosound", "-RenderOffscreen", "-nocefaccelpaint", "-RunningUnattendedScript",
            "-ddc=InstalledNoZenLocalFallback", "-PinWrightTransport", "-ReportExportPath=/l/report",
            "-Abslog=/l/a.log", "-x=1"])
        visible = pl.suite_argv("/p/H.uproject", "PinWright", "/l/a.log", "visible")
        self.assertNotIn("-RenderOffscreen", visible)
        self.assertFalse(any(a.startswith("-ReportExportPath") for a in visible))
        self.assertFalse(any(a.lower() == "-nullrhi" for a in windowless + visible))
        self.assertEqual(visible[-1], "-Abslog=/l/a.log")
        headless = pl.suite_argv("/p/H.uproject", "PinWright", "/l/a.log", "headless")
        # -NullRHI replaces the renderer; -RenderOffscreen is still what removes the OS window
        # and the Linux display requirement.
        self.assertIn("-NullRHI", headless)
        self.assertIn("-RenderOffscreen", headless)
        self.assertIn("-RunningUnattendedScript", headless)


class SuiteExecutableTest(TempDirTest):
    def test_visible_is_gui_editor(self):
        self.assertEqual(pl.suite_executable("/e/UnrealEditor", "visible"), "/e/UnrealEditor")

    def test_windows_windowless_uses_cmd_twin(self):
        ext = os.extsep + "exe"
        editor = self.write("Engine/Binaries/Win64/UnrealEditor" + ext, "")
        with mock.patch.object(pl.os, "name", "nt"):
            with self.assertRaises(FileNotFoundError):
                pl.suite_executable(editor, "offscreen")
            twin = self.write("Engine/Binaries/Win64/UnrealEditor-Cmd" + ext, "")
            for mode in ("offscreen", "headless"):
                self.assertEqual(os.path.normcase(pl.suite_executable(editor, mode)),
                                 os.path.normcase(twin))

    def test_linux_windowless_is_plain_binary(self):
        with mock.patch.object(pl.os, "name", "posix"):
            self.assertEqual(pl.suite_executable("/e/Linux/UnrealEditor", "headless"),
                             "/e/Linux/UnrealEditor")


class JobObjectTest(unittest.TestCase):
    def test_limit_struct_values(self):
        cap = pl.cap_bytes_for(68_000_000_001, 0.6)
        self.assertEqual(cap, 40_800_000_000)  # floor(total * fraction)
        for name, value in (("Normal", 0x20), ("BelowNormal", 0x4000), ("Idle", 0x40)):
            info = pl.job_limits(cap, name)
            self.assertEqual(info.BasicLimitInformation.LimitFlags, 0x100 | 0x2000 | 0x20)
            self.assertEqual(info.BasicLimitInformation.PriorityClass, value)
            self.assertEqual(info.ProcessMemoryLimit, cap)

    def _fakes(self, assign_ok=True):
        calls = []
        k32 = mock.MagicMock()
        k32.CreateJobObjectW.side_effect = lambda *a: calls.append("create") or 77
        captured = {}

        def set_info(job, cls, ref, size):
            calls.append("set")
            captured["info"], captured["cls"], captured["size"] = ref._obj, cls, size
            return 1
        k32.SetInformationJobObject.side_effect = set_info
        k32.AssignProcessToJobObject.side_effect = lambda job, h: calls.append(("assign", job, h)) or assign_ok
        nt = mock.MagicMock()
        nt.NtResumeProcess.side_effect = lambda h: calls.append(("resume", h)) or 0
        proc = mock.MagicMock(_handle=555, pid=4242)

        def popen(cmd, **kwargs):
            calls.append(("popen", kwargs["creationflags"]))
            return proc
        return calls, captured, k32, nt, proc, popen

    def test_suspended_then_assign_then_resume(self):
        calls, captured, k32, nt, proc, popen = self._fakes()
        with mock.patch.object(pl, "_kernel32", return_value=k32), \
                mock.patch.object(pl, "_ntdll", return_value=nt), \
                mock.patch.object(pl.subprocess, "Popen", side_effect=popen):
            got, job = pl._win_start_in_job('"x" a', 1234, "BelowNormal", pl.CREATE_NO_WINDOW,
                                            subprocess.DEVNULL, subprocess.DEVNULL)
        self.assertIs(got, proc)
        self.assertEqual(job, 77)
        self.assertEqual(calls, ["create", "set",
                                 ("popen", pl.CREATE_NO_WINDOW | pl.CREATE_SUSPENDED),
                                 ("assign", 77, 555), ("resume", 555)])
        self.assertEqual(captured["cls"], 9)
        self.assertEqual(captured["size"], ctypes.sizeof(pl.JOBOBJECT_EXTENDED_LIMIT_INFORMATION))
        self.assertEqual(captured["info"].ProcessMemoryLimit, 1234)
        self.assertEqual(captured["info"].BasicLimitInformation.PriorityClass, 0x4000)

    def test_assign_failure_kills_suspended_child(self):
        calls, _, k32, nt, proc, popen = self._fakes(assign_ok=False)
        with mock.patch.object(pl, "_kernel32", return_value=k32), \
                mock.patch.object(pl, "_ntdll", return_value=nt), \
                mock.patch.object(pl.subprocess, "Popen", side_effect=popen):
            with self.assertRaises(OSError):
                pl._win_start_in_job("x", 1, "Normal", 0, None, None)
        proc.kill.assert_called_once()
        nt.NtResumeProcess.assert_not_called()
        k32.CloseHandle.assert_called_with(77)

    def test_batch_file_is_hosted_by_cmd(self):
        line = pl._windows_command_line(["C:\\UE\\Build.bat", "HostEditor", "-Project=C:\\a b\\H.uproject"])
        self.assertIn(" /S /c \"", line)
        self.assertTrue(line.endswith('"C:\\a b\\H.uproject""') or line.endswith('H.uproject""'))
        self.assertEqual(pl._windows_command_line(["C:\\x\\tool", "a b"]), 'C:\\x\\tool "a b"')


class LinuxContainmentTest(TempDirTest):
    def test_systemd_prefix_and_fallbacks(self):
        self.assertEqual(pl.systemd_scope_prefix(10, which=lambda n: None),
                         (None, "systemd-run not found on PATH"))
        failing = mock.MagicMock(return_value=mock.MagicMock(returncode=1, stderr=b"Failed to connect to bus"))
        prefix, reason = pl.systemd_scope_prefix(10, run=failing, which=lambda n: "/usr/bin/systemd-run")
        self.assertIsNone(prefix)
        self.assertIn("Failed to connect to bus", reason)
        ok = mock.MagicMock(return_value=mock.MagicMock(returncode=0, stderr=b""))
        prefix, reason = pl.systemd_scope_prefix(2048, run=ok, which=lambda n: "/usr/bin/systemd-run")
        self.assertIsNone(reason)
        self.assertEqual(prefix, ["/usr/bin/systemd-run", "--user", "--scope", "--quiet",
                                  "-p", "MemoryMax=2048", "--"])
        probe_argv, stop_argv = ok.call_args_list[0][0][0], ok.call_args_list[1][0][0]
        unit = probe_argv[4][len("--unit="):]
        self.assertTrue(unit.startswith("pinwright-probe-") and unit.endswith(".scope"), unit)
        self.assertEqual(probe_argv, prefix[:4] + ["--unit=" + unit] + prefix[4:] + ["true"])
        # The probe scope is stopped by name: the user manager can fail to collect one that
        # emptied at once, and it then stays 'active (running)' with 0 tasks.
        self.assertEqual(stop_argv, ["systemctl", "--user", "stop", "--no-block", unit])
        raising = mock.MagicMock(side_effect=subprocess.TimeoutExpired("x", 15))
        self.assertIn("probe failed", pl.systemd_scope_prefix(1, run=raising, which=lambda n: "s")[1])

    def test_scope_readback_and_usage(self):
        proc_root, cg_root = os.path.join(self.tmp, "proc"), os.path.join(self.tmp, "cg")
        self.write("proc/42/cgroup", "0::/user.slice/user-1000.slice/user@1000.service/app.slice/run-r1.scope\n")
        scope = os.path.join(cg_root, "user.slice", "user-1000.slice", "user@1000.service",
                             "app.slice", "run-r1.scope")
        os.makedirs(scope)
        self.write(os.path.join(scope, "memory.max"), "8589930496\n")  # page-rounded 8 GiB - 4 KiB
        self.write(os.path.join(scope, "memory.peak"), "123456\n")
        self.write(os.path.join(scope, "memory.events"), "low 0\nhigh 0\nmax 3\noom 1\noom_kill 1\n")
        got, reason = pl.linux_wait_for_scope(42, 8 * 1024 ** 3, 0, proc_root, cg_root)
        self.assertIsNone(reason)
        self.assertEqual(os.path.normpath(got), scope)  # Linux paths joined on a Windows test host
        self.assertEqual(pl.linux_scope_usage(scope), (123456, 1))
        self.assertEqual(pl.linux_scope_usage(None), (0, 0))
        got, reason = pl.linux_wait_for_scope(42, 1024, 0, proc_root, cg_root)
        self.assertIsNone(got)
        self.assertIn("not delegated", reason)
        self.assertIsNone(pl.linux_cgroup_dir(99, proc_root, cg_root))

    def test_preexec_wiring(self):
        libc = mock.MagicMock()
        with mock.patch.object(pl, "_libc", return_value=libc), \
                mock.patch.object(pl.os, "setpgid", create=True) as setpgid, \
                mock.patch.object(pl.os, "nice", create=True) as nice, \
                mock.patch.object(pl.os, "getppid", return_value=100), \
                mock.patch.object(pl.os, "_exit") as exit_:
            pl.linux_preexec(10, True, 100)()
            setpgid.assert_called_once_with(0, 0)
            nice.assert_called_once_with(10)
            libc.prctl.assert_called_once_with(1, 9, 0, 0, 0)
            exit_.assert_not_called()
            pl.linux_preexec(10, True, 999)()  # parent already gone before prctl took effect
            exit_.assert_called_once_with(1)
            libc.reset_mock()
            nice.reset_mock()
            pl.linux_preexec(0, False, 100)()
            nice.assert_not_called()
            libc.prctl.assert_not_called()

    def test_nice_table(self):
        self.assertEqual(pl.NICE, {"Normal": 0, "BelowNormal": 10, "Idle": 19})


class ProcStartTest(TempDirTest):
    def test_canned_proc_tree(self):
        root = os.path.join(self.tmp, "proc")
        self.write("proc/stat", "cpu 1 2 3\nbtime 1700000000\nprocesses 5\n")
        fields = ["S"] + [str(i) for i in range(4, 22)] + ["12345"] + ["0"] * 30
        self.write("proc/321/stat", "321 (Unreal) Editor (x)) " + " ".join(fields) + "\n")
        self.assertEqual(pl.proc_start_ms(321, root, clk_tck=100), 1700000000 * 1000 + 123450)
        fields[0] = "Z"
        self.write("proc/322/stat", "322 (z) " + " ".join(fields) + "\n")
        self.assertIsNone(pl.proc_start_ms(322, root, clk_tck=100))
        self.assertIsNone(pl.proc_start_ms(999, root, clk_tck=100))

    def test_live_process(self):
        start = pl.process_start_ms(os.getpid())
        self.assertIsNotNone(start)
        self.assertLess(abs(start - time.time() * 1000), 24 * 3600 * 1000)
        self.assertIsNone(pl.process_start_ms(None))


_DRAINED = (
    "LogInit: Display: Command Line: H.uproject -ExecCmds=\"Automation RunTests PinWright,Quit\" "
    "-TestExit=\"Automation Test Queue Empty\" -unattended\n"
    "LogMemory: Process is running as part of a Windows Job with separate resource limits\n"
    "LogAutomationCommandLine: Display: Found 2 automation tests based on 'PinWright'\n"
    "LogAutomationController: Display: Test Started. Name={A} Path={PinWright.probe.A}\n"
    "LogAutomationController: Display: Test Completed. Result={Success} Name={A} Path={PinWright.probe.A}\n"
    "LogAutomationController: Display: Test Started. Name={B} Path={PinWright.probe.B}\n"
    "LogAutomationController: Display: Test Completed. Result={Success} Name={B} Path={PinWright.probe.B}\n"
    "LogAutomationCommandLine: Display: ...Automation Test Queue Empty 2 tests performed.\n"
    "LogExit: Display: **** TestExit: Automation Test Queue Empty ****\n"
)
_TRUNCATED = (
    "LogInit: Display: Command Line: H.uproject -TestExit=\"Automation Test Queue Empty\"\n"
    "LogAutomationCommandLine: Display: Found 3 automation tests based on 'PinWright'\n"
    "LogAutomationController: Display: Test Started. Name={A} Path={PinWright.probe.A}\n"
    "LogAutomationController: Error: Test Completed. Result={Fail} Name={A} Path={PinWright.probe.A}\n"
    "LogAutomationController: Display: Test Started. Name={B} Path={PinWright.probe.B}\n"
    "LogMemory: Error: Ran out of memory allocating 1024 bytes\n"
    "LogMemory: Warning: Freeing 32 bytes from backup pool to handle out of memory.\n"
    "LogPinWright: Warning: PINWRIGHT_MEMORY_WATERMARK_EXCEEDED used=0.9\n"
)


class LogScanTest(TempDirTest):
    def test_progress_and_memory_evidence(self):
        path = self.write("t.log", _TRUNCATED)
        self.assertEqual(pl.scan_test_progress(path), {
            "exists": True, "started": 2, "succeeded": 0, "failed": 1, "lastTest": "PinWright.probe.B"})
        scan = pl._scan_log(path)
        self.assertEqual((scan["oomAlloc"], scan["oomBackupPool"], scan["watermarkMarkers"]), (1, 1, 1))
        self.assertIsNone(scan["capSeenByEditor"])
        drained = pl._scan_log(self.write("d.log", _DRAINED))
        self.assertIn("Windows Job", drained["capSeenByEditor"])
        self.assertEqual(pl.scan_test_progress(os.path.join(self.tmp, "none.log")), {
            "exists": False, "started": 0, "succeeded": 0, "failed": 0, "lastTest": None})


class ResultLineTest(TempDirTest):
    def test_verdict_ladder(self):
        self.assertEqual(pl.verdict_for("suite", True, True, 1), "TIMEOUT")
        self.assertEqual(pl.verdict_for("suite", False, True, 1), "MEMORY_CAP_HIT")
        self.assertEqual(pl.verdict_for("suite", False, False, 3), "EDITOR_EXIT_NONZERO")
        self.assertEqual(pl.verdict_for("editor", False, False, 0), "EDITOR_EXITED")
        self.assertEqual(pl.verdict_for("command", False, False, -1), "COMMAND_EXIT_NONZERO")
        self.assertEqual(pl.verdict_for("command", False, False, 0), "COMMAND_EXITED")

    def test_cap_hit(self):
        self.assertTrue(pl.is_cap_hit(100, 98, False))
        self.assertFalse(pl.is_cap_hit(100, 97, False))
        self.assertFalse(pl.is_cap_hit(0, 500, False))
        self.assertTrue(pl.is_cap_hit(100, 0, True))
        self.assertTrue(pl.is_cap_hit(100, 0, False, oom_alloc=1))
        self.assertTrue(pl.is_cap_hit(100, 0, False, oom_backup_pool=2))

    def test_invariant_decimals_and_lines(self):
        gib = 1024 ** 3
        scan = pl._scan_log(self.write("t.log", _TRUNCATED))
        line = pl.format_suite_result("MEMORY_CAP_HIT", int(37.906 * gib), 38 * gib, "BelowNormal",
                                      -1073741795, scan, 150, "/l/a.log")
        self.assertEqual(line, "PINWRIGHT_SUITE_RESULT verdict=MEMORY_CAP_HIT cap_gb=37.91 peak_gb=38 "
                               "priority=BelowNormal exit=-1073741795 oom_alloc=1 oom_backup_pool=1 "
                               "watermark_markers=1 capSeenByEditor=False wall_min=2 log=/l/a.log")
        line = pl.format_job_result("COMMAND_EXITED", 0, "Idle", int(1.5 * gib), 0, 30, "/x/tool", None)
        self.assertEqual(line, "PINWRIGHT_JOB_RESULT verdict=COMMAND_EXITED exit=0 priority=Idle "
                               "cap_gb=1.5 peak_gb=0 wall_min=0 command=/x/tool output=<none>")
        path = os.path.join(self.tmp, "r.txt")
        pl._write_atomic(path, line + "\n")
        self.assertEqual(pl.read_result(path), (line, "COMMAND_EXITED", 0))
        self.assertEqual(pl.read_result(os.path.join(self.tmp, "missing")), (None, None, None))


class LinuxExitEvidenceTest(TempDirTest):
    """How a run ended, as the supervisor reads it (B-supervisor-linux-paths-unverified,
    F-memory-aware-editor-launch)."""

    def scan(self, text):
        return pl._scan_log(self.write("e.log", text))

    def test_killed_verdict_ranks_below_timeout_cap_and_cef_race(self):
        self.assertEqual(pl.verdict_for("suite", False, False, -9, killed=True), "EDITOR_KILLED_EXTERNALLY")
        self.assertEqual(pl.verdict_for("command", False, False, -9, killed=True), "COMMAND_KILLED_EXTERNALLY")
        self.assertEqual(pl.verdict_for("suite", True, False, -9, killed=True), "TIMEOUT")
        self.assertEqual(pl.verdict_for("suite", False, True, -9, killed=True), "MEMORY_CAP_HIT")
        self.assertEqual(pl.verdict_for("suite", False, False, 1, cef_race=True, killed=True),
                         "EDITOR_STARTUP_CEF_RACE")

    def test_linux_kill_is_sigkill_only(self):
        # UE shuts down gracefully on SIGTERM (exit 143) and re-raises crash signals after logging.
        with mock.patch.object(pl.os, "name", "posix"):
            self.assertTrue(pl.killed_externally(-9))
            for code in (0, 1, 143, -11, -6, -15):
                self.assertFalse(pl.killed_externally(code), code)

    def test_windows_kill_is_a_nonzero_exit_with_no_crash_and_no_exit_request(self):
        stopped = self.scan(_DRAINED.replace("LogExit: Display: **** TestExit: Automation Test Queue Empty ****\n", ""))
        crashed = self.scan("LogWindows: Error: Fatal error: [File:x.cpp] [Line: 1]\n")
        exited = self.scan("LogWindows: FPlatformMisc::RequestExit(1, UEngine::Exec)\n")
        with mock.patch.object(pl.os, "name", "nt"):
            self.assertTrue(pl.killed_externally(1, stopped))
            self.assertFalse(pl.killed_externally(0, stopped))
            self.assertFalse(pl.killed_externally(3, crashed))
            self.assertFalse(pl.killed_externally(1, exited))
            self.assertFalse(pl.killed_externally(1, None))

    def test_scan_records_the_last_exit_request_and_the_first_crash_line(self):
        scan = self.scan(_DRAINED + "LogCore: FUnixPlatformMisc::RequestExit(1, FEngineLoop::Tick.GScopedTestExit)\n")
        self.assertIn(pl._TEST_EXIT_CALL_SITE, scan["exitRequestLine"])
        self.assertIsNone(scan["crashLine"])
        self.assertIn("Caught signal", self.scan("LogCore: Error: Caught signal 11 Segmentation fault\n")["crashLine"])

    def test_cap_seen_is_written_as_na(self):
        scan = self.scan(_TRUNCATED)
        scan["capSeenByEditor"] = "n/a"
        line = pl.format_suite_result("EDITOR_EXITED", 1024 ** 3, 0, "BelowNormal", 1, scan, 60, "/l/a.log")
        self.assertIn(" capSeenByEditor=n/a ", line)

    @unittest.skipIf(os.name == "nt", "the forced -TestExit exit is _exit(1) on Unix only")
    def test_forced_test_exit_of_a_drained_linux_run_is_not_a_failure(self):
        # Every drained suite on the Linux dev host exited 1 (UnixPlatformMisc.cpp RequestExit,
        # Force -> _exit(1)) and was reported EDITOR_EXIT_NONZERO capSeenByEditor=False.
        log = self.write("s/automation.log", _DRAINED +
                         "LogCore: FUnixPlatformMisc::RequestExit(1, FEngineLoop::Tick.GScopedTestExit)\n")
        result = _supervise_inline(self, [sys.executable, "-c", "raise SystemExit(1)"], kind="suite",
                                   capped=False, logPath=log)
        self.assertIn("verdict=EDITOR_EXITED ", result)
        self.assertIn(" exit=1 ", result)
        self.assertIn(" capSeenByEditor=n/a ", result)


def _supervise_inline(test, argv, kind="command", capped=True, cap_bytes=256 * 1024 ** 2, **extra):
    """Run supervise() in this process on a real child; returns the result line. The supervisor
    log goes to test.supervisor_log."""
    spec = dict({"argv": argv, "mode": "headless", "kind": kind, "capped": capped,
                 "capBytes": cap_bytes, "priority": "BelowNormal", "timeoutMinutes": 2,
                 "handoffPath": os.path.join(test.tmp, "handoff.json"),
                 "resultPath": os.path.join(test.tmp, "result.txt")}, **extra)
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        test.assertEqual(pl.supervise(spec), 0)
    test.supervisor_log = err.getvalue()
    with open(spec["resultPath"], encoding="utf-8") as fh:
        return fh.read().strip()


def _real_scope_available():
    if not sys.platform.startswith("linux"):
        return False
    return pl.systemd_scope_prefix(256 * 1024 ** 2)[0] is not None


@unittest.skipUnless(_real_scope_available(), "needs Linux with a delegated systemd --user memory controller")
class LinuxRealScopeTest(TempDirTest):
    """The Linux containment on a REAL systemd --user scope, no mocks: the paths
    B-supervisor-linux-paths-unverified found verified only against a fake /proc."""

    def test_scope_holds_the_popen_pid_tree_niced_grouped_and_reaped(self):
        report = os.path.join(self.tmp, "child.json")
        code = ("import json, os, subprocess, sys\n"
                "g = subprocess.Popen(['sleep', '300'])\n"
                "json.dump({'pid': os.getpid(), 'nice': os.nice(0), 'pgid': os.getpgid(0),"
                " 'cgroup': open('/proc/self/cgroup').read().strip(), 'grandchild': g.pid},"
                " open(sys.argv[1], 'w'))\n")
        result = _supervise_inline(self, [sys.executable, "-c", code, report])
        with open(report) as fh:
            child = json.load(fh)
        started = int(self.supervisor_log.split("started pid ")[1].split(":")[0])
        self.assertEqual(child["pid"], started)  # systemd-run --scope execs: the Popen pid is the child
        self.assertTrue(child["cgroup"].endswith(".scope"), child["cgroup"])
        self.assertEqual((child["nice"], child["pgid"]), (pl.NICE["BelowNormal"], child["pid"]))
        self.assertIn("verdict=COMMAND_EXITED ", result)
        self.assertIn(" cap_gb=0.25 ", result)  # memory.max read back as the cap
        stat = "/proc/%d/stat" % child["grandchild"]
        self.assertFalse(os.path.exists(stat) and open(stat).read().split()[2] != "Z",
                         "killpg did not reach the grandchild")

    def test_an_oom_kill_is_counted_although_the_scope_is_gone(self):
        # The child dies inside the first 2 s poll, so the in-loop read never saw the kill, and
        # systemd removes the emptied scope before the final read: oom_kill used to read 0 and
        # the run was reported as a plain non-zero exit.
        # The sleep lets linux_wait_for_scope find the scope first (a child dead before that is
        # reported uncapped), and still ends well inside the first poll.
        code = "import time\ntime.sleep(0.8)\nb = []\nwhile True: b.append(bytearray(16 << 20))\n"
        result = _supervise_inline(self, [sys.executable, "-c", code], cap_bytes=128 * 1024 ** 2)
        self.assertIn("verdict=MEMORY_CAP_HIT ", result)
        self.assertIn("exit=-9 ", result)
        self.assertRegex(self.supervisor_log, r"violation oom_kill=[1-9]")

    def test_a_sigkill_from_outside_is_named(self):
        pid_file = os.path.join(self.tmp, "pid")
        code = ("import os, sys, time\nopen(sys.argv[1], 'w').write(str(os.getpid()))\n"
                "time.sleep(60)\n")

        def kill_when_started():
            deadline = time.monotonic() + 30
            while not os.path.exists(pid_file) and time.monotonic() < deadline:
                time.sleep(0.1)
            time.sleep(0.3)
            with open(pid_file) as fh:
                os.kill(int(fh.read()), 9)  # this test's own child, never an editor

        killer = threading.Thread(target=kill_when_started)
        killer.start()
        result = _supervise_inline(self, [sys.executable, "-c", code, pid_file])
        killer.join()
        self.assertIn("verdict=COMMAND_KILLED_EXTERNALLY ", result)
        self.assertIn("killed from outside the run (exit -9", self.supervisor_log)


class SupervisedRunTest(TempDirTest):
    def make(self, live):
        """live: set of pids process_start_ms reports alive (start time 1000)."""
        run = pl.SupervisedRun("rid", 10, 1000, 20, 1000, os.path.join(self.tmp, "result.txt"),
                               os.path.join(self.tmp, "supervisor.log"), None, {}, ["x"])
        patcher = mock.patch.object(pl, "process_start_ms",
                                    side_effect=lambda pid: 1000 if pid in live else None)
        patcher.start()
        self.addCleanup(patcher.stop)
        return run

    def test_running_then_exited(self):
        live = {10, 20}
        run = self.make(live)
        self.assertIsNone(run.poll())
        live.discard(10)
        self.assertIsNone(run.poll())  # child gone, supervisor still writing the result
        self.write("result.txt", "PINWRIGHT_JOB_RESULT verdict=EDITOR_EXIT_NONZERO exit=3 priority=Normal\n")
        self.assertEqual(run.poll(), 3)
        self.assertEqual(run.verdict, "EDITOR_EXIT_NONZERO")
        self.assertEqual(run.wait(), 3)
        self.assertFalse(run.supervisor_lost)

    def test_supervisor_lost(self):
        run = self.make(set())
        self.assertIsNone(run.poll())  # inside the grace window
        run.LOST_GRACE_SECONDS = 0
        self.assertEqual(run.poll(), -1)
        self.assertTrue(run.supervisor_lost)
        self.assertEqual(run.returncode, -1)

    def test_pid_reuse_is_not_alive(self):
        run = self.make(set())
        with mock.patch.object(pl, "process_start_ms", return_value=1000 + 60000):
            run.LOST_GRACE_SECONDS = 0
            run.poll()
            self.assertEqual(run.poll(), -1)

    def test_wait_timeout_and_signals(self):
        run = self.make({10, 20})
        with self.assertRaises(subprocess.TimeoutExpired):
            run.wait(timeout=0.1)
        with mock.patch.object(pl.os, "kill") as kill:
            run.terminate()
            kill.assert_called_once_with(10, pl.SIGTERM)
        run_dead = self.make(set())
        with mock.patch.object(pl.os, "kill") as kill:
            run_dead.terminate()
            kill.assert_not_called()


class SpawnSupervisedTest(TempDirTest):
    def test_rejects_bad_arguments_before_spawning(self):
        with mock.patch.object(pl, "_supervisor_popen") as popen:
            for kwargs in ({"reason": "  "}, {"reason": "x", "kind": "bogus"},
                           {"reason": "x", "priority": "High"}, {"reason": "x", "memory_fraction": 0}):
                args = dict(kind="command", launched_by="editor_start", mode="offscreen")
                args.update(kwargs)
                with self.assertRaises(ValueError):
                    pl.spawn_supervised([sys.executable], **args)
            popen.assert_not_called()

    def test_end_to_end_with_harmless_child(self):
        output = os.path.join(self.tmp, "out", "child.txt")
        run = pl.spawn_supervised([sys.executable, "-c", "print('hello from child')"],
                                  kind="command", reason="unit test", launched_by="editor_start",
                                  mode="offscreen", memory_fraction=0.5, timeout_minutes=1,
                                  output_path=output, pid_wait_seconds=60)
        self.assertGreater(run.pid, 0)
        self.assertNotEqual(run.pid, run.supervisor_pid)
        self.assertEqual(run.wait(timeout=90), 0)
        self.assertEqual(run.verdict, "COMMAND_EXITED")
        self.assertEqual(run.result_path, output + ".result.txt")
        with open(output, encoding="utf-8", errors="replace") as fh:
            self.assertIn("hello from child", fh.read())
        if os.name == "nt":
            self.assertTrue(run.handoff["capped"])
            self.assertEqual(run.handoff["capMechanism"], "windows-job-object")
            # A real WMI launch: the supervisor is not this process's child.
            self.assertEqual((run.detached, run.launch_mechanism),
                             (True, "wmi-win32-process-create"), run.detach_note)
        else:
            self.assertEqual(run.handoff["capped"], run.handoff["capMechanism"] == "systemd-run-scope")
        # Let the supervisor finish exiting before the temp dir (its log) is removed.
        deadline = time.monotonic() + 30
        while pl._matches(run.supervisor_pid, run.supervisor_start_ms) and time.monotonic() < deadline:
            time.sleep(0.2)

    def test_start_failure_is_reported(self):
        with self.assertRaises(RuntimeError) as ctx:
            pl.spawn_supervised([os.path.join(self.tmp, "does-not-exist")], kind="command",
                                reason="unit test", launched_by="editor_start", mode="offscreen",
                                output_path=os.path.join(self.tmp, "o.txt"), pid_wait_seconds=60)
        self.assertIn("supervised start failed", str(ctx.exception))
        # The supervisor exits right after its handoff; on Windows its log stays unrenamable
        # (and the temp dir undeletable) until it has.
        log = os.path.join(self.tmp, "o.txt.supervisor.log")
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                os.rename(log, log + ".done")
                break
            except OSError:
                time.sleep(0.2)


class InternalEntryPointTest(TempDirTest):
    def test_there_is_no_public_cli(self):
        # The run / suite / status subcommands are gone; the module only answers --supervise.
        self.assertFalse(hasattr(pl, "build_parser"))
        for argv in (["run", "--reason", "x", "--mode", "visible", "--", sys.executable],
                     ["suite", "--reason", "x"], ["status", "--log", "x.log"], [], ["--help"]):
            with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(pl.main(argv), 2)

    def test_supervise_reads_the_spec_file_and_deletes_it(self):
        # No protocolVersion: the spec-file exchange as the previous revision wrote it. It is the
        # same protocol and must keep working, or saving this file breaks every running proxy.
        spec = {"kind": "command", "argv": ["x"],
                "supervisorLogPath": os.path.join(self.tmp, "s.log")}
        spec_path = self.write("spec.json", json.dumps(spec))
        stderr = sys.stderr
        try:
            with mock.patch.object(pl, "supervise", return_value=0) as supervise:
                self.assertEqual(pl.main([pl.SUPERVISE_FLAG, spec_path]), 0)
            sys.stderr.close()
        finally:
            sys.stderr = stderr
        supervise.assert_called_once_with(spec)
        self.assertFalse(os.path.exists(spec_path), "the spec carries the environment")

    def test_spawn_starts_the_supervisor_through_the_internal_entry_point(self):
        seen = {}

        def fake_start(cmd, supervisor_log):
            with open(cmd[3], encoding="utf-8") as fh:
                seen["spec"] = json.load(fh)
            seen["cmd"] = cmd
            pl._write_atomic(seen["spec"]["handoffPath"],
                             '{"pid": 4242, "processStartMs": 1, "capped": true}')
            return 77, "wmi-win32-process-create", True, None

        with mock.patch.object(pl, "_start_supervisor", side_effect=fake_start), \
                mock.patch.object(pl, "total_physical_bytes", return_value=16 * 1024 ** 3), \
                mock.patch.object(pl, "process_start_ms", return_value=1):
            run = pl.spawn_supervised(["Build.bat", "HostEditor"], kind="command", reason="build",
                                      launched_by="editor_build", mode=None,
                                      output_path=os.path.join(self.tmp, "b", "build.log"),
                                      output_header="reason: build")
        self.assertEqual(seen["cmd"][1:3], [os.path.abspath(pl.__file__), "--supervise"])
        spec = seen["spec"]
        self.assertIsNone(spec["mode"])
        self.assertEqual(spec["outputHeader"], "reason: build")
        self.assertEqual(spec["argv"], ["Build.bat", "HostEditor"])  # a build gets no editor switches
        self.assertEqual(spec["protocolVersion"], pl.PROTOCOL_VERSION)
        # Explicit even when the caller passed none: a WMI-started supervisor would otherwise
        # hand the child the user's default environment instead of the proxy's.
        self.assertEqual(spec["env"], dict(os.environ))
        self.assertFalse(os.path.exists(seen["cmd"][3]), "spec file removed after the handoff")
        self.assertFalse(os.path.exists(spec["handoffPath"]))
        self.assertEqual((run.pid, run.supervisor_pid), (4242, 77))
        self.assertEqual((run.detached, run.launch_mechanism, run.detach_note),
                         (True, "wmi-win32-process-create", None))

    def test_a_supervisor_that_dies_without_a_handoff_is_reported(self):
        with mock.patch.object(pl, "_start_supervisor",
                               return_value=(77, "setsid", True, None)), \
                mock.patch.object(pl, "total_physical_bytes", return_value=16 * 1024 ** 3), \
                mock.patch.object(pl, "process_start_ms", return_value=None):
            with self.assertRaises(RuntimeError) as ctx:
                pl.spawn_supervised(["x"], kind="command", reason="r", launched_by="editor_build",
                                    mode=None, output_path=os.path.join(self.tmp, "o.log"))
        self.assertIn("exited without a handoff", str(ctx.exception))


class SpawnRequestEntryPointTest(TempDirTest):
    """`--spawn <request.json>`: the multi-engine tooling entry point (mcp-version-matrix)."""

    SUITE = {"kind": "suite", "reason": "matrix T1 on UE 5.6", "launchedBy": "mcp-version-matrix",
             "mode": "offscreen", "uproject": "C:\\H56\\H56.uproject",
             "editorExe": "C:\\UE_5.6\\Engine\\Binaries\\Win64\\UnrealEditor.exe",
             "filter": "PinWright", "logPath": "C:\\H56\\Saved\\Logs\\H56.log",
             "extraArgs": ["-PinWrightTestGcEvery=25"]}

    def _run(self, request, spawn=None):
        # A BOM, as PowerShell 5.1's Set-Content -Encoding utf8 writes one.
        path = os.path.join(self.tmp, "request.json")
        with open(path, "w", encoding="utf-8-sig") as fh:
            json.dump(request, fh)
        fake = pl.SupervisedRun("r", 4242, 1, 77, 1, "res.txt", "sup.log", request.get("logPath"),
                                {"capped": True, "capBytes": 10, "priority": "BelowNormal",
                                 "commandLine": "cmd", "detached": True})
        out = io.StringIO()
        with mock.patch.object(pl, "spawn_supervised", side_effect=spawn, return_value=fake) as sp, \
                mock.patch.object(pl, "suite_executable", return_value="C:\\UE\\UnrealEditor-Cmd.exe"), \
                contextlib.redirect_stdout(out):
            code = pl.main([pl.SPAWN_FLAG, path])
        return code, json.loads(out.getvalue().splitlines()[-1]), sp

    def test_suite_request_launches_the_editor_run_tests_argv(self):
        code, answer, sp = self._run(self.SUITE)
        self.assertEqual(code, 0)
        argv = sp.call_args.args[0]
        self.assertEqual(argv, ["C:\\UE\\UnrealEditor-Cmd.exe"] + pl.suite_argv(
            self.SUITE["uproject"], "PinWright", self.SUITE["logPath"], "offscreen",
            extra_args=["-PinWrightTestGcEvery=25"]))
        self.assertEqual(sp.call_args.kwargs, {"kind": "suite", "reason": "matrix T1 on UE 5.6",
                                               "launched_by": "mcp-version-matrix",
                                               "mode": "offscreen",
                                               "log_path": self.SUITE["logPath"]})
        self.assertEqual((answer["pid"], answer["supervisorPid"], answer["resultPath"]),
                         (4242, 77, "res.txt"))

    def test_command_request_writes_the_reason_into_the_output(self):
        request = {"kind": "command", "reason": "matrix C2", "launchedBy": "mcp-version-matrix",
                   "argv": ["Build.bat", "HostEditor"], "outputPath": "C:\\H\\build.log"}
        code, _, sp = self._run(request)
        self.assertEqual(code, 0)
        self.assertEqual(sp.call_args.args[0], ["Build.bat", "HostEditor"])
        kwargs = sp.call_args.kwargs
        self.assertEqual((kwargs["kind"], kwargs["mode"], kwargs["output_path"]),
                         ("command", None, "C:\\H\\build.log"))
        self.assertIn("matrix C2", kwargs["output_header"])

    def test_mode_and_reason_have_no_default(self):
        for drop in ("mode", "reason", "launchedBy"):
            request = {k: v for k, v in self.SUITE.items() if k != drop}
            with self.subTest(drop=drop):
                code, answer, sp = self._run(request)
                self.assertEqual(code, 2)
                self.assertIn(drop, answer["error"])
                sp.assert_not_called()
        for bad in ({"mode": "fast"}, {"reason": "  "}, {"memoryFraction": 0.9}, {"kind": "run"}):
            with self.subTest(bad=bad):
                code, _, sp = self._run(dict(self.SUITE, **bad))
                self.assertEqual(code, 2)
                sp.assert_not_called()

    def test_a_protocol_skew_is_reported_by_code(self):
        def skew(*_args, **_kwargs):
            raise pl.SupervisorVersionMismatch(pl._mismatch_text(3))

        code, answer, _ = self._run(self.SUITE, spawn=skew)
        self.assertEqual(code, pl.EXIT_VERSION_MISMATCH)
        self.assertEqual(answer["code"], pl.VERSION_MISMATCH)


class ProtocolVersionTest(TempDirTest):
    """The proxy keeps this module in memory but launches the file on disk, so the two can differ
    (a proxy started before an edit). Every skew must say so by name, not as a bare refusal."""

    def test_a_protocol_1_caller_gets_the_reason_on_both_of_its_channels(self):
        # Exactly what a proxy loaded before the spec-file exchange does: `--supervise`, the spec
        # on stdin, one JSON handoff line read from stdout, stderr routed to supervisor.log.
        proc = subprocess.Popen([sys.executable, os.path.abspath(pl.__file__), pl.SUPERVISE_FLAG],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        out, err = proc.communicate(json.dumps({"kind": "command", "argv": ["x"]}).encode(),
                                    timeout=60)
        handoff = json.loads(out.decode().splitlines()[0])
        self.assertEqual(proc.returncode, pl.EXIT_VERSION_MISMATCH)
        self.assertEqual(handoff["code"], pl.VERSION_MISMATCH)
        # The old caller raises "supervised start failed: <error>", so the reason reaches its tool
        # result; and its supervisor.log gets the same line.
        self.assertIn("Restart the MCP server", handoff["error"])
        self.assertIn(pl.VERSION_MISMATCH, err.decode())

    def test_an_incompatible_spec_version_is_refused_by_name(self):
        log = os.path.join(self.tmp, "s.log")
        handoff = os.path.join(self.tmp, "handoff.json")
        spec_path = self.write("spec.json", json.dumps({
            "protocolVersion": 99, "supervisorLogPath": log, "handoffPath": handoff}))
        stderr = sys.stderr
        try:
            with mock.patch.object(pl, "supervise") as supervise:
                self.assertEqual(pl.main([pl.SUPERVISE_FLAG, spec_path]),
                                 pl.EXIT_VERSION_MISMATCH)
            sys.stderr.close()
        finally:
            sys.stderr = stderr
        supervise.assert_not_called()
        self.assertEqual(pl._read_json(handoff)["code"], pl.VERSION_MISMATCH)
        with open(log, encoding="utf-8") as fh:
            self.assertIn("protocol 99", fh.read())
        self.assertFalse(os.path.exists(spec_path))

    def _spawn(self, fake_start, start_ms=1):
        with mock.patch.object(pl, "_start_supervisor", side_effect=fake_start), \
                mock.patch.object(pl, "total_physical_bytes", return_value=16 * 1024 ** 3), \
                mock.patch.object(pl, "process_start_ms", return_value=start_ms):
            return pl.spawn_supervised(["x"], kind="command", reason="r",
                                       launched_by="editor_build", mode=None,
                                       output_path=os.path.join(self.tmp, "o.log"))

    def test_a_mismatch_handoff_raises_the_typed_error(self):
        def fake_start(cmd, _log):
            with open(cmd[3], encoding="utf-8") as fh:
                spec = json.load(fh)
            pl._handoff(spec, {"error": pl._mismatch_text(3), "code": pl.VERSION_MISMATCH})
            return 77, "setsid", True, None

        with self.assertRaises(pl.SupervisorVersionMismatch) as ctx:
            self._spawn(fake_start)
        self.assertIn("protocol 3", str(ctx.exception))

    def test_a_supervisor_that_dies_says_why_from_its_log(self):
        # A newer script whose handoff moved elsewhere still logs the marker: typed error.
        def logs(line):
            def fake_start(_cmd, supervisor_log):
                with open(supervisor_log, "a", encoding="utf-8") as fh:
                    fh.write("[t] something earlier\n%s\n" % line)
                return 77, "setsid", True, None
            return fake_start

        with self.assertRaises(pl.SupervisorVersionMismatch):
            self._spawn(logs(pl._mismatch_text(3)), start_ms=None)
        os.remove(os.path.join(self.tmp, "o.log.supervisor.log"))
        # Any other early death names the log's last line instead of only its path.
        with self.assertRaises(RuntimeError) as ctx:
            self._spawn(logs("Traceback: boom"), start_ms=None)
        self.assertNotIsInstance(ctx.exception, pl.SupervisorVersionMismatch)
        self.assertIn("ends: Traceback: boom", str(ctx.exception))


_TREE_KILL_PARENT = r"""
import json, os, sys, time
sys.path.insert(0, sys.argv[1])
import pinwright_supervisor as pl
if sys.argv[3] == "direct":
    def refuse(*_args):
        raise RuntimeError("forced off for the counterfactual")
    pl._wmi_create = refuse
run = pl.spawn_supervised(
    [sys.executable, "-c", "import time\nfor _ in range(120): time.sleep(1)"],
    kind="command", reason="tree-kill survival test", launched_by="editor_build", mode=None,
    timeout_minutes=5, output_path=sys.argv[2], pid_wait_seconds=60)
print(json.dumps({"supervisor": [run.supervisor_pid, run.supervisor_start_ms],
                  "child": [run.pid, run.process_start_ms], "detached": run.detached,
                  "mechanism": run.launch_mechanism}), flush=True)
time.sleep(120)
"""


@unittest.skipUnless(os.name == "nt", "the WMI launch and taskkill are Windows-only")
class TreeKillSurvivalTest(TempDirTest):
    """The real failure, reproduced without ending a session: a parent that stands in for the MCP
    proxy starts a supervised run, then `taskkill /PID <parent> /T /F` (what Claude Code runs on
    the trees it owns) kills the parent's tree. Every process here is started by this test."""

    def _launch(self, how):
        parent = subprocess.Popen(
            [sys.executable, "-c", _TREE_KILL_PARENT, os.path.dirname(os.path.abspath(pl.__file__)),
             os.path.join(self.tmp, how, "child.txt"), how],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        line = parent.stdout.readline()
        if not line:
            parent.wait(30)
            self.fail("parent produced no run: %s" % parent.stderr.read().decode(errors="replace"))
        run = json.loads(line)
        subprocess.run(["taskkill", "/PID", str(parent.pid), "/T", "/F"], capture_output=True)
        parent.wait(30)
        parent.stdout.close()
        parent.stderr.close()
        return run

    def _alive(self, pid_start):
        return pl._matches(*pid_start)

    def _wait_gone(self, *pid_starts, timeout=30):
        deadline = time.monotonic() + timeout
        while any(self._alive(p) for p in pid_starts) and time.monotonic() < deadline:
            time.sleep(0.2)
        return not any(self._alive(p) for p in pid_starts)

    def test_a_wmi_started_run_survives_a_tree_kill_of_its_caller(self):
        run = self._launch("wmi")
        try:
            self.assertEqual((run["detached"], run["mechanism"]), (True, "wmi-win32-process-create"))
            time.sleep(2)
            self.assertTrue(self._alive(run["supervisor"]), "supervisor died with the caller's tree")
            self.assertTrue(self._alive(run["child"]), "child died with the caller's tree")
            # Stop the child by pid; the supervisor must still be working and record the exit.
            os.kill(run["child"][0], pl.SIGTERM)
            self.assertTrue(self._wait_gone(run["supervisor"]), "supervisor did not finish")
            line, verdict, _code = pl.read_result(os.path.join(self.tmp, "wmi", "child.txt.result.txt"))
            self.assertIsNotNone(line, "the surviving supervisor wrote no verdict")
            self.assertEqual(verdict, "COMMAND_EXIT_NONZERO")
        finally:
            for pid_start in (run["child"], run["supervisor"]):
                if self._alive(pid_start):
                    os.kill(pid_start[0], pl.SIGTERM)
            self._wait_gone(run["child"], run["supervisor"])

    def test_the_old_direct_child_launch_dies_with_the_tree_counterfactual(self):
        # Proves the test above can fail: the pre-fix launch, forced here, does not survive.
        run = self._launch("direct")
        try:
            self.assertEqual((run["detached"], run["mechanism"]), (False, "createprocess-breakaway"))
            self.assertTrue(self._wait_gone(run["supervisor"], run["child"], timeout=15),
                            "a direct child survived a tree kill; the test proves nothing")
        finally:
            for pid_start in (run["child"], run["supervisor"]):
                if self._alive(pid_start):
                    os.kill(pid_start[0], pl.SIGTERM)


class SupervisorLaunchTest(TempDirTest):
    """How the supervisor process is started: it must not be a parent-pid descendant of the MCP
    client on Windows, and a fallback must say it is not detached."""

    CMD = [sys.executable, os.path.abspath(pl.__file__), "--supervise", "spec.json"]

    def test_windows_launches_through_wmi(self):
        with mock.patch.object(pl.os, "name", "nt"), \
                mock.patch.object(pl, "_wmi_create", return_value=5150) as wmi, \
                mock.patch.object(pl, "_supervisor_popen") as popen:
            result = pl._start_supervisor(self.CMD, os.path.join(self.tmp, "s.log"))
        self.assertEqual(result, (5150, "wmi-win32-process-create", True, None))
        self.assertEqual(wmi.call_args.args[0], subprocess.list2cmdline(self.CMD))
        popen.assert_not_called()

    def _fallback(self, mechanism):
        proc = mock.MagicMock(pid=6160)
        with mock.patch.object(pl.os, "name", "nt"), \
                mock.patch.object(pl, "_wmi_create",
                                  side_effect=RuntimeError("Win32_Process.Create returned 2")), \
                mock.patch.object(pl, "_supervisor_popen", return_value=(proc, mechanism)):
            return pl._start_supervisor(self.CMD, os.path.join(self.tmp, "s.log"))

    def test_windows_wmi_failure_falls_back_and_says_not_detached(self):
        pid, mechanism, detached, note = self._fallback("createprocess-breakaway")
        self.assertEqual((pid, mechanism, detached), (6160, "createprocess-breakaway", False))
        self.assertIn("Win32_Process.Create returned 2", note)
        self.assertIn("dies with the MCP client", note)

    def test_refused_breakaway_is_named_in_the_note(self):
        note = self._fallback("createprocess-in-caller-job")[3]
        self.assertIn("refused breakaway", note)

    def test_linux_keeps_setsid_and_never_calls_wmi(self):
        proc = mock.MagicMock(pid=7)
        with mock.patch.object(pl.os, "name", "posix"), \
                mock.patch.object(pl, "_wmi_create") as wmi, \
                mock.patch.object(pl, "_supervisor_popen", return_value=(proc, "setsid")):
            self.assertEqual(pl._start_supervisor(self.CMD, os.path.join(self.tmp, "s.log")),
                             (7, "setsid", True, None))
        wmi.assert_not_called()

    def test_wmi_create_parses_the_pid_and_refuses_failures(self):
        def run(stdout, returncode=0):
            done = subprocess.CompletedProcess([], returncode, stdout=stdout, stderr="boom")
            with mock.patch.object(pl.subprocess, "run", return_value=done) as call:
                return pl._wmi_create('"C:\\py.exe" x.py --supervise s.json', "C:\\dir"), call

        pid, call = run("0 4242\r\n")
        self.assertEqual(pid, 4242)
        env = call.call_args.kwargs["env"]
        self.assertEqual(env["PINWRIGHT_WMI_COMMAND"], '"C:\\py.exe" x.py --supervise s.json')
        self.assertEqual(int(env["PINWRIGHT_WMI_FLAGS"]),
                         pl.DETACHED_PROCESS | pl.CREATE_NEW_PROCESS_GROUP)
        with self.assertRaises(RuntimeError) as ctx:
            run("9 0\r\n")
        self.assertIn("returned 9", str(ctx.exception))
        with self.assertRaises(RuntimeError) as ctx:
            run("", returncode=1)
        self.assertIn("boom", str(ctx.exception))

    def test_mode_may_be_omitted_only_for_a_plain_command(self):
        with mock.patch.object(pl, "_supervisor_popen") as popen:
            for kind in ("editor", "suite"):
                with self.assertRaises(ValueError):
                    pl.spawn_supervised([sys.executable], kind=kind, reason="x",
                                        launched_by="editor_start", mode=None)
            popen.assert_not_called()

    def test_output_header_precedes_the_child_output(self):
        output = os.path.join(self.tmp, "h", "out.log")
        run = pl.spawn_supervised([sys.executable, "-c", "print('child line')"],
                                  kind="command", reason="unit test", launched_by="editor_build",
                                  mode=None, timeout_minutes=1, output_path=output,
                                  output_header="PinWright build reason: unit test",
                                  pid_wait_seconds=60)
        self.assertEqual(run.wait(timeout=90), 0)
        with open(output, encoding="utf-8", errors="replace") as fh:
            lines = fh.read().splitlines()
        self.assertEqual(lines[0], "PinWright build reason: unit test")
        self.assertIn("child line", lines[1:])
        deadline = time.monotonic() + 30
        while pl._matches(run.supervisor_pid, run.supervisor_start_ms) and time.monotonic() < deadline:
            time.sleep(0.2)


if __name__ == "__main__":
    unittest.main()
