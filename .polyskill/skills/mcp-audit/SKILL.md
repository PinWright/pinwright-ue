---
name: mcp-audit
description: Review how pinwright MCP tools were used this session and propose improvements to the plugin. Analyzes usage patterns for weak points, bugs, missing features, bad usability, then updates the plugin's issue board with new entries and corrections. Use when the user says "audit MCP", "review MCP", "update issue board", "mcp improvements", or when wrapping up a session that used pinwright MCP tools.
---

# MCP Improvement Review

Review how pinwright MCP tools were used this session. Identify where tools fell short — bugs, missing features, friction, bad UX — then update the issue board.

This skill runs autonomously: it audits, validates findings against source, and writes the board entries itself via parallel validator subagents. It does **not** stop to ask for approval. The user invoked the skill — that's the green light. The user can always revert files if a particular entry was unwanted.

## Issue Board

`../../../.pinwright-board/` — one file per issue, filename = `{id}.md`. Every board path in this skill is relative to the plugin directory (`<host project>/Plugins/PinWright`), so the board is a sibling of the host-project checkout; it is the public repo `PinWright/pinwright-board`, cloned there.

Workflow rules, role definitions, frontmatter schema, and body template are in `../../../.pinwright-board/README.md`. Read it once before editing any entry.

Tracks **MCP tool issues only** — BPIR compiler bugs, widget_import_xml problems, property resolution failures, missing tool features, ergonomic gaps. NOT game-level bugs.

Issue IDs: `B-slug` = bugs, `F-slug` = missing features, `E-slug` = ergonomic improvements. Slugs are short kebab-case derived from the title (e.g., `B-enum-raw-integers`, `F-batch-pin-defaults`, `E-replace-auto-clean`).

## Phase 1: Review Session Usage

The pinwright MCP exposes a single tool, `mcp__pinwright__call`. Every underlying RPC method is reached through it: `call({path: "actor.spawn", args: {...}})` executes `actor.spawn`, `call({path: "actor"})` (no `args`) renders the wiki page for the `actor` namespace, and `call()` with no parameters renders the wiki root.

Go through the conversation and find every `mcp__pinwright__call` invocation. For each one, extract the `path` argument to identify which underlying RPC method (or wiki page) was being targeted, and check whether `args` was present (execute mode) or absent (wiki-navigation mode). Group findings by the underlying `path` value, not by the literal tool name — otherwise every call collapses into one bucket.

For each invocation (or sequence on the same `path`), ask:

