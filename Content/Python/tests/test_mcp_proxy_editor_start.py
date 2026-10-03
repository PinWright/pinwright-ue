# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Unit tests for mcp_proxy's editor lifecycle tools and pure automation evidence parsing.

Pure stdlib; resolvers and orchestration use injected fakes or temporary fixtures, so no Unreal
process or socket is opened. Run from this directory with:

    python -m unittest

or explicitly:

    python -m unittest test_mcp_proxy_editor_start
"""

import io
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

# mcp_proxy.py lives one directory up (Content/Python/).
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import mcp_proxy  # noqa: E402  (module handle for the few private helpers tested directly)
import pinwright_supervisor  # noqa: E402
from mcp_proxy import (
    EDITOR_LIST_TOOL,
    EDITOR_RESTART_TOOL,
    EDITOR_RUN_TESTS_TOOL,
    EDITOR_START_TOOL,
    EDITOR_TEST_STATUS_TOOL,
    MCP_INSTRUCTIONS_TEMPLATE,
    Proxy,
    _abslog_path,
    _association_engine_roots,
    _build_argument_parser,
    _editor_cmd_from_editor,
    _editor_exe_under,
    _launcher_engine_root,
    _registry_engine_root,
    build_editor_command,
    normalize_start_map,
    resolve_editor,
    resolve_editor_exe,
    resolve_uproject,
    serve_stdio,
)


_LOCK_DIR = tempfile.TemporaryDirectory()
_LOCK_PATCH = mock.patch("mcp_proxy._launch_lock_path",
                         return_value=os.path.join(_LOCK_DIR.name, "editor-launch.lock"))
# The machine-wide memory / editor-count refusal reads this host's live memory and editors; launch
# tests must not depend on either (it has its own tests in test_launch_capacity.py).
_CAPACITY_PATCH = mock.patch("mcp_proxy.Proxy._launch_capacity_guard", return_value=None)


def setUpModule():
    # A real PinWright launch on this machine holds the machine-wide editor-launch lock; these
    # tests must neither wait on it nor block it.
    _LOCK_PATCH.start()
    _CAPACITY_PATCH.start()


def tearDownModule():
    _CAPACITY_PATCH.stop()
    _LOCK_PATCH.stop()
    _LOCK_DIR.cleanup()


# Every launch verb requires an explicit mode and reason; tests that are not about those two
# parameters pass these.
REASON = "unit test: proxy lifecycle"
START = {"mode": "visible", "reason": REASON}


def _start_args(**overrides):
    args = dict(START)
    args.update(overrides)
    return args


def _same(a, b):
    """Path equality tolerant of slash/case differences on the running platform."""
    if a is None or b is None:
        return a is b
    return os.path.normcase(os.path.normpath(a)) == os.path.normcase(os.path.normpath(b))


class BuildEditorCommandTest(unittest.TestCase):
    def test_visible_gets_only_the_always_flags_and_skipcompile(self):
        cmd = build_editor_command("Editor.exe", "P.uproject", mode="visible", extra_args=[])
        self.assertEqual(cmd, ["Editor.exe", "P.uproject", "-AutoDeclinePackageRecovery",
                               "-SKIPCOMPILE"])

    # The engine's startup compile (LaunchEngineLoop.cpp:6581-6640) runs UBT on every visible
    # start when the project sets bForceCompilationAtStartup; -SKIPCOMPILE is its only off
    # switch. Counterfactual: drop the append and the default case fails.
    def test_visible_skips_the_startup_compile_unless_allow_build(self):
        default = build_editor_command("Editor.exe", "P.uproject", mode="visible", extra_args=[])
        allowed = build_editor_command("Editor.exe", "P.uproject", mode="visible", extra_args=[],
                                       allow_build=True)
        self.assertIn("-SKIPCOMPILE", default)
        self.assertNotIn("-SKIPCOMPILE", allowed)

    def test_windowless_never_carries_skipcompile(self):
        # The engine skips the whole startup module check under -unattended, so the switch
        # would be noise on an argv every windowless test pins exactly.
        for mode in ("offscreen", "headless"):
            with self.subTest(mode=mode):
                cmd = build_editor_command("Editor.exe", "P.uproject", mode=mode, extra_args=[])
                self.assertNotIn("-SKIPCOMPILE", cmd)

    def test_offscreen_has_the_windowless_flags_and_a_real_rhi(self):
        cmd = build_editor_command("Editor.exe", "P.uproject", mode="offscreen", extra_args=[])
        for flag in ("-RenderOffScreen", "-unattended", "-RunningUnattendedScript", "-nopause",
                     "-nosplash", "-nocefaccelpaint"):
            self.assertIn(flag, cmd)
        self.assertNotIn("-NullRHI", cmd)
        self.assertNotIn("-nullrhi", cmd)

    def test_headless_is_nullrhi_on_top_of_the_offscreen_flags(self):
        # -NullRHI only replaces the renderer; -RenderOffScreen is what selects the null platform
        # application (no OS window) and SDL's dummy driver (no X display) on Linux.
        cmd = build_editor_command("Editor.exe", "P.uproject", mode="headless", extra_args=[])
        self.assertEqual(cmd, ["Editor.exe", "P.uproject", "-AutoDeclinePackageRecovery",
                               "-NullRHI"] + mcp_proxy._OFFSCREEN_FLAGS)

    def test_unknown_mode_is_refused_by_the_builder(self):
        for mode in (True, False, "windowless", None):
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                build_editor_command("Editor.exe", "P.uproject", mode=mode, extra_args=[])

    def test_extra_args_appended_verbatim(self):
        cmd = build_editor_command("Editor.exe", "P.uproject", mode="visible",
                                   extra_args=["-windowed", "-resx=1280"])
        self.assertEqual(cmd[-2:], ["-windowed", "-resx=1280"])

    def test_none_extra_args_tolerated(self):
        cmd = build_editor_command("Editor.exe", "P.uproject", mode="visible", extra_args=None)
        self.assertEqual(cmd, ["Editor.exe", "P.uproject", "-AutoDeclinePackageRecovery",
                               "-SKIPCOMPILE"])

    # The startup map is read by FParse::Token as the FIRST token of the remaining command line
    # and skipped outright if that token starts with '-' (UnrealEdMisc.cpp:396-399). Index 2 -
    # immediately after the .uproject, before every switch - is therefore the only position that
    # works. Counterfactual: append the map anywhere after _ALWAYS_FLAGS and this fails.
    def test_map_is_the_first_token_after_the_uproject(self):
        cmd = build_editor_command("Editor.exe", "P.uproject", mode="offscreen",
                                   extra_args=["-windowed"], unattended_script=True,
                                   map_name="/Game/Maps/MyLevel")
        self.assertEqual(cmd[:3], ["Editor.exe", "P.uproject", "/Game/Maps/MyLevel"])
        self.assertEqual(cmd[3], "-AutoDeclinePackageRecovery")
        self.assertEqual(cmd[-1], "-windowed")

    def test_no_map_leaves_the_command_line_byte_identical(self):
        self.assertEqual(
            build_editor_command("Editor.exe", "P.uproject", mode="visible", extra_args=[],
                                 map_name=None),
            build_editor_command("Editor.exe", "P.uproject", mode="visible", extra_args=[]))


class NormalizeStartMapTest(unittest.TestCase):
    """Validation for the startup-map token. The rejections are the load-bearing half: a token
    the engine ignores fails SILENTLY (it boots the default startup map and logs nothing), so
    each of these would otherwise surface as 'editor_start worked but opened the wrong map'."""

    def test_absent_map_is_not_an_error(self):
        self.assertEqual(normalize_start_map(None), (None, None))

    def test_package_path_passes_through(self):
        self.assertEqual(normalize_start_map("/Game/Maps/MyLevel"), ("/Game/Maps/MyLevel", None))

    def test_short_name_passes_through(self):
        self.assertEqual(normalize_start_map("MyLevel"), ("MyLevel", None))

    def test_umap_extension_is_stripped(self):
        # SearchForPackageOnDisk matches on the package name, not the file name.
        self.assertEqual(normalize_start_map("/Game/Maps/MyLevel.umap")[0], "/Game/Maps/MyLevel")

    def test_surrounding_quotes_and_whitespace_are_trimmed(self):
        self.assertEqual(normalize_start_map('  "/Game/Maps/MyLevel"  ')[0], "/Game/Maps/MyLevel")

    def test_leading_dash_is_rejected(self):
        token, error = normalize_start_map("-Game/Maps/MyLevel")
        self.assertIsNone(token)
        self.assertIn("switch", error)

    def test_embedded_whitespace_is_rejected(self):
        token, error = normalize_start_map("/Game/Maps/My Level")
        self.assertIsNone(token)
        self.assertIn("whitespace", error)

    def test_empty_and_non_string_are_rejected(self):
        for value in ("", "   ", 7, []):
            token, error = normalize_start_map(value)
            self.assertIsNone(token, value)
            self.assertIsNotNone(error, value)


class ResolveEditorExeTest(unittest.TestCase):
    # An engine root shaped for the running platform; the resolver builds its candidate under it
    # via the same _editor_exe_under helper, so these tests are platform-agnostic.
    ROOT = "C:\\UE_5.7" if os.name == "nt" else os.path.join(os.sep + "opt", "UE_5.7")

    def test_explicit_wins_when_it_exists(self):
        got = resolve_editor_exe("C:/custom/UnrealEditor.exe", None, None, None,
                                 exists_fn=lambda p: p == "C:/custom/UnrealEditor.exe")
        self.assertEqual(got, "C:/custom/UnrealEditor.exe")

    def test_ue_root_env_used_when_no_explicit(self):
        want = _editor_exe_under(self.ROOT)
        got = resolve_editor_exe(None, self.ROOT, None, None, exists_fn=lambda p: _same(p, want))
        self.assertTrue(_same(got, want))

    def test_sys_executable_walk_up_is_the_default(self):
        # The engine's bundled python lives inside the engine tree; the resolver walks up to root.
        want = _editor_exe_under(self.ROOT)
        python_exe = os.path.join(self.ROOT, "Engine", "Binaries", "ThirdParty",
                                  "Python3", "Win64", "python.exe")
        got = resolve_editor_exe(None, None, python_exe, None, exists_fn=lambda p: _same(p, want))
        self.assertTrue(_same(got, want))

    def test_engine_association_last_resort(self):
        # Only reachable on the Windows C:\UE_<assoc> convention the resolver hardcodes.
        if os.name != "nt":
            self.skipTest("EngineAssociation fallback is a Windows C:\\UE_<ver> convention")
        want = _editor_exe_under("C:\\UE_5.7")
        got = resolve_editor_exe(None, None, None, "5.7", exists_fn=lambda p: _same(p, want))
        self.assertTrue(_same(got, want))

    def test_bad_explicit_degrades_to_auto_detect(self):
        want = _editor_exe_under(self.ROOT)
        python_exe = os.path.join(self.ROOT, "Engine", "Binaries", "ThirdParty",
                                  "Python3", "Win64", "python.exe")
        got = resolve_editor_exe("C:/missing/UnrealEditor.exe", None, python_exe, None,
                                 exists_fn=lambda p: _same(p, want))
        self.assertTrue(_same(got, want))

    def test_none_when_nothing_resolves(self):
        got = resolve_editor_exe(None, None, os.path.join("C:", "py", "python.exe"), "5.7",
                                 exists_fn=lambda p: False)
        self.assertIsNone(got)

    def test_pair_resolver_returns_engine_root_and_executable(self):
        want = _editor_exe_under(self.ROOT)
        root, exe = resolve_editor(
            None, self.ROOT, None, None, exists_fn=lambda path: _same(path, want)
        )
        self.assertTrue(_same(root, self.ROOT))
        self.assertTrue(_same(exe, want))


class ResolveEngineAssociationTest(unittest.TestCase):
    """The project's EngineAssociation decides which engine runs; nothing may substitute for it."""

    HOST_ROOT = "C:\\UE_5.7" if os.name == "nt" else os.path.join(os.sep + "opt", "UE_5.7")
    PROJECT_ROOT = "C:\\UE_5.8" if os.name == "nt" else os.path.join(os.sep + "opt", "UE_5.8")

    def _hosting_python(self):
        """The bundled python of the engine that HOSTS the proxy - not the project's engine."""
        return os.path.join(self.HOST_ROOT, "Engine", "Binaries", "ThirdParty",
                            "Python3", "Win64", "python.exe")

    def test_association_wins_over_the_engine_hosting_the_proxy(self):
        # Regression: the client config runs this proxy on whichever engine generated it (5.7
        # here), so a 5.8 project must still resolve 5.8 even though 5.7 is also installed.
        want = _editor_exe_under(self.PROJECT_ROOT)
        host = _editor_exe_under(self.HOST_ROOT)
        with mock.patch("mcp_proxy._association_engine_roots",
                        return_value=[self.PROJECT_ROOT]) as roots:
            got = resolve_editor_exe(
                None, None, self._hosting_python(), "5.8",
                exists_fn=lambda path: _same(path, want) or _same(path, host),
            )
        self.assertTrue(_same(got, want))
        self.assertEqual(roots.call_args.args[0], "5.8")

    def test_unresolvable_association_never_degrades_to_another_engine(self):
        # Hard failure is the point: the silent fall-through is what let 5.7 rebuild a 5.8 project.
        with mock.patch("mcp_proxy._association_engine_roots", return_value=[]):
            root, exe = resolve_editor(
                None, self.HOST_ROOT, self._hosting_python(), "5.8",
                exists_fn=lambda _path: True,
            )
        self.assertIsNone(root)
        self.assertIsNone(exe)

    def test_registry_and_launcher_precede_the_directory_convention(self):
        with mock.patch("mcp_proxy._registry_engine_root", return_value="R:\\reg"), \
                mock.patch("mcp_proxy._launcher_engine_root", return_value="L:\\launcher"):
            roots = _association_engine_roots("5.8")
        self.assertEqual(roots[:2], ["R:\\reg", "L:\\launcher"])
        if os.name == "nt":
            self.assertEqual(roots[2], "C:\\UE_5.8")

    def test_path_shaped_association_resolves_against_the_project(self):
        project = os.path.join(os.getcwd(), "Sub", "Host.uproject")
        with mock.patch("mcp_proxy._registry_engine_root", return_value=None), \
                mock.patch("mcp_proxy._launcher_engine_root", return_value=None):
            roots = _association_engine_roots(os.path.join(os.pardir, "UE5"), project)
        self.assertTrue(_same(roots[-1], os.path.join(os.getcwd(), "UE5")))

    def test_launcher_manifest_lookup_reads_install_location(self):
        with tempfile.TemporaryDirectory() as temp:
            manifest = os.path.join(temp, "LauncherInstalled.dat")
            with open(manifest, "w", encoding="utf-8") as fh:
                json.dump({"InstallationList": [
                    {"AppName": "QuixelBridge_5.8", "InstallLocation": "C:\\Bridge"},
                    {"AppName": "UE_5.8", "InstallLocation": "C:\\Engines\\UE_5.8"},
                ]}, fh)
            with mock.patch("mcp_proxy._launcher_manifest_path", return_value=manifest):
                self.assertEqual(_launcher_engine_root("5.8"), "C:\\Engines\\UE_5.8")
                self.assertIsNone(_launcher_engine_root("5.6"))

    def test_missing_launcher_manifest_is_not_an_error(self):
        with mock.patch("mcp_proxy._launcher_manifest_path",
                        return_value=os.path.join("Z:", "no-such-manifest.dat")):
            self.assertIsNone(_launcher_engine_root("5.8"))

    def test_unknown_association_is_not_registered(self):
        # Also covers non-Windows, where the registry lookup is a no-op.
        self.assertIsNone(_registry_engine_root("no-such-engine-{00000000}"))

    def test_install_ini_is_consulted_after_the_launcher_manifest(self):
        with mock.patch("mcp_proxy._registry_engine_root", return_value=None), \
                mock.patch("mcp_proxy._launcher_engine_root", return_value="L"), \
                mock.patch("mcp_proxy._install_ini_engine_root", return_value="I"):
            roots = _association_engine_roots("5.8")
        self.assertEqual(roots[:2], ["L", "I"])


