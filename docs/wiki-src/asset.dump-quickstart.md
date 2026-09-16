# Asset dump quickstart

Quick reference for invoking `asset.dump` / `asset.dump_folder`, choosing where output lands, and avoiding the gotchas that make a dump mirror misleading.

## Basic calls

Use the dump mirror before broad analysis. It converts binary `.uasset` state into stable text files that can be searched, diffed, and reused across a session. For the full cross-asset workflow, start with [`asset-audit`](asset-audit.md).

Single asset:

```json
{
  "path": "asset.dump",
  "args": {
    "assetPath": "/Game/UI/WBP_HUD"
  }
}
```

Folder sweep:

```json
{
  "path": "asset.dump_folder",
  "args": {
    "folderPath": "/Game/UI",
    "recursive": true
  }
}
```

The default output root is `<ProjectSavedDir>/PinWright/asset-dumps/` (configurable in Project Settings → Plugins → PinWright (Project)) and mirrors package paths: `/Game/UI/WBP_HUD` writes under `Saved/PinWright/asset-dumps/Game/UI/WBP_HUD/`.

Direct `asset.dump` always force-refreshes the requested asset and never returns a folder-cache hit. `asset.dump_folder` is incremental by default and skips unchanged assets when the private `.dumpcache.json` beside the asset dump still matches the saved package and expected sidecars.

`asset.dump_folder` and UWorld / `.umap` `asset.dump` use jobs. Streaming MCP calls block by default and stream progress to the final result; `wait:false` returns an immediate `ticket_id` to poll with `system.job_status` or read from `Saved/PinWright/jobs.jsonl`. Non-streaming clients receive the ticket normally. An all-cache-hit folder sweep still creates a job when `wait:false` is used.

Use `force=true` on `asset.dump_folder` to bypass cache hits and re-dump every eligible package. Use `includeLevels=true` only when you intentionally want maps; folder dumps skip levels by default. Use `diff=true` only with single-asset `asset.dump`; folder sweeps always write normal baseline output.

Specialized inspection pages:

- [`blueprint`](blueprint.md) — Blueprint class structure, BPIR, CDO defaults, references.
- [`blueprint.scs`](blueprint.scs.md) — Simple Construction Script component-template trees (`scs.json` in dumps).
- [`widget`](widget.md) — Widget Blueprint trees (`tree.xml` in dumps) and UMG event binding context.
- [`level`](level.md) — UWorld / ULevel package reads, world settings, sublevels, level blueprint, actors.
- [`system.inspect`](system.inspect.md) — live editor UObject / actor / class inspection when the active process is the source of truth.
- [`property`](property.md) — one-off live UPROPERTY reads/writes; use dump `properties.json` for repeatable baselines.

## Folder-dump level skipping (PKG_ContainsMap)

`ShouldSkipFolderDumpAsset` (`AssetDumpHandler.cpp`) skips two distinct shapes when `bIncludeLevels=false`:

1. The class-path filter (`AssetClassPath == World`).
2. **Any row whose package has the `PKG_ContainsMap` flag set**, regardless of asset class.

The second clause exists because non-World rows (typically `MapBuildDataRegistry`) get embedded in the level's own package. They previously slipped through the class-path filter, but their `Data.PackageName` resolves back to the World via `LoadObject` — which is what `includeLevels=false` was meant to prevent. Check the package flag, not just the class path.

Companion `_BuiltData` packages (which do NOT carry `PKG_ContainsMap`) must still be included. The regression test `FAssetDumpFolderSkipsExternalActorStorageTest` covers both halves with synthetic `FAssetData` rows.

## Property serializer dispatch coverage and transient skip

`PropertyUtils::ExportPropertyToJsonValue` dispatches the full UE numeric set — `FInt8`, `FInt16`, `FUInt16`, `FUInt32`, `FUInt64` alongside the previously covered `int32` / `int64` / `float` / `double` — and the full `FObjectPropertyBase` family (`FObjectProperty`, `FWeakObjectProperty`, `FLazyObjectProperty`, `FSoftObjectProperty`). `FOptionalProperty` (UE 5.5+) is guarded by `__has_include("UObject/PropertyOptional.h")` plus an engine-version check and dispatches the inner property type via `GetValuePointerForReadIfSet` — the null-on-unset return makes a preceding `IsSet()` call redundant.

The terminal `{ "_kind": "unsupported", "cpp_type": "<T>" }` sentinel emitted from the fallback path now indicates a true dispatch gap worth filing as a bug, not generic "unknown value" semantics. Consumer reader code should not treat the sentinel as nullptr / empty-string equivalent — surface it as a dispatch miss.

