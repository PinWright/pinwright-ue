# container.array

Per-element CRUD on a `TArray` UPROPERTY of any UObject: length-changing ops (`append`, `insert`, `remove`, `clear`) shift element memory; index-only ops (`get`, `set`) keep length stable. Use `container.array.append` rather than `insert(index=Length)` for a push, and `call("property.set")` to replace the whole array.

## Finding a TArray target

Every `container.array` verb needs an `objectPath` + `propertyName` for a reflected `TArray` UPROPERTY. Use the copy-paste CDO below for a guaranteed target; for the general `property.list` -> `cppType` scan (including the `nameMatch`/no-container-kind-filter caveat), see [`container`](container.md) > Cross-cluster overlap > Finding a container target.

- **Copy-paste target (always present):** `/Script/Engine.Default__AssetManagerSettings` exposes `DirectoriesToExclude`, a `config, EditAnywhere TArray<FDirectoryPath>` on the AssetManagerSettings CDO in every project. Use `objectPath:"/Script/Engine.Default__AssetManagerSettings"`, `propertyName:"DirectoriesToExclude"` as a known-good starting target.
