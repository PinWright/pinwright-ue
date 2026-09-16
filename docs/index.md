---
type: index
summary: Public documentation index for PinWright, with internal-only docs grouped separately.
date: 2026-06-16
tags: [docs, mcp, wiki-overlays]
---
# PinWright Docs Index

This index catalogs the public documentation for the PinWright plugin. The live wiki is served as a regular MCP tool — call the wiki-fetch RPC for the root index. The hand-written overlay sources live under [wiki-src/](wiki-src/). Internal development-process docs are listed in the final section and are not included in release packages.

## Core References

| Document | Summary |
|---|---|
| [architecture](arch.md) | Plugin architecture: MCP transport, JSON-RPC 2.0 envelope, dispatcher, catalog/wiki, handlers, jobs, and state. |
| [MCP transport](wiki-src/mcp-transport.md) | Maintainer-level transport reference: envelope, protocol methods, the single `call` tool and its three argument shapes, result wrapping. Agents should use their MCP client, not raw HTTP calls. |
| [reflection-invocation](reflection-invocation.md) | Rules for invoking a UFunction via ProcessEvent — FProperty init/destroy lifecycle, CPF_OutParm classification, CDO refusal, any-UObject path resolution. |
| [engine version support](engine-version-support.md) | What builds and is tested today (UE 5.8), the deferred 5.3-5.7 intent, the symbol-level backport blockers with call sites and first-available versions, and the latent engine crash sites recorded with their reachability precondition. |

End-user installation and security guidance now live in the plugin [README](../README.md).

## IR Authoring

| Document | Summary |
|---|---|
| [IR authoring guide](ir-authoring.md) | Shared contract for new IRs: ephemeral text, IrCore helpers, grammar target, diagnostics, sidecar/RPC dual-surface rule, testing patterns, and BPIR/MGIR/AGIR examples. |
| [CRIR language reference](crir-language-reference.md) | Control Rig IR: `rig_graph` / `rig_function` / `rig_hierarchy` + nested `rig_subgraph` blocks, full RigVM opcode set (`unit`, `var`, `reroute`, `comment`, `if`, `select`, `enum`, `invoke_entry`, `template`, `dispatch`, `collapse`, `function_ref`, `function_entry`, `function_return`), mutating `control` element with typed-prefix value grammar and `FRigControlSettings` sub-block. |

## BPIR

| Document | Summary |
|---|---|
| [BPIR language reference](wiki-src/bpir.md) | Blueprint Intermediate Representation syntax, entry forms, instructions, type forms, examples, and gotchas. |
| [BPIR examples](wiki-src/bpir.examples.md) | Example BPIR snippets with feature coverage notes. |

## PinWright Model

| Document | Summary |
|---|---|
| [pwmodel format reference](pwmodel-format.md) | Normative `.pwmodel` reference: lexical rules, document structure, reserved constructs, op vocabulary and its divergences from engine naming, the materials / uv / color / lightmap / collision contracts, the `PWMODEL_*` diagnostic set, the compile surface and provenance rule, and the version-0 policy. |
| [pwskel format reference](pwskel-format.md) | Normative `.pwskel` reference: version-0 bone hierarchy, local transforms, skeleton validation and compilation, asset provenance, RPC surfaces, and the shared and skeleton-specific diagnostic catalogs. |
| [pwanim format reference](pwanim-format.md) | Normative `.pwanim` reference: compiled-skeleton references, timebase and key grammar, easing, dense baking, loop seams, RPC surfaces, and the shared and animation-specific diagnostic catalogs. |
| [pwmodel design](pwmodel-design.md) | Why a generative mesh format is not an IR, the geometry-op extraction and compiler decisions, the data-not-code stance, CAD feature-tree prior art, derived-asset ownership, scope boundaries, and the M1-M3 roadmap. |
| [ADR 0001: one file, one asset](adr/0001-one-file-one-asset.md) | Decision record: a `.pwmodel` file produces exactly one `.uasset`; options considered and consequences accepted. |

## Workflow Topics

