# editor.collision-review

How to check that the geometry in a level actually has the collision it looks like it has. Switch the viewport to a collision view with `editor.set_view_mode`, capture it, and — this is the load-bearing half — read the collision report that comes back with it.

## The trap this page exists for

**A mesh with no collision draws nothing in a collision view.** Nothing. So an empty collision view and a perfectly collided scene produce the same picture, and the failure looks exactly like success. A screenshot alone can never answer "does everything here have collision?" — it can only show you what *does*.

There are three independent ways a mesh disappears from a collision view, and they need different fixes:

1. **Collision is disabled** on the component (`NoCollision`).
2. **There are no collision shapes** — the mesh has a collision setup that holds zero primitives. Gameplay traces pass straight through it.
3. **It ignores the channel that view queries.** A mesh can have perfect collision and still vanish from the world-collision view because it ignores the Pawn channel.

That is why `editor.set_view_mode` returns a `collision` report next to the mode switch. Read the report; use the picture as evidence for what the report tells you.

## The two collision views

| You want | `viewMode` | What it draws |
|---|---|---|
| World collision — what gameplay traces, characters and physics actually hit | `collisionSimple` | The **simple** collision shapes (boxes, spheres, capsules, convex hulls) |
| Precise collision — the exact surface | `collisionComplex` | The **per-triangle** collision geometry |

Aliases are accepted case-insensitively: `worldCollision` / `playerCollision` / `collisionPawn` for the first, `preciseCollision` / `visibilityCollision` / `collisionVis` for the second.

The values are named for what the geometry *is*, not for the engine channel that draws it, because the engine's own names ("Player Collision", "Visibility Collision") say nothing about simple versus complex — and the mapping **inverts** for a mesh set to *use complex as simple*: on such a mesh the world-collision view draws the per-triangle geometry instead. The report tells you which meshes those are.

## Recipe

```js
// 1. Switch the view and read the data.
call({ method: "editor.set_view_mode", args: { viewMode: "collisionSimple" } })

// 2. Capture the picture from wherever you want to look.
call({ method: "render.capture_open_level", args: { width: 800, height: 800 } })

// 3. Put the viewport back. This verb does NOT restore the mode by itself.
call({ method: "editor.set_view_mode", args: { viewMode: "Lit" } })
```

Step 3 is not optional politeness: the mode persists on the shared editor viewport, so every later capture — yours or another agent's — comes back as a collision view until something switches it back.

## Reading the report

```json
{
  "success": true,
  "viewMode": "CollisionSimple",
  "collision": {
    "channel": "collisionSimple",
    "draws": "simple",
    "scope": "level",
    "actorsInspected": 214,
    "counts": { "noCollision": 2, "complexOnly": 1, "complexAsSimple": 1,
                "unknown": 4, "drawn": 209, "notDrawn": 5 },
    "noCollision": ["SM_Tree_04", "SM_Tree_11"],
    "complexOnly": ["SM_Rock_02"],
    "complexAsSimple": ["SM_Cliff_01"],
    "notDrawn": [
      { "name": "SM_Tree_04", "class": "StaticMeshActor", "simpleShapes": 0,
        "collisionEnabled": true, "respondsToChannel": true,
        "drawnInThisChannel": false, "reason": "noGeometryInThisChannel" }
    ],
    "truncated": false, "nameLimit": 100
  }
}
```

- **`notDrawn` is the answer to the question the picture cannot answer** — precisely the actors that contribute no pixels to the view you are about to capture. Its `reason` distinguishes the three causes above: `collisionDisabled`, `channelIgnored`, `noGeometryInThisChannel`.
- **`noCollision`** — actors whose collision setup holds no simple shapes at all. On a prop or a tree this is usually the bug: traces fall through, characters walk through, and a downward ground probe with precise collision enabled will silently hit the *render* triangles instead of the ground.
- **`complexOnly`** — geometry that only answers as per-triangle collision.
- **`complexAsSimple`** — meshes where the two views swap what they draw. Expect them to look "missing" from whichever view you assumed.
- **`unknown`** — actors with no simple/complex split to report, such as landscape. Not a finding; they are excluded from the problem lists deliberately rather than reported as collisionless.
- **`counts` are always exact.** The name lists are capped at `nameLimit` entries and `truncated` says whether anything was clipped. Hitting the cap is itself the finding: the level has a systemic gap, not two stray props.

`scope` is `"level"` — the report covers every actor in the open level, not just what happens to be in frame. It is stated in the payload rather than implied, so a small `notDrawn` list is never mistaken for "nothing wrong outside the current view". Skip the scan with `collisionReport: false` when you only want the mode switched.

## Related pages

- [`editor`](editor.md) for the rest of the viewport and view-mode surface.
- [`visual-review`](visual-review.md) for choosing a capture surface and for capture resolution guidance.
- [`spatial`](spatial.md) — `spatial.raycast` reports the same underlying collision facts per hit (`simpleCollisionShapes`, `renderGeometryHit`), which is how you confirm a specific trace was blocked by a collisionless mesh. Capturing both views over one mesh is also how you *see* a hull that diverges from the render surface — the failure *Which trace, for which ground* on that page exists for, and the one no response field flags.
