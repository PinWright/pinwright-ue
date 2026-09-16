# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Self-test for `check_test_ids.py`, the host-independent dot-prefix scan.

WHY A SELF-TEST AND NOT JUST THE SCAN. The scan's whole value is that it fails when the tree is
wrong; a scan that can no longer fail greps identically to a clean tree. The two fixtures that
matter are a pair:

  * a real collision -- `…create_node` plus `…create_node.RefusalLeavesPackageClean`, the exact
    shape that killed a test on 2026-08-28 -- must be flagged;
  * a letter-extension -- `…create_node` plus `…create_node_guard.…`, the known false-positive
    shape -- must NOT be, because it never reaches the engine's split on `.` and both members run.

A scan that flags nothing and a scan that flags everything both look like a passing gate from the
outside. These two hold it between them.

Pure stdlib; no editor, no socket, no `uv`. Run under Unreal's bundled interpreter from
Content/Python:

    "%UE_ROOT%\\Engine\\Binaries\\ThirdParty\\Python3\\Win64\\python.exe" -m unittest discover tests

or explicitly:

    ...\\python.exe -m unittest tests.test_test_id_prefix_scan
"""

import contextlib
import io
import os
import sys
import tempfile
import unittest

# check_test_ids.py lives one directory up (Content/Python/).
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import check_test_ids  # noqa: E402

# A namespace that exists in no real test file, so a fixture can never be confused with a tree id.
NS = "PinWright.probe.graph"


def _macro(test_id, class_name="FProbeTest"):
    """One registration in the exact multi-line shape the tree uses."""
    return ('IMPLEMENT_SIMPLE_AUTOMATION_TEST(%s,\n'
            '    "%s",\n'
            '    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)\n'
            % (class_name, test_id))


class _Tree(object):
    """A throwaway source tree. Files are written under a tempdir, never inside the plugin."""

    def __init__(self, files):
        self._temp = tempfile.TemporaryDirectory(prefix="pinwright-test-id-scan-")
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

    def scan(self):
        records, unparsed, files = check_test_ids.scan_tree([self.root])
        pairs, duplicates = check_test_ids.find_collisions(records)
        # The CLI is exercised alongside the API so the exit code -- the only thing CI reads --
        # is covered by every case below rather than by a separate happy-path test.
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            code = check_test_ids.main([self.root])
        return {"records": records, "unparsed": unparsed, "files": files,
                "pairs": pairs, "duplicates": duplicates,
                "exit": code, "output": captured.getvalue()}


class DotPrefixScanTests(unittest.TestCase):

    def test_dot_extension_kills_the_existing_leaf_and_is_flagged(self):
        """The 2026-08-28 regression: a NEW suffixed id added under an EXISTING complete leaf."""
        with _Tree({
            "Source/PinWright/Private/Tests/Niagara/TestExisting.cpp":
                _macro(NS + ".create_node", "FExisting"),
            "Source/PinWright/Private/Tests/Niagara/TestAdded.cpp":
                _macro(NS + ".create_node.RefusalLeavesPackageClean", "FAdded"),
        }) as tree:
            result = tree.scan()

        self.assertEqual(len(result["pairs"]), 1, result["pairs"])
        shorter, longer = result["pairs"][0]
        self.assertEqual(shorter["id"], NS + ".create_node")
        self.assertEqual(longer["id"], NS + ".create_node.RefusalLeavesPackageClean")
        self.assertTrue(shorter["path"].endswith("TestExisting.cpp"), shorter["path"])
        self.assertEqual(shorter["line"], 1)
        self.assertNotEqual(result["exit"], 0)

        # The report must name the id that DIES, and say which of the two it is -- the reverse
        # direction is what the author of the new id gets wrong.
        self.assertIn("NEVER EXECUTES", result["output"])
        self.assertIn("rename the SHORTER id", result["output"])
        self.assertIn("the id that dies is the shorter one", result["output"])

    def test_letter_extension_is_not_a_collision(self):
        """`create_node_guard` never reaches the engine's split on `.`; both members run."""
        with _Tree({
            "Source/PinWright/Private/Tests/Niagara/TestExisting.cpp":
                _macro(NS + ".create_node", "FExisting"),
            "Source/PinWright/Private/Tests/Niagara/TestGuard.cpp":
                _macro(NS + ".create_node_guard.RejectionLeavesGraphUnchanged", "FGuard"),
        }) as tree:
            result = tree.scan()

        self.assertEqual(result["pairs"], [])
        self.assertEqual(result["duplicates"], [])
        self.assertEqual(len(result["records"]), 2)
        self.assertEqual(result["exit"], 0)

    def test_letter_extension_does_not_hide_a_later_collision(self):
        """The sorted walk must step PAST a letter-extension neighbour, not stop at it.

        `create_node` < `create_node.Payload` < `create_node_guard` under a plain string sort, so
        an implementation that breaks out of the inner loop on the first non-dot extension would
        still see this pair -- but the reverse order would hide it. Both orders are asserted by
        having all three ids present at once.
        """
        with _Tree({
            "Source/PinWright/Private/Tests/Niagara/TestAll.cpp":
                _macro(NS + ".create_node", "FBare")
                + _macro(NS + ".create_node_guard.Rejects", "FGuard")
                + _macro(NS + ".create_node.Payload", "FPayload"),
        }) as tree:
            result = tree.scan()

        self.assertEqual([(short["id"], long_["id"]) for short, long_ in result["pairs"]],
                         [(NS + ".create_node", NS + ".create_node.Payload")])

    def test_case_folds_the_way_the_engine_folds(self):
        """`EnsureReportExists` compares IgnoreCase, so a case-differing pair still collides."""
        with _Tree({
            "Source/PinWright/Private/Tests/Niagara/TestCase.cpp":
                _macro(NS + ".create_node", "FBare")
                + _macro(NS + ".CREATE_NODE.Payload", "FPayload"),
        }) as tree:
            result = tree.scan()

        self.assertEqual(len(result["pairs"]), 1, result["pairs"])

    def test_duplicate_ids_are_reported(self):
        with _Tree({
            "Source/PinWright/Private/Tests/Niagara/TestOne.cpp":
                _macro(NS + ".create_node.Payload", "FOne"),
            "Source/PinWrightGeometry/Private/Tests/TestTwo.cpp":
                _macro(NS + ".create_node.Payload", "FTwo"),
        }) as tree:
            result = tree.scan()

        self.assertEqual(result["pairs"], [])
        self.assertEqual(len(result["duplicates"]), 1)
        self.assertEqual(len(result["duplicates"][0]), 2)
        self.assertNotEqual(result["exit"], 0)

    def test_ids_behind_a_false_if_guard_are_still_scanned(self):
        """The whole reason this exists next to the runtime guard.

        `#if WITH_SOMETHING` is not evaluated: the id is collected regardless, so a collision
        inside a block the current host compiles out is still caught. The runtime walk over
        `GetValidTestNames` cannot see this pair at all on such a host.
        """
        guarded = ("#if WITH_PROBE_FEATURE\n"
                   + _macro(NS + ".create_node.OnlyWithFeature", "FGuarded")
                   + "#endif\n")
        with _Tree({
            "Source/PinWright/Private/Tests/Niagara/TestExisting.cpp":
                _macro(NS + ".create_node", "FExisting"),
            "Source/PinWright/Private/Tests/Niagara/TestGuarded.cpp": guarded,
        }) as tree:
            result = tree.scan()

        self.assertEqual(len(result["pairs"]), 1, result["pairs"])
        self.assertEqual(result["pairs"][0][1]["id"], NS + ".create_node.OnlyWithFeature")
        self.assertEqual(result["pairs"][0][1]["line"], 2)

    def test_commented_out_registrations_are_not_registrations(self):
        """Comments are lexical, not preprocessor, state -- so they ARE removed."""
        with _Tree({
            "Source/PinWright/Private/Tests/Niagara/TestExisting.cpp":
                _macro(NS + ".create_node", "FExisting"),
            "Source/PinWright/Private/Tests/Niagara/TestCommented.cpp":
                "// " + _macro(NS + ".create_node.LineCommented", "FLine").replace("\n", "\n// ")
                + "\n/*\n" + _macro(NS + ".create_node.BlockCommented", "FBlock") + "*/\n",
        }) as tree:
            result = tree.scan()

        self.assertEqual(result["pairs"], [])
        self.assertEqual([record["id"] for record in result["records"]], [NS + ".create_node"])
        self.assertEqual(result["exit"], 0)

    def test_ids_used_as_string_data_are_not_registrations(self):
        """Skip-marker tests build fixture lines out of `PinWright.*` literals; those are data."""
        with _Tree({
            "Source/PinWright/Private/Tests/Niagara/TestExisting.cpp":
                _macro(NS + ".create_node", "FExisting"),
            "Source/PinWright/Private/Tests/Infra/TestSkipMarker.cpp":
                'const FString Line = TEXT("Path={%s.create_node.SomeTest}");\n' % NS,
        }) as tree:
            result = tree.scan()

        self.assertEqual(result["pairs"], [])
        self.assertEqual(len(result["records"]), 1)

    def test_a_url_in_a_string_literal_does_not_eat_the_file(self):
        """`//` inside a literal is not a comment; the naive strip would blank the rest of it."""
        with _Tree({
            "Source/PinWright/Private/Tests/Niagara/TestUrl.cpp":
                'const FString Doc = TEXT("https://example.invalid/spec");\n'
                + _macro(NS + ".create_node", "FExisting")
                + _macro(NS + ".create_node.Payload", "FPayload"),
        }) as tree:
            result = tree.scan()

        self.assertEqual(len(result["records"]), 2)
        self.assertEqual(len(result["pairs"]), 1)

    def test_build_output_directories_are_excluded(self):
        """`dist/` holds a whole second copy of the tree; scanning it duplicates every id."""
        collision = _macro(NS + ".create_node", "FExisting") \
            + _macro(NS + ".create_node.Payload", "FPayload")
        with _Tree({
            "Source/PinWright/Private/Tests/Niagara/TestExisting.cpp":
                _macro(NS + ".create_node", "FExisting"),
            "dist/snapshot/Source/PinWright/Private/Tests/Niagara/TestExisting.cpp": collision,
            "Intermediate/Build/copy.cpp": collision,
            "Binaries/Win64/copy.cpp": collision,
            "Saved/scratch/copy.cpp": collision,
        }) as tree:
            result = tree.scan()

        self.assertEqual(len(result["records"]), 1)
        self.assertEqual(result["exit"], 0)

    def test_a_macro_whose_id_cannot_be_read_is_an_error_not_a_silent_drop(self):
        """An unparsed site is an UNCHECKED id, which is the failure mode being guarded."""
        with _Tree({
            "Source/PinWright/Private/Tests/Niagara/TestIndirect.cpp":
                "IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIndirect, PROBE_ID_MACRO, Flags)\n"
                + _macro(NS + ".create_node", "FExisting"),
        }) as tree:
            result = tree.scan()

        self.assertEqual(len(result["unparsed"]), 1)
        self.assertEqual(result["unparsed"][0]["line"], 1)
        self.assertNotEqual(result["exit"], 0)

    def test_an_empty_tree_is_not_green(self):
        """A scan that found nothing proves nothing; it must not exit 0 like a clean one."""
        with _Tree({"Source/PinWright/Private/Nothing.cpp": "int Unused = 0;\n"}) as tree:
            result = tree.scan()

        self.assertEqual(result["records"], [])
        self.assertNotEqual(result["exit"], 0)

    def test_every_engine_macro_spelling_is_matched(self):
        """The id sits at argument 2 in some spellings and 3 in others; first literal wins."""
        source = (
            'IMPLEMENT_COMPLEX_AUTOMATION_TEST(FComplex, "%s.Complex", Flags)\n'
            'IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FCustom, FBase, "%s.Custom", Flags)\n'
            'IMPLEMENT_NETWORKED_AUTOMATION_TEST(FNet, "%s.Networked", Flags, 2)\n'
            'IMPLEMENT_BDD_AUTOMATION_TEST(FBdd, "%s.Bdd", Flags)\n' % (NS, NS, NS, NS))
        with _Tree({"Source/PinWright/Private/Tests/TestVariants.cpp": source}) as tree:
            result = tree.scan()

        self.assertEqual(sorted(record["id"] for record in result["records"]),
                         [NS + ".Bdd", NS + ".Complex", NS + ".Custom", NS + ".Networked"])
        self.assertEqual(result["unparsed"], [])

    def test_the_real_plugin_tree_parses_and_is_clean(self):
        """Non-vacuity plus the gate itself, on the tree this file ships in.

        The lower bound is deliberately loose -- the suite total moves with every added test, and
        a hardcoded exact figure here would go stale and start lying. What it pins is that the
        parser still matches the shape the tree actually writes: a regex that stopped matching
        would report zero ids and every fixture test above would still pass.
        """
        root = check_test_ids.default_root()
        records, unparsed, _files = check_test_ids.scan_tree([root])
        self.assertGreater(len(records), 4000, "parser matched almost nothing under %s" % root)
        self.assertEqual(unparsed, [], "unparsed macro sites leave those ids unchecked")

        pairs, duplicates = check_test_ids.find_collisions(records)
        self.assertEqual(
            [(short["id"], long_["id"]) for short, long_ in pairs], [],
            "a test id is a dot-prefix of another and therefore never executes")
        self.assertEqual([sites[0]["id"] for sites in duplicates], [])


if __name__ == "__main__":
    unittest.main()
