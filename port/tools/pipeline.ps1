# The build pipeline, shared by port\regen.ps1 (the developer's) and
# setup\setup.ps1 (the player's), so there is one copy of it:
#
#   check default.xbe -> parse -> disassemble -> identify -> ABI -> lift -> compile
#
# Dot-source it:   . (Join-Path $PSScriptRoot 'tools\pipeline.ps1')
#
# Nothing here decides where things live. The callers pass every path:
# regen.ps1 keeps the toolkit's in-tree output folders and port\src\recomp\gen
# (the developer tools expect them there); setup.ps1 puts every output in its
# own work folder and leaves the sources untouched.

# The one executable the port is built against. Every address in main.c,
# recomp_manual.c and config\seed_functions.json is only right for it.
$GbXbeSha256   = '08389AE31309FACD89EC7A1057CEC32C4308D734D0B8AD9151437AA511CBDCFE'
$GbXbeTitleId  = 0x45410027
$GbXbeRegion   = 'USA'

# Title ID and region from an XBE's certificate (layout: xboxrecomp
# tools/xbe_parser). Returns $null if the file is not an XBE.
function Get-GbXbeInfo([string]$Path) {
    $b = [IO.File]::ReadAllBytes($Path)
    if ($b.Length -lt 0x200 -or [Text.Encoding]::ASCII.GetString($b, 0, 4) -ne 'XBEH') { return $null }
    $base = [BitConverter]::ToUInt32($b, 0x104)
    $cert = [BitConverter]::ToUInt32($b, 0x118) - $base
    if ($cert -lt 0 -or $cert + 0xB0 -gt $b.Length) { return $null }
    $flags = [BitConverter]::ToUInt32($b, $cert + 0xA0)
    $regions = @()
    if ($flags -band 1) { $regions += 'USA' }
    if ($flags -band 2) { $regions += 'Japan' }
    if ($flags -band 4) { $regions += 'Europe and rest of world' }
    [pscustomobject]@{
        TitleId = [BitConverter]::ToUInt32($b, $cert + 8)
        Region  = ($regions -join ', ')
        Sha256  = (Get-FileHash $Path -Algorithm SHA256).Hash
    }
}

# An error carrying a name (Data['code']) that setup\setup.ps1 and the
# launcher map to their own wording.
function New-GbError([string]$Code, [string]$Message) { $e = New-Object Exception $Message; $e.Data['code'] = $Code; $e }

# Stop unless this is the supported default.xbe. Three cases, each with its
# own message: a different game (title ID), a different release of this game
# (region), or this release modified or damaged. Each says which case, shows
# both hashes, and never suggests where another copy might come from.
function Assert-GbXbe([string]$Path) {
    $info = Get-GbXbeInfo $Path
    if (-not $info) { throw (New-GbError 'not_xbe' "$Path is not an Xbox executable (no XBEH header). Nothing was changed.") }
    if ($info.Sha256 -eq $GbXbeSha256) { return $info }
    $found = "Expected SHA-256 $GbXbeSha256`nFound    SHA-256 $($info.Sha256)"
    if ($info.TitleId -ne $GbXbeTitleId) {
        throw (New-GbError 'xbe_game' ("This disc is a different game (title ID {0:X8}). Gamebreaker only works with the game it was made for. Nothing was changed.`n{1}" -f $info.TitleId, $found))
    }
    if ($info.Region -ne $GbXbeRegion) {
        throw (New-GbError 'xbe_release' "This is a different release of the game (region: $($info.Region)) from the one Gamebreaker supports ($GbXbeRegion). The program code differs, so Gamebreaker cannot build from it. Nothing was changed.`n$found")
    }
    throw (New-GbError 'xbe_modified' "This disc's default.xbe does not match the supported release. It may have been modified (patched or trained) or damaged when it was copied. Gamebreaker needs an unmodified copy made from your own disc. Nothing was changed.`n$found")
}

# Run a native program; return its output lines (stdout and stderr together).
# Windows PowerShell 5.1 turns a program's stderr into error records and, under
# ErrorActionPreference 'Stop', ends the script on the first one; the
# toolkit's tools print progress on stderr. So judge by the exit code only.
function Invoke-GbNative([string]$Exe, [string[]]$ArgList, [string]$WorkDir, [string]$Log) {
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    if ($WorkDir) { Push-Location $WorkDir }
    try {
        # An empty stderr line arrives as an error record whose text is its
        # type name; print it as the empty line it was.
        $lines = @(& $Exe @ArgList 2>&1 | ForEach-Object { $s = "$_"; if ($s -eq 'System.Management.Automation.RemoteException') { '' } else { $s } })
        $code = $LASTEXITCODE
    } finally {
        if ($WorkDir) { Pop-Location }
        $ErrorActionPreference = $oldEap
    }
    if ($Log) { Add-Content -Path $Log -Value (@("> $Exe $($ArgList -join ' ')") + $lines + @("exit $code", '')) -Encoding utf8 }
    [pscustomobject]@{ Code = $code; Lines = $lines }
}

