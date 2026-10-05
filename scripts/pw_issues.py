# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml"]
# ///
"""pw_issues.py - the one shared helper agents use to read and write PinWright GitHub Issues.

Run it as `uv run scripts/pw_issues.py <command> ...` (from the plugin directory). It shells
out to the GitHub CLI (`gh`), which must be installed and authenticated. Default repo is
PinWright/pinwright-ue; override with --repo or the PW_ISSUES_REPO environment variable.

Two rules this helper enforces, and that every agent workflow relies on:
  * Pick work only via `list`. It returns open issues labelled status/accepted, so nothing
    a maintainer has not accepted can reach an agent.
  * Read issue text only via `show`. It prints a body or comment only when its author is
    OWNER, MEMBER or COLLABORATOR; anything else is replaced by a placeholder line, and
    links or attachments from such authors are never printed.

Commands:
  list     open issues an agent may work, ranked priority > severity > costly > encounters
           > number (issues with no priority sort after those with one)
           (--closed [--since DATE]: issues closed as completed, for re-testing)
  show     one issue with its comments, trust-filtered
  claim    take the 4h lease on an issue (prints WON or LOST)
  release  give the lease back
  comment  add a comment
  close    comment, then close as completed / not_planned / duplicate (also releases)
  reopen   comment, then reopen
  file     file a new issue, or bump encounters on an existing match (dedupe); a new
           issue needs --rice R,I,C,E unless it is --area harness
  meta     edit fields of the hidden <!-- pinwright ... --> metadata block (setting rice
           recomputes priority; priority itself is derived and cannot be set);
           --sync-field alone re-pushes the metadata priority to the issue field
  label    add or remove labels (one sev/* label at a time: adding one drops the others;
           a severity change recomputes priority)

Priority: score = R x Iw x C / E with Iw = 1/2/4 for I = 1/2/3; priority = round(100 x
score / 12), at least 90 for sev/critical and sev/high. rice and priority live in the
metadata block (the source of truth for list); every write that changes priority also sets the
org number issue field "RICE priority" (cleared when rice is unset; a missing field only warns).
  edit     change the title, or replace the body text below the metadata block

Exit codes:
  0  success (claim: WON)
  1  gh or GitHub API error
  2  usage error or invalid input (e.g. a title longer than 80 characters)
  3  claim: LOST, another host holds a live lease
  4  issue not found, or it is a pull request
  5  file: the dedupe match is a closed issue; it was bumped and commented, and the caller
     decides whether to reopen it

Global flags:
  --dry-run          print every write (gh command plus JSON payload) instead of running it;
                     reads still run, and claim simulates its own comment when re-reading
  --fake-data FILE   answer reads from a JSON fixture instead of GitHub (for tests):
                     {"issues": [<REST issue>...], "comments": {"<n>": [<REST comment>...]},
                      "issueFields": [<REST org issue field: node_id, name, data_type>...]}
                     (no "issueFields" key means the org has no RICE priority field)
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from datetime import datetime, timedelta, timezone

import yaml

DEFAULT_REPO = "PinWright/pinwright-ue"
TRUSTED = {"OWNER", "MEMBER", "COLLABORATOR"}
SEVERITY_RANK = {"critical": 4, "high": 3, "medium": 2, "low": 1}
LEASE_HOURS = 4
TITLE_MAX = 80
TYPE_PREFIX = {"bug": "B", "feature": "F", "ergonomic": "E"}
IMPACT_WEIGHT = {1: 1, 2: 2, 3: 4}
CONFIDENCE = (1, 0.8, 0.5)
PRIORITY_FIELD = "RICE priority"

META_RE = re.compile(r"\A(\s*)<!-- pinwright\n(.*?)-->[ \t]*\n?", re.S)
CLAIM_RE = re.compile(r"^pinwright-claim host=(\S+)", re.M)
RELEASE_RE = re.compile(r"^pinwright-release host=(\S+)", re.M)
PRIORITY_LINE_RE = re.compile(r"\n*\*\*Priority:\*\* [^\n]*(\n+|\Z)")


class PwError(Exception):
    def __init__(self, msg: str, code: int = 1):
        super().__init__(msg)
        self.code = code


# --------------------------------------------------------------------------- time


def utc_now() -> datetime:
    return datetime.now(timezone.utc).replace(microsecond=0)


def iso(dt: datetime) -> str:
    return dt.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")


def parse_time(s: str) -> datetime:
    return datetime.fromisoformat(s.replace("Z", "+00:00"))


# --------------------------------------------------------------------------- gh layer


class Gh:
    """All GitHub access. Reads may come from a fixture; writes may be printed only."""

    def __init__(self, repo: str, dry_run: bool, fake_path: str | None):
        self.repo = repo
        self.dry_run = dry_run
        self.fake = None
        if fake_path:
            with open(fake_path, encoding="utf-8") as f:
                self.fake = json.load(f)
            self.fake.setdefault("issues", [])
            self.fake.setdefault("comments", {})
        self._fake_id = 10_000_000
        self._priority_field = False  # not looked up yet

    # -- process

    def _run(self, args: list[str], payload: dict | None = None) -> str:
        cmd = ["gh"] + args
        try:
            p = subprocess.run(
                cmd,
                input=json.dumps(payload) if payload is not None else None,
                capture_output=True, text=True, encoding="utf-8",
            )
        except FileNotFoundError:
            raise PwError("gh (GitHub CLI) is not installed or not on PATH")
        if p.returncode != 0:
            err = (p.stderr or p.stdout).strip()
            if "Not Found" in err or "HTTP 404" in err:
                raise PwError(f"not found: {' '.join(args)}: {err}", 4)
            raise PwError(f"gh {' '.join(args)} failed ({p.returncode}): {err}")
        return p.stdout

    def _api_json(self, path: str, paginate: bool = False):
        args = ["api", "-H", "Accept: application/vnd.github+json"]
        if paginate:
            args += ["--paginate", "--slurp"]
        out = self._run(args + [path])
        data = json.loads(out) if out.strip() else []
        if paginate:
            data = [x for page in data for x in page]
        return data

    def write(self, method: str, path: str, payload: dict | None = None, ok_404: bool = False):
        args = ["api", "-X", method, path]
        if payload is not None:
            args += ["--input", "-"]
        if self.dry_run:
            line = "gh " + " ".join(shlex.quote(a) for a in args)
            print(f"[dry-run] {line}")
            if payload is not None:
                print(f"[dry-run]   payload: {json.dumps(payload, ensure_ascii=False)}")
            return None
        try:
            out = self._run(args, payload)
        except PwError as e:
            if ok_404 and e.code == 4:
                return None
            raise
        return json.loads(out) if out.strip() else None

    # -- reads

    def issues(self, state: str, since: datetime | None = None) -> list[dict]:
        if self.fake is not None:
            items = self.fake["issues"]
            return [i for i in items if state == "all" or i.get("state", "open") == state]
        q = f"state={state}&per_page=100" + (f"&since={iso(since)}" if since else "")
        return self._api_json(f"repos/{self.repo}/issues?{q}", paginate=True)

    def issue(self, n: int) -> dict:
        if self.fake is not None:
            for i in self.fake["issues"]:
                if i["number"] == n:
                    return i
            raise PwError(f"issue #{n} not found", 4)
        return self._api_json(f"repos/{self.repo}/issues/{n}")

    def comments(self, n: int) -> list[dict]:
        if self.fake is not None:
            return list(self.fake["comments"].get(str(n), []))
        return self._api_json(f"repos/{self.repo}/issues/{n}/comments?per_page=100", paginate=True)

    def recent_comments(self, since: datetime) -> list[dict]:
        """Repo-wide comments updated since `since`, one paginated call."""
        if self.fake is not None:
            out = []
            for n, cs in self.fake["comments"].items():
                for c in cs:
                    if parse_time(c["created_at"]) >= since:
                        out.append(dict(c, issue_url=f"https://api.github.com/repos/{self.repo}/issues/{n}"))
            return out
        return self._api_json(
            f"repos/{self.repo}/issues/comments?since={iso(since)}&per_page=100", paginate=True)

    def search(self, query: str) -> list[dict]:
        """Search open and closed issues; returns REST-shaped issue dicts."""
        if self.fake is not None:
            words = [w.lower() for w in re.findall(r"\w+", query)]
            hits = []
            for i in self.fake["issues"]:
                hay = (i.get("title", "") + "\n" + (i.get("body") or "")).lower()
                if all(w in hay for w in words):
                    hits.append(i)
            return hits
        out = self._run(["search", "issues", "--repo", self.repo, "--include-prs=false",
                         "--limit", "50", "--json",
                         "number,title,state,body,authorAssociation,labels", "--", query])
        res = []
        for r in json.loads(out or "[]"):
            res.append({
                "number": r["number"], "title": r["title"], "state": r["state"].lower(),
                "body": r.get("body") or "", "author_association": r.get("authorAssociation", ""),
                "labels": [{"name": l["name"]} for l in r.get("labels", [])],
            })
        return res

    def priority_field_id(self) -> str | None:
        """Node id of the org's PRIORITY_FIELD number field, looked up once; warns if absent."""
        if self._priority_field is False:
            org = self.repo.split("/")[0]
            if self.fake is not None:
                fields = self.fake.get("issueFields", [])
            else:
                try:
                    fields = self._api_json(f"orgs/{org}/issue-fields", paginate=True)
                except PwError as e:
                    if e.code != 4:
                        raise
                    fields = []
            self._priority_field = next((f["node_id"] for f in fields if f.get("name") == PRIORITY_FIELD
                                         and f.get("data_type") == "number"), None)
            if self._priority_field is None:
                print(f'warning: org {org} has no number issue field "{PRIORITY_FIELD}"; '
                      "priority stays in the metadata block only", file=sys.stderr)
        return self._priority_field

    # -- simulation for dry-run re-reads

    def simulated_comment(self, body: str) -> dict:
        self._fake_id += 1
        return {"id": self._fake_id, "body": body, "created_at": iso(utc_now()),
                "user": {"login": "(dry-run)"}, "author_association": "OWNER"}


