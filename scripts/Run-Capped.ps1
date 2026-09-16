# Copyright (c) 2026 Alexander Penkin. MIT License.
<#
.SYNOPSIS
    Runs an arbitrary command inside a Windows Job Object with a per-process memory cap and a
    below-normal CPU priority class.

.DESCRIPTION
    The generic sibling of Run-SuiteCapped.ps1. Same containment, no suite knowledge: the command
    is created suspended, assigned to a job carrying JOB_OBJECT_LIMIT_PROCESS_MEMORY (a fraction of
    physical RAM), JOB_OBJECT_LIMIT_PRIORITY_CLASS and JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, and then
    resumed. The interop itself lives in CappedJob.ps1 so both launchers share one implementation.

    WHY A JOB OBJECT AND NOT A WATCHDOG. The limits apply to every process in the job, including
    ones the command spawns after it starts -- cl.exe / link.exe / dotnet UnrealBuildTool for a
    build, ShaderCompileWorker for an editor -- and they apply from the child's first instruction
    rather than from whenever a poller next samples. A process in the job also cannot raise its own
    priority back above the job's, which is what makes the class stick to a build's whole tree.

    WHY PER-PROCESS AND NOT JOB-WIDE MEMORY. JOB_OBJECT_LIMIT_JOB_MEMORY would charge every child's
    working set to one shared budget and fail whichever process happens to allocate last;
    JOB_OBJECT_LIMIT_PROCESS_MEMORY gives each process its own ceiling.

    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE is set so that killing this script takes the command and
    everything it spawned with it; nothing survives the launcher.

    This script does not classify what the command did beyond its exit code. A build's own log, or
    a suite's checker, remains the verdict authority.

.EXAMPLE
    .\Run-Capped.ps1 -Command '<UE_ROOT>\Engine\Build\BatchFiles\Build.bat' `
        -CommandArgs @('<HostProject>Editor','Win64','Development','-Project="<HOST>\<HostProject>.uproject"','-WaitMutex','-NoHotReloadFromIDE') `
        -OutputPath <HOST>\Saved\Logs\pw_build.log
#>
[CmdletBinding()]
param(
    # Executable or .bat to run. Resolved to a full path; a missing file is a hard error.
    [Parameter(Mandatory = $true)] [string] $Command,
    # Passed through verbatim, exactly as the suite launcher does -- quote inside the element when
    # an argument needs quoting (e.g. '-Project="<HOST>\<HostProject>.uproject"').
    [string[]] $CommandArgs = @(),
    # When given, stdout+stderr are redirected into this file. See the cmd.exe note below.
    [string] $OutputPath,
    # Fraction of physical RAM any single process in the job may commit.
    [double] $MemoryFraction = 0.60,
    [ValidateSet('Normal', 'BelowNormal', 'Idle')] [string] $PriorityClass = 'BelowNormal',
    [int] $TimeoutMinutes = 120,
    # Machine-readable one-liner is written here as well as printed, so a detached launch can be
    # read back without capturing stdout. Defaults beside -OutputPath; without -OutputPath the line
    # is printed only.
    [string] $ResultPath
)

$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\CappedJob.ps1"

if (-not (Test-Path -LiteralPath $Command)) {
    $resolved = Get-Command -Name $Command -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $resolved) { throw "command not found: $Command" }
    $Command = $resolved.Source
}
$commandFull = (Resolve-Path -LiteralPath $Command).Path

if ($OutputPath) {
    $outDir = Split-Path -Parent $OutputPath
    if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    }
    if (-not $ResultPath) { $ResultPath = "$OutputPath.result.txt" }
}

$argsJoined = $CommandArgs -join ' '
$inner = "`"$commandFull`""
if ($argsJoined) { $inner += " $argsJoined" }

$cmdExe = Join-Path $env:SystemRoot 'System32\cmd.exe'
$isBatch = [System.IO.Path]::GetExtension($commandFull) -in @('.bat', '.cmd')

if ($OutputPath) {
    # CreateProcess runs with CREATE_NO_WINDOW and bInheritHandles=false, so the child has no
    # console and no inherited stdout handle -- it has nowhere to write. Launch through cmd.exe and
    # let the shell own the redirection instead.
    #
    # /S is load-bearing. Without it cmd re-parses the quotes in the rest of the line and can strip
    # the wrong pair; with it cmd removes ONLY the outer quote pair after /c and takes everything
    # between them verbatim, which is what keeps an embedded -Project="<HOST>\<HostProject>.uproject" intact.
    #
    # cmd.exe exits with the child's exit code, so the exit reported below is still the command's.
    # And Kill/Close reap correctly: terminating cmd alone would orphan the child, but closing the
    # job handle fires KILL_ON_JOB_CLOSE over the whole tree, so the timeout path stays correct.
    $launchExe = $cmdExe
    $commandLine = "`"$launchExe`" /S /c `"$inner > `"$OutputPath`" 2>&1`""
}
elseif ($isBatch) {
    # CreateProcess cannot execute a batch file -- it is not a PE image -- so a .bat/.cmd needs the
    # cmd.exe host even when there is no redirection to set up. With no -OutputPath and no console
    # under CREATE_NO_WINDOW the batch file's own output goes nowhere; that is the caller's choice.
    $launchExe = $cmdExe
    $commandLine = "`"$launchExe`" /S /c `"$inner`""
}
else {
    $launchExe = $commandFull
    $commandLine = $inner
}

