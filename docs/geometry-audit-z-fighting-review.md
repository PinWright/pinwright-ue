---
type: reference
summary: "Completion review for warning-only z-fighting detection, scale-derived thresholds, candidate filtering, reporting, and shared verdict behavior."
date: 2026-08-23
tags: [geometry, audit, static-mesh, z-fighting, testing, verification]
---

# Z-fighting audit completion review

## Scope

Review and repair the preserved warning-only z-fighting implementation in the nested PinWright repository. Preserve unrelated floating-geometry/model/compiler edits and do not commit, push, launch, or terminate processes.

## Review steps

1. Re-read the live nested diff and trace the static-mesh audit, decomposition, shared check-table, verdict, serialization, and error paths.
2. Validate the detector's scale-derived epsilon, spatial broad phase, exact projected-overlap tests, anti-parallel handling, transverse rejection, and region ranking.
3. Validate argument validation and bucket invariants before the sweep, warning severity, `FVerdict::DerivePass`, and model/static-mesh integration.
4. Repair only task-owned z-fighting files and tests, keeping existing floating-geometry/model changes intact.
5. Run bounded source/static checks and, if immediately safe, one scoped compile or automation interval; report exact evidence and any unverified runtime gaps.

## Acceptance checklist

- [ ] Cross-component/part near-coplanar projected-overlap pairs are detected in expected-linear candidate search.
- [ ] Parallel and anti-parallel normals are accepted; separated, side-by-side, and transverse triangles are rejected.
- [ ] Scale-free epsilon, proxy limitation, overlap area, pair/triangle/component counts, and ranked regions are reported.
- [ ] Warning-only audit behavior, bucket sums, argument errors, and shared verdict derivation are preserved.
- [ ] Synthetic tests explicitly split quads into triangles and cover coincident, separated, side-by-side, transverse, and anti-parallel directions.
- [ ] Diff, static checks, and bounded verification evidence are recorded without claiming unverified runtime/build results.

## Unresolved Questions

- None expected for this bounded repair; if live verification is unavailable, only the evidence classification remains to be reported.
