# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Self-test for `check_test_skips.py`, the host-independent note-and-pass scan.

Covers both emitters. `AddWarning` cases come first, then the `AddInfo` half below -- same defect,
same fixtures, plus the two adjacency shapes (a one-line guard, and a `;` inside the message) that
made a hand-written census read 128 sites where this scan reads 227. Those two cases are the
counting-shape regression guards: a scan that silently undercounts is worse than none, because its
number gets quoted.

WHY A SELF-TEST AND NOT JUST THE SCAN. The scan's whole value is that it fails when a test notes
and passes without the marker; a scan that can no longer fail prints CLEAN identically to a clean
tree, and nothing downstream can tell the two apart. Three fixture pairs hold it between them:

  * the defect -- `AddWarning(...); return true;` -- must be flagged;
  * the fix -- `PinWrightTestSkip::SkipAssertions(...); return true;` -- must NOT be, or the whole
    sweep this gate protects would report as 328 violations forever;
  * the known false-positive shape -- a warning followed by further assertions, and a warning
    followed by `return false` -- must NOT be, because a gate that flags correct code gets an
    allowlist bolted on or gets switched off.

The opt-out is held by its own pair: a marker WITH a reason waives the site, a marker WITHOUT one
is reported as a silencer. Without the second half the opt-out is a mute button.

Pure stdlib; no editor, no socket, no `uv`. Run under Unreal's bundled interpreter from
Content/Python:

    "%UE_ROOT%\\Engine\\Binaries\\ThirdParty\\Python3\\Win64\\python.exe" -m unittest discover tests

or explicitly:

    ...\\python.exe -m unittest tests.test_warn_and_pass_scan
