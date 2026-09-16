# physics

Configure physics assets and ragdoll behavior, including PhysicsAsset creation or assignment for skeletal meshes and ragdoll toggling on named actors.

Use this namespace for physics simulation state on skeletal meshes or placed actors; use `skeleton` for lower-level skeleton, bone, constraint, or retarget data authoring.

`physics.setup_physics_simulation` returns `PIE_ACTIVE` before creating a physics asset or folder while Play In Editor is active. Stop PIE and retry; the mesh and skeleton lookups remain read-only and PIE-safe.

### physics.setup_physics_simulation

**`physicsAssetName` is a BARE asset name, never a path.** It is validated against the engine's own object-naming rules (`FName::IsValidXName` / `INVALID_OBJECTNAME_CHARACTERS`) and the composed package path against `FPackageName::IsValidLongPackageName`, so a `/`, `\`, `.`, `..`, a leading or trailing slash, a space or an unmounted root is refused `INVALID_ARGUMENT` with the engine's own reason text quoted. This site was the most reachable of the sweep: name and folder were joined with `FString::Printf(TEXT("%s/%s"), ...)`, which doubles the separator **unconditionally** (unlike `FString::operator/`), so a *rooted* name composed `/Game/Physics//Game/...` — and `CreatePackage` logs a double slash at **Fatal**, which is not compiled out in any configuration. The call did not fail; the editor **process** died, taking every unsaved package in it. The same defect was measured end-to-end on `foliage.add_type`; see that verb on the [`foliage`](foliage.md) page.

**`savePath` carries the same hazard as the name, and a bare name does not save you.** A folder containing `//` is not caught by `savePath`'s own check: `IsValidLongPackageName` rejects it, but the fallback `TryConvertFilenameToLongPackageName` returns the string verbatim (`FPaths::NormalizeFilename` runs with `bRemoveDuplicateSlashes = false`) and the result is not re-checked, so `/Game//Physics` survives. What refuses it is the validation of the **composed** path — which is also why omitting `physicsAssetName` entirely does not make a bad folder safe: the derived `<MeshName>_Physics` default takes the same route.

**`savePath` (optional, default `/Game/Physics`) is how you choose the folder.** It is checked before the mesh is resolved, and both it and the name are refused ahead of any asset load — so a call with a bad name is refused `INVALID_ARGUMENT`, not `ASSET_NOT_FOUND`. The effective folder is always echoed as `savePath`, on the "already exists" branch too.

Omit `physicsAssetName` and the destination is `<MeshName>_Physics` under `savePath`. An existing asset at the composed path is returned as-is (`existingAsset: true`) rather than overwritten.

For a new asset, `save` defaults to `true` and writes the PhysicsAsset to disk through the measured save path. `save:false` leaves the new asset registered and dirty for a later flush. The response includes the standard `saveRequested`, `saved`, `savedToDisk`, `pendingFlush`, `saveState`, `saveDetail`, and `sizeBytes` fields; `saved:true` means the `.uasset` was verified on disk. When `assignToMesh:true`, `skeletalMeshSave` contains the same report for the changed SkeletalMesh package.
