# Copyright (c) 2026 Alexander Penkin. MIT License.
"""Offline reproduction of PinWright's GeometryUtils::FMeshOrientation over .pwmodel
`append_buffers` blocks. Pure text parsing, stdlib only -- it imports nothing and needs no
running editor.

Reproduces exactly:
  BoundaryEdges     edges adjacent to exactly one triangle
  InconsistentEdges interior edges whose two triangles traverse it the SAME way
  SignedVolume      divergence-theorem integral, sum dot(a, cross(b,c))/6
and adds a per-connected-component breakdown, which the RPC does not report.
"""
import re
import sys
import json
from collections import defaultdict


def parse_parts(path):
    txt = open(path, encoding="utf-8").read()
    parts = []
    for pm in re.finditer(r"^part\s+(\w+)\s*\{", txt, re.M):
        name = pm.group(1)
        # part body ends at the next line that is exactly '}' at column 0
        end = txt.index("\n}", pm.end())
        body = txt[pm.end():end]
        blocks = []
        for bm in re.finditer(r"append_buffers\s", body):
            seg = body[bm.end():]
            vm = re.search(r"vertices=\[(.*?)\]\s", seg, re.S)
            tm = re.search(r"triangles=\[(.*?)\]\s", seg, re.S)
            mm = re.search(r'material="([^"]*)"', seg[:200])
            verts = [tuple(float(x) for x in g.split(","))
                     for g in re.findall(r"\(([^)]*)\)", vm.group(1))]
            tris_flat = [int(x) for x in re.findall(r"-?\d+", tm.group(1))]
            tris = [tuple(tris_flat[i:i + 3]) for i in range(0, len(tris_flat), 3)]
            blocks.append((mm.group(1) if mm else "", verts, tris))
        parts.append((name, blocks))
    return parts


def measure(verts, tris):
    """-> dict matching FMeshOrientation plus component detail."""
    edge_dirs = defaultdict(list)  # unordered key -> list of directed (u,v)
    for (a, b, c) in tris:
        for (u, v) in ((a, b), (b, c), (c, a)):
            edge_dirs[(min(u, v), max(u, v))].append((u, v))

    boundary = sum(1 for k, d in edge_dirs.items() if len(d) == 1)
    inconsistent = 0
    for k, d in edge_dirs.items():
        if len(d) == 2 and d[0] == d[1]:
            inconsistent += 1
    nonmanifold_edges = sum(1 for k, d in edge_dirs.items() if len(d) > 2)

    def tri_vol(t):
        # EXACT reproduction of TMeshQueries::GetVolumeArea (MeshQueries.h:134), which is
        # what GeometryUtils::MeasureMeshOrientation reads: the per-triangle term is
        # N.X * (V0.X + V1.X + V2.X) with N = (V2-V0) x (V1-V0) -- the engine's facing
        # normal, the NEGATION of the right-hand rule, because Unreal is left-handed.
        # X-COMPONENT ONLY. On a closed, consistently-wound mesh this equals the full
        # divergence integral; on an inconsistently-wound one it does NOT, so the full
        # dot-product form is not a substitute.
        v0, v1, v2 = verts[t[0]], verts[t[1]], verts[t[2]]
        a1, a2, a3 = v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]
        b1, b2, b3 = v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]
        nx = a2 * b3 - a3 * b2
        return nx * (v0[0] + v1[0] + v2[0]) / 6.0

    def tri_area(t):
        a, b, c = verts[t[0]], verts[t[1]], verts[t[2]]
        ux = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        vx = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        n = (ux[1] * vx[2] - ux[2] * vx[1],
             ux[2] * vx[0] - ux[0] * vx[2],
             ux[0] * vx[1] - ux[1] * vx[0])
        return 0.5 * (n[0] ** 2 + n[1] ** 2 + n[2] ** 2) ** 0.5

    vol = sum(tri_vol(t) for t in tris)
    area = sum(tri_area(t) for t in tris)

    # connected components over triangles sharing an edge
    tri_of_edge = defaultdict(list)
    for i, (a, b, c) in enumerate(tris):
        for (u, v) in ((a, b), (b, c), (c, a)):
            tri_of_edge[(min(u, v), max(u, v))].append(i)
    seen = [False] * len(tris)
    comps = []
    for i in range(len(tris)):
        if seen[i]:
            continue
        stack, group = [i], []
        seen[i] = True
        while stack:
            j = stack.pop()
            group.append(j)
            a, b, c = tris[j]
            for (u, v) in ((a, b), (b, c), (c, a)):
                for k in tri_of_edge[(min(u, v), max(u, v))]:
                    if not seen[k]:
                        seen[k] = True
                        stack.append(k)
        comps.append(group)

    comp_detail = []
    for g in comps:
        ed = defaultdict(list)
        for j in g:
            a, b, c = tris[j]
            for (u, v) in ((a, b), (b, c), (c, a)):
                ed[(min(u, v), max(u, v))].append((u, v))
        comp_detail.append({
            "tris": len(g),
            "boundaryEdges": sum(1 for k, d in ed.items() if len(d) == 1),
            "inconsistentEdges": sum(1 for k, d in ed.items() if len(d) == 2 and d[0] == d[1]),
            "signedVolume": sum(tri_vol(tris[j]) for j in g),
        })

    return {
        "vertexCount": len(verts),
        "triangleCount": len(tris),
        "boundaryEdges": boundary,
        "inconsistentEdges": inconsistent,
        "nonManifoldEdges": nonmanifold_edges,
        "signedVolume": vol,
        "surfaceArea": area,
        "isClosed": boundary == 0,
        "orientationConsistent": inconsistent == 0,
        "componentCount": len(comps),
        "components": comp_detail,
    }


