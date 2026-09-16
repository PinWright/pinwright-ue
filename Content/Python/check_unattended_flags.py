#!/usr/bin/env python3
# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Host-independent static scan for editor launches that pass -unattended without
-RunningUnattendedScript.

THE DEFECT. The two switches are not interchangeable and shipping the first without the second is
the worst of the three reachable states. `FSlateApplication::AddModalWindow` consults
`GIsRunningUnattendedScript` and nothing else before returning without showing the dialog
(`Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:2134` in UE 5.8; `IsUnattended`
does not appear in that file at all). Only `-RunningUnattendedScript` sets that global
(`Runtime/Launch/Private/LaunchEngineLoop.cpp:6857-6860`); `-unattended` sets `FApp::IsUnattended()`,
which that path never reads. So under `-unattended` alone, one Slate modal raised during startup --
before any test registers, before a plugin ticker has run once -- parks the game thread for good.

WHY IT IS INVISIBLE WHEN IT HAPPENS. The wedged process shows ~0% CPU and zero I/O, the log simply
stops mid-startup, and under `-RenderOffScreen` the modal's window is never presented, so enumerating
the process's top-level windows finds nothing to dismiss. Every symptom points away from a dialog.
The only cheap way to keep it from recurring is to refuse the argv that permits it.

WHY THIS IS A TEXT SCAN OVER COMMAND FILES, NOT A UNIT TEST. The proxy's own argv tables
(`_HEADLESS_FLAGS`, `build_editor_command`, `_editor_prepare_tests`) are covered by
`tests/test_mcp_proxy_editor_start.py`. The surface that unit test cannot reach is every launch
written OUTSIDE the proxy: PowerShell smoke drivers and workflow scripts. Those
files are commands rather than prose, so a lexical scan of them is exact.

SCOPE, AND WHAT IS DELIBERATELY NOT SCANNED. Only `.ps1`, `.yml`, `.yaml` and `.js`. Markdown and
Python are excluded on purpose: both are full of prose that names `-unattended` while explaining this
very defect, and a scan that had to tell an explanation from an invocation would need an opt-out
comment at every paragraph -- an allowlist that large is what makes a gate get disabled instead of
fixed.

An occurrence is only judged when it looks like an argv rather than a sentence: at least
MIN_COMPANION_FLAGS distinct other engine launch switches must appear within WINDOW_LINES of it. That
window, rather than the single line, is what makes the scan indifferent to how the command was
wrapped -- PowerShell backticks, YAML block scalars, one-flag-per-line list literals all read the
same.

Pure stdlib, like `check_test_ids.py` and `check_test_skips.py` beside it, so Unreal's bundled
interpreter can run it as shipped:

    "%UE_ROOT%\\Engine\\Binaries\\ThirdParty\\Python3\\Win64\\python" check_unattended_flags.py [ROOT ...]

