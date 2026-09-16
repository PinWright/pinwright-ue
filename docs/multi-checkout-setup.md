---
type: guide
summary: "Set up a second local checkout of the host UE project that shares LFS storage and DDC with the primary checkout. Enables parallel plugin development on the same git branch without duplicating ~270 GB of .git/lfs/ on disk."
date: 2026-07-09
tags: [workflow, disk, git, lfs, ddc, checkouts]
---

# Multi-Checkout Setup

How to create an additional local checkout of the host UE project that shares the heavy regenerable storage with the primary checkout. The motivating use case: run the main game on `master` in one directory while iterating on this plugin in a second directory, also on `master`, with periodic sync.

## Why not a fresh `git clone`

The host project's `.git/` is dominated by LFS — typically ~270 GB in `.git/lfs/objects/` versus ~2 GB of regular git objects. A fresh `git clone` duplicates all of it, plus another ~130 GB of working tree. The shared-clone approach below adds only the working tree (~130 GB) and zero git/LFS duplication.

## Why not `git worktree`

`git worktree` shares `.git/` (good) but refuses to check out the same branch in two worktrees. Both checkouts need `master` simultaneously for this workflow, so worktrees force either a detached HEAD or a "shadow" branch kept rebased against `master`. Both are awkward when the goal is "same source state, different plugin state."

## Procedure (Windows, PowerShell + git bash)

Variables used below — change `$dst` to add a third checkout, replace `$src` if your primary checkout lives elsewhere:

```
$src = "<checkout>"
$dst = "<checkout>-pluginwork"
```

### 1. Prune dead LFS in the source (optional but recommended)

`git lfs prune` removes local LFS objects that are unreachable from HEAD or any local branch and older than `lfs.pruneoffsetdays` (default 3). It does not rewrite history; if an object is needed again, LFS fetches it from the remote. On a long-lived checkout this typically reclaims a large fraction of `.git/lfs/`.

```bash
# Always-safe cleanup of aborted downloads
rm -rf "$src/.git/lfs/incomplete"

# Inspect first, then prune
git -C "$src" lfs prune --dry-run --verbose
git -C "$src" lfs prune
```

For extra safety (one network roundtrip per candidate object): add `--verify-remote`. Default already retains unpushed local commits' LFS objects, so the extra check is only meaningful if you suspect the remote might be missing pushed objects.

### 2. Shared clone without checkout

`--shared` writes `.git/objects/info/alternates` pointing at the source's object database. `--no-checkout` defers populating the working tree until after LFS storage is reconfigured (otherwise LFS smudge filters would try to download every object from the remote).

```bash
git clone --no-checkout --shared "$src" "$dst"
```

### 3. Redirect LFS storage at the source

Each clone has its own `.git/lfs/` by default. Point this clone at the source's storage so LFS objects aren't duplicated.

```bash
cd "$dst"
git config lfs.storage "$src/.git/lfs"
git lfs install --local
```

`git lfs install --local` writes the LFS smudge/clean filters into this clone's `.git/config` only — does not touch user-global config.

### 4. Populate the working tree

```bash
git checkout master
```

LFS smudge filters run during checkout and resolve every LFS pointer against the redirected storage. No network traffic if the source already has all objects.

### 5. Clone the plugin into the new checkout

The plugin lives in its own repo and is ignored by the outer project's `.gitignore`. Each host-project checkout gets its own independent plugin clone, so the two can sit on different plugin commits.

```bash
cd "$dst"
git clone https://github.com/PinWright/pinwright-ue.git Plugins/PinWright
```

Sync plugin work between checkouts via push/fetch through GitHub, or add a local-path remote between the two plugin clones for faster iteration:

```bash
cd "$dst/Plugins/PinWright"
git remote add main "$src/Plugins/PinWright"
git fetch main
```

### 6. Junction DerivedDataCache

Both checkouts hit the same shader/asset cache. UE's local DDC uses file locking, so concurrent editor sessions against the shared cache are safe.

```powershell
if (Test-Path "$dst\DerivedDataCache") { Remove-Item -Recurse -Force "$dst\DerivedDataCache" }
cmd /c mklink /J "$dst\DerivedDataCache" "$src\DerivedDataCache"
```

## Operational rules

