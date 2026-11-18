# Read default.xbe out of an Xbox disc image without Python, so the launcher
# can check the disc before anything is downloaded. Dot-source it:
#
#   . setup\disc_peek.ps1
#   $r = Read-GbDiscFile -Image 'D:\disc.iso' -Name 'default.xbe' -Out 'C:\tmp\default.xbe'
#   if ($r.Error) { ... } else { $r.Size }
#
# A port of the toolkit's own reader (xboxrecomp tools/xiso/xdvdfs.py, MIT):
# the same partition bases, the same descriptor, the same linear walk of the
# root directory's entries with a resync at each 2048-byte sector. Only the
# root directory is read, which is where default.xbe lives. The image is
# opened read-only and shared.

function Read-GbDiscFile([string]$Image, [string]$Name, [string]$Out) {
    $SECTOR = 2048
    $magic = [Text.Encoding]::ASCII.GetBytes('MICROSOFT*XBOX*MEDIA')
    $bases = @(0x00000000, 0x0000FD90, 0x00030600, 0x0FD90000, 0x18300000)
    $fs = $null
    try {
        $fs = [IO.File]::Open($Image, 'Open', 'Read', 'ReadWrite')
        $br = New-Object IO.BinaryReader($fs)
        $base = -1
        foreach ($b in $bases) {
            $at = [long]$b + 32 * $SECTOR
            if ($at + $magic.Length -gt $fs.Length) { continue }
            $fs.Position = $at
            $got = $br.ReadBytes($magic.Length)
            if ([Linq.Enumerable]::SequenceEqual([byte[]]$got, [byte[]]$magic)) { $base = [long]$b; break }
        }
        if ($base -lt 0) { return [pscustomobject]@{ Error = 'not-xdvdfs'; Size = 0 } }
        $fs.Position = $base + 32 * $SECTOR
        $desc = $br.ReadBytes($SECTOR)
        $rootSector = [BitConverter]::ToUInt32($desc, 0x14)
        $rootSize   = [BitConverter]::ToUInt32($desc, 0x18)
        if ($rootSize -le 0 -or $rootSize -gt 64MB) { return [pscustomobject]@{ Error = 'bad-root'; Size = 0 } }
        $fs.Position = $base + [long]$rootSector * $SECTOR
        $data = $br.ReadBytes([int]$rootSize)
        $found = $null
        for ($s = 0; $s -lt $data.Length -and -not $found; $s += $SECTOR) {
            $end = [Math]::Min($s + $SECTOR, $data.Length)
            $off = $s
            while ($off + 0x0E -le $end) {
                $l = [BitConverter]::ToUInt16($data, $off); $r = [BitConverter]::ToUInt16($data, $off + 2)
                $sec = [BitConverter]::ToUInt32($data, $off + 4); $sz = [BitConverter]::ToUInt32($data, $off + 8)
                $attr = $data[$off + 12]; $nlen = $data[$off + 13]
                if (($l -eq 0xFFFF -and $r -eq 0xFFFF) -or $nlen -eq 0) { break }
                if ($off + 0x0E + $nlen -gt $end) { break }
                $nm = [Text.Encoding]::ASCII.GetString($data, $off + 0x0E, $nlen)
                if (-not ($attr -band 0x10) -and $nm -ieq $Name) { $found = @{ Sector = $sec; Size = $sz }; break }
                $off += (0x0E + $nlen + 3) -band (-bnot 3)
            }
        }
        if (-not $found) { return [pscustomobject]@{ Error = 'no-file'; Size = 0 } }
        $start = $base + [long]$found.Sector * $SECTOR
        if ($start + $found.Size -gt $fs.Length) { return [pscustomobject]@{ Error = 'truncated'; Size = 0 } }
        $fs.Position = $start
        $dir = Split-Path $Out
        if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
        [IO.File]::WriteAllBytes($Out, $br.ReadBytes([int]$found.Size))
        return [pscustomobject]@{ Error = $null; Size = $found.Size }
    } catch {
        return [pscustomobject]@{ Error = 'read: ' + $_.Exception.Message; Size = 0 }
    } finally {
        if ($fs) { $fs.Dispose() }
    }
}
