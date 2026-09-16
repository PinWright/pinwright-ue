# workflows

Task-oriented entry points for the PinWright MCP wiki. Use this page when you know the job you want done but not which namespace owns the calls.

## Current Workflow Pages

These workflows describe current RPC and wiki surfaces only; drill into the linked namespace pages for exact parameters.

| Task | Start with | Use when | Main supporting namespaces |
|---|---|---|---|
| Build an evidence cache for assets | [`asset-audit`](asset-audit.md) | You need repeatable text mirrors for Blueprints, Widget Blueprints, materials, levels, Niagara, textures, or references before analysis or edits. | `asset`, `blueprint`, `widget`, `material`, `level`, `system.job_status` |
| Make persistent edits safely | [`safe-mutation-save`](safe-mutation-save.md) | You are about to change an asset, level, actor, property, or container and need a read-edit-save-verify loop. | `property`, `container.*`, domain authoring namespaces, `editor.save_all`, `level.save` |
| Prove visual state | [`visual-review`](visual-review.md) | You need a PNG capture or runtime/widget visual evidence after an edit. | `widget`, `render`, `editor.screenshot`, `asset.dump`, `system.job_status` |
| Inspect live PIE UObjects | [`runtime-uobject-inspection`](runtime-uobject-inspection.md) | The source of truth is a live subsystem, GameInstance, PlayerController, or other transient UObject instead of a content package. | `system.inspect`, `property`, `object.call_function` |
| Build a level | [`level-building`](level-building.md) | You are taking a level from nothing to playable — working from reference imagery, creating and lighting the map, massing geometry at scale, conforming it to terrain, and driving it from re-runnable build scripts. | `level`, `level.structure`, `landscape`, `lighting`, `actor`, `geometry`, `spatial`, `water` |
| Review a level | [`level-review`](level-review.md) | You need to judge whether a level looks and plays right — comparable passes, a whole map framed without guesswork, motion sampled rather than assumed, proportion checked at ranges a top-down shot cannot resolve, and every reported figure traceable to what produced it. | `render`, `editor`, `lighting`, `spatial`, `camera`, `sequencer` |
| Author a camera flythrough | [`cinematic-flythrough`](cinematic-flythrough.md) | You are building a scripted camera move through a finished level — scouting shots before keying a path, authoring tangents that do not stall the camera, closing a loop in velocity, verifying motion rather than stills, and rendering offline. | `camera`, `render`, `sequencer`, `spatial`, `mrq` |
| Profile and optimise a level | [`performance-profiling`](performance-profiling.md) | You are chasing a frame-time problem — reproducing the user's conditions rather than a heuristic's worst case, telling a convergence cost from a steady-state one, finding the bound thread before ranking anything, and re-measuring interleaved because the machine drifts under you. | `insights`, `performance`, `render`, `editor`, `system.console_command` |
| Plant a level's vegetation | [`vegetation-authoring`](vegetation-authoring.md) | You are scattering trees, shrubs, groundcover or rocks — choosing between the five placement pathways, breaking the lattice, seating trunks that the seat solve cannot express, and verifying against scalability and grass settings that hide your work while every verb reports success. | `foliage`, `landscape`, `spatial`, `pcg`, `render`, `performance` |
| Produce a showcase video | [`showcase-video`](showcase-video.md) | You are shooting footage to be watched as motion — deciding which shots the offline pipeline owns, turning an asset on a turntable without the preview scene's per-shot lighting cycle reaching the cut, and proving which render ended up in the file you shipped. | `mrq`, `sequencer`, `render`, `camera`, `image`, `system.console_command` |

## How To Use A Workflow Page

1. Open the workflow page with `call("<slug>")`.
2. Follow the workflow until it names the namespace or method that owns the next step.
3. Open that namespace page, for example `call("asset")`, `call("widget")`, or `call("property")`, before executing.
4. Execute only after you have the exact method and parameter shape.
5. If a call returns `ticket_id`, wait through `call("system.job_status", {"ticket_id": "..."})` before consuming output.

## When No Workflow Fits

Start at `call()` for the root namespace index, then drill into the nearest namespace. Use `call("wiki")` for wiki navigation rules and `call("system.console.search")` before using `call("editor.console_command")` or `call("system.console_command")` with an unfamiliar console command or CVar.
