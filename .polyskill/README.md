# Polyskill skill sources

Source-of-truth for skills and subagents that ship to multiple coding-agent
harnesses. Author once in Claude flavor, compile per-target.

Compiled by [polyskill](https://github.com/SSS135/polyskill) — a standalone,
zero-dependency `npx` tool. This file documents the authoring contract; see
**Running** below to build.

## Layout

```
.polyskill/
  skills/<name>/SKILL.md       # required
  skills/<name>/<bundled>      # optional; non-.md files copy verbatim
  agents/<name>.md             # single-file subagent
  agents/<name>/<helpers>.md   # optional sibling folder (e.g. phase files)
  polyskill.config.json
```

## Macro tags

Inline conditional blocks tagged with the target `kind`:

```markdown
<claude>This sentence is only emitted to Claude outputs.</claude>
<codex>This sentence is only emitted to Codex outputs.</codex>
```

Anything outside any tag is shared by every target. Tags are only processed
inside `.md` files — every other file copies byte-for-byte.

If a `.md` needs different frontmatter per target, wrap each frontmatter block
in its kind tag at the top of the file. Exactly one block must survive after
the macro pass:

```markdown
<claude>
---
name: foo
description: …
---
</claude>
<codex>
---
name: foo
description: …
---
</codex>

# Body shared across targets
```

When frontmatter is identical across targets (the common case), leave it
unwrapped and the same block flows through to every output.

## Output paths (fixed per kind)

| Kind   | Skills                       | Subagents                |
| ------ | ---------------------------- | ------------------------ |
| claude | `<out>/.claude/skills/`      | `<out>/.claude/agents/`  |
| codex  | `<out>/.agents/skills/`      | `<out>/.codex/agents/`   |

`<out>` is the per-target `out` from `polyskill.config.json`, resolved relative
to the config file's directory.

## Codex conversion rules

- Skill `SKILL.md`: frontmatter shrunk to `name` + `description`; other Claude
  frontmatter fields are dropped silently unless wrapped in a `<codex>`
  frontmatter block. Bundled files copy verbatim with the same macro pass.
- Subagent `.md` → TOML. Body becomes `developer_instructions`. Field mapping:
  - `name`, `description` — kept as-is
  - `mcpServers` → `mcp_servers`
  - `effort` → `model_reasoning_effort`
  - `model` is **never emitted** (Codex uses its default)
  - `tools`, `disallowedTools`, `permissionMode`, `hooks`, `memory`,
    `isolation`, `background`, `initialPrompt`, `color` — dropped with a
    warning per file

## Running

Run from this directory so polyskill picks up `polyskill.config.json`:

```powershell
npx --yes polyskill                      # all targets
npx --yes polyskill --target claude      # one target
npx --yes polyskill --target claude --target codex
```

The build is overwrite-only — files unchanged across runs are not rewritten,
orphans from deleted sources are not auto-cleaned. Remove stale outputs by
hand.
