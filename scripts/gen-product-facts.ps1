# Emits product-facts.json at the plugin repo root: the single source of truth for every
# number published about this plugin (website, docs, Fab listing). Every figure is read from
# a build artifact or a source file that already owns it - nothing here is typed by hand -
# because hand-maintained copies are what produced the published "1,300+ operations" claim
# against a real figure of 1,169.
#
# Also writes dist\fab-listing.md: the Fab listing lives in Epic's seller portal and cannot
# be generated into, so the marketing sentences are emitted paste-ready with the correct
# numbers already substituted.
param(
    [string]$PluginRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    # Registry manifest written by the editor at launch (WikiDiskGenerator). Defaults to the
    # host project's default wiki output directory; pass this when WikiOutputDirectory moved.
    [string]$RegistryJson = ''
)

$ErrorActionPreference = 'Stop'

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

function Read-TextFile {
    # Explicit UTF-8: Windows PowerShell would otherwise decode a BOM-less UTF-8 file as ANSI
    # and mangle the en-dashes and arrows these sources contain.
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

function Write-TextFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )
    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding $false))
}

function Get-RoundedFloor {
    # Rounded marketing strings are LOWER bounds and must never overstate. The value is
    # floored to the largest bucket multiple STRICTLY below it, so an exact-multiple figure
    # steps down a bucket instead of publishing itself as a "+" claim:
    #   1169 -> 1,100+   3500 -> 3,400+   66 -> 60+
    # Bucket is 100 from three digits up, 10 below that (66 has no non-zero hundreds floor).
    param([Parameter(Mandatory = $true)][int]$Value)

    if ($Value -lt 2) {
        throw "Cannot produce a rounded lower-bound string for value $Value."
    }
    $bucket = if ($Value -ge 100) { 100 } else { 10 }
    $floored = [int]([Math]::Floor(($Value - 1) / $bucket) * $bucket)
    # Invariant culture: the operator's locale must not decide whether the published string
    # reads "1,100+" or "1 100+".
    return $floored.ToString('N0', [System.Globalization.CultureInfo]::InvariantCulture) + '+'
}

function Get-ShortDescription {
    # One README-sized line from a wiki description, never reworded: the first sentence with
    # link and emphasis markup stripped. A sentence over 120 characters is cut, all within 110
    # and outside brackets, at its first ':' ';' or ' - ' break, else at its last comma,
    # else at a word boundary; a cut ends with a period.
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)

    $t = [regex]::Replace($Text, '\[([^\]]*)\]\([^)]*\)', '$1').Replace('`', '').Replace('**', '')
    $t = [regex]::Replace($t, "\s*[$([char]0x2013)$([char]0x2014)]\s*", ' - ')
    $t = [regex]::Replace($t, '\*([A-Za-z][^*]*?[A-Za-z])\*', '$1')
    $t = [regex]::Replace($t, '\s+', ' ').Trim()
    # A sentence ends at . ! or ? before whitespace; ellipses and e.g./i.e. do not end one.
    $sentence = [regex]::Match($t, '^.*?(?<!\.|\be\.g|\bi\.e|\bvs|\betc)[.!?](?!\.)(?=\s|$)')
    if ($sentence.Success) { $t = $sentence.Value }
    if ($t.Length -le 120) { return $t }

    $head = $t.Substring(0, 110)
    $breaks = @([regex]::Matches($head, '(?<!:):(?!:)|;| - ') | ForEach-Object { $_.Index })
    $commas = @([regex]::Matches($head, ',') | ForEach-Object { $_.Index })
    [array]::Reverse($commas)
    foreach ($at in ($breaks + $commas)) {
        $clause = $head.Substring(0, $at).TrimEnd()
        $unclosed = ($clause -replace '[^({\[]', '').Length - ($clause -replace '[^)}\]]', '').Length
        if ($clause.Length -ge 15 -and $unclosed -eq 0) { return $clause + '.' }
    }
    $cut = $head -replace '\s+\S*$', ''
    if (($cut -replace '[^({\[]', '').Length -gt ($cut -replace '[^)}\]]', '').Length) { $cut = $cut.Substring(0, $cut.LastIndexOfAny([char[]]'({[')) }
    $cut = $cut -replace '(\s+(a|an|and|or|the|to|of|for|from|with|in|on|at|by|as|into|over|through|via|than|that|which))+\s*$', ''
    return $cut.TrimEnd(',', ';', ':', ' ', '-') + '.'
}

