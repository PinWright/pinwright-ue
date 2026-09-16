# Copyright (c) 2026 Alexander Penkin. MIT License.

"""The tolerance in the skip-marker regex, pinned deliberately instead of by accident.

WHAT THIS DEFENDS. `PINWRIGHT_ASSERTIONS_SKIPPED` is a wire format shared between the C++ test
tree and `mcp_proxy.py`. The regex there spells the colon `:?` and takes the whole remainder of
the line as best-effort context, so it counts four shapes: with and without the trailing colon,
and with and without a leading test id.

That tolerance was NOT deliberate. `TestAssetPreviewSubjects.cpp` and
`TestAssetPreviewSubjectTime.cpp` spelled the marker with no colon while every other emitter
spelled it with one, several committed comments claimed a single shared literal, and the counts
came out right anyway -- purely because of the `:?`. A future cleanup that tightened the regex to
match the "one true" spelling would silently have dropped those two files from the suite gate:
their skips would stop being counted and a run that measured nothing would classify
COMPLETED_CLEAN again, which is the exact defect `B-test-skips-assertions-silently` exists for.

The C++ side is now normalised -- one literal, one emitter, `PinWrightTestSkip::SkipAssertions`
in `Source/PinWright/Private/Tests/TestSkipReporting.h` -- so the tree emits one shape. This file
holds the OTHER shapes countable on purpose, because the tree is not the only source of these
logs: archived suite logs (`Saved/Logs/pw_final_suite.log` and older) were written before the
normalisation, a hand-written emitter can still produce the bare form, and the engine's own
" [file(line)]" suffix means the match must stay a prefix match either way. A verdict tool that
silently stops counting an old log is worse than one that never counted.

Pure stdlib; no editor, no socket, no `uv`. Run under Unreal's bundled interpreter from
Content/Python:

    "%UE_ROOT%\\Engine\\Binaries\\ThirdParty\\Python3\\Win64\\python.exe" -m unittest discover tests

or explicitly:

    ...\\python.exe -m unittest tests.test_skip_marker_literal
"""

import os
import sys
import tempfile
import unittest

# mcp_proxy.py lives one directory up (Content/Python/).
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import check_suite_log  # noqa: E402
from mcp_proxy import (  # noqa: E402
    STATE_COMPLETED_WITH_SKIPS,
    parse_automation_log,
)

# The literal itself, written out ONCE here and never imported from the code under test. A test
# that builds its expectation with the thing it is testing agrees with itself whatever the value
# becomes, which is precisely the failure this file is about.
MARKER = "PINWRIGHT_ASSERTIONS_SKIPPED"

# The engine appends " [file(line)]" to any AddWarning message on its way to the log, which is
# why every match has to be a PREFIX match rather than a whole-line one.
_ENGINE_SUFFIX = (
    " [D:\\Projects\\Example\\Plugins\\PinWright\\Source\\PinWright\\Private"
    "\\Tests\\Render\\TestCaptureSubjectAnimation.cpp(71)]"
)

_CMDLINE = (
    "LogInit: Display: Command Line: Host.uproject -ExecCmds=\"Automation RunTests PinWright,Quit\" "
    "-TestExit=\"Automation Test Queue Empty\" -unattended -nopause\n"
)


def _warning(body, suffix=_ENGINE_SUFFIX):
    """One automation-log warning line carrying `body`, with the engine's own suffix attached."""
    return ("[2026.08.22-10.12.44:594][786]LogAutomationController: Warning: "
            + body + suffix + "\n")


def _log_text(extra_lines=(), tests=3, performed=3):
    parts = [_CMDLINE,
             "LogAutomationCommandLine: Display: Found %d automation tests based on 'PinWright'\n"
             % tests]
    for index in range(tests):
        parts.append(
            "LogAutomationController: Display: Test Started. Name={T%d} "
            "Path={PinWright.probe.T%d}\n" % (index, index))
        parts.append(
            "LogAutomationController: Display: Test Completed. Result={Success} "
            "Name={T%d} Path={PinWright.probe.T%d}\n" % (index, index))
    parts.extend(extra_lines)
    parts.append(
        "LogAutomationCommandLine: Display: ...Automation Test Queue Empty %d tests performed.\n"
        % performed)
    parts.append("LogExit: Display: **** TestExit: Automation Test Queue Empty ****\n")
    return "".join(parts)


# The four shapes, named. Each row is (label, marker body, expected attributed id).
#
# The id column is the second half of the tolerance and is asserted separately from the count:
# `skippedTests` is best-effort context, so a shape with no id must count 1 and attribute NOTHING.
# Inventing an id there would be worse than reporting none -- it would name the wrong test.
SHAPES = [
    ("colon + id (what the tree emits today)",
     MARKER + ": PinWright.render.capture_subject_animation.PosedBoundsAreNotUsed "
     "reason=posed-bounds-spread-too-small -- the invariance assertion could not fail here",
     "PinWright.render.capture_subject_animation.PosedBoundsAreNotUsed"),
    ("NO colon + id (TestAssetPreviewSubjects.cpp before normalisation)",
     MARKER + " PinWright.render.capture_asset_preview.PinnedCapturesReproduceWithinTolerance "
     "reason=no-preview-viewport -- this host drew no frame",
     "PinWright.render.capture_asset_preview.PinnedCapturesReproduceWithinTolerance"),
    ("colon + NO id (the old inline AddWarning family)",
     MARKER + ": reason=no-live-viewport; the response-shape assertions did not run",
     None),
    ("NO colon + NO id (the degenerate form)",
     MARKER + " reason=no-live-viewport; the response-shape assertions did not run",
     None),
]


