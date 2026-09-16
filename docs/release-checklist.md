# Release Checklist

## Automated Package Checks

Run the release packager in validation mode:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/package-fab.ps1 -ValidateOnly -KeepStage
```

The validation checks:

- required public files exist;
- internal board, test, local log, build, and dependency folders are absent from the staged package;
- the staged descriptor parses as JSON;
- the staged descriptor omits the internal test module;
- public docs do not contain internal absolute paths;
- `Resources/Icon128.png` exists and is `128x128`.

Host-name scanning needs one untracked file. The host project's proper nouns are the one thing
that cannot be listed in this public repo, so they live in `scripts/package-fab.local.ps1`
(gitignored), which defines `$script:LocalHostVocabularyRules`, `$script:LocalBlockedLiterals`
and `$script:LocalDocsBlockedPatterns`. Without it the run prints `Host-vocabulary rules: none
(scripts/package-fab.local.ps1 absent)` and scans for no host names. **Packaging from inside a
host project without that file is not a clean result** - create it first, and check the run's
`Host-vocabulary coverage:` line names the host you are packaging from.

Create the release zip:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/package-fab.ps1
```

Create an engine-version-specific review package when submitting a Fab project version:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/package-fab.ps1 -EngineVersion 5.6.0 -PackageName PinWright-UE5.6
```

## Manual Checks Before Fab Submission

- Smoke-test the staged plugin in a blank Unreal project.
- Submit a zip that contains exactly one top-level `PinWright` plugin folder.
- Verify `README.md` is present in the staged package and that README links resolve inside the staged package (`LICENSE` and `THIRD_PARTY_NOTICES.md` are deliberately absent: Fab prohibits shipping license files).
- Confirm `CreatedBy`, `CreatedByURL`, `DocsURL`, `SupportURL`, and `MarketplaceURL` are blank or point to vendor-controlled release locations, not upstream source projects.
- The packager creates an empty staged `Content` folder for Fab structure compatibility; the development plugin remains `CanContainContent=false`.
- Confirm the README security section matches the actual default settings.
- Capture screenshots or video for the Fab listing.
- Fill the Fab listing metadata, support URL, engine version support, and pricing tiers.
- Complete publisher tax, payout, and trader-status setup in Fab.

## Published-Facts Sync

Every public surface (Fab listing, website, README, support repo) quotes the same counts —
operation count, namespace count, version, engine range. They drift unless regenerated from
one source in the same release. Do all of these, in order:

1. Regenerate the fact source against the live registry and commit it:
   `powershell -ExecutionPolicy Bypass -File scripts/gen-product-facts.ps1` → `product-facts.json`.
2. Copy it into the website repo (`PinWright/pinwright-website`) as `src\_data\facts.json`;
   commit and deploy there.
3. Re-render the Fab gallery slides, then **open every exported PNG and look at it**. Count
   changes reflow the text; clipped or overflowing lines are only visible by eye.
4. Paste the regenerated Fab listing copy into Epic's seller portal by hand — the portal has no
   API and cannot be generated into, so it is the surface that silently stays stale.
5. Update the plugin-version example in the bug-report form
   (`.github/ISSUE_TEMPLATE/bug_report.yml`).

Fab update ships no later than the GitHub tag (Fab Distribution Agreement 3(b)).

## Publishing a release

The public repo is **`PinWright/pinwright-ue`** and its `master` is the source of truth — there is
no separate curated mirror and no staging repo. A release is a tag on `master` plus a GitHub
Release pointing at it.

### Steps (run from the plugin root)

1. Bump `VersionName` in `PinWright.uplugin` and add the `CHANGELOG.md` entry; commit and push
   `master`.
2. Tag and push: `git tag vX.Y.Z && git push origin vX.Y.Z`.
3. Create the release: `gh release create vX.Y.Z --repo PinWright/pinwright-ue --title "vX.Y.Z" --notes-file <notes>`.

That is source-only, which is the default. Attach prebuilt per-engine binary zips only when you
actually want them: build one per engine with
`scripts/package-prebuilt.ps1 -EngineRoot C:\UE_5.x` (each call stages, validates, compiles and
zips into `dist/`), then append `dist/*-Win64.zip` to the `gh release create` command. There is no
CI, so run that loop locally and read each run's output before publishing.
