---
name: mcp-sprint
description: Implement accepted open GitHub issues for the PinWright MCP plugin. Picks issues with `pw_issues.py list`, claims them, dispatches one subagent per task for validity review + investigation + planning in parallel, then dispatches implementation subagents (parallel for independent tasks, sequential for tasks touching the same files), then runs N+3 review subagents in parallel (one spec/correctness reviewer per task plus three simplify-style cross-cutting reviewers for reuse, quality, and efficiency on the combined sprint diff), then compiles and runs each task's regression test and closes every verified issue as completed with the evidence in the closing comment. Use when the user says "implement issues", "implement board tasks", "fix MCP issues", "work on the issues", "mcp sprint", "pick tasks from the backlog", or asks to implement open PinWright plugin issues. Complements mcp-audit (files issues) and mcp-review (optional re-test of closed issues); this skill does the implementation and its own verification.
---

# Issue Sprint

Implement accepted open issues of the PinWright MCP plugin. This is the implementation step after `mcp-audit` (files issues); it verifies its own work and closes what passes. `mcp-review` can re-test closed issues later, but nothing waits for it.

## Execution mode: plan mode vs. autonomous

The shape of the sprint depends on whether plan mode is active when the skill is invoked.

**In plan mode** (the usual case when the user types `/mcp-sprint` with plan mode toggled on): Phases 1–3 (select tasks → analysis subagents → conflict grouping) all run **before** `ExitPlanMode`. The plan file you write must carry the actual per-task analysis outputs — reshape/reclassify/wontfix decisions, file lists, fix directions, regression test specs, conflict groups — so the user is approving the real sprint, not a meta-plan about how the sprint will be planned. After approval, only Phase 4 (implementation subagents) remains. This is the critical invariant: **never ask the user to approve a plan that still defers analysis to post-approval.** If you find yourself writing "will be determined after Phase 2 returns" in the plan, stop and run Phase 2 first.

**Outside plan mode** (user invoked autonomously, e.g., via `/loop` or a cron): run end-to-end without pausing. Don't stop to ask whether the task selection is correct, whether reshaping is acceptable, or whether to proceed to implementation. Make the call, log the reasoning in issue comments, move on. The user audits outcomes through the issue comments and, optionally, `mcp-review` re-tests.

The only reasons to stop early in either mode: (a) `pw_issues.py list` returns nothing, (b) every listed issue is a reopened one (skip rule below), or (c) a subagent returns a blocker that invalidates the whole sprint (`pw_issues.py` cannot reach GitHub, plugin source path wrong, etc.). In those cases, report and stop.

## Issue tracker

Issues live in GitHub Issues on `PinWright/pinwright-ue`. Every read and write goes through the shared helper `scripts/pw_issues.py`, run from the plugin directory (`<host project>/Plugins/PinWright`) as `uv run scripts/pw_issues.py <command>`; `--help` lists the commands and exit codes. Labels, the hidden metadata block, the lease and the severity rubric are described in the plugin `CLAUDE.md` -> **Issue tracker**.

Two rules, no exceptions:
- **Pick work only via `pw_issues.py list`.** It returns only open issues labelled `status/accepted`, ranked for you. Never pick from the web UI, raw `gh issue list` or a search result.
- **Read issue text only via `pw_issues.py show N`.** It withholds bodies and comments whose author is not OWNER, MEMBER or COLLABORATOR. Never fetch an issue with raw `gh`, `curl` or a browser, and never open a link or attachment from a withheld author.

Plugin source: `Plugins/PinWright/Source/PinWright/Private/`

## Role Constraints

You implement **and** verify. There is no review stage: an issue goes from open straight to closed. Close an issue as completed only after Phase 5 has compiled the sprint and run that task's regression test green, and put that evidence in the closing comment. A task that does not verify stays open: release its lease with a comment saying what failed.

## Phase 1: List and Claim Tasks

