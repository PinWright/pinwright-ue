---
name: mcp-review
description: Verify IN-REVIEW fixes on the PinWright MCP issue board by dispatching one subagent per ticket. Use when the user says "verify MCP fixes", "check in-review items", "test MCP board", "mcp review", "re-check fixes", "mcp verify", or asks to validate that IN-REVIEW issues actually work.
---

# MCP Issue Board Verification

Verify every IN-REVIEW ticket on the PinWright MCP issue board. **The main agent orchestrates only.** It does not read ticket bodies, design tests, run MCP tool calls, or edit ticket files. All of that happens inside subagents.

This matters because IN-REVIEW counts run into the dozens. Reading and writing every ticket from the main context burns tokens and serializes work. Pushing the per-ticket loop into subagents keeps the main context flat and lets independent verifications run in parallel.

## Roles

| Role | Where | What it does |
|---|---|---|
| **Main agent** | this conversation | Collects IDs, classifies interference, builds waves, dispatches subagents, summarizes. Never reads ticket bodies, never edits ticket files. |
| **Classifier subagent** | one-shot subagent | Reads frontmatter + latest IN-REVIEW history entry of each ticket. Returns one interference profile per ticket as JSONL. |
| **Verifier subagent** | one per ticket | Reads its ticket, designs a minimal test, runs MCP calls, decides PASS/FAIL/SKIP/CRASH, edits its own ticket frontmatter and history. |

## Issue Board Location

`../../../.pinwright-board/` — one file per issue, filename = `{id}.md`. Workflow rules and frontmatter schema live in `../../../.pinwright-board/README.md`. Every board path in this skill is relative to the plugin directory (`<host project>/Plugins/PinWright`), so the board is a sibling of the host-project checkout; it is the public repo `PinWright/pinwright-board`, cloned there.

**Terminal statuses are off-limits.** `DONE` and `WONTFIX` are never re-tested or reopened from inside this skill. The Phase 1 filter enforces this; never relax it.

## Phase 1: Collect IN-REVIEW IDs

Run this Grep call exactly — it returns the full list of IN-REVIEW ticket paths in one shot, without reading any bodies:

```
Grep
  pattern: "^status: IN-REVIEW"
  path: "../../../.pinwright-board"
  output_mode: "files_with_matches"
  head_limit: 0
```

`head_limit: 0` removes the default 250-line cap so a board with hundreds of IN-REVIEW tickets isn't silently truncated. The pattern is anchored to column 0 so it matches the YAML frontmatter line and not stray prose mentions of "IN-REVIEW".

Bash fallback (only if Grep is unavailable for some reason):

```bash
grep -lE '^status: IN-REVIEW' ../../../.pinwright-board/*.md
```

Note the count of returned files and tell the user before dispatching ("Found N IN-REVIEW tickets, classifying interference now."). Do not Read any of them in the main agent — the classifier subagent does that next.

## Phase 2: Classify Interference

Dispatch one classifier using the environment's configured worker defaults. The prompt is a single short instruction — **pass the protocol by absolute path, do not paste its contents**:

> Classify interference for these IN-REVIEW tickets. Read and follow the protocol at `<SKILL_ROOT>/classify-protocol.md`. Tickets:
> - `<absolute path 1>`
> - `<absolute path 2>`
> - ...

The subagent reads the protocol file itself. Inlining it into the prompt wastes the main agent's tokens, drifts as the protocol evolves, and defeats the whole point of the protocol file existing.

The subagent returns one JSON line per ticket and prints them in its final message.

Capture the JSONL into the conversation as compact lines like:

```
{"id":"B-foo","mode":"inert","targets":[],"reason":"decompile only"}
{"id":"B-bar","mode":"localized","targets":["/Game/X/Foo"],"reason":"compile_bpir replace"}
{"id":"B-baz","mode":"global","targets":[],"reason":"runs full test suite"}
```

