# level-building.build-scripts

Making a level rebuildable rather than hand-assembled: outliner organisation and tag-based undo, one script per layer, the editor dialogs that deadlock automation, and getting edits actually written to disk. Read it before the level grows past what you would be willing to rebuild by hand.

## Organisation And Cleanup

- File actors as you spawn them with `actor.spawn_batch {folder: "MyLevel/Walls"}`, or later with `actor.set_folder {actorNames, folderPath}` (`folderPath`, not `folder`).
- Tag each pass with `actor.add_tag {actorName, tag}` and undo it with `actor.delete_by_tag {tag}`. Tags are `FName`s matched **case-INSENSITIVELY** in both `actor.find_by_tag`'s default `matchType:"exact"` and `contains`; no match returns `count: 0`.
- `actor.create_snapshot {actorName, snapshotName}` stores a transform for that actor; `actor.restore_snapshot` restores it. Snapshots are in-memory and lost when the editor closes.
- Actor **labels are not unique**. A collision fails with `AMBIGUOUS_ACTOR_NAME` and lists each unique internal object name; retry with `candidates[].name` or `candidates[].path`. `actor.find_by_class` / `actor.find_by_tag` also return `objectName`; rename labels with `actor.set_label`.
- Bare `actor.list {}` spills on a populated level; pass `filter`, `limit`, or `namesOnly`.
- **`actor.list` defaults to a case-INSENSITIVE SUBSTRING filter** against label or internal name. It is unanchored: `SH_` matches `Brush_0` and `OP_` matches `Top_Pad`, while `totalMatches` makes the wrong count look authoritative. Use explicit semantics:

  ```js
  call("actor.list", { filter: "SH_", matchMode: "prefix", caseSensitive: true, namesOnly: true })
  call("actor.list", { filter: "^SH_", matchMode: "regex", caseSensitive: true, namesOnly: true })
  ```

  `matchMode` is `contains` (default, alias `substring`) / `prefix` (alias `starts_with`) / `exact` / `regex`; `caseSensitive` defaults to `false`. Regex is unanchored, so use `^`; an invalid pattern returns `INVALID_PATTERN`, not zero. The response echoes the three settings. The same parameters work on `system.inspect.list_objects` and `system.inspect.find_objects_by_class`, which match internal object name/class, not display label.
- For a one-capture hide, set and immediately restore transient `bHiddenEdTemporary`; unlike `SetActorHiddenInGame` or deletion it is not serialized and does not dirty the package.

## Build Scripts Are The Source Of Truth

Hand-edited actors do not survive the next rebuild. Drive each layer from one script, run with `python.execute` and an absolute path:

- **One prefix and one script per layer.** For example, `TW_` towers, `FO_` foliage, `WT_` water; record the ownership table where both script and reader can see it.
- A script destroys only its own prefix, then rebuilds from parameters. Never blanket-wipe a shared prefix; delete explicit labels instead.
- Layers rebuild independently when they query the current ground rather than baking absolute Z, but upstream changes silently invalidate dependents: terrain affects grounded assets, path width affects relative placements, re-scatter affects movers, and material edits invalidate earlier colour measurements. Re-run downstream layers; see [`level-review`](level-review.md).
- Put destructive work behind `DRY_RUN`, and finish with a readback verification (counts, symmetry, worst tilt, min/max ground gap). Author materials and baked meshes in a separate first script; layer scripts consume them.

## Blocking Modals Will Deadlock Automation

RPCs execute on the game thread, so **any editor dialog freezes every in-flight and subsequent call from every client until a human clicks it**. To an agent it is indistinguishable from a crash, and it stalls unrelated concurrent work too.

The reachable cases are asset creation and deletion when the target exists or is referenced:

- **`asset.exists {assetPath}` before `create_*`** is a registry probe and does not load the asset.
- **Update in place rather than recreate.** Clearing/rebuilding a referenced material graph is quiet; delete-and-recreate raises a dialog and then fails. `asset.duplicate` likewise warns for a referenced source instead of prompting.
- `asset.delete {paths}` succeeds only for an **unreferenced** asset; a referenced one is refused with `ASSET_IN_USE` and left untouched. Delete the actors using a mesh first, then delete the mesh. `force: true` exists but nulls every in-memory reference irreversibly and can still leave the file behind - read the `asset.delete` page before reaching for it. `EditorAssetLibrary.delete_asset` from `python.execute` is permission-blocked; use `asset.delete`.

## Saving

- `level.save {}` saves the active persistent level. **Handle both response shapes**: streaming-capable clients may get `{saved: true, levelPath}` inline; plain JSON and explicit `args: {wait: false}` get the ticketed async `{status: "running", ticket_id}`, whose package write lands on a later pump. Poll `system.job_status {ticket_id}` to terminal status before treating the map as on disk. See [`level`](level.md).
- `asset.save {assetPath}` persists one package; a throttled write returns `saved: false` plus `pendingFlush`, which `editor.save_all` flushes. `editor.save_all {}` returns `savedCount`, `totalDirty`, and `failedAssets`; `editor.list_dirty_packages` lists the rest.
- **Landscape sculpting and some Python edits may not mark a package dirty, so save can silently no-op.** On UE 5.8 the engine `mark_package_dirty()` / `Package.set_dirty_flag()` calls are not exposed to Python. Use:

  ```js
  call("asset.mark_dirty", { assetPath: "/Game/Maps/MyMap" })   // -> {package, wasDirty, isDirty}
  call("asset.is_dirty",   { assetPath: "/Game/Maps/MyMap" })
  ```

  ```python
  unreal.PinWrightPackageLibrary.mark_package_dirty_by_path('/Game/Maps/MyMap')  # -> bool
  unreal.PinWrightPackageLibrary.mark_actor_package_dirty(actor)
  unreal.PinWrightPackageLibrary.is_package_dirty_by_path('/Game/Maps/MyMap')
  unreal.PinWrightPackageLibrary.describe_mark_dirty_blocker(obj)   # "" means allowed
  ```

  Both routes report the package readback, not the engine return (which can be `true` while doing nothing for transient/package-less objects). Refusal is `false` plus `MARK_DIRTY_REFUSED` over RPC. The by-path form finds a **loaded** package only; actor edits may touch the actor package rather than the map, so read `get_package_name`.
- To force bytes to disk, `unreal.EditorAssetLibrary.save_asset('<package path>', only_if_is_dirty=False)` writes regardless of the flag. Re-read after either route.

See [`safe-mutation-save`](safe-mutation-save.md) for the full read-edit-verify-save contract.


## See also

- [`level-building`](level-building.md) for the rest of the build workflow.
- [`safe-mutation-save`](safe-mutation-save.md) for the full read-edit-verify-save contract.
- [`unattended`](unattended.md) for running the editor without a human to click dialogs.
- [`level-review`](level-review.md) for re-verifying downstream layers after a rebuild.
