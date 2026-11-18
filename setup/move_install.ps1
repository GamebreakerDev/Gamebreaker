# Move an install folder (the launcher's Settings, Advanced, Install folder, Move).
#
#   move_install.ps1 -From <install> -To <new folder> [-Events <file>]
#
# Everything in the install moves with it: the built game, the tools, the game
# files, saves, screenshots and logs. Nothing is rebuilt: the game runs from
# the new place at once. (The next Rebuild or update compiles afresh, because
# the build folder remembers where it was; setup sees that and starts it over.)
#
# Safe by construction:
#   same drive     one rename of the folder (all or nothing)
#   other drive    copy, compare every file's size, only then delete the old
#                  folder; a failed copy deletes the partial new folder and
#                  leaves the old one as it was
# A folder linked in by setup (game_files as a junction to the player's own
# extracted disc) is linked again at the new place, never copied, and is
# unlinked, never followed, when the old folder is deleted.
# Refused: the game running from the install; a target inside the install (or
# the other way round); a target that exists and is not empty; too little space.

param(
    [Parameter(Mandatory)] [string]$From,
    [Parameter(Mandatory)] [string]$To,
    [string]$Events = '',
    [switch]$NoPrompt     # accepted for the launcher's engine runner
)

$ErrorActionPreference = 'Stop'
$utf8 = New-Object Text.UTF8Encoding($false)
function Emit([hashtable]$o) {
    if (-not $Events) { return }
    $line = ($o | ConvertTo-Json -Compress -Depth 4) + "`n"
    for ($i = 0; $i -lt 10; $i++) { try { [IO.File]::AppendAllText($Events, $line, $utf8); return } catch { Start-Sleep -Milliseconds 25 } }
}
function Fail([string]$code, [string]$msg) { $e = New-Object Exception $msg; $e.Data['code'] = $code; throw $e }
function Get-Junctions([string]$dir) {
    @(Get-ChildItem -LiteralPath $dir -Force -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })
}
# Files and bytes under a folder, not following junctions.
function Measure-Tree([string]$dir) {
    $n = 0; [long]$b = 0
    $stack = New-Object System.Collections.Stack; $stack.Push((Get-Item -LiteralPath $dir))
    while ($stack.Count) {
        $d = $stack.Pop()
        foreach ($c in Get-ChildItem -LiteralPath $d.FullName -Force -ErrorAction Stop) {
            if ($c.PSIsContainer) { if (-not ($c.Attributes -band [IO.FileAttributes]::ReparsePoint)) { $stack.Push($c) } }
            else { $n++; $b += $c.Length }
        }
    }
    @{ files = $n; bytes = $b }
}
# Delete a folder without following any junction inside it.
function Remove-Tree([string]$dir) {
    if (-not (Test-Path -LiteralPath $dir)) { return }
    $stack = New-Object System.Collections.Stack; $stack.Push($dir)
    $all = New-Object System.Collections.ArrayList
    while ($stack.Count) {
        $d = $stack.Pop(); [void]$all.Add($d)
        foreach ($c in Get-ChildItem -LiteralPath $d -Force -Directory -ErrorAction SilentlyContinue) {
            if ($c.Attributes -band [IO.FileAttributes]::ReparsePoint) { [IO.Directory]::Delete($c.FullName, $false) }   # the link only
            else { $stack.Push($c.FullName) }
        }
    }
    for ($i = $all.Count - 1; $i -ge 0; $i--) {
        Get-ChildItem -LiteralPath $all[$i] -Force -File -ErrorAction SilentlyContinue | ForEach-Object { $_.Attributes = 'Normal'; [IO.File]::Delete($_.FullName) }
        [IO.Directory]::Delete($all[$i], $false)
    }
}

