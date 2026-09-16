# Wiki Overlay Directory

This directory holds editorial overlays merged into the rendered wiki output by `FWikiOverlay` at request time. The MCP `call` tool routes an omitted-`args` request to `WikiHandler::RenderPage`. Most markdown files map to one non-root namespace path: `<dotted.path>.md`. Cross-namespace workflow pages use standalone slugs such as `workflows.md`.

For a human catalog of the docs and overlay pages, see [../index.md](../index.md).

## Live Wiki Navigation

The MCP surface exposes one tool: `call`.

| Call shape | Meaning |
|---|---|
| no `method`, no `args` | Return the generated root wiki index. |
| `method`, no `args` | Return the wiki page for a branch, leaf namespace, hybrid namespace, or method. |
| `method` plus `args` | Execute the RPC named by `method`; use `args={}` for parameterless methods. |

`path` is accepted as an alias for `method` in every shape above; supplying both with conflicting values is rejected, as is any other top-level argument field.

Bare namespace pages such as `call("blueprint")` are intentional caller-facing indexes. They combine generated child/method content with these overlays; they are not static full-reference dumps.

## Overlay Shape

Each namespace overlay file has three structural parts:

- H1 title: `# <dotted.path>`. The merger strips it before rendering.
- Root summary / namespace prelude: content after the H1 and leading HTML comments until the first `## ` or `### ` heading. It appears in the generated root index for top-level namespaces and above generated namespace content on the namespace page.
- Optional method detail sections: `### <full.method.name>` sections appended to matching method pages.

Branch pages such as `container.md` and `material.md` use only a prelude. Leaf or hybrid pages such as `actor.md`, `blueprint.graph.md`, `spatial.md`, and `camera.md` may also include method detail sections. The `spatial.md` and `camera.md` overlays cover the spatial-perception and multi-angle-capture namespaces (see the [spatial authoring guide](../spatial-authoring.md) for the cross-namespace workflow they anchor).

## Authoring Rules

- Do not duplicate generated signatures, parameter lists, or one-line summaries.
- Use overlays for workflow, gotchas, cross-references, and examples.
- Write cross-refs and examples using `call("path")` or the explicit `path` / `args` shape.
- Never hand-write a "Key methods" / "Methods:" list — the `## Methods` index is auto-generated from the registry.
- Put longer category-page guidance under `##` headings so it stays off the root page.
- Keep non-empty intro text before the first `##` or `###`; a top-level overlay with no intro renders as a bare root entry.
- Add method H3 sections only when generated content alone would mislead.
- **Classify every new top-level namespace in `maturity.json`** (this directory; flat `slug -> "core" | "experimental" | "internal"`). It drives the root-index marker and the namespace page's `Stability:` line. The map **fails closed**: a slug with no entry, or an entry whose value is not one of the three, renders `(unclassified)` / `Stability: unclassified` and publishes `"tier": "unclassified"` in `registry.json`. Never write `unclassified` into the file — it is a render-time fallback that exists so a forgotten namespace cannot masquerade as `core` (`B-maturity-unmapped-namespace-fails-open`; bare used to mean both "core" and "no entry"). `PinWright.infra.wiki_handler.Maturity.EveryRegisteredNamespaceIsClassified` fails the suite naming any unclassified namespace; a key matching no registered namespace is a `LogWikiHandler` warning only.
- An `### ` heading reaches a method page only when its text is **exactly** the bare dotted method name. `### widget.wrap` works; ``### `widget.wrap` — wrap a widget with a new parent`` is keyed under a string nothing looks up, so its body reaches no page and it silently truncates the section above it. Sub-headings inside a method section (`### Params`, `### Examples`) have the same failure mode — use bold labels instead.

`PinWright.infra.wiki_src.SourcePagesFollowRenderingRules` enforces four rules across every page
here: each `### ` heading must be an exact slug the live renderer serves, no two `### ` headings in
a file may share text, no `## ` may follow the first `### `, and a top-level overlay must have prelude
text. Failures are otherwise silent — the source can look right while part of it renders nowhere.
The two-sentence prelude limit is not enforced; it remains a review item.

## Cross-Page Link Convention

**Link the method page, not an anchor on the namespace page.** Write ``[`asset.dump`](asset.dump.md)``, never `[asset.dump](asset.md#assetdump)`.

Decided 2026-08-14 across 44 links; do not re-litigate. `### asset.dump` never renders into the generated `asset.md` — the namespace page stops at the first `### ` — so the anchor form points at a heading that does not exist in the output the reader is looking at. Every method has its own generated page (`Saved/PinWright/wiki/<namespace.method>.md`, the same file `call("<namespace.method>")` resolves to), the generated tree is flat, so the relative link resolves for an agent reading or grepping that folder, matching how `index.md` already links topic pages. The cost is that the target does not exist inside `docs/wiki-src/` itself; the source tree is authoring material, the generated tree is the reader surface, so the reader surface wins.

Anchors to `## ` sections stay valid and stay in use (`system.md#long-running-jobs`) — `##` headings above the first `### ` do render. Confirm the target section is above the first `### ` on its page before linking to it.

Missing files fall back to generated content only. The merger reads files lazily and caches by mtime, so edits to existing files are picked up by the next wiki read without restarting the editor.

## Topic Pages

Standalone topic pages are cross-namespace walkthroughs rather than overlays for registered RPC namespaces. They live beside namespace overlays as `docs/wiki-src/<slug>.md` and are reachable directly through `call("<slug>")`, for example `call("workflows")`, `call("runtime-uobject-inspection")`, or `call("unattended")`.

The router discovers topic pages when `WikiHandler` builds its cache by enumerating `docs/wiki-src/*.md`. It skips `README*.md` files and skips any slug already owned by a registered category. Method and category nodes win on collisions, so do not reuse namespace or method names for topic slugs.

Topic pages use the same H1 and prelude parser as overlays, but they should not use `###` headings. `###` is reserved for method detail sections in namespace overlays, and standalone topic rendering stops before the first `###`.

Adding a brand-new topic file requires an editor restart because the topic-node set is built once with the wiki cache. Edits to an existing topic file are picked up by mtime revalidation.
