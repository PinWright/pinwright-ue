# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Read-only query surface over a recorded journal NDJSON session.

UE's embedded CPython ships no pandas/numpy, so this is stdlib only (json, bisect,
math). `load(path)` parses one session file into a `Session` object exposed to a
`recorder.query` snippet as `q`; `cap(result, max_rows)` clips an enumerable result
and reports truncation so the snippet author cannot accidentally return an unbounded
blob to a token-bounded LLM.

NDJSON schema (one JSON object per line, written by the C++ FJournalRecorder):
  header: session, fmt, engine, startUtc, t0, axis, domains[]
  obj:    key, label, path, type, parent, tFirst
  var:    tag, kind, unit, eps, domain, tFirst
  val:    t, dom, dt, df, key, tag, kind, v|s
  evt:    id, t, dom, key, name, sev, props
The primary axis is `t` (wall-clock seconds); `dt`/`df` are secondary domain handles.
"""

import bisect
import json
import math

HARD_ROW_CAP = 2000


class Point:
    """One change point in a (key, tag) series: wall-clock `t` plus the value.

    `v` is the scalar/vector payload (float, or list for Vec2/Vec3/Vec4/Quat),
    `s` is the string/enum-name payload. `dt`/`df` are the secondary domain
    time / domain frame carried for drill-down.
    """

    __slots__ = ("t", "kind", "v", "s", "dt", "df")

    def __init__(self, t, kind, v, s, dt, df):
        self.t = t
        self.kind = kind
        self.v = v
        self.s = s
        self.dt = dt
        self.df = df

    def as_dict(self):
        d = {"t": self.t, "kind": self.kind}
        if self.v is not None:
            d["v"] = self.v
        if self.s is not None:
            d["s"] = self.s
        if self.dt is not None:
            d["dt"] = self.dt
        if self.df is not None:
            d["df"] = self.df
        return d


class Session:
    """Parsed, read-only view of a recording. Series are sorted by `t`."""

    def __init__(self):
        self.session_id = ""
        self.label = None
        self.engine = None
        self.start_utc = None
        self.t0 = None
        self.axis = "time"
        self.domains = []
        # key -> obj record dict
        self.objects = {}
        # tag -> variable manifest dict
        self.variables = {}
        # (key, tag) -> list[Point], sorted by t
        self._series = {}
        # cached sorted t arrays for bisect, keyed by (key, tag)
        self._series_t = {}
        self.events = []
        self.min_ts = None
        self.max_ts = None

    # --- discovery surface ------------------------------------------------

    @property
    def series_keys(self):
        """Every (key, tag) pair that has a recorded series."""
        return list(self._series.keys())

    def series(self, key, tag):
        """The change-only series for (key, tag), sorted by t. Empty list if none."""
        return self._series.get((key, tag), [])

    def value_at(self, key, tag, ts):
        """As-of value: the last change at or before `ts` (carried forward).

        Returns the Point, or None when no value existed at or before `ts`.
        """
        points = self._series.get((key, tag))
        if not points:
            return None
        times = self._series_t[(key, tag)]
        idx = bisect.bisect_right(times, ts) - 1
        if idx < 0:
            return None
        return points[idx]

    @staticmethod
    def magnitude(point):
        """Scalar magnitude: abs for scalars, Euclidean norm for vectors, NaN for strings."""
        if point is None:
            return float("nan")
        if point.v is None:
            return float("nan")
        if isinstance(point.v, (list, tuple)):
            return math.sqrt(sum(c * c for c in point.v))
        return abs(point.v)

    # --- internal build ---------------------------------------------------

    def _add_value_line(self, line):
        key = line.get("key", "")
        tag = line.get("tag", "")
        t = line.get("t")
        if t is None:
            return
        point = Point(
            t=t,
            kind=line.get("kind"),
            v=line.get("v"),
            s=line.get("s"),
            dt=line.get("dt"),
            df=line.get("df"),
        )
        self._series.setdefault((key, tag), []).append(point)
        if self.min_ts is None or t < self.min_ts:
            self.min_ts = t
        if self.max_ts is None or t > self.max_ts:
            self.max_ts = t

    def _finalize(self):
        for sk, points in self._series.items():
            points.sort(key=lambda p: p.t)
            self._series_t[sk] = [p.t for p in points]


def load(path):
    """Parse an NDJSON session file into a Session.

    Stops cleanly on the first malformed line (a crash-truncated tail is expected),
    keeping every well-formed line read so far.
    """
    s = Session()
    with open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            raw = raw.strip()
            if not raw:
                continue
            try:
                line = json.loads(raw)
            except ValueError:
                # Truncated/corrupt tail line — stop, keep what we have.
                break
            kind = line.get("k")
            if kind == "header":
                s.session_id = line.get("session", "")
                s.label = line.get("label")
                s.engine = line.get("engine")
                s.start_utc = line.get("startUtc")
                s.t0 = line.get("t0")
                s.axis = line.get("axis", "time")
                s.domains = line.get("domains", []) or []
            elif kind == "obj":
                s.objects[line.get("key", "")] = line
            elif kind == "var":
                s.variables[line.get("tag", "")] = line
            elif kind == "val":
                s._add_value_line(line)
            elif kind == "evt":
                s.events.append(line)
    s._finalize()
    return s


def _row(item):
    """Coerce one result row into a JSON-serializable value."""
    if isinstance(item, Point):
        return item.as_dict()
    return item


def cap(result, max_rows):
    """Clip an enumerable result to `max_rows`, reporting truncation.

    Scalars and strings pass through untouched. A sized collection reports its
    true total; a lazy iterable is read at most cap+1 to detect overflow without
    draining an unbounded source. Returns the envelope shape the handler emits:
    {value, meta:{truncated, totalCount, elided}}.
    """
    cap_n = max_rows if max_rows and max_rows > 0 else 200
    cap_n = min(cap_n, HARD_ROW_CAP)

    if result is None or isinstance(result, (str, bytes, int, float, bool, dict)):
        return {"value": _row(result), "meta": {"truncated": False, "totalCount": None, "elided": 0}}

    # Sized collections (list/tuple) know their true count cheaply.
    if isinstance(result, (list, tuple)):
        total = len(result)
        rows = [_row(x) for x in result[:cap_n]]
        truncated = total > cap_n
        return {
            "value": rows,
            "meta": {
                "truncated": truncated,
                "totalCount": total,
                "elided": max(0, total - cap_n),
            },
        }

    # Generic iterable (generator, etc.): read at most cap+1 to detect overflow.
    try:
        iterator = iter(result)
    except TypeError:
        return {"value": _row(result), "meta": {"truncated": False, "totalCount": None, "elided": 0}}

    rows = []
    truncated = False
    for item in iterator:
        if len(rows) >= cap_n:
            truncated = True
            break
        rows.append(_row(item))
    return {
        "value": rows,
        "meta": {
            "truncated": truncated,
            "totalCount": None if truncated else len(rows),
            "elided": 0,
        },
    }
