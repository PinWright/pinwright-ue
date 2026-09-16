# board-commit.ps1 - commit one board ticket edit to the board's OWN git repo.
#
# The board (../../../.pinwright-board, relative to the plugin directory, i.e. a
# sibling of the host-project checkout; a clone of PinWright/pinwright-board) is a
# single git working tree that all
# four fuzz hosts edit directly, so this helper hides ALL the concurrency logic the
# workflows would otherwise have to reimplement per prompt:
#   - a MACHINE-GLOBAL named mutex serializes the .git/index critical section so
#     concurrent hosts never corrupt the shared index;
#   - a PATHSPEC commit (git commit -- <files>) records ONLY the named ticket
#     file(s), ignoring anything a sibling host staged for a DIFFERENT ticket, so a
#     commit never sweeps up another host's in-flight edit;
#   - a contended .git/index.lock is retried on a real time budget (-LockWaitSec), and a
#     lock frozen long enough to be abandoned is removed (-StaleLockSec with no git process
#     on this repo, -BusyLockSec with one) instead of wedging every future call on the
#     machine; while waiting it reports the lock's age and the pids still on the repo;
#   - "nothing to commit" is success ONLY when the named files are verified present
#     and clean in HEAD; otherwise it is a failure like any other;
#   - after the local commit, a BEST-EFFORT push to the backup remote (outside the
#     mutex, non-fatal) mirrors the board off-machine; a failed push self-heals on
#     the next commit's push, and never blocks the workflow.
# Bare lease writes (claimedBy/claimedAt at pick, stale-lease clears) are NOT
# committed - only meaningful ticket create/status/history changes call this.
#
# EXIT CODES - 0 means, and only means, "the named files are committed in HEAD":
#   0  the commit landed (hash printed), or the files were already committed and clean
#   1  anything else: bad input, lock never released, retries exhausted, git error,
#      or a post-commit verification that could not find the files in HEAD
# Every git call below is judged by its EXIT CODE. Never infer failure from the fact
# that git wrote something to stderr - git reports warnings there (the CRLF notice
# fires on every LF-authored ticket) and PowerShell turns those into a terminating
# NativeCommandError, which used to abort the script before its retry loop ran.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Board,     # board repo root
    [Parameter(Mandatory = $true)][string]$Files,     # comma-separated ticket file(s); absolute or board-relative
    [Parameter(Mandatory = $true)][string]$Message,   # commit message
    [int]$TimeoutSec = 30,                             # max wait for the commit mutex
    [int]$Retries = 5,                                 # minimum attempts (the real budget is -LockWaitSec)
    [int]$LockWaitSec = 60,                            # total time to keep retrying a contended index.lock
    [int]$StaleLockSec = 120,                          # index.lock untouched this long AND with no git process on
    #                                                    this repo is abandoned - remove it
    [int]$BusyLockSec = 600                            # ...but if a git process IS on this repo, wait until the lock
    #                                                    has been frozen this long before removing it anyway
)

$ErrorActionPreference = 'Stop'
# PowerShell 7.3+: keep a non-zero native exit code from becoming a terminating error,
# so $LASTEXITCODE is always ours to inspect. Harmless no-op on Windows PowerShell 5.1.
$PSNativeCommandUseErrorActionPreference = $false

