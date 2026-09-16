# container.set

Per-element CRUD on a `TSet` UPROPERTY — `add` is idempotent (silent no-op on existing member), and membership is tested by the reflected element type's `==` semantics (`TSet`'s own dedupe rule). Use `call("property.set")` instead when replacing the whole set in one go.

## Finding a TSet target

Every `container.set` verb needs an `objectPath` + `propertyName` pointing at a reflected `TSet` UPROPERTY, but `TSet` properties are rare on placed level actors — so the hard part is finding one. For the general `property.list` -> `cppType` scan recipe (and the `nameMatch`/no-container-kind-filter caveat) see [`container`](container.md) > Cross-cluster overlap > Finding a container target; for a guaranteed target use the copy-paste CDO below.

- **Copy-paste target (always present):** `/Script/Engine.Default__AssetManagerSettings` exposes `MetaDataTagsForAssetRegistry`, a `TSet<FName>` (`config, EditAnywhere`) on the AssetManagerSettings CDO in every project. Use `objectPath:"/Script/Engine.Default__AssetManagerSettings"`, `propertyName:"MetaDataTagsForAssetRegistry"` as a known-good starting target.
