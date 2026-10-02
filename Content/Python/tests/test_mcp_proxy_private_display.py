# Copyright (c) 2026 Alexander Penkin. MIT License.

"""editor_start display: "xvfb" | "xephyr" - a private X server per visible Linux editor
(board F-os-input-private-display). Pure stdlib: the X server is a fake script speaking -displayfd,
and the editor spawn is mocked, so no X server or Unreal process is started.

    python -m unittest test_mcp_proxy_private_display
"""

import os
import stat
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import mcp_proxy  # noqa: E402
from mcp_proxy import Proxy  # noqa: E402

LINUX = sys.platform.startswith("linux")
REASON = "unit test: private display"

# The editor log lines FVulkanDevice writes for the device the RHI created (UE 5.8).
GPU_LOG = [
    "[2026.10.01-15.32.17:829][  0]LogVulkanRHI: Display: - DeviceName: NVIDIA GeForce RTX 3060 Ti",
    "[2026.10.01-15.32.17:829][  0]LogVulkanRHI: Display: - DeviceID=0x2486 Type=VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU",
]
CPU_LOG = [
    "[2026.10.01-15.32.17:829][  0]LogVulkanRHI: Display: - DeviceName: llvmpipe (LLVM 15.0.7, 256 bits)",
    "[2026.10.01-15.32.17:829][  0]LogVulkanRHI: Display: - DeviceID=0x0 Type=VK_PHYSICAL_DEVICE_TYPE_CPU",
]


class _Proc:
    pid = 4321

    def __init__(self, code=None):
        self.code = code
        self.terminated = False

    def poll(self):
        return self.code

    def wait(self, *_args, **_kwargs):
        return self.code

    def terminate(self):
        self.terminated = True
        self.code = -15

    def kill(self):
        self.code = -9


class VulkanDeviceTest(unittest.TestCase):
    def test_a_gpu_is_not_software(self):
        name, device_type = mcp_proxy.vulkan_device(GPU_LOG)
        self.assertEqual(name, "NVIDIA GeForce RTX 3060 Ti")
        self.assertEqual(device_type, "VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU")
        self.assertFalse(mcp_proxy.is_software_vulkan(name, device_type))

    def test_a_cpu_device_is_software_by_type_or_by_name(self):
        self.assertTrue(mcp_proxy.is_software_vulkan(*mcp_proxy.vulkan_device(CPU_LOG)))
        self.assertTrue(mcp_proxy.is_software_vulkan("llvmpipe (LLVM 15)", None))
        self.assertTrue(mcp_proxy.is_software_vulkan(None, "VK_PHYSICAL_DEVICE_TYPE_CPU"))

    def test_no_device_lines_read_as_unknown(self):
        self.assertEqual(mcp_proxy.vulkan_device(["LogInit: nothing"]), (None, None))
        self.assertFalse(mcp_proxy.is_software_vulkan(None, None))