# One lifter step: python -m tools.<module> <args>, run in the toolkit folder.
#   $Python  the interpreter and its flags, e.g. @('py', '-3') or @('<dir>\python.exe', '-I', '-B')
function Invoke-GbPyStep([string]$Name, [string[]]$Python, [string]$Toolkit, [string[]]$ToolArgs, [string]$Log, [switch]$Quiet) {
    if (-not $Quiet) { Write-Host "== $Name" }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $exe = $Python[0]
    $pre = @($Python | Select-Object -Skip 1)
    $r = Invoke-GbNative $exe ($pre + @('-m') + $ToolArgs) $Toolkit $Log
    if (-not $Quiet) { $r.Lines | Select-Object -Last 4 | ForEach-Object { Write-Host "   $_" } }
    if ($r.Code -ne 0) {
        $tail = ($r.Lines | Select-Object -Last 3) -join "`n   "
        throw "$Name failed (exit $($r.Code)).`n   $tail"
    }
    $sec = $sw.Elapsed.TotalSeconds
    if (-not $Quiet) { Write-Host ("   done in {0}s" -f [int]$sec) }
    return $sec
}

# Parse, disassemble, identify, ABI, lift. Returns the seconds each step took.
#   -AnalysisDir  where the intermediate results go. Leave it out to use the
#                 toolkit's in-tree tools\*\output folders (the developer
#                 layout, which tools.seed_from_log and others expect).
function Invoke-GbLift {
    param(
        [Parameter(Mandatory)] [string[]]$Python,
        [Parameter(Mandatory)] [string]$Toolkit,
        [Parameter(Mandatory)] [string]$Xbe,
        [Parameter(Mandatory)] [string]$PortDir,
        [Parameter(Mandatory)] [string]$GenDir,
        [string]$AnalysisDir = '',
        [string]$Log = '',
        [switch]$Quiet,
        [scriptblock]$OnStep = $null       # called with (name, seconds) after each step
    )
    $seeds  = Join-Path $PortDir 'config\seed_functions.json'
    # --exclude-manual: the lifter reads src\recomp_manual.c and wires in what it
    # defines by hand (entry hooks such as sub_00205940_enter, wrapped functions).
    $manual = Join-Path $PortDir 'src\recomp_manual.c'
    $times = [ordered]@{}
    if ($AnalysisDir) {
        $d = @{}
        foreach ($k in 'parse', 'disasm', 'func_id', 'abi', 'recomp') {
            $d[$k] = Join-Path $AnalysisDir $k
            New-Item -ItemType Directory -Force $d[$k] | Out-Null
        }
        $analysis = Join-Path $d.parse 'default_analysis.json'
        $steps = @(
            @('parse',   @('tools.xbe_parser', $Xbe, '--json', $analysis)),
            # No --text-only: this title has code in D3D, D3DX, XGRPH, DSOUND and XPP.
            @('disasm',  @('tools.disasm', $Xbe, '--force', '--seed-functions', $seeds, '-o', $d.disasm, '--analysis-json', $analysis)),
            @('func_id', @('tools.func_id', $Xbe, '--functions', (Join-Path $d.disasm 'functions.json'),
                           '--strings', (Join-Path $d.disasm 'strings.json'), '--xrefs', (Join-Path $d.disasm 'xrefs.json'), '-o', $d.func_id)),
            @('abi',     @('tools.abi_analysis', $Xbe, '--disasm-dir', $d.disasm, '--func-id-dir', $d.func_id, '--output-dir', $d.abi)),
            @('lift',    @('tools.recomp', $Xbe, '--all', '--split', '250', '--game-name', 'Gamebreaker', '--gen-dir', $GenDir,
                           '--exclude-manual', $manual, '--disasm-dir', $d.disasm, '--func-id-dir', $d.func_id, '--abi-dir', $d.abi,
                           '-o', $d.recomp, '--icall-sites', (Join-Path $d.recomp 'icall_sites.json')))
        )
    } else {
        # The parse's JSON goes to the toolkit's own output folder, so nothing is
        # written beside default.xbe; the disassembler is told where it is.
        $parseOut = Join-Path $Toolkit 'tools\xbe_parser\output'
        New-Item -ItemType Directory -Force $parseOut | Out-Null
        $analysis = Join-Path $parseOut 'default_analysis.json'
        $steps = @(
            @('parse',   @('tools.xbe_parser', $Xbe, '--json', $analysis)),
            @('disasm',  @('tools.disasm', $Xbe, '--force', '--seed-functions', $seeds, '--analysis-json', $analysis)),
            @('func_id', @('tools.func_id', $Xbe)),
            @('abi',     @('tools.abi_analysis', $Xbe)),
            @('lift',    @('tools.recomp', $Xbe, '--all', '--split', '250', '--game-name', 'Gamebreaker', '--gen-dir', $GenDir, '--exclude-manual', $manual))
        )
    }
    foreach ($s in $steps) {
        $sec = Invoke-GbPyStep $s[0] $Python $Toolkit $s[1] $Log -Quiet:$Quiet
        $times[$s[0]] = $sec
        if ($OnStep) { & $OnStep $s[0] $sec }
    }
    return $times
}

