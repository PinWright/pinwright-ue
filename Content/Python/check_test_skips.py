#!/usr/bin/env python3
# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Host-independent static scan for automation tests that NOTE AND PASS without the skip marker.

THE DEFECT. A test that cannot measure its fixture takes an early return. Written as

    AddWarning(TEXT("No editor world, skipping."));   // or AddInfo(...)
    return true;

the test lands `Result={Success}`, is counted in `succeeded`, and is INDISTINGUISHABLE to the
verdict from a test that ran every assertion it owns. Started == succeeded, the drain marker is
present, `skipped=0`, exit code 0 -- a host that measured nothing classifies COMPLETED_CLEAN. The
house mechanism that makes such a return visible is `PinWrightTestSkip::SkipAssertions`
(`Source/PinWright/Private/Tests/TestSkipReporting.h`), which emits `PINWRIGHT_ASSERTIONS_SKIPPED:`
through `AddWarning`; `Content/Python/check_suite_log.py` counts those and classifies the run
COMPLETED_WITH_SKIPS instead. Every note-and-pass that does NOT go through it is a false green.

WHY A STATIC SCAN AND NOT A RUNTIME ONE. The runtime counterpart
`PinWright.infra.skip_marker.*` (`Source/PinWright/Private/Tests/Infra/TestSkipMarkerEmission.cpp`)
asserts the EMITTER's contract -- that the wire text is what the parser greps, and that a marker is
countable. It cannot assert ADOPTION: a call site that never emits the marker emits nothing to
observe, and the only host that could observe a given guard firing is a host on which that guard
fires. Most of these guards are near-dead (`no-editor-world` on an `EditorContext` test is false on
every host that can run the suite at all), so a runtime check would report them green forever while
the source stayed wrong. Reading the source is the only way to see a guard that never fires.

WHAT IS FLAGGED, EXACTLY. An `AddWarning(...)` or `AddInfo(...)` statement whose very next
statement is `return true;` (preprocessor-directive lines in between are stepped over -- a
directive is not a statement). Nothing else: not `return false` (that is a failure and reports as
one), not a note followed by further assertions (the test still measured something), not a note in
a non-test source. The shape is deliberately narrow because this is a GATE -- a scan that flags
correct code gets an allowlist bolted onto it, or gets switched off, and then guards nothing.

BOTH EMITTERS, FOR THE SAME REASON. `AddInfo` is not a quieter `AddWarning`; it is an emitter the
verdict cannot see. The engine DOES log Info entries -- `FAutomationControllerManager::
ReportAutomationResult` walks `Results.GetEntries()` and emits `EAutomationEventType::Info` through
`UE_LOGF(LogAutomationController, Log, ...)` (`AutomationControllerManager.cpp:1685-1693`), which
is why the `FIXTURE-SKIP:` audit token documented in `docs/test-organization.md` is greppable in a
real run log. What the entry does NOT carry is a severity or the wire marker, so `check_suite_log`
counts nothing and the run classifies COMPLETED_CLEAN. Same false green as the warn shape, one
emitter further from view.

