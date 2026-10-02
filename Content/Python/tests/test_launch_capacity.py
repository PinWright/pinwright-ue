# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Machine-wide launch refusals (F-memory-aware-editor-launch), the EDITOR_ALREADY_RUNNING owner
and slot_wait (F-multi-editor-per-checkout), and the killedExternally status fields.

Pure stdlib; the process census, memory figures, probe and supervisor are faked, so nothing is
launched and the host's real memory and editors never decide an outcome.
"""
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import mcp_proxy  # noqa: E402
import pinwright_supervisor  # noqa: E402
from mcp_proxy import Proxy  # noqa: E402

GIB = 1024 ** 3
URL = "http://127.0.0.1:27001/mcp"
REASON = "unit test: launch capacity"


def _row(pid, project, launched_by=None, extra=()):
    argv = ["/e/Engine/Binaries/Linux/UnrealEditor", project] + list(extra)
    if launched_by:
        argv += pinwright_supervisor.launch_identity_args("work on " + project, launched_by)
    return {"pid": pid, "exe": argv[0], "argv": argv, "commandLine": " ".join(argv),
            "startMs": 1700000000000 + pid, "cwd": None}


def _proxy(uproject=None):
    return Proxy(URL, list_timeout=0.1, call_timeout=0.1, probe_timeout=0.1, token_file=None,
                 port_file=None, editor_exe=None, start_timeout=0.01, uproject=uproject)


class _Env(unittest.TestCase):
    def env(self, **values):
        patcher = mock.patch.dict(os.environ, {key: str(value) for key, value in values.items()})
        patcher.start()
        self.addCleanup(patcher.stop)

    def memory(self, available_gib, commit_gib=None, total_gib=64):
        commit = None if commit_gib is None else int(commit_gib * GIB)
        for name, value in (("available_memory_bytes", (int(available_gib * GIB), commit)),
                            ("total_physical_bytes", int(total_gib * GIB))):
            patcher = mock.patch.object(pinwright_supervisor, name, return_value=value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def census(self, rows):
        patcher = mock.patch("mcp_proxy._editor_processes", return_value=rows)
        patcher.start()
        self.addCleanup(patcher.stop)


class LaunchCapacityGuardTest(_Env):
    def setUp(self):
        self.env(PINWRIGHT_LAUNCH_RESERVE_GB=3, PINWRIGHT_MAX_EDITORS=0)
        self.census([_row(11, "/a/A.uproject", "editor_run_tests"), _row(12, "/b/B.uproject")])

    def test_low_available_memory_refuses_with_numbers_and_the_running_editors(self):
        self.memory(available_gib=10)
        result = _proxy()._launch_capacity_guard("suite")
        structured = result["structuredContent"]
        self.assertEqual(structured["error"], "LAUNCH_MEMORY_LOW")
        self.assertEqual(structured["neededBytes"], 19 * GIB)  # 16 GiB suite peak + 3 GiB reserve
        self.assertEqual([e["pid"] for e in structured["running"]], [11, 12])
        text = result["content"][0]["text"]
        self.assertIn("available physical memory is 10.0 GiB", text)
        self.assertIn("pid 11 A visible by editor_run_tests", text)  # no window switches = visible

    def test_enough_memory_passes(self):
        self.memory(available_gib=40)
        self.assertIsNone(_proxy()._launch_capacity_guard("suite"))
        self.assertIsNone(_proxy()._launch_capacity_guard("editor"))

    def test_low_windows_commit_refuses_although_physical_memory_is_free(self):
        self.memory(available_gib=40, commit_gib=5)
        result = _proxy()._launch_capacity_guard("editor")
        self.assertEqual(result["structuredContent"]["error"], "LAUNCH_MEMORY_LOW")
        self.assertIn("available commit is 5.0 GiB", result["content"][0]["text"])

    def test_a_small_machine_needs_at_most_its_cap_plus_the_reserve(self):
        # 16 GiB machine: a capped suite cannot exceed 60% = 9.6 GiB, so 12.6 GiB is needed.
        self.memory(available_gib=13, total_gib=16)
        self.assertIsNone(_proxy()._launch_capacity_guard("suite"))
        self.memory(available_gib=12, total_gib=16)
        self.assertEqual(_proxy()._launch_capacity_guard("suite")["structuredContent"]["error"],
                         "LAUNCH_MEMORY_LOW")

    def test_a_negative_reserve_disables_the_memory_half(self):
        self.env(PINWRIGHT_LAUNCH_RESERVE_GB=-1)
        self.memory(available_gib=1)
        self.assertIsNone(_proxy()._launch_capacity_guard("suite"))

    def test_the_editor_cap_counts_pinwright_launched_editors_only_and_spares_builds(self):
        self.memory(available_gib=100)
        self.env(PINWRIGHT_MAX_EDITORS=1)  # pid 11 is PinWright-launched, pid 12 is not
        result = _proxy()._launch_capacity_guard("editor")
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_LIMIT_REACHED")
        self.assertEqual(result["structuredContent"]["maxEditors"], 1)
        self.assertIsNone(_proxy()._launch_capacity_guard("build"))
        self.env(PINWRIGHT_MAX_EDITORS=2)
        self.assertIsNone(_proxy()._launch_capacity_guard("suite"))

    def test_available_memory_reads_memavailable(self):
        with tempfile.TemporaryDirectory() as temp:
            path = os.path.join(temp, "meminfo")
            with open(path, "w") as fh:
                fh.write("MemTotal:       131954888 kB\nMemFree:  1 kB\nMemAvailable:   1048576 kB\n")
            with mock.patch.object(pinwright_supervisor.os, "name", "posix"):
                self.assertEqual(pinwright_supervisor.available_memory_bytes(path), (GIB, None))


class LaunchCapacityWiringTest(_Env):
    """Each launch verb consults the guard before spawning anything."""

    REFUSAL = {"content": [{"type": "text", "text": "LAUNCH_MEMORY_LOW: x"}],
               "structuredContent": {"error": "LAUNCH_MEMORY_LOW"}, "isError": True}

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.uproject = os.path.join(temp.name, "Host.uproject")
        with open(self.uproject, "w") as fh:
            fh.write('{"EngineAssociation": "5.8"}')
        self.census([])
        for target, value in (("mcp_proxy.pinwright_supervisor.spawn_supervised",
                               AssertionError("must not spawn")),
                              ("mcp_proxy.subprocess.Popen", AssertionError("must not spawn"))):
            patcher = mock.patch(target, side_effect=value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def refused(self, call, kind):
        proxy = _proxy(self.uproject)
        with mock.patch.object(proxy, "_probe_state", return_value=("not_running", "refused")), \
                mock.patch.object(proxy, "_launch_capacity_guard",
                                  return_value=self.REFUSAL) as guard, \
                mock.patch("mcp_proxy.resolve_editor", return_value=("/e", "/e/UnrealEditor")), \
                mock.patch("mcp_proxy.os.path.isfile", return_value=True):
            result = call(proxy)
        self.assertEqual(result["structuredContent"]["error"], "LAUNCH_MEMORY_LOW")
        self.assertEqual(guard.call_args[0][0], kind)

    def test_editor_start(self):
        self.refused(lambda p: p._editor_start({"mode": "offscreen", "reason": REASON}), "editor")

    def test_editor_run_tests(self):
        self.refused(lambda p: p._editor_run_tests(
            {"filter": "PinWright", "mode": "offscreen", "reason": REASON}), "suite")

    def test_editor_build(self):
        self.refused(lambda p: p._editor_build({"reason": REASON}), "build")


class SlotOwnerTest(_Env):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = temp.name
        os.makedirs(os.path.join(self.root, "Saved", "PinWright"))
        with open(os.path.join(self.root, "Saved", "PinWright", "gateway-port"), "w") as fh:
            fh.write("27001")
        self.project = os.path.join(self.root, "Host.uproject")
        with open(self.project, "w") as fh:
            fh.write("{}")

    def guard(self, state="alive", identity_pid=None):
        proxy = _proxy(self.project)
        with mock.patch.object(proxy, "_probe_state", return_value=(state, None)), \
                mock.patch.object(proxy, "_identity_pid", return_value=identity_pid):
            return proxy._editor_process_guard()

    def test_the_owner_is_the_editor_answering_identity(self):
        self.census([_row(21, self.project, "editor_start", ["-RenderOffScreen"]),
                     _row(22, self.project, "editor_run_tests")])
        result = self.guard(identity_pid=21)
        owner = result["structuredContent"]["owner"]
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_ALREADY_RUNNING")
        self.assertEqual((owner["pid"], owner["launchedBy"], owner["mode"]),
                         (21, "editor_start", "offscreen"))
        self.assertEqual(owner["reason"], "work on " + self.project)
        self.assertIn("UnrealEditor", owner["commandLine"])
        text = result["content"][0]["text"]
        self.assertIn("held by pid 21 (launchedBy editor_start, mode offscreen", text)
        self.assertIn("slot_wait", text)

    def test_a_starting_editor_is_named_from_the_census_alone(self):
        self.census([_row(31, self.project, "editor_run_tests"), _row(32, "/other/O.uproject")])
        owner = self.guard(state="not_ready")["structuredContent"]["owner"]
        self.assertEqual(owner["pid"], 31)

    def test_two_editors_on_the_port_without_identity_name_no_owner(self):
        self.census([_row(41, self.project, "editor_start"), _row(42, self.project)])
        result = self.guard(identity_pid=None)
        self.assertIsNone(result["structuredContent"]["owner"])
        self.assertIn("owner not identified", result["content"][0]["text"])

    def test_run_tests_refusal_carries_the_owner_too(self):
        self.census([_row(51, self.project, "editor_start")])
        proxy = _proxy(self.project)
        with mock.patch.object(proxy, "_probe_state", return_value=("alive", None)), \
                mock.patch.object(proxy, "_identity_pid", return_value=51):
            result = proxy._editor_run_tests(
                {"filter": "PinWright", "mode": "offscreen", "reason": REASON})
        self.assertEqual(result["structuredContent"]["owner"]["pid"], 51)


class SlotWaitTest(unittest.TestCase):
    def test_waits_until_the_slot_frees_then_the_guard_decides(self):
        proxy = _proxy()
        states = iter(["alive", "not_ready", "not_running", "not_running"])
        with mock.patch.object(proxy, "_probe_state", side_effect=lambda _url: (next(states), None)), \
                mock.patch.object(proxy._shutdown_requested, "wait", return_value=False) as sleep:
            self.assertIsNone(proxy._wait_for_free_slot({"slot_wait": 60}))
            self.assertIsNone(proxy._editor_process_guard())
        self.assertEqual(sleep.call_count, 2)

    def test_no_slot_wait_does_not_probe(self):
        proxy = _proxy()
        with mock.patch.object(proxy, "_probe_state", side_effect=AssertionError("probed")):
            self.assertIsNone(proxy._wait_for_free_slot({}))

    def test_invalid_values_are_refused(self):
        for value in (-1, 3601, "10", True):
            result = _proxy()._wait_for_free_slot({"slot_wait": value})
            self.assertEqual(result["structuredContent"]["param"], "slot_wait", value)


class KilledExternallyStatusTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.dir = temp.name

    def write(self, name, text):
        path = os.path.join(self.dir, name)
        with open(path, "w") as fh:
            fh.write(text)
        return path

    def test_test_status_names_an_external_kill_and_when_the_log_stopped(self):
        log = self.write("automation.log", "LogAutomationController: Display: Test Started. "
                                            "Name={A} Path={PinWright.a.A}\n")
        os.utime(log, (1700000000, 1700000000))
        self.write("automation.log.result.txt",
                   "PINWRIGHT_SUITE_RESULT verdict=EDITOR_KILLED_EXTERNALLY cap_gb=1 peak_gb=1 "
                   "priority=BelowNormal exit=-9 log=%s\n" % log)
        with mock.patch("mcp_proxy._editor_processes", return_value=[]):
            result = _proxy()._editor_test_status({"logPath": log})
        structured = result["structuredContent"]
        self.assertTrue(structured["killedExternally"])
        self.assertEqual(structured["logStoppedAt"], "2023-11-14T22:13:20Z")
        self.assertIn("KILLED FROM OUTSIDE", result["content"][0]["text"])

    def test_build_state_names_an_external_kill_and_a_normal_failure_does_not(self):
        log = self.write("build.log", "compiling\n")
        self.write("build.log.result.txt",
                   "PINWRIGHT_JOB_RESULT verdict=COMMAND_KILLED_EXTERNALLY exit=-9 priority=Normal\n")
        self.assertTrue(mcp_proxy.build_state(log)["killedExternally"])
        self.write("build.log.result.txt",
                   "PINWRIGHT_JOB_RESULT verdict=COMMAND_EXIT_NONZERO exit=6 priority=Normal\n")
        state = mcp_proxy.build_state(log)
        self.assertFalse(state["killedExternally"])
        self.assertNotIn("logStoppedAt", state)


if __name__ == "__main__":
    unittest.main()
