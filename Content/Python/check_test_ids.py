#!/usr/bin/env python3
# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Host-independent static scan for automation test ids that kill each other by dot-prefix.

THE DEFECT, IN BOTH DIRECTIONS. `FAutomationReport::EnsureReportExists` splits a test id on the
first `.` and matches an existing node by full path without caring whether that node is a leaf.
Register `A.B` and then `A.B.C` and the LEAF `A.B` is adopted as the branch node holding `C`;
every "is this runnable?" check in the controller is written `ChildReports.Num() == 0`, so `A.B`
then produces no result of any kind -- not a pass, not a fail, not a skip -- and it is absent
from the `<N> tests performed` count rather than failing it.

The direction people check is "is my new id a prefix of something that already exists?". The
direction that actually keeps reproducing is the REVERSE one: **adding a new suffixed id under an
id that is itself a complete leaf silently kills that existing leaf.** Both are the same pair, so
this scan reports the pair rather than a direction, and the fix is always the same -- give the
SHORTER id a leaf suffix naming what it asserts.

WHY THIS EXISTS ALONGSIDE THE RUNTIME GUARD, NOT INSTEAD OF IT.
`PinWright.infra.automation_registry.NoPrefixCollisions`
(`Source/PinWright/Private/Tests/Infra/TestAutomationTestIdPrefixCollisions.cpp`) walks
`FAutomationTestFramework::GetValidTestNames`, which is the set of ids THIS host would run right
now. Two whole classes of id are therefore invisible to it: anything inside a `#if` block the
host compiles out, and every id of an integration sub-module whose engine plugin is disabled on
that host. This scan reads source text, so it sees those -- and it needs no editor, no engine and
no build, which is what lets it run in CI on a machine with neither. Neither check subsumes the
other; keep both.

CONDITIONAL COMPILATION IS NOT EVALUATED, AND THAT IS THE POINT. This scan does not run a
preprocessor and does not pretend to know which `#if` branches a given host takes. Every id
literal in the tree is collected, guarded or not. The consequences, stated rather than implied:

  * It CANNOT miss an id behind a false `#if` -- exactly the runtime guard's blind spot.
  * It CAN flag a pair that no single build configuration would ever register together (two ids
    in mutually exclusive `#if` branches). That over-report is the safe direction: a dot-prefix
    id is a naming defect in the tree whichever host compiles it, the rename that fixes it is
    correct in every configuration, and a scan that guessed at preprocessor state would fail
    silently on the guess instead of loudly on the name.
  * Comments ARE removed, because `//` and block comments are lexical rather than preprocessor
    state -- a commented-out macro is not a registration. `#if 0` blocks are NOT removed, for the
    reason above.

Ids that merely appear as string data (the `PinWright.*` literals the skip-marker tests build
their fixtures from, for instance) are not registrations and are not collected: only a literal in
the argument list of an `IMPLEMENT_*AUTOMATION_TEST*` macro counts.

Pure stdlib, like `check_suite_log.py` beside it, so Unreal's bundled interpreter can run it as
shipped. Never `uv`, and never install anything into that interpreter:

    "%UE_ROOT%\\Engine\\Binaries\\ThirdParty\\Python3\\Win64\\python" -m check_test_ids [ROOT ...]