| Document | Summary |
|---|---|
| [workflow hub](wiki-src/workflows.md) | Task-oriented entry point for cross-namespace RPC workflows. |
| [asset audit](wiki-src/asset-audit.md) | Build dump and reference evidence before asset analysis or edits. |
| [safe mutation and save](wiki-src/safe-mutation-save.md) | Read-edit-verify-save loop for persistent editor changes. |
| [visual review](wiki-src/visual-review.md) | Choose the correct screenshot or capture surface for visual proof, compare two or more subjects in one shared scene, and take a multi-angle set without hand-computing a pose. |
| [visual review: model rig](wiki-src/visual-review.model-rig.md) | The cheapest-first diagnosis order for a suspected geometry defect, the neutral and UV-checker review materials, the light rig to shoot a compiled model under, and what the Static Mesh asset preview's own lighting does to each artefact class. |
| [runtime UObject inspection](wiki-src/runtime-uobject-inspection.md) | Inspect live PIE subsystems and transient UObjects through typed RPCs. |
| [level building](wiki-src/level-building.md) | Take a level from nothing to playable: palette material instances, batch and instanced placement, PCG generation, terrain conform and height probing, terrain/water material round-trips, scatter, captures, and idempotent build scripts. |
| [level building: terrain and water](wiki-src/level-building.terrain-and-water.md) | Ground height queries, conforming placements to terrain, and the landscape/water material edits that only reach the screen after a rebuild. |
| [level building: instancing and scatter](wiki-src/level-building.instancing-and-scatter.md) | HISM and foliage instancing, PCG generation, Python transform traps, and scatter spacing that reads as deliberate. |
| [level building: capture and review](wiki-src/level-building.capture-and-review.md) | Pinned exposure, whole-level framing, orthographic capture, and what a review set must contain. |
| [level building: build scripts](wiki-src/level-building.build-scripts.md) | Outliner organisation and tags, one script per layer, modal-dialog deadlocks, and saving or dirtying packages. |
| [level review](wiki-src/level-review.md) | Prove a level is right: look before you measure, pinned exposure, top-down framing math, a fixed multi-range pose set, motion sampling, z-fighting checks, and numeric placement verification. |
| [level review: framing math](wiki-src/level-review.framing-math.md) | Orthographic capture limits, the two field-of-view formulas, and empirical screen-axis verification before measuring pixels. |
| [capture subjects](wiki-src/render.capture-subjects.md) | The `subject` descriptor shared by the capture verbs: kind inference, the per-kind bounds sources, when the `subject` response block is omitted, the typed refusal for a kind with no time axis, the reproducibility position on particle captures, and which view-mode capabilities the scene-capture renderer can and cannot reach. |
| [capture exposure](wiki-src/render.capture-exposure.md) | Pinning exposure on the capture itself: what `viewport.exposure` measures field by field, why `adapted` is a gain and `ev100Equivalent` is the number to feed back, the auto-once-then-pin recipe, the comparison tolerance a correct pinned pair still needs, and the stop of darkness in the first frame from a fresh preview window. |
| [capture view modes](wiki-src/render.view-modes.md) | The `viewMode` capture parameter: the five diagnostic modes that earn their keep, the reflection-derived spelling rules, the four typed refusals that stop a mode falling back to Lit silently, and the measured `viewport.viewModeOverride` block. |
| [preview-scene rig](wiki-src/render.preview-scene-rig.md) | The `previewScene` capture parameter: key aim, intensity and colour, sky, floor and backdrop of an asset-editor preview scene, scoped to one capture and restored at three levels; the arrival-azimuth convention and the engine default that proves it; the measured `viewport.previewScene` block; why per-capture profile selection is refused; and what a profile does to an exposure pin. |
| [support](wiki-src/support.md) | Draft a bug or feature report and open a prefilled GitHub draft for the public support tracker; the human always approves and submits. |
| [running unattended](wiki-src/unattended.md) | Why a modal dialog wedges the game thread, the launch flags and settings that prevent one, the non-retryable `EDITOR_BLOCKED_ON_MODAL` report, and what still needs a human. |
| [synth cookbook](wiki-src/audio.synth.cookbook.md) | Building each SFX family (impacts, explosions, creatures, magic/UI) with the `audio.synth` recipe grammar: generator choice, envelope shapes, FX order, starting values, target metric ranges, and the per-generator `gainDb` and envelope-relativity traps. |

## Spatial and Level Authoring

| Document | Summary |
|---|---|
| [spatial authoring](spatial-authoring.md) | The closed capture → raycast/place → measure/verify → adjust loop for level prototyping, plus the procedural mesh-authoring path (append_buffers / measure / check_health / OBJ-STL round-trip) and the design rationale for stated relations + deterministic measurement over guessed coordinates. Anchors the `spatial`, `camera`, `render`, `actor`, and `geometry` namespaces. |

## Wiki Overlays