function Get-WikiOperations {
    # Method rows ("- `name` - description") from the "## Methods" sections of <slug>.md and
    # its sub-namespace pages <slug>.<sub>.md; method and guide pages carry no such section.
    param(
        [Parameter(Mandatory = $true)][string]$WikiDir,
        [Parameter(Mandatory = $true)][string]$Slug
    )
    $ops = @{}
    $pages = Get-ChildItem -LiteralPath $WikiDir -File -Filter "$Slug*.md" |
        Where-Object { $_.BaseName -eq $Slug -or $_.BaseName.StartsWith("$Slug.") }
    foreach ($page in $pages) {
        $section = [regex]::Match((Read-TextFile $page.FullName), '(?ms)^## Methods\r?\n(.*?)(?=^## |\z)')
        foreach ($row in [regex]::Matches($section.Groups[1].Value, ('(?m)^- `([^`]+)` ' + [char]0x2014 + ' (.*?)\r?$'))) {
            if (-not $ops.ContainsKey($row.Groups[1].Value)) { $ops[$row.Groups[1].Value] = $row.Groups[2].Value }
        }
    }
    return $ops
}

function Format-Cell {
    # Table-cell / <summary> safe: angle brackets would parse as HTML, | splits the row,
    # * would start emphasis (backticks are already stripped, so nothing protects them).
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)
    return $Text.Replace('<', '&lt;').Replace('>', '&gt;').Replace('|', '\|').Replace('*', '\*')
}

function Get-AgentToolCount {
    # Members of `enum class EAgentTool` - the agents the in-editor setup screen can
    # configure one-click. The generated clientsOneClick list is asserted against this so a
    # client added or removed in code cannot silently diverge from the published list.
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Agent configurator header is missing: $Path"
    }
    $match = [regex]::Match((Read-TextFile $Path), 'enum\s+class\s+EAgentTool\s*(?::\s*\w+\s*)?\{(?<body>[^}]*)\}')
    if (-not $match.Success) {
        throw "Could not locate 'enum class EAgentTool' in $Path"
    }
    $body = [regex]::Replace($match.Groups['body'].Value, '//[^\r\n]*', ' ')
    return @([regex]::Matches($body, '\b[A-Za-z_]\w*\b')).Count
}

function Assert-RegistryIsComplete {
    # registry.json is written from the LIVE registry at editor launch, and the integration
    # sub-modules only register their handlers when the owning engine plugin is enabled in
    # the host project (see the startup line "PinWright integrations: loaded=[...]
    # skipped=[...]"). An editor run in a project with, say, PCG disabled emits a total below
    # the true shipping figure - silently, and low enough to shift the floor-rounded string
    # (1169 -> "1,100+" but 1061 -> "1,000+"). Refuse to build facts from a partial registry.
    param([Parameter(Mandatory = $true)]$Registry)

    # slug -> minimum method count that proves the contributing sub-module registered.
    # "ui" is shared: PinWrightCommonUI adds 4 methods on top of the main module's 6, so
    # presence of the namespace alone proves nothing and the count has to be checked.
    $expected = [ordered]@{
        'geometry'    = @{ Minimum = 85; Plugin = 'GeometryScripting'; Module = 'PinWrightGeometry' }
        'pcg'         = @{ Minimum = 14; Plugin = 'PCG';               Module = 'PinWrightPCG' }
        'chooser'     = @{ Minimum = 6;  Plugin = 'Chooser';           Module = 'PinWrightChooser' }
        'pose_search' = @{ Minimum = 3;  Plugin = 'PoseSearch';        Module = 'PinWrightPoseSearch' }
        'ui'          = @{ Minimum = 10; Plugin = 'CommonUI';          Module = 'PinWrightCommonUI' }
    }

    $byslug = @{}
    foreach ($namespace in @($Registry.namespaces)) {
        $byslug[[string]$namespace.slug] = [int]$namespace.methods
    }

    $problems = @()
    foreach ($slug in $expected.Keys) {
        $entry = $expected[$slug]
        if (-not $byslug.ContainsKey($slug)) {
            $problems += "$slug (missing entirely; plugin '$($entry.Plugin)', module '$($entry.Module)')"
        }
        elseif ($byslug[$slug] -lt $entry.Minimum) {
            $problems += "$slug ($($byslug[$slug]) methods, expected at least $($entry.Minimum); plugin '$($entry.Plugin)', module '$($entry.Module)')"
        }
    }

    if ($problems.Count -gt 0) {
        throw ("registry.json is incomplete - these integration namespaces are absent or short: " +
               [string]::Join('; ', $problems) +
               ". The editor that wrote it ran in a host project with those engine plugins disabled, so the operation count is below the true shipping figure. " +
               "Enable every integration plugin in the host project (check the startup line 'PinWright integrations: loaded=[...] skipped=[...]'), relaunch the editor once, and re-run this script.")
    }
}

