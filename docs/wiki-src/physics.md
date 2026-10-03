# physics

Configure physics assets and ragdoll behavior, including PhysicsAsset creation or assignment for skeletal meshes and ragdoll toggling on named actors, plus the project's Physics settings (`UPhysicsSettings`) and its surface-type names.

Use this namespace for physics simulation state on skeletal meshes or placed actors; use `skeleton` for lower-level skeleton, bone, constraint, or retarget data authoring.

`physics.setup_physics_simulation` returns `PIE_ACTIVE` before creating a physics asset or folder while Play In Editor is active. Stop PIE and retry; the mesh and skeleton lookups remain read-only and PIE-safe.

## Project settings and surface types

`physics.get_project_settings`, `physics.set_project_settings` and `physics.set_surface_types` are the typed surface for `UPhysicsSettings` (Project Settings -> Engine -> Physics), persisted to `Config/DefaultEngine.ini` under `[/Script/Engine.PhysicsSettings]`; they mirror [`rendering`](rendering.md)'s pair one class over. Use them instead of `property.set` on `/Script/Engine.Default__PhysicsSettings`: that write is live-only (a `/Script/` CDO has no package to save, so `existsOnDisk:false` means the change is lost on restart), and it bypasses the surface-table validation below.

**Every writer reports the target and measures durability.** Responses always carry `configFile` and `configSection`. With `save:true` (default) the write goes through `TryUpdateDefaultConfigFile` (the Project Settings path, which rewrites the whole `[/Script/Engine.PhysicsSettings]` section with every property that differs from the base layer), then each written property is cleared in memory and re-imported from the reloaded config: `reloadVerified[]` / `reloadMismatch[]` name the outcome and `savedTo` appears only when every property came back with the requested value. A read-only ini, or a value a later layer overrides, returns `SAVE_FAILED` with the measured fields (`saveRequested`, `saved:false`, `saveState:"failed"`, `saveDetail`); the in-memory change stays applied. `save:false` reports `saveState:"notRequested"` and no `savedTo`.

**Surface types gate a whole enum.** `PhysicalSurfaces` is what names `EPhysicalSurface`'s `SurfaceType1..62`; an unnamed index is `Unused` and hidden in every dropdown. UE4-era `[PhysicalMaterial.SurfaceTypes]` sections (`SurfaceType1=Wood`) are **not read** by UE 5 — `physics.get_project_settings`'s `surfaceTypes[]` is the live table, so an empty one beside such a section means those names are dead. The Python wrapper `unreal.PhysicalSurface` is generated once at editor startup, so names registered live are not visible to Python until a restart (see [`python`](python.md)); `property.get` / `property.set` with the string `"SurfaceType3"` work immediately.

### physics.setup_physics_simulation

**`physicsAssetName` is a BARE asset name, never a path.** It is validated against the engine's own object-naming rules (`FName::IsValidXName` / `INVALID_OBJECTNAME_CHARACTERS`) and the composed package path against `FPackageName::IsValidLongPackageName`, so a `/`, `\`, `.`, `..`, a leading or trailing slash, a space or an unmounted root is refused `INVALID_ARGUMENT` with the engine's own reason text quoted. This site was the most reachable of the sweep: name and folder were joined with `FString::Printf(TEXT("%s/%s"), ...)`, which doubles the separator **unconditionally** (unlike `FString::operator/`), so a *rooted* name composed `/Game/Physics//Game/...` — and `CreatePackage` logs a double slash at **Fatal**, which is not compiled out in any configuration. The call did not fail; the editor **process** died, taking every unsaved package in it. The same defect was measured end-to-end on `foliage.add_type`; see that verb on the [`foliage`](foliage.md) page.

**`savePath` carries the same hazard as the name, and a bare name does not save you.** A folder containing `//` is not caught by `savePath`'s own check: `IsValidLongPackageName` rejects it, but the fallback `TryConvertFilenameToLongPackageName` returns the string verbatim (`FPaths::NormalizeFilename` runs with `bRemoveDuplicateSlashes = false`) and the result is not re-checked, so `/Game//Physics` survives. What refuses it is the validation of the **composed** path — which is also why omitting `physicsAssetName` entirely does not make a bad folder safe: the derived `<MeshName>_Physics` default takes the same route.

