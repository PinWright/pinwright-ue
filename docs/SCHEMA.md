# PinWright Docs Schema

This directory is the maintainer-facing knowledge base for the PinWright plugin. It covers architecture, BPIR, tests, release/security notes, and MCP wiki overlays.

## Directory Layout

```text
docs/
  SCHEMA.md                      This file. Documentation conventions.
  index.md                       Human-readable documentation catalog.
  tags.md                        Reverse tag index for maintainer docs and wiki overlays.
  lessons.md                     Append-only operational lessons.
  adr/                           Architecture decision records, one file per decision.
  wiki-src/                      MCP wiki editorial overlays.
  *.md                           Maintainer-facing docs and references.
```

The issue board is not part of this directory. It is the public repo `PinWright/pinwright-board`, cloned outside this repo at `../../../.pinwright-board` relative to the plugin directory, with its own schema in its `README.md`.

## Page Types

| Type | Purpose | Example |
|---|---|---|
| `system` | Architecture or implementation deep dive | `arch.md`, `bpir-compiler-internals.md` |
| `guide` | Human workflow or language guide | `ir-authoring.md`, `crir-language-reference.md` |
| `reference` | Narrow lookup material | `test-organization.md`, `ue-async-completion-delegates.md` |
| `index` | Catalog or reverse index | `index.md`, `tags.md` |

## Frontmatter

New or substantively edited maintainer docs should start with YAML frontmatter:

```yaml
---
type: system | guide | reference | research | index
summary: "Keyword-rich 1-2 sentence description."
date: YYYY-MM-DD
tags: [lowercase, freeform, tags]
---
```

Rules:

- `type`, `summary`, `date`, and `tags` are the canonical fields for maintainer docs.
- Use `date` for the last significant content update, not mechanical link/index edits.
- Tags are lowercase, kebab-case, and specific enough to be useful in [tags.md](tags.md).
- Normalize legacy frontmatter to this shape when touching a page for substantive content.

Files that do not use this frontmatter:

- `SCHEMA.md` by definition.
- `lessons.md`, which is append-only operational memory.
- `wiki-src/*.md`, which follows the MCP overlay format below.
- `adr/*.md`, which opens with the decision as its H1 and is immutable once accepted. Index it in [index.md](index.md) and [tags.md](tags.md) instead.

## MCP Wiki Overlays

Files in [wiki-src/](wiki-src/) are merged into the rendered wiki output by `FWikiOverlay` whenever a request body without `params` is routed to `WikiHandler::RenderPage`. They deliberately do not use YAML frontmatter because the merger reads a strict overlay shape:

- H1 title: `# <dotted.path>`.
- Root summary: content after the H1 and leading HTML comments until the first `## ` or `### ` heading. This appears in the generated root wiki index for top-level namespaces.
- Namespace prelude: the same content also renders above generated namespace content on the namespace page.
- Method detail sections: `### <full.method.name>` sections appended to matching method pages.

Overlay authoring rules:

- Do not duplicate generated signatures, parameter lists, or one-line summaries.
- Use overlays for workflow, gotchas, cross-references, and examples.
- Write cross-refs and examples using `call("path")` or the explicit `path` / `args` shape.
- Never hand-write a "Key methods" / "Methods:" list — the `## Methods` index is auto-generated from the registry, so such lists are dead weight and are deleted when found.
- Put longer category-page guidance under `##` headings so it stays off the root page.
- Keep non-empty intro text before the first `##` or `###`; a top-level overlay with no intro renders as a bare root entry.
- **Rendering stops at the first `### ` line**, so every `##` section must sit above it or it is silently invisible. Prefer bold labels to `###` on topic pages.
- Add method H3 sections only when generated content alone would mislead, and title them with the bare dotted method name — anything else strands the section.

Mutating RPC handlers documented in overlays must validate first, then open `FScopedTransaction` immediately before the first mutation, call `Modify()` on changed UObject owners, and keep structural modifications inside the transaction.

## Maintenance

When creating, renaming, or deleting a maintainer doc:

1. Update [index.md](index.md).
2. Update [tags.md](tags.md) if tags changed.
3. Add reciprocal links from related docs when useful.

`PinWright.core.docs_schema.EveryMaintainerDocIsIndexed` enforces the reachability half of steps 1 and 2: every `.md` under `docs/` outside `wiki-src/` must be linked from **both** indexes. It does not judge whether each frontmatter tag deserves its own row — rows are curated, and most tags should not get one.

`PinWright.infra.wiki_src.SourcePagesFollowRenderingRules` enforces the overlay rendering rules above across every page in `wiki-src/`; see [wiki-src/README.md](wiki-src/README.md).

When changing only `wiki-src/*.md`, update [tags.md](tags.md) if the overlay adds a new documented namespace or major workflow tag. The live MCP root index remains generated by calling the MCP `call` tool with no arguments.
