# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Editor-launch serialization against the CEF cache-dir race and the typed CEF startup death.

Pure stdlib; spawning is faked except one real child process that holds a file lock. No Unreal
process is started.
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import check_suite_log  # noqa: E402
import mcp_proxy  # noqa: E402
import pinwright_supervisor as pl  # noqa: E402
from mcp_proxy import Proxy  # noqa: E402

GPU = "[2026.09.29-10.10.20:100][  0]LogCEFBrowser: CEF GPU acceleration disabled"
RACE = ("[2026.09.29-10.10.37:810][  0]LogWebBrowser: Warning: Detected concurrent CEF "
        "initialization for cache dir C:/Users/u/AppData/Local/UnrealEngine/Host/webcache_6613_1!"
        " Retrying... (1 of 3)")
LOADED = "[2026.09.29-10.10.20:090][  0]LogWebBrowser: Loaded CEF3 version 128.4.13.3057 from "
NEXT = "[2026.09.29-10.10.21:000][  0]LogModuleManager: InternalLoadLibrary: 'Soundscape'"
HEAD = "Log file open, 09/29/26 13:10:16\nLogInit: Display: Running engine for game: Host"


class _Temp(unittest.TestCase):
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


class FileLockTest(_Temp):
    def test_second_holder_is_refused_until_the_first_unlocks(self):
        path = os.path.join(self.tmp, "sub", "x.lock")
        first = pl.try_lock_file(path)
        self.assertIsNotNone(first)
        self.assertIsNone(pl.try_lock_file(path))
        pl.unlock_file(first)
        again = pl.try_lock_file(path)
        self.assertIsNotNone(again)
        pl.unlock_file(again)

    def test_the_os_drops_the_lock_when_its_holder_process_dies(self):
        path = os.path.join(self.tmp, "x.lock")
        script = ("import sys; sys.path.insert(0, %r); import pinwright_supervisor as p; "
                  "h = p.try_lock_file(%r); print('locked' if h else 'busy', flush=True); "
                  "sys.stdin.read()" % (os.path.dirname(os.path.abspath(pl.__file__)), path))
        child = subprocess.Popen([sys.executable, "-c", script], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(child.stdout.readline().strip(), "locked")
            self.assertIsNone(pl.try_lock_file(path))
        finally:
            child.kill()
            child.wait()
            child.stdout.close()
            child.stdin.close()
        handle = pl.try_lock_file(path)
        self.assertIsNotNone(handle)
        pl.unlock_file(handle)


class CefLogClassificationTest(_Temp):
    def test_race_line_only_when_it_is_the_last_cef_line(self):
        self.assertEqual(pl.cef_race_line([LOADED, GPU, RACE]), RACE)
        # The retry reached CefInitialize again: the race was survived.
        self.assertIsNone(pl.cef_race_line([GPU, RACE, NEXT, GPU, NEXT]))
        self.assertIsNone(pl.cef_race_line([NEXT]))

    def test_settled(self):
        cases = (
            ([LOADED, GPU, NEXT], True),
            ([LOADED, GPU], False),
            ([GPU, RACE, NEXT], False),
            ([GPU, RACE, NEXT, LOADED, GPU, NEXT], True),
            ([NEXT, "LogInit: Display: Engine is initialized. Leaving FEngineLoop::Init()"], True),
            ([NEXT], False),
        )
        for lines, settled in cases:
            with self.subTest(lines=lines):
                self.assertEqual(pl.cef_settled(self.write("a.log", "\n".join(lines))), settled)
        self.assertFalse(pl.cef_settled(os.path.join(self.tmp, "missing.log")))

    def test_launches_cef(self):
        self.assertTrue(pl.launches_cef(["UnrealEditor", "-RenderOffScreen"]))
        self.assertFalse(pl.launches_cef(["UnrealEditor", "-NullRHI"]))
        self.assertFalse(pl.launches_cef(["UnrealEditor", "-nocef"]))

    def test_supervisor_verdict_names_the_cef_death(self):
        self.assertEqual(pl.verdict_for("suite", False, False, 777003, cef_race=True),
                         "EDITOR_STARTUP_CEF_RACE")
        self.assertEqual(pl.verdict_for("suite", False, True, 777003, cef_race=True),
                         "MEMORY_CAP_HIT")
        self.assertEqual(pl.verdict_for("suite", False, False, 0, cef_race=True), "EDITOR_EXITED")
        scan = pl._scan_log(self.write("s.log", "\n".join([HEAD, GPU, RACE])))
        self.assertEqual(scan["cefRaceLine"], RACE)
        started = "LogAutomationController: Display: Test Started. Name={x} Path={PinWright.x}"
        self.assertIsNone(pl._scan_log(
            self.write("t.log", "\n".join([HEAD, GPU, RACE, started])))["cefRaceLine"])

    def test_check_suite_log_names_the_startup_death(self):
        path = self.write("automation.log", "\n".join([HEAD, GPU, RACE]))
        verdict = check_suite_log.check_log(path, crash_scan=False)
        self.assertEqual(verdict["state"], "DID_NOT_COMPLETE")
        self.assertIn("CEF", verdict["reason"])
        self.assertIn("relaunch", verdict["reason"])
        plain = check_suite_log.check_log(self.write("p.log", "\n".join([HEAD, GPU, NEXT])),
                                          crash_scan=False)
        self.assertNotIn("CEF", plain["reason"])


class _Exited:
    pid = 4321

    def __init__(self, code):
        self.code = code

    def poll(self):
        return self.code


class LaunchLockTest(_Temp):
    def setUp(self):
        super().setUp()
        self.lock_path = os.path.join(self.tmp, "editor-launch.lock")
        patcher = mock.patch("mcp_proxy._launch_lock_path", return_value=self.lock_path)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.proxy = Proxy(None, 0.1, 0.1, 0.1, None, None)
        self.proxy.launch_lock_wait = 0.0

    def hold(self):
        handle = pl.try_lock_file(self.lock_path)
        self.addCleanup(pl.unlock_file, handle)
        return handle

    def test_a_held_lock_refuses_a_cef_launch_with_a_typed_error(self):
        self.hold()
        lock, error = self.proxy._acquire_launch_lock(["UnrealEditor", "-RenderOffScreen"], None)
        self.assertIsNone(lock)
        self.assertEqual(error["structuredContent"]["error"], "EDITOR_LAUNCH_QUEUE_TIMEOUT")
        self.assertEqual(error["structuredContent"]["lockPath"], self.lock_path)

    def test_a_launch_without_cef_never_waits(self):
        self.hold()
        lock, error = self.proxy._acquire_launch_lock(["UnrealEditor", "-NullRHI"], None)
        self.assertIsNone(error)
        self.assertIsNone(lock.handle)

    def test_editor_run_tests_does_not_spawn_while_another_launch_holds_the_lock(self):
        self.hold()
        project = self.write("Host/Host.uproject", '{"EngineAssociation": "5.8"}')
        proxy = Proxy(None, 0.1, 0.1, 0.1, None, None, uproject=project)
        proxy.launch_lock_wait = 0.0
        exe = os.path.join(self.tmp, "UnrealEditor")
        with mock.patch.object(proxy, "_probe_state", return_value=("not_running", "refused")), \
                mock.patch("mcp_proxy.resolve_editor", return_value=(self.tmp, exe)), \
                mock.patch("mcp_proxy.os.path.isfile", return_value=True), \
                mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                           side_effect=AssertionError("must not spawn")):
            result = proxy._editor_run_tests(
                {"filter": "Project", "reason": "unit test", "mode": "offscreen"})
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_LAUNCH_QUEUE_TIMEOUT")
        self.assertIn("logPath", result["structuredContent"])

    def test_the_lock_is_released_once_the_fresh_log_is_past_cef(self):
        log = self.write("run.log", "\n".join([HEAD, GPU, NEXT]))
        old = time.time() - 60
        os.utime(log, (old, old))  # a reused -Abslog from an earlier run: not this editor's yet
        lock, _error = self.proxy._acquire_launch_lock(["UnrealEditor"], log)
        lock.release_if_settled()
        self.assertIsNone(pl.try_lock_file(self.lock_path))
        self.write("run.log", "\n".join([HEAD, GPU]))
        lock.release_if_settled()
        self.assertIsNone(pl.try_lock_file(self.lock_path))
        self.write("run.log", "\n".join([HEAD, GPU, NEXT]))
        lock.release_if_settled()
        self.assertIsNone(lock.handle)
        handle = pl.try_lock_file(self.lock_path)
        self.assertIsNotNone(handle)
        pl.unlock_file(handle)

    def test_a_test_editor_dead_in_the_cef_retry_is_a_typed_startup_error(self):
        log = self.write("automation.log", "\n".join([HEAD, GPU, RACE]))
        result = self.proxy._wait_for_tests_started(_Exited(777003), {"logPath": log})
        structured = result["structuredContent"]
        self.assertEqual(structured["error"], "EDITOR_STARTUP_CEF_RACE")
        self.assertEqual(structured["cefLine"], RACE)
        self.assertEqual(structured["exitCode"], 777003)
        self.write("automation.log", "\n".join([HEAD, GPU, NEXT]))
        result = self.proxy._wait_for_tests_started(_Exited(1), {"logPath": log})
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_EXITED_BEFORE_TESTS")

    def test_an_editor_start_dead_in_the_cef_retry_is_a_typed_startup_error(self):
        log = self.write("editor.log", "\n".join([HEAD, GPU, RACE]))
        result = self.proxy._wait_for_ready(_Exited(777003), "cmd", ["-Abslog=" + log])
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_STARTUP_CEF_RACE")
        result = self.proxy._wait_for_ready(_Exited(777003), "cmd", [])
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_EXITED_BEFORE_READY")
