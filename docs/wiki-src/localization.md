# localization

Run the Unreal Localization Dashboard's GatherText commandlet from a tracked PinWright job. Both verbs use the named target to select a default config under `Config/Localization`; an explicit `config` must remain a project-relative `.ini` in that directory.

## Target/config safety

The launcher accepts no extra command-line text, so callers cannot turn this surface into an arbitrary process runner.

- `target` is a single identifier (`A-Z`, `a-z`, digits, `_`, `-`), not a path.
- Omitting `config` selects `<target>_Gather.ini` or `<target>_Compile.ini` below `Config/Localization`.
- Absolute paths, drive/UNC prefixes, `..` segments, non-INI files, missing files, and configs outside `Config/Localization` are rejected.
- The config's `[CommonSettings] ManifestName` must match the requested target, and its `GatherTextStep` must contain the operation's expected commandlet class.
- Gather/Compile never accepts an arbitrary executable, shell fragment, or additional arguments.

### localization.gather

Gather reads source/assets according to the selected config and reports the commandlet's exit code, captured output tail, and a full UTF-8 log under `Saved/PinWright/Localization/`. The initial response carries the project-relative `config` and `logPath`; poll `call("system.job_status")` until the job is terminal before using generated manifests/archives.

### localization.compile

Compile runs the config's `GenerateTextLocalizationResource` step. It uses the same ticket, cancellation, output capture, and evidence shape as Gather. A config that does not contain the expected operation's commandlet step is rejected before a child process starts.
