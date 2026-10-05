# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml"]
# ///
"""Self-check for pw_issues.py priority handling. Offline: --fake-data reads, --dry-run writes.

Run from the plugin directory: uv run scripts/test_pw_issues.py
"""
import contextlib
import io
import json
import os
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pw_issues as pw  # noqa: E402


def issue(n, title, sev, meta_lines, text="Body text.", priority_line=""):
    body = "<!-- pinwright\n" + "".join(f"{l}\n" for l in meta_lines) + "-->\n\n"
    body += (priority_line + "\n\n" if priority_line else "") + text + "\n"
    labels = [{"name": "status/accepted"}, {"name": "type/bug"}]
    if sev:
        labels.append({"name": f"sev/{sev}"})
    return {"number": n, "node_id": f"I_{n}", "title": title, "state": "open", "body": body,
            "author_association": "OWNER", "labels": labels}


FIXTURE = {"issues": [
    issue(1, "Old unscored crash", "critical", ["id: B-old", "encounters: 9"]),
    issue(2, "Low scored item", "low", ["id: B-low", "encounters: 1", "rice: [3, 2, 1, 1]",
                                         "priority: 50"],
          priority_line="**Priority:** 50 (RICE R3 I2 C1 E1)"),
    issue(3, "High floored item", "high", ["id: B-high", "encounters: 1",
                                           "rice: [1, 1, 0.5, 3]", "priority: 90"]),
    issue(4, "Medium tie a", "medium", ["id: B-tie-a", "encounters: 2", "rice: [3, 2, 1, 1]",
                                        "priority: 50"]),
], "comments": {}, "issueFields": [
    {"node_id": "IFSS_x", "name": "Priority", "data_type": "single_select"},
    {"node_id": "IFN_rice", "name": "RICE priority", "data_type": "number"},
]}
NO_FIELD = dict(FIXTURE, issueFields=[])


