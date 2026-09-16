# wiki

The wiki **is** this MCP: the single tool is `call`; its `method` and `args` fields choose documentation lookup or RPC dispatch (`path` aliases `method`).

## The three modes

There is no separate "list methods" or "describe method" tool. If you know the task before the namespace, start with `call("workflows")` for task-oriented pages such as asset audits, safe mutation/save loops, and visual review.

1. **Root** — `call` with no `path` and no `args`. Returns the top-level namespace index, one line per branch with a short summary. Use this when starting fresh and you don't know what's available.

2. **Wiki page** — `call` with `path="audio"` and no `args`. Returns the editorial + auto-generated page in four shapes:
   - **Branch** (e.g. `path="audio"`) — child namespace index plus orientation prose. No method signatures.
   - **Leaf-namespace** (e.g. `path="audio.authoring"`) — method signature list with one-line summaries plus workflow prose.
   - **Method** (e.g. `path="audio.authoring.create_metasound"`) — full signature, parameter table, and any editorial detail.
   - **Standalone topic page** (e.g. `path="runtime-uobject-inspection"`) — pure editorial cross-namespace walkthrough with no auto-generated signature content. See "Standalone topic pages" below.

3. **Execute** — `call` with both `path="audio.authoring.create_metasound"` and `args={...}`. Runs the RPC method named by `path` with `args` as the parameter object. Methods with no required params still need `args={}` to disambiguate from a wiki view.

## Standalone topic pages

Not every wiki page corresponds to a registered namespace or method. Cross-cutting walkthroughs that span several namespaces (e.g. "runtime UObject inspection in PIE", which weaves `system.inspect`, `property`, and `object.call_function`) live as standalone topic pages and are reachable directly via `call("<slug>")`.

The router auto-discovers them at `WikiHandler` cold-init by enumerating `Plugins/PinWright/docs/wiki-src/*.md`, skipping `README*.md` (case-insensitive), and skipping any slug already claimed by a registered Category. Category pages always win on slug collisions — so a topic page named `audio.md` would be shadowed by the `audio` namespace overlay. Discovered slugs land in both the resolvable wiki-node table (under `EWikiNodeKind::Topic`) and the fuzzy-suggestion pool used for typo correction.

Example: `call("runtime-uobject-inspection")` returns [runtime-uobject-inspection.md](runtime-uobject-inspection.md) directly.

The cache is a process-singleton built once at the first wiki render. Adding a brand-new topic-page file requires an editor restart to surface via routing (matching the immutable-registry contract used for the rest of `WikiHandler`). Edits to *existing* topic pages pick up automatically via the same mtime revalidation that `WikiOverlay` uses for namespace overlays.

## Discovery flow

If you misspell a path, the wiki returns a fuzzy-match suggestion list — substring-contains hits first, then Levenshtein-similarity matches. Re-call with the corrected path.

**Maturity markers.** Each root-index entry carries its tier in parentheses and each namespace page repeats it as a `Stability:` line. Unmarked means `core` — solid, primary surface. `(experimental)` works but is less complete and still changing. `(internal)` is plumbing, not meant for direct use. `(unclassified)` means the namespace has **no** maturity entry at all: nothing about it is promised, so treat it as weaker than experimental and verify anything it tells you. Only `core` renders bare, so an unmarked namespace is always a deliberate classification and never an omission.

## Where editorial content lives

Auto-content (signatures, parameter types, summaries) comes from C++ `REGISTER_RPC_HANDLER` macros in `Plugins/PinWright/Source/.../Handlers/`; editorial overlays (workflow prose, cross-refs, gotchas, examples) come from `Plugins/PinWright/docs/wiki-src/<dotted.path>.md`. The handler merges them at request time. At each editor launch, `WikiDiskGenerator` assembles every page, `registry.json`, and a conservative private `.pinwright-wiki-manifest.json`, then writes and byte-verifies that complete set in one unique sibling staging directory before creating an absent final output root or publishing a final file. After staging succeeds, byte-changed pages and `registry.json` are published one at a time through the shared atomic writer. Only after every desired output succeeds does pruning run. The generator then rebuilds the final manifest from desired filenames plus only failed deletions, validates its limits again, and atomically publishes it last. Per-file publication is deliberate because open Windows readers can block a whole-directory rename; a mid-publication or final-manifest failure can leave earlier successful publications or prunes and does not imply whole-tree rollback.

Pruning is fail-closed. It runs only in the canonical `Saved/PinWright/wiki/` root, and only stale top-level generated filenames from a valid previous manifest may be deleted. A configured `WikiOutputDirectory` override still receives generated output but is never pruned. Missing, unreadable, malformed, oversized, over-entry, wrong-owner, wrong-schema, unsorted, duplicate, absolute, nested, or traversal-bearing manifests authorize no pruning; neither a `*.md` suffix nor a generated header is ownership proof. A staging failure leaves the complete prior output tree unchanged and does not create an absent final root. Any desired-output publication failure stops before pruning. A stale file whose deletion fails remains in the final manifest so a later launch can retry it; successfully deleted names are omitted. See `Plugins/PinWright/docs/wiki-src/README.md` for the overlay format.