# CMake: a fetched one under $ToolsDir first, then PATH, then Visual Studio's.
function Find-GbCmake([string]$ToolsDir, [switch]$FetchedOnly) {
    if ($ToolsDir -and (Test-Path (Join-Path $ToolsDir 'cmake'))) {
        $c = Get-ChildItem (Join-Path $ToolsDir 'cmake') -Filter cmake.exe -Recurse -ErrorAction SilentlyContinue |
             Where-Object { $_.Directory.Name -eq 'bin' } | Select-Object -First 1
        if ($c) { return $c.FullName }
    }
    if ($FetchedOnly) { return $null }
    $c = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if ($c) { return $c }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -property installationPath
        $candidate = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}

# Configure (only when the build folder has no build.ninja yet; Ninja
# re-runs CMake by itself when CMakeLists.txt changes) and build with Clang.
# Returns 'built' or 'up to date'.
#   -GenDir / -Toolkit  left out: CMakeLists.txt's defaults
#                       (src\recomp\gen, ..\xboxrecomp)
function Invoke-GbClangBuild {
    param(
        [Parameter(Mandatory)] [string]$Cmake,
        [Parameter(Mandatory)] [string]$LlvmBin,
        [Parameter(Mandatory)] [string]$Ninja,
        [Parameter(Mandatory)] [string]$PortDir,
        [Parameter(Mandatory)] [string]$BuildDir,
        [string]$GenDir = '',
        [string]$Toolkit = '',
        [string]$Log = '',
        # Extra CMake cache values (for example GB_VERSION from a release's
        # setup\release.txt). An existing build folder is configured again only
        # when one of them differs from its cache.
        [hashtable]$Defines = @{}
    )
    $clang = Join-Path $LlvmBin 'x86_64-w64-mingw32-clang.exe'
    foreach ($f in $Cmake, $clang, $Ninja) { if (-not (Test-Path $f)) { throw "missing $f" } }
    $oldPath = $env:PATH
    # For this process only: the linker and the resource compiler are found next to clang.
    $env:PATH = "$LlvmBin;$(Split-Path $Ninja);$env:PATH"
    try {
        if (-not (Test-Path (Join-Path $BuildDir 'build.ninja'))) {
            $cfg = @('-S', $PortDir, '-B', $BuildDir, '-G', 'Ninja',
                     "-DCMAKE_MAKE_PROGRAM=$($Ninja.Replace('\', '/'))",
                     "-DCMAKE_C_COMPILER=$($clang.Replace('\', '/'))", '-DCMAKE_BUILD_TYPE=Release')
            if ($GenDir)  { $cfg += "-DGB_GEN_DIR=$($GenDir.Replace('\', '/'))" }
            if ($Toolkit) { $cfg += "-DXBOXRECOMP_DIR=$($Toolkit.Replace('\', '/'))" }
            foreach ($k in $Defines.Keys) { $cfg += "-D$k=$($Defines[$k])" }
            $r = Invoke-GbNative $Cmake $cfg '' $Log
            if ($r.Code -ne 0) {
                throw ("CMake could not configure the build (exit $($r.Code)).`n   " + (($r.Lines | Where-Object { $_ -match 'Error|error' } | Select-Object -First 3) -join "`n   "))
            }
        } elseif ($Defines.Count) {
            $cache = Join-Path $BuildDir 'CMakeCache.txt'
            $stale = @($Defines.Keys | Where-Object {
                $m = Select-String -Path $cache -Pattern ('^' + [regex]::Escape($_) + ':[A-Z]+=(.*)$') -ErrorAction SilentlyContinue | Select-Object -First 1
                -not $m -or $m.Matches[0].Groups[1].Value -ne [string]$Defines[$_]
            })
            if ($stale.Count) {
                # -S as well: with -B alone CMake takes the current folder as the source.
                $r = Invoke-GbNative $Cmake (@('-S', $PortDir, '-B', $BuildDir) + @($stale | ForEach-Object { "-D$_=$($Defines[$_])" })) '' $Log
                if ($r.Code -ne 0) { throw "CMake could not update the build settings (exit $($r.Code))." }
            }
        }
        # Warnings go to the log only: the toolkit and the lifted code produce about a thousand harmless ones.
        $r = Invoke-GbNative $Ninja @('-C', $BuildDir) '' $Log
        if ($r.Code -ne 0) {
            $errs = @($r.Lines | Where-Object { $_ -match 'error:|FAILED' } | Select-Object -First 5)
            throw ("the compile failed (exit $($r.Code)).`n   " + ($errs -join "`n   "))
        }
        if ($r.Lines -match 'no work to do') { return 'up to date' }
        return 'built'
    } finally { $env:PATH = $oldPath }
}
