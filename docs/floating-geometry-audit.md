---
type: system
summary: "Warning-only isolation checks for disconnected static-model and skeletal-animation components, with scale-derived proximity thresholds, structured reports, and synthetic verification."
date: 2026-08-26
tags: [geometry, audit, floating-geometry, static-mesh, animation, testing]
---

# Floating geometry audit

## Summary

Implement a warning-only spatial-isolation audit for static `.pwmodel` and skeletal animation geometry. Reuse the existing `MeshAudit::MeasureComponents` edge-connected component walk, then classify components with a scale-derived proximity graph. The largest component by triangle count is the model island; components in other graph islands are reported as floaters.

The static path will be available through the existing geometry audit and model compiler output. The animation path will evaluate actual `UAnimSequence` poses at requested samples and report each component's worst separation, while attributing bind-pose floaters to the model.

### Inferred decisions

- Use triangle count, rather than signed volume, to choose the largest component because open shells can have zero or unstable signed volume while triangle count remains directly measurable.
- Derive the default tolerance as `0.005 * model bounding-sphere radius` and expose the dimensionless fraction so callers can tune it per model without introducing unit-based thresholds.
- Add a component-specific `.pwmodel` suppression parameter (`allow_floating=true`) rather than a global audit disable. Suppressed components remain measurable and are marked in the report.
- Keep the check warning-only and default-enabled for model validation; it must never make a valid deliberately-floating asset an RPC or compiler error.
- Add the animation audit as a new geometry RPC surface if no existing skeletal audit has a suitable contract, using the shared audit table and verdict machinery.

## Decisions (confirmed)

- AABB bounds are only a pruning stage; surviving pairs use triangle-level nearest-distance queries.
- Every component reports triangle count, signed volume, centre, nearest other component, and nearest distance.
- Static analysis reports island count and all graph islands not containing the largest component.
- Animation samples every frame by default, accepts a caller stride, reports the actual sampled frame range/count, and records first and worst separating frames.
- Synthetic tests cover both accepting and failing directions for overlap, separation, a removed middle link, and a mid-sequence animation separation.
- Real-asset verification must catch the named known-bad siege asset and must not flag the three
  named many-shell tree assets.

## Constraints

- Git: inspect only; do not create branches, worktrees, commits, or pushes.
- Build: use the plugin's UE build path only when needed for verification; preserve the existing dirty sibling changes.
- Run: use the existing MCP/runtime surface for editor and asset checks; do not use raw HTTP or legacy RPC.
- Tests: add focused UE automation tests; run only scoped tests if the environment permits and record exact evidence.
- Search: use `rg`/`rg --files` for source and docs lookup.
- Legacy: leave existing component and interpenetration checks intact; extend shared utilities without deleting adjacent behavior.
- Review mode: simple, time-bounded self-review after implementation.
- Board mode: no; this is a freeform implementation request.

## Waves

### Wave 1 — shared measurement and static reporting

1. Extend `MeshAuditUtils` with component bounds/centres, exact component-pair distances, scale-free tolerance derivation, graph islands, and floating-component records. Keep `MeasureComponents` as the single component source.
2. Add the warning check to the shared audit table, handler parameters/results, and error-code/catalog documentation. Ensure `pass` is derived by `FVerdict::DerivePass`, buckets sum, and bad arguments fail before the sweep.
3. Feed the same analysis through `PwModelCompiler::ValidateMergedMesh` and expose the report in model compile JSON. Add specific part-level suppression and retain suppressed measurements in output.

### Wave 2 — animation and focused tests

1. Add the skeletal animation audit using actual posed vertex positions and the same graph classifier. Record bind-pose attribution, sampled-frame metadata, first separation, and worst separation per component.
2. Add synthetic static and animation fixtures and assert both pass/fail directions.
3. Update user-facing plugin docs with the tolerance derivation, warning semantics, suppression syntax, and sampling caveat.

### Wave 3 — verification

1. Compile the intended plugin targets and retain the required UBT `[1/1] Compile [x64] <file>` plus `Result:` evidence for any single-file claim.
2. Run the focused automation tests and inspect totals and log output.
3. Use the MCP client/runtime surface to audit and capture the siege asset with fixed exposure and multiple angles, then audit the three deliberately-many-shell tree assets. Report PASS, PARTIAL, or NOT VERIFIED separately for static, animation, and visual evidence.

## Chunk details

### Shared spatial classifier

- Files: `Source/PinWrightGeometry/Private/Handlers/Geometry/MeshAuditUtils.h`, `MeshAuditUtils.cpp`, `Source/PinWrightGeometry/Private/Handlers/Geometry/MeshAuditHandler.cpp`, `Source/PinWright/Private/Handlers/ErrorCodes.h`.
- Use the existing connected-component measurements, build per-component triangle AABB trees, prune with component boxes, and use real nearest-triangle distances for candidate pairs and nearest-neighbour reporting.
- Treat distance `<= tolerance` as linked; union linked components; select the largest triangle-count component's island as the model island.

### Model compiler integration

- Files: `Source/PinWrightGeometry/Private/Model/PwModelAst.h`, `PwModelCompiler.h`, `PwModelCompiler.cpp`, `Source/PinWright/Private/Handlers/Model/ModelCompileHandler.cpp`, `docs/pwmodel-format.md`.
- Run after merged geometry exists, emit warning diagnostics, and serialize the full measurement. Add a specific part suppression only; do not add a blanket check switch.

### Animation

- Files: existing animation handler/module files selected from the current PinWright surface, plus `MeshAuditUtils` and its tests.
- Pose the actual sequence at sampled frame times, skin each vertex using its real influences, rerun the graph per sampled frame, exclude bind-separated components from animation-caused findings, and report sampling metadata.

### Tests and docs

- Files: `Source/PinWrightGeometry/Private/Tests/Geometry/TestMeshAuditRules.cpp` and the relevant geometry/model docs.
- Use synthetic dynamic meshes and a two-bone sequence fixture; do not depend on host-project content in tests or shipped documentation.

## Verification acceptance

- Overlapping synthetic boxes produce one proximity island and no floating finding.
- Moved-apart boxes produce a warning with triangle count, volume, centre, nearest distance, and the expected failure direction.
- Removing the middle box from a three-box chain leaves the orphan side outside the main island while the linked chain remains accepted.
- The two-bone animation fixture is connected at bind pose and reports the first mid-sequence separation plus the worst distance; a sequence with no separation passes.
- Argument errors are RPC errors before measurement; an empty or unrunnable sweep cannot derive `pass=true`.
- The named siege asset is flagged, and the three named tree assets are not flagged by this check.

## Verification record

- Focused Unreal single-file compiles succeeded for the shared utility, static audit handler,
  animation audit handler, model compiler/handler, parser, and changed geometry automation test;
  the utility compile carries only the pre-existing `UStaticMesh::NaniteSettings` deprecation
  warning.
- The synthetic automation test source compiles, but it was not executed: the live editor was
  an older module instance and another client held the editor-use lease. The new test names were
  therefore not registered in that process (`NO_TESTS_MATCHED`).
- The named siege asset was captured through the live MCP surface with explicit camera
  location/rotation and four orbit angles. The orbit response reported `pinned: true` at EV100
  `-4`, and the images visibly show separated thin spikes/cones. The new numeric static audit
  could not be run in that stale process, so the three-asset static PASS/FAIL matrix remains
  unverified until a quiet editor can restart with the new module.

## Unresolved Questions

- None blocking. After real-asset measurements are available, should the project retain the default `0.5%` radius fraction or choose a different default while keeping the caller override?
