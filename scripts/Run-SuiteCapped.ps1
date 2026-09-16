# Copyright (c) 2026 Alexander Penkin. MIT License.
<#
.SYNOPSIS
    Runs the PinWright automation suite inside a Windows Job Object with a per-process memory cap
    and a below-normal CPU priority class.

.DESCRIPTION
    Same argv every caller already used; the only addition is containment. The editor is created
    suspended, assigned to a job whose JOB_OBJECT_LIMIT_PROCESS_MEMORY is a fraction of physical
    RAM and whose JOB_OBJECT_LIMIT_PRIORITY_CLASS is -PriorityClass, then resumed.

    WHY A JOB OBJECT AND NOT A WATCHDOG. UE reads the job's limit at startup
    (WindowsPlatformMemory.cpp:449-486, "Detected a per-process memory limit of %.1fGB for this
    job.") into MemoryConstants.TotalVirtual, so the cap is not only an outer wall: it also makes
    FAssetCompilingManager throttle against the capped figure rather than against 63 GB of host
    RAM. A poller sampling working sets does neither, and only kills after the box is already
    swapping.

    WHY PER-PROCESS AND NOT JOB-WIDE. The suite spawns ShaderCompileWorker children into the same
    job. JOB_OBJECT_LIMIT_JOB_MEMORY would charge their working sets to the editor's budget and
    fail the editor for someone else's allocation; JOB_OBJECT_LIMIT_PROCESS_MEMORY gives each
    process its own ceiling, which is the one the editor is actually being measured against.

    WHAT A CAP HIT LOOKS LIKE. A hard process memory limit does not kill the process; it makes
    allocations fail. UE's allocator then takes its OOM path, logs "Ran out of memory allocating"
    and "from backup pool to handle out of memory", and dies with a fatal banner -- a
    post-mortem-able failure at a known bound instead of a host that pages itself to a standstill
    and gets classified DID_NOT_COMPLETE.

    WHY THE PRIORITY CLASS IS A JOB LIMIT. It applies to the shader workers too, and a process in
    the job cannot raise itself back above it. BelowNormal by default so a suite left running keeps
    the box usable; see Get-PinWrightPriorityFlag in CappedJob.ps1 for why not Idle.

    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE is set so that killing this script takes the editor and its
    shader workers with it; nothing survives the launcher.

    This script does NOT classify the suite. Run Content/Python/check_suite_log.py against the
    same -LogPath afterwards -- it remains the single verdict authority.

    For anything that is not the suite -- a Build.bat, a commandlet, a packaging run -- use the
    sibling Run-Capped.ps1, which puts an arbitrary command in the same job.

.EXAMPLE
    .\Run-SuiteCapped.ps1 -HostProject <HOST>\<HostProject>.uproject `
        -LogPath <HOST>\Saved\Logs\pw_suite.log `
        -PriorityClass BelowNormal `
        -ExtraArgs @('-ddc=InstalledNoZenLocalFallback','-PinWrightTestGcEvery=25','-PinWrightTestMemoryWatermark=0.55')
#>
[CmdletBinding()]
param(
    # The host UE project the plugin is installed into. Mandatory and never defaulted: there is no
    # canonical host, so a baked-in path would only ever be wrong on someone else's machine.
    [Parameter(Mandatory = $true)] [Alias('HostProject')] [string] $UProject,
    [string] $EngineRoot = 'C:\UE_5.8',
    # Defaults to UnrealEditor-Cmd.exe under -EngineRoot. Overridable because the version matrix
    # launches the GUI UnrealEditor.exe offscreen; the cap is indifferent to which one runs.
    [string] $EditorExe,
    [string] $Filter = 'PinWright',
    [Parameter(Mandatory = $true)] [string] $LogPath,
    # Fraction of physical RAM the editor process may commit. 0.60 of a 63 GB host leaves the
    # shader workers, the OS and any second editor room to live while still being well above a
    # healthy suite's peak.
    [double] $MemoryFraction = 0.60,
    # Applied job-wide, so the shader workers inherit it and cannot climb back out.
    [ValidateSet('Normal', 'BelowNormal', 'Idle')] [string] $PriorityClass = 'BelowNormal',
    [string[]] $ExtraArgs = @(),
    [int] $TimeoutMinutes = 120,
    # Machine-readable one-liner is appended here as well as printed, so a detached launch can be
    # read back without capturing stdout.
    [string] $ResultPath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $UProject)) { throw "uproject not found: $UProject" }
$editorExe = if ($EditorExe) { $EditorExe } else { Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe' }
if (-not (Test-Path -LiteralPath $editorExe)) { throw "editor not found: $editorExe" }
$editorExe = (Resolve-Path -LiteralPath $editorExe).Path

$UProject = (Resolve-Path -LiteralPath $UProject).Path
$logDir = Split-Path -Parent $LogPath
if ($logDir -and -not (Test-Path -LiteralPath $logDir)) {
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
}
if (-not $ResultPath) { $ResultPath = "$LogPath.result.txt" }

# ---------------------------------------------------------------------------------------------
# Job Object interop (memory cap + priority class + kill-on-close) lives in the shared launcher.
# ---------------------------------------------------------------------------------------------
. "$PSScriptRoot\CappedJob.ps1"

# ---------------------------------------------------------------------------------------------
# argv: byte-identical to what every caller used before this script existed, plus -ExtraArgs.
# -unattended and -RunningUnattendedScript ship together; see Content/Python/check_unattended_flags.py.
# ---------------------------------------------------------------------------------------------
$argv = @(
    "`"$UProject`"",
    "-ExecCmds=`"Automation RunTests $Filter,Quit`"",
    '-TestExit="Automation Test Queue Empty"',
    '-unattended',
    '-nopause',
    '-nosplash',
    '-nosound',
    '-RenderOffscreen',
    '-nocefaccelpaint',
    '-RunningUnattendedScript',
    "-Abslog=`"$LogPath`""
) + $ExtraArgs

