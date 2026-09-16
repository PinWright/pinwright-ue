# Test-fix subagent protocol

You fix failing tests for **one test source unit** — a single `.cpp` under `Plugins/PinWright/Source/PinWright/Private/Tests/...`, plus its same-named `.h` in the same directory if one exists. The orchestrator gave you the unit's file paths, the list of failing test class names from that unit, the test log path, the cycle number, and the path to this protocol. Follow it end to end.

Most of the time the right fix is in **production code**, not in your test file. The default project rule (`feedback_tests_match_correct_behavior`) is: tests express what the code should do — when they fail, the production code is more often wrong than the test. Don't reflexively soften an assertion to make it green.

## What you receive

- Test unit file path(s), preferably relative to `<PLUGIN_ROOT>` or `<PROJECT_ROOT>`.
- A list of failing test class names (e.g. `FBpirCompilePrivateVarErrorMessageTest`, `FBlueprintHandlersDecompileTest`). Each is the first arg of an `IMPLEMENT_SIMPLE_AUTOMATION_TEST` macro in your `.cpp`.
- The test log path, normally passed as `<HOST_LOG>` by the orchestrator.
- The cycle number (mostly informational).

## Step 1 — Read your test file(s)

Open your `.cpp` and locate each failing test by its class name. Read the full `IMPLEMENT_SIMPLE_AUTOMATION_TEST` block plus the matching `RunTest` body — that is what actually executes. Note exactly what each test:

- Sets up (asset paths, RPC inputs, mock state)
- Calls (which production handler, util, RPC method)
- Asserts (`TestEqual`, `TestTrue`, `TestNotNull`, `TestStringContains`, etc. — and the expected values)

If a header sits next to your `.cpp` and the test uses fixture types or helper functions from it, read the header too.

## Step 2 — Read the test log for these tests

Pull the per-test failure messages from the test log path passed by the orchestrator. The relevant region for each failing test is between `BeginEvents: <FullTestPath>` and the next `EndEvents:` (or until `Result={Fail}`). The error lines inside that region are the actual reason the assertion fired — e.g. `Expected '<X>' but got '<Y>'`, `Expected non-null pointer`, `Test failed: <message passed to TestTrue>`.

Use Claude Grep with `-B 30 -A 0` around each `Result={Fail}` line for your test names, or read the log between known offsets. In Codex, use the equivalent `rg -n -B 30 -A 0 "<test name>|Result=\\{Fail\\}" "<HOST_LOG>"` pattern, or `Select-String -Context 30,0` if `rg` is unavailable.

## Step 3 — Read the production code under test

For each failing test, identify the production code path it exercises:

- An RPC handler in `Source/PinWright/Private/Handlers/<Domain>/<Method>Handler.cpp`
- A util function in `Private/Utils/<Whatever>Utils.cpp`
- A BPIR pipeline component in `Private/Compiler/...`
- A widget XML import/export pass in `Private/Widget/...`

Read the production code that matches the assertion. If multiple production files are plausibly involved, read all of them — undersized investigation produces undersized fixes.