# --------------------------------------------------------------------------- metadata


def parse_meta(body: str | None) -> dict:
    m = META_RE.match(body or "")
    if not m:
        return {}
    try:
        data = yaml.safe_load(m.group(2)) or {}
    except yaml.YAMLError:
        return {}
    return data if isinstance(data, dict) else {}


def _dump_field(key: str, value) -> str:
    # One flow-style line per field, the same shape the migration wrote: wrap the value in a
    # one-element list so PyYAML emits it inline, then drop the wrapper brackets.
    inner = yaml.safe_dump([value], default_flow_style=True, width=100000,
                           allow_unicode=True).strip()
    return f"{key}: {inner[1:-1]}\n"


def edit_meta(body: str | None, sets: dict, unsets: tuple = ()) -> str:
    """Change only the named fields of the metadata block; every other byte stays."""
    body = body or ""
    m = META_RE.match(body)
    if not m:
        block = "".join(_dump_field(k, v) for k, v in sets.items())
        return "<!-- pinwright\n" + block + "-->\n\n" + body
    lines = m.group(2).splitlines(keepends=True)
    if lines and not lines[-1].endswith("\n"):
        lines[-1] += "\n"
    # Group into top-level fields: a key line plus its indented / list continuation lines.
    fields: list[tuple[str | None, list[str]]] = []
    for ln in lines:
        km = re.match(r"^([A-Za-z_][\w-]*)\s*:", ln)
        if km:
            fields.append((km.group(1), [ln]))
        elif fields:
            fields[-1][1].append(ln)
        else:
            fields.append((None, [ln]))
    pending = dict(sets)
    out: list[str] = []
    for key, chunk in fields:
        if key in unsets:
            continue
        if key in pending:
            out.append(_dump_field(key, pending.pop(key)))
        else:
            out.extend(chunk)
    for k, v in pending.items():
        out.append(_dump_field(k, v))
    head = m.group(0)
    new_head = m.group(1) + "<!-- pinwright\n" + "".join(out) + "-->" + head[head.rindex("-->") + 3:]
    return new_head + body[m.end():]