$commandLine = "`"$editorExe`" " + ($argv -join ' ')

$totalPhysical = [uint64](Get-CimInstance -ClassName Win32_ComputerSystem).TotalPhysicalMemory
if ($totalPhysical -le 0) { throw 'Could not read TotalPhysicalMemory' }
$capBytes = [uint64]([math]::Floor($totalPhysical * $MemoryFraction))
$priorityFlag = Get-PinWrightPriorityFlag -Name $PriorityClass
$gib = 1GB

Write-Host "PinWright capped suite run"
Write-Host "  editor    : $editorExe"
Write-Host "  project   : $UProject"
Write-Host "  filter    : $Filter"
Write-Host "  log       : $LogPath"
Write-Host ("  hostRAM   : {0:N2} GiB" -f ($totalPhysical / $gib))
Write-Host ("  cap       : {0:N2} GiB ({1:P0} of physical, per-process)" -f ($capBytes / $gib), $MemoryFraction)
Write-Host ("  priority  : {0} (0x{1:X8}, job-wide)" -f $PriorityClass, $priorityFlag)
Write-Host "  extraArgs : $($ExtraArgs -join ' ')"
Write-Host "  cmdline   : $commandLine"

$started = Get-Date
$run = [PinWrightCappedRun]::Start($editorExe, $commandLine, $capBytes, $priorityFlag)
Write-Host "  pid       : $($run.ProcessId)"

$timedOut = $false
try {
    # Same shape as the version-matrix T2 loop: block in bounded slices so a caller with a tool
    # timeout can still be inside one call, and so a hung editor is bounded by -TimeoutMinutes.
    $deadline = $started.AddMinutes($TimeoutMinutes)
    while (-not $run.Wait(30000)) {
        if ((Get-Date) -gt $deadline) {
            Write-Host "  TIMEOUT after $TimeoutMinutes minutes -- terminating the job"
            $run.Kill()
            $timedOut = $true
            break
        }
    }

    $exitCode = $run.ExitCode()
    $peakBytes = $run.PeakProcessMemory()
    $run.ReadViolations()
    $violationFlags = if ($run.ViolationQueryed) { '0x{0:X8}' -f $run.ViolationLimitFlags } else { 'unavailable' }
    $violationIsMemory = $run.ViolationQueryed -and (($run.ViolationLimitFlags -band [PinWrightCappedRun]::JOB_OBJECT_LIMIT_PROCESS_MEMORY) -ne 0)
}
finally {
    $run.Close()
}

# ---------------------------------------------------------------------------------------------
# One streaming pass over the log. Never Get-Content/Select-String the whole file: a full suite
# log is hundreds of MB and the caller is told elsewhere not to stream Unreal output.
# ---------------------------------------------------------------------------------------------
$oomAlloc = 0
$oomBackupPool = 0
$watermarkMarkers = 0
$capSeenByEditor = $null
$memoryTotalLine = $null
$lastTestId = $null
if (Test-Path -LiteralPath $LogPath) {
    $reader = New-Object System.IO.StreamReader($LogPath, [System.Text.Encoding]::UTF8, $true)
    try {
        while ($null -ne ($line = $reader.ReadLine())) {
            if ($line.Contains('Ran out of memory allocating')) { $oomAlloc++ }
            if ($line.Contains('from backup pool to handle out of memory')) { $oomBackupPool++ }
            if ($line.Contains('PINWRIGHT_MEMORY_WATERMARK_EXCEEDED')) { $watermarkMarkers++ }
            # Two candidate proofs that the cap reached the editor, in preference order. The
            # engine's own "Detected a per-process memory limit of %.1fGB for this job." is
            # emitted from FWindowsPlatformMemory during memory init -- BEFORE GLog has a file --
            # so it usually never reaches -Abslog. What does reach it is FGenericPlatformMemory's
            # later dump: "Process is running as part of a Windows Job with separate resource
            # limits" plus a "Memory total: ... Virtual=<cap>GB" line whose Virtual figure IS
            # MemoryConstants.TotalVirtual, i.e. the cap. Absence of both means the job limit did
            # not take, and the run was uncapped whatever this script printed.
            if ($null -eq $capSeenByEditor -and (
                    $line.Contains('Detected a per-process memory limit of') -or
                    $line.Contains('running as part of a Windows Job with separate resource limits'))) {
                $capSeenByEditor = $line.Trim()
            }
            if ($null -eq $memoryTotalLine -and $line.Contains('Memory total: Physical=')) {
                $memoryTotalLine = $line.Trim()
            }
            if ($line.Contains('Test Started.')) { $lastTestId = $line }
        }
    }
    finally { $reader.Dispose() }
}
if ($lastTestId -and $lastTestId -match 'Path=\{([^}]*)\}') { $lastTestId = $Matches[1] }

$peakGb = if ($peakBytes -gt 0) { $peakBytes / $gib } else { 0 }
$capGb = $capBytes / $gib
# A hard per-process limit makes allocations FAIL rather than killing the process, so the two OOM
# strings are the primary evidence. The peak is a second, independent read: a run that walked into
# the wall leaves a peak pinned at the cap even if the log was truncated before the banner.
$capHit = ($oomAlloc -gt 0) -or ($oomBackupPool -gt 0) -or $violationIsMemory `
    -or ($peakBytes -gt 0 -and $peakBytes -ge [uint64]($capBytes * 0.98))

