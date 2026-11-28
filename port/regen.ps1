# Regenerate the lifted C code from your own default.xbe, then build.
#
#   powershell -ExecutionPolicy Bypass -File port\regen.ps1
#   powershell -ExecutionPolicy Bypass -File port\regen.ps1 -SkipBuild
#   powershell -ExecutionPolicy Bypass -File port\regen.ps1 -BuildOnly
#   powershell -ExecutionPolicy Bypass -File port\regen.ps1 -BuildOnly -Compiler both
#   powershell -ExecutionPolicy Bypass -File port\regen.ps1 -NoCache    (run every lift step)
#
# A lift step whose inputs did not change since it last wrote its outputs is
# kept (tools\pipeline.ps1, "The step cache"): after an edit inside a
# hand-written function in recomp_manual.c, nothing is lifted again.
#
# Expects, next to this folder:
#   ..\xboxrecomp\   a clone of https://github.com/sp00nznet/xboxrecomp
#   ..\game_files\   your own extracted disc (default.xbe + data)
#   ..\toolchain\    the Clang compiler and Ninja; port\tools\fetch_toolchain.ps1
#                    downloads them (not needed for -Compiler msvc)
#
# The generated code lands in port\src\recomp\gen\ and is never committed.
#
# Two builds exist side by side (port\tools\which_build.ps1 says which one
# the scripts run):
#   clang   port\build-mingw\gamebreaker.exe      llvm-mingw + Ninja. The
#           reference build: the one users get, played and measured. About
#           20 s from clean.
#   msvc    port\build\Release\gamebreaker.exe    Visual Studio. A second
#           opinion. About 2 minutes from clean.

param(
    [string]$Xbe = (Join-Path $PSScriptRoot '..\game_files\default.xbe'),
    [switch]$SkipBuild,
    [switch]$BuildOnly,                 # do not lift again: only build (after a change in port\src)
    [ValidateSet('clang', 'msvc', 'both')]
    [string]$Compiler = 'clang',
    [switch]$EvenIfPlaying,             # build although the game is running (a build takes every core and makes it hitch)
    [switch]$NoCache                    # run every step of the lift. By default a step whose inputs are
                                        # unchanged since it last wrote its outputs, and whose outputs are
                                        # still there byte for byte, is kept (tools\pipeline.ps1, "The step cache")
)

$ErrorActionPreference = 'Stop'
# The lift and the Clang build are shared with the player's setup
# (setup\setup.ps1); this script only chooses the developer's paths.
. (Join-Path $PSScriptRoot 'tools\pipeline.ps1')

$root    = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$toolkit = Join-Path $root 'xboxrecomp'
$genDir  = Join-Path $PSScriptRoot 'src\recomp\gen'

if (-not (Test-Path $Xbe))     { throw "default.xbe not found at $Xbe. Extract your own disc into game_files\." }
if (-not (Test-Path $toolkit)) { throw "xboxrecomp not found at $toolkit. Clone it next to port\." }
$Xbe = (Resolve-Path $Xbe).Path

# The port is built against one exact executable. Addresses in main.c,
# recomp_manual.c and the seed list are only valid for it.
$null = Assert-GbXbe $Xbe
Write-Host "default.xbe hash OK"

# The few changes this project needs inside the toolkit (port\patches\README.md).
# Before the lift: the patch also touches the disassembler's Python files.
& (Join-Path $PSScriptRoot 'patches\apply.ps1')

if (-not $BuildOnly) {
    # The developer layout: the toolkit's in-tree tools\*\output folders, which
    # tools.seed_from_log and the other bring-up tools read.
    $null = Invoke-GbLift -Python @('py', '-3') -Toolkit $toolkit -Xbe $Xbe -PortDir $PSScriptRoot -GenDir $genDir -StepCache:(-not $NoCache)
}

if ($SkipBuild) { return }

if (-not $EvenIfPlaying -and (Get-Process gamebreaker -ErrorAction SilentlyContinue)) {
    throw 'The game is running. A build takes every processor core and makes it hitch: close the game first, or pass -EvenIfPlaying.'
}

$cmake = Find-GbCmake (Join-Path $root 'toolchain')
if (-not $cmake) { throw 'cmake not found in toolchain\, on PATH or inside Visual Studio. port\tools\fetch_toolchain.ps1 fetches one.' }

if ($Compiler -ne 'msvc') {
    Write-Host '== build (clang)'
    $tc    = Join-Path $root 'toolchain'
    $bin   = Join-Path $tc 'llvm-mingw\llvm-mingw-20261104-ucrt-x86_64\bin'
    $ninja = Join-Path $tc 'ninja\ninja.exe'
    if (-not (Test-Path (Join-Path $bin 'x86_64-w64-mingw32-clang.exe')) -or -not (Test-Path $ninja)) {
        throw "The Clang toolchain is not in $tc. Run port\tools\fetch_toolchain.ps1 first (or use -Compiler msvc)."
    }
    $build = Join-Path $PSScriptRoot 'build-mingw'
    $buildLog = Join-Path $build 'build_log.txt'
    New-Item -ItemType Directory -Force $build | Out-Null
    Set-Content $buildLog '' -Encoding utf8
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        $state = Invoke-GbClangBuild -Cmake $cmake -LlvmBin $bin -Ninja $ninja -PortDir $PSScriptRoot -BuildDir $build -Log $buildLog
    } catch { throw "build failed (clang): $($_.Exception.Message)`nfull log: $buildLog" }
    Write-Host ("$state`: $build\gamebreaker.exe in {0}s" -f [int]$sw.Elapsed.TotalSeconds)
}

if ($Compiler -ne 'clang') {
    Write-Host '== build (msvc)'
    # Without these about a dozen idle MSBuild.exe processes stay for 15 minutes
    # after the build.
    $env:MSBUILDDISABLENODEREUSE = '1'
    $build = Join-Path $PSScriptRoot 'build'
    # Visual Studio's own CMake (or the one on PATH), not the fetched one: the
    # Visual Studio generator must know this Visual Studio's version.
    $cmake = Find-GbCmake ''
    if (-not $cmake) { throw 'cmake not found on PATH or inside Visual Studio (needed for -Compiler msvc).' }
    & $cmake -S $PSScriptRoot -B $build | Select-Object -Last 2
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed (msvc)' }
    & $cmake --build $build --config Release --parallel -- /nodeReuse:false | Select-Object -Last 3
    if ($LASTEXITCODE -ne 0) { throw 'build failed (msvc)' }
    Write-Host "built $build\Release\gamebreaker.exe"
}