| Area | Pages |
|---|---|
| Wiki usage | [wiki README](wiki-src/README.md), [wiki](wiki-src/wiki.md) |
| Core editor control | [actor](wiki-src/actor.md), [editor](wiki-src/editor.md), [level](wiki-src/level.md), [level.structure](wiki-src/level.structure.md), [property](wiki-src/property.md), [system](wiki-src/system.md), [system.inspect](wiki-src/system.inspect.md) |
| Blueprint and BPIR | [blueprint](wiki-src/blueprint.md), [blueprint.graph](wiki-src/blueprint.graph.md), [blueprint.scs](wiki-src/blueprint.scs.md) |
| Assets and content | [asset](wiki-src/asset.md), [material](wiki-src/material.md), [material.authoring](wiki-src/material.authoring.md), [material.graph](wiki-src/material.graph.md), [material.mgir](wiki-src/material.mgir.md), [texture](wiki-src/texture.md) |
| Audio | [audio](wiki-src/audio.md), [audio.authoring](wiki-src/audio.authoring.md), [audio.synth](wiki-src/audio.synth.md), [audio.analysis](wiki-src/audio.analysis.md), [audio.music](wiki-src/audio.music.md) |
| UI and widgets | [ui](wiki-src/ui.md), [widget](wiki-src/widget.md) |
| Animation and sequencing | [anim](wiki-src/anim.md), [animation](wiki-src/animation.md), [animation.authoring](wiki-src/animation.authoring.md), [controlrig](wiki-src/controlrig.md), [mrq](wiki-src/mrq.md), [pose_search](wiki-src/pose_search.md), [sequencer](wiki-src/sequencer.md), [sequencer.component-bindings](wiki-src/sequencer.component-bindings.md), [skeleton](wiki-src/skeleton.md) |
| Niagara and effects | [niagara](wiki-src/niagara.md), [niagara.graph](wiki-src/niagara.graph.md), [effect](wiki-src/effect.md) |
| Environment and scene systems | [environment](wiki-src/environment.md), [landscape](wiki-src/landscape.md), [foliage](wiki-src/foliage.md), [lighting](wiki-src/lighting.md), [world_partition](wiki-src/world_partition.md), [volume](wiki-src/volume.md), [spline](wiki-src/spline.md), [geometry](wiki-src/geometry.md) |
| Spatial perception and capture | [spatial](wiki-src/spatial.md), [camera](wiki-src/camera.md), [image](wiki-src/image.md) |
| PinWright Model | [model](wiki-src/model.md), [model.authoring](wiki-src/model.authoring.md), [model.vertex-color](wiki-src/model.vertex-color.md) |
| Gameplay namespaces | [ai](wiki-src/ai.md), [behavior_tree](wiki-src/behavior_tree.md), [character](wiki-src/character.md), [game_framework](wiki-src/game_framework.md), [gameplay_tags](wiki-src/gameplay_tags.md), [gas](wiki-src/gas.md), [input](wiki-src/input.md), [interaction](wiki-src/interaction.md), [navigation](wiki-src/navigation.md), [networking](wiki-src/networking.md), [physics](wiki-src/physics.md) |
| Data and utilities | [container](wiki-src/container.md), [container.array](wiki-src/container.array.md), [container.map](wiki-src/container.map.md), [container.set](wiki-src/container.set.md), [localization](wiki-src/localization.md), [misc](wiki-src/misc.md), [performance](wiki-src/performance.md), [pipeline](wiki-src/pipeline.md), [python](wiki-src/python.md), [render](wiki-src/render.md), [session](wiki-src/session.md) |

## Tag Index

Use [tags.md](tags.md) for reverse lookup by topic.

## Internal (not included in release packages)

Maintainer- and development-process docs. They reference the development host project and internal workflows, and are stripped from release packages.