$pluginRootFull = Get-FullPath $PluginRoot
if (-not (Test-Path -LiteralPath (Join-Path $pluginRootFull 'PinWright.uplugin'))) {
    throw "Plugin root does not contain PinWright.uplugin: $pluginRootFull"
}

# --- operations / namespaces: the live registry, never a source grep --------------------
if ([string]::IsNullOrWhiteSpace($RegistryJson)) {
    # Default WikiOutputDirectory is <HostProject>/Saved/PinWright/wiki; the plugin sits two
    # levels below the host project root.
    $RegistryJson = Join-Path (Split-Path -Parent (Split-Path -Parent $pluginRootFull)) 'Saved\PinWright\wiki\registry.json'
}
if (-not (Test-Path -LiteralPath $RegistryJson)) {
    throw ("Registry manifest not found: $RegistryJson`n" +
           "It is written from the live handler registry when the editor starts. Launch the host project's editor once (with every integration engine plugin enabled), then re-run this script. " +
           "Pass -RegistryJson if the project's WikiOutputDirectory setting moved the wiki output elsewhere. " +
           "Counting the source tree instead is what produced the wrong published figures, so there is deliberately no fallback.")
}

$registry = Read-TextFile $RegistryJson | ConvertFrom-Json
$operations = [int]$registry.operations
$namespaceCount = [int]$registry.namespaceCount
$namespaces = @($registry.namespaces)
if ($operations -le 0 -or $namespaceCount -le 0) {
    throw "Registry manifest has no usable counts (operations=$operations, namespaceCount=$namespaceCount): $RegistryJson"
}
if ($namespaces.Count -ne $namespaceCount) {
    throw "Registry manifest is inconsistent: namespaceCount=$namespaceCount but the namespaces array holds $($namespaces.Count) entries."
}
Assert-RegistryIsComplete -Registry $registry

# --- version: the descriptor ------------------------------------------------------------
$descriptor = Read-TextFile (Join-Path $pluginRootFull 'PinWright.uplugin') | ConvertFrom-Json
$version = [string]$descriptor.VersionName
if ([string]::IsNullOrWhiteSpace($version)) {
    throw 'PinWright.uplugin has no VersionName.'
}

# --- UE versions: the engine range the packaging loop is run against --------------------
# One list, verified by running scripts\package-prebuilt.ps1 -EngineRoot C:\UE_<v> per entry
# plus the version matrix; widen or narrow it here when that loop changes.
$ueVersions = @('5.3', '5.4', '5.5', '5.6', '5.7', '5.8')
if ($ueVersions.Count -lt 2) {
    throw "The UE version list must hold at least two versions, found $($ueVersions.Count)."
}
$ueRange = $ueVersions[0] + '-' + $ueVersions[$ueVersions.Count - 1]

# --- tests: the count published at docs\test-organization.md -----------------------------
# Same definition as the ripgrep one-liner documented there - matching LINES of
# IMPLEMENT_*_AUTOMATION_TEST / BEGIN_DEFINE_SPEC in .cpp/.h under Source\**\Private\Tests\ -
# done natively so a release script does not depend on ripgrep being installed. Verified to
# return the identical count.
$testFiles = @(Get-ChildItem -LiteralPath (Join-Path $pluginRootFull 'Source') -Recurse -File -Include *.cpp, *.h |
    Where-Object { $_.FullName -like '*\Private\Tests\*' })
$tests = @($testFiles | Select-String -Pattern 'IMPLEMENT_.*AUTOMATION_TEST|BEGIN_DEFINE_SPEC').Count
if ($tests -le 0) {
    throw 'Test count came back as 0 - the registration grep found nothing under Source\**\Private\Tests\.'
}