**`savePath` (optional, default `/Game/Physics`) is how you choose the folder.** It is checked before the mesh is resolved, and both it and the name are refused ahead of any asset load — so a call with a bad name is refused `INVALID_ARGUMENT`, not `ASSET_NOT_FOUND`. The effective folder is always echoed as `savePath`, on the "already exists" branch too.

Omit `physicsAssetName` and the destination is `<MeshName>_Physics` under `savePath`. An existing asset at the composed path is returned as-is (`existingAsset: true`) rather than overwritten.

For a new asset, `save` defaults to `true` and writes the PhysicsAsset to disk through the measured save path. `save:false` leaves the new asset registered and dirty for a later flush. The response includes the standard `saveRequested`, `saved`, `savedToDisk`, `pendingFlush`, `saveState`, `saveDetail`, and `sizeBytes` fields; `saved:true` means the `.uasset` was verified on disk. When `assignToMesh:true`, `skeletalMeshSave` contains the same report for the changed SkeletalMesh package.

### physics.get_project_settings

Enumerates every `CPF_Config | CPF_GlobalConfig` UPROPERTY on `UPhysicsSettings` (optional case-insensitive `filter` on the name). Response: `{ settings: {<UPROPERTYName>: <json>}, surfaceTypes: [{index, name}], configFile, configSection }`. `surfaceTypes` is the live `PhysicalSurfaces` table in `{index, name}` form. Read-only.

### physics.set_project_settings

`{ updates: {name: value}, save?: true }`. Each applied property gets `PostEditChangeProperty` (where `UPhysicsSettings` exports CVars, rebuilds physical materials and refreshes Chaos settings; open Project Settings tabs refresh too). Rejections are data, not errors: `unknown_property`, `not_config_serializable`, an import error message, and `use_physics.set_surface_types` for `PhysicalSurfaces`. Response: `{ applied[], rejected[{name, reason}], configFile, configSection, saveRequested, saved, saveState, saveDetail, reloadVerified[], reloadMismatch[], savedTo? }` — see **Project settings and surface types** above for the save contract.

### physics.set_surface_types

`{ surfaces: [{index, name}], replace?: false, save?: true }`. `surfaces` is a closed slot: any element key other than `index` / `name` is refused `UNKNOWN_NESTED_PARAMS`.

- `index` must be an integer in **1..62** (`SurfaceType_Default` = 0 is never renamed); `name` must be non-empty; an index listed twice is refused. All three are `INVALID_ARGUMENT`.
- `replace:false` upserts by index: each listed index ends up with exactly the one requested entry (in place of its first old entry, else appended), and every entry of an unlisted index is kept verbatim, including the repeated-index entries a hand-edited ini can carry. `replace:true` makes the table exactly `surfaces`.
- Each requested name must differ, case-insensitively (the engine's own Project Settings check), from the effective name of every other index (the last entry per index, as `LoadSurfaceType` applies them): `DUPLICATE_NAME` with `{name, indices}`. A duplicate the table already carried between two unlisted indices is left alone.
- Removing an index (only `replace:true` can) that any `UPhysicalMaterial` asset's `SurfaceType` uses is refused `SURFACE_TYPE_IN_USE` with `inUse[{index, physicalMaterials[]}]`; nothing changes. The check loads every registered physical material, plus in-memory unsaved ones.
- On success `LoadSurfaceType` refreshes `EPhysicalSurface`'s `DisplayName` / `Hidden` metadata, and removed indices are hidden again. `enumRefreshed` is measured from that metadata, not assumed.

Response: `{ registered[{index, name}], previous[{index, name}], removed[int], enumRefreshed, ...save fields }`. `previous` is the pre-call table verbatim, so a caller can put it back with `replace:true` unless it repeats an index (refused `INVALID_ARGUMENT`; restore such a table from source control, the ini as committed).