# --------------------------------------------------------------------------- priority


def check_rice(rice) -> list:
    """Validate [R, I, C, E]: R, I, E in 1..3, C one of CONFIDENCE. Returns it as a list."""
    ok = isinstance(rice, (list, tuple)) and len(rice) == 4
    if ok:
        r, i, c, e = rice
        ok = (all(isinstance(x, int) and not isinstance(x, bool) and 1 <= x <= 3 for x in (r, i, e))
              and isinstance(c, (int, float)) and not isinstance(c, bool) and c in CONFIDENCE)
    if not ok:
        raise PwError(f"rice must be R,I,C,E with R, I, E in 1..3 and C one of 1, 0.8, 0.5; "
                      f"got {rice!r}", 2)
    return [r, i, int(c) if c == 1 else c, e]


def parse_rice(text: str) -> list:
    try:
        parts = [yaml.safe_load(x) for x in text.split(",")]
    except yaml.YAMLError:
        parts = text
    return check_rice(parts)


def priority_of(rice: list, severity: str) -> int:
    r, i, c, e = rice
    p = round(100 * (r * IMPACT_WEIGHT[i] * c / e) / 12)
    return max(p, 90) if severity in ("critical", "high") else p


def strip_priority_line(body: str) -> str:
    """Drop the legacy visible **Priority:** line directly after the metadata block."""
    m = META_RE.match(body or "")
    old = m and PRIORITY_LINE_RE.match(body, m.end())
    if not old:
        return body
    head = m.group(0) if m.group(0).endswith("\n") else m.group(0) + "\n"
    return head + "\n" + body[old.end():]


def sync_priority_field(gh: Gh, n, node_id: str, priority: int | None):
    """Set the RICE priority issue field to `priority`, or clear it when None."""
    fid = gh.priority_field_id()
    if fid is None:
        return
    value = "delete: true" if priority is None else f"numberValue: {priority}"
    gh.write("POST", "graphql", {"query": (
        f"mutation {{ setIssueFieldValue(input: {{issueId: {json.dumps(node_id)}, issueFields: "
        f"[{{fieldId: {json.dumps(fid)}, {value}}}]}}) {{ clientMutationId }} }}")})
    print(f"#{n} {PRIORITY_FIELD} field " + ("cleared" if priority is None else f"= {priority}"))