Three modes:
- **inert** — verification is read-only OR operates on a private temp asset the verifier creates and deletes. Includes decompiles, dumps, schema lookups, exports.
- **localized** — verifier mutates one or more specific shared assets (named in `targets`). Two `localized` tickets conflict only if their `targets` overlap.
- **global** — affects the whole editor session: restarts the editor, runs the full automation test suite, builds lighting, rebuilds navmesh, deletes widely-referenced assets, mutates engine config.

The classifier defaults to escalating one tier when unsure. Better to run something alone than to corrupt a parallel wave.

## Phase 3: Build Waves

From the classification:

1. **Inert wave** — all `inert` tickets, one parallel batch.
2. **Localized waves** — greedy bin-packing of `localized` tickets so no two tickets in one batch share a `targets` entry. Aim for ~4 per batch.
3. **Global waves** — each `global` ticket alone, one wave per ticket.

Order: inert → localized batches → global. Within a wave, dispatch all verifier subagents in the same tool turn so they run in parallel. Use `run_in_background: true` so completion notifications arrive incrementally.

## Phase 4: Dispatch Verifier Subagents

For each ticket in a wave, spawn a verifier subagent. The prompt is one short instruction — **pass the protocol by absolute path, do not paste its contents, do not pre-design the test**:

> Verify the IN-REVIEW ticket at `<absolute ticket path>`. Read and follow the protocol at `<SKILL_ROOT>/verify-protocol.md`. Report PASS/FAIL/SKIP/CRASH in 3 sentences when done.

That is the entire prompt. Do not embed test plans, asset paths, expected response shapes, or excerpts of the protocol. The verifier reads its ticket and the protocol itself — every line of pre-design you slip in is wasted main-agent context and stale-by-construction guidance for the subagent.

Use the environment's configured worker defaults and available completion notifications. Follow its delegation policy instead of pinning a model in this skill.

## Phase 5: Summary

When the wave (or all waves) finish, present a single table to the user:

```
| Issue | Result | Notes |
|-------|--------|-------|
| B-foo | PASS   | <one-line reason from subagent report> |
| B-bar | FAIL   | <symptom> |
| B-baz | SKIP   | <why> |
```

If the user said "stop when these done" or similar mid-run, do not dispatch additional waves. Let the in-flight wave finish, then summarize what was covered and explicitly list which tickets were not tested.

## Failure Modes and How to Handle Them

- **Editor crash mid-wave.** A later verifier in the same wave will hit RPC timeouts. Surface this in the summary; the next `mcp-review` invocation re-tests anything left as IN-REVIEW.
- **Verifier subagent doesn't update its ticket.** The subagent's report is hearsay; the ticket file is the source of truth. After each wave, re-grep `^status: IN-REVIEW` against the ticket paths just dispatched. If a ticket the subagent claimed PASS is still IN-REVIEW, dispatch a one-line follow-up subagent: "Update the frontmatter and history of `<path>` per `verify-protocol.md` to reflect a PASS verification you ran earlier."
- **Classifier returns junk.** If JSONL is malformed or empty, re-dispatch with a stricter prompt. Don't try to salvage.
- **Asset listed in a ticket no longer exists.** That's the verifier's problem — it should `asset.search` for a substitute or SKIP.

## Important Notes

- **Never read ticket bodies in the main agent.** That's the verifier's job.
- **Never edit ticket files in the main agent.** That's the verifier's job.
- **Never inline protocol contents into subagent prompts.** Pass `classify-protocol.md` and `verify-protocol.md` by absolute path. Subagents read them. Inlining defeats the protocol-file pattern, drifts the moment the file changes, and burns main-agent tokens on text the subagent already has access to.
- **Never run the PinWright automated test suite as part of verification** — the suite is slow and tests different things than what we're checking here. The protocol explicitly forbids this.
- **No connectivity precheck.** Don't ping the editor before dispatching — the first verifier RPC failure surfaces a dead editor on its own. Subagents load their own MCP tool schemas via ToolSearch as needed; the main agent doesn't load them at all.
