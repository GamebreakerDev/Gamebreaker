# Download and unpack the tools named in tools.lock, each into its own folder
# under -Root. Nothing is installed and PATH is not changed; deleting -Root
# removes everything.
#
#   powershell -ExecutionPolicy Bypass -File setup\fetch.ps1 -Root <folder> [-Only python,capstone]
#
# Used by setup\setup.ps1 (the player's setup) and port\tools\fetch_toolchain.ps1
# (the developer's toolchain\ folder).
#
# Rules:
#   - every file is pinned by address, size and SHA-256 in tools.lock;
#   - it downloads to <name>.part and is renamed only once size and hash are
#     right; a wrong file is deleted, never unpacked;
#   - it unpacks into <dir>.partial and renames that to <dir> only when the
#     unpack finished, so an interrupted run leaves nothing half-done behind
#     and the next run starts that tool again;
#   - a finished folder holds .gb_tool with the SHA-256 it came from. A tool
#     whose folder holds the hash from the lock is not looked at again.
#
# Unpacking uses Windows' own tar.exe (part of Windows 10 1803 and later),
# several times faster than Expand-Archive. Only .zip and .whl (also a zip)
# are used, since Windows 10's tar may not read .xz.

param(
    [Parameter(Mandatory)] [string]$Root,
    [string]$Lock = (Join-Path $PSScriptRoot 'tools.lock'),
    [string[]]$Only = @(),
    [switch]$DeleteArchives,         # remove each archive once it is unpacked (setup does; the developer folder keeps them)
    [switch]$Quiet,                  # say nothing about tools that are already there
    [string]$Events = '',            # append progress as JSON lines here (setup.ps1 -Events; the launcher reads it)
    [string[]]$Skip = @()            # leave these out (setup: the toolkit when the release already holds a good copy)
)
# An entry with "verify": "tree" (the toolkit from its upstream repository) is
# checked after unpacking by the fingerprint of its files (tree_hash.ps1), not
# by the archive's size and hash, which GitHub does not promise to keep.
. (Join-Path $PSScriptRoot 'tree_hash.ps1')
. (Join-Path $PSScriptRoot 'retry.ps1')
function Get-Pin($t) { if ($t.verify -eq 'tree') { $t.tree_sha256 } else { $t.sha256 } }

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'    # Invoke-WebRequest is many times slower with its progress bar
# Windows PowerShell 5.1 on some Windows 10 installs offers only TLS 1.0/1.1 by
# default, which python.org and GitHub refuse.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$entries = (Get-Content $Lock -Raw | ConvertFrom-Json).tools
if ($Only.Count) {
    $Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
    foreach ($n in $Only) { if (-not ($entries | Where-Object name -eq $n)) { throw "tools.lock has no tool named '$n'" } }
    $entries = @($entries | Where-Object { $Only -contains $_.name })
}
if ($Skip.Count) { $entries = @($entries | Where-Object { $Skip -notcontains $_.name }) }

$dl = Join-Path $Root 'downloads'
New-Item -ItemType Directory -Force $dl | Out-Null
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
if (-not (Test-Path $tar)) { throw "Windows' tar.exe is missing ($tar). It is part of Windows 10 version 1803 and later." }

$utf8 = New-Object Text.UTF8Encoding($false)
function Emit([hashtable]$o) {
    if (-not $Events) { return }
    $line = ($o | ConvertTo-Json -Compress) + "`n"
    for ($i = 0; $i -lt 10; $i++) { try { [IO.File]::AppendAllText($Events, $line, $utf8); return } catch { Start-Sleep -Milliseconds 25 } }
}
function Fail([string]$code, [string]$msg) { $e = New-Object Exception $msg; $e.Data['code'] = $code; throw $e }

# Download to a file, reporting bytes received about four times a second.
function Get-GbFile([string]$url, [string]$to, [string]$name, [long]$size) {
    $req = [Net.HttpWebRequest]::Create($url)
    $req.UserAgent = 'Gamebreaker-setup'
    $req.Timeout = 30000; $req.ReadWriteTimeout = 60000
    $resp = $req.GetResponse()
    try {
        $in = $resp.GetResponseStream()
        $out = [IO.File]::Create($to)
        try {
            $buf = New-Object byte[] (1MB)
            $got = [long]0; $next = 0
            $clock = [Diagnostics.Stopwatch]::StartNew()
            while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
                $out.Write($buf, 0, $n); $got += $n
                if ($clock.ElapsedMilliseconds -ge $next) { Emit @{ ev = 'tool'; name = $name; state = 'progress'; got = $got; size = $size }; $next = $clock.ElapsedMilliseconds + 250 }
            }
            Emit @{ ev = 'tool'; name = $name; state = 'progress'; got = $got; size = $size }
        } finally { $out.Dispose(); $in.Dispose() }
    } finally { $resp.Dispose() }
}

function Size([long]$b) { if ($b -ge 1MB) { '{0:N1} MB' -f ($b / 1MB) } else { '{0:N0} KB' -f ($b / 1KB) } }

