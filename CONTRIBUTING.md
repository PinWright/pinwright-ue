# Contributing to PinWright

PinWright is an editor-only Unreal Engine plugin, developed inside a host UE project; every step
below assumes one.

## Get it building

1. Clone into a host project's `Plugins/` directory (UE 5.3-5.8, Windows or Linux):

   ```
   git clone https://github.com/PinWright/pinwright-ue.git <Project>/Plugins/PinWright
   ```

   Any C++-enabled UE project works. Some tests load Lyra mannequin content; see
   *Fixture skips* below.

2. Build the host project's **editor** target, Development configuration, with
   `-NoHotReloadFromIDE` (Live Coding leaves a half-linked DLL behind otherwise):

   ```
   # Windows
   "%UE_ROOT%\Engine\Build\BatchFiles\Build.bat" <Host>Editor Win64 Development ^
     "<Project>\<Host>.uproject" -NoHotReloadFromIDE -waitmutex
   # Linux
   "$UE_ROOT/Engine/Build/BatchFiles/Linux/Build.sh" <Host>Editor Linux Development \
     "<Project>/<Host>.uproject" -NoHotReloadFromIDE -waitmutex
   ```

3. Enable the plugin in the editor and check **Editor Preferences -> Plugins -> PinWright**.

### Standalone plugin build

To compile the plugin on its own against an engine, without a host project, use RunUAT:

```
<EngineRoot>/Engine/Build/BatchFiles/RunUAT.bat BuildPlugin \
  -Plugin=<abs path>/PinWright.uplugin -Package=<out dir> -TargetPlatforms=Win64 -Rocket
```

`scripts/package-prebuilt.ps1 -EngineRoot C:\UE_5.8` wraps that with descriptor staging and
validation; read it before hand-rolling the invocation.

## Tests

There is **no CI**: the maintainer runs the suite locally before merging a PR. Run what your
change touches and say so in the PR.

Test groups are `PinWright.<Group>` (`PinWright.Catalog`, `PinWright.Dispatch`,
`PinWright.Model`, `PinWright.Geometry`, `PinWright.render`, ...). Layout and group naming are in
[docs/test-organization.md](docs/test-organization.md).

One group, one editor launch (never one editor per group), through the stdio proxy's
`editor_run_tests`, then poll `editor_test_status`, on Windows and Linux alike. The caller picks
the mode. Renderer-dependent tests need `offscreen` or `visible`. A full-suite verdict is taken in
`offscreen` until renderer-dependent tests skip cleanly under NullRHI.

```
editor_run_tests {"filter": "PinWright.Catalog", "reason": "PinWright.Catalog after <change>", "mode": "offscreen"}
editor_test_status {"logPath": "<logPath from editor_run_tests>"}
```

It builds the suite argv (`-ExecCmds="Automation RunTests <filter>,Quit"`, `-TestExit`, `-Abslog`,
`-unattended -RunningUnattendedScript -nopause -nocefaccelpaint -ddc=InstalledNoZenLocalFallback`,
`-RenderOffscreen` in `offscreen` and `headless`, plus `-NullRHI` in `headless`) and runs it
detached under the capped supervisor: memory cap (Windows Job Object, Linux `systemd-run` scope),
BelowNormal priority, one `PINWRIGHT_SUITE_RESULT` line. `filter`, `reason` and `mode` are required.
Build first with `editor_build {"reason": ...}` and poll `editor_build_status`; it refuses while an
editor of this checkout runs.

The exact flags and the reason for each are in `CLAUDE.md` -> **Testing**. The full suite
(`filter: "PinWright"`) is large (5000+ tests); scope to the affected groups unless the change
touches dispatch, a shared helper, a response shape, an error table, or a `Build.cs`.

**Verdict comes from the checker, not from reading the log.** `Content/Python/check_suite_log.py`
is the only authority; it exits nonzero unless the run drained cleanly:

```
<EngineRoot>/Engine/Binaries/ThirdParty/Python3/<Platform>/python <plugin>/Content/Python/check_suite_log.py <same log path>
```

### Fixture skips

Tests that load host content by absolute `/Game/` path (the Lyra mannequin `ABP_Manny`, its
skeleton, `CR_Mannequin_Body`) **skip** on hosts that do not ship it, emitting `FIXTURE-SKIP:`
and a `PINWRIGHT_ASSERTIONS_SKIPPED` marker, so the checker reports `COMPLETED_WITH_SKIPS`. That
is expected on a non-Lyra host, not a failure. See
[docs/test-organization.md](docs/test-organization.md) -> **Host-Dependent Fixtures**.

## Docs

[docs/index.md](docs/index.md) is the map. Two rules:

- Agent-facing wiki pages are authored in `docs/wiki-src/` only. The served tree under
  `<Project>/Saved/PinWright/wiki/` is regenerated at editor startup - never hand-edit it.
- `rpc-method-reference.generated.md` is generated too.

## Issues

All tracking happens in [pinwright-ue issues](https://github.com/PinWright/pinwright-ue/issues):
bugs, feature requests and compatibility reports from users, and the maintainers' own backlog.
File through the issue forms. The old markdown board,
[PinWright/pinwright-board](https://github.com/PinWright/pinwright-board), is archived and kept
only for the history of tickets closed before the move; nothing needs to be cloned.

Labels:

| Label | Meaning |
|---|---|
| `type/bug`, `type/feature`, `type/ergonomic`, `type/compatibility` | Kind of issue |
| `area/harness` | The maintainers' workflow tooling |
| `sev/critical`, `sev/high`, `sev/medium`, `sev/low` | Severity |
| `status/needs-triage` | New, not yet reviewed by a maintainer |
| `status/accepted` | Reviewed; the only label that lets maintainer agents work the issue |
| `status/blocked` | Waiting on another issue |
| `status/claimed` | An agent is working it (a 4 hour lease held through comments) |
| `costly` | Encounters with it have cost real work |

Done means closed as completed, won't fix means closed as not planned, and duplicates are closed
as duplicate. The fixer verifies the fix and says how in the closing comment. An issue's history
is its comments; the `## History (board)` section appears only on issues migrated from the old
board.

Maintainers score each issue they file with RICE (reach, impact, confidence, effort). The helper
derives a 0-100 `priority` from it, shows it in the `RICE priority` issue field in the sidebar, and
lists accepted issues in that order; that is the order maintainer agents work them.

Maintainer agents act only on text written by the repository owner, members or collaborators. If
you are not one of them, a maintainer restates your report in a comment before adding
`status/accepted`, and agents work from that restatement. They never open links or attachments in
outside reports, so put the repro steps and log excerpts in the issue text itself.

Maintainers and their agents read and write issues through `scripts/pw_issues.py`
(`uv run scripts/pw_issues.py --help`), which enforces both gates and the lease. `CLAUDE.md` ->
**Issue tracker** has the full contract.

## Maintainer tooling

`CLAUDE.md` and `.polyskill/` are instructions for AI coding agents working on the plugin. A human
contributor does not need them, though `CLAUDE.md` is the most detailed build/test reference.

## Licensing of contributions

Contributions are accepted under the [MIT License](LICENSE). There is no CLA and no DCO sign-off:
opening a PR means you agree your contribution ships under MIT. New source files carry the same
header as the rest of the tree: `// Copyright (c) 2026 Alexander Penkin. MIT License.`