- **Did it fail?** Error responses, unexpected results, silent failures (reported success but value didn't persist)
- **Was it a workaround?** A multi-step sequence that should be a single call (e.g., 14 separate `set_pin_default_value` calls instead of one batch)
- **Was it frustrating?** Required retries, unintuitive parameters, confusing errors, had to guess the right format
- **Was something missing?** Needed a method that doesn't exist, had to fall back to `call({path: "python.execute", ...})` or manual editor work
- **Did it surprise?** Behavior contradicted expectations or documentation
- **Was wiki navigation needed?** Multiple `call({path: "<namespace>"})` reads before finding the right method indicate weak wiki content for that namespace — flag as an editorial-overlay gap

Also check `docs/lessons.md` for MCP-related entries that might not have corresponding board entries.

### Sample invocation extraction

A typical session invocation looks like:

```
mcp__pinwright__call({
  path: "blueprint.graph.set_pin_default_value",
  args: { blueprintPath: "/Game/Foo/BP_Bar", nodeId: "...", pinName: "...", value: "..." }
})
```

Extract: `path = "blueprint.graph.set_pin_default_value"` (the underlying RPC), `args` present → execute mode. Audit findings attach to that path, not to `call`.

A wiki read looks like:

```
mcp__pinwright__call({ path: "blueprint.graph" })
```

Extract: `path = "blueprint.graph"`, no `args` → wiki-navigation. Repeated wiki reads on the same namespace before any execute is the signal for an editorial-overlay gap.

## Phase 2: Initial Triage Against Board

Glob every file under `../../../.pinwright-board/*.md` (exclude `README.md`). Read the frontmatter of each to build a lightweight index of `{id, title, status, severity, category}`. Read the full body only for items that look relevant to the session findings.

For each session finding, do a *coarse* classification — final validation is the subagent's job. The categories are:

- **Likely regression** — finding matches a `DONE` item's symptom shape
- **Likely revival** — finding matches a `WONTFIX` item AND the session caused real friction (not "looked similar")
- **Likely existing-item update** — finding matches an `OPEN` or `IN-REVIEW` item; add evidence or adjust status
- **Likely new** — no obvious board match across all statuses

Don't over-think this phase. The validator subagents will read source, search semantically, and decide final disposition. This phase exists only to seed them with hypotheses.

## Phase 3: Dispatch Validators (one parallel subagent per finding)

This is the heart of the skill. For every finding from Phase 2, dispatch a validator subagent. Send all in a single message so they run in parallel. Each subagent has authority to file, refine, merge, reopen, revive, or skip — and **writes the result itself**. The skill does not aggregate proposals for user approval; the subagents are the operators.

Subagent dispatch policy:

- **One subagent per finding.** Don't batch multiple findings into one subagent — each must independently search the source, search the board, and decide. Batching causes shallow validation and missed duplicates.
- **Use configured worker defaults** under the environment's delegation policy unless the user requests an override.
- **Parallel by default.** Run all subagents in a single tool-call batch unless two would write the same file (in which case sequence them or give one of them the responsibility for both).
- **Authority is total within scope.** Each subagent decides: file new, merge into existing, reopen DONE as regression, flip WONTFIX, update OPEN/IN-REVIEW with evidence, or skip if the bug isn't real. It writes the file or edits the existing entry itself. It does not return a proposal for human review.

### Validator subagent prompt template

```
You're validating ONE proposed MCP issue board entry. Authority to file, refine,
merge, reopen, revive, or skip. WRITE the result yourself.

## Board location
`../../../.pinwright-board/` — schema in
`README.md`. ID convention: `B-*` bug, `F-*` feature, `E-*` ergonomic. History
entry format: `` - `#N-slug` `STATUS` role — comment `` where N is monotonic
file-local.

## Proposed entry
- **ID**: <proposed-id>
- **Title**: <one-liner>
- **Severity** (suggested): <Critical|High|Medium|Low>
- **Category**: <bug|feature|ergonomic>
- **Body**: <description: what tool does wrong, what it should do, why it matters>
- **Session evidence** (replayable): <step-by-step repro from this session,
  with concrete RPC paths, args, and verbatim error responses>

## Your job
1. Verify the bug/gap is real by reading handler source. Cite file paths and
   line numbers. Don't take the reporter's word for it.
2. Search the board for duplicates (semantic, not just title match) and for
   DONE/WONTFIX items whose status this finding should flip.
3. Decide disposition and WRITE the file:
   - Duplicate of OPEN/IN-REVIEW → edit that file, append session evidence as
     a new history line. Don't create a new file.
   - Regression of DONE → flip frontmatter `status: DONE → OPEN`, append
     `#N-regression-{tag}` history line citing this session's repro.
   - Revival of WONTFIX (only if session friction was concrete) → flip
     `status: WONTFIX → OPEN`, append `#N-revived-{tag}` line. Don't rewrite
     prior WONTFIX reasoning.
   - Genuinely new → write `board/<id>.md` with full frontmatter + body + one
     initial history entry `#1-{tag} OPEN reporter — ...`.
   - Invalid (can't reproduce, handler doesn't behave as reported) → skip.
4. Refine the ID, title, severity, framing if a sharper formulation surfaces
   during verification. Don't anchor on the proposed wording.

## Constraints
- Verify before writing. No fabricated handler paths or line numbers.
- Body under ~30 lines. Include `**Workaround:**` and `**Fix:**` lines when
  meaningful.
- History entry format is strict; the `#N` prefix matters so future Edit calls
  land at the true end of the section.
- Don't edit existing history entries — only append.

## Report back (one paragraph)
- Disposition: filed-new | merged | reopened | revived | skipped
- Final file path touched (if any)
- Rationale in one sentence
```

Once all subagents complete, summarize the dispositions to the user — file paths touched, what was filed, what was merged, what was skipped. This is informational, not a request for approval; the work is already done.

## Phase 4: Board file conventions (used by subagents)

Subagents follow these rules when writing. They're listed here so the dispatching skill can quote them into each subagent prompt if a finding has unusual shape.

**History bullet format (all appends):** `` - `#{N}-{entry-slug}` `STATUS` role — comment `` where `N` is the current highest `#` number in the file's `## History` section plus one, and `entry-slug` is a short kebab-case descriptor (2–5 words) of what this particular transition is about (not the issue slug — that's in the filename). `N` is file-local, monotonic, and never resets. The `#N` prefix guarantees the bullet is textually unique so Edit lands at the true end of the section.

**Regressions** — edit `board/{id}.md`. Change `status: DONE` to `status: OPEN` in frontmatter. Append:
```markdown
- `#{N+1}-regression-{short-tag}` `OPEN` reporter — Regression: {what was working, what's broken now}. Session evidence: {...}.
```

**WONTFIX revivals** — flip `status: WONTFIX → OPEN`. Append:
```markdown
- `#{N+1}-revived-{short-tag}` `OPEN` reporter — Revived: re-encountered this session. Friction: {concrete impact, workaround needed}.
```
Don't edit the body or rewrite prior WONTFIX reasoning. The revival history line + status flip is the entire change.

**Existing item evidence** — append:
```markdown
- `#{N+1}-additional-{short-tag}` `OPEN` reporter — Additional evidence: {new finding}.
```
Adjust severity in frontmatter only if the new evidence materially shifts impact.

**New entries** — write `../../../.pinwright-board/{new-id}.md` using the template from `../../../.pinwright-board/README.md`:

```markdown
---
id: {new-id}
title: "{Title}"
status: OPEN
severity: {Critical|High|Medium|Low}
category: {bug|feature|ergonomic}
tags: []
---

# {Title}

{Description — what the tool does wrong and what it should do instead. Cite
verified handler paths and line numbers. Include the repro from the session.}

**Workaround:** {if any}
**Fix:** {proposed approach, ideally pointing at the specific helper or
handler that would change}

## History
- `#1-initial-repro` `OPEN` reporter — {what happened this session, with
  RPC path, args, and verbatim error message}
```

Before writing the new file, verify `board/{new-id}.md` does not already exist.

**Rules (applied by every subagent):**
- Never delete history entries — append only.
- Don't try to repair or reconcile pre-existing entries — just emit new rows in the correct format.
- No dates in history entries (git blame provides timestamps).
- `category` must match the ID prefix (`B`→bug, `F`→feature, `E`→ergonomic).
- New IDs use kebab-case slugs. Keep slugs short (2-4 words).
- Verify source claims before writing them. A fabricated line number is worse than no line number.