@unittest.skipUnless(LINUX, "pass_fds / -displayfd are POSIX")
class StartPrivateDisplayTest(unittest.TestCase):
    def _fake_server(self, temp, write_number):
        """An executable standing in for Xvfb: records its argv, writes a display number to the
        -displayfd descriptor (or not), then waits to be stopped."""
        script = os.path.join(temp, "FakeX")
        with open(script, "w", encoding="utf-8") as fh:
            fh.write("#!%s\n" % sys.executable)
            fh.write("import os, sys, time\n")
            fh.write("open(%r, 'w').write(' '.join(sys.argv[1:]))\n" % os.path.join(temp, "argv"))
            if write_number:
                fh.write("os.write(int(sys.argv[sys.argv.index('-displayfd') + 1]), b'57\\n')\n")
            fh.write("time.sleep(60)\n")
        os.chmod(script, os.stat(script).st_mode | stat.S_IXUSR)
        return script

    def test_a_missing_binary_is_refused_naming_the_package(self):
        proc, display, error = mcp_proxy.start_private_display("xvfb", which=lambda _name: None)
        self.assertIsNone(proc)
        self.assertIsNone(display)
        self.assertTrue(error.startswith("PRIVATE_DISPLAY_UNAVAILABLE"))
        self.assertIn("xvfb package", error)
        error = mcp_proxy.start_private_display("xephyr", which=lambda _name: None)[2]
        self.assertIn("xserver-xephyr package", error)

    def test_the_server_picks_its_display_and_is_told_to_exit_with_its_last_client(self):
        with tempfile.TemporaryDirectory() as temp:
            script = self._fake_server(temp, write_number=True)
            proc, display, error = mcp_proxy.start_private_display(
                "xvfb", which=lambda _name: script, timeout=10)
            try:
                self.assertIsNone(error)
                self.assertEqual(display, ":57")
                self.assertIsNone(proc.poll())
                for _ in range(100):
                    if os.path.exists(os.path.join(temp, "argv")):
                        break
                    time.sleep(0.05)
                with open(os.path.join(temp, "argv"), encoding="utf-8") as fh:
                    argv = fh.read().split()
                self.assertIn("-displayfd", argv)
                self.assertIn("-terminate", argv)
            finally:
                mcp_proxy.stop_private_display(proc)
            self.assertIsNotNone(proc.poll())

    def test_a_server_that_never_reports_a_display_is_stopped(self):
        with tempfile.TemporaryDirectory() as temp:
            script = self._fake_server(temp, write_number=False)
            started = []
            real_popen = mcp_proxy.subprocess.Popen

            def popen(*args, **kwargs):
                started.append(real_popen(*args, **kwargs))
                return started[-1]

            with mock.patch("mcp_proxy.subprocess.Popen", side_effect=popen):
                proc, display, error = mcp_proxy.start_private_display(
                    "xvfb", which=lambda _name: script, timeout=0.5)
            self.assertIsNone(proc)
            self.assertTrue(error.startswith("PRIVATE_DISPLAY_FAILED"))
            self.assertIsNotNone(started[0].poll(), "the silent server must not be left running")


class PrivateDisplayEnvTest(unittest.TestCase):
    def test_the_editor_sees_only_the_private_x_display(self):
        env = mcp_proxy.private_display_env(
            {"DISPLAY": ":0", "WAYLAND_DISPLAY": "wayland-0", "XDG_SESSION_TYPE": "wayland",
             "HOME": "/home/u"}, ":57")
        self.assertEqual(env["DISPLAY"], ":57")
        self.assertNotIn("WAYLAND_DISPLAY", env)
        self.assertEqual(env["XDG_SESSION_TYPE"], "x11")
        self.assertEqual(env["SDL_VIDEODRIVER"], "x11")
        self.assertEqual(env["HOME"], "/home/u")

    def test_editor_list_reads_the_display_from_the_process_environment(self):
        with tempfile.TemporaryDirectory() as temp:
            with open(os.path.join(temp, "environ"), "wb") as fh:
                fh.write(b"HOME=/home/u\0DISPLAY=:57\0")
            self.assertEqual(mcp_proxy._process_env_value(temp, b"DISPLAY"), ":57")
            self.assertIsNone(mcp_proxy._process_env_value(temp, b"WAYLAND_DISPLAY"))
        self.assertIsNone(mcp_proxy._process_env_value("/nonexistent-pid", b"DISPLAY"))
        row = {"pid": 1, "argv": ["UnrealEditor"], "exe": "UnrealEditor", "display": ":57"}
        self.assertEqual(mcp_proxy.describe_editor_process(row)["display"], ":57")


