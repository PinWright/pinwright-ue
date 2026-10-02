# pose_search

Author `UPoseSearchSchema` and `UPoseSearchDatabase` assets used by Motion Matching AnimGraph nodes. Use this namespace for Pose Search authoring; reach for `call("animation.authoring")` for AnimBlueprint node placement and other non-Pose-Search animation asset work.

## Availability

`pose_search.*` requires the **PoseSearch** engine plugin. The integration auto-loads when that plugin is enabled in the host project; when it is disabled, these methods are unregistered and calling one returns `PLUGIN_DISABLED` (enable the PoseSearch plugin and restart the editor). Keep this namespace on public Pose Search APIs — schema creation adds skeletons and channels, then relies on the normal editor change/save path to make `GetChannels()` reflect authored channels; do not call private engine methods such as `UPoseSearchSchema::Finalize()`.

## Pipeline

The supported pipeline is schema first, database second:

1. Create a `UPoseSearchSchema` with `pose_search.create_schema`.
2. Add channels. Any concrete `UPoseSearchFeatureChannel` class is accepted (see [Channels](#channels)); a usable Motion Matching schema normally has at least `Trajectory` plus `Position`/`Velocity` or a `Pose` channel.
3. Create a `UPoseSearchDatabase` with `pose_search.create_database`.
4. Add sequence entries with `pose_search.add_database_animation`, or pass sequence entries through `animations` during database creation.

Example:

```
call("pose_search.create_schema", {
  "assetPath": "/Game/Animation/PS_Schema",
  "skeleton": "/Game/Characters/Heroes/Mannequin/Meshes/SK_Mannequin_Skeleton.SK_Mannequin_Skeleton",
  "channels": [
    { "type": "Trajectory" },
    { "type": "Velocity", "bone": "foot_l", "weight": 1.0 },
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

## Channels

Each `channels[]` entry is a kind string (`"Trajectory"`) or an object `{ "type" | "kind": <kind>, <setting>: <value>, ... }`; a missing kind means `Position`. The kind is any concrete `UPoseSearchFeatureChannel` subclass loaded in the editor, matched by its class name without the `PoseSearchFeatureChannel_` prefix, ignoring case and `_`/`-`/spaces (`Trajectory`, `Velocity`, `Heading`, `Pose`, `Position`, `Phase`, `Curve`, `Distance`, `Padding`, `SamplingTime`, `TimeToEvent`, ... on UE 5.8). An unknown kind returns `UNSUPPORTED_CHANNEL` listing the kinds this engine has.

Every other key names an editable property of that channel class (case-insensitive: `bone`, `originBone`, `sampleTimeOffset`, `weight`, `headingAxis`, `samples`, `sampledBones`, ...) and is set by reflection, so its value takes the property's own shape: numbers, enum names, objects for structs, arrays of objects for struct arrays. A bone-reference property also takes a bare bone name (`"bone": "root"`); nested ones use `{ "boneName": "root" }`. Bitmask fields such as `Trajectory.samples[].flags` and `Pose.sampledBones[].flags` are integers (the engine's `EPoseSearchTrajectoryFlags` / `EPoseSearchBoneFlags` bits). `boneName` / `originBoneName` stay as aliases. An unknown key or a value that does not fit returns `INVALID_ARGUMENT` naming the settable properties; nothing is created. Unset properties keep the engine defaults — a bare `Trajectory` gets UE's default locomotion samples.

```
{ "type": "Trajectory", "samples": [ { "offset": -0.4, "flags": 32 }, { "offset": 0.5, "flags": 48 } ] }
{ "type": "Pose", "sampledBones": [ { "reference": { "boneName": "foot_l" }, "flags": 3 } ] }
{ "type": "Heading", "bone": "pelvis", "headingAxis": "Y" }
```

The response's `channels[]` echoes each created channel in request order as `{ kind, className, settings }`, where `settings` holds every editable property's resolved value. `channelCount` is the schema's finalized channel count, which can exceed the request when the engine injects dependent channels. Group channels (`Group`) are created, but their instanced `subChannels` cannot be authored through this verb.

Channels are reached through reflection rather than their C++ headers, which are Private on UE 5.3-5.5 and Public from 5.6, so the namespace compiles in on every supported engine.

## Validation

Database authoring validates animation entries before mutation. `pose_search.create_database` prevalidates every supplied `animations` entry against the schema before creating the database asset, so a bad entry cannot leave a half-created database behind. `pose_search.add_database_animation` validates skeleton/schema compatibility before `UPoseSearchDatabase::AddAnimationAsset`; mismatches return `SKELETON_MISMATCH` and leave the database animation list unchanged.

Implementation code should reuse the shared path/load/save helpers around this flow (`BuildCreatePaths`, `LoadTypedAsset`, `McpSafeAssetSave`) instead of reimplementing package normalization or persistence rules locally.

## See also

- [`animation.authoring`](animation.authoring.md) for AnimBlueprint node authoring. Motion Matching graph nodes should go through `animation.authoring.add_graph_node`.
- [`asset`](asset.md) for dump and verification workflows around created assets.

### pose_search.create_schema

Create a `UPoseSearchSchema`, add the target Skeleton through `UPoseSearchSchema::AddSkeleton`, append feature channels of any kind (see [Channels](#channels)), and use the normal editor change/save path so `GetChannels()` reflects authored channels. Every channel spec is validated before the schema asset is created, so a refused spec leaves no schema behind.

### pose_search.create_database

Create a `UPoseSearchDatabase`, assign its `Schema`, and optionally append sequence entries from `animations`.

### pose_search.add_database_animation

Append a sequence-backed `FPoseSearchDatabaseSequence` entry to an existing database through `UPoseSearchDatabase::AddAnimationAsset(FInstancedStruct::Make(...))`.
