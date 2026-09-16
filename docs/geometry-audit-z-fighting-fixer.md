---
type: reference
summary: "Repair record for z-fighting measurements and synthetic fixtures, including unique projected-union area and fresh scoped verification."
date: 2026-08-23
tags: [geometry, audit, static-mesh, z-fighting, testing, verification]
---

# Geometry audit: z-fighting fixer plan

## Work list

1. Repair the synthetic static-mesh audit helper so the default-on `z_fighting` check receives the
   same measured triangle evidence as production measurement.
2. Replace pair-summed fighting area with unique projected-union area per connected fighting region,
   and add a multi-surface regression fixture.
3. Run the scoped geometry automation coverage and a fresh single-file compile where the shared
   editor/build state permits it. Attempt live acceptance only with a freshly linked plugin.
4. Record any runtime or performance evidence as demonstrated, inferred, or deferred; do not treat
   the older running editor DLL as proof of this source tree.

## Unresolved Questions

- Can the shared editor be relinked safely during this run, or must runtime acceptance wait for a
  fresh plugin link window?
- Which saved mesh is largest after the current LOD/read settings, and can it be measured without
  disturbing another agent's editor session?
