# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Unit tests for the standalone suite-log verdict gates used after editor_prepare_tests.

The proxy prepares the Unreal command and checker command. The standalone checker owns the
post-run verdict: zero matched tests, fatal or assertion diagnostics, incomplete queues, and
explicit PinWright skip markers must never be reported as a clean run. A reconciled clean log
must remain clean.

The fixtures are literal editor-log text written by this module. That keeps these tests focused
on the parser and checker entry point rather than on another test helper.

Pure stdlib; no editor, no socket, and no uv. Run under Unreal's bundled interpreter from
Content/Python.
"""
import contextlib
import io
import os
import sys
import tempfile
import unittest

# mcp_proxy.py and check_suite_log.py live one directory up (Content/Python/).
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import check_suite_log  # noqa: E402
from mcp_proxy import (  # noqa: E402
    STATE_COMPLETED_CLEAN,
    STATE_COMPLETED_WITH_FAILURES,
    STATE_COMPLETED_WITH_MEMORY_PRESSURE,
    STATE_COMPLETED_WITH_SKIPS,
    STATE_CRASHED,
    STATE_DID_NOT_COMPLETE,
    STATE_MEMORY_EXHAUSTED,
    STATE_NO_TESTS,
    parse_automation_log,
    scan_crash_reports,
)

# The engine's two allocation-failure strings, as they reach a log. Neither matches the fatal
# banner regex, which is the point: an OOM'd run can stop with no crash evidence at all.
_OOM_ALLOC_LINE = (
    "LogMemory: Error: Ran out of memory allocating 1048576 bytes with alignment 16\n"
)
_OOM_BACKUP_POOL_LINE = (
    "LogMemory: Warning: Freeing 209715200 bytes from backup pool to handle out of memory\n"
)

# The in-process suite-maintenance escalation, emitted by PinWrightSuiteMaintenance::RunResetNow
# when a full collect leaves the working set above the hard fraction.
_WATERMARK_LINE = (
    "[2026.09.09-12.00.00:000][ 42]LogPinWrightSuiteMaintenance: Error: "
    "PINWRIGHT_MEMORY_WATERMARK_EXCEEDED reset=17 lastTest=PinWright.probe.T1 usedPhysical "
    "48.20 -> 47.90 GiB of 63.20 GiB total, still at or above the hard fraction 0.75 after a "
    "full collect.\n"
)

# Every launch echoes its own command line, which is why the drain-marker check requires the
# "<N> tests performed" tail rather than the bare phrase. Reproduced so the fixtures carry the
# same trap a real log does.
_CMDLINE = (
    "LogInit: Display: Command Line: Host.uproject -ExecCmds=\"Automation RunTests PinWright,Quit\" "
    "-TestExit=\"Automation Test Queue Empty\" -unattended -nopause\n"
)

# The real marker, from a suite log with the checkout path swapped for a neutral one, including the " [file(line)]"
# tail the engine appends to an AddWarning message. The tail is why the grep must be a PREFIX
# match: an equality or end-anchored match reads this line as no marker at all.
_REAL_SKIP_LINE = (
    "[2026.08.21-10.12.44:594][786]LogAutomationController: Warning: "
    "PINWRIGHT_ASSERTIONS_SKIPPED: PinWright.render.capture_subject_animation."
    "PosedBoundsAreNotUsed reason=posed-bounds-spread-too-small -- posed bone-bounds radius "
    "spans 91.9043..99.6268 cm (8.40%), under the 10% this test needs to distinguish asset "
    "bounds from posed bounds; the invariance assertion below still ran but could not have "
    "failed on this fixture [D:\\Projects\\Example\\Plugins\\PinWright\\Source"
    "\\PinWright\\Private\\Tests\\Render\\TestCaptureSubjectAnimation.cpp(71)]\n"
)


def _log_text(found=3, tests=3, failed=0, performed=3, extra_lines=()):
    """Build an automation log body. `extra_lines` is spliced in among the test results, which is
    where a warning or a crash banner really lands."""
    parts = [_CMDLINE]
    if found is not None:
        parts.append(
            "LogAutomationCommandLine: Display: Found %d automation tests based on 'PinWright'\n"
            % found
        )
    for index in range(tests):
        parts.append(
            "LogAutomationController: Display: Test Started. Name={T%d} "
            "Path={PinWright.probe.T%d}\n" % (index, index)
        )
        if index < tests - failed:
            parts.append(
                "LogAutomationController: Display: Test Completed. Result={Success} "
                "Name={T%d} Path={PinWright.probe.T%d}\n" % (index, index)
            )
        else:
            parts.append(
                "LogAutomationController: Error: Test Completed. Result={Fail} "
                "Name={T%d} Path={PinWright.probe.T%d}\n" % (index, index)
            )
    parts.extend(extra_lines)
    if performed is not None:
        parts.append(
            "LogAutomationCommandLine: Display: ...Automation Test Queue Empty %d tests "
            "performed.\n" % performed
        )
        parts.append("LogExit: Display: **** TestExit: Automation Test Queue Empty ****\n")
    return "".join(parts)


class SuiteVerdictGateTest(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)

    def write(self, name, **kwargs):
        path = os.path.join(self._temp.name, name)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(_log_text(**kwargs))
        return path

    def cli(self, *paths):
        """The CLI exit code, which is what a CI step actually reads. stdout is captured so a
        passing run does not scroll the report of every fixture past the test summary."""
        with contextlib.redirect_stdout(io.StringIO()):
            return check_suite_log.main(list(paths))

    def state(self, path):
        """The state through the post-mortem entry point -- what check_suite_log exits on."""
        return check_suite_log.check_log(path)["state"]

    def test_fixture_command_matches_editor_prepare_tests_contract(self):
        self.assertIn(
            '-ExecCmds="Automation RunTests PinWright,Quit"', _CMDLINE)
        self.assertIn(
            '-TestExit="Automation Test Queue Empty"', _CMDLINE)
    # ---- (d) first, because a gate that fails everything proves nothing.

    def test_a_genuinely_clean_run_still_reads_completed_clean(self):
        path = self.write("clean.log", found=3, tests=3, failed=0, performed=3)
        log = parse_automation_log(path)
        # The fixture must be clean for the RIGHT reasons or it proves nothing.
        self.assertEqual((log["found"], log["started"], log["succeeded"], log["failed"]),
                         (3, 3, 3, 0))
        self.assertEqual(log["skipped"], 0)
        self.assertFalse(log["fatal"])
        self.assertTrue(log["queueEmpty"])
        self.assertEqual(self.state(path), STATE_COMPLETED_CLEAN)
        self.assertEqual(self.cli(path), 0)

    # ---- (a) zero tests.

    def test_zero_tests_found_with_a_terminal_marker_is_not_clean(self):
        # THE hole: found=0 is falsy, so `if found and finished < found` skipped, started=0
        # skipped, and a terminal marker with failed=0 classified clean.
        path = self.write("empty.log", found=0, tests=0, failed=0, performed=0)
        log = parse_automation_log(path)
        self.assertEqual(log["found"], 0, "fixture must record Found 0 or it tests nothing")
        self.assertTrue(log["queueEmpty"], "fixture must carry a terminal marker")
        self.assertEqual(log["failed"], 0, "fixture must grep clean or it tests nothing")
        self.assertEqual(self.state(path), STATE_NO_TESTS)
        self.assertEqual(self.cli(path), 1)

    def test_filter_that_matched_nothing_is_not_clean(self):
        # The other zero-test shape: UE 5.8 prints this instead of "Found 0", and the log has no
        # drain marker for a legitimate reason. It is NO_TESTS, not truncation -- a false
        # "truncated" alarm here is how a guard gets switched off.
        path = os.path.join(self._temp.name, "nomatch.log")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(_CMDLINE)
            fh.write("LogAutomationCommandLine: Error: No automation tests matched 'Nope'\n")
        self.assertEqual(self.state(path), STATE_NO_TESTS)
        self.assertEqual(self.cli(path), 1)

    def test_no_tests_is_not_reported_as_truncation(self):
        path = self.write("empty2.log", found=0, tests=0, failed=0, performed=0)
        result = check_suite_log.check_log(path)
        self.assertEqual(result["state"], STATE_NO_TESTS)
        self.assertIn("no test", result["reason"].lower())
    # ---- (b) a crash on the post-mortem path.

    def test_fatal_banner_with_a_terminal_marker_is_not_clean(self):
        # The counts reconcile, the queue drained, nothing is red -- and the editor crashed.
        # The standalone checker must reject fatal diagnostics before a clean verdict.
        path = self.write(
            "crash.log", found=3, tests=3, failed=0, performed=3,
            extra_lines=["LogWindows: Error: Fatal error: [File:D:/build/Engine/Source/Runtime/"
                         "Core/Private/Misc/AssertionMacros.cpp] [Line: 471] boom\n"])
        log = parse_automation_log(path)
        self.assertTrue(log["fatal"], "fixture must carry a crash banner or it tests nothing")
        self.assertEqual(log["failed"], 0, "fixture must grep clean or it tests nothing")
        self.assertTrue(log["queueEmpty"], "fixture must carry a terminal marker")
        self.assertEqual(log["started"], log["succeeded"] + log["failed"],
                         "fixture must reconcile its counts or it tests nothing")
        # CRASHED, not COMPLETED_WITH_FAILURES: no test failed here, the process died. Folding a
        # dead editor into the failure state invites "which test failed?" when none did.
        self.assertEqual(self.state(path), STATE_CRASHED)
        self.assertEqual(self.cli(path), 1)

    def test_assertion_failed_banner_is_also_fatal(self):
        path = self.write(
            "assert.log", found=3, tests=3, failed=0, performed=3,
            extra_lines=["LogOutputDevice: Error: Assertion failed: Ptr != nullptr\n"])
        self.assertEqual(self.state(path), STATE_CRASHED)

    def test_a_crash_before_the_queue_drained_says_so_in_the_reason(self):
        # The two axes stay apart: the state names the crash, and the reason still reports that
        # the queue had not drained when the process died.
        path = self.write(
            "crash_early.log", found=300, tests=3, failed=0, performed=None,
            extra_lines=["LogWindows: Error: Fatal error: boom\n"])
        result = check_suite_log.check_log(path)
        self.assertEqual(result["state"], STATE_CRASHED)
        self.assertIn("had NOT drained", result["reason"])

    def test_an_ensure_is_not_a_crash(self):
        # An ensure writes a full crash report, logs `Ensure condition failed:` and lets the run
        # continue. Grepping a short log for that family is how two truncations were filed as a
        # host crash. A drained run carrying ensures is still clean.
        path = self.write(
            "ensure.log", found=3, tests=3, failed=0, performed=3,
            extra_lines=["LogOutputDevice: Error: Ensure condition failed: Ptr != nullptr\n"
                         "LogOutputDevice: Error: Stack: EnsureFailed\n"])
        self.assertFalse(parse_automation_log(path)["fatal"],
                         "an ensure must not set the fatal flag")
        self.assertEqual(self.state(path), STATE_COMPLETED_CLEAN)

    # ---- (c) skipped assertions among the successes.

    def test_skip_marker_among_successes_is_not_clean(self):
        path = self.write(
            "skips.log", found=3, tests=3, failed=0, performed=3,
            extra_lines=[_REAL_SKIP_LINE])
        log = parse_automation_log(path)
        # The premise: every count still looks like a green run.
        self.assertEqual(log["failed"], 0)
        self.assertEqual(log["started"], log["succeeded"])
        self.assertTrue(log["queueEmpty"])
        self.assertFalse(log["fatal"])
        self.assertEqual(log["skipped"], 1)
        self.assertEqual(self.state(path), STATE_COMPLETED_WITH_SKIPS)
        self.assertEqual(self.cli(path), 1)

    def test_the_engine_appended_file_line_suffix_does_not_defeat_the_grep(self):
        # The marker is matched by PREFIX. The engine appends " [file(line)]" to an AddWarning
        # message, so an end-anchored or equality match reads the real line as no marker at all.
        path = self.write(
            "skips2.log", found=3, tests=3, failed=0, performed=3,
            extra_lines=[_REAL_SKIP_LINE])
        log = parse_automation_log(path)
        self.assertIn("[D:\\Projects", _REAL_SKIP_LINE, "fixture must carry the engine suffix")
        self.assertEqual(
            log["skippedTests"],
            ["PinWright.render.capture_subject_animation.PosedBoundsAreNotUsed"],
            "the id must come back clean, without the reason= tail or the file(line) suffix")

    def test_a_marker_with_no_test_id_still_counts(self):
        # Emitters are not uniform: several print "<marker>: reason=..." with no test id at all
        # (grep Source/PinWright/Private/Tests/Render). Anchoring the count on the id would miss
        # that whole family, so the count is over markers and the id is best-effort context.
        path = self.write(
            "skips_anon.log", found=3, tests=3, failed=0, performed=3,
            extra_lines=["LogAutomationController: Display: PINWRIGHT_ASSERTIONS_SKIPPED: "
                         "reason=no-live-viewport; the response-shape assertions did not run\n"])
        log = parse_automation_log(path)
        self.assertEqual(log["skipped"], 1)
        self.assertEqual(log["skippedTests"], [], "no id was printed, so none may be invented")
        self.assertEqual(self.state(path), STATE_COMPLETED_WITH_SKIPS)

    def test_failures_outrank_skips(self):
        # A red test is the more urgent fact, and a run with both must not be filed under the
        # softer state. The checker still reports the recorded skip count.
        path = self.write(
            "both.log", found=3, tests=3, failed=1, performed=3,
            extra_lines=[_REAL_SKIP_LINE])
        result = check_suite_log.check_log(path)
        self.assertEqual(result["state"], STATE_COMPLETED_WITH_FAILURES)
        self.assertEqual(result["log"]["skipped"], 1)
    def test_truncation_outranks_skips(self):
        path = self.write(
            "short_with_skips.log", found=300, tests=3, failed=0, performed=None,
            extra_lines=[_REAL_SKIP_LINE])
        self.assertEqual(self.state(path), STATE_DID_NOT_COMPLETE)

    def test_skips_are_reported_but_are_not_a_test_failure(self):
        # A skip marker is its own non-clean state, not a red test failure.
        path = self.write(
            "skips3.log", found=3, tests=3, failed=0, performed=3,
            extra_lines=[_REAL_SKIP_LINE])
        result = check_suite_log.check_log(path)
        self.assertEqual(result["state"], STATE_COMPLETED_WITH_SKIPS)
        self.assertEqual(result["log"]["failed"], 0)
        self.assertIn("PINWRIGHT_ASSERTIONS_SKIPPED", result["reason"])

    def test_checker_uses_the_log_skip_marker_as_verdict(self):
        # The standalone checker must read the log marker directly. No report payload can
        # override a skip recorded by the engine.
        path = self.write(
            "skips4.log", found=3, tests=3, failed=0, performed=3,
            extra_lines=[_REAL_SKIP_LINE])
        result = check_suite_log.check_log(path)
        self.assertEqual(result["state"], STATE_COMPLETED_WITH_SKIPS)
        self.assertEqual(result["log"]["skipped"], 1)
    # ---- The exit code, which is what a CI step and an agent actually read.

    def test_exit_code_is_zero_only_for_the_clean_log(self):
        clean = self.write("ok.log", found=3, tests=3, failed=0, performed=3)
        empty = self.write("none.log", found=0, tests=0, failed=0, performed=0)
        crashed = self.write("boom.log", found=3, tests=3, failed=0, performed=3,
                             extra_lines=["LogWindows: Error: Fatal error: boom\n"])
        skipped = self.write("skip.log", found=3, tests=3, failed=0, performed=3,
                             extra_lines=[_REAL_SKIP_LINE])
        self.assertEqual(self.cli(clean), 0)
        for path in (empty, crashed, skipped):
            with self.subTest(log=os.path.basename(path)):
                self.assertEqual(self.cli(path), 1)
        # One bad log in a batch must fail the batch.
        self.assertEqual(self.cli(clean, skipped), 1)

    def test_the_states_are_mutually_distinct(self):
        states = [
            self.state(self.write("s_clean.log", found=3, tests=3, failed=0, performed=3)),
            self.state(self.write("s_skip.log", found=3, tests=3, failed=0, performed=3,
                                  extra_lines=[_REAL_SKIP_LINE])),
            self.state(self.write("s_fail.log", found=3, tests=3, failed=1, performed=3)),
            self.state(self.write("s_none.log", found=0, tests=0, failed=0, performed=0)),
            self.state(self.write("s_short.log", found=300, tests=3, failed=0, performed=None)),
        ]
        self.assertEqual(states, [STATE_COMPLETED_CLEAN, STATE_COMPLETED_WITH_SKIPS,
                                  STATE_COMPLETED_WITH_FAILURES, STATE_NO_TESTS,
                                  STATE_DID_NOT_COMPLETE])
        self.assertEqual(len(set(states)), 5, states)

    def test_the_detail_line_prints_the_skip_count_beside_the_counts(self):
        # Reading "succeeded=3 skipped=1" is the fix: the 1 is INSIDE the 3.
        path = self.write("printed.log", found=3, tests=3, failed=0, performed=3,
                          extra_lines=[_REAL_SKIP_LINE])
        text = check_suite_log.format_result(check_suite_log.check_log(path))
        self.assertIn("succeeded=3 failed=0 skipped=1", text)
        self.assertIn("PinWright.render.capture_subject_animation.PosedBoundsAreNotUsed", text)
        self.assertIn(STATE_COMPLETED_WITH_SKIPS, text)

    # ---- Memory. Before these gates an OOM'd editor classified DID_NOT_COMPLETE: same truncated
    # log, no drain marker, and often no fatal banner, so the verdict named the symptom.

    def test_an_out_of_memory_run_is_not_a_truncation(self):
        path = self.write("oom.log", found=300, tests=3, failed=0, performed=None,
                          extra_lines=[_OOM_ALLOC_LINE])
        log = parse_automation_log(path)
        self.assertEqual(log["oom"], 1)
        self.assertFalse(log["fatal"])
        self.assertEqual(self.state(path), STATE_MEMORY_EXHAUSTED)
        self.assertEqual(self.cli(path), 1)

    def test_the_backup_pool_line_alone_is_enough(self):
        # The two strings are independent evidence; the allocator may spend the pool without ever
        # reaching the "Ran out of memory allocating" line.
        path = self.write("pool.log", found=300, tests=3, failed=0, performed=None,
                          extra_lines=[_OOM_BACKUP_POOL_LINE])
        self.assertEqual(parse_automation_log(path)["oom"], 1)
        self.assertEqual(self.state(path), STATE_MEMORY_EXHAUSTED)

    def test_an_out_of_memory_outranks_the_crash_it_causes(self):
        # THE case this state exists for, and the one that made it outrank CRASHED. A real OOM
        # does not stop quietly: UE logs the two strings, then dies with `Fatal error:` and
        # writes a crash report whose type is OutOfMemory. Ranked below CRASHED this state would
        # essentially never fire, and the verdict would say "the editor crashed" about a memory
        # failure -- true, and it sends the reader to Saved/Crashes instead of to the memory.
        # Measured: a deliberately capped run (Job Object at 0.05 of RAM) produced exactly this
        # shape and classified CRASHED before the reorder.
        path = self.write("oom_fatal.log", found=3, tests=3, failed=0, performed=None,
                          extra_lines=[_OOM_ALLOC_LINE, "LogWindows: Error: Fatal error: OOM\n"])
        self.assertEqual(self.state(path), STATE_MEMORY_EXHAUSTED)
        # The crash evidence is folded in, not discarded.
        self.assertIn("log banner", check_suite_log.check_log(path)["reason"])

    def test_a_crash_that_is_not_a_memory_failure_is_still_a_crash(self):
        # The control for the reorder above: with no allocation-failure line, a fatal banner
        # still reads CRASHED.
        path = self.write("plain_fatal.log", found=3, tests=3, failed=0, performed=None,
                          extra_lines=["LogWindows: Error: Fatal error: boom\n"])
        self.assertEqual(self.state(path), STATE_CRASHED)

    def test_an_out_of_memory_run_that_enqueued_nothing_still_says_memory(self):
        # NO_TESTS would be true and useless: the filter was fine, the editor died allocating
        # before it could register anything.
        path = self.write("oom_empty.log", found=0, tests=0, failed=0, performed=None,
                          extra_lines=[_OOM_ALLOC_LINE])
        self.assertEqual(self.state(path), STATE_MEMORY_EXHAUSTED)

    def test_a_run_that_survived_an_allocation_failure_is_not_exhausted(self):
        # The backup pool exists to absorb one. A run that drained and measured everything is
        # reported as pressure, not as exhaustion -- but it is not clean either.
        path = self.write("oom_survived.log", found=3, tests=3, failed=0, performed=3,
                          extra_lines=[_OOM_BACKUP_POOL_LINE])
        self.assertEqual(self.state(path), STATE_COMPLETED_WITH_MEMORY_PRESSURE)
        self.assertIn("survived", check_suite_log.check_log(path)["reason"])

    def test_the_reason_names_the_evidence_rather_than_asserting_memory(self):
        path = self.write("oom_reason.log", found=300, tests=3, failed=0, performed=None,
                          extra_lines=[_OOM_ALLOC_LINE, _OOM_BACKUP_POOL_LINE])
        result = check_suite_log.check_log(path)
        self.assertIn("2 allocation-failure line(s)", result["reason"])
        self.assertIn("had NOT drained", result["reason"])

    def test_the_internal_watermark_marker_is_not_a_clean_run(self):
        path = self.write("pressure.log", found=3, tests=3, failed=0, performed=3,
                          extra_lines=[_WATERMARK_LINE])
        log = parse_automation_log(path)
        self.assertEqual(log["memoryPressure"], 1)
        self.assertEqual(log["oom"], 0)
        self.assertTrue(log["queueEmpty"])
        self.assertEqual(self.state(path), STATE_COMPLETED_WITH_MEMORY_PRESSURE)
        self.assertEqual(self.cli(path), 1)

    def test_failures_and_skips_outrank_memory_pressure(self):
        failed = self.write("pressure_fail.log", found=3, tests=3, failed=1, performed=3,
                            extra_lines=[_WATERMARK_LINE])
        skipped = self.write("pressure_skip.log", found=3, tests=3, failed=0, performed=3,
                             extra_lines=[_WATERMARK_LINE, _REAL_SKIP_LINE])
        self.assertEqual(self.state(failed), STATE_COMPLETED_WITH_FAILURES)
        self.assertEqual(self.state(skipped), STATE_COMPLETED_WITH_SKIPS)

    def test_memory_exhausted_outranks_memory_pressure(self):
        # A run that warned about pressure and then died allocating is reported as the death.
        path = self.write("pressure_then_oom.log", found=300, tests=3, failed=0, performed=None,
                          extra_lines=[_WATERMARK_LINE, _OOM_ALLOC_LINE])
        self.assertEqual(self.state(path), STATE_MEMORY_EXHAUSTED)

    def test_the_memory_states_do_not_collapse_into_the_others(self):
        states = [
            self.state(self.write("m_clean.log", found=3, tests=3, failed=0, performed=3)),
            self.state(self.write("m_press.log", found=3, tests=3, failed=0, performed=3,
                                  extra_lines=[_WATERMARK_LINE])),
            self.state(self.write("m_oom.log", found=300, tests=3, failed=0, performed=None,
                                  extra_lines=[_OOM_ALLOC_LINE])),
            self.state(self.write("m_short.log", found=300, tests=3, failed=0, performed=None)),
        ]
        self.assertEqual(states, [STATE_COMPLETED_CLEAN, STATE_COMPLETED_WITH_MEMORY_PRESSURE,
                                  STATE_MEMORY_EXHAUSTED, STATE_DID_NOT_COMPLETE])
        self.assertEqual(len(set(states)), 4, states)

    def test_the_detail_line_reports_the_memory_counts_on_every_run(self):
        # Zeros included: silence about memory is what let an OOM read as a truncation.
        clean = self.write("m_detail_clean.log", found=3, tests=3, failed=0, performed=3)
        self.assertIn("memory: oomLines=0 watermarkMarkers=0",
                      check_suite_log.format_result(check_suite_log.check_log(clean)))
        pressured = self.write("m_detail_press.log", found=3, tests=3, failed=0, performed=3,
                               extra_lines=[_WATERMARK_LINE])
        self.assertIn("memory: oomLines=0 watermarkMarkers=1",
                      check_suite_log.format_result(check_suite_log.check_log(pressured)))


# The bare phrase, as it reaches a log: the argument to -TestExit, echoed by the launcher. A
# `rg -c "Automation Test Queue Empty"` returns non-zero on a run killed at test 1 because of
# these two lines, and a non-zero count reads as success.
_ECHO_LINES = (
    "LogCsvProfiler: Display: Metadata set : commandline=\"Host.uproject "
    "-ExecCmds=&quot;Automation RunTests PinWright,Quit&quot; "
    "-TestExit=&quot;Automation Test Queue Empty&quot; -unattended\"\n"
    "LogInit: Display: Command Line: Host.uproject "
    "-ExecCmds=\"Automation RunTests PinWright,Quit\" "
    "-TestExit=\"Automation Test Queue Empty\" -unattended -nopause\n"
)
_FOUND_LINE = (
    "[2026.08.27-20.10.00:001][  0]LogAutomationCommandLine: Display: "
    "Found 300 automation tests based on 'PinWright'\n"
)
_DRAIN_LINE = (
    "[2026.08.27-22.48.34:106][211]LogAutomationCommandLine: Display: "
    "...Automation Test Queue Empty 300 tests performed.\n"
)


def _results(count, failed=0, first_index=0):
    parts = []
    for index in range(first_index, first_index + count):
        parts.append("[2026.08.27-20.%02d.00:001][ 10]LogAutomationController: Display: "
                     "Test Started. Name={T%d} Path={PinWright.probe.T%d}\n"
                     % (index % 60, index, index))
        verdict = "Fail" if index - first_index < failed else "Success"
        parts.append("[2026.08.27-20.%02d.30:001][ 10]LogAutomationController: Display: "
                     "Test Completed. Result={%s} Name={T%d} Path={PinWright.probe.T%d}\n"
                     % (index % 60, verdict, index, index))
    return "".join(parts)


class CompletenessSelfTest(unittest.TestCase):
    """The five outcomes that used to collapse into "greps clean", pinned against each other.

    A verdict tool with no test of its own is how this class of defect survives: the rule was
    already written in prose, in two documents, and a killed run was still quoted as a green
    suite twice. These fixtures are synthetic on purpose -- they must run in a fresh clone, where
    none of the real logs exist.
    """

    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        # A project-shaped tree, so crash-directory discovery is exercised rather than stubbed.
        self.saved = os.path.join(self._temp.name, "Saved")
        self.logs = os.path.join(self.saved, "Logs")
        self.crashes = os.path.join(self.saved, "Crashes")
        os.makedirs(self.logs)
        os.makedirs(self.crashes)

    def write_log(self, name, body):
        path = os.path.join(self.logs, name)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(body)
        return path

    def write_crash(self, name, is_ensure, crash_type="Crash", age_seconds=0):
        """Write a CrashContext.runtime-xml with the two fields the scan reads."""
        directory = os.path.join(self.crashes, name)
        os.makedirs(directory)
        path = os.path.join(directory, "CrashContext.runtime-xml")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<FGenericCrashContext>\n"
                     "\t<RuntimeProperties>\n"
                     "\t\t<IsEnsure>%s</IsEnsure>\n\t\t<IsAssert>false</IsAssert>\n"
                     "\t\t<CrashType>%s</CrashType>\n\t\t<ErrorMessage>synthetic</ErrorMessage>\n"
                     % ("true" if is_ensure else "false", crash_type))
        if age_seconds:
            stamp = os.path.getmtime(path) - age_seconds
            os.utime(path, (stamp, stamp))
        return directory

    # ---- The five fixtures, and what each one has to be.

    def truncated(self):
        """Killed mid-queue. No marker, no crash, nothing red -- and no echo either, so this is
        the shape isolated from the echo trap."""
        return self.write_log("truncated.log", _FOUND_LINE + _results(120))

    def truncated_with_echo(self):
        """The same kill, in a log that ALSO carries the launcher's echo of its own
        -TestExit argument. The bare phrase is present; the run finished nothing."""
        return self.write_log("truncated_echo.log", _ECHO_LINES + _FOUND_LINE + _results(120))

    def crashed(self):
        return self.write_log(
            "crashed.log",
            _ECHO_LINES + _FOUND_LINE + _results(120)
            + "[2026.08.27-21.00.00:001][ 10]LogWindows: Error: Fatal error: "
              "[File:D:/build/Engine/Private/Misc/AssertionMacros.cpp] [Line: 471] boom\n")

    def completed_with_failures(self):
        return self.write_log(
            "failed.log",
            _ECHO_LINES + _FOUND_LINE + _results(300, failed=2) + _DRAIN_LINE)

    def completed_clean(self):
        return self.write_log(
            "clean.log", _ECHO_LINES + _FOUND_LINE + _results(300) + _DRAIN_LINE)

    def state(self, path, **kwargs):
        return check_suite_log.check_log(path, **kwargs)["state"]

    def test_the_five_outcomes_do_not_collapse(self):
        states = {
            "truncated": self.state(self.truncated()),
            "truncated_with_echo": self.state(self.truncated_with_echo()),
            "crashed": self.state(self.crashed()),
            "completed_with_failures": self.state(self.completed_with_failures()),
            "completed_clean": self.state(self.completed_clean()),
        }
        self.assertEqual(states, {
            "truncated": STATE_DID_NOT_COMPLETE,
            # SAME state as the plain truncation, deliberately, and that is the whole assertion:
            # the argument echo must not move the verdict one step toward "complete".
            "truncated_with_echo": STATE_DID_NOT_COMPLETE,
            "crashed": STATE_CRASHED,
            "completed_with_failures": STATE_COMPLETED_WITH_FAILURES,
            "completed_clean": STATE_COMPLETED_CLEAN,
        })
        # Only the clean one is a pass, and the exit code is what a CI step reads.
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(check_suite_log.main([self.completed_clean()]), 0)
            for path in (self.truncated(), self.truncated_with_echo(), self.crashed(),
                         self.completed_with_failures()):
                self.assertEqual(check_suite_log.main([path]), 1, path)

    def test_the_fixture_really_carries_the_trap(self):
        # If the echo fixture stopped containing the phrase, the test above would pass for the
        # wrong reason forever. Assert the naive grep IS satisfied and the checker is not.
        path = self.truncated_with_echo()
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn("Automation Test Queue Empty", text)
        log = parse_automation_log(path)
        self.assertEqual(log["bareQueuePhrase"], 2,
                         "the fixture must reproduce the real echo count")
        self.assertFalse(log["queueEmpty"], "the echo is not a drain marker")
        self.assertIsNone(log["markerKind"])

    def test_a_count_carrying_phrase_inside_the_command_line_is_still_not_a_marker(self):
        # Source, not just shape: even a command line that happened to carry the full tail form
        # is the launch quoting itself, and quoting is not evidence.
        path = self.write_log(
            "argecho.log",
            "LogInit: Display: Command Line: Host.uproject "
            "-TestExit=\"Automation Test Queue Empty 300 tests performed\"\n"
            + _FOUND_LINE + _results(120))
        log = parse_automation_log(path)
        self.assertFalse(log["queueEmpty"])
        self.assertEqual(self.state(path), STATE_DID_NOT_COMPLETE)

    def test_a_marker_above_the_last_test_result_is_not_terminal(self):
        # Position. Both engine emissions come after every test result, so a marker sitting above
        # them belongs to an appended older run or to quoted text, not to this one.
        path = self.write_log(
            "stale_marker.log", _FOUND_LINE + _results(60) + _DRAIN_LINE + _results(
                60, first_index=60))
        log = parse_automation_log(path)
        self.assertFalse(log["queueEmpty"],
                         "a marker before the last test result must not certify this run")
        self.assertEqual(self.state(path), STATE_DID_NOT_COMPLETE)

    def test_the_marker_line_and_number_are_reported(self):
        path = self.completed_clean()
        log = parse_automation_log(path)
        self.assertEqual(log["markerKind"], "queue-empty")
        self.assertIn("300 tests performed", log["markerLine"])
        with open(path, encoding="utf-8") as fh:
            lines = fh.read().splitlines()
        self.assertEqual(lines[log["markerLineNumber"] - 1].strip(), log["markerLine"])

    # ---- Provenance: a verdict has to be traceable to a file and a line.

    def test_a_clean_verdict_prints_the_log_path_and_the_marker_line(self):
        path = self.completed_clean()
        text = check_suite_log.format_result(check_suite_log.check_log(path))
        self.assertIn("provenance: %s:" % os.path.abspath(path), text)
        self.assertIn("300 tests performed", text)

    def test_an_incomplete_verdict_prints_that_there_is_nothing_to_cite(self):
        text = check_suite_log.format_result(check_suite_log.check_log(self.truncated_with_echo()))
        self.assertIn("NO TERMINAL MARKER", text)
        self.assertIn("command-line echo", text)

    # ---- Crashed vs truncated-with-no-crash-report, the pair that was being confused.

    def test_ensure_only_reports_leave_a_truncation_a_truncation(self):
        self.write_crash("UECC-Windows-AAA_0000", is_ensure=True, crash_type="Ensure")
        self.write_crash("UECC-Windows-AAA_0001", is_ensure=True, crash_type="Ensure")
        result = check_suite_log.check_log(self.truncated())
        self.assertEqual(result["state"], STATE_DID_NOT_COMPLETE)
        self.assertEqual(result["crash"]["crashes"], [])
        self.assertEqual(len(result["crash"]["ensures"]), 2)
        self.assertIn("2 ensure-only", result["reason"])

    def test_a_non_ensure_report_in_the_window_makes_it_a_crash(self):
        self.write_crash("UECC-Windows-BBB_0000", is_ensure=False, crash_type="Assert")
        result = check_suite_log.check_log(self.truncated())
        self.assertEqual(result["state"], STATE_CRASHED)
        self.assertIn("UECC-Windows-BBB_0000", result["reason"])
        self.assertIn("Assert", result["reason"])

    def test_a_report_from_before_the_run_is_not_this_run_s_crash(self):
        self.write_crash("UECC-Windows-CCC_0000", is_ensure=False, age_seconds=48 * 3600)
        result = check_suite_log.check_log(self.truncated())
        self.assertEqual(result["state"], STATE_DID_NOT_COMPLETE)
        self.assertEqual(result["crash"]["crashes"], [])

    def test_the_crash_directory_is_found_from_a_test_run_log_path_too(self):
        # The other sanctioned log location: Saved/PinWright/test-runs/<run>/automation.log.
        run_dir = os.path.join(self.saved, "PinWright", "test-runs", "batchN")
        os.makedirs(run_dir)
        path = os.path.join(run_dir, "automation.log")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(_FOUND_LINE + _results(120))
        scan = scan_crash_reports(path)
        self.assertTrue(scan["scanned"])
        self.assertEqual(os.path.normcase(scan["dir"]), os.path.normcase(self.crashes))

    def test_not_scanning_says_not_checked_rather_than_none(self):
        # "I did not look" and "there was nothing" are different claims, and reporting the first
        # as the second is the same failure this whole checker exists to refuse.
        result = check_suite_log.check_log(self.truncated(), crash_scan=False)
        self.assertEqual(result["state"], STATE_DID_NOT_COMPLETE)
        self.assertIn("crash reports not checked", result["reason"])

    def test_a_green_run_still_reports_what_the_crash_scan_saw(self):
        self.write_crash("UECC-Windows-DDD_0000", is_ensure=True, crash_type="Ensure")
        text = check_suite_log.format_result(check_suite_log.check_log(self.completed_clean()))
        self.assertIn("crashReports: 0 non-ensure, 1 ensure-only", text)


if __name__ == "__main__":
    unittest.main()