# --- MCP clients -------------------------------------------------------------------------
# One-click clients are the ones the in-editor setup screen writes configs for; the list is
# asserted against EAgentTool so code and copy cannot drift apart.
$clientsOneClick = @('Claude Code', 'Codex CLI', 'Cursor', 'Gemini CLI', 'VS Code Copilot')
$agentToolCount = Get-AgentToolCount (Join-Path $pluginRootFull 'Source\PinWright\Private\Setup\AgentMcpConfigurator.h')
if ($agentToolCount -ne $clientsOneClick.Count) {
    throw ("enum class EAgentTool has $agentToolCount member(s) but this script publishes $($clientsOneClick.Count) one-click client(s): " +
           [string]::Join(', ', $clientsOneClick) +
           ". A client was added or removed in code without updating the published list - fix clientsOneClick in this script.")
}
# Documentation-only: no setup-screen code backs these, users hand-edit the config.
$clientsManual = @('Windsurf', 'Cline')

$operationsRounded = Get-RoundedFloor $operations
$namespacesRounded = Get-RoundedFloor $namespaceCount
$testsRounded = Get-RoundedFloor $tests

$facts = [ordered]@{
    version           = $version
    ueRange           = $ueRange
    ueVersions        = $ueVersions
    operations        = $operations
    operationsRounded = $operationsRounded
    namespaceCount    = $namespaceCount
    namespacesRounded = $namespacesRounded
    namespaces        = @($namespaces | ForEach-Object {
        [ordered]@{ slug = [string]$_.slug; tier = [string]$_.tier; methods = [int]$_.methods }
    })
    tests             = $tests
    testsRounded      = $testsRounded
    clientsOneClick   = $clientsOneClick
    clientsManual     = $clientsManual
}

$factsPath = Join-Path $pluginRootFull 'product-facts.json'
Write-TextFile -Path $factsPath -Text (($facts | ConvertTo-Json -Depth 5) + "`r`n")

$listingPath = Join-Path $pluginRootFull 'dist\fab-listing.md'
$listing = @"
# Fab listing copy

Generated by scripts\gen-product-facts.ps1 from product-facts.json. The Fab listing lives in
Epic's seller portal and cannot be written to programmatically - paste these blocks by hand,
and regenerate this file whenever product-facts.json changes.

## Short description

Drive the Unreal Editor from your AI coding assistant over MCP: $operationsRounded editor
operations across $namespacesRounded namespaces, on Unreal Engine $ueRange.

## Long description

PinWright puts an MCP server inside the Unreal Editor, so your AI coding assistant can drive
the editor directly instead of guessing at your project from source files. It exposes
$operationsRounded editor operations across $namespacesRounded namespaces - actors, levels,
Blueprints, materials, Niagara, Sequencer, UMG, physics, networking and more - through a
single ``call`` tool with an on-disk wiki the assistant reads to discover them.

One-click setup for $([string]::Join(', ', $clientsOneClick)). $([string]::Join(' and ', $clientsManual)) connect with a
short manual config entry.

Works on Unreal Engine $ueRange, editor-only, Windows and Linux. Covered by $testsRounded
automated tests run against every supported engine version.

## Technical details

- Plugin version: $version
- Supported engine versions: $([string]::Join(', ', $ueVersions))
- Editor operations: $operationsRounded
- Namespaces: $namespacesRounded
- Automated tests: $testsRounded
- One-click MCP client setup: $([string]::Join(', ', $clientsOneClick))
- Manual MCP client setup: $([string]::Join(', ', $clientsManual))
"@
Write-TextFile -Path $listingPath -Text ($listing + "`r`n")

# --- README namespace list: regenerated between marker comments -------------------------
# Area groups and their order mirror the website's src/_data/namespaces.js so README and site
# read the same; a registered namespace missing here lands in "Other" with a warning.
$readmeGroups = [ordered]@{
    'Project & assets'      = @('asset', 'blueprint', 'data_table', 'chooser', 'input', 'texture', 'geometry', 'model')
    'Inspect, debug & data' = @('property', 'container', 'object', 'static_mesh', 'recorder', 'insights', 'performance')
    'Editor & system'       = @('editor', 'system', 'python', 'source_control', 'localization', 'misc', 'pipeline')
    'UI & widgets'          = @('widget', 'ui', 'drive')
    'Audio'                 = @('audio')
    'Scene, level & world'  = @('actor', 'level', 'world_partition', 'volume', 'spline', 'foliage', 'landscape', 'water', 'environment', 'navigation', 'spatial')
    'Rendering & look'      = @('material', 'niagara', 'lighting', 'post_process', 'rendering', 'render', 'image', 'camera', 'effect', 'mrq')
    'Animation & rigging'   = @('animation', 'anim', 'controlrig', 'skeleton', 'pose_search', 'physics', 'sequencer')
    'AI & gameplay'         = @('ai', 'behavior_tree', 'eqs', 'state_tree', 'gas', 'gameplay_tags', 'character', 'game_framework', 'game_features', 'interaction', 'session', 'networking', 'vehicle', 'pcg')
}
$listed = @($namespaces | Where-Object { $_.tier -ne 'internal' })
$grouped = @($readmeGroups.Values | ForEach-Object { $_ })
$other = @($listed | Where-Object { $grouped -notcontains $_.slug } | ForEach-Object { [string]$_.slug })
if ($other.Count -gt 0) {
    Write-Warning ("Not in the README/website area groups, listed under Other: " + [string]::Join(', ', $other))
    $readmeGroups['Other'] = $other
}

