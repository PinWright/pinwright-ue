# blueprint.scs

Edit a Blueprint class's Simple Construction Script — the component template tree that gets instanced on every newly spawned actor, mirroring the "Components" panel in the Blueprint editor. Use this for class-level template defaults that survive a recompile; for per-instance property tweaks on an already-placed actor, use `call("actor.set_component_properties")` instead.

## Cross-cluster overlap

The hard distinction: components added here are *templates on the class*, so every spawned instance gets them and they survive a recompile. Components added with `call("actor.add_component")` are *instance-only* — bound to one placed actor and lost on reconstruction. Picking the wrong side is the most common failure mode in BP automation; the wiki repeats this in `call("blueprint")` and `call("actor")` because it's that load-bearing.

Inherited Blueprint SCS components need one extra rule: validate the requested property against the inherited parent template first, then create or fetch the child Blueprint's override template through `UInheritableComponentHandler` only after validation succeeds. The override is keyed by `FComponentKey`. Do not mutate the parent `USCS_Node` template, and do not create the child override during lookup for an invalid request; either bug can make the RPC report success while spawned child instances keep the parent value, or dirty the child Blueprint on a rejected call.

For cached audits, [`asset.dump`](asset.dump.md) / [`asset.dump_folder`](asset.dump_folder.md) writes `scs.json` in the `blueprint.scs.get` shape plus dump-only `scs.txt` (`component(Name) { ... }` with nested `children {}`). Use `bpir.txt` / [`blueprint.decompile`](blueprint.decompile.md) for graph logic, or [`blueprint.inspect`](blueprint.inspect.md) for both live surfaces. Placed actors are different: `call("actor.describe")` or `actors/*.json` reports instance components, attachments, and sparse overrides.

`blueprint.scs.get` can narrow large trees with case-insensitive substring `nameMatch` / `name_match` or `componentClass` / `component_class` (the class and its subclasses). Filtered results may retain `parent` references to omitted components; use an unfiltered read for a complete hierarchy.

SCS is not the Widget Blueprint tree. For UMG widget variables, widget hierarchy XML, or `widget_event` binding workflows, use [`widget`](widget.md) plus `blueprint.compile_bpir`; for actor component templates, stay here.

Every SCS mutator that compiles (`add_component`, `remove_component`, `reparent_component`, `set_transform`, `set_property`, and `duplicate_component`) first checks for live instances of the Blueprint class. It refuses with `LIVE_INSTANCES_WOULD_BE_REINSTANCED` unless `allowReinstancing:true` explicitly accepts destroying and re-creating those instances. An accepted compile reports `compiled`, `status`, `compileErrors`, and `compileWarnings`; if it rebuilt live instances, it also reports the affected count and owning worlds in `reinstanced`, which is absent otherwise. These verbs are also dispatched from the between-frame safe point.

## No native root: UE promotes the first scene component and reparents the rest

For a Blueprint with **no inherited/native root** (a from-scratch `Actor` parent, no `RootComponent`), "attach to root" does not create independent roots. UE's scene-root validation (`USimpleConstructionScript::ValidateSceneRootNodes`, during compile) **promotes the first scene component to `RootComponent`** and **silently re-parents later root-level scene components under it**: adding `BeaconBase` then `DetectionZone` with an empty parent makes `DetectionZone` a child of `BeaconBase`.

Two consequences worth knowing before you diagnose a tree as wrong:
- `blueprint.scs.add_component` reports `"parent": "(root)"` for an empty `parentComponentName` even when UE nested the node; read the *compiled* tree with `blueprint.scs.get` and treat that nesting as engine behavior.
- `blueprint.scs.reparent_component` to an empty parent is typically a **no-op**: the next compile re-promotes the first scene component, so a later node returns under it.

## See also

- [`blueprint`](blueprint.md) for the parent authoring namespace and BPIR workflows.
- [`property`](property.md) for generic UObject property reads and writes.
- [`asset`](asset.md) for cached `scs.json` and `scs.txt` component-template dumps.

### blueprint.scs.add_component

For an empty `parentComponentName`, the success payload always reports `"parent": "(root)"` even when `ValidateSceneRootNodes` made the first scene component the `RootComponent` and nested this node beneath it. Read the real hierarchy with `blueprint.scs.get`; see the namespace note for why.

`materialPath` and `meshPath` resolve **before node creation**, so a rejected path adds no node and does not compile or save. Typed failures are:

- path outside a project asset root or containing traversal → `SECURITY_VIOLATION`;
- path that loads nothing → `MATERIAL_NOT_FOUND` / `MESH_NOT_FOUND`;
- incompatible `componentClass` → `INVALID_PARAMS`. `materialPath` needs a `UPrimitiveComponent` subclass; `meshPath` needs a `StaticMeshComponent` or `SkeletalMeshComponent` subclass. Omit the asset path for other classes.

This matches [`actor.spawn`](actor.spawn.md)'s resolve-then-mutate rule. Previously a bad path returned `success: true` with `material_applied: false` and a saved Blueprint edit; calls omitting both paths or passing loadable ones are unchanged.

On success, `material_applied: false` / `mesh_applied: false` means the created template was null or an unexpected type, an engine-side condition visible only after node creation. The response then adds `warnings` naming the dropped slot; it is absent otherwise.

### blueprint.scs.reparent_component

Emptying `newParentName` on such a node is typically a no-op: `ValidateSceneRootNodes` re-promotes the first scene component to `RootComponent`, and the handler short-circuits an already-correct parent with `"Component already under requested parent; no changes made"`. See the namespace note.

### blueprint.scs.set_property

`propertyName` takes a dot-path into nested struct members (`RelativeLocation.X`, `BodyInstance.CollisionEnabled`). One group of those paths is **not** a plain reflection store, and the difference is observable: the four collision fields of `BodyInstance` — `CollisionEnabled`, `CollisionProfileName`, `ObjectType`, and any channel inside `CollisionResponses` — route through `SetCollisionEnabled` / `SetCollisionProfileName` / `SetCollisionObjectType` / `SetCollisionResponseToChannels`, and the response then carries `collisionRouted: true`. Every other sub-field (mass, damping, `bNotifyRigidBodyCollision`, …) keeps the plain store and carries no such flag. Same contract as [`actor.set_component_properties`](actor.set_component_properties.md), so the class-level write and the instance-level write mean the same thing.

Why it cannot be a plain store on a template: instancing a component template duplicates it and routes `ConditionalPostLoad` → `UPrimitiveComponent::PostLoad` → `FBodyInstance::FixupData` → `LoadProfileData` → `UCollisionProfile::ReadConfig` on the copy, which re-derives `CollisionEnabled`, `ObjectType` and the response container **from `CollisionProfileName`, after the archetype's values have been copied in**. The default profile every `UPrimitiveComponent` constructor installs is `BlockAll`, so a stored `CollisionEnabled` reads back correctly off the template and is overwritten with `QueryAndPhysics` on every spawned instance. The engine setters keep the invariant a raw store breaks — they move the profile to `Custom`, the one name `LoadProfileData` will not re-derive from.

An explicit collision write also clears `bUseDefaultCollision` on a `StaticMeshComponent` template, because `UStaticMeshComponent::OnRegister` → `UpdateCollisionFromStaticMesh` otherwise re-reads the whole collision setup off the mesh asset at every registration. This mirrors what the Blueprint editor's details panel does before any explicit collision edit.

Read the result back off a **spawned** actor, not off the template: `blueprint.scs.get` and `asset.dump`'s `scs.json` both report template state, which is the half that was always right.