# --------------------------------------------------------------------------- helpers


def label_names(issue: dict) -> set[str]:
    return {(l["name"] if isinstance(l, dict) else l) for l in issue.get("labels", [])}


def trusted(obj: dict) -> bool:
    return obj.get("author_association", "") in TRUSTED


def login(obj: dict) -> str:
    return (obj.get("user") or {}).get("login", "?")


def severity_of(issue: dict) -> str:
    for name in label_names(issue):
        if name.startswith("sev/"):
            return name[4:]
    return "none"


def rank_key(issue: dict):
    meta = parse_meta(issue.get("body")) if trusted(issue) else {}
    labels = label_names(issue)
    costly = meta.get("costly")
    if not isinstance(costly, int):
        costly = 1 if "costly" in labels else 0
    enc = meta.get("encounters")
    if not isinstance(enc, int):
        enc = 1
    p = meta.get("priority")
    p = p if isinstance(p, int) and not isinstance(p, bool) else None
    return ((p is None, -(p or 0), -SEVERITY_RANK.get(severity_of(issue), 0), -costly, -enc,
             issue["number"]), costly, enc, meta)


def lease_state(comments: list[dict], now: datetime) -> tuple[str | None, dict | None]:
    """Return (host, comment) of the live lease holder, or (None, None).

    Only trusted-author comments count. The holder is the earliest claim newer than the
    latest release and younger than LEASE_HOURS."""
    cs = sorted((c for c in comments if trusted(c)),
                key=lambda c: (parse_time(c["created_at"]), c.get("id", 0)))
    last_release = None
    for c in cs:
        if RELEASE_RE.search(c.get("body") or ""):
            last_release = parse_time(c["created_at"])
    cutoff = now - timedelta(hours=LEASE_HOURS)
    for c in cs:
        m = CLAIM_RE.match(c.get("body") or "")
        if not m:
            continue
        t = parse_time(c["created_at"])
        if t < cutoff or (last_release is not None and t <= last_release):
            continue
        return m.group(1), c
    return None, None


def safe_title(issue: dict) -> str:
    if trusted(issue):
        return issue.get("title", "")
    return f"[untrusted author {login(issue)}: title withheld]"


def withheld(obj: dict) -> str:
    return f"[untrusted author {login(obj)}: content withheld]"


def read_body_arg(args) -> str:
    if getattr(args, "body_file", None):
        with open(args.body_file, encoding="utf-8") as f:
            return f.read()
    if getattr(args, "body", None) is not None:
        return args.body
    raise PwError("one of --body or --body-file is required", 2)


def slugify(title: str, prefix: str) -> str:
    words = re.findall(r"[a-z0-9]+", title.lower())
    stop = {"a", "an", "the", "and", "or", "of", "to", "in", "on", "for", "with", "is", "are",
            "be", "by", "at", "as", "it", "its", "from", "that", "this", "when", "into"}
    keep = [w for w in words if w not in stop][:5] or ["untitled"]
    return f"{prefix}-" + "-".join(keep)


def get_issue_checked(gh: Gh, n: int) -> dict:
    issue = gh.issue(n)
    if "pull_request" in issue:
        raise PwError(f"#{n} is a pull request, not an issue", 4)
    return issue


# --------------------------------------------------------------------------- commands


def cmd_list(gh: Gh, args) -> int:
    since = None
    if args.since:
        since = parse_time(args.since if "T" in args.since else args.since + "T00:00:00Z")
        if since.tzinfo is None:
            since = since.replace(tzinfo=timezone.utc)
    if args.closed:
        if args.all_states:
            raise PwError("--closed and --all-states are exclusive", 2)
        issues = [i for i in gh.issues("closed", since) if "pull_request" not in i
                  and i.get("state_reason") == "completed"
                  and (since is None or parse_time(i["closed_at"]) >= since)]
    else:
        issues = [i for i in gh.issues("all" if args.all_states else "open") if "pull_request" not in i]
    wanted = set(args.label or [])
    now = utc_now()
    rows = []
    claimed = []
    for i in issues:
        labels = label_names(i)
        if not args.any_status and "status/accepted" not in labels:
            continue
        if not wanted <= labels:
            continue
        if "area/harness" in labels and "area/harness" not in wanted:
            continue
        if "status/blocked" in labels and "status/blocked" not in wanted:
            continue
        rows.append(i)
        if "status/claimed" in labels:
            claimed.append(i["number"])
    if claimed:
        by_issue: dict[int, list] = {}
        for c in gh.recent_comments(now - timedelta(hours=LEASE_HOURS)):
            n = int(c["issue_url"].rstrip("/").rsplit("/", 1)[1])
            by_issue.setdefault(n, []).append(c)
        live = {}
        for n in claimed:
            host, _ = lease_state(by_issue.get(n, []), now)
            if host and host != args.host:
                live[n] = host
        rows = [i for i in rows if i["number"] not in live]
    ranked = []
    for i in rows:
        key, costly, enc, meta = rank_key(i)
        ranked.append((key, i, costly, enc, meta))
    ranked.sort(key=lambda r: r[0])
    if args.json:
        out = [{"number": i["number"], "title": safe_title(i), "state": i.get("state"),
                "priority": meta.get("priority"), "rice": meta.get("rice"),
                "severity": severity_of(i), "costly": costly, "encounters": enc,
                "id": meta.get("id"), "labels": sorted(label_names(i)), "trusted": trusted(i)}
               for _, i, costly, enc, meta in ranked]
        print(json.dumps(out, indent=1, ensure_ascii=False))
    else:
        if not ranked:
            print("(no matching issues)")
        for _, i, costly, enc, meta in ranked:
            p = meta.get("priority")
            print(f"#{i['number']}\tp={'-' if p is None else p}\t{severity_of(i)}\t"
                  f"costly={costly}\tenc={enc}\t"
                  f"{meta.get('id') or '-'}\t{safe_title(i)}")
    return 0


