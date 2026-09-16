# world_partition

Operate on World Partition editor systems for partitioned maps — cell loading, Data Layer creation/assignment, cleanup of invalid Data Layers, HLOD-related organization, and partitioned level state. Use this for World Partition state and Data Layer workflows; use `call("level.structure")` for detailed HLOD layer setup and `call("level")` / `call("actor")` for ordinary non-partition level edits.

## Mutation readback

`world_partition.set_datalayer` captures the engine mutation result and reads the
resolved actor with `AActor::ContainsDataLayer` before and after the call. Its
response reports `outcome` as `added`, `already_present`, or `rejected`, along
with `added`, `alreadyPresent`, `engineChanged`, and `verified`. Only an
engine-reported change from absent to present succeeds. An already-present
membership returns `DATALAYER_ALREADY_ASSIGNED`; any other engine-reported
no-change or unverified postcondition returns `VERIFICATION_FAILED`. Both
errors retain the actor verification payload.

`world_partition.cleanup_invalid_datalayers` checks `CanBeRemoved()` before
deleting each missing-asset instance. After each delete it re-enumerates the
world's data-layer manager by the instance path. The response always includes
`deleted[]`, `failed[]`, `candidateCount`, `deletedCount`, and `failedCount`.
A scan with no invalid instances returns `NO_INVALID_DATALAYERS`. Verified
failures return `DELETE_FAILED` when none were deleted or `DELETE_PARTIAL`
when cleanup made mixed progress.