$verdict =
    if ($timedOut) { 'TIMEOUT' }
    elseif ($capHit) { 'MEMORY_CAP_HIT' }
    elseif ($exitCode -ne 0) { 'EDITOR_EXIT_NONZERO' }
    else { 'EDITOR_EXITED' }

$wall = (Get-Date) - $started
Write-Host ''
Write-Host "  exit code            : $exitCode"
Write-Host "  wall time            : $([int]$wall.TotalMinutes) min $($wall.Seconds) s"
Write-Host ("  peak process commit  : {0:N2} GiB of the {1:N2} GiB cap" -f $peakGb, $capGb)
Write-Host "  violation flags      : $violationFlags$(if (-not $run.ViolationQueryed) { ' (hard limits do not populate the violation record)' })"
Write-Host "  cap seen by editor   : $(if ($capSeenByEditor) { $capSeenByEditor } else { 'NOT LOGGED -- the editor did not report a job memory limit; the run may be uncapped' })"
Write-Host "  editor memory total  : $(if ($memoryTotalLine) { $memoryTotalLine } else { '<not logged>' })"
Write-Host "  'Ran out of memory allocating'            : $oomAlloc"
Write-Host "  'from backup pool to handle out of memory': $oomBackupPool"
Write-Host "  PINWRIGHT_MEMORY_WATERMARK_EXCEEDED       : $watermarkMarkers"
Write-Host "  last Test Started    : $(if ($lastTestId) { $lastTestId } else { '<none>' })"
Write-Host ''
Write-Host "  This script does not classify the suite. Run:"
Write-Host "    & `"$EngineRoot\Engine\Binaries\ThirdParty\Python3\Win64\python.exe`" `"$PSScriptRoot\..\Content\Python\check_suite_log.py`" `"$LogPath`""

# Invariant culture, deliberately: this line is parsed, and on a non-English host "N2" prints
# "37,91" -- a decimal comma that reads as a thousands separator to every reader downstream.
$inv = [cultureinfo]::InvariantCulture
$resultLine = 'PINWRIGHT_SUITE_RESULT verdict={0} cap_gb={1} peak_gb={2} priority={3} exit={4} oom_alloc={5} oom_backup_pool={6} watermark_markers={7} capSeenByEditor={8} wall_min={9} log={10}' -f `
    $verdict,
    ([math]::Round($capGb, 2)).ToString($inv),
    ([math]::Round($peakGb, 2)).ToString($inv),
    $PriorityClass,
    $exitCode, $oomAlloc, $oomBackupPool, $watermarkMarkers,
    [bool]$capSeenByEditor, [int]$wall.TotalMinutes, $LogPath
Write-Host $resultLine
Set-Content -LiteralPath $ResultPath -Value $resultLine -Encoding UTF8

if ($verdict -eq 'MEMORY_CAP_HIT' -or $verdict -eq 'TIMEOUT') { exit 2 }
exit 0