def run(fixture_path, *argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        code = pw.main(["--fake-data", fixture_path, "--dry-run", *argv])
    return code, out.getvalue()


def patched_body(output):
    for line in output.splitlines():
        if line.startswith("[dry-run]   payload: "):
            payload = json.loads(line[len("[dry-run]   payload: "):])
            if "body" in payload:
                return payload["body"]
    raise AssertionError("no body payload in:\n" + output)


def field_writes(output):
    """numberValue (or None for a clear) of every setIssueFieldValue the dry-run printed."""
    vals = []
    for line in output.splitlines():
        if line.startswith("[dry-run]   payload: ") and "setIssueFieldValue" in line:
            q = json.loads(line[len("[dry-run]   payload: "):])["query"]
            assert 'fieldId: "IFN_rice"' in q, q
            vals.append(None if "delete: true" in q else int(q.split("numberValue: ")[1].split("}")[0]))
    return vals


def main():
    # Formula: score = R x Iw x C / E, priority = round(100 x score / 12), floor 90 for High+.
    assert pw.priority_of([3, 3, 1, 1], "medium") == 100
    assert pw.priority_of([1, 1, 0.5, 3], "low") == 1
    assert pw.priority_of([2, 2, 1, 1], "medium") == 33
    assert pw.priority_of([3, 1, 1, 2], "low") == 12  # 12.5 rounds half to even, as migrated
    assert pw.priority_of([2, 3, 0.8, 2], "none") == 27
    assert pw.priority_of([1, 1, 1, 3], "high") == 90
    assert pw.priority_of([1, 1, 1, 3], "critical") == 90
    assert pw.priority_of([3, 3, 1, 1], "high") == 100
    assert pw.parse_rice("2,3,0.8,1") == [2, 3, 0.8, 1]
    for bad in ("0,1,1,1", "1,4,1,1", "1,1,0.7,1", "1,1,1", "a,b,c,d", "1,1,1,4"):
        try:
            pw.parse_rice(bad)
        except pw.PwError as e:
            assert e.code == 2
        else:
            raise AssertionError(f"accepted bad rice {bad}")

    fd, path = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(FIXTURE, f)
    fd, nofield = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(NO_FIELD, f)
    try:
        # list: priority desc, ties by severity, unscored last whatever its severity.
        code, out = run(path, "list", "--json")
        rows = json.loads(out)
        assert code == 0 and [r["number"] for r in rows] == [3, 4, 2, 1], rows
        assert rows[0]["priority"] == 90 and rows[0]["rice"] == [1, 1, 0.5, 3]
        assert rows[3]["priority"] is None
        code, out = run(path, "list")
        assert out.splitlines()[0].startswith("#3\tp=90\thigh\t"), out
        assert out.splitlines()[3].startswith("#1\tp=-\tcritical\t"), out

        # file: a new issue needs --rice; with it, rice/priority are written and the field set.
        code, out = run(path, "file", "--title", "Brand new defect", "--type", "bug",
                        "--body", "Details.")
        assert code == 2 and "--rice" in out, out
        code, out = run(path, "file", "--title", "Brand new defect", "--type", "bug",
                        "--severity", "high", "--rice", "1,2,1,2", "--body", "Details.")
        body = patched_body(out)
        assert code == 0
        assert "rice: [1, 2, 1, 2]\npriority: 90\n" in body, body
        assert body.endswith("-->\n\nDetails.") and "**Priority:**" not in body, body
        assert "History" not in body
        assert field_writes(out) == [90], out
        code, out = run(path, "file", "--title", "Harness gap", "--type", "bug",
                        "--area", "harness", "--body", "x")
        assert code == 0 and "**Priority:**" not in patched_body(out) and field_writes(out) == []

        # file dedupe: --rice replaces rice, recomputes priority, strips the legacy line.
        code, out = run(path, "file", "--title", "Low scored item", "--type", "bug",
                        "--rice", "1,1,0.8,1", "--body", "Again.")
        body = patched_body(out)
        assert "encounters: 2" in body and "rice: [1, 1, 0.8, 1]" in body and "priority: 7" in body
        assert body.endswith("-->\n\nBody text.\n"), body
        assert field_writes(out) == [7] and 'issueId: \\"I_2\\"' in out, out

        # meta: setting rice recomputes priority (High floor) and sets the field, no body line.
        code, out = run(path, "meta", "3", "--set", "rice=[2,3,1,1]")
        body = patched_body(out)
        assert "priority: 90" in body and "**Priority:**" not in body and field_writes(out) == [90]
        code, out = run(path, "meta", "4", "--set", "rice=2,3,1,1")
        body = patched_body(out)
        assert body.endswith("-->\n\nBody text.\n") and field_writes(out) == [67], out
        # a missing field warns once and does not fail the command.
        code, out = run(nofield, "meta", "4", "--set", "rice=2,3,1,1")
        assert code == 0 and "priority: 67" in patched_body(out), out
        assert out.count('no number issue field "RICE priority"') == 1 and field_writes(out) == []
        code, out = run(path, "meta", "4", "--set", "priority=99")
        assert code == 2 and "derived" in out
        code, out = run(path, "meta", "4", "--set", "rice=[1,1,2,1]")
        assert code == 2
        code, out = run(path, "meta", "2", "--unset", "rice")
        body = patched_body(out)
        assert "priority" not in body and "**Priority:**" not in body
        assert body.endswith("-->\n\nBody text.\n"), body
        assert field_writes(out) == [None], out

        # meta --sync-field: the backfill; field from metadata, legacy line stripped.
        code, out = run(path, "meta", "2", "--sync-field")
        assert code == 0 and patched_body(out).endswith("-->\n\nBody text.\n"), out
        assert field_writes(out) == [50], out
        code, out = run(path, "meta", "4", "--sync-field")
        assert code == 0 and '"body"' not in out and field_writes(out) == [50], out
        code, out = run(path, "meta", "1", "--sync-field")
        assert code == 0 and field_writes(out) == [None], out

        # label: a severity change recomputes the floor and sets the field.
        code, out = run(path, "label", "3", "--add", "sev/low")
        body = patched_body(out)
        assert "priority: 1\n" in body and field_writes(out) == [1], out
        code, out = run(path, "label", "2", "--add", "sev/high")
        assert "priority: 90" in patched_body(out) and "**Priority:**" not in patched_body(out)
        assert field_writes(out) == [90], out
        code, out = run(path, "label", "4", "--add", "costly")
        assert '"body"' not in out and field_writes(out) == [], out  # priority unchanged

        # show: the priority is in the header line.
        code, out = run(path, "show", "4")
        assert out.splitlines()[0] == "#4 Medium tie a   priority: 50", out

        # edit: replacing the body keeps the metadata block and drops the legacy line.
        code, out = run(path, "edit", "2", "--body", "New text.")
        body = patched_body(out)
        assert body.endswith("priority: 50\n-->\n\nNew text."), body
        code, out = run(path, "edit", "1", "--body", "New text.")
        assert "**Priority:**" not in patched_body(out)
    finally:
        os.unlink(path)
        os.unlink(nofield)
    print("pw_issues self-check: OK")


if __name__ == "__main__":
    main()
