# Mods on the player's PC, from the command line (the launcher runs installs
# through this so its window stays responsive; tests use every action).
#   mods.ps1 -Action list    -ModsDir <dir> [-GameFiles <dir>] [-LogFile <the game's logs\game.log>]
#   mods.ps1 -Action inspect -ModsDir <dir> [-GameFiles <dir>] -Zip <file>
#   mods.ps1 -Action install -ModsDir <dir> [-GameFiles <dir>] -Zip <file>
#   mods.ps1 -Action remove  -ModsDir <dir> -ModFolder <name>
#   mods.ps1 -Action order   -ModsDir <dir> -OrderFile <json file: [{"folder":"a","on":true}, ...]>
# Prints one JSON object (or, with -Events <file>, appends JSON lines there:
# modprogress, then mods (installed) or modfail). Exit code 0 on success.
param(
    [Parameter(Mandatory)][ValidateSet('list', 'inspect', 'install', 'remove', 'order')][string]$Action,
    [Parameter(Mandatory)][string]$ModsDir,
    [string]$GameFiles = '',
    [string]$Zip = '',
    [string]$ModFolder = '',
    [string]$OrderFile = '',
    [string]$LogFile = '',
    [string]$Events = '',
    [switch]$NoPrompt
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'mods_lib.ps1')

function Out-Ev($o) {
    $j = $o | ConvertTo-Json -Compress -Depth 5
    if ($Events) { [IO.File]::AppendAllText($Events, $j + "`n", (New-Object Text.UTF8Encoding($false))) } else { $j }
}
if (-not $GameFiles) { $g = Join-Path (Split-Path $ModsDir) 'game_files'; if (Test-Path -LiteralPath $g) { $GameFiles = $g } }

switch ($Action) {
    'list' { Clear-GbModTemp $ModsDir; Out-Ev ([ordered]@{ ev = 'mods'; list = (Get-GbModList $ModsDir $GameFiles $LogFile).ToArray() }); exit 0 }
    'inspect' { $i = Get-GbZipInfo $Zip $ModsDir $GameFiles; $i.ev = 'modinspect'; Out-Ev $i; exit $(if ($i.ok) { 0 } else { 1 }) }
    'install' {
        $last = [DateTime]::MinValue
        $r = Install-GbModZip $Zip $ModsDir $GameFiles {
            param($p)
            if ($Events -and (($p -ge 100) -or ([DateTime]::Now - $script:last).TotalMilliseconds -ge 120)) { $script:last = [DateTime]::Now; Out-Ev ([ordered]@{ ev = 'modprogress'; p = $p }) }
        }
        if ($r.ok) { Out-Ev ([ordered]@{ ev = 'mods'; folder = $r.folder; list = (Get-GbModList $ModsDir $GameFiles).ToArray() }); exit 0 }
        Out-Ev ([ordered]@{ ev = 'modfail'; reason = $r.reason; detail = $r.detail }); exit 1
    }
    'remove' { $r = Remove-GbMod $ModsDir $ModFolder; $r.ev = 'modremoved'; if (-not $r.folder) { $r.folder = $ModFolder }; Out-Ev $r; exit $(if ($r.ok) { 0 } else { 1 }) }
    'order' {
        $want = @(([IO.File]::ReadAllText($OrderFile) | ConvertFrom-Json) | ForEach-Object { $_ })   # Windows PowerShell hands a JSON array over as one object
        Set-GbModOrder $ModsDir $want
        Out-Ev ([ordered]@{ ev = 'modorder'; ok = $true }); exit 0
    }
}
