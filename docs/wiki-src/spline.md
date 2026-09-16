# spline

Create and edit spline actors, spline components, spline points, spline mesh components, and mesh scattering along spline paths in the active level.

Use this namespace when the editable shape is a spline or spline-driven mesh layout; use `call("geometry")` for direct mesh topology changes after a shape has been generated.

## Availability

`spline.*` does NOT require the GeometryScripting plugin (unlike `geometry.*`); it lives in the always-loaded core module, so these methods are available on every install with no plugin gating.

## Verifying a scatter

`spline.get_splines_info` reads **curve geometry only**; it does **not** enumerate the plain
`UStaticMeshComponent`s that `spline.scatter_meshes_along_spline` attaches. A scatter-then-readback
loop therefore confirms the curve, not the instances.

Confirm a scatter with its `meshesCreated` count, or use
[`actor.get_components`](actor.get_components.md) on the spline actor and count
`StaticMeshComponent` entries. `get_splines_info` returning zero scatter rows is an absence, not a
wrong value, and cannot confirm one.

## Derived point scale (water splines)

`spline.set_spline_point_scale` **refuses** with `DERIVED_PROPERTY` when
`AllowsSplinePointScaleEditing()` is false. The engine sets that flag when point scale is a *cache
of other state*, not the authority.

The shipped case is `UWaterSplineComponent`: `SynchronizeWaterProperties` assigns `Scale.X` from
`UWaterSplineMetadata::RiverWidth` and `Scale.Y` from `Depth`, and `PostLoad` calls it. A `Scale`
write can read back at the requested number and save, then be recomputed on the next level load
*and* on `actor.duplicate`; one session lost a committed river width this way.

Use [`water.set_river_width_at_spline_point`](water.set_river_width_at_spline_point.md) and
`water.set_river_depth_at_spline_point` instead — they write the metadata the engine derives
from, then re-derive the scale, and report both numbers so you can see they agree.

The error payload carries a `derivedWrite` block naming the property, the state it is derived
from, and the verb to use, so the recovery path is machine-readable rather than prose.

Position, rotation, tangents and point type are **not** derived on water splines and continue
to work through this namespace.

### spline.get_splines_info

Returns **spline geometry only**. A named actor with `USplineComponent` emits `actorName`,
`pointCount`, `splineLength`, `closedLoop`, and `points[]` entries of `{index, location (local),
type}`; the no-arg list emits `actorName`, `splineComponentCount`, `pointCount`, and
`splineLength` per actor. A `spline.create_spline_mesh_actor` result has `USplineMeshComponent`
but no `USplineComponent`, so it reports `isSplineMesh` and the spline-mesh fields instead.

It never lists the plain `UStaticMeshComponent`s attached by
`spline.scatter_meshes_along_spline`—there is no scattered-mesh count, mesh path, or transform.
Confirm a scatter from that call's `meshesCreated`, or inspect per-instance components with
`actor.get_components`.

### spline.scatter_meshes_along_spline

Builds `floor(splineLength / spacing) + 1` separate `UStaticMeshComponent`s, attaches them to the
actor's `USplineComponent`, and returns `{meshesCreated, splineLength, spacing, meshComponents,
meshComponentsTruncated}` plus `actorPath` / `actorGuid` / `existsAfter`. Component names are
capped at 64; `meshComponentsTruncated:true` reports the cap.

**Verify-after-mutate:** use `meshesCreated` for the count. For per-instance transforms, call
`actor.get_components` on the spline actor, optionally with
`componentClass: "StaticMeshComponent"`. `spline.get_splines_info` reports curve geometry only.

### spline.set_spline_point_scale

Writes one Scale-curve point and returns the scale **read back from the component**, not the
request. It refuses with `DERIVED_PROPERTY` when
`USplineComponent::AllowsSplinePointScaleEditing()` is false; nothing is written. The
`derivedWrite` block gives `{property, derivedFrom, authoritativeVerb, survivesReload:false,
explanation}`, including the replacement verb.