def cmd_show(gh: Gh, args) -> int:
    issue = get_issue_checked(gh, args.number)
    comments = gh.comments(args.number)
    t = trusted(issue)
    if args.json:
        out = {
            "number": issue["number"], "state": issue.get("state"),
            "state_reason": issue.get("state_reason"), "title": safe_title(issue),
            "author": login(issue), "trusted": t, "labels": sorted(label_names(issue)),
            "meta": parse_meta(issue.get("body")) if t else None,
            "body": issue.get("body") if t else withheld(issue),
            "comments": [{"author": login(c), "trusted": trusted(c), "created_at": c["created_at"],
                          "body": c.get("body") if trusted(c) else withheld(c)} for c in comments],
        }
        print(json.dumps(out, indent=1, ensure_ascii=False))
        return 0
    p = parse_meta(issue.get("body")).get("priority") if t else None
    print(f"#{issue['number']} {safe_title(issue)}" + ("" if p is None else f"   priority: {p}"))
    reason = f" ({issue['state_reason']})" if issue.get("state_reason") else ""
    print(f"state: {issue.get('state')}{reason}   author: {login(issue)} "
          f"({issue.get('author_association', '?')})")
    print("labels: " + ", ".join(sorted(label_names(issue))))
    print()
    print((issue.get("body") or "") if t else withheld(issue))
    for c in comments:
        print()
        print(f"--- comment by {login(c)} ({c.get('author_association', '?')}) at {c['created_at']}")
        print((c.get("body") or "") if trusted(c) else withheld(c))
    return 0


def _post_comment(gh: Gh, n: int, body: str):
    return gh.write("POST", f"repos/{gh.repo}/issues/{n}/comments", {"body": body})


def _add_labels(gh: Gh, n: int, labels: list[str]):
    return gh.write("POST", f"repos/{gh.repo}/issues/{n}/labels", {"labels": labels})


def _remove_label(gh: Gh, n: int, label: str):
    return gh.write("DELETE", f"repos/{gh.repo}/issues/{n}/labels/{label.replace('/', '%2F')}",
                    ok_404=True)


def cmd_claim(gh: Gh, args) -> int:
    issue = get_issue_checked(gh, args.number)
    if issue.get("state") != "open":
        raise PwError(f"#{args.number} is closed; nothing to claim", 2)
    body = f"pinwright-claim host={args.host} at={iso(utc_now())}"
    _post_comment(gh, args.number, body)
    _add_labels(gh, args.number, ["status/claimed"])
    comments = gh.comments(args.number)
    if gh.dry_run:
        comments.append(gh.simulated_comment(body))
    host, c = lease_state(comments, utc_now())
    if host == args.host:
        print(f"WON #{args.number} host={args.host}")
        return 0
    print(f"LOST #{args.number} lease held by host={host} since {c['created_at'] if c else '?'}")
    return 3


def _release_line(host: str) -> str:
    return f"pinwright-release host={host} at={iso(utc_now())}"


def cmd_release(gh: Gh, args) -> int:
    get_issue_checked(gh, args.number)
    body = _release_line(args.host)
    if args.note:
        body += "\n\n" + args.note
    _post_comment(gh, args.number, body)
    _remove_label(gh, args.number, "status/claimed")
    print(f"released #{args.number} host={args.host}")
    return 0


def cmd_comment(gh: Gh, args) -> int:
    get_issue_checked(gh, args.number)
    r = _post_comment(gh, args.number, read_body_arg(args))
    print(f"commented on #{args.number}" + (f": {r['html_url']}" if r else ""))
    return 0