$wikiDir = Split-Path -Parent $RegistryJson
$listedOperations = 0
$body = New-Object System.Collections.Generic.List[string]
foreach ($group in $readmeGroups.Keys) {
    $members = @($readmeGroups[$group] | ForEach-Object { $slug = $_; $listed | Where-Object { $_.slug -eq $slug } })
    if ($members.Count -eq 0) { continue }
    $body.Add("**$group**")
    $body.Add('')
    foreach ($namespace in $members) {
        $slug = [string]$namespace.slug
        $ops = Get-WikiOperations -WikiDir $wikiDir -Slug $slug
        if ($ops.Count -ne [int]$namespace.methods) {
            throw "Wiki lists $($ops.Count) method(s) for '$slug' but registry.json registers $($namespace.methods); the wiki in $wikiDir is stale. Relaunch the editor to regenerate it."
        }
        $listedOperations += $ops.Count
        $page = Read-TextFile (Join-Path $wikiDir "$slug.md")
        $lede = [regex]::Match($page, '(?ms)^Stability:[^\n]*\n\s*\n([^#\s].*?)(?:\r?\n\s*\r?\n|\z)').Groups[1].Value
        $summary = (Get-ShortDescription $lede).TrimEnd('.').Replace('<', '&lt;').Replace('>', '&gt;')
        if ($summary) { $summary = ": $summary" }
        $noun = if ($ops.Count -eq 1) { 'operation' } else { 'operations' }
        $maturity = if ($namespace.tier -eq 'core') { 'Core' } else { 'Experimental' }
        $body.Add('<details>')
        $body.Add("<summary><code>$slug</code>$summary ($($ops.Count) $noun, $maturity)</summary>")
        $body.Add('')
        $body.Add('| Operation | What it does |')
        $body.Add('| --- | --- |')
        $names = [string[]]@($ops.Keys)
        [Array]::Sort($names, [StringComparer]::Ordinal)
        foreach ($name in $names) {
            $body.Add("| ``$name`` | $(Format-Cell (Get-ShortDescription $ops[$name])) |")
        }
        $body.Add('')
        $body.Add('</details>')
        $body.Add('')
    }
}

$block = @(
    '<!-- namespaces:begin -->'
    '<details>'
    "<summary><strong>All $($listed.Count) namespaces ($listedOperations operations)</strong></summary>"
    ''
    'Every operation is documented in the in-editor wiki; the agent reads `call("<namespace>")` for any of these.'
    ''
) + $body + @('</details>', '<!-- namespaces:end -->')

$readmePath = Join-Path $pluginRootFull 'README.md'
$readme = Read-TextFile $readmePath
$begin = $readme.IndexOf('<!-- namespaces:begin -->')
$end = $readme.IndexOf('<!-- namespaces:end -->')
if ($begin -lt 0 -or $end -lt $begin) {
    throw "README.md needs a '<!-- namespaces:begin -->' line followed by a '<!-- namespaces:end -->' line to hold the generated namespace list: $readmePath"
}
$end += '<!-- namespaces:end -->'.Length
Write-TextFile -Path $readmePath -Text ($readme.Substring(0, $begin) + [string]::Join("`r`n", $block) + $readme.Substring($end))

Write-Host "Registry: $RegistryJson"
Write-Host "Version: $version   UE: $ueRange ($([string]::Join(', ', $ueVersions)))"
Write-Host "Operations: $operations -> $operationsRounded"
Write-Host "Namespaces: $namespaceCount -> $namespacesRounded"
Write-Host "Tests: $tests -> $testsRounded"
Write-Host "Facts: $factsPath"
Write-Host "Fab listing: $listingPath"
