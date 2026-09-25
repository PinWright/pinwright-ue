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
from mcp_proxy import (
    EDITOR_PREPARE_TESTS_TOOL,
    EDITOR_RESTART_TOOL,
    EDITOR_START_TOOL,
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


def _same(a, b):
    """Path equality tolerant of slash/case differences on the running platform."""
    if a is None or b is None:
        return a is b
    return os.path.normcase(os.path.normpath(a)) == os.path.normcase(os.path.normpath(b))


class BuildEditorCommandTest(unittest.TestCase):
    def test_visible_gets_only_the_always_flags(self):
        cmd = build_editor_command("Editor.exe", "P.uproject", visible=True, extra_args=[])
        self.assertEqual(cmd, ["Editor.exe", "P.uproject", "-AutoDeclinePackageRecovery"])

    def test_invisible_has_headless_flags_and_no_nullrhi(self):
        cmd = build_editor_command("Editor.exe", "P.uproject", visible=False, extra_args=[])
        for flag in ("-RenderOffScreen", "-unattended", "-RunningUnattendedScript", "-nopause",
                     "-nosplash", "-nocefaccelpaint"):
            self.assertIn(flag, cmd)
        # Deliberately never -NullRHI (some paths need a real RHI).
        self.assertNotIn("-NullRHI", cmd)
        self.assertNotIn("-nullrhi", cmd)

    def test_extra_args_appended_verbatim(self):
        cmd = build_editor_command("Editor.exe", "P.uproject", visible=True,
                                   extra_args=["-windowed", "-resx=1280"])
        self.assertEqual(cmd[-2:], ["-windowed", "-resx=1280"])

    def test_none_extra_args_tolerated(self):
        cmd = build_editor_command("Editor.exe", "P.uproject", visible=True, extra_args=None)
        self.assertEqual(cmd, ["Editor.exe", "P.uproject", "-AutoDeclinePackageRecovery"])

    # The startup map is read by FParse::Token as the FIRST token of the remaining command line
    # and skipped outright if that token starts with '-' (UnrealEdMisc.cpp:396-399). Index 2 -
    # immediately after the .uproject, before every switch - is therefore the only position that
    # works. Counterfactual: append the map anywhere after _ALWAYS_FLAGS and this fails.
    def test_map_is_the_first_token_after_the_uproject(self):
        cmd = build_editor_command("Editor.exe", "P.uproject", visible=False,
                                   extra_args=["-windowed"], unattended_script=True,
                                   map_name="/Game/Maps/MyLevel")
        self.assertEqual(cmd[:3], ["Editor.exe", "P.uproject", "/Game/Maps/MyLevel"])
        self.assertEqual(cmd[3], "-AutoDeclinePackageRecovery")
        self.assertEqual(cmd[-1], "-windowed")

    def test_no_map_leaves_the_command_line_byte_identical(self):
        self.assertEqual(
            build_editor_command("Editor.exe", "P.uproject", visible=True, extra_args=[],
                                 map_name=None),
            build_editor_command("Editor.exe", "P.uproject", visible=True, extra_args=[]))


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
                start = proxy._editor_start({})
                prepare = proxy._editor_prepare_tests({"filter": "X"})
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
    """Pin the launch path of platforms with a .uproject open action and a display, and hide any
    operator engine override, so these tests read the same on every host. Linux's direct-spawn
    path is covered by LinuxLaunchTest."""
    for name, value in (("_association_open_supported", True), ("_visible_launch_env", {})):
        patcher = mock.patch("mcp_proxy." + name, return_value=value)
        patcher.start()
        test.addCleanup(patcher.stop)
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

    def test_default_uses_registered_open_after_resolving_the_project_engine(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project)
            events = []

            def probe(_url):
                events.append("ping")
                return ("not_running", "connection refused") if len(events) == 1 else (
                    "alive", None)

            def shell_open(path):
                events.append(("open", path))

            with mock.patch.object(proxy, "_probe_state", side_effect=probe), \
                    mock.patch("mcp_proxy._open_uproject", side_effect=shell_open), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=("C:\\UE_5.8", "Editor.exe")) as resolve, \
                    mock.patch("mcp_proxy.build_editor_command",
                               side_effect=AssertionError("must not build direct command")), \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               side_effect=AssertionError("must not spawn direct process")):                result = proxy._editor_start({})

            # The association mode resolves the engine too, so it cannot diverge from the
            # direct-spawn mode: the project's EngineAssociation is the only input.
            self.assertEqual(resolve.call_args.args[3], "5.8")
            self.assertFalse(result["isError"])
            self.assertEqual(events[0], "ping")
            self.assertEqual(events[1], ("open", os.path.normpath(project)))
            self.assertEqual(events[2], "ping")
            self.assertEqual(result["structuredContent"]["association"], "open")
            self.assertEqual(result["structuredContent"]["engineRoot"], "C:\\UE_5.8")
            self.assertNotIn("pid", result["structuredContent"])

    def test_unresolvable_association_fails_loudly_and_launches_nothing(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project)
            with mock.patch.object(
                    proxy, "_probe_state", return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy._association_engine_roots", return_value=[]), \
                    mock.patch("mcp_proxy._open_uproject",
                               side_effect=AssertionError("must not open the project")), \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               side_effect=AssertionError("must not spawn any editor")):
                result = proxy._editor_start({})
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
                result = proxy._editor_start({
                    "extra_args": ["-relay=tcp://127.0.0.1:7788"],
                })
            self.assertEqual(resolved.call_args.args[0], "5.8")
            self.assertTrue(_same(popen.call_args.args[0][0], _editor_exe_under(engine_root)))
            self.assertFalse(result["isError"])

    def test_shell_dispatch_failure_is_clear(self):
        with tempfile.TemporaryDirectory() as temp:
            proxy = self._proxy(uproject=self._project(temp))
            with mock.patch.object(
                    proxy, "_probe_state", return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=("C:\\UE_5.8", "Editor.exe")), \
                    mock.patch("mcp_proxy._open_uproject",
                               side_effect=OSError("association unavailable")):
                result = proxy._editor_start({})
            self.assertTrue(result["isError"])
            self.assertEqual(
                result["structuredContent"]["error"], "EDITOR_PROJECT_OPEN_FAILED"
            )

    def test_every_wait_mode_rejects_live_editor_before_resolution(self):
        for wait in ("ready", "exit", "invalid"):
            with self.subTest(wait=wait):
                proxy = self._proxy(uproject="missing.uproject")
                with mock.patch.object(
                        proxy, "_probe_state", return_value=("alive", None)), \
                        mock.patch("mcp_proxy.resolve_uproject",
                                   side_effect=AssertionError("project resolved")):
                    result = proxy._editor_start({"wait": wait})
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
            result = proxy._editor_start({"wait": "exit"})
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
                    result = proxy._editor_start({"wait": wait})
                self.assertTrue(result["isError"])
                self.assertEqual(
                    result["structuredContent"]["error"], "EDITOR_PLUGIN_OUTDATED")
                self.assertFalse(result["structuredContent"]["retryable"])
                self.assertEqual(result["structuredContent"]["url"], self.URL)

    def test_direct_headless_exit_uses_unbounded_wait(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project)
            process = _CompletedProcess()
            with mock.patch.object(
                    proxy, "_probe_state", return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=("C:\\UE_5.8", "Editor.exe")), \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               return_value=process) as popen:
                result = proxy._editor_start({
                    "visible": False,
                    "wait": "exit",
                    "extra_args": ["-Abslog=C:/logs/test.log"],
                })
            command = popen.call_args.args[0]
            self.assertIn("-RenderOffScreen", command)
            self.assertIn("-Abslog=C:/logs/test.log", command)
            self.assertEqual(process.wait_calls, [((), {})])
            self.assertFalse(result["isError"])

    def test_extra_args_force_direct_ready_mode(self):
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
                    mock.patch("mcp_proxy.subprocess.Popen",
                               return_value=process) as popen:
                result = proxy._editor_start({"extra_args": ["-windowed"]})
            self.assertEqual(popen.call_args.args[0][-1], "-windowed")
            self.assertFalse(result["isError"])
            self.assertEqual(result["structuredContent"]["pid"], process.pid)

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
                result = proxy._editor_start({
                    "extra_args": ["-Abslog=C:/logs/early-exit.log"],
                })
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
                result = proxy._editor_start({
                    "extra_args": ["-Abslog=C:/logs/ready-timeout.log"],
                })
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_START_TIMEOUT")
        self.assertEqual(
            result["structuredContent"]["logPath"], "C:/logs/ready-timeout.log"
        )

    def test_association_readiness_timeout_reports_project_without_pid(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project, start_timeout=0.0)
            with mock.patch.object(
                    proxy, "_probe_state", return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=("C:\\UE_5.8", "Editor.exe")), \
                    mock.patch("mcp_proxy._open_uproject"):
                result = proxy._editor_start({})
            self.assertEqual(result["structuredContent"]["error"], "EDITOR_START_TIMEOUT")
            self.assertEqual(result["structuredContent"]["uproject"],
                             os.path.normpath(project))
            self.assertEqual(result["structuredContent"]["engineRoot"], "C:\\UE_5.8")
            self.assertNotIn("pid", result["structuredContent"])

    def test_interruption_cleans_up_only_owned_direct_child(self):
        process = _InterruptedProcess()
        proxy = self._proxy()
        proxy._track_owned_child(process)
        result = proxy._wait_for_exit(process, "Editor.exe P.uproject", [])
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_RUN_INTERRUPTED")
        self.assertTrue(result["structuredContent"]["cleanupSucceeded"])
        self.assertTrue(process.terminated)
        self.assertEqual(process.wait_calls[0], ((), {}))
        self.assertEqual(process.wait_calls[1], ((), {"timeout": 5.0}))

    # ---- start-into-map -----------------------------------------------------
    #
    # These drive the real _editor_start handler with only Popen mocked, so the argv
    # asserted below is the one the real build_editor_command produced.

    def _spawn_argv(self, args):
        """Run the real _editor_start against a fake dead endpoint and return the spawned argv."""
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project, start_timeout=0.0)
            with mock.patch.object(
                    proxy, "_probe_state", return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=("C:\\UE_5.8", "Editor.exe")), \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               return_value=_BlockedProcess()) as popen:
                proxy._editor_start(args)
            return popen.call_args.args[0]

    def test_map_lands_immediately_after_the_uproject_on_the_real_launch(self):
        command = self._spawn_argv({"map": "/Game/Maps/Blockout"})
        self.assertEqual(command[2], "/Game/Maps/Blockout")
        self.assertEqual(command[3], "-AutoDeclinePackageRecovery")

    def test_map_forces_a_direct_spawn_instead_of_the_os_association(self):
        # The OS "open" verb carries no command line, so a map can only be delivered by a
        # direct spawn. Counterfactual: leave `start_map is None` out of the association_mode
        # predicate and _open_uproject runs while build_editor_command never does.
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project, start_timeout=0.0)
            with mock.patch.object(
                    proxy, "_probe_state", return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=("C:\\UE_5.8", "Editor.exe")), \
                    mock.patch("mcp_proxy._open_uproject",
                               side_effect=AssertionError("must not use the OS association")), \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               return_value=_BlockedProcess()) as popen:
                proxy._editor_start({"map": "/Game/Maps/MyLevel"})
            self.assertIn("/Game/Maps/MyLevel", popen.call_args.args[0])

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
        # human must not be sharing the window" - and visible:false HAS no window. What it does
        # have is -unattended, which sets FApp::IsUnattended() but leaves
        # GIsRunningUnattendedScript false - and GIsRunningUnattendedScript is the only global
        # FSlateApplication::AddModalWindow consults (SlateApplication.cpp:2134). So the old
        # behaviour shipped the one state the engine handles worst: a startup modal owns the
        # game thread forever, in a process with no window in which anyone could see or clear
        # it. a proxy-owned automation launch already hit exactly this wedge (25 minutes, zero tests, 0.8% CPU).
        command = self._spawn_argv({"visible": False, "wait": "exit"})
        self.assertIn("-unattended", command)
        self.assertIn("-RunningUnattendedScript", command)

    def test_visible_launch_without_map_still_has_neither_unattended_switch(self):
        # The half of the old rationale that is untouched: a visible editor can be handed to a
        # person, so nothing auto-answers its dialogs unless the caller asks (unattended_script)
        # or the call shape implies an agent-driven boot (map).
        command = self._spawn_argv({"visible": True, "wait": "exit"})
        self.assertNotIn("-RunningUnattendedScript", command)
        self.assertNotIn("-unattended", command)

    def test_windowless_opt_out_cannot_strip_the_switch_off_a_windowless_launch(self):
        # Deliberate compatibility break, and the reason the flag belongs in _HEADLESS_FLAGS
        # rather than in the unattended_script default: unattended_script:false must not be able
        # to assemble -unattended WITHOUT -RunningUnattendedScript. That combination wedges on a
        # startup modal AND makes FEditorFileUtils::PromptForCheckoutAndSave return PR_Cancelled
        # and save nothing (FileHelpers.cpp:4664). Opting out of modal suppression is only
        # meaningful where a modal can be answered, i.e. visible:true.
        command = self._spawn_argv({"visible": False, "wait": "exit",
                                    "unattended_script": False})
        self.assertIn("-unattended", command)
        self.assertIn("-RunningUnattendedScript", command)


    def test_invalid_map_is_rejected_before_anything_is_spawned(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project)
            with mock.patch.object(
                    proxy, "_probe_state", return_value=("not_running", "refused")), \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               side_effect=AssertionError("must not spawn")), \
                    mock.patch("mcp_proxy._open_uproject",
                               side_effect=AssertionError("must not open")):
                result = proxy._editor_start({"map": "-NotAMap"})
        self.assertTrue(result["isError"])
        self.assertEqual(result["structuredContent"]["error"], "INVALID_MAP")

    def test_map_schema_is_advertised(self):
        self.assertIn("map", EDITOR_START_TOOL["inputSchema"]["properties"])


