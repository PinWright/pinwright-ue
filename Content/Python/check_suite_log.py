#!/usr/bin/env python3
# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Report whether an Unreal automation log represents a run that actually FINISHED.

Why this exists as a separate entry point: the editor that produced the log is, in the failure
case this guards, already dead. A checker that needs a live editor cannot post-mortem the run
that killed it, so this is plain stdlib and imports nothing from Unreal.

The parser and verdict ladder are shared from `mcp_proxy` so this standalone checker remains the
single post-run verdict authority. The caller runs the prepared Unreal command separately, then
invokes this checker with the exact log path; no proxy-owned child process or timeout is involved.

Run it under Unreal's bundled interpreter (never `uv`, and never install anything into it -- this
file and `mcp_proxy` are stdlib-only on purpose so the editor's Python can run them as shipped):

    "%UE_ROOT%\\Engine\\Binaries\\ThirdParty\\Python3\\Win64\\python" -m check_suite_log \\
        <log> [<log> ...] [--expected N] [--min-ratio R] [--crashes DIR] [--no-crash-scan]

States, worst to best: CRASHED (the editor died, positively evidenced), MEMORY_EXHAUSTED (the
engine logged an allocation failure -- the host's wall, or the per-process Job Object cap
scripts/Run-SuiteCapped.ps1 applies), DID_NOT_COMPLETE (the queue never drained and nothing says it
crashed -- it was killed or wedged), NO_TESTS (nothing was enqueued, or nothing recorded a
success), COMPLETED_WITH_FAILURES, COMPLETED_WITH_SKIPS (drained, nothing red, but a test reported
success without running its assertions -- see PINWRIGHT_ASSERTIONS_SKIPPED),
COMPLETED_WITH_MEMORY_PRESSURE (drained and measured, but a suite maintenance collect could not get
the working set back under the hard fraction -- see PINWRIGHT_MEMORY_WATERMARK_EXCEEDED),
COMPLETED_CLEAN.

MEMORY_EXHAUSTED exists for the same reason CRASHED does: an OOM'd editor leaves a truncated log
with no drain marker and often no fatal banner, which classified DID_NOT_COMPLETE and sent readers
looking for a harness timeout instead of for the memory the suite did not give back.

CRASHED and DID_NOT_COMPLETE are separate states because they were being confused in both
directions: a truncated run greps like a crash if you grep a short log for `EnsureFailed`, and two
such truncations were filed as a host crash on that basis. The crash question is therefore
answered from `Saved/Crashes` (ensure reports excluded) and from fatal banners, never from log
length. That scan is ON by default and is found from the log path alone.

Every verdict prints a `provenance:` line naming the absolute log path and, for a completed run,
the exact terminal-marker line and its line number. A suite figure quoted without that linkage
cannot be re-derived, and a number that cannot be re-derived is a recollection.

Exit code is 0 only when every log given is COMPLETED_CLEAN.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mcp_proxy import (  # noqa: E402  (path must be set before this import)
    NO_REPORT_EVIDENCE as _NO_REPORT,
    STATE_COMPLETED_CLEAN,
    classify_log_state,
    parse_automation_log,
    scan_crash_reports,
)


def check_log(path, expected=None, min_ratio=0.5, crash_scan=True, crashes_dir=None):
    """Classify one log into exactly one of the run states."""
    log = parse_automation_log(path)
    # Default ON. The crash question has to be answered by somebody, and leaving it to whoever
    # reads the log afterwards is what produced a crash ticket built on two truncations.
    crash = scan_crash_reports(
        path, crashes_dir=crashes_dir, duration_seconds=log.get("durationSeconds")
    ) if crash_scan else None
    # classify_log_state applies the shared fail-closed ladder to the log after the caller's
    # Unreal command has finished. The checker owns this post-run classification and keeps no
    # process, timeout, or live-editor state.
    state, reason = classify_log_state(_NO_REPORT, log, crash)
    if not log.get("valid"):
        # No counts to compare an expectation against, so the baseline warning is skipped rather
        # than fired on a zero it never measured.
        return {"path": path, "state": state, "reason": reason, "warnings": [],
                "log": log, "crash": crash}

    warnings = []
    # The absolute-baseline check, deliberately a WARNING and deliberately optional.
    #
    # `performed == found` (inside assess_run_completion) proves the run drained everything it
    # enqueued, but it cannot prove `found` itself was right. A run that registers only a fraction
    # of the suite -- an integration sub-module that failed to load, or a launch whose map
    # argument no longer resolves -- enqueues few tests, drains all of them, emits the marker and
    # greps clean. That is a second, independent way to produce a silently-truncated green run,
    # and only an expectation from OUTSIDE the log can catch it.
    #
    # It is a warning, not a gate, and it is never hardcoded: the suite total legitimately moves
    # with every test added (3778, 3780 and 3797 were all correct within one day on this tree), so
    # a compiled-in constant would go stale and start lying -- the exact failure CLAUDE.md warns
    # about. The caller passes today's measured number or gets no absolute check at all.
    if expected is not None:
        performed = log.get("performed")
        if performed is None:
            performed = log.get("started", 0)
        if performed < expected * min_ratio:
            warnings.append(
                "performed %d is below %.0f%% of the expected %d -- suspicious even with a "
                "drain marker; check whether the whole suite registered"
                % (performed, min_ratio * 100, expected)
            )
        elif performed < expected:
            warnings.append(
                "performed %d is short of the expected %d" % (performed, expected)
            )

    return {"path": path, "state": state, "reason": reason,
            "warnings": warnings, "log": log, "crash": crash}


