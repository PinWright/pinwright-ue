# Compile-fix subagent protocol

You fix compile errors for **one source-file cluster** (a single `.cpp`, a single `.h`, or a paired `.cpp`+`.h` of the same name in the same directory). The orchestrator gave you the cluster's file paths, the error lines verbatim, the path to the compile log for additional context, the current cycle number, and the path to this protocol. Follow it end to end without checking back in until you have a fix or a SKIP.

## What you receive

- One or two source file paths, preferably relative to `<PLUGIN_ROOT>` or `<PROJECT_ROOT>`.
- The error lines verbatim in your prompt — one line per compiler diagnostic.
- The compile log path, normally passed as `<UBT_LOG>` by the orchestrator, if you need surrounding context (rare; the listed errors usually suffice). Each cl.exe stderr line is prefixed with ` Compile [x64] <file.cpp>: ` — strip that prefix to get the original diagnostic.

## Step 1 — Read your errors

The error lines are already in your prompt. They look like:

```
FooHandler.cpp(42): error C2065: 'UndeclaredThing': undeclared identifier
FooHandler.cpp(58): error C2061: syntax error: identifier 'BarParam'
```

Group them into **distinct error sites** (one per source line). Two errors at the same line/column referencing the same identifier are one site, not two.

## Step 2 — Read the source

For each error site, read the file at the reported line ± 20 lines. Always re-read; never trust the line number without seeing the surrounding code, because a prior fix in this cycle may have shifted things and the log captured an older state.

If the error is in a `.cpp` and references a symbol declared in the matching `.h`, read the header too. Same for the reverse.

## Step 3 — Classify

Common patterns and the canonical fix for each:

| Symptom | Likely cause | Canonical fix |
|---|---|---|
| `C2065: 'X': undeclared identifier` on a UE type | Missing `#include` for the type's owning header | Add the include, ordered after `CoreMinimal.h` and any `.generated.h` rules. |
| `C2061/C2062: syntax error: identifier 'X'` near a function param | Forward declaration insufficient, or include order issue | Replace forward decl with full include; or hoist include above the offending block. |
| `C2039: 'Member': is not a member of 'X'` | UE API renamed/moved between minor versions | Search for the new spelling in the engine source (`C:\UE_<ver>\Engine\Source\`); swap the call site. Don't fall back to private API. |
| `C2440/C2664: cannot convert 'A' to 'B'` | Signature drift in a UE delegate, getter, etc. | Read the new signature in engine headers; update the call site to match. |
| `LNK2019: unresolved external symbol` | Missing module dependency in `Build.cs`, OR a function declared but not defined | Check `Build.cs` first; only add a module dependency if the symbol genuinely belongs to another module. If the function is supposed to be defined in our cluster, add the missing definition. |
| `C2027: use of undefined type 'X'` | Pointer/reference compiles fine but the dereference needs a full type | Add the include that defines `X`. |
| `C1060: compiler is out of heap space` | Build flags issue, not source | This is not a source-fix. SKIP and report — the orchestrator will surface it. |
| ODR / multiple-definition errors | Almost always a Unity-build collision | Plugin already disables Unity (`bUseUnity = false`). If you see this anyway, SKIP — the build config drifted. |

UE 5.6 specifics worth keeping in mind: many former private methods have public equivalents (e.g. some `UEdGraphSchema::*` helpers became namespaced free functions). When tempted to access a private UE symbol, look for a public equivalent first; don't friend-class your way around UE's accessibility.

## Step 4 — Edit

One conservative edit per error site. Rules:

- Every changed line must trace to an error in your error list. No drive-by reformatting, no "while I'm here" tweaks.
- Match the file's existing style — indentation (4 spaces, no tabs per project convention), brace style, include ordering.
- Don't add `#include` for headers that aren't actually used by the new code.
- Don't disable warnings. If the project treats a warning as an error, fix the cause; don't silence it.
- Don't stub out a function with `return {};` to make a linker error go away — that turns a compile error into a silent runtime bug.

## Step 5 — Self-verify

Before reporting, re-read the file(s) you edited. Check:

1. Every error in your list now has a plausible fix at its line.
2. You introduced no new compile-visible issues (mismatched braces, dangling `#if`, etc.).
3. You didn't touch a file outside your cluster.

## Step 6 — Report

Reply to the orchestrator with three bullets, no diff, no preamble:

```
- Files touched: <paths>
- Errors addressed: <count> across <distinct sites>
- Skipped: <none | one-line per skipped error with reason>
```

If you SKIPped any error, name it specifically and say why. The orchestrator counts skipped errors against the inner-loop cap; nothing-but-skips means the skill escalates to the user.

## Hard limits

- Don't disable warnings-as-errors or add `#pragma warning(disable: ...)`. If a warning is genuinely wrong on this site, name the warning ID and SKIP — surface it for human judgment.
- Don't `#define` private names in foreign translation units.
- Don't stub functions to satisfy the linker. Either define them properly or SKIP.
- Don't modify `Build.cs` unless your error is unambiguously a missing module dependency, and you can name the symbol and the module that owns it.
- Don't touch files outside your cluster, even if they look obviously buggy. Other clusters have their own subagents; cross-talk causes lost edits.
