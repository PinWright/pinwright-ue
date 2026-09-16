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

One group, one editor launch (never one editor per group, and never `-NullRHI` - some tests need
a real RHI):

```
UnrealEditor-Cmd.exe <Project>\<Host>.uproject \
  -ExecCmds="Automation RunTests PinWright.Catalog;Quit" \
  -TestExit="Automation Test Queue Empty" \
  -Abslog=<Project>\Saved\PinWright\test-runs\<run>\automation.log \
  -unattended -RunningUnattendedScript -nopause -nocefaccelpaint \
  -ddc=InstalledNoZenLocalFallback -log
```

The exact flags and the reason for each are in `CLAUDE.md` -> **Testing**. The full suite is
large (5000+ tests); scope to the affected groups unless the change touches dispatch, a shared
helper, a response shape, an error table, or a `Build.cs`.

Full suite:

- Windows: `scripts\Run-SuiteCapped.ps1 -HostProject <Project>\<Host>.uproject` - same argv plus
  a job-object memory cap and BelowNormal priority.
- Linux: run `UnrealEditor-Cmd` directly with the argv above, filter `PinWright`.

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

The issue board is the public repo [PinWright/pinwright-board](https://github.com/PinWright/pinwright-board),
one markdown file per ticket. Clone it next to the host checkout so the maintainer scripts find
it at `../../../.pinwright-board` relative to this plugin directory:

```
git clone https://github.com/PinWright/pinwright-board.git <parent of host project>/.pinwright-board
```

Bugs and feature requests from users go to
[pinwright-ue issues](https://github.com/PinWright/pinwright-ue/issues).

## Maintainer tooling

`CLAUDE.md` and `.polyskill/` are instructions for AI coding agents working on the plugin. A human
contributor does not need them, though `CLAUDE.md` is the most detailed build/test reference.

## Licensing of contributions

Contributions are accepted under the [MIT License](LICENSE). There is no CLA and no DCO sign-off:
opening a PR means you agree your contribution ships under MIT. New source files carry the same
header as the rest of the tree: `// Copyright (c) 2026 Alexander Penkin. MIT License.`
