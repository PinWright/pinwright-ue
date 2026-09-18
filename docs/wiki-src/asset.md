# asset

Operate on `/Game/...` content packages - discover, load, dump, import, rename/move, delete, and read dependencies. Use `asset.*` as the catch-all for content-browser-level operations; type-specific authoring lives under the relevant domain namespace (`material.*`, `niagara.*`, `chooser.*`, etc.) and should be preferred for anything beyond create/list/delete.

## Cross-cluster overlap

- **Material creation and graph editing**: use `call("material.authoring")` (create material / instance, typed scalar/vector/switch/texture parameters) and `call("material.graph")` (add / connect / remove nodes, break connections, node details). These are the only material-authoring surfaces. `asset.*` no longer carries material creation or graph-edit verbs; only `asset.list_material_instances`, `asset.reset_instance_parameters`, and `asset.get_material_stats` remain under `asset.*` for material-instance and stat reads.
- **Niagara inspection** - `asset.dump` writes full Niagara aspect files for diffable review. Persistent Niagara edits are not grouped through `asset.*`; use one individual `niagara.*` edit RPC after reading current dump or inspect output.
- **Chooser authoring** - `chooser.*` is the domain surface for `UChooserTable` creation and first-slice row/column edits. Use it instead of generic asset mutation when creating chooser tables, adding rows, adding bool/float/enum/object/randomize columns, setting typed cells, assigning asset/class/evaluate_chooser results, or compiling the chooser.
- **Read-only audit** - `asset.find_objects_by_tag` searches the live world for tagged actors and components; for a pure level-actor query use `call("actor.find_by_tag")` or `call("system.inspect.find_by_tag")` instead. `asset.find_by_tag` is the asset-registry counterpart and answers a different question (tagged content packages, not tagged actors).
- **List / search verbs** - three verbs overlap on "find assets", split by which axis is primary, so pick by what you have in hand:
  - **Class-only list, no name pattern** ("every `SoundWave`") -> `asset.list { filter: { class: "SoundWave" } }` (see [asset.list](asset.list.md) for the `filter.class` short-name/full-path table) or `asset.search_assets { classNames:["SoundWave"] }`. Both are all-optional and take **no** `query`.
  - **Name pattern in hand** ("everything matching `BP_Enemy*`", "find me a grey material") -> `asset.search { query: "BP_Enemy*" }` (`pattern` is accepted as an alias for `query`). Here `query` is **required** - calling `asset.search` for a class-only list errors `[MISSING_REQUIRED_PARAM] 'query'`, so reach for `asset.list` / `asset.search_assets` when you have no name. **Do not browse a folder with `asset.list` and read the result to find a name** - that is the shape that spills megabytes; see [asset.search](asset.search.md).
  - **`asset.find` is not a verb.** The namespace ships only the tag-only `asset.find_by_tag` (asset registry) and `asset.find_objects_by_tag` (live world); a bare `asset.find` errors `[UNKNOWN_ACTION]` with a `Did you mean:` suggestion list. Use the class-list verbs above instead.
