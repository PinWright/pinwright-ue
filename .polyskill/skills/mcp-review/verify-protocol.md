# Verifier subagent protocol

You verify ONE IN-REVIEW ticket on the PinWright MCP issue board. The main agent gave you a ticket file path. Read this protocol once, then run the loop end-to-end. Do not loop back to the main agent — you own the ticket from "read it" through "edit it."

## How to reach the editor

Use the `mcp__pinwright__call` MCP tool. If it isn't in your tool list, load its schema first via `ToolSearch` with `query: "select:mcp__pinwright__call"`.

The tool routes to the editor automatically — you do **not** need to know the port. Do **not** probe with `curl`, `netstat`, `Test-NetConnection`, or any raw HTTP request.

The only valid "editor unreachable" signal is the MCP tool itself returning a transport error (not an `UNKNOWN_ACTION` or domain error from a successful round-trip). If that happens, SKIP.

## Source code is not verification

This skill exercises *behavior*, not diffs. Reading the C++ source, the test source, or the generated wiki to confirm "the code matches what the developer claimed" is **never PASS** — that's re-reading the developer's own work and trusting them, which is exactly what verification exists to avoid.

Source review is fine as input to your test design (it can tell you what to look for in the response), but the PASS/FAIL decision must rest on a live observation: an MCP response, a sidecar file on disk produced by a fresh `asset.dump`, or — for fixes whose entire surface area is a file deletion or a doc edit — the file-system / doc state itself.

If you cannot exercise the fix end-to-end and the ticket's surface is not "file/doc state", you SKIP. Do not paper-PASS from source inspection.

## Step 1: Read your ticket

Read the file. Note from the body and frontmatter:
- The bug or feature being fixed (title + body)
- The latest `IN-REVIEW` history bullet — the developer's claim about *what was changed and where*
- Any "Repro" / "Test" / "Acceptance" hints in the body — these usually name a specific asset path or symptom string

The latest IN-REVIEW entry is your primary contract. The body gives context; the IN-REVIEW entry tells you what the fix is supposed to do.

## Step 2: Design a minimal test

Pick the smallest live MCP call sequence that proves or disproves the fix. Prefer one call. Cap yourself at six.

Common shapes:

| Fix shape | Test shape |
|---|---|
| BPIR decompile emits a new form | `mcp__pinwright__call` path="blueprint.decompile" args={"assetPath":"<asset from Repro>"}; grep returned BPIR for the new form's presence and the old form's absence |
| BPIR compile accepts new syntax | Create temp BP at `/Game/PinWrightTests/W_McpVerifyTemp_<ticket-id>`; `compile_bpir` with the new syntax; check `compiled`, `errors`, `nodeCount`; delete temp BP at end |
| Asset-dump emits a new aspect file | `asset.dump` on a representative asset; read the resulting file under the dump root (default `<host project>/Saved/PinWright/asset-dumps`) at `<mirror>/<expected-file>` |
| Widget XML well-formedness | `widget.export_xml` on the asset from Repro; check the returned XML for the offending pattern |
| Schema/parameter exposed | Call the wiki page with `call("namespace.method")` (no args); check the schema; or call the method with the new parameter and confirm no `INVALID_PARAMS` |
| New RPC method exists | Call its wiki page; then call it with realistic args and check response shape |

If a referenced asset doesn't exist, use `asset.search` to find a substitute that exhibits the same shape. If nothing fits, SKIP.

**Hard limits:**
- Max 6 MCP calls.
- Never run the PinWright automated test suite (`system.run_tests` with no test name) as part of verification — it locks the editor for minutes and tests different things.
- Never restart the editor.
- If a test needs a temp BP, name it `/Game/PinWrightTests/W_McpVerifyTemp_<ticket-id>` and delete it at the end (`asset.delete`).

## Step 3: Decide PASS / FAIL / SKIP / CRASH

| Outcome | Means | Frontmatter | History label |
|---|---|---|---|
| **PASS** | A live observation (MCP response, dumped file on disk, file deletion, doc content) confirms the fix works as the IN-REVIEW entry described. Source review alone is never PASS — see "Source code is not verification" above. | `status: DONE` | `` `DONE` tester `` |
| **FAIL** | Test confirms the fix does NOT work, OR the new behavior is partially missing (e.g., one of two repro cases still broken). | `status: OPEN` | `` `OPEN` tester `` |
| **SKIP** | Fix can't be exercised via MCP (MCP tool returned a transport error, the fix needs runtime conditions you can't set up, or no representative asset exists). Source-only inspection of a behavioral fix is also SKIP, not PASS. | leave `status: IN-REVIEW` | `` `SKIP` tester `` |
| **CRASH** | Editor crashed during the test. | `status: OPEN`, optionally bump `severity:` if the crash is a new symptom | `` `OPEN` tester `` with the crash details |

Be specific in the result. "PASS" with no detail is useless on a re-read. Capture the MCP call you ran, the field you checked, and the value you observed.

If your test passes on one repro asset but a second asset (one you sampled to be safe) still shows the symptom, that's FAIL. Don't paper over partial fixes.

## Step 4: Update the ticket

Edit the same ticket file with the Edit tool.

**Frontmatter:** change `status:` per the table above.

**History bullet:** find the current highest `#N` in the `## History` section (look at the last bullet — `#3-something` means N=3, your bullet is `#4-...`). Append exactly one new bullet at the end of the section, no blank line before it:

```
- `#{N+1}-{slug}` `STATUS` tester — {body}
```

Where:
- `{slug}` is a short kebab-case descriptor (2–5 words) of *this transition*. Examples: `verify-fix`, `returned-still-fails-on-playsound`, `skip-needs-runtime-state`, `crash-during-decompile`.
- `STATUS` is `DONE` / `OPEN` / `SKIP`.
- `{body}`:
  - PASS: `Verified: {what you ran, observed values}`
  - FAIL: `Returned: {actual vs expected}. Test: {exact MCP call}.`
  - SKIP: `{reason}`
  - CRASH: `Crashed: {symptom, stack trace excerpt if visible}`

The leading backtick + `#N-slug` + trailing backtick is required — that prefix guarantees the bullet is unique so the Edit tool's append lands at the true end of the section.

Use absolute Windows paths (`C:/...`) for all file operations — there's a known Claude Code file-tool bug with mixed/relative paths.

## Step 5: Report back

Three sentences max:
1. What you tested (asset, MCP call).
2. The result (PASS/FAIL/SKIP/CRASH + one-line reason).
3. The file edits made (frontmatter status, history bullet `#N-slug`).

Do not include the full MCP response or the full ticket text in your report — those are in the editor logs and the ticket file. Keep the report tight.
