# pose_search

Author `UPoseSearchSchema` and `UPoseSearchDatabase` assets used by Motion Matching AnimGraph nodes. Use this namespace for Pose Search authoring; reach for `call("animation.authoring")` for AnimBlueprint node placement and other non-Pose-Search animation asset work.

## Availability

`pose_search.*` requires the **PoseSearch** engine plugin. The integration auto-loads when that plugin is enabled in the host project; when it is disabled, these methods are unregistered and calling one returns `PLUGIN_DISABLED` (enable the PoseSearch plugin and restart the editor). Keep this namespace on public Pose Search APIs — schema creation adds skeletons and channels, then relies on the normal editor change/save path to make `GetChannels()` reflect authored channels; do not call private engine methods such as `UPoseSearchSchema::Finalize()`.

## Pipeline

The supported pipeline is schema first, database second:

1. Create a `UPoseSearchSchema` with `pose_search.create_schema`.
2. Add at least one supported channel. The initial handler supports `Position` channels.
3. Create a `UPoseSearchDatabase` with `pose_search.create_database`.
4. Add sequence entries with `pose_search.add_database_animation`, or pass sequence entries through `animations` during database creation.

Example:

```
call("pose_search.create_schema", {
  "assetPath": "/Game/Animation/PS_Schema",
  "skeleton": "/Game/Characters/Heroes/Mannequin/Meshes/SK_Mannequin_Skeleton.SK_Mannequin_Skeleton",
  "channels": [
    { "type": "Position", "bone": "root", "sampleTimeOffset": 0.0 }
  ]
})

call("pose_search.create_database", {
  "assetPath": "/Game/Animation/PS_Locomotion_DB",
  "schema": "/Game/Animation/PS_Schema.PS_Schema"
})

call("pose_search.add_database_animation", {
  "assetPath": "/Game/Animation/PS_Locomotion_DB.PS_Locomotion_DB",
  "sequencePath": "/Game/Animation/AS_Run.AS_Run",
  "samplingRange": { "min": 0.1, "max": 1.2 }
})
```

`samplingRange` accepts `{min,max}`, `{start,end}`, or `[min,max]`. `[0,0]` means the full source sequence, matching UE's `FPoseSearchDatabaseSequence` default.

## Validation

Database authoring validates animation entries before mutation. `pose_search.create_database` prevalidates every supplied `animations` entry against the schema before creating the database asset, so a bad entry cannot leave a half-created database behind. `pose_search.add_database_animation` validates skeleton/schema compatibility before `UPoseSearchDatabase::AddAnimationAsset`; mismatches return `SKELETON_MISMATCH` and leave the database animation list unchanged.

Implementation code should reuse the shared path/load/save helpers around this flow (`BuildCreatePaths`, `LoadTypedAsset`, `McpSafeAssetSave`) instead of reimplementing package normalization or persistence rules locally.

## See also

- [`animation.authoring`](animation.authoring.md) for AnimBlueprint node authoring. Motion Matching graph nodes should go through `animation.authoring.add_graph_node`.
- [`asset`](asset.md) for dump and verification workflows around created assets.

### pose_search.create_schema

Create a `UPoseSearchSchema`, add the target Skeleton through `UPoseSearchSchema::AddSkeleton`, append supported feature channels, and use the normal editor change/save path so `GetChannels()` reflects authored channels.

### pose_search.create_database

Create a `UPoseSearchDatabase`, assign its `Schema`, and optionally append sequence entries from `animations`.

### pose_search.add_database_animation

Append a sequence-backed `FPoseSearchDatabaseSequence` entry to an existing database through `UPoseSearchDatabase::AddAnimationAsset(FInstancedStruct::Make(...))`.