foreach ($t in $entries) {
    $label = "$($t.name) $($t.version)"
    $dest  = Join-Path $Root $t.dir
    $mark  = Join-Path $dest '.gb_tool'
    $check = Join-Path $dest $t.check
    $tree = ($t.verify -eq 'tree')
    if ((Test-Path $check) -and (Test-Path $mark) -and ((Get-Content $mark -Raw).Trim() -eq (Get-Pin $t))) {
        Emit @{ ev = 'tool'; name = $t.name; state = 'present'; size = $t.size }
        if (-not $Quiet) { Write-Host "  $label  present" }; continue
    }
    if (-not $tree -and (Test-Path $check) -and -not (Test-Path $mark)) {
        # Unpacked by the first version of fetch_toolchain.ps1, which wrote no marker.
        Emit @{ ev = 'tool'; name = $t.name; state = 'present'; size = $t.size }
        if (-not $Quiet) { Write-Host "  $label  present (unpacked before markers existed)" }; continue
    }

    $file = Join-Path $dl (Split-Path $t.url -Leaf)
    if (Test-Path $file) {
        $have = (Get-FileHash $file -Algorithm SHA256).Hash.ToLower()
        if ($tree -or $have -ne $t.sha256) { Remove-Item $file -Force }     # a tree-verified archive is always fetched afresh
    }
    if (-not (Test-Path $file)) {
        Write-Host "  $label  downloading $(Size $t.size) from $(([uri]$t.url).Host)"
        $part = "$file.part"
        Remove-Item $part -Force -ErrorAction SilentlyContinue
        Emit @{ ev = 'tool'; name = $t.name; state = 'download'; got = 0; size = $t.size }
        try {
            Get-GbFile $t.url $part $t.name $t.size
        } catch {
            Remove-Item $part -Force -ErrorAction SilentlyContinue
            $x = $_.Exception
            while ($x.InnerException -and -not $x.Response) { $x = $x.InnerException }
            $why = $x.Message
            if ($x.Response) { $why = "HTTP $([int]$x.Response.StatusCode) $($x.Response.StatusDescription)" }
            Fail 'download' "$label could not be downloaded ($why).`nAddress: $($t.url)`nCheck the internet connection, then run setup again."
        }
        $len = (Get-Item $part).Length
        $sha = (Get-FileHash $part -Algorithm SHA256).Hash.ToLower()
        if (-not $tree -and ($len -ne $t.size -or $sha -ne $t.sha256)) {
            Remove-Item $part -Force
            Fail 'hash' ("$label`: the downloaded file is not the pinned one, so it was deleted and nothing was unpacked.`n" +
                   "Expected $($t.size) bytes, SHA-256 $($t.sha256)`nReceived $len bytes, SHA-256 $sha")
        }
        Invoke-GbRetry { Move-Item $part $file -Force } "$label's download"
    }
    Write-Host $(if ($tree) { "  $label  downloaded, unpacking (its files are checked next)" } else { "  $label  hash OK, unpacking" })
    Emit @{ ev = 'tool'; name = $t.name; state = 'unpack'; size = $t.size }

    $partial = "$dest.partial"
    if (Test-Path $partial) { Invoke-GbRetry { Remove-Item $partial -Recurse -Force } "$label's folder" }
    if (Test-Path $dest)    { Invoke-GbRetry { Remove-Item $dest -Recurse -Force } "$label's folder" }      # an older or broken unpack
    New-Item -ItemType Directory -Force $partial | Out-Null
    & $tar -xf $file -C $partial
    if ($LASTEXITCODE -ne 0) { Fail 'unpack' "$label`: unpacking failed (tar exit code $LASTEXITCODE). If antivirus software removed files, allow the folder $Root and run setup again." }
    if (-not (Test-Path (Join-Path $partial $t.check))) {
        Fail 'antivirus' "$label`: unpacked, but $($t.check) is missing. If antivirus software removed it, allow the folder $Root and run setup again."
    }
    if ($tree) {
        $fp = Get-GbTreeHash (Join-Path $partial $t.tree_root)
        if ($fp.Hash -ne $t.tree_sha256) {
            Remove-Item $partial -Recurse -Force
            if ($DeleteArchives) { Remove-Item $file -Force -ErrorAction SilentlyContinue }
            Fail 'hash' ("$label`: the downloaded files are not the pinned version, so they were deleted.`n" +
                   "Expected $($t.tree_files) files, fingerprint $($t.tree_sha256)`nReceived $($fp.Files) files, fingerprint $($fp.Hash)")
        }
    }
    Set-Content -Path (Join-Path $partial '.gb_tool') -Value (Get-Pin $t) -Encoding ascii
    Invoke-GbRetry { Rename-Item $partial (Split-Path $dest -Leaf) } "$label's folder"
    if ($DeleteArchives) { Invoke-GbRetry { Remove-Item $file -Force } "$label's download" }
    Emit @{ ev = 'tool'; name = $t.name; state = 'done'; size = $t.size }
}