# Run git and return its exit code plus both captured streams.
#
# Windows PowerShell 5.1 converts a native command's stderr into ErrorRecords, and under
# $ErrorActionPreference = 'Stop' that becomes a terminating NativeCommandError which unwinds
# the script before $LASTEXITCODE can be read - this is what "warning: LF will be replaced by
# CRLF" did to every first commit of an LF-authored ticket. Measured on 5.1.26100 / 7.6.5:
# redirecting to files is NOT sufficient on 5.1 (the record is still raised, merely rendered
# into the file), and 'SilentlyContinue' suppresses it so thoroughly the file comes back EMPTY,
# losing the diagnostic. 'Continue' is the one setting that is both non-terminating and keeps
# the text. On 7.x all three behave identically and write git's raw stderr.
function Invoke-Git {
    param([Parameter(Mandatory = $true)][string[]]$GitArgs)

    $outFile = [System.IO.Path]::GetTempFileName()
    $errFile = [System.IO.Path]::GetTempFileName()
    $prevPref = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $code = 0
    $thrown = ''
    try {
        $global:LASTEXITCODE = 0
        & git @GitArgs 1> $outFile 2> $errFile
        $code = $LASTEXITCODE
    }
    catch {
        # git.exe missing, or an error PowerShell raised anyway - treat as a failed run.
        $code = if ($LASTEXITCODE -ne 0) { $LASTEXITCODE } else { 1 }
        $thrown = $_.Exception.Message
    }
    finally { $ErrorActionPreference = $prevPref }

    # Read-back must never yield $null: Get-Content -Raw returns $null for an empty file and
    # casting $null to [string] leaves it $null, which would blow up every .Trim() below.
    $so = ''
    $se = ''
    try { $raw = Get-Content -LiteralPath $outFile -Raw -ErrorAction SilentlyContinue; if ($null -ne $raw) { $so = [string]$raw } } catch { }
    try { $raw = Get-Content -LiteralPath $errFile -Raw -ErrorAction SilentlyContinue; if ($null -ne $raw) { $se = [string]$raw } } catch { }
    Remove-Item -LiteralPath $outFile, $errFile -Force -ErrorAction SilentlyContinue
    if ($thrown) { $se = ($se + "`n" + $thrown) }

    # Windows PowerShell 5.1 writes a fully RENDERED ErrorRecord into the stderr file - git's
    # line plus PowerShell's own "At <script>.ps1:NN", caret markers and +CategoryInfo trailer
    # (546 bytes for a one-line CRLF warning, measured). PS 7 writes git's raw text. Strip the
    # decoration so failure messages read as git wrote them. The position line is localized, so
    # key on the script-path:line shape rather than on any English word.
    $keep = @()
    foreach ($line in ($se -split '\r?\n')) {
        if ($line -match '^\s*\+') { continue }
        if ($line -match '\.ps1:\d+') { continue }
        if (-not $line.Trim()) { continue }
        $keep += ($line -replace '^\s*git(\.exe)?\s*:\s*', '')
    }
    $se = ($keep -join "`n")

    [pscustomobject]@{
        ExitCode = $code
        Out      = $so
        Err      = $se
        Text     = ((($so + "`n" + $se) -replace '\s+$', '') -replace '^\s+', '')
    }
}

function Write-Line([string]$text) { Write-Output "board-commit: $text" }

# Pids of running git.exe processes whose command line names this repo. Best effort: a git
# started with the board as its working directory (rather than `git -C <board>`) does not name
# it and is invisible here, which is why age, not this list, is what authorizes a removal.
function Get-RepoGitPids {
    param([Parameter(Mandatory = $true)][string[]]$Needles)
    try {
        $procs = @(Get-CimInstance -ClassName Win32_Process -Filter "Name='git.exe'" -ErrorAction Stop)
    }
    catch { return @() }
    $hits = @()
    foreach ($proc in $procs) {
        $cmd = ([string]$proc.CommandLine).ToLower() -replace '\\', '/'
        foreach ($needle in $Needles) {
            if ($needle -and $cmd.Contains($needle)) { $hits += $proc.ProcessId; break }
        }
    }
    @($hits)
}

