# misc

Catch-all for cross-cutting editor primitives that don't justify their own namespace — viewport cameras, playback speed, post-process volumes, replication flags on Blueprint actor CDOs, and a few asset-creation conveniences. If a method here looks like it belongs somewhere else, it usually does; consult the See also block for the typed routes (`editor.create_bookmark`, `property.set`).

## Cross-cluster overlap

`set_replication` sets `bReplicates` / `bReplicateMovement` on an Actor Blueprint's CDO and marks the Blueprint modified (no compile, no save); the returned `replicates` / `replicateMovement` are read back from the CDO, not echoed from the request. A Blueprint whose generated class is not an Actor (Widget, Object, ActorComponent, ...) is refused with `INVALID_BLUEPRINT_CLASS` and left unmodified. It lives here rather than under `blueprint` because it configures runtime replication behavior, not graph or class structure. For the richer typed replication surface — per-property replication, RPC functions, cull distance, net update frequency — use the `networking` namespace.

## See also

- `call("editor.create_bookmark", …)` — canonical viewport bookmark, paired with `editor.jump_to_bookmark`; it stores the live viewport framing.
- `call("property.set", …)` — generic post-spawn setter for any actor property, including the post-process volume's `Settings` struct.