ROOT defaults to the plugin root inferred from this file's location. Exit code is 0 only when the
scan found command files at all and every argv-shaped `-unattended` in them was paired.
"""
import argparse
import os
import re
import sys

# Build output, packaged output and the editor's own scratch. A copy of the tree under any of these
# would report the same launch twice and name a path nobody can edit.
EXCLUDED_DIRS = frozenset({
    "dist", "Binaries", "Intermediate", "Saved", ".git", "__pycache__", "node_modules",
})

# Command surfaces only. See SCOPE above for why .md and .py are absent.
COMMAND_SUFFIXES = (".ps1", ".yml", ".yaml", ".js")

# Matches one command-line switch token. The `-` in the lookbehind is what keeps `--unattended`
# (the GitHub runner's own config.cmd flag, unrelated to the engine) out of the scan.
_SWITCH_RE = re.compile(r"(?<![\w-])-([A-Za-z][A-Za-z0-9_]*)")

_UNATTENDED = "unattended"
_PAIR = "runningunattendedscript"

# Other engine launch switches. Their presence is what separates an invocation from a sentence about
# one; none of them is required, and the pair switch is not among them because finding it is the
# pass condition rather than evidence of argv shape.
COMPANION_FLAGS = frozenset({
    "nopause", "nosplash", "nosound", "renderoffscreen", "nullrhi", "nocefaccelpaint",
    "execcmds", "testexit", "abslog", "reportexportpath", "ddc", "nosourcecontrol",
    "skipcompile", "stdout", "forcelogflush", "autodeclinepackagerecovery",
})

# Two, not one: `-nocefaccelpaint is required under -unattended` is a sentence with one companion,
# and sentences like it are the whole false-positive population.
MIN_COMPANION_FLAGS = 2

# Half-height of the window searched for companions and for the pair switch, in lines. Six covers
# every wrapping style in this tree (the longest is the proxy's one-flag-per-line launch_argv).
WINDOW_LINES = 6


def _switches(text):
    """Lowercased switch names appearing in one chunk of text."""
    return {match.group(1).lower() for match in _SWITCH_RE.finditer(text)}


def scan_text(lines):
    """Return the 1-based line numbers of argv-shaped `-unattended` uses that lack the pair switch."""
    violations = []
    for index, line in enumerate(lines):
        if _UNATTENDED not in _switches(line):
            continue
        low = max(0, index - WINDOW_LINES)
        window = "\n".join(lines[low:index + WINDOW_LINES + 1])
        present = _switches(window)
        if len(present & COMPANION_FLAGS) < MIN_COMPANION_FLAGS:
            continue
        if _PAIR in present:
            continue
        violations.append(index + 1)
    return violations


def scan_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.read().splitlines()
    return [{"path": path, "line": line, "text": lines[line - 1].strip()}
            for line in scan_text(lines)]


def scan_tree(roots):
    """Walk the roots and return (violations, files_scanned)."""
    violations = []
    files = 0
    for root in roots:
        if os.path.isfile(root):
            if root.lower().endswith(COMMAND_SUFFIXES):
                files += 1
                violations.extend(scan_file(root))
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [name for name in dirnames if name not in EXCLUDED_DIRS]
            for filename in filenames:
                if not filename.lower().endswith(COMMAND_SUFFIXES):
                    continue
                files += 1
                violations.extend(scan_file(os.path.join(dirpath, filename)))
    return violations, files


def format_report(violations, files, roots):
    """Build the human-readable report. Returns (lines, ok)."""
    lines = []
    for record in sorted(violations, key=lambda item: (item["path"], item["line"])):
        lines.append("UNATTENDED-ALONE %s:%d" % (record["path"], record["line"]))
        lines.append("    %s" % record["text"])
        lines.append(
            "    -unattended sets FApp::IsUnattended(), which FSlateApplication::AddModalWindow "
            "never reads. One Slate modal raised during startup then owns the game thread for the "
            "life of the process: ~0% CPU, no further log lines, and under -RenderOffScreen no "
            "visible window to dismiss.")
        lines.append(
            "    Fix: add -RunningUnattendedScript to this launch. It is the only switch that sets "
            "GIsRunningUnattendedScript, which is the only thing that path consults.")

    lines.append("SCANNED %d command file(s) under %s" % (files, ", ".join(roots)))

    if not files:
        # Fail closed, like check_test_ids.py: an empty scan prints a clean verdict exactly like a
        # clean one, and nothing else here can tell "the tree is fine" from "the root was wrong".
        lines.append("VACUOUS no .ps1/.yml/.yaml/.js file was found at all - the scan proves "
                     "nothing. Check the root path.")
        return lines, False

    ok = not violations
    lines.append("CLEAN every -unattended launch also passes -RunningUnattendedScript" if ok
                 else "FAILED %d launch(es) pass -unattended alone" % len(violations))
    return lines, ok


def default_root():
    """The plugin root, two directories up from Content/Python/."""
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Scan command files for editor launches that pass -unattended without "
                    "-RunningUnattendedScript, a pairing whose absence lets a startup Slate modal "
                    "wedge the game thread silently. Needs no editor and no build.")
    parser.add_argument("roots", nargs="*", default=None,
                        help="directories to scan (default: the plugin root above this script)")
    args = parser.parse_args(argv)

    roots = args.roots or [default_root()]
    violations, files = scan_tree(roots)
    lines, ok = format_report(violations, files, roots)
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
