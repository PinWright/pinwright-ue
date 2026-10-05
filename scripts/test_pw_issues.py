# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml"]
# ///
"""Self-check for pw_issues.py classification. Offline: --fake-data reads, --dry-run writes.

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

OPTIONS = {"Severity": ["Critical", "High", "Medium", "Low"],
           "Reach": ["1 Rare", "2 Occasional", "3 Common"],
           "Impact": ["1 Friction", "2 Wasted time", "3 Wrong data"],
           "Confidence": ["1.0 Verified", "0.8 Likely", "0.5 Unconfirmed"],
           "Effort": ["1 Small", "2 Medium", "3 Large"]}
OPT_NAME = {f: {pw._lead(o) if f != "Severity" else o: o for o in opts} for f, opts in OPTIONS.items()}


def issue(n, title, meta_lines, sev=None, rice=None, priority=None, type_="Bug", labels=(),
          text="Body text.", priority_line=""):
    body = "<!-- pinwright\n" + "".join(f"{l}\n" for l in meta_lines) + "-->\n\n"
    body += (priority_line + "\n\n" if priority_line else "") + text + "\n"
    values = [{"issue_field_name": "Severity", "value": sev}] if sev else []
    for f, v in zip(pw.RICE_FIELDS, rice or ()):
        values.append({"issue_field_name": f, "value": OPT_NAME[f][v]})
    if priority is not None:
        values.append({"issue_field_name": "RICE priority", "value": priority})
    return {"number": n, "node_id": f"I_{n}", "title": title, "state": "open", "body": body,
            "author_association": "OWNER", "type": {"name": type_} if type_ else None,
            "issue_field_values": values,
            "labels": [{"name": l} for l in ("status/accepted", *labels)]}


FIXTURE = {"issues": [
    issue(1, "Old unscored crash", ["id: B-old", "encounters: 9"], sev="Critical"),
    issue(2, "Low scored item", ["id: B-low", "encounters: 1"], sev="Low", rice=[3, 2, 1, 1],
          priority=50),
    issue(3, "High floored item", ["id: B-high", "encounters: 1"], sev="High",
          rice=[1, 1, 0.5, 3], priority=90),
    issue(4, "Medium tie a", ["id: B-tie-a", "encounters: 2"], sev="Medium", rice=[3, 2, 1, 1],
          priority=50),
    # Not migrated yet: type and severity are labels, rice and priority are metadata.
    issue(5, "Legacy item", ["id: B-legacy", "encounters: 1", "rice: [1, 2, 1, 2]",
                             "priority: 90"], type_=None, labels=("type/bug", "sev/high"),
          priority_line="**Priority:** 90 (RICE R1 I2 C1 E2)"),
    issue(6, "Legacy harness item", ["id: H-legacy", "category: feature"], type_=None,
          labels=("area/harness", "sev/low")),
    issue(7, "Legacy stale priority", ["id: B-stale", "rice: [1, 1, 1, 1]", "priority: 99"],
          type_=None, labels=("type/ergonomic",)),
], "comments": {}, "issueFields": [
    {"id": "F_rice", "name": "RICE priority"},
    *({"id": f"F_{f}", "name": f, "options": [{"id": f"O_{f}_{o}", "name": o} for o in opts]}
      for f, opts in OPTIONS.items()),
]}
NO_FIELD = dict(FIXTURE, issueFields=[])


def run(fixture_path, *argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        code = pw.main(["--fake-data", fixture_path, "--dry-run", *argv])
    return code, out.getvalue()


def payloads(output):
    return [json.loads(l[len("[dry-run]   payload: "):]) for l in output.splitlines()
            if l.startswith("[dry-run]   payload: ")]


def patched(output, key="body"):
    for p in payloads(output):
        if key in p:
            return p[key]
    raise AssertionError(f"no {key} payload in:\n" + output)


def field_writes(output):
    """{field: printed value} of the setIssueFieldValue calls the dry-run made."""
    vals = {}
    for line in output.splitlines():
        if " fields: " in line and line.startswith("#"):
            for kv in line.split(" fields: ", 1)[1].split(", "):
                k, v = kv.split("=", 1)
                vals[k] = v
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
    # Field values read back as RICE numbers.
    assert pw.rice_of(pw.current_fields(FIXTURE["issues"][2])) == [1, 1, 0.5, 3]
    assert pw.rice_of(pw.current_fields(FIXTURE["issues"][0])) is None
    # GitHub may put a single-select option's name in single_select_option; it wins.
    assert pw.current_fields({"issue_field_values": [
        {"issue_field_name": "Reach", "value": "x", "single_select_option": {"name": "2 Occasional"}},
        {"issue_field_name": "Severity", "value": None, "single_select_option": {"name": "Low"}},
    ]}) == {"Reach": 2, "Severity": "Low"}

    fd, path = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(FIXTURE, f)
    fd, nofield = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(NO_FIELD, f)
    try:
        # list: priority field desc, ties by Severity field, unscored last whatever its labels.
        code, out = run(path, "list", "--json")
        rows = json.loads(out)
        assert code == 0 and [r["number"] for r in rows] == [3, 4, 2, 1, 5, 7], rows
        r = rows[0]
        assert (r["priority"], r["type"], r["severity"], r["reach"], r["impact"],
                r["confidence"], r["effort"]) == (90, "Bug", "high", 1, 1, 0.5, 3), r
        assert rows[3]["priority"] is None and rows[4]["severity"] == "none"
        code, out = run(path, "list")
        assert out.splitlines()[0].startswith("#3\tp=90\tBug\thigh\t"), out
        assert out.splitlines()[3].startswith("#1\tp=-\tBug\tcritical\t"), out

        # file: a new issue needs --rice; with it, the type is set on create and the fields after.
        code, out = run(path, "file", "--title", "Brand new defect", "--type", "bug",
                        "--body", "Details.")
        assert code == 2 and "--rice" in out, out
        code, out = run(path, "file", "--title", "Brand new defect", "--type", "bug",
                        "--severity", "high", "--rice", "1,2,1,2", "--body", "Details.")
        create = payloads(out)[0]
        assert code == 0 and create["type"] == "Bug" and create["labels"] == ["status/accepted"]
        assert "rice" not in create["body"] and "priority" not in create["body"], create
        assert create["body"].endswith("-->\n\nDetails."), create
        assert field_writes(out) == {"Severity": "High", "Reach": "1", "Impact": "2",
                                     "Confidence": "1", "Effort": "2", "RICE priority": "90"}, out
        q = payloads(out)[1]["query"]
        assert 'singleSelectOptionId: "O_Severity_High"' in q and "numberValue: 90" in q, q
        code, out = run(path, "file", "--title", "Engine 5.9 load failure", "--type",
                        "compatibility", "--rice", "1,3,0.5,2", "--body", "x")
        assert code == 0 and payloads(out)[0]["type"] == "Compatibility" and "id: C-" in out, out
        code, out = run(path, "file", "--title", "Harness gap", "--type", "bug",
                        "--area", "harness", "--body", "x")
        assert code == 0 and field_writes(out) == {} and len(payloads(out)) == 1, out

        # file dedupe: --rice replaces the RICE fields that differ and recomputes priority.
        code, out = run(path, "file", "--title", "Low scored item", "--type", "bug",
                        "--rice", "1,1,0.8,1", "--body", "Again.")
        body = patched(out)
        assert "encounters: 2" in body and "rice" not in body, body
        assert field_writes(out) == {"Reach": "1", "Impact": "1", "Confidence": "0.8",
                                     "RICE priority": "7"}, out
        assert 'issueId: \\"I_2\\"' in out, out

        # score: RICE recompute with the High floor; severity change; type; no-op.
        code, out = run(path, "score", "3", "--rice", "2,3,1,1")
        assert code == 0 and field_writes(out) == {"Reach": "2", "Impact": "3", "Confidence": "1",
                                                   "Effort": "1"}, out  # still 90
        code, out = run(path, "score", "3", "--severity", "low")
        assert field_writes(out) == {"Severity": "Low", "RICE priority": "1"}, out
        code, out = run(path, "score", "2", "--severity", "high")
        assert field_writes(out) == {"Severity": "High", "RICE priority": "90"}, out
        code, out = run(path, "score", "4", "--type", "ergonomic")
        assert payloads(out) == [{"type": "Ergonomic"}] and field_writes(out) == {}, out
        code, out = run(path, "score", "2", "--severity", "low", "--type", "bug")
        assert code == 0 and "[dry-run]" not in out, out
        code, out = run(path, "score", "4")
        assert code == 2
        code, out = run(path, "score", "4", "--rice", "1,1,2,1")
        assert code == 2
        code, out = run(nofield, "score", "4", "--severity", "high")
        assert code == 1 and 'no issue field "Severity"' in out, out
        # An unmigrated issue is refused before any write: its RICE fields are still unset.
        code, out = run(path, "score", "5", "--severity", "low")
        assert code == 2 and "migrate 5" in out and "[dry-run]" not in out, out
        code, out = run(path, "file", "--title", "x", "--type", "bug", "--into", "5",
                        "--severity", "low", "--body", "y")
        assert code == 2 and "migrate 5" in out and "[dry-run]" not in out, out

        # meta refuses rice and priority; other keys still work.
        for argv in (["--set", "rice=[2,3,1,1]"], ["--set", "priority=99"], ["--unset", "rice"]):
            code, out = run(path, "meta", "4", *argv)
            assert code == 2 and "score" in out, out
        code, out = run(path, "meta", "4", "--set", "costly=1")
        assert code == 0 and "costly: 1\n" in patched(out) and field_writes(out) == {}

        # label refuses type/* and sev/*; other labels still work.
        for argv in (["--add", "sev/low"], ["--remove", "type/bug"]):
            code, out = run(path, "label", "4", *argv)
            assert code == 2 and "score" in out, out
        code, out = run(path, "label", "4", "--add", "costly")
        assert code == 0 and '"body"' not in out and field_writes(out) == {}, out

        # migrate: labels and metadata move to the type and fields, then the labels go.
        code, out = run(path, "migrate", "5")
        assert code == 0 and "warning" not in out, out
        assert field_writes(out) == {"Severity": "High", "Reach": "1", "Impact": "2",
                                     "Confidence": "1", "Effort": "2", "RICE priority": "90"}, out
        assert patched(out, "type") == "Bug"
        body = patched(out)
        assert body == "<!-- pinwright\nid: B-legacy\nencounters: 1\n-->\n\nBody text.\n", body
        assert out.count("-X DELETE") == 2 and "sev%2Fhigh" in out and "type%2Fbug" in out, out
        code, out = run(path, "migrate", "6")  # harness: type from metadata category
        assert payloads(out)[-1] == {"type": "Feature"}, out
        assert field_writes(out) == {"Severity": "Low"} and "sev%2Flow" in out, out
        code, out = run(path, "migrate", "7")
        assert "warning: #7 metadata priority 99 != computed 8" in out, out
        # idempotent: an already migrated issue gets no write at all.
        code, out = run(path, "migrate", "2")
        assert code == 0 and "[dry-run]" not in out, out

        # show: type, severity, RICE inputs and priority in the header line.
        code, out = run(path, "show", "4")
        assert out.splitlines()[0] == ("#4 Medium tie a   type: Bug   severity: medium   "
                                       "rice: 3,2,1,1   priority: 50"), out
        code, out = run(path, "show", "1")
        assert out.splitlines()[0].endswith("rice: -,-,-,-   priority: -"), out

        # edit: replacing the body keeps the metadata block and drops the legacy line.
        code, out = run(path, "edit", "5", "--body", "New text.")
        body = patched(out)
        assert body.endswith("priority: 90\n-->\n\nNew text.") and "**Priority:**" not in body
    finally:
        os.unlink(path)
        os.unlink(nofield)
    print("pw_issues self-check: OK")


if __name__ == "__main__":
    main()