class InstallIniEngineRootTest(unittest.TestCase):
    """Linux engine registration, read the way FDesktopPlatformLinux reads it."""

    GUID = "A1B2C3D4-0000-1111-2222-333344445555"

    def _resolve(self, ini_text, assoc, make_root=True):
        with tempfile.TemporaryDirectory() as temp:
            root = os.path.join(temp, "UE_5.8")
            if make_root:
                os.makedirs(root)
            ini = os.path.join(temp, "Install.ini")
            with open(ini, "w", encoding="utf-8") as fh:
                fh.write(ini_text.replace("<root>", root))
            with mock.patch("mcp_proxy._install_ini_path", return_value=ini):
                return mcp_proxy._install_ini_engine_root(assoc), root

    def test_released_build_key_serves_the_version_association(self):
        got, root = self._resolve("[Installations]\nUE_5.8=<root>\n", "5.8")
        self.assertEqual(got, root)
        self.assertIsNone(self._resolve("[Installations]\nUE_5.8=<root>\n", "5.7")[0])

    def test_guid_key_serves_a_guid_association_in_any_spelling(self):
        text = "[Installations]\n%s=<root>\n" % self.GUID
        for assoc in (self.GUID, "{%s}" % self.GUID.lower()):
            with self.subTest(assoc=assoc):
                got, root = self._resolve(text, assoc)
                self.assertEqual(got, root)

    def test_bare_version_key_is_not_a_registration(self):
        # The engine reads a non-UE_, non-GUID key as the zero GUID, so "5.8=" never serves
        # association "5.8"; matching it here would diverge from what the engine itself resolves.
        self.assertIsNone(self._resolve("[Installations]\n5.8=<root>\n", "5.8")[0])

    def test_missing_directory_and_other_sections_are_skipped(self):
        self.assertIsNone(self._resolve("[Installations]\nUE_5.8=<root>\n", "5.8",
                                        make_root=False)[0])
        self.assertIsNone(self._resolve("[Other]\nUE_5.8=<root>\n", "5.8")[0])

    def test_missing_install_ini_is_not_an_error(self):
        with mock.patch("mcp_proxy._install_ini_path",
                        return_value=os.path.join("Z:", "no-such", "Install.ini")):
            self.assertIsNone(mcp_proxy._install_ini_engine_root("5.8"))

    def test_install_ini_lives_under_the_linux_application_settings_dir(self):
        with mock.patch("mcp_proxy.sys.platform", "linux"), \
                mock.patch("mcp_proxy.os.path.expanduser", return_value="/home/u"):
            self.assertEqual(mcp_proxy._install_ini_path(),
                             os.path.join("/home/u", ".config", "Epic", "UnrealEngine",
                                          "Install.ini"))
        with mock.patch("mcp_proxy.sys.platform", "win32"):
            self.assertIsNone(mcp_proxy._install_ini_path())


class EngineRootOverrideTest(unittest.TestCase):
    """$PINWRIGHT_ENGINE_ROOT names an engine no association source can see."""

    ASSOC_ROOT = "C:\\UE_5.8" if os.name == "nt" else os.path.join(os.sep + "opt", "UE_5.8")
    OVERRIDE = "D:\\src\\UE_5.8" if os.name == "nt" else os.path.join(os.sep + "srv", "UE_5.8")

    def test_override_precedes_the_association(self):
        want = _editor_exe_under(self.OVERRIDE)
        with mock.patch("mcp_proxy._association_engine_roots", return_value=[self.ASSOC_ROOT]):
            root, exe = resolve_editor(None, None, None, "5.8", exists_fn=lambda _p: True,
                                       engine_root_override=self.OVERRIDE)
        self.assertTrue(_same(root, self.OVERRIDE))
        self.assertTrue(_same(exe, want))

    def test_override_without_an_editor_keeps_the_association_hard_stop(self):
        with mock.patch("mcp_proxy._association_engine_roots", return_value=[]):
            root, exe = resolve_editor(None, None, None, "5.8", exists_fn=lambda _p: False,
                                       engine_root_override=self.OVERRIDE)
        self.assertIsNone(root)
        self.assertIsNone(exe)

    def test_both_lifecycle_tools_read_the_override_from_the_environment(self):
        with tempfile.TemporaryDirectory() as temp, \
                mock.patch.dict(os.environ, {mcp_proxy.ENGINE_ROOT_ENV: self.OVERRIDE}):
            project = os.path.join(temp, "Host.uproject")
            with open(project, "w", encoding="utf-8") as fh:
                fh.write('{"EngineAssociation": "5.8"}')
            proxy = Proxy(None, 0.1, 0.1, 0.1, None, None, uproject=project)
            with mock.patch.object(proxy, "_probe_state",
                                   return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=(None, None)) as resolve:
                start = proxy._editor_start(_start_args())
                prepare = proxy._editor_run_tests(
                    {"filter": "X", "mode": "offscreen", "reason": REASON})
        for call in resolve.call_args_list:
            self.assertEqual(call.kwargs["engine_root_override"], self.OVERRIDE)
        self.assertEqual(resolve.call_count, 2)
        for result in (start, prepare):
            self.assertEqual(result["structuredContent"]["error"], "EDITOR_ENGINE_NOT_FOUND")
            self.assertIn(mcp_proxy.ENGINE_ROOT_ENV, result["content"][0]["text"])
            self.assertIn(self.OVERRIDE, result["content"][0]["text"])


class ResolveUprojectTest(unittest.TestCase):
    def _make_project(self, root, name):
        os.makedirs(root, exist_ok=True)
        path = os.path.join(root, name + ".uproject")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("{}")
        return path

    def test_explicit_wins(self):
        with tempfile.TemporaryDirectory() as temp:
            explicit = self._make_project(os.path.join(temp, "Explicit"), "Explicit")
            port_project = os.path.join(temp, "PortProject")
            self._make_project(port_project, "Port")
            port_file = os.path.join(port_project, "Saved", "PinWright", "gateway-port")
            got = resolve_uproject(explicit, port_file, __file__, os.path.exists)
            self.assertTrue(_same(got, explicit))

    def test_project_shaped_port_file_resolves_project(self):
        with tempfile.TemporaryDirectory() as temp:
            project = os.path.join(temp, "Project")
            want = self._make_project(project, "Host")
            port_file = os.path.join(project, "Saved", "PinWright", "gateway-port")
            got = resolve_uproject(None, port_file, os.path.join(temp, "elsewhere.py"),
                                   os.path.exists)
            self.assertTrue(_same(got, want))

    def test_non_project_shaped_port_file_is_ignored(self):
        with tempfile.TemporaryDirectory() as temp:
            self._make_project(temp, "ShouldNotResolve")
            got = resolve_uproject(
                None,
                os.path.join(temp, "gateway-port"),
                os.path.join(temp, "missing", "mcp_proxy.py"),
                os.path.exists,
            )
            self.assertIsNone(got)


class _CompletedProcess:
    pid = 4321

    def __init__(self, code=0):
        self.code = code
        self.wait_calls = []
        self.terminated = False
        self.killed = False

    def poll(self):
        return self.code

    def wait(self, *args, **kwargs):
        self.wait_calls.append((args, kwargs))
        return self.code

    def terminate(self):
        self.terminated = True

    def kill(self):
        self.killed = True


class _BlockedProcess(_CompletedProcess):
    """A spawned child that never exits and never becomes ready: _wait_for_ready falls straight
    through to its (zero) deadline. Lets a test assert on the argv without simulating readiness."""

    def poll(self):
        return None


class _InterruptedProcess(_CompletedProcess):
    def __init__(self):
        super().__init__(None)
        self._interrupted = False

    def wait(self, *args, **kwargs):
        self.wait_calls.append((args, kwargs))
        if not self._interrupted:
            self._interrupted = True
            raise KeyboardInterrupt()
        self.code = -15
        return self.code


class _BlockingProcess(_CompletedProcess):
    def __init__(self):
        super().__init__(None)
        self.started = threading.Event()
        self.finished = threading.Event()
        self.signals = []

    def poll(self):
        return self.code

    def wait(self, *args, **kwargs):
        self.wait_calls.append((args, kwargs))
        self.started.set()
        timeout = kwargs.get("timeout")
        if not self.finished.wait(timeout):
            raise subprocess.TimeoutExpired("fake-child", timeout)
        return self.code

    def send_signal(self, signum):
        self.signals.append(signum)
        self.terminate()

    def terminate(self):
        self.terminated = True
        self.code = -15
        self.finished.set()

    def kill(self):
        self.killed = True
        self.code = -9
        self.finished.set()


class _ControlledInput:
    _EOF = object()

    def __init__(self):
        self.items = queue.Queue()

    def __iter__(self):
        return self

    def __next__(self):
        item = self.items.get()
        if item is self._EOF:
            raise StopIteration
        return item

    def send(self, obj):
        self.items.put(json.dumps(obj) + "\n")

    def close(self):
        self.items.put(self._EOF)


class _CapturedOutput(io.StringIO):
    def __init__(self):
        super().__init__()
        self.changed = threading.Condition()

    def write(self, text):
        with self.changed:
            result = super().write(text)
            self.changed.notify_all()
            return result

    def wait_for_lines(self, count, timeout=2.0):
        with self.changed:
            return self.changed.wait_for(
                lambda: self.getvalue().count("\n") >= count, timeout
            )


def _pin_desktop_launch_platform(test):
    """Pin a host with a display and hide any operator engine override, so these tests read the
    same on every host. Linux's display borrowing is covered by LinuxLaunchTest."""
    patcher = mock.patch("mcp_proxy._visible_launch_env", return_value={})
    patcher.start()
    test.addCleanup(patcher.stop)
    # The direct visible spawn is the platform-neutral baseline these tests pin; the Windows
    # supervised visible launch has its own test.
    direct = mock.patch("mcp_proxy._visible_via_supervisor", return_value=False)
    direct.start()
    test.addCleanup(direct.stop)
    environ = mock.patch.dict(os.environ)
    environ.start()
    test.addCleanup(environ.stop)
    os.environ.pop(mcp_proxy.ENGINE_ROOT_ENV, None)