For UE engine internals (rare but happens), grep the engine source under `C:\UE_<ver>\Engine\Source\`.

## Step 4 — Form a hypothesis

Two outcomes are possible per failing test:

**(a) Production-bug.** The test's assertion is correct; the production code is wrong. Default to this.

**(b) Test-wrong.** The test's assertion contradicts a documented behavior, an issue board decision, or another currently-passing test. You can only choose this with a concrete pointer — name the doc, the ticket id, or the contradicting test that justifies the call. "It would be easier to make the test green by softening the assertion" is **not** a concrete pointer; that is the failure mode the project rule was written to prevent.

Edge case: the test may have been valid when written but is now testing legacy behavior the code intentionally moved away from. Even here, hypothesis (b) requires a **named** signal — a board ticket, a doc page, a PR or commit message that documented the intentional change.

If you have multiple failing tests and they suggest different hypotheses (some prod-bug, some test-wrong), that's fine — categorize each individually.

## Step 5 — Edit

Pick your touch list — your test unit file(s) plus any production files the fix requires — and edit only those. Rules:

**For production-bug fixes:**
- Find the smallest correct fix that makes the assertion pass for the right reason. "For the right reason" means: the same fix would also make a written-but-not-yet-existing similar test pass.
- Reuse existing utilities — the plugin's `Private/Utils/` tree is large, and reinvention is the most common cross-task waste pattern. Look for existing helpers in `AssetUtils`, `PropertyUtils`, `JsonUtils`, `PathUtils`, `ClassUtils`, `LogUtils`, `ActorUtils`, `BlueprintHandlerUtils`, before writing new code.
- Match plugin conventions (handler pattern, REGISTER_RPC_HANDLER macro, FHandlerContext getters, etc.).

**For test-wrong fixes:**
- Make the smallest change that aligns the test with the **documented** correct behavior. Cite the pointer in a comment if and only if the comment explains a non-obvious WHY (e.g. "Test was written before B-123 reclassified the contract as ergonomic-only"). Otherwise no comment.
- Don't expand the test's coverage in the same edit. If the test now over- or under-tests the right behavior, fix only the failing assertion in this pass; raise the broader gap in your final report.

**Both kinds:**
- Don't touch a file outside your test unit and the production code it exercises. Other units' subagents are running in parallel and overlapping edits will lose changes.
- Don't reformat unrelated code, don't reorder includes, don't rename things you didn't have to rename.
- Project style: 4 spaces, no tabs; UE prefixes (`U`, `A`, `F`, `E`, `I`); `b` prefix on bools; comments only for non-obvious WHY.

## Step 6 — Self-verify

Before reporting, mentally re-run each failing test against your edits:

1. Walk the test body line by line. After your fix, would each `TestEqual`/`TestTrue`/etc. pass?
2. Did you fix it for the right reason — or did you accidentally widen the production behavior in a way that makes this test pass while breaking another assertion (in this file or elsewhere)?
3. If you fixed production code, are there other call sites that depend on the old behavior? Grep for the symbol you changed; if other call sites exist, are they covered by other tests?

If a check raises doubt, name the doubt in your report. Don't bury it.

## Step 7 — Report

Three sentences max, no diff:

```
Fixed: <list of failing test names you addressed>
Touched: <abs file paths>
How: <one short clause per failing test — "prod fix in FooHandler.cpp: <root cause>" or "test fix per <doc/ticket>: <what the test now expects>">
Skipped: <none | one line per skipped test with the specific reason>
```

If you SKIPped any test, say which one and why with enough detail that the orchestrator's final summary lets the user act. "Test asserts behavior the production code intentionally removed in B-247; the test should be retired or rewritten" is actionable. "Couldn't figure it out" is not.

## Hard limits — non-negotiable

- **Do not delete a failing test.** Even if you think it's worthless. Surface it as a SKIP with reasoning.
- **Do not comment out the assertion.** Same rule.
- **Do not add `bSkipped = true` or any equivalent runtime suppression** to a `Run` body to make a failing test pass.
- **Do not weaken an assertion** so it can no longer fail (e.g. change `TestEqual(Got, "expected")` to `TestNotEmpty(Got)` just because the value drifted).
- **Do not change a test from FAIL to SKIP-style** by short-circuiting `RunTest` to return `true` early.
- **Do not silence with conditionals** (`#if 0`, `if (false) ...`, runtime flags).
- **Do not touch files outside your test unit and the production code it exercises.** Other units' subagents are running in parallel and overlapping edits will lose changes; the next compile cycle catches collisions.

## Why these rules exist

The whole point of the test loop is to drive code to a green state where green **means something**. Every shortcut above turns a real failure into a green checkmark that lies — and that is worse than leaving the failure visible, because the next person reading the suite will trust the lie. The user has explicit feedback rules against this; SKIP plus a clear written reason is always the better option than a silent suppression.
