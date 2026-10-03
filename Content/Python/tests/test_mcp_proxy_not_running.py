# Copyright (c) 2026 Alexander Penkin. MIT License.

"""EDITOR_NOT_RUNNING tells a never-started editor from one that served and died
(E-editor-not-running-cannot-distinguish-crash), and prescribes editor_start only for the former.

Pure stdlib on a temp project; the probe is faked as connection refused.
"""
import json
import os
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import mcp_proxy  # noqa: E402
from mcp_proxy import Proxy  # noqa: E402

URL = "http://127.0.0.1:27145/mcp"

LINUX_CRASH = (
    "[2026.09.30-18.49.50:000][100]LogPinWright: serving\n"
    "[2026.09.30-18.49.53:161][138]LogCore: === Critical error: ===\n"
    "Unhandled Exception: SIGSEGV: invalid attempt to write memory at address 0x0000000000000038\n"
    "\n"
    "[2026.09.30-18.49.53:161][138]LogCore: Fatal error!\n"
    "\n"
    "0x00007434a345dda2 libUnrealEditor-UMG.so!UUserWidget::RebuildWidget() [/e/UserWidget.cpp:1]\n"
    "0x00007434a345ddff libUnrealEditor-Core.so!Other::Frame() [/e/Other.cpp:2]\n"
)
WINDOWS_CRASH = (
    "[2026.09.02-19.44.57:001][  0]LogWindows: Error: === Critical error: ===\n"
    "[2026.09.02-19.44.57:001][  0]LogWindows: Error: \n"
    "[2026.09.02-19.44.57:001][  0]LogWindows: Error: Fatal error!\n"
    "[2026.09.02-19.44.57:001][  0]LogWindows: Error: \n"
    "[2026.09.02-19.44.57:001][  0]LogWindows: Error: Unhandled Exception: "
    "EXCEPTION_ACCESS_VIOLATION reading address 0x0000000000000038\n"
    "[2026.09.02-19.44.57:001][  0]LogWindows: Error: \n"
    "[2026.09.02-19.44.57:001][  0]LogWindows: Error: [Callstack] 0x00007ffb12345678 "
    "UnrealEditor-UMG.dll!UUserWidget::RebuildWidget() [D:\\UserWidget.cpp:1]\n"
)


