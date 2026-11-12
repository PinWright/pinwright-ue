# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Unit tests for pinwright_cli: instance discovery, selection, token choice and exit codes.

Pure stdlib with temporary project folders and injected process-start lookups; no Unreal process
or socket is opened. Run from Content/Python with `python -m unittest tests.test_pinwright_cli`.
"""

import io
import json
import os
import shutil
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import pinwright_cli  # noqa: E402


class InstanceDiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root, True)
        self.project = os.path.join(self.root, "Demo.uproject")
        with open(self.project, "w") as fh:
            fh.write("{}")
        self.pw = os.path.join(self.root, "Saved", "PinWright")
        os.makedirs(os.path.join(self.pw, "instances"))
        self.now_ms = time.time() * 1000
        # Where -SaveToUserDir puts Saved/: <UserSettingsDir>/<ProjectName>/Saved.
        user_settings = os.path.join(self.root, "user-settings")
        self.user_pw = os.path.join(user_settings, "Demo", "Saved", "PinWright")
        patcher = mock.patch.object(pinwright_cli, "_user_settings_dir", return_value=user_settings)
        patcher.start()
        self.addCleanup(patcher.stop)

    def _write(self, rel, data):
        path = os.path.join(self.pw, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            fh.write(data if isinstance(data, str) else json.dumps(data))
        return path

    def _instance(self, pid, port, **extra):
        data = {"pid": pid, "port": port, "token": "t%d" % pid, "kind": "editor",
                "project": self.project, "startTime": "2026-01-01T00:00:00Z", "reason": ""}
        data.update(extra)
        return self._write(os.path.join("instances", "%d.json" % pid), data)

    def _start_ms(self, table):
        return lambda pid: table.get(pid)

    def test_stale_and_reused_pids_are_skipped_and_primary_gets_gateway_token(self):
        self._instance(100, 40000)
        self._instance(200, 40001)  # dead
        self._instance(300, 40002)  # pid reused by a process started after the file
        self._write("gateway-port", "40000")
        self._write(os.path.join("cli-names", "a.json"), {"name": "a", "pid": 100})
        live = pinwright_cli.load_instances(self.project, self._start_ms(
            {100: self.now_ms - 60000, 300: self.now_ms + 600000}))
        self.assertEqual([e["pid"] for e in live], [100])
        self.assertTrue(live[0]["primary"])
        self.assertEqual(live[0]["name"], "a")
        self.assertEqual(live[0]["tokenFile"], os.path.join(self.pw, "gateway-token"))

    def test_gateway_port_without_instance_file_is_the_primary(self):
        self._instance(100, 40001)
        self._write("gateway-port", "40000")
        live = pinwright_cli.load_instances(self.project, self._start_ms({100: 0}))
        primary = pinwright_cli.select_instance(live)
        self.assertEqual((primary["port"], primary["pid"]), (40000, None))
        self.assertNotIn("tokenFile", pinwright_cli.select_instance(live, port=40001))

    def test_non_primary_uses_its_own_token(self):
        self._instance(100, 40001)
        live = pinwright_cli.load_instances(self.project, self._start_ms({100: 0}))
        endpoint = pinwright_cli.Endpoint(self.project, live[0])
        self.assertEqual(endpoint._read_token(), "t100")
        self.assertEqual(endpoint._resolve_url(), "http://127.0.0.1:40001/mcp")

    def _user_instance(self, pid, port, project):
        path = os.path.join(self.user_pw, "instances", "%d.json" % pid)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            json.dump({"pid": pid, "port": port, "token": "t%d" % pid, "kind": "game",
                       "project": project, "startTime": "2026-01-01T00:00:00Z"}, fh)
        return path

    def test_save_to_user_dir_instance_is_found(self):
        # A -game launched with -SaveToUserDir writes under the user-dir Saved, not the project's.
        path = self._user_instance(100, 40001, self.project)
        live = pinwright_cli.load_instances(self.project, self._start_ms({100: 0}))
        self.assertEqual([(e["pid"], e["instanceFile"]) for e in live], [(100, path)])
        endpoint = pinwright_cli.Endpoint(self.project, live[0])
        self.assertEqual(endpoint._read_token(), "t100")

    def test_user_dir_instance_of_another_checkout_is_skipped(self):
        # Every checkout named Demo shares the user-dir Saved; only this project's files count.
        other = os.path.join(self.root, "other-checkout", "Demo.uproject")
        self._user_instance(100, 40001, other)
        self._user_instance(200, 40002, None)
        live = pinwright_cli.load_instances(self.project, self._start_ms({100: 0, 200: 0}))
        self.assertEqual(live, [])

    def test_selection_errors_are_usage_errors(self):
        live = [{"pid": 1, "port": 40001, "name": "x", "primary": False},
                {"pid": 2, "port": 40002, "name": "x", "primary": False}]
        with self.assertRaises(pinwright_cli.UsageError):
            pinwright_cli.select_instance(live, name="x")  # ambiguous
        with self.assertRaises(pinwright_cli.UsageError):
            pinwright_cli.select_instance(live)  # no primary
        self.assertEqual(pinwright_cli.select_instance(live, pid=2)["port"], 40002)

    def test_call_exit_code_follows_is_error(self):
        entry = {"pid": 1, "port": 40001, "token": "t", "primary": False}
        for is_error, code in ((False, 0), (True, 1)):
            reply = {"jsonrpc": "2.0", "id": 1, "result": {
                "isError": is_error, "content": [{"type": "text", "text": "x"}],
                "structuredContent": {"ok": not is_error}}}
            with mock.patch.object(pinwright_cli, "load_instances", return_value=[entry]), \
                    mock.patch.object(pinwright_cli.Endpoint, "handle", return_value=reply), \
                    mock.patch("sys.stdout", new_callable=io.StringIO) as out, \
                    mock.patch("sys.stderr", new_callable=io.StringIO):
                self.assertEqual(pinwright_cli.main(
                    ["--project", self.project, "call", "--pid", "1", "system.identity", "{}"]),
                    code)
            self.assertEqual(json.loads(out.getvalue()), {"ok": not is_error})

    def test_bad_args_json_is_exit_2(self):
        with mock.patch.object(pinwright_cli, "load_instances",
                               return_value=[{"pid": 1, "port": 40001, "primary": True}]), \
                mock.patch("sys.stderr", new_callable=io.StringIO):
            self.assertEqual(pinwright_cli.main(
                ["--project", self.project, "call", "system.identity", "[1]"]), 2)


if __name__ == "__main__":
    unittest.main()