def run(path, comp_limit=12):
    print("#" * 78)
    print("#", path)
    parts = parse_parts(path)
    all_v, all_t = [], []
    for name, blocks in parts:
        pv, pt = [], []
        for mat, verts, tris in blocks:
            off = len(pv)
            pv.extend(verts)
            pt.extend([(a + off, b + off, c + off) for (a, b, c) in tris])
        off = len(all_v)
        all_v.extend(pv)
        all_t.extend([(a + off, b + off, c + off) for (a, b, c) in pt])
        m = measure(pv, pt)
        print("\n  PART %-10s  v=%-6d t=%-6d  closed=%-5s  boundaryEdges=%-5d"
              "  inconsistentEdges=%-5d  orientationConsistent=%-5s\n"
              "                    signedVolume=%+.6e  area=%.4e  components=%d"
              % (name, m["vertexCount"], m["triangleCount"], m["isClosed"],
                 m["boundaryEdges"], m["inconsistentEdges"],
                 m["orientationConsistent"], m["signedVolume"], m["surfaceArea"],
                 m["componentCount"]))
        neg = [c for c in m["components"] if c["signedVolume"] < 0]
        pos = [c for c in m["components"] if c["signedVolume"] >= 0]
        print("      components: %d negative (sum %+.4e), %d positive (sum %+.4e)"
              % (len(neg), sum(c["signedVolume"] for c in neg),
                 len(pos), sum(c["signedVolume"] for c in pos)))
        bad = [c for c in m["components"] if c["inconsistentEdges"] or c["boundaryEdges"]]
        print("      components with boundary or inconsistent edges: %d" % len(bad))
        for c in m["components"][:comp_limit]:
            print("        tris=%-5d bnd=%-4d inc=%-4d vol=%+.4e"
                  % (c["tris"], c["boundaryEdges"], c["inconsistentEdges"],
                     c["signedVolume"]))
        if m["componentCount"] > comp_limit:
            print("        ... %d more" % (m["componentCount"] - comp_limit))

    mm = measure(all_v, all_t)
    print("\n  MERGED  v=%d t=%d closed=%s boundaryEdges=%d inconsistentEdges=%d "
          "orientationConsistent=%s\n          signedVolume=%+.6e components=%d"
          % (mm["vertexCount"], mm["triangleCount"], mm["isClosed"],
             mm["boundaryEdges"], mm["inconsistentEdges"],
             mm["orientationConsistent"], mm["signedVolume"], mm["componentCount"]))
    return mm


def measure_file(path):
    """-> the same numbers `run` prints, as a plain dict, plus file provenance.

    Separate from `run` on purpose: `run` is the human-readable path and stays
    byte-identical, so a JSON artifact and a console reading cannot drift apart in
    formatting yet still come from one `measure()`.
    """
    import hashlib, os, datetime
    raw = open(path, "rb").read()
    out = {
        "path": path.replace("\\", "/"),
        "bytes": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "mtimeUtc": datetime.datetime.fromtimestamp(
            os.path.getmtime(path), datetime.timezone.utc).isoformat(),
        "appendBufferBlocks": 0,
        "parts": [],
    }
    all_v, all_t = [], []
    for name, blocks in parse_parts(path):
        out["appendBufferBlocks"] += len(blocks)
        pv, pt = [], []
        for mat, verts, tris in blocks:
            off = len(pv)
            pv.extend(verts)
            pt.extend([(a + off, b + off, c + off) for (a, b, c) in tris])
        off = len(all_v)
        all_v.extend(pv)
        all_t.extend([(a + off, b + off, c + off) for (a, b, c) in pt])
        m = measure(pv, pt)
        m["part"] = name
        m["appendBufferBlocks"] = len(blocks)
        out["parts"].append(m)
    out["merged"] = measure(all_v, all_t)
    # A model with no raw buffers is NOT measurable by this instrument: its geometry comes
    # from .pwmodel primitives the compiler expands, which this text parser never sees. The
    # zeros below are absence of input, not a healthy mesh -- callers must branch on this.
    out["measurable"] = out["appendBufferBlocks"] > 0
    return out


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--json":
        import datetime
        doc = {
            "schema": "pinwright.pwmodel.orientation/1",
            "generatedUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "instrument": "Plugins/PinWright/Content/Python/measure_pwm.py",
            "reproduces": "PinWrightGeometry GeometryUtils::MeasureMeshOrientation",
            "models": [measure_file(p) for p in sys.argv[3:]],
        }
        with open(sys.argv[2], "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("wrote %s (%d models)" % (sys.argv[2], len(doc["models"])))
    else:
        for p in sys.argv[1:]:
            run(p)