| Document | Summary |
|---|---|
| [docs schema](SCHEMA.md) | Documentation conventions for this directory: layout, page types, frontmatter fields, the `wiki-src/` overlay shape and its rendering limits, and the index/tag maintenance steps. |
| issue board (the public repo `PinWright/pinwright-board`, cloned as `../../../.pinwright-board` relative to the plugin directory, so outside this repo) | Issue board, one markdown file per ticket; workflow rules and frontmatter schema live in its own README. |
| [RPC design](rpc-design.md) | Living design rules for new RPC verbs, each earned from a shipped defect: response honesty, structural guarantees, required parameters, independent verification, persistence levels, batching/jobs/cancellation, tick safety, error codes, failure-direction tests, plus a pre-ship checklist. |
| [lessons](lessons.md) | Append-only operational lessons learned across the plugin (compiler, handlers, tests, engine API quirks). |
| [multi-checkout setup](multi-checkout-setup.md) | Set up a second local checkout of the host UE project that shares LFS storage and DDC with the primary — for parallel plugin development on the same branch. |
| [release checklist](release-checklist.md) | Packaging validation steps for cutting a release. |
| [BPIR compiler internals](bpir-compiler-internals.md) | Two-phase compile architecture, resolvers, rollback, layout engine, and design decisions. |
| [BPIR test matrix](bpir-test-matrix.md) | BPIR test coverage matrix by feature × layer, with known gaps. |
| [test organization](test-organization.md) | Test category taxonomy, placement rules, file layout, naming conventions. |
| [error code catalog](error-code-catalog.md) | Generated inventory of handler error codes for the registry migration. |
| [GeometryScript debug-sink sweep](geometry-debug-sink-sweep.md) | Every GeometryScript call site in `PinWrightGeometry` with a verdict on its `UGeometryScriptDebug*` argument: wired, unreachable, guarded, or no channel. Read before adding a GeometryScript call. |
| [UE async completion delegates](ue-async-completion-delegates.md) | Editor completion hook reference for async RPC handlers. |
| [widget geometry resolver](widget-geometry-resolver.md) | Tiered UMG widget geometry measurement internals. |
| [layout-quality integration](layout-quality-integration.md) | Deferred L3 step: how to wire node-graph layout-quality checks into mcp-test-workflow once the layout RPC + engine tickets are DONE. |
| [engine research: 2026-08 plugin pass](engine-research-2026-08-plugin-pass.md) | Engine-source findings from the 2026-08 pass, recorded against the engine version they were read from. |
| [RPC hard-removal rejected candidates](rpc-hard-removal-rejected-candidates.md) | Deferred RPC cleanup candidates and the generic parity gaps blocking hard removal. |
| [format plan decisions](format-decisions.md) | User decisions binding the unified format plan (pwmodel, pwanim, shared core). Where a plan section disagrees with this page, this page wins. |
| [pwmodel value system](pwmodel-value-system.md) | Value-system and grammar work for pwmodel: nested lists for a real loft, map-as-pair-list, named weight-map handles, `cut_material`, and the format-neutral parser core. |
| [pwmodel emitter and migration](pwmodel-emitter-and-migration.md) | The `.pwmodel` emitter, the shared `PwText` core in the main module, the multi-line bracketed-list grammar change, the `model.format` verb, and the version/migration machinery. |
| [pwmodel skeleton, skin and use-from](pwmodel-skeleton-skin-use.md) | Implementation plan for the `skeleton` and `skin` blocks and the `use … from` reference form, with brief verification, dependency ordering, and seven bugs found. |
| [pwanim animation format](pwanim-animation-format.md) | The `.pwanim` source format: decisions D1-D7, grammar, the text core shared with pwmodel, the bake-and-write path, dependency order, the T1-T15 test strategy, and what cannot be built yet. |
| [defect backlog](defect-backlog.md) | Every verified unfixed PinWright defect, grouped by subsystem, assigned to an owning plan section, ordered by hard dependency then severity, plus the ten seed claims rejected as already-fixed or non-existent. |
| [capture subject convergence](capture-subject-convergence.md) | Converging the eight capture verbs onto one viewport primitive plus a pluggable subject resolver, so every capture capability reaches every subject domain. Corrected reachability matrix, the `FCaptureSubject` contract, thirteen file-disjoint chunks, and what is structurally excluded. |
| [preview-scene rig](preview-scene-rig.md) | The scoped `previewScene` capture parameter and the `viewport.previewScene` report: the frozen contract, the three-level restore that disarms two engine-driven mutations (a shared-profile write and a committed-config write), `viewMode` on the ortho path, the widget-designer alpha stamp, the cross-verb parameter-parity gaps, and six refusals with the source behind each. |
| [floating geometry audit](floating-geometry-audit.md) | Warning-only isolation checks for disconnected static-model and skeletal-animation components, with scale-derived proximity thresholds, structured reports, and synthetic verification. |
| [geometry audit component winding](geometry-audit-component-winding.md) | Component-aware winding verdicts for static-mesh audits, preserving whole-mesh measurements while distinguishing answered, inverted, and unknown components. |
| [geometry audit z-fighting fixer](geometry-audit-z-fighting-fixer.md) | Repair plan for z-fighting measurements and synthetic fixtures, including unique projected-union area and fresh scoped verification. |
| [geometry audit z-fighting review](geometry-audit-z-fighting-review.md) | Completion review for warning-only z-fighting detection, scale-derived thresholds, candidate filtering, reporting, and shared verdict behavior. |
| [geometry audit z-fighting](geometry-audit-z-fighting.md) | Design plan for warning-only static-mesh z-fighting detection with spatial hashing, exact projected-overlap tests, ranked evidence, and failure-direction tests. |