try {
    $From = [IO.Path]::GetFullPath($From).TrimEnd('\')
    $To = [IO.Path]::GetFullPath($To).TrimEnd('\')
    if (-not (Test-Path -LiteralPath (Join-Path $From 'state'))) { Fail 'move' "$From is not a Gamebreaker install." }
    if ($From -ieq $To) { Fail 'move' 'That is the folder the game is in already.' }
    if ($To.StartsWith($From + '\', 'OrdinalIgnoreCase') -or $From.StartsWith($To + '\', 'OrdinalIgnoreCase')) { Fail 'move' 'The new folder cannot be inside the old one, or the other way round.' }
    if ((Test-Path -LiteralPath $To) -and @(Get-ChildItem -LiteralPath $To -Force).Count) { Fail 'move' "$To is not empty. Choose an empty or new folder." }
    # The exact program: the launcher's starter is also named Gamebreaker.exe.
    $gameExe = Join-Path $From 'build\gamebreaker.exe'
    $running = @(Get-Process gamebreaker -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -ieq $gameExe } catch { $false } })
    if ($running) { Fail 'running' 'The game is running. Close it, then move the folder.' }
    $parent = Split-Path $To
    if (-not $parent -or -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force $parent | Out-Null }

    $sameDrive = ([IO.Path]::GetPathRoot($From) -ieq [IO.Path]::GetPathRoot($To))
    Emit @{ ev = 'move'; state = 'start'; from = $From; to = $To; same = $sameDrive }
    if ($sameDrive) {
        if (Test-Path -LiteralPath $To) { [IO.Directory]::Delete($To, $false) }   # empty, checked above
        try { [IO.Directory]::Move($From, $To) }
        catch { Fail 'busy' "The folder could not be moved; a file in it may be open ($($_.Exception.Message.Trim())). Nothing was changed." }
    } else {
        $size = Measure-Tree $From
        $drive = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($To))
        if ($drive.AvailableFreeSpace -lt $size.bytes + 512MB) { Fail 'space' ("{0} has {1:N1} GB free; the game needs {2:N1} GB there." -f $drive.Name, ($drive.AvailableFreeSpace / 1GB), (($size.bytes + 512MB) / 1GB)) }
        Emit @{ ev = 'move'; state = 'copying'; files = $size.files; bytes = $size.bytes }
        $links = Get-Junctions $From
        $rc = Join-Path $env:SystemRoot 'System32\robocopy.exe'
        $xd = @(); foreach ($l in $links) { $xd += @('/XD', $l.FullName) }
        $p = Start-Process -FilePath $rc -ArgumentList (@("`"$From`"", "`"$To`"", '/E', '/COPY:DAT', '/DCOPY:T', '/R:1', '/W:1', '/XJ', '/NFL', '/NDL', '/NJH', '/NJS', '/NP') + ($xd | ForEach-Object { if ($_ -eq '/XD') { $_ } else { "`"$_`"" } })) -WindowStyle Hidden -PassThru
        $null = $p.Handle
        while (-not $p.WaitForExit(2000)) {
            try { $now = Measure-Tree $To; Emit @{ ev = 'move'; state = 'copying'; got = $now.bytes; bytes = $size.bytes } } catch { }
        }
        $ok = $p.ExitCode -lt 8
        if ($ok) {
            $copied = Measure-Tree $To
            $ok = ($copied.files -eq $size.files -and $copied.bytes -eq $size.bytes)
        }
        if (-not $ok) {
            Remove-Tree $To
            Fail 'copy' "Copying to $To did not complete (robocopy exit code $($p.ExitCode)). The game stays in $From; nothing there was changed."
        }
        foreach ($l in $links) {
            $target = $l.Target | Select-Object -First 1
            $dst = Join-Path $To $l.Name
            $r = cmd /c mklink /J "$dst" "$target" 2>&1
            if ($LASTEXITCODE) { Remove-Tree $To; Fail 'copy' "The linked folder $($l.Name) could not be linked at the new place ($r). The game stays in $From." }
        }
        Emit @{ ev = 'move'; state = 'removing' }
        Remove-Tree $From
    }
    Emit @{ ev = 'move'; state = 'done'; folder = $To }
    exit 0
} catch {
    $code = $(if ($_.Exception.Data['code']) { [string]$_.Exception.Data['code'] } else { 'move' })
    Emit @{ ev = 'error'; step = 'move'; code = $code; text = $_.Exception.Message }
    [Console]::Error.WriteLine("move_install: $($_.Exception.Message)")
    exit 1
}
