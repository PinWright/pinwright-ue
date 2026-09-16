# vegetation-authoring.pathways

What each vegetation placement pathway cannot do, and what it will get wrong quietly. The reference
half of [`vegetation-authoring`](vegetation-authoring.md) § *Pick the pathway first* — that page
chooses between them; this one is the per-pathway trap catalogue, so a pathway's limits can be read
before committing a level to it rather than discovered halfway through.

## Landscape Grass Output

**The cull defaults produce a hard ring, and it is not a look bug to shrug at.**
`landscape.create_grass_type` writes `StartCullDistance == EndCullDistance` (both 10000 on one
measured build). Start equal to End means **no fade band**, so the carpet ends on an edge rather
than thinning; and 100 m is shorter than many zones, so the carpet stops inside its own meadow.
Proof that a bare patch is the cull and not a weightmap hole: the same world point read bare with
the camera 12,903 uu away and full carpet at 3,905 uu, same weightmap, only the camera moved.
Stagger the ends across varieties — one build used 200 / 240 / 300 m — which removes the ring and
reaches the far corner.

**Cost scales with the square of the cull distance**, because the build radius *is*
`EndCullDistance`. One retune to those staggered ends cost roughly 4x the instances, order 6.5e5
around the camera. That is usually the right trade (see the cost note on the parent page: distant
grass instances are nearly free), but it is not a small number.

**Every density and cull field has a twin `*Quality` slot.** `GetDensity()` and
`GetEndCullDistance()` read `GrassDensityQuality` / `EndCullDistanceQuality` on hosts with
per-quality-level grass, so writing only the `PerPlatform` slot is a silent no-op there. Write both.
`PerPlatformInt.default` / `PerPlatformFloat.default` are read-only from Python, so a cull or density
change means constructing new struct instances rather than mutating in place.

**Merging varieties has no verb** — read each source asset's `grass_varieties`, append into one
array, write it back and mark the package dirty. The struct copy carries every field, including ones
Python cannot read individually.

**Grass instances are not addressable.** There is no per-blade query, move or removal, and no
console command that rebuilds the maps. `grass.Enable 0` then `grass.Enable 1` is the supported
drop-and-rebuild (about 45 s); `grass.FlushCache` is destructive — see the parent page.

## `foliage.*` Instancing

**`foliage.paint` writes `FRotator::ZeroRotator` and unit scale unconditionally**, ignoring the
foliage type's own `AlignToNormal` / `RandomYaw` / `RandomPitchAngle` — including on a type the verb
itself auto-created with those flags set. Projection is honest (`projected` is on every response and
the unprojected branch warns), but orientation is not, which limits paint to uniform fill.

**Painting with a `surface` runs a full seat solve per instance, synchronously, with no `limit`.**
A few thousand locations in one call holds the game thread. Chunk it.

**Nothing in the namespace is transactional** — no `FScopedTransaction` on paint, add or remove — so
plan recovery around `level.save` and your own undo records, not Ctrl+Z.

**A bare mesh path resolves to one global `Auto_<Mesh>` foliage type**, shared by every caller.
Create your own types instead (`foliage.add_type`, unique prefix); the full hazard is on the parent
page under *Working in a shared level*.

**Check `Content/Foliage/` after a batch.** The auto-created type has been observed present in the
level and absent from disk until an explicit save, and the package/object name pairing has bitten
callers who tried to save it by package path.

## Procedural Foliage Spawner

**Three rules decide whether the output reads as an ecosystem, and each cost a full tuning pass.**

- **`OverlapPriority` ordered by effective radius** (radius x mean scale), descending, or the large
  species is wiped out by the small one — a big-radius seed has more neighbours and so more
  independent chances to be dominated. One stand went 46 trees to 0 before priority was raised. The
  inverse bites too: strictly ordering four tree species by priority let the top one wipe the other
  three (85 / 1 / 2 / 0).
- **Species that must coexist need equal priority *and* equalised effective radii.** When a swap
  changes a species' mean scale, divide its collision and shade radii by the same factor, or the
  swap silently changes the distribution rather than the species.
- **`MaxInitialAge` above 0**, or whole simulation steps are the only thing ageing a seed and every
  instance lands on a four-value scale ladder — `{Min, Min+0.1D, Min+0.2D, Min+0.3D}`.

The rest of what the simulation actually reads:

- **`density` is inert.** It writes `UFoliageType::Density`, the paint-brush density. The simulation
  reads `InitialSeedDensity`, roughly `round(InitialSeedDensity² * TileSize² / 1e6)` seeds per tile.
- **`minScale` / `maxScale` are inert for this pathway.** They write `ScaleX/Y/Z`, read only by
  `GetRandomScale()`, which non-procedural placement calls. Procedural instances take
  `GetScaleForAge()` — i.e. `ProceduralScale`, left at its default. Measured: a 0.25-0.35 request
  read back correctly on the asset and produced instances at exactly `{1.0, 1.2, 1.4, 1.6}`.
- **`alignToNormal` works** on both paths (aligned types measured tilting to 37.7 degrees).
- **`randomYaw` is dropped into `ignoredFields` and it does not matter** — the underlying default is
  already true and the tile simulation reads it, so procedural scatter always gets full 0-360 yaw.
  Only *disabling* yaw randomisation is unreachable.
- **`tileSize` / `numUniqueTiles` work.** The plugin's `tileSize` default is 1000 against the
  engine's own 10000. At the defaults, one measured control strip produced **three distinct yaw
  values and four distinct scales across 71 plants**, with the same three-plant motif recurring in
  four tiles — a plain 10 m lattice from overhead. Raise `tileSize` toward the volume's own extent.
