<#
.SYNOPSIS
  One-shot prebuilt release packager for a single Unreal Engine version.

.DESCRIPTION
  Produces a drop-in, symbolicated plugin zip in a single command:
    1. Stages the curated public file set (delegates to package-fab.ps1, which
       also runs the text blocklist validation).
    2. Compiles the staged plugin against the given engine via RunUAT BuildPlugin.
    3. Strips build scratch (Intermediate / HostProject), keeps binaries + .pdb.
    4. Emits <OutputDir>\PinWright-<Version>-<EngineLabel>-Win64.zip
       with a single PinWright/ root folder.

  Symbols (.pdb) are included by default so tester crash reports symbolicate on
  the reporter's machine; pass -NoSymbols for a lean zip.

  Pass -CompilerVersion to pin a specific MSVC toolchain (e.g. UE 5.3 needs an
  older compiler): RunUAT BuildPlugin drops the CLI arg, so the pin is applied via
  the user-global BuildConfiguration.xml with backup/restore around the build.
  Pass -BuildLogPath to tee the RunUAT output to a file (in addition to the
  console) so callers can scan the log for warnings.

.EXAMPLE
  .\package-prebuilt.ps1 -EngineRoot C:\UE_5.7
  .\package-prebuilt.ps1 -EngineRoot C:\UE_5.6
#>
param(
    [Parameter(Mandatory = $true)][string]$EngineRoot,
    # Display label embedded in the zip name; derived from the engine's
    # Build.version (e.g. "UE5.7") when omitted.
    [string]$EngineLabel = '',
    [string]$PluginRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$OutputDir = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')).Path 'dist'),
    # Short scratch root for the build, to stay under the Windows 260-char path
    # limit (the plugin's deep intermediate paths overflow long roots).
    [string]$ScratchRoot = (Join-Path $env:SystemDrive 'ea_pkg_tmp'),
    [switch]$NoSymbols,
    # Pass -StrictIncludes to BuildPlugin (NoPCH/NoSharedPCH/DisableUnity) to match
    # Epic's marketplace gate and surface IWYU/missing-include errors that unity hides.
    [switch]$StrictIncludes,
    # Pin a specific MSVC toolchain via the user-global BuildConfiguration.xml (RunUAT
    # BuildPlugin drops -CompilerVersion); needed for UE 5.3. Empty = machine default.
    [string]$CompilerVersion = '',
    # Tee RunUAT output to this file (parent dir auto-created) so callers can scan the
    # log for warnings. Empty = console only.
    [string]$BuildLogPath = ''
)

$ErrorActionPreference = 'Stop'
$PluginName = 'PinWright'

function Resolve-EngineLabel {
    param([string]$Root)
    $verFile = Join-Path $Root 'Engine\Build\Build.version'
    if (-not (Test-Path -LiteralPath $verFile)) {
        throw "Not an engine root (no Engine\Build\Build.version): $Root"
    }
    $v = Get-Content -Raw -LiteralPath $verFile | ConvertFrom-Json
    return "UE$($v.MajorVersion).$($v.MinorVersion)"
}

