# container.map

Per-key CRUD on a `TMap` UPROPERTY: `set` is upsert, `get` returns `KEY_NOT_FOUND` when missing, `has_key` checks existence, and `get_keys` returns all keys for iteration. Use `call("property.set")` to replace the whole map.

## Finding a TMap target

Every `container.map` verb needs an `objectPath` + `propertyName` for a reflected `TMap` UPROPERTY. Because `TMap` properties are rare on placed actors, use the copy-paste CDO below or the general `property.list` -> `cppType` scan (including the `nameMatch`/no-container-kind-filter caveat) in [`container`](container.md) > Cross-cluster overlap > Finding a container target.

- **Copy-paste target (always present):** `/Script/Engine.Default__UserInterfaceSettings` exposes `HardwareCursors`, a `config, EditAnywhere TMap<...>` on the UserInterfaceSettings CDO in every project. Use `objectPath:"/Script/Engine.Default__UserInterfaceSettings"`, `propertyName:"HardwareCursors"` as a known-good starting target (note its key is an enum type, so author keys accordingly).