@unittest.skipUnless(LINUX, "a private display is Linux-only")
class EditorStartPrivateDisplayTest(unittest.TestCase):
    ENGINE_ROOT = os.path.join(os.sep + "opt", "UE_5.8")
    EXE = os.path.join(ENGINE_ROOT, "Engine", "Binaries", "Linux", "UnrealEditor")

    def _start(self, temp, log_lines, args=None):
        project = os.path.join(temp, "Host.uproject")
        with open(project, "w", encoding="utf-8") as fh:
            fh.write('{"EngineAssociation": "5.8"}')
        proxy = Proxy(None, 0.1, 0.1, 0.1, None, None, start_timeout=0.0, uproject=project)
        probes = iter([("not_running", "refused"), ("alive", None)])
        self.xserver = _Proc()
        self.editor = _Proc()
        launch = {"mode": "visible", "reason": REASON, "display": "xvfb"}
        launch.update(args or {})
        with mock.patch.dict(os.environ, {"WAYLAND_DISPLAY": "wayland-0"}), \
                mock.patch("mcp_proxy._launch_lock_path",
                           return_value=os.path.join(temp, "editor-launch.lock")), \
                mock.patch("mcp_proxy._visible_via_supervisor", return_value=False), \
                mock.patch("mcp_proxy._visible_launch_env",
                           side_effect=AssertionError("xvfb needs no desktop display")), \
                mock.patch.object(proxy, "_probe_state", side_effect=lambda _url: next(probes)), \
                mock.patch.object(proxy, "_resolve_url", return_value="http://x/mcp"), \
                mock.patch("mcp_proxy.resolve_editor", return_value=(self.ENGINE_ROOT, self.EXE)), \
                mock.patch("mcp_proxy.start_private_display",
                           return_value=(self.xserver, ":57", None)) as started, \
                mock.patch("mcp_proxy.pinwright_supervisor.read_lines",
                           return_value=log_lines) as read_lines, \
                mock.patch("mcp_proxy.subprocess.Popen", return_value=self.editor) as popen:
            result = proxy._editor_start(launch)
        return result, started, popen, read_lines

    def test_the_editor_runs_on_the_private_display_and_reports_it(self):
        with tempfile.TemporaryDirectory() as temp:
            result, started, popen, read_lines = self._start(temp, GPU_LOG)
        self.assertFalse(result["isError"], result)
        self.assertEqual(started.call_args.args[0], "xvfb")
        env = popen.call_args.kwargs["env"]
        self.assertEqual(env["DISPLAY"], ":57")
        self.assertNotIn("WAYLAND_DISPLAY", env)
        abslog = mcp_proxy._abslog_path(popen.call_args.args[0])
        self.assertIsNotNone(abslog, "a private-display editor gets a log of its own")
        self.assertEqual(read_lines.call_args.args[0], abslog)
        structured = result["structuredContent"]
        self.assertEqual(structured["display"], ":57")
        self.assertEqual(structured["displayServer"], "xvfb")
        self.assertEqual(structured["rhiDevice"], "NVIDIA GeForce RTX 3060 Ti")
        self.assertFalse(self.editor.terminated)
        self.assertFalse(self.xserver.terminated)

    def test_a_software_rhi_stops_the_editor_and_its_server_and_refuses(self):
        with tempfile.TemporaryDirectory() as temp:
            result = self._start(temp, CPU_LOG)[0]
        self.assertTrue(result["isError"])
        structured = result["structuredContent"]
        self.assertEqual(structured["error"], "PRIVATE_DISPLAY_SOFTWARE_RHI")
        self.assertEqual(structured["display"], ":57")
        self.assertIn("llvmpipe", result["content"][0]["text"])
        self.assertTrue(self.editor.terminated)
        self.assertTrue(self.xserver.terminated)

    def test_a_private_display_needs_mode_visible(self):
        with tempfile.TemporaryDirectory() as temp:
            result, started, popen, _read = self._start(temp, GPU_LOG, {"mode": "offscreen"})
        self.assertEqual(result["structuredContent"]["error"], "INVALID_DISPLAY")
        started.assert_not_called()
        popen.assert_not_called()

    def test_an_unknown_display_is_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            result = self._start(temp, GPU_LOG, {"display": "vnc"})[0]
        self.assertEqual(result["structuredContent"]["error"], "INVALID_DISPLAY")


# The machine-wide memory / editor-count refusal reads this host's live memory and editors; launch
# tests must not depend on either (it has its own tests in test_launch_capacity.py).
_CAPACITY_PATCH = mock.patch("mcp_proxy.Proxy._launch_capacity_guard", return_value=None)


def setUpModule():
    _CAPACITY_PATCH.start()


def tearDownModule():
    _CAPACITY_PATCH.stop()


if __name__ == "__main__":
    unittest.main()
