---
type: reference
summary: "Component-aware winding verdicts for static-mesh audits, preserving whole-mesh measurements while distinguishing answered, inverted, and unknown components."
date: 2026-08-23
tags: [geometry, audit, static-mesh, component-winding, verification]
---

# Geometry audit component-winding fix

Status: implementation complete; link build and runtime relink remain pending

1. Reproduce `geometry.audit_static_meshes` on the current editor and inspect the
   checked-in implementation, audit framework, tests, and geometry wiki contract.
2. Replace whole-mesh winding verdict logic with connected-component analysis,
   retaining a whole-mesh signed-volume figure when it is inexpensive. Classify
   every component as answered, inverted, or unknown using a scale-derived
   tolerance rather than a mesh-size magic number.
3. Make the RPC response and audit verdict distinguish answered components from
   unknown components; preserve `FVerdict::DerivePass`, the framework check table,
   and the shared `passRule`.
4. Add failure-direction tests for a balanced correct/flipped pair, a degenerate
   component, malformed arguments, and bucket reconciliation. Update general
   geometry documentation to describe the opposite blind spots of signed-volume
   and visual front/back checks.
5. Compile-check every touched C++ translation unit, inspect the nested-repo diff,
   and commit only task-owned paths with an `UNBUILT:` subject. Do not link, restart
   the editor, push, or edit files owned by another agent.

## Unresolved Questions

- Should a future version expose the scale-derived near-zero tolerance as an RPC
  option, or remain an implementation detail until callers request per-asset
  tuning?
