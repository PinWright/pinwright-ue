# safe-mutation-save

Workflow for persistent editor changes through MCP. The rule is: read current state, choose the narrowest current writer, verify with the same evidence shape, then save explicitly and wait for any save job.

## When To Use This

Use it before changing persistent asset or Blueprint defaults, Widget Blueprint trees or
properties, authored material/Niagara/audio/animation/texture data, or actor/level/world/
property/container state.

## Recipe

1. Capture the current state.
   - Broad package evidence: `asset.dump` or `asset.dump_folder`.
   - Widget tree evidence: `widget.export_xml` or `widget.describe`.
   - Reflected fields: `property.list` or `property.get`.
   - Live actors/components: `actor.describe` or `system.inspect.inspect_object`.
2. Pick the narrowest writer that already exists.
   - Domain writers first: `widget.*`, `blueprint.*`, `material.authoring.*`, `niagara.*`, `audio.authoring.*`, `animation.authoring.*`, `texture.*`, `actor.*`, `level.*`.
   - `property.set` / `property.reset` for a whole reflected field when no typed writer exists.
   - `container.array`, `container.map`, or `container.set` for one entry inside a reflected container.
   - `python.execute` only when the wiki has no typed RPC for the operation.
3. Mutate the smallest target that expresses the change.
4. Verify read-after-write with the same read surface used in step 1.
5. Save explicitly unless the method page states that the writer already saves as part of the call. Prefer the single-package save over a blanket flush.
   - One content package (the asset you just mutated): `asset.save`. Persists only that package, not every dirty one — the narrow choice after a graph/property mutation such as `blueprint.set_default`, `property.set`, or a `material.authoring.*` edit.
   - General dirty packages and worlds: `editor.save_all`.
   - Active persistent level only: `level.save`.
   - Save As for maps: `level.save_as`.
6. If saving returns `ticket_id`, poll `system.job_status` until terminal before claiming the asset is persisted.
7. If you used an asset dump baseline, re-run `asset.dump` or `asset.dump_folder` after save so the mirror matches the current package state.

## Choosing The Writer

| Desired change | Prefer | Why |
|---|---|---|
| Widget tree structure | `widget.import_xml`, `widget.add`, `widget.reparent_widget`, `widget.remove_widget`, `widget.wrap`, `widget.replace_class` | These handlers understand WidgetTree ownership, slots, and Blueprint variable registration. |
| Widget property or slot value | `widget.set` | It handles widget and slot properties in one call. |
| Blueprint class defaults | `blueprint.set_default` when the intent is specifically Blueprint default editing; otherwise `property.set` | `blueprint.set_default` owns the Blueprint-default workflow: it compiles the Blueprint and marks it for save, but does **not** write the `.uasset` (an immediate write on a Blueprint is a known corruption vector), so it answers `markedForSave:true, saved:false, pendingFlush:true` — follow with `asset.save`. `property.set` is the generic reflected writer. |
| One reflected scalar/struct/array replacement | `property.set` | It writes the field through reflection and can recompile Blueprint CDO targets. |
| One array/map/set element | `container.array.*`, `container.map.*`, `container.set.*` | Avoids replacing the entire container value when only one element changes. |
| Single content package (one mutated asset) | `asset.save` | Persists just that one package to disk — the targeted alternative to flushing every dirty package. `saved` is gated on the `.uasset` actually reaching disk; a throttled/deferred no-write reports `saved:false` + `pendingFlush`. |
| Level package save | `level.save` | Saves the active editor world's persistent level package. |
| All dirty packages | `editor.save_all` | Synchronous save covering dirty worlds and content packages; returns the result inline (no job ticket). |
| A mutation that left the package clean | `asset.mark_dirty`, then the matching save | A save no-ops on a clean package. Sets the flag only — writes nothing — and reports the resolved package (under World Partition an actor's package is not the map's). `asset.is_dirty` is the read-only check before and after saving. |

## Dirty State And Runtime State

Most asset-side writer calls mutate in-memory editor state and mark packages dirty. They are not persisted until a save method succeeds.

Read the persistence fields, not the top-level `success`. A writer that only marked the package dirty answers `markedForSave:true, saved:false, pendingFlush:true` — the edit is real but lives in memory and is discarded on a cold editor restart. A writer that wrote the file answers `saved:true` with no `pendingFlush`. Verification blocks separate the same two facts: `existsAfter` means the asset is resolvable right now, `existsOnDisk` means a package file exists, and `pendingSave` appears while the package still holds unwritten changes. Only `saved:true` / `existsOnDisk:true` are claims about durability. Saves are also rate-limited per asset (0.5 s): a second save of the same asset inside that window is skipped, and when the package is still dirty it reports `saved:false` + `pendingFlush:true` rather than success — retry with `asset.save {force: true}`, which bypasses the throttle. That remedy applies **only** when `saveState` is `deferred`; `asset.save` reports a PIE refusal as `PIE_ACTIVE`, `saveState:"blockedByPie"`, and `pendingFlush:false` because no flush can run until the play session ends.