class ProxyEditorStartTest(unittest.TestCase):
    URL = "http://127.0.0.1:19880/mcp"

    def setUp(self):
        _pin_desktop_launch_platform(self)

    def _proxy(self, uproject=None, url=URL, port_file=None, start_timeout=0.01):
        return Proxy(
            url,
            list_timeout=0.1,
            call_timeout=0.1,
            probe_timeout=0.1,
            token_file=None,
            port_file=port_file,
            editor_exe=None,
            start_timeout=start_timeout,
            uproject=uproject,
        )

    def _project(self, temp):
        path = os.path.join(temp, "Host.uproject")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write('{"EngineAssociation": "5.8"}')
        return path

    def test_unresolvable_association_fails_loudly_and_launches_nothing(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project)
            with mock.patch.object(
                    proxy, "_probe_state", return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy._association_engine_roots", return_value=[]), \
                    mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                               side_effect=AssertionError("must not spawn any editor")), \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               side_effect=AssertionError("must not spawn any editor")):
                result = proxy._editor_start(_start_args())
            self.assertTrue(result["isError"])
            self.assertEqual(
                result["structuredContent"]["error"], "EDITOR_ENGINE_NOT_FOUND"
            )
            self.assertEqual(result["structuredContent"]["engineAssociation"], "5.8")
            self.assertIn("5.8", result["content"][0]["text"])

    def test_extra_args_spawn_uses_the_association_resolved_engine(self):
        # The reported bug: extra_args take the direct-spawn path, which used to launch the
        # engine hosting the proxy instead of the one this project is associated with.
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project)
            engine_root = ("C:\\UE_5.8" if os.name == "nt"
                           else os.path.join(os.sep + "opt", "UE_5.8"))
            process = _CompletedProcess(code=None)
            probes = iter([("not_running", "refused"), ("alive", None)])
            with mock.patch.object(proxy, "_probe_state",
                                   side_effect=lambda _url: next(probes)), \
                    mock.patch("mcp_proxy.resolve_engine_root_for_association",
                               return_value=engine_root) as resolved, \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               return_value=process) as popen:
                result = proxy._editor_start(_start_args(
                    extra_args=["-relay=tcp://127.0.0.1:7788"]))
            self.assertEqual(resolved.call_args.args[0], "5.8")
            self.assertTrue(_same(popen.call_args.args[0][0], _editor_exe_under(engine_root)))
            self.assertFalse(result["isError"])

    def test_every_wait_mode_rejects_live_editor_before_resolution(self):
        for wait in ("ready", "exit", "invalid"):
            with self.subTest(wait=wait):
                proxy = self._proxy(uproject="missing.uproject")
                with mock.patch.object(
                        proxy, "_probe_state", return_value=("alive", None)), \
                        mock.patch("mcp_proxy.resolve_uproject",
                                   side_effect=AssertionError("project resolved")):
                    result = proxy._editor_start(_start_args(wait=wait))
                self.assertTrue(result["isError"])
                self.assertEqual(
                    result["structuredContent"]["error"], "EDITOR_ALREADY_RUNNING"
                )
                self.assertEqual(result["structuredContent"]["url"], self.URL)

    def test_unresponsive_guard_does_not_spawn_second_editor(self):
        proxy = self._proxy(uproject="missing.uproject")
        with mock.patch.object(
                proxy, "_probe_state",
                return_value=("unresponsive", "ping timed out")), \
                mock.patch("mcp_proxy.resolve_uproject",
                           side_effect=AssertionError("project resolved")):
            result = proxy._editor_start(_start_args(wait="exit"))
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_UNRESPONSIVE")

    def test_stale_plugin_guard_does_not_spawn_second_editor(self):
        # A stale-binary editor is still a RUNNING editor: the guard must block the
        # spawn (never resolve the project) and name the rebuild as the fix. Without the
        # guard edit this state falls through and a second editor is launched.
        for wait in ("ready", "exit"):
            with self.subTest(wait=wait):
                proxy = self._proxy(uproject="missing.uproject")
                with mock.patch.object(
                        proxy, "_probe_state",
                        return_value=("protocol_stale", "no editorReady field")), \
                        mock.patch("mcp_proxy.resolve_uproject",
                                   side_effect=AssertionError("project resolved")), \
                        mock.patch("mcp_proxy.subprocess.Popen",
                                   side_effect=AssertionError("second editor spawned")):
                    result = proxy._editor_start(_start_args(wait=wait))
                self.assertTrue(result["isError"])
                self.assertEqual(
                    result["structuredContent"]["error"], "EDITOR_PLUGIN_OUTDATED")
                self.assertFalse(result["structuredContent"]["retryable"])
                self.assertEqual(result["structuredContent"]["url"], self.URL)

    def test_windowless_exit_runs_under_the_capped_supervisor(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project)
            process = _CompletedProcess()
            with mock.patch.object(
                    proxy, "_probe_state", return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=("C:\\UE_5.8", "Editor.exe")), \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               side_effect=AssertionError("windowless must be supervised")), \
                    mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                               return_value=process) as supervised:
                result = proxy._editor_start({
                    "mode": "offscreen",
                    "reason": REASON,
                    "wait": "exit",
                    "extra_args": ["-Abslog=C:/logs/test.log"],
                })
            command = supervised.call_args.args[0]
            self.assertIn("-RenderOffScreen", command)
            self.assertIn("-Abslog=C:/logs/test.log", command)
            self.assertEqual(supervised.call_args.kwargs["kind"], "editor")
            self.assertEqual(supervised.call_args.kwargs["mode"], "offscreen")
            self.assertEqual(supervised.call_args.kwargs["reason"], REASON)
            self.assertEqual(supervised.call_args.kwargs["launched_by"], "editor_start")
            self.assertEqual(supervised.call_args.kwargs["log_path"], "C:/logs/test.log")
            self.assertIsNone(supervised.call_args.kwargs["timeout_minutes"])
            self.assertFalse(result["isError"])
            self.assertEqual(result["structuredContent"]["mode"], "offscreen")

    def test_visible_ready_spawns_the_editor_directly(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project)
            process = _CompletedProcess(code=None)
            probes = iter([
                ("not_running", "refused"),
                ("alive", None),
            ])
            with mock.patch.object(proxy, "_probe_state",
                                   side_effect=lambda _url: next(probes)), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=("C:\\UE_5.8", "Editor.exe")), \
                    mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                               side_effect=AssertionError("visible is not supervised")), \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               return_value=process) as popen:
                result = proxy._editor_start(_start_args(extra_args=["-windowed"]))
            self.assertEqual(popen.call_args.args[0][-1], "-windowed")
            self.assertFalse(result["isError"])
            self.assertEqual(result["structuredContent"]["pid"], process.pid)
            self.assertEqual(result["structuredContent"]["reason"], REASON)
            self.assertEqual(result["structuredContent"]["launchedBy"], "editor_start")
            self.assertEqual(result["structuredContent"]["mode"], "visible")
            self.assertFalse(result["structuredContent"]["capped"])

    def test_a_proxy_supervisor_skew_on_start_is_typed(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project)
            with mock.patch.object(
                    proxy, "_probe_state", return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=("C:\\UE_5.8", "Editor.exe")), \
                    mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                               side_effect=pinwright_supervisor.SupervisorVersionMismatch(
                                   pinwright_supervisor._mismatch_text(1))):
                result = proxy._editor_start(_start_args(mode="offscreen"))
        self.assertEqual(result["structuredContent"]["error"], "SUPERVISOR_VERSION_MISMATCH")
        self.assertIn("Restart the MCP server", result["content"][0]["text"])

    def test_windows_visible_goes_through_the_uncapped_supervisor_and_reports_detach(self):
        # Windows: the visible editor is started by the supervisor, itself created through WMI,
        # so it survives the MCP client's tree kill. Uncapped, normal priority, same argv.
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project)
            process = _CompletedProcess(code=None)
            process.detached, process.launch_mechanism = True, "wmi-win32-process-create"
            process.detach_note = None
            probes = iter([("not_running", "refused"), ("alive", None)])
            with mock.patch("mcp_proxy._visible_via_supervisor", return_value=True), \
                    mock.patch.object(proxy, "_probe_state",
                                      side_effect=lambda _url: next(probes)), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=("C:\\UE_5.8", "Editor.exe")), \
                    mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                               return_value=process) as supervised, \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               side_effect=AssertionError("not a direct spawn")):
                result = proxy._editor_start(_start_args(extra_args=["-windowed"]))
        kwargs = supervised.call_args.kwargs
        self.assertEqual(supervised.call_args.args[0][-1], "-windowed")
        self.assertEqual((kwargs["kind"], kwargs["mode"], kwargs["capped"], kwargs["priority"]),
                         ("editor", "visible", False, "Normal"))
        self.assertIsNone(kwargs["timeout_minutes"])
        self.assertFalse(result["isError"])
        structured = result["structuredContent"]
        self.assertEqual((structured["mode"], structured["capped"], structured["detached"],
                          structured["launchMechanism"]),
                         ("visible", False, True, "wmi-win32-process-create"))

    def test_direct_ready_early_exit_reports_abslog_path(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project)
            process = _CompletedProcess(code=7)
            with mock.patch.object(
                    proxy, "_probe_state", return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=("C:\\UE_5.8", "Editor.exe")), \
                    mock.patch("mcp_proxy.subprocess.Popen", return_value=process):
                result = proxy._editor_start(_start_args(
                    extra_args=["-Abslog=C:/logs/early-exit.log"]))
        self.assertEqual(
            result["structuredContent"]["error"], "EDITOR_EXITED_BEFORE_READY"
        )
        self.assertEqual(
            result["structuredContent"]["logPath"], "C:/logs/early-exit.log"
        )

    def test_direct_ready_timeout_reports_abslog_path(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project, start_timeout=0.0)
            process = _CompletedProcess(code=None)
            with mock.patch.object(
                    proxy, "_probe_state", return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=("C:\\UE_5.8", "Editor.exe")), \
                    mock.patch("mcp_proxy.subprocess.Popen", return_value=process):
                result = proxy._editor_start(_start_args(
                    extra_args=["-Abslog=C:/logs/ready-timeout.log"]))
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_START_TIMEOUT")
        self.assertEqual(
            result["structuredContent"]["logPath"], "C:/logs/ready-timeout.log"
        )

    def test_interruption_leaves_the_detached_editor_running(self):
        class _Interrupting(_CompletedProcess):
            def poll(self):
                raise KeyboardInterrupt()

        process = _Interrupting(code=None)
        result = self._proxy()._wait_for_exit(process, "Editor.exe P.uproject", [])
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_RUN_INTERRUPTED")
        self.assertTrue(result["structuredContent"]["leftRunning"])
        self.assertFalse(process.terminated)
        self.assertFalse(process.killed)

    # ---- start-into-map -----------------------------------------------------
    #
    # These drive the real _editor_start handler with only Popen mocked, so the argv
    # asserted below is the one the real build_editor_command produced.

    def _spawn_argv(self, args):
        """Run the real _editor_start against a fake dead endpoint and return the spawned argv,
        whether it went to a direct Popen (visible) or to the capped supervisor (windowless)."""
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project, start_timeout=0.0)
            with mock.patch.object(
                    proxy, "_probe_state", return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=("C:\\UE_5.8", "Editor.exe")), \
                    mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                               return_value=_BlockedProcess()) as supervised, \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               return_value=_BlockedProcess()) as popen:
                # wait='ready' against a zero start timeout returns at once; the argv is the
                # same for either wait mode.
                proxy._editor_start(_start_args(**dict(args, wait="ready")))
            spawner = popen if popen.called else supervised
            self.assertEqual(popen.called + supervised.called, 1)
            return spawner.call_args.args[0]

    def test_map_lands_immediately_after_the_uproject_on_the_real_launch(self):
        command = self._spawn_argv({"map": "/Game/Maps/Blockout"})
        self.assertEqual(command[2], "/Game/Maps/Blockout")
        self.assertEqual(command[3], "-AutoDeclinePackageRecovery")

    def test_map_defaults_modal_suppression_on(self):
        # A startup modal (e.g. 'Wait for ZenServer?') fires before PinWright loads, so no RPC
        # can clear it; -RunningUnattendedScript is the only lever for a visible launch.
        self.assertIn("-RunningUnattendedScript", self._spawn_argv({"map": "/Game/Maps/X"}))

    def test_explicit_unattended_script_false_wins_over_the_map_default(self):
        command = self._spawn_argv({"map": "/Game/Maps/X", "unattended_script": False})
        self.assertNotIn("-RunningUnattendedScript", command)

    def test_windowless_launch_gets_modal_suppression_without_asking(self):
        # Was pinned the other way as "backwards compatibility". The rationale for keeping
        # -RunningUnattendedScript opt-in is "it auto-answers dialogs with engine defaults, so a
        # human must not be sharing the window" - and mode offscreen HAS no window. What it does
        # have is -unattended, which sets FApp::IsUnattended() but leaves
        # GIsRunningUnattendedScript false - and GIsRunningUnattendedScript is the only global
        # FSlateApplication::AddModalWindow consults (SlateApplication.cpp:2134). So the old
        # behaviour shipped the one state the engine handles worst: a startup modal owns the
        # game thread forever, in a process with no window in which anyone could see or clear
        # it. a proxy-owned automation launch already hit exactly this wedge (25 minutes, zero tests, 0.8% CPU).
        command = self._spawn_argv({"mode": "offscreen", "wait": "exit"})
        self.assertIn("-unattended", command)
        self.assertIn("-RunningUnattendedScript", command)

    def test_visible_launch_without_map_still_has_neither_unattended_switch(self):
        # The half of the old rationale that is untouched: a visible editor can be handed to a
        # person, so nothing auto-answers its dialogs unless the caller asks (unattended_script)
        # or the call shape implies an agent-driven boot (map).
        command = self._spawn_argv({"mode": "visible", "wait": "exit"})
        self.assertNotIn("-RunningUnattendedScript", command)
        self.assertNotIn("-unattended", command)

    def test_windowless_opt_out_cannot_strip_the_switch_off_a_windowless_launch(self):
        # Deliberate compatibility break, and the reason the flag belongs in _OFFSCREEN_FLAGS
        # rather than in the unattended_script default: unattended_script:false must not be able
        # to assemble -unattended WITHOUT -RunningUnattendedScript. That combination wedges on a
        # startup modal AND makes FEditorFileUtils::PromptForCheckoutAndSave return PR_Cancelled
        # and save nothing (FileHelpers.cpp:4664). Opting out of modal suppression is only
        # meaningful where a modal can be answered, i.e. mode visible.
        command = self._spawn_argv({"mode": "offscreen", "wait": "exit",
                                    "unattended_script": False})
        self.assertIn("-unattended", command)
        self.assertIn("-RunningUnattendedScript", command)

    # ---- startup compile (B-editor-start-silent-module-rebuild) ---------------

    def test_visible_start_with_a_map_skips_the_startup_compile(self):
        # The ticket's call shape: a visible map launch silently ran UBT on the checkout.
        self.assertIn("-SKIPCOMPILE", self._spawn_argv({"map": "/Game/Maps/X"}))

    def test_allow_build_leaves_the_startup_compile_to_the_engine(self):
        self.assertNotIn("-SKIPCOMPILE",
                         self._spawn_argv({"map": "/Game/Maps/X", "allow_build": True}))

    def test_result_reports_which_startup_compile_policy_applied(self):
        for allow_build, expected in ((False, "skipped"), (True, "allowed")):
            with self.subTest(allow_build=allow_build), tempfile.TemporaryDirectory() as temp:
                proxy = self._proxy(uproject=self._project(temp))
                probes = iter([("not_running", "refused"), ("alive", None)])
                with mock.patch.object(proxy, "_probe_state",
                                       side_effect=lambda _url: next(probes)), \
                        mock.patch("mcp_proxy.resolve_editor",
                                   return_value=("C:\\UE_5.8", "Editor.exe")), \
                        mock.patch("mcp_proxy.subprocess.Popen",
                                   return_value=_CompletedProcess(code=None)):
                    result = proxy._editor_start(_start_args(allow_build=allow_build))
                self.assertFalse(result["isError"], result)
                self.assertEqual(result["structuredContent"]["startupCompile"], expected)

    def test_bad_allow_build_is_refused_before_the_slot_wait_and_guards(self):
        # slot_wait can block up to an hour; a malformed call must fail at once.
        proxy = self._proxy()
        with mock.patch.object(proxy, "_wait_for_free_slot",
                               side_effect=AssertionError("must not wait")), \
                mock.patch.object(proxy, "_editor_process_guard",
                                  side_effect=AssertionError("must not guard")), \
                mock.patch.object(proxy, "_launch_capacity_guard",
                                  side_effect=AssertionError("must not guard")):
            result = proxy._editor_start(_start_args(mode="offscreen", allow_build=True,
                                                     slot_wait=3600))
        self.assertEqual(result["structuredContent"],
                         {"error": "INVALID_ARGUMENTS", "param": "allow_build"})

    def test_startup_compile_reads_the_final_argv(self):
        # allow_build:true with a -SKIPCOMPILE in extra_args still skips; say so.
        with tempfile.TemporaryDirectory() as temp:
            proxy = self._proxy(uproject=self._project(temp))
            probes = iter([("not_running", "refused"), ("alive", None)])
            with mock.patch.object(proxy, "_probe_state",
                                   side_effect=lambda _url: next(probes)), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=("C:\\UE_5.8", "Editor.exe")), \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               return_value=_CompletedProcess(code=None)):
                result = proxy._editor_start(_start_args(allow_build=True,
                                                         extra_args=["-skipcompile"]))
        self.assertEqual(result["structuredContent"]["startupCompile"], "skipped")

    def test_bad_allow_build_is_refused_before_anything_is_spawned(self):
        for mode, value in (("offscreen", True), ("headless", True), ("visible", "yes")):
            with self.subTest(mode=mode, value=value), tempfile.TemporaryDirectory() as temp:
                proxy = self._proxy(uproject=self._project(temp))
                with mock.patch.object(
                        proxy, "_probe_state", return_value=("not_running", "refused")), \
                        mock.patch("mcp_proxy.subprocess.Popen",
                                   side_effect=AssertionError("must not spawn")), \
                        mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                                   side_effect=AssertionError("must not spawn")):
                    result = proxy._editor_start(_start_args(mode=mode, allow_build=value))
                self.assertTrue(result["isError"])
                self.assertEqual(result["structuredContent"],
                                 {"error": "INVALID_ARGUMENTS", "param": "allow_build"})


    def test_invalid_map_is_rejected_before_anything_is_spawned(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project)
            with mock.patch.object(
                    proxy, "_probe_state", return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               side_effect=AssertionError("must not spawn")), \
                    mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                               side_effect=AssertionError("must not spawn")):
                result = proxy._editor_start(_start_args(map="-NotAMap"))
        self.assertTrue(result["isError"])
        self.assertEqual(result["structuredContent"]["error"], "INVALID_MAP")

    def test_map_schema_is_advertised(self):
        self.assertIn("map", EDITOR_START_TOOL["inputSchema"]["properties"])

    # ---- mandatory launch mode and reason ------------------------------------

    def test_launch_schemas_require_mode_and_reason(self):
        for tool in (EDITOR_START_TOOL, EDITOR_RESTART_TOOL, EDITOR_RUN_TESTS_TOOL):
            with self.subTest(tool=tool["name"]):
                schema = tool["inputSchema"]
                self.assertTrue({"mode", "reason"} <= set(schema["required"]))
                self.assertNotIn("visible", schema["properties"])
                mode = schema["properties"]["mode"]
                self.assertEqual(mode["enum"], ["visible", "offscreen", "headless"])
                self.assertNotIn("default)", mode["description"].lower())
                self.assertIn("Required, no default", mode["description"])

    def _refused(self, launch, args):
        """Run a launch verb with every side effect armed to fail; return its result."""
        with mock.patch("mcp_proxy.subprocess.Popen",
                        side_effect=AssertionError("must not spawn")), \
                mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                           side_effect=AssertionError("must not spawn")), \
                mock.patch("mcp_proxy.resolve_uproject",
                           side_effect=AssertionError("must not resolve the project")):
            proxy = self._proxy(uproject="missing.uproject")
            with mock.patch.object(proxy, "_probe_state",
                                   side_effect=AssertionError("must not probe")):
                return getattr(proxy, launch)(args)

    def test_missing_or_invalid_mode_and_reason_are_refused_without_spawning(self):
        cases = (
            ({"reason": REASON}, "MISSING_REQUIRED_PARAM", "mode"),
            ({"visible": True, "reason": REASON}, "MISSING_REQUIRED_PARAM", "mode"),
            ({"mode": "windowless", "reason": REASON}, "INVALID_MODE", "mode"),
            ({"mode": "Visible", "reason": REASON}, "INVALID_MODE", "mode"),
            ({"mode": True, "reason": REASON}, "INVALID_MODE", "mode"),
            ({"mode": None, "reason": REASON}, "INVALID_MODE", "mode"),
            ({"mode": "headless"}, "MISSING_REQUIRED_PARAM", "reason"),
            ({"mode": "offscreen", "reason": ""}, "INVALID_REASON", "reason"),
            ({"mode": "offscreen", "reason": " \n\t "}, "INVALID_REASON", "reason"),
            ({"mode": "visible", "reason": 7}, "INVALID_REASON", "reason"),
            ({"mode": "visible", "reason": "x" * (pinwright_supervisor.REASON_MAX_CHARS + 1)},
             "INVALID_REASON", "reason"),
        )
        for launch in ("_editor_start", "_editor_restart"):
            for args, code, param in cases:
                with self.subTest(launch=launch, args=args):
                    result = self._refused(launch, args)
                    self.assertTrue(result["isError"])
                    self.assertEqual(result["structuredContent"]["error"], code)
                    self.assertEqual(result["structuredContent"]["param"], param)

    def test_mode_refusal_names_both_values_and_what_each_launches(self):
        for args in ({"reason": REASON}, {"mode": "no", "reason": REASON}):
            text = self._refused("_editor_start", args)["content"][0]["text"]
            for mode in ("'visible'", "'offscreen'", "'headless'"):
                self.assertIn(mode, text)
            self.assertIn("-NullRHI", text)
            self.assertIn("real RHI", text)

    def test_mode_visible_keeps_todays_command_line_plus_the_launch_switches(self):
        command = self._spawn_argv({"mode": "visible"})
        identity = pinwright_supervisor.launch_identity_args(REASON, "editor_start")
        self.assertEqual(command[2:], ["-AutoDeclinePackageRecovery", "-SKIPCOMPILE"] + identity)
        self.assertNotIn("-RenderOffScreen", command)

    def test_mode_offscreen_keeps_todays_command_line_plus_the_launch_switches(self):
        command = self._spawn_argv({"mode": "offscreen"})
        identity = pinwright_supervisor.launch_identity_args(REASON, "editor_start")
        self.assertEqual(command[2:], ["-AutoDeclinePackageRecovery"]
                         + mcp_proxy._OFFSCREEN_FLAGS + identity)

    def test_mode_headless_runs_nullrhi_under_the_capped_supervisor(self):
        with tempfile.TemporaryDirectory() as temp:
            proxy = self._proxy(uproject=self._project(temp), start_timeout=0.0)
            with mock.patch.object(
                    proxy, "_probe_state", return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=("C:\\UE_5.8", "Editor.exe")), \
                    mock.patch("mcp_proxy._visible_launch_env",
                               side_effect=AssertionError("headless needs no display")), \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               side_effect=AssertionError("headless must be supervised")), \
                    mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                               return_value=_BlockedProcess()) as supervised:
                result = proxy._editor_start(_start_args(mode="headless"))
        command = supervised.call_args.args[0]
        identity = pinwright_supervisor.launch_identity_args(REASON, "editor_start")
        self.assertEqual(command[2:], ["-AutoDeclinePackageRecovery", "-NullRHI"]
                         + mcp_proxy._OFFSCREEN_FLAGS + identity)
        self.assertEqual(supervised.call_args.kwargs["mode"], "headless")
        self.assertEqual(result["structuredContent"]["mode"], "headless")