def format_provenance(result):
    """The line that makes a verdict re-derivable: which file, which line, which marker text.

    A suite figure was carried into three board tickets with no log on disk containing it. The
    number may well have been true; it could not be re-derived, so it was worth nothing. Printing
    the citation beside the verdict is what makes the next quote checkable -- and on a run that
    did NOT complete, printing that there is no marker line to cite is the same service.
    """
    log = result["log"]
    path = os.path.abspath(result["path"])
    lineno = log.get("markerLineNumber")
    if lineno:
        return ("    provenance: %s:%d  %s marker: %s"
                % (path, lineno, log.get("markerKind"), log.get("markerLine")))
    echoes = log.get("bareQueuePhrase", 0)
    return ("    provenance: %s  NO TERMINAL MARKER -- nothing in this file certifies the run "
            "finished%s" % (path,
                            (" (the bare phrase appears %dx, all command-line echo)" % echoes)
                            if echoes else ""))


def format_result(result):
    log = result["log"]
    head = "%-23s %s" % (result["state"], os.path.basename(result["path"]))
    # `skipped` sits in the count line and not in a footnote on purpose: it is a count of
    # assertions that did NOT run, printed beside the counts that make the run look measured.
    # Reading `succeeded=4241 skipped=1` is the whole fix -- the 1 is inside the 4241.
    # Both terminal markers are printed for archived-log compatibility. New commands use
    # -TestExit with the drain marker; older `; Quit` logs may carry testComplete instead.
    detail = ("    found=%s started=%s succeeded=%s failed=%s skipped=%s performed=%s "
              "drainMarker=%s testExit=%s testComplete=%s"
              % (log.get("found"), log.get("started"), log.get("succeeded"),
                 log.get("failed"), log.get("skipped"), log.get("performed"),
                 log.get("queueEmpty"), log.get("testExit"), log.get("testComplete")))
    lines = [head, detail, format_provenance(result)]
    # Printed on every run, zeros included, for the reason the crash line is: "I looked, and here
    # is what was there" is evidence about memory; silence is what let an OOM read as a truncation.
    lines.append("    memory: oomLines=%s watermarkMarkers=%s"
                 % (log.get("oom", 0), log.get("memoryPressure", 0)))
    crash = result.get("crash")
    if crash and crash.get("scanned"):
        # Printed on every run, green ones included: "I looked, and here is what was there" is
        # evidence, while silence is what let a short log be read as a crash.
        lines.append("    crashReports: %d non-ensure, %d ensure-only in %s"
                     % (len(crash.get("crashes") or []), len(crash.get("ensures") or []),
                        crash.get("dir")))
    if result.get("reason"):
        lines.append("    reason: %s" % result["reason"])
    for name in log.get("skippedTests") or []:
        lines.append("    skipped assertions: %s" % name)
    for warning in result["warnings"]:
        lines.append("    WARNING: %s" % warning)
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Classify Unreal automation logs as COMPLETED_CLEAN, COMPLETED_WITH_SKIPS, "
                    "COMPLETED_WITH_FAILURES, NO_TESTS, DID_NOT_COMPLETE or CRASHED. Exit 0 only "
                    "when every log is COMPLETED_CLEAN.")
    parser.add_argument("logs", nargs="+", help="automation log file(s) to check")
    parser.add_argument("--expected", type=int, default=None,
                        help="expected test total; enables the absolute-baseline WARNING. "
                             "Pass a freshly measured number, never a remembered one.")
    parser.add_argument("--min-ratio", type=float, default=0.5,
                        help="warn loudly below this fraction of --expected (default 0.5)")
    parser.add_argument("--crashes", default=None,
                        help="crash-report directory; default is the Saved/Crashes found by "
                             "walking up from the log")
    parser.add_argument("--no-crash-scan", action="store_true",
                        help="skip the crash-report scan. The verdict then says 'crash reports "
                             "not checked' rather than implying there were none.")
    args = parser.parse_args(argv)

    worst = 0
    for path in args.logs:
        result = check_log(path, expected=args.expected, min_ratio=args.min_ratio,
                           crash_scan=not args.no_crash_scan, crashes_dir=args.crashes)
        print(format_result(result))
        if result["state"] != STATE_COMPLETED_CLEAN:
            worst = 1
    return worst


if __name__ == "__main__":
    sys.exit(main())
