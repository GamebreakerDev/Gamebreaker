# Updates a release folder (the unpacked zip) to a newer release from its feed.
# The release folder here is the zip's app\ subfolder; the one file outside it,
# the starter Gamebreaker.exe in the main folder, is replaced like any other.
#
#   update.ps1 -Action check     fetch the feed's manifest, compare, say what an update would change
#   update.ps1 -Action install   download the changed files, check each, then replace them
#                                (the old ones are kept in update\backup\<old version>\)
#   update.ps1 -Action rollback  put the old files back (and the game program, with -Folder)
#   update.ps1 -Action finish    after a successful rebuild: mark done, delete the staging files
#
# The launcher runs these and then setup.ps1 (which lifts and compiles again only
# what the changed files touch). Events go to -Events as JSON lines, like setup's.
#
# Where releases come from is a setting, `feed=` in setup\release.txt (written by
# make_release.ps1 -Feed). It is a folder or an http(s) address holding
# manifest.json and one file per SHA-256 (setup\release_manifest.ps1). No
# address is built into this script.
#
# What it never touches: anything outside the release folder; and inside it,
# the player's settings (launcher\prototype\state.json), the remembered
# install folder (setup\folder.txt), and the saves, screenshots, logs, game
# files, tools and build folders, should the install folder be the release
# folder itself. A manifest naming any of those is refused whole.
#
# The list itself must be signed with the project's key: manifest.json.sig
# beside it on the feed, checked against setup\release_key.txt before the list
# is read and before any file of the release is downloaded.
#
# Every step is undoable: update\journal.json is written before any file
# moves, and rollback works from it after a crash part-way through.

