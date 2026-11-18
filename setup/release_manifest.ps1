# A release's manifest: every file of the release with its size and SHA-256,
# so an installed copy can tell which files a newer release changed and check
# each one it downloads. Dot-sourced by make_release.ps1 (and the update tests).
#
# setup\manifest.json inside a release lists that release's own files (all of
# them except itself). A feed folder holds the same manifest.json (with its
# signature manifest.json.sig, added by make_release.ps1) beside one
# file per distinct content, named by its SHA-256, which is the layout a
# release's assets on a host can have: flat names, so a GitHub release works.
#
# Format 1:
#   { "format": 1, "product": "Gamebreaker", "version": "0.1.1",
#     "commit": "abc1234", "released": "2026-11-17T08:00:00Z",
#     "notes": ["one line per change"],
#     "files_base": "",            where the files are: empty = beside the manifest
#     "files": [ { "path": "setup/setup.ps1", "size": 1234, "sha256": "<64 hex>" }, ... ] }
# Paths use forward slashes and are relative to the release's app folder.
#
# A release unpacks to a main folder holding only the starter and one subfolder:
#   Gamebreaker.exe   the starter, listed in the manifest as "Gamebreaker.exe"
#   app\              everything else; the manifest's paths start here
# A folder not named app (a development checkout) keeps the starter beside its files.

# Where a file named in a manifest is on disk.
function Get-GbReleaseFilePath([string]$AppRoot, [string]$Path) {
    if ($Path -ieq 'Gamebreaker.exe' -and (Split-Path $AppRoot -Leaf) -ieq 'app') { return (Join-Path (Split-Path $AppRoot) 'Gamebreaker.exe') }
    Join-Path $AppRoot ($Path.Replace('/', '\'))
}

# Naming the main folder (or app itself) as the install folder would fill the folder
# the player opens with build files, so the install goes to app\game instead.
function Resolve-GbInstallFolder([string]$AppRoot, [string]$Folder) {
    $app = [IO.Path]::GetFullPath($AppRoot).TrimEnd('\')
    if ((Split-Path $app -Leaf) -ine 'app') { return $Folder }
    $full = [IO.Path]::GetFullPath($Folder).TrimEnd('\')
    if ($full -ieq $app -or $full -ieq (Split-Path $app)) { return (Join-Path $app 'game') }
    $Folder
}

function Get-GbReleaseFiles([string]$Root) {
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    $entry = { param($file, $rel) [ordered]@{ path = $rel; size = $file.Length; sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } }
    $list = @(Get-ChildItem -LiteralPath $rootFull -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($rootFull.Length + 1).Replace('\', '/')
        if ($rel -eq 'setup/manifest.json') { return }
        & $entry $_ $rel
    })
    $starter = Get-GbReleaseFilePath $rootFull 'Gamebreaker.exe'
    if ((Split-Path $starter) -ine $rootFull -and (Test-Path -LiteralPath $starter -PathType Leaf)) { $list += & $entry (Get-Item -LiteralPath $starter) 'Gamebreaker.exe' }
    $list | Sort-Object { $_.path }
}

# Writes <Root>\setup\manifest.json and returns the manifest.
function New-GbManifest([string]$Root, [string]$Version, [string]$Commit, [string[]]$Notes = @(), [string]$FilesBase = '') {
    $m = [ordered]@{
        format = 1; product = 'Gamebreaker'; version = $Version; commit = $Commit
        released = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        notes = @($Notes | Where-Object { $_ }); files_base = $FilesBase
        files = @(Get-GbReleaseFiles $Root)
    }
    $json = $m | ConvertTo-Json -Depth 5
    [IO.File]::WriteAllText((Join-Path $Root 'setup\manifest.json'), $json + "`n", (New-Object Text.UTF8Encoding($false)))
    $m
}

# Writes a feed folder for a release folder that already has its manifest:
# manifest.json and one file per distinct SHA-256.
function Write-GbFeed([string]$Root, [string]$FeedDir) {
    New-Item -ItemType Directory -Force $FeedDir | Out-Null
    $mf = Join-Path $Root 'setup\manifest.json'
    Copy-Item -LiteralPath $mf (Join-Path $FeedDir 'manifest.json') -Force
    $m = Get-Content -LiteralPath $mf -Raw | ConvertFrom-Json
    foreach ($f in @($m.files)) {
        $to = Join-Path $FeedDir $f.sha256
        if (-not (Test-Path -LiteralPath $to)) { Copy-Item -LiteralPath (Get-GbReleaseFilePath $Root $f.path) $to }
    }
}