- **Spacing is set by `ShadeRadius`, not `CollisionRadius`**, for types that cannot grow in shade;
  observed spacing runs about twice the sum of the two shade radii (random sequential adsorption,
  not hex packing). Budget for that before asking for a density.
- **Understory clustering at trunk bases is free**: a species allowed both to grow in shade **and**
  to spawn in it is simulated in a second pass seeded from the existing shade-casting instances.
- **`Height` and `GroundSlopeAngle` are placement filters, not simulation filters.** The tile
  simulation is flat and terrain-free, so a species suppresses its competitors across the whole tile
  and only then gets removed by its own height band. Two species cannot be given non-overlapping
  elevation niches by `Height` alone — equalise radii and priority first, then band them.
- **No verb re-simulates or clears an existing volume.** That is `python.execute` into
  `ProceduralFoliageEditorLibrary`; its clear removes exactly one component's instances by
  procedural GUID, which is the clean undo for this pathway.
- **A spawner's foliage types are unreachable from Python** — the type-object structs expose
  nothing — so the only route from a spawner to the types it drives is the `<Spawner>_FT_<index>`
  naming convention. That index is **positional**, so re-running the create verb with reordered
  types rewrites which mesh `_FT_0` means.

## PCG Graph Scatter

The parent page names the three behaviours to design around; here is what each one costs and how to
answer it.

- **Landscape-sampled points inherit the terrain normal AND a terrain-derived yaw.** The engine
  builds each point's transform with the landscape normal as its Z axis and derives the tangent from
  that normal, so a static-mesh spawner fed straight from a surface sampler produces trees that lean
  with the slope *and* whose yaw is a function of the hillside rather than random. Reset it on the
  transform node: absolute rotation on, with an explicit rotation range (a small pitch/roll band and
  a full 0-360 yaw). Leave it additive — absolute rotation **off** — for rocks and groundcover,
  where riding the normal is correct.
- **A weighted mesh selector partitions a fixed point set**, so `instanceCount` cannot see a species
  change: four materially different graph edits (three spawners re-meshed, a whole species dropped,
  weights changed) all returned `26664`, byte-identical. `instancedComponentCount` did move
  (19 → 18 → 16 → 15), but it is a weak signal, not a census. Verify a species change by walking the
  actor's instanced components.
- **A mesh spawner has no per-entry scale.** Each entry carries a descriptor and a weight; scale
  comes from the band's single transform node. Any spawner whose entries differ by more than about
  1.5x in mesh height has this defect by construction — one band authored for ~1300 cm trees carried
  six 157 cm snags and rendered them at 102-165 cm, with nothing reporting it.

Two more worth knowing:

- **Address the settings sub-object directly**, not the node. A node's settings live in a
  sub-object whose name mirrors the node index; read it off the node's `SettingsInterface` rather
  than guessing the suffix. Writing through the node reads back correctly and changed nothing on
  older builds — see the parent page's object-hop note.
- **Re-pointing a component's static mesh retargets every existing instance immediately**, on the
  next frame, with no re-simulate and no reload. It is by far the cheapest way to re-speciate an
  authored scatter — and it silently invalidates the seat, because each mesh hangs a different
  distance below its own origin.

## `spatial.scatter_layout`

Deterministic, tileable, world-origin anchored, pure. Its limits, in the order they bite:

- **It does not trace.** Every point sits at the region centre's Z; chain a ground verb.
- **It does not tilt.** `pitch: 0, roll: 0`, no knob. Bake lean into the transforms yourself.
- **Its jitter cannot break the row structure below `sqrt(3)/4`** — the geometry, the measurements
  and the superposition workaround are on
  [`vegetation-authoring.scatter-geometry`](vegetation-authoring.scatter-geometry.md).
- **No `minDistance`, no `latticeRotation`.** Both have to be written by the caller.
- **No server-side handoff.** `transforms[]` comes back in exactly the shape the placement verbs
  consume, but it travels through the caller's context both ways — roughly 130 KB each way for
  2,600 instances on one measured zone. That is the practical ceiling on this pathway and the reason
  a large groundcover layer usually ends up on the procedural spawner or in `python.execute`
  instead. A `transformsFile` or a layout handle a later call could reference would close it.
- **`maxPoints` above its ceiling is silently clamped while `jitter` above its cap is refused**,
  though the documentation calls both a ceiling.
- **Yaw is documented 0-360 and returned normalised to (-180, 180]** by the transform round-trip.
  The distribution is uniform; it only bites a caller range-checking the output.

## UE 5.8's Procedural Vegetation Editor

Authorable, not usable — the three structural reasons are on the parent page. Two wiring traps if
you author one anyway: every PV node's `In` pin is single-connection and the connect verb replaces
whatever was there while reporting success, and node creation requires a full
`/Script/<Module>.<Class>` path where graph creation accepts a short name.

Note also that an engine plugin referenced as `"Optional": true` in a `.uproject` can be dropped
entirely once the target build emits a non-empty plugin allowlist, and every skip in that path logs
below the default verbosity — so the classes never register, nothing appears in the log, and the
feature simply is not there. Omit the key for a plugin the project actually needs.

## See also

- [`vegetation-authoring`](vegetation-authoring.md) — the workflow, and how to choose between these.
- [`vegetation-authoring.scatter-geometry`](vegetation-authoring.scatter-geometry.md) — the lattice
  measurements behind the layout advice.
- [`foliage`](foliage.md), [`landscape`](landscape.md), [`pcg`](pcg.md), [`spatial`](spatial.md) —
  per-verb argument reference.
