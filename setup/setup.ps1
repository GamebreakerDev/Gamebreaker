# Gamebreaker setup: from this folder and your own disc image to a game that runs.
#
#   setup\setup.cmd "D:\path\to\your disc.iso"
#   setup\setup.cmd "D:\path\to\extracted disc folder"
#   setup\setup.cmd                       (again later: checks everything, changes nothing)
#
# Options (after the disc, or alone):
#   -Folder <path>              where everything goes. Without it the first run asks,
#                               offering %LOCALAPPDATA%\Gamebreaker, and later runs
#                               use the answer (kept in setup\folder.txt)
#   -NoPrompt                   do not ask; use the remembered folder or the default
#   -Events <file>              also write each event as one JSON line to <file>:
#                               what the launcher window (launcher\prototype\host.ps1)
#                               follows to show progress and errors
#   -CheckOnly                  run only the checks of step 1, then stop
#   -VerifyDisc                 only read default.xbe from the disc and check its
#                               hash (no download, no Python), then stop
#   -IgnoreSmartAppControl      skip the Smart App Control test program (Windows will
#                               most likely refuse to start the game)
#
# What it does, in order, each step only if it is not already done:
#   1. checks Windows, 64-bit, the folder's path, free disk space and Smart App Control
#   2. downloads the pinned tools in setup\tools.lock and checks each hash
#      (embeddable Python and capstone, llvm-mingw, Ninja, CMake)
#   3. copies the toolkit and applies port\patches (setup\apply_patch.py; no Git).
#      Before the disc, because the disc reader (tools.xiso) is part of it
#   4. reads default.xbe from the disc and checks it is the supported release,
#      before anything else is done with the disc
#   5. unpacks the disc (or links to the folder you gave)
#   6. lifts default.xbe to C (port\tools\pipeline.ps1, the same code as port\regen.ps1)
#   7. compiles with Clang
#   8. writes play.cmd, which starts the game with the switches it needs
#
# It only ever writes inside -Folder. Nothing is installed, PATH and the
# registry are not changed, and deleting the folder removes everything. The
# disc image is only read. A step that fails says which step and why; running
# setup again resumes at that step.
#