When a mutation does **not** dirty the package, the save silently does nothing and the edit is lost on editor close — the failure looks identical to success. Check with `call("asset.is_dirty")` and repair with `call("asset.mark_dirty")` before saving; do not reach for `EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)` from MCP, which rewrites packages that did not need rewriting. From bundled UE 5.8 Python, the engine exposes neither `asset.mark_package_dirty()`, `Actor.mark_package_dirty`, nor `Package.set_dirty_flag`; use the reflected `unreal.PinWrightPackageLibrary.mark_package_dirty(asset)`, assert `is_package_dirty(asset)`, force `unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)`, then require `is_package_dirty(asset)` to be false and verify the asset survives reload — see [`python`](python.md).

Runtime instance edits are different. A `call("property.set")` against a live PIE object changes that transient instance, not the source asset, unless the target is an asset path or Blueprint CDO. Use [`runtime-uobject-inspection`](runtime-uobject-inspection.md) for live UObject reads and only save asset/package edits.

## Save States

A save report is `saveRequested` + `saved`; requested non-durable work normally carries `pendingFlush:true`. A measured `blockedByPie` refusal carries `pendingFlush:false` because it is not queued work. Handlers that measured the outcome also carry `saveState` and `saveDetail`. `saved:true` is exactly `saveState` in {`written`, `alreadyCurrent`}.

| `saveState` | Durable | Retry? | Means | Do |
|---|---|---|---|---|
| `written` | yes | — | This call wrote the `.uasset`. | Nothing. |
| `alreadyCurrent` | yes | — | Nothing needed writing; the file already matched memory. | Nothing. |
| `deferred` | no | yes, now | The edit is in memory and the package is still dirty — the 0.5 s per-asset throttle skipped the write. | `asset.save` (`force: true` bypasses the throttle) or `editor.save_all`. |
| `blockedByPie` | no | yes, but only after PIE ends | Refused before writing a byte: a Play-In-Editor session is running, and the editor refuses **every** single-asset save while one is. `asset.save` returns `PIE_ACTIVE` with `pendingFlush:false`. | Wait for the session to end, then re-issue and require `written`. `force: true` and `editor.save_all` hit the same refusal. `pieWorlds` on the response names the session; `editor.pie_status` polls it; `editor.stop` ends it. |
| `failed` | no | no | The save was attempted and produced no durable revision: refused (Blueprint integrity gate), errored, or reported success while the file on disk did not move. | Do not flush — it will not help. The editor log carries a `SaveAssetToDiskReportingPresence` line naming the engine outcome, size before/after, timestamp before/after and the pre-save dirty flag. |
| `notPersistable` | no | never | Transient or unmounted package. | No flush ever persists it; recreate under a mounted content path. |
| `diskStateDiverged` | no | no | Refused before writing a byte: the `.uasset` on disk changed since the package was loaded or last saved. | Do not flush — it repeats the refusal. `asset.reload` and redo the edit, or re-issue with `overwriteDiskChanges: true`. See below. |
| `notRequested` | no | yes | `save` was `false`. | `asset.save` / `editor.save_all` when durability is wanted. |

`pendingFlush:true` is the legacy requested-but-not-durable signal. `blockedByPie` deliberately overrides it to `false`: no flush can run until PIE ends, and the caller must issue a new save request afterwards. Read `saveState` before retrying.

`sizeBytes`, where a save verb reports it, is the size of the `.uasset` **on disk after the call** — not the number of bytes this call wrote. On a `saved:false` over an asset that already existed it is therefore the previous revision's size, which reads exactly like a successful write; the response then carries `sizeBytesIsStale: true`. Never treat a plausible `sizeBytes`, or one equal to a known-good earlier save's, as evidence of durability.

A response carrying `saveRequested` but no `saveState` comes from a handler not yet threaded through; `saved` is then the only claim it makes.

## PIE Blocks Every Save In The Editor

While a Play-In-Editor session is running, the editor's single-asset save API refuses every write in the process — `EditorScriptingHelpers::CheckIfInEditorAndPIE`, which fires on `GEditor->PlayWorld || GIsPlayInEditorWorld` and consults nothing else. It is not a property of your asset, your package or your call: `force: true` does not bypass it, `editor.save_all` does not bypass it, and the block appears and disappears as sessions start and stop, so the *same* call succeeds and then silently does nothing minutes apart.