class LinuxLaunchTest(unittest.TestCase):
    """Linux has no dependable .uproject open action, and a proxy started over SSH has no
    DISPLAY, so a visible launch spawns the resolved editor with the desktop session's display."""

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
            with mock.patch("mcp_proxy._association_open_supported", return_value=False), \
                    mock.patch("mcp_proxy._visible_launch_env", **display_env) as display, \
                    mock.patch.object(proxy, "_probe_state",
                                      side_effect=lambda _url: next(probes)), \
                    mock.patch.object(proxy, "_resolve_url", return_value="http://x/mcp"), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=(self.ENGINE_ROOT, self.EXE)), \
                    mock.patch("mcp_proxy._open_uproject",
                               side_effect=AssertionError("must not use xdg-open")), \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               return_value=process or _CompletedProcess(code=None)) as popen:
                result = proxy._editor_start(args)
            return result, popen, display, os.path.normpath(project)

    def test_default_start_spawns_the_resolved_editor_with_the_session_display(self):
        result, popen, _display, project = self._start({}, {"return_value": self.SESSION})
        self.assertFalse(result["isError"])
        self.assertEqual(popen.call_args.args[0],
                         [self.EXE, project, "-AutoDeclinePackageRecovery"])
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
            {"visible": False, "wait": "exit"},
            {"side_effect": AssertionError("must not look for a display")},
            process=_CompletedProcess(code=0))
        display.assert_not_called()
        self.assertNotIn("env", popen.call_args.kwargs)
        self.assertIn("-RenderOffScreen", popen.call_args.args[0])


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
        with mock.patch("mcp_proxy._association_open_supported", return_value=False), \
                mock.patch.object(proxy, "_probe_state", side_effect=lambda _url: next(probes)), \
                mock.patch.object(proxy, "_resolve_url", return_value="http://x/mcp"), \
                mock.patch("mcp_proxy.resolve_editor", return_value=("root", sys.executable)), \
                mock.patch("mcp_proxy.build_editor_command", return_value=child):
            result = proxy._editor_start({"visible": False})
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
        for key in ("map", "visible", "save", "discard", "extra_args", "unattended_script"):
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
                {"map": "/Game/Maps/X", "visible": False, "save": True})
        quit_rpc.assert_called_once_with(self.URL, True, False)
        # save/discard are quit-half arguments and must not leak into the launch.
        start.assert_called_once_with({"map": "/Game/Maps/X", "visible": False})
        self.assertTrue(result["structuredContent"]["restarted"])
        self.assertTrue(result["structuredContent"]["stoppedPreviousEditor"])

    def test_no_running_editor_skips_the_quit_half(self):
        proxy = self._proxy()
        started = {"content": [], "structuredContent": {"success": True}, "isError": False}
        with mock.patch.object(proxy, "_probe_state", return_value=("not_running", "refused")), \
                mock.patch.object(proxy, "_request_editor_quit",
                                  side_effect=AssertionError("must not quit a dead editor")), \
                mock.patch.object(proxy, "_editor_start", return_value=started):
            result = proxy._editor_restart({"map": "/Game/Maps/X"})
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
            result = proxy._editor_restart({})
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
            result = proxy._editor_restart({})
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_BLOCKED_ON_MODAL")

    def test_a_stuck_shutdown_times_out_without_killing_or_respawning(self):
        proxy = self._proxy()
        with mock.patch.object(proxy, "_probe_state", return_value=("alive", None)), \
                mock.patch.object(proxy, "_request_editor_quit", return_value=None), \
                mock.patch.object(proxy, "_wait_for_endpoint_down", return_value=False), \
                mock.patch.object(proxy, "_editor_start",
                                  side_effect=AssertionError("must not start a second editor")):
            result = proxy._editor_restart({})
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_STOP_TIMEOUT")

    def test_save_and_discard_together_are_rejected(self):
        proxy = self._proxy()
        with mock.patch.object(proxy, "_probe_state",
                               side_effect=AssertionError("must not probe")):
            result = proxy._editor_restart({"save": True, "discard": True})
        self.assertEqual(result["structuredContent"]["error"], "INVALID_ARGUMENTS")

    def test_a_bad_map_is_rejected_before_the_editor_is_stopped(self):
        # Validating the map first means a typo costs nothing; validating it inside the start
        # half would leave the caller with no editor at all.
        proxy = self._proxy()
        with mock.patch.object(proxy, "_probe_state",
                               side_effect=AssertionError("must not probe")):
            result = proxy._editor_restart({"map": "-NotAMap"})
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

    def test_eof_cleans_blocked_owned_direct_editor_without_writing_response(self):
        with tempfile.TemporaryDirectory() as temp:
            proxy = self._proxy(self._project(temp))
            process = _BlockingProcess()
            input_stream = _ControlledInput()
            output_stream = _CapturedOutput()
            with mock.patch(
                    "mcp_proxy.resolve_editor",
                    return_value=("C:\\UE_5.8", "Editor.exe"),
                ), mock.patch(
                    "mcp_proxy.subprocess.Popen", return_value=process
                ) as popen:
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
                        "arguments": {"visible": False, "wait": "exit"},
                    },
                })
                self.assertTrue(process.started.wait(2.0))
                input_stream.close()
                server.join(2.0)

        self.assertFalse(server.is_alive())
        self.assertTrue(proxy.shutdown_requested())
        self.assertTrue(process.terminated)
        self.assertEqual(process.wait_calls[0], ((), {}))
        self.assertIn(((), {"timeout": 5.0}), process.wait_calls)
        self.assertEqual(output_stream.getvalue(), "")
        popen.assert_called_once()

    def test_eof_never_owns_or_terminates_shell_association_editor(self):
        with tempfile.TemporaryDirectory() as temp:
            proxy = self._proxy(self._project(temp))
            input_stream = _ControlledInput()
            output_stream = _CapturedOutput()
            opened = threading.Event()
            with mock.patch(
                    "mcp_proxy.resolve_editor", return_value=("C:\\UE_5.8", "Editor.exe")
                ), mock.patch(
                    "mcp_proxy._open_uproject", side_effect=lambda _path: opened.set()
                ), mock.patch(
                    "mcp_proxy.subprocess.Popen",
                    side_effect=AssertionError("association launch used Popen"),
                ), mock.patch.object(
                    proxy,
                    "_cleanup_child",
                    side_effect=AssertionError("association editor was treated as owned"),
                ):
                server = threading.Thread(
                    target=serve_stdio, args=(proxy, input_stream, output_stream)
                )
                server.start()
                input_stream.send({
                    "jsonrpc": "2.0",
                    "id": 8,
                    "method": "tools/call",
                    "params": {"name": "editor_start", "arguments": {}},
                })
                self.assertTrue(opened.wait(2.0))
                input_stream.close()
                server.join(2.0)

        self.assertFalse(server.is_alive())
        self.assertTrue(proxy.shutdown_requested())
        self.assertEqual(output_stream.getvalue(), "")

    def test_child_spawn_race_after_eof_is_cleaned_without_becoming_owned(self):
        proxy = self._proxy()
        process = _BlockingProcess()
        self.assertTrue(proxy.request_shutdown())
        with self.assertRaisesRegex(RuntimeError, "shutting down"):
            proxy._track_owned_child(process)
        self.assertTrue(process.terminated)
        self.assertTrue(proxy._cleanup_owned_child())


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