- **Inspection depth**: `asset.get` is shallow registry data (loads one asset); `asset.dump` writes one asset under `<ProjectSavedDir>/PinWright/asset-dumps/`; `asset.dump_folder` writes per-asset folders. For deep or repeatable inspection, use the dump mirror and its public `meta.json`, `properties.json`, BPIR, widget XML, SCS, actor, and world sidecars.
- **Dump-parity reads**: sidecars are also the contract for type-specific live reads that delegate to the dump builder without writing a cache. Current examples: `audio.authoring.describe_metasound` / `describe_sound_cue` / `describe_sound_wave`; `animation.describe_sequence` plus `animation.authoring.list_curves` / `list_notifies` / `list_sync_markers`; `texture.describe`; `skeleton.describe_mesh`; `data_table.list_rows`; and `static_mesh.describe`. The convention is namespace + `describe`, matching the sidecar filename; see [`asset-audit`](asset-audit.md#dump-sidecar--live-read-parity) for the parity matrix and the `material_instance.json` false-gap callout. Keep `audio.authoring.get_audio_info`, `animation.authoring.get_animation_info`, and `texture.get_texture_info` lightweight and backward-compatible.

For task-level dump/reference review, see [`asset-audit`](asset-audit.md). For Niagara, read with `asset.dump` or `niagara.inspect`, apply one `niagara.*` edit RPC, then run `niagara.validate`.

## Chooser authoring surface

`chooser.*` owns the first authoring slice: `chooser.create`, `add_column`, `add_row`, `set_cell`, `set_result`, and `compile` create `UChooserTable` assets. Column kinds: `bool`, `float`, `enum`, `object`, `randomize`; result kinds: `asset`, `class`, `evaluate_chooser`.

Status: IN-REVIEW. Invalid `enumType` / `allowedClass` must fail rather than create weak columns; `chooser.set_cell` must validate before resizing row storage; Chooser must not include the EQS handler header solely to reuse token normalization.

## See also

- Dump/review: [`asset.dump-quickstart`](asset.dump-quickstart.md), [`asset.dump-sidecars`](asset.dump-sidecars.md), and [`asset-audit`](asset-audit.md).
- Domain dumps: [`niagara`](niagara.md), [`niagara.dump-files`](niagara.dump-files.md), [`blueprint`](blueprint.md), [`material.mgir`](material.mgir.md), [`anim`](anim.md), [`widget`](widget.md), [`level`](level.md), [`audio.authoring`](audio.authoring.md), [`behavior_tree`](behavior_tree.md), and [`gas`](gas.md).
- Live typed reads: [`animation.authoring`](animation.authoring.md), [`texture`](texture.md), [`skeleton`](skeleton.md), and [`property`](property.md).
- `docs/arch.md` in the plugin folder covers dispatcher request ownership, async response tokens, and retained safe-point continuations. It is a maintainer document shipped beside the plugin, not a wiki page.

## Batch mutator outcome contract

`asset.source_control_checkout`, `asset.source_control_submit`, `asset.bulk_rename`, folder-mode `asset.duplicate`, and `asset.generate_lods` share one response contract. `partial` is optional and defaults to `false`. In that default mode the handler preflights every requested row and target; if any row is malformed, missing, unsafe, occupied, or otherwise refused, the envelope has `success:false` and **no valid survivor is mutated**. Rows that were ready to mutate carry `attempted:false` with `code:"BATCH_NOT_ATTEMPTED"`, while the original failing rows keep their specific `code` and `error`. An already-satisfied row may instead remain `ok:true`, `attempted:false`; bulk rename uses that state for a no-op name.

Pass `partial:true` only when changing the valid subset is intentional. Every row is still retained and reported, but valid survivors may run. The envelope may then have `success:true` when at least one row succeeds; always inspect `items[]`, `failed`, and the legacy verb-specific count rather than treating that envelope as an all-items verdict.

Every result carries `partial`, `requested`, `attempted`, `succeeded`, `failed`, and `items[]`. Array verbs emit one item for every original array element, including non-strings; folder duplication emits one item for every discovered child asset. Order is the input order (folder children use stable package-path order). Every item has `ok`, `attempted`, `error`, and `code`, retains the raw `input` for array verbs, and adds the paths relevant to that operation. Counts come from result/readback rows, not a filtered work list. A runtime failure can happen after another row has already changed, so a failed default envelope is **not** a rollback claim; use `items[]` as the mutation ledger.

### asset.set_metadata

Writes package metadata and saves the asset by default. Pass `save:false` only when a dirty in-memory edit is intentional. The response returns the canonical `assetPath`, `package`, `updatedKeys`, a `metadata` object read back from the asset for the keys this call wrote, and the standard `saveRequested` / `saved` / `saveState` / `saveDetail` persistence report. A request with no `metadata` object is a no-op and reports `saveRequested:false` with `saveState:"notRequested"`. Because the default path writes a package synchronously, dispatch routes this verb to a between-frame safe point even when `save:false` is supplied.

### asset.reset_instance_parameters

Clears the material instance constant's non-static parameter overrides and saves it by default; static parameter overrides, including static-switch overrides, are left untouched. Pass `save:false` to leave the reset only in the dirty resident package. The response returns the canonical `assetPath`, `package`, the standard persistence report, and `remainingOverrideCounts` with per-kind counts plus `total`; these values include any untouched static parameter overrides and are read from the instance after the clear. Because the default path writes a package synchronously, dispatch routes this verb to a between-frame safe point even when `save:false` is supplied.

### asset.exists

Checks an asset path without loading it. The lookup uses the asset registry, already-loaded objects and the mounted package file rather than `UEditorAssetLibrary`, so an active PIE session cannot turn a real asset into `exists:false`. Both package form (`/Game/Foo/Bar`) and object form (`/Game/Foo/Bar.Bar`) are accepted.

### asset.import

Imports `sourcePath` into the `/Game/...` `destinationPath`; `overwrite` is optional and defaults to `false`. With an empty destination, the production path calls the real `IAssetTools::ImportAssetsAutomated`. The response reports the primary output followed by every additional non-null output in the runner's order in `results[]`; `resultCount` is the exact length of that array. Do not infer that only the primary object was imported.

`overwrite:true` is a narrowly constrained in-place option, not permission to replace arbitrary content. An occupied destination is admitted only when it resolves to one exact, registry-identified `UTexture2D` object and the source is one bounded, fully decoded PNG. Production constructs a strongly held PinWright adapter modeled on UE 5.8's texture reimport factory, applies the requested source path, and passes that exact adapter to `FReimportManager`; success is rejected unless that adapter itself ran. A loaded but registry-unregistered object at the exact requested path still counts as occupied and can never fall through to general AssetTools import. With `overwrite:false`, an occupied destination is refused with `[ASSET_ALREADY_EXISTS]`. With `overwrite:true`, every identity-changing, class-changing, multi-conflict, collateral, or unsupported occupied case is refused with `OVERWRITE_UNSAFE` before either production runner is entered. There is no staging namespace, no `ForceReplaceReferences`, and no automatic reference fixup.

The successful occupied-texture response uses `mode:"inPlaceReimport"`, `identityPreserved:true`, `updatedInPlace:true`, `contentChangeMeasured:true`, `textureContentChanged:true`, and `pendingSave` to tell the caller that the in-memory object changed and still needs a later save. Same-object success is verified by retaining the original pointer and checking its class, object path, package, `StaticFindObject` result, Asset Registry entry, changed texture-source content identity, and dirty package state. Destination discovery and disk-resource fingerprinting are bounded; each resource's size and timestamp are captured before work, and its bytes are compared with a streaming hash. Because `asset.import` never saves, the package's disk resources must remain unchanged; `pendingSave` is therefore not evidence of a durable disk write.

The exact pre-call source metadata is restored on an admitted failure. If failure is proven before the adapter can mutate content, the prior package dirty state is also restored and the response reports `adapterInvoked:false`, `failureOccurredBeforeContentMutation:true`, and `failureMayHaveMutatedAsset:false`. Once the adapter may have run, a failure or no measured content change restores metadata but leaves or marks the package dirty; `failureMayHaveMutatedAsset:true`, `packageLeftDirtyForPossibleMutation:true`, and `inMemoryContentRestored:false` explicitly avoid claiming atomic content rollback. No identity-changing replacement is attempted by this verb. For that workflow, import a new asset at a caller-chosen path, repoint references explicitly, then use unforced delete on the old asset.

### asset.duplicate

Duplicates one asset from `sourcePath` to `destinationPath`. The source, destination and destination-directory checks are PIE-safe, but the editor duplication API is not: while play mode is active the verb refuses up front with `[PIE_ACTIVE]` and tells the caller to stop PIE and retry. A PIE refusal never reports the source as missing.

When `sourcePath` is a folder, `partial` follows the [shared batch contract](#batch-mutator-outcome-contract). Each `items[]` row carries the discovered child `sourcePath`, its derived `destinationPath`, and, after an attempt, `actualPath` / `existsAfter`. `duplicatedCount` is the number of child destinations observed after the operation. In default mode every child target is checked before the first write, and a preflight failure returns without creating even the destination folder. Runtime directory or duplicate failures are per child and do not roll back children already copied. `partial` applies only to folder mode; the single-asset branch has its own source/destination postcondition below.

For a single asset, the engine's success is reconciled with a registry/load and package-file read-back. Success requires a loadable object at the requested destination package and the source still present. The response reports the canonical `assetPath`, measured `existsAfter` / `existsOnDisk`, and the explicit `sourceObservedPath`, `sourceExistsAfter`, `sourceExistsOnDisk`, `sourceIsRedirector`, `destinationObservedPath`, `destinationExistsAfter`, and `destinationExistsOnDisk` fields. A native failure or a failed postcondition returns `[DUPLICATE_FAILED]` with those state fields instead of publishing the requested destination as proof.

### asset.rename

Renames one asset and leaves the engine's redirector at the old path for existing references. The destination is read back through the asset registry and loader and must be at the requested package; the original source must no longer contain the renamed asset. A source redirector is expected and is reported as `sourceIsRedirector:true`, so `sourceExistsAfter:false` can coexist with `sourceExistsOnDisk:true` while the redirector package remains. Success returns the canonical object `assetPath` plus the same registry/disk read-back fields documented for single `asset.duplicate`. A native failure or failed destination/source postcondition returns `[RENAME_FAILED]` with the observed paths and both registry/disk facts.

### asset.source_control_checkout

Checks out the required `assetPaths` array. `partial` follows the [shared batch contract](#batch-mutator-outcome-contract); source control being disabled, a malformed element, a missing asset, or a package with no resolvable filename is a preflight failure. Rows carry `inputPath`, resolved `path`, `packagePath`, `filename`, and attempted rows add `stateReadback` / `checkedOut`. The compatibility `assets[]` list contains only paths whose provider state readback accepted the checkout, and `checkedOut` is its measured count.

### asset.source_control_submit

Submits the required `assetPaths` array with optional `description` (default: `Automated submission via PinWright`). `partial` follows the [shared batch contract](#batch-mutator-outcome-contract). Rows carry the same resolved path/package/filename fields as checkout; attempted rows also report `stateReadback`, `pendingBefore`, `pendingAfter`, and `submitted`. `submitted` counts rows whose post-submit provider state no longer shows the pending change. A provider/runtime failure remains attached to that row; successful submissions are not rolled back.

### asset.map_references

Returns the same soft-UWorld UPROPERTY reference data that `asset.dump` writes to `map_references.json`, without creating or reading a dump cache entry.

Params: `assetPath` (required).

Response shape:

```json
{
  "schemaVersion": 1,
  "references": [
    {
      "property": "MapRef",
      "path": "/Engine/Maps/Templates/Template_Default.Template_Default",
      "source": "soft-uworld"
    }
  ]
}
```

When the asset cannot be loaded or has no qualifying soft-UWorld references, the call succeeds with `schemaVersion: 1` and `references: []`.

### asset.get_dependencies

Outbound hard package dependencies: what this asset references. Params: `assetPath` (required), `recursive` (optional, default `false` - transitive closure when `true`). For the inverse ("who references this?") use [`asset.dependencies`](#assetdependencies); for role/mode-classified output use [`asset.get_dependencies_classified`](#assetget_dependencies_classified).

**An unresolvable path is an error, not an empty list.** A path naming no package returns `[ASSET_NOT_FOUND]`. It previously returned `{"dependencies": []}` with `isError: false`, which is indistinguishable from a genuine "nothing depends on this" - and this verb sits on the delete path, where a caller checking before removing an asset reads an empty list as permission to proceed. The response echoes `assetPath` (what you sent) and `packageName` (what it resolved to), so an empty list is readable as a measurement rather than a possible typo.

**Path forms** are shared with `asset.get_dependencies_classified`, `asset.references` and `asset.dependencies`, and match what `asset.exists` accepts: `/Game/Foo/Bar`, `/Game/Foo/Bar.Bar`, a `:SubObject` suffix, and the `Class'/Game/Foo/Bar.Bar'` export-text form all resolve. Resolution is by **package**, not by appending `.<leaf>`, so a package whose asset name differs from its leaf name still resolves; and because the dependency graph is per package, several assets sharing one package all resolve to the same node and the same answer. Rejected: an empty path, a path under no mounted root, and a syntactically valid path whose package has no registry row, is not loaded, and has no file on disk.

### asset.get_dependencies_classified

`asset.get_dependencies` plus per-dependency classification. Params: `assetPath` (required), `recursive` (optional), `mode` (`all` | `runtime_only` | `ui_tree_only` | `editor_only` | `code_call_refs`), `role` (`all` | `package` | `manage` | `searchable_name`). Emits `classifiedDependencies[]` with `category` and the three `is*Dependency` role flags, plus `categoryCounts` / `roleCounts`.

Same path contract and same `[ASSET_NOT_FOUND]` verdict as [`asset.get_dependencies`](#assetget_dependencies). `mode` and `role` are validated **before** the path is resolved, so a bad `mode` still reports `[INVALID_MODE]` rather than being masked by a path failure.

### asset.references

Outbound dependencies of one asset, as registry `FAssetIdentifier` rows (`packageName`, optional `objectName`). Params: `assetPath` (required). Emits the legacy `references` / `referenceCount` keys and the direction-true `dependencies` / `dependencyCount` aliases side by side; the legacy names are direction-misleading and kept only for wire compatibility. For the inverse use [`asset.dependencies`](#assetdependencies); for a flat package-name list use [`asset.get_dependencies`](#assetget_dependencies).

**The short `/Game/Foo/Bar` form is accepted.** It previously worked for some assets and failed for others with `[ASSET_NOT_FOUND]` on the exact string `asset.exists` accepted, because the lookup needed a full object path and only succeeded when the package happened to already be loaded in memory - which is not something a caller controls. Both spellings now resolve through the shared package resolver described under [`asset.get_dependencies`](#assetget_dependencies) and return identical results.

### asset.dependencies

Inbound referencers: who references this asset. Params: `assetPath` (required). Emits the legacy `dependencies` / `dependencyCount` keys and the direction-true `referencers` / `referencerCount` aliases. This is the verb to run before a delete. Same path contract, same short-form fix, and same `[ASSET_NOT_FOUND]` verdict as [`asset.references`](#assetreferences).

### asset.save

The narrow single-package writer: persists ONE loaded asset to disk. Reach for this instead of the blunt `editor.save_all` (which flushes every dirty package) after a graph/property mutation - `blueprint.set_default`, `property.set`, `blueprint.graph.set_pin_default_values`, a `material.authoring.*` edit - leaves the asset dirty in memory with no per-asset flush. See [`safe-mutation-save`](safe-mutation-save.md) for the full read -> mutate -> verify -> save recipe.

**On a `saved:false`, branch on `saveState` and never on `pendingFlush` alone.** Only `saveState: "deferred"` is fixed by a flush now (`editor.save_all`, or `asset.save {force: true}` to bypass the 0.5 s throttle). `"failed"`, `"notPersistable"` and `"diskStateDiverged"` are never fixed by a flush. `"blockedByPie"` is a typed `PIE_ACTIVE` error with `pendingFlush:false`: a play session is running and the editor refuses **every** single-asset save while one is, `force: true` included. `saveDetail` states the remedy in one sentence, and the full table is in [`safe-mutation-save`](safe-mutation-save.md).

**PIE refusal.** When PIE is the blocker, `asset.save` fails with `PIE_ACTIVE`; its error data carries `saved:false`, `pendingFlush:false`, `saveState:"blockedByPie"`, `pieActive:true`, `editorMode:"PIE"` and `pieWorlds[]` (`pieInstance`, `mapName`, `worldPath`) naming the session holding the editor - which in a shared editor is usually another agent's. Poll `editor.pie_status` for `inPie:false`, then re-issue and require `saveState: "written"`.

```
call("asset.save", { assetPath: "/Game/VFX/E_Explosion", force: true })
# -> ERROR PIE_ACTIVE
# -> {"assetPath":"/Game/VFX/E_Explosion","package":"/Game/VFX/E_Explosion",
#     "saveRequested":true,"saved":false,"pendingFlush":false,
#     "pieActive":true,"editorMode":"PIE",
#     "pieWorlds":[{"pieInstance":0,"mapName":"T_UI",
#                   "worldPath":"/Game/Test/UEDPIE_0_T_UI.T_UI"}],
#     "saveState":"blockedByPie","saveDetail":"Nothing was written: the editor is in play mode...",
#     "sizeBytes":108204,"sizeBytesIsStale":true}
```

**`sizeBytes` is the file's size after the call, not the bytes this call wrote.** On a `saved:false` over an asset that already existed it is the *previous* revision's size - plausible, non-zero, and on a repeated attempt byte-identical to a successful save's - so the response marks it `sizeBytesIsStale: true`. Over an asset that never existed it is `0`. Neither value is evidence of a write; `saveState` is.

**Refuses `SAVE_DISK_STATE_DIVERGED` when the file changed under the editor.** A loaded package's `.uasset` can be replaced behind the editor's back - a `git checkout`, a `git reset --hard`, a manual revert, another tool, a second editor, a teammate's sync. The resident package is then a fork of an older revision, and saving it discards what is on disk. This verb refuses that write instead of making it. The error payload carries the measurement, not a guess:

```
call("asset.save", { assetPath: "/Game/Audio/SC_Master" })
# -> ERROR SAVE_DISK_STATE_DIVERGED
#    {"assetPath":"/Game/Audio/SC_Master","package":"/Game/Audio/SC_Master","saved":false,
#     "saveState":"diskStateDiverged",
#     "diskState":{"probed":true,"diverged":true,"fileExists":true,
#                  "file":"<...>/Content/Audio/SC_Master.uasset",
#                  "diskSavedHash":"<hash read out of the file>",
#                  "loadedSavedHash":"<hash the resident package last read or wrote>",
#                  "diskSizeBytes":1234,"diskModified":"2026-08-30T09:12:44Z"}}
```

`diskSavedHash` is measured from the file by this call; `loadedSavedHash` is what the in-memory package believes it last read or wrote. `probed:false` means **no comparison was made** (not "they match") and carries a `reason` with no measured fields - a package that has never been on disk, an unmounted name, a text-format or unreadable header. Two remedies: `asset.reload` the package and redo the edit on top of the current file, or re-issue with `overwriteDiskChanges: true` to discard the on-disk revision deliberately. `force: true` does **not** do that - it only bypasses the 0.5 s save throttle, and a forced save is refused the same way.

The check costs one header read of the `.uasset` per save. The ledger is `UPackage::GetSavedHash()` against the hash embedded in the file's own package summary: the engine writes both on every load and every save to a mounted path, so an ordinary editor save - from this plugin or from anywhere else - never trips the gate. Autosaves write elsewhere and do not either.

### asset.nanite_rebuild_mesh

Runs synchronously: it updates the Nanite settings on one StaticMesh, rebuilds it, and advances the shared asset-compilation pump for at most `timeoutSeconds` (default and maximum 120 seconds). A timeout returns `OPERATION_FAILED` with `timedOut:true`, does not attempt the save, and retains the render guard through an observation-only ticker until the engine reports terminal state; that ticker never pumps compilation after the request ends. `save` is optional and defaults to `true`; the default force-saves the rebuilt asset and verifies that its `.uasset` exists before reporting `saved:true`.

This is tick-unsafe and runs through the shared safe-point/render guard. Matching live StaticMesh and Niagara mesh-renderer consumers are quiesced before Build/PostEditChange and restored afterward; `MESH_REBUILD_CONSUMER_NOT_QUIESCABLE` refuses the rebuild before mutation or save when a consumer cannot be quiesced.

The response carries the resolved package name in `package`, plus `sizeBytes` (and `sizeBytesIsStale` when applicable), `saveRequested`, `saved`, `saveState`, `saveDetail`, and `pendingFlush` only when a requested save is not durable. With `save:false`, no disk write is attempted and the package remains dirty; an existing `.uasset` is probed, but its size is the prior stale revision (`sizeBytesIsStale:true`), not evidence of a write. If no prior file exists, `sizeBytes` is `0` and the stale marker is absent. `saveState` is `notRequested`, and `pendingFlush` is absent.

### asset.mark_dirty

Sets ONE loaded package's dirty flag and writes nothing. The step before a save, for the case where a mutation changed memory but left the package clean - **a save no-ops on a clean package**, so that edit is lost when the editor closes. Accepts a package path (`/Game/Maps/MyMap`) or an object path (`/Game/Maps/MyMap.MyMap`); it never loads, so an unloaded path returns `PACKAGE_NOT_FOUND` rather than pulling assets in as a side effect.

Reports `package` - the **resolved** package name, not your input. Under World Partition / One File Per Actor an actor lives in its own external package, so the package a save will write is not the map's. `wasDirty` distinguishes "this call set the flag" from "it was already set", which is what an idempotent re-run needs. `isDirty` is read back off the package after the write, so a success is never reported for a dirty that did not take; a refusal is `MARK_DIRTY_REFUSED` with a printable reason (transient / PIE duplicate / cooked / native script package, or the editor is loading, undoing, transacting, cooking, or async-loading).

```
call("asset.mark_dirty", { assetPath: "/Game/Maps/MyMap" })
# -> {"assetPath":"/Game/Maps/MyMap","package":"/Game/Maps/MyMap","wasDirty":false,"isDirty":true,"saved":false}
call("level.save", {})
# -> after the save, call asset.is_dirty and require isDirty:false
```

For a content asset, use `asset.save` with `force:true`; for a level package, use `level.save`. In both cases read back with `asset.is_dirty` and require `isDirty:false` before claiming persistence. This typed RPC is also the supported replacement for the engine APIs that UE 5.8 does **not** expose to Python - `asset.mark_package_dirty()`, `Actor.mark_package_dirty`, and `Package.set_dirty_flag` are all absent there. See [`python`](python.md) for the reflected `unreal.PinWrightPackageLibrary` wrapper and its force-save/read-back recipe.

### asset.is_dirty

Read-only companion: reports whether one loaded package currently needs saving, with no load, no dirty, and no save. Use it as the read-back after `asset.mark_dirty`, and as the post-condition for a "this sequence changed nothing" claim - `asset.get` carries no dirty field and `editor.save_all` can only answer the question destructively.

```
call("asset.is_dirty", { assetPath: "/Game/Maps/MyMap" })
# -> {"assetPath":"/Game/Maps/MyMap","package":"/Game/Maps/MyMap","isDirty":true}
```

### asset.dump

Writes one content package to a persistent text mirror under `<ProjectSavedDir>/PinWright/asset-dumps/` (or `outRoot`; the default root is configurable in Project Settings -> Plugins -> PinWright (Project)). The mirror path follows the package path, so `/Game/MyGame/UI/WBP_HUD` becomes `<root>/Game/MyGame/UI/WBP_HUD/`. Use this when you need a repeatable, greppable baseline instead of a live one-off RPC response.

Params: `assetPath` (required), `outRoot` (optional), `diff` (optional, default `false`), `includeWidgetScreenshot` (optional, default `false`).

Direct single-asset dumps always inspect and write the requested package. They do not consult `.dumpcache.json` and do not return cache hits; successful baseline dumps may refresh the metadata so later folder sweeps can skip unchanged assets.

Non-level assets return synchronously with `dumpDir`, `writtenPaths`, `skipped`, and `mode`. `UWorld` / `.umap` dumps run as jobs because level actor dumps can write many files. Streaming MCP calls block and stream progress by default; pass `wait:false` to receive a `{ticket_id}` immediately, then poll `system.job_status`. Completion reports `assetPath`, `dumpDir`, `mode`, `writtenCount`, and `skipped`.

The level (async) path runs in the background and can take several minutes for large levels - the kickoff response means *started*, not *done*. Track progress via the Monitor tool on `monitor_path` (Claude Code) or by polling `system.job_status` with the `ticket_id` (other agents); do not treat the dump as complete until status is `completed`.

When waiting on the JSONL instead, match `"event":"completed"` (or `"failed"` / `"cancelled"`) - `jobs.jsonl` carries no `status` field.

Files by asset type:

- Widget Blueprints: `meta.json`, `properties.json` from the generated-class CDO, sparse `tree.xml` for widget hierarchy and non-default widget values, `widget_animations.json` when persisted animations exist, `bpir.txt`, and opt-in `preview.png` when `includeWidgetScreenshot=true`.
- Blueprints: `meta.json`, `properties.json` from the generated-class CDO, `bpir.txt`, `scs.json`, and compact `scs.txt` when the Simple Construction Script is non-empty.
- Materials and Material Functions: `meta.json`, `properties.json`, and `mgir.txt` from the same MGIR decompiler used by `material.decompile_mgir`.
- Material instances: `meta.json`, `properties.json`, and `material_instance.json` - `UMaterialInstanceConstant` assets get a structured sidecar with `parent`, `parentChain`, per-parameter `overrides` (scalar, vector, texture, staticSwitch, staticComponentMask), `basePropertyOverrides`, `materialLayers`, lightmass / nanite / phys-material / subsurface overrides, and resource flags.
- Levels / UWorlds: `meta.json`, `properties.json`, `world_settings.json`, `level_bp.txt`, `sublevels.json`, plus `actors/manifest.json` and per-embedded-actor JSON files.
- Niagara systems and emitters: `meta.json`, `properties.json`, `nir.txt`, `niagara_parameters.json`, `niagara_stack.json`, `niagara_graphs.json`, and `niagara_compile.json`. Standalone NiagaraScript / NiagaraModule assets write `meta.json`, `properties.json`, `nir.txt`, `niagara_graphs.json`, and `niagara_compile.json`.
- Cascade particle systems: `meta.json`, `properties.json`, and read-only `cascade.json`.
- DataTables: `meta.json`, `properties.json`, and `data_table.json` - row struct path/name, row count, and all rows keyed by row name (sorted alphabetically), each row serialized from the live `RowMap` via `FJsonObjectConverter`.
- Textures: `meta.json`, `properties.json`, and generic `texture.json` for any `UTexture` asset, including Texture2D, TextureCube, Texture2DArray, TextureCubeArray, VolumeTexture, and render-target subclasses. `texture.json` is the single canonical native-summary sidecar for every `UTexture` subclass - there is no per-subclass specialization file. The Texture2D-shaped fields the legacy `texture_2d.json` exposed (dimensions, format, compression/sRGB/streaming flags, source size and format) are all subsumed by `texture.json`, plus class identity (`kind`, `textureClass`), `arraySize`, `size.z`, and `source.slices` for cube/array/volume cases.
- DataAssets and generic UObjects: `meta.json` and `properties.json` from the asset instance compared against the asset class CDO.

Schema summary:

- `meta.json`: sorted class and package metadata: `assetPath`, `className`, `parentClass`, `kind`, `blueprintType`, `sidecarsEmitted`, and `propertiesStatus`. `kind` is a coarse runtime classification (`Widget`/`Actor`/`Component`/`AnimInstance`/`Interface`/`FunctionLibrary`/`MacroLibrary`/`Object`); `blueprintType` is always present, using the raw `UBlueprint::BlueprintType` enum string for Blueprint assets and JSON `null` for non-Blueprint assets. `sidecarsEmitted` is the alphabetically-sorted list of every file the dispatcher wrote alongside `meta.json` for this asset (absent on skip stubs). `propertiesStatus` is uniformly present: Blueprint dumps with non-empty property maps use `{ "status": "ok", "propertyCount": N }`; empty Blueprint property maps use `{ "status": "empty", "reason": "no_overrides" }` or `{ "status": "empty", "reason": "no_uproperties_on_blueprint_type", "blueprintType": "FunctionLibrary|MacroLibrary|Interface" }`; missing generated classes use `{ "status": "error", "reason": "generated_class_missing" }` and omit `properties.json`; non-Blueprint and `UObjectRedirector` dumps use `{ "status": "n/a", "reason": "non_blueprint_asset" }`. Live Niagara compile availability is deliberately absent from persisted `meta.json`; use `niagara.inspect` or `niagara.validate` for runtime compile diagnostics.
- `properties.json`: sorted UPROPERTY entries that differ from the parent CDO, each with `type`, serialized `value`, whitelisted `CPF_*` `flags`, `inherited_from`, and `is_overridden_locally`. Properties whose value matches the parent CDO are omitted entirely (see the schema history on [`asset.dump-sidecars`](asset.dump-sidecars.md#recent-schema-additions-and-removals)).
- `scs.txt`: compact text companion for Blueprint SCS component-template dumps. It is emitted alongside `scs.json`, not instead of it; `blueprint.scs.get` remains the JSON live surface.
- `mgir.txt`: Material Graph IR for `UMaterial` and `UMaterialFunction` assets, using `entry material` or `entry function` as appropriate. If MGIR decompile fails, the dump keeps the generic files and records the aspect diagnostic instead of failing the whole asset dump.
- `tree.xml`: Widget Blueprint hierarchy with sparse non-default widget values. Reflected `bOverride_*` booleans are emitted explicitly even when `false`, so consumers do not have to infer override-disabled from absence. `FSlateBrush.ResourceObject` values use full UE export-text object syntax such as `/Script/Engine.Texture2D'/Game/UI/T_Icon.T_Icon'`. Child Widget Blueprints that inherit their root from a parent WBP still emit the inherited full tree, marked with a leading `<!-- inherited from ... -->` comment and `inherited_from="..."` on the root element. It does not resolve Slate geometry; use [`widget.export_xml`](widget.export_xml.md) with `resolve_geometry=true` for layout measurements.
- `widget_animations.json`: Widget Blueprint animation JSON using schema `pinwright.widget-animations.v1`, matching `widget.export_animations_json` with `includeEventMetadata=true`. It serializes persisted `UWidgetAnimation` MovieScene visual tracks, playback ranges, widget-name bindings, and inspection-only `eventTracks` / `delegateBindings` / `triggeredFunctions` metadata; event metadata is not importable visual track data.
- `bpir.txt`: all decompiled graphs in one file: ubergraph first, then functions and macros alphabetically, with decompiler warnings appended when present. **Elision policy:** `AssetDumpBuilder::ShouldEmitBpirText(UBlueprint*)` gates emission. The file is omitted entirely when the BP has zero non-empty graphs AND `ParentClass` does not derive from any of seven graph-bearing roots (`AActor`, `UActorComponent`, `UUserWidget`, `UBlueprintFunctionLibrary`, `UAnimInstance`, `UGameInstance`, `UGameModeBase`). Null `ParentClass` is fail-open (emits). Style classes (`UCommonTextStyle`, `UCommonButtonStyle`, `USaveGame`, plain `UObject` data BPs) thus elide entirely, while `UUserWidget` / `AActor` / etc. BPs with empty graphs still emit the file with the empty-marker so decompile-failure cases stay distinguishable. Level BPs use a different filename (`level_bp.txt`) and are not gated.
- `scs.json`: component-template tree in the same shape as [`blueprint.scs.get`](blueprint.scs.md), omitted when empty.
- `actors/`: `manifest.json` indexes embedded actors and external actor references. Embedded actor files use schema `pinwright.actor-describe.v1`, matching `call("actor.describe")` for the same loaded actor shape: identity, class, level, label/folder/tags, world transform, sparse modified actor `properties`, all loaded components, scene-component `relativeTransform`, attachment path/name/socket fields, and sparse modified component `properties`. External actor packages are listed as references, not loaded.
- `map_references.json`: soft-UWorld UPROPERTY references, using schema `{ "schemaVersion": 1, "references": [{ "property": string, "path": string, "source": "soft-uworld" }] }`. The live equivalent is `asset.map_references`.
- `nir.txt`: Niagara text IR for systems, emitters, and standalone scripts. It is generated by the same `NIRDecompiler::BuildNiagaraIrText(UObject*)` path as `niagara.decompile_nir`, and is the durable dump-sidecar representation for system/emitter flags, stacks, renderers, event handlers, simulation stages, and GPU script graphs. It does not statically flag GPU-incompatible modules (see [niagara.nir](niagara.nir.md) - GPU/CPU is per-script-usage, not per-emitter); genuine GPU-incompatibility is compile-time and surfaces through `niagara_compile.json`.
- Live Niagara JSON reads still expose compact model, system, and emitter builder shapes through Niagara RPCs, but `asset.dump` no longer writes those shapes as `niagara_model.json`, `niagara_system.json`, or `niagara_emitters.json`.
- `niagara_parameters.json`: accessible Niagara parameter stores with namespaces, types, defaults, serialized values, and bindings. Reflected script-struct values are copied out through `FNiagaraParameterStore::CopyParameterData` before property serialization so LWC/custom struct values are exported from caller-facing struct memory instead of Niagara's simulation/SWC store layout.
- `niagara_stack.json`: system and emitter stack/module entries in execution order with script usage, function-call ids, enabled state, script refs, selected versions, and input override summary.
- `niagara_graphs.json`: Niagara graphs by system/emitter/standalone-script usage with nodes, pins, pin defaults/types, links, GUIDs, and function-call script refs.
- `niagara_compile.json`: authored asset/script identities plus stable issues derived from authored structure. It deliberately omits live validity, readiness, outstanding-compilation, recompile, and compile-status fields; query `niagara.inspect` or `niagara.validate` for those runtime diagnostics. Standalone NiagaraScript assets use the same authored-only contract, scoped to the script asset.
- `cascade.json`: read-only Cascade `UParticleSystem` dump with emitters, LOD levels, required/spawn/type-data modules, module classes, and module property diffs. Cascade editing is not supported.
- `texture.json`: compact generic `UTexture` summary with class identity (`kind`, `textureClass`), dimensions (`size.x/y/z`), `arraySize`, pixel or render-target format when available, shared compression/LOD/sRGB/mip-generation/streaming flags, and editor source dimensions/format when source art exists.
- `material_instance.json`: structured per-instance overrides for `UMaterialInstanceConstant` assets - parent material identity, full parent chain, per-parameter overrides keyed by name (scalar, vector, texture, static-switch, static-component-mask), `basePropertyOverrides` with paired `bOverride_*` flags + values, material layers manifest, optional parent lightmass / nanite passthrough / phys-material / subsurface-profile overrides, and resource flags. Mirrors the live `material.authoring.get_material_instance_info` payload for the dump-shared subset.

Diff-mode artifacts, the `_new` / `_diff.txt` output shape under `Saved/PinWright/asset-dump-diffs/`, and dump-tree git hygiene live on [`asset.dump-quickstart`](asset.dump-quickstart.md#assetdump-diff-mode-artifacts-and-git-hygiene).

Overwrite semantics: normal `asset.dump` purges the asset dump directory before rewriting, then writes each file via a temp file and rename. This removes stale files when an asset changes type and avoids leaving partial output behind.

All generated text sidecars remove spaces and tabs at the end of each line at the final write
boundary. Diff mode compares this same cleaned form before deciding whether to create `_new` or
`_diff` files, so byte comparisons do not depend on which sidecar builder produced the text.

#### `ObjectRedirector` assets dump empty `properties.json`

If a `properties.json` looks empty, check `meta.json` for `className: ObjectRedirector` before filing a bug. `UObjectRedirector` has zero UPROPERTYs at the CDO level - the redirect target lives on a non-reflected `DestinationObject` member that the dumper does not synthesise into `properties.json`. The empty `{}` is faithful, not a missing-property regression. To find where a redirector points, fix it up first via `asset.fixup_redirectors`, or follow the chain manually with `asset.references`.

This means redirectors left behind by renames look like "missing data" entries in the dump mirror. They aren't - they're just dead weight in the content tree. A `asset.fixup_redirectors` pass clears them, and a subsequent `asset.dump_folder` of the swept subtree prunes their stale dump folders during reconciliation.

### asset.dump_folder

Starts an async sweep of every asset under a content folder and writes one dump folder per asset using the same schema as [`asset.dump`](asset.dump.md). Streaming MCP calls block and stream progress until the final result by default. Pass `wait:false` to receive a `{ticket_id}` immediately, then poll `system.job_status` or read `Saved/PinWright/jobs.jsonl`. Non-streaming clients receive the ticket normally.

This runs in the background and can take **~1-60 minutes depending on asset count** - the kickoff response means *started*, not *done*. Track progress via the Monitor tool on `monitor_path` (Claude Code) or by polling `system.job_status` with the `ticket_id` (other agents); do not treat the sweep as complete until status is `completed`.

**Wall time.** Measured on this project (UE 5.8, warm editor): `/Game` = 1890 assets, 1838 dumped, 52 cache hits → **38.7 s**; `/App` = 1018 assets, 249 dumped, 769 cache hits → **33.6 s**. Expect tens of seconds for a project-sized subtree; minutes only with `includeLevels=true`. Do not extrapolate a per-asset rate across folders — `/App` dumped 7× fewer assets in nearly the same wall time (Blueprint/widget assets carry compile deferrals), so count alone does not predict duration.

**Prefer letting the call block.** On a streaming MCP client, `wait` absent or `true` blocks and returns the finished result — no waiter, no polling, no ticket bookkeeping. That is the right default at these durations. Pass `wait:false` only for genuine fire-and-forget. Blocking stays alive on long sweeps because each accepted progress frame resets the request deadline (SSE heartbeats deliberately do not).

**Waiting on a `wait:false` ticket.** Check once before arming any waiter — a sweep this size is usually already finished:

```
call("system.job_status", {"ticket_id": "j_..."})   // reply has "status": running|completed|failed|cancelled
```

Only if it is still `running`, wait on the JSONL. **`jobs.jsonl` has no `status` field** — the terminal marker is `"event":"completed"` (or `"failed"` / `"cancelled"`). A loop matching `"status":"completed"` against the JSONL never fires and hangs forever. `ticket_id` and `event` are on the same line, so one grep per line is correct:

```sh
LOG="<Project>/Saved/PinWright/jobs.jsonl"; T="j_..."
until grep "$T" "$LOG" 2>/dev/null | grep -Eq '"event":"(completed|failed|cancelled)"'; do sleep 5; done
grep "$T" "$LOG" | grep -E '"event":"(completed|failed|cancelled)"' | tail -1
```

The terminal line carries the full `result` object and is flushed synchronously. It is never rate-limited — the `Min progress event interval` throttle (default 60 s) applies only to `progress` events, so a sub-minute sweep logs exactly three lines: `started`, one `progress`, and the terminal event. Do not wait on progress lines advancing; they may not.

Params: `folderPath` (required, must start with `/`), `recursive` (optional, default `true`), `outRoot` (optional), `includeLevels` (optional, default `false`), `includeWidgetScreenshot` (optional, default `false`), `force` (optional, default `false`). Levels are skipped by default because each load can pull in actors, sublevels, and data layers; set `includeLevels=true` when you intentionally want map dumps.

Run this as the first step of any deep-inspection workflow on a content subtree - dump the folder or plugin you're working in before you start inspecting. Subsequent reads are filesystem reads against the public sidecars in the dump mirror, which is cheaper than re-calling `blueprint.inspect` / `widget.export_xml` / `property.list` / `level.get_actors` for every asset. The analysis RPCs now emit a `hint` proposing an `asset.dump_folder` call when the inspected subtree's mirror is missing or stale, so you don't have to remember - but you choose the scope (the folder/plugin you're working in, or `/Game` for a full-project dump).

Folder sweeps are incremental by default. Each successful baseline dump writes a private `.dumpcache.json` beside the public sidecars, and the next `asset.dump_folder` skips that asset when the saved package fingerprint, dump options, dump-core version, expected sidecar versions, and written sidecar set still match. `.dumpcache.json` is internal validity metadata; consumers should keep reading public files such as `meta.json`, `properties.json`, `tree.xml`, BPIR, and type-specific IR sidecars for asset facts.

Set `force=true` to ignore `.dumpcache.json` and re-queue otherwise unchanged assets. Cache hits are also bypassed when `includeWidgetScreenshot=true` because `preview.png` can depend on external visual state. Map / `UWorld` packages are uncached in v1, even with `includeLevels=true`, because their output can depend on external actors, external objects, and World Partition state outside the owning package.

Folder sweeps allocate and attach the public job ticket before preflight, then publish `scanning_registry` and budget cache-freshness checks across ticker frames. The Asset Registry's individual `GetAssets` call is still synchronous, but it now runs after the job and notification exist. Dump work uses an 8 ms per-tick budget checked between assets. Before each synchronous asset operation it publishes `currentAsset`, `currentPhase`, phase timing, completed/remaining counts, elapsed time, and an estimated remaining time. Interactive MCP jobs show these fields in a fixed-height nonmodal editor notification with a Cancel button; each text row is single-line Slate-ellipsized and the current asset's full path remains available in its tooltip and job payload. The notification updates every asset, while `jobs.jsonl` progress uses the job registry's configured rate limit instead of writing one event per asset. Cancellation stops preflight or the dump queue, restores the pre-dump dirty-package baseline, and deliberately skips mirror reconciliation so pending assets are not pruned. It remains cooperative: a synchronous engine call already executing on the game thread cannot be hard-preempted, so cancellation takes effect after the current engine call returns. Compiling textures are requeued instead of blocking on compilation, a few dozen entries from the front of the queue so the retry happens within seconds rather than at the end of the sweep, and the release step leaves their packages loaded; they are skipped only after 120 seconds during which nothing anywhere finished compiling, preserving a prior dump when one exists. Per-asset load failures do not abort the sweep; they are appended to `skipped` and the sweep continues. After normal completion, reconciliation prunes stale dump directories under the swept subtree by looking for `meta.json` ownership markers. The prune is scoped to that subtree, and directories without `meta.json` are left alone.

A sweep releases what it loads. Assets loaded from disk carry `RF_Standalone`, which is inside the editor's `GARBAGE_COLLECTION_KEEPFLAGS`, so the editor's own idle collect never reclaims a swept package: without an explicit release a long sweep grows the working set until the process is killed. At an asset boundary, whichever of three triggers comes due first (Editor Preferences → Plugins → PinWright → Jobs; each can be set to 0 to disable that trigger) puts the sweep into `releasing_memory`: *Asset dump release interval (assets)*, default 200, counts assets processed since the last release; *Asset dump release memory watermark (fraction of physical RAM)*, default 0.5, fires once the process working set reaches that share of physical RAM; and *Asset dump release working-set growth (GiB)*, default 8, fires once the working set has grown that much since the working set measured after the last release collect. The growth trigger is the only one expressed in bytes-since-the-last-release, so it is what bounds a region whose assets are far heavier than the count trigger assumes on a machine roomy enough that the watermark is never reached — measured on one such sweep, ~200 heavy Nanite meshes took the working set from 11.9 to 52.6 GiB inside a single count interval. The last two additionally require 25 assets since the last release, so a limit the sweep cannot get back under does not turn into one drain-and-collect per asset. The release step drains async loading and asset compilation, drops the two `UClass`-keyed raw-`FProperty` caches, clears `RF_Standalone` and resets the loaders on the packages the sweep itself brought into memory, then asks the editor for a full purge through `ForceGarbageCollection` — the only collect primitive safe from the ticker. Ownership is tracked per package as the sweep loads it, and dirty packages, packages already dirty at sweep start, rooted packages, the editor world, anything with an asset editor open and any package still waiting on compilation are never released; a deferred package stays tracked, so a later release step frees it once it has been dumped. A `PostReachabilityAnalysis` callback restores `RF_Standalone` on survivors so a package that something still references is not left half-purged. The sweep loads no further assets while the collect is outstanding (`waiting_for_release_gc`, bounded at 60 s), and progress payloads report `releaseStepCount`, `releasedPackageCount`, and `workingSetBytes`. Each release step logs which trigger fired (`trigger=count|watermark|growth|final`) alongside the working set before the collect.

Completion counters distinguish the covered folder set from the queued stale work: `assetCount` is the filtered, deduped package count; `completed` is `dumped + unchanged + skipCount`; `remaining` is the unfinished portion; `queued` is the number of packages selected for dumping; `dumped` is the number successfully processed; `unchanged` is the number skipped as cache hits; and `skipCount` is the number that could not be dumped and wrote skip detail. Progress also includes `elapsedSeconds` and, after at least one queued asset completes, `estimatedRemainingSeconds`. Final folder jobs preserve `assetCount == dumped + unchanged + skipCount`.

Folder sweeps skip UE-managed packages whose package path contains exact `__ExternalActors__` or `__ExternalObjects__` segments, and helper packages under exact `/Maps/_GENERATED/` paths. With `includeLevels=false`, they also skip level packages and standalone `MapBuildDataRegistry` build-data packages.

For the currently loaded editor world, use `call("actor.describe", {"actorName": "..."})` when live state is the source of truth. For package source of truth, dump the `UWorld` with `asset.dump` and read `actors/*.json`.

**Dump errors and paths**

Dump calls can return:

- `ASSET_NOT_FOUND`: `assetPath` does not resolve.
- `ASSET_FILE_MISSING`: source `.uasset` file does not exist on disk (orphan-stub / baker residue / content-pack leftover). Distinguished from `ASSET_LOAD_FAILED` via a `FPackageName::DoesPackageExist` pre-check, so consumers can filter "not actionable" from genuine load failures.
- `ASSET_LOAD_FAILED`: the package file exists but `LoadObject` returned null (corrupt content, missing/renamed dependency, deprecated class, version mismatch).
- `ASSET_NO_BASELINE`: `diff=true` was called before a normal dump.
- `PATH_TOO_LONG`: generated dump directory exceeds the Windows 260-character path limit.
- `INVALID_FOLDER`: `folderPath` does not start with `/`.
- `DUMP_IN_PROGRESS`: another `asset.dump_folder` sweep is running; poll the active ticket before retrying.
- `REGISTRY_LOADING`: the asset registry initial scan is still in progress.
- `DUMP_WRITE_FAILED`: disk write failed.

The dump mirror does not hash or truncate package paths. If `PATH_TOO_LONG` is returned, shorten the project root or the package path, or ensure Windows long-path support is enabled.

### asset.list

`filter.class` accepts both forms:

| Form | Example | Notes |
|---|---|---|
| Full path | `/Script/UMGEditor.WidgetBlueprint` | Always deterministic. Preferred. |
| Short name | `WidgetBlueprint` | Resolved through `ResolveUClass` (see `Utils/ClassUtils.h`) - searches common script packages and strips `U`/`A` prefixes, matching UE's asset-registry UI. |

If the short name cannot be resolved, the filter falls through to a post-filter string match against the asset's class name and class path. Prefer full paths for determinism - short names work, but a typo or unresolvable name produces an empty result instead of an error. Never construct `FTopLevelAssetPath` directly from a short name; doing so fires a UE ensure at `TopLevelAssetPath.cpp:141`.

**`filter.class` is matched case-INSENSITIVELY**, against both the short class name and the full
class path, and the response echoes `classFilter`, `classFilterMode:"exact"` and
`classFilterCaseSensitive:false` whenever the filter is active. This is the same policy
[`asset.search`](asset.search.md) applies under `classFilterMode:"exact"`; the two verbs used to
disagree, with `asset.list` case-SENSITIVE, so a merely mis-cased `filter.class` returned an empty
page that reads as "no such assets exist" while the identical string worked in `asset.search`. Read
the echoed fields rather than assuming: a response that carries none of them came from a build
predating the convergence.

**Top-level `path` parameter:** honored whether or not `filter` is supplied. If the caller passes both a top-level `path` and a `filter` object without `filter.path` / `filter.pathStartsWith`, the top-level `path` is used. (Earlier behaviour silently dropped it and defaulted to `/Game`.)

**Pagination - `pagination` object (`{ offset, limit }`):** the cap is nested one level down under `pagination`, not a top-level `limit` like the sibling search verbs. `offset` (default `0`) drops that many leading rows; `limit` (default `-1` = unlimited) keeps at most that many. The response always echoes `totalCount` (the full match count *before* pagination), `count` (rows actually returned), `offset`, and `truncated` (a bool, `true` when `offset + count < totalCount`) - so a `count < totalCount` readback is the detectable-elision signal that the page is truncated, and `truncated` states it outright; page through `offset` to walk a large result set. Because the default is unlimited and each row is heavy - `name`, `path`, `class`, `packagePath`, **and** a verbose `tags` string array - the unpaginated array on a populated tree overflows the 10000-char inline budget, returns `outputTooLong`, and spills to `Saved/PinWright/HttpResponses/.../<uuid>.json`, forcing a Read just to pick one asset; pass `pagination.limit` up front to keep the listing inline. A "list so I can pick one" intent wants a small `limit` *and* the per-row projection below, so the page stays inline on the `tags` weight.

**If you are looking for an asset by NAME, this is the wrong verb.** Browsing a folder and reading the listing back is what produces the multi-megabyte spill: `/Engine/EngineMaterials` alone is ~3200 rows. [`asset.search`](asset.search.md) matches the name case-insensitively across a whole mount point, is capped at 50 rows by default, and returns rows without the `tags` array - the same question answered inline instead of through a file on disk. Reach for `asset.list` when the *folder* is the question ("what is in here", "what subfolders exist"), not when the *name* is.

**Per-row projection - `namesOnly` / `fields`:** each row defaults to the full `name` + `path` + `class` + `packagePath` + verbose `tags` array, which is what busts the inline budget even at a small `limit`. To return only the identity the "list assets so I can pick one" step needs, pass `namesOnly:true` (returns just `name` + `path`, dropping `class`, `packagePath`, and the `tags` array; `names_only` also accepted) or an explicit `fields` allow-list - a JSON array (or a single string) of the per-row keys to keep, case-insensitive, valid keys `name`/`path`/`class`/`packagePath`/`tags` (e.g. `fields:["name","path"]`). `fields` wins when both are supplied; omit both for the full unprojected shape. This is the same lever as [actor.list](actor.list.md)'s `namesOnly`/`fields`.

### asset.search

Name-pattern search: matches assets by name using substring or wildcard (`*` / `?`) matching. **`query` is required** - if you have no name pattern (e.g. "list every `SoundWave`"), this is the wrong verb and it errors `[MISSING_REQUIRED_PARAM] Missing required parameter 'query' (type: string)`; use [asset.list](asset.list.md) or [asset.search_assets](asset.search_assets.md) for a class-only list instead.

Params: `query` (required, alias `pattern`, supports `*`/`?`), `path` (optional scope), `classFilter` (optional, one class name **or an array of them**) / `classPathFilter` (optional), `parentClassPath` (optional - native or BP parent, recursive class filter), `classFilterMode` (`exact`|`prefix`|`contains`, default `exact`; aliases `substring` and `starts_with`), `limit` (default 50, max 500), `namesOnly` / `fields` (optional per-row projection).

**`classFilter` takes a string or an array.** `classFilter:["StaticMesh","SkeletalMesh"]` is OR-matched
across the entries - the same array spelling [`blueprint.build_api_index`](blueprint.build_api_index.md) declares for
this parameter name - and each entry is matched under `classFilterMode` against both the short class
name and the full class path. An array holding a non-string or an empty string, and an empty array,
are **rejected** with `INVALID_ARGUMENT`; the array used to be read as the empty string, which ran no
filter at all and returned every match as if it had been filtered. The applied filter is echoed back
in the shape it was sent, and `classFilterMode` is echoed only when a class filter actually ran.

**All three `classFilterMode` values are case-insensitive**, and `exact` is the same policy
[`asset.list`](asset.list.md) applies to `filter.class` - the two verbs agree on the class filter,
by design and by test. An unrecognised `classFilterMode` is **rejected** with `INVALID_MODE`
enumerating the accepted tokens; it used to fall through to `exact` silently, so
`classFilterMode:"regex"` ran a comparison the caller never asked for while the echoed mode
agreed with them.

**Matching is on the asset NAME only, never the path**, and it is case-insensitive in both modes - `query: "grey"` finds `T_GreyAmbient` and `127grey`. Plain text is a substring match; the moment the query contains `*` or `?` it switches to wildcard matching instead, so `"BP_Enemy*"` anchors the start while `"*Enemy*"` is the long form of the substring default. There is no regex mode and no anchoring syntax beyond the wildcard. A query that matches nothing is a **success with an empty `assets` array**, not an error - an empty result and a bad scope path look identical, so check `path` before concluding the asset does not exist.

**Scope and cost.** `path` accepts any mount point (`/Game`, `/Engine`, `/MyPlugin`), not just `/Game`; omitting it walks every mounted asset via `GetAllAssets`. That walk is registry-only - no packages are loaded - so an unscoped query is affordable, but pass `path` when you know the mount, because scoping is what keeps `totalMatches` meaningful rather than "everything in the editor that contains this substring".

**Truncation readback - `count` / `totalMatches` / `truncated`:** `limit` caps the rows RETURNED, never the scan. `totalMatches` counts every asset that matched and `truncated` is `true` when rows were withheld, so `{count:50, limit:50}` is no longer ambiguous between "exactly 50 exist" and "50 of 4000". Raise `limit` (max 500) or tighten `query` / `path` / `classFilter` when `truncated` comes back `true`; there is no `offset`, so narrowing the query is the only way to reach the rest.

**Row size at high limits.** The default 50-row page is ~9 KB against the 10000-character inline budget and stays inline; a 500-row page over deep `/Game` paths will not. Pass `namesOnly:true` (or `fields:["name","path"]`) to drop `class` and `packagePath` when you raise `limit` - the same lever as [`asset.list`](asset.list.md) and [`actor.list`](actor.list.md), minus the `tags` key, which this verb never emits.

### asset.find_by_tag

Searches **UMetaData tags**, which live in the package rather than in the asset-registry row - so unlike its `asset.search` / `asset.list` siblings this verb must **load every candidate asset** to test it. That is the cost to design around: it is not a registry query.

**Two bounds, and only one of them bounds the cost.** `limit` (default 50, max 500) caps the rows **returned**; it cannot cap the walk, because a tag with few or zero matches never fills it. `scanLimit` (default 500, max 10000) caps the candidates **loaded and tested**, and is the parameter that bounds cost. `limit` was previously documented as bounding both - it did not, and an unmatched tag under `/Game` loaded the entire content tree synchronously on the game thread before answering "0 matches".

Read the counters before trusting an empty or short result: `scanned` (candidates actually loaded and tested), `scanCandidates` (what the registry filter offered under `path`), `scanComplete` (the two agree), `truncated` (the inverse), and `stopReason` - `complete`, `limit` (more matches exist; raise `limit`), or `scanLimit` (candidates were never examined, so nothing is known about them). `totalMatches` is emitted **only when `scanComplete` is true**: after a stopped walk there is no measured total, and a number equal to a bound would describe the bound rather than the content.

Narrow `path` in preference to raising either bound - the cost is one synchronous package load per candidate examined, not per match returned.

Tags written by `asset.set_metadata` / `asset.set_tags` are what this reads. Asset-registry tags (a material's `BlendMode`, a mesh's `NaniteEnabled`) are a different store and are not visible here - read those with `asset.get` or `asset.get_metadata`.

### asset.search_assets

Filter-driven listing: every parameter is **optional**, so this is the no-name path for "list assets of class X" or "list assets under path Y". No `query` axis at all - pass `classNames[]` and/or `packagePaths[]`.

Params: `classNames` (array - short names like `SoundWave` or full paths like `/Script/Engine.SoundWave`), `packagePaths` (array, e.g. `['/Game', '/SomePlugin/Maps']`), `recursivePaths` (default `true`), `recursiveClasses` (default `false` - set `true` to include subclasses, e.g. `classNames=['DataAsset']` finds all `UDataAsset`-derived types), `limit` (default 100, `0` = unlimited).

Common recipe - list every asset of a class: `asset.search_assets { classNames:["SoundWave"] }` (equivalently `asset.list { filter: { class: "SoundWave" } }`).

**Truncation readback - `count` / `totalMatches` / `truncated`:** `limit` defaults to 100 (`0` = unlimited) and previously truncated silently, reporting only the post-cut `count`. The response now also carries `totalMatches` (matches before the cap) and `truncated`, so a 100-row answer states whether it is the whole set. `0` removes the cap and therefore the inline-budget protection - prefer a real `limit` plus a tighter `packagePaths` scope.

### asset.delete

Either `path` (aliases: `assetPath`) for one asset or folder, or `paths` (aliases: `assetPaths`) for a batch - at least one is required. `path` / `paths` are the canonical spellings; the `assetPath` / `assetPaths` aliases exist so a caller arriving from a neighbouring asset verb (`asset.get`, `asset.dump`, `asset.bulk_delete`) is not rejected with `UNKNOWN_PARAMS`. Both spellings resolve to the same slot; supplying `path` and `paths` together deletes the union.

The editor deletion primitives cannot run during PIE. In play mode the verb refuses before resolving or changing any requested path with `[PIE_ACTIVE]`; it never translates that global editor state into `missing:true` or a load failure.

**Deletes are no longer forced by default (2026-08-28).** A path anything still references - on disk or in memory - is refused: the entry carries `refused: true`, `errorCode: "ASSET_IN_USE"`, `referencesPreserved: true` and a `referencers` list, the response carries a top-level `errorCode`, and **nothing about the asset changed**: no pointer was nulled, no package was dirtied, no file was touched. Pass `force: true` to delete anyway. Before you do, understand what it buys and costs: `UEditorAssetLibrary::DeleteAsset` is `ObjectTools::ForceDeleteObjects`, which runs `ForceReplaceReferences(nullptr, ...)` over **every live UObject in the editor** *before* the engine decides whether the package may go - replacing each pointer to the target with `null` and marking those packages dirty - with no transaction and no `Modify()`, so it cannot be undone, and `CleanupAfterSuccessfulDelete` can still decline the package afterwards, leaving the `.uasset` intact and only the references destroyed. A forced entry therefore reports `forced: true` and `referencesNulled` (the packages that went dirty across the call, recovered by diffing the editor-wide dirty set because `ForceDeleteObjects` discards its own `FForceReplaceInfo`). **Do not save anything listed there** - `asset.save`, `editor.save_all`, `editor.quit {save:true}` and `asset.fixup_redirectors` all write the nulls to disk; `asset.reload` each listed package instead. `force: true` cannot be made safe from the plugin: the reference-replacement / `IsReferenced` mismatch Epic names in `ForceDeleteObjects`' own terminal `ensureMsgf` is inside `ObjectTools`. The unforced path routes through the engine's own safe delete (`ObjectTools::DeleteObjects` -> `FAssetDeleteModel`, whose `CanDelete()` is literally `!CanForceDelete()`), which is the same primitive [asset.bulk_delete](#assetbulk_delete) has always used - the two verbs no longer disagree. A **folder** is gated as one batch, so one asset held from outside refuses the whole folder; name individual paths to delete the rest.

`success` is `true` only when **nothing survived** - a partial batch is a failure of the request you made. Read `results[]` per entry (`existedBefore`, `deleteReported`, `existsAfter`, `existsOnDisk`, `deleted`, `missing`) plus the `deleted[]` / `failed[]` / `missing[]` roll-ups; `missing` means the path was already absent, which is not the same as this call having deleted it. Every entry is probed twice - the asset registry **and** the `.uasset` on disk - and `existsAfter` is the union, so a file the engine failed to remove can never read as a clean delete. `deleteReported` is the reconciled claim, not the engine's return value: it is `true` only when the engine reported the delete *and* the probe agrees. This verb does not close editors or force-GC for you.

**When it fails, the holder is usually not a Blueprint.** `UEditorAssetLibrary::DeleteAsset` counts objects torn out of **memory**; `ObjectTools::CleanupAfterSuccessfulDelete` then decides separately whether the file may go, and silently drops any package still referenced in memory - including by another `RF_Standalone` object inside the same package - with no error and no log line. So a failed entry carries `inMemoryReferencers` (the holders found by the engine's own `GatherObjectReferencersForDeletion` over the package, internal ones included) and `referencedByUndoBuffer`; `referencingBlueprints` answers the narrower on-disk question and is frequently empty on exactly these failures. Release the named holders and retry; if nothing is named, a native (non-`UPROPERTY`) holder or a read-only file is left, and an editor restart clears the former.

**`memoryDiskDivergence`** on an entry means the engine reported those objects deleted while the `.uasset` is still on disk: the editor's view and the content directory disagree until the editor restarts. Do not re-save assets that referenced the target - `ForceDeleteObjects` has already nulled their pointers, and saving them would bake in references to an asset that still exists on disk.

For a large batch that should also clean up its redirectors, prefer [asset.bulk_delete](asset.bulk_delete.md) (its batch slot is spelled `assetPaths` and is **required**), which runs a redirector fixup afterwards by default - scoped to the deleted assets' own folders.

### asset.bulk_delete

Deletes many assets in one call and then cleans up the redirectors left beside them. `assetPaths` (array) is **required**; `showConfirmation` defaults to `false`; `fixupRedirectors` defaults to `true`; `fixupScope` defaults to `"paths"`.

**`success` is `true` only when nothing the caller named survived**, and every requested path is probed afterwards against **both** the asset registry and the `.uasset` on disk - `existsAfter` is the union. Read `results[]` per entry (`path`, `existedBefore`, `attempted`, `existsAfter`, `existsOnDisk`, `deleted`, `missing`) plus the `deleted[]` / `failed[]` / `missing[]` roll-ups and the `deletedCount` / `failedCount` / `missingCount` / `requestedCount` / `attemptedCount` counters. A partial batch is refused with `BULK_DELETE_FAILED`, and the full response body travels with that error. `engineDeletedCount` is `ObjectTools::DeleteObjects`' own return, kept separate on purpose: `AddExtraObjectsToDelete` appends secondary and external-package objects, so it can exceed the request and is not evidence that any named asset went.

**Until 2026-08-28 it verified nothing.** `deleted[]` was the requested list, filled in before the delete ran, and `success` was `DeletedCount > 0` - so deleting 1 of 20 answered `success: true` with all 20 names in `deleted[]`. A path that failed `DoesAssetExist` / `LoadAsset` was dropped silently and `requested` counted what *loaded* rather than what was asked. If you are reading an older transcript that treats this verb's `deleted[]` as unverified, that is the defect, not current behaviour (board `B-bulk-delete-unverified-deletions`).

**`attempted: false` means the path was never handed to the engine** - it did not exist, or it existed and failed to load. Such a path is reported under `missing[]` when it was already absent; it is never counted as a deletion this call performed. `requestedCount` is the length of the array you sent, so it always reconciles against your own input.

**The failures this verb hides are not the ones `asset.delete` hides.** This route (`DeleteObjects` -> `DeleteItems` -> `FAssetDeleteModel::DoDelete` -> `DeleteObjectsUnchecked`) passes `bPerformReferenceCheck=false`, so a referenced asset is not culled per-object: `FAssetDeleteModel::CanDelete()` rejects the **whole batch** up front and the call errors honestly. What is silent is a read-only package (`MakeReadOnlyPackageWritable` answers its own dialog with the unattended default), an `OnAssetsCanDelete` veto inside `DeleteSingleObject`, and - after the count is already final - `CleanupAfterSuccessfulDelete` dropping any package whose filename it cannot resolve and discarding the return of `IFileManager::Delete`. Those leave a `.uasset` behind, which is why `existsOnDisk` is emitted separately and `memoryDiskDivergence` is set when the registry row is gone but the file is not. To learn *which* live object blocked an entry, retry that path through [`asset.delete`](#assetdelete), which names the in-memory holders.

**`fixupScope` bounds what the automatic fixup may delete, and the default is the deleted assets' own folders.**

- `"paths"` (default) - the fixup only considers redirectors sitting **directly in** the package folders the deleted assets live in. Non-recursive on purpose: a recursive sweep rooted at one deleted asset's folder re-widens to everything beneath it, and for an asset at the top of a content root that is the whole project again.
- `"project"` - every mounted content root. This fixes up and **deletes every `ObjectRedirector` in the project**, including ones with no relationship to this call, and re-saves their referencing packages. Opt in deliberately; there is no undo.
- Any other value is refused with `INVALID_ARGUMENT`, **before** anything is deleted. There is no fall-back, because the wrong guess is a project-wide purge.

**This default was wrong until 2026-08-21 and it destroyed host content.** The fixup built a class-only registry filter, so it collected every redirector in the project on the default path. In one host that irreversibly deleted 4 pre-existing `ObjectRedirector` packages the caller never named and rewrote 13 packages that referenced them; they were recoverable only because they were tracked in git. If you are reading an older transcript or an older copy of this page that describes the whole-project sweep as intentional, it is describing the defect.

**Read `redirectorsDeletedOutsideScope`.** It is present on every response that ran a fixup, empty array included, and lists the long package names of redirector packages this call removed from **outside** the folders you named. On the default scope it should be empty; on `"project"` it is the bill for the opt-in. The fixup's other counters (`redirectorsConsidered`, `redirectorsDeleted`, `redirectorsDeletedPaths`, `referencingPackagesFound` / `referencingPackagesSaved`, `failedPackages[]`, `codeReferences[]`) are the shared shape documented under [`asset.fixup_redirectors`](#assetfixup_redirectors) below, and `fixupScope` echoes the scope that actually ran.

`fixupRedirectors: false` skips the fixup entirely and is still the narrowest option; use it when the deletion cannot have stranded a redirector.

### asset.bulk_rename

Applies prefix / suffix / search-replace to many asset leaf names in one call. **It is a mutating, non-idempotent verb whose three documented behaviours were all wrong until this page was written** - read this before running a batch.

`partial` follows the [shared batch contract](#batch-mutator-outcome-contract). Preflight retains malformed and missing inputs and refuses invalid or occupied targets; `checkoutFiles:true` also makes unavailable source control a preflight refusal. Each row captures `oldPath` **before** mutation and gives the requested `newPath` / `newName`; attempted rows add `renameReported`, `actualPath`, and `changed`. A name that already matches is a successful, unattempted `changed:false` row. Consequently `renamed` counts observed path changes while `succeeded` also includes already-satisfied no-op rows. Runtime checkout/rename failures are per row and never imply rollback of earlier renames.

**Order is search-replace -> prefix -> suffix.** The registered summary used to claim prefix -> suffix -> search-replace, which is the reverse of what runs. The difference is observable: on `Rock` with `prefix:"SM_"` and `searchText:"SM_"` / `replaceText:"S_"`, the documented order predicts `S_Rock`; the code produces `SM_Rock`, because the replace runs first against a name that does not yet contain `SM_`.

**`searchText` is case-INsensitive.** It was documented case-sensitive but is implemented as `Replace(..., ESearchCase::IgnoreCase)`, so `searchText:"sm_"` also rewrites `SM_`. There is no `caseSensitive` opt-out on this verb yet - unlike [`actor.list`](actor.md), it does not take the shared `matchMode`/`caseSensitive` pair, because it performs a replacement rather than a match. If you need case-exact replacement, narrow `assetPaths` to the assets you have already confirmed client-side.

**`prefix` and `suffix` are applied unconditionally.** Both were documented "only added if not already present"; neither is guarded. Re-running the same call therefore double-applies: `SM_Rock` becomes `SM_SM_Rock`. Treat every `bulk_rename` as one-shot, and never retry a failed batch without first re-reading the current names.

Every rename leaves a redirector at the old path - follow with [`asset.fixup_redirectors`](#assetfixup_redirectors), which this verb does **not** run for you.

Safe recipe: run the call against a single asset first, read back the resulting name, then run the batch.

### asset.fixup_redirectors

Rename and move operations leave `UObjectRedirector` stubs at the old paths so existing references keep resolving. `asset.fixup_redirectors` walks those redirectors, re-saves every referencing asset against the resolved target path, then deletes the redirector packages.

Run this once after any batch of `asset.rename` / `asset.move` / `asset.bulk_rename` calls. Skipping it leaves the content tree functional but cluttered, and asset registry queries can return surprising results until the redirectors are gone.

**The two verbs have opposite defaults on scope, deliberately.** `directoryPath` is optional here and an empty one still scans the entire project: this verb's whole job is the sweep, so a caller who typed its name is asking for exactly that, and the scope parameter is right there to bound it. [`asset.bulk_delete`](#assetbulk_delete) is the opposite case - it is named for a delete, the fixup is a side effect, and its `fixupScope` therefore defaults to the deleted assets' own folders and reaches the project only on an explicit `fixupScope: "project"`. Pass `directoryPath` here whenever you know the folder; the unscoped run loads and re-saves every referencing package in the project.

**A redirector is deleted only when every package referencing it was re-saved.** Both verbs report the outcome, so read it rather than assuming a clean sweep:

- `redirectorsConsidered` / `redirectorsDeleted` - offered vs. actually removed.
- `redirectorsDeletedPaths[]` - the long package names actually removed, measured after the delete rather than predicted from the request. Present only when something was deleted. A count alone cannot tell you *which* packages went, which is how a project-wide purge stayed invisible to callers.
- `redirectorsSkipped` - present only when a redirector pointed at nothing; those are left alone.
- `referencingPackagesFound` / `referencingPackagesSaved`.
- `failedPackages[]` - referencing packages that would not re-save (read-only file, save error). Their redirectors survive on purpose: deleting one would break the reference the package still holds. Clear the cause and re-run.
- `codeReferences[]` - referencing packages that are compiled in. A C++ reference to the old path cannot be fixed from the editor at all; change the code.

Under automation this does **not** use the engine's `IAssetTools::FixupReferencers`, which is unusable here: it ends in a modal report whose answer the engine reads with an unchecked `TOptional::GetValue()`, so once PinWright's unattended scope cancels the window the editor hard-asserts and dies (UE 5.8 `AssetFixUpRedirectors.cpp:939` -> `Dialogs.h:280` -> `Optional.h:372`). PinWright drives the same steps directly instead. Two engine behaviours are consequently **not** reproduced: collections referencing a deleted redirector keep a stale entry, and packages loaded in order to be re-saved stay resident for the session.

### asset.generate_lods

Generates the standard StaticMesh LOD chain for the optional single `assetPath` (aliases: `meshPath`, legacy `landscapePath`) followed by any `assetPaths[]` rows. `lodCount` / `numLODs` selects the total count including LOD0; values above UE's `MAX_STATIC_MESH_LODS` cap (8) are rejected with `INVALID_PARAMS`. `partial` follows the [shared batch contract](#batch-mutator-outcome-contract): malformed, unsafe, missing, and non-StaticMesh inputs stay in `items[]`; default mode refuses before entering the rebuild path when any preflight row fails.

The operation still runs through the shared safe point and render-consumer quiesce guard. Each row carries `inputPath`, resolved `path`, and `requestedLodCount`; after a build, `actualLodCount` is read from the built/render LODs rather than merely echoing source-model metadata. `processed` counts only rows whose compilation reached a terminal result and whose actual render LOD count matched the request. A mesh disappearing after preflight or a render consumer that cannot be quiesced becomes a per-row runtime failure. Other meshes may already have rebuilt when a later runtime readback fails, so the failed envelope is not a rollback statement.

The request starts one deadline before the guarded callback; rows are processed sequentially against that same deadline. Each row triggers one StaticMesh build and waits through the shared compile pump before readback or save. `timeoutSeconds` controls the bounded request-wide wait (0-60 seconds, default 60); an unfinished wait reports `timedOut:true`, does not attempt a save, and does not count the row as processed. A row reached after an already-exhausted deadline is timed out before mutation. If the deadline expires during setup or compilation, the row is not saved or reported successful, though in-memory edits may remain. Its explicit non-attempted save state is `saveRequested:false`, `saved:false`, and `saveState:"notRequested"`; when compilation is still in flight, the render guard remains retained until the mesh reaches a terminal state. `save` defaults to `true`; a durable write uses the shared disk-presence save helper and reports `saveRequested`, `saved`, `saveState`, `saveDetail`, and measured size fields per row. A requested save that is not durable is a runtime failure, not a completed row. `save:false` explicitly skips the disk write, leaves the package dirty, and reports `saveRequested:false`, `saved:false`, and `saveState:"notRequested"`. For a request containing exactly one original input, the same persistence fields are also present at the top level; batch callers should inspect `items[]`.

### asset.generate_thumbnail

Render an asset's thumbnail offscreen - the same picture the Content Browser shows - and optionally write it to disk. It needs no level, no open asset editor and no viewport, which makes it the cheapest way to look at an arbitrary asset. It is **read-only**: the asset is never modified and never left needing a save.

The initial existence and load checks are PIE-safe. A true miss returns `[ASSET_NOT_FOUND]`, a registry-known asset that cannot be loaded returns `[LOAD_FAILED]`, and play mode is never translated into either a false path miss or a transient package-path diagnosis.

Args:

- `assetPath` (string, **required**).
- `width` / `height` (number, default `512`). Treated as a **maximum** by some asset types - a texture keeps its own aspect ratio - so the response reports the size actually rendered, plus `requestedWidth` / `requestedHeight` when they differ.
- `outputPath` (string) - where to write the file. **The extension picks the format**: `.jpg` / `.jpeg` write JPEG, everything else (including no extension) writes PNG. The response echoes the real `format`.
- `primitive` (string) - **material assets only**: the preview shape. One of `sphere`, `cube`, `plane`, `cylinder`, `shaderBall`, or `mesh`. Defaults to whatever shape the material itself is configured for. The response reports the shape **actually drawn**, plus `requestedPrimitive` and `primitiveReason` when the engine substituted a different one.
- `primitiveMesh` (string) - StaticMesh asset path, required when `primitive: "mesh"` and rejected otherwise. It must be a StaticMesh: a SkeletalMesh would silently render as a flat plane, so it is refused instead.
- `azimuth` / `elevation` (number, degrees) - camera angle, in the same vocabulary as `camera.frame_actor`: azimuth 0 sits on +X and increases toward +Y, elevation is measured above the horizon. Defaults to the asset's own stored angle. **Neither applies to the plane** - see the gotcha below.
- `zoom` (number) - camera distance offset in world units; negative moves closer.
- `allowFallback` (boolean, default `false`) - opt into retaining an image with a known Default Material substitution. Without it, a failed shader map with errors or an unassigned material used by a drawn section returns `MATERIAL_FALLBACK` instead of success. An incomplete shader state is uncertainty rather than a known substitution and does not require this opt-in.

```js
// The Content Browser picture, as a real PNG.
call({ path: "asset.generate_thumbnail",
       args: { assetPath: "/Game/Props/SM_Chair", width: 800, height: 800,
               outputPath: "D:/review/SM_Chair.png" } })

// A material on a cylinder, seen from the side.
call({ path: "asset.generate_thumbnail",
       args: { assetPath: "/Game/Materials/M_Brick", primitive: "cylinder",
               azimuth: 45, elevation: 10, outputPath: "D:/review/M_Brick.png" } })
```

**Omitting `outputPath` returns metadata only** - `success`, `width`, `height`. No image bytes, no temp path, nothing you can look at. An on-disk export is the only way to obtain pixels, so do not spend a call probing for an inline mode.

Gotchas worth knowing before you frame a shot:

- **The thumbnail camera is square and fixed at a narrow field of view.** A non-square `width`/`height` is stretched, not re-framed, so keep it square unless you want the distortion. For a framed, correctly-proportioned picture of a *placed* actor, use `camera.frame_actor` instead.
- **`primitive` reports what was drawn, not what was asked for.** The engine substitutes a flat plane for some materials whatever you request: a UI-domain material always, and a particle-sprite or Niagara-usage material *through its base material* (`UMaterial::ShouldForcePlanePreview`). The second rule is why a `UMaterial` can render a sphere while its own **material instance** renders a flat quad from the identical request - the engine reads the flag off the master, not off the asset you named. PinWright defeats the defeatable rules on both objects; when one survives, `primitive` comes back as `"plane"` with `requestedPrimitive` and `primitiveReason` beside it. Read `primitive` back rather than assuming the request was honoured.
- **The plane ignores `azimuth` and `elevation`.** The thumbnail plane is a zero-thickness quad pinned to one fixed attitude and it does not track the orbit, so any angle away from its default renders it edge on - a two-pixel sliver in an empty frame that reads as a broken material. Both angles are therefore dropped when the resolved shape is the plane, the asset's stored angles are used, and the response says so with `elevationApplied` / `azimuthApplied` `false` and a `cameraReason`. Use `cube`, `cylinder` or `sphere` when the angle is the point.
- **Every response describes its own pixels.** `imageStats` carries `meanLuminance`, `luminanceVariance`, `minLuminance`, `maxLuminance`, `litPixelCount`, `litPixelFraction`, `litLuminanceThreshold`, `toneLevelsUsed` and `toneLevelMinPixels`, measured whether or not `outputPath` was given, by the same classifier `render.capture_asset_preview` / `render.capture_open_level` publish - so `blank`, `crushed`, `blownOut` and `toneLevelsUsed` mean the same thing on all four verbs. `blank` is "nothing was drawn"; `crushed` / `blownOut` are "what was drawn cannot be read". Both are **reported, never rejected**: a flat or black asset is a legitimate thumbnail and the file is written either way. `frameWarning` is present only when the frame is degenerate, and `imageStatsMeasured: false` (with no `imageStats` block) means the render returned no pixels - never read an absent field as a measurement of zero.
- **Read `imageStats` before comparing two thumbnails.** The first render of a subject after a compile or a fresh load used to come back blank or half-drawn while an identical second call returned the finished picture, with byte-identical success payloads. A t0/t1 pair spanning that measures the warm-up, not the subject - it invalidated two motion proofs on one build. The verb now waits for the subject's async build, its shader map and the mips of the textures it samples before rendering (`readiness`, with `shaderMapCompleteBefore` / `shaderMapCompleteAfter` read back off the material resource), and re-renders once - only - when the first frame *measures* degenerate. `renderPasses` says how many passes it took; when it is 2, `coldFrameRetry` carries the first pass's numbers and `differingPixels` / `differingFraction` between the two. A high `differingFraction` there is the verb telling you its own first frame was not settled.
- **Material fallback is a separate shader-state verdict, not an image heuristic.** After the final render or cold-frame retry, the verb pumps any already-submitted material compile for one bounded wait, probes again, and always emits `materialReadiness`. It reads the resource of the actual material interface, so an instance's static permutation is not replaced by its master's verdict. The block distinguishes `compiled`, `compiling`, `failed`, `usingDefaultMaterial`, `fallbackOccurred`, and `fallbackPossible`, with one `subjects[]` row and `errors[]` per material. Mesh rows also say whether the slot is used by a drawn LOD0 section; both Static Mesh and Skeletal Mesh thumbnail renderers disable the engine LOD show flag, which selects LOD0. An unused broken slot remains readiness evidence but does not reject the image. An intentionally selected engine Default Material reports `usingDefaultMaterial:true` without fallback. A failed map with errors or an unassigned drawn slot is a known substitution and returns `MATERIAL_FALLBACK` with `success:false` by default even when the PNG and `imageStats` look healthy; `allowFallback:true` retains that image and adds a reason-aware warning. A final `notCompiled`, `outstanding`, or `timedOut` state is not promoted to failure: the capture succeeds without opt-in, reports `fallbackPossible:true` and `possibleReason:"shaderMapIncomplete"`, and warns the caller to inspect the exact subject status.
- Recommended size for a single still is a long edge of **800**; the `512` default is kept for backwards compatibility. The offscreen target the engine renders into is capped at 2048, so larger requests come back smaller - compare `width` against `requestedWidth` rather than assuming you got what you asked for.
- Assets with no thumbnail renderer of their own, or a requested file that could not be written, produce `THUMBNAIL_GENERATION_FAILED`; that capture/file failure takes priority over material fallback policy.