ROOT defaults to the plugin root inferred from this file's location. Exit code is 0 only when the
scan found ids at all, parsed every macro site it found, and reported no collision and no
duplicate.
"""
import argparse
import os
import re
import sys

# Directories never scanned: build output, packaged output, and the editor's own scratch. A copy
# of the tree under any of these would double every id and report the whole suite as duplicated.
EXCLUDED_DIRS = frozenset({"dist", "Binaries", "Intermediate", "Saved", ".git", "__pycache__"})

SOURCE_SUFFIXES = (".cpp", ".h", ".hpp", ".inl", ".cc", ".cxx")

# Matches every IMPLEMENT_*AUTOMATION_TEST* spelling the engine defines -- SIMPLE, COMPLEX,
# CUSTOM_SIMPLE, CUSTOM_COMPLEX, NETWORKED, BDD, and the _PRIVATE forms -- rather than only the
# SIMPLE one this tree happens to use today, so a first use of another variant is covered on the
# day it lands instead of silently escaping the scan.
_MACRO_RE = re.compile(r"\bIMPLEMENT_[A-Z0-9_]*AUTOMATION_TEST[A-Z0-9_]*\s*\(")

_STRING_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')

# How far ahead a `'` may look for its closing quote before it is treated as a digit separator
# (`1'000'000`) rather than a char literal. The longest char literal that matters is '\x41'.
_CHAR_LITERAL_SPAN = 8


def strip_comments(source):
    """Blank `//` and block comments, preserving every byte offset and every newline.

    Offsets are preserved so a match position still maps to the original line number, and
    newlines are preserved so a multi-line block comment does not shift that count. String and
    char literals are walked rather than skipped, so a `//` inside a literal survives.
    """
    out = []
    index = 0
    length = len(source)
    while index < length:
        char = source[index]
        if char == '"':
            out.append(char)
            index += 1
            while index < length:
                current = source[index]
                out.append(current)
                index += 1
                if current == "\\" and index < length:
                    out.append(source[index])
                    index += 1
                    continue
                if current == '"' or current == "\n":
                    break
            continue
        if char == "'":
            closing = source.find("'", index + 1, index + 1 + _CHAR_LITERAL_SPAN)
            if closing == -1:
                # A digit separator or a stray apostrophe, not a literal. Copy it and move on.
                out.append(char)
                index += 1
                continue
            out.append(source[index:closing + 1])
            index = closing + 1
            continue
        if char == "/" and index + 1 < length and source[index + 1] == "/":
            while index < length and source[index] != "\n":
                out.append(" ")
                index += 1
            continue
        if char == "/" and index + 1 < length and source[index + 1] == "*":
            out.append("  ")
            index += 2
            while index < length:
                if source[index] == "*" and index + 1 < length and source[index + 1] == "/":
                    out.append("  ")
                    index += 2
                    break
                out.append("\n" if source[index] == "\n" else " ")
                index += 1
            continue
        out.append(char)
        index += 1
    return "".join(out)


def _argument_span(source, open_paren):
    """The text between the macro's `(` and its matching `)`, or None if unbalanced."""
    depth = 0
    index = open_paren
    length = len(source)
    while index < length:
        char = source[index]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return source[open_paren + 1:index]
        index += 1
    return None


def _first_literal(span):
    """The macro's pretty-name argument: the first string literal in its argument list.

    Every engine spelling puts the id at a different argument index -- 2 for SIMPLE / COMPLEX /
    NETWORKED / BDD, 3 for the CUSTOM_* and _PRIVATE forms -- but in all of them the preceding
    arguments are class-name identifiers and the following ones are flag expressions, so the
    FIRST string literal is the id in every variant. Adjacent literals are concatenated the way
    the C++ compiler would.
    """
    match = _STRING_RE.search(span)
    if match is None:
        return None
    text = match.group(1)
    end = match.end()
    while True:
        following = _STRING_RE.search(span, end)
        if following is None or span[end:following.start()].strip():
            break
        text += following.group(1)
        end = following.end()
    return text


def scan_text(source, path):
    """One record per IMPLEMENT_*AUTOMATION_TEST* site in already-comment-stripped text.

    Returns (records, unparsed). `unparsed` holds sites whose id could not be read; they are
    reported as errors rather than dropped, because an unparsed site is an UNCHECKED id and
    silently under-reporting is the failure mode this whole file exists to prevent.
    """
    records = []
    unparsed = []
    for match in _MACRO_RE.finditer(source):
        line = source.count("\n", 0, match.start()) + 1
        span = _argument_span(source, match.end() - 1)
        test_id = _first_literal(span) if span is not None else None
        site = {"path": path, "line": line, "macro": match.group(0).rstrip("( \t\r\n")}
        if test_id:
            site["id"] = test_id
            records.append(site)
        else:
            unparsed.append(site)
    return records, unparsed


def scan_tree(roots):
    """Walk `roots` and collect every automation test id declared under them."""
    records = []
    unparsed = []
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
                        text = handle.read()
                except OSError as error:
                    unparsed.append({"path": path, "line": 0, "macro": "<unreadable>",
                                     "error": str(error)})
                    continue
                if "AUTOMATION_TEST" not in text:
                    continue
                files += 1
                found, bad = scan_text(strip_comments(text), path)
                records.extend(found)
                unparsed.extend(bad)
    return records, unparsed, files