Run `uv run scripts/pw_issues.py list --json`. It is already in work order (`RICE priority` highest first with unscored issues last, then severity, `costly`, `encounters`, number) and already excludes blocked, harness and live-claimed issues. Select 4–6 tasks from the top, following the priority rules in the plugin `CLAUDE.md` -> **Issue tracker** -> **Priority (RICE)** (batch by area, split an E=3 issue instead of starting it as filed), read each with `pw_issues.py show N`, then claim each one with `pw_issues.py claim N --host <host id>` (a stable id for this checkout, such as the machine name). Exit 3 (`LOST`) means another host holds it: drop it and take the next one. In plan mode, nothing is written before approval: select from `list` now and claim right after `ExitPlanMode`, before Phase 4, dropping any task you lose.

**Closed issues are off-limits.** Closed as completed (DONE) and closed as not planned (WONTFIX) are both terminal: never re-analyze, never re-investigate, never reopen from inside this skill. If an issue was closed as not planned, the user already decided it isn't worth fixing; we respect that decision. The only path back to open is via `mcp-audit` (or an `mcp-review` re-test) when the problem is re-encountered. Sprint never reasons about why something was closed or whether the decision should be revisited.

**Skip reopened tasks:** If `show N --json` reports `state_reason: "reopened"`, the issue was closed once and came back (a regression, or a failed re-test). Skip it: these are complicated edge cases that failed verification and need deeper analysis than a sprint provides. Release its lease if you already claimed it.

**Already implemented:** If a task is clearly already implemented (code matches what the issue asks for) but still open, it needs no code change and no subagent: add it to the Phase 5 verification set with the existing test that covers it, and close it there with a comment naming the code and the test result. Keep this for the obvious cases; anything uncertain goes through Phase 2 like a normal task.

Announce the selected task list (issue numbers + one-line reason per task) and move directly to Phase 2. In plan mode this announcement lands in user-facing text; in autonomous mode it's just a status update. Either way, do not request approval at this step — approval comes later via `ExitPlanMode` (plan mode) or not at all (autonomous).

## Phase 2: Per-Task Analysis Subagents (parallel)

Dispatch **one subagent per selected task**, all in the same tool-call batch so they run in parallel. Each subagent owns validity review + investigation + per-task plan for its single task.

**Each analysis subagent's prompt must include:**

- The issue number, and the instruction to read it with `uv run scripts/pw_issues.py show N` (never paste the issue text into the prompt, never fetch it another way)
- The plugin source root and a pointer to the README conventions
- The four deliverables it must produce in its report (below)
- A reminder that it is **analysis only** — do not edit any files, do not implement

**Each analysis subagent delivers back:**

1. **Validity outcome.** One of:
   - `valid-as-written` — proceed with the task unchanged.
   - `valid-bug-wrong-fix` — the bug is real but the proposed fix is wrong; propose a cleaner fix and the rewrite of the `**Fix:**` paragraph.
   - `reformulate`: same issue, new title/body/type/severity; include the rewritten title and body.
   - `reclassify-ergonomic`: this bug is really an ergonomic issue (error message improvement, discovery method, doc update). Include the new title, the targeted new body and the new `E-*` metadata id; the issue's type becomes Ergonomic, not closed.
   - `wontfix-candidate` — surface a recommendation in the final summary; do not change anything.
   Reporters frequently misuse the MCP or file the symptom rather than the root cause, so the subagent must actively consider these outcomes rather than defaulting to `valid-as-written`.