class LinuxLaunchTest(unittest.TestCase):
    """A proxy started over SSH has no DISPLAY, so a visible Linux launch spawns the resolved
    editor with the desktop session's display; a windowless one needs none."""

    ENGINE_ROOT = os.path.join(os.sep + "opt", "UE_5.8")
    EXE = os.path.join(ENGINE_ROOT, "Engine", "Binaries", "Linux", "UnrealEditor")
    SESSION = {"DISPLAY": ":1", "XAUTHORITY": "/run/user/1000/gdm/Xauthority"}

    def _start(self, args, display_env, process=None):
        with tempfile.TemporaryDirectory() as temp:
            project = os.path.join(temp, "Host.uproject")
            with open(project, "w", encoding="utf-8") as fh:
                fh.write('{"EngineAssociation": "5.8"}')
            proxy = Proxy(None, 0.1, 0.1, 0.1, None, None, start_timeout=0.0,
                          uproject=project)
            probes = iter([("not_running", "refused"), ("alive", None)])
            with mock.patch("mcp_proxy._visible_via_supervisor", return_value=False), \
                    mock.patch("mcp_proxy._visible_launch_env", **display_env) as display, \
                    mock.patch.object(proxy, "_probe_state",
                                      side_effect=lambda _url: next(probes)), \
                    mock.patch.object(proxy, "_resolve_url", return_value="http://x/mcp"), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=(self.ENGINE_ROOT, self.EXE)), \
                    mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                               return_value=process or _CompletedProcess(code=None)) as supervised, \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               return_value=process or _CompletedProcess(code=None)) as popen:
                result = proxy._editor_start(_start_args(**args))
            self.supervised = supervised
            return result, popen, display, os.path.normpath(project)

    def test_default_start_spawns_the_resolved_editor_with_the_session_display(self):
        result, popen, _display, project = self._start({}, {"return_value": self.SESSION})
        self.assertFalse(result["isError"])
        self.assertEqual(popen.call_args.args[0],
                         [self.EXE, project, "-AutoDeclinePackageRecovery", "-SKIPCOMPILE"]
                         + pinwright_supervisor.launch_identity_args(REASON, "editor_start"))
        env = popen.call_args.kwargs["env"]
        self.assertEqual(env["DISPLAY"], ":1")
        self.assertEqual(env["XAUTHORITY"], self.SESSION["XAUTHORITY"])
        self.assertEqual(result["structuredContent"]["pid"], _CompletedProcess.pid)

    def test_visible_start_without_any_display_fails_before_spawning(self):
        result, popen, _display, _project = self._start({}, {"return_value": None})
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_NO_DISPLAY")
        popen.assert_not_called()

    def test_existing_display_leaves_the_environment_alone(self):
        _result, popen, _display, _project = self._start({}, {"return_value": {}})
        self.assertNotIn("env", popen.call_args.kwargs)

    def test_windowless_start_needs_no_display(self):
        # -RenderOffScreen hints SDL's dummy video driver (LinuxPlatformApplicationMisc.cpp).
        _result, popen, display, _project = self._start(
            {"mode": "offscreen", "wait": "exit"},
            {"side_effect": AssertionError("must not look for a display")},
            process=_CompletedProcess(code=0))
        display.assert_not_called()
        popen.assert_not_called()
        self.assertNotIn("env", self.supervised.call_args.kwargs)
        self.assertIn("-RenderOffScreen", self.supervised.call_args.args[0])


def _proc_state(pid):
    """The /proc State letter of pid ('Z' for a zombie), or None once it is gone (reaped)."""
    try:
        with open("/proc/%d/status" % pid, encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("State:"):
                    return line.split()[1]
    except FileNotFoundError:
        return None
    return None


@unittest.skipUnless(os.path.isdir("/proc/self"), "needs /proc to observe zombies")
class ReapSpawnedChildTest(unittest.TestCase):
    """A real child spawned by editor_start and left running when the verb returns must not stay
    <defunct> under the long-lived proxy once it exits (board B-editor-quit-leaves-zombie-editor-child),
    and must not be killed by the proxy's shutdown either."""

    def test_child_left_running_survives_shutdown_and_is_reaped_on_exit(self):
        temp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, temp, True)
        release = os.path.join(temp, "release")
        self.addCleanup(lambda: open(release, "w").close())  # never leak the child
        child = [sys.executable, "-c",
                 "import os, time\nwhile not os.path.exists(%r): time.sleep(0.05)" % release]
        project = os.path.join(temp, "Host.uproject")
        with open(project, "w", encoding="utf-8") as fh:
            fh.write('{"EngineAssociation": "5.8"}')
        proxy = Proxy(None, 0.1, 0.1, 0.1, None, None, start_timeout=5.0, uproject=project)
        probes = iter([("not_running", "refused"), ("alive", None)])
        with mock.patch("mcp_proxy._visible_launch_env", return_value={}), \
                mock.patch.object(proxy, "_probe_state", side_effect=lambda _url: next(probes)), \
                mock.patch.object(proxy, "_resolve_url", return_value="http://x/mcp"), \
                mock.patch("mcp_proxy.resolve_editor", return_value=("root", sys.executable)), \
                mock.patch("mcp_proxy.build_editor_command", return_value=child):
            result = proxy._editor_start(_start_args())
        self.assertFalse(result["isError"], result)
        pid = result["structuredContent"]["pid"]

        # Left alone: still running after the verb returned and after the proxy shut down.
        proxy.request_shutdown()
        self.assertIn(_proc_state(pid), ("S", "R", "D"))

        open(release, "w").close()
        deadline = time.monotonic() + 10.0
        while _proc_state(pid) is not None and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertIsNone(_proc_state(pid), "child exited but was not reaped (zombie)")


class BorrowSessionDisplayTest(unittest.TestCase):
    def _proc(self, temp, environs):
        for pid, entries in environs.items():
            os.makedirs(os.path.join(temp, str(pid)))
            with open(os.path.join(temp, str(pid), "environ"), "wb") as fh:
                fh.write(b"\0".join(entries) + b"\0")
        os.makedirs(os.path.join(temp, "self"))
        return os.stat(os.path.join(temp, str(min(environs)), "environ")).st_uid

    def test_local_display_with_xauthority_wins_and_forwarded_ones_are_skipped(self):
        with tempfile.TemporaryDirectory() as temp:
            uid = self._proc(temp, {
                100: [b"DISPLAY=localhost:10.0", b"XAUTHORITY=/home/u/.Xauthority"],
                200: [b"DISPLAY=:0", b"HOME=/home/u"],
                300: [b"DISPLAY=:1", b"XAUTHORITY=/run/user/1000/gdm/Xauthority"],
            })
            got = mcp_proxy._borrow_session_display(temp, uid)
        self.assertEqual(got, {"DISPLAY": ":1",
                               "XAUTHORITY": "/run/user/1000/gdm/Xauthority"})

    def test_display_without_xauthority_is_the_fallback(self):
        with tempfile.TemporaryDirectory() as temp:
            uid = self._proc(temp, {200: [b"DISPLAY=:0"], 201: [b"PATH=/usr/bin"]})
            self.assertEqual(mcp_proxy._borrow_session_display(temp, uid), {"DISPLAY": ":0"})

    def test_other_users_processes_are_ignored(self):
        with tempfile.TemporaryDirectory() as temp:
            uid = self._proc(temp, {300: [b"DISPLAY=:1"]})
            self.assertIsNone(mcp_proxy._borrow_session_display(temp, uid + 1))

    def test_only_a_linux_proxy_without_display_goes_looking(self):
        with mock.patch("mcp_proxy._borrow_session_display", return_value={"DISPLAY": ":9"}):
            with mock.patch("mcp_proxy.sys.platform", "win32"):
                self.assertEqual(mcp_proxy._visible_launch_env(), {})
            with mock.patch("mcp_proxy.sys.platform", "linux"), \
                    mock.patch.dict(os.environ, {"DISPLAY": ":5"}):
                self.assertEqual(mcp_proxy._visible_launch_env(), {})
            with mock.patch("mcp_proxy.sys.platform", "linux"), \
                    mock.patch.dict(os.environ):
                os.environ.pop("DISPLAY", None)
                self.assertEqual(mcp_proxy._visible_launch_env(), {"DISPLAY": ":9"})


class StampDetachTest(unittest.TestCase):
    """Every supervised launch result says whether the run outlives the MCP client."""

    @staticmethod
    def _result():
        return {"content": [{"type": "text", "text": "BUILD_STARTED: x."}],
                "structuredContent": {"pid": 1}, "isError": False}

    def test_detached_run_reports_its_mechanism(self):
        run = mock.Mock(detached=True, launch_mechanism="wmi-win32-process-create",
                        detach_note=None)
        result = mcp_proxy._stamp_detach(self._result(), run)
        self.assertEqual(result["structuredContent"],
                         {"pid": 1, "detached": True, "detachNote": None,
                          "launchMechanism": "wmi-win32-process-create"})
        self.assertEqual(result["content"][0]["text"], "BUILD_STARTED: x.")

    def test_a_run_that_is_not_detached_says_so_in_the_text(self):
        run = mock.Mock(detached=False, launch_mechanism="createprocess-breakaway",
                        detach_note="WMI launch failed (boom)")
        result = mcp_proxy._stamp_detach(self._result(), run)
        self.assertFalse(result["structuredContent"]["detached"])
        self.assertIn("NOT DETACHED: WMI launch failed (boom)", result["content"][0]["text"])

    def test_a_run_without_the_fields_is_left_alone(self):
        result = mcp_proxy._stamp_detach(self._result(), _CompletedProcess())
        self.assertEqual(result["structuredContent"], {"pid": 1})


class SpawnKwargsTest(unittest.TestCase):
    def test_posix_editor_never_inherits_the_mcp_stdio(self):
        # stdout is the MCP frame stream; an editor holding it writes console output into it.
        with mock.patch.object(mcp_proxy.os, "name", "posix"):
            for visible in (True, False):
                kwargs = mcp_proxy._spawn_kwargs(visible)
                self.assertTrue(kwargs["start_new_session"])
                for stream in ("stdin", "stdout", "stderr"):
                    self.assertIs(kwargs[stream], subprocess.DEVNULL)


class ProxyEditorRestartTest(unittest.TestCase):
    URL = "http://127.0.0.1:19880/mcp"

    def _proxy(self):
        return Proxy(
            self.URL,
            list_timeout=0.1,
            call_timeout=0.1,
            probe_timeout=0.1,
            token_file=None,
            port_file=None,
            editor_exe=None,
            start_timeout=0.01,
            uproject=None,
        )

    def test_restart_schema_is_advertised(self):
        self.assertEqual(EDITOR_RESTART_TOOL["name"], "editor_restart")
        for key in ("map", "mode", "reason", "save", "discard", "extra_args",
                    "unattended_script"):
            self.assertIn(key, EDITOR_RESTART_TOOL["inputSchema"]["properties"])

    def test_dispatch_is_proxy_local(self):
        proxy = self._proxy()
        expected = {"content": [], "structuredContent": {"success": True}, "isError": False}
        with mock.patch.object(proxy, "_editor_restart", return_value=expected) as restart:
            response = proxy.handle({
                "jsonrpc": "2.0", "id": 3, "method": "tools/call",
                "params": {"name": "editor_restart", "arguments": {"map": "/Game/Maps/X"}},
            })
        restart.assert_called_once_with({"map": "/Game/Maps/X"})
        self.assertEqual(response["result"], expected)

    def test_quits_then_starts_and_forwards_only_start_arguments(self):
        proxy = self._proxy()
        started = {"content": [], "structuredContent": {"success": True}, "isError": False}
        with mock.patch.object(proxy, "_probe_state", return_value=("alive", None)), \
                mock.patch.object(proxy, "_request_editor_quit", return_value=None) as quit_rpc, \
                mock.patch.object(proxy, "_wait_for_endpoint_down", return_value=True), \
                mock.patch.object(proxy, "_editor_start", return_value=started) as start:
            result = proxy._editor_restart(
                {"map": "/Game/Maps/X", "mode": "offscreen", "reason": REASON, "save": True})
        quit_rpc.assert_called_once_with(self.URL, True, False)
        # save/discard are quit-half arguments and must not leak into the launch; mode and
        # reason are forwarded verbatim, never defaulted.
        start.assert_called_once_with(
            {"map": "/Game/Maps/X", "mode": "offscreen", "reason": REASON},
            launched_by="editor_restart")
        self.assertTrue(result["structuredContent"]["restarted"])
        self.assertTrue(result["structuredContent"]["stoppedPreviousEditor"])

    def test_allow_build_is_forwarded_to_the_start_half(self):
        proxy = self._proxy()
        started = {"content": [], "structuredContent": {"success": True}, "isError": False}
        with mock.patch.object(proxy, "_probe_state", return_value=("not_running", "refused")), \
                mock.patch.object(proxy, "_editor_start", return_value=started) as start:
            proxy._editor_restart(_start_args(allow_build=True))
        self.assertIs(start.call_args.args[0]["allow_build"], True)

    def test_windowless_allow_build_is_refused_before_the_quit(self):
        proxy = self._proxy()
        with mock.patch.object(proxy, "_probe_state", return_value=("alive", None)), \
                mock.patch.object(proxy, "_request_editor_quit",
                                  side_effect=AssertionError("must not quit")):
            result = proxy._editor_restart(_start_args(mode="offscreen", allow_build=True))
        self.assertEqual(result["structuredContent"],
                         {"error": "INVALID_ARGUMENTS", "param": "allow_build"})

    def test_no_running_editor_skips_the_quit_half(self):
        proxy = self._proxy()
        started = {"content": [], "structuredContent": {"success": True}, "isError": False}
        with mock.patch.object(proxy, "_probe_state", return_value=("not_running", "refused")), \
                mock.patch.object(proxy, "_request_editor_quit",
                                  side_effect=AssertionError("must not quit a dead editor")), \
                mock.patch.object(proxy, "_editor_start", return_value=started):
            result = proxy._editor_restart(_start_args(map="/Game/Maps/X"))
        self.assertFalse(result["structuredContent"]["stoppedPreviousEditor"])

    def test_a_refused_quit_never_starts_a_second_editor(self):
        # Relaying UNSAVED_CHANGES rather than force-discarding is the point: a restart must
        # not be a silent way to lose work, and it must not leave two editors running.
        proxy = self._proxy()
        refusal = {"isError": True,
                   "content": [{"type": "text", "text": "UNSAVED_CHANGES: 3 dirty packages"}],
                   "structuredContent": {"error": "UNSAVED_CHANGES"}}
        with mock.patch.object(proxy, "_probe_state", return_value=("alive", None)), \
                mock.patch.object(proxy, "_post",
                                  return_value={"jsonrpc": "2.0", "id": "_proxy_restart_quit",
                                                "result": refusal}), \
                mock.patch.object(proxy, "_editor_start",
                                  side_effect=AssertionError("must not start a second editor")):
            result = proxy._editor_restart(_start_args())
        self.assertTrue(result["isError"])
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_QUIT_REFUSED")
        self.assertIn("UNSAVED_CHANGES", result["content"][0]["text"])

    def test_a_modal_blocked_editor_is_reported_not_bypassed(self):
        # editor.quit runs on the game thread the modal owns, so it cannot land. Spawning a
        # second editor beside the wedged one would be worse than failing.
        proxy = self._proxy()
        with mock.patch.object(proxy, "_probe_state",
                               return_value=("blocked_on_modal", "a dialog is open")), \
                mock.patch.object(proxy, "_editor_start",
                                  side_effect=AssertionError("must not start a second editor")):
            result = proxy._editor_restart(_start_args())
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_BLOCKED_ON_MODAL")

    def test_a_stuck_shutdown_times_out_without_killing_or_respawning(self):
        proxy = self._proxy()
        with mock.patch.object(proxy, "_probe_state", return_value=("alive", None)), \
                mock.patch.object(proxy, "_request_editor_quit", return_value=None), \
                mock.patch.object(proxy, "_wait_for_endpoint_down", return_value=False), \
                mock.patch.object(proxy, "_editor_start",
                                  side_effect=AssertionError("must not start a second editor")):
            result = proxy._editor_restart(_start_args())
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_STOP_TIMEOUT")

    def test_save_and_discard_together_are_rejected(self):
        proxy = self._proxy()
        with mock.patch.object(proxy, "_probe_state",
                               side_effect=AssertionError("must not probe")):
            result = proxy._editor_restart(_start_args(save=True, discard=True))
        self.assertEqual(result["structuredContent"]["error"], "INVALID_ARGUMENTS")

    def test_a_bad_map_is_rejected_before_the_editor_is_stopped(self):
        # Validating the map first means a typo costs nothing; validating it inside the start
        # half would leave the caller with no editor at all.
        proxy = self._proxy()
        with mock.patch.object(proxy, "_probe_state",
                               side_effect=AssertionError("must not probe")):
            result = proxy._editor_restart(_start_args(map="-NotAMap"))
        self.assertEqual(result["structuredContent"]["error"], "INVALID_MAP")

    def test_quit_transport_error_is_treated_as_a_possible_clean_exit(self):
        # The editor closes its socket mid-shutdown, which is the requested outcome. The
        # endpoint-down wait, not the POST, is the verdict.
        proxy = self._proxy()
        with mock.patch.object(proxy, "_post", side_effect=OSError("connection reset")):
            self.assertIsNone(proxy._request_editor_quit(self.URL, False, False))

    def test_quit_payload_carries_the_editor_quit_rpc(self):
        proxy = self._proxy()
        with mock.patch.object(proxy, "_post", return_value={"result": {"isError": False}}) as post:
            proxy._request_editor_quit(self.URL, False, True)
        payload = post.call_args.args[0]
        self.assertEqual(payload["params"]["name"], "call")
        self.assertEqual(payload["params"]["arguments"]["method"], "editor.quit")
        self.assertTrue(payload["params"]["arguments"]["args"]["discard"])
        self.assertNotIn("save", payload["params"]["arguments"]["args"])