def cmd_close(gh: Gh, args) -> int:
    issue = get_issue_checked(gh, args.number)
    if args.reason == "duplicate" and not args.duplicate_of:
        raise PwError("--reason duplicate needs --duplicate-of", 2)
    text = read_body_arg(args).rstrip("\n")
    if args.duplicate_of:
        text = f"Duplicate of #{args.duplicate_of}\n\n" + text
    text += "\n\n" + _release_line(args.host)
    _post_comment(gh, args.number, text)
    if "status/claimed" in label_names(issue):
        _remove_label(gh, args.number, "status/claimed")
    gh.write("PATCH", f"repos/{gh.repo}/issues/{args.number}",
             {"state": "closed", "state_reason": args.reason})
    print(f"closed #{args.number} as {args.reason}")
    return 0


def cmd_reopen(gh: Gh, args) -> int:
    get_issue_checked(gh, args.number)
    _post_comment(gh, args.number, read_body_arg(args))
    gh.write("PATCH", f"repos/{gh.repo}/issues/{args.number}",
             {"state": "open", "state_reason": "reopened"})
    print(f"reopened #{args.number}")
    return 0


def _norm_title(t: str) -> str:
    return re.sub(r"\s+", " ", t).strip().lower()


def cmd_file(gh: Gh, args) -> int:
    title = args.title.strip()
    if len(title) > TITLE_MAX:
        raise PwError(f"title is {len(title)} characters; the cap is {TITLE_MAX}. "
                      "Write a shorter title and put the detail in the body.", 2)
    if args.severity and args.severity not in SEVERITY_RANK:
        raise PwError(f"--severity must be one of {', '.join(SEVERITY_RANK)}", 2)
    harness = args.area == "harness"
    rice = parse_rice(args.rice) if args.rice else None
    prefix = "H" if harness else TYPE_PREFIX[args.type]
    slug = args.id or slugify(title, prefix)
    body = read_body_arg(args)
    tags = [t.strip() for t in (args.tags or "").split(",") if t.strip()]
    now = iso(utc_now())

    seen: dict[int, dict] = {}
    for q in ([] if args.into else [args.dedupe_query or title, f'"{slug}"']):
        for hit in gh.search(q):
            seen.setdefault(hit["number"], hit)
    match = args.into
    for n in ([] if args.into else sorted(seen)):
        hit = seen[n]
        if not trusted(hit):
            continue
        if parse_meta(hit.get("body")).get("id") == slug or _norm_title(hit["title"]) == _norm_title(title):
            match = n
            break

    if match is not None:
        issue = get_issue_checked(gh, match)
        if not trusted(issue):
            raise PwError(f"#{match} was written by an untrusted author; file a new issue", 2)
        meta = parse_meta(issue.get("body"))
        enc = meta.get("encounters") if isinstance(meta.get("encounters"), int) else 1
        sets = {"encounters": enc + 1, "lastSeen": now}
        if args.costly:
            c = meta.get("costly") if isinstance(meta.get("costly"), int) else 0
            sets["costly"] = c + 1
        if tags:
            sets["tags"] = list(dict.fromkeys(list(meta.get("tags") or []) + tags))
        if rice:
            sets["rice"] = rice
            sets["priority"] = priority_of(rice, severity_of(issue))
        gh.write("PATCH", f"repos/{gh.repo}/issues/{match}",
                 {"body": strip_priority_line(edit_meta(issue.get("body"), sets))})
        if rice:
            sync_priority_field(gh, match, issue["node_id"], sets["priority"])
        _post_comment(gh, match, f"New encounter ({enc + 1}).\n\n" + body)
        if args.costly and "costly" not in label_names(issue):
            _add_labels(gh, match, ["costly"])
        state = issue.get("state")
        print(f"#{match} bumped encounters to {enc + 1} (state={state}"
              + (f", reason={issue.get('state_reason')}" if state == "closed" else "") + ")")
        return 5 if state == "closed" else 0

    if rice is None and not harness:
        raise PwError("a new issue needs --rice R,I,C,E (see CLAUDE.md -> Issue tracker -> "
                      "Priority (RICE)); without it the issue sorts below every scored one", 2)
    meta = {"id": slug, "tags": tags, "encounters": 1, "lastSeen": now}
    if args.costly:
        meta["costly"] = 1
    if rice:
        meta["rice"] = rice
        meta["priority"] = priority_of(rice, args.severity or "none")
    block = "<!-- pinwright\n" + yaml.safe_dump(meta, sort_keys=False, default_flow_style=None,
                                                width=100000, allow_unicode=True) + "-->\n\n"
    labels = [f"type/{args.type}", "status/accepted"]
    if args.severity:
        labels.append(f"sev/{args.severity}")
    if harness:
        labels.append("area/harness")
    if args.costly:
        labels.append("costly")
    r = gh.write("POST", f"repos/{gh.repo}/issues",
                 {"title": title, "body": block + body, "labels": labels})
    print(f"#{r['number']} created ({slug})" if r else f"(dry-run) would create {slug}")
    if rice:
        sync_priority_field(gh, r["number"] if r else slug, r["node_id"] if r else "<new issue>",
                            meta["priority"])
    return 0


