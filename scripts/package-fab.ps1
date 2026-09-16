param(
    [string]$PluginRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$OutputDir = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')).Path 'dist'),
    [string]$PackageName = 'PinWright-Fab',
    [string]$VersionName = '',
    [string]$EngineVersion = '',
    [string]$MarketplaceProductId = '',
    # Engine used to derive the module -> owning-engine-plugin map for
    # Assert-PluginRefsCoverModuleDeps. Empty = auto-discover the newest local install.
    [string]$EngineRoot = '',
    [switch]$DryRun,
    [switch]$ValidateOnly,
    [switch]$KeepStage,
    # Test seam for the vocabulary scanner only. It scans this tree and exits before package
    # setup, so a failure-direction fixture can prove excluded repository paths are covered.
    [string]$HostVocabularyProbeRoot = ''
)

$ErrorActionPreference = 'Stop'

# The host-specific half of the publicity guards lives in the untracked
# scripts\package-fab.local.ps1, not here: this plugin is developed inside a private host
# project, and a deny-list of that project's proper nouns cannot sit in the public tree without
# shipping the names it exists to block. A maintainer packaging from inside a host project
# creates that file; without it the content scan still runs, with no host-name rules.
$localRules = Join-Path $PSScriptRoot 'package-fab.local.ps1'
if (Test-Path -LiteralPath $localRules) {
    . $localRules
}

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

function Test-IsStrictSubPath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $normalizedRoot = Get-FullPath $Root
    $normalizedPath = Get-FullPath $Path
    return $normalizedPath.StartsWith($normalizedRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)
}

