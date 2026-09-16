---
name: mcp-retriage
description: Re-score the severity of every OPEN PinWright issue-board ticket against a fixed impact×reach rubric, as a bounded one-pass background Workflow, so the mcp-fix-workflow picker (which works OPEN tickets in severity order) works the genuinely-most-important tickets first. Scores all OPEN once, writes back only the tickets whose severity changed (one append-only #N-retriage history line each), commits the exact changed paths, and stops. Use when the user says "mcp-retriage", "re-score the board", "retriage tickets", "recompute severities", "re-prioritize the issue board", or "rebalance severities".
---

# MCP Retriage

Re-score the `severity` frontmatter of every **OPEN** PinWright issue-board ticket
against the fixed impact×reach rubric (the board README's "Severity Levels"
section), as a background Workflow. The `mcp-fix-workflow` picker ranks OPEN
tickets by severity and works the top band first, so a mis-rated ticket is worked
in the wrong order, and an unscored backlog where almost everything is `Low`
degenerates to alphabetical pick order. This skill corrects that drift: it reads
all OPEN, scores them in parallel against one shared rubric, writes back only the
tickets whose severity actually changed (one append-only `#N-retriage` history line
each), commits the exact changed paths, and stops.

It is a **single bounded sweep, not a supervised loop**: it returns on its own and
needs no supervision babysitting. It is **structurally idempotent**: a ticket whose
recomputed severity equals its current value gets no write. But the scoring is done
by LLM agents, so it is **not bit-stable** across runs: a re-run leaves the clear
cases alone yet may re-rate a handful of borderline tickets as scores vary by a
level. Run it on demand and **review the diff before pushing**; it is not meant for
unattended cadence without that review (or without adding hysteresis).

## Preflight (main conversation)

- No editor or MCP needed: this never calls the MCP, it is pure board file I/O.
- It coexists with live `mcp-fix-workflow` fuzz hosts writing the same board: it is
  **claim-aware** (skips a ticket carrying a fresh foreign lease), **append-only**,
  and **only ever edits `severity` plus one history line** (never `status`), so a
  concurrent `OPEN -> IN-REVIEW` flip is not clobbered.
- The board is a standalone shared directory outside the plugin repo (its own git
  root), not published with the plugin. By default the run commits to that board
  repo; pass `push:false` to stop at a local commit for review (recommended for the
  first run on a board with unrelated uncommitted changes), or `push:true` (the
  default) to publish to `origin/master`.

## Launch (background workflow)

Invoke the **Workflow** tool with:
- `scriptPath`: `<host project>\.claude\skills\mcp-retriage\mcp-retriage.workflow.js`
  (the compiled copy; author the source under `Plugins\PinWright\.polyskill\skills\mcp-retriage\` and run `npx polyskill` in `Plugins\PinWright\.polyskill` to regenerate it).
- `args`:
  - `boardPath` (default `../../../.pinwright-board`) — folder of `{id}.md` tickets. Board paths are relative to the plugin directory (`<host project>/Plugins/PinWright`), so the default is a sibling of the host-project checkout; the board is the public repo `PinWright/pinwright-board`, cloned there.
  - `boardRepo` (default `../../../.pinwright-board`) — git root for the exact-path commit.
  - `push` (default `true`) — `true` commits + `pull --rebase` + pushes to `origin/master`; `false` commits to the local clone only and stops.
  - `dryRun` (default `false`) — score and report, write nothing, commit nothing.
  - `maxTickets` (default `0` = all OPEN) — cap for a partial / test pass.
  - `scoreFanout` (default `5`) — number of parallel Score agents.
  - `claimTtlHours` (default `4`) — lease-freshness window, matching the board README's 4h rule.
  - `hostId` (optional) — this run's identity for the claim-skip (derived as `fuzzN` from a passed `host`/`projectPath`, else `retriage`); only a fresh lease by a DIFFERENT host blocks a ticket.

Report the task id. Because it is a bounded sweep, you do **not** need a supervision
loop: await the completion notification (it carries the return value), then report
the counts. Relaunching with the same args is safe (idempotent re-convergence).

## Per-pass pipeline (the workflow)

1. **Read.** One agent globs the board (excluding `README.md`), parses frontmatter,
   returns every `status: OPEN` ticket as `{id, path, currentSeverity, category,
   claimBlocked}` plus one `now` (`Get-Date -Format o`). `claimBlocked` is true only
   for a fresh (`< claimTtlHours`) lease by another host.
2. **Score.** The eligible OPEN list is split into `scoreFanout` deterministic
   contiguous slices; each agent re-reads its tickets' bodies and returns
   `{id, proposed, reason}` using the embedded rubric (identical text in every
   prompt, so scores stay consistent across agents).
3. **Decide (script).** Drop unchanged (`proposed == current`) so nothing churns;
   keep the rest as `{old, new, reason}`, tallying up vs down.
4. **Write.** One agent edits only the changed tickets: re-checks each is still
   `OPEN` (skips it otherwise), changes only the `severity:` line, and appends one
   `` `#N-retriage` `OPEN` triage — <old>→<new>: <reason> `` history line (N is that
   file's own current max + 1). Skipped entirely on `dryRun`.
5. **Commit.** One agent stages **only** the exact changed paths (never `add -A`),
   commits, and — when `push` is true — `pull --rebase` (keeping both sides on a
   history conflict) + pushes. Skipped on `dryRun`; stops after the local commit
   when `push` is false.

The return value carries `counts` (scored / changedUp / changedDown / unchanged /
skippedClaimed / written) and the `changed` list.

## Scope & safety

- **OPEN-only.** Never touches IN-REVIEW / DONE / WONTFIX, and never changes `status`.
- **Claim-aware.** Skips an OPEN ticket carrying a fresh (`claimedAt` within
  `claimTtlHours`) `claimedBy` by another host; a stale or own claim is fine to
  re-score. Retriage never writes a claim itself (it is not picking work).
- **Append-only.** Updates only `severity:` plus one `#N-retriage` history line;
  never edits a prior history line; `N` is the file's own current max + 1.
- **Structurally idempotent (not bit-stable).** `proposed == current` means no
  write, so unchanged tickets never churn. But LLM scoring varies run to run, so a
  re-run may re-rate a few borderline tickets rather than report `changed: 0`;
  review the diff instead of relying on convergence.
- **Exact-path commits.** Stages each changed ticket path individually, never
  `add -A` / `commit -a`; leaves any unrelated uncommitted board changes untouched.

## Classifying the completion (`stop_reason`)

- **`done`** — the sweep finished; report the `counts` and `changed` list (and
  whether it `pushed`). An empty `changed` list means the board was already
  converged.
- **`agent_died`** — a phase agent died after retries. **Transient**: relaunch the
  same Workflow; it re-converges (already-applied tickets now read unchanged and are
  skipped). Stop after 2 no-progress relaunches.
- **`fatal`** — board path missing / unreadable, or a git state a human must
  reconcile. **Do not relaunch**; surface the `reason`/`excerpt`.

## When to run

On demand before a sprint, or on a cadence (e.g. nightly via `CronCreate` / `/loop`)
to keep the picker honest as new tickets land at default severities. It is finite
and returns on its own; re-running is cheap and safe.
