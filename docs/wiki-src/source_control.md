# source_control

Query and mutate the editor's source-control provider state — read the active provider, list file status, view log/history, revert local changes, and mark new files for add. Use this for source-control workflow inside the editor; the actual commit / submit / push step still happens through the underlying VCS client.

## Reading status flags (provider-aware)

`source_control.status` returns per-file booleans. Their meaning is uniform across providers, but two are easy to misread under **Git**, which is lockless:

- `isCheckedOut` — for Git this is `true` for *every tracked file*, clean or dirty (Git has no per-file lock/checkout, so "tracked and editable" reads as checked-out). Do **not** treat `isCheckedOut:true` as "I hold a pending checkout" or as a dirty signal on Git; under Perforce it does mean an explicit open-for-edit.
- `isUnchanged` — true iff the file is under source control and has **no** pending add / delete / modify. It intentionally does not fold in `isCheckedOut`, so a pristine tracked git file correctly reports `isUnchanged:true`. A file that is not under source control reports `isUnchanged:false` (it is not "unchanged from the depot" — it is not in the depot).
- `isModified` is the reliable "this file is dirty" signal across providers.

## Revert resynchronizes the loaded packages, and says whether it managed to

`source_control.revert` does not just hand the filenames to the provider. The provider's
`FRevert` runs *inside* the engine's own revert-and-reload path
(`USourceControlHelpers::ApplyOperationAndReloadPackages`, the same one the Content Browser's
revert uses), so every currently-resident target is unlinked before the revert and re-read from
the reverted bytes afterwards. Without that step the file changed on disk while the loaded
`UPackage` kept the pre-revert values: readbacks reported the un-reverted state even though
`source_control.status` reported the file clean, and the next save of that package wrote the
stale state back out — a silent un-revert with a success receipt.

Read the measured fields, not just `success`:

- `count` is what you **requested**. `loadedCount`, `reloadedCount`, `removedCount` and
  `staleCount` are **measured** off the packages after the revert, and `files[]` carries the
  same per-path measurement (`wasLoaded`, `reloaded`, `removed`, `stale`).
- `resynchronized:false` plus `resyncWarning` is the field that matters: at least one package
  was reverted on disk but kept its pre-revert in-memory state. **Do not save it** — run
  `asset.reload` on each path flagged `stale` first.
- `removed:true` is a revert of an add: the file went away and the package went with it.

Side effects of the reload, which are the engine's and not optional: open asset editors for a
reloaded asset are closed and reopened on the new object, editor selection sets are cleared,
and every `UObject*` into a reloaded package is invalidated — re-resolve by path after a revert
rather than reusing a handle taken before it.

**Loaded maps are refused, not silently half-reverted.**

A loaded map package, or a loaded external (one-file-per-actor / external object) package whose
world is loaded, cannot be reloaded without tearing down the live world. Reverting it would
leave the world holding the old state with no way back, so the whole batch is refused with
`REVERT_REQUIRES_UNLOADED_PACKAGE` and **nothing is reverted**. `blockedPackages` names them.
Unload them first (`editor.open_level` onto another map for the active level) and retry.

**Known gap: `asset.save` does not check the disk.**

`asset.save` still writes the in-memory package unconditionally. It does not detect that the
file on disk changed under it since the package was loaded, so an out-of-band change to the
working tree — a `git checkout`, an external revert, another tool's write — is still silently
overwritten by the next save of a resident package. Reverting through `source_control.revert`
is safe (it resynchronizes); reverting behind the editor's back is not. Run `asset.reload`
after any out-of-band change to a loaded asset.