class ProxyStdioLifecycleTest(unittest.TestCase):
    def setUp(self):
        _pin_desktop_launch_platform(self)

    def _proxy(self, uproject=None, start_timeout=60.0):
        return Proxy(
            None,
            list_timeout=0.1,
            call_timeout=0.1,
            probe_timeout=0.1,
            token_file=None,
            port_file=None,
            editor_exe=None,
            start_timeout=start_timeout,
            uproject=uproject,
        )

    @staticmethod
    def _project(temp):
        path = os.path.join(temp, "Host.uproject")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write('{"EngineAssociation": "5.8"}')
        return path

    def test_json_lines_responses_are_serialized_in_request_order(self):
        proxy = self._proxy()
        input_stream = _ControlledInput()
        output_stream = _CapturedOutput()
        server = threading.Thread(
            target=serve_stdio, args=(proxy, input_stream, output_stream)
        )
        server.start()
        input_stream.send({
            "jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}
        })
        self.assertTrue(output_stream.wait_for_lines(1))
        input_stream.send({"jsonrpc": "2.0", "id": 2, "method": "ping"})
        self.assertTrue(output_stream.wait_for_lines(2))
        self.assertTrue(server.is_alive())
        input_stream.close()
        server.join(2.0)

        self.assertFalse(server.is_alive())
        encoded = output_stream.getvalue()
        self.assertTrue(encoded.endswith("\n"))
        frames = [json.loads(line) for line in encoded.splitlines()]
        self.assertEqual([frame["id"] for frame in frames], [1, 2])
        self.assertEqual(frames[1]["result"], {})

    def test_initialize_includes_pinwright_usage_instructions(self):
        plugin_root = os.path.normpath(os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."
        ))
        canonical_path = os.path.join(plugin_root, "mcp-instructions.md")
        with open(canonical_path, encoding="utf-8", newline="") as canonical_file:
            canonical_instructions = canonical_file.read()
        self.assertEqual(MCP_INSTRUCTIONS_TEMPLATE, canonical_instructions)

        with tempfile.TemporaryDirectory() as temp:
            response = self._proxy(self._project(temp)).handle({
                "jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}
            })
            wiki_directory = os.path.abspath(os.path.join(
                temp, "Saved", "PinWright", "wiki"
            )).replace("\\", "/").rstrip("/")

        instructions = response["result"].get("instructions")
        expected = canonical_instructions.replace(
            "{{PINWRIGHT_WIKI_DIRECTORY}}", wiki_directory
        )

        self.assertEqual(instructions, expected)
        self.assertTrue(os.path.isabs(wiki_directory))
        self.assertNotIn("\\", wiki_directory)
        self.assertFalse(wiki_directory.endswith("/"))
        self.assertEqual(instructions.count(wiki_directory), 2)
        self.assertIn("<namespace.method>.md", instructions)
        self.assertIn("direct filesystem search and file reading", instructions)
        self.assertIn("without `args` returns documentation", instructions)
        self.assertIn("args: {...}})` executes the RPC", instructions)
        self.assertIn("still require `args: {}` to execute", instructions)

    def test_eof_ends_the_wait_but_leaves_the_detached_editor_running(self):
        with tempfile.TemporaryDirectory() as temp:
            proxy = self._proxy(self._project(temp))
            process = _BlockedProcess()
            spawned = threading.Event()
            input_stream = _ControlledInput()
            output_stream = _CapturedOutput()

            def supervise(*_args, **_kwargs):
                spawned.set()
                return process

            with mock.patch(
                    "mcp_proxy.resolve_editor",
                    return_value=("C:\\UE_5.8", "Editor.exe"),
                ), mock.patch(
                    "mcp_proxy.pinwright_supervisor.spawn_supervised", side_effect=supervise
                ):
                server = threading.Thread(
                    target=serve_stdio, args=(proxy, input_stream, output_stream)
                )
                server.start()
                input_stream.send({
                    "jsonrpc": "2.0",
                    "id": 7,
                    "method": "tools/call",
                    "params": {
                        "name": "editor_start",
                        "arguments": {"mode": "offscreen", "reason": REASON, "wait": "exit"},
                    },
                })
                self.assertTrue(spawned.wait(2.0))
                input_stream.close()
                server.join(5.0)

        self.assertFalse(server.is_alive())
        self.assertTrue(proxy.shutdown_requested())
        self.assertFalse(process.terminated)
        self.assertFalse(process.killed)
        self.assertEqual(output_stream.getvalue(), "")


class ProxyCliTest(unittest.TestCase):
    def test_run_timeout_option_is_removed(self):
        parser = _build_argument_parser()
        self.assertNotIn("--run-timeout", parser.format_help())
        with mock.patch("sys.stderr", new_callable=io.StringIO), \
                self.assertRaises(SystemExit):
            parser.parse_args(["--run-timeout", "12"])


class ProxyUnavailableCallTest(unittest.TestCase):
    def _call(self):
        return {
            "jsonrpc": "2.0",
            "id": 7,
            "method": "tools/call",
            "params": {"name": "call", "arguments": {}},
        }

    def _proxy(self, url):
        return Proxy(url, 0.1, 0.1, 0.1, None, None)

    def test_missing_endpoint_is_editor_not_running_with_start_instruction(self):
        result = self._proxy(None).handle(self._call())["result"]
        self.assertTrue(result["isError"])
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_NOT_RUNNING")
        self.assertIn("editor_start MCP tool", result["content"][0]["text"])
        self.assertIn("retry call()", result["content"][0]["text"])

    def test_connection_refusal_is_editor_not_running(self):
        proxy = self._proxy("http://127.0.0.1:19880/mcp")
        with mock.patch.object(
                proxy, "_probe_state", return_value=("not_running", "connection refused")):
            result = proxy.handle(self._call())["result"]
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_NOT_RUNNING")

    def test_liveness_timeout_remains_unresponsive(self):
        proxy = self._proxy("http://127.0.0.1:19880/mcp")
        with mock.patch.object(
                proxy, "_probe_state",
                return_value=("unresponsive", "liveness ping timed out")):
            result = proxy.handle(self._call())["result"]
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_UNRESPONSIVE")
        self.assertNotIn("EDITOR_NOT_RUNNING", result["content"][0]["text"])

    def test_ping_without_editor_ready_is_starting_not_alive(self):
        proxy = self._proxy("http://127.0.0.1:19880/mcp")
        response = {
            "jsonrpc": "2.0",
            "id": "_proxy_probe",
            "result": {
                "editorReady": False,
                "retryable": True,
                "editorLoadingPackage": True,
                "editorSelectionSetAvailable": False,
                "error": "EDITOR_NOT_READY",
            },
        }
        with mock.patch.object(proxy, "_post", return_value=response) as post:
            state, detail = proxy._probe_state(proxy.url)
        self.assertEqual(state, "not_ready")
        self.assertIn("not reached operational readiness", detail)
        self.assertEqual(post.call_args.args[0]["method"], "ping")

    def test_public_call_while_editor_starting_is_retryable_not_ready(self):
        proxy = self._proxy("http://127.0.0.1:19880/mcp")
        with mock.patch.object(
                proxy, "_probe_state",
                return_value=("not_ready", "packages are still loading")):
            result = proxy.handle(self._call())["result"]
        self.assertTrue(result["isError"])
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_NOT_READY")
        self.assertTrue(result["structuredContent"]["retryable"])
        self.assertIn("packages are still loading", result["content"][0]["text"])

    def test_forwarding_connection_failure_becomes_editor_not_running(self):
        proxy = self._proxy("http://127.0.0.1:19880/mcp")
        with mock.patch.object(proxy, "_probe_state", return_value=("alive", None)), \
                mock.patch.object(proxy, "_post",
                                  side_effect=ConnectionRefusedError("refused")):
            result = proxy.handle(self._call())["result"]
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_NOT_RUNNING")

    def test_forwarding_timeout_remains_unresponsive(self):
        proxy = self._proxy("http://127.0.0.1:19880/mcp")
        with mock.patch.object(proxy, "_probe_state", return_value=("alive", None)), \
                mock.patch.object(proxy, "_post", side_effect=TimeoutError("timed out")):
            result = proxy.handle(self._call())["result"]
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_UNRESPONSIVE")

    def test_ping_without_the_editor_ready_key_is_a_stale_plugin_binary(self):
        # An editor built before the readiness protocol answers `ping` with {} - a dict
        # with NO editorReady key. That is terminal (the bit will never appear), not the
        # transient not_ready state, so it must classify separately.
        proxy = self._proxy("http://127.0.0.1:19880/mcp")
        response = {"jsonrpc": "2.0", "id": "_proxy_probe", "result": {}}
        with mock.patch.object(proxy, "_post", return_value=response):
            state, detail = proxy._probe_state(proxy.url)
        self.assertEqual(state, "protocol_stale")
        self.assertIn("no editorReady field", detail)
        self.assertIn("rebuild the PinWright plugin", detail)

    def test_stale_plugin_binary_call_is_not_retryable(self):
        # The regression this guards: the old code returned EDITOR_NOT_READY with
        # retryable:true for this state, so a polite client looped forever.
        proxy = self._proxy("http://127.0.0.1:19880/mcp")
        with mock.patch.object(
                proxy, "_probe_state",
                return_value=("protocol_stale", "compiled plugin predates the proxy")):
            result = proxy.handle(self._call())["result"]
        self.assertTrue(result["isError"])
        self.assertEqual(
            result["structuredContent"]["error"], "EDITOR_PLUGIN_OUTDATED")
        self.assertFalse(result["structuredContent"]["retryable"])
        self.assertNotIn("EDITOR_NOT_READY", result["content"][0]["text"])

    def test_ping_result_that_is_not_an_object_still_reads_as_not_ready(self):
        # Only a well-formed dict WITHOUT the key means "stale binary". A ping that
        # answers with an error envelope (no `result`) keeps its old not_ready meaning.
        proxy = self._proxy("http://127.0.0.1:19880/mcp")
        response = {"jsonrpc": "2.0", "id": "_proxy_probe",
                    "error": {"code": -32603, "message": "boom"}}
        with mock.patch.object(proxy, "_post", return_value=response):
            state, detail = proxy._probe_state(proxy.url)
        self.assertEqual(state, "not_ready")
        self.assertIn("retry after startup completes", detail)

    def test_editor_ready_false_remains_the_retryable_not_ready_state(self):
        # Guard the untouched path: the key PRESENT and False is a genuinely starting
        # editor and must stay retryable.
        proxy = self._proxy("http://127.0.0.1:19880/mcp")
        response = {"jsonrpc": "2.0", "id": "_proxy_probe",
                    "result": {"editorReady": False}}
        with mock.patch.object(proxy, "_post", return_value=response):
            state, _ = proxy._probe_state(proxy.url)
        self.assertEqual(state, "not_ready")
        with mock.patch.object(
                proxy, "_probe_state", return_value=("not_ready", "still starting")):
            result = proxy.handle(self._call())["result"]
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_NOT_READY")
        self.assertTrue(result["structuredContent"]["retryable"])