param(
    [Parameter(Mandatory)] [ValidateSet('check', 'install', 'rollback', 'finish')] [string]$Action,
    [string]$Folder = '',        # the install folder: its build\gamebreaker.exe is kept and restored
    [string]$Events = '',
    [string]$Reason = '',        # rollback: why, for the page
    [switch]$NoPrompt            # accepted for the launcher's engine runner; nothing here prompts
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$rootDir = $(if ($Root) { [IO.Path]::GetFullPath($Root).TrimEnd('\') } else { (Split-Path $PSScriptRoot) })
$upDir = Join-Path $rootDir 'update'
$journalFile = Join-Path $upDir 'journal.json'
$pendingFile = Join-Path $upDir 'pending.json'
$pendingList = Join-Path $upDir 'pending-manifest.json'
$pendingSig = Join-Path $upDir 'pending-manifest.json.sig'
$utf8 = New-Object Text.UTF8Encoding($false)
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

function Emit([hashtable]$o) {
    if (-not $Events) { return }
    $line = ($o | ConvertTo-Json -Compress -Depth 4) + "`n"
    for ($i = 0; $i -lt 10; $i++) { try { [IO.File]::AppendAllText($Events, $line, $utf8); return } catch { Start-Sleep -Milliseconds 25 } }
}
function Fail([string]$code, [string]$msg) { $e = New-Object Exception $msg; $e.Data['code'] = $code; throw $e }
function Save-Json($obj, [string]$path) {
    $tmp = "$path.tmp"
    [IO.File]::WriteAllText($tmp, ($obj | ConvertTo-Json -Depth 6), $utf8)
    Move-Item -LiteralPath $tmp $path -Force
}
function Read-Json([string]$path) { if (Test-Path -LiteralPath $path) { Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json } }
function Sha([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant() }

function Read-Release {
    $r = @{}
    $f = Join-Path $rootDir 'setup\release.txt'
    if (Test-Path $f) { foreach ($l in Get-Content $f) { if ($l -match '^([a-z_0-9]+)=(.*)$') { $r[$Matches[1]] = $Matches[2].Trim() } } }
    $r
}

# Versions: 1.2.3 or 1.2.3-tag. A release is newer than any -tag of the same number.
function Split-Version([string]$v) {
    if ($v -notmatch '^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.]+))?$') { return $null }
    @{ n = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3]); tag = [string]$Matches[4] }
}
function Compare-Version([string]$a, [string]$b) {
    $x = Split-Version $a; $y = Split-Version $b
    for ($i = 0; $i -lt 3; $i++) { if ($x.n[$i] -ne $y.n[$i]) { return [math]::Sign($x.n[$i] - $y.n[$i]) } }
    if ($x.tag -eq $y.tag) { return 0 }
    if (-not $x.tag) { return 1 }
    if (-not $y.tag) { return -1 }
    $p = $x.tag.Split('.'); $q = $y.tag.Split('.')
    for ($i = 0; $i -lt [math]::Min($p.Count, $q.Count); $i++) {
        $pi = 0; $qi = 0
        if ([int]::TryParse($p[$i], [ref]$pi) -and [int]::TryParse($q[$i], [ref]$qi)) { if ($pi -ne $qi) { return [math]::Sign($pi - $qi) } }
        else { $c = [string]::CompareOrdinal($p[$i], $q[$i]); if ($c) { return [math]::Sign($c) } }
    }
    [math]::Sign($p.Count - $q.Count)
}

# Paths an update may never write, move or delete (lower case, forward slashes).
$protectedDirs = @('update/', 'game/', 'saves/', 'screenshots/', 'logs/', 'game_files/', 'tools/', 'build/', 'state/', 'launcher/prototype/lib/', 'launcher/prototype/.webview/', '.git/')
$protectedFiles = @('launcher/prototype/state.json', 'launcher/prototype/last-run.txt', 'setup/folder.txt', 'setup/release_private.txt', 'setup/manifest.json')
function Test-SafePath([string]$p) {
    if ($p -notmatch '^[A-Za-z0-9._ +()-]+(/[A-Za-z0-9._ +()-]+)*$') { return $false }
    foreach ($seg in $p.Split('/')) { if ($seg -eq '.' -or $seg -eq '..' -or $seg.EndsWith('.') -or $seg.EndsWith(' ')) { return $false } }
    $l = $p.ToLowerInvariant()
    if ($protectedFiles -contains $l) { return $false }
    foreach ($d in $protectedDirs) { if ($l.StartsWith($d)) { return $false } }
    $true
}
function Local-Path([string]$p) {
    if ($p -ieq 'Gamebreaker.exe') { return (Get-GbReleaseFilePath $rootDir $p) }   # the starter, in the main folder above app\
    $full = [IO.Path]::GetFullPath((Join-Path $rootDir ($p.Replace('/', '\'))))
    if (-not $full.StartsWith($rootDir + '\', 'OrdinalIgnoreCase')) { Fail 'manifest' "A path in the update leaves the release folder: $p" }
    $full
}

# A manifest from the feed, checked field by field before anything uses it.
function Test-Manifest($m, [string]$what) {
    if ($m.format -ne 1) { Fail 'manifest' "The $what is in a format this launcher does not know (format $($m.format)). Download the new release by hand." }
    if ($m.product -ne 'Gamebreaker') { Fail 'manifest' "The $what is not a Gamebreaker release." }
    if (-not (Split-Version ([string]$m.version))) { Fail 'manifest' "The $what has no valid version." }
    $files = @($m.files)
    if ($files.Count -lt 1 -or $files.Count -gt 5000) { Fail 'manifest' "The $what lists $($files.Count) files." }
    $seen = @{}
    foreach ($f in $files) {
        $p = [string]$f.path
        if (-not (Test-SafePath $p)) { Fail 'manifest' "The $what names a file an update may not write: $p" }
        if ($seen.ContainsKey($p.ToLowerInvariant())) { Fail 'manifest' "The $what names a file twice: $p" }
        $seen[$p.ToLowerInvariant()] = $true
        if ([string]$f.sha256 -notmatch '^[0-9a-f]{64}$') { Fail 'manifest' "The $what has no valid SHA-256 for $p." }
        if ([long]$f.size -lt 0 -or [long]$f.size -gt 64MB) { Fail 'manifest' "The $what gives $p an impossible size." }
    }
}

# Reading from the feed: an http(s) address or a folder.
function Join-Feed([string]$base, [string]$name) {
    if ($base -match '^https?://') { $base.TrimEnd('/') + '/' + $name } else { Join-Path $base $name }
}
function Get-FeedBytes([string]$src, [long]$max) {
    if ($src -match '^https?://') {
        $req = [Net.HttpWebRequest]::Create($src)
        $req.UserAgent = 'Gamebreaker-update'; $req.Timeout = 20000; $req.ReadWriteTimeout = 30000
        $resp = $req.GetResponse()
        try {
            $in = $resp.GetResponseStream(); $ms = New-Object IO.MemoryStream
            $buf = New-Object byte[] 65536
            while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) { $ms.Write($buf, 0, $n); if ($ms.Length -gt $max) { Fail 'feed' "The feed sent more than $max bytes for $src." } }
            return , $ms.ToArray()
        } finally { $resp.Dispose() }
    }
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { Fail 'feed' "The feed has no $(Split-Path $src -Leaf)." }
    if ((Get-Item -LiteralPath $src).Length -gt $max) { Fail 'feed' "The feed's $(Split-Path $src -Leaf) is larger than $max bytes." }
    , [IO.File]::ReadAllBytes($src)
}
function Get-FeedFile([string]$src, [string]$to, [long]$size, [scriptblock]$onBytes) {
    if ($src -match '^https?://') {
        $req = [Net.HttpWebRequest]::Create($src)
        $req.UserAgent = 'Gamebreaker-update'; $req.Timeout = 20000; $req.ReadWriteTimeout = 30000
        $resp = $req.GetResponse()
        try {
            $in = $resp.GetResponseStream(); $out = [IO.File]::Create($to)
            try {
                $buf = New-Object byte[] 262144; $got = [long]0
                while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
                    $out.Write($buf, 0, $n); $got += $n
                    if ($got -gt $size) { Fail 'hash' 'A downloaded file is larger than the release lists, so nothing was changed.' }
                    & $onBytes $n
                }
            } finally { $out.Dispose() }
        } finally { $resp.Dispose() }
    } else {
        if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { Fail 'feed' "The feed is missing a file of the release ($(Split-Path $src -Leaf))." }
        Copy-Item -LiteralPath $src $to -Force
        & $onBytes (Get-Item -LiteralPath $to).Length
    }
}

# Which of the manifest's files differ from what is on disk now.
function Get-Changes($m) {
    $changed = @()
    foreach ($f in @($m.files)) {
        $lp = Local-Path $f.path
        if ((Test-Path -LiteralPath $lp -PathType Leaf) -and (Get-Item -LiteralPath $lp).Length -eq [long]$f.size -and (Sha $lp) -eq $f.sha256) { continue }
        $changed += $f
    }
    # Files of this release that the new one no longer has (from this release's own manifest).
    $removed = @()
    $own = Read-Json (Join-Path $rootDir 'setup\manifest.json')
    if ($own) {
        $keep = @{}; foreach ($f in @($m.files)) { $keep[$f.path.ToLowerInvariant()] = $true }
        foreach ($f in @($own.files)) {
            if ($keep.ContainsKey(([string]$f.path).ToLowerInvariant()) -or -not (Test-SafePath ([string]$f.path))) { continue }
            if (Test-Path -LiteralPath (Local-Path $f.path) -PathType Leaf) { $removed += [string]$f.path }
        }
    }
    @{ changed = $changed; removed = $removed }
}
# What has to be built again: 'full' (the lift runs again), 'quick' (a few files and a link) or 'none'.
function Get-RebuildKind([string[]]$paths) {
    $kind = 'none'
    foreach ($p in $paths) {
        if ($p -match '^(port/config/|port/patches/|port/src/recomp_manual\.c$|xboxrecomp/|setup/tools\.lock$|setup/apply_patch\.py$|port/tools/pipeline\.ps1$)') { return 'full' }
        if ($p -match '^port/') { $kind = 'quick' }
    }
    $kind
}
function Test-GameRunning {
    if (-not $Folder) { return $false }
    # The exact program: the launcher's starter is also named Gamebreaker.exe.
    $exe = Join-Path ([IO.Path]::GetFullPath($Folder)) 'build\gamebreaker.exe'
    [bool](Get-Process gamebreaker -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -ieq $exe } catch { $false } })
}
# Move a file out of the way; a running program can be renamed but not deleted.
function Remove-Aside([string]$p) {
    if (-not (Test-Path -LiteralPath $p)) { return }
    try { Remove-Item -LiteralPath $p -Force }
    catch {
        $trash = Join-Path $upDir 'trash'; New-Item -ItemType Directory -Force $trash | Out-Null
        Move-Item -LiteralPath $p (Join-Path $trash ([guid]::NewGuid().ToString('N') + '_' + (Split-Path $p -Leaf))) -Force
    }
}

# The list's signature, checked against the key this release carries.
function Assert-Signed([byte[]]$list, [string]$signature) {
    $kf = Join-Path $rootDir 'setup\release_key.txt'
    if (-not (Test-Path -LiteralPath $kf)) { Fail 'signature' 'This release has no key to check updates with, so the update was refused. Nothing was downloaded.' }
    if (-not (Test-GbSignature $list $signature (Get-Content -LiteralPath $kf -Raw))) {
        Fail 'signature' 'The update list is not signed with the project''s key, so it was refused. Nothing was downloaded.'
    }
}

try {
    . (Join-Path $PSScriptRoot 'release_sign.ps1')
    . (Join-Path $PSScriptRoot 'release_manifest.ps1')
    New-Item -ItemType Directory -Force $upDir | Out-Null
    $rel = Read-Release
    $current = $(if ($rel.version) { $rel.version } else { '' })

    switch ($Action) {
    'check' {
        if (Test-Path (Join-Path $rootDir '.git')) { Emit @{ ev = 'update'; state = 'off'; why = 'checkout' }; exit 0 }
        $feedSrc = $(if ($Feed) { $Feed } else { [string]$rel.feed })
        if (-not $feedSrc -or -not (Split-Version $current)) { Emit @{ ev = 'update'; state = 'off'; why = 'no feed' }; exit 0 }
        Get-ChildItem (Join-Path $upDir 'trash') -File -ErrorAction SilentlyContinue | ForEach-Object { try { Remove-Item -LiteralPath $_.FullName -Force } catch { } }
        Emit @{ ev = 'update'; state = 'checking' }
        $murl = $(if ($feedSrc -match '\.json$') { $feedSrc } else { Join-Feed $feedSrc 'manifest.json' })
        try { $bytes = Get-FeedBytes $murl 4MB }
        catch { if ($_.Exception.Data['code']) { throw }; Fail 'offline' "The update feed could not be reached ($($_.Exception.Message.Trim()))." }
        # The signature comes first: nothing in an unsigned list is read or acted on.
        try { $sigBytes = Get-FeedBytes "$murl.sig" 4KB }
        catch {
            $base = $_.Exception.GetBaseException()
            $gone = ($_.Exception.Data['code'] -eq 'feed') -or ($base -is [Net.WebException] -and $base.Response -and [int]$base.Response.StatusCode -eq 404)
            if ($gone) { Fail 'signature' 'The update list is not signed, so it was refused. Nothing was downloaded.' }
            Fail 'offline' "The update feed could not be reached ($($_.Exception.Message.Trim()))."
        }
        $sigText = [Text.Encoding]::ASCII.GetString($sigBytes)
        Assert-Signed $bytes $sigText
        try { $m = [Text.Encoding]::UTF8.GetString($bytes).TrimStart([char]0xFEFF) | ConvertFrom-Json } catch { Fail 'manifest' 'The update feed sent a list that could not be read.' }
        Test-Manifest $m 'update feed''s list'
        Set-Content (Join-Path $upDir 'last-check.txt') ((Get-Date).ToUniversalTime().ToString('o')) -Encoding ascii
        if ((Compare-Version $m.version $current) -le 0) {
            Remove-Item $pendingFile, $pendingList, $pendingSig -Force -ErrorAction SilentlyContinue
            Emit @{ ev = 'update'; state = 'current'; version = $current; latest = [string]$m.version }
            exit 0
        }
        $c = Get-Changes $m
        $bytesNeeded = [long]0; $seen = @{}
        foreach ($f in $c.changed) { if (-not $seen.ContainsKey($f.sha256)) { $seen[$f.sha256] = $true; $bytesNeeded += [long]$f.size } }
        $paths = @($c.changed | ForEach-Object { [string]$_.path }) + $c.removed
        $kind = Get-RebuildKind $paths
        # The files' base: the manifest's own entry, else beside the manifest.
        $base = $(if ($m.files_base) { [string]$m.files_base } elseif ($murl -match '^https?://') { $murl.Substring(0, $murl.LastIndexOf('/')) } else { Split-Path $murl })
        Save-Json ([ordered]@{ manifest = $m; base = $base; from = $current; checked = (Get-Date).ToUniversalTime().ToString('o') }) $pendingFile
        [IO.File]::WriteAllBytes($pendingList, $bytes)   # kept byte for byte for setup\manifest.json
        [IO.File]::WriteAllText($pendingSig, $sigText)
        Emit @{ ev = 'update'; state = 'available'; version = $current; latest = [string]$m.version; notes = @($m.notes); bytes = $bytesNeeded
                files = $c.changed.Count; removed = $c.removed.Count; rebuild = $kind
                launcher = [bool]($paths | Where-Object { $_ -match '^(launcher/|Gamebreaker\.exe$)' }) }
    }
    'install' {
        if (Test-Path (Join-Path $rootDir '.git')) { Fail 'checkout' 'This is a development checkout; it is updated with git, not by the launcher.' }
        $j = Read-Json $journalFile
        if ($j -and $j.state -in 'applying', 'applied') { Fail 'busy' 'An update is already half done; run rollback first.' }
        $pend = Read-Json $pendingFile
        if (-not $pend) { Fail 'manifest' 'There is no checked update to install. Check for updates first.' }
        # What was signed is what is installed: the list is read again from its signed bytes.
        if (-not (Test-Path -LiteralPath $pendingList) -or -not (Test-Path -LiteralPath $pendingSig)) { Fail 'signature' 'There is no signed update list to install. Check for updates first.' }
        $listBytes = [IO.File]::ReadAllBytes($pendingList)
        Assert-Signed $listBytes ([IO.File]::ReadAllText($pendingSig))
        $m = [Text.Encoding]::UTF8.GetString($listBytes).TrimStart([char]0xFEFF) | ConvertFrom-Json
        Test-Manifest $m 'saved update list'
        if ((Compare-Version $m.version $current) -le 0) { Fail 'manifest' "Version $current is already installed." }
        if (Test-GameRunning) { Fail 'running' 'The game is running. Close it, then update.' }
        $c = Get-Changes $m

        # 1. Download every changed file into staging, checking size and SHA-256.
        $stage = Join-Path $upDir ("staging\" + $m.version)
        New-Item -ItemType Directory -Force $stage | Out-Null
        $total = [long]0; foreach ($f in $c.changed) { $total += [long]$f.size }
        $script:got = [long]0; $script:next = 0
        $clock = [Diagnostics.Stopwatch]::StartNew()
        Emit @{ ev = 'update'; state = 'downloading'; got = 0; size = $total }
        foreach ($f in $c.changed) {
            $sp = Join-Path $stage ($f.path.Replace('/', '\'))
            if ((Test-Path -LiteralPath $sp) -and (Sha $sp) -eq $f.sha256) { $script:got += [long]$f.size; continue }
            New-Item -ItemType Directory -Force (Split-Path $sp) | Out-Null
            $part = "$sp.part"
            Get-FeedFile (Join-Feed ([string]$pend.base) $f.sha256) $part ([long]$f.size) {
                param($n) $script:got += $n
                if ($clock.ElapsedMilliseconds -ge $script:next) { Emit @{ ev = 'update'; state = 'downloading'; got = $script:got; size = $total }; $script:next = $clock.ElapsedMilliseconds + 250 }
            }
            if ((Get-Item -LiteralPath $part).Length -ne [long]$f.size -or (Sha $part) -ne $f.sha256) {
                Remove-Item -LiteralPath $part -Force
                Fail 'hash' "A downloaded file ($($f.path)) did not match the release's list, so nothing was changed."
            }
            Move-Item -LiteralPath $part $sp -Force
        }
        Emit @{ ev = 'update'; state = 'downloading'; got = $total; size = $total }

        # 2. Check the whole staged set again just before using it.
        Emit @{ ev = 'update'; state = 'verifying' }
        foreach ($f in $c.changed) {
            $sp = Join-Path $stage ($f.path.Replace('/', '\'))
            if ((Sha $sp) -ne $f.sha256) { Fail 'hash' "A downloaded file ($($f.path)) changed after it was checked, so nothing was changed." }
        }
        $relNew = $c.changed | Where-Object { $_.path -eq 'setup/release.txt' }
        if ($relNew) {
            $vl = Select-String -LiteralPath (Join-Path $stage 'setup\release.txt') -Pattern '^version=(.+)$' | Select-Object -First 1
            if (-not $vl -or $vl.Matches[0].Groups[1].Value.Trim() -ne $m.version) { Fail 'manifest' 'The new release names a different version inside it than in its list, so nothing was changed.' }
        }
        if (Test-GameRunning) { Fail 'running' 'The game is running. Close it, then update.' }

        # 3. Write the journal, keep the game program, then swap the files.
        $bak = Join-Path $upDir ("backup\" + $current)
        if (Test-Path $bak) { Remove-Item $bak -Recurse -Force }
        New-Item -ItemType Directory -Force (Join-Path $bak 'files') | Out-Null
        $changedList = @($c.changed | ForEach-Object { [ordered]@{ path = [string]$_.path; existed = (Test-Path -LiteralPath (Local-Path $_.path) -PathType Leaf) } })
        $launcherTouched = [bool](@($changedList | ForEach-Object { $_.path }) + $c.removed | Where-Object { $_ -match '^(launcher/|Gamebreaker\.exe$)' })
        $j = [ordered]@{ state = 'applying'; from = $current; to = [string]$m.version; folder = $Folder; backup = $bak
                         changed = $changedList; removed = @($c.removed); launcher = $launcherTouched; rebuild = (Get-RebuildKind (@($changedList | ForEach-Object { $_.path }) + $c.removed))
                         started = (Get-Date).ToUniversalTime().ToString('o'); shown = $false; reason = '' }
        Save-Json $j $journalFile
        if ($Folder) {
            $gexe = Join-Path $Folder 'build\gamebreaker.exe'
            if (Test-Path -LiteralPath $gexe) { New-Item -ItemType Directory -Force (Join-Path $bak 'game') | Out-Null; Copy-Item -LiteralPath $gexe (Join-Path $bak 'game\gamebreaker.exe') -Force }
        }
        Emit @{ ev = 'update'; state = 'replacing'; files = $changedList.Count; removed = $c.removed.Count }
        foreach ($e in $changedList) {
            $lp = Local-Path $e.path
            if ($e.existed) {
                $bp = Join-Path $bak ('files\' + $e.path.Replace('/', '\'))
                New-Item -ItemType Directory -Force (Split-Path $bp) | Out-Null
                Move-Item -LiteralPath $lp $bp -Force
            }
            New-Item -ItemType Directory -Force (Split-Path $lp) | Out-Null
            Copy-Item -LiteralPath (Join-Path $stage ($e.path.Replace('/', '\'))) $lp -Force
        }
        foreach ($p in $c.removed) {
            $bp = Join-Path $bak ('files\' + $p.Replace('/', '\'))
            New-Item -ItemType Directory -Force (Split-Path $bp) | Out-Null
            Move-Item -LiteralPath (Local-Path $p) $bp -Force
        }
        # The new release's own list, for the update after this one (the old one
        # is kept with the backup, for a rollback).
        $ownList = Join-Path $rootDir 'setup\manifest.json'
        if (Test-Path $ownList) { Copy-Item $ownList (Join-Path $bak 'manifest.json') -Force }
        Copy-Item -LiteralPath $pendingList $ownList -Force
        $j.state = 'applied'
        Save-Json $j $journalFile
        Remove-Item $pendingFile, $pendingList, $pendingSig -Force -ErrorAction SilentlyContinue
        Emit @{ ev = 'update'; state = 'applied'; version = [string]$m.version; from = $current; launcher = $launcherTouched; rebuild = $j.rebuild }
    }
    'rollback' {
        $j = Read-Json $journalFile
        if (-not $j -or $j.state -notin 'applying', 'applied', 'building') { Emit @{ ev = 'update'; state = 'nothing' }; exit 0 }
        $bak = [string]$j.backup
        foreach ($e in @($j.changed)) {
            $lp = Local-Path $e.path
            $bp = Join-Path $bak ('files\' + ([string]$e.path).Replace('/', '\'))
            if (Test-Path -LiteralPath $bp) { Remove-Aside $lp; New-Item -ItemType Directory -Force (Split-Path $lp) | Out-Null; Move-Item -LiteralPath $bp $lp -Force }
            elseif (-not $e.existed) { Remove-Aside $lp }
        }
        foreach ($p in @($j.removed)) {
            $lp = Local-Path $p
            $bp = Join-Path $bak ('files\' + ([string]$p).Replace('/', '\'))
            if ((Test-Path -LiteralPath $bp) -and -not (Test-Path -LiteralPath $lp)) { New-Item -ItemType Directory -Force (Split-Path $lp) | Out-Null; Move-Item -LiteralPath $bp $lp -Force }
        }
        # The old release's own list comes back with it (it was in neither list).
        $own = Join-Path $bak 'manifest.json'
        if (Test-Path $own) { Copy-Item $own (Join-Path $rootDir 'setup\manifest.json') -Force }
        else { Remove-Item (Join-Path $rootDir 'setup\manifest.json') -Force -ErrorAction SilentlyContinue }
        $gfolder = $(if ($Folder) { $Folder } else { [string]$j.folder })
        $gb = Join-Path $bak 'game\gamebreaker.exe'
        if ($gfolder -and (Test-Path -LiteralPath $gb)) {
            if (Test-GameRunning) { Fail 'running' 'The game is running. Close it, then try again.' }
            Copy-Item -LiteralPath $gb (Join-Path $gfolder 'build\gamebreaker.exe') -Force
        }
        $j.state = 'rolledback'; $j.reason = $(if ($Reason) { $Reason } else { [string]$j.reason }); $j.shown = $false
        Save-Json $j $journalFile
        Emit @{ ev = 'update'; state = 'rolledback'; version = [string]$j.from; to = [string]$j.to; reason = $j.reason; launcher = [bool]$j.launcher }
    }
    'finish' {
        $j = Read-Json $journalFile
        if (-not $j -or $j.state -notin 'applied', 'building') { Emit @{ ev = 'update'; state = 'nothing' }; exit 0 }
        $j.state = 'done'; $j.shown = $false
        Save-Json $j $journalFile
        Remove-Item (Join-Path $upDir 'staging') -Recurse -Force -ErrorAction SilentlyContinue
        # Keep only the backup of the version just replaced.
        Get-ChildItem (Join-Path $upDir 'backup') -Directory -ErrorAction SilentlyContinue | Where-Object { $_.FullName -ne [string]$j.backup } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
        Emit @{ ev = 'update'; state = 'done'; version = [string]$j.to; from = [string]$j.from }
    }
    }
    exit 0
} catch {
    $code = $(if ($_.Exception.Data['code']) { [string]$_.Exception.Data['code'] } else { 'update' })
    Emit @{ ev = 'error'; step = 'update'; code = $code; text = $_.Exception.Message }
    [Console]::Error.WriteLine("update: $($_.Exception.Message)")
    exit 1
}