2. **Investigation findings.** The root cause in prose: which file/handler is involved, what UE API is relevant (grep the engine source under `C:\UE_5.8\Engine\Source\` for engine internals, the plugin source for plugin code), what the correct behavior should be, and any existing utility/pattern to reuse.

3. **Per-task implementation plan (high-level, no code).** Files to modify with line numbers, what changes at each site, why. If the outcome was `valid-bug-wrong-fix`, `reformulate`, or `reclassify-ergonomic`, also include the exact issue edits (`pw_issues.py edit` title/body, `score` changes to type, severity or RICE, `meta --set id=...`, and the comment explaining the change).

4. **Regression test spec.** For every `B-*` (bug) ticket: produce a spec for a regression test that would have failed on the unmodified code and passes after the fix. Fields: test file path under `Source/PinWright/Private/Tests/<SubFolder>/`, `IMPLEMENT_SIMPLE_AUTOMATION_TEST` macro identifier, the assertion(s), and a one-sentence counterfactual — "if the fix in `<file>` is reverted, this assertion fails because `<reason>`." The counterfactual is the bar: if you can't write a true counterfactual sentence, the test isn't a regression test and is rejected in Phase 4.5.

   **When a bug test is not applicable.** Rare, but possible — the bug has no observable output to assert against (e.g., it manifests only as a log warning in the editor, or only in interactive UI). The subagent must explicitly state `"no test — <one-line reason>"` and name the specific quality that makes testing meaningless. "Hard to test", "needs the editor running", or "would require a complex fixture" are not valid reasons — only "nothing observable to assert" is.

   **`E-*` (ergonomic) tickets do not require a regression test.** Ergonomic improvements are docs, error-message wording, parameter-spec tweaks, discoverability, naming — quality-of-life changes that aren't bugs. The subagent may still propose a test if a natural one exists (e.g., an ergonomic rename also tightens a return shape), but absence of a test on an `E-*` ticket is the default and needs no justification.

   **The test is authored alongside the fix and run in Phase 5.** Phase 4 implementers write the test file without compiling it; Phase 5 builds the sprint and runs each task's test, and an issue closes only on a green run. Whether the test would fail on revert is not re-run: the counterfactual sentence is the contract, and reviewers check it statically in Phase 4.5. The fix and the regression test ship together as one diff.

5. **Files this task will touch.** A plain list of absolute-or-repo-relative paths that implementation will modify. This is what the main agent uses to compute conflict groups.

Main agent's job after all analysis subagents return: aggregate their outputs into a single sprint plan document, note all `wontfix-candidate` and `reshape` decisions in a running summary.

**The aggregated analysis must be persisted to a single sprint plan file** — this file becomes the canonical reference that Phase 4.5 verifier subagents read directly. Never re-print plan excerpts into verifier prompts; pass them the path and let them read what's actually there. Single source of truth.

**Plan file location:** Choose the plan directory based on the agent runtime using the skill, not on where this skill file lives. Codex writes sprint plans to `<repo>/.codex/plans/mcp-sprint-<timestamp>.md`. Claude writes sprint plans to `<repo>/.claude/plans/mcp-sprint-<timestamp>.md`. Create the directory if needed. Never write a Codex sprint plan under `.claude/plans`, and never write a Claude sprint plan under `.codex/plans`.

**In plan mode**, the plan file is where the user approves (the only file you can write during plan mode). Fill it with enough per-task detail that the user is approving the real sprint, not a meta-plan. Per task: issue number, validity outcome, files to be touched, one-sentence fix direction, regression test file + identifier + counterfactual sentence, and issue edits for reshape/reclassify. Compute conflict groups (Phase 3), include them in the plan, then `ExitPlanMode`. Post-approval, Phase 4 implements.

**Outside plan mode**, write the plan to the runtime-specific sprint workspace file from the location rule above. Same contents. This is what verifiers read. Don't skip persistence just because no user approval is needed — the verifiers need a stable reference.

## Phase 3: Conflict Grouping

Before dispatching implementation, compute implementation order based on the file lists from Phase 2.

1. Build a graph: nodes are tasks, edges connect any two tasks whose Phase 2 file lists intersect.
2. Each connected component is a **conflict group**. Tasks within the same group share at least one file (transitively) and must be implemented sequentially to avoid edit races.
3. Tasks with no shared files are in singleton groups and can run in parallel with everything else.

Announce the groups before dispatching: e.g. "Group A (sequential): T-101 → T-102 (both touch `BlueprintHandlers.cpp`). Group B (parallel): T-103, T-104, T-105."

## Phase 4: Implementation Subagents

Dispatch **one subagent per task** for the implementation. Scheduling rules:

- **Across groups:** dispatch all singleton groups and all first-task-of-each-multi-task-group in the same batch (parallel).
- **Within a multi-task group:** after the first subagent in the group completes, dispatch the next task in that group. Re-read any files the previous subagent touched before issuing the prompt, so the next subagent has accurate line numbers.
- Keep dispatching until every task is done.

**Each implementation subagent's prompt must include:**

- The specific plugin file(s) to modify with line numbers (from Phase 2)
- What the current code does and what it should do instead
- Key UE API details from the investigation (method names, struct fields, naming patterns)
- The regression test to author (mandatory for `B-*` unless Phase 2 marked it `"no test — <reason>"`; not required for `E-*`): file path, test macro identifier, the exact assertion, and the counterfactual sentence the subagent must echo back. The subagent writes the test in the same diff as the fix and does **not** compile or run anything; validation runs in a separate session. The fix is never gated on the test — author both, move on.
- The issue number for reference only. Implementers never write to issues: the main agent applies reshape edits after Phase 2 and closes issues in Phase 5.
- **Use configured worker defaults.** Follow the environment's delegation policy and worker model/reasoning defaults in every phase unless the user requests an override.

After each implementation subagent reports complete, **do not** rely on a main-agent "read the diff" pass to approve the task — dispatch a dedicated review subagent (Phase 4.5). Fresh eyes with an isolated context catch integrity failures the main agent misses when the implementer's own summary looks plausible.

## Phase 4.5: Sprint-Wide Review (3 simplify-style + 1 spec-reviewer per task)

After **all** implementation subagents have completed, launch the full review battery in a single parallel batch:

- **One spec-compliance reviewer per task** (N agents). Narrow scope: does this task's diff match the Phase 2 analysis, is the regression test valid, are the planned issue edits correct. Gets its task's Phase 2 analysis report verbatim.
- **Three cross-cutting simplify reviewers** (3 agents) looking at the combined sprint diff: reuse, quality, efficiency. Modelled on `/simplify`'s pattern. Get the full `git diff` for the whole sprint, not per task — this is the only way they spot cross-task patterns like "T1 and T4 both added similar string-repair logic that should be unified."

Total reviewers per sprint: **N + 3**, all in parallel. For a 5-task sprint that's 8 concurrent reviewers instead of 15 per-task × 3 axes. The spec reviewer owns the per-task "did we build the right thing" question; the simplify reviewers own the sprint-wide "is what we built clean, reuse-maximising, and efficient" question. Neither is forced to re-verify what the other checked, so each can dig deep in its lane.

Waiting for all implementations before launching means simplify reviewers see the final sprint state in one pass. The wall-time cost is small (implementations were already parallel) and the signal quality is worth it.

**Common context every reviewer gets** (put this in every prompt):
- Path to the sprint plan file (the one Phase 2 persisted) + the git-diff command to run (`git diff <pre-sprint-sha>..HEAD` or `git diff HEAD`)
- Instruction: report **APPROVED** or **ISSUES:** + bulleted list (file:line + what's wrong + what should be there). Reviewer does **not** fix anything.
- **Reviewers open the plan file and read it themselves.** Never re-print plan contents into the prompt — that introduces paraphrase drift between what the main agent thinks the spec says and what's actually in the file. The plan file is the single source of truth; the reviewer reads the authoritative version. Same for issues: pass the issue number, let the reviewer read it with `pw_issues.py show N`.
- Per-task reviewers additionally get: the issue number, the plan-file section heading for this task (so they jump straight to the right part), and the list of files the implementer reported touching.

### Per-task: Spec & Correctness Reviewer (dispatched once per task)

Verifies the implementation faithfully executes the Phase 2 plan for **this one task**, and that the regression test actually guards the fix.

1. Every file in the Phase 2 file list was touched; no unlisted file was touched. Drive-by changes are a reject.
2. For `B-*` tasks: the regression test file exists at the path from Phase 2, uses the correct `IMPLEMENT_SIMPLE_AUTOMATION_TEST` identifier, and sits under `Source/PinWright/Private/Tests/<SubFolder>/`. The only exception is a Phase 2 entry of the form `"no test — <reason>"`; in that case the reviewer verifies the reason is genuine (no observable output to assert against) and not a hand-wave like "hard to test" or "needs the editor running" — reject those and demand a real test. For `E-*` tasks: a test is not required; if one was authored, sanity-check it but don't reject the task for missing it.
3. **The test calls production code**, not an inline copy of the fix. If the fix helper is `static` (file-local), the test cannot reach it — reject and demand the helper be promoted to namespace-scope with a header declaration. A test that reimplements the fix as an inline lambda/helper and exercises the copy is always rejected: its counterfactual is false because reverting the production code doesn't touch the copy. This failure mode is the #1 reason this phase exists.
4. The counterfactual the implementer echoed back actually holds: mentally revert the production fix and trace the test's assertions — does at least one assertion fail purely because of the revert? If not, reject with the specific assertion that would still pass.
5. Issue: still open (nothing closes before Phase 5). For reshape/reclassify tasks: the title, body, labels and metadata id match the Phase 2 plan, and a comment explains the change.

### Sprint-wide: Reuse Reviewer (single agent, whole sprint diff)

"Did we reinvent something already in the plugin?" — the plugin has a large Utils tree and reinvention is a common implementer mistake.

1. Search the plugin for existing utilities that could replace newly written code. Common homes: `Private/Utils/` (`AssetUtils`, `PropertyUtils`, `JsonUtils`, `PathUtils`, `ClassUtils`, `LogUtils`, `ActorUtils`), `Private/Handlers/<Domain>/*HandlerUtils.cpp`, `PinWrightHelpers.h`.
2. For blueprint changes: check `BlueprintHandlerUtils.h/.cpp` for cascade/delegate/graph utilities before accepting new node-walking code.
3. For BPIR changes: check existing opcode handlers in `Private/Compiler/` before accepting new parse or emit logic — the dispatcher/bind_dispatcher pair is the canonical pattern for delegate-wiring features.
4. Flag any new function that duplicates an existing one; name the existing one with its file:line.
5. **Cross-task reuse**: if two tasks in this sprint added similar helpers (e.g., both repair a message, both walk a custom-event chain), flag it and propose the shared extraction. This is the unique value of the sprint-wide view.

### Sprint-wide: Quality Reviewer (single agent, whole sprint diff)

Hacky-pattern sweep, same axes as `/simplify`'s quality pass, adapted to this codebase:

1. **Redundant state / cached values** that could be derived from existing state.
2. **Parameter sprawl** — new params or boolean flags that splinter behavior instead of restructuring.
3. **Copy-paste with slight variation** — near-duplicate code blocks where a shared helper would serve (within a task or across tasks).
4. **Stringly-typed code** — raw strings where the plugin already has an enum, constant, or `FName` pool (handler method names, error codes, etc).
5. **Leaky abstractions** — exposing internal plugin state (e.g., `FPluginState` internals) where a narrower surface exists.
6. **Nested conditionals** — ternary chains or if/else trees 3+ deep that should flatten to early returns or a lookup.
7. **Unnecessary comments** — comments narrating what well-named code already says, or referencing the ticket/session (rots fast). Keep only non-obvious **why**: hidden constraints, subtle invariants, engine-bug workarounds. The plugin's CLAUDE.md is explicit: no "WHAT" comments.
8. **UE-idiom misuse** — missing `FScopedTransaction` on mutating paths, raw pointers where `TWeakObjectPtr` is the convention, manual string manipulation where `FPaths`/`FPackageName` already has a helper, ad-hoc JSON construction where `FJsonObject` shortcuts exist.

### Sprint-wide: Efficiency Reviewer (single agent, whole sprint diff)

Waste sweep on the new code:

1. **Unnecessary work on handler hot paths** — redundant `FindObject` calls, re-parsing JSON already available via `FHandlerContext` getters, rebuilding maps the dispatcher already caches.
2. **Missed concurrency within implementation code** — independent operations run sequentially where parallel would be safe. (Do not flag the dispatcher's reentrancy guard — that's intentional.)
3. **Unbounded data structures or missing cleanup** — TArrays that grow per-request without bounding, delegate bindings without matching unbind.
4. **Overly broad operations** — full `AssetRegistry` scans where a filtered query exists, loading entire `UBlueprint` subobjects when a single property is needed, full-file reads when a single line is needed.

### Handling review outcomes

Wait for all N+3 reviewers. Bucket their findings by task (per-task spec reports are already bucketed; simplify findings get routed to the task owning the file they touched — cross-cutting findings go to whichever task introduced the pattern first, with a note about the other task).

- **No ISSUES across the batch** — sprint complete. Proceed to final summary.
- **Any ISSUES** — dispatch **one fix subagent per affected task** (using configured worker defaults), giving it all reviewers' complaints that touched its files bundled verbatim, plus the original implementer prompt. After fix subagents complete, re-run the full N+3 review battery on the new sprint diff. Cap at 2 fix iterations; on the third, escalate to the user.

Some reviewer findings will be false positives — a pattern that's actually the right call here, or a "reuse" suggestion that would add more complexity than it saves. Fix-subagent prompts should instruct: address each finding directly OR note briefly why it's skipping one; do not argue. Same rule as `/simplify` Phase 3.

Do not ask the user between review and fix cycles — auto mode keeps the loop going.

Reviewers do not compile or run anything. They reason about whether tests **would** fail on revert; Phase 5 runs the tests once, after the review battery is clean.

## Phase 5: Compile, Verify, Close

After Phase 4.5 is clean, build once and verify every task before any issue closes:

1. Build the plugin with `editor_build` (see `CLAUDE.md` -> **Building**). Route compile errors back to the implementing task's agent, then re-run the build.
2. Run each task's regression test by name, not the full suite: `editor_run_tests` with the test's `filter`, a `reason` and `mode: "offscreen"`, then poll `editor_test_status` until it reports the `check_suite_log` verdict. Tasks with no test (an `E-*` without one, or a genuine `"no test"`) verify by the live observation the plan named, such as a wiki page or an error message read back over MCP.
3. For each task that verified, close it: `pw_issues.py close N --reason completed --host <host id> --body-file <comment>` with the comment format below. For each task that did not, `pw_issues.py release N --host <host id> --note "<what failed, with the log path>"`; it stays open.

Common compile issues:
- Private UE methods — replicate using public API equivalents
- Missing includes — add `#include` for types used (especially `EdGraph/EdGraph.h`, `UObject/UnrealType.h`)
- Feature-flag guards — use `__has_include` pattern for optional UE headers

## Closing Comment Format

Each verified task is closed as completed with one comment holding:

- What changed: files, and why that fixes the reported problem.
- The regression test: file, `IMPLEMENT_SIMPLE_AUTOMATION_TEST` identifier, and the counterfactual sentence.
- The evidence: the build result, the test run's `check_suite_log` verdict and its log path (or the live observation for a task with no test), and the commit sha if the sprint was committed.

Comments are the issue's history: never edit or delete an earlier one, just add a new one.

For reshaped / reclassified tasks (applied by the main agent after Phase 2, before implementation):
- valid-bug-wrong-fix: `pw_issues.py edit N --body-file <body>` with the rewritten `**Fix:**` paragraph, plus a comment naming the replaced approach.
- reformulate: `edit N --title ... --body-file ...` and `score N --severity <s>` as planned (with `--rice R,I,C,E` too if the investigation changed a factor, `--type T` if the type changed), plus a comment saying why.
- reclassify-ergonomic: `score N --type ergonomic`, `meta N --set id=E-<slug>`, `edit` with the new title and body, plus a comment saying why.

## Final Summary

After every task has cleared Phase 4.5 review (APPROVED or escalated), emit a concise summary to the user:
- Tasks implemented (id + one-line what-changed).
- Tasks reshaped or reclassified (id + new id if applicable + reason).
- Tasks surfaced as WONTFIX candidates (id + reason) — the user decides.
- Tasks left open after a failed verification (issue number + what failed).
- Optionally suggest `mcp-review` to re-test the issues this sprint closed.

## Phase 6: Affected documentation

When the requested changes invalidate existing wiki claims, use `wiki-sync` for those claims before the final response. Skip it when no documentation is invalidated, unless the user explicitly requested knowledge capture. Report unrelated documentation issues without expanding the sprint.

## Key Patterns

**Handler file pattern** — new handlers go in `Private/Handlers/<Domain>/`, use `REGISTER_RPC_HANDLER` macro, no header needed.

**Feature-flag includes** — for optional UE headers:
```
#if __has_include("SomeHeader.h")
#include "SomeHeader.h"
#define MCP_HAS_FEATURE 1
#else
#define MCP_HAS_FEATURE 0
#endif
```

**Log categories** — utility files use `DEFINE_LOG_CATEGORY_STATIC(LogSomething, Log, All)`, not subsystem categories.

**FScopedTransaction** — wrap mutating operations for undo support. Read-only handlers don't need it.

**Private UE methods** — if a UE method is private (like `IsDelegateValid`), replicate it using public APIs rather than calling it directly. Always verify accessibility before assuming.