function ConvertTo-RelativePath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $rootFull = Get-FullPath $Root
    $pathFull = Get-FullPath $Path
    if (-not $pathFull.StartsWith($rootFull + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is not under root. Root: $rootFull Path: $pathFull"
    }
    return $pathFull.Substring($rootFull.Length + 1).Replace('/', '\')
}

function Test-PathMatchesPattern {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string[]]$Patterns
    )

    $normalized = $RelativePath.Replace('/', '\').TrimStart('\')
    foreach ($pattern in $Patterns) {
        $candidate = $pattern.Replace('/', '\').TrimStart('\')
        if ($normalized.Equals($candidate, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
        if ($normalized.StartsWith($candidate + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}

function Copy-ReleaseEntry {
    param(
        [Parameter(Mandatory = $true)]$Entry,
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$DestinationRoot,
        [Parameter(Mandatory = $true)][string[]]$ExcludePatterns
    )

    $relative = [string]$Entry.path
    $source = Join-Path $SourceRoot $relative
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Required release path is missing: $relative"
    }

    $item = Get-Item -LiteralPath $source
    if (-not $item.PSIsContainer) {
        if (Test-PathMatchesPattern -RelativePath $relative -Patterns $ExcludePatterns) {
            $script:SkippedFiles++
            return
        }

        $destination = Join-Path $DestinationRoot $relative
        $parent = Split-Path -Parent $destination
        if (-not (Test-Path -LiteralPath $parent)) {
            New-Item -ItemType Directory -Path $parent | Out-Null
        }
        Copy-Item -LiteralPath $source -Destination $destination -Force
        $script:CopiedFiles++
        return
    }

    Get-ChildItem -LiteralPath $source -Recurse -File | ForEach-Object {
        $sourceRelative = ConvertTo-RelativePath -Root $SourceRoot -Path $_.FullName
        if (Test-PathMatchesPattern -RelativePath $sourceRelative -Patterns $ExcludePatterns) {
            $script:SkippedFiles++
        } else {
            $destination = Join-Path $DestinationRoot $sourceRelative
            $parent = Split-Path -Parent $destination
            if (-not (Test-Path -LiteralPath $parent)) {
                New-Item -ItemType Directory -Path $parent | Out-Null
            }
            Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
            $script:CopiedFiles++
        }
    }
}

function Remove-PathIfPresent {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Read-JsonFile {
    param([Parameter(Mandatory = $true)][string]$Path)
    return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
}

function Set-DescriptorScalar {
    # In-place replacement of "Key": <scalar> in the raw descriptor text, preserving the
    # original hand-formatting. ConvertTo-Json would reflow the whole file and add a UTF-8 BOM.
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Key,
        [Parameter(Mandatory = $true)][string]$Replacement
    )
    $pattern = '"' + [regex]::Escape($Key) + '"\s*:\s*(?:"[^"]*"|true|false|-?[0-9.]+)'
    $rx = [regex]::new($pattern)
    if (-not $rx.IsMatch($Text)) {
        throw "Descriptor key not found for in-place edit: $Key"
    }
    $eval = [System.Text.RegularExpressions.MatchEvaluator] { param($m) '"' + $Key + '": ' + $Replacement }
    return $rx.Replace($Text, $eval, 1)
}

function Update-StagedDescriptor {
    param(
        [Parameter(Mandatory = $true)][string]$DescriptorPath,
        [Parameter(Mandatory = $true)]$Manifest,
        [string]$VersionName,
        [string]$EngineVersion,
        [string]$MarketplaceProductId
    )

    # Edit only the fields the release process owns, in-place, so the descriptor keeps its
    # original formatting and UTF-8 (no BOM) encoding instead of being reflowed by ConvertTo-Json.
    $text = Get-Content -Raw -LiteralPath $DescriptorPath

    if ($null -ne $Manifest.descriptor.isBetaVersion) {
        $text = Set-DescriptorScalar -Text $text -Key 'IsBetaVersion' -Replacement (([bool]($Manifest.descriptor.isBetaVersion)).ToString().ToLowerInvariant())
    }
    if ($null -ne $Manifest.descriptor.isExperimentalVersion) {
        $text = Set-DescriptorScalar -Text $text -Key 'IsExperimentalVersion' -Replacement (([bool]($Manifest.descriptor.isExperimentalVersion)).ToString().ToLowerInvariant())
    }
    if (-not [string]::IsNullOrWhiteSpace($VersionName)) {
        $text = Set-DescriptorScalar -Text $text -Key 'VersionName' -Replacement ('"' + $VersionName + '"')
    }
    if (-not [string]::IsNullOrWhiteSpace($MarketplaceProductId)) {
        # Fab requirement 4.3.6.c: the descriptor must carry a "FabURL" key with the
        # com.epicgames.launcher://ue/Fab/product/<id> shape (not the legacy MarketplaceURL).
        $fabUrl = '"com.epicgames.launcher://ue/Fab/product/' + $MarketplaceProductId + '"'
        if ([regex]::IsMatch($text, '"FabURL"\s*:')) {
            $text = Set-DescriptorScalar -Text $text -Key 'FabURL' -Replacement $fabUrl
        }
        else {
            $text = [regex]::new('("VersionName"\s*:\s*"[^"]*",)').Replace($text, ('${1}' + "`n  `"FabURL`": " + $fabUrl + ','), 1)
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($EngineVersion)) {
        if ([regex]::IsMatch($text, '"EngineVersion"\s*:')) {
            $text = Set-DescriptorScalar -Text $text -Key 'EngineVersion' -Replacement ('"' + $EngineVersion + '"')
        }
        else {
            $text = [regex]::new('("VersionName"\s*:\s*"[^"]*",)').Replace($text, ('${1}' + "`n  `"EngineVersion`": `"" + $EngineVersion + "`","), 1)
        }
    }

    $descriptor = $text | ConvertFrom-Json
    $releaseModule = @($descriptor.Modules | Where-Object { $_.Name -eq 'PinWright' -and $_.Type -eq 'Editor' })
    if ($releaseModule.Count -ne 1) {
        throw 'Staged descriptor must contain exactly one PinWright editor module.'
    }
    $recorderModule = @($descriptor.Modules | Where-Object { $_.Name -eq 'PinWrightRecorder' -and $_.Type -eq 'Editor' })
    if ($recorderModule.Count -ne 1) {
        throw 'Staged descriptor must contain exactly one PinWrightRecorder editor module.'
    }

    [System.IO.File]::WriteAllText($DescriptorPath, $text, (New-Object System.Text.UTF8Encoding $false))
}

function Assert-RequiredFiles {
    param(
        [Parameter(Mandatory = $true)][string]$StagePluginRoot,
        [Parameter(Mandatory = $true)][string[]]$RequiredFiles
    )

    foreach ($relative in $RequiredFiles) {
        $path = Join-Path $StagePluginRoot $relative
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Required staged file is missing: $relative"
        }
    }
}

function Assert-RequiredPaths {
    param(
        [Parameter(Mandatory = $true)][string]$StagePluginRoot,
        [Parameter(Mandatory = $true)][string[]]$RequiredPaths
    )

    foreach ($relative in $RequiredPaths) {
        $path = Join-Path $StagePluginRoot $relative
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Required staged path is missing: $relative"
        }
    }
}

function Assert-BlockedPathsAbsent {
    param(
        [Parameter(Mandatory = $true)][string]$StagePluginRoot,
        [Parameter(Mandatory = $true)][string[]]$BlockedPaths
    )

    foreach ($relative in $BlockedPaths) {
        $path = Join-Path $StagePluginRoot $relative
        if (Test-Path -LiteralPath $path) {
            throw "Blocked release path is present: $relative"
        }
    }
}

function Assert-DocsArePublic {
    param([Parameter(Mandatory = $true)][string]$StagePluginRoot)

    $docsRoot = Join-Path $StagePluginRoot 'docs'
    if (-not (Test-Path -LiteralPath $docsRoot)) {
        return
    }

    $localProjectRoot = Split-Path -Parent (Split-Path -Parent (Get-FullPath $PluginRoot))
    # Host-specific patterns come from the untracked scripts\package-fab.local.ps1.
    $blockedPatterns = @($localProjectRoot, '.claude')
    if ($null -ne $script:LocalDocsBlockedPatterns) {
        $blockedPatterns += $script:LocalDocsBlockedPatterns
    }

    Get-ChildItem -LiteralPath $docsRoot -Recurse -File -Include *.md | ForEach-Object {
        $text = Get-Content -Raw -LiteralPath $_.FullName
        foreach ($pattern in $blockedPatterns) {
            if ($text.Contains($pattern)) {
                $relative = ConvertTo-RelativePath -Root $StagePluginRoot -Path $_.FullName
                throw "Public staged docs contain internal reference '$pattern' in $relative"
            }
        }
    }
}

function Get-TextLocation {
    # Character offset -> { Line, Start, Text }, so a finding can be cited as file:line with
    # the offending line beside it, and so a caller can convert the offset into a column
    # within that line. A blocked reference the report does not locate costs whoever fixes
    # it a grep across the whole staged tree. Findings are rare, so the linear scan is free.
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][int]$Offset
    )

    $clamped = [Math]::Max(0, [Math]::Min($Offset, $Text.Length))
    $line = 1
    for ($i = 0; $i -lt $clamped; $i++) {
        if ($Text[$i] -eq "`n") { $line++ }
    }
    $start = $clamped
    while ($start -gt 0 -and $Text[$start - 1] -ne "`n") { $start-- }
    $end = $clamped
    while ($end -lt $Text.Length -and $Text[$end] -ne "`n") { $end++ }
    return @{ Line = $line; Start = $start; Text = $Text.Substring($start, $end - $start).TrimEnd("`r") }
}

function Get-HostVocabularyRules {
    # Whole-word regex rules over the host project's proper nouns, defined by the untracked
    # scripts\package-fab.local.ps1 (see the note at the top of this file for why they are not
    # spelled here). Each rule carries a 'Why', an optional 'Allow' regex for a term whose
    # generic sense is real, and an optional 'CaseSensitive' flag. Empty when that file is absent,
    # which leaves the literal and docs scans running and only the host-name rules silent.
    if ($null -ne $script:LocalHostVocabularyRules) {
        return $script:LocalHostVocabularyRules
    }
    return @()
}

function Assert-HostVocabularyIsCovered {
    # Root-cause guard, and the reason this file gained a vocabulary table at all.
    # Get-HostVocabularyRules is hand-maintained, and the defect it prevents recurs the
    # moment this plugin is developed inside a host nobody has added to it - which is
    # exactly what happened. The host is discoverable rather than a matter of memory: it is
    # the .uproject sitting beside the Plugins/ directory this plugin lives in. So the
    # package can check that the table actually knows its host, instead of trusting that
    # someone remembered to teach it.
    #
    # This is a coverage check and never a substitute for the content scan: it proves the
    # table is aimed at the right project, not that the staged tree is clean.

    $rules = @(Get-HostVocabularyRules)
    if ($rules.Count -eq 0) {
        # No local rules file, so there is no table to aim: a public clone packages exactly this
        # way. Say so rather than failing on a check that has nothing to check.
        Write-Host 'Host-vocabulary rules: none (scripts/package-fab.local.ps1 absent); skipping host-name coverage check.'
        return
    }

    $localProjectRoot = Split-Path -Parent (Split-Path -Parent (Get-FullPath $PluginRoot))
    $hostProjects = @(Get-ChildItem -LiteralPath $localProjectRoot -File -Filter *.uproject -ErrorAction SilentlyContinue)
    if ($hostProjects.Count -eq 0) {
        # A bare plugin checkout with no host beside it - CI packages exactly this way. The
        # content scan still ran; only this coverage check is inapplicable, and it says so
        # out loud rather than passing silently.
        Write-Host 'Host-vocabulary coverage: no .uproject beside the plugin, so the vocabulary table could not be checked against a host project. The staged-text scan itself still ran.'
        return
    }

    foreach ($hostProject in $hostProjects) {
        $hostName = [System.IO.Path]::GetFileNameWithoutExtension($hostProject.Name)
        $covered = $false
        foreach ($rule in $rules) {
            if ([regex]::IsMatch($hostName, '\b(?:' + $rule.Pattern + ')\b', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) {
                $covered = $true
                break
            }
        }
        if (-not $covered) {
            throw "This plugin is being packaged from inside host project '$hostName', and no rule in Get-HostVocabularyRules matches that name. The vocabulary guard is therefore aimed at some previous host and cannot see this one's terms - which is the exact failure that let the last host's vocabulary ship. Add a rule for '$hostName' and for that project's proper nouns (theme, factions, named content, genre) before packaging."
        }
    }
    $hostNames = @($hostProjects | ForEach-Object { [System.IO.Path]::GetFileNameWithoutExtension($_.Name) })
    Write-Host "Host-vocabulary coverage: $($rules.Count) rule(s) checked against host project(s) $([string]::Join(', ', $hostNames))."
}

function Assert-StagedTextIsPublic {
    param(
        [Parameter(Mandatory = $true)][string]$StagePluginRoot,
        [switch]$HostVocabularyOnly,
        [string[]]$ExcludePaths = @()
    )

    $localProjectRoot = Split-Path -Parent (Split-Path -Parent (Get-FullPath $PluginRoot))
    # Literal, case-sensitive, substring. These are paths and proper nouns whose exact
    # spelling carries the whole signal. Only the local project root is computed here; the
    # host-specific spellings come from the untracked scripts\package-fab.local.ps1.
    $blockedLiterals = @($localProjectRoot)
    if ($null -ne $script:LocalBlockedLiterals) {
        $blockedLiterals += $script:LocalBlockedLiterals
    }
    # No-tickets-in-user-docs guard, scoped to prose. The scan is literal-substring
    # (String.IndexOf), not regex, so it cannot match bare kebab-case ticket slugs like
    # 'B-foo-bar-baz' directly; 'board ticket' catches the phrasing that precedes them in
    # prose instead. Deliberately NOT applied to source: a ticket id in a code comment sits
    # beside the reasoning it justifies and helps whoever reads the implementation, whereas
    # a ticket id in the wiki tells a reader to open a board they cannot reach.
    $docsOnlyLiterals = @(
        'board ticket'
    )

    if ($HostVocabularyOnly) {
        # Repository coverage is intentionally about the host vocabulary table. The other
        # internal literals above are package-publicity rules and legitimately occur in build,
        # board and maintenance files that never ship. docsOnlyLiterals is cleared for the same
        # reason: the .md scan unions the two lists, so leaving it set would flag every board
        # and maintenance doc that merely says "board ticket" in the repository-wide sweep.
        $blockedLiterals = @()
        $docsOnlyLiterals = @()
    }
    # Per-file exemptions: staged-relative path -> patterns allowed in that file. A pattern
    # is either a literal from the list above or a rule's Pattern string from
    # Get-HostVocabularyRules. Keep each one justified; an exemption is how a guard is
    # hollowed out one file at a time.
    $fileExemptions = @{
        # 'Drone' here is the musical sense - a sustained, unarticulated bed voice in the
        # synth parameter set - and has nothing to do with the host project the literal
        # blocks. The rule stays exactly as broad as it is for every other file.
        'Source\PinWright\Private\AudioGen\PwMusicScore.cpp' = @('Drone')
        'Source\PinWright\Private\AudioGen\PwMusicScore.h'   = @('Drone')
    }
    if ($HostVocabularyOnly) {
        $allHostPatterns = @(Get-HostVocabularyRules | ForEach-Object { $_.Pattern })
        # These files explain or implement the guard and must name what they prohibit - the
        # local rules file IS the table, which is why it is untracked.
        # No product code, test, example, format reference, wiki page or ordinary plan is exempt.
        $fileExemptions['scripts\package-fab.local.ps1'] = $allHostPatterns
        $fileExemptions['CLAUDE.md'] = $allHostPatterns
        $fileExemptions['ci\README.md'] = $allHostPatterns
        $fileExemptions['scripts\package-fab.ps1'] = $allHostPatterns
    }
    # Every extension that ships as text. '.py' was absent, so the whole shipped
    # Content/Python surface went unscanned; '.pwmodel' and '.mgir' ship under Examples/
    # and are comment-heavy authoring documents - prose is where host vocabulary hides.
    $textExtensions = @('.Build.cs', '.cpp', '.cs', '.h', '.ini', '.js', '.json', '.md', '.mgir', '.ps1', '.py', '.pwanim', '.pwmodel', '.pwskel', '.txt', '.uplugin')
    $extensionlessTextNames = @()

    # Compile once for the whole tree rather than once per file.
    $ignoreCase = [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
    $hostRules = @()
    foreach ($rule in Get-HostVocabularyRules) {
        $caseSensitive = $rule.ContainsKey('CaseSensitive') -and [bool]$rule.CaseSensitive
        $options = if ($caseSensitive) { [System.Text.RegularExpressions.RegexOptions]::None } else { $ignoreCase }
        $hostRules += [pscustomobject]@{
            Pattern = $rule.Pattern
            Why     = $rule.Why
            Regex   = [regex]::new('\b(?:' + $rule.Pattern + ')\b', $options)
            Allow   = if ($rule.ContainsKey('Allow')) { [regex]::new($rule.Allow, $ignoreCase) } else { $null }
        }
    }

    # Collect every finding and report them together. Throwing on the first one turns a
    # ten-minute cleanup into ten package runs.
    $findings = New-Object 'System.Collections.Generic.List[string]'

    Get-ChildItem -LiteralPath $StagePluginRoot -Recurse -File | ForEach-Object {
        $name = $_.Name
        $extension = $_.Extension
        $isText = $textExtensions -contains $extension -or $name.EndsWith('.Build.cs', [System.StringComparison]::OrdinalIgnoreCase) -or $extensionlessTextNames -contains $name
        if (-not $isText) {
            return
        }

        $relative = ConvertTo-RelativePath -Root $StagePluginRoot -Path $_.FullName
        if ($ExcludePaths.Count -gt 0 -and
            (Test-PathMatchesPattern -RelativePath $relative -Patterns $ExcludePaths)) {
            return
        }
        $exemptPatterns = if ($fileExemptions.ContainsKey($relative)) { $fileExemptions[$relative] } else { @() }

        $text = Get-Content -Raw -LiteralPath $_.FullName
        if ([string]::IsNullOrEmpty($text)) {
            return
        }

        # Prose-only literals apply to the wiki surface, not to code comments.
        $literalsForFile = if ($extension -eq '.md') { $blockedLiterals + $docsOnlyLiterals } else { $blockedLiterals }

        foreach ($pattern in $literalsForFile) {
            if ($exemptPatterns -contains $pattern) {
                continue
            }
            $offset = $text.IndexOf($pattern, [System.StringComparison]::Ordinal)
            if ($offset -ge 0) {
                $loc = Get-TextLocation -Text $text -Offset $offset
                $findings.Add("  ${relative}:$($loc.Line) - internal reference '$pattern' | $($loc.Text.Trim())")
            }
        }

        foreach ($rule in $hostRules) {
            if ($exemptPatterns -contains $rule.Pattern) {
                continue
            }
            foreach ($match in $rule.Regex.Matches($text)) {
                $loc = Get-TextLocation -Text $text -Offset $match.Index
                if ($null -ne $rule.Allow) {
                    # Suppress only when an Allow match SPANS this hit. Asking merely
                    # whether the line matches Allow lets one legitimate use elsewhere on
                    # the line hide a real leak - measured, not hypothetical.
                    $column = $match.Index - $loc.Start
                    $spanned = $false
                    foreach ($allowMatch in $rule.Allow.Matches($loc.Text)) {
                        if ($column -ge $allowMatch.Index -and $column -lt ($allowMatch.Index + $allowMatch.Length)) {
                            $spanned = $true
                            break
                        }
                    }
                    if ($spanned) {
                        continue
                    }
                }
                $findings.Add("  ${relative}:$($loc.Line) - host vocabulary '$($match.Value)' ($($rule.Why)) | $($loc.Text.Trim())")
            }
        }
    }

    if ($findings.Count -gt 0) {
        $shown = @($findings | Select-Object -First 40)
        $more = if ($findings.Count -gt $shown.Count) { "`n  ... and $($findings.Count - $shown.Count) more not listed." } else { '' }
        $scope = if ($HostVocabularyOnly) { 'Repository text' } else { 'Staged package text' }
        $impact = if ($HostVocabularyOnly) {
            'Everything scanned here is plugin-owned code, tests or documentation.'
        } else {
            "Everything scanned here is the shipped surface, so each one reaches an end user's source tree or wiki."
        }
        throw ("$scope carries $($findings.Count) blocked reference(s). $impact`n" +
            [string]::Join("`n", $shown) + $more +
            "`nRewrite the text generically. If a hit is a genuine generic use of the word, either give the rule an Allow regex in Get-HostVocabularyRules or add the staged-relative path to this check's `$fileExemptions with the pattern that fired - never delete the rule.")
    }
}

function Assert-ProductFactsConsistent {
    # Every number this plugin publishes - version, operation count, namespace count, UE
    # range - has exactly one source: product-facts.json at the plugin root, written by
    # scripts\gen-product-facts.ps1 from the live handler registry, the descriptor and the
    # engine version list in that script. This gate fails the package when staged copy
    # states a different one.
    # It exists because hand-maintained copies published "1,300+ operations" against a real
    # figure of 1,169.
    #
    # Scope is the published documentation surface (.md) plus the descriptor - NOT C++/Python
    # source. Engine-compat comments in source legitimately name other ranges ("UE 5.3-5.7"
    # for an API that changed in 5.8) and are per-API notes, not product claims.
    param([Parameter(Mandatory = $true)][string]$StagePluginRoot)

    $factsPath = Join-Path (Get-FullPath $PluginRoot) 'product-facts.json'
    if (-not (Test-Path -LiteralPath $factsPath)) {
        if ($ValidateOnly -or $DryRun) {
            Write-Warning "Published-number consistency was NOT validated: $factsPath is absent. Run scripts\gen-product-facts.ps1 (it needs a registry.json from one editor launch). A real package run fails instead of warning."
            return
        }
        throw "Cannot package: $factsPath is missing, so no staged number can be checked against the single source of truth. Launch the editor once to write registry.json, then run scripts\gen-product-facts.ps1."
    }

    $facts = Read-JsonFile $factsPath
    $factOperations = [int]$facts.operations
    $factNamespaces = [int]$facts.namespaceCount
    $factTests = [int]$facts.tests
    $factOperationsRounded = [int](($facts.operationsRounded -replace '[^0-9]', ''))
    $factNamespacesRounded = [int](($facts.namespacesRounded -replace '[^0-9]', ''))
    $factTestsRounded = [int](($facts.testsRounded -replace '[^0-9]', ''))
    $factUeRange = [string]$facts.ueRange

    # A staged descriptor version that differs from the facts is fatal on its own: it means
    # the release is being cut at a version the published copy was never regenerated for
    # (including a -VersionName override, which Update-StagedDescriptor already applied).
    $stagedDescriptor = Read-JsonFile (Join-Path $StagePluginRoot 'PinWright.uplugin')
    if ([string]$stagedDescriptor.VersionName -ne [string]$facts.version) {
        throw "Staged PinWright.uplugin VersionName is '$($stagedDescriptor.VersionName)' but product-facts.json says '$($facts.version)'. Re-run scripts\gen-product-facts.ps1 so the published copy matches the version being shipped."
    }

    # All four UE-range spellings in circulation after an explicit UE/Unreal Engine
    # introducer: UE 5.3-5.8, UE 5.3-5.8 (en/em dash), UE 5.3 to 5.8, UE 5.3 - 5.8.
    # Requiring that context avoids treating ordinary dotted numeric ranges such as
    # "1.0-1.25 canopy diameters" as product compatibility claims. Dashes come from char
    # codes so this file stays pure ASCII (Windows PowerShell would otherwise decode a
    # BOM-less source as ANSI and break the literals).
    $dashClass = '\-' + [char]0x2013 + [char]0x2014
    $rangePattern = [regex]::new('(?:\bUE\b|Unreal\s+Engine)\s*(?<lo>\d+\.\d+)\s*(?:[' + $dashClass + ']|to)\s*(?<hi>\d+\.\d+)', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    # Claim patterns. Each carries a floor: below it the number is a per-namespace or example
    # figure ("45 methods" on a namespace page, "42/100 tests" in a sample log line), not a
    # product-scale claim. Product figures are an order of magnitude larger.
    $claimPatterns = @(
        @{ Label = 'operation count'; Floor = 500;  Exact = $factOperations; Rounded = $factOperationsRounded; Regex = [regex]::new('(?<!RFC[- ])(?<![\d,])(?<num>\d[\d,]*)\s*(?<plus>\+?)\s*(?:registered\s+)?(?:RPC\s+)?(?:operations|methods|RPCs)', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase) },
        @{ Label = 'namespace count'; Floor = 20;   Exact = $factNamespaces; Rounded = $factNamespacesRounded; Regex = [regex]::new('(?<num>\d[\d,]*)\s*(?<plus>\+?)\s*(?:public\s+)?namespaces', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase) },
        @{ Label = 'test count';      Floor = 1000; Exact = $factTests;      Rounded = $factTestsRounded;      Regex = [regex]::new('(?<num>\d[\d,]*)\s*(?<plus>\+?)\s*tests', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase) }
    )
    # A version claim is only a claim when it is marked as one ("v0.7.0", "PinWright 0.7.0");
    # a bare dotted triple is usually an address or an engine version.
    $versionPattern = [regex]::new('(?:PinWright\s+v?|\bv)(?<ver>\d+\.\d+\.\d+)')

    # Per-file exemptions: staged-relative path -> claim literals allowed in that file.
    $fileExemptions = @{
        # A per-API engine-compat gotcha about NiagaraEditor helpers, not the supported range.
        'docs\wiki-src\niagara.authoring.md' = @('5.3-5.7')
        # Which array an Input Mapping Context stores its mappings in, per engine, not the
        # supported range: 5.7 moved the live data to DefaultKeyMappings.Mappings.
        'docs\wiki-src\input.md' = @('5.3-5.6')
    }
    $textExtensions = @('.md', '.uplugin')

    Get-ChildItem -LiteralPath $StagePluginRoot -Recurse -File | ForEach-Object {
        if ($textExtensions -notcontains $_.Extension) {
            return
        }

        $relative = ConvertTo-RelativePath -Root $StagePluginRoot -Path $_.FullName
        $exempt = if ($fileExemptions.ContainsKey($relative)) { $fileExemptions[$relative] } else { @() }
        # Explicit UTF-8 so en-dashed ranges decode as en-dashes instead of ANSI mojibake.
        $text = [System.IO.File]::ReadAllText($_.FullName, [System.Text.Encoding]::UTF8)

        foreach ($match in $rangePattern.Matches($text)) {
            $range = $match.Groups['lo'].Value + '-' + $match.Groups['hi'].Value
            if ($range -eq $factUeRange -or $exempt -contains $range) {
                continue
            }
            throw "Staged text states engine range '$($match.Value)' in $relative, but product-facts.json says the supported range is '$factUeRange'. Fix the text, or add the literal to this check's per-file exemptions if it is a per-API compat note rather than a product claim."
        }

        foreach ($claim in $claimPatterns) {
            foreach ($match in $claim.Regex.Matches($text)) {
                $number = [int]($match.Groups['num'].Value -replace ',', '')
                if ($number -lt $claim.Floor) {
                    continue
                }
                $expected = if ($match.Groups['plus'].Value -eq '+') { $claim.Rounded } else { $claim.Exact }
                if ($number -ne $expected) {
                    throw "Staged text states $($claim.Label) '$($match.Value)' in $relative, but product-facts.json says $($claim.Exact) (published as $($claim.Rounded)+). Re-run scripts\gen-product-facts.ps1 and update the text."
                }
            }
        }

        foreach ($match in $versionPattern.Matches($text)) {
            if ($match.Groups['ver'].Value -ne [string]$facts.version) {
                throw "Staged text states version '$($match.Value)' in $relative, but product-facts.json says '$($facts.version)'."
            }
        }
    }
}

function Assert-Icon128 {
    param([Parameter(Mandatory = $true)][string]$StagePluginRoot)

    $icon = Join-Path $StagePluginRoot 'Resources\Icon128.png'
    if (-not (Test-Path -LiteralPath $icon)) {
        throw 'Resources\Icon128.png is missing.'
    }

    Add-Type -AssemblyName System.Drawing
    $image = [System.Drawing.Image]::FromFile($icon)
    try {
        if ($image.Width -ne 128 -or $image.Height -ne 128) {
            throw "Resources\Icon128.png must be 128x128, got $($image.Width)x$($image.Height)."
        }
    }
    finally {
        $image.Dispose()
    }
}

function Assert-CopyrightHeaders {
    param([Parameter(Mandatory = $true)][string]$StagePluginRoot)

    $sourceExtensions = @('.cpp', '.h', '.cs', '.py')

    Get-ChildItem -LiteralPath $StagePluginRoot -Recurse -File | ForEach-Object {
        if ($sourceExtensions -notcontains $_.Extension) {
            return
        }

        $firstLines = @(Get-Content -LiteralPath $_.FullName -TotalCount 5)
        $hasHeader = $false
        foreach ($line in $firstLines) {
            if ($line -match 'Copyright \(c\) \d{4}') {
                $hasHeader = $true
                break
            }
        }
        if (-not $hasHeader) {
            $relative = ConvertTo-RelativePath -Root $StagePluginRoot -Path $_.FullName
            throw "Staged source file is missing a copyright header: $relative"
        }
    }
}

function Assert-NoExecutableNameLiterals {
    # Mirrors Fab's detect-exe-string-literal scanner (requirement 4.3.6.1.e): no staged
    # source line may mention a .exe or .msi file name, even inside comments.
    param([Parameter(Mandatory = $true)][string]$StagePluginRoot)

    $sourceExtensions = @('.cpp', '.h', '.cs', '.py')
    $pattern = [regex]::new('\.(exe|msi)\b', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)

    Get-ChildItem -LiteralPath $StagePluginRoot -Recurse -File | ForEach-Object {
        if ($sourceExtensions -notcontains $_.Extension) {
            return
        }

        $lineNumber = 0
        foreach ($line in Get-Content -LiteralPath $_.FullName) {
            $lineNumber++
            if ($pattern.IsMatch($line)) {
                $relative = ConvertTo-RelativePath -Root $StagePluginRoot -Path $_.FullName
                throw "Staged source file mentions an executable name (.exe/.msi) at ${relative}:${lineNumber}"
            }
        }
    }
}

function Assert-NoReservedFolderNames {
    # Epic's ingestion treats folders named like local build output (Binaries, Build,
    # Intermediate, Saved, DerivedDataCache) as strippable ANYWHERE in the tree, not just
    # at the plugin root - a source subfolder named Build gets deleted before their
    # BuildPlugin compile and every #include into it fails with C1083. Ban the names outright.
    param([Parameter(Mandatory = $true)][string]$StagePluginRoot)

    $reserved = @('Binaries', 'Build', 'Intermediate', 'Saved', 'DerivedDataCache')
    Get-ChildItem -LiteralPath $StagePluginRoot -Recurse -Directory | ForEach-Object {
        if ($reserved -contains $_.Name) {
            $relative = ConvertTo-RelativePath -Root $StagePluginRoot -Path $_.FullName
            throw "Staged tree contains a reserved folder name Epic's pipeline strips: $relative"
        }
    }
}

function Get-BuildCsModuleNames {
    # Engine module names a Build.cs references: the four *ModuleNames lists (both the
    # AddRange(new string[]{...}) array form and single .Add("X") calls) plus the main
    # module's TryAddConditionalModule() helper, whose third argument is forwarded to
    # PrivateDependencyModuleNames. Comments are stripped first so commented-out or merely
    # documented module names never register. Conditional (if-wrapped) adds are collected
    # exactly like unconditional ones - the invariant has to hold whenever the guarded
    # branch is the one that compiles.
    param([Parameter(Mandatory = $true)][string]$Text)

    $stripped = [regex]::Replace($Text, '/\*[\s\S]*?\*/', ' ')
    $stripped = [regex]::Replace($stripped, '//[^\r\n]*', ' ')

    $names = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
    $listPattern = '(?:Public|Private)(?:Dependency|IncludePath)ModuleNames'

    foreach ($match in [regex]::Matches($stripped, $listPattern + '\s*\.\s*AddRange\s*\(\s*new\s*(?:string\s*)?\[\s*\]\s*\{(?<body>[^}]*)\}')) {
        foreach ($literal in [regex]::Matches($match.Groups['body'].Value, '"([^"]+)"')) {
            [void]$names.Add($literal.Groups[1].Value)
        }
    }
    foreach ($match in [regex]::Matches($stripped, $listPattern + '\s*\.\s*Add\s*\(\s*"(?<name>[^"]+)"')) {
        [void]$names.Add($match.Groups['name'].Value)
    }
    foreach ($match in [regex]::Matches($stripped, 'TryAddConditionalModule\s*\(\s*[^,()]+,\s*[^,()]+,\s*"(?<name>[^"]+)"')) {
        [void]$names.Add($match.Groups['name'].Value)
    }

    return @($names)
}

function Assert-PluginRefsCoverModuleDeps {
    # Every engine-plugin-owned module a staged Build.cs names must have its owning plugin
    # referenced with "Enabled": true in the staged PinWright.uplugin.
    #
    # Why: UBT drops a plugin reference marked "Enabled": false from the build entirely, so
    # the owning plugin's public include paths are never registered and the compile dies
    # with C1083. That is precisely how Epic's Fab review rejected a submission - "Cannot
    # open include file" for Components/DynamicMeshComponent.h (GeometryFramework),
    # BoolColumn.h (Chooser) and Widgets/CommonActivatableWidgetContainer.h (CommonUI) -
    # while the same package built clean locally, because other plugins the descriptor does
    # enable (MovieRenderPipeline / Interchange / ChaosCloth -> Dataflow /
    # SkeletalMeshModelingTools) pulled those include paths in transitively. Only a static
    # check catches that drift; the local build cannot.
    #
    # The module -> owning-plugin map is derived from the engine at validation time
    # (<EngineRoot>\Engine\Plugins\**\*.uplugin), never hardcoded. Modules that belong to no
    # plugin (Core, Engine, UnrealEd, Json, Slate, ...) and this plugin's own modules are
    # ignored.
    param(
        [Parameter(Mandatory = $true)][string]$StagePluginRoot,
        [string]$EngineRoot = ''
    )

    # Plugin-owned modules that legitimately carry no descriptor reference. Keep this list
    # tiny and justified - anything absent from it must be declared. A reference that exists
    # but is disabled is never exempt: that is the failure mode this check exists for.
    $undeclaredExemptions = @{
        'StructUtils' = 'linked only on UE 5.4 and older (folded into CoreUObject from 5.5), so a permanent descriptor reference would be dead weight on every newer engine'
    }
    $reportedExemptions = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)

    if ([string]::IsNullOrWhiteSpace($EngineRoot)) {
        foreach ($candidate in @('C:\UE_5.8', 'C:\UE_5.7', 'C:\UE_5.6', 'C:\UE_5.5', 'C:\UE_5.4', 'C:\UE_5.3')) {
            if (Test-Path -LiteralPath (Join-Path $candidate 'Engine\Plugins')) {
                $EngineRoot = $candidate
                break
            }
        }
    }
    $enginePluginsDir = if ([string]::IsNullOrWhiteSpace($EngineRoot)) { '' } else { Join-Path $EngineRoot 'Engine\Plugins' }
    if ([string]::IsNullOrWhiteSpace($enginePluginsDir) -or -not (Test-Path -LiteralPath $enginePluginsDir)) {
        Write-Warning 'Plugin-reference coverage was NOT validated: no engine install found. The module -> owning-plugin map is built from <EngineRoot>\Engine\Plugins\**\*.uplugin, so without an engine this run cannot prove every hard-linked engine-plugin module has an "Enabled": true reference in PinWright.uplugin. Pass -EngineRoot to enable the check.'
        return
    }

    $moduleToPlugin = @{}
    $pluginRefs = @{}
    $enabledByDefault = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($descriptorFile in Get-ChildItem -LiteralPath $enginePluginsDir -Recurse -File -Filter *.uplugin) {
        try {
            $enginePlugin = Get-Content -Raw -LiteralPath $descriptorFile.FullName | ConvertFrom-Json
        }
        catch {
            continue
        }
        $enginePluginName = [System.IO.Path]::GetFileNameWithoutExtension($descriptorFile.Name)
        foreach ($module in @($enginePlugin.Modules)) {
            if ($null -eq $module -or [string]::IsNullOrWhiteSpace($module.Name)) {
                continue
            }
            if (-not $moduleToPlugin.ContainsKey($module.Name)) {
                $moduleToPlugin[$module.Name] = $enginePluginName
            }
        }
        if ([bool]$enginePlugin.EnabledByDefault) {
            [void]$enabledByDefault.Add($enginePluginName)
        }
        if (-not $pluginRefs.ContainsKey($enginePluginName)) {
            $pluginRefs[$enginePluginName] = @(@($enginePlugin.Plugins) | Where-Object { $null -ne $_ -and [bool]$_.Enabled -and -not [string]::IsNullOrWhiteSpace($_.Name) } | ForEach-Object { $_.Name })
        }
    }

    $descriptor = Read-JsonFile (Join-Path $StagePluginRoot 'PinWright.uplugin')
    $ownModules = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($module in @($descriptor.Modules)) {
        if ($null -ne $module -and -not [string]::IsNullOrWhiteSpace($module.Name)) {
            [void]$ownModules.Add($module.Name)
        }
    }
    $refEnabled = @{}
    foreach ($reference in @($descriptor.Plugins)) {
        if ($null -eq $reference -or [string]::IsNullOrWhiteSpace($reference.Name)) {
            continue
        }
        $refEnabled[$reference.Name] = [bool]$reference.Enabled
    }

    # Mirror UBT's plugin-reference BFS: a plugin is in the build if we enable it directly,
    # if the engine enables it by default, or if any plugin already in the closure enables
    # it (GeometryScripting -> GeometryProcessing is the live example - our descriptor never
    # names GeometryProcessing, yet DynamicMesh links fine because GeometryScripting pulls
    # it in).
    $enabledClosure = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $pending = New-Object 'System.Collections.Generic.Queue[string]'
    foreach ($name in @($enabledByDefault) + @($refEnabled.Keys | Where-Object { $refEnabled[$_] })) {
        if ($enabledClosure.Add($name)) {
            $pending.Enqueue($name)
        }
    }
    while ($pending.Count -gt 0) {
        $current = $pending.Dequeue()
        if (-not $pluginRefs.ContainsKey($current)) {
            continue
        }
        foreach ($name in $pluginRefs[$current]) {
            if ($enabledClosure.Add($name)) {
                $pending.Enqueue($name)
            }
        }
    }

    $checkedFiles = 0
    foreach ($buildFile in Get-ChildItem -LiteralPath (Join-Path $StagePluginRoot 'Source') -Recurse -File -Filter *.Build.cs) {
        $checkedFiles++
        $relative = ConvertTo-RelativePath -Root $StagePluginRoot -Path $buildFile.FullName
        foreach ($moduleName in Get-BuildCsModuleNames -Text (Get-Content -Raw -LiteralPath $buildFile.FullName)) {
            if ($ownModules.Contains($moduleName) -or -not $moduleToPlugin.ContainsKey($moduleName)) {
                continue
            }
            $owningPlugin = $moduleToPlugin[$moduleName]
            # A disabled reference is always fatal, even when something else would have
            # enabled the plugin: every name listed in a Plugins array is marked seen by the
            # engine's reference walk before any flag is read, so our disabled reference also
            # blocks the transitive enable that would otherwise have covered it.
            if ($refEnabled.ContainsKey($owningPlugin) -and -not $refEnabled[$owningPlugin]) {
                throw "$relative depends on engine module '$moduleName', owned by engine plugin '$owningPlugin', but PinWright.uplugin references '$owningPlugin' with `"Enabled`": false. UBT never adds a disabled reference to the build, so that plugin's public include paths are not registered and the compile fails with C1083 (Epic's Fab review rejected exactly this; it built clean locally only because other enabled plugins supplied the same include paths transitively). Set the reference to `"Enabled`": true, `"Optional`": true."
            }
            if ($enabledClosure.Contains($owningPlugin)) {
                continue
            }
            if ($undeclaredExemptions.ContainsKey($moduleName)) {
                if ($reportedExemptions.Add($moduleName)) {
                    Write-Host "Plugin-reference coverage: '$moduleName' (plugin '$owningPlugin') exempt from the reference requirement - $($undeclaredExemptions[$moduleName])."
                }
                continue
            }
            throw "$relative depends on engine module '$moduleName', owned by engine plugin '$owningPlugin', but nothing in PinWright.uplugin enables '$owningPlugin' (no reference, and no enabled reference reaches it). Add { `"Name`": `"$owningPlugin`", `"Enabled`": true, `"Optional`": true } to the Plugins array: without an enabled reference the plugin's public include paths are not registered and the compile fails with C1083, which is how Epic's Fab review rejected a submission that built clean locally."
        }
    }

    Write-Host "Plugin-reference coverage: validated $checkedFiles Build.cs file(s) against $EngineRoot ($($moduleToPlugin.Count) engine-plugin modules mapped)."
}

function Assert-PackageShape {
    param(
        [Parameter(Mandatory = $true)][string]$StagePluginRoot,
        [Parameter(Mandatory = $true)]$Manifest,
        [string]$EngineRoot = ''
    )

    Assert-RequiredFiles -StagePluginRoot $StagePluginRoot -RequiredFiles $Manifest.requiredFiles
    if ($Manifest.requiredPaths) {
        Assert-RequiredPaths -StagePluginRoot $StagePluginRoot -RequiredPaths $Manifest.requiredPaths
    }
    Assert-BlockedPathsAbsent -StagePluginRoot $StagePluginRoot -BlockedPaths $Manifest.blockedPaths
    Assert-DocsArePublic -StagePluginRoot $StagePluginRoot
    Assert-StagedTextIsPublic -StagePluginRoot $StagePluginRoot
    # The staged scan cannot see curated-out maintainer docs or automation tests. Scan the
    # repository too, before packaging, so exclusions cannot become a vocabulary blind spot.
    Assert-StagedTextIsPublic -StagePluginRoot (Get-FullPath $PluginRoot) -HostVocabularyOnly `
        -ExcludePaths @('.codex', '.git', '.polyskill', 'Binaries', 'Intermediate', 'Saved', 'dist', 'scratchpad')
    Assert-HostVocabularyIsCovered
    Assert-ProductFactsConsistent -StagePluginRoot $StagePluginRoot
    Assert-Icon128 -StagePluginRoot $StagePluginRoot
    Assert-CopyrightHeaders -StagePluginRoot $StagePluginRoot
    Assert-NoExecutableNameLiterals -StagePluginRoot $StagePluginRoot
    Assert-NoReservedFolderNames -StagePluginRoot $StagePluginRoot
    Assert-PluginRefsCoverModuleDeps -StagePluginRoot $StagePluginRoot -EngineRoot $EngineRoot
}

function Assert-ZipRootShape {
    param(
        [Parameter(Mandatory = $true)][string]$ZipPath,
        [Parameter(Mandatory = $true)][string]$RootFolderName
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $topLevelNames = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $archive.Entries) {
            if ([string]::IsNullOrWhiteSpace($entry.FullName)) {
                continue
            }
            $normalized = $entry.FullName.Replace('\', '/').Trim('/')
            if ([string]::IsNullOrWhiteSpace($normalized)) {
                continue
            }
            $topLevel = $normalized.Split('/')[0]
            [void]$topLevelNames.Add($topLevel)
        }

        if ($topLevelNames.Count -ne 1 -or -not $topLevelNames.Contains($RootFolderName)) {
            throw "Zip must contain exactly one top-level plugin folder named '$RootFolderName'. Found: $([string]::Join(', ', $topLevelNames))"
        }
    }
    finally {
        $archive.Dispose()
    }
}

if (-not [string]::IsNullOrWhiteSpace($HostVocabularyProbeRoot)) {
    Assert-StagedTextIsPublic -StagePluginRoot (Get-FullPath $HostVocabularyProbeRoot) -HostVocabularyOnly `
        -ExcludePaths @('.codex', '.git', '.polyskill', 'Binaries', 'Intermediate', 'Saved', 'dist', 'scratchpad')
    Write-Host 'Host-vocabulary probe: clean.'
    exit 0
}

$pluginRootFull = Get-FullPath $PluginRoot
$manifestPath = Join-Path $pluginRootFull 'scripts\release-manifest.json'
if (-not (Test-Path -LiteralPath (Join-Path $pluginRootFull 'PinWright.uplugin'))) {
    throw "Plugin root does not contain PinWright.uplugin: $pluginRootFull"
}
if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "Release manifest is missing: $manifestPath"
}

$manifest = Read-JsonFile $manifestPath
$outputDirFull = Get-FullPath $OutputDir
if (-not (Test-Path -LiteralPath $outputDirFull)) {
    New-Item -ItemType Directory -Path $outputDirFull | Out-Null
}

$stageRoot = Join-Path $outputDirFull '_stage'
$stagePluginRoot = Join-Path $stageRoot 'PinWright'
$zipPath = Join-Path $outputDirFull ($PackageName + '.zip')

if (-not (Test-IsStrictSubPath -Root $outputDirFull -Path $stageRoot)) {
    throw "Refusing to clean staging directory outside OutputDir: $stageRoot"
}

Remove-PathIfPresent $stageRoot
if (-not ($DryRun -or $ValidateOnly)) {
    Remove-PathIfPresent $zipPath
}
New-Item -ItemType Directory -Path $stagePluginRoot | Out-Null

$script:CopiedFiles = 0
$script:SkippedFiles = 0

foreach ($entry in $manifest.include) {
    Copy-ReleaseEntry -Entry $entry -SourceRoot $pluginRootFull -DestinationRoot $stagePluginRoot -ExcludePatterns $manifest.excludePaths | Out-Null
}

if ($manifest.createEmptyPaths) {
    foreach ($relative in $manifest.createEmptyPaths) {
        New-Item -ItemType Directory -Force -Path (Join-Path $stagePluginRoot $relative) | Out-Null
    }
}

$descriptorPath = Join-Path $stagePluginRoot 'PinWright.uplugin'
Update-StagedDescriptor -DescriptorPath $descriptorPath -Manifest $manifest -VersionName $VersionName -EngineVersion $EngineVersion -MarketplaceProductId $MarketplaceProductId
Assert-PackageShape -StagePluginRoot $stagePluginRoot -Manifest $manifest -EngineRoot $EngineRoot

$mode = if ($ValidateOnly) { 'validate-only' } elseif ($DryRun) { 'dry-run' } else { 'package' }
Write-Host "Mode: $mode"
Write-Host "Copied files: $script:CopiedFiles"
Write-Host "Excluded files: $script:SkippedFiles"
Write-Host "Stage: $stagePluginRoot"

if (-not ($DryRun -or $ValidateOnly)) {
    Compress-Archive -Path $stagePluginRoot -DestinationPath $zipPath -Force
    Assert-ZipRootShape -ZipPath $zipPath -RootFolderName 'PinWright'
    Write-Host "Zip: $zipPath"
} else {
    Write-Host "Zip: skipped"
}

if (-not $KeepStage) {
    Remove-PathIfPresent $stageRoot
}
