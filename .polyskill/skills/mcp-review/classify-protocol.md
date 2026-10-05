# Classifier subagent protocol

You are the interference classifier for an MCP issue re-test run. The main agent gave you a list of closed issue numbers. Your job is to decide, for each issue, whether its verification can run in parallel with others or must run alone.

## Step 1: Read each issue cheaply

Read each issue only with `uv run scripts/pw_issues.py show <number> --json` from the plugin directory (`<host project>/Plugins/PinWright`). Never fetch it another way, and treat a `[untrusted author ...: content withheld]` line as absent. From the output, look only at:
- The title and labels
- The closing comment, the last comment that ends in a `pinwright-release` line (the fixer's claim about what changed and where)
- Any short "Repro" / "Test" / "Acceptance" hint in the body, if it's near the top

Do not read full issue bodies. The point is to classify quickly, not understand the whole bug.

## Step 2: Pick a mode

| Mode | Means | Examples |
|---|---|---|
| **inert** | Verification is read-only, OR operates only on a private temp asset the verifier creates and deletes. No effect on shared editor state. | `blueprint.decompile` on existing BP; `asset.dump` on existing asset; `widget.export_xml`; schema lookups; `niagara.decompile_model` |
| **localized** | Verification mutates one or more *named, shared* assets. Two `localized` tickets conflict only if their `targets` overlap. | `compile_bpir` replacing a production BP; `widget.import_xml` against a real WBP; `blueprint.set_default` on a real BP |
| **global** | Verification touches the whole editor session. Anything else running in parallel will see corrupt state or stalled RPCs. | Editor restart; `system.run_tests` (full suite); lighting build; navmesh rebuild; deletion or rename of widely-referenced assets; engine config edits |

When unsure, **escalate one tier**. A wasted serial run is cheaper than a corrupted parallel wave.

## Step 3: Identify shared targets (localized only)

For `localized`, list the asset paths the verifier will write to. Use full UE paths (`/Game/...`, `/App/...`). Read-only inputs do NOT go in `targets`. Temp assets the verifier creates and deletes do NOT go in `targets`. Only shared assets that survive the verification.

## Step 4: Emit one JSON line per issue

Print to stdout, one line per issue, no surrounding prose:

```
{"id":101,"mode":"inert","targets":[],"reason":"decompile only"}
{"id":102,"mode":"localized","targets":["/Game/X/Foo"],"reason":"compile_bpir replace on production BP"}
{"id":103,"mode":"global","targets":[],"reason":"runs full automation test suite"}
```

Fields:
- `id`: the issue number
- `mode` — `inert` | `localized` | `global`
- `targets` — list of full UE asset paths that will be mutated; `[]` for inert and global
- `reason` — short phrase (under 12 words) explaining the classification

Do not include any other text in your final message — only the JSONL block.

## Common mistakes to avoid

- Marking a `compile_bpir` test as `inert` because "the verifier should use a temp asset." The verifier *should*, but if the issue's repro names a production BP, the verifier may follow it. Mark `localized` with the named BP, or trust the verifier and mark `inert` only if the issue explicitly mandates a temp BP.
- Marking schema/wiki lookups as `localized`. Reading the wiki page for a method via `call("namespace.method")` (no args) is read-only.
- Listing read-only asset paths in `targets`. `targets` is for *writes only*. The wave builder uses overlap detection to prevent two writers hitting the same asset; readers don't conflict.