Consequences of that narrowness, stated rather than implied. Each was measured over the tree on
2026-08-29, and each residue was judged and converted by hand rather than left for the gate:

  * A guard written `{ AddWarning(...); } return true;` -- warning in a nested block, unconditional
    return outside it -- is NOT flagged. Six such sites existed at the time of writing. They are a
    different shape (the return is not the warning's own early exit) and reading them as this one
    produces false positives on loops that warn per-item.
  * `AddWarning(...); Cleanup(); return true;` -- a real statement between the two -- is NOT
    flagged, for the same reason. One such site existed.
  * A skip emitted one call frame UP is invisible here: a helper taking `FAutomationTestBase&`
    that warns and returns false/nullptr, whose caller turns that into `return true`. Ten such
    helpers existed, several of them fronting many tests each. Nothing in the source text of the
    caller distinguishes that from an ordinary failed call, so no static shape can catch it; what
    keeps it honest is that the helper itself now calls `SkipAssertions`.
  * The `AddInfo` hole this scan used to leave open was swept on 2026-08-30 and the scan widened
    to cover it in the same change. THE CENSUS MUST BE TAKEN WITH THIS SCAN'S OWN ADJACENCY SHAPE,
    not with a hand-written regex: a naive `AddInfo\\([^;]*\\);\\s*\\n\\s*return true;` reads 128
    sites in 43 files against this scan's 227 in 70, because `[^;]*` bails on the first `;` inside
    a message string, `\\s*\\n` cannot see a one-line `{ AddInfo(...); return true; }`, and neither
    steps over a `#endif`. Both figures circulated as "the" count. The paren-matching shape below
    is the one that reproduces. Sweep result: 223 sites converted to `SkipAssertions`, 4 waived at
    the site as genuine post-measurement notes, plus the two `PINWRIGHT_SKIP_IF_*_FIXTURE*` macro
    bodies in `Tests/TestUtils.h` (47 call sites across 17 files) routed through the emitter once.
  * Conditional compilation is not evaluated, exactly as in `check_test_ids.py` beside this file.
    A guard inside a `#if` this host would compile out is still source that will warn-and-pass on
    the host that does compile it.
  * Comments ARE stripped before matching, so a commented-out guard is not a call site. The
    opt-out below is read from the RAW text for that reason.

THE OPT-OUT, AND WHY IT IS A COMMENT AND NOT A FILE. Some warn-and-pass sites are legitimate: a
test that ran all of its assertions, then noticed something worth saying, then returned true has
measured everything it claims to. Statically that reads identically to a skip. Rather than an
allowlist file -- which drifts from the code, is edited by whoever is trying to get green, and
carries no reason -- such a site is marked at the site with a comment carrying a reason:

    // PINWRIGHT_WARNING_IS_NOT_A_SKIP: every assertion above already ran; this notes the
    // measured spread for the next reader.
    AddWarning(FString::Printf(TEXT("spread %.2f"), Spread));
    return true;

`PINWRIGHT_INFO_IS_NOT_A_SKIP` is the same marker spelled for the other emitter; both are accepted
at either kind of site, so a converted or re-emitter-ed guard does not silently lose its waiver.
The marker must be followed by a non-empty reason and must sit on the emitting statement's own
lines or the contiguous comment block immediately above it. A bare marker is rejected, so the
opt-out cannot be used as a silencer.

Pure stdlib, like `check_test_ids.py` and `check_suite_log.py` beside it, so Unreal's bundled
interpreter can run it as shipped. Never `uv`, and never install anything into that interpreter:

    "%UE_ROOT%\\Engine\\Binaries\\ThirdParty\\Python3\\Win64\\python" -m check_test_skips [ROOT ...]

ROOT defaults to the plugin root inferred from this file's location. Exit code is 0 only when the
scan examined test sources at all and found no unmarked warn-and-pass.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Comment stripping is shared rather than re-derived: it preserves byte offsets and newlines so a
# match position still maps to the original line number, and getting that subtly wrong in a second
# copy would shift every reported site by an unpredictable amount.
from check_test_ids import (  # noqa: E402  (path must be set before this import)
    EXCLUDED_DIRS,
    SOURCE_SUFFIXES,
    strip_comments,
)

# Both emitters. `AddInfo` is the one the verdict cannot see at all (no severity, no marker), so
# leaving it out was the larger of the two holes rather than the smaller.
_EMITTER_RE = re.compile(r"\bAdd(Warning|Info)\s*\(")

# Kept as the historical name so an importer of this module does not break on the widening.
_WARN_RE = _EMITTER_RE

_EMITTER_NAMES = ("AddWarning", "AddInfo")

_RETURN_TRUE_RE = re.compile(r"\Areturn\s+true\s*;")

# Written at the site, with a reason, when a note-and-pass really did measure everything it claims.
# The trailing group must be non-empty: a bare marker is a silencer, not a justification. Either
# spelling is honoured at either kind of site, so re-pointing a guard at the other emitter does not
# silently drop its waiver.
_NOT_A_SKIP_RE = re.compile(r"PINWRIGHT_(?:WARNING|INFO)_IS_NOT_A_SKIP\b[:\s]*(\S.*)")

_BARE_NOT_A_SKIP_RE = re.compile(r"PINWRIGHT_(?:WARNING|INFO)_IS_NOT_A_SKIP\b")

# An inline emitter: the site already puts the wire marker on the log, so it is countable even
# though it did not route through PinWrightTestSkip. Archived spellings of the emitter and the
# formatter both count.
_EMITS_MARKER_RE = re.compile(r"PINWRIGHT_ASSERTIONS_SKIPPED|FormatSkipMessage")

# The house emitter, named here only so the report can tell a reader what to write instead.
SKIP_CALL = 'PinWrightTestSkip::SkipAssertions(*this, TEXT("<reason-slug>"), <detail>)'


def _is_test_source(path, text):
    """True for a source file that can register or support automation tests.

    Two independent signals, unioned. A path segment named `Tests` catches fixtures and helpers
    that carry no registration macro of their own; an `AUTOMATION_TEST` macro catches a test that
    lives outside such a directory. A handler that warns and returns true is not a test skip and is
    correctly out of scope.
    """
    if "AUTOMATION_TEST" in text:
        return True
    return "Tests" in path.replace("/", os.sep).split(os.sep)


def _paren_span_end(source, open_paren):
    """Index of the `)` matching `source[open_paren]`, or None if unbalanced.

    String literals are walked rather than skipped, so a `)` inside a message does not close the
    call early -- the messages this scans are full of them.
    """
    depth = 0
    index = open_paren
    length = len(source)
    while index < length:
        char = source[index]
        if char == '"':
            index += 1
            while index < length:
                if source[index] == "\\":
                    index += 2
                    continue
                if source[index] == '"':
                    break
                index += 1
            index += 1
            continue
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return index
        index += 1
    return None


def _skip_space(source, index):
    while index < len(source) and source[index] in " \t\r\n":
        index += 1
    return index


def _skip_space_and_directives(source, index):
    """Whitespace, plus any whole preprocessor-directive lines, from `index` onward.

    A directive is not a statement, so

        #if !PINWRIGHT_HAS_SOMETHING
            AddWarning(TEXT("the compile-time guarantee is not in force here"));
        #endif

            return true;

    is the same warn-and-pass as the adjacent spelling: on every build that compiles the branch,
    the test warns and then returns true having run nothing further. Skipping only lines that
    begin with `#` keeps that narrow -- a real statement between the two (a cleanup call, a
    teardown) still stops the match, because there the return is not the warning's own early exit.
    """
    while True:
        index = _skip_space(source, index)
        if index >= len(source) or source[index] != "#":
            return index
        end = source.find("\n", index)
        if end == -1:
            return len(source)
        index = end + 1


def _opt_out(raw_lines, first_line, last_line):
    """The opt-out reason covering a statement spanning lines `first_line..last_line`, 1-based.

    Searched over the statement's own lines plus the contiguous comment block directly above it,
    which is where a reader writes a justification -- and a justification worth honouring usually
    needs more than one line. The walk STOPS at the first non-comment line above, so a marker
    cannot drift up the function and end up waiving guards its author never read.

    Returns (reason, bare) -- `bare` is True when the marker was present with no reason after it,
    which is reported as an error rather than honoured.
    """
    start = first_line
    while start > 1 and raw_lines[start - 2].strip().startswith("//"):
        start -= 1
    bare = False
    for number in range(start, last_line + 1):
        if number > len(raw_lines):
            break
        line = raw_lines[number - 1]
        match = _NOT_A_SKIP_RE.search(line)
        if match:
            return match.group(1).strip(), False
        if _BARE_NOT_A_SKIP_RE.search(line):
            bare = True
    return None, bare


def scan_text(source, raw, path):
    """One record per unmarked warn-and-pass in already-comment-stripped `source`.

    `raw` is the same file WITHOUT comments stripped, and is read only for the opt-out comment.

    Returns (violations, exempted). `exempted` carries the honoured opt-outs so the report can
    state how many sites were waived and by whose reason -- a waiver nobody can see is an
    allowlist with extra steps.
    """
    violations = []
    exempted = []
    raw_lines = raw.splitlines()
    for match in _WARN_RE.finditer(source):
        close = _paren_span_end(source, match.end() - 1)
        if close is None:
            continue
        after = _skip_space(source, close + 1)
        if after >= len(source) or source[after] != ";":
            continue
        tail = _skip_space_and_directives(source, after + 1)
        if not _RETURN_TRUE_RE.match(source[tail:tail + 32]):
            continue

        argument = source[match.end():close]
        if _EMITS_MARKER_RE.search(argument):
            continue

        first_line = source.count("\n", 0, match.start()) + 1
        last_line = source.count("\n", 0, after) + 1
        record = {"path": path, "line": first_line, "endLine": last_line,
                  "emitter": "Add" + match.group(1),
                  "text": " ".join(source[match.start():after + 1].split())}

        reason, bare = _opt_out(raw_lines, first_line, last_line)
        if reason:
            record["reason"] = reason
            exempted.append(record)
            continue
        if bare:
            record["bare"] = True
        violations.append(record)
    return violations, exempted


def scan_tree(roots):
    """Walk `roots` and collect every unmarked warn-and-pass in a test source under them."""
    violations = []
    exempted = []
    files = 0
    for root in roots:
        for directory, subdirs, filenames in os.walk(root):
            subdirs[:] = [name for name in subdirs if name not in EXCLUDED_DIRS]
            for filename in filenames:
                if not filename.endswith(SOURCE_SUFFIXES):
                    continue
                path = os.path.join(directory, filename)
                try:
                    with open(path, "r", encoding="utf-8", errors="replace") as handle:
                        raw = handle.read()
                except OSError:
                    # Unreadable sources are the dot-prefix scan's problem to report; this scan
                    # would only duplicate that error on the same file.
                    continue
                if not _is_test_source(path, raw):
                    continue
                # Counted BEFORE the emitter filter, so the file count answers "did the scan see
                # the test tree?" and not "does the test tree still warn?". Counting only emitting
                # files would make a fully-converted tree read as VACUOUS -- the scan failing
                # closed at the exact moment it should report success.
                files += 1
                if not any(name in raw for name in _EMITTER_NAMES):
                    continue
                found, waived = scan_text(strip_comments(raw), raw, path)
                violations.extend(found)
                exempted.extend(waived)
    return violations, exempted, files


def _site(record):
    return "%s:%d" % (record["path"], record["line"])


def format_report(violations, exempted, files, roots, verbose=False):
    """Build the human-readable report. Returns (lines, ok)."""
    lines = []
    for record in sorted(violations, key=lambda item: (item["path"], item["line"])):
        emitter = record.get("emitter", "AddWarning")
        if record.get("bare"):
            lines.append(
                "BARE-OPT-OUT %s - PINWRIGHT_%s_IS_NOT_A_SKIP with no reason after it is a "
                "silencer, not a justification. Write the reason, or convert the site."
                % (_site(record), "INFO" if emitter == "AddInfo" else "WARNING"))
        else:
            lines.append("%s %s" % ("INFO-AND-PASS" if emitter == "AddInfo"
                                    else "WARN-AND-PASS", _site(record)))
        lines.append("    %s" % record["text"])
        lines.append(
            "    This test reports success without running its assertions and is "
            "indistinguishable from a real pass in the suite log, so the run classifies "
            "COMPLETED_CLEAN having measured nothing.")
        lines.append(
            "    Fix: emit the marker instead -- %s (include \"Tests/TestSkipReporting.h\"). If "
            "the test really did measure everything it claims, say so at the site with "
            "// PINWRIGHT_%s_IS_NOT_A_SKIP: <reason>."
            % (SKIP_CALL, "INFO" if emitter == "AddInfo" else "WARNING"))

    if verbose:
        for record in sorted(exempted, key=lambda item: (item["path"], item["line"])):
            lines.append("WAIVED %s - %s" % (_site(record), record["reason"]))

    lines.append("SCANNED %d test source file(s) under %s; %d waived at the site"
                 % (files, ", ".join(roots), len(exempted)))

    if not files:
        # Fail closed, for the same reason `check_test_ids.py` does: an empty scan prints a clean
        # verdict exactly like a clean one, and nothing else here can tell "the tree is fine" from
        # "the root argument was wrong".
        lines.append("VACUOUS no automation test source was found at all - the scan proves "
                     "nothing. Check the root path.")
        return lines, False

    ok = not violations
    lines.append("CLEAN every conditional skip emits PINWRIGHT_ASSERTIONS_SKIPPED" if ok
                 else "FAILED %d unmarked note-and-pass site(s) (%d AddWarning, %d AddInfo)"
                      % (len(violations),
                         sum(1 for r in violations if r.get("emitter") != "AddInfo"),
                         sum(1 for r in violations if r.get("emitter") == "AddInfo")))
    return lines, ok


def default_root():
    """The plugin root, two directories up from Content/Python/."""
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Scan automation test sources for AddWarning or AddInfo followed immediately "
                    "by `return true;` without the PINWRIGHT_ASSERTIONS_SKIPPED marker. Such a "
                    "test reports success having run no assertions and the suite cannot tell it "
                    "from a real pass. Needs no editor and no build.")
    parser.add_argument("roots", nargs="*", default=None,
                        help="directories to scan (default: the plugin root above this script)")
    parser.add_argument("--verbose", action="store_true",
                        help="also list the sites waived by an at-the-site opt-out comment")
    args = parser.parse_args(argv)

    roots = args.roots or [default_root()]
    violations, exempted, files = scan_tree(roots)
    lines, ok = format_report(violations, exempted, files, roots, verbose=args.verbose)
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