def cmd_meta(gh: Gh, args) -> int:
    issue = get_issue_checked(gh, args.number)
    if not trusted(issue):
        raise PwError(f"#{args.number} was written by an untrusted author; edit refused", 2)
    sets = {}
    for kv in args.set or []:
        if "=" not in kv:
            raise PwError(f"--set expects key=value, got {kv!r}", 2)
        k, v = kv.split("=", 1)
        sets[k.strip()] = yaml.safe_load(v) if v.strip() else None
    unsets = tuple(args.unset or ())
    if not sets and not unsets and not args.sync_field:
        raise PwError("nothing to change: pass --set k=v, --unset k or --sync-field", 2)
    if "priority" in sets or "priority" in unsets:
        raise PwError("priority is derived from rice and severity; set rice instead "
                      "(meta N --set rice=[R,I,C,E])", 2)
    if "rice" in sets:
        v = sets["rice"]
        sets["rice"] = parse_rice(v) if isinstance(v, str) else check_rice(v)
        sets["priority"] = priority_of(sets["rice"], severity_of(issue))
    if "rice" in unsets:
        unsets += ("priority",)
    old = issue.get("body") or ""
    body = strip_priority_line(edit_meta(old, sets, unsets) if sets or unsets else old)
    if body != old:
        gh.write("PATCH", f"repos/{gh.repo}/issues/{args.number}", {"body": body})
    if sets or unsets:
        print(f"#{args.number} metadata updated: " + ", ".join(
            [f"{k}={v!r}" for k, v in sets.items()] + [f"-{k}" for k in unsets]))
    if args.sync_field or "rice" in sets or "rice" in unsets:
        sync_priority_field(gh, args.number, issue["node_id"], parse_meta(body).get("priority"))
    return 0


def cmd_label(gh: Gh, args) -> int:
    issue = get_issue_checked(gh, args.number)
    have = label_names(issue)
    add = list(dict.fromkeys(args.add or []))
    remove = list(dict.fromkeys(args.remove or []))
    if not add and not remove:
        raise PwError("nothing to change: pass --add L or --remove L", 2)
    if any(l.startswith("sev/") for l in add):
        remove += [l for l in have if l.startswith("sev/") and l not in add and l not in remove]
    if "status/accepted" in add and not trusted(issue):
        print(f"note: #{args.number} is by an untrusted author; agents will read only "
              "maintainer comments on it", file=sys.stderr)
    to_add = [l for l in add if l not in have]
    if to_add:
        _add_labels(gh, args.number, to_add)
    for l in remove:
        if l in have:
            _remove_label(gh, args.number, l)
    print(f"#{args.number} labels: +{to_add} -{[l for l in remove if l in have]}")
    meta = parse_meta(issue.get("body")) if trusted(issue) else {}
    if meta.get("rice"):
        sev = severity_of({"labels": (have | set(add)) - set(remove)})
        p = priority_of(check_rice(meta["rice"]), sev)
        if p != meta.get("priority"):
            gh.write("PATCH", f"repos/{gh.repo}/issues/{args.number}",
                     {"body": strip_priority_line(edit_meta(issue.get("body"), {"priority": p}))})
            print(f"#{args.number} priority {meta.get('priority')} -> {p}")
            sync_priority_field(gh, args.number, issue["node_id"], p)
    return 0


def cmd_edit(gh: Gh, args) -> int:
    issue = get_issue_checked(gh, args.number)
    if not trusted(issue):
        raise PwError(f"#{args.number} was written by an untrusted author; edit refused", 2)
    payload = {}
    if args.title is not None:
        t = args.title.strip()
        if len(t) > TITLE_MAX:
            raise PwError(f"title is {len(t)} characters; the cap is {TITLE_MAX}", 2)
        payload["title"] = t
    if args.body is not None or args.body_file:
        old = issue.get("body") or ""
        m = META_RE.match(old)
        head = (m.group(0).rstrip("\n") + "\n\n") if m else ""
        payload["body"] = head + read_body_arg(args)
    if not payload:
        raise PwError("nothing to change: pass --title and/or --body/--body-file", 2)
    gh.write("PATCH", f"repos/{gh.repo}/issues/{args.number}", payload)
    print(f"#{args.number} edited: {', '.join(payload)}")
    return 0