In a shared editor the session usually belongs to a different agent doing unrelated, correct work, so nothing in your own actions hints at the cause. The save verbs therefore report it directly. `asset.save` returns a typed error with the measured save report:

```text
ERROR PIE_ACTIVE
{"saved": false, "pendingFlush": false, "saveState": "blockedByPie",
 "pieActive": true, "editorMode": "PIE",
 "pieWorlds": [{"pieInstance": 0, "mapName": "T_UI",
                "worldPath": "/Game/Test/UEDPIE_0_T_UI.T_UI"}]}
```

`pieActive` / `editorMode` / `pieWorlds` are emitted by every save report, including the ones that carry no `saveState` because their handler is not threaded through — so a verb that saves on your behalf still names the blocker. **On a save report that already says the save was requested and is not durable, the absence of `pieActive` means PIE was not running.** It is a positive marker of a measured condition, not an omitted measurement.

`editor.save_all` answers the same condition at the batch boundary: it returns `PIE_ACTIVE` before attempting any package, rather than reporting a partial `pieActive` / `BlockedByPie` result. Stop PIE, then retry the batch.

`model.compile {save: true}` answers it *before* doing its work, with the same code and the same report fields: a compile rebuilds the target `UStaticMesh` in place, so meeting the refusal at the save would leave the loaded asset ahead of its `.uasset` with no verb that reconciles them. The refusal states that nothing was built and nothing was written; `save: false` still compiles in memory during a session.

The correct response is to stop authoring against a save path that cannot work: hold the dirty asset, poll `editor.pie_status` until `inPie: false`, re-issue the save and require `saveState: "written"` before treating the edit as durable. Do not keep mutating in memory — a crash discards all of it.

## Never Change The Working Tree Under A Live Editor

A loaded package holds its own copy of the asset. Replace the `.uasset` on disk behind the editor's back — `git checkout`, `git reset --hard`, `git clean -fd`, a manual revert, a rebase, another tool, a second editor, a teammate's sync — and the resident package becomes a fork of an older revision. The editor is never told.

Saving such a package used to discard the on-disk revision and answer `saved:true`. It no longer does: the single-asset write path measures the file before writing, and a save that would overwrite an out-of-band change is refused with `SAVE_DISK_STATE_DIVERGED` and `saveState: "diskStateDiverged"`. The refusal payload's `diskState` block names the file and publishes both sides — `diskSavedHash` measured out of the file by that call, `loadedSavedHash` from the resident package. `probed:false` means the comparison could not be made, which is not the same claim as `diverged:false`; it comes with a `reason` and no measured fields.

Recovery, in order of preference:

1. `asset.reload` the package, re-read the current state, and redo the edit on top of it. This is the only route that keeps both revisions' intent.
2. `source_control.revert`, when the change came from source control — it resynchronizes the loaded package rather than leaving it stale.
3. `asset.save {overwriteDiskChanges: true}` to discard the on-disk revision deliberately, once you have read the refusal and decided the in-memory state wins.

`force: true` is not option 3. It bypasses the 0.5 s save throttle and nothing else; a forced save over a diverged file is refused identically. The gate itself is free of false positives from ordinary work: the ledger is `UPackage::GetSavedHash()` against the hash the engine embeds in the `.uasset`'s own package summary, and the engine updates both on every load and every save to a mounted path — including saves this plugin never sees. Autosaves write to `Saved/Autosaves/` and do not touch the asset's file.

The check is skipped, and reported as `probed:false`, for a package that has never been read from or written to a file — every create-and-save flow lands there, so creating an asset over an existing path is still the create verb's own decision, not this gate's.

## Source Control

When the project has an active UE source-control provider, use `call("source_control.status")` before persistent asset changes and `call("asset.source_control_checkout")` before writing locked assets. Use `call("asset.source_control_submit")` only when the workflow explicitly includes submitting files; normal MCP edits do not require a submit.

## Verification Patterns

- After `widget.*`, read `widget.export_xml` / `widget.describe` and optionally capture through [`visual-review`](visual-review.md).
- After `property.*`, read `property.get` or `property.list` with override state.
- After Blueprint or material edits, use the domain inspect/decompile surface (`blueprint.inspect`, `blueprint.decompile`, `material.decompile_mgir`) that matches the edit.
- After asset/package edits, use `asset.dump(diff=true)` when a baseline exists, or a normal dump when it does not.
- After save jobs, trust the terminal `system.job_status` result, not the initial `ticket_id` response.

## Related Pages

- [`asset-audit`](asset-audit.md) for baseline and diffable dumps.
- [`property`](property.md) for reflected UObject property reads/writes.
- [`container`](container.md) for array/map/set entry edits.
- [`editor`](editor.md) and [`level`](level.md) for save methods.
- [`workflows`](workflows.md) for the index of task-oriented guides this one belongs to.
