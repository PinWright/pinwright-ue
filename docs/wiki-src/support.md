# support

Help the user report a plugin **bug / critical gap** or request a **feature** by drafting a high-quality report and opening a prefilled GitHub draft — never submit it yourself. Use this when an RPC fails in a way you cannot work around, or when the user explicitly wants to request a feature.

## Where reports go

- **Bug or critical gap in an existing feature** → an Issue at `https://github.com/PinWright/pinwright-ue/issues/new?template=bug_report.yml`
- **New or extra feature idea** → an Issue at `https://github.com/PinWright/pinwright-ue/issues/new`

Source and issues live at `https://github.com/PinWright/pinwright-ue`.

## Before drafting

1. Read the plugin version: `VersionName` from `PinWright.uplugin` in the project's `Plugins/` tree. Take the Unreal Engine version, OS, and build target from your environment. Don't fabricate any of it.
2. Check for duplicates: `gh search issues "<keywords>" --repo PinWright/pinwright-ue` (search open **and** closed; if `gh` is unavailable, WebFetch the issues search). If a match exists, offer to comment on it instead of opening a duplicate.

## Write the report — facts only

Separate **observed facts** from **hypothesis**, and never blur them. This is the line between a useful report and the "AI slop" maintainers ban.

- **Observed (above the line):** the verbatim `call()` invocation(s) with method + args, the actual response/error, and what you expected. Only things that actually happened this session.
- **Suspected cause (below the line, labeled "may be wrong"):** your root-cause guess. Never present a guess as fact — a confident wrong cause wastes the maintainer's time.
- **Never fabricate.** Do not invent method names, file paths, log lines, or stack traces. If you didn't observe it, don't write it.
- **A real reproduction is mandatory.** Give the smallest call sequence you actually ran. If you could not reproduce it, say so plainly and don't file it as confirmed.

Bug body = minimal repro, expected, actual, environment. Feature body = the task you were doing, the specific missing capability, the workaround you tried and why it fell short, and an optional proposed API marked as a suggestion.

## Open the draft — only with approval

Bug-form field ids, all text, prefill via the query string: `version`, `engine`, `os`, `repro`, `expected`, `actual`, `notes`, plus `title` (write a real summary — no `[Bug]:` prefix). Type and severity are **labels**, not form fields (GitHub can't prefill dropdowns), so set them with the `labels=` param — which **replaces** the form's default labels, so list every label you want. Always include `type/bug` (or `type/critical-gap`), `status/needs-triage`, and `ai-assisted`; you may also suggest a severity, e.g. `severity/s3-medium`. So append `&labels=type/bug,status/needs-triage,ai-assisted` (keep the label values literal — don't URL-encode the commas or slashes).

- **Disclosure** is the `ai-assisted` label — that's enough. Don't prefill a "drafted by AI" sentence into `notes`; leave `notes` empty unless there's genuinely useful extra context.
- **Long body** → copy the full markdown to the clipboard (`Set-Clipboard`) and leave a `paste from clipboard (Ctrl+V)` placeholder in the prefilled field, to dodge the URL length limit.

**Two hard rules — never break them:**

1. **Never open the prefill page or write the clipboard without explicit user approval.** Show the drafted report first and ask for the specific action ("copy this to your clipboard and open the draft?"). "File a bug" is not by itself approval to open — confirm.
2. **Never submit.** You open a prefilled *draft* only; the user reviews the rendered GitHub page and clicks Submit. Do not use `gh issue create` — it submits immediately.

**Preferred path: open it in the user's browser.** On approval, `Start-Process "<url>"` (Windows) so the user only has to review and submit. Don't make the user copy a raw link out of chat — print the link only as a fallback when you can't open a browser (headless/remote session).
