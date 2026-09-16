# Test-fix protocol

Guidance for fixing failing PinWright automation tests. A test unit is a single `.cpp` under
`Plugins/PinWright/Source/PinWright/Private/Tests/...`, plus its same-named `.h` in the same directory if one
exists.

Most of the time the right fix is in **production code**, not in the test file. The default project rule
(`feedback_tests_match_correct_behavior`) is: tests express what the code should do — when they fail, the
production code is more often wrong than the test. Don't reflexively soften an assertion to make it green.

## What you have

- The failing test class names (each is the first arg of an `IMPLEMENT_*_AUTOMATION_TEST` macro in a `.cpp`).
- The test log path (the host project's `Saved/Logs/<host>.log`; UE rotates the prior run to a
  `-backup-<timestamp>.log`).

## Step 1 — Read the test

Locate each failing test by its class name. Read the full macro block plus the matching `RunTest` body — that
is what actually executes. Note what each test:

- Sets up (asset paths, RPC inputs, mock state)
- Calls (which production handler, util, RPC method)
- Asserts (`TestEqual`, `TestTrue`, `TestNotNull`, `TestStringContains`, etc. — and the expected values)

If a header sits next to the `.cpp` and the test uses fixture types or helper functions from it, read the
header too.

## Step 2 — Read the test log for these tests

Pull the per-test failure messages from the test log. The relevant region for each failing test is between
`BeginEvents: <FullTestPath>` and the next `EndEvents:` (or until `Result={Fail}`). The error lines inside
that region are the actual reason the assertion fired — e.g. `Expected '<X>' but got '<Y>'`,
`Expected non-null pointer`, `Test failed: <message passed to TestTrue>`. Grep with `-B 30 -A 0` around each
`Result={Fail}` line for the test name.

## Step 3 — Read the production code under test

For each failing test, identify the production code path it exercises:

- An RPC handler in `Source/PinWright/Private/Handlers/<Domain>/...`
- A util function in `Private/Utils/<Whatever>Utils.cpp`
- A BPIR pipeline component in `Private/Compiler/...`
- A widget XML import/export pass in `Private/Widget/...`

Read the production code that matches the assertion. If multiple production files are plausibly involved,
read all of them — undersized investigation produces undersized fixes. For UE engine internals, grep the engine
source under `C:\UE_<ver>\Engine\Source\`.

## Step 4 — Form a hypothesis

Two outcomes are possible per failing test:

**(a) Production-bug.** The test's assertion is correct; the production code is wrong. Default to this.

**(b) Test-wrong.** The test's assertion contradicts a documented behavior, an issue board decision, or
another currently-passing test. You can only choose this with a concrete pointer — name the doc, the ticket
id, or the contradicting test that justifies the call. "It would be easier to make the test green by
softening the assertion" is **not** a concrete pointer.

Cross-version note: a test that passes on one matrix version but fails on another usually means a
version-compat gap in the production code, not a wrong test. Fix the production path (version-guarded), not
the assertion.

## Step 5 — Edit

Edit only your test unit file(s) plus the production files the fix requires.

**For production-bug fixes:**
- Find the smallest correct fix that makes the assertion pass for the right reason.
- Reuse existing utilities — the plugin's `Private/Utils/` tree is large (`AssetUtils`, `PropertyUtils`,
  `JsonUtils`, `PathUtils`, `ClassUtils`, `LogUtils`, `ActorUtils`, ...). Reinvention is the most common
  cross-task waste pattern.
- Match plugin conventions (handler pattern, `REGISTER_RPC_HANDLER` macro, `FHandlerContext` getters).

**For test-wrong fixes:**
- Make the smallest change that aligns the test with the documented correct behavior. Comment only when it
  explains a non-obvious WHY.
- Don't expand the test's coverage in the same edit.

**Both kinds:**
- Project style: 4 spaces, no tabs; UE prefixes (`U`, `A`, `F`, `E`, `I`); `b` prefix on bools; comments only
  for non-obvious WHY.

## Step 6 — Self-verify

1. Walk each failing test body line by line. After your fix, would each assertion pass?
2. Did you fix it for the right reason — or accidentally widen production behavior in a way that makes this
   test pass while breaking another assertion (here or elsewhere)?
3. If you fixed production code, are there other call sites that depend on the old behavior? Grep the symbol;
   confirm the matrix's other versions are not regressed by an unguarded change.

## Hard limits — non-negotiable

- **Do not delete a failing test.** Surface it as a SKIP with reasoning.
- **Do not comment out the assertion.** Same rule.
- **Do not add `bSkipped = true`** or any equivalent runtime suppression to make a failing test pass.
- **Do not weaken an assertion** so it can no longer fail.
- **Do not change a test from FAIL to SKIP-style** by short-circuiting `RunTest` to return `true` early.
- **Do not silence with conditionals** (`#if 0`, `if (false) ...`, runtime flags).

## Why these rules exist

The whole point of the loop is to drive code to a green state where green **means something**. Every shortcut
above turns a real failure into a green checkmark that lies — worse than leaving the failure visible, because
the next person reading the suite will trust the lie. SKIP plus a clear written reason is always better than a
silent suppression.
