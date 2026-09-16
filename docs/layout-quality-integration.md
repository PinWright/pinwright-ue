---
type: guide
summary: "How to finish wiring node-graph layout-quality checks into mcp-test-workflow once the prerequisite layout RPC + engine tickets are DONE. This is the deferred L3 step."
tags: [layout, layout-quality, test-workflow, mcp-test-workflow, integration, deferred]
---

# Finishing the layout-quality integration (L3 wiring)

This is the **deferred half of L3** from the layout-quality effort. The C++ pieces
(metrics core, engine improvements, the `layout_report` / `auto_layout` RPCs) are
filed as fix-workflow tickets; this doc is the remaining **`mcp-test-workflow`
skill change** that turns those RPCs into a continuous runtime consumer. Without
this step, the `layout_report` RPC has no caller and poor layouts on
agent-generated graphs still ship unnoticed.

Do this **only after** the prerequisite tickets below are `DONE`. Until then, the
metrics core still does its job through the L2 engine regression tests (the unit
suite guards layout quality); this step adds the *runtime* catch.

## Prerequisites (all must be `DONE` on the board)

- `F-graph-layout-metrics-core` (L1) — the `FGraphLayoutMetrics` util + node-size
  estimator, **and its calibration pass** (this is where the score thresholds come
  from — see below).
- `F-graph-layout-report-rpc` (L3) — `blueprint.graph.layout_report`,
  `material.graph.layout_report`, `anim.graph.layout_report`,
  `controlrig.graph.layout_report`.
- `F-blueprint-auto-layout-rpc`, `F-anim-auto-layout-rpc`,
  `F-controlrig-auto-layout-rpc` (L3) — standalone re-flow RPCs.
  (Material already ships `material.authoring.auto_layout`.)
- `F-{material,anim,controlrig}-layout-bounds-aware` (L2) — so that calling
  `auto_layout` actually produces a good layout, not just a measured-bad one.

**Readiness check:** with a live editor, call each `*.layout_report` on a known
graph and confirm it returns a score + flagged nodes/edges; call each
`*.auto_layout` on a messy graph and confirm `layout_report` improves afterward.

## The calibrated threshold (do not invent it)

The floor below which a graph is "poor" is **a deliverable of the L1 calibration
pass**, not a guess. L1 scores two corpora — human-authored graphs (project
Blueprints, Content-Examples) as the "good" reference and current MCP/auto-layout
output as "to-improve" — and derives:

- **Overlap is an absolute fail** (any node bbox intersection > 0 → poor).
- The relative metrics (spacing / straightness / crossings) get their floor from
  roughly the **P10 of the human-authored distribution**.

Record the resulting threshold(s) where the L1 work puts them (a settings entry or
a constant in the metrics core) and have the test-workflow read/restate that value
— never hardcode a fresh number here.

## The wiring (mcp-test-workflow, fuzz-host repo)

This mirrors how the **CorruptionCheck** phase was added (see
`mcp-test-workflow.workflow.js`): a bounded check that only fires when the Attempt
actually touched a graph, integrated through the existing single board-writer.

1. **Pre-flight (MANDATORY).** Sync all four fuzz hosts first — see the fuzz
   pre-flight rule (push/clean all four review checkouts, rebase if
   needed) so the skill edit starts from one clean base. Stop the workflows first.

2. **Track touched graphs.** The Attempt already records touched asset paths
   (`assetPath` on `calls[]`, added for corruption detection). Reuse that: a graph
   edit/creation call carries the owning asset path. Collect the distinct
   graph-bearing assets the Attempt touched (Blueprint / Material / Anim /
   ControlRig).

3. **A `LayoutCheck` step** (fold into the existing flow — either a small phase
   after Attempt like CorruptionCheck, or inside the Audit pass). For each touched
   graph asset:
   - Call the matching `*.layout_report`.
   - If the score is **at/above** the calibrated floor and overlap is 0 → clean,
     nothing to do.
   - If **below** the floor → call the matching `*.auto_layout` to re-flow, then
     re-call `layout_report`.
     - If it now passes → the graph was just un-laid-out; the agent should have
       called auto_layout itself. File **nothing** (or, optionally, a low-severity
       ergonomic note that auto_layout wasn't invoked during authoring).
     - If it **still** fails after auto_layout → this is a real defect: either an
       engine gap (the bounds-aware engine can't resolve it) or hand-placed nodes
       the re-flow won't touch. File a finding.

4. **File a `layout-quality` finding** through the existing Judge/Audit board
   writer (single-writer invariant — do not write the board from a parallel step).
   Use category `ergonomic` (`E-`), `tags: [layout, layout-quality]`, severity per
   the board README rubric (a graph that stays poor after auto_layout is Medium
   friction at most — it still functions). Body: the asset + graph, the
   `layout_report` score + the specific flagged overlaps/edges, and whether
   auto_layout was tried. Dedup first (a recurring poor-layout pattern appends
   evidence to an existing ticket, it does not spawn duplicates).

5. **Bounded cost.** The check only runs for Attempts that touched a graph;
   `layout_report` is cheap (no editor restart needed, unlike CorruptionCheck), so
   this can run every such iteration.

6. **Recompile + propagate.** `npx polyskill` in the source fuzz repo's
   `.polyskill`, commit the exact skill paths, push to the hub, then the other
   three `git pull --rebase` + `npx polyskill`.

## Verification

- With a live editor, build a small graph and deliberately place two nodes
  overlapping. Run one test-workflow iteration. Confirm: `layout_report` flags it,
  `auto_layout` re-flows it, and **no** finding is filed (it recovered).
- Repeat but make the graph un-fixable by auto_layout (e.g. an engine still leaves
  overlap on that graph shape). Confirm a `layout-quality` `E-` finding is filed
  with the score + flagged elements.
- Confirm a clean, well-laid-out graph produces no finding (no false positives).

## Why this was deferred

The metrics RPC is only worth its weight with a live consumer; that consumer is
this test-workflow wiring. It was split out so the RPC and its consumer ship as a
pair, and so the wiring is built against **real calibration thresholds** from L1
rather than guessed ones. The trigger to do this work is the prerequisite tickets
above reaching `DONE`.
