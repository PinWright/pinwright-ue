# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Self-test for `check_unattended_flags.py`, the -unattended/-RunningUnattendedScript pairing scan.

WHY A SELF-TEST AND NOT JUST THE SCAN. A scan that can no longer fail prints CLEAN and exits 0
exactly like a clean tree, so the fixtures are a pair pulling in opposite directions:

  * a real violation -- an argv with `-unattended` and the usual windowless companions but no
    `-RunningUnattendedScript` -- must be flagged. That is the shape that wedged a cold CI editor
    for 13 minutes at ~0% CPU with no automation ever starting;
  * the sentences that describe that defect must NOT be. Every launch site in this repo sits beside
    a comment explaining the pairing, and a scan that could not tell an explanation from an
    invocation would need an opt-out on every paragraph.

Pure stdlib; no editor, no socket, no `uv`. Run under Unreal's bundled interpreter from
Content/Python:

    "%UE_ROOT%\\Engine\\Binaries\\ThirdParty\\Python3\\Win64\\python.exe" -m unittest discover tests

or explicitly:

    ...\\python.exe -m unittest tests.test_unattended_flag_scan
"""

import contextlib
import io
import os
import sys
import tempfile
import unittest

# check_unattended_flags.py lives one directory up (Content/Python/).
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import check_unattended_flags  # noqa: E402


class _Tree(object):
    """A throwaway command tree. Files are written under a tempdir, never inside the plugin."""

    def __init__(self, files):
        self._temp = tempfile.TemporaryDirectory(prefix="pinwright-unattended-scan-")
        self.root = self._temp.name
        for relative, text in files.items():
            path = os.path.join(self.root, relative.replace("/", os.sep))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(text)

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self._temp.cleanup()

    def scan(self):
        return check_unattended_flags.scan_tree([self.root])


UNPAIRED = (
    "$p = Start-Process -FilePath $editor -ArgumentList @('$proj', '-unattended', "
    "'-nopause', '-nosplash', '-nosound', '-RenderOffscreen', '-nocefaccelpaint') -PassThru\n"
)

PAIRED = (
    "$p = Start-Process -FilePath $editor -ArgumentList @('$proj', '-unattended', "
    "'-RunningUnattendedScript', '-nopause', '-nosplash', '-nosound', '-RenderOffscreen', "
    "'-nocefaccelpaint') -PassThru\n"
)


class UnattendedFlagScanTest(unittest.TestCase):

    def test_an_argv_without_the_pair_switch_is_flagged(self):
        with _Tree({"ci/smoke.ps1": UNPAIRED}) as tree:
            violations, files = tree.scan()
            self.assertEqual(files, 1)
            self.assertEqual(len(violations), 1)
            self.assertEqual(violations[0]["line"], 1)

    def test_the_same_argv_with_the_pair_switch_is_not(self):
        with _Tree({"ci/smoke.ps1": PAIRED}) as tree:
            self.assertEqual(tree.scan()[0], [])

    def test_the_pair_switch_counts_from_a_continuation_line(self):
        """How the command was wrapped must not change the verdict.

        PowerShell backticks, YAML block scalars and one-flag-per-line list literals all split a
        single argv across lines; the window is what makes those read the same as one line.
        """
        wrapped = (
            "& $editor $proj `\n"
            "  -ExecCmds=\"Automation RunTests PinWright,Quit\" `\n"
            "  -unattended -nopause -nosplash -nosound -RenderOffscreen -nocefaccelpaint `\n"
            "  -RunningUnattendedScript -Abslog=\"$log\"\n"
        )
        with _Tree({"ci/run.ps1": wrapped}) as tree:
            self.assertEqual(tree.scan()[0], [])

        one_per_line = (
            "const argv = [\n"
            "  '-ExecCmds=Automation RunTests PinWright,Quit',\n"
            "  '-TestExit=Automation Test Queue Empty',\n"
            "  '-unattended',\n"
            "  '-nopause',\n"
            "  '-nosplash',\n"
            "  '-RenderOffScreen',\n"
            "  '-nocefaccelpaint',\n"
            "];\n"
        )
        with _Tree({"skills/launch.js": one_per_line}) as tree:
            violations, _files = tree.scan()
            self.assertEqual([record["line"] for record in violations], [4])

    def test_a_sentence_about_the_defect_is_not_an_invocation(self):
        """The companion-count gate, which is what keeps the scan free of an allowlist."""
        prose = (
            "// -nocefaccelpaint is required under -unattended or CEF web widgets assert.\n"
            "// Nothing here is a launch, and nothing here should be flagged.\n"
        )
        with _Tree({"ci/notes.js": prose}) as tree:
            self.assertEqual(tree.scan()[0], [])

    def test_the_runner_config_double_dash_flag_is_not_an_engine_switch(self):
        """`--unattended` configures the GitHub runner service and has nothing to do with Slate.

        It appears beside enough other switches to look argv-shaped, so only the lookbehind keeps
        it out.
        """
        runner = (
            "& .\\config.cmd --url $url --token $token --labels windows-ue --unattended `\n"
            "  --nopause --nosplash --nosound --replace\n"
        )
        with _Tree({"ci/runner.ps1": runner}) as tree:
            self.assertEqual(tree.scan()[0], [])

    def test_prose_files_are_out_of_scope_by_construction(self):
        """Markdown and Python are excluded deliberately -- see SCOPE in the scanner's docstring.

        Pinned as a test because it is a design decision that reads like an oversight: the same
        violating text in a .md is silently ignored, and a later reader must not "fix" that by
        widening the suffix list without also solving the prose problem.
        """
        with _Tree({"docs/recipe.md": UNPAIRED, "Content/Python/proxy.py": UNPAIRED}) as tree:
            violations, files = tree.scan()
            self.assertEqual(files, 0)
            self.assertEqual(violations, [])

    def test_an_empty_scan_fails_closed(self):
        with _Tree({"docs/readme.md": "nothing to launch here\n"}) as tree:
            violations, files = tree.scan()
            lines, ok = check_unattended_flags.format_report(violations, files, [tree.root])
            self.assertFalse(ok, "a scan that found no command file proves nothing")
            self.assertTrue(any(line.startswith("VACUOUS") for line in lines))

    def test_main_exit_codes_and_report(self):
        with _Tree({"ci/smoke.ps1": UNPAIRED}) as tree:
            buffer = io.StringIO()
            with contextlib.redirect_stdout(buffer):
                code = check_unattended_flags.main([tree.root])
            self.assertEqual(code, 1)
            output = buffer.getvalue()
            self.assertIn("UNATTENDED-ALONE", output)
            self.assertIn("-RunningUnattendedScript", output)

        with _Tree({"ci/smoke.ps1": PAIRED}) as tree:
            buffer = io.StringIO()
            with contextlib.redirect_stdout(buffer):
                code = check_unattended_flags.main([tree.root])
            self.assertEqual(code, 0)
            self.assertIn("CLEAN", buffer.getvalue())

    def test_the_real_plugin_tree_is_scanned_and_clean(self):
        """Non-vacuity plus the gate itself, on the tree this file ships in.

        The lower bound is loose on purpose: a hardcoded file count would go stale. What it pins is
        that the walk still reaches the CI and skill launchers at all -- a suffix list that stopped
        matching would scan nothing and every fixture above would still pass.
        """
        root = check_unattended_flags.default_root()
        violations, files = check_unattended_flags.scan_tree([root])
        self.assertGreater(files, 3, "the walk found almost no command file under %s" % root)
        self.assertEqual(
            ["%s:%d" % (record["path"], record["line"]) for record in violations], [],
            "an editor launch passes -unattended without -RunningUnattendedScript, so one Slate "
            "modal raised during startup can park its game thread indefinitely")


if __name__ == "__main__":
    unittest.main()