- **Do not run `git gc --prune=now` or `git repack -ad` in the source repo while the shared clone exists.** Aggressive manual pruning can orphan objects the shared clone reads via alternates. Routine `git gc --auto` triggered by fetch/push is fine — it respects reachability across reflogs in the source.
- **Do not delete `$src/.git/lfs/` while the shared clone exists.** The shared clone has no copy of these objects; deleting them breaks both checkouts' LFS-tracked assets.
- **Do not delete `$src/DerivedDataCache/` from the shared clone.** Junction removal (`Remove-Item "$dst\DerivedDataCache"` against a junction) deletes only the link in the new checkout, not the target — but `Remove-Item -Recurse` against the junction *will* follow into the target and delete the cache. Test with a non-recursive remove first if in doubt, or use `cmd /c rmdir "$dst\DerivedDataCache"` (junction-aware).
- **`Saved/`, `Intermediate/`, `Binaries/` must remain per-checkout.** They are derived from each checkout's current source state and editor session.

## Agent/tool state — applied

Both checkouts on `master` carry identical tracked files. The shareable state below is **gitignored** (so it didn't transfer with the clone) but is codebase-scoped (so both checkouts want the same content).

Skip anything tracked by git — `git checkout master` already populated it.

### Apply at setup time

| Item | Mechanism | Target | Why |
|---|---|---|---|
| `CLAUDE.local.md` | hardlink | `$src\CLAUDE.local.md` | Local project guidance; identical between checkouts |

```powershell
# Files: hardlinks (same volume, no admin)
New-Item -ItemType HardLink -Path "$dst\CLAUDE.local.md" -Target "$src\CLAUDE.local.md" -Force | Out-Null
```

Per-user agent state (allow-lists, auto-memory directories, per-tool config files) is machine-specific
and deliberately not covered here — decide per tool whether the second checkout shares or duplicates it.

### Gitignored skill/agent symlinks — recreate per checkout

The source checkout has these gitignored symlinks that point at paths inside its own tree. Recreate them in the new checkout pointing at *its own* tree (not the source's):

| Link path | Target (relative to checkout root) |
|---|---|
| `.claude\skills\mcp-{audit,review,sprint,test-loop}` | `Plugins\PinWright\.claude\skills\<name>` |
| `.agents\skills\{llm-wiki,shortcut-audit,swagger-scan,ui-edit,wiki-sync,writing-wave-plan}` | `.claude\skills\<name>` (tracked dirs that come with the clone) |
| `.agents\skills\mcp-{audit,review,sprint,test-loop}` | `.claude\skills\<name>` (the junctions above) — create these **after** the `.claude\skills\mcp-*` junctions exist |
| `.codex\agents\wave-worker` | `.claude\agents\wave-worker` (tracked dir) |

Order matters: `.claude\skills\mcp-*` junctions point into the plugin clone, and `.agents\skills\mcp-*` junctions chain through them, so create the `.claude\skills\` set first. Use `cmd /c mklink /J <link> <target>` — junctions don't need admin and work for directory chains.

### Asset-dump cache (`.pinwright/asset-dumps/`)

> **Superseded.** This subsection describes a retired checkout layout and is no longer in use. The current setup commits the dump mirror directly in the host project's repo at `asset-dumps/` (machine-local `.dumpcache.json` markers are gitignored; mirror refreshes land as dedicated commits), so it transfers with a normal clone and needs no per-checkout setup. The text below is kept for historical reference.

The asset-dump cache is **its own local-only git repo** nested at `.pinwright/asset-dumps/` (branch `main`, no external remote). The host project's repo gitignores `/.pinwright/`, so the dumps don't transfer with `git checkout master`. Each host-project checkout that wants the cache needs its own clone of the asset-dumps repo. Two checkouts can sync via a local-path remote between their asset-dumps clones.

**Setup for the new checkout (local-clone with shared objects):**

```powershell
$src = "<checkout>"
$dst = "<checkout>-pluginwork"
New-Item -ItemType Directory -Force -Path "$dst\.pinwright" | Out-Null
# --shared is fine here too; the asset-dumps repo is small and local-only, so junction-via-alternates is optional
git clone --shared "$src/.pinwright/asset-dumps" "$dst/.pinwright/asset-dumps"
```

**Syncing between checkouts (after initial clone):**

`origin` in the new checkout's clone points back at the source's asset-dumps. To pull updates from the source:

```bash
cd $dst/.pinwright/asset-dumps
git fetch origin
git merge origin/main   # or rebase, or reset --hard origin/main if you keep this checkout downstream-only
```

To push from `$dst` back to `$src`, the source's working tree must not be on the same branch you're pushing to. Easiest: also add a remote in `$src` pointing at `$dst`, then pull from `$src`'s clone:

```bash
cd $src/.pinwright/asset-dumps
git remote add pluginwork "$dst/.pinwright/asset-dumps"
git fetch pluginwork
git merge pluginwork/main
```

**Schema-drift caveat:** if pluginwork's plugin updates the dump format and writes new-schema dumps, syncing those commits into main while main's plugin is on an older format will produce sidecars main can't fully read. Coordinate: either delay the sync until both checkouts' plugins agree on format, or branch the asset-dumps repo per plugin version.

**Alternative — junction:** if you don't need divergence between the two checkouts, `cmd /c mklink /J $dst\.pinwright $src\.pinwright` makes both share the same repo and working tree. Removes the sync step but costs you the ability to have different dump states in the two checkouts.

### State that should NOT be shared

| Item | Why |
|---|---|
| `.claude\scheduled_tasks.lock` | Runtime lock; sharing causes collisions |
| `.claude\test-loop-runs\`, `.claude\*-workspace\` | Per-run scratch |
| `~/.claude/projects/<slug>/*.jsonl`, UUID dirs, `sessions-index.json` | Session transcripts tied to the cwd they ran in |
| `Saved\`, `Intermediate\`, `Binaries\` | Per-checkout build/editor state |
| `.pytest_cache\` | Mtime-based; sharing causes false invalidation |
| `.playwright-mcp\`, `logs\`, `.codex\video_inspect\` | Per-session artifacts |
| `.idea\`, `.vscode\` | IDE config with absolute paths |

### Optional shares that need a case-by-case call

The following are gitignored and codebase-scoped but were *not* applied automatically — junction if you want, leave separate if you don't:

| Item | Junction safety |
|---|---|
| `.claude\plans\` | Sprint plan archive — both checkouts contribute to the same set |
| `.codex\plans\` | Same |
| `.serena\` | Serena MCP cache + memories; project name in `project.yml` is checkout-agnostic |
| `.dream-board\` | `wiki-dream` skill scratch notes; codebase-level |
| `node_modules\` (project root) | Orphaned npm install with no sibling `package.json`; identical bits between checkouts |

## Adding another checkout

Repeat sections 2–6 with a new `$dst`. All checkouts share the same source `.git/objects/` and `.git/lfs/`. Disk cost per additional checkout is ~130 GB working tree plus per-checkout `Intermediate/Binaries/Saved` (grows as you build).

## Removing a checkout

```powershell
# Junctions inside the checkout are removed cleanly by recursively deleting the parent
Remove-Item -Recurse -Force "$dst"
```

`Remove-Item -Recurse` follows junctions on Windows and will delete the target's contents. Before nuking a checkout, audit it for junctions pointing back at the primary:

```powershell
Get-ChildItem -Force "$dst" -Recurse -Attributes ReparsePoint -ErrorAction SilentlyContinue | Select-Object FullName, Target
```

If anything points back at `$src`, replace the junction with an empty directory (`cmd /c rmdir <junction>`) before the recursive remove. Junctions for `.git/lfs` (via `lfs.storage`) live in git config, not in the filesystem — those are removed automatically when the checkout's `.git/` goes away.

## Troubleshooting

- **`git checkout` fails with "missing LFS object"**: `lfs.storage` was not configured before checkout, or the source's `.git/lfs/` is missing the object. Run `git lfs fetch --all` in the source first, then retry the checkout.
- **`Binaries/` or `Intermediate/` shared accidentally**: delete in the new checkout and let UBT regenerate. Sharing these causes mysterious link errors when the two checkouts have different source states.
- **DDC corruption after editor crash**: delete `$src\DerivedDataCache\<backend>` for the affected backend and let UE rebuild on next launch. Both checkouts will see the new cache because the dir is junctioned.