class EditorNotRunningEvidenceTest(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.root = self._temp.name
        with open(os.path.join(self.root, "Demo.uproject"), "w") as fh:
            fh.write("{}")
        os.makedirs(os.path.join(self.root, "Saved", "PinWright"))
        os.makedirs(os.path.join(self.root, "Saved", "Logs"))
        self.port_file = os.path.join(self.root, "Saved", "PinWright", "gateway-port")
        self.log = os.path.join(self.root, "Saved", "Logs", "Demo.log")

    def tearDown(self):
        self._temp.cleanup()

    def _write(self, path, text):
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)

    def _log(self, path, body, opened_offset=-60):
        """A log whose `Log file open` header (local time) is opened_offset s from now."""
        header = time.strftime("Log file open, %m/%d/%y %H:%M:%S\n",
                               time.localtime(time.time() + opened_offset))
        self._write(path, header + body)

    def _call(self):
        proxy = Proxy(URL, 0.1, 0.1, 0.1, None, self.port_file)
        with mock.patch.object(proxy, "_probe_state",
                               return_value=("not_running", "connection refused")):
            result = proxy.handle({"jsonrpc": "2.0", "id": 7, "method": "tools/call",
                                   "params": {"name": "call", "arguments": {}}})["result"]
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_NOT_RUNNING")
        return result["content"][0]["text"], result["structuredContent"]["lastSession"]

    def test_no_breadcrumb_is_never_started_and_prescribes_editor_start(self):
        text, session = self._call()
        self.assertEqual(session["state"], "never_started")
        self.assertTrue(text.startswith("EDITOR_NOT_RUNNING (never started)"), text)
        self.assertIn("Start it with the editor_start MCP tool", text)
        self.assertIn("(connection refused)", text)

    def test_linux_crash_names_reason_frame_log_and_last_job_without_start_imperative(self):
        self._write(self.port_file, "27145")
        self._write(os.path.join(self.root, "Saved", "PinWright", "jobs.jsonl"),
                    json.dumps({"ts": "2026-09-30T18:49:00Z", "ticket_id": "j_1",
                                "method": "asset.list", "event": "started"}) + "\n" +
                    json.dumps({"ts": "2026-09-30T18:49:52Z", "ticket_id": "j_2",
                                "method": "python.execute", "event": "completed"}) + "\n")
        self._log(self.log, LINUX_CRASH)
        text, session = self._call()
        self.assertEqual(session["state"], "crashed")
        self.assertEqual(session["lastPort"], 27145)
        self.assertEqual(session["logPath"], self.log)
        self.assertEqual(session["crashFrame"], "UUserWidget::RebuildWidget")
        self.assertIn("SIGSEGV", session["crashReason"])
        self.assertEqual(session["lastJob"]["ticket_id"], "j_2")
        self.assertTrue(text.startswith("EDITOR_NOT_RUNNING (crashed)"), text)
        self.assertIn("at UUserWidget::RebuildWidget", text)
        self.assertIn(self.log, text)
        self.assertIn("j_2 (python.execute)", text)
        self.assertIn("report this rather than start or restart it", text)
        self.assertNotIn("Start it with the editor_start", text)

    def test_windows_critical_error_block(self):
        self._write(self.port_file, "27145")
        self._log(self.log, WINDOWS_CRASH)
        session = self._call()[1]
        self.assertEqual(session["crashReason"],
                         "Unhandled Exception: EXCEPTION_ACCESS_VIOLATION reading address "
                         "0x0000000000000038")
        self.assertEqual(session["crashFrame"], "UUserWidget::RebuildWidget")

    def test_newest_project_log_wins_and_clean_exit_is_exited(self):
        self._write(self.port_file, "27145")
        backup = os.path.join(self.root, "Saved", "Logs", "Demo-backup-2026.09.01-00.00.00.log")
        self._log(backup, LINUX_CRASH)
        os.utime(backup, (1, 1))
        self._log(self.log, "LogCore: Engine exit requested (reason: RequestExit())\n"
                              "Log file closed, 09/30/26 18:50:00\n")
        session = self._call()[1]
        self.assertEqual(session["logPath"], self.log)
        self.assertEqual(session["state"], "exited")

    def test_log_older_than_the_breadcrumb_and_crash_reporter_log_are_not_the_editors(self):
        self._log(self.log, LINUX_CRASH)
        os.utime(self.log, (1, 1))
        self._write(self.port_file, "27145")
        self._log(os.path.join(self.root, "Saved", "Logs", "Demo-CRC.log"), LINUX_CRASH)
        text, session = self._call()
        self.assertIsNone(session["logPath"])
        self.assertEqual(session["state"], "stopped")
        self.assertIn("No Saved/Logs log of this project was written since then", text)
        self.assertNotIn("Start it with the editor_start", text)

    def test_log_opened_after_the_bind_is_another_process(self):
        self._write(self.port_file, "27145")
        self._log(self.log, LINUX_CRASH, opened_offset=+120)
        session = self._call()[1]
        self.assertIsNone(session["logPath"])
        self.assertEqual(session["state"], "stopped")

    def test_stale_result_file_does_not_outrank_a_later_clean_exit(self):
        self._write(self.port_file, "27145")
        result = self.log + ".result.txt"
        self._write(result, "PINWRIGHT_JOB_RESULT verdict=EDITOR_KILLED_EXTERNALLY exit=-9\n")
        os.utime(result, (1, 1))
        self._log(self.log, "LogCore: Engine exit requested (reason: x)\n")
        self.assertEqual(self._call()[1]["state"], "exited")

    def test_undecodable_port_file_is_not_a_breadcrumb_crash(self):
        with open(self.port_file, "wb") as fh:
            fh.write(b"\xff\xfe")
        self.assertEqual(self._call()[1]["state"], "never_started")

    def test_supervisor_external_kill_verdict_is_reported(self):
        self._write(self.port_file, "27145")
        self._log(self.log, "LogPinWright: serving\n")
        self._write(self.log + ".result.txt",
                    "PINWRIGHT_JOB_RESULT verdict=EDITOR_KILLED_EXTERNALLY exit=-9 priority=x\n")
        text, session = self._call()
        self.assertEqual(session["state"], "killed_externally")
        self.assertTrue(session["killedExternally"])
        self.assertTrue(text.startswith("EDITOR_NOT_RUNNING (killed externally)"), text)

    def test_no_port_file_configured_keeps_generic_text(self):
        self.assertEqual(
            mcp_proxy.editor_not_running_text(None, "x"),
            "EDITOR_NOT_RUNNING: The Unreal editor is not running or PinWright MCP is "
            "unavailable. Start it with the editor_start MCP tool, then retry call(). (x)")


if __name__ == "__main__":
    unittest.main()