class ProxyEditorRunTestsTest(unittest.TestCase):
    URL = "http://127.0.0.1:19880/mcp"

    def _proxy(self, uproject=None, url=None):
        proxy = Proxy(
            url,
            list_timeout=0.1,
            call_timeout=0.1,
            probe_timeout=0.1,
            token_file=None,
            port_file=None,
            editor_exe=None,
            start_timeout=0.01,
            uproject=uproject,
        )
        proxy.test_start_timeout = 0.0
        return proxy

    def _project(self, temp):
        path = os.path.join(temp, "Host Project.uproject")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump({"EngineAssociation": "5.8"}, fh)
        return path

    def _engine(self, project):
        root = os.path.join(os.path.dirname(project), "UE_5.8")
        editor = os.path.join(root, "Engine", "Binaries", "Win64",
                              "UnrealEditor.exe")
        return root, editor

    def _run(self, proxy, args=None, probe=("not_running", "connection refused"),
             progress=None, run=None):
        """Drive the real _editor_run_tests with the supervisor and the log scan faked."""
        engine_root, editor_exe = self._engine(proxy.uproject)
        run = run or _BlockedProcess()
        progress = progress or {"exists": True, "started": 1, "succeeded": 0, "failed": 0,
                                "lastTest": "PinWright.infra.first"}
        with mock.patch.object(proxy, "_probe_state", return_value=probe), \
                mock.patch("mcp_proxy.resolve_editor",
                           return_value=(engine_root, editor_exe)), \
                mock.patch("mcp_proxy.os.path.isfile", return_value=True), \
                mock.patch("mcp_proxy._visible_launch_env", return_value={}), \
                mock.patch("mcp_proxy.subprocess.Popen",
                           side_effect=AssertionError("tests run only under the supervisor")), \
                mock.patch("mcp_proxy.pinwright_supervisor.scan_test_progress",
                           return_value=progress), \
                mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                           return_value=run) as supervised:
            result = proxy._editor_run_tests(
                args if args is not None
                else {"filter": "PinWright", "reason": REASON, "mode": "offscreen"})
        return result, supervised, engine_root, editor_exe

    def test_tool_schemas(self):
        schema = EDITOR_RUN_TESTS_TOOL["inputSchema"]
        self.assertEqual(set(schema["required"]), {"filter", "reason", "mode"})
        self.assertEqual(set(schema["properties"]), {"filter", "reason", "mode", "slot_wait"})
        self.assertFalse(schema["additionalProperties"])
        self.assertEqual(set(EDITOR_TEST_STATUS_TOOL["inputSchema"]["properties"]),
                         {"logPath", "runId"})
        self.assertNotIn("required", EDITOR_LIST_TOOL["inputSchema"])

    def test_tools_list_and_dispatch_are_proxy_local(self):
        proxy = self._proxy()
        with mock.patch.object(proxy, "refresh_tools", return_value=[{"name": "call"}]):
            response = proxy.handle({
                "jsonrpc": "2.0",
                "id": 1,
                "method": "tools/list",
            })
        self.assertEqual(
            [tool["name"] for tool in response["result"]["tools"]],
            ["call", "editor_start", "editor_restart", "editor_run_tests",
             "editor_test_status", "editor_list", "editor_build", "editor_build_status"],
        )
        self.assertFalse(hasattr(mcp_proxy, "EDITOR_PREPARE_TESTS_TOOL"))

        expected = {"content": [], "structuredContent": {"ok": True}, "isError": False}
        for name, method, arguments in (
                ("editor_run_tests", "_editor_run_tests", {"filter": "X"}),
                ("editor_test_status", "_editor_test_status", {"logPath": "x.log"}),
                ("editor_list", "_editor_list", {})):
            with self.subTest(tool=name), \
                    mock.patch.object(proxy, method, return_value=expected) as handler:
                response = proxy.handle({
                    "jsonrpc": "2.0", "id": 2, "method": "tools/call",
                    "params": {"name": name, "arguments": arguments},
                })
                handler.assert_called_once_with(arguments)
                self.assertEqual(response["result"], expected)
                self.assertFalse(mcp_proxy._forwards_concurrently({
                    "method": "tools/call", "id": 2, "params": {"name": name}}))

    def test_missing_or_invalid_arguments_are_refused_before_the_guard(self):
        invalid = (
            (None, "INVALID_ARGUMENTS"),
            ({"filter": "X", "reason": REASON}, "MISSING_REQUIRED_PARAM"),
            ({"filter": "X", "mode": "windowless", "reason": REASON}, "INVALID_MODE"),
            ({"filter": "X", "mode": "offscreen"}, "MISSING_REQUIRED_PARAM"),
            ({"filter": "X", "mode": "offscreen", "reason": "  "}, "INVALID_REASON"),
            ({"mode": "offscreen", "reason": REASON}, "INVALID_FILTER"),
            ({"filter": "", "mode": "offscreen", "reason": REASON}, "INVALID_FILTER"),
            ({"filter": " \t ", "mode": "offscreen", "reason": REASON}, "INVALID_FILTER"),
            ({"filter": "A\nB", "mode": "offscreen", "reason": REASON}, "INVALID_FILTER"),
            ({"filter": "A\u0085B", "mode": "offscreen", "reason": REASON}, "INVALID_FILTER"),
            ({"filter": "A,B", "mode": "offscreen", "reason": REASON}, "INVALID_FILTER"),
            ({"filter": "A;B", "mode": "offscreen", "reason": REASON}, "INVALID_FILTER"),
            ({"filter": 7, "mode": "offscreen", "reason": REASON}, "INVALID_FILTER"),
            ({"filter": "X", "mode": "offscreen", "reason": REASON, "extra_args": []},
             "INVALID_ARGUMENTS"),
        )
        proxy = self._proxy(uproject="missing.uproject", url=self.URL)
        with mock.patch.object(proxy, "_probe_state",
                               side_effect=AssertionError("guard ran before validation")), \
                mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                           side_effect=AssertionError("must not spawn")):
            for arguments, code in invalid:
                with self.subTest(arguments=arguments):
                    result = proxy._editor_run_tests(arguments)
                    self.assertEqual(result["structuredContent"]["error"], code)

    def test_a_live_editor_of_this_project_blocks_the_run(self):
        # Kept from editor_prepare_tests: a second editor of this project would fight the live
        # one for the gateway port and the project's Saved/ state.
        with tempfile.TemporaryDirectory() as temp:
            proxy = self._proxy(uproject=self._project(temp), url=self.URL)
            result, supervised, _root, _exe = self._run(proxy, probe=("alive", None))
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_ALREADY_RUNNING")
        self.assertEqual(result["structuredContent"]["editorGuard"]["state"], "alive")
        supervised.assert_not_called()

    def test_windowless_run_launches_the_suite_contract_and_returns_once_tests_start(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project, url=self.URL)
            result, supervised, _engine_root, _editor_exe = self._run(proxy)
        self.assertFalse(result["isError"], result)
        structured = result["structuredContent"]
        self.assertEqual(structured["status"], "TESTS_STARTED")
        self.assertEqual(structured["startedTests"], 1)
        self.assertEqual(structured["pid"], _BlockedProcess.pid)
        self.assertEqual(structured["project"], os.path.abspath(project))
        self.assertEqual(structured["reason"], REASON)
        self.assertEqual(structured["mode"], "offscreen")
        self.assertEqual(len(structured["runId"]), 32)
        self.assertTrue(os.path.isabs(structured["logPath"]))
        self.assertIn(structured["runId"], structured["logPath"])

        argv = supervised.call_args.args[0]
        kwargs = supervised.call_args.kwargs
        self.assertEqual(argv[1], os.path.abspath(project))
        self.assertIn("-ExecCmds=Automation RunTests PinWright,Quit", argv)
        self.assertIn("-TestExit=Automation Test Queue Empty", argv)
        for flag in ("-unattended", "-nopause", "-nosplash", "-nosound", "-RenderOffscreen",
                     "-nocefaccelpaint", "-RunningUnattendedScript",
                     "-ddc=InstalledNoZenLocalFallback", "-Abslog=" + structured["logPath"]):
            self.assertIn(flag, argv)
        self.assertFalse(any("nullrhi" in arg.lower() for arg in argv))
        self.assertEqual(kwargs["kind"], "suite")
        self.assertEqual(kwargs["reason"], REASON)
        self.assertEqual(kwargs["launched_by"], "editor_run_tests")
        self.assertEqual(kwargs["log_path"], structured["logPath"])

    @unittest.skipUnless(sys.platform == "win32",
                         "the UnrealEditor-Cmd console twin exists only on Windows (a subsystem "
                         "workaround); Linux runs the plain binary, pinned by "
                         "SuiteExecutableTest.test_linux_windowless_is_plain_binary")
    def test_windowless_runs_use_the_cmd_console_twin(self):
        for mode in ("offscreen", "headless"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temp:
                proxy = self._proxy(uproject=self._project(temp), url=self.URL)
                result, supervised, _root, editor_exe = self._run(
                    proxy, {"filter": "PinWright", "reason": REASON, "mode": mode})
                self.assertFalse(result["isError"], result)
                self.assertEqual(supervised.call_args.args[0][0],
                                 os.path.abspath(_editor_cmd_from_editor(editor_exe)))

    def test_headless_run_adds_nullrhi_to_the_offscreen_flags(self):
        with tempfile.TemporaryDirectory() as temp:
            proxy = self._proxy(uproject=self._project(temp), url=self.URL)
            result, supervised, _root, _editor_exe = self._run(
                proxy, {"filter": "PinWright", "reason": REASON, "mode": "headless"})
        self.assertFalse(result["isError"], result)
        argv = supervised.call_args.args[0]
        self.assertIn("-NullRHI", argv)
        self.assertIn("-RenderOffscreen", argv)
        self.assertEqual(supervised.call_args.kwargs["mode"], "headless")
        self.assertEqual(result["structuredContent"]["mode"], "headless")
        self.assertIn("headless", result["content"][0]["text"] + str(result["structuredContent"]))

    def test_visible_run_uses_the_gui_binary_without_render_offscreen(self):
        with tempfile.TemporaryDirectory() as temp:
            proxy = self._proxy(uproject=self._project(temp), url=self.URL)
            result, supervised, _root, editor_exe = self._run(
                proxy, {"filter": "PinWright", "reason": REASON, "mode": "visible"})
        self.assertFalse(result["isError"], result)
        argv = supervised.call_args.args[0]
        self.assertEqual(argv[0], os.path.abspath(editor_exe))
        self.assertNotIn("-RenderOffscreen", argv)
        self.assertIn("-RunningUnattendedScript", argv)
        self.assertEqual(result["structuredContent"]["mode"], "visible")

    def test_exit_before_the_first_test_is_a_typed_error(self):
        with tempfile.TemporaryDirectory() as temp:
            proxy = self._proxy(uproject=self._project(temp), url=self.URL)
            result, _sup, _root, _exe = self._run(
                proxy, progress={"exists": False, "started": 0, "succeeded": 0, "failed": 0,
                                 "lastTest": None},
                run=_CompletedProcess(code=3))
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_EXITED_BEFORE_TESTS")
        self.assertEqual(result["structuredContent"]["exitCode"], 3)
        self.assertFalse(result["structuredContent"]["leftRunning"])

    def test_no_test_within_the_deadline_is_a_typed_error_and_the_run_is_left_alone(self):
        with tempfile.TemporaryDirectory() as temp:
            proxy = self._proxy(uproject=self._project(temp), url=self.URL)
            run = _BlockedProcess()
            result, _sup, _root, _exe = self._run(
                proxy, progress={"exists": True, "started": 0, "succeeded": 0, "failed": 0,
                                 "lastTest": None}, run=run)
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_TESTS_NOT_STARTED")
        self.assertTrue(result["structuredContent"]["leftRunning"])
        self.assertFalse(run.terminated)
        self.assertEqual(mcp_proxy.TEST_START_TIMEOUT, 600.0)

    def test_a_modal_blocked_test_editor_is_reported(self):
        with tempfile.TemporaryDirectory() as temp:
            proxy = self._proxy(uproject=self._project(temp), url=self.URL)
            probes = iter([("not_running", "refused"), ("blocked_on_modal", "dialog")])
            engine_root, editor_exe = self._engine(proxy.uproject)
            with mock.patch.object(proxy, "_probe_state",
                                   side_effect=lambda _url: next(probes)), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=(engine_root, editor_exe)), \
                    mock.patch("mcp_proxy.os.path.isfile", return_value=True), \
                    mock.patch("mcp_proxy.pinwright_supervisor.scan_test_progress",
                               return_value={"exists": True, "started": 0, "succeeded": 0,
                                             "failed": 0, "lastTest": None}), \
                    mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                               return_value=_BlockedProcess()):
                proxy.test_start_timeout = 60.0
                result = proxy._editor_run_tests(
                    {"filter": "PinWright", "reason": REASON, "mode": "offscreen"})
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_BLOCKED_ON_MODAL")

    def test_engine_association_selects_the_engine(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project, url=self.URL)
            engine_root, editor_exe = self._engine(project)
            with mock.patch.object(proxy, "_probe_state",
                                   return_value=("not_running", "connection refused")), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=(engine_root, editor_exe)) as resolve, \
                    mock.patch("mcp_proxy.os.path.isfile", return_value=True), \
                    mock.patch("mcp_proxy.pinwright_supervisor.scan_test_progress",
                               return_value={"exists": True, "started": 2, "succeeded": 1,
                                             "failed": 0, "lastTest": None}), \
                    mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                               return_value=_BlockedProcess()):
                result = proxy._editor_run_tests(
                    {"filter": "PinWright", "reason": REASON, "mode": "offscreen"})
        self.assertFalse(result["isError"])
        self.assertEqual(resolve.call_args.args[3], "5.8")

    def test_windows_requires_the_cmd_twin_for_a_windowless_run(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project)
            root, editor_exe = self._engine(project)
            with mock.patch.object(proxy, "_probe_state",
                                   return_value=("not_running", "connection refused")), \
                    mock.patch("mcp_proxy.resolve_editor", return_value=(root, editor_exe)), \
                    mock.patch("mcp_proxy.os.path.isfile",
                               side_effect=lambda path: "-Cmd" not in os.path.basename(path)), \
                    mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                               side_effect=AssertionError("must not spawn")), \
                    mock.patch.object(pinwright_supervisor.os, "name", "nt"), \
                    mock.patch.object(mcp_proxy.os, "name", "nt"):
                result = proxy._editor_run_tests(
                    {"filter": "PinWright", "reason": REASON, "mode": "offscreen"})
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_COMMAND_NOT_FOUND")


def _row(pid, argv, exe=None, start_ms=1759140930000, cwd=None):
    """A raw process row as the platform enumerators return it."""
    return {"pid": pid, "exe": exe or argv[0], "argv": argv,
            "commandLine": subprocess.list2cmdline(argv), "startMs": start_ms, "cwd": cwd}


