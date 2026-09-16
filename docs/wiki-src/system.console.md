# system.console

<!-- No prelude by design: WikiOverlay::LoadMethodSection derives the overlay file by
     stripping the last dotted segment, so the `### system.console.search` section must
     live here (not in system.md) to reach the generated system.console.search page. -->

### system.console.search

Search the live console registry. Required: `query` (non-empty string). Optional: `kind` (`"variable"`, `"command"`, or `"any"`, default `"any"`), `limit` (1-500, default 50), and projections: `namesOnly: true` drops `help`, while `fields` allow-lists `name`/`kind`/`help`/`currentValue`/`flags`. Broad default queries emit full help per row and may spill to a file; use a projection, narrower `query`, or smaller `limit` to keep the result inline.

Use this before `system.console_command` or `editor.console_command` when you know the rough name but not the exact command/CVar.