$totalPhysical = [uint64](Get-CimInstance -ClassName Win32_ComputerSystem).TotalPhysicalMemory
if ($totalPhysical -le 0) { throw 'Could not read TotalPhysicalMemory' }
$capBytes = [uint64]([math]::Floor($totalPhysical * $MemoryFraction))
$priorityFlag = Get-PinWrightPriorityFlag -Name $PriorityClass
$gib = 1GB

Write-Host "PinWright capped run"
Write-Host "  command   : $commandFull"
Write-Host "  args      : $argsJoined"
Write-Host ("  hostRAM   : {0:N2} GiB" -f ($totalPhysical / $gib))
Write-Host ("  cap       : {0:N2} GiB ({1:P0} of physical, per-process)" -f ($capBytes / $gib), $MemoryFraction)
Write-Host ("  priority  : {0} (0x{1:X8}, job-wide)" -f $PriorityClass, $priorityFlag)
Write-Host "  output    : $(if ($OutputPath) { $OutputPath } else { '<none -- child stdout is discarded>' })"
Write-Host "  cmdline   : $commandLine"

$started = Get-Date
$run = [PinWrightCappedRun]::Start($launchExe, $commandLine, $capBytes, $priorityFlag)
Write-Host "  pid       : $($run.ProcessId)"

$timedOut = $false
try {
    # Block in bounded slices so a caller with a tool timeout can still be inside one call, and so
    # a hung command is bounded by -TimeoutMinutes.
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

$peakGb = if ($peakBytes -gt 0) { $peakBytes / $gib } else { 0 }
$capGb = $capBytes / $gib
# A hard per-process limit makes allocations FAIL rather than killing the process, so there is no
# exit code that means "capped". A peak pinned at the cap is the evidence; the violation record is
# a second, independent read that a hard-limit job may legitimately not populate.
$capHit = $violationIsMemory -or ($peakBytes -gt 0 -and $peakBytes -ge [uint64]($capBytes * 0.98))

$verdict =
    if ($timedOut) { 'TIMEOUT' }
    elseif ($capHit) { 'MEMORY_CAP_HIT' }
    elseif ($exitCode -ne 0) { 'COMMAND_EXIT_NONZERO' }
    else { 'COMMAND_EXITED' }

$wall = (Get-Date) - $started
Write-Host ''
Write-Host "  exit code            : $exitCode"
Write-Host "  wall time            : $([int]$wall.TotalMinutes) min $($wall.Seconds) s"
Write-Host ("  peak process commit  : {0:N2} GiB of the {1:N2} GiB cap" -f $peakGb, $capGb)
Write-Host "  violation flags      : $violationFlags$(if (-not $run.ViolationQueryed) { ' (hard limits do not populate the violation record)' })"

# Invariant culture, deliberately: this line is parsed, and on a non-English host "N2" prints
# "37,91" -- a decimal comma that reads as a thousands separator to every reader downstream.
$inv = [cultureinfo]::InvariantCulture
$resultLine = 'PINWRIGHT_JOB_RESULT verdict={0} exit={1} priority={2} cap_gb={3} peak_gb={4} wall_min={5} command={6} output={7}' -f `
    $verdict,
    $exitCode,
    $PriorityClass,
    ([math]::Round($capGb, 2)).ToString($inv),
    ([math]::Round($peakGb, 2)).ToString($inv),
    [int]$wall.TotalMinutes,
    $commandFull,
    $(if ($OutputPath) { $OutputPath } else { '<none>' })
Write-Host $resultLine
if ($ResultPath) { Set-Content -LiteralPath $ResultPath -Value $resultLine -Encoding UTF8 }

if ($verdict -eq 'MEMORY_CAP_HIT' -or $verdict -eq 'TIMEOUT') { exit 2 }
exit $exitCode
