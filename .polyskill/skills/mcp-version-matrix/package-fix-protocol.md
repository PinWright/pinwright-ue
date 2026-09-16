# Package-fix protocol

Guidance for clearing the Rocket / BuildPlugin **packaging gate** for the PinWright plugin. The gate runs
`scripts/package-prebuilt.ps1` (RunUAT `BuildPlugin -Rocket`, `-StrictIncludes` on 5.4+, MSVC 14.38 pinned on
5.3) and treats plugin-attributable warnings in the packaging log as failures. This is Fab parity: Epic
rejects a submission on "errors or consequential warnings", so a warning that survives the gate would fail
review. Zero tolerance.

Whether you are the inlined build-test-fix agent working a whole version or a dispatched subagent working one
warning site, the rules below are the same.

## What you have

- The packaging log tail (RunUAT / UBT output, redirected to the package log). Each cl.exe warning line is
  buried under an AutomationTool / UBT prefix; strip it to recover the raw `file.cpp(NN): warning C####:` shape.
- The package log path, for surrounding context.

## Step 1 - Read the gate scan

A line **GATES** (counts as a failure) if ANY of:

1. A compiler warning line (`: warning C####:`) whose file path is under the scratch or stage plugin tree.
2. Any `warning C4996` line, or any warning line containing "deprecated", even when the path is an engine
   header. C4996 is the MSVC deprecation code and its message text may omit the word "deprecated" entirely
   (the `MaterialTypes.h` case reads "Please update your code to the new API"). Only plugin translation
   units compile in a BuildPlugin run, so an engine-header deprecation is triggered by a plugin include.
3. A UBT / descriptor warning naming `Plugin 'PinWright'`.

**EXCLUDE** (not a gate):

- Note / message continuation lines (the follow-up lines printed under a primary diagnostic).
- UHT-phase warnings, unless they name a plugin header path.
- Engine-path warnings that are neither `C4996` nor deprecation-keyword.

**Dedupe** repeats to distinct sites: a site is (warning code + symbol + plugin-relative `path:line`). The same
warning emitted from three translation units is one site, not three.

## Step 2 - Fix each distinct site

| Warning | Likely cause | Canonical fix |
|---|---|---|
| `C4996: '<symbol>' ... deprecated` | A UE API deprecated in a newer minor; the plugin still calls the old spelling | Find the replacement API in the engine source (`C:\UE_<ver>\Engine\Source\`); swap the call. When the replacement does not exist on all of 5.3-5.8, guard the two spellings with `UE_VERSION_NEWER_THAN_OR_EQUAL` / `UE_VERSION_OLDER_THAN` (the codebase's canonical pattern, 15+ existing examples) so every matrix version stays green. Known day-one sites are listed below. |
| `C4996` on a deprecated **include** (e.g. `MaterialTypes.h` pulling the deprecated `Materials/MaterialParameters.h`) | The header include was itself deprecated in favor of a renamed / moved header | Switch to the header the deprecation message names. Use `__has_include(...)` when the replacement exists only on newer engines. |
| UBT descriptor warning: `Plugin 'PinWright' does not list plugin 'VoiceChat' ...` | A module linked at build time is not declared in the `.uplugin` `Plugins[]` | Add `{ "Name": "VoiceChat", "Enabled": true, "Optional": true }` to `PinWright.uplugin` `Plugins[]`. `Optional` is mandatory: the module is linked conditionally (`TryAddConditionalModule` in `Build.cs:132`) and may be absent on some engines; a non-optional entry would break plugin mount there. |

### Known day-one C4996 sites

These surfaced on the first Rocket build of the current tree. Fix each with the version-guard pattern in the
table above:

- `ForEachObjectWithPackage` in EditorCommandHandler.cpp:61
- `ISearchEngine::FindAllClasses` in MetaSoundSearchHandler.cpp:50
- `FCoreDelegates::OnPostEngineInit` in PinWrightModule.cpp:46,54 and RecorderLifecycle.cpp:25,31
- `GIsSavingPackage` in PinWrightSubsystem.cpp:216 and RpcDispatcher.cpp:337
- `ITimingProfilerProvider::ReadTimers` in TraceExportCore.cpp:541
- `ALandscape::GetLayerCount` in LandscapeHandler.cpp:106 (on 5.7)

## Hard limits

- Never `#pragma warning(disable: ...)` or any other warning suppression. A silenced warning still fails Fab
  review; suppression only hides it from the gate.
- Never an unguarded cross-version API swap. The replacement must compile on all of 5.3-5.8, or it goes behind
  a `UE_VERSION_*` guard.
- A warning you deliberately choose to ignore requires an explicit allowlist entry naming the warning code and
  the justification, never a wildcard.
- After every fix, re-run COMPILE and the full suite (the fix is production code and must stay green) before
  re-running PACKAGE.

## Self-check

Before re-running the gate, confirm:

1. A version-guarded fix leaves the other five versions' code path untouched.
2. The deprecated symbol / include is no longer referenced on the guarded versions.
3. `PinWright.uplugin` is still valid JSON with exactly the two Editor modules (package-fab's shape assertion
   checks this).