## Why one tool

Per-method tools fan out to 1,169 entries in the client tool list, which makes near-duplicate methods hard to choose correctly. A single `call` with a wiki-shaped surface forces a navigation step before execution, and lets editorial overlays steer toward canonical methods when near-duplicates exist.

## Core workflows

Use `call(...)` specifically for:

- **Task workflows** — `call("workflows")` when you need a cross-namespace recipe before choosing a method.
- **Wiki navigation** — `call()` for the root index, `call("<namespace>")` to drill down before picking a method.
- **Deep asset inspection** — dump the folder or plugin you're working in FIRST with `call("asset.dump_folder", {"folderPath": "/Game/..."})` before repeated `blueprint.inspect` / `widget.export_xml` / `property.list` calls; `/Game` is the safe default full-project dump, or `call("asset.dump", {"assetPath": "/Game/..."})` for a single package. Output lands under `<ProjectSavedDir>/PinWright/asset-dumps/` and is the preferred source for cached BPIR, widget XML, SCS, properties, level actors, and world metadata; the analysis handlers now surface a `hint` whenever a subtree's mirror is missing or stale.
- **Long-running work** — methods such as `asset.dump_folder`, UWorld `asset.dump`, lighting builds, tests, screenshots, and save operations return a `ticket_id`; poll `call("system.job_status", {"ticket_id": "..."})` or read `Saved/PinWright/jobs.jsonl`.
- **All-in-one BP inspection** — `call("blueprint.inspect", { assetPath })` returns metadata + decompile + refs + perf warnings in one round-trip.
- **BP compile / decompile** — `call("blueprint.compile_bpir", ...)` and `call("blueprint.decompile", ...)` (C++ pseudocode + BPIR). BPIR is the current preferred IR for code generation.
- **Single-call widget tree via XML** — `call("widget.export_xml", ...)` / `call("widget.import_xml", ...)` for asset-tree widget workflows.
- **Material authoring** — full graph creation/editing under `call("material.authoring.<method>", ...)`.
- **Live editor state** — `call("actor.describe", ...)`, `call("level")`, and `call("system.inspect")` when the open editor process is the source of truth.
- **Escape hatches** — `call("python.execute", ...)` only when no typed RPC exists.

## Tool usage conventions

**Class-name parameters — uniform resolution**

Every tool that accepts a class name (e.g. `asset.list` `filter.class`, `blueprint.inspect`, `system.inspect.inspect_class`, `widget.*`) routes the value through `ResolveUClass` (see `Utils/ClassUtils.h`). The resolver uniformly accepts:

| Form | Example |
|---|---|
| Short name (no prefix) | `WidgetBlueprint`, `Actor` |
| U/A-prefixed short name | `UBlueprint`, `AActor`, `UActorComponent` |
| Full `/Script/` path | `/Script/UMGEditor.WidgetBlueprint` |
| Content-mount Blueprint path | `/Game/UI/W_Foo`, `/MyPlugin/Widgets/W_Bar`, `/ShooterCore/BP_Baz` — with or without the `_C` suffix |

Any of these forms work anywhere a class is expected. Full paths remain preferred for determinism.

**Asset paths**

`blueprint.inspect` and most asset-aware tools accept short paths (`/Game/MyGame/Blueprints/B_MyGameMode`). A few tools still expect the double-name format (`/Game/Folder/AssetName.AssetName`); when in doubt, drill into the method's wiki page for the parameter notes.

**Editor must be running**

Every `call(...)` requires the Unreal Editor to be open with the PinWright plugin active on HTTP port 19880. There is no headless / commandlet mode; the plugin skips init during commandlet execution.

**MCP wiki refresh after C++ rebuilds**

After recompiling plugin C++ handlers, **restart the UE Editor and reconnect the MCP client**. The dispatch registry and editorial overlays are loaded at editor startup; without a restart, the wiki and the set of dispatchable methods reflect the previous build. The `call` tool itself never changes shape — only its content does.

**Mutating handlers and `REINST_` references**

If PIE fires ensures about stale `REINST_` references in `CheckAndHandleStaleWorldObjectReferences` after a `call(...)` mutation, the underlying handler is missing an `FScopedTransaction` wrapping. See the `FScopedTransaction` rule in `Plugins/PinWright/docs/wiki-src/README.md` for the overlay-author pattern.

## Reporting bugs & requesting features

Hit a plugin bug you can't work around, or want a feature? See `call("support")` — it walks through drafting a report and opening a prefilled GitHub draft (bugs and critical gaps → Issues; new feature ideas → Discussions/Ideas). Never open or file a report without the user's explicit approval.

## See also

- [`README`](README.md) — overlay authoring and validation rules.
- [`workflows`](workflows.md) — task-oriented routes across namespaces.
- [`support`](support.md) — public issue-report workflow.