class EditorListTest(unittest.TestCase):
    """editor_list reads everything from each editor's own command line: no launch registry."""

    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.root = self._temp.name
        self.engine = os.path.join(self.root, "UE 5.8")
        self.exe = os.path.join(self.engine, "Engine", "Binaries", "Win64", "UnrealEditor.exe")
        self.cmd_exe = os.path.join(self.engine, "Engine", "Binaries", "Win64",
                                    "UnrealEditor-Cmd.exe")

    def _project(self, *parts):
        path = os.path.join(self.root, *parts)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("{}")
        return path

    def test_quoted_project_path_with_spaces_resolves_from_the_command_line(self):
        project = self._project("My Checkout", "Game Proj.uproject")
        command_line = '"%s" "%s" -game' % (self.exe, project)
        row = {"pid": 11, "exe": self.exe, "commandLine": command_line,
               "argv": pinwright_supervisor.split_windows_command_line(command_line),
               "startMs": 1759140930000, "cwd": None}
        entry = mcp_proxy.describe_editor_process(row)
        self.assertTrue(_same(entry["project"], project))
        self.assertEqual(entry["projectName"], "Game Proj")
        self.assertTrue(_same(entry["checkoutRoot"], os.path.dirname(project)))
        self.assertEqual(entry["projectSource"], "commandLine")
        self.assertEqual(entry["mode"], "game")
        self.assertEqual(entry["startTime"], "2025-09-29T10:15:30Z")

    def test_relative_project_path_resolves_against_the_process_working_directory(self):
        project = self._project("work", "Rel", "Rel.uproject")
        row = _row(12, [self.exe, os.path.join("Rel", "Rel.uproject")],
                   cwd=os.path.join(self.root, "work"))
        entry = mcp_proxy.describe_editor_process(row)
        self.assertTrue(_same(entry["project"], project))
        self.assertEqual(entry["projectSource"], "commandLine")

    def test_project_passed_by_name_resolves_under_the_engine_root(self):
        project = self._project("UE 5.8", "Named", "Named.uproject")
        entry = mcp_proxy.describe_editor_process(_row(13, [self.exe, "Named", "-log"]))
        self.assertTrue(_same(entry["project"], project))
        self.assertEqual(entry["projectName"], "Named")

    def test_unresolvable_project_reports_unresolved(self):
        entry = mcp_proxy.describe_editor_process(_row(14, [self.exe, "Missing", "-log"]))
        self.assertIsNone(entry["project"])
        self.assertIsNone(entry["checkoutRoot"])
        self.assertEqual(entry["projectSource"], "unresolved")
        entry = mcp_proxy.describe_editor_process(_row(15, [self.exe, "-log"]))
        self.assertEqual(entry["projectSource"], "unresolved")
        self.assertIsNone(entry["projectName"])

    def test_two_checkouts_of_the_same_project_are_told_apart(self):
        main = self._project("game", "Game.uproject")
        dev = self._project("game-dev", "Game.uproject")
        port_dir = os.path.join(self.root, "game-dev", "Saved", "PinWright")
        os.makedirs(port_dir)
        with open(os.path.join(port_dir, "gateway-port"), "w", encoding="utf-8") as fh:
            fh.write("24966\n")
        first = mcp_proxy.describe_editor_process(_row(21, [self.exe, main]), this_project=main)
        second = mcp_proxy.describe_editor_process(_row(22, [self.exe, dev]), this_project=main)
        self.assertEqual(first["projectName"], second["projectName"])
        self.assertFalse(_same(first["checkoutRoot"], second["checkoutRoot"]))
        self.assertTrue(first["isThisProject"])
        self.assertFalse(second["isThisProject"])
        self.assertIsNone(first["gatewayPort"])
        self.assertEqual(second["gatewayPort"], 24966)

    def test_reason_and_launcher_come_from_the_launch_switches(self):
        project = self._project("p", "P.uproject")
        reason = 'fix "B-12", C:\\dir\\ 100% \u043f\u0440\u043e\u0432\u0435\u0440\u043a\u0430'
        argv = ([self.exe, project, "/Game/Maps/Example", "-RenderOffScreen", "-unattended",
                 "-Abslog=C:/logs/run.log"]
                + pinwright_supervisor.launch_identity_args(reason, "editor_start"))
        # Round-trip through the Windows command-line encoding the census actually reads.
        command_line = subprocess.list2cmdline(argv)
        row = {"pid": 31, "exe": self.exe, "commandLine": command_line,
               "argv": pinwright_supervisor.split_windows_command_line(command_line),
               "startMs": None, "cwd": None}
        entry = mcp_proxy.describe_editor_process(row)
        self.assertEqual(entry["reason"], reason)
        self.assertEqual(entry["launchedBy"], "editor_start")
        self.assertEqual(entry["mode"], "offscreen")
        self.assertEqual(entry["map"], "/Game/Maps/Example")
        self.assertEqual(entry["logPath"], "C:/logs/run.log")
        self.assertIsNone(entry["startTime"])

    def test_foreign_editor_without_switches_is_unknown(self):
        project = self._project("other-repo", "Other.uproject")
        entry = mcp_proxy.describe_editor_process(_row(41, [self.exe, project]))
        self.assertIsNone(entry["reason"])
        self.assertEqual(entry["launchedBy"], "unknown")
        self.assertEqual(entry["mode"], "visible")

    def test_headless_editor_is_classified_and_commandlet_wins_over_nullrhi(self):
        project = self._project("h", "H.uproject")
        entry = mcp_proxy.describe_editor_process(
            _row(52, [self.exe, project, "-NullRHI", "-RenderOffScreen"]))
        self.assertEqual(entry["mode"], "headless")
        entry = mcp_proxy.describe_editor_process(
            _row(53, [self.cmd_exe, project, "-run=Cook", "-NullRHI"]))
        self.assertEqual(entry["mode"], "commandlet")

    def test_commandlet_is_classified(self):
        project = self._project("c", "C.uproject")
        entry = mcp_proxy.describe_editor_process(
            _row(51, [self.cmd_exe, project, "-run=ResavePackages", "-unattended"]))
        self.assertEqual(entry["mode"], "commandlet")
        self.assertTrue(_same(entry["engineRoot"], self.engine))

    def test_windows_census_parses_the_cim_json(self):
        project = self._project("w", "W.uproject")
        reason_args = pinwright_supervisor.launch_identity_args("nightly suite", "editor_run_tests")
        rows = [
            {"ProcessId": 100, "ExecutablePath": self.exe, "StartMs": 1759140930123,
             "CommandLine": subprocess.list2cmdline([self.exe, project] + reason_args)},
            {"ProcessId": 101, "ExecutablePath": self.cmd_exe, "StartMs": 1759140931000,
             "CommandLine": subprocess.list2cmdline([self.cmd_exe, project, "-run=Foo"])},
        ]
        completed = subprocess.CompletedProcess(
            args=[], returncode=0, stdout=json.dumps(rows).encode("utf-8"), stderr=b"")
        processes = mcp_proxy._windows_editor_processes(run=lambda *a, **k: completed)
        self.assertEqual([proc["pid"] for proc in processes], [100, 101])
        self.assertEqual(processes[0]["argv"][-1], "-PinWrightLaunchedBy=editor_run_tests")
        self.assertIsNone(processes[0]["cwd"])
        # A single CIM row serializes as an object, not an array.
        single = subprocess.CompletedProcess(
            args=[], returncode=0, stdout=json.dumps(rows[0]).encode("utf-8"), stderr=b"")
        self.assertEqual(len(mcp_proxy._windows_editor_processes(run=lambda *a, **k: single)), 1)
        empty = subprocess.CompletedProcess(args=[], returncode=0, stdout=b"", stderr=b"")
        self.assertEqual(mcp_proxy._windows_editor_processes(run=lambda *a, **k: empty), [])

    def test_windows_census_failure_is_an_error_not_an_empty_list(self):
        failed = subprocess.CompletedProcess(args=[], returncode=1, stdout=b"",
                                             stderr=b"Get-CimInstance: access denied")
        with self.assertRaises(RuntimeError):
            mcp_proxy._windows_editor_processes(run=lambda *a, **k: failed)
        proxy = Proxy(None, 0.1, 0.1, 0.1, None, None)
        with mock.patch("mcp_proxy._editor_processes", side_effect=RuntimeError("denied")):
            result = proxy._editor_list({})
        self.assertTrue(result["isError"])
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_LIST_FAILED")

    def _proc_tree(self, entries):
        """A canned /proc: {pid: (argv, starttime_ticks)} plus /proc/stat btime."""
        proc_root = os.path.join(self.root, "proc")
        os.makedirs(proc_root)
        with open(os.path.join(proc_root, "stat"), "w", encoding="utf-8") as fh:
            fh.write("cpu 1 2 3\nbtime 1759140000\n")
        for pid, (argv, ticks) in entries.items():
            base = os.path.join(proc_root, str(pid))
            os.makedirs(base)
            with open(os.path.join(base, "cmdline"), "wb") as fh:
                fh.write(b"\0".join(arg.encode("utf-8") for arg in argv) + b"\0")
            fields = ["S"] + ["0"] * 18 + [str(ticks)] + ["0"] * 10
            with open(os.path.join(base, "stat"), "w", encoding="utf-8") as fh:
                fh.write("%d (%s) %s\n" % (pid, os.path.basename(argv[0])[:15],
                                            " ".join(fields)))
        os.makedirs(os.path.join(proc_root, "self"))
        return proc_root

    def test_linux_census_reads_proc(self):
        linux_exe = "/opt/UE_5.8/Engine/Binaries/Linux/UnrealEditor"
        proc_root = self._proc_tree({
            300: ([linux_exe, "/home/u/a b/Proj.uproject", "-RenderOffscreen", "-unattended"]
                  + pinwright_supervisor.launch_identity_args("linux run", "cli"), 9300),
            301: (["/usr/bin/bash", "-c", "sleep 1"], 100),
            302: ([linux_exe, "/home/u/other/Other.uproject", "-run=Cook"], 200),
        })
        processes = mcp_proxy._linux_editor_processes(proc_root, clk_tck=100)
        self.assertEqual([proc["pid"] for proc in processes], [300, 302])
        self.assertEqual(processes[0]["argv"][1], "/home/u/a b/Proj.uproject")
        self.assertEqual(processes[0]["startMs"], (1759140000 + 93) * 1000)
        entry = mcp_proxy.describe_editor_process(processes[0])
        self.assertEqual(entry["reason"], "linux run")
        self.assertEqual(entry["launchedBy"], "cli")
        self.assertEqual(entry["mode"], "offscreen")
        self.assertEqual(entry["projectName"], "Proj")
        self.assertEqual(mcp_proxy.describe_editor_process(processes[1])["mode"], "commandlet")

    def test_editor_list_result_marks_this_project_and_sorts_by_start(self):
        mine = self._project("mine", "Mine.uproject")
        other = self._project("theirs", "Theirs.uproject")
        rows = [_row(2, [self.exe, other], start_ms=2000),
                _row(1, [self.exe, mine] + pinwright_supervisor.launch_identity_args(
                    "why", "editor_start"), start_ms=1000)]
        proxy = Proxy(None, 0.1, 0.1, 0.1, None, None, uproject=mine)
        with mock.patch("mcp_proxy._editor_processes", return_value=rows):
            result = proxy._editor_list({})
        self.assertFalse(result["isError"])
        editors = result["structuredContent"]["editors"]
        self.assertEqual([entry["pid"] for entry in editors], [1, 2])
        self.assertTrue(editors[0]["isThisProject"])
        self.assertEqual(editors[0]["reason"], "why")
        self.assertIn("not launched by PinWright", result["content"][0]["text"])


class EditorTestStatusTest(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.log = os.path.join(self._temp.name, "automation.log")
        self.proxy = Proxy(None, 0.1, 0.1, 0.1, None, None)

    def _status(self, args, rows=(), progress=None, verdict=None):
        progress = progress or {"exists": True, "started": 3, "succeeded": 2, "failed": 0,
                                "lastTest": "PinWright.a.b"}
        with mock.patch("mcp_proxy._editor_processes", return_value=list(rows)), \
                mock.patch("mcp_proxy.pinwright_supervisor.scan_test_progress",
                           return_value=progress), \
                mock.patch("check_suite_log.check_log",
                           return_value=verdict or {"state": "COMPLETED_CLEAN", "reason": "ok",
                                                    "warnings": []}) as check:
            return self.proxy._editor_test_status(args), check

    def test_running_while_an_editor_writes_that_log(self):
        exe = "C:\\UE\\Engine\\Binaries\\Win64\\UnrealEditor-Cmd.exe"
        rows = [_row(77, [exe, "P.uproject", "-Abslog=" + self.log])]
        result, check = self._status({"logPath": self.log}, rows=rows)
        structured = result["structuredContent"]
        self.assertEqual(structured["status"], "running")
        self.assertEqual(structured["mode"], "visible")
        self.assertEqual(structured["pid"], 77)
        self.assertEqual((structured["started"], structured["succeeded"]), (3, 2))
        self.assertNotIn("verdict", structured)
        check.assert_not_called()

    def test_finished_runs_carry_the_check_suite_log_verdict(self):
        with open(self.log, "w", encoding="utf-8") as fh:
            fh.write("LogInit: Command Line:  C:/P/H.uproject -NullRHI -RenderOffscreen "
                     "-unattended \"-Abslog=%s\"\n" % self.log)
        with open(self.log + ".result.txt", "w", encoding="utf-8") as fh:
            fh.write("PINWRIGHT_SUITE_RESULT verdict=EDITOR_EXITED exit=0\n")
        result, check = self._status(
            {"logPath": self.log},
            verdict={"state": "COMPLETED_WITH_FAILURES", "reason": "1 failed", "warnings": []})
        structured = result["structuredContent"]
        self.assertEqual(structured["status"], "finished")
        self.assertEqual(structured["mode"], "headless")
        self.assertIsNone(structured["pid"])
        self.assertEqual(structured["verdict"]["state"], "COMPLETED_WITH_FAILURES")
        self.assertTrue(structured["supervisorResult"].startswith("PINWRIGHT_SUITE_RESULT"))
        check.assert_called_once_with(os.path.abspath(self.log))

    def test_run_id_is_resolved_under_this_project(self):
        project = os.path.join(self._temp.name, "Host.uproject")
        with open(project, "w", encoding="utf-8") as fh:
            fh.write("{}")
        self.proxy.uproject = project
        run_id = "0123456789abcdef0123456789abcdef"
        result, _check = self._status({"runId": run_id})
        self.assertTrue(_same(result["structuredContent"]["logPath"], os.path.join(
            self._temp.name, "Saved", "PinWright", "test-runs", run_id, "automation.log")))

    def test_bad_arguments_are_refused(self):
        for args, code in (({}, "MISSING_REQUIRED_PARAM"),
                           ({"logPath": "a", "runId": "b"}, "MISSING_REQUIRED_PARAM"),
                           ({"runId": "../x"}, "INVALID_RUN_ID"),
                           ({"logPath": "  "}, "INVALID_LOG_PATH"),
                           ({"logPath": "a", "extra": 1}, "INVALID_ARGUMENTS")):
            with self.subTest(args=args):
                result, _check = self._status(args)
                self.assertEqual(result["structuredContent"]["error"], code)

    def test_unknown_log_with_no_editor_is_not_found(self):
        result, _check = self._status(
            {"logPath": self.log},
            progress={"exists": False, "started": 0, "succeeded": 0, "failed": 0,
                      "lastTest": None})
        self.assertEqual(result["structuredContent"]["error"], "TEST_RUN_NOT_FOUND")


class EditorBuildTest(unittest.TestCase):
    """editor_build: argv, the own-checkout editor guard, and non-blocking start."""

    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.root = self._temp.name
        self.project = os.path.join(self.root, "Host Proj", "Host.uproject")
        os.makedirs(os.path.dirname(self.project))
        with open(self.project, "w", encoding="utf-8") as fh:
            fh.write('{"EngineAssociation": "5.8"}')
        self.engine = os.path.join(self.root, "UE_5.8")
        self.editor = os.path.join(self.engine, "Engine", "Binaries", "Win64", "UnrealEditor.exe")
        self.proxy = Proxy(None, 0.1, 0.1, 0.1, None, None, uproject=self.project)

    def _build(self, args, rows=(), os_name="nt"):
        run = _BlockedProcess()
        with mock.patch("mcp_proxy._editor_processes", return_value=list(rows)), \
                mock.patch("mcp_proxy.resolve_editor", return_value=(self.engine, self.editor)), \
                mock.patch("mcp_proxy.os.path.isfile", return_value=True), \
                mock.patch.object(mcp_proxy.os, "name", os_name), \
                mock.patch("mcp_proxy.subprocess.Popen",
                           side_effect=AssertionError("builds run only under the supervisor")), \
                mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                           return_value=run) as supervised:
            result = self.proxy._editor_build(args)
        return result, supervised

    def test_schema_requires_only_reason_and_has_no_mode(self):
        schema = mcp_proxy.EDITOR_BUILD_TOOL["inputSchema"]
        self.assertEqual(schema["required"], ["reason"])
        self.assertEqual(set(schema["properties"]), {"reason"})
        self.assertEqual(mcp_proxy.EDITOR_BUILD_STATUS_TOOL["inputSchema"]["required"], ["logPath"])
        for name in ("editor_build", "editor_build_status"):
            self.assertIn(name, mcp_proxy._LOCAL_TOOL_NAMES)

    def test_windows_argv_and_immediate_return(self):
        result, supervised = self._build({"reason": "rebuild after a header change"})
        self.assertFalse(result["isError"], result)
        argv = supervised.call_args.args[0]
        self.assertEqual(argv, [
            os.path.join(self.engine, "Engine", "Build", "BatchFiles", "Build.bat"),
            "HostEditor", "Win64", "Development", "-Project=" + os.path.abspath(self.project),
            "-WaitMutex", "-NoHotReloadFromIDE",
            "-Log=" + os.path.join(os.path.dirname(result["structuredContent"]["logPath"]),
                                   "ubt.log")])
        kwargs = supervised.call_args.kwargs
        self.assertEqual(kwargs["kind"], "command")
        self.assertIsNone(kwargs["mode"])
        self.assertEqual(kwargs["launched_by"], "editor_build")
        self.assertIn("reason=rebuild after a header change", kwargs["output_header"])
        structured = result["structuredContent"]
        self.assertEqual(structured["status"], "BUILD_STARTED")
        self.assertEqual(structured["pid"], _BlockedProcess.pid)
        self.assertEqual(kwargs["output_path"], structured["logPath"])
        self.assertTrue(structured["logPath"].startswith(
            os.path.join(os.path.dirname(os.path.abspath(self.project)), "Saved", "PinWright",
                         "builds")))
        # The reason never reaches UBT, which would pass it to the target rules.
        self.assertNotIn("-PinWrightLaunchReason", " ".join(argv))

    def test_linux_uses_build_sh_and_the_linux_platform(self):
        _result, supervised = self._build({"reason": "linux build"}, os_name="posix")
        argv = supervised.call_args.args[0]
        self.assertEqual(argv[0], os.path.join(self.engine, "Engine", "Build", "BatchFiles",
                                               "Linux", "Build.sh"))
        self.assertEqual(argv[1:4], ["HostEditor", "Linux", "Development"])

    def test_an_editor_of_this_checkout_blocks_the_build_and_is_named(self):
        rows = [_row(501, [self.editor, self.project, "-RenderOffScreen"]),
                _row(502, [self.editor.replace("UnrealEditor.exe", "UnrealEditor-Cmd.exe"),
                           self.project, "-run=Cook"])]
        result, supervised = self._build({"reason": "x"}, rows=rows)
        self.assertTrue(result["isError"])
        self.assertEqual(result["structuredContent"]["error"], "BUILD_BLOCKED_BY_EDITOR")
        self.assertEqual(result["structuredContent"]["pids"], [501, 502])
        self.assertIn("501", result["content"][0]["text"])
        supervised.assert_not_called()

    def test_an_editor_of_another_checkout_does_not_block(self):
        other = os.path.join(self.root, "Host Proj-dev", "Host.uproject")
        os.makedirs(os.path.dirname(other))
        with open(other, "w", encoding="utf-8") as fh:
            fh.write("{}")
        result, supervised = self._build({"reason": "x"},
                                         rows=[_row(601, [self.editor, other])])
        self.assertFalse(result["isError"], result)
        supervised.assert_called_once()

    def test_bad_arguments_are_refused_before_anything_runs(self):
        for args, code in (({}, "MISSING_REQUIRED_PARAM"),
                           ({"reason": "  "}, "INVALID_REASON"),
                           ({"reason": "x", "mode": "offscreen"}, "INVALID_ARGUMENTS"),
                           (None, "INVALID_ARGUMENTS")):
            with self.subTest(args=args), \
                    mock.patch("mcp_proxy._editor_processes",
                               side_effect=AssertionError("must not enumerate")), \
                    mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                               side_effect=AssertionError("must not spawn")):
                result = self.proxy._editor_build(args)
                self.assertEqual(result["structuredContent"]["error"], code)

    def test_a_proxy_supervisor_skew_is_its_own_error_naming_the_fix(self):
        skew = pinwright_supervisor.SupervisorVersionMismatch(
            pinwright_supervisor._mismatch_text(3) + " (see x.log)")
        with mock.patch("mcp_proxy._editor_processes", return_value=[]), \
                mock.patch("mcp_proxy.resolve_editor", return_value=(self.engine, self.editor)), \
                mock.patch("mcp_proxy.os.path.isfile", return_value=True), \
                mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised", side_effect=skew):
            result = self.proxy._editor_build({"reason": "x"})
        self.assertTrue(result["isError"])
        self.assertEqual(result["structuredContent"]["error"], "SUPERVISOR_VERSION_MISMATCH")
        self.assertTrue(result["content"][0]["text"].startswith("SUPERVISOR_VERSION_MISMATCH: "))
        self.assertIn("/mcp", result["content"][0]["text"])

    def test_a_failed_census_refuses_rather_than_risking_the_link(self):
        with mock.patch("mcp_proxy._editor_processes", side_effect=RuntimeError("denied")), \
                mock.patch("mcp_proxy.pinwright_supervisor.spawn_supervised",
                           side_effect=AssertionError("must not spawn")):
            result = self.proxy._editor_build({"reason": "x"})
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_LIST_FAILED")


