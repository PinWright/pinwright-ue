# asset-audit

Workflow for turning binary content packages into textual evidence before analysis, review, or edits. It uses only current asset dump, reference, and job surfaces.

## When To Use This

Use it to inspect authored Blueprint, Widget Blueprint, material, level, Niagara, texture,
DataTable, or animation state; compare files after an edit; trace references; and avoid
acting on stale memory instead of the current dump.

## Recipe

1. Pick scope.
   - One package: `call("asset.dump", {"assetPath": "/Game/..."})`.
   - Folder: `call("asset.dump_folder", {"folderPath": "/Game/...", "recursive": true})`.
2. Include levels only when you intend to inspect maps. Folder dumps skip UWorld packages by default; pass `includeLevels: true` when map contents are part of the audit.
3. Include Widget Blueprint preview PNGs only when visual review matters. `includeWidgetScreenshot: true` writes `preview.png` for supported Widget Blueprint dumps, but opening designers is slower than a pure text dump.
4. Wait for async work. Streaming MCP calls block and stream progress by default. Pass `wait:false` to `asset.dump_folder` or UWorld `asset.dump` when you need an immediate `ticket_id`, then poll `call("system.job_status", {"ticket_id": "..."})` until terminal. Otherwise consume the streamed final result directly.
5. Read the dump mirror under `<ProjectSavedDir>/PinWright/asset-dumps/` and use the sidecar that matches the asset type.
6. If the audit leads to an edit, switch to [`safe-mutation-save`](safe-mutation-save.md) before mutating.

## Where To Look In The Dump

| Asset type | Main dump files | Next wiki page |
|---|---|---|
| Blueprint | `meta.json`, `properties.json`, `bpir.txt`, `scs.json` | [`blueprint`](blueprint.md), [`blueprint.scs`](blueprint.scs.md) |
| Widget Blueprint | `tree.xml`, `widget_animations.json`, `bpir.txt`, optional `preview.png` | [`widget`](widget.md), [`visual-review`](visual-review.md) |
| Material or material function | `mgir.txt`, `properties.json` | [`material`](material.md), [`material.mgir`](material.mgir.md), [`material.authoring`](material.authoring.md) |
| Level / UWorld | `world_settings.json`, `level_bp.txt`, `sublevels.json`, `actors/` | [`level`](level.md), [`actor`](actor.md) |
| Niagara | `nir.txt`, `niagara_parameters.json`, `niagara_stack.json`, `niagara_graphs.json`, `niagara_compile.json` | [`niagara`](niagara.md) |
| DataTable | `data_table.json` | [`asset`](asset.md) |
| Texture | `texture.json` | [`texture`](texture.md) |

## Dump Sidecar / Live-Read Parity

When you need the same JSON shape a dump sidecar emits but without writing a cache entry, prefer the matching live `*.describe` RPC over re-deriving the data from `call("system.inspect")` / `call("property.get")`. Both surfaces delegate to the same exported `*DumpBuilder::Build*Json` function, so they cannot drift.

| Sidecar | Live read |
|---|---|
| `data_table.json` | `data_table.list_rows` |
| `static_mesh.json` | `static_mesh.describe` |
| `skeletal_mesh.json` | `skeleton.describe_mesh` |
| `sound_wave.json` | `audio.authoring.describe_sound_wave` |
| `sound_cue.json` | `audio.authoring.describe_sound_cue` |
| `metasound.json` | `audio.authoring.describe_metasound` |
| `anim_sequence.json` | `animation.describe_sequence` (also: `animation.authoring.list_curves` / `list_notifies` / `list_sync_markers` for sub-arrays) |
| `texture.json` | `texture.describe` |

`material_instance.json` is NOT a sidecar — `DumpFileNames` has no entry and `BuildAllFilesForAsset` (`Handlers/Asset/AssetDumpHandler.cpp`) has no `UMaterialInstance` branch. Material instances fall through to the default tail and emit only `properties.json`. The read surface for material instances is `material.authoring.get_material_instance_info` — do not add a `material_instance.describe` RPC.