def find_collisions(records):
    """Return (dot_prefix_pairs, duplicate_groups).

    Comparison folds case exactly as the engine does: `FAutomationReport::EnsureReportExists`
    matches with `FString::operator==` (IgnoreCase) and the controller sorts the batch with
    `FString::operator<` (Stricmp) before inserting, so `PinWright.foo` really does adopt
    `PinWright.FOO.Bar`. A case-sensitive walk here would miss that pair and report a false green.

    Only a `.` collides. `create_node` vs `create_node_guard` is a plain string prefix that never
    reaches the engine's split, and both members run -- hence the explicit `.` below rather than a
    bare startswith, and hence the inner loop CONTINUING past such a neighbour rather than
    breaking on it.
    """
    ordered = sorted(records, key=lambda record: (record["id"].casefold(),
                                                  record["path"], record["line"]))

    groups = {}
    for record in ordered:
        groups.setdefault(record["id"].casefold(), []).append(record)
    duplicates = [sites for sites in groups.values() if len(sites) > 1]

    pairs = []
    seen = set()
    for i, shorter in enumerate(ordered):
        key = shorter["id"].casefold()
        prefix = key + "."
        # Sorted, so every id beginning with `key` is contiguous after it; stop at the first
        # neighbour that does not begin with it. Near-linear over ~5k ids.
        for j in range(i + 1, len(ordered)):
            longer = ordered[j]
            other = longer["id"].casefold()
            if not other.startswith(key):
                break
            if not other.startswith(prefix):
                continue
            signature = (key, other)
            if signature in seen:
                continue
            seen.add(signature)
            pairs.append((shorter, longer))
    return pairs, duplicates


def _site(record):
    return "%s:%d" % (record["path"], record["line"])


def format_report(records, unparsed, files, roots):
    """Build the human-readable report. Returns (lines, ok)."""
    lines = []
    pairs, duplicates = find_collisions(records)

    for shorter, longer in pairs:
        lines.append(
            "COLLISION '%s' (%s) is a dot-prefix of '%s' (%s)"
            % (shorter["id"], _site(shorter), longer["id"], _site(longer)))
        lines.append(
            "    '%s' is adopted as a branch node by FAutomationReport::EnsureReportExists and "
            "NEVER EXECUTES - no pass, no fail, no skip, and it is absent from the "
            "'<N> tests performed' count rather than failing it." % shorter["id"])
        lines.append(
            "    Fix: rename the SHORTER id to a leaf naming what it asserts, e.g. "
            "'%s.<WhatItAsserts>', and update every reference to the old id. Whichever of the two "
            "was added last, the id that dies is the shorter one." % shorter["id"])

    for sites in duplicates:
        lines.append("DUPLICATE '%s' declared at %s"
                     % (sites[0]["id"], ", ".join(_site(site) for site in sites)))

    for site in unparsed:
        lines.append("UNPARSED %s at %s - id literal not found, so this id is UNCHECKED"
                     % (site.get("macro", "<macro>"), _site(site)))

    unique = len({record["id"].casefold() for record in records})
    lines.append("SCANNED %d id(s), %d unique, in %d file(s) under %s"
                 % (len(records), unique, files, ", ".join(roots)))

    if not records:
        # Fail closed. An empty scan greps clean exactly like a clean one, and this checker has no
        # other way to tell "the tree is fine" from "the root argument was wrong".
        lines.append("VACUOUS no automation test ids found at all - the scan proves nothing. "
                     "Check the root path.")
        return lines, False

    ok = not pairs and not duplicates and not unparsed
    lines.append("CLEAN no dot-prefix collisions, no duplicate ids" if ok
                 else "FAILED %d collision(s), %d duplicate id(s), %d unparsed site(s)"
                      % (len(pairs), len(duplicates), len(unparsed)))
    return lines, ok


def default_root():
    """The plugin root, two directories up from Content/Python/."""
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Scan source for IMPLEMENT_*AUTOMATION_TEST* ids and fail on any id that is "
                    "a dot-prefix of another (the shorter one is adopted as a branch node and "
                    "never executes) or is declared twice. Needs no editor and no build.")
    parser.add_argument("roots", nargs="*", default=None,
                        help="directories to scan (default: the plugin root above this script)")
    args = parser.parse_args(argv)

    roots = args.roots or [default_root()]
    records, unparsed, files = scan_tree(roots)
    lines, ok = format_report(records, unparsed, files, roots)
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