The asset-dump walk site `BuildClassPropertyJson` skips `CPF_Transient`, `CPF_DuplicateTransient`, `CPF_Deprecated`, and `CPF_SkipSerialization` UProperties upfront, so runtime-only and retired fields never produce `properties.json` entries even when their CDO value differs from the parent. Nested struct export applies the same suppression. The dumper also excludes a narrow owner-path/field-name list of known derived caches, including static-mesh cached counts, material texture-streaming data, MovieScene signatures, and Niagara compiled-data fields. The existing `flags` array describes emitted properties only; consumers must not use it to infer which fields the serializer suppressed.

## Properties override-only filter and ParentCDO contract

`BuildClassPropertyJson(Object, ParentCDO)` in `PropertyUtils.cpp` filters out properties whose value matches the parent CDO — only `is_overridden_locally` entries land in `properties.json`. This typically produces an 80–90% size reduction on real project assets.

Contract for callers:

- `ParentCDO != nullptr` — only overridden properties emitted. This is what `asset.dump` / `asset.dump_folder` always use.
- `ParentCDO == nullptr` — every property emitted (legacy full-dump). Existing tests that pass `nullptr` (e.g. `JsonShape`, `CoversAllProperties`) rely on this and remain unaffected.

The "all-keys" coverage test (`CoversAllProperties`) only guarantees full enumeration on the `ParentCDO=nullptr` path; do not assert it on the standard dump path.

## asset.dump diff mode artifacts and git hygiene

`asset.dump(diff=true)` never writes into the dump mirror. All diff output lands under `<ProjectSavedDir>/PinWright/asset-dump-diffs/<PackagePath>/`, one directory per asset:

- `<aspect>_new.<ext>` — full new content
- `<aspect>_diff.txt` — unified diff vs baseline
- `meta.json` — fresh metadata for the diffed state

The baseline mirror is left untouched in diff mode. Each diff run wipes and replaces the asset's previous diff directory, and a subsequent normal dump of the asset deletes its stale diff directory. In diff mode the result JSON's `dumpDir` points at the diff directory, not the mirror.

Diff mode is single-asset only. `asset.dump_folder` hard-codes `bDiff=false` (`AssetDumpHandler.cpp`), so diff output can only appear from manual per-asset `asset.dump(...,diff=true)` calls.

Git hygiene is self-contained: every dump seeds root-level scaffolding into the dump root (create-if-missing, never overwritten, safe to customize) — a `.gitignore` covering machine-local residue (`.dumpcache.json` markers, `*.tmp` atomic-write leftovers), a `.gitattributes` disabling text normalization (`* -text`), and `CLAUDE.md` + `AGENTS.md` (identical content) telling agents how to read the tree. A full-project baseline repo can grow large for big projects. Trees written by plugin builds that predate Saved-based diff output may still contain in-tree diff artifacts; extend the seeded `.gitignore` with `*_new.json` / `*_new.txt` / `*_new.xml` / `*_diff.txt` there.

## Dump root modes: Saved scratch vs committed mirror

The dump tree runs in one of two deployment modes:

1. **Saved scratch (default)** — root at `<ProjectSavedDir>/PinWright/asset-dumps/`. Machine-local and disposable, never in VCS. Right for personal analysis in any project.
2. **Committed mirror (diffable)** — root pointed inside the project repo (e.g. repo-root `asset-dumps/`). Versioned, diffable, and shared with teammates and their agents even when they don't run the plugin. Refresh it in dedicated commits, never mixed into code changes.

To switch modes, set `AssetDumpRootDirectory` in Project Settings → Plugins → PinWright (Project). The setting is `defaultconfig`, so edits persist to the project's committed `Config/DefaultEditor.ini` and the whole team inherits the mode; relative paths resolve against the project directory. A per-call `outRoot` overrides the setting for that dump only.

Operational effects of switching roots: freshness markers (`.dumpcache.json`) live beside each asset's dump dir under the active root, so pointing at a new root means a full first sweep there. The old tree is simply abandoned — delete it manually if unwanted. Diff-mode artifacts go to `Saved/PinWright/asset-dump-diffs/` in both modes and never pollute either tree.

## See also

- [`asset`](asset.md) for the complete `asset.dump` / `asset.dump_folder` contract and progress fields.
- [`asset-audit`](asset-audit.md) for the repeatable audit and cross-editor determinism workflow.
- [`asset.dump-sidecars`](asset.dump-sidecars.md) for typed sidecar schemas and aspect-version history.
