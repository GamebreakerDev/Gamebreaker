# Check files (the launcher's Settings, Advanced, Repair): compares an install
# and its release with what they should be, without downloading or building.
#
#   check_files.ps1 -Folder <install> [-Events <file>]
#
# What it checks, each against a record that already exists:
#   release files  every file of the release folder against setup\manifest.json
#                  (written by make_release.ps1; a development checkout has none)
#   toolkit        the downloaded toolkit's file fingerprint against setup\tools.lock
#   tools          each pinned tool is in place with its marker (as setup checks)
#   game files     game_files\default.xbe against the supported executable's SHA-256
#   game           build\gamebreaker.exe exists
#
# What it changes: when the toolkit or a tool fails, it deletes that tool's
# marker, so the next Rebuild downloads it again. Nothing else is written.
# The result says what fixes each problem: Rebuild, the release zip, or the disc.

param(
    [Parameter(Mandatory)] [string]$Folder,
    [string]$Events = '',
    [switch]$NoPrompt     # accepted for the launcher's engine runner
)

$ErrorActionPreference = 'Stop'
$src = Split-Path $PSScriptRoot
. (Join-Path $src 'port\tools\pipeline.ps1')
. (Join-Path $PSScriptRoot 'tree_hash.ps1')
. (Join-Path $PSScriptRoot 'release_manifest.ps1')
$utf8 = New-Object Text.UTF8Encoding($false)
function Emit([hashtable]$o) {
    if (-not $Events) { return }
    $line = ($o | ConvertTo-Json -Compress -Depth 4) + "`n"
    for ($i = 0; $i -lt 10; $i++) { try { [IO.File]::AppendAllText($Events, $line, $utf8); return } catch { Start-Sleep -Milliseconds 25 } }
}
$results = New-Object System.Collections.ArrayList
function Report([string]$part, $ok, [string]$text, [string]$fix = '') {
    [void]$results.Add(@{ part = $part; ok = $ok; text = $text; fix = $fix })
    Emit @{ ev = 'checkfiles'; part = $part; ok = $ok; text = $text; fix = $fix }
    Write-Host ("{0,-14} {1}  {2}" -f $part, $(if ($ok -eq $true) { 'ok  ' } elseif ($ok -eq $false) { 'FAIL' } else { '-   ' }), $text)
}

try {
    $Folder = [IO.Path]::GetFullPath($Folder).TrimEnd('\')
    $tools = Join-Path $Folder 'tools'
    Emit @{ ev = 'checkfiles'; state = 'start' }

    # 1. The release's own files.
    $mf = Join-Path $src 'setup\manifest.json'
    if (Test-Path (Join-Path $src '.git')) { Report 'release files' $null 'a development checkout: checked by git, not here' }
    elseif (-not (Test-Path $mf)) { Report 'release files' $null 'this release has no file list (made before 2026-11-17)' }
    else {
        $m = Get-Content $mf -Raw -Encoding UTF8 | ConvertFrom-Json
        $bad = @()
        foreach ($f in @($m.files)) {
            $p = Get-GbReleaseFilePath $src ([string]$f.path)
            if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { $bad += "$($f.path) (missing)"; continue }
            if ((Get-Item -LiteralPath $p).Length -ne [long]$f.size -or (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant() -ne $f.sha256) { $bad += "$($f.path) (changed)" }
        }
        if ($bad) { Report 'release files' $false ("Release files: {0} of {1} differ: {2}" -f $bad.Count, @($m.files).Count, (($bad | Select-Object -First 5) -join ', ')) 'release' }
        else { Report 'release files' $true ("{0} files match version {1}" -f @($m.files).Count, $m.version) }
    }

    # 2 and 3. The toolkit and the other pinned tools.
    $entries = (Get-Content (Join-Path $PSScriptRoot 'tools.lock') -Raw | ConvertFrom-Json).tools
    $tkEntry = $entries | Where-Object name -eq 'xboxrecomp'
    $tkLocal = Join-Path $src 'xboxrecomp'
    $tkUsedLocal = $false
    if ($tkEntry -and (Test-Path $tkLocal)) { $tkUsedLocal = ((Get-GbTreeHash $tkLocal).Hash -eq $tkEntry.tree_sha256) }
    foreach ($t in $entries) {
        $dir = Join-Path $tools $t.dir
        $marker = Join-Path $dir '.gb_tool'
        $pin = $(if ($t.verify -eq 'tree') { $t.tree_sha256 } else { $t.sha256 })
        if ($t.name -eq 'xboxrecomp' -and $tkUsedLocal) { Report 'toolkit' $true 'the copy beside setup matches the pinned fingerprint'; continue }
        $present = (Test-Path (Join-Path $dir $t.check)) -and (Test-Path $marker) -and ((Get-Content $marker -Raw).Trim() -eq $pin)
        if (-not $present) {
            if (Test-Path $marker) { Remove-Item $marker -Force }
            Report $(if ($t.name -eq 'xboxrecomp') { 'toolkit' } else { "tool $($t.name)" }) $false "$($t.name) $($t.version) is missing or incomplete" 'rebuild'
            continue
        }
        if ($t.verify -eq 'tree') {
            $fp = Get-GbTreeHash (Join-Path $dir $t.tree_root)
            if ($fp.Hash -ne $t.tree_sha256) { Remove-Item $marker -Force; Report 'toolkit' $false "the toolkit's files differ from the pinned version ($($fp.Files) files)" 'rebuild' }
            else { Report 'toolkit' $true "$($fp.Files) files match the pinned fingerprint" }
        } else { Report "tool $($t.name)" $true "$($t.name) $($t.version) in place" }
    }

    # 4. The game's own files (from the player's disc).
    $xbe = Join-Path $Folder 'game_files\default.xbe'
    if (-not (Test-Path $xbe)) { Report 'game files' $false 'game_files\default.xbe is missing' 'disc' }
    else {
        try { $null = Assert-GbXbe $xbe; Report 'game files' $true 'default.xbe is the supported release' }
        catch { Report 'game files' $false 'game_files\default.xbe no longer matches the supported release' 'disc' }
    }

    # 5. The built game.
    $exe = Join-Path $Folder 'build\gamebreaker.exe'
    if (Test-Path $exe) { Report 'game' $true ("build\gamebreaker.exe, {0:N1} MB" -f ((Get-Item $exe).Length / 1MB)) }
    else { Report 'game' $false 'build\gamebreaker.exe is missing' 'rebuild' }

    $fails = @($results | Where-Object { $_.ok -eq $false })
    $fix = $(if ($fails | Where-Object { $_.fix -eq 'release' }) { 'release' } elseif ($fails | Where-Object { $_.fix -eq 'disc' }) { 'disc' } elseif ($fails) { 'rebuild' } else { 'none' })
    Emit @{ ev = 'checkfiles'; state = 'done'; problems = $fails.Count; fix = $fix; list = @($fails | ForEach-Object { $_.text }) }
    exit 0
} catch {
    Emit @{ ev = 'error'; step = 'checkfiles'; code = 'checkfiles'; text = $_.Exception.Message }
    [Console]::Error.WriteLine("check_files: $($_.Exception.Message)")
    exit 1
}