_UBT_SUCCESS = """PinWright editor_build: reason=r; target=HostEditor Win64 Development; project=P
Using bundled DotNet SDK version: 8.0.300
Building HostEditor...
[1/3] Compile [x64] Module.PinWright.cpp
[3/3] Link [x64] UnrealEditor-PinWright.dll
Result: Succeeded
Total execution time: 42.10 seconds
"""

_UBT_COMPILE_ERROR = """Building HostEditor...
[1/4] Compile [x64] Module.PinWright.3.cpp
X:\\P\\Plugins\\PinWright\\Source\\A.cpp(12): error C4930: 'FScopedSink Sink(FString (__cdecl *)(void))': prototyped function not called
X:\\P\\Plugins\\PinWright\\Source\\A.cpp(40): warning C4996: deprecated
Result: Failed (OtherCompilationError)
"""

_UBT_LINK_LOCKED = """[5/5] Link [x64] UnrealEditor-PinWright.dll
LINK : fatal error LNK1104: cannot open file 'X:\\P\\Plugins\\PinWright\\Binaries\\Win64\\UnrealEditor-PinWright.dll'
Result: Failed (OtherCompilationError)
"""


class EditorBuildStatusTest(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.log = os.path.join(self._temp.name, "build.log")
        self.proxy = Proxy(None, 0.1, 0.1, 0.1, None, None)

    def _write(self, path, text):
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)

    def _status(self, log_text=None, result_line=None, child_alive=False, supervisor=True):
        if log_text is not None:
            self._write(self.log, log_text)
        if result_line is not None:
            self._write(self.log + ".result.txt", result_line + "\n")
        if supervisor:
            self._write(self.log + ".supervisor.log", "[t] started pid 9876: Build.bat HostEditor\n")
        with mock.patch("mcp_proxy.pinwright_supervisor.process_start_ms",
                        return_value=123 if child_alive else None):
            return self.proxy._editor_build_status({"logPath": self.log})

    def test_success(self):
        result = self._status(_UBT_SUCCESS, "PINWRIGHT_JOB_RESULT verdict=COMMAND_EXITED exit=0 "
                                            "priority=BelowNormal cap_gb=38 peak_gb=3 wall_min=1")
        structured = result["structuredContent"]
        self.assertEqual(structured["status"], "succeeded")
        self.assertEqual(structured["ubtResult"], "Succeeded")
        self.assertEqual(structured["exitCode"], 0)
        self.assertEqual(structured["errors"], [])

    def test_compile_error_lines_are_extracted(self):
        result = self._status(_UBT_COMPILE_ERROR, "PINWRIGHT_JOB_RESULT "
                              "verdict=COMMAND_EXIT_NONZERO exit=6 priority=BelowNormal")
        structured = result["structuredContent"]
        self.assertEqual(structured["status"], "failed")
        self.assertEqual(structured["ubtResult"], "Failed (OtherCompilationError)")
        self.assertEqual(len(structured["errors"]), 1)
        self.assertIn("error C4930", structured["errors"][0])
        self.assertIn("error C4930", result["content"][0]["text"])

    def test_link_error_with_a_locked_dll(self):
        result = self._status(_UBT_LINK_LOCKED, "PINWRIGHT_JOB_RESULT "
                              "verdict=COMMAND_EXIT_NONZERO exit=6 priority=BelowNormal")
        structured = result["structuredContent"]
        self.assertEqual(structured["status"], "failed")
        self.assertTrue(any("LNK1104" in line for line in structured["errors"]))

    def test_running_while_the_build_child_is_alive(self):
        result = self._status("Building HostEditor...\n", child_alive=True)
        structured = result["structuredContent"]
        self.assertEqual(structured["status"], "running")
        self.assertEqual(structured["pid"], 9876)

    def test_ubt_done_but_result_not_yet_written(self):
        result = self._status(_UBT_SUCCESS, child_alive=False)
        self.assertEqual(result["structuredContent"]["status"], "succeeded")

    def test_supervisor_died_without_a_result(self):
        result = self._status("Building HostEditor...\n", child_alive=False)
        self.assertEqual(result["structuredContent"]["status"], "lost")

    def test_unknown_log_and_bad_arguments(self):
        self.assertEqual(self._status(supervisor=False)["structuredContent"]["error"],
                         "BUILD_NOT_FOUND")
        for args, code in (({}, "MISSING_REQUIRED_PARAM"), ({"logPath": " "}, "INVALID_LOG_PATH"),
                           ({"logPath": "a", "x": 1}, "INVALID_ARGUMENTS")):
            with self.subTest(args=args):
                self.assertEqual(self.proxy._editor_build_status(args)["structuredContent"]["error"],
                                 code)


class AbslogPathTest(unittest.TestCase):
    def test_finds_abslog_value(self):
        self.assertEqual(
            _abslog_path(["-ExecCmds=Automation RunTests X,Quit", "-Abslog=C:/logs/x.log"]),
            "C:/logs/x.log")

    def test_case_insensitive(self):
        self.assertEqual(_abslog_path(["-abslog=/tmp/a.log"]), "/tmp/a.log")
        self.assertEqual(_abslog_path(["-AbsLog=/tmp/b.log"]), "/tmp/b.log")

    def test_strips_surrounding_quotes(self):
        self.assertEqual(_abslog_path(['-Abslog="C:/path with spaces/x.log"']),
                         "C:/path with spaces/x.log")

    def test_none_when_absent(self):
        self.assertIsNone(_abslog_path(["-unattended", "-nosplash"]))

    def test_none_for_empty_or_none(self):
        self.assertIsNone(_abslog_path([]))
        self.assertIsNone(_abslog_path(None))


class UnattendedFlagsTest(unittest.TestCase):
    """Launch-flag policy for unattended operation. The negative assertions are the
    load-bearing ones: they pin deliberate decisions that are easy to 'improve' back
    into a regression."""

    def test_unattended_flags_visible_declines_recovery_without_going_unattended(self):
        cmd = build_editor_command("Editor.exe", "P.uproject", mode="visible", extra_args=None)
        self.assertIn("-AutoDeclinePackageRecovery", cmd)
        # -unattended is process-global with no runtime off switch and strips every
        # confirmation prompt from a human sharing the window; on its own it also makes
        # PromptForCheckoutAndSave return PR_Cancelled and save NOTHING. Neither switch
        # may reach a visible editor unasked - not because of the save path (the windowless
        # set repairs that by pairing them), but because a person may be at that window.
        self.assertNotIn("-unattended", cmd)
        self.assertNotIn("-RunningUnattendedScript", cmd)

    def test_unattended_flags_headless_has_both_and_no_duplicate_decline(self):
        cmd = build_editor_command("Editor.exe", "P.uproject", mode="offscreen", extra_args=None)
        self.assertIn("-AutoDeclinePackageRecovery", cmd)
        self.assertIn("-unattended", cmd)
        self.assertIn("-RunningUnattendedScript", cmd)
        # The always-list and the headless list must not both contribute the flag.
        self.assertEqual(cmd.count("-AutoDeclinePackageRecovery"), 1)

    def test_unattended_flags_script_switch_is_opt_in_on_the_visible_path(self):
        # Opt-in applies to a VISIBLE launch only. mode="offscreen" carries it unconditionally;
        # that is pinned by the never-ships-alone invariant above.
        default = build_editor_command("Editor.exe", "P.uproject", mode="visible", extra_args=None)
        self.assertNotIn("-RunningUnattendedScript", default)

        opted_in = build_editor_command("Editor.exe", "P.uproject", mode="visible",
                                        extra_args=None, unattended_script=True)
        self.assertIn("-RunningUnattendedScript", opted_in)
        self.assertIn("-AutoDeclinePackageRecovery", opted_in)
        # Still not the process-global switch: -RunningUnattendedScript only sets
        # GIsRunningUnattendedScript, which PromptForCheckoutAndSave reads FIRST and
        # answers with a real save.
        self.assertNotIn("-unattended", opted_in)

    def test_unattended_flags_unattended_never_ships_without_the_script_switch(self):
        # THE invariant, and the one this class exists to keep. -unattended alone is the worst
        # of the three reachable states, on BOTH engine paths it touches:
        #   modals - FSlateApplication::AddModalWindow consults GIsRunningUnattendedScript and
        #     nothing else (SlateApplication.cpp:2134); -unattended sets only
        #     FApp::IsUnattended(), so the modal opens and owns the game thread.
        #   saving - FEditorFileUtils::PromptForCheckoutAndSave answers the two flags in the
        #     opposite order and the opposite way: GIsRunningUnattendedScript short-circuits to
        #     a real SavePackages (FileHelpers.cpp:4659), FApp::IsUnattended() four lines later
        #     returns PR_Cancelled and saves NOTHING (:4664). The first branch wins when both
        #     are set.
        # So the two switches must travel together on every argv this function can produce.
        # Counterfactual: drop -RunningUnattendedScript from _OFFSCREEN_FLAGS and the
        # mode="offscreen", unattended_script=False case fails.
        for mode in mcp_proxy.LAUNCH_MODES:
            for script in (True, False):
                cmd = build_editor_command("Editor.exe", "P.uproject", mode=mode,
                                           extra_args=None, unattended_script=script)
                if "-unattended" in cmd:
                    self.assertIn(
                        "-RunningUnattendedScript", cmd,
                        "mode=%s unattended_script=%s assembled -unattended alone"
                        % (mode, script))

    def test_unattended_flags_script_switch_is_never_duplicated(self):
        # The headless set and the opt-in must not both contribute it.
        cmd = build_editor_command("Editor.exe", "P.uproject", mode="offscreen",
                                   extra_args=None, unattended_script=True)
        self.assertEqual(cmd.count("-RunningUnattendedScript"), 1)

    def test_unattended_flags_extra_args_still_land_last(self):
        cmd = build_editor_command("Editor.exe", "P.uproject", mode="offscreen",
                                   extra_args=["-Abslog=C:/x.log"], unattended_script=True)
        self.assertEqual(cmd[-1], "-Abslog=C:/x.log")

    def test_unattended_flags_old_four_positional_signature_still_works(self):
        # patch_robustness's call sites pass four positional args; the new parameter is
        # keyword-defaulted precisely so they keep working.
        cmd = build_editor_command("Editor.exe", "P.uproject", "visible", ["-windowed"])
        self.assertEqual(cmd[0:2], ["Editor.exe", "P.uproject"])
        self.assertIn("-AutoDeclinePackageRecovery", cmd)
        self.assertEqual(cmd[-1], "-windowed")


class BlockedOnModalProbeTest(unittest.TestCase):
    """EDITOR_BLOCKED_ON_MODAL must be terminal everywhere the proxy polls. Without
    this the proxy spins on a non-retryable condition until start_timeout, reproducing
    the original hang one layer up."""

    def _proxy(self):
        return Proxy(
            "http://127.0.0.1:19880/mcp",
            list_timeout=0.1,
            call_timeout=0.1,
            probe_timeout=0.1,
            token_file=None,
            port_file=None,
            editor_exe=None,
            start_timeout=0.01,
            uproject=None,
        )

    @staticmethod
    def _blocked_ping():
        return {
            "jsonrpc": "2.0",
            "id": "_proxy_probe",
            "result": {
                "editorReady": False,
                "retryable": False,
                "error": "EDITOR_BLOCKED_ON_MODAL",
                "message": 'blocked on "Restore Packages" for 42 s',
                "blockedOnModal": True,
                "blockedSeconds": 42.1,
                "modalTitle": "Restore Packages",
            },
        }

    def test_unattended_flags_probe_reports_blocked_state(self):
        proxy = self._proxy()
        with mock.patch.object(proxy, "_post", return_value=self._blocked_ping()):
            state, detail = proxy._probe_state(proxy.url)
        self.assertEqual(state, "blocked_on_modal")
        self.assertIn("Restore Packages", detail)

    def test_unattended_flags_blocked_is_not_mistaken_for_not_ready(self):
        # The same reply carries editorReady:false. Falling into not_ready would make the
        # proxy poll a condition that can never clear on its own.
        proxy = self._proxy()
        with mock.patch.object(proxy, "_post", return_value=self._blocked_ping()):
            state, _ = proxy._probe_state(proxy.url)
        self.assertNotEqual(state, "not_ready")

    def test_unattended_flags_blocked_result_is_not_retryable(self):
        proxy = self._proxy()
        result = proxy._editor_unavailable_result("EDITOR_BLOCKED_ON_MODAL")
        self.assertTrue(result["isError"])
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_BLOCKED_ON_MODAL")
        self.assertFalse(result["structuredContent"]["retryable"])

    def test_unattended_flags_start_guard_blocks_a_second_editor(self):
        proxy = self._proxy()
        with mock.patch.object(proxy, "_probe_state",
                               return_value=("blocked_on_modal", "blocked on a dialog")), \
                mock.patch("mcp_proxy.subprocess.Popen",
                           side_effect=AssertionError("must not spawn a second editor")):
            guard = proxy._editor_process_guard()
        self.assertIsNotNone(guard)
        self.assertEqual(guard["structuredContent"]["error"], "EDITOR_BLOCKED_ON_MODAL")


class ClientIdentityHeaderTest(unittest.TestCase):
    """Every request must carry this proxy process's client id.

    It is what lets the editor tell one MCP session's traffic from another's, which is the whole
    basis of editor.quit's EDITOR_IN_USE guard. A request that reaches the editor without the
    header lands in the shared anonymous bucket, where the guard cannot protect it.
    """

    URL = "http://127.0.0.1:9/mcp"

    def _proxy(self):
        return Proxy(
            self.URL,
            list_timeout=0.1,
            call_timeout=0.1,
            probe_timeout=0.1,
            token_file=None,
            port_file=None,
            editor_exe=None,
            start_timeout=0.01,
            uproject=None,
        )

    def test_client_id_is_opaque_and_stable_within_the_process(self):
        # Stability is load-bearing: an id that changed per call would make every request look
        # like a new client, and the editor would then read this proxy's own traffic as foreign.
        self.assertEqual(mcp_proxy._CLIENT_ID, mcp_proxy._CLIENT_ID)
        self.assertRegex(mcp_proxy._CLIENT_ID, r"^[0-9a-f]{32}$")

    def test_post_sends_the_client_header(self):
        proxy = self._proxy()
        captured = {}

        class _Resp:
            def read(self):
                return b"{}"

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

        def _urlopen(req, timeout=None):
            captured["headers"] = dict(req.headers)
            return _Resp()

        with mock.patch("urllib.request.urlopen", _urlopen):
            proxy._post({"jsonrpc": "2.0", "id": 1}, self.URL, 0.1)

        # urllib title-cases header names as it stores them.
        self.assertEqual(captured["headers"].get("X-pinwright-client"), mcp_proxy._CLIENT_ID)

    def test_post_stream_sends_the_client_header(self):
        proxy = self._proxy()
        captured = {}

        class _Resp:
            def getheader(self, _name):
                return "application/json"

            def read(self):
                return b"{}"

        class _Conn:
            def __init__(self, *_args, **_kwargs):
                pass

            def request(self, _method, _path, body=None, headers=None):
                captured["headers"] = headers

            def getresponse(self):
                return _Resp()

            def close(self):
                pass

        with mock.patch("http.client.HTTPConnection", _Conn):
            proxy._post_stream({"jsonrpc": "2.0", "id": 1}, self.URL)

        self.assertEqual(captured["headers"].get("X-PinWright-Client"), mcp_proxy._CLIENT_ID)


if __name__ == "__main__":
    unittest.main()
