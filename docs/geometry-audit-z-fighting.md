---
type: system
summary: "Design record for warning-only static-mesh z-fighting detection with spatial hashing, exact projected-overlap tests, ranked evidence, and failure-direction tests."
date: 2026-08-23
tags: [geometry, audit, static-mesh, z-fighting, testing, verification]
---

# Geometry audit: z-fighting validation

## Summary

Add a warning-only `z_fighting` check to `geometry.audit_static_meshes`. It detects overlapping,
near-coplanar triangles from different connected components, reports scale-derived thresholds and
ranked regions, and remains safe for synthetic automation tests.

## Decisions

- Reuse `MeshAudit::MeasureComponents` and carry triangle positions, unit normals, bounds, and
  component ids into a pure detector; do not add host-content fixtures or model-compiler logic.
- Use a uniform spatial hash over expanded triangle bounds for candidate generation, then apply
  unit-normal alignment, two-sided plane-distance, and exact 2D projected triangle clipping. Fine
  cells, coarse references inspected by each large triangle, candidate pairs, and exact tests all
  have fixed limits; exceeding a limit fails closed as unrunnable rather than reporting clean.
- Derive plane epsilon as a fixed fraction of the measured model extent and publish both the
  epsilon and normal-dot threshold. This is a geometric proxy for depth-buffer precision, not a
  universal camera prediction.
- Aggregate connected fighting-pair evidence into ranked regions and report pair/triangle/
  component counts plus overlap areas in the existing finding measurements.
- Keep the check severity at warning and derive `pass` through the existing shared audit verdict.

## Constraints

- Preserve the pre-existing dirty `CLAUDE.md` and `Source/PinWrightGeometry/Private/Handlers/Model/ModelCompileHandler.cpp`.
- Keep shipped code/docs/tests general-purpose: no host asset paths, host vocabulary, or absolute
  machine paths.
- Preserve the audit bucket identities and reject malformed RPC arguments before a sweep; do not
  turn content findings into RPC errors.
- No commits, pushes, branches, worktrees, full builds, or unrelated-file edits.

## Waves/Steps

1. Extend the mesh measurement model and connected-component read path with synthetic-testable
   triangle evidence and the spatial-hash z-fighting analysis.
2. Register the warning check, evaluate it through `FVerdict::DerivePass`, serialize ranked
   measurements, and add the registered finding code.
3. Add synthetic failure-direction tests for coincident, separated, side-by-side, transverse,
    and anti-parallel triangles; update general geometry audit documentation.
4. Re-read the final diff and run only scoped static checks or automation/compile checks that can
   produce exact evidence.

## Acceptance

- Coplanar overlapping and anti-parallel synthetic surfaces flag with the expected overlap area;
  separated, side-by-side, and transverse surfaces do not.
- The response exposes total/region overlap area, pair and triangle counts, component ids,
  ranking, model extent, plane epsilon, and normal threshold.
- Selected-check buckets satisfy `applicable + notApplicable == assetsExamined` and
  `flagged + unrunnable + clean == applicable`; z-fighting is warning severity, so
  `failOn:error` does not fail solely on it.
- Documentation explains bounded expected-linear broad-phase behavior, scale-derived epsilon, and
  the depth-buffer proxy limitation without host-specific examples.

## Final status

Implementation status: **IMPLEMENTED BUT NOT VERIFIED** in the shared evaluator and RPC response
surface. Compile, automation, build, packaged/runtime, and visual verification are
**NOT VERIFIED** because none were run in this change. The detector's broad-phase candidate count,
exact survivor count, grid statistics, inspected fallback references, scale-derived epsilon, normal
threshold, total overlap area, and ranked region/pair/component/triangle evidence are returned as
measurements. Detector failure or a mesh with no usable finite triangles is an unrunnable finding
with `MESH_AUDIT_Z_FIGHTING_UNRUNNABLE`, so it cannot be counted as clean.

The audit supplies geometric evidence only. It does not claim a universal camera-distance or
depth-buffer prediction. A clean audit result and a clean visual capture are therefore separate
acceptance signals.

## Unresolved Questions

- None; no user decision is required for this scoped implementation.