class SkipMarkerLiteralTest(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)

    def write(self, name, **kwargs):
        path = os.path.join(self._temp.name, name)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(_log_text(**kwargs))
        return path

    def test_every_shape_is_counted(self):
        """One marker of each shape, alone in its own log, counts as exactly one skip."""
        for index, (label, body, _expected_id) in enumerate(SHAPES):
            with self.subTest(shape=label):
                path = self.write("shape%d.log" % index, extra_lines=[_warning(body)])
                log = parse_automation_log(path)
                self.assertEqual(
                    log["skipped"], 1,
                    "the '%s' shape stopped being counted; tightening the regex drops a whole "
                    "family of emitters out of the suite gate" % label)

    def test_every_shape_refuses_a_clean_verdict(self):
        """Counting is not the point -- REFUSING is. A counted marker that still classified
        COMPLETED_CLEAN would be a number nobody acts on."""
        for index, (label, body, _expected_id) in enumerate(SHAPES):
            with self.subTest(shape=label):
                path = self.write("verdict%d.log" % index, extra_lines=[_warning(body)])
                state = check_suite_log.check_log(path)["state"]
                self.assertEqual(state, STATE_COMPLETED_WITH_SKIPS, label)

    def test_attribution_is_best_effort_and_never_invented(self):
        """A shape carrying an id must report it, cleanly. A shape carrying none must report
        none rather than mistaking `reason=...` or an English word for a test id."""
        for index, (label, body, expected_id) in enumerate(SHAPES):
            with self.subTest(shape=label):
                path = self.write("attr%d.log" % index, extra_lines=[_warning(body)])
                ids = parse_automation_log(path)["skippedTests"]
                if expected_id is None:
                    self.assertEqual(
                        ids, [],
                        "'%s' carries no test id, so none may be invented" % label)
                else:
                    self.assertEqual(
                        ids, [expected_id],
                        "'%s' must attribute cleanly, without the reason= tail or the engine's "
                        "file(line) suffix" % label)

    def test_all_four_shapes_in_one_log_count_four(self):
        """A real transitional log carries a mix: archived runs and freshly emitted ones are read
        by the same parser. The count is over marker OCCURRENCES, so it must be 4 even though
        only two distinct ids can be attributed."""
        path = self.write(
            "mixed.log", extra_lines=[_warning(body) for _label, body, _id in SHAPES])
        log = parse_automation_log(path)
        self.assertEqual(log["skipped"], 4)
        # SORTED, not log order: mcp_proxy.py returns `sorted(skipped_tests)` so the same set of
        # skips reads the same however the runner interleaved them. Asserted against an
        # explicitly sorted expectation rather than against the SHAPES order, so this test cannot
        # pass by accident on a list that happens to already be alphabetical.
        self.assertEqual(
            log["skippedTests"],
            sorted(expected for _label, _body, expected in SHAPES if expected is not None))
        self.assertEqual(check_suite_log.check_log(path)["state"], STATE_COMPLETED_WITH_SKIPS)

    def test_the_engine_suffix_does_not_break_the_match(self):
        """The reason the match is a prefix match at all. An equality or end-anchored match reads
        a real logged marker as no marker, because the engine appends " [file(line)]"."""
        body = SHAPES[0][1]
        with_suffix = self.write("suffix.log", extra_lines=[_warning(body)])
        without = self.write("nosuffix.log", extra_lines=[_warning(body, suffix="")])
        self.assertEqual(parse_automation_log(with_suffix)["skipped"], 1)
        self.assertEqual(parse_automation_log(without)["skipped"], 1)
        self.assertEqual(
            parse_automation_log(with_suffix)["skippedTests"],
            parse_automation_log(without)["skippedTests"],
            "the engine suffix must not change which test the skip is attributed to")

    def test_a_log_with_no_marker_still_classifies_clean(self):
        """The mirror requirement. A gate that refuses every log is as useless as one that
        refuses none -- and it is what the four assertions above are measured against."""
        path = self.write("clean.log")
        log = parse_automation_log(path)
        self.assertEqual(log["skipped"], 0)
        self.assertEqual(log["skippedTests"], [])
        self.assertNotEqual(check_suite_log.check_log(path)["state"], STATE_COMPLETED_WITH_SKIPS)

    def test_a_line_that_only_discusses_skipping_is_not_counted(self):
        """The count is over the marker, not over anything that mentions the mechanism. A test
        that skipped without emitting is exactly the case the gate CANNOT see, and pretending
        otherwise would make the number mean less than it does."""
        path = self.write("nearmiss.log", extra_lines=[
            _warning("the test skipped its assertions but printed no marker"),
        ])
        self.assertEqual(parse_automation_log(path)["skipped"], 0)


if __name__ == "__main__":
    unittest.main()
