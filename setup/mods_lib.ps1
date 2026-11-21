# Mods on the player's PC: list, inspect a zip, install, remove, write the order.
# Functions only (dot-sourced by the launcher host and by setup\mods.ps1).
#
# The game's side (port\src\gb_mods.c) reads <install>\mods\<mod>\<disc path>
# and mods\load_order.txt: one folder name a line, first line first; a line
# starting with # is off; with the file present, an unlisted folder is off;
# without it every folder is on in name order. Folders and files whose name
# starts with "." are not read, so ".installing-*" and ".removing-*" folders
# below are invisible to the game.
#
# A mod zip needs nothing of ours: a Thunderstore-style package (manifest.json,
# icon.png, README.md at its top) or a plain zip of files laid out as on the
# disc, at the zip's root or inside one top folder. Nothing in a zip is ever run.

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
# The Recycle Bin through the shell, with every dialog off (no confirmation, no
# error box, no progress window): tests and players never get a window from it.
if (-not ('GbMods.Crc' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace GbMods {
    public static class Crc {
        static readonly uint[] T = Make();
        static uint[] Make() { uint[] t = new uint[256]; for (uint i = 0; i < 256; i++) { uint c = i; for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } return t; }
        public static uint Update(uint crc, byte[] b, int n) { uint c = ~crc; for (int i = 0; i < n; i++) c = T[(c ^ b[i]) & 0xFF] ^ (c >> 8); return ~c; }
    }
    public static class Bin {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct Op { public IntPtr hwnd; public uint func; public string from; public string to; public ushort flags; public bool aborted; public IntPtr names; public string title; }
        [DllImport("shell32.dll", CharSet = CharSet.Unicode)] static extern int SHFileOperationW(ref Op op);
        public static bool Recycle(string path) {
            // FO_DELETE; FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR
            Op op = new Op { func = 3, from = path + "\0\0", flags = 0x0040 | 0x0010 | 0x0004 | 0x0400 | 0x0200 };
            return SHFileOperationW(ref op) == 0 && !op.aborted;
        }
    }
}
'@
}

$script:GbModMeta = @('manifest.json', 'icon.png', 'readme.md', 'readme.txt', 'changelog.md', 'mod.json', 'license', 'license.txt', 'license.md')
# Programs and scripts: a data mod has no use for them, and the game never runs a file from a mod.
$script:GbModBlocked = @('exe', 'dll', 'com', 'scr', 'bat', 'cmd', 'ps1', 'psm1', 'psd1', 'ps1xml', 'vbs', 'vbe', 'js', 'jse', 'wsf', 'wsh', 'msi', 'msp', 'msc',
                         'lnk', 'hta', 'cpl', 'sys', 'drv', 'ocx', 'jar', 'py', 'pyw', 'reg', 'url', 'scf', 'inf', 'xbe', 'sh', 'appx', 'msix', 'application', 'gadget', 'pif')
# The disc's top folders, used only when the install has no game_files to compare with.
$script:GbDiscTops = @('anim', 'courtfx', 'credits', 'cscripts', 'database', 'fe', 'locale', 'movies', 'music', 'scripts', 'sfx', 'speech', 'xf')
$script:GbModLimits = @{ Total = 4GB; File = 2GB; Entries = 20000; Manifest = 64KB; Icon = 512KB; Readme = 48KB; PathLen = 200 }
$script:GbUtf8 = New-Object Text.UTF8Encoding($false)

# The disc's files as lower-case relative paths with "/" (a set), or $null when unknown.
function Get-GbDiscFiles([string]$GameFiles) {
    if (-not $GameFiles -or -not (Test-Path -LiteralPath $GameFiles)) { return $null }
    $root = (Get-Item -LiteralPath $GameFiles).FullName.TrimEnd('\') + '\'
    $set = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($f in [IO.Directory]::EnumerateFiles($root, '*', 'AllDirectories')) {
        $rel = $f.Substring($root.Length).Replace('\', '/').ToLowerInvariant()
        if ($rel -in 'default.xbe', 'default_analysis.json') { continue }    # the program, and setup's own notes
        [void]$set.Add($rel)
    }
    if ($set.Count -eq 0) { return $null }
    , $set
}
# The entry list of one of the disc's archives, as the game reads it
# (gb_mods.c): a BIG file ("BIGF", big-endian count at 8 and header size at
# 12, then per entry 8 bytes and a NUL-ended name) or a C0FB pack (C0 FB,
# big-endian count at 4, then per entry 6 bytes and a NUL-ended name).
# Returns @{ kind = 'big' | 'pack' | 'none'; count; names } with names lower-case
# to the first entry of that name. 'none' when the file is neither, or unreadable.
function Get-GbArchiveTable([string]$Path) {
    $none = @{ kind = 'none'; count = 0; names = @{} }
    try { $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite') } catch { return $none }
    try {
        $len = $fs.Length; if ($len -lt 16) { return $none }
        $h = New-Object byte[] 16; if ($fs.Read($h, 0, 16) -ne 16) { return $none }
        $be = { param($b, $i, $n) $v = [long]0; for ($k = 0; $k -lt $n; $k++) { $v = ($v -shl 8) -bor $b[$i + $k] }; $v }
        if ($h[0] -eq 0x42 -and $h[1] -eq 0x49 -and $h[2] -eq 0x47 -and $h[3] -eq 0x46) {
            $count = & $be $h 8 4; $hsize = & $be $h 12 4
            if ($hsize -lt 16 + $count * 9 -or $hsize -ge 64MB -or $hsize -gt $len) { return $none }
            $kind = 'big'; $at = 16; $skip = 8; $want = $hsize
        } elseif ($h[0] -eq 0xC0 -and $h[1] -eq 0xFB) {
            $count = ($h[4] -shl 8) -bor $h[5]
            $kind = 'pack'; $at = 6; $skip = 6; $want = [Math]::Min($len, 16MB)
        } else { return $none }
        $fs.Position = 0; $d = New-Object byte[] $want; $got = 0
        while ($got -lt $want) { $k = $fs.Read($d, $got, $want - $got); if ($k -le 0) { break }; $got += $k }
        $names = @{}
        for ($i = 0; $i -lt $count; $i++) {
            $s = $at + $skip; if ($s -ge $got) { return $none }
            $e = [Array]::IndexOf($d, [byte]0, $s); if ($e -lt 0) { return $none }
            $nm = [Text.Encoding]::ASCII.GetString($d, $s, $e - $s).ToLowerInvariant()
            if ($nm -and -not $names.ContainsKey($nm)) { $names[$nm] = $i }
            $at = $e + 1
        }
        @{ kind = $kind; count = $count; names = $names }
    } finally { $fs.Dispose() }
}

# An entry of an archive named by its own name (any case) or by its number
# counted from 0, as the name's digits before the first dot; -1 when neither.
function Get-GbEntryIndex($table, [string]$want) {
    if ($table.names.ContainsKey($want)) { return [int]$table.names[$want] }
    if ($want -notmatch '^(\d+)(\.|$)' -or $Matches[1].Length -gt 7) { return -1 }
    $v = [int]$Matches[1]; if ($v -lt $table.count) { $v } else { -1 }
}

# What the game makes of one file of a mod (a relative path with "/"), by the
# rules of port\src\gb_mods.c:
#   use   'game'     read in place of the disc's (key: what two mods clash on)
#         'missing'  an archive entry the archive does not have
#         'texture'  a picture under textures\, read by the renderer (gb_textures.c)
#         'unused'   the game has no place for it
#   key   whole file: the path; archive entry: "<archive>#<index>"; a line of a
#         text entry in a pack: "<archive>#<index>/<line>"; a picture: "tex:<fingerprint>"
#   note  "line 3" for a line of text, else ''
# $cache is a hashtable the caller keeps for the archives' entry lists.
function Resolve-GbModItem([string]$rel, $disc, [string]$GameFiles, [hashtable]$cache, [long]$Size = 1) {
    $l = $rel.ToLowerInvariant()
    $r = @{ use = 'unused'; key = $l; note = '' }
    if ($l.StartsWith('textures/')) {
        # A picture named by the fingerprint of the texture it replaces: 16 hex
        # digits, then the extension or a space, _ or - and a label; .png or
        # .dds; up to 8 folders below textures; not empty, at most 256 MB.
        # Two mods clash on the fingerprint, whatever their labels.
        $parts = $l.Split('/')
        if ($parts.Count -le 10 -and $Size -gt 0 -and $Size -le 256MB -and $parts[-1] -match '^([0-9a-f]{16})([ _\-][^/]*)?\.(png|dds)$') {
            $r.use = 'texture'; $r.key = "tex:$($Matches[1])"
        }
        return $r
    }
    if ($l -match '[^\x00-\x7f]') { return $r }                     # the disc's names are plain ASCII
    if (-not $disc) {                                                # no disc to compare with: its top folders
        $i = $l.IndexOf('/'); if ($i -gt 0 -and ($script:GbDiscTops -contains $l.Substring(0, $i))) { $r.use = 'game' }
        return $r
    }
    if ($disc.Contains($l)) { $r.use = 'game'; return $r }
    # An archive entry: the longest leading part of the path that is a disc file
    # names the archive; one name follows (an entry), or two for a line of text
    # in a pack (<entry>/<n>.txt).
    $parts = $l.Split('/'); $arch = $null
    for ($k = $parts.Count - 1; $k -ge 1; $k--) {
        $a = $parts[0..($k - 1)] -join '/'
        if ($disc.Contains($a)) { $arch = $a; $rest = @($parts[$k..($parts.Count - 1)]); break }
    }
    if (-not $arch -or $rest.Count -gt 2) { return $r }
    if (-not $cache.ContainsKey($arch)) {
        $cache[$arch] = $(if ($GameFiles) { Get-GbArchiveTable (Join-Path $GameFiles $arch.Replace('/', '\')) } else { @{ kind = 'none' } })
    }
    $t = $cache[$arch]
    if ($t.kind -eq 'none') {
        # An archive whose list cannot be read here (a stand-in disc): one name
        # counts, by that name; the game says in its log if it finds no entry.
        if ($rest.Count -eq 1) { $r.use = 'game'; $r.key = "$arch/$($rest[0])" }
        return $r
    }
    if ($t.kind -eq 'big' -and $rest.Count -ne 1) { return $r }     # lines of text are for packs only
    $idx = Get-GbEntryIndex $t $rest[0]
    if ($idx -lt 0) { $r.use = 'missing'; return $r }
    $r.key = "$arch#$idx"
    if ($rest.Count -eq 2) {
        if ($rest[1] -notmatch '^(\d+)(\.|$)' -or $Matches[1].Length -gt 6) { return $r }   # a line is named by its number
        $line = [int]$Matches[1]; $r.key += "/$line"; $r.note = "line $line"
    }
    $r.use = 'game'; $r
}
function Test-GbModMeta([string]$rel) { ($rel.IndexOf('/') -lt 0) -and ($script:GbModMeta -contains $rel.ToLowerInvariant()) }

# A safe relative path inside a mod, or the reason it is not.
function Test-GbModPath([string]$p) {
    if (-not $p) { return 'empty' }
    if ($p.Length -gt $script:GbModLimits.PathLen) { return 'long' }
    if ($p.StartsWith('/') -or $p -match '^[A-Za-z]:' -or $p.Contains(':')) { return 'absolute' }
    if ($p -match '[\x00-\x1f<>"|?*]') { return 'chars' }
    foreach ($seg in $p.Split('/')) {
        if ($seg -eq '' -or $seg -eq '.' -or $seg -eq '..') { return 'dots' }
        if ($seg.EndsWith('.') -or $seg.EndsWith(' ') -or $seg.StartsWith(' ')) { return 'trailing' }
        if ($seg -match '^(con|prn|aux|nul|com[0-9]|lpt[0-9])(\..*)?$') { return 'device' }
    }
    ''
}
# A folder name the game and Windows both take: letters, digits, space, _ . -
function ConvertTo-GbModFolder([string]$name) {
    $n = ($name -replace '[^A-Za-z0-9 _.\-]', '_').Trim(' ', '.', '_')
    if ($n.Length -gt 60) { $n = $n.Substring(0, 60).Trim(' ', '.') }
    if (-not $n -or $n -match '^(con|prn|aux|nul|com[0-9]|lpt[0-9])$') { $n = 'mod' }
    $n
}

function Read-GbJsonFile([string]$path, [int]$max) {
    $fi = Get-Item -LiteralPath $path -ErrorAction SilentlyContinue
    if (-not $fi -or $fi.Length -gt $max) { return $null }
    try { [IO.File]::ReadAllText($fi.FullName, $script:GbUtf8).TrimStart([char]0xFEFF) | ConvertFrom-Json } catch { $null }
}
function Read-GbText([string]$path, [int]$max) {
    $fi = Get-Item -LiteralPath $path -ErrorAction SilentlyContinue
    if (-not $fi) { return '' }
    $fs = [IO.File]::OpenRead($fi.FullName)
    try { $buf = New-Object byte[] ([Math]::Min($fi.Length, $max)); $n = $fs.Read($buf, 0, $buf.Length); $script:GbUtf8.GetString($buf, 0, $n).TrimStart([char]0xFEFF) } finally { $fs.Dispose() }
}
function Get-GbIconUri([byte[]]$bytes) {
    if (-not $bytes -or $bytes.Length -lt 8 -or $bytes.Length -gt $script:GbModLimits.Icon) { return '' }
    if ($bytes[0] -ne 0x89 -or $bytes[1] -ne 0x50 -or $bytes[2] -ne 0x4E -or $bytes[3] -ne 0x47) { return '' }   # PNG only
    'data:image/png;base64,' + [Convert]::ToBase64String($bytes)
}
# Name, version, description and author from a manifest (Thunderstore's fields,
# and "author"/"authors" when a mod adds them), the folder name filling gaps.
function Get-GbModInfo($man, [string]$folder) {
    $o = [ordered]@{ name = ''; version = ''; desc = ''; author = ''; website = '' }
    if ($man) {
        if ($man.name -is [string]) { $o.name = $man.name.Replace('_', ' ').Trim() }
        if ($man.display_name -is [string] -and $man.display_name.Trim()) { $o.name = $man.display_name.Trim() }
        foreach ($k in 'version_number', 'version') { if ($man.$k -is [string] -and -not $o.version) { $o.version = $man.$k.Trim() } }
        if ($man.description -is [string]) { $o.desc = $man.description.Trim() }
        if ($man.website_url -is [string]) { $o.website = $man.website_url.Trim() }
        if ($man.authors) { $o.author = (@($man.authors) | ForEach-Object { "$_".Trim() } | Where-Object { $_ }) -join ', ' }
        elseif ($man.author -is [string]) { $o.author = $man.author.Trim() }
        # A package downloaded from Thunderstore is unpacked into "Team-Name": the team is its author.
        if (-not $o.author -and $man.name -is [string] -and $folder -match ('^(.+)-' + [regex]::Escape($man.name) + '$')) { $o.author = $Matches[1] }
    }
    if (-not $o.name) { $o.name = $folder }
    foreach ($k in 'name', 'version', 'desc', 'author', 'website') { if ($o[$k].Length -gt 400) { $o[$k] = $o[$k].Substring(0, 400) } }
    $o
}

# ------------------------------------------------------------------ order
# The order file as entries @{ name; on } (first first), or $null when there is none.
function Read-GbLoadOrder([string]$ModsDir) {
    $f = Join-Path $ModsDir 'load_order.txt'
    if (-not (Test-Path -LiteralPath $f)) { return $null }
    $out = New-Object System.Collections.ArrayList
    foreach ($l in [IO.File]::ReadAllLines($f, $script:GbUtf8)) {
        $s = $l.Trim().TrimStart([char]0xFEFF).Trim()
        if (-not $s) { continue }
        $on = -not $s.StartsWith('#')
        if (-not $on) { $s = $s.TrimStart('#').Trim() }
        if (-not $s -or $s -match '[\\/:]') { continue }
        [void]$out.Add(@{ name = $s; on = $on })
    }
    , $out
}
# Writes the whole order: on mods as their folder name, off mods as "# name",
# each in its place. Written beside, then swapped in, so the game never reads half a file.
function Write-GbLoadOrder([string]$ModsDir, $entries) {
    New-Item -ItemType Directory -Force $ModsDir | Out-Null
    $lines = @($entries | ForEach-Object { if ($_.on) { $_.folder } else { '# ' + $_.folder } })
    $f = Join-Path $ModsDir 'load_order.txt'; $tmp = "$f.new"
    [IO.File]::WriteAllText($tmp, (($lines -join "`r`n") + $(if ($lines.Count) { "`r`n" } else { '' })), $script:GbUtf8)
    if (Test-Path -LiteralPath $f) { [IO.File]::Replace($tmp, $f, [NullString]::Value) } else { [IO.File]::Move($tmp, $f) }
}
# The order the page asks for ([{folder; on}], first first), kept to folders that
# exist, each once; folders it did not name keep their place at the end.
function Set-GbModOrder([string]$ModsDir, $want) {
    $have = Get-GbModOrder $ModsDir
    $out = New-Object System.Collections.ArrayList; $seen = @{}
    foreach ($w in @($want)) {
        $h = $have | Where-Object { $_.folder -ieq [string]$w.folder } | Select-Object -First 1
        if ($h -and -not $seen[$h.folder.ToLowerInvariant()]) { $seen[$h.folder.ToLowerInvariant()] = 1; [void]$out.Add(@{ folder = $h.folder; on = [bool]$w.on }) }
    }
    foreach ($h in $have) { if (-not $seen[$h.folder.ToLowerInvariant()]) { [void]$out.Add($h) } }
    Write-GbLoadOrder $ModsDir $out
}
# Folders in the game's order with on/off, as the game will read them.
function Get-GbModOrder([string]$ModsDir) {
    $dirs = @()
    if (Test-Path -LiteralPath $ModsDir) {
        $dirs = @(Get-ChildItem -LiteralPath $ModsDir -Directory -Force | Where-Object { -not $_.Name.StartsWith('.') } | Sort-Object Name | ForEach-Object Name)
    }
    $lo = Read-GbLoadOrder $ModsDir
    $out = New-Object System.Collections.ArrayList
    if ($null -eq $lo) { foreach ($d in $dirs) { [void]$out.Add(@{ folder = $d; on = $true }) }; return , $out }
    $seen = @{}
    foreach ($e in $lo) {
        $d = $dirs | Where-Object { $_ -ieq $e.name } | Select-Object -First 1
        if (-not $d -or $seen[$d.ToLowerInvariant()]) { continue }
        $seen[$d.ToLowerInvariant()] = 1
        # A folder listed both on and off: the game takes the on line.
        [void]$out.Add(@{ folder = $d; on = $e.on })
    }
    foreach ($e in $lo) { if ($e.on) { foreach ($o in $out) { if ($o.folder -ieq $e.name) { $o.on = $true } } } }
    foreach ($d in $dirs) { if (-not $seen[$d.ToLowerInvariant()]) { [void]$out.Add(@{ folder = $d; on = $false }) } }   # put in by hand: off until switched on
    , $out
}

# ------------------------------------------------------------------ listing
# Archive entries the game did not find at its last start ([MODS] lines in its
# log), by mod folder: lower-case paths with "/". Only lines newer than the mod
# folder count, so a mod installed or replaced since then starts clean.
function Get-GbModLogMisses([string]$LogFile) {
    $out = @{}
    if (-not $LogFile -or -not (Test-Path -LiteralPath $LogFile)) { return $out }
    $when = (Get-Item -LiteralPath $LogFile).LastWriteTimeUtc
    foreach ($l in [IO.File]::ReadAllLines($LogFile)) {
        # Not found: "<mod>\<archive>\<entry>: <archive> has no entry ...". Refused
        # (wrong kind of entry, text that is not UTF-8, a line past the end):
        # "<mod>\<archive>\<entry>[\<n>.txt] is not used: <why>".
        if ($l -match '\[MODS\] ([^\\]+)\\(.+?): .+ has no entry by that name or number') { $why = '' }
        elseif ($l -match '\[MODS\] ([^\\]+)\\(.+?) is not used: (.+)$') { $why = $Matches[3] }
        elseif ($l -match '\[MODS\] ([^\\]+)\\(textures\\.+?): (.+); not used$') { $why = $Matches[3] }   # a picture (gb_textures.c)
        else { continue }
        $k = $Matches[1].ToLowerInvariant()
        if (-not $out[$k]) { $out[$k] = @{ when = $when; files = New-Object System.Collections.ArrayList; why = @{} } }
        $f = $Matches[2].Replace('\', '/').ToLowerInvariant()
        if ($why) { $out[$k].why[$f] = $why } else { [void]$out[$k].files.Add($f) }
    }
    $out
}
function Get-GbModList([string]$ModsDir, [string]$GameFiles, [string]$LogFile = '') {
    $misses = Get-GbModLogMisses $LogFile
    $disc = Get-GbDiscFiles $GameFiles; $arch = @{}
    $list = New-Object System.Collections.ArrayList
    foreach ($e in (Get-GbModOrder $ModsDir)) {
        $dir = Join-Path $ModsDir $e.folder
        $root = (Get-Item -LiteralPath $dir).FullName.TrimEnd('\') + '\'
        $files = New-Object System.Collections.ArrayList; $keys = New-Object System.Collections.ArrayList; $notes = New-Object System.Collections.ArrayList
        $unused = New-Object System.Collections.ArrayList; $missing = New-Object System.Collections.ArrayList; $textures = New-Object System.Collections.ArrayList; $texKeys = New-Object System.Collections.ArrayList
        $size = [long]0; $n = 0
        foreach ($f in [IO.Directory]::EnumerateFiles($root, '*', 'AllDirectories')) {
            $rel = $f.Substring($root.Length).Replace('\', '/')
            if ($rel.Split('/') | Where-Object { $_.StartsWith('.') }) { continue }   # the game skips these
            if (Test-GbModMeta $rel) { continue }
            if (++$n -gt 5000) { break }
            $len = (New-Object IO.FileInfo $f).Length
            # The game uses a replacement only when it is not empty (gb_mods.c).
            $it = $(if ($len -gt 0) { Resolve-GbModItem $rel $disc $GameFiles $arch $len } else { @{ use = 'unused' } })
            switch ($it.use) {
                'texture' { [void]$textures.Add($rel); [void]$texKeys.Add($it.key); $size += $len }
                'unused' { [void]$unused.Add($rel) }
                default {
                    [void]$files.Add($rel); [void]$keys.Add($it.key); [void]$notes.Add($it.note); $size += $len
                    if ($it.use -eq 'missing') { [void]$missing.Add($rel) }
                }
            }
        }
        # What the game said at its last start, if newer than the mod: entries it
        # did not find (named by the entry, so a line of text by its entry's
        # folder) and items it refused, with why.
        $refused = New-Object System.Collections.ArrayList
        if (($m = $misses[$e.folder.ToLowerInvariant()]) -and $m.when -gt (Get-Item -LiteralPath $dir).LastWriteTimeUtc) {
            foreach ($f in @($files) + @($textures)) {
                $lf = $f.ToLowerInvariant(); $up = $(if ($lf.LastIndexOf('/') -gt 0) { $lf.Substring(0, $lf.LastIndexOf('/')) } else { '' })
                if (($m.files -contains $lf -or ($up -and $m.files -contains $up)) -and -not $missing.Contains($f)) { [void]$missing.Add($f) }
                elseif ($m.why.ContainsKey($lf)) { [void]$refused.Add([ordered]@{ file = $f; why = $m.why[$lf] }) }
            }
        }
        $manPath = Join-Path $dir 'manifest.json'; $hasMan = Test-Path -LiteralPath $manPath
        if (-not $hasMan -and (Test-Path -LiteralPath (Join-Path $dir 'mod.json'))) { $manPath = Join-Path $dir 'mod.json'; $hasMan = $true }
        $man = $null; $bad = ''
        if ($hasMan) { $man = Read-GbJsonFile $manPath $script:GbModLimits.Manifest; if (-not $man) { $bad = 'manifest.json could not be read' } }
        $info = Get-GbModInfo $man $e.folder
        $icon = ''; $ip = Join-Path $dir 'icon.png'
        if (Test-Path -LiteralPath $ip) { if ((Get-Item -LiteralPath $ip).Length -le $script:GbModLimits.Icon) { $icon = Get-GbIconUri ([IO.File]::ReadAllBytes($ip)) } }
        $readme = ''
        foreach ($r in 'README.md', 'readme.txt') { $rp = Join-Path $dir $r; if (Test-Path -LiteralPath $rp) { $readme = Read-GbText $rp $script:GbModLimits.Readme; break } }
        if (-not $bad -and $files.Count -eq 0 -and $textures.Count -eq 0) { $bad = 'No files for the game' }
        [void]$list.Add([ordered]@{
            folder = $e.folder; on = [bool]$e.on; name = $info.name; version = $info.version; author = $info.author; desc = $info.desc; website = $info.website
            manifest = [bool]$hasMan; icon = $icon; readme = $readme; files = @($files); keys = @($keys); notes = @($notes); unused = @($unused)
            textures = @($textures); textureKeys = @($texKeys); size = $size; bad = $bad; missing = @($missing); refused = @($refused) })
    }
    , $list
}

# ------------------------------------------------------------------ zips
# Looks at a zip without unpacking it. Returns @{ ok; reason; ... }: on success
# the mod's name and files, the folder it would go in, and the zip's prefix.
function Get-GbZipInfo([string]$Zip, [string]$ModsDir, [string]$GameFiles) {
    $leaf = Split-Path $Zip -Leaf
    $no = { param($r) [ordered]@{ ok = $false; zip = $leaf; reason = $r } }
    if (Test-Path -LiteralPath $Zip -PathType Container) { return (& $no 'Not a .zip file.') }
    if (-not (Test-Path -LiteralPath $Zip -PathType Leaf)) { return (& $no 'File not found.') }
    if ($leaf -notmatch '\.zip$') { return (& $no 'Not a .zip file.') }
    try { $z = [IO.Compression.ZipFile]::OpenRead((Get-Item -LiteralPath $Zip).FullName) } catch { return (& $no 'Not a zip, or damaged.') }
    try {
        $ents = @($z.Entries)
        if ($ents.Count -gt $script:GbModLimits.Entries) { return (& $no 'Over 20,000 files.') }
        $files = @(); $total = [long]0
        foreach ($en in $ents) {
            $p = $en.FullName.Replace('\', '/')
            if ($p.EndsWith('/')) { if (Test-GbModPath $p.TrimEnd('/')) { return (& $no 'Unsafe file path.') }; continue }   # a folder entry
            if (Test-GbModPath $p) { return (& $no 'Unsafe file path.') }
            $ext = [IO.Path]::GetExtension($p).TrimStart('.').ToLowerInvariant()
            if ($script:GbModBlocked -contains $ext) { return (& $no 'Contains programs.') }
            if ($en.Length -gt $script:GbModLimits.File) { return (& $no 'A file is over 2 GB.') }
            $total += $en.Length
            $files += , @($p, $en)
        }
        if ($total -gt $script:GbModLimits.Total) { return (& $no 'Over 4 GB unpacked.') }
        if (-not $files.Count) { return (& $no 'Empty zip.') }
        # Files at the root, or inside one top folder that is not itself one of the disc's folders.
        $prefix = ''
        $tops = @($files | ForEach-Object { $s = $_[0]; $i = $s.IndexOf('/'); if ($i -lt 0) { '' } else { $s.Substring(0, $i) } } | Sort-Object -Unique)
        $disc = Get-GbDiscFiles $GameFiles
        $discTops = $(if ($disc) { @($disc | ForEach-Object { $i = $_.IndexOf('/'); if ($i -gt 0) { $_.Substring(0, $i) } } | Sort-Object -Unique) } else { $script:GbDiscTops })
        $discTops = @($discTops) + 'textures'                        # a mod's own folder of texture replacements
        if ($tops.Count -eq 1 -and $tops[0] -and ($discTops -notcontains $tops[0].ToLowerInvariant())) { $prefix = $tops[0] + '/' }
        $game = @(); $notes = @(); $missing = @(); $textures = @(); $arch = @{}; $unused = @(); $size = [long]0; $man = $null; $icon = ''; $readme = ''; $manBad = $false; $hasMan = $false
        foreach ($pe in $files) {
            $p = $pe[0]; if ($prefix -and -not $p.StartsWith($prefix)) { continue }
            $rel = $p.Substring($prefix.Length); $en = $pe[1]
            if (Test-GbModMeta $rel) {
                $l = $rel.ToLowerInvariant()
                if (($l -eq 'manifest.json' -or ($l -eq 'mod.json' -and -not $hasMan)) -and $en.Length -le $script:GbModLimits.Manifest) {
                    $hasMan = $true; $man = $null
                    try { $sr = New-Object IO.StreamReader($en.Open(), $script:GbUtf8); try { $man = $sr.ReadToEnd().TrimStart([char]0xFEFF) | ConvertFrom-Json } finally { $sr.Dispose() } } catch { $man = $null }
                    $manBad = -not $man
                }
                elseif ($l -eq 'icon.png' -and $en.Length -le $script:GbModLimits.Icon) {
                    $ms = New-Object IO.MemoryStream; $s = $en.Open(); try { $s.CopyTo($ms) } finally { $s.Dispose() }; $icon = Get-GbIconUri $ms.ToArray()
                }
                elseif (($l -eq 'readme.md' -or ($l -eq 'readme.txt' -and -not $readme))) {
                    $s = $en.Open(); try { $buf = New-Object byte[] ([Math]::Min($en.Length, $script:GbModLimits.Readme)); $n = 0; while ($n -lt $buf.Length) { $k = $s.Read($buf, $n, $buf.Length - $n); if ($k -le 0) { break }; $n += $k }; $readme = $script:GbUtf8.GetString($buf, 0, $n).TrimStart([char]0xFEFF) } finally { $s.Dispose() }
                }
                continue
            }
            if ($rel.Split('/') | Where-Object { $_.StartsWith('.') }) { continue }   # the game never reads these
            $it = $(if ($en.Length -gt 0) { Resolve-GbModItem $rel $disc $GameFiles $arch $en.Length } else { @{ use = 'unused' } })
            switch ($it.use) {
                'unused' { $unused += $rel }
                'texture' { $textures += $rel; $size += $en.Length }
                default { $game += $rel; $notes += $it.note; $size += $en.Length; if ($it.use -eq 'missing') { $missing += $rel } }
            }
        }
        if (-not $game.Count -and -not $textures.Count) { return (& $no 'No files the game reads.') }
        # The folder: "Team-Name" for a package downloaded from Thunderstore
        # (Team-Name-1.2.3.zip), else the manifest's name, else the zip's name.
        $base = [IO.Path]::GetFileNameWithoutExtension($leaf)
        $mname = $(if ($man -and $man.name -is [string]) { $man.name.Trim() } else { '' })
        if ($mname -and $base -match ('^([A-Za-z0-9_]+)-' + [regex]::Escape($mname) + '-\d+\.\d+\.\d+$')) { $folder = ConvertTo-GbModFolder "$($Matches[1])-$mname" }
        elseif ($mname) { $folder = ConvertTo-GbModFolder $mname }
        else { $folder = ConvertTo-GbModFolder ($base -replace '[-_ ](v\d+(\.\d+)*|\d+(\.\d+)+)$', '') }
        $info = Get-GbModInfo $man $folder
        $replaces = $null
        if ($ModsDir -and (Test-Path -LiteralPath $ModsDir)) {
            $have = Get-ChildItem -LiteralPath $ModsDir -Directory -Force | Where-Object { $_.Name -ieq $folder } | Select-Object -First 1
            if ($have) { $folder = $have.Name; $replaces = @{ folder = $have.Name } }
        }
        [ordered]@{ ok = $true; zip = $leaf; folder = $folder; prefix = $prefix; name = $info.name; version = $info.version; author = $info.author
            desc = $info.desc; website = $info.website; manifest = $hasMan; manifestBad = $manBad; icon = $icon; readme = $readme
            files = @($game); notes = @($notes); missing = @($missing); textures = @($textures); unused = @($unused); size = $size; total = $total; replaces = $replaces }
    } finally { $z.Dispose() }
}

# Unpacks a zip into mods\.installing-<folder>, checks every file, then puts it
# in place (a replaced mod goes aside first and comes back if anything fails).
# Nothing in mods\ changes unless every step succeeds. $Progress is called with 0..100.
function Install-GbModZip([string]$Zip, [string]$ModsDir, [string]$GameFiles, [scriptblock]$Progress) {
    $info = Get-GbZipInfo $Zip $ModsDir $GameFiles
    if (-not $info.ok) { return $info }
    New-Item -ItemType Directory -Force $ModsDir | Out-Null
    Clear-GbModTemp $ModsDir
    $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot((Get-Item -LiteralPath $ModsDir).FullName))
    if ($drive.AvailableFreeSpace -lt $info.total + 256MB) { return [ordered]@{ ok = $false; zip = $info.zip; reason = ('Not enough free space: needs {0:N0} MB.' -f [Math]::Ceiling(($info.total + 256MB) / 1MB)) } }
    $folder = $info.folder
    $tmp = Join-Path $ModsDir ('.installing-' + $folder)
    $old = Join-Path $ModsDir ('.removing-' + $folder)
    $dst = Join-Path $ModsDir $folder
    $tmpRoot = [IO.Path]::GetFullPath($tmp).TrimEnd('\') + '\'
    $movedOld = $false
    try {
        New-Item -ItemType Directory -Force $tmp | Out-Null
        $z = [IO.Compression.ZipFile]::OpenRead((Get-Item -LiteralPath $Zip).FullName)
        try {
            $done = [long]0; $last = -1; $buf = New-Object byte[] 1048576
            foreach ($en in $z.Entries) {
                $p = $en.FullName.Replace('\', '/')
                if ($p.EndsWith('/') -or ($info.prefix -and -not $p.StartsWith($info.prefix))) { continue }
                $rel = $p.Substring($info.prefix.Length)
                if (Test-GbModPath $rel) { throw 'unsafe path' }                          # checked again: never trust the first look
                $ext = [IO.Path]::GetExtension($rel).TrimStart('.').ToLowerInvariant()
                if ($script:GbModBlocked -contains $ext) { throw 'program in zip' }
                $out = [IO.Path]::GetFullPath((Join-Path $tmp $rel.Replace('/', '\')))
                if (-not $out.StartsWith($tmpRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'path outside the mod' }
                New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
                $src = $en.Open(); $fs = [IO.File]::Open($out, 'CreateNew', 'Write', 'None')
                try {
                    $n = [long]0; $crc = [uint32]0
                    while (($k = $src.Read($buf, 0, $buf.Length)) -gt 0) {
                        $n += $k
                        if ($n -gt $en.Length) { throw 'a file is larger than the zip says' }   # a zip that lies about its sizes
                        $fs.Write($buf, 0, $k); $done += $k; $crc = [GbMods.Crc]::Update($crc, $buf, $k)
                        $pc = [int]([Math]::Floor($done * 95 / [Math]::Max(1, $info.total)))
                        if ($Progress -and $pc -ne $last) { $last = $pc; & $Progress $pc }
                    }
                    if ($n -ne $en.Length) { throw 'a file in the zip is damaged' }
                    # .NET Framework's zip reader does not check the stored CRC-32: do it here.
                    $want = Get-GbZipCrc $en
                    if ($null -ne $want -and $want -ne $crc) { throw 'a file in the zip is damaged' }
                } finally { $fs.Dispose(); $src.Dispose() }
            }
        } finally { $z.Dispose() }
        # In place: the old copy aside, the new one in, the old one deleted.
        if (Test-Path -LiteralPath $dst) { [IO.Directory]::Move($dst, $old); $movedOld = $true }
        [IO.Directory]::Move($tmp, $dst)
        $order = Get-GbModOrder $ModsDir
        if ($null -eq (Read-GbLoadOrder $ModsDir)) { foreach ($o in $order) { $o.on = $true } }   # no file yet: every folder was on
        $mine = $order | Where-Object { $_.folder -ieq $folder } | Select-Object -First 1
        if (-not $movedOld) { $order.Remove($mine); $mine.on = $true; [void]$order.Add($mine) }            # a new mod: last, on
        Write-GbLoadOrder $ModsDir $order
        if ($movedOld) { Remove-GbDirectory $old }
        if ($Progress) { & $Progress 100 }
        $info.ok = $true; return $info
    } catch {
        $why = $_.Exception.Message
        Remove-GbDirectory $tmp
        if ($movedOld -and -not (Test-Path -LiteralPath $dst) -and (Test-Path -LiteralPath $old)) { try { [IO.Directory]::Move($old, $dst) } catch { } }
        $reason = $(switch -Wildcard ($why) {
            'unsafe path' { 'Unsafe file path.' }
            'program in zip' { 'Contains programs.' }
            '*larger than the zip says*' { 'Damaged zip.' }
            '*damaged*' { 'Damaged zip.' }
            '*being used by another process*' { 'A file is in use. Close the game first.' }
            '*denied*' { 'No permission to write in the mods folder.' }
            default { 'Install failed.' } })
        return [ordered]@{ ok = $false; zip = $info.zip; reason = $reason; detail = $why }
    }
}

# The CRC-32 a zip stores for an entry (the reader keeps it in a private field), or $null.
function Get-GbZipCrc($en) {
    foreach ($nm in '_crc32', 'crc32') {
        $f = $en.GetType().GetField($nm, [Reflection.BindingFlags]'NonPublic,Instance')
        if ($f) { return [uint32]$f.GetValue($en) }
    }
    $p = $en.GetType().GetProperty('Crc32'); if ($p) { return [uint32]$p.GetValue($en) }
    $null
}

function Remove-GbDirectory([string]$dir) {
    if (-not $dir -or -not (Test-Path -LiteralPath $dir)) { return }
    # Never follow a junction or link out of the mods folder: remove the link itself.
    $it = Get-Item -LiteralPath $dir -Force
    if ($it.Attributes -band [IO.FileAttributes]::ReparsePoint) { [IO.Directory]::Delete($it.FullName, $false); return }
    foreach ($sub in Get-ChildItem -LiteralPath $dir -Force) {
        if ($sub.PSIsContainer) { Remove-GbDirectory $sub.FullName } else { $sub.Attributes = 'Normal'; $sub.Delete() }
    }
    [IO.Directory]::Delete($it.FullName, $false)
}
# Left behind by an install or removal that was cut short (power, a crash).
function Clear-GbModTemp([string]$ModsDir) {
    if (-not (Test-Path -LiteralPath $ModsDir)) { return }
    foreach ($d in Get-ChildItem -LiteralPath $ModsDir -Directory -Force | Where-Object { $_.Name -like '.installing-*' -or $_.Name -like '.removing-*' }) {
        $real = Join-Path $ModsDir ($d.Name -replace '^\.(installing|removing)-', '')
        # A replaced mod set aside and never put back (the new one did not arrive): restore it.
        if ($d.Name -like '.removing-*' -and -not (Test-Path -LiteralPath $real)) { try { [IO.Directory]::Move($d.FullName, $real); continue } catch { } }
        try { Remove-GbDirectory $d.FullName } catch { }
    }
    $n = Join-Path $ModsDir 'load_order.txt.new'
    if (Test-Path -LiteralPath $n) { Remove-Item -LiteralPath $n -Force -ErrorAction SilentlyContinue }
}

# Removes a mod: out of the game's sight first (renamed), then to the Recycle
# Bin (deleted for good where there is none), then out of the order.
function Remove-GbMod([string]$ModsDir, [string]$Folder) {
    $dir = Join-Path $ModsDir $Folder
    if (-not $Folder -or (Test-GbModPath $Folder) -or $Folder.Contains('/') -or -not (Test-Path -LiteralPath $dir -PathType Container)) { return @{ ok = $false; reason = 'Mod not found.' } }
    $aside = Join-Path $ModsDir ('.removing-' + $Folder + '-' + [DateTime]::Now.ToString('HHmmssfff'))
    try { [IO.Directory]::Move($dir, $aside) } catch { return @{ ok = $false; reason = 'A file is in use. Close the game first.' } }
    $order = @(Get-GbModOrder $ModsDir)
    if ($null -ne (Read-GbLoadOrder $ModsDir)) { Write-GbLoadOrder $ModsDir $order }
    $bin = $false
    # A folder holding a link (made by hand; installs never make one) is removed here,
    # link by link, so nothing outside the mod can be touched. GB_MODS_NO_BIN: tests.
    $links = @(Get-ChildItem -LiteralPath $aside -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue)
    if (-not $links.Count -and -not $env:GB_MODS_NO_BIN) { try { $bin = [GbMods.Bin]::Recycle((Get-Item -LiteralPath $aside -Force).FullName) } catch { } }
    if (Test-Path -LiteralPath $aside) { try { Remove-GbDirectory $aside } catch { } }
    @{ ok = $true; folder = $Folder; recycled = $bin }
}