# Deal with a .git/index.lock that is blocking us. Returns { State; Message } - the message is
# returned rather than printed because anything a function writes to the pipeline becomes part
# of its return value, which would turn the state into an array.
#   'none'    no lock file present (whatever failed, it was not this)
#   'fresh'   younger than -StaleLockSec: a live writer is plausibly mid-write, so wait
#   'busy'    stale, but a git process on this repo is alive and the lock is younger than
#             -BusyLockSec: wait, and say which pid and how old, so a caller can tell
#             "wait" from "recover" (this is the stalled-git case, board ticket encounter #2)
#   'removed' the lock was removed; the caller should retry immediately
#
# Deleting is NOT protected by the filesystem: git-for-Windows opens index.lock with
# FILE_SHARE_DELETE, so Windows lets the delete through even while a live git holds the handle
# (measured, 2026-09-03). The age thresholds are therefore the whole safety argument. Worst case
# for a wrongly-removed lock is bounded: git's final rename of index.lock onto index fails and
# that command reports an error - the index itself is not rewritten and not corrupted.
function Resolve-IndexLock {
    param(
        [Parameter(Mandatory = $true)][string]$GitDir,
        [Parameter(Mandatory = $true)][string[]]$Needles,
        [int]$StaleSec,
        [int]$BusySec
    )

    $lock = Join-Path $GitDir 'index.lock'
    if (-not (Test-Path -LiteralPath $lock)) { return [pscustomobject]@{ State = 'none'; Message = '' } }
    try { $item = Get-Item -LiteralPath $lock -Force -ErrorAction Stop }
    catch { return [pscustomobject]@{ State = 'none'; Message = '' } }
    $age = [int]((Get-Date) - $item.LastWriteTime).TotalSeconds
    if ($age -lt $StaleSec) { return [pscustomobject]@{ State = 'fresh'; Message = '' } }

    $holders = Get-RepoGitPids -Needles $Needles
    if ($holders.Count -gt 0 -and $age -lt $BusySec) {
        return [pscustomobject]@{
            State   = 'busy'
            Message = ("index.lock is {0}s old ({1} bytes) and git pid(s) {2} are still on this repo; waiting (recovery at {3}s)" -f `
                    $age, $item.Length, ($holders -join ', '), $BusySec)
        }
    }

    $why = if ($holders.Count -gt 0) { "abandoned by a stalled git (pid(s) $($holders -join ', ') alive but the lock has not moved)" }
           else { 'abandoned (no git process on this repo)' }
    try {
        Remove-Item -LiteralPath $lock -Force -ErrorAction Stop
    }
    catch {
        return [pscustomobject]@{
            State   = 'busy'
            Message = ("index.lock looks {0} but could not be removed: {1}" -f $why, $_.Exception.Message)
        }
    }
    # Sweep the zero-byte next-index-*.lock crumbs hosts leave behind when they die mid index
    # write; they accumulate one per dead host and nothing else ever clears them.
    foreach ($crumb in @(Get-ChildItem -LiteralPath $GitDir -Filter 'next-index-*.lock' -File -Force -ErrorAction SilentlyContinue)) {
        if ($crumb.Length -eq 0 -and ((Get-Date) - $crumb.LastWriteTime).TotalSeconds -ge $StaleSec) {
            Remove-Item -LiteralPath $crumb.FullName -Force -ErrorAction SilentlyContinue
        }
    }
    [pscustomobject]@{
        State   = 'removed'
        Message = ("removed index.lock, {0}: {1}, age {2}s, {3} bytes" -f $why, $lock, $age, $item.Length)
    }
}

try { $boardFull = (Resolve-Path -LiteralPath $Board).Path }
catch { Write-Line "board path not found: $Board"; exit 1 }

# Graceful degrade: a non-git board (e.g. a custom boardPath) has nothing to commit, ever.
$probe = Invoke-Git @('-C', $boardFull, 'rev-parse', '--is-inside-work-tree')
if ($probe.ExitCode -ne 0) { Write-Line 'board is not a git repo; skipping'; exit 0 }

$gitDirProbe = Invoke-Git @('-C', $boardFull, 'rev-parse', '--absolute-git-dir')
if ($gitDirProbe.ExitCode -ne 0) { Write-Line "cannot resolve the board's git dir: $($gitDirProbe.Text)"; exit 1 }
$gitDir = $gitDirProbe.Out.Trim()

# Normalize the file list to repo-relative pathspecs with forward slashes (GetFullPath does
# not require the file to exist yet, so a brand-new ticket file resolves fine; forward
# slashes keep the pathspec usable against ls-tree/status, whose paths always use them).
$fileArgs = @()
foreach ($f in ($Files -split ',')) {
    $p = $f.Trim().Trim('"')
    if (-not $p) { continue }
    if ([System.IO.Path]::IsPathRooted($p)) {
        $full = [System.IO.Path]::GetFullPath($p)
        if ($full.ToLower().StartsWith(($boardFull.ToLower().TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar))) {
            $p = $full.Substring($boardFull.Length).TrimStart('\', '/')
        } else { $p = $full }
    }
    $fileArgs += ($p -replace '\\', '/')
}
if ($fileArgs.Count -eq 0) { Write-Line 'no files given; nothing to do'; exit 1 }
$fileList = ($fileArgs -join ', ')

# Strip tool-call XML that agents' Write/Edit calls occasionally leak into ticket tails
# (</content>, </invoke>, </parameter>, ...) before staging - a known recurring corruption
# on the shared board (4 tickets 2026-07-04; commit b9d20e7 2026-07-10).
foreach ($rel in $fileArgs) {
    $abs = if ([System.IO.Path]::IsPathRooted($rel)) { $rel } else { Join-Path $boardFull $rel }
    if (Test-Path -LiteralPath $abs) {
        $raw = Get-Content -LiteralPath $abs -Raw
        $clean = $raw -replace '(?:\s*</?(?:antml:)?(?:content|invoke|parameter|function_results)[^>]*>)+\s*$', "`n"
        if ($clean -ne $raw) { Set-Content -LiteralPath $abs -Value $clean -NoNewline }
    }
}

# Serialize the commit across all hosts on this machine.
$mutex = New-Object System.Threading.Mutex($false, 'Global\pinwright-board-commit')
$held = $false
$committed = $false
$failure = ''
try {
    try { $held = $mutex.WaitOne([TimeSpan]::FromSeconds($TimeoutSec)) }
    catch [System.Threading.AbandonedMutexException] { $held = $true }  # prior holder died mid-commit; we own it now
    if (-not $held) {
        Write-Line "could not acquire the commit lock within ${TimeoutSec}s; NOT committed: $fileList"
        exit 1
    }

    $deadline = (Get-Date).AddSeconds($LockWaitSec)
    $attempt = 0
    $lastLockState = ''
    $repoNeedles = @($boardFull, $gitDir) | ForEach-Object { ([string]$_).ToLower().TrimEnd('\', '/') -replace '\\', '/' }
    $lockPattern = 'index\.lock|cannot lock ref|[Uu]nable to create|another git process|Unable to lock|File exists'
    # Text alone is not a reliable lock test: on PS 5.1 the captured message can be hard-wrapped
    # mid-token by the ErrorRecord renderer. An existing index.lock beside a failing git call is
    # the sturdier signal, and misreading some other failure as a lock only costs retry time.
    $lockFile = Join-Path $gitDir 'index.lock'
    while ($true) {
        $attempt++

        $add = Invoke-Git (@('-C', $boardFull, 'add', '--') + $fileArgs)
        if ($add.ExitCode -ne 0) {
            $failure = "git add failed (exit $($add.ExitCode)): $($add.Text)"
            if ($add.Text -notmatch $lockPattern -and -not (Test-Path -LiteralPath $lockFile)) { break }
        }
        else {
            # Pathspec commit: only $fileArgs are recorded, regardless of the shared index state.
            $commit = Invoke-Git (@('-C', $boardFull, 'commit', '-m', $Message, '--') + $fileArgs)
            if ($commit.ExitCode -eq 0) { $committed = $true; $failure = ''; break }
            if ($commit.Text -match 'nothing to commit|no changes added|nothing added to commit') { $failure = ''; break }
            $failure = "git commit failed (exit $($commit.ExitCode)): $($commit.Text)"
            if ($commit.Text -notmatch $lockPattern -and -not (Test-Path -LiteralPath $lockFile)) { break }
        }

        # A lock is in the way. Recover it if it is abandoned, otherwise wait it out.
        $lock = Resolve-IndexLock -GitDir $gitDir -Needles $repoNeedles -StaleSec $StaleLockSec -BusySec $BusyLockSec
        # The age in a 'busy' message changes every pass; report a state once, not per retry.
        if ($lock.Message -and ($lock.State -eq 'removed' -or $lock.State -ne $lastLockState)) { Write-Line $lock.Message }
        $lastLockState = $lock.State
        if ($attempt -ge $Retries -and (Get-Date) -ge $deadline) {
            $failure = "gave up after $attempt attempts / ${LockWaitSec}s waiting on the board index. $failure"
            break
        }
        if ($lock.State -ne 'removed') {
            Start-Sleep -Milliseconds ([int]([Math]::Min(2000, 250 * [Math]::Pow(2, [Math]::Min($attempt, 3))) + (Get-Random -Maximum 250)))
        }
    }

    # Verify rather than assume. The board state, not git's exit code, decides the verdict:
    # a git call can fail after the commit object already landed (two writers racing for the
    # index), and reporting that as a failure would make the caller re-file a filed ticket.
    $missing = @()
    foreach ($rel in $fileArgs) {
        $abs = if ([System.IO.Path]::IsPathRooted($rel)) { $rel } else { Join-Path $boardFull $rel }
        if (-not (Test-Path -LiteralPath $abs)) {
            # Gone from disk: a deletion we committed is fine; anything else never landed.
            if (-not $committed) { $missing += "'$rel' is not on disk and no commit was made" }
            continue
        }
        $inHead = Invoke-Git @('-C', $boardFull, 'ls-tree', '--name-only', 'HEAD', '--', $rel)
        if ($inHead.ExitCode -ne 0 -or -not $inHead.Out.Trim()) {
            $missing += "'$rel' is not in HEAD"
            continue
        }
        # On the no-op path, "clean" is the only evidence that the file's CURRENT content is
        # what is in HEAD. After our own commit we skip this: a sibling host is free to edit
        # the same file the moment we release the index, and that would not unmake our commit.
        if (-not $committed) {
            $dirty = Invoke-Git @('-C', $boardFull, 'status', '--porcelain', '--', $rel)
            if ($dirty.ExitCode -eq 0 -and $dirty.Out.Trim()) {
                $missing += "'$rel' still differs from HEAD ($($dirty.Out.Trim()))"
            }
        }
    }
    if ($missing.Count -gt 0) {
        if ($failure) { Write-Line $failure }
        foreach ($m in $missing) { Write-Line "NOT committed: $m" }
        exit 1
    }
}
finally {
    if ($held) { $mutex.ReleaseMutex(); $mutex.Dispose() } else { $mutex.Dispose() }
}

$hashProbe = Invoke-Git @('-C', $boardFull, 'rev-parse', '--short', 'HEAD')
$hash = if ($hashProbe.ExitCode -eq 0) { $hashProbe.Out.Trim() } else { '<unknown>' }

# Best-effort backup push OUTSIDE the lock (non-fatal - the next commit's push carries any backlog).
if ($committed) {
    $remotes = Invoke-Git @('-C', $boardFull, 'remote')
    if (($remotes.Out -split '\r?\n') -contains 'origin') {
        $pushed = $false
        for ($i = 0; $i -lt 2; $i++) {
            if ((Invoke-Git @('-C', $boardFull, 'push', 'origin', 'HEAD:master')).ExitCode -eq 0) { $pushed = $true; break }
            Start-Sleep -Milliseconds (300 + (Get-Random -Maximum 500))
        }
        if (-not $pushed) { Write-Line 'backup push failed; the next commit will carry it' }
    }
}

if ($failure) { Write-Line "git reported an error, but the files ARE on the board: $failure" }
if ($committed) { Write-Line "committed $hash - $fileList" }
else { Write-Line "already committed at $hash (no changes) - $fileList" }
exit 0