"""

import contextlib
import io
import os
import sys
import tempfile
import unittest

# check_test_skips.py lives one directory up (Content/Python/).
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import check_test_skips  # noqa: E402

# Every fixture goes under a `Tests` path segment, which is one of the two signals that make a
# source file in scope. The other -- an AUTOMATION_TEST macro -- gets its own case below.
TEST_PATH = "Source/PinWright/Private/Tests/Probe/TestProbe.cpp"


def _body(statements):
    """A RunTest in the exact shape the tree writes, so the fixtures match real spacing."""
    return ('bool FProbeTest::RunTest(const FString& Parameters)\n'
            '{\n'
            '%s'
            '}\n' % statements)


class _Tree(object):
    """A throwaway source tree. Files are written under a tempdir, never inside the plugin."""

    def __init__(self, files):
        self._temp = tempfile.TemporaryDirectory(prefix="pinwright-warn-and-pass-scan-")
        self.root = self._temp.name
        for relative, text in files.items():
            path = os.path.join(self.root, relative.replace("/", os.sep))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(text)

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self._temp.cleanup()

    def scan(self, argv=None):
        violations, exempted, files = check_test_skips.scan_tree([self.root])
        # The CLI is exercised alongside the API so the exit code -- the only thing CI reads -- is
        # covered by every case below rather than by one separate happy-path test.
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            code = check_test_skips.main((argv or []) + [self.root])
        return {"violations": violations, "exempted": exempted, "files": files,
                "exit": code, "output": captured.getvalue()}


class WarnAndPassScanTests(unittest.TestCase):

    def test_bare_warn_and_pass_is_flagged(self):
        """The defect itself: success reported with no assertion run and nothing countable said."""
        with _Tree({TEST_PATH: _body(
            '    if (!World)\n'
            '    {\n'
            '        AddWarning(TEXT("No editor world, skipping."));\n'
            '        return true;\n'
            '    }\n'
            '    TestTrue(TEXT("measured"), true);\n')}) as tree:
            result = tree.scan()

        self.assertEqual(len(result["violations"]), 1, result["violations"])
        self.assertEqual(result["violations"][0]["line"], 5)
        self.assertNotEqual(result["exit"], 0)
        # The report must name the consequence and the replacement, not merely the line.
        self.assertIn("COMPLETED_CLEAN having measured nothing", result["output"])
        self.assertIn("PinWrightTestSkip::SkipAssertions", result["output"])

    def test_converted_site_is_not_flagged(self):
        """The fix. If this ever regresses the gate reports the whole converted tree as broken."""
        with _Tree({TEST_PATH: _body(
            '    if (!World)\n'
            '    {\n'
            '        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),\n'
            '            TEXT("No editor world available."));\n'
            '        return true;\n'
            '    }\n')}) as tree:
            result = tree.scan()

        self.assertEqual(result["violations"], [])
        self.assertEqual(result["exit"], 0)

    def test_inline_marker_emitter_is_not_flagged(self):
        """A site that spells the wire marker itself is countable, however it was written.

        Archived emitters predate the header and put the literal straight into the message. They
        are not the house shape, but they ARE visible to `check_suite_log.py`, which is what this
        gate is protecting -- flagging them would be flagging a run that classifies correctly.
        """
        with _Tree({TEST_PATH: _body(
            '    AddWarning(FString::Printf(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: %s"), *Name));\n'
            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(result["violations"], [])
        self.assertEqual(result["exit"], 0)

    def test_warning_followed_by_assertions_is_not_flagged(self):
        """The known false-positive shape. The test warned, then went on measuring."""
        with _Tree({TEST_PATH: _body(
            '    AddWarning(TEXT("Fixture is unusually small."));\n'
            '    TestTrue(TEXT("still measured"), Value > 0);\n'
            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(result["violations"], [])
        self.assertEqual(result["exit"], 0)

    def test_warning_followed_by_return_false_is_not_flagged(self):
        """`return false` fails the test. It is loud already and needs no marker."""
        with _Tree({TEST_PATH: _body(
            '    AddWarning(TEXT("Cannot proceed."));\n'
            '    return false;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(result["violations"], [])
        self.assertEqual(result["exit"], 0)

    def test_parenthesis_and_semicolon_inside_the_message_do_not_end_the_call(self):
        """Real messages carry both. A naive scan reads the `)` as the call's close and misses."""
        with _Tree({TEST_PATH: _body(
            '    AddWarning(TEXT("no world (GEditor null); skipping"));\n'
            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(len(result["violations"]), 1, result["violations"])
        self.assertNotEqual(result["exit"], 0)

    def test_multi_line_call_is_reported_at_its_first_line(self):
        """Messages wrap. The site a reader needs is where the call starts, not where it ends."""
        with _Tree({TEST_PATH: _body(
            '    AddWarning(FString::Printf(\n'
            '        TEXT("host produced %d of %d"),\n'
            '        Got, Want));\n'
            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(len(result["violations"]), 1, result["violations"])
        self.assertEqual(result["violations"][0]["line"], 3)
        self.assertEqual(result["violations"][0]["endLine"], 5)

    def test_preprocessor_directive_between_the_two_does_not_hide_the_shape(self):
        """A `#endif` is not a statement. On every build that compiles the branch, this warns and
        returns true having run nothing further."""
        with _Tree({TEST_PATH: _body(
            '#if !PINWRIGHT_HAS_SOMETHING\n'
            '    AddWarning(TEXT("the compile-time guarantee is not in force here"));\n'
            '#endif\n'
            '\n'
            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(len(result["violations"]), 1, result["violations"])
        self.assertNotEqual(result["exit"], 0)

    def test_a_real_statement_between_the_two_is_not_flagged(self):
        """The cleanup shape. The return is not the warning's own early exit, and reading it as
        one produces false positives; those sites are judged by hand, not by this gate."""
        with _Tree({TEST_PATH: _body(
            '    AddWarning(TEXT("could not build the probe"));\n'
            '    CleanupTestAsset(Path);\n'
            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(result["violations"], [])
        self.assertEqual(result["exit"], 0)

    def test_commented_out_guard_is_not_a_call_site(self):
        """`//` and block comments are lexical. A commented guard emits nothing at runtime."""
        with _Tree({TEST_PATH: _body(
            '    // AddWarning(TEXT("old guard"));\n'
            '    // return true;\n'
            '    /* AddWarning(TEXT("older guard"));\n'
            '       return true; */\n'
            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(result["violations"], [])
        self.assertEqual(result["exit"], 0)

    def test_opt_out_with_a_reason_waives_the_site(self):
        """A test that really did measure everything says so AT the site, with a reason."""
        with _Tree({TEST_PATH: _body(
            '    TestTrue(TEXT("measured"), true);\n'
            '    // PINWRIGHT_WARNING_IS_NOT_A_SKIP: every assertion above already ran; this\n'
            '    // records the measured spread for the next reader.\n'
            '    AddWarning(FString::Printf(TEXT("spread %.2f"), Spread));\n'
            '    return true;\n')}) as tree:
            result = tree.scan(["--verbose"])

        self.assertEqual(result["violations"], [])
        self.assertEqual(len(result["exempted"]), 1, result["exempted"])
        self.assertIn("every assertion above already ran", result["exempted"][0]["reason"])
        self.assertEqual(result["exit"], 0)
        # A waiver nobody can see is an allowlist with extra steps, so --verbose must print it.
        self.assertIn("WAIVED", result["output"])

    def test_bare_opt_out_is_rejected_as_a_silencer(self):
        """Without this half the opt-out is a mute button with no argument attached."""
        with _Tree({TEST_PATH: _body(
            '    // PINWRIGHT_WARNING_IS_NOT_A_SKIP\n'
            '    AddWarning(TEXT("no world, skipping"));\n'
            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(len(result["violations"]), 1, result["violations"])
        self.assertTrue(result["violations"][0].get("bare"))
        self.assertIn("BARE-OPT-OUT", result["output"])
        self.assertNotEqual(result["exit"], 0)

    def test_opt_out_two_lines_above_does_not_reach(self):
        """The comment must sit on the statement or the line directly above it.

        A marker allowed to drift arbitrarily far up the function would eventually cover guards
        its author never read.
        """
        with _Tree({TEST_PATH: _body(
            '    // PINWRIGHT_WARNING_IS_NOT_A_SKIP: about something else entirely.\n'
            '    Value = Measure();\n'
            '    AddWarning(TEXT("no world, skipping"));\n'
            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(len(result["violations"]), 1, result["violations"])
        self.assertEqual(result["exempted"], [])
        self.assertNotEqual(result["exit"], 0)

    def test_non_test_source_is_out_of_scope(self):
        """A handler that warns and returns true is answering an RPC, not skipping assertions."""
        with _Tree({"Source/PinWright/Private/Handlers/ProbeHandler.cpp":
                    'bool Handle(FThing& Out)\n'
                    '{\n'
                    '    AddWarning(TEXT("nothing to do"));\n'
                    '    return true;\n'
                    '}\n'}) as tree:
            result = tree.scan()

        self.assertEqual(result["violations"], [])
        self.assertEqual(result["files"], 0)
        # Scanning nothing is never a pass: see the vacuity case below.
        self.assertNotEqual(result["exit"], 0)

    def test_test_outside_a_tests_directory_is_still_in_scope(self):
        """The macro is the second scope signal, so a test elsewhere in the tree is not exempt."""
        with _Tree({"Source/PinWright/Private/Core/ProbeInlineTest.cpp":
                    'IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProbeTest, "PinWright.probe.Inline",\n'
                    '    EAutomationTestFlags::EditorContext)\n'
                    + _body('    AddWarning(TEXT("no world, skipping"));\n'
                            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(len(result["violations"]), 1, result["violations"])
        self.assertNotEqual(result["exit"], 0)

    def test_empty_tree_fails_closed(self):
        """An empty scan prints CLEAN identically to a clean one. It must not exit 0."""
        with _Tree({"Source/PinWright/Private/Tests/Probe/README.md": "nothing here\n"}) as tree:
            result = tree.scan()

        self.assertEqual(result["files"], 0)
        self.assertIn("VACUOUS", result["output"])
        self.assertNotEqual(result["exit"], 0)

    # ------------------------------------------------------------------------------------------
    # The AddInfo half. Same defect, an emitter the verdict can see even less of: the engine DOES
    # log Info entries (AutomationControllerManager.cpp:1685-1693 emits them through UE_LOGF at
    # Log verbosity), but they carry no severity and no marker, so `check_suite_log` counts
    # nothing and the run classifies COMPLETED_CLEAN. These cases hold the widening in place.
    # ------------------------------------------------------------------------------------------

    def test_bare_info_and_pass_is_flagged(self):
        """The sibling defect. Before the widening this shape was invisible to the gate."""
        with _Tree({TEST_PATH: _body(
            '    if (!World)\n'
            '    {\n'
            '        AddInfo(TEXT("No editor world, skipping."));\n'
            '        return true;\n'
            '    }\n'
            '    TestTrue(TEXT("measured"), true);\n')}) as tree:
            result = tree.scan()

        self.assertEqual(len(result["violations"]), 1, result["violations"])
        self.assertEqual(result["violations"][0]["line"], 5)
        self.assertEqual(result["violations"][0]["emitter"], "AddInfo")
        # The label names the emitter, so a reader greps for the right thing at the site.
        self.assertIn("INFO-AND-PASS", result["output"])
        self.assertNotEqual(result["exit"], 0)

    def test_converted_info_site_is_not_flagged(self):
        """The fix, applied to the AddInfo sweep. 223 sites depend on this staying true."""
        with _Tree({TEST_PATH: _body(
            '    if (!World)\n'
            '    {\n'
            '        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),\n'
            '            TEXT("No editor world available."));\n'
            '        return true;\n'
            '    }\n')}) as tree:
            result = tree.scan()

        self.assertEqual(result["violations"], [])
        self.assertEqual(result["exit"], 0)

    def test_one_line_guard_is_flagged(self):
        """The counting shape that made two censuses disagree.

        `{ AddInfo(...); return true; }` on ONE line has no newline between the call and the
        return, so the hand-written regex that produced the filed figure of 128 sites never saw
        it -- while this scan's paren-matching adjacency does, and reads 227. A scan that cannot
        see this shape undercounts silently, and its number gets quoted.
        """
        with _Tree({TEST_PATH: _body(
            '    if (!F.Init()) { AddInfo(TEXT("skip")); return true; }\n'
            '    TestTrue(TEXT("measured"), true);\n')}) as tree:
            result = tree.scan()

        self.assertEqual(len(result["violations"]), 1, result["violations"])
        self.assertEqual(result["violations"][0]["emitter"], "AddInfo")
        self.assertNotEqual(result["exit"], 0)

    def test_semicolon_inside_an_info_message_does_not_end_the_call(self):
        """The other half of the same undercount: `[^;]*` bails on a `;` inside the string."""
        with _Tree({TEST_PATH: _body(
            '    AddInfo(TEXT("No editor world; skipping the live half."));\n'
            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(len(result["violations"]), 1, result["violations"])
        self.assertNotEqual(result["exit"], 0)

    def test_info_followed_by_assertions_is_not_flagged(self):
        """The known false-positive shape, on the info emitter. The test kept measuring."""
        with _Tree({TEST_PATH: _body(
            '    AddInfo(TEXT("Fixture is unusually small."));\n'
            '    TestTrue(TEXT("still measured"), Value > 0);\n'
            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(result["violations"], [])
        self.assertEqual(result["exit"], 0)

    def test_info_opt_out_with_a_reason_waives_the_site(self):
        """A note published AFTER every assertion ran. Four such sites exist in the tree."""
        with _Tree({TEST_PATH: _body(
            '    TestTrue(TEXT("measured"), true);\n'
            '    // PINWRIGHT_INFO_IS_NOT_A_SKIP: every assertion above already ran; this\n'
            '    // records the measured shot count for the next reader.\n'
            '    AddInfo(FString::Printf(TEXT("shots %d"), Shots));\n'
            '    return true;\n')}) as tree:
            result = tree.scan(["--verbose"])

        self.assertEqual(result["violations"], [])
        self.assertEqual(len(result["exempted"]), 1, result["exempted"])
        self.assertIn("every assertion above already ran", result["exempted"][0]["reason"])
        self.assertEqual(result["exit"], 0)
        self.assertIn("WAIVED", result["output"])

    def test_bare_info_opt_out_is_rejected_as_a_silencer(self):
        """Without this the widening ships a mute button along with the gate."""
        with _Tree({TEST_PATH: _body(
            '    // PINWRIGHT_INFO_IS_NOT_A_SKIP\n'
            '    AddInfo(TEXT("no world, skipping"));\n'
            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(len(result["violations"]), 1, result["violations"])
        self.assertTrue(result["violations"][0].get("bare"))
        self.assertIn("BARE-OPT-OUT", result["output"])
        self.assertIn("PINWRIGHT_INFO_IS_NOT_A_SKIP", result["output"])
        self.assertNotEqual(result["exit"], 0)

    def test_either_opt_out_spelling_is_honoured_at_either_site(self):
        """Re-pointing a guard at the other emitter must not silently drop its waiver.

        The two spellings are one convention, not two allowlists; a site that was justified as an
        AddWarning stays justified when it becomes an AddInfo, and vice versa.
        """
        with _Tree({TEST_PATH: _body(
            '    TestTrue(TEXT("measured"), true);\n'
            '    // PINWRIGHT_WARNING_IS_NOT_A_SKIP: measured everything; publishing the total.\n'
            '    AddInfo(TEXT("total 7"));\n'
            '    return true;\n'),
            "Source/PinWright/Private/Tests/Probe/TestProbeTwo.cpp": _body(
            '    TestTrue(TEXT("measured"), true);\n'
            '    // PINWRIGHT_INFO_IS_NOT_A_SKIP: measured everything; publishing the spread.\n'
            '    AddWarning(TEXT("spread 2"));\n'
            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(result["violations"], [])
        self.assertEqual(len(result["exempted"]), 2, result["exempted"])
        self.assertEqual(result["exit"], 0)

    def test_inline_marker_emitter_on_info_is_not_flagged(self):
        """An info site that spells the wire marker itself is countable however it was written."""
        with _Tree({TEST_PATH: _body(
            '    AddInfo(FString::Printf(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: %s"), *Name));\n'
            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(result["violations"], [])
        self.assertEqual(result["exit"], 0)

    def test_both_emitters_are_counted_separately_in_the_verdict(self):
        """The failure line splits the two, so a widening regression is readable from the tail."""
        with _Tree({TEST_PATH: _body(
            '    AddWarning(TEXT("no world"));\n'
            '    return true;\n'),
            "Source/PinWright/Private/Tests/Probe/TestProbeTwo.cpp": _body(
            '    AddInfo(TEXT("no world"));\n'
            '    return true;\n')}) as tree:
            result = tree.scan()

        self.assertEqual(len(result["violations"]), 2, result["violations"])
        self.assertIn("2 unmarked note-and-pass site(s) (1 AddWarning, 1 AddInfo)",
                      result["output"])
        self.assertNotEqual(result["exit"], 0)

    def test_the_real_plugin_tree_is_scanned_and_clean(self):
        """Non-vacuity plus the gate itself, on the tree this file ships in.

        The file-count floor is deliberately loose -- it moves with every added test file. What it
        pins is that the scan still SEES the tree: a scope predicate that stopped matching would
        report zero files and every fixture above would still pass.
        """
        root = check_test_skips.default_root()
        violations, _exempted, files = check_test_skips.scan_tree([root])
        self.assertGreater(files, 100, "scope predicate matched almost nothing under %s" % root)
        self.assertEqual(
            ["%s:%d" % (record["path"], record["line"]) for record in violations], [],
            "a test warns and returns true without PINWRIGHT_ASSERTIONS_SKIPPED, so it reports "
            "success having run no assertions and the suite cannot tell it from a real pass")


if __name__ == "__main__":
    unittest.main()
