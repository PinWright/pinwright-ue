# container

Per-element CRUD on `TArray`, `TMap`, and `TSet` UPROPERTYs of any UObject — the reflected per-entry verbs that `property.set` whole-container replacement can't express efficiently. Pure branch with no methods of its own; reach into `call("container.array")`, `call("container.map")`, or `call("container.set")` for the actual operations.

## Cross-cluster overlap

- **Whole-container replace** — use `call("property.set")` with the full new value. The `container.*` verbs are for incremental edits where re-sending the whole container is wasteful.
- **Iterate before edit** — `call("container.map.get_keys")` returns the key list; for arrays use a `call("property.get")` read of the whole array first if length matters.
- **Existence check** — prefer `container.map.has_key` and `container.set.contains` over wrapping `get` in error handling.
- **Finding a container target** — every `container.*` verb needs an `objectPath` + `propertyName` pointing at a reflected container UPROPERTY. To discover one, `call("property.list", {objectPath, includeAll:true})` and scan the result for an entry whose `cppType` begins `TArray`/`TSet`/`TMap` (`property.list` emits a `cppType` per property). The `property.list` `nameMatch` filter narrows by property *name* substring only — there is **no** `cppType`/container-kind filter, so you must scan the listing yourself rather than ask for "only the arrays/sets/maps". Each child overlay (`container.array`/`container.set`/`container.map`) names an always-present copy-paste CDO target to start from.
- **Mutation policy** — for choosing between whole-property replacement and per-entry edits, see [`safe-mutation-save`](safe-mutation-save.md).

## Value validation happens before mutation

Values for `container.array.set`, `container.map.set`, and `container.set.add` use the
same strict bool/number/integer conversion as [`property.set`](property.set.md). Array
elements are copied to scratch storage before the container is modified, so conversion
of any element type is atomic: a malformed nested field cannot partially update an
existing struct element. Numbers must be finite, integers must be integral and within
the reflected type's signed or unsigned range, and malformed strings are never coerced
to zero or a truncated value. Integer map keys and set elements use this strict parser
on lookup and mutation paths as well.

Invalid input returns that verb's existing validation error without changing the
container, dirtying its package, or sending an object-change notification. The
notification contract below therefore applies only after conversion succeeds.

## Container mutations notify the owning object

Every mutator in this cluster (`array.append` / `insert` / `set` / `remove` / `clear`,
`map.set` / `remove` / `clear`, `set.add` / `remove` / `clear`) emits a
`PostEditChangeProperty` naming the container property, with the change type UE's own
property editor uses for that operation: `ArrayAdd` for an add or insert (maps and sets
included), `ArrayRemove` for a removal, `ArrayClear` for a clear, and `ValueSet` for
`container.array.set`, which rewrites one element without changing the container's shape.
The edited index is not attached to the event — `FPropertyChangedEvent` carries it only
through the engine's multi-object `SetArrayIndexPerObject` map, which a single-object verb
has nothing truthful to fill in — so an override that needs the index re-reads the container.