$pluginRootFull = [System.IO.Path]::GetFullPath($PluginRoot).TrimEnd('\', '/')
$uplugin = Join-Path $pluginRootFull "$PluginName.uplugin"
if (-not (Test-Path -LiteralPath $uplugin)) { throw "Plugin descriptor missing: $uplugin" }

$runUat = Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat'
if (-not (Test-Path -LiteralPath $runUat)) { throw "RunUAT not found under EngineRoot: $runUat" }

if ([string]::IsNullOrWhiteSpace($EngineLabel)) { $EngineLabel = Resolve-EngineLabel $EngineRoot }
$Version = (Get-Content -Raw -LiteralPath $uplugin | ConvertFrom-Json).VersionName
if ([string]::IsNullOrWhiteSpace($Version)) { $Version = '0.0.0' }

# 1. Stage the curated, blocklist-validated source tree.
#    -EngineRoot is forwarded, not omitted: package-fab.ps1's Assert-PluginRefsCoverModuleDeps
#    builds its module -> owning-plugin map from <EngineRoot>\Engine\Plugins\**\*.uplugin, and
#    without the argument it auto-discovers the NEWEST local install (package-fab.ps1:571-577,
#    C:\UE_5.8 first). Every leg was therefore validating against 5.8's plugin map regardless of
#    the engine it was about to compile against, and on a machine with no engine at all the
#    check warned and returned (:581-582) instead of running on every package. This parameter
#    is already Mandatory here (:37), so there is nothing to discover.
Write-Host "[1/4] Staging curated source (with blocklist validation)..."
& (Join-Path $PSScriptRoot 'package-fab.ps1') -PluginRoot $pluginRootFull -EngineRoot $EngineRoot -ValidateOnly -KeepStage | Out-Null
$stageUplugin = Join-Path $pluginRootFull "dist\_stage\$PluginName\$PluginName.uplugin"
if (-not (Test-Path -LiteralPath $stageUplugin)) { throw "Stage did not produce $stageUplugin" }

# 2. Compile against the target engine. Build straight into a correctly-named,
#    short-path folder so the zip root and MAX_PATH are both handled.
$pkgDir = Join-Path $ScratchRoot $PluginName
if (Test-Path -LiteralPath $ScratchRoot) { Remove-Item -LiteralPath $ScratchRoot -Recurse -Force }
New-Item -ItemType Directory -Path $ScratchRoot | Out-Null
Write-Host "[2/4] BuildPlugin against $EngineLabel ($EngineRoot)..."
$buildArgs = @('BuildPlugin', "-Plugin=$stageUplugin", "-Package=$pkgDir", '-TargetPlatforms=Win64', '-Rocket')
if ($StrictIncludes) { $buildArgs += '-StrictIncludes' }

# Pin the MSVC toolchain via the user-global BuildConfiguration.xml when requested.
# The config is machine-wide, so back up any existing file and restore it in finally.
$cfgDir = Join-Path $env:APPDATA 'Unreal Engine\UnrealBuildTool'
$cfg    = Join-Path $cfgDir 'BuildConfiguration.xml'
$bak    = "$cfg.package-prebuilt-bak"
$pinned = $false
if ($CompilerVersion) {
    # Self-heal: a leftover backup means a prior run crashed before restoring, so
    # put it back over the current config before touching anything else.
    if (Test-Path -LiteralPath $bak) {
        if (Test-Path -LiteralPath $cfg) { Remove-Item -LiteralPath $cfg -Force }
        Move-Item -LiteralPath $bak -Destination $cfg -Force
    }
    New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
    if (Test-Path -LiteralPath $cfg) { Move-Item -LiteralPath $cfg -Destination $bak -Force }
    @(
        '<?xml version="1.0" encoding="utf-8" ?>',
        '<Configuration xmlns="https://www.unrealengine.com/BuildConfiguration">',
        '  <WindowsPlatform>',
        "    <CompilerVersion>$CompilerVersion</CompilerVersion>",
        '  </WindowsPlatform>',
        '</Configuration>'
    ) | Set-Content -LiteralPath $cfg -Encoding UTF8
    $pinned = $true
    Write-Host "Pinned MSVC $CompilerVersion for the package build."
}
try {
    if ($BuildLogPath) {
        $logParent = Split-Path -Parent $BuildLogPath
        if ($logParent -and -not (Test-Path -LiteralPath $logParent)) {
            New-Item -ItemType Directory -Force -Path $logParent | Out-Null
        }
        # Tee-Object leaves $LASTEXITCODE set by the native RunUAT call.
        & $runUat @buildArgs 2>&1 | Tee-Object -FilePath $BuildLogPath
        if ($LASTEXITCODE -ne 0) { throw "BuildPlugin failed (exit $LASTEXITCODE)" }
    }
    else {
        & $runUat @buildArgs
        if ($LASTEXITCODE -ne 0) { throw "BuildPlugin failed (exit $LASTEXITCODE)" }
    }
}
finally {
    if ($pinned) {
        Remove-Item -LiteralPath $cfg -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $bak) { Move-Item -LiteralPath $bak -Destination $cfg -Force }
        Write-Host "Restored prior BuildConfiguration.xml state."
    }
}

# 3. Strip build scratch; keep Binaries (+ .pdb unless -NoSymbols).
Write-Host "[3/4] Pruning build scratch..."
foreach ($junk in 'Intermediate', 'HostProject') {
    $p = Join-Path $pkgDir $junk
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
}
if ($NoSymbols) {
    Get-ChildItem -LiteralPath $pkgDir -Recurse -File -Filter *.pdb | Remove-Item -Force
}
if (-not (Test-Path -LiteralPath (Join-Path $pkgDir 'Binaries\Win64'))) {
    throw "Build produced no Binaries\Win64 — aborting."
}

# 4. Zip with a single plugin-root folder.
if (-not (Test-Path -LiteralPath $OutputDir)) { New-Item -ItemType Directory -Path $OutputDir | Out-Null }
$zipName = "$PluginName-$Version-$EngineLabel-Win64.zip"
$zipPath = Join-Path $OutputDir $zipName
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Write-Host "[4/4] Zipping -> $zipPath"
Compress-Archive -Path $pkgDir -DestinationPath $zipPath -Force
Remove-Item -LiteralPath $ScratchRoot -Recurse -Force

$sizeMb = '{0:N1}' -f ((Get-Item -LiteralPath $zipPath).Length / 1MB)
Write-Host "Done: $zipPath ($sizeMb MB)"