param(
    [Parameter(Position = 0)] [string]$Disc = '',
    [string]$Folder = '',
    [switch]$IgnoreSmartAppControl,
    [switch]$NoPrompt,              # do not ask for the folder: use -Folder, the remembered one, or the default
    [string]$Events = '',           # also write one JSON line per event to this file (the launcher window reads it)
    [switch]$CheckOnly,             # only step 1 (checks), then stop: what the launcher shows first
    [switch]$VerifyDisc             # only check the disc's default.xbe (no download, no Python), then stop
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

$src     = Split-Path $PSScriptRoot                 # the release (or repository) folder
$portDir = Join-Path $src 'port'
$tkSrc   = Join-Path $src 'xboxrecomp'
if (-not $Lock)   { $Lock = Join-Path $PSScriptRoot 'tools.lock' }
# The install folder: %LOCALAPPDATA%\Gamebreaker
# by default, but the first run asks. The answer is remembered in
# setup\folder.txt beside this script, so later runs go to the same place.
$remember = Join-Path $PSScriptRoot 'folder.txt'
$default  = Join-Path $env:LOCALAPPDATA 'Gamebreaker'
$chosenHere = -not $Folder
if (-not $Folder -and (Test-Path $remember)) { $Folder = (Get-Content $remember -Raw).Trim() }
if (-not $Folder) {
    $canAsk = -not $NoPrompt -and -not [Console]::IsInputRedirected
    if ($canAsk -and -not (Test-Path (Join-Path $default 'state'))) {
        Write-Host "Install folder. It needs about 3.5 GB. Press Enter for the default:"
        Write-Host "  $default"
        $answer = (Read-Host 'Folder').Trim().Trim('"')
        $Folder = if ($answer) { $answer } else { $default }
    } else {
        $Folder = $default
    }
}
$Folder = [IO.Path]::GetFullPath($Folder)
. (Join-Path $PSScriptRoot 'release_manifest.ps1')
$Folder = Resolve-GbInstallFolder $src $Folder      # never the folder that holds Gamebreaker.exe
# Remembered only when not given with -Folder (tests pass -Folder each time).
if ($chosenHere) { try { Set-Content -Path $remember -Value $Folder -Encoding utf8 } catch { } }   # a read-only setup folder only loses the memory

. (Join-Path $portDir 'tools\pipeline.ps1')

$tools   = Join-Path $Folder 'tools'
$toolkit = Join-Path $Folder 'toolkit'
$game    = Join-Path $Folder 'game_files'            # main.c opens game_files\ relative to the working folder
$work    = Join-Path $Folder 'work'
$gen     = Join-Path $work 'gen'
$build   = Join-Path $Folder 'build'
$state   = Join-Path $Folder 'state'
$logs    = Join-Path $Folder 'logs'

# Disk use, measured on the development PC (stage times from a full run there).
$NeedTools = 1.2GB; $NeedDisc = 2.2GB; $NeedWork = 0.7GB

# ---------------------------------------------------------------- output

$script:log = $null
$script:changed = 0
# Events for the launcher window: one compact JSON object a line, appended.
# ev = check, step, tool, xbe, lift, log, error, done. A line is written in
# one call, so a reader never sees half of one.
$script:utf8 = New-Object Text.UTF8Encoding($false)
function Emit([hashtable]$o) {
    if (-not $Events) { return }
    $line = ($o | ConvertTo-Json -Compress -Depth 4) + "`n"
    for ($i = 0; $i -lt 10; $i++) {
        try { [IO.File]::AppendAllText($Events, $line, $script:utf8); return } catch { Start-Sleep -Milliseconds 25 }
    }
}
# A failure with a name the launcher maps to its own wording (an error code).
function Fail([string]$code, [string]$msg) { $e = New-Object Exception $msg; $e.Data['code'] = $code; throw $e }
$script:times = [ordered]@{}
function Say([string]$label, [string]$text) {
    $line = '{0,-19}{1}' -f $label, $text
    Write-Host $line
    if ($script:log) { Add-Content -Path $script:log -Value $line -Encoding utf8 }
    Emit @{ ev = 'log'; text = $line }
}
function Stop-Setup([string]$step, [string]$why, [string]$code = '') {
    Emit @{ ev = 'error'; step = $step; code = $(if ($code) { $code } else { $step }); text = $why; log = [string]$script:log }
    Write-Host ''
    Write-Host "Setup stopped at step: $step" -ForegroundColor Red
    foreach ($l in ($why -split "`n")) { Write-Host "  $l" }
    if ($script:log) {
        Add-Content -Path $script:log -Value @('', "STOPPED at $step", $why) -Encoding utf8
        Write-Host "  Log: $script:log"
    }
    Write-Host '  Run setup again to continue from this step.'
    exit 1
}
# Run one step; any error becomes a named stop.
function Step([string]$name, [scriptblock]$body) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Emit @{ ev = 'step'; name = $name; state = 'start' }
    try { & $body } catch { Stop-Setup $name $_.Exception.Message ([string]$_.Exception.Data['code']) }
    $script:times[$name] = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    Emit @{ ev = 'step'; name = $name; state = 'done'; secs = $script:times[$name] }
}
function Done([string]$name)            { Join-Path $state "$name.done" }
function Test-Done([string]$name, [string]$fp) {
    $f = Done $name
    (Test-Path $f) -and ((Get-Content $f -Raw).Trim() -eq $fp)
}
function Set-Done([string]$name, [string]$fp) { Set-Content -Path (Done $name) -Value $fp -Encoding ascii }
function Get-Sha([string]$text) {
    $h = [Security.Cryptography.SHA256]::Create()
    ([BitConverter]::ToString($h.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))) -replace '-', '').ToLower()
}
function Get-FileSha([string]$path) { (Get-FileHash $path -Algorithm SHA256).Hash.ToLower() }
# Remove a folder link (junction) without touching what it points to.
# Windows PowerShell 5.1's Remove-Item -Recurse would delete the target's
# files, so the link goes through cmd's rmdir, which removes only the link.
function Remove-GbLink([string]$path) {
    $i = Get-Item $path -Force -ErrorAction SilentlyContinue
    if ($i -and ($i.Attributes -band [IO.FileAttributes]::ReparsePoint)) { cmd /c "rmdir `"$path`" 2>nul" | Out-Null }
}
function GB([double]$b) { '{0:N1} GB' -f ($b / 1GB) }
# The game built in this folder, by its exact path: the launcher's starter is
# also named Gamebreaker.exe and may sit beside an install folder of that name.
function Test-BuiltGameRunning {
    $exe = Join-Path $Folder 'build\gamebreaker.exe'
    [bool](Get-Process gamebreaker -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -ieq $exe } catch { $false } })
}

# ---------------------------------------------------------------- start

foreach ($p in @($portDir, $Lock)) {
    if (-not (Test-Path $p)) { Write-Host "This setup folder is incomplete: $p is missing. Unpack the whole release again." -ForegroundColor Red; exit 1 }
}
. (Join-Path $PSScriptRoot 'disc_peek.ps1')
. (Join-Path $PSScriptRoot 'retry.ps1')

# Read default.xbe from a disc image (or a folder) and check it, without
# Python: the launcher does this before anything is downloaded. Returns the
# checked file's info; throws a named failure otherwise.
function Test-GbDisc([string]$path, [string]$scratch) {
    if (-not $path) { Fail 'no_disc' "No disc given. Run:  setup.cmd `"<your disc image or extracted disc folder>`"" }
    if (-not (Test-Path -LiteralPath $path)) { Fail 'no_disc' "Not found: $path" }
    if ((Get-Item -LiteralPath $path).PSIsContainer) {
        $x = Join-Path $path 'default.xbe'
        if (-not (Test-Path -LiteralPath $x)) { Fail 'no_xbe' "$path has no default.xbe. Give the folder that holds the extracted disc, or the disc image." }
    } else {
        $ext = [IO.Path]::GetExtension($path).ToLower()
        if ($ext -in '.cci', '.cso', '.7z', '.zip', '.rar') {
            Fail 'compressed' "$ext files are not supported. Give an uncompressed Xbox disc image (.iso) or a folder with the extracted disc."
        }
        $x = Join-Path $scratch 'default.xbe'
        if (Test-Path $x) { Remove-Item $x -Force }
        $r = Read-GbDiscFile -Image $path -Name 'default.xbe' -Out $x
        if ($r.Error) {
            if ($r.Error -eq 'no-file') { Fail 'no_xbe' "This disc image has no default.xbe." }
            Fail 'not_disc' "This file could not be read as an Xbox disc image ($($r.Error)). Supported: an uncompressed Xbox disc image (.iso), or a folder with the extracted disc. Split images are not supported."
        }
    }
    $info = Assert-GbXbe $x
    Emit @{ ev = 'xbe'; ok = $true; sha = $info.Sha256; title = ('{0:X8}' -f $info.TitleId); region = $info.Region }
    return $info
}

if ($VerifyDisc) {
    $scratch = Join-Path ([IO.Path]::GetTempPath()) ('gb_disc_' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force $scratch | Out-Null
    try {
        $info = Test-GbDisc $Disc $scratch
        Write-Host "default.xbe is the supported release ($($info.Sha256))"
        Emit @{ ev = 'done'; mode = 'verify' }
        exit 0
    } catch {
        $code = [string]$_.Exception.Data['code']; if (-not $code) { $code = 'disc' }
        Emit @{ ev = 'error'; step = 'disc'; code = $code; text = $_.Exception.Message }
        Write-Host $_.Exception.Message -ForegroundColor Red
        exit 1
    } finally { Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue }
}

if (-not $CheckOnly) {
    New-Item -ItemType Directory -Force $Folder, $state, $logs, $work | Out-Null
    $script:log = Join-Path $logs ('setup_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '.txt')
}

# One setup at a time in a folder: the second would trip over the first's files.
if (-not $CheckOnly) { try {
    $guard = [IO.File]::Open((Join-Path $Folder 'setup.running'), 'OpenOrCreate', 'ReadWrite', 'None')
} catch {
    Emit @{ ev = 'error'; step = 'start'; code = 'busy'; text = "Setup is already running for $Folder." }
    Write-Host "Setup is already running for $Folder in another window." -ForegroundColor Red; exit 1
} }

$total = [Diagnostics.Stopwatch]::StartNew()
Write-Host 'Gamebreaker setup'
Say 'Folder' $Folder
Emit @{ ev = 'start'; folder = $Folder }

# ---------------------------------------------------------------- 1. checks

Step 'checks' {
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    $buildNo = [int]$cv.CurrentBuild
    $winName = if ($buildNo -ge 22000) { 'Windows 11' } else { 'Windows 10' }
    $ver = if ($cv.DisplayVersion) { " $($cv.DisplayVersion)" } else { '' }
    if (-not [Environment]::Is64BitOperatingSystem) { Fail '32bit' "$winName$ver is 32-bit. The game needs 64-bit Windows." }
    Say 'Windows' "$winName$ver (build $buildNo), 64-bit"
    Emit @{ ev = 'check'; key = 'os'; ok = ($buildNo -ge 17763); text = "$winName$ver, 64-bit" }
    if ($buildNo -lt 17763) { Fail 'oldwin' "Windows build $buildNo is older than Windows 10 version 1809 (build 17763), the oldest this setup supports." }

    # MinGW-family tools have failed under folders with non-English letters,
    # and Windows paths stop at 260 characters.
    if ($Folder -match '[^\x20-\x7E]') {
        Fail 'path' "The folder's path has letters outside plain English ($Folder). The compiler can fail there.`nRun setup again with a plain folder, for example:  setup.cmd `"<disc>`" -Folder C:\Gamebreaker"
    }
    if ($Folder.Length -gt 120) {
        Fail 'longpath' "The folder's path is $($Folder.Length) characters long; the build needs it under 120 (Windows' 260-character limit).`nRun setup again with a shorter one, for example:  -Folder C:\Gamebreaker"
    }

    $need = 0
    if (-not (Test-Path (Join-Path $tools 'llvm-mingw'))) { $need += $NeedTools }
    if (-not (Test-Path (Done 'disc')))                   { $need += $NeedDisc }
    if (-not (Test-Path (Join-Path $build 'gamebreaker.exe'))) { $need += $NeedWork }
    $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($Folder))
    Say 'Disk' ("{0} free on {1}, about {2} needed" -f (GB $drive.AvailableFreeSpace), $drive.Name.TrimEnd('\'), (GB $need))
    Emit @{ ev = 'check'; key = 'space'; ok = ($drive.AvailableFreeSpace -ge $need + 0.5GB); free = $drive.AvailableFreeSpace; need = ($need + 0.5GB)
            drive = $drive.Name.TrimEnd('\'); text = ("{0} free on {1}" -f (GB $drive.AvailableFreeSpace), $drive.Name.TrimEnd('\')) }
    if ($drive.AvailableFreeSpace -lt $need + 0.5GB) {
        Fail 'space' ("Not enough disk space: about {0} needed, {1} free on {2}.`nFree some space, or run setup again with -Folder on another drive." -f (GB ($need + 0.5GB)), (GB $drive.AvailableFreeSpace), $drive.Name)
    }

    # Smart App Control (Windows 11): when it is enforcing, Windows refuses to
    # start an unsigned program it has never seen, and a game compiled on this
    # PC is exactly that. Decided 2026-11-16: test
    # first. Setup goes on, and right after the tool download builds and runs a
    # tiny program (step 'sac test'); it stops only if Windows blocks that.
    $sac = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -ErrorAction SilentlyContinue).VerifiedAndReputablePolicyState
    $sacState = switch ($sac) { 0 { 'off' } 1 { 'on' } 2 { 'evaluation' } default { 'none' } }
    $script:sacOn = ($sac -eq 1 -or $sac -eq 2)     # evaluation mode may switch itself on later: test it too
    Emit @{ ev = 'check'; key = 'sac'; ok = $true; state = $sacState }
    switch ($sac) {
        0       { Say 'Smart App Control' 'off' }
        1       { Say 'Smart App Control' 'on: a test program is built and run after the download; setup stops if Windows blocks it' }
        2       { Say 'Smart App Control' 'evaluation mode: a test program is built and run after the download (Windows may switch it on later)' }
        default { Say 'Smart App Control' 'not present' }
    }

    # The graphics adapter the game will draw on, for the launcher's "This PC" list:
    # the working dedicated chip when there is one, otherwise the integrated one
    # (gpu_names.ps1). Only named here: whether it can do Direct3D 11 is found
    # when the game starts.
    . (Join-Path $PSScriptRoot 'gpu_names.ps1')
    $adapters = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue)
    $gpu = Select-GameGpu $adapters
    if ($gpu) { Say 'Graphics' $gpu }
    Emit @{ ev = 'check'; key = 'gpu'; ok = [bool]$gpu; text = $(if ($gpu) { [string]$gpu } else { 'No graphics adapter found' }) }

    if (Test-BuiltGameRunning) { Fail 'running' 'The game built in this folder is running. Close it, then run setup again.' }
}
if ($CheckOnly) { Emit @{ ev = 'done'; mode = 'check' }; exit 0 }

# ---------------------------------------------------------------- 2. tools

$entries = (Get-Content $Lock -Raw | ConvertFrom-Json).tools
$py = Join-Path $tools 'python\python.exe'
. (Join-Path $PSScriptRoot 'tree_hash.ps1')
$tkEntry = $entries | Where-Object name -eq 'xboxrecomp'
Step 'tools' {
    # The toolkit (decided 2026-11-16: "download it at setup"): a copy beside this
    # setup folder is used only when its files match the pinned fingerprint (a
    # release made with make_release.ps1 -WithToolkit, kept for the day the
    # upstream repository is gone); otherwise it is downloaded from upstream with
    # the tools below and checked the same way.
    $script:tkLocal = $false
    if ($tkEntry -and (Test-Path $tkSrc)) {
        $fp = Get-GbTreeHash $tkSrc
        if ($fp.Hash -eq $tkEntry.tree_sha256) { $script:tkLocal = $true; Say 'Toolkit' "using the copy beside setup ($($fp.Files) files, fingerprint matches)" }
        else { Say 'Toolkit' "the copy beside setup is not the pinned version ($($fp.Files) files); the pinned one is downloaded" }
    }
    $skip = @(); if ($script:tkLocal) { $skip = @('xboxrecomp') }
    $missing = @($entries | Where-Object { $skip -notcontains $_.name } | Where-Object {
        $m = Join-Path $tools "$($_.dir)\.gb_tool"
        $pin = $(if ($_.verify -eq 'tree') { $_.tree_sha256 } else { $_.sha256 })
        -not ((Test-Path (Join-Path $tools "$($_.dir)\$($_.check)")) -and (Test-Path $m) -and ((Get-Content $m -Raw).Trim() -eq $pin))
    })
    if ($missing) {
        $bytes = ($missing | Measure-Object size -Sum).Sum
        Say 'Tools' ("downloading {0:N1} MB: {1}" -f ($bytes / 1MB), (($missing | ForEach-Object { "$($_.name) $($_.version)" }) -join ', '))
        $script:downloaded = $bytes
        $script:changed++
    }
    & (Join-Path $PSScriptRoot 'fetch.ps1') -Root $tools -Lock $Lock -DeleteArchives -Quiet -Events $Events -Skip $skip
    if (-not $script:tkLocal -and $tkEntry) { $script:tkSrc = Join-Path $tools ("$($tkEntry.dir)\$($tkEntry.tree_root)") }

    # The embeddable Python reads python3NN._pth for where it may import from
    # and ignores PYTHONPATH and the current folder. Add capstone and the toolkit.
    $pth = Get-ChildItem (Join-Path $tools 'python') -Filter 'python3*._pth' | Select-Object -First 1
    $zip = Get-ChildItem (Join-Path $tools 'python') -Filter 'python3*.zip' | Select-Object -First 1
    if (-not $pth -or -not $zip) { throw 'The Python package is incomplete (no ._pth or standard-library zip).' }
    $want = @($zip.Name, '.', (Join-Path $tools 'capstone'), $toolkit) -join "`r`n"
    if ((Get-Content $pth.FullName -Raw).Trim() -ne $want) { Set-Content -Path $pth.FullName -Value $want -Encoding ascii }
    $r = Invoke-GbNative $py @('-I', '-B', '-c', 'import capstone, sys; print(capstone.__version__, sys.version.split()[0])') '' $script:log
    if ($r.Code -ne 0) { throw "The fetched Python cannot load capstone:`n  $(($r.Lines | Select-Object -Last 2) -join "`n  ")" }
    if (-not $missing) { Say 'Tools' ("present: " + (($entries | ForEach-Object { "$($_.name) $($_.version)" }) -join ', ')) }
}
$python = @($py, '-I', '-B', '-X', 'utf8')

# ---------------------------------------------------------------- 2b. Smart App Control, tested
# Only when it is on or in evaluation mode (decided 2026-11-16: "test first").
# After the download (the test needs the compiler) and before the long steps,
# builds a three-line program with the downloaded clang and runs it. How a
# block reaches the program that tried to start something (Microsoft's App
# Control documentation; untested on a PC with the feature on): the start
# fails, usually with Win32 error 4551 ("An Application Control policy has
# blocked this file", HRESULT 0x800711C7), sometimes 577 (signature could not
# be verified) or 1260 (blocked by policy), and the CodeIntegrity Operational
# log gets event 3077 naming the file. clang starting its linker and being
# refused shows on its error output. Any of these stops setup with code 'sac'.
# Run on every setup run while the feature is on (about a second), since
# Windows' verdict can change.
if ($script:sacOn -and -not $IgnoreSmartAppControl) {
    Step 'sac test' {
        $dir = Join-Path $state 'sac_test'
        if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
        New-Item -ItemType Directory -Force $dir | Out-Null
        [IO.File]::WriteAllText((Join-Path $dir 't.c'), "int main(void) { return 42; }`n")
        $llvm = $entries | Where-Object name -eq 'llvm-mingw'
        $cc = Join-Path $tools "$($llvm.dir)\$($llvm.check)"
        $t0 = Get-Date
        # The stop screen: plain words, no guide to Windows' settings (decided
        # 2026-11-16). The log keeps what was blocked.
        $sacMsg = "Windows blocked the game from starting on this PC.`nCheck Windows' security settings (Smart App Control), then try again."
        function Stop-Sac([string]$what) { Say 'Smart App Control' "Windows blocked $what"; Fail 'sac' $sacMsg }
        function Test-CiBlock([string]$name) {
            try {
                $ev = Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-CodeIntegrity/Operational'; Id = 3077, 3033; StartTime = $t0 } -ErrorAction Stop
                return [bool]($ev | Where-Object { $_.Message -match [regex]::Escape($name) })
            } catch { return $false }          # no events, or the log is not readable for this user
        }
        function Invoke-Blockable([string]$exe, [string]$cmdArgs, [string]$what) {
            $psi = New-Object Diagnostics.ProcessStartInfo($exe, $cmdArgs)
            $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.WorkingDirectory = $dir
            $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
            try { $p = [Diagnostics.Process]::Start($psi) }
            catch [ComponentModel.Win32Exception] {
                $code = $_.Exception.NativeErrorCode
                Say 'Smart App Control' "starting $(Split-Path $exe -Leaf) failed: Win32 error $code, $($_.Exception.Message)"
                if ($code -in 4551, 577, 1260, 5 -or (Test-CiBlock (Split-Path $exe -Leaf))) { Stop-Sac ($what) }
                throw
            }
            $err = $p.StandardError.ReadToEnd(); $null = $p.StandardOutput.ReadToEnd(); $p.WaitForExit()
            if ($err -match 'Application Control|blocked by (your|group) policy|0x800711C7|unable to execute command') {
                Say 'Smart App Control' ("blocked: " + ($err -split "`n" | Select-Object -First 1))
                Stop-Sac ($what)
            }
            return @{ code = $p.ExitCode; err = $err }
        }
        $r = Invoke-Blockable $cc 't.c -O1 -o t.exe' 'the downloaded compiler'
        if ($r.code -ne 0 -or -not (Test-Path (Join-Path $dir 't.exe'))) {
            if ((Test-CiBlock 'clang') -or (Test-CiBlock 'ld.lld')) { Stop-Sac ('the downloaded compiler') }
            throw "The Smart App Control test program did not compile:`n$($r.err)"
        }
        $r = Invoke-Blockable (Join-Path $dir 't.exe') '' 'a test program built on this PC'
        if ($r.code -ne 42) { Stop-Sac ("a test program built on this PC (it ended with code $($r.code))") }
        Say 'Smart App Control' 'the test program built here ran, so the game should run too (Windows may still decide otherwise later)'
    }
}

# ---------------------------------------------------------------- 3. toolkit (before the disc: the disc reader is part of it)

Step 'toolkit' {
    # The toolkit's own files, without Git's folder and the developer's
    # analysis output. Its fingerprint is names, sizes and times.
    $skip = '^(\.git(\\|$)|tools\\[^\\]+\\output(\\|$))|(^|\\)__pycache__(\\|$)'
    $files = @(Get-ChildItem $tkSrc -Recurse -File -Force | ForEach-Object {
        $rel = $_.FullName.Substring($tkSrc.Length + 1)
        if ($rel -notmatch $skip) { [pscustomobject]@{ Rel = $rel; Item = $_ } }
    } | Sort-Object Rel)
    $patches = @(Get-ChildItem (Join-Path $portDir 'patches') -Filter '*.patch' | Sort-Object Name)
    $fp = Get-Sha ((@($files | ForEach-Object { "$($_.Rel)|$($_.Item.Length)|$($_.Item.LastWriteTimeUtc.Ticks)" }) +
                    @($patches | ForEach-Object { "$($_.Name)|$(Get-FileSha $_.FullName)" }) +
                    @("apply_patch|$(Get-FileSha (Join-Path $PSScriptRoot 'apply_patch.py'))")) -join "`n")
    if ((Test-Done 'toolkit' $fp) -and (Test-Path $toolkit)) { Say 'Toolkit' "present, patches applied ($($patches.Count))"; return }
    $script:changed++
    $partial = "$toolkit.partial"
    if (Test-Path $partial) { Invoke-GbRetry { Remove-Item $partial -Recurse -Force } 'The toolkit folder' }
    foreach ($f in $files) {
        $to = Join-Path $partial $f.Rel
        $dir = Split-Path $to
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
        [IO.File]::Copy($f.Item.FullName, $to, $true)
    }
    $r = Invoke-GbNative $py (@('-I', '-B', (Join-Path $PSScriptRoot 'apply_patch.py'), $partial) + @($patches | ForEach-Object FullName)) '' $script:log
    if ($r.Code -ne 0) { throw "The toolkit patch does not apply:`n  $(($r.Lines | Select-Object -Last 2) -join "`n  ")" }
    if (Test-Path $toolkit) { Invoke-GbRetry { Remove-Item $toolkit -Recurse -Force } 'The toolkit folder' }
    Invoke-GbRetry { Rename-Item $partial (Split-Path $toolkit -Leaf) } 'The toolkit folder'
    Set-Done 'toolkit' $fp
    Say 'Toolkit' ("copied {0} files; {1}" -f $files.Count, (($r.Lines | Where-Object { $_ -match 'applied' }) -join '; '))
}

# ---------------------------------------------------------------- 4 and 5. disc

Step 'disc' {
    $xbe = Join-Path $game 'default.xbe'
    if ((Test-Path (Done 'disc')) -and (Test-Path $xbe)) {
        $null = Assert-GbXbe $xbe
        Say 'Disc' "unpacked; default.xbe is the supported release ($($GbXbeSha256.Substring(0, 12))...)"
        return
    }
    if (-not $Disc) { Fail 'no_disc' "No disc given. Run:  setup.cmd `"<your disc image or extracted disc folder>`"" }
    if (-not (Test-Path $Disc)) { Fail 'no_disc' "Not found: $Disc" }
    $Disc = (Resolve-Path $Disc).Path
    $script:changed++

    if ((Get-Item $Disc).PSIsContainer) {
        # An extracted disc: used where it is, through a folder link (a
        # junction; needs no administrator rights). Nothing is copied.
        $x = Join-Path $Disc 'default.xbe'
        if (-not (Test-Path $x)) { Fail 'no_xbe' "$Disc has no default.xbe. Give the folder that holds the extracted disc, or the disc image." }
        $null = Assert-GbXbe $x
        Say 'Disc' 'default.xbe is the supported release'
        Remove-GbLink $game
        if (Test-Path $game) { throw "$game exists and is not a link from an earlier run. Move it away, then run setup again." }
        New-Item -ItemType Junction -Path $game -Target $Disc | Out-Null
        Say 'Disc' "linked to $Disc"
    } else {
        $ext = [IO.Path]::GetExtension($Disc).ToLower()
        if ($ext -in '.cci', '.cso', '.7z', '.zip', '.rar') {
            Fail 'compressed' "$ext files are not supported. Give an uncompressed Xbox disc image (.iso) or a folder with the extracted disc."
        }
        # default.xbe first, alone (a second or two), and checked before
        # anything else is done with the disc.
        $chk = Join-Path $work 'xbe_check'
        if (Test-Path $chk) { Remove-Item $chk -Recurse -Force }
        New-Item -ItemType Directory -Force $chk | Out-Null
        $r = Invoke-GbNative $python[0] (@($python | Select-Object -Skip 1) + @('-m', 'tools.xiso', 'get', $Disc, 'default.xbe', '-o', $chk)) '' $script:log
        $x = Join-Path $chk 'default.xbe'
        if ($r.Code -ne 0 -or -not (Test-Path $x)) {
            Fail 'not_disc' ("This file could not be read as an Xbox disc image: $(($r.Lines | Select-Object -Last 1))`n" +
                   'Supported: an uncompressed Xbox disc image (.iso), or a folder with the extracted disc. Split images are not supported.')
        }
        $null = Assert-GbXbe $x
        Say 'Disc' 'default.xbe is the supported release'

        $partial = "$game.partial"
        if (Test-Path $partial) { Invoke-GbRetry { Remove-Item $partial -Recurse -Force } 'The disc folder' }
        Remove-GbLink $game
        if (Test-Path $game) { Invoke-GbRetry { Remove-Item $game -Recurse -Force } 'The disc folder' }    # a real folder here is an unpack of ours that lost its marker
        Say 'Disc' 'unpacking'
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $r = Invoke-GbNative $python[0] (@($python | Select-Object -Skip 1) + @('-m', 'tools.xiso', 'unpack', $Disc, '-o', $partial)) '' $script:log
        if ($r.Code -ne 0) { Fail 'unpack' "Unpacking the disc failed: $(($r.Lines | Select-Object -Last 1))" }
        $null = Assert-GbXbe (Join-Path $partial 'default.xbe')
        $n = @(Get-ChildItem $partial -Recurse -File).Count
        Invoke-GbRetry { Rename-Item $partial (Split-Path $game -Leaf) } 'The disc folder'
        Invoke-GbRetry { Remove-Item $chk -Recurse -Force } 'The disc check folder'
        Say 'Disc' ("unpacked {0} files in {1:N0} s" -f $n, $sw.Elapsed.TotalSeconds)
    }
    Set-Done 'disc' $Disc
}

# ---------------------------------------------------------------- 6. lift

Step 'lift' {
    $xbe = Join-Path $game 'default.xbe'
    $fp = Get-Sha ((@($GbXbeSha256, (Get-Content (Done 'toolkit') -Raw).Trim()) +
                    @('config\seed_functions.json', 'src\recomp_manual.c', 'tools\pipeline.ps1' | ForEach-Object { "$_|$(Get-FileSha (Join-Path $portDir $_))" })) -join "`n")
    if ((Test-Done 'lift' $fp) -and (Test-Path (Join-Path $gen 'recomp_funcs.h'))) { Say 'Lift' 'up to date'; return }
    $script:changed++
    Say 'Lift' 'parse, disassemble, identify, ABI, lift (about 3 minutes)'
    # Into a fresh folder, then copied over the old one file by file: files
    # that did not change keep their times (so the compile skips them), and a
    # file the new lift no longer writes is removed.
    $subs = @('parse', 'disasm', 'func_id', 'abi', 'lift')
    Emit @{ ev = 'lift'; sub = 'parse'; state = 'start' }
    $new = "$gen.new"
    if (Test-Path $new) { Invoke-GbRetry { Remove-Item $new -Recurse -Force } 'The lifted code folder' }
    $t = Invoke-GbLift -Python $python -Toolkit $toolkit -Xbe $xbe -PortDir $portDir -GenDir $new `
            -AnalysisDir (Join-Path $work 'analysis') -Log $script:log -Quiet `
            -OnStep { param($n, $s)
                Say '' ("{0,-8} {1,5:N0} s" -f $n, $s)
                Emit @{ ev = 'lift'; sub = $n; state = 'done'; secs = [math]::Round($s, 1) }
                $i = [array]::IndexOf($subs, $n)
                if ($i -ge 0 -and $i + 1 -lt $subs.Count) { Emit @{ ev = 'lift'; sub = $subs[$i + 1]; state = 'start' } }
            }
    foreach ($k in $t.Keys) { $script:times["lift.$k"] = [math]::Round($t[$k], 1) }
    New-Item -ItemType Directory -Force $gen | Out-Null
    $keep = @{}
    foreach ($f in Get-ChildItem $new -File) {
        $keep[$f.Name] = $true
        $old = Join-Path $gen $f.Name
        if (-not (Test-Path $old) -or (Get-FileSha $old) -ne (Get-FileSha $f.FullName)) { Invoke-GbRetry { Copy-Item $f.FullName $old -Force } 'A lifted code file' }
    }
    Invoke-GbRetry { Get-ChildItem $gen -File | Where-Object { -not $keep[$_.Name] } | Remove-Item -Force } 'A lifted code file'
    Invoke-GbRetry { Remove-Item $new -Recurse -Force } 'The lifted code folder'
    Set-Done 'lift' $fp
}

# ---------------------------------------------------------------- 7. compile

Step 'compile' {
    if (Test-BuiltGameRunning) { Fail 'running' 'The game built in this folder is running. Close it, then run setup again.' }
    $cmake = Find-GbCmake $tools -FetchedOnly
    if (-not $cmake) { throw "CMake is missing from $tools. Run setup again to download it." }
    $llvm = $entries | Where-Object name -eq 'llvm-mingw'
    $bin = Join-Path $tools "$($llvm.dir)\$($llvm.bin)"
    $ninja = Join-Path $tools 'ninja\ninja.exe'
    # A build folder remembers the source folder it was configured from. After
    # a new release is unpacked somewhere else, it would quietly go on compiling
    # the old sources (and fail to link against the new lift, as seen
    # 2026-11-16). Start it afresh when the source folder has moved.
    $cache = Join-Path $build 'CMakeCache.txt'
    if (Test-Path $cache) {
        $m = Select-String -Path $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$' | Select-Object -First 1
        $was = if ($m) { [IO.Path]::GetFullPath($m.Matches[0].Groups[1].Value.Trim()).TrimEnd('\') } else { '' }
        # The build folder itself moved (Settings, Advanced, Move: setup\move_install.ps1):
        # its cache and Ninja file name the old place.
        $mb = Select-String -Path $cache -Pattern '^CMAKE_CACHEFILE_DIR:INTERNAL=(.*)$' | Select-Object -First 1
        $wasBuild = if ($mb) { [IO.Path]::GetFullPath($mb.Matches[0].Groups[1].Value.Trim()).TrimEnd('\') } else { '' }
        if ($was -ne [IO.Path]::GetFullPath($portDir).TrimEnd('\')) {
            Say 'Compile' 'the sources moved since the last build: building afresh'
            Remove-Item $build -Recurse -Force
        } elseif ($wasBuild -and $wasBuild -ne [IO.Path]::GetFullPath($build).TrimEnd('\')) {
            Say 'Compile' 'the install folder moved since the last build: building afresh'
            Remove-Item $build -Recurse -Force
        }
    }
    # A release's version (setup\release.txt, written by make_release.ps1) is
    # the one the game's start screen shows; a checkout keeps CMake's default.
    # GB_QUIET_BUILD: no warnings from the lifted code and the toolkit, which would
    # fill the player's log with thousands of harmless lines. Errors still show.
    $defs = @{ GB_QUIET_BUILD = 'ON' }
    $relTxt = Join-Path $PSScriptRoot 'release.txt'
    if (Test-Path $relTxt) {
        $m = Select-String -Path $relTxt -Pattern '^version=([0-9A-Za-z.+-]+)\s*$' | Select-Object -First 1
        if ($m) { $defs.GB_VERSION = $m.Matches[0].Groups[1].Value }
    }
    $res = Invoke-GbClangBuild -Cmake $cmake -LlvmBin $bin -Ninja $ninja -PortDir $portDir -BuildDir $build `
               -GenDir $gen -Toolkit $toolkit -Log $script:log -Defines $defs
    $exe = Join-Path $build 'gamebreaker.exe'
    if (-not (Test-Path $exe)) { throw "The compile reported success but $exe is missing." }
    if ($res -eq 'built') { $script:changed++ }
    Say 'Compile' ("{0}: {1} ({2:N1} MB)" -f $res, $exe, ((Get-Item $exe).Length / 1MB))
}

# ---------------------------------------------------------------- 8. play.cmd

Step 'play.cmd' {
    # The program sets its own run switches and finds game_files\ by itself
    # (port\src\main.c), so play.cmd only starts it. It stays for the log
    # option and as the one obvious thing to double-click.
    $body = @(
        '@echo off'
        'rem Written by Gamebreaker setup (setup\setup.ps1). Starts the game.'
        'rem "play.cmd -log" writes the diagnostic log to logs\.'
        'cd /d "%~dp0"'
        'if /i "%~1"=="-log" ('
        '  "build\gamebreaker.exe" > "logs\game_stdout.txt" 2> "logs\game_stderr.txt"'
        '  exit /b %errorlevel%'
        ')'
        'start "" "build\gamebreaker.exe"'
    )
    $file = Join-Path $Folder 'play.cmd'
    $text = ($body -join "`r`n") + "`r`n"
    if ((Test-Path $file) -and ((Get-Content $file -Raw) -eq $text)) { Say 'play.cmd' 'up to date'; return }
    [IO.File]::WriteAllText($file, $text, [Text.Encoding]::ASCII)
    $script:changed++
    Say 'play.cmd' 'written'
}

# ---------------------------------------------------------------- done

$script:times['total'] = [math]::Round($total.Elapsed.TotalSeconds, 1)
$summary = [ordered]@{
    finished = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'); changed = $script:changed
    downloaded_bytes = [long]$script:downloaded; seconds = $script:times
}
$summary | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $state 'last_run.json') -Encoding utf8
Emit @{ ev = 'done'; mode = 'setup'; changed = $script:changed; secs = $script:times['total']; exe = (Join-Path $build 'gamebreaker.exe') }
Write-Host ''
if ($script:changed -eq 0) { Say 'Result' 'everything was already in place; nothing changed' }
else { Say 'Result' ("ready in {0:N0} s" -f $total.Elapsed.TotalSeconds) }
Say 'Play' (Join-Path $Folder 'play.cmd')
$guard.Close()