`cascade.json` has no live-read counterpart by design — Cascade has been deprecated since UE 4.20 and read-only inspection is sufficient.

Naming convention for new describe RPCs: asset-type namespace + `describe` verb, mirroring the sidecar filename (e.g. `static_mesh.json` → `static_mesh.describe`). Avoid domain-grouping namespaces like `mesh.describe_static` when an asset-type namespace already aligns with the sidecar name. (`data_table.json`'s live read is the pre-existing `data_table.list_rows`, which returns the full dump shape despite the `list_rows` name.)

## Diff Review

For a single package, take a normal baseline dump first, then call `asset.dump` with `diff: true` after the change. Diff mode leaves the baseline mirror untouched and writes `_new` and `_diff.txt` files for changed aspects to a per-asset directory under `Saved/PinWright/asset-dump-diffs/<PackagePath>/` (each diff run replaces the asset's previous diff directory). A later normal `asset.dump` promotes the current state to the baseline and deletes the stale diff directory.

Folder sweeps always write normal baseline output. Use a before/after filesystem diff around the dump folder when auditing a subtree.

## Determinism And State-Only Noise

Persistent sidecars are authored-state mirrors. The dumper avoids the known fresh-session noise sources that previously affected `/App` refreshes:

- A texture whose platform data is still compiling is deferred during folder sweeps. Direct `texture.describe` / `asset.dump` calls return `ASSET_COMPILING`; they do not serialize Unreal's temporary `32x32` / `PF_B8G8R8A8` stand-in. Folder sweeps preserve the prior dump and report `ASSET_COMPILE_TIMEOUT` if compilation does not finish within 120 seconds.
- Generic property export omits transient/deprecated/skip-serialization fields and exact known derived caches such as static-mesh cached counts, material texture-streaming data, MovieScene signatures, and Niagara compiled-data fields. Set values and known unordered sidecar collections are serialized in stable order.
- Persisted Niagara compile/NIR sidecars contain authored identities, graphs, parameters, issues, and true rapid-iteration overrides. Live compile readiness/status remains available through `niagara.inspect` and `niagara.validate`, not the git mirror.
- Baseline writes are content-aware and transactional. Byte-identical text and binary files keep their mtimes; changed files are staged before commit; stale sidecars are pruned only after successful writes; an incomplete rollback preserves its recovery directory.

For a strong determinism check, run a forced sweep, capture the dump-tree diff or hashes, restart the editor, and force the same sweep again. A cache-hit-only second pass proves cache convergence, not regeneration determinism. Review any residual diff against source-asset history before committing it.

The July 2026 `/App` validation used that stronger check: two cold forced sweeps over 8,192 assets produced the same full dump-diff hash, and the following incremental cache pass reported all 8,192 assets unchanged. That result validates the current schema against this corpus; repeat the cold forced comparison when serializer behavior or aspect versions change.

## Reference Impact

Use current reference readers before destructive package operations:

- `asset.map_references` for the same soft-UWorld reference shape written to `map_references.json`.
- `asset.get_dependencies` or `asset.get_dependencies_classified` to understand dependency direction from one asset.
- `asset.references` or `asset.get_asset_graph` when the question is broader than one sidecar.
- `asset.fixup_redirectors` only when the audit finds redirectors that should be cleaned up; re-dump the subtree afterward so stale redirector dump folders are reconciled.

## Asset Cache Vs Live State

Use the dump mirror when the package on disk is the source of truth and you need repeatable, greppable evidence. Use `call("system.inspect")`, `call("actor.describe")`, `call("property.get")`, or `call("property.list")` when the live editor or PIE instance is the source of truth.

## Related Pages

- [`asset`](asset.md) for exact dump parameters and sidecar schemas.
- [`safe-mutation-save`](safe-mutation-save.md) for the edit/save loop after an audit.
- [`visual-review`](visual-review.md) for screenshots and preview captures.
- [`workflows`](workflows.md) for the index of task-oriented guides this one belongs to.