# --------------------------------------------------------------------------- CLI


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="pw_issues.py", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--repo", default=os.environ.get("PW_ISSUES_REPO", DEFAULT_REPO))
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--fake-data", metavar="FILE")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("list", help="issues an agent may work, ranked")
    s.add_argument("--label", action="append", help="require this label (repeatable); "
                   "area/harness and status/blocked issues appear only when named here")
    s.add_argument("--any-status", action="store_true",
                   help="maintainers only: drop the status/accepted requirement")
    s.add_argument("--all-states", action="store_true", help="include closed issues")
    s.add_argument("--closed", action="store_true",
                   help="only issues closed as completed (for re-testing)")
    s.add_argument("--since", help="with --closed: closed at or after this date / ISO time")
    s.add_argument("--host", help="your host id: your own live lease does not hide an issue")
    s.add_argument("--json", action="store_true")
    s.set_defaults(fn=cmd_list)

    s = sub.add_parser("show", help="one issue and its comments, trust-filtered")
    s.add_argument("number", type=int)
    s.add_argument("--json", action="store_true")
    s.set_defaults(fn=cmd_show)

    s = sub.add_parser("claim", help="take the lease; exit 0 WON, 3 LOST")
    s.add_argument("number", type=int)
    s.add_argument("--host", required=True)
    s.set_defaults(fn=cmd_claim)

    s = sub.add_parser("release", help="give the lease back")
    s.add_argument("number", type=int)
    s.add_argument("--host", required=True)
    s.add_argument("--note")
    s.set_defaults(fn=cmd_release)

    def body_opts(sp, required=True):
        g = sp.add_mutually_exclusive_group(required=required)
        g.add_argument("--body")
        g.add_argument("--body-file")

    s = sub.add_parser("comment", help="add a comment")
    s.add_argument("number", type=int)
    body_opts(s)
    s.set_defaults(fn=cmd_comment)

    s = sub.add_parser("close", help="comment, then close (also releases the lease)")
    s.add_argument("number", type=int)
    s.add_argument("--reason", required=True, choices=["completed", "not_planned", "duplicate"])
    s.add_argument("--duplicate-of", type=int)
    s.add_argument("--host", default="maintainer", help="host id written on the release line")
    body_opts(s)
    s.set_defaults(fn=cmd_close)

    s = sub.add_parser("reopen", help="comment, then reopen")
    s.add_argument("number", type=int)
    body_opts(s)
    s.set_defaults(fn=cmd_reopen)

    s = sub.add_parser("file", help="file a new issue or bump an existing match")
    s.add_argument("--title", required=True, help=f"at most {TITLE_MAX} characters")
    s.add_argument("--type", required=True, choices=list(TYPE_PREFIX))
    s.add_argument("--severity", choices=list(SEVERITY_RANK))
    s.add_argument("--tags", help="comma-separated")
    s.add_argument("--costly", action="store_true", help="this encounter cost real work")
    s.add_argument("--area", choices=["harness"])
    s.add_argument("--rice", metavar="R,I,C,E",
                   help="RICE factors (R, I, E in 1..3; C 1, 0.8 or 0.5); priority is derived "
                   "from them. Required for a new issue unless --area harness; on a match it "
                   "replaces the issue's rice")
    s.add_argument("--id", help="metadata id slug (default: derived from the title)")
    s.add_argument("--dedupe-query", help="search text for the dedupe (default: the title)")
    s.add_argument("--into", type=int, metavar="N",
                   help="skip the search: this is a new encounter of issue N")
    body_opts(s)
    s.set_defaults(fn=cmd_file)

    s = sub.add_parser("meta", help="edit metadata block fields")
    s.add_argument("number", type=int)
    s.add_argument("--set", action="append", metavar="K=V", help="value is parsed as YAML")
    s.add_argument("--unset", action="append", metavar="K")
    s.add_argument("--sync-field", action="store_true",
                   help=f'set the "{PRIORITY_FIELD}" issue field from the metadata priority '
                   "(cleared when there is none) and drop a legacy **Priority:** body line")
    s.set_defaults(fn=cmd_meta)

    s = sub.add_parser("label", help="add or remove labels")
    s.add_argument("number", type=int)
    s.add_argument("--add", action="append", metavar="L")
    s.add_argument("--remove", action="append", metavar="L")
    s.set_defaults(fn=cmd_label)

    s = sub.add_parser("edit", help="change the title or the body below the metadata block")
    s.add_argument("number", type=int)
    s.add_argument("--title")
    body_opts(s, required=False)
    s.set_defaults(fn=cmd_edit)
    return p


def main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except Exception:
            pass
    args = build_parser().parse_args(argv)
    gh = Gh(args.repo, args.dry_run, args.fake_data)
    try:
        return args.fn(gh, args)
    except PwError as e:
        print(f"pw_issues: {e}", file=sys.stderr)
        return e.code


if __name__ == "__main__":
    sys.exit(main())