class ProxyEditorPrepareTestsTest(unittest.TestCase):
    URL = "http://127.0.0.1:19880/mcp"

    def _proxy(self, uproject=None, url=None):
        return Proxy(
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

    def _prepare(self, proxy, test_filter="PinWright",
                 probe=("not_running", "connection refused")):
        engine_root, editor_exe = self._engine(proxy.uproject)
        with mock.patch.object(proxy, "_probe_state", return_value=probe), \
                mock.patch("mcp_proxy.resolve_editor",
                           return_value=(engine_root, editor_exe)), \
                mock.patch("mcp_proxy.os.path.isfile", return_value=True), \
                mock.patch("mcp_proxy.subprocess.Popen",
                           side_effect=AssertionError("must not spawn Unreal")) as popen, \
                mock.patch.object(proxy, "_wait_for_ready",
                                  side_effect=AssertionError("must not wait")), \
                mock.patch.object(proxy, "_wait_for_exit",
                                  side_effect=AssertionError("must not wait")):
            result = proxy._editor_prepare_tests({"filter": test_filter})
        return result, popen, engine_root, editor_exe

    def test_tool_schema_requires_only_filter(self):
        schema = EDITOR_PREPARE_TESTS_TOOL["inputSchema"]
        self.assertEqual(schema["required"], ["filter"])
        self.assertEqual(set(schema["properties"]), {"filter"})
        self.assertFalse(schema["additionalProperties"])

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
            ["call", "editor_start", "editor_restart", "editor_prepare_tests"],
        )

        expected = {
            "content": [],
            "structuredContent": {"status": "COMMAND_READY"},
            "isError": False,
        }
        with mock.patch.object(proxy, "_editor_prepare_tests",
                               return_value=expected) as prepare:
            response = proxy.handle({
                "jsonrpc": "2.0",
                "id": 2,
                "method": "tools/call",
                "params": {
                    "name": "editor_prepare_tests",
                    "arguments": {"filter": "X"},
                },
            })
        prepare.assert_called_once_with({"filter": "X"})
        self.assertEqual(response["result"], expected)

    def test_guard_runs_before_filter_validation_or_project_path(self):
        proxy = self._proxy(uproject="missing.uproject", url=self.URL)
        with mock.patch.object(proxy, "_probe_state",
                               return_value=("alive", None)), \
                mock.patch.object(proxy, "_validate_test_arguments",
                                   side_effect=AssertionError("validated too early")), \
                mock.patch("mcp_proxy.resolve_uproject",
                           side_effect=AssertionError("resolved project too early")):
            result = proxy._editor_prepare_tests(None)
        self.assertTrue(result["isError"])
        structured = result["structuredContent"]
        self.assertEqual(structured["error"], "EDITOR_ALREADY_RUNNING")
        self.assertEqual(structured["editorGuard"]["state"], "alive")
        self.assertEqual(structured["editorGuard"]["status"], "unavailable")

    def test_not_probed_observation_is_explicit_and_successful(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project, url=None)
            result, popen, _root, _editor = self._prepare(proxy)
        self.assertFalse(result["isError"])
        structured = result["structuredContent"]
        self.assertEqual(structured["status"], "COMMAND_READY")
        self.assertEqual(structured["editorGuard"]["status"], "not_probed")
        self.assertEqual(structured["editorGuard"]["state"], "not_probed")
        self.assertEqual(popen.call_count, 0)

    def test_filter_validation_rejects_missing_unknown_empty_control_and_separator(self):
        invalid = (
            None,
            {},
            {"filter": ""},
            {"filter": " \t "},
            {"filter": "A\nB"},
            {"filter": "A\u0085B"},
            {"filter": "A,B"},
            {"filter": "A;B"},
            {"filter": "X", "extra_args": []},
            {"filter": 7},
        )
        with tempfile.TemporaryDirectory() as temp:
            proxy = self._proxy(uproject=self._project(temp), url=self.URL)
            with mock.patch.object(proxy, "_probe_state",
                                   return_value=("not_running", "connection refused")):
                for arguments in invalid:
                    with self.subTest(arguments=arguments):
                        result = proxy._editor_prepare_tests(arguments)
                        self.assertEqual(
                            result["structuredContent"]["error"], "INVALID_FILTER")

    def test_command_ready_contains_the_load_bearing_launch_contract(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project, url=self.URL)
            result, popen, engine_root, editor_exe = self._prepare(proxy)
        self.assertFalse(result["isError"])
        structured = result["structuredContent"]
        self.assertEqual(structured["status"], "COMMAND_READY")
        self.assertEqual(structured["filter"], "PinWright")
        self.assertEqual(structured["uproject"], os.path.abspath(project))
        self.assertEqual(structured["engineAssociation"], "5.8")
        self.assertEqual(structured["engineRoot"], os.path.abspath(engine_root))
        self.assertEqual(structured["editorGuard"]["status"], "clear")
        self.assertEqual(structured["editorGuard"]["state"], "not_running")
        self.assertEqual(popen.call_count, 0)

        launch = structured["launch"]
        self.assertEqual(launch["executable"],
                         os.path.abspath(_editor_cmd_from_editor(editor_exe)))
        self.assertEqual(launch["argv"][0], os.path.abspath(project))
        exec_cmd = next(arg for arg in launch["argv"]
                        if arg.startswith("-ExecCmds="))
        self.assertEqual(exec_cmd, "-ExecCmds=Automation RunTests PinWright,Quit")
        self.assertNotIn(";Quit", exec_cmd)
        self.assertIn("-TestExit=Automation Test Queue Empty", launch["argv"])
        for flag in (
                "-unattended",
                "-nopause",
                "-nosplash",
                "-nosound",
                "-RenderOffscreen",
                "-nocefaccelpaint",
                "-RunningUnattendedScript",
                # No ZenLocal store in the cache graph, so nothing at startup builds an
                # autolaunching FZenServiceInstance and the run does not pay the
                # spawn-and-wait-for-health loop (23.5 s measured) before the first test.
                "-ddc=InstalledNoZenLocalFallback",
        ):
            self.assertIn(flag, launch["argv"])
        self.assertFalse(any("nullrhi" in arg.lower() for arg in launch["argv"]))

        report_arg = next(arg for arg in launch["argv"]
                          if arg.startswith("-ReportExportPath="))
        self.assertTrue(os.path.isabs(report_arg.split("=", 1)[1]))
        abslog = next(arg for arg in launch["argv"]
                      if arg.startswith("-Abslog="))
        self.assertEqual(abslog.split("=", 1)[1], structured["logPath"])
        self.assertTrue(os.path.isabs(structured["logPath"]))
        self.assertEqual(launch["logPath"], structured["logPath"])

        checker = structured["checker"]
        checker_script = os.path.abspath(os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "check_suite_log.py",
        ))
        self.assertEqual(checker["executable"], os.path.abspath(sys.executable))
        self.assertEqual(checker["script"], checker_script)
        self.assertEqual(checker["argv"], [checker_script, structured["logPath"]])
        self.assertEqual(checker["logPath"], structured["logPath"])

        for key in (
                "success",
                "pid",
                "exitCode",
                "counts",
                "verdict",
                "result",
                "timeoutSeconds",
        ):
            self.assertNotIn(key, structured)

    def test_engine_association_selects_the_commandlet_without_spawning(self):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project, url=self.URL)
            engine_root, editor_exe = self._engine(project)
            with mock.patch.object(proxy, "_probe_state",
                                   return_value=("not_running", "connection refused")), \
                    mock.patch("mcp_proxy.resolve_editor",
                               return_value=(engine_root, editor_exe)) as resolve, \
                    mock.patch("mcp_proxy.os.path.isfile", return_value=True), \
                    mock.patch("mcp_proxy.subprocess.Popen",
                               side_effect=AssertionError("must not spawn Unreal")):
                result = proxy._editor_prepare_tests({"filter": "PinWright"})
        self.assertFalse(result["isError"])
        self.assertEqual(resolve.call_args.args[3], "5.8")
        self.assertTrue(
            result["structuredContent"]["launch"]["executable"].endswith(
                "UnrealEditor-Cmd.exe"))

    def test_checker_uses_the_exact_launch_log_path(self):
        with tempfile.TemporaryDirectory() as temp:
            proxy = self._proxy(uproject=self._project(temp), url=self.URL)
            result, _popen, _root, _editor = self._prepare(proxy)
        structured = result["structuredContent"]
        self.assertEqual(structured["checker"]["argv"][-1], structured["logPath"])
        self.assertEqual(
            next(arg for arg in structured["launch"]["argv"]
                 if arg.startswith("-Abslog=")),
            "-Abslog=" + structured["logPath"],
        )

    def _prepare_with_cmd_twin_missing(self, os_name, editor_name):
        with tempfile.TemporaryDirectory() as temp:
            project = self._project(temp)
            proxy = self._proxy(uproject=project)
            root = os.path.join(temp, "UE_5.8")
            editor_exe = os.path.join(root, "Engine", "Binaries", "Linux", editor_name)
            with mock.patch.object(proxy, "_probe_state",
                                   return_value=("not_running", "connection refused")), \
                    mock.patch("mcp_proxy.resolve_editor", return_value=(root, editor_exe)), \
                    mock.patch("mcp_proxy.os.path.isfile",
                               side_effect=lambda path: "-Cmd" not in os.path.basename(path)), \
                    mock.patch.object(mcp_proxy.os, "name", os_name):
                result = proxy._editor_prepare_tests({"filter": "PinWright"})
        return result, editor_exe

    def test_non_windows_runs_the_editor_binary_when_there_is_no_cmd_twin(self):
        result, editor_exe = self._prepare_with_cmd_twin_missing("posix", "UnrealEditor")
        self.assertFalse(result["isError"])
        self.assertEqual(result["structuredContent"]["launch"]["executable"],
                         os.path.abspath(editor_exe))

    def test_windows_still_requires_the_cmd_twin(self):
        result, _editor_exe = self._prepare_with_cmd_twin_missing("nt", "UnrealEditor.exe")
        self.assertEqual(result["structuredContent"]["error"], "EDITOR_COMMAND_NOT_FOUND")


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
        cmd = build_editor_command("Editor.exe", "P.uproject", visible=True, extra_args=None)
        self.assertIn("-AutoDeclinePackageRecovery", cmd)
        # -unattended is process-global with no runtime off switch and strips every
        # confirmation prompt from a human sharing the window; on its own it also makes
        # PromptForCheckoutAndSave return PR_Cancelled and save NOTHING. Neither switch
        # may reach a visible editor unasked - not because of the save path (the windowless
        # set repairs that by pairing them), but because a person may be at that window.
        self.assertNotIn("-unattended", cmd)
        self.assertNotIn("-RunningUnattendedScript", cmd)

    def test_unattended_flags_headless_has_both_and_no_duplicate_decline(self):
        cmd = build_editor_command("Editor.exe", "P.uproject", visible=False, extra_args=None)
        self.assertIn("-AutoDeclinePackageRecovery", cmd)
        self.assertIn("-unattended", cmd)
        self.assertIn("-RunningUnattendedScript", cmd)
        # The always-list and the headless list must not both contribute the flag.
        self.assertEqual(cmd.count("-AutoDeclinePackageRecovery"), 1)

    def test_unattended_flags_script_switch_is_opt_in_on_the_visible_path(self):
        # Opt-in applies to a VISIBLE launch only. visible=False carries it unconditionally;
        # that is pinned by the never-ships-alone invariant above.
        default = build_editor_command("Editor.exe", "P.uproject", visible=True, extra_args=None)
        self.assertNotIn("-RunningUnattendedScript", default)

        opted_in = build_editor_command("Editor.exe", "P.uproject", visible=True,
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
        # Counterfactual: drop -RunningUnattendedScript from _HEADLESS_FLAGS and the
        # visible=False, unattended_script=False case fails.
        for visible in (True, False):
            for script in (True, False):
                cmd = build_editor_command("Editor.exe", "P.uproject", visible=visible,
                                           extra_args=None, unattended_script=script)
                if "-unattended" in cmd:
                    self.assertIn(
                        "-RunningUnattendedScript", cmd,
                        "visible=%s unattended_script=%s assembled -unattended alone"
                        % (visible, script))

    def test_unattended_flags_script_switch_is_never_duplicated(self):
        # The headless set and the opt-in must not both contribute it.
        cmd = build_editor_command("Editor.exe", "P.uproject", visible=False,
                                   extra_args=None, unattended_script=True)
        self.assertEqual(cmd.count("-RunningUnattendedScript"), 1)

    def test_unattended_flags_extra_args_still_land_last(self):
        cmd = build_editor_command("Editor.exe", "P.uproject", visible=False,
                                   extra_args=["-Abslog=C:/x.log"], unattended_script=True)
        self.assertEqual(cmd[-1], "-Abslog=C:/x.log")

    def test_unattended_flags_old_four_positional_signature_still_works(self):
        # patch_robustness's call sites pass four positional args; the new parameter is
        # keyword-defaulted precisely so they keep working.
        cmd = build_editor_command("Editor.exe", "P.uproject", True, ["-windowed"])
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
